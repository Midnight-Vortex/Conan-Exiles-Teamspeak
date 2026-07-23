#ifndef UI_CONFIG_INTERNAL_H
#define UI_CONFIG_INTERNAL_H

/*
 * EN: Internal shared header for the split F10 settings dialog (ui_config_*.c,
 *     ui_presets.c, ui_path_steam.c, ui_main.c). Declares the handles/helpers
 *     that used to be file-static in the monolithic ui_main.c but are now used
 *     across the split files. Pure mechanical split (V8.7) — no behaviour change.
 * FR: En-tete interne partage pour le dialogue F10 eclate. Declare les aides
 *     autrefois statiques dans ui_main.c, desormais utilisees entre fichiers.
 *
 * Thread contract: all these run on the settings-dialog UI thread only
 * (own GetMessage loop in showConfigInterface). No TS API here.
 */

#include "plugin_internal.h"
#include <windows.h>

/* Shared config-dialog GDI state (defined in ui_config_dialog.c). */
extern HBRUSH g_hHudComboBrush;
extern BOOL g_configDialogDestroying;

/* HUD combo helpers shared between control creation (ui_config_controls.c)
   and the Save/preview handlers in WM_COMMAND (ui_main.c). */
void ui_sync_hud_theme_combo(void);
void ui_read_hud_theme_from_combo(void);
void ui_read_hud_position_from_combo(void);
void ui_read_hud_size_from_combo(void);

/* Layout / drawing / navigation helpers shared across the split files. */
void ShowCategoryControls(int category);
void ApplyFontToControl(HWND control, HFONT font);
HBITMAP LoadBackgroundFromResource(int resourceID);
LRESULT CALLBACK CheckboxLabelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
void browseSavedPath(HWND hwnd);
void processKeyCapture(void);

/* ConfigDialogProc message handlers, split across files but dispatched from
   the shell switch in ui_config_dialog.c. Each returns the exact LRESULT the
   original single switch returned for that message. */
LRESULT ui_config_on_create(HWND hwnd, WPARAM wParam, LPARAM lParam);         /* ui_config_controls.c */
LRESULT ui_config_on_erasebkgnd(HWND hwnd, WPARAM wParam, LPARAM lParam);     /* ui_config_draw.c */
LRESULT ui_config_on_ctlcolorstatic(HWND hwnd, WPARAM wParam, LPARAM lParam); /* ui_config_draw.c */
LRESULT ui_config_on_ctlcolorlistbox(HWND hwnd, WPARAM wParam, LPARAM lParam);/* ui_config_draw.c */
LRESULT ui_config_on_ctlcoloredit(HWND hwnd, WPARAM wParam, LPARAM lParam);   /* ui_config_draw.c */
LRESULT ui_config_on_command(HWND hwnd, WPARAM wParam, LPARAM lParam);        /* ui_config_command.c */
LRESULT ui_config_on_drawitem(HWND hwnd, WPARAM wParam, LPARAM lParam);       /* ui_config_draw.c */

#endif /* UI_CONFIG_INTERNAL_H */
