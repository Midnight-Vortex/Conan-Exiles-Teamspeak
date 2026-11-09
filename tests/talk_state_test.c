/* Unit tests for src/core/proximity/talk_state.c (067, 098).
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

#define NOW 100000ULL /* fake GetTickCount64() */

/* TS says "talking" and its voice was just played (098: both are needed). */
static void talk_on(unsigned int clientID) {
    talk_state_set(clientID, 1, 0);
    talk_state_note_voice(clientID, NOW);
}

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
    talk_on(11);
    talk_on(13);
    talk_on(14);
    talk_state_set(7, 1, 1); /* local */

    char buf[512];
    strcpy(buf, "{\"ok\":true}");
    int n = talk_state_append_json(buf, sizeof(buf), 0.0f, 0.0f, 0.0f, NOW);
    printf("       %s\n", buf);
    CHECK(n == (int)strlen(buf), "returns new length");
    CHECK(strcmp(buf, "{\"ok\":true,\"talk\":1,\"talkers\":\"100,0,0;300,400,150\"}") == 0,
        "local talk=1, nearest first, UE axes (X, Y=plugin Z, Z=plugin Y) in cm, silent + far left out");

    strcpy(buf, "{\"ok\":true,\"probe\":\"11,300,400,150\"}");
    talk_state_append_json(buf, sizeof(buf), 0.0f, 0.0f, 0.0f, NOW);
    CHECK(strstr(buf, "\"probe\":\"11,300,400,150\",\"talk\":1") != NULL, "appended after the probe list");

    char small[24];
    strcpy(small, "{\"ok\":true}");
    CHECK(talk_state_append_json(small, sizeof(small), 0, 0, 0, NOW) == 0 && strcmp(small, "{\"ok\":true}") == 0,
        "too small buffer -> 0, unchanged");
    strcpy(buf, "no json");
    CHECK(talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW) == 0, "input without closing brace -> 0");

    talk_state_clear();
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW);
    CHECK(strcmp(buf, "{\"ok\":true,\"talk\":0,\"talkers\":\"\"}") == 0, "nobody talking -> talk 0, empty list");

    /* cap: 20 talkers in range -> 16 listed */
    player_table_clear();
    for (int i = 0; i < 20; i++) {
        player_table_put((unsigned short)(100 + i), "p", (float)(i + 1), 0.0f, 0.0f, 30.0f, NULL);
        talk_on((unsigned)(100 + i));
    }
    char big[1536];
    strcpy(big, "{\"ok\":true}");
    talk_state_append_json(big, sizeof(big), 0, 0, 0, NOW);
    int semis = 0;
    for (const char* p = big; *p; p++) {
        semis += (*p == ';');
    }
    CHECK(semis == TALK_MAX_TALKERS - 1, "capped at TALK_MAX_TALKERS");
}

/* 098: a mouth only moves while TS says "talking" AND the voice is really played. */
static void test_voice_gate(void) {
    printf("[3] voice must be playing (098)\n");
    talk_state_clear();
    player_table_clear();
    player_table_put(21, "a", 2.0f, 0.0f, 0.0f, 30.0f, NULL);
    char buf[256];

    talk_state_set(21, 1, 0); /* TS status only, no voice block ever played */
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW);
    CHECK(strstr(buf, "\"talkers\":\"\"") != NULL, "status without voice (muted, whisper, lost audio) -> no mouth");

    talk_state_note_voice(21, NOW);
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW + 100);
    CHECK(strstr(buf, "\"talkers\":\"200,0,0\"") != NULL, "status + voice 100 ms ago -> mouth moves");

    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW + TALK_VOICE_HOLD_MS);
    CHECK(strstr(buf, "\"talkers\":\"200,0,0\"") != NULL, "exactly at the hold time still counts");

    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW + TALK_VOICE_HOLD_MS + 1);
    CHECK(strstr(buf, "\"talkers\":\"\"") != NULL, "stuck 'talking' flag, voice stopped -> mouth closes");

    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW - 5);
    CHECK(strstr(buf, "\"talkers\":\"200,0,0\"") != NULL, "voice block newer than the caller's clock read counts as fresh");

    talk_state_set(21, 0, 0); /* voice still fresh, but TS says stopped */
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW);
    CHECK(strstr(buf, "\"talkers\":\"\"") != NULL, "voice without status -> no mouth");

    talk_on(21);
    talk_state_clear_client(21);
    talk_state_set(21, 1, 0); /* same client ID reused by a new player: old voice tick must be gone */
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW);
    CHECK(strstr(buf, "\"talkers\":\"\"") != NULL, "clear_client forgets the voice tick (client ID reuse)");

    talk_on(21);
    talk_state_clear();
    talk_state_set(21, 1, 0);
    strcpy(buf, "{\"ok\":true}");
    talk_state_append_json(buf, sizeof(buf), 0, 0, 0, NOW);
    CHECK(strstr(buf, "\"talkers\":\"\"") != NULL, "clear() forgets all voice ticks");

    talk_state_note_voice(0, NOW);
    talk_state_note_voice(4096, NOW);
    CHECK(1, "out of range IDs in note_voice are ignored (no crash)");
}

int main(void) {
    printf("talk_state_test\n");
    test_flags();
    test_json();
    test_voice_gate();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
