#include "core/proximity/player_table.h"

#include "core/util/compat_crt.h"
#include "core/util/log.h"

#include <windows.h>
#include <string.h>

/* Private lock — copy in/out only, no calls into other modules while held. */
static CRITICAL_SECTION g_tableLock;
static INIT_ONCE g_tableLockOnce = INIT_ONCE_STATIC_INIT;
static PlayerEntry g_players[PLAYER_TABLE_MAX_PLAYERS];
static volatile long g_evictionCount = 0;

static BOOL CALLBACK table_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_tableLock);
    return TRUE;
}

static void table_lock_ensure(void) {
    InitOnceExecuteOnce(&g_tableLockOnce, table_lock_init_once, NULL, NULL);
}

static int entry_is_fresh(const PlayerEntry* e, unsigned long long now) {
    return e->valid && (now - e->lastUpdateMs) <= PLAYER_TABLE_STALE_MS;
}

int player_table_put(unsigned short clientID, const char* name,
    float x, float y, float z, float voiceDistance,
    unsigned short* evictedClientID) {
    if (clientID == 0) {
        return 0;
    }
    if (evictedClientID) {
        *evictedClientID = 0;
    }
    table_lock_ensure();

    const unsigned long long now = GetTickCount64();
    int slot = -1;
    int freeSlot = -1;

    EnterCriticalSection(&g_tableLock);
    for (int i = 0; i < PLAYER_TABLE_MAX_PLAYERS; i++) {
        if (g_players[i].valid && g_players[i].clientID == clientID) {
            slot = i;
            break;
        }
        if (freeSlot < 0 && (!g_players[i].valid || !entry_is_fresh(&g_players[i], now))) {
            freeSlot = i;
        }
    }
    if (slot < 0) {
        slot = freeSlot;
    }

    /* Table full of fresh entries — evict the stalest slot so new speakers are
       never silently dropped (Bugbot: permanent silence when put fails). */
    if (slot < 0) {
        int oldest = -1;
        ULONGLONG oldestMs = MAXULONGLONG;
        for (int i = 0; i < PLAYER_TABLE_MAX_PLAYERS; i++) {
            if (!g_players[i].valid) {
                slot = i;
                break;
            }
            if (g_players[i].lastUpdateMs < oldestMs) {
                oldestMs = g_players[i].lastUpdateMs;
                oldest = i;
            }
        }
        if (slot < 0 && oldest >= 0) {
            slot = oldest;
        }
    }

    if (slot >= 0) {
        PlayerEntry* e = &g_players[slot];
        if (evictedClientID && e->valid && e->clientID != clientID) {
            *evictedClientID = e->clientID;
            const long evictions = InterlockedIncrement(&g_evictionCount);
            if (evictions == 1 || (evictions % 16) == 0) {
                log_debug("PLAYER-TABLE: LRU eviction #%ld (dropped client %u for %u)",
                    evictions, (unsigned)e->clientID, (unsigned)clientID);
            }
        }
        e->clientID = clientID;
        if (name && name[0]) {
            strncpy_s(e->name, PLAYER_NAME_LEN, name, _TRUNCATE);
        }
        else if (!e->valid) {
            e->name[0] = '\0';
        }
        e->x = x;
        e->y = y;
        e->z = z;
        e->voiceDistance = voiceDistance;
        e->lastUpdateMs = now;
        e->valid = 1;
    }
    LeaveCriticalSection(&g_tableLock);

    return slot >= 0;
}

int player_table_get(unsigned short clientID, PlayerEntry* out) {
    if (!out || clientID == 0) {
        return 0;
    }
    table_lock_ensure();

    const unsigned long long now = GetTickCount64();
    int found = 0;

    EnterCriticalSection(&g_tableLock);
    for (int i = 0; i < PLAYER_TABLE_MAX_PLAYERS; i++) {
        if (g_players[i].valid && g_players[i].clientID == clientID) {
            if (entry_is_fresh(&g_players[i], now)) {
                *out = g_players[i];
                found = 1;
            }
            else {
                g_players[i].valid = 0;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_tableLock);
    return found;
}

int player_table_snapshot(PlayerEntry* out, int maxEntries) {
    if (!out || maxEntries <= 0) {
        return 0;
    }
    table_lock_ensure();

    const unsigned long long now = GetTickCount64();
    int count = 0;

    EnterCriticalSection(&g_tableLock);
    for (int i = 0; i < PLAYER_TABLE_MAX_PLAYERS && count < maxEntries; i++) {
        if (!g_players[i].valid) {
            continue;
        }
        if (!entry_is_fresh(&g_players[i], now)) {
            g_players[i].valid = 0;
            continue;
        }
        out[count++] = g_players[i];
    }
    LeaveCriticalSection(&g_tableLock);
    return count;
}

void player_table_remove(unsigned short clientID) {
    table_lock_ensure();

    EnterCriticalSection(&g_tableLock);
    for (int i = 0; i < PLAYER_TABLE_MAX_PLAYERS; i++) {
        if (g_players[i].valid && g_players[i].clientID == clientID) {
            g_players[i].valid = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_tableLock);
}

void player_table_clear(void) {
    table_lock_ensure();

    EnterCriticalSection(&g_tableLock);
    memset(g_players, 0, sizeof(g_players));
    LeaveCriticalSection(&g_tableLock);
}
