/* Unit tests for src/core/proximity/zone_resolve.c (pure functions).
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no TS SDK needed).
   Exit code 0 = all checks passed.

   Semantics under test (from zone_resolve.h/.c):
   - zones are quads (x1/z1..x4/z4) + optional GroundY..TopY height band,
   - zone_resolve probes 3 coordinate scales (1x, 100x, 0.01x) and both
     floor layouts (X-Z floor + Y height, X-Y floor + Z height),
   - soundproof is ONE-WAY: outside cannot hear a speaker inside the zone,
     inside CAN hear outside,
   - reverb is active as soon as listener OR speaker stands in a reverb zone. */

#include "core/proximity/zone_resolve.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* Zones are placed away from the origin on purpose: the resolver also tries
   the 0.01x scale, so shapes hugging (0,0) would swallow scaled-down points. */
static void make_settings(HubSettings* s) {
    memset(s, 0, sizeof(*s));
    s->valid = 1;
    s->zoneCount = 2;

    /* Zone 0 "cave": 20..30 square, unbounded height, soundproof, no reverb. */
    strcpy(s->zones[0].name, "cave");
    s->zones[0].x1 = 20.0f; s->zones[0].z1 = 20.0f;
    s->zones[0].x2 = 30.0f; s->zones[0].z2 = 20.0f;
    s->zones[0].x3 = 30.0f; s->zones[0].z3 = 30.0f;
    s->zones[0].x4 = 20.0f; s->zones[0].z4 = 30.0f;
    s->zones[0].soundproof = 1;
    s->zones[0].reverb = 0;

    /* Zone 1 "hall": 100..120 square, height band 50..60, reverb only. */
    strcpy(s->zones[1].name, "hall");
    s->zones[1].x1 = 100.0f; s->zones[1].z1 = 100.0f;
    s->zones[1].x2 = 120.0f; s->zones[1].z2 = 100.0f;
    s->zones[1].x3 = 120.0f; s->zones[1].z3 = 120.0f;
    s->zones[1].x4 = 100.0f; s->zones[1].z4 = 120.0f;
    s->zones[1].groundY = 50.0f;
    s->zones[1].topY = 60.0f;
    s->zones[1].soundproof = 0;
    s->zones[1].reverb = 1;
}

static void test_point_in_zone(void) {
    printf("[1] zone_resolve inside/outside/edge\n");
    HubSettings s;
    make_settings(&s);

    CHECK(zone_resolve(&s, 25.0f, 0.0f, 25.0f) == 0, "center of zone 0 -> index 0");
    CHECK(zone_resolve(&s, 5.0f, 0.0f, 5.0f) == -1, "point outside all zones -> -1");
    CHECK(zone_resolve(&s, 25.0f, 0.0f, 35.0f) == -1, "just north of zone 0 -> -1");

    /* Ray casting is half-open: min edge counts as inside, max edge as outside. */
    CHECK(zone_resolve(&s, 20.0f, 0.0f, 25.0f) == 0, "west edge (x=20) -> inside");
    CHECK(zone_resolve(&s, 30.0f, 0.0f, 25.0f) == -1, "east edge (x=30) -> outside");

    /* Scale probing: same zone found when coordinates arrive in UE cm (x0.01
       brings them back to meters via the 0.01 scale probe). */
    CHECK(zone_resolve(&s, 2500.0f, 0.0f, 2500.0f) == 0,
        "centimeter coordinates resolve via 0.01x scale");
}

static void test_height_band(void) {
    printf("[2] height band (GroundY..TopY)\n");
    HubSettings s;
    make_settings(&s);

    CHECK(zone_resolve(&s, 110.0f, 55.0f, 110.0f) == 1, "inside band (y=55) -> zone 1");
    CHECK(zone_resolve(&s, 110.0f, 70.0f, 110.0f) == -1, "above band (y=70) -> -1");
    CHECK(zone_resolve(&s, 110.0f, 40.0f, 110.0f) == -1, "below band (y=40) -> -1");
    /* 1 m tolerance on the band (hEps in zone_resolve.c). */
    CHECK(zone_resolve(&s, 110.0f, 60.5f, 110.0f) == 1, "0.5m above TopY -> tolerated");

    /* Zone 0 has groundY == topY == 0 -> unbounded height. */
    CHECK(zone_resolve(&s, 25.0f, 9999.0f, 25.0f) == 0, "unbounded zone ignores height");
}

static void test_soundproof_one_way(void) {
    printf("[3] soundproof one-way rules\n");
    HubSettings s;
    make_settings(&s);

    /* localZone = listener, remoteZone = speaker. Zone 0 is soundproof. */
    CHECK(zone_soundproof_muted(&s, -1, 0) == 1, "speaker inside, listener outside -> muted");
    CHECK(zone_soundproof_muted(&s, 1, 0) == 1, "speaker inside, listener in other zone -> muted");
    CHECK(zone_soundproof_muted(&s, 0, -1) == 0, "speaker outside, listener inside -> NOT muted");
    CHECK(zone_soundproof_muted(&s, 0, 0) == 0, "both in same zone -> normal proximity");
    CHECK(zone_soundproof_muted(&s, -1, 1) == 0, "speaker in non-soundproof zone -> not muted");
    CHECK(zone_soundproof_muted(&s, -1, -1) == 0, "both outside -> not muted");

    CHECK(zone_soundproof_muted(&s, -1, 99) == 0, "out-of-range speaker zone -> not muted");
    CHECK(zone_soundproof_muted(NULL, -1, 0) == 0, "NULL settings -> not muted");
}

static void test_reverb_active(void) {
    printf("[4] reverb active flag\n");
    HubSettings s;
    make_settings(&s);

    CHECK(zone_reverb_active(&s, 1, -1) == 1, "listener in reverb zone -> active");
    CHECK(zone_reverb_active(&s, -1, 1) == 1, "speaker in reverb zone -> active");
    CHECK(zone_reverb_active(&s, 1, 1) == 1, "both in reverb zone -> active");
    CHECK(zone_reverb_active(&s, 0, -1) == 0, "non-reverb zone -> inactive");
    CHECK(zone_reverb_active(&s, -1, -1) == 0, "open world -> inactive");
    CHECK(zone_reverb_active(&s, 99, -99) == 0, "out-of-range indices -> inactive");
    CHECK(zone_reverb_active(NULL, 1, 1) == 0, "NULL settings -> inactive");
}

static void test_degenerate_zones(void) {
    printf("[5] degenerate input\n");
    HubSettings s;

    CHECK(zone_resolve(NULL, 1, 1, 1) == -1, "NULL settings -> -1");

    memset(&s, 0, sizeof(s));
    CHECK(zone_resolve(&s, 1, 1, 1) == -1, "zoneCount 0 -> -1");

    /* Name-only zone (all corners 0) must be skipped, not match everything. */
    make_settings(&s);
    memset(&s.zones[0], 0, sizeof(s.zones[0]));
    strcpy(s.zones[0].name, "nameonly");
    CHECK(zone_resolve(&s, 25.0f, 0.0f, 25.0f) == -1, "corner-less zone is skipped");
}

static void test_overlapping_zones(void) {
    printf("[6] overlapping zones — first match wins\n");
    HubSettings s;
    make_settings(&s);

    /* Zone 1 hall (100..120) contains point (110,55,110); zone 0 cave does not. */
    CHECK(zone_resolve(&s, 110.0f, 55.0f, 110.0f) == 1, "point in hall only -> zone 1");

    /* Stack a second zone 0 copy on top of cave center — index 0 wins. */
    s.zones[0].x1 = 20.0f; s.zones[0].z1 = 20.0f;
    s.zones[0].x2 = 30.0f; s.zones[0].z2 = 20.0f;
    s.zones[0].x3 = 30.0f; s.zones[0].z3 = 30.0f;
    s.zones[0].x4 = 20.0f; s.zones[0].z4 = 30.0f;
    CHECK(zone_resolve(&s, 25.0f, 0.0f, 25.0f) == 0, "overlapping coverage -> lowest index");
}

static void test_corners_and_z_edges(void) {
    printf("[7] quad corners and Z edges\n");
    HubSettings s;
    make_settings(&s);

    CHECK(zone_resolve(&s, 20.0f, 0.0f, 20.0f) == 0, "SW corner (min x, min z) inside");
    CHECK(zone_resolve(&s, 20.1f, 0.0f, 29.9f) == 0, "inset NW near max z still inside");
    CHECK(zone_resolve(&s, 20.0f, 0.0f, 30.0f) == -1, "exact max z corner outside (half-open)");
    CHECK(zone_resolve(&s, 25.0f, 0.0f, 20.0f) == 0, "south edge (z=20) inside");
    CHECK(zone_resolve(&s, 25.0f, 0.0f, 30.0f) == -1, "north edge (z=30) outside");
}

static void test_height_band_boundaries(void) {
    printf("[8] height band ±1m tolerance\n");
    HubSettings s;
    make_settings(&s);

    /* hall band 50..60 with hEps=1 -> effective 49..61 */
    CHECK(zone_resolve(&s, 110.0f, 49.0f, 110.0f) == 1, "1m below GroundY tolerated");
    CHECK(zone_resolve(&s, 110.0f, 48.9f, 110.0f) == -1, "just outside lower tolerance -> -1");
    CHECK(zone_resolve(&s, 110.0f, 61.0f, 110.0f) == 1, "1m above TopY tolerated");
    CHECK(zone_resolve(&s, 110.0f, 61.1f, 110.0f) == -1, "just outside upper tolerance -> -1");
}

static void test_scale_probes(void) {
    printf("[9] coordinate scale probes\n");
    HubSettings s;
    memset(&s, 0, sizeof(s));
    s.valid = 1;
    s.zoneCount = 1;
    /* Admin entered UE centimeters; player position arrives in meters. */
    s.zones[0].x1 = 2000.0f; s.zones[0].z1 = 2000.0f;
    s.zones[0].x2 = 3000.0f; s.zones[0].z2 = 2000.0f;
    s.zones[0].x3 = 3000.0f; s.zones[0].z3 = 3000.0f;
    s.zones[0].x4 = 2000.0f; s.zones[0].z4 = 3000.0f;

    CHECK(zone_resolve(&s, 25.0f, 0.0f, 25.0f) == 0,
        "meters position ×100 matches cm corners");
    CHECK(zone_resolve(&s, 2500.0f, 0.0f, 2500.0f) == 0,
        "raw cm coordinates match at 1x scale");
}

static void test_ue_layout_and_reverb_mix(void) {
    printf("[10] UE X-Y floor layout + mixed reverb/soundproof\n");
    HubSettings s;
    memset(&s, 0, sizeof(s));
    s.valid = 1;
    s.zoneCount = 1;
    /* xzFloor=0: zone Z* fields hold world Y; player height is pz. */
    s.zones[0].x1 = 20.0f; s.zones[0].z1 = 20.0f;
    s.zones[0].x2 = 30.0f; s.zones[0].z2 = 20.0f;
    s.zones[0].x3 = 30.0f; s.zones[0].z3 = 30.0f;
    s.zones[0].x4 = 20.0f; s.zones[0].z4 = 30.0f;
    s.zones[0].groundY = 5.0f;
    s.zones[0].topY = 15.0f;
    s.zones[0].reverb = 1;

    CHECK(zone_resolve(&s, 25.0f, 25.0f, 10.0f) == 0, "UE layout: inside X-Y + height");
    CHECK(zone_resolve(&s, 25.0f, 25.0f, 3.0f) == -1, "UE layout: below height band");
    CHECK(zone_reverb_active(&s, 0, -1) == 1, "listener in UE-layout reverb zone");
    CHECK(zone_soundproof_muted(&s, -1, 0) == 0, "non-soundproof zone never hard-mutes");
}

int main(void) {
    test_point_in_zone();
    test_height_band();
    test_soundproof_one_way();
    test_reverb_active();
    test_degenerate_zones();
    test_overlapping_zones();
    test_corners_and_z_edges();
    test_height_band_boundaries();
    test_scale_probes();
    test_ue_layout_and_reverb_mix();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
