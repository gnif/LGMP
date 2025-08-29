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

#ifndef LGMP_PRIVATE_HEADERS_H
#define LGMP_PRIVATE_HEADERS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "lgmp.h"

#define LGMP_PROTOCOL_MAGIC   0x504d474c
#define LGMP_PROTOCOL_VERSION 10

// maximum number of client messages supported
#define LGMP_MSGS_MAX  16

/* Cacheline alignment helpers */
#if defined(_MSC_VER)
#  define ALIGNED_64 __declspec(align(64))
#  define ALIGNED_16 __declspec(align(16))
#  define ALIGNED_4  __declspec(align(4))
#else
#  define ALIGNED_64 __attribute__((aligned(64)))
#  define ALIGNED_16 __attribute__((aligned(16)))
#  define ALIGNED_4  __attribute__((aligned(4)))
#endif

#ifdef _MSC_VER
  #define LGMP_LOCK_INIT(lock) \
    (lock) = 0;

  #define LGMP_LOCK(lock) \
    while (InterlockedCompareExchange((volatile LONG *)&(lock), 1, 0) != 0) {}

  #define LGMP_TRY_LOCK(lock) \
    (InterlockedCompareExchange((volatile LONG *)&(lock), 1, 0) == 0)

  #define LGMP_UNLOCK(lock) \
    InterlockedExchange((volatile LONG *)&(lock), 0)

  #define _Atomic(T) volatile T

  /**
   * WARNING: These defines must be used with GREAT CARE. They do not perfectly
   * replicate the behaviour of the std C11 methods */
  #define atomic_load(var) *(var)
  #define atomic_store(var, v) (*var = v)
  #define atomic_fetch_add(var, v) (InterlockedAdd((volatile LONG *)(var), v) - v)
  #define atomic_fetch_and(var, v) InterlockedAnd((volatile LONG *)(var), v)
  #define atomic_fetch_sub(var, v) (InterlockedAdd((volatile LONG *)(var), -(v)) + v)
  #define atomic_exchange(var, v) InterlockedExchange((volatile LONG *)(var), v)

  #define atomic_load_explicit(var, x) atomic_load(var)
  #define atomic_store_explicit(var, v, x) atomic_store(var, v)
  #define atomic_fetch_add_explicit(var, v, x) atomic_fetch_add(var, v)
  #define atomic_fetch_and_explicit(var, v, x) atomic_fetch_and(var, v)
  #define atomic_fetch_sub_explicit(var, v, x) atomic_fetch_sub(var, v)
  #define atomic_exchange_explicit(var, v, x) atomic_exchange(var, v)

#else
  #include <stdatomic.h>

  /**
   * Note: we do not use atomic_flag in order to remain ABI compatible with MSVC
   */

  #define LGMP_LOCK_INIT(lock) \
    atomic_store(&(lock), 0);

  #if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    #include <immintrin.h>
    #define LGMP_CPU_RELAX() _mm_pause()
  #else
    #define LGMP_CPU_RELAX() do {} while (0)
  #endif

  static inline void _LGMP_LOCK(_Atomic(uint32_t) * lock)
  {
    uint32_t expected = 0;
    int spins = 0;
    for (;;)
    {
      if (atomic_compare_exchange_weak_explicit(lock, &expected, 1,
            memory_order_acquire, memory_order_relaxed))
        break;

      LGMP_CPU_RELAX();
      if ((++spins & 0xFF) == 0) { /* mild backoff */ }
    }
  }
  #define LGMP_LOCK(lock) _LGMP_LOCK(&(lock))

  static inline bool _LGMP_TRY_LOCK(_Atomic(uint32_t) * lock)
  {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(lock, &expected, 1,
        memory_order_acquire, memory_order_relaxed);
  }
  #define LGMP_TRY_LOCK(lock) _LGMP_TRY_LOCK(&(lock))

  #define LGMP_UNLOCK(lock) \
    atomic_store_explicit(&(lock), 0, memory_order_release);

#endif

#if defined(__GNUC__)
  #define likely(x)   __builtin_expect(!!(x), 1)
  #define unlikely(x) __builtin_expect(!!(x), 0)
#else
  #define likely(x)   (x)
  #define unlikely(x) (x)
#endif

#if defined(_MSC_VER)
  #include <immintrin.h>
  #ifndef _MM_HINT_NTA
    #define _MM_HINT_NTA 0
    #define _MM_HINT_T0  3
    #define _MM_HINT_T1  2
    #define _MM_HINT_T2  1
  #endif
  /* map 0..3 to NTA..T0 */
  #define LGMP_PREFETCH_R(p,loc) _mm_prefetch((const char*)(p), ((loc)==0)?_MM_HINT_NTA:((loc)==1)?_MM_HINT_T2:((loc)==2)?_MM_HINT_T1:_MM_HINT_T0)
  #define LGMP_PREFETCH_W(p,loc) _mm_prefetch((const char*)(p), ((loc)==0)?_MM_HINT_NTA:((loc)==1)?_MM_HINT_T2:((loc)==2)?_MM_HINT_T1:_MM_HINT_T0)
#else
  #define LGMP_PREFETCH_R(p,loc) __builtin_prefetch((p), 0, (loc))
  #define LGMP_PREFETCH_W(p,loc) __builtin_prefetch((p), 1, (loc))
#endif
#define LGMP_PREFETCH_DIST 2

#define LGMP_IS_POW2(x) ((x) && (((x) & ((x) - 1)) == 0))

#define LGMP_QUEUE_LOCK(hq) LGMP_LOCK(hq->lock)
#define LGMP_QUEUE_TRY_LOCK(hq) LGMP_TRY_LOCK(hq->lock)
#define LGMP_QUEUE_UNLOCK(hq) LGMP_UNLOCK(hq->lock)

/* Subscribers bitset (trimmed to 8). Use 32-bit atomics for MSVC friendliness.
 * High 16 bits = ON, Low 16 bits = BAD (only low 8 bits used). */
#define LGMP_SUBS_ON(x)          (uint16_t)((x) >> 16)
#define LGMP_SUBS_BAD(x)         (uint16_t)((x) & 0xFFFF)
#define LGMP_SUBS_OR_BAD(x, bad) ((uint32_t)((x) | (uint32_t)(bad)))
#define LGMP_SUBS_CLEAR(x, cl)   ((uint32_t)((x) & ~(((uint32_t)(cl)) | (((uint32_t)(cl)) << 16))))
#define LGMP_SUBS_SET(x, st)     ((uint32_t)((x) | (((uint32_t)(st)) << 16)))

/* Ensure default packing on MSVC so external headers cannot break our SHM ABI */
#if defined(_MSC_VER)
#  pragma pack(push, 8)
#endif

struct LGMPHeaderMessage
{
  uint32_t udata;
  uint32_t size;
  uint32_t offset;
  _Atomic(uint32_t) pendingSubs;
}
ALIGNED_16;

struct LGMPClientMessage
{
  uint32_t size;
  uint8_t  data[LGMP_MSGS_SIZE];
}
ALIGNED_16;

struct LGMPHeaderQueue
{
  /* ---- Line 0: host-hot ring counters (64B) ---- */
  _Atomic(uint32_t) position;   /* tail: next write pos (host)   [4]  */
  uint32_t          start;      /* head: oldest pending          [4]  */
  _Atomic(uint32_t) count;      /* items queued                  [4]  */
  uint32_t          _pad0;      /* align                         [4]  */
  _Atomic(uint64_t) msgTimeout; /* next GC deadline              [8]  */
   uint32_t          _padHot[10];/* pad to 64                     [40] */

  /* ---- Line 1: lock alone (64B) ---- */
  _Atomic(uint32_t) lock;       /* queue lock (host & clients)   [4]  */
  uint32_t          _padLock[15];                                  /* 60 */

  /* ---- Line 2: client->host control ring counters (64B) ---- */
  _Atomic(uint32_t) cMsgLock;                                      /* 4  */
  _Atomic(uint32_t) cMsgAvail;   /* free slots remaining           [4] */
  _Atomic(uint32_t) cMsgWPos;    /* producer index (client)        [4] */
  _Atomic(uint32_t) cMsgWSerial; /* producer serial (client)       [4] */
  _Atomic(uint32_t) cMsgRSerial; /* consumer serial (host)         [4] */
  uint32_t          _padCMsg[11];                                   /* 44 */

  /* ---- Subs/GC state (rarely mutated) ---- */
  _Atomic(uint32_t) subs;         /* [31:16]=ON, [15:0]=BAD            */
  _Atomic(uint32_t) newSubCount;  /* incremented on new subs           */
  uint32_t          clientID[LGMP_MAX_CLIENTS];
  uint64_t          timeout [LGMP_MAX_CLIENTS];

  /* ---- Read-mostly config ---- */
  uint32_t queueID;
  uint32_t numMessages;       /* ring size (+1 sentinel) */
  uint32_t messagesOffset;    /* byte offset to messages[] from SHM base */
  uint32_t maxTime;           /* ms timeout for subs */

  struct LGMPClientMessage cMsgs[LGMP_MSGS_MAX];
}
ALIGNED_64;

#if defined(_MSC_VER)
#  define LGMP_STATIC_ASSERT static_assert
#else
#  define LGMP_STATIC_ASSERT _Static_assert
#endif

/* Sanity checks: keep the hot blocks on their own lines */
/* ring invariants */
LGMP_STATIC_ASSERT((LGMP_MSGS_MAX & (LGMP_MSGS_MAX - 1)) == 0,
                   "LGMP_MSGS_MAX must be power-of-two");
LGMP_STATIC_ASSERT((offsetof(struct LGMPHeaderQueue, position) & 63) == 0,
                   "position must begin a cache line");
LGMP_STATIC_ASSERT((offsetof(struct LGMPHeaderQueue, lock) & 63) == 0,
                   "lock must begin a cache line");
LGMP_STATIC_ASSERT((offsetof(struct LGMPHeaderQueue, cMsgLock) & 63) == 0,
                   "cMsg counters must begin a cache line");

#ifdef _MSC_VER
  // don't warn on zero length arrays
  #pragma warning(push)
  #pragma warning(disable: 4200)
#endif

struct LGMPHeader
{
  /* place timestamp on its own cacheline (first) */
  _Atomic(uint64_t) timestamp;
  uint32_t magic;
  uint32_t version;
  uint32_t sessionID;
  uint32_t numQueues;
  struct LGMPHeaderQueue queues[LGMP_MAX_QUEUES];
  uint32_t udataSize;
  uint8_t  udata[0];
}
ALIGNED_64;

LGMP_STATIC_ASSERT((offsetof(struct LGMPHeader, queues) & 63) == 0,
                   "LGMPHeader.queues must be 64B-aligned");
LGMP_STATIC_ASSERT(offsetof(struct LGMPHeader, udataSize) >
                   offsetof(struct LGMPHeader, queues),
                   "LGMPHeader.udataSize must follow queues[]");

#ifdef _MSC_VER
  #pragma warning(pop)
  #pragma pack(pop)
#endif

#endif
