/* osmos_al.c -- OpenAL subset over libnx audren.
 *
 * MIT licensed. See LICENSE. See osmos_al.h for why this exists rather than
 * loading the APK's own libopenal.so.
 *
 * Design: a fixed pool of voices mixed in software into a double-buffered
 * audren wavebuf at 48 kHz stereo float. Software mixing rather than one
 * audren voice per source because pitch, per-source queueing and the
 * unqueue-when-consumed contract are all easier to make exactly right in one
 * place than to map onto the driver's voice model -- and with at most a few
 * dozen concurrent sounds, the cost does not matter.
 */

#include <switch.h>
#include <stdlib.h>
#include <malloc.h>   /* memalign: implicit here would truncate the pointer */
#include <string.h>
#include <math.h>

#include "config.h"
#include "osmos_al.h"
#include "osmos_jni.h"

/* --- AL constants (only the ones the engine uses) --- */
#define AL_NONE                 0
#define AL_FALSE                0
#define AL_TRUE                 1
#define AL_SOURCE_RELATIVE      0x0202
#define AL_PITCH                0x1003
#define AL_POSITION             0x1004
#define AL_LOOPING              0x1007
#define AL_BUFFER               0x1009
#define AL_GAIN                 0x100A
#define AL_SOURCE_STATE         0x1010
#define AL_INITIAL              0x1011
#define AL_PLAYING              0x1012
#define AL_PAUSED               0x1013
#define AL_STOPPED              0x1014
#define AL_BUFFERS_QUEUED       0x1015
#define AL_BUFFERS_PROCESSED    0x1016
#define AL_SEC_OFFSET           0x1024
#define AL_FORMAT_MONO8         0x1100
#define AL_FORMAT_MONO16        0x1101
#define AL_FORMAT_STEREO8       0x1102
#define AL_FORMAT_STEREO16      0x1103
#define AL_NO_ERROR             0
#define AL_INVALID_NAME         0xA001
#define AL_INVALID_VALUE        0xA003

#define MIX_RATE      48000
#define MIX_CHANNELS  2
#define MIX_FRAMES    960          /* 20 ms */
#define NUM_WAVEBUFS  4
#define MAX_BUFFERS   256
#define MAX_SOURCES   64
#define MAX_QUEUE     8            /* the engine double-buffers music */

typedef struct {
  int      used;
  int16_t *pcm;        /* always converted to 16-bit on upload */
  int      frames;     /* per channel */
  int      channels;
  int      rate;
} Buffer;

typedef struct {
  int   used;
  int   state;
  float gain, pitch, pan;
  int   looping;
  int   relative;

  int   queue[MAX_QUEUE];   /* ring of queued-but-unplayed buffer ids */
  int   qhead, qcount;

  /* Finished buffer ids awaiting alSourceUnqueueBuffers, in the order they
   * were consumed. This has to be a real FIFO of ids rather than just a
   * count: CTrackOpenAL::TickStream unqueues an id, refills THAT id through
   * LoadBufferFromCStreamingBuffer(unsigned int&, ...) -> alBufferData, and
   * re-queues it. An earlier version returned zeros here, so the engine
   * refilled buffer 0 (which does not exist), queued buffer 0, and the music
   * stopped after the first two buffers. */
  int   done[MAX_QUEUE];
  int   dhead, dcount;

  int   cur;                /* buffer id currently sounding, 0 = none */
  double pos;               /* fractional read position, in frames */
} Source;

static Buffer buffers[MAX_BUFFERS];
static Source sources[MAX_SOURCES];
static ALenum al_error = AL_NO_ERROR;

/* All AL state is touched from two threads, which is not obvious from the API.
 *
 * nativeActivateGame spawns loadWhileShowingSplash, and that thread calls
 * GetSoundSystem() and InitSound(bool) -- so alGenBuffers, alGenSources and
 * alBufferData run on the loader thread while osmos_al_update() is mixing on
 * the main thread. alBufferData does free(b->pcm) then malloc, so a mixer
 * reading b->pcm concurrently is a use-after-free, and one that would only
 * show up during loading.
 *
 * A single lock over the whole shim is the right trade here: the mixer holds
 * it for a 20 ms buffer sixty times a second, and the alternative is
 * per-object atomics for a subsystem that has no contention to speak of. */
static Mutex al_lock;
#define AL_LOCK()   mutexLock(&al_lock)
#define AL_UNLOCK() mutexUnlock(&al_lock)

static AudioDriver  drv;
static AudioDriverWaveBuf wavebufs[NUM_WAVEBUFS];
static int16_t     *mixmem;
static void        *mempool;
static int          audio_ready;
static int          suspended;
static float        master_gain = 1.0f;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Defined with the mixer below, but alSourceStop needs it too. */
static void retire(Source *s, int buffer_id);

static Buffer *buf_get(ALuint id) {
  if (id == 0 || id >= MAX_BUFFERS || !buffers[id].used) return NULL;
  return &buffers[id];
}
static Source *src_get(ALuint id) {
  if (id == 0 || id >= MAX_SOURCES || !sources[id].used) return NULL;
  return &sources[id];
}

/* ------------------------------------------------------------------ */
/* buffers                                                             */
/* ------------------------------------------------------------------ */

static void alGenBuffers_locked(ALsizei n, ALuint *out) {
  for (ALsizei k = 0; k < n; k++) {
    out[k] = 0;
    for (int i = 1; i < MAX_BUFFERS; i++) {
      if (!buffers[i].used) {
        memset(&buffers[i], 0, sizeof(Buffer));
        buffers[i].used = 1;
        out[k] = (ALuint)i;
        break;
      }
    }
    if (!out[k]) al_error = AL_INVALID_VALUE;
  }
}

static void alDeleteBuffers_locked(ALsizei n, const ALuint *ids) {
  for (ALsizei k = 0; k < n; k++) {
    Buffer *b = buf_get(ids[k]);
    if (!b) continue;
    free(b->pcm);
    memset(b, 0, sizeof(*b));
  }
}

static void alBufferData_locked(ALuint id, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei freq) {
  Buffer *b = buf_get(id);
  if (!b) { al_error = AL_INVALID_NAME; return; }

  int ch = (format == AL_FORMAT_STEREO8 || format == AL_FORMAT_STEREO16) ? 2 : 1;
  int wide = (format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16);

  int frames = wide ? (size / (2 * ch)) : (size / ch);
  free(b->pcm);
  b->pcm = malloc((size_t)frames * ch * sizeof(int16_t));
  if (!b->pcm) { al_error = AL_INVALID_VALUE; b->frames = 0; return; }

  if (wide) {
    memcpy(b->pcm, data, (size_t)frames * ch * sizeof(int16_t));
  } else {
    /* 8-bit PCM in OpenAL is unsigned with 128 as silence. */
    const uint8_t *s = data;
    for (int i = 0; i < frames * ch; i++)
      b->pcm[i] = (int16_t)((int)s[i] - 128) << 8;
  }
  b->frames = frames;
  b->channels = ch;
  b->rate = freq > 0 ? freq : MIX_RATE;
}

/* ------------------------------------------------------------------ */
/* sources                                                             */
/* ------------------------------------------------------------------ */

static void alGenSources_locked(ALsizei n, ALuint *out) {
  for (ALsizei k = 0; k < n; k++) {
    out[k] = 0;
    for (int i = 1; i < MAX_SOURCES; i++) {
      if (!sources[i].used) {
        Source *s = &sources[i];
        memset(s, 0, sizeof(*s));
        s->used = 1; s->state = AL_INITIAL;
        s->gain = 1.0f; s->pitch = 1.0f; s->pan = 0.0f;
        out[k] = (ALuint)i;
        break;
      }
    }
    if (!out[k]) al_error = AL_INVALID_VALUE;
  }
}

static void alDeleteSources_locked(ALsizei n, const ALuint *ids) {
  for (ALsizei k = 0; k < n; k++) {
    Source *s = src_get(ids[k]);
    if (s) memset(s, 0, sizeof(*s));
  }
}

static void alSourcei_locked(ALuint id, ALenum param, ALint v) {
  Source *s = src_get(id);
  if (!s) { al_error = AL_INVALID_NAME; return; }
  switch (param) {
    case AL_BUFFER:
      /* Static attach. Setting AL_BUFFER also clears any queue, per spec --
       * including the processed list, or a later unqueue would hand back ids
       * belonging to a stream that is no longer attached. */
      s->cur = v; s->pos = 0;
      s->qcount = s->qhead = 0;
      s->dcount = s->dhead = 0;
      break;
    case AL_LOOPING:         s->looping = v ? 1 : 0; break;
    case AL_SOURCE_RELATIVE: s->relative = v ? 1 : 0; break;
    default: break;
  }
}

static void alSourcef_locked(ALuint id, ALenum param, ALfloat v) {
  Source *s = src_get(id);
  if (!s) { al_error = AL_INVALID_NAME; return; }
  switch (param) {
    case AL_GAIN:  s->gain  = v < 0 ? 0 : v; break;
    /* Guard the pitch: the engine ramps it for its time-dilation effect, and
     * a zero or negative value would stall or reverse the read cursor. */
    case AL_PITCH: s->pitch = v > 0.01f ? v : 0.01f; break;
    case AL_SEC_OFFSET: {
      Buffer *b = buf_get((ALuint)s->cur);
      if (b) s->pos = (double)v * b->rate;
      break;
    }
    default: break;
  }
}

static void alGetSourcei_locked(ALuint id, ALenum param, ALint *out) {
  Source *s = src_get(id);
  if (!s || !out) { al_error = AL_INVALID_NAME; return; }
  switch (param) {
    case AL_SOURCE_STATE:      *out = s->state; break;
    case AL_BUFFERS_PROCESSED: *out = s->dcount; break;
    case AL_BUFFERS_QUEUED:
      /* Per spec this counts everything attached, processed included. */
      *out = s->qcount + s->dcount + (s->cur ? 1 : 0);
      break;
    case AL_BUFFER:            *out = s->cur; break;
    case AL_LOOPING:           *out = s->looping; break;
    default: *out = 0; break;
  }
}

static void alGetSourcef_locked(ALuint id, ALenum param, ALfloat *out) {
  Source *s = src_get(id);
  if (!s || !out) { al_error = AL_INVALID_NAME; return; }
  switch (param) {
    case AL_GAIN:  *out = s->gain;  break;
    case AL_PITCH: *out = s->pitch; break;
    case AL_SEC_OFFSET: {
      Buffer *b = buf_get((ALuint)s->cur);
      *out = b ? (float)(s->pos / b->rate) : 0.0f;
      break;
    }
    default: *out = 0.0f; break;
  }
}

static void alSourcePlay_locked(ALuint id) {
  Source *s = src_get(id);
  if (!s) { al_error = AL_INVALID_NAME; return; }
  if (s->state != AL_PAUSED) s->pos = 0;
  /* A queued source with nothing attached yet takes its first buffer here. */
  if (!s->cur && s->qcount > 0) {
    s->cur = s->queue[s->qhead];
    s->qhead = (s->qhead + 1) % MAX_QUEUE;
    s->qcount--;
    s->pos = 0;
  }
  s->state = AL_PLAYING;
}

static void alSourcePause_locked(ALuint id) {
  Source *s = src_get(id);
  if (s && s->state == AL_PLAYING) s->state = AL_PAUSED;
}

/* Stopping marks every attached buffer processed, per spec.
 * CTrackOpenAL::Stop() relies on it: it stops the source and then unqueues,
 * and if the ids never come back the engine leaks them out of its own pool. */
static void alSourceStop_locked(ALuint id) {
  Source *s = src_get(id);
  if (!s) return;
  if (s->cur) { retire(s, s->cur); s->cur = 0; }
  while (s->qcount > 0) {
    retire(s, s->queue[s->qhead]);
    s->qhead = (s->qhead + 1) % MAX_QUEUE;
    s->qcount--;
  }
  s->state = AL_STOPPED;
  s->pos = 0;
}

static void alSourceQueueBuffers_locked(ALuint id, ALsizei n, const ALuint *bufs) {
  Source *s = src_get(id);
  if (!s) { al_error = AL_INVALID_NAME; return; }
  for (ALsizei k = 0; k < n; k++) {
    if (s->qcount >= MAX_QUEUE) { al_error = AL_INVALID_VALUE; return; }
    s->queue[(s->qhead + s->qcount) % MAX_QUEUE] = (int)bufs[k];
    s->qcount++;
  }
  if (!s->cur && s->state == AL_PLAYING) {
    s->cur = s->queue[s->qhead];
    s->qhead = (s->qhead + 1) % MAX_QUEUE;
    s->qcount--;
    s->pos = 0;
  }
}

/* The engine's streaming loop is: query AL_BUFFERS_PROCESSED, unqueue that
 * many, refill them, queue them again. Reporting `processed` honestly is what
 * keeps the music advancing; reporting it early causes an audible repeat. */
static void alSourceUnqueueBuffers_locked(ALuint id, ALsizei n, ALuint *out) {
  Source *s = src_get(id);
  if (!s) { al_error = AL_INVALID_NAME; return; }
  if (n < 0 || n > s->dcount || !out) { al_error = AL_INVALID_VALUE; return; }

  /* Hand back the real ids, oldest first. The engine refills and re-queues
   * exactly these. */
  for (ALsizei k = 0; k < n; k++) {
    out[k] = (ALuint)s->done[s->dhead];
    s->dhead = (s->dhead + 1) % MAX_QUEUE;
    s->dcount--;
  }
}

void alListenerfv(ALenum param, const ALfloat *v) { (void)param; (void)v; }

ALenum alGetError(void) {
  AL_LOCK();
  const ALenum e = al_error;
  al_error = AL_NO_ERROR;
  AL_UNLOCK();
  return e;
}


/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

/* Thin locking wrappers around the *_locked bodies above.
 *
 * Written this way deliberately. The first attempt put AL_LOCK()/AL_UNLOCK()
 * inline in each body, which silently missed the one-line early returns --
 * `if (!s) { al_error = AL_INVALID_NAME; return; }` -- and every one of those
 * would have deadlocked the next caller. A wrapper cannot have that bug: there
 * is exactly one return path and it is visible. */
void alGenBuffers(ALsizei n, ALuint *out) {
  AL_LOCK();
  alGenBuffers_locked(n, out);
  AL_UNLOCK();
}
void alDeleteBuffers(ALsizei n, const ALuint *ids) {
  AL_LOCK();
  alDeleteBuffers_locked(n, ids);
  AL_UNLOCK();
}
void alBufferData(ALuint id, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei freq) {
  AL_LOCK();
  alBufferData_locked(id, format, data, size, freq);
  AL_UNLOCK();
}
void alGenSources(ALsizei n, ALuint *out) {
  AL_LOCK();
  alGenSources_locked(n, out);
  AL_UNLOCK();
}
void alDeleteSources(ALsizei n, const ALuint *ids) {
  AL_LOCK();
  alDeleteSources_locked(n, ids);
  AL_UNLOCK();
}
void alSourcei(ALuint id, ALenum param, ALint v) {
  AL_LOCK();
  alSourcei_locked(id, param, v);
  AL_UNLOCK();
}
void alSourcef(ALuint id, ALenum param, ALfloat v) {
  AL_LOCK();
  alSourcef_locked(id, param, v);
  AL_UNLOCK();
}
void alGetSourcei(ALuint id, ALenum param, ALint *out) {
  AL_LOCK();
  alGetSourcei_locked(id, param, out);
  AL_UNLOCK();
}
void alGetSourcef(ALuint id, ALenum param, ALfloat *out) {
  AL_LOCK();
  alGetSourcef_locked(id, param, out);
  AL_UNLOCK();
}
void alSourcePlay(ALuint id) {
  AL_LOCK();
  alSourcePlay_locked(id);
  AL_UNLOCK();
}
void alSourcePause(ALuint id) {
  AL_LOCK();
  alSourcePause_locked(id);
  AL_UNLOCK();
}
void alSourceStop(ALuint id) {
  AL_LOCK();
  alSourceStop_locked(id);
  AL_UNLOCK();
}
void alSourceQueueBuffers(ALuint id, ALsizei n, const ALuint *bufs) {
  AL_LOCK();
  alSourceQueueBuffers_locked(id, n, bufs);
  AL_UNLOCK();
}
void alSourceUnqueueBuffers(ALuint id, ALsizei n, ALuint *out) {
  AL_LOCK();
  alSourceUnqueueBuffers_locked(id, n, out);
  AL_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* mixing                                                              */
/* ------------------------------------------------------------------ */

/* Move a finished buffer id onto the processed FIFO. */
static void retire(Source *s, int buffer_id) {
  if (s->dcount >= MAX_QUEUE) {
    /* The engine is not draining. Drop the oldest rather than overwrite a
     * live entry; losing an id is recoverable, corrupting the ring is not. */
    s->dhead = (s->dhead + 1) % MAX_QUEUE;
    s->dcount--;
  }
  s->done[(s->dhead + s->dcount) % MAX_QUEUE] = buffer_id;
  s->dcount++;
}

static inline int16_t clamp16(int v) {
  return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* One voice into the accumulator. Linear interpolation on the read cursor
 * gives pitch; constant-power panning gives the stereo placement the engine
 * asks for with AL_POSITION. */
static void mix_source(Source *s, int32_t *acc, int frames) {
  Buffer *b = buf_get((ALuint)s->cur);
  if (!b || !b->pcm || b->frames <= 0) return;

  /* Recomputed after every buffer swap below: a queue is not guaranteed to
   * hold buffers of one sample rate, and a stale step would resample the new
   * buffer at the old buffer's rate. */
  double step = (double)b->rate / MIX_RATE * s->pitch;
  const float  g    = s->gain * master_gain;
  const float  gl   = g * sqrtf(0.5f * (1.0f - s->pan));
  const float  gr   = g * sqrtf(0.5f * (1.0f + s->pan));

  for (int i = 0; i < frames; i++) {
    if (s->pos >= b->frames) {
      if (s->looping) {
        s->pos -= b->frames;
      } else if (s->qcount > 0) {
        /* Streaming: retire this buffer and pull the next one in. */
        retire(s, s->cur);
        s->cur = s->queue[s->qhead];
        s->qhead = (s->qhead + 1) % MAX_QUEUE;
        s->qcount--;
        s->pos -= b->frames;
        b = buf_get((ALuint)s->cur);
        if (!b || !b->pcm || b->frames <= 0) { s->state = AL_STOPPED; return; }
        step = (double)b->rate / MIX_RATE * s->pitch;
        continue;
      } else {
        retire(s, s->cur);
        s->state = AL_STOPPED;
        s->cur = 0;
        s->pos = 0;
        return;
      }
    }

    const int   i0 = (int)s->pos;
    const int   i1 = (i0 + 1 < b->frames) ? i0 + 1 : i0;
    const float fr = (float)(s->pos - i0);

    float l, r;
    if (b->channels == 2) {
      const float a0 = b->pcm[i0 * 2],     a1 = b->pcm[i1 * 2];
      const float c0 = b->pcm[i0 * 2 + 1], c1 = b->pcm[i1 * 2 + 1];
      l = a0 + (a1 - a0) * fr;
      r = c0 + (c1 - c0) * fr;
    } else {
      const float a0 = b->pcm[i0], a1 = b->pcm[i1];
      l = r = a0 + (a1 - a0) * fr;
    }

    acc[i * 2]     += (int32_t)(l * gl);
    acc[i * 2 + 1] += (int32_t)(r * gr);
    s->pos += step;
  }
}

static void fill(int16_t *dst, int frames) {
  static int32_t acc[MIX_FRAMES * MIX_CHANNELS];
  memset(acc, 0, sizeof(int32_t) * (size_t)frames * MIX_CHANNELS);

  if (!suspended) {
    for (int i = 1; i < MAX_SOURCES; i++) {
      Source *s = &sources[i];
      if (s->used && s->state == AL_PLAYING) mix_source(s, acc, frames);
    }
  }
  for (int i = 0; i < frames * MIX_CHANNELS; i++) dst[i] = clamp16(acc[i]);
}

/* ------------------------------------------------------------------ */
/* audren plumbing                                                     */
/* ------------------------------------------------------------------ */

static const AudioRendererConfig ar_cfg = {
  .output_rate     = AudioRendererOutputRate_48kHz,
  .num_voices      = 4,
  .num_effects     = 0,
  .num_sinks       = 1,
  .num_mix_objs    = 1,
  .num_mix_buffers = 2,
};

int osmos_al_init(void) {
  mutexInit(&al_lock);
  master_gain = cfg_master_volume();

  if (R_FAILED(audrenInitialize(&ar_cfg))) return 0;
  if (R_FAILED(audrvCreate(&drv, &ar_cfg, MIX_CHANNELS))) { audrenExit(); return 0; }

  const size_t frame_bytes = MIX_FRAMES * MIX_CHANNELS * sizeof(int16_t);
  const size_t pool_size = (frame_bytes * NUM_WAVEBUFS + 0xFFF) & ~0xFFFUL;
  mempool = memalign(0x1000, pool_size);
  if (!mempool) { audrvClose(&drv); audrenExit(); return 0; }
  memset(mempool, 0, pool_size);
  mixmem = mempool;

  const int mpid = audrvMemPoolAdd(&drv, mempool, pool_size);
  audrvMemPoolAttach(&drv, mpid);

  static const u8 sink_ch[] = { 0, 1 };
  audrvDeviceSinkAdd(&drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_ch);
  audrvUpdate(&drv);
  audrenStartAudioRenderer();

  audrvVoiceInit(&drv, 0, MIX_CHANNELS, PcmFormat_Int16, MIX_RATE);
  audrvVoiceSetDestinationMix(&drv, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&drv, 0, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&drv, 0, 1.0f, 1, 1);
  audrvVoiceStart(&drv, 0);

  for (int i = 0; i < NUM_WAVEBUFS; i++) {
    wavebufs[i].data_raw        = mempool;
    wavebufs[i].size            = pool_size;
    wavebufs[i].start_sample_offset = i * MIX_FRAMES;
    wavebufs[i].end_sample_offset   = (i + 1) * MIX_FRAMES;
  }
  audio_ready = 1;
  return 1;
}

/* Called once per frame from main.c. Any wavebuf the driver has finished with
 * gets refilled and requeued. At 60 fps and 20 ms per buffer this keeps three
 * buffers in flight, which absorbs a slow frame without a dropout. */
void osmos_al_update(void) {
  if (!audio_ready) return;
  for (int i = 0; i < NUM_WAVEBUFS; i++) {
    if (wavebufs[i].state == AudioDriverWaveBufState_Free ||
        wavebufs[i].state == AudioDriverWaveBufState_Done) {
      int16_t *dst = mixmem + (size_t)i * MIX_FRAMES * MIX_CHANNELS;
      AL_LOCK();                 /* the loader thread may be in alBufferData */
      fill(dst, MIX_FRAMES);
      AL_UNLOCK();
      armDCacheFlush(dst, MIX_FRAMES * MIX_CHANNELS * sizeof(int16_t));
      audrvVoiceAddWaveBuf(&drv, 0, &wavebufs[i]);
    }
  }
  audrvUpdate(&drv);
}

void osmos_al_suspend(void) { suspended = 1; }
void osmos_al_resume(void)  { suspended = 0; }

void osmos_al_shutdown(void) {
  if (!audio_ready) return;
  audrvVoiceStop(&drv, 0);
  audrvUpdate(&drv);
  audrenStopAudioRenderer();
  audrvClose(&drv);
  audrenExit();
  free(mempool);
  mempool = NULL; mixmem = NULL; audio_ready = 0;
}

/* ------------------------------------------------------------------ */
/* ALC -- single implicit device and context                           */
/* ------------------------------------------------------------------ */

static int the_device  = 0xA1;
static int the_context = 0xC1;

void *alcOpenDevice(const char *name)            { (void)name; return &the_device; }
int   alcCloseDevice(void *dev)                  { (void)dev; return 1; }
void *alcCreateContext(void *d, const ALint *a)  { (void)d; (void)a; return &the_context; }
int   alcMakeContextCurrent(void *ctx)           { (void)ctx; return 1; }
void  alcDestroyContext(void *ctx)               { (void)ctx; }
void *alcGetCurrentContext(void)                 { return &the_context; }
void *alcGetContextsDevice(void *ctx)            { (void)ctx; return &the_device; }
void  alcProcessContext(void *ctx)               { (void)ctx; osmos_al_resume(); }
void  alcSuspendContext(void *ctx)               { (void)ctx; osmos_al_suspend(); }
void *alcGetJavaVM(void)                         { return jni_vm(); }
