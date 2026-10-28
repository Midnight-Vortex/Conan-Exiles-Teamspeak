#ifndef UI_PLUGIN_UI_COMPAT_H
#define UI_PLUGIN_UI_COMPAT_H

/*
 * Bridge between the legacy F10/HUD UI (plugin.h globals) and the rewrite
 * modules (g_config, voice_modes, server_profile, ts3_adapter, …).
 */

#include "plugin.h"
#include "core/config/config.h"
#include "core/voice/voice_modes.h"

void plugin_ui_init(void);
void plugin_ui_shutdown(void);

/* V8.12: F10 uses ui_cfg() working copy; these refresh legacy mirrors or commit. */
void plugin_ui_sync_from_config(void);
void plugin_ui_sync_to_config(void);

/* Refresh hub/zones/coordinates flags used by the legacy UI messages.
   CALLBACK THREAD ONLY: it rewrites the shared zones[]/races[]/hub* HUD state
   (single-writer rule). Called from any other thread it does not touch that
   state — it just raises the pending-sync flag and requests a wakeup; the
   CEDRAIN dispatcher then runs plugin_ui_flush_pending_sync(). */
void plugin_ui_sync_live_state(void);

/* Deferred sync (review-fix 088, C6).
   plugin_ui_has_pending_sync(): any thread, lock-free — true when a foreign
   thread (pos watcher / HTTP listener tick) asked for a sync that has not run
   yet. It belongs in ts3_pending_work_any().
   plugin_ui_flush_pending_sync(): CALLBACK THREAD ONLY (CEDRAIN dispatcher);
   runs the requested sync once (ticks coalesce) and refreshes the HUD. No-op
   off the callback thread. */
int plugin_ui_has_pending_sync(void);
void plugin_ui_flush_pending_sync(void);

/* Hub profile (zones/races) just applied — refresh HUD text and layout. */
void plugin_ui_on_hub_profile_updated(void);

/* Legacy overlay lifecycle wrappers used by ts3_entry.
   overlay_start() runs on the deferred overlay UI thread, overlay_stop() on the
   callback thread; they are synchronised (088): either start completes and
   stop joins its monitor thread, or start sees the stop request and does
   nothing / reaps its own monitor.
   Shutdown split (V8.8): overlay_stop() joins the monitor thread and posts
   WM_QUIT; the caller joins the overlay UI thread (the thread running start),
   then overlay_finalize() unregisters the window class and deletes the text
   lock (safe only after the join). */
void overlay_start(void);
void overlay_stop(void);
void overlay_finalize(void);
void overlay_ui_mark_thread(void);
void overlay_ui_clear_thread(void);
void overlay_ui_signal_quit(void);

/* Called from the pos watcher or HTTP inject notify thread to keep legacy flags
   fresh. Thread-safe part only: it queues the live-state sync for the callback
   thread (plugin_ui_has_pending_sync / plugin_ui_flush_pending_sync) and
   refreshes the overlay by PostMessage when not on the overlay thread. */
void plugin_ui_on_position_tick(void);

void plugin_ui_on_settings_saved(void);

/* Atomic open/close guard for the legacy settings dialog (F10 + TS configure). */
int config_dialog_try_open(void);
void config_dialog_close(void);
int config_dialog_is_open(void);

/* Open %AppData%\\TS3Client\\plugins in Explorer (any thread; non-blocking). */
void open_ts3_plugins_folder(void);

/* Open Documents\\Conan Exiles TeamSpeak plugin in Explorer (any thread; non-blocking). */
void open_plugin_log_folder(void);

/* Legacy voice_modes.c symbol used by config_files / UI. */
float getVoiceDistanceForMode(uint8_t voiceMode);

/* TS shims referenced by util_base / validation. */
int ts3_plugin_is_on_callback_thread(void);
void ts3_debug_log(const char* message);
void ts3_debug_logf(const char* fmt, ...);
int ts3_adapter_is_connected(void);
void ts3_adapter_print_chat(const char* message);
void ts3_adapter_print_chat_force(const char* message);
void ts3_adapter_request_chat_wakeup(void);

int ts3_plugin_is_proximity_active(void);
void ts3_plugin_apply_proximity_volumes_force(void);

#endif /* UI_PLUGIN_UI_COMPAT_H */
