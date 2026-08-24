#ifndef TS3_PROXIMITY_TS3_CEPING_H
#define TS3_PROXIMITY_TS3_CEPING_H

/*
 * CEPING — liveness heartbeat on the 30 ms plugin poll tick
 * (see doku/module/ce-protokoll.md).
 *
 * Wire format: plugin command "CEPING:<payloadVersion>;<seq>", e.g.
 * "CEPING:1;42". The codec itself lives in ts3_ceping_wire.h (host-tested).
 *
 * Purpose: each client sends an increasing sequence number alongside its
 * position stream (PLUGIN_POLL_INTERVAL_MS, same floor as CEPOS). A receiver
 * watches the per-peer sequence; a forward jump of more than 1 means
 * heartbeats — and the CEPOS updates they ride with — were lost, which it
 * logs for diagnosis. CEPING never changes how anything sounds; a missing
 * CEPING (old client, packet loss) only means no liveness log.
 *
 * Thread contract:
 *  - ts3_ceping_signal_send_pending / ts3_ceping_send_pending: any thread
 *    (driven by the pos watcher). Sets an Interlocked flag + requests a
 *    wakeup; no TS API.
 *  - ts3_ceping_flush / ts3_ceping_on_plugin_command / ts3_ceping_clear_client
 *    / ts3_ceping_reset: TS callback thread ONLY. The peer table has a single
 *    owner thread (the callback thread), so it needs no lock.
 */

#include "sdk/include/teamspeak/public_definitions.h"

/* Mark that a heartbeat should go out. Any thread (pos watcher tick). */
void ts3_ceping_signal_send_pending(void);

/* 1 when a heartbeat is waiting. Any thread. */
int ts3_ceping_send_pending(void);

/* Broadcast the next heartbeat if due (rate limited). TS callback ONLY. */
void ts3_ceping_flush(void);

/* Handle an incoming plugin command. Returns 1 when it was a CEPING command
   (handled or ignored), 0 otherwise. TS callback thread ONLY. */
int ts3_ceping_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID);

/* Forget one peer (left the server). TS callback thread ONLY. */
void ts3_ceping_clear_client(anyID clientID);

/* Forget all peers + own counter (disconnect / server switch / shutdown).
   TS callback thread ONLY. */
void ts3_ceping_reset(void);

#endif /* TS3_PROXIMITY_TS3_CEPING_H */
