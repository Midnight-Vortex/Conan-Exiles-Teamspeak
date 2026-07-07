#ifndef TS_PROXIMITY_TS3_CLIENT_LIMITS_H
#define TS_PROXIMITY_TS3_CLIENT_LIMITS_H

/*
 * Upper bound for sparse per-client arrays (audio snapshots, 3D dedup, unmutes).
 * TS anyID is uint16, but servers in practice stay well below 4096; the old
 * plugin used the same cap. IDs at or above this limit are ignored safely.
 */
#define TS3_MAX_CLIENT_ID 4096

static inline int ts3_client_id_valid(unsigned int clientID) {
    return clientID > 0 && clientID < TS3_MAX_CLIENT_ID;
}

#endif /* TS_PROXIMITY_TS3_CLIENT_LIMITS_H */
