/* V8.8 — CEPOS / player-table load simulation (host, no TS client).
 *
 * Simulates the pure-core hot path that runs when many players send position
 * updates: player_table_put (CEPOS receive) + snapshot + proximity volume math
 * (audio recompute). This is the scalability baseline from plan.md Phase 5.4.
 *
 * Scenarios:
 *   [1] 200 players x 30 update rounds (~30 s @ 1 Hz) — table stays stable
 *   [2] 600 unique clients in one wave — LRU eviction, snapshot <= 512
 *   [3] Listener recompute against full snapshot (worst-case math loop)
 *
 * Exit 0 = all checks passed. Timing is printed for human baseline only.
 */

#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"

#include <windows.h> /* test shim: win32_shim_advance_ms */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

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

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* One listener at origin recomputes volume for every fresh table entry
   (models audio_recompute_all_impl inner loop, without TS/zone overhead). */
static int simulate_listener_recompute(float listenX, float listenY, float listenZ) {
    static PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);
    int inRange = 0;
    for (int i = 0; i < count; i++) {
        const float d = prox_distance(listenX, listenY, listenZ,
            players[i].x, players[i].y, players[i].z);
        const float vol = prox_volume_from_distance(d, players[i].voiceDistance, 1.0f);
        if (vol > 0.001f) {
            inRange++;
        }
    }
    return inRange;
}

static void test_200_players_sustained(void) {
    printf("[1] 200 players x 30 rounds (simulated 1 Hz CEPOS)\n");
    player_table_clear();

    const int players = 200;
    const int rounds = 30;
    const double t0 = now_seconds();

    for (int r = 0; r < rounds; r++) {
        for (int id = 1; id <= players; id++) {
            const float angle = (float)id * 0.31f + (float)r * 0.05f;
            const float x = 50.0f * (float)id / (float)players;
            const float y = 10.0f * sinf(angle);
            const float z = 10.0f * cosf(angle);
            CHECK(player_table_put((unsigned short)id, "p", x, y, z, 15.0f, NULL) == 1,
                "put succeeds for active player");
        }
        win32_shim_advance_ms(1000);
        simulate_listener_recompute(0.0f, 0.0f, 0.0f);
    }

    static PlayerEntry buf[PLAYER_TABLE_MAX_PLAYERS];
    const int snap = player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS);
    const double elapsed = now_seconds() - t0;

    CHECK(snap == players, "snapshot has all 200 players");
    printf("  INFO 200x30 puts + recomputes in %.3f s (%.0f puts/s)\n",
        elapsed, (double)(players * rounds) / elapsed);
}

static void test_600_clients_eviction(void) {
    printf("[2] 600 unique clients — LRU eviction (cap %d)\n", PLAYER_TABLE_MAX_PLAYERS);
    player_table_clear();

    int evictions = 0;
    for (int id = 1; id <= 600; id++) {
        unsigned short evicted = 0;
        CHECK(player_table_put((unsigned short)id, "flood", (float)id, 0, 0, 20.0f, &evicted) == 1,
            "put accepts client under flood");
        if (evicted != 0) {
            evictions++;
        }
    }

    static PlayerEntry buf[PLAYER_TABLE_MAX_PLAYERS];
    const int snap = player_table_snapshot(buf, PLAYER_TABLE_MAX_PLAYERS);

    CHECK(snap <= PLAYER_TABLE_MAX_PLAYERS, "snapshot within table cap");
    CHECK(snap == PLAYER_TABLE_MAX_PLAYERS, "table full after 600 inserts");
    CHECK(evictions > 0, "LRU evicted at least one slot");
    printf("  INFO evictions during 600-insert wave: %d\n", evictions);
}

static void test_worst_case_recompute_cost(void) {
    printf("[3] worst-case recompute: 512 entries, all in shout range\n");
    player_table_clear();

    for (int id = 1; id <= PLAYER_TABLE_MAX_PLAYERS; id++) {
        player_table_put((unsigned short)id, "near", (float)id * 0.1f, 0, 0, 50.0f, NULL);
    }

    const double t0 = now_seconds();
    const int inRange = simulate_listener_recompute(0.0f, 0.0f, 0.0f);
    const double elapsed = now_seconds() - t0;

    CHECK(inRange == PLAYER_TABLE_MAX_PLAYERS, "all 512 within shout range");
    printf("  INFO single full-table recompute: %.3f ms (%d audible)\n",
        elapsed * 1000.0, inRange);
}

int main(void) {
    printf("cepos_load_test\n");
    test_200_players_sustained();
    test_600_clients_eviction();
    test_worst_case_recompute_cost();

    if (g_failures != 0) {
        printf("\nFAILED (%d)\n", g_failures);
        return 1;
    }
    printf("\nALL PASSED (0 failures)\n");
    return 0;
}
