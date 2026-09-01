/* osmos_paths.h -- where the game's files live. MIT licensed.
 *
 * Osmos never reads the APK. AssetList.getAssetFiles is declared in the dex
 * but is not referenced anywhere in libosmos.so, which means the Java side
 * copied assets/ out of the APK into the app's data directory at first run
 * and the engine then used plain fopen against that path. So there is no
 * asset manager and no zip reader here: nativeProvideDataDir gets a directory
 * and everything follows from it.
 */
#ifndef OSMOS_PATHS_H
#define OSMOS_PATHS_H

/* Find the game. The .nro may live in any folder under /switch, and the game
 * data may sit beside it or in a folder of its own; see osmos_paths.c for the
 * search order. Must run before cfg_load() and log_init(), both of which live
 * in the directory this finds. */
void        osmos_paths_init(void);
void        osmos_paths_log_search(void);  /* replay the search into debug.log */
int         osmos_paths_check(void);  /* 1 if libosmos.so and assets/ are there */
const char *osmos_paths_error(void);  /* why check() failed */

const char *osmos_root(void);         /* the folder the .nro was launched from */
const char *osmos_so_path(void);      /* <root>/libosmos.so */
/* Handed to nativeProvideDataDir. NOT the same as osmos_root(): the engine
 * builds asset paths directly beneath this ("Shaders/...", "LocFiles/..."),
 * so it is <root>/assets, while config.txt, debug.log and the saves stay in
 * <root> where the user put the .nro. */
const char *osmos_data_dir(void);
const char *osmos_device_uuid(void);  /* stable per-console id, generated once */

#endif
