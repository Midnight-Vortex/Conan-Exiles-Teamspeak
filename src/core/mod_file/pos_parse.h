#ifndef CORE_MOD_FILE_POS_PARSE_H
#define CORE_MOD_FILE_POS_PARSE_H

/*
 * Pure Pos.txt line parser + sample sanity checks (header-only, host-testable).
 *
 * Line format (the HTTP body in the legacy format is the same):
 *   SEQ=<int> X=<cm> Y=<cm> Z=<cm> YAW=<deg> [YAWY=<deg>]
 * The caller has already turned DE comma decimals into dots.
 *
 * Rules (one place for the file reader AND the HTTP inject path):
 *   - every KEY= must be followed by a real number ("YAW=" with nothing, or
 *     "X=abc", is NOT a zero - it is rejected; YAWY stays optional),
 *   - x, y, z, yaw, yawY must be finite and sane (|coord| and |angle| below
 *     1e8): a finite 1e300 would become inf after the float cast of the wire
 *     format and poison every distance computed from it,
 *   - seq is converted without overflow.
 *
 * Thread contract: pure, any thread.
 */

#include "core/mod_file/pos_file.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POS_COORD_MAX_CM  1.0e8   /* 1000 km - far beyond any Conan map */
#define POS_ANGLE_MAX_DEG 1.0e8

/* "KEY=<number>" anywhere in buffer.  Returns 1 only when the key exists AND a
   number follows it. */
static inline int pos_parse_field(const char* buffer, const char* key, double* out) {
    const char* p = strstr(buffer, key);
    char* end = NULL;
    double v;

    if (!p) {
        return 0;
    }
    p += strlen(key);
    v = strtod(p, &end);
    if (end == p) {
        return 0; /* empty or non-numeric value */
    }
    *out = v;
    return 1;
}

/* double -> int without undefined behaviour (NaN -> 0, out of range -> clamped). */
static inline int pos_seq_from_double(double seq) {
    if (!(seq == seq)) {
        return 0;
    }
    if (seq >= 2147483647.0) {
        return 2147483647;
    }
    if (seq <= -2147483648.0) {
        return (-2147483647 - 1);
    }
    return (int)seq;
}

/* Finite double without isfinite(): MinGW's type-generic isfinite converts
   through float (a conversion warning that MSVC /W4 /WX would turn into a
   build error). NaN fails both comparisons, +-Inf fails one. */
static inline int pos_finite_d(double v) {
    return v >= -DBL_MAX && v <= DBL_MAX;
}

/* All five fields finite and within the sane range. */
static inline int pos_sample_is_sane(const PosSample* s) {
    if (!s) {
        return 0;
    }
    if (!pos_finite_d(s->x) || !pos_finite_d(s->y) || !pos_finite_d(s->z)
        || !pos_finite_d(s->yaw) || !pos_finite_d(s->yawY)) {
        return 0;
    }
    if (fabs(s->x) > POS_COORD_MAX_CM || fabs(s->y) > POS_COORD_MAX_CM
        || fabs(s->z) > POS_COORD_MAX_CM) {
        return 0;
    }
    if (fabs(s->yaw) > POS_ANGLE_MAX_DEG || fabs(s->yawY) > POS_ANGLE_MAX_DEG) {
        return 0;
    }
    return 1;
}

/* Sane AND not the mod's loading placeholder (0,0,0 cm) - that one breaks the
   distance for every speaker. */
static inline int pos_sample_is_plausible(const PosSample* s) {
    if (!pos_sample_is_sane(s)) {
        return 0;
    }
    if (fabs(s->x) < 1.0 && fabs(s->y) < 1.0 && fabs(s->z) < 1.0) {
        return 0;
    }
    return 1;
}

/* Parse a Pos.txt style line.  Returns 1 and fills *out when SEQ, X, Y, Z and
   YAW are present with real numbers and the sample is sane.  *out is only
   written on success. */
static inline int pos_parse_line(const char* buffer, PosSample* out) {
    double seq = 0.0;
    PosSample sample;

    if (!buffer || !out) {
        return 0;
    }
    memset(&sample, 0, sizeof(sample));

    if (!pos_parse_field(buffer, "SEQ=", &seq)
        || !pos_parse_field(buffer, "X=", &sample.x)
        || !pos_parse_field(buffer, "Y=", &sample.y)
        || !pos_parse_field(buffer, "Z=", &sample.z)
        || !pos_parse_field(buffer, "YAW=", &sample.yaw)) {
        return 0;
    }
    sample.seq = pos_seq_from_double(seq);

    /* YAWY is optional (older mod versions). */
    if (!pos_parse_field(buffer, "YAWY=", &sample.yawY)) {
        sample.yawY = 0.0;
    }

    if (!pos_sample_is_sane(&sample)) {
        return 0;
    }
    *out = sample;
    return 1;
}

#endif /* CORE_MOD_FILE_POS_PARSE_H */
