#include "plugin_internal.h"
#include "ui/config/ui_config_state.h"
#include "ui/config/ui_config_logic.h"
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
//
// 087: this runs on EVERY accepted keystroke (EN_CHANGE). It only updates the dialog's
// working copy and the dynamic UI — nothing is written to g_config / plugin.cfg here.
// Persisting happens on Save (or a preset load). It used to call saveVoiceSettings()
// per keystroke: typing "150" wrote the file 3 times, broadcast 3 times, and made
// Cancel useless because the whole working copy had already been committed.
// 087 : chaque frappe ne modifie QUE la copie de travail et l'interface ; la
// sauvegarde se fait avec Save (ou le chargement d'un preset).
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
    // Strict parse (no server limits here — they are applied by the filter below).
    // Partial input while typing ("", ".", "abc", "nan") is simply ignored.
    float newValue = 0.0f;
    const BOOL parsedOk = (ui_dist_parse(text, 0, 0.0f, 0.0f, &newValue) == UI_DIST_OK);

    // Validate value with filtering | Valider la valeur avec filtrage
    if (parsedOk && newValue > 0) {
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

        // Update interface if necessary (working copy only — see the note above)
        // Mettre à jour l'interface si nécessaire (copie de travail uniquement)
        if (valueChanged) {
            updateDynamicInterface();
            forceInterfaceRefresh();
        }
    }
}


// Writes a distance as "%.1f" into an edit, but only when the edit is not focused and
// its current text differs. | Ecrit la distance en "%.1f" si le champ n'a pas le focus
// et que son texte differe.
static void ui_set_distance_edit_text(HWND hEdit, float value) {
    if (!hEdit || GetFocus() == hEdit) {
        return;
    }
    wchar_t wanted[32];
    swprintf(wanted, 32, L"%.1f", value);
    wchar_t current[32] = L"";
    GetWindowTextW(hEdit, current, 32);
    if (wcscmp(current, wanted) != 0) {
        SetWindowTextW(hEdit, wanted);
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

    // Update UI fields according to current context (zone or user) | Mettre à jour les champs UI (zone ou utilisateur)
    // A field is rewritten ONLY when it does not have the keyboard focus and its text
    // really differs: rewriting the field the user is typing in (to "%.1f") made the
    // caret jump and scrambled the digits after every keystroke.
    // Un champ n'est reecrit que s'il n'a pas le focus ET que son texte differe.
    ui_set_distance_edit_text(hDistanceWhisperEdit, newWhisper);
    ui_set_distance_edit_text(hDistanceNormalEdit, newNormal);
    ui_set_distance_edit_text(hDistanceShoutEdit, newShout);

    // Only update the working copy if NOT in a zone | Uniquement si HORS d'une zone
    // (hub-limit auto-correction). NOT persisted here: that happened silently on every
    // call (checkbox click, tab switch, keystroke) and committed the whole working copy,
    // so Cancel could not discard anything. Save persists the corrected values; the
    // audio path clamps to the hub limits at use time anyway (voice_mode_get_distance).
    // Corrections de limites serveur : copie de travail seulement ; Save les enregistre.
    if (currentZoneIndex == -1) {
        if (newWhisper != ui_cfg()->distanceWhisper) { ui_cfg()->distanceWhisper = newWhisper; }
        if (newNormal != ui_cfg()->distanceNormal) { ui_cfg()->distanceNormal = newNormal; }
        if (newShout != ui_cfg()->distanceShout) { ui_cfg()->distanceShout = newShout; }
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
