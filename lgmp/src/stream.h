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

#ifndef LGMP_PRIVATE_STREAM_H
#define LGMP_PRIVATE_STREAM_H

#include "headers.h"
#include "lgmp/stream.h"

#define LGMP_STREAM_SHARED_MAGIC   0x4853534cU
#define LGMP_STREAM_SHARED_VERSION 1U
#define LGMP_STREAM_CACHELINE      64U

enum LGMPStreamState
{
  LGMP_STREAM_STATE_UNBOUND  = 0,
  LGMP_STREAM_STATE_BINDING  = 1,
  LGMP_STREAM_STATE_READY    = 2,
  LGMP_STREAM_STATE_DRAINING = 3
};

/*
 * A cursor is logically the 64-bit value (epoch << 32) | sequence. Splitting
 * it into two alternately published 32-bit pairs keeps the ABI safe on Win32
 * without locked 64-bit operations on write-combined memory. Readers accept a
 * pair only when the publication stamp remains stable.
 */
struct LGMPStreamCursorValue
{
  _Atomic(uint32_t) epoch;
  _Atomic(uint32_t) sequence;
};

struct ALIGNED_64 LGMPStreamCursor
{
  _Atomic(uint32_t) stamp;
  _Atomic(uint32_t) active[2];
  uint32_t          _padStamp;
  struct LGMPStreamCursorValue value[2];
  uint32_t          _pad[8];
};

/*
 * The immutable geometry, host-owned binding, producer cursor, and consumer
 * cursor each occupy independent cache lines. A stream is SPSC after binding,
 * so publication needs no shared lock or read-modify-write operation.
 */
struct ALIGNED_64 LGMPStreamShared
{
  /* ---- Line 0: immutable geometry ---- */
  _Atomic(uint32_t) magic;
  uint16_t          version;
  uint16_t          headerSize;
  uint32_t          regionSize;
  uint32_t          sessionID;
  uint32_t          direction;
  uint32_t          policy;
  uint32_t          slotCount;
  uint32_t          slotSize;
  uint32_t          slotStride;
  uint32_t          slotsOffset;
  uint32_t          _padGeometry[6];

  /* ---- Line 1: host-owned binding ---- */
  _Atomic(uint32_t) state;
  _Atomic(uint32_t) epoch;
  _Atomic(uint32_t) clientID;
  uint32_t          _padBinding[13];

  /* ---- Line 2: producer-owned cursor ---- */
  struct LGMPStreamCursor producer;

  /* ---- Line 3: consumer-owned cursor ---- */
  struct LGMPStreamCursor consumer;
};

struct ALIGNED_64 LGMPStreamSlot
{
  uint32_t epoch;
  uint32_t ticket;
  uint32_t length;
  uint32_t _reserved;
  uint8_t  _pad[48];
};

LGMP_STATIC_ASSERT(sizeof(struct LGMPStreamDescriptor) == 32,
    "LGMPStreamDescriptor size must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPStreamCursor) == 64,
    "LGMPStreamCursor size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPStreamCursor) == 64,
    "LGMPStreamCursor alignment must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPStreamShared) == 256,
    "LGMPStreamShared size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPStreamShared) == 64,
    "LGMPStreamShared alignment must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPStreamSlot) == 64,
    "LGMPStreamSlot size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPStreamSlot) == 64,
    "LGMPStreamSlot alignment must remain stable");
LGMP_STATIC_ASSERT(offsetof(struct LGMPStreamShared, state) == 64,
    "stream binding must begin a cache line");
LGMP_STATIC_ASSERT(offsetof(struct LGMPStreamShared, producer) == 128,
    "stream producer must begin a cache line");
LGMP_STATIC_ASSERT(offsetof(struct LGMPStreamShared, consumer) == 192,
    "stream consumer must begin a cache line");

#endif
