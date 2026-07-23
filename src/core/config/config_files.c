#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/config/config.h"
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

// MODULE 2: CONFIGURATION AND FILES
// EN: plugin.cfg is written ONLY by config_save() (core/config/config.c). This
//     module keeps the voice-preset file (voice_presets.cfg) and the F10 save
//     entry points, all of which route persistence through
//     plugin_ui_on_settings_saved() → config_save().
// FR: plugin.cfg est écrit UNIQUEMENT par config_save() (core/config/config.c).
//     Ce module ne gère plus que voice_presets.cfg et les points d'entrée F10,
//     qui passent par plugin_ui_on_settings_saved() → config_save().
// ============================================================================

// Save voice settings and manage channel state | Sauvegarder les paramètres vocaux et gérer l'état des canaux
//
// V8.5b single-writer: this no longer touches plugin.cfg directly. It only
// refreshes the active voice distance and routes the current F10 globals
// through plugin_ui_on_settings_saved() → config_save(), the ONE writer.
void saveVoiceSettings() {
    // Update active distance based on absolute truth | Mettre à jour la distance active selon la vérité absolue
    localVoiceData.voiceDistance = getVoiceDistanceForMode(currentVoiceMode);
    plugin_ui_on_settings_saved();
}

// Initialize voice presets with default names | Initialiser les presets avec des noms par défaut
void initializeVoicePresets(void) {
    for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
        snprintf(voicePresets[i].name, PRESET_NAME_MAX_LENGTH, "Save %d", i + 1);
        voicePresets[i].whisperDistance = 2.0f;
        voicePresets[i].normalDistance = 10.0f;
        voicePresets[i].shoutDistance = 15.0f;
        voicePresets[i].whisperKey = 97;      // num 1
        voicePresets[i].normalKey = 98;       // num 2
        voicePresets[i].shoutKey = 99;        // num 3
        voicePresets[i].voiceToggleKey = 84;  // T
        voicePresets[i].isUsed = FALSE;
    }

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Voice presets initialized with default values and keyboard shortcuts");
    }
}

// Save current voice ranges to preset | Sauvegarder les portées vocales actuelles dans un preset
void saveVoicePreset(int presetIndex, const char* presetName) {
    if (presetIndex < 0 || presetIndex >= MAX_VOICE_PRESETS) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Invalid preset index for save");
        }
        return;
    }

    // Save current distances and keyboard shortcuts | Sauvegarder les distances actuelles et les touches clavier
    voicePresets[presetIndex].whisperDistance = distanceWhisper;
    voicePresets[presetIndex].normalDistance = distanceNormal;
    voicePresets[presetIndex].shoutDistance = distanceShout;
    voicePresets[presetIndex].whisperKey = whisperKey;
    voicePresets[presetIndex].normalKey = normalKey;
    voicePresets[presetIndex].shoutKey = shoutKey;
    voicePresets[presetIndex].voiceToggleKey = voiceToggleKey;
    voicePresets[presetIndex].isUsed = TRUE;

    // Update name if provided | Mettre à jour le nom si fourni
    if (presetName && strlen(presetName) > 0) {
        strncpy_s(voicePresets[presetIndex].name, PRESET_NAME_MAX_LENGTH, presetName, _TRUNCATE);
    }

    currentPresetIndex = presetIndex;

    // Save to config file | Sauvegarder dans le fichier de configuration
    savePresetsToConfigFile();

    // Update interface labels | Mettre à jour les labels d'interface
    if (hConfigDialog && IsWindow(hConfigDialog)) {
        updatePresetLabels();
    }
   
    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Voice preset saved: [%d] '%s' - Whisper:%.1f Normal:%.1f Shout:%.1f",
            presetIndex, voicePresets[presetIndex].name,
            distanceWhisper, distanceNormal, distanceShout);
        mumbleAPI.log(ownID, logMsg);
    }

    // Show confirmation | Afficher confirmation
    char confirmMsg[256];
    snprintf(confirmMsg, sizeof(confirmMsg),
        "✅ Preset saved: '%s'", voicePresets[presetIndex].name);
    displayInChat(confirmMsg);
}

// Load voice ranges from preset | Charger les portées vocales depuis un preset
// Load voice ranges from preset | Charger les portées vocales depuis un preset
void loadVoicePreset(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= MAX_VOICE_PRESETS) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Invalid preset index for load");
        }
        return;
    }

    if (!voicePresets[presetIndex].isUsed) {
        if (enableLogConfig) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Preset %d is empty - nothing to load", presetIndex);
            mumbleAPI.log(ownID, logMsg);
        }

        MessageBoxW(hConfigDialog, L"This preset slot is empty.", L"Empty Preset", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Load distances and keyboard shortcuts from preset | Charger les distances et les touches clavier depuis le preset
    distanceWhisper = voicePresets[presetIndex].whisperDistance;
    distanceNormal = voicePresets[presetIndex].normalDistance;
    distanceShout = voicePresets[presetIndex].shoutDistance;
    whisperKey = voicePresets[presetIndex].whisperKey;
    normalKey = voicePresets[presetIndex].normalKey;
    shoutKey = voicePresets[presetIndex].shoutKey;
    voiceToggleKey = voicePresets[presetIndex].voiceToggleKey;

#ifdef CONAN_EXILES_TS_EXPORTS
    voice_mode_reset_key_tracking();
#endif

    currentPresetIndex = presetIndex;

    // Update interface if open | Mettre à jour l'interface si ouverte
    if (hConfigDialog && IsWindow(hConfigDialog)) {
        isUpdatingInterface = TRUE;

        wchar_t whisperText[32], normalText[32], shoutText[32];
        swprintf(whisperText, 32, L"%.1f", distanceWhisper);
        swprintf(normalText, 32, L"%.1f", distanceNormal);
        swprintf(shoutText, 32, L"%.1f", distanceShout);

        if (hDistanceWhisperEdit) SetWindowTextW(hDistanceWhisperEdit, whisperText);
        if (hDistanceNormalEdit) SetWindowTextW(hDistanceNormalEdit, normalText);
        if (hDistanceShoutEdit) SetWindowTextW(hDistanceShoutEdit, shoutText);

        // Update keyboard shortcut displays in interface | Mettre à jour l'affichage des touches clavier dans l'interface
        if (hWhisperKeyEdit) SetWindowTextA(hWhisperKeyEdit, getKeyName(whisperKey));
        if (hNormalKeyEdit) SetWindowTextA(hNormalKeyEdit, getKeyName(normalKey));
        if (hShoutKeyEdit) SetWindowTextA(hShoutKeyEdit, getKeyName(shoutKey));
        if (hVoiceToggleKeyEdit) SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(voiceToggleKey));

        isUpdatingInterface = FALSE;

        updateDynamicInterface();
    }

    // Update active distance based on absolute truth | Mettre à jour la distance active selon la vérité absolue
    localVoiceData.voiceDistance = getVoiceDistanceForMode(currentVoiceMode);

    wchar_t gameFolder[MAX_PATH] = L"";
    {
        PluginConfig cfg;
        const wchar_t* savedBase = NULL;

        config_copy(&cfg);
        if (cfg.automaticPatchFind && cfg.automaticSavedPath[0]) {
            savedBase = cfg.automaticSavedPath;
        }
        else if (cfg.savedPath[0]) {
            savedBase = cfg.savedPath;
        }
        if (savedBase) {
            wcsncpy_s(gameFolder, MAX_PATH, savedBase, _TRUNCATE);
            wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
            if (conanSandbox) {
                *conanSandbox = L'\0';
            }
        }
    }

    wchar_t distWhisper[32], distNormal[32], distShout[32];
    swprintf(distWhisper, 32, L"%.1f", distanceWhisper);
    swprintf(distNormal, 32, L"%.1f", distanceNormal);
    swprintf(distShout, 32, L"%.1f", distanceShout);

    writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

    // Apply changes | Appliquer les changements
    if (enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Voice preset loaded and saved: [%d] '%s' - Whisper:%.1f Normal:%.1f Shout:%.1f Keys: W=%d N=%d S=%d T=%d",
            presetIndex, voicePresets[presetIndex].name,
            distanceWhisper, distanceNormal, distanceShout,
            whisperKey, normalKey, shoutKey, voiceToggleKey);
        mumbleAPI.log(ownID, logMsg);
    }

    // Show confirmation message | Afficher message de confirmation
    char confirmMsg[256];
    snprintf(confirmMsg, sizeof(confirmMsg),
        "✅ Preset loaded and saved: '%s'", voicePresets[presetIndex].name);
    displayInChat(confirmMsg);
}

// Rename voice preset | Renommer un preset vocal
BOOL renameVoicePreset(int presetIndex, const char* newName) {
    if (presetIndex < 0 || presetIndex >= MAX_VOICE_PRESETS) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Invalid preset index for rename");
        }
        return FALSE;
    }

    if (!newName || strlen(newName) == 0) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Empty name provided for rename");
        }
        return FALSE;
    }

    // ✅ VÉRIFIER LA LONGUEUR (10 caractères max)
    if (strlen(newName) > 15) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Name too long (max 10 characters)");
        }
        return FALSE;
    }

    char oldName[PRESET_NAME_MAX_LENGTH];
    strncpy_s(oldName, PRESET_NAME_MAX_LENGTH, voicePresets[presetIndex].name, _TRUNCATE);

    // Update name | Mettre à jour le nom
    strncpy_s(voicePresets[presetIndex].name, PRESET_NAME_MAX_LENGTH, newName, _TRUNCATE);

    // Save to config | Sauvegarder dans la configuration
    savePresetsToConfigFile();

    // ✅ MISE À JOUR IMMÉDIATE DE L'INTERFACE (sans fermer/rouvrir)
    if (hConfigDialog && IsWindow(hConfigDialog)) {
        // Forcer la mise à jour des labels de presets
        updatePresetLabels();

        // Forcer le redessin de la zone des presets
        RECT presetArea;
        presetArea.left = 40;
        presetArea.top = 200;
        presetArea.right = 560;
        presetArea.bottom = 600;
        InvalidateRect(hConfigDialog, &presetArea, TRUE);
        UpdateWindow(hConfigDialog);
    }

    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Preset renamed: [%d] '%s' -> '%s'",
            presetIndex, oldName, newName);
        mumbleAPI.log(ownID, logMsg);
    }

    return TRUE;
}

// Save presets to configuration file | Sauvegarder les presets dans le fichier de configuration
void savePresetsToConfigFile(void) {
    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) return;

    wchar_t presetFile[MAX_PATH];
    swprintf(presetFile, MAX_PATH, L"%s\\voice_presets.cfg", configFolder);

    FILE* file = NULL;
    errno_t err = _wfopen_s(&file, presetFile, L"w");
    if (err != 0 || !file) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Failed to create voice presets config file");
        }
        return;
    }

    // Write current preset index | Écrire l'index du preset actuel
    fwprintf(file, L"CurrentPreset=%d\n\n", currentPresetIndex);

    // Write all presets | Écrire tous les presets
    for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
        wchar_t wName[PRESET_NAME_MAX_LENGTH];
        size_t converted = 0;
        mbstowcs_s(&converted, wName, PRESET_NAME_MAX_LENGTH, voicePresets[i].name, _TRUNCATE);

        fwprintf(file, L"[Preset%d]\n", i);
        fwprintf(file, L"Name=%s\n", wName);
        fwprintf(file, L"Whisper=%.1f\n", voicePresets[i].whisperDistance);
        fwprintf(file, L"Normal=%.1f\n", voicePresets[i].normalDistance);
        fwprintf(file, L"Shout=%.1f\n", voicePresets[i].shoutDistance);
        fwprintf(file, L"WhisperKey=%d\n", voicePresets[i].whisperKey);
        fwprintf(file, L"NormalKey=%d\n", voicePresets[i].normalKey);
        fwprintf(file, L"ShoutKey=%d\n", voicePresets[i].shoutKey);
        fwprintf(file, L"VoiceToggleKey=%d\n", voicePresets[i].voiceToggleKey);
        fwprintf(file, L"IsUsed=%s\n\n", voicePresets[i].isUsed ? L"true" : L"false");
    }

    fclose(file);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Voice presets saved to config file");
    }
}

// Load presets from configuration file | Charger les presets depuis le fichier de configuration
void loadPresetsFromConfigFile(void) {
    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) return;

    wchar_t presetFile[MAX_PATH];
    swprintf(presetFile, MAX_PATH, L"%s\\voice_presets.cfg", configFolder);

    FILE* file = NULL;
    errno_t err = _wfopen_s(&file, presetFile, L"r");
    if (err != 0 || !file) {
        // File doesn't exist, initialize with defaults | Fichier inexistant, initialiser avec valeurs par défaut
        initializeVoicePresets();
        return;
    }

    wchar_t line[512];
    int currentPreset = -1;

    while (fgetws(line, 512, file)) {
        // Remove trailing newline | Supprimer le retour à la ligne
        wchar_t* nl = wcschr(line, L'\n');
        if (nl) *nl = L'\0';
        wchar_t* cr = wcschr(line, L'\r');
        if (cr) *cr = L'\0';

        // Parse current preset index | Parser l'index du preset actuel
        if (wcsncmp(line, L"CurrentPreset=", 14) == 0) {
            currentPresetIndex = _wtoi(line + 14);
        }
        // Parse preset section | Parser la section preset
        else if (wcsncmp(line, L"[Preset", 7) == 0) {
            wchar_t* endBracket = wcschr(line, L']');
            if (endBracket) {
                *endBracket = L'\0';
                currentPreset = _wtoi(line + 7);
            }
        }
        // Parse preset properties | Parser les propriétés du preset
        else if (currentPreset >= 0 && currentPreset < MAX_VOICE_PRESETS) {
            if (wcsncmp(line, L"Name=", 5) == 0) {
                size_t converted = 0;
                wcstombs_s(&converted, voicePresets[currentPreset].name, PRESET_NAME_MAX_LENGTH, line + 5, _TRUNCATE);
            }
            else if (wcsncmp(line, L"Whisper=", 8) == 0) {
                voicePresets[currentPreset].whisperDistance = (float)_wtof(line + 8);
            }
            else if (wcsncmp(line, L"Normal=", 7) == 0) {
                voicePresets[currentPreset].normalDistance = (float)_wtof(line + 7);
            }
            else if (wcsncmp(line, L"Shout=", 6) == 0) {
                voicePresets[currentPreset].shoutDistance = (float)_wtof(line + 6);
            }
            else if (wcsncmp(line, L"WhisperKey=", 11) == 0) {
                voicePresets[currentPreset].whisperKey = _wtoi(line + 11);
            }
            else if (wcsncmp(line, L"NormalKey=", 10) == 0) {
                voicePresets[currentPreset].normalKey = _wtoi(line + 10);
            }
            else if (wcsncmp(line, L"ShoutKey=", 9) == 0) {
                voicePresets[currentPreset].shoutKey = _wtoi(line + 9);
            }
            else if (wcsncmp(line, L"VoiceToggleKey=", 15) == 0) {
                voicePresets[currentPreset].voiceToggleKey = _wtoi(line + 15);
            }
            else if (wcsncmp(line, L"IsUsed=", 7) == 0) {
                voicePresets[currentPreset].isUsed = (wcscmp(line + 7, L"true") == 0);
            }
        }
    }

    fclose(file);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Voice presets loaded from config file");
    }
}

// Persist the active Saved path + distances | Enregistrer le chemin Saved actif + distances
//
// V8.5b single-writer: no direct plugin.cfg write anymore. It computes the
// active Saved path, updates the F10 globals, then routes everything through
// plugin_ui_on_settings_saved() → config_save() (the ONE writer). config_save
// preserves SavedPath / AutomaticSavedPath automatically (config_copy keeps the
// unchanged field), so the old "read existing path before overwrite" dance is
// no longer needed.
void writeFullConfiguration(const wchar_t* gameFolder, const wchar_t* distWhisper, const wchar_t* distNormal, const wchar_t* distShout) {
    // Construire le chemin COMPLET (avec \ConanSandbox\Saved)
    wchar_t savedPathFull[MAX_PATH];

    /* Caller-supplied gameFolder wins over stale F10 label text (preset load,
       background autodetect). displayedPathText is only a UI mirror. */
    if (gameFolder && wcslen(gameFolder) > 0) {
        swprintf(savedPathFull, MAX_PATH, L"%s\\ConanSandbox\\Saved", gameFolder);
    }
    else if (wcslen(displayedPathText) > 0) {
        swprintf(savedPathFull, MAX_PATH, L"%s\\ConanSandbox\\Saved", displayedPathText);
    }
    else {
        PluginConfig cfg;
        const wchar_t* savedBase = NULL;

        config_copy(&cfg);
        if (cfg.automaticPatchFind && cfg.automaticSavedPath[0]) {
            savedBase = cfg.automaticSavedPath;
        }
        else if (cfg.savedPath[0]) {
            savedBase = cfg.savedPath;
        }
        if (savedBase) {
            wcsncpy_s(savedPathFull, MAX_PATH, savedBase, _TRUNCATE);
        }
        else {
            wcscpy_s(savedPathFull, MAX_PATH, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Conan Exiles\\ConanSandbox\\Saved");
        }
    }

    // Save current voice mode before modifying distances | Sauvegarder le mode de voix actuel AVANT de modifier les distances
    float currentVoiceDistance = localVoiceData.voiceDistance;

    // Update user distances WITHOUT clamping | Mettre à jour les distances SANS limites
    distanceWhisper = (float)_wtof(distWhisper);
    distanceNormal = (float)_wtof(distNormal);
    distanceShout = (float)_wtof(distShout);

    // Update mod file path | Mettre à jour le chemin du fichier mod
    size_t converted = 0;
    char modFilePathTemp[MAX_PATH] = "";
    wcstombs_s(&converted, modFilePathTemp, MAX_PATH, savedPathFull, _TRUNCATE);
    snprintf(modFilePath, MAX_PATH, "%s\\Pos.txt", modFilePathTemp);

    /* Active-path global: the compat bridge routes it into the matching cfg
       field (AutomaticSavedPath when auto mode, SavedPath otherwise). */
    wcscpy_s(savedPath, MAX_PATH, savedPathFull);

    // Restore current voice mode instead of forcing Normal | Restaurer le mode de voix actuel au lieu de forcer Normal
    if (fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceNormal) &&
        fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceShout)) {
        localVoiceData.voiceDistance = distanceWhisper;
    }
    else if (fabsf(currentVoiceDistance - distanceShout) < fabsf(currentVoiceDistance - distanceNormal)) {
        localVoiceData.voiceDistance = distanceShout;
    }
    else {
        localVoiceData.voiceDistance = distanceNormal;
    }

    /* Single writer: push globals into g_config and rewrite plugin.cfg
       canonically via config_save(). */
    plugin_ui_on_settings_saved();

    // Reinstall keyboard monitoring | Réinstaller la surveillance du clavier
    removeKeyMonitoring();
    installKeyMonitoring();
}

// ============================================================================
