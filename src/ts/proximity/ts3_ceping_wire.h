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
 * CEPING is a low-rate liveness heartbeat (about once per second, armed by the
 * position tick). The receiver watches the sequence per peer: a forward jump
 * of more than 1 means heartbeats (and therefore the position stream they
 * accompany) were lost. It never changes how anything sounds — it is
 * diagnostics only.
 *
 * The receive-side bookkeeping (ceping_track_observe) is pure too: it decides
 * whether a message moves the per-peer sequence forward, how many heartbeats
 * it skipped, and whether that deserves a (rate limited) log line.
 */

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CEPING_CMD_PREFIX       "CEPING:"
#define CEPING_PREFIX_LEN       7
#define CEPING_PAYLOAD_VERSION  1
#define CEPING_CMD_MAX          28
#define CEPING_SEQ_MASK         0xFFFFFFFFUL

/* A sequence that is BEHIND the last one by at most this many steps is a
   reordered/late packet and is ignored. Further behind means the peer
   restarted its counter (plugin reload) and we adopt the new baseline. */
#define CEPING_REORDER_WINDOW   16UL

/* At most one loss log line per peer per this many milliseconds; the loss
   events in between are only counted. */
#define CEPING_LOSS_LOG_MIN_MS  1000ULL

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

/* ---- receive-side tracking (pure) ------------------------------------------- */

/* Per-peer receive state. All-zero is the valid "nothing seen yet" state. */
typedef struct CepingTrack {
    unsigned long lastSeq;          /* newest sequence accepted so far */
    unsigned long suppressed;       /* loss events not logged since lastLogMs */
    unsigned long long lastLogMs;   /* when the last loss line was allowed */
    unsigned char hasSeq;           /* 1 = lastSeq is valid */
    unsigned char hasLogged;        /* 1 = lastLogMs is valid */
} CepingTrack;

/* What one received heartbeat means. */
typedef struct CepingObserved {
    unsigned long missed;       /* heartbeats this message skipped (0 = none) */
    unsigned long suppressed;   /* earlier loss events folded into this line */
    int log;                    /* 1 = write ONE loss line now */
} CepingObserved;

/* Feed one received sequence number into the peer's state.
 *
 * - First message: becomes the baseline, nothing is lost.
 * - Forward step (0 < diff <= 0x7FFFFFFF, wraparound-safe): lastSeq advances;
 *   diff-1 heartbeats were skipped.
 * - Duplicate or a SMALL step backwards (reordered packet): ignored, lastSeq
 *   stays — otherwise 10,12,11,13 would report a phantom loss twice.
 * - A LARGE step backwards (more than CEPING_REORDER_WINDOW): the peer
 *   restarted its counter, so lastSeq is re-based without claiming a loss.
 *   Without this a restarted peer would be ignored until its counter caught
 *   up with the old value.
 * - Loss lines are rate limited per peer (CEPING_LOSS_LOG_MIN_MS); a peer
 *   that sends wild sequence numbers can therefore not flood the log. The
 *   events in between are counted in `suppressed`. */
static inline CepingObserved ceping_track_observe(CepingTrack* track,
    unsigned long seq, unsigned long long nowMs) {
    CepingObserved out;
    unsigned long diff;

    out.missed = 0;
    out.suppressed = 0;
    out.log = 0;
    if (!track) {
        return out;
    }

    seq &= CEPING_SEQ_MASK;
    if (!track->hasSeq) {
        track->lastSeq = seq;
        track->hasSeq = 1;
        return out;
    }

    diff = (seq - track->lastSeq) & CEPING_SEQ_MASK;
    if (diff == 0) {
        return out; /* duplicate */
    }
    if (diff > 0x7FFFFFFFUL) {
        const unsigned long back = (track->lastSeq - seq) & CEPING_SEQ_MASK;
        if (back > CEPING_REORDER_WINDOW) {
            track->lastSeq = seq; /* peer restarted — adopt the new baseline */
        }
        return out; /* reordered / late: lastSeq stays */
    }

    track->lastSeq = seq;
    out.missed = diff - 1;
    if (out.missed == 0) {
        return out;
    }

    if (!track->hasLogged || nowMs < track->lastLogMs
        || nowMs - track->lastLogMs >= CEPING_LOSS_LOG_MIN_MS) {
        out.log = 1;
        out.suppressed = track->suppressed;
        track->suppressed = 0;
        track->hasLogged = 1;
        track->lastLogMs = nowMs;
    }
    else if (track->suppressed != ULONG_MAX) {
        track->suppressed++;
    }
    return out;
}

#endif /* TS_PROXIMITY_TS3_CEPING_WIRE_H */
