/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2026 Geoffrey McRae <geoff@hostfission.com>
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

#include "lgmp/stream.h"

#include "lgmp/client.h"
#include "lgmp/host.h"
#include "lgmp.h"
#include "stream.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct LGMPStreamLocal
{
  struct LGMPStreamShared     * shared;
  struct LGMPStreamDescriptor   descriptor;
  uint8_t                     * slots;
  uint32_t                      slotStride;
  uint32_t                    * sessionID;
  uint32_t                      expectedSessionID;
  uint32_t                      expectedClientID;
  uint32_t                      expectedEpoch;
  bool                          hostSide;

  bool                          writePending;
  uint64_t                      writeTicket;
  void                        * writeData;

  bool                          readPending;
  uint64_t                      readTicket;
  void                        * readData;

  LGMPStreamNotifyFn            notifier;
  void                        * notifierOpaque;
};

struct LGMPHostStream
{
  struct LGMPStreamLocal local;
  PLGMPMemory            memory;
};

struct LGMPClientStream
{
  struct LGMPStreamLocal local;
};

/*
 * The primary LGMP mapping may be write-combined. A C acquire/release pair is
 * not sufficient to drain WC stores on all supported compilers, so publishing
 * a cursor also includes an explicit hardware fence. Cursor updates remain
 * ordinary single-writer stores; there is no shared read-modify-write in the
 * stream hot path.
 */
static void streamWriteFence(void)
{
#if defined(_MSC_VER)
  MemoryBarrier();
#elif defined(__x86_64__) || defined(__i386__)
  _mm_sfence();
  atomic_thread_fence(memory_order_release);
#else
  atomic_thread_fence(memory_order_seq_cst);
#endif
}

static void streamReadFence(void)
{
#if defined(_MSC_VER)
  MemoryBarrier();
#elif defined(__x86_64__) || defined(__i386__)
  atomic_thread_fence(memory_order_acquire);
  _mm_lfence();
#else
  atomic_thread_fence(memory_order_seq_cst);
#endif
}

static uint32_t streamObserve(_Atomic(uint32_t) * value)
{
  const uint32_t result = atomic_load_explicit(value, memory_order_acquire);
  streamReadFence();
  return result;
}

static void streamPublish(_Atomic(uint32_t) * value, uint32_t next)
{
  streamWriteFence();
  atomic_store_explicit(value, next, memory_order_release);
  streamWriteFence();
}

static bool streamObserveCursor(struct LGMPStreamCursor * cursor,
    uint64_t * value)
{
  for(unsigned int attempt = 0; attempt < 8U; ++attempt)
  {
    const uint32_t before = streamObserve(&cursor->stamp);
    const unsigned int index = before & 1U;
    const uint32_t epoch = atomic_load_explicit(
        &cursor->value[index].epoch, memory_order_relaxed);
    const uint32_t sequence = atomic_load_explicit(
        &cursor->value[index].sequence, memory_order_relaxed);
    streamReadFence();
    const uint32_t after    = streamObserve(&cursor->stamp);
    if (before == after)
    {
      *value = ((uint64_t)epoch << 32) | sequence;
      return true;
    }
  }

  return false;
}

static void streamPublishCursor(struct LGMPStreamCursor * cursor,
    uint64_t value)
{
  const uint32_t stamp = atomic_load_explicit(&cursor->stamp,
      memory_order_relaxed) + 1U;
  const unsigned int index = stamp & 1U;
  atomic_store_explicit(&cursor->value[index].epoch,
      (uint32_t)(value >> 32),
      memory_order_relaxed);
  atomic_store_explicit(&cursor->value[index].sequence, (uint32_t)value,
      memory_order_relaxed);
  streamPublish(&cursor->stamp, stamp);
}

static uint64_t makeTicket(uint32_t epoch, uint32_t sequence)
{
  return ((uint64_t)epoch << 32) | sequence;
}

static uint32_t ticketEpoch(uint64_t ticket)
{
  return (uint32_t)(ticket >> 32);
}

static uint32_t ticketSequence(uint64_t ticket)
{
  return (uint32_t)ticket;
}

static uint32_t loadSessionID(const struct LGMPStreamLocal * local)
{
  const volatile uint32_t * sessionID = local->sessionID;
  return *sessionID;
}

static bool directionValid(uint32_t direction)
{
  return direction == LGMP_STREAM_HOST_TO_CLIENT ||
    direction == LGMP_STREAM_CLIENT_TO_HOST;
}

static bool policySupported(uint32_t policy)
{
  return policy == LGMP_STREAM_RELIABLE_FIFO;
}

static LGMP_STATUS calculateGeometry(uint32_t slotCount, uint32_t slotSize,
    uint32_t * slotStride, uint32_t * regionSize)
{
  if (!LGMP_IS_POW2(slotCount) || slotCount < 2U || slotSize == 0U)
    return LGMP_ERR_INVALID_ARGUMENT;

  const uint64_t stride64 =
    ((uint64_t)sizeof(struct LGMPStreamSlot) + slotSize +
      (LGMP_STREAM_CACHELINE - 1U)) &
    ~((uint64_t)LGMP_STREAM_CACHELINE - 1U);
  const uint64_t region64 = sizeof(struct LGMPStreamShared) +
    (uint64_t)slotCount * stride64;

  if (stride64 > UINT32_MAX || region64 > UINT32_MAX)
    return LGMP_ERR_INVALID_SIZE;

  *slotStride = (uint32_t)stride64;
  *regionSize = (uint32_t)region64;
  return LGMP_OK;
}

static LGMP_STATUS validateDescriptor(
    const struct LGMPStreamDescriptor * descriptor, const uint8_t * memory,
    size_t memorySize, uint32_t sessionID,
    struct LGMPStreamShared ** sharedResult, uint32_t * slotStrideResult)
{
  if (!descriptor || !memory || !sharedResult || !slotStrideResult)
    return LGMP_ERR_INVALID_ARGUMENT;

  if (descriptor->magic != LGMP_STREAM_DESCRIPTOR_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (descriptor->version != LGMP_STREAM_DESCRIPTOR_VERSION ||
      descriptor->size != sizeof(*descriptor))
    return LGMP_ERR_INVALID_VERSION;

  if (!directionValid(descriptor->direction) ||
      !policySupported(descriptor->policy))
    return LGMP_ERR_INVALID_ARGUMENT;

  uint32_t slotStride;
  uint32_t regionSize;
  LGMP_STATUS status = calculateGeometry(descriptor->slotCount,
      descriptor->slotSize, &slotStride, &regionSize);
  if (status != LGMP_OK)
    return status;

  if (descriptor->regionSize != regionSize ||
      (descriptor->offset & (LGMP_STREAM_CACHELINE - 1U)) != 0U ||
      descriptor->offset > memorySize ||
      descriptor->regionSize > memorySize - descriptor->offset)
    return LGMP_ERR_INVALID_SIZE;

  struct LGMPStreamShared * shared = (struct LGMPStreamShared *)
    (memory + descriptor->offset);
  if (((uintptr_t)shared & (LGMP_STREAM_CACHELINE - 1U)) != 0U)
    return LGMP_ERR_INVALID_ALIGNMENT;

  if (streamObserve(&shared->magic) != LGMP_STREAM_SHARED_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (shared->version != LGMP_STREAM_SHARED_VERSION ||
      shared->headerSize != sizeof(*shared))
    return LGMP_ERR_INVALID_VERSION;

  if (shared->sessionID != sessionID)
    return LGMP_ERR_INVALID_SESSION;

  if (shared->regionSize != descriptor->regionSize ||
      shared->direction != descriptor->direction ||
      shared->policy != descriptor->policy ||
      shared->slotCount != descriptor->slotCount ||
      shared->slotSize != descriptor->slotSize ||
      shared->slotStride != slotStride ||
      shared->slotsOffset != sizeof(*shared))
    return LGMP_ERR_CORRUPTED;

  *sharedResult = shared;
  *slotStrideResult = slotStride;
  return LGMP_OK;
}

static void initLocal(struct LGMPStreamLocal * local,
    struct LGMPStreamShared * shared,
    const struct LGMPStreamDescriptor * descriptor, uint32_t slotStride,
    uint32_t * sessionID, uint32_t expectedSessionID,
    uint32_t expectedClientID, uint32_t expectedEpoch, bool hostSide)
{
  memset(local, 0, sizeof(*local));
  local->shared             = shared;
  local->descriptor         = *descriptor;
  local->slots              = (uint8_t *)shared + sizeof(*shared);
  local->slotStride         = slotStride;
  local->sessionID          = sessionID;
  local->expectedSessionID  = expectedSessionID;
  local->expectedClientID   = expectedClientID;
  local->expectedEpoch      = expectedEpoch;
  local->hostSide           = hostSide;
}

static LGMP_STATUS validateSession(const struct LGMPStreamLocal * local)
{
  if (unlikely(loadSessionID(local) != local->expectedSessionID ||
      local->shared->sessionID != local->expectedSessionID))
    return LGMP_ERR_INVALID_SESSION;

  return LGMP_OK;
}

static LGMP_STATUS validateBinding(const struct LGMPStreamLocal * local,
    bool allowDraining)
{
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  if (local->expectedEpoch == 0U)
    return LGMP_ERR_STREAM_UNBOUND;

  uint32_t state = streamObserve(&local->shared->state);
  if (state != LGMP_STREAM_STATE_READY &&
      (!allowDraining || state != LGMP_STREAM_STATE_DRAINING))
    return LGMP_ERR_STREAM_UNBOUND;

  const uint32_t epoch    = streamObserve(&local->shared->epoch);
  const uint32_t clientID = streamObserve(&local->shared->clientID);
  if (epoch != local->expectedEpoch ||
      clientID != local->expectedClientID)
    return LGMP_ERR_STREAM_STALE;

  state = streamObserve(&local->shared->state);
  if ((state != LGMP_STREAM_STATE_READY &&
        (!allowDraining || state != LGMP_STREAM_STATE_DRAINING)) ||
      streamObserve(&local->shared->epoch) != epoch ||
      streamObserve(&local->shared->clientID) != clientID)
    return LGMP_ERR_STREAM_STALE;

  return LGMP_OK;
}

static bool localCanWrite(const struct LGMPStreamLocal * local)
{
  if (local->hostSide)
    return local->descriptor.direction == LGMP_STREAM_HOST_TO_CLIENT;

  return local->descriptor.direction == LGMP_STREAM_CLIENT_TO_HOST;
}

static struct LGMPStreamCursor * operationCursor(
    struct LGMPStreamLocal * local, bool write)
{
  return write ? &local->shared->producer : &local->shared->consumer;
}

static LGMP_STATUS beginOperation(struct LGMPStreamLocal * local, bool write)
{
  LGMP_STATUS status = validateBinding(local, !write);
  if (status != LGMP_OK)
    return status;

  struct LGMPStreamCursor * cursor = operationCursor(local, write);
  _Atomic(uint32_t) * active =
    &cursor->active[local->expectedEpoch & 1U];
  streamPublish(active, local->expectedEpoch);

  status = validateBinding(local, !write);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_INVALID_SESSION)
      streamPublish(active, 0);
    return status;
  }

  if (streamObserve(active) != local->expectedEpoch)
  {
    streamPublish(active, 0);
    return LGMP_ERR_CORRUPTED;
  }

  return LGMP_OK;
}

static void endOperation(struct LGMPStreamLocal * local, bool write)
{
  struct LGMPStreamCursor * cursor = operationCursor(local, write);
  streamPublish(&cursor->active[local->expectedEpoch & 1U], 0);
}

static bool hasActiveOperations(struct LGMPStreamShared * shared)
{
  for(unsigned int i = 0; i < 2U; ++i)
    if (streamObserve(&shared->producer.active[i]) != 0U ||
        streamObserve(&shared->consumer.active[i]) != 0U)
      return true;

  return false;
}

static LGMP_STATUS streamDrained(struct LGMPStreamLocal * local,
    bool * drained)
{
  uint64_t producer;
  uint64_t consumer;
  if (!streamObserveCursor(&local->shared->producer, &producer) ||
      !streamObserveCursor(&local->shared->consumer, &consumer))
    return LGMP_ERR_CORRUPTED;

  if (ticketEpoch(producer) != local->expectedEpoch ||
      ticketEpoch(consumer) != local->expectedEpoch)
    return LGMP_ERR_STREAM_STALE;

  const uint32_t used = ticketSequence(producer) - ticketSequence(consumer);
  if (used > local->descriptor.slotCount)
    return LGMP_ERR_CORRUPTED;

  *drained = used == 0U;
  return LGMP_OK;
}

static void abandonLocalOperations(struct LGMPStreamLocal * local)
{
  if (local->writePending)
  {
    endOperation(local, true);
    local->writePending = false;
    local->writeData    = NULL;
  }

  if (local->readPending)
  {
    endOperation(local, false);
    local->readPending = false;
    local->readData    = NULL;
  }
}

static struct LGMPStreamSlot * getSlot(struct LGMPStreamLocal * local,
    uint64_t ticket)
{
  const uint32_t index = ticketSequence(ticket) &
    (local->descriptor.slotCount - 1U);
  return (struct LGMPStreamSlot *)(local->slots +
      (size_t)index * local->slotStride);
}

static void notifyPeer(struct LGMPStreamLocal * local, uint32_t reasons)
{
  if (local->notifier)
    local->notifier(local->notifierOpaque, &local->descriptor, reasons);
}

LGMP_STATUS lgmpStreamPollInit(LGMPStreamPollState * state,
    const struct LGMPStreamPollConfig config)
{
  if (!state || config.minWaitUs == 0U ||
      config.maxWaitUs < config.minWaitUs)
    return LGMP_ERR_INVALID_ARGUMENT;

  state->_config = config;
  lgmpStreamPollActivity(state);
  return LGMP_OK;
}

void lgmpStreamPollActivity(LGMPStreamPollState * state)
{
  assert(state);
  state->_spinRemaining = state->_config.spinCount;
  state->_nextWaitUs    = state->_config.minWaitUs;
}

uint32_t lgmpStreamPollIdle(LGMPStreamPollState * state)
{
  assert(state);

  if (state->_spinRemaining)
  {
    --state->_spinRemaining;
    return 0U;
  }

  const uint32_t waitUs = state->_nextWaitUs;
  if (waitUs >= state->_config.maxWaitUs ||
      waitUs > state->_config.maxWaitUs / 2U)
    state->_nextWaitUs = state->_config.maxWaitUs;
  else
    state->_nextWaitUs = waitUs * 2U;

  return waitUs;
}

static LGMP_STATUS writeAcquire(struct LGMPStreamLocal * local,
    LGMPStreamBuffer * buffer)
{
  if (!buffer || !localCanWrite(local))
    return LGMP_ERR_INVALID_ARGUMENT;

  if (local->writePending)
    return LGMP_ERR_STREAM_BUSY;

  LGMP_STATUS status = beginOperation(local, true);
  if (status != LGMP_OK)
    return status;

  uint64_t producer;
  uint64_t consumer;
  if (!streamObserveCursor(&local->shared->producer, &producer) ||
      !streamObserveCursor(&local->shared->consumer, &consumer))
  {
    endOperation(local, true);
    return LGMP_ERR_CORRUPTED;
  }
  if (unlikely(ticketEpoch(producer) != local->expectedEpoch ||
      ticketEpoch(consumer) != local->expectedEpoch))
  {
    endOperation(local, true);
    return LGMP_ERR_STREAM_STALE;
  }

  const uint32_t used = ticketSequence(producer) - ticketSequence(consumer);
  if (unlikely(used > local->descriptor.slotCount))
  {
    endOperation(local, true);
    return LGMP_ERR_CORRUPTED;
  }
  if (used == local->descriptor.slotCount)
  {
    endOperation(local, true);
    return LGMP_ERR_STREAM_FULL;
  }

  struct LGMPStreamSlot * slot = getSlot(local, producer);
  void * data = (uint8_t *)slot + sizeof(*slot);

  local->writePending = true;
  local->writeTicket  = producer;
  local->writeData    = data;

  buffer->data      = data;
  buffer->capacity  = local->descriptor.slotSize;
  buffer->size      = 0;
  buffer->_ticket   = producer;
  buffer->_epoch    = local->expectedEpoch;
  return LGMP_OK;
}

static bool reservationMatches(bool pending, uint64_t ticket, void * data,
    uint32_t epoch, const LGMPStreamBuffer * buffer)
{
  return pending && buffer && buffer->data == data &&
    buffer->_ticket == ticket && buffer->_epoch == epoch;
}

static LGMP_STATUS writeCommit(struct LGMPStreamLocal * local,
    const LGMPStreamBuffer * buffer, uint32_t usedLength)
{
  if (!localCanWrite(local) || !reservationMatches(local->writePending,
        local->writeTicket, local->writeData, local->expectedEpoch, buffer))
    return LGMP_ERR_INVALID_ARGUMENT;

  if (usedLength > local->descriptor.slotSize)
    return LGMP_ERR_INVALID_SIZE;

  LGMP_STATUS status = validateBinding(local, true);
  if (status != LGMP_OK)
  {
    local->writePending = false;
    local->writeData    = NULL;
    if (status != LGMP_ERR_INVALID_SESSION)
      endOperation(local, true);
    return status;
  }

  uint64_t producer;
  if (!streamObserveCursor(&local->shared->producer, &producer))
  {
    local->writePending = false;
    local->writeData    = NULL;
    endOperation(local, true);
    return LGMP_ERR_CORRUPTED;
  }
  if (unlikely(producer != local->writeTicket))
  {
    local->writePending = false;
    local->writeData    = NULL;
    endOperation(local, true);
    if (ticketEpoch(producer) != local->expectedEpoch)
      return LGMP_ERR_STREAM_STALE;
    return LGMP_ERR_CORRUPTED;
  }

  struct LGMPStreamSlot * slot = getSlot(local, producer);
  slot->epoch     = ticketEpoch(producer);
  slot->ticket    = ticketSequence(producer);
  slot->length    = usedLength;
  slot->_reserved = 0;

  streamPublishCursor(&local->shared->producer,
      makeTicket(local->expectedEpoch, ticketSequence(producer) + 1U));
  local->writePending = false;
  local->writeData    = NULL;
  endOperation(local, true);
  notifyPeer(local, LGMP_STREAM_NOTIFY_DATA);
  return LGMP_OK;
}

static LGMP_STATUS writeCancel(struct LGMPStreamLocal * local,
    const LGMPStreamBuffer * buffer)
{
  if (!localCanWrite(local) || !reservationMatches(local->writePending,
        local->writeTicket, local->writeData, local->expectedEpoch, buffer))
    return LGMP_ERR_INVALID_ARGUMENT;

  LGMP_STATUS status = validateSession(local);
  local->writePending = false;
  local->writeData    = NULL;
  if (status == LGMP_OK)
    endOperation(local, true);
  return status;
}

static LGMP_STATUS readPeek(struct LGMPStreamLocal * local,
    LGMPStreamBuffer * buffer)
{
  if (!buffer || localCanWrite(local))
    return LGMP_ERR_INVALID_ARGUMENT;

  if (local->readPending)
    return LGMP_ERR_STREAM_BUSY;

  LGMP_STATUS status = beginOperation(local, false);
  if (status != LGMP_OK)
    return status;

  uint64_t consumer;
  uint64_t producer;
  if (!streamObserveCursor(&local->shared->consumer, &consumer) ||
      !streamObserveCursor(&local->shared->producer, &producer))
  {
    endOperation(local, false);
    return LGMP_ERR_CORRUPTED;
  }
  if (unlikely(ticketEpoch(producer) != local->expectedEpoch ||
      ticketEpoch(consumer) != local->expectedEpoch))
  {
    endOperation(local, false);
    return LGMP_ERR_STREAM_STALE;
  }

  const uint32_t used = ticketSequence(producer) - ticketSequence(consumer);
  if (unlikely(used > local->descriptor.slotCount))
  {
    endOperation(local, false);
    return LGMP_ERR_CORRUPTED;
  }
  if (used == 0U)
  {
    endOperation(local, false);
    return LGMP_ERR_STREAM_EMPTY;
  }

  struct LGMPStreamSlot * slot = getSlot(local, consumer);
  const uint32_t slotEpoch  = slot->epoch;
  const uint32_t slotTicket = slot->ticket;
  const uint32_t slotLength = slot->length;
  if (unlikely(slotEpoch != ticketEpoch(consumer) ||
      slotTicket != ticketSequence(consumer) ||
      slotLength > local->descriptor.slotSize))
  {
    endOperation(local, false);
    return LGMP_ERR_CORRUPTED;
  }

  void * data = (uint8_t *)slot + sizeof(*slot);
  local->readPending = true;
  local->readTicket  = consumer;
  local->readData    = data;

  buffer->data      = data;
  buffer->capacity  = local->descriptor.slotSize;
  buffer->size      = slotLength;
  buffer->_ticket   = consumer;
  buffer->_epoch    = local->expectedEpoch;
  return LGMP_OK;
}

static LGMP_STATUS readRelease(struct LGMPStreamLocal * local,
    const LGMPStreamBuffer * buffer)
{
  if (localCanWrite(local) || !reservationMatches(local->readPending,
        local->readTicket, local->readData, local->expectedEpoch, buffer))
    return LGMP_ERR_INVALID_ARGUMENT;

  LGMP_STATUS status = validateBinding(local, true);
  if (status != LGMP_OK)
  {
    local->readPending = false;
    local->readData    = NULL;
    if (status != LGMP_ERR_INVALID_SESSION)
      endOperation(local, false);
    return status;
  }

  uint64_t consumer;
  if (!streamObserveCursor(&local->shared->consumer, &consumer))
  {
    local->readPending = false;
    local->readData    = NULL;
    endOperation(local, false);
    return LGMP_ERR_CORRUPTED;
  }
  if (unlikely(consumer != local->readTicket))
  {
    local->readPending = false;
    local->readData    = NULL;
    endOperation(local, false);
    if (ticketEpoch(consumer) != local->expectedEpoch)
      return LGMP_ERR_STREAM_STALE;
    return LGMP_ERR_CORRUPTED;
  }

  streamPublishCursor(&local->shared->consumer,
      makeTicket(local->expectedEpoch, ticketSequence(consumer) + 1U));
  local->readPending = false;
  local->readData    = NULL;
  endOperation(local, false);
  notifyPeer(local, LGMP_STREAM_NOTIFY_CREDIT);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostStreamNew(PLGMPHost host,
    const struct LGMPStreamConfig config, PLGMPHostStream * result)
{
  assert(host);
  assert(result);
  *result = NULL;

  if (!directionValid(config.direction) || !policySupported(config.policy))
    return LGMP_ERR_INVALID_ARGUMENT;

  uint32_t slotStride;
  uint32_t regionSize;
  LGMP_STATUS status = calculateGeometry(config.slotCount, config.slotSize,
      &slotStride, &regionSize);
  if (status != LGMP_OK)
    return status;

  PLGMPHostStream stream = calloc(1, sizeof(*stream));
  if (!stream)
    return LGMP_ERR_NO_MEM;

  status = lgmpHostMemAllocAligned(host, regionSize, LGMP_STREAM_CACHELINE,
      &stream->memory);
  if (status != LGMP_OK)
  {
    free(stream);
    return status;
  }

  uint8_t * memory;
  size_t memorySize;
  uint32_t * sessionID;
  lgmpHostGetMemoryContext(host, &memory, &memorySize, &sessionID);
  (void)memorySize;

  if (((uintptr_t)memory & (LGMP_STREAM_CACHELINE - 1U)) != 0U)
  {
    lgmpHostMemFree(&stream->memory);
    free(stream);
    return LGMP_ERR_INVALID_ALIGNMENT;
  }

  struct LGMPStreamShared * shared = lgmpHostMemPtr(stream->memory);
  memset(shared, 0, regionSize);
  shared->version     = LGMP_STREAM_SHARED_VERSION;
  shared->headerSize  = sizeof(*shared);
  shared->regionSize  = regionSize;
  shared->sessionID   = *sessionID;
  shared->direction   = config.direction;
  shared->policy      = config.policy;
  shared->slotCount   = config.slotCount;
  shared->slotSize    = config.slotSize;
  shared->slotStride  = slotStride;
  shared->slotsOffset = sizeof(*shared);
  atomic_store_explicit(&shared->state, LGMP_STREAM_STATE_UNBOUND,
      memory_order_relaxed);
  atomic_store_explicit(&shared->epoch, 0, memory_order_relaxed);
  atomic_store_explicit(&shared->clientID, 0, memory_order_relaxed);
  streamPublishCursor(&shared->producer, 0);
  streamPublishCursor(&shared->consumer, 0);

  struct LGMPStreamDescriptor descriptor;
  descriptor.magic      = LGMP_STREAM_DESCRIPTOR_MAGIC;
  descriptor.version    = LGMP_STREAM_DESCRIPTOR_VERSION;
  descriptor.size       = sizeof(struct LGMPStreamDescriptor);
  descriptor.offset     = stream->memory->offset;
  descriptor.regionSize = regionSize;
  descriptor.direction  = config.direction;
  descriptor.policy     = config.policy;
  descriptor.slotCount  = config.slotCount;
  descriptor.slotSize   = config.slotSize;

  initLocal(&stream->local, shared, &descriptor, slotStride, sessionID,
      *sessionID, 0, 0, true);
  streamPublish(&shared->magic, LGMP_STREAM_SHARED_MAGIC);
  *result = stream;
  return LGMP_OK;
}

void lgmpHostStreamFree(PLGMPHostStream * stream)
{
  assert(stream);
  if (!*stream)
    return;

  lgmpHostStreamForceUnbind(*stream);

  lgmpHostMemFree(&(*stream)->memory);
  free(*stream);
  *stream = NULL;
}

void lgmpHostStreamGetDescriptor(PLGMPHostStream stream,
    struct LGMPStreamDescriptor * descriptor)
{
  assert(stream);
  assert(descriptor);
  *descriptor = stream->local.descriptor;
}

LGMP_STATUS lgmpHostStreamBind(PLGMPHostStream stream, uint32_t clientID,
    uint32_t * epochResult)
{
  assert(stream);
  if (!clientID)
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPStreamLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  if (streamObserve(&local->shared->state) != LGMP_STREAM_STATE_UNBOUND ||
      hasActiveOperations(local->shared))
    return LGMP_ERR_STREAM_BUSY;

  streamPublish(&local->shared->state, LGMP_STREAM_STATE_BINDING);
  if (hasActiveOperations(local->shared))
  {
    streamPublish(&local->shared->state, LGMP_STREAM_STATE_DRAINING);
    notifyPeer(local, LGMP_STREAM_NOTIFY_BINDING);
    return LGMP_ERR_STREAM_BUSY;
  }

  uint32_t epoch = streamObserve(&local->shared->epoch) + 1U;
  if (epoch == 0U)
    epoch = 1U;

  streamPublishCursor(&local->shared->producer, makeTicket(epoch, 0));
  streamPublishCursor(&local->shared->consumer, makeTicket(epoch, 0));
  atomic_store_explicit(&local->shared->clientID, clientID,
      memory_order_relaxed);
  atomic_store_explicit(&local->shared->epoch, epoch, memory_order_relaxed);

  local->expectedClientID = clientID;
  local->expectedEpoch    = epoch;
  local->writePending     = false;
  local->readPending      = false;
  local->writeData        = NULL;
  local->readData         = NULL;

  streamPublish(&local->shared->state, LGMP_STREAM_STATE_READY);
  if (epochResult)
    *epochResult = epoch;
  notifyPeer(local, LGMP_STREAM_NOTIFY_BINDING);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostStreamUnbind(PLGMPHostStream stream)
{
  assert(stream);
  struct LGMPStreamLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  const uint32_t state = streamObserve(&local->shared->state);
  if (state == LGMP_STREAM_STATE_UNBOUND)
    return hasActiveOperations(local->shared) ?
      LGMP_ERR_STREAM_BUSY : LGMP_OK;

  if (state == LGMP_STREAM_STATE_BINDING)
    return LGMP_ERR_STREAM_BUSY;

  if (state == LGMP_STREAM_STATE_READY)
  {
    streamPublish(&local->shared->state, LGMP_STREAM_STATE_DRAINING);
    notifyPeer(local, LGMP_STREAM_NOTIFY_BINDING);
  }
  else if (state != LGMP_STREAM_STATE_DRAINING)
    return LGMP_ERR_CORRUPTED;

  if (hasActiveOperations(local->shared))
    return LGMP_ERR_STREAM_BUSY;

  bool drained;
  status = streamDrained(local, &drained);
  if (status != LGMP_OK)
    return status;
  if (!drained)
    return LGMP_ERR_STREAM_BUSY;

  streamPublish(&local->shared->state, LGMP_STREAM_STATE_UNBOUND);
  atomic_store_explicit(&local->shared->clientID, 0, memory_order_relaxed);
  streamWriteFence();

  local->expectedClientID = 0;
  local->expectedEpoch    = 0;
  local->writePending     = false;
  local->readPending      = false;
  local->writeData        = NULL;
  local->readData         = NULL;
  notifyPeer(local, LGMP_STREAM_NOTIFY_BINDING);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostStreamForceUnbind(PLGMPHostStream stream)
{
  assert(stream);
  struct LGMPStreamLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  streamPublish(&local->shared->state, LGMP_STREAM_STATE_DRAINING);
  abandonLocalOperations(local);

  uint32_t epoch = streamObserve(&local->shared->epoch) + 1U;
  if (epoch == 0U)
    epoch = 1U;

  for(unsigned int i = 0; i < 2U; ++i)
  {
    streamPublish(&local->shared->producer.active[i], 0);
    streamPublish(&local->shared->consumer.active[i], 0);
  }

  streamPublishCursor(&local->shared->producer, makeTicket(epoch, 0));
  streamPublishCursor(&local->shared->consumer, makeTicket(epoch, 0));
  atomic_store_explicit(&local->shared->clientID, 0, memory_order_relaxed);
  atomic_store_explicit(&local->shared->epoch, epoch, memory_order_relaxed);

  local->expectedClientID = 0;
  local->expectedEpoch    = 0;
  streamPublish(&local->shared->state, LGMP_STREAM_STATE_UNBOUND);
  notifyPeer(local, LGMP_STREAM_NOTIFY_BINDING);
  return LGMP_OK;
}

static LGMP_STATUS getBinding(struct LGMPStreamLocal * local,
    uint32_t * clientID, uint32_t * epoch)
{
  if (!clientID || !epoch)
    return LGMP_ERR_INVALID_ARGUMENT;

  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  const uint32_t state = streamObserve(&local->shared->state);
  if (state != LGMP_STREAM_STATE_READY &&
      state != LGMP_STREAM_STATE_DRAINING)
  {
    *clientID = 0;
    *epoch    = streamObserve(&local->shared->epoch);
    return LGMP_ERR_STREAM_UNBOUND;
  }

  *clientID = streamObserve(&local->shared->clientID);
  *epoch    = streamObserve(&local->shared->epoch);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostStreamGetBinding(PLGMPHostStream stream,
    uint32_t * clientID, uint32_t * epoch)
{
  assert(stream);
  return getBinding(&stream->local, clientID, epoch);
}

void lgmpHostStreamSetNotifier(PLGMPHostStream stream,
    LGMPStreamNotifyFn notifier, void * opaque)
{
  assert(stream);
  stream->local.notifier       = notifier;
  stream->local.notifierOpaque = opaque;
}

LGMP_STATUS lgmpHostStreamWriteAcquire(PLGMPHostStream stream,
    LGMPStreamBuffer * buffer)
{
  assert(stream);
  return writeAcquire(&stream->local, buffer);
}

LGMP_STATUS lgmpHostStreamWriteCommit(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer, uint32_t usedLength)
{
  assert(stream);
  return writeCommit(&stream->local, buffer, usedLength);
}

LGMP_STATUS lgmpHostStreamWriteCancel(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer)
{
  assert(stream);
  return writeCancel(&stream->local, buffer);
}

LGMP_STATUS lgmpHostStreamReadPeek(PLGMPHostStream stream,
    LGMPStreamBuffer * buffer)
{
  assert(stream);
  return readPeek(&stream->local, buffer);
}

LGMP_STATUS lgmpHostStreamReadRelease(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer)
{
  assert(stream);
  return readRelease(&stream->local, buffer);
}

static LGMP_STATUS clientActivate(struct LGMPStreamLocal * local,
    uint32_t * epochResult)
{
  if (local->writePending || local->readPending)
    return LGMP_ERR_STREAM_BUSY;

  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  uint32_t state = streamObserve(&local->shared->state);
  if (state != LGMP_STREAM_STATE_READY &&
      state != LGMP_STREAM_STATE_DRAINING)
    return LGMP_ERR_STREAM_UNBOUND;

  const uint32_t clientID = streamObserve(&local->shared->clientID);
  const uint32_t epoch    = streamObserve(&local->shared->epoch);
  if (clientID != local->expectedClientID || epoch == 0U)
    return LGMP_ERR_STREAM_STALE;

  uint64_t producer;
  uint64_t consumer;
  if (!streamObserveCursor(&local->shared->producer, &producer) ||
      !streamObserveCursor(&local->shared->consumer, &consumer))
    return LGMP_ERR_CORRUPTED;

  if (ticketEpoch(producer) != epoch || ticketEpoch(consumer) != epoch)
    return LGMP_ERR_STREAM_STALE;

  if (ticketSequence(producer) - ticketSequence(consumer) >
      local->descriptor.slotCount)
    return LGMP_ERR_CORRUPTED;

  state = streamObserve(&local->shared->state);
  if ((state != LGMP_STREAM_STATE_READY &&
        state != LGMP_STREAM_STATE_DRAINING) ||
      streamObserve(&local->shared->clientID) != clientID ||
      streamObserve(&local->shared->epoch) != epoch)
    return LGMP_ERR_STREAM_STALE;

  local->expectedEpoch = epoch;
  if (epochResult)
    *epochResult = epoch;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientStreamAttach(PLGMPClient client,
    const struct LGMPStreamDescriptor * descriptor,
    PLGMPClientStream * result)
{
  assert(client);
  assert(result);
  *result = NULL;

  if (!descriptor)
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPStreamDescriptor descriptorSnapshot;
  memcpy(&descriptorSnapshot, descriptor, sizeof(descriptorSnapshot));

  uint8_t * memory;
  size_t memorySize;
  uint32_t * sessionID;
  uint32_t clientID;
  LGMP_STATUS status = lgmpClientGetMemoryContext(client, &memory,
      &memorySize, &sessionID, &clientID);
  if (status != LGMP_OK)
    return status;

  struct LGMPStreamShared * shared;
  uint32_t slotStride;
  status = validateDescriptor(&descriptorSnapshot, memory, memorySize,
      *sessionID, &shared, &slotStride);
  if (status != LGMP_OK)
    return status;

  PLGMPClientStream stream = calloc(1, sizeof(*stream));
  if (!stream)
    return LGMP_ERR_NO_MEM;

  initLocal(&stream->local, shared, &descriptorSnapshot, slotStride,
      sessionID, *sessionID, clientID, 0, false);

  status = clientActivate(&stream->local, NULL);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
      status != LGMP_ERR_STREAM_STALE)
  {
    free(stream);
    return status;
  }

  *result = stream;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientStreamActivate(PLGMPClientStream stream,
    uint32_t * epoch)
{
  assert(stream);
  return clientActivate(&stream->local, epoch);
}

void lgmpClientStreamDetach(PLGMPClientStream * stream)
{
  assert(stream);
  if (!*stream)
    return;

  if (validateSession(&(*stream)->local) == LGMP_OK)
    abandonLocalOperations(&(*stream)->local);
  free(*stream);
  *stream = NULL;
}

LGMP_STATUS lgmpClientStreamGetBinding(PLGMPClientStream stream,
    uint32_t * clientID, uint32_t * epoch)
{
  assert(stream);
  LGMP_STATUS status = getBinding(&stream->local, clientID, epoch);
  if (status != LGMP_OK)
    return status;

  if (*clientID != stream->local.expectedClientID ||
      *epoch != stream->local.expectedEpoch)
    return LGMP_ERR_STREAM_STALE;

  return LGMP_OK;
}

void lgmpClientStreamSetNotifier(PLGMPClientStream stream,
    LGMPStreamNotifyFn notifier, void * opaque)
{
  assert(stream);
  stream->local.notifier       = notifier;
  stream->local.notifierOpaque = opaque;
}

LGMP_STATUS lgmpClientStreamWriteAcquire(PLGMPClientStream stream,
    LGMPStreamBuffer * buffer)
{
  assert(stream);
  return writeAcquire(&stream->local, buffer);
}

LGMP_STATUS lgmpClientStreamWriteCommit(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer, uint32_t usedLength)
{
  assert(stream);
  return writeCommit(&stream->local, buffer, usedLength);
}

LGMP_STATUS lgmpClientStreamWriteCancel(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer)
{
  assert(stream);
  return writeCancel(&stream->local, buffer);
}

LGMP_STATUS lgmpClientStreamReadPeek(PLGMPClientStream stream,
    LGMPStreamBuffer * buffer)
{
  assert(stream);
  return readPeek(&stream->local, buffer);
}

LGMP_STATUS lgmpClientStreamReadRelease(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer)
{
  assert(stream);
  return readRelease(&stream->local, buffer);
}
