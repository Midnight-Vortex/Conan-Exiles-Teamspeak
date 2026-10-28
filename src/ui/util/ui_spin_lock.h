#ifndef UI_UTIL_UI_SPIN_LOCK_H
#define UI_UTIL_UI_SPIN_LOCK_H

/*
 * Tiny header-only lock for UI-layer bookkeeping (chat ring, thread-handle
 * slots). Review-fix 088.
 *
 * Why not a CRITICAL_SECTION? A statically zero-initialised spin lock needs no
 * init call and no teardown, so there is no init-order or "deleted while in
 * use" problem at plugin load/unload — the failure class V8.8 had to clean up
 * for overlayTextLock. The protected sections are a handful of instructions
 * (index arithmetic, one memcpy, a handle swap), never a TS-API call and never
 * a blocking wait that can involve another lock holder.
 *
 * Contention policy: spin briefly, then yield the time slice, then Sleep(1) so
 * a lock holder that was pre-empted at a lower priority still makes progress
 * (a plain yield only hands the CPU to threads of EQUAL or higher priority).
 *
 * Thread contract: ui_spin_lock / ui_spin_unlock are callable from any thread.
 * NOT recursive. Never call the TS API or block on another thread while
 * holding it — except where a caller documents a bounded wait (key_watcher.c).
 *
 * Host build: the same header compiles with plain gcc on Linux (GCC __sync
 * builtins) so tests/ui_chat_ring_test.c exercises the REAL lock.
 */

#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

typedef struct UiSpinLock {
    volatile long state; /* 0 = free, 1 = held */
} UiSpinLock;

#define UI_SPIN_LOCK_INIT { 0 }

static inline int ui_spin_try_lock(UiSpinLock* l) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&l->state, 1, 0) == 0;
#else
    return __sync_val_compare_and_swap(&l->state, 0, 1) == 0;
#endif
}

static inline void ui_spin_lock(UiSpinLock* l) {
    unsigned spins = 0;
    while (!ui_spin_try_lock(l)) {
        spins++;
        if (spins < 64) {
#if defined(_WIN32)
            YieldProcessor();
#endif
        }
        else if (spins < 1000) {
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
        else {
#if defined(_WIN32)
            Sleep(1);
#else
            usleep(1000);
#endif
        }
    }
}

static inline void ui_spin_unlock(UiSpinLock* l) {
#if defined(_WIN32)
    InterlockedExchange(&l->state, 0); /* full barrier: publishes the section's writes */
#else
    __sync_lock_release(&l->state);
#endif
}

#endif /* UI_UTIL_UI_SPIN_LOCK_H */
