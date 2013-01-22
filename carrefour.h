/*
Copyright (C) 2013  
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef PROFILER_H_
#define PROFILER_H_

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/syscall.h>
#define __EXPORTED_HEADERS__
#include <sys/sysinfo.h>
#undef __EXPORTED_HEADERS__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <dirent.h>
#include <numa.h>

#define MAX_CORE                64
#define TIME_SECOND             1000000
#define PAGE_SIZE               (4*1024)

#undef __NR_perf_counter_open
#ifdef __powerpc__
#define __NR_perf_counter_open  319
#elif defined(__x86_64__)
#define __NR_perf_counter_open  298
#elif defined(__i386__)
#define __NR_perf_counter_open  336
#endif

#ifdef __x86_64__
#define rdtscll(val) {                                           \
    unsigned int __a,__d;                                        \
    asm volatile("rdtsc" : "=a" (__a), "=d" (__d));              \
    (val) = ((unsigned long)__a) | (((unsigned long)__d)<<32);   \
}
#else
   #define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#endif

#define __unused__ __attribute__((unused))

typedef struct _event {
   uint64_t type;
   uint64_t config;
   uint64_t exclude_kernel;
   uint64_t exclude_user;
   const char* name;
   long leader;
} event_t;


struct perf_read_ev {
   uint64_t value;
   uint64_t time_enabled;
   uint64_t time_running;
};

#endif /* PROFILER_H_ */
