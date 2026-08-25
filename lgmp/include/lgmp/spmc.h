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

#ifndef LGMP_SPMC_H
#define LGMP_SPMC_H

#include <stdint.h>

#include "lgmp.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LGMP_SPMC_DESCRIPTOR_MAGIC   0x434d5053U
#define LGMP_SPMC_DESCRIPTOR_VERSION 1U

enum LGMPSPMCReaderBinding
{
  LGMP_SPMC_READER_UNBOUND  = 0,
  LGMP_SPMC_READER_BINDING  = 1,
  LGMP_SPMC_READER_READY    = 2,
  LGMP_SPMC_READER_DRAINING = 3
};

/*
 * A native-endian, fixed-size description of a single-producer,
 * multiple-consumer region. Applications may copy this structure into their
 * own LGMP control protocol. The receiver must pass the complete structure to
 * lgmpClientSPMCAttach and must never construct pointers from it itself.
 */
struct LGMPSPMCDescriptor
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t offset;
  uint32_t regionSize;
  uint32_t slotCount;
  uint32_t slotSize;
  uint32_t maxReaders;
  uint32_t reserved;
};

struct LGMPSPMCConfig
{
  uint32_t slotCount;
  uint32_t slotSize;
  uint32_t maxReaders;
};

/* Only one host write reservation may be outstanding per handle. */
typedef struct LGMPSPMCBuffer
{
  void    * data;
  uint32_t capacity;
  uint32_t _generation;
  uint64_t sequence;
}
LGMPSPMCBuffer;

/* Read consumes one record by copying it into caller-owned storage. */
struct LGMPSPMCRecord
{
  uint64_t sequence;
  uint64_t skipped;
  uint32_t size;
  uint32_t reserved;
};

/* A host snapshot used to apply application-selected reader policy. */
struct LGMPSPMCReaderState
{
  uint32_t readerID;
  uint32_t state;
  uint32_t clientID;
  uint32_t epoch;
  uint64_t producerSequence;
  uint64_t consumerSequence;
};

LGMP_STATUS lgmpHostSPMCNew(PLGMPHost host,
    const struct LGMPSPMCConfig config, PLGMPHostSPMC * result);
void lgmpHostSPMCFree(PLGMPHostSPMC * stream);
void lgmpHostSPMCGetDescriptor(PLGMPHostSPMC stream,
    struct LGMPSPMCDescriptor * descriptor);

/* Reader binding calls for a given stream must be serialized by the host. */
LGMP_STATUS lgmpHostSPMCReaderBind(PLGMPHostSPMC stream, uint32_t clientID,
    uint32_t * readerID, uint32_t * epoch);
LGMP_STATUS lgmpHostSPMCReaderUnbind(PLGMPHostSPMC stream,
    uint32_t readerID);
/* Force unbind is valid only after the client has disconnected. */
LGMP_STATUS lgmpHostSPMCReaderForceUnbind(PLGMPHostSPMC stream,
    uint32_t readerID);
LGMP_STATUS lgmpHostSPMCReaderGetState(PLGMPHostSPMC stream,
    uint32_t readerID, struct LGMPSPMCReaderState * state);

/* Publish calls for a stream must be serialized by its sole producer. */
LGMP_STATUS lgmpHostSPMCWriteAcquire(PLGMPHostSPMC stream,
    LGMPSPMCBuffer * buffer);
LGMP_STATUS lgmpHostSPMCWriteCommit(PLGMPHostSPMC stream,
    const LGMPSPMCBuffer * buffer, uint32_t usedLength);
/* Cancel retires the reserved sequence as a reader-visible gap. */
LGMP_STATUS lgmpHostSPMCWriteCancel(PLGMPHostSPMC stream,
    const LGMPSPMCBuffer * buffer);
LGMP_STATUS lgmpHostSPMCPublish(PLGMPHostSPMC stream, const void * data,
    uint32_t size, uint64_t * sequence);
LGMP_STATUS lgmpHostSPMCPublishV(PLGMPHostSPMC stream,
    const void * first, uint32_t firstSize,
    const void * second, uint32_t secondSize, uint64_t * sequence);

LGMP_STATUS lgmpClientSPMCAttach(PLGMPClient client,
    const struct LGMPSPMCDescriptor * descriptor, uint32_t readerID,
    PLGMPClientSPMC * result);
LGMP_STATUS lgmpClientSPMCActivate(PLGMPClientSPMC stream,
    uint32_t * epoch);
void lgmpClientSPMCDetach(PLGMPClientSPMC * stream);
LGMP_STATUS lgmpClientSPMCGetBinding(PLGMPClientSPMC stream,
    uint32_t * clientID, uint32_t * epoch);

/* Advance this reader to the current producer head without reading data. */
LGMP_STATUS lgmpClientSPMCSync(PLGMPClientSPMC stream,
    uint64_t * skipped);
LGMP_STATUS lgmpClientSPMCRead(PLGMPClientSPMC stream, void * data,
    uint32_t capacity, struct LGMPSPMCRecord * record);

#ifdef __cplusplus
}
#endif

#endif
