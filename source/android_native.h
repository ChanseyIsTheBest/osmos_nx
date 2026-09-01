/* android_native.h -- NDK opaque types.
 *
 * libc_shim.c includes this by name. In the reference ports it declares the
 * whole NativeActivity ABI; Osmos is a GLSurfaceView game and uses none of it
 * (libosmos.so imports exactly one symbol from libandroid.so, and that one is
 * really libc), so only the opaque typedefs remain.
 */
#ifndef OSMOS_ANDROID_NATIVE_H
#define OSMOS_ANDROID_NATIVE_H

#include <stdint.h>
#include <stddef.h>

typedef struct ANativeWindow   ANativeWindow;
typedef struct AInputQueue     AInputQueue;
typedef struct AInputEvent     AInputEvent;
typedef struct ALooper         ALooper;
typedef struct AConfiguration  AConfiguration;
typedef struct AAssetManager   AAssetManager;
typedef struct ANativeActivity ANativeActivity;

#endif
