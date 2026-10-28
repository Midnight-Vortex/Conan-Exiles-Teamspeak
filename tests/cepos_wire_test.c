/* Unit test for the CEPOS receive-side helpers (src/ts/proximity/ts3_cepos_wire.h).
   Header-only and pure, so this exercises the exact code the plugin ships.
   Build/run on Linux: bash tests/run_tests.sh
   Exit code 0 = all checks passed. */

#include "ts/proximity/ts3_cepos_wire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- control bytes ----------------------------------------------------------- */

static void test_control_bytes(void) {
    char name[17];
    int i;
    int anyControl;

    printf("[1] control bytes become '?'\n");

    memset(name, 0, sizeof(name));
    memcpy(name, "A\nB\rC\tD", 7);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(strcmp(name, "A?B?C?D") == 0, "newline, carriage return, tab replaced");

    memset(name, 0, sizeof(name));
    memcpy(name, "x\x1b[31my\x7fz", 9);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(strcmp(name, "x?[31my?z") == 0, "ESC and DEL replaced, printable rest intact");

    /* Every byte 0x01..0x1F and 0x7F, one at a time. */
    anyControl = 0;
    for (i = 1; i < 0x20; i++) {
        memset(name, 0, sizeof(name));
        name[0] = 'a';
        name[1] = (char)i;
        name[2] = 'b';
        cepos_wire_sanitize_name(name, sizeof(name));
        if (strcmp(name, "a?b") != 0) {
            anyControl = 1;
        }
    }
    memset(name, 0, sizeof(name));
    name[0] = 'a';
    name[1] = (char)0x7F;
    name[2] = 'b';
    cepos_wire_sanitize_name(name, sizeof(name));
    if (strcmp(name, "a?b") != 0) {
        anyControl = 1;
    }
    CHECK(!anyControl, "all of 0x01..0x1F and 0x7F are replaced");
}

/* ---- bytes that must survive -------------------------------------------------- */

static void test_untouched(void) {
    char name[17];

    printf("[2] legit names stay unchanged\n");

    memset(name, 0, sizeof(name));
    memcpy(name, "Conan_99 [Clan]", 15);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(strcmp(name, "Conan_99 [Clan]") == 0, "ASCII incl. space and brackets");

    memset(name, 0, sizeof(name));
    memcpy(name, "J\xC3\xB6rg \xE2\x80\x94 \xF0\x9F\x98\x80", 14);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(memcmp(name, "J\xC3\xB6rg \xE2\x80\x94 \xF0\x9F\x98\x80", 14) == 0, "UTF-8 multibyte bytes (>= 0x80) kept");

    memset(name, 0, sizeof(name));
    memcpy(name, " ~", 2);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(strcmp(name, " ~") == 0, "0x20 (space) and 0x7E (~) are the printable boundary");
}

/* ---- termination + bounds ----------------------------------------------------- */

static void test_bounds(void) {
    char name[17];
    char guard[8];

    printf("[3] stops at NUL, never reads past the buffer\n");

    memset(name, 0, sizeof(name));
    memcpy(name, "ab\0c\nd", 6);
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(memcmp(name, "ab\0c\nd", 6) == 0, "bytes after the first NUL are not touched");

    /* 16 payload bytes, caller-forced NUL at [16] like cepos_on_plugin_command. */
    memset(name, '\n', 16);
    name[16] = '\0';
    cepos_wire_sanitize_name(name, sizeof(name));
    CHECK(strlen(name) == 16 && name[0] == '?' && name[15] == '?', "full 16-byte name sanitized");

    /* `size` bounds the scan even without a terminator. */
    memset(guard, '\n', sizeof(guard));
    cepos_wire_sanitize_name(guard, 4);
    CHECK(guard[3] == '?' && guard[4] == '\n', "size limit respected (byte 4 untouched)");

    cepos_wire_sanitize_name(NULL, 10);
    CHECK(1, "NULL buffer is harmless");

    name[0] = '\n';
    cepos_wire_sanitize_name(name, 0);
    CHECK(name[0] == '\n', "size 0: nothing touched");
}

int main(void) {
    printf("cepos receive helper tests\n");
    test_control_bytes();
    test_untouched();
    test_bounds();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
