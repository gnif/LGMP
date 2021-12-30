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

#ifndef LGMP_STATUS_H
#define LGMP_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  LGMP_OK,
  LGMP_ERR_CLOCK_FAILURE,
  LGMP_ERR_INVALID_ARGUMENT,
  LGMP_ERR_INVALID_SIZE,
  LGMP_ERR_INVALID_ALIGNMENT,
  LGMP_ERR_INVALID_SESSION,
  LGMP_ERR_NO_MEM,
  LGMP_ERR_NO_SHARED_MEM,
  LGMP_ERR_HOST_STARTED,
  LGMP_ERR_NO_QUEUES,
  LGMP_ERR_QUEUE_FULL,
  LGMP_ERR_QUEUE_EMPTY,
  LGMP_ERR_QUEUE_UNSUBSCRIBED,
  LGMP_ERR_QUEUE_TIMEOUT,
  LGMP_ERR_INVALID_MAGIC,
  LGMP_ERR_INVALID_VERSION,
  LGMP_ERR_NO_SUCH_QUEUE,
  LGMP_ERR_CORRUPTED
}
LGMP_STATUS;

const char * lgmpStatusString(LGMP_STATUS status);

#ifdef __cplusplus
}
#endif

#endif
