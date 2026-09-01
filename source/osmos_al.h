/* osmos_al.h -- the OpenAL subset libosmos.so actually uses.
 *
 * MIT licensed. See LICENSE.
 *
 * WHY WE DO NOT LOAD THE GAME'S OWN libopenal.so
 * ----------------------------------------------
 * The APK ships an OpenAL Soft build, and loading it through so_util would be
 * the obvious move. It does not work here, and the reason is worth recording
 * so nobody tries it twice:
 *
 *     $ readelf -sW --dyn-syms libopenal.so | grep alc_.*_init
 *     alc_audiotrack_init
 *     alc_null_init
 *
 * Exactly two backends are compiled in. There is no OpenSL ES path, so the
 * opensles.c from the Sonic Jump and Killer Bean ports is no help. The only
 * working backend drives android.media.AudioTrack over JNI, which would mean
 * implementing a real AudioTrack in the JNI layer and pumping it -- more work
 * than the API surface we are replacing.
 *
 * That surface is small. libosmos.so imports 26 OpenAL symbols, and the enum
 * constants that appear as immediates anywhere in .text are only:
 *
 *     AL_GAIN AL_PITCH AL_POSITION AL_LOOPING AL_BUFFER AL_SOURCE_RELATIVE
 *     AL_SEC_OFFSET AL_SOURCE_STATE AL_BUFFERS_PROCESSED
 *     AL_FORMAT_MONO8/16 AL_FORMAT_STEREO8/16
 *
 * No distance model, no cones, no doppler, no EFX. Vorbis is decoded inside
 * the engine (libosmos.so exports the whole vorbis decoder), so OpenAL only
 * ever receives PCM. What is needed is a mixer with gain, pitch and stereo
 * panning, static and queued buffers -- which is what this is.
 */

#ifndef OSMOS_AL_H
#define OSMOS_AL_H

#include <stdint.h>

/* ---- lifecycle, called from main.c ---- */
int  osmos_al_init(void);
void osmos_al_update(void);     /* once per frame: recycle processed buffers */
void osmos_al_suspend(void);    /* console lost focus */
void osmos_al_resume(void);
void osmos_al_shutdown(void);

/* ---- the AL entry points named in imports_osmos.c ---- */

typedef int      ALenum;
typedef int      ALint;
typedef unsigned ALuint;
typedef int      ALsizei;
typedef float    ALfloat;
typedef void     ALvoid;

void  alGenBuffers(ALsizei n, ALuint *buffers);
void  alDeleteBuffers(ALsizei n, const ALuint *buffers);
void  alBufferData(ALuint buffer, ALenum format, const ALvoid *data,
                   ALsizei size, ALsizei freq);

void  alGenSources(ALsizei n, ALuint *sources);
void  alDeleteSources(ALsizei n, const ALuint *sources);
void  alSourcei(ALuint source, ALenum param, ALint value);
void  alSourcef(ALuint source, ALenum param, ALfloat value);
void  alGetSourcei(ALuint source, ALenum param, ALint *value);
void  alGetSourcef(ALuint source, ALenum param, ALfloat *value);
void  alSourcePlay(ALuint source);
void  alSourcePause(ALuint source);
void  alSourceStop(ALuint source);
void  alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers);
void  alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers);

void  alListenerfv(ALenum param, const ALfloat *values);
ALenum alGetError(void);

/* ---- ALC ---- */
void *alcOpenDevice(const char *name);
int   alcCloseDevice(void *dev);
void *alcCreateContext(void *dev, const ALint *attrs);
int   alcMakeContextCurrent(void *ctx);
void  alcDestroyContext(void *ctx);
void *alcGetCurrentContext(void);
void *alcGetContextsDevice(void *ctx);
void  alcProcessContext(void *ctx);
void  alcSuspendContext(void *ctx);

/* Android extension. The engine calls it to hand the VM to the audiotrack
 * backend; ours ignores it, but the symbol has to resolve. */
void *alcGetJavaVM(void);

#endif
