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

#include "lgmp/stream.h"

struct NotifyState
{
  unsigned int calls;
  uint32_t     reasons;
  uint32_t     descriptorMagic;
};

static void streamNotify(void * opaque,
    const struct LGMPStreamDescriptor * descriptor, uint32_t reasons)
{
  struct NotifyState * state = opaque;
  ++state->calls;
  state->reasons        |= reasons;
  state->descriptorMagic = descriptor->magic;
}

static LGMP_STATUS producerAcquire(uint32_t direction,
    PLGMPHostStream hostStream, PLGMPClientStream clientStream,
    LGMPStreamBuffer * buffer)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
    return lgmpHostStreamWriteAcquire(hostStream, buffer);

  return lgmpClientStreamWriteAcquire(clientStream, buffer);
}

static LGMP_STATUS producerCommit(uint32_t direction,
    PLGMPHostStream hostStream, PLGMPClientStream clientStream,
    const LGMPStreamBuffer * buffer, uint32_t size)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
    return lgmpHostStreamWriteCommit(hostStream, buffer, size);

  return lgmpClientStreamWriteCommit(clientStream, buffer, size);
}

static LGMP_STATUS producerCancel(uint32_t direction,
    PLGMPHostStream hostStream, PLGMPClientStream clientStream,
    const LGMPStreamBuffer * buffer)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
    return lgmpHostStreamWriteCancel(hostStream, buffer);

  return lgmpClientStreamWriteCancel(clientStream, buffer);
}

static LGMP_STATUS consumerPeek(uint32_t direction,
    PLGMPHostStream hostStream, PLGMPClientStream clientStream,
    LGMPStreamBuffer * buffer)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
    return lgmpClientStreamReadPeek(clientStream, buffer);

  return lgmpHostStreamReadPeek(hostStream, buffer);
}

static LGMP_STATUS consumerRelease(uint32_t direction,
    PLGMPHostStream hostStream, PLGMPClientStream clientStream,
    const LGMPStreamBuffer * buffer)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
    return lgmpClientStreamReadRelease(clientStream, buffer);

  return lgmpHostStreamReadRelease(hostStream, buffer);
}

static void setNotifiers(uint32_t direction, PLGMPHostStream hostStream,
    PLGMPClientStream clientStream, struct NotifyState * producer,
    struct NotifyState * consumer)
{
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
  {
    lgmpHostStreamSetNotifier(hostStream, streamNotify, producer);
    lgmpClientStreamSetNotifier(clientStream, streamNotify, consumer);
  }
  else
  {
    lgmpClientStreamSetNotifier(clientStream, streamNotify, producer);
    lgmpHostStreamSetNotifier(hostStream, streamNotify, consumer);
  }
}

static bool checkPolling(void)
{
  LGMPStreamPollState state = { 0 };
  const struct LGMPStreamPollConfig invalid =
  {
    .spinCount = 1U,
    .minWaitUs = 0U,
    .maxWaitUs = 10U
  };
  if (!testExpectStatus("invalid lgmpStreamPollInit",
        lgmpStreamPollInit(&state, invalid), LGMP_ERR_INVALID_ARGUMENT))
    return false;

  const struct LGMPStreamPollConfig config =
  {
    .spinCount = 2U,
    .minWaitUs = 10U,
    .maxWaitUs = 40U
  };
  if (!testExpectStatus("lgmpStreamPollInit",
        lgmpStreamPollInit(&state, config), LGMP_OK) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 0U) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 0U) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 10U) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 20U) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 40U) ||
      !TEST_CHECK(lgmpStreamPollIdle(&state) == 40U))
    return false;

  lgmpStreamPollActivity(&state);
  return TEST_CHECK(lgmpStreamPollIdle(&state) == 0U);
}

static bool runStream(uint32_t direction)
{
  bool                success             = false;
  struct TestFixture  fixture             = { 0 };
  PLGMPHostStream     hostStream          = NULL;
  PLGMPHostStream     invalidHostStream   = NULL;
  PLGMPClientStream   clientStream        = NULL;
  PLGMPClientStream   invalidStream       = NULL;
  PLGMPClient         client              = NULL;
  uint32_t            clientID            = 0U;
  struct NotifyState  producerNotify      = { 0 };
  struct NotifyState  consumerNotify      = { 0 };

  const struct LGMPStreamConfig invalidConfig =
  {
    .direction = direction,
    .policy    = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = 3U,
    .slotSize  = 32U
  };

  const struct LGMPStreamConfig config =
  {
    .direction = direction,
    .policy    = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = 2U,
    .slotSize  = 32U
  };

  if (!checkPolling() ||
      !testFixtureInit(&fixture) ||
      !testExpectStatus("invalid lgmpHostStreamNew",
        lgmpHostStreamNew(fixture.host, invalidConfig, &invalidHostStream),
        LGMP_ERR_INVALID_ARGUMENT) ||
      !TEST_CHECK(invalidHostStream == NULL) ||
      !testExpectStatus("lgmpHostStreamNew",
        lgmpHostStreamNew(fixture.host, config, &hostStream), LGMP_OK) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID))
    goto cleanup;

  struct LGMPStreamDescriptor descriptor;
  lgmpHostStreamGetDescriptor(hostStream, &descriptor);
  if (!TEST_CHECK(descriptor.magic == LGMP_STREAM_DESCRIPTOR_MAGIC) ||
      !TEST_CHECK(descriptor.version == LGMP_STREAM_DESCRIPTOR_VERSION) ||
      !TEST_CHECK(descriptor.direction == direction) ||
      !TEST_CHECK(descriptor.policy == LGMP_STREAM_RELIABLE_FIFO) ||
      !TEST_CHECK(descriptor.slotCount == config.slotCount) ||
      !TEST_CHECK(descriptor.slotSize == config.slotSize))
    goto cleanup;

  struct LGMPStreamDescriptor invalidDescriptor = descriptor;
  invalidDescriptor.magic = 0U;
  if (!testExpectStatus("invalid lgmpClientStreamAttach",
        lgmpClientStreamAttach(client, &invalidDescriptor, &invalidStream),
        LGMP_ERR_INVALID_MAGIC) ||
      !TEST_CHECK(invalidStream == NULL) ||
      !testExpectStatus("lgmpClientStreamAttach",
        lgmpClientStreamAttach(client, &descriptor, &clientStream),
        LGMP_OK) ||
      !testExpectStatus("unbound lgmpClientStreamActivate",
        lgmpClientStreamActivate(clientStream, NULL),
        LGMP_ERR_STREAM_UNBOUND))
    goto cleanup;

  uint32_t hostEpoch   = 0U;
  uint32_t clientEpoch = 0U;
  if (!testExpectStatus("zero-client lgmpHostStreamBind",
        lgmpHostStreamBind(hostStream, 0U, NULL),
        LGMP_ERR_INVALID_ARGUMENT) ||
      !testExpectStatus("lgmpHostStreamBind",
        lgmpHostStreamBind(hostStream, clientID, &hostEpoch), LGMP_OK) ||
      !TEST_CHECK(hostEpoch != 0U) ||
      !testExpectStatus("lgmpClientStreamActivate",
        lgmpClientStreamActivate(clientStream, &clientEpoch), LGMP_OK) ||
      !TEST_CHECK(clientEpoch == hostEpoch))
    goto cleanup;

  uint32_t bindingClient = 0U;
  uint32_t bindingEpoch  = 0U;
  if (!testExpectStatus("host stream binding",
        lgmpHostStreamGetBinding(hostStream, &bindingClient, &bindingEpoch),
        LGMP_OK) ||
      !TEST_CHECK(bindingClient == clientID) ||
      !TEST_CHECK(bindingEpoch == hostEpoch) ||
      !testExpectStatus("client stream binding",
        lgmpClientStreamGetBinding(clientStream, &bindingClient,
          &bindingEpoch), LGMP_OK) ||
      !TEST_CHECK(bindingClient == clientID) ||
      !TEST_CHECK(bindingEpoch == hostEpoch))
    goto cleanup;

  LGMPStreamBuffer buffer = { 0 };
  if (direction == LGMP_STREAM_HOST_TO_CLIENT)
  {
    if (!testExpectStatus("client write on host stream",
          lgmpClientStreamWriteAcquire(clientStream, &buffer),
          LGMP_ERR_INVALID_ARGUMENT) ||
        !testExpectStatus("host read on host stream",
          lgmpHostStreamReadPeek(hostStream, &buffer),
          LGMP_ERR_INVALID_ARGUMENT))
      goto cleanup;
  }
  else if (!testExpectStatus("host write on client stream",
        lgmpHostStreamWriteAcquire(hostStream, &buffer),
        LGMP_ERR_INVALID_ARGUMENT) ||
      !testExpectStatus("client read on client stream",
        lgmpClientStreamReadPeek(clientStream, &buffer),
        LGMP_ERR_INVALID_ARGUMENT))
    goto cleanup;

  LGMPStreamBuffer competing = { 0 };
  if (!testExpectStatus("stream write acquire for cancel",
        producerAcquire(direction, hostStream, clientStream, &buffer),
        LGMP_OK) ||
      !TEST_CHECK(buffer.capacity == config.slotSize) ||
      !testExpectStatus("duplicate stream write acquire",
        producerAcquire(direction, hostStream, clientStream, &competing),
        LGMP_ERR_STREAM_BUSY) ||
      !testExpectStatus("oversized stream write commit",
        producerCommit(direction, hostStream, clientStream, &buffer,
          config.slotSize + 1U), LGMP_ERR_INVALID_SIZE) ||
      !testExpectStatus("stream write cancel",
        producerCancel(direction, hostStream, clientStream, &buffer),
        LGMP_OK) ||
      !testExpectStatus("empty stream after cancel",
        consumerPeek(direction, hostStream, clientStream, &buffer),
        LGMP_ERR_STREAM_EMPTY))
    goto cleanup;

  setNotifiers(direction, hostStream, clientStream, &producerNotify,
      &consumerNotify);

  static const char first [] = "stream record one";
  static const char second[] = "stream record two";
  const char * values[]      = { first, second };
  const uint32_t sizes[]     = { sizeof(first), sizeof(second) };
  for(unsigned int i = 0; i < 2U; ++i)
  {
    if (!testExpectStatus("stream write acquire",
          producerAcquire(direction, hostStream, clientStream, &buffer),
          LGMP_OK) ||
        !TEST_CHECK(buffer.capacity == config.slotSize))
      goto cleanup;

    memcpy(buffer.data, values[i], sizes[i]);
    if (!testExpectStatus("stream write commit",
          producerCommit(direction, hostStream, clientStream, &buffer,
            sizes[i]), LGMP_OK))
      goto cleanup;
  }

  if (!testExpectStatus("full stream write acquire",
        producerAcquire(direction, hostStream, clientStream, &buffer),
        LGMP_ERR_STREAM_FULL))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    if (!testExpectStatus("stream read peek",
          consumerPeek(direction, hostStream, clientStream, &buffer),
          LGMP_OK) ||
        !TEST_CHECK(buffer.size == sizes[i]) ||
        !TEST_CHECK(memcmp(buffer.data, values[i], sizes[i]) == 0))
      goto cleanup;

    LGMPStreamBuffer duplicate = { 0 };
    if (!testExpectStatus("duplicate stream read peek",
          consumerPeek(direction, hostStream, clientStream, &duplicate),
          LGMP_ERR_STREAM_BUSY) ||
        !testExpectStatus("stream read release",
          consumerRelease(direction, hostStream, clientStream, &buffer),
          LGMP_OK))
      goto cleanup;
  }

  if (!testExpectStatus("empty stream read peek",
        consumerPeek(direction, hostStream, clientStream, &buffer),
        LGMP_ERR_STREAM_EMPTY) ||
      !TEST_CHECK(producerNotify.calls == 2U) ||
      !TEST_CHECK(producerNotify.reasons == LGMP_STREAM_NOTIFY_DATA) ||
      !TEST_CHECK(producerNotify.descriptorMagic ==
          LGMP_STREAM_DESCRIPTOR_MAGIC) ||
      !TEST_CHECK(consumerNotify.calls == 2U) ||
      !TEST_CHECK(consumerNotify.reasons == LGMP_STREAM_NOTIFY_CREDIT) ||
      !TEST_CHECK(consumerNotify.descriptorMagic ==
          LGMP_STREAM_DESCRIPTOR_MAGIC))
    goto cleanup;

  static const char draining[] = "draining record";
  if (!testExpectStatus("draining write acquire",
        producerAcquire(direction, hostStream, clientStream, &buffer),
        LGMP_OK))
    goto cleanup;
  memcpy(buffer.data, draining, sizeof(draining));
  if (!testExpectStatus("draining write commit",
        producerCommit(direction, hostStream, clientStream, &buffer,
          sizeof(draining)), LGMP_OK) ||
      !testExpectStatus("busy graceful unbind",
        lgmpHostStreamUnbind(hostStream), LGMP_ERR_STREAM_BUSY) ||
      !testExpectStatus("draining stream read",
        consumerPeek(direction, hostStream, clientStream, &buffer),
        LGMP_OK) ||
      !TEST_CHECK(buffer.size == sizeof(draining)) ||
      !TEST_CHECK(memcmp(buffer.data, draining, sizeof(draining)) == 0) ||
      !testExpectStatus("draining stream release",
        consumerRelease(direction, hostStream, clientStream, &buffer),
        LGMP_OK) ||
      !testExpectStatus("graceful unbind",
        lgmpHostStreamUnbind(hostStream), LGMP_OK) ||
      !testExpectStatus("activate after unbind",
        lgmpClientStreamActivate(clientStream, NULL),
        LGMP_ERR_STREAM_UNBOUND))
    goto cleanup;

  success = true;

cleanup:
  if (invalidStream)
    lgmpClientStreamDetach(&invalidStream);
  if (clientStream)
    lgmpClientStreamDetach(&clientStream);
  if (hostStream)
    lgmpHostStreamFree(&hostStream);
  if (invalidHostStream)
    lgmpHostStreamFree(&invalidHostStream);
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
  if (strcmp(argv[1], "host-to-client") == 0)
    success = runStream(LGMP_STREAM_HOST_TO_CLIENT);
  else if (strcmp(argv[1], "client-to-host") == 0)
    success = runStream(LGMP_STREAM_CLIENT_TO_HOST);
  else
    fprintf(stderr, "unknown stream test case: %s\n", argv[1]);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
