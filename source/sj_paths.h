/* sj_paths.h -- forwarder.
 *
 * libc_shim.c is reused unmodified from the reference ports, and includes this
 * header by name. Osmos needs none of what it originally declared (no APK
 * asset pack, no sockets, no fake descriptors), so the declarations live in
 * compat_stubs.h and this exists only so the include resolves. Keeping
 * libc_shim.c untouched is what lets fixes be pulled back from upstream.
 */
#ifndef OSMOS_FWD_SJ_PATHS_H
#define OSMOS_FWD_SJ_PATHS_H
#include "compat_stubs.h"
#endif
