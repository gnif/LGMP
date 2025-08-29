/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2025 Geoffrey McRae <geoff@hostfission.com>
 * https://github.com/gnif/LGMP
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

#include "lgmp/host.h"

#include "lgmp.h"
#include "headers.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define ALIGN(x) ((x + (3)) & ~(3))
#define ALIGN_TO(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define CACHELINE 64u

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
  uint64_t  lastTimestamp;

  struct LGMPHeader    * header;
  struct LGMPHostQueue   queues[LGMP_MAX_QUEUES];
};

static void initHeader(PLGMPHost host)
{
  host->header->timestamp = lgmpGetClockMS();
  host->header->version   = LGMP_PROTOCOL_VERSION;
  host->header->numQueues = host->numQueues;
  host->header->udataSize = host->udataSize;
  memcpy(host->header->udata, host->udata, host->udataSize);

  // this must be set last to ensure a client doesn't read invalid data before
  // we're ready
  host->header->magic = LGMP_PROTOCOL_MAGIC;
  host->lastTimestamp = host->header->timestamp;
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

  /* Require power-of-two ring size (no sentinel needed when using count) */
  if (!LGMP_IS_POW2(config.numMessages) || config.numMessages < 2)
    return LGMP_ERR_INVALID_ARGUMENT;
  uint32_t numMessages = config.numMessages;

  const uint32_t msgBytes     = sizeof(struct LGMPHeaderMessage) * numMessages;
  const uint32_t startAligned = ALIGN_TO(host->nextFree, CACHELINE);
  const uint32_t pad          = startAligned - host->nextFree;
  const uint32_t needed       = pad + ALIGN_TO(msgBytes, CACHELINE);
  if (host->avail < needed)
    return LGMP_ERR_NO_SHARED_MEM;

  const unsigned idx = host->numQueues++;
  *result = &host->queues[idx];
  PLGMPHostQueue queue = *result;

  struct LGMPHeaderQueue * hq = &host->header->queues[idx];
  hq->queueID        = config.queueID;
  hq->numMessages    = numMessages;

  /* message ring placement, 64B aligned */
  hq->messagesOffset = startAligned;
  host->nextFree     = startAligned;
  host->avail       -= pad;

  atomic_store(&hq->newSubCount, 0);
  LGMP_LOCK_INIT(hq->lock);
  atomic_store(&hq->subs, 0);
  hq->position       = 0;
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
  queue->index      = idx;
  queue->position   = 0;
  queue->hq         = hq;

  /* consume the aligned region */
  host->avail    -= ALIGN_TO(msgBytes, CACHELINE);
  host->nextFree += ALIGN_TO(msgBytes, CACHELINE);

  ++host->header->numQueues;
  return LGMP_OK;
}

bool lgmpHostQueueHasSubs(PLGMPHostQueue queue)
{
  assert(queue);
  return LGMP_SUBS_ON(atomic_load_explicit(&queue->hq->subs,
        memory_order_relaxed)) != 0;
}

uint32_t lgmpHostQueueNewSubs(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_exchange_explicit(&queue->hq->newSubCount, 0,
      memory_order_relaxed);
}

uint32_t lgmpHostQueuePending(PLGMPHostQueue queue)
{
  assert(queue);
  return atomic_load_explicit(&queue->hq->count, memory_order_relaxed);
}

LGMP_STATUS lgmpHostProcess(PLGMPHost host)
{
  assert(host);

  // for an unkown reason sometimes when the guest starts the shared memory is
  // zeroed by something external after we have initialized it, detect this and
  // report it.
  if (unlikely(host->header->magic != LGMP_PROTOCOL_MAGIC))
    return LGMP_ERR_CORRUPTED;

  const uint64_t now = lgmpGetClockMS();
  if (unlikely(now - host->lastTimestamp >= 250))
  {
    atomic_store_explicit(&host->header->timestamp, now, memory_order_release);
    host->lastTimestamp = now;
  }

  // each queue
  for(unsigned int i = 0; i < host->numQueues; ++i)
  {
    struct LGMPHostQueue     *queue    = &host->queues[i];
    struct LGMPHeaderQueue   *hq       = queue->hq;
    struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
      (host->mem + hq->messagesOffset);

    if (atomic_load_explicit(&hq->count, memory_order_acquire) == 0)
      continue;

    LGMP_QUEUE_LOCK(hq);
    if (unlikely(atomic_load_explicit(&hq->count, memory_order_relaxed) == 0))
    {
      LGMP_QUEUE_UNLOCK(hq);
      continue;
    }

    uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
    const uint32_t mask = hq->numMessages - 1;
    for(;;)
    {
      const uint32_t next = (hq->start + 1) & mask;
      LGMP_PREFETCH_R(&messages[hq->start], 2);
      LGMP_PREFETCH_R(&messages[next],      2);
#if (LGMP_PREFETCH_DIST >= 2)
      uint32_t n2 = (next + 1) & mask;
      LGMP_PREFETCH_R(&messages[n2], 1);
#endif

      struct LGMPHeaderMessage *msg = &messages[hq->start];
      uint32_t pend = atomic_load_explicit(&msg->pendingSubs,
          memory_order_acquire) & LGMP_SUBS_ON(subs);

      const uint32_t newBadSubs = pend & ~((uint32_t)LGMP_SUBS_BAD(subs));
      if (unlikely(newBadSubs && now > atomic_load_explicit(&hq->msgTimeout,
            memory_order_relaxed)))
      {
        // reset garbage collection timeout for new bad subs
        subs = LGMP_SUBS_OR_BAD(subs, newBadSubs);
        const uint64_t timeout = now + hq->maxTime;
        for(unsigned int id = 0; id < LGMP_MAX_CLIENTS; ++id)
          if (newBadSubs & (1U << id))
            hq->timeout[id] = timeout;

        // clear the pending subs
        atomic_store_explicit(&msg->pendingSubs, 0, memory_order_release);
        pend = 0;
      }

      // if there are still valid pending subs break out
      if (pend & ~LGMP_SUBS_BAD(subs))
        break;

      // message finished
      hq->start = (hq->start + 1) & mask;

      // decrement the queue count and break out if there are no more messages
      if (atomic_fetch_sub_explicit(&queue->hq->count, 1,
            memory_order_relaxed) == 1)
        break;

      // update the timeout
      atomic_store_explicit(&hq->msgTimeout, now + hq->maxTime,
          memory_order_relaxed);
    }

    atomic_store_explicit(&hq->subs, subs, memory_order_relaxed);
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

  // get the subscribers
  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
  uint32_t pend = LGMP_SUBS_ON(subs) & ~(LGMP_SUBS_BAD(subs));

  // if nobody has subscribed there is no point in posting the message
  if (!pend)
    return LGMP_OK;

  LGMP_QUEUE_LOCK(hq);
  subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);
  pend = LGMP_SUBS_ON(subs) & ~((uint32_t)LGMP_SUBS_BAD(subs));
  if (unlikely(!pend))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_OK;
  }

  // full when count == numMessages
  if (unlikely(atomic_load_explicit(&hq->count,
        memory_order_relaxed) == hq->numMessages))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_FULL;
  }

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->host->mem + hq->messagesOffset);

  const uint32_t mask = hq->numMessages - 1;
  LGMP_PREFETCH_W(&messages[queue->position], 3);
  uint32_t npos = (queue->position + 1) & mask;
  LGMP_PREFETCH_W(&messages[npos], 2);

  struct LGMPHeaderMessage *msg = &messages[queue->position];

  msg->udata       = udata;
  msg->size        = payload->size;
  msg->offset      = payload->offset;
  atomic_store_explicit(&msg->pendingSubs, pend, memory_order_release);

  // increment the queue count, if it were zero update the msgTimeout
  if (atomic_fetch_add_explicit(&hq->count, 1, memory_order_release) == 0)
    atomic_store_explicit(&hq->msgTimeout, lgmpGetClockMS() + hq->maxTime,
        memory_order_relaxed);

  queue->position = (queue->position + 1) & mask;

  atomic_store_explicit(&hq->position, queue->position, memory_order_release);

  LGMP_QUEUE_UNLOCK(hq);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostReadData(PLGMPHostQueue queue, void * restrict data,
    size_t * restrict size)
{
  struct LGMPHeaderQueue *hq = queue->hq;

  if (atomic_load_explicit(&hq->cMsgAvail,
        memory_order_acquire) == LGMP_MSGS_MAX)
    return LGMP_ERR_QUEUE_EMPTY;

  // lock the client message buffer
  LGMP_LOCK(hq->cMsgLock);

  struct LGMPClientMessage * msg = &hq->cMsgs[queue->cMsgPos];
  queue->cMsgPos = (queue->cMsgPos + 1) & (LGMP_MSGS_MAX - 1);

  LGMP_PREFETCH_R(&hq->cMsgs[queue->cMsgPos], 2);
#if (LGMP_PREFETCH_DIST >= 2)
  uint32_t n2 = (queue->cMsgPos + 1) & (LGMP_MSGS_MAX - 1);
  LGMP_PREFETCH_R(&hq->cMsgs[n2], 1);
#endif

  memcpy(data, msg->data, msg->size);
  *size = msg->size;

  atomic_fetch_add_explicit(&hq->cMsgAvail, 1, memory_order_release);
  LGMP_UNLOCK(hq->cMsgLock);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostAckData(PLGMPHostQueue queue)
{
  struct LGMPHeaderQueue *hq = queue->hq;
  atomic_fetch_add_explicit(&hq->cMsgRSerial, 1, memory_order_relaxed);
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
  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);

  *count = 0;
  for(int i = 0; i < LGMP_MAX_CLIENTS; ++i)
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
