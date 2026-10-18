#ifndef CORE_PROXIMITY_TALK_STATE_H
#define CORE_PROXIMITY_TALK_STATE_H

/*
 * talk_state — who is talking right now (from TeamSpeak's own talk status),
 * handed to the game mod so it can move the characters' mouths (doku 067).
 *
 * TeamSpeak reports talk status per client (onTalkStatusChangeEvent) no matter
 * which push-to-talk key or voice activation a player uses, so the mod never
 * needs to know anybody's key.
 *
 * Thread contract:
 *   - talk_state_set / talk_state_clear_client / talk_state_clear: TS callback
 *     thread (single atomic writes).
 *   - talk_state_local / talk_state_is_talking: any thread (atomic reads).
 *   - talk_state_append_json: HTTP listener thread only (uses a module-static
 *     player-table snapshot buffer).
 *   Never calls the TS API, never takes a lock held by the audio thread.
 */

#define TALK_STATE_MAX_CLIENT_ID 4096   /* mirrors TS3_MAX_CLIENT_ID (core must not include ts/) */
#define TALK_MAX_TALKERS         16
#define TALK_RANGE_M             50.0f  /* mouths farther away are not worth animating */

/* Store a client's talk status. isLocal = the own TS client. */
void talk_state_set(unsigned int clientID, int talking, int isLocal);

/* 1 while the own TS client is transmitting. */
int talk_state_local(void);

/* 1 while a (remote) client is talking; 0 for unknown / out of range IDs. */
int talk_state_is_talking(unsigned int clientID);

/* Forget one client (left the server / channel). */
void talk_state_clear_client(unsigned int clientID);

/* Forget everybody (disconnect / reset). */
void talk_state_clear(void);

/* Append ,"talk":<0|1>,"talkers":"<ueX>,<ueY>,<ueZ>;..." to the JSON object in
   out (which must end with '}'). Talkers = nearest TALK_MAX_TALKERS talking
   remote players within TALK_RANGE_M of the listener (plugin coordinates,
   meters, Y-up); written in Unreal axes and centimeters like the occlusion
   probes. Returns the new string length, 0 when out does not end with '}' or
   the buffer is too small (out is then left unchanged). */
int talk_state_append_json(char* out, int outSize,
    float localX, float localY, float localZ);

#endif /* CORE_PROXIMITY_TALK_STATE_H */
