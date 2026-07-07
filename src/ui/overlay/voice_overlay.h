#ifndef UI_OVERLAY_VOICE_OVERLAY_H
#define UI_OVERLAY_VOICE_OVERLAY_H

/*
 * Phase 13.3 — in-game voice HUD (mode + zone), click-through layered window.
 *
 * The overlay runs on its OWN UI thread with a private message loop; the
 * window is created and destroyed only there (Win32 requirement — the old
 * plugin crashed on cross-thread DestroyWindow). A 250 ms timer on that
 * thread polls the thread-safe getters (voice mode, Pos.txt validity,
 * server profile zones) and repaints when something changed. It never
 * touches the TS API.
 *
 * Thread contract:
 *  - overlay_start / overlay_stop: TS callback thread (init/shutdown).
 *    Stop blocks until the UI thread has exited (max ~2 s).
 *  - Everything else is internal to the overlay thread.
 */

void overlay_start(void);
void overlay_stop(void);

void plugin_ui_on_settings_saved(void);

/* Thread id hook retained for legacy UI helpers (0 when unused). */
unsigned long overlay_ui_thread_id(void);

#endif /* UI_OVERLAY_VOICE_OVERLAY_H */
