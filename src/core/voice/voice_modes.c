#include "core/voice/voice_modes.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/zone_resolve.h"
#include "core/util/log.h"
#include "ui/overlay/voice_overlay.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/proximity/ts3_cepos.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define VOICE_POLL_SUPPRESS_MS 300

/* Current mode — atomic, read from cepos build path / written by hotkeys. */
static volatile long g_currentMode = VOICE_MODE_NORMAL;

/* Pending chat notification (mode value + 1, 0 = none). Set anywhere,
   consumed on the callback thread. */
static volatile long g_pendingNotify = 0;

/* Hotkey state — pos watcher thread only. One trigger per press; re-arm on
   GetKeyState release because GetAsyncKeyState can stay stuck after the TS
   push-to-talk hook captures the key (old plugin lesson). */
static char g_keyArmed[256];
static ULONGLONG g_keySuppressUntil[256];

/* ---- 11.1 distance ---------------------------------------------------------- */

static float voice_mode_global_distance(VoiceMode mode) {
    switch (mode) {
    case VOICE_MODE_WHISPER: return g_config.distanceWhisper;
    case VOICE_MODE_SHOUT:   return g_config.distanceShout;
    case VOICE_MODE_NORMAL:
    default:                 return g_config.distanceNormal;
    }
}

float voice_mode_get_distance(VoiceMode mode) {
    float distance = voice_mode_global_distance(mode);

    HubSettings hub;
    const int hubActive = server_profile_get(&hub);

    /* Zone override beats global config. */
    if (hubActive && hub.zoneCount > 0) {
        PosSample local;
        if (pos_get_current(&local)) {
            const int zone = zone_resolve(&hub,
                local.x / 100.0f, local.y / 100.0f, local.z / 100.0f);
            if (zone >= 0) {
                const HubZone* z = &hub.zones[zone];
                float zoneDist = 0.0f;
                switch (mode) {
                case VOICE_MODE_WHISPER: zoneDist = z->whisperDist; break;
                case VOICE_MODE_SHOUT:   zoneDist = z->shoutDist; break;
                case VOICE_MODE_NORMAL:
                default:                 zoneDist = z->normalDist; break;
                }
                if (zoneDist > 0.0f) {
                    distance = zoneDist;
                }
            }
        }
    }

    /* Server profile min/max clamp per mode — the server-forced limits the
       user cannot escape. A race the local player belongs to (SteamID match)
       replaces the global limits with its own. */
    if (hubActive) {
        float minV = 0.0f, maxV = 0.0f;

        HubRace race;
        if (server_profile_get_local_race(&race)) {
            switch (mode) {
            case VOICE_MODE_WHISPER: minV = race.minWhisper; maxV = race.maxWhisper; break;
            case VOICE_MODE_SHOUT:   minV = race.minShout;   maxV = race.maxShout;   break;
            case VOICE_MODE_NORMAL:
            default:                 minV = race.minNormal;  maxV = race.maxNormal;  break;
            }
        }
        else {
            switch (mode) {
            case VOICE_MODE_WHISPER: minV = hub.minWhisper; maxV = hub.maxWhisper; break;
            case VOICE_MODE_SHOUT:   minV = hub.minShout;   maxV = hub.maxShout;   break;
            case VOICE_MODE_NORMAL:
            default:                 minV = hub.minNormal;  maxV = hub.maxNormal;  break;
            }
        }
        if (minV > 0.0f && distance < minV) {
            distance = minV;
        }
        if (maxV > 0.0f && distance > maxV) {
            distance = maxV;
        }
    }

    return distance;
}

float voice_mode_get_current_distance(void) {
    return voice_mode_get_distance(voice_mode_get_current());
}

VoiceMode voice_mode_get_current(void) {
    return (VoiceMode)InterlockedCompareExchange(&g_currentMode, 0, 0);
}

/* ---- 11.2 apply -------------------------------------------------------------- */

void voice_mode_apply(VoiceMode mode) {
    if (mode != VOICE_MODE_WHISPER && mode != VOICE_MODE_SHOUT) {
        mode = VOICE_MODE_NORMAL;
    }
    InterlockedExchange(&g_currentMode, (long)mode);

    /* New distance must reach the other clients right away. */
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();

    InterlockedExchange(&g_pendingNotify, (long)mode + 1);
    ts3_request_wakeup();
    if (ts3_thread_is_callback()) {
        voice_mode_flush_notify();
    }
    updateVoiceOverlay();
}

void voice_mode_toggle(void) {
    switch (voice_mode_get_current()) {
    case VOICE_MODE_WHISPER: voice_mode_apply(VOICE_MODE_NORMAL); break;
    case VOICE_MODE_NORMAL:  voice_mode_apply(VOICE_MODE_SHOUT); break;
    case VOICE_MODE_SHOUT:
    default:                 voice_mode_apply(VOICE_MODE_WHISPER); break;
    }
}

/* ---- 11.3 hotkeys (pos watcher thread) ----------------------------------------- */

static int voice_key_pressed(int vkCode) {
    if (vkCode <= 0 || vkCode >= 256) {
        return 0;
    }
    if (GetTickCount64() < g_keySuppressUntil[vkCode]) {
        return 0;
    }

    const int asyncDown = (GetAsyncKeyState(vkCode) & 0x8000) != 0;
    const int syncDown = (GetKeyState(vkCode) & 0x8000) != 0;

    if (!syncDown) {
        g_keyArmed[vkCode] = 1;
    }
    if (!asyncDown && !syncDown) {
        return 0;
    }
    if (!g_keyArmed[vkCode]) {
        return 0;
    }
    g_keyArmed[vkCode] = 0;
    g_keySuppressUntil[vkCode] = GetTickCount64() + VOICE_POLL_SUPPRESS_MS;
    return 1;
}

void voice_mode_hotkey_poll(void) {
    if (voice_key_pressed(g_config.whisperKey)) {
        voice_mode_apply(VOICE_MODE_WHISPER);
        return;
    }
    if (voice_key_pressed(g_config.shoutKey)) {
        voice_mode_apply(VOICE_MODE_SHOUT);
        return;
    }
    if (voice_key_pressed(g_config.normalKey)) {
        voice_mode_apply(VOICE_MODE_NORMAL);
        return;
    }
    if (g_config.enableVoiceToggle && voice_key_pressed(g_config.voiceToggleKey)) {
        voice_mode_toggle();
    }
}

void voice_mode_reset_key_tracking(void) {
    memset(g_keyArmed, 0, sizeof(g_keyArmed));
    memset(g_keySuppressUntil, 0, sizeof(g_keySuppressUntil));
}

void voice_mode_notify_hotkey(int vkCode) {
    if (vkCode > 0 && vkCode < 256) {
        g_keySuppressUntil[vkCode] = GetTickCount64() + VOICE_POLL_SUPPRESS_MS;
        g_keyArmed[vkCode] = 0;
    }
}

/* ---- chat notify (callback thread) ----------------------------------------------- */

void voice_mode_flush_notify(void) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    const long pending = InterlockedExchange(&g_pendingNotify, 0);
    if (pending == 0) {
        return;
    }

    static const char* const modeNames[] = { "Whisper", "Normal", "Shout" };
    const int mode = (int)pending - 1;
    const float distance = voice_mode_get_distance((VoiceMode)mode);

    char message[128];
    snprintf(message, sizeof(message), "[Conan Exiles] Voice mode: %s - Distance: %.1f m",
        modeNames[mode >= 0 && mode <= 2 ? mode : 1], distance);
    ts3_print_to_chat(message);
    log_write("VOICE: mode=%s distance=%.1f", modeNames[mode >= 0 && mode <= 2 ? mode : 1], distance);
}

int voice_mode_has_pending_notify(void) {
    return InterlockedCompareExchange(&g_pendingNotify, 0, 0) != 0;
}
