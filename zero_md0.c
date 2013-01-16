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
#include <time.h>
#include <libaio.h>
#include <linux/fs.h>
#include <linux/raw.h>
#include <getopt.h>

/*
 * http://linux.derkeiler.com/Mailing-Lists/Kernel/2006-11/msg00966.html
 */

/*
 * variable secrete : /sys/devices/virtual/block/md0/md/stripe_cache_size
 */

//#define MYDEV "/dev/md2"
static char *mydev = 0;
static int mode_write = 0;
static int mode_rnd = 0;

static unsigned long long int mydevsize; /* size of the block device, in bytes */
static int myiosize = 0; /* optimal_io_size */
static unsigned long long int mycount = 10000; /* number of i/o to do */

/*
 * XXX : you NEED to check that nr_request is sufficiently sized for MY_AIO_FLIGHT_MAX,
 * otherwise io_submit() would block. Not bad for a dumb test, but still not clean.
 */
//#define MY_AIO_FLIGHT_MAX 200
//#define MY_AIO_SUBMIT_MAX 10
static int MY_AIO_FLIGHT_MAX = 200;
static int MY_AIO_SUBMIT_MAX = 10;
/* TODO : ok j'ai tout compris pour le sequentiel, il faut tester l'aleatoire maintenant */
static int copied; /* # of aios dones */
static int inflight; /* # of aios in flight */

/* Fatal error handler */
static void io_error(const char *func, int rc)
{
  if (rc == -ENOSYS)
    fprintf(stderr, "AIO not in this kernel\n");
  else
    fprintf(stderr, "%s: %s\n", func, strerror(-rc));

  exit(1);
}

/* XXX : wtf after 2 Go fail, at least res2... something is going on with those two long res/res2 while is iocb contains long long... */
static void io_done(io_context_t ctx, struct iocb *iocb, long res, long res2)
{
  if (res2 != 0) {
    io_error("aio ", res2);
  }
  if (res != iocb->u.c.nbytes) {
    printf("aio missed bytes expect %lu got %ld at off %lld\n", iocb->u.c.nbytes, res2, iocb->u.c.offset);
    exit(1);
  }
  --inflight;
  ++copied;
  free(iocb);
  /* printf("done at off %lld\n", iocb->u.c.offset); */
  /* printf("done copy : inflight [%d/%d] copied [%d/%d]\n", */
  /* 	 inflight, MY_AIO_FLIGHT_MAX, copied, mytocpy); */
}

int loop_write(int fd, void *addr)
{
  int i;

  for (i = 0; i < 1000000; ++i)
    {
      int nbytes = write(fd, addr, myiosize);
      if (nbytes != myiosize)
  	printf("%d bytes written\n", nbytes);
      /* printf("time %ld\n", time(NULL)); */
    }
}

void loop_aio(int fd, void *addr)
{
  struct timeval tv_start, tv_end;
  io_context_t myctx;
  memset(&myctx, 0, sizeof(myctx));
  int res, i;

  gettimeofday(&tv_start, NULL);
  if (res = io_queue_init(MY_AIO_FLIGHT_MAX, &myctx))
    io_error("io_queue_init", res);
  //printf("Will initiate %llu io\n", mycount);
  while (copied < mycount)
    {
      struct iocb *ioq[MY_AIO_FLIGHT_MAX];
      int nr = 0;

      while (copied + inflight + nr < mycount &&
	     inflight + nr < MY_AIO_FLIGHT_MAX &&
	     nr < MY_AIO_SUBMIT_MAX)
	{
	  /* Simultaneous asynchronous operations using the same iocb produce undefined results. */
	  struct iocb *iocb = calloc(1, sizeof(*iocb));
	  unsigned long long int index = copied + inflight + nr;
	  if (mode_rnd)
	    {
	      /* random io */
	      index = (mydevsize / myiosize) * random() / RAND_MAX;
	    }
	  if (0)
	    {
	      /* for raid0+5, align index to the first raid0, and then ensure perfect distribution */
	      index -= index % 2;
	      index += (copied + inflight + nr) % 2;
	    }
	  /* printf("index : %llu\n", index); */
	  if (mode_write)
	    io_prep_pwrite(iocb, fd, addr, myiosize, index * myiosize);
	  else
	    io_prep_pread(iocb, fd, addr, myiosize, index * myiosize);
	  io_set_callback(iocb, io_done);
	  ioq[nr++] = iocb;
	}
      if (nr)
	{
	  if (rand() % 100 == 0)
	    printf("<- io_submit %03d inflight [%03d/%03d] copied [%d/%lld] ...\n",
		   nr, inflight, MY_AIO_FLIGHT_MAX, copied, mycount);
	  struct timeval tv1, tv2;
	  gettimeofday(&tv1, NULL);
	  if ((res = io_submit(myctx, nr, ioq)) != nr)
	    {
	      printf("only %d io submitted\n", res);
	      io_error("io_submit write", res);
	    }
	  gettimeofday(&tv2, NULL);
	  int elapsed = (tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec - tv1.tv_usec) / 1000;
	  //	  elapsed = 0;
	  /* printf("elapsed : %d\n", elapsed); */
	  if (elapsed > 200)
	    printf("\e[33mwarning\e[0m: io_submit() took %d ms, this is suspicious, maybe nr_request is too low.\n", elapsed);
	  inflight += nr;
	  /* printf("-> scheduled   : inflight [%03d/%03d] copied [%d/%d]\n", */
	  /* 	 inflight, MY_AIO_FLIGHT_MAX, copied, mycount); */
	  if (copied + inflight == mycount)
	    printf("all job scheduled\n");
	}
      if ((res = io_queue_run(myctx)) < 0)
	io_error("io_queue_run", res);
      if (inflight == MY_AIO_FLIGHT_MAX ||
	  inflight && copied + inflight == mycount)
	{
	  /* printf("wait\n"); */
	  /* if ((res = io_queue_wait(myctx, NULL)) < 0) */
	  /*   io_error("io_queue_wait", res); */
	  /* printf("wait return %d\n", res); */
	  struct io_event event;
	  if ((res = io_getevents(myctx, 1, 1, &event, NULL)) < 0)
	    io_error("io_getevents", res);
	  if (res != 1)
	    errx(1, "no events?");
	  ((io_callback_t)event.obj->data)(myctx, event.obj, event.res, event.res2);
	}
    }
  io_queue_release(myctx);
  gettimeofday(&tv_end, NULL);
  int elapsed = (tv_end.tv_sec - tv_start.tv_sec) * 1000 + (tv_end.tv_usec - tv_start.tv_usec) / 1000;
  printf("all job done in %d ms\n", elapsed);
  printf("iop/s %d\n", (int)((double)mycount / elapsed * 1000));
  printf("average lantency in ms : %.3f\n", (double)elapsed / mycount);
  printf("average bandwidth : %.3f MB/s (%.3f Mb/s)\n",
	 (double)myiosize * mycount / elapsed * 1000 / (1024 * 1024),
	 (double)myiosize * mycount / elapsed * 1000 / (1024 * 1024) * 8);
  printf("total size in MB : %lld\n", (unsigned long long)myiosize * mycount / 1024 / 1024);
}

int get_raw(int fd)
{
  struct raw_config_request rq;
  int ret;

  int fd_rawctl = open("/dev/raw/rawctl", O_RDWR);
  if (fd_rawctl < 0)
    err(1, "open /dev/raw/rawctl, please modprobe raw");
  memset(&rq, '\0', sizeof(rq));
  rq.raw_minor = 1;
  ret = ioctl(fd_rawctl, RAW_SETBIND, &rq);
  if (ret == -1)
    err(1, "ioctl");
  struct stat buf;
  fstat(fd, &buf);
  printf("blockdev major %d minor %d\n", major(buf.st_rdev), minor(buf.st_rdev));
  rq.block_major = major(buf.st_rdev);
  rq.block_minor = minor(buf.st_rdev);
  ret = ioctl(fd_rawctl, RAW_SETBIND, &rq);
  if (ret == -1)
    err(1, "ioctl");
  close(fd);
  fd = open("/dev/raw/raw1", O_RDWR | O_DIRECT);
  return fd;
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
	  MY_AIO_SUBMIT_MAX = atoi(optarg);
	  if (MY_AIO_SUBMIT_MAX == 0)
	    {
	      fprintf(stderr, "Invalid maximum io submit: %s\n", optarg);
	      return 1;
	    }
	  break;
	case 'f':
	  MY_AIO_FLIGHT_MAX = atoi(optarg);
	  if (MY_AIO_FLIGHT_MAX == 0)
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
  fprintf(stderr, "Usage: %s <--dev=/dev/xxx> [--write] [--iosize=x (kB)] [--iocount=x] [--random] [--maxsubmit=x] [--maxinflight=x]\n", s);
  return 2;
}

int main(int argc, char **argv)
{
  if (main_getopt(argc, argv))
    return 1;

  if (!mydev)
    return usage(argv[0]);

  cpu_set_t mask;

  CPU_ZERO(&mask);
  CPU_SET(0, &mask);
  sched_setaffinity(0, sizeof(mask), &mask);

  //printf("sizeof 8 bytes type : %lu\n", sizeof(long long int));
  int fd = open(mydev, O_RDWR | O_DIRECT);
  if (fd < 0)
    err(1, "open %s", mydev);
  /* fd = get_raw(fd); */
  ioctl(fd, BLKGETSIZE64, &mydevsize);
  /* mydevsize = 262144000000; */
  printf("Device size : %llu\n", mydevsize);
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
  //myiosize = 4096;
  //myiosize = 4096;
  /* myiosize = 8 * 1024; */
  //  myiosize = 16 * 1024;
  //  myiosize = 32 * 1024;
  //   myiosize = 64 * 1024;
  //myiosize = 128 * 1024;
  //myiosize = 4 * 128 * 1024;
  // myiosize = 256 * 1024;
  /* myiosize = 512 * 1024; */
  /* myiosize = 5 * 256 * 1024; */
//  myiosize = 7 * 256 * 1024;
  /* myiosize = 851968; */
  /* myiosize = 1048576; */
  /* myiosize = 13 * 1024 * 32; */
  /* myiosize = 256 * 5 * 1024; */
  printf("Using device %s\n", mydev);
  printf("Performing %s%s test\n", mode_rnd ? "random " : "",  mode_write ? "write" : "read");
  printf("Will perform %llux %lukB IOs\n", mycount, (long unsigned int)(myiosize / 1024));
  printf("Maximum IO submit / inflight: %d / %d\n", MY_AIO_SUBMIT_MAX, MY_AIO_FLIGHT_MAX);
  //mycount = mydevsize / myiosize;
  posix_fadvise(fd, 0, mydevsize, POSIX_FADV_RANDOM);
  void *addr = mmap(NULL, myiosize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!addr)
    err(1, "mmap");
  int i;
  /* if ((unsigned long)addr % 512 || (unsigned long)addr % 4096) */
  if ((uintptr_t)addr % 512 || (uintptr_t)addr % 4096)
    errx(1, "mmap is not aligned...");
  //printf("initialize buffer\n");
  for (i = 0; i < myiosize; ++i)
    {
      ((unsigned char *)addr)[i] = rand();
    }
  //printf("loop\n");
  /* loop_write(fd, addr); */
  loop_aio(fd, addr);
}
