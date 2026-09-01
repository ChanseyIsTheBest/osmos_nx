/* osmos_diag.c -- thread registry, wait beacons and a stall watchdog.
 *
 * MIT licensed. See LICENSE and osmos_diag.h.
 *
 * Ported from the Killer Bean port's diag.c. Two of its hard-won details are
 * preserved verbatim in spirit because they are not obvious and are expensive
 * to rediscover:
 *
 *   1. NEVER log while a thread is paused. Collect into a plain buffer, resume
 *      the thread, and only then write. The paused thread is very likely
 *      holding the stdio lock -- during boot every thread logs constantly --
 *      and logging into it freezes the whole process, turning the diagnostic
 *      into a second, worse hang.
 *
 *   2. Validate stack-scan hits. A raw scan of the stack for values that look
 *      like code addresses reports jump-table targets, vtable pointers and
 *      stale frames. A real return address has a BL or BLR in the four bytes
 *      before it. That check is what makes the backtrace trustworthy rather
 *      than merely plausible.
 *
 * Dropped from the original: the Boehm GC stop-the-world bridge. Osmos has no
 * GC, and its thread count is two rather than thirty, so the "which threads
 * are worth snapshotting" filter is gone too -- all of them are.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "osmos_diag.h"

#if OSMOS_DIAG
#include "so_util.h"
#include "osmos_paths.h"
#include "libc_shim.h"
#include "osmos_io.h"

#define DIAG_MAX_THREADS  16
#define DIAG_POLL_NS      (1000ull * 1000ull * 1000ull)   /* tick every 1 s   */
#define DIAG_STALL_NS     (8000ull * 1000ull * 1000ull)   /* stall after 8 s  */
#define DIAG_REDUMP_NS    (15000ull * 1000ull * 1000ull)  /* re-dump cadence  */

typedef struct {
  volatile int         in_use;
  u64                  tid;          /* svcGetThreadId: matches crash reports */
  Handle               handle;       /* for svcGetThreadContext3              */
  char                 name[32];
  const void          *entry;
  int                  is_main;
  volatile int         wait_kind;
  volatile const void *wait_obj;
  volatile u64         wait_since;
  volatile u64         waits_total, wakes_total;
} DiagThread;

static DiagThread g_threads[DIAG_MAX_THREADS];
static Mutex      g_reg_lock;
static __thread DiagThread *self;

/* ---- progress signals ---- */
static atomic_ullong n_fopen, n_fopen_fail, n_yield, n_bytes;
static atomic_ullong n_written, n_malloc_fail;
static volatile u64  g_last_progress;
static volatile unsigned long long g_frame;
static const char *volatile g_phase = "startup";

static char  last_file[192];
static Mutex last_file_lock;

/* The watchdog writes through a raw descriptor, not through the FILE* the
 * rest of the port logs to.
 *
 * If the wedged thread is inside malloc or inside stdio, it holds newlib's
 * malloc lock or the FILE lock, and the watchdog calling fprintf would block
 * on the very thing it is trying to report. write(2) on a pre-opened fd with
 * a static buffer touches neither. The normal log is written too, but only
 * AFTER the raw write, so a hang there cannot cost us the dump. */
static int wd_fd = -1;

static void wd_out(const char *fmt, ...) {
  static char buf[1024];             /* watchdog thread only; no malloc */
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
  buf[n++] = '\n';
  buf[n] = 0;

  if (wd_fd >= 0) {
    write(wd_fd, buf, (size_t)n);
    fsync(wd_fd);                    /* a hang produces no close(); flush now */
  }
  LOGI("%.*s", n - 1, buf);
}

static volatile int g_wd_started;
static volatile bool g_wd_run;
static Thread g_wd_thread;

static inline u64 now_tick(void)      { return armGetSystemTick(); }
static inline u64 tick_ns(u64 t)      { return armTicksToNs(t); }
static inline void progress(void)     { g_last_progress = now_tick(); }

/* Nanoseconds since the last progress signal, clamped at zero.
 *
 * The watchdog samples `now`, then does arithmetic with g_last_progress -- and
 * the main thread can call progress() in between, leaving g_last_progress
 * AHEAD of now. The unsigned subtraction then wrapped to ~1.8e19 ticks, which
 * is why every dump was stamped "1537228672.8s since last progress" and why
 * the stall check fired at a steady 58 fps. */
static u64 idle_ns(u64 now) {
  const u64 last = g_last_progress;
  return (now > last) ? tick_ns(now - last) : 0;
}

static const char *wait_kind_name(int k) {
  switch (k) {
    case DIAG_W_COND:  return "cond";
    case DIAG_W_JOIN:  return "join";
    case DIAG_W_MUTEX: return "mutex";
    case DIAG_W_YIELD: return "yield";
    default:           return "-";
  }
}

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

void diag_thread_register(const void *entry, int is_main) {
  if (self) return;
  mutexLock(&g_reg_lock);
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use) continue;
    DiagThread *t = &g_threads[i];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->entry = entry;
    t->is_main = is_main;
    t->handle = threadGetCurHandle();
    svcGetThreadId(&t->tid, t->handle);
    snprintf(t->name, sizeof(t->name), "%s", is_main ? "main" : "engine");
    self = t;
    break;
  }
  mutexUnlock(&g_reg_lock);
}

void diag_thread_unregister(void) {
  if (!self) return;
  mutexLock(&g_reg_lock);
  self->in_use = 0;
  self = NULL;
  mutexUnlock(&g_reg_lock);
}

void diag_wait_enter(int kind, const void *obj) {
  if (!self) return;
  self->wait_kind = kind;
  self->wait_obj = obj;
  self->wait_since = now_tick();
  self->waits_total++;
}

void diag_wait_exit(void) {
  if (!self) return;
  self->wait_kind = DIAG_W_NONE;
  self->wait_obj = NULL;
  self->wait_since = 0;
  self->wakes_total++;
}

void diag_phase(const char *p) {
  g_phase = p ? p : "?";
  progress();
  LOGI("phase: %s", g_phase);

  /* Self-check. diag_start() went missing once -- an edit to main.c was lost --
   * and the only symptom was the absence of a stall dump, which reads exactly
   * like "the watchdog ran and found nothing". Silence is the worst possible
   * failure mode for a diagnostic, so say so out loud. */
  static int warned;
  if (!g_wd_started && !warned) {
    warned = 1;
    LOGW("watchdog is NOT running -- diag_start() was never called, so no "
         "stall dump will appear no matter how long this hangs");
  }
}

void diag_frame(unsigned long long frame) {
  g_frame = frame;
  progress();
}

/* ------------------------------------------------------------------ */
/* wrapped libc                                                        */
/* ------------------------------------------------------------------ */

/* The engine keeps its own log, and it is far more informative about the game
 * than anything this port can see from outside -- it records which assets
 * failed, which shaders compiled, and every font error. It lives in the data
 * directory, so collecting it means asking for a second file every time.
 *
 * Instead, notice when the engine opens it and tee its writes into debug.log.
 * One file to send, and the engine's own diagnosis is interleaved with ours in
 * the right order. */
static FILE *g_engine_log;

static int is_engine_log(const char *path) {
  if (!path) return 0;
  const size_t n = strlen(path);
  return n >= 9 && strcmp(path + n - 9, "Osmos.log") == 0;
}

FILE *fopen_diag(const char *path, const char *mode) {
  FILE *f = fopen_locked(path, mode);
  atomic_fetch_add(&n_fopen, 1);
  if (!f) {
    atomic_fetch_add(&n_fopen_fail, 1);
    /* Name every failure. There were twelve in the last run and the counter
     * alone said nothing about which; failures are rare enough that logging
     * each one cannot flood. */
    LOGW("open FAILED (%s): %s", mode ? mode : "?", path ? path : "(null)");
  }
  if (path) {
    /* Only the most recent path is kept: 227 lines for the textures alone
     * would slow down the very thing being measured. */
    mutexLock(&last_file_lock);
    snprintf(last_file, sizeof(last_file), "%s%s", f ? "" : "(FAILED) ", path);
    mutexUnlock(&last_file_lock);
  }
  if (f && is_engine_log(path)) {
    g_engine_log = f;
    LOGI("engine log opened (%s); its output is mirrored below as [osmos]", path);
  }
  progress();
  return f;
}

size_t fread_diag(void *p, size_t sz, size_t n, FILE *f) {
  const size_t got = fread_locked(p, sz, n, f);
  atomic_fetch_add(&n_bytes, (unsigned long long)(got * sz));
  progress();
  return got;
}

/* Writes were not counted, and that misread the last run badly. The engine was
 * retrying a failed font init on every text measurement and logging each
 * failure, so it was writing to SD continuously -- progressing, just slowly.
 * With only reads counted that looked exactly like a hard stall. */
size_t fwrite_diag(const void *p, size_t sz, size_t n, FILE *f) {
  const size_t put = fwrite_locked(p, sz, n, f);
  atomic_fetch_add(&n_written, (unsigned long long)(put * sz));

  if (f && f == g_engine_log && p && put) {
    /* Copy out, trim the trailing newline, and log a line at a time. The
     * engine writes wide text converted to UTF-8, so it is already printable. */
    char buf[512];
    size_t len = put * sz;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    if (len) LOGI("[osmos] %s", buf);
  }

  progress();
  return put;
}

/* A NULL from malloc is the quietest possible failure: FT_Init_FreeType just
 * returns an error and the engine logs a message no one reads. */
void *malloc_diag(size_t n) {
  void *p = malloc(n);
  if (!p && n) {
    atomic_fetch_add(&n_malloc_fail, 1);
    LOGW("malloc(%llu) returned NULL", (unsigned long long)n);
  }
  return p;
}

/* FreeType's FT_Stream_Open uses open(), not fopen(), so font loading has been
 * invisible in every log so far -- and fonts are exactly what the main menu
 * needs. libosmos.so imports the FORTIFY spelling __open_2, which libc_shim
 * translates; wrap that. */
int __open_2_diag(const char *path, int flags) {
  const int fd = __open_2_locked(path, flags);
  atomic_fetch_add(&n_fopen, 1);
  if (fd < 0) {
    atomic_fetch_add(&n_fopen_fail, 1);
    LOGW("open FAILED (fd, flags=0x%x): %s", flags, path ? path : "(null)");
  } else if (path && strstr(path, "font")) {
    LOGI("font open ok: %s (fd %d)", path, fd);
  }
  if (path) {
    mutexLock(&last_file_lock);
    snprintf(last_file, sizeof(last_file), "%s%s", fd < 0 ? "(FAILED) " : "", path);
    mutexUnlock(&last_file_lock);
  }
  progress();
  return fd;
}

int sched_yield_diag(void) {
  atomic_fetch_add(&n_yield, 1);
  /* Deliberately NOT progress(): a thread spinning on yield is exactly the
   * case this watchdog exists to catch, so yielding must not count as
   * getting anywhere. */
  if (self) self->wait_kind = DIAG_W_YIELD;
  return sched_yield_fake();
}

/* ------------------------------------------------------------------ */
/* symbolication                                                       */
/* ------------------------------------------------------------------ */

static u64 g_nro_base, g_nro_size;

static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init)) && mi.size) {
    g_nro_base = mi.addr;
    g_nro_size = mi.size;
  }
}

/* "libosmos.so+0x190cd8" is directly feedable to the symbol lookup in
 * tools/, which is the whole point of printing it this way. */
static void resolve_addr(char *buf, size_t n, u64 addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    const char *base = m->name, *p;
    for (p = m->name; *p; p++)
      if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(buf, n, "%.24s+0x%llx", base,
             (unsigned long long)(addr - (u64)(uintptr_t)m->load_virtbase));
  } else if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size) {
    snprintf(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  } else {
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
  }
}

/* ------------------------------------------------------------------ */
/* buffered dump -- see the header comment on why this is not logged live */
/* ------------------------------------------------------------------ */

static char   g_buf[8192];
static size_t g_len;

static void emit(const char *fmt, ...) {
  if (g_len >= sizeof(g_buf) - 1) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(g_buf + g_len, sizeof(g_buf) - g_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_len += (size_t)n;
    if (g_len > sizeof(g_buf) - 1) g_len = sizeof(g_buf) - 1;
  }
}

static void dump_context(const char *name, const ThreadContext *ctx) {
  char a[48], b[48];
  resolve_addr(a, sizeof(a), ctx->pc.x);
  resolve_addr(b, sizeof(b), ctx->lr);
  emit("  %s  PC=%s  LR=%s\n", name, a, b);
  emit("    SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx X2=0x%llx\n",
       (unsigned long long)ctx->sp, (unsigned long long)ctx->fp,
       (unsigned long long)ctx->cpu_gprs[0].x,
       (unsigned long long)ctx->cpu_gprs[1].x,
       (unsigned long long)ctx->cpu_gprs[2].x);

  /* Bound every dereference to the thread's mapped stack, or a wild frame
   * pointer faults the watchdog itself and takes the diagnostic with it. */
  u64 slo = 0, shi = 0;
  { MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx->sp)) && mi.size) {
      slo = mi.addr; shi = mi.addr + mi.size;
    } }

  u64 fp = ctx->fp;
  for (int depth = 0; depth < 24 && (fp & 7) == 0; depth++) {
    if (slo) { if (fp < slo || fp + 16 > shi) break; }
    else if (fp < 0x1000) break;
    const u64 nextfp = ((const u64 *)(uintptr_t)fp)[0];
    const u64 lr     = ((const u64 *)(uintptr_t)fp)[1];
    if (!lr) break;
    char s[48];
    resolve_addr(s, sizeof(s), lr);
    emit("    bt[%d] %s\n", depth, s);
    if (nextfp <= fp) break;          /* the chain must climb */
    fp = nextfp;
  }

  /* The frame-pointer chain dead-ends wherever the engine used a leaf or a
   * hand-written stub, so scan the live stack for return addresses it missed.
   * The BL/BLR check is what separates real return addresses from vtable
   * pointers and stale frames that merely look like code. */
  if (!slo) return;
  u64 sp = ctx->sp & ~7ull;
  if (sp < slo) sp = slo;
  u64 top = sp + 0x2000;
  if (top > shi) top = shi;

  int printed = 0;
  for (u64 addr = sp; addr + 8 <= top && printed < 20; addr += 8) {
    const u64 v = ((const u64 *)(uintptr_t)addr)[0];
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
    if (!m) continue;
    { MemoryInfo pmi; u32 ppi;
      if (R_FAILED(svcQueryMemory(&pmi, &ppi, v - 4)) || !(pmi.perm & Perm_R)) continue; }
    const u32 prev = ((const u32 *)(uintptr_t)(v - 4))[0];
    const int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
    const int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
    if (!is_bl && !is_blr) continue;
    char s[48];
    resolve_addr(s, sizeof(s), v);
    emit("    ret@0x%-4llx %s%s\n", (unsigned long long)(addr - sp), s,
         is_blr ? " (via blr)" : "");
    printed++;
  }
}

/* Pause only long enough to snapshot; resume BEFORE writing anything. */
static void snapshot(DiagThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;

  ThreadContext ctx;
  const Result pr = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  const Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_FAILED(gr)) {
    if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
    wd_out("  %s: snapshot failed rc=0x%x", t->name, (unsigned)gr);
    return;
  }

  g_len = 0; g_buf[0] = 0;
  dump_context(t->name[0] ? t->name : "?", &ctx);

  svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  wd_out("%s", g_buf);                     /* only now is output safe */
}

static u64 prev_waits[DIAG_MAX_THREADS], prev_wakes[DIAG_MAX_THREADS];

static void dump_all(int episode, u64 now) {
  const u64 ns = idle_ns(now);
  char file[sizeof(last_file)];
  mutexLock(&last_file_lock);
  memcpy(file, last_file, sizeof(file));
  mutexUnlock(&last_file_lock);

  wd_out("[wd] ===== dump #%d: %llu.%llus since last progress =====",
         episode, (unsigned long long)(ns / 1000000000ull),
         (unsigned long long)((ns % 1000000000ull) / 100000000ull));
  wd_out("  phase=%s  frame=%llu", g_phase, (unsigned long long)g_frame);
  wd_out("  opens=%llu (%llu failed)  read=%lluKB  yields=%llu",
       (unsigned long long)atomic_load(&n_fopen),
       (unsigned long long)atomic_load(&n_fopen_fail),
       (unsigned long long)(atomic_load(&n_bytes) / 1024),
       (unsigned long long)atomic_load(&n_yield));
  wd_out("  last file: %s", file[0] ? file : "(none)");
  wd_out("  %-8s %-10s %-7s %-18s %6s  d_wait d_wake", "name", "tid", "state",
       "wait_obj", "secs");

  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    const int kind = t->wait_kind;
    const u64 since = t->wait_since;
    const u64 parked = (kind != DIAG_W_NONE && since) ? tick_ns(now - since) : 0;
    const u64 dw = t->waits_total - prev_waits[i];
    const u64 dk = t->wakes_total - prev_wakes[i];
    prev_waits[i] = t->waits_total;
    prev_wakes[i] = t->wakes_total;
    wd_out("  %-8s %-10llu %-7s 0x%-16llx %3llu.%llu  %6llu %6llu%s",
         t->name[0] ? t->name : "?", (unsigned long long)t->tid,
         wait_kind_name(kind), (unsigned long long)(uintptr_t)t->wait_obj,
         (unsigned long long)(parked / 1000000000ull),
         (unsigned long long)((parked % 1000000000ull) / 100000000ull),
         (unsigned long long)dw, (unsigned long long)dk,
         t->is_main ? "  <main>" : "");
  }
  wd_out("  legend: d_wait/d_wake are deltas since the last dump. 0/0 means hard "
       "parked; d_wait > d_wake means it entered a wait it has not left.");

  wd_out("--- CPU contexts ---");
  for (int i = 0; i < DIAG_MAX_THREADS; i++)
    if (g_threads[i].in_use) snapshot(&g_threads[i]);
}

/* ------------------------------------------------------------------ */
/* watchdog                                                            */
/* ------------------------------------------------------------------ */

static void wd_main(void *unused) {
  (void)unused;
  u64 last_dump = 0, last_probe = 0, last_beat = 0;
  int episode = 0, probes = 0;
  unsigned long long prev_opens = 0, prev_bytes = 0, prev_yields = 0;

  wd_out("[wd] watchdog thread running");

  while (g_wd_run) {
    for (int i = 0; i < 10 && g_wd_run; i++) svcSleepThread(DIAG_POLL_NS / 10);
    if (!g_wd_run) break;
    const u64 now = now_tick();

    /* ---- heartbeat, unconditional ----------------------------------- *
     * Proves the watchdog is alive and shows whether the counters move.
     * Without this, "the watchdog never ran" and "the watchdog ran and saw
     * no stall" produce the same empty log, and telling those apart cost a
     * whole test cycle. */
    if (!last_beat || tick_ns(now - last_beat) >= 3000000000ull) {
      last_beat = now;
      const unsigned long long o = atomic_load(&n_fopen);
      const unsigned long long b = atomic_load(&n_bytes);
      const unsigned long long y = atomic_load(&n_yield);
      char file[sizeof(last_file)];
      mutexLock(&last_file_lock);
      memcpy(file, last_file, sizeof(file));
      mutexUnlock(&last_file_lock);
      const unsigned long long w = atomic_load(&n_written);
      wd_out("[wd] alive  phase=%s  frame=%llu  opens=%llu(+%llu,%llu failed) "
             "read=%lluKB(+%lluKB) wrote=%lluKB yields=%llu(+%llu) "
             "mallocfail=%llu  last=%s",
             g_phase, (unsigned long long)g_frame,
             o, o - prev_opens, (unsigned long long)atomic_load(&n_fopen_fail),
             b / 1024, (b - prev_bytes) / 1024, w / 1024,
             y, y - prev_yields,
             (unsigned long long)atomic_load(&n_malloc_fail),
             file[0] ? file : "(none)");
      prev_opens = o; prev_bytes = b; prev_yields = y;
    }

    /* ---- bounded periodic probe during bring-up ---------------------- *
     * A stall is declared only when NOTHING moves, but a retry loop that
     * keeps reopening the same file moves the counters forever and would
     * never trip it. Dumping unconditionally a few times during loading
     * catches exactly that case, which the stall check cannot. */
    if (probes < 12 && (!last_probe || tick_ns(now - last_probe) >= 5000000000ull)) {
      last_probe = now;
      probes++;
      wd_out("[wd] ---- periodic probe #%d ----", probes);
      dump_all(1000 + probes, now);
      last_dump = now;
      continue;
    }

    /* ---- stall ------------------------------------------------------- */
    if (idle_ns(now) < DIAG_STALL_NS) { last_dump = 0; continue; }
    if (last_dump && tick_ns(now - last_dump) < DIAG_REDUMP_NS) continue;
    last_dump = now;
    dump_all(++episode, now);
  }
}

void diag_start(void) {
  if (g_wd_started) return;
  mutexInit(&g_reg_lock);
  mutexInit(&last_file_lock);
  nro_range_init();
  progress();

  { char p[FS_MAX_PATH];
    snprintf(p, sizeof(p), "%s/watchdog.log", osmos_root());
    wd_fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); }

  diag_thread_register(NULL, 1);           /* the main thread */

  g_wd_run = true;

  /* Priority 0x2C -- the SAME band as the main thread, not below it.
   *
   * An earlier version used 0x2F on the reasoning that the watchdog should
   * never starve the thread it observes. On Horizon a higher number is a
   * LOWER priority, so that reasoning was exactly backwards: a 0x2F thread
   * sharing a core with a 0x2C thread that spins without ever blocking is
   * never scheduled at all. The watchdog armed, logged that it had armed, and
   * then never ran again -- which looks identical to a watchdog that ran and
   * found nothing.
   *
   * Belt and braces: try core 2 first so it runs concurrently regardless of
   * priority, and fall back to the default core if that is unavailable. */
  Result rc = threadCreate(&g_wd_thread, wd_main, NULL, NULL, 0x8000, 0x2C, 2);
  if (R_FAILED(rc))
    rc = threadCreate(&g_wd_thread, wd_main, NULL, NULL, 0x8000, 0x2C, -2);
  if (R_FAILED(rc)) {
    LOGW("watchdog: threadCreate failed rc=0x%x; continuing without it", (unsigned)rc);
    return;
  }
  if (R_FAILED(threadStart(&g_wd_thread))) {
    LOGW("watchdog: threadStart failed");
    threadClose(&g_wd_thread);
    return;
  }
  g_wd_started = 1;
  LOGI("watchdog: armed (dumps after %llus without progress)",
       (unsigned long long)(DIAG_STALL_NS / 1000000000ull));
}

void diag_stop(void) {
  if (wd_fd >= 0) { close(wd_fd); wd_fd = -1; }
  if (!g_wd_started) return;
  g_wd_run = false;
  threadWaitForExit(&g_wd_thread);
  threadClose(&g_wd_thread);
  g_wd_started = 0;
}

#endif  /* OSMOS_DIAG */
