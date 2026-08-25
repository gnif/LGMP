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

#ifndef LGMP_PRIVATE_SPMC_H
#define LGMP_PRIVATE_SPMC_H

#include "headers.h"
#include "lgmp/spmc.h"

#define LGMP_SPMC_SHARED_MAGIC      0x48504d53U
#define LGMP_SPMC_SHARED_VERSION    1U
#define LGMP_SPMC_CACHELINE         64U
#define LGMP_SPMC_READ_ATTEMPTS     8U

#define LGMP_SPMC_SLOT_CANCELLED    (1U << 0)

struct LGMPSPMCCursorValue
{
  _Atomic(uint32_t) low;
  _Atomic(uint32_t) high;
};

/* Alternating 32-bit pairs keep 64-bit cursor publication safe on Win32. */
struct ALIGNED_64 LGMPSPMCCursor
{
  _Atomic(uint32_t) stamp;
  uint32_t          _padStamp[3];
  struct LGMPSPMCCursorValue value[2];
  uint32_t          _pad[8];
};

struct ALIGNED_64 LGMPSPMCReaderShared
{
  /* ---- Line 0: host-owned binding and reader activity ---- */
  _Atomic(uint32_t) state;
  _Atomic(uint32_t) epoch;
  _Atomic(uint32_t) clientID;
  _Atomic(uint32_t) active[2];
  uint32_t          _padBinding[11];

  /* ---- Line 1: reader-owned cursor ---- */
  struct LGMPSPMCCursor cursor;
};

struct ALIGNED_64 LGMPSPMCShared
{
  /* ---- Line 0: immutable geometry ---- */
  _Atomic(uint32_t) magic;
  uint16_t          version;
  uint16_t          headerSize;
  uint32_t          regionSize;
  uint32_t          sessionID;
  uint32_t          slotCount;
  uint32_t          slotSize;
  uint32_t          slotStride;
  uint32_t          readersOffset;
  uint32_t          maxReaders;
  uint32_t          slotsOffset;
  uint32_t          _padGeometry[6];

  /* ---- Line 1: producer-owned monotonic head ---- */
  struct LGMPSPMCCursor producer;
};

struct ALIGNED_64 LGMPSPMCSlot
{
  _Atomic(uint32_t) generation;
  uint32_t          sequenceLow;
  uint32_t          sequenceHigh;
  uint32_t          length;
  uint32_t          flags;
  uint8_t           _pad[44];
};

LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCDescriptor) == 32,
    "LGMPSPMCDescriptor size must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCConfig) == 12,
    "LGMPSPMCConfig size must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCRecord) == 24,
    "LGMPSPMCRecord size must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCReaderState) == 32,
    "LGMPSPMCReaderState size must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCCursor) == 64,
    "LGMPSPMCCursor size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPSPMCCursor) == 64,
    "LGMPSPMCCursor alignment must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCReaderShared) == 128,
    "LGMPSPMCReaderShared size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPSPMCReaderShared) == 64,
    "LGMPSPMCReaderShared alignment must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCShared) == 128,
    "LGMPSPMCShared size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPSPMCShared) == 64,
    "LGMPSPMCShared alignment must remain stable");
LGMP_STATIC_ASSERT(sizeof(struct LGMPSPMCSlot) == 64,
    "LGMPSPMCSlot size must remain stable");
LGMP_STATIC_ASSERT(LGMP_ALIGNOF(struct LGMPSPMCSlot) == 64,
    "LGMPSPMCSlot alignment must remain stable");
LGMP_STATIC_ASSERT(offsetof(struct LGMPSPMCReaderShared, cursor) == 64,
    "SPMC reader cursor must begin a cache line");
LGMP_STATIC_ASSERT(offsetof(struct LGMPSPMCShared, producer) == 64,
    "SPMC producer cursor must begin a cache line");

#endif
