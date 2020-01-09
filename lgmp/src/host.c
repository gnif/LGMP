/*
LGMP - Looking Glass Memory Protocol
Copyright (C) 2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "lgmp/host.h"

#include "lgmp.h"
#include "headers.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#ifndef LGMP_REALCY
#define LGMP_MAX_MESSAGE_AGE   150   //150ms
#define LGMP_MAX_QUEUE_TIMEOUT 10000 //10s
#else
#define LGMP_MAX_MESSAGE_AGE   1500   //1500ms
#define LGMP_MAX_QUEUE_TIMEOUT 100000 //100s
#endif

struct LGMPHostQueue
{
  PLGMPHost    host;
  unsigned int index;
  uint32_t     position;
  uint32_t     count;

  unsigned int start;
  uint64_t     msgTimeout;
};

struct LGMPHost
{
  uint8_t * mem;
  size_t    size;
  size_t    avail;
  size_t    nextFree;
  bool      started;

  struct LGMPHeader * header;
  struct LGMPHostQueue   queues[LGMP_MAX_QUEUES];
};

LGMP_STATUS lgmpHostInit(void *mem, const size_t size, PLGMPHost * result)
{
  assert(mem);
  assert(size > 0);
  assert(result);

  *result = NULL;

  // make sure that lgmpGetClockMS works
  if (!lgmpGetClockMS())
    return LGMP_ERR_CLOCK_FAILURE;

  if (size < sizeof(struct LGMPHeader))
    return LGMP_ERR_INVALID_SIZE;

  *result = malloc(sizeof(struct LGMPHost));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPHost host = *result;

  host->mem      = mem;
  host->size     = size;
  host->avail    = size - sizeof(struct LGMPHeader);
  host->nextFree = sizeof(struct LGMPHeader);
  host->header   = (struct LGMPHeader *)mem;
  host->started  = false;

  // ensure the sessionID changes so that clients can determine if the host was
  // restarted.
  const uint32_t sessionID = host->header->sessionID;
  while(sessionID == host->header->sessionID)
    host->header->sessionID = rand();

  host->header->magic     = LGMP_PROTOCOL_MAGIC;
  host->header->timestamp = lgmpGetClockMS();
  host->header->version   = LGMP_PROTOCOL_VERSION;
  host->header->caps      = 0;
  host->header->numQueues = 0;

  return LGMP_OK;
}

void lgmpHostFree(PLGMPHost * host)
{
  assert(host);
  if (!*host)
    return;

  free(*host);
  *host = NULL;
}

LGMP_STATUS lgmpHostQueueNew(PLGMPHost host, uint32_t queueID, uint32_t numMessages, PLGMPHostQueue * result)
{
  assert(host);
  assert(result);

  *result = NULL;
  if (host->started)
    return LGMP_ERR_HOST_STARTED;

  if (host->header->numQueues == LGMP_MAX_QUEUES)
    return LGMP_ERR_NO_QUEUES;

  // add an extra message as the end marker
  ++numMessages;

  const size_t needed = sizeof(struct LGMPHeaderMessage) * numMessages;
  if (host->avail < needed)
    return LGMP_ERR_NO_SHARED_MEM;

  *result = &host->queues[host->header->numQueues];
  PLGMPHostQueue queue = *result;

  queue->host       = host;
  queue->index      = host->header->numQueues;
  queue->position   = 0;
  queue->count      = 0;
  queue->start      = 0;
  queue->msgTimeout = lgmpGetClockMS() + LGMP_MAX_MESSAGE_AGE;

  struct LGMPHeaderQueue * hq = &host->header->queues[host->header->numQueues++];
  hq->queueID        = queueID;
  hq->numMessages    = numMessages;
  hq->newSubCount    = 0;
  atomic_flag_clear(&hq->lock);
  hq->subs           = 0;
  hq->position       = 0;
  hq->messagesOffset = host->nextFree;

  host->avail    -= needed;
  host->nextFree += needed;

  return LGMP_OK;
}

uint32_t lgmpHostQueueNewSubs(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_exchange(&queue->host->header->queues[queue->index].newSubCount, 0);
}

uint32_t lgmpHostQueuePending(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_load(&queue->count);
}

LGMP_STATUS lgmpHostProcess(PLGMPHost host)
{
  assert(host);
  const uint64_t now = lgmpGetClockMS();
  atomic_store(&host->header->timestamp, now);

  // each queue
  for(unsigned int i = 0; i < host->header->numQueues; ++i)
  {
    struct LGMPHostQueue *queue = &host->queues[i];

    // check the first message
    if(!atomic_load(&queue->count))
      continue;

    struct LGMPHeaderQueue   *hq       = &host->header->queues[i];
    struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
      (host->mem + hq->messagesOffset);
    struct LGMPHeaderMessage *msg = &messages[queue->start];

    while(atomic_flag_test_and_set(&hq->lock)) {};
    uint64_t subs = atomic_load(&hq->subs);
    uint32_t pend = atomic_load(&msg->pendingSubs);

    const uint32_t newBadSubs = pend & ~LGMP_SUBS_BAD(subs);
    if (newBadSubs && now > atomic_load(&queue->msgTimeout))
    {
      // reset garbage collection timeout for new bad subs
      subs = LGMP_SUBS_OR_BAD(subs, newBadSubs);
      const uint64_t timeout = now + LGMP_MAX_QUEUE_TIMEOUT;
      for(unsigned int id = 0; id < 32; ++id)
        if (newBadSubs & (1 << id))
          hq->timeout[id] = timeout;

      // clear the pending subs
      atomic_store(&msg->pendingSubs, 0);
      pend = 0;
    }

    if (!(pend & ~LGMP_SUBS_BAD(subs)))
    {
      // message finished
      if (++queue->start == hq->numMessages)
        queue->start = 0;

      // decrement the queue and check if we need to update the timeout
      if (atomic_fetch_sub(&queue->count, 1) == 1)
        atomic_store(&queue->msgTimeout, now + LGMP_MAX_MESSAGE_AGE);
    }

    atomic_store(&hq->subs, subs);
    atomic_flag_clear(&hq->lock);
  }

  return LGMP_OK;
}

size_t lgmpHostMemAvail(PLGMPHost host)
{
  assert(host);
  return host->avail;
}

LGMP_STATUS lgmpHostMemAlloc(PLGMPHost host, uint32_t size, PLGMPMemory *result)
{
  assert(host);
  assert(result);

  if (size > host->avail)
    return LGMP_ERR_NO_SHARED_MEM;

  *result = malloc(sizeof(struct LGMPMemory));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPMemory mem = *result;
  mem->host   = host;
  mem->offset = host->nextFree;
  mem->size   = size;
  mem->mem    = host->mem + host->nextFree;

  host->nextFree += size;
  host->avail    -= size;

  return LGMP_OK;
}

void lgmpHostMemFree(PLGMPMemory * mem)
{
  assert(mem);
  if (!*mem)
    return;

  free(*mem);
  *mem = NULL;
}

void * lgmpHostMemPtr(PLGMPMemory mem)
{
  assert(mem);
  return mem->mem;
}

LGMP_STATUS lgmpHostQueuePost(PLGMPHostQueue queue, uint32_t udata, PLGMPMemory payload)
{
  struct LGMPHeaderQueue *hq = &queue->host->header->queues[queue->index];

  // get the subscribers
  const uint64_t subs = atomic_load(&hq->subs);
  const uint32_t pend = LGMP_SUBS_ON(subs) & ~(LGMP_SUBS_BAD(subs));

  // if nobody has subscribed there is no point in posting the message
  if (!pend)
    return LGMP_OK;

  // we should never fully fill the buffer
  if (atomic_load(&queue->count) == hq->numMessages - 1)
    return LGMP_ERR_QUEUE_FULL;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->host->mem + hq->messagesOffset);

  struct LGMPHeaderMessage *msg = &messages[queue->position];

  msg->udata       = udata;
  msg->size        = payload->size;
  msg->offset      = payload->offset;
  msg->pendingSubs = pend;

  // increment the queue count, if it were zero update the msgTimeout
  while(atomic_flag_test_and_set(&hq->lock)) {};
  if (atomic_fetch_add(&queue->count, 1) == 0)
    atomic_store(&queue->msgTimeout, lgmpGetClockMS() + LGMP_MAX_MESSAGE_AGE);
  atomic_flag_clear(&hq->lock);

  if (++queue->position == hq->numMessages)
    queue->position = 0;

  atomic_store(&hq->position, queue->position);
  return LGMP_OK;
}