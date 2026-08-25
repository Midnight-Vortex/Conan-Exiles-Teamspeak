#ifndef CORE_CONFIG_CONFIG_H
#define CORE_CONFIG_CONFIG_H

/*
 * plugin.cfg — Documents\Conan Exiles TeamSpeak plugin\plugin.cfg
 *
 * Thread contract: config_load/config_save are called from the TS callback
 * thread (init, settings dialog). The values struct is written only there;
 * other threads read it after init. No locks needed for Phase 1.
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

/* Global config instance — written on the callback thread only (via
   config_load / config_apply). Other threads may read SCALAR fields
   directly (4-byte aligned loads are atomic on x86/x64); anyone reading
   the STRING fields (savedPath/automaticSavedPath) must use config_copy
   to avoid torn strings while the settings dialog applies changes. */
extern PluginConfig g_config;

/* Replace g_config under the internal lock. TS callback thread. */
void config_apply(const PluginConfig* src);

/* Consistent snapshot of g_config (for string fields). Any thread. */
void config_copy(PluginConfig* out);

/* Documents\Conan Exiles TeamSpeak plugin (created if missing). NULL on failure. */
const wchar_t* config_get_folder_path(void);

/* Fill g_config with defaults, then overlay values found in plugin.cfg.
   Creates the file with defaults when missing. Returns 1 on success. */
int config_load(void);

/* Write current g_config to plugin.cfg. Returns 1 on success. */
int config_save(void);

/* Clamp numeric fields into safe ranges (used after load and after the
   settings dialog). Pure, any thread. */
void config_clamp(PluginConfig* cfg);

#endif /* CORE_CONFIG_CONFIG_H */
