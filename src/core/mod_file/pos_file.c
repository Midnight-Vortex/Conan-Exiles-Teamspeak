#include "core/mod_file/pos_file.h"
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

static int pos_parse_field(const char* buffer, const char* key, float* out) {
    const char* p = strstr(buffer, key);
    if (!p) {
        return 0;
    }
    *out = (float)strtod(p + strlen(key), NULL);
    return 1;
}

int pos_file_read_once(const wchar_t* filePath, PosSample* out) {
    if (!filePath || !out || !filePath[0]) {
        return 0;
    }

    char buffer[256];
    if (!pos_read_raw(filePath, buffer, sizeof(buffer))) {
        return 0;
    }

    float seq = 0.0f;
    PosSample sample;
    memset(&sample, 0, sizeof(sample));

    if (!pos_parse_field(buffer, "SEQ=", &seq)
        || !pos_parse_field(buffer, "X=", &sample.x)
        || !pos_parse_field(buffer, "Y=", &sample.y)
        || !pos_parse_field(buffer, "Z=", &sample.z)
        || !pos_parse_field(buffer, "YAW=", &sample.yaw)) {
        return 0;
    }
    sample.seq = (int)seq;

    /* YAWY is optional (older mod versions). */
    if (!pos_parse_field(buffer, "YAWY=", &sample.yawY)) {
        sample.yawY = 0.0f;
    }

    *out = sample;
    return 1;
}

/* Reject mod loading placeholder (0,0,0 cm) — breaks distance for all speakers. */
static int pos_sample_is_plausible(const PosSample* sample) {
    if (!sample) {
        return 0;
    }
    if (!isfinite(sample->x) || !isfinite(sample->y) || !isfinite(sample->z)) {
        return 0;
    }
    if (fabsf(sample->x) < 1.0f && fabsf(sample->y) < 1.0f && fabsf(sample->z) < 1.0f) {
        return 0;
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

/* Milliseconds since Pos.txt was last written, or MAXULONGLONG if missing. */
static ULONGLONG pos_file_write_age_ms(const wchar_t* filePath) {
    ULONGLONG writtenQuad = 0;
    if (!pos_get_file_write_quad(filePath, &writtenQuad)) {
        return MAXULONGLONG;
    }

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
    if (!path_detect_conan_saved(detected, CONFIG_MAX_PATH)) {
        if (cfg.automaticSavedPath[0]) {
            log_write("POS: autodetect failed — clearing invalid AutomaticSavedPath");
            cfg.automaticSavedPath[0] = L'\0';
            config_apply(&cfg);
            config_save();
        }
        return;
    }
    wcsncpy_s(cfg.automaticSavedPath, CONFIG_MAX_PATH, detected, _TRUNCATE);
    config_apply(&cfg);
    config_save();
    log_write("POS: autodetect saved path -> %ls", cfg.automaticSavedPath);
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
    int lastSeq = -1;

    log_write("POS: watcher started");

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, PLUGIN_POLL_INTERVAL_MS) != WAIT_TIMEOUT) {
            break;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastAutodetectMs >= POS_AUTODETECT_RETRY_MS) {
            lastAutodetectMs = now;
            pos_autodetect_saved_path();
        }

        pos_resolve_file_path(filePath, sizeof(filePath) / sizeof(filePath[0]));

        if (!filePath[0]) {
            if (now - lastMissingLog > POS_LOG_THROTTLE_MS) {
                lastMissingLog = now;
                log_debug("POS: no Pos.txt path configured");
            }
            InterlockedExchange(&g_coordinatesValid, 0);
            continue;
        }

        const ULONGLONG age = pos_file_write_age_ms(filePath);
        const int fileActive = (age != MAXULONGLONG && age <= POS_STALE_MS);

        ULONGLONG writeQuad = 0;
        int writeChanged = 0;
        if (pos_get_file_write_quad(filePath, &writeQuad)) {
            writeChanged = (g_lastFileWriteQuad != 0 && writeQuad != g_lastFileWriteQuad);
            g_lastFileWriteQuad = writeQuad;
        }

        PosSample sample;
        memset(&sample, 0, sizeof(sample));
        int readOk = fileActive && pos_file_read_once(filePath, &sample);
        if (readOk && !pos_sample_is_plausible(&sample)) {
            if (now - lastValidLog > POS_LOG_THROTTLE_MS) {
                lastValidLog = now;
                log_debug("POS: read rejected (invalid coords seq=%d pos=%.1f/%.1f/%.1f)",
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

        if (acceptRead) {
            EnterCriticalSection(&g_posLock);
            g_currentSample = sample;
            LeaveCriticalSection(&g_posLock);

            g_lastValidTick = now;

            if (!currentlyValid) {
                log_write("POS: coordinates valid (seq=%d pos=%.1f/%.1f/%.1f yaw=%.1f)",
                    sample.seq, sample.x, sample.y, sample.z, sample.yaw);
            }
            InterlockedExchange(&g_coordinatesValid, 1);

            if (sample.seq != lastSeq || now - lastValidLog > POS_LOG_THROTTLE_MS) {
                if (now - lastValidLog > POS_LOG_THROTTLE_MS) {
                    lastValidLog = now;
                    log_debug("POS: seq=%d pos=%.1f/%.1f/%.1f yaw=%.1f yawY=%.1f age=%llums",
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

            if (inGrace) {
                /* File stale/missing briefly — keep last good coords (login tolerance). */
            }
            else if (currentlyValid) {
                log_write("POS: coordinates invalid (file %s, age=%llums, grace=%llums)",
                    age == MAXULONGLONG ? "missing" : "stale",
                    age == MAXULONGLONG ? 0ULL : (unsigned long long)age,
                    g_lastValidTick != 0 ? (unsigned long long)(now - g_lastValidTick) : 0ULL);
                InterlockedExchange(&g_coordinatesValid, 0);
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
