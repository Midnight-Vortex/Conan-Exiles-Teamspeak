/*
 * mod_password.c — ingame channel password delivered by the game mod (077).
 * See mod_password.h for the contract.
 */

#include "core/hub/mod_password.h"
#include "core/util/log.h"

#include <windows.h>
#include <string.h>

#define MOD_PASSWORD_FIELD     "TSPW="
#define MOD_PASSWORD_FIELD_LEN 5

static CRITICAL_SECTION g_pwLock;
static INIT_ONCE g_pwLockOnce = INIT_ONCE_STATIC_INIT;
static char g_pw[MOD_PASSWORD_MAX];
static unsigned long long g_pwSeenMs = 0;

static BOOL CALLBACK pw_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_pwLock);
    return TRUE;
}

static void pw_lock(void) {
    InitOnceExecuteOnce(&g_pwLockOnce, pw_lock_init_once, NULL, NULL);
    EnterCriticalSection(&g_pwLock);
}

/* Overwrite secret bytes (volatile: the compiler must not drop the stores). */
static void pw_wipe(char* p, int len) {
    volatile char* v = p;
    for (int i = 0; i < len; i++) {
        v[i] = 0;
    }
}

void mod_password_store(const char* value, int len, unsigned long long nowMs) {
    if (!value || len < 0) {
        return;
    }
    if (len >= MOD_PASSWORD_MAX) {
        return; /* longer than any TS channel password we can hand on */
    }
    for (int i = 0; i < len; i++) {
        if ((unsigned char)value[i] < 0x20 || value[i] == 0x7f) {
            return; /* control characters never belong to a password */
        }
    }

    int changed;
    pw_lock();
    changed = (int)strlen(g_pw) != len || memcmp(g_pw, value, (size_t)len) != 0;
    if (changed) {
        pw_wipe(g_pw, (int)sizeof(g_pw));
        memcpy(g_pw, value, (size_t)len);
        g_pw[len] = '\0';
    }
    g_pwSeenMs = nowMs;
    LeaveCriticalSection(&g_pwLock);

    if (changed) {
        if (len > 0) {
            log_write("PWD: ingame channel password received from the game mod (len=%d)", len);
        }
        else {
            log_write("PWD: game mod reports no ingame channel password");
        }
    }
}

int mod_password_take_from_body(char* body, int bodyLen, unsigned long long nowMs) {
    if (!body || bodyLen <= 0) {
        return bodyLen;
    }
    int avail = 0;
    while (avail < bodyLen && body[avail] != '\0') {
        avail++;
    }

    /* The field must start the body or follow a blank, so a value of another
       field that merely contains "TSPW=" never matches. First match wins:
       everything behind the marker belongs to the password. */
    int start = -1;
    for (int i = 0; i + MOD_PASSWORD_FIELD_LEN <= avail; i++) {
        if (memcmp(body + i, MOD_PASSWORD_FIELD, MOD_PASSWORD_FIELD_LEN) == 0
            && (i == 0 || body[i - 1] == ' ' || body[i - 1] == '\t')) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        return bodyLen;
    }

    const int valueAt = start + MOD_PASSWORD_FIELD_LEN;
    int valueEnd = avail;
    while (valueEnd > valueAt && (body[valueEnd - 1] == '\r' || body[valueEnd - 1] == '\n')) {
        valueEnd--;
    }
    mod_password_store(body + valueAt, valueEnd - valueAt, nowMs);

    int cut = start;
    while (cut > 0 && (body[cut - 1] == ' ' || body[cut - 1] == '\t')) {
        cut--;
    }
    pw_wipe(body + cut, avail - cut);
    return cut;
}

int mod_password_get(char* out, int outLen, unsigned long long nowMs) {
    if (!out || outLen <= 0) {
        return 0;
    }
    out[0] = '\0';
    int ok = 0;
    pw_lock();
    const int len = (int)strlen(g_pw);
    if (len > 0 && len < outLen
        && nowMs >= g_pwSeenMs && nowMs - g_pwSeenMs <= MOD_PASSWORD_TTL_MS) {
        memcpy(out, g_pw, (size_t)len + 1);
        ok = 1;
    }
    LeaveCriticalSection(&g_pwLock);
    return ok;
}

void mod_password_clear(void) {
    pw_lock();
    pw_wipe(g_pw, (int)sizeof(g_pw));
    g_pwSeenMs = 0;
    LeaveCriticalSection(&g_pwLock);
}
