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

#define LGMP_MAX_MESSAGE_AGE   150   //150ms
#define LGMP_MAX_QUEUE_TIMEOUT 10000 //10s

struct LGMPHost
{
  uint8_t * mem;
  size_t    size;
  size_t    avail;
  size_t    nextFree;
  bool      started;

  struct LGMPHeader * header;
  struct LGMPQueue    queues[LGMP_MAX_QUEUES];
};

LGMP_STATUS lgmpHostInit(void *mem, const size_t size, LGMPHost * result)
{
  assert(mem);
  assert(size > 0);
  assert(result);

  *result = NULL;
  if (size < sizeof(struct LGMPHeader))
    return LGMP_ERR_INVALID_SIZE;

  *result = malloc(sizeof(struct LGMPHost));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  LGMPHost host = *result;

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
  host->header->heartbeat = 0;
  host->header->version   = LGMP_PROTOCOL_VERSION;
  host->header->caps      = 0;
  host->header->numQueues = 0;

  return LGMP_OK;
}

void lgmpHostFree(LGMPHost * host)
{
  assert(host);
  if (!*host)
    return;

  free(*host);
  *host = NULL;
}

LGMP_STATUS lgmpHostAddQueue(LGMPHost host, uint32_t type, uint32_t numMessages, LGMPQueue * result)
{
  assert(host);
  assert(result);

  *result = NULL;
  if (host->started)
    return LGMP_ERR_HOST_STARTED;

  if (host->header->numQueues == LGMP_MAX_QUEUES)
    return LGMP_ERR_NO_QUEUES;

  // we need an extra message to mark the end of the ring
  numMessages += 1;

  const size_t needed = sizeof(struct LGMPHeaderMessage) * numMessages;
  if (host->avail < needed)
    return LGMP_ERR_NO_SHARED_MEM;

  *result = &host->queues[host->header->numQueues];
  LGMPQueue queue = *result;

  queue->host       = host;
  queue->client     = NULL;
  queue->index      = host->header->numQueues;
  queue->position   = 0;
  queue->start      = 0;
  queue->count      = 0;
  queue->msgTimeout = lgmpGetClock() + LGMP_MAX_MESSAGE_AGE;

  struct LGMPHeaderQueue * hq = &host->header->queues[host->header->numQueues++];
  hq->type           = type;
  hq->numMessages    = numMessages;
  hq->lock           = 0;
  hq->subs           = 0;
  hq->badSubs        = 0;
  hq->position       = 0;
  hq->messagesOffset = host->nextFree;

  host->avail    -= needed;
  host->nextFree += needed;

  return LGMP_OK;
}

LGMP_STATUS lgmpHostProcess(LGMPHost host)
{
  assert(host);

  ++host->header->heartbeat;
  const uint64_t now = lgmpGetClock();

  // each queue
  for(unsigned int i = 0; i < host->header->numQueues; ++i)
  {
    struct LGMPQueue         *queue    = &host->queues[i];
    struct LGMPHeaderQueue   *hq       = &host->header->queues[i];
    struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
      (host->mem + hq->messagesOffset);

    // check the first message
    if(queue->count)
    {
      struct LGMPHeaderMessage *msg = &messages[queue->start];
      if ((msg->pendingSubs & ~hq->badSubs) && now > queue->msgTimeout)
      {
        printf("Timeout %08x %lu %lu\n", msg->pendingSubs, now, queue->msgTimeout);
        // take the queue lock
        while(__sync_lock_test_and_set(&hq->lock, 1)) while(hq->lock);

        // get the new bad subscribers
        const uint32_t newBadSubs = hq->subs & msg->pendingSubs;
        __sync_fetch_and_or(&hq->badSubs, newBadSubs);

        // reset garbage collection timeout for new bad subs
        if (newBadSubs)
        {
          const uint64_t timeout = now + LGMP_MAX_QUEUE_TIMEOUT;
          for(unsigned int id = 0; id < 32; ++id)
            if (newBadSubs & (1 << id))
              queue->timeout[id] = timeout;
        }

        // clear the pending subs and release the queue lock
        __sync_fetch_and_and(&msg->pendingSubs, 0);
        __sync_lock_release(&hq->lock);
      }

      if (!(msg->pendingSubs & ~hq->badSubs))
      {
        // message finished
        if (++queue->start == hq->numMessages)
          queue->start = 0;

        // decrement the queue and check if we need to update the timeout
        if (__sync_fetch_and_sub(&queue->count, 1))
          queue->msgTimeout = now + LGMP_MAX_MESSAGE_AGE;
      }
    }

    // recover subs for reuse that have been flagged as bad and have exceeded the queue timeout
    if (hq->badSubs)
    {
      uint32_t reap = 0;
      for(unsigned int id = 0; id < 32; ++id)
      {
        if ((hq->badSubs & (1 << id)) && now > queue->timeout[id])
          reap |= (1 << id);
      }

      if (reap)
      {
        // take the queue lock
        while(__sync_lock_test_and_set(&hq->lock, 1)) while(hq->lock);

        // clear the reaped subs
        __sync_fetch_and_and(&hq->badSubs, ~reap);
        __sync_fetch_and_and(&hq->subs   , ~reap);

        // relese the lock
        __sync_lock_release(&hq->lock);
      }
    }
  }

  return LGMP_OK;
}

LGMP_STATUS lgmpHostMemAlloc(LGMPHost host, uint32_t size, LGMPMemory *result)
{
  assert(host);
  assert(result);

  if (size > host->avail)
    return LGMP_ERR_NO_SHARED_MEM;

  *result = malloc(sizeof(struct LGMPMemory));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  LGMPMemory mem = *result;
  mem->host   = host;
  mem->offset = host->nextFree;
  mem->size   = size;
  mem->mem    = host->mem + host->nextFree;

  host->nextFree += size;
  host->avail    -= size;

  return LGMP_OK;
}

void lgmpHostMemFree(LGMPMemory * mem)
{
  assert(mem);
  if (!*mem)
    return;

  free(*mem);
  *mem = NULL;
}

void * lgmpHostMemPtr(LGMPMemory mem)
{
  assert(mem);
  return mem->mem;
}

LGMP_STATUS lgmpHostPost(LGMPQueue queue, uint32_t type, LGMPMemory payload)
{
  struct LGMPHeaderQueue *hq = &queue->host->header->queues[queue->index];

  // we should never fully fill the buffer
  if (queue->count == hq->numMessages - 1)
    return LGMP_ERR_QUEUE_FULL;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->host->mem + hq->messagesOffset);

  struct LGMPHeaderMessage *msg = &messages[queue->position];

  msg->type        = type;
  msg->size        = payload->size;
  msg->offset      = payload->offset;

  // take the queue lock
  while(__sync_lock_test_and_set(&hq->lock, 1)) while(hq->lock);

  // copy the subs into the message
  msg->pendingSubs = hq->subs & ~hq->badSubs;

  // release the lock
  __sync_lock_release(&hq->lock);

  // increment the queue count, if it were zero update the msgTimeout
  if (!__sync_fetch_and_add(&queue->count, 1))
    queue->msgTimeout = lgmpGetClock() + LGMP_MAX_MESSAGE_AGE;

  if (++queue->position == hq->numMessages)
    queue->position = 0;

  if (hq->position == hq->numMessages - 1)
    hq->position = 0;
  else
    ++hq->position;
}