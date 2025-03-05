/**
 * LGMP - Looking Glass Memory Protocol
 * Copyright © 2020-2025 Geoffrey McRae <geoff@hostfission.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include "../kvmfr.h"

#include "lgmp/client.h"
#include "../../lgmp/src/lgmp.h"

void * ram;

int main(int argc, char * argv[])
{
  unsigned int delay = 50;
  const char * shmFile = NULL;
  bool error = false;

  int opt;
  while ((opt = getopt(argc, argv, "f:d:")) != -1) {
    switch(opt)
    {
      case 'f':
        shmFile = optarg;
        break;

      case 'd':
        delay = atoi(optarg) * 1000;
        break;

      default:
        error = true;
        break;
    }
  }

  srand(lgmpGetClockMS());

  if (!shmFile || error)
  {
    fprintf(stderr, "Invalid usage, expected: -f /dev/shm/file -d N\n");
    exit(EXIT_FAILURE);
  }

  int fd;
  bool dmabuf = false;
  unsigned devSize;

  if (strlen(shmFile) > 8 && memcmp(shmFile, "/dev/kvmfr", 10) == 0)
  {
    dmabuf = true;
    fd = open(shmFile, O_RDWR, (mode_t)0600);

    // get the device size
    devSize = ioctl(fd, KVMFR_DMABUF_GETSIZE, 0);
  }
  else
  {
    struct stat st;
    if (stat(shmFile, &st) != 0)
    {
      perror("stat of shmFile failed");
      exit(EXIT_FAILURE);
    }
    devSize = st.st_size;
    fd = open(shmFile, O_RDWR);
  }

  if (!fd)
  {
    perror("open failed");
    exit(EXIT_FAILURE);
  }

  void * ram = mmap(0, devSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (!ram)
  {
    perror("mmap failed");
    goto out_close;
  }

  PLGMPClient client;
  LGMP_STATUS status;
  while((status = lgmpClientInit(ram, devSize, &client))
      != LGMP_OK)
  {
    printf("lgmpClientInit %s\n", lgmpStatusString(status));
    goto out_unmap;
  }

  uint32_t   udataSize;
  uint8_t  * udata;
  uint32_t   clientID;
  while((status = lgmpClientSessionInit(client, &udataSize, &udata, &clientID))
      != LGMP_OK)
  {
    usleep(100000);
    printf("lgmpClientSessionInit: %s\n", lgmpStatusString(status));
  }

  printf("Session valid, clientID: %x\n", clientID);

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

  uint32_t serial;
  bool dataDone = true;

#if 0
  uint8_t data[32];
  for(int i = 0; i < 20; ++i)
  {
    if ((status = lgmpClientSendData(queue, data, sizeof(data), &serial)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_FULL)
      {
        --i;
        continue;
      }

      printf("lgmpClientSendData: %s\n", lgmpStatusString(status));
      goto out_lgmpclient;
    }
  }
#endif

  uint32_t lastCount = 0;
  unsigned int msgCount = 0;
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

    printf("message %u\n", ++msgCount);

    if (delay)
      printf("Got %4u: %s\n", msg.udata, (char *)msg.mem);

#if 0
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
#endif

    if (delay)
      usleep(delay);

    status = lgmpClientMessageDone(queue);
    if (status != LGMP_OK)
      printf("lgmpClientMessageDone: %s\n", lgmpStatusString(status));

    if (!dataDone)
    {
      uint32_t hostSerial;
      lgmpClientGetSerial(queue, &hostSerial);
      printf("serial %u - hostSerial %u\n", serial, hostSerial);
      if (hostSerial >= serial)
      {
        dataDone = true;
        printf("data done\n");
        goto out_unsub;
      }
    }
  }

  printf("Shutdown\n");

out_unsub:
  if (lgmpClientSessionValid(client))
    lgmpClientUnsubscribe(&queue);
out_lgmpclient:
  lgmpClientFree(&client);
out_unmap:
  munmap(ram, devSize);
out_close:
  close(fd);
out:
  return 0;
}
