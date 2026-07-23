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

// MODULE 14: SETTINGS UI (F10 DIALOG)
// EN: plugin.cfg editor — paths, voice distances, zone overrides, presets, debug options.
// FR: Éditeur plugin.cfg — chemins, distances voix, surcharges zone, presets, options debug.
// ============================================================================

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

// ---------------------------------------------------------------------------
// HUD theme picker (F10 -> Advanced Options) and config-dialog GDI cleanup.
// Sélecteur de thème HUD (F10 -> Options avancées) et nettoyage GDI du dialogue.
// voiceHudTheme (0..5) is stored in plugin.cfg as HudTheme=N and drives
// the in-game voice overlay palette in voice_overlay.c.
// voiceHudTheme (0..5) est enregistré dans plugin.cfg (HudTheme=N) et pilote
// la palette de l'overlay vocal en jeu dans voice_overlay.c.
// ---------------------------------------------------------------------------
static HBRUSH g_hHudComboBrush = NULL;
// Set during WM_DESTROY so WM_CTLCOLOR* handlers skip freed brushes/fonts.
// Activé pendant WM_DESTROY pour que WM_CTLCOLOR* n'utilise plus les ressources libérées.
static BOOL g_configDialogDestroying = FALSE;

// Returns the dark brush for the HUD theme combo, or a stock brush while tearing down.
// Retourne le pinceau sombre du combo thème HUD, ou un pinceau système pendant la fermeture.
static HBRUSH ui_hud_combo_brush(void) {
    if (g_configDialogDestroying || !g_hHudComboBrush) {
        return (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    }
    return g_hHudComboBrush;
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

// Selects the combo item that matches the loaded voiceHudTheme value.
// Sélectionne l'entrée correspondant à la valeur voiceHudTheme chargée.
static void ui_sync_hud_theme_combo(void) {
    if (!hHudThemeCombo || !IsWindow(hHudThemeCombo)) {
        return;
    }
    if (voiceHudTheme < 0 || voiceHudTheme >= VOICE_HUD_THEME_COUNT) {
        voiceHudTheme = VOICE_HUD_THEME_GRAY;
    }
    SendMessage(hHudThemeCombo, CB_SETCURSEL, (WPARAM)voiceHudTheme, 0);
}

// Reads the user's combo selection into voiceHudTheme (not persisted until Save).
// Lit la sélection utilisateur dans voiceHudTheme (non sauvegardé avant Enregistrer).
static void ui_read_hud_theme_from_combo(void) {
    if (!hHudThemeCombo || !IsWindow(hHudThemeCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudThemeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_THEME_COUNT) {
        voiceHudTheme = sel;
    }
}

static void ui_sync_hud_position_combo(void) {
    if (!hHudPositionCombo || !IsWindow(hHudPositionCombo)) {
        return;
    }
    if (voiceHudPosition < 0 || voiceHudPosition >= VOICE_HUD_POSITION_COUNT) {
        voiceHudPosition = VOICE_HUD_POSITION_BOTTOM_RIGHT;
    }
    SendMessage(hHudPositionCombo, CB_SETCURSEL, (WPARAM)voiceHudPosition, 0);
}

static void ui_read_hud_position_from_combo(void) {
    if (!hHudPositionCombo || !IsWindow(hHudPositionCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudPositionCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_POSITION_COUNT) {
        voiceHudPosition = sel;
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
    if (voiceHudSize < 0 || voiceHudSize >= VOICE_HUD_SIZE_COUNT) {
        voiceHudSize = VOICE_HUD_SIZE_BIG;
    }
    SendMessage(hHudSizeCombo, CB_SETCURSEL, (WPARAM)voiceHudSize, 0);
}

static void ui_read_hud_size_from_combo(void) {
    if (!hHudSizeCombo || !IsWindow(hHudSizeCombo)) {
        return;
    }
    int sel = (int)SendMessage(hHudSizeCombo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < VOICE_HUD_SIZE_COUNT) {
        voiceHudSize = sel;
    }
}

static int ui_is_hud_styled_combo_child(HWND hwnd) {
    if (!hwnd) {
        return 0;
    }
    HWND parent = GetParent(hwnd);
    return (parent == hHudThemeCombo || parent == hHudPositionCombo || parent == hHudSizeCombo);
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

// Check if Saved folder exists in game folder | Vérifier que le dossier Saved existe dans le dossier du jeu
int savedExistsInFolder(const wchar_t* folderPath) {
    wchar_t savedCheckPath[MAX_PATH];
    swprintf(savedCheckPath, MAX_PATH, L"%s\\ConanSandbox\\Saved", folderPath);
    DWORD attribs = GetFileAttributesW(savedCheckPath);
    return (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY));
}

// Key capture processing | Traitement de la capture de touches
void processKeyCapture() {
    if (!isCapturingKey) return;

    BOOL keyFound = FALSE;

    for (int vk = 1; vk < 256; vk++) {
        if (vk == 27) continue; // Skip ESC | Ignorer Echap
        if (GetAsyncKeyState(vk) & 0x8000) {
            // Touche détectée | Key detected
            switch (captureKeyTarget) {
            case 1: whisperKey = vk; if (hWhisperKeyEdit) SetWindowTextA(hWhisperKeyEdit, getKeyName(vk)); break;
            case 2: normalKey = vk; if (hNormalKeyEdit) SetWindowTextA(hNormalKeyEdit, getKeyName(vk)); break;
            case 3: shoutKey = vk; if (hShoutKeyEdit) SetWindowTextA(hShoutKeyEdit, getKeyName(vk)); break;
            case 4: configUIKey = vk; if (hConfigKeyEdit) SetWindowTextA(hConfigKeyEdit, getKeyName(vk)); break;
            case 5: voiceToggleKey = vk; if (hVoiceToggleKeyEdit) SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(vk)); break;
            }

            isCapturingKey = FALSE;
            captureKeyTarget = 0;
            keyFound = TRUE;

            // Réactiver TOUS les boutons immédiatement | Re-enable ALL buttons immediately
            if (hWhisperButton) EnableWindow(hWhisperButton, TRUE);
            if (hNormalButton) EnableWindow(hNormalButton, TRUE);
            if (hShoutButton) EnableWindow(hShoutButton, TRUE);
            if (hConfigButton) EnableWindow(hConfigButton, TRUE);
            if (hVoiceToggleButton) EnableWindow(hVoiceToggleButton, TRUE);

            break;
        }
    }

    // ✅ CORRECTION : Ne pas attendre si aucune touche trouvée
    // Cette condition évite le blocage sur 200ms si l'utilisateur relâche avant la détection
    if (keyFound) {
        Sleep(100); // Attendre le relâchement | Wait for key release
    }
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

// Load default settings from config file | Charger les paramètres par défaut depuis le fichier de config
void loadDefaultSettingsFromConfig() {
    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\default_settings.cfg", configFolder);

    FILE* f = _wfopen(configFile, L"r");
    if (!f) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "Default settings file not found - will be created on first connection");
        }
        return;
    }

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') ++p;
        wchar_t* end = p + wcslen(p);
        while (end > p && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' ' || end[-1] == L'\t'))
            *--end = L'\0';

        if (*p == L'#' || *p == L';' || *p == L'\0') continue;

        wchar_t* eq = wcschr(p, L'=');
        if (!eq) continue;
        *eq = L'\0';
        wchar_t* key = p;
        wchar_t* val = eq + 1;
        while (*val == L' ' || *val == L'\t') ++val;

        if (wcsncmp(key, L"ServerConfigHash", 16) == 0) {
            size_t converted = 0;
            wcstombs_s(&converted, serverConfigHash, sizeof(serverConfigHash), val, _TRUNCATE);
        }
        else if (wcsncmp(key, L"HasAppliedDefaultSettings", 25) == 0) {
            hasAppliedDefaultSettings = (wcsncmp(val, L"true", 4) == 0);
        }
        else if (wcsncmp(key, L"DefaultWhisperKey", 17) == 0) {
            defaultWhisperKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultNormalKey", 16) == 0) {
            defaultNormalKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultShoutKey", 15) == 0) {
            defaultShoutKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultVoiceToggleKey", 21) == 0) {
            defaultVoiceToggleKey = _wtoi(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceWhisper", 22) == 0) {
            defaultDistanceWhisper = (float)_wtof(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceNormal", 21) == 0) {
            defaultDistanceNormal = (float)_wtof(val);
        }
        else if (wcsncmp(key, L"DefaultDistanceShout", 20) == 0) {
            defaultDistanceShout = (float)_wtof(val);
        }
    }

    fclose(f);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Default settings loaded from config file");
    }
}

// Save default settings to config file ONLY if feature is enabled | Sauvegarder les paramètres par défaut UNIQUEMENT si la fonctionnalité est activée
void saveDefaultSettingsToConfig() {
    // Ne sauvegarder que si la fonctionnalité est activée | Only save if feature is enabled
    if (!enableDefaultSettingsOnFirstConnection) {
        if (enableLogConfig) {
            mumbleAPI.log(ownID, "Default settings NOT saved - feature disabled (enableDefaultSettingsOnFirstConnection=false)");
        }
        return;
    }

    wchar_t* configFolder = getConfigFolderPath();
    if (!configFolder) return;

    wchar_t configFile[MAX_PATH];
    swprintf(configFile, MAX_PATH, L"%s\\default_settings.cfg", configFolder);

    FILE* f = _wfopen(configFile, L"w");
    if (!f) return;

    fwprintf(f, L"# Default Settings Configuration | Configuration des paramètres par défaut\n");
    fwprintf(f, L"# This file tracks server configuration hash and default settings applied\n\n");

    fwprintf(f, L"ServerConfigHash=%S\n", serverConfigHash);
    fwprintf(f, L"HasAppliedDefaultSettings=%s\n", hasAppliedDefaultSettings ? L"true" : L"false");
    fwprintf(f, L"\n");

    fwprintf(f, L"# Default suggested keys | Touches par défaut suggérées\n");
    fwprintf(f, L"DefaultWhisperKey=%d\n", defaultWhisperKey);
    fwprintf(f, L"DefaultNormalKey=%d\n", defaultNormalKey);
    fwprintf(f, L"DefaultShoutKey=%d\n", defaultShoutKey);
    fwprintf(f, L"DefaultVoiceToggleKey=%d\n", defaultVoiceToggleKey);
    fwprintf(f, L"\n");

    fwprintf(f, L"# Default suggested distances (meters) | Distances par défaut suggérées (mètres)\n");
    fwprintf(f, L"DefaultDistanceWhisper=%.1f\n", defaultDistanceWhisper);
    fwprintf(f, L"DefaultDistanceNormal=%.1f\n", defaultDistanceNormal);
    fwprintf(f, L"DefaultDistanceShout=%.1f\n", defaultDistanceShout);

    fclose(f);

    if (enableLogConfig) {
        mumbleAPI.log(ownID, "Default settings saved to config file");
    }
}

// Main window procedure | Procédure de la fenêtre principale
LRESULT CALLBACK ConfigDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND control;

    switch (msg) {
    case WM_CREATE:
        g_configDialogDestroying = FALSE;
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

        hBackgroundBitmap = LoadBackgroundFromResource(IDB_BACKGROUND);

        hBackgroundAdvancedBitmap = LoadBackgroundFromResource(IDB_Background_Plugin_Settings);
        if (!hBackgroundAdvancedBitmap && enableLogGeneral) {
            mumbleAPI.log(ownID, "WARNING: Advanced background image (IDB_Background_Plugin_Settings) not loaded");
        }

        hBackgroundPresetsBitmap = LoadBackgroundFromResource(IDB_Background_Voice_Presets);

        // Disable double buffering to reduce flickering | Désactiver le double buffering pour réduire les scintillements
        SetWindowLongPtr(hwnd, GWL_EXSTYLE,
            GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_COMPOSITED);

        // Get real dimensions of IDB_Main_Button_01 image | Récupérer les dimensions réelles de l'image IDB_Main_Button_01
        HMODULE hModuleBtn = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ConfigDialogProc, &hModuleBtn);

        HBITMAP hTempBitmap = (HBITMAP)LoadImageW(hModuleBtn, MAKEINTRESOURCEW(IDB_Main_Button_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int imageWidth = 170;  // Valeur par défaut au cas où l'image ne charge pas
        int imageHeight = 40;  // Valeur par défaut

        if (hTempBitmap) {
            BITMAP bm;
            GetObject(hTempBitmap, sizeof(BITMAP), &bm);
            imageWidth = bm.bmWidth;   // ✅ Largeur RÉELLE de l'image
            imageHeight = bm.bmHeight; // ✅ Hauteur RÉELLE de l'image
            DeleteObject(hTempBitmap);

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

        // ✅ CORRECTION CRITIQUE : Lire le fichier de config CORRECTEMENT
        wchar_t* configFolder = getConfigFolderPath();
        if (configFolder) {
            wchar_t configFile[MAX_PATH];
            swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);
            FILE* f = _wfopen(configFile, L"r");
            if (f) {
                wchar_t line[512];
                BOOL foundAutomaticPatchFind = FALSE;
                BOOL automaticPatchFindEnabled = FALSE;

                // ✅ LIRE TOUTES LES LIGNES EN UNE SEULE PASSE
                while (fgetws(line, 512, f)) {
                    wchar_t* p = line;
                    while (*p == L' ' || *p == L'\t') p++;
                    wchar_t* end = p + wcslen(p) - 1;
                    while (end > p && (*end == L'\r' || *end == L'\n' || *end == L' ' || *end == L'\t')) {
                        *end = L'\0';
                        end--;
                    }

                    if (wcsncmp(p, L"AutomaticPatchFind=", 19) == 0) {
                        wchar_t* value = p + 19;
                        while (*value == L' ' || *value == L'\t') value++;
                        foundAutomaticPatchFind = TRUE;
                        automaticPatchFindEnabled = (wcsncmp(value, L"true", 4) == 0 || wcsncmp(value, L"True", 4) == 0);
                    }
                    else if (wcsncmp(p, L"AutomaticSavedPath=", 19) == 0) {
                        wchar_t* pathStart = p + 19;
                        while (*pathStart == L' ' || *pathStart == L'\t') pathStart++;
                        wcscpy_s(automaticPathFromConfig, MAX_PATH, pathStart);
                    }
                    else if (wcsncmp(p, L"SavedPath=", 10) == 0) {
                        wchar_t* pathStart = p + 10;
                        while (*pathStart == L' ' || *pathStart == L'\t') pathStart++;
                        wcscpy_s(savedPathFromConfig, MAX_PATH, pathStart);
                    }
                }
                fclose(f);

                // ✅ DÉCIDER APRÈS AVOIR TOUT LU
                if (automaticPatchFindEnabled && wcslen(automaticPathFromConfig) > 0) {
                    // Mode automatique : afficher le chemin automatique
                    wcscpy_s(displayedPathText, MAX_PATH, automaticPathFromConfig);
                    wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                    if (conanSandbox) {
                        *conanSandbox = L'\0';
                    }
                }
                else if (wcslen(savedPathFromConfig) > 0) {
                    // Mode manuel : afficher le chemin manuel
                    wcscpy_s(displayedPathText, MAX_PATH, savedPathFromConfig);
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
            else {
                // Fichier de config n'existe pas
                wcscpy_s(displayedPathText, MAX_PATH, L"(Not configured)");
            }
        }
        else {
            // Impossible d'obtenir le dossier de config
            wcscpy_s(displayedPathText, MAX_PATH, L"(Not configured)");
        }

        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ConfigDialogProc, &hModule);


        hPathBoxBitmap = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Path_Box),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int pathBoxWidth = 380;
        int pathBoxHeight = 35;
        if (hPathBoxBitmap) {
            BITMAP bmPath;
            GetObject(hPathBoxBitmap, sizeof(BITMAP), &bmPath);
            pathBoxWidth = bmPath.bmWidth;
            pathBoxHeight = bmPath.bmHeight;
            SendMessage(hSavedPathBg, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hPathBoxBitmap);
        }

        HBITMAP hBrowseTemp = (HBITMAP)LoadImageW(hModule, MAKEINTRESOURCEW(IDB_Browse_Button),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int browseBtnWidth = 100;
        int browseBtnHeight = 35;
        if (hBrowseTemp) {
            BITMAP bmBrowse;
            GetObject(hBrowseTemp, sizeof(BITMAP), &bmBrowse);
            browseBtnWidth = bmBrowse.bmWidth;
            browseBtnHeight = bmBrowse.bmHeight;
            DeleteObject(hBrowseTemp);
        }

        int pathX = 40;
        int pathY = 270;
        int browseGap = 10;
        int browseX = pathX + pathBoxWidth + browseGap;
        int browseY = pathY + ((pathBoxHeight - browseBtnHeight) / 2);

        // Load checkmark icon from resources | Charger l'icône checkmark depuis les ressources
        HMODULE hModuleCheckmark = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ConfigDialogProc, &hModuleCheckmark);

        HICON hCheckIcon = (HICON)LoadImageW(hModuleCheckmark, MAKEINTRESOURCEW(IDI_CHECKMARK),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

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
            WS_CHILD | SS_LEFT | SS_NOTIFY | WS_DISABLED,  // ✅ ENLEVER WS_VISIBLE
            80, 222, 400, 20,
            hwnd, (HMENU)2004, NULL, NULL);
        ApplyFontToControl(hAutomaticPatchFindLabel, hFont);

        SetWindowSubclass(hAutomaticPatchFindLabel, CheckboxLabelProc, 200, (DWORD_PTR)hAutomaticPatchFindCheck);

        // Set checkbox state from config | Définir l'état de la checkbox depuis la config
        // Default to TRUE if not yet configured | Par défaut TRUE si non configuré
        CheckDlgButton(hwnd, 200, enableAutomaticPatchFind ? BST_CHECKED : BST_UNCHECKED);
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
            85, 147, 295, 20, hwnd, (HMENU)2001, NULL, NULL);
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
            85, 167, 375, 20, hwnd, (HMENU)2002, NULL, NULL);
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
            85, 187, 375, 20, hwnd, (HMENU)2003, NULL, NULL);
        ApplyFontToControl(hVoiceToggleLabel, hFont);

        SetWindowSubclass(hVoiceToggleLabel, CheckboxLabelProc, 204, (DWORD_PTR)hEnableVoiceToggleCheck);

        // Get real dimensions of IDB_Key_Box_01 | Récupérer les dimensions réelles de IDB_Key_Box_01
        HMODULE hModuleKey = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ConfigDialogProc, &hModuleKey);

        HBITMAP hKeyBoxTemp = (HBITMAP)LoadImageW(hModuleKey, MAKEINTRESOURCEW(IDB_Key_Box_01),
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

        int keyBoxWidth = 110;  // Valeur par défaut
        int keyBoxHeight = 30;  // Valeur par défaut

        if (hKeyBoxTemp) {
            BITMAP bmKey;
            GetObject(hKeyBoxTemp, sizeof(BITMAP), &bmKey);
            keyBoxWidth = bmKey.bmWidth;
            keyBoxHeight = bmKey.bmHeight;
            DeleteObject(hKeyBoxTemp);

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

        // ✅ 8) CHARGER LES VALEURS (une seule fois) — depuis g_config (single reader)
        plugin_ui_sync_from_config();

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

        SetWindowTextA(hWhisperKeyEdit, getKeyName(whisperKey));
        SetWindowTextA(hNormalKeyEdit, getKeyName(normalKey));
        SetWindowTextA(hShoutKeyEdit, getKeyName(shoutKey));
        SetWindowTextA(hConfigKeyEdit, getKeyName(configUIKey));
        SetWindowTextA(hVoiceToggleKeyEdit, getKeyName(voiceToggleKey));
        CheckDlgButton(hwnd, 201, enableDistanceMuting ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 203, enableAutomaticChannelChange ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 204, enableVoiceToggle ? BST_CHECKED : BST_UNCHECKED);

        // CORRECTION: Utiliser les valeurs CHARGÉES depuis le fichier de configuration
        // au lieu des valeurs par défaut
        wchar_t whisperText[32], normalText[32], shoutText[32];
        swprintf(whisperText, 32, L"%.1f", distanceWhisper);
        swprintf(normalText, 32, L"%.1f", distanceNormal);
        swprintf(shoutText, 32, L"%.1f", distanceShout);

        SetWindowTextW(hDistanceWhisperEdit, whisperText);
        SetWindowTextW(hDistanceNormalEdit, normalText);
        SetWindowTextW(hDistanceShoutEdit, shoutText);

        if (enableLogConfig) {
            char debugMsg[256];
            snprintf(debugMsg, sizeof(debugMsg),
                "WM_CREATE: Distances set in fields - Whisper: %.1f, Normal: %.1f, Shout: %.1f",
                distanceWhisper, distanceNormal, distanceShout);
            mumbleAPI.log(ownID, debugMsg);
        }

        // Create preset category controls | Créer les contrôles de la catégorie presets
        createPresetsCategory();

        // Load presets from config | Charger les presets depuis la configuration
        loadPresetsFromConfigFile();

        ShowCategoryControls(1);

        break;

    case WM_ERASEBKGND: {
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

    case WM_CTLCOLORSTATIC: {
        // During teardown, fall through to DefWindowProc — brush may already be freed.
        // Pendant la fermeture, laisser DefWindowProc gérer — le pinceau peut être libéré.
        if (g_configDialogDestroying) {
            break;
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

    case WM_CTLCOLORLISTBOX: {
        // Dropdown list popup for HUD theme combo (white text on dark gray).
        // Liste déroulante du combo thème HUD (texte blanc sur gris foncé).
        if (g_configDialogDestroying) {
            break;
        }
        HWND hwndList = (HWND)lParam;
        if (hwndList && ui_is_hud_styled_combo_child(hwndList)) {
            HDC hdcList = (HDC)wParam;
            SetBkMode(hdcList, OPAQUE);
            SetTextColor(hdcList, RGB(255, 255, 255));
            SetBkColor(hdcList, RGB(45, 45, 50));
            return (LRESULT)ui_hud_combo_brush();
        }
        break;
    }

    case WM_CTLCOLOREDIT: {
        // Some Windows versions route combo display through WM_CTLCOLOREDIT.
        // Certaines versions de Windows passent par WM_CTLCOLOREDIT pour l'affichage du combo.
        if (g_configDialogDestroying) {
            break;
        }
        HWND hwndEdit = (HWND)lParam;
        if (hwndEdit && ui_is_hud_styled_combo_child(hwndEdit)) {
            HDC hdcEdit = (HDC)wParam;
            SetBkMode(hdcEdit, OPAQUE);
            SetTextColor(hdcEdit, RGB(255, 255, 255));
            SetBkColor(hdcEdit, RGB(45, 45, 50));
            return (LRESULT)ui_hud_combo_brush();
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 301: ShowCategoryControls(1); break;
        case 302:
            ShowCategoryControls(2);
            ui_sync_hud_theme_combo();
            updateConsolidatedDistanceMessages();
            break;
        case 303: ShowCategoryControls(3); break;

        case 105:
            if (enableAutomaticPatchFind) {
                // Automatic patch find is enabled | Automatic patch find est activé
                MessageBoxW(hwnd,
                    L"Automatic Patch Find is currently enabled.\n\n"
                    L"To use manual mode and browse for a custom patch location:\n"
                    L"1. Uncheck 'Automatic Patch Find' in the Patch Configuration tab\n"
                    L"2. Click 'Save Configuration'\n"
                    L"3. Then use Browse to select your custom patch location",
                    L"Manual Mode Disabled", MB_OK | MB_ICONWARNING);
            }
            else {
                browseSavedPath(hwnd);
            }
            break;

        case 101:
            isCapturingKey = TRUE; captureKeyTarget = 1;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hWhisperKeyEdit, "Press key..."); break;

        case 102:
            isCapturingKey = TRUE; captureKeyTarget = 2;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hNormalKeyEdit, "Press key..."); break;

        case 103:
            isCapturingKey = TRUE; captureKeyTarget = 3;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hShoutKeyEdit, "Press key..."); break;

        case 104:
            isCapturingKey = TRUE; captureKeyTarget = 4;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            SetWindowTextA(hConfigKeyEdit, "Press key..."); break;

        case 106:
            isCapturingKey = TRUE; captureKeyTarget = 5;
            EnableWindow(hWhisperButton, FALSE); EnableWindow(hNormalButton, FALSE);
            EnableWindow(hShoutButton, FALSE); EnableWindow(hConfigButton, FALSE);
            EnableWindow(hVoiceToggleButton, FALSE);
            SetWindowTextA(hVoiceToggleKeyEdit, "Press key..."); break;

        case 201: // Distance Muting checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                if (hubForceDistanceBasedMuting) {
                    // Serveur force -> garder coché
                    CheckDlgButton(hwnd, 201, BST_CHECKED);
                    enableDistanceMuting = TRUE;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
                    updateDynamicInterface();
                }
            }
            break;

        case 203: // Automatic Channel Change checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                if (hubForceAutomaticChannelSwitching) {
                    // Serveur force -> garder coché
                    CheckDlgButton(hwnd, 203, BST_CHECKED);
                    enableAutomaticChannelChange = TRUE;
                    showStatusMessage(L"Cannot disable: enforced by server", TRUE);
                    MessageBeep(MB_ICONWARNING);
                }
                else {
                    // Toggle normal géré par Windows
                    enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
                    updateDynamicInterface();
                }
            }
            break;

        case 215: // HUD theme combo — live overlay preview | combo thème HUD — aperçu live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_theme_from_combo();
                voice_overlay_refresh_theme();
            }
            break;

        case 216: // HUD position combo — live overlay reposition | combo position HUD — reposition live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_position_from_combo();
                voice_overlay_refresh_position();
            }
            break;

        case 217: // HUD size combo — live overlay resize | combo taille HUD — redimensionnement live
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                ui_read_hud_size_from_combo();
                voice_overlay_refresh_size();
            }
            break;

        case 204: // Voice Toggle checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                // Pas de verrou serveur - toggle normal
                enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            }
            break;

        case 200: // Automatic Patch Find checkbox
            if (HIWORD(wParam) == BN_CLICKED) {
                enableAutomaticPatchFind = (IsDlgButtonChecked(hwnd, 200) == BST_CHECKED);

                if (enableAutomaticPatchFind) {
                    // ✅ REDEMANDER le chemin Steam pour être SÛR
                    wchar_t autoPathFull[MAX_PATH] = L"";
                    if (findConanExilesAutomatic(autoPathFull, MAX_PATH)) {
                        // ✅ Afficher SANS \ConanSandbox\Saved
                        wcscpy_s(displayedPathText, MAX_PATH, autoPathFull);
                        wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                        if (conanSandbox) {
                            *conanSandbox = L'\0';
                        }

                        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                            InvalidateRect(hSavedPathBg, NULL, TRUE);
                            UpdateWindow(hSavedPathBg);
                        }

                        if (enableLogConfig) {
                            char logMsg[512];
                            size_t converted = 0;
                            char pathUtf8[MAX_PATH];
                            wcstombs_s(&converted, pathUtf8, MAX_PATH, autoPathFull, _TRUNCATE);
                            snprintf(logMsg, sizeof(logMsg),
                                "✅ AUTOMATIC PATH DETECTED: %s", pathUtf8);
                            mumbleAPI.log(ownID, logMsg);
                        }

                        showStatusMessage(L"Automatic path found - Click Save to apply", FALSE);
                    }
                    else {
                        wcscpy_s(displayedPathText, MAX_PATH, L"(Not found)");
                        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                            InvalidateRect(hSavedPathBg, NULL, TRUE);
                            UpdateWindow(hSavedPathBg);
                        }
                        showStatusMessage(L"Could not find Conan Exiles - Check Steam installation", TRUE);
                    }
                }
                else {
                    // MODE MANUEL : Charger le chemin MANUEL depuis la config
                    wchar_t* configFolder = getConfigFolderPath();
                    if (configFolder) {
                        wchar_t configFile[MAX_PATH];
                        swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);
                        FILE* fRead = _wfopen(configFile, L"r");
                        if (fRead) {
                            wchar_t line[512];
                            while (fgetws(line, 512, fRead)) {
                                if (wcsncmp(line, L"SavedPath=", 10) == 0) {
                                    wchar_t* pathStart = line + 10;
                                    wchar_t* nl = wcschr(pathStart, L'\n');
                                    if (nl) *nl = L'\0';
                                    wchar_t* cr = wcschr(pathStart, L'\r');
                                    if (cr) *cr = L'\0';

                                    wcscpy_s(displayedPathText, MAX_PATH, pathStart);
                                    wchar_t* conanSandbox = wcsstr(displayedPathText, L"\\ConanSandbox\\Saved");
                                    if (conanSandbox) {
                                        *conanSandbox = L'\0';
                                    }
                                    break;
                                }
                            }
                            fclose(fRead);
                        }
                    }

                    if (wcslen(displayedPathText) == 0) {
                        wcscpy_s(displayedPathText, MAX_PATH, L"No path configured");
                    }

                    if (hSavedPathBg && IsWindow(hSavedPathBg)) {
                        InvalidateRect(hSavedPathBg, NULL, TRUE);
                        UpdateWindow(hSavedPathBg);
                    }
                    showStatusMessage(L"Manual path displayed", FALSE);
                }
            }
            break;

        case 1: { // Save Configuration
            if (currentCategory == 1) {
                // === CATÉGORIE 1 : PATCH CONFIGURATION ===

                // ✅ CORRECTION : Construire le chemin COMPLET incluant ConanSandbox\Saved
                wchar_t pathToSave[MAX_PATH] = L"";

                if (enableAutomaticPatchFind) {
                    // ✅ REDEMANDER le chemin Steam RÉEL pour être 100% SÛR
                    wchar_t realSteamPath[MAX_PATH] = L"";
                    if (!findConanExilesAutomatic(realSteamPath, MAX_PATH)) {
                        showStatusMessage(L"⚠ Error: Could not find Conan Exiles automatically", TRUE);
                        break;
                    }

                    // ✅ Utiliser le VRAI chemin Steam (pas displayedPathText)
                    wcscpy_s(pathToSave, MAX_PATH, realSteamPath);

        // ✅ pathToSave contient maintenant le VRAI chemin Steam complet
        if (enableLogConfig) {
            char logMsg[512];
            size_t converted = 0;
            char pathUtf8[MAX_PATH];
            wcstombs_s(&converted, pathUtf8, MAX_PATH, pathToSave, _TRUNCATE);
            snprintf(logMsg, sizeof(logMsg),
                "✅ AUTOMATIC MODE: Using REAL Steam path: %s", pathUtf8);
            mumbleAPI.log(ownID, logMsg);
        }
    }
    else {
        // Mode manuel : utiliser displayedPathText
        if (wcslen(displayedPathText) == 0) {
            MessageBoxW(hwnd,
                L"Please select your Conan Exiles game folder using the Browse button.",
                L"Missing Path", MB_OK | MB_ICONWARNING);

            showStatusMessage(L"⚠ Error: No game path specified", TRUE);
            break;
        }

        // Construire le chemin complet (displayedPathText + \ConanSandbox\Saved)
        wcscpy_s(pathToSave, MAX_PATH, displayedPathText);
        wcscat_s(pathToSave, MAX_PATH, L"\\ConanSandbox\\Saved");

        if (enableLogConfig) {
            char logMsg[512];
            size_t converted = 0;
            char pathUtf8[MAX_PATH];
            wcstombs_s(&converted, pathUtf8, MAX_PATH, pathToSave, _TRUNCATE);
            snprintf(logMsg, sizeof(logMsg),
                "✅ MANUAL MODE: Using manual path: %s", pathUtf8);
            mumbleAPI.log(ownID, logMsg);
        }
    }

    // ✅ 2) Vérifier UNIQUEMENT que le dossier ConanSandbox\Saved existe
    DWORD savedAttribs = GetFileAttributesW(pathToSave);
    if (savedAttribs == INVALID_FILE_ATTRIBUTES || !(savedAttribs & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t errorMsg[512];
        swprintf(errorMsg, 512,
            L"The folder 'ConanSandbox\\Saved' does not exist in:\n%s\n\n"
            L"Please verify:\n"
            L"1. This is your Conan Exiles game folder\n",
            pathToSave);

        MessageBoxW(hwnd, errorMsg, L"Folder Not Found", MB_OK | MB_ICONERROR);
        showStatusMessage(L"⚠ Error: ConanSandbox\\Saved folder not found", TRUE);
        break;
    }

    // ✅ 3) Toutes les vérifications passées → SAUVEGARDER
    wchar_t distWhisper[32], distNormal[32], distShout[32];
    swprintf(distWhisper, 32, L"%.1f", distanceWhisper);
    swprintf(distNormal, 32, L"%.1f", distanceNormal);
    swprintf(distShout, 32, L"%.1f", distanceShout);

    // Extraire le dossier du jeu (sans ConanSandbox\Saved) pour writeFullConfiguration
    wchar_t gameFolder[MAX_PATH];
    wcscpy_s(gameFolder, MAX_PATH, pathToSave);
    wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
    if (conanSandbox) {
        *conanSandbox = L'\0';
    }

    BOOL wasAlreadySaved = isPatchAlreadySaved();

    writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

    // ✅ MISE À JOUR IMMÉDIATE DE L'AFFICHAGE APRÈS SAUVEGARDE
    if (enableAutomaticPatchFind) {
        // Afficher le chemin Steam dans l'interface
        wcscpy_s(displayedPathText, MAX_PATH, gameFolder);
        if (hSavedPathBg && IsWindow(hSavedPathBg)) {
            InvalidateRect(hSavedPathBg, NULL, TRUE);
            UpdateWindow(hSavedPathBg);
        }
    }

    if (!wasAlreadySaved) {
        showConfigSavedNotice(hwnd, L"Patch configuration saved successfully!");
    }
    else {
        showConfigSavedNotice(hwnd, L"Patch configuration updated successfully!");
    }

    if (enableLogConfig) {
        char logMsg[512];
        size_t converted = 0;
        char savedPathUtf8[MAX_PATH];
        wcstombs_s(&converted, savedPathUtf8, MAX_PATH, pathToSave, _TRUNCATE);

        snprintf(logMsg, sizeof(logMsg),
            "✅ SECURITY PASSED: Saved folder verified at: %s",
            savedPathUtf8);
        mumbleAPI.log(ownID, logMsg);
    }
}
           else if (currentCategory == 2) {
               // === CATÉGORIE 2 : ADVANCED OPTIONS (reste inchangé) ===
               enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
               enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
               enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);

               wchar_t distWhisper[32], distNormal[32], distShout[32];
               GetWindowTextW(hDistanceWhisperEdit, distWhisper, 32);
               GetWindowTextW(hDistanceNormalEdit, distNormal, 32);
               GetWindowTextW(hDistanceShoutEdit, distShout, 32);

               distanceWhisper = (float)_wtof(distWhisper);
               distanceNormal = (float)_wtof(distNormal);
               distanceShout = (float)_wtof(distShout);
               ui_read_hud_theme_from_combo();
               ui_read_hud_position_from_combo();
               ui_read_hud_size_from_combo();

               wchar_t gameFolder[MAX_PATH] = L"";

               wchar_t* configFolder = getConfigFolderPath();
               if (configFolder) {
                   wchar_t configFile[MAX_PATH];
                   swprintf(configFile, MAX_PATH, L"%s\\plugin.cfg", configFolder);
                   FILE* f = _wfopen(configFile, L"r");
                   if (f) {
                       wchar_t line[512];
                       while (fgetws(line, 512, f)) {
                           if (wcsncmp(line, L"SavedPath=", 10) == 0) {
                               wchar_t* pathStart = line + 10;
                               wchar_t* nl = wcschr(pathStart, L'\n');
                               if (nl) *nl = L'\0';
                               wchar_t* cr = wcschr(pathStart, L'\r');
                               if (cr) *cr = L'\0';

                               wcscpy_s(gameFolder, MAX_PATH, pathStart);
                               wchar_t* conanSandbox = wcsstr(gameFolder, L"\\ConanSandbox\\Saved");
                               if (conanSandbox) {
                                   *conanSandbox = L'\0';
                               }
                               break;
                           }
                       }
                       fclose(f);
                   }
               }

               writeFullConfiguration(gameFolder, distWhisper, distNormal, distShout);

               float currentVoiceDistance = localVoiceData.voiceDistance;
               if (fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceNormal) &&
                   fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceShout)) {
                   localVoiceData.voiceDistance = distanceWhisper;
               }
               else if (fabsf(currentVoiceDistance - distanceShout) < fabsf(currentVoiceDistance - distanceNormal)) {
                   localVoiceData.voiceDistance = distanceShout;
               }
               else {
                   localVoiceData.voiceDistance = distanceNormal;
               }

               applyDistanceToAllPlayers();

               showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

               if (enableLogConfig) {
                   char logMsg[512];
                   snprintf(logMsg, sizeof(logMsg),
                       "✅ ADVANCED OPTIONS SAVED: WhisperKey=%d NormalKey=%d ShoutKey=%d ConfigKey=%d VoiceToggleKey=%d Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                       whisperKey, normalKey, shoutKey, configUIKey, voiceToggleKey,
                       distanceWhisper, distanceNormal, distanceShout,
                       enableDistanceMuting ? "true" : "false",
                       enableAutomaticChannelChange ? "true" : "false",
                       enableVoiceToggle ? "true" : "false");
                   mumbleAPI.log(ownID, logMsg);
               }
           }
           break;
       }

        case 11: { // Save Voice Range (Advanced Options)
            showPresetSaveDialog();
            break;
        }

        case 12: { // Save Configuration (Advanced Options - save ALL to plugin.cfg)
            // Save current voice mode before modifying | Sauvegarder le mode vocal actuel
            float currentVoiceDistance = localVoiceData.voiceDistance;

            // Get values from interface | Récupérer les valeurs de l'interface
            enableDistanceMuting = (IsDlgButtonChecked(hwnd, 201) == BST_CHECKED);
            enableAutomaticChannelChange = (IsDlgButtonChecked(hwnd, 203) == BST_CHECKED);
            enableVoiceToggle = (IsDlgButtonChecked(hwnd, 204) == BST_CHECKED);
            ui_read_hud_theme_from_combo();
            ui_read_hud_position_from_combo();
            ui_read_hud_size_from_combo();

            wchar_t distWhisper[32], distNormal[32], distShout[32];
            GetWindowTextW(hDistanceWhisperEdit, distWhisper, 32);
            GetWindowTextW(hDistanceNormalEdit, distNormal, 32);
            GetWindowTextW(hDistanceShoutEdit, distShout, 32);

            // Convert distances | Convertir les distances
            float whisperValue = (float)_wtof(distWhisper);
            float normalValue = (float)_wtof(distNormal);
            float shoutValue = (float)_wtof(distShout);

            // Update global distances | Mettre à jour les distances globales
            distanceWhisper = whisperValue;
            distanceNormal = normalValue;
            distanceShout = shoutValue;

            // Save everything using saveVoiceSettings() | Tout sauvegarder avec saveVoiceSettings()
            saveVoiceSettings();
            voice_overlay_refresh_position();
            voice_overlay_refresh_size();

            // Restore voice mode | Restaurer le mode vocal
            if (fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceNormal) &&
                fabsf(currentVoiceDistance - distanceWhisper) < fabsf(currentVoiceDistance - distanceShout)) {
                localVoiceData.voiceDistance = distanceWhisper;
            }
            else if (fabsf(currentVoiceDistance - distanceShout) < fabsf(currentVoiceDistance - distanceNormal)) {
                localVoiceData.voiceDistance = distanceShout;
            }
            else {
                localVoiceData.voiceDistance = distanceNormal;
            }

            // Apply changes | Appliquer les changements
            applyDistanceToAllPlayers();

            showConfigSavedNotice(hwnd, L"Advanced options saved successfully!");

            if (enableLogConfig) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg),
                    "Advanced options saved: Whisper=%.1f Normal=%.1f Shout=%.1f Muting=%s AutoChannel=%s VoiceToggle=%s",
                    distanceWhisper, distanceNormal, distanceShout,
                    enableDistanceMuting ? "true" : "false",
                    enableAutomaticChannelChange ? "true" : "false",
                    enableVoiceToggle ? "true" : "false");
                mumbleAPI.log(ownID, logMsg);
            }
            break;
        }

        case 2: DestroyWindow(hwnd); break;

            // CORRECTION CRITIQUE: Gérer les boutons LOAD et RENAME **EN DEHORS** de EN_CHANGE
        default:
            // Handle preset load buttons | Gérer boutons load
            if (LOWORD(wParam) >= 900 && LOWORD(wParam) < 900 + MAX_VOICE_PRESETS) {
                int presetIndex = LOWORD(wParam) - 900;
                loadVoicePreset(presetIndex);
                break;
            }
            // Handle preset rename buttons | Gérer boutons rename
            else if (LOWORD(wParam) >= 950 && LOWORD(wParam) < 950 + MAX_VOICE_PRESETS) {
                int presetIndex = LOWORD(wParam) - 950;
                renamePresetIndex = presetIndex;

                if (!hPresetRenameDialog || !IsWindow(hPresetRenameDialog)) {
                    const wchar_t RENAME_DIALOG_CLASS[] = L"PresetRenameDialogClass";
                    WNDCLASSW wc = { 0 };
                    wc.lpfnWndProc = PresetRenameDialogProc;
                    wc.hInstance = GetModuleHandleW(NULL);
                    wc.lpszClassName = RENAME_DIALOG_CLASS;
                    wc.hbrBackground = CreateSolidBrush(RGB(248, 249, 250));
                    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

                    UnregisterClassW(RENAME_DIALOG_CLASS, wc.hInstance);
                    RegisterClassW(&wc);

                    // ✅ CORRECTION : Centrer au-dessus de l'interface principale
                    int dialogWidth = 300;
                    int dialogHeight = 240;
                    int dialogX, dialogY;

                    if (hwnd && IsWindow(hwnd)) {
                        // Get parent window position and size | Obtenir position et taille de la fenêtre parente
                        RECT parentRect;
                        GetWindowRect(hwnd, &parentRect);

                        int parentWidth = parentRect.right - parentRect.left;
                        int parentHeight = parentRect.bottom - parentRect.top;
                        int parentX = parentRect.left;
                        int parentY = parentRect.top;
                        dialogX = parentX + (parentWidth - dialogWidth) / 2;
                        dialogY = parentY + (parentHeight - dialogHeight) / 2;

                        if (enableLogGeneral) {
                            char logMsg[256];
                            snprintf(logMsg, sizeof(logMsg),
                                "Rename dialog: Parent at (%d,%d), Dialog at (%d,%d)",
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

                    hPresetRenameDialog = CreateWindowExW(
                        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                        RENAME_DIALOG_CLASS,
                        L"Rename Preset",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        dialogX, dialogY, dialogWidth, dialogHeight,
                        hwnd, NULL, wc.hInstance, NULL);

                    if (hPresetRenameDialog) {
                        SetLayeredWindowAttributes(hPresetRenameDialog, 0, 250, LWA_ALPHA);
                        ShowWindow(hPresetRenameDialog, SW_SHOW);
                        UpdateWindow(hPresetRenameDialog);
                    }
                }
                break;
            }
            // Handle distance field changes | Gestion des changements dans les champs de distance
            else if (HIWORD(wParam) == EN_CHANGE) {
                HWND hEditControl = (HWND)lParam;
                if (hEditControl == hDistanceWhisperEdit) {
                    handleDistanceEditChange(1);
                }
                else if (hEditControl == hDistanceNormalEdit) {
                    handleDistanceEditChange(2);
                }
                else if (hEditControl == hDistanceShoutEdit) {
                    handleDistanceEditChange(3);
                }
            }
            break;
        }
        break;

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

    case WM_DRAWITEM: {
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
        break;
    }

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


// ============================================================================
