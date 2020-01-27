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

#define LGMP_HEARTBEAT_TIMEOUT 2000

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
  if (header->magic != LGMP_PROTOCOL_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (header->version != LGMP_PROTOCOL_VERSION)
    return LGMP_ERR_INVALID_VERSION;

#ifndef LGMP_REALACY
  // check the host's timestamp is updating
  const uint64_t hosttime = atomic_load(&header->timestamp);
  int to = LGMP_HEARTBEAT_TIMEOUT;
  while(--to)
  {
    usleep(1000);
    if (atomic_load(&header->timestamp) != hosttime)
      break;
  }

  if (to < 0)
    return LGMP_ERR_INVALID_SESSION;
#endif

  *result = malloc(sizeof(struct LGMPClient));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPClient client = *result;
  client->mem           = (uint8_t*)mem;
  client->header        = header;
  client->sessionID     = header->sessionID;
  client->hosttime      = header->timestamp;
  client->lastHeartbeat = lgmpGetClockMS();

  memset(&client->queues, 0, sizeof(client->queues));
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
  while(atomic_flag_test_and_set(&hq->lock)) {};
  uint64_t subs = atomic_load(&hq->subs);

  // recover subs for reuse that have been flagged as bad and have exceeded the
  // queue timeout
  if (LGMP_SUBS_ON(subs))
  {
    const uint64_t hosttime = atomic_load(&client->header->timestamp);
    uint32_t reap = 0;
    for(unsigned int id = 0; id < 32; ++id)
    {
      if ((LGMP_SUBS_BAD(subs) & (1 << id)) && hosttime > hq->timeout[id])
        reap |= (1 << id);
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
    atomic_flag_clear(&hq->lock);
    return LGMP_ERR_QUEUE_FULL; //TODO: better return error
  }

  subs = LGMP_SUBS_SET(subs, 1U << id);
  atomic_store(&hq->subs, subs);
  atomic_flag_clear(&hq->lock);
  atomic_fetch_add(&hq->newSubCount, 1);

  q->header   = client->header;
  q->client   = client;
  q->index    = queueIndex;
  q->id       = id;
  q->position = hq->position;
  q->hq       = hq;

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

  while(atomic_flag_test_and_set(&hq->lock)) {};
  uint64_t subs = atomic_load(&hq->subs);
  if (LGMP_SUBS_BAD(subs) & bit)
  {
    atomic_flag_clear(&hq->lock);
    return LGMP_ERR_QUEUE_TIMEOUT;
  }

  // unset the queue id bit
  subs = LGMP_SUBS_CLEAR(subs, bit);
  atomic_store(&hq->subs, subs);
  atomic_flag_clear(&hq->lock);

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
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  uint32_t end = atomic_load(&hq->position);
  if (end == queue->position)
    return LGMP_ERR_QUEUE_EMPTY;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->client->mem + hq->messagesOffset);

  uint32_t next = queue->position;
  uint32_t last;
  while(true)
  {
    last = next;
    if (++next == hq->numMessages)
      next = 0;

    if (next == end)
      break;

    // turn off the pending bit for our queue
    struct LGMPHeaderMessage *msg = &messages[last];
    atomic_fetch_and(&msg->pendingSubs, ~bit);
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
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

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
    return LGMP_ERR_QUEUE_UNSUBSCRIBED;

  if (hq->position == queue->position)
    return LGMP_ERR_QUEUE_EMPTY;

  struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
    (queue->client->mem + hq->messagesOffset);
  struct LGMPHeaderMessage *msg = &messages[queue->position];

  // turn off the pending bit for our queue
  if ((atomic_fetch_and(&msg->pendingSubs, ~bit) & ~bit) == 0)
  {
    // if we are the last subscriber update the host queue position
    while(atomic_flag_test_and_set(&hq->lock)) {};
    uint32_t start = atomic_load(&hq->start);

    // check if the host process loop has not already done this
    if (start == queue->position)
    {
      // message finished
      if (++start == hq->numMessages)
        start = 0;

      // decrement the count and update the timeout if needed
      if (atomic_fetch_sub(&hq->count, 1) > 1)
        atomic_store(&hq->msgTimeout,
            atomic_load(&queue->header->timestamp) + hq->maxTime);

      atomic_store(&hq->start, start);
    }

    atomic_flag_clear(&hq->lock);
  }

  if (++queue->position == hq->numMessages)
    queue->position = 0;

  return LGMP_OK;
}
