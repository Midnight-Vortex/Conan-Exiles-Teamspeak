/* Behaviour test for the CE protocol senders/receivers (083 review fixes).

   Unlike the *_wire_test suites (pure codecs) this one compiles the REAL
   production files ts3_cemode.c, ts3_ceping.c, ts3_cepos.c and ts3_3d.c and
   drives them through their public functions. Everything they talk to (TS
   adapter, voice modes, pos file, player table, audio, log) is stubbed below,
   and GetTickCount64 is a fake clock the test advances by hand — so the
   timing rules (30 ms CEPOS floor, 1 s CEPING floor, 1 s nickname refresh,
   3D backoff) are checked exactly, without sleeping.

   Covers:
   - CEMODE: a mode edge that lands while flush runs is NOT lost; a failed
     send is retried a bounded number of times,
   - CEPING: 1 s send floor; reordered packets are no phantom loss; the loss
     log is rate limited; a restarted peer is re-based,
   - CEPOS: an idle flush does not call the nickname API; position, voice
     distance, send-cache invalidation and rename still go out in time; the
     keepalive keeps its 1 s period; received names are sanitized,
   - 3D: a failed client is backed off; one pass is capped at 64 API calls and
     continues round-robin; invalidate clears the backoff.

   Build/run on Linux: bash tests/run_tests.sh (plain gcc + test shims). The
   shim directory tests/support/proto_shim must be given as `-iquote` (so its
   plugin.h wins over src/plugin.h); GetTickCount64 is defined HERE, so
   win32_shim.c is not linked.
   Exit code 0 = all checks passed. */

#include <windows.h>

#include "core/mod_file/pos_file.h"
#include "core/proximity/player_table.h"
#include "core/util/log.h"
#include "core/voice/voice_modes.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/proximity/ts3_3d.h"
#include "ts/proximity/ts3_cemode.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_ceping.h"
#include "ts/proximity/ts3_client_limits.h"
#include "ts/proximity/ts3_proximity_audio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- fake clock -------------------------------------------------------------- */

static ULONGLONG g_now = 1000000;

ULONGLONG GetTickCount64(void) { return g_now; }

static void advance(ULONGLONG ms) { g_now += ms; }

/* ---- stubs: log ---------------------------------------------------------------- */

#define LOG_LINES 256
static char g_log[LOG_LINES][320];
static int g_logCount = 0;

static void log_capture(const char* fmt, va_list ap) {
    if (g_logCount < LOG_LINES) {
        vsnprintf(g_log[g_logCount], sizeof(g_log[0]), fmt, ap);
        g_logCount++;
    }
}

void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_capture(fmt, ap);
    va_end(ap);
}

void log_debug(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_capture(fmt, ap);
    va_end(ap);
}

static int log_count_containing(const char* needle) {
    int n = 0;
    for (int i = 0; i < g_logCount; i++) {
        if (strstr(g_log[i], needle)) {
            n++;
        }
    }
    return n;
}

/* ---- stubs: TS adapter ----------------------------------------------------------- */

BOOL pluginShuttingDown = FALSE;

static int g_connected = 1;
static int g_sendOk = 1;
static int g_wakeups = 0;
#define SENT_MAX 512
static char g_sent[SENT_MAX][200];
static int g_sentCount = 0;

static char g_nick[64] = "Alice";
static int g_nickOk = 1;
static int g_nickCalls = 0;

const char* ts3_get_plugin_id(void) { return "conan_exiles"; }
int ts3_thread_is_callback(void) { return 1; }
int ts3_is_connected(void) { return g_connected; }
anyID ts3_get_local_client_id(void) { return 1; }
void ts3_request_wakeup(void) { g_wakeups++; }

int ts3_send_plugin_command_server(const char* command) {
    if (!g_sendOk) {
        return 0;
    }
    if (g_sentCount < SENT_MAX) {
        snprintf(g_sent[g_sentCount], sizeof(g_sent[0]), "%s", command);
        g_sentCount++;
    }
    return 1;
}

int ts3_get_own_nickname(char* out, int len) {
    g_nickCalls++;
    if (!g_nickOk) {
        return 0;
    }
    snprintf(out, (size_t)len, "%s", g_nick);
    return 1;
}

/* 3D raw calls: per-client call counter + per-client "make it fail" switch. */
static int g_set3dCalls[TS3_MAX_CLIENT_ID];
static char g_set3dFail[TS3_MAX_CLIENT_ID];
static int g_set3dTotal = 0;

int ts3_set_3d_settings(float distanceFactor, float rolloffScale) {
    (void)distanceFactor;
    (void)rolloffScale;
    return 1;
}

int ts3_set_3d_listener(const TS3_VECTOR* position, const TS3_VECTOR* forward,
    const TS3_VECTOR* up) {
    (void)position;
    (void)forward;
    (void)up;
    return 1;
}

int ts3_set_3d_client(anyID clientID, const TS3_VECTOR* position) {
    (void)position;
    g_set3dCalls[clientID]++;
    g_set3dTotal++;
    return !g_set3dFail[clientID];
}

/* ---- stubs: voice modes, pos, table, audio, profile ------------------------------ */

static VoiceMode g_mode = VOICE_MODE_NORMAL;
static float g_modeDistance[3] = { 5.0f, 30.0f, 60.0f };
static int g_distanceCalls = 0;
static void (*g_onModeRead)(void) = NULL; /* fires once, right AFTER a mode read */

VoiceMode voice_mode_get_current(void) {
    const VoiceMode m = g_mode;
    if (g_onModeRead) {
        void (*h)(void) = g_onModeRead;
        g_onModeRead = NULL;
        h(); /* "another thread" switches the mode just after we read it */
    }
    return m;
}

float voice_mode_get_distance(VoiceMode mode) {
    g_distanceCalls++;
    return g_modeDistance[mode >= 0 && mode <= 2 ? mode : 1];
}

float voice_mode_get_current_distance(void) {
    return voice_mode_get_distance(g_mode);
}

static PosSample g_pos = { 1, 1000.0, 2000.0, 300.0, 90.0, 0.0 }; /* cm */

int pos_get_current(PosSample* out) {
    *out = g_pos;
    return 1;
}

static PlayerEntry g_table[PLAYER_TABLE_MAX_PLAYERS];
static int g_tableCount = 0;
static char g_putName[64];
static int g_putCalls = 0;

int player_table_snapshot(PlayerEntry* out, int maxEntries) {
    const int n = g_tableCount < maxEntries ? g_tableCount : maxEntries;
    memcpy(out, g_table, (size_t)n * sizeof(PlayerEntry));
    return n;
}

int player_table_put(unsigned short clientID, const char* name,
    float x, float y, float z, float voiceDistance, unsigned short* evictedClientID) {
    (void)clientID;
    (void)x;
    (void)y;
    (void)z;
    (void)voiceDistance;
    if (evictedClientID) {
        *evictedClientID = 0;
    }
    snprintf(g_putName, sizeof(g_putName), "%s", name);
    g_putCalls++;
    return 1;
}

void player_table_clear(void) { g_tableCount = 0; }

void ts3_audio_invalidate_client(anyID clientID) { (void)clientID; }
void ts3_audio_mark_client_dirty(anyID clientID) { (void)clientID; }

float server_profile_get_listen_add_distance(void) { return 0.0f; }

/* ---- helpers ------------------------------------------------------------------------ */

static void reset_all(void) {
    g_now += 100000;
    g_connected = 1;
    g_sendOk = 1;
    g_wakeups = 0;
    g_sentCount = 0;
    g_logCount = 0;
    g_nickCalls = 0;
    g_nickOk = 1;
    snprintf(g_nick, sizeof(g_nick), "Alice");
    g_mode = VOICE_MODE_NORMAL;
    g_onModeRead = NULL;
    g_distanceCalls = 0;
    g_modeDistance[0] = 5.0f;
    g_modeDistance[1] = 30.0f;
    g_modeDistance[2] = 60.0f;
    g_pos.x = 1000.0;
    g_pos.y = 2000.0;
    g_pos.z = 300.0;
    g_tableCount = 0;
    g_putCalls = 0;
    g_putName[0] = '\0';
    g_set3dTotal = 0;
    memset(g_set3dCalls, 0, sizeof(g_set3dCalls));
    memset(g_set3dFail, 0, sizeof(g_set3dFail));
    ts3_cemode_reset();
    ts3_ceping_reset();
    cepos_reset();
    ts3d_reset();
    for (int i = 1; i < TS3_MAX_CLIENT_ID; i++) {
        ts3d_invalidate_client((anyID)i);
    }
}

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char* in, size_t n, char* out) {
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        const size_t rem = n - i;
        const unsigned a = in[i];
        const unsigned b = rem > 1 ? in[i + 1] : 0;
        const unsigned c = rem > 2 ? in[i + 2] : 0;
        const unsigned t = (a << 16) | (b << 8) | c;
        out[j++] = b64chars[(t >> 18) & 63];
        out[j++] = b64chars[(t >> 12) & 63];
        out[j++] = rem > 1 ? b64chars[(t >> 6) & 63] : '=';
        out[j++] = rem > 2 ? b64chars[t & 63] : '=';
    }
    out[j] = '\0';
}

static int b64_val(char c) {
    const char* p = strchr(b64chars, c);
    return (c && p) ? (int)(p - b64chars) : -1;
}

/* Decode a sent "CEPOS:<base64>" command into a packet. */
static int decode_cepos(const char* command, CeposPacket* out) {
    unsigned char buf[128];
    size_t j = 0;
    unsigned val = 0;
    int valb = -8;

    if (strncmp(command, "CEPOS:", 6) != 0) {
        return 0;
    }
    for (const char* p = command + 6; *p && *p != '='; p++) {
        const int v = b64_val(*p);
        if (v < 0) {
            continue;
        }
        val = (val << 6) + (unsigned)v;
        valb += 6;
        if (valb >= 0) {
            if (j >= sizeof(buf)) {
                return 0;
            }
            buf[j++] = (unsigned char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    if (j != sizeof(*out)) {
        return 0;
    }
    memcpy(out, buf, sizeof(*out));
    return 1;
}

/* One CEPOS tick: the pos watcher signals, CEDRAIN flushes. */
static void cepos_tick(void) {
    cepos_signal_send_pending();
    cepos_flush();
}

static int last_sent_is(const char* prefix) {
    return g_sentCount > 0 && strncmp(g_sent[g_sentCount - 1], prefix, strlen(prefix)) == 0;
}

static int count_sent_prefix(const char* prefix) {
    int n = 0;
    for (int i = 0; i < g_sentCount; i++) {
        if (strncmp(g_sent[i], prefix, strlen(prefix)) == 0) {
            n++;
        }
    }
    return n;
}

/* ---- [1] CEMODE ---------------------------------------------------------------------- */

static void switch_to_shout_from_other_thread(void) {
    g_mode = VOICE_MODE_SHOUT;
    ts3_cemode_signal_send_pending(); /* what voice_mode_apply's hook does */
}

static void test_cemode(void) {
    printf("[1] CEMODE flush\n");

    reset_all();
    ts3_cemode_signal_send_pending();
    ts3_cemode_flush();
    CHECK(g_sentCount == 1 && last_sent_is("CEMODE:1;1;300"), "plain edge: normal mode, 30.0 m sent");
    CHECK(!ts3_cemode_send_pending(), "plain edge: flag consumed");

    /* The lost-edge race: the mode flips right after flush read it. */
    reset_all();
    ts3_cemode_signal_send_pending();
    g_onModeRead = switch_to_shout_from_other_thread;
    ts3_cemode_flush();
    CHECK(g_sentCount == 1 && last_sent_is("CEMODE:1;1;300"), "race: this flush still sent the mode it had read");
    CHECK(ts3_cemode_send_pending(), "race: the concurrent edge re-armed the flag (not wiped)");
    advance(40);
    ts3_cemode_flush();
    CHECK(g_sentCount == 2 && last_sent_is("CEMODE:1;2;600"), "race: next flush sends the new mode (shout, 60.0 m)");
    CHECK(!ts3_cemode_send_pending(), "race: flag consumed after the second send");

    /* Failed send: bounded retry, then give up (no endless spin). */
    reset_all();
    g_sendOk = 0;
    ts3_cemode_signal_send_pending();
    int pendingAfterFlush = 0;
    int flushes = 0;
    for (; flushes < 20; flushes++) {
        ts3_cemode_flush();
        pendingAfterFlush = ts3_cemode_send_pending();
        if (!pendingAfterFlush) {
            break;
        }
        advance(40);
    }
    CHECK(!pendingAfterFlush, "failing send: stops re-arming after the retry limit");
    CHECK(flushes == 5, "failing send: exactly 6 attempts (1 + 5 retries)");
    CHECK(log_count_containing("giving up") == 1, "failing send: gave up with one log line");
    g_sendOk = 1;
    ts3_cemode_signal_send_pending();
    advance(40);
    ts3_cemode_flush();
    CHECK(g_sentCount == 1, "failing send: a later edge sends normally again");

    /* A failed send that recovers is retried and arrives. */
    reset_all();
    g_sendOk = 0;
    ts3_cemode_signal_send_pending();
    ts3_cemode_flush();
    CHECK(ts3_cemode_send_pending() && g_sentCount == 0, "retry: edge kept after one failed send");
    g_sendOk = 1;
    advance(40);
    ts3_cemode_flush();
    CHECK(g_sentCount == 1 && !ts3_cemode_send_pending(), "retry: edge delivered once the send works");

    /* Rate limit: second signal inside 30 ms waits, flag stays armed. */
    reset_all();
    ts3_cemode_signal_send_pending();
    ts3_cemode_flush();
    g_mode = VOICE_MODE_SHOUT;
    ts3_cemode_signal_send_pending();
    advance(10);
    ts3_cemode_flush();
    CHECK(g_sentCount == 1 && ts3_cemode_send_pending(), "30 ms floor: early flush waits, flag kept");
    advance(30);
    ts3_cemode_flush();
    CHECK(g_sentCount == 2, "30 ms floor: sent after the floor");
}

/* ---- [2] CEPING ---------------------------------------------------------------------- */

static void ping_cmd(unsigned long seq, anyID from) {
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "CEPING:1;%lu", seq);
    (void)ts3_ceping_on_plugin_command("conan_exiles", cmd, from);
}

static void test_ceping(void) {
    printf("[2] CEPING send floor + receive tracking\n");

    reset_all();
    /* 3 simulated seconds of 30 ms position ticks. */
    for (int t = 0; t <= 3000; t += 30) {
        ts3_ceping_signal_send_pending();
        ts3_ceping_flush();
        advance(30);
    }
    CHECK(count_sent_prefix("CEPING:") >= 3 && count_sent_prefix("CEPING:") <= 4,
        "send floor: ~1 heartbeat per second (3-4 in 3 s, not ~100)");
    CHECK(strcmp(g_sent[0], "CEPING:1;1") == 0, "send floor: first heartbeat goes out at once with seq 1");
    CHECK(strcmp(g_sent[1], "CEPING:1;2") == 0, "send floor: sequence increments per real send");

    /* Reordered packets: 10, 12, 11, 13 -> one loss line at most (12), not two. */
    reset_all();
    ping_cmd(10, 7);
    ping_cmd(12, 7);
    ping_cmd(11, 7);
    ping_cmd(13, 7);
    CHECK(log_count_containing("lost") == 1, "reorder 10,12,11,13: a single loss line (was two)");
    ping_cmd(14, 7);
    ping_cmd(15, 7);
    CHECK(log_count_containing("lost") == 1, "reorder: 13->14->15 consecutive, no further loss");

    /* Flood: attacker-chosen gaps must not spam the log. */
    reset_all();
    ping_cmd(100, 9);
    for (int i = 1; i <= 19; i++) {
        ping_cmd(100UL + (unsigned long)i * 1000UL, 9);
    }
    CHECK(log_count_containing("lost") == 1, "flood: 19 gap messages in 1 s -> 1 log line");
    advance(1000);
    ping_cmd(100UL + 20UL * 1000UL, 9);
    CHECK(log_count_containing("lost") == 2, "flood: next line after 1 s");
    CHECK(log_count_containing("+18 earlier") == 1, "flood: suppressed counter reported (18)");

    /* The throttle is per peer. */
    reset_all();
    ping_cmd(1, 20);
    ping_cmd(1, 21);
    ping_cmd(5, 20);
    ping_cmd(5, 21);
    CHECK(log_count_containing("lost") == 2, "throttle is per peer: both peers log once");

    /* Peer restart (counter back to ~0) re-bases without a phantom loss. */
    reset_all();
    ping_cmd(5000, 30);
    ping_cmd(1, 30);
    ping_cmd(2, 30);
    ping_cmd(3, 30);
    CHECK(log_count_containing("lost") == 0, "restarted peer: re-based, no phantom loss");
    ping_cmd(5, 30);
    CHECK(log_count_containing("lost 1 ") == 1, "restarted peer: tracking works again afterwards");

    /* Invalid input is still ignored. */
    reset_all();
    ping_cmd(1, 0);
    (void)ts3_ceping_on_plugin_command("conan_exiles", "CEPING:1;9", 5000);
    (void)ts3_ceping_on_plugin_command("conan_exiles", "CEPING:9;9", 8);
    (void)ts3_ceping_on_plugin_command("other_plugin", "CEPING:1;9", 8);
    CHECK(log_count_containing("lost") == 0, "invalid id / version / plugin ignored");
}

/* ---- [3] CEPOS ----------------------------------------------------------------------- */

static void test_cepos_send(void) {
    CeposPacket pkt;

    printf("[3] CEPOS flush — cheap idle path\n");

    reset_all();
    cepos_tick();
    CHECK(g_sentCount == 1 && decode_cepos(g_sent[0], &pkt), "first tick: packet sent and decodes (56 bytes)");
    CHECK(strcmp(pkt.playerName, "Alice") == 0, "first tick: own nickname in packet");
    CHECK(pkt.x > 9.99f && pkt.x < 10.01f && pkt.y > 19.99f && pkt.y < 20.01f, "first tick: position in meters");
    CHECK(pkt.voiceDistance > 29.99f && pkt.voiceDistance < 30.01f, "first tick: voice distance 30 m");
    CHECK(g_nickCalls == 1, "first tick: nickname read once");

    /* 3 s idle: 100 ticks of 30 ms. */
    const int distBefore = g_distanceCalls;
    for (int i = 0; i < 100; i++) {
        advance(30);
        cepos_tick();
    }
    CHECK(g_sentCount == 3, "idle 3 s: only the 1 s keepalives go out (2 more)");
    CHECK(g_nickCalls <= 4, "idle 3 s: nickname API ~1/s (<= 4 calls, was 101)");
    CHECK(g_distanceCalls - distBefore == 100,
        "idle: voice distance still checked every flush (hub changes must go out at once)");

    /* Keepalive period: consecutive keepalives >= 1 s apart (sent counts above).
       Position moved by 1 m: immediate send. */
    const int sentBefore = g_sentCount;
    g_pos.x += 100.0;
    advance(30);
    cepos_tick();
    CHECK(g_sentCount == sentBefore + 1, "movement 1 m: sent on the very next tick");
    g_pos.x += 5.0; /* 5 cm < 8 cm epsilon */
    advance(30);
    cepos_tick();
    CHECK(g_sentCount == sentBefore + 1, "movement 5 cm (below epsilon): nothing sent");

    /* Voice distance change. */
    g_modeDistance[1] = 40.0f;
    advance(30);
    cepos_tick();
    CHECK(g_sentCount == sentBefore + 2, "voice distance 30->40: sent on the very next tick");
    CHECK(decode_cepos(g_sent[g_sentCount - 1], &pkt) && pkt.voiceDistance > 39.99f && pkt.voiceDistance < 40.01f,
        "voice distance: new value on the wire");

    /* Send cache invalidation (mode switch / settings applied). */
    cepos_invalidate_send_cache();
    advance(30);
    cepos_tick();
    CHECK(g_sentCount == sentBefore + 3, "cache invalidated: forced send on the very next tick");

    /* Rename: reaches the wire within the 1 s refresh. */
    snprintf(g_nick, sizeof(g_nick), "Bob");
    int ticks = 0;
    int sentAtRename = g_sentCount;
    while (ticks < 60 && g_sentCount == sentAtRename) {
        advance(30);
        cepos_tick();
        ticks++;
    }
    CHECK(g_sentCount == sentAtRename + 1 && decode_cepos(g_sent[g_sentCount - 1], &pkt)
        && strcmp(pkt.playerName, "Bob") == 0, "rename: new name sent");
    CHECK(ticks * 30 <= 1100, "rename: lag <= ~1 s");

    /* A failing nickname read keeps the last good name. */
    g_nickOk = 0;
    for (int i = 0; i < 80; i++) {
        advance(30);
        cepos_tick();
    }
    CHECK(decode_cepos(g_sent[g_sentCount - 1], &pkt) && strcmp(pkt.playerName, "Bob") == 0,
        "nickname API failure: last good name kept (no flip to 'Player')");

    /* Never had a name: fallback text. */
    reset_all();
    g_nickOk = 0;
    cepos_tick();
    CHECK(decode_cepos(g_sent[0], &pkt) && strcmp(pkt.playerName, "Player") == 0,
        "no nickname ever: 'Player' fallback as before");

    /* Name is truncated to the 15 chars of the wire field. */
    reset_all();
    snprintf(g_nick, sizeof(g_nick), "AVeryLongNicknameBeyondSixteen");
    cepos_tick();
    CHECK(decode_cepos(g_sent[0], &pkt) && strlen(pkt.playerName) == 15, "long nickname: cut to 15 chars");

    /* Rate floor: a tick 10 ms after a send waits and requests a retry wakeup. */
    reset_all();
    cepos_tick();
    g_pos.x += 500.0;
    advance(10);
    const int wakeBefore = g_wakeups;
    cepos_tick();
    CHECK(g_sentCount == 1 && g_wakeups > wakeBefore, "30 ms floor: early tick deferred with a retry wakeup");
    advance(30);
    cepos_flush();
    CHECK(g_sentCount == 2, "30 ms floor: sent after the floor");

    /* Disconnect/reset re-reads the nickname. */
    reset_all();
    cepos_tick();
    const int nickCallsBefore = g_nickCalls;
    cepos_reset();
    advance(30);
    cepos_tick();
    CHECK(g_nickCalls == nickCallsBefore + 1, "cepos_reset: nickname re-read on the next send");
}

static void test_cepos_receive(void) {
    printf("[4] CEPOS receive — name sanitizing\n");

    CeposPacket p;
    char cmd[200];
    char enc[128];

    reset_all();
    memset(&p, 0, sizeof(p));
    p.x = 10.0f;
    p.y = 20.0f;
    p.z = 3.0f;
    p.voiceDistance = 30.0f;
    memcpy(p.playerName, "Ev\nil\r\x1b[31mX\x7f!", 14);
    b64_encode((const unsigned char*)&p, sizeof(p), enc);
    snprintf(cmd, sizeof(cmd), "CEPOS:%s", enc);

    CHECK(cepos_on_plugin_command("conan_exiles", cmd, 5) == 1, "hostile name: command handled");
    CHECK(g_putCalls == 1, "hostile name: still stored (only sanitized)");
    int clean = 1;
    for (const char* c = g_putName; *c; c++) {
        if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7F) {
            clean = 0;
        }
    }
    CHECK(clean, "hostile name: no control byte reaches the player table");
    CHECK(strcmp(g_putName, "Ev?il??[31mX?!") == 0, "hostile name: control bytes became '?', rest intact");
    int logClean = 1;
    for (int i = 0; i < g_logCount; i++) {
        for (const char* c = g_log[i]; *c; c++) {
            if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7F) {
                logClean = 0;
            }
        }
    }
    CHECK(g_logCount > 0 && logClean, "hostile name: log line holds no control byte (no log injection)");

    /* Legit names are untouched, including UTF-8. */
    reset_all();
    memset(&p, 0, sizeof(p));
    p.x = 10.0f;
    p.y = 20.0f;
    p.z = 3.0f;
    p.voiceDistance = 30.0f;
    memcpy(p.playerName, "J\xC3\xB6rg_99 [Clan]", 16);
    b64_encode((const unsigned char*)&p, sizeof(p), enc);
    snprintf(cmd, sizeof(cmd), "CEPOS:%s", enc);
    (void)cepos_on_plugin_command("conan_exiles", cmd, 6);
    CHECK(strcmp(g_putName, "J\xC3\xB6rg_99 [Clan]") == 0, "legit UTF-8 name unchanged");

    /* Unterminated 16-byte name stays bounded. */
    reset_all();
    memset(&p, 0, sizeof(p));
    p.x = 10.0f;
    p.y = 20.0f;
    p.z = 3.0f;
    p.voiceDistance = 30.0f;
    memset(p.playerName, 'A', sizeof(p.playerName));
    b64_encode((const unsigned char*)&p, sizeof(p), enc);
    snprintf(cmd, sizeof(cmd), "CEPOS:%s", enc);
    (void)cepos_on_plugin_command("conan_exiles", cmd, 6);
    CHECK(strlen(g_putName) == 16, "unterminated name: bounded to the 16-char table field");
}

/* ---- [5] 3D -------------------------------------------------------------------------- */

static void fill_table(int count, float xBase) {
    g_tableCount = count;
    memset(g_table, 0, sizeof(g_table));
    for (int i = 0; i < count; i++) {
        g_table[i].clientID = (unsigned short)(10 + i);
        g_table[i].x = xBase + (float)(i % 10) * 0.01f;
        g_table[i].y = 20.0f;
        g_table[i].z = 3.0f;
        g_table[i].voiceDistance = 30.0f;
        g_table[i].valid = 1;
    }
}

static void test_3d(void) {
    printf("[5] 3D apply — backoff + per-pass cap\n");

    /* Negative cache. */
    reset_all();
    fill_table(1, 12.0f);
    g_set3dFail[10] = 1;
    ts3d_apply();
    CHECK(g_set3dCalls[10] == 1, "failure: first attempt made");
    for (int i = 0; i < 18; i++) { /* 18 passes, 60 ms apart = ~1.08 s */
        advance(60);
        ts3d_apply();
    }
    CHECK(g_set3dCalls[10] == 2, "failure: ~1 s of passes -> exactly one retry (was ~19 calls)");
    g_set3dFail[10] = 0;
    advance(1100);
    ts3d_apply();
    CHECK(g_set3dCalls[10] == 3, "failure: retried after the backoff and succeeded");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dCalls[10] == 3, "success: unchanged position is deduped again");

    /* invalidate clears the backoff. */
    reset_all();
    fill_table(1, 12.0f);
    g_set3dFail[10] = 1;
    ts3d_apply();
    ts3d_invalidate_client(10);
    advance(60);
    ts3d_apply();
    CHECK(g_set3dCalls[10] == 2, "invalidate: backoff cleared, retried at once");

    /* Cap: 200 clients, all new positions. */
    reset_all();
    fill_table(200, 12.0f);
    ts3d_apply();
    CHECK(g_set3dTotal == 64, "cap: first pass makes exactly 64 calls");
    CHECK(ts3d_has_pending(), "cap: pending reported after a capped pass");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 128, "cap: second pass continues with the next 64");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 192, "cap: third pass");
    CHECK(ts3d_has_pending(), "cap: still pending while clients wait");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 200, "cap: fourth pass finishes the remaining 8");
    CHECK(!ts3d_has_pending(), "cap: nothing pending once everyone was served");
    int everyoneOnce = 1;
    for (int i = 0; i < 200; i++) {
        if (g_set3dCalls[10 + i] != 1) {
            everyoneOnce = 0;
        }
    }
    CHECK(everyoneOnce, "cap: round-robin served every client exactly once");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 200, "cap: converged — further passes make no calls");

    /* Pending expires on its own if no further pass comes. */
    reset_all();
    fill_table(200, 12.0f);
    ts3d_apply();
    CHECK(ts3d_has_pending(), "pending window: set after a capped pass");
    advance(600);
    CHECK(!ts3d_has_pending(), "pending window: expires (cannot spin the dispatcher)");

    /* Many failing clients cost one capped batch, then nothing. */
    reset_all();
    fill_table(100, 12.0f);
    for (int i = 0; i < 100; i++) {
        g_set3dFail[10 + i] = 1;
    }
    ts3d_apply();
    CHECK(g_set3dTotal == 64, "failing crowd: first pass capped at 64 attempts");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 100, "failing crowd: second pass tries only the 36 never attempted");
    advance(60);
    ts3d_apply();
    CHECK(g_set3dTotal == 100, "failing crowd: third pass makes no calls (all backed off)");

    /* Out-of-range peers cost nothing. */
    reset_all();
    fill_table(1, 500.0f);
    ts3d_apply();
    CHECK(g_set3dTotal == 0, "out of hear range: no API call");

    /* Reset clears pending + backoff. */
    reset_all();
    fill_table(200, 12.0f);
    ts3d_apply();
    ts3d_reset();
    CHECK(!ts3d_has_pending(), "reset: pending cleared");
}

int main(void) {
    printf("protocol flush/receive harness (real cemode/ceping/cepos/3d, stubbed environment)\n");
    test_cemode();
    test_ceping();
    test_cepos_send();
    test_cepos_receive();
    test_3d();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
