#include "core/config/config.h"
#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <wctype.h>

PluginConfig g_config;

/* Guards whole-struct writes vs. snapshot reads (string fields). */
static CRITICAL_SECTION g_cfgLock;
static INIT_ONCE g_cfgLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cfg_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cfgLock);
    return TRUE;
}

static void cfg_lock_ensure(void) {
    InitOnceExecuteOnce(&g_cfgLockOnce, cfg_lock_init_once, NULL, NULL);
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
    cfg->distanceWhisper = 3.0f;
    cfg->distanceNormal = 13.0f;
    cfg->distanceShout = 26.0f;
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

static int parse_bool(const wchar_t* val) {
    return (_wcsicmp(val, L"true") == 0 || wcscmp(val, L"1") == 0) ? 1 : 0;
}

/* Trim leading/trailing whitespace in place, returns start pointer. */
static wchar_t* trim(wchar_t* s) {
    while (*s == L' ' || *s == L'\t') {
        s++;
    }
    wchar_t* end = s + wcslen(s);
    while (end > s && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' ' || end[-1] == L'\t')) {
        *--end = L'\0';
    }
    return s;
}

static void config_apply_line(PluginConfig* cfg, wchar_t* line) {
    wchar_t* p = trim(line);
    if (*p == L'#' || *p == L';' || *p == L'\0') {
        return;
    }

    wchar_t* eq = wcschr(p, L'=');
    if (!eq) {
        return;
    }
    *eq = L'\0';
    wchar_t* key = trim(p);
    wchar_t* val = trim(eq + 1);

    if (_wcsicmp(key, L"SavedPath") == 0) {
        wcsncpy_s(cfg->savedPath, CONFIG_MAX_PATH, val, _TRUNCATE);
    }
    else if (_wcsicmp(key, L"AutomaticSavedPath") == 0) {
        wcsncpy_s(cfg->automaticSavedPath, CONFIG_MAX_PATH, val, _TRUNCATE);
    }
    else if (_wcsicmp(key, L"AutomaticPatchFind") == 0) {
        cfg->automaticPatchFind = parse_bool(val);
    }
    else if (_wcsicmp(key, L"DistanceWhisper") == 0) {
        cfg->distanceWhisper = (float)_wtof(val);
    }
    else if (_wcsicmp(key, L"DistanceNormal") == 0) {
        cfg->distanceNormal = (float)_wtof(val);
    }
    else if (_wcsicmp(key, L"DistanceShout") == 0) {
        cfg->distanceShout = (float)_wtof(val);
    }
    else if (_wcsicmp(key, L"WhisperKey") == 0) {
        cfg->whisperKey = _wtoi(val);
    }
    else if (_wcsicmp(key, L"NormalKey") == 0) {
        cfg->normalKey = _wtoi(val);
    }
    else if (_wcsicmp(key, L"ShoutKey") == 0) {
        cfg->shoutKey = _wtoi(val);
    }
    else if (_wcsicmp(key, L"VoiceToggleKey") == 0) {
        cfg->voiceToggleKey = _wtoi(val);
    }
    else if (_wcsicmp(key, L"ConfigUIKey") == 0) {
        cfg->configUIKey = _wtoi(val);
    }
    else if (_wcsicmp(key, L"EnableDistanceMuting") == 0) {
        cfg->enableDistanceMuting = parse_bool(val);
    }
    else if (_wcsicmp(key, L"EnableAutomaticChannelChange") == 0) {
        cfg->enableAutomaticChannelChange = parse_bool(val);
    }
    else if (_wcsicmp(key, L"EnableVoiceToggle") == 0) {
        cfg->enableVoiceToggle = parse_bool(val);
    }
    else if (_wcsicmp(key, L"EnableVoiceOverlay") == 0) {
        cfg->enableVoiceOverlay = parse_bool(val);
    }
    else if (_wcsicmp(key, L"HudTheme") == 0) {
        cfg->hudTheme = _wtoi(val);
    }
    else if (_wcsicmp(key, L"HudPosition") == 0) {
        cfg->hudPosition = _wtoi(val);
    }
    else if (_wcsicmp(key, L"HudSize") == 0) {
        cfg->hudSize = _wtoi(val);
    }
    else if (_wcsicmp(key, L"DebugMode") == 0) {
        cfg->debugMode = parse_bool(val);
    }
    else if (_wcsicmp(key, L"EnablePosFile") == 0) {
        cfg->enablePosFile = parse_bool(val);
    }
    else if (_wcsicmp(key, L"DefaultsAppliedServer") == 0) {
        /* Server UID is plain ASCII (base64 alphabet). */
        size_t j = 0;
        for (const wchar_t* src = val; *src && j + 1 < sizeof(cfg->defaultsAppliedServer); src++) {
            if (*src < 128) {
                cfg->defaultsAppliedServer[j++] = (char)*src;
            }
        }
        cfg->defaultsAppliedServer[j] = '\0';
    }
}

/* Clamp numeric values into safe ranges after load. */
void config_clamp(PluginConfig* cfg) {
    if (cfg->distanceWhisper < 0.5f) cfg->distanceWhisper = 0.5f;
    if (cfg->distanceWhisper > 500.0f) cfg->distanceWhisper = 500.0f;
    if (cfg->distanceNormal < 0.5f) cfg->distanceNormal = 0.5f;
    if (cfg->distanceNormal > 500.0f) cfg->distanceNormal = 500.0f;
    if (cfg->distanceShout < 0.5f) cfg->distanceShout = 0.5f;
    if (cfg->distanceShout > 500.0f) cfg->distanceShout = 500.0f;

    if (cfg->whisperKey <= 0 || cfg->whisperKey >= 256) cfg->whisperKey = 97;
    if (cfg->normalKey <= 0 || cfg->normalKey >= 256) cfg->normalKey = 98;
    if (cfg->shoutKey <= 0 || cfg->shoutKey >= 256) cfg->shoutKey = 99;
    if (cfg->voiceToggleKey <= 0 || cfg->voiceToggleKey >= 256) cfg->voiceToggleKey = 84;
    if (cfg->configUIKey <= 0 || cfg->configUIKey >= 256) cfg->configUIKey = 121;

    if (cfg->hudTheme < 0) cfg->hudTheme = 0;
    if (cfg->hudTheme > 11) cfg->hudTheme = 11;
    if (cfg->hudPosition < 0) cfg->hudPosition = 0;
    if (cfg->hudPosition > 4) cfg->hudPosition = 4;
    if (cfg->hudSize < 0) cfg->hudSize = 0;
    if (cfg->hudSize > 2) cfg->hudSize = 2;
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

    FILE* f = _wfopen(path, L"r");
    if (!f) {
        /* First run — persist defaults so the user has a file to edit. */
        config_apply(&cfg);
        log_write("CONFIG: plugin.cfg missing, creating defaults");
        return config_save();
    }

    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        config_apply_line(&cfg, line);
    }
    fclose(f);

    config_clamp(&cfg);
    config_apply(&cfg);
    log_set_enabled(g_config.debugMode);
    log_write("CONFIG: loaded (whisper=%.1f normal=%.1f shout=%.1f debug=%d posFile=%d)",
        g_config.distanceWhisper, g_config.distanceNormal, g_config.distanceShout,
        g_config.debugMode, g_config.enablePosFile);
    return 1;
}

int config_save(void) {
    wchar_t path[CONFIG_MAX_PATH];
    if (!config_file_path(path, CONFIG_MAX_PATH)) {
        return 0;
    }

    PluginConfig cfg;
    config_copy(&cfg);

    FILE* f = _wfopen(path, L"w");
    if (!f) {
        log_write("CONFIG: failed to write plugin.cfg");
        return 0;
    }

    fwprintf(f, L"SavedPath=%s\n", cfg.savedPath);
    fwprintf(f, L"AutomaticSavedPath=%s\n", cfg.automaticSavedPath);
    fwprintf(f, L"AutomaticPatchFind=%s\n", cfg.automaticPatchFind ? L"true" : L"false");
    fwprintf(f, L"DistanceWhisper=%.1f\n", cfg.distanceWhisper);
    fwprintf(f, L"DistanceNormal=%.1f\n", cfg.distanceNormal);
    fwprintf(f, L"DistanceShout=%.1f\n", cfg.distanceShout);
    fwprintf(f, L"WhisperKey=%d\n", cfg.whisperKey);
    fwprintf(f, L"NormalKey=%d\n", cfg.normalKey);
    fwprintf(f, L"ShoutKey=%d\n", cfg.shoutKey);
    fwprintf(f, L"VoiceToggleKey=%d\n", cfg.voiceToggleKey);
    fwprintf(f, L"ConfigUIKey=%d\n", cfg.configUIKey);
    fwprintf(f, L"EnableDistanceMuting=%s\n", cfg.enableDistanceMuting ? L"true" : L"false");
    fwprintf(f, L"EnableAutomaticChannelChange=%s\n", cfg.enableAutomaticChannelChange ? L"true" : L"false");
    fwprintf(f, L"EnableVoiceToggle=%s\n", cfg.enableVoiceToggle ? L"true" : L"false");
    fwprintf(f, L"EnableVoiceOverlay=%s\n", cfg.enableVoiceOverlay ? L"true" : L"false");
    fwprintf(f, L"HudTheme=%d\n", cfg.hudTheme);
    fwprintf(f, L"HudPosition=%d\n", cfg.hudPosition);
    fwprintf(f, L"HudSize=%d\n", cfg.hudSize);
    fwprintf(f, L"DebugMode=%s\n", cfg.debugMode ? L"true" : L"false");
    fwprintf(f, L"EnablePosFile=%s\n", cfg.enablePosFile ? L"true" : L"false");
    fwprintf(f, L"DefaultsAppliedServer=%hs\n", cfg.defaultsAppliedServer);
    fclose(f);

    log_debug("CONFIG: saved plugin.cfg");
    return 1;
}
