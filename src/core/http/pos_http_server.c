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
 *     Successful POST /v1/position → one log_write line (always visible).
 *     Dropped connections (recv fail / incomplete headers) → throttled drop log
 *     (~1 Hz max). Per-connection recv timeout: 100 ms (fail-fast).
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

#include "core/http/pos_http_server.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"

/* ---- constants --------------------------------------------------------- */

#define HTTP_BUF_SIZE    4096   /* header + body combined recv buffer */
#define HTTP_BODY_MAX    1024   /* max body bytes we care about */
#define HTTP_RECV_TMO_MS 100    /* per-connection recv timeout (fail-fast) */

/* ---- module state ------------------------------------------------------ */

static SOCKET        g_listenSocket = INVALID_SOCKET;
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

/* Drop log throttle — HTTP listener thread only (sequential accept+handle). */
static ULONGLONG g_httpLastDropLog  = 0;

/* ---- helpers ----------------------------------------------------------- */

/* Throttled drop log (~1 Hz max) for recv failures and incomplete headers. */
static void http_drop_log(const char* reason, int err) {
    const ULONGLONG now = GetTickCount64();

    if (g_httpLastDropLog != 0 && (now - g_httpLastDropLog) < 1000ULL) {
        return;
    }
    g_httpLastDropLog = now;

    if (reason && strcmp(reason, "recv") == 0) {
        log_write("HTTP: drop reason=recv err=%d", err);
    }
    else {
        log_write("HTTP: drop reason=%s", reason ? reason : "unknown");
    }
}

/* Send a complete HTTP/1.1 response and close the connection. */
static void http_send(SOCKET s, int status, const char* body) {
    char resp[512];
    const char* status_text =
        status == 200 ? "OK" :
        status == 400 ? "Bad Request" : "Not Found";
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
        out->seq = (int)seq;
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
        out->seq = (int)seq;
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

/* Log raw inbound request. 200 → log_debug; 400/404 → log_write. */
static void http_log_raw_in(const char* method, const char* path,
    const char* body, int bodyLen, int httpStatus) {
    char raw[420];

    if (!method || !path) {
        return;
    }
    http_format_raw(body, bodyLen, raw, (int)sizeof(raw));

    if (httpStatus == 200) {
        log_debug("HTTP: IN %s %s status=%d cl=%d raw=\"%s\"",
            method, path, httpStatus, bodyLen, raw);
    }
    else {
        log_write("HTTP: IN %s %s status=%d cl=%d raw=\"%s\"",
            method, path, httpStatus, bodyLen, raw);
    }
}

/* Update POST rate stats; emit ~1 Hz summary via log_write. */
static void http_post_rate_tick(int injected, ULONGLONG dtMs) {
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
    }
    else {
        g_httpWindowFail++;
    }

    if (now - g_httpWindowStart >= 1000ULL) {
        const ULONGLONG dtMinOut = (g_httpDtMin == MAXULONGLONG) ? 0ULL : g_httpDtMin;
        const ULONGLONG dtAvgOut = (g_httpDtSamples > 0)
            ? (g_httpDtSum / (ULONGLONG)g_httpDtSamples) : 0ULL;

        log_write("HTTP: rate n=%d/s dt=min/avg/max=%llu/%llu/%llums ok=%d fail=%d",
            g_httpWindowCount,
            (unsigned long long)dtMinOut,
            (unsigned long long)dtAvgOut,
            (unsigned long long)g_httpDtMax,
            g_httpWindowOk,
            g_httpWindowFail);

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

/* Handle one accepted client connection. */
static void http_handle_client(SOCKET client) {
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
        const int n = recv(client, buf + total, (int)(sizeof(buf) - 1 - total), 0);
        if (n <= 0) {
            http_drop_log("recv", (n < 0) ? (int)WSAGetLastError() : 0);
            return;
        }
        total += n;
        buf[total] = '\0';
        const char* hdr_sep = strstr(buf, "\r\n\r\n");
        if (hdr_sep) {
            header_end = (int)(hdr_sep - buf) + 4;
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

        /* Determine body length from Content-Length header. */
        int content_length = 0;
        const char* cl = strstr(buf, "Content-Length:");
        if (!cl) {
            cl = strstr(buf, "content-length:");
        }
        if (cl) {
            content_length = atoi(cl + 15);
            if (content_length < 0) {
                content_length = 0;
            }
        }

        /* Read remaining body bytes if not yet in buffer. */
        int body_have = total - header_end;
        while (body_have < content_length && total < (int)(sizeof(buf) - 1)) {
            const int n = recv(client, buf + total, (int)(sizeof(buf) - 1 - total), 0);
            if (n <= 0) {
                break;
            }
            total += n;
            body_have += n;
        }
        buf[total] = '\0';

        {
            const char* body = buf + header_end;
            const int bodyLen = (content_length > 0) ? content_length : (total - header_end);
            PosSample sample;
            const int parsed = http_parse_body(body, &sample);
            const int injected = parsed && pos_inject_sample(&sample);
            const int status = injected ? 200 : 400;
            const ULONGLONG now = GetTickCount64();
            ULONGLONG dtMs = 0;

            if (g_httpLastTick != 0) {
                dtMs = now - g_httpLastTick;
            }

            http_post_rate_tick(injected, dtMs);

            if (injected) {
                log_write("HTTP: POST seq=%d pos=X=%.6f Y=%.6f Z=%.6f YAW=%.6f YAWY=%.6f dt=%llums status=%d",
                    sample.seq,
                    sample.x, sample.y, sample.z,
                    sample.yaw, sample.yawY,
                    (unsigned long long)dtMs,
                    status);
            }
            else if (!parsed) {
                char raw[420];
                http_format_raw(body, bodyLen, raw, (int)sizeof(raw));
                log_write("HTTP: POST status=400 reason=parse cl=%d raw=\"%s\"",
                    bodyLen, raw);
            }

            if (parsed && !injected) {
                char raw[420];
                http_format_raw(body, bodyLen, raw, (int)sizeof(raw));
                log_write("HTTP: POST status=400 reason=reject seq=%d pos=X=%.6f Y=%.6f Z=%.6f cl=%d raw=\"%s\"",
                    sample.seq,
                    sample.x, sample.y, sample.z,
                    bodyLen, raw);
            }

            if (injected) {
                http_send(client, 200, "{\"ok\":true}");
            }
            else {
                http_send(client, 400, "{\"ok\":false,\"error\":\"invalid position\"}");
            }
        }
        return;
    }

    http_log_raw_in(method, path, "", 0, 404);
    http_send(client, 404, "{\"ok\":false,\"error\":\"not found\"}");
}

/* ---- listener thread --------------------------------------------------- */

static unsigned __stdcall http_server_thread(void* arg) {
    (void)arg;
    log_write("HTTP: listening on " POS_HTTP_BASE_URL);

    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_TIMEOUT) {
            break;
        }
        struct sockaddr_in client_addr;
        int addr_len = (int)sizeof(client_addr);
        SOCKET client = accept(g_listenSocket,
            (struct sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            /* Stop was signalled (socket closed) or accept error. */
            break;
        }
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

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSocket == INVALID_SOCKET) {
        log_write("HTTP: socket() failed (%d)", WSAGetLastError());
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    /* Allow quick restart after plugin reload. */
    const int opt = 1;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&opt, (int)sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */
    addr.sin_port        = htons(POS_HTTP_PORT);

    if (bind(g_listenSocket, (struct sockaddr*)&addr, (int)sizeof(addr))
            == SOCKET_ERROR) {
        log_write("HTTP: bind() failed (%d) — port %d already in use?",
            WSAGetLastError(), POS_HTTP_PORT);
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    if (listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        log_write("HTTP: listen() failed (%d)", WSAGetLastError());
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) {
        log_write("HTTP: CreateEvent failed");
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        InterlockedExchange(&g_httpRunning, 0);
        return;
    }

    g_httpThread = (HANDLE)_beginthreadex(NULL, 0, http_server_thread,
        NULL, 0, NULL);
    if (!g_httpThread) {
        log_write("HTTP: _beginthreadex failed");
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
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
    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
    if (g_httpThread) {
        if (WaitForSingleObject(g_httpThread, 5000) != WAIT_OBJECT_0) {
            log_write("HTTP: server thread slow to exit — waiting");
            WaitForSingleObject(g_httpThread, INFINITE);
        }
        CloseHandle(g_httpThread);
        g_httpThread = NULL;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = NULL;
    }

    WSACleanup();
    InterlockedExchange(&g_httpRunning, 0);
}
