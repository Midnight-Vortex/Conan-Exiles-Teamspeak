#include "ts/channel/channel_manage.h"
#include "ts/channel/chan_move_backoff.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/mod_file/pos_file.h"
#include "ts/nick/nick_anonymize.h"
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
#define CHAN_MAX_CHANNELS         1024  /* scan cap (was 128, 086); static buffer below */
#define CHAN_IGNORE_CACHE_SIZE    16
#define CHAN_IGNORE_CACHE_TTL_MS  30000

/* All state is TS callback thread only — no locks needed. The three
   volatile flags are Interlocked anyway: chan_has_pending_work() and
   chan_signal_position_update() may be reached from other threads. */
static uint64 g_hubChannelID = 0;
static uint64 g_ingameChannelID = 0;
static ULONGLONG g_lastFindMs = 0;

/* Channel list scan buffer (8 KB). Static — too big for the callback
   thread's stack budget; chan_find_hub_and_ingame is never re-entered. */
static uint64 g_channelScan[CHAN_MAX_CHANNELS];

static int g_moveInFlight = 0;
static ULONGLONG g_moveInFlightSince = 0;
static uint64 g_moveTarget = 0;       /* target of the move in flight */
static int g_moveToIngame = 0;        /* that move pre-anonymized the nickname */
static ULONGLONG g_lastMoveMs = 0;
static ULONGLONG g_lastTickMs = 0;
static ChanMoveBackoff g_moveBackoff; /* 086: retry schedule after rejected moves */
static volatile long g_positionUpdatePending = 0;
static volatile long g_channelListDirty = 0; /* 086: re-resolve hub/ingame on next tick */
static volatile long g_moveWanted = 0;       /* 086: a wanted move waits for a short retry */
static ULONGLONG g_lastMoveSkipLogMs = 0;
static ULONGLONG g_ingameArrivalMs = 0;

static struct {
    uint64_t channelID;
    int ignored;
    ULONGLONG cachedAt;
} g_ignoreCache[CHAN_IGNORE_CACHE_SIZE];

/* SaltyChat-style: channels whose name contains "event" or "support" anywhere
   (Event, [Event] Halle, Eventhalle, Support, Supportraum — case-insensitive,
   079) or "plot" as a whole word (Plot Nord, not Plotter) suspend auto-move. */
static int chan_name_has_ignore_keyword(const char* name) {
    static const struct {
        const char* word;
        int wholeWord;
    } keywords[] = { { "event", 0 }, { "support", 0 }, { "plot", 1 } };

    if (!name || !name[0]) {
        return 0;
    }

    for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        const char* kw = keywords[k].word;
        const size_t kwLen = strlen(kw);

        for (const char* p = name; *p; p++) {
            if (_strnicmp(p, kw, kwLen) != 0) {
                continue;
            }
            if (!keywords[k].wholeWord) {
                return 1;
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

    const int count = ts3_get_channel_list(g_channelScan, CHAN_MAX_CHANNELS);
    if (count <= 0) {
        /* List unreadable (API error): keep the IDs we already know instead of
           dropping the proximity gate because of one failed call. */
        return g_hubChannelID != 0 && g_ingameChannelID != 0;
    }

    uint64 hub = 0;
    uint64 ingame = 0;
    for (int i = 0; i < count; i++) {
        char name[128] = "";
        if (!ts3_get_channel_name(g_channelScan[i], name, sizeof(name))) {
            continue;
        }
        if (hub == 0 && _stricmp(name, "hub") == 0) {
            hub = g_channelScan[i];
        }
        else if (ingame == 0 && _stricmp(name, "ingame") == 0) {
            ingame = g_channelScan[i];
        }
    }
    if (count >= CHAN_MAX_CHANNELS) {
        /* Truncated list: a channel that is missing may just lie beyond the
           cap — keep the cached ID rather than forgetting a working one. */
        if (hub == 0) {
            hub = g_hubChannelID;
        }
        if (ingame == 0) {
            ingame = g_ingameChannelID;
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

/* ---- 8.3 move request (in-flight + cooldown + failure backoff) ------------------ */

/* The move in flight failed (server rejected it, or no answer within
   CHAN_MOVE_INFLIGHT_MAX_MS). Clears the in-flight state, gives the real
   nickname back when this move pre-anonymized it, and starts the retry
   backoff for that target. Logs ONE line per failed attempt. */
static void chan_move_attempt_failed(ULONGLONG now, unsigned int error, int timedOut) {
    const uint64 target = g_moveTarget;
    const int toIngame = g_moveToIngame;

    g_moveInFlight = 0;
    g_moveInFlightSince = 0;
    g_moveTarget = 0;
    g_moveToIngame = 0;

    const uint64_t delay = chan_backoff_on_failure(&g_moveBackoff, target, now);

    if (toIngame) {
        nick_restore_in_hub(); /* we renamed in the hub for this move — undo it */
    }

    if (timedOut) {
        log_write("CHAN: move to channel %llu got no answer within %u ms - next attempt in %llu s",
            (unsigned long long)target, (unsigned)CHAN_MOVE_INFLIGHT_MAX_MS,
            (unsigned long long)(delay / 1000u));
    }
    else {
        log_write("CHAN: move to channel %llu rejected by the server (error 0x%04x) - "
            "next attempt in %llu s",
            (unsigned long long)target, error, (unsigned long long)(delay / 1000u));
    }
}

/* retrySoon (may be NULL): set to 1 when a refused request only has to wait a
   short, self-resolving time (in-flight, cooldown, backoff up to 10 s) — the
   caller then keeps the CEDRAIN loop alive. Waiting for the Root profile, for
   the mod's password or after a failed send is event driven: 0. */
static int chan_request_move_impl(uint64 targetChannelID, int* retrySoon) {
    if (retrySoon) {
        *retrySoon = 0;
    }
    if (!ts3_thread_is_callback() || !ts3_is_connected() || targetChannelID == 0) {
        return 0;
    }

    const ULONGLONG now = GetTickCount64();

    if (g_moveInFlight) {
        if (now - g_moveInFlightSince < CHAN_MOVE_INFLIGHT_MAX_MS) {
            if (retrySoon) {
                *retrySoon = 1;
            }
            return 0; /* never two moves in parallel (old plugin bug) */
        }
        chan_move_attempt_failed(now, 0, 1);
    }
    if (now - g_lastMoveMs < CHAN_MOVE_COOLDOWN_MS) {
        if (retrySoon) {
            *retrySoon = 1;
        }
        return 0;
    }
    if (chan_backoff_blocks(&g_moveBackoff, targetChannelID, now)) {
        if (retrySoon) {
            *retrySoon = chan_backoff_keeps_loop(&g_moveBackoff);
        }
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

    /* 077: the game mod's password (Conan server, "dc tsvm") wins over the
       root channel's IngameChannelPassword. A protected channel without any
       password waits for the mod instead of provoking a TS error.
       086: the secret is only sent to a channel that is really protected
       (1); an unprotected channel (0) gets an empty password, an unknown
       state (-1) keeps the old behaviour. */
    char password[HUB_PASSWORD_LEN];
    password[0] = '\0';
    if (toIngame) {
        const int hasPassword = ts3_channel_has_password(targetChannelID);
        if (hasPassword != 0) {
            const int pwSource = server_profile_copy_ingame_password(password, (int)sizeof(password));
            if (pwSource == 0 && hasPassword == 1) {
                static ULONGLONG lastPwWaitLog = 0;
                if (lastPwWaitLog == 0 || now - lastPwWaitLog >= 60000) {
                    lastPwWaitLog = now;
                    log_write("CHAN: ingame channel is password protected - waiting for the password "
                        "from the game mod (server: dc tsvm <password>)");
                }
                return 0;
            }
        }
    }

    /* Rename while still in the hub so ingame clients never see the
       "realName -> digits" rename event (Phase 12). */
    if (toIngame) {
        nick_anonymize_before_ingame(targetChannelID);
    }

    /* Cooldown starts with the attempt, whatever the result: a synchronous
       failure used to be retried on every tick (086). */
    g_lastMoveMs = now;
    const int moveSent = ts3_request_client_move(localID, targetChannelID, password);
    SecureZeroMemory(password, sizeof(password));
    if (!moveSent) {
        if (toIngame) {
            nick_restore_in_hub(); /* move failed — roll back pre-anonymize */
        }
        return 0;
    }

    g_moveInFlight = 1;
    g_moveInFlightSince = now;
    g_moveTarget = targetChannelID;
    g_moveToIngame = toIngame;
    log_write("CHAN: move requested -> channel %llu (%s)",
        (unsigned long long)targetChannelID,
        toIngame ? "ingame"
        : chan_is_hub_channel(targetChannelID) ? "hub" : "?");
    if (retrySoon) {
        *retrySoon = 1; /* keep ticking until the own-move event (or an error) arrives */
    }
    return 1;
}

int chan_request_move(uint64 targetChannelID) {
    return chan_request_move_impl(targetChannelID, NULL);
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

/* Distance-based muting: local toggle, server profile can force it on.
   086: scalar getter instead of copying the whole HubSettings every tick. */
static int chan_distance_muting_enabled(void) {
    if (g_config.enableDistanceMuting) {
        return 1;
    }
    return server_profile_force_distance_muting();
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

/* ---- 079 ForceSpeakersOn ---------------------------------------------------------- */

#define CHAN_MAX_CHANNEL_CLIENTS 512

static int g_deafPollActive = 0;  /* deaf flags currently maintained */
static int g_speakerHintShown = 0; /* chat hint once per muted episode */

/* Others in the ingame channel with muted speakers are silenced for us (their
   plugin moves them out; this covers old plugins and the moment before). */
static void chan_update_deaf_speakers(uint64 ownChannel, anyID localID, int force) {
    if (!force || !chan_is_ingame_channel(ownChannel)) {
        if (g_deafPollActive) {
            ts3_audio_clear_client_deaf();
            g_deafPollActive = 0;
        }
        return;
    }
    anyID clients[CHAN_MAX_CHANNEL_CLIENTS];
    const int count = ts3_get_channel_client_list(ownChannel, clients, CHAN_MAX_CHANNEL_CLIENTS);
    if (!g_deafPollActive) {
        ts3_audio_clear_client_deaf();
        g_deafPollActive = 1;
    }
    for (int i = 0; i < count; i++) {
        if (clients[i] == localID) {
            continue;
        }
        const int muted = ts3_client_speakers_muted(clients[i]);
        if (muted >= 0) {
            ts3_audio_set_client_deaf(clients[i], muted);
        }
    }
}

/* ---- tick driver ------------------------------------------------------------------ */

/* One placement pass (throttle already passed). Returns 1 when a move is wanted
   AND is only waiting for a short, self-resolving time (cooldown, in-flight,
   backoff up to 10 s): the caller then keeps the CEDRAIN loop alive so the
   retry happens without any TS event. 0 for "nothing to move" and for waits
   that need an event (profile, password, long backoff, failed send). */
static int chan_tick_run(ULONGLONG now) {
    /* An answer that never came (no own-move event, no error): count it as a
       failure even when no move is wanted any more, so a stale in-flight flag
       can never keep the CEDRAIN loop alive forever (086). */
    if (g_moveInFlight && now - g_moveInFlightSince >= CHAN_MOVE_INFLIGHT_MAX_MS) {
        chan_move_attempt_failed(now, 0, 1);
    }

    /* (Re-)resolve channel IDs. Needed for auto-move and for the hub hard-mute
       playback gate. Throttled while missing (CHAN_FIND_RETRY_MS); a channel
       list change (C3) forces exactly one immediate rescan. The cached IDs
       stay in use until the rescan replaced them (086). */
    const int channelListChanged = InterlockedExchange(&g_channelListDirty, 0) != 0;
    if (channelListChanged
        || ((g_hubChannelID == 0 || g_ingameChannelID == 0)
            && now - g_lastFindMs >= CHAN_FIND_RETRY_MS)) {
        g_lastFindMs = now;
        chan_find_hub_and_ingame();
    }

    const anyID localID = ts3_get_local_client_id();
    if (localID == 0) {
        return 0;
    }
    const uint64 ownChannel = ts3_get_channel_of_client(localID);
    if (ownChannel == 0) {
        return 0;
    }

    /* Playback gate — independent of auto-move. */
    if (chan_distance_muting_enabled()) {
        chan_update_audio_mode(ownChannel);
    }
    else {
        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    }

    /* 079 ForceSpeakersOn: whoever mutes his speakers in TS leaves the ingame
       channel (also without auto-move) and comes back once they are on again. */
    const int forceSpeakers = server_profile_force_speakers_on();
    chan_update_deaf_speakers(ownChannel, localID, forceSpeakers);
    const int ownSpeakersMuted = forceSpeakers && ts3_client_speakers_muted(localID) == 1;
    if (!ownSpeakersMuted) {
        g_speakerHintShown = 0;
    }
    else if (chan_is_ingame_channel(ownChannel)) {
        if (!g_speakerHintShown) {
            g_speakerHintShown = 1;
            log_write("CHAN: speakers muted in the ingame channel (ForceSpeakersOn) - moving to hub");
            ts3_print_to_chat("[Conan Exiles] Your speakers are muted - moved to the hub. "
                "Unmute your speakers to get back ingame.");
        }
        if (g_hubChannelID != 0) {
            int retrySoon = 0;
            chan_request_move_impl(g_hubChannelID, &retrySoon);
            return retrySoon;
        }
        return 0;
    }

    if (!chan_auto_move_enabled()) {
        return 0;
    }

    /* Event/Plot channels: user joined deliberately — no auto-move either way. */
    if (chan_is_ignored_channel(ownChannel)) {
        return 0;
    }

    int moveWanted = 0;
    int retrySoon = 0;
    const int wantIngame = chan_should_be_ingame() && !ownSpeakersMuted;

    if (wantIngame) {
        if (!chan_is_ingame_channel(ownChannel)) {
            if (g_ingameChannelID != 0) {
                moveWanted = 1;
                if (!chan_request_move_impl(g_ingameChannelID, &retrySoon)
                    && !chan_backoff_blocks(&g_moveBackoff, g_ingameChannelID, now)
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
        if (g_hubChannelID != 0) {
            moveWanted = 1;
            if (!chan_request_move_impl(g_hubChannelID, &retrySoon)
                && !chan_backoff_blocks(&g_moveBackoff, g_hubChannelID, now)
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

    return moveWanted && retrySoon;
}

void chan_tick(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    /* Throttle FIRST, clear the position flag only afterwards (086): a
       throttled tick used to swallow the "position changed" edge. The flag
       keeps chan_has_pending_work() true, so the next drain ticks again. */
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTickMs < CHAN_TICK_MIN_MS) {
        return;
    }
    g_lastTickMs = now;
    InterlockedExchange(&g_positionUpdatePending, 0);

    /* A blocked/pending move must be retried even when no TS event arrives:
       keep the CEDRAIN wakeup loop alive (rate-limited inside the adapter)
       until the own-move event confirms the new channel, or the server
       rejects it (chan_on_move_failed -> backoff). The CEDRAIN handler only
       ticks while chan_has_pending_work() is true, hence the flag. */
    const int wakeLoop = chan_tick_run(now);
    InterlockedExchange(&g_moveWanted, wakeLoop ? 1 : 0);
    if (wakeLoop) {
        ts3_request_wakeup();
    }
}

void chan_leave_ingame_on_plugin_disable(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }
    if (g_hubChannelID == 0 || g_ingameChannelID == 0) {
        chan_find_hub_and_ingame();
    }
    if (g_hubChannelID == 0) {
        log_write("CHAN: plugin disable — no hub channel, stay put");
        return;
    }

    const anyID localID = ts3_get_local_client_id();
    if (localID == 0) {
        return;
    }
    const uint64 ownChannel = ts3_get_channel_of_client(localID);
    if (ownChannel == 0 || !chan_is_ingame_channel(ownChannel)) {
        return;
    }

    /* Restore the real name now — onClientMoveEvent will not run after unload. */
    nick_restore_in_hub();

    /* Last chance: ignore in-flight/cooldown/backoff so the request goes out. */
    g_moveInFlight = 0;
    g_lastMoveMs = 0;
    if (ts3_request_client_move(localID, g_hubChannelID, "")) {
        const ULONGLONG now = GetTickCount64();
        g_moveInFlight = 1;
        g_moveInFlightSince = now;
        g_moveTarget = g_hubChannelID;
        g_moveToIngame = 0;
        g_lastMoveMs = now;
        log_write("CHAN: plugin disable — move ingame -> hub requested");
    }
    else {
        log_write("CHAN: plugin disable — hub move failed");
    }
}

/* ---- events / reset ---------------------------------------------------------------- */

void chan_on_own_move(uint64 newChannelID) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    g_moveInFlight = 0;
    g_moveInFlightSince = 0;
    g_moveTarget = 0;
    g_moveToIngame = 0;
    chan_backoff_reset(&g_moveBackoff); /* any own channel change ends the failure series */
    InterlockedExchange(&g_moveWanted, 0); /* the tick right after this event re-decides */
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

void chan_on_move_failed(unsigned int error) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    if (!g_moveInFlight) {
        return; /* nothing of ours in flight (already timed out / answered) */
    }
    chan_move_attempt_failed(GetTickCount64(), error, 0);
    g_lastTickMs = 0; /* let the next tick schedule the retry / keep-alive */
    ts3_request_wakeup();
}

void chan_on_channel_list_changed(uint64 channelID) {
    (void)channelID; /* which channel does not matter: one coalesced rescan */
    if (!ts3_thread_is_callback()) {
        return;
    }
    InterlockedExchange(&g_channelListDirty, 1);
    ts3_request_wakeup(); /* the CEDRAIN drain ticks even without other work */
}

void chan_reset(void) {
    g_hubChannelID = 0;
    g_ingameChannelID = 0;
    g_lastFindMs = 0;
    g_moveInFlight = 0;
    g_moveInFlightSince = 0;
    g_moveTarget = 0;
    g_moveToIngame = 0;
    chan_backoff_reset(&g_moveBackoff);
    g_lastMoveMs = 0;
    g_lastTickMs = 0;
    InterlockedExchange(&g_positionUpdatePending, 0);
    InterlockedExchange(&g_channelListDirty, 0);
    InterlockedExchange(&g_moveWanted, 0);
    g_lastMoveSkipLogMs = 0;
    g_ingameArrivalMs = 0;
    memset(g_ignoreCache, 0, sizeof(g_ignoreCache));
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    ts3_audio_clear_client_deaf();
    g_deafPollActive = 0;
    g_speakerHintShown = 0;
}

int chan_has_pending_work(void) {
    return g_moveInFlight != 0
        || InterlockedCompareExchange(&g_positionUpdatePending, 0, 0) != 0
        || InterlockedCompareExchange(&g_channelListDirty, 0, 0) != 0
        || InterlockedCompareExchange(&g_moveWanted, 0, 0) != 0;
}

uint64 chan_get_hub_channel_id(void) {
    return g_hubChannelID;
}

uint64 chan_get_ingame_channel_id(void) {
    return g_ingameChannelID;
}
