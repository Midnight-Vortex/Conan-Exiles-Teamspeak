#ifndef UI_OVERLAY_VOICE_OVERLAY_H
#define UI_OVERLAY_VOICE_OVERLAY_H

/*
 * Phase 13.3 — in-game voice HUD (mode + zone), click-through layered window.
 *
 * Window ownership: the HUD window is created, painted and destroyed ONLY on
 * the overlay UI thread (Win32 requirement — the old plugin crashed on
 * cross-thread DestroyWindow). That thread is the deferred start thread in
 * ts3_entry.c (overlay_deferred_start_thread): it calls overlay_start(), then
 * pumps messages until WM_QUIT / pluginShuttingDown, then destroys the window
 * itself. There is NO timer: repainting is event driven. Other threads post
 * WM_VOICEOVERLAY_REFRESH / _REPOSITION / _RESIZE; WM_PAINT reads the shared
 * HUD state (voice mode, currentZoneIndex/zones/races mirrors, speaker text
 * under overlayTextLock via TryEnter). Nothing in here touches the TS API.
 *
 * Thread contract:
 *  - overlay_start(): OVERLAY UI THREAD (called by the deferred start thread,
 *    not the TS callback thread). Creates the window and the resolution
 *    monitor thread. Synchronised with overlay_stop(): either start finishes
 *    and stop joins the monitor, or start sees the stop request and does
 *    nothing / reaps its own monitor — no monitor outlives shutdown.
 *  - overlay_stop(): TS CALLBACK THREAD at shutdown. Joins the resolution
 *    monitor thread (<= ~2 s, its Sleep period) and posts WM_QUIT; it does NOT
 *    destroy the window. The caller must then join the overlay UI thread and
 *    only afterwards call overlay_finalize() (text-lock teardown, window class
 *    unregistration).
 *  - createVoiceOverlay / destroyVoiceOverlay / voice_overlay_unregister_class:
 *    overlay UI thread (window create/destroy), resp. callback thread after the
 *    overlay thread was joined (class unregister).
 *  - updateVoiceOverlay, updateVoiceOverlayVisibility,
 *    voice_overlay_refresh_theme/_position/_size, repositionVoiceOverlay:
 *    ANY thread — pos watcher / HTTP listener (plugin_ui_on_position_tick),
 *    key monitor (voice-mode hotkeys), settings-dialog thread, TS callback
 *    thread. Off the overlay thread they only PostMessage (never touch the
 *    window or GDI); on it they act directly. All are no-ops once
 *    pluginShuttingDown is set.
 *  - Resolution monitor thread (overlayMonitorThread): polls the screen size
 *    every 2 s and calls repositionVoiceOverlay() (PostMessage only).
 */

void overlay_start(void);
void overlay_stop(void);

void createVoiceOverlay(void);
void updateVoiceOverlay(void);
void updateVoiceOverlayVisibility(void);
void voice_overlay_refresh_theme(void);
void voice_overlay_refresh_position(void);
void voice_overlay_refresh_size(void);

/* Unregister the HUD window class (shutdown, after the overlay thread join). */
void voice_overlay_unregister_class(void);

void plugin_ui_on_settings_saved(void);

/* Thread id hook retained for legacy UI helpers (0 when unused). */
unsigned long overlay_ui_thread_id(void);

#endif /* UI_OVERLAY_VOICE_OVERLAY_H */
