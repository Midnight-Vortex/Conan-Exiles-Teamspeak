#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/proximity/proximity_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <process.h>
#include <ole2.h>
#if defined(_MSC_VER)
#pragma warning(disable : 4456) /* legacy UI reuses block-scoped names */
#endif
#include <uxtheme.h>
#if defined(_MSC_VER)
#pragma comment(lib, "uxtheme.lib")
#endif
#include "ui_config_internal.h"
#include "ui/config/ui_config_state.h"

/*
 * ui_main.c: default-settings load/save, key capture.
 * WM_COMMAND lives in ui_config_command.c (V8.10). UI thread only.
 */

// Key capture processing | Traitement de la capture de touches
void processKeyCapture() {
    if (!isCapturingKey) return;

    BOOL keyFound = FALSE;

    for (int vk = 1; vk < 256; vk++) {
        if (vk == 27) continue; // Skip ESC | Ignorer Echap
        if (GetAsyncKeyState(vk) & 0x8000) {
            // Touche détectée | Key detected
            switch (captureKeyTarget) {
            case 1: ui_cfg()->whisperKey = vk; if (hWhisperKeyEdit) SetWindowTextA(hWhisperKeyEdit, getKeyName(vk)); break;
            case 2: ui_cfg()->normalKey = vk; if (hNormalKeyEdit) SetWindowTextA(hNormalKeyEdit, getKeyName(vk)); break;
            case 3: ui_cfg()->shoutKey = vk; if (hShoutKeyEdit) SetWindowTextA(hShoutKeyEdit, getKeyName(vk)); break;
            case 4: ui_cfg()->configUIKey = vk; if (hConfigKeyEdit) SetWindowTextA(hConfigKeyEdit, getKeyName(vk)); break;
            case 5: ui_cfg()->voiceToggleKey = vk; if (hVoiceToggleKeyEdit) SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(vk)); break;
            }

            isCapturingKey = FALSE;
            captureKeyTarget = 0;
            keyFound = TRUE;

            // Réactiver TOUS les boutons immédiatement | Re-enable ALL buttons immediately
            if (hWhisperButton) EnableWindow(hWhisperButton, TRUE);
            if (hNormalButton) EnableWindow(hNormalButton, TRUE);
            if (hShoutButton) EnableWindow(hShoutButton, TRUE);
            if (hConfigButton) EnableWindow(hConfigButton, TRUE);
            if (hVoiceToggleButton) EnableWindow(hVoiceToggleButton, TRUE);

            break;
        }
    }

    // ✅ CORRECTION : Ne pas attendre si aucune touche trouvée
    // Cette condition évite le blocage sur 200ms si l'utilisateur relâche avant la détection
    if (keyFound) {
        Sleep(100); // Attendre le relâchement | Wait for key release
    }
}

// Load default settings from config file | Charger les paramètres par défaut depuis le fichier de config
void loadDefaultSettingsFromConfig() {
    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\default_settings.cfg", configFolder);

    FILE* f = _wfopen(configFile, L"r");
    if (!f) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "Default settings file not found - will be created on first connection");
        }
        return;
    }

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') ++p;
        wchar_t* end = p + wcslen(p);
        while (end > p && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' ' || end[-1] == L'\t'))
            *--end = L'\0';

        if (*p == L'#' || *p == L';' || *p == L'\0') continue;

        wchar_t* eq = wcschr(p, L'=');
        if (!eq) continue;
        *eq = L'\0';
        wchar_t* key = p;
        wchar_t* val = eq + 1;
        while (*val == L' ' || *val == L'\t') ++val;

        if (wcsncmp(key, L"ServerConfigHash", 16) == 0) {
            size_t converted = 0;
            wcstombs_s(&converted, serverConfigHash, sizeof(serverConfigHash), val, _TRUNCATE);
        }
        else if (wcsncmp(key, L"HasAppliedDefaultSettings", 25) == 0) {
            hasAppliedDefaultSettings = (wcsncmp(val, L"true", 4) == 0);
        }
        else if (wcsncmp(key, L"DefaultWhisperKey", 17) == 0) {
            defaultWhisperKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultNormalKey", 16) == 0) {
            defaultNormalKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultShoutKey", 15) == 0) {
            defaultShoutKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultVoiceToggleKey", 21) == 0) {
            defaultVoiceToggleKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceWhisper", 22) == 0) {
            defaultDistanceWhisper = (float)_wtof(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceNormal", 21) == 0) {
            defaultDistanceNormal = (float)_wtof(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceShout", 20) == 0) {
            defaultDistanceShout = (float)_wtof(val);
        }
    }

    fclose(f);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Default settings loaded from config file");
    }
}

// Save default settings to config file ONLY if feature is enabled | Sauvegarder les paramètres par défaut UNIQUEMENT si la fonctionnalité est activée
void saveDefaultSettingsToConfig() {
    // Ne sauvegarder que si la fonctionnalité est activée | Only save if feature is enabled
    if (!enableDefaultSettingsOnFirstConnection) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "Default settings NOT saved - feature disabled (enableDefaultSettingsOnFirstConnection=false)");
        }
        return;
    }

    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\default_settings.cfg", configFolder);

    FILE* f = _wfopen(configFile, L"w");
    if (!f) return;

    fwprintf(f, L"# Default Settings Configuration | Configuration des paramètres par défaut\n");
    fwprintf(f, L"# This file tracks server configuration hash and default settings applied\n\n");

    fwprintf(f, L"ServerConfigHash=%S\n", serverConfigHash);
    fwprintf(f, L"HasAppliedDefaultSettings=%s\n", hasAppliedDefaultSettings ? L"true" : L"false");
    fwprintf(f, L"\n");

    fwprintf(f, L"# Default suggested keys | Touches par défaut suggérées\n");
    fwprintf(f, L"DefaultWhisperKey=%d\n", defaultWhisperKey);
    fwprintf(f, L"DefaultNormalKey=%d\n", defaultNormalKey);
    fwprintf(f, L"DefaultShoutKey=%d\n", defaultShoutKey);
    fwprintf(f, L"DefaultVoiceToggleKey=%d\n", defaultVoiceToggleKey);
    fwprintf(f, L"\n");

    fwprintf(f, L"# Default suggested distances (meters) | Distances par défaut suggérées (mètres)\n");
    fwprintf(f, L"DefaultDistanceWhisper=%.1f\n", defaultDistanceWhisper);
    fwprintf(f, L"DefaultDistanceNormal=%.1f\n", defaultDistanceNormal);
    fwprintf(f, L"DefaultDistanceShout=%.1f\n", defaultDistanceShout);

    fclose(f);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Default settings saved to config file");
    }
}
