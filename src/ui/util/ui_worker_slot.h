#ifndef UI_UTIL_UI_WORKER_SLOT_H
#define UI_UTIL_UI_WORKER_SLOT_H

/*
 * Exactly-once ownership hand-off for a worker thread that one thread STARTS
 * and another STOPS (review-fix 088, overlay monitor thread).
 *
 * Problem: the starter creates a thread and stores its handle; the stopper
 * reads the handle to join it. If the stopper runs between "stop requested"
 * and "handle stored" it sees NULL, joins nothing, and the worker outlives
 * the shutdown (and, in a plugin, the DLL).
 *
 * Protocol (no lock held across any thread/window creation):
 *   starter:  if (!ui_worker_may_start(s)) return;
 *             h = create worker;
 *             mine = ui_worker_publish(s, h);   // after the worker exists
 *             if (mine) { stop + join + close mine; }
 *   stopper:  mine = ui_worker_request_stop(s);
 *             if (mine) { join + close mine; }
 *
 * Each side first WRITES its own variable and then READS the other's, both
 * with sequentially consistent operations — so at least one of them sees the
 * other: either the stopper finds the published handle, or the starter sees
 * the stop request and takes the handle back itself. The handle is swapped out
 * with an atomic exchange, so exactly ONE side ever gets it back.
 *
 * Header-only, no ownership of the handle's lifetime — the caller joins/closes.
 * Thread contract: all functions are lock-free and callable from any thread.
 * Host build: GCC __atomic builtins, so tests/ui_worker_slot_test.c runs the
 * REAL protocol under pthreads.
 */

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct UiWorkerSlot {
    void* volatile handle;        /* published worker handle, NULL = none/taken */
    volatile long  stopRequested; /* 1 once ui_worker_request_stop ran */
} UiWorkerSlot;

#define UI_WORKER_SLOT_INIT { NULL, 0 }

static inline void* ui_worker_xchg_handle(UiWorkerSlot* s, void* h) {
#if defined(_WIN32)
    return InterlockedExchangePointer(&s->handle, h);
#else
    return __atomic_exchange_n(&s->handle, h, __ATOMIC_SEQ_CST);
#endif
}

static inline long ui_worker_load_stop(UiWorkerSlot* s) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&s->stopRequested, 0, 0);
#else
    return __atomic_load_n(&s->stopRequested, __ATOMIC_SEQ_CST);
#endif
}

/* 1 while no stop was requested (the starter may build the worker). */
static inline int ui_worker_may_start(UiWorkerSlot* s) {
    return ui_worker_load_stop(s) == 0;
}

/* 1 when a worker handle is currently published (not yet taken). */
static inline int ui_worker_has_handle(UiWorkerSlot* s) {
#if defined(_WIN32)
    return InterlockedCompareExchangePointer(&s->handle, NULL, NULL) != NULL;
#else
    return __atomic_load_n(&s->handle, __ATOMIC_SEQ_CST) != NULL;
#endif
}

/* Starter: publish the created worker. Returns NULL when the slot now owns it
   (a later ui_worker_request_stop hands it to the stopper). Returns the
   handle itself when a stop was requested concurrently and the stopper did
   not (or could not) take it — the caller must stop/join/close it. */
static inline void* ui_worker_publish(UiWorkerSlot* s, void* h) {
    ui_worker_xchg_handle(s, h);
    if (ui_worker_load_stop(s)) {
        return ui_worker_xchg_handle(s, NULL);
    }
    return NULL;
}

/* Stopper: request the stop and take over whatever is published. Returns the
   handle to join/close, or NULL when nothing is published (no worker, or the
   starter is mid-way and will reap its own worker after publishing). */
static inline void* ui_worker_request_stop(UiWorkerSlot* s) {
#if defined(_WIN32)
    InterlockedExchange(&s->stopRequested, 1);
#else
    __atomic_exchange_n(&s->stopRequested, 1, __ATOMIC_SEQ_CST);
#endif
    return ui_worker_xchg_handle(s, NULL);
}

#endif /* UI_UTIL_UI_WORKER_SLOT_H */
