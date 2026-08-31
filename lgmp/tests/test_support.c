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

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <time.h>
#endif

const uint8_t testSessionUdata[] =
{
  0x4cU, 0x47U, 0x4dU, 0x50U, 0x12U, 0x34U
};

const uint32_t testSessionUdataSize = (uint32_t)sizeof(testSessionUdata);

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

void testAtomicBoolInit(struct TestAtomicBool * value, bool initial)
{
#if defined(_WIN32)
  InterlockedExchange(&value->value, initial ? 1L : 0L);
#else
  atomic_init(&value->value, initial);
#endif
}

bool testAtomicBoolLoad(struct TestAtomicBool * value)
{
#if defined(_WIN32)
  return InterlockedCompareExchange(&value->value, 0L, 0L) != 0L;
#else
  return atomic_load_explicit(&value->value, memory_order_relaxed);
#endif
}

void testAtomicBoolStore(struct TestAtomicBool * value, bool next)
{
#if defined(_WIN32)
  InterlockedExchange(&value->value, next ? 1L : 0L);
#else
  atomic_store_explicit(&value->value, next, memory_order_relaxed);
#endif
}

#if defined(_WIN32)
static unsigned __stdcall testThreadEntry(void * opaque)
#else
static void * testThreadEntry(void * opaque)
#endif
{
  struct TestThread * thread = opaque;
  thread->function(thread->opaque);

#if defined(_WIN32)
  return 0U;
#else
  return NULL;
#endif
}

bool testThreadStart(struct TestThread * thread,
    TestThreadFunction function, void * opaque)
{
  if (thread->started)
  {
    fprintf(stderr, "testThreadStart: thread is already running\n");
    return false;
  }

  thread->function = function;
  thread->opaque   = opaque;

#if defined(_WIN32)
  const uintptr_t handle = _beginthreadex(NULL, 0U, testThreadEntry,
      thread, 0U, NULL);
  if (handle == 0U)
  {
    fprintf(stderr, "_beginthreadex failed with error %d\n", errno);
    thread->function = NULL;
    thread->opaque   = NULL;
    return false;
  }
  thread->handle = handle;
#else
  const int error = pthread_create(&thread->handle, NULL, testThreadEntry,
      thread);
  if (error != 0)
  {
    fprintf(stderr, "pthread_create: %s\n", strerror(error));
    thread->function = NULL;
    thread->opaque   = NULL;
    return false;
  }
#endif

  thread->started = true;
  return true;
}

bool testThreadJoin(struct TestThread * thread)
{
  if (!thread->started)
    return true;

#if defined(_WIN32)
  const HANDLE handle     = (HANDLE)thread->handle;
  const DWORD  waitResult = WaitForSingleObject(handle, INFINITE);
  if (waitResult != WAIT_OBJECT_0)
  {
    fprintf(stderr, "WaitForSingleObject failed with result %lu and error %lu\n",
        (unsigned long)waitResult, (unsigned long)GetLastError());
    return false;
  }

  if (!CloseHandle(handle))
  {
    fprintf(stderr, "CloseHandle failed with error %lu\n",
        (unsigned long)GetLastError());
    return false;
  }
  thread->handle = 0U;
#else
  const int error = pthread_join(thread->handle, NULL);
  if (error != 0)
  {
    fprintf(stderr, "pthread_join: %s\n", strerror(error));
    return false;
  }
#endif

  thread->function = NULL;
  thread->opaque   = NULL;
  thread->started  = false;
  return true;
}

bool testMonotonicMS(uint64_t * result)
{
#if defined(_WIN32)
  LARGE_INTEGER frequency = { 0 };
  LARGE_INTEGER counter   = { 0 };
  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter) ||
      frequency.QuadPart <= 0 || counter.QuadPart < 0)
  {
    fprintf(stderr, "QueryPerformanceCounter failed\n");
    return false;
  }

  const uint64_t seconds =
    (uint64_t)(counter.QuadPart / frequency.QuadPart);
  const uint64_t ticks   =
    (uint64_t)(counter.QuadPart % frequency.QuadPart);
  *result = seconds * UINT64_C(1000) +
    ticks * UINT64_C(1000) / (uint64_t)frequency.QuadPart;
#else
  struct timespec now = { 0 };
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
  {
    perror("clock_gettime");
    return false;
  }

  *result = (uint64_t)now.tv_sec * UINT64_C(1000) +
    (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
  return true;
}

bool testSleepMS(unsigned int milliseconds)
{
#if defined(_WIN32)
  Sleep((DWORD)milliseconds);
#else
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
#endif

  return true;
}

static void hostPump(void * opaque)
{
  struct TestFixture * fixture = opaque;
  fixture->pumpStatus = LGMP_OK;

  while(!testAtomicBoolLoad(&fixture->pumpStop))
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
  testAtomicBoolInit(&fixture->pumpStop, false);

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
  if (fixture->pumpThread.started)
    return true;

  testAtomicBoolStore(&fixture->pumpStop, false);
  fixture->pumpStatus = LGMP_OK;

  if (!testThreadStart(&fixture->pumpThread, hostPump, fixture))
    return false;

  return true;
}

bool testFixtureStop(struct TestFixture * fixture)
{
  if (!fixture->pumpThread.started)
    return true;

  testAtomicBoolStore(&fixture->pumpStop, true);
  if (!testThreadJoin(&fixture->pumpThread))
    return false;

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
  if (fixture->pumpThread.started)
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
