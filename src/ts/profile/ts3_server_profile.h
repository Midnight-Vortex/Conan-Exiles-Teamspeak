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
 *  - server_profile_tick / _on_description_update / _on_channel_list_changed
 *    / _reset: TS callback thread ONLY.
 *  - server_profile_get / server_profile_get_local_race: any thread (private
 *    lock, struct copy).
 *  - Scalar getters (max volume, force flags, nickname randomizer, realistic
 *    audio, filter intensity, hrtf, listen bonus): any thread, lock-free
 *    (086: published as atomics when a profile is applied / reset).
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

/* 086: a channel was created / deleted / moved / edited (0 = unknown). Sets a
   dirty flag + wakeup only; the next server_profile_tick re-resolves the Root
   channel once (TS fires one event per channel on connect). The cached Root ID
   stays valid until the rescan replaced it. Callback thread ONLY. */
void server_profile_on_channel_list_changed(uint64 channelID);

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

/* 1 when the server forces distance-based muting on (0 without profile).
   086: lock-free scalar, replaces server_profile_get() copies. Any thread. */
int server_profile_force_distance_muting(void);

/* 079: root key ForceSpeakersOn (0 without profile). Any thread. */
int server_profile_force_speakers_on(void);

/* 091: root key ForceRangesInFoundations (0 without profile): every player sees
   the voice ranges in foundations while connected to this server.  Display only;
   the player's own setting is never changed by it.  Any thread, lock-free. */
int server_profile_force_ranges_in_foundations(void);

/* 1 when ingame nicknames should be replaced with random digits before the
   ingame move. Default 1 without profile (legacy behavior). Any thread. */
int server_profile_get_nickname_randomizer(void);

/* Ingame channel password into out ("" when none). 077: the password the
   game mod delivers (Conan server, "dc tsvm") wins over the root channel's
   IngameChannelPassword. Returns 2 = from the mod, 1 = from the root
   channel, 0 = none. Callback thread. */
int server_profile_copy_ingame_password(char* out, int outLen);

/* SteamID64 of the logged-in Steam user (registry, cached after first read).
   0 when Steam is not running. Any thread. */
unsigned long long server_profile_get_local_steam_id(void);

/* Race the local player belongs to (matched by SteamID during apply).
   Returns 1 and fills out when in a race, 0 otherwise. Any thread. */
int server_profile_get_local_race(HubRace* out);

/* Listener distance bonus of the local player's race (0 when not in a
   race / no profile). Any thread, lock-free. */
float server_profile_get_listen_add_distance(void);

/* 1 when the server enables Mumble-style spatial filtering (open world).
   0 without profile or when RealisticAudio=False. Any thread. */
int server_profile_get_realistic_audio(void);

/* 1 when the root channel enables the binaural HRTF (EnableHrtf=True).
   0 without profile. Any thread, lock-free (audio-thread safe). */
int server_profile_get_hrtf_enabled(void);

/* Filter strength 0..100 when realistic audio is on (100 = full path).
   Returns 100.0 without profile. Any thread. */
float server_profile_get_filter_intensity(void);

/* Drop everything (disconnect / new connection); wipes the legacy root-channel
   password from memory (086). Callback thread. */
void server_profile_reset(void);

#endif /* TS3_PROFILE_TS3_SERVER_PROFILE_H */
