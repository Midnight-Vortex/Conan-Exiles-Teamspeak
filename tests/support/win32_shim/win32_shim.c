/* Implementation of the test-only <windows.h> shim (see windows.h here). */

#include <windows.h>

#include <time.h>

static ULONGLONG g_tickOffsetMs = 0;

ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL
        + (ULONGLONG)ts.tv_nsec / 1000000ULL
        + g_tickOffsetMs;
}

void win32_shim_advance_ms(ULONGLONG ms) {
    g_tickOffsetMs += ms;
}
