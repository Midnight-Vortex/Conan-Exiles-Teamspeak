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

/* Control IDs of the clickable labels next to the checkboxes. They must stay
   unique inside the dialog: 2001-2005 are the owner-drawn key boxes (created in
   ui_config_controls.c, drawn in ui_config_draw.c). Before 087 the labels used
   2001-2004 as well, so GetDlgItem() could return the wrong control.
   IDs uniques des libelles de cases a cocher (2001-2005 = boites de touches). */
#define IDC_LABEL_AUTOPATCH   2010 /* "Automatic Patch Find" (Patch Configuration tab) */
#define IDC_LABEL_DISTMUTING  2011 /* distance-based muting   (Advanced tab) */
#define IDC_LABEL_CHANSWITCH  2012 /* automatic channel switching (Advanced tab) */
#define IDC_LABEL_VOICETOGGLE 2013 /* voice mode toggle button (Advanced tab) */
#define IDC_LABEL_RANGEUNIT   2014 /* "Show ranges in foundations" (091). Unlike the three above its
                                      text is NOT baked into the background bitmap: this static draws it. */

/* Checkbox of that label (Plugin Settings tab, 091). 200/201/203/204 are the other checkboxes. */
#define IDC_CHECK_RANGEUNIT   205

/* Image cache (ui_config_draw.c). Resource images used by the owner-draw code
   are decoded ONCE (on first use) and kept until the dialog is gone, instead of
   one LoadImageW per control per WM_DRAWITEM. The cache owns the handles: never
   DeleteObject/DestroyIcon what these return.
   Cache d'images : chaque ressource est decodee UNE fois puis liberee a la
   fermeture du dialogue. */
HBITMAP ui_image_cache_get(int resourceId);  /* NULL when the resource is missing */
HICON ui_checkmark_icon_get(void);           /* 16x16 IDI_CHECKMARK, NULL when missing */
/* Frees the cache, the category backgrounds (hBackground*Bitmap) and the check
   icon and NULLs the shared handles. Call after the dialog window is destroyed. */
void ui_image_cache_release(void);

/* Save helper (ui_config_command.c): reads and validates the three distance
   edits. TRUE = all valid (values returned). FALSE = one field is invalid: a
   status message was shown and NOTHING must be overwritten. In a hub zone the
   edits display the zone's distances, so the personal working-copy distances are
   returned unchanged. */
BOOL ui_collect_distances_for_save(float* outWhisper, float* outNormal, float* outShout);

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
