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

#include "lgmp/client.h"

#include "lgmp.h"
#include "headers.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define LGMP_HEARTBEAT_TIMEOUT 1000

struct LGMPClientQueue
{
  PLGMPClient   client;
  unsigned int  id;
  unsigned int  index;
  uint32_t      position;

  struct LGMPHeader      * header;
  struct LGMPHeaderQueue * hq;
};

struct LGMPClient
{
  uint8_t           * mem;
  struct LGMPHeader * header;

  uint32_t sessionID;
  uint64_t hosttime;
  uint64_t lastHeartbeat;

  struct LGMPClientQueue queues[LGMP_MAX_QUEUES];
};

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
  client->header        = header;
  client->hosttime      = atomic_load(&header->timestamp);
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
    uint8_t ** udata)
{
  assert(client);
  struct LGMPHeader * header = client->header;

  if (header->magic != LGMP_PROTOCOL_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (header->version != LGMP_PROTOCOL_VERSION)
    return LGMP_ERR_INVALID_VERSION;

  uint64_t timestamp = atomic_load(&header->timestamp);
#ifndef LGMP_REALACY
  // check the host's timestamp is updating
  if (timestamp == client->hosttime)
    return LGMP_ERR_INVALID_SESSION;
#endif

  client->sessionID     = header->sessionID;
  client->hosttime      = timestamp;
  client->lastHeartbeat = lgmpGetClockMS();

  if (udataSize) *udataSize = header->udataSize;
  if (udata    ) *udata     = (uint8_t*)&header->udata;

  memset(&client->queues, 0, sizeof(client->queues));
  return LGMP_OK;
}

bool lgmpClientSessionValid(PLGMPClient client)
{
  assert(client);

  // check if the host has been restarted
  if (client->sessionID != client->header->sessionID)
    return false;

#ifndef LGMP_REALACY

  // check if the heartbeat changed
  const uint64_t hosttime = atomic_load(&client->header->timestamp);
  const uint64_t now      = lgmpGetClockMS();
  if (client->hosttime != hosttime)
  {
    client->lastHeartbeat = now;
    client->hosttime      = hosttime;
    return true;
  }

  // check if the heartbeat timeout has been exceeded
  if (now - client->lastHeartbeat > LGMP_HEARTBEAT_TIMEOUT)
    return false;

#endif

  return true;
}

LGMP_STATUS lgmpClientSubscribe(PLGMPClient client, uint32_t queueID,
    PLGMPClientQueue * result)
{
  assert(client);
  assert(result);

  *result = NULL;

  struct LGMPHeaderQueue *hq = NULL;
  uint32_t queueIndex;
  for(queueIndex = 0; queueIndex < client->header->numQueues; ++queueIndex)
    if (client->header->queues[queueIndex].queueID == queueID)
    {
      hq = &client->header->queues[queueIndex];
      break;
    }

  if (!hq)
    return LGMP_ERR_NO_SUCH_QUEUE;

  *result = &client->queues[queueIndex];
  PLGMPClientQueue q = *result;

  // take the queue lock
  LGMP_QUEUE_LOCK(hq);
  uint64_t subs = atomic_load(&hq->subs);

  // recover subs for reuse that have been flagged as bad and have exceeded the
  // queue timeout
  if (LGMP_SUBS_ON(subs))
  {
    const uint64_t hosttime = atomic_load(&client->header->timestamp);
    uint32_t reap = 0;
    for(unsigned int id = 0; id < 32; ++id)
    {
      if ((LGMP_SUBS_BAD(subs) & (1U << id)) && hosttime > hq->timeout[id])
        reap |= (1U << id);
    }
    subs = LGMP_SUBS_CLEAR(subs, reap);
  }

  // find the next free queue ID
  unsigned int id = 0;
  while(id < 32 && ((LGMP_SUBS_ON(subs) | LGMP_SUBS_BAD(subs)) & (1U << id)))
    ++id;

  // check if full
  if (id == 32)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_FULL; //TODO: better return error
  }

  subs = LGMP_SUBS_SET(subs, 1U << id);
  atomic_store(&hq->subs, subs);
  atomic_fetch_add(&hq->newSubCount, 1);

  q->header   = client->header;
  q->client   = client;
  q->index    = queueIndex;
  q->id       = id;
  q->position = hq->position;
  q->hq       = hq;

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

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;

  LGMP_QUEUE_LOCK(hq);
  uint64_t subs = atomic_load(&hq->subs);
  if (LGMP_SUBS_BAD(subs) & bit)
  {
    LGMP_QUEUE_UNLOCK(hq);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  // unset the queue id bit
  subs = LGMP_SUBS_CLEAR(subs, bit);
  atomic_store(&hq->subs, subs);
  LGMP_QUEUE_UNLOCK(hq);

  memset(queue, 0, sizeof(struct LGMPClientQueue));
  *result = NULL;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientAdvanceToLast(PLGMPClientQueue queue)
{
  assert(queue);

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint64_t subs = atomic_load(&hq->subs);

  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (!(LGMP_SUBS_ON(subs) & bit))
  {
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  uint32_t end = atomic_load(&hq->position);
  if (end == queue->position)
    return LGMP_ERR_QUEUE_EMPTY;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->client->mem + hq->messagesOffset);

  uint32_t next = queue->position;
  uint32_t last;
  bool cleanup = true;
  bool locked  = false;
  while(true)
  {
    last = next;
    if (++next == hq->numMessages)
      next = 0;

    if (next == end)
      break;

    // turn off the pending bit for our queue
    struct LGMPHeaderMessage *msg = &messages[last];

    // turn off the pending bit for our queue
    if (((atomic_fetch_and(&msg->pendingSubs, ~bit) & ~bit) == 0) && cleanup)
    {
      if (!locked)
      {
        if (LGMP_QUEUE_TRY_LOCK(hq))
          locked = true;
        else
        {
          cleanup = false;
          continue;
        }
      }

      // someone else may have done this before we got the lock, so check
      if (hq->start != last)
      {
        LGMP_QUEUE_UNLOCK(hq);
        cleanup = false;
        locked  = false;
        continue;
      }

      // message finished
      hq->start = next;

      // decrement the count
      atomic_fetch_sub(&hq->count, 1);
    }
  }

  // release the lock if we have it
  if (locked)
  {
    // update the timeout
    atomic_store(&hq->msgTimeout,
        atomic_load(&queue->header->timestamp) + hq->maxTime);
    LGMP_QUEUE_UNLOCK(hq);
  }

  queue->position = last;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientProcess(PLGMPClientQueue queue, PLGMPMessage result)
{
  assert(queue);
  assert(result);

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint64_t subs = atomic_load(&hq->subs);

  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (!(LGMP_SUBS_ON(subs) & bit))
  {
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  if (atomic_load(&hq->position) == queue->position)
    return LGMP_ERR_QUEUE_EMPTY;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->client->mem + hq->messagesOffset);
  struct LGMPHeaderMessage *msg = &messages[queue->position];

  result->udata = msg->udata;
  result->size  = msg->size;
  result->mem   = queue->client->mem + msg->offset;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientMessageDone(PLGMPClientQueue queue)
{
  assert(queue);

  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint64_t subs = atomic_load(&hq->subs);

  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  if (!(LGMP_SUBS_ON(subs) & bit))
  {
    if (lgmpClientSessionValid(queue->client))
      return LGMP_ERR_QUEUE_UNSUBSCRIBED;
    else
      return LGMP_ERR_INVALID_SESSION;
  }

  if (atomic_load(&hq->position) == queue->position)
    return LGMP_ERR_QUEUE_EMPTY;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->client->mem + hq->messagesOffset);
  struct LGMPHeaderMessage *msg = &messages[queue->position];

  // turn off the pending bit for our queue and try to dequeue the message if
  // it's finished.
  if ((atomic_fetch_and(&msg->pendingSubs, ~bit) & ~bit) == 0 &&
      LGMP_QUEUE_TRY_LOCK(hq))
  {
    // someone else may have done this before we got the lock, so check
    if (hq->start != queue->position)
    {
      LGMP_QUEUE_UNLOCK(hq);
      goto done;
    }

    // message finished
    if (++hq->start == hq->numMessages)
      hq->start = 0;

    // decrement the count and update the timeout
    atomic_fetch_sub(&hq->count, 1);
    atomic_store(&hq->msgTimeout,
      atomic_load(&queue->header->timestamp) + hq->maxTime);

    LGMP_QUEUE_UNLOCK(hq);
  }

done:
  if (++queue->position == hq->numMessages)
    queue->position = 0;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientSendData(PLGMPClientQueue queue, const void * data,
    size_t size, uint32_t * serial)
{
  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint64_t subs = atomic_load(&hq->subs);

  if (size > LGMP_MSGS_SIZE)
    return LGMP_ERR_INVALID_SIZE;

  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  // if there is no room, just return
  if (atomic_load(&hq->cMsgAvail) == 0)
    return LGMP_ERR_QUEUE_FULL;

  // lock the client message buffer
  LGMP_LOCK(hq->cMsgLock);

  // if there is now now room, unlock and return
  if (atomic_load(&hq->cMsgAvail) == 0)
  {
    LGMP_UNLOCK(hq->cMsgLock);
    return LGMP_ERR_QUEUE_FULL;
  }

  // get the write position and copy in the data
  uint32_t wpos = atomic_load(&hq->cMsgWPos);
  hq->cMsgs[wpos].size = size;
  memcpy(hq->cMsgs[wpos].data, data, size);

  // advance the write pointer and decrement the available count
  if (wpos++ == LGMP_MSGS_MAX)
    wpos = 0;
  atomic_store(&hq->cMsgWPos, wpos);
  atomic_fetch_sub(&hq->cMsgAvail, 1);

  // increment the write serial
  uint32_t tmp = atomic_fetch_add(&hq->cMsgWSerial, 1);

  // unlock the client message buffer
  LGMP_UNLOCK(hq->cMsgLock);

  // return the message serial if it's wanted
  if (serial)
    *serial = tmp + 1;

  return LGMP_OK;
};

LGMP_STATUS lgmpClientGetSerial(PLGMPClientQueue queue, uint32_t * serial)
{
  struct LGMPHeaderQueue *hq = queue->hq;
  const uint32_t bit = 1U << queue->id;
  const uint64_t subs = atomic_load(&hq->subs);

  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  *serial = atomic_load(&hq->cMsgRSerial);
  return LGMP_OK;
}
