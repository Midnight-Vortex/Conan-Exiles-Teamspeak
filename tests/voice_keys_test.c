/* Host test for the hotkey debounce state in src/core/voice/voice_modes.c (086).

   Finding 15: g_keyArmed / g_keySuppressUntil belong to the key-watcher thread,
   but voice_mode_reset_key_tracking() (memset) and voice_mode_notify_hotkey()
   were called from the TS callback / UI threads. Now both only set a request
   flag; voice_mode_hotkey_poll() (the single owner) applies it at its next poll.

   voice_modes.c is #included here so the test can feed its own key state into
   GetAsyncKeyState / GetKeyState (the win32 shim returns "up" for everything).
   Time is moved by the shim (win32_shim_advance_ms).
   Build/run: bash tests/run_tests.sh. Exit code 0 = all checks passed. */

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- fake keyboard ------------------------------------------------------------- */

static int g_keyDown[256];

static SHORT test_key_state(int vk) {
    return (vk > 0 && vk < 256 && g_keyDown[vk]) ? (SHORT)0x8000 : (SHORT)0;
}

#define GetAsyncKeyState(vk) test_key_state(vk)
#define GetKeyState(vk) test_key_state(vk)

/* ---- stubs for voice_modes' core dependencies --------------------------------- */

#include "core/voice/voice_modes.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"

PluginConfig g_config;
void config_copy(PluginConfig* out) { *out = g_config; }
void log_write(const char* fmt, ...) { (void)fmt; }
int pos_get_current(PosSample* out) { (void)out; return 0; }

#include "core/voice/voice_modes.c"

/* ---- helpers --------------------------------------------------------------------- */

#define KEY_TOGGLE 0x70 /* F1 */

static void setup(void) {
    memset(&g_config, 0, sizeof(g_config));
    g_config.distanceWhisper = 5.0f;
    g_config.distanceNormal = 15.0f;
    g_config.distanceShout = 40.0f;
    g_config.enableVoiceToggle = 1;
    g_config.voiceToggleKey = KEY_TOGGLE;
    memset(g_keyDown, 0, sizeof(g_keyDown));
    voice_mode_set_hooks(NULL);
    voice_mode_apply(VOICE_MODE_NORMAL);
    voice_mode_reset_key_tracking();
    voice_mode_hotkey_poll(); /* applies the reset request, key is up -> armed */
    win32_shim_advance_ms(1000);
}

static void press(int vk)   { g_keyDown[vk] = 1; }
static void release(int vk) { g_keyDown[vk] = 0; }

/* ---- tests -------------------------------------------------------------------------- */

static void test_poll_baseline(void) {
    printf("[1] baseline: a press toggles once, holding does not repeat\n");
    setup();
    CHECK(voice_mode_get_current() == VOICE_MODE_NORMAL, "start in normal");

    press(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "press -> toggle normal -> shout");

    win32_shim_advance_ms(500);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "key still held: no second toggle");

    release(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    win32_shim_advance_ms(500);
    press(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_WHISPER, "release + press -> next toggle");
}

static void test_reset_is_deferred(void) {
    printf("[2] reset_key_tracking only sets a request; the poller applies it\n");
    setup();

    press(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "first press toggles");
    win32_shim_advance_ms(500);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "held key is disarmed");

    voice_mode_reset_key_tracking(); /* e.g. UI thread after a preset load */
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "the call alone changes nothing");
    CHECK(g_keyArmed[KEY_TOGGLE] == 0, "debounce state untouched by the caller thread");

    win32_shim_advance_ms(500);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_WHISPER,
        "next poll performs the reset: held key is armed again -> toggles");

    win32_shim_advance_ms(500);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_WHISPER, "request is one-shot (no repeat)");
}

static void test_notify_is_deferred(void) {
    printf("[3] notify_hotkey only sets a request; the poller swallows the press\n");
    setup();

    press(KEY_TOGGLE);
    voice_mode_notify_hotkey(KEY_TOGGLE); /* TS hotkey event handled this press */
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_NORMAL, "the poller does not toggle a second time");

    win32_shim_advance_ms(500);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_NORMAL, "held key stays disarmed");

    release(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    win32_shim_advance_ms(500);
    press(KEY_TOGGLE);
    voice_mode_hotkey_poll();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "a new press works again");

    voice_mode_notify_hotkey(0);
    voice_mode_notify_hotkey(256);
    voice_mode_notify_hotkey(-5);
    CHECK(1, "out-of-range VK codes are ignored without crashing");
}

int main(void) {
    test_poll_baseline();
    test_reset_is_deferred();
    test_notify_is_deferred();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
