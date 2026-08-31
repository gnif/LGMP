/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2026 Geoffrey McRae <geoff@hostfission.com>
 * https://github.com/gnif/LGMP
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#define _POSIX_C_SOURCE 200809L

#include "test_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const uint8_t testSessionUdata[] =
{
  0x4cU, 0x47U, 0x4dU, 0x50U, 0x12U, 0x34U
};

const uint32_t testSessionUdataSize = sizeof(testSessionUdata);

bool testCheck(bool condition, const char * expression,
    const char * file, unsigned int line)
{
  if (condition)
    return true;

  fprintf(stderr, "%s:%u: check failed: %s\n", file, line, expression);
  return false;
}

bool testExpectStatus(const char * operation, LGMP_STATUS actual,
    LGMP_STATUS expected)
{
  if (actual == expected)
    return true;

  fprintf(stderr, "%s: expected %s, got %s\n", operation,
      lgmpStatusString(expected), lgmpStatusString(actual));
  return false;
}

size_t testHostAllocationSize(size_t requestedSize)
{
  return (requestedSize + 3U) & ~(size_t)3U;
}

bool testMonotonicMS(uint64_t * result)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  {
    perror("clock_gettime");
    return false;
  }

  *result = (uint64_t)now.tv_sec * UINT64_C(1000) +
    (uint64_t)now.tv_nsec / UINT64_C(1000000);
  return true;
}

bool testSleepMS(unsigned int milliseconds)
{
  struct timespec remaining =
  {
    .tv_sec  = milliseconds / 1000U,
    .tv_nsec = (long)(milliseconds % 1000U) * 1000000L
  };

  while(nanosleep(&remaining, &remaining) != 0)
  {
    if (errno == EINTR)
      continue;

    perror("nanosleep");
    return false;
  }

  return true;
}

static void * hostPump(void * opaque)
{
  struct TestFixture * fixture = opaque;
  fixture->pumpStatus = LGMP_OK;

  while(!atomic_load_explicit(&fixture->pumpStop, memory_order_relaxed))
  {
    fixture->pumpStatus = lgmpHostProcess(fixture->host);
    if (fixture->pumpStatus != LGMP_OK)
      break;

    if (!testSleepMS(1U))
    {
      fixture->pumpStatus = LGMP_ERR_CLOCK_FAILURE;
      break;
    }
  }

  return NULL;
}

static bool initHost(struct TestFixture * fixture)
{
  return testExpectStatus("lgmpHostInit",
      lgmpHostInit(fixture->memory, TEST_MEMORY_SIZE, &fixture->host,
        testSessionUdataSize, (uint8_t *)testSessionUdata), LGMP_OK);
}

bool testFixtureInit(struct TestFixture * fixture)
{
  memset(fixture, 0, sizeof(*fixture));
  atomic_init(&fixture->pumpStop, false);

  const size_t allocationSize = TEST_MEMORY_SIZE +
    TEST_MEMORY_ALIGNMENT - 1U;
  fixture->allocation = malloc(allocationSize);
  if (!fixture->allocation)
  {
    perror("malloc");
    return false;
  }

  const uintptr_t address = (uintptr_t)fixture->allocation;
  const uintptr_t aligned = (address + TEST_MEMORY_ALIGNMENT - 1U) &
    ~((uintptr_t)TEST_MEMORY_ALIGNMENT - 1U);
  fixture->memory = (uint8_t *)aligned;
  memset(fixture->memory, 0, TEST_MEMORY_SIZE);

  if (!TEST_CHECK(((uintptr_t)fixture->memory &
        (TEST_MEMORY_ALIGNMENT - 1U)) == 0U) ||
      !initHost(fixture))
  {
    free(fixture->allocation);
    fixture->allocation = NULL;
    fixture->memory     = NULL;
    return false;
  }

  return true;
}

bool testFixtureStart(struct TestFixture * fixture)
{
  if (fixture->pumpStarted)
    return true;

  atomic_store_explicit(&fixture->pumpStop, false, memory_order_relaxed);
  fixture->pumpStatus = LGMP_OK;

  const int error = pthread_create(&fixture->pumpThread, NULL, hostPump,
      fixture);
  if (error != 0)
  {
    fprintf(stderr, "pthread_create: %s\n", strerror(error));
    return false;
  }

  fixture->pumpStarted = true;
  return true;
}

bool testFixtureStop(struct TestFixture * fixture)
{
  if (!fixture->pumpStarted)
    return true;

  atomic_store_explicit(&fixture->pumpStop, true, memory_order_relaxed);
  const int error = pthread_join(fixture->pumpThread, NULL);
  if (error != 0)
  {
    fprintf(stderr, "pthread_join: %s\n", strerror(error));
    return false;
  }

  fixture->pumpStarted = false;
  return testExpectStatus("lgmpHostProcess", fixture->pumpStatus, LGMP_OK);
}

bool testFixtureRestart(struct TestFixture * fixture)
{
  if (!testFixtureStop(fixture))
    return false;

  lgmpHostFree(&fixture->host);
  if (!initHost(fixture))
    return false;

  return testFixtureStart(fixture);
}

bool testFixtureDestroy(struct TestFixture * fixture)
{
  const bool stopped = testFixtureStop(fixture);
  if (fixture->pumpStarted)
    return false;

  if (fixture->host)
    lgmpHostFree(&fixture->host);

  free(fixture->allocation);
  fixture->allocation = NULL;
  fixture->memory     = NULL;
  return stopped;
}

bool testClientInit(struct TestFixture * fixture, PLGMPClient * client,
    uint32_t * clientID)
{
  *client   = NULL;
  *clientID = 0U;

  if (!testExpectStatus("lgmpClientInit",
        lgmpClientInit(fixture->memory, TEST_MEMORY_SIZE, client), LGMP_OK))
    return false;

  uint32_t  udataSize     = 0U;
  uint8_t * udata         = NULL;
  uint32_t  remoteVersion = UINT32_MAX;
  if (!testExpectStatus("lgmpClientSessionInit",
        lgmpClientSessionInit(*client, &udataSize, &udata, clientID,
          &remoteVersion), LGMP_OK) ||
      !TEST_CHECK(*clientID != 0U) ||
      !TEST_CHECK(remoteVersion == 0U) ||
      !TEST_CHECK(udataSize == testSessionUdataSize) ||
      !TEST_CHECK(udata != NULL) ||
      !TEST_CHECK(memcmp(udata, testSessionUdata,
          testSessionUdataSize) == 0) ||
      !TEST_CHECK(lgmpClientSessionValid(*client)))
  {
    lgmpClientFree(client);
    *clientID = 0U;
    return false;
  }

  return true;
}
