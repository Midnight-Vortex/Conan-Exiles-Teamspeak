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
