#include "ui/config/ui_config_state.h"

#include "ui/config/ui_config_logic.h"
#include "core/config/range_units.h"
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

/* HUD options that are currently "saved": the values at dialog open, replaced
   by the working copy's values after every commit. The HUD live preview
   (ui_cfg_hud_preview) pushes unsaved combo choices into g_config; closing the
   dialog without saving puts these values back. UI thread only. */
static int s_hudSavedTheme = 0;
static int s_hudSavedPosition = 0;
static int s_hudSavedSize = 0;

/* 091: unit the dialog's distance fields are rendered in (1 = foundations).
   Changes only through ui_range_unit_refresh(); the texts of the fields and the
   code that parses them back therefore always agree. UI thread only. */
static int s_dialogUnit = 0;

typedef struct UiHudValues {
    int theme;
    int position;
    int size;
} UiHudValues;

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

/* ConfigMutator: write the three HUD options into the live g_config. */
static void ui_cfg_hud_mutator(PluginConfig* g, void* ctx) {
    const UiHudValues* v = (const UiHudValues*)ctx;
    g->hudTheme = v->theme;
    g->hudPosition = v->position;
    g->hudSize = v->size;
}

void ui_cfg_dialog_begin(void) {
    config_copy(&s_f10Cfg);
    s_dialogUnit = range_effective_unit(s_f10Cfg.rangeInFoundations, ui_ranges_forced_by_server());
    s_hudSavedTheme = s_f10Cfg.hudTheme;
    s_hudSavedPosition = s_f10Cfg.hudPosition;
    s_hudSavedSize = s_f10Cfg.hudSize;
    InterlockedExchange(&s_f10DialogActive, 1);
    ui_cfg_publish_legacy_mirrors();
}

void ui_cfg_dialog_end(void) {
    /* Idempotent: only the call that really closes the dialog restores. */
    if (!InterlockedExchange(&s_f10DialogActive, 0)) {
        return;
    }

    /* Closed without Save: undo a HUD live preview that was never committed. */
    PluginConfig cur;
    config_copy(&cur);
    if (cur.hudTheme != s_hudSavedTheme || cur.hudPosition != s_hudSavedPosition
        || cur.hudSize != s_hudSavedSize) {
        UiHudValues saved = { s_hudSavedTheme, s_hudSavedPosition, s_hudSavedSize };
        config_modify(ui_cfg_hud_mutator, &saved);
        voice_overlay_refresh_theme();
        voice_overlay_refresh_position();
        voice_overlay_refresh_size();
    }
}

void ui_cfg_hud_preview(unsigned parts) {
    if (!InterlockedCompareExchange(&s_f10DialogActive, 0, 0)) {
        return;
    }
    UiHudValues v = { s_f10Cfg.hudTheme, s_f10Cfg.hudPosition, s_f10Cfg.hudSize };
    config_modify(ui_cfg_hud_mutator, &v);
    if (parts & UI_HUD_PART_THEME) {
        voice_overlay_refresh_theme();
    }
    if (parts & UI_HUD_PART_POSITION) {
        voice_overlay_refresh_position();
    }
    if (parts & UI_HUD_PART_SIZE) {
        voice_overlay_refresh_size();
    }
}

int ui_ranges_forced_by_server(void) {
    return (hubDescriptionAvailable && hubForceRangesInFoundations) ? 1 : 0;
}

int ui_ranges_in_foundations(void) {
    return s_dialogUnit;
}

int ui_range_unit_refresh(void) {
    const int unit = range_effective_unit(ui_cfg()->rangeInFoundations, ui_ranges_forced_by_server());
    if (unit == s_dialogUnit) {
        return 0;
    }
    s_dialogUnit = unit;
    return 1;
}

void ui_range_text(wchar_t* out, size_t cap, float meters) {
    swprintf(out, cap, L"%.1f", (double)range_from_meters(meters, ui_ranges_in_foundations()));
}

void ui_range_transport_text(wchar_t* out, size_t cap, float meters) {
    if (ui_ranges_in_foundations()) {
        swprintf(out, cap, L"%.9g", (double)meters);
    }
    else {
        swprintf(out, cap, L"%.1f", (double)meters);
    }
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

/* ConfigMutator: copy the dialog-owned fields of the working copy into g_config. */
static void ui_cfg_commit_mutator(PluginConfig* g, void* ctx) {
    ui_cfg_merge_dialog_fields(g, (const PluginConfig*)ctx);
}

void ui_cfg_commit(void) {
    if (InterlockedCompareExchange(&s_f10DialogActive, 0, 0)) {
        /* Merge ONLY what the dialog edits. The working copy is a snapshot from
           dialog open; copying all of it (config_apply) used to overwrite newer
           DefaultsAppliedServer / AutomaticSavedPath values written meanwhile by
           other threads. */
        config_clamp(&s_f10Cfg);
        const PluginConfig work = s_f10Cfg;
        config_modify(ui_cfg_commit_mutator, (void*)&work);

        /* These HUD values are now permanent: Cancel must not revert them. */
        s_hudSavedTheme = work.hudTheme;
        s_hudSavedPosition = work.hudPosition;
        s_hudSavedSize = work.hudSize;
    }
    config_save();

    PluginConfig now;
    config_copy(&now);
    log_set_enabled(now.debugMode);

    /* No voice_mode_apply() here: the dialog never changes the voice mode, and
       re-applying the stale currentVoiceMode mirror could only revert a mode
       the player switched by hotkey while the dialog was open. */
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_request_recompute_all();

    ui_cfg_publish_legacy_mirrors();
}
