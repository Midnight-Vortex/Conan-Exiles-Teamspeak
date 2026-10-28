#ifndef CORE_CONFIG_CONFIG_H
#define CORE_CONFIG_CONFIG_H

/*
 * plugin.cfg — Documents\Conan Exiles TeamSpeak plugin\plugin.cfg
 *
 * Thread contract:
 *   - config_load: TS callback thread, once at init.
 *   - g_config is guarded by one internal lock.  config_apply / config_copy /
 *     config_modify take it and may be called from ANY thread (settings
 *     dialog, TS callback, pos watcher, ...).
 *   - config_save: ANY thread.  Saves are serialized by their own lock and
 *     atomic (plugin.cfg.tmp + rename): a crash or a full disk never leaves a
 *     truncated plugin.cfg, and two threads saving at once cannot interleave.
 *   - Cross-thread updates must not do load-modify-store of the WHOLE struct
 *     (config_copy ... config_apply): that loses what another thread changed
 *     in between.  Use config_modify and touch only the fields you own.
 */

#include <wchar.h>

#define CONFIG_MAX_PATH 260

typedef struct PluginConfig {
    /* Pos.txt location: manual path + auto-detected path + which one to use. */
    wchar_t savedPath[CONFIG_MAX_PATH];          /* manual: ...\ConanSandbox\Saved */
    wchar_t automaticSavedPath[CONFIG_MAX_PATH]; /* auto-detected variant */
    int automaticPatchFind;                      /* 1 = prefer automatic path */

    /* Voice distances in meters. */
    float distanceWhisper;
    float distanceNormal;
    float distanceShout;

    /* Hotkeys (virtual key codes). */
    int whisperKey;
    int normalKey;
    int shoutKey;
    int voiceToggleKey;
    int configUIKey;

    /* Feature toggles. */
    int enableDistanceMuting;
    int enableAutomaticChannelChange;
    int enableVoiceToggle;

    /* Voice overlay (HUD). */
    int enableVoiceOverlay;
    int hudTheme;
    int hudPosition;
    int hudSize;

    /* Debug logging (log_debug on/off). */
    int debugMode;

    /* Pos.txt file reader: 1 = read Pos.txt (default), 0 = HTTP-only (skip file). */
    int enablePosFile;

    /* Server whose [DEFAULT_SETTINGS] were already applied (virtual server
       unique identifier). Defaults are applied ONCE per server; afterwards
       the user's own key/distance changes are never overridden again. */
    char defaultsAppliedServer[128];
} PluginConfig;

/* Global config instance.  Writers (config_load / config_apply /
   config_modify) hold the internal lock.  Other threads may read SCALAR
   fields directly (4-byte aligned loads are atomic on x86/x64); anyone
   reading the STRING fields (savedPath/automaticSavedPath) must use
   config_copy to avoid torn strings while another thread rewrites them. */
extern PluginConfig g_config;

/* Replace g_config (whole struct) under the internal lock.  Any thread.
   Prefer config_modify when only some fields are yours. */
void config_apply(const PluginConfig* src);

/* Consistent snapshot of g_config (for string fields). Any thread. */
void config_copy(PluginConfig* out);

/* Change g_config in place: runs fn(&g_config, ctx) under the config lock,
   then config_clamp.  Does NOT save - call config_save() afterwards when the
   change must persist.  Any thread.  fn must be short, must only touch the
   fields its caller owns, and must not call config_* itself or the TS API. */
typedef void (*ConfigMutator)(PluginConfig* cfg, void* ctx);
void config_modify(ConfigMutator fn, void* ctx);

/* Documents\Conan Exiles TeamSpeak plugin (created if missing). NULL on failure. */
const wchar_t* config_get_folder_path(void);

/* Fill g_config with defaults, then overlay values found in plugin.cfg.
   Creates the file with defaults when missing. Returns 1 on success. */
int config_load(void);

/* Write current g_config to plugin.cfg. Any thread: serialized (own save lock)
   and atomic (temp file + rename).  Returns 1 on success; on failure returns 0
   and the previous plugin.cfg is left untouched.  Non-ASCII paths are stored
   so that config_load reads them back (see core/config/config_text.h). */
int config_save(void);

/* Clamp numeric fields into safe ranges (used after load and after the
   settings dialog). Pure, any thread. */
void config_clamp(PluginConfig* cfg);

#endif /* CORE_CONFIG_CONFIG_H */
