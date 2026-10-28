#ifndef TESTS_SUPPORT_AUDIO_SHIM_WINDOWS_H
#define TESTS_SUPPORT_AUDIO_SHIM_WINDOWS_H

/*
 * TEST-ONLY <windows.h> extension for tests/audio_pcm_test.c (review fix 084).
 *
 * The audio test #includes the REAL src/ts/proximity/ts3_proximity_audio.c on a
 * Linux host. That file needs a few more Win32 names than the small shared shim
 * in ../win32_shim/windows.h provides (LONG, LONG64, the 64-bit and Decrement
 * interlocked operations). This header pulls in the shared shim and adds them.
 * Put this directory BEFORE ../win32_shim on the include path. The real Windows
 * builds never see this file.
 */

#include "../win32_shim/windows.h"

typedef long LONG;
typedef long long LONG64;

static inline long InterlockedDecrement(volatile long* value) {
    return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static inline LONG64 InterlockedExchange64(volatile LONG64* target, LONG64 value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

static inline LONG64 InterlockedCompareExchange64(volatile LONG64* dest, LONG64 exchange,
    LONG64 comparand) {
    __atomic_compare_exchange_n(dest, &comparand, exchange, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

#endif /* TESTS_SUPPORT_AUDIO_SHIM_WINDOWS_H */
