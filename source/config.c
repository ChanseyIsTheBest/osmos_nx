/* config.c -- the two settings that survive, plus logging.
 *
 * MIT licensed. See LICENSE.
 *
 * Everything else moved into config.h as a constant. A setting is a place to
 * put a wrong value, and in this port most of them were exactly that: the UI
 * scale, the render size and the device layout were all shipped wrong at least
 * once while being "configurable". They are derived or decided now.
 *
 * The two that remain are the two only the player can answer.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "config.h"
#include "osmos_paths.h"
#include "osmos_io.h"
#include "error.h"

static char c_language[16] = "auto";
static int  c_light_mode   = 0;
static float c_pan_sens    = OSMOS_PAN_SENSITIVITY_DEFAULT;
static int   c_pan_dead    = OSMOS_PAN_DEADZONE_DEFAULT;

/* One block per setting, so a config.txt written by an older build can have
 * the settings it is missing appended without disturbing the choices already
 * in it.
 *
 * The file is only created when absent, so a new setting would otherwise be
 * invisible forever to anyone who has run the port before -- they would keep
 * the old defaults and never learn the option existed. That already happened
 * once with the retired `dpi` key. */
typedef struct { const char *key, *block; } CfgBlock;

static const CfgBlock CFG_BLOCKS[] = {
  { "language",
    "# language: which localisation to load. \"auto\" follows the console.\n"
    "#   en de es fr it ja ko pt ru zh-Hans iu\n"
    "language = auto\n\n" },

  { "light_mode",
    "# light_mode: Osmos sells its \"light mode\" through Google Play, which does\n"
    "# not exist on this console, so the game can neither sell it nor look up\n"
    "# what you own. If you already bought it on your Play account, change this\n"
    "# to \"owned\" and the loader unlocks it at startup.\n"
    "light_mode = locked\n\n" },

  { "pan_sensitivity",
    "# pan_sensitivity: how strongly a one-finger left/right pan changes time.\n"
    "#\n"
    "# The engine's response is pow(base, pixels_panned). This raises every one\n"
    "# of its bases to this power, so the number behaves as \"as if the finger\n"
    "# moved N times further\" -- the curve keeps its shape, only the rate\n"
    "# changes. 1.0 is the game exactly as shipped.\n"
    "#\n"
    "#   1.0 -> a 100 px slow pan is 1.22x     3.0 -> 1.82x\n"
    "#   2.0 -> 1.49x                          6.0 -> 3.32x\n"
    "pan_sensitivity = 6.0\n\n" },

  { "pan_deadzone",
    "# pan_deadzone: how far your finger must move before a pan is recognised,\n"
    "# in touch-panel pixels (the panel is 1280 wide whatever the render size).\n"
    "#\n"
    "# The engine will not start panning until the stroke exceeds this, and its\n"
    "# own value is a fixed 14.4% of the screen width -- the same on every phone\n"
    "# it shipped on, but this panel is physically about twice as wide, so the\n"
    "# same fraction means twice the finger travel: 19.7 mm here against 8.4 mm\n"
    "# on an iPhone. 64 px is about 6.9 mm.\n"
    "#\n"
    "# The same threshold decides tap-versus-swipe, so going very low will start\n"
    "# treating sloppy taps as swipes. Ordinary taps drift 1-11 px.\n"
    "pan_deadzone = 64\n\n" },
};

#define CFG_NBLOCKS ((int)(sizeof(CFG_BLOCKS) / sizeof(*CFG_BLOCKS)))

static const char *CFG_HEADER =
  "# osmos_nx configuration.\n"
  "#\n"
  "# Almost everything is hardcoded. These are the settings only you can\n"
  "# answer, so they live here.\n"
  "\n";

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

void cfg_load(void) {
  char path[FS_MAX_PATH];
  snprintf(path, sizeof(path), "%s/config.txt", osmos_root());

  int seen[CFG_NBLOCKS];
  memset(seen, 0, sizeof(seen));

  FILE *f = fopen_locked(path, "r");
  if (!f) {
    f = fopen_locked(path, "w");
    if (f) {
      fputs(CFG_HEADER, f);
      for (int i = 0; i < CFG_NBLOCKS; i++) fputs(CFG_BLOCKS[i].block, f);
      fclose_locked(f);
    }
    f = fopen_locked(path, "r");
    if (!f) return;
  }

  int seen_dead_key = 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;

    for (int i = 0; i < CFG_NBLOCKS; i++)
      if (!strcmp(key, CFG_BLOCKS[i].key)) seen[i] = 1;

    if      (!strcmp(key, "language"))        snprintf(c_language, sizeof(c_language), "%s", val);
    else if (!strcmp(key, "light_mode"))      c_light_mode = !strcmp(val, "owned");
    else if (!strcmp(key, "pan_sensitivity")) c_pan_sens = strtof(val, NULL);
    else if (!strcmp(key, "pan_deadzone"))    c_pan_dead = (int)strtol(val, NULL, 10);
    else                                      seen_dead_key = 1;
  }
  fclose_locked(f);

  /* Append anything this build added. The existing choices are untouched --
   * only the missing settings are written, with their documentation, so an old
   * config.txt gains new options instead of silently lacking them. */
  int missing = 0;
  for (int i = 0; i < CFG_NBLOCKS; i++) if (!seen[i]) missing++;
  if (missing) {
    FILE *a = fopen_locked(path, "a");
    if (a) {
      for (int i = 0; i < CFG_NBLOCKS; i++)
        if (!seen[i]) fputs(CFG_BLOCKS[i].block, a);
      fclose_locked(a);
    }
    LOGW("config.txt was missing %d setting(s); appended with defaults.", missing);
  }

  if (c_pan_sens < 0.25f) c_pan_sens = 0.25f;
  if (c_pan_sens > 8.0f)  c_pan_sens = 8.0f;

  /* Below ~16 px a tap's own drift would register as a pan. */
  if (c_pan_dead < 16)  c_pan_dead = 16;
  if (c_pan_dead > 320) c_pan_dead = 320;

  if (seen_dead_key)
    LOGW("config.txt has settings that are no longer configurable; ignored.");
}

const char *cfg_language(void) {
  if (strcmp(c_language, "auto") != 0) return c_language;

  /* libnx's default init does NOT bring up the 'set' service, so it has to be
   * opened and closed around the query. Both reference ports do the same. */
  u64 lc = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lc)))
      setMakeLanguage(lc, &sl);
    setExit();
  }

  /* The engine names its files Osmos-<code>.loc, and those codes are not all
   * the IETF tags libnx reports -- zh-Hans and pt are the ones that differ. */
  switch (sl) {
    case SetLanguage_JA:     return "ja";
    case SetLanguage_FR:
    case SetLanguage_FRCA:   return "fr";
    case SetLanguage_DE:     return "de";
    case SetLanguage_IT:     return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419:  return "es";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: return "zh-Hans";
    case SetLanguage_KO:     return "ko";
    case SetLanguage_RU:     return "ru";
    case SetLanguage_PT:
    case SetLanguage_PTBR:   return "pt";
    default:                 return "en";
  }
}

int   cfg_light_mode_owned(void)  { return c_light_mode; }
float cfg_pan_sensitivity(void)   { return c_pan_sens; }
int   cfg_pan_deadzone(void)      { return c_pan_dead; }

/* gPixelDensityScale = width / 320 on the phone layout. Derived rather than
 * chosen: the engine's own criticalLength came out to a constant fraction of
 * screen width on every device this game shipped on. At 1920 that is 6.0. */
float cfg_ui_scale_for(int width) { return (float)width / OSMOS_UI_SCALE_BASE; }
float cfg_dpi_for(int width)      { return cfg_ui_scale_for(width) * OSMOS_DPI_BASE; }

float cfg_master_volume(void)     { return OSMOS_MASTER_VOLUME; }
int   cfg_vsync(void)             { return OSMOS_VSYNC; }

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static FILE *logf;
static int   log_ready;

/* Opened on demand.
 *
 * With DEBUG_LOG off a normal session writes nothing at all -- no file is even
 * created. The first warning or error opens it, so a session that goes wrong
 * still leaves something to read, which is the only time the file has ever
 * been worth having. */
static void log_open_if_needed(void) {
  if (log_ready) return;
  log_ready = 1;
  char p[FS_MAX_PATH];
  snprintf(p, sizeof(p), "%s/debug.log", osmos_root());
  logf = fopen_locked(p, "w");
}

void log_init(void) {
#if DEBUG_LOG
  log_open_if_needed();
#endif
}

/* Flushed per line. A log that does not survive the crash it was describing
 * tells you the last thing that worked, not the first thing that did not. */
void log_write(char level, const char *fmt, ...) {
  log_open_if_needed();
  if (!logf) return;
  fprintf(logf, "[%c] ", level);
  va_list va;
  va_start(va, fmt);
  vfprintf(logf, fmt, va);
  va_end(va);
  fputc('\n', logf);
  fflush(logf);
}

void log_close(void) { if (logf) { fclose_locked(logf); logf = NULL; } }

/* Engine-originated user messages (toasts, URLs it wanted to open). Logged
 * here; a future pass can draw them over the frame. */
void overlay_note(const char *text) {
  LOGI("[note] %s", text ? text : "");
}

/* Fatal errors and the startup status screen live in error.c, which so_util.c
 * also calls into. Keeping them there avoids two definitions of error_screen.
 */
