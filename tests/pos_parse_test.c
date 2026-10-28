/* Unit tests for src/core/mod_file/pos_parse.h (Pos.txt / legacy HTTP line
   parser and the sample sanity checks shared by the file reader and the HTTP
   inject path).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\pos_parse_test.c /Fe:tests\out\pos_parse_test.exe */

#include "core/mod_file/pos_parse.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void test_accepted_formats(void) {
    printf("[1] accepted formats unchanged\n");
    PosSample s;

    CHECK(pos_parse_line("SEQ=7 X=12345.5 Y=-67890.25 Z=200 YAW=45 YAWY=-10", &s)
        && s.seq == 7 && s.x == 12345.5 && s.y == -67890.25 && s.z == 200.0 && s.yaw == 45.0 && s.yawY == -10.0,
        "full line incl. YAWY");
    CHECK(pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=90", &s) && s.yawY == 0.0, "YAWY optional -> 0");
    CHECK(pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=90 YAWY=", &s) && s.yawY == 0.0, "empty optional YAWY -> 0 (as before)");
    CHECK(pos_parse_line("SEQ=1\nX=100\nY=200\nZ=300\nYAW=90\n", &s) && s.x == 100.0, "line breaks as separators");
    CHECK(pos_parse_line("SEQ=1 X=1e3 Y=2.5E2 Z=-3e0 YAW=0", &s) && s.x == 1000.0 && s.y == 250.0 && s.z == -3.0,
        "exponent notation");
    CHECK(pos_parse_line("SEQ=1 X= 100 Y=200 Z=300 YAW=90", &s) && s.x == 100.0, "blank after '=' is still a number");
    CHECK(pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=90 OCC=3:40:T:0:1.2.3", &s), "trailing OCC field ignored");
    CHECK(pos_parse_line("SEQ=2 X=0 Y=0 Z=0 YAW=0", &s) && s.x == 0.0, "parse accepts (0,0,0); the placeholder rule is 'plausible'");
}

static void test_rejected(void) {
    printf("[2] rejected input\n");
    PosSample s;
    memset(&s, 0x5a, sizeof(s));
    PosSample before = s;

    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=", &s), "empty YAW= (was accepted as 0.0)");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=\n", &s), "YAW= at line end");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW= YAWY=5", &s), "YAW= followed by the next key");
    CHECK(!pos_parse_line("SEQ=1 X=abc Y=200 Z=300 YAW=0", &s), "non-numeric X");
    CHECK(!pos_parse_line("SEQ= X=1 Y=2 Z=3 YAW=0", &s), "empty SEQ");
    CHECK(!pos_parse_line("SEQ=1 Y=200 Z=300 YAW=0", &s), "missing X");
    CHECK(!pos_parse_line("X=1 Y=2 Z=3 YAW=0", &s), "missing SEQ");
    CHECK(!pos_parse_line("", &s) && !pos_parse_line("garbage", &s), "empty / garbage");
    CHECK(!pos_parse_line(NULL, &s) && !pos_parse_line("SEQ=1 X=1 Y=2 Z=3 YAW=0", NULL), "NULL arguments");

    CHECK(!pos_parse_line("SEQ=1 X=1e300 Y=200 Z=300 YAW=0", &s), "x=1e300 (finite, but inf after the float cast)");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=-1e300 Z=300 YAW=0", &s), "y=-1e300");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=1e9 YAW=0", &s), "z=1e9 cm (10000 km)");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=1e300", &s), "yaw=1e300");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=0 YAWY=1e300", &s), "yawY=1e300");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=inf", &s), "yaw=inf");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=nan", &s), "yaw=nan");
    CHECK(!pos_parse_line("SEQ=1 X=100 Y=200 Z=300 YAW=0 YAWY=nan", &s), "yawY=nan");
    CHECK(!pos_parse_line("SEQ=1 X=inf Y=200 Z=300 YAW=0", &s), "x=inf");
    CHECK(!pos_parse_line("SEQ=1 X=1e999 Y=200 Z=300 YAW=0", &s), "x=1e999 (overflow -> inf)");

    CHECK(memcmp(&s, &before, sizeof(s)) == 0, "a rejected line never writes the output");
}

static void test_limits_and_seq(void) {
    printf("[3] sanity limits + seq\n");
    PosSample s;

    CHECK(pos_parse_line("SEQ=1 X=99999999 Y=-99999999 Z=1 YAW=359.99", &s), "just below 1e8 cm");
    CHECK(pos_parse_line("SEQ=1 X=100000000 Y=0 Z=1 YAW=0", &s), "exactly 1e8 cm is still allowed");
    CHECK(!pos_parse_line("SEQ=1 X=100000001 Y=0 Z=1 YAW=0", &s), "just above 1e8 cm");

    CHECK(pos_parse_line("SEQ=1e300 X=1 Y=2 Z=3 YAW=0", &s) && s.seq == 2147483647, "huge seq clamps (was undefined behaviour)");
    CHECK(pos_parse_line("SEQ=-1e300 X=1 Y=2 Z=3 YAW=0", &s) && s.seq == (-2147483647 - 1), "huge negative seq clamps");
    CHECK(pos_seq_from_double(12.9) == 12 && pos_seq_from_double(-3.5) == -3, "normal seq truncates like (int)");
    CHECK(pos_seq_from_double(NAN) == 0, "NaN seq -> 0");
}

static void test_plausible(void) {
    printf("[4] plausible (inject path)\n");
    PosSample s;
    memset(&s, 0, sizeof(s));

    s.x = 0; s.y = 0; s.z = 0;
    CHECK(!pos_sample_is_plausible(&s), "(0,0,0) placeholder rejected");
    s.x = 0.5; s.y = -0.5; s.z = 0.9;
    CHECK(!pos_sample_is_plausible(&s), "all below 1 cm rejected");
    s.x = 1.0;
    CHECK(pos_sample_is_plausible(&s), "one coordinate >= 1 cm is a real position");
    s.x = 12345.0; s.y = 67890.0; s.z = 200.0; s.yaw = 45.0; s.yawY = 0.0;
    CHECK(pos_sample_is_plausible(&s), "normal position");
    CHECK(!pos_sample_is_plausible(NULL), "NULL");

    s.yaw = INFINITY;
    CHECK(!pos_sample_is_plausible(&s), "yaw=inf rejected (not checked before)");
    s.yaw = 45.0; s.yawY = -INFINITY;
    CHECK(!pos_sample_is_plausible(&s), "yawY=-inf rejected (not checked before)");
    s.yawY = 0.0; s.x = 1e300;
    CHECK(!pos_sample_is_plausible(&s), "x=1e300 rejected (HTTP JSON path)");
    s.x = 100.0; s.yaw = 1e300;
    CHECK(!pos_sample_is_plausible(&s), "yaw=1e300 rejected");
    s.yaw = NAN;
    CHECK(!pos_sample_is_plausible(&s), "yaw=nan rejected");
}

int main(void) {
    printf("pos_parse_test\n");
    test_accepted_formats();
    test_rejected();
    test_limits_and_seq();
    test_plausible();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
