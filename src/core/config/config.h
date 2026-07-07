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
    int hudTheme;
    int hudPosition;
    int hudSize;

    /* Debug logging (log_debug on/off). */
    int debugMode;
} PluginConfig;

/* Global config instance — written on callback thread only. */
extern PluginConfig g_config;

/* Documents\Conan Exiles TeamSpeak plugin (created if missing). NULL on failure. */
const wchar_t* config_get_folder_path(void);

/* Fill g_config with defaults, then overlay values found in plugin.cfg.
   Creates the file with defaults when missing. Returns 1 on success. */
int config_load(void);

/* Write current g_config to plugin.cfg. Returns 1 on success. */
int config_save(void);

#endif /* CORE_CONFIG_CONFIG_H */
