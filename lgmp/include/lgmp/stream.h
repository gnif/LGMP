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

#ifndef LGMP_STREAM_H
#define LGMP_STREAM_H

#include <stdint.h>

#include "lgmp.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LGMP_STREAM_DESCRIPTOR_MAGIC   0x5254534cU
#define LGMP_STREAM_DESCRIPTOR_VERSION 1U

enum LGMPStreamDirection
{
  LGMP_STREAM_HOST_TO_CLIENT = 1,
  LGMP_STREAM_CLIENT_TO_HOST = 2
};

enum LGMPStreamPolicy
{
  LGMP_STREAM_RELIABLE_FIFO = 1,

  /* Reserved for a future bounded-latency implementation. */
  LGMP_STREAM_REALTIME_FIFO = 2
};

enum LGMPStreamNotifyReason
{
  /* The producer published one or more records. */
  LGMP_STREAM_NOTIFY_DATA    = 1U << 0,
  /* The consumer returned one or more slots. */
  LGMP_STREAM_NOTIFY_CREDIT  = 1U << 1,
  /* The host changed the binding state or epoch. */
  LGMP_STREAM_NOTIFY_BINDING = 1U << 2
};

/*
 * A native-endian, fixed-size description of one unidirectional stream.
 * Applications may copy this structure into their own LGMP control protocol.
 * The receiver must pass the complete structure to lgmpClientStreamAttach;
 * it must never construct pointers from these fields itself.
 */
struct LGMPStreamDescriptor
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t offset;
  uint32_t regionSize;
  uint32_t direction;
  uint32_t policy;
  uint32_t slotCount;
  uint32_t slotSize;
};

struct LGMPStreamConfig
{
  uint32_t direction;
  uint32_t policy;
  uint32_t slotCount;
  uint32_t slotSize;
};

/*
 * A reservation is valid until committed, released, or cancelled. Only one
 * write reservation and one read reservation may be outstanding per handle.
 * Applications must treat the underscore-prefixed members as opaque.
 */
typedef struct LGMPStreamBuffer
{
  void    * data;
  uint32_t capacity;
  uint32_t size;
  uint64_t _ticket;
  uint32_t _epoch;
}
LGMPStreamBuffer;

/*
 * Adaptive polling is the mandatory fallback for stream transports whose
 * peer notification is unavailable, coalesced, or lost. The state is local;
 * it is never placed in shared memory and performs no allocation.
 *
 * A caller should invoke lgmpStreamPollActivity after making progress or
 * after its wait primitive reports a peer notification. When an operation
 * returns LGMP_ERR_STREAM_EMPTY or LGMP_ERR_STREAM_FULL, call
 * lgmpStreamPollIdle and wait for at most the returned number of
 * microseconds. A zero result requests another immediate attempt.
 *
 * The state is owned by the polling thread. A notifier running on another
 * thread should wake that thread rather than modify the state directly.
 */
struct LGMPStreamPollConfig
{
  uint32_t spinCount;
  uint32_t minWaitUs;
  uint32_t maxWaitUs;
};

typedef struct LGMPStreamPollState
{
  struct LGMPStreamPollConfig _config;
  uint32_t                    _spinRemaining;
  uint32_t                    _nextWaitUs;
}
LGMPStreamPollState;

LGMP_STATUS lgmpStreamPollInit(LGMPStreamPollState * state,
    const struct LGMPStreamPollConfig config);
void lgmpStreamPollActivity(LGMPStreamPollState * state);
uint32_t lgmpStreamPollIdle(LGMPStreamPollState * state);

/*
 * The notifier is an advisory request to wake the peer after the associated
 * shared-memory publication is visible. It may be coalesced or lost by the
 * transport, must not block, and must not be the only way the peer makes
 * progress. Callers must retain a bounded polling fallback.
 *
 * Installing or replacing a notifier must be serialized with operations on
 * the same local stream handle. The callback may inspect the descriptor but
 * must not re-enter that handle.
 */
typedef void (*LGMPStreamNotifyFn)(void * opaque,
    const struct LGMPStreamDescriptor * descriptor, uint32_t reasons);

LGMP_STATUS lgmpHostStreamNew(PLGMPHost host,
    const struct LGMPStreamConfig config, PLGMPHostStream * result);
void lgmpHostStreamFree(PLGMPHostStream * stream);
void lgmpHostStreamGetDescriptor(PLGMPHostStream stream,
    struct LGMPStreamDescriptor * descriptor);
/* Bind and unbind calls for a given stream must be serialized by the host. */
LGMP_STATUS lgmpHostStreamBind(PLGMPHostStream stream, uint32_t clientID,
    uint32_t * epoch);
/*
 * Graceful unbind enters draining and returns LGMP_ERR_STREAM_BUSY until all
 * published records have been consumed and all reservations are released.
 */
LGMP_STATUS lgmpHostStreamUnbind(PLGMPHostStream stream);
/*
 * Force reset is valid only after the bound peer has disconnected or the
 * containing LGMP session is being torn down. It discards in-flight data.
 */
LGMP_STATUS lgmpHostStreamForceUnbind(PLGMPHostStream stream);
LGMP_STATUS lgmpHostStreamGetBinding(PLGMPHostStream stream,
    uint32_t * clientID, uint32_t * epoch);
void lgmpHostStreamSetNotifier(PLGMPHostStream stream,
    LGMPStreamNotifyFn notifier, void * opaque);

LGMP_STATUS lgmpHostStreamWriteAcquire(PLGMPHostStream stream,
    LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpHostStreamWriteCommit(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer, uint32_t usedLength);
LGMP_STATUS lgmpHostStreamWriteCancel(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpHostStreamReadPeek(PLGMPHostStream stream,
    LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpHostStreamReadRelease(PLGMPHostStream stream,
    const LGMPStreamBuffer * buffer);

LGMP_STATUS lgmpClientStreamAttach(PLGMPClient client,
    const struct LGMPStreamDescriptor * descriptor,
    PLGMPClientStream * result);
/*
 * Attach validates the descriptor and may return an inactive handle while the
 * stream is unbound. Activate latches only a binding owned by this LGMP client
 * and must be called after the host acknowledges a later bind or rebind.
 */
LGMP_STATUS lgmpClientStreamActivate(PLGMPClientStream stream,
    uint32_t * epoch);
void lgmpClientStreamDetach(PLGMPClientStream * stream);
LGMP_STATUS lgmpClientStreamGetBinding(PLGMPClientStream stream,
    uint32_t * clientID, uint32_t * epoch);
void lgmpClientStreamSetNotifier(PLGMPClientStream stream,
    LGMPStreamNotifyFn notifier, void * opaque);

LGMP_STATUS lgmpClientStreamWriteAcquire(PLGMPClientStream stream,
    LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpClientStreamWriteCommit(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer, uint32_t usedLength);
LGMP_STATUS lgmpClientStreamWriteCancel(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpClientStreamReadPeek(PLGMPClientStream stream,
    LGMPStreamBuffer * buffer);
LGMP_STATUS lgmpClientStreamReadRelease(PLGMPClientStream stream,
    const LGMPStreamBuffer * buffer);

#ifdef __cplusplus
}
#endif

#endif
