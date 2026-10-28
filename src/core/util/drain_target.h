#ifndef CORE_UTIL_DRAIN_TARGET_H
#define CORE_UTIL_DRAIN_TARGET_H

/*
 * Pure decision logic: WHO receives the "CEDRAIN:1" plugin command.
 *
 * Background: the wakeup thread (ts3_adapter.c) sends "CEDRAIN:1" to make the
 * callback thread run the dispatcher. Sent to the whole SERVER, every client
 * relays ~33 drains/s to every other client (N^2) and every client runs the
 * foreign drains. The cheap fix is to address the command to the OWN client
 * only (PluginCommandTarget_CLIENT, targetIDs = {ownId, 0}).
 *
 * How TeamSpeak treats a plugin command addressed to the sender itself is NOT
 * verified. So the plugin probes at runtime and falls back:
 *
 *   - CLIENT mode: every send is remembered as an open "run" (count + time of
 *     the first send). The callback thread stamps `lastEchoMs` whenever it sees
 *     its OWN CEDRAIN arrive (invokerClientID == local id). An echo that is
 *     newer than the start of the run confirms the mode and closes the run.
 *   - If a run has >= DRAIN_PROBE_MIN_SENDS sends and the first one is
 *     >= DRAIN_PROBE_TIMEOUT_MS old without any echo, the mode flips to
 *     SERVER for the rest of the connection (the pre-fix behaviour, which is
 *     known to work). Drains therefore never stop permanently.
 *   - drain_target_reset() re-arms the probe (new connection / tab switch).
 *
 * No Win32 / TS coupling, so tests/wakeup_policy_test.c can run it on the host.
 * Thread contract: a DrainTargetState is touched by the wakeup thread ONLY;
 * the callback thread merely publishes lastEchoMs (an atomic the caller reads).
 */

#include <stdint.h>
#include <string.h>

#define DRAIN_TARGET_CLIENT 0       /* PluginCommandTarget_CLIENT, own client only */
#define DRAIN_TARGET_SERVER 1       /* PluginCommandTarget_SERVER, whole server    */

#define DRAIN_PROBE_MIN_SENDS   5     /* unechoed sends before a fallback is allowed */
#define DRAIN_PROBE_TIMEOUT_MS  2000  /* ... and the run must be at least this old   */
#define DRAIN_PROBE_RETRY_MS    250   /* watchdog: re-send while a probe is open     */

/* Event bits returned by drain_target_poll / drain_target_pick (one-shot logs). */
#define DRAIN_EVT_CONFIRMED 1       /* first own echo seen on this connection */
#define DRAIN_EVT_FALLBACK  2       /* switched CLIENT -> SERVER              */

typedef struct DrainTargetState {
    int     mode;        /* DRAIN_TARGET_CLIENT or DRAIN_TARGET_SERVER */
    int     runOpen;     /* CLIENT mode: sends are waiting for an echo */
    int     runSends;    /* CLIENT sends in the open run               */
    int64_t runStartMs;  /* time of the first send of the open run     */
    int     confirmed;   /* an echo was seen (one-shot log guard)      */
    int     fellBack;    /* fallback happened (one-shot log guard)     */
} DrainTargetState;

/* Start (or restart) probing: CLIENT mode, nothing outstanding. */
static inline void drain_target_reset(DrainTargetState* s) {
    memset(s, 0, sizeof(*s));
    s->mode = DRAIN_TARGET_CLIENT;
}

/* Close the open run when an own echo newer than its start has been seen.
   Returns DRAIN_EVT_* bits. lastEchoMs == 0 means "never seen". */
static inline int drain_target_poll(DrainTargetState* s, int64_t lastEchoMs) {
    int ev = 0;
    if (s->mode == DRAIN_TARGET_CLIENT && s->runOpen
        && lastEchoMs != 0 && lastEchoMs >= s->runStartMs) {
        s->runOpen = 0;
        s->runSends = 0;
        if (!s->confirmed) {
            s->confirmed = 1;
            ev |= DRAIN_EVT_CONFIRMED;
        }
    }
    return ev;
}

/* 1 while CLIENT sends are waiting for their echo (call after _poll). The
   wakeup thread uses it to arm its retry watchdog. */
static inline int drain_target_probe_open(const DrainTargetState* s) {
    return s->mode == DRAIN_TARGET_CLIENT && s->runOpen;
}

/* Decide the target for the send that is about to happen.
 *   nowMs        monotonic clock (GetTickCount64)
 *   lastEchoMs   newest own-echo time the callback thread published (0 = none)
 *   haveLocalId  0 when the own client id is unknown -> SERVER, state untouched
 *   *events      out (optional): DRAIN_EVT_* bits for one-shot logging
 * Returns DRAIN_TARGET_CLIENT or DRAIN_TARGET_SERVER. */
static inline int drain_target_pick(DrainTargetState* s, int64_t nowMs,
                                    int64_t lastEchoMs, int haveLocalId,
                                    int* events) {
    int ev = drain_target_poll(s, lastEchoMs);
    int target;

    if (!haveLocalId || s->mode == DRAIN_TARGET_SERVER) {
        target = DRAIN_TARGET_SERVER;
    }
    else if (s->runOpen && s->runSends >= DRAIN_PROBE_MIN_SENDS
             && (nowMs - s->runStartMs) >= DRAIN_PROBE_TIMEOUT_MS) {
        /* No echo for a whole probe window: CLIENT addressing does not come
           back to us on this server/client. Use the known-good target. */
        s->mode = DRAIN_TARGET_SERVER;
        s->runOpen = 0;
        s->runSends = 0;
        if (!s->fellBack) {
            s->fellBack = 1;
            ev |= DRAIN_EVT_FALLBACK;
        }
        target = DRAIN_TARGET_SERVER;
    }
    else {
        if (!s->runOpen) {
            s->runOpen = 1;
            s->runStartMs = nowMs;
            s->runSends = 0;
        }
        s->runSends++;
        target = DRAIN_TARGET_CLIENT;
    }

    if (events) {
        *events = ev;
    }
    return target;
}

/* Callback-thread side of the foreign-drain filter (onPluginCommandEvent).
 *   localId     own client id (0 = unknown)
 *   invokerId   client id that sent the CEDRAIN (0 = sender not reported)
 * Returns 1 = run the dispatcher, 0 = a drain of ANOTHER client, ignore.
 * Fail-open on purpose ("drains must never stop"): an unknown own id or an
 * unattributed sender (0 is never a real TeamSpeak client id) is accepted, so
 * a filter that cannot recognise our own drains can never silence the plugin.
 * Such an unattributed drain is NOT counted as an echo (drain_target_echo_is_own),
 * the addressing self-check then simply falls back to SERVER. */
static inline int drain_accept_invoker(unsigned localId, unsigned invokerId) {
    return localId == 0 || invokerId == 0 || invokerId == localId;
}

/* 1 when this CEDRAIN is positively our own (the echo the probe waits for). */
static inline int drain_target_echo_is_own(unsigned localId, unsigned invokerId) {
    return localId != 0 && invokerId == localId;
}

#endif /* CORE_UTIL_DRAIN_TARGET_H */
