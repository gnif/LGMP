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

#ifndef LGMP_LGMP_H
#define LGMP_LGMP_H

// this MUST match the size defined in `src/headers.h`
#define LGMP_MSGS_SIZE 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LGMPHost        * PLGMPHost;
typedef struct LGMPClient      * PLGMPClient;
typedef struct LGMPHostQueue   * PLGMPHostQueue;
typedef struct LGMPClientQueue * PLGMPClientQueue;
typedef struct LGMPMemory      * PLGMPMemory;

#ifdef __cplusplus
}
#endif

#endif
