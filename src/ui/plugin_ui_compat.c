#include "ui/plugin_ui_compat.h"
#include "core/util/log.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/zone_resolve.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ui/input/key_watcher.h"
#include "plugin_modules.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static mumble_error_t compat_mumble_log(mumble_plugin_id_t callerID, const char* message) {
    (void)callerID;
    if (message) {
        log_write("%s", message);
    }
    return MUMBLE_STATUS_OK;
}

static int hud_pos_from_legacy(int legacy) {
    switch (legacy) {
    case VOICE_HUD_POSITION_TOP_RIGHT: return 1;
    case VOICE_HUD_POSITION_BOTTOM_LEFT: return 2;
    case VOICE_HUD_POSITION_BOTTOM_RIGHT: return 3;
    case VOICE_HUD_POSITION_TOP_CENTER: return 4;
    default: return 0;
    }
}

static int hud_pos_to_legacy(int cfg) {
    switch (cfg) {
    case 1: return VOICE_HUD_POSITION_TOP_RIGHT;
    case 2: return VOICE_HUD_POSITION_BOTTOM_LEFT;
    case 3: return VOICE_HUD_POSITION_BOTTOM_RIGHT;
    case 4: return VOICE_HUD_POSITION_TOP_CENTER;
    default: return VOICE_HUD_POSITION_TOP_LEFT;
    }
}
// Background image
HBITMAP hBackgroundBitmap = NULL;
HBITMAP hBackgroundAdvancedBitmap = NULL;
HBITMAP hBackgroundPresetsBitmap = NULL;
HBITMAP hBackgroundSavePresetBitmap = NULL;
HBITMAP hBackgroundRenamePresetBitmap = NULL;
BOOL backgroundDrawn = FALSE;

// Plugin control variables | Variables de contrôle du plugin
BOOL enableGetPlayerCoordinates = TRUE;
BOOL TEMP = FALSE;
BOOL enableAutomaticPatchFind = FALSE;
HWND hAutomaticPatchFindCheck = NULL;

// F9 coordinate broadcast variables | Variables pour la diffusion des coordonnées en F9
BOOL f9CoordinateBroadcastActive = FALSE;
ULONGLONG lastCoordinateBroadcast = 0;

// Log control variables | Variables pour contrôler l'activation des logs
BOOL enableLogCoordinates = FALSE;
BOOL enableLogModFile = FALSE;
BOOL enableLogConfig = FALSE;
BOOL enableLogGeneral = FALSE;

// Mumble API interface | Interface API Mumble
struct MumbleAPI_v_1_0_x mumbleAPI;
mumble_plugin_id_t ownID;

// Audio distance variables | Variables de distance audio
double serverMaximumAudioDistance = 45.0;
BOOL maxAudioDistanceRetrieved = FALSE;
ULONGLONG lastMaxDistanceCheck = 0;

// Overlay border highlight variables | Variables de surbrillance de l'overlay
BOOL overlayBorderHighlight = FALSE;
mumble_userid_t overlayHighlightUserID = 0;
char overlaySpeakerText[128] = "";
CRITICAL_SECTION overlayTextLock;

// Channel management variables | Variables de gestion des canaux
mumble_channelid_t hubChannelID = -1;
mumble_channelid_t rootChannelID = -1;
mumble_channelid_t ingameChannelID = -1;
mumble_channelid_t lastTargetChannel = -1;
mumble_channelid_t lastValidChannel = -1;
BOOL channelManagementActive = FALSE;
BOOL enableAutomaticChannelChange = FALSE;
ULONGLONG lastChannelCheck = 0;

// Player position variables | Variables de position du joueur
float axe_x = 0.0f;
float axe_y = 0.0f;
float axe_z = 0.0f;
float avatarAxisX = 0.0f;
float avatarAxisY = 0.0f;
float avatarAxisZ = 0.0f;

// Adaptive mod system variables | Variables pour le système de Mod adaptatif
time_t lastFileCheck = 0;
time_t LastFileModification = 0;
int lastSeq = -1;
BOOL modDataValid = FALSE;
char modFilePath[MAX_PATH] = "";
BOOL coordinatesValid = FALSE;
struct ModFileData currentModData = { 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, FALSE };
ULONGLONG lastModDataTick = 0;

// Global variables for the interface | Variables globales pour l'interface
HWND hConfigDialog = NULL;
HWND hWhisperKeyEdit, hNormalKeyEdit, hShoutKeyEdit, hConfigKeyEdit;
HWND hWhisperButton, hNormalButton, hShoutButton, hConfigButton;
HWND hEnableDistanceMutingCheck, hEnableAutomaticChannelChangeCheck;
HWND hDistanceWhisperEdit, hDistanceNormalEdit, hDistanceShoutEdit;
HWND hSavedPathEdit, hSavedPathButton, hSavedPathBg;
HBITMAP hPathBoxBitmap = NULL;
HWND hCategoryPatch, hCategoryAdvanced;
HWND hEnableVoiceToggleCheck, hVoiceToggleKeyEdit, hVoiceToggleButton;
HWND hHudThemeLabel = NULL, hHudThemeCombo = NULL;
HWND hHudPositionLabel = NULL, hHudPositionCombo = NULL;
HWND hHudSizeLabel = NULL, hHudSizeCombo = NULL; /* Advanced tab: overlay color theme UI
                                                       Onglet avancé : UI couleur de l'overlay */
VoiceRangePreset voicePresets[MAX_VOICE_PRESETS];
int currentPresetIndex = -1;
uint8_t currentVoiceMode = 1; // 0: Whisper, 1: Normal, 2: Shout | 0: Murmure, 1: Normal, 2: Cri
HWND hCategoryPresets = NULL;
HWND hPresetLabels[MAX_VOICE_PRESETS] = { NULL };
HWND hPresetLoadButtons[MAX_VOICE_PRESETS] = { NULL };
HWND hPresetRenameButtons[MAX_VOICE_PRESETS] = { NULL };
HWND hPresetSaveDialog = NULL;
HWND hPresetRenameDialog = NULL;
char renameBuffer[PRESET_NAME_MAX_LENGTH] = "";
int renamePresetIndex = -1;
HWND hStatusMessage = NULL;
HWND hDistanceLimitMessage = NULL;
HFONT hFont = NULL, hFontBold = NULL, hFontLarge = NULL, hFontEmoji = NULL;
HFONT hPathFont = NULL;

// Interface message controls | Contrôles de messages de l'interface
HWND hDistanceWhisperMessage = NULL;
HWND hDistanceNormalMessage = NULL;
HWND hDistanceShoutMessage = NULL;
HWND hDistanceMutingMessage = NULL;
HWND hChannelSwitchingMessage = NULL;
HWND hPositionalAudioMessage = NULL;
HWND hDistanceServerLimitWhisper = NULL;
HWND hDistanceServerLimitNormal = NULL;
HWND hDistanceServerLimitShout = NULL;

// Interface state variables | Variables d'état de l'interface
int currentCategory = 1;
BOOL isCapturingKey = FALSE;
int captureKeyTarget = 0;
wchar_t savedPath[MAX_PATH] = L""; 
wchar_t displayedPathText[MAX_PATH] = L"";
BOOL isUpdatingInterface = FALSE;
ULONGLONG lastInterfaceUpdate = 0;

// Voice toggle variables | Variables pour le toggle de voix
int voiceToggleKey = 84;
BOOL enableVoiceToggle = TRUE;
ULONGLONG lastVoiceTogglePress = 0;

// Key bindings | Raccourcis clavier
int whisperKey = 97;
int normalKey = 98;
int shoutKey = 99;
int configUIKey = 121;

// Key monitoring variables | Variables de surveillance des touches globales
BOOL isConfigDialogOpen = FALSE;
DWORD lastKeyPressTime = 0;
BOOL keyMonitorThreadRunning = FALSE;
HANDLE keyMonitorThread = NULL;
BOOL lastKeyState = FALSE;

// Thread stop flags | Flags d'arrêt des threads
BOOL modFileWatcherRunning = FALSE;
BOOL voiceSystemRunning = FALSE;
BOOL channelManagementRunning = FALSE;
BOOL hubDescriptionMonitorRunning = FALSE;
BOOL pluginShuttingDown = FALSE;
static volatile LONG overlayTextLockInitialized = 0;

int plugin_overlay_text_lock_try(void) {
    if (pluginShuttingDown) {
        return 0;
    }
    if (!InterlockedCompareExchange(&overlayTextLockInitialized, 0, 0)) {
        return 0;
    }
    return TryEnterCriticalSection(&overlayTextLock) ? 1 : 0;
}

void plugin_overlay_text_lock_release(void) {
    LeaveCriticalSection(&overlayTextLock);
}

double hubMinimumWhisper = 0.0;
double hubMaximumWhisper = 5.0;
double hubMinimumNormal = 5.0;
double hubMaximumNormal = 15.0;
double hubMinimumShout = 15.0;
double hubMaximumShout = 50.0;
BOOL hubForceDistanceBasedMuting = TRUE;
BOOL hubForceAutomaticChannelSwitching = TRUE;

CompletePositionalData localVoiceData = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f, "" };
CompletePositionalData remotePlayersData[64];
size_t remotePlayerCount = 0;
ULONGLONG lastVoiceDataSent = 0;
ULONGLONG lastKeyCheck = 0;

float distanceWhisper = 3.0f;
float distanceNormal = 13.0f;
float distanceShout = 26.0f;
BOOL enableDistanceMuting = FALSE;

HWND hVoiceOverlay = NULL;
HWND hVoiceText = NULL;
BOOL enableVoiceOverlay = TRUE;
int voiceHudTheme = VOICE_HUD_THEME_GRAY;
int voiceHudPosition = VOICE_HUD_POSITION_BOTTOM_RIGHT;
int voiceHudSize = VOICE_HUD_SIZE_BIG;
HFONT hOverlayFont = NULL;
BOOL overlayThreadRunning = FALSE;

PlayerMuteState playerMuteStates[64];
size_t playerMuteStateCount = 0;
ULONGLONG lastDistanceCheck = 0;
BOOL forceGlobalMuteRefresh = FALSE;
ULONGLONG lastGlobalRefresh = 0;
BOOL enableAutoAudioSettings = TRUE;
ULONGLONG lastAudioSettingsApply = 0;

Race races[MAX_RACES];
size_t raceCount = 0;
int currentPlayerRaceIndex = -1;
float currentListenAddDistance = 0.0f;
uint64_t steamID = 0;

Zone zones[MAX_ZONES];
size_t zoneCount = 0;
int currentZoneIndex = -1;

AdaptivePlayerData adaptivePlayerStates[64];
size_t adaptivePlayerCount = 0;
Vector3 localPlayerPosition = { 0.0f, 0.0f, 0.0f };
AudioVolumeState audioVolumeStates[64];
size_t audioVolumeCount = 0;

#ifdef CONAN_EXILES_TS_EXPORTS
mumble_channelid_t ts3LocalChannelID = -1;
#endif

void plugin_ui_sync_from_config(void) {
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
    voiceHudPosition = hud_pos_to_legacy(cfg.hudPosition);
    voiceHudSize = cfg.hudSize;
    wcsncpy_s(savedPath, MAX_PATH, cfg.savedPath, _TRUNCATE);
    currentVoiceMode = (uint8_t)voice_mode_get_current();
    localVoiceData.voiceDistance = voice_mode_get_current_distance();
}

void plugin_ui_sync_to_config(void) {
    PluginConfig cfg;
    config_copy(&cfg);
    cfg.distanceWhisper = distanceWhisper;
    cfg.distanceNormal = distanceNormal;
    cfg.distanceShout = distanceShout;
    cfg.whisperKey = whisperKey;
    cfg.normalKey = normalKey;
    cfg.shoutKey = shoutKey;
    cfg.voiceToggleKey = voiceToggleKey;
    cfg.configUIKey = configUIKey;
    cfg.enableDistanceMuting = enableDistanceMuting ? 1 : 0;
    cfg.enableAutomaticChannelChange = enableAutomaticChannelChange ? 1 : 0;
    cfg.enableVoiceToggle = enableVoiceToggle ? 1 : 0;
    cfg.enableVoiceOverlay = enableVoiceOverlay ? 1 : 0;
    cfg.automaticPatchFind = enableAutomaticPatchFind ? 1 : 0;
    cfg.debugMode = enableLogGeneral ? 1 : 0;
    cfg.hudTheme = voiceHudTheme;
    cfg.hudPosition = hud_pos_from_legacy(voiceHudPosition);
    cfg.hudSize = voiceHudSize;
    wcsncpy_s(cfg.savedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
    config_clamp(&cfg);
    config_apply(&cfg);
    config_save();
    log_set_enabled(cfg.debugMode);
    voice_mode_apply((VoiceMode)currentVoiceMode);
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
}

void plugin_ui_sync_live_state(void) {
    coordinatesValid = pos_coordinates_valid() ? TRUE : FALSE;
    isConnectedToServer = ts3_is_connected() ? TRUE : FALSE;
    HubSettings hub;
    if (server_profile_get(&hub) && hub.valid) {
        hubDescriptionAvailable = TRUE;
        hubLimitsActive = hub.forceDistanceMuting ? TRUE : FALSE;
        hubForceDistanceBasedMuting = hub.forceDistanceMuting ? TRUE : FALSE;
        hubForceAutomaticChannelSwitching = hub.forceAutoChannelSwitch ? TRUE : FALSE;
        hubForcePositionalAudio = hub.forceDistanceMuting ? TRUE : FALSE;
        hubMinimumWhisper = hub.minWhisper;
        hubMaximumWhisper = hub.maxWhisper;
        hubMinimumNormal = hub.minNormal;
        hubMaximumNormal = hub.maxNormal;
        hubMinimumShout = hub.minShout;
        hubMaximumShout = hub.maxShout;
        hubAudioMinDistance = hub.audioMinDistance > 0.0f ? hub.audioMinDistance : 1.0;
        hubAudioMaxVolume = hub.audioMaxVolume;
        zoneCount = (size_t)hub.zoneCount;
        if (zoneCount > MAX_ZONES) {
            zoneCount = MAX_ZONES;
        }
        for (size_t i = 0; i < zoneCount; i++) {
            const HubZone* z = &hub.zones[i];
            Zone* dst = &zones[i];
            snprintf(dst->name, sizeof(dst->name), "%s", z->name);
            dst->x1 = z->x1; dst->z1 = z->z1; dst->x2 = z->x2; dst->z2 = z->z2;
            dst->x3 = z->x3; dst->z3 = z->z3; dst->x4 = z->x4; dst->z4 = z->z4;
            dst->groundY = z->groundY; dst->topY = z->topY;
            dst->whisperDist = z->whisperDist; dst->normalDist = z->normalDist; dst->shoutDist = z->shoutDist;
            dst->audioMinDistance = z->audioMinDistance;
            dst->audioMaxVolume = hub.audioMaxVolume;
            dst->isSoundproof = z->soundproof ? TRUE : FALSE;
            dst->isReverb = z->reverb ? TRUE : FALSE;
        }
        PosSample local;
        if (pos_get_current(&local)) {
            currentZoneIndex = zone_resolve(&hub, local.x / 100.0f, local.y / 100.0f, local.z / 100.0f);
        }
        else {
            currentZoneIndex = -1;
        }
    }
    else {
        hubDescriptionAvailable = FALSE;
        hubLimitsActive = FALSE;
        zoneCount = 0;
        currentZoneIndex = -1;
    }
    currentVoiceMode = (uint8_t)voice_mode_get_current();
}

float getVoiceDistanceForMode(uint8_t voiceMode) {
    if (voiceMode > 2) {
        voiceMode = 1;
    }
    if (currentZoneIndex >= 0 && (size_t)currentZoneIndex < zoneCount) {
        switch (voiceMode) {
        case 0: return zones[currentZoneIndex].whisperDist > 0.0f ? zones[currentZoneIndex].whisperDist : distanceWhisper;
        case 2: return zones[currentZoneIndex].shoutDist > 0.0f ? zones[currentZoneIndex].shoutDist : distanceShout;
        default: return zones[currentZoneIndex].normalDist > 0.0f ? zones[currentZoneIndex].normalDist : distanceNormal;
        }
    }
    switch (voiceMode) {
    case 0: return distanceWhisper;
    case 2: return distanceShout;
    default: return distanceNormal;
    }
}

void plugin_ui_on_position_tick(void) {
    plugin_ui_sync_live_state();
}

void plugin_ui_init(void) {
    mumbleAPI.log = compat_mumble_log;
    ownID = 1;
    InitializeCriticalSection(&overlayTextLock);
    InterlockedExchange(&overlayTextLockInitialized, 1);
    plugin_ui_sync_from_config();
    plugin_ui_sync_live_state();
    initializeVoicePresets();
    loadPresetsFromConfigFile();
}

void plugin_ui_shutdown(void) {
    pluginShuttingDown = TRUE;
}

void overlay_start(void) {
    if (!enableVoiceOverlay) {
        return;
    }
    createVoiceOverlay();
    installKeyMonitoring();
}

void overlay_stop(void) {
    removeKeyMonitoring();
    plugin_destroy_voice_overlay_safely();
    if (InterlockedCompareExchange(&overlayTextLockInitialized, 0, 0)) {
        DeleteCriticalSection(&overlayTextLock);
        InterlockedExchange(&overlayTextLockInitialized, 0);
    }
}

int ts3_plugin_is_on_callback_thread(void) {
    return ts3_thread_is_callback();
}

void ts3_debug_log(const char* message) {
    if (message) {
        log_debug("%s", message);
    }
}

void ts3_debug_logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_debug("%s", buf);
}

int ts3_adapter_is_connected(void) {
    return ts3_is_connected();
}

void ts3_adapter_print_chat(const char* message) {
    (void)message;
}

void ts3_adapter_print_chat_force(const char* message) {
    (void)message;
}

void ts3_adapter_request_chat_wakeup(void) {
    ts3_request_wakeup();
}

int ts3_plugin_is_proximity_active(void) {
    return ts3_is_connected() && enableDistanceMuting;
}

void ts3_plugin_apply_proximity_volumes_force(void) {
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
    if (ts3_is_connected()) {
        ts3_request_wakeup();
    }
}

void plugin_ui_on_settings_saved(void) {
    PluginConfig cfg;
    config_copy(&cfg);
    cfg.distanceWhisper = distanceWhisper;
    cfg.distanceNormal = distanceNormal;
    cfg.distanceShout = distanceShout;
    cfg.whisperKey = whisperKey;
    cfg.normalKey = normalKey;
    cfg.shoutKey = shoutKey;
    cfg.voiceToggleKey = voiceToggleKey;
    cfg.configUIKey = configUIKey;
    cfg.enableDistanceMuting = enableDistanceMuting ? 1 : 0;
    cfg.enableAutomaticChannelChange = enableAutomaticChannelChange ? 1 : 0;
    cfg.enableVoiceToggle = enableVoiceToggle ? 1 : 0;
    cfg.enableVoiceOverlay = enableVoiceOverlay ? 1 : 0;
    cfg.automaticPatchFind = enableAutomaticPatchFind ? 1 : 0;
    cfg.debugMode = enableLogGeneral ? 1 : 0;
    cfg.hudTheme = voiceHudTheme;
    cfg.hudPosition = hud_pos_from_legacy(voiceHudPosition);
    cfg.hudSize = voiceHudSize;
    wcsncpy_s(cfg.savedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
    config_clamp(&cfg);
    config_apply(&cfg);
    log_set_enabled(cfg.debugMode);
    voice_mode_apply((VoiceMode)currentVoiceMode);
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
    voice_overlay_refresh_theme();
    voice_overlay_refresh_position();
    voice_overlay_refresh_size();
    updateVoiceOverlayVisibility();
}

unsigned long overlay_ui_thread_id(void) {
    return 0;
}
