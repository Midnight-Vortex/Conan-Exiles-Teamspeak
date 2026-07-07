#include "ts/adapter/ts3_adapter.h"
#include "core/util/poll_interval.h"
#include "core/util/log.h"

#include "sdk/include/teamspeak/public_errors.h"
#include "sdk/include/teamspeak/public_rare_definitions.h"

#include <windows.h>
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

static volatile uint64 g_activeConnection = 0;
static volatile long g_connected = 0;
static volatile long g_localClientID = 0;

int ts3_on_connect_status_changed(uint64 serverConnectionHandlerID, int newStatus) {
    if (newStatus == STATUS_DISCONNECTED) {
        /* Only a disconnect of the ACTIVE tab may tear the plugin down —
           other tabs disconnecting are none of our business (14.2). */
        if (g_activeConnection != 0 && serverConnectionHandlerID != g_activeConnection) {
            log_debug("TS-EVT: DISCONNECTED on inactive conn=%llu - ignored",
                (unsigned long long)serverConnectionHandlerID);
            return 0;
        }
        InterlockedExchange(&g_connected, 0);
        InterlockedExchange(&g_localClientID, 0);
        g_activeConnection = 0;
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

        g_activeConnection = serverConnectionHandlerID;
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

    g_activeConnection = serverConnectionHandlerID;
    if (status == STATUS_CONNECTION_ESTABLISHED) {
        anyID localID = 0;
        if (g_ts3.getClientID
            && g_ts3.getClientID(serverConnectionHandlerID, &localID) == ERROR_ok) {
            InterlockedExchange(&g_localClientID, (long)localID);
        }
        InterlockedExchange(&g_connected, 1);
    }
    else {
        InterlockedExchange(&g_connected, 0);
        InterlockedExchange(&g_localClientID, 0);
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
    return g_activeConnection;
}

anyID ts3_get_local_client_id(void) {
    return (anyID)InterlockedCompareExchange(&g_localClientID, 0, 0);
}

/* ---- 3.2 command queue --------------------------------------------------- */

#define TS3_CMD_QUEUE_SIZE 256

/* Ring buffer with its own private lock. The lock only guards the memcpy of
   one command in push/pop — commands are executed OUTSIDE the lock, so the
   TS API is never called while this lock is held. */
static CRITICAL_SECTION g_cmdLock;
static INIT_ONCE g_cmdLockOnce = INIT_ONCE_STATIC_INIT;
static Ts3Command g_cmdQueue[TS3_CMD_QUEUE_SIZE];
static int g_cmdHead = 0; /* next pop  */
static int g_cmdTail = 0; /* next push */
static int g_cmdCount = 0;
static volatile long g_cmdDropped = 0;

static BOOL CALLBACK ts3_cmd_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cmdLock);
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

    int pushed = 0;
    EnterCriticalSection(&g_cmdLock);
    if (g_cmdCount < TS3_CMD_QUEUE_SIZE) {
        g_cmdQueue[g_cmdTail] = *cmd;
        g_cmdTail = (g_cmdTail + 1) % TS3_CMD_QUEUE_SIZE;
        g_cmdCount++;
        pushed = 1;
    }
    LeaveCriticalSection(&g_cmdLock);

    if (!pushed) {
        long dropped = InterlockedIncrement(&g_cmdDropped);
        if ((dropped % 100) == 1) {
            log_write("TS-CMD: queue full, dropped=%ld", dropped);
        }
    }
    return pushed;
}

static int ts3_cmd_queue_pop(Ts3Command* out) {
    ts3_cmd_lock_ensure();

    int popped = 0;
    EnterCriticalSection(&g_cmdLock);
    if (g_cmdCount > 0) {
        *out = g_cmdQueue[g_cmdHead];
        g_cmdHead = (g_cmdHead + 1) % TS3_CMD_QUEUE_SIZE;
        g_cmdCount--;
        popped = 1;
    }
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
    while (executed < TS3_CMD_QUEUE_SIZE && ts3_cmd_queue_pop(&cmd)) {
        ts3_cmd_execute(&cmd);
        executed++;
    }
}

int ts3_cmd_queue_nonempty(void) {
    ts3_cmd_lock_ensure();
    int nonempty = 0;
    EnterCriticalSection(&g_cmdLock);
    nonempty = g_cmdCount > 0;
    LeaveCriticalSection(&g_cmdLock);
    return nonempty;
}

/* ---- 3.3 wakeup ----------------------------------------------------------- */

static volatile LONG64 s_lastWakeMs = 0;

static void ts3_send_wakeup(int urgent) {
    if (!g_ts3FunctionsSet || !ts3_is_connected()
        || g_pluginID[0] == '\0' || !g_ts3.sendPluginCommand) {
        return;
    }

    LONG64 now = (LONG64)GetTickCount64();
    if (!urgent) {
        /* Rate limit: one wakeup round trip per PLUGIN_POLL_INTERVAL_MS. */
        LONG64 last = InterlockedCompareExchange64(&s_lastWakeMs, 0, 0);
        if (now - last < PLUGIN_POLL_INTERVAL_MS) {
            return;
        }
        if (InterlockedCompareExchange64(&s_lastWakeMs, now, last) != last) {
            return; /* someone else just sent one */
        }
    }
    else {
        InterlockedExchange64(&s_lastWakeMs, now);
    }

    g_ts3.sendPluginCommand(g_activeConnection, g_pluginID, "CEDRAIN:1",
        PluginCommandTarget_SERVER, NULL, NULL);
}

void ts3_request_wakeup(void) {
    ts3_send_wakeup(0);
}

void ts3_request_wakeup_urgent(void) {
    ts3_send_wakeup(1);
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
}

void ts3_adapter_set_plugin_id(const char* id) {
    if (!id) {
        return;
    }
    strncpy_s(g_pluginID, sizeof(g_pluginID), id, _TRUNCATE);
}

void ts3_adapter_shutdown(void) {
    InterlockedExchange(&g_connected, 0);
    g_activeConnection = 0;

    /* Drop anything still queued. */
    ts3_cmd_lock_ensure();
    EnterCriticalSection(&g_cmdLock);
    g_cmdHead = 0;
    g_cmdTail = 0;
    g_cmdCount = 0;
    LeaveCriticalSection(&g_cmdLock);
}
