#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "plugin_modules.h"
#include "core/voice/voice_modes.h"
#include "core/config/config.h"
#include "core/util/poll_interval.h"

#include <process.h>
#include <stdio.h>

// MODULE 17: HOTKEY WATCHER
// EN: Config hotkey (F10 default) + voice-mode hotkeys.
// ============================================================================

/* Settings dialog thread handle (V8.8): _beginthreadex so the handle is
   joinable — _beginthread hands ownership to the CRT, which closes it when
   the thread exits, so waiting on it races a recycled handle. Only one
   dialog thread exists at a time (config_dialog_try_open gate). */
static HANDLE g_settingsDialogThread = NULL;

static unsigned __stdcall settings_dialog_thread(void* arg) {
    (void)arg;
    showConfigInterface();
    config_dialog_close();
    return 0;
}

void settings_dialog_open_async(void) {
    /* The previous dialog thread (if any) has released the dialog guard —
       it is exiting; reap its handle before storing a new one. */
    if (g_settingsDialogThread) {
        WaitForSingleObject(g_settingsDialogThread, INFINITE);
        CloseHandle(g_settingsDialogThread);
        g_settingsDialogThread = NULL;
    }
    g_settingsDialogThread = (HANDLE)_beginthreadex(NULL, 0, settings_dialog_thread, NULL, 0, NULL);
    if (!g_settingsDialogThread) {
        config_dialog_close();
    }
}

/* Shutdown: close the F10 dialog (its GetMessage loop only ends via
   WM_CLOSE -> WM_DESTROY -> PostQuitMessage) and JOIN the dialog thread so
   it is out of plugin code before the DLL unloads. A plain join without the
   close would block on the user-driven modal loop forever; the WM_CLOSE is
   re-posted in the wait loop because the window may not exist yet when the
   first post happens (thread still inside showConfigInterface setup). */
void settings_dialog_shutdown(void) {
    if (!g_settingsDialogThread) {
        return;
    }
    DWORD waitedMs = 0;
    for (;;) {
        HWND dlg = hConfigDialog;
        if (dlg && IsWindow(dlg)) {
            PostMessageW(dlg, WM_CLOSE, 0, 0);
        }
        if (WaitForSingleObject(g_settingsDialogThread, 200) == WAIT_OBJECT_0) {
            break;
        }
        waitedMs += 200;
        if (waitedMs == 10000) {
            ts3_debug_log("SHUTDOWN: settings dialog thread slow to exit - waiting");
        }
    }
    CloseHandle(g_settingsDialogThread);
    g_settingsDialogThread = NULL;
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
            if (config_dialog_try_open() && !pluginShuttingDown) {
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

void startKeyMonitorThread(void) {
    /* Claim the running flag BEFORE spawning so a double start cannot race
       the thread's own flag write (the V7 version set it inside the thread). */
    if (InterlockedCompareExchange(&keyMonitorThreadRunning, 1, 0) != 0) {
        return;
    }
    /* _beginthreadex: joinable handle we own (see settings thread note). */
    keyMonitorThread = (HANDLE)_beginthreadex(NULL, 0, keyMonitorThreadFunction, NULL, 0, NULL);
    if (!keyMonitorThread) {
        InterlockedExchange(&keyMonitorThreadRunning, 0);
        return;
    }

    if (enableLogGeneral) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Key monitor thread started for key: %s (VK:%d)",
            getKeyName(g_config.configUIKey), g_config.configUIKey);
        mumbleAPI.log(ownID, msg);
    }
}

void stopKeyMonitorThread(void) {
    InterlockedExchange(&keyMonitorThreadRunning, 0);
    if (keyMonitorThread != NULL) {
        /* JOIN: the loop wakes every PLUGIN_POLL_INTERVAL_MS, so this is
           quick; fall back to an unbounded wait rather than closing the
           handle of a still-running thread. */
        if (WaitForSingleObject(keyMonitorThread, 5000) != WAIT_OBJECT_0) {
            ts3_debug_log("SHUTDOWN: key monitor thread slow to exit - waiting");
            WaitForSingleObject(keyMonitorThread, INFINITE);
        }
        CloseHandle(keyMonitorThread);
        keyMonitorThread = NULL;

        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Key monitor thread stopped");
        }
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
