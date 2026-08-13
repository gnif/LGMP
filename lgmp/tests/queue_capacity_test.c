/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2026 Geoffrey McRae <geoff@hostfission.com>
 * https://github.com/gnif/LGMP
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lgmp/client.h"
#include "lgmp/host.h"

#define TEST_MEMORY_SIZE (1024u * 1024u)
#define TEST_QUEUE_BASE  0x1000u
#define TEST_UDATA       UINT64_C(0x123456789abcdef0)

struct HostPump
{
  PLGMPHost host;
  atomic_bool stop;
  LGMP_STATUS status;
};

static void * hostPump(void * opaque)
{
  struct HostPump * pump = opaque;
  pump->status = LGMP_OK;

  while(!atomic_load_explicit(&pump->stop, memory_order_relaxed))
  {
    pump->status = lgmpHostProcess(pump->host);
    if (pump->status != LGMP_OK)
      break;
    usleep(1000);
  }

  return NULL;
}

static bool expectStatus(const char * operation, LGMP_STATUS actual,
    LGMP_STATUS expected)
{
  if (actual == expected)
    return true;

  fprintf(stderr, "%s: expected %s, got %s\n", operation,
      lgmpStatusString(expected), lgmpStatusString(actual));
  return false;
}

int main(void)
{
  _Static_assert(LGMP_MAX_QUEUES == 6,
      "this test covers the six-queue protocol layout");

  int result = EXIT_FAILURE;
  void * memory = calloc(1, TEST_MEMORY_SIZE);
  PLGMPHost host = NULL;
  PLGMPMemory payload = NULL;
  PLGMPClient clients[2] = { NULL, NULL };
  PLGMPClientQueue clientQueues[2] = { NULL, NULL };
  PLGMPHostQueue queues[LGMP_MAX_QUEUES] = { NULL };
  uint32_t clientIDs[2] = { 0 };
  pthread_t pumpThread;
  bool pumpStarted = false;
  struct HostPump pump = { 0 };

  if (!memory)
  {
    perror("calloc");
    goto cleanup;
  }

  if (!expectStatus("lgmpHostInit",
        lgmpHostInit(memory, TEST_MEMORY_SIZE, &host, 0, NULL), LGMP_OK))
    goto cleanup;

  for(unsigned int i = 0; i < LGMP_MAX_QUEUES; ++i)
  {
    const struct LGMPQueueConfig config =
    {
      .queueID = TEST_QUEUE_BASE + i,
      .numMessages = 2,
      .subTimeout = 5000
    };

    if (!expectStatus("lgmpHostQueueNew",
          lgmpHostQueueNew(host, config, &queues[i]), LGMP_OK))
      goto cleanup;
  }

  const struct LGMPQueueConfig extraConfig =
  {
    .queueID = TEST_QUEUE_BASE + LGMP_MAX_QUEUES,
    .numMessages = 2,
    .subTimeout = 5000
  };
  PLGMPHostQueue extraQueue = NULL;
  if (!expectStatus("seventh lgmpHostQueueNew",
        lgmpHostQueueNew(host, extraConfig, &extraQueue), LGMP_ERR_NO_QUEUES))
    goto cleanup;

  pump.host = host;
  atomic_init(&pump.stop, false);
  if (pthread_create(&pumpThread, NULL, hostPump, &pump) != 0)
  {
    perror("pthread_create");
    goto cleanup;
  }
  pumpStarted = true;

  for(unsigned int i = 0; i < 2; ++i)
  {
    if (!expectStatus("lgmpClientInit",
          lgmpClientInit(memory, TEST_MEMORY_SIZE, &clients[i]), LGMP_OK) ||
        !expectStatus("lgmpClientSessionInit",
          lgmpClientSessionInit(clients[i], NULL, NULL, &clientIDs[i], NULL),
          LGMP_OK) ||
        !expectStatus("lgmpClientSubscribe",
          lgmpClientSubscribe(clients[i], TEST_QUEUE_BASE + 5,
            &clientQueues[i]), LGMP_OK))
      goto cleanup;
  }

  static const char value[32] = "sixth queue targeted payload";
  if (!expectStatus("lgmpHostMemAlloc",
        lgmpHostMemAlloc(host, sizeof(value), &payload), LGMP_OK))
    goto cleanup;
  memcpy(lgmpHostMemPtr(payload), value, sizeof(value));

  unsigned int recipients = 0;
  if (!expectStatus("lgmpHostQueuePostForClients",
        lgmpHostQueuePostForClients(queues[5], TEST_UDATA, payload,
          &clientIDs[0], 1, &recipients), LGMP_OK))
    goto cleanup;
  if (recipients != 1)
  {
    fprintf(stderr, "targeted post reached %u clients, expected 1\n",
        recipients);
    goto cleanup;
  }

  LGMPMessage message;
  if (!expectStatus("non-target lgmpClientProcess",
        lgmpClientProcess(clientQueues[1], &message), LGMP_ERR_QUEUE_EMPTY) ||
      !expectStatus("target lgmpClientProcess",
        lgmpClientProcess(clientQueues[0], &message), LGMP_OK))
    goto cleanup;

  if (message.udata != TEST_UDATA || message.size != sizeof(value) ||
      memcmp(message.mem, value, sizeof(value)) != 0)
  {
    fprintf(stderr, "targeted message contents did not match\n");
    goto cleanup;
  }

  if (!expectStatus("lgmpClientMessageDone",
        lgmpClientMessageDone(clientQueues[0]), LGMP_OK))
    goto cleanup;

  const uint32_t missingClientID = UINT32_MAX;
  recipients = 1;
  if (!expectStatus("unmatched lgmpHostQueuePostForClients",
        lgmpHostQueuePostForClients(queues[5], TEST_UDATA, payload,
          &missingClientID, 1, &recipients), LGMP_OK))
    goto cleanup;
  if (recipients != 0 || lgmpHostQueuePayloadPending(queues[5], payload))
  {
    fprintf(stderr, "unmatched targeted post transferred payload ownership\n");
    goto cleanup;
  }

  result = EXIT_SUCCESS;

cleanup:
  if (pumpStarted)
  {
    atomic_store_explicit(&pump.stop, true, memory_order_relaxed);
    pthread_join(pumpThread, NULL);
    if (result == EXIT_SUCCESS && pump.status != LGMP_OK)
    {
      fprintf(stderr, "lgmpHostProcess: %s\n", lgmpStatusString(pump.status));
      result = EXIT_FAILURE;
    }
  }

  for(unsigned int i = 0; i < 2; ++i)
  {
    if (clientQueues[i])
      lgmpClientUnsubscribe(&clientQueues[i]);
    if (clients[i])
      lgmpClientFree(&clients[i]);
  }
  if (payload)
    lgmpHostMemFree(&payload);
  if (host)
    lgmpHostFree(&host);
  free(memory);
  return result;
}
