#ifndef TS_PROXIMITY_TS3_CLIENT_LIMITS_H
#define TS_PROXIMITY_TS3_CLIENT_LIMITS_H

/*
 * Upper bound for sparse per-client arrays (audio snapshots, 3D dedup, unmutes).
 * TS anyID is uint16, so the whole range 1..65535 is covered (099). With the
 * former cap of 4096 a speaker whose client ID was 4096 or higher was ignored by
 * the plugin: no position data, no distance gain - his voice played at full
 * volume for everybody in the ingame channel. ID 0 is "no client".
 */
#define TS3_MAX_CLIENT_ID 65536

static inline int ts3_client_id_valid(unsigned int clientID) {
    return clientID > 0 && clientID < TS3_MAX_CLIENT_ID;
}

#endif /* TS_PROXIMITY_TS3_CLIENT_LIMITS_H */
