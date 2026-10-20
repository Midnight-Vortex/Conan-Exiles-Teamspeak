/* Unit tests for src/core/hub/mod_password.c (077).
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\mod_password_test.c src\core\hub\mod_password.c
        /Fe:tests\out\mod_password_test.exe */

#include "core/hub/mod_password.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Every log line is kept, so the tests can prove the password never shows up. */
static char g_log[4096];

static void log_capture(const char* fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    if (strlen(g_log) + strlen(line) + 2 < sizeof(g_log)) {
        strcat(g_log, line);
        strcat(g_log, "\n");
    }
}
void log_debug(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_capture(fmt, ap);
    va_end(ap);
}
void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_capture(fmt, ap);
    va_end(ap);
}

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* Runs take_from_body on a copy of text; returns the new length, body in buf. */
static int take(const char* text, char* buf, int cap, unsigned long long now) {
    strncpy(buf, text, (size_t)cap - 1);
    buf[cap - 1] = '\0';
    return mod_password_take_from_body(buf, (int)strlen(buf), now);
}

static void test_take(void) {
    printf("[1] take_from_body\n");
    char body[512], pw[MOD_PASSWORD_MAX];
    mod_password_clear();
    g_log[0] = '\0';

    const char* full = "SEQ=7 X=1,5 Y=2 Z=3 YAW=90 YAWY=0 OCC=3:40:T:0:1,2,3 TSPW=Ge heim!X=9";
    int n = take(full, body, sizeof(body), 1000);
    CHECK(strcmp(body, "SEQ=7 X=1,5 Y=2 Z=3 YAW=90 YAWY=0 OCC=3:40:T:0:1,2,3") == 0, "field cut off, rest intact");
    CHECK(n == (int)strlen(body), "returned length = new body length");
    CHECK(memchr(body, 'G', sizeof(body)) == NULL && strstr(body + n + 1, "heim") == NULL, "value wiped from the buffer");
    CHECK(mod_password_get(pw, sizeof(pw), 1000) && strcmp(pw, "Ge heim!X=9") == 0, "value with blank and X= kept whole");

    n = take("SEQ=1 X=1 Y=2 Z=3 YAW=0", body, sizeof(body), 1100);
    CHECK(n == (int)strlen("SEQ=1 X=1 Y=2 Z=3 YAW=0") && strcmp(body, "SEQ=1 X=1 Y=2 Z=3 YAW=0") == 0, "no field: body unchanged");
    CHECK(mod_password_get(pw, sizeof(pw), 1100) && strcmp(pw, "Ge heim!X=9") == 0, "no field: stored value kept");

    take("SEQ=1 X=1 OCC=XTSPW=abc", body, sizeof(body), 1200);
    CHECK(strcmp(body, "SEQ=1 X=1 OCC=XTSPW=abc") == 0, "marker inside another value does not match");

    take("TSPW=start", body, sizeof(body), 1300);
    CHECK(body[0] == '\0' && mod_password_get(pw, sizeof(pw), 1300) && strcmp(pw, "start") == 0, "marker at the very start");

    take("SEQ=1 TSPW=line\r\n", body, sizeof(body), 1400);
    CHECK(strcmp(body, "SEQ=1") == 0 && mod_password_get(pw, sizeof(pw), 1400) && strcmp(pw, "line") == 0, "trailing CR/LF trimmed");

    take("SEQ=1 TSPW=", body, sizeof(body), 1500);
    CHECK(strcmp(body, "SEQ=1") == 0 && !mod_password_get(pw, sizeof(pw), 1500) && pw[0] == '\0', "empty value clears");

    CHECK(strstr(g_log, "heim") == NULL && strstr(g_log, "start") == NULL && strstr(g_log, "line\n") == NULL
        && strstr(g_log, "len=11") != NULL, "log has the length, never the value");
}

static void test_store_rules(void) {
    printf("[2] store rules + TTL\n");
    char pw[MOD_PASSWORD_MAX], big[MOD_PASSWORD_MAX + 8];
    mod_password_clear();

    mod_password_store("abc", 3, 5000);
    CHECK(mod_password_get(pw, sizeof(pw), 5000 + MOD_PASSWORD_TTL_MS), "fresh up to the TTL");
    CHECK(!mod_password_get(pw, sizeof(pw), 5001 + MOD_PASSWORD_TTL_MS) && pw[0] == '\0', "stale after the TTL");
    mod_password_store("abc", 3, 20000);
    CHECK(mod_password_get(pw, sizeof(pw), 20000), "same value refreshes the time");
    CHECK(!mod_password_get(pw, sizeof(pw), 19999), "clock behind the stamp counts as stale");

    memset(big, 'x', sizeof(big));
    mod_password_store(big, MOD_PASSWORD_MAX, 21000);
    CHECK(mod_password_get(pw, sizeof(pw), 21000) && strcmp(pw, "abc") == 0, "too long ignored");
    mod_password_store(big, MOD_PASSWORD_MAX - 1, 21000);
    CHECK(mod_password_get(pw, sizeof(pw), 21000) && strlen(pw) == MOD_PASSWORD_MAX - 1, "longest allowed value kept");
    CHECK(!mod_password_get(pw, 4, 21000) && pw[0] == '\0', "too small out buffer refused");

    mod_password_store("ab\x01" "c", 4, 22000);
    CHECK(mod_password_get(pw, sizeof(pw), 22000) && pw[0] == 'x', "control character ignored");
    mod_password_store("\xc3\xa4pfel", 6, 22000);
    CHECK(mod_password_get(pw, sizeof(pw), 22000) && strcmp(pw, "\xc3\xa4pfel") == 0, "UTF-8 kept");

    mod_password_clear();
    CHECK(!mod_password_get(pw, sizeof(pw), 22000), "clear forgets it");
}

int main(void) {
    printf("mod_password_test\n");
    test_take();
    test_store_rules();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
