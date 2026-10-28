/* Unit test for src/ts/channel/chan_move_backoff.h — the retry schedule after a
   channel move the server rejected (086).

   Header-only, pure C: no Win32, no TS API, no clock (the test passes "now").
   Build/run on Linux: bash tests/run_tests.sh (plain gcc).
   Exit code 0 = all checks passed.

   Checked:
   - schedule 5 s, 10 s, 30 s, 60 s, then flat 60 s,
   - the block lasts exactly until retryAt (boundary),
   - failures count per target; a different target starts again at 5 s and
     is not blocked by the other target's backoff,
   - reset clears everything (successful move / own channel change),
   - the failure counter saturates instead of wrapping to the 5 s step,
   - the wakeup loop is only kept alive for waits up to 10 s. */

#include "ts/channel/chan_move_backoff.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void test_schedule(void) {
    printf("[1] delay schedule 5 / 10 / 30 / 60 s, then flat\n");
    CHECK(chan_backoff_delay_ms(0) == 0u,       "no failure -> no wait");
    CHECK(chan_backoff_delay_ms(1) == 5000u,    "1st failure -> 5 s");
    CHECK(chan_backoff_delay_ms(2) == 10000u,   "2nd failure -> 10 s");
    CHECK(chan_backoff_delay_ms(3) == 30000u,   "3rd failure -> 30 s");
    CHECK(chan_backoff_delay_ms(4) == 60000u,   "4th failure -> 60 s");
    CHECK(chan_backoff_delay_ms(5) == 60000u,   "5th failure -> still 60 s (max)");
    CHECK(chan_backoff_delay_ms(1000000u) == 60000u, "huge count -> still 60 s");
}

static void test_blocks_window(void) {
    printf("[2] a failure blocks the same target until retryAt\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);

    CHECK(!chan_backoff_blocks(&b, 7, 1000), "fresh state does not block");

    const uint64_t delay = chan_backoff_on_failure(&b, 7, 100000);
    CHECK(delay == 5000u, "first failure returns 5 s");
    CHECK(chan_backoff_blocks(&b, 7, 100000),       "blocked right after the failure");
    CHECK(chan_backoff_blocks(&b, 7, 104999),       "blocked 1 ms before the end");
    CHECK(!chan_backoff_blocks(&b, 7, 105000),      "free exactly at retryAt");
    CHECK(!chan_backoff_blocks(&b, 7, 200000),      "free long after");
    CHECK(!chan_backoff_blocks(&b, 0, 100000),      "target 0 is never blocked");
}

static void test_series_same_target(void) {
    printf("[3] repeated failures to the same target escalate\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);

    uint64_t t = 1000000;
    const uint64_t expect[] = { 5000, 10000, 30000, 60000, 60000, 60000 };
    int ok = 1;
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        const uint64_t d = chan_backoff_on_failure(&b, 42, t);
        if (d != expect[i] || b.retryAtMs != t + expect[i]) {
            ok = 0;
        }
        t += d; /* the next attempt happens right at the end of the wait */
    }
    CHECK(ok, "6 failures in a row: 5/10/30/60/60/60 s, retryAt = now + delay");
}

static void test_other_target(void) {
    printf("[4] backoff is per target\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);

    chan_backoff_on_failure(&b, 10, 0);
    chan_backoff_on_failure(&b, 10, 6000);          /* 2nd -> 10 s */
    CHECK(chan_backoff_blocks(&b, 10, 7000),        "target 10 blocked");
    CHECK(!chan_backoff_blocks(&b, 20, 7000),       "target 20 not blocked by target 10");

    const uint64_t d = chan_backoff_on_failure(&b, 20, 7000);
    CHECK(d == 5000u,                               "other target starts at 5 s again");
    CHECK(chan_backoff_blocks(&b, 20, 7001),        "target 20 now blocked");
    CHECK(!chan_backoff_blocks(&b, 10, 7001),       "target 10 no longer tracked");
}

static void test_reset(void) {
    printf("[5] reset (successful move / own channel change)\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);
    chan_backoff_on_failure(&b, 5, 0);
    chan_backoff_on_failure(&b, 5, 5000);
    chan_backoff_on_failure(&b, 5, 15000);
    chan_backoff_reset(&b);

    CHECK(!chan_backoff_blocks(&b, 5, 15001), "not blocked after reset");
    CHECK(chan_backoff_on_failure(&b, 5, 20000) == 5000u, "schedule restarts at 5 s");
}

static void test_saturation(void) {
    printf("[6] failure counter does not wrap\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);
    uint64_t d = 0;
    for (int i = 0; i < 5000; i++) {
        d = chan_backoff_on_failure(&b, 9, 0);
    }
    CHECK(d == 60000u, "after thousands of failures: still 60 s, not 5 s");
    CHECK(b.failCount <= 1000u, "counter is capped");
}

static void test_loop_policy(void) {
    printf("[7] wakeup loop only for short waits\n");
    ChanMoveBackoff b;
    chan_backoff_reset(&b);
    CHECK(chan_backoff_keeps_loop(&b), "no failure -> loop ok");
    chan_backoff_on_failure(&b, 3, 0);
    CHECK(chan_backoff_keeps_loop(&b), "5 s wait -> loop ok");
    chan_backoff_on_failure(&b, 3, 0);
    CHECK(chan_backoff_keeps_loop(&b), "10 s wait -> loop ok (limit is inclusive)");
    chan_backoff_on_failure(&b, 3, 0);
    CHECK(!chan_backoff_keeps_loop(&b), "30 s wait -> no loop (retry on next event)");
    chan_backoff_on_failure(&b, 3, 0);
    CHECK(!chan_backoff_keeps_loop(&b), "60 s wait -> no loop");
}

int main(void) {
    test_schedule();
    test_blocks_window();
    test_series_same_target();
    test_other_target();
    test_reset();
    test_saturation();
    test_loop_policy();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
