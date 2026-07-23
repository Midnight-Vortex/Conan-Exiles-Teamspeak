#ifndef TESTS_SUPPORT_WIN32_SHIM_WINDOWS_H
#define TESTS_SUPPORT_WIN32_SHIM_WINDOWS_H

/*
 * TEST-ONLY <windows.h> replacement (V8.1).
 *
 * Lets pure-ish core modules that use a handful of Win32 primitives
 * (player_table.c: CRITICAL_SECTION, INIT_ONCE, GetTickCount64,
 * InterlockedIncrement) compile UNCHANGED on a Linux host for unit tests.
 * run_tests.sh puts this directory on the include path BEFORE the system
 * headers; the real Windows builds (MSVC, MinGW) never see this file.
 *
 * Mapping: CRITICAL_SECTION -> pthread_mutex_t, INIT_ONCE -> guarded flag,
 * GetTickCount64 -> CLOCK_MONOTONIC (+ test-controlled offset),
 * InterlockedIncrement -> __atomic builtin.
 */

#include <pthread.h>

typedef int BOOL;
typedef void* PVOID;
typedef unsigned long long ULONGLONG;

#define CALLBACK
#define TRUE  1
#define FALSE 0
#define MAXULONGLONG (~0ULL)

/* ---- CRITICAL_SECTION -> pthread mutex ---------------------------------- */

typedef pthread_mutex_t CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_init(cs, NULL);
}
static inline void EnterCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_lock(cs);
}
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_unlock(cs);
}
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_destroy(cs);
}

/* ---- INIT_ONCE -> mutex-guarded flag ------------------------------------ */

typedef struct shim_init_once {
    pthread_mutex_t lock;
    int done;
} INIT_ONCE, *PINIT_ONCE;

#define INIT_ONCE_STATIC_INIT { PTHREAD_MUTEX_INITIALIZER, 0 }

typedef BOOL (CALLBACK *PINIT_ONCE_FN)(PINIT_ONCE once, PVOID param, PVOID* context);

static inline BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn,
    PVOID param, PVOID* context) {
    pthread_mutex_lock(&once->lock);
    if (!once->done) {
        fn(once, param, context);
        once->done = 1;
    }
    pthread_mutex_unlock(&once->lock);
    return TRUE;
}

/* ---- ticks + atomics ----------------------------------------------------- */

/* Monotonic milliseconds + offset the test can advance to fake elapsed time
   (entry expiry is 120 s — nobody wants to sleep that long in a unit test). */
ULONGLONG GetTickCount64(void);
void win32_shim_advance_ms(ULONGLONG ms);

static inline long InterlockedIncrement(volatile long* value) {
    return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static inline long InterlockedExchange(volatile long* target, long value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

static inline long InterlockedCompareExchange(volatile long* dest, long exchange,
    long comparand) {
    __atomic_compare_exchange_n(dest, &comparand, exchange, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}

/* ---- keyboard state (voice_modes hotkey poll — unused in unit tests) ------ */
/* voice_modes.c compiles its hotkey helpers even when the tests only exercise
   the distance logic; these no-op stubs let it link on the host. */
typedef short SHORT;

#define VK_NEXT 0x22
#define VK_END  0x23
#define VK_DOWN 0x28

static inline SHORT GetAsyncKeyState(int vKey) { (void)vKey; return 0; }
static inline SHORT GetKeyState(int nVirtKey)  { (void)nVirtKey; return 0; }

#endif /* TESTS_SUPPORT_WIN32_SHIM_WINDOWS_H */
