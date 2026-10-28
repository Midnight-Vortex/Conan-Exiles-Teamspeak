#ifndef CORE_PROXIMITY_AUDIO_SNAP_H
#define CORE_PROXIMITY_AUDIO_SNAP_H

/*
 * audio_snap — seqlock snapshots between the callback thread (writer) and the
 * audio thread (reader). Header-only, pure C, no Win32 / TS coupling, so the
 * exact code the plugin runs is also unit-tested on the host
 * (tests/audio_fx_test.c, incl. a real writer-thread stress test).
 *
 * Thread contract:
 *  - audio_snap_publish / audio_dir_publish: ONE writer at a time (the plugin
 *    serializes all writers with its private g_writerLock). Never call these
 *    from the audio thread.
 *  - audio_snap_read / audio_dir_read: audio thread (or any reader). Lock-free,
 *    no allocation, bounded retries — never spins unbounded, never blocks.
 *
 * Seqlock in one sentence: the writer makes `seq` odd, writes the data, makes
 * `seq` even again; a reader copies the data and only trusts the copy when
 * `seq` was even and did not change while it copied.
 *
 * Tri-state read result (review fix 084 #6): the old reader returned 0 for BOTH
 * "snapshot is invalid" and "writer is busy / got preempted". The audio thread
 * then muted the speaker and reset its render state although nothing was wrong
 * (click / dropout). Now the two cases are told apart:
 *   AUDIO_SNAP_OK      stable copy, snapshot valid
 *   AUDIO_SNAP_INVALID stable copy, snapshot says "no proximity data" (mute)
 *   AUDIO_SNAP_BUSY    no stable copy within the retries (writer mid-write) —
 *                      the caller keeps using its last good values
 */

/* Compiler barriers: the data loads must stay BETWEEN the two `seq` loads.
   (`seq` is volatile, the data fields are plain — without a barrier the
   compiler may move the plain loads across the volatile ones.) */
#if defined(_MSC_VER)
#include <intrin.h>
#define AUDIO_SNAP_BARRIER()  _ReadWriteBarrier()
#define AUDIO_SNAP_BUMP(p)    ((void)_InterlockedIncrement((volatile long*)(p)))
#else
#define AUDIO_SNAP_BARRIER()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define AUDIO_SNAP_BUMP(p)    ((void)__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#endif

/* Bounded retries of one read (a write takes a few nanoseconds). */
#define AUDIO_SNAP_READ_ATTEMPTS 3

typedef enum AudioSnapRead {
    AUDIO_SNAP_BUSY    = -1,
    AUDIO_SNAP_INVALID = 0,
    AUDIO_SNAP_OK      = 1
} AudioSnapRead;

/* Target values the audio thread renders towards. */
typedef struct AudioTarget {
    float gain;
    float panL;
    float panR;
    float cutoffHz;      /* lowpass cutoff; >= bypass = off */
    float drr;           /* 1.0 = no diffuse; lower = more diffuse mix */
    int soundproof;      /* hard mute (different soundproof zone) */
    int reverbSlot;      /* cave reverb slot, -1 = off */
    float reverbWet;     /* 1 = cave, < 1 = indoor room reverb */
} AudioTarget;

typedef struct AudioSnap {
    volatile long seq;   /* odd = write in progress */
    AudioTarget t;
    int valid;
} AudioSnap;

/* Listener-relative direction (HRTF), radians. */
typedef struct AudioDir {
    float azimuth;
    float elevation;
} AudioDir;

typedef struct AudioDirSnap {
    volatile long seq;   /* odd = write in progress */
    AudioDir d;
    int valid;
} AudioDirSnap;

/* ---- writer side (callback thread, under the writer lock) -------------------- */

static inline void audio_snap_publish(AudioSnap* s, const AudioTarget* t, int valid) {
    AUDIO_SNAP_BUMP(&s->seq);          /* odd: write in progress */
    s->t = *t;
    s->valid = valid;
    AUDIO_SNAP_BUMP(&s->seq);          /* even: stable */
}

static inline void audio_dir_publish(AudioDirSnap* s, float azimuth, float elevation, int valid) {
    AUDIO_SNAP_BUMP(&s->seq);
    s->d.azimuth = azimuth;
    s->d.elevation = elevation;
    s->valid = valid;
    AUDIO_SNAP_BUMP(&s->seq);
}

/* ---- reader side (audio thread) --------------------------------------------- */

static inline AudioSnapRead audio_snap_read(const AudioSnap* s, AudioTarget* out) {
    for (int attempt = 0; attempt < AUDIO_SNAP_READ_ATTEMPTS; attempt++) {
        const long seqBefore = s->seq;
        if (seqBefore & 1) {
            continue;
        }
        AUDIO_SNAP_BARRIER();          /* data loads must not move above the seq load */
        const AudioTarget copy = s->t;
        const int valid = s->valid;
        AUDIO_SNAP_BARRIER();          /* ... nor below the validating seq load */
        if (s->seq == seqBefore) {
            if (!valid) {
                return AUDIO_SNAP_INVALID;
            }
            *out = copy;
            return AUDIO_SNAP_OK;
        }
    }
    return AUDIO_SNAP_BUSY;
}

static inline AudioSnapRead audio_dir_read(const AudioDirSnap* s, AudioDir* out) {
    for (int attempt = 0; attempt < AUDIO_SNAP_READ_ATTEMPTS; attempt++) {
        const long seqBefore = s->seq;
        if (seqBefore & 1) {
            continue;
        }
        AUDIO_SNAP_BARRIER();
        const AudioDir copy = s->d;
        const int valid = s->valid;
        AUDIO_SNAP_BARRIER();
        if (s->seq == seqBefore) {
            if (!valid) {
                return AUDIO_SNAP_INVALID;
            }
            *out = copy;
            return AUDIO_SNAP_OK;
        }
    }
    return AUDIO_SNAP_BUSY;
}

/* ---- expiry sweep (review fix 084 #2) ---------------------------------------- */

/* Collect client IDs 1..maxId-1 whose snapshot is still valid although the
   client is no longer part of the fresh player snapshot (`fresh[id] == 0`):
   the player-table entry expired (peer crashed / quit but stays in the channel)
   so the snapshot must be withdrawn ("expired -> mute"). Writer side: call
   under the writer lock so `valid` is stable. Returns the number written to
   `out` (at most outMax; the rest is found by the next sweep). */
static inline int audio_snap_find_orphans(const AudioSnap* snaps, const unsigned char* fresh,
    int maxId, unsigned short* out, int outMax) {
    int n = 0;
    for (int id = 1; id < maxId && n < outMax; id++) {
        if (snaps[id].valid && !fresh[id]) {
            out[n++] = (unsigned short)id;
        }
    }
    return n;
}

#endif /* CORE_PROXIMITY_AUDIO_SNAP_H */
