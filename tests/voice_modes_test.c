/* Unit tests for src/core/voice/voice_modes.c — the mode -> distance decision.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc + the Win32 shim).
   Exit code 0 = all checks passed.

   voice_modes became host-testable in V8.6b: the module no longer includes
   ts/ or ui/ headers, it reads the server profile through the get_profile
   hook. The test wires a fake profile + a fake position and checks:
   - global config only:  whisper < normal < shout,
   - server profile min/max clamp raises/lowers the global value,
   - a race the local player is in beats the hub-wide clamp,
   - a zone override beats the global config (and is then still clamped),
   - 086: voice_mode_apply detects a mode change atomically — two threads
     applying the same new mode at the same time produce exactly ONE
     "changed" edge (one chat line / CEMODE), never two.

   The real zone_resolve.c (pure) is linked; config/pos/log are stubbed here. */

#include "core/voice/voice_modes.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static int feq(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 0.001f;
}

/* ---- stubs for voice_modes' core dependencies ------------------------------ */

PluginConfig g_config;

void config_copy(PluginConfig* out) { *out = g_config; }

void log_write(const char* fmt, ...) { (void)fmt; }

static PosSample g_testPos;
static int g_testPosValid = 0;

int pos_get_current(PosSample* out) {
    if (!g_testPosValid) {
        return 0;
    }
    *out = g_testPos;
    return 1;
}

/* ---- fake server profile behind the get_profile hook ----------------------- */

static VoiceModeProfile g_fakeProfile;
static int g_fakeProfileActive = 0;

static int fake_get_profile(VoiceModeProfile* out) {
    if (!g_fakeProfileActive) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = g_fakeProfile;
    return 1;
}

static void reset_state(void) {
    memset(&g_config, 0, sizeof(g_config));
    g_config.distanceWhisper = 5.0f;
    g_config.distanceNormal  = 15.0f;
    g_config.distanceShout   = 40.0f;

    memset(&g_fakeProfile, 0, sizeof(g_fakeProfile));
    g_fakeProfileActive = 0;
    memset(&g_testPos, 0, sizeof(g_testPos));
    g_testPosValid = 0;

    VoiceModeHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.get_profile = fake_get_profile;
    voice_mode_set_hooks(&hooks);
}

/* ---- tests ----------------------------------------------------------------- */

static void test_global_only(void) {
    printf("[1] global config only (no profile)\n");
    reset_state();

    CHECK(feq(voice_mode_get_distance(VOICE_MODE_WHISPER), 5.0f),  "whisper -> config whisper");
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 15.0f),  "normal  -> config normal");
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_SHOUT), 40.0f),   "shout   -> config shout");
    CHECK(voice_mode_get_distance(VOICE_MODE_WHISPER)
        < voice_mode_get_distance(VOICE_MODE_NORMAL),              "whisper < normal");
    CHECK(voice_mode_get_distance(VOICE_MODE_NORMAL)
        < voice_mode_get_distance(VOICE_MODE_SHOUT),               "normal  < shout");
}

static void test_profile_clamp(void) {
    printf("[2] server profile min/max clamp\n");
    reset_state();
    g_fakeProfileActive = 1;
    g_fakeProfile.active = 1;
    g_fakeProfile.hub.valid = 1;
    g_fakeProfile.hub.minNormal = 20.0f; /* raises global 15 -> 20 */
    g_fakeProfile.hub.maxNormal = 30.0f;
    g_fakeProfile.hub.minShout  = 0.0f;
    g_fakeProfile.hub.maxShout  = 25.0f; /* lowers global 40 -> 25 */

    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 20.0f), "below min -> clamped up to min");
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_SHOUT), 25.0f),  "above max -> clamped down to max");

    g_config.distanceNormal = 27.0f; /* inside [20,30] -> unchanged */
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 27.0f), "inside range -> unchanged");
}

static void test_race_beats_hub(void) {
    printf("[3] local race clamp beats hub clamp\n");
    reset_state();
    g_fakeProfileActive = 1;
    g_fakeProfile.active = 1;
    g_fakeProfile.hub.valid = 1;
    g_fakeProfile.hub.minNormal = 10.0f;  /* hub-wide */
    g_fakeProfile.hasRace = 1;
    g_fakeProfile.race.minNormal = 25.0f; /* race wins -> 15 clamped up to 25 */

    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 25.0f), "race min applied instead of hub min");
}

static void test_zone_override(void) {
    printf("[4] zone override beats global config\n");
    reset_state();
    g_fakeProfileActive = 1;
    g_fakeProfile.active = 1;
    g_fakeProfile.hub.valid = 1;
    g_fakeProfile.hub.zoneCount = 1;
    /* Zone quad 20..30 m; normalDist override = 99. */
    g_fakeProfile.hub.zones[0].x1 = 20.0f; g_fakeProfile.hub.zones[0].z1 = 20.0f;
    g_fakeProfile.hub.zones[0].x2 = 30.0f; g_fakeProfile.hub.zones[0].z2 = 20.0f;
    g_fakeProfile.hub.zones[0].x3 = 30.0f; g_fakeProfile.hub.zones[0].z3 = 30.0f;
    g_fakeProfile.hub.zones[0].x4 = 20.0f; g_fakeProfile.hub.zones[0].z4 = 30.0f;
    g_fakeProfile.hub.zones[0].normalDist = 99.0f;

    /* Pos.txt is centimeters; 2500 cm = 25 m -> inside the zone. */
    g_testPosValid = 1;
    g_testPos.x = 2500.0f; g_testPos.y = 0.0f; g_testPos.z = 2500.0f;

    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 99.0f), "inside zone -> zone override wins");

    /* Same zone, but a hub max of 50 still clamps the override down. */
    g_fakeProfile.hub.maxNormal = 50.0f;
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 50.0f), "zone override still clamped by max");

    /* Player outside the zone -> back to global config. */
    g_fakeProfile.hub.maxNormal = 0.0f;
    g_testPos.x = 100.0f; g_testPos.z = 100.0f; /* 1 m, outside 20..30 m */
    CHECK(feq(voice_mode_get_distance(VOICE_MODE_NORMAL), 15.0f), "outside zone -> global config");
}

/* ---- 086: atomic mode change edge ------------------------------------------ */

static volatile long g_hookChanged = 0;
static volatile long g_hookChat = 0;
static volatile long g_hookCepos = 0;

static void hook_changed(void)       { __atomic_add_fetch(&g_hookChanged, 1, __ATOMIC_SEQ_CST); }
static void hook_notify(const char* m) { (void)m; __atomic_add_fetch(&g_hookChat, 1, __ATOMIC_SEQ_CST); }
static void hook_cepos(void)         { __atomic_add_fetch(&g_hookCepos, 1, __ATOMIC_SEQ_CST); }

static void install_counting_hooks(void) {
    VoiceModeHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.get_profile = fake_get_profile;
    hooks.mode_changed = hook_changed;
    hooks.notify_chat = hook_notify;
    hooks.invalidate_cepos_cache = hook_cepos;
    voice_mode_set_hooks(&hooks);
    g_hookChanged = 0;
    g_hookChat = 0;
    g_hookCepos = 0;
}

static void test_apply_edge(void) {
    printf("[5] voice_mode_apply: one edge per real change\n");
    reset_state();
    install_counting_hooks();

    voice_mode_apply(VOICE_MODE_NORMAL); /* normalise (module default is normal) */
    g_hookChanged = 0; g_hookChat = 0; g_hookCepos = 0;

    voice_mode_apply(VOICE_MODE_NORMAL);
    CHECK(g_hookChanged == 0 && g_hookChat == 0, "same mode again: no edge");

    voice_mode_apply(VOICE_MODE_WHISPER);
    CHECK(voice_mode_get_current() == VOICE_MODE_WHISPER, "mode stored");
    CHECK(g_hookChanged == 1 && g_hookChat == 1 && g_hookCepos == 1, "real change: one edge");

    voice_mode_apply(VOICE_MODE_WHISPER);
    CHECK(g_hookChanged == 1 && g_hookChat == 1, "repeated hotkey press: still one edge");

    voice_mode_apply((VoiceMode)99);
    CHECK(voice_mode_get_current() == VOICE_MODE_NORMAL, "invalid mode falls back to normal");
    CHECK(g_hookChanged == 2, "fallback to normal is a real change");

    voice_mode_toggle();
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT && g_hookChanged == 3, "toggle normal -> shout");
    voice_mode_apply(VOICE_MODE_NORMAL);
}

#define RACE_ROUNDS 3000

static pthread_barrier_t g_raceStart;
static pthread_barrier_t g_raceEnd;

static void* race_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < RACE_ROUNDS; i++) {
        pthread_barrier_wait(&g_raceStart);
        voice_mode_apply(VOICE_MODE_SHOUT);
        pthread_barrier_wait(&g_raceEnd);
    }
    return NULL;
}

static void test_apply_race(void) {
    printf("[6] two threads applying the same new mode: exactly one edge\n");
    reset_state();
    install_counting_hooks();
    voice_mode_apply(VOICE_MODE_NORMAL);

    pthread_barrier_init(&g_raceStart, NULL, 3);
    pthread_barrier_init(&g_raceEnd, NULL, 3);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, race_worker, NULL);
    pthread_create(&t2, NULL, race_worker, NULL);

    int doubles = 0;
    int zeros = 0;
    for (int i = 0; i < RACE_ROUNDS; i++) {
        voice_mode_apply(VOICE_MODE_NORMAL);
        g_hookChanged = 0;
        pthread_barrier_wait(&g_raceStart);
        pthread_barrier_wait(&g_raceEnd);
        if (g_hookChanged > 1) {
            doubles++;
        }
        else if (g_hookChanged == 0) {
            zeros++;
        }
    }
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_barrier_destroy(&g_raceStart);
    pthread_barrier_destroy(&g_raceEnd);

    CHECK(doubles == 0, "never a duplicate 'mode changed' edge (chat line / CEMODE)");
    CHECK(zeros == 0, "never a lost edge either");
    CHECK(voice_mode_get_current() == VOICE_MODE_SHOUT, "final mode is shout");
    voice_mode_apply(VOICE_MODE_NORMAL);
}

int main(void) {
    test_global_only();
    test_profile_clamp();
    test_race_beats_hub();
    test_zone_override();
    test_apply_edge();
    test_apply_race();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
