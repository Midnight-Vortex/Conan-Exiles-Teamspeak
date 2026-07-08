#include "ui/dialogs/ui_settings.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"
#include "core/voice/voice_modes.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"

#include <stdio.h>
#include <string.h>

#define DLG_CLASS_NAME L"CESettingsClass"
#define DLG_W 460
#define DLG_H 470

/* Control IDs. */
#define IDC_PATH            1001
#define IDC_AUTOPATH        1002
#define IDC_DIST_WHISPER    1010
#define IDC_DIST_NORMAL     1011
#define IDC_DIST_SHOUT      1012
#define IDC_KEY_WHISPER     1020
#define IDC_KEY_NORMAL      1021
#define IDC_KEY_SHOUT       1022
#define IDC_KEY_TOGGLE      1023
#define IDC_KEY_CONFIG      1024
#define IDC_CHK_MUTING      1030
#define IDC_CHK_AUTOCHAN    1031
#define IDC_CHK_TOGGLE      1032
#define IDC_CHK_OVERLAY     1033
#define IDC_CHK_DEBUG       1034
#define IDC_CMB_THEME       1040
#define IDC_CMB_POSITION    1041
#define IDC_CMB_SIZE        1042
#define IDC_BTN_SAVE        1050
#define IDC_BTN_CLOSE       1051
#define IDC_STATUS          1052

#define UI_KEY_SUPPRESS_MS 300

/* UI thread only. */
static HWND g_dlg = NULL;
static HFONT g_dlgFont = NULL;
static char g_cfgKeyArmed = 1;
static ULONGLONG g_cfgKeySuppressUntil = 0;

/* Staging config published by "Speichern", consumed on the callback thread. */
static CRITICAL_SECTION g_pendingLock;
static INIT_ONCE g_pendingLockOnce = INIT_ONCE_STATIC_INIT;
static volatile long g_pendingValid = 0;
static PluginConfig g_pending;

static BOOL CALLBACK pending_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_pendingLock);
    return TRUE;
}

static void pending_lock_ensure(void) {
    InitOnceExecuteOnce(&g_pendingLockOnce, pending_lock_init_once, NULL, NULL);
}

/* ---- helpers (UI thread) ------------------------------------------------------ */

static HWND dlg_add(const wchar_t* cls, const wchar_t* text, DWORD style,
    int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, g_dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (ctl && g_dlgFont) {
        SendMessageW(ctl, WM_SETFONT, (WPARAM)g_dlgFont, TRUE);
    }
    return ctl;
}

static void dlg_set_float(int id, float value) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", value);
    SetWindowTextW(GetDlgItem(g_dlg, id), buf);
}

static void dlg_set_int(int id, int value) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", value);
    SetWindowTextW(GetDlgItem(g_dlg, id), buf);
}

static float dlg_get_float(int id, float fallback) {
    wchar_t buf[64] = L"";
    GetWindowTextW(GetDlgItem(g_dlg, id), buf, 64);
    if (!buf[0]) {
        return fallback;
    }
    /* Accept comma decimals (DE locale). */
    for (wchar_t* p = buf; *p; p++) {
        if (*p == L',') *p = L'.';
    }
    return (float)_wtof(buf);
}

static int dlg_get_int(int id, int fallback) {
    wchar_t buf[64] = L"";
    GetWindowTextW(GetDlgItem(g_dlg, id), buf, 64);
    return buf[0] ? _wtoi(buf) : fallback;
}

static int dlg_get_check(int id) {
    return SendMessageW(GetDlgItem(g_dlg, id), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}

static void dlg_set_check(int id, int checked) {
    SendMessageW(GetDlgItem(g_dlg, id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ---- load current config into the controls -------------------------------------- */

static void dlg_load_values(void) {
    PluginConfig cfg;
    config_copy(&cfg);

    SetWindowTextW(GetDlgItem(g_dlg, IDC_PATH), cfg.savedPath);
    dlg_set_check(IDC_AUTOPATH, cfg.automaticPatchFind);

    dlg_set_float(IDC_DIST_WHISPER, cfg.distanceWhisper);
    dlg_set_float(IDC_DIST_NORMAL, cfg.distanceNormal);
    dlg_set_float(IDC_DIST_SHOUT, cfg.distanceShout);

    dlg_set_int(IDC_KEY_WHISPER, cfg.whisperKey);
    dlg_set_int(IDC_KEY_NORMAL, cfg.normalKey);
    dlg_set_int(IDC_KEY_SHOUT, cfg.shoutKey);
    dlg_set_int(IDC_KEY_TOGGLE, cfg.voiceToggleKey);
    dlg_set_int(IDC_KEY_CONFIG, cfg.configUIKey);

    dlg_set_check(IDC_CHK_MUTING, cfg.enableDistanceMuting);
    dlg_set_check(IDC_CHK_AUTOCHAN, cfg.enableAutomaticChannelChange);
    dlg_set_check(IDC_CHK_TOGGLE, cfg.enableVoiceToggle);
    dlg_set_check(IDC_CHK_OVERLAY, cfg.enableVoiceOverlay);
    dlg_set_check(IDC_CHK_DEBUG, cfg.debugMode);

    SendMessageW(GetDlgItem(g_dlg, IDC_CMB_THEME), CB_SETCURSEL, cfg.hudTheme, 0);
    SendMessageW(GetDlgItem(g_dlg, IDC_CMB_POSITION), CB_SETCURSEL, cfg.hudPosition, 0);
    SendMessageW(GetDlgItem(g_dlg, IDC_CMB_SIZE), CB_SETCURSEL, cfg.hudSize, 0);
}

/* ---- apply staged or direct config ---------------------------------------------- */

static void settings_apply_core(const PluginConfig* cfg) {
    config_apply(cfg);
    config_save();
    log_set_enabled(g_config.debugMode);
    pos_autodetect_saved_path();
}

static void settings_apply_ts_side(void) {
    if (!ts3_is_connected()) {
        return;
    }
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all_force();
}

static void dlg_save_values(void) {
    PluginConfig cfg;
    config_copy(&cfg); /* preserve fields the dialog does not edit */

    GetWindowTextW(GetDlgItem(g_dlg, IDC_PATH), cfg.savedPath, CONFIG_MAX_PATH);
    cfg.automaticPatchFind = dlg_get_check(IDC_AUTOPATH);

    cfg.distanceWhisper = dlg_get_float(IDC_DIST_WHISPER, cfg.distanceWhisper);
    cfg.distanceNormal = dlg_get_float(IDC_DIST_NORMAL, cfg.distanceNormal);
    cfg.distanceShout = dlg_get_float(IDC_DIST_SHOUT, cfg.distanceShout);

    cfg.whisperKey = dlg_get_int(IDC_KEY_WHISPER, cfg.whisperKey);
    cfg.normalKey = dlg_get_int(IDC_KEY_NORMAL, cfg.normalKey);
    cfg.shoutKey = dlg_get_int(IDC_KEY_SHOUT, cfg.shoutKey);
    cfg.voiceToggleKey = dlg_get_int(IDC_KEY_TOGGLE, cfg.voiceToggleKey);
    cfg.configUIKey = dlg_get_int(IDC_KEY_CONFIG, cfg.configUIKey);

    cfg.enableDistanceMuting = dlg_get_check(IDC_CHK_MUTING);
    cfg.enableAutomaticChannelChange = dlg_get_check(IDC_CHK_AUTOCHAN);
    cfg.enableVoiceToggle = dlg_get_check(IDC_CHK_TOGGLE);
    cfg.enableVoiceOverlay = dlg_get_check(IDC_CHK_OVERLAY);
    cfg.debugMode = dlg_get_check(IDC_CHK_DEBUG);

    LRESULT sel;
    sel = SendMessageW(GetDlgItem(g_dlg, IDC_CMB_THEME), CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) cfg.hudTheme = (int)sel;
    sel = SendMessageW(GetDlgItem(g_dlg, IDC_CMB_POSITION), CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) cfg.hudPosition = (int)sel;
    sel = SendMessageW(GetDlgItem(g_dlg, IDC_CMB_SIZE), CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) cfg.hudSize = (int)sel;

    config_clamp(&cfg);

    if (ts3_is_connected()) {
        pending_lock_ensure();
        EnterCriticalSection(&g_pendingLock);
        g_pending = cfg;
        LeaveCriticalSection(&g_pendingLock);
        InterlockedExchange(&g_pendingValid, 1);
        ts3_request_wakeup();
        SetWindowTextW(GetDlgItem(g_dlg, IDC_STATUS), L"Gespeichert - wird angewendet...");
        log_write("UI: settings saved (pending apply)");
    }
    else {
        settings_apply_core(&cfg);
        SetWindowTextW(GetDlgItem(g_dlg, IDC_STATUS), L"Gespeichert.");
        log_write("UI: settings saved (offline apply)");
    }
}

/* ---- window ------------------------------------------------------------------------ */

static void dlg_create_controls(void) {
    const int labelW = 170, editW = 70, x1 = 15, x2 = 190;
    int y = 12;

    dlg_add(L"STATIC", L"Conan Saved-Pfad (leer = Auto):", 0, x1, y + 3, labelW + 60, 18, 0);
    y += 22;
    dlg_add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x1, y, DLG_W - 45, 22, IDC_PATH);
    y += 28;
    dlg_add(L"BUTTON", L"Pfad automatisch suchen", BS_AUTOCHECKBOX, x1, y, 250, 20, IDC_AUTOPATH);
    y += 30;

    dlg_add(L"STATIC", L"Distanz Fluestern (m):", 0, x1, y + 3, labelW, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x2, y, editW, 22, IDC_DIST_WHISPER);
    dlg_add(L"STATIC", L"Taste (VK):", 0, x2 + editW + 15, y + 3, 80, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_NUMBER, x2 + editW + 95, y, 50, 22, IDC_KEY_WHISPER);
    y += 28;
    dlg_add(L"STATIC", L"Distanz Normal (m):", 0, x1, y + 3, labelW, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x2, y, editW, 22, IDC_DIST_NORMAL);
    dlg_add(L"STATIC", L"Taste (VK):", 0, x2 + editW + 15, y + 3, 80, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_NUMBER, x2 + editW + 95, y, 50, 22, IDC_KEY_NORMAL);
    y += 28;
    dlg_add(L"STATIC", L"Distanz Schreien (m):", 0, x1, y + 3, labelW, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x2, y, editW, 22, IDC_DIST_SHOUT);
    dlg_add(L"STATIC", L"Taste (VK):", 0, x2 + editW + 15, y + 3, 80, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_NUMBER, x2 + editW + 95, y, 50, 22, IDC_KEY_SHOUT);
    y += 34;

    dlg_add(L"STATIC", L"Modus-Wechsel-Taste (VK):", 0, x1, y + 3, labelW, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_NUMBER, x2, y, 50, 22, IDC_KEY_TOGGLE);
    y += 28;
    dlg_add(L"STATIC", L"Einstellungen-Taste (VK):", 0, x1, y + 3, labelW, 18, 0);
    dlg_add(L"EDIT", L"", WS_BORDER | ES_NUMBER, x2, y, 50, 22, IDC_KEY_CONFIG);
    y += 34;

    dlg_add(L"BUTTON", L"Distanz-Stummschaltung", BS_AUTOCHECKBOX, x1, y, 210, 20, IDC_CHK_MUTING);
    dlg_add(L"BUTTON", L"Auto-Kanalwechsel", BS_AUTOCHECKBOX, x2 + 50, y, 180, 20, IDC_CHK_AUTOCHAN);
    y += 26;
    dlg_add(L"BUTTON", L"Modus-Wechsel-Taste aktiv", BS_AUTOCHECKBOX, x1, y, 210, 20, IDC_CHK_TOGGLE);
    dlg_add(L"BUTTON", L"Voice-Overlay (HUD)", BS_AUTOCHECKBOX, x2 + 50, y, 180, 20, IDC_CHK_OVERLAY);
    y += 26;
    dlg_add(L"BUTTON", L"Debug-Log", BS_AUTOCHECKBOX, x1, y, 210, 20, IDC_CHK_DEBUG);
    y += 34;

    dlg_add(L"STATIC", L"HUD-Farbe:", 0, x1, y + 3, 80, 18, 0);
    HWND cmb = dlg_add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x1 + 85, y, 100, 200, IDC_CMB_THEME);
    static const wchar_t* const themes[] = { L"Grau", L"Lila", L"Blau", L"Gruen", L"Bernstein", L"Rot" };
    for (int i = 0; i < 6; i++) {
        SendMessageW(cmb, CB_ADDSTRING, 0, (LPARAM)themes[i]);
    }
    cmb = dlg_add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x1 + 195, y, 120, 200, IDC_CMB_POSITION);
    static const wchar_t* const positions[] = { L"Oben links", L"Oben rechts", L"Unten links", L"Unten rechts", L"Oben mittig" };
    for (int i = 0; i < 5; i++) {
        SendMessageW(cmb, CB_ADDSTRING, 0, (LPARAM)positions[i]);
    }
    cmb = dlg_add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x1 + 325, y, 90, 200, IDC_CMB_SIZE);
    static const wchar_t* const sizes[] = { L"Klein", L"Mittel", L"Gross" };
    for (int i = 0; i < 3; i++) {
        SendMessageW(cmb, CB_ADDSTRING, 0, (LPARAM)sizes[i]);
    }
    y += 40;

    dlg_add(L"STATIC", L"", 0, x1, y, DLG_W - 45, 18, IDC_STATUS);
    y += 26;

    dlg_add(L"BUTTON", L"Speichern", BS_DEFPUSHBUTTON, DLG_W - 220, y, 95, 28, IDC_BTN_SAVE);
    dlg_add(L"BUTTON", L"Schliessen", 0, DLG_W - 115, y, 95, 28, IDC_BTN_CLOSE);
}

static LRESULT CALLBACK dlg_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_SAVE:
            dlg_save_values();
            return 0;
        case IDC_BTN_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); /* keep the window for reuse */
        return 0;
    case WM_DESTROY:
        g_dlg = NULL;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void dlg_ensure_created(void) {
    if (g_dlg && IsWindow(g_dlg)) {
        return;
    }

    if (!g_dlgFont) {
        g_dlgFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dlg_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = DLG_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    UnregisterClassW(DLG_CLASS_NAME, wc.hInstance);
    if (!RegisterClassW(&wc)) {
        log_write("UI: settings RegisterClass failed (%lu)", GetLastError());
        return;
    }

    const int x = (GetSystemMetrics(SM_CXSCREEN) - DLG_W) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - DLG_H) / 2;
    g_dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        DLG_CLASS_NAME, L"Conan Exiles Voice - Einstellungen",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, DLG_W, DLG_H, NULL, NULL, wc.hInstance, NULL);
    if (!g_dlg) {
        log_write("UI: settings CreateWindowEx failed (%lu)", GetLastError());
        return;
    }
    dlg_create_controls();
    log_write("UI: settings dialog created");
}

static void dlg_toggle(void) {
    dlg_ensure_created();
    if (!g_dlg) {
        return;
    }
    if (IsWindowVisible(g_dlg)) {
        ShowWindow(g_dlg, SW_HIDE);
        return;
    }
    dlg_load_values();
    SetWindowTextW(GetDlgItem(g_dlg, IDC_STATUS), L"");
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

static void dlg_run_message_loop(void) {
    MSG msg;
    while (g_dlg && IsWindow(g_dlg) && IsWindowVisible(g_dlg)) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return;
            }
            if (!IsDialogMessageW(g_dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        Sleep(10);
    }
}

void ui_settings_open_blocking(void) {
    static volatile long s_dialogOpen = 0;
    if (InterlockedCompareExchange(&s_dialogOpen, 1, 0) != 0) {
        return;
    }

    dlg_ensure_created();
    if (!g_dlg) {
        InterlockedExchange(&s_dialogOpen, 0);
        return;
    }

    dlg_load_values();
    SetWindowTextW(GetDlgItem(g_dlg, IDC_STATUS), L"");
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
    log_write("UI: settings dialog opened");

    dlg_run_message_loop();

    InterlockedExchange(&s_dialogOpen, 0);
    log_write("UI: settings dialog closed");
}

/* ---- public: UI thread ---------------------------------------------------------- */

void ui_settings_hotkey_poll(void) {
    const int vk = g_config.configUIKey;
    if (vk <= 0 || vk >= 256) {
        return;
    }
    if (GetTickCount64() < g_cfgKeySuppressUntil) {
        return;
    }
    const int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) {
        g_cfgKeyArmed = 1;
        return;
    }
    if (!g_cfgKeyArmed) {
        return;
    }
    g_cfgKeyArmed = 0;
    g_cfgKeySuppressUntil = GetTickCount64() + UI_KEY_SUPPRESS_MS;
    dlg_toggle();
}

int ui_settings_handle_message(MSG* msg) {
    if (g_dlg && IsWindow(g_dlg) && IsWindowVisible(g_dlg)) {
        if (IsDialogMessageW(g_dlg, msg)) {
            return 1;
        }
    }
    return 0;
}

void ui_settings_destroy(void) {
    if (g_dlg && IsWindow(g_dlg)) {
        DestroyWindow(g_dlg);
    }
    g_dlg = NULL;
}

/* ---- public: TS callback thread --------------------------------------------------- */

void ui_settings_flush_apply(void) {
    if (!ts3_thread_is_callback()) {
        return;
    }
    if (!InterlockedCompareExchange(&g_pendingValid, 0, 0)) {
        return;
    }

    PluginConfig staged;
    pending_lock_ensure();
    EnterCriticalSection(&g_pendingLock);
    staged = g_pending;
    LeaveCriticalSection(&g_pendingLock);
    InterlockedExchange(&g_pendingValid, 0);

    settings_apply_core(&staged);
    settings_apply_ts_side();

    log_write("UI: settings applied (whisper=%.1f normal=%.1f shout=%.1f)",
        g_config.distanceWhisper, g_config.distanceNormal, g_config.distanceShout);
}

int ui_settings_has_pending_apply(void) {
    return InterlockedCompareExchange(&g_pendingValid, 0, 0) != 0;
}
