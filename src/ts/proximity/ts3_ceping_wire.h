#ifndef TS_PROXIMITY_TS3_CEPING_WIRE_H
#define TS_PROXIMITY_TS3_CEPING_WIRE_H

/*
 * CEPING wire codec — pure, header-only (see doku/module/ce-protokoll.md).
 *
 * Kept free of Win32, the TS SDK and globals so the host unit test
 * (tests/ceping_wire_test.c) verifies the shipping code, not a copy.
 *
 * Thread contract: none — every function is pure (arguments in, result out).
 *
 * Wire text: "CEPING:<payloadVersion>;<seq>"
 *   payloadVersion  currently 1; a receiver drops what it does not know
 *   seq             uint32 heartbeat counter, wraps around at 2^32
 *
 * CEPING is a liveness heartbeat on the same 30 ms poll tick as CEPOS.
 * The receiver watches the sequence per peer: a forward jump of more than 1
 * means heartbeats (and therefore the position stream they accompany) were
 * lost. It never changes how anything sounds — it is diagnostics only.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CEPING_CMD_PREFIX       "CEPING:"
#define CEPING_PREFIX_LEN       7
#define CEPING_PAYLOAD_VERSION  1
#define CEPING_CMD_MAX          28
#define CEPING_SEQ_MASK         0xFFFFFFFFUL

/* Build "CEPING:1;<seq>". Returns 1 on success, 0 on a buffer too small
   (out is then left as an empty string). */
static inline int ceping_wire_format(unsigned long seq, char* out, size_t outSize) {
    int written;

    if (!out || outSize == 0) {
        return 0;
    }
    out[0] = '\0';
    written = snprintf(out, outSize, "%s%d;%lu", CEPING_CMD_PREFIX,
        CEPING_PAYLOAD_VERSION, seq & CEPING_SEQ_MASK);
    if (written <= 0 || (size_t)written >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Parse the payload AFTER the "CEPING:" prefix. Returns 1 only for a payload
   version we know and a numeric sequence; remote input is never trusted.
   A trailing ";<extra>" is accepted and ignored (forward compatible). */
static inline int ceping_wire_parse(const char* payload, unsigned long* outSeq) {
    long version;
    unsigned long seq;
    char* end = NULL;

    if (!payload || !outSeq) {
        return 0;
    }

    version = strtol(payload, &end, 10);
    if (end == payload || *end != ';') {
        return 0;
    }
    if (version != CEPING_PAYLOAD_VERSION) {
        return 0;
    }

    payload = end + 1;
    seq = strtoul(payload, &end, 10);
    if (end == payload) {
        return 0; /* empty or non-numeric sequence */
    }
    if (*end != '\0' && *end != ';') {
        return 0; /* trailing junk that is not an additive field */
    }

    *outSeq = seq & CEPING_SEQ_MASK;
    return 1;
}

/* Lost heartbeats between two sequence numbers, wraparound-safe over uint32.
   Returns 0 for the expected next-in-line (prev+1) and also for a duplicate
   or a backward/reordered sequence (treated as "nothing missed"), so a late
   or repeated packet never reports a phantom loss. */
static inline unsigned long ceping_seq_gap(unsigned long prev, unsigned long next) {
    unsigned long diff = (next - prev) & CEPING_SEQ_MASK;

    if (diff == 0 || diff > 0x7FFFFFFFUL) {
        return 0; /* duplicate, or a backward step from reordering */
    }
    return diff - 1;
}

#endif /* TS_PROXIMITY_TS3_CEPING_WIRE_H */
