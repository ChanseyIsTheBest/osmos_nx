/* osmos_io.c -- see osmos_io.h. MIT licensed. */

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "osmos_io.h"
#include "libc_shim.h"

/* One recursive lock over all of it.
 *
 * Recursive because the shims call each other -- libc_shim's fopen path can
 * reach open(), and a plain mutex would deadlock on the second acquire. The
 * cost is a lock per file call, which against SD-card latency is nothing. */
static RMutex io_lock;

void io_init(void) { rmutexInit(&io_lock); }

#define IO_ENTER() rmutexLock(&io_lock)
#define IO_LEAVE() rmutexUnlock(&io_lock)

FILE *fopen_locked(const char *path, const char *mode) {
  IO_ENTER();
  FILE *f = fopen(path, mode);
  IO_LEAVE();
  return f;
}

int fclose_locked(FILE *f) {
  IO_ENTER();
  const int r = fclose(f);
  IO_LEAVE();
  return r;
}

size_t fread_locked(void *p, size_t sz, size_t n, FILE *f) {
  IO_ENTER();
  const size_t r = fread(p, sz, n, f);
  IO_LEAVE();
  return r;
}

size_t fwrite_locked(const void *p, size_t sz, size_t n, FILE *f) {
  IO_ENTER();
  const size_t r = fwrite(p, sz, n, f);
  IO_LEAVE();
  return r;
}

int fseek_locked(FILE *f, long off, int whence) {
  IO_ENTER();
  const int r = fseek(f, off, whence);
  IO_LEAVE();
  return r;
}

long ftell_locked(FILE *f) {
  IO_ENTER();
  const long r = ftell(f);
  IO_LEAVE();
  return r;
}

int __open_2_locked(const char *path, int flags) {
  IO_ENTER();
  const int fd = __open_2_fake(path, flags);
  IO_LEAVE();
  return fd;
}

int read_locked(int fd, void *buf, size_t n) {
  IO_ENTER();
  const int r = (int)read(fd, buf, n);
  IO_LEAVE();
  return r;
}

int close_locked(int fd) {
  IO_ENTER();
  const int r = close(fd);
  IO_LEAVE();
  return r;
}
