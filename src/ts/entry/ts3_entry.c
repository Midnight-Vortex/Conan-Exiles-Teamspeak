/*
 * TeamSpeak 3 plugin entry points.
 *
 * Thread contract: every function in this file is called by TeamSpeak on its
 * callback/UI thread. Each event marks the callback thread and forwards to
 * the adapter; nothing here blocks. Exceptions that are safe from any thread:
 *  - ts3plugin_registerPluginID only stores the ID and requests a wakeup.
 *  - the PCM edit events (audio thread) do no API calls and take no locks.
 * CEDRAIN: the dispatcher below runs only for this client's own CEDRAIN (the
 * wakeup thread addresses it to the own client; foreign ones are ignored and
 * the arrival of the own one is reported to the adapter's self-check).
 * Shutdown additionally joins the deferred overlay start thread, see
 * overlay_deferred_start_thread.
 */

#include "ts3_exports.h"

#include "core/util/log.h"
#include "core/util/drain_target.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/proximity_math.h"
#include "core/proximity/player_table.h"
#include "core/proximity/talk_state.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_cemode.h"
#include "ts/proximity/ts3_ceping.h"
#include "ts/proximity/ts3_ceauth.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/proximity/ts3_3d.h"
#include "ts/channel/channel_manage.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/info/ts3_plugin_version.h"
#include "core/voice/voice_modes.h"
#include "core/http/pos_http_server.h"
#include "plugin_modules.h"
#include "sdk/include/teamspeak/public_errors.h"
#include "ts/nick/nick_anonymize.h"
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
/* Owned handle of the overlay UI thread (V8.8) — joined in ts3plugin_shutdown.
   Only the callback thread creates/joins/closes it. */
static HANDLE g_overlayThread = NULL;

/* Shutdown gate of the deferred start (review fix). The deferred thread used
   to check "not shutting down" and call overlay_start() later, so
   overlay_stop() on the callback thread could run in between and the overlay
   (window + monitor thread) would be created AFTER the stop. Two flags make
   the outcome deterministic (Dekker pattern, every access is an Interlocked
   full barrier, no lock is held across window creation):
     - g_overlayStartClosed:     shutdown sets it first; afterwards no NEW
                                 overlay_start() may begin.
     - g_overlayStartInProgress: the thread sets it BEFORE re-checking the
                                 closed flag and clears it after overlay_start()
                                 returned.
   Either the thread sees "closed" and builds nothing, or shutdown sees "in
   progress" and waits (overlay_wait_start_done) until the start finished — so
   overlay_stop() never runs before or during a start, only after it. */
static volatile long g_overlayStartClosed = 0;
static volatile long g_overlayStartInProgress = 0;

static unsigned __stdcall overlay_deferred_start_thread(void* arg) {
    (void)arg;
    for (int waited = 0; waited < 4000 && !pluginShuttingDown; waited += 50) {
        if (InterlockedCompareExchange(&g_overlay_want_immediate, 0, 0)) {
            break;
        }
        Sleep(50);
    }
    InterlockedExchange(&g_overlayStartInProgress, 1);
    if (InterlockedCompareExchange(&g_overlayStartClosed, 0, 0)
        || pluginShuttingDown || !InterlockedCompareExchange(&g_overlay_armed, 0, 0)) {
        InterlockedExchange(&g_overlayStartInProgress, 0);
        return 0;
    }
    overlay_ui_mark_thread();
    overlay_start();
    InterlockedExchange(&g_overlayStartInProgress, 0);

    MSG msg;
    while (!pluginShuttingDown && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* This thread owns the overlay HWND — destroy it HERE, on the owning
       thread, before exiting (V8.8). The old cross-thread SendMessage destroy
       from overlay_stop raced the exiting message loop. */
    destroyVoiceOverlay();

    overlay_ui_clear_thread();
    InterlockedExchange(&g_overlay_armed, 0);
    return 0;
}

static void overlay_schedule_start(void) {
    if (pluginShuttingDown) {
        return; /* a thread started now would outlive the shutdown join */
    }
    if (InterlockedCompareExchange(&g_overlay_armed, 1, 0) != 0) {
        return;
    }
    /* Reap the previous overlay thread's handle (armed was 0, so it has
       exited or is returning right now — the wait is momentary). */
    if (g_overlayThread) {
        WaitForSingleObject(g_overlayThread, INFINITE);
        CloseHandle(g_overlayThread);
        g_overlayThread = NULL;
    }
    g_overlayThread = (HANDLE)_beginthreadex(NULL, 0, overlay_deferred_start_thread, NULL, 0, NULL);
    if (!g_overlayThread) {
        InterlockedExchange(&g_overlay_armed, 0);
    }
}

/* Shutdown: close the start gate (no new overlay_start() can begin) ... */
static void overlay_close_start_gate(void) {
    InterlockedExchange(&g_overlayStartClosed, 1);
}

/* ... and wait for an overlay_start() that already began. Must run before
   overlay_stop(), so the stop can never overtake the start. */
static void overlay_wait_start_done(void) {
    DWORD waitedMs = 0;
    while (InterlockedCompareExchange(&g_overlayStartInProgress, 0, 0)) {
        Sleep(5);
        waitedMs += 5;
        if (waitedMs == 10000) {
            log_write("SHUTDOWN: overlay start slow to finish - waiting");
        }
    }
}

/* JOIN the overlay UI thread (shutdown only, after overlay_stop signalled
   WM_QUIT). The quit signal is re-posted while waiting: PostThreadMessage is
   lost when it races the thread before its message queue exists, and the
   thread checks pluginShuttingDown only between messages. */
static void overlay_join_ui_thread(void) {
    if (!g_overlayThread) {
        return;
    }
    DWORD waitedMs = 0;
    while (WaitForSingleObject(g_overlayThread, 500) != WAIT_OBJECT_0) {
        overlay_ui_signal_quit();
        waitedMs += 500;
        if (waitedMs == 10000) {
            log_write("SHUTDOWN: overlay thread slow to exit - waiting");
        }
    }
    CloseHandle(g_overlayThread);
    g_overlayThread = NULL;
}

static void overlay_request_immediate_start(void) {
    InterlockedExchange(&g_overlay_want_immediate, 1);
    overlay_schedule_start();
}

/* Pos tick (watcher or HTTP inject notify thread): refresh HUD zone via
   plugin_ui_on_position_tick (pos/hub atomics only; overlay uses PostMessage
   when not on the overlay thread), then queue CEPOS and audio snapshots.
   No TS API calls in here. */
static void ts3_on_position_update_common(void) {
    plugin_ui_on_position_tick();
    chan_signal_position_update();
    cepos_signal_send_pending();
    ts3_ceping_signal_send_pending();
    ts3_audio_on_local_position_update();
}

static void ts3_on_local_position_update(void) {
    ts3_on_position_update_common();
}

static void ts3_on_http_position_update(void) {
    ts3_on_position_update_common();
}

/* ---- voice_modes layering hooks (V8.6b) -----------------------------------
   voice_modes.c is pure core and no longer includes ts/ or ui/ headers. This
   is where the ts/ui layer "plugs in the socket": the real chat/CEPOS/overlay
   functions and the server-profile read used by the distance clamp. Wired once
   in ts3plugin_init (callback thread). */
static void voice_hooks_notify_chat(const char* message) {
    if (ts3_is_connected()) {
        displayInChat(message);
    }
}

static void voice_hooks_invalidate_cepos_cache(void) {
    cepos_invalidate_send_cache();
}

static void voice_hooks_signal_send_pending(void) {
    cepos_signal_send_pending();
}

static void voice_hooks_overlay_sync(void) {
    updateVoiceOverlay();
}

static int voice_hooks_get_profile(VoiceModeProfile* out) {
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->active = server_profile_get(&out->hub);
    if (out->active) {
        out->hasRace = server_profile_get_local_race(&out->race);
    }
    return out->active;
}

static int voice_hooks_has_pending_chat(void) {
    return ts3_plugin_has_pending_chat();
}

static void voice_hooks_flush_pending_chat(void) {
    ts3_plugin_flush_pending_chat();
}

/* Announce the new mode to the other clients (CEMODE edge). CEPOS carries the
   distance anyway; this tells peers WHICH mode produced it. */
static void voice_hooks_mode_changed(void) {
    ts3_cemode_signal_send_pending();
}

static void voice_mode_wire_hooks(void) {
    static const VoiceModeHooks hooks = {
        voice_hooks_notify_chat,
        voice_hooks_invalidate_cepos_cache,
        voice_hooks_signal_send_pending,
        voice_hooks_overlay_sync,
        voice_hooks_get_profile,
        voice_hooks_has_pending_chat,
        voice_hooks_flush_pending_chat,
        voice_hooks_mode_changed,
    };
    voice_mode_set_hooks(&hooks);
}

#define PLUGIN_API_VERSION 26

/* TS function table — stored on load, used by later phases. */
static struct TS3Functions g_ts3Functions;

const char* ts3plugin_name(void) {
    return "Conan Exiles";
}

const char* ts3plugin_version(void) {
    return "8.0.15";
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
    /* TeamSpeak registers the ID AFTER ts3plugin_init. A plugin enabled while
       already connected tried its CEVER broadcast in init, before any ID
       existed, and sendPluginCommand was refused. Now that the ID is known,
       wake the drain: it sends the remembered broadcast. Flag + wakeup only —
       no TS API from here (this may not be the callback thread). */
    if (ts3_version_broadcast_pending()) {
        ts3_request_wakeup();
    }
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

static void ts3_bootstrap_connected_session(void);

int ts3plugin_init(void) {
    ts3_thread_mark_callback();
    log_write("BOOT: plugin version %s starting", ts3plugin_version());
    log_write("BOOT: hub GLOBAL NicknameRandomizer + PROFILE nickRand log");
    config_load();
    voice_mode_wire_hooks();
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
    InterlockedExchange(&g_overlayStartClosed, 0);
    plugin_ui_init();
    pos_autodetect_saved_path();
    pos_watcher_set_update_callback(ts3_on_local_position_update);
    /* No tick callback: voice hotkeys are polled ONLY by the key-watcher
       thread (single owner of the arming/debounce state — see voice_modes.h). */
    pos_watcher_start();
    pos_inject_set_notify_callback(ts3_on_http_position_update);
    pos_http_server_start();
    overlay_schedule_start();
    if (ts3_adopt_current_connection() && ts3_is_connected()) {
        log_write("BOOT: already connected — place hub/ingame from live position");
        ts3_bootstrap_connected_session();
    }
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

/* V8.8 fixed shutdown order (doku/01-architektur-v8.md, "Shutdown-Reihenfolge"):
   0. Leave ingame for hub (TS API still valid; 057).
   1. Stop accepting work: audio to passthrough (PCM path inert),
      pluginShuttingDown set (UI/overlay code stops touching HWNDs/GDI),
      deferred overlay start disarmed and its start gate closed.
   2. Stop + JOIN every plugin thread, in dependency order:
      a) key monitor (it spawns settings-dialog threads — stop it first)
      b) settings dialog (WM_CLOSE + join; may still read pos/config state)
      c) pos watcher (stop event + join; deletes its lock AFTER the join,
         safe now because the dialog thread that read pos state is gone)
      d) wait until a deferred overlay_start() that already began has
         returned (overlay_wait_start_done), THEN overlay monitor (join) +
         overlay UI thread (WM_QUIT + join; the overlay thread destroys its
         own HWND), then overlay_finalize() deletes overlayTextLock — only
         safe after that join.
   3. Invalidate snapshots / reset module state (pure, no TS API calls).
   4. Adapter shutdown: joins the wakeup thread and clears the connection
      identity — after this ANY TS API call is a programming error (the
      callback-thread guards log it).
   5. Close the log LAST so every step above can still log. */
void ts3plugin_shutdown(void) {
    log_write("SHUTDOWN: plugin stopping");
    /* 0: still connected — leave ingame for hub before any teardown. */
    chan_leave_ingame_on_plugin_disable();
    /* 1: stop accepting work */
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    plugin_ui_shutdown();
    InterlockedExchange(&g_overlay_armed, 0);
    overlay_close_start_gate();
    /* 2: stop + join all plugin threads */
    removeKeyMonitoring();
    settings_dialog_shutdown();
    pos_http_server_stop();
    pos_watcher_stop();
    overlay_wait_start_done(); /* the deferred start must be finished before the stop */
    overlay_stop();
    overlay_join_ui_thread();
    overlay_finalize();
    /* 3: reset module state */
    player_table_clear();
    talk_state_clear(); /* 067 */
    cepos_reset();
    ts3_cemode_reset();
    ts3_ceping_reset();
    ts3_ceauth_reset();
    ts3d_reset();
    chan_reset();
    server_profile_reset();
    nick_reset();
    ts3_version_reset();
    ts3_audio_reset();
    /* 4: adapter down (joins wakeup thread; no TS API after this) */
    ts3_adapter_shutdown();
    log_write("SHUTDOWN: done");
    /* 5: log closed last */
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

/* Connect, tab switch, or plugin re-enable while already online. */
static void ts3_bootstrap_connected_session(void) {
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
    ts3_cemode_signal_send_pending();
    ts3_ceauth_signal_send_pending();
    ts3_request_wakeup();
}

/* Reset every per-connection cache (disconnect / server switch / tab change). */
static void ts3_reset_connection_state(void) {
    /* Quiesce PCM processing first — the audio thread may still be applying
       the previous session's snapshots while the adapter publishes a new
       active connection (Bugbot: stale gain/pan on tab switch). */
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    player_table_clear();
    talk_state_clear(); /* 067 */
    cepos_reset();
    ts3_cemode_reset();
    ts3_ceping_reset();
    ts3_ceauth_reset();
    ts3_audio_reset();
    ts3d_reset();
    chan_reset();
    server_profile_reset();
    /* CEVER table: replied flags and versions of the old session must not
       leak into the new one (peers would never get our reply again, and stale
       versions showed up in the info panel). Pure reset, no TS API. */
    ts3_version_reset();
    /* Keep g_savedNickname across disconnect/tab reset so hub restore still
       works when the TS client reconnects with the anonymized digits name. */
}

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    (void)errorNumber;
    ts3_thread_mark_callback();

    /* Adapter decides whether this event concerns the active tab; events of
       background tabs (disconnect or connect) are ignored entirely. The audio
       mode is touched only AFTER this decision: a background tab finishing its
       connect used to force the ACTIVE tab's audio to passthrough. For an
       accepted connect, ts3_reset_connection_state() below does the quiesce
       (passthrough is its first statement). */
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
        ts3_bootstrap_connected_session();
        /* Drop stale offline chat (e.g. voice keys pressed before connect). */
        ts3_plugin_clear_pending_chat();
        /* Self-test from Phase 3: exercise queue + wakeup + channel queries.
           Debug logging only — the command writes server channel names into
           plugin.log, which must not happen in production. */
        if (log_is_enabled()) {
            Ts3Command cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = TS3_CMD_LOG_CHANNEL_LIST;
            ts3_cmd_queue_push(&cmd);
            ts3_request_wakeup();
        }
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
            ts3_bootstrap_connected_session();
        }
    }
}

/* Single source of truth for "is there CEDRAIN work waiting?". Used by BOTH the
   drain early-out and the end-of-drain "re-wake if leftover" logic, so a new
   pending source only has to be registered in ONE place.
 *
 * >>> ADD NEW PENDING SOURCES HERE <<<  (V8.2 fix 2 was a forgotten entry.) */
static int ts3_pending_work_any(void) {
    return ts3_cmd_queue_nonempty()
        || plugin_ui_has_pending_sync()
        || ts3_version_broadcast_pending()
        || voice_mode_has_pending_notify()
        || ts3_plugin_has_pending_chat()
        || cepos_send_pending()
        || ts3_cemode_send_pending()
        || ts3_ceping_send_pending()
        || ts3_ceauth_send_pending()
        || ts3_audio_has_pending_unmutes()
        || ts3_audio_has_pending_recompute()
        || ts3d_has_pending()   /* capped 3D pass left clients behind (083), <= 500 ms */
        || chan_has_pending_work();
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

    if (ts3_cemode_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        /* May have armed a one-time reply — send it in the next drain. */
        if (ts3_cemode_send_pending()) {
            ts3_request_wakeup();
        }
        return;
    }

    if (ts3_ceping_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        return;
    }

    if (ts3_ceauth_on_plugin_command(pluginName, pluginCommand, invokerClientID)) {
        /* May have armed a one-time reply — send it in the next drain. */
        if (ts3_ceauth_send_pending()) {
            ts3_request_wakeup();
        }
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
        /* The wakeup thread addresses CEDRAIN to this client only, but a
           server-wide fallback (or an older plugin version) still delivers the
           drains of OTHER clients here: ignore those — running a foreign drain
           costs a full dispatcher pass for nothing. The arrival of our OWN
           CEDRAIN is recorded first: it is the echo the wakeup thread's
           self-check waits for. Own id unknown (0) or sender not reported (0)
           -> never drop a drain (fail-open, see drain_accept_invoker). */
        const anyID localId = ts3_get_local_client_id();
        if (!drain_accept_invoker(localId, invokerClientID)) {
            return;
        }
        if (drain_target_echo_is_own(localId, invokerClientID)) {
            ts3_wakeup_note_own_echo();
        }
        if (!ts3_pending_work_any()) {
            return;
        }
        /* UI mirror sync requested off the callback thread (pos watcher / HTTP
           notify): the mirrors are written here only, see plugin.h. */
        if (plugin_ui_has_pending_sync()) {
            plugin_ui_flush_pending_sync();
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
        }
        if (ts3_cemode_send_pending()) {
            ts3_cemode_flush();
        }
        if (ts3_ceping_send_pending()) {
            ts3_ceping_flush();
        }
        if (ts3_ceauth_send_pending()) {
            ts3_ceauth_flush();
        }
        if (ts3_version_broadcast_pending()) {
            ts3_version_flush_broadcast();
        }
        if (ts3_audio_has_pending_recompute()) {
            ts3_audio_flush_recomputes();
        }
        /* Refresh remote players' TS 3D positions on EVERY proximity drain, not
           only when our own CEPOS send was pending. Incoming remote CEPOS (no
           local send pending) used to leave their 3D positions stale. ts3d_apply
           has a 20 Hz internal throttle (TS3D_APPLY_MIN_MS) + epsilon dedup, so
           the idle cost here is a timestamp check. */
        if (ts3_audio_get_mode() == TS3_AUDIO_PROXIMITY) {
            ts3d_apply();
        }
        server_profile_tick();
        chan_tick();
        if (ts3_audio_has_pending_unmutes()) {
            ts3_audio_flush_unmutes();
        }
        ts3_audio_diag_flush(); /* step-0 channel layout log (on change only) */
        /* Re-wake if any work is still pending — leftover from a per-drain
           budget (e.g. dirty-client recomputes capped at
           TS3_RECOMPUTE_DRAIN_BUDGET) or queued from another thread while we
           drained. Chat is latency-sensitive, so it gets the urgent wakeup. */
        if (ts3_plugin_has_pending_chat()) {
            ts3_request_wakeup_urgent();
        }
        else if (ts3_pending_work_any()) {
            ts3_request_wakeup();
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

void ts3plugin_onEditPostProcessVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {
    /* Audio thread, after TS 3D positioning — same rules as playback above. */
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    ts3_audio_process_postprocess(clientID, samples, sampleCount, channels,
        channelSpeakerArray, channelFillMask);
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
        ts3_cemode_clear_client(clientID);
        ts3_ceping_clear_client(clientID);
        ts3_ceauth_clear_client(clientID);
        player_table_remove(clientID);
        talk_state_clear_client(clientID); /* 067 */
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

/* Client timed out. TS reports the client as gone (newChannelID 0): same
   cleanup as a normal leave. The own client timing out is followed by the
   DISCONNECTED status event, the own-client branch of ts3_on_client_move
   handles it like any own move. */
void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* timeoutMessage) {
    (void)oldChannelID;
    (void)newChannelID;
    (void)visibility;
    (void)timeoutMessage;
    ts3_on_client_move(serverConnectionHandlerID, clientID, 0);
}

/* Client kicked from its channel: it stays on the server and lands in another
   channel (newChannelID != 0) — exactly a move, including the own client. */
void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
    (void)oldChannelID;
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;
    ts3_on_client_move(serverConnectionHandlerID, clientID, newChannelID);
}

/* Client kicked from the server: gone (newChannelID 0), like a leave. */
void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
    (void)oldChannelID;
    (void)newChannelID;
    (void)visibility;
    (void)kickerID;
    (void)kickerName;
    (void)kickerUniqueIdentifier;
    (void)kickMessage;
    ts3_on_client_move(serverConnectionHandlerID, clientID, 0);
}

/* Server answer to a command that carried one of OUR plugin return codes
   (currently: requestClientMove). Return value contract (SDK): 1 = the plugin
   handled it, TeamSpeak prints nothing; 0 = TeamSpeak continues as usual
   (prints the error). Success is swallowed, failure is reported to the
   channel module and still printed by TeamSpeak exactly as before. Codes that
   are not ours (or NULL/empty) return 0 = unchanged behaviour. */
int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage) {
    (void)extraMessage;

    ts3_thread_mark_callback();
    if (!returnCode || !returnCode[0]
        || serverConnectionHandlerID != ts3_get_active_connection()) {
        return 0;
    }
    if (!ts3_consume_move_return_code(returnCode)) {
        return 0;
    }
    if (error == ERROR_ok) {
        return 1;
    }
    log_write("TS-EVT: requestClientMove failed err=%u (%s)", error,
        errorMessage ? errorMessage : "");
    chan_on_move_failed(error);
    return 0;
}

/* Channel list changes (channel created / deleted / moved / edited): the
   channel and profile modules cache channel IDs (hub, ingame, Root). They
   keep the old IDs until the next re-resolve, so just tell them and make sure
   a drain follows. 0 = unknown channel. Callback thread, active tab only. */
static void ts3_on_channel_list_changed(uint64 serverConnectionHandlerID, uint64 channelID) {
    ts3_thread_mark_callback();
    if (serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    chan_on_channel_list_changed(channelID);
    server_profile_on_channel_list_changed(channelID);
    if (chan_has_pending_work()) {
        ts3_request_wakeup();
    }
}

/* Fires once per existing channel while the channel list is received and for
   channels that appear later. */
void ts3plugin_onNewChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID) {
    (void)channelParentID;
    ts3_on_channel_list_changed(serverConnectionHandlerID, channelID);
}

void ts3plugin_onNewChannelCreatedEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
    (void)channelParentID;
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;
    ts3_on_channel_list_changed(serverConnectionHandlerID, channelID);
}

void ts3plugin_onDelChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;
    ts3_on_channel_list_changed(serverConnectionHandlerID, channelID);
}

void ts3plugin_onChannelMoveEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 newChannelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
    (void)newChannelParentID;
    (void)invokerID;
    (void)invokerName;
    (void)invokerUniqueIdentifier;
    ts3_on_channel_list_changed(serverConnectionHandlerID, channelID);
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
    /* A renamed Root/hub/ingame channel invalidates the cached channel IDs.
       After the edit handling above: that one still relies on the old cache. */
    ts3_on_channel_list_changed(serverConnectionHandlerID, channelID);
}

/* PTT indicator on the range HUD: highlight border while the local client
   is transmitting (STATUS_TALKING / STATUS_TALKING_WHILE_DISABLED). */
void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
    (void)isReceivedWhisper;

    ts3_thread_mark_callback();
    if (pluginShuttingDown || serverConnectionHandlerID != ts3_get_active_connection()) {
        return;
    }
    if (clientID == 0) {
        return;
    }
    const BOOL talking = (status == STATUS_TALKING || status == STATUS_TALKING_WHILE_DISABLED);
    const int isLocal = (clientID == ts3_get_local_client_id());
    /* 067: every client's talk status (any PTT key / voice activation) for the
       mod's mouth animation. */
    talk_state_set(clientID, talking, isLocal);
    if (!isLocal) {
        return;
    }
    setOverlayHighlightState(clientID, serverConnectionHandlerID, talking);
}
