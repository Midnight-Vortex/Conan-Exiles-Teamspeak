#ifndef TS3_PROXIMITY_TS3_3D_H
#define TS3_PROXIMITY_TS3_3D_H

/*
 * Phase 7 — TS native 3D audio (directional hearing).
 *
 * TeamSpeak places every remote client in 3D space relative to the listener.
 * The distance curve stays NEUTRAL here (ts3d_on_custom_rolloff returns 1.0)
 * because the real attenuation runs in the PCM path (Phase 6) — native 3D
 * only adds the directional component on top of the PCM equal-power pan.
 *
 * Every API call is deduplicated: settings once per connection, listener and
 * client positions only when moved beyond an epsilon (old plugin flooded the
 * API with identical 3D updates under load).
 *
 * Thread contract:
 *  - ts3d_init / ts3d_set_listener / ts3d_set_client_pos / ts3d_apply /
 *    ts3d_reset: TS callback thread ONLY (raw API goes through the adapter).
 *  - ts3d_on_custom_rolloff: TS audio thread — pure, no locks, no API.
 */

#include "sdk/include/teamspeak/public_definitions.h"

/* 7.1 systemset3DSettings once per connection (dedup flag). */
void ts3d_init(void);

/* 7.2 listener position (meters) + look direction; up vector is derived.
   Skipped when unchanged (>0.25 m position / >0.02 direction). */
void ts3d_set_listener(float x, float y, float z,
    float fwdX, float fwdY, float fwdZ);

/* 7.3 remote client position (meters), per-client epsilon dedup. */
void ts3d_set_client_pos(anyID clientID, float x, float y, float z);

/* 7.4 custom rolloff — neutral 1.0, attenuation lives in the PCM path. */
void ts3d_on_custom_rolloff(anyID clientID, float distance, float* volume);

/* Push listener + all known players to TS 3D. Callback thread ONLY. */
void ts3d_apply(void);

/* Clear all dedup state (disconnect / new connection). Callback thread. */
void ts3d_reset(void);

#endif /* TS3_PROXIMITY_TS3_3D_H */
