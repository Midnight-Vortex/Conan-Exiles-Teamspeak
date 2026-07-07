#include "ts/info/ts3_plugin_version.h"

#include "ts/adapter/ts3_adapter.h"
#include "ts/entry/ts3_exports.h"
#include "core/util/log.h"

#include <stdio.h>
#include <string.h>

#define TS3_MAX_CLIENT_ID       4096
#define TS3_VERSION_MAX_LEN     24
#define TS3_DISPLAY_NAME_LEN    48
#define TS3_UNIQUE_ID_LEN       48
#define CEVER_CMD_PREFIX        "CEVER:"

static char g_clientVersion[TS3_MAX_CLIENT_ID][TS3_VERSION_MAX_LEN];
static char g_clientDisplayName[TS3_MAX_CLIENT_ID][TS3_DISPLAY_NAME_LEN];
static char g_clientUniqueId[TS3_MAX_CLIENT_ID][TS3_UNIQUE_ID_LEN];
static char g_versionReplied[TS3_MAX_CLIENT_ID];

static int plugin_name_matches(const char* pluginName) {
    if (!pluginName || !pluginName[0]) {
        return 0;
    }
    if (strcmp(pluginName, ts3_get_plugin_id()) == 0) {
        return 1;
    }
    return strcmp(pluginName, "conan_exiles") == 0
        || strcmp(pluginName, "conan_exiles_ts") == 0;
}

static int version_string_valid(const char* ver) {
    if (!ver || !ver[0]) {
        return 0;
    }
    const size_t len = strlen(ver);
    if (len == 0 || len >= TS3_VERSION_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = ver[i];
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

static void cache_display_name(anyID clientID, const char* name) {
    if (clientID == 0 || clientID >= TS3_MAX_CLIENT_ID || !name || !name[0]) {
        return;
    }
    strncpy_s(g_clientDisplayName[clientID], sizeof(g_clientDisplayName[clientID]),
        name, _TRUNCATE);
}

static void cache_unique_id(anyID clientID, const char* uniqueId) {
    if (clientID == 0 || clientID >= TS3_MAX_CLIENT_ID || !uniqueId || !uniqueId[0]) {
        return;
    }
    strncpy_s(g_clientUniqueId[clientID], sizeof(g_clientUniqueId[clientID]),
        uniqueId, _TRUNCATE);
}

static void set_client_version(anyID clientID, const char* version) {
    if (clientID == 0 || clientID >= TS3_MAX_CLIENT_ID || !version_string_valid(version)) {
        return;
    }
    strncpy_s(g_clientVersion[clientID], sizeof(g_clientVersion[clientID]),
        version, _TRUNCATE);
}

static void purge_stale_version_entries(anyID keepClientID, const char* uniqueId, const char* displayName) {
    for (anyID i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        if (i == keepClientID || !g_clientVersion[i][0]) {
            continue;
        }
        if (uniqueId && uniqueId[0] && g_clientUniqueId[i][0]
            && strcmp(g_clientUniqueId[i], uniqueId) == 0) {
            g_clientVersion[i][0] = '\0';
            g_versionReplied[i] = 0;
            continue;
        }
        if (displayName && displayName[0] && g_clientDisplayName[i][0]
            && strcmp(g_clientDisplayName[i], displayName) == 0) {
            g_clientVersion[i][0] = '\0';
            g_versionReplied[i] = 0;
        }
    }
}

static void send_version_command(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }
    const char* ver = ts3plugin_version();
    if (!ver || !ver[0]) {
        return;
    }
    char command[64];
    snprintf(command, sizeof(command), "%s%s", CEVER_CMD_PREFIX, ver);
    if (ts3_send_plugin_command_server(command)) {
        const anyID localId = ts3_get_local_client_id();
        if (localId != 0) {
            set_client_version(localId, ver);
        }
        log_debug("VERSION: broadcast %s", ver);
    }
}

void ts3_version_reset(void) {
    memset(g_clientVersion, 0, sizeof(g_clientVersion));
    memset(g_clientDisplayName, 0, sizeof(g_clientDisplayName));
    memset(g_clientUniqueId, 0, sizeof(g_clientUniqueId));
    memset(g_versionReplied, 0, sizeof(g_versionReplied));
}

void ts3_version_clear_client(anyID clientID) {
    if (clientID == 0 || clientID >= TS3_MAX_CLIENT_ID) {
        return;
    }
    g_clientVersion[clientID][0] = '\0';
    g_clientDisplayName[clientID][0] = '\0';
    g_clientUniqueId[clientID][0] = '\0';
    g_versionReplied[clientID] = 0;
}

void ts3_version_broadcast(void) {
    send_version_command();
}

int ts3_version_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID, const char* invokerDisplayName, const char* invokerUniqueIdentity) {
    if (!pluginCommand || strncmp(pluginCommand, CEVER_CMD_PREFIX, strlen(CEVER_CMD_PREFIX)) != 0) {
        return 0;
    }
    if (!plugin_name_matches(pluginName)) {
        return 1;
    }

    const char* versionText = pluginCommand + strlen(CEVER_CMD_PREFIX);
    if (invokerClientID == 0 || invokerClientID >= TS3_MAX_CLIENT_ID) {
        return 1;
    }
    if (!version_string_valid(versionText)) {
        return 1;
    }

    purge_stale_version_entries(invokerClientID, invokerUniqueIdentity, invokerDisplayName);
    cache_display_name(invokerClientID, invokerDisplayName);
    cache_unique_id(invokerClientID, invokerUniqueIdentity);
    set_client_version(invokerClientID, versionText);

    if (!g_versionReplied[invokerClientID]) {
        g_versionReplied[invokerClientID] = 1;
        send_version_command();
    }
    return 1;
}

int ts3_version_format_info(anyID clientID, char* buf, size_t bufSize) {
    const char* currentVer = ts3plugin_version();
    int written;

    if (!buf || bufSize < 32) {
        return 0;
    }

    if (clientID != 0) {
        if (g_clientVersion[clientID][0]) {
            const char* remoteVer = g_clientVersion[clientID];
            if (currentVer && strcmp(remoteVer, currentVer) == 0) {
                written = snprintf(buf, bufSize,
                    "Conan Exiles Proximity Voice %s (aktuell)", remoteVer);
            }
            else {
                written = snprintf(buf, bufSize,
                    "Conan Exiles Proximity Voice %s (veraltet - neu: %s)",
                    remoteVer, currentVer ? currentVer : "?");
            }
        }
        else {
            written = snprintf(buf, bufSize,
                "Kein Plugin erkannt\n(Conan Exiles %s erwartet)",
                currentVer ? currentVer : "?");
        }
        return written > 0 ? written : 0;
    }

    written = snprintf(buf, bufSize,
        "Conan Exiles Proximity Voice %s\n\n"
        "Erkannte Plugin-Versionen:\n",
        currentVer ? currentVer : "?");
    if (written < 0 || (size_t)written >= bufSize) {
        return written > 0 ? written : 0;
    }

    char* line = buf + written;
    size_t remain = bufSize - (size_t)written;
    int anyListed = 0;
    anyID clientIds[256];
    const int count = ts3_get_connected_client_ids(clientIds, (int)(sizeof(clientIds) / sizeof(clientIds[0])));

    for (int i = 0; i < count && remain > 48; i++) {
        const anyID cid = clientIds[i];
        if (cid == 0 || cid >= TS3_MAX_CLIENT_ID || !g_clientVersion[cid][0]) {
            continue;
        }
        anyListed = 1;
        const char* ver = g_clientVersion[cid];
        const char* status = (currentVer && strcmp(ver, currentVer) == 0) ? "ok" : "veraltet";
        const char* name = g_clientDisplayName[cid][0] ? g_clientDisplayName[cid] : NULL;
        int lineLen;
        if (name) {
            lineLen = snprintf(line, remain, "  %s: %s (%s)\n", name, ver, status);
        }
        else {
            lineLen = snprintf(line, remain, "  Client %u: %s (%s)\n", (unsigned)cid, ver, status);
        }
        if (lineLen <= 0 || (size_t)lineLen >= remain) {
            break;
        }
        line += lineLen;
        remain -= (size_t)lineLen;
    }

    if (!anyListed) {
        snprintf(line, remain, "  (noch keine - kurz warten nach Connect)\n");
    }
    else if (remain > 48) {
        snprintf(line, remain, "\nOhne Eintrag = kein Plugin oder noch nicht gemeldet.\n");
    }

    return (int)strlen(buf);
}
