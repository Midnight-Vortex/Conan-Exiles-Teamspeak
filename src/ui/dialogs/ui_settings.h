#ifndef UI_DIALOGS_UI_SETTINGS_H
#define UI_DIALOGS_UI_SETTINGS_H

/*
 * Phase 13.1 — settings dialog (path, distances, hotkeys, toggles, HUD).
 *
 * The dialog lives on the overlay UI thread (never on the TS callback
 * thread). It edits a STAGING copy of the config; pressing "Speichern"
 * publishes the copy and the TS callback thread applies it on the next
 * CEDRAIN drain (g_config is written on the callback thread only).
 *
 * Thread contract:
 *  - ui_settings_hotkey_poll / _handle_message / _destroy: UI thread ONLY
 *    (called from voice_overlay.c).
 *  - ui_settings_flush_apply: TS callback thread ONLY.
 */

#include <windows.h>

/* Poll the config hotkey (F10 default) — toggles the dialog. UI thread. */
void ui_settings_hotkey_poll(void);

/* Give the dialog a chance to handle a message (tab order, enter/escape).
   Returns 1 when consumed. UI thread. */
int ui_settings_handle_message(MSG* msg);

/* Destroy the dialog window if open. UI thread (shutdown path). */
void ui_settings_destroy(void);

/* Apply a pending "Speichern" to g_config + plugin.cfg. TS callback thread. */
void ui_settings_flush_apply(void);

#endif /* UI_DIALOGS_UI_SETTINGS_H */
