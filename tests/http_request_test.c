/* Unit tests for src/core/http/http_request.h (pure HTTP request helpers of the
   localhost position server: header parsing, browser guard, Content-Length,
   log throttle, accept() error class, rebind backoff).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\http_request_test.c /Fe:tests\out\http_request_test.exe */

#include "core/http/http_request.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

#define PORT 52734

/* Header block of a request (everything up to and incl. the blank line). */
static int hdr_len(const char* req) {
    return http_find_header_end(req, (int)strlen(req));
}

static int guard_of(const char* req, char* what, int whatSize) {
    return http_request_guard(req, hdr_len(req), PORT, what, whatSize);
}

/* What the Conan Exiles Enhanced "Start Http Request" node sends (Host, UA and
   X-Request-Id are set by CEE itself; Content-Type as the mod author chose). */
static const char* const REQ_CEE =
    "POST /v1/position HTTP/1.1\r\n"
    "Host: 127.0.0.1:52734\r\n"
    "User-Agent: UnrealEngine\r\n"
    "X-Request-Id: 0f3a\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 54\r\n"
    "\r\n"
    "{\"seq\":42,\"x\":12345.0,\"y\":67890.0,\"z\":200.0,\"yaw\":45.0}";

static void test_header_block(void) {
    printf("[1] header block + single headers\n");
    char v[64];
    int full = 0;
    const char* r =
        "POST /v1/position HTTP/1.1\r\n"
        "host:   127.0.0.1:52734  \r\n"
        "CONTENT-length: 12\r\n"
        "X-Content-Length: 99\r\n"
        "Folded: a\r\n"
        " Content-Length: 5\r\n"
        "Spaced : yes\r\n"
        "\r\n"
        "Content-Length: 7\r\nOrigin: evil\r\n";
    const int he = hdr_len(r);

    CHECK(he > 0 && memcmp(r + he - 4, "\r\n\r\n", 4) == 0, "header end found just behind the blank line");
    CHECK(http_find_header_end("GET / HTTP/1.1\r\nHost: x\r\n", 25) == -1, "incomplete header block -> -1");
    CHECK(http_find_header_end("\r\n\r\n", 4) == 4, "minimal block");
    CHECK(http_find_header_end("abc\r\n\r", 6) == -1, "partial terminator -> -1");

    CHECK(http_header_get(r, he, "Host", 0, v, sizeof(v), &full) && strcmp(v, "127.0.0.1:52734") == 0 && full == 15,
        "name case-insensitive, value trimmed");
    CHECK(http_header_get(r, he, "content-length", 0, v, sizeof(v), NULL) && strcmp(v, "12") == 0,
        "upper-case name matches lower-case query");
    CHECK(!http_header_get(r, he, "Content-Length", 1, v, sizeof(v), NULL),
        "X-Content-Length, folded line and body text are no second Content-Length");
    CHECK(!http_header_get(r, he, "Origin", 0, v, sizeof(v), NULL), "text in the body is never a header");
    CHECK(http_header_get(r, he, "Spaced", 0, v, sizeof(v), NULL) && strcmp(v, "yes") == 0, "blank before the colon tolerated");
    CHECK(!http_header_get(r, he, "Folded-x", 0, v, sizeof(v), NULL), "unknown header absent");
    CHECK(!http_header_get(r, he, "GET", 0, v, sizeof(v), NULL), "request line is not a header");

    char tiny[4];
    CHECK(http_header_get(r, he, "Host", 0, tiny, sizeof(tiny), &full) && strcmp(tiny, "127") == 0 && full == 15,
        "value truncated into a small buffer, full length reported");
}

static void test_content_length(void) {
    printf("[2] Content-Length parsing\n");
    long n = -1;
#define CL(req) http_content_length((req), hdr_len(req), 3000, &n)

    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 54\r\n\r\n") == HTTP_CL_OK && n == 54, "plain number");
    CHECK(CL("POST / HTTP/1.1\r\ncontent-length: 54\r\n\r\n") == HTTP_CL_OK && n == 54, "lower-case spelling");
    CHECK(CL("POST / HTTP/1.1\r\nCONTENT-LENGTH:54\r\n\r\n") == HTTP_CL_OK && n == 54, "no blank after the colon");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n") == HTTP_CL_OK && n == 0, "zero");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 007\r\n\r\n") == HTTP_CL_OK && n == 7, "leading zeros");
    CHECK(CL("POST / HTTP/1.1\r\nHost: x\r\n\r\n") == HTTP_CL_ABSENT, "absent");
    CHECK(CL("POST / HTTP/1.1\r\nX-Content-Length: 12\r\n\r\n") == HTTP_CL_ABSENT, "X-Content-Length is not Content-Length");
    CHECK(CL("POST / HTTP/1.1\r\nHost: x\r\n\r\nContent-Length: 12") == HTTP_CL_ABSENT, "Content-Length text in the body is ignored");

    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n") == HTTP_CL_INVALID, "letters");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n") == HTTP_CL_INVALID, "negative");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: +5\r\n\r\n") == HTTP_CL_INVALID, "plus sign");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 1e3\r\n\r\n") == HTTP_CL_INVALID, "exponent");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 0x10\r\n\r\n") == HTTP_CL_INVALID, "hex");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 12 34\r\n\r\n") == HTTP_CL_INVALID, "blank inside");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length:\r\n\r\n") == HTTP_CL_INVALID, "empty value");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n") == HTTP_CL_INVALID, "huge number (overflow)");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 4294967297\r\n\r\n") == HTTP_CL_INVALID
        || CL("POST / HTTP/1.1\r\nContent-Length: 4294967297\r\n\r\n") == HTTP_CL_TOO_BIG,
        "32-bit wrap value is never 'ok'");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 12\r\nContent-Length: 12\r\n\r\n") == HTTP_CL_OK && n == 12,
        "identical duplicates are fine");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 12\r\nContent-Length: 13\r\n\r\n") == HTTP_CL_INVALID,
        "conflicting duplicates rejected");

    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 3000\r\n\r\n") == HTTP_CL_OK && n == 3000, "exactly the buffer room");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 3001\r\n\r\n") == HTTP_CL_TOO_BIG && n == 3001, "one byte too much");
    CHECK(CL("POST / HTTP/1.1\r\nContent-Length: 2147483647\r\n\r\n") == HTTP_CL_TOO_BIG, "INT_MAX does not fit the buffer");
#undef CL

    /* body length: clamp to what really arrived, never read stale bytes */
    CHECK(http_body_len(1, 54, 54) == 54, "body = announced length");
    CHECK(http_body_len(1, 20, 54) == 20, "extra bytes behind the announced body are ignored");
    CHECK(http_body_len(1, 54, 30) == 30, "never longer than what arrived (stale stack bytes)");
    CHECK(http_body_len(0, 0, 41) == 41, "no Content-Length: what came with the headers");
    CHECK(http_body_len(1, 0, 41) == 0, "Content-Length: 0 means empty");
    CHECK(http_body_len(0, 0, -3) == 0, "negative available -> 0");

    CHECK(http_body_incomplete(1, 54, 30) == 1, "short body detected (late / truncated)");
    CHECK(http_body_incomplete(1, 54, 54) == 0 && http_body_incomplete(1, 54, 60) == 0, "complete body");
    CHECK(http_body_incomplete(0, 0, 0) == 0, "no Content-Length: nothing to wait for");
}

static void test_host_and_origin(void) {
    printf("[3] Host / Origin matching\n");
    CHECK(http_host_is_local("127.0.0.1") && http_host_is_local("127.0.0.1:52734"), "127.0.0.1 with/without port");
    CHECK(http_host_is_local("localhost") && http_host_is_local("LOCALHOST:52734") && http_host_is_local("LocalHost"), "localhost, any case");
    CHECK(http_host_is_local("[::1]") && http_host_is_local("[::1]:52734"), "[::1] with/without port");
    CHECK(http_host_is_local("127.0.0.1:80"), "any port is fine, only the NAME matters");
    CHECK(!http_host_is_local("evil.example") && !http_host_is_local("evil.example:52734"), "foreign host");
    CHECK(!http_host_is_local("127.0.0.1.evil.example") && !http_host_is_local("localhost.evil.example"), "look-alike suffix");
    CHECK(!http_host_is_local("127.0.0.1@evil.example") && !http_host_is_local("evil.example/127.0.0.1"), "userinfo / path tricks");
    CHECK(!http_host_is_local("") && !http_host_is_local(":52734"), "empty host");
    CHECK(!http_host_is_local("127.0.0.1:abc") && !http_host_is_local("127.0.0.1:1234567"), "bad port");
    CHECK(!http_host_is_local("[::1") && !http_host_is_local("[::2]") && !http_host_is_local("::1"), "malformed / other IPv6");
    CHECK(!http_host_is_local("0.0.0.0") && !http_host_is_local("192.168.1.5:52734"), "other addresses");

    CHECK(http_origin_is_own("http://127.0.0.1:52734", PORT), "own origin (127.0.0.1)");
    CHECK(http_origin_is_own("http://localhost:52734", PORT), "own origin (localhost)");
    CHECK(http_origin_is_own("HTTP://LocalHost:52734", PORT), "scheme + host ignore case");
    CHECK(!http_origin_is_own("http://127.0.0.1:52735", PORT), "port must match exactly");
    CHECK(!http_origin_is_own("http://127.0.0.1", PORT), "no port = port 80, not ours");
    CHECK(!http_origin_is_own("https://127.0.0.1:52734", PORT), "https is not ours");
    CHECK(!http_origin_is_own("http://127.0.0.1:52734/", PORT) && !http_origin_is_own("http://127.0.0.1:52734.evil.example", PORT), "no suffix");
    CHECK(!http_origin_is_own("null", PORT) && !http_origin_is_own("", PORT) && !http_origin_is_own("https://evil.example", PORT), "null / empty / foreign");
}

static void test_guard(void) {
    printf("[4] browser guard\n");
    char what[96];

    CHECK(guard_of(REQ_CEE, what, sizeof(what)) == HTTP_GUARD_OK, "CEE-style POST (Host 127.0.0.1:52734, no Origin) passes");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: localhost:52734\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK, "GET /health with localhost host");
    CHECK(guard_of("GET /health HTTP/1.0\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK, "no Host header at all passes");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nSec-Fetch-Site: none\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK,
        "address bar navigation (Sec-Fetch-Site: none) passes");

    /* CEE might send its approved origin: accepted */
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nOrigin: http://127.0.0.1:52734\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK,
        "own Origin http://127.0.0.1:52734 passes");
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\nOrigin: http://localhost:52734\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK,
        "own Origin http://localhost:52734 passes");

    /* The attack from the review: text/plain no-cors POST from a web page */
    int g = guard_of("POST /v1/position HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nOrigin: https://evil.example\r\n"
        "Content-Type: text/plain;charset=UTF-8\r\nContent-Length: 40\r\n\r\n", what, sizeof(what));
    CHECK(g == HTTP_GUARD_ORIGIN && strcmp(what, "https://evil.example") == 0, "no-cors fetch from a web page -> Origin rejected, value reported");
    g = guard_of("POST /v1/position HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nOrigin: null\r\n\r\n", what, sizeof(what));
    CHECK(g == HTTP_GUARD_ORIGIN && strcmp(what, "null") == 0, "Origin: null (sandboxed page / redirect) rejected");
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\nOrigin: http://127.0.0.1:9999\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_ORIGIN,
        "another local port is a different origin");
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\norigin:http://evil.example\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_ORIGIN,
        "header name case-insensitive");
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\nOrigin: http://127.0.0.1:52734\r\nOrigin: https://evil.example\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_ORIGIN,
        "second Origin header is checked too");

    g = guard_of("GET /health HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nSec-Fetch-Site: cross-site\r\n\r\n", what, sizeof(what));
    CHECK(g == HTTP_GUARD_FETCH_SITE && strcmp(what, "cross-site") == 0, "Sec-Fetch-Site: cross-site rejected");
    CHECK(guard_of("GET /health HTTP/1.1\r\nSec-Fetch-Site: SAME-SITE\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_FETCH_SITE, "same-site rejected");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nSec-Fetch-Site: same-origin\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_OK,
        "same-origin passes");

    g = guard_of("GET /health HTTP/1.1\r\nHost: attacker.example:52734\r\n\r\n", what, sizeof(what));
    CHECK(g == HTTP_GUARD_HOST && strcmp(what, "attacker.example:52734") == 0, "DNS rebinding (foreign Host) rejected");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: 127.0.0.1.attacker.example\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_HOST, "look-alike host rejected");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: \r\n\r\n", what, sizeof(what)) == HTTP_GUARD_HOST, "empty Host rejected");
    CHECK(guard_of("GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: attacker.example\r\n\r\n", what, sizeof(what)) == HTTP_GUARD_HOST,
        "second Host header is checked too");

    /* Text that only LOOKS like headers, inside the body, must not matter. */
    CHECK(guard_of("POST /v1/position HTTP/1.1\r\nHost: 127.0.0.1:52734\r\nContent-Length: 30\r\n\r\n"
        "Origin: https://evil.example\r\n", what, sizeof(what)) == HTTP_GUARD_OK, "Origin text inside the body is just body");

    CHECK(strcmp(http_guard_header(HTTP_GUARD_ORIGIN), "Origin") == 0
        && strcmp(http_guard_header(HTTP_GUARD_FETCH_SITE), "Sec-Fetch-Site") == 0
        && strcmp(http_guard_header(HTTP_GUARD_HOST), "Host") == 0, "log header names");

    /* long values are truncated for the log but still rejected */
    char big[400];
    char req[600];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(req, sizeof(req), "POST /x HTTP/1.1\r\nOrigin: %s\r\n\r\n", big);
    CHECK(guard_of(req, what, sizeof(what)) == HTTP_GUARD_ORIGIN && strlen(what) < sizeof(what), "oversized Origin rejected, log value bounded");
}

static void test_throttle(void) {
    printf("[5] log throttle + suppressed counter\n");
    HttpThrottle t;
    int sup = -1;
    char txt[32];
    memset(&t, 0, sizeof(t));

    CHECK(http_throttle_pass(&t, 0, 1000, &sup) && sup == 0, "first line passes (even at tick 0)");
    CHECK(!http_throttle_pass(&t, 10, 1000, &sup) && sup == 0, "second line within the interval is swallowed");
    CHECK(!http_throttle_pass(&t, 500, 1000, &sup), "...still swallowed");
    CHECK(!http_throttle_pass(&t, 999, 1000, &sup), "...up to the interval");
    CHECK(http_throttle_pass(&t, 1000, 1000, &sup) && sup == 3, "passes after the interval and reports 3 suppressed");
    CHECK(http_throttle_pass(&t, 2500, 1000, &sup) && sup == 0, "nothing suppressed in between -> 0");

    /* 33 rejected POSTs per second for 10 s -> 10 lines, not 330 */
    int logged = 0;
    unsigned long long now;
    memset(&t, 0, sizeof(t));
    for (now = 5000; now < 15000; now += 30) {
        if (http_throttle_pass(&t, now, 1000, &sup)) {
            logged++;
        }
    }
    CHECK(logged >= 10 && logged <= 11, "mod at the main menu (33/s for 10 s) logs ~1 line per second");

    http_suppressed_text(txt, sizeof(txt), 0);
    CHECK(txt[0] == '\0', "no suffix when nothing was suppressed");
    http_suppressed_text(txt, sizeof(txt), 32);
    CHECK(strcmp(txt, " (+32 suppressed)") == 0, "suffix text");
}

static void test_accept_recovery(void) {
    printf("[6] accept() errors + rebind backoff\n");
    CHECK(http_wsa_error_is_transient(10054) && http_wsa_error_is_transient(10053)
        && http_wsa_error_is_transient(10004) && http_wsa_error_is_transient(10035),
        "WSAECONNRESET / ECONNABORTED / EINTR / EWOULDBLOCK are transient");
    CHECK(!http_wsa_error_is_transient(10038) && !http_wsa_error_is_transient(10022)
        && !http_wsa_error_is_transient(10048) && !http_wsa_error_is_transient(0),
        "WSAENOTSOCK / EINVAL / EADDRINUSE / 0 are not transient");

    CHECK(http_backoff_ms(0) == 250 && http_backoff_ms(1) == 250, "first retry after 250 ms (as before)");
    CHECK(http_backoff_ms(2) == 500 && http_backoff_ms(3) == 1000 && http_backoff_ms(4) == 2000
        && http_backoff_ms(5) == 4000, "doubling");
    CHECK(http_backoff_ms(6) == 5000 && http_backoff_ms(7) == 5000 && http_backoff_ms(1000) == 5000,
        "capped at 5 s");
    /* monotonic */
    int ok = 1;
    int i;
    for (i = 1; i < 40; i++) {
        if (http_backoff_ms(i + 1) < http_backoff_ms(i)) {
            ok = 0;
        }
    }
    CHECK(ok, "never decreases");
}

int main(void) {
    printf("http_request_test\n");
    test_header_block();
    test_content_length();
    test_host_and_origin();
    test_guard();
    test_throttle();
    test_accept_recovery();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
