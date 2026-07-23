#ifndef CORE_UTIL_COMPAT_CRT_H
#define CORE_UTIL_COMPAT_CRT_H

/*
 * CRT portability shims (V8.0).
 *
 * Purpose: let the pure core modules (hub_parser, player_table, ...) compile
 * unchanged on three toolchains:
 *
 *   - MSVC        -> this header is a no-op (secure CRT is native).
 *   - MinGW-w64   -> this header is a no-op; the cross build passes
 *                    -DMINGW_HAS_SECURE_API=1 so the _s functions and
 *                    _strnicmp/_stricmp come from the MinGW CRT headers.
 *   - plain gcc   -> (Linux host, unit tests only) thin static-inline
 *                    wrappers map the MSVC-specific names onto POSIX/C89
 *                    equivalents with the same truncation semantics we rely on.
 *
 * Only functions actually used by the shimmed modules are provided. This is
 * NOT a general secure-CRT emulation; it exists so unit tests run anywhere.
 *
 * Thread contract: header-only, pure wrappers - callable from any thread.
 */

#if !defined(_WIN32) && !defined(_MSC_VER)

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#ifndef _COMPAT_CRT_ERRNO_T_DEFINED
#define _COMPAT_CRT_ERRNO_T_DEFINED
typedef int errno_t;
#endif

#define _strnicmp strncasecmp
#define _stricmp  strcasecmp

/* strncpy_s with _TRUNCATE semantics: copy at most destSize-1 chars,
   always NUL-terminate. Returns 0 on success (or STRUNCATE-like 80). */
static inline errno_t compat_strncpy_s(char* dest, size_t destSize,
    const char* src, size_t count) {
    size_t i = 0;
    if (!dest || destSize == 0) {
        return EINVAL;
    }
    if (!src) {
        dest[0] = '\0';
        return EINVAL;
    }
    if (count == _TRUNCATE) {
        count = destSize - 1;
    }
    while (i < count && i + 1 < destSize && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
    return 0;
}
#define strncpy_s compat_strncpy_s

static inline errno_t compat_strcpy_s(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) {
        return EINVAL;
    }
    if (!src || strlen(src) + 1 > destSize) {
        dest[0] = '\0';
        return EINVAL;
    }
    memcpy(dest, src, strlen(src) + 1);
    return 0;
}
#define strcpy_s compat_strcpy_s

static inline errno_t compat_strcat_s(char* dest, size_t destSize, const char* src) {
    size_t used;
    if (!dest || destSize == 0 || !src) {
        return EINVAL;
    }
    used = strlen(dest);
    if (used + strlen(src) + 1 > destSize) {
        return EINVAL;
    }
    memcpy(dest + used, src, strlen(src) + 1);
    return 0;
}
#define strcat_s compat_strcat_s

static inline char* compat_strtok_s(char* str, const char* delims, char** context) {
    return strtok_r(str, delims, context);
}
#define strtok_s compat_strtok_s

#endif /* !_WIN32 && !_MSC_VER */

#endif /* CORE_UTIL_COMPAT_CRT_H */
