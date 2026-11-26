#ifndef CORE_MOD_FILE_POS_TICK_H
#define CORE_MOD_FILE_POS_TICK_H

/*
 * Pure wait-time helper of the pos watcher loop (pos_file.c).
 *
 * The watcher waits on TWO events: "stop" and "an HTTP sample was injected".
 * An inject wakes it at once, but the file/staleness tick must still run about
 * every PLUGIN_POLL_INTERVAL_MS - no matter how many injects arrive in between
 * (the game mod posts ~33 times a second). So the loop keeps a DEADLINE for the
 * next tick and, before every wait, asks this helper how long it may still wait.
 *
 * Thread contract: header-only, no state. Callable from any thread.
 */

/* Milliseconds to wait until `deadlineMs` (same clock as `nowMs`), never more
   than `intervalMs`: 0 when the deadline is already due or past. The cap keeps
   a deadline that is far away (clock jump) from stretching one wait. */
static inline unsigned long pos_wait_ms(unsigned long long nowMs,
    unsigned long long deadlineMs, unsigned long intervalMs) {
    unsigned long long left;

    if (deadlineMs <= nowMs) {
        return 0UL;
    }
    left = deadlineMs - nowMs;
    return (left > (unsigned long long)intervalMs) ? intervalMs : (unsigned long)left;
}

#endif /* CORE_MOD_FILE_POS_TICK_H */
