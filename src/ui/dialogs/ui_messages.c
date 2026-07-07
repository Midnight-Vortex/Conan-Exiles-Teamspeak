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

// MODULE 15: UI - STATUS MESSAGES
// EN: In-game chat status lines (connection, zone, errors) shown to the player.
// FR: Lignes de statut chat ingame (connexion, zone, erreurs) affichées au joueur.
// ============================================================================

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
        if (enableDistanceMuting) {
            swprintf(message, 256, L"INFO: Distance-based muting: Enabled (No server restrictions)");
        }
        else {
            swprintf(message, 256, L"INFO: Distance-based muting: Disabled (No server restrictions)");
        }
    }
    else if (hubForceDistanceBasedMuting) {
        if (enableDistanceMuting) {
            swprintf(message, 256, L"LOCKED: Distance-based muting: FORCED by server (cannot disable)");
        }
        else {
            swprintf(message, 256, L"LOCKED: Distance-based muting: FORCED by server - enabling automatically");
        }
    }
    else {
        if (enableDistanceMuting) {
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
        if (enableAutomaticChannelChange) {
            swprintf(message, 256, L"INFO: Automatic channel switching: Enabled (No server restrictions)");
        }
        else {
            swprintf(message, 256, L"INFO: Automatic channel switching: Disabled (No server restrictions)");
        }
    }
    else if (hubForceAutomaticChannelSwitching) {
        if (enableAutomaticChannelChange) {
            swprintf(message, 256, L"LOCKED: Automatic channel switching: FORCED by server (cannot disable)");
        }
        else {
            swprintf(message, 256, L"LOCKED: Automatic channel switching: FORCED by server - enabling automatically");
        }
    }
    else {
        if (enableAutomaticChannelChange) {
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

    // Whisper consolidated message | Message whisper consolidé
    if (hDistanceWhisperMessage && IsWindow(hDistanceWhisperMessage)) {
        SetWindowTextW(hDistanceWhisperMessage, L"");
        InvalidateRect(hDistanceWhisperMessage, NULL, TRUE);

        wchar_t whisperMsg[300] = L"";

        if (currentZoneIndex != -1) {
            swprintf(whisperMsg, 300, L"Whisper: %.1f meters (ZONE: %S - Range: %.1f-%.1f)",
                distanceWhisper, zones[currentZoneIndex].name, zones[currentZoneIndex].whisperDist, zones[currentZoneIndex].whisperDist);
        }
        else if (!limitsActive) {
            swprintf(whisperMsg, 300, L"Whisper: %.1f meters (Free range - no server limits)", distanceWhisper);
        }
        else {
            if (distanceWhisper < hubMinimumWhisper) {
                swprintf(whisperMsg, 300, L"Whisper: %.1f→%.1f meters (Auto-corrected: below minimum %.1f)",
                    distanceWhisper, (float)hubMinimumWhisper, (float)hubMinimumWhisper);
            }
            else if (distanceWhisper > hubMaximumWhisper) {
                swprintf(whisperMsg, 300, L"Whisper: %.1f→%.1f meters (Auto-corrected: above maximum %.1f)",
                    distanceWhisper, (float)hubMaximumWhisper, (float)hubMaximumWhisper);
            }
            else {
                swprintf(whisperMsg, 300, L"Whisper: %.1f meters (Valid range: %.1f-%.1f)",
                    distanceWhisper, (float)hubMinimumWhisper, (float)hubMaximumWhisper);
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
            swprintf(normalMsg, 300, L"Normal: %.1f meters (ZONE: %S - Range: %.1f-%.1f)",
                distanceNormal, zones[currentZoneIndex].name, zones[currentZoneIndex].normalDist, zones[currentZoneIndex].normalDist);
        }
        else if (!limitsActive) {
            swprintf(normalMsg, 300, L" Normal: %.1f meters (Free range - no server limits)", distanceNormal);
        }
        else {
            if (distanceNormal < hubMinimumNormal) {
                swprintf(normalMsg, 300, L"Normal: %.1f→%.1f meters (Auto-corrected: below minimum %.1f)",
                    distanceNormal, (float)hubMinimumNormal, (float)hubMinimumNormal);
            }
            else if (distanceNormal > hubMaximumNormal) {
                swprintf(normalMsg, 300, L"Normal: %.1f→%.1f meters (Auto-corrected: above maximum %.1f)",
                    distanceNormal, (float)hubMaximumNormal, (float)hubMaximumNormal);
            }
            else {
                swprintf(normalMsg, 300, L"Normal: %.1f meters (Valid range: %.1f-%.1f)",
                    distanceNormal, (float)hubMinimumNormal, (float)hubMaximumNormal);
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
            swprintf(shoutMsg, 300, L"Shout: %.1f meters (ZONE: %S - Range: %.1f-%.1f)",
                distanceShout, zones[currentZoneIndex].name, zones[currentZoneIndex].shoutDist, zones[currentZoneIndex].shoutDist);
        }
        else if (!limitsActive) {
            swprintf(shoutMsg, 300, L" Shout: %.1f meters (Free range - no server limits)", distanceShout);
        }
        else {
            if (distanceShout < hubMinimumShout) {
                swprintf(shoutMsg, 300, L"Shout: %.1f→%.1f meters (Auto-corrected: below minimum %.1f)",
                    distanceShout, (float)hubMinimumShout, (float)hubMinimumShout);
            }
            else if (distanceShout > hubMaximumShout) {
                swprintf(shoutMsg, 300, L"Shout: %.1f→%.1f meters (Auto-corrected: above maximum %.1f)",
                    distanceShout, (float)hubMaximumShout, (float)hubMaximumShout);
            }
            else {
                swprintf(shoutMsg, 300, L"Shout: %.1f meters (Valid range: %.1f-%.1f)",
                    distanceShout, (float)hubMinimumShout, (float)hubMaximumShout);
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
void showStatusMessage(const wchar_t* message, BOOL isError) {
    if (hStatusMessage) {
        SetWindowTextW(hStatusMessage, message);

        // Change color based on message type | Changer la couleur selon le type de message
        if (isError) {
            SendMessage(hStatusMessage, WM_CTLCOLORSTATIC, (WPARAM)GetDC(hStatusMessage), (LPARAM)hStatusMessage);
        }
        else {
            SendMessage(hStatusMessage, WM_CTLCOLORSTATIC, (WPARAM)GetDC(hStatusMessage), (LPARAM)hStatusMessage);
        }

        SetTimer(hConfigDialog, 2, 5000, NULL);
    }
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
        distanceWhisper, (distanceWhisper == serverMaximumAudioDistance) ? "(LIMITED)" : "",
        distanceNormal, (distanceNormal == serverMaximumAudioDistance) ? "(LIMITED)" : "",
        distanceShout, (distanceShout == serverMaximumAudioDistance) ? "(LIMITED)" : "");

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, dynamicMsg);
    }
}

// ============================================================================
