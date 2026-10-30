/* Unit tests for src/core/config/config_text.h + src/core/util/num_parse.h
   (plugin.cfg text parsing, clamp incl. NaN/Inf, decimal comma, UTF-8 / ANSI
   decision).  The Win32 parts of config.c (file IO, code pages, locking) are
   not host-testable; this runs the exact parse/clamp/validation code they use.
   Build/run on Linux: bash tests/run_tests.sh. Windows (MSVC Developer prompt):
     cl /nologo /O2 /I. /Isrc tests\config_text_test.c /Fe:tests\out\config_text_test.exe */

#include "core/config/config_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static void defaults(PluginConfig* c) {
    memset(c, 0, sizeof(*c));
    c->distanceWhisper = CFG_DEFAULT_DIST_WHISPER;
    c->distanceNormal = CFG_DEFAULT_DIST_NORMAL;
    c->distanceShout = CFG_DEFAULT_DIST_SHOUT;
    c->whisperKey = 97; c->normalKey = 98; c->shoutKey = 99;
    c->voiceToggleKey = 84; c->configUIKey = 121;
}

/* parse a (writable copy of a) wide text, then clamp like config_load does */
static void load(PluginConfig* c, const wchar_t* text) {
    static wchar_t buf[8192];
    defaults(c);
    wcsncpy(buf, text, 8191);
    buf[8191] = L'\0';
    cfg_parse_text(c, buf);
    cfg_clamp_values(c);
}

static void test_numbers(void) {
    printf("[1] numbers + decimal comma\n");
    PluginConfig c;

    load(&c, L"DistanceWhisper=2.5\nDistanceNormal=13.5\nDistanceShout=26\n");
    CHECK(c.distanceWhisper == 2.5f && c.distanceNormal == 13.5f && c.distanceShout == 26.0f, "dot decimals as before");

    load(&c, L"DistanceWhisper=2,5\r\nDistanceNormal=13,5\r\nDistanceShout=26,25\r\n");
    CHECK(c.distanceWhisper == 2.5f && c.distanceNormal == 13.5f && c.distanceShout == 26.25f,
        "comma decimals: 13,5 -> 13.5 (was 13)");

    load(&c, L"DistanceNormal=1,000.5\n");
    CHECK(c.distanceNormal == 1.0f, "comma AND dot: old result (1)");
    load(&c, L"DistanceNormal=7,2,1\n");
    CHECK(c.distanceNormal == 7.0f, "two commas: old result (7)");

    load(&c, L"DistanceNormal=abc\nDistanceShout=,5\n");
    CHECK(c.distanceNormal == 0.5f && c.distanceShout == 0.5f, "typos: parse -> 0 -> clamp 0.5 (unchanged fallback)");

    load(&c, L"WhisperKey=100\nNormalKey=101\nShoutKey=102\nVoiceToggleKey=70\nConfigUIKey=122\nHudTheme=3\nHudPosition=2\nHudSize=1\n");
    CHECK(c.whisperKey == 100 && c.normalKey == 101 && c.shoutKey == 102 && c.voiceToggleKey == 70
        && c.configUIKey == 122 && c.hudTheme == 3 && c.hudPosition == 2 && c.hudSize == 1, "integers");
    load(&c, L"WhisperKey=13,5\n");
    CHECK(c.whisperKey == 13, "integer keys keep atoi semantics (13)");
    load(&c, L"WhisperKey=99999999999999\nNormalKey=-5\nHudTheme=99\n");
    CHECK(c.whisperKey == 97 && c.normalKey == 98 && c.hudTheme == 11, "out-of-range ints -> default / clamp");
}

static void test_range_unit_key(void) {
    printf("[1b] ShowRangesInFoundations (091): bool key, default off, 0/1 after clamp\n");
    PluginConfig c;

    load(&c, L"DistanceNormal=13.0\n");
    CHECK(c.rangeInFoundations == 0, "absent key -> meters (default off)");
    load(&c, L"ShowRangesInFoundations=true\n");
    CHECK(c.rangeInFoundations == 1, "true -> foundations");
    load(&c, L"ShowRangesInFoundations=false\n");
    CHECK(c.rangeInFoundations == 0, "false -> meters");
    load(&c, L"showrangesinfoundations = TRUE\r\n");
    CHECK(c.rangeInFoundations == 1, "key and value are case-insensitive, spaces and CRLF are fine");
    load(&c, L"ShowRangesInFoundations=1\n");
    CHECK(c.rangeInFoundations == 1, "1 -> foundations");
    load(&c, L"ShowRangesInFoundations=0\n");
    CHECK(c.rangeInFoundations == 0, "0 -> meters");
    load(&c, L"ShowRangesInFoundations=yes\n");
    CHECK(c.rangeInFoundations == 0, "anything that is not true/1 -> meters (same rule as every bool)");

    /* the key does not disturb its neighbours */
    load(&c, L"DistanceShout=26.0\nShowRangesInFoundations=true\nWhisperKey=100\n");
    CHECK(c.rangeInFoundations == 1 && c.distanceShout == 26.0f && c.whisperKey == 100,
        "neighbouring keys still parsed");
    /* stored distances are meters whatever the unit */
    load(&c, L"ShowRangesInFoundations=true\nDistanceNormal=15.4\n");
    CHECK(c.distanceNormal == 15.4f, "distances are meters even in foundation mode");

    /* clamp normalises to 0/1 (a bool field must not carry 7 or -3 around) */
    defaults(&c);
    c.rangeInFoundations = 7;
    cfg_clamp_values(&c);
    CHECK(c.rangeInFoundations == 1, "clamp: 7 -> 1");
    c.rangeInFoundations = -3;
    cfg_clamp_values(&c);
    CHECK(c.rangeInFoundations == 1, "clamp: -3 -> 1");
    c.rangeInFoundations = 0;
    cfg_clamp_values(&c);
    CHECK(c.rangeInFoundations == 0, "clamp: 0 stays 0");
}

static void test_clamp_nonfinite(void) {
    printf("[2] clamp: NaN / Inf distances\n");
    PluginConfig c;

    load(&c, L"DistanceWhisper=nan\nDistanceNormal=inf\nDistanceShout=-inf\n");
    CHECK(c.distanceWhisper == CFG_DEFAULT_DIST_WHISPER && c.distanceNormal == CFG_DEFAULT_DIST_NORMAL
        && c.distanceShout == CFG_DEFAULT_DIST_SHOUT, "nan / inf / -inf in the file -> defaults (3 / 13 / 26)");

    load(&c, L"DistanceNormal=1e999\n");
    CHECK(c.distanceNormal == CFG_DEFAULT_DIST_NORMAL, "1e999 (overflow to inf) -> default");

    defaults(&c);
    c.distanceWhisper = NAN;
    c.distanceNormal = INFINITY;
    c.distanceShout = -INFINITY;
    cfg_clamp_values(&c);
    CHECK(isfinite(c.distanceWhisper) && c.distanceWhisper == 3.0f && c.distanceNormal == 13.0f && c.distanceShout == 26.0f,
        "clamp on a struct (settings dialog path)");

    defaults(&c);
    c.distanceWhisper = 0.0f; c.distanceNormal = 9999.0f; c.distanceShout = -3.0f;
    cfg_clamp_values(&c);
    CHECK(c.distanceWhisper == 0.5f && c.distanceNormal == 500.0f && c.distanceShout == 0.5f, "finite values still clamp to 0.5..500");

    defaults(&c);
    c.distanceNormal = 13.5f;
    cfg_clamp_values(&c);
    CHECK(c.distanceNormal == 13.5f, "in-range value untouched");
}

static void test_lines_and_strings(void) {
    printf("[3] lines, bools, strings\n");
    PluginConfig c;

    load(&c, L"# comment\n; comment\n\nno equals here\nAutomaticPatchFind=true\nEnableVoiceToggle=1\nDebugMode=False\n"
        L"EnablePosFile=TRUE\n  EnableVoiceOverlay =  true  \n");
    CHECK(c.automaticPatchFind == 1 && c.enableVoiceToggle == 1 && c.debugMode == 0 && c.enablePosFile == 1
        && c.enableVoiceOverlay == 1, "comments skipped, bools, trimming, case-insensitive values");
    load(&c, L"debugmode=true\nDISTANCENORMAL=20\n");
    CHECK(c.debugMode == 1 && c.distanceNormal == 20.0f, "keys case-insensitive");

    load(&c, L"SavedPath=C:\\Games\\Conan Exiles\\ConanSandbox\\Saved\r\nAutomaticSavedPath=D:\\Steam\\x\r\n");
    CHECK(wcscmp(c.savedPath, L"C:\\Games\\Conan Exiles\\ConanSandbox\\Saved") == 0
        && wcscmp(c.automaticSavedPath, L"D:\\Steam\\x") == 0, "paths, CRLF trimmed");

    /* non-ASCII path survives the parse (Cyrillic / CJK Steam folders) */
    load(&c, L"SavedPath=D:\\\u0418\u0433\u0440\u044B\\\u6E38\u620F\\Saved\n");
    CHECK(wcscmp(c.savedPath, L"D:\\\u0418\u0433\u0440\u044B\\\u6E38\u620F\\Saved") == 0, "non-ASCII path kept");

    /* a line longer than the old 1024-char fgetws buffer is not split in two */
    {
        static wchar_t big[3000];
        wcscpy(big, L"SavedPath=");
        size_t n = wcslen(big);
        size_t i;
        for (i = 0; i < 1500; i++) {
            big[n + i] = L'a';
        }
        wcscpy(big + n + i, L"\nDistanceNormal=18\n");
        load(&c, big);
        CHECK(wcslen(c.savedPath) == CONFIG_MAX_PATH - 1 && c.distanceNormal == 18.0f,
            "overlong path truncated, next line still read");
    }

    load(&c, L"DefaultsAppliedServer=abc/DEF+123=\n");
    CHECK(strcmp(c.defaultsAppliedServer, "abc/DEF+123=") == 0, "server uid");
    load(&c, L"DefaultsAppliedServer=ab\u00e9c\n");
    CHECK(strcmp(c.defaultsAppliedServer, "abc") == 0, "non-ASCII dropped from the uid (as before)");

    load(&c, L"SavedPath=no newline at the end");
    CHECK(wcscmp(c.savedPath, L"no newline at the end") == 0, "last line without newline");
    load(&c, L"");
    CHECK(c.distanceNormal == 13.0f, "empty text keeps defaults");
}

static void test_utf8(void) {
    printf("[4] UTF-8 / ANSI decision\n");
#define V(s) cfg_utf8_is_valid((const unsigned char*)(s), sizeof(s) - 1)
    CHECK(V("SavedPath=C:\\Games\r\n"), "ASCII is valid");
    CHECK(V(""), "empty is valid");
    CHECK(V("D:\\M\xC3\xBCller"), "ue (C3 BC)");
    CHECK(V("\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B"), "Cyrillic");
    CHECK(V("\xE6\xB8\xB8\xE6\x88\x8F"), "CJK (3-byte)");
    CHECK(V("\xF0\x9F\x98\x80"), "4-byte (emoji)");
    CHECK(V("\xEF\xBB\xBF" "abc"), "BOM + ASCII");

    CHECK(!V("D:\\M\xFC" "ller"), "lone FC (cp1252 ue) is NOT UTF-8 -> ANSI");
    CHECK(!V("\xE4\xF6\xFC"), "cp1252 umlauts");
    CHECK(!V("\xC0\x80"), "overlong NUL");
    CHECK(!V("\xC1\xBF"), "overlong 2-byte");
    CHECK(!V("\xE0\x80\x80"), "overlong 3-byte");
    CHECK(!V("\xED\xA0\x80"), "UTF-16 surrogate");
    CHECK(!V("\xF4\x90\x80\x80"), "above U+10FFFF");
    CHECK(!V("\xF5\x80\x80\x80"), "F5 lead");
    CHECK(!V("\x80"), "stray continuation");
    CHECK(!V("abc\xE2\x82"), "truncated 3-byte at the end");
    CHECK(!V("abc\xC3"), "truncated 2-byte at the end");
    CHECK(!V("\xC3\x28"), "bad continuation");
    CHECK(!V("\xFF\xFE"), "FF FE");
#undef V

    /* writer side: ANSI form only if the reader will decode it as ANSI again */
#define U(s) cfg_ansi_bytes_are_unambiguous((const unsigned char*)(s), sizeof(s) - 1)
    CHECK(U("plain ascii"), "ASCII: unambiguous");
    CHECK(U("M\xFC" "ller"), "cp1252 ue: unambiguous (not UTF-8) -> ANSI file ok");
    CHECK(!U("M\xC3\xBC" "ller"), "bytes that are valid UTF-8 would be misread -> write UTF-8 instead");
#undef U
    CHECK(cfg_has_non_ascii((const unsigned char*)"a\xC3\xBC", 3) && !cfg_has_non_ascii((const unsigned char*)"abc", 3),
        "non-ASCII detection");
}

static void test_num_parse(void) {
    printf("[5] num_strtod shapes\n");
    CHECK(num_strtod("13.5") == 13.5, "13.5");
    CHECK(num_strtod("  13,5") == 13.5, "leading blanks + comma");
    CHECK(num_strtod("0,25") == 0.25, "0,25");
    CHECK(num_strtod("1,5e2") == 150.0, "1,5e2");
    CHECK(num_strtod("-2,5") == -2.5, "negative");
    CHECK(num_strtod("13,5 meters") == 13.5, "trailing text ignored like strtod");
    CHECK(num_strtod("13,") == 13.0 && num_strtod(",5") == 0.0 && num_strtod("a,5") == 0.0, "no digit on one side: strtod result");
    CHECK(num_strtod("1,2,3") == 1.0, "list: first number only (as before)");
    CHECK(num_strtod("") == 0.0 && num_strtod(NULL) == 0.0, "empty / NULL");
    CHECK(num_strtod("5") == 5.0 && num_strtod("-0") == 0.0, "integers");
    {
        char longv[200];
        memset(longv, '1', sizeof(longv) - 1);
        longv[sizeof(longv) - 1] = '\0';
        longv[70] = ',';
        CHECK(num_strtod(longv) == strtod(longv, NULL), "very long token: no copy-buffer overflow, plain strtod result");
    }
}

int main(void) {
    printf("config_text_test\n");
    test_numbers();
    test_range_unit_key();
    test_clamp_nonfinite();
    test_lines_and_strings();
    test_utf8();
    test_num_parse();
    printf(g_failures ? "RESULT: %d FAILED\n" : "RESULT: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
