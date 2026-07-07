#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/adapter/ts3_adapter.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define TS3_AUDIO_MAX_CLIENT   4096
#define TS3_UNMUTE_REARM_MS    2000
#define TS3_UNMUTE_BATCH_MAX   64
#define TS3_AUDIBLE_GAIN       0.001f
#define TS3_CEPOS_PI           3.14159265f

/* ---- 6.1 seqlock snapshots ------------------------------------------------ */

typedef struct AudioSnap {
    volatile LONG seq;   /* odd = write in progress */
    float gain;
    float panL;
    float panR;
    int valid;
} AudioSnap;

static AudioSnap g_snap[TS3_AUDIO_MAX_CLIENT];

/* Writer-side lock (callback thread + pos watcher). Audio thread never takes it. */
static CRITICAL_SECTION g_writerLock;
static volatile long g_writerLockReady = 0;

/* Render gain per client — audio thread private (ramping state). */
static float g_renderGain[TS3_AUDIO_MAX_CLIENT];

/* Unmute bookkeeping. Flags set from any thread, consumed on callback thread. */
static volatile long g_pendingUnmute[TS3_AUDIO_MAX_CLIENT];
static volatile long g_pendingUnmuteCount = 0;
static char g_clientUnlocked[TS3_AUDIO_MAX_CLIENT];       /* callback thread only */
static ULONGLONG g_lastUnmuteMs[TS3_AUDIO_MAX_CLIENT];    /* callback thread only */

static void audio_signal_unmute_flag_only(anyID clientID);

static void writer_lock_ensure(void) {
    if (InterlockedCompareExchange(&g_writerLockReady, 0, 0)) {
        return;
    }
    InitializeCriticalSection(&g_writerLock);
    InterlockedExchange(&g_writerLockReady, 1);
}

static void snap_publish(anyID clientID, float gain, float panL, float panR, int valid) {
    AudioSnap* s = &g_snap[clientID];
    InterlockedIncrement(&s->seq);          /* odd: write in progress */
    s->gain = gain;
    s->panL = panL;
    s->panR = panR;
    s->valid = valid;
    InterlockedIncrement(&s->seq);          /* even: stable */
}

/* Audio-thread reader; returns 0 when no stable/valid snapshot exists. */
static int snap_read(anyID clientID, float* gain, float* panL, float* panR) {
    const AudioSnap* s = &g_snap[clientID];
    for (int attempt = 0; attempt < 3; attempt++) {
        const LONG seqBefore = s->seq;
        if (seqBefore & 1) {
            continue;
        }
        const float g = s->gain;
        const float l = s->panL;
        const float r = s->panR;
        const int valid = s->valid;
        if (s->seq == seqBefore) {
            if (!valid) {
                return 0;
            }
            *gain = g;
            *panL = l;
            *panR = r;
            return 1;
        }
    }
    return 0;
}

/* ---- gain/pan computation (writer side) ----------------------------------- */

static void audio_compute_and_publish(anyID clientID, const PosSample* local,
    int localValid) {
    PlayerEntry remote;
    if (!localValid || !player_table_get(clientID, &remote)) {
        snap_publish(clientID, 0.0f, 0.7071f, 0.7071f, 0);
        return;
    }

    const float lx = local->x / 100.0f;
    const float ly = local->y / 100.0f;
    const float lz = local->z / 100.0f;

    const float distance = prox_distance(lx, ly, lz, remote.x, remote.y, remote.z);
    const float gain = prox_volume_from_distance(distance, remote.voiceDistance, 1.0f);

    const float yawRad = local->yaw * TS3_CEPOS_PI / 180.0f;
    const float dirX = -cosf(yawRad);
    const float dirZ = -sinf(yawRad);

    float panL, panR;
    prox_stereo_pan(dirX, dirZ, remote.x - lx, remote.z - lz, &panL, &panR);

    snap_publish(clientID, gain, panL, panR, 1);

    if (gain > TS3_AUDIBLE_GAIN) {
        ts3_audio_signal_unmute(clientID);
    }
}

void ts3_audio_recompute_client(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
    }
    writer_lock_ensure();

    PosSample local;
    const int localValid = pos_get_current(&local);

    EnterCriticalSection(&g_writerLock);
    audio_compute_and_publish(clientID, &local, localValid);
    LeaveCriticalSection(&g_writerLock);
}

void ts3_audio_recompute_all(void) {
    writer_lock_ensure();

    PosSample local;
    const int localValid = pos_get_current(&local);

    PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);

    EnterCriticalSection(&g_writerLock);
    for (int i = 0; i < count; i++) {
        if (players[i].clientID != 0 && players[i].clientID < TS3_AUDIO_MAX_CLIENT) {
            audio_compute_and_publish(players[i].clientID, &local, localValid);
        }
    }
    LeaveCriticalSection(&g_writerLock);
}

/* ---- 6.2 PCM hot path (audio thread) --------------------------------------- */

void ts3_audio_process_playback(anyID clientID, short* samples, int sampleCount, int channels) {
    if (!samples || sampleCount <= 0 || channels <= 0
        || clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
    }

    float target, panL, panR;
    if (!snap_read(clientID, &target, &panL, &panR)) {
        /* No proximity data — leave audio untouched (normal TS behavior). */
        g_renderGain[clientID] = 1.0f;
        return;
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

    for (anyID i = 1; i < TS3_AUDIO_MAX_CLIENT && batchCount < TS3_UNMUTE_BATCH_MAX - 1; i++) {
        if (InterlockedCompareExchange(&g_pendingUnmute[i], 0, 0) == 0) {
            continue;
        }
        /* First unmute is immediate; re-unmute of an already unlocked client is
           rate-limited. Pending flag stays set until the API call really runs. */
        if (g_clientUnlocked[i] && now - g_lastUnmuteMs[i] < TS3_UNMUTE_REARM_MS) {
            continue;
        }
        batch[batchCount++] = i;
    }

    if (batchCount == 0) {
        return;
    }

    const int unmuted = ts3_unmute_clients_for_pcm(batch, batchCount);
    if (unmuted <= 0) {
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
}

/* ---- cleanup ----------------------------------------------------------------- */

void ts3_audio_invalidate_client(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_AUDIO_MAX_CLIENT) {
        return;
    }
    snap_publish(clientID, 0.0f, 0.7071f, 0.7071f, 0);
    if (InterlockedCompareExchange(&g_pendingUnmute[clientID], 0, 1) == 1) {
        InterlockedDecrement(&g_pendingUnmuteCount);
    }
    g_clientUnlocked[clientID] = 0;
    g_lastUnmuteMs[clientID] = 0;
    g_renderGain[clientID] = 1.0f;
}

void ts3_audio_reset(void) {
    for (anyID i = 1; i < TS3_AUDIO_MAX_CLIENT; i++) {
        if (g_snap[i].valid || g_pendingUnmute[i] || g_clientUnlocked[i]) {
            ts3_audio_invalidate_client(i);
        }
    }
    InterlockedExchange(&g_pendingUnmuteCount, 0);
}
