#ifndef CORE_VOICE_VOICE_MODES_H
#define CORE_VOICE_VOICE_MODES_H

/*
 * Phase 11 — voice modes (whisper / normal / shout) + hotkeys.
 *
 * The mode only changes the OWN broadcast voice distance (CEPOS field);
 * remote clients attenuate the speaker with it. Zone overrides beat global
 * config, the server profile clamps the result.
 *
 * Thread contract:
 *  - voice_mode_hotkey_poll: key-monitor thread ONLY (single owner of the
 *    arming/debounce state; PLUGIN_POLL_INTERVAL_MS). Only touches atomics
 *    + Win32 key state; never the TS API.
 *  - voice_mode_apply / voice_mode_toggle: any thread (atomic mode store,
 *    wakeup request; chat notify is deferred to the callback thread).
 *  - voice_mode_flush_notify: TS callback thread ONLY (prints to chat).
 *  - voice_mode_get_current_distance: any thread.
 */

#include "core/hub/hub_parser.h" /* HubSettings, HubRace (core types) */

typedef enum VoiceMode {
    VOICE_MODE_WHISPER = 0,
    VOICE_MODE_NORMAL  = 1,
    VOICE_MODE_SHOUT   = 2
} VoiceMode;

/* Server-profile snapshot the distance clamp needs. Filled by the ts/ layer
   through the get_profile hook (see below) so this core module never has to
   include ts/profile itself. All fields use pure core types (hub_parser.h). */
typedef struct VoiceModeProfile {
    int        active;  /* 1 = a server profile is applied */
    HubSettings hub;    /* copy of the active hub settings */
    int        hasRace; /* 1 = local player belongs to a race */
    HubRace    race;    /* that race (only valid when hasRace) */
} VoiceModeProfile;

/*
 * Layering inversion (V8.6): voice_modes is PURE mode/distance logic and must
 * not include ts/ or ui/ headers. The few side effects it needs (chat notify,
 * CEPOS send, overlay refresh) and the one data read it needs (server profile
 * for the clamp) are provided as init-time hooks.
 *
 * Bild fuer Anfaenger: core stellt eine Steckdose bereit, die ts/ui-Schicht
 * steckt beim Start den Stecker rein. Ohne Stecker (Hook == NULL) passiert
 * gefahrlos nichts (No-Op) — so bleibt das Modul auf jedem Rechner testbar.
 *
 * Every hook may be NULL: a missing action is a safe no-op, a missing
 * get_profile means "no server profile" (global config only).
 */
typedef struct VoiceModeHooks {
    void (*notify_chat)(const char* message);   /* show a mode-change chat line */
    void (*invalidate_cepos_cache)(void);        /* force the next CEPOS send */
    void (*signal_send_pending)(void);           /* wake the CEPOS send path */
    void (*overlay_sync)(void);                  /* refresh the voice overlay */
    int  (*get_profile)(VoiceModeProfile* out);  /* 1 when out was filled */
    int  (*has_pending_chat)(void);              /* 1 when a chat line waits */
    void (*flush_pending_chat)(void);            /* print waiting chat lines */
} VoiceModeHooks;

/* Wire the real ts/ui functions (plugin init, callback thread). Passing NULL
   clears all hooks. Copies the struct — the caller need not keep it alive. */
void voice_mode_set_hooks(const VoiceModeHooks* hooks);

/* 11.1 distance for a mode: zone override > global config, then profile
   min/max clamp. Any thread. */
float voice_mode_get_distance(VoiceMode mode);

/* Distance for the currently active mode. Any thread. */
float voice_mode_get_current_distance(void);

VoiceMode voice_mode_get_current(void);

/* 11.2 switch mode: stores mode, invalidates the CEPOS send cache (distance
   change must go out immediately) and queues a chat notification. */
void voice_mode_apply(VoiceMode mode);

/* Cycle whisper -> normal -> shout -> whisper (voice toggle hotkey). */
void voice_mode_toggle(void);

/* 11.3 poll hotkeys (debounced, TS-PTT-safe: re-arm only on GetKeyState
   release). Single caller: key-watcher thread (keyMonitorThreadFunction) —
   never add a second poller (unsynchronized arming state). */
void voice_mode_hotkey_poll(void);

/* Print pending mode-change notification to the TS chat tab.
   TS callback thread ONLY. */
void voice_mode_flush_notify(void);

/* 1 when a mode-change chat line is waiting. Any thread. */
int voice_mode_has_pending_notify(void);

/* Clear hotkey debounce state (e.g. after loading a preset). Pos watcher thread. */
void voice_mode_reset_key_tracking(void);

/* Suppress duplicate poll trigger after TS hotkey event handled the same VK. */
void voice_mode_notify_hotkey(int vkCode);

#endif /* CORE_VOICE_VOICE_MODES_H */
