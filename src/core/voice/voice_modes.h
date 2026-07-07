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
 *  - voice_mode_hotkey_poll: pos watcher thread (~50 ms). Only touches
 *    atomics + Win32 key state; never the TS API.
 *  - voice_mode_apply / voice_mode_toggle: any thread (atomic mode store,
 *    wakeup request; chat notify is deferred to the callback thread).
 *  - voice_mode_flush_notify: TS callback thread ONLY (prints to chat).
 *  - voice_mode_get_current_distance: any thread.
 */

typedef enum VoiceMode {
    VOICE_MODE_WHISPER = 0,
    VOICE_MODE_NORMAL  = 1,
    VOICE_MODE_SHOUT   = 2
} VoiceMode;

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
   release). Pos watcher thread. */
void voice_mode_hotkey_poll(void);

/* Print pending mode-change notification to the TS chat tab.
   TS callback thread ONLY. */
void voice_mode_flush_notify(void);

/* 1 when a mode-change chat line is waiting. Any thread. */
int voice_mode_has_pending_notify(void);

/* Clear hotkey debounce state (e.g. after loading a preset). Pos watcher thread. */
void voice_mode_reset_key_tracking(void);

#endif /* CORE_VOICE_VOICE_MODES_H */
