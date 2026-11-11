/* 102 — two-process test for the port handover of pos_http_server.c (real
   sockets on 127.0.0.1, no TeamSpeak, no game).

   This program is ONE plugin instance: it starts the real listener with
   stubbed neighbours (position store, occlusion, talk state, log) and runs for
   a number of seconds. The driver script tests/http_takeover_itest.ps1 starts
   several of them and checks who ends up on which port.

   It uses TEST ports (47834 / 47835), never the real 52734, so a running
   TeamSpeak with the plugin is not touched.

   Build (MSVC Developer prompt, from the repo root):
     cl /nologo /W4 /O2 /I. /Isrc /DPOS_HTTP_PORT_LIST="{47834,47835}" ^
        tests\http_takeover_itest.c src\core\http\pos_http_server.c ^
        /Fe:tests\out\http_takeover_itest.exe ws2_32.lib iphlpapi.lib

   Usage: http_takeover_itest.exe <label> <active 0|1> <seconds> [becomeActiveAfterSeconds]
   Prints one line per event to stdout:  <label> <text>                          */

#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/http/http_ports.h"
#include "core/http/pos_http_server.h"
#include "core/hub/mod_password.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/occlusion.h"
#include "core/proximity/talk_state.h"
#include "core/util/log.h"

static const char* g_label = "?";

/* ---- stubs for everything the listener calls ----------------------------- */

void log_write(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s LOG %s\n", g_label, line);
    fflush(stdout);
}

void log_debug(const char* fmt, ...) {
    (void)fmt;
}

int pos_inject_sample(const PosSample* sample) {
    return sample != NULL;
}

int mod_password_take_from_body(char* body, int bodyLen, unsigned long long nowMs) {
    (void)body;
    (void)nowMs;
    return bodyLen;
}

int occlusion_is_enabled(void) {
    return 0;
}

int occlusion_store_from_text(const char* text, unsigned long long nowMs) {
    (void)text;
    (void)nowMs;
    return -1;
}

int occlusion_format_probes(char* out, int outSize, float x, float y, float z) {
    (void)out;
    (void)outSize;
    (void)x;
    (void)y;
    (void)z;
    return 0;
}

int occlusion_append_cfg_json(char* out, int outSize) {
    (void)out;
    (void)outSize;
    return 0;
}

int talk_state_append_json(char* out, int outSize, float localX, float localY, float localZ,
    unsigned long long nowMs) {
    (void)out;
    (void)outSize;
    (void)localX;
    (void)localY;
    (void)localZ;
    (void)nowMs;
    return 0;
}

/* ---- main --------------------------------------------------------------- */

/* "HOLD <port> <seconds>": a program that keeps a port and never answers - with
   the same executable file as the instance under test, i.e. what a frozen
   TeamSpeak looks like to the plugin. */
static int hold_port(int port, int seconds) {
    WSADATA wsa;
    struct sockaddr_in addr;
    SOCKET s;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 3;
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr*)&addr, (int)sizeof(addr)) == SOCKET_ERROR
        || listen(s, SOMAXCONN) == SOCKET_ERROR) {
        printf("HOLD FAILED %d\n", WSAGetLastError());
        fflush(stdout);
        return 4;
    }
    printf("HOLD PORT %d pid=%lu\n", port, (unsigned long)GetCurrentProcessId());
    fflush(stdout);
    Sleep((DWORD)seconds * 1000);
    printf("HOLD EXIT\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    int active;
    int seconds;
    int activeAfter = -1;
    int restartAfter = -1;
    int lastPort = -1;

    if (argc < 4) {
        printf("usage: %s <label> <active 0|1> <seconds> [becomeActiveAfterSeconds|-1] [restartAfterSeconds]\n"
               "       %s HOLD <port> <seconds>\n", argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "HOLD") == 0) {
        return hold_port(atoi(argv[2]), atoi(argv[3]));
    }
    g_label = argv[1];
    active = atoi(argv[2]) != 0;
    seconds = atoi(argv[3]);
    if (argc >= 5) {
        activeAfter = atoi(argv[4]);
    }
    if (argc >= 6) {
        restartAfter = atoi(argv[5]);
    }

    pos_http_server_set_version("itest");
    pos_http_server_start();
    pos_http_server_set_active(active); /* like the plugin: the callback thread sets it after start() */

    for (int tick = 0; tick < seconds * 10; tick++) {
        const int port = pos_http_server_bound_port();
        int noticePort = 0;
        unsigned long holder = 0;
        const int notice = pos_http_server_take_notice(&noticePort, &holder);
        const unsigned long ended = pos_http_server_take_ended_frozen();
        if (port != lastPort) {
            lastPort = port;
            printf("%s PORT %d\n", g_label, port);
            fflush(stdout);
        }
        if (notice != POS_HTTP_NOTICE_NONE) {
            printf("%s NOTICE %d port=%d holder=%s\n", g_label, notice, noticePort, holder ? "known" : "unknown");
            fflush(stdout);
        }
        if (ended != 0) {
            printf("%s ENDED %lu\n", g_label, ended);
            fflush(stdout);
        }
        if (activeAfter >= 0 && tick == activeAfter * 10) {
            active = 1;
            pos_http_server_set_active(1);
            printf("%s ACTIVE 1\n", g_label);
            fflush(stdout);
        }
        if (restartAfter >= 0 && tick == restartAfter * 10) {
            /* plugin disable + enable inside one process */
            pos_http_server_stop();
            printf("%s RESTART\n", g_label);
            fflush(stdout);
            lastPort = -1;
            pos_http_server_start();
            pos_http_server_set_active(active);
        }
        Sleep(100);
    }

    pos_http_server_stop();
    printf("%s EXIT port=%d\n", g_label, lastPort);
    fflush(stdout);
    return 0;
}
