#include "plugin_internal.h"
#include "ui/config/ui_config_state.h"
#include "ui/config/ui_config_logic.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/config/range_units.h"
#include "core/proximity/proximity_math.h"
#include "ui_config_internal.h"
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

    // 091: the Root force came or went while the dialog is open -> the fields still show the
    // old unit. Re-render them all from the working copy instead of reading the text back.
    // 091 : l'unite a change pendant que le dialogue est ouvert : tout reafficher, ne rien relire.
    if (ui_range_unit_refresh()) {
        ui_range_unit_changed();
        return;
    }

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
    // 091: the text is in the DISPLAY unit (meters or foundations); the working copy is meters.
    // 091 : le texte est dans l'unite d'AFFICHAGE ; la copie de travail reste en metres.
    float newValue = 0.0f;
    const BOOL parsedOk = (ui_dist_parse(text, 0, 0.0f, 0.0f, &newValue) == UI_DIST_OK);

    // Validate value with filtering | Valider la valeur avec filtrage
    if (parsedOk && newValue > 0) {
        const int inFoundations = ui_ranges_in_foundations();

        // No drift: a text that still reads like the stored value in this unit is not a change
        // by the player (dialog open, preset load, unit switch). Keep the exact meters:
        // 40 m shows as "15.6" and must not become 15.6 * 2.56 = 39.936 m.
        // Pas de derive : un texte identique a la valeur affichee n'est pas une modification.
        if (range_display_unchanged(*targetDistance, inFoundations, newValue)) {
            return;
        }

        BOOL valueChanged = FALSE;
        float correctedValue = newValue; // display unit until it is converted below

        // Meter limits the converted value is pulled into (0 = none) | Limites en metres (0 = aucune)
        float clampMinMeters = 0.0f;
        float clampMaxMeters = 0.0f;

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

            // Hub limits in the display unit (min rounded up, max rounded down, so every
            // value inside the shown range is also valid in meters; a range narrower than
            // one 0.1 F step is rounded to nearest, never shown inverted). Meter mode: unchanged.
            if (inFoundations) {   // meter mode: no clamp, exactly as before 091
                clampMinMeters = minimum;
                clampMaxMeters = maximum;
            }
            range_limits_display(minimum, maximum, inFoundations, &minimum, &maximum);

            // Check if we have enough digits before validating | Vérifier si on a assez de chiffres avant de valider
            if (!shouldValidateValue(newValue, minimum, maximum, modeName)) {
                return;
            }

            correctedValue = validateDistanceValue(newValue, minimum, maximum, modeName);

            // Update field if value was corrected | Mettre à jour le champ si la valeur a été corrigée
            if (correctedValue != newValue) {
                isUpdatingInterface = TRUE;

                wchar_t correctedText[32];
                swprintf(correctedText, 32, L"%.1f", correctedValue); // display unit

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

        // Apply corrected or original value (converted to METERS; foundation mode pulls it
        // into the meter limits: 10 F = 25.5999985 m must not fail a 25.6 m minimum)
        // | Appliquer la valeur corrigée ou originale (en METRES, dans les limites)
        const float newMeters = range_clamp_meters(range_to_meters(correctedValue, inFoundations),
            clampMinMeters, clampMaxMeters);
        if (newMeters != *targetDistance) {
            *targetDistance = newMeters;
            valueChanged = TRUE;

            if (enableLogGeneral) {
                char changeMsg[128];
                snprintf(changeMsg, sizeof(changeMsg), "Distance changed: %s = %.1f", modeName, newMeters);
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


// 091: set only while ui_range_unit_changed() re-renders after the unit changed: a
// focused field must show the new unit too. UI thread only.
static BOOL s_forceEditRewrite = FALSE;

// Writes a distance (given in METERS) as "%.1f" in the display unit into an edit, but
// only when the edit is not focused and its current text differs.
// | Ecrit la distance (en METRES) en "%.1f" dans l'unite d'affichage si le champ n'a
// pas le focus et que son texte differe.
static void ui_set_distance_edit_text(HWND hEdit, float meters, BOOL rewriteFocused) {
    if (!hEdit || (GetFocus() == hEdit && !rewriteFocused)) {
        return;
    }
    wchar_t wanted[32];
    ui_range_text(wanted, 32, meters);
    wchar_t current[32] = L"";
    GetWindowTextW(hEdit, current, 32);
    if (wcscmp(current, wanted) != 0) {
        SetWindowTextW(hEdit, wanted);
    }
}

// Clamp a working-copy distance (METERS) into the hub limits. In foundation mode a
// correction lands on the limit AS SHOWN (min rounded up / max rounded down to 0.1 F),
// so the field, the status line and the stored value agree and the value is valid in
// meters too (2.1 m min is shown as 0.9 F = 2.304 m, not as 0.8). The result is always
// pulled into the meter limits, so float noise (10 F = 25.5999985 m vs 25.6 m) or a
// range narrower than one 0.1 F step (15..15 m) cannot leave it outside and make it
// flip on every refresh. A value that is already valid in meters is never touched.
// Meter mode: exactly validateDistanceValue.
static float ui_correct_to_hub_limits(float meters, double hubMin, double hubMax, const char* modeName) {
    const float corrected = validateDistanceValue(meters, (float)hubMin, (float)hubMax, modeName);
    if (corrected == meters || !ui_ranges_in_foundations()) {
        return corrected;
    }
    float shownMin = 0.0f;
    float shownMax = 0.0f;
    range_limits_display((float)hubMin, (float)hubMax, 1, &shownMin, &shownMax);
    const float target = (meters < (float)hubMin) ? range_to_meters(shownMin, 1) : range_to_meters(shownMax, 1);
    return range_clamp_meters(target, (float)hubMin, (float)hubMax);
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

    // 091: pick up a changed display unit (own checkbox, or the Root force that came / went
    // while the dialog is open). A unit change must rewrite a focused field as well - it would
    // otherwise keep the old unit's number next to the new unit's labels.
    const BOOL rewriteFocused = (ui_range_unit_refresh() || s_forceEditRewrite) ? TRUE : FALSE;

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

    // 091: "Show ranges in foundations". Forced by the Root -> checked and locked, like the
    // forced distance muting; the working copy keeps the player's OWN choice (never
    // overwritten), so it applies again as soon as the player leaves that server.
    // Imposee par le serveur -> cochee et verrouillee ; le choix du joueur n'est pas ecrase.
    if (hRangeUnitCheck) {
        CheckDlgButton(hConfigDialog, IDC_CHECK_RANGEUNIT,
            ui_ranges_in_foundations() ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(hRangeUnitCheck, ui_ranges_forced_by_server() ? FALSE : TRUE);
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
        newWhisper = ui_correct_to_hub_limits(ui_cfg()->distanceWhisper, hubMinimumWhisper, hubMaximumWhisper, "Whisper");
        newNormal = ui_correct_to_hub_limits(ui_cfg()->distanceNormal, hubMinimumNormal, hubMaximumNormal, "Normal");
        newShout = ui_correct_to_hub_limits(ui_cfg()->distanceShout, hubMinimumShout, hubMaximumShout, "Shout");
    }

    // Update UI fields according to current context (zone or user) | Mettre à jour les champs UI (zone ou utilisateur)
    // A field is rewritten ONLY when it does not have the keyboard focus and its text
    // really differs: rewriting the field the user is typing in (to "%.1f") made the
    // caret jump and scrambled the digits after every keystroke.
    // Un champ n'est reecrit que s'il n'a pas le focus ET que son texte differe.
    ui_set_distance_edit_text(hDistanceWhisperEdit, newWhisper, rewriteFocused);
    ui_set_distance_edit_text(hDistanceNormalEdit, newNormal, rewriteFocused);
    ui_set_distance_edit_text(hDistanceShoutEdit, newShout, rewriteFocused);

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

// 091: the display unit changed (checkbox clicked): show every distance field and status line
// again from the working copy (meters) in the new unit. Nothing is written to g_config or to
// disk, and no field text is parsed back - the working copy is only ever READ here, so toggling
// the box back and forth cannot drift the stored meters. Focused fields are rewritten too.
// 091 : l'unite d'affichage a change : tout est reaffiche depuis la copie de travail (metres).
void ui_range_unit_changed(void) {
    ui_range_unit_refresh(); // adopt the new unit first (also when this is not the Plugin Settings tab)
    s_forceEditRewrite = TRUE;
    lastInterfaceUpdate = 0; // bypass the 100 ms throttle: the click must show at once
    updateDynamicInterface();
    s_forceEditRewrite = FALSE;
    forceInterfaceRefresh(); // status lines (updateDynamicInterface skips them while it is updating)
}

// ============================================================================
