/* Unit test for the CEPING wire codec (src/ts/proximity/ts3_ceping_wire.h).
   Header-only and pure, so this exercises the exact code the plugin ships.
   Build/run on Linux: bash tests/run_tests.sh
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_ceping_wire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- round trip ------------------------------------------------------------ */

static void test_round_trip(void) {
    char buf[CEPING_CMD_MAX];
    unsigned long seq;

    printf("[1] round trip\n");

    CHECK(ceping_wire_format(0, buf, sizeof(buf)) == 1, "format seq 0 ok");
    CHECK(strcmp(buf, "CEPING:1;0") == 0, "seq 0 exact wire text");
    seq = 999;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse seq 0 ok");
    CHECK(seq == 0, "seq 0 survives");

    CHECK(ceping_wire_format(42, buf, sizeof(buf)) == 1, "format seq 42 ok");
    CHECK(strcmp(buf, "CEPING:1;42") == 0, "seq 42 exact wire text");
    seq = 0;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse seq 42 ok");
    CHECK(seq == 42, "seq 42 survives");

    CHECK(ceping_wire_format(4294967295UL, buf, sizeof(buf)) == 1, "format max u32 ok");
    seq = 0;
    CHECK(ceping_wire_parse(buf + CEPING_PREFIX_LEN, &seq) == 1, "parse max u32 ok");
    CHECK(seq == 4294967295UL, "max u32 survives");
}

/* ---- malformed payloads ---------------------------------------------------- */

static void test_malformed(void) {
    unsigned long seq = 123;

    printf("[2] malformed payload rejection\n");

    CHECK(ceping_wire_parse("", &seq) == 0, "empty string");
    CHECK(ceping_wire_parse("1", &seq) == 0, "version only");
    CHECK(ceping_wire_parse("1;", &seq) == 0, "empty sequence field");
    CHECK(ceping_wire_parse("x;5", &seq) == 0, "non-numeric version");
    CHECK(ceping_wire_parse("1;x", &seq) == 0, "non-numeric sequence");
    CHECK(ceping_wire_parse("1;5x", &seq) == 0, "trailing junk on sequence");
    CHECK(ceping_wire_parse("0;5", &seq) == 0, "payload version 0 rejected");
    CHECK(ceping_wire_parse("2;5", &seq) == 0, "unknown future version rejected");
    CHECK(ceping_wire_parse(NULL, &seq) == 0, "NULL payload rejected");
}

/* ---- forward compatibility ------------------------------------------------- */

static void test_forward_compat(void) {
    unsigned long seq = 0;

    printf("[3] forward compatibility — additive trailing field ignored\n");

    CHECK(ceping_wire_parse("1;77;extra", &seq) == 1, "extra field parses");
    CHECK(seq == 77, "extra field: sequence intact");
}

/* ---- sequence gap (wraparound-safe) ---------------------------------------- */

static void test_seq_gap(void) {
    printf("[4] sequence gap — lost-heartbeat count\n");

    CHECK(ceping_seq_gap(10, 11) == 0, "consecutive -> 0 lost");
    CHECK(ceping_seq_gap(10, 12) == 1, "one skipped -> 1 lost");
    CHECK(ceping_seq_gap(10, 15) == 4, "four skipped -> 4 lost");
    CHECK(ceping_seq_gap(10, 10) == 0, "duplicate -> 0");
    CHECK(ceping_seq_gap(10, 9) == 0, "backward/reorder -> 0");
    CHECK(ceping_seq_gap(10, 5) == 0, "big backward -> 0");

    /* uint32 wraparound. 0xFFFFFFFE -> 0xFFFFFFFF -> 0x00000000 -> 0x00000001. */
    CHECK(ceping_seq_gap(0xFFFFFFFEUL, 0xFFFFFFFFUL) == 0, "wrap consecutive -> 0");
    CHECK(ceping_seq_gap(0xFFFFFFFFUL, 0x00000000UL) == 0, "wrap FF->0 consecutive -> 0");
    CHECK(ceping_seq_gap(0xFFFFFFFEUL, 0x00000000UL) == 1, "wrap skips FF -> 1");
    CHECK(ceping_seq_gap(0xFFFFFFFFUL, 0x00000001UL) == 1, "wrap skips 0 -> 1");
}

/* ---- buffer safety --------------------------------------------------------- */

static void test_small_buffer(void) {
    char buf[6]; /* "CEPING:1;42" needs 12 */
    char tiny[1];

    printf("[5] small buffer — returns 0, never overflows\n");

    memset(buf, 'X', sizeof(buf));
    CHECK(ceping_wire_format(42, buf, sizeof(buf)) == 0, "too-small buffer -> 0");
    CHECK(buf[0] == '\0', "too-small buffer left empty");

    tiny[0] = 'X';
    CHECK(ceping_wire_format(1, tiny, sizeof(tiny)) == 0, "1-byte buffer -> 0");
    CHECK(tiny[0] == '\0', "1-byte buffer left empty");

    CHECK(ceping_wire_format(1, NULL, 16) == 0, "NULL buffer -> 0");
}

/* ---- receive-side tracking (083) ------------------------------------------ */

/* Feed a list of sequences at the same instant; return the number of loss log
   lines and (via out) the total of heartbeats reported lost. */
static int feed(CepingTrack* t, const unsigned long* seqs, int n,
    unsigned long long nowMs, unsigned long* totalMissed) {
    int lines = 0;
    int i;

    *totalMissed = 0;
    for (i = 0; i < n; i++) {
        const CepingObserved o = ceping_track_observe(t, seqs[i], nowMs);
        if (o.log) {
            lines++;
            *totalMissed += o.missed;
        }
    }
    return lines;
}

static void test_track_basic(void) {
    CepingTrack t;
    CepingObserved o;

    printf("[6] tracking — baseline, forward steps, duplicates\n");

    memset(&t, 0, sizeof(t));
    o = ceping_track_observe(&t, 100, 5000);
    CHECK(o.missed == 0 && !o.log, "first message is the baseline, no loss");
    CHECK(t.hasSeq && t.lastSeq == 100, "baseline stored");

    o = ceping_track_observe(&t, 101, 5030);
    CHECK(o.missed == 0 && !o.log && t.lastSeq == 101, "consecutive: nothing lost, lastSeq advances");

    o = ceping_track_observe(&t, 101, 5060);
    CHECK(o.missed == 0 && !o.log && t.lastSeq == 101, "duplicate: ignored");

    o = ceping_track_observe(&t, 105, 5090);
    CHECK(o.missed == 3 && o.log && t.lastSeq == 105, "forward jump of 4: 3 lost, logged, lastSeq advances");

    CHECK(ceping_track_observe(NULL, 1, 0).log == 0, "NULL track is harmless");
}

static void test_track_reorder(void) {
    CepingTrack t;
    unsigned long seqs[] = { 10, 12, 11, 13 };
    unsigned long missed;
    CepingObserved o;

    printf("[7] tracking — reordered packets never move lastSeq backwards\n");

    memset(&t, 0, sizeof(t));
    CHECK(feed(&t, seqs, 4, 1000, &missed) == 1, "10,12,11,13: exactly one loss line (was two)");
    CHECK(missed == 1, "10,12,11,13: only the unavoidable 12 gap is reported");
    CHECK(t.lastSeq == 13, "10,12,11,13: lastSeq ends at 13");

    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 20, 0);
    (void)ceping_track_observe(&t, 22, 0);
    (void)ceping_track_observe(&t, 21, 0);
    CHECK(t.lastSeq == 22, "late 21 after 22: lastSeq stays 22");
    o = ceping_track_observe(&t, 23, 5000);
    CHECK(o.missed == 0 && !o.log, "23 after the late 21: consecutive to 22, no phantom loss");
}

static void test_track_wrap(void) {
    CepingTrack t;
    CepingObserved o;

    printf("[8] tracking — uint32 wraparound\n");

    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0xFFFFFFFEUL, 0);
    o = ceping_track_observe(&t, 0xFFFFFFFFUL, 1);
    CHECK(o.missed == 0 && t.lastSeq == 0xFFFFFFFFUL, "FFFFFFFE -> FFFFFFFF consecutive");
    o = ceping_track_observe(&t, 0x00000000UL, 2);
    CHECK(o.missed == 0 && !o.log && t.lastSeq == 0, "FFFFFFFF -> 0 consecutive (wrap), no loss");
    o = ceping_track_observe(&t, 0x00000003UL, 3);
    CHECK(o.missed == 2 && o.log && t.lastSeq == 3, "0 -> 3 after the wrap: 2 lost");

    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0xFFFFFFFFUL, 0);
    o = ceping_track_observe(&t, 0x00000001UL, 5000);
    CHECK(o.missed == 1 && o.log, "FFFFFFFF -> 1: skips 0 -> 1 lost");

    /* Reorder across the wrap: 0 arrives late after 1. */
    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0xFFFFFFFFUL, 0);
    (void)ceping_track_observe(&t, 1, 0);
    (void)ceping_track_observe(&t, 0, 0);
    CHECK(t.lastSeq == 1, "late 0 after 1 across the wrap: lastSeq stays 1");

    /* Largest forward step the algorithm accepts. */
    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0, 0);
    o = ceping_track_observe(&t, 0x7FFFFFFFUL, 5000);
    CHECK(o.missed == 0x7FFFFFFEUL && o.log, "forward step 0x7FFFFFFF is a forward step");
}

static void test_track_restart(void) {
    CepingTrack t;
    CepingObserved o;

    printf("[9] tracking — peer restart (counter back near 0) is re-based\n");

    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 5000, 0);

    o = ceping_track_observe(&t, 4990, 0);
    CHECK(o.missed == 0 && !o.log && t.lastSeq == 5000,
        "10 steps back is reordering (inside the window): ignored");
    o = ceping_track_observe(&t, 5000 - CEPING_REORDER_WINDOW, 0);
    CHECK(t.lastSeq == 5000, "exactly the window size back: still reordering");

    o = ceping_track_observe(&t, 1, 0);
    CHECK(o.missed == 0 && !o.log, "counter fell back far: no phantom loss");
    CHECK(t.lastSeq == 1, "counter fell back far: baseline re-based to 1");
    o = ceping_track_observe(&t, 2, 0);
    CHECK(o.missed == 0 && !o.log && t.lastSeq == 2, "tracking continues after the re-base");
    o = ceping_track_observe(&t, 4, 5000);
    CHECK(o.missed == 1 && o.log, "a real loss after the re-base is detected again");
}

static void test_track_throttle(void) {
    CepingTrack t;
    CepingObserved o;
    unsigned long i;
    int lines;

    printf("[10] tracking — loss log throttle (<= 1 line per peer per second)\n");

    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0, 1000);

    o = ceping_track_observe(&t, 1000, 1000);
    CHECK(o.log && o.missed == 999 && o.suppressed == 0, "first loss is logged immediately");

    lines = 0;
    for (i = 2; i <= 20; i++) { /* attacker: 19 more wild jumps in the same second */
        o = ceping_track_observe(&t, i * 1000UL, 1500);
        if (o.log) {
            lines++;
        }
    }
    CHECK(lines == 0, "19 further jumps within 1 s: no log line");
    CHECK(t.suppressed == 19, "...but all 19 are counted");

    o = ceping_track_observe(&t, 21000UL, 1999);
    CHECK(!o.log, "999 ms after the last line: still throttled");

    o = ceping_track_observe(&t, 22000UL, 2000);
    CHECK(o.log && o.suppressed == 20 && o.missed == 999, "after 1 s: one line carrying the suppressed count");
    CHECK(t.suppressed == 0, "suppressed counter reset after reporting");

    o = ceping_track_observe(&t, 23000UL, 2100);
    CHECK(!o.log && t.suppressed == 1, "throttle window restarts after each line");

    /* Clock going backwards must not wedge the log forever. */
    memset(&t, 0, sizeof(t));
    (void)ceping_track_observe(&t, 0, 90000);
    o = ceping_track_observe(&t, 10, 90000);
    CHECK(o.log, "first loss logs");
    o = ceping_track_observe(&t, 20, 100);
    CHECK(o.log, "now < lastLogMs (clock reset): allowed instead of throttled forever");

    /* A clean (cleared) peer starts fresh. */
    memset(&t, 0, sizeof(t));
    CHECK(ceping_track_observe(&t, 7, 0).log == 0 && t.hasSeq, "zeroed state = fresh peer");
}

int main(void) {
    printf("ceping wire codec tests\n");
    test_round_trip();
    test_malformed();
    test_forward_compat();
    test_seq_gap();
    test_small_buffer();
    test_track_basic();
    test_track_reorder();
    test_track_wrap();
    test_track_restart();
    test_track_throttle();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
