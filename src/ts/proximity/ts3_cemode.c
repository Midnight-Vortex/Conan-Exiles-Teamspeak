#include "ts/proximity/ts3_cemode.h"

#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_cemode_wire.h"
#include "ts/proximity/ts3_client_limits.h"
#include "core/voice/voice_modes.h"
#include "core/util/log.h"
#include "core/util/poll_interval.h"
#include "plugin.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Same 30 ms floor as CEPOS / CEDRAIN. Edge-triggered; this only caps bursts. */
#define CEMODE_SEND_MIN_MS      PLUGIN_POLL_INTERVAL_MS

/* One slot per client ID (sparse, bounded by TS3_MAX_CLIENT_ID). Written by
   the callback thread, read by the info panel — which TeamSpeak may call from
   its UI thread, so the table needs a lock (same reasoning as the CEVER map
   in ts3_plugin_version.c). The lock is held only for the struct copy. */
typedef struct CemodePeer {
    float distance;         /* meters, as announced by that peer */
    unsigned char mode;     /* VoiceMode value */
    unsigned char known;    /* 1 = we received at least one CEMODE */
    unsigned char replied;  /* 1 = we already answered this peer's first CEMODE */
} CemodePeer;

static CemodePeer g_peers[TS3_MAX_CLIENT_ID];
static CRITICAL_SECTION g_peersLock;
static INIT_ONCE g_peersLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cemode_lock_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_peersLock);
    return TRUE;
}

static void cemode_lock_ensure(void) {
    InitOnceExecuteOnce(&g_peersLockOnce, cemode_lock_init, NULL, NULL);
}

/* Set from any thread (mode switch), consumed on the callback thread. */
static volatile long g_sendPending = 0;

/* Send state — TS callback thread only. */
static ULONGLONG g_lastSendMs = 0;

static const char* cemode_mode_name(unsigned char mode) {
    switch (mode) {
    case VOICE_MODE_WHISPER: return "Whisper";
    case VOICE_MODE_SHOUT:   return "Shout";
    case VOICE_MODE_NORMAL:
    default:                 return "Normal";
    }
}

/* Accept our own plugin ID plus the legacy IDs, so a mixed old/new server
   still exchanges what it can (old clients simply never send CEMODE). */
static int cemode_plugin_name_matches(const char* pluginName) {
    const char* registered;

    if (!pluginName || !pluginName[0]) {
        return 0;
    }
    registered = ts3_get_plugin_id();
    if (registered && registered[0] && strcmp(pluginName, registered) == 0) {
        return 1;
    }
    return strcmp(pluginName, "conan_exiles") == 0
        || strcmp(pluginName, "conan_exiles_ts") == 0;
}

/* ---- send ------------------------------------------------------------------ */

void ts3_cemode_signal_send_pending(void) {
    /* One CEDRAIN round trip per pending cycle — re-armed by the flush when
       the rate limit made it skip a send. */
    if (InterlockedCompareExchange(&g_sendPending, 1, 0) == 0) {
        ts3_request_wakeup();
    }
}

int ts3_cemode_send_pending(void) {
    return InterlockedCompareExchange(&g_sendPending, 0, 0) != 0;
}

void ts3_cemode_flush(void) {
    char command[CEMODE_CMD_MAX];
    ULONGLONG now;
    long distanceDm;

    if (!ts3_thread_is_callback() || !ts3_is_connected() || pluginShuttingDown) {
        return;
    }
    if (InterlockedCompareExchange(&g_sendPending, 0, 0) == 0) {
        return;
    }

    now = GetTickCount64();
    if (g_lastSendMs != 0 && now - g_lastSendMs < CEMODE_SEND_MIN_MS) {
        ts3_request_wakeup(); /* retry after the rate limit */
        return;
    }

    distanceDm = cemode_wire_distance_dm(voice_mode_get_current_distance());
    if (!cemode_wire_format((int)voice_mode_get_current(), distanceDm,
            command, sizeof(command))) {
        InterlockedExchange(&g_sendPending, 0); /* unsendable — do not spin */
        return;
    }

    if (ts3_send_plugin_command_server(command)) {
        g_lastSendMs = now;
        log_debug("CEMODE: SEND %s", command);
    }
    InterlockedExchange(&g_sendPending, 0);
}

/* ---- receive --------------------------------------------------------------- */

int ts3_cemode_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID) {
    int mode = 0;
    long distanceDm = 0;
    CemodePeer* peer;
    int needReply;

    if (!pluginCommand || pluginShuttingDown) {
        return 0;
    }
    if (strncmp(pluginCommand, CEMODE_CMD_PREFIX, CEMODE_PREFIX_LEN) != 0) {
        return 0;
    }
    if (!cemode_plugin_name_matches(pluginName) || !ts3_thread_is_callback()) {
        return 1;
    }
    /* Our own broadcast comes back to us — nothing to learn from it. */
    if (invokerClientID != 0 && invokerClientID == ts3_get_local_client_id()) {
        return 1;
    }
    if (!ts3_client_id_valid(invokerClientID)) {
        return 1;
    }
    if (!cemode_wire_parse(pluginCommand + CEMODE_PREFIX_LEN, &mode, &distanceDm)) {
        return 1;
    }

    cemode_lock_ensure();
    EnterCriticalSection(&g_peersLock);
    peer = &g_peers[invokerClientID];
    peer->mode = (unsigned char)mode;
    peer->distance = (float)distanceDm / 10.0f;
    peer->known = 1;
    /* First contact: answer once so the newcomer learns our mode too. */
    needReply = !peer->replied;
    peer->replied = 1;
    LeaveCriticalSection(&g_peersLock);

    /* The reply is coalesced into the next CEDRAIN, so N joining peers cost
       one broadcast, not N. */
    if (needReply) {
        ts3_cemode_signal_send_pending();
    }
    return 1;
}

/* ---- lifecycle + display ---------------------------------------------------- */

void ts3_cemode_clear_client(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    cemode_lock_ensure();
    EnterCriticalSection(&g_peersLock);
    memset(&g_peers[clientID], 0, sizeof(g_peers[clientID]));
    LeaveCriticalSection(&g_peersLock);
}

void ts3_cemode_reset(void) {
    cemode_lock_ensure();
    EnterCriticalSection(&g_peersLock);
    memset(g_peers, 0, sizeof(g_peers));
    LeaveCriticalSection(&g_peersLock);
    g_lastSendMs = 0;
    InterlockedExchange(&g_sendPending, 0);
}

int ts3_cemode_format_peer(anyID clientID, char* buf, size_t bufSize) {
    CemodePeer peer;
    int written;

    if (!buf || bufSize < 32 || !ts3_client_id_valid(clientID)) {
        return 0;
    }

    cemode_lock_ensure();
    EnterCriticalSection(&g_peersLock);
    peer = g_peers[clientID];
    LeaveCriticalSection(&g_peersLock);

    if (!peer.known) {
        return 0;
    }
    written = snprintf(buf, bufSize, "Sprechmodus: %s (%.0f m)",
        cemode_mode_name(peer.mode), peer.distance);
    return written > 0 ? written : 0;
}
