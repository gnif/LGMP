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

#include "lgmp/host.h"

void * ram;
#define SHARED_FILE "/dev/shm/lgmp-test"
#define RAM_SIZE (10*1048576)

int main(int argc, char * argv[])
{
  int fd = open(SHARED_FILE, O_RDWR, (mode_t)0600);
  if (fd < 0)
  {
    printf("Failed to open %s", SHARED_FILE);
    return -1;
  }

  ram = mmap(0, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (!ram)
  {
    printf("Failed to mmap\n");
    goto out_close;
  }

  PLGMPHost host;
  if (lgmpHostInit(ram, RAM_SIZE, &host) != LGMP_OK)
  {
    printf("lgmpHostInit failed\n");
    goto out_unmap;
  }

  PLGMPQueue queue;
  if (lgmpHostAddQueue(host, 0, 10, &queue) != LGMP_OK)
  {
    printf("lgmpHostAddQueue failed\n");
    goto out_lgmphost;
  }

  PLGMPMemory mem[10] = { 0 };
  for(int i = 0; i < 10; ++i)
  {
    if (lgmpHostMemAlloc(host, 1024, &mem[i]) != LGMP_OK)
    {
      printf("lgmpHostAlloc failed\n");
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
  uint32_t count = 0;

  while(true)
  {
    while(lgmpHostPost(queue, count, mem[count % 10]) != LGMP_ERR_QUEUE_FULL)
      ++count;

    if (lgmpHostProcess(host) != LGMP_OK)
      break;

//    usleep(1);
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