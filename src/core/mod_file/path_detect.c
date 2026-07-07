#include "core/mod_file/path_detect.h"
#include "core/util/log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CONAN_APPID_LINE L"\"440900\""

/* Steam install folder from the registry. Returns 1 on success. */
static int detect_steam_install_path(wchar_t* out, size_t outLen) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
            0, KEY_READ, &hKey) != ERROR_SUCCESS
        && RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return 0;
    }

    wchar_t installPath[MAX_PATH] = L"";
    DWORD dataSize = sizeof(installPath);
    const LONG result = RegQueryValueExW(hKey, L"InstallPath", NULL, NULL,
        (LPBYTE)installPath, &dataSize);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS || !installPath[0]) {
        return 0;
    }
    installPath[MAX_PATH - 1] = L'\0';
    swprintf(out, outLen, L"%s", installPath);
    return 1;
}

/* Extract the value of a `"key"  "value"` VDF line into out. Returns 1 on
   success. p must point at the opening quote of the key. */
static int vdf_line_value(const wchar_t* p, wchar_t* out, size_t outLen) {
    const wchar_t* q1 = wcschr(p, L'"');            /* "key */
    if (!q1) return 0;
    const wchar_t* q2 = wcschr(q1 + 1, L'"');       /* key" */
    if (!q2) return 0;
    const wchar_t* q3 = wcschr(q2 + 1, L'"');       /* "value */
    if (!q3) return 0;
    const wchar_t* q4 = wcschr(q3 + 1, L'"');       /* value" */
    if (!q4) return 0;

    const size_t len = (size_t)(q4 - (q3 + 1));
    if (len == 0 || len >= outLen) {
        return 0;
    }
    wcsncpy_s(out, outLen, q3 + 1, len);

    /* VDF escapes backslashes: "C:\\\\Games" -> C:\Games. */
    size_t w = 0;
    for (size_t r = 0; out[r] != L'\0'; r++) {
        out[w++] = out[r];
        if (out[r] == L'\\' && out[r + 1] == L'\\') {
            r++;
        }
    }
    out[w] = L'\0';
    return w > 0;
}

/* Scan libraryfolders.vdf for the library that contains appid 440900 and
   build + verify the Saved path. Returns 1 on success. */
static int detect_from_library_vdf(const wchar_t* vdfPath, wchar_t* out, size_t outLen) {
    FILE* f = _wfopen(vdfPath, L"r, ccs=UTF-8");
    if (!f) {
        return 0;
    }

    wchar_t line[1024];
    wchar_t libraryPath[MAX_PATH] = L"";
    int found = 0;

    while (!found && fgetws(line, 1024, f)) {
        const wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') {
            p++;
        }

        if (wcsncmp(p, L"\"path\"", 6) == 0) {
            if (!vdf_line_value(p, libraryPath, MAX_PATH)) {
                libraryPath[0] = L'\0';
            }
            continue;
        }

        if (libraryPath[0] && wcsncmp(p, CONAN_APPID_LINE, 8) == 0) {
            wchar_t candidate[MAX_PATH];
            swprintf(candidate, MAX_PATH,
                L"%s\\steamapps\\common\\Conan Exiles\\ConanSandbox\\Saved", libraryPath);
            const DWORD attribs = GetFileAttributesW(candidate);
            if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
                swprintf(out, outLen, L"%s", candidate);
                found = 1;
            }
        }
    }

    fclose(f);
    return found;
}

int path_detect_conan_saved(wchar_t* out, size_t outLen) {
    if (!out || outLen == 0) {
        return 0;
    }
    out[0] = L'\0';

    wchar_t steamPath[MAX_PATH] = L"";
    if (!detect_steam_install_path(steamPath, MAX_PATH)) {
        static int s_loggedSteamMissing = 0;
        if (!s_loggedSteamMissing) {
            s_loggedSteamMissing = 1;
            log_debug("PATH: Steam not found in registry");
        }
        return 0;
    }

    wchar_t vdfPath[MAX_PATH];
    swprintf(vdfPath, MAX_PATH, L"%s\\steamapps\\libraryfolders.vdf", steamPath);
    if (GetFileAttributesW(vdfPath) == INVALID_FILE_ATTRIBUTES) {
        log_debug("PATH: libraryfolders.vdf missing at %ls", vdfPath);
        return 0;
    }

    if (!detect_from_library_vdf(vdfPath, out, outLen)) {
        log_write("PATH: Conan Exiles (440900) not found in any Steam library");
        return 0;
    }

    log_write("PATH: auto-detected Conan Saved folder: %ls", out);
    return 1;
}
