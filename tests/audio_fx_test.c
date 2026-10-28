/* Unit tests for the header-only audio helpers (review fix 084):
     src/core/proximity/audio_fx.h    cave reverb (#4) + lowpass (#5)
     src/core/proximity/audio_snap.h  seqlock snapshots, tri-state read (#6), expiry sweep (#2)
     src/core/proximity/voice_cap.h   cap for a peer's announced voice distance (#9)
     src/ts/proximity/ts3_proximity_audio.h  render_gap_exceeded / render_ramp_start (#3)

   Build/run on Linux: bash tests/run_tests.sh (plain gcc, -Isdk/include; no Win32,
   no TS coupling). Exit code 0 = all checks passed.

   The key proof of this file: the cave reverb and the lowpass keep the SOUND
   BIT-IDENTICAL to the in-game-tested pre-fix code. The test carries a verbatim
   copy of the OLD algorithms ("old_*") and compares float-for-float. */

#include "core/proximity/audio_fx.h"
#include "core/proximity/audio_snap.h"
#include "core/proximity/voice_cap.h"
#include "ts/proximity/ts3_proximity_audio.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* Deterministic pseudo random, no libc rand() state. */
static unsigned int g_rng = 12345u;
static int rnd_s16(int amplitude) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (int)((g_rng >> 8) % (unsigned)(2 * amplitude + 1)) - amplitude;
}

/* =========================================================================
 * OLD cave reverb — verbatim pre-fix algorithm: L and R share combPos[c] /
 * apPos[a] (advance by 2 per sample), designed lengths {2546,3507,4257,5466}.
 * ========================================================================= */
#define OLD_COMB_MAX 5466
static const int OLD_COMB_LEN[CAVE_COMBS] = { 2546, 3507, 4257, 5466 };

typedef struct OldCave {
    float combL[CAVE_COMBS][OLD_COMB_MAX];
    float combR[CAVE_COMBS][OLD_COMB_MAX];
    int combPos[CAVE_COMBS];
    float combLpL[CAVE_COMBS];
    float combLpR[CAVE_COMBS];
    float apL[CAVE_ALLPASS][CAVE_AP_MAX];
    float apR[CAVE_ALLPASS][CAVE_AP_MAX];
    int apPos[CAVE_ALLPASS];
    float predelayL[CAVE_PREDELAY];
    float predelayR[CAVE_PREDELAY];
    int predelayPos;
} OldCave;

static void old_cave_process_sample(OldCave* rev, float inL, float inR,
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
        outL += cave_comb(rev->combL[c], OLD_COMB_LEN[c], &rev->combPos[c],
            revInL, CAVE_COMB_FB[c], &rev->combLpL[c]);
        outR += cave_comb(rev->combR[c], OLD_COMB_LEN[c], &rev->combPos[c],
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

/* Runs old and new side by side; returns the number of samples that differ
   (bitwise) in wetL or wetR. */
static long compare_cave(const float* inL, const float* inR, int n) {
    static OldCave oldRev;
    static CaveDsp newRev;
    memset(&oldRev, 0, sizeof(oldRev));
    cave_dsp_reset(&newRev);
    long diffs = 0;
    for (int i = 0; i < n; i++) {
        float oL, oR, nL, nR;
        old_cave_process_sample(&oldRev, inL[i], inR[i], &oL, &oR);
        cave_dsp_process_sample(&newRev, inL[i], inR[i], &nL, &nR);
        if (memcmp(&oL, &nL, sizeof(float)) != 0 || memcmp(&oR, &nR, sizeof(float)) != 0) {
            diffs++;
        }
    }
    return diffs;
}

static void test_cave_identical(void) {
    printf("[1] cave reverb: new (per-channel positions, effective lengths) == old, bit for bit\n");

    enum { N = 400000 };   /* ~8 s @ 48 kHz: many wraps of every delay line */
    static float inL[N], inR[N];

    /* a) single impulse per channel, silence afterwards: shows every delay tap */
    memset(inL, 0, sizeof(inL));
    memset(inR, 0, sizeof(inR));
    inL[0] = 30000.0f;
    inR[3] = -25000.0f;
    CHECK(compare_cave(inL, inR, N) == 0, "impulse response identical");

    /* b) white noise on both channels */
    for (int i = 0; i < N; i++) {
        inL[i] = (float)rnd_s16(20000);
        inR[i] = (float)rnd_s16(20000);
    }
    CHECK(compare_cave(inL, inR, N) == 0, "stereo noise identical");

    /* c) mono signal (same samples on both channels) with a pause: burst, silence, burst */
    for (int i = 0; i < N; i++) {
        const int inBurst = (i < 60000) || (i >= 200000 && i < 230000);
        inL[i] = inBurst ? (float)rnd_s16(15000) : 0.0f;
        inR[i] = inL[i];
    }
    CHECK(compare_cave(inL, inR, N) == 0, "mono bursts with pause identical");

    /* d) the effective lengths really are the old effective delays */
    CHECK(CAVE_COMB_LEN[0] == OLD_COMB_LEN[0] / 2, "comb 0: even 2546 acted as 1273");
    CHECK(CAVE_COMB_LEN[1] == OLD_COMB_LEN[1], "comb 1: odd 3507 unchanged");
    CHECK(CAVE_COMB_LEN[2] == OLD_COMB_LEN[2], "comb 2: odd 4257 unchanged");
    CHECK(CAVE_COMB_LEN[3] == OLD_COMB_LEN[3] / 2, "comb 3: even 5466 acted as 2733");
}

static void test_cave_reset(void) {
    printf("[1b] cave_dsp_reset: a reset reverb behaves like a fresh one\n");
    static CaveDsp used, fresh;
    cave_dsp_reset(&used);
    cave_dsp_reset(&fresh);
    float wl, wr;
    for (int i = 0; i < 30000; i++) {
        cave_dsp_process_sample(&used, (float)rnd_s16(20000), (float)rnd_s16(20000), &wl, &wr);
    }
    cave_dsp_reset(&used);
    long diffs = 0;
    for (int i = 0; i < 30000; i++) {
        const float a = (float)rnd_s16(20000), b = (float)rnd_s16(20000);
        float ul, ur, fl, fr;
        cave_dsp_process_sample(&used, a, b, &ul, &ur);
        cave_dsp_process_sample(&fresh, a, b, &fl, &fr);
        if (memcmp(&ul, &fl, sizeof(float)) || memcmp(&ur, &fr, sizeof(float))) {
            diffs++;
        }
    }
    CHECK(diffs == 0, "no tail of the previous utterance survives a reset");
}

/* =========================================================================
 * OLD lowpass — verbatim pre-fix code (initialized set after the loop, dead
 * second pass).
 * ========================================================================= */
typedef struct OldLpf {
    float prevL, prevR;
    float alpha;
    float lastCutoff;
    int initialized;
} OldLpf;

static void old_lowpass(short* samples, int sampleCount, int channels,
    float cutoffHz, OldLpf* fx) {
    if (cutoffHz != fx->lastCutoff) {
        const float dt = 1.0f / 48000.0f;
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
            samples[li] = audio_fx_clamp_sample(fx->prevL);
            samples[ri] = audio_fx_clamp_sample(fx->prevR);
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
            samples[s] = audio_fx_clamp_sample(fx->prevL);
        }
    }
    fx->initialized = 1;
}

#define LPF_BLOCK 960   /* 20 ms @ 48 kHz */

static void fill_noise(short* buf, int count, int amplitude) {
    for (int i = 0; i < count; i++) {
        buf[i] = (short)rnd_s16(amplitude);
    }
}

static void test_lpf_first_buffer(void) {
    printf("[2] lowpass: the first buffer after (re)init is filtered now (fix 5a)\n");

    /* Step input: 0 ... 0 then a constant 10000. A one-pole filter must lag. */
    short stepOld[LPF_BLOCK * 2], stepNew[LPF_BLOCK * 2], orig[LPF_BLOCK * 2];
    for (int i = 0; i < LPF_BLOCK; i++) {
        orig[2 * i] = (short)(i < 100 ? 0 : 10000);
        orig[2 * i + 1] = (short)(i < 100 ? 0 : -10000);
    }
    memcpy(stepOld, orig, sizeof(orig));
    memcpy(stepNew, orig, sizeof(orig));

    OldLpf o; memset(&o, 0, sizeof(o));
    AudioLpf n; memset(&n, 0, sizeof(n));
    old_lowpass(stepOld, LPF_BLOCK, 2, 900.0f, &o);
    audio_lpf_apply(stepNew, LPF_BLOCK, 2, 900.0f, 48000.0f, &n);

    CHECK(memcmp(stepOld, orig, sizeof(orig)) == 0,
        "OLD code: first buffer passed through unfiltered (the bug)");
    CHECK(memcmp(stepNew, orig, sizeof(orig)) != 0, "new code: first buffer is filtered");
    CHECK(stepNew[2 * 100] > 0 && stepNew[2 * 100] < 2000,
        "new: step edge is smoothed (first filtered sample far below 10000)");
    CHECK(stepNew[2 * (LPF_BLOCK - 1)] > 9000, "new: output converges to the input level");
    CHECK(n.initialized == 1, "initialized set immediately");

    /* The filter memory is seeded from the first sample: a DC input must pass
       unchanged from the very first sample (no start-up transient from 0). */
    short dc[LPF_BLOCK * 2];
    for (int i = 0; i < LPF_BLOCK * 2; i++) dc[i] = 8000;
    AudioLpf d; memset(&d, 0, sizeof(d));
    audio_lpf_apply(dc, LPF_BLOCK, 2, 900.0f, 48000.0f, &d);
    int dcOk = 1;
    for (int i = 0; i < LPF_BLOCK * 2; i++) {
        if (dc[i] != 8000) { dcOk = 0; break; }
    }
    CHECK(dcOk, "seeded from first sample: DC passes unchanged from sample 0");

    /* Mono */
    short monoOld[LPF_BLOCK], monoNew[LPF_BLOCK], monoOrig[LPF_BLOCK];
    for (int i = 0; i < LPF_BLOCK; i++) monoOrig[i] = (short)(i < 100 ? 0 : 10000);
    memcpy(monoOld, monoOrig, sizeof(monoOrig));
    memcpy(monoNew, monoOrig, sizeof(monoOrig));
    OldLpf om; memset(&om, 0, sizeof(om));
    AudioLpf nm; memset(&nm, 0, sizeof(nm));
    old_lowpass(monoOld, LPF_BLOCK, 1, 900.0f, &om);
    audio_lpf_apply(monoNew, LPF_BLOCK, 1, 900.0f, 48000.0f, &nm);
    CHECK(memcmp(monoOld, monoOrig, sizeof(monoOrig)) == 0, "OLD mono: first buffer unfiltered");
    CHECK(memcmp(monoNew, monoOrig, sizeof(monoOrig)) != 0, "new mono: first buffer filtered");
}

static void test_lpf_identical_afterwards(void) {
    printf("[3] lowpass: with the same state the output is identical to the old code (fix 5b)\n");

    const float cutoffs[] = { 900.0f, 2500.0f, 8000.0f, 300.0f };
    for (int ci = 0; ci < 4; ci++) {
        for (int channels = 1; channels <= 3; channels++) {
            OldLpf o; AudioLpf n;
            memset(&o, 0, sizeof(o));
            memset(&n, 0, sizeof(n));
            /* Both start "initialized" with the same memory, so the first-buffer
               difference (5a) is out of the picture: this isolates the dead
               second pass. */
            o.initialized = 1; n.initialized = 1;
            o.prevL = n.prevL = 123.0f; o.prevR = n.prevR = -456.0f;

            long diffs = 0;
            static short bufO[LPF_BLOCK * 3], bufN[LPF_BLOCK * 3];
            for (int block = 0; block < 60; block++) {
                const int count = LPF_BLOCK * channels;
                fill_noise(bufO, count, 12000);
                memcpy(bufN, bufO, sizeof(short) * (size_t)count);
                old_lowpass(bufO, LPF_BLOCK, channels, cutoffs[ci], &o);
                audio_lpf_apply(bufN, LPF_BLOCK, channels, cutoffs[ci], 48000.0f, &n);
                if (memcmp(bufO, bufN, sizeof(short) * (size_t)count) != 0) diffs++;
            }
            char name[96];
            snprintf(name, sizeof(name), "cutoff %.0f Hz, %d channel(s): 60 blocks identical",
                cutoffs[ci], channels);
            CHECK(diffs == 0, name);
        }
    }

    /* After the (unfiltered) old first buffer, hand the old state to the new
       filter: from the second buffer on both are identical. */
    OldLpf o; memset(&o, 0, sizeof(o));
    AudioLpf n; memset(&n, 0, sizeof(n));
    static short a[LPF_BLOCK * 2], b[LPF_BLOCK * 2];
    fill_noise(a, LPF_BLOCK * 2, 10000);
    old_lowpass(a, LPF_BLOCK, 2, 1200.0f, &o);                /* old first buffer */
    n.prevL = o.prevL; n.prevR = o.prevR; n.alpha = o.alpha;
    n.lastCutoff = o.lastCutoff; n.initialized = 1;
    long diffs = 0;
    for (int block = 0; block < 40; block++) {
        fill_noise(a, LPF_BLOCK * 2, 10000);
        memcpy(b, a, sizeof(a));
        old_lowpass(a, LPF_BLOCK, 2, 1200.0f, &o);
        audio_lpf_apply(b, LPF_BLOCK, 2, 1200.0f, 48000.0f, &n);
        if (memcmp(a, b, sizeof(a)) != 0) diffs++;
    }
    CHECK(diffs == 0, "second buffer onwards identical (state handed over)");

    /* Channels beyond the first two are not touched. */
    short tri[9] = { 1000, 2000, 3000, -1000, -2000, -3000, 500, 600, 700 };
    AudioLpf t; memset(&t, 0, sizeof(t));
    audio_lpf_apply(tri, 3, 3, 900.0f, 48000.0f, &t);
    CHECK(tri[2] == 3000 && tri[5] == -3000 && tri[8] == 700, "3rd channel untouched");
}

/* =========================================================================
 * Seqlock snapshots
 * ========================================================================= */
static AudioTarget make_target(float v) {
    AudioTarget t;
    t.gain = v;
    t.panL = v + 1.0f;
    t.panR = v + 2.0f;
    t.cutoffHz = v + 3.0f;
    t.drr = v + 4.0f;
    t.soundproof = ((int)v) & 1;
    t.reverbSlot = (int)v;
    t.reverbWet = v + 5.0f;
    return t;
}

static int target_consistent(const AudioTarget* t) {
    const float v = t->gain;
    return t->panL == v + 1.0f && t->panR == v + 2.0f && t->cutoffHz == v + 3.0f
        && t->drr == v + 4.0f && t->soundproof == (((int)v) & 1)
        && t->reverbSlot == (int)v && t->reverbWet == v + 5.0f;
}

static void test_snap_tristate(void) {
    printf("[4] seqlock: OK / INVALID / BUSY are told apart (fix 6)\n");
    AudioSnap s;
    memset(&s, 0, sizeof(s));
    AudioTarget out;

    CHECK(audio_snap_read(&s, &out) == AUDIO_SNAP_INVALID, "never published -> INVALID");

    const AudioTarget t = make_target(42.0f);
    audio_snap_publish(&s, &t, 1);
    memset(&out, 0, sizeof(out));
    CHECK(audio_snap_read(&s, &out) == AUDIO_SNAP_OK, "published valid -> OK");
    CHECK(memcmp(&out, &t, sizeof(t)) == 0, "all fields round-trip");

    audio_snap_publish(&s, &t, 0);
    CHECK(audio_snap_read(&s, &out) == AUDIO_SNAP_INVALID, "published invalid -> INVALID");

    /* Writer preempted mid-write: seq is odd. The old reader returned 0 here,
       exactly like INVALID (mute + state reset). Now it is BUSY. */
    audio_snap_publish(&s, &t, 1);
    s.seq += 1;                       /* writer entered the critical section */
    CHECK(audio_snap_read(&s, &out) == AUDIO_SNAP_BUSY, "odd seq (writer mid-write) -> BUSY");
    CHECK(audio_snap_read(&s, &out) != AUDIO_SNAP_INVALID, "BUSY is not INVALID");
    s.seq += 1;                       /* writer done */
    CHECK(audio_snap_read(&s, &out) == AUDIO_SNAP_OK, "writer done -> OK again");

    /* Same for the direction snapshot. */
    AudioDirSnap d;
    memset(&d, 0, sizeof(d));
    AudioDir dir;
    CHECK(audio_dir_read(&d, &dir) == AUDIO_SNAP_INVALID, "dir: never published -> INVALID");
    audio_dir_publish(&d, 0.5f, -0.25f, 1);
    CHECK(audio_dir_read(&d, &dir) == AUDIO_SNAP_OK && dir.azimuth == 0.5f && dir.elevation == -0.25f,
        "dir: OK round-trip");
    d.seq += 1;
    CHECK(audio_dir_read(&d, &dir) == AUDIO_SNAP_BUSY, "dir: odd seq -> BUSY");
    d.seq += 1;
    audio_dir_publish(&d, 0.0f, 0.0f, 0);
    CHECK(audio_dir_read(&d, &dir) == AUDIO_SNAP_INVALID, "dir: invalid publish -> INVALID");
}

typedef struct StressCtx {
    AudioSnap* snap;
    AudioDirSnap* dir;
    volatile int stop;
} StressCtx;

static void* stress_writer(void* arg) {
    StressCtx* ctx = (StressCtx*)arg;
    float v = 0.0f;
    while (!ctx->stop) {
        const AudioTarget t = make_target(v);
        audio_snap_publish(ctx->snap, &t, 1);
        audio_dir_publish(ctx->dir, v, -v, 1);
        v += 1.0f;
        if (v > 1000000.0f) v = 0.0f;
    }
    return NULL;
}

static void test_snap_stress(void) {
    printf("[5] seqlock: real writer thread, no torn OK reads\n");
    static AudioSnap snap;
    static AudioDirSnap dir;
    memset(&snap, 0, sizeof(snap));
    memset(&dir, 0, sizeof(dir));
    const AudioTarget t0 = make_target(0.0f);
    audio_snap_publish(&snap, &t0, 1);
    audio_dir_publish(&dir, 0.0f, 0.0f, 1);

    StressCtx ctx = { &snap, &dir, 0 };
    pthread_t th;
    pthread_create(&th, NULL, stress_writer, &ctx);

    long ok = 0, busy = 0, invalid = 0, torn = 0, dirOk = 0, dirBusy = 0, dirTorn = 0;
    for (long i = 0; i < 3000000; i++) {
        AudioTarget out;
        switch (audio_snap_read(&snap, &out)) {
        case AUDIO_SNAP_OK:
            ok++;
            if (!target_consistent(&out)) torn++;
            break;
        case AUDIO_SNAP_BUSY: busy++; break;
        default: invalid++; break;
        }
        AudioDir dd;
        const AudioSnapRead r = audio_dir_read(&dir, &dd);
        if (r == AUDIO_SNAP_OK) {
            dirOk++;
            if (dd.elevation != -dd.azimuth) dirTorn++;
        }
        else if (r == AUDIO_SNAP_BUSY) {
            dirBusy++;
        }
    }
    ctx.stop = 1;
    pthread_join(th, NULL);
    printf("       (info) snap: ok=%ld busy=%ld invalid=%ld | dir: ok=%ld busy=%ld\n",
        ok, busy, invalid, dirOk, dirBusy);
    CHECK(torn == 0, "no torn snapshot returned as OK");
    CHECK(dirTorn == 0, "no torn direction returned as OK");
    CHECK(invalid == 0, "a busy writer never reads as INVALID");
    CHECK(ok > 0 && dirOk > 0, "readers got stable copies");
}

static void test_orphans(void) {
    printf("[6] expiry sweep: valid snapshots without a fresh player are found (fix 2)\n");
    static AudioSnap snaps[64];
    memset(snaps, 0, sizeof(snaps));
    unsigned char fresh[64];
    memset(fresh, 0, sizeof(fresh));
    const AudioTarget t = make_target(1.0f);
    audio_snap_publish(&snaps[3], &t, 1);
    audio_snap_publish(&snaps[5], &t, 1);
    audio_snap_publish(&snaps[7], &t, 1);
    audio_snap_publish(&snaps[9], &t, 0);   /* already invalid: nothing to withdraw */
    fresh[5] = 1;

    unsigned short out[8];
    int n = audio_snap_find_orphans(snaps, fresh, 64, out, 8);
    CHECK(n == 2 && out[0] == 3 && out[1] == 7, "expired clients 3 and 7 found; fresh 5 and invalid 9 not");

    n = audio_snap_find_orphans(snaps, fresh, 64, out, 1);
    CHECK(n == 1 && out[0] == 3, "output limit respected (rest comes with the next sweep)");

    fresh[3] = 1; fresh[7] = 1;
    n = audio_snap_find_orphans(snaps, fresh, 64, out, 8);
    CHECK(n == 0, "all fresh -> nothing to withdraw");

    /* id 0 is never a client */
    audio_snap_publish(&snaps[0], &t, 1);
    memset(fresh, 0, sizeof(fresh));
    n = audio_snap_find_orphans(snaps, fresh, 64, out, 8);
    int has0 = 0;
    for (int i = 0; i < n; i++) if (out[i] == 0) has0 = 1;
    CHECK(!has0, "client ID 0 is never reported");
}

/* =========================================================================
 * Voice distance cap (fix 9)
 * ========================================================================= */
static float feq_abs(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 0.0001f;
}

static void test_voice_cap(void) {
    printf("[7] voice cap: largest legit announced distance (fix 9)\n");
    static HubSettings hub;
    memset(&hub, 0, sizeof(hub));

    CHECK(voice_distance_cap(NULL) == 0.0f, "no hub -> no cap");
    CHECK(voice_distance_cap(&hub) == 0.0f, "hub without [GLOBAL] (valid=0) -> no cap");

    hub.valid = 1;
    CHECK(voice_distance_cap(&hub) == 0.0f, "profile without any limit -> no cap");

    hub.maxWhisper = 10.0f; hub.maxNormal = 30.0f;
    CHECK(voice_distance_cap(&hub) == 0.0f, "shout has no max -> not capturable -> no cap");

    hub.maxShout = 100.0f;
    CHECK(feq_abs(voice_distance_cap(&hub), 100.0f), "all three limited -> cap = largest max");

    hub.maxWhisper = 150.0f;
    CHECK(feq_abs(voice_distance_cap(&hub), 150.0f), "cap follows the largest, whatever the mode");

    /* A race may have larger limits than the global ones. */
    hub.raceCount = 1;
    hub.races[0].maxWhisper = 20.0f; hub.races[0].maxNormal = 60.0f; hub.races[0].maxShout = 250.0f;
    CHECK(feq_abs(voice_distance_cap(&hub), 250.0f), "race with higher max raises the cap");

    /* A race without a max for one mode is unlimited in that mode. */
    hub.races[0].maxNormal = 0.0f;
    CHECK(voice_distance_cap(&hub) == 0.0f, "race mode without max -> no cap");
    hub.races[0].maxNormal = 60.0f;

    /* Zone overrides can only raise the cap (never lower it). */
    hub.zoneCount = 2;
    hub.zones[0].normalDist = 400.0f;
    hub.zones[1].shoutDist = 90.0f;
    CHECK(feq_abs(voice_distance_cap(&hub), 400.0f), "zone distance above the limits raises the cap");
    hub.zones[0].normalDist = 5.0f;
    CHECK(feq_abs(voice_distance_cap(&hub), 250.0f), "zone distance below the limits changes nothing");

    /* Clamp */
    CHECK(voice_distance_clamp(1000.0f, 0.0f) == 1000.0f, "cap 0 (none) leaves the value alone");
    CHECK(voice_distance_clamp(1000.0f, 100.0f) == 100.0f, "modified client's 1000 m is cut to the cap");
    CHECK(voice_distance_clamp(100.0f, 100.0f) == 100.0f, "value == cap is kept");
    CHECK(voice_distance_clamp(35.0f, 100.0f) == 35.0f, "value below the cap is kept");
    CHECK(voice_distance_clamp(0.0f, 100.0f) == 0.0f, "0 stays 0");
}

/* =========================================================================
 * Render helpers in ts3_proximity_audio.h (fix 3)
 * ========================================================================= */
static void test_render_helpers(void) {
    printf("[8] render_gap_exceeded / render_ramp_start (fix 3)\n");
    CHECK(render_gap_exceeded(0, 5000, TS3_RENDER_GAP_MS), "never rendered (0) -> fresh start");
    CHECK(!render_gap_exceeded(5000, 5020, TS3_RENDER_GAP_MS), "20 ms later -> same utterance");
    CHECK(!render_gap_exceeded(5000, 5200, TS3_RENDER_GAP_MS), "exactly 200 ms -> still same utterance");
    CHECK(render_gap_exceeded(5000, 5201, TS3_RENDER_GAP_MS), "201 ms -> new utterance");
    CHECK(render_gap_exceeded(5000, 90000, TS3_RENDER_GAP_MS), "long pause -> new utterance");
    CHECK(render_gap_exceeded(5000, 4000, TS3_RENDER_GAP_MS), "clock going backwards -> treated as new utterance");

    CHECK(render_ramp_start(1, 1.0f, 0.05f) == 0.05f, "fresh start begins at the target (not at stale 1.0)");
    CHECK(render_ramp_start(0, 1.0f, 0.05f) == 1.0f, "continuation keeps the previous value (PASSTHROUGH -> PROXIMITY)");
}

int main(void) {
    printf("audio helper tests (084)\n");
    test_cave_identical();
    test_cave_reset();
    test_lpf_first_buffer();
    test_lpf_identical_afterwards();
    test_snap_tristate();
    test_snap_stress();
    test_orphans();
    test_voice_cap();
    test_render_helpers();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
