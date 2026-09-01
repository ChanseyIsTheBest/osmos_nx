/* osmos_diag.h -- thread registry, wait beacons and a stall watchdog.
 *
 * MIT licensed. Ported from the Killer Bean port's diag.c
 * (Copyright (C) 2021 Andy Nguyen, fgsfds and contributors), adapted for a
 * two-thread engine with no GC bridge.
 *
 * A parked thread prints nothing, so the hang is made to report itself. Every
 * thread that can block publishes a wait beacon -- what kind of wait, on which
 * object, since when -- into a small registry. An independent libnx watchdog
 * thread, deliberately NOT routed through the pthread shim being debugged,
 * notices when progress stops and dumps every thread's CPU context with a
 * symbolicated backtrace.
 *
 * Overhead on the happy path is a few volatile stores; nothing is logged until
 * a stall is actually detected.
 */
#ifndef OSMOS_DIAG_H
#define OSMOS_DIAG_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include <stdlib.h>
#include "osmos_io.h"

#if !OSMOS_DIAG
/* Diagnostics off. File access still has to be serialised -- that is not a
 * diagnostic, it is a correctness requirement next to the engine's worker
 * thread -- so these resolve to the locked calls rather than to nothing. */
#define fopen_diag        fopen_locked
#define fread_diag        fread_locked
#define fwrite_diag       fwrite_locked
#define __open_2_diag     __open_2_locked
#define sched_yield_diag  sched_yield_fake
#define malloc_diag       malloc

#define diag_start()               ((void)0)
#define diag_stop()                ((void)0)
#define diag_phase(p)              ((void)(p))
#define diag_frame(f)              ((void)(f))
#define diag_thread_register(e, m) ((void)0)
#define diag_thread_unregister()   ((void)0)
#define diag_wait_enter(k, o)      ((void)0)
#define diag_wait_exit()           ((void)0)

#else

/* Wait kinds, kept in sync with wait_kind_name() in the .c file. */
enum {
  DIAG_W_NONE = 0,
  DIAG_W_COND,     /* pthread_cond_wait / timedwait   */
  DIAG_W_JOIN,     /* pthread_join                    */
  DIAG_W_MUTEX,    /* contended pthread_mutex_lock    */
  DIAG_W_YIELD,    /* spinning on sched_yield         */
};

void diag_start(void);          /* register the main thread, spawn the watchdog */
void diag_stop(void);

/* Called from pthread_create_fake for each engine thread. */
void diag_thread_register(const void *entry, int is_main);
void diag_thread_unregister(void);

/* Beacons. enter() publishes (kind, obj, now); exit() clears. Cheap. */
void diag_wait_enter(int kind, const void *obj);
void diag_wait_exit(void);

/* Progress signals. Any of these resets the stall timer -- Osmos hangs before
 * the frame loop exists, so frames alone are not enough to tell live from
 * wedged. */
void diag_phase(const char *p);
void diag_frame(unsigned long long frame);

/* Wrapped libc entry points, named in imports_osmos.c. */
FILE  *fopen_diag(const char *path, const char *mode);
size_t fread_diag(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite_diag(const void *p, size_t sz, size_t n, FILE *f);
void  *malloc_diag(size_t n);
int    __open_2_diag(const char *path, int flags);
int    sched_yield_diag(void);

#endif  /* !OSMOS_DIAG */

#endif
