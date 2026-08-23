#ifndef TS_PROXIMITY_TS3_CEAUTH_WIRE_H
#define TS_PROXIMITY_TS3_CEAUTH_WIRE_H

/*
 * CEAUTH wire codec — pure, header-only (see doku/module/ce-protokoll.md).
 *
 * Kept free of Win32, the TS SDK and globals so the host unit test
 * (tests/ceauth_wire_test.c) verifies the shipping code, not a copy.
 *
 * Thread contract: none — every function is pure (arguments in, result out).
 *
 * Wire text: "CEAUTH:<payloadVersion>;<steamID64>"
 *   payloadVersion  currently 1; a receiver drops what it does not know
 *   steamID64       the player's public SteamID64 (decimal, non-zero)
 *
 * SOFT identity only. The SteamID is self-reported, unauthenticated and
 * trivially spoofable — it is fine for display and player association, but it
 * MUST NOT be used for any trust, permission or anti-cheat decision.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CEAUTH_CMD_PREFIX       "CEAUTH:"
#define CEAUTH_PREFIX_LEN       7
#define CEAUTH_PAYLOAD_VERSION  1
#define CEAUTH_CMD_MAX          40

/* Build "CEAUTH:1;<steamID64>". Returns 1 on success, 0 on a zero id or a
   buffer too small (out is then left as an empty string). */
static inline int ceauth_wire_format(unsigned long long steamID, char* out, size_t outSize) {
    int written;

    if (!out || outSize == 0) {
        return 0;
    }
    out[0] = '\0';
    if (steamID == 0ULL) {
        return 0; /* 0 means "no identity" — never put it on the wire */
    }
    written = snprintf(out, outSize, "%s%d;%llu", CEAUTH_CMD_PREFIX,
        CEAUTH_PAYLOAD_VERSION, steamID);
    if (written <= 0 || (size_t)written >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Parse the payload AFTER the "CEAUTH:" prefix. Returns 1 only for a payload
   version we know and a non-zero numeric id; remote input is never trusted.
   A trailing ";<extra>" is accepted and ignored (forward compatible). */
static inline int ceauth_wire_parse(const char* payload, unsigned long long* outSteamID) {
    long version;
    unsigned long long steamID;
    char* end = NULL;

    if (!payload || !outSteamID) {
        return 0;
    }

    version = strtol(payload, &end, 10);
    if (end == payload || *end != ';') {
        return 0;
    }
    if (version != CEAUTH_PAYLOAD_VERSION) {
        return 0;
    }

    payload = end + 1;
    steamID = strtoull(payload, &end, 10);
    if (end == payload) {
        return 0; /* empty or non-numeric id */
    }
    if (*end != '\0' && *end != ';') {
        return 0; /* trailing junk that is not an additive field */
    }
    if (steamID == 0ULL) {
        return 0;
    }

    *outSteamID = steamID;
    return 1;
}

#endif /* TS_PROXIMITY_TS3_CEAUTH_WIRE_H */
