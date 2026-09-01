/* main.c -- osmos_nx entry point and frame loop.
 *
 * MIT licensed. See LICENSE. Ships no game code and no game assets.
 *
 * WHY THIS PORT LOOKS DIFFERENT FROM THE OTHER TWO
 * ------------------------------------------------
 * The Sonic Jump and Killer Bean wrappers both target NativeActivity games:
 * the game owns the main loop, and the wrapper has to impersonate Android
 * around it (ALooper, AInputQueue, ANativeWindow, AAssetManager, the whole
 * activity lifecycle).
 *
 * Osmos predates that. It is a GLSurfaceView game: the Java side owned the
 * loop and called static native methods on
 * com.hemispheregames.osmos.wrappers.OsmosJNILib. So *we* drive it. That is
 * why this file has a visible frame loop and no android_native.c.
 *
 * Two consequences worth stating outright, both verified against the library:
 *
 *   - libosmos.so imports exactly one symbol from libandroid.so
 *     (android_set_abort_message, which is really libc). No asset manager,
 *     no looper, no native window.
 *
 *   - libosmos.so imports NO egl* symbols at all. On Android, GLSurfaceView
 *     owned the context and did the swap. So this file owns EGL outright and
 *     the game never sees it.
 */

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <malloc.h>

#include "config.h"
#include "so_util.h"
#include "imports.h"   /* dynlib_functions, dynlib_numfunctions */
#include "util.h"
#include "osmos_jni.h"
#include "osmos_al.h"
#include "nx_pointer.h"
#include "osmos_io.h"
#include "osmos_paths.h"
#include "osmos_diag.h"
#include "osmos_tune.h"
#include "osmos_gl.h"
#include "compat_stubs.h"
#include "error.h"

/* ------------------------------------------------------------------ */
/* The module                                                          */
/* ------------------------------------------------------------------ */

so_module osmos_mod;

/* Heap reserved for the loaded module's LOAD segments. libosmos.so is ~3 MB
 * of file image with a .bss that brings the mapped size to about 4 MB; 16 MB
 * leaves room for a future build without another round of tuning. */
#define SO_LOAD_SIZE (16 * 1024 * 1024)
static uint8_t *so_load_area;

/* Every thread that runs module code needs its own bionic TLS block: the
 * engine's stack-protector prologues read the canary from TPIDR_EL0+0x28.
 * There are 1126 tpidr_el0 sites in .text, so this is not optional and it is
 * not only the main thread. libc_shim's pthread_create_fake installs one for
 * spawned threads; this is the main thread's. */
static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/* The game's JNI entry points                                         */
/* ------------------------------------------------------------------ */

/* Signatures below are the JNI ABI for a *static* native method:
 *     (JNIEnv *env, jclass cls, <declared args...>)
 * The declared args come from the Java declarations in OsmosJNILib, which are
 * reproduced in comments so the two can be diffed by eye. */

typedef void (*fn_v)(void *env, void *cls);
typedef void (*fn_i)(void *env, void *cls, int a);
typedef void (*fn_ii)(void *env, void *cls, int a, int b);
typedef void (*fn_f)(void *env, void *cls, float a);
typedef void (*fn_s)(void *env, void *cls, void *jstr);
typedef void (*fn_ffi)(void *env, void *cls, float x, float y, int id);
typedef void (*fn_zz)(void *env, void *cls, int locked, int enabled);
typedef int  (*fn_ret_z)(void *env, void *cls);

static struct {
  fn_v   InitAppDelegate;      /* ()V   */
  fn_i   InitRenderer;         /* (I)V  */
  fn_ii  Changed;              /* (II)V */
  fn_v   ActivateGame;         /* ()V   */
  fn_v   Render;               /* ()V   */
  fn_v   RecreateFBO;          /* ()V   */

  fn_s   ProvideDataDir;       /* (Ljava/lang/String;)V */
  fn_s   ProvideLang;          /* (Ljava/lang/String;)V */
  fn_s   ProvideUUID;          /* (Ljava/lang/String;)V */
  fn_f   ProvideDensity;       /* (F)V  */

  fn_ffi TouchBegan;           /* (FFI)V */
  fn_ffi TouchMoved;           /* (FFI)V */
  fn_ffi TouchEnded;           /* (FFI)V */
  fn_ffi TouchCancelled;       /* (FFI)V */

  fn_ret_z BackButtonPressed;  /* ()Z   */
  fn_v   PauseGame;            /* ()V   */
  fn_v   WillEnterForeground;  /* ()V   */
  fn_v   DidEnterBackground;   /* ()V   */
  fn_v   WillTerminate;        /* ()V   */

  fn_v   SetGameHasBeenPurchased; /* ()V   */
  fn_zz  SetLightModeLocked;      /* (ZZ)V */
} g;

#define JNI_PREFIX "Java_com_hemispheregames_osmos_wrappers_OsmosJNILib_native"

/* so_find_addr_rx aborts on a missing symbol, which is what we want for the
 * entry points the loop cannot run without. */
#define BIND(field, name) \
  g.field = (void *)so_find_addr_rx(&osmos_mod, JNI_PREFIX name)
/* Optional entry points: absent in some builds, and the loop copes. */
#define BIND_OPT(field, name) \
  g.field = (void *)so_try_find_addr_rx(&osmos_mod, JNI_PREFIX name)

static void bind_entrypoints(void) {
  BIND(InitAppDelegate, "InitAppDelegate");
  BIND(InitRenderer,    "InitRenderer");
  BIND(Changed,         "Changed");
  BIND(ActivateGame,    "ActivateGame");
  BIND(Render,          "Render");
  BIND_OPT(RecreateFBO, "RecreateFBO");   /* checked at the call site */

  BIND(ProvideDataDir,  "ProvideDataDir");
  BIND(ProvideLang,     "ProvideLang");
  BIND_OPT(ProvideUUID, "ProvideUUID");
  BIND(ProvideDensity,  "ProvideDensity");

  BIND(TouchBegan,      "TouchBegan");
  BIND(TouchMoved,      "TouchMoved");
  BIND(TouchEnded,      "TouchEnded");
  BIND_OPT(TouchCancelled, "TouchCancelled");

  BIND_OPT(BackButtonPressed,   "BackButtonPressed");
  BIND_OPT(PauseGame,           "PauseGame");
  BIND_OPT(WillEnterForeground, "ApplicationWillEnterForeground");
  BIND_OPT(DidEnterBackground,  "ApplicationDidEnterBackground");
  BIND_OPT(WillTerminate,       "ApplicationWillTerminate");
  BIND_OPT(SetGameHasBeenPurchased, "SetGameHasBeenPurchased");
  BIND_OPT(SetLightModeLocked,      "SetLightModeLocked");
}

/* ------------------------------------------------------------------ */
/* EGL                                                                 */
/* ------------------------------------------------------------------ */

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static EGLContext egl_ctx = EGL_NO_CONTEXT;

static int egl_start(void) {
  egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_dpy == EGL_NO_DISPLAY) { LOGE("eglGetDisplay failed"); return 0; }
  if (!eglInitialize(egl_dpy, NULL, NULL)) { LOGE("eglInitialize failed"); return 0; }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { LOGE("eglBindAPI failed"); return 0; }

  /* The engine renders to an offscreen FBO and composites, and asks for a
   * depth buffer via GLESBlobRenderer. A 24-bit depth buffer with no stencil
   * matches what the Android build requested from GLSurfaceView. */
  static const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      24,
    EGL_NONE
  };
  EGLConfig cfg; EGLint n = 0;
  if (!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
    LOGE("eglChooseConfig found no config"); return 0;
  }

  /* Ask for the render size explicitly. The default window is the panel's
   * 1280x720 in handheld; without this, requesting 1080p would silently get
   * 720p and every derived value (UI scale, FBO texture) would be built for a
   * surface that does not exist. */
  if (R_FAILED(nwindowSetDimensions(nwindowGetDefault(),
                                    OSMOS_RENDER_W, OSMOS_RENDER_H)))
    LOGW("nwindowSetDimensions(%d, %d) failed; the surface may be 720p",
         OSMOS_RENDER_W, OSMOS_RENDER_H);

  egl_surf = eglCreateWindowSurface(egl_dpy, cfg, nwindowGetDefault(), NULL);
  if (egl_surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return 0; }

  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (egl_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return 0; }

  if (!eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx)) {
    LOGE("eglMakeCurrent failed"); return 0;
  }
  eglSwapInterval(egl_dpy, cfg_vsync() ? 1 : 0);

  /* Log what we actually got. If the context silently came up on a software
   * path, or as GLES1, every later symptom is downstream of this line. */
  { EGLint r = 0, g = 0, b = 0, a = 0, d = 0, s = 0;
    eglGetConfigAttrib(egl_dpy, cfg, EGL_RED_SIZE,     &r);
    eglGetConfigAttrib(egl_dpy, cfg, EGL_GREEN_SIZE,   &g);
    eglGetConfigAttrib(egl_dpy, cfg, EGL_BLUE_SIZE,    &b);
    eglGetConfigAttrib(egl_dpy, cfg, EGL_ALPHA_SIZE,   &a);
    eglGetConfigAttrib(egl_dpy, cfg, EGL_DEPTH_SIZE,   &d);
    eglGetConfigAttrib(egl_dpy, cfg, EGL_STENCIL_SIZE, &s);
    LOGB("EGL config  R%d G%d B%d A%d  depth %d  stencil %d", r, g, b, a, d, s); }
  LOGB("GL_VENDOR   %s", (const char *)glGetString(GL_VENDOR));
  LOGB("GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
  LOGB("GL_VERSION  %s", (const char *)glGetString(GL_VERSION));
  return 1;
}

static void egl_stop(void) {
  if (egl_dpy == EGL_NO_DISPLAY) return;
  eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (egl_ctx)  eglDestroyContext(egl_dpy, egl_ctx);
  if (egl_surf) eglDestroySurface(egl_dpy, egl_surf);
  eglTerminate(egl_dpy);
  egl_dpy = EGL_NO_DISPLAY; egl_surf = EGL_NO_SURFACE; egl_ctx = EGL_NO_CONTEXT;
}

/* ------------------------------------------------------------------ */
/* Display mode                                                        */
/* ------------------------------------------------------------------ */

/* Handheld is 1280x720; docked can be 1920x1080. The engine is told the size
 * through nativeChanged and rescales its UI from it, so a dock/undock has to
 * be noticed and forwarded rather than silently stretched.
 *
 * Note that every nativeChanged constructs a fresh GLMacRenderDevice and
 * overwrites the global pointer without freeing the old one, so each
 * dock/undock leaks one device object. That is the engine's own behaviour --
 * Android's onSurfaceChanged could fire repeatedly too -- and the object is
 * small, so it is left alone rather than papered over from outside. */
static int cur_w, cur_h;

/* 1080p in handheld as well as docked.
 *
 * The handheld panel is 720p, so the compositor downscales -- but this engine
 * draws 20-60 batches a frame, which is nothing for this GPU, and a fixed
 * render size means the UI scale, the camera and the render-to-texture FBO do
 * not have to be rebuilt on a dock or undock. The whole class of
 * surface-change bugs simply does not arise. */
static void surface_size(int *w, int *h) {
  *w = OSMOS_RENDER_W;
  *h = OSMOS_RENDER_H;
}

static void apply_surface_size(int w, int h) {
  nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
  cur_w = w; cur_h = h;
  nxp_set_screen(w, h);
  /* Density BEFORE Changed, and again on every surface change.
   *
   * nativeChanged recomputes gPixelDensityScale as gPixelDensity/163, and that
   * one value drives both CreateButtonYw's layout and
   * IBlobRenderer::WindowToWorldX/Y. The scale is derived from the width, so a
   * dock or undock has to re-provide the density or the new surface would be
   * laid out and hit-tested at the old size. This used to be called once at
   * boot. */
  LOGB("boot: ProvideDensity(%.1f) [ui_scale %.2f = %d/%d]",
       (double)cfg_dpi_for(w), (double)cfg_ui_scale_for(w),
       w, OSMOS_TABLET_LAYOUT ? 1024 : 320);
  g.ProvideDensity(jni_env(), jni_osmos_class(), cfg_dpi_for(w));

  LOGI("boot: criticalLength = %d px  (%s * %.2f * 1.15) -- a tap must stay "
       "inside this to count as a click",
       (int)((OSMOS_TABLET_LAYOUT ? 60.0f : 40.0f) * cfg_ui_scale_for(w) * 1.15f),
       OSMOS_TABLET_LAYOUT ? "60" : "40", (double)cfg_ui_scale_for(w));
  LOGB("boot: Changed(%d, %d) -- constructs the render device", w, h);
  diag_phase("Changed");
  g.Changed(jni_env(), jni_osmos_class(), w, h);

  /* RecreateFBO, and it is NOT optional.
   *
   * GLESBlobRenderer renders every frame into an off-screen texture and then
   * composites it back:
   *
   *     PrepareDraw()         glBindFramebuffer(fbo)      <- scene goes here
   *     ...draw the scene...
   *     ApplyLightModePass()  glBindFramebuffer(0)        <- back to the screen
   *                           draw a fullscreen quad sampling the FBO texture
   *
   * and RecreateTextureFBO() is what creates that pair -- glGenFramebuffers,
   * glGenTextures, glTexImage2D at the view size, glFramebufferTexture2D.
   * It has NO internal callers. On Android the GLSurfaceView.Renderer called
   * nativeRecreateFBO from onSurfaceChanged; this port only called it on a
   * dock or undock, so on a normal boot it never ran at all.
   *
   * With both handles left at 0, PrepareDraw binds framebuffer 0 -- the
   * default -- so the scene draws to the screen and looks fine, and then
   * ApplyLightModePass paints a fullscreen quad textured with texture 0 over
   * the top of it. Black screen, healthy draw counts, and the GL trace only
   * ever showing "fbo 0" because it never changes.
   *
   * Must follow Changed: RecreateTextureFBO sizes its texture from
   * IBlobRenderer::GetViewSize(), which needs the render device Changed
   * builds. */
  /* After Changed, which is what computes criticalLength. */
  osmos_tune_deadzone(&osmos_mod);

  if (g.RecreateFBO) {
    LOGB("boot: RecreateFBO -- creates the render-to-texture FBO");
    g.RecreateFBO(jni_env(), jni_osmos_class());
  } else {
    LOGE("nativeRecreateFBO is missing from this build of libosmos.so; "
         "the compositing pass will sample an uncreated texture");
  }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/* Ordering here is the single most likely thing to need adjusting on the first
 * hardware run, so the reasoning is written down rather than left implicit.
 *
 * From the disassembly of nativeInitAppDelegate, it runs, in order:
 *     signal(SIGPIPE, SIG_IGN)
 *     GHostParams()->SetInstallDir(<the CStr filled in by ProvideDataDir>)
 *     setupUserDirectories(<same CStr>)
 *     PreInitialize(); Initialize()
 *     Resources::PreSplashLoadTextures()          <-- uploads GL textures
 *     GMainMenu()->PreInitialize(...)
 *
 * Two hard constraints follow:
 *   1. ProvideDataDir MUST precede InitAppDelegate, or SetInstallDir gets an
 *      empty string and every asset open fails.
 *   2. A current GL context MUST exist before InitAppDelegate, because
 *      PreSplashLoadTextures uploads textures.
 *
 * InitRenderer is placed before InitAppDelegate because it is what constructs
 * the GLESBlobRenderer that Initialize() then expects to find.
 *
 * A third constraint, found the hard way and documented at the call site
 * below: nativeChanged must ALSO precede InitAppDelegate, because it is what
 * constructs the render device that PreSplashLoadTextures dereferences. An
 * earlier version of this file called it afterwards and died at address 0. */
static int boot_game(void) {
  int w, h;
  surface_size(&w, &h);

  /* nativeInitRenderer(int): the disassembly is `gIsiPad = (arg > 1)`, and
   * gIsiPad selects the tablet UI layout *and* which asset directories the
   * engine reads (asset_list.json puts Textures under iPadAssetDirectories).
   * A Switch is a tablet-class screen, so pass 2. */
  LOGB("boot: InitRenderer(%d) [%s layout]", OSMOS_INITRENDERER_ARG,
       OSMOS_TABLET_LAYOUT ? "tablet/iPad 4:3" : "phone 16:9");
  diag_phase("InitRenderer");
  /* Phone layout, hardcoded. The argument sets gIsiPad, which picks the UI
   * layout and the DPI divisor together; see OSMOS_TABLET_LAYOUT in config.h
   * for why 16:9 wants the phone one. */
  g.InitRenderer(jni_env(), jni_osmos_class(), OSMOS_INITRENDERER_ARG);

  /* Density is provided from apply_surface_size(), together with Changed --
   * see the note there. */

  g.ProvideDataDir(jni_env(), jni_osmos_class(), jni_new_string(osmos_data_dir()));
  g.ProvideLang(jni_env(), jni_osmos_class(), jni_new_string(cfg_language()));
  if (g.ProvideUUID)
    g.ProvideUUID(jni_env(), jni_osmos_class(), jni_new_string(osmos_device_uuid()));

  /* nativeChanged MUST come before nativeInitAppDelegate.
   *
   * Not obvious from the name, and it cost a crash to find. nativeChanged is
   * the ONLY writer of the render-device global at .data+0x2ea288 -- it does
   *
   *     bl   GLMacRenderDevice::GLMacRenderDevice()
   *     str  x20, [x9, #0x288]
   *
   * and nativeInitAppDelegate runs Resources::PreSplashLoadTextures(), which
   * reads that global back through GRenderDevice() and dereferences it. Call
   * InitAppDelegate first and the engine logs
   *
   *     IBlobRenderer::GetViewSize() - NULL render device
   *
   * and then takes a Data Abort at address 0.
   *
   * This is also the faithful GLSurfaceView order: onSurfaceCreated ->
   * InitRenderer, onSurfaceChanged -> Changed, and only then the delegate.
   * ProvideDensity has to precede it too, since Changed divides gPixelDensity
   * by 132 or 163 to derive the UI scale. */
  apply_surface_size(w, h);

  LOGI("boot: InitAppDelegate -- PreInitialize/Initialize/PreSplashLoadTextures");
  diag_phase("InitAppDelegate: Initialize -> WriteLicense -> LoadTextures -> LoadFonts");
  g.InitAppDelegate(jni_env(), jni_osmos_class());
  LOGB("boot: InitAppDelegate returned");

  /* Osmos sells "light mode" through Play billing, which does not exist here.
   * Mirror the Killer Bean port's purchases.txt approach: if the user says
   * they own it, tell the engine directly and skip the store path. */
  LOGI("boot: ActivateGame -- spawns loadWhileShowingSplash");
  diag_phase("ActivateGame");
  g.ActivateGame(jni_env(), jni_osmos_class());
  LOGB("boot: ActivateGame returned; entering frame loop");
  diag_phase("frame loop");

  /* BOTH calls are needed, and that is not obvious: they drive two independent
   * console variables.
   *
   *   nativeSetGameHasBeenPurchased -> GGameHasBeenPurchased
   *   nativeSetLightModeLocked      -> GInverseColorModeLocked
   *
   * Cross-referencing every instruction that touches the two globals shows
   * nothing in the engine derives one from the other -- that linkage lived in
   * the Java layer. Setting only the purchase flag leaves the light-mode
   * button locked, which is the opposite of what config.txt promises.
   *
   * Arguments are (locked, enabled): unlock it but leave it off, so the menu
   * toggle behaves normally. This runs after ActivateGame because the call
   * reaches into GetBlobRenderer() and CMainMenu::RedrawLightModeButton(). */
  if (cfg_light_mode_owned()) {
    LOGI("boot: light_mode=owned -- unlocking purchase and light mode");
    if (g.SetGameHasBeenPurchased)
      g.SetGameHasBeenPurchased(jni_env(), jni_osmos_class());
    if (g.SetLightModeLocked)
      g.SetLightModeLocked(jni_env(), jni_osmos_class(), 0, 0);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Frame loop                                                          */
/* ------------------------------------------------------------------ */

/* Round to whole pixels before handing a point to the engine.
 *
 * TouchMoved accumulates the pan into an INT:
 *
 *     accum = (int)(GetLastMovement().x + (float)accum)
 *
 * so the fractional part of every delta is truncated away and never carries.
 * The panel reports 1280 wide and we render at 1920, so nx_pointer scales by
 * 1.5 and every delta arrives as x.0 or x.5 -- half of them losing 0.5 px,
 * every frame, forever. A steady slow pan loses a flat third of itself, which
 * is exactly where the control felt dead.
 *
 * Rounding each POSITION (rather than the delta) is what fixes it without
 * introducing drift: a constant 1.5 px/frame becomes deltas of 2, 1, 2, 1,
 * which the engine accumulates exactly. The average rate is preserved to the
 * pixel and nothing is thrown away.
 *
 * Positions are rounded, not floored, so taps still land where the finger is. */
static inline float px(float v) { return (float)(int)(v + 0.5f); }

/* Replay the intermediate points Android would have delivered.
 *
 * This is the thing Android does that the port did not. A MotionEvent carries
 * the samples the panel produced BETWEEN frames -- getHistoricalX/Y -- and the
 * game consumes every one of them. nx_pointer, like any frame-driven input
 * layer, reports a single current position per frame.
 *
 * That matters here far more than it usually would, because the pan gesture is
 * multiplicative per call:
 *
 *     TouchMoved:            accum += dx;  enqueue smoothTimeDilation pow(b, accum)
 *     SmoothTimeDilation(v): dilation *= v;  then clamp
 *
 * so the total over a gesture is pow(b, SUM of accum) ~ pow(b, N*D/2) -- it
 * scales with N, the NUMBER of calls, not only with the distance travelled.
 * One sample per frame is the smallest N possible, and it is why raising the
 * exponent base did nothing: the response saturates against the engine's clamp
 * either way, and what governs the feel is how finely the gesture is sampled.
 *
 * Straight-line interpolation is the right reconstruction. A finger travelling
 * a few pixels per frame is not doing anything a spline would capture better,
 * and the endpoints are exact, so the total displacement is unchanged and the
 * tap/swipe classification sees the same stroke extent it did before. */
#define TOUCH_SUBSTEPS 4

static float last_x[16], last_y[16];
static int   last_valid[16];

static void emit_move(int id, float x, float y) {
  const int slot = (id >= 0 && id < 16) ? id : 0;

  if (last_valid[slot]) {
    const float x0 = last_x[slot], y0 = last_y[slot];
    for (int s = 1; s < TOUCH_SUBSTEPS; s++) {
      const float t = (float)s / (float)TOUCH_SUBSTEPS;
      g.TouchMoved(jni_env(), jni_osmos_class(),
                   px(x0 + (x - x0) * t), px(y0 + (y - y0) * t), id);
    }
  }
  g.TouchMoved(jni_env(), jni_osmos_class(), px(x), px(y), id);

  last_x[slot] = x; last_y[slot] = y; last_valid[slot] = 1;
}

static void dispatch_touches(void) {
  /* nx_pointer owns the pad, the touchscreen, the USB mouse, the gyro and the
   * on-screen cursor, and hands back plain pointer events. Its phases are
   * NXP_DOWN/MOVE/UP, which map one-to-one onto the engine's three touch
   * entry points. */
  NxpEvent ev[16];
  const int n = nxp_poll(ev, (int)(sizeof(ev) / sizeof(*ev)));

  for (int i = 0; i < n; i++) {
    switch (ev[i].phase) {
      case NXP_DOWN: {
        const int slot = (ev[i].id >= 0 && ev[i].id < 16) ? ev[i].id : 0;
        last_x[slot] = ev[i].x; last_y[slot] = ev[i].y; last_valid[slot] = 1;
        g.TouchBegan(jni_env(), jni_osmos_class(),
                     px(ev[i].x), px(ev[i].y), ev[i].id);
        break;
      }
      case NXP_MOVE:
        emit_move(ev[i].id, ev[i].x, ev[i].y);
        break;
      case NXP_UP: {
        const int slot = (ev[i].id >= 0 && ev[i].id < 16) ? ev[i].id : 0;
        last_valid[slot] = 0;
        g.TouchEnded(jni_env(), jni_osmos_class(),
                     px(ev[i].x), px(ev[i].y), ev[i].id);
        break;
      }
      default: break;
    }
  }
}

/* B is Android Back, and nx_pointer does not claim it.
 *
 * A separate PadState rather than sharing one: padGetButtonsDown is computed
 * against that PadState's own previous snapshot, so two independent readers
 * each see the edge. Sharing one and calling padUpdate twice a frame would
 * make whichever read second miss every press. */
static PadState back_pad;
static int back_pressed;

static void back_button_update(void) {
  padUpdate(&back_pad);
  if (padGetButtonsDown(&back_pad) & HidNpadButton_B) back_pressed = 1;
}

static int take_back_press(void) {
  const int v = back_pressed;
  back_pressed = 0;
  return v;
}

/* nativeBackButtonPressed returns false when the engine did not consume the
 * press, which on Android meant "let the Activity finish". That is our quit. */
static int handle_back(void) {
  if (!g.BackButtonPressed) return 0;
  return g.BackButtonPressed(jni_env(), jni_osmos_class()) ? 0 : 1;
}

static int focused = 1;

static void pump_applet(void) {
  int w, h;
  surface_size(&w, &h);
  if (w != cur_w || h != cur_h) {
    /* apply_surface_size re-provides the density, re-sends Changed and
     * recreates the FBO at the new size -- all three have to move together. */
    apply_surface_size(w, h);
  }

  const int now_focused = (appletGetFocusState() == AppletFocusState_InFocus);
  if (now_focused != focused) {
    focused = now_focused;
    if (focused) {
      osmos_al_resume();
      if (g.WillEnterForeground) g.WillEnterForeground(jni_env(), jni_osmos_class());
    } else {
      if (g.PauseGame)          g.PauseGame(jni_env(), jni_osmos_class());
      if (g.DidEnterBackground) g.DidEnterBackground(jni_env(), jni_osmos_class());
      osmos_al_suspend();
    }
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  /* Before anything else: the module's stack canaries come out of TPIDR_EL0,
   * and libnx keeps its own thread pointer in TPIDR_RO_EL0, so taking
   * TPIDR_EL0 for bionic is safe. Doing this late means every module call
   * before it reads a canary out of libnx's thread struct. */
  install_bionic_tls(main_tls);

  /* Before anything can reach mmap. g_mmap_big_align is used as a divisor and
   * as a loop step inside libc_shim's arena allocator, so it must be non-zero
   * before the first allocation, not merely before the first large one. */
  io_init();
  osmos_paths_init();           /* finds the folder the .nro was launched from */
  cfg_load();                   /* config.txt in the game directory */
  log_init();
  osmos_paths_log_search();     /* the search ran before logging existed */

  /* After log_init so the result is actually recorded -- the previous build
   * called this first and the line went nowhere. Still before anything can
   * reach mmap: nothing between here and so_load allocates through it. */
  osmos_mmap_arena_init();

  if (!osmos_paths_check()) {
    /* Missing libosmos.so or assets/: say which, and stop. */
    error_screen(osmos_paths_error());
    return 0;
  }

  so_load_area = memalign(0x1000, SO_LOAD_SIZE);
  if (!so_load_area) { error_screen("out of memory reserving the module area"); return 0; }

  if (so_load(&osmos_mod, osmos_so_path(), so_load_area, SO_LOAD_SIZE) < 0) {
    error_screen("so_load failed for libosmos.so"); return 0;
  }
  if (so_relocate(&osmos_mod) < 0) {
    error_screen("so_relocate failed"); return 0;
  }
  /* taint_missing_imports = 1: an unresolved import becomes a trap that names
   * itself when called, rather than a jump to zero. libosmos.so is BIND_NOW,
   * so in practice a gap fails here and not later. */
  if (so_resolve(&osmos_mod, dynlib_functions, (int)dynlib_numfunctions, 1) < 0) {
    error_screen("so_resolve failed -- run tools/verify_imports.py"); return 0;
  }
  /* ORDER MATTERS HERE, and getting it wrong is a Data Abort at boot.
   *
   * so_load only *reserves* the virtual range (virtmemFindCodeMemory +
   * virtmemAddReservation). A reservation is bookkeeping; nothing is mapped
   * at load_virtbase yet. so_finalize performs the svcMapProcessCodeMemory
   * that aliases load_base to load_virtbase and sets page permissions.
   *
   * so_flush_caches touches load_virtbase directly, so it MUST come after
   * so_finalize. An earlier version of this file had them the other way
   * round; so_util.c catches it with an explicit message rather than letting
   * it fault inside armDCacheFlush with nothing pointing back at the loader.
   *
   * so_patch_stack_canaries is deliberately NOT called by default. See
   * PORTING_OSMOS.md section 14 for why. If it is ever enabled it has to
   * run *before* so_finalize, because it writes to .text and the kernel
   * never permits a W->X transition on code memory once mapped. */
  /* Constant patches go here: load_base is writable until so_finalize maps it
   * as code, and the kernel does not permit W->X afterwards. */
  osmos_tune_apply(&osmos_mod);

  /* so_patch_stack_canaries is deliberately not called. Per-thread bionic TLS
   * makes the engine's guard consistent, so the checks pass on their own, and
   * neither reference port patches them -- one documents that NOPing 2000+
   * b.ne sites risks matching branches that are not canary checks at all. */

  so_finalize(&osmos_mod);
  so_flush_caches(&osmos_mod);

  /* Before the engine spawns threads: nxp_init reads cursor.png off the SD
   * card, and it is handed this port's locked fopen/fclose so its settings
   * writes cannot race the engine's file I/O. */
  { NxpConfig np = {0};
    np.screen_w = OSMOS_RENDER_W;  np.screen_h = OSMOS_RENDER_H;
    np.panel_w  = OSMOS_PANEL_W;   np.panel_h  = OSMOS_PANEL_H;
    np.data_dir = osmos_root();
    np.cursor_id = 8;              /* clear of the panel's finger ids */
    np.max_touch_slots = 8;
    np.fopen_fn = fopen_locked;
    np.fclose_fn = fclose_locked;
    nxp_init(&np); }

  jni_init();
  bind_entrypoints();

  if (!egl_start()) { error_screen("could not create a GLES2 context"); return 0; }

  /* From here on the console cannot be opened without giving the window back,
   * and so_util.c's fatal_error() may fire at any time during init_array. */
  error_set_gfx_release(egl_stop);
  if (!osmos_al_init()) LOGW("audio init failed; continuing muted");

  padInitializeDefault(&back_pad);


  /* Static initialisers. 27 entries in .init_array, all of them libc++ and
   * engine globals. They run with GL current and TLS installed, which is why
   * this is here and not immediately after so_relocate. */
  so_execute_init_array(&osmos_mod);

  /* Only now release the raw ELF image. mod->syms and mod->dynstrtab point
   * into load_base and survive this, so symbol lookup still works, but
   * elf_hdr, sec_hdr and shstrtab all point into the buffer being freed.
   * Both reference ports hold it until this point; 3 MB is not worth
   * reclaiming early for the chance of a use-after-free during init. */
  so_free_temp(&osmos_mod);

  /* Arm the watchdog before bring-up: everything interesting happens inside
   * boot_game(), where nothing in this file can observe progress. */
  diag_start();

  if (!boot_game()) { error_screen("game bring-up failed"); return 0; }

  /* Heartbeat.
   *
   * A black screen has two very different causes that look identical from
   * outside: the loop is not running at all (stuck inside the engine), or the
   * loop is running fine and drawing nothing. Bracketing nativeRender tells
   * them apart -- if "frame N enter" appears with no matching exit, the engine
   * is wedged inside MainTickAndDraw. Log density falls off so a long session
   * does not fill the SD card. */
  uint64_t frame = 0;
  const uint64_t t0 = armGetSystemTick();
  uint64_t last_beat = t0;

  int quit = 0;
  while (appletMainLoop() && !quit) {
    pump_applet();

    nxp_update();
    back_button_update();
    if (take_back_press()) quit = handle_back();
    dispatch_touches();

    frame++;
    diag_frame(frame);
    const int trace = (frame <= 5) || (frame == 10) || (frame == 60) ||
                      (frame == 300);
    if (trace) LOGI("frame %llu: enter nativeRender", (unsigned long long)frame);

    g.Render(jni_env(), jni_osmos_class());

    if (trace) {
      const GLenum e = glGetError();
      LOGI("frame %llu: nativeRender returned%s", (unsigned long long)frame,
           e ? " (GL error pending)" : "");
      if (e) LOGW("frame %llu: glGetError = 0x%04x", (unsigned long long)frame, e);
    }

    osmos_al_update();
    osmos_gl_frame();

    /* On top of the finished frame, before the swap. */
    nxp_draw();

    eglSwapBuffers(egl_dpy, egl_surf);

    /* Once every 5 s: proves the loop is alive and gives a real frame rate,
     * which separates "running but black" from "running at 0.2 fps". */
    const uint64_t now = armGetSystemTick();
    if (armTicksToNs(now - last_beat) > 5000000000ull) {
      LOGI("alive: frame %llu, %.1f fps average",
           (unsigned long long)frame,
           (double)frame / (armTicksToNs(now - t0) / 1e9));
      last_beat = now;
    }

    if (jni_quit_requested) quit = 1;
  }

  if (g.WillTerminate) g.WillTerminate(jni_env(), jni_osmos_class());

  nxp_save_settings();          /* flush any pending sensitivity change */
  diag_stop();
  osmos_al_shutdown();
  egl_stop();
  log_close();
  return 0;
}
