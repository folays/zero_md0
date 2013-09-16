/*
 *
 * Copyright (c) 2011
 * Eric Gouyer <folays@folays.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <libaio.h>
#include <linux/fs.h>
#include <linux/raw.h>
#include <getopt.h>
#include <err.h>

/*
 * http://linux.derkeiler.com/Mailing-Lists/Kernel/2006-11/msg00966.html
 *
 * variable secrete : /sys/devices/virtual/block/md0/md/stripe_cache_size
 */

/*
 * XXX : you NEED to check that nr_request is sufficiently sized for maxinflight,
 * otherwise io_submit() would block. Not bad for a dumb test, but still not clean.
 */

static char *mydev = 0; /* device to be used */
static int mode_write = 0; /* 0 => read, 1 => write */
static int mode_rnd = 0; /* 0 => sequential, 1 => random */
static unsigned long long int mydevsize; /* size of the block device in bytes */
static int myiosize = 0; /* optimal_io_size */
static unsigned long long int mycount = 10000; /* number of i/o to do */
static int copied; /* # of aios dones */
static int inflight; /* # of aios in flight */
static int maxinflight = 200; /* maximum # of aios in flight */
static int maxsubmit = 10; /* maximum # of aios to submit */

struct timeval tv_temp;
static int copied_temp = 0;

/* Fatal error handler */
static void io_error(const char *func, int rc)
{
  if (rc == -ENOSYS)
    fprintf(stderr, "AIO not in this kernel\n");
  else
    fprintf(stderr, "%s: %s\n", func, strerror(-rc));

  exit(1);
}

/*
 * XXX : wtf after 2 Go fail, at least res2... something is going on
 * with those two long res/res2 while is iocb contains long long...
 */

static void io_done(io_context_t ctx, struct iocb *iocb, long res, long res2)
{
  struct timeval tv_now;
  double elapsed, avgbw;

  if (res2 != 0)
    io_error("aio ", res2);

  if (res != iocb->u.c.nbytes)
  {
    printf("aio missed bytes expect %lu got %ld at off %lld\n", iocb->u.c.nbytes, res2, iocb->u.c.offset);
    exit(1);
  }

  --inflight;
  ++copied;
  free(iocb);

  /* stats */
  gettimeofday(&tv_now, NULL);
  elapsed = (tv_now.tv_sec - tv_temp.tv_sec) * 1000 + (tv_now.tv_usec - tv_temp.tv_usec) / 1000;
  avgbw = (double)myiosize * copied / elapsed * 1000 / (1024 * 1024);
  printf("debug: ms=%lfms bw=%lf\n", elapsed, avgbw);
  /* memcpy(&tv_temp, &tv_now, sizeof(tv_temp));
  copied_temp = copied; */

  if (copied % 100 == 0)
    printf("done copy: inflight [%d/%d] copied [%d/%llu]\n",
      inflight, maxinflight, copied, mycount);

  if (inflight == 0 && copied < mycount)
    printf("warning: buffer underrun (0 inflight i/o)\n");
}

void loop_aio(int fd, void *buf)
{
  struct timeval tv_start, tv_end;
  int res;

  /* i/o context initialization */
  io_context_t myctx;
  memset(&myctx, 0, sizeof(myctx));
  if ((res = io_queue_init(maxinflight, &myctx)))
    io_error("io_queue_init", res);

  gettimeofday(&tv_start, NULL);
  memcpy(&tv_temp, &tv_start, sizeof(tv_temp));

  while (copied < mycount)
  {
    struct iocb *ioq[maxsubmit];
    int tosubmit = 0;
    unsigned long long int index;
    struct iocb *iocb;
    struct timeval tv1, tv2;
    int elapsed;

    /* filling a context with io queries */
    while (copied + inflight + tosubmit < mycount &&
      inflight + tosubmit < maxinflight &&
      tosubmit < maxsubmit)
    {
      /* Simultaneous asynchronous operations using the same iocb produce undefined results. */
      /*iocb = calloc(1, sizeof(*iocb));*/
      iocb = malloc(sizeof(struct iocb));

      if (mode_rnd)
        index = (mydevsize / myiosize) * random() / RAND_MAX;
      else
        index = copied + inflight + tosubmit;

      if (mode_write)
        io_prep_pwrite(iocb, fd, buf, myiosize, index * myiosize);
      else
        io_prep_pread(iocb, fd, buf, myiosize, index * myiosize);

      io_set_callback(iocb, io_done);
      ioq[tosubmit] = iocb;
      tosubmit += 1;
    }

    /* if there are available slots for submitting queries, do it */
    if (tosubmit)
    {
      /* submit io and check elapsed time */
      gettimeofday(&tv1, NULL);
      if ((res = io_submit(myctx, tosubmit, ioq)) != tosubmit)
      {
        printf("only %d io submitted\n", res);
        io_error("io_submit write", res);
      }
      gettimeofday(&tv2, NULL);
      elapsed = (tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec - tv1.tv_usec) / 1000;
      if (elapsed > 200)
        printf("\e[33mwarning\e[0m: io_submit() took %d ms, this is suspicious, maybe nr_request is too low.\n", elapsed);

      /* inflight io += newly submitted io */
      inflight += tosubmit;
    }

    /* handle completed io events */
    if ((res = io_queue_run(myctx)) < 0)
      io_error("io_queue_run", res);

    if (inflight == maxinflight ||
      (inflight && copied + inflight == mycount))
    {
      struct io_event event;
      if ((res = io_getevents(myctx, 1, 1, &event, NULL)) < 0)
        io_error("io_getevents", res);
      if (res != 1)
        errx(1, "no events?");
      ((io_callback_t)event.obj->data)(myctx, event.obj, event.res, event.res2);
    }
  }

  gettimeofday(&tv_end, NULL);
  io_queue_release(myctx);

  /* calculating statistics */
  int elapsed = (tv_end.tv_sec - tv_start.tv_sec) * 1000 + (tv_end.tv_usec - tv_start.tv_usec) / 1000;
  printf("all job done in %d ms\n", elapsed);
  printf("iop/s: %d\n", (int)((double)mycount / elapsed * 1000));
  printf("average lantency in ms: %.3f\n", (double)elapsed / mycount);
  printf("average bandwidth: %.3f MB/s (%.3f Mb/s)\n",
    (double)myiosize * mycount / elapsed * 1000 / (1024 * 1024),
    (double)myiosize * mycount / elapsed * 1000 / (1024 * 1024) * 8);
  printf("total size in MB: %lld\n", (unsigned long long)myiosize * mycount / 1024 / 1024);
}

static struct option long_options[] = {
  {"dev", required_argument, NULL, 'd'},
  {"iosize", required_argument, NULL, 's'},
  {"write", no_argument, NULL, 'w'},
  {"iocount", required_argument, NULL, 'c'},
  {"random", no_argument, NULL, 'r'},
  {"maxsubmit", required_argument, NULL, 'b'},
  {"maxinflight", required_argument, NULL, 'f'},
  {NULL, 0, NULL, 0},
};

int main_getopt(int argc, char **argv)
{
  int c;

  while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1)
  {
    switch (c)
    {
      case 'd':
        mydev = strdup(optarg);
        break;
      case 's':
        myiosize = atoi(optarg);
        if (myiosize == 0)
        {
          fprintf(stderr, "Invalid io size: %s\n", optarg);
          return 1;
        }
        myiosize *= 1024;
        break;
      case 'w':
        mode_write = 1;
        break;
      case 'c':
        mycount = atoll(optarg);
        if (mycount == 0)
        {
          fprintf(stderr, "Invalid io count: %s\n", optarg);
          return 1;
        }
        break;
      case 'r':
        mode_rnd = 1;
        break;
      case 'b':
        maxsubmit = atoi(optarg);
        if (maxsubmit == 0)
        {
          fprintf(stderr, "Invalid maximum io submit: %s\n", optarg);
          return 1;
        }
        break;
      case 'f':
        maxinflight = atoi(optarg);
        if (maxinflight == 0)
        {
          fprintf(stderr, "Invalid maximum inflight io: %s\n", optarg);
          return 1;
        }
        break;
    }
  }
  return 0;
}

int usage(char *s)
{
  fprintf(stderr, "Usage: %s <--dev=/dev/xxx> [--write] [--iosize=x (kB)] [--iocount=x]\
[--random] [--maxsubmit=x] [--maxinflight=x]\n", s);
  return 2;
}

int main(int argc, char **argv)
{
  if (main_getopt(argc, argv))
    return 1;

  if (!mydev)
    return usage(argv[0]);

  /* CPU affinity selection */
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(0, &mask);
  sched_setaffinity(0, sizeof(mask), &mask);

  printf("Using device: %s\n", mydev);

  //printf("sizeof 8 bytes type : %lu\n", sizeof(long long int));
  /* opening device */
  int fd = open(mydev, O_RDWR | O_DIRECT);
  if (fd < 0)
    err(1, "open %s", mydev);

  /* detecting device size */
  ioctl(fd, BLKGETSIZE64, &mydevsize);
  printf("Device size: %llu\n", mydevsize);

  /* detecting optimal io size */
  if (!myiosize)
  {
    ioctl(fd, BLKIOOPT, &myiosize);

    if (!myiosize)
    {
      fprintf(stderr, "Please specify IO size\n");
      return 1;
    }
    else
      printf("Using auto-detected optimal_io_size for %s\n", mydev);
  }

  printf("Performing %s %s test\n", mode_rnd ? "random " : "sequential",  mode_write ? "write" : "read");
  printf("Will perform %llu x %lukB IOs\n", mycount, (long unsigned int)(myiosize / 1024));
  printf("Maximum IO submit / inflight: %d / %d\n", maxsubmit, maxinflight);
  //mycount = mydevsize / myiosize;

  /* advising kernel of future io pattern */
  if (mode_rnd)
    posix_fadvise(fd, 0, mydevsize, POSIX_FADV_RANDOM);
  else
    posix_fadvise(fd, 0, mydevsize, POSIX_FADV_SEQUENTIAL);

  /* initialize a buffer with random data for disk writes */
  void *buf = mmap(NULL, myiosize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!buf)
    err(1, "mmap");
  if ((uintptr_t)buf % 512 || (uintptr_t)buf % 4096)
    errx(1, "mmap is not aligned...");
  int i;
  for (i = 0; i < myiosize / sizeof(int); ++i)
    ((int *)buf)[i] = rand();

  loop_aio(fd, buf);

  return 0;
}
