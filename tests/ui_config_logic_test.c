/* Unit test for src/ui/config/ui_config_logic.h — the pure helpers of the F10
   settings dialog (087 review fixes):
     [1] ui_wcs_copy            bounded copy (no CRT invalid-parameter abort)
     [2] ui_game_folder_fits    "\ConanSandbox\Saved" must still fit in MAX_PATH
     [3] ui_dist_parse          strict distance parsing (empty / nan / limits)
     [4] ui_preset_*            voice_presets.cfg line validation
     [5] ui_cfg_merge_*         which PluginConfig fields the dialog commits

   The header is Win32-free so it builds with plain gcc on Linux:
     bash tests/run_tests.sh   (exit code 0 = all checks passed) */

#include "ui/config/ui_config_logic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

#define MAX_PRESETS 10

/* ---- [1] ui_wcs_copy -------------------------------------------------------- */

static void test_wcs_copy(void) {
    printf("[1] ui_wcs_copy: bounded, always NUL-terminated\n");

    /* Destination with guard words on both sides to detect overruns. */
    struct { wchar_t guardLo[4]; wchar_t buf[8]; wchar_t guardHi[4]; } g;
    for (int i = 0; i < 4; i++) { g.guardLo[i] = L'#'; g.guardHi[i] = L'#'; }

    int fit = ui_wcs_copy(g.buf, 8, L"abc");
    CHECK(fit == 1 && wcscmp(g.buf, L"abc") == 0, "short string fits");

    fit = ui_wcs_copy(g.buf, 8, L"1234567");
    CHECK(fit == 1 && wcscmp(g.buf, L"1234567") == 0, "exact fit (cap-1 chars + NUL)");

    fit = ui_wcs_copy(g.buf, 8, L"12345678");
    CHECK(fit == 0 && wcscmp(g.buf, L"1234567") == 0, "one char too long -> truncated, NUL kept");

    fit = ui_wcs_copy(g.buf, 8, L"this string is far longer than the buffer");
    CHECK(fit == 0 && wcslen(g.buf) == 7 && wcsncmp(g.buf, L"this st", 7) == 0, "long string -> truncated");

    int guardsIntact = 1;
    for (int i = 0; i < 4; i++) {
        if (g.guardLo[i] != L'#' || g.guardHi[i] != L'#') guardsIntact = 0;
    }
    CHECK(guardsIntact, "no write outside the destination");

    fit = ui_wcs_copy(g.buf, 1, L"abc");
    CHECK(fit == 0 && g.buf[0] == L'\0', "cap 1 -> empty string");

    g.buf[0] = L'x';
    fit = ui_wcs_copy(g.buf, 0, L"abc");
    CHECK(fit == 0 && g.buf[0] == L'x', "cap 0 -> destination untouched");

    fit = ui_wcs_copy(g.buf, 8, NULL);
    CHECK(fit == 0 && g.buf[0] == L'\0', "NULL source -> empty string");

    fit = ui_wcs_copy(NULL, 8, L"abc");
    CHECK(fit == 0, "NULL destination -> 0, no crash");

    /* The real scenario: a plugin.cfg path of 400 chars into a MAX_PATH buffer. */
    wchar_t longPath[401];
    for (int i = 0; i < 400; i++) longPath[i] = L'a';
    longPath[400] = L'\0';
    wchar_t small[260];
    fit = ui_wcs_copy(small, 260, longPath);
    CHECK(fit == 0 && wcslen(small) == 259, "400-char path into 260 buffer -> 259 chars, no abort");
}

/* ---- [2] ui_game_folder_fits ------------------------------------------------ */

static void test_game_folder_fits(void) {
    printf("[2] ui_game_folder_fits: room for \\ConanSandbox\\Saved\n");

    CHECK(wcslen(UI_SAVED_SUBPATH) == (size_t)UI_SAVED_SUBPATH_LEN, "suffix length constant is 19");

    wchar_t folder[300];
    for (int i = 0; i < 299; i++) folder[i] = L'a';

    folder[240] = L'\0';
    CHECK(ui_game_folder_fits(folder, 260) == 1, "240 chars + suffix (19) + NUL = 260 -> fits");
    folder[240] = L'a';
    folder[241] = L'\0';
    CHECK(ui_game_folder_fits(folder, 260) == 0, "241 chars -> does not fit");
    folder[241] = L'a';
    folder[259] = L'\0';
    CHECK(ui_game_folder_fits(folder, 260) == 0, "259 chars (max a MAX_PATH buffer holds) -> does not fit");
    CHECK(ui_game_folder_fits(L"C:\\Games\\Conan Exiles", 260) == 1, "normal path fits");
    CHECK(ui_game_folder_fits(L"", 260) == 1, "empty folder fits (caller rejects empty separately)");
    CHECK(ui_game_folder_fits(NULL, 260) == 0, "NULL -> 0");

    /* Prove the claim by really building the string with the bounded copy. */
    wchar_t full[260];
    folder[240] = L'\0';
    ui_wcs_copy(full, 260, folder);
    int ok = ui_wcs_copy(full + wcslen(full), 260 - wcslen(full), UI_SAVED_SUBPATH);
    CHECK(ok == 1 && wcslen(full) == 259, "240-char folder + suffix = exactly 259 chars");
}

/* ---- [3] ui_dist_parse ------------------------------------------------------ */

static UiDistStatus parse(const wchar_t* t, float* out) {
    return ui_dist_parse(t, 0, 0.0f, 0.0f, out);
}

static void test_dist_parse_valid(void) {
    printf("[3a] ui_dist_parse: valid input\n");
    float v = -1.0f;

    CHECK(parse(L"2", &v) == UI_DIST_OK && v == 2.0f, "\"2\" -> 2");
    CHECK(parse(L"2.5", &v) == UI_DIST_OK && fabsf(v - 2.5f) < 1e-6f, "\"2.5\" -> 2.5");
    CHECK(parse(L"15.0", &v) == UI_DIST_OK && v == 15.0f, "\"15.0\" -> 15");
    CHECK(parse(L"  15.0  ", &v) == UI_DIST_OK && v == 15.0f, "surrounding blanks allowed");
    CHECK(parse(L"+3", &v) == UI_DIST_OK && v == 3.0f, "\"+3\" -> 3");
    CHECK(parse(L".5", &v) == UI_DIST_OK && fabsf(v - 0.5f) < 1e-6f, "\".5\" -> 0.5");
    CHECK(parse(L"3.", &v) == UI_DIST_OK && v == 3.0f, "\"3.\" (typed so far) -> 3");
    CHECK(parse(L"2,5", &v) == UI_DIST_OK && fabsf(v - 2.5f) < 1e-6f, "comma decimal separator -> 2.5");
    CHECK(parse(L"150", &v) == UI_DIST_OK && v == 150.0f, "\"150\" -> 150");
    CHECK(parse(L"0.1", &v) == UI_DIST_OK && fabsf(v - 0.1f) < 1e-6f, "\"0.1\" -> 0.1");

    /* Round trip of the text the dialog writes itself ("%.1f"). */
    int roundTripOk = 1;
    const float samples[] = { 0.5f, 2.0f, 3.0f, 13.0f, 26.0f, 50.0f, 123.4f, 500.0f };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        wchar_t text[32];
        swprintf(text, 32, L"%.1f", samples[i]);
        float back = 0.0f;
        if (parse(text, &back) != UI_DIST_OK || fabsf(back - samples[i]) > 1e-4f) roundTripOk = 0;
    }
    CHECK(roundTripOk, "\"%.1f\" output of the dialog reads back");
}

static void test_dist_parse_invalid(void) {
    printf("[3b] ui_dist_parse: invalid input is rejected, *out untouched\n");
    float v = 42.0f;

    CHECK(parse(L"", &v) == UI_DIST_EMPTY, "empty -> EMPTY");
    CHECK(parse(L"   ", &v) == UI_DIST_EMPTY, "blanks only -> EMPTY");
    CHECK(parse(NULL, &v) == UI_DIST_EMPTY, "NULL -> EMPTY");
    CHECK(parse(L"abc", &v) == UI_DIST_NOT_NUMBER, "\"abc\" -> NOT_NUMBER");
    CHECK(parse(L"nan", &v) == UI_DIST_NOT_NUMBER, "\"nan\" -> NOT_NUMBER");
    CHECK(parse(L"NaN", &v) == UI_DIST_NOT_NUMBER, "\"NaN\" -> NOT_NUMBER");
    CHECK(parse(L"inf", &v) == UI_DIST_NOT_NUMBER, "\"inf\" -> NOT_NUMBER");
    CHECK(parse(L"-inf", &v) == UI_DIST_NOT_NUMBER, "\"-inf\" -> NOT_NUMBER");
    CHECK(parse(L"1e5", &v) == UI_DIST_NOT_NUMBER, "exponent -> NOT_NUMBER");
    CHECK(parse(L"0x10", &v) == UI_DIST_NOT_NUMBER, "hex -> NOT_NUMBER");
    CHECK(parse(L"2.5.1", &v) == UI_DIST_NOT_NUMBER, "two dots -> NOT_NUMBER");
    CHECK(parse(L"--3", &v) == UI_DIST_NOT_NUMBER, "double sign -> NOT_NUMBER");
    CHECK(parse(L".", &v) == UI_DIST_NOT_NUMBER, "lone dot -> NOT_NUMBER");
    CHECK(parse(L"-", &v) == UI_DIST_NOT_NUMBER, "lone minus -> NOT_NUMBER");
    CHECK(parse(L"1 2", &v) == UI_DIST_NOT_NUMBER, "inner blank -> NOT_NUMBER");
    CHECK(parse(L"15m", &v) == UI_DIST_NOT_NUMBER, "trailing unit -> NOT_NUMBER");

    /* 100 digits overflow float -> must not become inf. */
    wchar_t huge[101];
    for (int i = 0; i < 100; i++) huge[i] = L'9';
    huge[100] = L'\0';
    CHECK(parse(huge, &v) == UI_DIST_NOT_NUMBER, "100 digits (float overflow) -> NOT_NUMBER");

    CHECK(parse(L"0", &v) == UI_DIST_NOT_POSITIVE, "\"0\" -> NOT_POSITIVE");
    CHECK(parse(L"0.0", &v) == UI_DIST_NOT_POSITIVE, "\"0.0\" -> NOT_POSITIVE");
    CHECK(parse(L"-5", &v) == UI_DIST_NOT_POSITIVE, "\"-5\" -> NOT_POSITIVE");
    CHECK(parse(L"-0", &v) == UI_DIST_NOT_POSITIVE, "\"-0\" -> NOT_POSITIVE");

    CHECK(v == 42.0f, "*out was never written on failure");
    CHECK(ui_dist_parse(L"2", 0, 0.0f, 0.0f, NULL) == UI_DIST_OK, "NULL out pointer is allowed");
}

static void test_dist_parse_limits(void) {
    printf("[3c] ui_dist_parse: server (hub) limits\n");
    float v = 0.0f;

    /* Limits active, min 5 / max 15. */
    CHECK(ui_dist_parse(L"4.9", 1, 5.0f, 15.0f, &v) == UI_DIST_OUT_OF_RANGE, "4.9 below min 5");
    CHECK(ui_dist_parse(L"5", 1, 5.0f, 15.0f, &v) == UI_DIST_OK && v == 5.0f, "5 == min -> ok");
    CHECK(ui_dist_parse(L"15", 1, 5.0f, 15.0f, &v) == UI_DIST_OK && v == 15.0f, "15 == max -> ok");
    CHECK(ui_dist_parse(L"15.1", 1, 5.0f, 15.0f, &v) == UI_DIST_OUT_OF_RANGE, "15.1 above max 15");
    CHECK(ui_dist_parse(L"10", 1, 5.0f, 15.0f, &v) == UI_DIST_OK && v == 10.0f, "10 inside range");

    /* The limit itself is a float (e.g. 15.1f); the same text must be accepted. */
    CHECK(ui_dist_parse(L"15.1", 1, 15.1f, 50.0f, &v) == UI_DIST_OK, "text equal to a float limit is accepted");
    CHECK(ui_dist_parse(L"50.1", 1, 15.1f, 50.1f, &v) == UI_DIST_OK, "text equal to a float max is accepted");

    /* A limit <= 0 counts as "not set" (same rule as voice_mode_get_distance). */
    CHECK(ui_dist_parse(L"1", 1, 0.0f, 15.0f, &v) == UI_DIST_OK, "min 0 = not set");
    CHECK(ui_dist_parse(L"500", 1, 5.0f, 0.0f, &v) == UI_DIST_OK, "max 0 = not set");
    CHECK(ui_dist_parse(L"1", 1, 5.0f, 0.0f, &v) == UI_DIST_OUT_OF_RANGE, "min only: below");

    /* Limits inactive: range ignored, positivity still required. */
    CHECK(ui_dist_parse(L"1", 0, 5.0f, 15.0f, &v) == UI_DIST_OK && v == 1.0f, "limits off: below min ok");
    CHECK(ui_dist_parse(L"99", 0, 5.0f, 15.0f, &v) == UI_DIST_OK && v == 99.0f, "limits off: above max ok");
    CHECK(ui_dist_parse(L"0", 0, 5.0f, 15.0f, &v) == UI_DIST_NOT_POSITIVE, "limits off: 0 still refused");
}

/* ---- [3d] 091: a distance edit read in the display unit ---------------------- */

static void test_dist_parse_display(void) {
    printf("[3d] ui_dist_parse_display: meters or foundations, no drift on Save\n");
    float out = -1.0f;

    /* NO DRIFT: 40 m is shown as "15.6" foundations; leaving the field alone keeps 40 m */
    CHECK(ui_dist_parse_display(L"15.6", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_OK && out == 40.0f,
        "foundations: unchanged text keeps the exact 40 m (not 39.936)");
    CHECK(ui_dist_parse_display(L"15.60", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_OK && out == 40.0f,
        "foundations: '15.60' reads as the same number -> exact 40 m");
    CHECK(ui_dist_parse_display(L"5,9", 1, 15.0f, 0, 0, 0, &out) == UI_DIST_OK && out == 15.0f,
        "foundations: decimal comma, 15 m shown as 5.9 -> exact 15 m");
    CHECK(ui_dist_parse_display(L"13.0", 0, 13.0f, 0, 0, 0, &out) == UI_DIST_OK && out == 13.0f,
        "meters: unchanged -> exact");
    CHECK(ui_dist_parse_display(L"13", 0, 13.04f, 0, 0, 0, &out) == UI_DIST_OK && out == 13.04f,
        "meters: 13.04 shown as 13.0, typed '13' -> the stored 13.04 is kept");

    /* a real change converts the typed number */
    CHECK(ui_dist_parse_display(L"6.5", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_OK && fabsf(out - 16.64f) < 0.0001f,
        "foundations: typed 6.5 -> 16.64 m");
    CHECK(ui_dist_parse_display(L"1", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_OK && fabsf(out - 2.56f) < 0.0001f,
        "foundations: typed 1 -> 2.56 m");
    CHECK(ui_dist_parse_display(L"16", 1, 3.0f, 0, 0, 0, &out) == UI_DIST_OK && fabsf(out - 40.96f) < 0.0001f,
        "foundations: typed 16 -> 40.96 m");
    CHECK(ui_dist_parse_display(L"25.5", 0, 13.0f, 0, 0, 0, &out) == UI_DIST_OK && out == 25.5f,
        "meters: typed 25.5 -> 25.5 (identity, as before the feature)");

    /* strictness is the same as ui_dist_parse, in both units */
    CHECK(ui_dist_parse_display(L"", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_EMPTY, "empty");
    CHECK(ui_dist_parse_display(L"abc", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_NOT_NUMBER, "abc");
    CHECK(ui_dist_parse_display(L"nan", 0, 40.0f, 0, 0, 0, &out) == UI_DIST_NOT_NUMBER, "nan");
    CHECK(ui_dist_parse_display(L"0", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_NOT_POSITIVE, "0");
    CHECK(ui_dist_parse_display(L"-2", 1, 40.0f, 0, 0, 0, &out) == UI_DIST_NOT_POSITIVE, "-2");
    out = 123.0f;
    ui_dist_parse_display(L"abc", 1, 40.0f, 0, 0, 0, &out);
    CHECK(out == 123.0f, "*out untouched on every error");

    /* server limits are checked in the DISPLAY unit: 15 m max = 5.8 F, 5 m min = 2.0 F */
    CHECK(ui_dist_parse_display(L"5.8", 1, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OK
        && fabsf(out - 14.848f) < 0.001f && out <= 15.0f, "foundations: shown max 5.8 is accepted and is <= 15 m");
    CHECK(ui_dist_parse_display(L"5.9", 1, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OUT_OF_RANGE,
        "foundations: 5.9 F = 15.1 m is over the max");
    CHECK(ui_dist_parse_display(L"2.0", 1, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OK
        && out >= 5.0f, "foundations: shown min 2.0 is accepted and is >= 5 m");
    CHECK(ui_dist_parse_display(L"1.9", 1, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OUT_OF_RANGE,
        "foundations: 1.9 F = 4.86 m is under the min");
    CHECK(ui_dist_parse_display(L"0.9", 1, 10.0f, 1, 2.1f, 15.0f, &out) == UI_DIST_OK && out >= 2.1f,
        "min 2.1 m (0.82 F): 0.9 is the first valid value");
    CHECK(ui_dist_parse_display(L"0.8", 1, 10.0f, 1, 2.1f, 15.0f, &out) == UI_DIST_OUT_OF_RANGE,
        "min 2.1 m: 0.8 F = 2.048 m is below the limit");
    CHECK(ui_dist_parse_display(L"4.9", 0, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OUT_OF_RANGE
        && ui_dist_parse_display(L"5.0", 0, 10.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OK && out == 5.0f,
        "meters: limits exactly as ui_dist_parse");
    CHECK(ui_dist_parse_display(L"500", 1, 10.0f, 0, 5.0f, 15.0f, &out) == UI_DIST_OK,
        "limits inactive: not checked");
    CHECK(ui_dist_parse_display(L"100", 1, 10.0f, 1, 0.0f, 0.0f, &out) == UI_DIST_OK,
        "limits active but unset (0): not checked");

    /* unchanged text is validated against the limits in METERS, not in rounded tenths */
    CHECK(ui_dist_parse_display(L"0.8", 1, 2.1f, 1, 2.1f, 15.0f, &out) == UI_DIST_OK && out == 2.1f,
        "stored 2.1 m (== min) shows as 0.8 F, unchanged: still valid, kept exactly");
    CHECK(ui_dist_parse_display(L"6.0", 1, 15.36f, 1, 5.0f, 15.36f, &out) == UI_DIST_OK && out == 15.36f,
        "stored value == max in meters, unchanged: valid");
    CHECK(ui_dist_parse_display(L"7.8", 1, 20.0f, 1, 5.0f, 15.0f, &out) == UI_DIST_OUT_OF_RANGE,
        "stored value above the max (server changed the limit), unchanged: reported, not silently kept");

    /* the file round trip never invents a change: display -> meters -> 0.1 m file -> display */
    CHECK(ui_dist_parse_display(L"5.9", 1, 15.1f, 0, 0, 0, &out) == UI_DIST_OK && out == 15.1f,
        "15.1 m (what a typed 5.9 F becomes in plugin.cfg) shows as 5.9 and stays 15.1");
}

/* ---- [4] voice_presets.cfg validation --------------------------------------- */

static void test_preset_header(void) {
    printf("[4a] ui_preset_parse_header: [PresetN]\n");
    int idx = -99;

    CHECK(ui_preset_parse_header(L"[Preset0]", MAX_PRESETS, &idx) == 1 && idx == 0, "[Preset0] -> 0");
    CHECK(ui_preset_parse_header(L"[Preset9]", MAX_PRESETS, &idx) == 1 && idx == 9, "[Preset9] -> 9");
    CHECK(ui_preset_parse_header(L"[Preset3]  ", MAX_PRESETS, &idx) == 1 && idx == 3, "trailing blanks ok");

    idx = -99;
    CHECK(ui_preset_parse_header(L"[Preset10]", MAX_PRESETS, &idx) == 0, "[Preset10] out of range");
    CHECK(ui_preset_parse_header(L"[Preset-1]", MAX_PRESETS, &idx) == 0, "[Preset-1] rejected");
    CHECK(ui_preset_parse_header(L"[PresetX]", MAX_PRESETS, &idx) == 0, "[PresetX] rejected (was preset 0 before!)");
    CHECK(ui_preset_parse_header(L"[Preset]", MAX_PRESETS, &idx) == 0, "[Preset] rejected (was preset 0 before!)");
    CHECK(ui_preset_parse_header(L"[Preset3", MAX_PRESETS, &idx) == 0, "missing ']' rejected");
    CHECK(ui_preset_parse_header(L"[Preset3]junk", MAX_PRESETS, &idx) == 0, "text after ']' rejected");
    CHECK(ui_preset_parse_header(L"[Preset 3]", MAX_PRESETS, &idx) == 0, "blank inside rejected");
    CHECK(ui_preset_parse_header(L"[Preset99999999999]", MAX_PRESETS, &idx) == 0, "huge index rejected");
    CHECK(ui_preset_parse_header(L"[Other3]", MAX_PRESETS, &idx) == 0, "other section rejected");
    CHECK(ui_preset_parse_header(L"", MAX_PRESETS, &idx) == 0, "empty line rejected");
    CHECK(ui_preset_parse_header(NULL, MAX_PRESETS, &idx) == 0, "NULL rejected");
    CHECK(idx == -99, "*outIndex untouched on failure");
}

static void test_preset_values(void) {
    printf("[4b] preset distances / keys / CurrentPreset\n");

    float d = 7.0f;
    CHECK(ui_preset_parse_distance(L"2.5", &d) == 1 && fabsf(d - 2.5f) < 1e-6f, "2.5 accepted");
    d = 7.0f;
    CHECK(ui_preset_parse_distance(L"nan", &d) == 0 && d == 7.0f, "nan -> kept");
    CHECK(ui_preset_parse_distance(L"inf", &d) == 0 && d == 7.0f, "inf -> kept");
    CHECK(ui_preset_parse_distance(L"abc", &d) == 0 && d == 7.0f, "abc -> kept");
    CHECK(ui_preset_parse_distance(L"", &d) == 0 && d == 7.0f, "empty -> kept");
    CHECK(ui_preset_parse_distance(L"0", &d) == 1 && d == UI_PRESET_DIST_MIN, "0 -> clamped to 0.5");
    CHECK(ui_preset_parse_distance(L"-3", &d) == 1 && d == UI_PRESET_DIST_MIN, "-3 -> clamped to 0.5");
    CHECK(ui_preset_parse_distance(L"0.1", &d) == 1 && d == UI_PRESET_DIST_MIN, "0.1 -> clamped to 0.5");
    CHECK(ui_preset_parse_distance(L"9999", &d) == 1 && d == UI_PRESET_DIST_MAX, "9999 -> clamped to 500");
    CHECK(ui_preset_parse_distance(L"500", &d) == 1 && d == 500.0f, "500 accepted");
    CHECK(ui_preset_parse_distance(L"0.5", &d) == 1 && d == 0.5f, "0.5 accepted");

    int k = 97;
    CHECK(ui_preset_parse_key(L"84", &k) == 1 && k == 84, "key 84 accepted");
    k = 97;
    CHECK(ui_preset_parse_key(L"0", &k) == 0 && k == 97, "key 0 rejected");
    CHECK(ui_preset_parse_key(L"256", &k) == 0 && k == 97, "key 256 rejected");
    CHECK(ui_preset_parse_key(L"-5", &k) == 0 && k == 97, "key -5 rejected");
    CHECK(ui_preset_parse_key(L"abc", &k) == 0 && k == 97, "key abc rejected");
    CHECK(ui_preset_parse_key(L"12abc", &k) == 0 && k == 97, "key 12abc rejected");
    CHECK(ui_preset_parse_key(L"", &k) == 0 && k == 97, "empty key rejected");
    CHECK(ui_preset_parse_key(L"255", &k) == 1 && k == 255, "key 255 accepted");
    CHECK(ui_preset_parse_key(L"1", &k) == 1 && k == 1, "key 1 accepted");

    CHECK(ui_preset_parse_current(L"-1", MAX_PRESETS) == -1, "CurrentPreset -1 (none)");
    CHECK(ui_preset_parse_current(L"0", MAX_PRESETS) == 0, "CurrentPreset 0");
    CHECK(ui_preset_parse_current(L"9", MAX_PRESETS) == 9, "CurrentPreset 9");
    CHECK(ui_preset_parse_current(L"10", MAX_PRESETS) == -1, "CurrentPreset 10 -> none");
    CHECK(ui_preset_parse_current(L"-7", MAX_PRESETS) == -1, "CurrentPreset -7 -> none");
    CHECK(ui_preset_parse_current(L"x", MAX_PRESETS) == -1, "CurrentPreset x -> none");
}

/* Simulates loadPresetsFromConfigFile() line handling on a damaged file: the
   section with the malformed header must NOT overwrite preset 0. */
static void test_preset_file_simulation(void) {
    printf("[4c] damaged voice_presets.cfg: malformed header does not overwrite preset 0\n");

    struct Preset { float whisper; int whisperKey; } presets[MAX_PRESETS];
    for (int i = 0; i < MAX_PRESETS; i++) { presets[i].whisper = 2.0f + (float)i; presets[i].whisperKey = 97; }

    const wchar_t* lines[] = {
        L"CurrentPreset=2",
        L"[Preset0]", L"Whisper=11.0", L"WhisperKey=70",
        L"[PresetBROKEN]", L"Whisper=99.0", L"WhisperKey=71",
        L"[Preset1]", L"Whisper=nan", L"WhisperKey=300",
        L"[Preset12]", L"Whisper=55.0",
    };
    int current = -1;
    int currentPreset = -1;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const wchar_t* line = lines[i];
        if (wcsncmp(line, L"CurrentPreset=", 14) == 0) {
            current = ui_preset_parse_current(line + 14, MAX_PRESETS);
        }
        else if (wcsncmp(line, L"[Preset", 7) == 0) {
            if (!ui_preset_parse_header(line, MAX_PRESETS, &currentPreset)) currentPreset = -1;
        }
        else if (currentPreset >= 0 && currentPreset < MAX_PRESETS) {
            if (wcsncmp(line, L"Whisper=", 8) == 0) ui_preset_parse_distance(line + 8, &presets[currentPreset].whisper);
            else if (wcsncmp(line, L"WhisperKey=", 11) == 0) ui_preset_parse_key(line + 11, &presets[currentPreset].whisperKey);
        }
    }
    CHECK(current == 2, "CurrentPreset read");
    CHECK(presets[0].whisper == 11.0f && presets[0].whisperKey == 70, "preset 0 keeps ITS values (not 99/71)");
    CHECK(presets[1].whisper == 3.0f && presets[1].whisperKey == 97, "preset 1: nan / key 300 ignored, defaults kept");
    CHECK(presets[2].whisper == 4.0f, "no other preset was touched by the broken sections");
}

/* ---- [5] commit field ownership --------------------------------------------- */

static void test_merge_ownership(void) {
    printf("[5] ui_cfg_merge_dialog_fields: only dialog-owned fields are copied\n");

    PluginConfig g, w;
    memset(&g, 0x11, sizeof(g));
    memset(&w, 0x22, sizeof(w));

    /* Strings must be NUL-terminated; give both recognisable content. */
    wcscpy(g.savedPath, L"G:\\manual");
    wcscpy(g.automaticSavedPath, L"G:\\auto-from-watcher");
    wcscpy(w.savedPath, L"W:\\dialog-manual");
    wcscpy(w.automaticSavedPath, L"W:\\stale-snapshot");
    strcpy(g.defaultsAppliedServer, "server-uid-from-profile");
    strcpy(w.defaultsAppliedServer, "stale-snapshot-uid");

    PluginConfig before = g;
    ui_cfg_merge_dialog_fields(&g, &w);

    /* Dialog-owned: now equal to the working copy. */
    CHECK(wcscmp(g.savedPath, L"W:\\dialog-manual") == 0, "savedPath copied");
    CHECK(g.automaticPatchFind == w.automaticPatchFind, "automaticPatchFind copied");
    CHECK(memcmp(&g.distanceWhisper, &w.distanceWhisper, sizeof(float)) == 0, "distanceWhisper copied");
    CHECK(memcmp(&g.distanceNormal, &w.distanceNormal, sizeof(float)) == 0, "distanceNormal copied");
    CHECK(memcmp(&g.distanceShout, &w.distanceShout, sizeof(float)) == 0, "distanceShout copied");
    CHECK(g.whisperKey == w.whisperKey && g.normalKey == w.normalKey && g.shoutKey == w.shoutKey,
        "three voice keys copied");
    CHECK(g.voiceToggleKey == w.voiceToggleKey && g.configUIKey == w.configUIKey,
        "toggle + config keys copied");
    CHECK(g.enableDistanceMuting == w.enableDistanceMuting
        && g.enableAutomaticChannelChange == w.enableAutomaticChannelChange
        && g.enableVoiceToggle == w.enableVoiceToggle, "three feature toggles copied");
    CHECK(g.hudTheme == w.hudTheme && g.hudPosition == w.hudPosition && g.hudSize == w.hudSize,
        "HUD theme/position/size copied");
    CHECK(g.rangeInFoundations == w.rangeInFoundations, "rangeInFoundations (091, display unit) copied");

    /* NOT dialog-owned: exactly what another thread may have changed meanwhile. */
    CHECK(wcscmp(g.automaticSavedPath, L"G:\\auto-from-watcher") == 0,
        "automaticSavedPath (pos watcher) NOT overwritten by the stale snapshot");
    CHECK(strcmp(g.defaultsAppliedServer, "server-uid-from-profile") == 0,
        "defaultsAppliedServer (server profile) NOT overwritten by the stale snapshot");
    CHECK(g.enableVoiceOverlay == before.enableVoiceOverlay, "enableVoiceOverlay untouched");
    CHECK(g.debugMode == before.debugMode, "debugMode untouched");
    CHECK(g.enablePosFile == before.enablePosFile, "enablePosFile untouched");

    /* HUD-only merge (live preview / restore) touches nothing else. */
    PluginConfig g2 = before;
    ui_cfg_merge_hud_fields(&g2, &w);
    CHECK(g2.hudTheme == w.hudTheme && g2.hudPosition == w.hudPosition && g2.hudSize == w.hudSize,
        "hud merge copies the three HUD options");
    CHECK(g2.distanceWhisper == before.distanceWhisper || memcmp(&g2.distanceWhisper, &before.distanceWhisper, sizeof(float)) == 0,
        "hud merge leaves the distances alone");
    CHECK(g2.whisperKey == before.whisperKey && g2.automaticPatchFind == before.automaticPatchFind,
        "hud merge leaves keys / auto-find alone");
    CHECK(g2.rangeInFoundations == before.rangeInFoundations, "hud merge leaves the display unit alone");

    /* A maximal (259 char) savedPath copies completely and stays terminated. */
    for (int i = 0; i < CONFIG_MAX_PATH - 1; i++) w.savedPath[i] = L'z';
    w.savedPath[CONFIG_MAX_PATH - 1] = L'\0';
    PluginConfig g3 = before;
    ui_cfg_merge_dialog_fields(&g3, &w);
    CHECK(wcslen(g3.savedPath) == CONFIG_MAX_PATH - 1, "a 259 char savedPath is copied completely");
}

int main(void) {
    printf("ui_config_logic tests (F10 dialog pure helpers)\n");
    test_wcs_copy();
    test_game_folder_fits();
    test_dist_parse_valid();
    test_dist_parse_invalid();
    test_dist_parse_limits();
    test_dist_parse_display();
    test_preset_header();
    test_preset_values();
    test_preset_file_simulation();
    test_merge_ownership();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
