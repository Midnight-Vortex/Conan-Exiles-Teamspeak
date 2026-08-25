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

#define LOG_MAX_BYTES (100ull * 1024ull * 1024ull)

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

static ULONGLONG log_file_size_bytes(void) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER sz;

    if (!GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fad)) {
        return 0;
    }
    sz.LowPart = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    return sz.QuadPart;
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

    /* FILE_SHARE_* so writes succeed while the user has plugin.log open in an editor.
       Truncate at 100 MB so debug/rate spam cannot grow the file without bound. */
    {
        const int rotate = (log_file_size_bytes() >= LOG_MAX_BYTES);
        HANDLE h = CreateFileW(g_logPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            rotate ? CREATE_ALWAYS : OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (rotate) {
                char notice[192];
                const int noticeLen = snprintf(notice, sizeof(notice),
                    "%04u-%02u-%02u %02u:%02u:%02u.%03u LOG: cleared (plugin.log exceeded 100 MB)\r\n",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                if (noticeLen > 0 && noticeLen < (int)sizeof(notice)) {
                    WriteFile(h, notice, (DWORD)noticeLen, &written, NULL);
                }
            }
            WriteFile(h, line, (DWORD)lineLen, &written, NULL);
            CloseHandle(h);
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
    /* Files are opened per write — nothing to close. Kept for the shutdown
       sequence so the call site documents the log lifecycle end. */
}
