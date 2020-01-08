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

#define LGMP_HEARTBEAT_TIMEOUT 200

struct LGMPClient
{
  uint8_t           * mem;
  struct LGMPHeader * header;

  uint32_t            sessionID;
  uint32_t            heartbeat;
  uint64_t            lastHeartbeat;

  struct LGMPCQueue   queues[LGMP_MAX_QUEUES];
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

  // sleep for the timeout to see if the host is alive and updating the heartbeat
  const uint32_t heartbeat = atomic_load(&header->heartbeat);
  usleep(LGMP_HEARTBEAT_TIMEOUT * 1000);
  if (header->heartbeat == heartbeat)
    return LGMP_ERR_INVALID_SESSION;

  *result = malloc(sizeof(struct LGMPClient));
  if (!*result)
    return LGMP_ERR_NO_MEM;

  PLGMPClient client = *result;
  client->mem           = (uint8_t*)mem;
  client->header        = header;
  client->sessionID     = header->sessionID;
  client->heartbeat     = header->heartbeat;
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

  const uint64_t now = lgmpGetClockMS();

  // check if the heartbeat changed
  if (client->heartbeat != client->header->heartbeat)
  {
    client->lastHeartbeat = now;
    client->heartbeat     = client->header->heartbeat;
    return true;
  }

  // check if the heartbeat timeout has been exceeded
  if (now - client->lastHeartbeat > LGMP_HEARTBEAT_TIMEOUT)
    return false;

  return true;
}

LGMP_STATUS lgmpClientSubscribe(PLGMPClient client, uint32_t queueID, PLGMPCQueue * result)
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

  *result      = &client->queues[queueIndex];
  PLGMPCQueue q = *result;

  // take the queue lock
  while(atomic_flag_test_and_set(&hq->lock)) {};
  const uint64_t subs = atomic_load(&hq->subs);

  // find the next free queue ID
  unsigned int id = 0;
  while(id < 32 && (LGMP_SUBS_ON(hq->subs) & (1U << id)))
    ++id;

  // check if full
  if (id == 32)
  {
    atomic_flag_clear(&hq->lock);
    return LGMP_ERR_QUEUE_FULL; //TODO: better return error
  }

  atomic_fetch_or  (&hq->subs, LGMP_SUBS_SET(0, 1U << id));
  atomic_flag_clear(&hq->lock);

  q->client   = client;
  q->index    = queueIndex;
  q->id       = id;
  q->position = hq->position;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientUnsubscribe(PLGMPCQueue * result)
{
  assert(*result);
  PLGMPCQueue queue = *result;
  assert(queue->client);

  struct LGMPHeaderQueue *hq = &queue->client->header->queues[queue->index];

  const uint32_t bit = 1U << queue->id;
  uint64_t subs = atomic_load(&hq->subs);
  if (LGMP_SUBS_BAD(subs) & bit)
    return LGMP_ERR_QUEUE_TIMEOUT;

  // unset the queue id bit
  while(atomic_flag_test_and_set(&hq->lock)) {};
  atomic_fetch_and(&hq->subs, ~bit);
  atomic_flag_clear(&hq->lock);

  memset(queue, 0, sizeof(struct LGMPCQueue));
  *result = NULL;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientProcess(PLGMPCQueue queue, PLGMPMessage result)
{
  assert(queue);
  assert(result);

  struct LGMPHeaderQueue *hq = &queue->client->header->queues[queue->index];

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

LGMP_STATUS lgmpClientMessageDone(PLGMPCQueue queue)
{
  assert(queue);

  struct LGMPHeaderQueue *hq = &queue->client->header->queues[queue->index];

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
  atomic_fetch_and(&msg->pendingSubs, ~bit);

  if (++queue->position == hq->numMessages)
    queue->position = 0;

  return LGMP_OK;
}