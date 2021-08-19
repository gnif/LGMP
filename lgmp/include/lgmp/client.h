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

#ifndef LGMP_CLIENT_H
#define LGMP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lgmp.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

LGMP_STATUS lgmpClientInit(void * mem, const size_t size, PLGMPClient * result);
void        lgmpClientFree(PLGMPClient * client);
LGMP_STATUS lgmpClientSessionInit(PLGMPClient client, uint32_t * udataSize,
    uint8_t ** udata);
bool        lgmpClientSessionValid(PLGMPClient client);

LGMP_STATUS lgmpClientSubscribe(PLGMPClient client, uint32_t queueID,
    PLGMPClientQueue * result);
LGMP_STATUS lgmpClientUnsubscribe(PLGMPClientQueue * result);

typedef struct
{
  uint32_t   udata;
  uint32_t   size;
  void     * mem;
}
LGMPMessage, * PLGMPMessage;

LGMP_STATUS lgmpClientAdvanceToLast(PLGMPClientQueue queue);
LGMP_STATUS lgmpClientProcess(PLGMPClientQueue queue, PLGMPMessage result);
LGMP_STATUS lgmpClientMessageDone(PLGMPClientQueue queue);

// send data to the host of up to LGMP_MSGS_SIZE in size
// serial is set to the seral number of the message just added if successful
LGMP_STATUS lgmpClientSendData(PLGMPClientQueue queue, const void * data,
    size_t size, uint32_t * serial);

// get the last serial processed by the host
// this can be used to determine if data messages have been processed by the host
LGMP_STATUS lgmpClientGetSerial(PLGMPClientQueue queue, uint32_t * serial);

#ifdef __cplusplus
}
#endif

#endif
