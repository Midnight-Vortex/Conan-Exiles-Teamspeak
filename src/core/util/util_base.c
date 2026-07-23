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

// MODULE 1: BASE UTILITIES (shared helpers)
// EN: Config folder paths, in-game chat messages, 3D distance, server hash tracking.
// FR: Chemins dossier config, messages chat ingame, distance 3D, suivi hash serveur.
// ============================================================================
// Convert key codes to names | Conversion des codes de touches en noms
const char* getKeyName(int vkCode) {
    switch (vkCode) {
    case 17: return "Ctrl";
    case 16: return "Shift";
    case 18: return "Alt";
    case 32: return "Space";
    case 13: return "Enter";
    case 27: return "Escape";
    case 9: return "Tab";
    case 8: return "Backspace";
    case 46: return "Delete";
    case 36: return "Home";
    case 35: return "End";
    case 33: return "Page Up";
    case 34: return "Page Down";
    case 45: return "Insert";
    case 20: return "Caps Lock";
    case 144: return "Num Lock";
    case 145: return "Scroll Lock";
    case 37: return "Left Arrow";
    case 38: return "Up Arrow";
    case 39: return "Right Arrow";
    case 40: return "Down Arrow";
    case 112: return "F1"; case 113: return "F2"; case 114: return "F3"; case 115: return "F4";
    case 116: return "F5"; case 117: return "F6"; case 118: return "F7"; case 119: return "F8";
    case 120: return "F9"; case 121: return "F10"; case 122: return "F11"; case 123: return "F12";
    case 65: return "A"; case 66: return "B"; case 67: return "C"; case 68: return "D";
    case 69: return "E"; case 70: return "F"; case 71: return "G"; case 72: return "H";
    case 73: return "I"; case 74: return "J"; case 75: return "K"; case 76: return "L";
    case 77: return "M"; case 78: return "N"; case 79: return "O"; case 80: return "P";
    case 81: return "Q"; case 82: return "R"; case 83: return "S"; case 84: return "T";
    case 85: return "U"; case 86: return "V"; case 87: return "W"; case 88: return "X";
    case 89: return "Y"; case 90: return "Z";
    case 48: return "0"; case 49: return "1"; case 50: return "2"; case 51: return "3";
    case 52: return "4"; case 53: return "5"; case 54: return "6"; case 55: return "7";
    case 56: return "8"; case 57: return "9";
    case 96: return "Num 0"; case 97: return "Num 1"; case 98: return "Num 2"; case 99: return "Num 3";
    case 100: return "Num 4"; case 101: return "Num 5"; case 102: return "Num 6"; case 103: return "Num 7";
    case 104: return "Num 8"; case 105: return "Num 9"; case 106: return "Num *"; case 107: return "Num +";
    case 109: return "Num -"; case 110: return "Num ."; case 111: return "Num /";
    case 186: return ";"; case 187: return "="; case 188: return ","; case 189: return "-";
    case 190: return "."; case 191: return "/"; case 192: return "`"; case 219: return "[";
    case 220: return "\\"; case 221: return "]"; case 222: return "'";
    case 1: return "Left Click"; case 2: return "Right Click"; case 4: return "Middle Click";
    case 5: return "X1 Mouse"; case 6: return "X2 Mouse";
    case 0: return "None";
    default: {
        static char buffer[16];
        snprintf(buffer, sizeof(buffer), "Key_%d", vkCode);
        return buffer;
    }
    }
}

#ifdef CONAN_EXILES_TS_EXPORTS
#define TS3_CHAT_QUEUE_SIZE 32
static char ts3ChatQueue[TS3_CHAT_QUEUE_SIZE][512];
static volatile long ts3ChatQueueHead = 0;
static volatile long ts3ChatQueueTail = 0;

int ts3_is_plugin_data_lock_held_by_this_thread(void);

void ts3_queue_chat_message(const char* message) {
    if (!message) {
        return;
    }
    long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
    long next = (tail + 1) % TS3_CHAT_QUEUE_SIZE;
    if (next == InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0)) {
        ts3_debug_log("TS-CHAT: queue full — message dropped");
        return;
    }
    strncpy_s(ts3ChatQueue[tail], sizeof(ts3ChatQueue[tail]), message, _TRUNCATE);
    InterlockedExchange(&ts3ChatQueueTail, next);
}

int ts3_plugin_has_pending_chat(void) {
    long head = InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0);
    long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
    return head != tail;
}

void ts3_plugin_clear_pending_chat(void) {
    InterlockedExchange(&ts3ChatQueueHead, 0);
    InterlockedExchange(&ts3ChatQueueTail, 0);
}

void ts3_plugin_flush_pending_chat(void) {
    if (!ts3_plugin_is_on_callback_thread()) {
        return;
    }
    for (;;) {
        long head = InterlockedCompareExchange(&ts3ChatQueueHead, 0, 0);
        long tail = InterlockedCompareExchange(&ts3ChatQueueTail, 0, 0);
        if (head == tail) {
            break;
        }
        ts3_debug_logf("TS-CHAT: flush '%.120s'", ts3ChatQueue[head]);
        ts3_adapter_print_chat_force(ts3ChatQueue[head]);
        InterlockedExchange(&ts3ChatQueueHead, (head + 1) % TS3_CHAT_QUEUE_SIZE);
    }
}
#endif

// Display chat message | Afficher un message de chat
void displayInChat(const char* message) {
    if (!message) {
        return;
    }
#ifdef CONAN_EXILES_TS_EXPORTS
    ts3_adapter_print_chat(message);
#else
    mumbleAPI.log(ownID, message);
#endif
}

// Display hub parameters confirmation in chat | Afficher la confirmation des paramètres du hub dans le chat
void displayHubParametersConfirmation(BOOL globalSuccess, BOOL racesSuccess, BOOL playerInRace, BOOL zonesSuccess) {
    char confirmMsg[1024] = "";

    // Build confirmation message | Construire le message de confirmation
    strcat_s(confirmMsg, sizeof(confirmMsg), "Root Parameters: ");

    // Global parameters status | Statut des paramètres globaux
    strcat_s(confirmMsg, sizeof(confirmMsg), globalSuccess ? "GLOBAL✓ " : "GLOBAL✗ ");

    // Races status | Statut des races
    if (racesSuccess) {
        char raceMsg[128];
        if (playerInRace && currentPlayerRaceIndex != -1) {
            snprintf(raceMsg, sizeof(raceMsg), "RACES(%zu)✓ [YOUR RACE: %s] ",
                raceCount, races[currentPlayerRaceIndex].name);
        }
        else if (raceCount > 0) {
            snprintf(raceMsg, sizeof(raceMsg), "RACES(%zu)✓ [NOT IN RACE] ", raceCount);
        }
        else {
            snprintf(raceMsg, sizeof(raceMsg), "RACES(0)✓ ");
        }
        strcat_s(confirmMsg, sizeof(confirmMsg), raceMsg);
    }
    else {
        strcat_s(confirmMsg, sizeof(confirmMsg), "RACES✗ ");
    }

    // Zones status | Statut des zones
    if (zonesSuccess) {
        char zoneMsg[64];
        snprintf(zoneMsg, sizeof(zoneMsg), "ZONES(%zu)✓", zoneCount);
        strcat_s(confirmMsg, sizeof(confirmMsg), zoneMsg);
    }
    else {
        strcat_s(confirmMsg, sizeof(confirmMsg), "ZONES✗");
    }

    displayInChat(confirmMsg);
}

// Check if patch is already saved | Vérifier si le patch est déjà sauvegardé
int isPatchAlreadySaved() {
    const wchar_t* configFolder = config_get_folder_path();
    if (!configFolder) {
        return 0;
    }

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);

    FILE* file = _wfopen(configFile, L"r");
    if (!file) {
        return 0;
    }
    wchar_t line[512];
    int found = 0;
    while (fgetws(line, 512, file)) {
        if (wcsncmp(line, L"SavedPath=", 10) == 0) {
            // Check if there's content after 'SavedPath=' | Vérifier qu'il y a du contenu après 'SavedPath='
            wchar_t* value = line + 10;
            while (*value == L' ' || *value == L'\t') value++;
            if (*value != L'\0' && *value != L'\n' && *value != L'\r') {
                found = 1;
                break;
            }
        }
    }
    fclose(file);
    return found;
}

// Count significant digits in value | Compter les chiffres significatifs dans une valeur
int countSignificantDigits(float value) {
    if (value == 0.0f) return 1;

    int integerPart = (int)value;
    if (integerPart == 0) return 1;

    int digitCount = 0;
    while (integerPart > 0) {
        digitCount++;
        integerPart /= 10;
    }
    return digitCount;
}

// Get server hash for tracking default settings | Obtenir le hash du serveur pour le suivi des paramètres par défaut
BOOL getServerHashForTracking(mumble_connection_t connection, char* outHash, size_t hashSize) {
    if (!outHash || hashSize < 65) {
        if (outHash && hashSize > 0) outHash[0] = '\0';
        return FALSE;
    }

    const char* serverHash = NULL;
    mumble_error_t result = mumbleAPI.getServerHash(ownID, connection, &serverHash);

    if (result != MUMBLE_STATUS_OK || !serverHash) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Failed to retrieve server hash from Mumble API");
        }
        outHash[0] = '\0';
        return FALSE;
    }

    // Copy server hash to output buffer | Copier le hash du serveur au buffer de sortie
    strncpy_s(outHash, hashSize, serverHash, _TRUNCATE);

    // Free Mumble API memory | Libérer la mémoire de l'API Mumble
    mumbleAPI.freeMemory(ownID, serverHash);

    if (enableLogGeneral) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg), "Server hash retrieved: %s", outHash);
        mumbleAPI.log(ownID, logMsg);
    }

    return TRUE;
}

// ============================================================================
