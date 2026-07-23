/* Unit tests for src/core/proximity/proximity_math.c (pure functions).
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no TS SDK needed).
   Exit code 0 = all checks passed. */

#include "core/proximity/proximity_math.h"

#include <math.h>
#include <stdio.h>

/* proximity_math.c logs through log_write — stub it out for the host build. */
void log_write(const char* fmt, ...) {
    (void)fmt;
}

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static void test_distance(void) {
    printf("[1] prox_distance\n");
    CHECK(NEAR(prox_distance(0, 0, 0, 3, 4, 0), 5.0f, 1e-5f), "3-4-5 triangle = 5");
    CHECK(NEAR(prox_distance(1, 2, 3, 1, 2, 3), 0.0f, 1e-6f), "same point = 0");
    CHECK(NEAR(prox_distance(0, 0, 0, 0, 0, -7), 7.0f, 1e-5f), "negative axis = 7");
}

static void test_volume_curve(void) {
    printf("[2] prox_volume_from_distance\n");
    const float voice = 15.0f;

    CHECK(NEAR(prox_volume_from_distance(0.0f, voice, 1.0f), 1.0f, 0.01f),
        "0m -> ~1.0 (clamped to maxVolume)");
    CHECK(prox_volume_from_distance(voice * 1.12f, voice, 1.0f) == 0.0f,
        "norm >= fadeEnd (1.12x range) -> 0.0");
    CHECK(prox_volume_from_distance(voice * 3.0f, voice, 1.0f) == 0.0f,
        "far beyond range -> 0.0");

    /* Monotonic decreasing over the audible range. */
    int monotonic = 1;
    float prev = prox_volume_from_distance(0.1f, voice, 1.0f);
    for (float d = 0.5f; d <= voice * 1.2f; d += 0.25f) {
        const float v = prox_volume_from_distance(d, voice, 1.0f);
        if (v > prev + 1e-5f) {
            monotonic = 0;
        }
        prev = v;
    }
    CHECK(monotonic, "curve monotonically decreasing");

    /* maxVolume caps and scales the output. */
    CHECK(prox_volume_from_distance(0.0f, voice, 1.3f) <= 1.3f + 1e-6f,
        "output never exceeds maxVolume");
    CHECK(prox_volume_from_distance(5.0f, voice, 0.5f)
        < prox_volume_from_distance(5.0f, voice, 1.0f),
        "lower maxVolume -> lower gain");
}

static void test_stereo_pan(void) {
    printf("[3] prox_stereo_pan\n");
    float l, r;

    /* Listener faces -X (CEPOS yaw 0 convention, see prox_math_self_test). */
    prox_stereo_pan(-1.0f, 0.0f, 0.0f, 1.0f, &l, &r);
    CHECK(r > 0.95f && l < 0.05f, "remote at +Z of -X gaze -> full right");

    prox_stereo_pan(-1.0f, 0.0f, 0.0f, -1.0f, &l, &r);
    CHECK(l > 0.95f && r < 0.05f, "remote at -Z of -X gaze -> full left");

    prox_stereo_pan(-1.0f, 0.0f, -1.0f, 0.0f, &l, &r);
    CHECK(NEAR(l, 0.7071f, 0.001f) && NEAR(r, 0.7071f, 0.001f),
        "remote straight ahead -> centered 0.707/0.707");

    prox_stereo_pan(-1.0f, 0.0f, 0.0f, 0.0f, &l, &r);
    CHECK(NEAR(l, 0.7071f, 0.001f) && NEAR(r, 0.7071f, 0.001f),
        "same spot -> centered 0.707/0.707");

    /* Equal-power law: L^2 + R^2 == 1 for any direction. */
    int equalPower = 1;
    for (int deg = 0; deg < 360; deg += 15) {
        const float rad = (float)deg * 3.14159265f / 180.0f;
        prox_stereo_pan(-1.0f, 0.0f, cosf(rad), sinf(rad), &l, &r);
        if (!NEAR(l * l + r * r, 1.0f, 0.001f)) {
            equalPower = 0;
        }
    }
    CHECK(equalPower, "equal-power property (L^2+R^2=1) for all angles");
}

static void test_lowpass_cutoff(void) {
    printf("[4] prox_lowpass_cutoff_hz\n");
    int inRange = 1;
    int monotonic = 1;
    float prev = prox_lowpass_cutoff_hz(0.0f);
    for (float d = 1.0f; d <= 200.0f; d += 1.0f) {
        const float c = prox_lowpass_cutoff_hz(d);
        if (c < 900.0f - 1e-3f || c > 8000.0f + 1e-3f) {
            inRange = 0;
        }
        if (c > prev + 1e-3f) {
            monotonic = 0;
        }
        prev = c;
    }
    CHECK(inRange, "cutoff stays within 900..8000 Hz");
    CHECK(monotonic, "cutoff monotonically decreasing with distance");
    CHECK(prox_lowpass_cutoff_hz(1.0f) > 7000.0f, "close up ~8 kHz");
}

static void test_direct_reverb_ratio(void) {
    printf("[5] prox_direct_reverb_ratio\n");
    CHECK(NEAR(prox_direct_reverb_ratio(1.0f, 1.0f), 0.5f, 1e-4f),
        "distance == reference -> 0.5");
    CHECK(prox_direct_reverb_ratio(0.0f, 1.0f) >= 0.95f, "very close -> ~1.0");
    CHECK(NEAR(prox_direct_reverb_ratio(1000.0f, 1.0f), 0.05f, 1e-4f),
        "very far -> floor 0.05");
    CHECK(prox_direct_reverb_ratio(10.0f, 0.5f) < prox_direct_reverb_ratio(2.0f, 0.5f),
        "DRR falls with distance");

    int inRange = 1;
    for (float d = 0.0f; d <= 100.0f; d += 0.5f) {
        const float drr = prox_direct_reverb_ratio(d, 1.0f);
        if (drr < 0.05f - 1e-6f || drr > 1.0f + 1e-6f) {
            inRange = 0;
        }
    }
    CHECK(inRange, "DRR stays within 0.05..1.0");
}

static void test_rear_psychoacoustics(void) {
    printf("[6] prox_rear_psychoacoustics\n");
    ProxRearPsycho p;

    prox_rear_psychoacoustics(1.0f, &p);
    CHECK(p.directionVolume == 1.0f && p.cutoffMul == 1.0f && p.drrMul == 1.0f,
        "fully in front -> neutral");

    prox_rear_psychoacoustics(0.0f, &p);
    CHECK(p.directionVolume == 1.0f && p.cutoffMul == 1.0f && p.drrMul == 1.0f,
        "sideways -> neutral");

    prox_rear_psychoacoustics(-1.0f, &p);
    CHECK(NEAR(p.directionVolume, 0.88f, 1e-4f), "fully behind -> volume 0.88");
    CHECK(p.cutoffMul == 0.75f && p.drrMul == 0.85f, "behind -> cutoff/drr muls");

    prox_rear_psychoacoustics(-0.5f, &p);
    CHECK(p.directionVolume < 1.0f && p.directionVolume > 0.88f,
        "half behind -> between 0.88 and 1.0");
}

static void test_binaural_gains(void) {
    printf("[7] prox_binaural_stereo_gains\n");
    float l1, r1, l2, r2;

    /* Listener faces -X; remote to the side at +Z / -Z (frontBack = 0,
       so only the left/right asymmetry acts). */
    prox_binaural_stereo_gains(-1, 0, 0, 0, 0, 1, &l1, &r1);
    prox_binaural_stereo_gains(-1, 0, 0, 0, 0, -1, &l2, &r2);
    CHECK(l1 != r1, "side source -> asymmetric gains");
    CHECK(NEAR(l1, r2, 1e-5f) && NEAR(r1, l2, 1e-5f),
        "mirrored source swaps L/R (symmetry)");

    /* Range sanity over many directions. Pan floor is 0.15; the rear
       attenuation (x0.55..x1.0) applies afterwards, so the true lower
       bound is 0.15 * 0.55 = 0.0825. */
    int inRange = 1;
    for (int deg = 0; deg < 360; deg += 10) {
        const float rad = (float)deg * 3.14159265f / 180.0f;
        float l, r;
        prox_binaural_stereo_gains(-1, 0, 0, cosf(rad), 0.0f, sinf(rad), &l, &r);
        if (l < 0.0825f - 1e-5f || l > 1.0f + 1e-5f
            || r < 0.0825f - 1e-5f || r > 1.0f + 1e-5f) {
            inRange = 0;
        }
    }
    CHECK(inRange, "gains stay within 0.0825..1.0 for all directions");

    /* Degenerate input: same spot -> neutral 1.0/1.0. */
    prox_binaural_stereo_gains(-1, 0, 0, 0, 0, 0, &l1, &r1);
    CHECK(l1 == 1.0f && r1 == 1.0f, "zero offset -> neutral 1.0/1.0");
}

int main(void) {
    test_distance();
    test_volume_curve();
    test_stereo_pan();
    test_lowpass_cutoff();
    test_direct_reverb_ratio();
    test_rear_psychoacoustics();
    test_binaural_gains();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
