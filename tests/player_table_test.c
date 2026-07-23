/* Unit tests for src/core/proximity/player_table.c.
   Build/run on Linux: bash tests/run_tests.sh — player_table.c compiles
   unchanged against the test-only Win32 shim in tests/support/win32_shim/.
   Exit code 0 = all checks passed. */

#include "core/proximity/player_table.h"

#include <windows.h> /* test shim: win32_shim_advance_ms */

#include <stdio.h>
#include <string.h>

/* player_table.c logs evictions through log_debug — stub for the host build. */
void log_debug(const char* fmt, ...) {
    (void)fmt;
}

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void test_put_get(void) {
    printf("[1] put/get roundtrip\n");
    player_table_clear();

    PlayerEntry e;
    CHECK(player_table_put(7, "Conan", 1.0f, 2.0f, 3.0f, 15.0f, NULL) == 1, "put succeeds");
    CHECK(player_table_get(7, &e) == 1, "get finds entry");
    CHECK(strcmp(e.name, "Conan") == 0, "name stored");
    CHECK(e.x == 1.0f && e.y == 2.0f && e.z == 3.0f, "position stored");
    CHECK(e.voiceDistance == 15.0f, "voice distance stored");

    /* Update in place: same clientID, new position, empty name keeps old one. */
    CHECK(player_table_put(7, "", 9.0f, 9.0f, 9.0f, 40.0f, NULL) == 1, "update succeeds");
    CHECK(player_table_get(7, &e) == 1, "get after update");
    CHECK(e.x == 9.0f && e.voiceDistance == 40.0f, "values updated");
    CHECK(strcmp(e.name, "Conan") == 0, "empty name keeps previous name");

    CHECK(player_table_get(999, &e) == 0, "unknown client not found");
}

static void test_bounds(void) {
    printf("[2] bounds / invalid input\n");
    player_table_clear();

    PlayerEntry e;
    CHECK(player_table_put(0, "x", 0, 0, 0, 5.0f, NULL) == 0, "clientID 0 rejected");
    CHECK(player_table_get(0, &e) == 0, "get clientID 0 rejected");
    CHECK(player_table_get(1, NULL) == 0, "NULL out rejected");
    CHECK(player_table_snapshot(NULL, 10) == 0, "NULL snapshot buffer rejected");
    CHECK(player_table_snapshot(&e, 0) == 0, "snapshot maxEntries 0 rejected");

    /* Long names must be truncated, not overflow (PLAYER_NAME_LEN = 17). */
    CHECK(player_table_put(5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 0, 0, 0, 5.0f, NULL) == 1,
        "oversized name accepted");
    CHECK(player_table_get(5, &e) == 1 && strlen(e.name) == PLAYER_NAME_LEN - 1,
        "name truncated to 16 chars + NUL");
}

static void test_snapshot_and_remove(void) {
    printf("[3] snapshot / remove / clear\n");
    player_table_clear();

    static PlayerEntry buf[PLAYER_TABLE_MAX_PLAYERS];
    for (unsigned short id = 1; id <= 10; id++) {
        player_table_put(id, "p", (float)id, 0, 0, 15.0f, NULL);
    }
    CHECK(player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS) == 10, "snapshot sees 10");
    CHECK(player_table_snapshot(buf, 4) == 4, "snapshot respects caller buffer size");

    player_table_remove(3);
    PlayerEntry e;
    CHECK(player_table_get(3, &e) == 0, "removed entry gone");
    CHECK(player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS) == 9, "snapshot sees 9");

    player_table_clear();
    CHECK(player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS) == 0, "clear drops all");
}

static void test_expiry(void) {
    printf("[4] expiry (PLAYER_TABLE_STALE_MS)\n");
    player_table_clear();

    PlayerEntry e;
    player_table_put(42, "old", 0, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2);
    CHECK(player_table_get(42, &e) == 1, "entry still fresh at half stale time");

    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2 + 1000);
    CHECK(player_table_get(42, &e) == 0, "entry expired after stale time");

    /* Refresh resets the clock. */
    player_table_put(43, "fresh", 0, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS - 1000);
    player_table_put(43, "fresh", 1, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS - 1000);
    CHECK(player_table_get(43, &e) == 1, "refreshed entry survives");
}

static void test_eviction(void) {
    printf("[5] LRU eviction when full\n");
    player_table_clear();

    /* Fill all slots with fresh entries; client 1 is the oldest. */
    for (int id = 1; id <= PLAYER_TABLE_MAX_PLAYERS; id++) {
        player_table_put((unsigned short)id, "p", 0, 0, 0, 15.0f, NULL);
        win32_shim_advance_ms(1);
    }

    unsigned short evicted = 0;
    CHECK(player_table_put(9999, "new", 0, 0, 0, 15.0f, &evicted) == 1,
        "put succeeds on full table (evicts)");
    CHECK(evicted == 1, "stalest entry (client 1) evicted");

    PlayerEntry e;
    CHECK(player_table_get(9999, &e) == 1, "new entry present");
    CHECK(player_table_get(1, &e) == 0, "evicted entry gone");
    CHECK(player_table_get(2, &e) == 1, "other entries untouched");
}

int main(void) {
    test_put_get();
    test_bounds();
    test_snapshot_and_remove();
    test_expiry();
    test_eviction();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
