#ifndef TS3_VERSION_MAP_H
#define TS3_VERSION_MAP_H

/*
 * Pure per-client version table for the CEVER exchange (header-only).
 *
 * Open addressing with LINEAR PROBING over a fixed array. Deleting from such a
 * table must not simply wipe the slot: the slot may sit in the middle of a
 * probe chain, and a wiped (EMPTY) slot makes every later entry of the chain
 * unreachable (find stops at the first EMPTY). That was the old bug: clients
 * "lost" their version ("Kein Plugin erkannt") and were inserted a second time.
 *
 * Fix: three slot states. A deleted slot becomes a TOMBSTONE — find() probes
 * THROUGH tombstones and only stops at EMPTY; insert() reuses the first
 * tombstone/empty slot of the probe path (the key is known to be absent
 * because find() just failed). ts3_version_reset() clears the whole table, so
 * tombstones never accumulate across connections.
 *
 * No locking and no Win32/TS coupling here: ts3_plugin_version.c wraps every
 * call in its own critical section; tests/adapter_lifecycle_test.c runs the
 * logic on the host.
 */

#include "sdk/include/teamspeak/public_definitions.h"   /* anyID */

#include <stddef.h>
#include <string.h>

#define VERSION_MAP_SLOTS     512
#define VERSION_TEXT_MAX      24
#define VERSION_NAME_MAX      48
#define VERSION_UID_MAX       48

typedef struct VersionClientSlot {
    char version[VERSION_TEXT_MAX];
    char displayName[VERSION_NAME_MAX];
    char uniqueId[VERSION_UID_MAX];
    unsigned char replied;
    unsigned char hasVersion;
} VersionClientSlot;

enum {
    VMAP_EMPTY = 0,   /* never used: ends a probe chain           */
    VMAP_USED  = 1,   /* holds a client                           */
    VMAP_TOMB  = 2    /* deleted: probe continues, slot reusable  */
};

typedef struct VersionMapEntry {
    anyID clientID;
    unsigned char state;
    VersionClientSlot slot;
} VersionMapEntry;

static inline int vmap_client_id_ok(anyID clientID) {
    return clientID > 0;
}

/* The USED entry of clientID, or NULL. Probes through tombstones. */
static inline VersionMapEntry* vmap_find(VersionMapEntry* map, anyID clientID) {
    if (!vmap_client_id_ok(clientID)) {
        return NULL;
    }
    const size_t start = (size_t)clientID % VERSION_MAP_SLOTS;
    for (size_t i = 0; i < VERSION_MAP_SLOTS; i++) {
        VersionMapEntry* entry = &map[(start + i) % VERSION_MAP_SLOTS];
        if (entry->state == VMAP_EMPTY) {
            return NULL;   /* end of the chain: the key is not stored */
        }
        if (entry->state == VMAP_USED && entry->clientID == clientID) {
            return entry;
        }
    }
    return NULL;
}

/* Existing slot, or (create != 0) a fresh zeroed one. NULL when the id is
   invalid, absent and !create, or the table is full. */
static inline VersionClientSlot* vmap_get(VersionMapEntry* map, anyID clientID, int create) {
    VersionMapEntry* entry = vmap_find(map, clientID);
    if (entry) {
        return &entry->slot;
    }
    if (!create || !vmap_client_id_ok(clientID)) {
        return NULL;
    }
    const size_t start = (size_t)clientID % VERSION_MAP_SLOTS;
    for (size_t i = 0; i < VERSION_MAP_SLOTS; i++) {
        entry = &map[(start + i) % VERSION_MAP_SLOTS];
        if (entry->state != VMAP_USED) {   /* EMPTY or TOMB: reuse */
            memset(entry, 0, sizeof(*entry));
            entry->clientID = clientID;
            entry->state = VMAP_USED;
            return &entry->slot;
        }
    }
    return NULL;
}

/* Delete one entry in place (safe while iterating the array by index). */
static inline void vmap_remove_entry(VersionMapEntry* entry) {
    memset(entry, 0, sizeof(*entry));
    entry->state = VMAP_TOMB;
}

static inline void vmap_remove(VersionMapEntry* map, anyID clientID) {
    VersionMapEntry* entry = vmap_find(map, clientID);
    if (entry) {
        vmap_remove_entry(entry);
    }
}

static inline void vmap_clear(VersionMapEntry* map) {
    memset(map, 0, sizeof(VersionMapEntry) * VERSION_MAP_SLOTS);
}

#endif /* TS3_VERSION_MAP_H */
