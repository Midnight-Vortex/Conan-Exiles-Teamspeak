/* Host test for src/ts/channel/channel_manage.c (086 review fixes).

   channel_manage.c is #included here (not linked) so the test can run it on
   Linux against a FAKE TeamSpeak world: every adapter / audio / profile / nick
   / pos / log function it calls is a stub below, time is moved by the Win32
   shim (win32_shim_advance_ms). Build/run: bash tests/run_tests.sh.
   Exit code 0 = all checks passed.

   Covered findings:
   1  rejected move: no more blind re-sends, nickname restored, backoff 5/10/30/60 s,
      one log line per failed attempt, backoff reset by an own channel change;
      the in-flight timeout is treated the same way
   2  a synchronous send failure also starts the cooldown
   3  channel list change (C3): one coalesced rescan, cached IDs survive a failed scan
   4  channel cap 1024 (Hub/Ingame behind position 128 are found)
   5  no unthrottled scans on servers without Hub/Ingame
   10 the ingame password is only sent to a channel that really has one
   11 position flag survives a throttled tick; a wanted move keeps the CEDRAIN loop
      alive for short waits (and does not for long ones) */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define SecureZeroMemory(p, n) memset((p), 0, (n))

#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/nick/nick_anonymize.h"
#include "core/mod_file/pos_file.h"
#include "core/config/config.h"
#include "core/util/log.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- fake world --------------------------------------------------------------- */

#define FAKE_MAX_CHANNELS 2048

typedef struct FakeChannel {
    uint64 id;
    char name[64];
    int hasPassword; /* 1 / 0 / -1 as ts3_channel_has_password reports it */
} FakeChannel;

static FakeChannel g_channels[FAKE_MAX_CHANNELS];
static int g_channelCount = 0;

static int g_connected = 1;
static anyID g_localId = 10;
static uint64 g_ownChannel = 0;
static int g_listFails = 0;          /* 1 = ts3_get_channel_list returns 0 (API error) */
static int g_listCalls = 0;
static int g_nameCalls = 0;

static int g_posValid = 0;
static int g_profileActive = 1;
static int g_pwSource = 0;           /* what server_profile_copy_ingame_password reports */
static char g_pwText[HUB_PASSWORD_LEN] = "";
static int g_forceMuting = 0;

static int g_moveSendResult = 1;     /* ts3_request_client_move return value */
static int g_moveCalls = 0;
static uint64 g_lastMoveTarget = 0;
static char g_lastMovePassword[HUB_PASSWORD_LEN] = "";

static int g_anonymizeCalls = 0;
static int g_restoreCalls = 0;
static int g_wakeups = 0;
static Ts3AudioMode g_audioMode = TS3_AUDIO_PASSTHROUGH;

#define LOG_LINES 512
static char g_log[LOG_LINES][256];
static int g_logCount = 0;

PluginConfig g_config;

static void add_channel(uint64 id, const char* name, int hasPassword) {
    g_channels[g_channelCount].id = id;
    snprintf(g_channels[g_channelCount].name, sizeof(g_channels[g_channelCount].name), "%s", name);
    g_channels[g_channelCount].hasPassword = hasPassword;
    g_channelCount++;
}

static const FakeChannel* find_channel(uint64 id) {
    for (int i = 0; i < g_channelCount; i++) {
        if (g_channels[i].id == id) {
            return &g_channels[i];
        }
    }
    return NULL;
}

static int log_count(const char* needle) {
    int n = 0;
    for (int i = 0; i < g_logCount && i < LOG_LINES; i++) {
        if (strstr(g_log[i], needle)) {
            n++;
        }
    }
    return n;
}

/* ---- stubs: adapter ------------------------------------------------------------- */

int ts3_thread_is_callback(void) { return 1; }
int ts3_is_connected(void) { return g_connected; }
anyID ts3_get_local_client_id(void) { return g_localId; }
uint64 ts3_get_channel_of_client(anyID clientID) { (void)clientID; return g_ownChannel; }

int ts3_get_channel_list(uint64* out, int maxChannels) {
    g_listCalls++;
    if (g_listFails) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < g_channelCount && n < maxChannels; i++) { /* like the real adapter: up to max */
        out[n++] = g_channels[i].id;
    }
    return n;
}

int ts3_get_channel_name(uint64 channelID, char* out, int outLen) {
    g_nameCalls++;
    const FakeChannel* c = find_channel(channelID);
    if (!c) {
        return 0;
    }
    snprintf(out, (size_t)outLen, "%s", c->name);
    return 1;
}

int ts3_channel_has_password(uint64 channelID) {
    const FakeChannel* c = find_channel(channelID);
    return c ? c->hasPassword : -1;
}

int ts3_client_speakers_muted(anyID clientID) { (void)clientID; return 0; }
int ts3_get_channel_client_list(uint64 channelID, anyID* out, int maxClients) {
    (void)channelID; (void)out; (void)maxClients; return 0;
}
void ts3_print_to_chat(const char* message) { (void)message; }

int ts3_request_client_move(anyID clientID, uint64 channelID, const char* password) {
    (void)clientID;
    g_moveCalls++;
    g_lastMoveTarget = channelID;
    snprintf(g_lastMovePassword, sizeof(g_lastMovePassword), "%s", password ? password : "<null>");
    return g_moveSendResult;
}

void ts3_request_wakeup(void) { g_wakeups++; }

/* ---- stubs: audio / pos / profile / nick / log -------------------------------------- */

void ts3_audio_set_mode(Ts3AudioMode mode) { g_audioMode = mode; }
void ts3_audio_clear_client_deaf(void) {}
void ts3_audio_set_client_deaf(anyID clientID, int deaf) { (void)clientID; (void)deaf; }

int pos_coordinates_valid(void) { return g_posValid; }

int server_profile_is_active(void) { return g_profileActive; }
int server_profile_force_distance_muting(void) { return g_forceMuting; }
int server_profile_force_speakers_on(void) { return 0; }
int server_profile_force_auto_channel(void) { return 0; }
int server_profile_copy_ingame_password(char* out, int outLen) {
    snprintf(out, (size_t)outLen, "%s", g_pwSource ? g_pwText : "");
    return g_pwSource;
}

void nick_anonymize_before_ingame(uint64 ingameChannelID) { (void)ingameChannelID; g_anonymizeCalls++; }
void nick_restore_in_hub(void) { g_restoreCalls++; }

void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log[g_logCount % LOG_LINES], sizeof(g_log[0]), fmt, ap);
    va_end(ap);
    g_logCount++;
}
void log_debug(const char* fmt, ...) { (void)fmt; }

/* ---- code under test -------------------------------------------------------------------- */

#include "ts/channel/channel_manage.c"

/* ---- helpers ---------------------------------------------------------------------------- */

#define ID_ROOT   1
#define ID_HUB    2
#define ID_INGAME 3
#define ID_LOBBY  4

static void reset_world(void) {
    memset(g_channels, 0, sizeof(g_channels));
    g_channelCount = 0;
    add_channel(ID_ROOT, "Root", 0);
    add_channel(ID_HUB, "Hub", 0);
    add_channel(ID_INGAME, "Ingame", 0);
    add_channel(ID_LOBBY, "Lobby", 0);

    g_connected = 1;
    g_localId = 10;
    g_ownChannel = ID_LOBBY;
    g_listFails = 0;
    g_listCalls = 0;
    g_nameCalls = 0;
    g_posValid = 1;
    g_profileActive = 1;
    g_pwSource = 0;
    g_pwText[0] = '\0';
    g_forceMuting = 0;
    g_moveSendResult = 1;
    g_moveCalls = 0;
    g_lastMoveTarget = 0;
    g_lastMovePassword[0] = '\0';
    g_anonymizeCalls = 0;
    g_restoreCalls = 0;
    g_wakeups = 0;
    g_audioMode = TS3_AUDIO_PASSTHROUGH;
    g_logCount = 0;

    memset(&g_config, 0, sizeof(g_config));
    g_config.enableAutomaticChannelChange = 1;
    g_config.enableDistanceMuting = 1;

    chan_reset();
    win32_shim_advance_ms(100000); /* far past every throttle window */
}

/* One tick after letting `ms` pass (always > the 500 ms tick throttle). */
static void tick_after(unsigned ms) {
    win32_shim_advance_ms(ms);
    chan_tick();
}

/* ---- tests -------------------------------------------------------------------------------- */

static void test_rejected_move_backoff(void) {
    printf("[1] rejected move: restore nick, backoff 5/10/30 s, one log line each\n");
    reset_world();
    g_posValid = 1;

    tick_after(1000);
    CHECK(g_moveCalls == 1 && g_lastMoveTarget == ID_INGAME, "tick sends the first move to ingame");
    CHECK(g_anonymizeCalls == 1, "nickname pre-anonymized for the move");
    CHECK(chan_has_pending_work(), "move in flight keeps the CEDRAIN loop alive");

    tick_after(3000);
    CHECK(g_moveCalls == 1, "no second move while the first is in flight");

    chan_on_move_failed(0x030d);
    CHECK(g_restoreCalls == 1, "server rejection restores the real nickname");
    CHECK(log_count("rejected by the server") == 1, "exactly one log line for the failure");
    CHECK(log_count("next attempt in 5 s") == 1, "log names the 5 s wait");

    tick_after(2500);
    CHECK(g_moveCalls == 1, "backoff 5 s: no re-send 2.5 s after the failure");
    CHECK(chan_has_pending_work(), "short backoff keeps the loop alive");
    tick_after(1500);
    CHECK(g_moveCalls == 1, "backoff 5 s: still no re-send 4 s after the failure");
    tick_after(2000);
    CHECK(g_moveCalls == 2, "re-send once the 5 s are over");

    chan_on_move_failed(0x030d);
    CHECK(log_count("next attempt in 10 s") == 1, "second failure: 10 s wait");
    tick_after(9000);
    CHECK(g_moveCalls == 2, "10 s backoff holds at 9 s");
    tick_after(1500);
    CHECK(g_moveCalls == 3, "re-send after 10 s");

    chan_on_move_failed(0x030d);
    CHECK(log_count("next attempt in 30 s") == 1, "third failure: 30 s wait");
    CHECK(g_restoreCalls == 3, "nickname restored after every rejection");
    tick_after(3000); /* cooldown (2 s) is over, only the long backoff is left */
    CHECK(!chan_has_pending_work(), "30 s wait does not keep the CEDRAIN loop alive");
    tick_after(25000);
    CHECK(g_moveCalls == 3, "30 s backoff holds at 28 s");
    tick_after(3000);
    CHECK(g_moveCalls == 4, "re-send after 30 s");

    /* Own channel change (success) resets the schedule. */
    chan_on_own_move(ID_INGAME);
    g_ownChannel = ID_LOBBY; /* user left again; still wants ingame */
    tick_after(3000);
    CHECK(g_moveCalls == 5, "after an own move the next attempt is immediate");
    chan_on_move_failed(0x030d);
    CHECK(log_count("next attempt in 5 s") >= 2, "schedule restarted at 5 s after the reset");
}

static void test_failure_ignored_without_flight(void) {
    printf("[2] chan_on_move_failed without a move in flight is a no-op\n");
    reset_world();
    chan_on_move_failed(0x0a08);
    CHECK(g_restoreCalls == 0, "no nickname restore");
    CHECK(log_count("rejected") == 0, "no log line");
    tick_after(1000);
    CHECK(g_moveCalls == 1, "next tick still moves normally (no phantom backoff)");
}

static void test_inflight_timeout(void) {
    printf("[3] in-flight timeout = failure (restore nick + backoff)\n");
    reset_world();
    tick_after(1000);
    CHECK(g_moveCalls == 1, "move sent");
    tick_after(5500);
    CHECK(g_moveCalls == 1, "timeout does not re-send immediately");
    CHECK(g_restoreCalls == 1, "timeout restores the nickname");
    CHECK(log_count("no answer within") == 1, "one log line for the timeout");
    tick_after(2000);
    CHECK(g_moveCalls == 1, "still inside the 5 s backoff");
    tick_after(4000);
    CHECK(g_moveCalls == 2, "re-send after the backoff");
}

static void test_timeout_without_wanted_move(void) {
    printf("[3b] a stale in-flight flag cannot keep the CEDRAIN loop alive\n");
    reset_world();
    tick_after(1000);
    CHECK(g_moveCalls == 1 && chan_has_pending_work(), "move in flight");
    g_config.enableAutomaticChannelChange = 0; /* no move wanted any more */
    tick_after(2000);
    CHECK(chan_has_pending_work(), "still waiting for the answer after 2 s");
    tick_after(4000);
    CHECK(!chan_has_pending_work(), "after the timeout nothing is pending any more");
    CHECK(g_restoreCalls == 1, "timeout restored the nickname");
}

static void test_sync_failure_cooldown(void) {
    printf("[4] synchronous send failure starts the cooldown\n");
    reset_world();
    g_moveSendResult = 0;
    tick_after(1000);
    CHECK(g_moveCalls == 1, "first attempt");
    CHECK(g_restoreCalls == 1, "failed send rolls the nickname back");
    tick_after(600);
    CHECK(g_moveCalls == 1, "0.6 s later: cooldown blocks the retry");
    CHECK(chan_has_pending_work(), "cooldown-blocked move keeps the loop alive");
    tick_after(1000);
    CHECK(g_moveCalls == 1, "1.6 s later: still cooling down");
    tick_after(700);
    CHECK(g_moveCalls == 2, "2.3 s later: next attempt");
}

static void test_password_rule(void) {
    printf("[5] ingame password only goes to a protected channel\n");
    reset_world();
    g_pwSource = 2;
    snprintf(g_pwText, sizeof(g_pwText), "secret");

    g_channels[2].hasPassword = 0;
    tick_after(1000);
    CHECK(g_moveCalls == 1 && strcmp(g_lastMovePassword, "") == 0,
        "unprotected ingame channel: empty password");

    reset_world();
    g_pwSource = 2;
    snprintf(g_pwText, sizeof(g_pwText), "secret");
    g_channels[2].hasPassword = 1;
    tick_after(1000);
    CHECK(g_moveCalls == 1 && strcmp(g_lastMovePassword, "secret") == 0,
        "protected channel: password sent");

    reset_world();
    g_pwSource = 2;
    snprintf(g_pwText, sizeof(g_pwText), "secret");
    g_channels[2].hasPassword = -1;
    tick_after(1000);
    CHECK(g_moveCalls == 1 && strcmp(g_lastMovePassword, "secret") == 0,
        "unknown protection state: old behaviour (password sent)");

    reset_world();
    g_pwSource = 0;
    g_channels[2].hasPassword = 1;
    tick_after(1000);
    CHECK(g_moveCalls == 0, "protected channel without any password: wait for the mod");
    CHECK(!chan_has_pending_work(), "waiting for the mod does not spin the CEDRAIN loop");

    reset_world();
    g_pwSource = 1;
    snprintf(g_pwText, sizeof(g_pwText), "legacy");
    g_channels[2].hasPassword = 0;
    tick_after(1000);
    CHECK(g_moveCalls == 1 && strcmp(g_lastMovePassword, "") == 0,
        "legacy root password is not sent to an unprotected channel either");
}

static void test_channel_list_dirty(void) {
    printf("[6] channel list change: one coalesced rescan, cache survives errors\n");
    reset_world();
    g_config.enableAutomaticChannelChange = 0; /* audio gate only, no moves */
    g_posValid = 1;
    g_ownChannel = ID_INGAME;
    tick_after(1000);
    CHECK(chan_get_ingame_channel_id() == ID_INGAME && chan_get_hub_channel_id() == ID_HUB,
        "initial resolve");
    CHECK(g_audioMode == TS3_AUDIO_PROXIMITY, "ingame channel -> proximity");

    /* Admin deletes + recreates Ingame (new ID 30); we sit in the new one. */
    g_channels[2].id = 30;
    snprintf(g_channels[2].name, sizeof(g_channels[2].name), "Ingame");
    g_ownChannel = 30;
    const int listBefore = g_listCalls;
    tick_after(1000);
    CHECK(g_listCalls == listBefore, "without an event nothing is rescanned (IDs known)");
    CHECK(g_audioMode == TS3_AUDIO_PASSTHROUGH, "stale cache: new channel not recognised yet");

    for (int i = 0; i < 50; i++) { /* TS fires one event per channel */
        chan_on_channel_list_changed(30);
    }
    CHECK(chan_has_pending_work(), "dirty flag keeps the CEDRAIN loop alive");
    tick_after(1000);
    CHECK(g_listCalls == listBefore + 1, "50 events -> exactly one rescan");
    CHECK(chan_get_ingame_channel_id() == 30, "new Ingame ID resolved");
    CHECK(g_audioMode == TS3_AUDIO_PROXIMITY, "proximity restored for the recreated channel");
    CHECK(!chan_has_pending_work(), "dirty flag consumed");

    /* A failed list call must not wipe the cache. */
    chan_on_channel_list_changed(0);
    g_listFails = 1;
    tick_after(1000);
    CHECK(chan_get_ingame_channel_id() == 30 && chan_get_hub_channel_id() == ID_HUB,
        "unreadable list keeps the cached IDs");
    CHECK(g_audioMode == TS3_AUDIO_PROXIMITY, "proximity stays on during the failed scan");
    g_listFails = 0;
}

static void test_channel_cap(void) {
    printf("[7] 1024-channel cap\n");
    reset_world();
    g_config.enableAutomaticChannelChange = 0;
    g_channelCount = 0;
    for (int i = 0; i < 1500; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Channel %d", i);
        add_channel((uint64)(100 + i), name, 0);
    }
    snprintf(g_channels[900].name, sizeof(g_channels[900].name), "Hub");
    snprintf(g_channels[1000].name, sizeof(g_channels[1000].name), "Ingame");
    g_ownChannel = g_channels[5].id;

    tick_after(1000);
    CHECK(chan_get_hub_channel_id() == g_channels[900].id, "Hub at position 900 found (old cap was 128)");
    CHECK(chan_get_ingame_channel_id() == g_channels[1000].id, "Ingame at position 1000 found");

    /* Truncated list: channels beyond the cap must not erase a good cache. */
    snprintf(g_channels[900].name, sizeof(g_channels[900].name), "Channel 800");
    snprintf(g_channels[1300].name, sizeof(g_channels[1300].name), "Hub");
    chan_on_channel_list_changed(0);
    tick_after(1000);
    CHECK(chan_get_hub_channel_id() == g_channels[900].id,
        "hub now beyond the cap: cached ID kept, not dropped");
}

static void test_scan_throttle(void) {
    printf("[8] no unthrottled scans without Hub/Ingame\n");
    reset_world();
    g_channelCount = 0;
    add_channel(1, "General", 0);
    add_channel(2, "Chat", 0);
    g_ownChannel = 1;
    g_posValid = 1; /* wants ingame, ingame does not exist */

    for (int i = 0; i < 40; i++) {
        tick_after(600);
    }
    /* 40 ticks over 24 s: the 5 s retry allows ~5 scans, not 40. */
    CHECK(g_listCalls >= 3 && g_listCalls <= 6, "list scans follow the 5 s retry throttle");
    CHECK(g_moveCalls == 0, "no move without an ingame channel");

    g_posValid = 0; /* now wants hub, hub does not exist */
    const int before = g_listCalls;
    for (int i = 0; i < 20; i++) {
        tick_after(600);
    }
    CHECK(g_listCalls - before <= 3, "hub branch is throttled too");
}

static void test_wake_flags(void) {
    printf("[9] position flag edge + wake loop\n");
    reset_world();
    g_ownChannel = ID_INGAME;
    g_posValid = 1;
    tick_after(1000);
    CHECK(!chan_has_pending_work(), "idle ingame: nothing pending");

    chan_signal_position_update();
    CHECK(chan_has_pending_work(), "position update is pending");
    chan_tick(); /* same instant: throttled */
    CHECK(chan_has_pending_work(), "throttled tick does NOT swallow the position edge");
    tick_after(600);
    CHECK(!chan_has_pending_work(), "a real tick consumes it");

    /* Game closed, user in the lobby: hub move blocked only by the cooldown. */
    reset_world();
    g_posValid = 0;
    g_ownChannel = ID_LOBBY;
    g_moveSendResult = 0;
    tick_after(1000);
    CHECK(g_moveCalls == 1, "hub move attempted (send fails)");
    tick_after(600);
    CHECK(chan_has_pending_work(), "cooldown-blocked move keeps the loop alive without position updates");
    tick_after(2000);
    CHECK(g_moveCalls == 2, "and is retried by the next tick");

    /* No longer wanted -> flag cleared. */
    g_ownChannel = ID_HUB;
    tick_after(600);
    CHECK(!chan_has_pending_work(), "already in the hub: nothing wanted, loop released");
}

static void test_reset(void) {
    printf("[10] chan_reset drops move state\n");
    reset_world();
    tick_after(1000);
    CHECK(g_moveCalls == 1, "move in flight");
    chan_on_move_failed(0x030d);
    chan_on_channel_list_changed(0);
    chan_reset();
    CHECK(!chan_has_pending_work(), "nothing pending after reset");
    win32_shim_advance_ms(10000);
    tick_after(1000);
    CHECK(g_moveCalls == 2, "backoff gone after reset (new connection)");
}

int main(void) {
    test_rejected_move_backoff();
    test_failure_ignored_without_flight();
    test_inflight_timeout();
    test_timeout_without_wanted_move();
    test_sync_failure_cooldown();
    test_password_rule();
    test_channel_list_dirty();
    test_channel_cap();
    test_scan_throttle();
    test_wake_flags();
    test_reset();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
