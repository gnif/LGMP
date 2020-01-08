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

#ifndef LGMP_PRIVATE_HEADERS_H
#define LGMP_PRIVATE_HEADERS_H

#include <stdint.h>
#include <stdatomic.h>

#define LGMP_PROTOCOL_MAGIC   0x504d474c
#define LGMP_PROTOCOL_VERSION 1
#define LGMP_MAX_QUEUES       5

struct LGMPHeaderMessage
{
  uint32_t type;
  uint32_t size;
  uint32_t offset;
  uint32_t pendingSubs;
};

struct LGMPHeaderQueue
{
  uint32_t type;
  uint32_t numMessages;

  uint32_t lock;
  uint32_t subs;
  uint32_t badSubs;

  uint32_t position;
  uint32_t count;
  uint32_t messagesOffset;
};

struct LGMPHeader
{
  uint32_t magic;
  uint32_t version;
  uint32_t sessionID;
  uint32_t heartbeat;
  uint32_t caps;
  uint32_t numQueues;
  struct LGMPHeaderQueue queues[LGMP_MAX_QUEUES];
};

#endif