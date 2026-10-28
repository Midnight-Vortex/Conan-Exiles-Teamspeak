#include "ts/nick/nick_anonymize.h"
#include "core/util/log.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NICK_MIN_DIGITS   8
#define NICK_MAX_DIGITS   10
#define NICK_MAX_ATTEMPTS 32
#define NICK_MAX_CLIENTS  256

/* TS allows 64 CHARACTERS (TS3_MAX_SIZE_CLIENT_NICKNAME); in UTF-8 that is up
   to 4 bytes each. A 64-byte buffer cut long CJK/emoji names mid-character and
   the "restored" name was corrupt (086). */
#define NICK_BUF_LEN      (4 * 64 + 1)
#define NICK_UID_LEN      128

/* The connection a saved real name belongs to (086). Server UID where the
   adapter knows it, always the connection handler ID: two tabs (or one tab
   after a server switch) must never hand each other's name back. */
typedef struct NickBinding {
    uint64 handlerID;
    char serverUid[NICK_UID_LEN];
} NickBinding;

/* Callback thread only. */
static char g_savedNickname[NICK_BUF_LEN] = "";
static NickBinding g_savedBinding;
static char g_reuseAnonNickname[16] = "";
static int g_anonymized = 0;

static void nick_binding_current(NickBinding* out) {
    memset(out, 0, sizeof(*out));
    out->handlerID = ts3_get_active_connection();
    if (!ts3_get_server_uid(out->serverUid, (int)sizeof(out->serverUid))) {
        out->serverUid[0] = '\0';
    }
}

/* Same connection? The handler ID must match; the server UID is compared when
   both sides know it (it may be unavailable right at connect). */
static int nick_binding_matches(const NickBinding* saved, const NickBinding* now) {
    if (saved->handlerID != now->handlerID) {
        return 0;
    }
    if (saved->serverUid[0] && now->serverUid[0]
        && strcmp(saved->serverUid, now->serverUid) != 0) {
        return 0;
    }
    return 1;
}

/* Remember `realName` as the restore name of the active connection. */
static void nick_save_real_name(const char* realName) {
    snprintf(g_savedNickname, sizeof(g_savedNickname), "%s", realName);
    nick_binding_current(&g_savedBinding);
}

/* 1 when the saved name belongs to the active connection. A name saved on
   another tab / server is dropped here, never restored onto this one. */
static int nick_saved_name_usable(void) {
    if (!g_savedNickname[0]) {
        return 0;
    }
    NickBinding now;
    nick_binding_current(&now);
    if (nick_binding_matches(&g_savedBinding, &now)) {
        return 1;
    }
    log_write("NICK: saved real name belongs to another server connection - dropped");
    g_savedNickname[0] = '\0';
    memset(&g_savedBinding, 0, sizeof(g_savedBinding));
    return 0;
}

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
        char name[NICK_BUF_LEN] = "";
        if (!ts3_get_client_nickname(clients[i], name, sizeof(name))) {
            continue;
        }
        if (strcmp(name, candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- connect: remember the real name ------------------------------------------ */

void nick_on_connected(void) {
    if (!ts3_thread_is_callback()) {
        return;
    }

    char current[NICK_BUF_LEN] = "";
    if (!ts3_get_own_nickname(current, sizeof(current))) {
        return;
    }
    if (!nick_looks_anonymized(current)) {
        nick_save_real_name(current);
        g_anonymized = 0;
        g_reuseAnonNickname[0] = '\0';
        return;
    }

    /* Relog with digits still set — keep the saved real name from before, but
       only when it was saved on THIS connection (a tab switch must not bring
       back the name of another server). */
    g_anonymized = 1;
    if (!nick_saved_name_usable()) {
        log_write("NICK: connected with anonymized name and no saved restore name");
    }
}

/* ---- 12.2 anonymize before the ingame move ------------------------------------ */

void nick_anonymize_before_ingame(uint64 ingameChannelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected() || ingameChannelID == 0) {
        return;
    }
    if (!server_profile_get_nickname_randomizer()) {
        return;
    }

    char current[NICK_BUF_LEN] = "";
    if (!ts3_get_own_nickname(current, sizeof(current))) {
        return;
    }
    if (nick_looks_anonymized(current)) {
        /* Relog case: still carrying digits from the previous round. Keep
           them; the hub restore falls back to the connect-time name saved
           by nick_on_connected (when one was available for this connection). */
        if (!nick_saved_name_usable()) {
            log_write("NICK: already anonymized and no saved name - hub restore unavailable");
        }
        g_anonymized = 1;
        return;
    }
    if (g_anonymized) {
        /* Stale flag (restore failed earlier) — real name is visible again. */
        g_anonymized = 0;
    }

    nick_save_real_name(current);

    /* 086: the real name is never written to the log (privacy) - only the
       anonymized value and how many bytes were saved. */
    if (g_reuseAnonNickname[0]
        && !nick_taken_in_channel(ingameChannelID, g_reuseAnonNickname)
        && ts3_set_own_nickname(g_reuseAnonNickname)) {
        g_anonymized = 1;
        log_write("NICK: reusing anonymized nick -> %s (real name saved, %u bytes)",
            g_reuseAnonNickname, (unsigned)strlen(g_savedNickname));
        return;
    }

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
            snprintf(g_reuseAnonNickname, sizeof(g_reuseAnonNickname), "%s", candidate);
            log_write("NICK: pre-ingame anonymized -> %s (real name saved, %u bytes)",
                candidate, (unsigned)strlen(g_savedNickname));
            return;
        }
    }

    log_write("NICK: failed to assign a unique ingame nickname (keeping the real name)");
    g_savedNickname[0] = '\0';
    memset(&g_savedBinding, 0, sizeof(g_savedBinding));
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
    if (!nick_saved_name_usable()) {
        nick_reset(); /* nothing saved, or saved on another connection */
        return;
    }
    if (ts3_set_own_nickname(g_savedNickname)) {
        log_write("NICK: hub restore done (real name back, %u bytes)",
            (unsigned)strlen(g_savedNickname));
        nick_reset();
    }
    else {
        /* Keep g_anonymized + g_savedNickname so the next hub move retries. */
        log_write("NICK: hub restore failed - will retry");
    }
}

void nick_reset(void) {
    g_savedNickname[0] = '\0';
    memset(&g_savedBinding, 0, sizeof(g_savedBinding));
    g_anonymized = 0;
}
