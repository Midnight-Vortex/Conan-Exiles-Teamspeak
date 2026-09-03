/*
 * ts3_proximity_audio.c — proximity PCM path (gain/pan ramp, LPF, cave reverb)
 * plus the seqlock snapshots and TS unmute batching that feed it.
 *
 * THREAD CONTRACT (V8.3 — PCM owns its render state exclusively)
 * ------------------------------------------------------------------
 *  - The audio thread (onEditPlaybackVoiceDataEvent -> ts3_audio_process_playback)
 *    is the ONLY writer of the render arrays g_renderGain/g_renderPanL/
 *    g_renderPanR and the per-client lowpass state g_lpf. It never calls the TS
 *    API and never takes a lock; it reads target values from lock-free seqlock
 *    snapshots only.
 *  - The callback thread (invalidate/reset from disconnect, eviction, channel
 *    move, tab switch) does NOT write those render arrays anymore. To invalidate
 *    a client it publishes a neutral snapshot (as before) AND bumps that client's
 *    generation counter g_snapGeneration[id] (InterlockedIncrement). That is the
 *    only cross-thread signal about render state.
 *  - The audio thread keeps its own last-seen copy g_renderGeneration[id]
 *    (single-writer = itself, no atomics needed). At buffer start it compares the
 *    two; if the callback bumped the generation, the audio thread reinitializes
 *    the ramp/LPF for that client itself — replicating the exact state the
 *    callback used to write (gain 1.0, pan 0.7071/0.7071, LPF uninitialized).
 *
 *  Result: exactly one writer per array => no torn floats, no cross-thread race
 *  on client-ID reuse. See doku/aenderungen/004-pcm-besitz-generation-counter.md.
 */
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/proximity/ts3_3d.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"
#include "core/proximity/zone_resolve.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"

#include "ts/proximity/ts3_client_limits.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define TS3_RECOMPUTE_ALL_MIN_MS 100   /* 10 Hz cap when local pos unchanged */
#define TS3_RECOMPUTE_POS_EPS_CM 5.0f  /* centimeters — movement triggers refresh */
/* Per-drain budget for the dirty-client recompute path (V8.4). Caps callback
   work per CEDRAIN so a CEPOS burst (up to PLAYER_TABLE_MAX_PLAYERS dirty at
   once) cannot spike one callback; leftovers re-wake for the next drain. */
#define TS3_RECOMPUTE_DRAIN_BUDGET 64
#define TS3_UNMUTE_REARM_MS    500
/* Must match ts3_unmute_clients_for_pcm cap (63 clients + API zero sentinel). */
#define TS3_UNMUTE_BATCH_MAX   64
#define TS3_UNMUTE_RING_SIZE   512   /* sparse flush queue */
/* §8 enter/exit hysteresis: a speaker becomes an audible candidate at its
   nominal hear range (enter) and is only culled again once this many meters
   beyond it (exit). Holds the previous decision inside the band so a speaker
   hovering at the boundary no longer flips compute/neutral (and mute/unmute)
   every recompute. See doku/aenderungen/036-proximity-hysterese.md. */
#define TS3_AUDIO_EXIT_MARGIN_M 10.0f
#define TS3_AUDIBLE_GAIN       0.001f
#define TS3_SILENT_GAIN        0.00001f
#define TS3_CEPOS_PI           3.14159265f
#define TS3_LPF_BYPASS_HZ      19000.0f
#define TS3_SAMPLE_RATE        48000.0f

/* ---- 6.1 seqlock snapshots ------------------------------------------------ */

typedef struct AudioSnap {
    volatile LONG seq;   /* odd = write in progress */
    float gain;
    float panL;
    float panR;
    float cutoffHz;      /* 10.3 lowpass cutoff; >= bypass = off */
    float drr;           /* 1.0 = no diffuse; lower = more diffuse mix */
    int soundproof;      /* 10.2 hard mute (different soundproof zone) */
    int reverbSlot;      /* 10.4 cave reverb slot, -1 = off */
    int valid;
} AudioSnap;

static AudioSnap g_snap[TS3_MAX_CLIENT_ID];

/* Writer-side lock (callback thread + pos watcher). Audio thread never takes it. */
static CRITICAL_SECTION g_writerLock;
static INIT_ONCE g_writerLockOnce = INIT_ONCE_STATIC_INIT;

/* Render gain per client — audio thread private (ramping state). */
static float g_renderGain[TS3_MAX_CLIENT_ID];
static float g_renderPanL[TS3_MAX_CLIENT_ID];
static float g_renderPanR[TS3_MAX_CLIENT_ID];

/* V8.3 ownership signal. The callback thread bumps g_snapGeneration[id]
   (InterlockedIncrement) instead of writing the render arrays; the audio thread
   compares it against its own g_renderGeneration[id] and reinitializes its
   ramp/LPF for that client when it changed. See the file-header thread contract. */
static volatile LONG g_snapGeneration[TS3_MAX_CLIENT_ID];  /* callback thread writes */
static LONG g_renderGeneration[TS3_MAX_CLIENT_ID];         /* audio thread only */

/* Unmute bookkeeping. Flags set from any thread, consumed on callback thread. */
static volatile long g_pendingUnmute[TS3_MAX_CLIENT_ID];
static volatile long g_pendingUnmuteCount = 0;
static anyID g_unmuteRing[TS3_UNMUTE_RING_SIZE];
static volatile long g_unmuteRingWrite = 0;
static volatile long g_unmuteRingRead = 0;
static char g_clientUnlocked[TS3_MAX_CLIENT_ID];       /* callback thread only */
static ULONGLONG g_lastUnmuteMs[TS3_MAX_CLIENT_ID];    /* callback thread only */

/* §8 hysteresis latch: 1 = this speaker is currently inside the audible range.
   Callback thread only (written from audio_recompute_all_impl via the cull test,
   cleared on invalidate/reset) — same convention as g_clientUnlocked above. */
static char g_hearInRange[TS3_MAX_CLIENT_ID];

/* Phase 4.1 — throttle full recomputes on local position polls (atomics). */
static volatile long g_lastLocalSeq = -1;
static volatile long g_lastLocalXcm = 0;
static volatile long g_lastLocalYcm = 0;
static volatile long g_lastLocalZcm = 0;
static volatile LONG64 g_lastRecomputeAllMs = 0;

/* Phase 4.2 — batched per-client recomputes from CEPOS. */
static volatile long g_recomputeDirty[TS3_MAX_CLIENT_ID];
static volatile long g_recomputeDirtyCount = 0;
static volatile long g_recomputeAllPending = 0;

/* Phase 4.5 — diagnostics (throttled log in flush paths). */
static volatile long g_unmuteRingOverflow = 0;
static volatile long g_unmuteRingPushLock = 0;

static void unmute_ring_push(anyID clientID) {
    /* Serialize producers: publish write index only after the slot store is
       complete so ts3_audio_flush_unmutes never observes a new index early. */
    while (InterlockedCompareExchange(&g_unmuteRingPushLock, 1, 0) != 0) {
        YieldProcessor();
    }
    const long w = InterlockedCompareExchange(&g_unmuteRingWrite, 0, 0);
    const long r = InterlockedCompareExchange(&g_unmuteRingRead, 0, 0);
    if (w - r >= TS3_UNMUTE_RING_SIZE) {
        InterlockedIncrement(&g_unmuteRingOverflow);
    }
    g_unmuteRing[w % TS3_UNMUTE_RING_SIZE] = clientID;
    InterlockedExchange(&g_unmuteRingWrite, w + 1);
    InterlockedExchange(&g_unmuteRingPushLock, 0);
}

static void audio_signal_unmute_flag_only(anyID clientID);

static void audio_clear_client_dirty(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    if (InterlockedCompareExchange(&g_recomputeDirty[clientID], 0, 1) == 1) {
        InterlockedDecrement(&g_recomputeDirtyCount);
    }
}

static void audio_reconcile_dirty_count(void) {
    long actual = 0;
    for (int i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        if (InterlockedCompareExchange(&g_recomputeDirty[i], 0, 0) != 0) {
            actual++;
        }
    }
    InterlockedExchange(&g_recomputeDirtyCount, actual);
}

/* ---- 8.4 playback gate ------------------------------------------------------ */

static volatile long g_audioMode = TS3_AUDIO_PASSTHROUGH;

void ts3_audio_set_mode(Ts3AudioMode mode) {
    const long previous = InterlockedExchange(&g_audioMode, (long)mode);
    if (previous != (long)mode) {
        log_write("AUDIO: mode %ld -> %d (%s)", previous, (int)mode,
            mode == TS3_AUDIO_MUTE ? "hub mute"
            : mode == TS3_AUDIO_PROXIMITY ? "proximity" : "passthrough");
    }
}

Ts3AudioMode ts3_audio_get_mode(void) {
    return (Ts3AudioMode)InterlockedCompareExchange(&g_audioMode, 0, 0);
}

static BOOL CALLBACK writer_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_writerLock);
    return TRUE;
}

static void writer_lock_ensure(void) {
    InitOnceExecuteOnce(&g_writerLockOnce, writer_lock_init_once, NULL, NULL);
}

static void snap_publish(anyID clientID, float gain, float panL, float panR,
    float cutoffHz, float drr, int soundproof, int reverbSlot, int valid) {
    AudioSnap* s = &g_snap[clientID];
    InterlockedIncrement(&s->seq);          /* odd: write in progress */
    s->gain = gain;
    s->panL = panL;
    s->panR = panR;
    s->cutoffHz = cutoffHz;
    s->drr = drr;
    s->soundproof = soundproof;
    s->reverbSlot = reverbSlot;
    s->valid = valid;
    InterlockedIncrement(&s->seq);          /* even: stable */
}

/* Publish "no proximity influence" for one client. */
static void snap_publish_neutral(anyID clientID, int valid) {
    snap_publish(clientID, 0.0f, 0.7071f, 0.7071f, TS3_LPF_BYPASS_HZ, 1.0f, 0, -1, valid);
}

/* Audio-thread reader; returns 0 when no stable/valid snapshot exists. */
static int snap_read(anyID clientID, float* gain, float* panL, float* panR,
    float* cutoffHz, float* drr, int* soundproof, int* reverbSlot) {
    const AudioSnap* s = &g_snap[clientID];
    for (int attempt = 0; attempt < 3; attempt++) {
        const LONG seqBefore = s->seq;
        if (seqBefore & 1) {
            continue;
        }
        const float g = s->gain;
        const float l = s->panL;
        const float r = s->panR;
        const float c = s->cutoffHz;
        const float d = s->drr;
        const int sp = s->soundproof;
        const int rv = s->reverbSlot;
        const int valid = s->valid;
        if (s->seq == seqBefore) {
            if (!valid) {
                return 0;
            }
            *gain = g;
            *panL = l;
            *panR = r;
            *cutoffHz = c;
            *drr = d;
            *soundproof = sp;
            *reverbSlot = rv;
            return 1;
        }
    }
    return 0;
}

/* ---- 10.3/10.4 audio-thread FX state ---------------------------------------- */

/* Per-client single-pole lowpass state (small, static for all clients). */
typedef struct LpfState {
    float prevL, prevR;
    float alpha;
    float lastCutoff;
    int initialized;
} LpfState;

static LpfState g_lpf[TS3_MAX_CLIENT_ID];

/* Schroeder cave reverb (comb + allpass), same tuning as the old plugin.
   Fixed static pool — no malloc anywhere near the audio path. Slots are
   assigned/released on the writer side; the audio thread only indexes. */
#define CAVE_SLOTS        32
#define CAVE_COMBS        4
#define CAVE_ALLPASS      2
#define CAVE_PREDELAY     1680  /* ~35 ms */
#define CAVE_COMB_MAX     5466
#define CAVE_AP_MAX       607
#define CAVE_WET_GAIN     0.26f
#define CAVE_DRY_GAIN     0.92f
#define CAVE_COMB_MIX     0.18f
#define CAVE_COMB_DAMP    0.60f
#define CAVE_AP_GAIN      0.55f
#define CAVE_STEREO_WIDTH 0.68f

static const float CAVE_COMB_FB[CAVE_COMBS] = { 0.79f, 0.80f, 0.81f, 0.82f };
static const int CAVE_COMB_LEN[CAVE_COMBS] = { 2546, 3507, 4257, 5466 };
static const int CAVE_AP_LEN[CAVE_ALLPASS] = { 607, 481 };

typedef struct CaveState {
    float combL[CAVE_COMBS][CAVE_COMB_MAX];
    float combR[CAVE_COMBS][CAVE_COMB_MAX];
    int combPos[CAVE_COMBS];
    float combLpL[CAVE_COMBS];
    float combLpR[CAVE_COMBS];
    float apL[CAVE_ALLPASS][CAVE_AP_MAX];
    float apR[CAVE_ALLPASS][CAVE_AP_MAX];
    int apPos[CAVE_ALLPASS];
    float predelayL[CAVE_PREDELAY];
    float predelayR[CAVE_PREDELAY];
    int predelayPos;
    volatile LONG owner; /* clientID, 0 = free (writer side) */
} CaveState;

static CaveState g_cave[CAVE_SLOTS];

/* Writer-side slot per client (guarded by g_writerLock), -1 = none. */
static short g_reverbSlotByClient[TS3_MAX_CLIENT_ID];
static int g_reverbSlotsInit = 0;

static void cave_slots_ensure_init(void) {
    if (!g_reverbSlotsInit) {
        for (int i = 0; i < TS3_MAX_CLIENT_ID; i++) {
            g_reverbSlotByClient[i] = -1;
        }
        g_reverbSlotsInit = 1;
    }
}

/* Writer side, g_writerLock held. Returns slot or -1 (pool exhausted). */
static int cave_slot_acquire(anyID clientID) {
    cave_slots_ensure_init();
    if (g_reverbSlotByClient[clientID] >= 0) {
        return g_reverbSlotByClient[clientID];
    }
    for (int i = 0; i < CAVE_SLOTS; i++) {
        if (InterlockedCompareExchange(&g_cave[i].owner, 0, 0) != 0) {
            continue;
        }
        CaveState* c = &g_cave[i];
        memset(c->combL, 0, sizeof(c->combL));
        memset(c->combR, 0, sizeof(c->combR));
        memset(c->combPos, 0, sizeof(c->combPos));
        memset(c->combLpL, 0, sizeof(c->combLpL));
        memset(c->combLpR, 0, sizeof(c->combLpR));
        memset(c->apL, 0, sizeof(c->apL));
        memset(c->apR, 0, sizeof(c->apR));
        memset(c->apPos, 0, sizeof(c->apPos));
        memset(c->predelayL, 0, sizeof(c->predelayL));
        memset(c->predelayR, 0, sizeof(c->predelayR));
        c->predelayPos = 0;
        if (InterlockedCompareExchange(&g_cave[i].owner, (LONG)clientID, 0) != 0) {
            continue;
        }
        g_reverbSlotByClient[clientID] = (short)i;
        return i;
    }
    return -1;
}

/* Writer side, g_writerLock held. */
static void cave_slot_release(anyID clientID) {
    cave_slots_ensure_init();
    const short slot = g_reverbSlotByClient[clientID];
    if (slot >= 0 && slot < CAVE_SLOTS) {
        InterlockedCompareExchange(&g_cave[slot].owner, 0, (LONG)clientID);
        g_reverbSlotByClient[clientID] = -1;
    }
}

/* ---- gain/pan computation (writer side) ----------------------------------- */

static float audio_zone_reference_distance(const HubSettings* hub, int localZone) {
    float ref = 1.0f;
    if (hub) {
        if (hub->audioMinDistance > 0.0f) {
            ref = hub->audioMinDistance;
        }
        if (localZone >= 0 && localZone < hub->zoneCount) {
            const float zoneRef = hub->zones[localZone].audioMinDistance;
            if (zoneRef > 0.0f) {
                ref = zoneRef;
            }
        }
    }
    if (ref < 0.1f) {
        ref = 0.1f;
    }
    return ref;
}

static int audio_local_position_changed(const PosSample* local, int localValid) {
    if (!localValid) {
        return 0;
    }
    if ((int)local->seq != (int)InterlockedCompareExchange(&g_lastLocalSeq, 0, 0)) {
        return 1;
    }
    const long xcm = (long)local->x;
    const long ycm = (long)local->y;
    const long zcm = (long)local->z;
    const long lastX = InterlockedCompareExchange(&g_lastLocalXcm, 0, 0);
    const long lastY = InterlockedCompareExchange(&g_lastLocalYcm, 0, 0);
    const long lastZ = InterlockedCompareExchange(&g_lastLocalZcm, 0, 0);
    return xcm - lastX > (long)TS3_RECOMPUTE_POS_EPS_CM
        || xcm - lastX < -(long)TS3_RECOMPUTE_POS_EPS_CM
        || ycm - lastY > (long)TS3_RECOMPUTE_POS_EPS_CM
        || ycm - lastY < -(long)TS3_RECOMPUTE_POS_EPS_CM
        || zcm - lastZ > (long)TS3_RECOMPUTE_POS_EPS_CM
        || zcm - lastZ < -(long)TS3_RECOMPUTE_POS_EPS_CM;
}

static void audio_note_local_position(const PosSample* local, int localValid) {
    if (!localValid) {
        return;
    }
    InterlockedExchange(&g_lastLocalSeq, (long)local->seq);
    InterlockedExchange(&g_lastLocalXcm, (long)local->x);
    InterlockedExchange(&g_lastLocalYcm, (long)local->y);
    InterlockedExchange(&g_lastLocalZcm, (long)local->z);
}

static int audio_local_zone(const HubSettings* hub, const PosSample* local,
    int localValid, int hubActive) {
    if (!hubActive || !localValid || hub->zoneCount <= 0) {
        return -1;
    }
    return zone_resolve(hub, (float)(local->x / 100.0), (float)(local->y / 100.0), (float)(local->z / 100.0));
}

/* Callback thread only (updates the per-client hysteresis latch). enter =
   nominal hear range, exit = enter + TS3_AUDIO_EXIT_MARGIN_M. */
static int audio_in_hear_range(anyID clientID, const PosSample* local, int localValid,
    const PlayerEntry* remote, float listenAdd) {
    if (!localValid || !remote) {
        return 1;
    }
    const float lx = (float)(local->x / 100.0);
    const float ly = (float)(local->y / 100.0);
    const float lz = (float)(local->z / 100.0);
    const float dist = prox_distance(lx, ly, lz, remote->x, remote->y, remote->z);
    const float enterDist = remote->voiceDistance + listenAdd;
    const float exitDist = enterDist + TS3_AUDIO_EXIT_MARGIN_M;

    const int prev = ts3_client_id_valid(clientID) ? g_hearInRange[clientID] : 0;
    const int inRange = prox_hysteresis_in_range(dist, enterDist, exitDist, prev);
    if (ts3_client_id_valid(clientID)) {
        g_hearInRange[clientID] = (char)inRange;
    }
    return inRange;
}

typedef struct AudioPublishParams {
    float gain;
    float panL;
    float panR;
    float cutoffHz;
    float drr;
    int soundproof;
    int reverbZone;
    int reverbSlot;
    int valid;
    int neutral;
} AudioPublishParams;

static float audio_lerp_realistic(float bypass, float target, float intensity) {
    return bypass + (target - bypass) * intensity;
}

/* Phase 4.3 — math outside lock; cave slots + publish under g_writerLock. */
static void audio_compute_client(anyID clientID, const PosSample* local,
    int localValid, const HubSettings* hub, int localZone,
    AudioPublishParams* out) {
    memset(out, 0, sizeof(*out));
    out->cutoffHz = TS3_LPF_BYPASS_HZ;
    out->drr = 1.0f;
    out->reverbSlot = -1;
    out->panL = 0.7071f;
    out->panR = 0.7071f;

    PlayerEntry remote;
    if (!localValid || !player_table_get(clientID, &remote)) {
        out->neutral = 1;
        out->valid = 0;
        return;
    }

    const float lx = (float)(local->x / 100.0);
    const float ly = (float)(local->y / 100.0);
    const float lz = (float)(local->z / 100.0);
    const float distance = prox_distance(lx, ly, lz, remote.x, remote.y, remote.z);

    int soundproof = 0;
    int reverbZone = 0;
    float cutoffHz = TS3_LPF_BYPASS_HZ;
    float drr = 1.0f;
    int remoteZone = -1;

    if (hub && hub->zoneCount > 0) {
        remoteZone = zone_resolve(hub, remote.x, remote.y, remote.z);
        soundproof = zone_soundproof_muted(hub, localZone, remoteZone);
        reverbZone = !soundproof && zone_reverb_active(hub, localZone, remoteZone);
    }

    float maxVolume = server_profile_get_max_volume();
    if (hub && localZone >= 0 && localZone < hub->zoneCount
        && hub->zones[localZone].audioMaxVolume > 0.0f) {
        maxVolume = hub->zones[localZone].audioMaxVolume;
    }
    const float hearRange = remote.voiceDistance
        + server_profile_get_listen_add_distance();
    float gain = soundproof ? 0.0f
        : prox_volume_from_distance(distance, hearRange, maxVolume);

    const float yawRad = (float)(local->yaw * TS3_CEPOS_PI / 180.0);
    const float dirX = -cosf(yawRad);
    const float dirY = sinf((float)(-local->yawY * TS3_CEPOS_PI / 180.0));
    const float dirZ = -sinf(yawRad);
    const float toRemoteX = remote.x - lx;
    const float toRemoteY = remote.y - ly;
    const float toRemoteZ = remote.z - lz;

    const int realisticOn = server_profile_get_realistic_audio();
    const float filterIntensity = server_profile_get_filter_intensity() / 100.0f;
    const int realisticActive = realisticOn && filterIntensity > 0.001f && !soundproof;

    if (realisticActive) {
        const float refDist = audio_zone_reference_distance(hub, localZone);
        const float rawCutoff = prox_lowpass_cutoff_hz(distance);
        const float rawDrr = prox_direct_reverb_ratio(distance, refDist);
        const float frontBack = prox_front_back_dot3d(dirX, dirY, dirZ,
            toRemoteX, toRemoteY, toRemoteZ);
        ProxRearPsycho rear;
        prox_rear_psychoacoustics(frontBack, &rear);

        const float targetCutoff = rawCutoff * rear.cutoffMul;
        cutoffHz = audio_lerp_realistic(TS3_LPF_BYPASS_HZ, targetCutoff, filterIntensity);

        const float targetDrr = rawDrr * rear.drrMul;
        drr = 1.0f - (1.0f - targetDrr) * filterIntensity;

        const float directionVolume = audio_lerp_realistic(1.0f, rear.directionVolume,
            filterIntensity);
        gain *= directionVolume;
    }
    else if (reverbZone) {
        /* Cave character is LPF + Schroeder wet. Do not apply DRR to gain or
           drr — prox_direct_reverb_ratio hits ~0.05 at a few meters and would
           crush others via gain or prox_apply_diffuse_samples. */
        cutoffHz = prox_lowpass_cutoff_hz(distance);
    }

    float panL, panR;
    /* Mumble "TRUE stereo" (~4090) — light spatial pan; always in proximity mode.
       RealisticAudio owns LPF/diffuse blend; reverb zones add LPF + cave only. */
    prox_binaural_stereo_gains(dirX, dirY, dirZ,
        toRemoteX, toRemoteY, toRemoteZ, &panL, &panR);

    out->gain = gain;
    out->panL = panL;
    out->panR = panR;
    out->cutoffHz = cutoffHz;
    out->drr = drr;
    out->soundproof = soundproof;
    out->reverbZone = reverbZone;
    out->valid = 1;
    out->neutral = 0;

    if (gain > TS3_AUDIBLE_GAIN) {
        audio_signal_unmute_flag_only(clientID);
    }
}

/* Caller must hold g_writerLock. */
static void audio_publish_client(anyID clientID, const AudioPublishParams* params) {
    if (params->neutral || !params->valid) {
        cave_slot_release(clientID);
        snap_publish_neutral(clientID, params->valid);
        return;
    }

    int reverbSlot = -1;
    if (params->reverbZone) {
        reverbSlot = cave_slot_acquire(clientID);
    }
    else {
        cave_slot_release(clientID);
    }

    snap_publish(clientID, params->gain, params->panL, params->panR,
        params->cutoffHz, params->drr, params->soundproof, reverbSlot, params->valid);
}

static void audio_recompute_client_impl(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    writer_lock_ensure();

    PosSample local;
    const int localValid = pos_get_current(&local);

    HubSettings hub;
    const int hubActive = server_profile_get(&hub);
    const int localZone = audio_local_zone(&hub, &local, localValid, hubActive);

    /* Same §8 enter/exit gate as audio_recompute_all_impl — CEPOS dirty drains
       must not republish (and unmute) speakers the full pass would keep culled. */
    PlayerEntry remote;
    const float listenAdd = server_profile_get_listen_add_distance();
    AudioPublishParams params;
    if (!player_table_get(clientID, &remote)
        || !audio_in_hear_range(clientID, &local, localValid, &remote, listenAdd)) {
        memset(&params, 0, sizeof(params));
        params.neutral = 1;
        params.valid = 0;
    }
    else {
        audio_compute_client(clientID, &local, localValid,
            hubActive ? &hub : NULL, localZone, &params);
    }

    EnterCriticalSection(&g_writerLock);
    audio_publish_client(clientID, &params);
    LeaveCriticalSection(&g_writerLock);
}

static void audio_recompute_all_impl(void) {
    writer_lock_ensure();

    /* Claim the pending flag BEFORE reading any inputs. A request that arrives
       while this recompute is running re-sets the flag and wakes CEDRAIN again,
       so its (possibly newer) inputs get a fresh pass instead of being wiped
       by a clear at the end (lost-update race found in the V8.3 review). */
    InterlockedExchange(&g_recomputeAllPending, 0);

    PosSample local;
    const int localValid = pos_get_current(&local);

    HubSettings hub;
    const int hubActive = server_profile_get(&hub);
    const int localZone = audio_local_zone(&hub, &local, localValid, hubActive);

    PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);
    const float listenAdd = server_profile_get_listen_add_distance();

    AudioPublishParams batch[PLAYER_TABLE_MAX_PLAYERS];
    anyID batchIds[PLAYER_TABLE_MAX_PLAYERS];
    int batchCount = 0;

    for (int i = 0; i < count; i++) {
        const anyID cid = (anyID)players[i].clientID;
        if (!ts3_client_id_valid(players[i].clientID)) {
            continue;
        }
        AudioPublishParams* params = &batch[batchCount];
        if (!audio_in_hear_range(cid, &local, localValid, &players[i], listenAdd)) {
            memset(params, 0, sizeof(*params));
            params->neutral = 1;
            params->valid = 0;
        }
        else {
            audio_compute_client(cid, &local, localValid,
                hubActive ? &hub : NULL, localZone, params);
        }
        batchIds[batchCount] = cid;
        batchCount++;
        audio_clear_client_dirty(cid);
    }
    audio_reconcile_dirty_count();

    EnterCriticalSection(&g_writerLock);
    for (int i = 0; i < batchCount; i++) {
        audio_publish_client(batchIds[i], &batch[i]);
    }
    LeaveCriticalSection(&g_writerLock);

    if (InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0) {
        ts3_request_wakeup();
    }
}

void ts3_audio_recompute_client(anyID clientID) {
    audio_recompute_client_impl(clientID);
}

void ts3_audio_recompute_all_force(void) {
    const ULONGLONG now = GetTickCount64();
    InterlockedExchange64(&g_lastRecomputeAllMs, (LONG64)now);
    {
        PosSample local;
        if (pos_get_current(&local)) {
            audio_note_local_position(&local, 1);
        }
    }
    audio_recompute_all_impl();
}

void ts3_audio_recompute_all(void) {
    ts3_audio_recompute_all_force();
}

void ts3_audio_request_recompute_all(void) {
    InterlockedExchange(&g_recomputeAllPending, 1);
    ts3_request_wakeup();
}

void ts3_audio_on_local_position_update(void) {
    PosSample local;
    const int localValid = pos_get_current(&local);
    const ULONGLONG now = GetTickCount64();
    const int moved = audio_local_position_changed(&local, localValid);
    const ULONGLONG lastMs = (ULONGLONG)InterlockedCompareExchange64(
        &g_lastRecomputeAllMs, 0, 0);

    if (!moved && (now - lastMs) < TS3_RECOMPUTE_ALL_MIN_MS) {
        return;
    }

    if (moved) {
        audio_note_local_position(&local, localValid);
    }
    InterlockedExchange64(&g_lastRecomputeAllMs, (LONG64)now);
    InterlockedExchange(&g_recomputeAllPending, 1);
    ts3_request_wakeup();
}

void ts3_audio_mark_client_dirty(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    if (InterlockedCompareExchange(&g_recomputeDirty[clientID], 1, 0) == 0) {
        InterlockedIncrement(&g_recomputeDirtyCount);
    }
    ts3_request_wakeup();
}

int ts3_audio_has_pending_recompute(void) {
    return InterlockedCompareExchange(&g_recomputeAllPending, 0, 0) != 0
        || InterlockedCompareExchange(&g_recomputeDirtyCount, 0, 0) > 0;
}

void ts3_audio_flush_recomputes(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    if (InterlockedCompareExchange(&g_recomputeAllPending, 0, 0)) {
        audio_recompute_all_impl();
        return;
    }

    const long dirtyCount = InterlockedCompareExchange(&g_recomputeDirtyCount, 0, 0);
    if (dirtyCount <= 0) {
        return;
    }

    if (dirtyCount >= 32) {
        static ULONGLONG s_lastDirtyLog = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastDirtyLog >= 10000) {
            s_lastDirtyLog = nowMs;
            log_write("AUDIO: recompute backlog %ld dirty client(s)", dirtyCount);
        }
    }

    PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);
    int processed = 0;
    for (int i = 0; i < count; i++) {
        const anyID cid = (anyID)players[i].clientID;
        if (!ts3_client_id_valid(players[i].clientID)) {
            continue;
        }
        if (processed >= TS3_RECOMPUTE_DRAIN_BUDGET) {
            break; /* budget spent — finish the rest on the next drain */
        }
        if (InterlockedCompareExchange(&g_recomputeDirty[cid], 0, 1) == 1) {
            InterlockedDecrement(&g_recomputeDirtyCount);
            audio_recompute_client_impl(cid);
            processed++;
        }
    }
    audio_reconcile_dirty_count();

    /* Leftover dirty clients (budget or a request that arrived mid-drain):
       re-wake so CEDRAIN runs again for the remainder. */
    if (InterlockedCompareExchange(&g_recomputeDirtyCount, 0, 0) > 0) {
        ts3_request_wakeup();
    }
}

/* ---- 10.3 lowpass (audio thread, no locks, no allocations) ------------------- */

static short audio_clamp_sample(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (short)v;
}

static void audio_apply_lowpass(short* samples, int sampleCount, int channels,
    float cutoffHz, LpfState* fx) {
    if (cutoffHz != fx->lastCutoff) {
        const float dt = 1.0f / TS3_SAMPLE_RATE;
        const float rc = 1.0f / (cutoffHz * 6.28318530718f);
        fx->alpha = dt / (rc + dt);
        fx->lastCutoff = cutoffHz;
    }
    const float alpha = fx->alpha;

    if (channels >= 2) {
        for (int s = 0; s < sampleCount; s++) {
            const int li = s * channels;
            const int ri = li + 1;
            const float inL = (float)samples[li];
            const float inR = (float)samples[ri];
            if (!fx->initialized) {
                fx->prevL = inL;
                fx->prevR = inR;
            }
            fx->prevL += alpha * (inL - fx->prevL);
            fx->prevR += alpha * (inR - fx->prevR);
            const float leftSecond = fx->prevL;
            const float rightSecond = fx->prevR;
            fx->prevL += alpha * 0.7f * (leftSecond - fx->prevL);
            fx->prevR += alpha * 0.7f * (rightSecond - fx->prevR);
            samples[li] = audio_clamp_sample(fx->prevL);
            samples[ri] = audio_clamp_sample(fx->prevR);
        }
    }
    else {
        for (int s = 0; s < sampleCount; s++) {
            const float in = (float)samples[s];
            if (!fx->initialized) {
                fx->prevL = in;
            }
            fx->prevL += alpha * (in - fx->prevL);
            const float secondPass = fx->prevL;
            fx->prevL += alpha * 0.7f * (secondPass - fx->prevL);
            samples[s] = audio_clamp_sample(fx->prevL);
        }
    }
    fx->initialized = 1;
}

/* ---- 10.4 cave reverb (audio thread, fixed memory) ---------------------------- */

/* Freeverb-style comb with damped feedback. */
static float cave_comb(float* buf, int len, int* pos, float input,
    float feedback, float* lpState) {
    const float output = buf[*pos];
    *lpState = output * (1.0f - CAVE_COMB_DAMP) + (*lpState) * CAVE_COMB_DAMP;
    buf[*pos] = input + (*lpState) * feedback;
    if (++(*pos) >= len) {
        *pos = 0;
    }
    return output;
}

/* Allpass smears early reflections into a diffuse tail. */
static float cave_allpass(float* buf, int len, int* pos, float input) {
    const float delayed = buf[*pos];
    const float output = -input + delayed;
    buf[*pos] = input + delayed * CAVE_AP_GAIN;
    if (++(*pos) >= len) {
        *pos = 0;
    }
    return output;
}

static void cave_process_sample(CaveState* rev, float inL, float inR,
    float* wetL, float* wetR) {
    const float revInL = rev->predelayL[rev->predelayPos];
    const float revInR = rev->predelayR[rev->predelayPos];
    rev->predelayL[rev->predelayPos] = inL;
    rev->predelayR[rev->predelayPos] = inR;
    if (++rev->predelayPos >= CAVE_PREDELAY) {
        rev->predelayPos = 0;
    }

    float outL = 0.0f, outR = 0.0f;
    for (int c = 0; c < CAVE_COMBS; c++) {
        outL += cave_comb(rev->combL[c], CAVE_COMB_LEN[c], &rev->combPos[c],
            revInL, CAVE_COMB_FB[c], &rev->combLpL[c]);
        outR += cave_comb(rev->combR[c], CAVE_COMB_LEN[c], &rev->combPos[c],
            revInR, CAVE_COMB_FB[c], &rev->combLpR[c]);
    }
    outL *= CAVE_COMB_MIX;
    outR *= CAVE_COMB_MIX;

    for (int a = 0; a < CAVE_ALLPASS; a++) {
        outL = cave_allpass(rev->apL[a], CAVE_AP_LEN[a], &rev->apPos[a], outL);
        outR = cave_allpass(rev->apR[a], CAVE_AP_LEN[a], &rev->apPos[a], outR);
    }

    const float mono = (outL + outR) * 0.5f;
    *wetL = mono + (outL - mono) * CAVE_STEREO_WIDTH;
    *wetR = mono + (outR - mono) * CAVE_STEREO_WIDTH;
}

static void audio_apply_cave(short* samples, int sampleCount, int channels,
    CaveState* rev, LONG expectedOwner) {
    /* Owner checked once — mid-buffer abort left dry/wet mix and caused clicks. */
    if (InterlockedCompareExchange(&rev->owner, 0, 0) != expectedOwner) {
        return;
    }
    if (channels >= 2) {
        for (int s = 0; s < sampleCount; s++) {
            const int li = s * channels;
            const int ri = li + 1;
            const float inL = (float)samples[li];
            const float inR = (float)samples[ri];
            float wetL, wetR;
            cave_process_sample(rev, inL, inR, &wetL, &wetR);
            samples[li] = audio_clamp_sample(inL * CAVE_DRY_GAIN + wetL * CAVE_WET_GAIN);
            samples[ri] = audio_clamp_sample(inR * CAVE_DRY_GAIN + wetR * CAVE_WET_GAIN);
        }
    }
    else {
        for (int s = 0; s < sampleCount; s++) {
            const float in = (float)samples[s];
            float wetL, wetR;
            cave_process_sample(rev, in, in, &wetL, &wetR);
            samples[s] = audio_clamp_sample(
                in * CAVE_DRY_GAIN + (wetL + wetR) * 0.5f * CAVE_WET_GAIN);
        }
    }
}

/* ---- 6.2 PCM hot path (audio thread) --------------------------------------- */

void ts3_audio_process_playback(anyID clientID, short* samples, int sampleCount, int channels) {
    if (!samples || sampleCount <= 0 || channels <= 0
        || !ts3_client_id_valid(clientID)) {
        return;
    }

    /* V8.3 PCM-owns-its-state: if the callback thread signaled invalidation
       (client left / evicted / moved away / reset) since our last buffer, we
       reinitialize the ramp + LPF ourselves — the exact state the callback used
       to write (gain 1.0, neutral pan, LPF uninitialized). Reading the volatile
       LONG generation is a single aligned read (InterlockedCompareExchange with
       a no-op compare); the last-seen copy is audio-thread-private. */
    const LONG curGen = InterlockedCompareExchange(&g_snapGeneration[clientID], 0, 0);
    if (render_state_needs_reinit(g_renderGeneration[clientID], curGen)) {
        g_renderGain[clientID] = 1.0f;
        g_renderPanL[clientID] = 0.7071f;
        g_renderPanR[clientID] = 0.7071f;
        g_lpf[clientID].initialized = 0;
        g_renderGeneration[clientID] = curGen;
    }

    const long mode = InterlockedCompareExchange(&g_audioMode, 0, 0);
    if (mode == TS3_AUDIO_PASSTHROUGH) {
        g_renderGain[clientID] = 1.0f;
        return;
    }
    if (mode == TS3_AUDIO_MUTE) {
        /* Hub: hard mute — zero the buffer so nothing is audible. */
        memset(samples, 0, (size_t)sampleCount * (size_t)channels * sizeof(short));
        g_renderGain[clientID] = 0.0f;
        return;
    }

    float target, panL, panR, cutoffHz, drr;
    int soundproof, reverbSlot;
    if (!snap_read(clientID, &target, &panL, &panR, &cutoffHz, &drr, &soundproof, &reverbSlot)) {
        /* Ingame proximity mode but no position data for this speaker (no
           CEPOS yet / expired) — mute instead of leaking full-volume voice
           that should be distance-attenuated. The next CEPOS packet
           publishes a snapshot and unmutes. */
        memset(samples, 0, (size_t)sampleCount * (size_t)channels * sizeof(short));
        g_renderGain[clientID] = 0.0f;
        g_lpf[clientID].initialized = 0;
        return;
    }

    /* 10.2 soundproof: hard silence, no ramp, no ghost tail. */
    if (soundproof) {
        memset(samples, 0, (size_t)sampleCount * (size_t)channels * sizeof(short));
        g_renderGain[clientID] = 0.0f;
        g_lpf[clientID].initialized = 0;
        return;
    }

    /* Fully silent: skip FX; ramp handles the tail (no hard cut at audible threshold). */
    if (target <= TS3_SILENT_GAIN && g_renderGain[clientID] <= TS3_SILENT_GAIN) {
        memset(samples, 0, (size_t)sampleCount * (size_t)channels * sizeof(short));
        g_renderGain[clientID] = 0.0f;
        g_renderPanL[clientID] = 0.7071f;
        g_renderPanR[clientID] = 0.7071f;
        g_lpf[clientID].initialized = 0;
        return;
    }

    /* 10.3 distance lowpass (reverb zones or realistic open-world path). */
    if (cutoffHz < TS3_LPF_BYPASS_HZ) {
        audio_apply_lowpass(samples, sampleCount, channels, cutoffHz, &g_lpf[clientID]);
    }
    else {
        g_lpf[clientID].initialized = 0;
    }

    /* Realistic path: diffuse/DRR mix before gain (Mumble order). */
    if (drr < 0.99f) {
        prox_apply_diffuse_samples(samples, sampleCount, channels, drr);
    }

    /* 10.4 cave reverb — only while this client still owns the slot. */
    if (reverbSlot >= 0 && reverbSlot < CAVE_SLOTS) {
        CaveState* rev = &g_cave[reverbSlot];
        const LONG owner = InterlockedCompareExchange(&rev->owner, 0, 0);
        if (owner == (LONG)clientID) {
            audio_apply_cave(samples, sampleCount, channels, rev, owner);
        }
    }

    if (target > TS3_AUDIBLE_GAIN) {
        audio_signal_unmute_flag_only(clientID);
    }

    /* 6.5 click-free ramp: spread gain + pan across this buffer. */
    float current = g_renderGain[clientID];
    const float step = (target - current) / (float)sampleCount;
    float panLCur = g_renderPanL[clientID];
    float panRCur = g_renderPanR[clientID];
    const float panStepL = (panL - panLCur) / (float)sampleCount;
    const float panStepR = (panR - panRCur) / (float)sampleCount;

    if (channels == 1) {
        for (int i = 0; i < sampleCount; i++) {
            current += step;
            float v = samples[i] * current;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            samples[i] = (short)v;
        }
    }
    else {
        for (int i = 0; i < sampleCount; i++) {
            current += step;
            panLCur += panStepL;
            panRCur += panStepR;
            short* frame = samples + (size_t)i * channels;
            float vl = frame[0] * current * panLCur;
            float vr = frame[1] * current * panRCur;
            if (vl > 32767.0f) vl = 32767.0f;
            if (vl < -32768.0f) vl = -32768.0f;
            if (vr > 32767.0f) vr = 32767.0f;
            if (vr < -32768.0f) vr = -32768.0f;
            frame[0] = (short)vl;
            frame[1] = (short)vr;
            /* Extra channels (>2) get the plain gain. */
            for (int c = 2; c < channels; c++) {
                float v = frame[c] * current;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                frame[c] = (short)v;
            }
        }
    }
    g_renderGain[clientID] = target;
    g_renderPanL[clientID] = panL;
    g_renderPanR[clientID] = panR;
}

/* ---- 6.3 unmute signal ------------------------------------------------------ */

/* Flag only — no wakeup. Safe on the audio thread (no TS API, no locks). */
static void audio_signal_unmute_flag_only(anyID clientID) {
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 1, 0) == 0) {
        InterlockedIncrement(&g_pendingUnmuteCount);
        unmute_ring_push(clientID);
    }
}

void ts3_audio_signal_unmute(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 1, 0) == 0) {
        InterlockedIncrement(&g_pendingUnmuteCount);
        unmute_ring_push(clientID);
        /* Wakeup sends a plugin command — never do that from the audio thread.
           Audio-thread callers use the flag-only path below. */
        ts3_request_wakeup();
    }
}

/* ---- 6.4 batch flush (callback thread) -------------------------------------- */

void ts3_audio_flush_unmutes(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }
    if (InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) <= 0) {
        return;
    }

    {
        const long pending = InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0);
        if (pending >= 32) {
            static ULONGLONG s_lastBacklogLog = 0;
            const ULONGLONG nowMs = GetTickCount64();
            if (nowMs - s_lastBacklogLog >= 10000) {
                s_lastBacklogLog = nowMs;
                log_write("AUDIO: unmute backlog %ld pending", pending);
            }
        }
        const long overflow = InterlockedCompareExchange(&g_unmuteRingOverflow, 0, 0);
        if (overflow > 0) {
            static ULONGLONG s_lastOverflowLog = 0;
            const ULONGLONG nowMs = GetTickCount64();
            if (nowMs - s_lastOverflowLog >= 10000) {
                s_lastOverflowLog = nowMs;
                log_write("AUDIO: unmute ring overflow count %ld", overflow);
            }
        }
    }

    const ULONGLONG now = GetTickCount64();
    anyID batch[TS3_UNMUTE_BATCH_MAX];
    int batchCount = 0;

    const long writeIdx = InterlockedCompareExchange(&g_unmuteRingWrite, 0, 0);
    long readIdx = InterlockedCompareExchange(&g_unmuteRingRead, 0, 0);

    while (readIdx < writeIdx && batchCount < TS3_UNMUTE_BATCH_MAX - 1) {
        const anyID id = g_unmuteRing[readIdx % TS3_UNMUTE_RING_SIZE];
        readIdx++;
        if (!ts3_client_id_valid(id)) {
            continue;
        }
        if (InterlockedCompareExchange(&g_pendingUnmute[id], 0, 0) == 0) {
            continue;
        }
        if (g_clientUnlocked[id] && now - g_lastUnmuteMs[id] < TS3_UNMUTE_REARM_MS) {
            continue;
        }
        batch[batchCount++] = id;
    }
    InterlockedExchange(&g_unmuteRingRead, readIdx);

    /* Ring may have wrapped while flags remain — scan ring slots once. */
    if (batchCount == 0
        && InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0) {
        for (int i = 0; i < TS3_UNMUTE_RING_SIZE && batchCount < TS3_UNMUTE_BATCH_MAX - 1; i++) {
            const anyID id = g_unmuteRing[i];
            if (!ts3_client_id_valid(id)) {
                continue;
            }
            if (InterlockedCompareExchange(&g_pendingUnmute[id], 0, 0) == 0) {
                continue;
            }
            if (g_clientUnlocked[id] && now - g_lastUnmuteMs[id] < TS3_UNMUTE_REARM_MS) {
                continue;
            }
            int dup = 0;
            for (int j = 0; j < batchCount; j++) {
                if (batch[j] == id) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                batch[batchCount++] = id;
            }
        }
    }

    if (batchCount == 0) {
        if (ts3_audio_has_pending_unmutes()) {
            ts3_request_wakeup();
        }
        return;
    }

    const int unmuted = ts3_unmute_clients_for_pcm(batch, batchCount);
    if (unmuted <= 0) {
        if (ts3_audio_has_pending_unmutes()) {
            ts3_request_wakeup();
        }
        return; /* pending flags stay set; next flush retries */
    }

    int newlyUnmuted = 0;
    for (int i = 0; i < unmuted; i++) {
        const anyID id = batch[i];
        if (!g_clientUnlocked[id]) {
            newlyUnmuted++;
        }
        if (InterlockedCompareExchange(&g_pendingUnmute[id], 0, 1) == 1) {
            InterlockedDecrement(&g_pendingUnmuteCount);
        }
        g_clientUnlocked[id] = 1;
        g_lastUnmuteMs[id] = now;
    }
    if (newlyUnmuted > 0) {
        log_debug("AUDIO: unmuted %d client(s)", newlyUnmuted);
    }
    if (ts3_audio_has_pending_unmutes()) {
        ts3_request_wakeup();
    }
}

int ts3_audio_has_pending_unmutes(void) {
    return InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0;
}

/* ---- cleanup ----------------------------------------------------------------- */

void ts3_audio_invalidate_client(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    writer_lock_ensure();
    EnterCriticalSection(&g_writerLock);
    cave_slot_release(clientID);
    snap_publish_neutral(clientID, 0);
    LeaveCriticalSection(&g_writerLock);

    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 0, 1) == 1) {
        InterlockedDecrement(&g_pendingUnmuteCount);
    }
    g_clientUnlocked[clientID] = 0;
    g_lastUnmuteMs[clientID] = 0;
    g_hearInRange[clientID] = 0; /* §8 latch: a fresh/reused ID starts out of range */
    /* Render ramp + LPF are audio-thread-owned. Signal invalidation via the
       generation counter; the audio thread resets the ramp/LPF itself on its
       next buffer for this ID (same values this used to write directly). */
    InterlockedIncrement(&g_snapGeneration[clientID]);
    audio_clear_client_dirty(clientID);
    ts3d_invalidate_client(clientID);
}

int ts3_proximity_audio_soundproof_muted(unsigned int clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return 0;
    }
    float gain, panL, panR, cutoffHz, drr;
    int soundproof, reverbSlot;
    if (!snap_read((anyID)clientID, &gain, &panL, &panR, &cutoffHz, &drr, &soundproof, &reverbSlot)) {
        return 0;
    }
    return soundproof ? 1 : 0;
}

static int audio_client_has_reset_state(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return 0;
    }
    if (g_snap[clientID].valid) {
        return 1;
    }
    cave_slots_ensure_init();
    if (g_reverbSlotByClient[clientID] >= 0) {
        return 1;
    }
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 0, 0) != 0) {
        return 1;
    }
    if (InterlockedCompareExchange(&g_recomputeDirty[clientID], 0, 0) != 0) {
        return 1;
    }
    return 0;
}

void ts3_audio_reset(void) {
    cave_slots_ensure_init();
    for (int i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        if (audio_client_has_reset_state((anyID)i)) {
            ts3_audio_invalidate_client((anyID)i);
        }
    }
    /* Cave slots can outlive g_reverbSlotByClient if a race cleared the map. */
    for (int s = 0; s < CAVE_SLOTS; s++) {
        const LONG owner = InterlockedCompareExchange(&g_cave[s].owner, 0, 0);
        if (owner > 0 && ts3_client_id_valid((anyID)owner)
            && g_reverbSlotByClient[owner] < 0) {
            ts3_audio_invalidate_client((anyID)owner);
        }
    }
    /* Reconcile any flags invalidate missed (counter/flag drift). */
    for (int i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        InterlockedExchange(&g_pendingUnmute[i], 0);
    }
    InterlockedExchange(&g_pendingUnmuteCount, 0);
    memset(g_hearInRange, 0, sizeof(g_hearInRange)); /* §8 latch: all out of range */
    while (InterlockedCompareExchange(&g_unmuteRingPushLock, 1, 0) != 0) {
        YieldProcessor();
    }
    InterlockedExchange(&g_unmuteRingWrite, 0);
    InterlockedExchange(&g_unmuteRingRead, 0);
    InterlockedExchange(&g_unmuteRingOverflow, 0);
    memset(g_unmuteRing, 0, sizeof(g_unmuteRing));
    InterlockedExchange(&g_unmuteRingPushLock, 0);
    InterlockedExchange(&g_recomputeDirtyCount, 0);
    InterlockedExchange(&g_recomputeAllPending, 0);
    InterlockedExchange(&g_lastLocalSeq, -1);
    InterlockedExchange64(&g_lastRecomputeAllMs, 0);
    /* Ramp/pan/LPF state lives on the audio thread. Bump every client's
       generation counter so the next PCM buffer for any (possibly reused) ID
       reinitializes its own ramp/LPF — even if no callback ran for it and even
       if its snapshot was already invalid (the reused-ID leak the old full
       plain-write sweep guarded against). This replaces that O(TS3_MAX_CLIENT_ID)
       write loop with one atomic per ID; it never touches the render arrays, so
       there is no race with the audio thread. The O(active) invalidate scan
       above (Phase 5.2) is unchanged. Render state is only ever consumed by the
       audio thread, so bumping the generation is sufficient: a NEW connection
       reusing an ID cannot inherit stale gain/pan/LPF. */
    for (int i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        InterlockedIncrement(&g_snapGeneration[i]);
    }
}
