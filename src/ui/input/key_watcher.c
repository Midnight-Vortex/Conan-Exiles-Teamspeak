#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "plugin_modules.h"
#include "core/voice/voice_modes.h"
#include "core/config/config.h"
#include "core/util/poll_interval.h"
#include "ui/util/ui_spin_lock.h"

#include <process.h>
#include <stdio.h>

// MODULE 17: HOTKEY WATCHER
// EN: Config hotkey (F10 default) + voice-mode hotkeys.
// ============================================================================

/*
 * Thread contract (review-fix 088):
 *  - keyMonitorThreadFunction: its own thread (the ONLY hotkey poller). Touches
 *    no TS API; it opens the F10 dialog (settings_dialog_open_async) and polls
 *    the voice-mode hotkeys (voice_modes.c, queue/flag based).
 *  - start/stopKeyMonitorThread / install/removeKeyMonitoring: callback thread
 *    (init/shutdown) or a settings-dialog thread. Serialised by
 *    g_keyMonitorLock, so concurrent stop/start can neither double-close the
 *    handle nor leave an un-joined thread. stop JOINS while holding the lock
 *    (bounded by the thread's PLUGIN_POLL_INTERVAL_MS loop); the key thread
 *    never takes this lock, so there is no lock cycle.
 *  - settings_dialog_open_async: callable from the key-monitor thread, the TS
 *    callback thread (menu item / configure) and TS's configure thread — but
 *    ONLY after the caller won config_dialog_try_open(). The dialog thread is
 *    the single place that releases that guard (config_dialog_close() after
 *    showConfigInterface() returned); open_async releases it itself only when
 *    it refuses to start a thread.
 *  - settings_dialog_shutdown: callback thread, shutdown only.
 */

/* Settings dialog thread handle (V8.8): _beginthreadex so the handle is
   joinable — _beginthread hands ownership to the CRT, which closes it when
   the thread exits, so waiting on it races a recycled handle. Only one
   dialog thread exists at a time (config_dialog_try_open gate). The handle
   slot is guarded by g_settingsDialogLock (open_async may run on several
   threads over the plugin's lifetime, shutdown on yet another). */
static HANDLE g_settingsDialogThread = NULL;
static UiSpinLock g_settingsDialogLock = UI_SPIN_LOCK_INIT;

static unsigned __stdcall settings_dialog_thread(void* arg) {
    (void)arg;
    showConfigInterface();
    /* C7: the ONLY release of the dialog guard. Releasing it earlier (inside
       showConfigInterface) would let the next F10 press start a second dialog
       thread whose guard this line then clears. */
    config_dialog_close();
    return 0;
}

void settings_dialog_open_async(void) {
    ui_spin_lock(&g_settingsDialogLock);
    if (pluginShuttingDown) {
        /* Shutdown began: no new dialog threads (settings_dialog_shutdown may
           already have joined). Give back the guard the caller took. */
        ui_spin_unlock(&g_settingsDialogLock);
        config_dialog_close();
        return;
    }
    /* The previous dialog thread (if any) has released the dialog guard —
       that is its last action, so it is exiting; reap its handle before
       storing a new one (the wait is momentary). */
    if (g_settingsDialogThread) {
        WaitForSingleObject(g_settingsDialogThread, INFINITE);
        CloseHandle(g_settingsDialogThread);
        g_settingsDialogThread = NULL;
    }
    g_settingsDialogThread = (HANDLE)_beginthreadex(NULL, 0, settings_dialog_thread, NULL, 0, NULL);
    const int started = g_settingsDialogThread != NULL;
    ui_spin_unlock(&g_settingsDialogLock);
    if (!started) {
        config_dialog_close();
    }
}

/* Shutdown: close the F10 dialog (its GetMessage loop only ends via
   WM_CLOSE -> WM_DESTROY -> PostQuitMessage) and JOIN the dialog thread so
   it is out of plugin code before the DLL unloads. A plain join without the
   close would block on the user-driven modal loop forever; the WM_CLOSE is
   re-posted in the wait loop because the window may not exist yet when the
   first post happens (thread still inside showConfigInterface setup).
   Works for a dialog started by ANY caller of settings_dialog_open_async:
   the handle is taken under the same lock open_async stores it under, and
   open_async refuses to start once pluginShuttingDown is set. */
void settings_dialog_shutdown(void) {
    ui_spin_lock(&g_settingsDialogLock);
    HANDLE dialogThread = g_settingsDialogThread;
    g_settingsDialogThread = NULL;
    ui_spin_unlock(&g_settingsDialogLock);
    if (!dialogThread) {
        return;
    }
    DWORD waitedMs = 0;
    for (;;) {
        HWND dlg = hConfigDialog;
        if (dlg && IsWindow(dlg)) {
            PostMessageW(dlg, WM_CLOSE, 0, 0);
        }
        if (WaitForSingleObject(dialogThread, 200) == WAIT_OBJECT_0) {
            break;
        }
        waitedMs += 200;
        if (waitedMs == 10000) {
            ts3_debug_log("SHUTDOWN: settings dialog thread slow to exit - waiting");
        }
    }
    CloseHandle(dialogThread);
}

static unsigned __stdcall keyMonitorThreadFunction(void* arg) {
    (void)arg;

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread: Started with ultra-reactive detection");
    }

    while (InterlockedCompareExchange(&keyMonitorThreadRunning, 0, 0)) {
        if (pluginShuttingDown) {
            break;
        }
        const BOOL currentKeyState = (GetAsyncKeyState(g_config.configUIKey) & 0x8000) != 0;

        if (currentKeyState && !lastKeyState) {
            if (config_dialog_try_open()) {
                /* open_async refuses (and releases the guard) once shutdown began. */
                if (enableLogGeneral) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "KEY INSTANT-DETECTED! %s (VK:%d) - opening settings...",
                        getKeyName(g_config.configUIKey), g_config.configUIKey);
                    mumbleAPI.log(ownID, msg);
                }

                settings_dialog_open_async();
            }
        }

        lastKeyState = currentKeyState;
        voice_mode_hotkey_poll();
        Sleep(PLUGIN_POLL_INTERVAL_MS);
    }

    InterlockedExchange(&keyMonitorThreadRunning, 0);

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread: Stopped");
    }
    return 0;
}

/* Start/stop are serialised by g_keyMonitorLock: the handle slot
   (keyMonitorThread) and the running flag change together, so a concurrent
   stop+start can neither close one handle twice nor spawn a second thread
   while the first is still exiting. */
static UiSpinLock g_keyMonitorLock = UI_SPIN_LOCK_INIT;

void startKeyMonitorThread(void) {
    ui_spin_lock(&g_keyMonitorLock);
    /* Once shutdown began nothing may (re)start — shutdown's stop must be the
       last word, otherwise a late start leaves a thread nobody joins. */
    if (pluginShuttingDown) {
        ui_spin_unlock(&g_keyMonitorLock);
        return;
    }
    if (keyMonitorThread != NULL) {
        if (InterlockedCompareExchange(&keyMonitorThreadRunning, 0, 0)) {
            ui_spin_unlock(&g_keyMonitorLock);
            return; /* already running */
        }
        /* Handle of a thread that already ended on its own — reap it. */
        WaitForSingleObject(keyMonitorThread, INFINITE);
        CloseHandle(keyMonitorThread);
        keyMonitorThread = NULL;
    }
    /* Claim the running flag BEFORE spawning so the thread never sees 0 on its
       first check (the V7 version set it inside the thread). */
    InterlockedExchange(&keyMonitorThreadRunning, 1);
    /* _beginthreadex: joinable handle we own (see settings thread note). */
    keyMonitorThread = (HANDLE)_beginthreadex(NULL, 0, keyMonitorThreadFunction, NULL, 0, NULL);
    if (!keyMonitorThread) {
        InterlockedExchange(&keyMonitorThreadRunning, 0);
        ui_spin_unlock(&g_keyMonitorLock);
        return;
    }
    ui_spin_unlock(&g_keyMonitorLock);

    if (enableLogGeneral) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Key monitor thread started for key: %s (VK:%d)",
            getKeyName(g_config.configUIKey), g_config.configUIKey);
        mumbleAPI.log(ownID, msg);
    }
}

void stopKeyMonitorThread(void) {
    ui_spin_lock(&g_keyMonitorLock);
    InterlockedExchange(&keyMonitorThreadRunning, 0);
    const HANDLE stopped = keyMonitorThread;
    if (stopped != NULL) {
        /* JOIN while holding the lock: a second stop (or a start) waits here
           until this one is done, so "stop returned" always means "thread is
           gone". The loop wakes every PLUGIN_POLL_INTERVAL_MS, so this is
           quick; fall back to an unbounded wait rather than closing the
           handle of a still-running thread. */
        if (WaitForSingleObject(stopped, 5000) != WAIT_OBJECT_0) {
            ts3_debug_log("SHUTDOWN: key monitor thread slow to exit - waiting");
            WaitForSingleObject(stopped, INFINITE);
        }
        CloseHandle(stopped);
        keyMonitorThread = NULL;
    }
    ui_spin_unlock(&g_keyMonitorLock);

    if (stopped != NULL && enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread stopped");
    }
}

void installKeyMonitoring(void) {
    startKeyMonitorThread();
}

void removeKeyMonitoring(void) {
    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Removing key monitoring");
    }
    stopKeyMonitorThread();
}
