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
/* v2 (072). Written before pct/tick, so a reader that sees a fresh tick sees
   the matching fields (a torn read across two updates 100 ms apart is harmless). */
static volatile long g_occKind[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_occRoom[OCCLUSION_MAX_CLIENT_ID];   /* 0..100 */
static volatile long g_occRelay[OCCLUSION_MAX_CLIENT_ID][3]; /* Unreal cm */
static volatile long g_occRelayValid[OCCLUSION_MAX_CLIENT_ID];

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
        /* v2 tail (072): ":<kind>:<room>:<x>,<y>,<z>" - every part optional. */
        long kind = pct > 0 ? OCCLUSION_KIND_TRANSMIT : OCCLUSION_KIND_NONE;
        long room = 0;
        long relay[3] = { 0, 0, 0 };
        long relayValid = 0;
        if (*p == ':') {
            p++;
            if (*p == 'N' || *p == 'n') {
                kind = OCCLUSION_KIND_NONE;
                p++;
            }
            else if (*p == 'T' || *p == 't') {
                kind = OCCLUSION_KIND_TRANSMIT;
                p++;
            }
            else if (*p == 'D' || *p == 'd') {
                kind = OCCLUSION_KIND_DIFFRACT;
                p++;
            }
            if (*p == ':') {
                p++;
                const long r = strtol(p, &end, 10);
                if (end != p) {
                    room = r < 0 ? 0 : (r > 100 ? 100 : r);
                    p = end;
                }
            }
            if (*p == ':') {
                p++;
                int k = 0;
                for (; k < 3; k++) {
                    relay[k] = strtol(p, &end, 10);
                    if (end == p) {
                        break;
                    }
                    p = end;
                    if (k < 2) {
                        if (*p != ',') {
                            break;
                        }
                        p++;
                    }
                }
                relayValid = (k == 3 && kind == OCCLUSION_KIND_DIFFRACT) ? 1 : 0;
            }
            /* skip anything unknown up to the entry separator */
            while (*p && *p != ';') {
                p++;
            }
        }
        if (kind == OCCLUSION_KIND_NONE) {
            pct = 0;
        }
        if (cid > 0 && cid < OCCLUSION_MAX_CLIENT_ID) {
            InterlockedExchange(&g_occKind[cid], kind);
            InterlockedExchange(&g_occRoom[cid], room);
            InterlockedExchange(&g_occRelay[cid][0], relay[0]);
            InterlockedExchange(&g_occRelay[cid][1], relay[1]);
            InterlockedExchange(&g_occRelay[cid][2], relay[2]);
            InterlockedExchange(&g_occRelayValid[cid], relayValid);
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

int occlusion_get_info(unsigned int clientID, unsigned long long nowMs, OcclusionInfo* out) {
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (clientID == 0 || clientID >= OCCLUSION_MAX_CLIENT_ID) {
        return 0;
    }
    const long tick = InterlockedCompareExchange(&g_occTick[clientID], 0, 0);
    if (tick == 0 || (unsigned long)nowMs - (unsigned long)tick > OCCLUSION_STALE_MS) {
        return 0;
    }
    out->amount = (float)InterlockedCompareExchange(&g_occPct[clientID], 0, 0) / 100.0f;
    out->kind = (int)InterlockedCompareExchange(&g_occKind[clientID], 0, 0);
    out->room = (float)InterlockedCompareExchange(&g_occRoom[clientID], 0, 0) / 100.0f;
    out->relayValid = InterlockedCompareExchange(&g_occRelayValid[clientID], 0, 0) != 0;
    if (out->relayValid) {
        /* Unreal (X, Y, Z-up, cm) -> plugin (x, y-up, z, m), as in occlusion_format_probes. */
        out->relayX = (float)InterlockedCompareExchange(&g_occRelay[clientID][0], 0, 0) / 100.0f;
        out->relayZ = (float)InterlockedCompareExchange(&g_occRelay[clientID][1], 0, 0) / 100.0f;
        out->relayY = (float)InterlockedCompareExchange(&g_occRelay[clientID][2], 0, 0) / 100.0f;
    }
    if (out->kind == OCCLUSION_KIND_NONE) {
        out->amount = 0.0f;
    }
    return 1;
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
        InterlockedExchange(&g_occKind[i], 0);
        InterlockedExchange(&g_occRoom[i], 0);
        InterlockedExchange(&g_occRelayValid[i], 0);
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

/* ---- 072: mod parameters (forwarded as "occcfg") ---------------------------- */

static volatile long g_occMod[OCCLUSION_MOD_PARAM_COUNT] = { 80, 150, 400, 150, 300, 30, 60, 60, 70, 80, 60, 2 };

void occlusion_set_mod_params(const int* values) {
    for (int i = 0; i < OCCLUSION_MOD_PARAM_COUNT; i++) {
        InterlockedExchange(&g_occMod[i], values ? (long)occlusion_mod_param_clamp(i, values[i]) : occlusion_mod_param_default(i));
    }
}

int occlusion_append_cfg_json(char* out, int outSize) {
    if (!out || outSize < 32) {
        return 0;
    }
    const int len = (int)strlen(out);
    if (len < 2 || out[len - 1] != '}') {
        return 0;
    }
    char cfg[160];
    int n = snprintf(cfg, sizeof(cfg), ",\"occcfg\":\"%d", OCCLUSION_PROTOCOL_VERSION);
    for (int i = 0; i < OCCLUSION_MOD_PARAM_COUNT && n > 0 && n < (int)sizeof(cfg); i++) {
        n += snprintf(cfg + n, sizeof(cfg) - (size_t)n, ",%ld", InterlockedCompareExchange(&g_occMod[i], 0, 0));
    }
    if (n <= 0 || n >= (int)sizeof(cfg) - 3 || len - 1 + n + 3 > outSize) {
        return 0;
    }
    /* overwrite the closing brace, then close the string and the object again */
    memcpy(out + len - 1, cfg, (size_t)n);
    memcpy(out + len - 1 + n, "\"}", 3);
    return 1;
}

/* ---- 072: plugin-side sound parameters -------------------------------------- */

static volatile long g_occAttackMs = OCCLUSION_ATTACK_MS_DEFAULT;
static volatile long g_occReleaseMs = OCCLUSION_RELEASE_MS_DEFAULT;
static volatile long g_occDiffractionPct = OCCLUSION_DIFFRACTION_DEFAULT;
static volatile long g_occReverbPct = OCCLUSION_REVERB_DEFAULT;

static long occ_clamp_long(float v, long maxV) {
    if (!(v > 0.0f)) {
        return 0; /* also NaN */
    }
    return v > (float)maxV ? maxV : lroundf(v);
}

void occlusion_set_sound_params(float attackMs, float releaseMs, float diffractionPct, float reverbPct) {
    InterlockedExchange(&g_occAttackMs, occ_clamp_long(attackMs, OCCLUSION_SMOOTH_MS_MAX));
    InterlockedExchange(&g_occReleaseMs, occ_clamp_long(releaseMs, OCCLUSION_SMOOTH_MS_MAX));
    InterlockedExchange(&g_occDiffractionPct, occ_clamp_long(diffractionPct, OCCLUSION_DIFFRACTION_MAX));
    InterlockedExchange(&g_occReverbPct, occ_clamp_long(reverbPct, OCCLUSION_REVERB_MAX));
}

float occlusion_get_attack_ms(void) {
    return (float)InterlockedCompareExchange(&g_occAttackMs, 0, 0);
}

float occlusion_get_release_ms(void) {
    return (float)InterlockedCompareExchange(&g_occReleaseMs, 0, 0);
}

float occlusion_get_diffraction(void) {
    return (float)InterlockedCompareExchange(&g_occDiffractionPct, 0, 0) / 100.0f;
}

float occlusion_get_reverb(void) {
    return (float)InterlockedCompareExchange(&g_occReverbPct, 0, 0) / 100.0f;
}
