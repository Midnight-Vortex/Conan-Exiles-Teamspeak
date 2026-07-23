/* Unit test for render_state_needs_reinit() — the pure decision the audio
   thread makes at PCM buffer start (V8.3 PCM-owns-its-state).

   The predicate lives in src/ts/proximity/ts3_proximity_audio.h as a static
   inline so it can be tested here WITHOUT the Win32/TS coupling of the .c.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no Win32 needed).
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_proximity_audio.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void test_needs_reinit(void) {
    printf("[1] render_state_needs_reinit\n");

    /* Same generation => audio thread keeps its ramp/LPF (steady state). */
    CHECK(render_state_needs_reinit(0, 0) == 0, "equal gen (0,0) -> no reinit");
    CHECK(render_state_needs_reinit(7, 7) == 0, "equal gen (7,7) -> no reinit");

    /* Callback bumped the generation => reinit exactly once, then last-seen
       catches up and the next comparison is steady again. */
    CHECK(render_state_needs_reinit(0, 1) != 0, "first invalidate (0->1) -> reinit");
    CHECK(render_state_needs_reinit(4, 5) != 0, "next invalidate (4->5) -> reinit");

    /* Multiple bumps between two buffers still resolve to a single reinit. */
    CHECK(render_state_needs_reinit(2, 9) != 0, "batched bumps (2->9) -> reinit");

    /* Generation wrap (LONG overflow) is still a change, so still a reinit. */
    CHECK(render_state_needs_reinit(-1, 0) != 0, "wrap (-1->0) -> reinit");
}

/* Simulate the audio-thread book-keeping: reinit fires once per bump, then the
   last-seen copy is advanced so steady buffers do not keep reinitializing. */
static void test_last_seen_catchup(void) {
    printf("[2] last-seen catch-up (single reinit per invalidation)\n");
    long lastSeen = 3;      /* audio-thread private copy */
    long curGen = 3;        /* published generation */

    CHECK(render_state_needs_reinit(lastSeen, curGen) == 0, "steady -> no reinit");

    curGen = 4;             /* callback invalidates the client */
    int firstBuffer = render_state_needs_reinit(lastSeen, curGen);
    if (firstBuffer) {
        lastSeen = curGen;  /* audio thread records what it reinitialized */
    }
    CHECK(firstBuffer != 0, "buffer after invalidate -> reinit");
    CHECK(render_state_needs_reinit(lastSeen, curGen) == 0,
        "following buffer -> no repeat reinit");
}

int main(void) {
    printf("render_state_needs_reinit tests\n");
    test_needs_reinit();
    test_last_seen_catchup();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
