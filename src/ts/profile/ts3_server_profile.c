#include "ts/profile/ts3_server_profile.h"
#include "ts/adapter/ts3_adapter.h"
#include "core/config/config.h"
#include "core/voice/voice_modes.h"
#include "core/util/log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PROFILE_FIND_RETRY_MS      5000   /* re-resolve missing Root channel */
#define PROFILE_REQUEST_RETRY_MS   30000  /* re-request until a profile arrived */
#define PROFILE_INFLIGHT_MAX_MS    10000  /* pending request times out */
#define PROFILE_DESC_MAX           16384

/* Request state — TS callback thread only. */
static uint64 g_rootChannelID = 0;
static ULONGLONG g_lastFindMs = 0;
static int g_requestInFlight = 0;
static ULONGLONG g_requestSentMs = 0;
static ULONGLONG g_lastRequestMs = 0;

/* Active profile — written on the callback thread, read from any thread. */
static CRITICAL_SECTION g_profileLock;
static INIT_ONCE g_profileLockOnce = INIT_ONCE_STATIC_INIT;
static HubSettings g_active;
static volatile long g_profileActive = 0;

/* Callback thread only (password consumer runs there too). */
static char g_ingamePassword[HUB_PASSWORD_LEN] = "";

/* Local race match (index into g_active.races, -1 = none) + listener bonus.
   Written on the callback thread during apply, read from any thread. */
static volatile long g_localRaceIndex = -1;
static volatile long g_listenAddBits = 0; /* float stored as raw bits */

/* Chat confirmation is shown once per connection (reset on reconnect). */
static int g_announced = 0;

static BOOL CALLBACK profile_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_profileLock);
    return TRUE;
}

static void profile_lock_ensure(void) {
    InitOnceExecuteOnce(&g_profileLockOnce, profile_lock_init_once, NULL, NULL);
}

/* ---- Steam ID (registry, read once) -------------------------------------- */

/* SteamID64 of the logged-in Steam user, 0 when Steam is not running.
   HKCU\...\ActiveProcess\ActiveUser holds the 32-bit account ID; SteamID64
   is that plus the public-universe base offset. Cached after first read. */
unsigned long long server_profile_get_local_steam_id(void) {
    static volatile LONGLONG s_cached = -1; /* -1 = not read yet */

    LONGLONG cached = InterlockedCompareExchange64(&s_cached, 0, 0);
    if (cached >= 0) {
        return (unsigned long long)cached;
    }

    unsigned long long id = 0ULL;
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam\\ActiveProcess",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD activeUser = 0;
        DWORD size = sizeof(activeUser);
        if (RegQueryValueExW(key, L"ActiveUser", NULL, NULL,
                (LPBYTE)&activeUser, &size) == ERROR_SUCCESS && activeUser != 0) {
            id = 76561197960265728ULL + (unsigned long long)activeUser;
        }
        RegCloseKey(key);
    }

    InterlockedExchange64(&s_cached, (LONGLONG)id);
    if (id != 0ULL) {
        log_write("PROFILE: local SteamID64 = %llu", id);
    }
    else {
        log_write("PROFILE: no Steam ID found (Steam not running?)");
    }
    return id;
}

/* ---- race resolve ---------------------------------------------------------- */

static int profile_resolve_race_index(const HubSettings* settings,
    unsigned long long steamID) {
    if (steamID == 0ULL) {
        return -1;
    }
    for (int i = 0; i < settings->raceCount && i < HUB_MAX_RACES; i++) {
        const HubRace* race = &settings->races[i];
        for (int j = 0; j < race->steamIDCount && j < HUB_MAX_STEAMIDS_PER_RACE; j++) {
            if (race->steamIDs[j] == steamID) {
                return i;
            }
        }
    }
    return -1;
}

/* ---- first-connection defaults ([DEFAULT_SETTINGS], once per server) ------- */

static void profile_apply_defaults_once(const HubSettings* settings) {
    if (!settings->defaults.enabled || !ts3_thread_is_callback()) {
        return;
    }

    char serverUid[128] = "";
    if (!ts3_get_server_uid(serverUid, sizeof(serverUid))) {
        return; /* no stable identity — never risk re-applying */
    }

    PluginConfig cfg;
    config_copy(&cfg);
    if (strcmp(cfg.defaultsAppliedServer, serverUid) == 0) {
        return; /* already applied for this server — user changes stick */
    }

    const HubDefaults* def = &settings->defaults;
    if (def->whisperKey > 0)          cfg.whisperKey = def->whisperKey;
    if (def->normalKey > 0)           cfg.normalKey = def->normalKey;
    if (def->shoutKey > 0)            cfg.shoutKey = def->shoutKey;
    if (def->voiceToggleKey > 0)      cfg.voiceToggleKey = def->voiceToggleKey;
    if (def->distanceWhisper > 0.0f)  cfg.distanceWhisper = def->distanceWhisper;
    if (def->distanceNormal > 0.0f)   cfg.distanceNormal = def->distanceNormal;
    if (def->distanceShout > 0.0f)    cfg.distanceShout = def->distanceShout;
    strncpy_s(cfg.defaultsAppliedServer, sizeof(cfg.defaultsAppliedServer),
        serverUid, _TRUNCATE);

    config_clamp(&cfg);
    config_apply(&cfg);
    config_save();
    voice_mode_reset_key_tracking();

    log_write("PROFILE: first-connection defaults applied (keys %d/%d/%d/%d, "
        "dist %.1f/%.1f/%.1f)",
        def->whisperKey, def->normalKey, def->shoutKey, def->voiceToggleKey,
        def->distanceWhisper, def->distanceNormal, def->distanceShout);
    ts3_print_to_chat("[Conan Exiles] Server default settings applied (first connection to this server).");
}

/* ---- join confirmation (chat, once per connection) -------------------------- */

static void profile_announce(const HubSettings* settings, int raceIndex) {
    if (g_announced || !ts3_thread_is_callback()) {
        return;
    }
    g_announced = 1;

    char line[512];
    char racePart[160];

    if (settings->raceCount > 0 && raceIndex >= 0) {
        snprintf(racePart, sizeof(racePart), "RACES(%d) OK [YOUR RACE: %s]",
            settings->raceCount, settings->races[raceIndex].name);
    }
    else if (settings->raceCount > 0) {
        snprintf(racePart, sizeof(racePart), "RACES(%d) OK [NOT IN RACE]",
            settings->raceCount);
    }
    else {
        snprintf(racePart, sizeof(racePart), "RACES(0)");
    }

    snprintf(line, sizeof(line),
        "[Conan Exiles] Root Parameters loaded: GLOBAL OK | %s | ZONES(%d)%s",
        racePart, settings->zoneCount, settings->zoneCount > 0 ? " OK" : "");
    ts3_print_to_chat(line);

    const unsigned long long steamID = server_profile_get_local_steam_id();
    if (steamID != 0ULL) {
        if (raceIndex >= 0) {
            snprintf(line, sizeof(line),
                "[Conan Exiles] SteamID %llu recognized - race '%s' (listen bonus +%.1f m)",
                steamID, settings->races[raceIndex].name,
                settings->races[raceIndex].listenAddDistance);
        }
        else {
            snprintf(line, sizeof(line),
                "[Conan Exiles] SteamID %llu recognized - no race assigned",
                steamID);
        }
    }
    else {
        snprintf(line, sizeof(line),
            "[Conan Exiles] No Steam ID detected - race settings unavailable");
    }
    ts3_print_to_chat(line);
}

static void profile_announce_reload(const HubSettings* settings, int raceIndex) {
    (void)settings;
    (void)raceIndex;
    if (!ts3_thread_is_callback()) {
        return;
    }
    ts3_print_to_chat("[Conan Exiles] Root parameters reloaded.");
}

/* Server force flags -> plugin.cfg (keys untouched). Same rule as the old
   plugin: only force ON, never undo a user-enabled option when the server
   stops forcing. */
static void profile_apply_hub_forces(const HubSettings* settings) {
    if (!settings || !ts3_thread_is_callback()) {
        return;
    }

    PluginConfig cfg;
    config_copy(&cfg);
    int changed = 0;

    if (settings->forceDistanceMuting && !cfg.enableDistanceMuting) {
        cfg.enableDistanceMuting = 1;
        changed = 1;
    }
    if (settings->forceAutoChannelSwitch && !cfg.enableAutomaticChannelChange) {
        cfg.enableAutomaticChannelChange = 1;
        changed = 1;
    }
    if (!changed) {
        return;
    }

    config_clamp(&cfg);
    config_apply(&cfg);
    config_save();
    log_write("PROFILE: server force flags saved (muting=%d autoChan=%d)",
        cfg.enableDistanceMuting, cfg.enableAutomaticChannelChange);
}

void server_profile_apply(const HubSettings* settings) {
    if (!settings || !settings->valid) {
        return;
    }
    profile_lock_ensure();

    const int wasActive = InterlockedCompareExchange(&g_profileActive, 0, 0) != 0;

    /* Resolve the local race BEFORE publishing, so readers never see a new
       profile with a stale race index. */
    const int raceIndex = profile_resolve_race_index(settings,
        server_profile_get_local_steam_id());
    const float listenAdd = (raceIndex >= 0)
        ? settings->races[raceIndex].listenAddDistance : 0.0f;

    EnterCriticalSection(&g_profileLock);
    g_active = *settings;
    InterlockedExchange(&g_localRaceIndex, raceIndex);
    long bits;
    memcpy(&bits, &listenAdd, sizeof(bits));
    InterlockedExchange(&g_listenAddBits, bits);
    InterlockedExchange(&g_profileActive, 1);
    LeaveCriticalSection(&g_profileLock);

    strncpy_s(g_ingamePassword, sizeof(g_ingamePassword),
        settings->ingameChannelPassword, _TRUNCATE);

    profile_apply_hub_forces(settings);
    if (!wasActive) {
        profile_apply_defaults_once(settings);
        profile_announce(settings, raceIndex);
    }
    else {
        profile_announce_reload(settings, raceIndex);
    }

    log_write("PROFILE: applied - maxVol=%.2f whisper=%.0f..%.0f normal=%.0f..%.0f "
        "shout=%.0f..%.0f forceMute=%d forceAutoChan=%d pw=%s zones=%d races=%d "
        "localRace=%d defaults=%d",
        settings->audioMaxVolume,
        settings->minWhisper, settings->maxWhisper,
        settings->minNormal, settings->maxNormal,
        settings->minShout, settings->maxShout,
        settings->forceDistanceMuting, settings->forceAutoChannelSwitch,
        settings->ingameChannelPassword[0] ? "yes" : "no",
        settings->zoneCount, settings->raceCount, raceIndex,
        settings->defaults.enabled);
    for (int i = 0; i < settings->zoneCount; i++) {
        const HubZone* z = &settings->zones[i];
        log_debug("PROFILE: zone[%d] '%s' soundproof=%d reverb=%d Y=%.1f..%.1f "
            "X=(%.1f,%.1f,%.1f,%.1f) Z=(%.1f,%.1f,%.1f,%.1f)",
            i, z->name, z->soundproof, z->reverb, z->groundY, z->topY,
            z->x1, z->x2, z->x3, z->x4, z->z1, z->z2, z->z3, z->z4);
    }
}

/* ---- getters -------------------------------------------------------------- */

/* The active flag is re-checked INSIDE the lock — a concurrent
   server_profile_reset between check and copy must not leak a stale
   profile to the caller. */
int server_profile_get(HubSettings* out) {
    if (!out) {
        return 0;
    }
    int active = 0;
    if (InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        profile_lock_ensure();
        EnterCriticalSection(&g_profileLock);
        active = InterlockedCompareExchange(&g_profileActive, 0, 0) != 0;
        if (active) {
            *out = g_active;
        }
        LeaveCriticalSection(&g_profileLock);
    }
    if (!active) {
        memset(out, 0, sizeof(*out));
        out->audioMaxVolume = 1.0f;
    }
    return active;
}

int server_profile_is_active(void) {
    return InterlockedCompareExchange(&g_profileActive, 0, 0) != 0;
}

float server_profile_get_max_volume(void) {
    HubSettings settings;
    if (!server_profile_get(&settings)) {
        return 1.0f;
    }
    return settings.audioMaxVolume;
}

int server_profile_force_auto_channel(void) {
    HubSettings settings;
    if (!server_profile_get(&settings)) {
        return 0;
    }
    return settings.forceAutoChannelSwitch;
}

const char* server_profile_get_ingame_password(void) {
    return g_ingamePassword;
}

int server_profile_get_local_race(HubRace* out) {
    const long index = InterlockedCompareExchange(&g_localRaceIndex, 0, 0);
    if (index < 0 || !InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        return 0;
    }
    if (out) {
        profile_lock_ensure();
        EnterCriticalSection(&g_profileLock);
        const int valid = InterlockedCompareExchange(&g_profileActive, 0, 0)
            && index < g_active.raceCount;
        if (valid) {
            *out = g_active.races[index];
        }
        LeaveCriticalSection(&g_profileLock);
        if (!valid) {
            memset(out, 0, sizeof(*out));
            return 0;
        }
    }
    return 1;
}

float server_profile_get_listen_add_distance(void) {
    if (InterlockedCompareExchange(&g_localRaceIndex, 0, 0) < 0
        || !InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        return 0.0f;
    }
    const long bits = InterlockedCompareExchange(&g_listenAddBits, 0, 0);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return (value > 0.0f && value < 1000.0f) ? value : 0.0f;
}

/* ---- 9.1 request driver ------------------------------------------------------ */

static uint64 profile_find_root_channel(void) {
    uint64 channels[128];
    const int count = ts3_get_channel_list(channels, 128);
    for (int i = 0; i < count; i++) {
        char name[128] = "";
        if (ts3_get_channel_name(channels[i], name, sizeof(name))
            && _stricmp(name, "root") == 0) {
            return channels[i];
        }
    }
    return 0;
}

void server_profile_tick(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    const ULONGLONG now = GetTickCount64();

    if (g_rootChannelID == 0) {
        if (now - g_lastFindMs < PROFILE_FIND_RETRY_MS) {
            return;
        }
        g_lastFindMs = now;
        g_rootChannelID = profile_find_root_channel();
        if (g_rootChannelID == 0) {
            return;
        }
        log_write("PROFILE: Root channel found (id=%llu)",
            (unsigned long long)g_rootChannelID);
    }

    /* Description arrives async — the update event clears in-flight. */
    if (g_requestInFlight) {
        if (now - g_requestSentMs < PROFILE_INFLIGHT_MAX_MS) {
            return; /* max 1 request in flight (old plugin flood bug) */
        }
        log_write("PROFILE: description request timed out - will retry");
        g_requestInFlight = 0;
    }

    /* Once a profile is active we stop polling entirely; live edits still
       arrive via onChannelDescriptionUpdateEvent (Root stays subscribed). */
    if (InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        return;
    }
    if (now - g_lastRequestMs < PROFILE_REQUEST_RETRY_MS && g_lastRequestMs != 0) {
        return;
    }

    if (ts3_request_channel_description(g_rootChannelID)) {
        g_requestInFlight = 1;
        g_requestSentMs = now;
        g_lastRequestMs = now;
        log_debug("PROFILE: description requested (root=%llu)",
            (unsigned long long)g_rootChannelID);
    }
}

static unsigned int profile_desc_hash(const char* desc) {
    unsigned int h = 2166136261u;
    if (!desc) {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)desc; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static unsigned int g_lastAppliedDescHash = 0;

static int profile_try_apply_root_description(uint64 channelID) {
    static char desc[PROFILE_DESC_MAX]; /* callback thread only — no reentry */
    if (!ts3_get_channel_description(channelID, desc, sizeof(desc))) {
        return 0;
    }

    const unsigned int hash = profile_desc_hash(desc);
    if (hash == g_lastAppliedDescHash
        && InterlockedCompareExchange(&g_profileActive, 0, 0) != 0) {
        log_debug("PROFILE: Root description unchanged - skip reload");
        return 0;
    }

    /* Parsed settings are static — ~10 KB with races/zones, too big for the
       callback thread's stack budget. Callback thread only, no reentry. */
    static HubSettings parsed;
    if (!hub_parse_settings(desc, &parsed)) {
        log_write("PROFILE: Root description has no [GLOBAL] section - ignored");
        return 0;
    }
    g_lastAppliedDescHash = hash;
    server_profile_apply(&parsed);
    return 1;
}

static int profile_ensure_root_channel_id(void) {
    if (g_rootChannelID != 0) {
        return 1;
    }
    g_rootChannelID = profile_find_root_channel();
    return g_rootChannelID != 0;
}

int server_profile_on_description_update(uint64 channelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return 0;
    }
    if (!profile_ensure_root_channel_id() || channelID != g_rootChannelID) {
        return 0;
    }
    g_requestInFlight = 0;
    return profile_try_apply_root_description(channelID);
}

int server_profile_on_channel_edited(uint64 channelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return 0;
    }
    if (!profile_ensure_root_channel_id() || channelID != g_rootChannelID) {
        return 0;
    }

    log_write("PROFILE: Root channel edited - reloading settings");
    g_requestInFlight = 0;

    const int applied = profile_try_apply_root_description(channelID);

    /* Description may not be in the local cache yet — request from server. */
    const ULONGLONG now = GetTickCount64();
    if (!g_requestInFlight && now - g_lastRequestMs >= 500) {
        if (ts3_request_channel_description(g_rootChannelID)) {
            g_requestInFlight = 1;
            g_requestSentMs = now;
            g_lastRequestMs = now;
            log_debug("PROFILE: description refresh requested after Root edit");
        }
    }
    return applied;
}

/* ---- reset ---------------------------------------------------------------------- */

void server_profile_reset(void) {
    g_rootChannelID = 0;
    g_lastFindMs = 0;
    g_requestInFlight = 0;
    g_requestSentMs = 0;
    g_lastRequestMs = 0;
    g_ingamePassword[0] = '\0';
    g_announced = 0; /* next profile after (re)connect announces again */
    g_lastAppliedDescHash = 0;
    /* Clear flag under the lock so a concurrent server_profile_get can
       never copy the profile after it was invalidated. */
    profile_lock_ensure();
    EnterCriticalSection(&g_profileLock);
    InterlockedExchange(&g_profileActive, 0);
    InterlockedExchange(&g_localRaceIndex, -1);
    InterlockedExchange(&g_listenAddBits, 0);
    LeaveCriticalSection(&g_profileLock);
}
