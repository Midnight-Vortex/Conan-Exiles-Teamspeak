#ifndef UI_CONFIG_STATE_H
#define UI_CONFIG_STATE_H

/*
 * V8.12 — F10 settings state owned by g_config (via a dialog working copy).
 *
 * Thread contract:
 *   - ui_cfg_dialog_begin/end: F10 UI thread only (showConfigInterface loop).
 *     ui_cfg_dialog_end() is idempotent (WM_DESTROY and the exit of
 *     showConfigInterface both call it).
 *   - ui_cfg(): working copy while the dialog is open, else &g_config (scalars
 *     only — do not read string fields outside the dialog without config_copy).
 *   - ui_cfg_commit(): F10 save path — merges ONLY the dialog-owned fields of
 *     the working copy into g_config (config_modify, under the config lock),
 *     config_save(), side effects, then refreshes legacy mirrors for
 *     overlay/key_watcher.
 *   - ui_cfg_hud_preview(): F10 UI thread; pushes the HUD options of the
 *     working copy into g_config WITHOUT saving (live preview). They are
 *     restored by ui_cfg_dialog_end() unless a commit made them permanent.
 */

#include "core/config/config.h"

PluginConfig* ui_cfg(void);

void ui_cfg_dialog_begin(void);
void ui_cfg_dialog_end(void);

/* Persist ui_cfg() working copy → g_config + plugin.cfg + runtime hooks. */
void ui_cfg_commit(void);

/* Copy persisted g_config into legacy plugin.h mirrors (overlay, etc.). */
void ui_cfg_publish_legacy_mirrors(void);

/* HUD live preview (087): which overlay properties to refresh. */
#define UI_HUD_PART_THEME    1u
#define UI_HUD_PART_POSITION 2u
#define UI_HUD_PART_SIZE     4u

/* Apply the working copy's HUD theme/position/size to g_config (no file save)
   and refresh the selected overlay properties. No-op when no dialog is open. */
void ui_cfg_hud_preview(unsigned parts);

/* 091: unit the dialog SHOWS the voice distances in (UI thread). Storage and
   every calculation stay in meters; see core/config/range_units.h.
   ui_ranges_forced_by_server(): the Root description says
   ForceRangesInFoundations=True (and a profile is active) - the checkbox is then
   shown checked + locked.
   ui_ranges_in_foundations(): the unit the dialog fields are rendered in right
   now = the working copy's own choice OR that force, as of the last
   ui_range_unit_refresh().  The force is never written into the working copy.
   ui_range_unit_refresh(): recompute that unit; returns 1 when it CHANGED (the
   Root force arrived / was removed while the dialog is open) - the caller must
   then re-render every distance field before it reads one back.
   ui_range_text(): "%.1f" of a distance given in METERS, in that unit.
   ui_range_transport_text(): METERS as the text writeFullConfiguration() parses
   back (it is a hand-off, not a display). Meter mode: "%.1f", exactly as before
   091. Foundation mode: "%.9g" = the exact float, so 15.36 m (6 foundations)
   does not become 15.4 m and fail the next Save against a 15.36 m limit. */
int ui_ranges_forced_by_server(void);
int ui_ranges_in_foundations(void);
int ui_range_unit_refresh(void);
void ui_range_text(wchar_t* out, size_t cap, float meters);
void ui_range_transport_text(wchar_t* out, size_t cap, float meters);

/* Active Pos.txt base (.../ConanSandbox/Saved) from ui_cfg path fields. */
void ui_cfg_get_active_saved_path(wchar_t* out, size_t outLen);

/* Set manual or automatic saved path from a full .../ConanSandbox/Saved path. */
void ui_cfg_set_active_saved_path_full(const wchar_t* savedPathFull);

#endif /* UI_CONFIG_STATE_H */
