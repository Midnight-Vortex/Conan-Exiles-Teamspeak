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
 * ui_config_draw.c: WM_DRAWITEM/WM_ERASEBKGND/WM_CTLCOLOR* + bitmap helpers.
 * ui_config_draw.c : dessin des controles + aides bitmap.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
 */

// Returns the dark brush for the HUD theme combo, or a stock brush while tearing down.
// Retourne le pinceau sombre du combo thème HUD, ou un pinceau système pendant la fermeture.
static HBRUSH ui_hud_combo_brush(void) {
    if (g_configDialogDestroying || !g_hHudComboBrush) {
        return (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    }
    return g_hHudComboBrush;
}

static int ui_is_hud_styled_combo_child(HWND hwnd) {
    if (!hwnd) {
        return 0;
    }
    HWND parent = GetParent(hwnd);
    return (parent == hHudThemeCombo || parent == hHudPositionCombo || parent == hHudSizeCombo);
}

int calculateButtonWidth(const wchar_t* text, HFONT font) {
    HDC hdc = GetDC(NULL);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);

    SIZE textSize;
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &textSize);

    SelectObject(hdc, oldFont);
    ReleaseDC(NULL, hdc);

    // Largeur du texte + 10 pixels de marge totale (5px de chaque côté) + padding Windows
    return textSize.cx + 10 + 20; // 20px = padding interne du bouton Windows
}

// Load background bitmap from resources | Charger l'image de fond depuis les ressources
HBITMAP LoadBackgroundFromResource(int resourceID) {
    HMODULE hModule = NULL;
    // Obtenir le HMODULE de la DLL à partir d'une adresse interne (fonction dans cette DLL)
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)LoadBackgroundFromResource, &hModule)) {
        DWORD err = GetLastError();
        if (enableLogGeneral) {
            char errorMsg[128];
            snprintf(errorMsg, sizeof(errorMsg),
                "ERROR: GetModuleHandleExW failed (Error: %lu)", err);
            mumbleAPI.log(ownID, errorMsg);
        }
        return NULL;
    }

    // Charger le bitmap depuis les ressources de la DLL (Unicode)
    HBITMAP hBitmap = (HBITMAP)LoadImageW(
        hModule,
        MAKEINTRESOURCEW(resourceID),
        IMAGE_BITMAP,
        0, 0,
        LR_CREATEDIBSECTION
    );

    if (!hBitmap) {
        DWORD error = GetLastError();
        if (enableLogGeneral) {
            char errorMsg[192];
            snprintf(errorMsg, sizeof(errorMsg),
                "ERROR: Failed to load background bitmap (ID=%d) from DLL resources (Error: %lu)",
                resourceID, error);
            mumbleAPI.log(ownID, errorMsg);
        }
        return NULL;
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Background bitmap loaded successfully from DLL resources");
    }

    return hBitmap;
}

void DrawButtonWithBitmap(LPDRAWITEMSTRUCT lpDIS) {
    HDC hdc = lpDIS->hDC;
    RECT rect = lpDIS->rcItem;

    int ctrlId = lpDIS->CtlID;
    int bitmapResource = IDB_Main_Button_01; // Initialize with default value | Initialiser avec valeur par défaut

    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)DrawButtonWithBitmap, &hModule);

    // Use main image for category and bottom buttons | Utiliser l'image principale pour les boutons de catégorie et du bas
    if (ctrlId == 301 || ctrlId == 302 || ctrlId == 303 || ctrlId == 1 || ctrlId == 11 || ctrlId == 2) {
        bitmapResource = IDB_Main_Button_01;
    }
    // Use preset box image for preset save buttons (IDs 700-709) | Utiliser l'image de preset pour les boutons de sauvegarde de preset (IDs 700-709)
    else if (ctrlId >= 700 && ctrlId < 700 + MAX_VOICE_PRESETS) {
        bitmapResource = IDB_Preset_Box_01;
    }
    // Use dedicated image for Browse button | Utiliser l'image dédiée pour le bouton Browse (ID 105)
    else if (ctrlId == 105) {
        bitmapResource = IDB_Browse_Button;
    }

    // Charger le bitmap (chaque appel charge une instance locale afin d'éviter conflits de HBITMAP partagés)
    HBITMAP hButtonBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(bitmapResource),
        IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

    if (hButtonBitmap) {
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hButtonBitmap);

        BITMAP bm;
        GetObject(hButtonBitmap, sizeof(BITMAP), &bm);

        int btnWidth = rect.right - rect.left;
        int btnHeight = rect.bottom - rect.top;

        // Draw main/button images and preset box images at real size | Dessiner les images principales et les boîtes de preset à taille réelle
        if (ctrlId == 301 || ctrlId == 302 || ctrlId == 303 || ctrlId == 1 || ctrlId == 11 || ctrlId == 2 ||
            (ctrlId >= 700 && ctrlId < 700 + MAX_VOICE_PRESETS)) {
            // Draw bitmap at natural size (no stretch) | Dessiner le bitmap à taille naturelle (sans étirement)
            BitBlt(hdc,
                rect.left,
                rect.top,
                bm.bmWidth,
                bm.bmHeight,
                hdcMem,
                0, 0,
                SRCCOPY);

            // Centrer le texte sur l'image (texte récupéré depuis le bouton si disponible)
            wchar_t overlayText[128] = L"";
            GetWindowTextW(lpDIS->hwndItem, overlayText, (int)(sizeof(overlayText) / sizeof(wchar_t)));
            if (wcslen(overlayText) == 0) {
                // Valeurs par défaut si aucun texte (fallback lisible)
                if (ctrlId == 301) wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Patch Configuration");
                else if (ctrlId == 302) wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Advanced Options");
                else if (ctrlId == 303) wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Voice Presets");
                else if (ctrlId == 1)   wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Save Configuration");
                else if (ctrlId == 11)  wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Save Voice Range");
                else if (ctrlId == 2)   wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Cancel");
            }

            HFONT hTextFont = NULL;
            HFONT hOldFont = NULL;
            if (hFontBold) {
                hTextFont = hFontBold;
                hOldFont = (HFONT)SelectObject(hdc, hTextFont);
            }
            else {
                hTextFont = CreateFontW(
                    16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                hOldFont = (HFONT)SelectObject(hdc, hTextFont);
            }

            RECT textRect;
            textRect.left = rect.left;
            textRect.top = rect.top;
            textRect.right = rect.left + bm.bmWidth;
            textRect.bottom = rect.top + bm.bmHeight;

            // Si le bouton réel est plus grand que le bitmap, étendre le rect de texte pour centrer proprement
            if ((textRect.right - textRect.left) < btnWidth) textRect.right = rect.right;
            if ((textRect.bottom - textRect.top) < btnHeight) textRect.bottom = rect.bottom;

            SetBkMode(hdc, TRANSPARENT);
            // Ombre
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT shadowRect = textRect;
            OffsetRect(&shadowRect, 1, 1);
            DrawTextW(hdc, overlayText, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Texte principal
            SetTextColor(hdc, RGB(240, 240, 240));
            DrawTextW(hdc, overlayText, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            if (!hFontBold && hTextFont) {
                DeleteObject(hTextFont);
            }
        }
        else if (ctrlId == 105) {
            // Bouton Browse : dessiner le bitmap à taille réelle, sans stretch (identique logique)
            BitBlt(hdc,
                rect.left,
                rect.top,
                bm.bmWidth,
                bm.bmHeight,
                hdcMem,
                0, 0,
                SRCCOPY);

            wchar_t overlayText[128] = L"";
            GetWindowTextW(lpDIS->hwndItem, overlayText, (int)(sizeof(overlayText) / sizeof(wchar_t)));
            if (wcslen(overlayText) == 0) wcscpy_s(overlayText, sizeof(overlayText) / sizeof(wchar_t), L"Browse");

            HFONT hTextFont = NULL;
            HFONT hOldFont = NULL;
            if (hFontBold) {
                hTextFont = hFontBold;
                hOldFont = (HFONT)SelectObject(hdc, hTextFont);
            }
            else if (hFont) {
                hTextFont = hFont;
                hOldFont = (HFONT)SelectObject(hdc, hTextFont);
            }
            else {
                hTextFont = CreateFontW(
                    16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                hOldFont = (HFONT)SelectObject(hdc, hTextFont);
            }

            RECT textRect;
            textRect.left = rect.left;
            textRect.top = rect.top;
            textRect.right = rect.left + bm.bmWidth;
            textRect.bottom = rect.top + bm.bmHeight;

            int btnWidthLocal = rect.right - rect.left;
            int btnHeightLocal = rect.bottom - rect.top;
            if ((textRect.right - textRect.left) < btnWidthLocal) textRect.right = rect.right;
            if ((textRect.bottom - textRect.top) < btnHeightLocal) textRect.bottom = rect.bottom;

            SetBkMode(hdc, TRANSPARENT);

            SetBkMode(hdc, TRANSPARENT);
            // Ombre
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT shadowRect = textRect;
            OffsetRect(&shadowRect, 1, 1);
            DrawTextW(hdc, overlayText, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Texte principal en blanc
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, overlayText, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            if (!hFontBold && hTextFont) {
                DeleteObject(hTextFont);
            }
        }

        SelectObject(hdcMem, hOldBitmap);
        DeleteDC(hdcMem);
        DeleteObject(hButtonBitmap);
    }
    else {
        // Fallback : fond gris si l'image ne charge pas
        HBRUSH hBrush = CreateSolidBrush(RGB(220, 220, 220));
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);
    }

    // Do not redraw default text for main, bottom and preset save buttons | Ne pas redessiner le texte par défaut pour les boutons principaux, du bas et de sauvegarde des presets
    if (ctrlId != 301 && ctrlId != 302 && ctrlId != 303 && ctrlId != 105 && ctrlId != 1 && ctrlId != 11 && ctrlId != 2
        && !(ctrlId >= 700 && ctrlId < 700 + MAX_VOICE_PRESETS)) {

        wchar_t text[256];
        GetWindowTextW(lpDIS->hwndItem, text, 256);
        SetBkMode(hdc, TRANSPARENT);

        if (lpDIS->itemState & ODS_SELECTED) {
            rect.left += 2;
            rect.top += 2;
        }

        // Shadow text | Texte ombre
        SetTextColor(hdc, RGB(0, 0, 0));
        RECT shadowRect = rect;
        OffsetRect(&shadowRect, 1, 1);
        DrawTextW(hdc, text, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Main text in white | Texte principal en blanc
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

}

// Subclass procedure pour rendre les labels cliquables
LRESULT CALLBACK CheckboxLabelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        // Cliquer sur le label = cocher/décocher la checkbox associée
        HWND hCheckbox = (HWND)dwRefData;
        if (hCheckbox && IsWindow(hCheckbox)) {
            LRESULT checkState = SendMessage(hCheckbox, BM_GETCHECK, 0, 0);
            SendMessage(hCheckbox, BM_SETCHECK, (checkState == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);

            // Envoyer notification au parent (simule un clic sur la checkbox)
            HWND hParent = GetParent(hCheckbox);
            if (hParent) {
                SendMessage(hParent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hCheckbox), BN_CLICKED), (LPARAM)hCheckbox);
            }
        }
        return 0;
    }

    case WM_SETCURSOR:
        // Afficher le curseur "main" (pointeur) au survol
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, CheckboxLabelProc, uIdSubclass);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// WM_ERASEBKGND: paint the category background image (or solid fallback).
LRESULT ui_config_on_erasebkgnd(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hwnd, &rect);

        int winW = rect.right - rect.left;
        int winH = rect.bottom - rect.top;

        // CATÉGORIE 1 : Afficher BACKGROUND.bmp | Category 1: Display BACKGROUND.bmp
        if (currentCategory == 1 && hBackgroundBitmap) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBackgroundBitmap);

            BITMAP bm;
            GetObject(hBackgroundBitmap, sizeof(BITMAP), &bm);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                0, 0, winW, winH,
                hdcMem,
                0, 0, bm.bmWidth, bm.bmHeight,
                SRCCOPY);

            SelectObject(hdcMem, hOldBitmap);
            DeleteDC(hdcMem);
        }
        // CATÉGORIE 2 : Afficher BACKGROUND_Plugin_Settings | Category 2: Display BACKGROUND_Plugin_Settings
        else if (currentCategory == 2 && hBackgroundAdvancedBitmap) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBackgroundAdvancedBitmap);

            BITMAP bm;
            GetObject(hBackgroundAdvancedBitmap, sizeof(BITMAP), &bm);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                0, 0, winW, winH,
                hdcMem,
                0, 0, bm.bmWidth, bm.bmHeight,
                SRCCOPY);

            SelectObject(hdcMem, hOldBitmap);
            DeleteDC(hdcMem);
        }
        // CATÉGORIE 3 : Afficher Background_Voice_Presets | Category 3: Display Background_Voice_Presets
        else if (currentCategory == 3 && hBackgroundPresetsBitmap) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBackgroundPresetsBitmap);

            BITMAP bm;
            GetObject(hBackgroundPresetsBitmap, sizeof(BITMAP), &bm);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                0, 0, winW, winH,
                hdcMem,
                0, 0, bm.bmWidth, bm.bmHeight,
                SRCCOPY);

            SelectObject(hdcMem, hOldBitmap);
            DeleteDC(hdcMem);
        }
        else {
            // Fallback : Fond uni pour toutes les catégories sans image | Fallback: Solid background for all categories without image
            HBRUSH hBrush = CreateSolidBrush(RGB(248, 249, 250));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
        }

        return 1;
}

// WM_CTLCOLORSTATIC: colour labels/status/combo text.
LRESULT ui_config_on_ctlcolorstatic(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
        // During teardown, fall through to DefWindowProc — brush may already be freed.
        // Pendant la fermeture, laisser DefWindowProc gérer — le pinceau peut être libéré.
        if (g_configDialogDestroying) {
            return 0;
        }
        HDC hdcStatic = (HDC)wParam;
        HWND hwndStatic = (HWND)lParam;
        int controlId = GetDlgCtrlID(hwndStatic);

        SetBkMode(hdcStatic, TRANSPARENT);

        wchar_t messageText[300];
        GetWindowTextW(hwndStatic, messageText, 300);

        // ========== TEXTES D'INFORMATION (IDs 401, 402, 403) EN BLEU ==========
        if (controlId >= 401 && controlId <= 403) {
            SetTextColor(hdcStatic, RGB(0, 0, 0)); // Noir
        }
        // ========== MESSAGES DE VALIDATION DE RANGE (BLEU) ==========
        // IDs 520-522 = Messages whisper/normal/shout au bas de l'interface
        else if (controlId >= 520 && controlId <= 522) {
            // Si le message contient "Valid range" ou "Free range" → BLEU
            if (wcsstr(messageText, L"Valid range") || wcsstr(messageText, L"Free range")) {
                SetTextColor(hdcStatic, RGB(59, 130, 246)); // Bleu vif
            }
            // Si le message contient "Auto-corrected" → ROUGE
            else if (wcsstr(messageText, L"Auto-corrected")) {
                SetTextColor(hdcStatic, RGB(220, 38, 38)); // Rouge
            }
            else {
                SetTextColor(hdcStatic, RGB(107, 114, 128)); // Gris par défaut
            }
        }
        // ========== MESSAGES DE STATUT (IDs 523-525) ==========
        // Distance Muting, Channel Switching, Positional Audio
        else if (controlId >= 523 && controlId <= 525) {
            // ✅ CORRECTION : Vérifier "INFO" EN PREMIER (avant LOCKED/FORCED)
            if (wcsstr(messageText, L"INFO:")) {
                SetTextColor(hdcStatic, RGB(59, 130, 246)); // BLEU pour INFO
            }
            // Si le message contient "LOCKED" ou "FORCED" → ROUGE FONCÉ
            else if (wcsstr(messageText, L"LOCKED") || wcsstr(messageText, L"FORCED") ||
                wcsstr(messageText, L"ACTIVE")) {
                SetTextColor(hdcStatic, RGB(139, 0, 0)); // Rouge foncé (DarkRed)
            }
            // Si le message contient "OK" ou "Enabled" → VERT
            else if (wcsstr(messageText, L"OK") || wcsstr(messageText, L"Enabled")) {
                SetTextColor(hdcStatic, RGB(34, 197, 94)); // Vert
            }
            else {
                SetTextColor(hdcStatic, RGB(107, 114, 128)); // Gris
            }
        }
        // ========== MESSAGE DE STATUT EN BAS (ID 600) ==========
        else if (controlId == 600) {
            // Erreurs → ROUGE
            if (wcsstr(messageText, L"\u26A0") || wcsstr(messageText, L"Error") ||
                wcsstr(messageText, L"does not exist")) {
                SetTextColor(hdcStatic, RGB(220, 53, 69)); // Rouge vif
            }
            // Succès → VERT
            else if (wcsstr(messageText, L"\u2705") || wcsstr(messageText, L"\u2699") ||
                wcsstr(messageText, L"success")) {
                SetTextColor(hdcStatic, RGB(40, 167, 69)); // Vert
            }
            else {
                SetTextColor(hdcStatic, RGB(108, 117, 125)); // Gris
            }
        }
        // HUD theme / position / size labels (526, 527, 528): white text on dark background art.
        else if (controlId == 526 || controlId == 527 || controlId == 528) {
            SetTextColor(hdcStatic, RGB(255, 255, 255));
        }
        // HUD theme / position / size combo closed state (215, 216, 217): dark fill, white text.
        else if (controlId == 215 || controlId == 216 || controlId == 217) {
            SetTextColor(hdcStatic, RGB(255, 255, 255));
            SetBkColor(hdcStatic, RGB(45, 45, 50));
            return (LRESULT)ui_hud_combo_brush();
        }
        // ========== AUTRES TEXTES (COULEUR PAR DÉFAUT) ==========
        else {
            SetTextColor(hdcStatic, RGB(33, 37, 41)); // Gris très foncé
        }

        // ✅ Retourner NULL_BRUSH pour fond transparent
        return (LRESULT)GetStockObject(NULL_BRUSH);
}

// WM_CTLCOLORLISTBOX: dark styling for the HUD combo dropdown list.
LRESULT ui_config_on_ctlcolorlistbox(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
        // Dropdown list popup for HUD theme combo (white text on dark gray).
        // Liste déroulante du combo thème HUD (texte blanc sur gris foncé).
        if (g_configDialogDestroying) {
            return 0;
        }
        HWND hwndList = (HWND)lParam;
        if (hwndList && ui_is_hud_styled_combo_child(hwndList)) {
            HDC hdcList = (HDC)wParam;
            SetBkMode(hdcList, OPAQUE);
            SetTextColor(hdcList, RGB(255, 255, 255));
            SetBkColor(hdcList, RGB(45, 45, 50));
            return (LRESULT)ui_hud_combo_brush();
        }
        return 0;
}

// WM_CTLCOLOREDIT: dark styling for the HUD combo display field.
LRESULT ui_config_on_ctlcoloredit(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
        // Some Windows versions route combo display through WM_CTLCOLOREDIT.
        // Certaines versions de Windows passent par WM_CTLCOLOREDIT pour l'affichage du combo.
        if (g_configDialogDestroying) {
            return 0;
        }
        HWND hwndEdit = (HWND)lParam;
        if (hwndEdit && ui_is_hud_styled_combo_child(hwndEdit)) {
            HDC hdcEdit = (HDC)wParam;
            SetBkMode(hdcEdit, OPAQUE);
            SetTextColor(hdcEdit, RGB(255, 255, 255));
            SetBkColor(hdcEdit, RGB(45, 45, 50));
            return (LRESULT)ui_hud_combo_brush();
        }
        return 0;
}

// WM_DRAWITEM: owner-draw for key boxes, preset buttons, path box, buttons.
LRESULT ui_config_on_drawitem(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    (void)wParam;
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;
        int ctrlId = lpDIS->CtlID;

    

        if (ctrlId >= 2001 && ctrlId <= 2005) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Load bitmap resource | Charger la ressource bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ConfigDialogProc, &hModule);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Key_Box_01),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

            if (hBitmap) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;

                StretchBlt(hdc, 0, 0, width, height, hdcMem, 0, 0, width, height, SRCCOPY);

                SelectObject(hdcMem, hOldBitmap);
                DeleteDC(hdcMem);
                DeleteObject(hBitmap);
            }

            // Draw text with Patch Configuration style | Dessiner le texte avec le style de Patch Configuration
            HWND hCtrl = lpDIS->hwndItem;
            wchar_t text[256] = L"";
            GetWindowTextW(hCtrl, text, 256);

            if (wcslen(text) > 0) {
                HFONT hTextFont = NULL;
                HFONT hOldFont = NULL;
                if (hFont) {
                    hTextFont = hFont;
                    hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                }
                else {
                    hTextFont = CreateFontW(
                        18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                }

                SetBkMode(hdc, TRANSPARENT);

                // Shadow text | Texte ombre
                SetTextColor(hdc, RGB(0, 0, 0));
                RECT shadowRect = rect;
                OffsetRect(&shadowRect, 1, 1);
                DrawTextW(hdc, text, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Main text | Texte principal
                SetTextColor(hdc, RGB(240, 240, 240));
                DrawTextW(hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, hOldFont);
                if (!hFontBold && hTextFont) {
                    DeleteObject(hTextFont);
                }
            }

            return TRUE;
        }

        // Draw Rename buttons with Rename Box image | Dessiner les boutons Rename avec l'image Rename Box
        if (ctrlId >= 950 && ctrlId < 950 + MAX_VOICE_PRESETS) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Load bitmap resource | Charger la ressource bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ConfigDialogProc, &hModule);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Rename_Box_01),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

            if (hBitmap) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

                BITMAP bm;
                GetObject(hBitmap, sizeof(BITMAP), &bm);

                // Draw bitmap at real size (no stretch) | Dessiner le bitmap à taille réelle (pas de stretch)
                BitBlt(hdc,
                    rect.left,
                    rect.top,
                    bm.bmWidth,
                    bm.bmHeight,
                    hdcMem,
                    0, 0,
                    SRCCOPY);

                SelectObject(hdcMem, hOldBitmap);
                DeleteDC(hdcMem);
                DeleteObject(hBitmap);

                // Draw text centered on image | Dessiner le texte centré sur l'image
                wchar_t text[32] = L"Rename";
                if (wcslen(text) > 0) {
                    HFONT hTextFont = hFont ? hFont : CreateFontW(
                        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    HFONT hOldFont = (HFONT)SelectObject(hdc, hTextFont);

                    RECT textRect;
                    textRect.left = rect.left;
                    textRect.top = rect.top;
                    textRect.right = rect.left + bm.bmWidth;
                    textRect.bottom = rect.top + bm.bmHeight;

                    SetBkMode(hdc, TRANSPARENT);

                    // Shadow text | Texte ombre
                    SetTextColor(hdc, RGB(0, 0, 0));
                    RECT shadowRect = textRect;
                    OffsetRect(&shadowRect, 1, 1);
                    DrawTextW(hdc, text, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Main text | Texte principal
                    SetTextColor(hdc, RGB(240, 240, 240));
                    DrawTextW(hdc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(hdc, hOldFont);
                    if (!hFont) DeleteObject(hTextFont);
                }
            }

            return TRUE;
        }

        // Draw Load buttons with Load Box image | Dessiner les boutons Load avec l'image Load Box
        if (ctrlId >= 900 && ctrlId < 900 + MAX_VOICE_PRESETS) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Load bitmap resource | Charger la ressource bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ConfigDialogProc, &hModule);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Load_Box_01),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

            if (hBitmap) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

                BITMAP bm;
                GetObject(hBitmap, sizeof(BITMAP), &bm);

                // Draw bitmap at real size (no stretch) | Dessiner le bitmap à taille réelle (pas de stretch)
                BitBlt(hdc,
                    rect.left,
                    rect.top,
                    bm.bmWidth,
                    bm.bmHeight,
                    hdcMem,
                    0, 0,
                    SRCCOPY);

                SelectObject(hdcMem, hOldBitmap);
                DeleteDC(hdcMem);
                DeleteObject(hBitmap);

                // Draw text centered on image | Dessiner le texte centré sur l'image
                wchar_t text[32] = L"Load";
                if (wcslen(text) > 0) {
                    HFONT hTextFont = hFont ? hFont : CreateFontW(
                        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    HFONT hOldFont = (HFONT)SelectObject(hdc, hTextFont);

                    RECT textRect;
                    textRect.left = rect.left;
                    textRect.top = rect.top;
                    textRect.right = rect.left + bm.bmWidth;
                    textRect.bottom = rect.top + bm.bmHeight;

                    SetBkMode(hdc, TRANSPARENT);

                    // Shadow text | Texte ombre
                    SetTextColor(hdc, RGB(0, 0, 0));
                    RECT shadowRect = textRect;
                    OffsetRect(&shadowRect, 1, 1);
                    DrawTextW(hdc, text, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Main text | Texte principal
                    SetTextColor(hdc, RGB(240, 240, 240));
                    DrawTextW(hdc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(hdc, hOldFont);
                    if (!hFont) DeleteObject(hTextFont);
                }
            }

            return TRUE;
        }

        // Draw Set Key buttons with Patch Configuration style | Dessiner les boutons Set Key avec le style de Patch Configuration
        if (ctrlId == 101 || ctrlId == 102 || ctrlId == 103 || ctrlId == 104 || ctrlId == 106) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Load bitmap resource | Charger la ressource bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ConfigDialogProc, &hModule);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Key_Box_01),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

            if (hBitmap) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

                BITMAP bm;
                GetObject(hBitmap, sizeof(BITMAP), &bm);

                // Draw bitmap at real size (no stretch) | Dessiner le bitmap à taille réelle (pas de stretch)
                BitBlt(hdc,
                    rect.left,
                    rect.top,
                    bm.bmWidth,
                    bm.bmHeight,
                    hdcMem,
                    0, 0,
                    SRCCOPY);

                SelectObject(hdcMem, hOldBitmap);
                DeleteDC(hdcMem);
                DeleteObject(hBitmap);

                // Draw text with Patch Configuration style | Dessiner le texte avec le style de Patch Configuration
                wchar_t text[32] = L"";
                GetWindowTextW(lpDIS->hwndItem, text, 32);

                if (wcslen(text) > 0) {
                    HFONT hTextFont = NULL;
                    HFONT hOldFont = NULL;
                    if (hFont) {
                        hTextFont = hFont;
                        hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                    }
                    else {
                        hTextFont = CreateFontW(
                            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                        hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                    }

                    RECT textRect;
                    textRect.left = rect.left;
                    textRect.top = rect.top;
                    textRect.right = rect.left + bm.bmWidth;
                    textRect.bottom = rect.top + bm.bmHeight;

                    SetBkMode(hdc, TRANSPARENT);

                    // Shadow text | Texte ombre
                    SetTextColor(hdc, RGB(0, 0, 0));
                    RECT shadowRect = textRect;
                    OffsetRect(&shadowRect, 1, 1);
                    DrawTextW(hdc, text, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    // Main text | Texte principal
                    SetTextColor(hdc, RGB(240, 240, 240));
                    DrawTextW(hdc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(hdc, hOldFont);
                    if (!hFontBold && hTextFont) {
                        DeleteObject(hTextFont);
                    }
                }
            }

            return TRUE;
        }

        // Dans WM_DRAWITEM pour ctrlId == 1100
        if (ctrlId == 1100) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Charger le bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)ConfigDialogProc, &hModule);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Path_Box),
                IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

            int bmWidth = rect.right - rect.left;
            int bmHeight = rect.bottom - rect.top;

            if (hBitmap) {
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

                BITMAP bm;
                GetObject(hBitmap, sizeof(BITMAP), &bm);

                // utiliser les dimensions réelles du bitmap
                bmWidth = bm.bmWidth;
                bmHeight = bm.bmHeight;

                BitBlt(hdc,
                    rect.left, rect.top,
                    bm.bmWidth, bm.bmHeight,
                    hdcMem,
                    0, 0,
                    SRCCOPY);

                SelectObject(hdcMem, hOldBitmap);
                DeleteDC(hdcMem);
                DeleteObject(hBitmap);
            }

            // Convertir modFilePath de char* en wchar_t* pour affichage
            wchar_t displayPath[MAX_PATH] = L"";

            if (enableAutomaticPatchFind) {
                // Mode automatique : afficher le chemin automatique détecté
                wchar_t autoPath[MAX_PATH] = L"";
                if (findConanExilesAutomatic(autoPath, MAX_PATH)) {
                    // Supprimer \ConanSandbox\Saved pour n'afficher que le dossier du jeu
                    wcscpy_s(displayPath, MAX_PATH, autoPath);
                    wchar_t* conanSandbox = wcsstr(displayPath, L"\\ConanSandbox\\Saved");
                    if (conanSandbox) {
                        *conanSandbox = L'\0';
                    }
                }
                else {
                    wcscpy_s(displayPath, MAX_PATH, L"(Not found)");
                }
            }
            else {
                // Mode manuel : afficher displayedPathText (déjà sans \ConanSandbox\Saved)
                wcscpy_s(displayPath, MAX_PATH, displayedPathText);
            }

            if (wcslen(displayPath) == 0) {
                wcscpy_s(displayPath, MAX_PATH, L"(Not configured)");
            }

            if (wcslen(displayPath) > 0) {
                // Sélection de la police
                HFONT hTextFont = NULL;
                HFONT hOldFont = NULL;
                BOOL createdLocalFont = FALSE;
                if (hPathFont) {
                    hTextFont = hPathFont;
                    hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                }
                else if (hFont) {
                    hTextFont = hFont;
                    hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                }
                else {
                    hTextFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    hOldFont = (HFONT)SelectObject(hdc, hTextFont);
                    createdLocalFont = TRUE;
                }

                SetBkMode(hdc, TRANSPARENT);

                // rectangle image exact
                RECT imageRect;
                imageRect.left = rect.left;
                imageRect.top = rect.top;
                imageRect.right = rect.left + bmWidth;
                imageRect.bottom = rect.top + bmHeight;

                const int horizMargin = 8;
                int availWidth = bmWidth - horizMargin * 2;
                if (availWidth < 1) availWidth = 1;

                // mesurer le texte actuel avec la police sélectionnée
                SIZE textSize = { 0, 0 };
                GetTextExtentPoint32W(hdc, displayPath, (int)wcslen(displayPath), &textSize);

                if (textSize.cx <= availWidth) {
                    // texte tient → on calcule une position exacte (pixel-perfect)
                    int textX = imageRect.left + (bmWidth - textSize.cx) / 2;
                    int textY = imageRect.top + (bmHeight - textSize.cy) / 2;

                    // ✅ Texte principal UNIQUEMENT (PAS d'ombre)
                    SetTextColor(hdc, RGB(255, 255, 255));
                    TextOutW(hdc, textX, textY, displayPath, (int)wcslen(displayPath));
                }
                else {
                    // trop long → utiliser DrawText avec DT_END_ELLIPSIS et centrer verticalement
                    RECT dtRect = imageRect;
                    dtRect.left += horizMargin;
                    dtRect.right -= horizMargin;

                    // Calcule la hauteur du texte (DT_CALCRECT) pour centrer verticalement
                    RECT calcRect = dtRect;
                    DrawTextW(hdc, displayPath, -1, &calcRect, DT_SINGLELINE | DT_CALCRECT | DT_END_ELLIPSIS);

                    int textH = calcRect.bottom - calcRect.top;
                    if (textH <= 0) textH = (bmHeight / 2);

                    // Positionner verticalement au centre
                    int top = imageRect.top + (bmHeight - textH) / 2;
                    dtRect.top = top;
                    dtRect.bottom = top + textH;

                    // ✅ Texte principal UNIQUEMENT (PAS d'ombre)
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawTextW(hdc, displayPath, -1, &dtRect,
                        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                }

                // restaurer police et nettoyer si nécessaire
                SelectObject(hdc, hOldFont);
                if (createdLocalFont && hTextFont) {
                    DeleteObject(hTextFont);
                }
            }

            return TRUE;
        }

        // Dessiner les boutons (code existant)
        if (lpDIS->CtlType == ODT_BUTTON) {
            int ctrlId = lpDIS->CtlID;
            if (ctrlId != 201 && ctrlId != 203 && ctrlId != 204) {
                DrawButtonWithBitmap(lpDIS);
                return TRUE;
            }
        }
        return 0;
}
