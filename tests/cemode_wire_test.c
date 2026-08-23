/* Unit test for the CEMODE wire codec (src/ts/proximity/ts3_cemode_wire.h).
   Header-only and pure, so this exercises the exact code the plugin ships.
   Build/run on Linux: bash tests/run_tests.sh
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_cemode_wire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* Built at runtime so the compiler cannot fold them into a diagnostic. */
static float make_nan(void) {
    volatile float zero = 0.0f;
    return zero / zero;
}

static float make_inf(void) {
    volatile float big = 1e38f;
    return big * big;
}

/* ---- meters -> decimeters -------------------------------------------------- */

static void test_distance_dm(void) {
    printf("[1] meters -> decimeters (saturating, NaN/inf safe)\n");

    CHECK(cemode_wire_distance_dm(0.0f) == 0, "0 m -> 0 dm");
    CHECK(cemode_wire_distance_dm(60.0f) == 600, "60 m -> 600 dm");
    CHECK(cemode_wire_distance_dm(12.34f) == 123, "12.34 m -> 123 dm (rounded)");
    CHECK(cemode_wire_distance_dm(-5.0f) == 0, "negative -> 0 dm");
    CHECK(cemode_wire_distance_dm(1000.0f) == CEMODE_DISTANCE_MAX_DM, "1000 m -> max");
    CHECK(cemode_wire_distance_dm(99999.0f) == CEMODE_DISTANCE_MAX_DM, "huge -> max");
    CHECK(cemode_wire_distance_dm(make_inf()) == CEMODE_DISTANCE_MAX_DM, "+inf -> max");
    CHECK(cemode_wire_distance_dm(make_nan()) == 0, "NaN -> 0 dm");
}

/* ---- round trip ------------------------------------------------------------ */

static void test_round_trip(void) {
    char buf[CEMODE_CMD_MAX];
    int mode;
    long dm;

    printf("[2] round trip — all three modes\n");

    CHECK(cemode_wire_format(0, 100, buf, sizeof(buf)) == 1, "format whisper ok");
    CHECK(strcmp(buf, "CEMODE:1;0;100") == 0, "whisper exact wire text");
    mode = -1; dm = -1;
    CHECK(cemode_wire_parse(buf + CEMODE_PREFIX_LEN, &mode, &dm) == 1, "parse whisper ok");
    CHECK(mode == 0 && dm == 100, "whisper values survive");

    CHECK(cemode_wire_format(1, 300, buf, sizeof(buf)) == 1, "format normal ok");
    mode = -1; dm = -1;
    CHECK(cemode_wire_parse(buf + CEMODE_PREFIX_LEN, &mode, &dm) == 1, "parse normal ok");
    CHECK(mode == 1 && dm == 300, "normal values survive");

    CHECK(cemode_wire_format(2, 600, buf, sizeof(buf)) == 1, "format shout ok");
    CHECK(strcmp(buf, "CEMODE:1;2;600") == 0, "shout exact wire text");
    mode = -1; dm = -1;
    CHECK(cemode_wire_parse(buf + CEMODE_PREFIX_LEN, &mode, &dm) == 1, "parse shout ok");
    CHECK(mode == 2 && dm == 600, "shout values survive");
}

/* ---- mode validation ------------------------------------------------------- */

static void test_mode_bounds(void) {
    char buf[CEMODE_CMD_MAX];
    int mode;
    long dm;

    printf("[3] mode bounds\n");

    CHECK(cemode_wire_format(3, 100, buf, sizeof(buf)) == 0, "format mode 3 -> fail");
    CHECK(cemode_wire_format(-1, 100, buf, sizeof(buf)) == 0, "format mode -1 -> fail");

    mode = 99; dm = 0;
    CHECK(cemode_wire_parse("1;3;100", &mode, &dm) == 0, "parse mode 3 -> fail");
    CHECK(cemode_wire_parse("1;-1;100", &mode, &dm) == 0, "parse mode -1 -> fail");
}

/* ---- distance validation --------------------------------------------------- */

static void test_wire_distance_bounds(void) {
    char buf[CEMODE_CMD_MAX];
    int mode;
    long dm;

    printf("[4] distance bounds on the wire\n");

    CHECK(cemode_wire_format(1, -1, buf, sizeof(buf)) == 0, "format dm -1 -> fail");
    CHECK(cemode_wire_format(1, CEMODE_DISTANCE_MAX_DM + 1, buf, sizeof(buf)) == 0,
        "format dm 10001 -> fail");
    CHECK(cemode_wire_format(1, 0, buf, sizeof(buf)) == 1, "format dm 0 ok");
    CHECK(cemode_wire_format(1, CEMODE_DISTANCE_MAX_DM, buf, sizeof(buf)) == 1,
        "format dm 10000 ok");

    mode = 99; dm = -99;
    CHECK(cemode_wire_parse("1;1;-1", &mode, &dm) == 0, "parse dm -1 -> fail");
    CHECK(cemode_wire_parse("1;1;10001", &mode, &dm) == 0, "parse dm 10001 -> fail");

    mode = -1; dm = -1;
    CHECK(cemode_wire_parse("1;1;0", &mode, &dm) == 1 && dm == 0, "parse dm 0 ok");
    mode = -1; dm = -1;
    CHECK(cemode_wire_parse("1;1;10000", &mode, &dm) == 1 && dm == CEMODE_DISTANCE_MAX_DM,
        "parse dm 10000 ok");
}

/* ---- malformed payloads ---------------------------------------------------- */

static void test_malformed(void) {
    int mode = 99;
    long dm = 99;

    printf("[5] malformed payload rejection\n");

    CHECK(cemode_wire_parse("", &mode, &dm) == 0, "empty string");
    CHECK(cemode_wire_parse("1", &mode, &dm) == 0, "version only");
    CHECK(cemode_wire_parse("1;2", &mode, &dm) == 0, "version;mode only");
    CHECK(cemode_wire_parse("1;2;", &mode, &dm) == 0, "empty distance field");
    CHECK(cemode_wire_parse("x;2;100", &mode, &dm) == 0, "non-numeric version");
    CHECK(cemode_wire_parse("1;x;100", &mode, &dm) == 0, "non-numeric mode");
    CHECK(cemode_wire_parse("1;2;abc", &mode, &dm) == 0, "non-numeric distance");
    CHECK(cemode_wire_parse("1;2;100x", &mode, &dm) == 0, "trailing junk on distance");
    CHECK(cemode_wire_parse("0;1;100", &mode, &dm) == 0, "payload version 0 rejected");
    CHECK(cemode_wire_parse("2;1;100", &mode, &dm) == 0, "unknown future version rejected");
    CHECK(cemode_wire_parse(NULL, &mode, &dm) == 0, "NULL payload rejected");
}

/* ---- forward compatibility ------------------------------------------------- */

static void test_forward_compat(void) {
    int mode = -1;
    long dm = -1;

    printf("[6] forward compatibility — additive trailing field ignored\n");

    CHECK(cemode_wire_parse("1;2;600;7", &mode, &dm) == 1, "extra field parses");
    CHECK(mode == 2 && dm == 600, "extra field: first three values intact");
}

/* ---- buffer safety --------------------------------------------------------- */

static void test_small_buffer(void) {
    char buf[8]; /* "CEMODE:1;2;600" needs 15 */
    char tiny[1];

    printf("[7] small buffer — returns 0, never overflows\n");

    memset(buf, 'X', sizeof(buf));
    CHECK(cemode_wire_format(2, 600, buf, sizeof(buf)) == 0, "too-small buffer -> 0");
    CHECK(buf[0] == '\0', "too-small buffer left empty");

    tiny[0] = 'X';
    CHECK(cemode_wire_format(1, 100, tiny, sizeof(tiny)) == 0, "1-byte buffer -> 0");
    CHECK(tiny[0] == '\0', "1-byte buffer left empty");

    CHECK(cemode_wire_format(1, 100, NULL, 16) == 0, "NULL buffer -> 0");
}

int main(void) {
    printf("cemode wire codec tests\n");
    test_distance_dm();
    test_round_trip();
    test_mode_bounds();
    test_wire_distance_bounds();
    test_malformed();
    test_forward_compat();
    test_small_buffer();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
