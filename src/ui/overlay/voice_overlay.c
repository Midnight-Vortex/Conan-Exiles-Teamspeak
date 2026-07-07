#include "ui/overlay/voice_overlay.h"
#include "ui/dialogs/ui_settings.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/zone_resolve.h"
#include "core/voice/voice_modes.h"
#include "core/util/log.h"
#include "ts/profile/ts3_server_profile.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

#define OVERLAY_CLASS_NAME   L"CEOverlayClass"
#define OVERLAY_TIMER_ID     1
#define OVERLAY_TIMER_MS     250
#define OVERLAY_REPAINT_MS   1000  /* forced repaint even without changes */

typedef struct HudPalette {
    COLORREF bg, border, mode, modeShadow, zoneDefault;
    BYTE alpha;
} HudPalette;

/* Index = g_config.hudTheme. */
static const HudPalette g_palettes[] = {
    { RGB(0, 0, 0),    RGB(200, 200, 200), RGB(230, 230, 230), RGB(0, 0, 0),   RGB(170, 190, 230), 190 }, /* gray */
    { RGB(22, 8, 38),  RGB(120, 65, 175),  RGB(235, 215, 255), RGB(12, 0, 22), RGB(195, 165, 240), 190 }, /* purple */
    { RGB(8, 18, 38),  RGB(65, 95, 175),   RGB(215, 225, 255), RGB(0, 8, 22),  RGB(165, 185, 240), 190 }, /* blue */
    { RGB(8, 28, 14),  RGB(55, 120, 75),   RGB(215, 245, 220), RGB(0, 18, 8),  RGB(165, 210, 175), 190 }, /* green */
    { RGB(32, 22, 8),  RGB(150, 110, 45),  RGB(255, 235, 200), RGB(18, 12, 0), RGB(220, 190, 140), 190 }, /* amber */
    { RGB(32, 8, 8),   RGB(150, 55, 55),   RGB(255, 215, 215), RGB(18, 0, 0),  RGB(220, 165, 165), 190 }, /* red */
};
#define HUD_THEME_COUNT ((int)(sizeof(g_palettes) / sizeof(g_palettes[0])))

/* Owned by the overlay thread (except where noted). */
static HANDLE g_uiThread = NULL;
static volatile DWORD g_uiThreadId = 0;      /* read from other threads */
static volatile long g_uiThreadStop = 0;
static HWND g_hwnd = NULL;
static HFONT g_modeFont = NULL;
static HFONT g_subFont = NULL;
static int g_fontSizeKey = -1;

/* Last drawn state — repaint only on change (or every OVERLAY_REPAINT_MS). */
static int g_lastMode = -1;
static int g_lastZone = -2;
static int g_lastScreenW = 0;
static int g_lastScreenH = 0;
static ULONGLONG g_lastRepaintMs = 0;

unsigned long overlay_ui_thread_id(void) {
    return (unsigned long)g_uiThreadId;
}

static const HudPalette* hud_palette(void) {
    int idx = g_config.hudTheme;
    if (idx < 0 || idx >= HUD_THEME_COUNT) {
        idx = 0;
    }
    return &g_palettes[idx];
}

static void hud_dimensions(int* outW, int* outH) {
    switch (g_config.hudSize) {
    case 1:  *outW = 228; *outH = 74; break; /* medium */
    case 2:  *outW = 260; *outH = 88; break; /* big */
    default: *outW = 195; *outH = 62; break; /* small */
    }
}

static void hud_font_sizes(int* outModePt, int* outSubPt) {
    switch (g_config.hudSize) {
    case 1:  *outModePt = 17; *outSubPt = 13; break;
    case 2:  *outModePt = 20; *outSubPt = 15; break;
    default: *outModePt = 14; *outSubPt = 11; break;
    }
}

static void hud_screen_position(int screenW, int screenH, int w, int h, int* outX, int* outY) {
    const int marginX = 20;
    int marginY = screenH * 2 / 100;
    if (marginY < 15) {
        marginY = 15;
    }

    switch (g_config.hudPosition) {
    case 1:  *outX = screenW - w - marginX; *outY = marginY; break;                 /* top right */
    case 2:  *outX = marginX; *outY = screenH - h - marginY; break;                 /* bottom left */
    case 3:  *outX = screenW - w - marginX; *outY = screenH - h - marginY; break;   /* bottom right */
    case 4:  *outX = (screenW - w) / 2; *outY = marginY; break;                     /* top center */
    default: *outX = marginX; *outY = marginY; break;                               /* top left */
    }
}

static void overlay_release_fonts(void) {
    if (g_modeFont) { DeleteObject(g_modeFont); g_modeFont = NULL; }
    if (g_subFont) { DeleteObject(g_subFont); g_subFont = NULL; }
    g_fontSizeKey = -1;
}

static void overlay_ensure_fonts(void) {
    if (g_fontSizeKey == g_config.hudSize && g_modeFont && g_subFont) {
        return;
    }
    overlay_release_fonts();
    g_fontSizeKey = g_config.hudSize;

    int modePt = 20, subPt = 15;
    hud_font_sizes(&modePt, &subPt);
    g_modeFont = CreateFontW(modePt, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_subFont = CreateFontW(subPt, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void overlay_reposition(void) {
    if (!g_hwnd) {
        return;
    }
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int w = 0, h = 0, x = 0, y = 0;
    hud_dimensions(&w, &h);
    hud_screen_position(screenW, screenH, w, h, &x, &y);
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    g_lastScreenW = screenW;
    g_lastScreenH = screenH;
}

/* Current zone index (-1 = none) — pure lookups, no TS API. */
static int overlay_current_zone(HubSettings* hub) {
    if (!server_profile_get(hub) || hub->zoneCount <= 0) {
        return -1;
    }
    PosSample local;
    if (!pos_get_current(&local)) {
        return -1;
    }
    return zone_resolve(hub, local.x / 100.0f, local.y / 100.0f, local.z / 100.0f);
}

static void overlay_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rect;
    GetClientRect(hwnd, &rect);
    const int boxW = rect.right - rect.left;
    const int boxH = rect.bottom - rect.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, boxW, boxH);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);
    const HudPalette* pal = hud_palette();

    HBRUSH bg = CreateSolidBrush(pal->bg);
    FillRect(memDC, &rect, bg);
    DeleteObject(bg);

    HPEN borderPen = CreatePen(PS_SOLID, 2, pal->border);
    HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
    Rectangle(memDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBrush);
    DeleteObject(borderPen);

    /* Line 1: voice mode. */
    static const wchar_t* const modeNames[] = { L"WHISPER", L"NORMAL", L"SHOUT" };
    const int mode = (int)voice_mode_get_current();
    const wchar_t* modeText = modeNames[(mode >= 0 && mode <= 2) ? mode : 1];

    /* Line 2: zone status. */
    HubSettings hub;
    const int zone = overlay_current_zone(&hub);
    wchar_t zoneText[96];
    COLORREF zoneColor = pal->zoneDefault;
    if (zone >= 0 && zone < hub.zoneCount) {
        const HubZone* z = &hub.zones[zone];
        if (z->soundproof && z->reverb) {
            swprintf(zoneText, 96, L"Zone: %.28hs [abhoersicher] [Hoehle]", z->name);
            zoneColor = RGB(225, 170, 80);
        }
        else if (z->soundproof) {
            swprintf(zoneText, 96, L"Zone: %.40hs [abhoersicher]", z->name);
            zoneColor = RGB(225, 170, 80);
        }
        else if (z->reverb) {
            swprintf(zoneText, 96, L"Zone: %.40hs [Hoehle/Echo]", z->name);
            zoneColor = RGB(140, 190, 230);
        }
        else {
            swprintf(zoneText, 96, L"Zone: %.40hs", z->name);
            zoneColor = RGB(120, 205, 120);
        }
    }
    else if (hub.valid && hub.zoneCount > 0) {
        swprintf(zoneText, 96, L"Ausserhalb");
        zoneColor = RGB(170, 150, 190);
    }
    else {
        swprintf(zoneText, 96, L"Keine Zonen geladen");
        zoneColor = RGB(160, 160, 160);
    }

    SetBkMode(memDC, TRANSPARENT);
    overlay_ensure_fonts();
    const int lineGap = (g_config.hudSize == 0) ? 2 : 3;

    SIZE modeSize = { 0 }, zoneSize = { 0 };
    SelectObject(memDC, g_modeFont);
    GetTextExtentPoint32W(memDC, modeText, (int)wcslen(modeText), &modeSize);
    SelectObject(memDC, g_subFont);
    GetTextExtentPoint32W(memDC, zoneText, (int)wcslen(zoneText), &zoneSize);

    int y = (boxH - (modeSize.cy + lineGap + zoneSize.cy)) / 2;
    if (y < 2) y = 2;

    int x = (boxW - modeSize.cx) / 2;
    if (x < 2) x = 2;
    SelectObject(memDC, g_modeFont);
    SetTextColor(memDC, pal->modeShadow);
    TextOutW(memDC, x + 1, y + 1, modeText, (int)wcslen(modeText));
    SetTextColor(memDC, pal->mode);
    TextOutW(memDC, x, y, modeText, (int)wcslen(modeText));
    y += modeSize.cy + lineGap;

    x = (boxW - zoneSize.cx) / 2;
    if (x < 2) x = 2;
    SelectObject(memDC, g_subFont);
    SetTextColor(memDC, pal->modeShadow);
    TextOutW(memDC, x + 1, y + 1, zoneText, (int)wcslen(zoneText));
    SetTextColor(memDC, zoneColor);
    TextOutW(memDC, x, y, zoneText, (int)wcslen(zoneText));

    BitBlt(hdc, 0, 0, boxW, boxH, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    g_lastMode = mode;
    g_lastZone = zone;
    g_lastRepaintMs = GetTickCount64();
    EndPaint(hwnd, &ps);
}

/* Timer tick: show/hide, reposition on resolution change, repaint on change.
   Also polls the settings-dialog hotkey (F10) — this thread always runs,
   unlike the pos watcher callback which only fires while ingame. */
static void overlay_tick(HWND hwnd) {
    ui_settings_hotkey_poll();

    const int shouldShow = g_config.enableVoiceOverlay && pos_coordinates_valid();
    const int isVisible = IsWindowVisible(hwnd);
    if (shouldShow && !isVisible) {
        overlay_reposition();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    else if (!shouldShow && isVisible) {
        ShowWindow(hwnd, SW_HIDE);
    }
    if (!shouldShow) {
        return;
    }

    if (GetSystemMetrics(SM_CXSCREEN) != g_lastScreenW
        || GetSystemMetrics(SM_CYSCREEN) != g_lastScreenH) {
        overlay_reposition();
    }

    HubSettings hub;
    const int mode = (int)voice_mode_get_current();
    const int zone = overlay_current_zone(&hub);
    const ULONGLONG now = GetTickCount64();
    if (mode != g_lastMode || zone != g_lastZone
        || now - g_lastRepaintMs >= OVERLAY_REPAINT_MS) {
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        overlay_ensure_fonts();
        SetTimer(hwnd, OVERLAY_TIMER_ID, OVERLAY_TIMER_MS, NULL);
        return 0;
    case WM_TIMER:
        if (wParam == OVERLAY_TIMER_ID && !InterlockedCompareExchange(&g_uiThreadStop, 0, 0)) {
            overlay_tick(hwnd);
        }
        return 0;
    case WM_PAINT:
        overlay_paint(hwnd);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT; /* click-through (paired with WS_EX_TRANSPARENT) */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        KillTimer(hwnd, OVERLAY_TIMER_ID);
        overlay_release_fonts();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

/* ---- UI thread ------------------------------------------------------------- */

static unsigned __stdcall overlay_thread_main(void* arg) {
    (void)arg;
    g_uiThreadId = GetCurrentThreadId();

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = overlay_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    UnregisterClassW(OVERLAY_CLASS_NAME, wc.hInstance);
    if (!RegisterClassW(&wc)) {
        log_write("OVERLAY: RegisterClass failed (%lu)", GetLastError());
        g_uiThreadId = 0;
        return 0;
    }

    int w = 0, h = 0, x = 0, y = 0;
    hud_dimensions(&w, &h);
    hud_screen_position(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), w, h, &x, &y);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS_NAME, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        log_write("OVERLAY: CreateWindowEx failed (%lu)", GetLastError());
        g_uiThreadId = 0;
        return 0;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, hud_palette()->alpha, LWA_ALPHA);
    g_lastScreenW = GetSystemMetrics(SM_CXSCREEN);
    g_lastScreenH = GetSystemMetrics(SM_CYSCREEN);
    log_write("OVERLAY: HUD window created (%dx%d at %d,%d)", w, h, x, y);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Settings dialog shares this thread and needs its keyboard input. */
        if (!ui_settings_handle_message(&msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    ui_settings_destroy();
    g_hwnd = NULL;
    g_uiThreadId = 0;
    return 0;
}

/* ---- lifecycle (TS callback thread) ------------------------------------------ */

void overlay_start(void) {
    if (g_uiThread) {
        return;
    }
    InterlockedExchange(&g_uiThreadStop, 0);
    g_uiThread = (HANDLE)_beginthreadex(NULL, 0, overlay_thread_main, NULL, 0, NULL);
    if (!g_uiThread) {
        log_write("OVERLAY: failed to start UI thread");
    }
}

void overlay_stop(void) {
    if (!g_uiThread) {
        return;
    }
    InterlockedExchange(&g_uiThreadStop, 1);

    /* Destroy on the owning thread: post WM_CLOSE-equivalent via thread msg. */
    HWND hwnd = g_hwnd;
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    else if (g_uiThreadId != 0) {
        PostThreadMessageW(g_uiThreadId, WM_QUIT, 0, 0);
    }

    /* The DLL must NOT unload while this thread still runs (instant crash).
       Escalate: WM_CLOSE -> direct WM_QUIT -> forced termination. */
    if (WaitForSingleObject(g_uiThread, 2000) != WAIT_OBJECT_0) {
        log_write("OVERLAY: UI thread ignored WM_CLOSE - posting WM_QUIT");
        if (g_uiThreadId != 0) {
            PostThreadMessageW(g_uiThreadId, WM_QUIT, 0, 0);
        }
        if (WaitForSingleObject(g_uiThread, 5000) != WAIT_OBJECT_0) {
            /* Last resort: a leaked lock is better than executing unloaded
               code. The thread only touches UI objects at this point. */
            log_write("OVERLAY: UI thread stuck - terminating");
#pragma warning(suppress: 6258) /* deliberate TerminateThread as final fallback */
            TerminateThread(g_uiThread, 1);
            WaitForSingleObject(g_uiThread, 1000);
            g_hwnd = NULL;
            g_uiThreadId = 0;
        }
    }
    CloseHandle(g_uiThread);
    g_uiThread = NULL;
    log_write("OVERLAY: stopped");
}
