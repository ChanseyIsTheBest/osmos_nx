/* compat_stubs.c -- see compat_stubs.h. MIT licensed. */

#include <errno.h>
#include <string.h>
#include <malloc.h>

#include "compat_stubs.h"
#include "config.h"
#include "osmos_paths.h"

void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;

/* libc_shim's mmap arena is no longer reachable: mmap and munmap now resolve
 * to osmos_mmap/osmos_munmap in osmos_shim.c, which are sized for what this
 * game actually does (one FreeType font mapping) rather than for Unity.
 *
 * These globals still have to exist because libc_shim.c is compiled in
 * unmodified and references them. g_mmap_big_align must stay NON-ZERO
 * regardless -- libc_shim divides by it and steps a loop by it, and zero there
 * is an infinite loop. Keeping it at the reference port's 64 MB costs nothing
 * and keeps the dead code well defined.
 */
size_t g_mmap_big_align = (size_t)64 * 1024 * 1024;

void osmos_mmap_arena_init(void) {
  /* Deliberately allocates nothing.
   *
   * The previous version memaligned 64-256 MB out of the newlib heap so
   * libc_shim's arena would have somewhere to live. Nothing reaches that arena
   * any more, and the allocation was actively harmful: it competed with the
   * engine for the same heap, and the run after it was introduced ended with
   * malloc(400) returning an unmapped address inside FT_New_Library.
   *
   * Kept as a named no-op rather than deleted so that main.c's call site, and
   * the reason it exists, stay visible. */
  LOGB("mmap: using osmos_mmap (no arena; libc_shim's arena is unreachable)");
}

int asset_pack_close_fd(int fd) { (void)fd; return -1; }

int asset_pack_closedir_path(void *dir) { (void)dir; return -1; }

int asset_pack_dir_is(const void *dir) { (void)dir; return 0; }

int asset_pack_fd_is(int fd) { (void)fd; return 0; }

/* MUST RETURN 0. These three are predicates -- "did I handle it?" -- not
 * error codes, and libc_shim tests them as `if (asset_pack_fstat_fd(...))`.
 *
 * Returning -1 was the single worst bug in this port. `if (-1)` is true, so
 * fstat_fake took the asset-pack branch every time and filled struct stat
 * from `packed_size`, an UNINITIALISED stack variable. Every fstat in the
 * game returned a garbage st_size. FreeType then sized its font mapping from
 * it and asked for 91 GB; the engine sized texture reads from it and got 3 MB
 * of the 30 MB it wanted. Nothing reported an error anywhere -- the calls all
 * "succeeded". */
int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory) {
  (void)fd; (void)size; (void)ino; (void)directory;
  return 0;                                  /* not an asset-pack fd */
}

long asset_pack_lseek_fd(int fd, long offset, int whence) { (void)fd; (void)offset; (void)whence; return -1; }

int asset_pack_open_path(const char *path) { (void)path; return -1; }

long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset) { (void)fd; (void)buffer; (void)count; (void)offset; return -1; }

int asset_pack_read_all_path(const char *path, void **data, size_t *size) {
  (void)path; (void)data; (void)size;
  return 0;                                  /* tested as `if (!...) return NULL;` */
}

long asset_pack_read_fd(int fd, void *buffer, size_t count) { (void)fd; (void)buffer; (void)count; return -1; }

const char *asset_pack_readdir_path(void *dir, uint8_t *type, uint64_t *ino) { (void)dir; (void)type; (void)ino; return NULL; }

int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino, int *directory) {
  (void)path; (void)size; (void)ino; (void)directory;
  return 0;                                  /* not an asset-pack path */
}

int fakefd_close(int fd) { (void)fd; return -1; }

/* MUST RETURN 0, for the same reason as the asset_pack predicates above.
 *
 * libc_shim tests it as `if (fakefd_is_fake(fd)) return fakefd_read(...)`, so
 * -1 sent EVERY read(), write() and close() in the module down the fake path,
 * which then returned EBADF. This port opens only real files; nothing here is
 * ever a fake descriptor.
 *
 * This one was correct in the first hand-written version of this file and was
 * reintroduced when the stubs were regenerated from the linker-derived symbol
 * list -- the generator special-cased asset_pack_dir_is and asset_pack_fd_is
 * and lost this one. tools/check_stub_polarity.py now catches it. */
int fakefd_is_fake(int fd) { (void)fd; return 0; }

int fakefd_pipe(int fds[2]) { (void)fds; return -1; }

long fakefd_read(int fd, void *buffer, unsigned long size) { (void)fd; (void)buffer; (void)size; return -1; }

long fakefd_write(int fd, const void *buffer, unsigned long size) { (void)fd; (void)buffer; (void)size; return -1; }

void *firebase_stub_lookup(const char *name) {
  /* Osmos has no Firebase. libc_shim.c consults this from its dlsym
   * path; answering NULL makes dlsym report "not found", which is
   * true. */
  (void)name; return NULL;
}

int nx_accept(int fd, void *addr, void *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_accept");
  errno = ENOSYS;
  return -1;
}

int nx_bind(int fd, const void *addr, unsigned addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_bind");
  errno = ENOSYS;
  return -1;
}

int nx_connect(int fd, const void *addr, unsigned addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_connect");
  errno = ENOSYS;
  return -1;
}

void nx_freeaddrinfo(void *res) {
  (void)res;
  LOGW("unexpected socket call: nx_freeaddrinfo");
}

int nx_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
  (void)node; (void)service; (void)hints; (void)res;
  LOGW("unexpected socket call: nx_getaddrinfo");
  errno = ENOSYS;
  return -1;
}

int nx_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen, char *serv, unsigned servlen, int flags) {
  (void)addr; (void)addrlen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
  LOGW("unexpected socket call: nx_getnameinfo");
  errno = ENOSYS;
  return -1;
}

int nx_getpeername(int fd, void *addr, void *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_getpeername");
  errno = ENOSYS;
  return -1;
}

int nx_getsockname(int fd, void *addr, void *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_getsockname");
  errno = ENOSYS;
  return -1;
}

int nx_getsockopt(int fd, int level, int optname, void *optval, void *optlen) {
  (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
  LOGW("unexpected socket call: nx_getsockopt");
  errno = ENOSYS;
  return -1;
}

int nx_listen(int fd, int backlog) {
  (void)fd; (void)backlog;
  LOGW("unexpected socket call: nx_listen");
  errno = ENOSYS;
  return -1;
}

int nx_poll(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds; (void)timeout;
  LOGW("unexpected socket call: nx_poll");
  errno = ENOSYS;
  return -1;
}

long nx_recv(int fd, void *buf, size_t len, int flags) {
  (void)fd; (void)buf; (void)len; (void)flags;
  LOGW("unexpected socket call: nx_recv");
  errno = ENOSYS;
  return -1;
}

long nx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_recvfrom");
  errno = ENOSYS;
  return -1;
}

int nx_select(int nfds, void *rd, void *wr, void *ex, void *timeout) {
  (void)nfds; (void)rd; (void)wr; (void)ex; (void)timeout;
  LOGW("unexpected socket call: nx_select");
  errno = ENOSYS;
  return -1;
}

long nx_send(int fd, const void *buf, size_t len, int flags) {
  (void)fd; (void)buf; (void)len; (void)flags;
  LOGW("unexpected socket call: nx_send");
  errno = ENOSYS;
  return -1;
}

long nx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
  LOGW("unexpected socket call: nx_sendto");
  errno = ENOSYS;
  return -1;
}

int nx_setsockopt(int fd, int level, int optname, const void *optval, unsigned optlen) {
  (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
  LOGW("unexpected socket call: nx_setsockopt");
  errno = ENOSYS;
  return -1;
}

int nx_shutdown(int fd, int how) {
  (void)fd; (void)how;
  LOGW("unexpected socket call: nx_shutdown");
  errno = ENOSYS;
  return -1;
}

int nx_socket(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol;
  LOGW("unexpected socket call: nx_socket");
  errno = ENOSYS;
  return -1;
}

const char *sj_home(void) { return osmos_root(); }

void sj_mark(const char *what) { LOGI("[mark] %s", what ? what : ""); }

void sj_trace_open(const char *path, int ok) { (void)path; (void)ok; }
