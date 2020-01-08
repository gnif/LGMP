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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lgmp/lgmp.h"
#include "status.h"

LGMP_STATUS lgmpHostInit(void *mem, const size_t size, LGMPHost * result);
void lgmpHostFree(LGMPHost * host);

LGMP_STATUS lgmpHostAddQueue(LGMPHost host, uint32_t type, uint32_t numMessages, LGMPQueue * result);
LGMP_STATUS lgmpHostProcess(LGMPHost host);

/**
 * Allocates some RAM for application use from the shared memory
 *
 * Note: These allocations are permanant! Calling lgmpHostMemFree only frees
 * the LGMPMemory structure, but does not recover the shared memory for later
 * use.
 */
LGMP_STATUS lgmpHostMemAlloc(LGMPHost host, uint32_t size, LGMPMemory * result);
void        lgmpHostMemFree (LGMPMemory * mem);
void *      lgmpHostMemPtr  (LGMPMemory mem);

LGMP_STATUS lgmpHostPost(LGMPQueue queue, uint32_t type, LGMPMemory payload);