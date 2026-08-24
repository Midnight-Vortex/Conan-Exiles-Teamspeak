#include "ts3_exports.h"

#include "ts/info/ts3_plugin_version.h"
#include "ts/proximity/ts3_cemode.h"
#include "ts/proximity/ts3_ceauth.h"
#include "plugin_modules.h"
#include "ui/plugin_ui_compat.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Plugins → Conan Exiles submenu (PLUGIN_MENU_TYPE_GLOBAL). IDs are ours. */
enum {
    MENU_ID_SETTINGS = 1,
    MENU_ID_OPEN_PLUGINS_FOLDER = 2,
    MENU_ID_OPEN_LOG_FOLDER = 3
};

static struct PluginMenuItem* menu_item_create(enum PluginMenuType type, int id,
    const char* text) {
    struct PluginMenuItem* item = (struct PluginMenuItem*)malloc(sizeof(*item));
    if (!item) {
        return NULL;
    }
    item->type = type;
    item->id = id;
    item->icon[0] = '\0';
    strncpy_s(item->text, PLUGIN_MENU_BUFSZ, text, _TRUNCATE);
    return item;
}

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    struct PluginMenuItem** items;

    if (!menuItems || !menuIcon) {
        return;
    }
    *menuIcon = NULL; /* no plugin submenu icon */

    /* 3 items + NULL terminator — TeamSpeak frees each entry via freeMemory. */
    items = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * 4);
    if (!items) {
        *menuItems = NULL;
        return;
    }
    items[0] = menu_item_create(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_SETTINGS,
        "Einstellungen");
    items[1] = menu_item_create(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_OPEN_PLUGINS_FOLDER,
        "Plugins-Ordner oeffnen");
    items[2] = menu_item_create(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_OPEN_LOG_FOLDER,
        "Log-Ordner oeffnen");
    items[3] = NULL;
    if (!items[0] || !items[1] || !items[2]) {
        free(items[0]);
        free(items[1]);
        free(items[2]);
        free(items);
        *menuItems = NULL;
        return;
    }
    *menuItems = items;
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type,
    int menuItemID, uint64 selectedItemID) {
    (void)serverConnectionHandlerID;
    (void)selectedItemID;

    if (pluginShuttingDown || type != PLUGIN_MENU_TYPE_GLOBAL) {
        return;
    }

    switch (menuItemID) {
    case MENU_ID_SETTINGS:
        /* Must not block the UI/callback thread with showConfigInterface(). */
        if (config_dialog_try_open()) {
            settings_dialog_open_async();
        }
        break;
    case MENU_ID_OPEN_PLUGINS_FOLDER:
        open_ts3_plugins_folder();
        break;
    case MENU_ID_OPEN_LOG_FOLDER:
        open_plugin_log_folder();
        break;
    default:
        break;
    }
}

void ts3plugin_freeMemory(void* data) {
    if (data) {
        free(data);
    }
}

int ts3plugin_requestAutoload(void) {
    return 0;
}

int ts3plugin_offersConfigure(void) {
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

void ts3plugin_configure(void* handle, void* qParentWidget) {
    (void)handle;
    (void)qParentWidget;
    if (!config_dialog_try_open()) {
        return;
    }
    showConfigInterface();
    config_dialog_close();
}

const char* ts3plugin_infoTitle(void) {
    return "Conan Exiles";
}

void ts3plugin_infoData(uint64 serverConnectionHandlerID, uint64 id, enum PluginItemType type, char** data) {
    anyID clientID = 0;
    size_t infoSize = 4096;
    char* info;

    (void)serverConnectionHandlerID;

    if (!data) {
        return;
    }
    if (pluginShuttingDown) {
        *data = NULL;
        return;
    }

    if (type == PLUGIN_CLIENT && id != 0) {
        clientID = (anyID)id;
        infoSize = 512;
    }

    info = (char*)malloc(infoSize);
    if (!info) {
        *data = NULL;
        return;
    }
    info[0] = '\0';

    if (!ts3_version_format_info(clientID, info, infoSize)) {
        snprintf(info, infoSize, "Conan Exiles Proximity Voice %s", ts3plugin_version());
    }

    /* Peer voice mode (CEMODE) — only shown once that client announced one. */
    if (clientID != 0) {
        const size_t used = strlen(info);
        if (infoSize - used > 48) {
            char mode[64];
            if (ts3_cemode_format_peer(clientID, mode, sizeof(mode))) {
                snprintf(info + used, infoSize - used, "\n%s", mode);
            }
        }
    }

    /* Peer soft identity (CEAUTH) — SteamID64, display only, never trusted. */
    if (clientID != 0) {
        const size_t used = strlen(info);
        if (infoSize - used > 48) {
            char ident[64];
            if (ts3_ceauth_format_peer(clientID, ident, sizeof(ident))) {
                snprintf(info + used, infoSize - used, "\n%s", ident);
            }
        }
    }

    *data = info;
}
