#ifndef CORE_HTTP_POS_HTTP_SERVER_H
#define CORE_HTTP_POS_HTTP_SERVER_H

/*
 * Localhost HTTP position bridge — Workshop / CEE Blueprint endpoint.
 *
 * Frozen contract for mod authors: doku/module/pos-http.md
 *
 * Fixed URL (burned into the Workshop mod):
 *   http://127.0.0.1:52734/
 *
 * Approved CEE origin (exact): http://127.0.0.1:52734
 *
 * Routes:
 *   GET  /health         → 200 {"ok":true,...}
 *   POST /v1/position    → 200 / 400  (JSON body — contract format)
 *   POST /position       → alias for /v1/position
 *
 * Contract JSON (UTF-8):
 *   {"seq":N,"x":cm,"y":cm,"z":cm,"yaw":deg,"yawY":deg}
 *   Required: x, y, z, yaw (centimeters / degrees). seq and yawY optional.
 *
 * Thread contract:
 *   - pos_http_server_start / pos_http_server_stop: TS callback thread only.
 *   - HTTP listener thread: calls pos_inject_sample only (any-thread safe).
 *     Never calls the TS API; never touches UI.
 *
 * Lifecycle:
 *   start after pos_watcher_start(); stop BEFORE pos_watcher_stop().
 */

#define POS_HTTP_PORT     52734
#define POS_HTTP_BASE_URL "http://127.0.0.1:52734"

/* Start the HTTP listener thread. Idempotent. */
void pos_http_server_start(void);

/* Stop the HTTP listener thread and release all Winsock resources.
   Blocks until the thread has exited (max ~5 s). */
void pos_http_server_stop(void);

/* ---- 102: fallback ports + handover between plugin instances (http_ports.h) ----
 *
 * The listener prefers POS_HTTP_PORT. When that port is taken it asks the
 * holder (GET /v1/status, POST /v1/yield): an idle instance of this plugin
 * hands the port over, anything else keeps it and this instance listens on a
 * fallback port instead. Extra routes:
 *   GET  /v1/status → {"ok":true,"service":"conan_exiles_ts","act":0|1,"pid":N,"port":N,"ver":"…"}
 *   POST /v1/yield  → {"ok":true,"yield":0|1}
 * Every position reply ends with "act":0|1.
 */

/* 1 = this instance has an active Conan server profile ("act" in the replies;
   an inactive instance hands the primary port over on request). Any thread. */
void pos_http_server_set_active(int active);

/* Plugin version shown by GET /v1/status. Call before start(); the pointer
   must stay valid (string literal). */
void pos_http_server_set_version(const char* version);

/* Port the listener is bound to right now, 0 = none. Any thread. */
int pos_http_server_bound_port(void);

#define POS_HTTP_NOTICE_NONE     0
#define POS_HTTP_NOTICE_FALLBACK 1 /* primary port taken by something that did not hand over */
#define POS_HTTP_NOTICE_NO_PORT  2 /* no port could be bound: positions only through Pos.txt */

/* One-shot notice for the user (the listener thread cannot print to the TS
   chat). Returns POS_HTTP_NOTICE_* and clears it; *port = our port (0 for
   NO_PORT), *holderPid = process that holds the primary port (0 = unknown).
   Any thread; meant for the TS callback thread. */
int pos_http_server_take_notice(int* port, unsigned long* holderPid);

/* Process id of a FROZEN TeamSpeak that held the primary port and was ended by
   this plugin (owner decision 2026-11-11; the exact conditions are in
   pos_http_server.c), 0 = nothing happened. One-shot, any thread. */
unsigned long pos_http_server_take_ended_frozen(void);

#endif /* CORE_HTTP_POS_HTTP_SERVER_H */
