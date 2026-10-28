/* Unit test for the start/stop worker hand-off (review-fix 088, overlay
   monitor thread). src/ui/util/ui_worker_slot.h is header-only with no Win32
   dependency on the host, so the REAL protocol runs under pthreads here.

   Property under test, for every interleaving of one starter and one stopper:
     - a worker that was created is reaped (joined/closed) EXACTLY once,
       by either the starter or the stopper, never by both and never by none;
     - a worker that was not created is never reaped and the slot ends empty;
     - after a stop request no new worker may start.

   Build/run: bash tests/run_tests.sh   Exit 0 = all checks passed. */

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ui/util/ui_worker_slot.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- single-threaded protocol steps ------------------------------------- */

static void test_sequential(void) {
    printf("[1] sequential protocol\n");
    UiWorkerSlot s = UI_WORKER_SLOT_INIT;
    int worker;

    CHECK(ui_worker_may_start(&s) == 1, "fresh slot: may start");
    CHECK(ui_worker_has_handle(&s) == 0, "fresh slot: no handle");
    CHECK(ui_worker_publish(&s, &worker) == NULL, "publish without stop: slot owns the worker");
    CHECK(ui_worker_has_handle(&s) == 1, "handle published");
    CHECK(ui_worker_request_stop(&s) == &worker, "stop gets the published handle");
    CHECK(ui_worker_has_handle(&s) == 0, "slot empty after stop");
    CHECK(ui_worker_may_start(&s) == 0, "no start after stop");
    CHECK(ui_worker_request_stop(&s) == NULL, "second stop: nothing left to reap");

    /* stop BEFORE the starter published (the race the review found) */
    UiWorkerSlot t = UI_WORKER_SLOT_INIT;
    CHECK(ui_worker_request_stop(&t) == NULL, "stop before publish: nothing to join");
    CHECK(ui_worker_publish(&t, &worker) == &worker,
        "late publish after stop: handle handed back to the starter");
    CHECK(ui_worker_has_handle(&t) == 0, "slot stays empty");
}

/* ---- concurrent starter vs stopper -------------------------------------- */

#define ITERATIONS 20000

typedef struct {
    UiWorkerSlot slot;
    pthread_barrier_t barrier;
    int worker;             /* the "thread handle" is the address of this field */
    unsigned seed_start;
    unsigned seed_stop;
    int created;            /* starter built the worker */
    int reaped_by_start;
    int reaped_by_stop;
} Round;

static void jitter(unsigned* seed, unsigned mask) {
    /* xorshift: 0..mask spin iterations to vary the interleaving */
    unsigned x = *seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *seed = x;
    for (volatile unsigned i = 0; i < (x & mask); i++) {
    }
}

static void* starter_main(void* arg) {
    Round* r = (Round*)arg;
    pthread_barrier_wait(&r->barrier);
    jitter(&r->seed_start, 15u);
    if (!ui_worker_may_start(&r->slot)) {
        return NULL;
    }
    jitter(&r->seed_start, 255u); /* "create window + thread" takes time */
    r->created = 1;
    void* mine = ui_worker_publish(&r->slot, &r->worker);
    if (mine) {
        r->reaped_by_start++;
    }
    return NULL;
}

static void* stopper_main(void* arg) {
    Round* r = (Round*)arg;
    pthread_barrier_wait(&r->barrier);
    jitter(&r->seed_stop, 255u);
    void* mine = ui_worker_request_stop(&r->slot);
    if (mine) {
        r->reaped_by_stop++;
    }
    return NULL;
}

static void test_concurrent(void) {
    printf("[2] %d concurrent start/stop races\n", ITERATIONS);
    static Round r;
    long bad_exactly_once = 0;
    long bad_slot_not_empty = 0;
    long created_total = 0;
    long reaped_by_start_total = 0;
    long reaped_by_stop_total = 0;

    for (int it = 0; it < ITERATIONS; it++) {
        memset(&r, 0, sizeof(r));
        r.slot = (UiWorkerSlot)UI_WORKER_SLOT_INIT;
        r.seed_start = 2463534242u + (unsigned)it * 2654435761u;
        r.seed_stop = 88172645u + (unsigned)it * 40503u;
        if (r.seed_start == 0) r.seed_start = 1;
        if (r.seed_stop == 0) r.seed_stop = 1;
        pthread_barrier_init(&r.barrier, NULL, 2);

        pthread_t a, b;
        pthread_create(&a, NULL, starter_main, &r);
        pthread_create(&b, NULL, stopper_main, &r);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        pthread_barrier_destroy(&r.barrier);

        const int reaped = r.reaped_by_start + r.reaped_by_stop;
        if (r.created) {
            created_total++;
            if (reaped != 1) {
                bad_exactly_once++;
            }
        }
        else if (reaped != 0) {
            bad_exactly_once++;
        }
        if (ui_worker_has_handle(&r.slot)) {
            bad_slot_not_empty++;
        }
        reaped_by_start_total += r.reaped_by_start;
        reaped_by_stop_total += r.reaped_by_stop;
    }

    CHECK(bad_exactly_once == 0, "every created worker reaped exactly once, never lost, never twice");
    CHECK(bad_slot_not_empty == 0, "slot always empty at the end");
    printf("       (workers created: %ld; reaped by stopper: %ld, by starter: %ld)\n",
        created_total, reaped_by_stop_total, reaped_by_start_total);
}

int main(void) {
    test_sequential();
    test_concurrent();

    if (g_failures) {
        printf("\nui_worker_slot_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("\nui_worker_slot_test: all checks passed\n");
    return 0;
}
