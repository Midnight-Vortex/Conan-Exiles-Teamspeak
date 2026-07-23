#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "core/config/config.h"

#include <stdio.h>
#include <wchar.h>

/* In-game chat display + patch-saved probe. Callable from UI / callback threads
   (displayInChat routes to TS adapter which enforces callback thread internally). */

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

void displayHubParametersConfirmation(BOOL globalSuccess, BOOL racesSuccess,
    BOOL playerInRace, BOOL zonesSuccess) {
    char confirmMsg[1024] = "";

    strcat_s(confirmMsg, sizeof(confirmMsg), "Root Parameters: ");
    strcat_s(confirmMsg, sizeof(confirmMsg), globalSuccess ? "GLOBAL✓ " : "GLOBAL✗ ");

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

int isPatchAlreadySaved(void) {
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
            wchar_t* value = line + 10;
            while (*value == L' ' || *value == L'\t') {
                value++;
            }
            if (*value != L'\0' && *value != L'\n' && *value != L'\r') {
                found = 1;
                break;
            }
        }
    }
    fclose(file);
    return found;
}
