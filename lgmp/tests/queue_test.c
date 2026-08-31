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

#define TEST_QUEUE_ID 0x200U

static bool cleanupClients(PLGMPClientQueue * queues, PLGMPClient * clients,
    unsigned int count)
{
  bool success = true;
  for(unsigned int i = 0; i < count; ++i)
  {
    if (queues[i] &&
        !testExpectStatus("cleanup lgmpClientUnsubscribe",
          lgmpClientUnsubscribe(&queues[i]), LGMP_OK))
      success = false;
    if (clients[i])
      lgmpClientFree(&clients[i]);
  }

  return success;
}

static bool allocatePayload(struct TestFixture * fixture,
    const void * data, uint32_t size, PLGMPMemory * payload)
{
  if (!testExpectStatus("lgmpHostMemAlloc",
        lgmpHostMemAlloc(fixture->host, size, payload), LGMP_OK))
    return false;

  memcpy(lgmpHostMemPtr(*payload), data, size);
  return true;
}

static bool containsClientID(const uint32_t * clientIDs,
    unsigned int count, uint32_t expected)
{
  for(unsigned int i = 0; i < count; ++i)
    if (clientIDs[i] == expected)
      return true;

  return false;
}

static bool runBroadcast(void)
{
  bool               success         = false;
  struct TestFixture fixture         = { 0 };
  PLGMPHostQueue     hostQueue       = NULL;
  PLGMPClient        clients[2]      = { NULL, NULL };
  PLGMPClientQueue   clientQueues[2] = { NULL, NULL };
  uint32_t           clientIDs[2]    = { 0U, 0U };
  PLGMPMemory        payload         = NULL;

  const struct LGMPQueueConfig config =
  {
    .queueID     = TEST_QUEUE_ID,
    .numMessages = 4U,
    .subTimeout  = 5000U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostQueueNew",
        lgmpHostQueueNew(fixture.host, config, &hostQueue), LGMP_OK) ||
      !testFixtureStart(&fixture))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
    if (!testClientInit(&fixture, &clients[i], &clientIDs[i]) ||
        !testExpectStatus("lgmpClientSubscribe",
          lgmpClientSubscribe(clients[i], TEST_QUEUE_ID, &clientQueues[i]),
          LGMP_OK))
      goto cleanup;

  uint32_t     subscribedIDs[32] = { 0U };
  unsigned int subscribedCount   = 0U;
  if (!TEST_CHECK(lgmpHostQueueHasSubs(hostQueue)) ||
      !TEST_CHECK(lgmpHostQueueNewSubs(hostQueue) == 2U) ||
      !TEST_CHECK(lgmpHostQueueNewSubs(hostQueue) == 0U) ||
      !testExpectStatus("lgmpHostGetClientIDs",
        lgmpHostGetClientIDs(hostQueue, subscribedIDs, &subscribedCount),
        LGMP_OK) ||
      !TEST_CHECK(subscribedCount == 2U) ||
      !TEST_CHECK(containsClientID(subscribedIDs, subscribedCount,
          clientIDs[0])) ||
      !TEST_CHECK(containsClientID(subscribedIDs, subscribedCount,
          clientIDs[1])))
    goto cleanup;

  static const char value[] = "broadcast payload";
  const uint32_t messageData = 0x12345678U;
  if (!allocatePayload(&fixture, value, sizeof(value), &payload) ||
      !testExpectStatus("lgmpHostQueuePost",
        lgmpHostQueuePost(hostQueue, messageData, payload), LGMP_OK) ||
      !TEST_CHECK(lgmpHostQueuePending(hostQueue) == 1U) ||
      !TEST_CHECK(lgmpHostQueuePayloadPending(hostQueue, payload)) ||
      !TEST_CHECK(lgmpHostQueueMessagePending(hostQueue, payload,
          messageData)))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    LGMPMessage message = { 0 };
    if (!testExpectStatus("lgmpClientProcess",
          lgmpClientProcess(clientQueues[i], &message), LGMP_OK) ||
        !TEST_CHECK(message.udata == messageData) ||
        !TEST_CHECK(message.size == testHostAllocationSize(sizeof(value))) ||
        !TEST_CHECK(memcmp(message.mem, value, sizeof(value)) == 0) ||
        !testExpectStatus("lgmpClientMessageDone",
          lgmpClientMessageDone(clientQueues[i]), LGMP_OK))
      goto cleanup;

    if (i == 0U &&
        (!TEST_CHECK(lgmpHostQueuePending(hostQueue) == 1U) ||
         !TEST_CHECK(lgmpHostQueuePayloadPending(hostQueue, payload))))
      goto cleanup;
  }

  if (!TEST_CHECK(lgmpHostQueuePending(hostQueue) == 0U) ||
      !TEST_CHECK(!lgmpHostQueuePayloadPending(hostQueue, payload)) ||
      !TEST_CHECK(!lgmpHostQueueMessagePending(hostQueue, payload,
          messageData)))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    LGMPMessage message = { 0 };
    if (!testExpectStatus("empty lgmpClientProcess",
          lgmpClientProcess(clientQueues[i], &message),
          LGMP_ERR_QUEUE_EMPTY) ||
        !testExpectStatus("lgmpClientUnsubscribe",
          lgmpClientUnsubscribe(&clientQueues[i]), LGMP_OK))
      goto cleanup;
  }

  if (!TEST_CHECK(!lgmpHostQueueHasSubs(hostQueue)))
    goto cleanup;

  success = true;

cleanup:
  if (!cleanupClients(clientQueues, clients, 2U))
    success = false;
  if (payload)
    lgmpHostMemFree(&payload);
  if (!testFixtureDestroy(&fixture))
    success = false;
  return success;
}

static bool runTargeted(void)
{
  bool               success         = false;
  struct TestFixture fixture         = { 0 };
  PLGMPHostQueue     hostQueue       = NULL;
  PLGMPClient        clients[2]      = { NULL, NULL };
  PLGMPClientQueue   clientQueues[2] = { NULL, NULL };
  uint32_t           clientIDs[2]    = { 0U, 0U };
  PLGMPMemory        payload         = NULL;

  const struct LGMPQueueConfig config =
  {
    .queueID     = TEST_QUEUE_ID,
    .numMessages = 2U,
    .subTimeout  = 5000U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostQueueNew",
        lgmpHostQueueNew(fixture.host, config, &hostQueue), LGMP_OK) ||
      !testFixtureStart(&fixture))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
    if (!testClientInit(&fixture, &clients[i], &clientIDs[i]) ||
        !testExpectStatus("lgmpClientSubscribe",
          lgmpClientSubscribe(clients[i], TEST_QUEUE_ID, &clientQueues[i]),
          LGMP_OK))
      goto cleanup;

  static const char value[] = "targeted payload";
  const uint64_t messageData = UINT64_C(0x123456789abcdef0);
  if (!allocatePayload(&fixture, value, sizeof(value), &payload))
    goto cleanup;

  unsigned int recipients = 99U;
  if (!testExpectStatus("null targeted client IDs",
        lgmpHostQueuePostForClients(hostQueue, messageData, payload, NULL,
          1U, &recipients), LGMP_ERR_INVALID_ARGUMENT) ||
      !TEST_CHECK(recipients == 0U) ||
      !TEST_CHECK(!lgmpHostQueuePayloadPending(hostQueue, payload)))
    goto cleanup;

  if (!testExpectStatus("targeted post",
        lgmpHostQueuePostForClients(hostQueue, messageData, payload,
          &clientIDs[0], 1U, &recipients), LGMP_OK) ||
      !TEST_CHECK(recipients == 1U))
    goto cleanup;

  LGMPMessage message = { 0 };
  if (!testExpectStatus("non-target client process",
        lgmpClientProcess(clientQueues[1], &message),
        LGMP_ERR_QUEUE_EMPTY) ||
      !testExpectStatus("target client process",
        lgmpClientProcess(clientQueues[0], &message), LGMP_OK) ||
      !TEST_CHECK(message.udata == messageData) ||
      !TEST_CHECK(message.size == testHostAllocationSize(sizeof(value))) ||
      !TEST_CHECK(memcmp(message.mem, value, sizeof(value)) == 0) ||
      !testExpectStatus("target client done",
        lgmpClientMessageDone(clientQueues[0]), LGMP_OK))
    goto cleanup;

  const uint32_t missingClientID = UINT32_MAX;
  recipients = 99U;
  if (!testExpectStatus("unmatched targeted post",
        lgmpHostQueuePostForClients(hostQueue, messageData, payload,
          &missingClientID, 1U, &recipients), LGMP_OK) ||
      !TEST_CHECK(recipients == 0U) ||
      !TEST_CHECK(lgmpHostQueuePending(hostQueue) == 0U) ||
      !TEST_CHECK(!lgmpHostQueuePayloadPending(hostQueue, payload)))
    goto cleanup;

  success = true;

cleanup:
  if (!cleanupClients(clientQueues, clients, 2U))
    success = false;
  if (payload)
    lgmpHostMemFree(&payload);
  if (!testFixtureDestroy(&fixture))
    success = false;
  return success;
}

static bool runCapacity(void)
{
  bool               success                     = false;
  struct TestFixture fixture                     = { 0 };
  PLGMPHostQueue     hostQueues[LGMP_MAX_QUEUES] = { NULL };
  PLGMPClient        client                      = NULL;
  PLGMPClientQueue   clientQueue                 = NULL;
  PLGMPMemory        payloads[3]                 = { NULL, NULL, NULL };
  uint32_t           clientID                    = 0U;

  if (!testFixtureInit(&fixture) || !TEST_CHECK(LGMP_MAX_QUEUES == 6))
    goto cleanup;

  const struct LGMPQueueConfig invalidConfig =
  {
    .queueID     = TEST_QUEUE_ID,
    .numMessages = 1U,
    .subTimeout  = 5000U
  };
  PLGMPHostQueue invalidQueue = NULL;
  if (!testExpectStatus("one-entry queue",
        lgmpHostQueueNew(fixture.host, invalidConfig, &invalidQueue),
        LGMP_ERR_INVALID_ARGUMENT) ||
      !TEST_CHECK(invalidQueue == NULL))
    goto cleanup;

  for(unsigned int i = 0; i < LGMP_MAX_QUEUES; ++i)
  {
    const struct LGMPQueueConfig config =
    {
      .queueID     = TEST_QUEUE_ID + i,
      .numMessages = 2U,
      .subTimeout  = 5000U
    };
    if (!testExpectStatus("lgmpHostQueueNew",
          lgmpHostQueueNew(fixture.host, config, &hostQueues[i]), LGMP_OK))
      goto cleanup;
  }

  const struct LGMPQueueConfig extraConfig =
  {
    .queueID     = TEST_QUEUE_ID + LGMP_MAX_QUEUES,
    .numMessages = 2U,
    .subTimeout  = 5000U
  };
  PLGMPHostQueue extraQueue = NULL;
  if (!testExpectStatus("seventh lgmpHostQueueNew",
        lgmpHostQueueNew(fixture.host, extraConfig, &extraQueue),
        LGMP_ERR_NO_QUEUES) ||
      !TEST_CHECK(extraQueue == NULL) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID) ||
      !testExpectStatus("sixth queue subscription",
        lgmpClientSubscribe(client,
          TEST_QUEUE_ID + LGMP_MAX_QUEUES - 1U, &clientQueue), LGMP_OK))
    goto cleanup;

  static const char first [] = "first";
  static const char second[] = "second";
  static const char third [] = "third";
  if (!allocatePayload(&fixture, first, sizeof(first), &payloads[0]) ||
      !allocatePayload(&fixture, second, sizeof(second), &payloads[1]) ||
      !allocatePayload(&fixture, third, sizeof(third), &payloads[2]) ||
      !testExpectStatus("first queue post",
        lgmpHostQueuePost(hostQueues[LGMP_MAX_QUEUES - 1U], 1U,
          payloads[0]), LGMP_OK) ||
      !testExpectStatus("second queue post",
        lgmpHostQueuePost(hostQueues[LGMP_MAX_QUEUES - 1U], 2U,
          payloads[1]), LGMP_OK) ||
      !testExpectStatus("full queue post",
        lgmpHostQueuePost(hostQueues[LGMP_MAX_QUEUES - 1U], 3U,
          payloads[2]), LGMP_ERR_QUEUE_FULL) ||
      !TEST_CHECK(lgmpHostQueuePending(
          hostQueues[LGMP_MAX_QUEUES - 1U]) == 2U) ||
      !testExpectStatus("lgmpClientAdvanceToLast",
        lgmpClientAdvanceToLast(clientQueue), LGMP_OK) ||
      !TEST_CHECK(lgmpHostQueuePending(
          hostQueues[LGMP_MAX_QUEUES - 1U]) == 1U))
    goto cleanup;

  LGMPMessage message = { 0 };
  if (!testExpectStatus("last queued message",
        lgmpClientProcess(clientQueue, &message), LGMP_OK) ||
      !TEST_CHECK(message.udata == 2U) ||
      !TEST_CHECK(message.size == testHostAllocationSize(sizeof(second))) ||
      !TEST_CHECK(memcmp(message.mem, second, sizeof(second)) == 0) ||
      !testExpectStatus("last queued message done",
        lgmpClientMessageDone(clientQueue), LGMP_OK) ||
      !testExpectStatus("post after queue drain",
        lgmpHostQueuePost(hostQueues[LGMP_MAX_QUEUES - 1U], 3U,
          payloads[2]), LGMP_OK) ||
      !testExpectStatus("post-drain message",
        lgmpClientProcess(clientQueue, &message), LGMP_OK) ||
      !TEST_CHECK(message.udata == 3U) ||
      !TEST_CHECK(message.size == testHostAllocationSize(sizeof(third))) ||
      !TEST_CHECK(memcmp(message.mem, third, sizeof(third)) == 0) ||
      !testExpectStatus("post-drain message done",
        lgmpClientMessageDone(clientQueue), LGMP_OK))
    goto cleanup;

  success = true;

cleanup:
  if (clientQueue &&
      !testExpectStatus("cleanup lgmpClientUnsubscribe",
        lgmpClientUnsubscribe(&clientQueue), LGMP_OK))
    success = false;
  if (client)
    lgmpClientFree(&client);
  for(unsigned int i = 0; i < 3U; ++i)
    if (payloads[i])
      lgmpHostMemFree(&payloads[i]);
  if (!testFixtureDestroy(&fixture))
    success = false;
  return success;
}

static bool runClientData(void)
{
  bool               success     = false;
  struct TestFixture fixture     = { 0 };
  PLGMPHostQueue     hostQueue   = NULL;
  PLGMPClient        client      = NULL;
  PLGMPClientQueue   clientQueue = NULL;
  uint32_t           clientID    = 0U;

  const struct LGMPQueueConfig config =
  {
    .queueID     = TEST_QUEUE_ID,
    .numMessages = 2U,
    .subTimeout  = 5000U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostQueueNew",
        lgmpHostQueueNew(fixture.host, config, &hostQueue), LGMP_OK) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID) ||
      !testExpectStatus("lgmpClientSubscribe",
        lgmpClientSubscribe(client, TEST_QUEUE_ID, &clientQueue), LGMP_OK))
    goto cleanup;

  uint32_t acknowledgedSerial = UINT32_MAX;
  if (!testExpectStatus("initial lgmpClientGetSerial",
        lgmpClientGetSerial(clientQueue, &acknowledgedSerial), LGMP_OK) ||
      !TEST_CHECK(acknowledgedSerial == 0U))
    goto cleanup;

  uint8_t oversized[LGMP_MSGS_SIZE + 1U] = { 0U };
  if (!testExpectStatus("oversized lgmpClientSendData",
        lgmpClientSendData(clientQueue, oversized, sizeof(oversized), NULL),
        LGMP_ERR_INVALID_SIZE))
    goto cleanup;

  static const char first [] = "client message one";
  static const char second[] = "client message two";
  uint32_t firstSerial  = 0U;
  uint32_t secondSerial = 0U;
  if (!testExpectStatus("lgmpClientSendData",
        lgmpClientSendData(clientQueue, first, sizeof(first), &firstSerial),
        LGMP_OK) ||
      !TEST_CHECK(firstSerial == 1U) ||
      !testExpectStatus("lgmpClientTrySendData",
        lgmpClientTrySendData(clientQueue, second, sizeof(second),
          &secondSerial), LGMP_OK) ||
      !TEST_CHECK(secondSerial == 2U))
    goto cleanup;

  char     received[LGMP_MSGS_SIZE] = { 0 };
  size_t   receivedSize             = 0U;
  uint32_t sourceClientID           = 0U;
  if (!testExpectStatus("lgmpHostReadDataWithSource",
        lgmpHostReadDataWithSource(hostQueue, received, &receivedSize,
          &sourceClientID), LGMP_OK) ||
      !TEST_CHECK(sourceClientID == clientID) ||
      !TEST_CHECK(receivedSize == sizeof(first)) ||
      !TEST_CHECK(memcmp(received, first, sizeof(first)) == 0) ||
      !testExpectStatus("first lgmpHostAckData",
        lgmpHostAckData(hostQueue), LGMP_OK) ||
      !testExpectStatus("first acknowledged serial",
        lgmpClientGetSerial(clientQueue, &acknowledgedSerial), LGMP_OK) ||
      !TEST_CHECK(acknowledgedSerial == firstSerial))
    goto cleanup;

  memset(received, 0, sizeof(received));
  receivedSize = 0U;
  if (!testExpectStatus("lgmpHostReadData",
        lgmpHostReadData(hostQueue, received, &receivedSize), LGMP_OK) ||
      !TEST_CHECK(receivedSize == sizeof(second)) ||
      !TEST_CHECK(memcmp(received, second, sizeof(second)) == 0) ||
      !testExpectStatus("second lgmpHostAckData",
        lgmpHostAckData(hostQueue), LGMP_OK) ||
      !testExpectStatus("second acknowledged serial",
        lgmpClientGetSerial(clientQueue, &acknowledgedSerial), LGMP_OK) ||
      !TEST_CHECK(acknowledgedSerial == secondSerial) ||
      !testExpectStatus("empty lgmpHostReadData",
        lgmpHostReadData(hostQueue, received, &receivedSize),
        LGMP_ERR_QUEUE_EMPTY))
    goto cleanup;

  success = true;

cleanup:
  if (clientQueue &&
      !testExpectStatus("cleanup lgmpClientUnsubscribe",
        lgmpClientUnsubscribe(&clientQueue), LGMP_OK))
    success = false;
  if (client)
    lgmpClientFree(&client);
  if (!testFixtureDestroy(&fixture))
    success = false;
  return success;
}

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s CASE\n", argv[0]);
    return EXIT_FAILURE;
  }

  bool success = false;
  if (strcmp(argv[1], "broadcast") == 0)
    success = runBroadcast();
  else if (strcmp(argv[1], "targeted") == 0)
    success = runTargeted();
  else if (strcmp(argv[1], "capacity") == 0)
    success = runCapacity();
  else if (strcmp(argv[1], "client-data") == 0)
    success = runClientData();
  else
    fprintf(stderr, "unknown queue test case: %s\n", argv[1]);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
