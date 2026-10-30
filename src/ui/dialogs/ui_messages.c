#include "plugin_internal.h"
#include "ui/config/ui_config_state.h"
#include "ui/plugin_ui_compat.h"
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
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <process.h>
#include <ole2.h>

// MODULE 15: UI - STATUS MESSAGES
// EN: In-game chat status lines (connection, zone, errors) shown to the player.
// FR: Lignes de statut chat ingame (connexion, zone, erreurs) affichées au joueur.
// ============================================================================

// 091: the status lines below show a distance in the display unit (meters, or Conan
// foundations when the player ticked the box / the server forces it). Everything they
// COMPARE stays in meters (working copy and hub limits); only the numbers that are
// printed are converted. Hub limits are shown rounded inward (min up, max down) so
// every shown value is also valid in meters. In meter mode all three are identities.
// 091 : seuls les nombres AFFICHES sont convertis ; les comparaisons restent en metres.
static float ui_shown(float meters) {
    return range_from_meters(meters, ui_ranges_in_foundations());
}

// Both limits of a mode are needed: a range narrower than one 0.1 F step is shown
// rounded to nearest instead of inverted (range_limits_display).
static float ui_shown_min(double hubMinMeters, double hubMaxMeters) {
    float lo = 0.0f;
    range_limits_display((float)hubMinMeters, (float)hubMaxMeters, ui_ranges_in_foundations(), &lo, NULL);
    return lo;
}

static float ui_shown_max(double hubMinMeters, double hubMaxMeters) {
    float hi = 0.0f;
    range_limits_display((float)hubMinMeters, (float)hubMaxMeters, ui_ranges_in_foundations(), NULL, &hi);
    return hi;
}

// Update distance message in real time | Fonction pour mettre à jour les messages de distance en temps réel
void updateDistanceMessage(HWND hMessageControl, float currentValue, float minimum, float maximum, const char* modeName) {
    if (!hMessageControl || !IsWindow(hMessageControl)) return;

    SetWindowTextW(hMessageControl, L"");
    InvalidateRect(hMessageControl, NULL, TRUE);

    wchar_t message[256] = L"";

    if (!shouldApplyDistanceLimits()) {
        swprintf(message, 256, L"INFO: %S distance: %.1f (No server limits - free range)", modeName, currentValue);
    }
    else if (currentValue < minimum) {
        swprintf(message, 256, L"WARNING: %S: %.1f too low (min: %.1f) - auto-corrected", modeName, currentValue, minimum);
    }
    else if (currentValue > maximum) {
        swprintf(message, 256, L"WARNING: %S: %.1f too high (max: %.1f) - auto-corrected", modeName, currentValue, maximum);
    }

    SetWindowTextW(hMessageControl, message);
    ShowWindow(hMessageControl, SW_SHOW);
    UpdateWindow(hMessageControl);
}

// Update distance muting message | Fonction pour mettre à jour le message de muting
void updateDistanceMutingMessage() {
    if (!hDistanceMutingMessage || !IsWindow(hDistanceMutingMessage)) return;

    SetWindowTextW(hDistanceMutingMessage, L"");

    wchar_t message[256] = L"";

    if (!shouldApplyDistanceLimits()) {
        if (ui_cfg()->enableDistanceMuting) {
            swprintf(message, 256, L"INFO: Distance-based muting: Enabled (No server restrictions)");
        }
        else {
            swprintf(message, 256, L"INFO: Distance-based muting: Disabled (No server restrictions)");
        }
    }
    else if (hubForceDistanceBasedMuting) {
        if (ui_cfg()->enableDistanceMuting) {
            swprintf(message, 256, L"LOCKED: Distance-based muting: FORCED by server (cannot disable)");
        }
        else {
            swprintf(message, 256, L"LOCKED: Distance-based muting: FORCED by server - enabling automatically");
        }
    }
    else {
        if (ui_cfg()->enableDistanceMuting) {
            swprintf(message, 256, L"OK: Distance-based muting: Enabled (user choice)");
        }
        else {
            swprintf(message, 256, L"INFO: Distance-based muting: Disabled (user choice)");
        }
    }

    InvalidateRect(hDistanceMutingMessage, NULL, TRUE);
    SetWindowTextW(hDistanceMutingMessage, message);
    ShowWindow(hDistanceMutingMessage, SW_SHOW);
    UpdateWindow(hDistanceMutingMessage);
}

void updatePositionalAudioMessage() {
    if (!hPositionalAudioMessage || !IsWindow(hPositionalAudioMessage)) return;

    SetWindowTextW(hPositionalAudioMessage, L"");
    InvalidateRect(hPositionalAudioMessage, NULL, TRUE);

    wchar_t message[400] = L"";

    if (!shouldApplyDistanceLimits()) {
        if (enableAutoAudioSettings) {
            swprintf(message, 400, L"INFO: Positional audio: Enabled (No server restrictions)");
        }
        else {
            swprintf(message, 400, L"INFO: Positional audio: Disabled (No server restrictions)");
        }
    }
    else if (hubForcePositionalAudio) {
        if (enableAutoAudioSettings) {
            swprintf(message, 400,
                L"ACTIVE: Positional audio FORCED - MinDist=%.1f MaxDist=%.1f MaxVol=%.0f%% (Scientific model)",
                hubAudioMinDistance, hubAudioMaxDistance, hubAudioMaxVolume);
        }
        else {
            swprintf(message, 400, L"LOCKED: Positional audio: FORCED by server - enabling automatically");
        }
    }
    else {
        if (enableAutoAudioSettings) {
            swprintf(message, 400,
                L"OK: Positional audio enabled - MinDist=%.1f MaxDist=%.1f MaxVol=%.0f%% (Scientific model)",
                hubAudioMinDistance, hubAudioMaxDistance, hubAudioMaxVolume);
        }
        else {
            swprintf(message, 400, L"INFO: Positional audio: Disabled (user choice)");
        }
    }

    SetWindowTextW(hPositionalAudioMessage, message);
    ShowWindow(hPositionalAudioMessage, SW_SHOW);
    UpdateWindow(hPositionalAudioMessage);
}

void updateChannelSwitchingMessage() {
    if (!hChannelSwitchingMessage || !IsWindow(hChannelSwitchingMessage)) return;

    SetWindowTextW(hChannelSwitchingMessage, L"");

    wchar_t message[256] = L"";

    if (!shouldApplyDistanceLimits()) {
        if (ui_cfg()->enableAutomaticChannelChange) {
            swprintf(message, 256, L"INFO: Automatic channel switching: Enabled (No server restrictions)");
        }
        else {
            swprintf(message, 256, L"INFO: Automatic channel switching: Disabled (No server restrictions)");
        }
    }
    else if (hubForceAutomaticChannelSwitching) {
        if (ui_cfg()->enableAutomaticChannelChange) {
            swprintf(message, 256, L"LOCKED: Automatic channel switching: FORCED by server (cannot disable)");
        }
        else {
            swprintf(message, 256, L"LOCKED: Automatic channel switching: FORCED by server - enabling automatically");
        }
    }
    else {
        if (ui_cfg()->enableAutomaticChannelChange) {
            swprintf(message, 256, L"OK: Automatic channel switching: Enabled (user choice)");
        }
        else {
            swprintf(message, 256, L"INFO: Automatic channel switching: Disabled (user choice)");
        }
    }

    InvalidateRect(hChannelSwitchingMessage, NULL, TRUE);
    SetWindowTextW(hChannelSwitchingMessage, message);
    ShowWindow(hChannelSwitchingMessage, SW_SHOW);
    UpdateWindow(hChannelSwitchingMessage);
}


// Create consolidated message for each mode | Fonction pour créer un message consolidé unique pour chaque mode
void updateConsolidatedDistanceMessages() {
    if (isUpdatingInterface) return;
    if (!hConfigDialog || !IsWindow(hConfigDialog)) return;

    BOOL limitsActive = shouldApplyDistanceLimits();
    const wchar_t* unit = range_unit_name_w(ui_ranges_in_foundations()); // 091: "meters" / "foundations"

    // Whisper consolidated message | Message whisper consolidé
    if (hDistanceWhisperMessage && IsWindow(hDistanceWhisperMessage)) {
        SetWindowTextW(hDistanceWhisperMessage, L"");
        InvalidateRect(hDistanceWhisperMessage, NULL, TRUE);

        wchar_t whisperMsg[300] = L"";

        if (currentZoneIndex != -1) {
            swprintf(whisperMsg, 300, L"Whisper: %.1f %ls (ZONE: %S - Range: %.1f-%.1f)",
                ui_shown(ui_cfg()->distanceWhisper), unit, zones[currentZoneIndex].name,
                ui_shown(zones[currentZoneIndex].whisperDist), ui_shown(zones[currentZoneIndex].whisperDist));
        }
        else if (!limitsActive) {
            swprintf(whisperMsg, 300, L"Whisper: %.1f %ls (Free range - no server limits)",
                ui_shown(ui_cfg()->distanceWhisper), unit);
        }
        else {
            if (ui_cfg()->distanceWhisper < hubMinimumWhisper) {
                swprintf(whisperMsg, 300, L"Whisper: %.1f→%.1f %ls (Auto-corrected: below minimum %.1f)",
                    ui_shown(ui_cfg()->distanceWhisper), ui_shown_min(hubMinimumWhisper, hubMaximumWhisper), unit,
                    ui_shown_min(hubMinimumWhisper, hubMaximumWhisper));
            }
            else if (ui_cfg()->distanceWhisper > hubMaximumWhisper) {
                swprintf(whisperMsg, 300, L"Whisper: %.1f→%.1f %ls (Auto-corrected: above maximum %.1f)",
                    ui_shown(ui_cfg()->distanceWhisper), ui_shown_max(hubMinimumWhisper, hubMaximumWhisper), unit,
                    ui_shown_max(hubMinimumWhisper, hubMaximumWhisper));
            }
            else {
                swprintf(whisperMsg, 300, L"Whisper: %.1f %ls (Valid range: %.1f-%.1f)",
                    ui_shown(ui_cfg()->distanceWhisper), unit,
                    ui_shown_min(hubMinimumWhisper, hubMaximumWhisper), ui_shown_max(hubMinimumWhisper, hubMaximumWhisper));
            }
        }

        SetWindowTextW(hDistanceWhisperMessage, whisperMsg);
        ShowWindow(hDistanceWhisperMessage, SW_SHOW);
        UpdateWindow(hDistanceWhisperMessage);
    }

    // Normal consolidated message | Message normal consolidé
    if (hDistanceNormalMessage && IsWindow(hDistanceNormalMessage)) {
        SetWindowTextW(hDistanceNormalMessage, L"");
        InvalidateRect(hDistanceNormalMessage, NULL, TRUE);

        wchar_t normalMsg[300] = L"";

        if (currentZoneIndex != -1) {
            swprintf(normalMsg, 300, L"Normal: %.1f %ls (ZONE: %S - Range: %.1f-%.1f)",
                ui_shown(ui_cfg()->distanceNormal), unit, zones[currentZoneIndex].name,
                ui_shown(zones[currentZoneIndex].normalDist), ui_shown(zones[currentZoneIndex].normalDist));
        }
        else if (!limitsActive) {
            swprintf(normalMsg, 300, L" Normal: %.1f %ls (Free range - no server limits)",
                ui_shown(ui_cfg()->distanceNormal), unit);
        }
        else {
            if (ui_cfg()->distanceNormal < hubMinimumNormal) {
                swprintf(normalMsg, 300, L"Normal: %.1f→%.1f %ls (Auto-corrected: below minimum %.1f)",
                    ui_shown(ui_cfg()->distanceNormal), ui_shown_min(hubMinimumNormal, hubMaximumNormal), unit,
                    ui_shown_min(hubMinimumNormal, hubMaximumNormal));
            }
            else if (ui_cfg()->distanceNormal > hubMaximumNormal) {
                swprintf(normalMsg, 300, L"Normal: %.1f→%.1f %ls (Auto-corrected: above maximum %.1f)",
                    ui_shown(ui_cfg()->distanceNormal), ui_shown_max(hubMinimumNormal, hubMaximumNormal), unit,
                    ui_shown_max(hubMinimumNormal, hubMaximumNormal));
            }
            else {
                swprintf(normalMsg, 300, L"Normal: %.1f %ls (Valid range: %.1f-%.1f)",
                    ui_shown(ui_cfg()->distanceNormal), unit,
                    ui_shown_min(hubMinimumNormal, hubMaximumNormal), ui_shown_max(hubMinimumNormal, hubMaximumNormal));
            }
        }

        SetWindowTextW(hDistanceNormalMessage, normalMsg);
        ShowWindow(hDistanceNormalMessage, SW_SHOW);
        UpdateWindow(hDistanceNormalMessage);
    }

    // Shout consolidated message | Message shout consolidé
    if (hDistanceShoutMessage && IsWindow(hDistanceShoutMessage)) {
        SetWindowTextW(hDistanceShoutMessage, L"");
        InvalidateRect(hDistanceShoutMessage, NULL, TRUE);

        wchar_t shoutMsg[300] = L"";

        if (currentZoneIndex != -1) {
            swprintf(shoutMsg, 300, L"Shout: %.1f %ls (ZONE: %S - Range: %.1f-%.1f)",
                ui_shown(ui_cfg()->distanceShout), unit, zones[currentZoneIndex].name,
                ui_shown(zones[currentZoneIndex].shoutDist), ui_shown(zones[currentZoneIndex].shoutDist));
        }
        else if (!limitsActive) {
            swprintf(shoutMsg, 300, L" Shout: %.1f %ls (Free range - no server limits)",
                ui_shown(ui_cfg()->distanceShout), unit);
        }
        else {
            if (ui_cfg()->distanceShout < hubMinimumShout) {
                swprintf(shoutMsg, 300, L"Shout: %.1f→%.1f %ls (Auto-corrected: below minimum %.1f)",
                    ui_shown(ui_cfg()->distanceShout), ui_shown_min(hubMinimumShout, hubMaximumShout), unit,
                    ui_shown_min(hubMinimumShout, hubMaximumShout));
            }
            else if (ui_cfg()->distanceShout > hubMaximumShout) {
                swprintf(shoutMsg, 300, L"Shout: %.1f→%.1f %ls (Auto-corrected: above maximum %.1f)",
                    ui_shown(ui_cfg()->distanceShout), ui_shown_max(hubMinimumShout, hubMaximumShout), unit,
                    ui_shown_max(hubMinimumShout, hubMaximumShout));
            }
            else {
                swprintf(shoutMsg, 300, L"Shout: %.1f %ls (Valid range: %.1f-%.1f)",
                    ui_shown(ui_cfg()->distanceShout), unit,
                    ui_shown_min(hubMinimumShout, hubMaximumShout), ui_shown_max(hubMinimumShout, hubMaximumShout));
            }
        }

        SetWindowTextW(hDistanceShoutMessage, shoutMsg);
        ShowWindow(hDistanceShoutMessage, SW_SHOW);
        UpdateWindow(hDistanceShoutMessage);
    }
}

// Display server limits separately | Fonction pour afficher les limites serveur distinctement
void updateServerLimitMessages() {
    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "DEBUG: updateServerLimitMessages() FUNCTION CALLED SUCCESSFULLY!");
    }

    if (isUpdatingInterface) return;
    if (!hConfigDialog || !IsWindow(hConfigDialog)) return;

    BOOL limitsActive = shouldApplyDistanceLimits();

    if (enableLogGeneral) {
        char debugMsg[128];
        snprintf(debugMsg, sizeof(debugMsg), "DEBUG: updateServerLimitMessages - limitsActive = %s",
            limitsActive ? "TRUE" : "FALSE");
        mumbleAPI.log(ownID, debugMsg);
    }
}

// Display status message in interface | Afficher un message de statut dans l'interface
//
// 087: the colour (red for errors, green for success) is chosen from the message TEXT by
// the dialog's WM_CTLCOLORSTATIC handler (control 600) when the control repaints, so
// isError is not needed here. The two SendMessage(hStatusMessage, WM_CTLCOLORSTATIC,
// GetDC(...)) calls that used to follow were removed: they sent the message to the
// static control itself (no visible effect) and leaked one DC per status message.
// 087 : la couleur vient du TEXTE (WM_CTLCOLORSTATIC du dialogue). Les deux appels
// SendMessage(..., GetDC(...)) etaient sans effet et fuyaient un DC par message.
void showStatusMessage(const wchar_t* message, BOOL isError) {
    (void)isError;
    if (hStatusMessage) {
        SetWindowTextW(hStatusMessage, message);
        InvalidateRect(hStatusMessage, NULL, TRUE);
        UpdateWindow(hStatusMessage);

        SetTimer(hConfigDialog, 2, 5000, NULL);
    }
}

void showConfigSavedNotice(HWND parent, const wchar_t* statusText) {
    showStatusMessage(statusText, FALSE);
    MessageBoxW(parent,
        L"Configuration saved successfully.",
        L"Save Configuration",
        MB_OK | MB_ICONINFORMATION);
}

// Clear status message | Effacer le message de statut
void clearStatusMessage() {
    if (hStatusMessage) {
        SetWindowTextW(hStatusMessage, L"");
    }
}

// Generate dynamic distance limit message | Générer un message dynamique sur les limites de distance
void showDynamicDistanceLimitMessage() {

    char dynamicMsg[1024];
    snprintf(dynamicMsg, sizeof(dynamicMsg),
        "Voice distance information:\n"
        "Server Maximum Audio Distance: %.1f meters\n"
        "Current Settings:\n"
        "  • Whisper: %.1f meters %s\n"
        "  • Normal: %.1f meters %s\n"
        "  • Shout: %.1f meters %s\n"
        "Note: Distances are automatically limited by server settings. "
        "Each server may have different maximum distances.",
        serverMaximumAudioDistance,
        ui_cfg()->distanceWhisper, (ui_cfg()->distanceWhisper == serverMaximumAudioDistance) ? "(LIMITED)" : "",
        ui_cfg()->distanceNormal, (ui_cfg()->distanceNormal == serverMaximumAudioDistance) ? "(LIMITED)" : "",
        ui_cfg()->distanceShout, (ui_cfg()->distanceShout == serverMaximumAudioDistance) ? "(LIMITED)" : "");

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, dynamicMsg);
    }
}

// ============================================================================
