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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile long g_occPct[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_occTick[OCCLUSION_MAX_CLIENT_ID]; /* low 32 bits of ms tick */
/* v2 (072). One entry = pct, tick, kind, room, relay; published as a unit with a
   per-client seqlock (odd = write in progress). The writers - HTTP listener
   (occlusion_store_from_text) and the switch-off path (occlusion_clear) - are
   serialized by g_occWriteLock, so the seqlock always has a single writer; the
   readers (audio compute) never take the lock, they retry. */
static volatile long g_occSeq[OCCLUSION_MAX_CLIENT_ID];
static CRITICAL_SECTION g_occWriteLock;
static INIT_ONCE g_occWriteLockOnce = INIT_ONCE_STATIC_INIT;
static volatile long g_occEnabled = 0; /* root-channel switch, default off */

static BOOL CALLBACK occ_write_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_occWriteLock);
    return TRUE;
}

static void occ_write_lock(void) {
    InitOnceExecuteOnce(&g_occWriteLockOnce, occ_write_lock_init_once, NULL, NULL);
    EnterCriticalSection(&g_occWriteLock);
}
static volatile long g_occKind[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_occRoom[OCCLUSION_MAX_CLIENT_ID];   /* 0..100 */
static volatile long g_occRelay[OCCLUSION_MAX_CLIENT_ID][3]; /* Unreal cm */
static volatile long g_occRelayValid[OCCLUSION_MAX_CLIENT_ID];
/* B1 smoothing state (072), see occlusion_smooth. */
static volatile long g_smTick[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smGain[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smLogCut[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smRelayW[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smRoom[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smRelayKnown[OCCLUSION_MAX_CLIENT_ID];
static volatile long g_smRelay[OCCLUSION_MAX_CLIENT_ID][3];

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

    /* Writers serialized; a store that lost the race against switch-off is dropped
       instead of surviving the clear with a fresh timestamp (review 2026-10-20). */
    occ_write_lock();
    if (InterlockedCompareExchange(&g_occEnabled, 0, 0) == 0) {
        LeaveCriticalSection(&g_occWriteLock);
        return 0;
    }
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
            InterlockedIncrement(&g_occSeq[cid]); /* odd: write in progress */
            InterlockedExchange(&g_occKind[cid], kind);
            InterlockedExchange(&g_occRoom[cid], room);
            InterlockedExchange(&g_occRelay[cid][0], relay[0]);
            InterlockedExchange(&g_occRelay[cid][1], relay[1]);
            InterlockedExchange(&g_occRelay[cid][2], relay[2]);
            InterlockedExchange(&g_occRelayValid[cid], relayValid);
            InterlockedExchange(&g_occPct[cid], pct);
            InterlockedExchange(&g_occTick[cid], (long)(unsigned long)nowMs);
            InterlockedIncrement(&g_occSeq[cid]); /* even: stable */
            stored++;
        }
        if (*p != ';') {
            break;
        }
        p++;
    }
    LeaveCriticalSection(&g_occWriteLock);
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
    /* Seqlock read: the writer publishes the whole entry between two increments;
       a mixed read (e.g. old kind D with a new zeroed relay point) is retried. */
    long tick = 0, pct = 0, kind = 0, room = 0, relayValid = 0, r0 = 0, r1 = 0, r2 = 0;
    int stable = 0;
    for (int attempt = 0; attempt < 8 && !stable; attempt++) {
        const long seqBefore = InterlockedCompareExchange(&g_occSeq[clientID], 0, 0);
        if (seqBefore & 1) {
            continue;
        }
        tick = InterlockedCompareExchange(&g_occTick[clientID], 0, 0);
        pct = InterlockedCompareExchange(&g_occPct[clientID], 0, 0);
        kind = InterlockedCompareExchange(&g_occKind[clientID], 0, 0);
        room = InterlockedCompareExchange(&g_occRoom[clientID], 0, 0);
        relayValid = InterlockedCompareExchange(&g_occRelayValid[clientID], 0, 0);
        r0 = InterlockedCompareExchange(&g_occRelay[clientID][0], 0, 0);
        r1 = InterlockedCompareExchange(&g_occRelay[clientID][1], 0, 0);
        r2 = InterlockedCompareExchange(&g_occRelay[clientID][2], 0, 0);
        stable = InterlockedCompareExchange(&g_occSeq[clientID], 0, 0) == seqBefore;
    }
    if (!stable || tick == 0 || (unsigned long)nowMs - (unsigned long)tick > OCCLUSION_STALE_MS) {
        return 0;
    }
    out->amount = (float)pct / 100.0f;
    out->kind = (int)kind;
    out->room = (float)room / 100.0f;
    out->relayValid = relayValid != 0;
    if (out->relayValid) {
        /* Unreal (X, Y, Z-up, cm) -> plugin (x, y-up, z, m), as in occlusion_format_probes. */
        out->relayX = (float)r0 / 100.0f;
        out->relayZ = (float)r1 / 100.0f;
        out->relayY = (float)r2 / 100.0f;
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
    occ_write_lock(); /* no HTTP store can interleave (see g_occWriteLock) */
    for (int i = 0; i < OCCLUSION_MAX_CLIENT_ID; i++) {
        InterlockedIncrement(&g_occSeq[i]);
        InterlockedExchange(&g_occTick[i], 0);
        InterlockedExchange(&g_occPct[i], 0);
        InterlockedExchange(&g_occKind[i], 0);
        InterlockedExchange(&g_occRoom[i], 0);
        InterlockedExchange(&g_occRelayValid[i], 0);
        InterlockedIncrement(&g_occSeq[i]);
        InterlockedExchange(&g_smTick[i], 0);
        InterlockedExchange(&g_smRelayKnown[i], 0);
    }
    LeaveCriticalSection(&g_occWriteLock);
}

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

void occlusion_apply_diffraction(float amount, float strength, float* gain, float* cutoffHz) {
    if (!(amount > 0.0f) || !(strength > 0.0f) || !gain || !cutoffHz) {
        return;
    }
    float a = amount * strength;
    if (a > 3.0f) {
        a = 3.0f;
    }
    float g = 1.0f - OCCLUSION_DIFFRACT_GAIN_LOSS * a;
    if (g < OCCLUSION_FULL_GAIN) {
        g = OCCLUSION_FULL_GAIN;
    }
    *gain *= g;
    float target = 19000.0f * powf(OCCLUSION_DIFFRACT_CUTOFF / 19000.0f, a);
    if (target < OCCLUSION_DIFFRACT_MIN_CUTOFF) {
        target = OCCLUSION_DIFFRACT_MIN_CUTOFF;
    }
    if (target < *cutoffHz) {
        *cutoffHz = target;
    }
}

/* ---- 072 B1: smoothing state per client -------------------------------------
   Floats kept as their bit pattern in longs (single atomic stores). Two threads
   may compute the same client at the same time; the worst case is one lost
   smoothing step, never a torn float. */

/* Through a 32-bit integer: long is 8 bytes on 64-bit Linux (host tests), a
   float always 4 - copying sizeof(long) bytes would read past the float. */
static float occ_bits_to_float(long v) {
    const uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static long occ_float_to_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return (long)(int32_t)u;
}

/* One exponential step towards target; tau = attack when moving in the
   "more muffled" direction (moreMuffled), else release. */
static float occ_step(float cur, float target, int moreMuffled, float dtMs) {
    const float tau = moreMuffled ? occlusion_get_attack_ms() : occlusion_get_release_ms();
    if (!(tau > 0.5f) || !(dtMs > 0.0f)) {
        return tau > 0.5f ? cur : target;
    }
    const float alpha = 1.0f - expf(-dtMs / tau);
    return cur + (target - cur) * alpha;
}

void occlusion_smooth(unsigned int clientID, unsigned long long nowMs, const OcclusionInfo* info,
    float targetGain, float targetCutoffHz, OcclusionSmoothed* out) {
    if (!out) {
        return;
    }
    const float targetRelayW = (info && info->kind == OCCLUSION_KIND_DIFFRACT && info->relayValid) ? 1.0f : 0.0f;
    const float targetLogCut = log2f(targetCutoffHz > 1.0f ? targetCutoffHz : 1.0f);
    out->gain = targetGain;
    out->cutoffHz = targetCutoffHz;
    const float targetRoom = info ? info->room : 0.0f;
    out->relayWeight = targetRelayW;
    out->room = targetRoom;
    out->relayKnown = 0;
    out->relayX = out->relayY = out->relayZ = 0.0f;
    if (clientID == 0 || clientID >= OCCLUSION_MAX_CLIENT_ID) {
        if (info && info->relayValid) {
            out->relayKnown = 1;
            out->relayX = info->relayX;
            out->relayY = info->relayY;
            out->relayZ = info->relayZ;
        }
        return;
    }
    const long last = InterlockedCompareExchange(&g_smTick[clientID], 0, 0);
    const unsigned long dt = (unsigned long)nowMs - (unsigned long)last;
    if (info && info->relayValid) {
        /* The relay point itself is smoothed (release time constant), so a switch
           to another opening glides instead of jumping the pan/HRTF direction. */
        float rx = info->relayX, ry = info->relayY, rz = info->relayZ;
        if (InterlockedCompareExchange(&g_smRelayKnown[clientID], 0, 0) && last != 0 && dt <= 2000UL) {
            rx = occ_step(occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][0], 0, 0)), rx, 0, (float)dt);
            ry = occ_step(occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][1], 0, 0)), ry, 0, (float)dt);
            rz = occ_step(occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][2], 0, 0)), rz, 0, (float)dt);
        }
        InterlockedExchange(&g_smRelay[clientID][0], occ_float_to_bits(rx));
        InterlockedExchange(&g_smRelay[clientID][1], occ_float_to_bits(ry));
        InterlockedExchange(&g_smRelay[clientID][2], occ_float_to_bits(rz));
        InterlockedExchange(&g_smRelayKnown[clientID], 1);
    }
    float gain = targetGain, logCut = targetLogCut, relayW = targetRelayW, room = targetRoom;
    if (last != 0 && dt <= 2000UL) {
        const float curGain = occ_bits_to_float(InterlockedCompareExchange(&g_smGain[clientID], 0, 0));
        const float curLogCut = occ_bits_to_float(InterlockedCompareExchange(&g_smLogCut[clientID], 0, 0));
        const float curRelayW = occ_bits_to_float(InterlockedCompareExchange(&g_smRelayW[clientID], 0, 0));
        gain = occ_step(curGain, targetGain, targetGain < curGain, (float)dt);
        logCut = occ_step(curLogCut, targetLogCut, targetLogCut < curLogCut, (float)dt);
        relayW = occ_step(curRelayW, targetRelayW, targetRelayW > curRelayW, (float)dt);
        const float curRoom = occ_bits_to_float(InterlockedCompareExchange(&g_smRoom[clientID], 0, 0));
        room = occ_step(curRoom, targetRoom, targetRoom > curRoom, (float)dt);
    }
    InterlockedExchange(&g_smGain[clientID], occ_float_to_bits(gain));
    InterlockedExchange(&g_smLogCut[clientID], occ_float_to_bits(logCut));
    InterlockedExchange(&g_smRelayW[clientID], occ_float_to_bits(relayW));
    InterlockedExchange(&g_smRoom[clientID], occ_float_to_bits(room));
    InterlockedExchange(&g_smTick[clientID], (long)(unsigned long)(nowMs ? nowMs : 1ULL));

    out->gain = gain;
    out->cutoffHz = exp2f(logCut);
    out->relayWeight = relayW;
    out->room = room;
    if (InterlockedCompareExchange(&g_smRelayKnown[clientID], 0, 0)) {
        out->relayKnown = 1;
        out->relayX = occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][0], 0, 0));
        out->relayY = occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][1], 0, 0));
        out->relayZ = occ_bits_to_float(InterlockedCompareExchange(&g_smRelay[clientID][2], 0, 0));
    }
}
