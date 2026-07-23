#include "ui/config/ui_config_state.h"

#include "plugin_internal.h"
#include "ui/plugin_ui_compat.h"
#include "core/util/log.h"
#include "core/voice/voice_modes.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/adapter/ts3_adapter.h"

#include <string.h>
#include <wchar.h>

static PluginConfig s_f10Cfg;
static volatile long s_f10DialogActive = 0;

PluginConfig* ui_cfg(void) {
    if (InterlockedCompareExchange(&s_f10DialogActive, 0, 0)) {
        return &s_f10Cfg;
    }
    return &g_config;
}

void ui_cfg_publish_legacy_mirrors(void) {
    PluginConfig cfg;
    config_copy(&cfg);

    distanceWhisper = cfg.distanceWhisper;
    distanceNormal = cfg.distanceNormal;
    distanceShout = cfg.distanceShout;
    whisperKey = cfg.whisperKey;
    normalKey = cfg.normalKey;
    shoutKey = cfg.shoutKey;
    voiceToggleKey = cfg.voiceToggleKey;
    configUIKey = cfg.configUIKey;
    enableDistanceMuting = cfg.enableDistanceMuting ? TRUE : FALSE;
    enableAutomaticChannelChange = cfg.enableAutomaticChannelChange ? TRUE : FALSE;
    enableVoiceToggle = cfg.enableVoiceToggle ? TRUE : FALSE;
    enableVoiceOverlay = cfg.enableVoiceOverlay ? TRUE : FALSE;
    enableAutomaticPatchFind = cfg.automaticPatchFind ? TRUE : FALSE;
    enableLogGeneral = cfg.debugMode ? TRUE : FALSE;
    voiceHudTheme = cfg.hudTheme;
    voiceHudPosition = cfg.hudPosition;
    voiceHudSize = cfg.hudSize;

    ui_cfg_get_active_saved_path(savedPath, MAX_PATH);

    currentVoiceMode = (uint8_t)voice_mode_get_current();
    localVoiceData.voiceDistance = voice_mode_get_current_distance();
}

void ui_cfg_dialog_begin(void) {
    config_copy(&s_f10Cfg);
    InterlockedExchange(&s_f10DialogActive, 1);
    ui_cfg_publish_legacy_mirrors();
}

void ui_cfg_dialog_end(void) {
    InterlockedExchange(&s_f10DialogActive, 0);
}

void ui_cfg_get_active_saved_path(wchar_t* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    out[0] = L'\0';

    PluginConfig cfg;
    if (InterlockedCompareExchange(&s_f10DialogActive, 0, 0)) {
        cfg = s_f10Cfg;
    }
    else {
        config_copy(&cfg);
    }

    if (cfg.automaticPatchFind && cfg.automaticSavedPath[0]) {
        wcsncpy_s(out, outLen, cfg.automaticSavedPath, _TRUNCATE);
    }
    else if (cfg.savedPath[0]) {
        wcsncpy_s(out, outLen, cfg.savedPath, _TRUNCATE);
    }
}

void ui_cfg_set_active_saved_path_full(const wchar_t* savedPathFull) {
    if (!savedPathFull || !savedPathFull[0]) {
        return;
    }
    if (wcsstr(savedPathFull, L"(Not configured)") != NULL) {
        return;
    }

    PluginConfig* cfg = ui_cfg();
    if (cfg->automaticPatchFind) {
        wcsncpy_s(cfg->automaticSavedPath, CONFIG_MAX_PATH, savedPathFull, _TRUNCATE);
    }
    else {
        wcsncpy_s(cfg->savedPath, CONFIG_MAX_PATH, savedPathFull, _TRUNCATE);
    }
}

void ui_cfg_commit(void) {
    PluginConfig* cfg = ui_cfg();
    config_clamp(cfg);
    config_apply(cfg);
    config_save();
    log_set_enabled(cfg->debugMode);

    if ((VoiceMode)currentVoiceMode != voice_mode_get_current()) {
        voice_mode_apply((VoiceMode)currentVoiceMode);
    }
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_request_recompute_all();

    ui_cfg_publish_legacy_mirrors();
}
