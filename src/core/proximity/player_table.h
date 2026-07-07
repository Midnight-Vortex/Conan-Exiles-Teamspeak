#ifndef CORE_PROXIMITY_PLAYER_TABLE_H
#define CORE_PROXIMITY_PLAYER_TABLE_H

/*
 * Table of known remote players (position, voice distance, freshness).
 *
 * Written by the TS callback thread when a CEPOS packet arrives, read by any
 * thread. Guarded by a private lock inside this module (held only for the
 * struct copy — never while calling other modules).
 *
 * Entries expire after PLAYER_TABLE_STALE_MS without an update.
 * Sized for 200+ concurrent proximity speakers (LRU eviction when full).
 */

#define PLAYER_TABLE_MAX_PLAYERS 512

/* Legacy UI overlay/cache arrays — keep in sync with proximity table size. */
#define PLAYER_UI_LEGACY_MAX   PLAYER_TABLE_MAX_PLAYERS
#define PLAYER_TABLE_STALE_MS    120000
#define PLAYER_NAME_LEN          17 /* 16 chars + NUL, matches CEPOS field */

typedef struct PlayerEntry {
    unsigned short clientID;
    char name[PLAYER_NAME_LEN];
    float x, y, z;            /* meters */
    float voiceDistance;      /* meters */
    unsigned long long lastUpdateMs;
    int valid;
} PlayerEntry;

/* Insert or update a player. Returns 1 on success, 0 when table is full.
   When a slot is reused for a different clientID, *evictedClientID is set to
   the previous occupant (optional — pass NULL to ignore). */
int player_table_put(unsigned short clientID, const char* name,
    float x, float y, float z, float voiceDistance,
    unsigned short* evictedClientID);

/* Copy one player's entry. Returns 1 when found and fresh. */
int player_table_get(unsigned short clientID, PlayerEntry* out);

/* Copy all fresh entries into caller buffer, returns count. Expires stale
   entries as a side effect. */
int player_table_snapshot(PlayerEntry* out, int maxEntries);

/* Remove one player (client left). */
void player_table_remove(unsigned short clientID);

/* Drop everything (disconnect). */
void player_table_clear(void);

#endif /* CORE_PROXIMITY_PLAYER_TABLE_H */
