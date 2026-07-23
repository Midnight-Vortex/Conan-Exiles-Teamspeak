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
#include "ts/info/ts3_plugin_version.h"
#include "core/voice/voice_modes.h"
#include "plugin_modules.h"
#include "core/nick/nick_anonymize.h"
#include "ui/overlay/voice_overlay.h"
#include "ui/plugin_ui_compat.h"
#include "plugin_modules.h"
#include "plugin.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <process.h>

/* Deferred overlay/key watcher: legacy createVoiceOverlay() builds a TOPMOST
   HWND during ts3plugin_init and can prevent the Qt main window from appearing
   (TS log stops after SERVERVIEW, process stays alive with no MainWindowTitle). */
static volatile long g_overlay_armed = 0;
static volatile long g_overlay_want_immediate = 0;

static unsigned __stdcall overlay_deferred_start_thread(void* arg) {
    (void)arg;
    for (int waited = 0; waited < 4000 && !pluginShuttingDown; waited += 50) {
        if (InterlockedCompareExchange(&g_overlay_want_immediate, 0, 0)) {
            break;
        }
        Sleep(50);
    }
    if (pluginShuttingDown || !InterlockedCompareExchange(&g_overlay_armed, 0, 0)) {
        return 0;
    }
    overlay_ui_mark_thread();
    overlay_start();

    MSG msg;
    while (!pluginShuttingDown && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    overlay_ui_clear_thread();
    InterlockedExchange(&g_overlay_armed, 0);
    return 0;
}

static void overlay_schedule_start(void) {
    if (InterlockedCompareExchange(&g_overlay_armed, 1, 0) != 0) {
        return;
    }
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, overlay_deferred_start_thread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    }
    else {
        InterlockedExchange(&g_overlay_armed, 0);
    }
}

static void overlay_request_immediate_start(void) {
    InterlockedExchange(&g_overlay_want_immediate, 1);
    overlay_schedule_start();
}

/* Pos watcher tick (watcher thread): queue own CEPOS send and refresh the
   audio snapshots for all known speakers. No TS API calls in here. */
static void ts3_on_local_position_update(void) {
    plugin_ui_on_position_tick();
    chan_signal_position_update();
    cepos_signal_send_pending();
    ts3_audio_on_local_position_update();
}

#define PLUGIN_API_VERSION 26

/* TS function table — stored on load, used by later phases. */
static struct TS3Functions g_ts3Functions;

const char* ts3plugin_name(void) {
    return "Conan Exiles";
}

const char* ts3plugin_version(void) {
    return "7.0.4";
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

static struct PluginHotkey* ts3_create_hotkey(const char* keyword, const char* description) {
    struct PluginHotkey* hotkey = (struct PluginHotkey*)malloc(sizeof(struct PluginHotkey));
    if (!hotkey) {
        return NULL;
    }
    strncpy(hotkey->keyword, keyword, PLUGIN_HOTKEY_BUFSZ - 1);
    hotkey->keyword[PLUGIN_HOTKEY_BUFSZ - 1] = '\0';
    strncpy(hotkey->description, description, PLUGIN_HOTKEY_BUFSZ - 1);
    hotkey->description[PLUGIN_HOTKEY_BUFSZ - 1] = '\0';
    return hotkey;
}

void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys) {
    struct PluginHotkey** list;

    if (!hotkeys) {
        return;
    }
    list = (struct PluginHotkey**)malloc(sizeof(struct PluginHotkey*) * 5);
    if (!list) {
        *hotkeys = NULL;
        return;
    }
    list[0] = ts3_create_hotkey("voice_toggle", "Conan: Voice mode toggle");
    list[1] = ts3_create_hotkey("voice_whisper", "Conan: Whisper mode");
    list[2] = ts3_create_hotkey("voice_normal", "Conan: Normal mode");
    list[3] = ts3_create_hotkey("voice_shout", "Conan: Shout mode");
    list[4] = NULL;
    if (!list[0] || !list[1] || !list[2] || !list[3]) {
        for (int i = 0; i < 4; i++) {
            if (list[i]) {
                free(list[i]);
            }
        }
        free(list);
        *hotkeys = NULL;
        return;
    }
    *hotkeys = list;
}

void ts3plugin_onHotkeyEvent(const char* keyword) {
    if (!keyword || pluginShuttingDown) {
        return;
    }
    ts3_thread_mark_callback();

    if (strcmp(keyword, "voice_toggle") == 0) {
        if (g_config.enableVoiceToggle) {
            voice_mode_notify_hotkey(g_config.voiceToggleKey);
            voice_mode_toggle();
        }
    }
    else if (strcmp(keyword, "voice_whisper") == 0) {
        voice_mode_notify_hotkey(g_config.whisperKey);
        voice_mode_apply(VOICE_MODE_WHISPER);
    }
    else if (strcmp(keyword, "voice_normal") == 0) {
        voice_mode_notify_hotkey(g_config.normalKey);
        voice_mode_apply(VOICE_MODE_NORMAL);
    }
    else if (strcmp(keyword, "voice_shout") == 0) {
        voice_mode_notify_hotkey(g_config.shoutKey);
        voice_mode_apply(VOICE_MODE_SHOUT);
    }
    else {
        return;
    }
}

int ts3plugin_init(void) {
    ts3_thread_mark_callback();
    log_write("BOOT: plugin version %s starting", ts3plugin_version());
    config_load();
    {
        const wchar_t* logPath = log_get_path();
        if (logPath) {
            char pathUtf8[MAX_PATH * 3];
            const int n = WideCharToMultiByte(CP_UTF8, 0, logPath, -1, pathUtf8, (int)sizeof(pathUtf8), NULL, NULL);
            if (n > 0) {
                log_write("BOOT: log file: %s", pathUtf8);
            }
        }
    }
    plugin_ui_init();
    pos_autodetect_saved_path();
    pos_watcher_set_update_callback(ts3_on_local_position_update);
    /* No tick callback: voice hotkeys are polled ONLY by the key-watcher
       thread (single owner of the arming/debounce state — see voice_modes.h). */
    pos_watcher_start();
    overlay_schedule_start();
    if (log_is_enabled()) {
        prox_math_self_test();
    }
    log_write("BOOT: init done");
    {
        char bootMsg[160];
        snprintf(bootMsg, sizeof(bootMsg),
            "Conan Exiles proximity plugin loaded successfully (version %s)",
            ts3plugin_version());
        ts3_log_client(bootMsg);
    }
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
    plugin_ui_shutdown();
    InterlockedExchange(&g_overlay_armed, 0);
    overlay_stop();
    pos_watcher_stop();
    player_table_clear();
    cepos_reset();
    ts3d_reset();
    chan_reset();
    server_profile_reset();
    nick_reset();
    ts3_version_reset();
    ts3_audio_reset();
    ts3_adapter_shutdown();
    log_write("SHUTDOWN: done");
    log_close();
}

static void ts3_sync_overlay_channel_state(uint64 knownChannelID) {
    if (knownChannelID != 0) {
        ts3LocalChannelID = (mumble_channelid_t)knownChannelID;
    }
    else if (ts3_is_connected()) {
        const anyID localID = ts3_get_local_client_id();
        const uint64 ch = localID ? ts3_get_channel_of_client(localID) : 0;
        ts3LocalChannelID = ch ? (mumble_channelid_t)ch : -1;
    }
    else {
        ts3LocalChannelID = -1;
    }
    plugin_ui_sync_live_state();
    updateVoiceOverlayVisibility();
    updateVoiceOverlay();
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
        ts3_sync_overlay_channel_state(0);
        return;
    }

    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        /* 14.2 server switch: every per-connection cache starts empty. */
        ts3_reset_connection_state();
        /* Remember the real name now — needed for the hub restore when the
           plugin later finds an already-anonymized nickname (relog case). */
        nick_on_connected();
        plugin_ui_sync_from_config();
        plugin_ui_sync_live_state();
        pos_autodetect_saved_path();
        plugin_ui_sync_from_config();
        server_profile_tick();
        chan_tick();
        overlay_request_immediate_start();
        ts3_sync_overlay_channel_state(0);
        ts3_version_broadcast();
        /* Drop stale offline chat (e.g. voice keys pressed before connect). */
        ts3_plugin_clear_pending_chat();
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
            plugin_ui_sync_from_config();
            plugin_ui_sync_live_state();
            server_profile_tick();
            chan_tick();
            overlay_request_immediate_start();
            ts3_sync_overlay_channel_state(0);
            ts3_version_broadcast();
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

    if (ts3_version_on_plugin_command(pluginName, pluginCommand, invokerClientID,
            invokerName, invokerUniqueIdentity)) {
        return;
    }

    if (cepos_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        /* cepos_on_plugin_command updates the player table and queues a batched
           audio recompute (dirty flag). Defer own send, 3D, channel, and unmute
           work to CEDRAIN — never run the full drain per packet (200+ players
           @ 1 Hz CEPOS would flood the callback thread). */
        if (cepos_send_pending() || ts3_audio_has_pending_unmutes()
            || ts3_audio_has_pending_recompute()) {
            ts3_request_wakeup();
        }
        return;
    }

    if (pluginCommand && strncmp(pluginCommand, "CEDRAIN:", 8) == 0) {
        if (!ts3_cmd_queue_nonempty()
            && !voice_mode_has_pending_notify()
            && !ts3_plugin_has_pending_chat()
            && !cepos_send_pending()
            && !ts3_audio_has_pending_unmutes()
            && !ts3_audio_has_pending_recompute()
            && !chan_has_pending_work()) {
            return;
        }
        if (ts3_cmd_queue_nonempty()) {
            ts3_cmd_queue_drain();
        }
        if (voice_mode_has_pending_notify()) {
            voice_mode_flush_notify();
        }
        if (ts3_plugin_has_pending_chat()) {
            ts3_plugin_flush_pending_chat();
        }
        if (cepos_send_pending()) {
            cepos_flush();
            ts3d_apply();
        }
        if (ts3_audio_has_pending_recompute()) {
            ts3_audio_flush_recomputes();
        }
        server_profile_tick();
        chan_tick();
        if (ts3_audio_has_pending_unmutes()) {
            ts3_audio_flush_unmutes();
        }
        /* Chat may be queued from a hotkey thread while we were draining. */
        if (ts3_plugin_has_pending_chat()) {
            ts3_request_wakeup_urgent();
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
        ts3_sync_overlay_channel_state(newChannelID);
        chan_tick(); /* re-evaluate hub/ingame placement after manual moves */
        return;
    }
    if (newChannelID == 0 && clientID != 0) {
        ts3_version_clear_client(clientID);
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
    /* Profile apply may have written first-connection defaults into the
       config — mirror them into the legacy UI globals (only when a profile
       was really applied; this event fires for every channel edit). */
    if (server_profile_on_description_update(channelID)) {
        plugin_ui_sync_from_config();
        plugin_ui_on_hub_profile_updated();
    }
}

/* requestChannelDescription completes via onUpdateChannelEvent (SDK contract);
   onChannelDescriptionUpdateEvent only fires on live edits. Without this
   export the Root description request always timed out. */
void ts3plugin_onUpdateChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
    ts3_thread_mark_callback();
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    if (server_profile_on_description_update(channelID)) {
        plugin_ui_sync_from_config();
        plugin_ui_on_hub_profile_updated();
    }
}

void ts3plugin_onUpdateChannelEditedEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;

    ts3_thread_mark_callback();
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    if (server_profile_on_channel_edited(channelID)) {
        plugin_ui_sync_from_config();
        plugin_ui_on_hub_profile_updated();
    }
}

/* PTT indicator on the range HUD: highlight border while the local client
   is transmitting (STATUS_TALKING / STATUS_TALKING_WHILE_DISABLED). */
void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
    (void)isReceivedWhisper;

    ts3_thread_mark_callback();
    if (pluginShuttingDown || serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    if (clientID == 0 || clientID != ts3_get_local_client_id()) {
        return;
    }

    const BOOL talking = (status == STATUS_TALKING || status == STATUS_TALKING_WHILE_DISABLED);
    setOverlayHighlightState(clientID, serverConnectionHandlerID, talking);
}
