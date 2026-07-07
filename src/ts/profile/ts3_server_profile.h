#ifndef TS3_PROFILE_TS3_SERVER_PROFILE_H
#define TS3_PROFILE_TS3_SERVER_PROFILE_H

/*
 * Phase 9 — server profile from the "Root" channel description.
 *
 * The server admin writes the plugin settings ([GLOBAL] / [ZONES], see
 * hub_parser.h) into the description of a channel named "Root". This module
 * requests that description (9.1, max 1 request in flight, rate-limited —
 * the old plugin flooded the server here), parses it via hub_parse_settings
 * (9.2) and holds the active profile (9.3).
 *
 * Thread contract:
 *  - server_profile_tick / _on_description_update / _reset:
 *    TS callback thread ONLY.
 *  - server_profile_get / getters: any thread (private lock, struct copy).
 */

#include "core/hub/hub_parser.h"
#include "sdk/include/teamspeak/public_definitions.h"

/* 9.1 driver: resolve the Root channel and request its description when due.
   Called from the CEDRAIN drain path. TS callback thread ONLY. */
void server_profile_tick(void);

/* Channel description arrived (onChannelDescriptionUpdateEvent) or
   requestChannelDescription completed (onUpdateChannelEvent).
   Fetches + parses + applies when it is the Root channel. Returns 1 when a
   profile was applied (callers refresh dependent state). Callback thread. */
int server_profile_on_description_update(uint64 channelID);

/* Root channel edited in the TS client (onUpdateChannelEditedEvent).
   Reloads and applies immediately; hotkeys are left unchanged. */
int server_profile_on_channel_edited(uint64 channelID);

/* 9.3 activate a parsed profile (also used by the update handler). */
void server_profile_apply(const HubSettings* settings);

/* Copy of the active settings. Returns 1 when a profile is active. Any thread. */
int server_profile_get(HubSettings* out);

/* 1 when the Root description was parsed and is active. Any thread. */
int server_profile_is_active(void);

/* Gain cap from the profile (1.0 without profile). Any thread. */
float server_profile_get_max_volume(void);

/* 1 when the server forces hub<->ingame auto-move. Any thread. */
int server_profile_force_auto_channel(void);

/* Ingame channel password ("" when none). Callback thread. */
const char* server_profile_get_ingame_password(void);

/* SteamID64 of the logged-in Steam user (registry, cached after first read).
   0 when Steam is not running. Any thread. */
unsigned long long server_profile_get_local_steam_id(void);

/* Race the local player belongs to (matched by SteamID during apply).
   Returns 1 and fills out when in a race, 0 otherwise. Any thread. */
int server_profile_get_local_race(HubRace* out);

/* Listener distance bonus of the local player's race (0 when not in a
   race / no profile). Any thread, lock-free. */
float server_profile_get_listen_add_distance(void);

/* Drop everything (disconnect / new connection). Callback thread. */
void server_profile_reset(void);

#endif /* TS3_PROFILE_TS3_SERVER_PROFILE_H */
