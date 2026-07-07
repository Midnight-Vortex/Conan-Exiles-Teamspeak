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

/* Pull g_config (+ live hub/pos state) into legacy globals. */
void plugin_ui_sync_from_config(void);

/* Push legacy globals into g_config and persist. */
void plugin_ui_sync_to_config(void);

/* Refresh hub/zones/coordinates flags used by the legacy UI messages. */
void plugin_ui_sync_live_state(void);

/* Hub profile (zones/races) just applied — refresh HUD text and layout. */
void plugin_ui_on_hub_profile_updated(void);

/* Legacy overlay lifecycle wrappers used by ts3_entry. */
void overlay_start(void);
void overlay_stop(void);
void overlay_ui_mark_thread(void);
void overlay_ui_clear_thread(void);
void overlay_ui_signal_quit(void);

/* Called from the pos watcher (PLUGIN_POLL_INTERVAL_MS) to keep legacy flags fresh. */
void plugin_ui_on_position_tick(void);

void plugin_ui_on_settings_saved(void);

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
