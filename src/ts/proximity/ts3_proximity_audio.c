#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"
#include "core/proximity/zone_resolve.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define TS3_AUDIO_MAX_CLIENT   65536  /* anyID is uint16 — index by clientID directly */
#define TS3_UNMUTE_REARM_MS    2000
#define TS3_UNMUTE_BATCH_MAX   64
#define TS3_AUDIBLE_GAIN       0.001f
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
    int soundproof;      /* 10.2 hard mute (different soundproof zone) */
    int reverbSlot;      /* 10.4 cave reverb slot, -1 = off */
    int valid;
} AudioSnap;

static AudioSnap g_snap[TS3_AUDIO_MAX_CLIENT];

/* Writer-side lock (callback thread + pos watcher). Audio thread never takes it. */
static CRITICAL_SECTION g_writerLock;
static INIT_ONCE g_writerLockOnce = INIT_ONCE_STATIC_INIT;

/* Render gain per client — audio thread private (ramping state). */
static float g_renderGain[TS3_AUDIO_MAX_CLIENT];

/* Unmute bookkeeping. Flags set from any thread, consumed on callback thread. */
static volatile long g_pendingUnmute[TS3_AUDIO_MAX_CLIENT];
static volatile long g_pendingUnmuteCount = 0;
static char g_clientUnlocked[TS3_AUDIO_MAX_CLIENT];       /* callback thread only */
static ULONGLONG g_lastUnmuteMs[TS3_AUDIO_MAX_CLIENT];    /* callback thread only */

static void audio_signal_unmute_flag_only(anyID clientID);

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
    float cutoffHz, int soundproof, int reverbSlot, int valid) {
    AudioSnap* s = &g_snap[clientID];
    InterlockedIncrement(&s->seq);          /* odd: write in progress */
    s->gain = gain;
    s->panL = panL;
    s->panR = panR;
    s->cutoffHz = cutoffHz;
    s->soundproof = soundproof;
    s->reverbSlot = reverbSlot;
    s->valid = valid;
    InterlockedIncrement(&s->seq);          /* even: stable */
}

/* Publish "no proximity influence" for one client. */
static void snap_publish_neutral(anyID clientID, int valid) {
    snap_publish(clientID, 0.0f, 0.7071f, 0.7071f, TS3_LPF_BYPASS_HZ, 0, -1, valid);
}

/* Audio-thread reader; returns 0 when no stable/valid snapshot exists. */
static int snap_read(anyID clientID, float* gain, float* panL, float* panR,
    float* cutoffHz, int* soundproof, int* reverbSlot) {
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

static LpfState g_lpf[TS3_AUDIO_MAX_CLIENT];

/* Schroeder cave reverb (comb + allpass), same tuning as the old plugin.
   Fixed static pool — no malloc anywhere near the audio path. Slots are
   assigned/released on the writer side; the audio thread only indexes. */
#define CAVE_SLOTS        8
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
static short g_reverbSlotByClient[TS3_AUDIO_MAX_CLIENT];
static int g_reverbSlotsInit = 0;

static void cave_slots_ensure_init(void) {
    if (!g_reverbSlotsInit) {
        for (int i = 0; i < TS3_AUDIO_MAX_CLIENT; i++) {
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

static void audio_compute_and_publish(anyID clientID, const PosSample* local,
    int localValid, const HubSettings* hub, int localZone) {
    PlayerEntry remote;
    if (!localValid || !player_table_get(clientID, &remote)) {
        cave_slot_release(clientID);
        snap_publish_neutral(clientID, 0);
        return;
    }

    const float lx = local->x / 100.0f;
    const float ly = local->y / 100.0f;
    const float lz = local->z / 100.0f;

    /* 10.1/10.2 zones: speaker in a foreign soundproof zone -> hard mute. */
    int soundproof = 0;
    int reverbSlot = -1;
    float cutoffHz = TS3_LPF_BYPASS_HZ;

    if (hub && hub->zoneCount > 0) {
        const int remoteZone = zone_resolve(hub, remote.x, remote.y, remote.z);
        soundproof = zone_soundproof_muted(hub, localZone, remoteZone);

        /* 10.3/10.4 effects only inside reverb zones (open world stays clean). */
        if (!soundproof && zone_reverb_active(hub, localZone, remoteZone)) {
            const float distance = prox_distance(lx, ly, lz, remote.x, remote.y, remote.z);
            cutoffHz = prox_lowpass_cutoff_hz(distance);
            reverbSlot = cave_slot_acquire(clientID);
        }
    }
    if (reverbSlot < 0) {
        cave_slot_release(clientID);
    }

    const float distance = prox_distance(lx, ly, lz, remote.x, remote.y, remote.z);
    /* Gain cap from the server profile (1.0 without profile, Phase 9).
       Race listener bonus: the local player's race hears others further
       (listenAddDistance extends the speaker's range on OUR side only). */
    const float hearRange = remote.voiceDistance
        + server_profile_get_listen_add_distance();
    const float gain = soundproof ? 0.0f
        : prox_volume_from_distance(distance, hearRange,
            server_profile_get_max_volume());

    const float yawRad = local->yaw * TS3_CEPOS_PI / 180.0f;
    const float dirX = -cosf(yawRad);
    const float dirZ = -sinf(yawRad);

    float panL, panR;
    prox_stereo_pan(dirX, dirZ, remote.x - lx, remote.z - lz, &panL, &panR);

    snap_publish(clientID, gain, panL, panR, cutoffHz, soundproof, reverbSlot, 1);

    if (gain > TS3_AUDIBLE_GAIN) {
        /* Flag only — caller may hold g_writerLock; ts3_request_wakeup is
           deferred to recompute_* after the lock is released. */
        audio_signal_unmute_flag_only(clientID);
    }
}

/* Shared writer context: hub settings + local zone (zone lookup only once). */
static int audio_local_zone(const HubSettings* hub, const PosSample* local,
    int localValid, int hubActive) {
    if (!hubActive || !localValid || hub->zoneCount <= 0) {
        return -1;
    }
    return zone_resolve(hub, local->x / 100.0f, local->y / 100.0f, local->z / 100.0f);
}

void ts3_audio_recompute_client(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
    }
    writer_lock_ensure();

    PosSample local;
    const int localValid = pos_get_current(&local);

    HubSettings hub;
    const int hubActive = server_profile_get(&hub);
    const int localZone = audio_local_zone(&hub, &local, localValid, hubActive);

    EnterCriticalSection(&g_writerLock);
    audio_compute_and_publish(clientID, &local, localValid,
        hubActive ? &hub : NULL, localZone);
    LeaveCriticalSection(&g_writerLock);

    if (InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0) {
        ts3_request_wakeup();
    }
}

void ts3_audio_recompute_all(void) {
    writer_lock_ensure();

    PosSample local;
    const int localValid = pos_get_current(&local);

    HubSettings hub;
    const int hubActive = server_profile_get(&hub);
    const int localZone = audio_local_zone(&hub, &local, localValid, hubActive);

    PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);

    EnterCriticalSection(&g_writerLock);
    for (int i = 0; i < count; i++) {
        if (players[i].clientID != 0 && players[i].clientID < TS3_AUDIO_MAX_CLIENT) {
            audio_compute_and_publish(players[i].clientID, &local, localValid,
                hubActive ? &hub : NULL, localZone);
        }
    }
    LeaveCriticalSection(&g_writerLock);

    if (InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0) {
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
    if (channels >= 2) {
        for (int s = 0; s < sampleCount; s++) {
            if (InterlockedCompareExchange(&rev->owner, 0, 0) != expectedOwner) {
                return;
            }
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
            if (InterlockedCompareExchange(&rev->owner, 0, 0) != expectedOwner) {
                return;
            }
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
        || clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
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

    float target, panL, panR, cutoffHz;
    int soundproof, reverbSlot;
    if (!snap_read(clientID, &target, &panL, &panR, &cutoffHz, &soundproof, &reverbSlot)) {
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

    /* Fully silent: skip FX and gain math entirely. */
    if (target <= TS3_AUDIBLE_GAIN && g_renderGain[clientID] <= TS3_AUDIBLE_GAIN) {
        memset(samples, 0, (size_t)sampleCount * (size_t)channels * sizeof(short));
        g_renderGain[clientID] = 0.0f;
        g_lpf[clientID].initialized = 0;
        return;
    }

    /* 10.3 distance lowpass (reverb zones only — bypass value elsewhere). */
    if (cutoffHz < TS3_LPF_BYPASS_HZ) {
        audio_apply_lowpass(samples, sampleCount, channels, cutoffHz, &g_lpf[clientID]);
    }
    else {
        g_lpf[clientID].initialized = 0;
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

    /* 6.5 click-free ramp: spread the gain change across this buffer. */
    float current = g_renderGain[clientID];
    const float step = (target - current) / (float)sampleCount;

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
            short* frame = samples + (size_t)i * channels;
            float vl = frame[0] * current * panL;
            float vr = frame[1] * current * panR;
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
}

/* ---- 6.3 unmute signal ------------------------------------------------------ */

/* Flag only — no wakeup. Safe on the audio thread (no TS API, no locks). */
static void audio_signal_unmute_flag_only(anyID clientID) {
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 1, 0) == 0) {
        InterlockedIncrement(&g_pendingUnmuteCount);
    }
}

void ts3_audio_signal_unmute(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
    }
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 1, 0) == 0) {
        InterlockedIncrement(&g_pendingUnmuteCount);
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

    const ULONGLONG now = GetTickCount64();
    anyID batch[TS3_UNMUTE_BATCH_MAX];
    int batchCount = 0;

    /* Loop counter must be wider than anyID (uint16): i < 65536 would be
       always true for a uint16 counter -> infinite loop on the callback
       thread (TS froze at startup before the main window appeared). */
    for (int i = 1; i < TS3_AUDIO_MAX_CLIENT && batchCount < TS3_UNMUTE_BATCH_MAX - 1; i++) {
        if (InterlockedCompareExchange(&g_pendingUnmute[i], 0, 0) == 0) {
            continue;
        }
        /* First unmute is immediate; re-unmute of an already unlocked client is
           rate-limited. Pending flag stays set until the API call really runs. */
        if (g_clientUnlocked[i] && now - g_lastUnmuteMs[i] < TS3_UNMUTE_REARM_MS) {
            continue;
        }
        batch[batchCount++] = (anyID)i;
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

    for (int i = 0; i < unmuted; i++) {
        const anyID id = batch[i];
        if (InterlockedCompareExchange(&g_pendingUnmute[id], 0, 1) == 1) {
            InterlockedDecrement(&g_pendingUnmuteCount);
        }
        g_clientUnlocked[id] = 1;
        g_lastUnmuteMs[id] = now;
    }
    log_debug("AUDIO: unmuted %d client(s)", unmuted);
    if (ts3_audio_has_pending_unmutes()) {
        ts3_request_wakeup();
    }
}

int ts3_audio_has_pending_unmutes(void) {
    return InterlockedCompareExchange(&g_pendingUnmuteCount, 0, 0) > 0;
}

/* ---- cleanup ----------------------------------------------------------------- */

void ts3_audio_invalidate_client(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
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
    g_renderGain[clientID] = 1.0f;
}

int ts3_proximity_audio_soundproof_muted(unsigned int clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return 0;
    }
    float gain, panL, panR, cutoffHz;
    int soundproof, reverbSlot;
    if (!snap_read((anyID)clientID, &gain, &panL, &panR, &cutoffHz, &soundproof, &reverbSlot)) {
        return 0;
    }
    return soundproof ? 1 : 0;
}

void ts3_audio_reset(void) {
    /* int counter, NOT anyID: anyID is uint16, so "i < 65536" never turns
       false and the callback thread spins forever (startup freeze). */
    for (int i = 1; i < TS3_AUDIO_MAX_CLIENT; i++) {
        ts3_audio_invalidate_client((anyID)i);
    }
    /* Reconcile any flags invalidate missed (counter/flag drift). */
    for (int i = 1; i < TS3_AUDIO_MAX_CLIENT; i++) {
        InterlockedExchange(&g_pendingUnmute[i], 0);
    }
    InterlockedExchange(&g_pendingUnmuteCount, 0);
}
