/* Unit tests for src/core/proximity/hrtf.c (pure functions + caller state).
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no TS SDK needed).
   Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\hrtf_test.c src\core\proximity\hrtf.c /Fe:tests\out\hrtf_test.exe
   Exit code 0 = all checks passed. */

#include "core/proximity/hrtf.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))
#define DEG(r) ((r) * 57.2957795f)
#define FS 48000.0f
#define N 4800

static unsigned int g_seed = 12345u;
static float noise(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return ((float)((g_seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
}

static float energy(const float* v, int n) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        e += (double)v[i] * v[i];
    }
    return (float)e;
}

static int peak_index(const float* v, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (fabsf(v[i]) > fabsf(v[best])) {
            best = i;
        }
    }
    return best;
}

static void test_angles(void) {
    printf("[1] hrtf_listener_angles (UE yaw/pitch, plugin coords y=height)\n");
    float az, el;
    /* Facing UE +X. Plugin z = UE Y = to the right. */
    CHECK(hrtf_listener_angles(0, 0, 0, 0, 5, &az, &el) && NEAR(DEG(az), 90.0f, 0.01f), "UE +Y is right (+90)");
    hrtf_listener_angles(0, 0, 0, 0, -5, &az, &el);
    CHECK(NEAR(DEG(az), -90.0f, 0.01f), "UE -Y is left (-90)");
    hrtf_listener_angles(0, 0, 5, 0, 0, &az, &el);
    CHECK(NEAR(DEG(az), 0.0f, 0.01f) && NEAR(DEG(el), 0.0f, 0.01f), "UE +X is front (0,0)");
    hrtf_listener_angles(0, 0, -5, 0, 0, &az, &el);
    CHECK(NEAR(fabsf(DEG(az)), 180.0f, 0.01f), "UE -X is behind (+-180)");
    hrtf_listener_angles(0, 0, 0, 5, 0, &az, &el);
    CHECK(NEAR(DEG(el), 90.0f, 0.01f), "plugin +y (height) is above (+90 elevation)");
    hrtf_listener_angles(90, 0, 0, 0, 5, &az, &el);
    CHECK(NEAR(DEG(az), 0.0f, 0.01f), "yaw 90 faces UE +Y -> source there is front");
    hrtf_listener_angles(90, 0, 5, 0, 0, &az, &el);
    CHECK(NEAR(DEG(az), -90.0f, 0.01f), "yaw 90: UE +X is on the left");
    hrtf_listener_angles(0, 30, 5, 0, 0, &az, &el);
    CHECK(NEAR(DEG(el), -30.0f, 0.01f), "looking up 30: level source appears 30 below");
    hrtf_listener_angles(0, 330, 5, 0, 0, &az, &el);
    CHECK(NEAR(DEG(el), 30.0f, 0.01f), "pitch 330 (= -30, looking down): source appears 30 above");
    CHECK(!hrtf_listener_angles(0, 0, 0, 0, 0, &az, &el) && az == 0.0f && el == 0.0f, "source on listener -> 0, outputs 0");
}

static void render_dir(float azDeg, float elDeg, const float* in, float* l, float* r) {
    HrtfState s;
    hrtf_reset(&s);
    hrtf_render(&s, in, N, FS, azDeg / 57.2957795f, elDeg / 57.2957795f, l, r);
}

static void test_lateral(void) {
    printf("[2] left/right cues (level + time)\n");
    static float in[N], l[N], r[N], l2[N], r2[N];
    for (int i = 0; i < N; i++) {
        in[i] = noise();
    }
    render_dir(90.0f, 0.0f, in, l, r);
    const float dB = 10.0f * log10f(energy(r, N) / energy(l, N));
    printf("       source right: right ear +%.1f dB\n", dB);
    CHECK(dB > 3.0f, "source right -> right ear louder by > 3 dB");

    render_dir(-90.0f, 0.0f, in, l2, r2);
    CHECK(NEAR(energy(l2, N), energy(r, N), energy(r, N) * 1e-3f)
        && NEAR(energy(r2, N), energy(l, N), energy(l, N) * 1e-3f), "left is the mirror of right");

    render_dir(0.0f, 0.0f, in, l, r);
    CHECK(NEAR(energy(l, N), energy(r, N), energy(r, N) * 1e-3f), "front -> both ears equal");

    /* Impulse: arrival order and interaural delay (~31.5 samples @48k at 90 deg). */
    static float imp[N];
    memset(imp, 0, sizeof(imp));
    imp[10] = 1.0f;
    render_dir(90.0f, 0.0f, imp, l, r);
    const int pl = peak_index(l, 200);
    const int pr = peak_index(r, 200);
    printf("       impulse peaks: right %d, left %d (diff %d)\n", pr, pl, pl - pr);
    CHECK(pr < pl, "source right -> right ear hears it first");
    CHECK(pl - pr >= 25 && pl - pr <= 36, "interaural delay ~31 samples at 90 deg");
}

static void test_front_back_and_safety(void) {
    printf("[3] front/back differ, no NaN, reset\n");
    static float in[N], lf[N], rf[N], lb[N], rb[N];
    for (int i = 0; i < N; i++) {
        in[i] = noise();
    }
    render_dir(0.0f, 0.0f, in, lf, rf);
    render_dir(180.0f, 0.0f, in, lb, rb);
    double diff = 0.0;
    for (int i = 0; i < N; i++) {
        diff += (double)(lf[i] - lb[i]) * (lf[i] - lb[i]);
    }
    const float rel = (float)(diff / energy(lf, N));
    printf("       front vs back relative difference %.3f\n", rel);
    CHECK(rel > 0.05f, "front and back sound different (pinna cues)");

    int finite = 1;
    HrtfState s;
    hrtf_reset(&s);
    static float l[N], r[N];
    const float dirs[][2] = { { 3.14159f, 1.5708f }, { -3.14159f, -1.5708f }, { 0.0f, 1.5708f },
                              { 1.0e9f, 0.0f }, { NAN, 0.0f }, { 2.0f, -0.7f } };
    for (int d = 0; d < 6; d++) {
        hrtf_render(&s, in, N, FS, dirs[d][0], dirs[d][1], l, r);
        for (int i = 0; i < N; i++) {
            if (!isfinite(l[i]) || !isfinite(r[i])) {
                finite = 0;
            }
        }
    }
    CHECK(finite, "extreme / NaN directions never produce NaN or Inf");

    hrtf_reset(&s);
    static float zero[N];
    memset(zero, 0, sizeof(zero));
    hrtf_render(&s, zero, N, FS, 1.0f, 0.2f, l, r);
    CHECK(energy(l, N) == 0.0f && energy(r, N) == 0.0f, "after reset: silence in -> silence out");
}

static void test_interleaved(void) {
    printf("[4] hrtf_process_interleaved_s16 (plugin helper)\n");
    static short buf[N * 4];
    static short ref[N * 4];
    for (int i = 0; i < N; i++) {
        const short v = (short)(noise() * 8000.0f);
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }
    memcpy(ref, buf, sizeof(short) * N * 2);
    HrtfState s;
    hrtf_reset(&s);
    hrtf_process_interleaved_s16(&s, buf, N, 2, 0, 1, FS, 1.0f, 0.0f);
    double ein = 0.0, eout = 0.0, el = 0.0, er = 0.0;
    for (int i = 512; i < N; i++) { /* skip the first chunks (gain settles) */
        ein += (double)ref[i * 2] * ref[i * 2] + (double)ref[i * 2 + 1] * ref[i * 2 + 1];
        eout += (double)buf[i * 2] * buf[i * 2] + (double)buf[i * 2 + 1] * buf[i * 2 + 1];
        el += (double)buf[i * 2] * buf[i * 2];
        er += (double)buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    printf("       loudness out/in %.3f, right/left %.1f dB\n", eout / ein, 10.0 * log10(er / el));
    CHECK(eout / ein > 0.9 && eout / ein < 1.1, "block loudness kept within 10%");
    CHECK(er > el, "azimuth +57 deg -> right channel louder");

    /* 4 channels: only the chosen pair changes. */
    for (int i = 0; i < N * 4; i++) {
        buf[i] = (short)(noise() * 8000.0f);
    }
    memcpy(ref, buf, sizeof(short) * N * 4);
    hrtf_reset(&s);
    hrtf_process_interleaved_s16(&s, buf, N, 4, 0, 1, FS, -1.0f, 0.0f);
    int othersSame = 1;
    for (int i = 0; i < N; i++) {
        if (buf[i * 4 + 2] != ref[i * 4 + 2] || buf[i * 4 + 3] != ref[i * 4 + 3]) {
            othersSame = 0;
        }
    }
    CHECK(othersSame, "channels outside the pair are untouched");

    memcpy(buf, ref, sizeof(short) * N * 4);
    hrtf_reset(&s);
    hrtf_process_interleaved_s16(&s, buf, N, 1, 0, 1, FS, 1.0f, 0.0f);
    hrtf_process_interleaved_s16(&s, buf, N, 2, 0, 0, FS, 1.0f, 0.0f);
    hrtf_process_interleaved_s16(&s, buf, N, 2, 0, 1, FS, NAN, 0.0f);
    CHECK(memcmp(buf, ref, sizeof(short) * N * 4) == 0, "mono / same pair / NaN direction -> buffer unchanged");
}

int main(void) {
    printf("hrtf_test\n");
    test_angles();
    test_lateral();
    test_front_back_and_safety();
    test_interleaved();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
