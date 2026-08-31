/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2026 Geoffrey McRae <geoff@hostfission.com>
 * https://github.com/gnif/LGMP
 * SPDX-License-Identifier: GPL-2.0-or-later
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

#include "lgmp/spmc.h"

#include "lgmp/client.h"
#include "lgmp/host.h"
#include "lgmp.h"
#include "spmc.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct LGMPSPMCLocal
{
  struct LGMPSPMCShared       * shared;
  struct LGMPSPMCDescriptor    descriptor;
  struct LGMPSPMCReaderShared * readers;
  uint8_t                     * slots;
  uint32_t                      slotStride;
  uint32_t                    * sessionID;
  uint32_t                      expectedSessionID;
};

struct LGMPHostSPMC
{
  struct LGMPSPMCLocal local;
  PLGMPMemory          memory;
  bool                 writePending;
  uint64_t             writeSequence;
  uint32_t             writeGeneration;
  void               * writeData;
};

struct LGMPClientSPMC
{
  struct LGMPSPMCLocal         local;
  struct LGMPSPMCReaderShared * reader;
  uint32_t                      readerID;
  uint32_t                      expectedClientID;
  uint32_t                      expectedEpoch;
  uint64_t                      pendingSkipped;
  _Atomic(uint32_t)             operationActive;
};

static uint32_t spmcObserve(_Atomic(uint32_t) * value)
{
  const uint32_t result = atomic_load_explicit(value, memory_order_acquire);
  lgmpSharedReadFence();
  return result;
}

static void spmcPublish(_Atomic(uint32_t) * value, uint32_t next)
{
  lgmpSharedWriteFence();
  atomic_store_explicit(value, next, memory_order_release);
  lgmpSharedWriteFence();
}

static bool spmcObserveCursor(struct LGMPSPMCCursor * cursor,
    uint64_t * value)
{
  for(unsigned int attempt = 0; attempt < LGMP_SPMC_READ_ATTEMPTS; ++attempt)
  {
    const uint32_t before = spmcObserve(&cursor->stamp);
    const unsigned int index = before & 1U;
    const uint32_t low = atomic_load_explicit(
        &cursor->value[index].low, memory_order_relaxed);
    const uint32_t high = atomic_load_explicit(
        &cursor->value[index].high, memory_order_relaxed);
    lgmpSharedReadFence();
    const uint32_t after = spmcObserve(&cursor->stamp);
    if (before == after)
    {
      *value = ((uint64_t)high << 32) | low;
      return true;
    }
  }

  return false;
}

static void spmcPublishCursor(struct LGMPSPMCCursor * cursor,
    uint64_t value)
{
  const uint32_t stamp = atomic_load_explicit(&cursor->stamp,
      memory_order_relaxed) + 1U;
  const unsigned int index = stamp & 1U;
  atomic_store_explicit(&cursor->value[index].low, (uint32_t)value,
      memory_order_relaxed);
  atomic_store_explicit(&cursor->value[index].high,
      (uint32_t)(value >> 32), memory_order_relaxed);
  spmcPublish(&cursor->stamp, stamp);
}

static uint32_t loadSessionID(const struct LGMPSPMCLocal * local)
{
  const volatile uint32_t * sessionID = local->sessionID;
  return *sessionID;
}

static LGMP_STATUS calculateGeometry(uint32_t slotCount, uint32_t slotSize,
    uint32_t maxReaders, uint32_t * slotStride, uint32_t * slotsOffset,
    uint32_t * regionSize)
{
  if (!LGMP_IS_POW2(slotCount) || slotCount < 2U || slotSize == 0U ||
      maxReaders == 0U || maxReaders > LGMP_MAX_CLIENTS)
    return LGMP_ERR_INVALID_ARGUMENT;

  const uint64_t stride64 =
    ((uint64_t)sizeof(struct LGMPSPMCSlot) + slotSize +
      (LGMP_SPMC_CACHELINE - 1U)) &
    ~((uint64_t)LGMP_SPMC_CACHELINE - 1U);
  const uint64_t slotsOffset64 = sizeof(struct LGMPSPMCShared) +
    (uint64_t)maxReaders * sizeof(struct LGMPSPMCReaderShared);
  const uint64_t region64 = slotsOffset64 +
    (uint64_t)slotCount * stride64;

  if (stride64 > UINT32_MAX || slotsOffset64 > UINT32_MAX ||
      region64 > UINT32_MAX)
    return LGMP_ERR_INVALID_SIZE;

  *slotStride = (uint32_t)stride64;
  *slotsOffset = (uint32_t)slotsOffset64;
  *regionSize = (uint32_t)region64;
  return LGMP_OK;
}

static LGMP_STATUS validateDescriptor(
    const struct LGMPSPMCDescriptor * descriptor, const uint8_t * memory,
    size_t memorySize, uint32_t sessionID,
    struct LGMPSPMCShared ** sharedResult, uint32_t * slotStrideResult)
{
  if (!descriptor || !memory || !sharedResult || !slotStrideResult)
    return LGMP_ERR_INVALID_ARGUMENT;

  if (descriptor->magic != LGMP_SPMC_DESCRIPTOR_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (descriptor->version != LGMP_SPMC_DESCRIPTOR_VERSION ||
      descriptor->size != sizeof(*descriptor))
    return LGMP_ERR_INVALID_VERSION;

  if (descriptor->reserved != 0U)
    return LGMP_ERR_INVALID_ARGUMENT;

  uint32_t slotStride;
  uint32_t slotsOffset;
  uint32_t regionSize;
  LGMP_STATUS status = calculateGeometry(descriptor->slotCount,
      descriptor->slotSize, descriptor->maxReaders, &slotStride,
      &slotsOffset, &regionSize);
  if (status != LGMP_OK)
    return status;

  if (descriptor->regionSize != regionSize ||
      (descriptor->offset & (LGMP_SPMC_CACHELINE - 1U)) != 0U ||
      descriptor->offset > memorySize ||
      descriptor->regionSize > memorySize - descriptor->offset)
    return LGMP_ERR_INVALID_SIZE;

  struct LGMPSPMCShared * shared = (struct LGMPSPMCShared *)
    (memory + descriptor->offset);
  if (((uintptr_t)shared & (LGMP_SPMC_CACHELINE - 1U)) != 0U)
    return LGMP_ERR_INVALID_ALIGNMENT;

  if (spmcObserve(&shared->magic) != LGMP_SPMC_SHARED_MAGIC)
    return LGMP_ERR_INVALID_MAGIC;

  if (shared->version != LGMP_SPMC_SHARED_VERSION ||
      shared->headerSize != sizeof(*shared))
    return LGMP_ERR_INVALID_VERSION;

  if (shared->sessionID != sessionID)
    return LGMP_ERR_INVALID_SESSION;

  if (shared->regionSize != descriptor->regionSize ||
      shared->slotCount != descriptor->slotCount ||
      shared->slotSize != descriptor->slotSize ||
      shared->slotStride != slotStride ||
      shared->readersOffset != sizeof(*shared) ||
      shared->maxReaders != descriptor->maxReaders ||
      shared->slotsOffset != slotsOffset)
    return LGMP_ERR_CORRUPTED;

  *sharedResult = shared;
  *slotStrideResult = slotStride;
  return LGMP_OK;
}

static void initLocal(struct LGMPSPMCLocal * local,
    struct LGMPSPMCShared * shared,
    const struct LGMPSPMCDescriptor * descriptor, uint32_t slotStride,
    uint32_t * sessionID, uint32_t expectedSessionID)
{
  memset(local, 0, sizeof(*local));
  local->shared            = shared;
  local->descriptor        = *descriptor;
  local->readers           = (struct LGMPSPMCReaderShared *)
    ((uint8_t *)shared + shared->readersOffset);
  local->slots             = (uint8_t *)shared + shared->slotsOffset;
  local->slotStride        = slotStride;
  local->sessionID         = sessionID;
  local->expectedSessionID = expectedSessionID;
}

static LGMP_STATUS validateSession(const struct LGMPSPMCLocal * local)
{
  if (unlikely(loadSessionID(local) != local->expectedSessionID ||
      local->shared->sessionID != local->expectedSessionID))
    return LGMP_ERR_INVALID_SESSION;

  return LGMP_OK;
}

static struct LGMPSPMCReaderShared * getReader(
    struct LGMPSPMCLocal * local, uint32_t readerID)
{
  if (readerID >= local->descriptor.maxReaders)
    return NULL;

  return &local->readers[readerID];
}

static struct LGMPSPMCSlot * getSlot(struct LGMPSPMCLocal * local,
    uint64_t sequence)
{
  const uint32_t index = (uint32_t)sequence &
    (local->descriptor.slotCount - 1U);
  return (struct LGMPSPMCSlot *)(local->slots +
      (size_t)index * local->slotStride);
}

static bool readerHasActiveOperation(
    struct LGMPSPMCReaderShared * reader)
{
  return spmcObserve(&reader->active[0]) != 0U ||
    spmcObserve(&reader->active[1]) != 0U;
}

static LGMP_STATUS validateClientBinding(
    const struct LGMPClientSPMC * stream)
{
  LGMP_STATUS status = validateSession(&stream->local);
  if (status != LGMP_OK)
    return status;

  if (stream->expectedEpoch == 0U)
    return LGMP_ERR_STREAM_UNBOUND;

  uint32_t state = spmcObserve(&stream->reader->state);
  if (state != LGMP_SPMC_READER_READY)
    return LGMP_ERR_STREAM_UNBOUND;

  const uint32_t epoch = spmcObserve(&stream->reader->epoch);
  const uint32_t clientID = spmcObserve(&stream->reader->clientID);
  if (epoch != stream->expectedEpoch ||
      clientID != stream->expectedClientID)
    return LGMP_ERR_STREAM_STALE;

  state = spmcObserve(&stream->reader->state);
  if (state != LGMP_SPMC_READER_READY ||
      spmcObserve(&stream->reader->epoch) != epoch ||
      spmcObserve(&stream->reader->clientID) != clientID)
    return LGMP_ERR_STREAM_STALE;

  return LGMP_OK;
}

static LGMP_STATUS beginClientOperation(struct LGMPClientSPMC * stream)
{
  if (atomic_exchange_explicit(&stream->operationActive, 1U,
        memory_order_acquire) != 0U)
    return LGMP_ERR_STREAM_BUSY;

  LGMP_STATUS status = validateClientBinding(stream);
  if (status != LGMP_OK)
  {
    atomic_exchange_explicit(&stream->operationActive, 0U,
        memory_order_release);
    return status;
  }

  _Atomic(uint32_t) * active =
    &stream->reader->active[stream->expectedEpoch & 1U];
  spmcPublish(active, stream->expectedEpoch);

  status = validateClientBinding(stream);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_INVALID_SESSION)
      spmcPublish(active, 0U);
    atomic_exchange_explicit(&stream->operationActive, 0U,
        memory_order_release);
    return status;
  }

  if (spmcObserve(active) != stream->expectedEpoch)
  {
    spmcPublish(active, 0U);
    atomic_exchange_explicit(&stream->operationActive, 0U,
        memory_order_release);
    return LGMP_ERR_CORRUPTED;
  }

  return LGMP_OK;
}

static void unlockClientOperation(struct LGMPClientSPMC * stream)
{
  atomic_exchange_explicit(&stream->operationActive, 0U,
      memory_order_release);
}

static void endClientOperation(struct LGMPClientSPMC * stream)
{
  spmcPublish(&stream->reader->active[stream->expectedEpoch & 1U], 0U);
  unlockClientOperation(stream);
}

static uint64_t addSkipped(uint64_t current, uint64_t additional)
{
  if (additional > UINT64_MAX - current)
    return UINT64_MAX;

  return current + additional;
}

LGMP_STATUS lgmpHostSPMCNew(PLGMPHost host,
    const struct LGMPSPMCConfig config, PLGMPHostSPMC * result)
{
  assert(host);
  assert(result);
  *result = NULL;

  uint32_t slotStride;
  uint32_t slotsOffset;
  uint32_t regionSize;
  LGMP_STATUS status = calculateGeometry(config.slotCount, config.slotSize,
      config.maxReaders, &slotStride, &slotsOffset, &regionSize);
  if (status != LGMP_OK)
    return status;

  PLGMPHostSPMC stream = calloc(1, sizeof(*stream));
  if (!stream)
    return LGMP_ERR_NO_MEM;

  status = lgmpHostMemAllocAligned(host, regionSize, LGMP_SPMC_CACHELINE,
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

  if (((uintptr_t)memory & (LGMP_SPMC_CACHELINE - 1U)) != 0U)
  {
    lgmpHostMemFree(&stream->memory);
    free(stream);
    return LGMP_ERR_INVALID_ALIGNMENT;
  }

  struct LGMPSPMCShared * shared = lgmpHostMemPtr(stream->memory);
  memset(shared, 0, regionSize);
  shared->version       = LGMP_SPMC_SHARED_VERSION;
  shared->headerSize    = sizeof(*shared);
  shared->regionSize    = regionSize;
  shared->sessionID     = *sessionID;
  shared->slotCount     = config.slotCount;
  shared->slotSize      = config.slotSize;
  shared->slotStride    = slotStride;
  shared->readersOffset = sizeof(*shared);
  shared->maxReaders    = config.maxReaders;
  shared->slotsOffset   = slotsOffset;
  spmcPublishCursor(&shared->producer, 0U);

  struct LGMPSPMCReaderShared * readers =
    (struct LGMPSPMCReaderShared *)((uint8_t *)shared +
      shared->readersOffset);
  for(uint32_t i = 0; i < config.maxReaders; ++i)
  {
    atomic_store_explicit(&readers[i].state, LGMP_SPMC_READER_UNBOUND,
        memory_order_relaxed);
    spmcPublishCursor(&readers[i].cursor, 0U);
  }

  struct LGMPSPMCDescriptor descriptor;
  descriptor.magic      = LGMP_SPMC_DESCRIPTOR_MAGIC;
  descriptor.version    = LGMP_SPMC_DESCRIPTOR_VERSION;
  descriptor.size       = sizeof(struct LGMPSPMCDescriptor);
  descriptor.offset     = stream->memory->offset;
  descriptor.regionSize = regionSize;
  descriptor.slotCount  = config.slotCount;
  descriptor.slotSize   = config.slotSize;
  descriptor.maxReaders = config.maxReaders;
  descriptor.reserved   = 0U;

  initLocal(&stream->local, shared, &descriptor, slotStride, sessionID,
      *sessionID);
  spmcPublish(&shared->magic, LGMP_SPMC_SHARED_MAGIC);
  *result = stream;
  return LGMP_OK;
}

void lgmpHostSPMCFree(PLGMPHostSPMC * stream)
{
  assert(stream);
  if (!*stream)
    return;

  if ((*stream)->writePending)
  {
    LGMPSPMCBuffer buffer;
    buffer.data = (*stream)->writeData;
    buffer.capacity = (*stream)->local.descriptor.slotSize;
    buffer._generation = (*stream)->writeGeneration;
    buffer.sequence = (*stream)->writeSequence;
    (void)lgmpHostSPMCWriteCancel(*stream, &buffer);
  }

  for(uint32_t i = 0; i < (*stream)->local.descriptor.maxReaders; ++i)
    (void)lgmpHostSPMCReaderForceUnbind(*stream, i);

  lgmpHostMemFree(&(*stream)->memory);
  free(*stream);
  *stream = NULL;
}

void lgmpHostSPMCGetDescriptor(PLGMPHostSPMC stream,
    struct LGMPSPMCDescriptor * descriptor)
{
  assert(stream);
  assert(descriptor);
  *descriptor = stream->local.descriptor;
}

LGMP_STATUS lgmpHostSPMCReaderBind(PLGMPHostSPMC stream, uint32_t clientID,
    uint32_t * readerIDResult, uint32_t * epochResult)
{
  assert(stream);
  if (!clientID || !readerIDResult || !epochResult)
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  struct LGMPSPMCReaderShared * reader = NULL;
  uint32_t readerID = 0U;
  for(uint32_t i = 0; i < local->descriptor.maxReaders; ++i)
  {
    const uint32_t state = spmcObserve(&local->readers[i].state);
    if (state != LGMP_SPMC_READER_UNBOUND &&
        spmcObserve(&local->readers[i].clientID) == clientID)
      return LGMP_ERR_STREAM_BUSY;

    if (!reader && state == LGMP_SPMC_READER_UNBOUND &&
        !readerHasActiveOperation(&local->readers[i]))
    {
      reader = &local->readers[i];
      readerID = i;
    }
  }

  if (!reader)
    return LGMP_ERR_STREAM_FULL;

  spmcPublish(&reader->state, LGMP_SPMC_READER_BINDING);
  if (readerHasActiveOperation(reader))
  {
    /* The activity belongs to a stale operation from the prior binding.
     * Leave its marker intact, but make the slot claimable once it clears. */
    spmcPublish(&reader->state, LGMP_SPMC_READER_UNBOUND);
    return LGMP_ERR_STREAM_BUSY;
  }

  uint64_t producer;
  if (!spmcObserveCursor(&local->shared->producer, &producer))
  {
    spmcPublish(&reader->state, LGMP_SPMC_READER_UNBOUND);
    return LGMP_ERR_CORRUPTED;
  }

  uint32_t epoch = spmcObserve(&reader->epoch) + 1U;
  if (epoch == 0U)
    epoch = 1U;

  spmcPublish(&reader->active[0], 0U);
  spmcPublish(&reader->active[1], 0U);
  spmcPublishCursor(&reader->cursor, producer);
  atomic_store_explicit(&reader->clientID, clientID, memory_order_relaxed);
  atomic_store_explicit(&reader->epoch, epoch, memory_order_relaxed);
  spmcPublish(&reader->state, LGMP_SPMC_READER_READY);

  *readerIDResult = readerID;
  *epochResult = epoch;
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCReaderUnbind(PLGMPHostSPMC stream,
    uint32_t readerID)
{
  assert(stream);
  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  struct LGMPSPMCReaderShared * reader = getReader(local, readerID);
  if (!reader)
    return LGMP_ERR_INVALID_ARGUMENT;

  const uint32_t state = spmcObserve(&reader->state);
  if (state == LGMP_SPMC_READER_UNBOUND)
    return readerHasActiveOperation(reader) ?
      LGMP_ERR_STREAM_BUSY : LGMP_OK;

  if (state == LGMP_SPMC_READER_BINDING)
    return LGMP_ERR_STREAM_BUSY;

  if (state == LGMP_SPMC_READER_READY)
    spmcPublish(&reader->state, LGMP_SPMC_READER_DRAINING);
  else if (state != LGMP_SPMC_READER_DRAINING)
    return LGMP_ERR_CORRUPTED;

  if (readerHasActiveOperation(reader))
    return LGMP_ERR_STREAM_BUSY;

  atomic_store_explicit(&reader->clientID, 0U, memory_order_relaxed);
  spmcPublish(&reader->state, LGMP_SPMC_READER_UNBOUND);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCReaderForceUnbind(PLGMPHostSPMC stream,
    uint32_t readerID)
{
  assert(stream);
  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  struct LGMPSPMCReaderShared * reader = getReader(local, readerID);
  if (!reader)
    return LGMP_ERR_INVALID_ARGUMENT;

  spmcPublish(&reader->state, LGMP_SPMC_READER_DRAINING);

  uint32_t epoch = spmcObserve(&reader->epoch) + 1U;
  if (epoch == 0U)
    epoch = 1U;

  uint64_t producer;
  if (!spmcObserveCursor(&local->shared->producer, &producer))
    return LGMP_ERR_CORRUPTED;

  spmcPublish(&reader->active[0], 0U);
  spmcPublish(&reader->active[1], 0U);
  spmcPublishCursor(&reader->cursor, producer);
  atomic_store_explicit(&reader->clientID, 0U, memory_order_relaxed);
  atomic_store_explicit(&reader->epoch, epoch, memory_order_relaxed);
  spmcPublish(&reader->state, LGMP_SPMC_READER_UNBOUND);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCReaderGetState(PLGMPHostSPMC stream,
    uint32_t readerID, struct LGMPSPMCReaderState * stateResult)
{
  assert(stream);
  if (!stateResult)
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  struct LGMPSPMCReaderShared * reader = getReader(local, readerID);
  if (!reader)
    return LGMP_ERR_INVALID_ARGUMENT;

  for(unsigned int attempt = 0; attempt < LGMP_SPMC_READ_ATTEMPTS; ++attempt)
  {
    const uint32_t state = spmcObserve(&reader->state);
    const uint32_t epoch = spmcObserve(&reader->epoch);
    const uint32_t clientID = spmcObserve(&reader->clientID);
    uint64_t consumer;
    uint64_t producer;
    if (!spmcObserveCursor(&reader->cursor, &consumer) ||
        !spmcObserveCursor(&local->shared->producer, &producer))
      continue;

    if (spmcObserve(&reader->state) != state ||
        spmcObserve(&reader->epoch) != epoch ||
        spmcObserve(&reader->clientID) != clientID)
      continue;

    if (state > LGMP_SPMC_READER_DRAINING || consumer > producer)
      return LGMP_ERR_CORRUPTED;

    stateResult->readerID         = readerID;
    stateResult->state            = state;
    stateResult->clientID         = clientID;
    stateResult->epoch            = epoch;
    stateResult->producerSequence = producer;
    stateResult->consumerSequence = consumer;
    return LGMP_OK;
  }

  return LGMP_ERR_STREAM_BUSY;
}

LGMP_STATUS lgmpHostSPMCWriteAcquire(PLGMPHostSPMC stream,
    LGMPSPMCBuffer * buffer)
{
  assert(stream);
  if (!buffer)
    return LGMP_ERR_INVALID_ARGUMENT;
  if (stream->writePending)
    return LGMP_ERR_STREAM_BUSY;

  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
    return status;

  uint64_t producer;
  if (!spmcObserveCursor(&local->shared->producer, &producer))
    return LGMP_ERR_CORRUPTED;
  if (producer == UINT64_MAX)
    return LGMP_ERR_CORRUPTED;

  struct LGMPSPMCSlot * slot = getSlot(local, producer);
  const uint32_t generation = spmcObserve(&slot->generation);
  if (generation & 1U)
    return LGMP_ERR_STREAM_BUSY;

  spmcPublish(&slot->generation, generation + 1U);
  void * data = (uint8_t *)slot + sizeof(*slot);

  stream->writePending = true;
  stream->writeSequence = producer;
  stream->writeGeneration = generation + 1U;
  stream->writeData = data;
  buffer->data = data;
  buffer->capacity = local->descriptor.slotSize;
  buffer->_generation = generation + 1U;
  buffer->sequence = producer;
  return LGMP_OK;
}

static bool writeReservationMatches(const struct LGMPHostSPMC * stream,
    const LGMPSPMCBuffer * buffer)
{
  return stream->writePending && buffer &&
    buffer->data == stream->writeData &&
    buffer->sequence == stream->writeSequence &&
    buffer->_generation == stream->writeGeneration;
}

static void clearWriteReservation(struct LGMPHostSPMC * stream)
{
  stream->writePending = false;
  stream->writeSequence = 0U;
  stream->writeGeneration = 0U;
  stream->writeData = NULL;
}

LGMP_STATUS lgmpHostSPMCWriteCommit(PLGMPHostSPMC stream,
    const LGMPSPMCBuffer * buffer, uint32_t usedLength)
{
  assert(stream);
  if (!writeReservationMatches(stream, buffer))
    return LGMP_ERR_INVALID_ARGUMENT;
  if (usedLength > stream->local.descriptor.slotSize)
    return LGMP_ERR_INVALID_SIZE;

  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
  {
    clearWriteReservation(stream);
    return status;
  }

  uint64_t producer;
  if (!spmcObserveCursor(&local->shared->producer, &producer) ||
      producer != stream->writeSequence)
  {
    clearWriteReservation(stream);
    return LGMP_ERR_CORRUPTED;
  }

  struct LGMPSPMCSlot * slot = getSlot(local, producer);
  if (spmcObserve(&slot->generation) != stream->writeGeneration ||
      !(stream->writeGeneration & 1U))
  {
    clearWriteReservation(stream);
    return LGMP_ERR_CORRUPTED;
  }

  slot->sequenceLow  = (uint32_t)producer;
  slot->sequenceHigh = (uint32_t)(producer >> 32);
  slot->length       = usedLength;
  slot->flags        = 0U;
  spmcPublish(&slot->generation, stream->writeGeneration + 1U);
  spmcPublishCursor(&local->shared->producer, producer + 1U);
  clearWriteReservation(stream);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCWriteCancel(PLGMPHostSPMC stream,
    const LGMPSPMCBuffer * buffer)
{
  assert(stream);
  if (!writeReservationMatches(stream, buffer))
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPSPMCLocal * local = &stream->local;
  LGMP_STATUS status = validateSession(local);
  if (status != LGMP_OK)
  {
    clearWriteReservation(stream);
    return status;
  }

  uint64_t producer;
  if (!spmcObserveCursor(&local->shared->producer, &producer) ||
      producer != stream->writeSequence)
  {
    clearWriteReservation(stream);
    return LGMP_ERR_CORRUPTED;
  }

  struct LGMPSPMCSlot * slot = getSlot(local, producer);
  if (spmcObserve(&slot->generation) != stream->writeGeneration ||
      !(stream->writeGeneration & 1U))
  {
    clearWriteReservation(stream);
    return LGMP_ERR_CORRUPTED;
  }

  slot->sequenceLow  = (uint32_t)producer;
  slot->sequenceHigh = (uint32_t)(producer >> 32);
  slot->length       = 0U;
  slot->flags        = LGMP_SPMC_SLOT_CANCELLED;
  spmcPublish(&slot->generation, stream->writeGeneration + 1U);
  spmcPublishCursor(&local->shared->producer, producer + 1U);
  clearWriteReservation(stream);
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCPublishV(PLGMPHostSPMC stream,
    const void * first, uint32_t firstSize,
    const void * second, uint32_t secondSize, uint64_t * sequenceResult)
{
  assert(stream);
  if ((!first && firstSize != 0U) || (!second && secondSize != 0U))
    return LGMP_ERR_INVALID_ARGUMENT;
  if (firstSize > stream->local.descriptor.slotSize ||
      secondSize > stream->local.descriptor.slotSize - firstSize)
    return LGMP_ERR_INVALID_SIZE;

  LGMPSPMCBuffer buffer;
  LGMP_STATUS status = lgmpHostSPMCWriteAcquire(stream, &buffer);
  if (status != LGMP_OK)
    return status;

  if (firstSize)
    memcpy(buffer.data, first, firstSize);
  if (secondSize)
    memcpy((uint8_t *)buffer.data + firstSize, second, secondSize);

  status = lgmpHostSPMCWriteCommit(stream, &buffer,
      firstSize + secondSize);
  if (status != LGMP_OK)
  {
    (void)lgmpHostSPMCWriteCancel(stream, &buffer);
    return status;
  }

  if (sequenceResult)
    *sequenceResult = buffer.sequence;
  return LGMP_OK;
}

LGMP_STATUS lgmpHostSPMCPublish(PLGMPHostSPMC stream, const void * data,
    uint32_t size, uint64_t * sequence)
{
  return lgmpHostSPMCPublishV(stream, data, size, NULL, 0U, sequence);
}

static LGMP_STATUS clientActivate(struct LGMPClientSPMC * stream,
    uint32_t * epochResult)
{
  LGMP_STATUS status = validateSession(&stream->local);
  if (status != LGMP_OK)
    return status;

  uint32_t state = spmcObserve(&stream->reader->state);
  if (state != LGMP_SPMC_READER_READY)
    return LGMP_ERR_STREAM_UNBOUND;

  const uint32_t clientID = spmcObserve(&stream->reader->clientID);
  const uint32_t epoch = spmcObserve(&stream->reader->epoch);
  if (clientID != stream->expectedClientID || epoch == 0U)
    return LGMP_ERR_STREAM_STALE;

  uint64_t consumer;
  uint64_t producer;
  if (!spmcObserveCursor(&stream->reader->cursor, &consumer) ||
      !spmcObserveCursor(&stream->local.shared->producer, &producer))
    return LGMP_ERR_CORRUPTED;
  if (consumer > producer)
    return LGMP_ERR_CORRUPTED;

  state = spmcObserve(&stream->reader->state);
  if (state != LGMP_SPMC_READER_READY ||
      spmcObserve(&stream->reader->clientID) != clientID ||
      spmcObserve(&stream->reader->epoch) != epoch)
    return LGMP_ERR_STREAM_STALE;

  if (stream->expectedEpoch != epoch)
    stream->pendingSkipped = 0U;
  stream->expectedEpoch = epoch;
  if (epochResult)
    *epochResult = epoch;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientSPMCAttach(PLGMPClient client,
    const struct LGMPSPMCDescriptor * descriptor, uint32_t readerID,
    PLGMPClientSPMC * result)
{
  assert(client);
  assert(result);
  *result = NULL;

  if (!descriptor)
    return LGMP_ERR_INVALID_ARGUMENT;

  struct LGMPSPMCDescriptor descriptorSnapshot;
  memcpy(&descriptorSnapshot, descriptor, sizeof(descriptorSnapshot));
  if (readerID >= descriptorSnapshot.maxReaders)
    return LGMP_ERR_INVALID_ARGUMENT;

  uint8_t * memory;
  size_t memorySize;
  uint32_t * sessionID;
  uint32_t clientID;
  LGMP_STATUS status = lgmpClientGetMemoryContext(client, &memory,
      &memorySize, &sessionID, &clientID);
  if (status != LGMP_OK)
    return status;

  struct LGMPSPMCShared * shared;
  uint32_t slotStride;
  status = validateDescriptor(&descriptorSnapshot, memory, memorySize,
      *sessionID, &shared, &slotStride);
  if (status != LGMP_OK)
    return status;

  PLGMPClientSPMC stream = calloc(1, sizeof(*stream));
  if (!stream)
    return LGMP_ERR_NO_MEM;

  initLocal(&stream->local, shared, &descriptorSnapshot, slotStride,
      sessionID, *sessionID);
  stream->reader = &stream->local.readers[readerID];
  stream->readerID = readerID;
  stream->expectedClientID = clientID;
  atomic_store_explicit(&stream->operationActive, 0U,
      memory_order_relaxed);

  status = clientActivate(stream, NULL);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
      status != LGMP_ERR_STREAM_STALE)
  {
    free(stream);
    return status;
  }

  *result = stream;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientSPMCActivate(PLGMPClientSPMC stream,
    uint32_t * epoch)
{
  assert(stream);
  return clientActivate(stream, epoch);
}

void lgmpClientSPMCDetach(PLGMPClientSPMC * stream)
{
  assert(stream);
  if (!*stream)
    return;

  free(*stream);
  *stream = NULL;
}

LGMP_STATUS lgmpClientSPMCGetBinding(PLGMPClientSPMC stream,
    uint32_t * clientID, uint32_t * epoch)
{
  assert(stream);
  if (!clientID || !epoch)
    return LGMP_ERR_INVALID_ARGUMENT;

  LGMP_STATUS status = validateSession(&stream->local);
  if (status != LGMP_OK)
    return status;

  const uint32_t state = spmcObserve(&stream->reader->state);
  if (state != LGMP_SPMC_READER_READY &&
      state != LGMP_SPMC_READER_DRAINING)
  {
    *clientID = 0U;
    *epoch = spmcObserve(&stream->reader->epoch);
    return LGMP_ERR_STREAM_UNBOUND;
  }

  *clientID = spmcObserve(&stream->reader->clientID);
  *epoch = spmcObserve(&stream->reader->epoch);
  if (*clientID != stream->expectedClientID ||
      *epoch != stream->expectedEpoch)
    return LGMP_ERR_STREAM_STALE;

  return LGMP_OK;
}

LGMP_STATUS lgmpClientSPMCSync(PLGMPClientSPMC stream,
    uint64_t * skippedResult)
{
  assert(stream);
  LGMP_STATUS status = beginClientOperation(stream);
  if (status != LGMP_OK)
    return status;

  uint64_t consumer;
  uint64_t producer;
  if (!spmcObserveCursor(&stream->reader->cursor, &consumer) ||
      !spmcObserveCursor(&stream->local.shared->producer, &producer))
  {
    endClientOperation(stream);
    return LGMP_ERR_CORRUPTED;
  }
  if (consumer > producer)
  {
    endClientOperation(stream);
    return LGMP_ERR_CORRUPTED;
  }

  status = validateClientBinding(stream);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_INVALID_SESSION)
      endClientOperation(stream);
    else
      unlockClientOperation(stream);
    return status;
  }

  spmcPublishCursor(&stream->reader->cursor, producer);
  endClientOperation(stream);
  if (skippedResult)
    *skippedResult = addSkipped(stream->pendingSkipped,
        producer - consumer);
  stream->pendingSkipped = 0U;
  return LGMP_OK;
}

LGMP_STATUS lgmpClientSPMCRead(PLGMPClientSPMC stream, void * data,
    uint32_t capacity, struct LGMPSPMCRecord * record)
{
  assert(stream);
  if (!data || !record)
    return LGMP_ERR_INVALID_ARGUMENT;

  memset(record, 0, sizeof(*record));
  LGMP_STATUS status = beginClientOperation(stream);
  if (status != LGMP_OK)
    return status;

  for(unsigned int attempt = 0; attempt < LGMP_SPMC_READ_ATTEMPTS; ++attempt)
  {
    uint64_t consumer;
    uint64_t producer;
    if (!spmcObserveCursor(&stream->reader->cursor, &consumer) ||
        !spmcObserveCursor(&stream->local.shared->producer, &producer))
      continue;

    if (consumer > producer)
    {
      endClientOperation(stream);
      return LGMP_ERR_CORRUPTED;
    }
    if (consumer == producer)
    {
      endClientOperation(stream);
      return LGMP_ERR_STREAM_EMPTY;
    }

    const uint64_t available = producer - consumer;
    uint64_t target = consumer;
    uint64_t skipped = 0U;
    if (available > stream->local.descriptor.slotCount)
    {
      target = producer - stream->local.descriptor.slotCount;
      skipped = target - consumer;
    }

    struct LGMPSPMCSlot * slot = getSlot(&stream->local, target);
    const uint32_t before = spmcObserve(&slot->generation);
    if (before & 1U)
      continue;

    const uint32_t sequenceLow = *(const volatile uint32_t *)
      &slot->sequenceLow;
    const uint32_t sequenceHigh = *(const volatile uint32_t *)
      &slot->sequenceHigh;
    const uint32_t length = *(const volatile uint32_t *)&slot->length;
    const uint32_t flags = *(const volatile uint32_t *)&slot->flags;
    lgmpSharedReadFence();
    const uint64_t slotSequence =
      ((uint64_t)sequenceHigh << 32) | sequenceLow;

    if (length > stream->local.descriptor.slotSize)
    {
      if (spmcObserve(&slot->generation) == before)
      {
        endClientOperation(stream);
        return LGMP_ERR_CORRUPTED;
      }
      continue;
    }

    if (slotSequence != target)
      continue;

    if (flags & ~LGMP_SPMC_SLOT_CANCELLED)
    {
      if (spmcObserve(&slot->generation) == before)
      {
        endClientOperation(stream);
        return LGMP_ERR_CORRUPTED;
      }
      continue;
    }

    if (flags & LGMP_SPMC_SLOT_CANCELLED)
    {
      const uint32_t after = spmcObserve(&slot->generation);
      if (before != after || (after & 1U))
        continue;
      if (length != 0U)
      {
        endClientOperation(stream);
        return LGMP_ERR_CORRUPTED;
      }

      status = validateClientBinding(stream);
      if (status != LGMP_OK)
      {
        if (status != LGMP_ERR_INVALID_SESSION)
          endClientOperation(stream);
        else
          unlockClientOperation(stream);
        return status;
      }

      uint64_t current;
      if (!spmcObserveCursor(&stream->reader->cursor, &current) ||
          current != consumer)
      {
        endClientOperation(stream);
        return LGMP_ERR_CORRUPTED;
      }

      spmcPublishCursor(&stream->reader->cursor, target + 1U);
      stream->pendingSkipped = addSkipped(stream->pendingSkipped,
          addSkipped(skipped, 1U));
      continue;
    }

    if (capacity < length)
    {
      const uint32_t after = spmcObserve(&slot->generation);
      if (before != after || (after & 1U))
        continue;

      record->sequence = target;
      record->skipped = addSkipped(stream->pendingSkipped, skipped);
      record->size = length;
      record->reserved = 0U;
      endClientOperation(stream);
      return LGMP_ERR_INVALID_SIZE;
    }

    if (length)
      memcpy(data, (const uint8_t *)slot + sizeof(*slot), length);
    lgmpSharedReadFence();

    const uint32_t after = spmcObserve(&slot->generation);
    if (before != after || (after & 1U) ||
        *(const volatile uint32_t *)&slot->sequenceLow != sequenceLow ||
        *(const volatile uint32_t *)&slot->sequenceHigh != sequenceHigh ||
        *(const volatile uint32_t *)&slot->length != length ||
        *(const volatile uint32_t *)&slot->flags != flags)
      continue;

    status = validateClientBinding(stream);
    if (status != LGMP_OK)
    {
      if (status != LGMP_ERR_INVALID_SESSION)
        endClientOperation(stream);
      else
        unlockClientOperation(stream);
      return status;
    }

    uint64_t current;
    if (!spmcObserveCursor(&stream->reader->cursor, &current) ||
        current != consumer)
    {
      endClientOperation(stream);
      return LGMP_ERR_CORRUPTED;
    }

    spmcPublishCursor(&stream->reader->cursor, target + 1U);
    record->sequence = target;
    record->skipped = addSkipped(stream->pendingSkipped, skipped);
    record->size = length;
    record->reserved = 0U;
    stream->pendingSkipped = 0U;
    endClientOperation(stream);
    return LGMP_OK;
  }

  endClientOperation(stream);
  return LGMP_ERR_STREAM_BUSY;
}
