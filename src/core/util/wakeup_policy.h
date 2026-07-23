#ifndef CORE_UTIL_WAKEUP_POLICY_H
#define CORE_UTIL_WAKEUP_POLICY_H

#include <stdint.h>

/*
 * V8.4 — pure decision used by the single wakeup-owner thread.
 *
 * The wakeup thread coalesces many "please drain" requests (from any thread)
 * into at most one "CEDRAIN:1" plugin command per rate window. This function
 * is the whole decision, with no Win32/TS coupling, so it can be unit-tested
 * on the host (see tests/wakeup_policy_test.c):
 *
 *   - urgent requests bypass the rate limit (chat / UI feedback must not wait
 *     for the next CEPOS cycle),
 *   - otherwise a send is allowed only when at least rateMs have passed since
 *     the last send. lastMs == 0 means "never sent yet"; since nowMs is the
 *     real uptime clock (GetTickCount64, always >> rateMs) the first request
 *     after startup passes.
 *
 * nowMs / lastMs / rateMs are monotonic milliseconds (GetTickCount64 in the
 * plugin). Returns 1 when the wakeup should be sent now, 0 when it should be
 * dropped (the pending flags stay set and a later request retries).
 */
static inline int wakeup_should_send(int64_t nowMs, int64_t lastMs,
                                     int urgent, int64_t rateMs) {
    if (urgent) {
        return 1;
    }
    return (nowMs - lastMs) >= rateMs;
}

#endif /* CORE_UTIL_WAKEUP_POLICY_H */
