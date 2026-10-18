/*
 * talk_state.c — talk status per TS client for the mod's mouth animation (067).
 * See talk_state.h for the thread contract.
 */

#include "core/proximity/talk_state.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static volatile long g_talk[TALK_STATE_MAX_CLIENT_ID];
static volatile long g_localTalk = 0;

/* HTTP listener thread only (talk_state_append_json). */
static PlayerEntry g_talkSnap[PLAYER_TABLE_MAX_PLAYERS];

void talk_state_set(unsigned int clientID, int talking, int isLocal) {
    const long v = talking ? 1 : 0;
    if (isLocal) {
        InterlockedExchange(&g_localTalk, v);
    }
    if (clientID > 0 && clientID < TALK_STATE_MAX_CLIENT_ID) {
        InterlockedExchange(&g_talk[clientID], v);
    }
}

int talk_state_local(void) {
    return InterlockedCompareExchange(&g_localTalk, 0, 0) != 0;
}

int talk_state_is_talking(unsigned int clientID) {
    if (clientID == 0 || clientID >= TALK_STATE_MAX_CLIENT_ID) {
        return 0;
    }
    return InterlockedCompareExchange(&g_talk[clientID], 0, 0) != 0;
}

void talk_state_clear_client(unsigned int clientID) {
    if (clientID > 0 && clientID < TALK_STATE_MAX_CLIENT_ID) {
        InterlockedExchange(&g_talk[clientID], 0);
    }
}

void talk_state_clear(void) {
    for (int i = 0; i < TALK_STATE_MAX_CLIENT_ID; i++) {
        InterlockedExchange(&g_talk[i], 0);
    }
    InterlockedExchange(&g_localTalk, 0);
}

int talk_state_append_json(char* out, int outSize,
    float localX, float localY, float localZ) {
    if (!out || outSize < 32) {
        return 0;
    }
    const int len = (int)strlen(out);
    if (len < 2 || out[len - 1] != '}') {
        return 0;
    }

    /* Nearest talking remote players inside the range (insertion list). */
    const int count = player_table_snapshot(g_talkSnap, PLAYER_TABLE_MAX_PLAYERS);
    int pick[TALK_MAX_TALKERS];
    float pickDist[TALK_MAX_TALKERS];
    int picked = 0;
    for (int i = 0; i < count; i++) {
        if (!talk_state_is_talking(g_talkSnap[i].clientID)) {
            continue;
        }
        const float d = prox_distance(localX, localY, localZ,
            g_talkSnap[i].x, g_talkSnap[i].y, g_talkSnap[i].z);
        if (!(d <= TALK_RANGE_M)) {
            continue;
        }
        int slot = picked;
        while (slot > 0 && pickDist[slot - 1] > d) {
            slot--;
        }
        if (slot >= TALK_MAX_TALKERS) {
            continue;
        }
        const int last = (picked < TALK_MAX_TALKERS) ? picked : TALK_MAX_TALKERS - 1;
        for (int k = last; k > slot; k--) {
            pick[k] = pick[k - 1];
            pickDist[k] = pickDist[k - 1];
        }
        pick[slot] = i;
        pickDist[slot] = d;
        if (picked < TALK_MAX_TALKERS) {
            picked++;
        }
    }

    char tail[TALK_MAX_TALKERS * 40 + 64];
    int n = snprintf(tail, sizeof(tail), ",\"talk\":%d,\"talkers\":\"", talk_state_local());
    for (int k = 0; k < picked && n > 0 && n < (int)sizeof(tail); k++) {
        const PlayerEntry* e = &g_talkSnap[pick[k]];
        /* Plugin is Y-up (meters); Unreal is Z-up (centimeters). */
        n += snprintf(tail + n, sizeof(tail) - (size_t)n, "%s%ld,%ld,%ld", k ? ";" : "",
            lroundf(e->x * 100.0f), lroundf(e->z * 100.0f), lroundf(e->y * 100.0f));
    }
    if (n <= 0 || n + 3 > (int)sizeof(tail)) {
        return 0;
    }
    tail[n++] = '"';
    tail[n++] = '}';
    tail[n] = '\0';

    if (len - 1 + n + 1 > outSize) {
        return 0;
    }
    memcpy(out + len - 1, tail, (size_t)n + 1); /* replaces the closing '}' */
    return len - 1 + n;
}
