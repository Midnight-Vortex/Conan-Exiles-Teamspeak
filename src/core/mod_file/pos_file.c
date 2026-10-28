#include "core/mod_file/pos_file.h"
#include "core/mod_file/pos_parse.h"
#include "core/mod_file/path_detect.h"
#include "core/config/config.h"
#include "core/util/log.h"
#include "core/util/poll_interval.h"

#include <windows.h>
#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POS_STALE_MS            5000
#define POS_COORD_GRACE_MS      15000  /* keep valid after last good read when file goes stale */
#define POS_FRESH_ACCEPT_MS     2000   /* cold-start: accept without write-change only when very fresh */
#define POS_LOG_THROTTLE_MS     30000
#define POS_AUTODETECT_RETRY_MS 15000
#define POS_DEBUG_STATUS_MS     1000   /* debug=1: method/hold/fileAge snapshot */

#define POS_SOURCE_NONE  0
#define POS_SOURCE_HTTP  1
#define POS_SOURCE_FILE  2

/* Private lock guarding g_currentSample. Never shared with other modules. */
static CRITICAL_SECTION g_posLock;
static PosSample g_currentSample;
static volatile long g_coordinatesValid = 0;
static ULONGLONG g_lastValidTick = 0;
static ULONGLONG g_lastFileWriteQuad = 0;

static HANDLE g_watcherThread = NULL;
static HANDLE g_stopEvent = NULL;
static volatile long g_watcherRunning = 0;
static void (*g_updateCallback)(void) = NULL;
static void (*g_tickCallback)(void) = NULL;
static void (*g_injectNotifyCallback)(void) = NULL;
/* Hold window: when an HTTP-injected position is active the watcher skips file
   reads (only the file's timestamp is still looked at) and does not
   invalidate coordinates. Written by any thread (x64 aligned
   volatile ULONGLONG write is atomic); read only by the watcher thread. */
static volatile ULONGLONG g_httpHoldUntilMs = 0;
static volatile long g_posSource = POS_SOURCE_NONE;

static const char* pos_source_name(long src) {
    switch (src) {
    case POS_SOURCE_HTTP:
        return "http";
    case POS_SOURCE_FILE:
        return "file";
    default:
        return "none";
    }
}

/* Log only on change (HTTP thread or watcher). Never TS API. */
static void pos_set_source(long next) {
    const long prev = InterlockedExchange(&g_posSource, next);
    if (prev == next) {
        return;
    }
    log_write("POS: method %s -> %s", pos_source_name(prev), pos_source_name(next));
}

void pos_watcher_set_update_callback(void (*callback)(void)) {
    g_updateCallback = callback;
}

void pos_watcher_set_tick_callback(void (*callback)(void)) {
    g_tickCallback = callback;
}

/* ---- 2.1 pure read function ------------------------------------------- */

static int pos_read_raw(const wchar_t* filePath, char* buffer, size_t bufferSize) {
    for (int attempt = 0; attempt < 6; attempt++) {
        HANDLE hFile = CreateFileW(
            filePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION) {
                Sleep(attempt < 2 ? 1 : 5);
                continue;
            }
            return 0;
        }

        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hFile, buffer, (DWORD)(bufferSize - 1), &bytesRead, NULL);
        CloseHandle(hFile);
        if (!ok || bytesRead == 0) {
            Sleep(2);
            continue;
        }

        buffer[bytesRead] = '\0';
        /* DE locale writes comma decimals — normalize for strtod. */
        for (char* p = buffer; *p; ++p) {
            if (*p == ',') {
                *p = '.';
            }
        }
        return 1;
    }
    return 0;
}

int pos_file_read_once(const wchar_t* filePath, PosSample* out) {
    if (!filePath || !out || !filePath[0]) {
        return 0;
    }

    char buffer[256];
    if (!pos_read_raw(filePath, buffer, sizeof(buffer))) {
        return 0;
    }

    /* Pure parse (pos_parse.h): every field needs a real number, all five
       values finite and sane. */
    return pos_parse_line(buffer, out);
}

/* ---- inject from external source --------------------------------------- */

void pos_inject_set_notify_callback(void (*callback)(void)) {
    g_injectNotifyCallback = callback;
}

int pos_inject_sample(const PosSample* sample) {
    if (!sample || !pos_sample_is_plausible(sample)) {
        return 0;
    }
    /* Guard: lock only exists while the watcher is running. */
    if (InterlockedCompareExchange(&g_watcherRunning, 0, 0) == 0) {
        return 0;
    }

    const ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&g_posLock);
    g_currentSample = *sample;
    LeaveCriticalSection(&g_posLock);

    InterlockedExchange(&g_coordinatesValid, 1);
    g_lastValidTick = now;
    g_httpHoldUntilMs = now + 2000ULL;
    pos_set_source(POS_SOURCE_HTTP);

    if (g_injectNotifyCallback) {
        g_injectNotifyCallback();
    }
    return 1;
}

/* ---- 2.3 staleness ------------------------------------------------------ */

static int pos_get_file_write_quad(const wchar_t* filePath, ULONGLONG* outQuad) {
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(filePath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }
    FindClose(hFind);

    ULARGE_INTEGER written;
    written.LowPart = findData.ftLastWriteTime.dwLowDateTime;
    written.HighPart = findData.ftLastWriteTime.dwHighDateTime;
    *outQuad = written.QuadPart;
    return 1;
}

/* Milliseconds since a Pos.txt write time (FILETIME quad), clock-skew safe. */
static ULONGLONG pos_age_from_quad(ULONGLONG writtenQuad) {
    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER now;
    now.LowPart = ftNow.dwLowDateTime;
    now.HighPart = ftNow.dwHighDateTime;
    if (writtenQuad > now.QuadPart) {
        return 0;
    }
    return (now.QuadPart - writtenQuad) / 10000ULL;
}

/* ---- path validation ---------------------------------------------------- */

/* 1 when the Saved-folder path is usable (exists on disk, not a UI placeholder). */
static int pos_saved_path_is_valid(const wchar_t* path) {
    if (!path || !path[0]) {
        return 0;
    }
    if (wcsstr(path, L"(Not configured)") != NULL
        || wcsstr(path, L"(not configured)") != NULL) {
        return 0;
    }
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* ---- watcher thread ----------------------------------------------------- */

/* Compare-and-set of ONLY automaticSavedPath (config_modify mutator): applied
   when the field still holds what the detection saw, so a path the settings
   dialog set in the meantime is never overwritten. */
typedef struct AutoPathPatch {
    const wchar_t* expected; /* value the detection started from */
    const wchar_t* value;    /* new value ("" = clear) */
    int applied;
} AutoPathPatch;

static void pos_autodetect_patch(PluginConfig* cfg, void* ctx) {
    AutoPathPatch* patch = (AutoPathPatch*)ctx;
    if (wcscmp(cfg->automaticSavedPath, patch->expected) != 0) {
        return;
    }
    wcsncpy_s(cfg->automaticSavedPath, CONFIG_MAX_PATH, patch->value, _TRUNCATE);
    patch->applied = 1;
}

void pos_autodetect_saved_path(void) {
    PluginConfig cfg;
    config_copy(&cfg);

    if (!cfg.automaticPatchFind) {
        return;
    }
    /* Keep a stored path that still exists; re-detect when empty or bogus. */
    if (pos_saved_path_is_valid(cfg.automaticSavedPath)) {
        return;
    }

    wchar_t detected[CONFIG_MAX_PATH] = L"";
    AutoPathPatch patch;
    patch.expected = cfg.automaticSavedPath;
    patch.applied = 0;

    if (!path_detect_conan_saved(detected, CONFIG_MAX_PATH)) {
        if (cfg.automaticSavedPath[0]) {
            log_write("POS: autodetect failed — clearing invalid AutomaticSavedPath");
            patch.value = L"";
            config_modify(pos_autodetect_patch, &patch);
            if (patch.applied) {
                config_save();
            }
        }
        return;
    }
    patch.value = detected;
    config_modify(pos_autodetect_patch, &patch);
    if (patch.applied) {
        config_save();
        log_write("POS: autodetect saved path -> %ls", detected);
    }
}

/* Pos.txt path from config: automatic path when enabled and set, else manual.
   Uses config_copy — the settings dialog may rewrite the path strings while
   this thread reads them. */
static void pos_resolve_file_path(wchar_t* out, size_t outLen) {
    PluginConfig cfg;
    config_copy(&cfg);

    const wchar_t* base = NULL;
    if (cfg.automaticPatchFind && pos_saved_path_is_valid(cfg.automaticSavedPath)) {
        base = cfg.automaticSavedPath;
    }
    else if (pos_saved_path_is_valid(cfg.savedPath)) {
        base = cfg.savedPath;
    }

    if (base) {
        swprintf(out, outLen, L"%s\\Pos.txt", base);
    }
    else {
        out[0] = L'\0';
    }
}

static unsigned __stdcall pos_watcher_thread(void* arg) {
    (void)arg;

    wchar_t filePath[CONFIG_MAX_PATH + 16];
    ULONGLONG lastMissingLog = 0;
    ULONGLONG lastValidLog = 0;
    ULONGLONG lastAutodetectMs = 0;
    ULONGLONG lastStatusMs = 0;
    ULONGLONG lastHoldSkipLog = 0;
    int lastSeq = -1;

    log_write("POS: watcher started");
    if (!g_config.enablePosFile) {
        log_write("POS: Pos.txt disabled (EnablePosFile=false, HTTP-only)");
    }

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, PLUGIN_POLL_INTERVAL_MS) != WAIT_TIMEOUT) {
            break;
        }

        const ULONGLONG now = GetTickCount64();
        const int posFileEnabled = g_config.enablePosFile;
        ULONGLONG dbgFileAge = MAXULONGLONG;

        if (posFileEnabled && now - lastAutodetectMs >= POS_AUTODETECT_RETRY_MS) {
            lastAutodetectMs = now;
            pos_autodetect_saved_path();
        }

        /* Pos.txt is used only when it is enabled AND a path is known. Without a
           path the tick behaves exactly like HTTP-only mode (else branch below):
           an active HTTP inject hold or its grace keeps the coordinates. It used
           to invalidate them on every 30 ms tick, so with the game mod posting
           over HTTP but no Saved path found, "valid" flapped against the inject. */
        int useFile = 0;
        if (posFileEnabled) {
            pos_resolve_file_path(filePath, sizeof(filePath) / sizeof(filePath[0]));
            if (filePath[0]) {
                useFile = 1;
            }
            else if (now - lastMissingLog > POS_LOG_THROTTLE_MS) {
                lastMissingLog = now;
                log_debug("POS: no Pos.txt path configured");
            }
        }

        if (useFile) {
            /* ONE directory lookup per tick gives both the write time (change
               detection) and the age (staleness). */
            ULONGLONG writeQuad = 0;
            int writeChanged = 0;
            ULONGLONG age = MAXULONGLONG;
            if (pos_get_file_write_quad(filePath, &writeQuad)) {
                age = pos_age_from_quad(writeQuad);
                writeChanged = (g_lastFileWriteQuad != 0 && writeQuad != g_lastFileWriteQuad);
                g_lastFileWriteQuad = writeQuad;
            }
            dbgFileAge = age;
            const int fileActive = (age != MAXULONGLONG && age <= POS_STALE_MS);

            /* HTTP hold: the injected position is authoritative and any file
               sample would be discarded below - do not even open/read Pos.txt
               (its write time above is still tracked). */
            const ULONGLONG holdUntil = g_httpHoldUntilMs;
            const int httpHoldActive = (now < holdUntil);
            if (httpHoldActive && fileActive
                && now - lastHoldSkipLog >= POS_DEBUG_STATUS_MS) {
                lastHoldSkipLog = now;
                log_debug("POS: Pos.txt skipped (HTTP hold left=%llums age=%llums)",
                    (unsigned long long)(holdUntil - now),
                    (unsigned long long)age);
            }

            PosSample sample;
            memset(&sample, 0, sizeof(sample));
            int readOk = fileActive && !httpHoldActive && pos_file_read_once(filePath, &sample);
            if (readOk && !pos_sample_is_plausible(&sample)) {
                if (now - lastValidLog > POS_LOG_THROTTLE_MS) {
                    lastValidLog = now;
                    log_debug("POS: read rejected (invalid coords seq=%d pos=X=%.6f Y=%.6f Z=%.6f)",
                        sample.seq, sample.x, sample.y, sample.z);
                }
                readOk = 0;
            }
            const int currentlyValid = InterlockedCompareExchange(&g_coordinatesValid, 0, 0) != 0;
            int acceptRead = readOk;
            if (readOk && !currentlyValid) {
                /* Reject leftover Pos.txt from a previous session (old plugin rule). */
                acceptRead = writeChanged
                    || age <= POS_FRESH_ACCEPT_MS
                    || (lastSeq >= 0 && sample.seq != lastSeq);
            }
            /* HTTP hold: injected position is authoritative; block file overwrites. */
            if (acceptRead && now < g_httpHoldUntilMs) {
                if (now - lastHoldSkipLog >= POS_DEBUG_STATUS_MS) {
                    lastHoldSkipLog = now;
                    log_debug("POS: Pos.txt skipped (HTTP hold left=%llums seq=%d age=%llums)",
                        (unsigned long long)(g_httpHoldUntilMs - now),
                        sample.seq,
                        (unsigned long long)age);
                }
                acceptRead = 0;
            }

            if (acceptRead) {
                EnterCriticalSection(&g_posLock);
                g_currentSample = sample;
                LeaveCriticalSection(&g_posLock);

                g_lastValidTick = now;

                if (!currentlyValid) {
                    log_write("POS: coordinates valid source=file (seq=%d pos=X=%.6f Y=%.6f Z=%.6f YAW=%.6f)",
                        sample.seq, sample.x, sample.y, sample.z, sample.yaw);
                }
                InterlockedExchange(&g_coordinatesValid, 1);
                pos_set_source(POS_SOURCE_FILE);

                if (sample.seq != lastSeq || now - lastValidLog > POS_LOG_THROTTLE_MS) {
                    if (now - lastValidLog > POS_LOG_THROTTLE_MS) {
                        lastValidLog = now;
                        log_debug("POS: method=file seq=%d pos=X=%.6f Y=%.6f Z=%.6f YAW=%.6f YAWY=%.6f age=%llums",
                            sample.seq, sample.x, sample.y, sample.z,
                            sample.yaw, sample.yawY, (unsigned long long)age);
                    }
                    lastSeq = sample.seq;
                }

                if (g_updateCallback && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                    g_updateCallback();
                }
            }
            else {
                const int inGrace = currentlyValid
                    && g_lastValidTick != 0
                    && now - g_lastValidTick < POS_COORD_GRACE_MS;
                const int httpHold = (now < g_httpHoldUntilMs);

                if (inGrace || httpHold) {
                    /* Keep coords — file temporarily stale/missing (grace) or HTTP hold active. */
                }
                else if (currentlyValid) {
                    log_write("POS: coordinates invalid source=file (%s, age=%llums, grace=%llums)",
                        age == MAXULONGLONG ? "missing" : "stale",
                        age == MAXULONGLONG ? 0ULL : (unsigned long long)age,
                        g_lastValidTick != 0 ? (unsigned long long)(now - g_lastValidTick) : 0ULL);
                    InterlockedExchange(&g_coordinatesValid, 0);
                    pos_set_source(POS_SOURCE_NONE);
                    g_lastValidTick = 0;
                    if (g_updateCallback && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                        g_updateCallback(); /* one shot on the valid->invalid edge */
                    }
                }
                else {
                    InterlockedExchange(&g_coordinatesValid, 0);
                }

                if (readOk && !acceptRead && now - lastValidLog > POS_LOG_THROTTLE_MS) {
                    lastValidLog = now;
                    log_debug("POS: read rejected (stale leftover? age=%llums seq=%d writeChanged=%d)",
                        (unsigned long long)age, sample.seq, writeChanged);
                }

                if (age == MAXULONGLONG && now - lastMissingLog > POS_LOG_THROTTLE_MS) {
                    lastMissingLog = now;
                    log_debug("POS: Pos.txt missing at %ls", filePath);
                }
            }
        }
        else {
            /* HTTP-only (EnablePosFile=false, or no Pos.txt path known): skip
               Pos.txt reads; keep coords via inject hold + grace. */
            const int currentlyValid = InterlockedCompareExchange(&g_coordinatesValid, 0, 0) != 0;
            const int inGrace = currentlyValid
                && g_lastValidTick != 0
                && now - g_lastValidTick < POS_COORD_GRACE_MS;
            const int httpHold = (now < g_httpHoldUntilMs);

            if (inGrace || httpHold) {
                /* Keep coords — HTTP hold active or grace after last inject. */
            }
            else if (currentlyValid) {
                log_write("POS: coordinates invalid source=http (HTTP-only, grace=%llums)",
                    g_lastValidTick != 0 ? (unsigned long long)(now - g_lastValidTick) : 0ULL);
                InterlockedExchange(&g_coordinatesValid, 0);
                pos_set_source(POS_SOURCE_NONE);
                g_lastValidTick = 0;
                if (g_updateCallback && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                    g_updateCallback();
                }
            }
            else {
                InterlockedExchange(&g_coordinatesValid, 0);
            }
        }

        if (log_is_enabled() && now - lastStatusMs >= POS_DEBUG_STATUS_MS) {
            lastStatusMs = now;
            const ULONGLONG holdLeft = (now < g_httpHoldUntilMs)
                ? (g_httpHoldUntilMs - now) : 0ULL;
            if (dbgFileAge == MAXULONGLONG) {
                log_debug("POS: method=%s valid=%d holdLeft=%llums posFile=%d fileAge=n/a seq=%d",
                    pos_source_name(InterlockedCompareExchange(&g_posSource, 0, 0)),
                    InterlockedCompareExchange(&g_coordinatesValid, 0, 0) != 0,
                    (unsigned long long)holdLeft,
                    posFileEnabled,
                    lastSeq);
            }
            else {
                log_debug("POS: method=%s valid=%d holdLeft=%llums posFile=%d fileAge=%llums seq=%d",
                    pos_source_name(InterlockedCompareExchange(&g_posSource, 0, 0)),
                    InterlockedCompareExchange(&g_coordinatesValid, 0, 0) != 0,
                    (unsigned long long)holdLeft,
                    posFileEnabled,
                    (unsigned long long)dbgFileAge,
                    lastSeq);
            }
        }
        if (g_tickCallback && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
            g_tickCallback();
        }
    }

    log_write("POS: watcher stopped");
    return 0;
}

/* ---- 2.2 start/stop ----------------------------------------------------- */

void pos_watcher_start(void) {
    if (InterlockedCompareExchange(&g_watcherRunning, 1, 0) != 0) {
        return; /* already running */
    }

    InitializeCriticalSection(&g_posLock);
    memset(&g_currentSample, 0, sizeof(g_currentSample));
    InterlockedExchange(&g_coordinatesValid, 0);
    InterlockedExchange(&g_posSource, POS_SOURCE_NONE);
    g_lastValidTick = 0;
    g_lastFileWriteQuad = 0;

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) {
        log_write("POS: failed to create stop event");
        DeleteCriticalSection(&g_posLock);
        InterlockedExchange(&g_watcherRunning, 0);
        return;
    }

    g_watcherThread = (HANDLE)_beginthreadex(NULL, 0, pos_watcher_thread, NULL, 0, NULL);
    if (!g_watcherThread) {
        log_write("POS: failed to start watcher thread");
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        DeleteCriticalSection(&g_posLock);
        InterlockedExchange(&g_watcherRunning, 0);
    }
}

void pos_watcher_stop(void) {
    if (InterlockedCompareExchange(&g_watcherRunning, 0, 0) == 0) {
        return; /* not running */
    }

    SetEvent(g_stopEvent);
    if (WaitForSingleObject(g_watcherThread, 10000) != WAIT_OBJECT_0) {
        log_write("POS: watcher thread slow to exit - waiting");
        WaitForSingleObject(g_watcherThread, INFINITE);
    }
    CloseHandle(g_watcherThread);
    g_watcherThread = NULL;
    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;

    InterlockedExchange(&g_coordinatesValid, 0);
    InterlockedExchange(&g_watcherRunning, 0); /* readers bail before lock teardown */
    DeleteCriticalSection(&g_posLock);
}

/* ---- readers ------------------------------------------------------------ */

int pos_get_current(PosSample* out) {
    if (!out || InterlockedCompareExchange(&g_watcherRunning, 0, 0) == 0) {
        return 0;
    }

    EnterCriticalSection(&g_posLock);
    *out = g_currentSample;
    LeaveCriticalSection(&g_posLock);

    return pos_coordinates_valid();
}

int pos_coordinates_valid(void) {
    return InterlockedCompareExchange(&g_coordinatesValid, 0, 0) != 0;
}
