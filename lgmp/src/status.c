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

#include "lgmp/status.h"

const char * lgmpStatusString(LGMP_STATUS status)
{
  switch(status)
  {
    case LGMP_OK                    : return "LGMP_OK";
    case LGMP_ERR_CLOCK_FAILURE     : return "LGMP_CLOCK_FAILURE";
    case LGMP_ERR_INVALID_ARGUMENT  : return "LGMP_ERR_INVALID_ARGUMENT";
    case LGMP_ERR_INVALID_SIZE      : return "LGMP_ERR_INVALID_SIZE";
    case LGMP_ERR_INVALID_ALIGNMENT : return "LGMP_ERR_INVALID_ALIGNMENT";
    case LGMP_ERR_INVALID_SESSION   : return "LGMP_ERR_INVALID_SESSION";
    case LGMP_ERR_NO_MEM            : return "LGMP_ERR_NO_MEM";
    case LGMP_ERR_NO_SHARED_MEM     : return "LGMP_ERR_NO_SHARED_MEM";
    case LGMP_ERR_HOST_STARTED      : return "LGMP_ERR_HOST_STARTED";
    case LGMP_ERR_NO_QUEUES         : return "LGMP_ERR_NO_QUEUES";
    case LGMP_ERR_QUEUE_FULL        : return "LGMP_ERR_QUEUE_FULL";
    case LGMP_ERR_QUEUE_EMPTY       : return "LGMP_ERR_QUEUE_EMPTY";
    case LGMP_ERR_QUEUE_UNSUBSCRIBED: return "LGMP_ERR_QUEUE_UNSUBSCRIBED";
    case LGMP_ERR_QUEUE_TIMEOUT     : return "LGMP_ERR_QUEUE_TIMEOUT";
    case LGMP_ERR_INVALID_MAGIC     : return "LGMP_ERR_INVALID_MAGIC";
    case LGMP_ERR_INVALID_VERSION   : return "LGMP_ERR_INVALID_VERSION";
    case LGMP_ERR_NO_SUCH_QUEUE     : return "LGMP_ERR_NO_SUCH_QUEUE";
  }
  return "Invalid status!";
}