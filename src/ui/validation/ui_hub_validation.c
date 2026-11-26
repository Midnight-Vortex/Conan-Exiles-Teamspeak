#include "plugin_internal.h"
#include "ui/config/ui_config_logic.h"

#include <math.h>
#include <stdio.h>

/*
 * Hub-enforced distance limits for the F10 dialog (ui_dynamic, ui_messages).
 * Zone polygon tests live in core/proximity/zone_resolve.c — not here.
 * UI / settings thread only.
 *
 * 108: a hub limit of 0 means "not set" (only one side of a range given in the
 * Root description, or none). The comparison itself lives in
 * ui/config/ui_config_logic.h (ui_hub_below_min / ui_hub_above_max /
 * ui_hub_required_digits) so the host test runs the exact rule used here.
 */

extern int countSignificantDigits(float value);

BOOL shouldApplyDistanceLimits(void) {
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
            mumbleAPI.log(ownID,
                "Distance limits DISABLED: ForceDistanceBasedMuting = FALSE - user has full control");
        }
        return FALSE;
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Distance limits ENABLED: ForceDistanceBasedMuting = TRUE");
    }
    return TRUE;
}

BOOL shouldValidateValue(float value, float minimum, float maximum, const char* modeName) {
    const int digitCount = countSignificantDigits(value);
    const int minDigits = countSignificantDigits(minimum);
    const int maxDigits = countSignificantDigits(maximum);
    // 108: a missing maximum (0) no longer counts like a one-digit maximum.
    const int requiredDigits = ui_hub_required_digits(minimum, maximum, minDigits, maxDigits);

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

float validateDistanceValue(float value, float minimum, float maximum, const char* modeName) {
    if (!shouldApplyDistanceLimits()) {
        return value;
    }

    // 108: 0 = limit not set, the same rule as the runtime (voice_mode_get_distance)
    // and the Save check (ui_dist_parse). Comparing against the raw 0 pulled every
    // value to "maximum 0" when the Root description set only a minimum.
    if (ui_hub_below_min(value, minimum)) {
        if (enableLogGeneral) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "%s distance auto-corrected: %.1f -> %.1f (below minimum)",
                modeName, value, minimum);
            mumbleAPI.log(ownID, logMsg);
        }
        return minimum;
    }
    if (ui_hub_above_max(value, maximum)) {
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
