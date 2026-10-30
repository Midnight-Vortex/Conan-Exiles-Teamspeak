#include "plugin_internal.h"
#include "ui/config/ui_config_state.h"
#include "ui/config/ui_config_logic.h"
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

// MODULE: VOICE PRESETS + F10 SAVE ENTRY POINTS (ui/config/ui_voice_presets.c)
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
    voicePresets[presetIndex].whisperDistance = ui_cfg()->distanceWhisper;
    voicePresets[presetIndex].normalDistance = ui_cfg()->distanceNormal;
    voicePresets[presetIndex].shoutDistance = ui_cfg()->distanceShout;
    voicePresets[presetIndex].whisperKey = ui_cfg()->whisperKey;
    voicePresets[presetIndex].normalKey = ui_cfg()->normalKey;
    voicePresets[presetIndex].shoutKey = ui_cfg()->shoutKey;
    voicePresets[presetIndex].voiceToggleKey = ui_cfg()->voiceToggleKey;
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
            ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout);
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
    ui_cfg()->distanceWhisper = voicePresets[presetIndex].whisperDistance;
    ui_cfg()->distanceNormal = voicePresets[presetIndex].normalDistance;
    ui_cfg()->distanceShout = voicePresets[presetIndex].shoutDistance;
    ui_cfg()->whisperKey = voicePresets[presetIndex].whisperKey;
    ui_cfg()->normalKey = voicePresets[presetIndex].normalKey;
    ui_cfg()->shoutKey = voicePresets[presetIndex].shoutKey;
    ui_cfg()->voiceToggleKey = voicePresets[presetIndex].voiceToggleKey;

#ifdef CONAN_EXILES_TS_EXPORTS
    voice_mode_reset_key_tracking();
#endif

    currentPresetIndex = presetIndex;

    // Update interface if open | Mettre à jour l'interface si ouverte
    if (hConfigDialog && IsWindow(hConfigDialog)) {
        isUpdatingInterface = TRUE;

        // 091: the preset values are meters (now in the working copy); the edits show the display unit.
        wchar_t whisperText[32], normalText[32], shoutText[32];
        ui_range_text(whisperText, 32, ui_cfg()->distanceWhisper);
        ui_range_text(normalText, 32, ui_cfg()->distanceNormal);
        ui_range_text(shoutText, 32, ui_cfg()->distanceShout);

        if (hDistanceWhisperEdit) SetWindowTextW(hDistanceWhisperEdit, whisperText);
        if (hDistanceNormalEdit) SetWindowTextW(hDistanceNormalEdit, normalText);
        if (hDistanceShoutEdit) SetWindowTextW(hDistanceShoutEdit, shoutText);

        // Update keyboard shortcut displays in interface | Mettre à jour l'affichage des touches clavier dans l'interface
        if (hWhisperKeyEdit) SetWindowTextA(hWhisperKeyEdit, getKeyName(ui_cfg()->whisperKey));
        if (hNormalKeyEdit) SetWindowTextA(hNormalKeyEdit, getKeyName(ui_cfg()->normalKey));
        if (hShoutKeyEdit) SetWindowTextA(hShoutKeyEdit, getKeyName(ui_cfg()->shoutKey));
        if (hVoiceToggleKeyEdit) SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(ui_cfg()->voiceToggleKey));

        isUpdatingInterface = FALSE;

        updateDynamicInterface();
    }

    // Update active distance based on absolute truth | Mettre à jour la distance active selon la vérité absolue
    localVoiceData.voiceDistance = getVoiceDistanceForMode(currentVoiceMode);

    wchar_t gameFolder[MAX_PATH] = L"";
    ui_cfg_get_active_saved_path(gameFolder, MAX_PATH);
    if (gameFolder[0]) {
        wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
        if (conanSandbox) {
            *conanSandbox = L'\0';
        }
    }

    // writeFullConfiguration takes METERS as text (not the edit texts) - unchanged by 091.
    // 091: meters as hand-off text; exact in foundation mode (see ui_range_transport_text).
    wchar_t distWhisper[32], distNormal[32], distShout[32];
    ui_range_transport_text(distWhisper, 32, ui_cfg()->distanceWhisper);
    ui_range_transport_text(distNormal, 32, ui_cfg()->distanceNormal);
    ui_range_transport_text(distShout, 32, ui_cfg()->distanceShout);

    writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

    // Apply changes | Appliquer les changements
    if (ui_cfg()->enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Voice preset loaded and saved: [%d] '%s' - Whisper:%.1f Normal:%.1f Shout:%.1f Keys: W=%d N=%d S=%d T=%d",
            presetIndex, voicePresets[presetIndex].name,
            ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout,
            ui_cfg()->whisperKey, ui_cfg()->normalKey, ui_cfg()->shoutKey, ui_cfg()->voiceToggleKey);
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
//
// 087: the file is written to "voice_presets.cfg.tmp" first and only then moved over the
// real file (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH). It used to be opened with
// "w" directly, which truncates the file at once: a failed/half write (disk full, crash,
// TeamSpeak killed) lost ALL presets, and the fclose/ferror results were never checked.
// The file format is unchanged.
// 087 : ecriture dans un .tmp puis remplacement atomique ; erreurs verifiees.
void savePresetsToConfigFile(void) {
    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) return;

    wchar_t presetFile[MAX_PATH];
    wchar_t tmpFile[MAX_PATH];
    if (swprintf(presetFile, MAX_PATH, L"%s\\voice_presets.cfg", configFolder) < 0
        || swprintf(tmpFile, MAX_PATH, L"%s\\voice_presets.cfg.tmp", configFolder) < 0) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Voice presets config path too long");
        }
        return;
    }

    FILE* file = NULL;
    errno_t err = _wfopen_s(&file, tmpFile, L"w");
    if (err != 0 || !file) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Failed to create voice presets config file");
        }
        return;
    }

    int writeFailed = 0;

    // Write current preset index | Écrire l'index du preset actuel
    if (fwprintf(file, L"CurrentPreset=%d\n\n", currentPresetIndex) < 0) writeFailed = 1;

    // Write all presets | Écrire tous les presets
    for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
        wchar_t wName[PRESET_NAME_MAX_LENGTH];
        size_t converted = 0;
        mbstowcs_s(&converted, wName, PRESET_NAME_MAX_LENGTH, voicePresets[i].name, _TRUNCATE);

        if (fwprintf(file, L"[Preset%d]\n", i) < 0) writeFailed = 1;
        if (fwprintf(file, L"Name=%s\n", wName) < 0) writeFailed = 1;
        if (fwprintf(file, L"Whisper=%.1f\n", voicePresets[i].whisperDistance) < 0) writeFailed = 1;
        if (fwprintf(file, L"Normal=%.1f\n", voicePresets[i].normalDistance) < 0) writeFailed = 1;
        if (fwprintf(file, L"Shout=%.1f\n", voicePresets[i].shoutDistance) < 0) writeFailed = 1;
        if (fwprintf(file, L"WhisperKey=%d\n", voicePresets[i].whisperKey) < 0) writeFailed = 1;
        if (fwprintf(file, L"NormalKey=%d\n", voicePresets[i].normalKey) < 0) writeFailed = 1;
        if (fwprintf(file, L"ShoutKey=%d\n", voicePresets[i].shoutKey) < 0) writeFailed = 1;
        if (fwprintf(file, L"VoiceToggleKey=%d\n", voicePresets[i].voiceToggleKey) < 0) writeFailed = 1;
        if (fwprintf(file, L"IsUsed=%s\n\n", voicePresets[i].isUsed ? L"true" : L"false") < 0) writeFailed = 1;
    }

    if (ferror(file)) writeFailed = 1;
    if (fflush(file) != 0) writeFailed = 1;
    if (fclose(file) != 0) writeFailed = 1;

    if (writeFailed) {
        DeleteFileW(tmpFile);
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Writing voice presets failed - previous file kept");
        }
        return;
    }

    if (!MoveFileExW(tmpFile, presetFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpFile);
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: Replacing voice presets file failed - previous file kept");
        }
        return;
    }

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Voice presets saved to config file");
    }
}

// Load presets from configuration file | Charger les presets depuis le fichier de configuration
//
// 087: the file is user-editable text and is validated while it is read: a malformed
// "[PresetX]" header ignores the whole section (it used to become preset 0 and overwrite
// it), the index must be in range, distances must be finite numbers (clamped to
// 0.5..500 m, same as config_clamp) and keys must be real virtual-key codes (1..255).
// Anything invalid keeps the value the preset already had (the defaults).
// 087 : le fichier est valide a la lecture (en-tete, index, distances, touches).
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
            currentPresetIndex = ui_preset_parse_current(line + 14, MAX_VOICE_PRESETS);
        }
        // Parse preset section | Parser la section preset
        else if (wcsncmp(line, L"[Preset", 7) == 0) {
            // Invalid header -> currentPreset = -1: the lines below are ignored.
            if (!ui_preset_parse_header(line, MAX_VOICE_PRESETS, &currentPreset)) {
                currentPreset = -1;
                if (enableLogConfig) {
                    mumbleAPI.log(ownID, "WARNING: voice_presets.cfg - invalid [PresetN] header ignored");
                }
            }
        }
        // Parse preset properties | Parser les propriétés du preset
        else if (currentPreset >= 0 && currentPreset < MAX_VOICE_PRESETS) {
            VoiceRangePreset* preset = &voicePresets[currentPreset];
            if (wcsncmp(line, L"Name=", 5) == 0) {
                size_t converted = 0;
                wcstombs_s(&converted, preset->name, PRESET_NAME_MAX_LENGTH, line + 5, _TRUNCATE);
            }
            else if (wcsncmp(line, L"Whisper=", 8) == 0) {
                ui_preset_parse_distance(line + 8, &preset->whisperDistance);
            }
            else if (wcsncmp(line, L"Normal=", 7) == 0) {
                ui_preset_parse_distance(line + 7, &preset->normalDistance);
            }
            else if (wcsncmp(line, L"Shout=", 6) == 0) {
                ui_preset_parse_distance(line + 6, &preset->shoutDistance);
            }
            else if (wcsncmp(line, L"WhisperKey=", 11) == 0) {
                ui_preset_parse_key(line + 11, &preset->whisperKey);
            }
            else if (wcsncmp(line, L"NormalKey=", 10) == 0) {
                ui_preset_parse_key(line + 10, &preset->normalKey);
            }
            else if (wcsncmp(line, L"ShoutKey=", 9) == 0) {
                ui_preset_parse_key(line + 9, &preset->shoutKey);
            }
            else if (wcsncmp(line, L"VoiceToggleKey=", 15) == 0) {
                ui_preset_parse_key(line + 15, &preset->voiceToggleKey);
            }
            else if (wcsncmp(line, L"IsUsed=", 7) == 0) {
                preset->isUsed = (wcscmp(line + 7, L"true") == 0);
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
    int pathOk = 1;

    /* Caller-supplied gameFolder wins over stale F10 label text (preset load,
       background autodetect). displayedPathText is only a UI mirror.
       087: a folder that leaves no room for "\ConanSandbox\Saved" is NOT stored
       (a truncated path would be wrong); the callers show a message for it. */
    if (gameFolder && wcslen(gameFolder) > 0) {
        if (ui_game_folder_fits(gameFolder, MAX_PATH)) {
            swprintf(savedPathFull, MAX_PATH, L"%s%s", gameFolder, UI_SAVED_SUBPATH);
        }
        else {
            pathOk = 0;
        }
    }
    else if (wcslen(displayedPathText) > 0) {
        if (ui_game_folder_fits(displayedPathText, MAX_PATH)) {
            swprintf(savedPathFull, MAX_PATH, L"%s%s", displayedPathText, UI_SAVED_SUBPATH);
        }
        else {
            pathOk = 0;
        }
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
            ui_wcs_copy(savedPathFull, MAX_PATH, savedBase);
        }
        else {
            wcscpy_s(savedPathFull, MAX_PATH, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Conan Exiles\\ConanSandbox\\Saved");
        }
    }
    if (!pathOk) {
        savedPathFull[0] = L'\0';
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "ERROR: writeFullConfiguration - game folder too long, path not stored");
        }
    }

    // Save current voice mode before modifying distances | Sauvegarder le mode de voix actuel AVANT de modifier les distances
    float currentVoiceDistance = localVoiceData.voiceDistance;

    // Update user distances WITHOUT clamping | Mettre à jour les distances SANS limites
    // 087: strict parse — text that is not a finite number > 0 leaves the working-copy
    // value untouched (_wtof turned it into 0, which config_clamp then made 0.5 m).
    {
        float parsed = 0.0f;
        if (ui_dist_parse(distWhisper, 0, 0.0f, 0.0f, &parsed) == UI_DIST_OK) ui_cfg()->distanceWhisper = parsed;
        if (ui_dist_parse(distNormal, 0, 0.0f, 0.0f, &parsed) == UI_DIST_OK) ui_cfg()->distanceNormal = parsed;
        if (ui_dist_parse(distShout, 0, 0.0f, 0.0f, &parsed) == UI_DIST_OK) ui_cfg()->distanceShout = parsed;
    }

    if (pathOk) {
        // Update mod file path | Mettre à jour le chemin du fichier mod
        size_t converted = 0;
        char modFilePathTemp[MAX_PATH] = "";
        wcstombs_s(&converted, modFilePathTemp, MAX_PATH, savedPathFull, _TRUNCATE);
        snprintf(modFilePath, MAX_PATH, "%s\\Pos.txt", modFilePathTemp);

        /* Active-path global: the compat bridge routes it into the matching cfg
           field (AutomaticSavedPath when auto mode, SavedPath otherwise). */
        ui_cfg_set_active_saved_path_full(savedPathFull);
    }

    // Restore current voice mode instead of forcing Normal | Restaurer le mode de voix actuel au lieu de forcer Normal
    if (fabsf(currentVoiceDistance - ui_cfg()->distanceWhisper) < fabsf(currentVoiceDistance - ui_cfg()->distanceNormal) &&
        fabsf(currentVoiceDistance - ui_cfg()->distanceWhisper) < fabsf(currentVoiceDistance - ui_cfg()->distanceShout)) {
        localVoiceData.voiceDistance = ui_cfg()->distanceWhisper;
    }
    else if (fabsf(currentVoiceDistance - ui_cfg()->distanceShout) < fabsf(currentVoiceDistance - ui_cfg()->distanceNormal)) {
        localVoiceData.voiceDistance = ui_cfg()->distanceShout;
    }
    else {
        localVoiceData.voiceDistance = ui_cfg()->distanceNormal;
    }

    /* Single writer: push globals into g_config and rewrite plugin.cfg
       canonically via config_save(). */
    plugin_ui_on_settings_saved();

    /* 087: the old "removeKeyMonitoring(); installKeyMonitoring();" pair was removed.
       It stopped and restarted the hotkey thread on the dialog thread at every Save,
       preset load and dialog open — racing with plugin shutdown (the hotkey thread could
       be started again after shutdown had joined it) and blocking up to 5 s in the
       join. It was never needed: key_watcher.c re-reads g_config.configUIKey on every
       loop and voice_mode_hotkey_poll() takes a config_copy() each tick, so new hotkeys
       apply by themselves.
       087 : la paire remove/installKeyMonitoring a ete supprimee (course avec l'arret
       du plugin) ; les nouvelles touches sont relues a chaque tour par le thread. */
}

// ============================================================================
