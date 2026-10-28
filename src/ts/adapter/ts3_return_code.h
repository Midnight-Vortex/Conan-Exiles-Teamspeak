#ifndef TS3_RETURN_CODE_H
#define TS3_RETURN_CODE_H

/*
 * Pure remembered-return-code set (header-only, no Win32/TS coupling).
 *
 * requestClientMove is asynchronous: the server answers later via
 * onServerErrorEvent(..., returnCode, ...). To tell OUR move apart from any
 * other command, ts3_adapter.c creates a plugin return code per move
 * (createReturnCode), passes it to requestClientMove and remembers it here.
 * onServerErrorEvent then asks ts3_rc_track_take(): a hit means "this result
 * belongs to one of our moves" and the code is consumed (one answer per code).
 *
 * Small ring: with the in-flight flag in channel_manage at most one move is
 * outstanding, a few spare slots cover lost answers. A code that is never
 * answered is simply overwritten by the oldest-first ring — no leak, no lock.
 *
 * Thread contract: TS callback thread ONLY (creation in the adapter, matching
 * in onServerErrorEvent — both run there). No locking by design.
 */

#include <string.h>

#define TS3_RC_TRACK_SLOTS 8
#define TS3_RC_MAX         128   /* TS return-code buffer used by the SDK sample */

typedef struct Ts3RcTracker {
    char codes[TS3_RC_TRACK_SLOTS][TS3_RC_MAX];
    int  next;   /* slot the next add() overwrites (oldest first) */
} Ts3RcTracker;

static inline void ts3_rc_track_clear(Ts3RcTracker* t) {
    memset(t, 0, sizeof(*t));
}

/* Remember a code. Returns 1 when stored, 0 for NULL/empty/too-long input
   (a truncated copy could never match, so it is rejected instead). */
static inline int ts3_rc_track_add(Ts3RcTracker* t, const char* code) {
    if (!code || !code[0] || strlen(code) >= TS3_RC_MAX) {
        return 0;
    }
    memcpy(t->codes[t->next], code, strlen(code) + 1);
    t->next = (t->next + 1) % TS3_RC_TRACK_SLOTS;
    return 1;
}

/* 1 when `code` was remembered (and forgets it), 0 otherwise. */
static inline int ts3_rc_track_take(Ts3RcTracker* t, const char* code) {
    if (!code || !code[0]) {
        return 0;
    }
    for (int i = 0; i < TS3_RC_TRACK_SLOTS; i++) {
        if (t->codes[i][0] && strcmp(t->codes[i], code) == 0) {
            t->codes[i][0] = '\0';
            return 1;
        }
    }
    return 0;
}

#endif /* TS3_RETURN_CODE_H */
