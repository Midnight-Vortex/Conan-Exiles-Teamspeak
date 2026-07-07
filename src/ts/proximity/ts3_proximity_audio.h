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

/* Drop one client (left server). Callback thread. */
void ts3_audio_invalidate_client(anyID clientID);

/* Reset everything (disconnect/shutdown). Callback thread. */
void ts3_audio_reset(void);

#endif /* TS3_PROXIMITY_TS3_PROXIMITY_AUDIO_H */
