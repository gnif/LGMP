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

#include "test_support.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CONCURRENT_MESSAGES 2000U
#define TEST_CONCURRENT_TIMEOUT  5000U
#define TEST_QUEUE_ID            0x300U

struct Producer
{
  PLGMPHostQueue         queue;
  PLGMPMemory            payload;
  struct TestAtomicBool  cancel;
  LGMP_STATUS            status;
  uint32_t               produced;
  uint64_t               deadline;
};

static void produceMessages(void * opaque)
{
  struct Producer * producer = opaque;
  producer->status = LGMP_OK;

  for(uint32_t sequence = 1U;
      sequence <= TEST_CONCURRENT_MESSAGES;)
  {
    if (testAtomicBoolLoad(&producer->cancel))
      return;

    const LGMP_STATUS status = lgmpHostQueuePost(producer->queue, sequence,
        producer->payload);
    if (status == LGMP_OK)
    {
      producer->produced = sequence;
      ++sequence;
      continue;
    }

    if (status != LGMP_ERR_QUEUE_FULL)
    {
      producer->status = status;
      return;
    }

    uint64_t now;
    if (!testMonotonicMS(&now))
    {
      producer->status = LGMP_ERR_CLOCK_FAILURE;
      return;
    }
    if (now >= producer->deadline)
    {
      producer->status = LGMP_ERR_QUEUE_TIMEOUT;
      return;
    }
    if (!testSleepMS(1U))
    {
      producer->status = LGMP_ERR_CLOCK_FAILURE;
      return;
    }
  }
}

int main(void)
{
  int                result          = EXIT_FAILURE;
  struct TestFixture fixture         = { 0 };
  PLGMPHostQueue     hostQueue       = NULL;
  PLGMPClient        client          = NULL;
  PLGMPClientQueue   clientQueue     = NULL;
  PLGMPMemory        payload         = NULL;
  uint32_t           clientID        = 0U;
  struct TestThread  producerThread   = { 0 };
  struct Producer    producer         = { 0 };

  const struct LGMPQueueConfig config =
  {
    .queueID     = TEST_QUEUE_ID,
    .numMessages = 64U,
    .subTimeout  = 60000U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostQueueNew",
        lgmpHostQueueNew(fixture.host, config, &hostQueue), LGMP_OK) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID) ||
      !testExpectStatus("lgmpClientSubscribe",
        lgmpClientSubscribe(client, TEST_QUEUE_ID, &clientQueue), LGMP_OK))
    goto cleanup;

  static const char value[] = "concurrent queue payload";
  if (!testExpectStatus("lgmpHostMemAlloc",
        lgmpHostMemAlloc(fixture.host, sizeof(value), &payload), LGMP_OK))
    goto cleanup;
  memcpy(lgmpHostMemPtr(payload), value, sizeof(value));

  uint64_t start;
  if (!testMonotonicMS(&start) ||
      !TEST_CHECK(start <= UINT64_MAX - TEST_CONCURRENT_TIMEOUT))
    goto cleanup;

  producer.queue    = hostQueue;
  producer.payload  = payload;
  producer.status   = LGMP_OK;
  producer.produced = 0U;
  producer.deadline = start + TEST_CONCURRENT_TIMEOUT;
  testAtomicBoolInit(&producer.cancel, false);

  if (!testThreadStart(&producerThread, produceMessages, &producer))
    goto cleanup;

  uint32_t consumed = 0U;
  while(consumed < TEST_CONCURRENT_MESSAGES)
  {
    LGMPMessage message = { 0 };
    const LGMP_STATUS status = lgmpClientProcess(clientQueue, &message);
    if (status == LGMP_OK)
    {
      const uint32_t expected = consumed + 1U;
      if (!TEST_CHECK(message.udata == expected) ||
          !TEST_CHECK(message.size ==
              testHostAllocationSize(sizeof(value))) ||
          !TEST_CHECK(memcmp(message.mem, value, sizeof(value)) == 0) ||
          !testExpectStatus("lgmpClientMessageDone",
            lgmpClientMessageDone(clientQueue), LGMP_OK))
        goto cleanup;

      ++consumed;
      continue;
    }

    if (status != LGMP_ERR_QUEUE_EMPTY)
    {
      (void)testExpectStatus("lgmpClientProcess", status,
          LGMP_ERR_QUEUE_EMPTY);
      goto cleanup;
    }

    uint64_t now;
    if (!testMonotonicMS(&now) ||
        !TEST_CHECK(now < producer.deadline) ||
        !testSleepMS(1U))
      goto cleanup;
  }

  if (!testThreadJoin(&producerThread))
    goto cleanup;

  if (!testExpectStatus("concurrent producer", producer.status, LGMP_OK) ||
      !TEST_CHECK(producer.produced == TEST_CONCURRENT_MESSAGES) ||
      !TEST_CHECK(lgmpHostQueuePending(hostQueue) == 0U) ||
      !TEST_CHECK(!lgmpHostQueuePayloadPending(hostQueue, payload)))
    goto cleanup;

  result = EXIT_SUCCESS;

cleanup:
  if (producerThread.started)
  {
    testAtomicBoolStore(&producer.cancel, true);
    if (!testThreadJoin(&producerThread))
      return EXIT_FAILURE;
  }
  if (clientQueue &&
      !testExpectStatus("cleanup lgmpClientUnsubscribe",
        lgmpClientUnsubscribe(&clientQueue), LGMP_OK))
    result = EXIT_FAILURE;
  if (client)
    lgmpClientFree(&client);
  if (payload)
    lgmpHostMemFree(&payload);
  if (!testFixtureDestroy(&fixture))
    result = EXIT_FAILURE;
  return result;
}
