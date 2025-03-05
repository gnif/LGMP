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
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <signal.h>
#include <curses.h>
#include <string.h>

#include <sys/ioctl.h>
#include "../kvmfr.h"

#include "../../lgmp/src/headers.h"

bool running = true;

static void finish(int sig)
{
  running = false;
}

int main(int argc, char * argv[])
{
  const char * shmFile = NULL;
  bool error = false;

  int opt;
  while ((opt = getopt(argc, argv, "f:")) != -1) {
    switch(opt)
    {
      case 'f':
        shmFile = optarg;
        break;

      default:
        error = true;
        break;
    }
  }

  if (!shmFile || error)
  {
    fprintf(stderr, "Invalid usage, expected: -f /dev/shm/file\n");
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
    fd = open(shmFile, O_RDONLY);
  }

  if (!fd)
  {
    perror("open failed");
    exit(EXIT_FAILURE);
  }

  void * ram = mmap(0, devSize, PROT_READ, MAP_SHARED, fd, 0);
  if (!ram)
  {
    perror("mmap failed");
    goto out_close;
  }

  fprintf(stderr, "Mapped %s - %uMiB\n", shmFile, devSize / 1048576UL);

  signal(SIGINT, finish);
  initscr();

  while(running)
  {
    erase();
    struct LGMPHeader * header = (struct LGMPHeader *)ram;

    printw(
      "LGMPHeader\n"
      "  magic     = %08x\n"
      "  version   = %u\n"
      "  sessionID = %u\n"
      "  timestamp = %lu\n"
      "  numQueues = %u\n"
      "  udataSize = %u\n",
      header->magic,
      header->version,
      header->sessionID,
      atomic_load(&header->timestamp),
      header->numQueues,
      header->udataSize);

    for(int i = 0; i < header->numQueues; ++i)
    {
      struct LGMPHeaderQueue * hq        = &header->queues[i];
      struct LGMPHeaderMessage *messages = (struct LGMPHeaderMessage *)
        (ram + hq->messagesOffset);

      printw(
        "LGMPHeaderQueue(%d)\n"
        "  queueID        = 0x%08x\n"
        "  numMessages    = %u\n"
        "  maxTime        = %u\n"
        "  position       = %u\n"
        "  msgTimeout     = %u\n"
        "  messagesOffset = 0x%08x\n"
        "  cMsgWPos       = %u\n"
        "  cMsgAvail      = %u\n",
        i,
        hq->queueID,
        hq->numMessages,
        hq->maxTime,
        atomic_load(&hq->position),
        atomic_load(&hq->msgTimeout),
        hq->messagesOffset,
        hq->cMsgWPos,
        hq->cMsgAvail);

      for(int i = 0; i < 32; ++i)
        printw("  client %-2d     = id:%x, timeout:%u\n",
          i,
          hq->clientID[i],
          hq->timeout[i]);

      printw(
        "  subs           = 0x%016lx\n"
        "  start          = %u\n"
        "  count          = %u\n",
        atomic_load(&hq->subs),
        hq->start,
        atomic_load(&hq->count));
    }

    refresh();
  }

  endwin();
  munmap(ram, devSize);
out_close:
  close(fd);
out:
  return 0;
}
