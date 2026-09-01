/* osmos_tune.c -- constants patched in the loaded module.
 *
 * MIT licensed. See LICENSE.
 *
 * The one-finger horizontal pan is Osmos's fine time control, and its response
 * is set by two doubles in libosmos.so's .rodata rather than by anything
 * reachable from outside. TouchMoved accumulates the stroke's movement in
 * whole pixels and then does:
 *
 *     accum += (int)CStroke::GetLastMovement()
 *     if (accum >= 1)  value = pow(1.0030000210,  accum)
 *     if (accum <  0)  value = pow(0.9970089793, -accum)
 *     CStr::Printf(...); GetCommandController()->EnqueueText(...)
 *
 * -- the gesture is literally typed into the game's own console. The two bases
 * are reciprocals of each other, so the curve is symmetric.
 *
 * At the shipped 1.003, a hundred pixels of pan is a 1.35x change, which is
 * fine on a phone held close and too slow on a handheld held at arm's length.
 *
 * The clean way to change it is to raise the base to a power, because
 *
 *     pow(b0^N, px)  ==  pow(b0, N*px)
 *
 * so N is exactly "as if the finger had moved N times further". The curve
 * keeps its shape and its symmetry; only the rate changes.
 *
 * Located by VALUE, not by offset. Both constants occur exactly once in the
 * library, and searching for them survives a different build of the game --
 * whereas a hardcoded 0xcb290 would silently patch whatever happened to live
 * there instead.
 */

#include <switch.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "osmos_tune.h"
#include "so_util.h"

/* The five bases, and where each lives.
 *
 * Two are doubles in .rodata. The other three are FLOAT IMMEDIATES built by a
 * MOVZ/MOVK pair in the instruction stream, which is why an earlier version
 * that patched only the .rodata pair had no effect on hardware: the slow-pan
 * branch -- selected when the stroke has lasted more than 0.25 s, which is
 * exactly what a deliberate pan is -- uses 1.002 as an immediate.
 *
 *     0x197014  pow   1.003000021   double, .rodata
 *     0x197050  pow   0.997008979   double, .rodata   (reciprocal of the above)
 *     0x197138  powf  1.008000016   immediate         quick flick
 *     0x19718c  powf  1.008000016   immediate         quick flick
 *     0x197294  powf  1.001999974   immediate         slow pan  <-- the one that matters
 */
static const double PAN_BASE_POS = 1.0030000209808350;
static const double PAN_BASE_NEG = 0.9970089793205261;

static const float  PAN_FLOAT_BASES[] = { 1.008000016f, 1.001999974f };

/* Replace every occurrence of `from` with `to` in the module's writable image.
 * Returns how many were changed. */
static int patch_double(so_module *mod, double from, double to) {
  if (!mod->load_base || !mod->load_size) return 0;

  uint8_t *base = (uint8_t *)mod->load_base;
  const size_t n = mod->load_size;
  int hits = 0;

  /* 8-byte aligned only: a double constant is never placed unaligned, and
   * scanning every byte offset would risk matching the middle of unrelated
   * data that happens to share the pattern. */
  for (size_t i = 0; i + sizeof(double) <= n; i += 8) {
    if (memcmp(base + i, &from, sizeof(double)) != 0) continue;
    memcpy(base + i, &to, sizeof(double));
    hits++;
  }
  return hits;
}

/* Rewrite a 32-bit float built by `movz wN, #lo` + `movk wN, #hi, lsl #16`.
 *
 * Located by the value the pair produces rather than by address, for the same
 * reason as the doubles: an offset would silently patch whatever a different
 * build of the game happened to put there.
 *
 * AArch64 encodings, 32-bit forms:
 *     MOVZ  0x52800000 | (hw << 21) | (imm16 << 5) | Rd
 *     MOVK  0x72800000 | (hw << 21) | (imm16 << 5) | Rd
 * Only the imm16 field is touched; the register and everything else stay. */
static int patch_float_imm(so_module *mod, float from, float to) {
  if (!mod->load_base || !mod->load_size) return 0;

  uint32_t from_bits, to_bits;
  memcpy(&from_bits, &from, 4);
  memcpy(&to_bits, &to, 4);

  uint32_t *code = (uint32_t *)mod->load_base;
  const size_t words = mod->load_size / 4;
  int hits = 0;

  for (size_t i = 0; i + 8 < words; i++) {
    const uint32_t a = code[i];
    if ((a & 0xFFE00000u) != 0x52800000u) continue;            /* MOVZ, hw=0 */
    const uint32_t rd = a & 0x1Fu;
    const uint32_t lo = (a >> 5) & 0xFFFFu;
    if (lo != (from_bits & 0xFFFFu)) continue;

    /* The matching MOVK follows within a few instructions. */
    for (size_t j = i + 1; j < i + 8; j++) {
      const uint32_t b = code[j];
      if ((b & 0xFFE00000u) != 0x72A00000u) continue;          /* MOVK, hw=1 */
      if ((b & 0x1Fu) != rd) continue;
      if (((b >> 5) & 0xFFFFu) != (from_bits >> 16)) continue;

      code[i] = (a & ~(0xFFFFu << 5)) | ((to_bits & 0xFFFFu) << 5);
      code[j] = (b & ~(0xFFFFu << 5)) | ((to_bits >> 16) << 5);
      hits++;
      break;
    }
  }
  return hits;
}

void osmos_tune_apply(so_module *mod) {
  const float sens = cfg_pan_sensitivity();
  if (sens == 1.0f) return;            /* the game exactly as shipped */

  const double n = (double)sens;
  const double pos = pow(PAN_BASE_POS, n);
  const double neg = pow(PAN_BASE_NEG, n);

  const int a = patch_double(mod, PAN_BASE_POS, pos);
  const int b = patch_double(mod, PAN_BASE_NEG, neg);

  /* And the three float immediates, which are what a slow deliberate pan
   * actually goes through. */
  int f = 0;
  for (size_t i = 0; i < sizeof(PAN_FLOAT_BASES) / sizeof(*PAN_FLOAT_BASES); i++) {
    const float from = PAN_FLOAT_BASES[i];
    const float to = (float)pow((double)from, n);
    const int k = patch_float_imm(mod, from, to);
    LOGB("tune: pan base %.6f -> %.6f (%d site%s)",
         (double)from, (double)to, k, k == 1 ? "" : "s");
    f += k;
  }

  if (a == 1 && b == 1 && f == 3) {
    LOGB("tune: pan sensitivity x%.2f applied to all 5 bases; a 100 px slow "
         "pan is now %.2fx instead of %.2fx",
         (double)sens,
         pow(pow(1.001999974, n), 100.0), pow(1.001999974, 100.0));
    return;
  }

  /* Say so rather than leaving it ambiguous. Zero means this build of
   * libosmos.so uses different constants and the game will simply feel as it
   * always did; more than one means the pattern is not unique here and
   * something unrelated may have been overwritten, which matters more. */
  LOGW("tune: pan sensitivity NOT fully applied (%d positive, %d negative, "
       "%d float sites; expected 1, 1 and 3). Any site that was missed keeps "
       "the shipped response.", a, b, f);
}

/* ------------------------------------------------------------------ */
/* pan dead zone                                                       */
/* ------------------------------------------------------------------ */

/* criticalLength decides when TouchMoved enters the pan state:
 *
 *     GetTotalMovement(); cmp |dx|, criticalLength; b.le -> no pan
 *
 * nativeChanged sets it to (gIsiPad ? 60 : 40) * gPixelDensityScale * 1.15 --
 * a constant 14.4% of the screen width. That is the wrong invariant for a
 * threshold a finger has to cross: the fraction is identical on every device,
 * but this panel is physically about twice as wide as a phone's, so the travel
 * required is twice as far.
 *
 * Written as a plain global rather than patched into code, and re-applied
 * after every Changed because that is what recomputes it. */
void osmos_tune_deadzone(so_module *mod) {
  const uintptr_t addr = so_try_find_addr_rx(mod, "criticalLength");
  if (!addr) {
    LOGW("tune: criticalLength not found; the pan dead zone stays at the "
         "engine's own 14.4%% of screen width");
    return;
  }

  int *crit = (int *)addr;
  const int before = *crit;
  (void)before;                    /* only read by LOGB, which may compile out */

  /* The setting is in panel pixels because that is what the finger actually
   * moves across; criticalLength is compared against render-space movement. */
  const int want = (int)((float)cfg_pan_deadzone()
                         * (float)OSMOS_RENDER_W / (float)OSMOS_PANEL_W);
  if (want <= 0) return;

  *crit = want;
  LOGB("tune: pan dead zone %d -> %d render px (%d panel px); tap/swipe "
       "threshold moves with it",
       before, want, cfg_pan_deadzone());
}
