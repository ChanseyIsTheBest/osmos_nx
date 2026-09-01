/* osmos_shim.c -- see osmos_shim.h. MIT licensed. */

#include <switch.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include "config.h"
#include <time.h>
#include <errno.h>
#include <locale.h>

#include "osmos_shim.h"

/* sincos/sincosf moved to imports_helpers.c, which owns every entry the table
 * names *_fake, so there is one place to look for them. */

/* Android/bionic and devkitA64/newlib assign different numeric clock IDs:
 *
 *     bionic CLOCK_REALTIME  = 0     newlib CLOCK_REALTIME  = 1
 *     bionic CLOCK_MONOTONIC = 1     newlib CLOCK_MONOTONIC = 4
 *
 * libosmos.so passes 1 from both GetTickCount() and mach_continuous_time(),
 * meaning CLOCK_MONOTONIC -- which newlib reads as CLOCK_REALTIME. That still
 * returns a time, so it does not fail loudly; it just makes the engine's frame
 * clock follow the wall clock and jump if the system time changes. Translate
 * properly rather than relying on the ids happening to overlap. */
int clock_gettime_bionic(int android_id, struct timespec *tp) {
  if (!tp) { errno = EINVAL; return -1; }

  clockid_t host;
  switch (android_id) {
    case 0:  /* CLOCK_REALTIME          */
    case 5:  /* CLOCK_REALTIME_COARSE   */
      host = CLOCK_REALTIME; break;
    case 1:  /* CLOCK_MONOTONIC         */
    case 2:  /* CLOCK_PROCESS_CPUTIME_ID: elapsed monotonic is close enough */
    case 3:  /* CLOCK_THREAD_CPUTIME_ID */
    case 4:  /* CLOCK_MONOTONIC_RAW     */
    case 6:  /* CLOCK_MONOTONIC_COARSE  */
    case 7:  /* CLOCK_BOOTTIME          */
      host = CLOCK_MONOTONIC; break;
    default:
      errno = EINVAL; return -1;
  }
  return clock_gettime(host, tp);
}

/* clock_getres MUST report 1 ns. This is not a hardware question.
 *
 * The engine derives its timebase from it:
 *
 *     CCounter::GetPerformanceCounter()  -> mach_continuous_time() -> NANOSECONDS
 *     CCounter::GetPerformanceFrequency() -> 1e9 / clock_getres().tv_nsec
 *     CCounter::GetSecs()                 -> counter / frequency
 *
 * The counter is already in nanoseconds, so the only frequency that makes
 * GetSecs() correct is 1e9 -- which requires tv_nsec == 1. That is exactly
 * what bionic returns for CLOCK_MONOTONIC on Android, the platform this engine
 * was built and tuned against.
 *
 * An earlier version returned 6, reasoning that it should report "the real
 * hardware tick rather than claiming 1 ns and having the engine compute a
 * nonsense frame budget". That was backwards: the engine is not asking how
 * precise the clock is, it is asking for the divisor that converts its
 * nanosecond counter to seconds. Returning 6 made the frequency 166,666,666
 * instead of 1,000,000,000, so every CCounter-derived duration ran SIX TIMES
 * fast. (The 6 was not even right on its own terms -- the Switch system
 * counter is 19.2 MHz, or 52 ns, not the 192 MHz the comment claimed.)
 */
int clock_getres_fake(int clk, struct timespec *res) {
  (void)clk;
  if (!res) { errno = EFAULT; return -1; }
  res->tv_sec  = 0;
  res->tv_nsec = 1;
  return 0;
}

/* setlocale category numbering is not merely different between bionic and
 * newlib, it is a different ordering entirely:
 *
 *     bionic:  LC_CTYPE 0  LC_NUMERIC 1  LC_TIME 2  LC_COLLATE 3
 *              LC_MONETARY 4  LC_MESSAGES 5  LC_ALL 6
 *     newlib:  LC_ALL 0  LC_COLLATE 1  LC_CTYPE 2  LC_MONETARY 3
 *              LC_NUMERIC 4  LC_TIME 5  LC_MESSAGES 6
 *
 * libosmos.so calls setlocale(6, ...) from std::__ndk1::locale::global --
 * that is bionic's LC_ALL, which newlib reads as LC_MESSAGES. Passing it
 * through unchanged sets the wrong category and, when the name is one newlib
 * does not know, returns NULL. libc++ treats a NULL return from locale::global
 * as grounds to throw. Translate instead. */
static int lc_bionic_to_newlib(int c) {
  switch (c) {
    case 0: return LC_CTYPE;
    case 1: return LC_NUMERIC;
    case 2: return LC_TIME;
    case 3: return LC_COLLATE;
    case 4: return LC_MONETARY;
    case 5: return LC_MESSAGES;
    case 6: return LC_ALL;
    default: return LC_ALL;
  }
}

char *setlocale_bionic(int category, const char *locale) {
  return setlocale(lc_bionic_to_newlib(category), locale);
}

/* Osmos parses its .cfg and .loc files through the C locale only -- it never
 * calls newlocale with anything but LC_GLOBAL_LOCALE -- so ignoring the locale
 * argument here is correct rather than merely convenient. */
int isdigit_l_fake(int c, void *loc)  { (void)loc; return isdigit(c);  }
int islower_l_fake(int c, void *loc)  { (void)loc; return islower(c);  }
int isupper_l_fake(int c, void *loc)  { (void)loc; return isupper(c);  }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int tolower_l_fake(int c, void *loc)  { (void)loc; return tolower(c);  }
int toupper_l_fake(int c, void *loc)  { (void)loc; return toupper(c);  }

/* ------------------------------------------------------------------ */
/* mmap / munmap                                                       */
/* ------------------------------------------------------------------ */

/* Cross-referencing every PLT call site in libosmos.so shows mmap is reached
 * from exactly ONE function -- FreeType's FT_Stream_Open, mapping a font file
 * read-only -- and munmap from exactly one, ft_close_stream_by_munmap.
 * Nothing else in the game maps anything, and mprotect/madvise/mremap are not
 * imported at all.
 *
 * libc_shim's arena is roughly 700 lines written for Unity. Its own comment
 * says why: Unity reserves big aligned pools by over-mapping and then
 * munmapping the head and tail, so a plain malloc/free per mmap would free the
 * whole block when the head is trimmed. FreeType does none of that. It maps a
 * file, reads it, and unmaps exactly what it mapped.
 *
 * Serving that with the Unity machinery produced three separate failures here:
 * an unbreakable infinite loop when the granule was left at zero; the
 * over-map/trim path being taken for a 3.2 MB font because the threshold sat
 * below it; and a large arena carved out of the same newlib heap the engine
 * allocates from, after which malloc(400) returned an unmapped address and
 * FT_Init_FreeType failed.
 *
 * The requirement is: allocate, fill from the file, remember the size, free on
 * unmap. That is what this does. The arena code stays in the tree, unmodified
 * and unused, so upstream fixes can still be pulled into it.
 */

#define OSMOS_MAP_ANONYMOUS 0x20      /* bionic MAP_ANONYMOUS */
#define OSMOS_MAP_FAILED    ((void *)-1)
#define OSMOS_MAX_MAPS      32

static struct { void *p; size_t len; } g_maps[OSMOS_MAX_MAPS];
static Mutex g_maps_lock;

void *osmos_mmap(void *addr, size_t len, int prot, int flags,
                 int fd, long offset) {
  (void)addr; (void)prot;
  if (len == 0) { errno = EINVAL; return OSMOS_MAP_FAILED; }

  /* Page-aligned because callers assume it, and because FreeType computes the
   * face pointer as an offset from the mapping base. */
  void *p = memalign(0x1000, len);
  if (!p) { errno = ENOMEM; return OSMOS_MAP_FAILED; }

  if (flags & OSMOS_MAP_ANONYMOUS) {
    memset(p, 0, len);                    /* anonymous memory reads as zero */
  } else if (fd >= 0) {
    /* Fill from the file. A short read is not an error -- a mapping may run
     * past EOF -- but the tail must be zero rather than whatever memalign
     * handed back, or FreeType parses uninitialised memory as font data. */
    const long saved = lseek(fd, 0, SEEK_CUR);
    size_t got = 0;
    if (lseek(fd, offset, SEEK_SET) >= 0) {
      while (got < len) {
        const long r = read(fd, (char *)p + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
    }
    if (got < len) memset((char *)p + got, 0, len - got);
    if (saved >= 0) lseek(fd, saved, SEEK_SET);
  } else {
    memset(p, 0, len);
  }

  mutexLock(&g_maps_lock);
  int slot = -1;
  for (int i = 0; i < OSMOS_MAX_MAPS; i++)
    if (!g_maps[i].p) { slot = i; break; }
  if (slot >= 0) { g_maps[slot].p = p; g_maps[slot].len = len; }
  mutexUnlock(&g_maps_lock);

  if (slot < 0) {
    /* Out of table slots. Leaking is survivable; handing back a pointer that
     * munmap cannot free and free() would reject is not. */
    LOGW("mmap: tracking table full (%d live); leaking %u KB",
         OSMOS_MAX_MAPS, (unsigned)(len >> 10));
  }
  return p;
}

int osmos_munmap(void *addr, size_t len) {
  (void)len;
  if (!addr || addr == OSMOS_MAP_FAILED) return 0;

  mutexLock(&g_maps_lock);
  void *found = NULL;
  for (int i = 0; i < OSMOS_MAX_MAPS; i++) {
    if (g_maps[i].p == addr) {
      found = g_maps[i].p;
      g_maps[i].p = NULL;
      g_maps[i].len = 0;
      break;
    }
  }
  mutexUnlock(&g_maps_lock);

  /* Only free pointers this shim handed out. Calling free() on anything else
   * is precisely the kind of heap corruption that cost this port a week. */
  if (found) free(found);
  return 0;
}
