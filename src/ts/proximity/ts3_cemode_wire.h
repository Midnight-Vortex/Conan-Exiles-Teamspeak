#ifndef TS_PROXIMITY_TS3_CEMODE_WIRE_H
#define TS_PROXIMITY_TS3_CEMODE_WIRE_H

/*
 * CEMODE wire codec — pure, header-only (see doku/module/ce-protokoll.md).
 *
 * These are the exact functions the plugin ships with, kept free of Win32, the
 * TS SDK and globals so the host unit test (tests/cemode_wire_test.c) verifies
 * production code instead of a copy.
 *
 * Thread contract: none — every function is pure (arguments in, result out).
 *
 * Wire text: "CEMODE:<payloadVersion>;<mode>;<distanceDm>"
 *   payloadVersion  currently 1; a receiver drops what it does not know
 *   mode            0 = whisper, 1 = normal, 2 = shout
 *   distanceDm      voice distance in DECIMETERS (meters x 10), 0..10000
 *
 * The distance travels as an integer on purpose: writing or reading a decimal
 * point goes through the CRT locale, so a German client would send "60,0"
 * while an English one expects "60.0" and would drop the message. One decimal
 * of range is plenty for a display value.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CEMODE_CMD_PREFIX       "CEMODE:"
#define CEMODE_PREFIX_LEN       7
#define CEMODE_PAYLOAD_VERSION  1
#define CEMODE_CMD_MAX          40
#define CEMODE_MODE_MIN         0
#define CEMODE_MODE_MAX         2
#define CEMODE_DISTANCE_MAX_DM  10000L  /* 1000 m */

/* Meters -> wire decimeters, saturating. NaN and infinities are folded into
   the valid range here so no caller has to pull in <math.h>. */
static inline long cemode_wire_distance_dm(float meters) {
    if (meters != meters) { /* NaN */
        return 0;
    }
    if (meters <= 0.0f) {
        return 0;
    }
    if (meters >= 1000.0f) { /* also catches +inf */
        return CEMODE_DISTANCE_MAX_DM;
    }
    return (long)(meters * 10.0f + 0.5f);
}

/* Build the full command text. Returns 1 on success, 0 on invalid input or a
   buffer too small (out is then left as an empty string). */
static inline int cemode_wire_format(int mode, long distanceDm, char* out, size_t outSize) {
    int written;

    if (!out || outSize == 0) {
        return 0;
    }
    out[0] = '\0';
    if (mode < CEMODE_MODE_MIN || mode > CEMODE_MODE_MAX) {
        return 0;
    }
    if (distanceDm < 0 || distanceDm > CEMODE_DISTANCE_MAX_DM) {
        return 0;
    }

    written = snprintf(out, outSize, "%s%d;%d;%ld", CEMODE_CMD_PREFIX,
        CEMODE_PAYLOAD_VERSION, mode, distanceDm);
    if (written <= 0 || (size_t)written >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Parse the payload AFTER the "CEMODE:" prefix. Returns 1 only for a payload
   version we know, a mode in range and a distance in range — remote input is
   never trusted.

   Trailing ";<extra>" after the distance is accepted and ignored, so a future
   client may append a field without breaking us. A change to the MEANING of
   the first three fields must bump the payload version instead. */
static inline int cemode_wire_parse(const char* payload, int* outMode, long* outDistanceDm) {
    long values[3];
    char* end = NULL;

    if (!payload || !outMode || !outDistanceDm) {
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        values[i] = strtol(payload, &end, 10);
        if (end == payload) {
            return 0; /* empty or non-numeric field */
        }
        if (i < 2) {
            if (*end != ';') {
                return 0;
            }
        }
        else if (*end != '\0' && *end != ';') {
            return 0; /* trailing junk that is not an additive field */
        }
        payload = end + 1;
    }

    if (values[0] != CEMODE_PAYLOAD_VERSION) {
        return 0;
    }
    if (values[1] < CEMODE_MODE_MIN || values[1] > CEMODE_MODE_MAX) {
        return 0;
    }
    if (values[2] < 0 || values[2] > CEMODE_DISTANCE_MAX_DM) {
        return 0;
    }

    *outMode = (int)values[1];
    *outDistanceDm = values[2];
    return 1;
}

#endif /* TS_PROXIMITY_TS3_CEMODE_WIRE_H */
