/* Unit test for the CEPING wire codec (src/ts/proximity/ts3_ceping_wire.h).
   Header-only and pure, so this exercises the exact code the plugin ships.
   Build/run on Linux: bash tests/run_tests.sh
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_ceping_wire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- round trip ------------------------------------------------------------ */

static void test_round_trip(void) {
    char buf[CEPING_CMD_MAX];
    unsigned long seq;

    printf("[1] round trip\n");

    CHECK(ceping_wire_format(0, buf, sizeof(buf)) == 1, "format seq 0 ok");
    CHECK(strcmp(buf, "CEPING:1;0") == 0, "seq 0 exact wire text");
    seq = 999;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse seq 0 ok");
    CHECK(seq == 0, "seq 0 survives");

    CHECK(ceping_wire_format(42, buf, sizeof(buf)) == 1, "format seq 42 ok");
    CHECK(strcmp(buf, "CEPING:1;42") == 0, "seq 42 exact wire text");
    seq = 0;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse seq 42 ok");
    CHECK(seq == 42, "seq 42 survives");

    CHECK(ceping_wire_format(4294967295UL, buf, sizeof(buf)) == 1, "format max u32 ok");
    seq = 0;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse max u32 ok");
    CHECK(seq == 4294967295UL, "max u32 survives");
}

/* ---- malformed payloads ---------------------------------------------------- */

static void test_malformed(void) {
    unsigned long seq = 123;

    printf("[2] malformed payload rejection\n");

    CHECK(ceping_wire_parse("", &seq) == 0, "empty string");
    CHECK(ceping_wire_parse("1", &seq) == 0, "version only");
    CHECK(ceping_wire_parse("1;", &seq) == 0, "empty sequence field");
    CHECK(ceping_wire_parse("x;5", &seq) == 0, "non-numeric version");
    CHECK(ceping_wire_parse("1;x", &seq) == 0, "non-numeric sequence");
    CHECK(ceping_wire_parse("1;5x", &seq) == 0, "trailing junk on sequence");
    CHECK(ceping_wire_parse("0;5", &seq) == 0, "payload version 0 rejected");
    CHECK(ceping_wire_parse("2;5", &seq) == 0, "unknown future version rejected");
    CHECK(ceping_wire_parse(NULL, &seq) == 0, "NULL payload rejected");
}

/* ---- forward compatibility ------------------------------------------------- */

static void test_forward_compat(void) {
    unsigned long seq = 0;

    printf("[3] forward compatibility — additive trailing field ignored\n");

    CHECK(ceping_wire_parse("1;77;extra", &seq) == 1, "extra field parses");
    CHECK(seq == 77, "extra field: sequence intact");
}

/* ---- sequence gap (wraparound-safe) ---------------------------------------- */

static void test_seq_gap(void) {
    printf("[4] sequence gap — lost-heartbeat count\n");

    CHECK(ceping_seq_gap(10, 11) == 0, "consecutive -> 0 lost");
    CHECK(ceping_seq_gap(10, 12) == 1, "one skipped -> 1 lost");
    CHECK(ceping_seq_gap(10, 15) == 4, "four skipped -> 4 lost");
    CHECK(ceping_seq_gap(10, 10) == 0, "duplicate -> 0");
    CHECK(ceping_seq_gap(10, 9) == 0, "backward/reorder -> 0");
    CHECK(ceping_seq_gap(10, 5) == 0, "big backward -> 0");

    /* uint32 wraparound. 0xFFFFFFFE -> 0xFFFFFFFF -> 0x00000000 -> 0x00000001. */
    CHECK(ceping_seq_gap(0xFFFFFFFEUL, 0xFFFFFFFFUL) == 0, "wrap consecutive -> 0");
    CHECK(ceping_seq_gap(0xFFFFFFFFUL, 0x00000000UL) == 0, "wrap FF->0 consecutive -> 0");
    CHECK(ceping_seq_gap(0xFFFFFFFEUL, 0x00000000UL) == 1, "wrap skips FF -> 1");
    CHECK(ceping_seq_gap(0xFFFFFFFFUL, 0x00000001UL) == 1, "wrap skips 0 -> 1");
}

/* ---- buffer safety --------------------------------------------------------- */

static void test_small_buffer(void) {
    char buf[6]; /* "CEPING:1;42" needs 12 */
    char tiny[1];

    printf("[5] small buffer — returns 0, never overflows\n");

    memset(buf, 'X', sizeof(buf));
    CHECK(ceping_wire_format(42, buf, sizeof(buf)) == 0, "too-small buffer -> 0");
    CHECK(buf[0] == '\0', "too-small buffer left empty");

    tiny[0] = 'X';
    CHECK(ceping_wire_format(1, tiny, sizeof(tiny)) == 0, "1-byte buffer -> 0");
    CHECK(tiny[0] == '\0', "1-byte buffer left empty");

    CHECK(ceping_wire_format(1, NULL, 16) == 0, "NULL buffer -> 0");
}

int main(void) {
    printf("ceping wire codec tests\n");
    test_round_trip();
    test_malformed();
    test_forward_compat();
    test_seq_gap();
    test_small_buffer();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
