#ifndef TS_CHANNEL_CHAN_MOVE_BACKOFF_H
#define TS_CHANNEL_CHAN_MOVE_BACKOFF_H

/*
 * 086 — retry schedule for rejected channel moves (pure, header-only).
 *
 * requestClientMove returns ERROR_ok even when the server later refuses the
 * move (wrong password, channel full, no permission): the failure arrives
 * asynchronously. Without a pause the plugin re-sent the same move every few
 * seconds forever, and every attempt showed a TS error to the user.
 *
 * This helper holds ONLY the arithmetic: how long to wait after the n-th
 * failure to the SAME target, and whether a new attempt is still blocked.
 * It has no clock, no TS API and no locks (the caller passes "now"), so it
 * is host-unit-tested (tests/chan_move_backoff_test.c).
 *
 * Schedule (failures in a row to the same target): 5 s, 10 s, 30 s, 60 s,
 * then 60 s for every further failure.
 *
 * Thread contract: none of its own — channel_manage.c uses it on the TS
 * callback thread only.
 */

#include <stdint.h>

#define CHAN_BACKOFF_STEP1_MS 5000u
#define CHAN_BACKOFF_STEP2_MS 10000u
#define CHAN_BACKOFF_STEP3_MS 30000u
#define CHAN_BACKOFF_MAX_MS   60000u

/* The CEDRAIN wakeup loop (one server round trip every ~30 ms) is only worth
   keeping alive for waits up to this long; longer waits are retried on the
   next TS event instead of polling the server for a minute. */
#define CHAN_BACKOFF_LOOP_MAX_MS 10000u

typedef struct ChanMoveBackoff {
    uint64_t target;     /* channel the failures belong to, 0 = none */
    unsigned failCount;  /* failures in a row to that target */
    uint64_t retryAtMs;  /* earliest time for the next attempt (caller's clock) */
} ChanMoveBackoff;

/* Wait after the failCount-th failure in a row (0 failures -> 0 ms). */
static inline uint64_t chan_backoff_delay_ms(unsigned failCount) {
    switch (failCount) {
    case 0:  return 0u;
    case 1:  return CHAN_BACKOFF_STEP1_MS;
    case 2:  return CHAN_BACKOFF_STEP2_MS;
    case 3:  return CHAN_BACKOFF_STEP3_MS;
    default: return CHAN_BACKOFF_MAX_MS;
    }
}

/* Forget all failures (successful move, own channel change, new connection). */
static inline void chan_backoff_reset(ChanMoveBackoff* b) {
    b->target = 0;
    b->failCount = 0;
    b->retryAtMs = 0;
}

/* A move to `target` failed at nowMs. Failures to the same target count up; a
   different target starts again at the first step. Returns the wait in ms. */
static inline uint64_t chan_backoff_on_failure(ChanMoveBackoff* b,
    uint64_t target, uint64_t nowMs) {
    if (b->target != target) {
        b->target = target;
        b->failCount = 0;
    }
    if (b->failCount < 1000u) { /* saturate: the schedule is flat from 4 on */
        b->failCount++;
    }
    const uint64_t delay = chan_backoff_delay_ms(b->failCount);
    b->retryAtMs = nowMs + delay;
    return delay;
}

/* 1 while a new attempt to `target` must still wait. Other targets and the
   "no failures" state never block. */
static inline int chan_backoff_blocks(const ChanMoveBackoff* b,
    uint64_t target, uint64_t nowMs) {
    return b->failCount != 0 && b->target == target && target != 0
        && nowMs < b->retryAtMs;
}

/* 1 when the current wait is short enough to keep the wakeup loop alive. */
static inline int chan_backoff_keeps_loop(const ChanMoveBackoff* b) {
    return chan_backoff_delay_ms(b->failCount) <= CHAN_BACKOFF_LOOP_MAX_MS;
}

#endif /* TS_CHANNEL_CHAN_MOVE_BACKOFF_H */
