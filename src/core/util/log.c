#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>

/* Private lock — never shared with any other module. */
static CRITICAL_SECTION g_logLock;
static INIT_ONCE g_logLockInit = INIT_ONCE_STATIC_INIT;
static volatile long g_logEnabled = 0;
static wchar_t g_logPath[MAX_PATH];
static volatile long g_logPathReady = 0;

#define LOG_MAX_BYTES_NORMAL (250ull * 1024ull * 1024ull)
#define LOG_MAX_BYTES_DEBUG  (1ull * 1024ull * 1024ull * 1024ull)

/* Persistent log handle (review fix). The file used to be opened, size-checked
 * and closed for EVERY line (GetFileAttributesExW + CreateFileW + WriteFile +
 * CloseHandle under the log lock) — a lot of syscalls on the hot path of a
 * plugin that logs at debug rates. The handle now stays open:
 *   - shared READ|WRITE|DELETE, so editors, copy and delete keep working;
 *   - opened with FILE_APPEND_DATA (+ FILE_READ_ATTRIBUTES to query the size),
 *     every WriteFile appends atomically even with a second TS instance;
 *   - re-opened after the size-cap reset (CREATE_ALWAYS truncates), after a
 *     write error, and when the periodic path check (every
 *     LOG_VERIFY_INTERVAL_MS) finds the file gone (deleted / renamed /
 *     folder removed) — the next line then re-creates it, as before;
 *   - closed in log_close().
 * All of it is guarded by g_logLock. Cap, debug toggle, line format and the
 * "LOG: cleared" notice are unchanged. */
#define LOG_VERIFY_INTERVAL_MS 1000ull

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static ULONGLONG g_logKnownBytes = 0;   /* size of the open file as far as we know */
static ULONGLONG g_logLastVerifyMs = 0; /* last path check / size resync */
static int g_logClosed = 0;             /* log_close ran: late lines open+close again */

static BOOL CALLBACK log_lock_init_once(PINIT_ONCE initOnce, PVOID param, PVOID* context) {
    (void)initOnce;
    (void)param;
    (void)context;
    InitializeCriticalSection(&g_logLock);
    return TRUE;
}

/* Resolve Documents\Conan Exiles TeamSpeak plugin\plugin.log (once). */
static int log_resolve_path(void) {
    if (InterlockedCompareExchange(&g_logPathReady, 0, 0)) {
        return 1;
    }

    PWSTR documentsPath = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &documentsPath))) {
        return 0;
    }

    wchar_t folder[MAX_PATH];
    swprintf(folder, MAX_PATH, L"%s\\Conan Exiles TeamSpeak plugin", documentsPath);
    CoTaskMemFree(documentsPath);
    CreateDirectoryW(folder, NULL);

    swprintf(g_logPath, MAX_PATH, L"%s\\plugin.log", folder);
    InterlockedExchange(&g_logPathReady, 1);
    return 1;
}

const wchar_t* log_get_path(void) {
    if (!log_resolve_path()) {
        return NULL;
    }
    return g_logPath;
}

static void log_file_close_locked(void) {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

/* (Re)open the log. disposition: OPEN_ALWAYS = append to / create the file,
   CREATE_ALWAYS = truncate it (size-cap reset). Caller holds g_logLock. */
static int log_file_open_locked(DWORD disposition) {
    log_file_close_locked();

    /* FILE_SHARE_* so writes succeed while the user has plugin.log open in an
       editor, and the user can still copy or delete the file while we hold it. */
    HANDLE h = CreateFileW(g_logPath,
        FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        disposition,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        size.QuadPart = 0;
    }
    g_logFile = h;
    g_logKnownBytes = (ULONGLONG)size.QuadPart;
    g_logLastVerifyMs = GetTickCount64();
    return 1;
}

/* Append len bytes through the open handle. 1 = fully written. */
static int log_file_write_locked(const char* data, int len) {
    DWORD written = 0;
    if (g_logFile == INVALID_HANDLE_VALUE || len <= 0) {
        return 0;
    }
    if (!WriteFile(g_logFile, data, (DWORD)len, &written, NULL) || written != (DWORD)len) {
        return 0;
    }
    g_logKnownBytes += written;
    return 1;
}

/* Once per LOG_VERIFY_INTERVAL_MS: is the file still where we think it is, and
   how big is it really? Handles a user deleting/renaming plugin.log or the
   whole folder while the handle is open (the next line re-creates it) and
   picks up appends from a second TS instance. The size comes from the HANDLE,
   not from the directory entry (NTFS updates that lazily for open files). */
static void log_file_verify_locked(void) {
    const ULONGLONG nowMs = GetTickCount64();
    if (g_logFile == INVALID_HANDLE_VALUE
        || nowMs - g_logLastVerifyMs < LOG_VERIFY_INTERVAL_MS) {
        return;
    }
    g_logLastVerifyMs = nowMs;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fad)) {
        log_file_close_locked(); /* name is gone: reopen (re-create) below */
        return;
    }
    LARGE_INTEGER size;
    if (GetFileSizeEx(g_logFile, &size) && size.QuadPart >= 0) {
        g_logKnownBytes = (ULONGLONG)size.QuadPart;
    }
}

static void log_write_line(const char* prefix, const char* fmt, va_list args) {
    if (!log_resolve_path()) {
        return;
    }

    InitOnceExecuteOnce(&g_logLockInit, log_lock_init_once, NULL, NULL);
    EnterCriticalSection(&g_logLock);

    char body[2048];
    va_list argsCopy;
    va_copy(argsCopy, args);
    const int bodyLen = vsnprintf(body, sizeof(body), fmt, argsCopy);
    va_end(argsCopy);
    if (bodyLen < 0) {
        LeaveCriticalSection(&g_logLock);
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2300];
    const int lineLen = snprintf(line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %s%s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        prefix ? prefix : "", body);
    if (lineLen <= 0 || lineLen >= (int)sizeof(line)) {
        LeaveCriticalSection(&g_logLock);
        return;
    }

    /* Cap depends on debug: 250 MB off, 1 GB on. */
    {
        const ULONGLONG cap = log_is_enabled() ? LOG_MAX_BYTES_DEBUG : LOG_MAX_BYTES_NORMAL;
        int rotate = 0;

        log_file_verify_locked();
        if (g_logFile == INVALID_HANDLE_VALUE
            && !log_file_open_locked(OPEN_ALWAYS)) {
            LeaveCriticalSection(&g_logLock);
            return; /* cannot open (folder gone, share violation): drop the line as before */
        }
        if (g_logKnownBytes >= cap) {
            /* Size-cap reset: truncate, then continue on the fresh file. */
            rotate = 1;
            if (!log_file_open_locked(CREATE_ALWAYS)) {
                LeaveCriticalSection(&g_logLock);
                return;
            }
        }

        if (rotate) {
            char notice[224];
            const char* capLabel = log_is_enabled() ? "1 GB" : "250 MB";
            const int noticeLen = snprintf(notice, sizeof(notice),
                "%04u-%02u-%02u %02u:%02u:%02u.%03u LOG: cleared (plugin.log exceeded %s)\r\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                capLabel);
            if (noticeLen > 0 && noticeLen < (int)sizeof(notice)) {
                log_file_write_locked(notice, noticeLen);
            }
        }
        if (!log_file_write_locked(line, lineLen)) {
            /* The handle went bad (volume removed, file revoked, ...): reopen
               once and retry; if that fails too, drop the handle and the line —
               the next line starts from a clean open. */
            if (!log_file_open_locked(OPEN_ALWAYS)
                || !log_file_write_locked(line, lineLen)) {
                log_file_close_locked();
            }
        }
        if (g_logClosed) {
            /* A line after log_close(): do not keep a handle open past the
               plugin's lifetime. */
            log_file_close_locked();
        }
    }

    LeaveCriticalSection(&g_logLock);
}

void log_set_enabled(int enabled) {
    InterlockedExchange(&g_logEnabled, enabled ? 1 : 0);
}

int log_is_enabled(void) {
    return InterlockedCompareExchange(&g_logEnabled, 0, 0) != 0;
}

void log_write(const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    log_write_line("", fmt, args);
    va_end(args);
}

void log_debug(const char* fmt, ...) {
    if (!fmt || !log_is_enabled()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    log_write_line("DBG ", fmt, args);
    va_end(args);
}

void log_close(void) {
    /* Last step of the shutdown sequence: release the persistent handle. A
       (stray) line written afterwards still works but does not keep it open. */
    InitOnceExecuteOnce(&g_logLockInit, log_lock_init_once, NULL, NULL);
    EnterCriticalSection(&g_logLock);
    g_logClosed = 1;
    log_file_close_locked();
    LeaveCriticalSection(&g_logLock);
}
