#include "ts/profile/ts3_server_profile.h"
#include "ts/adapter/ts3_adapter.h"
#include "core/util/log.h"

#include <windows.h>
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
static volatile long g_profileLockReady = 0;
static HubSettings g_active;
static volatile long g_profileActive = 0;

/* Callback thread only (password consumer runs there too). */
static char g_ingamePassword[HUB_PASSWORD_LEN] = "";

static void profile_lock_ensure(void) {
    if (InterlockedCompareExchange(&g_profileLockReady, 0, 0)) {
        return;
    }
    InitializeCriticalSection(&g_profileLock);
    InterlockedExchange(&g_profileLockReady, 1);
}

/* ---- 9.3 apply ---------------------------------------------------------- */

void server_profile_apply(const HubSettings* settings) {
    if (!settings || !settings->valid) {
        return;
    }
    profile_lock_ensure();

    EnterCriticalSection(&g_profileLock);
    g_active = *settings;
    LeaveCriticalSection(&g_profileLock);
    InterlockedExchange(&g_profileActive, 1);

    strncpy_s(g_ingamePassword, sizeof(g_ingamePassword),
        settings->ingameChannelPassword, _TRUNCATE);

    log_write("PROFILE: applied - maxVol=%.2f whisper=%.0f..%.0f normal=%.0f..%.0f "
        "shout=%.0f..%.0f forceMute=%d forceAutoChan=%d pw=%s zones=%d",
        settings->audioMaxVolume,
        settings->minWhisper, settings->maxWhisper,
        settings->minNormal, settings->maxNormal,
        settings->minShout, settings->maxShout,
        settings->forceDistanceMuting, settings->forceAutoChannelSwitch,
        settings->ingameChannelPassword[0] ? "yes" : "no",
        settings->zoneCount);
    for (int i = 0; i < settings->zoneCount; i++) {
        const HubZone* z = &settings->zones[i];
        log_debug("PROFILE: zone[%d] '%s' soundproof=%d reverb=%d Y=%.1f..%.1f "
            "X=(%.1f,%.1f,%.1f,%.1f) Z=(%.1f,%.1f,%.1f,%.1f)",
            i, z->name, z->soundproof, z->reverb, z->groundY, z->topY,
            z->x1, z->x2, z->x3, z->x4, z->z1, z->z2, z->z3, z->z4);
    }
}

/* ---- getters -------------------------------------------------------------- */

int server_profile_get(HubSettings* out) {
    if (!out) {
        return 0;
    }
    if (!InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        memset(out, 0, sizeof(*out));
        out->audioMaxVolume = 1.0f;
        return 0;
    }
    profile_lock_ensure();
    EnterCriticalSection(&g_profileLock);
    *out = g_active;
    LeaveCriticalSection(&g_profileLock);
    return 1;
}

float server_profile_get_max_volume(void) {
    if (!InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        return 1.0f;
    }
    profile_lock_ensure();
    EnterCriticalSection(&g_profileLock);
    const float v = g_active.audioMaxVolume;
    LeaveCriticalSection(&g_profileLock);
    return v;
}

int server_profile_force_auto_channel(void) {
    if (!InterlockedCompareExchange(&g_profileActive, 0, 0)) {
        return 0;
    }
    profile_lock_ensure();
    EnterCriticalSection(&g_profileLock);
    const int f = g_active.forceAutoChannelSwitch;
    LeaveCriticalSection(&g_profileLock);
    return f;
}

const char* server_profile_get_ingame_password(void) {
    return g_ingamePassword;
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

void server_profile_on_description_update(uint64 channelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }
    if (g_rootChannelID == 0 || channelID != g_rootChannelID) {
        return;
    }
    g_requestInFlight = 0;

    static char desc[PROFILE_DESC_MAX]; /* callback thread only — no reentry */
    if (!ts3_get_channel_description(channelID, desc, sizeof(desc))) {
        log_write("PROFILE: failed to fetch Root description");
        return;
    }

    HubSettings parsed;
    if (!hub_parse_settings(desc, &parsed)) {
        log_write("PROFILE: Root description has no [GLOBAL] section - ignored");
        return;
    }
    server_profile_apply(&parsed);
}

/* ---- reset ---------------------------------------------------------------------- */

void server_profile_reset(void) {
    g_rootChannelID = 0;
    g_lastFindMs = 0;
    g_requestInFlight = 0;
    g_requestSentMs = 0;
    g_lastRequestMs = 0;
    g_ingamePassword[0] = '\0';
    InterlockedExchange(&g_profileActive, 0);
}
