#ifndef CORE_HTTP_HTTP_PORTS_H
#define CORE_HTTP_HTTP_PORTS_H

/*
 * 102 — fallback ports and port handover between plugin instances.
 * Pure, header-only helpers (no sockets, no clock, no locks) so they are
 * host-unit-tested (tests/http_ports_test.c). The socket work lives in
 * pos_http_server.c.
 *
 * Why: two TeamSpeak processes on one PC both load the plugin. Only one can
 * bind 127.0.0.1:52734; the game mod then talks to whichever holds the port,
 * even when that instance is not the one connected to the Conan server.
 *
 *   Ports      52734 (primary, burned into every mod version) plus ONE
 *              fallback port (owner 2026-11-11: one extra address, so players
 *              confirm the game's consent question for a single new origin).
 *              The fallback lies outside the Windows dynamic port range
 *              (49152-65535), so one reserved block cannot cover both.
 *   Status     GET /v1/status -> {"ok":true,"service":"conan_exiles_ts",
 *              "act":0|1,"pid":N,"port":N,"ver":"x.y.z"}
 *              act = 1: this instance has an active Conan server profile.
 *              Every position reply carries "act" too.
 *   Handover   POST /v1/yield: an instance that holds the PRIMARY port and is
 *              NOT active closes its listener and moves to a fallback port, so
 *              the asking instance can bind the primary.
 *
 * Thread contract: none of its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef POS_HTTP_PORT_LIST
#define POS_HTTP_PORT_LIST { 52734, 42734 }
#endif
#define POS_HTTP_PORT_COUNT 2

#define HTTP_SERVICE_NAME "conan_exiles_ts"

/* Port by preference index (0 = primary); 0 for an index out of range. */
static inline int http_port_at(int index) {
    static const int ports[POS_HTTP_PORT_COUNT] = POS_HTTP_PORT_LIST;
    return (index >= 0 && index < POS_HTTP_PORT_COUNT) ? ports[index] : 0;
}

/* Preference index of one of our ports, -1 for a foreign port. */
static inline int http_port_index(int port) {
    for (int i = 0; i < POS_HTTP_PORT_COUNT; i++) {
        if (http_port_at(i) == port) {
            return i;
        }
    }
    return -1;
}

/* Integer value of  "key":N  in a flat JSON text. Returns 1 when found. */
static inline int http_json_int(const char* body, const char* key, long* out) {
    char token[40];
    if (!body || !key || !out
        || snprintf(token, sizeof(token), "\"%s\":", key) >= (int)sizeof(token)) {
        return 0;
    }
    const char* p = strstr(body, token);
    if (!p) {
        return 0;
    }
    p += strlen(token);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char* end = NULL;
    const long v = strtol(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Status body of this instance. Returns the length, 0 when it did not fit. */
static inline int http_status_json(char* out, int outLen, int act, unsigned long pid,
    int port, const char* version) {
    if (!out || outLen <= 0) {
        return 0;
    }
    const int n = snprintf(out, (size_t)outLen,
        "{\"ok\":true,\"service\":\"" HTTP_SERVICE_NAME "\",\"act\":%d,\"pid\":%lu,\"port\":%d,\"ver\":\"%s\"}",
        act ? 1 : 0, pid, port, version ? version : "");
    if (n <= 0 || n >= outLen) {
        out[0] = '\0';
        return 0;
    }
    return n;
}

typedef struct HttpPeerStatus {
    int isPlugin;      /* body names our service */
    int hasAct;        /* body carries "act" (plugin 8.0.23 or newer) */
    int act;           /* 1 = that instance has an active Conan server profile */
    unsigned long pid; /* its process id, 0 when unknown */
} HttpPeerStatus;

/* Parse the body another instance sent for GET /v1/status. A 404 body of an
   older plugin (or of a foreign program) leaves isPlugin = 0. */
static inline void http_status_parse(const char* body, HttpPeerStatus* out) {
    long v = 0;
    memset(out, 0, sizeof(*out));
    if (!body) {
        return;
    }
    out->isPlugin = strstr(body, "\"service\":\"" HTTP_SERVICE_NAME "\"") != NULL;
    if (http_json_int(body, "act", &v)) {
        out->hasAct = 1;
        out->act = (v != 0);
    }
    if (http_json_int(body, "pid", &v) && v > 0) {
        out->pid = (unsigned long)v;
    }
}

typedef enum HttpTakeover {
    HTTP_TAKEOVER_FALLBACK = 0, /* leave the holder alone, use a fallback port */
    HTTP_TAKEOVER_ASK_YIELD = 1 /* holder is an idle instance of ours: ask it for the port */
} HttpTakeover;

/* What a starting instance does when the primary port is taken. Only an
   instance that answered, is our plugin and says it is NOT active is asked. */
static inline HttpTakeover http_takeover_decide(int peerAnswered, const HttpPeerStatus* st) {
    if (peerAnswered && st && st->isPlugin && st->hasAct && !st->act) {
        return HTTP_TAKEOVER_ASK_YIELD;
    }
    return HTTP_TAKEOVER_FALLBACK;
}

/* What the HOLDER answers to POST /v1/yield: it gives away only the primary
   port and only while it has no active Conan server profile. */
static inline int http_yield_allowed(int boundPort, int active) {
    return boundPort != 0 && boundPort == http_port_at(0) && !active;
}

/* Insert  ,"act":N  before the closing brace of a JSON object reply.
   Returns 1 when done, 0 when the reply is no object or the buffer is full
   (the reply is then left untouched). */
static inline int http_reply_append_act(char* reply, int size, int act) {
    if (!reply || size <= 0) {
        return 0;
    }
    const int len = (int)strlen(reply);
    if (len < 2 || reply[len - 1] != '}') {
        return 0;
    }
    /* An empty object "{}" gets no leading comma. */
    const int empty = (reply[len - 2] == '{');
    const char* add = act ? ",\"act\":1}" : ",\"act\":0}";
    if (empty) {
        add++;
    }
    const int addLen = (int)strlen(add);
    if (len - 1 + addLen >= size) {
        return 0;
    }
    memcpy(reply + len - 1, add, (size_t)addLen + 1);
    return 1;
}

#endif /* CORE_HTTP_HTTP_PORTS_H */
