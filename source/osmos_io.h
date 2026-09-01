/* osmos_io.h -- serialised file I/O.
 *
 * MIT licensed.
 *
 * devkitPro's newlib keeps a process-wide file handle table that is not
 * thread-safe, and this port is not single-threaded: nativeActivateGame spawns
 * loadWhileShowingSplash, which streams .ogg files while the main thread loads
 * textures and writes logs. Every file call the engine makes therefore has to
 * be serialised, and so does every file call this port makes alongside it.
 *
 * These are the only file entry points in imports_osmos.c, and nx_pointer is
 * handed the same lock through its fopen_fn/fclose_fn hooks.
 */
#ifndef OSMOS_IO_H
#define OSMOS_IO_H

#include <stdio.h>
#include <stddef.h>

void io_init(void);

FILE  *fopen_locked(const char *path, const char *mode);
int    fclose_locked(FILE *f);
size_t fread_locked(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite_locked(const void *p, size_t sz, size_t n, FILE *f);
int    fseek_locked(FILE *f, long off, int whence);
long   ftell_locked(FILE *f);
int    __open_2_locked(const char *path, int flags);
int    read_locked(int fd, void *buf, size_t n);
int    close_locked(int fd);

#endif
