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
 * ui_config_dialog.c: dialog shell (ConfigDialogProc), category switching, foreground helper, GDI teardown, showConfigInterface + thread entry.
 * ui_config_dialog.c : coquille du dialogue, changement de categorie, cycle de vie.
 * Thread: settings-dialog UI thread only. Pure move-split (V8.7).
 */

// Shared config-dialog GDI state (declared in ui_config_internal.h).
// Set during WM_DESTROY so WM_CTLCOLOR* handlers skip freed brushes/fonts.
HBRUSH g_hHudComboBrush = NULL;
BOOL g_configDialogDestroying = FALSE;

// Force window to foreground without affecting mouse | Forcer la fenêtre au premier plan sans affecter la souris
void forceWindowToForegroundNoMouse(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // Force activation without mouse manipulation | Forcer l'activation sans manipulation de la souris
    DWORD currentThreadId = GetCurrentThreadId();
    DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), NULL);

    if (currentThreadId != foregroundThreadId) {
        AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }
    else {
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
    }

    BringWindowToTop(hwnd);
}

// Deletes fonts and the combo brush created for the config dialog.
// Supprime les polices et le pinceau du combo créés pour le dialogue de config.
// Nulls global handles so mumble_shutdown() does not double-free them.
// Met les handles globaux à NULL pour éviter une double libération dans mumble_shutdown().
static void ui_release_config_dialog_gdi(void) {
    if (hFont) {
        DeleteObject(hFont);
        hFont = NULL;
    }
    if (hFontBold) {
        DeleteObject(hFontBold);
        hFontBold = NULL;
    }
    if (hFontLarge) {
        DeleteObject(hFontLarge);
        hFontLarge = NULL;
    }
    if (hFontEmoji) {
        DeleteObject(hFontEmoji);
        hFontEmoji = NULL;
    }
    if (hPathFont) {
        DeleteObject(hPathFont);
        hPathFont = NULL;
    }
    if (g_hHudComboBrush) {
        DeleteObject(g_hHudComboBrush);
        g_hHudComboBrush = NULL;
    }
}

// Show/hide controls based on category | Afficher/masquer les contrôles selon la catégorie
void ShowCategoryControls(int category) {
    currentCategory = category;

    // ✅ DÉSACTIVER LE REDESSIN PENDANT LA TRANSITION
    SendMessage(hConfigDialog, WM_SETREDRAW, FALSE, 0);

    // Get all control handles | Récupérer tous les handles de contrôles
    HWND hExplanation1 = GetDlgItem(hConfigDialog, 401);
    HWND hExplanation2 = GetDlgItem(hConfigDialog, 402);
    HWND hExplanation3 = GetDlgItem(hConfigDialog, 403);
    HWND hPathLabel = GetDlgItem(hConfigDialog, 404);

    HWND hPluginLabel = GetDlgItem(hConfigDialog, 501);
    HWND hKeyLabel = GetDlgItem(hConfigDialog, 502);
    HWND hWhisperLabel = GetDlgItem(hConfigDialog, 503);
    HWND hNormalLabel = GetDlgItem(hConfigDialog, 504);
    HWND hShoutLabel = GetDlgItem(hConfigDialog, 505);
    HWND hConfigLabel = GetDlgItem(hConfigDialog, 506);
    HWND hConfigExplain = GetDlgItem(hConfigDialog, 507);
    HWND hDistanceLabel = GetDlgItem(hConfigDialog, 508);
    HWND hDistanceWhisperLabel = GetDlgItem(hConfigDialog, 509);
    HWND hDistanceNormalLabel = GetDlgItem(hConfigDialog, 510);
    HWND hDistanceShoutLabel = GetDlgItem(hConfigDialog, 511);
    HWND hToggleLabel = GetDlgItem(hConfigDialog, 514);
    HWND hDistanceMutingLabel = GetDlgItem(hConfigDialog, 2001);
    HWND hChannelSwitchingLabel = GetDlgItem(hConfigDialog, 2002);
    HWND hVoiceToggleLabel = GetDlgItem(hConfigDialog, 2003);

    // Preset category controls | Contrôles de la catégorie presets
    HWND hPresetTitle = GetDlgItem(hConfigDialog, 800);
    HWND hPresetInstructions = GetDlgItem(hConfigDialog, 801);

    // ✅ RÉCUPÉRER TOUS LES BOUTONS UNE SEULE FOIS
    HWND hSaveConfigButton = GetDlgItem(hConfigDialog, 1);       // Save Configuration (Patch + Advanced)
    HWND hSaveVoiceRangeButton = GetDlgItem(hConfigDialog, 11); // Save Voice Range (Advanced uniquement)
    HWND hCancelButton = GetDlgItem(hConfigDialog, 2);          // Cancel (jamais affiché)

    if (category == 1) { // ========== PATCH CONFIGURATION ==========
        // Afficher catégorie 1
        if (hSavedPathBg) ShowWindow(hSavedPathBg, SW_SHOW);
        if (hSavedPathEdit) ShowWindow(hSavedPathEdit, SW_SHOW);
        if (hSavedPathButton) ShowWindow(hSavedPathButton, SW_SHOW);

        if (hAutomaticPatchFindCheck) ShowWindow(hAutomaticPatchFindCheck, SW_SHOW);
        HWND hAutomaticPatchFindLabel = GetDlgItem(hConfigDialog, 2004);
        if (hAutomaticPatchFindLabel) ShowWindow(hAutomaticPatchFindLabel, SW_SHOW);

        HWND hideControls[] = {
            hPluginLabel, hKeyLabel, hWhisperLabel, hNormalLabel, hShoutLabel,
            hConfigLabel, hConfigExplain, hDistanceLabel, hDistanceWhisperLabel,
            hDistanceNormalLabel, hDistanceShoutLabel, hToggleLabel,
            hWhisperKeyEdit, hWhisperButton, hNormalKeyEdit, hNormalButton,
            hShoutKeyEdit, hShoutButton, hConfigKeyEdit, hConfigButton,
            hEnableDistanceMutingCheck, hEnableAutomaticChannelChangeCheck,
            hEnableVoiceToggleCheck, hVoiceToggleKeyEdit, hVoiceToggleButton,
            hHudThemeLabel, hHudThemeCombo,
            hHudPositionLabel, hHudPositionCombo,
            hHudSizeLabel, hHudSizeCombo,
            hDistanceWhisperEdit, hDistanceNormalEdit, hDistanceShoutEdit,
            hDistanceMutingLabel, hChannelSwitchingLabel, hVoiceToggleLabel,
            hDistanceWhisperMessage, hDistanceNormalMessage, hDistanceShoutMessage,
            hDistanceMutingMessage, hChannelSwitchingMessage, hPositionalAudioMessage,
            hPresetTitle, hPresetInstructions
        };

        for (int i = 0; i < sizeof(hideControls) / sizeof(HWND); i++) {
            if (hideControls[i]) ShowWindow(hideControls[i], SW_HIDE);
        }

        // Masquer presets
        for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
            if (hPresetLabels[i]) ShowWindow(hPresetLabels[i], SW_HIDE);
            if (hPresetLoadButtons[i]) ShowWindow(hPresetLoadButtons[i], SW_HIDE);
            if (hPresetRenameButtons[i]) ShowWindow(hPresetRenameButtons[i], SW_HIDE);
        }

        // Boutons
        if (hSaveConfigButton) {
            ShowWindow(hSaveConfigButton, SW_SHOW);
            // S'assurer que le bouton est au-dessus après redraw
            SetWindowPos(hSaveConfigButton, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (hSaveVoiceRangeButton) {
            ShowWindow(hSaveVoiceRangeButton, SW_HIDE);
        }
        if (hCancelButton) ShowWindow(hCancelButton, SW_HIDE);
    }
    else if (category == 2) { // ========== ADVANCED OPTIONS ==========
        if (hSavedPathBg) ShowWindow(hSavedPathBg, SW_HIDE);
        // Hide patch controls | Masquer contrôles patch
        if (hSavedPathBg) ShowWindow(hSavedPathBg, SW_HIDE);

        // ✅ MASQUER la checkbox et le label Automatic Patch Find
        if (hAutomaticPatchFindCheck) ShowWindow(hAutomaticPatchFindCheck, SW_HIDE);
        HWND hAutomaticPatchFindLabel = GetDlgItem(hConfigDialog, 2004);
        if (hAutomaticPatchFindLabel) ShowWindow(hAutomaticPatchFindLabel, SW_HIDE);

        // Hide Automatic Patch Find controls | Masquer les contrôles Automatic Patch Find
        HWND hAutomaticPatchFindLabelHide3 = GetDlgItem(hConfigDialog, 2000);
        if (hAutomaticPatchFindLabelHide3) ShowWindow(hAutomaticPatchFindLabelHide3, SW_HIDE);
        if (hExplanation2) ShowWindow(hExplanation2, SW_HIDE);
        if (hExplanation3) ShowWindow(hExplanation3, SW_HIDE);
        if (hPathLabel) ShowWindow(hPathLabel, SW_HIDE);
        if (hSavedPathEdit) ShowWindow(hSavedPathEdit, SW_HIDE);
        if (hSavedPathButton) ShowWindow(hSavedPathButton, SW_HIDE);

        // Hide Automatic Patch Find controls in category 3 | Masquer les contrôles Automatic Patch Find en catégorie 3
        
        HWND hAutomaticPatchFindLabelHide = GetDlgItem(hConfigDialog, 2000);
        if (hAutomaticPatchFindLabelHide) ShowWindow(hAutomaticPatchFindLabelHide, SW_HIDE);

        // Show advanced options | Afficher options avancées
        if (hPluginLabel) ShowWindow(hPluginLabel, SW_SHOW);
        if (hKeyLabel) ShowWindow(hKeyLabel, SW_SHOW);
        if (hWhisperLabel) ShowWindow(hWhisperLabel, SW_SHOW);
        if (hNormalLabel) ShowWindow(hNormalLabel, SW_SHOW);
        if (hShoutLabel) ShowWindow(hShoutLabel, SW_SHOW);
        if (hConfigLabel) ShowWindow(hConfigLabel, SW_SHOW);
        if (hConfigExplain) ShowWindow(hConfigExplain, SW_SHOW);
        if (hDistanceLabel) ShowWindow(hDistanceLabel, SW_SHOW);
        if (hDistanceWhisperLabel) ShowWindow(hDistanceWhisperLabel, SW_SHOW);
        if (hDistanceNormalLabel) ShowWindow(hDistanceNormalLabel, SW_SHOW);
        if (hDistanceShoutLabel) ShowWindow(hDistanceShoutLabel, SW_SHOW);
        if (hWhisperKeyEdit) ShowWindow(hWhisperKeyEdit, SW_SHOW);
        if (hWhisperButton) ShowWindow(hWhisperButton, SW_SHOW);
        if (hNormalKeyEdit) ShowWindow(hNormalKeyEdit, SW_SHOW);
        if (hNormalButton) ShowWindow(hNormalButton, SW_SHOW);
        if (hShoutKeyEdit) ShowWindow(hShoutKeyEdit, SW_SHOW);
        if (hShoutButton) ShowWindow(hShoutButton, SW_SHOW);
        if (hConfigKeyEdit) ShowWindow(hConfigKeyEdit, SW_SHOW);
        if (hConfigButton) ShowWindow(hConfigButton, SW_SHOW);
        if (hEnableDistanceMutingCheck) ShowWindow(hEnableDistanceMutingCheck, SW_SHOW);
        if (hEnableAutomaticChannelChangeCheck) ShowWindow(hEnableAutomaticChannelChangeCheck, SW_SHOW);
        if (hDistanceWhisperEdit) ShowWindow(hDistanceWhisperEdit, SW_SHOW);
        if (hDistanceNormalEdit) ShowWindow(hDistanceNormalEdit, SW_SHOW);
        if (hDistanceShoutEdit) ShowWindow(hDistanceShoutEdit, SW_SHOW);
        if (hEnableVoiceToggleCheck) ShowWindow(hEnableVoiceToggleCheck, SW_SHOW);
        if (hVoiceToggleKeyEdit) ShowWindow(hVoiceToggleKeyEdit, SW_SHOW);
        if (hVoiceToggleButton) ShowWindow(hVoiceToggleButton, SW_SHOW);
        if (hHudThemeLabel) ShowWindow(hHudThemeLabel, SW_SHOW);
        if (hHudThemeCombo) ShowWindow(hHudThemeCombo, SW_SHOW);
        if (hHudPositionLabel) ShowWindow(hHudPositionLabel, SW_SHOW);
        if (hHudPositionCombo) ShowWindow(hHudPositionCombo, SW_SHOW);
        if (hHudSizeLabel) ShowWindow(hHudSizeLabel, SW_SHOW);
        if (hHudSizeCombo) ShowWindow(hHudSizeCombo, SW_SHOW);
        if (hToggleLabel) ShowWindow(hToggleLabel, SW_SHOW);
        if (hDistanceMutingLabel) ShowWindow(hDistanceMutingLabel, SW_SHOW);
        if (hChannelSwitchingLabel) ShowWindow(hChannelSwitchingLabel, SW_SHOW);
        if (hVoiceToggleLabel) ShowWindow(hVoiceToggleLabel, SW_SHOW);

        // Show messages | Afficher messages
        if (hDistanceWhisperMessage) ShowWindow(hDistanceWhisperMessage, SW_SHOW);
        if (hDistanceNormalMessage) ShowWindow(hDistanceNormalMessage, SW_SHOW);
        if (hDistanceShoutMessage) ShowWindow(hDistanceShoutMessage, SW_SHOW);
        if (hDistanceMutingMessage) ShowWindow(hDistanceMutingMessage, SW_SHOW);
        if (hChannelSwitchingMessage) ShowWindow(hChannelSwitchingMessage, SW_SHOW);
        if (hPositionalAudioMessage) ShowWindow(hPositionalAudioMessage, SW_SHOW);

        updateDynamicInterface();

        // Hide preset controls | Masquer contrôles presets
        if (hPresetTitle) ShowWindow(hPresetTitle, SW_HIDE);
        if (hPresetInstructions) ShowWindow(hPresetInstructions, SW_HIDE);
        for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
            if (hPresetLabels[i]) ShowWindow(hPresetLabels[i], SW_HIDE);
            if (hPresetLoadButtons[i]) ShowWindow(hPresetLoadButtons[i], SW_HIDE);
            if (hPresetRenameButtons[i]) ShowWindow(hPresetRenameButtons[i], SW_HIDE);
        }

        // ✅ BOUTONS POUR CATÉGORIE 2 : Save Voice Range + Save Configuration (PAS Cancel)
        if (hSaveConfigButton) {
            ShowWindow(hSaveConfigButton, SW_SHOW);
            SetWindowPos(hSaveConfigButton, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (hSaveVoiceRangeButton) {
            ShowWindow(hSaveVoiceRangeButton, SW_SHOW);
            // Forcer Z-order au-dessus (résout "partiellement visible / coupé")
            SetWindowPos(hSaveVoiceRangeButton, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (hCancelButton) ShowWindow(hCancelButton, SW_SHOW);
    }
    else if (category == 3) { // ========== VOICE RANGE PRESETS ==========
        // Hide patch controls | Masquer contrôles patch
        if (hSavedPathBg) ShowWindow(hSavedPathBg, SW_HIDE);
        if (hAutomaticPatchFindCheck) ShowWindow(hAutomaticPatchFindCheck, SW_HIDE);
        HWND hAutomaticPatchFindLabel = GetDlgItem(hConfigDialog, 2004);
        if (hAutomaticPatchFindLabel) ShowWindow(hAutomaticPatchFindLabel, SW_HIDE);
        if (hExplanation1) ShowWindow(hExplanation1, SW_HIDE);
        if (hExplanation2) ShowWindow(hExplanation2, SW_HIDE);
        if (hExplanation3) ShowWindow(hExplanation3, SW_HIDE);
        if (hPathLabel) ShowWindow(hPathLabel, SW_HIDE);
        if (hSavedPathEdit) ShowWindow(hSavedPathEdit, SW_HIDE);
        if (hSavedPathButton) ShowWindow(hSavedPathButton, SW_HIDE);
        // Hide Automatic Patch Find controls in category 3 | Masquer les contrôles Automatic Patch Find en catégorie 3
        HWND hAutomaticPatchFindLabelHide = GetDlgItem(hConfigDialog, 2000);
        if (hAutomaticPatchFindLabelHide) ShowWindow(hAutomaticPatchFindLabelHide, SW_HIDE);
        if (hDistanceMutingLabel) ShowWindow(hDistanceMutingLabel, SW_HIDE);
        if (hChannelSwitchingLabel) ShowWindow(hChannelSwitchingLabel, SW_HIDE);
        if (hVoiceToggleLabel) ShowWindow(hVoiceToggleLabel, SW_HIDE);

        // Hide advanced options | Masquer options avancées
        if (hPluginLabel) ShowWindow(hPluginLabel, SW_HIDE);
        if (hKeyLabel) ShowWindow(hKeyLabel, SW_HIDE);
        if (hWhisperLabel) ShowWindow(hWhisperLabel, SW_HIDE);
        if (hNormalLabel) ShowWindow(hNormalLabel, SW_HIDE);
        if (hShoutLabel) ShowWindow(hShoutLabel, SW_HIDE);
        if (hConfigLabel) ShowWindow(hConfigLabel, SW_HIDE);
        if (hConfigExplain) ShowWindow(hConfigExplain, SW_HIDE);
        if (hDistanceLabel) ShowWindow(hDistanceLabel, SW_HIDE);
        if (hDistanceWhisperLabel) ShowWindow(hDistanceWhisperLabel, SW_HIDE);
        if (hDistanceNormalLabel) ShowWindow(hDistanceNormalLabel, SW_HIDE);
        if (hDistanceShoutLabel) ShowWindow(hDistanceShoutLabel, SW_HIDE);
        if (hWhisperKeyEdit) ShowWindow(hWhisperKeyEdit, SW_HIDE);
        if (hWhisperButton) ShowWindow(hWhisperButton, SW_HIDE);
        if (hNormalKeyEdit) ShowWindow(hNormalKeyEdit, SW_HIDE);
        if (hNormalButton) ShowWindow(hNormalButton, SW_HIDE);
        if (hShoutKeyEdit) ShowWindow(hShoutKeyEdit, SW_HIDE);
        if (hShoutButton) ShowWindow(hShoutButton, SW_HIDE);
        if (hConfigKeyEdit) ShowWindow(hConfigKeyEdit, SW_HIDE);
        if (hConfigButton) ShowWindow(hConfigButton, SW_HIDE);
        if (hEnableDistanceMutingCheck) ShowWindow(hEnableDistanceMutingCheck, SW_HIDE);
        if (hEnableAutomaticChannelChangeCheck) ShowWindow(hEnableAutomaticChannelChangeCheck, SW_HIDE);
        if (hDistanceWhisperEdit) ShowWindow(hDistanceWhisperEdit, SW_HIDE);
        if (hDistanceNormalEdit) ShowWindow(hDistanceNormalEdit, SW_HIDE);
        if (hDistanceShoutEdit) ShowWindow(hDistanceShoutEdit, SW_HIDE);
        if (hEnableVoiceToggleCheck) ShowWindow(hEnableVoiceToggleCheck, SW_HIDE);
        if (hVoiceToggleKeyEdit) ShowWindow(hVoiceToggleKeyEdit, SW_HIDE);
        if (hVoiceToggleButton) ShowWindow(hVoiceToggleButton, SW_HIDE);
        if (hHudThemeLabel) ShowWindow(hHudThemeLabel, SW_HIDE);
        if (hHudThemeCombo) ShowWindow(hHudThemeCombo, SW_HIDE);
        if (hHudPositionLabel) ShowWindow(hHudPositionLabel, SW_HIDE);
        if (hHudPositionCombo) ShowWindow(hHudPositionCombo, SW_HIDE);
        if (hHudSizeLabel) ShowWindow(hHudSizeLabel, SW_HIDE);
        if (hHudSizeCombo) ShowWindow(hHudSizeCombo, SW_HIDE);
        if (hToggleLabel) ShowWindow(hToggleLabel, SW_HIDE);

        // Hide messages | Masquer messages
        if (hDistanceWhisperMessage) {
            SetWindowTextW(hDistanceWhisperMessage, L"");
            ShowWindow(hDistanceWhisperMessage, SW_HIDE);
        }
        if (hDistanceNormalMessage) {
            SetWindowTextW(hDistanceNormalMessage, L"");
            ShowWindow(hDistanceNormalMessage, SW_HIDE);
        }
        if (hDistanceShoutMessage) {
            SetWindowTextW(hDistanceShoutMessage, L"");
            ShowWindow(hDistanceShoutMessage, SW_HIDE);
        }
        if (hDistanceMutingMessage) {
            SetWindowTextW(hDistanceMutingMessage, L"");
            ShowWindow(hDistanceMutingMessage, SW_HIDE);
        }
        if (hChannelSwitchingMessage) {
            SetWindowTextW(hChannelSwitchingMessage, L"");
            ShowWindow(hChannelSwitchingMessage, SW_HIDE);
        }
        if (hPositionalAudioMessage) {
            SetWindowTextW(hPositionalAudioMessage, L"");
            ShowWindow(hPositionalAudioMessage, SW_HIDE);
        }

        // Show preset controls | Afficher contrôles presets
        if (hPresetTitle) ShowWindow(hPresetTitle, SW_SHOW);
        if (hPresetInstructions) ShowWindow(hPresetInstructions, SW_SHOW);
        for (int i = 0; i < MAX_VOICE_PRESETS; i++) {
            if (hPresetLabels[i]) ShowWindow(hPresetLabels[i], SW_SHOW);
            if (hPresetLoadButtons[i]) ShowWindow(hPresetLoadButtons[i], SW_SHOW);
            if (hPresetRenameButtons[i]) ShowWindow(hPresetRenameButtons[i], SW_SHOW);
        }

        // ✅ BOUTONS POUR CATÉGORIE 3 : TOUT MASQUÉ (pas de boutons en bas)
        if (hSaveConfigButton) ShowWindow(hSaveConfigButton, SW_HIDE);
        if (hSaveVoiceRangeButton) ShowWindow(hSaveVoiceRangeButton, SW_HIDE);
        if (hCancelButton) ShowWindow(hCancelButton, SW_HIDE);

        updatePresetLabels();
    }

    SendMessage(hConfigDialog, WM_SETREDRAW, TRUE, 0);

    // Remplacer InvalidateRect + UpdateWindow par un RedrawWindow complet
    // RDW_ERASE forcera l'appel de WM_ERASEBKGND (qui dessine l'image seulement si currentCategory==1)
    // RDW_ALLCHILDREN assure que les enfants sont rafraîchis proprement — évite les "carrés blancs" résiduels.
    RedrawWindow(hConfigDialog, NULL, NULL,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // Mettre à jour l'état des boutons de catégorie
    if (hCategoryPatch && hCategoryAdvanced && hCategoryPresets) {
        SendMessage(hCategoryPatch, BM_SETSTATE, (category == 1) ? TRUE : FALSE, 0);
        SendMessage(hCategoryAdvanced, BM_SETSTATE, (category == 2) ? TRUE : FALSE, 0);
        SendMessage(hCategoryPresets, BM_SETSTATE, (category == 3) ? TRUE : FALSE, 0);
    }
}

// Apply font to control | Appliquer la police à un contrôle
void ApplyFontToControl(HWND control, HFONT font) {
    if (font && control) {
        SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
    }
}

// Main window procedure | Procedure de la fenetre principale
LRESULT CALLBACK ConfigDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return ui_config_on_create(hwnd, wParam, lParam);

    case WM_ERASEBKGND:
        return ui_config_on_erasebkgnd(hwnd, wParam, lParam);

    case WM_CTLCOLORSTATIC:
        return ui_config_on_ctlcolorstatic(hwnd, wParam, lParam);

    case WM_CTLCOLORLISTBOX:
        return ui_config_on_ctlcolorlistbox(hwnd, wParam, lParam);

    case WM_CTLCOLOREDIT:
        return ui_config_on_ctlcoloredit(hwnd, wParam, lParam);

    case WM_COMMAND:
        return ui_config_on_command(hwnd, wParam, lParam);

    case WM_TIMER:
        if (wParam == 1) {
            // Timer for key capture | Timer pour capture des touches
            processKeyCapture();
        }
        else if (wParam == 2) {
            // Timer to clear status message | Timer pour effacer le message de statut
            clearStatusMessage();
            KillTimer(hwnd, 2);
        }

        break;

    case WM_DRAWITEM:
        return ui_config_on_drawitem(hwnd, wParam, lParam);

    case WM_DESTROY:
        // Tear down order: flag handlers, clear HWND globals, free GDI, exit message loop.
        // Ordre de fermeture : signaler aux handlers, vider les HWND, libérer le GDI, quitter la boucle.
        g_configDialogDestroying = TRUE;
        hHudThemeCombo = NULL;
        hHudThemeLabel = NULL;
        hHudPositionCombo = NULL;
        hHudPositionLabel = NULL;
        hHudSizeCombo = NULL;
        hHudSizeLabel = NULL;
        hConfigDialog = NULL;
        ui_release_config_dialog_gdi();
        PostQuitMessage(0); // Ends GetMessage loop in showConfigInterface() | Termine la boucle GetMessage
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return 0;
}

// Show configuration interface centered on screen | Afficher l'interface de configuration centrée à l'écran
int showConfigInterface() {
    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Function started");
    }

    // Try automatic patch find if enabled | Essayer le patch automatique si activé
    if (enableAutomaticPatchFind) {
        wchar_t automaticPath[MAX_PATH] = L"";
        if (findConanExilesAutomatic(automaticPath, MAX_PATH)) {
            wcscpy_s(savedPath, MAX_PATH, automaticPath);
            size_t converted = 0;
            wcstombs_s(&converted, modFilePath, MAX_PATH, automaticPath, _TRUNCATE);
            strcat_s(modFilePath, MAX_PATH, "\\Pos.txt");

            // Save automatic path to config immediately | Sauvegarder le chemin automatique immédiatement
            wchar_t gameFolder[MAX_PATH];
            wcscpy_s(gameFolder, MAX_PATH, automaticPath);
            wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
            if (conanSandbox) {
                *conanSandbox = L'\0';
            }

            wchar_t distWhisper[32], distNormal[32], distShout[32];
            swprintf(distWhisper, 32, L"%.1f", distanceWhisper);
            swprintf(distNormal, 32, L"%.1f", distanceNormal);
            swprintf(distShout, 32, L"%.1f", distanceShout);

            writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

            if (enableLogConfig) {
                char logMsg[512];
                snprintf(logMsg, sizeof(logMsg), "Automatic patch found, applied and saved: %s", modFilePath);
                mumbleAPI.log(ownID, logMsg);
            }
        }
    }

    /* V8.5b single reader: pull current values from g_config (loaded once at
       init by config_load) instead of re-reading plugin.cfg directly. */
    plugin_ui_sync_from_config();
    voice_overlay_refresh_theme();
    voice_overlay_refresh_position();
    voice_overlay_refresh_size();

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Configuration settings read");
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        if (enableLogGeneral) {
            char errorMsg[128];
            snprintf(errorMsg, sizeof(errorMsg), "showConfigInterface: COM initialization failed with HRESULT: 0x%08X", hr);
            mumbleAPI.log(ownID, errorMsg);
        }
        MessageBoxW(NULL, L"Failed to initialize COM", L"Error", MB_OK | MB_ICONERROR);
        config_dialog_close();
        return -1;
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: COM initialized successfully");
    }

    const wchar_t CONFIG_CLASS_NAME[] = L"ModernConfigClass";

    // Charger l'image de fond depuis la ressource AVANT d'enregistrer la classe
    if (!hBackgroundBitmap) {
        hBackgroundBitmap = LoadBackgroundFromResource(IDB_BACKGROUND);
        if (hBackgroundBitmap && enableLogGeneral) {
            mumbleAPI.log(ownID, "Background bitmap loaded for class background");
        }
    }

    // ✅ IMPORTANT : Utiliser NULL_BRUSH pour empêcher Windows de peindre le fond
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ConfigDialogProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = CONFIG_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    UnregisterClassW(CONFIG_CLASS_NAME, wc.hInstance);

    ATOM classAtom = RegisterClassW(&wc);
    if (classAtom == 0) {
        DWORD error = GetLastError();
        if (enableLogGeneral) {
            char errorMsg[128];
            snprintf(errorMsg, sizeof(errorMsg), "showConfigInterface: RegisterClassW failed with error: %lu", error);
            mumbleAPI.log(ownID, errorMsg);
        }
        CoUninitialize();
        config_dialog_close();
        return -1;
    }

    // Get screen dimensions without affecting mouse | Obtenir les dimensions de l'écran sans affecter la souris
    RECT desktopRect;
    GetWindowRect(GetDesktopWindow(), &desktopRect);

    int windowWidth = 600;
    int windowHeight = 780;

    // Center on main screen | Centrer sur l'écran principal
    int windowX = (desktopRect.right - desktopRect.left - windowWidth) / 2;
    int windowY = (desktopRect.bottom - desktopRect.top - windowHeight) / 2;

    if (windowX < 10) windowX = 10;
    if (windowY < 10) windowY = 10;

    if (enableLogGeneral) {
        char posMsg[256];
        snprintf(posMsg, sizeof(posMsg), "showConfigInterface: Positioning window at screen center - Window: (%d,%d)",
            windowX, windowY);
        mumbleAPI.log(ownID, posMsg);
    }

    hConfigDialog = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CONFIG_CLASS_NAME,
        L"\U0001F3AE Plugin Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, wc.hInstance, NULL);

    if (!hConfigDialog) {
        DWORD error = GetLastError();
        if (enableLogGeneral) {
            char errorMsg[128];
            snprintf(errorMsg, sizeof(errorMsg), "showConfigInterface: CreateWindowExW failed with error: %lu", error);
            mumbleAPI.log(ownID, errorMsg);
        }
        CoUninitialize();
        config_dialog_close();
        return -1;
    }

    if (enableLogGeneral) {
        char msg[128];
        snprintf(msg, sizeof(msg), "showConfigInterface: Window created successfully, hWnd = 0x%p", hConfigDialog);
        mumbleAPI.log(ownID, msg);
    }

    SetLayeredWindowAttributes(hConfigDialog, 0, 255, LWA_ALPHA);

    InvalidateRect(hConfigDialog, NULL, TRUE);
    RedrawWindow(hConfigDialog, NULL, NULL,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    UpdateWindow(hConfigDialog);

    // Force window to foreground without affecting mouse | Forcer la fenêtre au premier plan sans affecter la souris
    forceWindowToForegroundNoMouse(hConfigDialog);

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Window positioned at screen center and forced to foreground");
    }

    SetTimer(hConfigDialog, 1, 50, NULL);

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Timer set, entering message loop");
    }

    MSG msg = { 0 };
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Message loop exited");
    }

    CoUninitialize();
    config_dialog_close();

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showConfigInterface: Function completed successfully");
    }

    return 0;
}

// Path selection dialog thread | Thread pour la boîte de dialogue de sélection de chemin
void showPathSelectionDialogThread(void* arg) {
    (void)arg;
    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "showPathSelectionDialogThread: Thread started");
    }

    int result = showConfigInterface();

    if (enableLogGeneral) {
        char msg[128];
        snprintf(msg, sizeof(msg), "showPathSelectionDialogThread: Thread finished with result: %d", result);
        mumbleAPI.log(ownID, msg);
    }
}
