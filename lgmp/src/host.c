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

#define ALIGN(x) ((x + (3)) & ~(3))

struct LGMPHostQueue
{
  PLGMPHost    host;
  unsigned int index;
  uint32_t     position;
  uint32_t     cMsgPos;

  struct LGMPHeaderQueue * hq;
};

struct LGMPHost
{
  uint8_t * mem;
  size_t    size;
  uint32_t  avail;
  uint32_t  nextFree;
  bool      started;
  uint32_t  sessionID;
  uint32_t  numQueues;
  uint8_t * udata;
  uint32_t  udataSize;

  struct LGMPHeader    * header;
  struct LGMPHostQueue   queues[LGMP_MAX_QUEUES];
};

static void initHeader(PLGMPHost host)
{
  host->header->magic     = LGMP_PROTOCOL_MAGIC;
  host->header->timestamp = lgmpGetClockMS();
  host->header->version   = LGMP_PROTOCOL_VERSION;
  host->header->numQueues = host->numQueues;
  host->header->udataSize = host->udataSize;
  memcpy(host->header->udata, host->udata, host->udataSize);
}

LGMP_STATUS lgmpHostInit(void *mem, const uint32_t size, PLGMPHost * result,
    uint32_t udataSize, uint8_t * udata)
{
  assert(mem);
  assert(size > 0);
  assert(result);

  *result = NULL;

  // make sure that lgmpGetClockMS works
  if (!lgmpGetClockMS())
    return LGMP_ERR_CLOCK_FAILURE;

  if (size < sizeof(struct LGMPHeader) + udataSize)
    return LGMP_ERR_INVALID_SIZE;

  *result = calloc(1, sizeof(**result));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPHost host = *result;
  host->mem      = mem;
  host->size     = size;
  host->avail    = size - ALIGN(sizeof(struct LGMPHeader) + udataSize);
  host->nextFree = ALIGN(sizeof(struct LGMPHeader) + udataSize);
  host->header   = (struct LGMPHeader *)mem;

  // take a copy of the user data so we can re-init the header if it gets wiped
  // out by a misbehaving process.
  host->udata = malloc(udataSize);
  if (!host->udata)
  {
    free(*result);
    *result = NULL;
    return LGMP_ERR_NO_MEM;
  }
  memcpy(host->udata, udata, udataSize);
  host->udataSize = udataSize;

  // ensure the sessionID changes so that clients can determine if the host was
  // restarted.
  const uint32_t sessionID = host->header->sessionID;
  host->sessionID = rand();
  while(sessionID == host->sessionID)
    host->sessionID = rand();
  host->header->sessionID = host->sessionID;

  initHeader(host);
  return LGMP_OK;
}

void lgmpHostFree(PLGMPHost * host)
{
  assert(host);
  if (!*host)
    return;

  free((*host)->udata);
  free(*host);
  *host = NULL;
}

LGMP_STATUS lgmpHostQueueNew(PLGMPHost host, const struct LGMPQueueConfig config,
    PLGMPHostQueue * result)
{
  assert(host);
  assert(result);

  *result = NULL;
  if (host->started)
    return LGMP_ERR_HOST_STARTED;

  if (host->numQueues == LGMP_MAX_QUEUES)
    return LGMP_ERR_NO_QUEUES;

  // + 1 for end marker
  uint32_t numMessages = config.numMessages + 1;

  const size_t needed = sizeof(struct LGMPHeaderMessage) * numMessages;
  if (host->avail < needed)
    return LGMP_ERR_NO_SHARED_MEM;

  *result = &host->queues[host->numQueues];
  PLGMPHostQueue queue = *result;

  struct LGMPHeaderQueue * hq = &host->header->queues[host->numQueues++];
  hq->queueID        = config.queueID;
  hq->numMessages    = numMessages;
  hq->newSubCount    = 0;
  LGMP_LOCK_INIT(hq->lock);
  hq->subs           = 0;
  hq->position       = 0;
  hq->messagesOffset = host->nextFree;
  hq->start          = 0;
  atomic_store(&hq->msgTimeout, 0);
  hq->maxTime        = config.subTimeout;
  hq->count          = 0;

  LGMP_LOCK_INIT(hq->cMsgLock);
  atomic_store(&hq->cMsgAvail  , LGMP_MSGS_MAX);
  atomic_store(&hq->cMsgWPos   , 0);
  atomic_store(&hq->cMsgWSerial, 0);
  atomic_store(&hq->cMsgRSerial, 0);

  queue->host       = host;
  queue->index      = host->numQueues;
  queue->position   = 0;
  queue->hq         = hq;

  host->avail    -= ALIGN(needed);
  host->nextFree += ALIGN(needed);

  ++host->header->numQueues;
  return LGMP_OK;
}

bool lgmpHostQueueHasSubs(PLGMPHostQueue queue)
{
  assert(queue);
  return LGMP_SUBS_ON(atomic_load(&queue->hq->subs)) != 0;
}

uint32_t lgmpHostQueueNewSubs(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_exchange(&queue->hq->newSubCount, 0);
}

uint32_t lgmpHostQueuePending(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_load(&queue->hq->count);
}

LGMP_STATUS lgmpHostProcess(PLGMPHost host)
{
  assert(host);

  // for an unkown reason sometimes when the guest starts the shared memory is
  // zeroed by something external after we have initialized it, detect this and
  // report it.
  if (host->header->magic != LGMP_PROTOCOL_MAGIC)
    return LGMP_ERR_CORRUPTED;

  const uint64_t now = lgmpGetClockMS();
  atomic_store(&host->header->timestamp, now);

  // each queue
  for(unsigned int i = 0; i < host->numQueues; ++i)
  {
    struct LGMPHostQueue     *queue    = &host->queues[i];
    struct LGMPHeaderQueue   *hq       = queue->hq;
    struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
      (host->mem + hq->messagesOffset);

    LGMP_QUEUE_LOCK(hq);
    if(!atomic_load(&queue->hq->count))
    {
      LGMP_QUEUE_UNLOCK(hq);
      continue;
    }

    uint64_t subs = atomic_load(&hq->subs);
    for(;;)
    {
      struct LGMPHeaderMessage *msg = &messages[hq->start];
      uint32_t pend = msg->pendingSubs & LGMP_SUBS_ON(subs);

      const uint32_t newBadSubs = pend & ~LGMP_SUBS_BAD(subs);
      if (newBadSubs && now > atomic_load(&hq->msgTimeout))
      {
        // reset garbage collection timeout for new bad subs
        subs = LGMP_SUBS_OR_BAD(subs, newBadSubs);
        const uint64_t timeout = now + hq->maxTime;
        for(unsigned int id = 0; id < 32; ++id)
          if (newBadSubs & (1U << id))
            hq->timeout[id] = timeout;

        // clear the pending subs
        msg->pendingSubs = 0;
        pend = 0;
      }

      // if there are still valid pending subs break out
      if (pend & ~LGMP_SUBS_BAD(subs))
        break;

      // message finished
      if (hq->start + 1 == hq->numMessages)
        hq->start = 0;
      else
        ++hq->start;

      // decrement the queue count and break out if there are no more messages
      if (atomic_fetch_sub(&queue->hq->count, 1) == 1)
        break;

      // update the timeout
      atomic_store(&hq->msgTimeout, now + hq->maxTime);
    }

    atomic_store(&hq->subs, subs);
    LGMP_QUEUE_UNLOCK(hq);
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
  return lgmpHostMemAllocAligned(host, size, 4, result);
}

LGMP_STATUS lgmpHostMemAllocAligned(PLGMPHost host, uint32_t size,
    uint32_t alignment, PLGMPMemory *result)
{
  assert(host);
  assert(result);

  uint32_t nextFree = host->nextFree;
  if (alignment > 0)
  {
    // alignment must be a power of two
    if ((alignment & (alignment - 1)) != 0)
      return LGMP_ERR_INVALID_ALIGNMENT;

    size     = (size     + (alignment - 1)) & ~(alignment - 1);
    nextFree = (nextFree + (alignment - 1)) & ~(alignment - 1);
  }

  if (size > host->avail - (nextFree - host->nextFree))
    return LGMP_ERR_NO_SHARED_MEM;

  *result = calloc(1, sizeof(**result));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPMemory mem = *result;
  mem->host   = host;
  mem->offset = nextFree;
  mem->size   = size;
  mem->mem    = host->mem + nextFree;

  host->avail   -= (nextFree - host->nextFree) + size;
  host->nextFree = nextFree + size;

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

LGMP_STATUS lgmpHostQueuePost(PLGMPHostQueue queue, uint32_t udata,
    PLGMPMemory payload)
{
  struct LGMPHeaderQueue *hq = queue->hq;

  LGMP_QUEUE_LOCK(hq);

  // get the subscribers
  const uint64_t subs = atomic_load(&hq->subs);
  const uint32_t pend = LGMP_SUBS_ON(subs) & ~(LGMP_SUBS_BAD(subs));

  // if nobody has subscribed there is no point in posting the message
  if (!pend)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_OK;
  }

  // we should never fully fill the buffer
  if (atomic_load(&queue->hq->count) == hq->numMessages - 1)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_FULL;
  }

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->host->mem + hq->messagesOffset);

  struct LGMPHeaderMessage *msg = &messages[queue->position];

  msg->udata       = udata;
  msg->size        = payload->size;
  msg->offset      = payload->offset;
  msg->pendingSubs = pend;

  // increment the queue count, if it were zero update the msgTimeout
  if (atomic_fetch_add(&hq->count, 1) == 0)
    atomic_store(&hq->msgTimeout, lgmpGetClockMS() + hq->maxTime);

  if (++queue->position == hq->numMessages)
    queue->position = 0;

  atomic_store(&hq->position, queue->position);

  LGMP_QUEUE_UNLOCK(hq);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostReadData(PLGMPHostQueue queue, void * data, size_t * size)
{
  struct LGMPHeaderQueue *hq = queue->hq;

  if (atomic_load(&hq->cMsgAvail) == LGMP_MSGS_MAX)
    return LGMP_ERR_QUEUE_EMPTY;

  // lock the client message buffer
  LGMP_LOCK(hq->cMsgLock);

  struct LGMPClientMessage * msg = &hq->cMsgs[queue->cMsgPos];
  if (++queue->cMsgPos == LGMP_MSGS_MAX)
    queue->cMsgPos = 0;

  memcpy(data, msg->data, msg->size);
  *size = msg->size;

  atomic_fetch_add(&hq->cMsgAvail, 1);
  LGMP_UNLOCK(hq->cMsgLock);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostAckData(PLGMPHostQueue queue)
{
  struct LGMPHeaderQueue *hq = queue->hq;
  atomic_fetch_add(&hq->cMsgRSerial, 1);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostGetClientIDs(PLGMPHostQueue queue, uint32_t clientIDs[32],
    unsigned int * count)
{
  assert(queue);
  assert(count);

  struct LGMPHeaderQueue *hq = queue->hq;

  LGMP_QUEUE_LOCK(hq);
  const uint64_t now = lgmpGetClockMS();

  uint64_t subs = atomic_load(&hq->subs);
  *count = 0;
  for(int i = 0; i < 32; ++i)
  {
    const uint32_t bit = 1U << i;
    if (!(LGMP_SUBS_ON(subs) & bit) ||
        ((LGMP_SUBS_BAD(subs) & bit) && now > hq->timeout[i]))
      continue;

    clientIDs[(*count)++] = hq->clientID[i];
  }
  LGMP_QUEUE_UNLOCK(hq);
  return LGMP_OK;
}
