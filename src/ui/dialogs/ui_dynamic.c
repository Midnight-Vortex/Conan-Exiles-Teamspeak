#include "plugin_internal.h"
#include "ui/config/ui_config_state.h"
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

// MODULE 16: UI - DYNAMIC CONTROLS
// EN: Runtime UI widgets — voice mode buttons, distance sliders, zone-specific controls.
// FR: Widgets UI runtime — boutons mode voix, curseurs distance, contrôles spécifiques zone.
// ============================================================================

// Handle distance edit changes with smart filtering | Gérer les changements d'édition de distance avec filtrage intelligent
static float* ui_cfg_distance_ptr(int editId) {
    PluginConfig* cfg = ui_cfg();
    switch (editId) {
    case 1: return &cfg->distanceWhisper;
    case 2: return &cfg->distanceNormal;
    case 3: return &cfg->distanceShout;
    default: return NULL;
    }
}

void handleDistanceEditChange(int editId) {
    if (isUpdatingInterface) return;

    HWND hEdit = NULL;
    float* targetDistance = NULL;
    const char* modeName = "";

    switch (editId) {
    case 1:
        hEdit = hDistanceWhisperEdit;
        modeName = "Whisper";
        break;
    case 2:
        hEdit = hDistanceNormalEdit;
        modeName = "Normal";
        break;
    case 3:
        hEdit = hDistanceShoutEdit;
        modeName = "Shout";
        break;
    default:
        return;
    }
    targetDistance = ui_cfg_distance_ptr(editId);

    if (!hEdit || !targetDistance) return;

    wchar_t text[32];
    GetWindowTextW(hEdit, text, 32);
    float newValue = (float)_wtof(text);

    // Validate value with filtering | Valider la valeur avec filtrage
    if (newValue > 0) {
        BOOL valueChanged = FALSE;
        float correctedValue = newValue;

        // Apply digit filter if server limits are active | Appliquer le filtre de chiffres si les limites serveur sont actives
        if (shouldApplyDistanceLimits()) {
            float minimum, maximum;

            switch (editId) {
            case 1: // Whisper
                minimum = (float)hubMinimumWhisper;
                maximum = (float)hubMaximumWhisper;
                break;
            case 2: // Normal
                minimum = (float)hubMinimumNormal;
                maximum = (float)hubMaximumNormal;
                break;
            case 3: // Shout
                minimum = (float)hubMinimumShout;
                maximum = (float)hubMaximumShout;
                break;
            default:
                return;
            }

            // Check if we have enough digits before validating | Vérifier si on a assez de chiffres avant de valider
            if (!shouldValidateValue(newValue, minimum, maximum, modeName)) {
                return;
            }

            correctedValue = validateDistanceValue(newValue, minimum, maximum, modeName);

            // Update field if value was corrected | Mettre à jour le champ si la valeur a été corrigée
            if (correctedValue != newValue) {
                isUpdatingInterface = TRUE;

                wchar_t correctedText[32];
                swprintf(correctedText, 32, L"%.1f", correctedValue);
                SetWindowTextW(hEdit, correctedText);

                SendMessage(hEdit, EM_SETSEL, wcslen(correctedText), wcslen(correctedText));

                isUpdatingInterface = FALSE;
                valueChanged = TRUE;

                if (enableLogGeneral) {
                    char logMsg[128];
                    snprintf(logMsg, sizeof(logMsg), "CORRECTED: %s %.1f -> %.1f (server limits)",
                        modeName, newValue, correctedValue);
                    mumbleAPI.log(ownID, logMsg);
                }
            }
        }
        else {
            // No server limits - accept values with at least 1 digit | Pas de limites serveur - accepter les valeurs avec au moins 1 chiffre
            if (countSignificantDigits(newValue) < 1) {
                return;
            }
        }

        // Apply corrected or original value | Appliquer la valeur corrigée ou originale
        if (correctedValue != *targetDistance) {
            *targetDistance = correctedValue;
            valueChanged = TRUE;

            if (enableLogGeneral) {
                char changeMsg[128];
                snprintf(changeMsg, sizeof(changeMsg), "Distance changed: %s = %.1f", modeName, correctedValue);
                mumbleAPI.log(ownID, changeMsg);
            }
        }

        // Save and update interface if necessary | Sauvegarder et mettre à jour l'interface si nécessaire
        if (valueChanged) {
            saveVoiceSettings();
            updateDynamicInterface();
            forceInterfaceRefresh();
        }
    }
}


// Update dynamic interface | Mettre à jour l'interface dynamique
void updateDynamicInterface() {
    if (isUpdatingInterface) return;
    if (!hConfigDialog || !IsWindow(hConfigDialog)) return;

    if (currentCategory != 2) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "updateDynamicInterface: Not in category 2 - skipping update");
        }
        return;
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "DEBUG: updateDynamicInterface() CALLED!");
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "DEBUG: updateDynamicInterface() CALLED!");
    }

    ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastInterfaceUpdate < 100) return;
    lastInterfaceUpdate = currentTime;

    isUpdatingInterface = TRUE;

    // Force distance-based muting if required | Forcer le muting basé sur la distance si nécessaire
    if (hubForceDistanceBasedMuting && !ui_cfg()->enableDistanceMuting) {
        ui_cfg()->enableDistanceMuting = 1;
        if (hEnableDistanceMutingCheck) {
            CheckDlgButton(hConfigDialog, 201, BST_CHECKED);
        }
    }

    // Update distance muting checkbox state | Mettre à jour l'état de la checkbox de muting
    if (hEnableDistanceMutingCheck) {
        BOOL shouldDisable = hubDescriptionAvailable && hubForceDistanceBasedMuting;
        EnableWindow(hEnableDistanceMutingCheck, !shouldDisable);

        if (enableLogGeneral) {
            char debugMsg[256];
            snprintf(debugMsg, sizeof(debugMsg),
                "Distance muting checkbox: hubDescriptionAvailable=%s, hubForceDistanceBasedMuting=%s, shouldDisable=%s",
                hubDescriptionAvailable ? "TRUE" : "FALSE",
                hubForceDistanceBasedMuting ? "TRUE" : "FALSE",
                shouldDisable ? "TRUE" : "FALSE");
            mumbleAPI.log(ownID, debugMsg);
        }
    }

    // Force automatic channel change if required | Forcer le changement automatique de canal si nécessaire
    if (hubForceAutomaticChannelSwitching && !ui_cfg()->enableAutomaticChannelChange) {
        ui_cfg()->enableAutomaticChannelChange = 1;
        if (hEnableAutomaticChannelChangeCheck) {
            CheckDlgButton(hConfigDialog, 203, BST_CHECKED);
        }
    }

    // Update channel switching checkbox state | Mettre à jour l'état de la checkbox de changement de canal
    if (hEnableAutomaticChannelChangeCheck) {
        BOOL shouldDisable = hubDescriptionAvailable && hubForceAutomaticChannelSwitching;
        EnableWindow(hEnableAutomaticChannelChangeCheck, !shouldDisable);

        if (enableLogGeneral) {
            char debugMsg[256];
            snprintf(debugMsg, sizeof(debugMsg),
                "Channel switching checkbox: hubDescriptionAvailable=%s, hubForceAutomaticChannelSwitching=%s, shouldDisable=%s",
                hubDescriptionAvailable ? "TRUE" : "FALSE",
                hubForceAutomaticChannelSwitching ? "TRUE" : "FALSE",
                shouldDisable ? "TRUE" : "FALSE");
            mumbleAPI.log(ownID, debugMsg);
        }
    }

    // Force positional audio if required | Forcer l'audio positionnel si nécessaire
    if (hubForcePositionalAudio && !enableAutoAudioSettings) {
        enableAutoAudioSettings = TRUE;
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Hub: Positional audio FORCED by server - enabling automatically");
        }
    }

    updateConsolidatedDistanceMessages();

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "DEBUG: About to call updateServerLimitMessages()");
    }

    updateServerLimitMessages();
    updateDistanceMutingMessage();
    updateChannelSwitchingMessage();
    updatePositionalAudioMessage();

    float newWhisper = ui_cfg()->distanceWhisper;
    float newNormal = ui_cfg()->distanceNormal;
    float newShout = ui_cfg()->distanceShout;

    // Apply server or zone limits | Appliquer les limites serveur ou zone
    if (currentZoneIndex != -1) {
        newWhisper = zones[currentZoneIndex].whisperDist;
        newNormal = zones[currentZoneIndex].normalDist;
        newShout = zones[currentZoneIndex].shoutDist;
    }
    else if (shouldApplyDistanceLimits()) {
        newWhisper = validateDistanceValue(ui_cfg()->distanceWhisper, (float)hubMinimumWhisper, (float)hubMaximumWhisper, "Whisper");
        newNormal = validateDistanceValue(ui_cfg()->distanceNormal, (float)hubMinimumNormal, (float)hubMaximumNormal, "Normal");
        newShout = validateDistanceValue(ui_cfg()->distanceShout, (float)hubMinimumShout, (float)hubMaximumShout, "Shout");
    }

    BOOL distanceChanged = FALSE;

    // Update UI fields according to current context (zone or user) | Mettre à jour les champs UI (zone ou utilisateur)
    if (hDistanceWhisperEdit) { wchar_t vt[32]; swprintf(vt, 32, L"%.1f", newWhisper); SetWindowTextW(hDistanceWhisperEdit, vt); }
    if (hDistanceNormalEdit) { wchar_t vt[32]; swprintf(vt, 32, L"%.1f", newNormal); SetWindowTextW(hDistanceNormalEdit, vt); }
    if (hDistanceShoutEdit) { wchar_t vt[32]; swprintf(vt, 32, L"%.1f", newShout); SetWindowTextW(hDistanceShoutEdit, vt); }

    // Only update global variables and save if NOT in a zone | Uniquement si HORS d'une zone
    if (currentZoneIndex == -1) {
        if (newWhisper != ui_cfg()->distanceWhisper) { ui_cfg()->distanceWhisper = newWhisper; distanceChanged = TRUE; }
        if (newNormal != ui_cfg()->distanceNormal) { ui_cfg()->distanceNormal = newNormal; distanceChanged = TRUE; }
        if (newShout != ui_cfg()->distanceShout) { ui_cfg()->distanceShout = newShout; distanceChanged = TRUE; }
    }

    // Always save distance changes | Toujours sauvegarder les changements de distance
    if (distanceChanged) {
        saveVoiceSettings();
        if (ui_cfg()->enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }

        if (enableLogGeneral) {
            char saveMsg[256];
            snprintf(saveMsg, sizeof(saveMsg),
                "Distances saved to config: Whisper=%.1f, Normal=%.1f, Shout=%.1f (ForceDistanceBasedMuting=%s)",
                ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout,
                hubForceDistanceBasedMuting ? "TRUE" : "FALSE");
            mumbleAPI.log(ownID, saveMsg);
        }
    }

    // Force redraw of all messages | Forcer le redessin de tous les messages
    if (hDistanceMutingMessage) {
        InvalidateRect(hDistanceMutingMessage, NULL, TRUE);
        UpdateWindow(hDistanceMutingMessage);
    }
    if (hChannelSwitchingMessage) {
        InvalidateRect(hChannelSwitchingMessage, NULL, TRUE);
        UpdateWindow(hChannelSwitchingMessage);
    }
    if (hPositionalAudioMessage) {
        InvalidateRect(hPositionalAudioMessage, NULL, TRUE);
        UpdateWindow(hPositionalAudioMessage);
    }

    isUpdatingInterface = FALSE;
}

void forceInterfaceRefresh() {
    if (!hConfigDialog || !IsWindow(hConfigDialog)) return;
    if (currentCategory != 2) return;

    // Force immediate message updates | Forcer la mise à jour immédiate des messages
    updateConsolidatedDistanceMessages();
    updateDistanceMutingMessage();
    updateChannelSwitchingMessage();
    updatePositionalAudioMessage();

    // Force redraw of all messages | Forcer le redessin de tous les messages
    if (hDistanceWhisperMessage) {
        InvalidateRect(hDistanceWhisperMessage, NULL, TRUE);
        UpdateWindow(hDistanceWhisperMessage);
    }
    if (hDistanceNormalMessage) {
        InvalidateRect(hDistanceNormalMessage, NULL, TRUE);
        UpdateWindow(hDistanceNormalMessage);
    }
    if (hDistanceShoutMessage) {
        InvalidateRect(hDistanceShoutMessage, NULL, TRUE);
        UpdateWindow(hDistanceShoutMessage);
    }
}

// ============================================================================
