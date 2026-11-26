#ifndef CORE_MOD_FILE_POS_FILE_H
#define CORE_MOD_FILE_POS_FILE_H

/*
 * Pos.txt reader + watcher — own position from the Conan Exiles mod.
 *
 * File format (single line, values may use comma decimals on DE locale):
 *   SEQ=<int> X=<float> Y=<float> Z=<float> YAW=<float> [YAWY=<float>]
 *   X/Y/Z are centimeters (stored as double internally); consumers divide by 100 for meters.
 *
 * Thread contract:
 *  - pos_file_read_once: pure read, callable from any thread.
 *  - pos_watcher_start/stop: called on the TS callback thread (init/shutdown).
 *  - pos_get_current / pos_coordinates_valid: callable from any thread; the
 *    snapshot is guarded by a private lock inside this module.
 *  - pos_inject_sample: any thread; only STORES the sample and queues the
 *    notify (a flag + an auto-reset event); logs POS method http->… via
 *    log_write (log lock only, never TS API). It never runs the notify
 *    callback itself, so the HTTP listener answers the game without waiting
 *    for any plugin work.
 *  - The inject notify callback runs on the WATCHER thread (woken at once by
 *    the event, not at its next 30 ms tick), coalesced: several injects
 *    before the watcher wakes give ONE call.
 *  - Watcher logs POS method file/none (log_write on change) and, with
 *    debug=1, a 1 Hz status line.
 */

#include "core/util/poll_interval.h"

#include <wchar.h>

typedef struct PosSample {
    int seq;
    double x;     /* centimeters — double preserves Pos.txt/HTTP digits until CEPOS float wire */
    double y;
    double z;
    double yaw;   /* degrees */
    double yawY;  /* degrees, 0 if missing */
} PosSample;

/* Read + parse Pos.txt once. Returns 1 on success. Shared-access open with
   retries because Conan may hold the file while writing.  Parsing and sanity
   rules (real numbers only, finite/sane values): core/mod_file/pos_parse.h. */
int pos_file_read_once(const wchar_t* filePath, PosSample* out);

/* Start/stop the polling thread (PLUGIN_POLL_INTERVAL_MS). Start is idempotent. Stop blocks
   until the thread has exited (max ~2 s). */
void pos_watcher_start(void);
void pos_watcher_stop(void);

/* Optional: called from the watcher thread on every poll while coordinates
   are valid, plus once when they turn invalid. The callback must be fast and
   must NOT call the TS API. Register before pos_watcher_start. */
void pos_watcher_set_update_callback(void (*callback)(void));

/* Optional: called every PLUGIN_POLL_INTERVAL_MS on the watcher thread,
   regardless of coordinate validity (hotkeys, etc.). Must be fast and must
   NOT call the TS API. Register before pos_watcher_start. */
void pos_watcher_set_tick_callback(void (*callback)(void));

/* Auto-detect the Conan Saved folder (Steam registry + libraryfolders.vdf)
   when AutomaticPatchFind is on and the stored path is empty/gone; persists
   the result to plugin.cfg.  Any thread (TS callback at init / settings apply,
   and the pos watcher every 15 s): it changes ONLY automaticSavedPath, through
   config_modify (compare-and-set), so it cannot overwrite what another thread
   changed in the settings struct. */
void pos_autodetect_saved_path(void);

/* Latest sample. Returns 1 when coordinates are valid (fresh). */
int pos_get_current(PosSample* out);

/* 1 while Pos.txt is actively written or within the post-read grace window. */
int pos_coordinates_valid(void);

/* Inject a position sample from an external source (e.g. HTTP thread).
   Callable from any thread; must NOT call the TS API.
   Returns 1 if accepted (sample is plausible and watcher is running).
   Accepted injections suppress ALL Pos.txt file access (path lookup, stat and
   read) for ~2 s (HTTP hold window); the file is used only when HTTP stops. */
int pos_inject_sample(const PosSample* sample);

/* Register a callback that runs after successful pos_inject_sample calls - on
   the WATCHER thread, not on the injecting thread, and coalesced: injects that
   arrive before the watcher wakes give one call. It runs on the same thread as
   the update callback, so the two never run at the same time. Every consumer
   must therefore be an idempotent "pending" signal. Must be fast and must NOT
   call the TS API. Not called while the watcher stops. */
void pos_inject_set_notify_callback(void (*callback)(void));

#endif /* CORE_MOD_FILE_POS_FILE_H */
