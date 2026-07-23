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

/*
 * ui_main.c: WM_COMMAND handler, default-settings load/save, key capture.
 * ui_main.c : gestionnaire WM_COMMAND, parametres par defaut, capture touche.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
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
            case 1: whisperKey = vk; if (hWhisperKeyEdit) SetWindowTextA(hWhisperKeyEdit, getKeyName(vk)); break;
            case 2: normalKey = vk; if (hNormalKeyEdit) SetWindowTextA(hNormalKeyEdit, getKeyName(vk)); break;
            case 3: shoutKey = vk; if (hShoutKeyEdit) SetWindowTextA(hShoutKeyEdit, getKeyName(vk)); break;
            case 4: configUIKey = vk; if (hConfigKeyEdit) SetWindowTextA(hConfigKeyEdit, getKeyName(vk)); break;
            case 5: voiceToggleKey = vk; if (hVoiceToggleKeyEdit) SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(vk)); break;
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

// WM_COMMAND body: buttons, checkboxes, combos, presets, distance edits.
// Corps de WM_COMMAND : boutons, cases, combos, presets, champs distance.
LRESULT ui_config_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam) {
        switch (LOWORD(wParam)) {
        case 301: ShowCategoryControls(1); break;
        case 302:
            ShowCategoryControls(2);
            ui_sync_hud_theme_combo();
            updateConsolidatedDistanceMessages();
            break;
        case 303: ShowCategoryControls(3); break;

        case 105:
            if (enableAutomaticPatchFind) {
                // Automatic patch find is enabled | Automatic patch find est activé
                MessageBoxW(hwnd,
                    L"Automatic Patch Find is currently enabled.\n\n"
                    L"To use manual mode and browse for a custom patch location:\n"
                    L"1. Uncheck 'Automatic Patch Find' in the Patch Configuration tab\n"
                    L"2. Click 'Save Configuration'\n"
                    L"3. Then use Browse to select your custom patch location",
                    L"Manual Mode Disabled", MB_OK | MB_ICONWARNING);
            }
            else {
                browseSavedPath(hwnd);
            }
            break;

        case 101:
            isCapturingKey = TRUE; captureKeyTarget = 1;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hWhisperKeyEdit, "Press key..."); break;

        case 102:
            isCapturingKey = TRUE; captureKeyTarget = 2;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hNormalKeyEdit, "Press key..."); break;

        case 103:
            isCapturingKey = TRUE; captureKeyTarget = 3;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hShoutKeyEdit, "Press key..."); break;

        case 104:
            isCapturingKey = TRUE; captureKeyTarget = 4;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hConfigKeyEdit, "Press key..."); break;

        case 106:
            isCapturingKey = TRUE; captureKeyTarget = 5;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            EnableWindow(hVoiceToggleButton, FALSE);
            SetWindowTextA(hVoiceToggleKeyEdit, "Press key..."); break;

        case 201: // Distance Muting checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                if (hubForceDistanceBasedMuting) {
                    // Serveur force -> garder coché
                    CheckDlgButton(hwnd, 201, BST_CHECKED);
                    enableDistanceMuting = TRUE;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
                    updateDynamicInterface();
                }
            }
            break;

        case 203: // Automatic Channel Change checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                if (hubForceAutomaticChannelSwitching) {
                    // Serveur force -> garder coché
                    CheckDlgButton(hwnd, 203, BST_CHECKED);
                    enableAutomaticChannelChange = TRUE;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
                    updateDynamicInterface();
                }
            }
            break;

        case 215: // HUD theme combo — live overlay preview | combo thème HUD — aperçu live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_theme_from_combo();
                voice_overlay_refresh_theme();
            }
            break;

        case 216: // HUD position combo — live overlay reposition | combo position HUD — reposition live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_position_from_combo();
                voice_overlay_refresh_position();
            }
            break;

        case 217: // HUD size combo — live overlay resize | combo taille HUD — redimensionnement live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_size_from_combo();
                voice_overlay_refresh_size();
            }
            break;

        case 204: // Voice Toggle checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                // Pas de verrou serveur - toggle normal
                enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            }
            break;

        case 200: // Automatic Patch Find checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                enableAutomaticPatchFind = (IsDlgButtonChecked(hwnd, 200) == BST_CHECKED);

                if (enableAutomaticPatchFind) {
                    // ✅ REDEMANDER le chemin Steam pour être SÛR
                    wchar_t autoPathFull[MAX_PATH] = L"";
                    if (findConanExilesAutomatic(autoPathFull, MAX_PATH)) {
                        // ✅ Afficher SANS \ConanSandbox\Saved
                        wcscpy_s(displayedPathText, MAX_PATH, autoPathFull);
                        wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                        if (conanSandbox) {
                            *conanSandbox = L'\0';
                        }

                        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                            InvalidateRect(hSavedPathBg, NULL, TRUE);
                            UpdateWindow(hSavedPathBg);
                        }

                        if (enableLogConfig) {
                            char logMsg[512];
                            size_t converted = 0;
                            char pathUtf8[MAX_PATH];
                            wcstombs_s(&converted, pathUtf8, MAX_PATH, autoPathFull, _TRUNCATE);
                            snprintf(logMsg, sizeof(logMsg),
                                "✅ AUTOMATIC PATH DETECTED: %s", pathUtf8);
                            mumbleAPI.log(ownID, logMsg);
                        }

                        showStatusMessage(L"Automatic path found - Click Save to apply", FALSE);
                    }
                    else {
                        wcscpy_s(displayedPathText, MAX_PATH, L"(Not found)");
                        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                            InvalidateRect(hSavedPathBg, NULL, TRUE);
                            UpdateWindow(hSavedPathBg);
                        }
                        showStatusMessage(L"Could not find Conan Exiles - Check Steam installation", TRUE);
                    }
                }
                else {
                    // MODE MANUEL : Charger le chemin MANUEL depuis la config
                    const wchar_t* configFolder = config_get_folder_path();
                    if (configFolder) {
                        wchar_t configFile[MAX_PATH];
                        swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);
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

                                    wcscpy_s(displayedPathText, MAX_PATH, pathStart);
                                    wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                                    if (conanSandbox) {
                                        *conanSandbox = L'\0';
                                    }
                                    break;
                                }
                            }
                            fclose(fRead);
                        }
                    }

                    if (wcslen(displayedPathText) == 0) {
                        wcscpy_s(displayedPathText, MAX_PATH, L"No path configured");
                    }

                    if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                        InvalidateRect(hSavedPathBg, NULL, TRUE);
                        UpdateWindow(hSavedPathBg);
                    }
                    showStatusMessage(L"Manual path displayed", FALSE);
                }
            }
            break;

        case 1: { // Save Configuration
            if (currentCategory == 1) {
                // === CATÉGORIE 1 : PATCH CONFIGURATION ===

                // ✅ CORRECTION : Construire le chemin COMPLET incluant ConanSandbox\Saved
                wchar_t pathToSave[MAX_PATH] = L"";

                if (enableAutomaticPatchFind) {
                    // ✅ REDEMANDER le chemin Steam RÉEL pour être 100% SÛR
                    wchar_t realSteamPath[MAX_PATH] = L"";
                    if (!findConanExilesAutomatic(realSteamPath, MAX_PATH)) {
                        showStatusMessage(L"⚠ Error: Could not find Conan Exiles automatically", TRUE);
                        break;
                    }

                    // ✅ Utiliser le VRAI chemin Steam (pas displayedPathText)
                    wcscpy_s(pathToSave, MAX_PATH, realSteamPath);

        // ✅ pathToSave contient maintenant le VRAI chemin Steam complet
        if (enableLogConfig) {
            char logMsg[512];
            size_t converted = 0;
            char pathUtf8[MAX_PATH];
            wcstombs_s(&converted, pathUtf8, MAX_PATH, pathToSave, _TRUNCATE);
            snprintf(logMsg, sizeof(logMsg),
                "✅ AUTOMATIC MODE: Using REAL Steam path: %s", pathUtf8);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else {
        // Mode manuel : utiliser displayedPathText
        if (wcslen(displayedPathText) == 0) {
            MessageBoxW(hwnd,
                L"Please select your Conan Exiles game folder using the Browse button.",
                L"Missing Path", MB_OK | MB_ICONWARNING);

            showStatusMessage(L"⚠ Error: No game path specified", TRUE);
            break;
        }

        // Construire le chemin complet (displayedPathText + \ConanSandbox\Saved)
        wcscpy_s(pathToSave, MAX_PATH, displayedPathText);
        wcscat_s(pathToSave, MAX_PATH, L"\\ConanSandbox\\Saved");

        if (enableLogConfig) {
            char logMsg[512];
            size_t converted = 0;
            char pathUtf8[MAX_PATH];
            wcstombs_s(&converted, pathUtf8, MAX_PATH, pathToSave, _TRUNCATE);
            snprintf(logMsg, sizeof(logMsg),
                "✅ MANUAL MODE: Using manual path: %s", pathUtf8);
            mumbleAPI.log(ownID, logMsg);
        }
    }

    // ✅ 2) Vérifier UNIQUEMENT que le dossier ConanSandbox\Saved existe
    DWORD savedAttribs = GetFileAttributesW(pathToSave);
    if (savedAttribs == INVALID_FILE_ATTRIBUTES || !(savedAttribs & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t errorMsg[512];
        swprintf(errorMsg, 512,
            L"The folder 'ConanSandbox\\Saved' does not exist in:\n%s\n\n"
            L"Please verify:\n"
            L"1. This is your Conan Exiles game folder\n",
            pathToSave);

        MessageBoxW(hwnd, errorMsg, L"Folder Not Found", MB_OK | MB_ICONERROR);
        showStatusMessage(L"⚠ Error: ConanSandbox\\Saved folder not found", TRUE);
        break;
    }

    // ✅ 3) Toutes les vérifications passées → SAUVEGARDER
    wchar_t distWhisper[32], distNormal[32], distShout[32];
    swprintf(distWhisper, 32, L"%.1f", distanceWhisper);
    swprintf(distNormal, 32, L"%.1f", distanceNormal);
    swprintf(distShout, 32, L"%.1f", distanceShout);

    // Extraire le dossier du jeu (sans ConanSandbox\Saved) pour writeFullConfiguration
    wchar_t gameFolder[MAX_PATH];
    wcscpy_s(gameFolder, MAX_PATH, pathToSave);
    wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
    if (conanSandbox) {
        *conanSandbox = L'\0';
    }

    BOOL wasAlreadySaved = isPatchAlreadySaved();

    writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

    // ✅ MISE À JOUR IMMÉDIATE DE L'AFFICHAGE APRÈS SAUVEGARDE
    if (enableAutomaticPatchFind) {
        // Afficher le chemin Steam dans l'interface
        wcscpy_s(displayedPathText, MAX_PATH, gameFolder);
        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
            InvalidateRect(hSavedPathBg, NULL, TRUE);
            UpdateWindow(hSavedPathBg);
        }
    }

    if (!wasAlreadySaved) {
        showConfigSavedNotice(hwnd, L"Patch configuration saved successfully!");
    }
    else {
        showConfigSavedNotice(hwnd, L"Patch configuration updated successfully!");
    }

    if (enableLogConfig) {
        char logMsg[512];
        size_t converted = 0;
        char savedPathUtf8[MAX_PATH];
        wcstombs_s(&converted, savedPathUtf8, MAX_PATH, pathToSave, _TRUNCATE);

        snprintf(logMsg, sizeof(logMsg),
            "✅ SECURITY PASSED: Saved folder verified at: %s",
            savedPathUtf8);
        mumbleAPI.log(ownID, logMsg);
    }
}
           else if (currentCategory == 2) {
               // === CATÉGORIE 2 : ADVANCED OPTIONS (reste inchangé) ===
               enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
               enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
               enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);

               wchar_t distWhisper[32], distNormal[32], distShout[32];
               GetWindowTextW(hDistanceWhisperEdit, distWhisper, 32);
               GetWindowTextW(hDistanceNormalEdit, distNormal, 32);
               GetWindowTextW(hDistanceShoutEdit, distShout, 32);

               distanceWhisper = (float)_wtof(distWhisper);
               distanceNormal = (float)_wtof(distNormal);
               distanceShout = (float)_wtof(distShout);
               ui_read_hud_theme_from_combo();
               ui_read_hud_position_from_combo();
               ui_read_hud_size_from_combo();

               wchar_t gameFolder[MAX_PATH] = L"";

               const wchar_t* configFolder = config_get_folder_path();
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

               writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

               float currentVoiceDistance = localVoiceData.voiceDistance;
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

               if (enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

               showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

               if (enableLogConfig) {
                   char logMsg[512];
                   snprintf(logMsg, sizeof(logMsg),
                       "✅ ADVANCED OPTIONS SAVED: WhisperKey=%d NormalKey=%d ShoutKey=%d ConfigKey=%d VoiceToggleKey=%d Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                       whisperKey, normalKey, shoutKey, configUIKey, voiceToggleKey,
                       distanceWhisper, distanceNormal, distanceShout,
                       enableDistanceMuting ? "true" : "false",
                       enableAutomaticChannelChange ? "true" : "false",
                       enableVoiceToggle ? "true" : "false");
                   mumbleAPI.log(ownID, logMsg);
               }
           }
           break;
       }

        case 11: { // Save Voice Range (Advanced Options)
            showPresetSaveDialog();
            break;
        }

        case 12: { // Save Configuration (Advanced Options - save ALL to plugin.cfg)
            // Save current voice mode before modifying | Sauvegarder le mode vocal actuel
            float currentVoiceDistance = localVoiceData.voiceDistance;

            // Get values from interface | Récupérer les valeurs de l'interface
            enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
            enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
            enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            ui_read_hud_theme_from_combo();
            ui_read_hud_position_from_combo();
            ui_read_hud_size_from_combo();

            wchar_t distWhisper[32], distNormal[32], distShout[32];
            GetWindowTextW(hDistanceWhisperEdit, distWhisper, 32);
            GetWindowTextW(hDistanceNormalEdit, distNormal, 32);
            GetWindowTextW(hDistanceShoutEdit, distShout, 32);

            // Convert distances | Convertir les distances
            float whisperValue = (float)_wtof(distWhisper);
            float normalValue = (float)_wtof(distNormal);
            float shoutValue = (float)_wtof(distShout);

            // Update global distances | Mettre à jour les distances globales
            distanceWhisper = whisperValue;
            distanceNormal = normalValue;
            distanceShout = shoutValue;

            // Save everything using saveVoiceSettings() | Tout sauvegarder avec saveVoiceSettings()
            saveVoiceSettings();
            voice_overlay_refresh_position();
            voice_overlay_refresh_size();

            // Restore voice mode | Restaurer le mode vocal
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

            // Apply changes | Appliquer les changements
            if (enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

            showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg),
                    "Advanced options saved: Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                    distanceWhisper, distanceNormal, distanceShout,
                    enableDistanceMuting ? "true" : "false",
                    enableAutomaticChannelChange ? "true" : "false",
                    enableVoiceToggle ? "true" : "false");
                mumbleAPI.log(ownID, logMsg);
            }
            break;
        }

        case 2: DestroyWindow(hwnd); break;

            // CORRECTION CRITIQUE: Gérer les boutons LOAD et RENAME **EN DEHORS** de EN_CHANGE
        default:
            // Handle preset load buttons | Gérer boutons load
            if (LOWORD(wParam) >= 900 && LOWORD(wParam) < 900 + MAX_VOICE_PRESETS) {
                int presetIndex = LOWORD(wParam) - 900;
                loadVoicePreset(presetIndex);
                break;
            }
            // Handle preset rename buttons | Gérer boutons rename
            else if (LOWORD(wParam) >= 950 && LOWORD(wParam) < 950 + MAX_VOICE_PRESETS) {
                int presetIndex = LOWORD(wParam) - 950;
                renamePresetIndex = presetIndex;

                if (!hPresetRenameDialog || !IsWindow(hPresetRenameDialog)) {
                    const wchar_t RENAME_DIALOG_CLASS[] = L"PresetRenameDialogClass";
                    WNDCLASSW wc = { 0 };
                    wc.lpfnWndProc = PresetRenameDialogProc;
                    wc.hInstance = GetModuleHandleW(NULL);
                    wc.lpszClassName = RENAME_DIALOG_CLASS;
                    wc.hbrBackground = CreateSolidBrush(RGB(248, 249, 250));
                    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

                    UnregisterClassW(RENAME_DIALOG_CLASS, wc.hInstance);
                    RegisterClassW(&wc);

                    // ✅ CORRECTION : Centrer au-dessus de l'interface principale
                    int dialogWidth = 300;
                    int dialogHeight = 240;
                    int dialogX, dialogY;

                    if (hwnd && IsWindow(hwnd)) {
                        // Get parent window position and size | Obtenir position et taille de la fenêtre parente
                        RECT parentRect;
                        GetWindowRect(hwnd, &parentRect);

                        int parentWidth = parentRect.right - parentRect.left;
                        int parentHeight = parentRect.bottom - parentRect.top;
                        int parentX = parentRect.left;
                        int parentY = parentRect.top;
                        dialogX = parentX + (parentWidth - dialogWidth) / 2;
                        dialogY = parentY + (parentHeight - dialogHeight) / 2;

                        if (enableLogGeneral) {
                            char logMsg[256];
                            snprintf(logMsg, sizeof(logMsg),
                                "Rename dialog: Parent at (%d,%d), Dialog at (%d,%d)",
                                parentX, parentY, dialogX, dialogY);
                            mumbleAPI.log(ownID, logMsg);
                        }
                    }
                    else {
                        // Fallback to screen center if parent not available | Centrer sur l'écran si parent indisponible
                        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                        dialogX = (screenWidth - dialogWidth) / 2;
                        dialogY = (screenHeight - dialogHeight) / 2;
                    }

                    hPresetRenameDialog = CreateWindowExW(
                        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                        RENAME_DIALOG_CLASS,
                        L"Rename Preset",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        dialogX, dialogY, dialogWidth, dialogHeight,
                        hwnd, NULL, wc.hInstance, NULL);

                    if (hPresetRenameDialog) {
                        SetLayeredWindowAttributes(hPresetRenameDialog, 0, 250, LWA_ALPHA);
                        ShowWindow(hPresetRenameDialog, SW_SHOW);
                        UpdateWindow(hPresetRenameDialog);
                    }
                }
                break;
            }
            // Handle distance field changes | Gestion des changements dans les champs de distance
            else if (HIWORD(wParam) == EN_CHANGE) {
                HWND hEditControl = (HWND)lParam;
                if (hEditControl == hDistanceWhisperEdit) {
                    handleDistanceEditChange(1);
                }
                else if (hEditControl == hDistanceNormalEdit) {
                    handleDistanceEditChange(2);
                }
                else if (hEditControl == hDistanceShoutEdit) {
                    handleDistanceEditChange(3);
                }
            }
            break;
        }
    return 0;
}
