#ifndef CORE_PROXIMITY_AUDIO_FX_H
#define CORE_PROXIMITY_AUDIO_FX_H

/*
 * audio_fx — the sample-level effects of the proximity PCM path: one-pole
 * lowpass and the Schroeder "cave" reverb (comb + allpass). Header-only, pure
 * C, no Win32 / TS coupling, so the exact code the plugin runs is unit-tested
 * on the host (tests/audio_fx_test.c).
 *
 * Thread contract: pure functions on caller-owned state. Every state struct
 * belongs to exactly ONE thread (the TS audio thread in the plugin). No
 * globals, no locks, no allocation — safe on the real-time audio path.
 *
 * Review fix 084 (#4, #5): the cave reverb now keeps SEPARATE read/write
 * positions for the left and right channel, and the lowpass honestly is a
 * one-pole filter. Both were rewritten so the SOUND STAYS BIT-IDENTICAL to the
 * in-game-tested one (see the notes at CAVE_COMB_LEN and audio_lpf_apply).
 */

#include <string.h>

/* ---- lowpass ------------------------------------------------------------------ */

typedef struct AudioLpf {
    float prevL, prevR;
    float alpha;
    float lastCutoff;
    int initialized;
} AudioLpf;

static inline short audio_fx_clamp_sample(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (short)v;
}

/* One-pole lowpass over an interleaved s16 buffer (channels >= 2: first two
   channels filtered, the rest untouched; mono: channel 0).

   Fix #5a: the filter memory is seeded from the FIRST sample of the first
   buffer and `initialized` is set at once. The old code set `initialized` only
   after the loop, so `prev` followed the input for the whole first buffer
   (a no-op filter) and the first buffer after every (re)init was unfiltered.

   Fix #5b: the old code had a "second pass" `x = prev; prev += alpha*0.7*(x - prev)`
   which adds exactly 0 (x == prev), i.e. the filter always was one-pole. The
   dead code is gone, the output is identical. A real second pole would change
   the occlusion/distance sound (steeper roll-off) — a design decision for
   later, not part of this fix. */
static inline void audio_lpf_apply(short* samples, int sampleCount, int channels,
    float cutoffHz, float sampleRate, AudioLpf* fx) {
    if (cutoffHz != fx->lastCutoff) {
        const float dt = 1.0f / sampleRate;
        const float rc = 1.0f / (cutoffHz * 6.28318530718f);
        fx->alpha = dt / (rc + dt);
        fx->lastCutoff = cutoffHz;
    }
    const float alpha = fx->alpha;

    if (channels >= 2) {
        if (!fx->initialized) {
            fx->prevL = (float)samples[0];
            fx->prevR = (float)samples[1];
            fx->initialized = 1;
        }
        for (int s = 0; s < sampleCount; s++) {
            const int li = s * channels;
            const int ri = li + 1;
            fx->prevL += alpha * ((float)samples[li] - fx->prevL);
            fx->prevR += alpha * ((float)samples[ri] - fx->prevR);
            samples[li] = audio_fx_clamp_sample(fx->prevL);
            samples[ri] = audio_fx_clamp_sample(fx->prevR);
        }
    }
    else {
        if (!fx->initialized) {
            fx->prevL = (float)samples[0];
            fx->initialized = 1;
        }
        for (int s = 0; s < sampleCount; s++) {
            fx->prevL += alpha * ((float)samples[s] - fx->prevL);
            samples[s] = audio_fx_clamp_sample(fx->prevL);
        }
    }
}

/* ---- cave reverb (Schroeder: predelay -> 4 combs -> 2 allpasses) -------------- */

#define CAVE_COMBS        4
#define CAVE_ALLPASS      2
#define CAVE_PREDELAY     1680  /* ~35 ms */
#define CAVE_AP_MAX       607
#define CAVE_WET_GAIN     0.26f
#define CAVE_DRY_GAIN     0.92f
#define CAVE_COMB_MIX     0.18f
#define CAVE_COMB_DAMP    0.60f
#define CAVE_AP_GAIN      0.55f
#define CAVE_STEREO_WIDTH 0.68f

/* Comb delays in samples @ 48 kHz — the EFFECTIVE lengths of the in-game-tested
   sound. History: the code was designed for {2546, 3507, 4257, 5466}, but L and R
   shared ONE position index per comb, so it advanced by 2 per sample. An odd
   length (3507, 4257) then still means a delay of `len` samples, an EVEN length
   (2546, 5466) only `len / 2`: the sound actually heard in game always was
   {1273, 3507, 4257, 2733}. These are now written down as they are; with
   independent L/R positions the output is bit-identical (proved by
   tests/audio_fx_test.c against a copy of the old algorithm).
   The designed lengths 2546 / 5466 would be a (longer, denser) SOUND CHANGE —
   a user decision for later; then set CAVE_COMB_MAX to 5466 as well. */
#define CAVE_LEN_0        1273
#define CAVE_LEN_1        3507
#define CAVE_LEN_2        4257
#define CAVE_LEN_3        2733
#define CAVE_COMB_MAX     4257  /* >= every CAVE_LEN_n (buffer size) */
static const float CAVE_COMB_FB[CAVE_COMBS] = { 0.79f, 0.80f, 0.81f, 0.82f };
static const int CAVE_COMB_LEN[CAVE_COMBS] = { CAVE_LEN_0, CAVE_LEN_1, CAVE_LEN_2, CAVE_LEN_3 };
static const int CAVE_AP_LEN[CAVE_ALLPASS] = { 607, 481 };

/* Compile-time guard: a delay longer than its buffer would overflow it. */
typedef char cave_comb_len_guard_[
    (CAVE_LEN_0 <= CAVE_COMB_MAX && CAVE_LEN_1 <= CAVE_COMB_MAX
     && CAVE_LEN_2 <= CAVE_COMB_MAX && CAVE_LEN_3 <= CAVE_COMB_MAX) ? 1 : -1];

/* Buffers and positions of one reverb instance. Zero state = fresh reverb.
   All fields are written ONLY by the audio thread. */
typedef struct CaveDsp {
    float combL[CAVE_COMBS][CAVE_COMB_MAX];
    float combR[CAVE_COMBS][CAVE_COMB_MAX];
    int combPosL[CAVE_COMBS];
    int combPosR[CAVE_COMBS];
    float combLpL[CAVE_COMBS];
    float combLpR[CAVE_COMBS];
    float apL[CAVE_ALLPASS][CAVE_AP_MAX];
    float apR[CAVE_ALLPASS][CAVE_AP_MAX];
    int apPosL[CAVE_ALLPASS];
    int apPosR[CAVE_ALLPASS];
    float predelayL[CAVE_PREDELAY];
    float predelayR[CAVE_PREDELAY];
    int predelayPos;
} CaveDsp;

/* Forget everything (fresh slot owner / new utterance after a pause). */
static inline void cave_dsp_reset(CaveDsp* rev) {
    memset(rev, 0, sizeof(*rev));
}

/* Freeverb-style comb with damped feedback. */
static inline float cave_comb(float* buf, int len, int* pos, float input,
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
static inline float cave_allpass(float* buf, int len, int* pos, float input) {
    const float delayed = buf[*pos];
    const float output = -input + delayed;
    buf[*pos] = input + delayed * CAVE_AP_GAIN;
    if (++(*pos) >= len) {
        *pos = 0;
    }
    return output;
}

static inline void cave_dsp_process_sample(CaveDsp* rev, float inL, float inR,
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
        outL += cave_comb(rev->combL[c], CAVE_COMB_LEN[c], &rev->combPosL[c],
            revInL, CAVE_COMB_FB[c], &rev->combLpL[c]);
        outR += cave_comb(rev->combR[c], CAVE_COMB_LEN[c], &rev->combPosR[c],
            revInR, CAVE_COMB_FB[c], &rev->combLpR[c]);
    }
    outL *= CAVE_COMB_MIX;
    outR *= CAVE_COMB_MIX;

    for (int a = 0; a < CAVE_ALLPASS; a++) {
        outL = cave_allpass(rev->apL[a], CAVE_AP_LEN[a], &rev->apPosL[a], outL);
        outR = cave_allpass(rev->apR[a], CAVE_AP_LEN[a], &rev->apPosR[a], outR);
    }

    const float mono = (outL + outR) * 0.5f;
    *wetL = mono + (outL - mono) * CAVE_STEREO_WIDTH;
    *wetR = mono + (outR - mono) * CAVE_STEREO_WIDTH;
}

#endif /* CORE_PROXIMITY_AUDIO_FX_H */
