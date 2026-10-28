/*
 * pos_http_server.c — localhost HTTP position bridge (port 52734).
 *
 * Accepts GET /health and POST /v1/position (alias /position) on
 * 127.0.0.1 only.  Parses either JSON or Pos.txt line body format and
 * forwards the sample to pos_inject_sample().
 *
 * Thread contract:
 *   - start/stop:   TS callback thread.
 *   - Listener thread: never calls the TS API; calls pos_inject_sample()
 *     which is safe from any thread. No overlay, no UI.
 *   - Logging (rate summary, POST success, errors, raw IN for GET/404): HTTP
 *     thread only, via log_write / log_debug (log module lock — never TS API).
 *     Successful POST /v1/position → http_send first, then log_debug per sample
 *     (visible with debug=1); ~1 Hz rate line is log_debug when fail=0,
 *     log_write when the window had failures. POST 400 → log_write.
 *     Every error/reject log is rate limited (~1 line/s per kind, with a
 *     "+N suppressed" counter): a local client can no longer flood plugin.log.
 *     Dropped connections (recv fail / incomplete headers / deadline) →
 *     throttled drop log.
 *   - Per connection: every recv() waits at most HTTP_RECV_TMO_MS and the whole
 *     request must be complete within HTTP_CONN_DEADLINE_MS after accept(), so a
 *     slow-drip client cannot hold the single listener thread (and with it
 *     pos_http_server_stop()) hostage.
 *   - Browser guard (http_request.h): requests with a foreign Origin (anything
 *     but our own http://127.0.0.1:52734 / http://localhost:52734), a
 *     Sec-Fetch-Site of cross-site / same-site or a non-loopback Host get 403 on
 *     every endpoint - web pages cannot drive the listener, the CEE mod can.
 *     Every 403 logs the header name and value (throttled).
 *   - The listener thread owns the listen socket: it (re)binds with an
 *     exponential backoff (250 ms .. 5 s) after accept() errors or when the
 *     first bind in start() failed.  Transient accept() errors (one client
 *     vanished) do not tear the socket down.  stop() stays prompt: all waits
 *     are on the stop event.
 *
 * winsock2.h MUST precede windows.h — kept first here.
 */

/* winsock2.h must come before windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "core/http/http_request.h"
#include "core/http/pos_http_server.h"
#include "core/hub/mod_password.h"
#include "core/mod_file/pos_file.h"
#include "core/mod_file/pos_parse.h"
#include "core/proximity/occlusion.h"
#include "core/proximity/talk_state.h"
#include "core/util/log.h"

/* ---- constants --------------------------------------------------------- */

#define HTTP_BUF_SIZE    4096   /* header + body combined recv buffer */
#define HTTP_BODY_MAX    1024   /* max body bytes we care about */
#define HTTP_RECV_TMO_MS 100    /* per-connection recv timeout (fail-fast) */
#define HTTP_CONN_DEADLINE_MS 500 /* whole request (headers + body) after accept */
#define HTTP_LOG_THROTTLE_MS  1000  /* error / reject log lines: max 1 per second per kind */
#define HTTP_BIND_LOG_MS      30000 /* repeated bind failures: one line per 30 s */
#define HTTP_ACCEPT_TRANSIENT_MAX 50 /* consecutive "transient" accept errors before a rebind */

/* ---- module state ------------------------------------------------------ */

/* Written by the listener thread (bind) and closed by BOTH the listener thread
   (accept recovery) and pos_http_server_stop (to unblock accept): every write
   goes through an atomic exchange so the socket is closed exactly once. */
static volatile SOCKET g_listenSocket = INVALID_SOCKET;
static HANDLE        g_httpThread   = NULL;
static HANDLE        g_stopEvent    = NULL;
static volatile long g_httpRunning  = 0;

/* POST rate stats — HTTP listener thread only (sequential accept+handle). */
static ULONGLONG g_httpLastTick     = 0;
static ULONGLONG g_httpWindowStart  = 0;
static int       g_httpWindowCount  = 0;
static int       g_httpWindowOk     = 0;
static int       g_httpWindowFail   = 0;
static int       g_httpDtSamples    = 0;
static ULONGLONG g_httpDtMin        = MAXULONGLONG;
static ULONGLONG g_httpDtMax        = 0;
static ULONGLONG g_httpDtSum        = 0;
static int       g_httpWindowLastSeq = 0;
static double    g_httpWindowLastX   = 0.0;
static double    g_httpWindowLastY   = 0.0;
static double    g_httpWindowLastZ   = 0.0;

/* Log throttles — HTTP listener thread only (sequential accept+handle).  The
   bind throttle is also used by start() before the thread exists. */
static HttpThrottle g_thrDrop;      /* dropped connections */
static HttpThrottle g_thrForbidden; /* 403 browser guard */
static HttpThrottle g_thrPost400;   /* POST answered with 400 */
static HttpThrottle g_thrIn;        /* raw IN line for 404 / unknown requests */
static HttpThrottle g_thrAccept;    /* accept() failures */
static HttpThrottle g_thrBind;      /* bind()/listen() failures */

/* ---- helpers ----------------------------------------------------------- */

/* Throttled drop log (~1 Hz max) for recv failures, incomplete headers and
   connections that missed their deadline. */
static void http_drop_log(const char* reason, int err) {
    char tail[32];
    int suppressed = 0;

    if (!http_throttle_pass(&g_thrDrop, GetTickCount64(), HTTP_LOG_THROTTLE_MS, &suppressed)) {
        return;
    }
    http_suppressed_text(tail, (int)sizeof(tail), suppressed);

    if (reason && strcmp(reason, "recv") == 0) {
        log_write("HTTP: drop reason=recv err=%d%s", err, tail);
    }
    else {
        log_write("HTTP: drop reason=%s%s", reason ? reason : "unknown", tail);
    }
}

/* Send a complete HTTP/1.1 response and close the connection. */
static void http_send(SOCKET s, int status, const char* body) {
    char resp[2048]; /* 061 probes (<= 640 B) + 067 talkers (<= ~700 B) + headers */
    const char* status_text =
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" :
        status == 403 ? "Forbidden" : "Not Found";
    const int body_len = (int)strlen(body);
    const int n = snprintf(resp, (int)sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, status_text, body_len, body);
    if (n > 0 && n < (int)sizeof(resp)) {
        send(s, resp, n, 0);
    }
    shutdown(s, SD_SEND);
}

/* Parse a named float field from a JSON body: "key": <number> */
static int json_float(const char* buf, const char* key, double* out) {
    /* Build search token: "key": */
    char token[48];
    if (snprintf(token, sizeof(token), "\"%s\":", key) >= (int)sizeof(token)) {
        return 0;
    }
    const char* p = strstr(buf, token);
    if (!p) {
        return 0;
    }
    p += strlen(token);
    /* Skip optional whitespace after colon. */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char* end;
    *out = strtod(p, &end);
    return (end != p) ? 1 : 0;
}

/* Parse a named float field from a Pos.txt line: KEY=<number> */
static int pos_float(const char* buf, const char* key, double* out) {
    const char* p = strstr(buf, key);
    if (!p) {
        return 0;
    }
    p += strlen(key);
    char* end;
    *out = strtod(p, &end);
    return (end != p) ? 1 : 0;
}

/* Parse a position body.  Returns 1 on success.
   Contract format (Workshop / CEE Blueprint — doku/module/pos-http.md):
     JSON: {"seq":1,"x":<cm>,"y":<cm>,"z":<cm>,"yaw":<deg>,"yawY":<deg>}
   Legacy (still accepted, not the mod contract):
     Pos.txt line: SEQ=1 X=<cm> Y=<cm> Z=<cm> YAW=<deg> [YAWY=<deg>] */
static int http_parse_body(const char* body, PosSample* out) {
    if (!body || !out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    /* Skip leading whitespace. */
    while (*body == ' ' || *body == '\r' || *body == '\n' || *body == '\t') {
        body++;
    }

    if (*body == '{') {
        /* JSON format — lowercase field names per spec. */
        double seq = 0.0;
        if (!json_float(body, "x", &out->x)
            || !json_float(body, "y", &out->y)
            || !json_float(body, "z", &out->z)
            || !json_float(body, "yaw", &out->yaw)) {
            return 0;
        }
        json_float(body, "seq", &seq);
        if (!json_float(body, "yawY", &out->yawY)) {
            out->yawY = 0.0;
        }
        out->seq = pos_seq_from_double(seq);
    } else {
        /* Pos.txt line format — normalize DE-locale comma decimals first. */
        char normalized[HTTP_BODY_MAX];
        size_t body_len = strlen(body);
        if (body_len >= sizeof(normalized)) {
            body_len = sizeof(normalized) - 1;
        }
        memcpy(normalized, body, body_len);
        normalized[body_len] = '\0';
        for (char* p = normalized; *p; p++) {
            if (*p == ',') {
                *p = '.';
            }
        }

        double seq = 0.0;
        if (!pos_float(normalized, "SEQ=", &seq)
            || !pos_float(normalized, "X=", &out->x)
            || !pos_float(normalized, "Y=", &out->y)
            || !pos_float(normalized, "Z=", &out->z)
            || !pos_float(normalized, "YAW=", &out->yaw)) {
            return 0;
        }
        if (!pos_float(normalized, "YAWY=", &out->yawY)) {
            out->yawY = 0.0;
        }
        out->seq = pos_seq_from_double(seq);
    }
    return 1;
}

/* Collapse body to a single printable line (truncated). Returns bytes written. */
static int http_format_raw(const char* body, int bodyLen, char* raw, int rawSize) {
    int n = 0;
    int i;

    if (!raw || rawSize <= 0) {
        return 0;
    }
    raw[0] = '\0';
    if (!body) {
        body = "";
    }
    if (bodyLen < 0) {
        bodyLen = (int)strlen(body);
    }

    for (i = 0; i < bodyLen && n < rawSize - 1; i++) {
        const unsigned char c = (unsigned char)body[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            raw[n++] = ' ';
        }
        else if (c >= 32 && c < 127) {
            raw[n++] = (char)c;
        }
        else {
            raw[n++] = '?';
        }
    }
    raw[n] = '\0';
    if (bodyLen > n && n >= 3) {
        raw[n - 3] = '.';
        raw[n - 2] = '.';
        raw[n - 1] = '.';
    }
    return n;
}

/* Log raw inbound request. 200 → log_debug; everything else → log_write,
   rate limited (404 / unknown paths can be hammered by any local client). */
static void http_log_raw_in(const char* method, const char* path,
    const char* body, int bodyLen, int httpStatus) {
    char raw[420];

    if (!method || !path) {
        return;
    }

    if (httpStatus == 200) {
        http_format_raw(body, bodyLen, raw, (int)sizeof(raw));
        log_debug("HTTP: IN %s %s status=%d cl=%d raw=\"%s\"",
            method, path, httpStatus, bodyLen, raw);
    }
    else {
        char tail[32];
        int suppressed = 0;

        if (!http_throttle_pass(&g_thrIn, GetTickCount64(), HTTP_LOG_THROTTLE_MS, &suppressed)) {
            return;
        }
        http_suppressed_text(tail, (int)sizeof(tail), suppressed);
        http_format_raw(body, bodyLen, raw, (int)sizeof(raw));
        log_write("HTTP: IN %s %s status=%d cl=%d raw=\"%s\"%s",
            method, path, httpStatus, bodyLen, raw, tail);
    }
}

/* 403 from the browser guard: throttled log naming the header and its value
   (sanitised), so a blocked CEE node is immediately visible in plugin.log. */
static void http_forbidden_log(int guard, const char* what) {
    char value[96];
    char tail[32];
    int suppressed = 0;

    if (!http_throttle_pass(&g_thrForbidden, GetTickCount64(), HTTP_LOG_THROTTLE_MS, &suppressed)) {
        return;
    }
    http_suppressed_text(tail, (int)sizeof(tail), suppressed);
    http_format_raw(what, -1, value, (int)sizeof(value));
    log_write("HTTP: 403 rejected header %s=\"%s\"%s", http_guard_header(guard), value, tail);
}

/* Update POST rate stats; emit ~1 Hz summary via log_write. */
static void http_post_rate_tick(int injected, ULONGLONG dtMs, const PosSample* okSample) {
    const ULONGLONG now = GetTickCount64();

    if (g_httpLastTick != 0) {
        if (dtMs < g_httpDtMin) {
            g_httpDtMin = dtMs;
        }
        if (dtMs > g_httpDtMax) {
            g_httpDtMax = dtMs;
        }
        g_httpDtSum += dtMs;
        g_httpDtSamples++;
    }
    g_httpLastTick = now;

    if (g_httpWindowStart == 0) {
        g_httpWindowStart = now;
    }
    g_httpWindowCount++;
    if (injected) {
        g_httpWindowOk++;
        if (okSample) {
            g_httpWindowLastSeq = okSample->seq;
            g_httpWindowLastX = okSample->x;
            g_httpWindowLastY = okSample->y;
            g_httpWindowLastZ = okSample->z;
        }
    }
    else {
        g_httpWindowFail++;
    }

    if (now - g_httpWindowStart >= 1000ULL) {
        const ULONGLONG dtMinOut = (g_httpDtMin == MAXULONGLONG) ? 0ULL : g_httpDtMin;
        const ULONGLONG dtAvgOut = (g_httpDtSamples > 0)
            ? (g_httpDtSum / (ULONGLONG)g_httpDtSamples) : 0ULL;

        if (g_httpWindowOk > 0) {
            if (g_httpWindowFail > 0) {
                log_write("HTTP: rate n=%d/s dt=min/avg/max=%llu/%llu/%llums ok=%d fail=%d last_seq=%d last=X=%.6f Y=%.6f Z=%.6f",
                    g_httpWindowCount,
                    (unsigned long long)dtMinOut,
                    (unsigned long long)dtAvgOut,
                    (unsigned long long)g_httpDtMax,
                    g_httpWindowOk,
                    g_httpWindowFail,
                    g_httpWindowLastSeq,
                    g_httpWindowLastX,
                    g_httpWindowLastY,
                    g_httpWindowLastZ);
            }
            else {
                log_debug("HTTP: rate n=%d/s dt=min/avg/max=%llu/%llu/%llums ok=%d fail=%d last_seq=%d last=X=%.6f Y=%.6f Z=%.6f",
                    g_httpWindowCount,
                    (unsigned long long)dtMinOut,
                    (unsigned long long)dtAvgOut,
                    (unsigned long long)g_httpDtMax,
                    g_httpWindowOk,
                    g_httpWindowFail,
                    g_httpWindowLastSeq,
                    g_httpWindowLastX,
                    g_httpWindowLastY,
                    g_httpWindowLastZ);
            }
        }
        else {
            log_write("HTTP: rate n=%d/s dt=min/avg/max=%llu/%llu/%llums ok=%d fail=%d",
                g_httpWindowCount,
                (unsigned long long)dtMinOut,
                (unsigned long long)dtAvgOut,
                (unsigned long long)g_httpDtMax,
                g_httpWindowOk,
                g_httpWindowFail);
        }

        g_httpWindowStart = now;
        g_httpWindowCount = 0;
        g_httpWindowOk = 0;
        g_httpWindowFail = 0;
        g_httpDtSamples = 0;
        g_httpDtMin = MAXULONGLONG;
        g_httpDtMax = 0;
        g_httpDtSum = 0;
    }
}

/* POST that cannot be served (bad / oversized / incomplete body): answer 400,
   count it as a failed POST, log it throttled.  NEVER logs the body - the mod
   password (TSPW=) has not been cut out of it at this point. */
static void http_post_fail(SOCKET client, const char* reason, long contentLength) {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG dtMs = 0;
    char tail[32];
    int suppressed = 0;

    http_send(client, 400, "{\"ok\":false,\"error\":\"invalid request\"}");

    if (g_httpLastTick != 0) {
        dtMs = now - g_httpLastTick;
    }
    http_post_rate_tick(0, dtMs, NULL);

    if (http_throttle_pass(&g_thrPost400, now, HTTP_LOG_THROTTLE_MS, &suppressed)) {
        http_suppressed_text(tail, (int)sizeof(tail), suppressed);
        log_write("HTTP: POST status=400 reason=%s cl=%ld%s", reason, contentLength, tail);
    }
}

/* Handle one accepted client connection. */
static void http_handle_client(SOCKET client) {
    /* The whole request must be in within HTTP_CONN_DEADLINE_MS of accept():
       the single listener thread must not be held by a slow-drip client. */
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)HTTP_CONN_DEADLINE_MS;

    /* Per-connection recv timeout — fail fast on dead/cancelled CEE sockets. */
    DWORD tmo = HTTP_RECV_TMO_MS;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, (int)sizeof(tmo));

    {
        const int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
            (const char*)&nodelay, (int)sizeof(nodelay));
    }

    char buf[HTTP_BUF_SIZE];
    int total = 0;
    int header_end = -1;

    /* Receive until end-of-headers marker is found or buffer fills. */
    while (total < (int)(sizeof(buf) - 1)) {
        if (GetTickCount64() >= deadline) {
            http_drop_log("deadline", 0);
            return;
        }
        const int n = recv(client, buf + total, (int)(sizeof(buf) - 1 - total), 0);
        if (n <= 0) {
            http_drop_log("recv", (n < 0) ? (int)WSAGetLastError() : 0);
            return;
        }
        total += n;
        buf[total] = '\0';
        header_end = http_find_header_end(buf, total);
        if (header_end >= 0) {
            break;
        }
    }
    if (header_end < 0) {
        http_drop_log("incomplete", 0);
        return;
    }

    /* Parse request line: METHOD PATH HTTP/x.x */
    char method[8];
    char path[128];
    if (sscanf(buf, "%7s %127s", method, path) != 2) {
        return;
    }

    /* Browser guard, every endpoint: Origin / Sec-Fetch-Site / Host. */
    {
        char what[96];
        const int guard = http_request_guard(buf, header_end, POS_HTTP_PORT, what, (int)sizeof(what));
        if (guard != HTTP_GUARD_OK) {
            http_send(client, 403, "{\"ok\":false,\"error\":\"forbidden\"}");
            http_forbidden_log(guard, what);
            return;
        }
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        http_log_raw_in(method, path, "", 0, 200);
        http_send(client, 200,
            "{\"ok\":true,\"service\":\"conan_exiles_ts\",\"url\":\"" POS_HTTP_BASE_URL "\"}");
        return;
    }

    /* POST /v1/position  (or alias /position) */
    if (strcmp(method, "POST") == 0
        && (strcmp(path, "/v1/position") == 0 || strcmp(path, "/position") == 0)) {

        /* Body length from the Content-Length HEADER (header block only). */
        long content_length = 0;
        const int clState = http_content_length(buf, header_end,
            (long)(sizeof(buf) - 1 - (size_t)header_end), &content_length);
        if (clState == HTTP_CL_INVALID) {
            http_post_fail(client, "content-length", 0);
            return;
        }
        if (clState == HTTP_CL_TOO_BIG) {
            http_post_fail(client, "body-too-large", content_length);
            return;
        }
        const int haveCl = (clState == HTTP_CL_OK);

        /* Read the remaining body bytes (exactly as many as announced). */
        int body_have = total - header_end;
        while (haveCl && (long)body_have < content_length) {
            if (GetTickCount64() >= deadline) {
                break;
            }
            const int n = recv(client, buf + total, (int)(content_length - (long)body_have), 0);
            if (n <= 0) {
                break;
            }
            total += n;
            body_have += n;
        }
        if (http_body_incomplete(haveCl, content_length, body_have)) {
            /* Late / short body: never parse a half password or half position. */
            http_post_fail(client, "body-incomplete", content_length);
            return;
        }

        {
            char* body = buf + header_end;
            int bodyLen = http_body_len(haveCl, content_length, total - header_end);
            body[bodyLen] = '\0'; /* nothing behind the announced body is ever parsed */
            /* 077: the mod's ingame channel password (" TSPW=<pw>", always the
               LAST field) is cut off and wiped here, before anything parses or
               logs the body (the 400 paths log the raw body). */
            bodyLen = mod_password_take_from_body(body, bodyLen, GetTickCount64());
            PosSample sample;
            const int parsed = http_parse_body(body, &sample);
            /* PROTOTYPE occlusion (061): store before inject so the recompute
               triggered by this sample already sees the new values. Server
               switch off (064) -> no store, no probes: the mod traces nothing. */
            const int occOptIn = parsed && occlusion_is_enabled()
                && occlusion_store_from_text(body, GetTickCount64()) >= 0;
            const int injected = parsed && pos_inject_sample(&sample);
            const int status = injected ? 200 : 400;
            const ULONGLONG now = GetTickCount64();
            ULONGLONG dtMs = 0;

            if (g_httpLastTick != 0) {
                dtMs = now - g_httpLastTick;
            }

            if (injected) {
                const float lx = (float)(sample.x / 100.0);
                const float ly = (float)(sample.y / 100.0);
                const float lz = (float)(sample.z / 100.0);
                char reply[1536];
                if (!occOptIn || occlusion_format_probes(reply, 640, lx, ly, lz) <= 0) {
                    strcpy_s(reply, sizeof(reply), "{\"ok\":true}");
                }
                else {
                    /* 072: tell the mod this plugin reads OCC v2 + its parameters */
                    occlusion_append_cfg_json(reply, (int)sizeof(reply));
                }
                /* 067: talk status for the mod's mouth animation (appended last,
                   so the mod's probe parser - text up to the next quote - is unaffected). */
                talk_state_append_json(reply, (int)sizeof(reply), lx, ly, lz);
                http_send(client, 200, reply);
            }
            else {
                http_send(client, 400, "{\"ok\":false,\"error\":\"invalid position\"}");
            }

            http_post_rate_tick(injected, dtMs, injected ? &sample : NULL);

            if (injected) {
                log_debug("HTTP: POST seq=%d pos=X=%.6f Y=%.6f Z=%.6f YAW=%.6f YAWY=%.6f dt=%llums status=%d",
                    sample.seq,
                    sample.x, sample.y, sample.z,
                    sample.yaw, sample.yawY,
                    (unsigned long long)dtMs,
                    status);
            }
            else {
                /* Rejected POST: one line per second, "+N suppressed" for the
                   rest (a mod sitting at the main menu posts 0,0,0 ~33x/s). */
                char tail[32];
                int suppressed = 0;
                if (http_throttle_pass(&g_thrPost400, now, HTTP_LOG_THROTTLE_MS, &suppressed)) {
                    char raw[420];
                    http_suppressed_text(tail, (int)sizeof(tail), suppressed);
                    http_format_raw(body, bodyLen, raw, (int)sizeof(raw));
                    if (!parsed) {
                        log_write("HTTP: POST status=400 reason=parse cl=%d raw=\"%s\"%s",
                            bodyLen, raw, tail);
                    }
                    else {
                        log_write("HTTP: POST status=400 reason=reject seq=%d pos=X=%.6f Y=%.6f Z=%.6f cl=%d raw=\"%s\"%s",
                            sample.seq,
                            sample.x, sample.y, sample.z,
                            bodyLen, raw, tail);
                    }
                }
            }
        }
        return;
    }

    http_log_raw_in(method, path, "", 0, 404);
    http_send(client, 404, "{\"ok\":false,\"error\":\"not found\"}");
}

/* Bind failures are logged throttled: a persistent failure is retried with
   backoff and must not fill plugin.log. */
static void http_bind_fail_log(const char* what, int err) {
    char tail[32];
    int suppressed = 0;

    if (!http_throttle_pass(&g_thrBind, GetTickCount64(), HTTP_BIND_LOG_MS, &suppressed)) {
        return;
    }
    http_suppressed_text(tail, (int)sizeof(tail), suppressed);
    if (strcmp(what, "bind") == 0) {
        log_write("HTTP: bind() failed (%d) — port %d already in use? retrying in the background%s",
            err, POS_HTTP_PORT, tail);
    }
    else {
        log_write("HTTP: %s() failed (%d) — retrying in the background%s", what, err, tail);
    }
}

/* Bind 127.0.0.1:POS_HTTP_PORT and listen.  Caller owns WSAStartup.
   Returns 1 and sets g_listenSocket on success. */
static int http_open_listen_socket(void) {
    struct sockaddr_in addr;
    int viaReuse = 0;
    int rc;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        http_bind_fail_log("socket", WSAGetLastError());
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(POS_HTTP_PORT);

    /* Plain bind first: on Windows SO_REUSEADDR lets ANOTHER process bind the
       same port too.  Only when the port is reported in use (e.g. our own
       previous instance still lingering after a plugin restart) retry with
       SO_REUSEADDR - that is what always worked here - and warn. */
    rc = bind(s, (struct sockaddr*)&addr, (int)sizeof(addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEADDRINUSE) {
        closesocket(s);
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            http_bind_fail_log("socket", WSAGetLastError());
            return 0;
        }
        {
            const int opt = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, (int)sizeof(opt));
        }
        rc = bind(s, (struct sockaddr*)&addr, (int)sizeof(addr));
        viaReuse = (rc != SOCKET_ERROR);
    }
    if (rc == SOCKET_ERROR) {
        http_bind_fail_log("bind", WSAGetLastError());
        closesocket(s);
        return 0;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        http_bind_fail_log("listen", WSAGetLastError());
        closesocket(s);
        return 0;
    }

    InterlockedExchangePointer((PVOID volatile*)&g_listenSocket, (PVOID)s);
    if (viaReuse) {
        log_write("HTTP: WARNING port %d was in use - bound with SO_REUSEADDR "
            "(another program may share it)", POS_HTTP_PORT);
    }
    return 1;
}

/* ---- listener thread --------------------------------------------------- */

static void http_close_listen_socket(void) {
    /* Take the socket atomically: the stopper and the listener thread may both
       get here at the same time; only the one that took it closes it. */
    const SOCKET s = (SOCKET)InterlockedExchangePointer(
        (PVOID volatile*)&g_listenSocket, (PVOID)INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
}

static unsigned __stdcall http_server_thread(void* arg) {
    int failStreak = 0;      /* consecutive bind / hard accept failures; reset by a served client */
    int transientStreak = 0; /* consecutive transient accept errors */
    int announced = 0;       /* "listening" already logged once */
    (void)arg;

    if (g_listenSocket != INVALID_SOCKET) {
        announced = 1;
        log_write("HTTP: listening on " POS_HTTP_BASE_URL);
    }

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            break;
        }

        /* (Re)open the listen socket: first bind failed in start(), or the
           socket was torn down after a hard accept() error. */
        if (g_listenSocket == INVALID_SOCKET) {
            if (!http_open_listen_socket()) {
                failStreak++;
                if (WaitForSingleObject(g_stopEvent, http_backoff_ms(failStreak)) != WAIT_TIMEOUT) {
                    break;
                }
                continue;
            }
            /* Stop may have run while we (re)bound — drop the new socket. */
            if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
                http_close_listen_socket();
                break;
            }
            if (announced) {
                log_write("HTTP: listening again on " POS_HTTP_BASE_URL);
            }
            else {
                log_write("HTTP: listening on " POS_HTTP_BASE_URL);
            }
            announced = 1;
        }

        struct sockaddr_in client_addr;
        int addr_len = (int)sizeof(client_addr);
        SOCKET client = accept(g_listenSocket,
            (struct sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            /* Shutdown closes the listen socket — that is a clean stop. */
            if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
                break;
            }
            const int err = WSAGetLastError();
            if (http_wsa_error_is_transient(err)
                && ++transientStreak < HTTP_ACCEPT_TRANSIENT_MAX) {
                continue; /* one client vanished - the listen socket is fine */
            }
            transientStreak = 0;
            failStreak++;
            {
                char tail[32];
                int suppressed = 0;
                if (http_throttle_pass(&g_thrAccept, GetTickCount64(), HTTP_LOG_THROTTLE_MS, &suppressed)) {
                    http_suppressed_text(tail, (int)sizeof(tail), suppressed);
                    log_write("HTTP: accept failed (%d) — recovering listener%s", err, tail);
                }
            }
            http_close_listen_socket();
            if (WaitForSingleObject(g_stopEvent, http_backoff_ms(failStreak)) != WAIT_TIMEOUT) {
                break;
            }
            continue; /* the top of the loop rebinds */
        }
        transientStreak = 0;
        failStreak = 0;
        http_handle_client(client);
        closesocket(client);
    }

    log_write("HTTP: server stopped");
    return 0;
}

/* ---- public API -------------------------------------------------------- */

void pos_http_server_start(void) {
    if (InterlockedCompareExchange(&g_httpRunning, 1, 0) != 0) {
        return; /* already running */
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_write("HTTP: WSAStartup failed (%d)", WSAGetLastError());
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) {
        log_write("HTTP: CreateEvent failed");
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    /* A failed first bind is not fatal: the listener thread keeps retrying
       (port busy during a plugin restart, firewall hiccup, ...) so HTTP comes
       back without restarting TeamSpeak. */
    if (!http_open_listen_socket()) {
        log_write("HTTP: could not listen on " POS_HTTP_BASE_URL " yet — the listener thread keeps retrying");
    }

    g_httpThread = (HANDLE)_beginthreadex(NULL, 0, http_server_thread,
        NULL, 0, NULL);
    if (!g_httpThread) {
        log_write("HTTP: _beginthreadex failed");
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        http_close_listen_socket();
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }
}

void pos_http_server_stop(void) {
    if (InterlockedCompareExchange(&g_httpRunning, 0, 0) == 0) {
        return; /* not running */
    }

    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    /* Close the listen socket to unblock accept() on the listener thread. */
    http_close_listen_socket();
    if (g_httpThread) {
        /* Bounded: a client in progress is cut off by HTTP_CONN_DEADLINE_MS and
           every wait of the thread is on the stop event. */
        if (WaitForSingleObject(g_httpThread, 5000) != WAIT_OBJECT_0) {
            log_write("HTTP: server thread slow to exit — waiting");
            WaitForSingleObject(g_httpThread, INFINITE);
        }
        CloseHandle(g_httpThread);
        g_httpThread = NULL;
    }
    /* Thread may have rebound after our first closesocket. */
    http_close_listen_socket();
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }

    WSACleanup();
    InterlockedExchange(&g_httpRunning, 0);
}
