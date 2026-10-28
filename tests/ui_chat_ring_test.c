/* Unit test for the multi-producer deferred-chat ring (review-fix 088).
   The ring and its spin lock live in src/ui/util/ui_chat_ring.h /
   ui_spin_lock.h as header-only code with NO TS-API dependency, so the REAL
   lock + ring build and run on Linux with plain gcc + pthreads. In the DLL
   the same header backs ui_ts_chat_queue.c.

   Covers: empty/FIFO/truncation/full-drop/clear/wraparound (single thread),
   then N concurrent producers + a consumer (+ a concurrent clear) checking
   that no line is torn, duplicated or reordered per producer, and that the
   producer-side and ring-side drop counters agree.

   Build/run: bash tests/run_tests.sh   Exit 0 = all checks passed. */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ui/util/ui_chat_ring.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- single-threaded mechanics ------------------------------------------ */

static void test_empty(void) {
    printf("[1] fresh ring is empty\n");
    static UiChatRing r;
    ui_chat_ring_init(&r);
    char out[UI_CHAT_MSG_MAX];
    CHECK(ui_chat_ring_nonempty(&r) == 0, "nonempty == 0");
    CHECK(ui_chat_ring_pop(&r, out, sizeof(out)) == 0, "pop on empty returns 0");
    CHECK(ui_chat_ring_push(&r, NULL) == 0, "push(NULL) rejected");
    CHECK(ui_chat_ring_pop(&r, NULL, 10) == 0, "pop(NULL out) rejected");
    CHECK(ui_chat_ring_pop(&r, out, 0) == 0, "pop(outSize 0) rejected");
    CHECK(ui_chat_ring_dropped(&r) == 0, "dropped == 0");
}

static void test_fifo_and_wrap(void) {
    printf("[2] FIFO order, also across the wrap-around\n");
    static UiChatRing r;
    ui_chat_ring_init(&r);
    char msg[32];
    char out[UI_CHAT_MSG_MAX];
    int ordered = 1;
    int next_push = 0;
    int next_pop = 0;
    /* push 20, pop 20, repeated: head/tail wrap the 32-slot array several times */
    for (int round = 0; round < 7; round++) {
        for (int i = 0; i < 20; i++) {
            snprintf(msg, sizeof(msg), "m%d", next_push++);
            if (!ui_chat_ring_push(&r, msg)) {
                ordered = 0;
            }
        }
        for (int i = 0; i < 20; i++) {
            snprintf(msg, sizeof(msg), "m%d", next_pop++);
            if (!ui_chat_ring_pop(&r, out, sizeof(out)) || strcmp(out, msg) != 0) {
                ordered = 0;
            }
        }
    }
    CHECK(ordered, "140 lines popped in push order (wrapped 4+ times)");
    CHECK(ui_chat_ring_nonempty(&r) == 0, "empty again");
}

static void test_truncation(void) {
    printf("[3] long lines are truncated, never overflow\n");
    static UiChatRing r;
    ui_chat_ring_init(&r);
    char big[UI_CHAT_MSG_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(ui_chat_ring_push(&r, big) == 1, "oversized line accepted (truncated)");

    char out[UI_CHAT_MSG_MAX];
    memset(out, 'Z', sizeof(out));
    CHECK(ui_chat_ring_pop(&r, out, sizeof(out)) == 1, "popped");
    CHECK(strlen(out) == UI_CHAT_MSG_MAX - 1, "stored length == MSG_MAX-1");

    ui_chat_ring_push(&r, "hello world");
    char small[6];
    CHECK(ui_chat_ring_pop(&r, small, sizeof(small)) == 1, "pop into a small buffer");
    CHECK(strcmp(small, "hello") == 0, "small buffer: truncated + NUL-terminated");
}

static void test_full_drop(void) {
    printf("[4] full ring drops the NEW line and counts it\n");
    static UiChatRing r;
    ui_chat_ring_init(&r);
    char msg[16];
    int accepted = 0;
    for (int i = 0; i < UI_CHAT_RING_SLOTS; i++) {
        snprintf(msg, sizeof(msg), "k%d", i);
        accepted += ui_chat_ring_push(&r, msg);
    }
    CHECK(accepted == UI_CHAT_RING_SLOTS, "all 32 slots usable");
    CHECK(ui_chat_ring_push(&r, "overflow") == 0, "33rd push rejected");
    CHECK(ui_chat_ring_push(&r, "overflow2") == 0, "34th push rejected");
    CHECK(ui_chat_ring_dropped(&r) == 2, "dropped == 2");

    char out[UI_CHAT_MSG_MAX];
    CHECK(ui_chat_ring_pop(&r, out, sizeof(out)) == 1 && strcmp(out, "k0") == 0,
        "oldest line survived (nothing overwritten)");
    CHECK(ui_chat_ring_push(&r, "again") == 1, "push works again after one pop");
}

static void test_clear(void) {
    printf("[5] clear drops everything, ring stays usable\n");
    static UiChatRing r;
    ui_chat_ring_init(&r);
    for (int i = 0; i < 10; i++) {
        ui_chat_ring_push(&r, "stale");
    }
    ui_chat_ring_clear(&r);
    char out[UI_CHAT_MSG_MAX];
    CHECK(ui_chat_ring_nonempty(&r) == 0, "empty after clear");
    CHECK(ui_chat_ring_pop(&r, out, sizeof(out)) == 0, "pop after clear returns 0");
    CHECK(ui_chat_ring_push(&r, "fresh") == 1, "push after clear");
    CHECK(ui_chat_ring_pop(&r, out, sizeof(out)) == 1 && strcmp(out, "fresh") == 0,
        "fresh line comes out");
}

/* ---- multi-producer stress ---------------------------------------------- */

#define PRODUCERS      4
#define PER_PRODUCER   30000
#define PAD_LEN        300   /* long enough that a torn copy is detectable */

typedef struct {
    int id;
    long rejected;   /* pushes the ring refused (full) */
} ProducerCtx;

static UiChatRing g_ring;
static int g_producersDone = 0; /* accessed via __atomic builtins */

/* "P<id>:<seq>:" + PAD_LEN copies of ('a' + id): any interleaving of two
   writers in one slot shows up as a mixed pad or a wrong length. */
static void make_msg(char* dst, size_t cap, int id, int seq) {
    int n = snprintf(dst, cap, "P%d:%d:", id, seq);
    for (int i = 0; i < PAD_LEN && (size_t)n + 1 < cap; i++) {
        dst[n++] = (char)('a' + id);
    }
    dst[n] = '\0';
}

static void* producer_main(void* arg) {
    ProducerCtx* ctx = (ProducerCtx*)arg;
    char msg[UI_CHAT_MSG_MAX];
    for (int seq = 0; seq < PER_PRODUCER; seq++) {
        make_msg(msg, sizeof(msg), ctx->id, seq);
        if (!ui_chat_ring_push(&g_ring, msg)) {
            ctx->rejected++;
        }
    }
    return NULL;
}

typedef struct {
    long received[PRODUCERS];
    int  last_seq[PRODUCERS];
    long corrupt;    /* torn / malformed lines */
    long reordered;  /* per-producer seq went backwards or repeated */
} ConsumerStats;

static void check_line(const char* line, ConsumerStats* st) {
    int id = -1, seq = -1, off = 0;
    if (sscanf(line, "P%d:%d:%n", &id, &seq, &off) != 2 || id < 0 || id >= PRODUCERS) {
        st->corrupt++;
        return;
    }
    const char* pad = line + off;
    if (strlen(pad) != PAD_LEN) {
        st->corrupt++;
        return;
    }
    for (int i = 0; i < PAD_LEN; i++) {
        if (pad[i] != (char)('a' + id)) {
            st->corrupt++;
            return;
        }
    }
    if (seq <= st->last_seq[id]) {
        st->reordered++;
    }
    st->last_seq[id] = seq;
    st->received[id]++;
}

static void* consumer_main(void* arg) {
    ConsumerStats* st = (ConsumerStats*)arg;
    char line[UI_CHAT_MSG_MAX];
    for (;;) {
        if (ui_chat_ring_pop(&g_ring, line, sizeof(line))) {
            check_line(line, st);
            continue;
        }
        if (__atomic_load_n(&g_producersDone, __ATOMIC_ACQUIRE)) {
            /* producers finished: one last drain, then stop */
            while (ui_chat_ring_pop(&g_ring, line, sizeof(line))) {
                check_line(line, st);
            }
            return NULL;
        }
    }
}

static int g_clearStop = 0; /* accessed via __atomic builtins */
static long g_clearCalls = 0;

static void* clearer_main(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&g_clearStop, __ATOMIC_ACQUIRE)) {
        ui_chat_ring_clear(&g_ring);
        g_clearCalls++;
    }
    return NULL;
}

static void run_stress(int withClear) {
    ui_chat_ring_init(&g_ring);
    __atomic_store_n(&g_producersDone, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_clearStop, 0, __ATOMIC_RELEASE);
    g_clearCalls = 0;

    ConsumerStats st;
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < PRODUCERS; i++) {
        st.last_seq[i] = -1;
    }
    ProducerCtx ctx[PRODUCERS];
    pthread_t prod[PRODUCERS], cons, clr;

    pthread_create(&cons, NULL, consumer_main, &st);
    if (withClear) {
        pthread_create(&clr, NULL, clearer_main, NULL);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        ctx[i].id = i;
        ctx[i].rejected = 0;
        pthread_create(&prod[i], NULL, producer_main, &ctx[i]);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(prod[i], NULL);
    }
    __atomic_store_n(&g_producersDone, 1, __ATOMIC_RELEASE);
    pthread_join(cons, NULL);
    if (withClear) {
        __atomic_store_n(&g_clearStop, 1, __ATOMIC_RELEASE);
        pthread_join(clr, NULL);
    }

    long received = 0;
    long rejected = 0;
    for (int i = 0; i < PRODUCERS; i++) {
        received += st.received[i];
        rejected += ctx[i].rejected;
    }
    const long sent = (long)PRODUCERS * PER_PRODUCER;

    CHECK(st.corrupt == 0, "no torn / malformed line");
    CHECK(st.reordered == 0, "per-producer order preserved, no duplicates");
    CHECK(ui_chat_ring_dropped(&g_ring) == rejected,
        "ring drop counter == producer-side rejections");
    if (!withClear) {
        CHECK(received + rejected == sent, "every line accepted exactly once (received + dropped == sent)");
    }
    else {
        CHECK(received + rejected <= sent, "with concurrent clear: nothing delivered twice");
        printf("       (clear calls: %ld, received: %ld of %ld)\n", g_clearCalls, received, sent);
    }
    CHECK(ui_chat_ring_nonempty(&g_ring) == 0, "ring drained at the end");
    /* indices still consistent after the storm */
    CHECK(ui_chat_ring_push(&g_ring, "after") == 1, "ring usable after the storm");
    char out[UI_CHAT_MSG_MAX];
    CHECK(ui_chat_ring_pop(&g_ring, out, sizeof(out)) == 1 && strcmp(out, "after") == 0,
        "post-storm line round-trips");
}

static void test_multi_producer(void) {
    printf("[6] %d producers x %d lines + 1 consumer\n", PRODUCERS, PER_PRODUCER);
    run_stress(0);
}

static void test_multi_producer_with_clear(void) {
    printf("[7] same, with a thread hammering clear()\n");
    run_stress(1);
}

int main(void) {
    test_empty();
    test_fifo_and_wrap();
    test_truncation();
    test_full_drop();
    test_clear();
    test_multi_producer();
    test_multi_producer_with_clear();

    if (g_failures) {
        printf("\nui_chat_ring_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("\nui_chat_ring_test: all checks passed\n");
    return 0;
}
