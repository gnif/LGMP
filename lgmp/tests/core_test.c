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

struct StatusName
{
  LGMP_STATUS  status;
  const char * name;
};

static bool checkStatusNames(void)
{
  static const struct StatusName names[] =
  {
    { LGMP_OK                    , "LGMP_OK"                     },
    { LGMP_ERR_CLOCK_FAILURE     , "LGMP_CLOCK_FAILURE"          },
    { LGMP_ERR_INVALID_ARGUMENT  , "LGMP_ERR_INVALID_ARGUMENT"   },
    { LGMP_ERR_INVALID_SIZE      , "LGMP_ERR_INVALID_SIZE"       },
    { LGMP_ERR_INVALID_ALIGNMENT , "LGMP_ERR_INVALID_ALIGNMENT"  },
    { LGMP_ERR_INVALID_SESSION   , "LGMP_ERR_INVALID_SESSION"    },
    { LGMP_ERR_NO_MEM            , "LGMP_ERR_NO_MEM"             },
    { LGMP_ERR_NO_SHARED_MEM     , "LGMP_ERR_NO_SHARED_MEM"      },
    { LGMP_ERR_HOST_STARTED      , "LGMP_ERR_HOST_STARTED"       },
    { LGMP_ERR_NO_QUEUES         , "LGMP_ERR_NO_QUEUES"          },
    { LGMP_ERR_QUEUE_FULL        , "LGMP_ERR_QUEUE_FULL"         },
    { LGMP_ERR_QUEUE_EMPTY       , "LGMP_ERR_QUEUE_EMPTY"        },
    { LGMP_ERR_QUEUE_UNSUBSCRIBED, "LGMP_ERR_QUEUE_UNSUBSCRIBED" },
    { LGMP_ERR_QUEUE_TIMEOUT     , "LGMP_ERR_QUEUE_TIMEOUT"      },
    { LGMP_ERR_INVALID_MAGIC     , "LGMP_ERR_INVALID_MAGIC"      },
    { LGMP_ERR_INVALID_VERSION   , "LGMP_ERR_INVALID_VERSION"    },
    { LGMP_ERR_NO_SUCH_QUEUE     , "LGMP_ERR_NO_SUCH_QUEUE"      },
    { LGMP_ERR_CORRUPTED         , "LGMP_ERR_CORRUPTED"          },
    { LGMP_ERR_QUEUE_BUSY        , "LGMP_ERR_QUEUE_BUSY"         },
    { LGMP_ERR_STREAM_FULL       , "LGMP_ERR_STREAM_FULL"        },
    { LGMP_ERR_STREAM_EMPTY      , "LGMP_ERR_STREAM_EMPTY"       },
    { LGMP_ERR_STREAM_UNBOUND    , "LGMP_ERR_STREAM_UNBOUND"     },
    { LGMP_ERR_STREAM_STALE      , "LGMP_ERR_STREAM_STALE"       },
    { LGMP_ERR_STREAM_BUSY       , "LGMP_ERR_STREAM_BUSY"        }
  };

  for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    const char * actual = lgmpStatusString(names[i].status);
    if (strcmp(actual, names[i].name) != 0)
    {
      fprintf(stderr, "status %u: expected %s, got %s\n",
          (unsigned int)names[i].status, names[i].name, actual);
      return false;
    }
  }

  return TEST_CHECK(strcmp(lgmpStatusString((LGMP_STATUS)UINT32_MAX),
      "Invalid status!") == 0);
}

int main(void)
{
  int                result            = EXIT_FAILURE;
  struct TestFixture fixture           = { 0 };
  PLGMPClient        client            = NULL;
  PLGMPClient        restarted         = NULL;
  PLGMPClient        invalidClient     = NULL;
  PLGMPClientQueue   queue             = NULL;
  PLGMPHost          invalidHost       = NULL;
  PLGMPHostQueue     hostQueue         = NULL;
  PLGMPMemory        allocation        = NULL;
  PLGMPMemory        invalidAllocation = NULL;
  uint32_t           clientID          = 0U;
  uint32_t           restartedID       = 0U;

  if (!checkStatusNames() || !testFixtureInit(&fixture))
    goto cleanup;

  if (!testExpectStatus("small lgmpHostInit",
        lgmpHostInit(fixture.memory, 1U, &invalidHost,
          testSessionUdataSize, (uint8_t *)testSessionUdata),
        LGMP_ERR_INVALID_SIZE) ||
      !TEST_CHECK(invalidHost == NULL) ||
      !testExpectStatus("small lgmpClientInit",
        lgmpClientInit(fixture.memory, 1U, &invalidClient),
        LGMP_ERR_INVALID_SIZE) ||
      !TEST_CHECK(invalidClient == NULL))
    goto cleanup;

  if (!testExpectStatus("invalid alignment",
        lgmpHostMemAllocAligned(fixture.host, 64U, 3U,
          &invalidAllocation), LGMP_ERR_INVALID_ALIGNMENT) ||
      !TEST_CHECK(invalidAllocation == NULL) ||
      !testExpectStatus("oversized aligned allocation",
        lgmpHostMemAllocAligned(fixture.host, UINT32_MAX, 4U,
          &invalidAllocation), LGMP_ERR_INVALID_SIZE) ||
      !TEST_CHECK(invalidAllocation == NULL))
    goto cleanup;

  const size_t availableBefore = lgmpHostMemAvail(fixture.host);
  if (!testExpectStatus("aligned allocation",
        lgmpHostMemAllocAligned(fixture.host, 73U, 256U, &allocation),
        LGMP_OK) ||
      !TEST_CHECK(allocation != NULL) ||
      !TEST_CHECK(((uintptr_t)lgmpHostMemPtr(allocation) & 255U) == 0U) ||
      !TEST_CHECK(lgmpHostMemAvail(fixture.host) < availableBefore))
    goto cleanup;

  lgmpHostMemFree(&allocation);
  if (!TEST_CHECK(allocation == NULL) ||
      !TEST_CHECK(lgmpHostMemAvail(fixture.host) < availableBefore))
    goto cleanup;

  const uint32_t unavailableSize =
    (uint32_t)lgmpHostMemAvail(fixture.host) + 1U;
  if (!testExpectStatus("exhausted shared memory",
        lgmpHostMemAlloc(fixture.host, unavailableSize, &allocation),
        LGMP_ERR_NO_SHARED_MEM) ||
      !TEST_CHECK(allocation == NULL))
    goto cleanup;

  const struct LGMPQueueConfig invalidConfig =
  {
    .queueID     = 0x100U,
    .numMessages = 3U,
    .subTimeout  = 1000U
  };
  if (!testExpectStatus("non-power-of-two queue",
        lgmpHostQueueNew(fixture.host, invalidConfig, &hostQueue),
        LGMP_ERR_INVALID_ARGUMENT) ||
      !TEST_CHECK(hostQueue == NULL) ||
      !testFixtureStart(&fixture) ||
      !testClientInit(&fixture, &client, &clientID) ||
      !TEST_CHECK(clientID != 0U) ||
      !testExpectStatus("missing queue subscription",
        lgmpClientSubscribe(client, invalidConfig.queueID, &queue),
        LGMP_ERR_NO_SUCH_QUEUE) ||
      !TEST_CHECK(queue == NULL))
    goto cleanup;

  if (!testFixtureRestart(&fixture) ||
      !TEST_CHECK(!lgmpClientSessionValid(client)))
    goto cleanup;

  if (!testClientInit(&fixture, &restarted, &restartedID) ||
      !TEST_CHECK(restartedID != 0U))
    goto cleanup;

  result = EXIT_SUCCESS;

cleanup:
  if (queue)
    (void)lgmpClientUnsubscribe(&queue);
  if (invalidClient)
    lgmpClientFree(&invalidClient);
  if (restarted)
    lgmpClientFree(&restarted);
  if (client)
    lgmpClientFree(&client);
  if (invalidHost)
    lgmpHostFree(&invalidHost);
  if (allocation)
    lgmpHostMemFree(&allocation);
  if (!testFixtureDestroy(&fixture))
    result = EXIT_FAILURE;
  return result;
}
