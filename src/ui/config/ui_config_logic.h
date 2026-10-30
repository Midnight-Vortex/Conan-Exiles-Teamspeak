#ifndef UI_CONFIG_LOGIC_H
#define UI_CONFIG_LOGIC_H

/*
 * EN: Pure (Win32-free, header-only) helper logic of the F10 settings dialog:
 *     bounded string copy, game-folder length check, strict distance parsing,
 *     voice_presets.cfg line validation and the "which PluginConfig fields does
 *     the dialog own" merge used by ui_cfg_commit(). Kept free of Windows
 *     headers so tests/ui_config_logic_test.c can compile it with plain gcc.
 * FR: Logique pure du dialogue F10 (sans Win32, en-tete seul) : copie bornee,
 *     longueur du dossier de jeu, lecture stricte des distances, validation du
 *     fichier voice_presets.cfg et fusion des champs possedes par le dialogue.
 *
 * Thread contract: no state, no I/O — every function is thread-safe as long as
 * the caller owns the buffers it passes in.
 */

#include "core/config/config.h"
#include "core/config/range_units.h"

#include <math.h>
#include <stddef.h>
#include <wchar.h>

/* ---- bounded string copy ---------------------------------------------------
   wcscpy_s/wcscat_s call the CRT invalid-parameter handler (= terminate the
   whole TeamSpeak process) when the destination is too small. A path typed or
   edited by the user can be that long, so the dialog copies through this
   helper instead: it always NUL-terminates and reports truncation. */

/* Copies src into dst[cap]. Returns 1 when the whole string fit, 0 when it was
   truncated (dst still holds the first cap-1 characters) or an argument was
   invalid. Never overruns dst. */
static inline int ui_wcs_copy(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = L'\0';
        return 0;
    }
    size_t n = wcslen(src);
    int fits = (n < cap);
    if (!fits) {
        n = cap - 1;
    }
    wmemcpy(dst, src, n);
    dst[n] = L'\0';
    return fits;
}

/* "\ConanSandbox\Saved" is appended to the game folder before it is stored. */
#define UI_SAVED_SUBPATH L"\\ConanSandbox\\Saved"
#define UI_SAVED_SUBPATH_LEN 19

/* 1 when folder + "\ConanSandbox\Saved" + NUL fits into a buffer of cap wide
   chars (cap is normally MAX_PATH). */
static inline int ui_game_folder_fits(const wchar_t* folder, size_t cap) {
    if (!folder) {
        return 0;
    }
    return wcslen(folder) + UI_SAVED_SUBPATH_LEN < cap;
}

/* ---- distance edit fields --------------------------------------------------
   The three distance edits are plain EDIT controls (no ES_NUMBER), so the text
   can be anything. _wtof() turned "", "abc" or "1x" into 0 / a partial number
   without telling the user and accepted "nan". This parser is strict and
   locale independent (it accepts '.' and ',' as the decimal separator, so the
   "%.1f" text the dialog writes itself always reads back). */

typedef enum UiDistStatus {
    UI_DIST_OK = 0,
    UI_DIST_EMPTY,        /* nothing typed */
    UI_DIST_NOT_NUMBER,   /* letters, nan/inf, exponent, two dots, overflow ... */
    UI_DIST_NOT_POSITIVE, /* 0 or negative */
    UI_DIST_OUT_OF_RANGE  /* outside the server (hub) min/max */
} UiDistStatus;

/* Parses text. When limitsActive is non-zero the server limits apply: a limit
   counts as "set" only when it is > 0 (same rule as voice_mode_get_distance).
   *out is written only on UI_DIST_OK. */
static inline UiDistStatus ui_dist_parse(const wchar_t* text, int limitsActive,
    float hubMin, float hubMax, float* out) {
    if (!text) {
        return UI_DIST_EMPTY;
    }
    const wchar_t* p = text;
    while (*p == L' ' || *p == L'\t') {
        p++;
    }
    if (*p == L'\0') {
        return UI_DIST_EMPTY;
    }

    int negative = 0;
    if (*p == L'+' || *p == L'-') {
        negative = (*p == L'-');
        p++;
    }

    double intPart = 0.0;
    double fracPart = 0.0;
    double fracScale = 1.0;
    int digits = 0;
    int dots = 0;
    for (;; p++) {
        if (*p >= L'0' && *p <= L'9') {
            const double d = (double)(*p - L'0');
            if (dots == 0) {
                intPart = intPart * 10.0 + d;
            }
            else {
                fracScale /= 10.0;
                fracPart += d * fracScale;
            }
            digits++;
        }
        else if (*p == L'.' || *p == L',') {
            dots++;
        }
        else {
            break;
        }
    }
    while (*p == L' ' || *p == L'\t') {
        p++;
    }
    if (*p != L'\0' || digits == 0 || dots > 1) {
        return UI_DIST_NOT_NUMBER;
    }

    double v = intPart + fracPart;
    if (negative) {
        v = -v;
    }
    const float fv = (float)v;
    if (!isfinite(fv)) {
        return UI_DIST_NOT_NUMBER;
    }
    if (fv <= 0.0f) {
        return UI_DIST_NOT_POSITIVE;
    }
    if (limitsActive) {
        if (hubMin > 0.0f && fv < hubMin) {
            return UI_DIST_OUT_OF_RANGE;
        }
        if (hubMax > 0.0f && fv > hubMax) {
            return UI_DIST_OUT_OF_RANGE;
        }
    }
    if (out) {
        *out = fv;
    }
    return UI_DIST_OK;
}

/* ---- 091: a distance edit read in the DISPLAY unit -------------------------
   The edits show meters or foundations (core/config/range_units.h); the working
   copy and everything behind it stay in meters. This is the Save-time read of
   one edit:

   - The text is parsed strictly (ui_dist_parse) and must be a number > 0.
   - NO DRIFT: when the text still reads exactly like the stored value shown in
     this unit (the player did not change the field), the stored meters are kept
     bit for bit. Otherwise 40 m is shown as "15.6" foundations and pressing Save
     would turn it into 15.6 * 2.56 = 39.936 m. Applied in both units.
   - A changed text is checked against the server limits in the display unit
     (min rounded up, max rounded down, see range_limits_display), converted to
     meters and pulled into the meter limits (float noise, range_clamp_meters).
     An unchanged value is checked against the limits in meters; one a hair
     outside that still reads as the typed text is pulled onto the limit.
   *outMeters is written only on UI_DIST_OK. */
static inline UiDistStatus ui_dist_parse_display(const wchar_t* text, int inFoundations,
    float storedMeters, int limitsActive, float hubMinMeters, float hubMaxMeters,
    float* outMeters) {
    float typed = 0.0f;
    const UiDistStatus st = ui_dist_parse(text, 0, 0.0f, 0.0f, &typed);
    if (st != UI_DIST_OK) {
        return st;
    }

    if (range_display_unchanged(storedMeters, inFoundations, typed)) {
        /* A stored value a hair outside the limits that still READS as the typed
           text (15.4 m from an older Save against a 15.36 m = 6.0 F limit) is pulled
           onto the limit; one that reads differently (16 m against 15 m) is refused
           as before. */
        const float kept = limitsActive
            ? range_clamp_meters(storedMeters, hubMinMeters, hubMaxMeters) : storedMeters;
        if (kept != storedMeters
            && (!range_display_unchanged(kept, inFoundations, typed)
                || (hubMinMeters > 0.0f && kept < hubMinMeters)
                || (hubMaxMeters > 0.0f && kept > hubMaxMeters))) {
            return UI_DIST_OUT_OF_RANGE;   /* reads differently, or min > max (admin error) */
        }
        if (outMeters) {
            *outMeters = kept;
        }
        return UI_DIST_OK;
    }

    float shownMin = 0.0f;
    float shownMax = 0.0f;
    range_limits_display(hubMinMeters, hubMaxMeters, inFoundations, &shownMin, &shownMax);
    float shown = 0.0f;
    const UiDistStatus st2 = ui_dist_parse(text, limitsActive, shownMin, shownMax, &shown);
    if (st2 != UI_DIST_OK) {
        return st2;
    }
    if (outMeters) {
        const float meters = range_to_meters(shown, inFoundations);
        *outMeters = limitsActive ? range_clamp_meters(meters, hubMinMeters, hubMaxMeters) : meters;
    }
    return UI_DIST_OK;
}

/* ---- voice_presets.cfg validation ------------------------------------------
   The file is user-editable text. Before 087 a damaged "[PresetX]" header was
   read with _wtoi() as preset 0 (overwriting it), and distances/keys were not
   range-checked. The helpers below accept only well-formed values. */

/* Same bounds config_clamp() applies to the personal distances. */
#define UI_PRESET_DIST_MIN 0.5f
#define UI_PRESET_DIST_MAX 500.0f

/* "[Preset3]" -> *outIndex = 3. Returns 1 only for a strictly well-formed
   header whose index is in 0..maxPresets-1; 0 otherwise (the caller then
   ignores the lines of that section). */
static inline int ui_preset_parse_header(const wchar_t* line, int maxPresets, int* outIndex) {
    if (!line || wcsncmp(line, L"[Preset", 7) != 0) {
        return 0;
    }
    const wchar_t* p = line + 7;
    if (*p < L'0' || *p > L'9') {
        return 0;
    }
    long v = 0;
    int nd = 0;
    while (*p >= L'0' && *p <= L'9') {
        v = v * 10 + (long)(*p - L'0');
        if (++nd > 6) {
            return 0;
        }
        p++;
    }
    if (*p != L']') {
        return 0;
    }
    p++;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') {
        p++;
    }
    if (*p != L'\0') {
        return 0;
    }
    if (v < 0 || v >= (long)maxPresets) {
        return 0;
    }
    if (outIndex) {
        *outIndex = (int)v;
    }
    return 1;
}

/* Strict decimal integer ("97", "-1", " 12 "). Returns 1 on success. */
static inline int ui_preset_parse_int(const wchar_t* s, int* out) {
    if (!s) {
        return 0;
    }
    while (*s == L' ' || *s == L'\t') {
        s++;
    }
    int negative = 0;
    if (*s == L'+' || *s == L'-') {
        negative = (*s == L'-');
        s++;
    }
    long v = 0;
    int nd = 0;
    while (*s >= L'0' && *s <= L'9') {
        v = v * 10 + (long)(*s - L'0');
        if (++nd > 9) {
            return 0;
        }
        s++;
    }
    while (*s == L' ' || *s == L'\t') {
        s++;
    }
    if (nd == 0 || *s != L'\0') {
        return 0;
    }
    if (out) {
        *out = (int)(negative ? -v : v);
    }
    return 1;
}

/* A virtual-key code the dialog / config_clamp() accepts (1..255). */
static inline int ui_preset_key_valid(int vk) {
    return vk > 0 && vk < 256;
}

/* Reads a key value from the file. Returns 1 and writes *out only for a valid
   key; otherwise *out is left untouched (keeps the default / previous key). */
static inline int ui_preset_parse_key(const wchar_t* s, int* out) {
    int v = 0;
    if (!ui_preset_parse_int(s, &v) || !ui_preset_key_valid(v)) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Reads a distance from the file. A number is clamped into
   [UI_PRESET_DIST_MIN, UI_PRESET_DIST_MAX] (0 / negative -> minimum); text that
   is not a finite number leaves *out untouched. Returns 1 when *out was set. */
static inline int ui_preset_parse_distance(const wchar_t* s, float* out) {
    float v = 0.0f;
    const UiDistStatus st = ui_dist_parse(s, 0, 0.0f, 0.0f, &v);
    if (st == UI_DIST_NOT_POSITIVE) {
        v = UI_PRESET_DIST_MIN;
    }
    else if (st != UI_DIST_OK) {
        return 0;
    }
    if (v < UI_PRESET_DIST_MIN) {
        v = UI_PRESET_DIST_MIN;
    }
    if (v > UI_PRESET_DIST_MAX) {
        v = UI_PRESET_DIST_MAX;
    }
    *out = v;
    return 1;
}

/* "CurrentPreset=" value: -1 (none) .. maxPresets-1, anything else -> -1. */
static inline int ui_preset_parse_current(const wchar_t* s, int maxPresets) {
    int v = -1;
    if (!ui_preset_parse_int(s, &v) || v < -1 || v >= maxPresets) {
        return -1;
    }
    return v;
}

/* ---- F10 commit: which PluginConfig fields belong to the dialog -------------
   g_config is also written by other threads while the dialog is open
   (DefaultsAppliedServer by the server-profile code, AutomaticSavedPath by the
   Pos.txt watcher, ...). The commit therefore copies ONLY the fields the dialog
   edits into the live g_config (under the config lock, via config_modify) and
   leaves everything else alone.

   Dialog-owned: savedPath, automaticPatchFind, the three distances, the display
   unit of the distances (rangeInFoundations, 091), the five hotkeys, the three
   feature toggles, the three HUD options.
   NOT dialog-owned (never copied): automaticSavedPath, enableVoiceOverlay,
   debugMode, enablePosFile, defaultsAppliedServer. */
static inline void ui_cfg_merge_hud_fields(PluginConfig* g, const PluginConfig* w) {
    g->hudTheme = w->hudTheme;
    g->hudPosition = w->hudPosition;
    g->hudSize = w->hudSize;
}

static inline void ui_cfg_merge_dialog_fields(PluginConfig* g, const PluginConfig* w) {
    ui_wcs_copy(g->savedPath, CONFIG_MAX_PATH, w->savedPath);
    g->automaticPatchFind = w->automaticPatchFind;

    g->distanceWhisper = w->distanceWhisper;
    g->distanceNormal = w->distanceNormal;
    g->distanceShout = w->distanceShout;
    g->rangeInFoundations = w->rangeInFoundations;

    g->whisperKey = w->whisperKey;
    g->normalKey = w->normalKey;
    g->shoutKey = w->shoutKey;
    g->voiceToggleKey = w->voiceToggleKey;
    g->configUIKey = w->configUIKey;

    g->enableDistanceMuting = w->enableDistanceMuting;
    g->enableAutomaticChannelChange = w->enableAutomaticChannelChange;
    g->enableVoiceToggle = w->enableVoiceToggle;

    ui_cfg_merge_hud_fields(g, w);
}

#endif /* UI_CONFIG_LOGIC_H */
