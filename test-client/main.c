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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>

#include "lgmp/client.h"

void * ram;
#define SHARED_FILE "/dev/shm/lgmp-test"
#define RAM_SIZE (10*1048576)

int main(int argc, char * argv[])
{
  unsigned int delay = 50;
  if (argc > 1)
    delay = atoi(argv[1]) * 1000;

  int fd = open(SHARED_FILE, O_RDWR, (mode_t)0600);
  if (fd < 0)
  {
    perror("open failed");
    return -1;
  }

  ram = mmap(0, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (!ram)
  {
    perror("mmap failed");
    goto out_close;
  }

  PLGMPClient client;
  LGMP_STATUS status;
  while((status = lgmpClientInit(ram, RAM_SIZE, &client))
      != LGMP_OK)
  {
    printf("lgmpClientInit %s\n", lgmpStatusString(status));
    goto out_unmap;
  }

  uint32_t   udataSize;
  uint8_t  * udata;
  while((status = lgmpClientSessionInit(client, &udataSize, &udata)) != LGMP_OK)
  {
    usleep(100000);
    printf("lgmpClientSessionInit: %s\n", lgmpStatusString(status));
  }

  PLGMPClientQueue queue;
  while((status = lgmpClientSubscribe(client, 0, &queue)) != LGMP_OK)
  {
    if (status == LGMP_ERR_NO_SUCH_QUEUE)
      usleep(250000);
    else
    {
      printf("lgmpClientSubscrbe: %s\n", lgmpStatusString(status));
      goto out_lgmpclient;
    }
  }

  uint32_t lastCount = 0;
  while(lgmpClientSessionValid(client))
  {
    LGMPMessage msg;
    if((status = lgmpClientProcess(queue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
      {
        usleep(1);
        continue;
      }
      else
      {
        printf("lgmpClientProcess: %s\n", lgmpStatusString(status));
        goto out_unsub;
      }
    }

    if (delay)
      printf("Got %4u: %s\n", msg.udata, (char *)msg.mem);

    if (!lastCount)
      lastCount = msg.udata;
    else
    {
      if (lastCount != msg.udata - 1)
      {
        printf("MISSED MESSAGE\n");
        goto out_unsub;
      }
      lastCount = msg.udata;
    }

    if (delay)
      usleep(delay);

    status = lgmpClientMessageDone(queue);
    if (status != LGMP_OK)
      printf("lgmpClientMessageDone: %s\n", lgmpStatusString(status));
  }

  printf("Shutdown\n");

out_unsub:
  if (lgmpClientSessionValid(client))
    lgmpClientUnsubscribe(&queue);
out_lgmpclient:
  lgmpClientFree(&client);
out_unmap:
  munmap(ram, RAM_SIZE);
out_close:
  close(fd);
out:
  return 0;
}
