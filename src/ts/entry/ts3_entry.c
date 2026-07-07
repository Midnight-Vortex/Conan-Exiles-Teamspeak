/*
 * TeamSpeak 3 plugin entry points.
 *
 * Thread contract: every function in this file is called by TeamSpeak on its
 * callback/UI thread. Each event marks the callback thread and forwards to
 * the adapter; nothing here blocks.
 */

#include "ts3_exports.h"

#include "core/util/log.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/proximity_math.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"

#include <string.h>

/* Pos watcher tick (watcher thread): queue own CEPOS send and refresh the
   audio snapshots for all known speakers. No TS API calls in here. */
static void ts3_on_local_position_update(void) {
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
}

#define PLUGIN_API_VERSION 26

/* TS function table — stored on load, used by later phases. */
static struct TS3Functions g_ts3Functions;

const char* ts3plugin_name(void) {
    return "Conan Exiles";
}

const char* ts3plugin_version(void) {
    return "7.0.0-dev";
}

int ts3plugin_apiVersion(void) {
    return PLUGIN_API_VERSION;
}

const char* ts3plugin_author(void) {
    return "Dino_Rex (TeamSpeak port)";
}

const char* ts3plugin_description(void) {
    return "Proximity voice for Conan Exiles via Pos.txt mod.";
}

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    g_ts3Functions = funcs;
    ts3_adapter_set_functions(&funcs);
}

void ts3plugin_registerPluginID(const char* id) {
    ts3_adapter_set_plugin_id(id);
}

int ts3plugin_init(void) {
    ts3_thread_mark_callback();
    log_write("BOOT: plugin version %s starting", ts3plugin_version());
    config_load();
    pos_watcher_set_update_callback(ts3_on_local_position_update);
    pos_watcher_start();
    if (log_is_enabled()) {
        prox_math_self_test();
    }
    log_write("BOOT: init done");
    return 0;
}

void ts3plugin_shutdown(void) {
    log_write("SHUTDOWN: plugin stopping");
    pos_watcher_stop();
    ts3_adapter_shutdown();
    log_close();
}

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    (void)errorNumber;
    ts3_thread_mark_callback();
    ts3_on_connect_status_changed(serverConnectionHandlerID, newStatus);

    if (newStatus == STATUS_DISCONNECTED) {
        cepos_reset();
        ts3_audio_reset();
        return;
    }

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        cepos_reset();
        /* Self-test from Phase 3: exercise queue + wakeup + channel queries. */
        Ts3Command cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = TS3_CMD_LOG_CHANNEL_LIST;
        ts3_cmd_queue_push(&cmd);
        ts3_request_wakeup();
    }
}

void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    (void)serverConnectionHandlerID;
    ts3_thread_mark_callback();
}

void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
    (void)serverConnectionHandlerID;
    (void)invokerName;
    (void)invokerUniqueIdentity;

    ts3_thread_mark_callback();

    if (cepos_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        ts3_audio_flush_unmutes();
        return;
    }

    if (pluginCommand && strncmp(pluginCommand, "CEDRAIN:", 8) == 0) {
        ts3_cmd_queue_drain();
        cepos_flush();
        ts3_audio_flush_unmutes();
    }
}

void ts3plugin_onEditPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels) {
    (void)serverConnectionHandlerID;
    /* Audio thread — no API calls, no locks, snapshot reads only. */
    ts3_audio_process_playback(clientID, samples, sampleCount, channels);
}
