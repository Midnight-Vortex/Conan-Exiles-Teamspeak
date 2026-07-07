#ifndef TS3_ADAPTER_H
#define TS3_ADAPTER_H

/*
 * TS-API core — the ONLY module that talks to the TeamSpeak client API.
 *
 * Architecture rule (lesson from the old plugin's lock-corruption crashes):
 *  - TS API functions are called ONLY on the TS callback thread.
 *  - Background threads (pos watcher, later audio/heartbeat) never call the
 *    API directly; they push commands into the queue and request a wakeup.
 *  - The single exception is ts3_request_wakeup(): it may send one
 *    rate-limited "CEDRAIN" plugin command from any thread (the TS client
 *    SDK is internally thread-safe; the old plugin's crashes came from its
 *    own shared CRITICAL_SECTION, which this rewrite does not have).
 *
 * Thread contract per function is noted below.
 */

#include "sdk/include/ts3_functions.h"
#include "sdk/include/teamspeak/public_definitions.h"
#include "sdk/include/plugin_definitions.h"

#include <stdint.h>

/* ---- setup (TS callback thread) ---------------------------------------- */

void ts3_adapter_set_functions(const struct TS3Functions* funcs);
void ts3_adapter_set_plugin_id(const char* id);

/* ---- 3.5 thread contract ------------------------------------------------ */

/* Mark the current thread as the TS callback thread. Called at the top of
   every ts3plugin_* event in ts3_entry.c. */
void ts3_thread_mark_callback(void);

/* 1 when the current thread is the TS callback thread. Any thread. */
int ts3_thread_is_callback(void);

/* ---- 3.1 connection state (written on callback thread, read anywhere) --- */

void ts3_on_connect_status_changed(uint64 serverConnectionHandlerID, int newStatus);

int ts3_is_connected(void);                 /* any thread */
uint64 ts3_get_active_connection(void);     /* any thread */
anyID ts3_get_local_client_id(void);        /* any thread */

/* ---- 3.2 command queue --------------------------------------------------- */

typedef enum Ts3CmdType {
    TS3_CMD_NONE = 0,
    TS3_CMD_LOG_CHANNEL_LIST   /* Phase 3 self-test: log channel count+names */
} Ts3CmdType;

typedef struct Ts3Command {
    Ts3CmdType type;
    uint64 u64a;
    uint64 u64b;
    float f0, f1, f2, f3;
    char text[128];
} Ts3Command;

/* Push a command from ANY thread. Returns 1 on success, 0 when queue full
   (command is dropped and counted, never blocks). */
int ts3_cmd_queue_push(const Ts3Command* cmd);

/* Drain and execute all queued commands. TS callback thread ONLY —
   returns immediately (logging once) when called from a wrong thread. */
void ts3_cmd_queue_drain(void);

/* ---- 3.3 wakeup ---------------------------------------------------------- */

/* Ask the TS callback thread to run ts3_cmd_queue_drain() soon. Any thread.
   Rate-limited to one server round trip per 50 ms. */
void ts3_request_wakeup(void);

/* ---- 3.4 channel queries (TS callback thread ONLY) ----------------------- */

/* Channel of a client, 0 on error. */
uint64 ts3_get_channel_of_client(anyID clientID);

/* Fetch channel list into caller buffer; returns count (0 on error). */
int ts3_get_channel_list(uint64* outChannels, int maxChannels);

/* Channel name lookup, returns 1 on success. */
int ts3_get_channel_name(uint64 channelID, char* outName, int outLen);

/* ---- plugin commands (TS callback thread ONLY) ---------------------------- */

/* Registered plugin ID ("" until registered). Any thread (read-only). */
const char* ts3_get_plugin_id(void);

/* Send a plugin command to all clients on the server. Returns 1 on success. */
int ts3_send_plugin_command_server(const char* command);

/* Own TS nickname, returns 1 on success. */
int ts3_get_own_nickname(char* outName, int outLen);

/* Print a line into the current chat tab. TS callback thread ONLY. */
void ts3_print_to_chat(const char* message);

/* Temporarily unmute clients for playback (batch, one API round trip).
   TS callback thread ONLY. Returns number of clients unmuted. */
int ts3_unmute_clients_for_pcm(const anyID* clients, int count);

/* Move the local client to a channel (password "" when none). Returns 1 when
   the request was sent. TS callback thread ONLY — flood protection lives in
   channel_manage (in-flight flag + cooldown). */
int ts3_request_client_move(anyID clientID, uint64 channelID, const char* password);

/* Ask the server for a channel's description (arrives via
   onChannelDescriptionUpdateEvent). Returns 1 when the request was sent.
   TS callback thread ONLY — rate limiting lives in ts3_server_profile. */
int ts3_request_channel_description(uint64 channelID);

/* Fetch a channel's description into the caller buffer. Returns 1 on
   success. TS callback thread ONLY. */
int ts3_get_channel_description(uint64 channelID, char* out, int outLen);

/* ---- 3D audio (TS callback thread ONLY, raw calls — dedup lives in ts3_3d) */

/* systemset3DSettings. Returns 1 on success. */
int ts3_set_3d_settings(float distanceFactor, float rolloffScale);

/* systemset3DListenerAttributes. Returns 1 on success. */
int ts3_set_3d_listener(const TS3_VECTOR* position, const TS3_VECTOR* forward,
    const TS3_VECTOR* up);

/* channelset3DAttributes for one client. Returns 1 on success. */
int ts3_set_3d_client(anyID clientID, const TS3_VECTOR* position);

/* ---- shutdown (TS callback thread) --------------------------------------- */

void ts3_adapter_shutdown(void);

#endif /* TS3_ADAPTER_H */
