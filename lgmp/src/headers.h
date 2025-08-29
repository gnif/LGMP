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

#include "lgmp.h"

#define LGMP_PROTOCOL_MAGIC   0x504d474c
#define LGMP_PROTOCOL_VERSION 8
#define LGMP_MAX_QUEUES       5

// maximum number of client messages supported
#define LGMP_MSGS_MAX  16

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

  #define __builtin_prefetch(...) do {} while(0)

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

#define LGMP_QUEUE_LOCK(hq) LGMP_LOCK(hq->lock)
#define LGMP_QUEUE_TRY_LOCK(hq) LGMP_TRY_LOCK(hq->lock)
#define LGMP_QUEUE_UNLOCK(hq) LGMP_UNLOCK(hq->lock)

#define LGMP_SUBS_ON(x)          (uint32_t)(x >> 32)
#define LGMP_SUBS_BAD(x)         (uint32_t)(x >>  0)
#define LGMP_SUBS_OR_BAD(x, bad) ((x) | (bad))
#define LGMP_SUBS_CLEAR(x, cl)   ((x) & ~((cl) | ((uint64_t)(cl) << 32)))
#define LGMP_SUBS_SET(x, st)     ((x) | ((uint64_t)(st) << 32))

#ifdef _MSC_VER
  //NOTE: the default alignment of MSVC matches that of gcc (for now)
  //If this becomes an issue in the future we will need to investigate a
  //solution
  #define ALIGNED
#else
  #define ALIGNED __attribute__((aligned(4)))
#endif

struct LGMPHeaderMessage
{
  uint32_t udata;
  uint32_t size;
  uint32_t offset;
  _Atomic(uint32_t) pendingSubs;
}
ALIGNED;

struct LGMPClientMessage
{
  uint32_t size;
  uint8_t  data[LGMP_MSGS_SIZE];
}
ALIGNED;

struct LGMPHeaderQueue
{
  uint32_t queueID;
  uint32_t numMessages;
  _Atomic(uint32_t) newSubCount;
  uint32_t maxTime;

  _Atomic(uint32_t) position;
  uint32_t messagesOffset;
  uint64_t timeout[32];
  uint32_t clientID[32];

  /* the lock MUST be held to use the following values */
  _Atomic(uint32_t) lock;
  _Atomic(uint64_t) subs; // see LGMP_SUBS_* macros
  uint32_t start;
  _Atomic(uint64_t) msgTimeout;
  _Atomic(uint32_t) count;

  /* messages submitted from the client */
  _Atomic(uint32_t) cMsgLock;
  _Atomic(uint32_t) cMsgAvail;
  _Atomic(uint32_t) cMsgWPos;
  _Atomic(uint32_t) cMsgWSerial;
  _Atomic(uint32_t) cMsgRSerial;
  struct LGMPClientMessage cMsgs[LGMP_MSGS_MAX];
}
ALIGNED;

#ifdef _MSC_VER
  // don't warn on zero length arrays
  #pragma warning(push)
  #pragma warning(disable: 4200)
#endif

struct LGMPHeader
{
  uint32_t magic;
  uint32_t version;
  uint32_t sessionID;
  uint32_t numQueues;
  _Atomic(uint64_t) timestamp;
  struct LGMPHeaderQueue queues[LGMP_MAX_QUEUES];
  uint32_t udataSize;
  uint8_t  udata[0];
}
ALIGNED;

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

#endif
