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

#endif /* CORE_HTTP_POS_HTTP_SERVER_H */
