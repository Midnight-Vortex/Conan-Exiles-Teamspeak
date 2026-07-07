#include "core/channel/channel_manage.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/mod_file/pos_file.h"
#include "core/nick/nick_anonymize.h"
#include "core/config/config.h"
#include "core/util/log.h"

#include <windows.h>
#include <string.h>
#include <ctype.h>

#define CHAN_TICK_MIN_MS          500   /* max tick rate (called per TS event) */
#define CHAN_FIND_RETRY_MS        5000  /* re-resolve missing channels */
#define CHAN_MOVE_COOLDOWN_MS     2000  /* min gap between two move requests */
#define CHAN_MOVE_INFLIGHT_MAX_MS 5000  /* stuck in-flight flag times out */
#define CHAN_INGAME_DWELL_MS      30000 /* after ingame arrival: no hub bounce during login */
#define CHAN_MAX_CHANNELS         128
#define CHAN_IGNORE_CACHE_SIZE    16
#define CHAN_IGNORE_CACHE_TTL_MS  30000

/* All state is TS callback thread only — no locks needed. */
static uint64 g_hubChannelID = 0;
static uint64 g_ingameChannelID = 0;
static ULONGLONG g_lastFindMs = 0;

static int g_moveInFlight = 0;
static ULONGLONG g_moveInFlightSince = 0;
static ULONGLONG g_lastMoveMs = 0;
static ULONGLONG g_lastTickMs = 0;
static volatile long g_positionUpdatePending = 0;
static ULONGLONG g_lastMoveSkipLogMs = 0;
static ULONGLONG g_ingameArrivalMs = 0;

static struct {
    uint64_t channelID;
    int ignored;
    ULONGLONG cachedAt;
} g_ignoreCache[CHAN_IGNORE_CACHE_SIZE];

/* SaltyChat-style: channels whose name contains "event" or "plot" as a whole
   word suspend auto-move (Event, [Event] Halle, Plot Nord — not Development). */
static int chan_name_has_ignore_keyword(const char* name) {
    static const char* const keywords[] = { "event", "plot" };

    if (!name || !name[0]) {
        return 0;
    }

    for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        const char* kw = keywords[k];
        const size_t kwLen = strlen(kw);

        for (const char* p = name; *p; p++) {
            if (_strnicmp(p, kw, kwLen) != 0) {
                continue;
            }
            const int startOk = (p == name) || !isalpha((unsigned char)p[-1]);
            const int endOk = !isalpha((unsigned char)p[kwLen]);
            if (startOk && endOk) {
                return 1;
            }
        }
    }
    return 0;
}

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
    if (pos_coordinates_valid()) {
        return 1;
    }
    /* Conan login can pause Pos.txt briefly — stay ingame instead of hub bounce. */
    if (g_ingameArrivalMs != 0) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_ingameArrivalMs < CHAN_INGAME_DWELL_MS) {
            return 1;
        }
    }
    return 0;
}

/* ---- channel identity (numeric ID or name fallback) --------------------------- */

static int chan_channel_name_is(const uint64 channelID, const char* expected) {
    char name[128] = "";
    return channelID != 0
        && ts3_get_channel_name(channelID, name, sizeof(name))
        && _stricmp(name, expected) == 0;
}

static int chan_is_hub_channel(uint64 channelID) {
    if (g_hubChannelID != 0) {
        return channelID == g_hubChannelID;
    }
    return chan_channel_name_is(channelID, "hub");
}

static int chan_is_ingame_channel(uint64 channelID) {
    if (g_ingameChannelID != 0) {
        return channelID == g_ingameChannelID;
    }
    return chan_channel_name_is(channelID, "ingame");
}

static int chan_is_root_channel(uint64 channelID) {
    return chan_channel_name_is(channelID, "root");
}

static int chan_is_ignored_channel(uint64 channelID) {
    if (channelID == 0) {
        return 0;
    }
    /* Managed channels are never ignore channels. */
    if (chan_is_hub_channel(channelID) || chan_is_ingame_channel(channelID)
        || chan_is_root_channel(channelID)) {
        return 0;
    }

    const ULONGLONG now = GetTickCount64();
    size_t slot = 0;
    ULONGLONG oldest = (ULONGLONG)-1;

    for (size_t i = 0; i < CHAN_IGNORE_CACHE_SIZE; i++) {
        if (g_ignoreCache[i].channelID == channelID) {
            if (now - g_ignoreCache[i].cachedAt < CHAN_IGNORE_CACHE_TTL_MS) {
                return g_ignoreCache[i].ignored;
            }
            slot = i;
            oldest = 0;
            break;
        }
        if (g_ignoreCache[i].cachedAt < oldest) {
            oldest = g_ignoreCache[i].cachedAt;
            slot = i;
        }
    }

    char name[128] = "";
    int ignored = 0;
    if (ts3_get_channel_name(channelID, name, sizeof(name))) {
        ignored = chan_name_has_ignore_keyword(name);
        if (ignored) {
            static ULONGLONG lastIgnLog = 0;
            if (now - lastIgnLog >= 10000) {
                lastIgnLog = now;
                log_debug("CHAN: channel %llu '%s' is ignore channel (no auto-move)",
                    (unsigned long long)channelID, name);
            }
        }
    }

    g_ignoreCache[slot].channelID = 0;
    g_ignoreCache[slot].ignored = ignored;
    g_ignoreCache[slot].cachedAt = now;
    g_ignoreCache[slot].channelID = channelID;
    return ignored;
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
    /* Ingame channel may be password-protected (server profile, Phase 9).
       Never send requestClientMove before Root is loaded — empty password
       triggers a visible TS error on reconnect (old plugin hub-read gate). */
    const int toIngame = chan_is_ingame_channel(targetChannelID);
    if (toIngame && !server_profile_is_active()) {
        log_debug("CHAN: ingame move deferred - waiting for Root profile");
        return 0;
    }

    const char* password = toIngame ? server_profile_get_ingame_password() : "";

    /* Rename while still in the hub so ingame clients never see the
       "realName -> digits" rename event (Phase 12). */
    if (toIngame) {
        nick_anonymize_before_ingame(targetChannelID);
    }

    if (!ts3_request_client_move(localID, targetChannelID, password)) {
        if (toIngame) {
            nick_restore_in_hub(); /* move failed — roll back pre-anonymize */
        }
        return 0;
    }

    g_moveInFlight = 1;
    g_moveInFlightSince = now;
    g_lastMoveMs = now;
    log_write("CHAN: move requested -> channel %llu (%s)",
        (unsigned long long)targetChannelID,
        toIngame ? "ingame"
        : chan_is_hub_channel(targetChannelID) ? "hub" : "?");
    return 1;
}

/* ---- 8.4 playback gate from current channel ------------------------------------- */

static void chan_update_audio_mode(uint64 ownChannelID) {
    if (chan_is_ignored_channel(ownChannelID)) {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }
    else if (chan_is_ingame_channel(ownChannelID)) {
        ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);
    }
    else if (chan_is_hub_channel(ownChannelID)) {
        ts3_audio_set_mode(TS3_AUDIO_MUTE);
    }
    else {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }
}

/* Distance-based muting: local toggle, server profile can force it on. */
static int chan_distance_muting_enabled(void) {
    if (g_config.enableDistanceMuting) {
        return 1;
    }
    HubSettings hub;
    return server_profile_get(&hub) && hub.forceDistanceMuting;
}

static int chan_auto_move_enabled(void) {
    if (g_config.enableAutomaticChannelChange) {
        return 1;
    }
    return server_profile_force_auto_channel();
}

void chan_signal_position_update(void) {
    InterlockedExchange(&g_positionUpdatePending, 1);
    ts3_request_wakeup();
}

/* ---- tick driver ------------------------------------------------------------------ */

void chan_tick(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    InterlockedExchange(&g_positionUpdatePending, 0);

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTickMs < CHAN_TICK_MIN_MS) {
        return;
    }
    g_lastTickMs = now;

    /* (Re-)resolve channel IDs, throttled while missing. Needed for auto-move
       and for the hub hard-mute playback gate. */
    if (g_hubChannelID == 0 || g_ingameChannelID == 0) {
        if (now - g_lastFindMs >= CHAN_FIND_RETRY_MS) {
            g_lastFindMs = now;
            chan_find_hub_and_ingame();
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

    /* Playback gate — independent of auto-move. */
    if (chan_distance_muting_enabled()) {
        chan_update_audio_mode(ownChannel);
    }
    else {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }

    if (!chan_auto_move_enabled()) {
        return;
    }

    /* Event/Plot channels: user joined deliberately — no auto-move either way. */
    if (chan_is_ignored_channel(ownChannel)) {
        return;
    }

    int moveWanted = 0;
    const int wantIngame = chan_should_be_ingame();

    if (wantIngame) {
        if (!chan_is_ingame_channel(ownChannel)) {
            if (g_ingameChannelID == 0) {
                chan_find_hub_and_ingame();
            }
            if (g_ingameChannelID != 0) {
                moveWanted = 1;
                if (!chan_request_move(g_ingameChannelID)
                    && now - g_lastMoveSkipLogMs >= 5000) {
                    g_lastMoveSkipLogMs = now;
                    log_write("CHAN: auto-move to ingame blocked (in-flight=%d cooldown=%llums coords=%d profile=%d)",
                        g_moveInFlight,
                        (unsigned long long)(now - g_lastMoveMs),
                        pos_coordinates_valid(),
                        server_profile_is_active());
                }
            }
            else if (now - g_lastMoveSkipLogMs >= 5000) {
                g_lastMoveSkipLogMs = now;
                log_write("CHAN: auto-move to ingame wanted but ingame channel not found");
            }
        }
        else {
            /* Already ingame (relog / manual join) but maybe with real name. */
            nick_anonymize_before_ingame(ownChannel);
        }
    }
    else if (!chan_is_hub_channel(ownChannel)) {
        /* No game coords (or Conan closed): lobby channel. Old-plugin rule —
           move anyone who is NOT in hub (root on join, ingame after quit, etc.). */
        if (g_hubChannelID == 0) {
            chan_find_hub_and_ingame();
        }
        if (g_hubChannelID != 0) {
            moveWanted = 1;
            if (!chan_request_move(g_hubChannelID)
                && now - g_lastMoveSkipLogMs >= 5000) {
                g_lastMoveSkipLogMs = now;
                log_write("CHAN: auto-move to hub blocked (from ch=%llu in-flight=%d cooldown=%llums)",
                    (unsigned long long)ownChannel,
                    g_moveInFlight,
                    (unsigned long long)(now - g_lastMoveMs));
            }
        }
        else if (now - g_lastMoveSkipLogMs >= 5000) {
            g_lastMoveSkipLogMs = now;
            log_write("CHAN: auto-move to hub wanted (own=%llu) but hub channel not found",
                (unsigned long long)ownChannel);
        }
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
    g_lastTickMs = 0; /* bypass tick throttle — placement must re-check now */
    chan_update_audio_mode(newChannelID);

    if (chan_is_ingame_channel(newChannelID)) {
        g_ingameArrivalMs = GetTickCount64();
    }
    else {
        g_ingameArrivalMs = 0;
    }

    /* Back outside the ingame channel -> real name again (Phase 12). */
    if (!chan_is_ingame_channel(newChannelID)) {
        nick_restore_in_hub();
    }

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
    InterlockedExchange(&g_positionUpdatePending, 0);
    g_lastMoveSkipLogMs = 0;
    g_ingameArrivalMs = 0;
    memset(g_ignoreCache, 0, sizeof(g_ignoreCache));
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
}

int chan_has_pending_work(void) {
    return g_moveInFlight != 0;
}

uint64 chan_get_hub_channel_id(void) {
    return g_hubChannelID;
}

uint64 chan_get_ingame_channel_id(void) {
    return g_ingameChannelID;
}
