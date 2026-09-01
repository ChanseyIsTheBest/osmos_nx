/* osmos_paths.c, osmos_save.c and config.c live together because they are all
 * small and all concern "where things are on the SD card".
 *
 * MIT licensed. See LICENSE.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "config.h"
#include "osmos_paths.h"
#include "osmos_save.h"

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

static char root[FS_MAX_PATH];
static char so_path[FS_MAX_PATH];
static char data_dir[FS_MAX_PATH];
static char uuid[40];
/* Big enough for the prose plus a full FS_MAX_PATH path; 256 truncated the
 * path in exactly the message whose whole job is to show it. */
static char path_err[FS_MAX_PATH + 512];

/* snprintf truncates silently, and a shortened path shows up much later as an
 * unexplained "file not found". Fail loudly at composition time instead. */
static void resolve_data_dir(void);

static void join(char *out, size_t cap, const char *dir, const char *leaf) {
  const int n = snprintf(out, cap, "%s%s", dir, leaf);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = 0;
    LOGE("path too long: %s%s", dir, leaf);
  }
}

/* A folder qualifies as the game directory if it holds both halves of what
 * prepare_game.sh produces. Checking only libosmos.so would happily select a
 * folder with the library and no assets, and the failure would then surface
 * much later as a texture load fault rather than as "assets are missing". */
static int looks_like_game_dir(const char *dir) {
  char probe[FS_MAX_PATH];
  struct stat st;

  join(probe, sizeof(probe), dir, "/libosmos.so");
  if (!probe[0] || stat(probe, &st) != 0) return 0;

  join(probe, sizeof(probe), dir, "/assets/defaults.cfg");
  if (!probe[0] || stat(probe, &st) != 0) return 0;

  return 1;
}

/* A short record of where we looked, replayed into debug.log once logging is
 * up. osmos_paths_init has to run before log_init -- the log lives in the
 * directory this function is trying to find -- so it cannot log as it goes. */
#define SEARCH_LOG_CAP 1024
static char search_log[SEARCH_LOG_CAP];

#define SD_SWITCH "sdmc:/switch"

static void note(const char *fmt, ...) {
  const size_t used = strlen(search_log);
  if (used + 2 >= SEARCH_LOG_CAP) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(search_log + used, SEARCH_LOG_CAP - used, fmt, ap);
  va_end(ap);
}

/* Scan under sdmc:/switch for a game directory.
 *
 * Two levels, because organising homebrew as /switch/apps/<game>/ is as common
 * as /switch/<game>/ and a search that missed it would look arbitrary. Deeper
 * than that is not scanned: argv[0] already covers arbitrary nesting whenever
 * the launcher provides it, and an unbounded walk of the SD card at startup is
 * a poor trade for the remaining cases.
 *
 * Two passes, and the order is deliberate: a folder whose name mentions osmos
 * wins over one that merely happens to contain the files. Without that, the
 * choice among several candidates would depend on readdir order, which is
 * filesystem order -- so the game could pick a different folder after an
 * unrelated file was added, which is the kind of bug nobody enjoys.
 */
static int name_mentions_osmos(const char *name) {
  /* strcasestr is not in newlib; lowercase by hand. */
  char lower[64];
  size_t i = 0;
  for (; name[i] && i < sizeof(lower) - 1; i++) {
    const char c = name[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lower[i] = 0;
  return strstr(lower, "osmos") != NULL;
}

/* One directory level. depth 0 = the children of `base`, depth 1 = recurse
 * into each child too. `named_only` selects the first pass. */
static int scan_level(const char *base, int depth, int named_only,
                      char *out, size_t cap) {
  DIR *d = opendir(base);
  if (!d) return 0;

  struct dirent *e;
  int looked = 0;
  while ((e = readdir(d)) != NULL && looked < 256) {
    if (e->d_name[0] == '.') continue;
    looked++;

    char cand[FS_MAX_PATH];
    const int n = snprintf(cand, sizeof(cand), "%s/%s", base, e->d_name);
    if (n < 0 || (size_t)n >= sizeof(cand)) continue;

    if ((!named_only || name_mentions_osmos(e->d_name)) &&
        looks_like_game_dir(cand)) {
      closedir(d);
      snprintf(out, cap, "%s", cand);
      return 1;
    }

    if (depth > 0 && scan_level(cand, depth - 1, named_only, out, cap)) {
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}

static int scan_switch_dir(char *out, size_t cap) {
  for (int named_only = 1; named_only >= 0; named_only--) {
    if (scan_level(SD_SWITCH, 1, named_only, out, cap)) {
      note("  found in %s\n", out);
      return 1;
    }
  }
  note("  nothing under %s/*/ or %s/*/*/\n", SD_SWITCH, SD_SWITCH);
  return 0;
}

/* Find the game.
 *
 * The .nro may sit in any folder under /switch, and the game data may sit
 * beside it or in a folder of its own. So:
 *
 *   1. the directory the .nro was launched from (argv[0]) -- the normal case,
 *      and the only one that works at arbitrary nesting depth
 *   2. any folder one level under sdmc:/switch -- covers a forwarder that
 *      passes no argv, and the case where the .nro and the data are kept
 *      apart
 *   3. sdmc:/switch and sdmc:/osmos as last resorts
 *
 * Whichever directory wins also holds config.txt, debug.log and the saves, so
 * the game keeps its state where its data is.
 */
void osmos_paths_init(void) {
  extern int __system_argc;
  extern char **__system_argv;

  root[0] = 0;
  search_log[0] = 0;

  /* --- 1. beside the .nro --- */
  if (__system_argc > 0 && __system_argv && __system_argv[0]) {
    char launch[FS_MAX_PATH];
    snprintf(launch, sizeof(launch), "%s", __system_argv[0]);

    /* Some launchers hand over a bare filename with no directory, and some
     * omit the sdmc: prefix. Neither is a path we can use as-is. */
    char *slash = strrchr(launch, '/');
    if (slash) {
      *slash = 0;
      /* "sdmc:" is 5 chars, so prefixing needs 5 spare; join() reports rather
       * than silently shortening, which for a path is the difference between
       * a clear error and an unexplained missing file. */
      if (strncmp(launch, "sdmc:", 5) != 0 && launch[0] == '/')
        join(root, sizeof(root), "sdmc:", launch);
      else
        snprintf(root, sizeof(root), "%s", launch);

      note("  argv[0] -> %s\n", root);
      if (!looks_like_game_dir(root)) {
        note("    (no libosmos.so + assets/ there)\n");
        root[0] = 0;
      }
    } else {
      note("  argv[0] had no directory: %s\n", launch);
    }
  } else {
    note("  no argv[0] from the launcher\n");
  }

  /* --- 2. one level under sdmc:/switch --- */
  if (!root[0]) {
    note("  scanning %s/*/ and %s/*/*/\n", SD_SWITCH, SD_SWITCH);
    char found[FS_MAX_PATH];
    if (scan_switch_dir(found, sizeof(found)))
      snprintf(root, sizeof(root), "%s", found);
  }

  /* --- 3. last resorts --- */
  if (!root[0]) {
    static const char *const fallbacks[] = {
      "sdmc:/switch/osmos", "sdmc:/switch", "sdmc:/osmos",
    };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(*fallbacks); i++) {
      if (looks_like_game_dir(fallbacks[i])) {
        snprintf(root, sizeof(root), "%s", fallbacks[i]);
        note("  found at %s\n", fallbacks[i]);
        break;
      }
    }
  }

  /* Nothing found. Keep the most likely location so the error screen can name
   * a concrete path rather than an empty string. */
  if (!root[0]) {
    snprintf(root, sizeof(root), "sdmc:/switch/osmos");
    note("  nothing found; defaulting to %s\n", root);
  }

  join(so_path, sizeof(so_path), root, "/libosmos.so");
  resolve_data_dir();
}

/* nativeProvideDataDir gets the directory the engine treats as its install
 * root, and the engine builds every asset path directly beneath it:
 * "Shaders/TexturedVertexShader.vsh", "LocFiles/TouchInput" + "-en.loc",
 * "Textures/", "fonts/", and bare "Osmos-en.loc" at the top. Those literals
 * are wchar_t, so they only show up in the binary as UTF-32.
 *
 * On Android the Java side copied the *contents* of assets/ into the app data
 * directory, so the engine saw Shaders/ directly. Here the folder is kept as
 * assets/ for tidiness, which means the data dir is <root>/assets and NOT
 * <root>. Getting this wrong does not crash at the open: every asset load
 * fails quietly, the shaders never compile, and the first symptom is a null
 * dereference much later.
 *
 * A flattened layout -- Shaders/ directly under <root> -- is accepted too,
 * since that is what an Android data directory actually looked like. */
static void resolve_data_dir(void) {
  char probe[FS_MAX_PATH];
  struct stat st;

  join(probe, sizeof(probe), root, "/assets/Shaders");
  if (probe[0] && stat(probe, &st) == 0) {
    join(data_dir, sizeof(data_dir), root, "/assets");
    return;
  }

  join(probe, sizeof(probe), root, "/Shaders");
  if (probe[0] && stat(probe, &st) == 0) {
    snprintf(data_dir, sizeof(data_dir), "%s", root);
    return;
  }

  /* Neither layout. osmos_paths_check() reports it properly; default to the
   * documented one so the message names a plausible path. */
  join(data_dir, sizeof(data_dir), root, "/assets");
}

/* Called once logging is up, since the search necessarily runs before it. */
void osmos_paths_log_search(void) {
  LOGB("game directory: %s", root);
  LOGB("engine data dir: %s", data_dir);
  LOGI("search:\n%s", search_log[0] ? search_log : "  (none)");
}

/* The engine's setupUserDirectories() creates what it needs under the install
 * dir, so this only has to confirm the two things we cannot create. */
int osmos_paths_check(void) {
  struct stat st;

  if (stat(so_path, &st) != 0) {
    snprintf(path_err, sizeof(path_err),
             "libosmos.so not found.\n\n"
             "  Looked in: %s\n"
             "  ...and in every folder one level under sdmc:/switch/\n\n"
             "Extract lib/arm64-v8a/libosmos.so from your own copy of the\n"
             "Osmos APK and put it, plus the assets/ folder, next to the .nro.\n"
             "tools/prepare_game.sh does both.", root);
    return 0;
  }

  /* Check a file the engine itself opens, through the same data dir it will
   * be given, rather than merely checking that some assets folder exists. */
  char probe[FS_MAX_PATH];
  join(probe, sizeof(probe), data_dir, "/Shaders/TexturedVertexShader.vsh");
  if (stat(probe, &st) != 0) {
    snprintf(path_err, sizeof(path_err),
             "Game assets are incomplete.\n\n"
             "  Data dir: %s\n"
             "  Missing:  Shaders/TexturedVertexShader.vsh\n\n"
             "libosmos.so was found, but the engine cannot see its assets.\n"
             "Copy the whole assets/ folder out of your APK next to it.\n"
             "tools/prepare_game.sh does both.", data_dir);
    return 0;
  }
  return 1;
}

const char *osmos_paths_error(void) { return path_err; }
const char *osmos_root(void)        { return root; }
const char *osmos_so_path(void)     { return so_path; }
const char *osmos_data_dir(void)    { return data_dir; }

/* nativeProvideUUID feeds the engine's per-install id. It is used for local
 * bookkeeping only -- there is no server to report it to -- so any stable
 * value works. Generate once and keep it so progress stays associated. */
const char *osmos_device_uuid(void) {
  if (uuid[0]) return uuid;

  char p[FS_MAX_PATH];
  join(p, sizeof(p), root, "/uuid.txt");

  FILE *f = fopen(p, "r");
  if (f) {
    if (fgets(uuid, sizeof(uuid), f)) {
      char *nl = strpbrk(uuid, "\r\n");
      if (nl) *nl = 0;
    }
    fclose(f);
    if (uuid[0]) return uuid;
  }

  /* randomGet is libnx's own CSPRNG and needs no service brought up first;
   * csrngGetRandomBytes would require csrngInitialize, which the default
   * libnx init does not do. */
  u64 a = armGetSystemTick(), b = 0;
  randomGet(&b, sizeof(b));
  snprintf(uuid, sizeof(uuid), "%016llx%016llx",
           (unsigned long long)a, (unsigned long long)b);

  f = fopen(p, "w");
  if (f) { fprintf(f, "%s\n", uuid); fclose(f); }
  return uuid;
}

/* ------------------------------------------------------------------ */
/* cloud-save stand-in                                                 */
/* ------------------------------------------------------------------ */

static void cloud_path(char *out, size_t n) {
  join(out, n, root, "/cloudsave.bin");
}

void *osmos_save_read_cloud(int *len_out) {
  char p[FS_MAX_PATH]; cloud_path(p, sizeof(p));
  if (len_out) *len_out = 0;

  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }

  void *buf = malloc((size_t)n);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);

  if (got != (size_t)n) { free(buf); return NULL; }
  if (len_out) *len_out = (int)n;
  return buf;
}

/* Write to a temporary file and rename over the original. A half-written save
 * from a crash or a pulled dock is worse than no save, because the engine will
 * happily load the truncated one. */
void osmos_save_write_cloud(const void *data, int len) {
  if (!data || len <= 0) return;
  char p[FS_MAX_PATH], t[FS_MAX_PATH];
  cloud_path(p, sizeof(p));
  join(t, sizeof(t), p, ".tmp");

  FILE *f = fopen(t, "wb");
  if (!f) return;
  const size_t wrote = fwrite(data, 1, (size_t)len, f);
  fclose(f);
  if (wrote != (size_t)len) { remove(t); return; }
  remove(p);
  rename(t, p);
}

void osmos_save_reset(void) {
  char p[FS_MAX_PATH];
  cloud_path(p, sizeof(p));
  remove(p);
  join(p, sizeof(p), root, "/achievements.txt");
  remove(p);
  LOGI("progress reset requested; local save cleared");
}

/* There is no Play Games here, so achievements are appended to a text file.
 * The engine fires updateAchievement repeatedly for the same id, so dedupe. */
void osmos_save_record_achievement(int id) {
  char p[FS_MAX_PATH];
  join(p, sizeof(p), root, "/achievements.txt");

  FILE *f = fopen(p, "r");
  if (f) {
    char line[64];
    while (fgets(line, sizeof(line), f)) {
      if (atoi(line) == id) { fclose(f); return; }
    }
    fclose(f);
  }
  f = fopen(p, "a");
  if (!f) return;
  fprintf(f, "%d\n", id);
  fclose(f);
  LOGI("achievement %d unlocked", id);
}
