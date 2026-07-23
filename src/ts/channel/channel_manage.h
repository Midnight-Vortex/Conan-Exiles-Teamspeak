#ifndef TS_CHANNEL_CHANNEL_MANAGE_H
#define TS_CHANNEL_CHANNEL_MANAGE_H

/*
 * Phase 8 — channel management (hub <-> ingame auto-move + playback gate).
 *
 * Game running (Pos.txt fresh)  -> player belongs in the "ingame" channel.
 * Game closed  (Pos.txt stale)  -> player belongs in the "hub" channel.
 * Hub is hard-muted (nobody audible), ingame runs the proximity path.
 * Event/Plot channels suspend auto-move in both directions (SaltyChat rule).
 * Root and other non-hub channels without coords still get moved back to hub.
 *
 * Rules (lessons from the old plugin):
 *  - Never two moves in parallel: one in-flight flag with timeout, plus a
 *    cooldown between moves (old bug: parallel requestClientMove floods).
 *  - All decisions run on the TS callback thread inside chan_tick(); no
 *    background thread touches the TS API.
 *  - Audio mode is set only on change (dedup) and read lock-free by the
 *    audio thread (ts3_proximity_audio).
 *
 * Thread contract: every function here is TS callback thread ONLY.
 */

#include "sdk/include/teamspeak/public_definitions.h"

/* 8.1 resolve "hub" and "ingame" channel IDs by name (case-insensitive).
   Returns 1 when both were found. */
int chan_find_hub_and_ingame(void);

/* 8.2 pure decision: 1 = player belongs ingame (coordinates valid). */
int chan_should_be_ingame(void);

/* 8.3 request a move of the local client. Enforces in-flight + cooldown;
   returns 1 when the request was sent. */
int chan_request_move(uint64 targetChannelID);

/* Periodic driver: resolve channels, update the playback gate, and issue
   hub<->ingame moves when needed. Called from the CEDRAIN drain path. */
void chan_tick(void);

/* Pos watcher may call from any thread when coordinates become valid/invalid.
   Schedules a callback-thread chan_tick via the CEDRAIN wakeup loop. */
void chan_signal_position_update(void);

/* Own client changed channel (onClientMoveEvent): clears the in-flight
   flag and refreshes the playback gate. */
void chan_on_own_move(uint64 newChannelID);

/* Drop all state (disconnect / new connection). */
void chan_reset(void);

/* 1 when a channel move is in-flight (CEDRAIN should keep ticking). */
int chan_has_pending_work(void);

/* Read-only channel IDs resolved by chan_find_hub_and_ingame (0 when unknown). */
uint64 chan_get_hub_channel_id(void);
uint64 chan_get_ingame_channel_id(void);

#endif /* TS_CHANNEL_CHANNEL_MANAGE_H */
