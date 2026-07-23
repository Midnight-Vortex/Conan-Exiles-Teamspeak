#ifndef TS3_CMD_RING_H
#define TS3_CMD_RING_H

/*
 * Pure fixed-capacity ring buffer for the typed command queue (V8.4 finalise).
 *
 * This header holds ONLY the ring mechanics of channel (B) of the control
 * plane (see ts3_adapter.h): the head/tail/count index arithmetic and the
 * by-value copy of one Ts3Command. It has NO thread-safety of its own and NO
 * Win32/TS-API dependency, so it is unit-testable on any host with plain gcc
 * (tests/cmd_queue_test.c) — same pattern as wakeup_policy.h / render_state.
 *
 * Thread-safety is the caller's job: ts3_adapter.c owns the single producer
 * lock (a CRITICAL_SECTION) and wraps every ts3_cmd_ring_* call below inside
 * it. Overflow is NOT silently swallowed — a full ring rejects the push
 * (return 0) and bumps `dropped`; the adapter logs it (throttled).
 */

#include "ts/adapter/ts3_adapter.h"   /* Ts3Command, Ts3CmdType */

/* 256 discrete one-shot actions in flight is far more than any real burst;
   the coalescing FLAGS (channel A) carry the high-frequency state, not this. */
#define TS3_CMD_RING_CAPACITY 256

typedef struct Ts3CmdRing {
    Ts3Command slots[TS3_CMD_RING_CAPACITY];
    int  head;      /* index of the next pop  */
    int  tail;      /* index of the next push */
    int  count;     /* entries currently stored (0..CAPACITY) */
    long dropped;   /* total pushes rejected because the ring was full */
} Ts3CmdRing;

static inline void ts3_cmd_ring_init(Ts3CmdRing* r) {
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    r->dropped = 0;
}

/* Enqueue one command. Returns 1 on success, 0 when the ring is full (the
   command is dropped and `dropped` is incremented) or the command is invalid
   (NULL / TS3_CMD_NONE). Never blocks. */
static inline int ts3_cmd_ring_push(Ts3CmdRing* r, const Ts3Command* cmd) {
    if (!cmd || cmd->type == TS3_CMD_NONE) {
        return 0;
    }
    if (r->count >= TS3_CMD_RING_CAPACITY) {
        r->dropped++;
        return 0;
    }
    r->slots[r->tail] = *cmd;
    r->tail = (r->tail + 1) % TS3_CMD_RING_CAPACITY;
    r->count++;
    return 1;
}

/* Dequeue one command into *out (FIFO). Returns 1 when one was popped, 0 when
   the ring was empty. */
static inline int ts3_cmd_ring_pop(Ts3CmdRing* r, Ts3Command* out) {
    if (r->count <= 0) {
        return 0;
    }
    *out = r->slots[r->head];
    r->head = (r->head + 1) % TS3_CMD_RING_CAPACITY;
    r->count--;
    return 1;
}

/* Number of entries currently stored. */
static inline int ts3_cmd_ring_count(const Ts3CmdRing* r) {
    return r->count;
}

#endif /* TS3_CMD_RING_H */
