/* Unit tests for src/core/config/range_units.h (091): voice ranges shown in
   Conan Exiles "foundations" (1 foundation = 2.56 m) instead of meters.
   Everything is stored in meters; only display/input converts - these checks
   pin the conversion, the hub-limit rounding (min up, max down, so every shown
   value is also valid in meters), the "did the player change the number?" test
   behind the no-drift rule, and the unit strings.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, header only).
   Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\range_units_test.c /Fe:tests\out\range_units_test.exe */

#include "core/config/range_units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static int near(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static void test_constant(void) {
    printf("[1] the foundation edge\n");
    CHECK(near(RANGE_FOUNDATION_EDGE_M, 2.56f, 0.0f), "1 foundation = 2.56 m (256 Unreal units)");
    CHECK(near(RANGE_FOUNDATION_EDGE_M * 100.0f, 256.0f, 0.001f), "2.56 m = 256 UU (1 UU = 1 cm)");
}

static void test_conversion(void) {
    printf("[2] meters <-> display\n");

    /* the doku table (two decimals; 40 / 2.56 = 15.625 exactly) */
    CHECK(near(range_from_meters(2.0f, 1), 0.78f, 0.006f), "2 m  -> 0.78 F");
    CHECK(near(range_from_meters(15.0f, 1), 5.86f, 0.006f), "15 m -> 5.86 F");
    CHECK(near(range_from_meters(40.0f, 1), 15.63f, 0.006f), "40 m -> 15.63 F");
    CHECK(near(range_to_meters(1.0f, 1), 2.56f, 0.0001f), "1 F  -> 2.56 m");
    CHECK(near(range_to_meters(6.0f, 1), 15.36f, 0.0001f), "6 F  -> 15.36 m");
    CHECK(near(range_to_meters(16.0f, 1), 40.96f, 0.0001f), "16 F -> 40.96 m");
    CHECK(near(range_to_meters(6.5f, 1), 16.64f, 0.0001f), "6.5 F (six and a half) -> 16.64 m");
    CHECK(near(range_from_meters(2.56f, 1), 1.0f, 0.0f), "2.56 m is exactly 1.0 F");

    /* meter mode: the number is not touched at all */
    CHECK(range_from_meters(13.0f, 0) == 13.0f && range_to_meters(13.0f, 0) == 13.0f,
        "meter mode: identity both ways");
    CHECK(range_from_meters(0.5f, 0) == 0.5f && range_to_meters(500.0f, 0) == 500.0f,
        "meter mode: identity at the clamp bounds");

    /* round trips */
    int ok = 1;
    for (int i = 5; i <= 5000; i += 7) {
        const float m = (float)i / 10.0f;
        if (!near(range_to_meters(range_from_meters(m, 1), 1), m, m * 1e-6f + 1e-6f)) ok = 0;
    }
    CHECK(ok, "meters -> F -> meters round trip stays within float noise (0.5 .. 500 m)");

    /* what a player types ("%.1f" precision, 0.1 F = 0.256 m) survives the
       %.1f storage of plugin.cfg (0.1 m): display -> meters -> file -> display */
    ok = 1;
    for (int i = 1; i <= 1953; i++) {              /* 0.1 F .. 195.3 F (= 500 m) */
        const float shown = (float)i / 10.0f;
        const float meters = range_to_meters(shown, 1);
        char file[32];
        snprintf(file, sizeof(file), "%.1f", (double)meters);   /* what config_save writes */
        float back = 0.0f;
        if (sscanf(file, "%f", &back) != 1) { ok = 0; break; }
        if (!range_display_unchanged(back, 1, shown)) ok = 0;
    }
    CHECK(ok, "every 0.1 F step reads back as the same number after the 0.1 m file rounding");
}

static void test_effective_unit(void) {
    printf("[3] effective unit = own choice OR server force\n");
    CHECK(range_effective_unit(0, 0) == 0, "neither -> meters");
    CHECK(range_effective_unit(1, 0) == 1, "own choice -> foundations");
    CHECK(range_effective_unit(0, 1) == 1, "server force -> foundations");
    CHECK(range_effective_unit(1, 1) == 1, "both -> foundations");
    CHECK(range_effective_unit(5, 0) == 1 && range_effective_unit(0, -1) == 1, "any non-zero counts as on");
}

static void test_limit_rounding(void) {
    printf("[4] hub limits: min rounded up, max rounded down\n");

    /* the example of the brief: 2.1 m = 0.82 F must read 0.9, not 0.8 */
    CHECK(near(range_limit_min_display(2.1f, 1), 0.9f, 0.0001f), "min 2.1 m (0.82 F) -> 0.9");
    CHECK(range_limit_min_display(2.1f, 1) * RANGE_FOUNDATION_EDGE_M >= 2.1f, "0.9 F = 2.304 m is >= 2.1 m");
    CHECK(range_from_meters(2.1f, 1) * 10.0f < 9.0f && range_from_meters(2.1f, 1) > 0.8f,
        "(precondition) plain rounding would give 0.8 F = 2.048 m, below the limit");

    CHECK(near(range_limit_min_display(15.0f, 1), 5.9f, 0.0001f), "min 15 m (5.86 F) -> 5.9");
    CHECK(near(range_limit_max_display(15.0f, 1), 5.8f, 0.0001f), "max 15 m (5.86 F) -> 5.8");
    CHECK(near(range_limit_max_display(40.0f, 1), 15.6f, 0.0001f), "max 40 m (15.63 F) -> 15.6");
    CHECK(near(range_limit_min_display(5.0f, 1), 2.0f, 0.0001f), "min 5 m (1.95 F) -> 2.0");
    CHECK(near(range_limit_max_display(5.0f, 1), 1.9f, 0.0001f), "max 5 m (1.95 F) -> 1.9");

    /* exact multiples must not be pushed to the next tenth by float noise */
    int ok = 1;
    for (int k = 1; k <= 195; k++) {
        const float meters = (float)k * RANGE_FOUNDATION_EDGE_M;
        const float want = (float)k;
        if (!near(range_limit_min_display(meters, 1), want, 0.0001f)) ok = 0;
        if (!near(range_limit_max_display(meters, 1), want, 0.0001f)) ok = 0;
    }
    CHECK(ok, "k * 2.56 m (1..195 F) -> exactly k, for min AND max");
    CHECK(near(range_limit_min_display(2.56f, 1), 1.0f, 0.0f), "min 2.56 m -> 1.0");
    CHECK(near(range_limit_max_display(2.56f, 1), 1.0f, 0.0f), "max 2.56 m -> 1.0");
    CHECK(near(range_limit_min_display(5.12f, 1), 2.0f, 0.0f), "min 5.12 m -> 2.0");
    {
        /* the value as a hub admin's "7.68" arrives from the parser (float) */
        const float parsed = (float)7.68;
        CHECK(near(range_limit_min_display(parsed, 1), 3.0f, 0.0001f)
            && near(range_limit_max_display(parsed, 1), 3.0f, 0.0001f), "7.68 m parsed as float -> 3.0 both ways");
    }

    /* property: every value shown inside [min, max] is valid in meters too */
    ok = 1;
    for (int i = 1; i <= 2000; i++) {
        const float limit = (float)i * 0.25f;      /* 0.25 .. 500 m */
        const float lo = range_limit_min_display(limit, 1);
        const float hi = range_limit_max_display(limit, 1);
        if (range_to_meters(lo, 1) < limit - 0.001f) ok = 0;            /* min: shown value never below */
        if (range_to_meters(lo - 0.1f, 1) >= limit - 0.0001f) ok = 0;   /* ... and it is the smallest such tenth */
        if (range_to_meters(hi, 1) > limit + 0.001f && hi > 0.1f) ok = 0; /* max: shown value never above */
        if (range_to_meters(hi + 0.1f, 1) <= limit + 0.0001f) ok = 0;   /* ... and it is the largest such tenth */
    }
    CHECK(ok, "2000 limits: shown min is the smallest, shown max the largest valid tenth");

    /* meter mode: unchanged, including odd values */
    CHECK(range_limit_min_display(2.1f, 0) == 2.1f && range_limit_max_display(15.0f, 0) == 15.0f,
        "meter mode returns the limit unchanged");
    CHECK(range_limit_min_display(7.77f, 0) == 7.77f && range_limit_max_display(0.3f, 0) == 0.3f,
        "meter mode: no rounding of odd limits");

    /* 0 = "no limit" stays 0 in both modes (voice_mode_get_distance: set only when > 0) */
    CHECK(range_limit_min_display(0.0f, 1) == 0.0f && range_limit_max_display(0.0f, 1) == 0.0f,
        "0 (unset) stays 0 in foundation mode");
    CHECK(range_limit_min_display(-3.0f, 1) == -3.0f && range_limit_max_display(-3.0f, 1) == -3.0f,
        "negative (unset) is passed through");
    CHECK(isnan(range_limit_min_display(NAN, 1)) && isnan(range_limit_max_display(NAN, 1)),
        "NaN is passed through (never becomes a number)");
    CHECK(near(range_limit_max_display(0.1f, 1), 0.1f, 0.0001f),
        "a tiny SET max (0.1 m) stays a set limit (0.1 F), not 0 = 'no limit'");
}

static void test_unchanged(void) {
    printf("[5] range_display_unchanged: the no-drift test\n");

    /* 40 m is shown as 15.6 F; the player leaves the field alone */
    CHECK(range_display_unchanged(40.0f, 1, 15.6f), "40 m shown as 15.6 F, text still 15.6 -> unchanged");
    CHECK(range_display_unchanged(40.0f, 1, 15.63f), "typed 15.63 prints as 15.6 -> unchanged");
    CHECK(range_display_unchanged(40.0f, 1, 15.600001f), "float noise around 15.6 -> unchanged");
    CHECK(!range_display_unchanged(40.0f, 1, 15.7f), "15.7 is a real change");
    CHECK(!range_display_unchanged(40.0f, 1, 16.0f), "16 is a real change");
    CHECK(range_display_unchanged(15.0f, 1, 5.9f), "15 m shows as 5.9 F (nearest), 5.9 -> unchanged");
    CHECK(range_display_unchanged(3.0f, 1, 1.2f), "3 m shows as 1.2 F -> unchanged");

    /* meter mode */
    CHECK(range_display_unchanged(13.0f, 0, 13.0f), "meters: 13.0 -> unchanged");
    CHECK(range_display_unchanged(13.0f, 0, 13), "meters: typed '13' reads as 13.0 -> unchanged");
    CHECK(range_display_unchanged(13.04f, 0, 13.0f), "meters: 13.04 shows as 13.0 -> unchanged");
    CHECK(!range_display_unchanged(13.0f, 0, 13.1f), "meters: 13.1 is a real change");

    /* the failure the rule prevents: converting the displayed text back */
    const float drifted = range_to_meters(15.6f, 1);
    CHECK(!near(drifted, 40.0f, 0.01f) && near(drifted, 39.936f, 0.001f),
        "(precondition) typing 15.6 F back would give 39.936 m, not 40 m");
}

static void test_units(void) {
    printf("[6] unit names\n");
    CHECK(wcscmp(range_unit_name_w(0), L"meters") == 0, "wide name, meters");
    CHECK(wcscmp(range_unit_name_w(1), L"foundations") == 0, "wide name, foundations");
    CHECK(wcscmp(range_unit_short_w(0), L"m") == 0, "wide short, meters");
    CHECK(wcscmp(range_unit_short_w(1), L"fnd.") == 0, "wide short, foundations");
    CHECK(strcmp(range_unit_name_a(0), "meters") == 0, "narrow name, meters");
    CHECK(strcmp(range_unit_name_a(1), "foundations") == 0, "narrow name, foundations");
    CHECK(wcscmp(range_unit_name_w(7), L"foundations") == 0 && wcscmp(range_unit_short_w(-1), L"fnd.") == 0,
        "any non-zero flag means foundations");
}

int main(void) {
    printf("range_units_test\n");
    test_constant();
    test_conversion();
    test_effective_unit();
    test_limit_rounding();
    test_unchanged();
    test_units();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
