#ifndef CORE_MOD_FILE_PATH_DETECT_H
#define CORE_MOD_FILE_PATH_DETECT_H

/*
 * Automatic Conan Exiles "Saved" folder detection (ported from the old
 * plugin's findConanExilesAutomatic):
 *   Steam registry (InstallPath) -> steamapps\libraryfolders.vdf ->
 *   library containing appid 440900 ->
 *   <library>\steamapps\common\Conan Exiles\ConanSandbox\Saved
 *
 * Pure Win32/file access — no TS API, no globals. Any thread, in practice
 * the TS callback thread (init / settings apply).
 */

#include <wchar.h>

/* Fill out with the verified Saved folder. Returns 1 on success,
   0 when Steam/Conan could not be located. */
int path_detect_conan_saved(wchar_t* out, size_t outLen);

#endif /* CORE_MOD_FILE_PATH_DETECT_H */
