/* Unit test for the CEAUTH wire codec (src/ts/proximity/ts3_ceauth_wire.h).
   Header-only and pure, so this exercises the exact code the plugin ships.
   Build/run on Linux: bash tests/run_tests.sh
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_ceauth_wire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- round trip ------------------------------------------------------------ */

static void test_round_trip(void) {
    char buf[CEAUTH_CMD_MAX];
    unsigned long long id;

    printf("[1] round trip\n");

    CHECK(ceauth_wire_format(76561197960265728ULL, buf, sizeof(buf)) == 1,
        "format base SteamID64 ok");
    CHECK(strcmp(buf, "CEAUTH:1;76561197960265728") == 0, "base id exact wire text");
    id = 0ULL;
    CHECK(ceauth_wire_parse(buf + CEAUTH_PREFIX_LEN, &id) == 1, "parse base id ok");
    CHECK(id == 76561197960265728ULL, "base id survives");

    CHECK(ceauth_wire_format(76561198000000123ULL, buf, sizeof(buf)) == 1,
        "format real-looking id ok");
    id = 0ULL;
    CHECK(ceauth_wire_parse(buf + CEAUTH_PREFIX_LEN, &id) == 1, "parse real-looking id ok");
    CHECK(id == 76561198000000123ULL, "real-looking id survives");

    CHECK(ceauth_wire_format(18446744073709551615ULL, buf, sizeof(buf)) == 1,
        "format max u64 ok");
    id = 0ULL;
    CHECK(ceauth_wire_parse(buf + CEAUTH_PREFIX_LEN, &id) == 1, "parse max u64 ok");
    CHECK(id == 18446744073709551615ULL, "max u64 survives");
}

/* ---- zero id --------------------------------------------------------------- */

static void test_zero_id(void) {
    char buf[CEAUTH_CMD_MAX];
    unsigned long long id = 999ULL;

    printf("[2] zero id — never on the wire\n");

    memset(buf, 'X', sizeof(buf));
    CHECK(ceauth_wire_format(0ULL, buf, sizeof(buf)) == 0, "format id 0 -> 0");
    CHECK(buf[0] == '\0', "format id 0 leaves empty buffer");
    CHECK(ceauth_wire_parse("1;0", &id) == 0, "parse id 0 rejected");
}

/* ---- malformed payloads ---------------------------------------------------- */

static void test_malformed(void) {
    unsigned long long id = 123ULL;

    printf("[3] malformed payload rejection\n");

    CHECK(ceauth_wire_parse("", &id) == 0, "empty string");
    CHECK(ceauth_wire_parse("1", &id) == 0, "version only");
    CHECK(ceauth_wire_parse("1;", &id) == 0, "empty id field");
    CHECK(ceauth_wire_parse("x;5", &id) == 0, "non-numeric version");
    CHECK(ceauth_wire_parse("1;x", &id) == 0, "non-numeric id");
    CHECK(ceauth_wire_parse("1;123x", &id) == 0, "trailing junk on id");
    CHECK(ceauth_wire_parse("0;123", &id) == 0, "payload version 0 rejected");
    CHECK(ceauth_wire_parse("2;123", &id) == 0, "unknown future version rejected");
    CHECK(ceauth_wire_parse(NULL, &id) == 0, "NULL payload rejected");
}

/* ---- forward compatibility ------------------------------------------------- */

static void test_forward_compat(void) {
    unsigned long long id = 0ULL;

    printf("[4] forward compatibility — additive trailing field ignored\n");

    CHECK(ceauth_wire_parse("1;76561197960265728;extra", &id) == 1,
        "extra field parses");
    CHECK(id == 76561197960265728ULL, "extra field: id intact");
}

/* ---- buffer safety --------------------------------------------------------- */

static void test_small_buffer(void) {
    char buf[10]; /* "CEAUTH:1;76561197960265728" needs 27 */
    char tiny[1];

    printf("[5] small buffer — returns 0, never overflows\n");

    memset(buf, 'X', sizeof(buf));
    CHECK(ceauth_wire_format(76561197960265728ULL, buf, sizeof(buf)) == 0,
        "too-small buffer -> 0");
    CHECK(buf[0] == '\0', "too-small buffer left empty");

    tiny[0] = 'X';
    CHECK(ceauth_wire_format(1ULL, tiny, sizeof(tiny)) == 0, "1-byte buffer -> 0");
    CHECK(tiny[0] == '\0', "1-byte buffer left empty");

    CHECK(ceauth_wire_format(1ULL, NULL, 16) == 0, "NULL buffer -> 0");
}

int main(void) {
    printf("ceauth wire codec tests\n");
    test_round_trip();
    test_zero_id();
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
