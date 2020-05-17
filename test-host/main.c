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
#include <string.h>

#include "lgmp/host.h"

void * ram;
#define SHARED_FILE "/dev/shm/lgmp-test"
#define RAM_SIZE (10*1048576)

int main(int argc, char * argv[])
{
  int fd = open(SHARED_FILE, O_RDWR | O_CREAT, (mode_t)0600);
  if (fd < 0)
  {
    perror("open failed");
    return -1;
  }

  if (ftruncate(fd, RAM_SIZE) != 0)
  {
    perror("ftruncate failed");
    goto out_close;
  }

  ram = mmap(0, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (!ram)
  {
    perror("mmap failed");
    goto out_close;
  }

  PLGMPHost host;
  LGMP_STATUS status;

  uint8_t udata[32];
  memset(udata, 0xaa, sizeof(udata));

  if ((status = lgmpHostInit(ram, RAM_SIZE, &host, sizeof(udata), udata))
      != LGMP_OK)
  {
    printf("lgmpHostInit failed: %s\n", lgmpStatusString(status));
    goto out_unmap;
  }

  const struct LGMPQueueConfig conf =
  {
    .queueID     = 0,
    .numMessages = 10,
    .subTimeout  = 100
  };

  PLGMPHostQueue queue;
  if ((status = lgmpHostQueueNew(host, conf, &queue)) != LGMP_OK)
  {
    printf("lgmpHostQueueNew failed: %s\n", lgmpStatusString(status));
    goto out_lgmphost;
  }

  PLGMPMemory mem[10] = { 0 };
  for(int i = 0; i < 10; ++i)
  {
    if ((status = lgmpHostMemAlloc(host, 1024, &mem[i])) != LGMP_OK)
    {
      printf("lgmpHostAlloc failed: %s\n", lgmpStatusString(status));
      goto out_lgmphost;
    }
  }


  sprintf(lgmpHostMemPtr(mem[0]), "This is a test from the host application");
  sprintf(lgmpHostMemPtr(mem[1]), "With multiple buffers");
  sprintf(lgmpHostMemPtr(mem[2]), "Containing text");
  sprintf(lgmpHostMemPtr(mem[3]), "That might or might not be");
  sprintf(lgmpHostMemPtr(mem[4]), "interesting.");
  sprintf(lgmpHostMemPtr(mem[5]), "This is buffer number 6");
  sprintf(lgmpHostMemPtr(mem[6]), "Now number 7");
  sprintf(lgmpHostMemPtr(mem[7]), "And now number 8");
  sprintf(lgmpHostMemPtr(mem[8]), "Second last buffer");
  sprintf(lgmpHostMemPtr(mem[9]), "It's over!");

  uint32_t time = 0;
  uint32_t count = 0;

  while(true)
  {
    ++time;

    if((status = lgmpHostQueuePost(queue, count, mem[count % 10])) != LGMP_ERR_QUEUE_FULL)
      ++count;

    if (time % 1000 == 0)
    {
      if (lgmpHostProcess(host) != LGMP_OK)
      {
        printf("lgmpHostQueuePost Failed: %s\n", lgmpStatusString(status));
        break;
      }

      uint32_t newSubs;
      if ((newSubs = lgmpHostQueueNewSubs(queue)) > 0)
        printf("newSubs: %u\n", newSubs);
    }

    usleep(1000);
  }

  for(int i = 0; i < 10; ++i)
    lgmpHostMemFree(&mem[i]);

out_lgmphost:
  lgmpHostFree(&host);
out_unmap:
  munmap(ram, RAM_SIZE);
out_close:
  close(fd);
out:
  return 0;
}
