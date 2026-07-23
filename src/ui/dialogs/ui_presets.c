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
 * ui_presets.c: voice-preset save/rename dialogs + presets category.
 * ui_presets.c : dialogues sauvegarde/renommage de presets + categorie presets.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
 */

// Preset rename dialog procedure | Procédure de dialogue de renommage de preset
LRESULT CALLBACK PresetRenameDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hPresetRenameDialog = hwnd;

        // Load rename dialog background from resources | Charger le fond du dialogue de renommage depuis les ressources
        HMODULE hModuleRename = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)PresetRenameDialogProc, &hModuleRename);

        hBackgroundRenamePresetBitmap = (HBITMAP)LoadImageW(
            hModuleRename,
            MAKEINTRESOURCEW(IDB_Background_Rename_Preset),
            IMAGE_BITMAP,
            0, 0,
            LR_CREATEDIBSECTION);

        if (!hBackgroundRenamePresetBitmap && enableLogGeneral) {
            mumbleAPI.log(ownID, "WARNING: Rename dialog background (IDB_Background_Rename_Preset) not loaded");
        }

        // Current name label | Label du nom actuel
        wchar_t currentText[128];
        swprintf(currentText, 128, L"%S", voicePresets[renamePresetIndex].name);
        HWND hCurrentLabel = CreateWindowW(L"STATIC", currentText,
            WS_VISIBLE | WS_CHILD | SS_LEFT | SS_OWNERDRAW,
            92, 37, 260, 25, hwnd, (HMENU)1500, NULL, NULL);
        ApplyFontToControl(hCurrentLabel, hFont);

        // Get dialog client area for centering | Obtenir la zone cliente du dialogue pour centrage
        RECT dlgRect;
        GetClientRect(hwnd, &dlgRect);
        int dlgWidth = dlgRect.right - dlgRect.left;

        // Input edit control dimensions | Dimensions du contrôle d'édition
        int inputWidth = 260;
        int inputHeight = 30;
        int inputX = (dlgWidth - inputWidth) / 2;
        int inputY = 90;

        HWND hInputEdit = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            inputX, inputY, inputWidth, inputHeight, hwnd, (HMENU)1001, NULL, NULL);

        // ✅ LIMITER À 10 CARACTÈRES MAXIMUM
        SendMessage(hInputEdit, EM_LIMITTEXT, 15, 0);

        SetWindowTextA(hInputEdit, voicePresets[renamePresetIndex].name);
        SetFocus(hInputEdit);
        SendMessage(hInputEdit, EM_SETSEL, 0, -1);

        // Get real dimensions of IDB_OK_Box_01 | Récupérer les dimensions réelles de IDB_OK_Box_01
        HMODULE hModuleOkBtn = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)PresetRenameDialogProc, &hModuleOkBtn);

        if (!hModuleOkBtn) {
            // Fallback: Get module handle from DLL | Repli: Obtenir le handle du module depuis la DLL
            hModuleOkBtn = GetModuleHandleW(NULL);
        }

        HBITMAP hOkBoxTemp = (HBITMAP)LoadImageW(hModuleOkBtn, MAKEINTRESOURCEW(IDB_OK_Box_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        if (!hOkBoxTemp && enableLogGeneral) {
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "ERROR: Failed to load IDB_OK_Box_01 - HMODULE=0x%p", hModuleOkBtn);
            mumbleAPI.log(ownID, logMsg);
        }

        int okBoxWidth = 80;   // Valeur par défaut
        int okBoxHeight = 35;  // Valeur par défaut

        if (hOkBoxTemp) {
            BITMAP bmOk;
            GetObject(hOkBoxTemp, sizeof(BITMAP), &bmOk);
            okBoxWidth = bmOk.bmWidth;
            okBoxHeight = bmOk.bmHeight;
            DeleteObject(hOkBoxTemp);

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_OK_Box_01 size: %dx%d pixels", okBoxWidth, okBoxHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        // Recalculate button positions with real image dimensions | Recalculer les positions avec les vraies dimensions
        int okBtnGap = 10;
        int totalOkBtnWidth = (okBoxWidth * 2) + okBtnGap;
        int okBtnStartX = (dlgWidth - totalOkBtnWidth) / 2;
        int okBtnY = inputY + inputHeight + 30;

        // OK button with real image size | Bouton OK à taille réelle
        HWND hOkButton = CreateWindowW(L"BUTTON", L"OK",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            okBtnStartX, okBtnY, okBoxWidth, okBoxHeight, hwnd, (HMENU)1002, NULL, NULL);
        ApplyFontToControl(hOkButton, hFont);

        // Cancel button with real image size | Bouton Cancel à taille réelle
        HWND hCancelButton = CreateWindowW(L"BUTTON", L"Cancel",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            okBtnStartX + okBoxWidth + okBtnGap, okBtnY, okBoxWidth, okBoxHeight, hwnd, (HMENU)1003, NULL, NULL);
        ApplyFontToControl(hCancelButton, hFont);

        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hwnd, &rect);

        // Draw dialog background bitmap if available | Dessiner le bitmap de fond si disponible
        if (hBackgroundRenamePresetBitmap) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBackgroundRenamePresetBitmap);

            BITMAP bm;
            GetObject(hBackgroundRenamePresetBitmap, sizeof(BITMAP), &bm);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                0, 0, rect.right - rect.left, rect.bottom - rect.top,
                hdcMem,
                0, 0, bm.bmWidth, bm.bmHeight,
                SRCCOPY);

            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        else {
            // Fallback: solid background | Repli : fond uni
            HBRUSH hBrush = CreateSolidBrush(RGB(248, 249, 250));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
        }

        return 1;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;

        // Draw OK and Cancel buttons with bitmap | Dessiner les boutons OK et Cancel avec bitmap
        if (lpDIS->CtlID == 1002 || lpDIS->CtlID == 1003) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            // Load bitmap resource | Charger la ressource bitmap
            HMODULE hModule = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)PresetRenameDialogProc, &hModule);

            if (!hModule) hModule = GetModuleHandleW(NULL);

            HBITMAP hBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_OK_Box_01),
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
                wchar_t text[32] = L"";
                GetWindowTextW(lpDIS->hwndItem, text, 32);

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

                // Main text in white | Texte principal en blanc
                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextW(hdc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, hOldFont);
                if (!hFont) DeleteObject(hTextFont);
            }

            return TRUE;
        }

        // Draw current label with white text and shadow | Dessiner le label actuel avec texte blanc et ombre
        if (lpDIS->CtlID == 1500) {
            HDC hdc = lpDIS->hDC;
            RECT rect = lpDIS->rcItem;

            SetBkMode(hdc, TRANSPARENT);

            wchar_t text[128] = L"";
            GetWindowTextW(lpDIS->hwndItem, text, 128);

            HFONT hTextFont = hFont ? hFont : CreateFontW(
                16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hTextFont);

            // Shadow text | Texte ombre
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT shadowRect = rect;
            OffsetRect(&shadowRect, 1, 1);
            DrawTextW(hdc, text, -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Main text in white | Texte principal en blanc
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            if (!hFont) DeleteObject(hTextFont);

            return TRUE;
        }

        // Draw button bitmaps | Dessiner les bitmaps des boutons
        if (lpDIS->CtlType == ODT_BUTTON) {
            DrawButtonWithBitmap(lpDIS);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case 1002: { // OK
            HWND hEdit = GetDlgItem(hwnd, 1001);
            char newName[PRESET_NAME_MAX_LENGTH];
            GetWindowTextA(hEdit, newName, PRESET_NAME_MAX_LENGTH);

            if (strlen(newName) > 0) {
                renameVoicePreset(renamePresetIndex, newName);
            }

            DestroyWindow(hwnd);
            return 0;
        }
        case 1003: // Cancel
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, RGB(33, 37, 41));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }



    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        SetBkMode(hdcEdit, TRANSPARENT);
        SetTextColor(hdcEdit, RGB(33, 37, 41));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_DESTROY:
        hPresetRenameDialog = NULL;
        // Free background bitmap for rename dialog | Libérer le bitmap de fond du dialogue de renommage
        if (hBackgroundRenamePresetBitmap) {
            DeleteObject(hBackgroundRenamePresetBitmap);
            hBackgroundRenamePresetBitmap = NULL;
        }
        // Réinitialiser le flag pour permettre le redessin à la prochaine ouverture
        backgroundDrawn = FALSE;
        return 0;


    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Preset save dialog procedure | Procédure de dialogue de sauvegarde de preset
LRESULT CALLBACK PresetSaveDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hPresetSaveDialog = hwnd;

        // Load save-presets dialog background from resources | Charger le fond du dialogue de sauvegarde depuis les ressources
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)PresetSaveDialogProc, &hModule);

        hBackgroundSavePresetBitmap = (HBITMAP)LoadImageW(
            hModule,
            MAKEINTRESOURCEW(IDB_Background_Save_Voice_Range_Presets),
            IMAGE_BITMAP,
            0, 0,
            LR_CREATEDIBSECTION);

        if (!hBackgroundSavePresetBitmap && enableLogGeneral) {
            mumbleAPI.log(ownID, "WARNING: Save presets dialog background (IDB_Background_Save_Voice_Range_Presets) not loaded");
        }

        // Create 10 preset buttons using preset box image size | Créer 10 boutons de preset en utilisant la taille de l'image de preset
        int yPos = 90;

        // Get HMODULE and real dimensions of IDB_Preset_Box_01 | Obtenir HMODULE et dimensions réelles de IDB_Preset_Box_01
        HMODULE hPresetModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)PresetSaveDialogProc, &hPresetModule);

        HBITMAP hPresetBoxTemp = (HBITMAP)LoadImageW(hPresetModule, MAKEINTRESOURCEW(IDB_Preset_Box_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int presetBoxWidth = 400;  // fallback width | largeur de repli
        int presetBoxHeight = 35;  // fallback height | hauteur de repli

        if (hPresetBoxTemp) {
            BITMAP bmPreset;
            GetObject(hPresetBoxTemp, sizeof(BITMAP), &bmPreset);
            presetBoxWidth = bmPreset.bmWidth;
            presetBoxHeight = bmPreset.bmHeight;
            DeleteObject(hPresetBoxTemp);

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_Preset_Box_01 size: %dx%d pixels", presetBoxWidth, presetBoxHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
            wchar_t buttonText[128];
            if (voicePresets[i].isUsed) {
                wchar_t wName[PRESET_NAME_MAX_LENGTH];
                size_t converted = 0;
                mbstowcs_s(&converted, wName, PRESET_NAME_MAX_LENGTH, voicePresets[i].name, _TRUNCATE);
                swprintf(buttonText, 128, L"[%d] %s (%.1f / %.1f / %.1f)",
                    i + 1, wName,
                    voicePresets[i].whisperDistance,
                    voicePresets[i].normalDistance,
                    voicePresets[i].shoutDistance);
            }
            else {
                swprintf(buttonText, 128, L"[%d] Empty Slot", i + 1);
            }

            // Calculate centered X for preset button | Calculer la position X centrée pour le bouton preset
            RECT dlgRect;
            GetClientRect(hwnd, &dlgRect);
            int clientWidth = dlgRect.right - dlgRect.left;
            int presetX = (clientWidth - presetBoxWidth) / 2;
            if (presetX < 10) presetX = 10; // keep small left margin if image wider than dialog | garder une petite marge si l'image est plus large que la fenêtre

            HWND hPresetButton = CreateWindowW(L"BUTTON", buttonText,
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
                presetX, yPos, presetBoxWidth, presetBoxHeight, hwnd, (HMENU)(INT_PTR)(700 + i), NULL, NULL);
            ApplyFontToControl(hPresetButton, hFont);

            // vertical spacing = image height + 5px gap | espacement vertical = hauteur image + 5px
            yPos += presetBoxHeight + 5;
        }

        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rect;
        GetClientRect(hwnd, &rect);

        // Draw dialog background bitmap if available | Dessiner le bitmap de fond du dialogue si disponible
        if (hBackgroundSavePresetBitmap) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBackgroundSavePresetBitmap);

            BITMAP bm;
            GetObject(hBackgroundSavePresetBitmap, sizeof(BITMAP), &bm);

            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                0, 0, rect.right - rect.left, rect.bottom - rect.top,
                hdcMem,
                0, 0, bm.bmWidth, bm.bmHeight,
                SRCCOPY);

            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        else {
            // Fallback: solid background | Repli : fond uni
            HBRUSH hBrush = CreateSolidBrush(RGB(248, 249, 250));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
        }

        return 1;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;

        // Dessiner le bitmap pour TOUS les boutons
        if (lpDIS->CtlType == ODT_BUTTON) {
            DrawButtonWithBitmap(lpDIS);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        int buttonId = LOWORD(wParam);

        // Handle preset selection | Gérer la sélection de preset
        if (buttonId >= 700 && buttonId < 700 + MAX_VOICE_PRESETS) {
            int presetIndex = buttonId - 700;
            saveVoicePreset(presetIndex, NULL);
            DestroyWindow(hwnd);
            return 0;
        }
        // Handle cancel | Gérer l'annulation
        else if (buttonId == 999) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, RGB(33, 37, 41));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_DESTROY:
        hPresetSaveDialog = NULL;

        // Free background bitmap for save dialog | Libérer le bitmap de fond du dialogue de sauvegarde
        if (hBackgroundSavePresetBitmap) {
            DeleteObject(hBackgroundSavePresetBitmap);
            hBackgroundSavePresetBitmap = NULL;
        }

        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Show preset save dialog | Afficher le dialogue de sauvegarde de preset
void showPresetSaveDialog(void) {
    if (hPresetSaveDialog && IsWindow(hPresetSaveDialog)) {
        SetForegroundWindow(hPresetSaveDialog);
        return;
    }

    const wchar_t PRESET_DIALOG_CLASS[] = L"PresetSaveDialogClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = PresetSaveDialogProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = PRESET_DIALOG_CLASS;
    wc.hbrBackground = CreateSolidBrush(RGB(248, 249, 250));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    UnregisterClassW(PRESET_DIALOG_CLASS, wc.hInstance);
    RegisterClassW(&wc);

    // ✅ CORRECTION : Centrer au-dessus de l'interface principale
    int dialogWidth = 440;
    int dialogHeight = 580;
    int dialogX, dialogY;

    if (hConfigDialog && IsWindow(hConfigDialog)) {
        // Get parent window position and size | Obtenir position et taille de la fenêtre parente
        RECT parentRect;
        GetWindowRect(hConfigDialog, &parentRect);

        int parentWidth = parentRect.right - parentRect.left;
        int parentHeight = parentRect.bottom - parentRect.top;
        int parentX = parentRect.left;
        int parentY = parentRect.top;

        dialogX = parentX + (parentWidth - dialogWidth) / 2;
        dialogY = parentY + (parentHeight - dialogHeight) / 2;

        if (enableLogGeneral) {
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                "Save dialog: Parent at (%d,%d), Dialog at (%d,%d)",
                parentX, parentY, dialogX, dialogY);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else {
        // Fallback to screen center if parent not available | Centrer sur l'écran si parent indisponible
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        dialogX = (screenWidth - dialogWidth) / 2;
        dialogY = (screenHeight - dialogHeight) / 2;
    }

    hPresetSaveDialog = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PRESET_DIALOG_CLASS,
        L"Save Voice Range Preset",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        dialogX, dialogY, dialogWidth, dialogHeight,
        hConfigDialog, NULL, wc.hInstance, NULL);

    if (hPresetSaveDialog) {
        SetLayeredWindowAttributes(hPresetSaveDialog, 0, 250, LWA_ALPHA);
        ShowWindow(hPresetSaveDialog, SW_SHOW);
        UpdateWindow(hPresetSaveDialog);
    }
}

// Update preset labels in category 3 | Mettre à jour les labels de preset dans la catégorie 3
void updatePresetLabels(void) {
    if (!hConfigDialog || !IsWindow(hConfigDialog)) return;
    if (currentCategory != 3) return;

    for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
        if (hPresetLabels[i] && IsWindow(hPresetLabels[i])) {
            wchar_t labelText[256];
            wchar_t wName[PRESET_NAME_MAX_LENGTH];
            size_t converted = 0;
            mbstowcs_s(&converted, wName, PRESET_NAME_MAX_LENGTH, voicePresets[i].name, _TRUNCATE);

            if (voicePresets[i].isUsed) {
                // ✅ FORMAT SANS CROCHETS
                swprintf(labelText, 256, L"%d %s - W:%.1f N:%.1f S:%.1f m",
                    i + 1, wName,
                    voicePresets[i].whisperDistance,
                    voicePresets[i].normalDistance,
                    voicePresets[i].shoutDistance);
            }
            else {
                swprintf(labelText, 256, L"%d %s (Empty)", i + 1, wName);
            }

            SetWindowTextW(hPresetLabels[i], labelText);
        }

        if (hPresetLoadButtons[i] && IsWindow(hPresetLoadButtons[i])) {
            EnableWindow(hPresetLoadButtons[i], voicePresets[i].isUsed);
        }
    }
}

LRESULT CALLBACK PresetLabelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)dwRefData;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        // Fond transparent
        SetBkMode(hdc, TRANSPARENT);

        // Récupérer le texte du label
        wchar_t text[256];
        GetWindowTextW(hwnd, text, 256);

        // Charger l'icône checkmark
        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)PresetLabelProc, &hModule);

        HICON hCheckIcon = (HICON)LoadImageW(hModule, MAKEINTRESOURCEW(IDI_CHECKMARK),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

        if (hCheckIcon) {
            // Dessiner l'icône à gauche
            int iconX = 2;
            int iconY = (rect.bottom - 16) / 2;
            DrawIconEx(hdc, iconX, iconY, hCheckIcon, 16, 16, 0, NULL, DI_NORMAL);
            DestroyIcon(hCheckIcon);
        }

        // Dessiner le texte décalé de 20px à droite
        RECT textRect = rect;
        textRect.left += 20;

        // Ombre noire | Black shadow
        SetTextColor(hdc, RGB(0, 0, 0));
        RECT shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 1);
        DrawTextW(hdc, text, -1, &shadowRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Texte principal en blanc | Main text in white
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, PresetLabelProc, uIdSubclass);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Create presets category controls | Créer les contrôles de la catégorie presets
void createPresetsCategory(void) {
    // Create preset rows | Créer les lignes de preset
        // Get module handle for loading resources | Obtenir le handle du module pour charger les ressources
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)createPresetsCategory, &hModule);

    int presetYPositions[MAX_VOICE_PRESETS] = {
            215,    // Preset 1
            257,    // Preset 2
            298,    // Preset 3
            340,    // Preset 4
            380,    // Preset 5
            423,    // Preset 6
            465,    // Preset 7
            505,    // Preset 8
            547,    // Preset 9
            587     // Preset 10
    };

    // Manual Y positions for Load and Rename buttons | Positions Y manuelles pour les boutons Load et Rename
    int buttonYPositions[MAX_VOICE_PRESETS] = {
            217,    // Preset 1
            259,    // Preset 2
            300,    // Preset 3
            342,    // Preset 4
            382,    // Preset 5
            426,    // Preset 6
            467,    // Preset 7
            507,    // Preset 8
            549,    // Preset 9
            589     // Preset 10
    };

    for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
        int yPos = presetYPositions[i];
        int buttonY = buttonYPositions[i];

        // Preset name label | Label du nom du preset - SANS EMOJI
        wchar_t labelText[256];
        wchar_t wName[PRESET_NAME_MAX_LENGTH];
        size_t converted = 0;
        mbstowcs_s(&converted, wName, PRESET_NAME_MAX_LENGTH, voicePresets[i].name, _TRUNCATE);

        if (voicePresets[i].isUsed) {
            // ✅ FORMAT CORRIGÉ : Pas d'emoji microphone
            swprintf(labelText, 256, L"[%d] %s - W:%.1f N:%.1f S:%.1f m",
                i + 1, wName,
                voicePresets[i].whisperDistance,
                voicePresets[i].normalDistance,
                voicePresets[i].shoutDistance);
        }
        else {
            swprintf(labelText, 256, L"[%d] %s (Empty)", i + 1, wName);
        }

        hPresetLabels[i] = CreateWindowW(L"STATIC", labelText,
            WS_CHILD | SS_LEFT | SS_OWNERDRAW,
            40, yPos + 5, 300, 25, hConfigDialog, (HMENU)(INT_PTR)(850 + i), NULL, NULL);
        ApplyFontToControl(hPresetLabels[i], hFont);

        // ✅ Activer le custom draw
        SetWindowSubclass(hPresetLabels[i], PresetLabelProc, 850 + i, 0);

        // Get real dimensions of IDB_Load_Box_01 | Récupérer les dimensions réelles de IDB_Load_Box_01
        HBITMAP hLoadBoxTemp = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Load_Box_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int loadBoxWidth = 80;   // Valeur par défaut
        int loadBoxHeight = 30;  // Valeur par défaut

        if (hLoadBoxTemp) {
            BITMAP bmLoad;
            GetObject(hLoadBoxTemp, sizeof(BITMAP), &bmLoad);
            loadBoxWidth = bmLoad.bmWidth;
            loadBoxHeight = bmLoad.bmHeight;
            DeleteObject(hLoadBoxTemp);

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_Load_Box_01 size: %dx%d pixels", loadBoxWidth, loadBoxHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        // Load button with Load Box image size | Bouton charger avec la taille de l'image Load Boxf
        hPresetLoadButtons[i] = CreateWindowW(L"BUTTON", L"Load",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            370, buttonY, loadBoxWidth, loadBoxHeight, hConfigDialog, (HMENU)(INT_PTR)(900 + i), NULL, NULL);
        ApplyFontToControl(hPresetLoadButtons[i], hFont);
        EnableWindow(hPresetLoadButtons[i], voicePresets[i].isUsed);

        // Get real dimensions of IDB_Rename_Box_01 | Récupérer les dimensions réelles de IDB_Rename_Box_01
        HBITMAP hRenameBoxTemp = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Rename_Box_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int renameBoxWidth = 100;  // Valeur par défaut
        int renameBoxHeight = 30;  // Valeur par défaut

        if (hRenameBoxTemp) {
            BITMAP bmRename;
            GetObject(hRenameBoxTemp, sizeof(BITMAP), &bmRename);
            renameBoxWidth = bmRename.bmWidth;
            renameBoxHeight = bmRename.bmHeight;
            DeleteObject(hRenameBoxTemp);

            if (enableLogGeneral) {
                char logMsg[128];
                snprintf(logMsg, sizeof(logMsg),
                    "IDB_Rename_Box_01 size: %dx%d pixels", renameBoxWidth, renameBoxHeight);
                mumbleAPI.log(ownID, logMsg);
            }
        }

        // Rename button with Rename Box image size | Bouton renommer avec la taille de l'image Rename Box
        hPresetRenameButtons[i] = CreateWindowW(L"BUTTON", L"Rename",
            WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
            465, buttonY, renameBoxWidth, renameBoxHeight, hConfigDialog, (HMENU)(INT_PTR)(950 + i), NULL, NULL);
    }
}
