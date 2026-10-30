/* Host test for src/ts/profile/ts3_server_profile.c (086 review fixes).

   ts3_server_profile.c is #included here (not linked) and runs on Linux against
   a FAKE TeamSpeak world; the real hub_parser.c turns the fake Root description
   into HubSettings. Build/run: bash tests/run_tests.sh. Exit code 0 = all passed.

   Covered findings:
   3  Root channel ID refresh: channel list change (C4) -> one coalesced rescan,
      a recreated Root is re-resolved and its description requested again,
      a failed scan keeps the cached ID
   4  scan cap 1024 (Root behind position 128 is found; truncated list keeps cache)
   5  channel scans are throttled (no scan per onUpdateChannel event)
   6  a top-level "Root" beats a sub-channel called "Root"; fallback = first match
   7  scalar getters are lock-free atomics with the old "no profile" defaults
   8  [DEFAULT_SETTINGS] / force flags go through config_modify + config_save
   9  reset wipes the legacy root-channel password from every buffer */

#include <windows.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "core/util/compat_crt.h" /* strncpy_s, _stricmp, _TRUNCATE on the host */

/* ---- minimal Win32 stand-ins the shim does not have ------------------------------- */

typedef long long LONGLONG;
typedef unsigned int DWORD;
typedef unsigned char* LPBYTE;
typedef void* HKEY;
#define HKEY_CURRENT_USER ((HKEY)1)
#define KEY_READ 1
#define ERROR_SUCCESS 0L

static inline long RegOpenKeyExW(HKEY k, const wchar_t* sub, DWORD opt, DWORD sam, HKEY* out) {
    (void)k; (void)sub; (void)opt; (void)sam; (void)out;
    return 2; /* ERROR_FILE_NOT_FOUND: no Steam in the test */
}
static inline long RegQueryValueExW(HKEY k, const wchar_t* name, DWORD* r, DWORD* t, LPBYTE d, DWORD* s) {
    (void)k; (void)name; (void)r; (void)t; (void)d; (void)s;
    return 2;
}
static inline long RegCloseKey(HKEY k) { (void)k; return 0; }

static inline LONGLONG InterlockedCompareExchange64(volatile LONGLONG* dest, LONGLONG exchange,
    LONGLONG comparand) {
    __atomic_compare_exchange_n(dest, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
static inline LONGLONG InterlockedExchange64(volatile LONGLONG* target, LONGLONG value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

#define SecureZeroMemory(p, n) memset((p), 0, (n))

#include "ts/adapter/ts3_adapter.h"
#include "core/config/config.h"
#include "core/hub/mod_password.h"
#include "core/proximity/occlusion.h"
#include "core/voice/voice_modes.h"
#include "core/util/log.h"

/* Contract C1 (config.h): repeating identical typedef/prototype is harmless in C11. */
typedef void (*ConfigMutator)(PluginConfig* cfg, void* ctx);
void config_modify(ConfigMutator fn, void* ctx);

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

/* ---- fake world ------------------------------------------------------------------------ */

#define FAKE_MAX_CHANNELS 2048

typedef struct FakeChannel {
    uint64 id;
    char name[64];
    uint64 parent;
    const char* desc;
} FakeChannel;

static FakeChannel g_channels[FAKE_MAX_CHANNELS];
static int g_channelCount = 0;
static int g_listFails = 0;
static int g_listCalls = 0;
static int g_parentFails = 0;
static int g_connected = 1;
static char g_serverUid[64] = "UID-A";
static int g_descRequests = 0;
static uint64 g_lastDescRequest = 0;
static int g_wakeups = 0;
static int g_saveCalls = 0;
static int g_modifyCalls = 0;
static int g_keyResets = 0;
static int g_modPwClears = 0;
static int g_chatLines = 0;
static int g_defaultsChatSeen = 0;
PluginConfig g_config;

#define LOG_LINES 256
static char g_log[LOG_LINES][512];
static int g_logCount = 0;

static int log_contains(const char* needle) {
    for (int i = 0; i < g_logCount && i < LOG_LINES; i++) {
        if (strstr(g_log[i], needle)) {
            return 1;
        }
    }
    return 0;
}

static void add_channel(uint64 id, const char* name, uint64 parent, const char* desc) {
    FakeChannel* c = &g_channels[g_channelCount++];
    c->id = id;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->parent = parent;
    c->desc = desc;
}

static FakeChannel* find_channel(uint64 id) {
    for (int i = 0; i < g_channelCount; i++) {
        if (g_channels[i].id == id) {
            return &g_channels[i];
        }
    }
    return NULL;
}

/* ---- stubs: adapter ----------------------------------------------------------------------- */

int ts3_thread_is_callback(void) { return 1; }
int ts3_is_connected(void) { return g_connected; }

int ts3_get_server_uid(char* out, int outLen) {
    if (!g_serverUid[0]) {
        return 0;
    }
    snprintf(out, (size_t)outLen, "%s", g_serverUid);
    return 1;
}

void ts3_print_to_chat(const char* message) {
    g_chatLines++;
    if (strstr(message, "default settings applied")) {
        g_defaultsChatSeen++;
    }
}

int ts3_get_channel_list(uint64* out, int maxChannels) {
    g_listCalls++;
    if (g_listFails) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < g_channelCount && n < maxChannels; i++) {
        out[n++] = g_channels[i].id;
    }
    return n;
}

int ts3_get_channel_name(uint64 channelID, char* out, int outLen) {
    const FakeChannel* c = find_channel(channelID);
    if (!c) {
        return 0;
    }
    snprintf(out, (size_t)outLen, "%s", c->name);
    return 1;
}

int ts3_get_channel_parent(uint64 channelID, uint64* outParent) {
    const FakeChannel* c = find_channel(channelID);
    if (g_parentFails || !c) {
        return 0;
    }
    *outParent = c->parent;
    return 1;
}

int ts3_request_channel_description(uint64 channelID) {
    g_descRequests++;
    g_lastDescRequest = channelID;
    return 1;
}

int ts3_get_channel_description(uint64 channelID, char* out, int outLen) {
    const FakeChannel* c = find_channel(channelID);
    if (!c || !c->desc) {
        return 0;
    }
    snprintf(out, (size_t)outLen, "%s", c->desc);
    return 1;
}

void ts3_request_wakeup(void) { g_wakeups++; }

/* ---- stubs: config / voice / mod password / occlusion / log ------------------------------- */

void config_modify(ConfigMutator fn, void* ctx) {
    g_modifyCalls++;
    fn(&g_config, ctx);
}
int config_save(void) { g_saveCalls++; return 1; }

void voice_mode_reset_key_tracking(void) { g_keyResets++; }

int mod_password_get(char* out, int outLen, unsigned long long nowMs) {
    (void)out; (void)outLen; (void)nowMs;
    return 0;
}
void mod_password_clear(void) { g_modPwClears++; }

void occlusion_set_enabled(int enabled) { (void)enabled; }
void occlusion_set_intensity(float percent) { (void)percent; }
void occlusion_set_sound_params(float a, float r, float d, float v) { (void)a; (void)r; (void)d; (void)v; }
void occlusion_set_mod_params(const int* values) { (void)values; }

void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log[g_logCount % LOG_LINES], sizeof(g_log[0]), fmt, ap);
    va_end(ap);
    g_logCount++;
}
void log_debug(const char* fmt, ...) { (void)fmt; }

/* ---- code under test ------------------------------------------------------------------------ */

#include "ts/profile/ts3_server_profile.c"

/* ---- helpers ---------------------------------------------------------------------------------- */

static const char* DESC_FULL =
    "[GLOBAL]\n"
    "AudioMaxVolume=150\n"
    "ForceDistanceBasedMuting=True\n"
    "ForceAutomaticChanelSwitching=True\n"
    "ForceSpeakersOn=True\n"
    "NicknameRandomizer=False\n"
    "RealisticAudio=True\n"
    "FilterIntensity=55\n"
    "EnableHrtf=True\n"
    "IngameChannelPassword=topsecret\n";

static const char* DESC_PLAIN =
    "[GLOBAL]\n"
    "AudioMaxVolume=1.2\n";

static const char* DESC_DEFAULTS =
    "[GLOBAL]\n"
    "AudioMaxVolume=1.0\n"
    "[DEFAULT_SETTINGS]\n"
    "EnableDefaultSettingsOnFirstConnection=True\n"
    "DefaultWhisperKey=65\n"
    "DefaultNormalKey=66\n"
    "DefaultShoutKey=67\n"
    "DefaultVoiceToggleKey=68\n"
    "DefaultDistanceWhisper=4\n"
    "DefaultDistanceNormal=14\n"
    "DefaultDistanceShout=44\n";

/* 091: ranges written in foundations; the force flag stands after them. */
static const char* DESC_FOUNDATIONS =
    "[GLOBAL]\n"
    "MinimumNormal=6\n"
    "MaximumNormal=6\n"
    "ForceRangesInFoundations=True\n"
    "[RACE]\n"
    "Race=Wolf\n"
    "listenAddDistance=4\n";

static void reset_world(void) {
    memset(g_channels, 0, sizeof(g_channels));
    g_channelCount = 0;
    g_listFails = 0;
    g_listCalls = 0;
    g_parentFails = 0;
    g_connected = 1;
    snprintf(g_serverUid, sizeof(g_serverUid), "UID-A");
    g_descRequests = 0;
    g_lastDescRequest = 0;
    g_wakeups = 0;
    g_saveCalls = 0;
    g_modifyCalls = 0;
    g_keyResets = 0;
    g_modPwClears = 0;
    g_chatLines = 0;
    g_defaultsChatSeen = 0;
    g_logCount = 0;
    memset(&g_config, 0, sizeof(g_config));

    server_profile_reset();
    win32_shim_advance_ms(100000); /* far past every throttle window */
}

/* Resolve Root + load its description through the real driver path. */
static int load_profile(uint64 rootId) {
    server_profile_tick();                       /* finds Root, requests its description */
    return server_profile_on_description_update(rootId); /* "description arrived" */
}

static int bytes_all_zero(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- tests -------------------------------------------------------------------------------------- */

static void test_scalar_getters(void) {
    printf("[1] scalar getters: old 'no profile' defaults, profile values, reset\n");
    reset_world();

    CHECK(feq(server_profile_get_max_volume(), 1.0f), "no profile: max volume 1.0");
    CHECK(feq(server_profile_get_filter_intensity(), 100.0f), "no profile: filter intensity 100");
    CHECK(server_profile_force_auto_channel() == 0, "no profile: no forced auto channel");
    CHECK(server_profile_force_distance_muting() == 0, "no profile: no forced muting");
    CHECK(server_profile_force_speakers_on() == 0, "no profile: no forced speakers");
    CHECK(server_profile_get_nickname_randomizer() == 1, "no profile: nickname randomizer ON (legacy)");
    CHECK(server_profile_get_realistic_audio() == 0, "no profile: realistic audio off");
    CHECK(server_profile_get_hrtf_enabled() == 0, "no profile: hrtf off");

    add_channel(1, "Root", 0, DESC_FULL);
    CHECK(load_profile(1) == 1, "profile applied through tick + description event");
    CHECK(server_profile_is_active(), "profile active");
    CHECK(feq(server_profile_get_max_volume(), 1.5f), "max volume 1.5 (AudioMaxVolume=150)");
    CHECK(feq(server_profile_get_filter_intensity(), 55.0f), "filter intensity 55");
    CHECK(server_profile_force_auto_channel() == 1, "forced auto channel");
    CHECK(server_profile_force_distance_muting() == 1, "forced muting");
    CHECK(server_profile_force_speakers_on() == 1, "forced speakers");
    CHECK(server_profile_get_nickname_randomizer() == 0, "NicknameRandomizer=False -> 0");
    CHECK(server_profile_get_realistic_audio() == 1, "realistic audio on");
    CHECK(server_profile_get_hrtf_enabled() == 1, "hrtf on");

    HubSettings copy;
    CHECK(server_profile_get(&copy) == 1 && feq(copy.audioMaxVolume, 1.5f), "server_profile_get still returns the full copy");

    server_profile_reset();
    CHECK(!server_profile_is_active(), "reset: inactive");
    CHECK(feq(server_profile_get_max_volume(), 1.0f), "reset: max volume back to 1.0");
    CHECK(feq(server_profile_get_filter_intensity(), 100.0f), "reset: filter back to 100");
    CHECK(server_profile_force_auto_channel() == 0 && server_profile_force_distance_muting() == 0
        && server_profile_force_speakers_on() == 0, "reset: force flags off");
    CHECK(server_profile_get_nickname_randomizer() == 1, "reset: randomizer ON again");
    CHECK(server_profile_get_realistic_audio() == 0 && server_profile_get_hrtf_enabled() == 0,
        "reset: realistic/hrtf off");
    CHECK(server_profile_get(&copy) == 0 && feq(copy.audioMaxVolume, 1.0f), "reset: get() reports no profile");
}

static void test_password_wipe(void) {
    printf("[2] reset wipes the legacy root-channel password\n");
    reset_world();
    add_channel(1, "Root", 0, DESC_FULL);
    CHECK(load_profile(1) == 1, "profile with IngameChannelPassword applied");

    char pw[HUB_PASSWORD_LEN];
    CHECK(server_profile_copy_ingame_password(pw, sizeof(pw)) == 1 && strcmp(pw, "topsecret") == 0,
        "password available while connected");
    CHECK(strstr(g_descBuf, "topsecret") != NULL, "(precondition) description buffer holds the password");

    server_profile_reset();
    CHECK(bytes_all_zero(g_ingamePassword, sizeof(g_ingamePassword)), "g_ingamePassword zeroed");
    CHECK(bytes_all_zero(g_descBuf, sizeof(g_descBuf)), "description buffer zeroed");
    CHECK(bytes_all_zero(&g_parsedBuf, sizeof(g_parsedBuf)), "parsed HubSettings buffer zeroed");
    CHECK(bytes_all_zero(&g_active, sizeof(g_active)), "active profile copy zeroed");
    CHECK(server_profile_copy_ingame_password(pw, sizeof(pw)) == 0 && pw[0] == '\0',
        "no password after reset");
    CHECK(g_modPwClears >= 1, "mod password cleared as before");
}

static void test_config_modify(void) {
    printf("[3] defaults + force flags go through config_modify/config_save\n");
    reset_world();
    g_config.enableDistanceMuting = 0;
    g_config.enableAutomaticChannelChange = 0;
    add_channel(1, "Root", 0, DESC_FULL);
    CHECK(load_profile(1) == 1, "profile applied");
    CHECK(g_modifyCalls == 1, "force flags: exactly one config_modify");
    CHECK(g_config.enableDistanceMuting == 1 && g_config.enableAutomaticChannelChange == 1,
        "force flags switched on in the config");
    CHECK(g_saveCalls == 1, "saved once");

    /* Same forces again (user-enabled already): nothing to save. */
    server_profile_reset();
    g_saveCalls = 0;
    CHECK(load_profile(1) == 1, "profile applied again after reconnect");
    CHECK(g_saveCalls == 0, "already on -> no save");
    g_config.enableDistanceMuting = 1; /* never undone when the server stops forcing */

    /* No force keys: config_modify not even needed. */
    reset_world();
    add_channel(1, "Root", 0, DESC_PLAIN);
    CHECK(load_profile(1) == 1, "plain profile applied");
    CHECK(g_modifyCalls == 0 && g_saveCalls == 0, "no forces -> config untouched");

    /* [DEFAULT_SETTINGS] once per server. */
    reset_world();
    add_channel(1, "Root", 0, DESC_DEFAULTS);
    g_config.whisperKey = 11;
    CHECK(load_profile(1) == 1, "defaults profile applied");
    CHECK(g_config.whisperKey == 65 && g_config.normalKey == 66 && g_config.shoutKey == 67
        && g_config.voiceToggleKey == 68, "default keys applied");
    CHECK(feq(g_config.distanceWhisper, 4.0f) && feq(g_config.distanceNormal, 14.0f)
        && feq(g_config.distanceShout, 44.0f), "default distances applied");
    CHECK(strcmp(g_config.defaultsAppliedServer, "UID-A") == 0, "server marked as applied");
    CHECK(g_saveCalls == 1 && g_keyResets == 1, "saved + key tracking reset requested");
    CHECK(g_defaultsChatSeen == 1, "chat confirmation");

    g_config.whisperKey = 77; /* the user changes a key afterwards */
    server_profile_reset();
    g_saveCalls = 0;
    g_keyResets = 0;
    CHECK(load_profile(1) == 1, "reconnect to the same server");
    CHECK(g_config.whisperKey == 77, "user's key change sticks (defaults not re-applied)");
    CHECK(g_saveCalls == 0 && g_keyResets == 0, "no save / reset the second time");

    snprintf(g_serverUid, sizeof(g_serverUid), "UID-B");
    server_profile_reset();
    CHECK(load_profile(1) == 1, "connect to another server");
    CHECK(g_config.whisperKey == 65, "other server: defaults applied again");
    CHECK(strcmp(g_config.defaultsAppliedServer, "UID-B") == 0, "marker moved to the new server");

    /* No server identity: never risk re-applying. */
    reset_world();
    g_serverUid[0] = '\0';
    add_channel(1, "Root", 0, DESC_DEFAULTS);
    g_config.whisperKey = 11;
    CHECK(load_profile(1) == 1, "profile applied without server UID");
    CHECK(g_config.whisperKey == 11 && g_saveCalls == 0, "no UID -> defaults skipped");
}

static void test_top_level_root(void) {
    printf("[4] top-level Root beats a sub-channel called Root\n");
    reset_world();
    add_channel(10, "Root", 5, DESC_PLAIN);   /* crafted: sub-channel, sorts first */
    add_channel(5, "Lobby", 0, NULL);
    add_channel(11, "Root", 0, DESC_FULL);    /* the real one */
    server_profile_tick();
    CHECK(g_rootChannelID == 11, "top-level Root (11) selected, not the first match (10)");
    CHECK(g_lastDescRequest == 11, "description requested from the real Root");

    reset_world();
    add_channel(10, "Root", 5, DESC_PLAIN);
    add_channel(5, "Lobby", 0, NULL);
    server_profile_tick();
    CHECK(g_rootChannelID == 10, "only a sub-channel Root exists: first match (old behaviour)");

    reset_world();
    g_parentFails = 1;
    add_channel(10, "Root", 5, DESC_PLAIN);
    add_channel(11, "Root", 0, DESC_FULL);
    server_profile_tick();
    CHECK(g_rootChannelID == 10, "parent lookup unavailable: first match (old behaviour)");
}

static void test_scan_throttle(void) {
    printf("[5] channel scans are throttled, dirty flag forces exactly one\n");
    reset_world();
    add_channel(1, "General", 0, NULL);
    add_channel(2, "Chat", 0, NULL);

    for (int i = 0; i < 100; i++) {
        server_profile_on_description_update(1);
        server_profile_on_channel_edited(2);
        server_profile_tick();
        win32_shim_advance_ms(100);
    }
    /* 10 s of events: the 5 s retry allows 2-3 scans (old code: ~300). */
    CHECK(g_listCalls >= 1 && g_listCalls <= 3, "no Root: scans follow the 5 s retry throttle");

    win32_shim_advance_ms(6000);
    const int before = g_listCalls;
    for (int i = 0; i < 50; i++) {
        server_profile_on_channel_list_changed((uint64)i);
    }
    for (int i = 0; i < 20; i++) {
        server_profile_on_description_update(1);
        server_profile_on_channel_edited(1);
    }
    CHECK(g_listCalls - before <= 1, "event handlers never scan once per event");
    server_profile_tick();
    server_profile_tick();
    CHECK(g_listCalls - before <= 2, "50 channel events + tick: one forced rescan at most");
    CHECK(g_wakeups >= 1, "channel event asks for a wakeup");

    /* A new Root appears: the dirty flag finds it without waiting for the 5 s retry. */
    add_channel(3, "Root", 0, DESC_PLAIN);
    server_profile_on_channel_list_changed(3);
    server_profile_tick();
    CHECK(g_rootChannelID == 3, "forced rescan found the new Root immediately");
}

static void test_root_recreated(void) {
    printf("[6] Root deleted + recreated: re-resolved, description requested again\n");
    reset_world();
    add_channel(11, "Root", 0, DESC_PLAIN);
    CHECK(load_profile(11) == 1, "profile loaded from Root 11");
    CHECK(feq(server_profile_get_max_volume(), 1.2f), "max volume 1.2");

    static const char* newDesc = "[GLOBAL]\nAudioMaxVolume=1.7\n";
    g_channels[0].id = 40; /* admin deleted Root and created a new one */
    g_channels[0].desc = newDesc;
    CHECK(server_profile_on_description_update(40) == 0, "event for the new ID is ignored before the rescan");

    const int requestsBefore = g_descRequests;
    server_profile_on_channel_list_changed(40);
    server_profile_tick();
    CHECK(g_rootChannelID == 40, "rescan switched to the new Root ID");
    CHECK(g_descRequests == requestsBefore + 1 && g_lastDescRequest == 40,
        "new Root description requested although a profile is active");
    CHECK(log_contains("Root channel changed"), "change logged");
    CHECK(server_profile_on_description_update(40) == 1, "new description applied");
    CHECK(feq(server_profile_get_max_volume(), 1.7f), "new settings active (max volume 1.7)");

    server_profile_tick();
    CHECK(g_descRequests == requestsBefore + 1, "request is not repeated");

    /* Unreadable list keeps the cached Root. */
    g_listFails = 1;
    server_profile_on_channel_list_changed(0);
    server_profile_tick();
    CHECK(g_rootChannelID == 40, "failed scan keeps the cached Root ID");
    g_listFails = 0;
}

static void test_channel_cap(void) {
    printf("[7] 1024-channel cap\n");
    reset_world();
    for (int i = 0; i < 1500; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Channel %d", i);
        add_channel((uint64)(100 + i), name, 0, NULL);
    }
    snprintf(g_channels[900].name, sizeof(g_channels[900].name), "Root");
    g_channels[900].desc = DESC_PLAIN;
    server_profile_tick();
    CHECK(g_rootChannelID == g_channels[900].id, "Root at position 900 found (old cap was 128)");

    /* Moves beyond the cap on the next rescan: the cached ID is kept. */
    snprintf(g_channels[900].name, sizeof(g_channels[900].name), "Channel 800");
    snprintf(g_channels[1300].name, sizeof(g_channels[1300].name), "Root");
    server_profile_on_channel_list_changed(0);
    server_profile_tick();
    CHECK(g_rootChannelID == g_channels[900].id, "truncated list without a hit keeps the cached Root");
}

/* ---- lock-free getters under concurrent apply/reset ---------------------------------- */

static volatile int g_readerStop = 0;
static volatile int g_readerBad = 0;
static volatile long g_readerLoops = 0;

static void* reader_thread(void* arg) {
    (void)arg;
    while (!g_readerStop) {
        const float v = server_profile_get_max_volume();
        const float f = server_profile_get_filter_intensity();
        if (!(feq(v, 1.0f) || feq(v, 1.5f)) || !(feq(f, 100.0f) || feq(f, 55.0f))) {
            g_readerBad++;
        }
        (void)server_profile_force_speakers_on();
        (void)server_profile_get_nickname_randomizer();
        g_readerLoops++;
    }
    return NULL;
}

static void test_concurrent_readers(void) {
    printf("[8] getters while another thread applies / resets\n");
    reset_world();
    HubSettings s;
    hub_parse_settings(DESC_FULL, &s);

    g_readerStop = 0;
    g_readerBad = 0;
    pthread_t t;
    pthread_create(&t, NULL, reader_thread, NULL);
    for (int i = 0; i < 3000; i++) {
        server_profile_apply(&s);
        server_profile_reset();
    }
    g_readerStop = 1;
    pthread_join(t, NULL);
    CHECK(g_readerBad == 0, "readers only ever see default or profile values (no torn floats)");
    CHECK(g_readerLoops > 0, "reader thread ran");
}

static void test_force_ranges(void) {
    printf("[9] ForceRangesInFoundations (091): lock-free getter, meters in the profile, chat line\n");
    reset_world();

    CHECK(server_profile_force_ranges_in_foundations() == 0, "no profile: not forced");

    add_channel(1, "Root", 0, DESC_PLAIN);
    CHECK(load_profile(1) == 1 && server_profile_force_ranges_in_foundations() == 0,
        "profile without the key: not forced");

    reset_world();
    add_channel(1, "Root", 0, DESC_FOUNDATIONS);
    CHECK(load_profile(1) == 1, "profile with the key applied");
    CHECK(server_profile_force_ranges_in_foundations() == 1, "getter: forced while the profile is active");

    HubSettings copy;
    CHECK(server_profile_get(&copy) == 1 && copy.forceRangesInFoundations == 1, "full copy carries the flag");
    CHECK(feq(copy.minNormal, 15.36f) && feq(copy.maxNormal, 15.36f),
        "the profile holds METERS: 6 F -> 15.36 m (audio and clamps never see foundations)");
    CHECK(copy.raceCount == 1 && feq(copy.races[0].listenAddDistance, 10.24f),
        "race listen bonus 4 F -> 10.24 m");

    server_profile_reset();
    CHECK(server_profile_force_ranges_in_foundations() == 0,
        "reset (disconnect): not forced any more - the player's own choice applies again");
    CHECK(g_config.rangeInFoundations == 0, "the force was never written into the player's own setting");

    /* the listen-bonus chat line (profile_announce): meters unchanged, foundation mode adds F */
    char line[256];
    profile_listen_bonus_line(line, sizeof(line), 76561198063092371ULL, "Wolf", 2.0f, 0);
    CHECK(strcmp(line, "[Conan Exiles] SteamID 76561198063092371 recognized - race 'Wolf' (listen bonus +2.0 m)") == 0,
        "meter mode: line exactly as before");
    profile_listen_bonus_line(line, sizeof(line), 76561198063092371ULL, "Wolf", 2.0f, 1);
    CHECK(strcmp(line, "[Conan Exiles] SteamID 76561198063092371 recognized - race 'Wolf' (listen bonus +0.8 foundations (2.0 m))") == 0,
        "foundation mode: +0.8 foundations (2.0 m)");
    profile_listen_bonus_line(line, sizeof(line), 1ULL, "Faun", 10.24f, 1);
    CHECK(strstr(line, "(listen bonus +4.0 foundations (10.2 m))") != NULL, "10.24 m -> +4.0 foundations (10.2 m)");
}

int main(void) {
    test_scalar_getters();
    test_password_wipe();
    test_config_modify();
    test_top_level_root();
    test_scan_throttle();
    test_root_recreated();
    test_channel_cap();
    test_concurrent_readers();
    test_force_ranges();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
