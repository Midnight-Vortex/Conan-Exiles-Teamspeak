#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "plugin_modules.h"
#include "core/voice/voice_modes.h"
#include "core/util/poll_interval.h"

#include <process.h>
#include <stdio.h>

// MODULE 17: HOTKEY WATCHER
// EN: Config hotkey (F10 default) + voice-mode hotkeys.
// ============================================================================

static void settings_dialog_thread(void* arg) {
    (void)arg;
    showConfigInterface();
    config_dialog_close();
}

void keyMonitorThreadFunction(void* arg) {
    (void)arg;
    keyMonitorThreadRunning = TRUE;

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread: Started with ultra-reactive detection");
    }

    while (keyMonitorThreadRunning) {
        if (pluginShuttingDown) {
            break;
        }
        const BOOL currentKeyState = (GetAsyncKeyState(configUIKey) & 0x8000) != 0;

        if (currentKeyState && !lastKeyState) {
            if (config_dialog_try_open() && !pluginShuttingDown) {
                if (enableLogGeneral) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "KEY INSTANT-DETECTED! %s (VK:%d) - opening settings...",
                        getKeyName(configUIKey), configUIKey);
                    mumbleAPI.log(ownID, msg);
                }

                _beginthread(settings_dialog_thread, 0, NULL);
            }
        }

        lastKeyState = currentKeyState;
        voice_mode_hotkey_poll();
        Sleep(PLUGIN_POLL_INTERVAL_MS);
    }

    keyMonitorThreadRunning = FALSE;

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread: Stopped");
    }
}

void startKeyMonitorThread(void) {
    if (!keyMonitorThreadRunning) {
        keyMonitorThread = (HANDLE)_beginthread(keyMonitorThreadFunction, 0, NULL);

        if (enableLogGeneral) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Key monitor thread started for key: %s (VK:%d)",
                getKeyName(configUIKey), configUIKey);
            mumbleAPI.log(ownID, msg);
        }
    }
}

void stopKeyMonitorThread(void) {
    keyMonitorThreadRunning = FALSE;
    if (keyMonitorThread != NULL) {
        WaitForSingleObject(keyMonitorThread, 2000);
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
