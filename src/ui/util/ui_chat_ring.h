#ifndef UI_UTIL_UI_CHAT_RING_H
#define UI_UTIL_UI_CHAT_RING_H

/*
 * Multi-producer / single-consumer ring of deferred TS chat lines
 * (review-fix 088). Header-only, no TS-API and no Win32-only dependency, so it
 * is unit-tested on any host (tests/ui_chat_ring_test.c).
 *
 * Producers (any thread: key monitor, dialog thread, pos watcher, ...) push;
 * the consumer is the TS callback thread (CEDRAIN) which pops one line at a
 * time and prints it AFTER the lock was released — the TS API is never called
 * while the ring lock is held.
 *
 * Thread contract: every function below is safe from any thread. All ring
 * state (head, tail, count, slots, dropped) is guarded by `lock`; the critical
 * sections are index arithmetic plus one bounded memcpy.
 *
 * Old behaviour (V8): head/tail were separate Interlocked indices with a
 * single-producer protocol — two producers could pick the same slot, and
 * clear() raced pushes. A full ring still drops the NEW message and reports
 * it (return 0); nothing is overwritten.
 */

#include <stddef.h>
#include <string.h>

#include "ui/util/ui_spin_lock.h"

#define UI_CHAT_RING_SLOTS 32
#define UI_CHAT_MSG_MAX    512

typedef struct UiChatRing {
    UiSpinLock lock;
    int  head;     /* index of the next pop  */
    int  tail;     /* index of the next push */
    int  count;    /* lines stored (0..UI_CHAT_RING_SLOTS) */
    long dropped;  /* pushes rejected because the ring was full */
    char slots[UI_CHAT_RING_SLOTS][UI_CHAT_MSG_MAX];
} UiChatRing;

/* A zero-initialised static ring is already valid; init is for tests/reuse. */
static inline void ui_chat_ring_init(UiChatRing* r) {
    r->lock.state = 0;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
}

/* Copy `msg` (truncated to UI_CHAT_MSG_MAX-1 bytes) into the next free slot.
   Returns 1 when stored, 0 when the ring is full (message dropped, `dropped`
   incremented) or msg is NULL. */
static inline int ui_chat_ring_push(UiChatRing* r, const char* msg) {
    if (!msg) {
        return 0;
    }
    size_t n = strlen(msg);
    if (n >= UI_CHAT_MSG_MAX) {
        n = UI_CHAT_MSG_MAX - 1;
    }
    ui_spin_lock(&r->lock);
    if (r->count >= UI_CHAT_RING_SLOTS) {
        r->dropped++;
        ui_spin_unlock(&r->lock);
        return 0;
    }
    memcpy(r->slots[r->tail], msg, n);
    r->slots[r->tail][n] = '\0';
    r->tail = (r->tail + 1) % UI_CHAT_RING_SLOTS;
    r->count++;
    ui_spin_unlock(&r->lock);
    return 1;
}

/* Pop the oldest line into out[outSize] (always NUL-terminated, truncated if
   outSize is smaller than the stored line). Returns 1 when a line was
   popped, 0 when the ring was empty. The caller prints it AFTER this returns,
   i.e. outside the lock. */
static inline int ui_chat_ring_pop(UiChatRing* r, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return 0;
    }
    ui_spin_lock(&r->lock);
    if (r->count <= 0) {
        ui_spin_unlock(&r->lock);
        return 0;
    }
    size_t n = strlen(r->slots[r->head]);
    if (n >= outSize) {
        n = outSize - 1;
    }
    memcpy(out, r->slots[r->head], n);
    out[n] = '\0';
    r->head = (r->head + 1) % UI_CHAT_RING_SLOTS;
    r->count--;
    ui_spin_unlock(&r->lock);
    return 1;
}

/* 1 when at least one line is waiting. */
static inline int ui_chat_ring_nonempty(UiChatRing* r) {
    ui_spin_lock(&r->lock);
    const int nonempty = r->count > 0;
    ui_spin_unlock(&r->lock);
    return nonempty;
}

/* Drop every waiting line (connect: stale offline chat). Safe against
   concurrent producers — they either land before the clear or after it. */
static inline void ui_chat_ring_clear(UiChatRing* r) {
    ui_spin_lock(&r->lock);
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    ui_spin_unlock(&r->lock);
}

/* Total pushes rejected because the ring was full (diagnostics/tests). */
static inline long ui_chat_ring_dropped(UiChatRing* r) {
    ui_spin_lock(&r->lock);
    const long d = r->dropped;
    ui_spin_unlock(&r->lock);
    return d;
}

#endif /* UI_UTIL_UI_CHAT_RING_H */
