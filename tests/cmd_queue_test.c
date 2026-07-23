/* Unit test for the typed command ring — channel (B) of the control plane
   (V8.4 finalise). The pure mechanics live in src/ts/adapter/ts3_cmd_ring.h
   as static inline functions with NO Win32/TS coupling, so they build/run on
   Linux with plain gcc. In the DLL the ring is wrapped by a CRITICAL_SECTION
   in ts3_adapter.c; here we exercise the lock-free core directly.

   Build/run: bash tests/run_tests.sh   Exit 0 = all checks passed. */

/* stddef/string first: the SDK's ts3_functions.h (pulled in via the ring
   header -> ts3_adapter.h) uses size_t without including <stddef.h> itself —
   in the DLL build <windows.h> provides it earlier. */
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "ts/adapter/ts3_cmd_ring.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static Ts3Command mk(Ts3CmdType type, uint64 a) {
    Ts3Command c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    c.u64a = a;
    return c;
}

static void test_empty(void) {
    printf("[1] fresh ring is empty\n");
    Ts3CmdRing r;
    ts3_cmd_ring_init(&r);
    Ts3Command out;
    CHECK(ts3_cmd_ring_count(&r) == 0, "count == 0");
    CHECK(ts3_cmd_ring_pop(&r, &out) == 0, "pop on empty returns 0");
    CHECK(r.dropped == 0, "dropped == 0");
}

static void test_fifo(void) {
    printf("[2] push/pop preserve FIFO order\n");
    Ts3CmdRing r;
    ts3_cmd_ring_init(&r);
    for (int i = 0; i < 5; i++) {
        Ts3Command c = mk(TS3_CMD_LOG_CHANNEL_LIST, (uint64)(100 + i));
        CHECK(ts3_cmd_ring_push(&r, &c) == 1, "push accepted");
    }
    CHECK(ts3_cmd_ring_count(&r) == 5, "count == 5 after 5 pushes");

    int ordered = 1;
    for (int i = 0; i < 5; i++) {
        Ts3Command out;
        if (!ts3_cmd_ring_pop(&r, &out) || out.u64a != (uint64)(100 + i)) {
            ordered = 0;
        }
    }
    CHECK(ordered, "popped in the order pushed");
    CHECK(ts3_cmd_ring_count(&r) == 0, "empty again after draining");
}

static void test_reject_invalid(void) {
    printf("[3] NULL / TS3_CMD_NONE are rejected, not stored\n");
    Ts3CmdRing r;
    ts3_cmd_ring_init(&r);
    Ts3Command none = mk(TS3_CMD_NONE, 1);
    CHECK(ts3_cmd_ring_push(&r, NULL) == 0, "NULL rejected");
    CHECK(ts3_cmd_ring_push(&r, &none) == 0, "TS3_CMD_NONE rejected");
    CHECK(ts3_cmd_ring_count(&r) == 0, "nothing stored");
    CHECK(r.dropped == 0, "invalid pushes are not counted as overflow drops");
}

static void test_overflow(void) {
    printf("[4] full ring drops + counts, never overwrites\n");
    Ts3CmdRing r;
    ts3_cmd_ring_init(&r);
    for (int i = 0; i < TS3_CMD_RING_CAPACITY; i++) {
        Ts3Command c = mk(TS3_CMD_LOG_CHANNEL_LIST, (uint64)i);
        ts3_cmd_ring_push(&r, &c);
    }
    CHECK(ts3_cmd_ring_count(&r) == TS3_CMD_RING_CAPACITY, "ring is full");

    Ts3Command extra = mk(TS3_CMD_LOG_CHANNEL_LIST, 9999);
    CHECK(ts3_cmd_ring_push(&r, &extra) == 0, "push on full returns 0");
    CHECK(r.dropped == 1, "one drop counted");
    ts3_cmd_ring_push(&r, &extra);
    ts3_cmd_ring_push(&r, &extra);
    CHECK(r.dropped == 3, "further drops accumulate");

    /* Oldest entry must still be the first one pushed (no overwrite). */
    Ts3Command out;
    CHECK(ts3_cmd_ring_pop(&r, &out) == 1 && out.u64a == 0,
        "head still holds the oldest command");
}

static void test_wraparound(void) {
    printf("[5] head/tail wrap correctly under sustained use\n");
    Ts3CmdRing r;
    ts3_cmd_ring_init(&r);
    uint64 expect = 0;   /* next value we should pop */
    uint64 next = 0;      /* next value to push */

    /* Prime it partly full, then churn many push/pop cycles so tail and head
       both wrap past CAPACITY several times. */
    for (int i = 0; i < 10; i++) {
        Ts3Command c = mk(TS3_CMD_LOG_CHANNEL_LIST, next++);
        ts3_cmd_ring_push(&r, &c);
    }
    int ok = 1;
    for (int cycle = 0; cycle < TS3_CMD_RING_CAPACITY * 3; cycle++) {
        Ts3Command out;
        if (!ts3_cmd_ring_pop(&r, &out) || out.u64a != expect++) {
            ok = 0;
            break;
        }
        Ts3Command c = mk(TS3_CMD_LOG_CHANNEL_LIST, next++);
        if (!ts3_cmd_ring_push(&r, &c)) {
            ok = 0;
            break;
        }
    }
    CHECK(ok, "FIFO order preserved across wraparound");
    CHECK(ts3_cmd_ring_count(&r) == 10, "steady count kept through churn");
    CHECK(r.dropped == 0, "no drops while never exceeding capacity");
}

int main(void) {
    printf("ts3_cmd_ring tests\n");
    test_empty();
    test_fifo();
    test_reject_invalid();
    test_overflow();
    test_wraparound();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
