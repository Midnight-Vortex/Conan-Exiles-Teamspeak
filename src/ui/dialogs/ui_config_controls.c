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
#include "ui/config/ui_config_state.h"
#include "ui/config/ui_config_logic.h"

/*
 * ui_config_controls.c: WM_CREATE control layout + HUD combo helpers.
 * ui_config_controls.c : creation des controles (WM_CREATE) + aides combo HUD.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
 */

// Fills the dropdown with labels matching VOICE_HUD_THEME_* indices in plugin.h.
// Remplit la liste déroulante selon les indices VOICE_HUD_THEME_* de plugin.h.
static void ui_populate_hud_theme_combo(HWND combo) {
    if (!combo) {
        return;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Gray (Default)");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Purple");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Blue");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Green");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Amber");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Red");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Cyan");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Pink");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Orange");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Teal");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"White");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Lime");
}

static void ui_populate_hud_position_combo(HWND combo) {
    if (!combo) {
        return;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    /* Order must match VOICE_HUD_POSITION_* indices in plugin.h (0..4).
       Reihenfolge muss den VOICE_HUD_POSITION_*-Indizes in plugin.h entsprechen (0..4). */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Top Left");      /* 0 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Top Right");     /* 1 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Bottom Left");   /* 2 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Top Center");    /* 3 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Bottom Right");  /* 4 */
}

// Selects the combo item that matches the loaded ui_cfg()->hudTheme value.
// Sélectionne l'entrée correspondant à la valeur ui_cfg()->hudTheme chargée.
void ui_sync_hud_theme_combo(void) {
    if (!hHudThemeCombo || !IsWindow(hHudThemeCombo)) {
        return;
    }
    if (ui_cfg()->hudTheme < 0 || ui_cfg()->hudTheme >= VOICE_HUD_THEME_COUNT) {
        ui_cfg()->hudTheme = VOICE_HUD_THEME_GRAY;
    }
    SendMessage(hHudThemeCombo, CB_SETCURSEL, (WPARAM)ui_cfg()->hudTheme, 0);
}

// Reads the user's combo selection into ui_cfg()->hudTheme (not persisted until Save).
// Lit la sélection utilisateur dans ui_cfg()->hudTheme (non sauvegardé avant Enregistrer).
void ui_read_hud_theme_from_combo(void) {
    if (!hHudThemeCombo || !IsWindow(hHudThemeCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudThemeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_THEME_COUNT) {
        ui_cfg()->hudTheme = sel;
    }
}

static void ui_sync_hud_position_combo(void) {
    if (!hHudPositionCombo || !IsWindow(hHudPositionCombo)) {
        return;
    }
    if (ui_cfg()->hudPosition < 0 || ui_cfg()->hudPosition >= VOICE_HUD_POSITION_COUNT) {
        ui_cfg()->hudPosition = VOICE_HUD_POSITION_BOTTOM_RIGHT;
    }
    SendMessage(hHudPositionCombo, CB_SETCURSEL, (WPARAM)ui_cfg()->hudPosition, 0);
}

void ui_read_hud_position_from_combo(void) {
    if (!hHudPositionCombo || !IsWindow(hHudPositionCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudPositionCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_POSITION_COUNT) {
        ui_cfg()->hudPosition = sel;
    }
}

static void ui_populate_hud_size_combo(HWND combo) {
    if (!combo) {
        return;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    /* Order must match VOICE_HUD_SIZE_* indices in plugin.h (0..2). */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Small");   /* 0 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Medium");  /* 1 */
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Big");     /* 2 */
}

static void ui_sync_hud_size_combo(void) {
    if (!hHudSizeCombo || !IsWindow(hHudSizeCombo)) {
        return;
    }
    if (ui_cfg()->hudSize < 0 || ui_cfg()->hudSize >= VOICE_HUD_SIZE_COUNT) {
        ui_cfg()->hudSize = VOICE_HUD_SIZE_BIG;
    }
    SendMessage(hHudSizeCombo, CB_SETCURSEL, (WPARAM)ui_cfg()->hudSize, 0);
}

void ui_read_hud_size_from_combo(void) {
    if (!hHudSizeCombo || !IsWindow(hHudSizeCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudSizeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_SIZE_COUNT) {
        ui_cfg()->hudSize = sel;
    }
}

// WM_CREATE body: create every control and load initial values.
// Corps de WM_CREATE : creer tous les controles et charger les valeurs.
LRESULT ui_config_on_create(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    HWND control;
    (void)wParam;
    (void)lParam;
        g_configDialogDestroying = FALSE;
        // A key capture that was still running when the previous dialog closed must
        // not survive into this one (it would bind the first key that is held down,
        // typically F10 that opened the dialog).
        // Une capture de touche restee active a la fermeture ne doit pas survivre.
        isCapturingKey = FALSE;
        captureKeyTarget = 0;
        // Shared brush for HUD theme combo styling (WM_CTLCOLOR* handlers).
        // Pinceau partagé pour le style du combo thème HUD (gestionnaires WM_CTLCOLOR*).
        if (!g_hHudComboBrush) {
            g_hHudComboBrush = CreateSolidBrush(RGB(45, 45, 50));
        }
        hConfigDialog = hwnd;

        // ✅ 1) CRÉER LES POLICES UNE SEULE FOIS
        hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        hFontBold = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        hFontLarge = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        hFontEmoji = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");

        // The three category backgrounds are freed by ui_image_cache_release() once
        // the dialog window is destroyed (they used to leak on every open).
        // Les trois fonds de categorie sont liberes par ui_image_cache_release().
        if (!hBackgroundBitmap) {
            hBackgroundBitmap = LoadBackgroundFromResource(IDB_BACKGROUND);
        }

        if (!hBackgroundAdvancedBitmap) {
            hBackgroundAdvancedBitmap = LoadBackgroundFromResource(IDB_Background_Plugin_Settings);
        }
        if (!hBackgroundAdvancedBitmap && enableLogGeneral) {
            mumbleAPI.log(ownID, "WARNING: Advanced background image (IDB_Background_Plugin_Settings) not loaded");
        }

        if (!hBackgroundPresetsBitmap) {
            hBackgroundPresetsBitmap = LoadBackgroundFromResource(IDB_Background_Voice_Presets);
        }

        // Disable double buffering to reduce flickering | Désactiver le double buffering pour réduire les scintillements
        SetWindowLongPtr(hwnd, GWL_EXSTYLE,
            GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_COMPOSITED);

        // Get real dimensions of IDB_Main_Button_01 image (cached bitmap — not deleted here)
        // Récupérer les dimensions réelles de l'image IDB_Main_Button_01 (bitmap en cache)
        HBITMAP hTempBitmap = ui_image_cache_get(IDB_Main_Button_01);

        int imageWidth = 170;  // Valeur par défaut au cas où l'image ne charge pas
        int imageHeight = 40;  // Valeur par défaut

        if (hTempBitmap) {
            BITMAP bm;
            GetObject(hTempBitmap, sizeof(BITMAP), &bm);
            imageWidth = bm.bmWidth;   // ✅ Largeur RÉELLE de l'image
            imageHeight = bm.bmHeight; // ✅ Hauteur RÉELLE de l'image

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_Main_Button_01 size: %dx%d pixels", imageWidth, imageHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        {
            // Get actual window width | Récupérer la largeur réelle de la fenêtre
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int windowWidth = clientRect.right - clientRect.left;

            const int numButtons = 3;
            const float edgeToGapRatio = 2.0f; // Ratio between edge gap and internal gap | Ratio entre l'écart des bords et l'écart interne

            int totalButtonWidth = imageWidth * numButtons;
            int availableSpace = windowWidth - totalButtonWidth;

            // Check if there is enough space | Vérifier s'il y a assez d'espace
            if (availableSpace < 0) {
                if (enableLogGeneral) {
                    char errorMsg[128];
                    snprintf(errorMsg, sizeof(errorMsg),
                        "ERROR: Not enough space for buttons (window: %d, buttons: %d)",
                        windowWidth, totalButtonWidth);
                    mumbleAPI.log(ownID, errorMsg);
                }
                availableSpace = 20; // Valeur de secours
            }

            float edgeGap = (float)availableSpace * edgeToGapRatio / (2.0f * edgeToGapRatio + 2.0f);
            float internalGap = edgeGap / edgeToGapRatio;

            // Positions calculées dynamiquement
            int patchX = (int)edgeGap;
            int advX = patchX + imageWidth + (int)internalGap;
            int presetsX = advX + imageWidth + (int)internalGap;

            hCategoryPatch = CreateWindowW(L"BUTTON", L"Patch Configuration",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
                patchX - 2, 68, imageWidth, imageHeight,
                hwnd, (HMENU)301, NULL, NULL);

            hCategoryAdvanced = CreateWindowW(L"BUTTON", L"Advanced Options",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
                advX, 68, imageWidth, imageHeight, hwnd, (HMENU)302, NULL, NULL);
            ApplyFontToControl(hCategoryAdvanced, hFontBold);

            hCategoryPresets = CreateWindowW(L"BUTTON", L"Voice Presets",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
                presetsX + 2, 68, imageWidth, imageHeight, hwnd, (HMENU)303, NULL, NULL);
            ApplyFontToControl(hCategoryPresets, hFontBold);
        }

        // Create background image first | Créer d'abord l'image de fond
        hSavedPathBg = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            40, 270, 380, 35, hwnd, (HMENU)1100, NULL, NULL);

        wchar_t gamePathFromConfig[MAX_PATH] = L"";
        wchar_t savedPathFromConfig[MAX_PATH] = L"";
        wchar_t automaticPathFromConfig[MAX_PATH] = L"";

        // Path label: read the LOADED configuration instead of re-parsing plugin.cfg
        // here. config.c decodes the file correctly (UTF-8 or ANSI, review fix 085);
        // a second ANSI reader in the dialog garbled non-Latin-1 paths (Cyrillic, CJK).
        // Libelle du chemin : lire la configuration chargee (config.c decode le fichier).
        {
            PluginConfig loaded;
            config_copy(&loaded);
            ui_wcs_copy(automaticPathFromConfig, MAX_PATH, loaded.automaticSavedPath); // truncating
            ui_wcs_copy(savedPathFromConfig, MAX_PATH, loaded.savedPath);              // truncating
            const BOOL automaticPatchFindEnabled = loaded.automaticPatchFind ? TRUE : FALSE;

            if (automaticPatchFindEnabled && wcslen(automaticPathFromConfig) > 0) {
                // Mode automatique : afficher le chemin automatique
                ui_wcs_copy(displayedPathText, MAX_PATH, automaticPathFromConfig);
                wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                if (conanSandbox) {
                    *conanSandbox = L'\0';
                }
            }
            else if (wcslen(savedPathFromConfig) > 0) {
                // Mode manuel : afficher le chemin manuel
                ui_wcs_copy(displayedPathText, MAX_PATH, savedPathFromConfig);
                wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                if (conanSandbox) {
                    *conanSandbox = L'\0';
                }
            }
            else {
                // Aucun chemin configuré
                wcscpy_s(displayedPathText, MAX_PATH, L"(Not configured)");
            }
        }

        // Automatic mode: the path box shows the detected Steam folder. It is resolved
        // ONCE here (the owner-draw handler used to query the registry + parse
        // libraryfolders.vdf on every repaint and now just prints displayedPathText).
        // Mode automatique : dossier detecte, resolu UNE fois ici (plus a chaque dessin).
        if (ui_cfg()->automaticPatchFind) {
            wchar_t autoPathFull[MAX_PATH] = L"";
            if (findConanExilesAutomatic(autoPathFull, MAX_PATH)) {
                ui_wcs_copy(displayedPathText, MAX_PATH, autoPathFull);
                wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                if (conanSandbox) {
                    *conanSandbox = L'\0';
                }
            }
            else {
                wcscpy_s(displayedPathText, MAX_PATH, L"(Not found)");
            }
        }

        // hPathBoxBitmap aliases the cached bitmap (freed by ui_image_cache_release()).
        // hPathBoxBitmap = bitmap du cache (libere par ui_image_cache_release()).
        hPathBoxBitmap = ui_image_cache_get(IDB_Path_Box);

        int pathBoxWidth = 380;
        int pathBoxHeight = 35;
        if (hPathBoxBitmap) {
            BITMAP bmPath;
            GetObject(hPathBoxBitmap, sizeof(BITMAP), &bmPath);
            pathBoxWidth = bmPath.bmWidth;
            pathBoxHeight = bmPath.bmHeight;
            SendMessage(hSavedPathBg, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hPathBoxBitmap);
        }

        HBITMAP hBrowseTemp = ui_image_cache_get(IDB_Browse_Button); // cached — not deleted here

        int browseBtnWidth = 100;
        int browseBtnHeight = 35;
        if (hBrowseTemp) {
            BITMAP bmBrowse;
            GetObject(hBrowseTemp, sizeof(BITMAP), &bmBrowse);
            browseBtnWidth = bmBrowse.bmWidth;
            browseBtnHeight = bmBrowse.bmHeight;
        }

        int pathX = 40;
        int pathY = 270;
        int browseGap = 10;
        int browseX = pathX + pathBoxWidth + browseGap;
        int browseY = pathY + ((pathBoxHeight - browseBtnHeight) / 2);

        // Checkmark icon from the image cache (destroyed by ui_image_cache_release())
        // Icône checkmark issue du cache (détruite par ui_image_cache_release())
        HICON hCheckIcon = ui_checkmark_icon_get();

        // Automatic patch find checkbox | Checkbox pour la recherche automatique de patch
        hAutomaticPatchFindCheck = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | BS_AUTOCHECKBOX,  // ✅ ENLEVER WS_VISIBLE
            60, 220, 20, 20,
            hwnd, (HMENU)200, GetModuleHandle(NULL), NULL);

        // Set checkmark icon if loaded | Définir l'icône checkmark si chargée
        if (hCheckIcon) {
            SendMessage(hAutomaticPatchFindCheck, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hCheckIcon);
        }

        HWND hAutomaticPatchFindLabel = CreateWindowW(L"STATIC", L"Automatic Patch Find",
            WS_CHILD | SS_LEFT | SS_NOTIFY,  // ✅ ENLEVER WS_VISIBLE (no WS_DISABLED: a disabled static never gets the click)
            80, 222, 400, 20,
            hwnd, (HMENU)IDC_LABEL_AUTOPATCH, NULL, NULL);
        ApplyFontToControl(hAutomaticPatchFindLabel, hFont);

        SetWindowSubclass(hAutomaticPatchFindLabel, CheckboxLabelProc, 200, (DWORD_PTR)hAutomaticPatchFindCheck);

        // Set checkbox state from config | Définir l'état de la checkbox depuis la config
        // Default to TRUE if not yet configured | Par défaut TRUE si non configuré
        CheckDlgButton(hwnd, 200, ui_cfg()->automaticPatchFind ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(hAutomaticPatchFindCheck, SW_SHOW);

        hSavedPathButton = CreateWindowW(L"BUTTON", L"Browse",
            WS_CHILD | BS_OWNERDRAW,
            browseX, browseY, browseBtnWidth, browseBtnHeight, hwnd, (HMENU)105, NULL, NULL);
        ApplyFontToControl(hSavedPathButton, hFontBold);

        // ========== CATÉGORIE 2 : ADVANCED OPTIONS ==========

        // === CHECKBOX 1 : Distance-based muting ===
        hEnableDistanceMutingCheck = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | BS_AUTOCHECKBOX,
            60, 145, 20, 20, hwnd, (HMENU)201, NULL, NULL);

        if (hCheckIcon) {
            SendMessage(hEnableDistanceMutingCheck, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hCheckIcon);
        }

        HWND hDistanceMutingLabel = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT | SS_NOTIFY,
            85, 147, 295, 20, hwnd, (HMENU)IDC_LABEL_DISTMUTING, NULL, NULL);
        ApplyFontToControl(hDistanceMutingLabel, hFont);

        SetWindowSubclass(hDistanceMutingLabel, CheckboxLabelProc, 201, (DWORD_PTR)hEnableDistanceMutingCheck);


        // === CHECKBOX 2 : Automatic channel switching ===
        hEnableAutomaticChannelChangeCheck = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | BS_AUTOCHECKBOX,  // ✅ WS_CHILD uniquement
            60, 165, 20, 20, hwnd, (HMENU)203, NULL, NULL);

        if (hCheckIcon) {
            SendMessage(hEnableAutomaticChannelChangeCheck, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hCheckIcon);
        }

        HWND hChannelSwitchingLabel = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT | SS_NOTIFY,  // ✅ ENLEVER WS_VISIBLE
            85, 167, 375, 20, hwnd, (HMENU)IDC_LABEL_CHANSWITCH, NULL, NULL);
        ApplyFontToControl(hChannelSwitchingLabel, hFont);

        SetWindowSubclass(hChannelSwitchingLabel, CheckboxLabelProc, 203, (DWORD_PTR)hEnableAutomaticChannelChangeCheck);


        // === CHECKBOX 3 : Voice toggle ===
        hEnableVoiceToggleCheck = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | BS_AUTOCHECKBOX,  // ✅ WS_CHILD uniquement
            60, 185, 20, 20, hwnd, (HMENU)204, NULL, NULL);

        if (hCheckIcon) {
            SendMessage(hEnableVoiceToggleCheck, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hCheckIcon);
        }

        HWND hVoiceToggleLabel = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT | SS_NOTIFY,  // ✅ ENLEVER WS_VISIBLE
            85, 187, 375, 20, hwnd, (HMENU)IDC_LABEL_VOICETOGGLE, NULL, NULL);
        ApplyFontToControl(hVoiceToggleLabel, hFont);

        SetWindowSubclass(hVoiceToggleLabel, CheckboxLabelProc, 204, (DWORD_PTR)hEnableVoiceToggleCheck);

        // Get real dimensions of IDB_Key_Box_01 (cached bitmap — not deleted here)
        // Récupérer les dimensions réelles de IDB_Key_Box_01 (bitmap en cache)
        HBITMAP hKeyBoxTemp = ui_image_cache_get(IDB_Key_Box_01);

        int keyBoxWidth = 110;  // Valeur par défaut
        int keyBoxHeight = 30;  // Valeur par défaut

        if (hKeyBoxTemp) {
            BITMAP bmKey;
            GetObject(hKeyBoxTemp, sizeof(BITMAP), &bmKey);
            keyBoxWidth = bmKey.bmWidth;
            keyBoxHeight = bmKey.bmHeight;

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_Key_Box_01 size: %dx%d pixels", keyBoxWidth, keyBoxHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        // Create all key edit and button controls with Patch Configuration style | Créer tous les contrôles avec le style de Patch Configuration
        hWhisperKeyEdit = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            200, 303, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)2001, NULL, NULL);
        ApplyFontToControl(hWhisperKeyEdit, hFont);

        hWhisperButton = CreateWindowW(L"BUTTON", L"Set Key",
            WS_CHILD | BS_OWNERDRAW,
            200 + keyBoxWidth + 10, 303, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)101, NULL, NULL);
        ApplyFontToControl(hWhisperButton, hFontBold);

        hNormalKeyEdit = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            200, 338, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)2002, NULL, NULL);
        ApplyFontToControl(hNormalKeyEdit, hFont);

        hNormalButton = CreateWindowW(L"BUTTON", L"Set Key",
            WS_CHILD | BS_OWNERDRAW,
            200 + keyBoxWidth + 10, 338, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)102, NULL, NULL);
        ApplyFontToControl(hNormalButton, hFontBold);

        hShoutKeyEdit = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            200, 373, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)2003, NULL, NULL);
        ApplyFontToControl(hShoutKeyEdit, hFont);

        hShoutButton = CreateWindowW(L"BUTTON", L"Set Key",
            WS_CHILD | BS_OWNERDRAW,
            200 + keyBoxWidth + 10, 373, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)103, NULL, NULL);
        ApplyFontToControl(hShoutButton, hFontBold);

        hConfigKeyEdit = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            200, 408, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)2004, NULL, NULL);
        ApplyFontToControl(hConfigKeyEdit, hFont);

        hConfigButton = CreateWindowW(L"BUTTON", L"Set Key",
            WS_CHILD | BS_OWNERDRAW,
            200 + keyBoxWidth + 10, 408, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)104, NULL, NULL);
        ApplyFontToControl(hConfigButton, hFontBold);

        hVoiceToggleKeyEdit = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_OWNERDRAW,
            200, 485, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)2005, NULL, NULL);
        ApplyFontToControl(hVoiceToggleKeyEdit, hFont);

        hVoiceToggleButton = CreateWindowW(L"BUTTON", L"Set Key",
            WS_CHILD | BS_OWNERDRAW,
            200 + keyBoxWidth + 10, 485, keyBoxWidth, keyBoxHeight, hwnd, (HMENU)106, NULL, NULL);
        ApplyFontToControl(hVoiceToggleButton, hFontBold);

        // HUD theme picker: right of keyboard shortcuts (Set Key column ends ~x430).
        // Sélecteur thème HUD : à droite des raccourcis clavier (colonne Set Key finit ~x430).
        // Label above dropdown; only visible on Advanced Options tab (category 2).
        // Libellé au-dessus du menu ; visible uniquement dans Options avancées (catégorie 2).
        hHudThemeLabel = CreateWindowW(L"STATIC", L"HUD Theme:",
            WS_CHILD | SS_LEFT,
            418, 276, 140, 20, hwnd, (HMENU)526, NULL, NULL);
        ApplyFontToControl(hHudThemeLabel, hFontBold);

        hHudThemeCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            418, 300, 140, 200, hwnd, (HMENU)215, NULL, NULL);
        ApplyFontToControl(hHudThemeCombo, hFont);
        // Disable visual styles so WM_CTLCOLOR* can paint dark background + white text.
        // Désactive les styles visuels pour fond sombre + texte blanc via WM_CTLCOLOR*.
        SetWindowTheme(hHudThemeCombo, L"", L"");
        ui_populate_hud_theme_combo(hHudThemeCombo);
        ui_sync_hud_theme_combo();

        hHudPositionLabel = CreateWindowW(L"STATIC", L"HUD Position:",
            WS_CHILD | SS_LEFT,
            418, 330, 140, 20, hwnd, (HMENU)527, NULL, NULL);
        ApplyFontToControl(hHudPositionLabel, hFontBold);

        hHudPositionCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            418, 354, 140, 200, hwnd, (HMENU)216, NULL, NULL);
        ApplyFontToControl(hHudPositionCombo, hFont);
        SetWindowTheme(hHudPositionCombo, L"", L"");
        ui_populate_hud_position_combo(hHudPositionCombo);
        ui_sync_hud_position_combo();

        hHudSizeLabel = CreateWindowW(L"STATIC", L"HUD Size:",
            WS_CHILD | SS_LEFT,
            418, 384, 140, 20, hwnd, (HMENU)528, NULL, NULL);
        ApplyFontToControl(hHudSizeLabel, hFontBold);

        hHudSizeCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            418, 408, 140, 200, hwnd, (HMENU)217, NULL, NULL);
        ApplyFontToControl(hHudSizeCombo, hFont);
        SetWindowTheme(hHudSizeCombo, L"", L"");
        ui_populate_hud_size_combo(hHudSizeCombo);
        ui_sync_hud_size_combo();

        hDistanceWhisperEdit = CreateWindowW(L"EDIT", L"2.0",
            WS_CHILD | WS_BORDER | ES_CENTER,
            155, 550, 60, 28, hwnd, NULL, NULL, NULL);
        ApplyFontToControl(hDistanceWhisperEdit, hFont);

        hDistanceNormalEdit = CreateWindowW(L"EDIT", L"15.0",
            WS_CHILD | WS_BORDER | ES_CENTER,
            315, 550, 60, 28, hwnd, NULL, NULL, NULL);
        ApplyFontToControl(hDistanceNormalEdit, hFont);

        hDistanceShoutEdit = CreateWindowW(L"EDIT", L"50.0",
            WS_CHILD | WS_BORDER | ES_CENTER,
            465, 550, 60, 28, hwnd, NULL, NULL, NULL);
        ApplyFontToControl(hDistanceShoutEdit, hFont);

        hDistanceWhisperMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 585, 480, 20, hwnd, (HMENU)520, NULL, NULL);
        ApplyFontToControl(hDistanceWhisperMessage, hFont);

        hDistanceNormalMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 600, 480, 20, hwnd, (HMENU)521, NULL, NULL);
        ApplyFontToControl(hDistanceNormalMessage, hFont);

        hDistanceShoutMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 615, 480, 20, hwnd, (HMENU)522, NULL, NULL);
        ApplyFontToControl(hDistanceShoutMessage, hFont);

        hDistanceMutingMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 210, 460, 18, hwnd, (HMENU)523, NULL, NULL);
        ApplyFontToControl(hDistanceMutingMessage, hFont);

        hChannelSwitchingMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 230, 460, 18, hwnd, (HMENU)524, NULL, NULL);
        ApplyFontToControl(hChannelSwitchingMessage, hFont);

        hPositionalAudioMessage = CreateWindowW(L"STATIC", L"",
            WS_CHILD | SS_LEFT,
            60, 250, 460, 18, hwnd, (HMENU)525, NULL, NULL);
        ApplyFontToControl(hPositionalAudioMessage, hFont);

        const wchar_t* saveConfigText = L"Save Configuration";
        const wchar_t* saveVoiceRangeText = L"Save Voice Range";
        const wchar_t* cancelText = L"Cancel";

        int bottomBtnWidth = imageWidth;
        int bottomBtnHeight = imageHeight;

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        int clientWidth = clientRect.right - clientRect.left;

        const int gap = 9;
        const int btnY = 654;

        // Centrer les trois boutons en utilisant la largeur réelle de l'image
        int totalWidth = bottomBtnWidth * 3 + gap * 2;
        int startX = (clientWidth - totalWidth) / 2;
        if (startX < 10) startX = 10;

        int saveVoiceRangeX = startX;
        int saveConfigX = startX + bottomBtnWidth + gap;
        int cancelX = startX + (bottomBtnWidth + gap) * 2;

        // Créer les boutons avec BS_OWNERDRAW — DrawButtonWithBitmap dessinera l'image à taille réelle
        control = CreateWindowW(L"BUTTON", saveVoiceRangeText,
            WS_CHILD | BS_OWNERDRAW,  // Caché par défaut
            saveVoiceRangeX, btnY, bottomBtnWidth, bottomBtnHeight, hwnd, (HMENU)11, NULL, NULL);
        ApplyFontToControl(control, hFont);

        control = CreateWindowW(L"BUTTON", saveConfigText,
            WS_CHILD | BS_OWNERDRAW,
            saveConfigX, btnY, bottomBtnWidth, bottomBtnHeight, hwnd, (HMENU)1, NULL, NULL);
        ApplyFontToControl(control, hFont);

        control = CreateWindowW(L"BUTTON", cancelText,
            WS_CHILD | BS_OWNERDRAW,
            cancelX, btnY, bottomBtnWidth, bottomBtnHeight, hwnd, (HMENU)2, NULL, NULL);
        ApplyFontToControl(control, hFont);

        // Status message (toujours visible)
        hStatusMessage = CreateWindowW(L"STATIC", L"",
            WS_VISIBLE | WS_CHILD | SS_CENTER,
            40, 740, 520, 25, hwnd, (HMENU)600, NULL, NULL);
        ApplyFontToControl(hStatusMessage, hFont);

        // ✅ 8) CHARGER LES VALEURS (une seule fois) — ui_cfg() working copy (V8.12)

        ShowCategoryControls(1);

        // Set actual values | Définir les valeurs réelles
        if (wcslen(gamePathFromConfig) > 0) {
            SetWindowTextW(hSavedPathEdit, gamePathFromConfig);
        }
        else {
            SetWindowTextW(hSavedPathEdit, L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Conan Exiles");
        }

        // Set checkbox labels | Définir les labels des checkboxes
        HWND hAutomaticPatchFindLabelText = GetDlgItem(hwnd, 2000);
        if (hAutomaticPatchFindLabelText) SetWindowTextW(hAutomaticPatchFindLabelText, L"Automatic Patch Find");

        SetWindowTextA(hWhisperKeyEdit, getKeyName(ui_cfg()->whisperKey));
        SetWindowTextA(hNormalKeyEdit, getKeyName(ui_cfg()->normalKey));
        SetWindowTextA(hShoutKeyEdit, getKeyName(ui_cfg()->shoutKey));
        SetWindowTextA(hConfigKeyEdit, getKeyName(ui_cfg()->configUIKey));
        SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(ui_cfg()->voiceToggleKey));
        CheckDlgButton(hwnd, 201, ui_cfg()->enableDistanceMuting ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 203, ui_cfg()->enableAutomaticChannelChange ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 204, ui_cfg()->enableVoiceToggle ? BST_CHECKED : BST_UNCHECKED);

        // CORRECTION: Utiliser les valeurs CHARGÉES depuis le fichier de configuration
        // au lieu des valeurs par défaut
        wchar_t whisperText[32], normalText[32], shoutText[32];
        swprintf(whisperText, 32, L"%.1f", ui_cfg()->distanceWhisper);
        swprintf(normalText, 32, L"%.1f", ui_cfg()->distanceNormal);
        swprintf(shoutText, 32, L"%.1f", ui_cfg()->distanceShout);

        SetWindowTextW(hDistanceWhisperEdit, whisperText);
        SetWindowTextW(hDistanceNormalEdit, normalText);
        SetWindowTextW(hDistanceShoutEdit, shoutText);

        if (enableLogConfig) {
            char debugMsg[256];
            snprintf(debugMsg, sizeof(debugMsg),
                "WM_CREATE: Distances set in fields - Whisper: %.1f, Normal: %.1f, Shout: %.1f",
                ui_cfg()->distanceWhisper, ui_cfg()->distanceNormal, ui_cfg()->distanceShout);
            mumbleAPI.log(ownID, debugMsg);
        }

        // Create preset category controls | Créer les contrôles de la catégorie presets
        createPresetsCategory();

        // Load presets from config | Charger les presets depuis la configuration
        loadPresetsFromConfigFile();

        ShowCategoryControls(1);

    return 0;
}
