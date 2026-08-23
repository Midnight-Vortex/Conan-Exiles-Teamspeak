#ifndef TS3_PROXIMITY_TS3_CEMODE_H
#define TS3_PROXIMITY_TS3_CEMODE_H

/*
 * CEMODE — voice mode edge protocol (see doku/module/ce-protokoll.md).
 *
 * Wire format: plugin command "CEMODE:<payloadVersion>;<mode>;<distanceDm>",
 * e.g. "CEMODE:1;2;600" (mode 0 = whisper, 1 = normal, 2 = shout; the distance
 * is an integer in decimeters). The codec itself lives in ts3_cemode_wire.h.
 *
 * CEPOS stays the single source of truth for position AND for the audio
 * distance; CEMODE only tells peers WHICH mode produced that distance, so the
 * TeamSpeak client info panel can show it. A missing CEMODE (old client,
 * packet loss) never changes how anything sounds.
 *
 * Thread contract:
 *  - ts3_cemode_signal_send_pending / ts3_cemode_send_pending: any thread
 *    (voice mode switch). Sets a flag and requests a wakeup; no TS API.
 *  - ts3_cemode_flush / ts3_cemode_on_plugin_command: TS callback thread ONLY
 *    (they call the TS API).
 *  - ts3_cemode_clear_client / ts3_cemode_reset / ts3_cemode_format_peer: any
 *    thread — the peer table has its own lock and no TS API is touched. The
 *    info panel is the reason: TeamSpeak may call it from its UI thread.
 */

#include "sdk/include/teamspeak/public_definitions.h"

#include <stddef.h>

/* Mark that our own mode must go out (mode changed / connected). Any thread. */
void ts3_cemode_signal_send_pending(void);

/* 1 when a send is waiting. Any thread. */
int ts3_cemode_send_pending(void);

/* Broadcast own mode if pending and not rate limited. TS callback ONLY. */
void ts3_cemode_flush(void);

/* Handle an incoming plugin command. Returns 1 when it was a CEMODE command
   (handled or ignored), 0 otherwise. TS callback thread ONLY. */
int ts3_cemode_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID);

/* Forget one peer (left the server). Any thread. */
void ts3_cemode_clear_client(anyID clientID);

/* Forget all peers (disconnect / server switch / shutdown). Any thread. */
void ts3_cemode_reset(void);

/* Write "Sprechmodus: Shout (60 m)" for a peer into buf. Returns bytes written
   (excluding NUL), 0 when that peer's mode is unknown. Any thread. */
int ts3_cemode_format_peer(anyID clientID, char* buf, size_t bufSize);

#endif /* TS3_PROXIMITY_TS3_CEMODE_H */
