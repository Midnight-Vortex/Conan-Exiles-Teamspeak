#ifndef TESTS_SUPPORT_PROTO_SHIM_PROTO_COMPAT_H
#define TESTS_SUPPORT_PROTO_SHIM_PROTO_COMPAT_H

/*
 * TEST-ONLY force-include (gcc -include) for the protocol harness: the MSVC
 * "secure" string functions the production code uses (strncpy_s, strcpy_s,
 * _TRUNCATE), with the same observable behaviour for the cases used here.
 * Real Windows builds get these from the CRT and never see this file.
 */

#include <stddef.h>
#include <string.h>

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

static inline int strncpy_s(char* dst, size_t dstSize, const char* src, size_t count) {
    size_t n;
    if (!dst || dstSize == 0) {
        return 22;
    }
    if (!src) {
        dst[0] = '\0';
        return 22;
    }
    n = strlen(src);
    if (count != _TRUNCATE && n > count) {
        n = count;
    }
    if (n >= dstSize) {
        if (count != _TRUNCATE) {
            dst[0] = '\0';
            return 34;
        }
        n = dstSize - 1; /* _TRUNCATE: cut silently */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

static inline int strcpy_s(char* dst, size_t dstSize, const char* src) {
    size_t n;
    if (!dst || dstSize == 0) {
        return 22;
    }
    if (!src) {
        dst[0] = '\0';
        return 22;
    }
    n = strlen(src);
    if (n >= dstSize) {
        dst[0] = '\0';
        return 34;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

#endif /* TESTS_SUPPORT_PROTO_SHIM_PROTO_COMPAT_H */
