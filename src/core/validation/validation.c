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

// MODULE 3: VALIDATION AND LIMITS
// EN: Zone polygon tests, distance validation, player-in-zone checks for soundproof/reverb.
// FR: Tests polygones de zone, validation distance, vérifications joueur-en-zone pour insonorisation/reverb.
// ============================================================================
// Check if distance limits should be applied | Fonction pour déterminer si les limites de distance doivent être appliquées
BOOL shouldApplyDistanceLimits() {
    if (!isConnectedToServer) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Distance limits DISABLED: Not connected to server");
        }
        return FALSE;
    }

    if (rootChannelID == -1) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Distance limits DISABLED: No hub channel found");
        }
        return FALSE;
    }

    if (!hubDescriptionAvailable) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Distance limits DISABLED: Hub description not available");
        }
        return FALSE;
    }

    if (!hubForceDistanceBasedMuting) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Distance limits DISABLED: ForceDistanceBasedMuting = FALSE - user has full control");
        }
        return FALSE;
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Distance limits ENABLED: ForceDistanceBasedMuting = TRUE");
    }
    return TRUE;
}

// Check connection status | Fonction pour vérifier l'état de la connexion
void checkConnectionStatus() {
    ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastConnectionCheck < 2000) {
        return;
    }
    lastConnectionCheck = currentTime;

    const BOOL wasConnected = isConnectedToServer;
    isConnectedToServer = ts3_is_connected() ? TRUE : FALSE;

    if (wasConnected != isConnectedToServer) {
        if (!isConnectedToServer) {
            hubDescriptionAvailable = FALSE;
            hubLimitsActive = FALSE;
        }
    }
    plugin_ui_sync_live_state();
}

// Determine if value should be validated | Déterminer si une valeur doit être validée
BOOL shouldValidateValue(float value, float minimum, float maximum, const char* modeName) {
    int digitCount = countSignificantDigits(value);

    int minDigits = countSignificantDigits(minimum);
    int maxDigits = countSignificantDigits(maximum);
    int requiredDigits = (maxDigits > minDigits) ? maxDigits : minDigits;

    if (maximum >= 10.0f) {
        requiredDigits = 2;
    }

    if (digitCount < requiredDigits) {
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg),
                "FILTER: %s value %.1f has %d digits, need %d digits - IGNORING",
                modeName, value, digitCount, requiredDigits);
            mumbleAPI.log(ownID, logMsg);
        }
        return FALSE;
    }

    if (enableLogGeneral) {
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg),
            "FILTER: %s value %.1f has %d digits, need %d digits - VALIDATING",
            modeName, value, digitCount, requiredDigits);
        mumbleAPI.log(ownID, logMsg);
    }

    return TRUE;
}

// Validate and correct distance in real time | Fonction pour valider et corriger une distance en temps réel
float validateDistanceValue(float value, float minimum, float maximum, const char* modeName) {
    if (!shouldApplyDistanceLimits()) {
        return value;
    }

    if (value < minimum) {
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "%s distance auto-corrected: %.1f -> %.1f (below minimum)",
                modeName, value, minimum);
            mumbleAPI.log(ownID, logMsg);
        }
        return minimum;
    }
    else if (value > maximum) {
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "%s distance auto-corrected: %.1f -> %.1f (above maximum)",
                modeName, value, maximum);
            mumbleAPI.log(ownID, logMsg);
        }
        return maximum;
    }
    return value;
}

void validatePlayerDistances() {
    // Override limits if player is in a zone | Surcharger les limites si le joueur est dans une zone
    // Only flag limits as active and update UI if in a zone | Marquer les limites comme actives et MAJ interface si en zone
    if (currentZoneIndex != -1) {
        hubLimitsActive = TRUE;
        applyDistanceToAllPlayers(); // Recalculate based on zone values via getVoiceDistanceForMode | Recalculer via getVoiceDistanceForMode
        if (hConfigDialog && IsWindow(hConfigDialog)) updateDynamicInterface();
        return; // Global variables remain unchanged (stay in RAM as user settings) | Les variables globales restent inchangées (restent en RAM comme réglages utilisateur)
    }

    // Check if limits should be applied | Vérifier si les limites doivent être appliquées
    if (!shouldApplyDistanceLimits()) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Distance validation SKIPPED: ForceDistanceBasedMuting = FALSE - user has full control of distances");
        }
        hubLimitsActive = FALSE;

        // Ensure messages reflect user freedom | S'assurer que les messages reflètent la liberté de l'utilisateur
        if (hConfigDialog && IsWindow(hConfigDialog)) {
            updateDynamicInterface();
        }

        // Trigger immediate channel management check | Déclencher immédiatement une vérification de gestion des canaux
        if (enableAutomaticChannelChange && channelManagementActive) {
            lastChannelCheck = 0;
            if (TEMP) {
                mumbleAPI.log(ownID, "IMMEDIATE: Triggering channel management check");
            }
        }

        return;
    }

    hubLimitsActive = TRUE;

    BOOL distanceChanged = FALSE;
    float originalWhisper = distanceWhisper;
    float originalNormal = distanceNormal;
    float originalShout = distanceShout;

    // Validate whisper distance | Valider la distance whisper
    if (distanceWhisper < hubMinimumWhisper) {
        distanceWhisper = (float)hubMinimumWhisper;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Whisper distance corrected: %.1f -> %.1f (below minimum)",
                originalWhisper, distanceWhisper);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else if (distanceWhisper > hubMaximumWhisper) {
        distanceWhisper = (float)hubMaximumWhisper;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Whisper distance corrected: %.1f -> %.1f (above maximum)",
                originalWhisper, distanceWhisper);
            mumbleAPI.log(ownID, logMsg);
        }
    }

    // Validate normal distance | Valider la distance normale
    if (distanceNormal < hubMinimumNormal) {
        distanceNormal = (float)hubMinimumNormal;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Normal distance corrected: %.1f -> %.1f (below minimum)",
                originalNormal, distanceNormal);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else if (distanceNormal > hubMaximumNormal) {
        distanceNormal = (float)hubMaximumNormal;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Normal distance corrected: %.1f -> %.1f (above maximum)",
                originalNormal, distanceNormal);
            mumbleAPI.log(ownID, logMsg);
        }
    }

    // Validate shout distance | Valider la distance shout
    if (distanceShout < hubMinimumShout) {
        distanceShout = (float)hubMinimumShout;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Shout distance corrected: %.1f -> %.1f (below minimum)",
                originalShout, distanceShout);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else if (distanceShout > hubMaximumShout) {
        distanceShout = (float)hubMaximumShout;
        distanceChanged = TRUE;
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "Shout distance corrected: %.1f -> %.1f (above maximum)",
                originalShout, distanceShout);
            mumbleAPI.log(ownID, logMsg);
        }
    }

    // Save and apply changes if necessary | Sauvegarder et appliquer les changements si nécessaire
    if (distanceChanged) {
        saveVoiceSettings();
        applyDistanceToAllPlayers();

        if (enableLogGeneral) {
            char summaryMsg[256];
            snprintf(summaryMsg, sizeof(summaryMsg),
                "Distance validation complete: Whisper=%.1f, Normal=%.1f, Shout=%.1f",
                distanceWhisper, distanceNormal, distanceShout);
            mumbleAPI.log(ownID, summaryMsg);
        }
    }
    else {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "All player distances are valid - no corrections needed");
        }
    }
}

// Helper: 2D point-in-polygon (ray casting, even-odd rule)
BOOL isPointInPolygon(float px, float pz, float x1, float z1, float x2, float z2, float x3, float z3, float x4, float z4) {
    /* Ray-casting (even-odd rule): robust for convex quads regardless of winding. */
    const float vx[4] = { x1, x2, x3, x4 };
    const float vz[4] = { z1, z2, z3, z4 };
    int crossings = 0;

    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        float xi = vx[i], zi = vz[i];
        float xj = vx[j], zj = vz[j];

        if ((zi <= pz && zj > pz) || (zj <= pz && zi > pz)) {
            float dz = zj - zi;
            if (fabsf(dz) > 1e-6f) {
                float intersectX = xi + (pz - zi) * (xj - xi) / dz;
                if (px < intersectX) {
                    crossings++;
                }
            }
        }
    }
    return (crossings & 1) != 0;
}

/* One 3D zone box: horizontal quadrilateral + GroundY..TopY height.
   xzFloor=1: X-Z floor, Y height (original Mumble plugin layout).
   xzFloor=0: X-Y floor, Z height (Conan / UE; Z1..Z4 in config are world Y).
   If GroundY and TopY are both 0, vertical bounds are not enforced. */
BOOL zoneContainsPoint(const Zone* z, float px, float py, float pz, int xzFloor) {
    const float hEps = 1.0f;
    float hMin = z->groundY < z->topY ? z->groundY : z->topY;
    float hMax = z->groundY > z->topY ? z->groundY : z->topY;
    float height = xzFloor ? py : pz;
    float horizA = px;
    float horizB = xzFloor ? pz : py;

    if (z->groundY != 0.0f || z->topY != 0.0f) {
        if (height < (hMin - hEps) || height > (hMax + hEps)) {
            return FALSE;
        }
    }
    return isPointInPolygon(horizA, horizB,
        z->x1, z->z1, z->x2, z->z2, z->x3, z->z3, z->x4, z->z4);
}

static int getPlayerZoneAtScale(float playerX, float playerY, float playerZ) {
    for (size_t i = 0; i < zoneCount; i++) {
        const Zone* z = &zones[i];

        if (z->x1 == 0.0f && z->x2 == 0.0f && z->x3 == 0.0f && z->x4 == 0.0f
            && z->z1 == 0.0f && z->z2 == 0.0f && z->z3 == 0.0f && z->z4 == 0.0f) {
            continue;
        }

        /* Legacy X-Z floor + Y height first (original Mumble plugin layout). */
        if (zoneContainsPoint(z, playerX, playerY, playerZ, 1)) {
            return (int)i;
        }
        /* Conan/UE: X-Y floor, Z height. */
        if (zoneContainsPoint(z, playerX, playerY, playerZ, 0)) {
            return (int)i;
        }
    }
    return -1;
}

/* Check if a player is inside a 3D zone (4-corner horizontal polygon + height).
   Tries meters, UE centimeters, and legacy scale factors (Pos.txt / channel config). */
int getPlayerZone(float playerX, float playerY, float playerZ) {
    if (zoneCount == 0) {
        return -1;
    }

    static const float scales[] = { 1.0f, 100.0f, 0.01f };
    for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
        float sc = scales[si];
        int idx = getPlayerZoneAtScale(playerX * sc, playerY * sc, playerZ * sc);
        if (idx >= 0) {
            return idx;
        }
    }
    return -1;
}

// ============================================================================
