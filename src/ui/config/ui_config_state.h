#ifndef UI_CONFIG_STATE_H
#define UI_CONFIG_STATE_H

/*
 * V8.12 — F10 settings state owned by g_config (via a dialog working copy).
 *
 * Thread contract:
 *   - ui_cfg_dialog_begin/end: F10 UI thread only (showConfigInterface loop).
 *   - ui_cfg(): working copy while the dialog is open, else &g_config (scalars
 *     only — do not read string fields outside the dialog without config_copy).
 *   - ui_cfg_commit(): F10 save path — applies working copy, config_save(), side
 *     effects, then refreshes legacy mirrors for overlay/key_watcher.
 */

#include "core/config/config.h"

PluginConfig* ui_cfg(void);

void ui_cfg_dialog_begin(void);
void ui_cfg_dialog_end(void);

/* Persist ui_cfg() working copy → g_config + plugin.cfg + runtime hooks. */
void ui_cfg_commit(void);

/* Copy persisted g_config into legacy plugin.h mirrors (overlay, etc.). */
void ui_cfg_publish_legacy_mirrors(void);

/* Active Pos.txt base (.../ConanSandbox/Saved) from ui_cfg path fields. */
void ui_cfg_get_active_saved_path(wchar_t* out, size_t outLen);

/* Set manual or automatic saved path from a full .../ConanSandbox/Saved path. */
void ui_cfg_set_active_saved_path_full(const wchar_t* savedPathFull);

#endif /* UI_CONFIG_STATE_H */
