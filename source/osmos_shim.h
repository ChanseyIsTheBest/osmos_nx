/* osmos_shim.h -- the handful of libc entry points neither reference port
 * covers and newlib does not provide. MIT licensed.
 *
 * Everything here exists because bionic has it and devkitA64's newlib does
 * not, or has it only as a macro. Nothing here is Osmos-specific logic.
 */
#ifndef OSMOS_SHIM_H
#define OSMOS_SHIM_H

#include <time.h>
#include <stddef.h>

/* bionic and newlib number the setlocale categories differently -- and in a
 * different order, not merely with an offset. See the .c file. */
char *setlocale_bionic(int category, const char *locale);

/* bionic and newlib number the clocks differently; see the .c file. */
int clock_gettime_bionic(int android_id, struct timespec *tp);

/* newlib has clock_getres only behind _POSIX_TIMERS on some builds. */
int clock_getres_fake(int clk, struct timespec *res);

/* bionic exports these as real functions; newlib makes several of them
 * macros, so the module cannot take their address. */
int isdigit_l_fake(int c, void *loc);
int islower_l_fake(int c, void *loc);
int isupper_l_fake(int c, void *loc);
int isxdigit_l_fake(int c, void *loc);
int tolower_l_fake(int c, void *loc);
int toupper_l_fake(int c, void *loc);

/* mmap/munmap sized for what this game actually does; see the .c file. */
void *osmos_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset);
int   osmos_munmap(void *addr, size_t len);

#endif
