/* Host test for src/ts/nick/nick_anonymize.c (086 review fixes).

   nick_anonymize.c is #included here (not linked) and runs against a FAKE
   TeamSpeak client with two server tabs. Build/run: bash tests/run_tests.sh.
   Exit code 0 = all checks passed.

   Covered findings:
   12 the saved real name is bound to its connection: after a tab switch or a
      server switch in the same tab a name saved elsewhere is dropped, never
      restored onto the wrong server (same-tab relog still restores)
   13 names of 64 characters (up to 256 bytes UTF-8) survive anonymize/restore
      byte for byte (the old 64-byte buffer cut them mid-character)
   14 the real name never reaches the log */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shim has no QueryPerformanceCounter / GetCurrentProcessId (nick_rand seeds
   with them) — minimal stand-ins. */
typedef union {
    long long QuadPart;
} LARGE_INTEGER;
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* p) { p->QuadPart = 424242; return TRUE; }
static inline unsigned long GetCurrentProcessId(void) { return 4242; }

#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/util/log.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- fake client with two tabs -------------------------------------------------- */

#define NICK_FAKE_LEN 512

typedef struct FakeTab {
    uint64 handler;
    char uid[64]; /* "" = server UID not available */
    char nick[NICK_FAKE_LEN];
} FakeTab;

static FakeTab g_tabs[2];
static int g_activeTab = 0;
static int g_connected = 1;
static int g_setNickFails = 0;
static int g_randomizer = 1;

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

int ts3_thread_is_callback(void) { return 1; }
int ts3_is_connected(void) { return g_connected; }
uint64 ts3_get_active_connection(void) { return g_tabs[g_activeTab].handler; }

int ts3_get_server_uid(char* out, int outLen) {
    if (!g_tabs[g_activeTab].uid[0]) {
        return 0;
    }
    snprintf(out, (size_t)outLen, "%s", g_tabs[g_activeTab].uid);
    return 1;
}

int ts3_get_own_nickname(char* out, int outLen) {
    if (strlen(g_tabs[g_activeTab].nick) >= (size_t)outLen) {
        return 0; /* does not fit - the real strncpy_s(_TRUNCATE) would fail too */
    }
    snprintf(out, (size_t)outLen, "%s", g_tabs[g_activeTab].nick);
    return 1;
}

int ts3_set_own_nickname(const char* nickname) {
    if (g_setNickFails || !nickname || strlen(nickname) < 3) {
        return 0;
    }
    snprintf(g_tabs[g_activeTab].nick, sizeof(g_tabs[g_activeTab].nick), "%s", nickname);
    return 1;
}

int ts3_get_channel_client_list(uint64 channelID, anyID* out, int maxClients) {
    (void)channelID; (void)out; (void)maxClients;
    return 0;
}
int ts3_get_client_nickname(anyID clientID, char* out, int outLen) {
    (void)clientID; (void)out; (void)outLen;
    return 0;
}

int server_profile_get_nickname_randomizer(void) { return g_randomizer; }

void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log[g_logCount % LOG_LINES], sizeof(g_log[0]), fmt, ap);
    va_end(ap);
    g_logCount++;
}
void log_debug(const char* fmt, ...) { (void)fmt; }

/* ---- code under test -------------------------------------------------------------- */

#include "ts/nick/nick_anonymize.c"

/* ---- helpers ------------------------------------------------------------------------ */

#define INGAME_CH 3

static void setup(void) {
    memset(g_tabs, 0, sizeof(g_tabs));
    g_tabs[0].handler = 1;
    snprintf(g_tabs[0].uid, sizeof(g_tabs[0].uid), "UID-A");
    g_tabs[1].handler = 2;
    snprintf(g_tabs[1].uid, sizeof(g_tabs[1].uid), "UID-B");
    g_activeTab = 0;
    g_connected = 1;
    g_setNickFails = 0;
    g_randomizer = 1;
    g_logCount = 0;
    nick_reset();
    g_reuseAnonNickname[0] = '\0';
}

static const char* nick_of(int tab) { return g_tabs[tab].nick; }

/* ---- tests -------------------------------------------------------------------------- */

static void test_basic_roundtrip(void) {
    printf("[1] anonymize -> hub restore\n");
    setup();
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "Alice");

    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(nick_looks_anonymized(nick_of(0)), "ingame name is digits");
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), "Alice") == 0, "hub restore gives 'Alice' back");

    /* second round reuses the same digits */
    nick_anonymize_before_ingame(INGAME_CH);
    char first[NICK_FAKE_LEN];
    snprintf(first, sizeof(first), "%s", nick_of(0));
    nick_restore_in_hub();
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(strcmp(first, nick_of(0)) == 0, "second round reuses the anonymized digits");
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), "Alice") == 0, "and restores again");
}

static void test_long_names(void) {
    printf("[2] 64-character names (256 bytes UTF-8) survive byte for byte\n");
    setup();

    char cjk[NICK_FAKE_LEN] = "";     /* 64 x 3-byte characters = 192 bytes */
    for (int i = 0; i < 64; i++) {
        strcat(cjk, "\xE6\xBC\xA2");
    }
    char emoji[NICK_FAKE_LEN] = "";    /* 64 x 4-byte characters = 256 bytes */
    for (int i = 0; i < 64; i++) {
        strcat(emoji, "\xF0\x9F\x98\x80");
    }

    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "%s", cjk);
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(nick_looks_anonymized(nick_of(0)), "192-byte CJK name anonymized");
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), cjk) == 0 && strlen(nick_of(0)) == 192, "192-byte CJK name restored intact");

    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "%s", emoji);
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(nick_looks_anonymized(nick_of(0)), "256-byte emoji name anonymized");
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), emoji) == 0 && strlen(nick_of(0)) == 256, "256-byte emoji name restored intact");
}

static void test_privacy_log(void) {
    printf("[3] the real name never reaches the log\n");
    setup();
    const char* secret = "RealName_Zebra";
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "%s", secret);

    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);          /* pre-ingame anonymized */
    nick_restore_in_hub();                            /* hub restore done */
    nick_anonymize_before_ingame(INGAME_CH);          /* reusing anonymized nick */
    g_setNickFails = 1;
    nick_restore_in_hub();                            /* hub restore failed */
    g_setNickFails = 0;
    nick_restore_in_hub();
    CHECK(log_contains("pre-ingame anonymized"), "anonymize line was logged");
    CHECK(log_contains("reusing anonymized nick"), "reuse line was logged");
    CHECK(log_contains("hub restore done"), "restore line was logged");
    CHECK(log_contains("hub restore failed"), "restore-failure line was logged");

    /* No unique name can be set: failure path logged too. */
    nick_reset();
    g_reuseAnonNickname[0] = '\0';
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "%s", secret);
    g_setNickFails = 1;
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(log_contains("failed to assign"), "assign-failure line was logged");
    g_setNickFails = 0;

    CHECK(!log_contains(secret), "no log line contains the real name");
    CHECK(!log_contains("RealName"), "not even a fragment of it");
}

static void test_tab_switch(void) {
    printf("[4] tab switch: the saved name never lands on the wrong server\n");
    setup();
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "Alice");
    snprintf(g_tabs[1].nick, sizeof(g_tabs[1].nick), "Bob");

    /* Tab A: connect, go ingame (anonymized). */
    g_activeTab = 0;
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);
    CHECK(nick_looks_anonymized(nick_of(0)), "tab A is anonymized");

    /* Switch to tab B (not anonymized): its own name is remembered. */
    g_activeTab = 1;
    nick_on_connected();

    /* Back to tab A (still digits): the saved name belongs to B -> dropped. */
    g_activeTab = 0;
    nick_on_connected();
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), "Bob") != 0, "'Bob' is NOT restored onto server A");
    CHECK(nick_looks_anonymized(nick_of(0)), "tab A keeps its digits (real name unknown)");
    CHECK(strcmp(nick_of(1), "Bob") == 0, "tab B is untouched");
    CHECK(log_contains("another server connection"), "the drop is logged");
}

static void test_same_tab_relog(void) {
    printf("[5] same tab, same server: relog with digits still restores\n");
    setup();
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "Alice");
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);

    /* Disconnect/reconnect keeps the digits name; the plugin state is kept. */
    nick_on_connected();
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), "Alice") == 0, "restored after the relog");

    /* Server UID unavailable on one side: the handler ID decides. */
    setup();
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "Alice");
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);
    g_tabs[0].uid[0] = '\0';
    nick_on_connected();
    nick_restore_in_hub();
    CHECK(strcmp(nick_of(0), "Alice") == 0, "missing UID on one side: same handler still matches");
}

static void test_server_switch_same_tab(void) {
    printf("[6] same tab, other server: saved name is dropped\n");
    setup();
    snprintf(g_tabs[0].nick, sizeof(g_tabs[0].nick), "Alice");
    nick_on_connected();
    nick_anonymize_before_ingame(INGAME_CH);

    /* The user reconnects this tab to a different server; digits still set. */
    snprintf(g_tabs[0].uid, sizeof(g_tabs[0].uid), "UID-OTHER");
    nick_on_connected();
    nick_restore_in_hub();
    CHECK(nick_looks_anonymized(nick_of(0)), "no foreign name restored on the new server");
    CHECK(strcmp(nick_of(0), "Alice") != 0, "'Alice' (server A's name) is not applied");
}

int main(void) {
    test_basic_roundtrip();
    test_long_names();
    test_privacy_log();
    test_tab_switch();
    test_same_tab_relog();
    test_server_switch_same_tab();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
