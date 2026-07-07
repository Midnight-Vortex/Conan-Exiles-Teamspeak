#ifndef CORE_MOD_FILE_POS_FILE_H
#define CORE_MOD_FILE_POS_FILE_H

/*
 * Pos.txt reader + watcher — own position from the Conan Exiles mod.
 *
 * File format (single line, values may use comma decimals on DE locale):
 *   SEQ=<int> X=<float> Y=<float> Z=<float> YAW=<float> [YAWY=<float>]
 *   X/Y/Z are centimeters; consumers divide by 100 for meters.
 *
 * Thread contract:
 *  - pos_file_read_once: pure read, callable from any thread.
 *  - pos_watcher_start/stop: called on the TS callback thread (init/shutdown).
 *  - pos_get_current / pos_coordinates_valid: callable from any thread; the
 *    snapshot is guarded by a private lock inside this module.
 */

#include <wchar.h>

typedef struct PosSample {
    int seq;
    float x;      /* centimeters */
    float y;
    float z;
    float yaw;    /* degrees */
    float yawY;   /* degrees, 0 if missing */
} PosSample;

/* Read + parse Pos.txt once. Returns 1 on success. Shared-access open with
   retries because Conan may hold the file while writing. */
int pos_file_read_once(const wchar_t* filePath, PosSample* out);

/* Start/stop the polling thread (~30 ms). Start is idempotent. Stop blocks
   until the thread has exited (max ~2 s). */
void pos_watcher_start(void);
void pos_watcher_stop(void);

/* Optional: called from the watcher thread on every poll while coordinates
   are valid, plus once when they turn invalid. The callback must be fast and
   must NOT call the TS API. Register before pos_watcher_start. */
void pos_watcher_set_update_callback(void (*callback)(void));

/* Auto-detect the Conan Saved folder (Steam registry + libraryfolders.vdf)
   when AutomaticPatchFind is on and the stored path is empty/gone; persists
   the result to plugin.cfg. TS callback thread (init / settings apply). */
void pos_autodetect_saved_path(void);

/* Latest sample. Returns 1 when coordinates are valid (fresh). */
int pos_get_current(PosSample* out);

/* 1 while Pos.txt is actively written (last write <= 5 s ago and parsed ok). */
int pos_coordinates_valid(void);

#endif /* CORE_MOD_FILE_POS_FILE_H */
