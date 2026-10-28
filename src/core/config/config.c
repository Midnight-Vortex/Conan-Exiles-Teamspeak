#include "core/config/config.h"
#include "core/config/config_text.h"
#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

PluginConfig g_config;

/* Guards whole-struct writes vs. snapshot reads (string fields). */
static CRITICAL_SECTION g_cfgLock;
static INIT_ONCE g_cfgLockOnce = INIT_ONCE_STATIC_INIT;

/* Serializes config_save(): one writer at a time, newest snapshot wins.
   Always taken BEFORE g_cfgLock (config_save -> config_copy), never the
   other way round. */
static CRITICAL_SECTION g_cfgSaveLock;
static INIT_ONCE g_cfgSaveLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cfg_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cfgLock);
    return TRUE;
}

static BOOL CALLBACK cfg_save_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cfgSaveLock);
    return TRUE;
}

static void cfg_lock_ensure(void) {
    InitOnceExecuteOnce(&g_cfgLockOnce, cfg_lock_init_once, NULL, NULL);
}

static void cfg_save_lock_ensure(void) {
    InitOnceExecuteOnce(&g_cfgSaveLockOnce, cfg_save_lock_init_once, NULL, NULL);
}

void config_apply(const PluginConfig* src) {
    if (!src) {
        return;
    }
    cfg_lock_ensure();
    EnterCriticalSection(&g_cfgLock);
    g_config = *src;
    LeaveCriticalSection(&g_cfgLock);
}

void config_copy(PluginConfig* out) {
    if (!out) {
        return;
    }
    cfg_lock_ensure();
    EnterCriticalSection(&g_cfgLock);
    *out = g_config;
    LeaveCriticalSection(&g_cfgLock);
}

void config_modify(ConfigMutator fn, void* ctx) {
    if (!fn) {
        return;
    }
    cfg_lock_ensure();
    EnterCriticalSection(&g_cfgLock);
    fn(&g_config, ctx);
    config_clamp(&g_config);
    LeaveCriticalSection(&g_cfgLock);
}

const wchar_t* config_get_folder_path(void) {
    static wchar_t s_configPath[CONFIG_MAX_PATH];
    static volatile long s_ready = 0;

    if (InterlockedCompareExchange(&s_ready, 0, 0)) {
        return s_configPath;
    }

    PWSTR documentsPath = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &documentsPath))) {
        return NULL;
    }

    swprintf(s_configPath, CONFIG_MAX_PATH, L"%s\\Conan Exiles TeamSpeak plugin", documentsPath);
    CoTaskMemFree(documentsPath);
    CreateDirectoryW(s_configPath, NULL);
    InterlockedExchange(&s_ready, 1);
    return s_configPath;
}

static void config_set_defaults(PluginConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->automaticPatchFind = 1;
    cfg->distanceWhisper = CFG_DEFAULT_DIST_WHISPER;
    cfg->distanceNormal = CFG_DEFAULT_DIST_NORMAL;
    cfg->distanceShout = CFG_DEFAULT_DIST_SHOUT;
    cfg->whisperKey = 97;      /* Num1 */
    cfg->normalKey = 98;       /* Num2 */
    cfg->shoutKey = 99;        /* Num3 */
    cfg->voiceToggleKey = 84;  /* T */
    cfg->configUIKey = 121;    /* F10 */
    cfg->enableDistanceMuting = 1;
    cfg->enableAutomaticChannelChange = 1;
    cfg->enableVoiceToggle = 1;
    cfg->enableVoiceOverlay = 1;
    cfg->hudTheme = 0;
    cfg->hudPosition = 0;
    cfg->hudSize = 0;
    cfg->debugMode = 0;
    cfg->enablePosFile = 1;
}

static int config_file_path(wchar_t* out, size_t outLen) {
    const wchar_t* folder = config_get_folder_path();
    if (!folder) {
        return 0;
    }
    swprintf(out, outLen, L"%s\\plugin.cfg", folder);
    return 1;
}

/* Clamp numeric values into safe ranges after load (NaN/Inf -> default). */
void config_clamp(PluginConfig* cfg) {
    cfg_clamp_values(cfg);
}

/* ---- file IO ------------------------------------------------------------------ */

#define CFG_FILE_MAX_BYTES (256u * 1024u) /* plugin.cfg is ~1 KB; a runaway file is cut here */

/* Read the whole file into a malloc'd buffer (not NUL-terminated).  Returns 1
   on success.  *notFound = 1 when the file simply does not exist; any other
   failure (sharing violation that outlasts the retries, ...) leaves it 0. */
static int cfg_read_file(const wchar_t* path, unsigned char** outBytes, size_t* outLen, int* notFound) {
    HANDLE h = INVALID_HANDLE_VALUE;
    int attempt;

    *outBytes = NULL;
    *outLen = 0;
    *notFound = 0;

    for (attempt = 0; attempt < 4; attempt++) {
        h = CreateFileW(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            break;
        }
        {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                *notFound = 1;
                return 0;
            }
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED
                && err != ERROR_LOCK_VIOLATION) {
                return 0;
            }
        }
        Sleep(5 + 10 * (DWORD)attempt);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    {
        unsigned char* data = (unsigned char*)malloc(CFG_FILE_MAX_BYTES);
        size_t used = 0;
        int ok = (data != NULL);

        while (ok && used < CFG_FILE_MAX_BYTES) {
            DWORD got = 0;
            if (!ReadFile(h, data + used, (DWORD)(CFG_FILE_MAX_BYTES - used), &got, NULL)) {
                ok = 0;
                break;
            }
            if (got == 0) {
                break;
            }
            used += got;
        }
        CloseHandle(h);
        if (!ok) {
            free(data);
            return 0;
        }
        *outBytes = data;
        *outLen = used;
        return 1;
    }
}

/* Raw file bytes -> NUL-terminated wide text (malloc'd; NULL on failure).
   A UTF-8 BOM is skipped; valid UTF-8 is decoded as UTF-8, anything else as
   the ANSI code page (files written by older plugin versions). */
static wchar_t* cfg_decode_text(const unsigned char* bytes, size_t len) {
    wchar_t* out;
    int n;
    UINT cp;
    DWORD flags;

    if (len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bytes += 3;
        len -= 3;
    }
    if (len == 0) {
        out = (wchar_t*)malloc(sizeof(wchar_t));
        if (out) {
            out[0] = L'\0';
        }
        return out;
    }
    if (len > (size_t)INT_MAX) {
        return NULL;
    }

    if (cfg_utf8_is_valid(bytes, len)) {
        cp = CP_UTF8;
        flags = MB_ERR_INVALID_CHARS;
    }
    else {
        cp = CP_ACP;
        flags = 0;
    }
    n = MultiByteToWideChar(cp, flags, (LPCSTR)bytes, (int)len, NULL, 0);
    if (n <= 0 && cp == CP_UTF8) {
        cp = CP_ACP; /* the OS disagrees about "valid" - fall back to ANSI */
        flags = 0;
        n = MultiByteToWideChar(cp, flags, (LPCSTR)bytes, (int)len, NULL, 0);
    }
    if (n <= 0) {
        return NULL;
    }
    out = (wchar_t*)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!out) {
        return NULL;
    }
    if (MultiByteToWideChar(cp, flags, (LPCSTR)bytes, (int)len, out, n) != n) {
        free(out);
        return NULL;
    }
    out[n] = L'\0';
    return out;
}

int config_load(void) {
    PluginConfig cfg;
    config_set_defaults(&cfg);

    wchar_t path[CONFIG_MAX_PATH];
    if (!config_file_path(path, CONFIG_MAX_PATH)) {
        config_apply(&cfg);
        log_write("CONFIG: config folder unavailable");
        return 0;
    }

    unsigned char* bytes = NULL;
    size_t byteLen = 0;
    int notFound = 0;
    if (!cfg_read_file(path, &bytes, &byteLen, &notFound)) {
        config_apply(&cfg);
        if (notFound) {
            /* First run — persist defaults so the user has a file to edit. */
            log_write("CONFIG: plugin.cfg missing, creating defaults");
            return config_save();
        }
        /* Exists but cannot be read right now: run with defaults in memory and
           do NOT overwrite the user's file with them. */
        log_write("CONFIG: plugin.cfg unreadable (err=%lu) - using defaults, file left untouched",
            (unsigned long)GetLastError());
        return 0;
    }

    wchar_t* text = cfg_decode_text(bytes, byteLen);
    free(bytes);
    if (text) {
        cfg_parse_text(&cfg, text);
        free(text);
    }
    else {
        log_write("CONFIG: plugin.cfg could not be decoded - using defaults");
    }

    config_clamp(&cfg);
    config_apply(&cfg);
    log_set_enabled(g_config.debugMode);
    log_write("CONFIG: loaded (whisper=%.1f normal=%.1f shout=%.1f debug=%d posFile=%d)",
        g_config.distanceWhisper, g_config.distanceNormal, g_config.distanceShout,
        g_config.debugMode, g_config.enablePosFile);
    return 1;
}

/* Wide path -> bytes in the given code page.  Returns the length without NUL,
   or -1 when it does not fit / is not representable (ANSI only). */
static int cfg_encode_path(const wchar_t* w, UINT cp, char* out, int cap) {
    BOOL usedDefault = FALSE;
    int n;

    if (w[0] == L'\0') {
        out[0] = '\0';
        return 0;
    }
    if (cp == CP_UTF8) {
        n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    }
    else {
        n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w, -1, out, cap, NULL, &usedDefault);
        if (usedDefault) {
            return -1;
        }
    }
    return (n > 0) ? n - 1 : -1;
}

/* plugin.cfg content for cfg.  Returns the byte count (-1 on failure).  ASCII
   paths give the same bytes the plugin always wrote.  Non-ASCII paths are
   stored in the ANSI code page when that is lossless and unambiguous (every
   older reader keeps working), otherwise as UTF-8 without BOM.  CRLF line
   ends like the old text-mode writer. */
static int cfg_serialize(const PluginConfig* cfg, char* out, size_t cap) {
    char savedAnsi[CONFIG_MAX_PATH * 4 + 4];
    char autoAnsi[CONFIG_MAX_PATH * 4 + 4];
    char savedUtf8[CONFIG_MAX_PATH * 4 + 4];
    char autoUtf8[CONFIG_MAX_PATH * 4 + 4];
    const char* saved;
    const char* autoP;
    int ls, la, lsU, laU;
    int n;

    lsU = cfg_encode_path(cfg->savedPath, CP_UTF8, savedUtf8, (int)sizeof(savedUtf8));
    laU = cfg_encode_path(cfg->automaticSavedPath, CP_UTF8, autoUtf8, (int)sizeof(autoUtf8));
    if (lsU < 0 || laU < 0) {
        return -1;
    }
    saved = savedUtf8;
    autoP = autoUtf8;

    if (cfg_has_non_ascii((const unsigned char*)savedUtf8, (size_t)lsU)
        || cfg_has_non_ascii((const unsigned char*)autoUtf8, (size_t)laU)) {
        ls = cfg_encode_path(cfg->savedPath, CP_ACP, savedAnsi, (int)sizeof(savedAnsi));
        la = cfg_encode_path(cfg->automaticSavedPath, CP_ACP, autoAnsi, (int)sizeof(autoAnsi));
        if (ls >= 0 && la >= 0
            && cfg_ansi_bytes_are_unambiguous((const unsigned char*)savedAnsi, (size_t)ls)
            && cfg_ansi_bytes_are_unambiguous((const unsigned char*)autoAnsi, (size_t)la)) {
            saved = savedAnsi;
            autoP = autoAnsi;
        }
    }

    n = snprintf(out, cap,
        "SavedPath=%s\r\n"
        "AutomaticSavedPath=%s\r\n"
        "AutomaticPatchFind=%s\r\n"
        "DistanceWhisper=%.1f\r\n"
        "DistanceNormal=%.1f\r\n"
        "DistanceShout=%.1f\r\n"
        "WhisperKey=%d\r\n"
        "NormalKey=%d\r\n"
        "ShoutKey=%d\r\n"
        "VoiceToggleKey=%d\r\n"
        "ConfigUIKey=%d\r\n"
        "EnableDistanceMuting=%s\r\n"
        "EnableAutomaticChannelChange=%s\r\n"
        "EnableVoiceToggle=%s\r\n"
        "EnableVoiceOverlay=%s\r\n"
        "HudTheme=%d\r\n"
        "HudPosition=%d\r\n"
        "HudSize=%d\r\n"
        "DebugMode=%s\r\n"
        "EnablePosFile=%s\r\n"
        "DefaultsAppliedServer=%s\r\n",
        saved, autoP,
        cfg->automaticPatchFind ? "true" : "false",
        cfg->distanceWhisper, cfg->distanceNormal, cfg->distanceShout,
        cfg->whisperKey, cfg->normalKey, cfg->shoutKey,
        cfg->voiceToggleKey, cfg->configUIKey,
        cfg->enableDistanceMuting ? "true" : "false",
        cfg->enableAutomaticChannelChange ? "true" : "false",
        cfg->enableVoiceToggle ? "true" : "false",
        cfg->enableVoiceOverlay ? "true" : "false",
        cfg->hudTheme, cfg->hudPosition, cfg->hudSize,
        cfg->debugMode ? "true" : "false",
        cfg->enablePosFile ? "true" : "false",
        cfg->defaultsAppliedServer);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* Write data to tmp, flush, then swap it over path.  The old plugin.cfg stays
   untouched until the new file is complete on disk.  1 on success. */
static int cfg_write_atomic(const wchar_t* path, const wchar_t* tmp, const char* data, DWORD len, DWORD* outErr) {
    HANDLE h;
    DWORD written = 0;
    BOOL ok;
    int attempt;

    *outErr = 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *outErr = GetLastError();
        return 0;
    }
    ok = WriteFile(h, data, len, &written, NULL) && written == len;
    if (!ok) {
        *outErr = GetLastError();
    }
    if (ok && !FlushFileBuffers(h)) {
        *outErr = GetLastError();
        ok = FALSE;
    }
    if (!CloseHandle(h) && ok) {
        *outErr = GetLastError();
        ok = FALSE;
    }
    if (!ok) {
        DeleteFileW(tmp);
        return 0;
    }

    /* A reader (settings dialog, antivirus) may hold plugin.cfg open for a few
       ms without FILE_SHARE_DELETE - retry the swap shortly. */
    for (attempt = 0; attempt < 5; attempt++) {
        if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return 1;
        }
        *outErr = GetLastError();
        if (*outErr != ERROR_ACCESS_DENIED && *outErr != ERROR_SHARING_VIOLATION) {
            break;
        }
        Sleep(10u << attempt);
    }
    DeleteFileW(tmp);
    return 0;
}

int config_save(void) {
    wchar_t path[CONFIG_MAX_PATH];
    wchar_t tmp[CONFIG_MAX_PATH + 8];
    char data[4096];
    PluginConfig cfg;
    DWORD err = 0;
    int len;
    int ok = 0;

    if (!config_file_path(path, CONFIG_MAX_PATH)) {
        return 0;
    }
    swprintf(tmp, CONFIG_MAX_PATH + 8, L"%ls.tmp", path);

    /* Snapshot INSIDE the save lock: of two racing saves the later one always
       carries the newer state and finishes last. */
    cfg_save_lock_ensure();
    EnterCriticalSection(&g_cfgSaveLock);
    config_copy(&cfg);
    len = cfg_serialize(&cfg, data, sizeof(data));
    if (len > 0) {
        ok = cfg_write_atomic(path, tmp, data, (DWORD)len, &err);
    }
    LeaveCriticalSection(&g_cfgSaveLock);

    if (!ok) {
        log_write("CONFIG: failed to write plugin.cfg (%s, err=%lu) - old file kept",
            len > 0 ? "write" : "format", (unsigned long)err);
        return 0;
    }
    log_debug("CONFIG: saved plugin.cfg");
    return 1;
}
