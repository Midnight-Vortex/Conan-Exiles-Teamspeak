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

// MODULE 2: CONFIGURATION AND FILES
// EN: plugin.cfg read/write, voice presets, distance settings, Pos.txt path persistence.
// FR: Lecture/écriture plugin.cfg, presets voix, réglages distance, persistance chemin Pos.txt.
// ============================================================================
void loadVoiceDistancesFromConfig() {
    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);

    FILE* f = _wfopen(configFile, L"r");
    if (f) {
        wchar_t line[1024];
        while (fgetws(line, 1024, f)) {
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

            if (wcsncmp(key, L"DistanceWhisper", 15) == 0) {
                distanceWhisper = (float)_wtof(val);
            }
            else if (wcsncmp(key, L"DistanceNormal", 14) == 0) {
                distanceNormal = (float)_wtof(val);
            }
            else if (wcsncmp(key, L"DistanceShout", 13) == 0) {
                distanceShout = (float)_wtof(val);
            }
        }
        fclose(f);
    }
}

void readConfigurationSettings() {
    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);

    BOOL fileExists = (GetFileAttributesW(configFile) != INVALID_FILE_ATTRIBUTES);
    BOOL foundSavedPath = FALSE;
    BOOL foundEnableAutomaticChannelChange = FALSE;
    BOOL foundDistanceWhisper = FALSE;
    BOOL foundDistanceNormal = FALSE;
    BOOL foundDistanceShout = FALSE;
    BOOL foundAutomaticPatchFind = FALSE;

    wchar_t savedPathValue[MAX_PATH] = L"";

    if (fileExists) {
        FILE* f = _wfopen(configFile, L"r");
        if (f) {
            wchar_t line[1024];
            while (fgetws(line, 1024, f)) {
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

                wchar_t valLower[16] = { 0 };
                int i = 0;
                for (; i < 15 && val[i]; ++i) valLower[i] = (wchar_t)towlower(val[i]);
                valLower[i] = 0;

                if (wcsncmp(key, L"SavedPath", 9) == 0) {
                    wcsncpy_s(savedPathValue, MAX_PATH, val, _TRUNCATE);
                    foundSavedPath = TRUE;
                }
                else if (wcsncmp(key, L"EnableAutomaticChannelChange", 28) == 0) {
                    enableAutomaticChannelChange = (wcscmp(valLower, L"true") == 0 || wcscmp(valLower, L"1") == 0);
                    foundEnableAutomaticChannelChange = TRUE;
                }
                else if (wcsncmp(key, L"WhisperKey", 10) == 0) {
                    whisperKey = _wtoi(val);
                }
                else if (wcsncmp(key, L"NormalKey", 9) == 0) {
                    normalKey = _wtoi(val);
                }
                else if (wcsncmp(key, L"ShoutKey", 8) == 0) {
                    shoutKey = _wtoi(val);
                }
                else if (wcsncmp(key, L"ConfigUIKey", 11) == 0) {
                    configUIKey = _wtoi(val);
                }
                else if (wcsncmp(key, L"EnableDistanceMuting", 20) == 0) {
                    enableDistanceMuting = (wcscmp(valLower, L"true") == 0 || wcscmp(valLower, L"1") == 0);
                }
                else if (wcsncmp(key, L"DistanceWhisper", 15) == 0) {
                    distanceWhisper = (float)_wtof(val);
                    foundDistanceWhisper = TRUE;
                }
                else if (wcsncmp(key, L"DistanceNormal", 14) == 0) {
                    distanceNormal = (float)_wtof(val);
                    foundDistanceNormal = TRUE;
                }
                else if (wcsncmp(key, L"DistanceShout", 13) == 0) {
                    distanceShout = (float)_wtof(val);
                    foundDistanceShout = TRUE;
                }
                else if (wcsncmp(key, L"VoiceToggleKey", 14) == 0) {
                    voiceToggleKey = _wtoi(val);
                }
                else if (wcsncmp(key, L"EnableVoiceToggle", 17) == 0) {
                    enableVoiceToggle = (wcscmp(valLower, L"true") == 0 || wcscmp(valLower, L"1") == 0);
                }
                else if (wcsncmp(key, L"HudTheme", 8) == 0) {
                    /* Voice overlay palette index (0=gray .. 11=lime), from plugin.cfg
                       Index de palette overlay (0=gris .. 11=citron vert), depuis plugin.cfg */
                    int theme = _wtoi(val);
                    if (theme < 0) {
                        theme = VOICE_HUD_THEME_GRAY;
                    }
                    if (theme >= VOICE_HUD_THEME_COUNT) {
                        theme = VOICE_HUD_THEME_COUNT - 1;
                    }
                    voiceHudTheme = theme;
                }
                else if (wcsncmp(key, L"HudPosition", 11) == 0) {
                    /* Voice overlay screen position (0=top-left .. 3=top-center), from plugin.cfg
                       Position à l'écran du HUD (0=haut-gauche .. 3=haut-centre), depuis plugin.cfg */
                    int position = _wtoi(val);
                    if (position < 0) {
                        position = VOICE_HUD_POSITION_BOTTOM_RIGHT;
                    }
                    if (position >= VOICE_HUD_POSITION_COUNT) {
                        position = VOICE_HUD_POSITION_COUNT - 1;
                    }
                    voiceHudPosition = position;
                }
                else if (wcsncmp(key, L"HudSize", 7) == 0) {
                    int hudSize = _wtoi(val);
                    if (hudSize < 0) {
                        hudSize = VOICE_HUD_SIZE_BIG;
                    }
                    if (hudSize >= VOICE_HUD_SIZE_COUNT) {
                        hudSize = VOICE_HUD_SIZE_COUNT - 1;
                    }
                    voiceHudSize = hudSize;
                }
                else if (wcsncmp(key, L"AutomaticPatchFind", 18) == 0) {
                    enableAutomaticPatchFind = (wcscmp(valLower, L"true") == 0 || wcscmp(valLower, L"1") == 0);
                    foundAutomaticPatchFind = TRUE;
                }

            }
            fclose(f);
        }
    }
    // If file doesn't exist or critical settings are missing, create or update it | Si le fichier n'existe pas ou si des paramètres critiques manquent, le créer ou le mettre à jour
    if (!fileExists || !foundSavedPath || !foundEnableAutomaticChannelChange || !foundAutomaticPatchFind) {
        FILE* f = _wfopen(configFile, L"w");
        if (f) {
            fwprintf(f, L"SavedPath=%s\n", foundSavedPath ? savedPathValue : L"");
            fwprintf(f, L"AutomaticSavedPath=\n");
            fwprintf(f, L"AutomaticPatchFind=%s\n", TRUE ? L"true" : L"false");
            fwprintf(f, L"AutomaticSavedPath=\n");
            fwprintf(f, L"EnableAutomaticChannelChange=%s\n", enableAutomaticChannelChange ? L"true" : L"false");
            fwprintf(f, L"WhisperKey=%d\n", whisperKey);
            fwprintf(f, L"NormalKey=%d\n", normalKey);
            fwprintf(f, L"ShoutKey=%d\n", shoutKey);
            fwprintf(f, L"ConfigUIKey=%d\n", configUIKey);
            fwprintf(f, L"EnableDistanceMuting=%s\n", enableDistanceMuting ? L"true" : L"false");
            fwprintf(f, L"DistanceWhisper=%.1f\n", foundDistanceWhisper ? distanceWhisper : 3.0f);
            fwprintf(f, L"DistanceNormal=%.1f\n", foundDistanceNormal ? distanceNormal : 13.0f);
            fwprintf(f, L"DistanceShout=%.1f\n", foundDistanceShout ? distanceShout : 26.0f);
            fwprintf(f, L"VoiceToggleKey=%d\n", voiceToggleKey);
            fwprintf(f, L"EnableVoiceToggle=%s\n", enableVoiceToggle ? L"true" : L"false");
            fclose(f);
        }
        // ✅ CORRECTION CRITIQUE : Définir enableAutomaticPatchFind à TRUE lors de la création initiale
        enableAutomaticPatchFind = TRUE;
    }

    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Configuration loaded - Whisper: %.1fm, Normal: %.1fm, Shout: %.1fm, AutomaticPatchFind: %s",
            distanceWhisper, distanceNormal, distanceShout, enableAutomaticPatchFind ? "TRUE" : "FALSE");
        mumbleAPI.log(ownID, logMsg);
    }
}

// Helper function to save specific parameter | Fonction pour sauvegarder un paramètre spécifique
void saveConfigurationChange(const char* key, const wchar_t* value) {
    // Call complete function to save everything at once | Appeler la fonction complète pour tout sauvegarder d'un coup
    saveVoiceSettings();

    if (TEMP) {
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), "SAVED CONFIG CHANGE: %s = %ls", key, value);
        mumbleAPI.log(ownID, logMsg);
    }
}

// Save voice settings and manage channel state | Sauvegarder les paramètres vocaux et gérer l'état des canaux
void saveVoiceSettings() {
    // Get configuration folder path | Obtenir le chemin du dossier de configuration
    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) return;

    // Store current voice distance before modifications | Stocker la distance vocale actuelle avant modifications
    float currentVoiceDistance = localVoiceData.voiceDistance;

    // Build configuration file path | Construire le chemin du fichier de configuration
    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);

    // Arrays to store configuration lines | Tableaux pour stocker les lignes de configuration
    wchar_t (*lines)[1024] = (wchar_t(*)[1024])calloc(100, sizeof(*lines));
    if (!lines) {
        // Allocation échouée, sortir proprement
        return;
    }
    int lineCount = 0;
    BOOL foundWhisper = FALSE, foundNormal = FALSE, foundShout = FALSE;
    BOOL foundDistanceMuting = FALSE, foundChannelChange = FALSE;
    BOOL foundHudTheme = FALSE;
    BOOL foundHudPosition = FALSE;
    BOOL foundHudSize = FALSE;

    // Read existing configuration file | Lire le fichier de configuration existant
    FILE* f = NULL;
    errno_t err = _wfopen_s(&f, configFile, L"r");
    if (err == 0 && f) {
        while (fgetws(lines[lineCount], 1024, f) && lineCount < 99) {
            // Update whisper distance line | Mettre à jour la ligne de distance whisper
            if (wcsncmp(lines[lineCount], L"DistanceWhisper=", 16) == 0) {
                swprintf(lines[lineCount], 1024, L"DistanceWhisper=%.1f\n", distanceWhisper);
                foundWhisper = TRUE;
            }
            // Update normal distance line | Mettre à jour la ligne de distance normale
            else if (wcsncmp(lines[lineCount], L"DistanceNormal=", 15) == 0) {
                swprintf(lines[lineCount], 1024, L"DistanceNormal=%.1f\n", distanceNormal);
                foundNormal = TRUE;
            }
            // Update shout distance line | Mettre à jour la ligne de distance shout
            else if (wcsncmp(lines[lineCount], L"DistanceShout=", 14) == 0) {
                swprintf(lines[lineCount], 1024, L"DistanceShout=%.1f\n", distanceShout);
                foundShout = TRUE;
            }
            // Update distance muting setting | Mettre à jour le paramètre de muting par distance
            else if (wcsncmp(lines[lineCount], L"EnableDistanceMuting=", 21) == 0) {
                swprintf(lines[lineCount], 1024, L"EnableDistanceMuting=%s\n", enableDistanceMuting ? L"true" : L"false");
                foundDistanceMuting = TRUE;
            }
            // Update automatic channel change setting | Mettre à jour le paramètre de changement automatique de canal
            else if (wcsncmp(lines[lineCount], L"EnableAutomaticChannelChange=", 29) == 0) {
                swprintf(lines[lineCount], 1024, L"EnableAutomaticChannelChange=%s\n", enableAutomaticChannelChange ? L"true" : L"false");
                foundChannelChange = TRUE;
            }
            else if (wcsncmp(lines[lineCount], L"HudTheme=", 9) == 0) {
                /* Update existing HudTheme line when rewriting plugin.cfg
                   Met à jour la ligne HudTheme existante lors de la réécriture de plugin.cfg */
                swprintf(lines[lineCount], 1024, L"HudTheme=%d\n", voiceHudTheme);
                foundHudTheme = TRUE;
            }
            else if (wcsncmp(lines[lineCount], L"HudPosition=", 12) == 0) {
                swprintf(lines[lineCount], 1024, L"HudPosition=%d\n", voiceHudPosition);
                foundHudPosition = TRUE;
            }
            else if (wcsncmp(lines[lineCount], L"HudSize=", 8) == 0) {
                swprintf(lines[lineCount], 1024, L"HudSize=%d\n", voiceHudSize);
                foundHudSize = TRUE;
            }
            lineCount++;
        }
        fclose(f);
    }

    // Add missing configuration lines | Ajouter les lignes de configuration manquantes
    if (!foundWhisper && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"DistanceWhisper=%.1f\n", distanceWhisper);
    }
    if (!foundNormal && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"DistanceNormal=%.1f\n", distanceNormal);
    }
    if (!foundShout && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"DistanceShout=%.1f\n", distanceShout);
    }
    if (!foundDistanceMuting && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"EnableDistanceMuting=%s\n", enableDistanceMuting ? L"true" : L"false");
    }
    if (!foundChannelChange && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"EnableAutomaticChannelChange=%s\n", enableAutomaticChannelChange ? L"true" : L"false");
    }
    if (!foundHudTheme && lineCount < 99) {
        /* Append HudTheme if older plugin.cfg files lack the key
           Ajoute HudTheme si les anciens plugin.cfg n'ont pas cette clé */
        swprintf(lines[lineCount++], 1024, L"HudTheme=%d\n", voiceHudTheme);
    }
    if (!foundHudPosition && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"HudPosition=%d\n", voiceHudPosition);
    }
    if (!foundHudSize && lineCount < 99) {
        swprintf(lines[lineCount++], 1024, L"HudSize=%d\n", voiceHudSize);
    }

    // Write updated configuration to file | Écrire la configuration mise à jour dans le fichier
    f = NULL;
    err = _wfopen_s(&f, configFile, L"w");
    if (err == 0 && f) {
        BOOL hasAutomaticSavedPath = FALSE;
        BOOL hasAutomaticPatchFind = FALSE;

        for (int i = 0; i < lineCount; i++) {
            fwprintf(f, L"%s", lines[i]);
            if (wcsncmp(lines[i], L"AutomaticSavedPath=", 19) == 0) {
                hasAutomaticSavedPath = TRUE;
            }
            if (wcsncmp(lines[i], L"AutomaticPatchFind=", 19) == 0) {
                hasAutomaticPatchFind = TRUE;
            }
        }

        fclose(f);

        // Update active distance based on absolute truth | Mettre à jour la distance active selon la vérité absolue
        localVoiceData.voiceDistance = getVoiceDistanceForMode(currentVoiceMode);

        // Debug logging | Log de debug
        if (TEMP) {
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "Config saved - Voice mode preserved - Current distance: %.1f",
                localVoiceData.voiceDistance);
            mumbleAPI.log(ownID, logMsg);
        }
    }
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

    // Save current voice mode | Sauvegarder le mode vocal actuel
    float currentVoiceDistance = localVoiceData.voiceDistance;

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
    wchar_t* configFolder = getConfigFolderPath();
    if (configFolder) {
        wchar_t configFile[MAX_PATH];
        swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);
        FILE* f = _wfopen(configFile, L"r");
        if (f) {
            wchar_t line[512];
            while (fgetws(line, 512, f)) {
                if (wcsncmp(line, L"SavedPath=", 10) == 0) {
                    wchar_t* pathStart = line + 10;
                    wchar_t* nl = wcschr(pathStart, L'\n');
                    if (nl) *nl = L'\0';
                    wchar_t* cr = wcschr(pathStart, L'\r');
                    if (cr) *cr = L'\0';

                    wcscpy_s(gameFolder, MAX_PATH, pathStart);
                    wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
                    if (conanSandbox) {
                        *conanSandbox = L'\0';
                    }
                    break;
                }
            }
            fclose(f);
        }
    }

    wchar_t distWhisper[32], distNormal[32], distShout[32];
    swprintf(distWhisper, 32, L"%.1f", distanceWhisper);
    swprintf(distNormal, 32, L"%.1f", distanceNormal);
    swprintf(distShout, 32, L"%.1f", distanceShout);

    writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

    // Apply changes | Appliquer les changements
    applyDistanceToAllPlayers();

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
    wchar_t* configFolder = getConfigFolderPath();
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
    wchar_t* configFolder = getConfigFolderPath();
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

// Write full Saved path to config file | Écriture du chemin complet Saved dans le fichier de configuration
void writeFullConfiguration(const wchar_t* gameFolder, const wchar_t* distWhisper, const wchar_t* distNormal, const wchar_t* distShout) {
    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) {
        return;
    }

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);

    // Construire le chemin COMPLET pour l'enregistrement (avec \ConanSandbox\Saved)
    wchar_t savedPathFull[MAX_PATH];

    // Si displayedPathText contient le chemin, ajouter \ConanSandbox\Saved
    if (wcslen(displayedPathText) > 0) {
        // displayedPathText = C:\...\Conan Exiles (SANS \ConanSandbox\Saved)
        // Construire le chemin COMPLET = C:\...\Conan Exiles\ConanSandbox\Saved
        swprintf(savedPathFull, MAX_PATH, L"%s\\ConanSandbox\\Saved", displayedPathText);
    }
    // Sinon, construire depuis gameFolder (fallback)
    else if (gameFolder && wcslen(gameFolder) > 0) {
        swprintf(savedPathFull, MAX_PATH, L"%s\\ConanSandbox\\Saved", gameFolder);
    }
    else {
        // Valeur par défaut si rien n'est disponible
        wcscpy_s(savedPathFull, MAX_PATH, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Conan Exiles\\ConanSandbox\\Saved");
    }

    // Save current voice mode before modifying distances | Sauvegarder le mode de voix actuel AVANT de modifier les distances
    float currentVoiceDistance = localVoiceData.voiceDistance;

    // CORRECTION: Convertir les valeurs d'entrée SANS appliquer de limites
    float whisperValue = (float)_wtof(distWhisper);
    float normalValue = (float)_wtof(distNormal);
    float shoutValue = (float)_wtof(distShout);

    // CORRECTION: Mettre à jour les variables globales avec les valeurs ORIGINALES de l'utilisateur
    distanceWhisper = whisperValue;
    distanceNormal = normalValue;
    distanceShout = shoutValue;

    // Read existing SavedPath and AutomaticSavedPath before overwriting | Lire le SavedPath et AutomaticSavedPath existants avant écrasement
    wchar_t existingSavedPath[MAX_PATH] = L"";
    wchar_t existingAutoPath[MAX_PATH] = L"";

    FILE* fRead = _wfopen(configFile, L"r");
    if (fRead) {
        wchar_t line[512];
        while (fgetws(line, 512, fRead)) {
            if (wcsncmp(line, L"SavedPath=", 10) == 0) {
                wchar_t* pathStart = line + 10;
                wchar_t* nl = wcschr(pathStart, L'\n');
                if (nl) *nl = L'\0';
                wchar_t* cr = wcschr(pathStart, L'\r');
                if (cr) *cr = L'\0';
                wcscpy_s(existingSavedPath, MAX_PATH, pathStart);
            }
            else if (wcsncmp(line, L"AutomaticSavedPath=", 19) == 0) {
                wchar_t* pathStart = line + 19;
                wchar_t* nl = wcschr(pathStart, L'\n');
                if (nl) *nl = L'\0';
                wchar_t* cr = wcschr(pathStart, L'\r');
                if (cr) *cr = L'\0';
                wcscpy_s(existingAutoPath, MAX_PATH, pathStart);
            }
        }
        fclose(fRead);
    }

    // Write configuration file | Écrire le fichier de configuration
    FILE* file = _wfopen(configFile, L"w");
    if (!file) {
        return;
    }

    // Logic for SavedPath | Logique pour SavedPath
    if (enableAutomaticPatchFind) {
        // Automatic mode: preserve manual path in SavedPath, write to AutomaticSavedPath | Mode automatique : préserver le chemin manuel dans SavedPath, écrire dans AutomaticSavedPath
        if (wcslen(existingSavedPath) > 0) {
            fwprintf(file, L"SavedPath=%s\n", existingSavedPath);
        }
        else {
            fwprintf(file, L"SavedPath=\n");
        }
        fwprintf(file, L"AutomaticSavedPath=%s\n", savedPathFull);
    }
    else {
        // Manual mode: write to SavedPath, preserve automatic path in AutomaticSavedPath | Mode manuel : écrire dans SavedPath, préserver le chemin automatique dans AutomaticSavedPath
        fwprintf(file, L"SavedPath=%s\n", savedPathFull);
        if (wcslen(existingAutoPath) > 0) {
            fwprintf(file, L"AutomaticSavedPath=%s\n", existingAutoPath);
        }
        else {
            fwprintf(file, L"AutomaticSavedPath=\n");
        }
    }

    fwprintf(file, L"AutomaticPatchFind=%s\n", enableAutomaticPatchFind ? L"true" : L"false");
    fwprintf(file, L"EnableDistanceMuting=%s\n", enableDistanceMuting ? L"true" : L"false");
    fwprintf(file, L"EnableAutomaticChannelChange=%s\n", enableAutomaticChannelChange ? L"true" : L"false");
    fwprintf(file, L"WhisperKey=%d\n", whisperKey);
    fwprintf(file, L"NormalKey=%d\n", normalKey);
    fwprintf(file, L"ShoutKey=%d\n", shoutKey);
    fwprintf(file, L"ConfigUIKey=%d\n", configUIKey);
    fwprintf(file, L"DistanceWhisper=%.1f\n", distanceWhisper);
    fwprintf(file, L"DistanceNormal=%.1f\n", distanceNormal);
    fwprintf(file, L"DistanceShout=%.1f\n", distanceShout);
    fwprintf(file, L"VoiceToggleKey=%d\n", voiceToggleKey);
    fwprintf(file, L"EnableVoiceToggle=%s\n", enableVoiceToggle ? L"true" : L"false");
    fwprintf(file, L"HudTheme=%d\n", voiceHudTheme);
    fwprintf(file, L"HudPosition=%d\n", voiceHudPosition);
    fwprintf(file, L"HudSize=%d\n", voiceHudSize);
    fclose(file);

    if (enableLogConfig) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "✅ USER VALUES SAVED: Whisper=%.1f, Normal=%.1f, Shout=%.1f",
            distanceWhisper, distanceNormal, distanceShout);
        mumbleAPI.log(ownID, logMsg);
    }

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

    // Update mod file path | Mettre à jour le chemin du fichier mod
    size_t converted = 0;
    char modFilePathTemp[MAX_PATH] = "";
    wcstombs_s(&converted, modFilePathTemp, MAX_PATH, savedPathFull, _TRUNCATE);
    snprintf(modFilePath, MAX_PATH, "%s\\Pos.txt", modFilePathTemp);

    // Reinstall keyboard monitoring | Réinstaller la surveillance du clavier
    removeKeyMonitoring();
    installKeyMonitoring();
}

// ============================================================================
