#ifndef TS3_ADAPTER_H
#define TS3_ADAPTER_H

/*
 * TS-API core — the ONLY module that talks to the TeamSpeak client API.
 *
 * Architecture rule (lesson from the old plugin's lock-corruption crashes):
 *  - TS API functions are called ONLY on the TS callback thread.
 *  - Background threads (pos watcher, UI, audio) never call the API directly;
 *    they hand work over and request a wakeup.
 *
 * ===========================================================================
 *  THE TWO-CHANNEL CONTROL PLANE (V8.4, finalised — read this before adding
 *  a new cross-thread trigger)
 * ===========================================================================
 *
 * Off-thread producers can drive callback-thread TS work over TWO channels.
 * Both are drained by the SINGLE dispatcher `ts3plugin_onPluginCommandEvent`
 * ("CEDRAIN" branch, ts3_entry.c) in a fixed order with a per-drain budget.
 * Pick the channel by the NATURE of the work:
 *
 *  (A) COALESCING pending-work FLAGS  — for high-frequency state where "N
 *      requests" collapse to "one thing to do". A producer sets an Interlocked
 *      flag (or bumps a per-client dirty bit) and requests a wakeup. The
 *      dispatcher asks ts3_pending_work_any() (the single "is there work?"
 *      aggregator) and, when a flag is set, runs that step ONCE over current
 *      state. Examples that MUST stay flags: audio recompute-all / per-client
 *      dirty recompute, CEPOS send-pending, unmute-pending, channel
 *      position-update, voice-mode notify, pending chat.
 *      WHY: at 200 players @ 1 Hz CEPOS, turning "recompute needed" into N
 *      queued commands would flood the callback thread. Coalescing is a
 *      correctness/perf FEATURE, not a missing queue. Do NOT convert these.
 *
 *  (B) The typed command RING (Ts3Command / Ts3CmdType, this header) — for
 *      DISCRETE one-shot actions that do NOT coalesce and cannot flood: a
 *      one-time "log the channel list", a UI-triggered single request, etc.
 *      A producer fills a Ts3Command and ts3_cmd_queue_push()es it (safe from
 *      any thread; full ring drops + logs), then requests a wakeup. The
 *      dispatcher drains the ring FIRST each cycle. The pure ring mechanics
 *      live in ts3_cmd_ring.h (host-unit-tested); the lock lives in the .c.
 *
 * Rule of thumb: if the same request fired 200x in one tick should still be
 * "do it once", it is (A) a flag. If each request is a distinct action that
 * must each happen, and it can never fire at per-packet frequency, it is (B)
 * a command. When unsure, prefer a flag — flooding is the expensive mistake.
 *
 * V8.4 wakeup rebuild: ts3_request_wakeup()/..._urgent() no longer touch the
 * TS API at all — they only set an Interlocked flag and SetEvent (pure Win32,
 * callable from ANY thread incl. the PCM audio thread). A single dedicated
 * wakeup thread OWNS the off-callback sendPluginCommand("CEDRAIN:1") call.
 *
 * Result: the TS API is touched by exactly TWO threads total — the TS callback
 * thread (everything) and the wakeup thread (exactly one function,
 * sendPluginCommand). Full single-thread purity is impossible because the SDK
 * offers no timer/wake callback; this is the minimal possible surface.
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

/* Connect status event. Returns 1 when the event changed the plugin's
   active connection (active tab disconnected, or a connection was adopted);
   0 when it was ignored (background tab). Callers reset their
   per-connection caches only when this returns 1. */
int ts3_on_connect_status_changed(uint64 serverConnectionHandlerID, int newStatus);

/* User switched the active server tab. Updates the active connection and
   the connected/local-client state to the new tab. Returns 1 when the
   active connection actually changed (callers must then reset all
   per-connection caches). TS callback thread ONLY. */
int ts3_on_active_server_changed(uint64 serverConnectionHandlerID);

/* Plugin re-enabled while TS is already on a server: adopt that tab.
   Returns 1 when the active connection changed. Callback thread ONLY. */
int ts3_adopt_current_connection(void);

int ts3_is_connected(void);                 /* any thread */
uint64 ts3_get_active_connection(void);     /* any thread */
anyID ts3_get_local_client_id(void);        /* any thread */

/* ---- 3.2 command queue — channel (B): DISCRETE one-shot actions ---------- */

/* Command types for the typed ring. ONLY for discrete one-shot work that does
 * not coalesce and can never fire at per-packet frequency (see the two-channel
 * note above). High-frequency, naturally-coalescing state stays a FLAG behind
 * ts3_pending_work_any() — do NOT add "recompute", "cepos", "unmute", etc. here.
 *
 * >>> ADD NEW COMMAND TYPES HERE <<<  (parallels ts3_entry.c's
 * ">>> ADD NEW PENDING SOURCES HERE <<<" for channel A's flags.) */
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

/* 1 when the command queue has at least one entry. Any thread. */
int ts3_cmd_queue_nonempty(void);

/* ---- 3.3 wakeup ---------------------------------------------------------- */

/* Ask the callback thread to run ts3_cmd_queue_drain() soon. Any thread
   (incl. the PCM audio thread): sets a flag + SetEvent only, no TS API. The
   dedicated wakeup thread sends the actual CEDRAIN, rate-limited to one server
   round trip per PLUGIN_POLL_INTERVAL_MS. No-op after adapter shutdown. */
void ts3_request_wakeup(void);

/* Same as ts3_request_wakeup but bypasses the rate limit — for chat/UI
   feedback that must not wait for the next CEPOS/CEDRAIN cycle. */
void ts3_request_wakeup_urgent(void);

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

/* Rename the own client (flushes immediately). Returns 1 on success.
   TS callback thread ONLY. */
int ts3_set_own_nickname(const char* nickname);

/* Clients in a channel into caller buffer; returns count (0 on error).
   TS callback thread ONLY. */
int ts3_get_channel_client_list(uint64 channelID, anyID* outClients, int maxClients);

/* Nickname of any client, returns 1 on success. TS callback thread ONLY. */
int ts3_get_client_nickname(anyID clientID, char* outName, int outLen);

/* Print a line into the current chat tab. TS callback thread ONLY. */
void ts3_print_to_chat(const char* message);

/* Write a line into the TeamSpeak client log (ts3client_*.log).
   TS callback thread ONLY. */
void ts3_log_client(const char* message);

/* Virtual server unique identifier (stable per server). Returns 1 on
   success. TS callback thread ONLY. */
int ts3_get_server_uid(char* out, int outLen);

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
