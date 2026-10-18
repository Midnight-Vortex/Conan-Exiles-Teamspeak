/* Unit tests for src/core/proximity/talk_state.c (067).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\talk_state_test.c src\core\proximity\talk_state.c
        src\core\proximity\player_table.c src\core\proximity\proximity_math.c /Fe:tests\out\talk_state_test.exe */

#include "core/proximity/talk_state.h"
#include "core/proximity/player_table.h"

#include <stdio.h>
#include <string.h>

/* player_table.c / proximity_math.c log through the plugin log module. */
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

static void test_flags(void) {
    printf("[1] talk flags\n");
    talk_state_clear();
    CHECK(!talk_state_local() && !talk_state_is_talking(5), "start: nobody talks");
    talk_state_set(5, 1, 0);
    talk_state_set(9, 1, 1);
    CHECK(talk_state_is_talking(5) && talk_state_is_talking(9), "remote 5 talks, local 9 talks");
    CHECK(talk_state_local(), "local flag set");
    talk_state_set(9, 0, 1);
    CHECK(!talk_state_local(), "local stopped");
    talk_state_clear_client(5);
    CHECK(!talk_state_is_talking(5), "clear_client");
    talk_state_set(0, 1, 0);
    talk_state_set(4096, 1, 0);
    CHECK(!talk_state_is_talking(0) && !talk_state_is_talking(4096), "out of range IDs ignored");
}

static void test_json(void) {
    printf("[2] talk_state_append_json\n");
    talk_state_clear();
    player_table_clear();
    /* plugin coordinates (meters, Y-up): listener at origin */
    player_table_put(11, "near", 3.0f, 1.5f, 4.0f, 30.0f, NULL);   /* 5 m */
    player_table_put(12, "silent", 1.0f, 0.0f, 1.0f, 30.0f, NULL); /* not talking */
    player_table_put(13, "far", 80.0f, 0.0f, 0.0f, 30.0f, NULL);   /* out of range */
    player_table_put(14, "closest", 1.0f, 0.0f, 0.0f, 30.0f, NULL);/* 1 m */
    talk_state_set(11, 1, 0);
    talk_state_set(13, 1, 0);
    talk_state_set(14, 1, 0);
    talk_state_set(7, 1, 1); /* local */

    char buf[512];
    strcpy(buf, "{\"ok\":true}");
    int n = talk_state_append_json(buf, sizeof(buf), 0.0f, 0.0f, 0.0f);
    printf("       %s\n", buf);
    CHECK(n == (int)strlen(buf), "returns new length");
    CHECK(strcmp(buf, "{\"ok\":true,\"talk\":1,\"talkers\":\"100,0,0;300,400,150\"}") == 0,
        "local talk=1, nearest first, UE axes (X, Y=plugin Z, Z=plugin Y) in cm, silent + far left out");

    strcpy(buf, "{\"ok\":true,\"probe\":\"11,300,400,150\"}");
    talk_state_append_json(buf, sizeof(buf), 0.0f, 0.0f, 0.0f);
    CHECK(strstr(buf, "\"probe\":\"11,300,400,150\",\"talk\":1") != NULL, "appended after the probe list");

    char small[24];
    strcpy(small, "{\"ok\":true}");
    CHECK(talk_state_append_json(small, sizeof(small), 0, 0, 0) == 0 && strcmp(small, "{\"ok\":true}") == 0,
        "too small buffer -> 0, unchanged");
    strcpy(buf, "no json");
    CHECK(talk_state_append_json(buf, sizeof(buf), 0, 0, 0) == 0, "input without closing brace -> 0");

    talk_state_clear();
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0);
    CHECK(strcmp(buf, "{\"ok\":true,\"talk\":0,\"talkers\":\"\"}") == 0, "nobody talking -> talk 0, empty list");

    /* cap: 20 talkers in range -> 16 listed */
    player_table_clear();
    for (int i = 0; i < 20; i++) {
        player_table_put((unsigned short)(100 + i), "p", (float)(i + 1), 0.0f, 0.0f, 30.0f, NULL);
        talk_state_set((unsigned)(100 + i), 1, 0);
    }
    char big[1536];
    strcpy(big, "{\"ok\":true}");
    talk_state_append_json(big, sizeof(big), 0, 0, 0);
    int semis = 0;
    for (const char* p = big; *p; p++) {
        semis += (*p == ';');
    }
    CHECK(semis == TALK_MAX_TALKERS - 1, "capped at TALK_MAX_TALKERS");
}

int main(void) {
    printf("talk_state_test\n");
    test_flags();
    test_json();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
