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

#ifndef LGMP_PRIVATE_LGMP_H
#define LGMP_PRIVATE_LGMP_H

#include "lgmp/lgmp.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

struct LGMPMemory
{
  PLGMPHost    host;
  unsigned int offset;
  uint32_t     size;
  void        *mem;
};

// returns a milliseond resolution monotonic counter
inline static uint64_t lgmpGetClockMS(void)
{
#if defined(_WIN32)
  static LARGE_INTEGER freq  = { 0 };
  static LARGE_INTEGER start = { 0 };
  if (!freq.QuadPart)
  {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
  }

  LARGE_INTEGER time;
  QueryPerformanceCounter(&time);
  return (((time.QuadPart - start.QuadPart) * 1000ULL) / freq.QuadPart) + 1;
#else
  struct timespec tsnow;
  if (clock_gettime(CLOCK_MONOTONIC, &tsnow) != 0)
    return 0;
  return (uint64_t)tsnow.tv_sec * 1000ULL + (uint64_t)tsnow.tv_nsec / 1000000ULL;
#endif
}

#endif
