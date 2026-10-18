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
    occlusion_apply(0.0f, &g, &c);
    CHECK(feq(g, 0.8f) && feq(c, 19000.0f), "occ 0 is a no-op");

    g = 1.0f; c = 19000.0f;
    occlusion_apply(1.0f, &g, &c);
    CHECK(feq(g, OCCLUSION_FULL_GAIN), "occ 1 -> full gain multiplier");
    CHECK(fabsf(c - OCCLUSION_FULL_CUTOFF) < 1.0f, "occ 1 -> full cutoff");

    float g5 = 1.0f, c5 = 19000.0f;
    occlusion_apply(0.5f, &g5, &c5);
    CHECK(g5 < 1.0f && g5 > OCCLUSION_FULL_GAIN, "occ 0.5 gain between");
    CHECK(c5 < 19000.0f && c5 > OCCLUSION_FULL_CUTOFF, "occ 0.5 cutoff between");

    g = 1.0f; c = 500.0f;
    occlusion_apply(0.3f, &g, &c);
    CHECK(feq(c, 500.0f), "never raises an already lower cutoff");
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

int main(void) {
    printf("occlusion_test\n");
    test_parse();
    test_stale();
    test_apply();
    test_probes();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
