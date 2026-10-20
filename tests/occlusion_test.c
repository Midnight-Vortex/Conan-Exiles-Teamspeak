/* Unit tests for src/core/proximity/occlusion.c (PROTOTYPE, branch
   test/occlusion-prototype). Build/run: bash tests/run_tests.sh.

   Semantics under test (occlusion.h):
   - "OCC=<cid>:<pct>;..." parsing, clamping 0..100, bounds on client id,
     -1 when the key is missing (= no opt-in),
   - values go stale after OCCLUSION_STALE_MS,
   - occlusion_apply lowers gain + cutoff monotonically and never raises cutoff,
   - probe list: Unreal axes in cm, nearest first, range + count limits. */

#include "core/proximity/occlusion.h"
#include "core/proximity/player_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* player_table.c / proximity_math.c log through the plugin log module — stub
   both for the host build (the suite did not link without them). */
void log_debug(const char* fmt, ...) {
    (void)fmt;
}
void log_write(const char* fmt, ...) {
    (void)fmt;
}

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-4f;
}

static void test_parse(void) {
    occlusion_clear();
    const unsigned long long now = 100000ULL;
    CHECK(occlusion_store_from_text("SEQ=1 X=1 Y=2 Z=3 YAW=0", now) == -1, "no OCC key -> -1");
    CHECK(occlusion_store_from_text("SEQ=1 X=1 Y=2 Z=3 YAW=0 OCC=", now) == 0, "empty OCC -> opt-in, 0 stored");
    CHECK(occlusion_store_from_text("SEQ=1 X=1 OCC=12:80;15:0;20:250;0:50;9999:40;7:-5", now) == 4,
        "4 valid ids stored (0 and 9999 rejected)");
    CHECK(feq(occlusion_get(12, now), 0.80f), "cid 12 = 0.80");
    CHECK(feq(occlusion_get(15, now), 0.0f), "cid 15 = 0");
    CHECK(feq(occlusion_get(20, now), 1.0f), "cid 20 clamped to 1.0");
    CHECK(feq(occlusion_get(7, now), 0.0f), "cid 7 negative clamped to 0");
    CHECK(feq(occlusion_get(99, now), 0.0f), "unknown cid = 0");
    CHECK(occlusion_store_from_text("OCC=30:55,5;31:10", now) == 2, "decimal tail tolerated");
    CHECK(feq(occlusion_get(30, now), 0.55f) && feq(occlusion_get(31, now), 0.10f), "decimal tail values");
    CHECK(occlusion_store_from_text("OCC=abc", now) == 0, "garbage after OCC -> 0");
}

static void test_stale(void) {
    occlusion_clear();
    occlusion_store_from_text("OCC=40:60", 5000ULL);
    CHECK(feq(occlusion_get(40, 5000ULL + OCCLUSION_STALE_MS), 0.60f), "fresh at stale limit");
    CHECK(feq(occlusion_get(40, 5000ULL + OCCLUSION_STALE_MS + 1), 0.0f), "stale after limit");
}

static void test_apply(void) {
    float g = 0.8f, c = 19000.0f;
    occlusion_apply(0.0f, 1.0f, &g, &c);
    CHECK(feq(g, 0.8f) && feq(c, 19000.0f), "occ 0 is a no-op");

    g = 1.0f; c = 19000.0f;
    occlusion_apply(1.0f, 1.0f, &g, &c);
    CHECK(feq(g, OCCLUSION_FULL_GAIN), "occ 1 -> full gain multiplier");
    CHECK(fabsf(c - OCCLUSION_FULL_CUTOFF) < 1.0f, "occ 1 -> full cutoff");

    float g5 = 1.0f, c5 = 19000.0f;
    occlusion_apply(0.5f, 1.0f, &g5, &c5);
    CHECK(g5 < 1.0f && g5 > OCCLUSION_FULL_GAIN, "occ 0.5 gain between");
    CHECK(c5 < 19000.0f && c5 > OCCLUSION_FULL_CUTOFF, "occ 0.5 cutoff between");

    g = 1.0f; c = 500.0f;
    occlusion_apply(0.3f, 1.0f, &g, &c);
    CHECK(feq(c, 500.0f), "never raises an already lower cutoff");
}

/* 069: OcclusionIntensity scales the dB loss; 100 % keeps the 8.0.8 curve. */
static void test_strength(void) {
    float g = 1.0f, c = 19000.0f;
    occlusion_apply(1.0f, 0.0f, &g, &c);
    CHECK(feq(g, 1.0f) && feq(c, 19000.0f), "strength 0 is a no-op");

    float ga = 1.0f, ca = 19000.0f, gb = 1.0f, cb = 19000.0f;
    occlusion_apply(0.7f, 1.0f, &ga, &ca);
    gb = 1.0f - 0.7f * (1.0f - OCCLUSION_FULL_GAIN);
    cb = 19000.0f * powf(OCCLUSION_FULL_CUTOFF / 19000.0f, 0.7f);
    CHECK(ga == gb && ca == cb, "strength 1 is bit-identical to the 8.0.8 formula");

    g = 1.0f; c = 19000.0f;
    occlusion_apply(1.0f, 2.0f, &g, &c);
    CHECK(feq(g, OCCLUSION_FULL_GAIN * OCCLUSION_FULL_GAIN), "strength 2 -> twice the dB loss (-24 dB)");
    CHECK(feq(c, OCCLUSION_MIN_CUTOFF), "strength 2 -> cutoff clamped at the minimum");

    float gh = 1.0f, ch = 19000.0f;
    occlusion_apply(1.0f, 1.25f, &gh, &ch);
    CHECK(gh < OCCLUSION_FULL_GAIN && ch < OCCLUSION_FULL_CUTOFF && ch > OCCLUSION_MIN_CUTOFF,
        "strength 1.25 -> stronger than 100 %, cutoff above the minimum");

    occlusion_set_intensity(150.0f);
    CHECK(feq(occlusion_get_strength(), 1.5f), "intensity 150 -> strength 1.5");
    occlusion_set_intensity(999.0f);
    CHECK(feq(occlusion_get_strength(), OCCLUSION_INTENSITY_MAX / 100.0f), "intensity clamped to max");
    occlusion_set_intensity(-5.0f);
    CHECK(feq(occlusion_get_strength(), 0.0f), "negative intensity -> 0");
    occlusion_set_intensity((float)OCCLUSION_INTENSITY_DEFAULT);
    CHECK(feq(occlusion_get_strength(), 1.0f), "default intensity -> strength 1");
}

static void test_probes(void) {
    player_table_clear();
    /* plugin coords: meters, Y-up */
    player_table_put(11, "far", 100.0f, 0.0f, 0.0f, 30.0f, NULL);   /* 100 m: out of range */
    player_table_put(12, "near", 3.0f, 1.5f, 4.0f, 30.0f, NULL);    /* 5 m */
    player_table_put(13, "mid", 10.0f, 0.0f, 0.0f, 30.0f, NULL);    /* 10 m */
    char buf[640];
    const int n = occlusion_format_probes(buf, (int)sizeof(buf), 0.0f, 0.0f, 0.0f);
    CHECK(n > 0, "probe json written");
    CHECK(strcmp(buf, "{\"ok\":true,\"probe\":\"12,300,400,150;13,1000,0,0\"}") == 0,
        "nearest first, UE axes (X, Y=plugin Z, Z=plugin Y) in cm, far one dropped");
    printf("       %s\n", buf);

    player_table_clear();
    for (int i = 0; i < 40; i++) {
        player_table_put((unsigned short)(100 + i), "p", (float)(40 - i), 0.0f, 0.0f, 30.0f, NULL);
    }
    occlusion_format_probes(buf, (int)sizeof(buf), 0.0f, 0.0f, 0.0f);
    int count = 1;
    for (const char* p = buf; *p; p++) {
        if (*p == ';') {
            count++;
        }
    }
    CHECK(count == OCCLUSION_MAX_PROBES, "probe count capped");
    CHECK(strstr(buf, "\"139,100,0,0") != NULL, "closest (1 m) is first");

    CHECK(occlusion_format_probes(buf, 16, 0.0f, 0.0f, 0.0f) == 0, "too small buffer -> 0");
}

static void test_switch(void) {
    occlusion_clear();
    CHECK(!occlusion_is_enabled(), "switch default off (064)");
    occlusion_set_enabled(1);
    CHECK(occlusion_is_enabled(), "switch on");
    occlusion_store_from_text("OCC=12:80", 1000ull);
    CHECK(occlusion_get(12, 1000ull) > 0.79f, "value stored while on");
    occlusion_set_enabled(0);
    CHECK(!occlusion_is_enabled() && occlusion_get(12, 1000ull) == 0.0f, "switch off forgets values");
    /* review 2026-10-20: a store after switch-off is dropped under the writer lock */
    CHECK(occlusion_store_from_text("OCC=12:80;13:50:T:20:0,0,0", 1000ull) == 0 && occlusion_get(12, 1000ull) == 0.0f,
        "store while off is rejected");
    CHECK(occlusion_store_from_text("no occ key", 1000ull) == -1, "missing OCC key still -1 while off");
}

/* 072: v2 entries, v1 compatibility, occcfg, parameters */
static void test_v2(void) {
    occlusion_clear();
    OcclusionInfo info;
    const int n = occlusion_store_from_text(
        "X=1 OCC=21:0:N:40:0,0,0;22:55:T:100:0,0,0;23:30:D:80:1200,-340,250;24:70", 1000ULL);
    CHECK(n == 4, "v2: four entries stored (mixed with a v1 entry)");
    CHECK(occlusion_get_info(21, 1000ULL, &info) && info.kind == OCCLUSION_KIND_NONE && feq(info.amount, 0.0f)
        && feq(info.room, 0.40f), "v2 N: no occlusion, room kept for the reverb");
    CHECK(occlusion_get_info(22, 1000ULL, &info) && info.kind == OCCLUSION_KIND_TRANSMIT && feq(info.amount, 0.55f)
        && feq(info.room, 1.0f) && !info.relayValid, "v2 T: amount + room, no relay");
    CHECK(occlusion_get_info(23, 1000ULL, &info) && info.kind == OCCLUSION_KIND_DIFFRACT && feq(info.amount, 0.30f)
        && info.relayValid, "v2 D: relay valid");
    /* Unreal (1200, -340, 250) cm -> plugin x 12, y(up) 2.5, z -3.4 m */
    CHECK(feq(info.relayX, 12.0f) && feq(info.relayY, 2.5f) && feq(info.relayZ, -3.4f), "v2 D: relay converted to plugin axes");
    CHECK(occlusion_get_info(24, 1000ULL, &info) && info.kind == OCCLUSION_KIND_TRANSMIT && feq(info.amount, 0.70f)
        && feq(info.room, 0.0f), "v1 entry after v2 entries -> kind T, room 0");
    CHECK(feq(occlusion_get(23, 1000ULL), 0.30f), "occlusion_get still returns the amount");

    CHECK(occlusion_store_from_text("OCC=31:40:Q:20:junk;32:10:T", 2000ULL) == 2, "unknown kind/tail skipped to ';'");
    CHECK(occlusion_get_info(32, 2000ULL, &info) && info.kind == OCCLUSION_KIND_TRANSMIT && feq(info.amount, 0.10f),
        "entry after a malformed tail still parsed");
    CHECK(occlusion_store_from_text("OCC=33:20:D:150:1,2", 2000ULL) == 1
        && occlusion_get_info(33, 2000ULL, &info) && !info.relayValid && feq(info.room, 1.0f),
        "incomplete relay -> not valid, room clamped to 100");
    CHECK(!occlusion_get_info(22, 1000ULL + OCCLUSION_STALE_MS + 1, &info) && info.kind == OCCLUSION_KIND_NONE,
        "stale -> zeroed info");

    char buf[256] = "{\"ok\":true,\"probe\":\"5,1,2,3\"}";
    occlusion_set_mod_params(NULL);
    CHECK(occlusion_append_cfg_json(buf, (int)sizeof(buf))
        && strcmp(buf, "{\"ok\":true,\"probe\":\"5,1,2,3\",\"occcfg\":\"2,80,150,400,150,300,30,60,60,70,80,60,2\"}") == 0,
        "occcfg appended with defaults");
    char small[40] = "{\"ok\":true,\"probe\":\"5,1,2,3\"}";
    CHECK(!occlusion_append_cfg_json(small, (int)sizeof(small)) && strcmp(small, "{\"ok\":true,\"probe\":\"5,1,2,3\"}") == 0,
        "too small buffer -> untouched");
    int v[OCCLUSION_MOD_PARAM_COUNT];
    for (int i = 0; i < OCCLUSION_MOD_PARAM_COUNT; i++) {
        v[i] = occlusion_mod_param_default(i);
    }
    v[0] = 120;
    v[11] = 4;
    occlusion_set_mod_params(v);
    char buf2[256] = "{\"ok\":true}";
    CHECK(occlusion_append_cfg_json(buf2, (int)sizeof(buf2)) && strstr(buf2, "\"2,120,150,") && strstr(buf2, ",60,4\"}"),
        "occcfg follows the set values");
    CHECK(occlusion_mod_param_clamp(0, 999.0) == 200 && occlusion_mod_param_clamp(10, 1.0) == 20
        && occlusion_mod_param_clamp(5, 33.6) == 34, "mod params clamped + rounded");
    occlusion_set_mod_params(NULL);

    occlusion_set_sound_params(800.0f, 9999.0f, -5.0f, 150.0f);
    CHECK(feq(occlusion_get_attack_ms(), 800.0f) && feq(occlusion_get_release_ms(), (float)OCCLUSION_SMOOTH_MS_MAX)
        && feq(occlusion_get_diffraction(), 0.0f) && feq(occlusion_get_reverb(), 1.5f), "sound params clamped");
    occlusion_set_sound_params((float)OCCLUSION_ATTACK_MS_DEFAULT, (float)OCCLUSION_RELEASE_MS_DEFAULT,
        (float)OCCLUSION_DIFFRACTION_DEFAULT, (float)OCCLUSION_REVERB_DEFAULT);
}

/* 072 B2 + B1 */
static void test_diffraction_and_smoothing(void) {
    float g = 1.0f, c = 19000.0f;
    occlusion_apply_diffraction(1.0f, 1.0f, &g, &c);
    CHECK(feq(g, 0.7f) && fabsf(c - OCCLUSION_DIFFRACT_CUTOFF) < 1.0f, "diffraction 1/1 -> -3 dB, 2.5 kHz");
    g = 1.0f; c = 19000.0f;
    occlusion_apply_diffraction(0.0f, 1.0f, &g, &c);
    CHECK(feq(g, 1.0f) && feq(c, 19000.0f), "diffraction 0 is a no-op");
    g = 1.0f; c = 19000.0f;
    occlusion_apply_diffraction(1.0f, 3.0f, &g, &c);
    CHECK(g >= OCCLUSION_FULL_GAIN && feq(c, OCCLUSION_DIFFRACT_MIN_CUTOFF), "diffraction strength 3 -> clamped");

    occlusion_clear();
    occlusion_set_sound_params(500.0f, 250.0f, 100.0f, 100.0f);
    OcclusionInfo t = { 0.8f, OCCLUSION_KIND_TRANSMIT, 0.0f, 0, 0.0f, 0.0f, 0.0f };
    OcclusionInfo n = { 0.0f, OCCLUSION_KIND_NONE, 0.0f, 0, 0.0f, 0.0f, 0.0f };
    OcclusionSmoothed sm;
    occlusion_smooth(41, 10000ULL, &t, 0.25f, 900.0f, &sm);
    CHECK(feq(sm.gain, 0.25f) && fabsf(sm.cutoffHz - 900.0f) < 1.0f, "first call jumps to the target");
    occlusion_smooth(41, 10100ULL, &n, 1.0f, 19000.0f, &sm);
    CHECK(fabsf(sm.gain - (0.25f + 0.75f * (1.0f - expf(-100.0f / 250.0f)))) < 0.002f, "clearer -> release time constant");
    const float g1 = sm.gain;
    CHECK(sm.cutoffHz > 900.0f && sm.cutoffHz < 19000.0f, "cutoff moves on the log scale");
    occlusion_smooth(41, 10200ULL, &t, 0.25f, 900.0f, &sm);
    CHECK(fabsf(sm.gain - (g1 + (0.25f - g1) * (1.0f - expf(-100.0f / 500.0f)))) < 0.002f, "more muffled -> attack time constant");
    occlusion_smooth(41, 13000ULL, &n, 1.0f, 19000.0f, &sm);
    CHECK(feq(sm.gain, 1.0f), "after > 2 s without calls -> jumps to the target");

    OcclusionInfo d = { 0.3f, OCCLUSION_KIND_DIFFRACT, 0.5f, 1, 4.0f, 1.5f, -2.0f };
    occlusion_smooth(42, 20000ULL, &d, 0.9f, 4000.0f, &sm);
    CHECK(sm.relayKnown && feq(sm.relayWeight, 1.0f) && feq(sm.relayX, 4.0f) && feq(sm.relayZ, -2.0f), "D: relay weight 1, point kept");
    /* review 2026-10-20: another opening -> the relay point glides instead of jumping */
    OcclusionInfo d2 = { 0.3f, OCCLUSION_KIND_DIFFRACT, 0.5f, 1, 8.0f, 1.5f, -2.0f };
    occlusion_smooth(42, 20050ULL, &d2, 0.9f, 4000.0f, &sm);
    CHECK(sm.relayX > 4.0f && sm.relayX < 8.0f && feq(sm.relayZ, -2.0f), "new relay point is approached smoothly");
    occlusion_smooth(42, 20100ULL, &n, 1.0f, 19000.0f, &sm);
    CHECK(sm.relayKnown && sm.relayWeight < 1.0f && sm.relayWeight > 0.0f, "after D the direction fades back with the old point");

    OcclusionInfo in1 = { 0.0f, OCCLUSION_KIND_NONE, 1.0f, 0, 0.0f, 0.0f, 0.0f };
    occlusion_smooth(43, 40000ULL, &in1, 1.0f, 19000.0f, &sm);
    CHECK(feq(sm.room, 1.0f), "B4: room starts at the target");
    occlusion_smooth(43, 40100ULL, &n, 1.0f, 19000.0f, &sm);
    CHECK(sm.room < 1.0f && sm.room > 0.5f, "B4: room fades (release) instead of dropping");

    occlusion_set_sound_params(0.0f, 0.0f, 100.0f, 100.0f);
    occlusion_smooth(41, 30000ULL, &t, 0.25f, 900.0f, &sm);
    occlusion_smooth(41, 30050ULL, &n, 1.0f, 19000.0f, &sm);
    CHECK(feq(sm.gain, 1.0f), "time constants 0 -> no smoothing");
    occlusion_set_sound_params((float)OCCLUSION_ATTACK_MS_DEFAULT, (float)OCCLUSION_RELEASE_MS_DEFAULT,
        (float)OCCLUSION_DIFFRACTION_DEFAULT, (float)OCCLUSION_REVERB_DEFAULT);
    occlusion_clear();
}

int main(void) {
    printf("occlusion_test\n");
    test_switch();            /* checks the default-off state, so it runs first */
    occlusion_set_enabled(1); /* stores are only accepted while the switch is on */
    test_v2();
    test_diffraction_and_smoothing();
    test_parse();
    test_stale();
    test_apply();
    test_strength();
    test_probes();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
