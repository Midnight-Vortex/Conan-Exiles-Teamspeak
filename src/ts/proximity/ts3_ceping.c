#include "ts/proximity/ts3_ceping.h"

#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_ceping_wire.h"
#include "ts/proximity/ts3_client_limits.h"
#include "core/util/log.h"
#include "plugin.h"

#include <windows.h>
#include <string.h>

/* One heartbeat per second is enough to spot lost update bursts without adding
   noticeable traffic (CEPOS already runs at up to ~30 Hz while moving). */
#define CEPING_SEND_MIN_MS      1000

/* Per-client last seen sequence. Callback thread is the only owner (send,
   receive, clear and reset all run there), so no lock is needed — same
   contract as the CEPOS send state. */
typedef struct CepingPeer {
    unsigned long lastSeq;
    unsigned char hasSeq;
} CepingPeer;

static CepingPeer g_peers[TS3_MAX_CLIENT_ID];

/* Set from any thread (pos watcher), consumed on the callback thread. */
static volatile long g_sendPending = 0;

/* Send state — TS callback thread only. */
static unsigned long g_localSeq = 0;
static ULONGLONG g_lastSendMs = 0;

/* Accept our own plugin ID plus the legacy IDs, so a mixed old/new server
   still exchanges what it can (old clients simply never send CEPING). */
static int ceping_plugin_name_matches(const char* pluginName) {
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

void ts3_ceping_signal_send_pending(void) {
    if (InterlockedCompareExchange(&g_sendPending, 1, 0) == 0) {
        ts3_request_wakeup();
    }
}

int ts3_ceping_send_pending(void) {
    return InterlockedCompareExchange(&g_sendPending, 0, 0) != 0;
}

void ts3_ceping_flush(void) {
    char command[CEPING_CMD_MAX];
    ULONGLONG now;

    if (!ts3_thread_is_callback() || !ts3_is_connected() || pluginShuttingDown) {
        InterlockedExchange(&g_sendPending, 0);
        return;
    }
    if (InterlockedCompareExchange(&g_sendPending, 0, 0) == 0) {
        return;
    }

    now = GetTickCount64();
    if (g_lastSendMs != 0 && now - g_lastSendMs < CEPING_SEND_MIN_MS) {
        /* Not yet time. Drop the flag instead of re-arming a wakeup: the pos
           watcher re-signals on its next tick, so a heartbeat is never lost
           and the callback thread is never spun just to wait out the rate. */
        InterlockedExchange(&g_sendPending, 0);
        return;
    }

    g_localSeq = (g_localSeq + 1) & CEPING_SEQ_MASK;
    if (ceping_wire_format(g_localSeq, command, sizeof(command))
        && ts3_send_plugin_command_server(command)) {
        g_lastSendMs = now;
    }
    InterlockedExchange(&g_sendPending, 0);
}

/* ---- receive --------------------------------------------------------------- */

int ts3_ceping_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID) {
    unsigned long seq = 0;
    CepingPeer* peer;

    if (!pluginCommand || pluginShuttingDown) {
        return 0;
    }
    if (strncmp(pluginCommand, CEPING_CMD_PREFIX, CEPING_PREFIX_LEN) != 0) {
        return 0;
    }
    if (!ceping_plugin_name_matches(pluginName) || !ts3_thread_is_callback()) {
        return 1;
    }
    /* Our own heartbeat comes back to us — nothing to learn from it. */
    if (invokerClientID != 0 && invokerClientID == ts3_get_local_client_id()) {
        return 1;
    }
    if (!ts3_client_id_valid(invokerClientID)) {
        return 1;
    }
    if (!ceping_wire_parse(pluginCommand + CEPING_PREFIX_LEN, &seq)) {
        return 1;
    }

    peer = &g_peers[invokerClientID];
    if (peer->hasSeq) {
        const unsigned long missed = ceping_seq_gap(peer->lastSeq, seq);
        if (missed > 0) {
            log_debug("CEPING: peer %u lost %lu update(s)",
                (unsigned)invokerClientID, missed);
        }
    }
    peer->lastSeq = seq;
    peer->hasSeq = 1;
    return 1;
}

/* ---- lifecycle ------------------------------------------------------------- */

void ts3_ceping_clear_client(anyID clientID) {
    if (ts3_client_id_valid(clientID)) {
        memset(&g_peers[clientID], 0, sizeof(g_peers[clientID]));
    }
}

void ts3_ceping_reset(void) {
    memset(g_peers, 0, sizeof(g_peers));
    g_localSeq = 0;
    g_lastSendMs = 0;
    InterlockedExchange(&g_sendPending, 0);
}
