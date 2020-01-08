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

LGMP_STATUS lgmpClientInit(void * mem, const size_t size, LGMPClient * result);
void        lgmpClientFree(LGMPClient * client);
bool        lgmpClientSessionValid(LGMPClient client);

LGMP_STATUS lgmpClientSubscribe(LGMPClient client, uint32_t type, LGMPQueue * result);
LGMP_STATUS lgmpClientUnsubscribe(LGMPQueue * result);

typedef struct
{
  uint32_t   type;
  uint32_t   size;
  void     * mem;
}
LGMPMessage;

LGMP_STATUS lgmpClientProcess(LGMPQueue queue, LGMPMessage * result);
LGMP_STATUS lgmpClientMessageDone(LGMPQueue queue);