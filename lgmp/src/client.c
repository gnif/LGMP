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

#include "lgmp/client.h"

#include "lgmp.h"
#include "headers.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define LGMP_HEARTBEAT_TIMEOUT 1000
#define LGMP_MESSAGE_ALIGNMENT 16u

/* Old LGMP header layout where magic & version were at offset 0.
 * Used for detecting old hosts after the struct was restructured. */
struct LGMPHeader_Old
{
  uint32_t magic;
  uint32_t version;
};

struct LGMPClientQueue
{
  PLGMPClient   client;
  unsigned int  id;
  unsigned int  index;
  uint32_t      position;
  uint32_t      queueID;
  uint32_t      numMessages;
  uint32_t      messageMask;

  struct LGMPHeader      * header;
  struct LGMPHeaderQueue * hq;
  struct LGMPHeaderMessage * messages;
};

struct LGMPClient
{
  uint8_t           * mem;
  size_t              size;
  struct LGMPHeader * header;

  uint32_t id;
  uint32_t sessionID;
  _Atomic(uint32_t) heartbeatLock;
  uint64_t          hosttime;
  uint64_t          lastHeartbeat;
  size_t            firstMessageOffset;
  uint32_t          numQueues;

  struct LGMPClientQueue queues[LGMP_MAX_QUEUES];
};

struct LGMPClientQueueState
{
  uint32_t position;
  uint32_t start;
  uint32_t count;
};

struct LGMPClientMessageState
{
  uint32_t avail;
  uint32_t wpos;
};

static bool clientSessionMatches(PLGMPClient client, uint32_t sessionID)
{
  struct LGMPHeader * header = client->header;
  if (lgmpSharedObserve32(&header->sessionID) != sessionID ||
      lgmpSharedObserve32(&header->magic) != LGMP_PROTOCOL_MAGIC)
    return false;

  return lgmpSharedObserve32(&header->sessionID) == sessionID;
}

static bool cacheClientQueue(PLGMPClient client, uint32_t index,
    size_t firstMessageOffset, PLGMPClientQueue queue)
{
  struct LGMPHeaderQueue * hq = &client->header->queues[index];
  const uint32_t numMessages = hq->numMessages;
  const size_t messagesOffset = hq->messagesOffset;

  if (numMessages < 2 || !LGMP_IS_POW2(numMessages) ||
      messagesOffset < firstMessageOffset ||
      (messagesOffset & (LGMP_MESSAGE_ALIGNMENT - 1u)) != 0 ||
      messagesOffset > client->size ||
      numMessages >
        (client->size - messagesOffset) / sizeof(struct LGMPHeaderMessage))
    return false;

  queue->client      = client;
  queue->index       = index;
  queue->queueID     = hq->queueID;
  queue->numMessages = numMessages;
  queue->messageMask = numMessages - 1;
  queue->header      = client->header;
  queue->hq          = hq;
  queue->messages    = (struct LGMPHeaderMessage *)
    (client->mem + messagesOffset);
  return true;
}

static LGMP_STATUS refreshClientQueuesLocked(PLGMPClient client)
{
  struct LGMPHeader * header = client->header;
  if (unlikely(!clientSessionMatches(client, client->sessionID)))
    return LGMP_ERR_INVALID_SESSION;

  const uint32_t numQueues = atomic_load_explicit(&header->numQueues,
      memory_order_acquire);
  lgmpSharedReadFence();
  const uint32_t sessionUdataSize = header->udataSize;
  if (numQueues > LGMP_MAX_QUEUES || numQueues < client->numQueues ||
      sessionUdataSize > client->size - LGMP_HEADER_DATA_OFFSET ||
      LGMP_HEADER_DATA_OFFSET + sessionUdataSize !=
        client->firstMessageOffset)
    return clientSessionMatches(client, client->sessionID) ?
      LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  struct LGMPClientQueue queues[LGMP_MAX_QUEUES] = { 0 };
  for(uint32_t i = client->numQueues; i < numQueues; ++i)
    if (!cacheClientQueue(
          client, i, client->firstMessageOffset, &queues[i]))
      return clientSessionMatches(client, client->sessionID) ?
        LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  if (unlikely(!clientSessionMatches(client, client->sessionID)))
    return LGMP_ERR_INVALID_SESSION;
  const uint32_t currentNumQueues = atomic_load_explicit(&header->numQueues,
      memory_order_acquire);
  lgmpSharedReadFence();
  if (unlikely(currentNumQueues < numQueues ||
        currentNumQueues > LGMP_MAX_QUEUES ||
        sessionUdataSize != header->udataSize))
    return clientSessionMatches(client, client->sessionID) ?
      LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  for(uint32_t i = client->numQueues; i < numQueues; ++i)
    client->queues[i] = queues[i];
  client->numQueues = numQueues;
  return LGMP_OK;
}

static bool snapshotClientQueueState(PLGMPClientQueue queue,
    struct LGMPClientQueueState * state)
{
  struct LGMPHeaderQueue * hq = queue->hq;
  state->position = atomic_load_explicit(&hq->position,
      memory_order_relaxed);
  state->start = hq->start;
  state->count = atomic_load_explicit(&hq->count, memory_order_relaxed);

  return state->position < queue->numMessages &&
    state->start < queue->numMessages && state->count <= queue->numMessages;
}

static bool snapshotClientMessageState(PLGMPClientQueue queue,
    struct LGMPClientMessageState * state)
{
  struct LGMPHeaderQueue * hq = queue->hq;
  state->avail = atomic_load_explicit(&hq->cMsgAvail, memory_order_relaxed);
  state->wpos  = atomic_load_explicit(&hq->cMsgWPos , memory_order_relaxed);
  return state->avail <= LGMP_MSGS_MAX && state->wpos < LGMP_MSGS_MAX;
}

LGMP_STATUS lgmpClientGetMemoryContext(PLGMPClient client, uint8_t ** mem,
    size_t * size, uint32_t ** sessionID, uint32_t * clientID)
{
  assert(client);
  assert(mem);
  assert(size);
  assert(sessionID);
  assert(clientID);

  if (!client->sessionID ||
      unlikely(client->sessionID != client->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  *mem       = client->mem;
  *size      = client->size;
  *sessionID = &client->header->sessionID;
  *clientID  = client->id;
  return LGMP_OK;
}

static void clearSubscriberMessages(PLGMPClientQueue queue, uint32_t bit)
{
  for(uint32_t i = 0; i < queue->numMessages; ++i)
    atomic_fetch_and_explicit(&queue->messages[i].pendingSubs, ~bit,
        memory_order_relaxed);
}

static bool snapshotClientPayload(PLGMPClientQueue queue,
    const struct LGMPHeaderMessage * message, PLGMPMessage result)
{
  const uint64_t udata = message->udata;
  const uint32_t size = message->size;
  const size_t offset = message->offset;

  if (offset > queue->client->size ||
      size > queue->client->size - offset)
    return false;

  result->udata = udata;
  result->size  = size;
  result->mem   = queue->client->mem + offset;
  return true;
}

LGMP_STATUS lgmpClientInit(void * mem, const size_t size, PLGMPClient * result)
{
  assert(mem);
  assert(size > 0);
  assert(result);

  *result = NULL;
  if (size < sizeof(struct LGMPHeader))
    return LGMP_ERR_INVALID_SIZE;

  // make sure that lgmpGetClockMS works
  if (!lgmpGetClockMS())
    return LGMP_ERR_CLOCK_FAILURE;

  struct LGMPHeader *header = (struct LGMPHeader*)mem;

  *result = calloc(1, sizeof(**result));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPClient client = *result;
  client->mem           = (uint8_t*)mem;
  client->size          = size;
  client->header        = header;
  LGMP_LOCK_INIT(client->heartbeatLock);
  client->hosttime = atomic_load_explicit(&header->timestamp,
      memory_order_relaxed);
  return LGMP_OK;
}

void lgmpClientFree(PLGMPClient * client)
{
  assert(client);
  if (!*client)
    return;

  free(*client);
  *client = NULL;
}

LGMP_STATUS lgmpClientSessionInit(PLGMPClient client, uint32_t * udataSize,
    uint8_t ** udata, uint32_t * clientID, uint32_t * remoteVersion)
{
  assert(client);
  struct LGMPHeader * header = client->header;

  if (remoteVersion)
    *remoteVersion = 0;

  const uint32_t magic = lgmpSharedObserve32(&header->magic);
  if (magic != LGMP_PROTOCOL_MAGIC)
  {
    /* magic not at the new layout position — try the old layout where
     * magic & version were at offset 0 */
    struct LGMPHeader_Old * old = (struct LGMPHeader_Old *)header;
    if (old->magic != LGMP_PROTOCOL_MAGIC)
      return LGMP_ERR_INVALID_MAGIC;

    /* magic found at the old position → host is old, version mismatch */
    if (remoteVersion)
      *remoteVersion = old->version;
    return LGMP_ERR_INVALID_VERSION;
  }

  const uint32_t sessionID = lgmpSharedObserve32(&header->sessionID);
  if (!sessionID)
    return LGMP_ERR_INVALID_SESSION;

  if (header->version != LGMP_PROTOCOL_VERSION)
  {
    if (remoteVersion)
      *remoteVersion = header->version;
    return LGMP_ERR_INVALID_VERSION;
  }

  const uint32_t numQueues = atomic_load_explicit(&header->numQueues,
      memory_order_acquire);
  lgmpSharedReadFence();
  const uint32_t sessionUdataSize = header->udataSize;
  if (numQueues > LGMP_MAX_QUEUES)
    return clientSessionMatches(client, sessionID) ?
      LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  if (sessionUdataSize > client->size - LGMP_HEADER_DATA_OFFSET)
    return clientSessionMatches(client, sessionID) ?
      LGMP_ERR_INVALID_SIZE : LGMP_ERR_INVALID_SESSION;

  const size_t firstMessageOffset =
    LGMP_HEADER_DATA_OFFSET + sessionUdataSize;

  uint64_t timestamp = atomic_load_explicit(&header->timestamp,
      memory_order_relaxed);
#ifndef LGMP_REALACY
  LGMP_LOCK(client->heartbeatLock);
  const uint64_t initialHosttime = client->hosttime;
  LGMP_UNLOCK(client->heartbeatLock);

  // check the host's timestamp is updating
  const uint64_t end = lgmpGetClockMS() + 500;
  bool valid = false;
  do
  {
    if (timestamp != initialHosttime)
    {
      valid = true;
      break;
    }
    timestamp = atomic_load_explicit(&header->timestamp, memory_order_relaxed);
    lgmpSleepMs(1);
  }
  while(lgmpGetClockMS() < end);

  if (!valid)
    return LGMP_ERR_INVALID_SESSION;
#endif

  if (unlikely(!clientSessionMatches(client, sessionID)))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPClientQueue queues[LGMP_MAX_QUEUES] = { 0 };
  for(uint32_t i = 0; i < numQueues; ++i)
    if (!cacheClientQueue(client, i, firstMessageOffset, &queues[i]))
      return clientSessionMatches(client, sessionID) ?
        LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  if (unlikely(!clientSessionMatches(client, sessionID)))
    return LGMP_ERR_INVALID_SESSION;
  const uint32_t currentNumQueues = atomic_load_explicit(&header->numQueues,
      memory_order_acquire);
  lgmpSharedReadFence();
  if (unlikely(currentNumQueues < numQueues ||
        currentNumQueues > LGMP_MAX_QUEUES ||
        sessionUdataSize != header->udataSize))
    return clientSessionMatches(client, sessionID) ?
      LGMP_ERR_CORRUPTED : LGMP_ERR_INVALID_SESSION;

  do
  {
    client->id = atomic_fetch_add_explicit(&header->nextClientID, 1,
        memory_order_relaxed) + 1;
  }
  while(client->id == 0);

  if (unlikely(!clientSessionMatches(client, sessionID)))
    return LGMP_ERR_INVALID_SESSION;

  LGMP_LOCK(client->heartbeatLock);
  memcpy(client->queues, queues, sizeof(client->queues));
  client->sessionID          = sessionID;
  client->hosttime           = timestamp;
  client->lastHeartbeat      = lgmpGetClockMS();
  client->firstMessageOffset = firstMessageOffset;
  client->numQueues          = numQueues;
  LGMP_UNLOCK(client->heartbeatLock);

  if (udataSize) *udataSize = sessionUdataSize;
  if (udata    ) *udata     = (uint8_t*)&header->udata;
  if (clientID ) *clientID  = client->id;

  return LGMP_OK;
}

bool lgmpClientSessionValid(PLGMPClient client)
{
  assert(client);

  LGMP_LOCK(client->heartbeatLock);

  // check if the host has been restarted
  if (unlikely(client->sessionID != client->header->sessionID))
  {
    LGMP_UNLOCK(client->heartbeatLock);
    return false;
  }

#ifndef LGMP_REALACY

  // check if the heartbeat changed
  const uint64_t hosttime = atomic_load_explicit(&client->header->timestamp,
      memory_order_relaxed);
  const uint64_t now = lgmpGetClockMS();
  if (likely(client->hosttime != hosttime))
  {
    client->lastHeartbeat = now;
    client->hosttime      = hosttime;
    LGMP_UNLOCK(client->heartbeatLock);
    return true;
  }

  // check if the heartbeat timeout has been exceeded
  if (unlikely(now - client->lastHeartbeat > LGMP_HEARTBEAT_TIMEOUT))
  {
    LGMP_UNLOCK(client->heartbeatLock);
    return false;
  }

#endif

  LGMP_UNLOCK(client->heartbeatLock);
  return true;
}

static bool lockClientShared(PLGMPClient client,
    _Atomic(uint32_t) * lock)
{
  unsigned int spins = 0;
  bool contended = false;

  /* The host can terminate while owning a shared lock. Keep checking its
   * heartbeat so an abandoned lock cannot block client teardown forever. */
  if (unlikely(!lgmpClientSessionValid(client)))
    return false;

  while (!LGMP_TRY_LOCK(*lock))
  {
    contended = true;
    if ((++spins & 0xFF) == 0)
    {
      if (unlikely(!lgmpClientSessionValid(client)))
        return false;

      lgmpSleepMs(0);
    }
  }

  if (unlikely(contended && !lgmpClientSessionValid(client)))
  {
    LGMP_UNLOCK(*lock);
    return false;
  }

  return true;
}

static bool lockClientQueue(PLGMPClient client,
    struct LGMPHeaderQueue * hq)
{
  return lockClientShared(client, &hq->lock);
}

LGMP_STATUS lgmpClientSubscribe(PLGMPClient client, uint32_t queueID,
    PLGMPClientQueue * result)
{
  assert(client);
  assert(result);

  *result = NULL;

  PLGMPClientQueue q = NULL;
  LGMP_LOCK(client->heartbeatLock);
  const LGMP_STATUS refresh = refreshClientQueuesLocked(client);
  if (refresh != LGMP_OK)
  {
    LGMP_UNLOCK(client->heartbeatLock);
    return refresh;
  }

  uint32_t queueIndex;
  for(queueIndex = 0; queueIndex < client->numQueues; ++queueIndex)
    if (client->queues[queueIndex].queueID == queueID)
    {
      q = &client->queues[queueIndex];
      break;
    }
  LGMP_UNLOCK(client->heartbeatLock);

  if (!q)
    return LGMP_ERR_NO_SUCH_QUEUE;

  struct LGMPHeaderQueue * hq = q->hq;

  // take the queue lock
  if (!lockClientQueue(client, hq))
    return LGMP_ERR_INVALID_SESSION;

  if (unlikely(client->sessionID != client->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  struct LGMPClientQueueState queueState;
  struct LGMPClientMessageState messageState;
  if (unlikely(!snapshotClientQueueState(q, &queueState) ||
        !snapshotClientMessageState(q, &messageState)))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_CORRUPTED;
  }

  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);

  // recover subs for reuse that have been flagged as bad and have exceeded the
  // queue timeout
  if (LGMP_SUBS_ON(subs))
  {
    const uint64_t hosttime = atomic_load_explicit(&client->header->timestamp,
        memory_order_relaxed);
    uint32_t reap = 0u;
    for(unsigned int id = 0; id < LGMP_MAX_CLIENTS; ++id)
    {
      uint32_t bit = (1U << id);
      if ((LGMP_SUBS_BAD(subs) & bit) && hosttime > hq->timeout[id])
      {
        reap |= bit;
        hq->timeout [id] = 0;
        hq->clientID[id] = 0;
      }
    }
    subs = LGMP_SUBS_CLEAR(subs, reap);
  }

  // find the next free queue ID
  unsigned int id = 0;
  while (id < LGMP_MAX_CLIENTS &&
      ((LGMP_SUBS_ON(subs) | LGMP_SUBS_BAD(subs)) & (1U << id)))
    ++id;

  // check if full
  if (id == LGMP_MAX_CLIENTS)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_FULL; //TODO: better return error
  }

  const uint32_t bit = 1U << id;
  clearSubscriberMessages(q, bit);
  hq->timeout [id] = 0;
  hq->clientID[id] = client->id;
  subs = LGMP_SUBS_SET(subs, bit);
  atomic_store_explicit(&hq->subs, subs, memory_order_release);
  atomic_fetch_add_explicit(&hq->newSubCount, 1, memory_order_relaxed);

  q->id       = id;
  q->position = queueState.position;

  *result = q;
  LGMP_QUEUE_UNLOCK(hq);
  return LGMP_OK;
}

LGMP_STATUS lgmpClientUnsubscribe(PLGMPClientQueue * result)
{
  assert(result);

  if (!*result)
    return LGMP_OK;

  PLGMPClientQueue queue = *result;
  assert(queue->client);

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;

  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
  if ((LGMP_SUBS_BAD(subs) & bit) ||
      hq->clientID[queue->id] != queue->client->id)
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (!lockClientQueue(queue->client, hq))
    return LGMP_ERR_INVALID_SESSION;

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  struct LGMPClientQueueState queueState;
  struct LGMPClientMessageState messageState;
  if (unlikely(!snapshotClientQueueState(queue, &queueState) ||
        !snapshotClientMessageState(queue, &messageState)))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_CORRUPTED;
  }

  subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);
  if ((LGMP_SUBS_BAD(subs) & bit) ||
      hq->clientID[queue->id] != queue->client->id)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  // unset the queue id bit
  clearSubscriberMessages(queue, bit);
  subs = LGMP_SUBS_CLEAR(subs, bit);
  atomic_store_explicit(&hq->subs, subs, memory_order_release);
  hq->timeout [queue->id] = 0;
  hq->clientID[queue->id] = 0;
  LGMP_QUEUE_UNLOCK(hq);

  queue->id       = 0;
  queue->position = 0;
  *result = NULL;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientAdvanceToLast(PLGMPClientQueue queue)
{
  assert(queue);

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint32_t mask = queue->messageMask;
  struct LGMPHeaderMessage * messages = queue->messages;
  if (!lockClientQueue(queue->client, hq))
    return LGMP_ERR_INVALID_SESSION;

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  const uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);
  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
  {
    LGMP_QUEUE_UNLOCK(hq);
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  struct LGMPClientQueueState state;
  if (unlikely(!snapshotClientQueueState(queue, &state)))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_CORRUPTED;
  }

  uint32_t pos = state.start;
  uint32_t last = 0;
  bool found = false;

  for(uint32_t i = 0; i < state.count; ++i)
  {
    if (atomic_load_explicit(&messages[pos].pendingSubs,
          memory_order_relaxed) & bit)
    {
      last = pos;
      found = true;
    }
    pos = (pos + 1) & mask;
  }

  if (!found)
  {
    queue->position = state.position;
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_EMPTY;
  }

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  pos = state.start;
  while(pos != last)
  {
    atomic_fetch_and_explicit(&messages[pos].pendingSubs, ~bit,
        memory_order_relaxed);
    pos = (pos + 1) & mask;
  }

  bool cleaned = false;
  const uint32_t active =
    LGMP_SUBS_ON(subs) & ~((uint32_t)LGMP_SUBS_BAD(subs));
  uint32_t remaining = state.count;
  uint32_t start = state.start;
  while(remaining)
  {
    struct LGMPHeaderMessage *msg = &messages[start];
    if (atomic_load_explicit(&msg->pendingSubs,
          memory_order_relaxed) & active)
      break;

    atomic_store_explicit(&msg->pendingSubs, 0, memory_order_relaxed);
    start = (start + 1) & mask;
    hq->start = start;
    const uint32_t oldCount = atomic_fetch_sub_explicit(&hq->count, 1,
        memory_order_relaxed);
    if (unlikely(oldCount != remaining))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_CORRUPTED;
    }
    --remaining;
    cleaned = true;
  }

  if (cleaned)
  {
    const uint64_t timeout =
      atomic_load_explicit(&queue->header->timestamp,
          memory_order_relaxed) + hq->maxTime;
    if (timeout > atomic_load_explicit(&hq->msgTimeout,
          memory_order_relaxed))
      atomic_store_explicit(&hq->msgTimeout, timeout, memory_order_relaxed);
  }

  queue->position = last;
  LGMP_QUEUE_UNLOCK(hq);
  return LGMP_OK;
}

LGMP_STATUS lgmpClientProcess(PLGMPClientQueue queue, PLGMPMessage result)
{
  assert(queue);
  assert(result);

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);

  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
  {
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  if (unlikely(queue->position >= queue->numMessages))
    return LGMP_ERR_CORRUPTED;

  struct LGMPHeaderMessage * messages = queue->messages;
  struct LGMPHeaderMessage * msg = &messages[queue->position];

  if (!(atomic_load_explicit(&msg->pendingSubs,
          memory_order_acquire) & bit))
  {
    if (!lockClientQueue(queue->client, hq))
      return LGMP_ERR_INVALID_SESSION;

    if (unlikely(queue->client->sessionID != queue->header->sessionID))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_INVALID_SESSION;
    }

    subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);
    if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
          hq->clientID[queue->id] != queue->client->id))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_QUEUE_TIMEOUT;
    }

    if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    }

    struct LGMPClientQueueState state;
    if (unlikely(!snapshotClientQueueState(queue, &state)))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_CORRUPTED;
    }

    uint32_t pos = state.start;
    msg = NULL;

    for(uint32_t i = 0; i < state.count; ++i)
    {
      if (atomic_load_explicit(&messages[pos].pendingSubs,
            memory_order_relaxed) & bit)
      {
        queue->position = pos;
        msg = &messages[pos];
        break;
      }
      pos = (pos + 1) & queue->messageMask;
    }

    if (!msg)
    {
      queue->position = state.position;
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_QUEUE_EMPTY;
    }

    if (unlikely(!snapshotClientPayload(queue, msg, result)))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_CORRUPTED;
    }

    if (unlikely(queue->client->sessionID != queue->header->sessionID))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_INVALID_SESSION;
    }

    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_OK;
  }

  LGMP_PREFETCH_R(msg, 3);
  uint32_t npos = (queue->position + 1) & queue->messageMask;
  LGMP_PREFETCH_R(&messages[npos], 2);

  if (unlikely(!snapshotClientPayload(queue, msg, result)))
    return LGMP_ERR_CORRUPTED;

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientMessageDone(PLGMPClientQueue queue)
{
  assert(queue);

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;

  if (!lockClientQueue(queue->client, hq))
    return LGMP_ERR_INVALID_SESSION;

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  const uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_relaxed);

  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
  {
    LGMP_QUEUE_UNLOCK(hq);
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  struct LGMPClientQueueState state;
  if (unlikely(!snapshotClientQueueState(queue, &state) ||
        queue->position >= queue->numMessages))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_CORRUPTED;
  }

  struct LGMPHeaderMessage * messages = queue->messages;
  const uint32_t mask = queue->messageMask;
  uint32_t pos = state.start;
  bool active = false;

  for(uint32_t i = 0; i < state.count; ++i)
  {
    if (pos == queue->position)
    {
      active = true;
      break;
    }
    pos = (pos + 1) & mask;
  }

  if (!active)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_EMPTY;
  }

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  struct LGMPHeaderMessage *msg = &messages[queue->position];

  // The queue lock keeps this subscriber slot from being reassigned between
  // validating its identity and clearing its pending bit.
  const uint32_t pending = atomic_fetch_and_explicit(&msg->pendingSubs, ~bit,
      memory_order_acq_rel);
  if (!(pending & bit))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_EMPTY;
  }

  if ((pending & ~bit) == 0)
  {
    // someone else may have done this before we got the lock, so check
    if (state.start != queue->position)
      goto done;

    // message finished
    hq->start = (state.start + 1) & mask;

    // decrement the count and update the timeout
    uint32_t oldCount = atomic_fetch_sub_explicit(&hq->count, 1,
        memory_order_acquire);

    // check for a concurrent corruption or underflow
    if (unlikely(oldCount != state.count))
    {
      if (oldCount == 0)
        atomic_store_explicit(&hq->count, 0, memory_order_release);
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_CORRUPTED;
    }

    // update the timeout if we need to. We hold the lock so there is no need to
    // use a comapre exchange.
    uint64_t oldTimeout = atomic_load_explicit(&hq->msgTimeout,
        memory_order_relaxed);
    uint64_t newTimeout = atomic_load_explicit(&queue->header->timestamp,
        memory_order_relaxed) + hq->maxTime;

    if (newTimeout > oldTimeout)
      atomic_store_explicit(&hq->msgTimeout, newTimeout, memory_order_relaxed);
  }

done:
  LGMP_QUEUE_UNLOCK(hq);
  queue->position = (queue->position + 1) & mask;
  LGMP_PREFETCH_R(&messages[queue->position], 2);

  return LGMP_OK;
}

static LGMP_STATUS lockClientMessageQueue(PLGMPClientQueue queue, bool wait)
{
  struct LGMPHeaderQueue * hq = queue->hq;

  if (wait)
  {
    if (!lockClientQueue(queue->client, hq))
      return LGMP_ERR_INVALID_SESSION;

    if (!lockClientShared(queue->client, &hq->cMsgLock))
    {
      LGMP_QUEUE_UNLOCK(hq);
      return LGMP_ERR_INVALID_SESSION;
    }

    return LGMP_OK;
  }

  if (unlikely(!lgmpClientSessionValid(queue->client)))
    return LGMP_ERR_INVALID_SESSION;

  if (!LGMP_QUEUE_TRY_LOCK(hq))
    return LGMP_ERR_QUEUE_BUSY;

  if (!LGMP_TRY_LOCK(hq->cMsgLock))
  {
    LGMP_QUEUE_UNLOCK(hq);
    return lgmpClientSessionValid(queue->client) ?
      LGMP_ERR_QUEUE_BUSY : LGMP_ERR_INVALID_SESSION;
  }

  if (unlikely(!lgmpClientSessionValid(queue->client)))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  return LGMP_OK;
}

static LGMP_STATUS clientSendData(PLGMPClientQueue queue,
    const void * restrict data, uint32_t size, uint32_t * serial, bool wait)
{
  if (unlikely(size > LGMP_MSGS_SIZE))
    return LGMP_ERR_INVALID_SIZE;

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);

  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  // if there is no room, just return
  const uint32_t initialAvail =
    atomic_load_explicit(&hq->cMsgAvail, memory_order_acquire);
  if (unlikely(initialAvail > LGMP_MSGS_MAX))
    return LGMP_ERR_CORRUPTED;
  if (unlikely(initialAvail == 0))
    return LGMP_ERR_QUEUE_FULL;

  // lock the subscription and client message buffer
  const LGMP_STATUS lockStatus = lockClientMessageQueue(queue, wait);
  if (lockStatus != LGMP_OK)
    return lockStatus;

  subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
  if (unlikely(queue->client->sessionID != queue->header->sessionID))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_INVALID_SESSION;
  }

  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;
  }

  struct LGMPClientMessageState state;
  if (unlikely(!snapshotClientMessageState(queue, &state)))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_CORRUPTED;
  }

  // if there is now no room, unlock and return
  if (unlikely(state.avail == 0))
  {
    LGMP_UNLOCK(hq->cMsgLock);
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_FULL;
  }

  // get the write position and copy in the data
  uint32_t wpos = state.wpos;

  LGMP_PREFETCH_W(&hq->cMsgs[wpos], 3);
  uint32_t wnext = (wpos + 1) & (LGMP_MSGS_MAX - 1);
  LGMP_PREFETCH_W(&hq->cMsgs[wnext], 2);
  LGMP_PREFETCH_R(data, 2);

  hq->cMsgs[wpos].size     = size;
  hq->cMsgs[wpos].clientID = queue->client->id;
  memcpy(hq->cMsgs[wpos].data, data, size);

  // advance the write pointer and decrement the available count
  wpos = (wpos + 1) & (LGMP_MSGS_MAX - 1);
  atomic_store_explicit(&hq->cMsgWPos, wpos, memory_order_release);
  atomic_fetch_sub_explicit(&hq->cMsgAvail, 1, memory_order_release);

  // increment the write serial
  uint32_t tmp = atomic_fetch_add(&hq->cMsgWSerial, 1);

  // unlock the client message buffer
  LGMP_UNLOCK(hq->cMsgLock);
  LGMP_QUEUE_UNLOCK(hq);

  // return the message serial if it's wanted
  if (serial)
    *serial = tmp + 1;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientSendData(PLGMPClientQueue queue,
    const void * restrict data, uint32_t size, uint32_t * serial)
{
  return clientSendData(queue, data, size, serial, true);
}

LGMP_STATUS lgmpClientTrySendData(PLGMPClientQueue queue,
    const void * restrict data, uint32_t size, uint32_t * serial)
{
  return clientSendData(queue, data, size, serial, false);
}

LGMP_STATUS lgmpClientGetSerial(PLGMPClientQueue queue, uint32_t * serial)
{
  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  uint32_t subs = atomic_load_explicit(&hq->subs, memory_order_acquire);

  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  const uint32_t value =
    atomic_load_explicit(&hq->cMsgRSerial, memory_order_acquire);

  if (unlikely(queue->client->sessionID != queue->header->sessionID))
    return LGMP_ERR_INVALID_SESSION;

  subs = atomic_load_explicit(&hq->subs, memory_order_acquire);
  if (unlikely((LGMP_SUBS_BAD(subs) & bit) ||
        hq->clientID[queue->id] != queue->client->id))
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (unlikely(!(LGMP_SUBS_ON(subs) & bit)))
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  *serial = value;
  return LGMP_OK;
}
