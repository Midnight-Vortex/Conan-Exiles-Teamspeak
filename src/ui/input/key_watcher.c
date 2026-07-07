#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/proximity/proximity_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <process.h>
#include <ole2.h>

// MODULE 17: HOTKEY WATCHER
// EN: F10 settings dialog, voice-mode hotkeys — runs on dedicated key-monitor thread.
// FR: Dialogue réglages F10, raccourcis mode voix — tourne sur thread dédié surveillance touches.
// ============================================================================

// Key monitoring thread | Thread de surveillance des touches
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
        BOOL currentKeyState = (GetAsyncKeyState(configUIKey) & 0x8000) != 0;

        // Rising edge detection | Détection de front montant
        if (currentKeyState && !lastKeyState) {
            if (!isConfigDialogOpen && !pluginShuttingDown) {
                isConfigDialogOpen = TRUE;

                if (enableLogGeneral) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "KEY INSTANT-DETECTED! %s (VK:%d) - opening interface immediately...",
                        getKeyName(configUIKey), configUIKey);
                    mumbleAPI.log(ownID, msg);
                }

                _beginthread(showPathSelectionDialogThread, 0, NULL);
            }
        }

        lastKeyState = currentKeyState;
        Sleep(30);
    }

    keyMonitorThreadRunning = FALSE;

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Key monitor thread: Stopped");
    }
}


// Start monitoring thread | Démarrer le thread de surveillance
void startKeyMonitorThread() {
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


// Stop monitoring thread | Arrêter le thread de surveillance
void stopKeyMonitorThread() {
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

// Install key monitoring | Installer la surveillance des touches
void installKeyMonitoring() {
    if (enableLogGeneral) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Installing key monitoring for: %s (VK:%d)",
            getKeyName(configUIKey), configUIKey);
        mumbleAPI.log(ownID, msg);
    }

    startKeyMonitorThread();
}

// Remove key monitoring | Supprimer la surveillance des touches
void removeKeyMonitoring() {
    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Removing key monitoring");
    }

    stopKeyMonitorThread();
}

// ============================================================================
