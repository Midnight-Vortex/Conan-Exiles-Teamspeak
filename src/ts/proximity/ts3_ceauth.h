#ifndef TS3_PROXIMITY_TS3_CEAUTH_H
#define TS3_PROXIMITY_TS3_CEAUTH_H

/*
 * CEAUTH — soft identity exchange (see doku/module/ce-protokoll.md).
 *
 * Wire format: plugin command "CEAUTH:<payloadVersion>;<steamID64>",
 * e.g. "CEAUTH:1;76561197960265728". The codec lives in ts3_ceauth_wire.h.
 *
 * Purpose: let peers associate a TeamSpeak client with a Conan Exiles player
 * (SteamID64) so the client info panel can show it and future features can key
 * on it. The identity is broadcast once on connect and re-sent once whenever a
 * newcomer announces theirs (first-contact reply), never periodically.
 *
 * SOFT identity only. The SteamID is self-reported and trivially spoofable —
 * display and association only, NEVER a trust / permission / anti-cheat input.
 *
 * Thread contract (mirrors CEMODE):
 *  - ts3_ceauth_signal_send_pending / ts3_ceauth_send_pending: any thread.
 *    Sets a flag and requests a wakeup; no TS API.
 *  - ts3_ceauth_flush / ts3_ceauth_on_plugin_command: TS callback thread ONLY
 *    (they call the TS API).
 *  - ts3_ceauth_clear_client / ts3_ceauth_reset / ts3_ceauth_format_peer: any
 *    thread — the peer table has its own lock and touches no TS API. The info
 *    panel is the reason: TeamSpeak may call it from its UI thread.
 */

#include "sdk/include/teamspeak/public_definitions.h"

#include <stddef.h>

/* Mark that our own identity must go out (connected / first-contact reply).
   Any thread. */
void ts3_ceauth_signal_send_pending(void);

/* 1 when a send is waiting. Any thread. */
int ts3_ceauth_send_pending(void);

/* Broadcast own identity if pending and not rate limited. TS callback ONLY. */
void ts3_ceauth_flush(void);

/* Handle an incoming plugin command. Returns 1 when it was a CEAUTH command
   (handled or ignored), 0 otherwise. TS callback thread ONLY. */
int ts3_ceauth_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID);

/* Forget one peer (left the server). Any thread. */
void ts3_ceauth_clear_client(anyID clientID);

/* Forget all peers + send state (disconnect / server switch / shutdown).
   Any thread. */
void ts3_ceauth_reset(void);

/* Write "SteamID: 7656..." for a peer into buf. Returns bytes written
   (excluding NUL), 0 when that peer's identity is unknown. Any thread. */
int ts3_ceauth_format_peer(anyID clientID, char* buf, size_t bufSize);

#endif /* TS3_PROXIMITY_TS3_CEAUTH_H */
