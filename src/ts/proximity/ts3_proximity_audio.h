#ifndef TS3_PROXIMITY_TS3_PROXIMITY_AUDIO_H
#define TS3_PROXIMITY_TS3_PROXIMITY_AUDIO_H

/*
 * Proximity PCM path — per-client gain/pan snapshots + TS unmute batching.
 *
 * Thread model:
 *   writers (callback thread on CEPOS, pos watcher on local move)
 *       --seqlock publish--> per-client snapshot
 *       --read only-->  audio thread (onEditPlaybackVoiceDataEvent)
 *
 * Rules (lessons from the old plugin's crashes):
 *  - The audio thread NEVER calls the TS API and NEVER takes locks; it only
 *    reads seqlock snapshots and signals unmute via an atomic flag.
 *  - TS unmutes run ONLY as a batch on the callback thread (one API round
 *    trip), first unmute immediate, re-unmute rate-limited, pending requests
 *    are never discarded.
 */

#include "sdk/include/teamspeak/public_definitions.h"

/* 8.4 playback gate — decides what the audio thread does with incoming voice.
   Set on the callback thread (channel_manage), read lock-free per PCM buffer. */
typedef enum Ts3AudioMode {
    TS3_AUDIO_PASSTHROUGH = 0, /* plugin inactive — normal TS behavior */
    TS3_AUDIO_MUTE        = 1, /* hub/lobby — every voice hard-muted */
    TS3_AUDIO_PROXIMITY   = 2  /* ingame — distance gain + pan (Phase 6) */
} Ts3AudioMode;

/* Change the gate. Deduplicated: only acts (and logs) on an actual change.
   Any thread (atomic store), in practice the TS callback thread. */
void ts3_audio_set_mode(Ts3AudioMode mode);

Ts3AudioMode ts3_audio_get_mode(void);

/* Recompute gain/pan snapshot for one client from the player table and the
   local position. Any thread (writers serialize on a private lock). */
void ts3_audio_recompute_client(anyID clientID);

/* Recompute all known clients (local player moved). Any thread. */
void ts3_audio_recompute_all(void);

/* 6.2 PCM hot path — apply gain ramp + pan. Audio thread; lock-free. */
void ts3_audio_process_playback(anyID clientID, short* samples, int sampleCount, int channels);

/* 6.3 flag a client as needing a TS temporary unmute. Any thread. */
void ts3_audio_signal_unmute(anyID clientID);

/* 6.4 batch-flush pending unmutes. TS callback thread ONLY. */
void ts3_audio_flush_unmutes(void);

/* 1 when the audio thread flagged clients needing a TS unmute. Any thread. */
int ts3_audio_has_pending_unmutes(void);

/* Drop one client (left server). Callback thread. */
void ts3_audio_invalidate_client(anyID clientID);

/* 1 when the client's proximity snapshot is soundproof-muted. Any thread. */
int ts3_proximity_audio_soundproof_muted(unsigned int clientID);

/* Reset everything (disconnect/shutdown). Callback thread. */
void ts3_audio_reset(void);

#endif /* TS3_PROXIMITY_TS3_PROXIMITY_AUDIO_H */
