#ifndef CORE_CONFIG_CONFIG_TEXT_H
#define CORE_CONFIG_CONFIG_TEXT_H

/*
 * plugin.cfg text logic that needs no Win32: parsing a decoded config text into
 * a PluginConfig, clamping a PluginConfig, and the UTF-8 validity test that
 * decides how the raw file bytes are decoded.  Header-only so the host unit
 * tests (tests/config_text_test.c) run the exact code the plugin runs.
 * config.c adds the Win32 parts (paths, locking, file IO, code pages).
 *
 * File encoding (config.c):
 *   - ASCII-only files are byte-identical to what the plugin always wrote.
 *   - Reading: bytes that are valid UTF-8 are decoded as UTF-8, everything
 *     else as the ANSI code page (files written by older plugin versions).
 *
 * Numbers: distances accept one decimal comma ("13,5" = 13.5); text that is
 * not a number stays 0 (then clamped), exactly like before.
 *
 * Thread contract: pure functions on caller-owned data, any thread.
 */

#include "core/config/config.h"
#include "core/util/num_parse.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Defaults, shared by config_set_defaults() and the "not finite" clamp. */
#define CFG_DEFAULT_DIST_WHISPER 3.0f
#define CFG_DEFAULT_DIST_NORMAL  13.0f
#define CFG_DEFAULT_DIST_SHOUT   26.0f

/* ---- small wide-string helpers (ASCII keys) ------------------------------- */

static inline int cfg_wlower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (int)(c + 32) : (int)c;
}

/* ASCII case-insensitive equality of two wide strings. */
static inline int cfg_wieq(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        if (cfg_wlower(*a) != cfg_wlower(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Copy src into dst (capacity cap incl. NUL), truncating. */
static inline void cfg_wcopy(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != L'\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = L'\0';
}

/* Leading ASCII part of a wide value as a narrow string (stops at the first
   non-ASCII character, cap incl. NUL). */
static inline void cfg_wide_to_ascii(const wchar_t* in, char* out, size_t cap) {
    size_t i = 0;
    while (in[i] != L'\0' && in[i] < 128 && i + 1 < cap) {
        out[i] = (char)in[i];
        i++;
    }
    out[i] = '\0';
}

static inline float cfg_parse_float(const wchar_t* val) {
    char buf[64];
    cfg_wide_to_ascii(val, buf, sizeof(buf));
    return (float)num_strtod(buf);
}

static inline int cfg_parse_int(const wchar_t* val) {
    char buf[64];
    long n;
    cfg_wide_to_ascii(val, buf, sizeof(buf));
    n = strtol(buf, NULL, 10);
    if (n > INT_MAX) return INT_MAX;
    if (n < INT_MIN) return INT_MIN;
    return (int)n;
}

static inline int cfg_parse_bool(const wchar_t* val) {
    return (cfg_wieq(val, L"true") || wcscmp(val, L"1") == 0) ? 1 : 0;
}

/* Trim leading/trailing whitespace in place, returns start pointer. */
static inline wchar_t* cfg_trim(wchar_t* s) {
    wchar_t* end;
    while (*s == L' ' || *s == L'\t') {
        s++;
    }
    end = s + wcslen(s);
    while (end > s && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' ' || end[-1] == L'\t')) {
        *--end = L'\0';
    }
    return s;
}

/* ---- one "Key=Value" line ---------------------------------------------------- */

static inline void cfg_apply_line(PluginConfig* cfg, wchar_t* line) {
    wchar_t* p = cfg_trim(line);
    wchar_t* eq;
    wchar_t* key;
    wchar_t* val;

    if (*p == L'#' || *p == L';' || *p == L'\0') {
        return;
    }

    eq = wcschr(p, L'=');
    if (!eq) {
        return;
    }
    *eq = L'\0';
    key = cfg_trim(p);
    val = cfg_trim(eq + 1);

    if (cfg_wieq(key, L"SavedPath")) {
        cfg_wcopy(cfg->savedPath, CONFIG_MAX_PATH, val);
    }
    else if (cfg_wieq(key, L"AutomaticSavedPath")) {
        cfg_wcopy(cfg->automaticSavedPath, CONFIG_MAX_PATH, val);
    }
    else if (cfg_wieq(key, L"AutomaticPatchFind")) {
        cfg->automaticPatchFind = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"DistanceWhisper")) {
        cfg->distanceWhisper = cfg_parse_float(val);
    }
    else if (cfg_wieq(key, L"DistanceNormal")) {
        cfg->distanceNormal = cfg_parse_float(val);
    }
    else if (cfg_wieq(key, L"DistanceShout")) {
        cfg->distanceShout = cfg_parse_float(val);
    }
    else if (cfg_wieq(key, L"ShowRangesInFoundations")) {
        cfg->rangeInFoundations = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"WhisperKey")) {
        cfg->whisperKey = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"NormalKey")) {
        cfg->normalKey = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"ShoutKey")) {
        cfg->shoutKey = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"VoiceToggleKey")) {
        cfg->voiceToggleKey = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"ConfigUIKey")) {
        cfg->configUIKey = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"EnableDistanceMuting")) {
        cfg->enableDistanceMuting = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"EnableAutomaticChannelChange")) {
        cfg->enableAutomaticChannelChange = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"EnableVoiceToggle")) {
        cfg->enableVoiceToggle = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"EnableVoiceOverlay")) {
        cfg->enableVoiceOverlay = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"HudTheme")) {
        cfg->hudTheme = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"HudPosition")) {
        cfg->hudPosition = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"HudSize")) {
        cfg->hudSize = cfg_parse_int(val);
    }
    else if (cfg_wieq(key, L"DebugMode")) {
        cfg->debugMode = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"EnablePosFile")) {
        cfg->enablePosFile = cfg_parse_bool(val);
    }
    else if (cfg_wieq(key, L"DefaultsAppliedServer")) {
        /* Server UID is plain ASCII (base64 alphabet). */
        size_t j = 0;
        const wchar_t* src;
        for (src = val; *src && j + 1 < sizeof(cfg->defaultsAppliedServer); src++) {
            if (*src < 128) {
                cfg->defaultsAppliedServer[j++] = (char)*src;
            }
        }
        cfg->defaultsAppliedServer[j] = '\0';
    }
}

/* Whole decoded file: split at '\n' ('\r' is trimmed per line) and apply each
   line.  text is modified in place. */
static inline void cfg_parse_text(PluginConfig* cfg, wchar_t* text) {
    wchar_t* line = text;
    while (line && *line) {
        wchar_t* nl = wcschr(line, L'\n');
        if (nl) {
            *nl = L'\0';
        }
        cfg_apply_line(cfg, line);
        line = nl ? nl + 1 : NULL;
    }
}

/* ---- clamp ------------------------------------------------------------------- */

static inline void cfg_clamp_dist(float* v, float def) {
    if (!isfinite(*v)) {
        *v = def; /* nan / inf in plugin.cfg must never reach the voice distance */
    }
    if (*v < 0.5f) *v = 0.5f;
    if (*v > 500.0f) *v = 500.0f;
}

/* Clamp numeric fields into safe ranges.  Non-finite distances fall back to
   the default. */
static inline void cfg_clamp_values(PluginConfig* cfg) {
    cfg_clamp_dist(&cfg->distanceWhisper, CFG_DEFAULT_DIST_WHISPER);
    cfg_clamp_dist(&cfg->distanceNormal, CFG_DEFAULT_DIST_NORMAL);
    cfg_clamp_dist(&cfg->distanceShout, CFG_DEFAULT_DIST_SHOUT);

    cfg->rangeInFoundations = cfg->rangeInFoundations ? 1 : 0; /* 091: bool field, 0/1 only */

    if (cfg->whisperKey <= 0 || cfg->whisperKey >= 256) cfg->whisperKey = 97;
    if (cfg->normalKey <= 0 || cfg->normalKey >= 256) cfg->normalKey = 98;
    if (cfg->shoutKey <= 0 || cfg->shoutKey >= 256) cfg->shoutKey = 99;
    if (cfg->voiceToggleKey <= 0 || cfg->voiceToggleKey >= 256) cfg->voiceToggleKey = 84;
    if (cfg->configUIKey <= 0 || cfg->configUIKey >= 256) cfg->configUIKey = 121;

    if (cfg->hudTheme < 0) cfg->hudTheme = 0;
    if (cfg->hudTheme > 11) cfg->hudTheme = 11;
    if (cfg->hudPosition < 0) cfg->hudPosition = 0;
    if (cfg->hudPosition > 4) cfg->hudPosition = 4;
    if (cfg->hudSize < 0) cfg->hudSize = 0;
    if (cfg->hudSize > 2) cfg->hudSize = 2;
}

/* ---- encoding ---------------------------------------------------------------- */

/* 1 when the bytes are well-formed UTF-8 (no overlong forms, no surrogates,
   nothing above U+10FFFF).  Pure ASCII counts as valid. */
static inline int cfg_utf8_is_valid(const unsigned char* s, size_t len) {
    size_t i = 0;
    while (i < len) {
        const unsigned char c = s[i];
        size_t extra;
        unsigned long cp;
        size_t k;

        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1Fu;
        }
        else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0Fu;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07u;
        }
        else {
            return 0; /* stray continuation byte, C0/C1, F5..FF */
        }
        if (i + extra >= len) {
            return 0; /* truncated sequence */
        }
        for (k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0u) != 0x80u) {
                return 0;
            }
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if ((extra == 2 && cp < 0x800ul) || (extra == 3 && cp < 0x10000ul)) {
            return 0; /* overlong */
        }
        if (cp > 0x10FFFFul || (cp >= 0xD800ul && cp <= 0xDFFFul)) {
            return 0;
        }
        i += extra + 1;
    }
    return 1;
}

/* 1 when the bytes contain anything outside ASCII. */
static inline int cfg_has_non_ascii(const unsigned char* s, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (s[i] >= 0x80) {
            return 1;
        }
    }
    return 0;
}

/* The writer may store the ANSI form of the file only when the reader will
   decode it as ANSI again: it must contain non-ASCII (else both are the same)
   and must NOT also be valid UTF-8 (the reader would pick UTF-8 then). */
static inline int cfg_ansi_bytes_are_unambiguous(const unsigned char* s, size_t len) {
    return !cfg_has_non_ascii(s, len) || !cfg_utf8_is_valid(s, len);
}

#endif /* CORE_CONFIG_CONFIG_TEXT_H */
