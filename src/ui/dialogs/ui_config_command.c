#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "core/config/config.h"
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
#include <uxtheme.h>
#include "ui_config_internal.h"
#include "ui/config/ui_config_state.h"

/*
 * ui_config_command.c: WM_COMMAND handler for the F10 settings dialog.
 * Pure move-split from ui_main.c (V8.10). UI thread only.
 */

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
            if (ui_cfg()->automaticPatchFind) {
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
                    ui_cfg()->enableDistanceMuting = 1;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    ui_cfg()->enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
                    updateDynamicInterface();
                }
            }
            break;

        case 203: // Automatic Channel Change checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                if (hubForceAutomaticChannelSwitching) {
                    // Serveur force -> garder coché
                    CheckDlgButton(hwnd, 203, BST_CHECKED);
                    ui_cfg()->enableAutomaticChannelChange = 1;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    ui_cfg()->enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
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
                ui_cfg()->enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            }
            break;

        case 200: // Automatic Patch Find checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                ui_cfg()->automaticPatchFind = (IsDlgButtonChecked(hwnd, 200) == BST_CHECKED);

                if (ui_cfg()->automaticPatchFind) {
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
                    if (ui_cfg()->savedPath[0]) {
                        wcsncpy_s(displayedPathText, MAX_PATH, ui_cfg()->savedPath, _TRUNCATE);
                        wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                        if (conanSandbox) {
                            *conanSandbox = L'\0';
                        }
                    }
                    else if (wcslen(displayedPathText) == 0) {
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

                if (ui_cfg()->automaticPatchFind) {
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
    swprintf(distWhisper, 32, L"%.1f", ui_cfg()->distanceWhisper);
    swprintf(distNormal, 32, L"%.1f", ui_cfg()->distanceNormal);
    swprintf(distShout, 32, L"%.1f", ui_cfg()->distanceShout);

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
    if (ui_cfg()->automaticPatchFind) {
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
               ui_cfg()->enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
               ui_cfg()->enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
               ui_cfg()->enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);

               wchar_t distWhisper[32], distNormal[32], distShout[32];
               GetWindowTextW(hDistanceWhisperEdit, distWhisper, 32);
               GetWindowTextW(hDistanceNormalEdit, distNormal, 32);
               GetWindowTextW(hDistanceShoutEdit, distShout, 32);

               ui_cfg()->distanceWhisper = (float)_wtof(distWhisper);
               ui_cfg()->distanceNormal = (float)_wtof(distNormal);
               ui_cfg()->distanceShout = (float)_wtof(distShout);
               ui_read_hud_theme_from_combo();
               ui_read_hud_position_from_combo();
               ui_read_hud_size_from_combo();

               wchar_t gameFolder[MAX_PATH] = L"";
               wchar_t activeSaved[MAX_PATH] = L"";
               ui_cfg_get_active_saved_path(activeSaved, MAX_PATH);
               if (activeSaved[0]) {
                   wcsncpy_s(gameFolder, MAX_PATH, activeSaved, _TRUNCATE);
                   wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
                   if (conanSandbox) {
                       *conanSandbox = L'\0';
                   }
               }

               writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

               float currentVoiceDistance = localVoiceData.voiceDistance;
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

               if (ui_cfg()->enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

               showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

               if (enableLogConfig) {
                   char logMsg[512];
                   snprintf(logMsg, sizeof(logMsg),
                       "✅ ADVANCED OPTIONS SAVED: WhisperKey=%d NormalKey=%d ShoutKey=%d ConfigKey=%d VoiceToggleKey=%d Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                       ui_cfg()->whisperKey, ui_cfg()->normalKey, ui_cfg()->shoutKey, ui_cfg()->configUIKey, ui_cfg()->voiceToggleKey,
                       ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout,
                       ui_cfg()->enableDistanceMuting ? "true" : "false",
                       ui_cfg()->enableAutomaticChannelChange ? "true" : "false",
                       ui_cfg()->enableVoiceToggle ? "true" : "false");
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
            ui_cfg()->enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
            ui_cfg()->enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
            ui_cfg()->enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
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
            ui_cfg()->distanceWhisper = whisperValue;
            ui_cfg()->distanceNormal = normalValue;
            ui_cfg()->distanceShout = shoutValue;

            // Save everything using saveVoiceSettings() | Tout sauvegarder avec saveVoiceSettings()
            saveVoiceSettings();
            voice_overlay_refresh_position();
            voice_overlay_refresh_size();

            // Restore voice mode | Restaurer le mode vocal
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

            // Apply changes | Appliquer les changements
            if (ui_cfg()->enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

            showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg),
                    "Advanced options saved: Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                    ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout,
                    ui_cfg()->enableDistanceMuting ? "true" : "false",
                    ui_cfg()->enableAutomaticChannelChange ? "true" : "false",
                    ui_cfg()->enableVoiceToggle ? "true" : "false");
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
