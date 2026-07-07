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
#include "core/proximity/player_table.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/proximity/ts3_3d.h"
#include "core/channel/channel_manage.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/voice/voice_modes.h"
#include "core/nick/nick_anonymize.h"
#include "ui/overlay/voice_overlay.h"
#include "ui/dialogs/ui_settings.h"

#include <string.h>

/* Pos watcher tick (watcher thread): poll voice hotkeys, queue own CEPOS
   send and refresh the audio snapshots for all known speakers. No TS API
   calls in here. */
static void ts3_on_local_position_update(void) {
    voice_mode_hotkey_poll();
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
    pos_autodetect_saved_path();
    pos_watcher_set_update_callback(ts3_on_local_position_update);
    pos_watcher_start();
    overlay_start();
    if (log_is_enabled()) {
        prox_math_self_test();
    }
    log_write("BOOT: init done");
    return 0;
}

/* 14.1 fixed shutdown sequence:
   1. audio gate -> passthrough (PCM path inert, no snapshot reads matter)
   2. UI thread down (overlay + settings dialog)
   3. pos watcher thread down (no more update callbacks)
   4. module state cleared (no TS API calls in any of these)
   5. adapter down (command queue emptied, connection flags cleared)
   6. log closed LAST so every step above can still log. */
void ts3plugin_shutdown(void) {
    log_write("SHUTDOWN: plugin stopping");
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    if (ui_settings_has_pending_apply()) {
        ui_settings_flush_apply();
    }
    overlay_stop();
    pos_watcher_stop();
    player_table_clear();
    cepos_reset();
    ts3d_reset();
    chan_reset();
    server_profile_reset();
    nick_reset();
    ts3_audio_reset();
    ts3_adapter_shutdown();
    log_write("SHUTDOWN: done");
    log_close();
}

/* Reset every per-connection cache (disconnect / server switch / tab change). */
static void ts3_reset_connection_state(void) {
    /* Quiesce PCM processing first — the audio thread may still be applying
       the previous session's snapshots while the adapter publishes a new
       active connection (Bugbot: stale gain/pan on tab switch). */
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    player_table_clear();
    cepos_reset();
    ts3_audio_reset();
    ts3d_reset();
    chan_reset();
    server_profile_reset();
    /* Keep g_savedNickname across disconnect/tab reset so hub restore still
       works when the TS client reconnects with the anonymized digits name. */
}

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    (void)errorNumber;
    ts3_thread_mark_callback();

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }

    /* Adapter decides whether this event concerns the active tab; events of
       background tabs (disconnect or connect) are ignored entirely. */
    const int accepted = ts3_on_connect_status_changed(serverConnectionHandlerID, newStatus);
    if (!accepted) {
        return;
    }

    if (newStatus == STATUS_DISCONNECTED) {
        ts3_reset_connection_state();
        return;
    }

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        /* 14.2 server switch: every per-connection cache starts empty. */
        ts3_reset_connection_state();
        /* Remember the real name now — needed for the hub restore when the
           plugin later finds an already-anonymized nickname (relog case). */
        nick_on_connected();
        chan_tick();
        /* Self-test from Phase 3: exercise queue + wakeup + channel queries. */
        Ts3Command cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = TS3_CMD_LOG_CHANNEL_LIST;
        ts3_cmd_queue_push(&cmd);
        ts3_request_wakeup();
    }
}

void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    ts3_thread_mark_callback();
    if (serverConnectionHandlerID == 0
        || serverConnectionHandlerID == ts3_get_active_connection()) {
        return;
    }
    /* Drop stale audio/player state BEFORE adopting the new tab's connection
       ID so the playback thread cannot apply the old session to the new tab. */
    ts3_reset_connection_state();
    if (ts3_on_active_server_changed(serverConnectionHandlerID)) {
        if (ts3_is_connected()) {
            nick_on_connected();
            chan_tick();
            ts3_request_wakeup();
        }
    }
}

void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
    (void)invokerName;
    (void)invokerUniqueIdentity;

    ts3_thread_mark_callback();

    /* 14.2 multi-tab hardening: only the active connection drives the plugin. */
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }

    if (cepos_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        if (cepos_send_pending()) {
            cepos_flush();
        }
        ts3d_apply();
        server_profile_tick();
        chan_tick();
        if (ts3_audio_has_pending_unmutes()) {
            ts3_audio_flush_unmutes();
        }
        return;
    }

    if (pluginCommand && strncmp(pluginCommand, "CEDRAIN:", 8) == 0) {
        /* Each step is gated — foreign clients' wakeups must not run our full
           drain (cepos/3D/unmute) when this client has nothing pending. */
        if (ts3_cmd_queue_nonempty()) {
            ts3_cmd_queue_drain();
        }
        if (ui_settings_has_pending_apply()) {
            ui_settings_flush_apply();
        }
        if (voice_mode_has_pending_notify()) {
            voice_mode_flush_notify();
        }
        if (cepos_send_pending()) {
            cepos_flush();
            ts3d_apply();
        }
        server_profile_tick();
        chan_tick();
        if (ts3_audio_has_pending_unmutes()) {
            ts3_audio_flush_unmutes();
        }
    }
}

void ts3plugin_onEditPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels) {
    /* Audio thread — no API calls, no locks, snapshot reads only. Voice from
       other server tabs passes through untouched (14.2). */
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    ts3_audio_process_playback(clientID, samples, sampleCount, channels);
}

void ts3plugin_onCustom3dRolloffCalculationClientEvent(uint64 serverConnectionHandlerID, anyID clientID, float distance, float* volume) {
    (void)serverConnectionHandlerID;
    /* Audio thread — pure function, no API calls, no locks. */
    ts3d_on_custom_rolloff(clientID, distance, volume);
}

/* Shared handler for both move event flavors (self move / moved by admin). */
static void ts3_on_client_move(uint64 serverConnectionHandlerID, anyID clientID, uint64 newChannelID) {
    ts3_thread_mark_callback();
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    if (clientID != 0 && clientID == ts3_get_local_client_id()) {
        chan_on_own_move(newChannelID);
        return;
    }
    /* Remote client left our visibility (channel 0 = disconnected). */
    if (newChannelID == 0 && clientID != 0) {
        player_table_remove(clientID);
        ts3_audio_invalidate_client(clientID);
    }
    chan_tick();
}

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage) {
    (void)oldChannelID;
    (void)visibility;
    (void)moveMessage;
    ts3_on_client_move(serverConnectionHandlerID, clientID, newChannelID);
}

void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID moverID, const char* moverName, const char* moverUniqueIdentifier, const char* moveMessage) {
    (void)oldChannelID;
    (void)visibility;
    (void)moverID;
    (void)moverName;
    (void)moverUniqueIdentifier;
    (void)moveMessage;
    ts3_on_client_move(serverConnectionHandlerID, clientID, newChannelID);
}

void ts3plugin_onChannelDescriptionUpdateEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
    ts3_thread_mark_callback();
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    server_profile_on_description_update(channelID);
}
