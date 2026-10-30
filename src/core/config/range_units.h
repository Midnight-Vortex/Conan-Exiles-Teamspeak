#ifndef CORE_CONFIG_RANGE_UNITS_H
#define CORE_CONFIG_RANGE_UNITS_H

/*
 * 091 — voice ranges shown in Conan Exiles "foundations" instead of meters.
 *
 * DISPLAY / INPUT ONLY.  Everything that is stored or computed stays in METERS:
 * plugin.cfg, voice_presets.cfg, the hub limits of the Root description, CEPOS,
 * the audio math.  Only the F10 dialog and two chat lines convert, using the
 * helpers below.  1.0 = one foundation edge, 6.5 = six and a half foundations.
 *
 * Header-only and free of ts/ and ui/ includes, so the host test
 * (tests/range_units_test.c) runs the exact code the plugin runs.
 *
 * Thread contract: pure functions on caller-owned values, no state; any thread.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ONE foundation edge in meters = 256 Unreal units.
   Source (Conan Exiles DevKit): the BuildSockets of BP_BuildFoundation,
   BP_BuildFoundation_T2, BP_BuildFoundation_T3 and BP_HL_BuildFoundation_T3 sit
   at X/Y = +-128 around the center of the piece, so two neighbouring
   foundations are exactly 256 UU = 2.56 m apart - the same for every tier.
   The meshes LOOK bigger than that, which is where the "300" quoted on the
   internet comes from; the grid spacing is what builders count. */
#define RANGE_FOUNDATION_EDGE_M 2.56f

/* Tolerance (in tenths of a foundation) for the limit rounding below: float
   noise must not turn an exact multiple such as 2.56 m (= 1.0) into 1.1. */
#define RANGE_LIMIT_EPS_TENTHS 0.001

/* The effective display unit: the player's own choice OR the server force
   (Root key ForceRangesInFoundations).  The force is never written into the
   player's own setting - it only decides what is shown while connected. */
static inline int range_effective_unit(int userChoice, int serverForce) {
    return (userChoice != 0 || serverForce != 0) ? 1 : 0;
}

/* meters -> value shown to the player (inFoundations = 0: unchanged). */
static inline float range_from_meters(float meters, int inFoundations) {
    return inFoundations ? meters / RANGE_FOUNDATION_EDGE_M : meters;
}

/* value typed by the player -> meters (inFoundations = 0: unchanged). */
static inline float range_to_meters(float display, int inFoundations) {
    return inFoundations ? display * RANGE_FOUNDATION_EDGE_M : display;
}

/* Lower hub limit as shown in foundation mode: rounded UP to 0.1, so that every
   value from the shown minimum upwards is also >= the limit in meters.
   Example: 2.1 m = 0.82 F is shown as 0.9 (0.8 F = 2.048 m would be too low).
   Meter mode returns the limit unchanged; 0 / negative / NaN = "no limit" and
   is returned unchanged in both modes. */
static inline float range_limit_min_display(float minMeters, int inFoundations) {
    if (!inFoundations || !(minMeters > 0.0f)) {
        return minMeters;
    }
    const double tenths = (double)minMeters / (double)RANGE_FOUNDATION_EDGE_M * 10.0;
    return (float)(ceil(tenths - RANGE_LIMIT_EPS_TENTHS) / 10.0);
}

/* Upper hub limit as shown in foundation mode: rounded DOWN to 0.1, so that
   every value up to the shown maximum is also <= the limit in meters.
   Example: 15 m = 5.86 F is shown as 5.8.  A limit that is set but smaller than
   one tenth stays at 0.1 (0 would read as "no limit").  Meter mode, 0, negative
   and NaN: unchanged. */
static inline float range_limit_max_display(float maxMeters, int inFoundations) {
    if (!inFoundations || !(maxMeters > 0.0f)) {
        return maxMeters;
    }
    const double tenths = (double)maxMeters / (double)RANGE_FOUNDATION_EDGE_M * 10.0;
    double shown = floor(tenths + RANGE_LIMIT_EPS_TENTHS) / 10.0;
    if (shown < 0.1) {
        shown = 0.1;
    }
    return (float)shown;
}

/* BOTH hub limits as shown, for one voice mode.  Normally min rounded up and max
   rounded down (the two helpers above).  When the two meter limits are so close
   that no 0.1 F step lies between them, that would read "min > max" (15..15 m =
   5.86 F: up 5.9, down 5.8) and no typed value could ever pass.  Then both are
   rounded to the NEAREST tenth instead (15..15 m is shown as 5.9..5.9); rounding
   keeps the order, so the shown range is never inverted.  (Limits below 0.05 F
   = 0.128 m still show 0.1: far under the 0.5 m every personal range has.)
   A value typed in that range is pulled into the meter limits by
   range_clamp_meters below.
   Meter mode, or only one limit set: exactly the two helpers above. */
static inline void range_limits_display(float minMeters, float maxMeters, int inFoundations,
    float* outMin, float* outMax) {
    float lo = range_limit_min_display(minMeters, inFoundations);
    float hi = range_limit_max_display(maxMeters, inFoundations);
    if (inFoundations && minMeters > 0.0f && maxMeters >= minMeters && lo > hi) {
        /* "nearest" = exactly what a field shows for that value ("%.1f", the same
           formatting as range_display_unchanged), so the shown limit and the field
           agree even on an exact half tenth (3.2 m = 1.25 F). */
        char txt[48];
        snprintf(txt, sizeof(txt), "%.1f", (double)range_from_meters(minMeters, 1));
        lo = (float)strtod(txt, NULL);
        snprintf(txt, sizeof(txt), "%.1f", (double)range_from_meters(maxMeters, 1));
        hi = (float)strtod(txt, NULL);
        if (lo < 0.1f) {
            lo = 0.1f;
        }
        if (hi < lo) {
            hi = lo;
        }
    }
    if (outMin) {
        *outMin = lo;
    }
    if (outMax) {
        *outMax = hi;
    }
}

/* A value converted from the display unit back to meters, pulled into the hub
   limits (0 = no limit).  The shown limits already passed, so any difference is
   float noise (10 F = 25.5999985 m against a 25.6 m limit) or the nearest-rounded
   narrow range above (5.9 F = 15.104 m against 15..15 m).  Same order as the
   meter-mode clamp (validateDistanceValue): below the minimum -> the minimum. */
static inline float range_clamp_meters(float meters, float minMeters, float maxMeters) {
    if (minMeters > 0.0f && meters < minMeters) {
        return minMeters;
    }
    if (maxMeters > 0.0f && meters > maxMeters) {
        return maxMeters;
    }
    return meters;
}

/* 1 when a field that shows "%.1f" of the stored value still reads exactly as
   the player typed it, i.e. the player did NOT change the number.  The dialog
   then keeps the stored meters untouched (no drift: 40 m shows as 15.6 F, and
   15.6 F must not turn it into 39.936 m on Save).  Compares the rounded texts,
   so "15.6", "15.60" and "15.55" (when it prints as 15.6) are all "unchanged". */
static inline int range_display_unchanged(float meters, int inFoundations, float typedDisplay) {
    char shown[48];
    char typed[48];
    snprintf(shown, sizeof(shown), "%.1f", (double)range_from_meters(meters, inFoundations));
    snprintf(typed, sizeof(typed), "%.1f", (double)typedDisplay);
    return strcmp(shown, typed) == 0;
}

/* Unit names.  Dialog: wide.  Chat: narrow.  The short form is for the preset
   list where the line is tight ("W:2.0 N:10.0 S:15.0 m"). */
static inline const wchar_t* range_unit_name_w(int inFoundations) {
    return inFoundations ? L"foundations" : L"meters";
}

static inline const wchar_t* range_unit_short_w(int inFoundations) {
    return inFoundations ? L"fnd." : L"m";
}

static inline const char* range_unit_name_a(int inFoundations) {
    return inFoundations ? "foundations" : "meters";
}

#endif /* CORE_CONFIG_RANGE_UNITS_H */
