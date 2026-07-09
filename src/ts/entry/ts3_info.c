#include "ts3_exports.h"

#include "ts/info/ts3_plugin_version.h"
#include "plugin_modules.h"
#include "ui/plugin_ui_compat.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>

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

    *data = info;
}
