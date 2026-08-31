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

#include "lgmp/spmc.h"

static bool readExpected(PLGMPClientSPMC stream, const void * expected,
    uint32_t expectedSize, uint64_t expectedSequence,
    uint64_t expectedSkipped)
{
  uint8_t               data[64] = { 0U };
  struct LGMPSPMCRecord record;
  if (!testExpectStatus("lgmpClientSPMCRead",
        lgmpClientSPMCRead(stream, data, sizeof(data), &record), LGMP_OK) ||
      !TEST_CHECK(record.sequence == expectedSequence) ||
      !TEST_CHECK(record.skipped == expectedSkipped) ||
      !TEST_CHECK(record.size == expectedSize) ||
      !TEST_CHECK(record.reserved == 0U) ||
      !TEST_CHECK(memcmp(data, expected, expectedSize) == 0))
    return false;

  return true;
}

static bool readEmpty(PLGMPClientSPMC stream)
{
  uint8_t               data[64] = { 0U };
  struct LGMPSPMCRecord record;
  return testExpectStatus("empty lgmpClientSPMCRead",
      lgmpClientSPMCRead(stream, data, sizeof(data), &record),
      LGMP_ERR_STREAM_EMPTY);
}

static bool runBasic(void)
{
  bool               success             = false;
  struct TestFixture fixture             = { 0 };
  PLGMPHostSPMC      hostStream          = NULL;
  PLGMPClient        clients[2]          = { NULL, NULL };
  PLGMPClientSPMC    clientStreams[2]    = { NULL, NULL };
  PLGMPClientSPMC    invalidClientStream = NULL;
  uint32_t           clientIDs[2]        = { 0U, 0U };
  uint32_t           readerIDs[2]        = { UINT32_MAX, UINT32_MAX };
  uint32_t           epochs[2]           = { 0U, 0U };
  bool               readerBound[2]      = { false, false };

  const struct LGMPSPMCConfig config =
  {
    .slotCount  = 4U,
    .slotSize   = 32U,
    .maxReaders = 2U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostSPMCNew",
        lgmpHostSPMCNew(fixture.host, config, &hostStream), LGMP_OK))
    goto cleanup;

  struct LGMPSPMCDescriptor descriptor;
  lgmpHostSPMCGetDescriptor(hostStream, &descriptor);
  if (!TEST_CHECK(descriptor.magic == LGMP_SPMC_DESCRIPTOR_MAGIC) ||
      !TEST_CHECK(descriptor.version == LGMP_SPMC_DESCRIPTOR_VERSION) ||
      !TEST_CHECK(descriptor.size == (uint16_t)sizeof(descriptor)) ||
      !TEST_CHECK(descriptor.regionSize != 0U) ||
      !TEST_CHECK(descriptor.slotCount == config.slotCount) ||
      !TEST_CHECK(descriptor.slotSize == config.slotSize) ||
      !TEST_CHECK(descriptor.maxReaders == config.maxReaders) ||
      !TEST_CHECK(descriptor.reserved == 0U) ||
      !testFixtureStart(&fixture))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
    if (!testClientInit(&fixture, &clients[i], &clientIDs[i]))
      goto cleanup;

  struct LGMPSPMCDescriptor invalidDescriptor = descriptor;
  invalidDescriptor.magic = 0U;
  if (!testExpectStatus("invalid lgmpClientSPMCAttach",
        lgmpClientSPMCAttach(clients[0], &invalidDescriptor, 0U,
          &invalidClientStream), LGMP_ERR_INVALID_MAGIC) ||
      !TEST_CHECK(invalidClientStream == NULL))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    if (!testExpectStatus("lgmpHostSPMCReaderBind",
          lgmpHostSPMCReaderBind(hostStream, clientIDs[i], &readerIDs[i],
            &epochs[i]), LGMP_OK))
      goto cleanup;
    readerBound[i] = true;

    if (!TEST_CHECK(epochs[i] != 0U) ||
        !testExpectStatus("lgmpClientSPMCAttach",
          lgmpClientSPMCAttach(clients[i], &descriptor, readerIDs[i],
            &clientStreams[i]), LGMP_OK))
      goto cleanup;

    uint32_t activeEpoch = 0U;
    if (!testExpectStatus("lgmpClientSPMCActivate",
          lgmpClientSPMCActivate(clientStreams[i], &activeEpoch), LGMP_OK) ||
        !TEST_CHECK(activeEpoch == epochs[i]))
      goto cleanup;

    uint32_t bindingClientID = 0U;
    uint32_t bindingEpoch    = 0U;
    if (!testExpectStatus("lgmpClientSPMCGetBinding",
          lgmpClientSPMCGetBinding(clientStreams[i], &bindingClientID,
            &bindingEpoch), LGMP_OK) ||
        !TEST_CHECK(bindingClientID == clientIDs[i]) ||
        !TEST_CHECK(bindingEpoch == epochs[i]))
      goto cleanup;

    struct LGMPSPMCReaderState state;
    if (!testExpectStatus("lgmpHostSPMCReaderGetState",
          lgmpHostSPMCReaderGetState(hostStream, readerIDs[i], &state),
          LGMP_OK) ||
        !TEST_CHECK(state.readerID == readerIDs[i]) ||
        !TEST_CHECK(state.state == LGMP_SPMC_READER_READY) ||
        !TEST_CHECK(state.clientID == clientIDs[i]) ||
        !TEST_CHECK(state.epoch == epochs[i]) ||
        !TEST_CHECK(state.producerSequence == 0U) ||
        !TEST_CHECK(state.consumerSequence == 0U))
      goto cleanup;
  }

  if (!TEST_CHECK(readerIDs[0] != readerIDs[1]))
    goto cleanup;

  static const char first[] = "first SPMC record";

  uint64_t sequence = UINT64_MAX;
  if (!testExpectStatus("lgmpHostSPMCPublish",
        lgmpHostSPMCPublish(hostStream, first, sizeof(first), &sequence),
        LGMP_OK) ||
      !TEST_CHECK(sequence == 0U))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
    if (!readExpected(clientStreams[i], first, sizeof(first), 0U, 0U) ||
        !readEmpty(clientStreams[i]))
      goto cleanup;

  static const char prefix  [] = "split ";
  static const char suffix  [] = "record";
  static const char combined[] = "split record";
  if (!testExpectStatus("lgmpHostSPMCPublishV",
        lgmpHostSPMCPublishV(hostStream, prefix, sizeof(prefix) - 1U,
          suffix, sizeof(suffix), &sequence), LGMP_OK) ||
      !TEST_CHECK(sequence == 1U))
    goto cleanup;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    if (!readExpected(clientStreams[i], combined, sizeof(combined), 1U,
          0U))
      goto cleanup;

    struct LGMPSPMCReaderState state;
    if (!testExpectStatus("updated lgmpHostSPMCReaderGetState",
          lgmpHostSPMCReaderGetState(hostStream, readerIDs[i], &state),
          LGMP_OK) ||
        !TEST_CHECK(state.producerSequence == 2U) ||
        !TEST_CHECK(state.consumerSequence == 2U) ||
        !testExpectStatus("lgmpHostSPMCReaderUnbind",
          lgmpHostSPMCReaderUnbind(hostStream, readerIDs[i]), LGMP_OK))
      goto cleanup;
    readerBound[i] = false;

    if (!testExpectStatus("activate after reader unbind",
          lgmpClientSPMCActivate(clientStreams[i], NULL),
          LGMP_ERR_STREAM_UNBOUND))
      goto cleanup;
  }

  success = true;

cleanup:
  if (invalidClientStream)
    lgmpClientSPMCDetach(&invalidClientStream);
  for(unsigned int i = 0; i < 2U; ++i)
    if (clientStreams[i])
      lgmpClientSPMCDetach(&clientStreams[i]);
  if (hostStream)
  {
    for(unsigned int i = 0; i < 2U; ++i)
      if (readerBound[i] &&
          !testExpectStatus("cleanup lgmpHostSPMCReaderUnbind",
            lgmpHostSPMCReaderUnbind(hostStream, readerIDs[i]), LGMP_OK))
        success = false;
    lgmpHostSPMCFree(&hostStream);
  }
  for(unsigned int i = 0; i < 2U; ++i)
    if (clients[i])
      lgmpClientFree(&clients[i]);
  if (!testFixtureDestroy(&fixture))
    success = false;
  return success;
}

static bool runOverrun(void)
{
  bool               success      = false;
  struct TestFixture fixture      = { 0 };
  PLGMPHostSPMC      hostStream   = NULL;
  PLGMPClient        client       = NULL;
  PLGMPClientSPMC    clientStream = NULL;
  uint32_t           clientID     = 0U;
  uint32_t           readerID     = UINT32_MAX;
  uint32_t           hostEpoch    = 0U;
  bool               readerBound  = false;

  const struct LGMPSPMCConfig config =
  {
    .slotCount  = 4U,
    .slotSize   = 16U,
    .maxReaders = 1U
  };

  if (!testFixtureInit(&fixture) ||
      !testExpectStatus("lgmpHostSPMCNew",
        lgmpHostSPMCNew(fixture.host, config, &hostStream), LGMP_OK) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID) ||
      !testExpectStatus("lgmpHostSPMCReaderBind",
        lgmpHostSPMCReaderBind(hostStream, clientID, &readerID, &hostEpoch),
        LGMP_OK))
    goto cleanup;
  readerBound = true;

  struct LGMPSPMCDescriptor descriptor;
  lgmpHostSPMCGetDescriptor(hostStream, &descriptor);
  uint32_t clientEpoch = 0U;
  if (!testExpectStatus("lgmpClientSPMCAttach",
        lgmpClientSPMCAttach(client, &descriptor, readerID, &clientStream),
        LGMP_OK) ||
      !testExpectStatus("lgmpClientSPMCActivate",
        lgmpClientSPMCActivate(clientStream, &clientEpoch), LGMP_OK) ||
      !TEST_CHECK(clientEpoch == hostEpoch))
    goto cleanup;

  static const uint32_t values[] =
  {
    100U, 101U, 102U, 103U, 104U, 105U, 106U, 107U
  };

  for(unsigned int i = 0; i < 6U; ++i)
  {
    uint64_t sequence = UINT64_MAX;
    if (!testExpectStatus("overwriting lgmpHostSPMCPublish",
          lgmpHostSPMCPublish(hostStream, &values[i], sizeof(values[i]),
            &sequence), LGMP_OK) ||
        !TEST_CHECK(sequence == i))
      goto cleanup;
  }

  for(unsigned int i = 2U; i < 6U; ++i)
  {
    const uint64_t skipped = i == 2U ? 2U : 0U;
    if (!readExpected(clientStream, &values[i], sizeof(values[i]), i,
          skipped))
      goto cleanup;
  }

  if (!readEmpty(clientStream))
    goto cleanup;

  for(unsigned int i = 6U; i < 8U; ++i)
  {
    uint64_t sequence = UINT64_MAX;
    if (!testExpectStatus("pre-sync lgmpHostSPMCPublish",
          lgmpHostSPMCPublish(hostStream, &values[i], sizeof(values[i]),
            &sequence), LGMP_OK) ||
        !TEST_CHECK(sequence == i))
      goto cleanup;
  }

  uint64_t synced = UINT64_MAX;
  if (!testExpectStatus("lgmpClientSPMCSync",
        lgmpClientSPMCSync(clientStream, &synced), LGMP_OK) ||
      !TEST_CHECK(synced == 2U) ||
      !readEmpty(clientStream))
    goto cleanup;

  struct LGMPSPMCReaderState state;
  if (!testExpectStatus("lgmpHostSPMCReaderGetState",
        lgmpHostSPMCReaderGetState(hostStream, readerID, &state), LGMP_OK) ||
      !TEST_CHECK(state.producerSequence == 8U) ||
      !TEST_CHECK(state.consumerSequence == 8U))
    goto cleanup;

  success = true;

cleanup:
  if (clientStream)
    lgmpClientSPMCDetach(&clientStream);
  if (hostStream)
  {
    if (readerBound &&
        !testExpectStatus("cleanup lgmpHostSPMCReaderUnbind",
          lgmpHostSPMCReaderUnbind(hostStream, readerID), LGMP_OK))
      success = false;
    lgmpHostSPMCFree(&hostStream);
  }
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
  if (strcmp(argv[1], "basic") == 0)
    success = runBasic();
  else if (strcmp(argv[1], "overrun") == 0)
    success = runOverrun();
  else
    fprintf(stderr, "unknown SPMC test case: %s\n", argv[1]);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
