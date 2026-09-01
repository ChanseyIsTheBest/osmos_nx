/* config.h -- config.txt next to the .nro. MIT licensed. */
#ifndef OSMOS_CONFIG_H
#define OSMOS_CONFIG_H

void cfg_load(void);

const char *cfg_language(void);      /* "en", "de", ... or system default */
/* Derived from the render width; see OSMOS_UI_SCALE_BASE. */
float cfg_ui_scale_for(int width);
float cfg_dpi_for(int width);

/* Which of the game's two UI layouts to use. HARDCODED TO PHONE.
 *
 * nativeInitRenderer(arg) sets `gIsiPad = (arg > 1)`, and gIsiPad picks the
 * layout and the divisor that turns ui_scale into a density.
 *
 * Phone, not tablet, and the reason is aspect rather than resolution. The
 * engine lays its interface out in a space 480 units wide
 * (gNormalizedWidthFactor = 480/width), so the height in units is 480/aspect:
 * 270 on 16:9, 360 on 4:3. The tablet layout was built against an iPad, which
 * is 4:3; this console is 16:9 docked and handheld alike, so anything the
 * tablet layout places below y=270 is simply off the bottom of the screen.
 *
 * These three are kept together deliberately. The InitRenderer argument and
 * the DPI divisor have to agree -- gPixelDensityScale feeds both
 * CreateButtonYw's layout and IBlobRenderer::WindowToWorldX/Y, so a mismatch
 * makes the interface invisible and unhittable at the same time. Flipping the
 * one constant flips both correctly. */
#define OSMOS_TABLET_LAYOUT      0
#define OSMOS_INITRENDERER_ARG   (OSMOS_TABLET_LAYOUT ? 2 : 1)
#define OSMOS_DPI_BASE           (OSMOS_TABLET_LAYOUT ? 132.0f : 163.0f)
float cfg_master_volume(void);   /* constant; kept as a call for osmos_al.c */
float cfg_pan_sensitivity(void); /* config.txt override, or the default above */
int   cfg_pan_deadzone(void);    /* panel px before a pan is recognised       */
int   cfg_vsync(void);
int   cfg_light_mode_owned(void);

/* ------------------------------------------------------------------ */
/* Hardcoded settings                                                  */
/* ------------------------------------------------------------------ */

/* Everything below used to live in config.txt. Only `language` and
 * `light_mode` remain configurable; the rest are decisions, not preferences,
 * and every one of them was a way to ship a wrong value.
 *
 * RENDER SIZE: 1080p in handheld as well as docked. The panel is 720p, so the
 * compositor downscales -- but this engine draws 20-60 batches a frame, which
 * is nothing for this GPU, and rendering at the docked size means the UI
 * layout and the render-to-texture FBO do not change on a dock or undock. */
#define OSMOS_RENDER_W 1920
#define OSMOS_RENDER_H 1080

/* The touch panel always reports in 1280x720 regardless of render size. */
#define OSMOS_PANEL_W  1280
#define OSMOS_PANEL_H  720

/* Which of the game's two UI layouts. Phone, because the engine lays out in a
 * 480-unit-wide space (height 480/aspect: 270 at 16:9, 360 at 4:3) and the
 * tablet layout was built for a 4:3 iPad. See §31/§37 of PORTING_OSMOS.md. */
#define OSMOS_TABLET_LAYOUT      0
#define OSMOS_INITRENDERER_ARG   (OSMOS_TABLET_LAYOUT ? 2 : 1)
#define OSMOS_DPI_BASE           (OSMOS_TABLET_LAYOUT ? 132.0f : 163.0f)

/* gPixelDensityScale = width/320 on the phone layout. Derived, not chosen:
 * the engine's own criticalLength came out to a constant fraction of screen
 * width on every device this game shipped on. 1920/320 = 6.0. */
#define OSMOS_UI_SCALE_BASE      (OSMOS_TABLET_LAYOUT ? 1024.0f : 320.0f)

/* One-finger horizontal pan is the fine time control.
 *
 * The engine's response is pow(base, pixels), and there are FIVE bases, not
 * one: two doubles in .rodata for one gesture mode, and three float immediates
 * in the instruction stream for the other. The slow-pan branch -- chosen when
 * the stroke lasts longer than 0.25 s, which is what a deliberate pan is --
 * uses a float immediate. Patching only the .rodata pair did nothing on
 * hardware, which is exactly what that looks like.
 *
 * This raises every base to the power N, so N is "as if the finger moved N
 * times further": pow(b^N, px) == pow(b, N*px). The curve keeps its shape.
 *
 *     1.0 -> a 100 px slow pan is 1.22x      3.0 -> 1.82x
 *     2.0 -> 1.49x                           4.0 -> 2.22x
 *
 * This is the DEFAULT; config.txt can override it, because unlike the
 * other constants here it is a preference rather than a decision -- how
 * strong a gesture should feel is not something the port can be right
 * about on someone else's behalf.
 */
#define OSMOS_PAN_SENSITIVITY_DEFAULT  6.0f

/* How far the finger must travel before a pan is recognised at all, in TOUCH
 * PANEL pixels (the panel is 1280 wide whatever the render size).
 *
 * TouchMoved will not enter the pan state until |dx| exceeds criticalLength:
 *
 *     GetTotalMovement(); cmp |dx|, criticalLength; b.le  -> no pan
 *     |dy|/|dx| < 0.4                               -> state = 4, panning
 *
 * and criticalLength is (gIsiPad ? 60 : 40) * gPixelDensityScale * 1.15, which
 * is a constant 14.4% of the screen width on every device the game shipped on.
 * A constant fraction is the wrong invariant for a threshold measured by a
 * finger: this panel is physically about twice as wide as a phone's, so the
 * same fraction is twice the travel -- 19.7 mm here against 8.4 mm on an
 * iPhone. That is the whole reason the pan feels slow to start.
 *
 * 64 panel px is about 6.8 mm, which is snappier than a phone while staying
 * clear of the drift in an ordinary tap (measured extents were 1-11 px, worst
 * case 47). Note this same constant decides tap-versus-swipe in TouchEnded, so
 * setting it very low will start classifying sloppy taps as swipes. */
#define OSMOS_PAN_DEADZONE_DEFAULT     64

#define OSMOS_VSYNC              1
#define OSMOS_MASTER_VOLUME      1.0f

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* OFF. The tracing that found the FBO bug is expensive enough to stutter the
 * frame: a GL wrapper on every draw, a log line per JNI call and per touch,
 * the engine's own log mirrored write-for-write, and a watchdog that pauses
 * threads to read their registers.
 *
 * Set to 1 to bring all of it back. With it off the GL entry points resolve
 * straight to the real functions rather than through wrappers (see
 * osmos_gl.h), so there is no per-draw cost at all -- not merely a cheaper
 * one. Errors and warnings are still logged either way; they are rare. */
#ifndef OSMOS_DIAG
#define OSMOS_DIAG 0
#endif

/* Logging OFF. No debug.log is created and the boot lines compile away.
 *
 * Warnings and errors are the exception: they lazily create the file the first
 * time one fires. So a normal session writes nothing at all, and a session
 * that goes wrong still leaves something to read -- which is the only reason
 * the file has ever been useful. Set to 1 for the full boot log. */
#ifndef DEBUG_LOG
#define DEBUG_LOG 0
#endif

void log_init(void);
void log_close(void);
void log_write(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void overlay_note(const char *text);   /* engine-originated user message */
/* error_screen() and fatal_error() are declared in error.h. */

/* LOGW/LOGE always compile in: they are rare, and a silent failure is the
 * worst possible outcome. LOGB is the boot summary and follows DEBUG_LOG.
 * LOGI is the chatty per-frame tracing and follows OSMOS_DIAG. */
#define LOGW(...) log_write('W', __VA_ARGS__)
#define LOGE(...) log_write('E', __VA_ARGS__)

#if DEBUG_LOG
#define LOGB(...) log_write('I', __VA_ARGS__)
#else
#define LOGB(...) ((void)0)
#endif

#if DEBUG_LOG && OSMOS_DIAG
#define LOGI(...) log_write('I', __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

#endif
