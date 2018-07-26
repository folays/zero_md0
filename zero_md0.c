/*
 *
 * Copyright (c) 2011
 * Eric Gouyer <folays@folays.net>
 *
 * Contributors
 * Nicolas Limage
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
#include <math.h>
#include <float.h>
#include "zero_md0.h"

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
static unsigned int mode_write = 0; /* 0 => read, 1 => write */
static unsigned int mode_sync = 0; /* 0 => [], 1 => O_SYNC */
static unsigned int mode_rnd = 0; /* 0 => sequential, 1 => random */
static unsigned long long int mydevsize; /* size of the block device in bytes */
static int myiosize = 0; /* io size */
static unsigned int mycount = 100; /* number of i/o to do */
static unsigned int maxinflight = 200; /* maximum # of aios in flight */
static unsigned int maxsubmit = 20; /* maximum # of aios to submit */
static unsigned int myruns = 100; /* number of runs */
static unsigned int opt_verbose = 0; /* verbose */

static unsigned int runid; /* # of aios dones */
static unsigned int copied; /* # of aios dones */
static unsigned int inflight; /* # of aios in flight */

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

  if (opt_verbose > 1 && copied % 100 == 0)
    printf("done copy: inflight [%u/%u] copied [%u/%u]\n",
      inflight, maxinflight, copied, mycount);

  if (inflight == 0 && copied < mycount)
    printf("warning: buffer underrun (0 inflight i/o)\n");
}

void loop_aio(int fd, void *buf)
{
  int res;
  unsigned int elapsed;

  /* i/o context initialization */
  io_context_t myctx;
  memset(&myctx, 0, sizeof(myctx));
  if ((res = io_queue_init(maxinflight, &myctx)))
    io_error("io_queue_init", res);

  copied = 0;
  inflight = 0;

  if (opt_verbose)
    printf("[run %d] start\n", runid);

  while (copied < mycount)
  {
    struct iocb *ioq[maxsubmit];
    int tosubmit = 0;
    unsigned long long int index;
    struct iocb *iocb;
    struct timeval tv1, tv2;

    /* filling a context with io queries */
    while (copied + inflight + tosubmit < mycount &&
      inflight + tosubmit < maxinflight &&
      tosubmit < maxsubmit)
    {
      /* Simultaneous asynchronous operations using the same iocb produce undefined results. */
      iocb = calloc(1, sizeof(struct iocb));

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
      if (opt_verbose > 1)
	printf("io_submit: inflight [%u/%u] copied [%u/%u] tosubmit [%d]\n",
	       inflight, maxinflight, copied, mycount, tosubmit);
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
        printf("warning: io_submit() took %d ms, this is suspicious, maybe nr_request is too low.\n", elapsed);

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

  io_queue_release(myctx);
}

static struct option long_options[] = {
  {"dev", required_argument, NULL, 'd'},
  {"iosize", required_argument, NULL, 's'},
  {"write", no_argument, NULL, 'w'},
  {"sync", no_argument, NULL, 'y'},
  {"random", no_argument, NULL, 'r'},
  {"iocount", required_argument, NULL, 'c'},
  {"maxsubmit", required_argument, NULL, 'b'},
  {"maxinflight", required_argument, NULL, 'f'},
  {"runs", required_argument, NULL, 'n'},
  {"verbose", optional_argument, NULL, 'v'},
  {NULL, 0, NULL, 0},
};

int main_getopt(int argc, char **argv)
{
  int c;

  while ((c = getopt_long(argc, argv, "d:s:wyrc:b:f:n:v::", long_options, NULL)) != -1)
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
      case 'y':
        mode_sync = 1;
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
      case 'v':
        opt_verbose += 1;
        break;
      case 'n':
        myruns = atoi(optarg);
        if (myruns == 0)
        {
          fprintf(stderr, "Invalid maximum runs: %s\n", optarg);
          return 1;
        }
        break;
    }
  }
  return 0;
}

int usage(char *s)
{
  fprintf(stderr, "Usage: %s <--dev=/dev/xxx> [--write] [--random] \
[--iosize=x (kB)] [--iocount=x] [--runs=x] [--verbose] \
[--maxsubmit=x] [--maxinflight=x]\n", s);
  return 2;
}

/*
 * variance-related functions
 * http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm
 */

void init_variance(variance_t *variance)
{
  variance->n = 0;
  variance->mean = 0;
  variance->m2 = 0;
}

void update_variance(variance_t *variance, double value)
{
  double delta;

  variance->n += 1;
  delta = value - variance->mean;
  variance->mean = variance->mean + delta / variance->n;
  variance->m2 = variance->m2 + delta * (value - variance->mean);
}

double get_variance(const variance_t *variance)
{
  return variance->m2 / (variance->n - 1);
}

double get_stddev(const variance_t *variance)
{
  return sqrt(get_variance(variance));
}

void update_minmax(const double *cur, double *min, double *max)
{
    if (*cur < *min)
      *min = *cur;
    else if (*cur > *max)
      *max = *cur;
}

int main(int argc, char **argv)
{
  struct timeval tv_start, tv_end;
  unsigned int elapsed;
  unsigned int stat_time = 0, stat_iocount = 0;
  double lat_cur, lat_min = DBL_MAX, lat_max = 0;
  double bw_cur, bw_min = DBL_MAX, bw_max = 0;
  double iops_cur, iops_min = DBL_MAX, iops_max = 0;
  variance_t lat_variance, bw_variance, iops_variance;

  if (main_getopt(argc, argv))
    return 1;

  if (!mydev)
    return usage(argv[0]);

  /* CPU affinity selection */
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(0, &mask);
  sched_setaffinity(0, sizeof(mask), &mask);

  /* opening device */
  printf("Using device: %s\n", mydev);
  int fd = open(mydev, O_RDWR | O_DIRECT | (mode_sync ? O_SYNC : 0));
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

  printf("Performing %s %s %s test, %d runs\n",
    mode_rnd ? "random " : "sequential",
    mode_write ? "write" : "read",
    mode_sync ? "sync" : "nosync",
    myruns);
  printf("Will perform %u x %u x %ukB IOs\n",
    myruns, mycount, myiosize / 1024);
  printf("Maximum IO submit / inflight: %d / %d\n",
    maxsubmit, maxinflight);
  printf("Verbosity : %d\n", opt_verbose);

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

  init_variance(&lat_variance);
  init_variance(&bw_variance);
  init_variance(&iops_variance);

  /* start X runs and collect stats */
  for (runid = 0; runid < myruns; ++runid)
  {
    gettimeofday(&tv_start, NULL);
    loop_aio(fd, buf);
    gettimeofday(&tv_end, NULL);

    /* update stats */
    elapsed = (tv_end.tv_sec - tv_start.tv_sec) * 1000 + (tv_end.tv_usec - tv_start.tv_usec) / 1000;

    stat_time += elapsed;
    stat_iocount += mycount;

    lat_cur = (double)elapsed / mycount;
    bw_cur = (double)myiosize * mycount / elapsed * 1000 / (1024 * 1024);
    iops_cur = (double)mycount / elapsed * 1000;

    update_minmax(&lat_cur, &lat_min, &lat_max);
    update_minmax(&bw_cur, &bw_min, &bw_max);
    update_minmax(&iops_cur, &iops_min, &iops_max);

    update_variance(&lat_variance, lat_cur);
    update_variance(&bw_variance, bw_cur);
    update_variance(&iops_variance, iops_cur);

    if (opt_verbose)
    {
      printf("[run %d] done in %d ms\n", runid, stat_time);
      printf("[run %d] iop/s: %.3lf\n", runid, iops_cur);
      printf("[run %d] avg bandwidth: %.3lf MB/s\n", runid, bw_cur);
      printf("[run %d] avg latency: %.3lf ms\n", runid, lat_cur);
    }
  }

  printf("all jobs done in %d ms\n", stat_time);
  printf("total data size: %lld MB\n",
    (unsigned long long)myiosize * stat_iocount / 1024 / 1024);
  printf("total %s io: %u\n", mode_write ? "write" : "read", stat_iocount);
  printf("average iop/s: %.2lf (min/max/stddev=%.2lf/%.2lf/%.2lf)\n",
    (double)stat_iocount / stat_time * 1000,
    iops_min, iops_max, get_stddev(&iops_variance));
  printf("average latency: %.3lf ms (min/max/stddev=%.2lf/%.2lf/%.2lf)\n",
    (double)stat_time / stat_iocount, lat_min, lat_max,
    get_stddev(&lat_variance));
  printf("average bandwidth: %.3lf MB/s %.3lf Mb/s) (min/max/stddev=%.2lf/%.2lf/%.2lf)\n",
    (double)myiosize * stat_iocount / stat_time * 1000 / (1024 * 1024),
    (double)myiosize * stat_iocount / stat_time * 1000 / (1024 * 1024) * 8,
    bw_min, bw_max, get_stddev(&bw_variance));

  return 0;
}
