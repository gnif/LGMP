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

#ifndef LGMP_PRIVATE_LGMP_H
#define LGMP_PRIVATE_LGMP_H

#include "lgmp/lgmp.h"
#include "lgmp/status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef _MSC_VER
#include <unistd.h>
#endif

struct LGMPMemory
{
  PLGMPHost    host;
  unsigned int offset;
  uint32_t     size;
  void        *mem;
};

/* Internal context accessors used by separately allocated LGMP ABIs. */
void lgmpHostGetMemoryContext(PLGMPHost host, uint8_t ** mem, size_t * size,
    uint32_t ** sessionID);
LGMP_STATUS lgmpClientGetMemoryContext(PLGMPClient client, uint8_t ** mem,
    size_t * size, uint32_t ** sessionID, uint32_t * clientID);

// reads a millisecond-resolution monotonic counter
static inline bool lgmpClockReadMS(uint64_t * result)
{
#if defined(_WIN32)
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&counter) ||
      frequency.QuadPart <= 0 || counter.QuadPart < 0)
    return false;

  const uint64_t seconds =
    (uint64_t)(counter.QuadPart / frequency.QuadPart);
  const uint64_t ticks   =
    (uint64_t)(counter.QuadPart % frequency.QuadPart);
  *result = seconds * 1000ULL +
    ticks * 1000ULL / (uint64_t)frequency.QuadPart;
#else
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return false;

  *result = (uint64_t)now.tv_sec * 1000ULL +
    (uint64_t)now.tv_nsec / 1000000ULL;
#endif

  return true;
}

inline static void lgmpSleepMs(unsigned ms)
{
#ifdef _MSC_VER
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

#endif
