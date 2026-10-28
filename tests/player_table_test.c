/* Unit tests for src/core/proximity/player_table.c.
   Build/run on Linux: bash tests/run_tests.sh — player_table.c compiles
   unchanged against the test-only Win32 shim in tests/support/win32_shim/.
   Exit code 0 = all checks passed. */

#define _GNU_SOURCE /* pthread_timedjoin_np (lock probe below) */

#include "core/proximity/player_table.h"

#include <windows.h> /* test shim: win32_shim_advance_ms */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* player_table.c logs evictions through log_debug. The stub doubles as a lock
   probe (review fix 084 #11: no logging inside g_tableLock): every call asks a
   helper thread to take the table lock (player_table_get). If the logging
   thread still holds g_tableLock, the helper cannot finish within the timeout. */
static int g_logCalls = 0;
static int g_logWithLockHeld = 0;

static void* lock_probe_thread(void* arg) {
    PlayerEntry e;
    (void)player_table_get(65000, &e); /* an ID never inserted: only the lock matters */
    (void)arg;
    return NULL;
}

void log_debug(const char* fmt, ...) {
    (void)fmt;
    g_logCalls++;
    pthread_t th;
    pthread_create(&th, NULL, lock_probe_thread, NULL);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 200 * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    if (pthread_timedjoin_np(th, NULL, &deadline) == ETIMEDOUT) {
        g_logWithLockHeld++;
        pthread_detach(th); /* finishes once put() leaves the lock - never join here (would deadlock) */
    }
    else {
        /* joined by pthread_timedjoin_np */
    }
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

static void test_name_on_slot_reuse(void) {
    printf("[6] slot reuse never keeps the previous occupant's name (084 #11)\n");
    player_table_clear();

    PlayerEntry e;
    unsigned short evicted = 0;

    /* Stale-but-valid slot of client 7 is taken over by client 8 with an empty name. */
    player_table_put(7, "Conan", 0, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS + 1000);
    CHECK(player_table_put(8, "", 1, 2, 3, 20.0f, &evicted) == 1, "put by the new occupant succeeds");
    CHECK(evicted == 7, "previous occupant reported as evicted");
    CHECK(player_table_get(8, &e) == 1, "new occupant found");
    CHECK(e.name[0] == '\0', "empty new name: previous occupant's name is gone");

    /* Same client ID, entry expired meanwhile, empty name: also a new life. */
    player_table_put(9, "Old", 0, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS + 1000);
    player_table_put(9, "", 5, 5, 5, 15.0f, NULL);
    CHECK(player_table_get(9, &e) == 1 && e.name[0] == '\0',
        "expired entry re-put without a name: stale name not kept");

    /* LRU eviction reuses the stalest slot: its name must not leak either. */
    player_table_clear();
    for (int id = 1; id <= PLAYER_TABLE_MAX_PLAYERS; id++) {
        player_table_put((unsigned short)id, "name", 0, 0, 0, 15.0f, NULL);
        win32_shim_advance_ms(1);
    }
    evicted = 0;
    CHECK(player_table_put(5000, "", 0, 0, 0, 15.0f, &evicted) == 1 && evicted == 1, "LRU eviction of client 1");
    CHECK(player_table_get(5000, &e) == 1 && e.name[0] == '\0', "evicted occupant's name not inherited");

    /* An ordinary in-place update still keeps the name (unchanged behavior). */
    player_table_put(5001, "Keep", 0, 0, 0, 15.0f, NULL);
    player_table_put(5001, "", 9, 9, 9, 15.0f, NULL);
    CHECK(player_table_get(5001, &e) == 1 && strcmp(e.name, "Keep") == 0,
        "fresh in-place update with empty name keeps the name");
}

static void test_log_outside_lock(void) {
    printf("[7] the eviction log is written after the table lock is released (084 #11)\n");
    player_table_clear();
    g_logCalls = 0;
    g_logWithLockHeld = 0;
    /* Evictions 1 and every 16th are logged; make plenty of them. */
    for (int id = 1; id <= PLAYER_TABLE_MAX_PLAYERS; id++) {
        player_table_put((unsigned short)id, "p", 0, 0, 0, 15.0f, NULL);
        win32_shim_advance_ms(1);
    }
    unsigned short evicted = 0;
    for (int k = 0; k < 40; k++) {
        player_table_put((unsigned short)(6000 + k), "n", 0, 0, 0, 15.0f, &evicted);
        win32_shim_advance_ms(1);
    }
    CHECK(g_logCalls >= 2, "evictions were logged");
    CHECK(g_logWithLockHeld == 0, "log_debug never ran while g_tableLock was held");
}

static void test_snapshot_drops_expired(void) {
    printf("[8] snapshot drops expired entries (basis of the audio expiry sweep)\n");
    player_table_clear();
    static PlayerEntry buf[PLAYER_TABLE_MAX_PLAYERS];
    player_table_put(1, "a", 0, 0, 0, 15.0f, NULL);
    player_table_put(2, "b", 0, 0, 0, 15.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2);
    player_table_put(2, "b", 1, 0, 0, 15.0f, NULL);   /* only b stays alive */
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2 + 1000);
    const int n = player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS);
    CHECK(n == 1 && buf[0].clientID == 2, "only the fresh entry is in the snapshot");
}

int main(void) {
    test_put_get();
    test_bounds();
    test_snapshot_and_remove();
    test_expiry();
    test_eviction();
    test_name_on_slot_reuse();
    test_log_outside_lock();
    test_snapshot_drops_expired();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
