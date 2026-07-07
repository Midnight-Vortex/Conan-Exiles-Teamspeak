#include "ts/info/ts3_plugin_version.h"

#include "ts/adapter/ts3_adapter.h"
#include "ts/entry/ts3_exports.h"
#include "plugin.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define VERSION_MAP_SLOTS     512
#define VERSION_TEXT_MAX      24
#define VERSION_NAME_MAX      48
#define VERSION_UID_MAX       48
#define VERSION_CMD_PREFIX    "CEVER:"
#define VERSION_CMD_MAX       64

typedef struct VersionClientSlot {
    char version[VERSION_TEXT_MAX];
    char displayName[VERSION_NAME_MAX];
    char uniqueId[VERSION_UID_MAX];
    unsigned char replied;
    unsigned char hasVersion;
} VersionClientSlot;

typedef struct VersionMapEntry {
    anyID clientID;
    VersionClientSlot slot;
} VersionMapEntry;

static VersionMapEntry g_versionMap[VERSION_MAP_SLOTS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK version_lock_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void version_lock_ensure(void) {
    InitOnceExecuteOnce(&g_lockOnce, version_lock_init, NULL, NULL);
}

static int version_client_id_ok(anyID clientID) {
    return clientID > 0;
}

static VersionMapEntry* version_map_find(anyID clientID) {
    if (!version_client_id_ok(clientID)) {
        return NULL;
    }
    const size_t start = (size_t)clientID % VERSION_MAP_SLOTS;
    for (size_t i = 0; i < VERSION_MAP_SLOTS; i++) {
        VersionMapEntry* entry = &g_versionMap[(start + i) % VERSION_MAP_SLOTS];
        if (entry->clientID == clientID) {
            return entry;
        }
        if (entry->clientID == 0) {
            return NULL;
        }
    }
    return NULL;
}

static VersionClientSlot* version_map_get(anyID clientID, int create) {
    VersionMapEntry* entry = version_map_find(clientID);
    if (entry) {
        return &entry->slot;
    }
    if (!create || !version_client_id_ok(clientID)) {
        return NULL;
    }
    const size_t start = (size_t)clientID % VERSION_MAP_SLOTS;
    for (size_t i = 0; i < VERSION_MAP_SLOTS; i++) {
        entry = &g_versionMap[(start + i) % VERSION_MAP_SLOTS];
        if (entry->clientID == 0) {
            entry->clientID = clientID;
            memset(&entry->slot, 0, sizeof(entry->slot));
            return &entry->slot;
        }
    }
    return NULL;
}

static int version_plugin_name_ok(const char* pluginName) {
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

static int version_text_ok(const char* text) {
    size_t len;

    if (!text || !text[0]) {
        return 0;
    }
    len = strlen(text);
    if (len == 0 || len >= VERSION_TEXT_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if ((c >= '0' && c <= '9')
            || (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || c == '.' || c == '-' || c == '_') {
            continue;
        }
        return 0;
    }
    return 1;
}

static void version_clear_slot_locked(anyID clientID) {
    VersionMapEntry* entry = version_map_find(clientID);
    if (entry) {
        memset(entry, 0, sizeof(*entry));
    }
}

static void version_purge_stale_locked(anyID keepClientID,
    const char* uniqueId, const char* displayName) {
    for (size_t i = 0; i < VERSION_MAP_SLOTS; i++) {
        VersionMapEntry* entry = &g_versionMap[i];
        VersionClientSlot* slot;

        if (entry->clientID == 0 || entry->clientID == keepClientID || !entry->slot.hasVersion) {
            continue;
        }
        slot = &entry->slot;
        if (uniqueId && uniqueId[0] && slot->uniqueId[0]
            && strcmp(slot->uniqueId, uniqueId) == 0) {
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        if (displayName && displayName[0] && slot->displayName[0]
            && strcmp(slot->displayName, displayName) == 0) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void version_store_locked(anyID clientID, const char* versionText,
    const char* displayName, const char* uniqueId) {
    VersionClientSlot* slot;

    if (!version_text_ok(versionText)) {
        return;
    }
    slot = version_map_get(clientID, 1);
    if (!slot) {
        return;
    }
    strncpy_s(slot->version, sizeof(slot->version), versionText, _TRUNCATE);
    slot->hasVersion = 1;
    if (displayName && displayName[0]) {
        strncpy_s(slot->displayName, sizeof(slot->displayName), displayName, _TRUNCATE);
    }
    if (uniqueId && uniqueId[0]) {
        strncpy_s(slot->uniqueId, sizeof(slot->uniqueId), uniqueId, _TRUNCATE);
    }
}

static int version_send_broadcast(void) {
    const char* ver;
    char command[VERSION_CMD_MAX];
    anyID localId;

    if (!ts3_thread_is_callback() || !ts3_is_connected() || pluginShuttingDown) {
        return 0;
    }

    ver = ts3plugin_version();
    if (!ver || !ver[0] || !version_text_ok(ver)) {
        return 0;
    }

    snprintf(command, sizeof(command), "%s%s", VERSION_CMD_PREFIX, ver);
    if (!ts3_send_plugin_command_server(command)) {
        return 0;
    }

    localId = ts3_get_local_client_id();
    if (version_client_id_ok(localId)) {
        version_lock_ensure();
        EnterCriticalSection(&g_lock);
        version_store_locked(localId, ver, NULL, NULL);
        LeaveCriticalSection(&g_lock);
    }
    return 1;
}

void ts3_version_reset(void) {
    version_lock_ensure();
    EnterCriticalSection(&g_lock);
    memset(g_versionMap, 0, sizeof(g_versionMap));
    LeaveCriticalSection(&g_lock);
}

void ts3_version_clear_client(anyID clientID) {
    version_lock_ensure();
    EnterCriticalSection(&g_lock);
    version_clear_slot_locked(clientID);
    LeaveCriticalSection(&g_lock);
}

void ts3_version_broadcast(void) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    version_send_broadcast();
}

int ts3_version_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID, const char* invokerDisplayName, const char* invokerUniqueIdentity) {
    const char* versionText;
    int needReply = 0;

    if (!pluginCommand || pluginShuttingDown) {
        return 0;
    }
    if (strncmp(pluginCommand, VERSION_CMD_PREFIX, strlen(VERSION_CMD_PREFIX)) != 0) {
        return 0;
    }
    if (!version_plugin_name_ok(pluginName)) {
        return 1;
    }
    if (!ts3_thread_is_callback()) {
        return 1;
    }

    versionText = pluginCommand + strlen(VERSION_CMD_PREFIX);
    if (!version_text_ok(versionText)) {
        return 1;
    }

    version_lock_ensure();
    EnterCriticalSection(&g_lock);
    version_purge_stale_locked(invokerClientID, invokerUniqueIdentity, invokerDisplayName);
    version_store_locked(invokerClientID, versionText, invokerDisplayName, invokerUniqueIdentity);
    {
        VersionClientSlot* invokerSlot = version_map_get(invokerClientID, 0);
        needReply = invokerSlot && !invokerSlot->replied;
        if (needReply && invokerSlot) {
            invokerSlot->replied = 1;
        }
    }
    LeaveCriticalSection(&g_lock);

    if (needReply) {
        version_send_broadcast();
    }
    return 1;
}

static int version_format_one_client(anyID clientID, char* buf, size_t bufSize) {
    const char* currentVer = ts3plugin_version();
    VersionClientSlot slot;
    int written;

    if (!buf || bufSize < 32 || !version_client_id_ok(clientID)) {
        return 0;
    }

    version_lock_ensure();
    EnterCriticalSection(&g_lock);
    {
        VersionMapEntry* entry = version_map_find(clientID);
        if (entry) {
            slot = entry->slot;
        }
        else {
            memset(&slot, 0, sizeof(slot));
        }
    }
    LeaveCriticalSection(&g_lock);

    if (slot.hasVersion) {
        if (currentVer && strcmp(slot.version, currentVer) == 0) {
            written = snprintf(buf, bufSize,
                "Conan Exiles Proximity Voice %s (aktuell)", slot.version);
        }
        else {
            written = snprintf(buf, bufSize,
                "Conan Exiles Proximity Voice %s (veraltet - neu: %s)",
                slot.version, currentVer ? currentVer : "?");
        }
    }
    else {
        written = snprintf(buf, bufSize,
            "Kein Plugin erkannt\n(Conan Exiles %s erwartet)",
            currentVer ? currentVer : "?");
    }
    return written > 0 ? written : 0;
}

static int version_format_server_list(char* buf, size_t bufSize) {
    const char* currentVer = ts3plugin_version();
    char* line;
    size_t remain;
    int written;
    int anyListed = 0;

    if (!buf || bufSize < 64) {
        return 0;
    }

    written = snprintf(buf, bufSize,
        "Conan Exiles Proximity Voice %s\n\n"
        "Erkannte Plugin-Versionen:\n",
        currentVer ? currentVer : "?");
    if (written < 0 || (size_t)written >= bufSize) {
        return written > 0 ? written : 0;
    }

    line = buf + written;
    remain = bufSize - (size_t)written;

    version_lock_ensure();
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < VERSION_MAP_SLOTS && remain > 48; i++) {
        const VersionMapEntry* entry = &g_versionMap[i];
        const VersionClientSlot* slot = &entry->slot;
        const char* status;
        int lineLen;

        if (entry->clientID == 0 || !slot->hasVersion) {
            continue;
        }
        anyListed = 1;
        status = (currentVer && strcmp(slot->version, currentVer) == 0) ? "ok" : "veraltet";
        if (slot->displayName[0]) {
            lineLen = snprintf(line, remain, "  %s: %s (%s)\n",
                slot->displayName, slot->version, status);
        }
        else {
            lineLen = snprintf(line, remain, "  Client %u: %s (%s)\n",
                (unsigned)entry->clientID, slot->version, status);
        }
        if (lineLen <= 0 || (size_t)lineLen >= remain) {
            break;
        }
        line += lineLen;
        remain -= (size_t)lineLen;
    }
    LeaveCriticalSection(&g_lock);

    if (!anyListed) {
        snprintf(line, remain, "  (noch keine - kurz warten nach Connect)\n");
    }
    else if (remain > 48) {
        snprintf(line, remain, "\nOhne Eintrag = kein Plugin oder noch nicht gemeldet.\n");
    }

    return (int)strlen(buf);
}

int ts3_version_format_info(anyID clientID, char* buf, size_t bufSize) {
    if (pluginShuttingDown || !buf || bufSize < 32) {
        return 0;
    }
    if (clientID != 0) {
        return version_format_one_client(clientID, buf, bufSize);
    }
    return version_format_server_list(buf, bufSize);
}
