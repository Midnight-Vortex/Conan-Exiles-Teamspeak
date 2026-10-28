/* Unit test for wakeup_should_send() — the pure decision the single wakeup
   owner thread makes when coalescing wakeup requests (V8.4) — and for the
   CEDRAIN addressing self-check (drain_target.h, review fix: own-client
   addressing with runtime probe + fallback to the server-wide target).

   The predicate lives in src/core/util/wakeup_policy.h as a static inline so it
   can be tested here WITHOUT the Win32/TS coupling of the adapter .c.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no Win32 needed).
   Exit code 0 = all checks passed. */

#include "core/util/wakeup_policy.h"
#include "core/util/drain_target.h"

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


/* ---- CEDRAIN addressing: CLIENT mode with echo self-check + fallback ------ */

#define T0 100000   /* any "uptime" far above 0 */

/* The echo arrives: the run closes, the mode is confirmed once, CLIENT stays. */
static void test_drain_confirmed(void) {
    printf("[4] drain target: echo confirms CLIENT addressing\n");
    DrainTargetState s;
    int ev = -1;
    drain_target_reset(&s);

    CHECK(drain_target_pick(&s, T0, 0, 1, &ev) == DRAIN_TARGET_CLIENT, "first send -> CLIENT");
    CHECK(ev == 0, "no event on the first send");
    CHECK(drain_target_probe_open(&s) == 1, "probe open after a send");

    /* Own echo stamped 40 ms later; the next send sees it. */
    CHECK(drain_target_pick(&s, T0 + 100, T0 + 40, 1, &ev) == DRAIN_TARGET_CLIENT,
        "send after the echo -> still CLIENT");
    CHECK((ev & DRAIN_EVT_CONFIRMED) != 0, "CONFIRMED event reported");
    CHECK(s.confirmed == 1 && s.mode == DRAIN_TARGET_CLIENT, "confirmed, mode CLIENT");
    CHECK(s.runSends == 1, "a fresh run started with this send");

    /* CONFIRMED is one-shot per connection. */
    CHECK(drain_target_pick(&s, T0 + 300, T0 + 250, 1, &ev) == DRAIN_TARGET_CLIENT, "still CLIENT");
    CHECK((ev & DRAIN_EVT_CONFIRMED) == 0, "CONFIRMED is not repeated");
}

/* No echo at all: fall back to SERVER exactly when >=5 sends AND >=2000 ms. */
static void test_drain_fallback(void) {
    printf("[5] drain target: no echo -> fallback to SERVER\n");
    DrainTargetState s;
    int ev = 0;
    drain_target_reset(&s);

    /* 5 sends in the first 150 ms: count reached, time not -> stay CLIENT. */
    int allClient = 1;
    for (int i = 0; i < 5; i++) {
        if (drain_target_pick(&s, T0 + i * 30, 0, 1, &ev) != DRAIN_TARGET_CLIENT) {
            allClient = 0;
        }
    }
    CHECK(allClient, "5 quick sends stay CLIENT (window not elapsed)");
    CHECK(drain_target_pick(&s, T0 + 1999, 0, 1, &ev) == DRAIN_TARGET_CLIENT,
        "1999 ms after the run start -> still CLIENT");
    CHECK(ev == 0, "no event yet");

    CHECK(drain_target_pick(&s, T0 + 2000, 0, 1, &ev) == DRAIN_TARGET_SERVER,
        "2000 ms without echo -> SERVER");
    CHECK((ev & DRAIN_EVT_FALLBACK) != 0, "FALLBACK event reported");
    CHECK(s.mode == DRAIN_TARGET_SERVER, "mode is SERVER now");

    /* Stays SERVER for the rest of the connection, event is one-shot. */
    CHECK(drain_target_pick(&s, T0 + 2030, 0, 1, &ev) == DRAIN_TARGET_SERVER, "stays SERVER");
    CHECK(ev == 0, "FALLBACK is not repeated");
    CHECK(drain_target_pick(&s, T0 + 600000, T0 + 599999, 1, &ev) == DRAIN_TARGET_SERVER,
        "a late echo does not flip back within the connection");
    CHECK(drain_target_probe_open(&s) == 0, "no probe in SERVER mode");
}

/* Sparse sends (idle connection) must NOT fall back: the count rule protects
   against "no traffic" being mistaken for "no echo". */
static void test_drain_sparse_sends(void) {
    printf("[6] drain target: fewer than 5 unechoed sends never fall back\n");
    DrainTargetState s;
    int ev = 0;
    drain_target_reset(&s);

    int allClient = 1;
    for (int i = 0; i < 5; i++) {   /* one send every 10 s, no echo */
        if (drain_target_pick(&s, T0 + (int64_t)i * 10000, 0, 1, &ev) != DRAIN_TARGET_CLIENT) {
            allClient = 0;
        }
    }
    CHECK(allClient, "5 sends 10 s apart: all CLIENT (5th pick saw only 4 earlier sends)");
    CHECK(drain_target_pick(&s, T0 + 50000, 0, 1, &ev) == DRAIN_TARGET_SERVER,
        "the 6th send finally sees >=5 unechoed sends and >=2 s -> SERVER");
}

/* The wakeup thread's 250 ms watchdog: with no echo the probe completes in
   ~2 s by itself, with a healthy echo it never falls back. */
static void test_drain_watchdog_simulation(void) {
    printf("[7] drain target: watchdog simulation\n");
    DrainTargetState s;
    int ev = 0;

    /* (a) echo never arrives. The first send comes from a normal request, then
       the wakeup thread's watchdog re-sends every 250 ms while the probe is
       open (exactly the loop in ts3_wakeup_thread). */
    drain_target_reset(&s);
    int clientSends = 0;
    int64_t fallbackAt = -1;
    int64_t t = T0;
    if (drain_target_pick(&s, t, 0, 1, &ev) == DRAIN_TARGET_CLIENT) {
        clientSends++;
    }
    while (drain_target_probe_open(&s) && t < T0 + 10000) {
        t += DRAIN_PROBE_RETRY_MS;                 /* WaitForSingleObject timeout */
        drain_target_poll(&s, 0);
        if (!drain_target_probe_open(&s)) {
            break;
        }
        if (drain_target_pick(&s, t, 0, 1, &ev) == DRAIN_TARGET_CLIENT) {
            clientSends++;
        }
        else {
            fallbackAt = t - T0;
        }
    }
    CHECK(fallbackAt == 2000, "fallback exactly 2000 ms after the first send");
    CHECK(clientSends == 8, "8 CLIENT sends (every 250 ms) before the fallback");
    CHECK(drain_target_probe_open(&s) == 0, "watchdog stops after the fallback");

    /* (b) healthy echo, 80 ms round trip, one send every 30 ms for 10 s: the
       echo of the send at time s arrives at s + 80. */
    drain_target_reset(&s);
    int fellBack = 0;
    int confirmedEvents = 0;
    for (t = T0; t < T0 + 10000; t += 30) {
        int64_t lastEcho = 0;
        if (t - T0 >= 80) {
            lastEcho = T0 + ((t - T0 - 80) / 30) * 30 + 80;   /* newest arrived echo */
        }
        if (drain_target_pick(&s, t, lastEcho, 1, &ev) == DRAIN_TARGET_SERVER) {
            fellBack = 1;
        }
        if (ev & DRAIN_EVT_CONFIRMED) {
            confirmedEvents++;
        }
    }
    CHECK(!fellBack, "healthy echo: never falls back");
    CHECK(confirmedEvents == 1, "CONFIRMED reported exactly once");
}

static void test_drain_unknown_id_and_reset(void) {
    printf("[8] drain target: unknown own id, stale echo, re-probe on reset\n");
    DrainTargetState s;
    int ev = 0;
    drain_target_reset(&s);

    /* Own client id unknown -> SERVER send, probe state untouched. */
    CHECK(drain_target_pick(&s, T0, 0, 0, &ev) == DRAIN_TARGET_SERVER, "id unknown -> SERVER");
    CHECK(s.mode == DRAIN_TARGET_CLIENT && s.runOpen == 0 && s.runSends == 0,
        "no probe progress without an id");

    /* An echo older than the run start must not confirm it. */
    CHECK(drain_target_pick(&s, T0 + 1000, 0, 1, &ev) == DRAIN_TARGET_CLIENT, "send starts a run");
    CHECK(drain_target_pick(&s, T0 + 1100, T0 + 500, 1, &ev) == DRAIN_TARGET_CLIENT, "next send");
    CHECK((ev & DRAIN_EVT_CONFIRMED) == 0 && s.runSends == 2, "stale echo (before the run) ignored");

    /* Drive it into SERVER mode, then a new connection re-probes. */
    for (int i = 0; i < 6; i++) {
        drain_target_pick(&s, T0 + 1200 + i * 500, 0, 1, &ev);
    }
    CHECK(s.mode == DRAIN_TARGET_SERVER, "fell back");
    drain_target_reset(&s);
    CHECK(s.mode == DRAIN_TARGET_CLIENT && s.fellBack == 0 && s.confirmed == 0,
        "reset: CLIENT again, one-shot guards cleared");
    CHECK(drain_target_pick(&s, T0 + 9000, 0, 1, &ev) == DRAIN_TARGET_CLIENT, "re-probes CLIENT");
}

static void test_drain_accept_invoker(void) {
    printf("[9] drain target: foreign CEDRAIN filter\n");
    CHECK(drain_accept_invoker(7, 7) == 1, "own CEDRAIN accepted");
    CHECK(drain_accept_invoker(7, 9) == 0, "foreign CEDRAIN ignored");
    CHECK(drain_accept_invoker(0, 9) == 1, "own id unknown -> never drop a drain");
    CHECK(drain_accept_invoker(0, 0) == 1, "both unknown -> accepted");
    CHECK(drain_accept_invoker(7, 0) == 1, "sender not reported (0) -> accepted, never silence the plugin");

    CHECK(drain_target_echo_is_own(7, 7) == 1, "positively own -> counts as echo");
    CHECK(drain_target_echo_is_own(7, 9) == 0, "foreign is no echo");
    CHECK(drain_target_echo_is_own(7, 0) == 0, "unattributed is no echo (self-check falls back to SERVER)");
    CHECK(drain_target_echo_is_own(0, 0) == 0, "unknown own id is no echo");
}

int main(void) {
    printf("wakeup_should_send + drain target tests\n");
    test_urgent_bypasses();
    test_rate_limit();
    test_coalesce_sequence();
    test_drain_confirmed();
    test_drain_fallback();
    test_drain_sparse_sends();
    test_drain_watchdog_simulation();
    test_drain_unknown_id_and_reset();
    test_drain_accept_invoker();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
