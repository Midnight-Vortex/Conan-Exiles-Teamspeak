/* Unit tests for src/core/mod_file/pos_tick.h (wait-time helper of the pos
   watcher loop: wake at once on an inject, keep the 30 ms tick deadline).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\pos_tick_test.c /Fe:tests\out\pos_tick_test.exe */

#include "core/mod_file/pos_tick.h"
#include "core/util/poll_interval.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

#define TICK PLUGIN_POLL_INTERVAL_MS

static void test_wait_ms(void) {
    printf("[1] pos_wait_ms\n");
    CHECK(pos_wait_ms(1000, 1030, TICK) == 30UL, "full interval left -> 30 ms");
    CHECK(pos_wait_ms(1010, 1030, TICK) == 20UL, "20 ms left -> 20 ms");
    CHECK(pos_wait_ms(1029, 1030, TICK) == 1UL, "1 ms left -> 1 ms");
    CHECK(pos_wait_ms(1030, 1030, TICK) == 0UL, "deadline reached -> 0 (tick is due)");
    CHECK(pos_wait_ms(1031, 1030, TICK) == 0UL, "deadline passed -> 0, never negative / wrapped");
    CHECK(pos_wait_ms(5000000, 1030, TICK) == 0UL, "far past -> 0");
    CHECK(pos_wait_ms(1000, 1000000, TICK) == (unsigned long)TICK, "far future (clock jump) is capped at one interval");
    CHECK(pos_wait_ms(0, 30, TICK) == 30UL, "starts from tick 0");
    CHECK(pos_wait_ms(0xFFFFFFFFFFULL, 0xFFFFFFFFFFULL + 12, TICK) == 12UL, "64-bit tick values (uptime > 49 days)");
}

/* Simulation of the watcher loop with a fake clock: the loop of pos_watcher_thread
   (wait for min(remaining tick time, next inject), run the inject notify, run
   the tick when due) with the game posting every `injectGapMs`. Counts how many
   ticks and notifies ran in `runMs`. */
static void simulate(unsigned long long runMs, unsigned long long injectGapMs,
    unsigned long* ticks, unsigned long* notifies) {
    unsigned long long now = 0;
    unsigned long long nextTick = now + TICK;
    unsigned long long nextInject = injectGapMs ? injectGapMs : (unsigned long long)-1;

    *ticks = 0;
    *notifies = 0;

    while (now < runMs) {
        const unsigned long wait = pos_wait_ms(now, nextTick, TICK);
        /* the inject is the earlier event when it falls inside the wait */
        if (nextInject - now < wait) {
            now = nextInject;
            nextInject += injectGapMs;
            (*notifies)++; /* the watcher wakes at once and runs the notify */
            if (now < nextTick) {
                continue;
            }
        }
        else {
            now += wait;
        }
        (*ticks)++;
        nextTick = now + TICK;
    }
}

static void test_cadence(void) {
    printf("[2] watcher cadence with injects (fake clock)\n");
    unsigned long ticks, notifies;

    simulate(3000, 0, &ticks, &notifies);
    CHECK(ticks == 100UL && notifies == 0UL, "no injects: one tick per 30 ms (100 in 3 s)");

    simulate(3000, 30, &ticks, &notifies);
    CHECK(ticks >= 99UL && ticks <= 100UL, "inject every 30 ms: tick cadence unchanged");
    CHECK(notifies >= 99UL, "every inject wakes the watcher (no waiting for the next tick)");

    simulate(3000, 7, &ticks, &notifies);
    CHECK(ticks >= 99UL && ticks <= 100UL, "inject every 7 ms (faster than the tick): tick does NOT run more often");

    simulate(3000, 1, &ticks, &notifies);
    CHECK(ticks >= 99UL && ticks <= 100UL, "inject every 1 ms: still ~one tick per 30 ms");
    CHECK(notifies >= 2990UL, "...and every inject wakes the watcher");

    simulate(3000, 3000, &ticks, &notifies);
    CHECK(ticks >= 99UL && ticks <= 100UL && notifies == 0UL, "a single late inject does not disturb the ticks");
}

int main(void) {
    printf("pos_tick_test\n");
    test_wait_ms();
    test_cadence();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
