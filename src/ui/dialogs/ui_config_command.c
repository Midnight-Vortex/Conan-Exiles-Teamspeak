#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "core/config/config.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/config/range_units.h"
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
#include "ui/config/ui_config_logic.h"

/*
 * ui_config_command.c: WM_COMMAND handler for the F10 settings dialog.
 * Pure move-split from ui_main.c (V8.10). UI thread only.
 *
 * 087: Save validates the distance fields (ui_collect_distances_for_save), the HUD
 * combos preview live through g_config (ui_cfg_hud_preview), and the Rename button
 * only retargets the preset when it really opens the rename dialog.
 */

// Reports an invalid distance field: status message (red), beep, focus on the field.
// Nothing has been written to the working copy or to disk at this point.
// 091: the server limits arrive in METERS and are shown in the display unit (min rounded up,
// max rounded down - the same numbers the status lines show); foundation mode names the
// unit, meter mode keeps the text exactly as it was.
// Signale un champ de distance invalide : message de statut, bip, focus sur le champ.
static void ui_report_bad_distance(HWND edit, const wchar_t* modeName, UiDistStatus status,
    float hubMinMeters, float hubMaxMeters, int inFoundations) {
    const float hubMin = range_limit_min_display(hubMinMeters, inFoundations);
    const float hubMax = range_limit_max_display(hubMaxMeters, inFoundations);
    const wchar_t* unit = inFoundations ? L" foundations" : L"";
    wchar_t msg[220];
    switch (status) {
    case UI_DIST_EMPTY:
        swprintf(msg, 220, L"\u26A0 Error: %ls distance is empty - enter a number greater than 0", modeName);
        break;
    case UI_DIST_NOT_POSITIVE:
        swprintf(msg, 220, L"\u26A0 Error: %ls distance must be greater than 0", modeName);
        break;
    case UI_DIST_OUT_OF_RANGE:
        if (hubMin > 0.0f && hubMax > 0.0f) {
            swprintf(msg, 220, L"\u26A0 Error: %ls distance must be between %.1f and %.1f%ls (server limits)",
                modeName, hubMin, hubMax, unit);
        }
        else if (hubMin > 0.0f) {
            swprintf(msg, 220, L"\u26A0 Error: %ls distance must be at least %.1f%ls (server limit)", modeName, hubMin, unit);
        }
        else {
            swprintf(msg, 220, L"\u26A0 Error: %ls distance must be at most %.1f%ls (server limit)", modeName, hubMax, unit);
        }
        break;
    case UI_DIST_NOT_NUMBER:
    default:
        swprintf(msg, 220, L"\u26A0 Error: %ls distance is not a valid number", modeName);
        break;
    }
    showStatusMessage(msg, TRUE);
    MessageBeep(MB_ICONWARNING);
    if (edit && IsWindow(edit)) {
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
}

// Reads + validates the three distance edits for a Save (see ui_config_internal.h).
BOOL ui_collect_distances_for_save(float* outWhisper, float* outNormal, float* outShout) {
    PluginConfig* cfg = ui_cfg();
    *outWhisper = cfg->distanceWhisper;
    *outNormal = cfg->distanceNormal;
    *outShout = cfg->distanceShout;

    // In a hub zone the edits show the ZONE's distances (updateDynamicInterface). They
    // must not be stored as the player's own distances: keep the personal working-copy
    // values. | Dans une zone les champs affichent les distances de la zone : on garde
    // les distances personnelles de la copie de travail.
    if (currentZoneIndex != -1) {
        return TRUE;
    }

    // 091: the Root force came or went while the dialog was open: the fields still show the old
    // unit. Re-render them from the working copy and make the player look at them once before
    // anything is saved - a number typed in one unit must never be read as the other.
    // 091 : l'unite a change pendant que le dialogue etait ouvert : reafficher, ne rien enregistrer.
    if (ui_range_unit_refresh()) {
        ui_range_unit_changed();
        showStatusMessage(L"\u26A0 Display unit changed by the server - check the values, then click Save again", TRUE);
        MessageBeep(MB_ICONWARNING);
        return FALSE;
    }

    // 091: the edits hold the DISPLAY unit (meters or foundations). ui_dist_parse_display reads
    // them back to meters, checks the server limits in that unit and keeps the stored meters
    // bit for bit when the player did not change a field (no 40 m -> 39.936 m drift).
    // 091 : les champs contiennent l'unite d'AFFICHAGE ; les metres stockes restent intacts
    // si le joueur n'a rien change.
    const BOOL limitsActive = shouldApplyDistanceLimits();
    const int inFoundations = ui_ranges_in_foundations();
    const struct {
        HWND edit;
        const wchar_t* name;
        double hubMin;
        double hubMax;
        float stored; // working copy, meters
    } fields[3] = {
        { hDistanceWhisperEdit, L"Whisper", hubMinimumWhisper, hubMaximumWhisper, cfg->distanceWhisper },
        { hDistanceNormalEdit, L"Normal", hubMinimumNormal, hubMaximumNormal, cfg->distanceNormal },
        { hDistanceShoutEdit, L"Shout", hubMinimumShout, hubMaximumShout, cfg->distanceShout },
    };
    float parsed[3] = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < 3; i++) {
        wchar_t text[32] = L"";
        UiDistStatus status = UI_DIST_EMPTY;
        if (fields[i].edit) {
            if (GetWindowTextLengthW(fields[i].edit) >= 32) {
                status = UI_DIST_NOT_NUMBER; // would be cut by the 32-char buffer
            }
            else {
                GetWindowTextW(fields[i].edit, text, 32);
                status = ui_dist_parse_display(text, inFoundations, fields[i].stored, limitsActive,
                    (float)fields[i].hubMin, (float)fields[i].hubMax, &parsed[i]);
            }
        }
        if (status != UI_DIST_OK) {
            ui_report_bad_distance(fields[i].edit, fields[i].name, status,
                (float)fields[i].hubMin, (float)fields[i].hubMax, inFoundations);
            return FALSE;
        }
    }

    *outWhisper = parsed[0];
    *outNormal = parsed[1];
    *outShout = parsed[2];
    return TRUE;
}

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

        case IDC_CHECK_RANGEUNIT: // "Show ranges in foundations" (091)
            if (HIWORD(wParam) == BN_CLICKED) {
                if (ui_ranges_forced_by_server()) {
                    // The Root forces the display -> stay checked. The player's own choice
                    // (working copy) is not touched, so it applies again off that server.
                    // Serveur force -> garder coche ; le choix du joueur n'est pas modifie.
                    CheckDlgButton(hwnd, IDC_CHECK_RANGEUNIT, BST_CHECKED);
                    showStatusMessage(L"Cannot change: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Working copy only (Save persists, Cancel discards, like every dialog
                    // setting); every distance is re-rendered at once from the meters it holds.
                    ui_cfg()->rangeInFoundations = (IsDlgButtonChecked(hwnd, IDC_CHECK_RANGEUNIT) == BST_CHECKED) ? 1 : 0;
                    ui_range_unit_changed();
                }
            }
            break;

        // HUD combos — live preview (087): the overlay reads g_config.hud*, not the dialog's
        // working copy, so the choice is pushed into g_config (no file save) and the overlay
        // is refreshed. Closing the dialog without Save restores the values from dialog open.
        // Combos HUD — aperçu live : le choix est poussé dans g_config (sans sauvegarde) ;
        // fermer sans Save restaure les valeurs d'ouverture.
        case 215: // HUD theme combo | combo thème HUD
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_theme_from_combo();
                ui_cfg_hud_preview(UI_HUD_PART_THEME);
            }
            break;

        case 216: // HUD position combo | combo position HUD
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_position_from_combo();
                ui_cfg_hud_preview(UI_HUD_PART_POSITION);
            }
            break;

        case 217: // HUD size combo | combo taille HUD
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_size_from_combo();
                ui_cfg_hud_preview(UI_HUD_PART_SIZE);
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

        // "\ConanSandbox\Saved" is appended below: a game folder that leaves no room for it
        // gets a message (wcscat_s used to abort the whole TeamSpeak process here).
        // Un dossier trop long donne un message au lieu de faire planter TeamSpeak.
        if (!ui_game_folder_fits(displayedPathText, MAX_PATH)) {
            showStatusMessage(L"\u26A0 Error: Game path is too long", TRUE);
            break;
        }

        // Construire le chemin complet (displayedPathText + \ConanSandbox\Saved)
        wcscpy_s(pathToSave, MAX_PATH, displayedPathText);
        wcscat_s(pathToSave, MAX_PATH, UI_SAVED_SUBPATH);

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
               // === CATÉGORIE 2 : ADVANCED OPTIONS ===
               // Validate the distance fields FIRST: an empty / non-numeric / out-of-range
               // field is refused with a status message and nothing is overwritten
               // (it used to become 0.5 m silently and report "saved successfully").
               float whisperValue, normalValue, shoutValue;
               if (!ui_collect_distances_for_save(&whisperValue, &normalValue, &shoutValue)) {
                   break;
               }

               ui_cfg()->enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
               ui_cfg()->enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
               ui_cfg()->enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);

               // writeFullConfiguration() takes the distances as text and stores them in the
               // working copy itself.
               wchar_t distWhisper[32], distNormal[32], distShout[32];
               swprintf(distWhisper, 32, L"%.1f", whisperValue);
               swprintf(distNormal, 32, L"%.1f", normalValue);
               swprintf(distShout, 32, L"%.1f", shoutValue);

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

            // Validate the distance fields first (nothing is overwritten when one is invalid)
            // Valider d'abord les champs de distance (rien n'est ecrase s'ils sont invalides)
            float whisperValue, normalValue, shoutValue;
            if (!ui_collect_distances_for_save(&whisperValue, &normalValue, &shoutValue)) {
                break;
            }

            // Get values from interface | Récupérer les valeurs de l'interface
            ui_cfg()->enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
            ui_cfg()->enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
            ui_cfg()->enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            ui_read_hud_theme_from_combo();
            ui_read_hud_position_from_combo();
            ui_read_hud_size_from_combo();

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

                if (hPresetRenameDialog && IsWindow(hPresetRenameDialog)) {
                    // The rename dialog is modeless and already open for its preset: a second
                    // click on any Rename button must not retarget it (the OK button would then
                    // rename the wrong preset) — just bring it to the front.
                    // Deja ouvert : ne pas changer de cible, juste le remettre devant.
                    SetForegroundWindow(hPresetRenameDialog);
                }
                else {
                    renamePresetIndex = presetIndex; // set only when the dialog is really created
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
