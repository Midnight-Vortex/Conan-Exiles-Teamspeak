#include "plugin_internal.h"

#include <math.h>
#include <stdio.h>

/*
 * Hub-enforced distance limits for the F10 dialog (ui_dynamic, ui_messages).
 * Zone polygon tests live in core/proximity/zone_resolve.c — not here.
 * UI / settings thread only.
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
    if (value > maximum) {
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
