/*
 * hrtf.c — generic structural HRTF (Brown & Duda 1998), see hrtf.h.
 *
 * Thread contract: no globals except const tables, no locks, no allocation.
 * Every function works only on the caller-owned HrtfState.
 */

#include "core/proximity/hrtf.h"

#include <math.h>
#include <string.h>

#define HRTF_PI              3.14159265358979f
#define HRTF_HALF_PI         1.57079632679490f
#define HRTF_DEG             0.01745329251994f
#define HRTF_HEAD_RADIUS_M   0.0875f   /* average adult head */
#define HRTF_SPEED_OF_SOUND  343.0f    /* m/s */
#define HRTF_ALPHA_MIN       0.1f      /* strongest high-frequency shadow */
#define HRTF_THETA_MIN       2.61799388f /* 150 deg: angle of strongest shadow */
#define HRTF_PINNA_REF_RATE  44100.0f  /* pinna table is given in samples @44.1k */
#define HRTF_SUBBLOCK        32        /* direction update step (frames) */
#define HRTF_PINNA_MASK      (HRTF_PINNA_HISTORY - 1)
#define HRTF_ITD_MASK        (HRTF_ITD_HISTORY - 1)
#define HRTF_GAIN_MIN        0.25f
#define HRTF_GAIN_MAX        4.0f

/* Brown & Duda 1998, Table 1: reflection coefficient, A, B (samples @44.1k), D. */
static const float k_pinnaRho[HRTF_PINNA_ECHOES] = { 0.5f, -1.0f, 0.5f, -0.25f, 0.25f };
static const float k_pinnaA[HRTF_PINNA_ECHOES]   = { 1.0f, 5.0f, 5.0f, 5.0f, 5.0f };
static const float k_pinnaB[HRTF_PINNA_ECHOES]   = { 2.0f, 4.0f, 7.0f, 11.0f, 13.0f };
static const float k_pinnaD[HRTF_PINNA_ECHOES]   = { 1.0f, 0.5f, 0.5f, 0.5f, 0.5f };

typedef struct EarParams {
    float b0, b1, a1;   /* head-shadow filter */
    float itd;          /* extra delay of this ear in samples */
} EarParams;

typedef struct DirParams {
    EarParams left;
    EarParams right;
    float pinna[HRTF_PINNA_ECHOES]; /* echo delays in samples (both ears) */
} DirParams;

static float hrtf_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Wrap an angle difference into (-pi, pi] so a turn through "behind" takes
   the short way. fmodf instead of a subtract loop: a loop never ends for huge
   values, where a - 2*pi == a in float. */
static float hrtf_wrap(float a) {
    if (!isfinite(a)) {
        return 0.0f;
    }
    a = fmodf(a, 2.0f * HRTF_PI);
    if (a > HRTF_PI) {
        a -= 2.0f * HRTF_PI;
    }
    else if (a <= -HRTF_PI) {
        a += 2.0f * HRTF_PI;
    }
    return a;
}

/* Head shadow: H(s) = (alpha*s + beta) / (s + beta), beta = 2c/a, discretized
   with the bilinear transform. Gain is 1 at DC and alpha at high frequencies:
   alpha = 2 (+6 dB) facing the ear, 0.1 (-20 dB) at 150 deg. */
static void hrtf_head_shadow(float theta, float sampleRate, EarParams* e) {
    const float alpha = (1.0f + HRTF_ALPHA_MIN * 0.5f)
        + (1.0f - HRTF_ALPHA_MIN * 0.5f) * cosf(theta / HRTF_THETA_MIN * HRTF_PI);
    const float beta = 2.0f * HRTF_SPEED_OF_SOUND / HRTF_HEAD_RADIUS_M;
    const float k = 2.0f * sampleRate;
    const float den = beta + k;
    e->b0 = (beta + k * alpha) / den;
    e->b1 = (beta - k * alpha) / den;
    e->a1 = (beta - k) / den;
}

/* Woodworth arrival time of one ear (seconds, relative to the head centre).
   theta = angle between source direction and the ear axis. */
static float hrtf_woodworth(float theta) {
    const float ac = HRTF_HEAD_RADIUS_M / HRTF_SPEED_OF_SOUND;
    return theta < HRTF_HALF_PI ? -ac * cosf(theta) : ac * (theta - HRTF_HALF_PI);
}

static void hrtf_dir_params(float azimuth, float elevation, float sampleRate, DirParams* p) {
    const float ce = cosf(elevation);
    const float xr = hrtf_clampf(sinf(azimuth) * ce, -1.0f, 1.0f); /* toward right ear */
    const float yu = sinf(elevation);
    const float zf = cosf(azimuth) * ce;
    const float thetaRight = acosf(xr);
    const float thetaLeft = acosf(-xr);

    hrtf_head_shadow(thetaLeft, sampleRate, &p->left);
    hrtf_head_shadow(thetaRight, sampleRate, &p->right);

    const float tLeft = hrtf_woodworth(thetaLeft);
    const float tRight = hrtf_woodworth(thetaRight);
    const float tMin = tLeft < tRight ? tLeft : tRight;
    p->left.itd = hrtf_clampf((tLeft - tMin) * sampleRate, 0.0f, (float)(HRTF_ITD_HISTORY - 2));
    p->right.itd = hrtf_clampf((tRight - tMin) * sampleRate, 0.0f, (float)(HRTF_ITD_HISTORY - 2));

    /* Pinna delays use interaural-polar coordinates: lateral angle (left/right)
       and polar angle around the ear axis (0 front, pi/2 up, pi behind). */
    const float lateral = asinf(xr);
    float polar = atan2f(yu, zf);
    if (polar < -HRTF_HALF_PI) {
        polar += 2.0f * HRTF_PI; /* range [-pi/2, 3pi/2) as in the paper */
    }
    const float scale = sampleRate / HRTF_PINNA_REF_RATE;
    for (int k = 0; k < HRTF_PINNA_ECHOES; k++) {
        const float t = k_pinnaA[k] * cosf(lateral * 0.5f)
            * sinf(k_pinnaD[k] * (HRTF_HALF_PI - polar)) + k_pinnaB[k];
        p->pinna[k] = hrtf_clampf(t * scale, 0.5f, (float)(HRTF_PINNA_HISTORY - 2));
    }
}

/* Sample that was written `delay` samples before index pos (linear
   interpolation between the two neighbours). */
static float hrtf_hist_read(const float* hist, unsigned int mask, unsigned int pos, float delay) {
    const unsigned int whole = (unsigned int)delay;
    const float frac = delay - (float)whole;
    const float a = hist[(pos - whole) & mask];
    const float b = hist[(pos - whole - 1u) & mask];
    return a + (b - a) * frac;
}

/* One sample for one ear: head shadow -> pinna echoes -> interaural delay.
   Delays slide linearly from `from` to `to` across the sub-block (f = 0..1). */
static float hrtf_ear_step(HrtfEar* e, const EarParams* from, const EarParams* to,
    const float* pinnaFrom, const float* pinnaTo, float f, float x, unsigned int pos) {
    float h = to->b0 * x + to->b1 * e->x1 - to->a1 * e->y1;
    if (h > -1e-15f && h < 1e-15f) {
        h = 0.0f; /* keep the filter out of denormals during silence */
    }
    e->x1 = x;
    e->y1 = h;

    e->pinnaHist[pos & HRTF_PINNA_MASK] = h;
    float p = h;
    for (int k = 0; k < HRTF_PINNA_ECHOES; k++) {
        const float d = pinnaFrom[k] + (pinnaTo[k] - pinnaFrom[k]) * f;
        p += k_pinnaRho[k] * hrtf_hist_read(e->pinnaHist, HRTF_PINNA_MASK, pos, d);
    }

    e->itdHist[pos & HRTF_ITD_MASK] = p;
    const float d = from->itd + (to->itd - from->itd) * f;
    return hrtf_hist_read(e->itdHist, HRTF_ITD_MASK, pos, d);
}

void hrtf_reset(HrtfState* state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->energyGain = 1.0f;
}

int hrtf_listener_angles(float yawDeg, float pitchDeg,
    float dx, float dy, float dz,
    float* azimuthRad, float* elevationRad) {
    if (!azimuthRad || !elevationRad) {
        return 0;
    }
    *azimuthRad = 0.0f;
    *elevationRad = 0.0f;

    /* Plugin coordinates (y = height) back to the Unreal frame (z = height). */
    const float ux = dx;
    const float uy = dz;
    const float uz = dy;
    const float len = sqrtf(ux * ux + uy * uy + uz * uz);
    if (!isfinite(len) || len < 1e-4f || !isfinite(yawDeg) || !isfinite(pitchDeg)) {
        return 0;
    }

    float pitch = fmodf(pitchDeg, 360.0f);
    if (pitch > 180.0f) {
        pitch -= 360.0f;
    }
    else if (pitch < -180.0f) {
        pitch += 360.0f;
    }
    const float cy = cosf(yawDeg * HRTF_DEG);
    const float sy = sinf(yawDeg * HRTF_DEG);
    const float cp = cosf(pitch * HRTF_DEG);
    const float sp = sinf(pitch * HRTF_DEG);

    /* Head axes in Unreal space (X forward, Y right, Z up at yaw = pitch = 0). */
    const float fx = cp * cy, fy = cp * sy, fz = sp;
    const float rx = -sy, ry = cy;
    const float upx = -sp * cy, upy = -sp * sy, upz = cp;

    const float right = (ux * rx + uy * ry) / len;
    const float up = (ux * upx + uy * upy + uz * upz) / len;
    const float front = (ux * fx + uy * fy + uz * fz) / len;

    *azimuthRad = atan2f(right, front);
    *elevationRad = atan2f(up, sqrtf(right * right + front * front));
    return 1;
}

void hrtf_render(HrtfState* state, const float* mono, int frameCount,
    float sampleRate, float azimuthRad, float elevationRad,
    float* outLeft, float* outRight) {
    if (!state || !mono || !outLeft || !outRight || frameCount <= 0 || !(sampleRate > 0.0f)) {
        return;
    }
    if (!isfinite(azimuthRad) || !isfinite(elevationRad)) {
        azimuthRad = state->lastAzimuth;
        elevationRad = state->lastElevation;
    }
    if (!state->initialized) {
        state->lastAzimuth = azimuthRad;
        state->lastElevation = elevationRad;
        state->initialized = 1;
    }

    const float startAz = state->lastAzimuth;
    const float startEl = state->lastElevation;
    const float deltaAz = hrtf_wrap(azimuthRad - startAz);
    const float deltaEl = elevationRad - startEl;

    DirParams cur;
    DirParams next;
    hrtf_dir_params(startAz, startEl, sampleRate, &cur);

    for (int base = 0; base < frameCount; base += HRTF_SUBBLOCK) {
        const int len = (frameCount - base) < HRTF_SUBBLOCK ? (frameCount - base) : HRTF_SUBBLOCK;
        const float t = (float)(base + len) / (float)frameCount;
        hrtf_dir_params(startAz + deltaAz * t, startEl + deltaEl * t, sampleRate, &next);

        for (int i = 0; i < len; i++) {
            const float f = (float)(i + 1) / (float)len;
            const float x = mono[base + i];
            const unsigned int pos = state->pos;
            outLeft[base + i] = hrtf_ear_step(&state->left, &cur.left, &next.left,
                cur.pinna, next.pinna, f, x, pos);
            outRight[base + i] = hrtf_ear_step(&state->right, &cur.right, &next.right,
                cur.pinna, next.pinna, f, x, pos);
            state->pos = pos + 1u;
        }
        cur = next;
    }

    state->lastAzimuth = hrtf_wrap(startAz + deltaAz);
    state->lastElevation = elevationRad;
}

static short hrtf_to_s16(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (short)v;
}

void hrtf_process_interleaved_s16(HrtfState* state, short* samples,
    int frameCount, int channels, int leftIndex, int rightIndex,
    float sampleRate, float azimuthRad, float elevationRad) {
    if (!state || !samples || frameCount <= 0 || channels < 2
        || leftIndex < 0 || rightIndex < 0 || leftIndex >= channels || rightIndex >= channels
        || leftIndex == rightIndex || !(sampleRate > 0.0f)) {
        return;
    }
    if (!isfinite(azimuthRad) || !isfinite(elevationRad)) {
        return;
    }

    float mono[HRTF_MAX_CHUNK_FRAMES];
    float outL[HRTF_MAX_CHUNK_FRAMES];
    float outR[HRTF_MAX_CHUNK_FRAMES];

    const int first = !state->initialized;
    const float startAz = first ? azimuthRad : state->lastAzimuth;
    const float startEl = first ? elevationRad : state->lastElevation;
    const float deltaAz = hrtf_wrap(azimuthRad - startAz);
    const float deltaEl = elevationRad - startEl;

    for (int base = 0; base < frameCount; base += HRTF_MAX_CHUNK_FRAMES) {
        const int len = (frameCount - base) < HRTF_MAX_CHUNK_FRAMES
            ? (frameCount - base) : HRTF_MAX_CHUNK_FRAMES;

        float energyIn = 0.0f;
        for (int i = 0; i < len; i++) {
            const short* frame = samples + (size_t)(base + i) * (size_t)channels;
            const float l = (float)frame[leftIndex];
            const float r = (float)frame[rightIndex];
            energyIn += l * l + r * r;
            mono[i] = 0.5f * (l + r);
        }

        const float t = (float)(base + len) / (float)frameCount;
        hrtf_render(state, mono, len, sampleRate,
            startAz + deltaAz * t, startEl + deltaEl * t, outL, outR);

        float energyOut = 0.0f;
        for (int i = 0; i < len; i++) {
            energyOut += outL[i] * outL[i] + outR[i] * outR[i];
        }
        float target = state->energyGain;
        if (energyOut > 1e-3f) {
            target = hrtf_clampf(sqrtf(energyIn / energyOut), HRTF_GAIN_MIN, HRTF_GAIN_MAX);
        }
        if (first && base == 0) {
            state->energyGain = target; /* no fade-in on the very first block */
        }

        float gain = state->energyGain;
        const float step = (target - gain) / (float)len;
        for (int i = 0; i < len; i++) {
            gain += step;
            short* frame = samples + (size_t)(base + i) * (size_t)channels;
            frame[leftIndex] = hrtf_to_s16(outL[i] * gain);
            frame[rightIndex] = hrtf_to_s16(outR[i] * gain);
        }
        state->energyGain = target;
    }
}
