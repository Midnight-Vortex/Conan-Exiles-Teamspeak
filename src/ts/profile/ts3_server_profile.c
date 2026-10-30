#include "ts/profile/ts3_server_profile.h"
#include "ts/adapter/ts3_adapter.h"
#include "core/config/config.h"
#include "core/config/range_units.h"
#include "core/hub/mod_password.h"
#include "core/proximity/occlusion.h"
#include "core/voice/voice_modes.h"
#include "core/util/log.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROFILE_FIND_RETRY_MS      5000   /* re-resolve missing Root channel */
#define PROFILE_REQUEST_RETRY_MS   30000  /* re-request until a profile arrived */
#define PROFILE_INFLIGHT_MAX_MS    10000  /* pending request times out */
#define PROFILE_DESC_MAX           16384
#define PROFILE_MAX_CHANNELS       1024   /* scan cap (was 128, 086) */
#define PROFILE_REFRESH_MIN_MS     500    /* min gap between tries to refresh a replaced Root */

/* Request state — TS callback thread only. */
static uint64 g_rootChannelID = 0;
static ULONGLONG g_lastFindMs = 0;
static int g_requestInFlight = 0;
static ULONGLONG g_requestSentMs = 0;
static ULONGLONG g_lastRequestMs = 0;
static volatile long g_rootDirty = 0;     /* 086: channel list changed - re-resolve Root */
static int g_rootRefreshWanted = 0;       /* 086: Root ID changed - re-request its description */
static ULONGLONG g_lastRefreshTryMs = 0;

/* Channel list scan buffer (8 KB). Static — too big for the callback thread's
   stack budget; the scan is never re-entered. Callback thread only. */
static uint64 g_channelScan[PROFILE_MAX_CHANNELS];

/* Active profile — written on the callback thread, read from any thread. */
static CRITICAL_SECTION g_profileLock;
static INIT_ONCE g_profileLockOnce = INIT_ONCE_STATIC_INIT;
static HubSettings g_active;
static volatile long g_profileActive = 0;

/* Callback thread only (password consumer runs there too). */
static char g_ingamePassword[HUB_PASSWORD_LEN] = "";

/* Scratch buffers of the Root description parse. File scope (not function
   statics) so server_profile_reset can wipe them: the description of a legacy
   server still carries IngameChannelPassword in clear text. Callback thread
   only, no reentry. */
static char g_descBuf[PROFILE_DESC_MAX];
static HubSettings g_parsedBuf; /* ~10 KB with races/zones — too big for the stack */

/* 086: scalar getters read these instead of copying the whole HubSettings
   (~8 KB) under the lock. They always hold the EFFECTIVE value: the profile's
   value while a profile is active, the "no profile" default otherwise (set in
   server_profile_reset). Floats are stored as their raw bit pattern. */
#define PROFILE_BITS_FLOAT_1_0   0x3F800000L /* 1.0f   - default max volume */
#define PROFILE_BITS_FLOAT_100_0 0x42C80000L /* 100.0f - default filter intensity */
static volatile long g_maxVolumeBits = PROFILE_BITS_FLOAT_1_0;
static volatile long g_filterIntensityBits = PROFILE_BITS_FLOAT_100_0;
static volatile long g_forceAutoChannel = 0;
static volatile long g_forceSpeakersOn = 0;
static volatile long g_forceRangesInFoundations = 0; /* 091 */
static volatile long g_forceDistanceMuting = 0;
static volatile long g_nicknameRandomizer = 1; /* legacy default without profile */
static volatile long g_realisticAudio = 0;

/* Local race match (index into g_active.races, -1 = none) + listener bonus.
   Written on the callback thread during apply, read from any thread. */
static volatile long g_localRaceIndex = -1;
static volatile long g_listenAddBits = 0; /* float stored as raw bits */
static volatile long g_hrtfEnabled = 0;   /* EnableHrtf, read lock-free by the audio thread */

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

static long profile_float_bits(float value) {
    int32_t bits; /* fixed 32 bit, independent of the platform's long */
    memcpy(&bits, &value, sizeof(bits));
    return (long)bits;
}

static float profile_bits_float(long bits) {
    const int32_t raw = (int32_t)bits;
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

/* Publish the scalar getters' values (profile applied) or their "no profile"
   defaults (settings == NULL). Called under g_profileLock. */
static void profile_publish_scalars(const HubSettings* settings) {
    InterlockedExchange(&g_maxVolumeBits,
        settings ? profile_float_bits(settings->audioMaxVolume) : PROFILE_BITS_FLOAT_1_0);
    InterlockedExchange(&g_filterIntensityBits,
        settings ? profile_float_bits(settings->filterIntensity) : PROFILE_BITS_FLOAT_100_0);
    InterlockedExchange(&g_forceAutoChannel, settings ? settings->forceAutoChannelSwitch : 0);
    InterlockedExchange(&g_forceSpeakersOn, settings ? settings->forceSpeakersOn : 0);
    InterlockedExchange(&g_forceRangesInFoundations, settings ? settings->forceRangesInFoundations : 0);
    InterlockedExchange(&g_forceDistanceMuting, settings ? settings->forceDistanceMuting : 0);
    InterlockedExchange(&g_nicknameRandomizer, settings ? settings->nicknameRandomizer : 1);
    InterlockedExchange(&g_realisticAudio, settings ? settings->realisticAudio : 0);
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

typedef struct ProfileDefaultsCtx {
    const HubDefaults* defaults;
    const char* serverUid;
    int applied;
} ProfileDefaultsCtx;

/* Runs on g_config under the config lock (config_modify) — the "already
   applied" check and the write are one step, so a concurrent settings change
   from another thread can no longer be lost (086). */
static void profile_defaults_mutator(PluginConfig* cfg, void* ctx) {
    ProfileDefaultsCtx* c = (ProfileDefaultsCtx*)ctx;
    if (strcmp(cfg->defaultsAppliedServer, c->serverUid) == 0) {
        return; /* already applied for this server — user changes stick */
    }

    const HubDefaults* def = c->defaults;
    if (def->whisperKey > 0)          cfg->whisperKey = def->whisperKey;
    if (def->normalKey > 0)           cfg->normalKey = def->normalKey;
    if (def->shoutKey > 0)            cfg->shoutKey = def->shoutKey;
    if (def->voiceToggleKey > 0)      cfg->voiceToggleKey = def->voiceToggleKey;
    if (def->distanceWhisper > 0.0f)  cfg->distanceWhisper = def->distanceWhisper;
    if (def->distanceNormal > 0.0f)   cfg->distanceNormal = def->distanceNormal;
    if (def->distanceShout > 0.0f)    cfg->distanceShout = def->distanceShout;
    strncpy_s(cfg->defaultsAppliedServer, sizeof(cfg->defaultsAppliedServer),
        c->serverUid, _TRUNCATE);
    c->applied = 1;
}

static void profile_apply_defaults_once(const HubSettings* settings) {
    if (!settings->defaults.enabled || !ts3_thread_is_callback()) {
        return;
    }

    char serverUid[128] = "";
    if (!ts3_get_server_uid(serverUid, sizeof(serverUid))) {
        return; /* no stable identity — never risk re-applying */
    }

    ProfileDefaultsCtx ctx = { &settings->defaults, serverUid, 0 };
    config_modify(profile_defaults_mutator, &ctx); /* clamps, does not save */
    if (!ctx.applied) {
        return;
    }
    config_save();
    voice_mode_reset_key_tracking();

    const HubDefaults* def = &settings->defaults;
    log_write("PROFILE: first-connection defaults applied (keys %d/%d/%d/%d, "
        "dist %.1f/%.1f/%.1f)",
        def->whisperKey, def->normalKey, def->shoutKey, def->voiceToggleKey,
        def->distanceWhisper, def->distanceNormal, def->distanceShout);
    ts3_print_to_chat("[Conan Exiles] Server default settings applied (first connection to this server).");
}

/* ---- join confirmation (chat, once per connection) -------------------------- */

/* "SteamID .. recognized - race '..' (listen bonus +..)" chat line.  The bonus
   is stored and used in meters; in foundation mode (091) the line also shows it
   in foundations, same style as the voice-mode line: "+1.0 foundations (2.6 m)". */
static void profile_listen_bonus_line(char* out, size_t cap, unsigned long long steamID,
    const char* raceName, float bonusMeters, int inFoundations) {
    if (inFoundations) {
        snprintf(out, cap,
            "[Conan Exiles] SteamID %llu recognized - race '%s' (listen bonus +%.1f %s (%.1f m))",
            steamID, raceName, range_from_meters(bonusMeters, 1), range_unit_name_a(1), bonusMeters);
    }
    else {
        snprintf(out, cap,
            "[Conan Exiles] SteamID %llu recognized - race '%s' (listen bonus +%.1f m)",
            steamID, raceName, bonusMeters);
    }
}

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
            /* Own choice (g_config scalar read) OR the Root force (091). */
            profile_listen_bonus_line(line, sizeof(line), steamID,
                settings->races[raceIndex].name, settings->races[raceIndex].listenAddDistance,
                range_effective_unit(g_config.rangeInFoundations, settings->forceRangesInFoundations));
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
typedef struct ProfileForcesCtx {
    int forceDistanceMuting;
    int forceAutoChannelSwitch;
    int changed;
    int muting;   /* resulting values, for the log line */
    int autoChan;
} ProfileForcesCtx;

/* Runs on g_config under the config lock (config_modify), see 086. */
static void profile_forces_mutator(PluginConfig* cfg, void* ctx) {
    ProfileForcesCtx* c = (ProfileForcesCtx*)ctx;
    if (c->forceDistanceMuting && !cfg->enableDistanceMuting) {
        cfg->enableDistanceMuting = 1;
        c->changed = 1;
    }
    if (c->forceAutoChannelSwitch && !cfg->enableAutomaticChannelChange) {
        cfg->enableAutomaticChannelChange = 1;
        c->changed = 1;
    }
    c->muting = cfg->enableDistanceMuting;
    c->autoChan = cfg->enableAutomaticChannelChange;
}

static void profile_apply_hub_forces(const HubSettings* settings) {
    if (!settings || !ts3_thread_is_callback()) {
        return;
    }
    if (!settings->forceDistanceMuting && !settings->forceAutoChannelSwitch) {
        return;
    }

    ProfileForcesCtx ctx = { settings->forceDistanceMuting, settings->forceAutoChannelSwitch, 0, 0, 0 };
    config_modify(profile_forces_mutator, &ctx); /* clamps, does not save */
    if (!ctx.changed) {
        return;
    }
    config_save();
    log_write("PROFILE: server force flags saved (muting=%d autoChan=%d)",
        ctx.muting, ctx.autoChan);
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
    InterlockedExchange(&g_listenAddBits, profile_float_bits(listenAdd));
    InterlockedExchange(&g_hrtfEnabled, settings->enableHrtf ? 1 : 0);
    profile_publish_scalars(settings); /* 086: before the active flag, like the race index */
    occlusion_set_enabled(settings->enableOcclusion); /* 064 root-channel switch */
    occlusion_set_intensity(settings->occlusionIntensity); /* 069 */
    occlusion_set_sound_params(settings->occAttackMs, settings->occReleaseMs,
        settings->occDiffraction, settings->occReverb); /* 072 */
    occlusion_set_mod_params(settings->occModParams);  /* 072: forwarded as occcfg */
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
        "shout=%.0f..%.0f forceMute=%d forceAutoChan=%d forceSpeakers=%d nickRand=%d realistic=%d filter=%.0f hrtf=%d occlusion=%d occIntensity=%.0f occAtk=%.0f occRel=%.0f occDiff=%.0f occRev=%.0f "
        "pw=%s zones=%d races=%d localRace=%d defaults=%d",
        settings->audioMaxVolume,
        settings->minWhisper, settings->maxWhisper,
        settings->minNormal, settings->maxNormal,
        settings->minShout, settings->maxShout,
        settings->forceDistanceMuting, settings->forceAutoChannelSwitch, settings->forceSpeakersOn,
        settings->nicknameRandomizer,
        settings->realisticAudio, settings->filterIntensity, settings->enableHrtf,
        settings->enableOcclusion, settings->occlusionIntensity,
        settings->occAttackMs, settings->occReleaseMs, settings->occDiffraction, settings->occReverb,
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
    return profile_bits_float(InterlockedCompareExchange(&g_maxVolumeBits, 0, 0));
}

int server_profile_force_auto_channel(void) {
    return InterlockedCompareExchange(&g_forceAutoChannel, 0, 0) != 0;
}

int server_profile_force_distance_muting(void) {
    return InterlockedCompareExchange(&g_forceDistanceMuting, 0, 0) != 0;
}

int server_profile_force_speakers_on(void) {
    return InterlockedCompareExchange(&g_forceSpeakersOn, 0, 0) != 0;
}

int server_profile_force_ranges_in_foundations(void) {
    return InterlockedCompareExchange(&g_forceRangesInFoundations, 0, 0) != 0;
}

int server_profile_get_nickname_randomizer(void) {
    return InterlockedCompareExchange(&g_nicknameRandomizer, 0, 0) != 0;
}

int server_profile_copy_ingame_password(char* out, int outLen) {
    if (!out || outLen <= 0) {
        return 0;
    }
    if (mod_password_get(out, outLen, GetTickCount64())) {
        return 2;
    }
    strncpy_s(out, (size_t)outLen, g_ingamePassword, _TRUNCATE);
    return out[0] ? 1 : 0;
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
    const float value = profile_bits_float(InterlockedCompareExchange(&g_listenAddBits, 0, 0));
    return (value > 0.0f && value < 1000.0f) ? value : 0.0f;
}

int server_profile_get_realistic_audio(void) {
    return InterlockedCompareExchange(&g_realisticAudio, 0, 0) != 0;
}

int server_profile_get_hrtf_enabled(void) {
    return InterlockedCompareExchange(&g_profileActive, 0, 0) != 0
        && InterlockedCompareExchange(&g_hrtfEnabled, 0, 0) != 0;
}

float server_profile_get_filter_intensity(void) {
    return profile_bits_float(InterlockedCompareExchange(&g_filterIntensityBits, 0, 0));
}

/* ---- 9.1 request driver ------------------------------------------------------ */

/* Scan the channel list for the "Root" channel. Returns 0 when the list could
   not be read (the caller keeps what it has), else 1 with *outRoot set (0 =
   no Root in the list) and *outTruncated = 1 when the list hit the scan cap.
   086: a top-level "Root" wins over a sub-channel with the same name — a user
   who may create channels must not be able to shadow the real Root with a
   crafted [GLOBAL] description. When no top-level match exists the first
   match is used (the old behaviour), so existing servers keep working. */
static int profile_find_root_channel(uint64* outRoot, int* outTruncated) {
    *outRoot = 0;
    *outTruncated = 0;
    const int count = ts3_get_channel_list(g_channelScan, PROFILE_MAX_CHANNELS);
    if (count <= 0) {
        return 0;
    }
    *outTruncated = count >= PROFILE_MAX_CHANNELS;

    uint64 firstMatch = 0;
    for (int i = 0; i < count; i++) {
        char name[128] = "";
        if (!ts3_get_channel_name(g_channelScan[i], name, sizeof(name))
            || _stricmp(name, "root") != 0) {
            continue;
        }
        if (firstMatch == 0) {
            firstMatch = g_channelScan[i];
        }
        uint64 parent = 0;
        if (ts3_get_channel_parent(g_channelScan[i], &parent) && parent == 0) {
            *outRoot = g_channelScan[i]; /* top-level Root */
            return 1;
        }
    }
    *outRoot = firstMatch;
    return 1;
}

/* Re-resolve the Root channel ID now. The old ID stays in place when the list
   is unreadable or truncated without a hit. */
static void profile_rescan_root(ULONGLONG now) {
    g_lastFindMs = now;
    InterlockedExchange(&g_rootDirty, 0);

    uint64 found = 0;
    int truncated = 0;
    if (!profile_find_root_channel(&found, &truncated)) {
        return;
    }
    if (found == 0 && g_rootChannelID != 0 && truncated) {
        return; /* Root may lie beyond the scan cap — keep the cached ID */
    }
    if (found == g_rootChannelID) {
        return;
    }

    const uint64 old = g_rootChannelID;
    g_rootChannelID = found;
    g_requestInFlight = 0; /* an open request belonged to the old channel */
    g_lastRequestMs = 0;   /* the new channel is requested right away, not after the 30 s retry */
    if (found == 0) {
        log_write("PROFILE: Root channel %llu is gone", (unsigned long long)old);
    }
    else if (old == 0) {
        log_write("PROFILE: Root channel found (id=%llu)", (unsigned long long)found);
    }
    else {
        log_write("PROFILE: Root channel changed (id %llu -> %llu) - reloading settings",
            (unsigned long long)old, (unsigned long long)found);
        g_rootRefreshWanted = 1; /* new channel: fetch its description even with an active profile */
    }
}

/* Make sure a Root ID is known, with the existing retry throttle while it is
   missing (086: no scan per channel event on non-Conan servers). The tick
   passes honorDirty=1 so a channel-list change (C4) forces exactly one
   rescan; event handlers pass 0 and leave the flag for the tick, otherwise
   the burst of onNewChannel events at connect would rescan once per event. */
static int profile_resolve_root(ULONGLONG now, int honorDirty) {
    const int dirty = honorDirty && InterlockedCompareExchange(&g_rootDirty, 0, 0) != 0;
    if (dirty || (g_rootChannelID == 0 && now - g_lastFindMs >= PROFILE_FIND_RETRY_MS)) {
        profile_rescan_root(now);
    }
    return g_rootChannelID != 0;
}

void server_profile_on_channel_list_changed(uint64 channelID) {
    (void)channelID; /* which channel does not matter: one coalesced rescan */
    if (!ts3_thread_is_callback()) {
        return;
    }
    InterlockedExchange(&g_rootDirty, 1);
    ts3_request_wakeup(); /* the CEDRAIN drain runs server_profile_tick */
}

void server_profile_tick(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    const ULONGLONG now = GetTickCount64();

    if (!profile_resolve_root(now, 1)) {
        return;
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
       arrive via onChannelDescriptionUpdateEvent (Root stays subscribed).
       Exception (086): the Root channel was replaced — its description has
       to be fetched once. */
    if (InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        if (!g_rootRefreshWanted
            || (g_lastRefreshTryMs != 0 && now - g_lastRefreshTryMs < PROFILE_REFRESH_MIN_MS)) {
            return;
        }
        g_lastRefreshTryMs = now; /* also when the request fails: no API/log spam per drain */
    }
    else if (now - g_lastRequestMs < PROFILE_REQUEST_RETRY_MS && g_lastRequestMs != 0) {
        return;
    }

    if (ts3_request_channel_description(g_rootChannelID)) {
        g_requestInFlight = 1;
        g_requestSentMs = now;
        g_lastRequestMs = now;
        g_rootRefreshWanted = 0;
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
    char* const desc = g_descBuf; /* file scope: wiped by server_profile_reset */
    if (!ts3_get_channel_description(channelID, desc, sizeof(g_descBuf))) {
        return 0;
    }

    const unsigned int hash = profile_desc_hash(desc);
    if (hash == g_lastAppliedDescHash
        && InterlockedCompareExchange(&g_profileActive, 0, 0) != 0) {
        log_debug("PROFILE: Root description unchanged - skip reload");
        return 0;
    }

    /* Parsed settings are file-scope static (g_parsedBuf) — ~10 KB with
       races/zones, too big for the callback thread's stack budget. Callback
       thread only, no reentry. */
    if (!hub_parse_settings(desc, &g_parsedBuf)) {
        log_write("PROFILE: Root description has no [GLOBAL] section - ignored");
        return 0;
    }
    g_lastAppliedDescHash = hash;
    server_profile_apply(&g_parsedBuf);
    return 1;
}

/* Event handlers: known Root, or a throttled lookup while it is missing. They
   never consume the channel-list dirty flag (see profile_resolve_root). */
static int profile_ensure_root_channel_id(void) {
    return profile_resolve_root(GetTickCount64(), 0);
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
    InterlockedExchange(&g_rootDirty, 0);
    g_rootRefreshWanted = 0;
    g_lastRefreshTryMs = 0;
    /* Legacy root-channel password (IngameChannelPassword) must not outlive the
       connection: wipe it and every buffer it was parsed through (086). */
    SecureZeroMemory(g_ingamePassword, sizeof(g_ingamePassword));
    SecureZeroMemory(g_descBuf, sizeof(g_descBuf));
    SecureZeroMemory(&g_parsedBuf, sizeof(g_parsedBuf));
    mod_password_clear(); /* 077: belongs to the old connection */
    g_announced = 0; /* next profile after (re)connect announces again */
    g_lastAppliedDescHash = 0;
    /* Clear flag under the lock so a concurrent server_profile_get can
       never copy the profile after it was invalidated. */
    profile_lock_ensure();
    EnterCriticalSection(&g_profileLock);
    InterlockedExchange(&g_profileActive, 0);
    InterlockedExchange(&g_localRaceIndex, -1);
    InterlockedExchange(&g_listenAddBits, 0);
    InterlockedExchange(&g_hrtfEnabled, 0);
    profile_publish_scalars(NULL); /* 086: back to the "no profile" defaults */
    SecureZeroMemory(&g_active, sizeof(g_active)); /* carries the legacy password too */
    LeaveCriticalSection(&g_profileLock);
    occlusion_set_enabled(0); /* 064: no server profile -> no occlusion */
    occlusion_set_intensity((float)OCCLUSION_INTENSITY_DEFAULT);
    occlusion_set_sound_params((float)OCCLUSION_ATTACK_MS_DEFAULT, (float)OCCLUSION_RELEASE_MS_DEFAULT,
        (float)OCCLUSION_DIFFRACTION_DEFAULT, (float)OCCLUSION_REVERB_DEFAULT);
    occlusion_set_mod_params(NULL);
}
