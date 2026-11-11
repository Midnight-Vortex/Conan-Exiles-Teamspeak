/* Unit tests for src/core/http/http_ports.h (102: fallback port and handover
   between plugin instances - pure helpers, no sockets).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\http_ports_test.c /Fe:tests\out\http_ports_test.exe */

#include "core/http/http_ports.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void test_ports(void) {
    printf("[1] port list\n");
    CHECK(POS_HTTP_PORT_COUNT == 2, "exactly one fallback port");
    CHECK(http_port_at(0) == 52734, "primary port is 52734 (burned into every mod version)");
    CHECK(http_port_at(1) == 42734, "fallback port is 42734");
    CHECK(http_port_at(1) < 49152, "fallback lies outside the Windows dynamic port range");
    CHECK(http_port_at(-1) == 0 && http_port_at(2) == 0, "index out of range gives 0");
    CHECK(http_port_index(52734) == 0 && http_port_index(42734) == 1, "index of our ports");
    CHECK(http_port_index(80) == -1 && http_port_index(0) == -1, "foreign port has no index");
}

static void test_status(void) {
    char body[256];
    char tiny[16];
    HttpPeerStatus st;

    printf("[2] status body\n");
    CHECK(http_status_json(body, (int)sizeof(body), 1, 4711UL, 52734, "8.0.23") > 0, "status fits");
    CHECK(strcmp(body, "{\"ok\":true,\"service\":\"conan_exiles_ts\",\"act\":1,\"pid\":4711,\"port\":52734,\"ver\":\"8.0.23\"}") == 0,
        "status text (active)");
    http_status_parse(body, &st);
    CHECK(st.isPlugin && st.hasAct && st.act == 1 && st.pid == 4711UL, "round trip: active instance");

    http_status_json(body, (int)sizeof(body), 0, 99UL, 42734, "8.0.23");
    http_status_parse(body, &st);
    CHECK(st.isPlugin && st.hasAct && st.act == 0 && st.pid == 99UL, "round trip: idle instance");

    {
        /* volatile: the compiler must not see the too small size at compile time */
        volatile int tinyLen = (int)sizeof(tiny);
        CHECK(http_status_json(tiny, tinyLen, 1, 1UL, 52734, "8.0.23") == 0 && tiny[0] == '\0',
            "too small buffer: 0 and empty string");
    }
    CHECK(http_status_json(NULL, 10, 1, 1UL, 1, "x") == 0, "NULL buffer");

    /* What an older plugin (no /v1/status route) and a foreign program answer. */
    http_status_parse("{\"ok\":false,\"error\":\"not found\"}", &st);
    CHECK(!st.isPlugin && !st.hasAct && st.pid == 0, "404 body of plugin 8.0.22: not recognised");
    http_status_parse("<html>hello</html>", &st);
    CHECK(!st.isPlugin && !st.hasAct, "foreign program");
    http_status_parse(NULL, &st);
    CHECK(!st.isPlugin && !st.hasAct && !st.act && st.pid == 0, "no body");
    http_status_parse("{\"service\":\"conan_exiles_ts\",\"act\": 0 }", &st);
    CHECK(st.isPlugin && st.hasAct && st.act == 0, "blank behind the colon");
    http_status_parse("{\"service\":\"conan_exiles_ts\",\"act\":x}", &st);
    CHECK(st.isPlugin && !st.hasAct, "act without a number does not count");
}

static void test_json_int(void) {
    long v = -1;
    printf("[3] integer field\n");
    CHECK(http_json_int("{\"a\":1,\"act\":0,\"pid\":77}", "pid", &v) && v == 77, "finds pid");
    CHECK(http_json_int("{\"act\":1}", "act", &v) && v == 1, "finds act");
    CHECK(!http_json_int("{\"fact\":1}", "act", &v), "key match is by quoted name (fact is not act)");
    CHECK(!http_json_int("{\"x\":1}", "act", &v), "missing key");
    CHECK(!http_json_int(NULL, "act", &v) && !http_json_int("{}", NULL, &v) && !http_json_int("{}", "act", NULL),
        "NULL arguments");
}

static void test_takeover(void) {
    HttpPeerStatus idle = { 1, 1, 0, 10UL };
    HttpPeerStatus active = { 1, 1, 1, 10UL };
    HttpPeerStatus oldPlugin = { 0, 0, 0, 0UL };
    HttpPeerStatus noAct = { 1, 0, 0, 0UL };

    printf("[4] takeover decision\n");
    CHECK(http_takeover_decide(1, &idle) == HTTP_TAKEOVER_ASK_YIELD, "idle instance of ours: ask for the port");
    CHECK(http_takeover_decide(1, &active) == HTTP_TAKEOVER_FALLBACK, "active instance keeps the port");
    CHECK(http_takeover_decide(1, &oldPlugin) == HTTP_TAKEOVER_FALLBACK, "older plugin / foreign program: fallback");
    CHECK(http_takeover_decide(1, &noAct) == HTTP_TAKEOVER_FALLBACK, "our service without act: fallback");
    CHECK(http_takeover_decide(0, &idle) == HTTP_TAKEOVER_FALLBACK, "holder did not answer: fallback");
    CHECK(http_takeover_decide(1, NULL) == HTTP_TAKEOVER_FALLBACK, "no status: fallback");

    printf("[5] holder side\n");
    CHECK(http_yield_allowed(52734, 0) == 1, "idle holder of the primary port yields");
    CHECK(http_yield_allowed(52734, 1) == 0, "active holder never yields");
    CHECK(http_yield_allowed(42734, 0) == 0, "the fallback port is never given away");
    CHECK(http_yield_allowed(0, 0) == 0, "not bound: nothing to give");
}

static void test_append_act(void) {
    char reply[64];
    char tight[32];

    printf("[6] act in the position reply\n");
    strcpy(reply, "{\"ok\":true}");
    CHECK(http_reply_append_act(reply, (int)sizeof(reply), 1) && strcmp(reply, "{\"ok\":true,\"act\":1}") == 0,
        "plain reply, active");
    strcpy(reply, "{\"ok\":true,\"talk\":0,\"talkers\":\"\"}");
    CHECK(http_reply_append_act(reply, (int)sizeof(reply), 0)
        && strcmp(reply, "{\"ok\":true,\"talk\":0,\"talkers\":\"\",\"act\":0}") == 0, "behind talkers, idle");
    strcpy(reply, "{}");
    CHECK(http_reply_append_act(reply, (int)sizeof(reply), 1) && strcmp(reply, "{\"act\":1}") == 0, "empty object");
    strcpy(reply, "not json");
    CHECK(!http_reply_append_act(reply, (int)sizeof(reply), 1) && strcmp(reply, "not json") == 0, "no object: untouched");
    strcpy(reply, "");
    CHECK(!http_reply_append_act(reply, (int)sizeof(reply), 1), "empty text");
    /* 11 chars, the closing brace is replaced by 9 chars: 19 chars + NUL = 20 bytes needed */
    strcpy(tight, "{\"ok\":true}");
    CHECK(!http_reply_append_act(tight, 19, 1) && strcmp(tight, "{\"ok\":true}") == 0, "buffer one byte short: untouched");
    CHECK(http_reply_append_act(tight, 20, 1) && strcmp(tight, "{\"ok\":true,\"act\":1}") == 0, "buffer exactly large enough");
    CHECK(!http_reply_append_act(NULL, 10, 1) && !http_reply_append_act(reply, 0, 1), "NULL / zero size");

    /* The mod's talkers parser reads up to the next quote: the field must come AFTER talkers. */
    strcpy(reply, "{\"ok\":true,\"talkers\":\"1,2,3\"}");
    http_reply_append_act(reply, (int)sizeof(reply), 1);
    CHECK(strstr(reply, "\"talkers\":\"1,2,3\",\"act\":1}") != NULL, "talkers value is not touched");
}

int main(void) {
    test_ports();
    test_status();
    test_json_int();
    test_takeover();
    test_append_act();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
