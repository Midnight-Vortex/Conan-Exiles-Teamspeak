/*
 * occlusion.c — PROTOTYPE (branch test/occlusion-prototype).
 *
 * Stores per-client occlusion percentages reported by the game mod and
 * builds the probe list the mod has to trace. Contract and data flow: see
 * occlusion.h and doku/aenderungen/061-occlusion-machbarkeit.md.
 *
 * Thread contract:
 *   - HTTP listener thread: occlusion_store_from_text, occlusion_format_probes
 *     (g_probeSnap is only touched there — the listener handles one
 *     connection at a time).
 *   - Callback thread (audio_compute_client): occlusion_get + occlusion_apply.
 *   - Value and timestamp are two independent atomics; a torn pair only means
 *     one recompute uses a value that is at most one POST (~30 ms) old.
 *   - No TS API, no locks besides player_table's own short copy lock.
 */
#include "core/proximity/occlusion.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile long g_occPct[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_occTick[OCCLUSION_MAX_CLIENT_ID]; /* low 32 bits of ms tick */

/* HTTP thread only. */
static PlayerEntry g_probeSnap[PLAYER_TABLE_MAX_PLAYERS];

int occlusion_store_from_text(const char* body, unsigned long long nowMs) {
    if (!body) {
        return -1;
    }
    const char* p = strstr(body, "OCC=");
    if (!p) {
        return -1;
    }
    p += 4;

    int stored = 0;
    for (;;) {
        char* end = NULL;
        const long cid = strtol(p, &end, 10);
        if (end == p || *end != ':') {
            break;
        }
        p = end + 1;
        long pct = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        /* Tolerate a decimal tail ("80.0" / "80,0") — integer part counts. */
        if (*p == '.' || *p == ',') {
            p++;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        if (cid > 0 && cid < OCCLUSION_MAX_CLIENT_ID) {
            InterlockedExchange(&g_occPct[cid], pct);
            InterlockedExchange(&g_occTick[cid], (long)(unsigned long)nowMs);
            stored++;
        }
        if (*p != ';') {
            break;
        }
        p++;
    }
    return stored;
}

float occlusion_get(unsigned int clientID, unsigned long long nowMs) {
    if (clientID == 0 || clientID >= OCCLUSION_MAX_CLIENT_ID) {
        return 0.0f;
    }
    const long tick = InterlockedCompareExchange(&g_occTick[clientID], 0, 0);
    const long pct = InterlockedCompareExchange(&g_occPct[clientID], 0, 0);
    if (tick == 0 || pct <= 0) {
        return 0.0f;
    }
    /* Unsigned 32-bit difference survives the tick wrap. */
    const unsigned long age = (unsigned long)nowMs - (unsigned long)tick;
    if (age > OCCLUSION_STALE_MS) {
        return 0.0f;
    }
    return (float)pct / 100.0f;
}

void occlusion_apply(float occ, float strength, float* gain, float* cutoffHz) {
    if (!(occ > 0.0f) || !(strength > 0.0f) || !gain || !cutoffHz) {
        return;
    }
    if (occ > 1.0f) {
        occ = 1.0f;
    }
    /* Raising the 8.0.8 multiplier to the power "strength" scales its dB loss
       linearly; strength 1 leaves the old curve bit-identical. */
    const float base = 1.0f - occ * (1.0f - OCCLUSION_FULL_GAIN);
    *gain *= (strength == 1.0f) ? base : powf(base, strength);
    /* Exponential sweep 19 kHz -> OCCLUSION_FULL_CUTOFF (perceptually even). */
    float target = 19000.0f * powf(OCCLUSION_FULL_CUTOFF / 19000.0f, occ * strength);
    if (target < OCCLUSION_MIN_CUTOFF) {
        target = OCCLUSION_MIN_CUTOFF;
    }
    if (target < *cutoffHz) {
        *cutoffHz = target;
    }
}

int occlusion_format_probes(char* out, int outSize,
    float localX, float localY, float localZ) {
    if (!out || outSize < 32) {
        return 0;
    }
    const int count = player_table_snapshot(g_probeSnap, PLAYER_TABLE_MAX_PLAYERS);

    /* Pick the nearest OCCLUSION_MAX_PROBES players inside the probe range
       (small insertion list — count is bounded by the table size). */
    int pick[OCCLUSION_MAX_PROBES];
    float pickDist[OCCLUSION_MAX_PROBES];
    int picked = 0;
    for (int i = 0; i < count; i++) {
        const float d = prox_distance(localX, localY, localZ,
            g_probeSnap[i].x, g_probeSnap[i].y, g_probeSnap[i].z);
        if (d > OCCLUSION_PROBE_RANGE_M) {
            continue;
        }
        int slot = picked;
        while (slot > 0 && pickDist[slot - 1] > d) {
            slot--;
        }
        if (slot >= OCCLUSION_MAX_PROBES) {
            continue;
        }
        const int last = (picked < OCCLUSION_MAX_PROBES) ? picked : OCCLUSION_MAX_PROBES - 1;
        for (int k = last; k > slot; k--) {
            pick[k] = pick[k - 1];
            pickDist[k] = pickDist[k - 1];
        }
        pick[slot] = i;
        pickDist[slot] = d;
        if (picked < OCCLUSION_MAX_PROBES) {
            picked++;
        }
    }

    int n = snprintf(out, (size_t)outSize, "{\"ok\":true,\"probe\":\"");
    if (n <= 0 || n >= outSize) {
        return 0;
    }
    for (int k = 0; k < picked; k++) {
        const PlayerEntry* e = &g_probeSnap[pick[k]];
        /* Plugin is Y-up (meters); Unreal is Z-up (centimeters). */
        const int w = snprintf(out + n, (size_t)(outSize - n), "%s%u,%ld,%ld,%ld",
            k ? ";" : "", (unsigned)e->clientID,
            lroundf(e->x * 100.0f), lroundf(e->z * 100.0f), lroundf(e->y * 100.0f));
        if (w <= 0 || w >= outSize - n - 3) {
            break; /* keep room for the closing "\"}" */
        }
        n += w;
    }
    const int w = snprintf(out + n, (size_t)(outSize - n), "\"}");
    if (w <= 0 || w >= outSize - n) {
        return 0;
    }
    return n + w;
}

void occlusion_clear(void) {
    for (int i = 0; i < OCCLUSION_MAX_CLIENT_ID; i++) {
        InterlockedExchange(&g_occPct[i], 0);
        InterlockedExchange(&g_occTick[i], 0);
    }
}

static volatile long g_occEnabled = 0; /* root-channel switch, default off */

void occlusion_set_enabled(int enabled) {
    const long next = enabled ? 1 : 0;
    const long prev = InterlockedExchange(&g_occEnabled, next);
    if (prev && !next) {
        occlusion_clear();
    }
}

int occlusion_is_enabled(void) {
    return InterlockedCompareExchange(&g_occEnabled, 0, 0) != 0;
}

static volatile long g_occIntensityPct = OCCLUSION_INTENSITY_DEFAULT;

void occlusion_set_intensity(float percent) {
    if (!(percent > 0.0f)) {
        percent = 0.0f; /* also NaN */
    }
    if (percent > (float)OCCLUSION_INTENSITY_MAX) {
        percent = (float)OCCLUSION_INTENSITY_MAX;
    }
    InterlockedExchange(&g_occIntensityPct, lroundf(percent));
}

float occlusion_get_strength(void) {
    return (float)InterlockedCompareExchange(&g_occIntensityPct, 0, 0) / 100.0f;
}
