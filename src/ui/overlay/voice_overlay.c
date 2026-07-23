#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "resource.h"
#ifdef CONAN_EXILES_TS_EXPORTS
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#endif
#include "core/proximity/proximity_math.h"
#include "core/config/config.h"
#include "core/voice/voice_modes.h"
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

// MODULE 13: IN-GAME VOICE OVERLAY (HUD)
// EN: Transparent overlay — voice mode, zone name, proximity status (separate overlay thread).
// FR: Overlay transparent — mode voix, nom zone, statut proximité (thread overlay dédié).
// ============================================================================

#define WM_VOICEOVERLAY_REFRESH (WM_APP + 100)
#define WM_VOICEOVERLAY_REPOSITION (WM_APP + 101)
#define WM_VOICEOVERLAY_RESIZE (WM_APP + 103)
/* Destroy overlay on its owning thread (DestroyWindow from another thread crashes later).
   Détruire l'overlay sur son thread propriétaire (DestroyWindow depuis un autre thread plante plus tard). */
#define WM_VOICEOVERLAY_DESTROY (WM_APP + 102)

#define VOICE_OVERLAY_WIDTH_BIG  260
#define VOICE_OVERLAY_HEIGHT_BIG 88

typedef struct {
    COLORREF bg;
    COLORREF border;
    COLORREF borderHi;
    COLORREF modeColor;
    COLORREF modeShadow;
    COLORREF raceColor;
    BYTE alpha;
} VoiceHudThemePalette;

// Color palettes for the bottom-right voice HUD. Index = g_config.hudTheme from plugin.cfg.
// Palettes de couleurs du HUD vocal en bas à droite. Index = g_config.hudTheme dans plugin.cfg.
static const VoiceHudThemePalette g_voiceHudThemes[VOICE_HUD_THEME_COUNT] = {
    /* Gray (original) */
    { RGB(0, 0, 0), RGB(200, 200, 200), RGB(255, 255, 255), RGB(140, 140, 140), RGB(0, 0, 0), RGB(170, 190, 230), 100 },
    /* Purple */
    { RGB(22, 8, 38), RGB(120, 65, 175), RGB(200, 150, 255), RGB(235, 215, 255), RGB(12, 0, 22), RGB(195, 165, 240), 100 },
    /* Blue */
    { RGB(8, 18, 38), RGB(65, 95, 175), RGB(150, 180, 255), RGB(215, 225, 255), RGB(0, 8, 22), RGB(165, 185, 240), 100 },
    /* Green */
    { RGB(8, 28, 14), RGB(55, 120, 75), RGB(150, 220, 170), RGB(215, 245, 220), RGB(0, 18, 8), RGB(165, 210, 175), 100 },
    /* Amber */
    { RGB(32, 22, 8), RGB(150, 110, 45), RGB(230, 190, 120), RGB(255, 235, 200), RGB(18, 12, 0), RGB(220, 190, 140), 100 },
    /* Red */
    { RGB(32, 8, 8), RGB(150, 55, 55), RGB(230, 140, 140), RGB(255, 215, 215), RGB(18, 0, 0), RGB(220, 165, 165), 100 },
    /* Cyan */
    { RGB(6, 24, 30), RGB(45, 130, 150), RGB(120, 210, 230), RGB(200, 245, 255), RGB(0, 12, 18), RGB(150, 210, 225), 100 },
    /* Pink */
    { RGB(30, 10, 22), RGB(150, 70, 110), RGB(230, 150, 190), RGB(255, 220, 235), RGB(16, 0, 10), RGB(225, 175, 205), 100 },
    /* Orange */
    { RGB(34, 18, 6), RGB(170, 90, 35), RGB(240, 160, 90), RGB(255, 225, 195), RGB(18, 8, 0), RGB(230, 185, 140), 100 },
    /* Teal */
    { RGB(6, 22, 22), RGB(40, 120, 115), RGB(110, 200, 195), RGB(200, 245, 240), RGB(0, 14, 12), RGB(155, 205, 200), 100 },
    /* White */
    { RGB(28, 28, 32), RGB(170, 170, 180), RGB(245, 245, 250), RGB(255, 255, 255), RGB(12, 12, 14), RGB(210, 215, 225), 100 },
    /* Lime */
    { RGB(14, 26, 8), RGB(90, 140, 45), RGB(180, 230, 110), RGB(230, 250, 200), RGB(6, 14, 0), RGB(190, 220, 140), 100 },
};

static const VoiceHudThemePalette* voice_overlay_get_palette(void) {
    int idx = g_config.hudTheme;
    if (idx < 0 || idx >= VOICE_HUD_THEME_COUNT) {
        idx = VOICE_HUD_THEME_GRAY;
    }
    return &g_voiceHudThemes[idx];
}

/* Safe HWND for cross-thread PostMessage — avoids stale handle use during shutdown. */
static HWND voice_overlay_lock_hwnd(void) {
    if (pluginShuttingDown) {
        return NULL;
    }
    HWND hwnd = hVoiceOverlay;
    if (!hwnd || !IsWindow(hwnd)) {
        return NULL;
    }
    return hwnd;
}

static void voice_overlay_post(UINT msg) {
    HWND hwnd = voice_overlay_lock_hwnd();
    if (hwnd) {
        PostMessage(hwnd, msg, 0, 0);
    }
}

static void voice_overlay_apply_window_alpha(void) {
    HWND hwnd = voice_overlay_lock_hwnd();
    if (!hwnd) {
        return;
    }
    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hwnd, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() != overlayThreadId) {
        voice_overlay_post(WM_VOICEOVERLAY_REFRESH);
        return;
    }
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), voice_overlay_get_palette()->alpha, LWA_ALPHA);
}

void voice_overlay_refresh_theme(void) {
    if (pluginShuttingDown) {
        return;
    }
    // Called when user changes HUD theme in F10 or after loading plugin.cfg.
    // Appelé quand l'utilisateur change le thème HUD dans F10 ou après chargement de plugin.cfg.
    voice_overlay_apply_window_alpha();
    updateVoiceOverlay();
}

static void voice_overlay_compute_screen_position(int screenWidth, int screenHeight,
    int overlayWidth, int overlayHeight, int* outX, int* outY) {
    const int marginX = 20;
    int marginY = screenHeight * 2 / 100;
    if (marginY < 15) {
        marginY = 15;
    }

    int pos = g_config.hudPosition;
    if (pos < 0 || pos >= VOICE_HUD_POSITION_COUNT) {
        pos = VOICE_HUD_POSITION_BOTTOM_RIGHT;
    }

    switch (pos) {
    case VOICE_HUD_POSITION_TOP_RIGHT:
        *outX = screenWidth - overlayWidth - marginX;
        *outY = marginY;
        break;
    case VOICE_HUD_POSITION_BOTTOM_LEFT:
        *outX = marginX;
        *outY = screenHeight - overlayHeight - marginY;
        break;
    case VOICE_HUD_POSITION_TOP_CENTER:
        *outX = (screenWidth - overlayWidth) / 2;
        *outY = marginY;
        break;
    case VOICE_HUD_POSITION_BOTTOM_RIGHT:
        *outX = screenWidth - overlayWidth - marginX;
        *outY = screenHeight - overlayHeight - marginY;
        break;
    case VOICE_HUD_POSITION_TOP_LEFT:
    default:
        *outX = marginX;
        *outY = marginY;
        break;
    }
}

void voice_overlay_refresh_position(void) {
    if (pluginShuttingDown) {
        return;
    }
    // Called when user changes HUD position in F10 or after loading plugin.cfg.
    // Appelé quand l'utilisateur change la position HUD dans F10 ou après chargement de plugin.cfg.
    repositionVoiceOverlay();
    updateVoiceOverlay();
}

void voice_overlay_get_dimensions(int* outWidth, int* outHeight) {
    int size = g_config.hudSize;
    if (size < 0 || size >= VOICE_HUD_SIZE_COUNT) {
        size = VOICE_HUD_SIZE_BIG;
    }

    switch (size) {
    case VOICE_HUD_SIZE_SMALL:
        if (outWidth) *outWidth = 195;
        if (outHeight) *outHeight = 62;
        break;
    case VOICE_HUD_SIZE_MEDIUM:
        if (outWidth) *outWidth = 228;
        if (outHeight) *outHeight = 74;
        break;
    case VOICE_HUD_SIZE_BIG:
    default:
        if (outWidth) *outWidth = VOICE_OVERLAY_WIDTH_BIG;
        if (outHeight) *outHeight = VOICE_OVERLAY_HEIGHT_BIG;
        break;
    }
}

static void voice_overlay_get_font_sizes(int* outModePt, int* outSubPt) {
    int size = g_config.hudSize;
    if (size < 0 || size >= VOICE_HUD_SIZE_COUNT) {
        size = VOICE_HUD_SIZE_BIG;
    }

    switch (size) {
    case VOICE_HUD_SIZE_SMALL:
        if (outModePt) *outModePt = 14;
        if (outSubPt) *outSubPt = 11;
        break;
    case VOICE_HUD_SIZE_MEDIUM:
        if (outModePt) *outModePt = 17;
        if (outSubPt) *outSubPt = 13;
        break;
    case VOICE_HUD_SIZE_BIG:
    default:
        if (outModePt) *outModePt = 20;
        if (outSubPt) *outSubPt = 15;
        break;
    }
}

static HFONT g_hudModeFont = NULL;
static HFONT g_hudSubFont = NULL;
static int g_hudFontSizeKey = -1;
static int g_overlayLastZone = -2;
static int g_overlayLastMode = -1;
static int g_overlayLastHighlight = -1;
static ULONGLONG g_overlayLastRepaintTick = 0;

static void overlay_release_fonts(void) {
    if (g_hudModeFont) {
        DeleteObject(g_hudModeFont);
        g_hudModeFont = NULL;
    }
    if (g_hudSubFont) {
        DeleteObject(g_hudSubFont);
        g_hudSubFont = NULL;
    }
    g_hudFontSizeKey = -1;
}

static void overlay_ensure_fonts(void) {
    if (g_hudFontSizeKey == g_config.hudSize && g_hudModeFont && g_hudSubFont) {
        return;
    }

    overlay_release_fonts();
    g_hudFontSizeKey = g_config.hudSize;

    int modePt = 20;
    int subPt = 15;
    voice_overlay_get_font_sizes(&modePt, &subPt);

    g_hudModeFont = CreateFontA(
        modePt, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hudSubFont = CreateFontA(
        subPt, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void overlay_request_repaint(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static int plugin_is_ingame_channel_for_hud(void) {
    /* Non-debug HUD: only while seated in the TS ingame channel. */
    if (ingameChannelID == -1 || ts3LocalChannelID == -1) {
        return 0;
    }
    return ts3LocalChannelID == ingameChannelID;
}

int plugin_should_show_voice_overlay(void) {
    if (!g_config.enableVoiceOverlay) {
        return 0;
    }
    if (enableLogGeneral) {
        return 1;
    }
    return plugin_is_ingame_channel_for_hud();
}

void updateVoiceOverlayVisibility(void) {
    if (pluginShuttingDown) {
        return;
    }
    if (!hVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }

    // Overlay window lives on its own thread — marshal show/hide via posted message.
    // La fenêtre overlay a son propre thread — afficher/masquer via message posté.
    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hVoiceOverlay, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() != overlayThreadId) {
        voice_overlay_post(WM_VOICEOVERLAY_REFRESH);
        return;
    }

    BOOL shouldShow = plugin_should_show_voice_overlay();
    BOOL isVisible = IsWindowVisible(hVoiceOverlay);
    if (shouldShow && !isVisible) {
        ShowWindow(hVoiceOverlay, SW_SHOWNOACTIVATE);
    }
    else if (!shouldShow && isVisible) {
        ShowWindow(hVoiceOverlay, SW_HIDE);
    }
}

void repositionVoiceOverlayImpl(void) {
    // Skip during plugin shutdown to avoid touching a window being destroyed.
    // Ignorer pendant l'arrêt du plugin pour ne pas toucher une fenêtre en destruction.
    if (pluginShuttingDown || !enableGetPlayerCoordinates) {
        return;
    }
    if (!hVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = 0;
    int posY = 0;
    int overlayWidth = 0;
    int overlayHeight = 0;
    voice_overlay_get_dimensions(&overlayWidth, &overlayHeight);
    voice_overlay_compute_screen_position(screenWidth, screenHeight,
        overlayWidth, overlayHeight, &posX, &posY);

    UINT showFlags = plugin_should_show_voice_overlay() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    SetWindowPos(hVoiceOverlay, HWND_TOPMOST, posX, posY, overlayWidth, overlayHeight,
        SWP_NOACTIVATE | showFlags);
}

void voice_overlay_refresh_size(void) {
    if (pluginShuttingDown) {
        return;
    }
    if (!hVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }

    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hVoiceOverlay, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() != overlayThreadId) {
        voice_overlay_post(WM_VOICEOVERLAY_RESIZE);
        return;
    }

    overlay_release_fonts();
    overlay_ensure_fonts();
    repositionVoiceOverlayImpl();
    overlay_request_repaint(hVoiceOverlay);
}

void setOverlayHighlightState(mumble_userid_t userID, mumble_connection_t connection, BOOL highlight) {
    (void)connection;
    // Ne pas modifier de texte, ne rien envoyer au serveur — uniquement UI locale
    if (highlight) {
        overlayBorderHighlight = TRUE;
        overlayHighlightUserID = userID;
    }
    else {
        if (overlayBorderHighlight && overlayHighlightUserID == userID) {
            overlayBorderHighlight = FALSE;
            overlayHighlightUserID = 0;
        }
    }

    updateVoiceOverlay();
}

// Get current voice mode text | Obtenir le texte du mode de voix actuel
const char* getCurrentVoiceModeText() {
    switch (voice_mode_get_current()) {
    case VOICE_MODE_WHISPER: return "WHISPER";
    case VOICE_MODE_SHOUT:   return "SHOUT";
    case VOICE_MODE_NORMAL:
    default:                 return "NORMAL";
    }
}

// Voice overlay window procedure | Procédure de fenêtre pour l'overlay vocal
LRESULT CALLBACK VoiceOverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        overlay_ensure_fonts();
        return 0;

    case WM_VOICEOVERLAY_DESTROY:
        DestroyWindow(hwnd);
        return 0;

    case WM_VOICEOVERLAY_REFRESH:
        if (pluginShuttingDown) {
            return 0;
        }
        if (!plugin_should_show_voice_overlay()) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        overlay_request_repaint(hwnd);
        return 0;

    case WM_VOICEOVERLAY_REPOSITION:
        if (pluginShuttingDown) {
            return 0;
        }
        repositionVoiceOverlayImpl();
        return 0;

    case WM_VOICEOVERLAY_RESIZE:
        if (pluginShuttingDown) {
            return 0;
        }
        overlay_release_fonts();
        overlay_ensure_fonts();
        repositionVoiceOverlayImpl();
        overlay_request_repaint(hwnd);
        return 0;

    case WM_PAINT: {
        if (pluginShuttingDown) {
            PAINTSTRUCT psSkip;
            BeginPaint(hwnd, &psSkip);
            EndPaint(hwnd, &psSkip);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);
        const int boxW = rect.right - rect.left;
        const int boxH = rect.bottom - rect.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, boxW, boxH);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);
        const VoiceHudThemePalette* theme = voice_overlay_get_palette();

        HBRUSH hBgBrush = CreateSolidBrush(theme->bg);
        FillRect(memDC, &rect, hBgBrush);
        DeleteObject(hBgBrush);

        HPEN hBorderPen = CreatePen(PS_SOLID, 2, theme->border);
        HPEN hOldPen = (HPEN)SelectObject(memDC, hBorderPen);
        HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, hNullBrush);
        Rectangle(memDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1);
        SelectObject(memDC, hOldPen);
        SelectObject(memDC, hOldBrush);
        DeleteObject(hBorderPen);

        if (overlayBorderHighlight) {
            HPEN hHiPen = CreatePen(PS_SOLID, 3, theme->borderHi);
            HPEN hPrev = (HPEN)SelectObject(memDC, hHiPen);
            Rectangle(memDC, rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2);
            SelectObject(memDC, hPrev);
            DeleteObject(hHiPen);
        }

        char textBuffer[128] = { 0 };
        if (plugin_overlay_text_lock_try()) {
            if (overlaySpeakerText[0] != '\0') {
                strncpy_s(textBuffer, sizeof(textBuffer), overlaySpeakerText, _TRUNCATE);
            }
            plugin_overlay_text_lock_release();
        }

        const char* modeText = textBuffer;
        if (modeText[0] == '\0') {
            modeText = getCurrentVoiceModeText();
        }

        /* Build all three lines, then draw centered as a block inside the box. */
        char zoneText[96];
        COLORREF zoneColor;
        int zi = currentZoneIndex;
        if (!hubDescriptionAvailable && isConnectedToServer) {
            snprintf(zoneText, sizeof(zoneText), "Server-Parameter laden...");
            zoneColor = RGB(190, 190, 210);
        }
        else if (zoneCount == 0) {
            snprintf(zoneText, sizeof(zoneText), "Keine Zonen");
            zoneColor = RGB(170, 170, 180);
        }
        else if (zi >= 0 && (size_t)zi < zoneCount) {
            if (zones[zi].isSoundproof && zones[zi].isReverb) {
                snprintf(zoneText, sizeof(zoneText), "Zone: %.28s [abhoersicher] [Hoehle]", zones[zi].name);
                zoneColor = RGB(225, 170, 80);
            }
            else if (zones[zi].isSoundproof) {
                snprintf(zoneText, sizeof(zoneText), "Zone: %.40s [abhoersicher]", zones[zi].name);
                zoneColor = RGB(225, 170, 80);
            }
            else if (zones[zi].isReverb) {
                snprintf(zoneText, sizeof(zoneText), "Zone: %.40s [Hoehle/Echo]", zones[zi].name);
                zoneColor = RGB(140, 190, 230);
            }
            else {
                snprintf(zoneText, sizeof(zoneText), "Zone: %.40s", zones[zi].name);
                zoneColor = RGB(120, 205, 120);
            }
        }
        else {
            snprintf(zoneText, sizeof(zoneText), "Ausserhalb");
            zoneColor = RGB(170, 150, 190);
        }

        char raceText[80] = { 0 };
        const int hasRace = (currentPlayerRaceIndex >= 0
            && (size_t)currentPlayerRaceIndex < raceCount);
        if (hasRace) {
            snprintf(raceText, sizeof(raceText), "Rasse: %.32s", races[currentPlayerRaceIndex].name);
        }

        SetBkMode(memDC, TRANSPARENT);
        int lineGap = (g_config.hudSize == VOICE_HUD_SIZE_SMALL) ? 2 : 3;
        overlay_ensure_fonts();

        SIZE modeSize = { 0 }, zoneSize = { 0 }, raceSize = { 0 };
        SelectObject(memDC, g_hudModeFont);
        GetTextExtentPoint32A(memDC, modeText, (int)strlen(modeText), &modeSize);
        SelectObject(memDC, g_hudSubFont);
        GetTextExtentPoint32A(memDC, zoneText, (int)strlen(zoneText), &zoneSize);
        if (hasRace) {
            GetTextExtentPoint32A(memDC, raceText, (int)strlen(raceText), &raceSize);
        }

        int totalH = modeSize.cy + lineGap + zoneSize.cy;
        if (hasRace) {
            totalH += lineGap + raceSize.cy;
        }
        int y = (boxH - totalH) / 2;
        if (y < 2) y = 2;

        int modeX = (boxW - modeSize.cx) / 2;
        if (modeX < 2) modeX = 2;
        SelectObject(memDC, g_hudModeFont);
        SetTextColor(memDC, theme->modeShadow);
        TextOutA(memDC, modeX + 1, y + 1, modeText, (int)strlen(modeText));
        SetTextColor(memDC, theme->modeColor);
        TextOutA(memDC, modeX, y, modeText, (int)strlen(modeText));
        y += modeSize.cy + lineGap;

        int zoneX = (boxW - zoneSize.cx) / 2;
        if (zoneX < 2) zoneX = 2;
        SelectObject(memDC, g_hudSubFont);
        SetTextColor(memDC, theme->modeShadow);
        TextOutA(memDC, zoneX + 1, y + 1, zoneText, (int)strlen(zoneText));
        SetTextColor(memDC, zoneColor);
        TextOutA(memDC, zoneX, y, zoneText, (int)strlen(zoneText));

        if (hasRace) {
            y += zoneSize.cy + lineGap;
            int raceX = (boxW - raceSize.cx) / 2;
            if (raceX < 2) raceX = 2;
            SetTextColor(memDC, theme->modeShadow);
            TextOutA(memDC, raceX + 1, y + 1, raceText, (int)strlen(raceText));
            SetTextColor(memDC, theme->raceColor);
            TextOutA(memDC, raceX, y, raceText, (int)strlen(raceText));
        }

        BitBlt(hdc, 0, 0, boxW, boxH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        g_overlayLastZone = currentZoneIndex;
        g_overlayLastMode = voice_mode_get_current();
        g_overlayLastHighlight = overlayBorderHighlight ? 1 : 0;
        g_overlayLastRepaintTick = GetTickCount64();

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        /* Pass mouse hits to the game (paired with WS_EX_TRANSPARENT at create time). */
        if (pluginShuttingDown) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        /* Never steal focus from the game when the overlay is briefly hit-tested. */
        return MA_NOACTIVATE;

    case WM_DESTROY:
        overlay_release_fonts();
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Create voice overlay | Créer l'overlay vocal
void createVoiceOverlay() {
    if (hVoiceOverlay != NULL) return;
    if (!g_config.enableVoiceOverlay) return;

    // Register window class | Enregistrer la classe de fenêtre
    const wchar_t OVERLAY_CLASS_NAME[] = L"VoiceOverlayClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = VoiceOverlayProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    UnregisterClassW(OVERLAY_CLASS_NAME, wc.hInstance);
    if (RegisterClassW(&wc) == 0) {
        if (enableLogGeneral) {
            mumbleAPI.log(ownID, "ERROR: Failed to register overlay window class");
        }
        return;
    }

    // Get screen dimensions | Obtenir les dimensions de l'écran
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // Overlay dimensions | Dimensions de l'overlay
    // Higher than before so a second line (zone status) fits under the voice mode.
    int overlayWidth = 0;
    int overlayHeight = 0;
    voice_overlay_get_dimensions(&overlayWidth, &overlayHeight);

    int posX = 0;
    int posY = 0;
    voice_overlay_compute_screen_position(screenWidth, screenHeight, overlayWidth, overlayHeight, &posX, &posY);

    // Create with extended styles for fullscreen compatibility | Créer avec styles étendus pour compatibilité plein écran
    // WS_EX_TRANSPARENT: OS-level click-through for fullscreen games.
    // WM_NCHITTEST/HTTRANSPARENT remains as a fallback; not related to TS shutdown crashes.
    hVoiceOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS_NAME,
        L"",
        WS_POPUP,
        posX, posY, overlayWidth, overlayHeight,
        NULL, NULL, wc.hInstance, NULL
    );

    if (hVoiceOverlay) {
        // Set transparency | Définir la transparence
        SetLayeredWindowAttributes(hVoiceOverlay, RGB(0, 0, 0), voice_overlay_get_palette()->alpha, LWA_ALPHA);

        // Force maximum z-order for fullscreen compatibility | Forcer l'ordre z maximum pour compatibilité plein écran
        SetWindowPos(hVoiceOverlay, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
            | (plugin_should_show_voice_overlay() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

        // Additional fullscreen compatibility measures | Mesures supplémentaires pour compatibilité plein écran
        LONG_PTR exStyle = GetWindowLongPtrW(hVoiceOverlay, GWL_EXSTYLE);
        exStyle |= WS_EX_TOPMOST | WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hVoiceOverlay, GWL_EXSTYLE, exStyle);

        // Show window | Afficher la fenêtre
        updateVoiceOverlayVisibility();
        overlay_request_repaint(hVoiceOverlay);

        if (enableLogGeneral) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Voice overlay created with FULLSCREEN COMPATIBILITY at (%d, %d)",
                posX, posY);
            mumbleAPI.log(ownID, msg);
        }
    }
    else {
        DWORD error = GetLastError();
        if (enableLogGeneral) {
            char errorMsg[128];
            snprintf(errorMsg, sizeof(errorMsg), "ERROR: Failed to create overlay window. Error: %lu", error);
            mumbleAPI.log(ownID, errorMsg);
        }
    }
}

// Add fullscreen overlay refresh function | Ajouter fonction de rafraîchissement pour plein écran
void refreshOverlayForFullscreen() {
    if (pluginShuttingDown) {
        return;
    }
    if (!hVoiceOverlay || !g_config.enableVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }
    if (!plugin_should_show_voice_overlay()) {
        return;
    }

    /* Never paint/update from the monitor thread — PostMessage only. */
    voice_overlay_post(WM_VOICEOVERLAY_REFRESH);
}

// Reposition voice overlay | Repositionner l'overlay vocal
void repositionVoiceOverlay() {
    if (pluginShuttingDown) {
        return;
    }
    if (!hVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }

    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hVoiceOverlay, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() != overlayThreadId) {
        voice_overlay_post(WM_VOICEOVERLAY_REPOSITION);
        return;
    }

    repositionVoiceOverlayImpl();

    if (enableLogGeneral) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Voice overlay repositioned for screen %dx%d",
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        mumbleAPI.log(ownID, msg);
    }
}

// Update voice overlay display | Mettre à jour l'affichage de l'overlay
void updateVoiceOverlay() {
    if (pluginShuttingDown) {
        return;
    }
    if (!hVoiceOverlay || !g_config.enableVoiceOverlay || !IsWindow(hVoiceOverlay)) {
        return;
    }

    updateVoiceOverlayVisibility();
    if (!plugin_should_show_voice_overlay()) {
        return;
    }

    int contentChanged = (currentZoneIndex != g_overlayLastZone
        || voice_mode_get_current() != g_overlayLastMode
        || (overlayBorderHighlight ? 1 : 0) != g_overlayLastHighlight);
    ULONGLONG now = GetTickCount64();
    if (!contentChanged && now - g_overlayLastRepaintTick < 400) {
        return;
    }

    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hVoiceOverlay, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() == overlayThreadId) {
        overlay_request_repaint(hVoiceOverlay);
    }
    else {
        voice_overlay_post(WM_VOICEOVERLAY_REFRESH);
    }
}

// Destroy overlay HWND on its owning thread (required by Win32).
// Détruire le HWND overlay sur son thread propriétaire (exigé par Win32).
void plugin_destroy_voice_overlay_safely(void) {
    HWND hwnd = hVoiceOverlay;
    hVoiceOverlay = NULL;

    if (!hwnd || !IsWindow(hwnd)) {
        overlay_release_fonts();
        if (hOverlayFont) {
            DeleteObject(hOverlayFont);
            hOverlayFont = NULL;
        }
        return;
    }

    DWORD overlayThreadId = 0;
    GetWindowThreadProcessId(hwnd, &overlayThreadId);
    if (overlayThreadId != 0 && GetCurrentThreadId() != overlayThreadId) {
        SendMessage(hwnd, WM_VOICEOVERLAY_DESTROY, 0, 0);
    }
    else {
        DestroyWindow(hwnd);
    }

    if (hOverlayFont) {
        DeleteObject(hOverlayFont);
        hOverlayFont = NULL;
    }
}

// Destroy voice overlay | Détruire l'overlay vocal
void destroyVoiceOverlay() {
    plugin_destroy_voice_overlay_safely();
}

unsigned __stdcall overlayMonitorThreadEx(void* arg) {
    overlayMonitorThread(arg);
    return 0;
}

// Resolution monitor thread | Thread pour surveiller les changements de résolution
void overlayMonitorThread(void* arg) {
    (void)arg;
    overlayThreadRunning = TRUE;

    int lastScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    int lastScreenHeight = GetSystemMetrics(SM_CYSCREEN);

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Voice overlay monitor thread started with fullscreen support");
    }

    while (overlayThreadRunning && enableGetPlayerCoordinates) {
        if (pluginShuttingDown) {
            break;
        }
        int currentScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        int currentScreenHeight = GetSystemMetrics(SM_CYSCREEN);

        if (currentScreenWidth != lastScreenWidth || currentScreenHeight != lastScreenHeight) {
            if (enableLogGeneral) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Screen resolution changed: %dx%d -> %dx%d",
                    lastScreenWidth, lastScreenHeight, currentScreenWidth, currentScreenHeight);
                mumbleAPI.log(ownID, msg);
            }

            repositionVoiceOverlay();
            lastScreenWidth = currentScreenWidth;
            lastScreenHeight = currentScreenHeight;
        }

        Sleep(2000);
    }

    overlayThreadRunning = FALSE;

    if (enableLogGeneral) {
        mumbleAPI.log(ownID, "Voice overlay monitor thread stopped");
    }
}

// ============================================================================
