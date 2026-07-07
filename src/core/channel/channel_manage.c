#include "core/channel/channel_manage.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "core/mod_file/pos_file.h"
#include "core/config/config.h"
#include "core/util/log.h"

#include <windows.h>
#include <string.h>

#define CHAN_TICK_MIN_MS          500   /* max tick rate (called per TS event) */
#define CHAN_FIND_RETRY_MS        5000  /* re-resolve missing channels */
#define CHAN_MOVE_COOLDOWN_MS     2000  /* min gap between two move requests */
#define CHAN_MOVE_INFLIGHT_MAX_MS 5000  /* stuck in-flight flag times out */
#define CHAN_MAX_CHANNELS         128

/* All state is TS callback thread only — no locks needed. */
static uint64 g_hubChannelID = 0;
static uint64 g_ingameChannelID = 0;
static ULONGLONG g_lastFindMs = 0;

static int g_moveInFlight = 0;
static ULONGLONG g_moveInFlightSince = 0;
static ULONGLONG g_lastMoveMs = 0;
static ULONGLONG g_lastTickMs = 0;

/* ---- 8.1 find hub + ingame channels ----------------------------------------- */

int chan_find_hub_and_ingame(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return 0;
    }

    uint64 channels[CHAN_MAX_CHANNELS];
    const int count = ts3_get_channel_list(channels, CHAN_MAX_CHANNELS);

    uint64 hub = 0;
    uint64 ingame = 0;
    for (int i = 0; i < count; i++) {
        char name[128] = "";
        if (!ts3_get_channel_name(channels[i], name, sizeof(name))) {
            continue;
        }
        if (hub == 0 && _stricmp(name, "hub") == 0) {
            hub = channels[i];
        }
        else if (ingame == 0 && _stricmp(name, "ingame") == 0) {
            ingame = channels[i];
        }
    }

    if (hub != g_hubChannelID || ingame != g_ingameChannelID) {
        g_hubChannelID = hub;
        g_ingameChannelID = ingame;
        log_write("CHAN: resolved hub=%llu ingame=%llu (%d channels)",
            (unsigned long long)hub, (unsigned long long)ingame, count);
    }
    return g_hubChannelID != 0 && g_ingameChannelID != 0;
}

/* ---- 8.2 decision (pure) ------------------------------------------------------ */

int chan_should_be_ingame(void) {
    return pos_coordinates_valid();
}

/* ---- 8.3 move request (in-flight + cooldown) ----------------------------------- */

int chan_request_move(uint64 targetChannelID) {
    if (!ts3_thread_is_callback() || !ts3_is_connected() || targetChannelID == 0) {
        return 0;
    }

    const ULONGLONG now = GetTickCount64();

    if (g_moveInFlight) {
        if (now - g_moveInFlightSince < CHAN_MOVE_INFLIGHT_MAX_MS) {
            return 0; /* never two moves in parallel (old plugin bug) */
        }
        log_write("CHAN: move in-flight timed out - clearing");
        g_moveInFlight = 0;
    }
    if (now - g_lastMoveMs < CHAN_MOVE_COOLDOWN_MS) {
        return 0;
    }

    const anyID localID = ts3_get_local_client_id();
    if (localID == 0) {
        return 0;
    }
    if (!ts3_request_client_move(localID, targetChannelID)) {
        return 0;
    }

    g_moveInFlight = 1;
    g_moveInFlightSince = now;
    g_lastMoveMs = now;
    log_write("CHAN: move requested -> channel %llu (%s)",
        (unsigned long long)targetChannelID,
        targetChannelID == g_ingameChannelID ? "ingame"
        : targetChannelID == g_hubChannelID ? "hub" : "?");
    return 1;
}

/* ---- 8.4 playback gate from current channel ------------------------------------- */

static void chan_update_audio_mode(uint64 ownChannelID) {
    if (g_hubChannelID != 0 && ownChannelID == g_hubChannelID) {
        ts3_audio_set_mode(TS3_AUDIO_MUTE);
    }
    else if (g_ingameChannelID != 0 && ownChannelID == g_ingameChannelID) {
        ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);
    }
    else {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }
}

/* ---- tick driver ------------------------------------------------------------------ */

void chan_tick(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTickMs < CHAN_TICK_MIN_MS) {
        return;
    }
    g_lastTickMs = now;

    if (!g_config.enableAutomaticChannelChange) {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
        return;
    }

    /* (Re-)resolve channel IDs, throttled while missing. */
    if (g_hubChannelID == 0 || g_ingameChannelID == 0) {
        if (now - g_lastFindMs < CHAN_FIND_RETRY_MS) {
            return;
        }
        g_lastFindMs = now;
        if (!chan_find_hub_and_ingame()) {
            ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
            return;
        }
    }

    const anyID localID = ts3_get_local_client_id();
    if (localID == 0) {
        return;
    }
    const uint64 ownChannel = ts3_get_channel_of_client(localID);
    if (ownChannel == 0) {
        return;
    }

    chan_update_audio_mode(ownChannel);

    int moveWanted = 0;
    if (chan_should_be_ingame()) {
        if (ownChannel != g_ingameChannelID) {
            moveWanted = 1;
            chan_request_move(g_ingameChannelID);
        }
    }
    else if (ownChannel == g_ingameChannelID) {
        /* Game closed while ingame -> back to hub. Users sitting in other
           channels are left alone. */
        moveWanted = 1;
        chan_request_move(g_hubChannelID);
    }

    /* A blocked/pending move must be retried even when no TS event arrives:
       keep the CEDRAIN wakeup loop alive (rate-limited inside the adapter)
       until the own-move event confirms the new channel. */
    if (moveWanted) {
        ts3_request_wakeup();
    }
}

/* ---- events / reset ---------------------------------------------------------------- */

void chan_on_own_move(uint64 newChannelID) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    g_moveInFlight = 0;
    g_moveInFlightSince = 0;
    chan_update_audio_mode(newChannelID);
    log_debug("CHAN: own move -> channel %llu", (unsigned long long)newChannelID);
}

void chan_reset(void) {
    g_hubChannelID = 0;
    g_ingameChannelID = 0;
    g_lastFindMs = 0;
    g_moveInFlight = 0;
    g_moveInFlightSince = 0;
    g_lastMoveMs = 0;
    g_lastTickMs = 0;
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
}
