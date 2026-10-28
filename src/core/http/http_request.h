#ifndef CORE_HTTP_HTTP_REQUEST_H
#define CORE_HTTP_HTTP_REQUEST_H

/*
 * Pure helpers for the localhost position server (pos_http_server.c).
 *
 * Everything that can be decided from bytes and numbers alone lives here so
 * the host unit tests (tests/http_request_test.c) can exercise it without
 * Winsock:
 *   - finding the end of the header block, reading single headers,
 *   - the browser guard (Origin / Sec-Fetch-Site / Host),
 *   - Content-Length parsing and body-length clamping,
 *   - log throttling with a "suppressed" counter,
 *   - accept() error classification and the rebind backoff.
 *
 * Why a guard: the listener answers on 127.0.0.1 without any credentials.
 * A browser on the same PC can reach that address too - any web page can do
 *   fetch("http://127.0.0.1:52734/v1/position", {method:"POST", mode:"no-cors", ...})
 * ("simple" requests need no preflight), and with DNS rebinding a page can
 * even READ the answers.  The game mod (Conan Exiles Enhanced "Start Http
 * Request" node) is NOT a browser: it sends no Sec-Fetch-* headers, CEE sets
 * Host itself from the URL (127.0.0.1:52734), and if CEE sends an Origin at
 * all it is its own approved origin http://127.0.0.1:52734.  Browsers always
 * send Origin on a cross-site POST (never our own origin - the plugin serves
 * no web pages) and a non-local Host after rebinding, so those requests can
 * be told apart reliably.
 *
 * Thread contract: header-only, no state except what the caller passes in.
 * Callable from any thread.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ASCII helpers ------------------------------------------------------ */

static inline int http_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

/* 1 when a[0..aLen) equals the NUL-terminated literal b (ASCII, ignoring case). */
static inline int http_ieq(const char* a, int aLen, const char* b) {
    int i;
    for (i = 0; i < aLen; i++) {
        if (b[i] == '\0'
            || http_lower((unsigned char)a[i]) != http_lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return b[i] == '\0';
}

/* ---- header block -------------------------------------------------------- */

/* Index just behind the first "\r\n\r\n" in buf[0..len), or -1 when the header
   block is not complete yet. */
static inline int http_find_header_end(const char* buf, int len) {
    int i;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

/* Reads the nth (0-based) header called `name` (ASCII case-insensitive) from
   the header block head[0..headLen) - the request line is skipped, the body
   behind the header block is never looked at.  The value is trimmed and
   copied (truncated) into out; *outFullLen (optional) receives the untruncated
   length.  Returns 1 when such a header exists.  Obsolete folded lines
   (starting with blank/tab) are ignored. */
static inline int http_header_get(const char* head, int headLen, const char* name,
    int nth, char* out, int outSize, int* outFullLen) {
    int pos = 0;
    int found = 0;

    if (out && outSize > 0) {
        out[0] = '\0';
    }
    if (outFullLen) {
        *outFullLen = 0;
    }
    if (!head || headLen <= 0 || !name) {
        return 0;
    }

    /* skip the request line */
    while (pos < headLen && head[pos] != '\n') {
        pos++;
    }
    if (pos < headLen) {
        pos++;
    }

    while (pos < headLen) {
        const int ls = pos;
        int le = ls;
        int lend;
        int colon;
        int nameEnd;

        while (le < headLen && head[le] != '\n') {
            le++;
        }
        pos = (le < headLen) ? le + 1 : le;
        lend = le;
        if (lend > ls && head[lend - 1] == '\r') {
            lend--;
        }
        if (lend == ls || head[ls] == ' ' || head[ls] == '\t') {
            continue; /* blank or folded line */
        }

        colon = ls;
        while (colon < lend && head[colon] != ':') {
            colon++;
        }
        if (colon >= lend) {
            continue;
        }
        nameEnd = colon;
        while (nameEnd > ls && (head[nameEnd - 1] == ' ' || head[nameEnd - 1] == '\t')) {
            nameEnd--;
        }
        if (!http_ieq(head + ls, nameEnd - ls, name)) {
            continue;
        }
        if (found++ < nth) {
            continue;
        }

        {
            int vs = colon + 1;
            int ve = lend;
            int n;
            while (vs < ve && (head[vs] == ' ' || head[vs] == '\t')) {
                vs++;
            }
            while (ve > vs && (head[ve - 1] == ' ' || head[ve - 1] == '\t')) {
                ve--;
            }
            n = ve - vs;
            if (outFullLen) {
                *outFullLen = n;
            }
            if (out && outSize > 0) {
                int c = (n < outSize - 1) ? n : outSize - 1;
                memcpy(out, head + vs, (size_t)c);
                out[c] = '\0';
            }
        }
        return 1;
    }
    return 0;
}

/* ---- browser guard -------------------------------------------------------- */

#define HTTP_GUARD_OK          0
#define HTTP_GUARD_ORIGIN      1 /* Origin present and not our own origin (incl. "null") */
#define HTTP_GUARD_FETCH_SITE  2 /* Sec-Fetch-Site: cross-site or same-site */
#define HTTP_GUARD_HOST        3 /* Host present and not a loopback name */

/* Header name for the log line of a rejected request. */
static inline const char* http_guard_header(int guard) {
    switch (guard) {
    case HTTP_GUARD_ORIGIN:     return "Origin";
    case HTTP_GUARD_FETCH_SITE: return "Sec-Fetch-Site";
    case HTTP_GUARD_HOST:       return "Host";
    default:                    return "";
    }
}

/* 1 when a Host header value names this PC: 127.0.0.1, localhost or [::1],
   optionally followed by ":<port>" (port digits only, any port). */
static inline int http_host_is_local(const char* host) {
    const char* rest;
    int nameLen;
    int digits = 0;

    if (!host) {
        return 0;
    }
    if (host[0] == '[') {
        const char* close = strchr(host, ']');
        if (!close) {
            return 0;
        }
        nameLen = (int)(close - host) + 1;
        rest = close + 1;
    }
    else {
        const char* colon = strchr(host, ':');
        nameLen = colon ? (int)(colon - host) : (int)strlen(host);
        rest = host + nameLen;
    }
    if (!(http_ieq(host, nameLen, "127.0.0.1")
        || http_ieq(host, nameLen, "localhost")
        || http_ieq(host, nameLen, "[::1]"))) {
        return 0;
    }
    if (*rest == '\0') {
        return 1;
    }
    if (*rest != ':') {
        return 0;
    }
    rest++;
    while (*rest >= '0' && *rest <= '9') {
        rest++;
        digits++;
    }
    return *rest == '\0' && digits <= 5;
}

/* 1 when an Origin header value is exactly OUR origin: http://127.0.0.1:<port>
   or http://localhost:<port> (scheme and host ignoring case, port exact).
   CEE may send its approved origin; no web page can ever have this origin
   because the plugin serves no pages. */
static inline int http_origin_is_own(const char* origin, int port) {
    char expect[48];
    const int len = origin ? (int)strlen(origin) : 0;

    if (!origin) {
        return 0;
    }
    snprintf(expect, sizeof(expect), "http://127.0.0.1:%d", port);
    if (http_ieq(origin, len, expect)) {
        return 1;
    }
    snprintf(expect, sizeof(expect), "http://localhost:%d", port);
    return http_ieq(origin, len, expect);
}

/* Browser guard for ONE request (every endpoint).  head[0..headLen) is the
   header block; port is the port this listener serves.  Returns HTTP_GUARD_OK
   or the reason; `what` (optional) receives the offending header value for
   the log.
   Rules:
     - Origin absent                        -> ok
     - Origin == http://127.0.0.1:<port> or http://localhost:<port> -> ok
     - any other Origin (even "null")       -> reject
     - Sec-Fetch-Site cross-site / same-site-> reject; absent, none,
                                               same-origin -> ok
     - Host absent                          -> ok (HTTP/1.0 style clients)
     - Host 127.0.0.1 / localhost / [::1] (port optional) -> ok
     - any other Host                       -> reject (DNS rebinding) */
static inline int http_request_guard(const char* head, int headLen, int port,
    char* what, int whatSize) {
    char v[128];
    int full = 0;
    int i;

    if (what && whatSize > 0) {
        what[0] = '\0';
    }

    for (i = 0; i < 8
        && http_header_get(head, headLen, "Origin", i, v, (int)sizeof(v), &full); i++) {
        if (full >= (int)sizeof(v) || !http_origin_is_own(v, port)) {
            if (what && whatSize > 0) {
                snprintf(what, (size_t)whatSize, "%s", v);
            }
            return HTTP_GUARD_ORIGIN;
        }
    }
    for (i = 0; i < 8
        && http_header_get(head, headLen, "Sec-Fetch-Site", i, v, (int)sizeof(v), &full); i++) {
        if ((full == 10 && http_ieq(v, 10, "cross-site"))
            || (full == 9 && http_ieq(v, 9, "same-site"))) {
            if (what && whatSize > 0) {
                snprintf(what, (size_t)whatSize, "%s", v);
            }
            return HTTP_GUARD_FETCH_SITE;
        }
    }
    for (i = 0; i < 8
        && http_header_get(head, headLen, "Host", i, v, (int)sizeof(v), &full); i++) {
        if (full >= (int)sizeof(v) || !http_host_is_local(v)) {
            if (what && whatSize > 0) {
                snprintf(what, (size_t)whatSize, "%s", v);
            }
            return HTTP_GUARD_HOST;
        }
    }
    return HTTP_GUARD_OK;
}

/* ---- Content-Length / body ------------------------------------------------ */

#define HTTP_CL_INVALID (-1) /* present but not a plain decimal number / conflicting duplicates */
#define HTTP_CL_ABSENT    0  /* no Content-Length header */
#define HTTP_CL_OK        1  /* *outLen holds the length */
#define HTTP_CL_TOO_BIG   2  /* valid, but larger than maxBody (does not fit the buffer) */

/* Content-Length from the header block only (never from the body, never from
   "X-Content-Length"): digits only, strtol with range check. */
static inline int http_content_length(const char* head, int headLen, long maxBody, long* outLen) {
    char v[32];
    char v2[32];
    int full = 0;
    int full2 = 0;
    int i;
    char* end = NULL;
    long n;

    if (outLen) {
        *outLen = 0;
    }
    if (!http_header_get(head, headLen, "Content-Length", 0, v, (int)sizeof(v), &full)) {
        return HTTP_CL_ABSENT;
    }
    if (http_header_get(head, headLen, "Content-Length", 1, v2, (int)sizeof(v2), &full2)) {
        if (full != full2 || strcmp(v, v2) != 0) {
            return HTTP_CL_INVALID; /* conflicting duplicates */
        }
    }
    if (full <= 0 || full >= (int)sizeof(v)) {
        return HTTP_CL_INVALID;
    }
    for (i = 0; i < full; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return HTTP_CL_INVALID;
        }
    }
    errno = 0;
    n = strtol(v, &end, 10);
    if (errno == ERANGE || end == v || *end != '\0' || n < 0) {
        return HTTP_CL_INVALID;
    }
    if (outLen) {
        *outLen = n;
    }
    return (n > maxBody) ? HTTP_CL_TOO_BIG : HTTP_CL_OK;
}

/* Body bytes the handler may look at.  With a Content-Length: exactly that
   many (never more than what really arrived); without: what arrived with the
   headers.  Never negative, so a stale buffer is never read past `available`. */
static inline int http_body_len(int haveContentLength, long contentLength, int available) {
    long n = haveContentLength ? contentLength : (long)available;
    if (n > (long)available) {
        n = (long)available;
    }
    return (n < 0) ? 0 : (int)n;
}

/* 1 when the body is still short of the announced Content-Length. */
static inline int http_body_incomplete(int haveContentLength, long contentLength, int available) {
    return haveContentLength && (long)available < contentLength;
}

/* ---- log throttle ---------------------------------------------------------- */

typedef struct HttpThrottle {
    int armed;                 /* 0 until the first line went out */
    unsigned long long last;   /* tick of the last line that went out */
    int suppressed;            /* lines swallowed since then */
} HttpThrottle;

/* 1 when a line may be logged now (at most one per intervalMs).  *suppressed
   receives how many lines were swallowed since the previous one. */
static inline int http_throttle_pass(HttpThrottle* t, unsigned long long now,
    unsigned long long intervalMs, int* suppressed) {
    if (suppressed) {
        *suppressed = 0;
    }
    if (t->armed && now - t->last < intervalMs) {
        if (t->suppressed < 1000000) {
            t->suppressed++;
        }
        return 0;
    }
    t->armed = 1;
    t->last = now;
    if (suppressed) {
        *suppressed = t->suppressed;
    }
    t->suppressed = 0;
    return 1;
}

/* " (+N suppressed)" or "" - appended to a throttled log line. */
static inline void http_suppressed_text(char* out, int outSize, int suppressed) {
    if (!out || outSize <= 0) {
        return;
    }
    if (suppressed > 0) {
        snprintf(out, (size_t)outSize, " (+%d suppressed)", suppressed);
    }
    else {
        out[0] = '\0';
    }
}

/* ---- accept() recovery -------------------------------------------------------- */

#define HTTP_WSAEINTR        10004
#define HTTP_WSAEWOULDBLOCK  10035
#define HTTP_WSAECONNABORTED 10053
#define HTTP_WSAECONNRESET   10054

/* accept() errors that only concern ONE incoming connection (the client went
   away before we took it).  The listen socket itself is fine - keep going. */
static inline int http_wsa_error_is_transient(int err) {
    return err == HTTP_WSAECONNRESET || err == HTTP_WSAECONNABORTED
        || err == HTTP_WSAEINTR || err == HTTP_WSAEWOULDBLOCK;
}

#define HTTP_BACKOFF_FIRST_MS 250
#define HTTP_BACKOFF_MAX_MS   5000

/* Wait before the next rebind attempt after `streak` consecutive failures
   (1 = first): 250, 500, 1000, 2000, 4000, 5000, 5000 ... */
static inline unsigned long http_backoff_ms(int streak) {
    unsigned long ms = HTTP_BACKOFF_FIRST_MS;
    int i;
    for (i = 1; i < streak && ms < HTTP_BACKOFF_MAX_MS; i++) {
        ms *= 2UL;
    }
    return (ms > HTTP_BACKOFF_MAX_MS) ? HTTP_BACKOFF_MAX_MS : ms;
}

#endif /* CORE_HTTP_HTTP_REQUEST_H */
