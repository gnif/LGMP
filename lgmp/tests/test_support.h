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

#ifndef LGMP_TEST_SUPPORT_H
#define LGMP_TEST_SUPPORT_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lgmp/client.h"
#include "lgmp/host.h"
#include "lgmp/status.h"

#define TEST_MEMORY_SIZE      (1024U * 1024U)
#define TEST_MEMORY_ALIGNMENT 4096U

struct TestFixture
{
  void        * allocation;
  uint8_t     * memory;
  PLGMPHost     host;
  pthread_t     pumpThread;
  atomic_bool   pumpStop;
  LGMP_STATUS   pumpStatus;
  bool          pumpStarted;
};

extern const uint8_t  testSessionUdata[];
extern const uint32_t testSessionUdataSize;

bool testCheck(bool condition, const char * expression,
    const char * file, unsigned int line);
bool testExpectStatus(const char * operation, LGMP_STATUS actual,
    LGMP_STATUS expected);
size_t testHostAllocationSize(size_t requestedSize);

#define TEST_CHECK(expression) \
  testCheck(!!(expression), #expression, __FILE__, __LINE__)

bool testFixtureInit(struct TestFixture * fixture);
bool testFixtureStart(struct TestFixture * fixture);
bool testFixtureStop(struct TestFixture * fixture);
bool testFixtureRestart(struct TestFixture * fixture);
bool testFixtureDestroy(struct TestFixture * fixture);

bool testClientInit(struct TestFixture * fixture, PLGMPClient * client,
    uint32_t * clientID);

bool testMonotonicMS(uint64_t * result);
bool testSleepMS(unsigned int milliseconds);

#endif
