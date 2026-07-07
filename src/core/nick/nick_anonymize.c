#include "core/nick/nick_anonymize.h"
#include "core/util/log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NICK_MIN_DIGITS   8
#define NICK_MAX_DIGITS   10
#define NICK_MAX_ATTEMPTS 32
#define NICK_MAX_CLIENTS  128

/* Callback thread only. */
static char g_savedNickname[64] = "";
static int g_anonymized = 0;

/* ---- 12.1 random digits (pure) ---------------------------------------------- */

static unsigned int nick_rand(void) {
    static volatile long s_seeded = 0;
    if (InterlockedCompareExchange(&s_seeded, 1, 0) == 0) {
        LARGE_INTEGER pc;
        QueryPerformanceCounter(&pc);
        srand((unsigned int)(GetTickCount64() ^ (ULONGLONG)pc.QuadPart ^ GetCurrentProcessId()));
    }
    return ((unsigned int)rand() << 16) ^ (unsigned int)rand();
}

void nick_make_random(char* out, int outSize, int digitCount) {
    if (digitCount < NICK_MIN_DIGITS) {
        digitCount = NICK_MIN_DIGITS;
    }
    if (digitCount > NICK_MAX_DIGITS) {
        digitCount = NICK_MAX_DIGITS;
    }
    if (!out || outSize < digitCount + 1) {
        if (out && outSize > 0) {
            out[0] = '\0';
        }
        return;
    }

    unsigned int r = nick_rand();
    out[0] = (char)('1' + (r % 9)); /* no leading zero */
    for (int i = 1; i < digitCount; i++) {
        r = r * 1103515245u + 12345u + (unsigned int)i;
        out[i] = (char)('0' + (r % 10));
    }
    out[digitCount] = '\0';
}

int nick_looks_anonymized(const char* nick) {
    if (!nick || !nick[0]) {
        return 0;
    }
    const size_t len = strlen(nick);
    if (len < NICK_MIN_DIGITS || len > NICK_MAX_DIGITS) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (nick[i] < '0' || nick[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* ---- collision check in the target channel ----------------------------------- */

static int nick_taken_in_channel(uint64 channelID, const char* candidate) {
    anyID clients[NICK_MAX_CLIENTS];
    const int count = ts3_get_channel_client_list(channelID, clients, NICK_MAX_CLIENTS);
    for (int i = 0; i < count; i++) {
        char name[64] = "";
        if (!ts3_get_client_nickname(clients[i], name, sizeof(name))) {
            continue;
        }
        if (strcmp(name, candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- 12.2 anonymize before the ingame move ------------------------------------ */

void nick_anonymize_before_ingame(uint64 ingameChannelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected() || ingameChannelID == 0) {
        return;
    }

    char current[64] = "";
    if (!ts3_get_own_nickname(current, sizeof(current))) {
        return;
    }
    if (nick_looks_anonymized(current)) {
        /* Relog case: still carrying digits from the previous round. Keep them,
           but make sure a later hub restore has a name to fall back to. */
        g_anonymized = 1;
        return;
    }
    if (g_anonymized) {
        /* Stale flag (restore failed earlier) — real name is visible again. */
        nick_reset();
    }

    snprintf(g_savedNickname, sizeof(g_savedNickname), "%s", current);

    for (int attempt = 0; attempt < NICK_MAX_ATTEMPTS; attempt++) {
        const int digits = NICK_MIN_DIGITS
            + (int)(nick_rand() % (NICK_MAX_DIGITS - NICK_MIN_DIGITS + 1));
        char candidate[16];
        nick_make_random(candidate, sizeof(candidate), digits);

        if (nick_taken_in_channel(ingameChannelID, candidate)) {
            continue;
        }
        if (ts3_set_own_nickname(candidate)) {
            g_anonymized = 1;
            log_write("NICK: pre-ingame anonymized -> %s (was '%s')", candidate, g_savedNickname);
            return;
        }
    }

    log_write("NICK: failed to assign a unique ingame nickname (keeping '%s')", g_savedNickname);
    g_savedNickname[0] = '\0';
}

/* ---- 12.3 restore in hub -------------------------------------------------------- */

void nick_restore_in_hub(void) {
    if (!ts3_thread_is_callback() || !g_anonymized) {
        return;
    }
    if (!ts3_is_connected()) {
        nick_reset();
        return;
    }
    if (g_savedNickname[0]) {
        if (ts3_set_own_nickname(g_savedNickname)) {
            log_write("NICK: hub restore -> '%s'", g_savedNickname);
        }
        else {
            log_write("NICK: hub restore of '%s' failed", g_savedNickname);
        }
    }
    nick_reset();
}

void nick_reset(void) {
    g_savedNickname[0] = '\0';
    g_anonymized = 0;
}
