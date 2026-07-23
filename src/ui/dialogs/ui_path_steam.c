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
#if defined(_MSC_VER)
#pragma warning(disable : 4456) /* legacy UI reuses block-scoped names */
#endif
#include <uxtheme.h>
#if defined(_MSC_VER)
#pragma comment(lib, "uxtheme.lib")
#endif
#include "ui_config_internal.h"

/*
 * ui_path_steam.c: game-folder browse + Steam auto-detect.
 * ui_path_steam.c : parcours du dossier + auto-detection Steam.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
 */

// Modern folder browser | Fonction pour parcourir les dossiers (moderne)
void browseSavedPath(HWND hwnd) {
    // ✅ 1. Initialiser COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        MessageBoxW(hwnd, L"Failed to initialize COM", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    // ✅ 2. Créer le dialogue de sélection de fichier
    IFileOpenDialog* pFileOpen = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        &IID_IFileOpenDialog, (void**)&pFileOpen);

    if (SUCCEEDED(hr)) {
        // ✅ 3. Configurer pour sélectionner des DOSSIERS (pas des fichiers)
        DWORD dwOptions;
        hr = pFileOpen->lpVtbl->GetOptions(pFileOpen, &dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->SetOptions(pFileOpen, dwOptions | FOS_PICKFOLDERS);
        }

        // ✅ 4. Définir le titre du dialogue
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->SetTitle(pFileOpen, L"Select your Conan Exiles game folder");
        }

        // ✅ 5. Afficher le dialogue
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->Show(pFileOpen, hwnd);

            // ✅ 6. Récupérer le chemin sélectionné SI l'utilisateur a cliqué OK
            if (SUCCEEDED(hr)) {
                IShellItem* pItem = NULL;
                hr = pFileOpen->lpVtbl->GetResult(pFileOpen, &pItem);

                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath = NULL;
                    hr = pItem->lpVtbl->GetDisplayName(pItem, SIGDN_FILESYSPATH, &pszFilePath);

                    // ✅ 7. Stocker dans une variable TEMPORAIRE pour éviter d'écraser displayedPathText
                    if (SUCCEEDED(hr) && pszFilePath) {
                        wchar_t tempSelectedPath[MAX_PATH];
                        wcscpy_s(tempSelectedPath, MAX_PATH, pszFilePath);

                        // ✅ Afficher seulement le dossier du jeu (SANS \ConanSandbox\Saved)
                        wcscpy_s(displayedPathText, MAX_PATH, tempSelectedPath);

                        // ✅ Forcer le redessin de l'image pour afficher le nouveau texte
                        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                            InvalidateRect(hSavedPathBg, NULL, TRUE);
                            UpdateWindow(hSavedPathBg);
                        }

                        CoTaskMemFree(pszFilePath);
                    }

                    pItem->lpVtbl->Release(pItem);
                }
            }
        }

        pFileOpen->lpVtbl->Release(pFileOpen);
    }

    // ✅ 8. Libérer COM
    CoUninitialize();
}

// Modern folder browser | Explorateur de dossier moderne
void browseFolderModern(HWND hwnd) {
    IFileDialog* pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void**)&pfd);
    if (SUCCEEDED(hr)) {
        DWORD options;
        pfd->lpVtbl->GetOptions(pfd, &options);
        pfd->lpVtbl->SetOptions(pfd, options | FOS_PICKFOLDERS);
        pfd->lpVtbl->SetTitle(pfd, L"Select the Conan Exiles folder");
        hr = pfd->lpVtbl->Show(pfd, hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* psi;
            hr = pfd->lpVtbl->GetResult(pfd, &psi);
            if (SUCCEEDED(hr)) {
                wchar_t* path = NULL;
                hr = psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &path);
                if (SUCCEEDED(hr) && path) {
                    SetWindowTextW(hSavedPathEdit, path);
                    CoTaskMemFree(path);
                }
                psi->lpVtbl->Release(psi);
            }
        }
        pfd->lpVtbl->Release(pfd);
    }
}

// Check if Saved folder exists in game folder | Vérifier que le dossier Saved existe dans le dossier du jeu
int savedExistsInFolder(const wchar_t* folderPath) {
    wchar_t savedCheckPath[MAX_PATH];
    swprintf(savedCheckPath, MAX_PATH, L"%s\\ConanSandbox\\Saved", folderPath);
    DWORD attribs = GetFileAttributesW(savedCheckPath);
    return (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY));
}

// Find Steam installation path and parse libraries | Trouver le chemin d'installation Steam et parser les bibliothèques
BOOL findConanExilesAutomatic(wchar_t* outPath, size_t pathSize) {
    if (!outPath || pathSize == 0) return FALSE;

    // Debug: Log the registry key access | Déboguer: Logger l'accès aux clés du registre
    if (enableLogConfig) {
        mumbleAPI.log(ownID, "DEBUG: Attempting to read Steam registry keys...");
    }

    HKEY hKey = NULL;
    // Try 64-bit registry first | Essayer le registre 64-bit d'abord
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
        0, KEY_READ, &hKey);

    if (result != ERROR_SUCCESS) {
        // Try 32-bit registry | Essayer le registre 32-bit
        result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Valve\\Steam",
            0, KEY_READ, &hKey);
    }

    if (result != ERROR_SUCCESS) {
        if (enableLogConfig) {
            char errorMsg[256];
            snprintf(errorMsg, sizeof(errorMsg),
                "Registry: Steam installation key not found - Error code: %ld", result);
            mumbleAPI.log(ownID, errorMsg);
        }
        return FALSE;
    }

    wchar_t installPath[MAX_PATH] = L"";
    DWORD dataSize = sizeof(installPath);
    result = RegQueryValueExW(hKey, L"InstallPath", NULL, NULL,
        (LPBYTE)installPath, &dataSize);

    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS || wcslen(installPath) == 0) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "Registry: Steam InstallPath value not found");
        }
        return FALSE;
    }

    // Build path to libraryfolders.vdf | Construire le chemin vers libraryfolders.vdf
    wchar_t vdfPath[MAX_PATH];
    swprintf(vdfPath, MAX_PATH, L"%s\\steamapps\\libraryfolders.vdf", installPath);

    // Debug: Log VDF path | Déboguer: Logger le chemin du fichier VDF
    if (enableLogConfig) {
        char vdfPathUtf8[MAX_PATH];
        size_t converted = 0;
        wcstombs_s(&converted, vdfPathUtf8, MAX_PATH, vdfPath, _TRUNCATE);
        char debugMsg[512];
        snprintf(debugMsg, sizeof(debugMsg),
            "DEBUG: Looking for VDF file at: %s", vdfPathUtf8);
        mumbleAPI.log(ownID, debugMsg);
    }

    // Debug: Check if VDF file exists | Déboguer: Vérifier si le fichier VDF existe
    DWORD vdfAttribs = GetFileAttributesW(vdfPath);
    if (vdfAttribs == INVALID_FILE_ATTRIBUTES) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "DEBUG: VDF file does NOT exist at this location");
        }
        return FALSE;
    }
    else {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "DEBUG: VDF file found - proceeding to parse");
        }
    }

    // ✅ CORRECTION : Parse VDF et retourne le chemin COMPLET incluant \ConanSandbox\Saved
    return parseSteamLibraryFolders(vdfPath, outPath, pathSize);
}

// Parse Steam libraryfolders.vdf file | Parser le fichier libraryfolders.vdf de Steam
BOOL parseSteamLibraryFolders(const wchar_t* vdfPath, wchar_t* outConanPath, size_t pathSize) {
    if (!vdfPath || !outConanPath || pathSize == 0) return FALSE;

    FILE* file = _wfopen(vdfPath, L"r, ccs=UTF-8");
    if (!file) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "DEBUG: Failed to open libraryfolders.vdf");
        }
        return FALSE;
    }

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "DEBUG: Successfully opened libraryfolders.vdf - parsing...");
    }

    wchar_t line[1024];
    wchar_t currentLibraryPath[MAX_PATH] = L"";
    BOOL foundConanExiles = FALSE;
    int lineNumber = 0;
    int libraryDepth = 0; // ✅ Profondeur des accolades pour détecter les sections

    while (fgetws(line, 1024, file) && !foundConanExiles) {
        lineNumber++;
        wchar_t* p = line;

        // ✅ Nettoyer les espaces/tabs/retours à la ligne
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;

        // ✅ DÉTECTER LES ACCOLADES OUVRANTES/FERMANTES
        if (wcschr(p, L'{')) {
            libraryDepth++;
            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg), "DEBUG: Opening brace at line %d, depth=%d", lineNumber, libraryDepth);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        if (wcschr(p, L'}')) {
            libraryDepth--;

            // ✅ RÉINITIALISER LE CHEMIN QUAND ON SORT D'UNE BIBLIOTHÈQUE
            if (libraryDepth == 1) {
                if (enableLogConfig) {
                    mumbleAPI.log(ownID, "DEBUG: Exiting library section - resetting path");
                }
                currentLibraryPath[0] = L'\0';
            }
        }

        // ✅ CHERCHER "path" UNIQUEMENT SI DANS UNE BIBLIOTHÈQUE (depth >= 2)
        if (libraryDepth >= 2 && wcsncmp(p, L"\"path\"", 6) == 0) {
            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg), "DEBUG: Found 'path' line at line %d (depth=%d)", lineNumber, libraryDepth);
                mumbleAPI.log(ownID, logMsg);
            }

            // ✅ MÉTHODE ROBUSTE : Chercher les guillemets correctement
            wchar_t* firstQuote = wcschr(p, L'\"');       // Premier guillemet de "path"
            if (!firstQuote) continue;

            wchar_t* secondQuote = wcschr(firstQuote + 1, L'\"'); // Deuxième guillemet de path"
            if (!secondQuote) continue;

            // ✅ APRÈS "path", chercher le PROCHAIN guillemet ouvrant (après espaces/tabs)
            wchar_t* searchPos = secondQuote + 1;
            while (*searchPos && (*searchPos == L' ' || *searchPos == L'\t')) searchPos++;

            wchar_t* thirdQuote = wcschr(searchPos, L'\"'); // Premier guillemet du chemin
            if (!thirdQuote) continue;

            wchar_t* fourthQuote = wcschr(thirdQuote + 1, L'\"'); // Deuxième guillemet du chemin
            if (!fourthQuote) continue;

            // ✅ Extraire le chemin entre thirdQuote et fourthQuote
            wchar_t* pathStart = thirdQuote + 1;
            size_t pathLen = fourthQuote - pathStart;

            if (pathLen == 0 || pathLen >= MAX_PATH) continue;

            wcsncpy_s(currentLibraryPath, MAX_PATH, pathStart, pathLen);
            currentLibraryPath[pathLen] = L'\0';

            // ✅ TRIM : Supprimer espaces/tabs au DÉBUT
            wchar_t* trimStart = currentLibraryPath;
            while (*trimStart == L' ' || *trimStart == L'\t') trimStart++;

            // ✅ TRIM : Supprimer espaces/tabs à la FIN
            wchar_t* trimEnd = currentLibraryPath + wcslen(currentLibraryPath) - 1;
            while (trimEnd > trimStart && (*trimEnd == L' ' || *trimEnd == L'\t')) {
                *trimEnd = L'\0';
                trimEnd--;
            }

            // Si après trim on a une chaîne vide, ignorer
            if (wcslen(trimStart) == 0) continue;

            // Copier le chemin trimmé vers le début du buffer
            if (trimStart != currentLibraryPath) {
                wcscpy_s(currentLibraryPath, MAX_PATH, trimStart);
            }

            // ✅ NETTOYER LES DOUBLES BACKSLASHES (\\\\  →  \\)
            wchar_t cleanPath[MAX_PATH] = L"";
            size_t j = 0;
            for (size_t i = 0; i < wcslen(currentLibraryPath) && j < MAX_PATH - 1; i++) {
                cleanPath[j++] = currentLibraryPath[i];
                if (currentLibraryPath[i] == L'\\' && currentLibraryPath[i + 1] == L'\\') {
                    i++; // Sauter le second backslash
                }
            }
            cleanPath[j] = L'\0';
            wcscpy_s(currentLibraryPath, MAX_PATH, cleanPath);

            if (enableLogConfig) {
                char pathUtf8[MAX_PATH];
                size_t converted = 0;
                wcstombs_s(&converted, pathUtf8, MAX_PATH, currentLibraryPath, _TRUNCATE);
                char logMsg[512];
                snprintf(logMsg, sizeof(logMsg), "DEBUG: Extracted library path: '%s'", pathUtf8);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        // ✅ CHERCHER "440900" (Conan Exiles) SI UN CHEMIN EST STOCKÉ
        if (wcslen(currentLibraryPath) > 0 && wcsncmp(p, L"\"440900\"", 8) == 0) {
            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg), "DEBUG: Found Conan Exiles (440900) at line %d", lineNumber);
                mumbleAPI.log(ownID, logMsg);
            }

            // ✅ CONSTRUIRE LE CHEMIN COMPLET
            swprintf(outConanPath, pathSize, L"%s\\steamapps\\common\\Conan Exiles\\ConanSandbox\\Saved",
                currentLibraryPath);

            if (enableLogConfig) {
                char pathUtf8[MAX_PATH];
                size_t converted = 0;
                wcstombs_s(&converted, pathUtf8, MAX_PATH, outConanPath, _TRUNCATE);
                char logMsg[512];
                snprintf(logMsg, sizeof(logMsg), "DEBUG: Testing path: %s", pathUtf8);
                mumbleAPI.log(ownID, logMsg);
            }

            // ✅ VÉRIFIER QUE LE DOSSIER EXISTE
            DWORD attribs = GetFileAttributesW(outConanPath);
            if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY)) {
                foundConanExiles = TRUE;
                if (enableLogConfig) {
                    mumbleAPI.log(ownID, "DEBUG: Path VERIFIED - folder exists!");
                }
            }
            else {
                if (enableLogConfig) {
                    char logMsg[256];
                    snprintf(logMsg, sizeof(logMsg), "DEBUG: Path INVALID - GetFileAttributesW returned: %lu", attribs);
                    mumbleAPI.log(ownID, logMsg);
                }
            }
        }
    }

    fclose(file);

    if (foundConanExiles) {
        if (enableLogConfig) {
            char successMsg[512];
            size_t converted = 0;
            char pathUtf8[MAX_PATH];
            wcstombs_s(&converted, pathUtf8, MAX_PATH, outConanPath, _TRUNCATE);
            snprintf(successMsg, sizeof(successMsg),
                "SUCCESS: Conan Exiles found at: %s", pathUtf8);
            mumbleAPI.log(ownID, successMsg);
        }
        return TRUE;
    }
    else {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "DEBUG: Conan Exiles NOT FOUND in any library");
        }
        return FALSE;
    }
}

// Read Steam ID from Windows Registry | Lire le Steam ID depuis le registre Windows
BOOL readSteamIDFromRegistry(uint64_t* outSteamID) {
    if (!outSteamID) return FALSE;

    HKEY hKey = NULL;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Valve\\Steam\\ActiveProcess",
        0, KEY_READ, &hKey);

    if (result != ERROR_SUCCESS) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Registry: Steam ActiveProcess key not found - Steam may not be running");
        }
        return FALSE;
    }

    DWORD activeUser = 0;
    DWORD dataSize = sizeof(DWORD);
    result = RegQueryValueExW(hKey, L"ActiveUser", NULL, NULL,
        (LPBYTE)&activeUser, &dataSize);

    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS || activeUser == 0) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "Registry: ActiveUser value not found or invalid");
        }
        return FALSE;
    }

    // Convert AccountID (32-bit) to SteamID64 | Convertir AccountID (32-bit) en SteamID64
    *outSteamID = 76561197960265728ULL + (uint64_t)activeUser;

    if (enableLogGeneral) {
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg),
            "Registry: Steam ID retrieved successfully - AccountID: %lu, SteamID64: %llu",
            activeUser, *outSteamID);
        mumbleAPI.log(ownID, logMsg);
    }

    return TRUE;
}
