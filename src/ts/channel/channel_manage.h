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
 *  - A move the server rejects (wrong password, channel full, no permission)
 *    is reported asynchronously (chan_on_move_failed); the next attempt to the
 *    same target waits 5 s, 10 s, 30 s, 60 s (chan_move_backoff.h, 086).
 *  - All decisions run on the TS callback thread inside chan_tick(); no
 *    background thread touches the TS API.
 *  - Audio mode is set only on change (dedup) and read lock-free by the
 *    audio thread (ts3_proximity_audio).
 *
 * Thread contract: every function here is TS callback thread ONLY, except
 * chan_signal_position_update and chan_has_pending_work (any thread — they
 * only touch Interlocked flags).
 */

#include "sdk/include/teamspeak/public_definitions.h"

/* 8.1 resolve "hub" and "ingame" channel IDs by name (case-insensitive).
   Returns 1 when both were found. */
int chan_find_hub_and_ingame(void);

/* 8.2 pure decision: 1 = player belongs ingame (coordinates valid). */
int chan_should_be_ingame(void);

/* 8.3 request a move of the local client. Enforces in-flight + cooldown +
   the failure backoff; returns 1 when the request was sent. */
int chan_request_move(uint64 targetChannelID);

/* Periodic driver: resolve channels, update the playback gate, and issue
   hub<->ingame moves when needed. Called from the CEDRAIN drain path. */
void chan_tick(void);

/* Pos watcher may call from any thread when coordinates become valid/invalid.
   Schedules a callback-thread chan_tick via the CEDRAIN wakeup loop. */
void chan_signal_position_update(void);

/* Own client changed channel (onClientMoveEvent): clears the in-flight
   flag and the failure backoff and refreshes the playback gate. */
void chan_on_own_move(uint64 newChannelID);

/* 086: a requestClientMove sent by chan_request_move was rejected by the
   server (onServerErrorEvent, e.g. ERROR_channel_invalid_password). Clears
   the in-flight flag, restores the real nickname when the move was
   pre-anonymized, and starts the retry backoff for that target. No-op when
   no move is in flight. TS callback thread. */
void chan_on_move_failed(unsigned int error);

/* 086: a channel was created / deleted / moved / edited (0 = unknown). Only
   sets a dirty flag + wakeup; the next chan_tick re-resolves hub/ingame once
   (TS fires one event per channel on connect — never a scan per event). The
   old cached IDs stay valid until the rescan replaced them. Callback thread. */
void chan_on_channel_list_changed(uint64 channelID);

/* Last TS-API move before plugin unload: if the local client is in
   ingame, request hub and restore the real nickname. Callback thread. */
void chan_leave_ingame_on_plugin_disable(void);

/* Drop all state (disconnect / new connection). */
void chan_reset(void);

/* 1 when CEDRAIN should keep ticking: a move is in flight, a position update
   or channel-list change is waiting for its tick, or a wanted move is only
   blocked by a short wait (cooldown / backoff up to 10 s). Any thread. */
int chan_has_pending_work(void);

/* Read-only channel IDs resolved by chan_find_hub_and_ingame (0 when unknown). */
uint64 chan_get_hub_channel_id(void);
uint64 chan_get_ingame_channel_id(void);

#endif /* TS_CHANNEL_CHANNEL_MANAGE_H */
