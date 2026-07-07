#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
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

// MODULE 6: AUDIO - VOLUME CALCULATIONS
// EN: Legacy volume multiplier + plugin_proximity_volume_context for proximity_math curve.
// FR: Multiplicateur volume legacy + plugin_proximity_volume_context pour courbe proximity_math.
// ============================================================================

// Calculate volume multiplier by distance (legacy curve; hub path uses proximity_math)
// Calcule le multiplicateur de volume par distance (courbe legacy ; hub utilise proximity_math)
float calculateVolumeMultiplier(float distance, float maxDistance) {
    if (distance >= maxDistance) {
        return 0.0f;
    }

    if (distance <= 1.0f) {
        return 1.0f;
    }

    float minDistance = 1.0f;
    float normalizedDistance = (distance - minDistance) / (maxDistance - minDistance);
    normalizedDistance = fmaxf(0.0f, fminf(normalizedDistance, 1.0f));

    float volumeMultiplier = 1.0f / (1.0f + 2.0f * normalizedDistance * normalizedDistance);

    if (volumeMultiplier < 0.0f) {
        volumeMultiplier = 0.0f;
    }

    return volumeMultiplier;
}

// Hub + zone aware volume curve (duplicate of proximity_math for legacy callers)
// Courbe volume hub + zone (doublon de proximity_math pour appels legacy)
float calculateVolumeMultiplierWithHubSettings(float distance, float voiceDistance) {
    ProximityVolumeContext ctx = plugin_proximity_volume_context();
    return proximity_calculate_volume_with_hub(distance, voiceDistance, &ctx);
}

// Build ProximityVolumeContext from current hub/zone/race state.
// Construit ProximityVolumeContext depuis l'état hub/zone/race actuel.
ProximityVolumeContext plugin_proximity_volume_context(void) {
    ProximityVolumeContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hubAudioMinDistance = hubAudioMinDistance;
    ctx.hubAudioMaxVolume = hubAudioMaxVolume;
    ctx.currentZoneIndex = currentZoneIndex;
    ctx.currentPlayerRaceIndex = currentPlayerRaceIndex;
    ctx.currentListenAddDistance = currentListenAddDistance;
    if (currentZoneIndex != -1 && (size_t)currentZoneIndex < zoneCount) {
        ctx.zoneAudioMinDistance = zones[currentZoneIndex].audioMinDistance;
        ctx.zoneAudioMaxVolume = zones[currentZoneIndex].audioMaxVolume;
    }
    return ctx;
}

// ============================================================================

// MODULE 10: AUDIO - APPLY DISTANCE TO ALL PLAYERS
// EN: Mumble-only full apply loop; TS build uses ts3_proximity_apply instead.
// FR: Boucle apply complète Mumble uniquement ; build TS utilise ts3_proximity_apply.
// ============================================================================
// Apply distance changes to all connected players | Appliquer les changements de distance à tous les joueurs connectés
void applyDistanceToAllPlayers() {
    if (!enableDistanceMuting) return;
    if (!ts3_plugin_is_proximity_active()) {
        ts3_plugin_apply_proximity_volumes_force();
        return;
    }
    /* Ingame: distance + stereo pan come from calculateLocalPositionalAudio /
       apply_proximity — never flatten L/R via setUserAdaptiveVolume here. */
    ts3_plugin_apply_proximity_volumes_force();
}

// ============================================================================
