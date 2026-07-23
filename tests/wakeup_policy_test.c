/* Unit test for wakeup_should_send() — the pure decision the single wakeup
   owner thread makes when coalescing wakeup requests (V8.4).

   The predicate lives in src/core/util/wakeup_policy.h as a static inline so it
   can be tested here WITHOUT the Win32/TS coupling of the adapter .c.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no Win32 needed).
   Exit code 0 = all checks passed. */

#include "core/util/wakeup_policy.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* Mirror of the plugin's PLUGIN_POLL_INTERVAL_MS (30). Kept literal here so the
   test does not need the Win32 include chain of poll_interval consumers. */
#define RATE 30

static void test_urgent_bypasses(void) {
    printf("[1] urgent bypasses the rate limit\n");
    /* Urgent always sends, no matter how recent the last send was. */
    CHECK(wakeup_should_send(0, 0, 1, RATE) == 1, "urgent at t=0 -> send");
    CHECK(wakeup_should_send(1000, 999, 1, RATE) == 1, "urgent 1ms after last -> send");
    CHECK(wakeup_should_send(1000, 1000, 1, RATE) == 1, "urgent same ms -> send");
}

static void test_rate_limit(void) {
    printf("[2] non-urgent respects the rate window\n");
    /* Never sent yet (lastMs == 0): the real clock (GetTickCount64 = uptime ms)
       is always far larger than the window, so the first request passes. */
    CHECK(wakeup_should_send(100000, 0, 0, RATE) == 1, "first ever (now>>rate) -> send");

    /* Inside the window after a recent send: drop. */
    CHECK(wakeup_should_send(1000, 1000, 0, RATE) == 0, "same ms as last -> drop");
    CHECK(wakeup_should_send(1010, 1000, 0, RATE) == 0, "10ms after last -> drop");
    CHECK(wakeup_should_send(1029, 1000, 0, RATE) == 0, "29ms after last -> drop");

    /* Boundary + beyond: send. */
    CHECK(wakeup_should_send(1030, 1000, 0, RATE) == 1, "exactly 30ms after -> send");
    CHECK(wakeup_should_send(1100, 1000, 0, RATE) == 1, "100ms after -> send");
}

/* A burst of non-urgent requests inside one window collapses to a single send;
   the next window boundary lets one through again (coalescing behaviour). */
static void test_coalesce_sequence(void) {
    printf("[3] coalescing: one send per window\n");
    int64_t last = 0;      /* never sent */
    int sends = 0;

    /* Requests at t=1000,1005,...,1055 with last-send tracked as the plugin. */
    for (int64_t t = 1000; t <= 1055; t += 5) {
        if (wakeup_should_send(t, last, 0, RATE)) {
            last = t;
            sends++;
        }
    }
    /* Sends expected at t=1000 (first) and t=1030 (window elapsed) => 2 total. */
    CHECK(sends == 2, "12 requests over 55ms -> 2 sends");
}

int main(void) {
    printf("wakeup_should_send tests\n");
    test_urgent_bypasses();
    test_rate_limit();
    test_coalesce_sequence();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
