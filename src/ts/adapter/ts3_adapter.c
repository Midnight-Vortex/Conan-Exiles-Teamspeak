#include "ts/adapter/ts3_adapter.h"
#include "ts/adapter/ts3_cmd_ring.h"
#include "core/util/poll_interval.h"
#include "core/util/wakeup_policy.h"
#include "core/util/log.h"

#include "sdk/include/teamspeak/public_errors.h"
#include "sdk/include/teamspeak/public_rare_definitions.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

static struct TS3Functions g_ts3;
static int g_ts3FunctionsSet = 0;
static char g_pluginID[64] = "";

/* ---- 3.5 thread contract ------------------------------------------------ */

static volatile DWORD g_callbackThreadId = 0;

void ts3_thread_mark_callback(void) {
    g_callbackThreadId = GetCurrentThreadId();
}

int ts3_thread_is_callback(void) {
    DWORD id = g_callbackThreadId;
    return id != 0 && id == GetCurrentThreadId();
}

/* Guard used at the top of every API function in this module. */
static int ts3_require_callback_thread(const char* where) {
    if (ts3_thread_is_callback()) {
        return 1;
    }
    log_write("TS-API: %s blocked - not on callback thread", where);
    return 0;
}

/* ---- 3.1 connection state ----------------------------------------------- */

/*
 * Connection identity contract (V8.5):
 *
 * The identity is the triple (g_activeConnection, g_localClientID, g_connected).
 * Writers: ONLY the callback thread (connect status event, tab-switch event,
 * adapter shutdown). Cross-thread readers: wakeup thread and PCM thread.
 *
 * Publish order (enforced by the Interlocked full barriers below):
 *   CONNECT / tab adopt:  g_connected=0 -> id + local client -> g_connected=1 LAST
 *   DISCONNECT/shutdown:  g_connected=0 FIRST -> id + local client cleared
 *
 * Reader rules:
 *   - Gate on ts3_is_connected() BEFORE trusting the id (wakeup thread does).
 *   - Or compare the id for exact equality (PCM playback event); id 0 never
 *     matches a real serverConnectionHandlerID, so a cleared id is inert.
 * Because g_connected flips to 1 last and to 0 first, no reader can ever see
 * "connected" paired with a half-published identity.
 *
 * All cross-thread accesses to the 64-bit id go through conn_id_store/_load
 * (InterlockedExchange64 / InterlockedCompareExchange64): x64 aligned loads
 * are not torn, but the ORDERING against g_connected needs the barrier —
 * plain volatile gives none under MinGW GCC. Callback-thread-only reads
 * (the g_ts3.*() call sites below) stay plain: same thread as every writer.
 */
static volatile uint64 g_activeConnection = 0;
static volatile long g_connected = 0;
static volatile long g_localClientID = 0;

static void conn_id_store(uint64 id) {
    InterlockedExchange64((volatile LONG64*)&g_activeConnection, (LONG64)id);
}

static uint64 conn_id_load(void) {
    return (uint64)InterlockedCompareExchange64((volatile LONG64*)&g_activeConnection, 0, 0);
}

int ts3_on_connect_status_changed(uint64 serverConnectionHandlerID, int newStatus) {
    if (newStatus == STATUS_DISCONNECTED) {
        /* Only a disconnect of the ACTIVE tab may tear the plugin down —
           other tabs disconnecting are none of our business (14.2). */
        if (g_activeConnection != 0 && serverConnectionHandlerID != g_activeConnection) {
            log_debug("TS-EVT: DISCONNECTED on inactive conn=%llu - ignored",
                (unsigned long long)serverConnectionHandlerID);
            return 0;
        }
        /* Publish order: connected off FIRST, then the identity. */
        InterlockedExchange(&g_connected, 0);
        InterlockedExchange(&g_localClientID, 0);
        conn_id_store(0);
        log_write("TS-EVT: DISCONNECTED");
        return 1;
    }

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        /* A BACKGROUND tab finishing its connect must not hijack the plugin
           while the active tab is alive. Adopt the new connection only when
           we have none, or when it is the tab the user is looking at. */
        if (g_activeConnection != 0
            && serverConnectionHandlerID != g_activeConnection
            && InterlockedCompareExchange(&g_connected, 0, 0)) {
            uint64 visibleTab = 0;
            if (g_ts3FunctionsSet && g_ts3.getCurrentServerConnectionHandlerID) {
                visibleTab = g_ts3.getCurrentServerConnectionHandlerID();
            }
            if (visibleTab != serverConnectionHandlerID) {
                log_write("TS-EVT: CONNECTION_ESTABLISHED on background conn=%llu - ignored",
                    (unsigned long long)serverConnectionHandlerID);
                return 0;
            }
        }

        /* Publish order: identity (id + local client) FIRST, connected LAST. */
        InterlockedExchange(&g_connected, 0);
        conn_id_store(serverConnectionHandlerID);
        anyID localID = 0;
        if (g_ts3.getClientID
            && g_ts3.getClientID(serverConnectionHandlerID, &localID) == ERROR_ok) {
            InterlockedExchange(&g_localClientID, (long)localID);
        }
        InterlockedExchange(&g_connected, 1);
        log_write("TS-EVT: CONNECTION_ESTABLISHED conn=%llu localClient=%d",
            (unsigned long long)serverConnectionHandlerID, (int)localID);
        return 1;
    }
    return 0;
}

int ts3_on_active_server_changed(uint64 serverConnectionHandlerID) {
    if (serverConnectionHandlerID == 0
        || serverConnectionHandlerID == g_activeConnection) {
        return 0;
    }

    int status = STATUS_DISCONNECTED;
    if (g_ts3FunctionsSet && g_ts3.getConnectionStatus) {
        if (g_ts3.getConnectionStatus(serverConnectionHandlerID, &status) != ERROR_ok) {
            status = STATUS_DISCONNECTED;
        }
    }

    /* Adopting a different tab = new identity. Publish order: connected off
       FIRST (readers stop trusting the old id), then the new identity, then
       connected on LAST — never expose connected=1 with a half-switched id
       (the old code stored the id first, so the wakeup thread could send on
       a not-yet-established tab). */
    InterlockedExchange(&g_connected, 0);
    InterlockedExchange(&g_localClientID, 0);
    conn_id_store(serverConnectionHandlerID);
    if (status == STATUS_CONNECTION_ESTABLISHED) {
        anyID localID = 0;
        if (g_ts3.getClientID
            && g_ts3.getClientID(serverConnectionHandlerID, &localID) == ERROR_ok) {
            InterlockedExchange(&g_localClientID, (long)localID);
        }
        InterlockedExchange(&g_connected, 1);
    }
    log_write("TS-EVT: active server tab changed -> conn=%llu (connected=%d)",
        (unsigned long long)serverConnectionHandlerID,
        status == STATUS_CONNECTION_ESTABLISHED ? 1 : 0);
    return 1;
}

int ts3_is_connected(void) {
    return InterlockedCompareExchange(&g_connected, 0, 0) != 0;
}

uint64 ts3_get_active_connection(void) {
    /* Cross-thread reader (PCM playback event) — barriered load, see contract. */
    return conn_id_load();
}

anyID ts3_get_local_client_id(void) {
    return (anyID)InterlockedCompareExchange(&g_localClientID, 0, 0);
}

/* ---- 3.2 command queue (control-plane channel B — see ts3_adapter.h) ----- */

/* The pure ring (index math + element copy) lives in ts3_cmd_ring.h so it can
   be host-unit-tested without Win32. THIS file owns the single lock that makes
   it safe from any producer thread: the lock only guards one push/pop copy —
   commands EXECUTE outside the lock (see ts3_cmd_queue_drain), so the TS API is
   never called while the lock is held. */
static CRITICAL_SECTION g_cmdLock;
static INIT_ONCE g_cmdLockOnce = INIT_ONCE_STATIC_INIT;
static Ts3CmdRing g_cmdRing;
static long g_cmdDroppedLogged = 0; /* last dropped count we logged at */

static BOOL CALLBACK ts3_cmd_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cmdLock);
    ts3_cmd_ring_init(&g_cmdRing);
    return TRUE;
}

static void ts3_cmd_lock_ensure(void) {
    InitOnceExecuteOnce(&g_cmdLockOnce, ts3_cmd_lock_init_once, NULL, NULL);
}

int ts3_cmd_queue_push(const Ts3Command* cmd) {
    if (!cmd || cmd->type == TS3_CMD_NONE) {
        return 0;
    }
    ts3_cmd_lock_ensure();

    int pushed;
    long dropped;
    EnterCriticalSection(&g_cmdLock);
    pushed = ts3_cmd_ring_push(&g_cmdRing, cmd);
    dropped = g_cmdRing.dropped;
    LeaveCriticalSection(&g_cmdLock);

    if (!pushed && dropped != g_cmdDroppedLogged) {
        g_cmdDroppedLogged = dropped;
        log_write("TS-CMD: queue full, dropped=%ld", dropped);
    }
    return pushed;
}

static int ts3_cmd_queue_pop(Ts3Command* out) {
    ts3_cmd_lock_ensure();

    int popped;
    EnterCriticalSection(&g_cmdLock);
    popped = ts3_cmd_ring_pop(&g_cmdRing, out);
    LeaveCriticalSection(&g_cmdLock);
    return popped;
}

/* ---- 3.4 channel queries (callback thread only) -------------------------- */

uint64 ts3_get_channel_of_client(anyID clientID) {
    if (!ts3_require_callback_thread("getChannelOfClient")) {
        return 0;
    }
    if (!g_ts3FunctionsSet || !ts3_is_connected() || !g_ts3.getChannelOfClient) {
        return 0;
    }
    uint64 channelID = 0;
    if (g_ts3.getChannelOfClient(g_activeConnection, clientID, &channelID) != ERROR_ok) {
        return 0;
    }
    return channelID;
}

int ts3_get_channel_list(uint64* outChannels, int maxChannels) {
    if (!ts3_require_callback_thread("getChannelList")) {
        return 0;
    }
    if (!outChannels || maxChannels <= 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getChannelList || !g_ts3.freeMemory) {
        return 0;
    }

    uint64* list = NULL;
    if (g_ts3.getChannelList(g_activeConnection, &list) != ERROR_ok || !list) {
        return 0;
    }

    int count = 0;
    for (int i = 0; list[i] != 0 && count < maxChannels; i++) {
        outChannels[count++] = list[i];
    }
    g_ts3.freeMemory(list);
    return count;
}

int ts3_get_channel_name(uint64 channelID, char* outName, int outLen) {
    if (!ts3_require_callback_thread("getChannelName")) {
        return 0;
    }
    if (!outName || outLen <= 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getChannelVariableAsString || !g_ts3.freeMemory) {
        return 0;
    }

    char* name = NULL;
    if (g_ts3.getChannelVariableAsString(g_activeConnection, channelID, CHANNEL_NAME, &name) != ERROR_ok || !name) {
        return 0;
    }
    strncpy_s(outName, (size_t)outLen, name, _TRUNCATE);
    g_ts3.freeMemory(name);
    return 1;
}

/* ---- command execution ---------------------------------------------------- */

static void ts3_cmd_execute(const Ts3Command* cmd) {
    switch (cmd->type) {
    case TS3_CMD_LOG_CHANNEL_LIST: {
        uint64 channels[64];
        int count = ts3_get_channel_list(channels, 64);
        log_write("TS-CMD: channel list (%d channels)", count);
        for (int i = 0; i < count && i < 16; i++) {
            char name[128] = "";
            ts3_get_channel_name(channels[i], name, sizeof(name));
            log_write("TS-CMD:   channel %llu '%s'", (unsigned long long)channels[i], name);
        }
        break;
    }
    case TS3_CMD_NONE:
    default:
        break;
    }
}

void ts3_cmd_queue_drain(void) {
    if (!ts3_thread_is_callback()) {
        static volatile long s_warned = 0;
        if (InterlockedCompareExchange(&s_warned, 1, 0) == 0) {
            log_write("TS-CMD: drain called off the callback thread - ignored");
        }
        return;
    }

    Ts3Command cmd;
    int executed = 0;
    /* Bounded by capacity so a producer that keeps pushing during the drain
       cannot pin the callback thread here forever — leftovers ride the next
       CEDRAIN (the dispatcher re-wakes via ts3_pending_work_any). */
    while (executed < TS3_CMD_RING_CAPACITY && ts3_cmd_queue_pop(&cmd)) {
        ts3_cmd_execute(&cmd);
        executed++;
    }
}

int ts3_cmd_queue_nonempty(void) {
    ts3_cmd_lock_ensure();
    int nonempty = 0;
    EnterCriticalSection(&g_cmdLock);
    nonempty = ts3_cmd_ring_count(&g_cmdRing) > 0;
    LeaveCriticalSection(&g_cmdLock);
    return nonempty;
}

/* ---- 3.3 wakeup — single owner (V8.4) ------------------------------------- */

/*
 * V7 called sendPluginCommand("CEDRAIN:1") DIRECTLY from any requesting thread
 * (pos watcher, UI, settings). That made stability depend on the SDK being
 * thread-safe for that one call from arbitrary threads — the documented V7
 * gap. V8.4 gives a single dedicated thread exclusive ownership of that call:
 *
 *   producers (any thread, incl. PCM) --> set pending/urgent flag + SetEvent
 *   wakeup thread (this file, ONE thread) --> coalesce + rate-limit + send
 *
 * ts3_request_wakeup()/..._urgent() now do PURE Win32 only (Interlocked flag +
 * SetEvent), so they are safe from any thread. The wakeup thread is the ONLY
 * place sendPluginCommand runs off the callback thread.
 *
 * Full single-thread purity is impossible: the TS SDK has no timer/wake
 * callback we could use to run this on the callback thread. So the TS API is
 * touched by exactly TWO threads total — the TS callback thread (everything)
 * and this wakeup thread (exactly one function, sendPluginCommand). That is
 * the minimal possible surface.
 */

static volatile LONG64 s_lastWakeMs = 0;   /* last send time, wakeup thread */

static HANDLE g_wakeupEvent = NULL;        /* auto-reset; signals "work queued" */
static HANDLE g_wakeupThread = NULL;
static volatile long g_wakeupStarted = 0;
static volatile long g_wakeupStop = 0;     /* set once at shutdown, never reset here */
static volatile long g_wakeupPending = 0;  /* a wakeup was requested */
static volatile long g_wakeupUrgent = 0;   /* the pending request was urgent */

static unsigned __stdcall ts3_wakeup_thread(void* arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_wakeupEvent, INFINITE);
        if (InterlockedCompareExchange(&g_wakeupStop, 0, 0)) {
            break;
        }
        /* Coalesce: many SetEvents collapse into the single pending flag.
           Do NOT clear pending until CEDRAIN actually sends — otherwise a
           rate-limit or transient disconnect drops the drain forever. */
        if (InterlockedCompareExchange(&g_wakeupPending, 0, 0) == 0) {
            continue; /* spurious/shutdown wake with nothing queued */
        }

        /* Drop while unconnected — keep pending and retry (reconnect / next tick).
           Identity contract: gate on g_connected FIRST, then load the id with
           a barrier; a concurrent disconnect clears g_connected before the id,
           so a stale-but-valid id is the worst case and id 0 is skipped. */
        if (!g_ts3FunctionsSet || !ts3_is_connected()
            || g_pluginID[0] == '\0' || !g_ts3.sendPluginCommand) {
            Sleep(PLUGIN_POLL_INTERVAL_MS);
            SetEvent(g_wakeupEvent);
            continue;
        }
        const uint64 conn = conn_id_load();
        if (conn == 0) {
            Sleep(PLUGIN_POLL_INTERVAL_MS);
            SetEvent(g_wakeupEvent);
            continue; /* disconnect raced us — retry while pending stays set */
        }

        const long urgent = InterlockedCompareExchange(&g_wakeupUrgent, 0, 0);
        const LONG64 now = (LONG64)GetTickCount64();
        const LONG64 last = InterlockedCompareExchange64(&s_lastWakeMs, 0, 0);
        if (!wakeup_should_send(now, last, urgent != 0, PLUGIN_POLL_INTERVAL_MS)) {
            const LONG64 elapsed = now - last;
            DWORD waitMs = (DWORD)(PLUGIN_POLL_INTERVAL_MS - elapsed);
            if (waitMs == 0 || waitMs > (DWORD)PLUGIN_POLL_INTERVAL_MS) {
                waitMs = 1;
            }
            if (urgent) {
                InterlockedExchange(&g_wakeupUrgent, 1);
            }
            Sleep(waitMs);
            SetEvent(g_wakeupEvent);
            continue; /* rate limited: pending stays set, retry after window */
        }

        InterlockedExchange(&g_wakeupPending, 0);
        InterlockedExchange(&g_wakeupUrgent, 0);
        InterlockedExchange64(&s_lastWakeMs, now);

        g_ts3.sendPluginCommand(conn, g_pluginID, "CEDRAIN:1",
            PluginCommandTarget_SERVER, NULL, NULL);
    }
    return 0;
}

/* Start the single wakeup thread once. Called when the function pointers are
   set (earliest point the send is meaningful). Idempotent. */
static void ts3_wakeup_start(void) {
    if (InterlockedCompareExchange(&g_wakeupStarted, 1, 0) != 0) {
        return;
    }
    InterlockedExchange(&g_wakeupStop, 0);
    g_wakeupEvent = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
    if (!g_wakeupEvent) {
        InterlockedExchange(&g_wakeupStarted, 0);
        log_write("TS-WAKE: CreateEvent failed - wakeup disabled");
        return;
    }
    g_wakeupThread = (HANDLE)_beginthreadex(NULL, 0, ts3_wakeup_thread, NULL, 0, NULL);
    if (!g_wakeupThread) {
        CloseHandle(g_wakeupEvent);
        g_wakeupEvent = NULL;
        InterlockedExchange(&g_wakeupStarted, 0);
        log_write("TS-WAKE: thread create failed - wakeup disabled");
    }
}

/* Signal + JOIN the wakeup thread. Must run BEFORE the connection state the
   thread reads (g_connected/g_activeConnection/g_pluginID) is torn down. */
static void ts3_wakeup_stop(void) {
    if (InterlockedCompareExchange(&g_wakeupStarted, 0, 0) == 0) {
        return;
    }
    InterlockedExchange(&g_wakeupStop, 1);
    if (g_wakeupEvent) {
        SetEvent(g_wakeupEvent);
    }
    if (g_wakeupThread) {
        WaitForSingleObject(g_wakeupThread, INFINITE);
        CloseHandle(g_wakeupThread);
        g_wakeupThread = NULL;
    }
    if (g_wakeupEvent) {
        CloseHandle(g_wakeupEvent);
        g_wakeupEvent = NULL;
    }
    InterlockedExchange(&g_wakeupStarted, 0);
}

/* Ask the wakeup thread to send a CEDRAIN. PURE Win32 — any thread, incl. PCM.
   No-op once shutdown has been signalled. */
void ts3_request_wakeup(void) {
    if (InterlockedCompareExchange(&g_wakeupStop, 0, 0)) {
        return;
    }
    InterlockedExchange(&g_wakeupPending, 1);
    if (g_wakeupEvent) {
        SetEvent(g_wakeupEvent);
    }
}

void ts3_request_wakeup_urgent(void) {
    if (InterlockedCompareExchange(&g_wakeupStop, 0, 0)) {
        return;
    }
    InterlockedExchange(&g_wakeupUrgent, 1);
    InterlockedExchange(&g_wakeupPending, 1);
    if (g_wakeupEvent) {
        SetEvent(g_wakeupEvent);
    }
}

/* ---- plugin commands -------------------------------------------------------- */

const char* ts3_get_plugin_id(void) {
    return g_pluginID;
}

int ts3_send_plugin_command_server(const char* command) {
    if (!ts3_require_callback_thread("sendPluginCommand")) {
        return 0;
    }
    if (!command || !g_ts3FunctionsSet || !ts3_is_connected()
        || g_pluginID[0] == '\0' || !g_ts3.sendPluginCommand) {
        return 0;
    }
    g_ts3.sendPluginCommand(g_activeConnection, g_pluginID, command,
        PluginCommandTarget_SERVER, NULL, NULL);
    return 1;
}

int ts3_get_own_nickname(char* outName, int outLen) {
    if (!ts3_require_callback_thread("getOwnNickname")) {
        return 0;
    }
    if (!outName || outLen <= 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getClientSelfVariableAsString || !g_ts3.freeMemory) {
        return 0;
    }

    char* name = NULL;
    if (g_ts3.getClientSelfVariableAsString(g_activeConnection, CLIENT_NICKNAME, &name) != ERROR_ok || !name) {
        return 0;
    }
    strncpy_s(outName, (size_t)outLen, name, _TRUNCATE);
    g_ts3.freeMemory(name);
    return 1;
}

int ts3_set_own_nickname(const char* nickname) {
    if (!ts3_require_callback_thread("setOwnNickname")) {
        return 0;
    }
    if (!nickname || !nickname[0]
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.setClientSelfVariableAsString || !g_ts3.flushClientSelfUpdates) {
        return 0;
    }
    if (strlen(nickname) < TS3_MIN_SIZE_CLIENT_NICKNAME) {
        return 0;
    }
    unsigned int err = g_ts3.setClientSelfVariableAsString(g_activeConnection,
        CLIENT_NICKNAME, nickname);
    if (err == ERROR_ok) {
        err = g_ts3.flushClientSelfUpdates(g_activeConnection, NULL);
    }
    if (err != ERROR_ok) {
        log_write("TS-API: setOwnNickname failed err=%u", err);
        return 0;
    }
    return 1;
}

int ts3_get_channel_client_list(uint64 channelID, anyID* outClients, int maxClients) {
    if (!ts3_require_callback_thread("getChannelClientList")) {
        return 0;
    }
    if (!outClients || maxClients <= 0 || channelID == 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getChannelClientList || !g_ts3.freeMemory) {
        return 0;
    }

    anyID* list = NULL;
    if (g_ts3.getChannelClientList(g_activeConnection, channelID, &list) != ERROR_ok || !list) {
        return 0;
    }
    int count = 0;
    for (int i = 0; list[i] != 0 && count < maxClients; i++) {
        outClients[count++] = list[i];
    }
    g_ts3.freeMemory(list);
    return count;
}

int ts3_get_client_nickname(anyID clientID, char* outName, int outLen) {
    if (!ts3_require_callback_thread("getClientNickname")) {
        return 0;
    }
    if (!outName || outLen <= 0 || clientID == 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getClientVariableAsString || !g_ts3.freeMemory) {
        return 0;
    }

    char* name = NULL;
    if (g_ts3.getClientVariableAsString(g_activeConnection, clientID, CLIENT_NICKNAME, &name) != ERROR_ok
        || !name) {
        return 0;
    }
    strncpy_s(outName, (size_t)outLen, name, _TRUNCATE);
    g_ts3.freeMemory(name);
    return 1;
}

int ts3_get_server_uid(char* out, int outLen) {
    if (!ts3_require_callback_thread("getServerUid")) {
        return 0;
    }
    if (!out || outLen <= 0 || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getServerVariableAsString || !g_ts3.freeMemory) {
        return 0;
    }

    char* uid = NULL;
    if (g_ts3.getServerVariableAsString(g_activeConnection,
            VIRTUALSERVER_UNIQUE_IDENTIFIER, &uid) != ERROR_ok || !uid) {
        return 0;
    }
    strncpy_s(out, (size_t)outLen, uid, _TRUNCATE);
    g_ts3.freeMemory(uid);
    return out[0] != '\0';
}

void ts3_print_to_chat(const char* message) {
    if (!ts3_require_callback_thread("printToChat")) {
        return;
    }
    if (!message || !g_ts3FunctionsSet || !g_ts3.printMessageToCurrentTab) {
        return;
    }
    char formatted[640];
    snprintf(formatted, sizeof(formatted), "[color=green]%s[/color]", message);
    g_ts3.printMessageToCurrentTab(formatted);
}

void ts3_log_client(const char* message) {
    if (!ts3_require_callback_thread("logClient")) {
        return;
    }
    if (!message || !g_ts3FunctionsSet || !g_ts3.logMessage) {
        return;
    }
    g_ts3.logMessage(message, LogLevel_INFO, "Conan Exiles", 0);
}

int ts3_unmute_clients_for_pcm(const anyID* clients, int count) {
    if (!ts3_require_callback_thread("unmuteClients")) {
        return 0;
    }
    if (!clients || count <= 0 || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.requestUnmuteClientsTemporary) {
        return 0;
    }

    /* Neutral volume modifier so TS's own attenuation never stacks with ours. */
    if (g_ts3.setClientVolumeModifier) {
        for (int i = 0; i < count; i++) {
            g_ts3.setClientVolumeModifier(g_activeConnection, clients[i], 0.0f);
        }
    }

    /* API expects a zero-terminated array. */
    anyID batch[64];
    int n = count > 63 ? 63 : count;
    for (int i = 0; i < n; i++) {
        batch[i] = clients[i];
    }
    batch[n] = 0;

    if (g_ts3.requestUnmuteClientsTemporary(g_activeConnection, batch, NULL) != ERROR_ok) {
        return 0;
    }
    return n;
}

int ts3_request_client_move(anyID clientID, uint64 channelID, const char* password) {
    if (!ts3_require_callback_thread("requestClientMove")) {
        return 0;
    }
    if (clientID == 0 || channelID == 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.requestClientMove) {
        return 0;
    }
    const unsigned int err = g_ts3.requestClientMove(g_activeConnection, clientID, channelID,
        password ? password : "", NULL);
    if (err != ERROR_ok) {
        log_write("TS-API: requestClientMove -> %llu failed err=%u",
            (unsigned long long)channelID, err);
        return 0;
    }
    return 1;
}

int ts3_request_channel_description(uint64 channelID) {
    if (!ts3_require_callback_thread("requestChannelDescription")) {
        return 0;
    }
    if (channelID == 0 || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.requestChannelDescription) {
        return 0;
    }
    const unsigned int err = g_ts3.requestChannelDescription(g_activeConnection, channelID, NULL);
    if (err != ERROR_ok) {
        log_write("TS-API: requestChannelDescription %llu failed err=%u",
            (unsigned long long)channelID, err);
        return 0;
    }
    return 1;
}

int ts3_get_channel_description(uint64 channelID, char* out, int outLen) {
    if (!ts3_require_callback_thread("getChannelDescription")) {
        return 0;
    }
    if (!out || outLen <= 0 || channelID == 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.getChannelVariableAsString || !g_ts3.freeMemory) {
        return 0;
    }

    char* desc = NULL;
    if (g_ts3.getChannelVariableAsString(g_activeConnection, channelID, CHANNEL_DESCRIPTION, &desc) != ERROR_ok
        || !desc) {
        return 0;
    }
    strncpy_s(out, (size_t)outLen, desc, _TRUNCATE);
    g_ts3.freeMemory(desc);
    return 1;
}

/* ---- 3D audio (raw API, callback thread only) ------------------------------- */

int ts3_set_3d_settings(float distanceFactor, float rolloffScale) {
    if (!ts3_require_callback_thread("set3DSettings")) {
        return 0;
    }
    if (!g_ts3FunctionsSet || !ts3_is_connected() || !g_ts3.systemset3DSettings) {
        return 0;
    }
    return g_ts3.systemset3DSettings(g_activeConnection, distanceFactor, rolloffScale) == ERROR_ok;
}

int ts3_set_3d_listener(const TS3_VECTOR* position, const TS3_VECTOR* forward,
    const TS3_VECTOR* up) {
    if (!ts3_require_callback_thread("set3DListener")) {
        return 0;
    }
    if (!position || !forward || !up
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.systemset3DListenerAttributes) {
        return 0;
    }
    return g_ts3.systemset3DListenerAttributes(g_activeConnection, position, forward, up) == ERROR_ok;
}

int ts3_set_3d_client(anyID clientID, const TS3_VECTOR* position) {
    if (!ts3_require_callback_thread("set3DClient")) {
        return 0;
    }
    if (!position || clientID == 0
        || !g_ts3FunctionsSet || !ts3_is_connected()
        || !g_ts3.channelset3DAttributes) {
        return 0;
    }
    return g_ts3.channelset3DAttributes(g_activeConnection, clientID, position) == ERROR_ok;
}

/* ---- setup / shutdown ------------------------------------------------------ */

void ts3_adapter_set_functions(const struct TS3Functions* funcs) {
    if (!funcs) {
        return;
    }
    g_ts3 = *funcs;
    g_ts3FunctionsSet = 1;
    ts3_cmd_lock_ensure();
    /* Earliest lifecycle hook where sendPluginCommand is meaningful — start the
       single wakeup owner here (joined in ts3_adapter_shutdown). */
    ts3_wakeup_start();
}

void ts3_adapter_set_plugin_id(const char* id) {
    if (!id) {
        return;
    }
    strncpy_s(g_pluginID, sizeof(g_pluginID), id, _TRUNCATE);
}

void ts3_adapter_shutdown(void) {
    /* Join the wakeup thread FIRST: after this it cannot read g_connected /
       g_activeConnection / g_pluginID / g_ts3, and ts3_request_wakeup is a
       no-op (g_wakeupStop set). Only then tear the connection state down. */
    ts3_wakeup_stop();

    /* Publish order: connected off FIRST, then the identity (contract above). */
    InterlockedExchange(&g_connected, 0);
    InterlockedExchange(&g_localClientID, 0);
    conn_id_store(0);

    /* Drop anything still queued (preserve the lifetime drop counter). */
    ts3_cmd_lock_ensure();
    EnterCriticalSection(&g_cmdLock);
    g_cmdRing.head = 0;
    g_cmdRing.tail = 0;
    g_cmdRing.count = 0;
    LeaveCriticalSection(&g_cmdLock);
}
