/* osmos_gl.c -- trace the GL state that can blank a screen.
 *
 * MIT licensed. See LICENSE.
 *
 * The menu renders for about half a second and then disappears while the
 * engine keeps running at ~53 fps, keeps loading assets, and logs no error.
 * At the GL level there are only a few mechanical ways that happens, and they
 * are distinguishable from outside:
 *
 *   glViewport shrinks or moves off-screen   -> everything clipped away
 *   glBindFramebuffer to an FBO never blitted -> drawing into nowhere
 *   glColorMask(0,0,0,0)                      -> draws land but write nothing
 *   glClearColor + glClear covering the frame -> painted over
 *   draw calls stop                           -> the engine stopped drawing
 *   none of the above                         -> drawn correctly, but the
 *                                                camera is looking elsewhere
 *
 * That last case is the interesting one, because CMainMenu::Build issues
 * `zoomToOdyssey` and IBlobRenderer::FixZoomTarget and
 * GLESBlobRenderer::CreateCamera both scale by gPixelDensityScale -- so a
 * camera animation running to a wrong target would empty the screen while
 * every counter here stays perfectly healthy.
 *
 * State changes are logged only when the value actually changes, so this is a
 * handful of lines per run rather than per frame. Draw and clear counts are
 * summarised once a second.
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "config.h"
#include "osmos_gl.h"

#if OSMOS_DIAG

static unsigned long long n_draw, n_clear, n_frames;
static unsigned long long last_report_tick;

/* ---- state we watch, with "impossible" initial values so the first call
 * always reports ---- */
static GLint  vp[4]        = { -1, -1, -1, -1 };
static GLuint bound_fb     = 0xFFFFFFFFu;
static GLboolean cmask[4]  = { 0xFF, 0xFF, 0xFF, 0xFF };
static GLfloat ccol[4]     = { -1.0f, -1.0f, -1.0f, -1.0f };

void glViewport_diag(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (x != vp[0] || y != vp[1] || w != vp[2] || h != vp[3]) {
    vp[0] = x; vp[1] = y; vp[2] = w; vp[3] = h;
    LOGI("gl: glViewport(%d, %d, %d, %d)%s", x, y, w, h,
         (w <= 0 || h <= 0) ? "   <-- DEGENERATE, nothing can draw" : "");
  }
  glViewport(x, y, w, h);
}

static unsigned long long n_bind_fbo, n_bind_default;

void glBindFramebuffer_diag(GLenum target, GLuint fb) {
  glBindFramebuffer(target, fb);
  if (fb) n_bind_fbo++; else n_bind_default++;
  if (fb != bound_fb) {
    bound_fb = fb;
    if (fb == 0) {
      LOGI("gl: glBindFramebuffer(default) -- drawing to the screen");
    } else {
      /* Check completeness at the point of binding: an incomplete FBO
       * silently discards every draw that follows it. */
      const GLenum st = glCheckFramebufferStatus(target);
      LOGI("gl: glBindFramebuffer(fbo %u) status=0x%04x%s", fb, st,
           st == GL_FRAMEBUFFER_COMPLETE
             ? "" : "   <-- INCOMPLETE, draws are discarded");
    }
  }
}

void glColorMask_diag(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  if (r != cmask[0] || g != cmask[1] || b != cmask[2] || a != cmask[3]) {
    cmask[0] = r; cmask[1] = g; cmask[2] = b; cmask[3] = a;
    LOGI("gl: glColorMask(%d, %d, %d, %d)%s", r, g, b, a,
         (!r && !g && !b) ? "   <-- colour writes disabled" : "");
  }
  glColorMask(r, g, b, a);
}

void glClearColor_diag(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  if (r != ccol[0] || g != ccol[1] || b != ccol[2] || a != ccol[3]) {
    ccol[0] = r; ccol[1] = g; ccol[2] = b; ccol[3] = a;
    LOGI("gl: glClearColor(%.3f, %.3f, %.3f, %.3f)",
         (double)r, (double)g, (double)b, (double)a);
  }
  glClearColor(r, g, b, a);
}

static GLbitfield last_clear_mask = 0xFFFFFFFFu;

void glClear_diag(GLbitfield mask) {
  n_clear++;
  if (mask != last_clear_mask) {
    last_clear_mask = mask;
    LOGI("gl: glClear(0x%04x)%s%s%s", mask,
         (mask & 0x4000) ? " COLOR" : "",
         (mask & 0x0100) ? " DEPTH" : "",
         (mask & 0x0400) ? " STENCIL" : "");
  }
  glClear(mask);
}

void glDrawArrays_diag(GLenum mode, GLint first, GLsizei count) {
  n_draw++;
  glDrawArrays(mode, first, count);
}

/* Called once per frame from main.c. Draw counts are the single most useful
 * number here: if they hold steady while the screen is black, the engine is
 * drawing and the geometry is simply not where the camera is looking. */
/* The engine's transform and colour, sampled once a second.
 *
 * Everything else has been eliminated: draws are healthy, the viewport is
 * 1280x720, no FBO is bound, colour writes are on. So the geometry is being
 * submitted and is not appearing. Only two things can still explain that, and
 * both are uploaded through uniforms:
 *
 *   the matrix  -- where the geometry lands in clip space. Anything that maps
 *                  the scene outside [-1,1] puts it off-screen, and the same
 *                  transform is what IBlobRenderer::WindowToWorldX/Y inverts
 *                  to hit-test a touch, which is why the menu would be both
 *                  invisible and unresponsive.
 *   the colour  -- if alpha is 0, the draws land and write nothing.
 *
 * Sampled rather than traced: the engine uploads a matrix per batch and there
 * are ~50 batches a frame.
 */
static int sample_uniforms;   /* how many matrices still to print this sample */

void glUniformMatrix4fv_diag(GLint loc, GLsizei n, GLboolean transpose,
                             const GLfloat *v) {
  if (sample_uniforms > 0 && n >= 1 && v) {
    sample_uniforms--;
    /* Six decimals, because three could not distinguish 0.00375 (a 533-unit
     * span, matching the 1.778 screen) from 0.0075 (266 units, an aspect of
     * 2.0). Both printed as "0.004"/"0.008". Also derive the span, which is
     * the number that actually means something. */
    const double sx = v[0], sy = v[5];
    LOGI("gl: matrix scale %.6f, %.6f -> spans %.1f x %.1f units, aspect %.3f",
         sx, sy, sx != 0.0 ? 2.0 / sx : 0.0, sy != 0.0 ? 2.0 / sy : 0.0,
         (sx != 0.0 && sy != 0.0) ? (2.0 / sx) / (2.0 / sy) : 0.0);
    LOGI("gl:   translate %.6f, %.6f, %.6f", (double)v[12], (double)v[13], (double)v[14]);
  }
  glUniformMatrix4fv(loc, n, transpose, v);
}

/* Colour is watched continuously but only reported when it changes, and the
 * alpha is what matters: a run of draws at alpha 0 is invisible geometry. */
/* The engine never calls glUniform4f -- the previous run logged zero of them,
 * because colour goes through CBatcher into a vertex attribute rather than a
 * uniform. glUniform4fv is watched instead, and blending below, which is the
 * remaining way a draw can land and contribute nothing. */
void glUniform4fv_diag(GLint loc, GLsizei n, const GLfloat *v) {
  static int reported;
  if (reported < 10 && n >= 1 && v) {
    reported++;
    LOGI("gl: uniform4fv(%.3f, %.3f, %.3f, alpha %.3f)%s",
         (double)v[0], (double)v[1], (double)v[2], (double)v[3],
         v[3] == 0.0f ? "   <-- fully transparent" : "");
  }
  glUniform4fv(loc, n, v);
}

/* A blend function of (ZERO, ONE) keeps the destination and discards every
 * fragment: geometry submitted, nothing changed, draw counters healthy. */
void glBlendFunc_diag(GLenum src, GLenum dst) {
  static GLenum ls = 0xFFFF, ld = 0xFFFF;
  if (src != ls || dst != ld) {
    ls = src; ld = dst;
    LOGI("gl: glBlendFunc(0x%04x, 0x%04x)%s", src, dst,
         (src == 0 /*GL_ZERO*/) ? "   <-- source discarded" : "");
  }
  glBlendFunc(src, dst);
}

/* GL_BLEND 0x0BE2, GL_DEPTH_TEST 0x0B71, GL_CULL_FACE 0x0B44,
 * GL_SCISSOR_TEST 0x0C11 -- any of which can hide everything. */
static const char *cap_name(GLenum c) {
  switch (c) {
    case 0x0BE2: return "BLEND";
    case 0x0B71: return "DEPTH_TEST";
    case 0x0B44: return "CULL_FACE";
    case 0x0C11: return "SCISSOR_TEST";
    case 0x0B90: return "STENCIL_TEST";
    default:     return NULL;
  }
}

void glEnable_diag(GLenum cap) {
  const char *n = cap_name(cap);
  static GLenum seen[8]; static int nseen;
  if (n) {
    int dup = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == (cap | 0x10000)) dup = 1;
    if (!dup && nseen < 8) { seen[nseen++] = cap | 0x10000; LOGI("gl: enable  %s", n); }
  }
  glEnable(cap);
}

void glDisable_diag(GLenum cap) {
  const char *n = cap_name(cap);
  static GLenum seen[8]; static int nseen;
  if (n) {
    int dup = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == cap) dup = 1;
    if (!dup && nseen < 8) { seen[nseen++] = cap; LOGI("gl: disable %s", n); }
  }
  glDisable(cap);
}

void glUniform4f_diag(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
  static GLfloat last[4] = { -1, -1, -1, -1 };
  static int reported;
  if (reported < 12 && (x != last[0] || y != last[1] || z != last[2] || w != last[3])) {
    last[0] = x; last[1] = y; last[2] = z; last[3] = w;
    reported++;
    LOGI("gl: uniform4f(%.3f, %.3f, %.3f, alpha %.3f)%s",
         (double)x, (double)y, (double)z, (double)w,
         (w == 0.0f) ? "   <-- fully transparent" : "");
  }
  glUniform4f(loc, x, y, z, w);
}

/* osmos_gl_opaque() is GONE, deliberately.
 *
 * It forced glColorMask(1,1,1,0) every frame on the theory that the engine's
 * blending was draining the surface's alpha and the compositor was fading the
 * frame to black. The only evidence was a glReadPixels probe that turned out
 * to report 000000 a=00 off a splash screen plainly visible on the panel --
 * it does not work against the window surface on this driver.
 *
 * The real cause was the uncreated render FBO. Leaving a disproven change
 * behind a debug flag is worse than deleting it: the next person to build with
 * OSMOS_DIAG=1 would silently get it back while debugging something else.
 */

/* Sample the finished frame before the swap.
 *
 * If these come back as the clear colour, nothing visible was drawn and the
 * problem is upstream in the engine. If they come back with real colour, the
 * frame HAS content and it is being lost between here and the panel -- which
 * is what an alpha or compositor problem looks like. One sample a second; it
 * is a pipeline stall, so not more often. */
/* The glReadPixels frame sampler that used to live here is deleted, not
 * disabled. It reported 000000 a=00 off a splash screen plainly visible on the
 * panel -- reading back the window surface does not work on this driver -- and
 * a probe that returns black from a screen you can see does not become useful
 * by being switched off. It cost one wrong diagnosis already.
 */

void osmos_gl_frame(void) {
  { static u64 last_u;
    const u64 t = armGetSystemTick();
    if (!last_u || armTicksToNs(t - last_u) > 2000000000ull) {
      last_u = t; sample_uniforms = 3; } }
  n_frames++;
  const u64 now = armGetSystemTick();
  if (!last_report_tick) { last_report_tick = now; return; }
  if (armTicksToNs(now - last_report_tick) < 2000000000ull) return;

  /* Binds per frame is the number that shows the compositing pass is real:
   * one bind to the render FBO and one back to the default, every frame. When
   * the FBO had never been created both were 0, so the count of non-zero binds
   * stayed at zero and the "fbo" column never changed -- which is exactly how
   * the bug hid. */
  LOGI("gl: %llu frames, %llu draws (%llu/frame), %llu clears, "
       "viewport %dx%d, binds fbo=%llu default=%llu",
       n_frames, n_draw, n_frames ? n_draw / n_frames : 0, n_clear,
       vp[2], vp[3], n_bind_fbo, n_bind_default);

  n_frames = n_draw = n_clear = n_bind_fbo = n_bind_default = 0;
  last_report_tick = now;
}

#endif  /* OSMOS_DIAG */
