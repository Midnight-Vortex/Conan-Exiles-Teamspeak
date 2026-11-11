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
#include <iphlpapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "core/http/http_ports.h"
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
#define HTTP_PEER_TMO_MS      400   /* 102: question to the instance that holds the primary port */
#define HTTP_YIELD_WAIT_MS    1000  /* 102: how long to wait for the primary port after a yield */
#define HTTP_YIELD_AVOID_MS   15000 /* 102: after handing the primary port over, do not take it back */

/* ---- module state ------------------------------------------------------ */

/* Written by the listener thread (bind) and closed by BOTH the listener thread
   (accept recovery) and pos_http_server_stop (to unblock accept): every write
   goes through an atomic exchange so the socket is closed exactly once. */
static volatile SOCKET g_listenSocket = INVALID_SOCKET;
static HANDLE        g_httpThread   = NULL;
static HANDLE        g_stopEvent    = NULL;
static volatile long g_httpRunning  = 0;

/* 102 — fallback ports + handover. g_boundPort / g_instanceActive / g_notice*
   are read from other threads (Interlocked); the rest is listener thread only. */
static volatile long g_boundPort      = 0; /* port of g_listenSocket, 0 = none */
static volatile long g_instanceActive = 0; /* this instance has an active Conan server profile */
static volatile long g_yieldPending   = 0; /* POST /v1/yield accepted: give the primary port away */
static volatile long g_notice         = POS_HTTP_NOTICE_NONE; /* waiting for the callback thread */
static volatile long g_noticePort     = 0;
static volatile long g_noticeHolder   = 0;
static long          g_noticeLast     = POS_HTTP_NOTICE_NONE; /* last state published (dedupe) */
static ULONGLONG     g_avoidPrimaryUntilMs = 0;
static const char*   g_pluginVersion  = "";

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

    /* 102: the port this listener really serves (primary or a fallback). */
    const int ownPort = (int)InterlockedCompareExchange(&g_boundPort, 0, 0);
    const int active = InterlockedCompareExchange(&g_instanceActive, 0, 0) != 0;

    /* Browser guard, every endpoint: Origin / Sec-Fetch-Site / Host. */
    {
        char what[96];
        const int guard = http_request_guard(buf, header_end, ownPort ? ownPort : POS_HTTP_PORT,
            what, (int)sizeof(what));
        if (guard != HTTP_GUARD_OK) {
            http_send(client, 403, "{\"ok\":false,\"error\":\"forbidden\"}");
            http_forbidden_log(guard, what);
            return;
        }
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[160];
        http_log_raw_in(method, path, "", 0, 200);
        snprintf(body, sizeof(body),
            "{\"ok\":true,\"service\":\"" HTTP_SERVICE_NAME "\",\"url\":\"http://127.0.0.1:%d\"}",
            ownPort ? ownPort : POS_HTTP_PORT);
        http_send(client, 200, body);
        return;
    }

    /* 102: GET /v1/status - who is this instance (another instance or the mod asks). */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/status") == 0) {
        char body[192];
        if (!http_status_json(body, (int)sizeof(body), active, (unsigned long)GetCurrentProcessId(),
                ownPort, g_pluginVersion)) {
            strcpy_s(body, sizeof(body), "{\"ok\":true,\"service\":\"" HTTP_SERVICE_NAME "\"}");
        }
        http_send(client, 200, body);
        return;
    }

    /* 102: POST /v1/yield - a newer instance asks for the primary port. Only an
       instance WITHOUT an active Conan server profile gives it away; the listener
       loop closes the socket after this reply and reopens on a fallback port. */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/yield") == 0) {
        const int yield = http_yield_allowed(ownPort, active);
        if (yield) {
            InterlockedExchange(&g_yieldPending, 1);
        }
        http_send(client, 200, yield ? "{\"ok\":true,\"yield\":1}" : "{\"ok\":true,\"yield\":0}");
        log_write("HTTP: another TeamSpeak instance asked for port %d - %s", ownPort,
            yield ? "handing it over (this instance has no active Conan server)"
                  : "keeping it (this instance is active or not on the primary port)");
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
                talk_state_append_json(reply, (int)sizeof(reply), lx, ly, lz, GetTickCount64());
                /* 102: tell the mod whether THIS instance is the one with the Conan
                   server (two TeamSpeak processes: the mod then looks for the other). */
                http_reply_append_act(reply, (int)sizeof(reply), active);
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
        log_write("HTTP: bind() failed (%d) — no port free (%d / %d)? retrying in the background%s",
            err, http_port_at(0), http_port_at(1), tail);
    }
    else {
        log_write("HTTP: %s() failed (%d) — retrying in the background%s", what, err, tail);
    }
}

/* A listening socket on 127.0.0.1:<port>, INVALID_SOCKET on failure (*err =
   Winsock error; nothing is logged here: the caller decides, 102).
   The bind is EXCLUSIVE: two TeamSpeak processes must never share the port -
   the game mod would then reach one of them at random. The old "retry with
   SO_REUSEADDR" is gone for the same reason; a port that is busy for a moment
   is retried by the listener thread instead. */
static SOCKET http_make_listener(int port, int* err) {
    struct sockaddr_in addr;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (err) {
        *err = 0;
    }
    if (s == INVALID_SOCKET) {
        if (err) {
            *err = WSAGetLastError();
        }
        return INVALID_SOCKET;
    }
    {
        const int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, (int)sizeof(opt));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr*)&addr, (int)sizeof(addr)) == SOCKET_ERROR
        || listen(s, SOMAXCONN) == SOCKET_ERROR) {
        if (err) {
            *err = WSAGetLastError();
        }
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* Bind + publish. Caller owns WSAStartup. Returns 1 and sets g_listenSocket /
   g_boundPort on success. */
static int http_bind_port(int port, int* err) {
    const SOCKET s = http_make_listener(port, err);
    if (s == INVALID_SOCKET) {
        return 0;
    }
    InterlockedExchange(&g_boundPort, port);
    InterlockedExchangePointer((PVOID volatile*)&g_listenSocket, (PVOID)s);
    return 1;
}

/* ---- 102: who holds the primary port? ------------------------------------ */

/* One small request to another program on 127.0.0.1:<port> (normally another
   instance of this plugin). Blocking, every step bounded by HTTP_PEER_TMO_MS.
   Returns the body length (>= 0) or -1 when nothing usable came back.
   Listener thread only. */
static int http_peer_request(int port, const char* method, const char* path, char* body, int bodySize) {
    struct sockaddr_in addr;
    char req[192];
    char buf[1024];
    int total = 0;
    u_long nonBlocking = 1;
    DWORD tmo = HTTP_PEER_TMO_MS;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (body && bodySize > 0) {
        body[0] = '\0';
    }
    if (s == INVALID_SOCKET || !body || bodySize <= 0) {
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);

    /* Connect with a timeout: a frozen holder must not block the listener. */
    ioctlsocket(s, FIONBIO, &nonBlocking);
    if (connect(s, (struct sockaddr*)&addr, (int)sizeof(addr)) == SOCKET_ERROR
        && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return -1;
    }
    {
        fd_set writable;
        struct timeval tv;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        tv.tv_sec = 0;
        tv.tv_usec = HTTP_PEER_TMO_MS * 1000;
        if (select(0, NULL, &writable, NULL, &tv) <= 0) {
            closesocket(s);
            return -1;
        }
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, (int)sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, (int)sizeof(tmo));

    {
        const int n = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            method, path, port);
        if (n <= 0 || n >= (int)sizeof(req) || send(s, req, n, 0) != n) {
            closesocket(s);
            return -1;
        }
    }
    {
        /* One deadline for the whole answer: a program that drips one byte at
           a time must not hold the listener thread (and with it stop()). */
        const ULONGLONG deadline = GetTickCount64() + 2ULL * HTTP_PEER_TMO_MS;
        while (total < (int)sizeof(buf) - 1 && GetTickCount64() < deadline
            && (!g_stopEvent || WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT)) {
            const int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n <= 0) {
                break;
            }
            total += n;
        }
    }
    closesocket(s);
    buf[total] = '\0';

    {
        const char* p = strstr(buf, "\r\n\r\n");
        int len;
        if (!p) {
            return -1;
        }
        p += 4;
        len = (int)strlen(p);
        if (len >= bodySize) {
            len = bodySize - 1;
        }
        memcpy(body, p, (size_t)len);
        body[len] = '\0';
        return len;
    }
}

/* Process that listens on <port> (IPv4), 0 = unknown. Read-only system query. */
static unsigned long http_listener_pid(int port) {
    DWORD size = 0;
    unsigned long pid = 0;
    MIB_TCPTABLE_OWNER_PID* table;

    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
            != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return 0;
    }
    table = (MIB_TCPTABLE_OWNER_PID*)malloc(size);
    if (!table) {
        return 0;
    }
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            if (ntohs((u_short)table->table[i].dwLocalPort) == (u_short)port) {
                pid = (unsigned long)table->table[i].dwOwningPid;
                break;
            }
        }
    }
    free(table);
    return pid;
}

/* Full path of a process' executable, "" when it cannot be read. */
static void http_process_path(unsigned long pid, char* out, int outLen) {
    DWORD len = (DWORD)outLen;
    HANDLE h;

    out[0] = '\0';
    if (pid == 0 || outLen <= 1) {
        return;
    }
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        return;
    }
    if (!QueryFullProcessImageNameA(h, 0, out, &len)) {
        out[0] = '\0';
    }
    CloseHandle(h);
}

/* File name of a process ("ts3client_win64.exe"), "" when it cannot be read. */
static void http_process_name(unsigned long pid, char* out, int outLen) {
    char path[MAX_PATH];
    const char* base;

    out[0] = '\0';
    http_process_path(pid, path, (int)sizeof(path));
    if (!path[0]) {
        return;
    }
    base = strrchr(path, '\\');
    strncpy_s(out, (size_t)outLen, base ? base + 1 : path, _TRUNCATE);
}

/* ---- 102: a frozen TeamSpeak that keeps the port ------------------------- *
 * Owner decision 2026-11-11: when a TeamSpeak process holds the primary port
 * and does not answer any more (crashed / frozen), this plugin may end it.
 * It is ended ONLY when ALL of this holds:
 *   - this instance is active (it needs the port for a Conan server),
 *   - the holder did not answer the status question HTTP_FROZEN_MIN_TRIES
 *     times in a row over at least HTTP_FROZEN_MIN_MS,
 *   - the holder runs the SAME executable file as this process (it is a
 *     TeamSpeak client like us - a foreign program is never touched),
 *   - it has no visible window any more, or one of its windows does not
 *     respond.
 * A healthy TeamSpeak never gets here: its plugin answers within milliseconds
 * (an older plugin answers 404, which is an answer too). */
#define HTTP_FROZEN_MIN_TRIES 3
#define HTTP_FROZEN_MIN_MS    8000ULL

static unsigned long g_silentHolderPid   = 0; /* listener thread only */
static int           g_silentHolderTries = 0;
static ULONGLONG     g_silentHolderSince = 0;
static volatile long g_endedFrozenPid    = 0; /* for the callback thread (chat line), one-shot */

static void http_silent_holder_reset(void) {
    g_silentHolderPid = 0;
    g_silentHolderTries = 0;
    g_silentHolderSince = 0;
}

typedef struct HttpWindowScan {
    DWORD pid;
    int visible;
    int hung;
} HttpWindowScan;

static BOOL CALLBACK http_window_scan_cb(HWND hwnd, LPARAM param) {
    HttpWindowScan* scan = (HttpWindowScan*)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == scan->pid && IsWindowVisible(hwnd)) {
        scan->visible++;
        if (IsHungAppWindow(hwnd)) {
            scan->hung++;
        }
    }
    return TRUE;
}

/* 1 when <pid> runs the same executable as this process and looks frozen (no
   visible window, or a window that does not respond). */
static int http_holder_is_frozen_teamspeak(unsigned long pid) {
    char own[MAX_PATH];
    char other[MAX_PATH];
    HttpWindowScan scan;

    if (pid == 0 || pid == (unsigned long)GetCurrentProcessId()) {
        return 0;
    }
    http_process_path((unsigned long)GetCurrentProcessId(), own, (int)sizeof(own));
    http_process_path(pid, other, (int)sizeof(other));
    if (!own[0] || !other[0] || _stricmp(own, other) != 0) {
        return 0;
    }
    scan.pid = (DWORD)pid;
    scan.visible = 0;
    scan.hung = 0;
    EnumWindows(http_window_scan_cb, (LPARAM)&scan);
    return scan.visible == 0 || scan.hung > 0;
}

/* End the process. Returns 1 when it is gone. */
static int http_end_process(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
    BOOL ok;
    if (!h) {
        return 0;
    }
    ok = TerminateProcess(h, 1);
    if (ok) {
        WaitForSingleObject(h, 2000);
    }
    CloseHandle(h);
    return ok ? 1 : 0;
}

/* Publish a state for the callback thread (chat hint) - once per change. */
static void http_publish_notice(long notice, int port, unsigned long holderPid) {
    if (notice == g_noticeLast) {
        return;
    }
    g_noticeLast = notice;
    InterlockedExchange(&g_noticePort, port);
    InterlockedExchange(&g_noticeHolder, (long)holderPid);
    InterlockedExchange(&g_notice, notice);
}

/* Swap in a new listening socket; the old one (if any) is closed. */
static void http_publish_listener(SOCKET fresh, int port) {
    const SOCKET old = (SOCKET)InterlockedExchangePointer(
        (PVOID volatile*)&g_listenSocket, (PVOID)fresh);
    InterlockedExchange(&g_boundPort, port);
    if (old != INVALID_SOCKET) {
        closesocket(old);
    }
}

typedef struct HttpClaim {
    int bindErr;            /* Winsock error of the plain bind */
    unsigned long holder;   /* process that holds the primary port, 0 = unknown */
    int answered;           /* the holder answered the status question */
    int isPlugin;           /* ... as an instance of this plugin */
    int activeInstance;     /* ... and it has an active Conan server profile */
    int yieldAcked;         /* the holder agreed to hand the port over */
} HttpClaim;

/* Try to get the primary port. Plain bind first; when it is taken, ask the
   holder (GET /v1/status):
     - an idle instance of this plugin is asked to hand the port over
       (POST /v1/yield) - by a starting instance always (mayAskIdle), later
       only by an ACTIVE one, so two idle instances never swap the port back
       and forth;
     - a holder that does not answer may be a frozen TeamSpeak: see the rule
       above (only an active instance ends it).
   Returns the new listening socket or INVALID_SOCKET. Listener thread only. */
static SOCKET http_claim_primary(int mayAskIdle, HttpClaim* c) {
    const int primary = http_port_at(0);
    const int active = InterlockedCompareExchange(&g_instanceActive, 0, 0) != 0;
    char body[512];
    HttpPeerStatus st;
    SOCKET s;
    int len;

    memset(c, 0, sizeof(*c));
    s = http_make_listener(primary, &c->bindErr);
    if (s != INVALID_SOCKET) {
        http_silent_holder_reset();
        return s;
    }

    len = http_peer_request(primary, "GET", "/v1/status", body, (int)sizeof(body));
    http_status_parse(len >= 0 ? body : NULL, &st);
    c->answered = (len >= 0);
    c->isPlugin = st.isPlugin;
    c->holder = st.pid ? st.pid : http_listener_pid(primary);
    c->activeInstance = (c->answered && st.isPlugin && st.hasAct && st.act);

    if (c->answered) {
        http_silent_holder_reset();
        if ((mayAskIdle || active) && http_takeover_decide(1, &st) == HTTP_TAKEOVER_ASK_YIELD) {
            long yes = 0;
            const int ylen = http_peer_request(primary, "POST", "/v1/yield", body, (int)sizeof(body));
            c->yieldAcked = (ylen >= 0 && http_json_int(body, "yield", &yes) && yes == 1);
            /* The holder said yes: it closes the port within a moment. Wait
               for it instead of taking the fallback port it is moving to. */
            for (int waited = 0; c->yieldAcked && waited < HTTP_YIELD_WAIT_MS; waited += 100) {
                if (WaitForSingleObject(g_stopEvent, 100) != WAIT_TIMEOUT) {
                    return INVALID_SOCKET;
                }
                s = http_make_listener(primary, &c->bindErr);
                if (s != INVALID_SOCKET) {
                    log_write("HTTP: port %d taken over from an idle TeamSpeak instance (PID %lu)",
                        primary, c->holder);
                    return s;
                }
            }
        }
        return INVALID_SOCKET;
    }

    /* No answer. Count it; an active instance may end a frozen TeamSpeak. */
    if (!active || c->holder == 0) {
        http_silent_holder_reset();
        return INVALID_SOCKET;
    }
    {
        const ULONGLONG now = GetTickCount64();
        if (g_silentHolderPid != c->holder) {
            g_silentHolderPid = c->holder;
            g_silentHolderTries = 0;
            g_silentHolderSince = now;
        }
        g_silentHolderTries++;
        if (g_silentHolderTries < HTTP_FROZEN_MIN_TRIES
            || now - g_silentHolderSince < HTTP_FROZEN_MIN_MS
            || !http_holder_is_frozen_teamspeak(c->holder)) {
            return INVALID_SOCKET;
        }
        if (!http_end_process(c->holder)) {
            log_write("HTTP: PID %lu is a frozen TeamSpeak that holds port %d, but it could not be ended (error %lu)",
                c->holder, primary, (unsigned long)GetLastError());
            http_silent_holder_reset();
            return INVALID_SOCKET;
        }
        log_write("HTTP: ended the frozen TeamSpeak process PID %lu - it held port %d and did not answer "
            "for %llu s", c->holder, primary, (unsigned long long)((now - g_silentHolderSince) / 1000ULL));
        InterlockedExchange(&g_endedFrozenPid, (long)c->holder);
        http_silent_holder_reset();
        for (int waited = 0; waited < 2000; waited += 100) {
            s = http_make_listener(primary, &c->bindErr);
            if (s != INVALID_SOCKET) {
                return s;
            }
            if (WaitForSingleObject(g_stopEvent, 100) != WAIT_TIMEOUT) {
                break;
            }
        }
    }
    return INVALID_SOCKET;
}

/* (Re)open the listener. Primary port first; when it is taken and
   allowTakeover is set (listener thread), http_claim_primary asks the holder.
   If the primary stays with someone else we take the fallback port.
   allowTakeover = 0 (start(), TS callback thread): plain bind of the primary
   only, no waiting. Returns 1 when a port is bound. */
static int http_open_listen_socket(int allowTakeover) {
    const int primary = http_port_at(0);
    const ULONGLONG now = GetTickCount64();
    const int mayUsePrimary = (now >= g_avoidPrimaryUntilMs);
    int err = 0;
    HttpClaim claim;

    memset(&claim, 0, sizeof(claim));
    if (!allowTakeover) {
        if (mayUsePrimary && http_bind_port(primary, &err)) {
            http_publish_notice(POS_HTTP_NOTICE_NONE, primary, 0);
            return 1;
        }
        return 0;
    }

    if (mayUsePrimary) {
        const SOCKET s = http_claim_primary(1, &claim);
        if (s != INVALID_SOCKET) {
            http_publish_listener(s, primary);
            http_publish_notice(POS_HTTP_NOTICE_NONE, primary, 0);
            return 1;
        }
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            return 0;
        }
        if (claim.yieldAcked) {
            /* The holder is leaving the primary port but it is not free yet.
               Come back for it (the caller retries shortly) - the fallback
               port is the one the holder is moving to. */
            return 0;
        }

        /* Who keeps the port? One line per 30 s at most. */
        {
            char tail[32];
            char name[64];
            int suppressed = 0;
            if (http_throttle_pass(&g_thrBind, now, HTTP_BIND_LOG_MS, &suppressed)) {
                http_suppressed_text(tail, (int)sizeof(tail), suppressed);
                http_process_name(claim.holder, name, (int)sizeof(name));
                log_write("HTTP: port %d is taken (bind error %d) by PID %lu %s - %s%s",
                    primary, claim.bindErr, claim.holder, name[0] ? name : "(name unknown)",
                    claim.activeInstance ? "an active TeamSpeak instance of this plugin, it keeps the port"
                    : !claim.answered ? "it does not answer"
                    : claim.isPlugin ? "a TeamSpeak instance of this plugin that did not hand it over"
                    : "not this plugin or an older plugin version",
                    tail);
            }
        }
    }

    {
        const unsigned long holder = claim.holder;
        const int holderIsActiveInstance = claim.activeInstance;
        const int firstErr = claim.bindErr;

    for (int i = 1; i < POS_HTTP_PORT_COUNT; i++) {
        if (http_bind_port(http_port_at(i), &err)) {
            /* No hint when we gave the primary away ourselves or an active
               instance of ours serves it - nothing for the user to do. */
            if (mayUsePrimary && !holderIsActiveInstance) {
                http_publish_notice(POS_HTTP_NOTICE_FALLBACK, http_port_at(i), holder);
            }
            else {
                http_publish_notice(POS_HTTP_NOTICE_NONE, http_port_at(i), 0);
            }
            return 1;
        }
    }

    http_bind_fail_log("bind", firstErr ? firstErr : err);
    http_publish_notice(POS_HTTP_NOTICE_NO_PORT, 0, holder);
    }
    return 0;
}

/* ---- listener thread --------------------------------------------------- */

static void http_close_listen_socket(void) {
    /* Take the socket atomically: the stopper and the listener thread may both
       get here at the same time; only the one that took it closes it. */
    const SOCKET s = (SOCKET)InterlockedExchangePointer(
        (PVOID volatile*)&g_listenSocket, (PVOID)INVALID_SOCKET);
    InterlockedExchange(&g_boundPort, 0);
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
}

/* 102: while we sit on the fallback port, get the primary port back (an older
   mod version only knows the primary):
     - an IDLE instance only takes it when it is free (plain bind);
     - an ACTIVE instance also asks the holder: an idle instance of this plugin
       hands it over, a frozen TeamSpeak may be ended (http_claim_primary).
       This covers "the instance that ended up on the fallback port is the one
       the user then connects to the Conan server".
   Returns 1 when the listener moved (the caller must not accept() on the
   socket it waited on). Listener thread only. */
static int http_try_return_to_primary(void) {
    const int primary = http_port_at(0);
    const int bound = (int)InterlockedCompareExchange(&g_boundPort, 0, 0);
    const int active = InterlockedCompareExchange(&g_instanceActive, 0, 0) != 0;
    int err = 0;
    SOCKET fresh;

    /* An active instance may come back at once: the 15 s after a handover only
       keep an IDLE instance from taking the port straight back. */
    if (bound == 0 || bound == primary || (!active && GetTickCount64() < g_avoidPrimaryUntilMs)) {
        return 0;
    }
    if (active) {
        HttpClaim claim;
        fresh = http_claim_primary(0, &claim);
    }
    else {
        fresh = http_make_listener(primary, &err);
    }
    if (fresh == INVALID_SOCKET) {
        return 0;
    }
    http_publish_listener(fresh, primary);
    log_write("HTTP: listening on port %d now (was on fallback port %d)", primary, bound);
    http_publish_notice(POS_HTTP_NOTICE_NONE, primary, 0);
    return 1;
}

static unsigned __stdcall http_server_thread(void* arg) {
    int failStreak = 0;      /* consecutive bind / hard accept failures; reset by a served client */
    int transientStreak = 0; /* consecutive transient accept errors */
    int announced = 0;       /* "listening" already logged once */
    ULONGLONG lastPrimaryTry = GetTickCount64();
    (void)arg;

    if (g_listenSocket != INVALID_SOCKET) {
        announced = 1;
        log_write("HTTP: listening on http://127.0.0.1:%d", (int)g_boundPort);
    }

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            break;
        }

        /* (Re)open the listen socket: first bind failed in start(), the socket
           was torn down after a hard accept() error, or the primary port was
           handed over to another instance (102). */
        if (g_listenSocket == INVALID_SOCKET) {
            if (!http_open_listen_socket(1)) {
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
            {
                const int port = (int)InterlockedCompareExchange(&g_boundPort, 0, 0);
                log_write("HTTP: listening%s on http://127.0.0.1:%d%s", announced ? " again" : "", port,
                    port == http_port_at(0) ? "" : " (fallback port)");
            }
            announced = 1;
            lastPrimaryTry = GetTickCount64();
        }

        /* Wait for a client at most one second, so the loop can look after the
           ports while nothing is sent (102). */
        {
            fd_set readable;
            struct timeval tv;
            const SOCKET ls = g_listenSocket;
            int ready;
            if (ls == INVALID_SOCKET) {
                continue; /* closed by stop() - the stop check above ends the loop */
            }
            FD_ZERO(&readable);
            FD_SET(ls, &readable);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            ready = select(0, &readable, NULL, NULL, &tv);
            if (GetTickCount64() - lastPrimaryTry >= 5000ULL) {
                lastPrimaryTry = GetTickCount64();
                if (http_try_return_to_primary()) {
                    continue; /* new socket: wait on that one */
                }
            }
            if (ready == 0) {
                continue; /* nobody connected within the second */
            }
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

        /* 102: this request was POST /v1/yield and we agreed - give the primary
           port away now. The top of the loop reopens on the fallback port; the
           primary stays off limits for a while so the asking instance gets it. */
        if (InterlockedExchange(&g_yieldPending, 0) != 0) {
            g_avoidPrimaryUntilMs = GetTickCount64() + (ULONGLONG)HTTP_YIELD_AVOID_MS;
            http_close_listen_socket();
        }
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

    /* 102: fresh state for this run (start/stop pairs within one process).
       "active" starts at 0: the callback thread sets it again with its next
       tick (a stale 1 would refuse to hand the port over while disconnected). */
    InterlockedExchange(&g_yieldPending, 0);
    InterlockedExchange(&g_notice, POS_HTTP_NOTICE_NONE);
    InterlockedExchange(&g_instanceActive, 0);
    InterlockedExchange(&g_endedFrozenPid, 0);
    g_noticeLast = POS_HTTP_NOTICE_NONE;
    g_avoidPrimaryUntilMs = 0;
    http_silent_holder_reset();

    /* A failed first bind is not fatal: the listener thread keeps retrying
       (port busy during a plugin restart, firewall hiccup, ...) so HTTP comes
       back without restarting TeamSpeak. Here (TS callback thread) only the
       primary port is tried, without waiting; asking the holder of a taken
       port and the fallback port are the listener thread's job (102). */
    if (!http_open_listen_socket(0)) {
        log_write("HTTP: port %d is not free yet — the listener thread asks the holder and "
            "falls back to port %d if needed", http_port_at(0), http_port_at(1));
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

/* ---- 102: fallback port + handover ------------------------------------- */

void pos_http_server_set_active(int active) {
    InterlockedExchange(&g_instanceActive, active ? 1 : 0);
}

void pos_http_server_set_version(const char* version) {
    g_pluginVersion = version ? version : "";
}

int pos_http_server_bound_port(void) {
    return (int)InterlockedCompareExchange(&g_boundPort, 0, 0);
}

int pos_http_server_take_notice(int* port, unsigned long* holderPid) {
    const long notice = InterlockedExchange(&g_notice, POS_HTTP_NOTICE_NONE);
    if (port) {
        *port = (int)InterlockedCompareExchange(&g_noticePort, 0, 0);
    }
    if (holderPid) {
        *holderPid = (unsigned long)InterlockedCompareExchange(&g_noticeHolder, 0, 0);
    }
    return (int)notice;
}

unsigned long pos_http_server_take_ended_frozen(void) {
    return (unsigned long)InterlockedExchange(&g_endedFrozenPid, 0);
}
