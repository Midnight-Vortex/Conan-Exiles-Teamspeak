#include "ui/plugin_ui_compat.h"
#include "core/util/log.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/zone_resolve.h"
#include "core/channel/channel_manage.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ui/overlay/voice_overlay.h"
#include "plugin_modules.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <process.h>

static mumble_error_t compat_mumble_log(mumble_plugin_id_t callerID, const char* message) {
    (void)callerID;
    if (message) {
        log_write("%s", message);
    }
    return MUMBLE_STATUS_OK;
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

BOOL isConnectedToServer = FALSE;
BOOL hubDescriptionAvailable = FALSE;
BOOL hubLimitsActive = FALSE;
ULONGLONG lastConnectionCheck = 0;
char serverConfigHash[256] = "";
BOOL hasAppliedDefaultSettings = FALSE;
BOOL enableDefaultSettingsOnFirstConnection = TRUE;
int defaultWhisperKey = 97;
int defaultNormalKey = 98;
int defaultShoutKey = 99;
int defaultVoiceToggleKey = 84;
float defaultDistanceWhisper = 3.0f;
float defaultDistanceNormal = 13.0f;
float defaultDistanceShout = 26.0f;
double hubAudioMinDistance = 2.0;
double hubAudioMaxDistance = 50.0;
double hubAudioMaxVolume = 85.0;
double hubAudioBloom = 0.0;
double hubAudioFilterIntensity = 0.0;
BOOL hubForcePositionalAudio = FALSE;
ULONGLONG lastHubDescriptionCheck = 0;
char* lastHubDescriptionCache = NULL;
const wchar_t* infoText1 = L"";
const wchar_t* infoText2 = L"";
const wchar_t* infoText3 = L"";

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
    voiceHudPosition = cfg.hudPosition;
    voiceHudSize = cfg.hudSize;
    /* Legacy savedPath global = ACTIVE Pos.txt base path (same selection as
       pos_resolve_file_path): automatic path when enabled and set, else manual. */
    if (cfg.automaticPatchFind && cfg.automaticSavedPath[0]) {
        wcsncpy_s(savedPath, MAX_PATH, cfg.automaticSavedPath, _TRUNCATE);
    }
    else {
        wcsncpy_s(savedPath, MAX_PATH, cfg.savedPath, _TRUNCATE);
    }
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
    cfg.hudPosition = voiceHudPosition;
    cfg.hudSize = voiceHudSize;
    /* Active-path routing — see plugin_ui_on_settings_saved. */
    if (savedPath[0] && wcsstr(savedPath, L"(Not configured)") == NULL) {
        if (enableAutomaticPatchFind) {
            wcsncpy_s(cfg.automaticSavedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
        }
        else {
            wcsncpy_s(cfg.savedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
        }
    }
    config_clamp(&cfg);
    config_apply(&cfg);
    config_save();
    log_set_enabled(cfg.debugMode);
    /* Only a REAL mode change goes through voice_mode_apply (chat notify);
       plain saves just refresh distances silently. */
    if ((VoiceMode)currentVoiceMode != voice_mode_get_current()) {
        voice_mode_apply((VoiceMode)currentVoiceMode);
    }
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
}

void plugin_ui_sync_live_state(void) {
    coordinatesValid = pos_coordinates_valid() ? TRUE : FALSE;
    isConnectedToServer = ts3_is_connected() ? TRUE : FALSE;

    PosSample localPos;
    if (pos_get_current(&localPos)) {
        axe_x = localPos.x;
        axe_y = localPos.y;
        axe_z = localPos.z;
        localPlayerPosition.x = localPos.x / 100.0f;
        localPlayerPosition.y = localPos.y / 100.0f;
        localPlayerPosition.z = localPos.z / 100.0f;
    }

    const uint64 hubId = chan_get_hub_channel_id();
    const uint64 ingameId = chan_get_ingame_channel_id();
    hubChannelID = hubId ? (mumble_channelid_t)hubId : -1;
    ingameChannelID = ingameId ? (mumble_channelid_t)ingameId : -1;

    HubSettings hub;
    if (server_profile_get(&hub) && hub.valid) {
        hubDescriptionAvailable = TRUE;
        rootChannelID = 1;
        channelManagementActive = TRUE;
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
        /* Legacy globals keep percent semantics (85.0 = 85%); the new
           HubSettings stores a gain factor (0.85). */
        hubAudioMaxVolume = hub.audioMaxVolume * 100.0;
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
            dst->audioMaxVolume = (z->audioMaxVolume > 0.0f
                ? z->audioMaxVolume : hub.audioMaxVolume) * 100.0;
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

        /* Races for the F10 dialog / HUD. The local player's race replaces
           the hub limits (same behavior as the old plugin). */
        steamID = server_profile_get_local_steam_id();
        raceCount = (size_t)hub.raceCount;
        if (raceCount > MAX_RACES) {
            raceCount = MAX_RACES;
        }
        currentPlayerRaceIndex = -1;
        for (size_t i = 0; i < raceCount; i++) {
            const HubRace* src = &hub.races[i];
            Race* dst = &races[i];
            memset(dst, 0, sizeof(*dst));
            snprintf(dst->name, sizeof(dst->name), "%s", src->name);
            dst->steamIDCount = (size_t)src->steamIDCount;
            if (dst->steamIDCount > MAX_STEAMIDS_PER_RACE) {
                dst->steamIDCount = MAX_STEAMIDS_PER_RACE;
            }
            for (size_t j = 0; j < dst->steamIDCount; j++) {
                dst->steamIDs[j] = src->steamIDs[j];
            }
            dst->minimumWhisper = src->minWhisper;
            dst->maximumWhisper = src->maxWhisper;
            dst->minimumNormal = src->minNormal;
            dst->maximumNormal = src->maxNormal;
            dst->minimumShout = src->minShout;
            dst->maximumShout = src->maxShout;
            dst->listenAddDistance = src->listenAddDistance;
            dst->isActive = TRUE;
        }
        HubRace localRace;
        if (server_profile_get_local_race(&localRace)) {
            for (size_t i = 0; i < raceCount; i++) {
                if (strcmp(races[i].name, localRace.name) == 0) {
                    currentPlayerRaceIndex = (int)i;
                    break;
                }
            }
            currentListenAddDistance = localRace.listenAddDistance;
            hubMinimumWhisper = localRace.minWhisper;
            hubMaximumWhisper = localRace.maxWhisper;
            hubMinimumNormal = localRace.minNormal;
            hubMaximumNormal = localRace.maxNormal;
            hubMinimumShout = localRace.minShout;
            hubMaximumShout = localRace.maxShout;
        }
        else {
            currentListenAddDistance = 0.0f;
        }
    }
    else {
        hubDescriptionAvailable = FALSE;
        hubLimitsActive = FALSE;
        rootChannelID = -1;
        zoneCount = 0;
        currentZoneIndex = -1;
        raceCount = 0;
        currentPlayerRaceIndex = -1;
        currentListenAddDistance = 0.0f;
    }
    currentVoiceMode = (uint8_t)voice_mode_get_current();
}

void plugin_ui_on_hub_profile_updated(void) {
    plugin_ui_sync_live_state();
    ts3_audio_recompute_all();
    voice_overlay_refresh_position();
    updateVoiceOverlay();
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
    localVoiceData.voiceDistance = voice_mode_get_current_distance();
    if (enableVoiceOverlay && hVoiceOverlay) {
        updateVoiceOverlay();
    }
}

#define POS_TXT_TO_WORLD_SCALE 0.01f

static float pos_txt_to_world(float v) {
    return v * POS_TXT_TO_WORLD_SCALE;
}

void pluginGetLocalWorldPos(float* outX, float* outY, float* outZ) {
    if (outX) {
        *outX = pos_txt_to_world(axe_x);
    }
    if (outY) {
        *outY = pos_txt_to_world(axe_y);
    }
    if (outZ) {
        *outZ = pos_txt_to_world(axe_z);
    }
}

int getLocalPlayerZoneIndex(void) {
    float wx, wy, wz;
    pluginGetLocalWorldPos(&wx, &wy, &wz);
    int idx = resolvePlayerZoneIndex(wx, wy, wz);
    if (idx >= 0) {
        return idx;
    }
    return resolvePlayerZoneIndex(axe_x, axe_y, axe_z);
}

int getRemotePlayerZoneIndex(float rx, float ry, float rz) {
    return resolvePlayerZoneIndex(rx, ry, rz);
}

int resolvePlayerZoneIndex(float px, float py, float pz) {
    int idx = getPlayerZone(px, py, pz);
    if (idx >= 0) {
        return idx;
    }
    idx = getPlayerZone(px * 100.0f, py * 100.0f, pz * 100.0f);
    if (idx >= 0) {
        return idx;
    }
    return getPlayerZone(px * POS_TXT_TO_WORLD_SCALE, py * POS_TXT_TO_WORLD_SCALE,
        pz * POS_TXT_TO_WORLD_SCALE);
}

int ts3_plugin_resolve_remote_zone(float rx, float ry, float rz) {
    int idx = getRemotePlayerZoneIndex(rx, ry, rz);
    if (idx >= 0) {
        return idx;
    }
    idx = getPlayerZone(rx * 100.0f, ry * 100.0f, rz * 100.0f);
    return idx >= 0 ? idx : -1;
}

int ts3_plugin_resolve_local_zone(void) {
    int z = getLocalPlayerZoneIndex();
    if (z < 0 && currentZoneIndex >= 0 && (size_t)currentZoneIndex < zoneCount) {
        z = currentZoneIndex;
    }
    return z;
}

float plugin_clamp_remote_voice_distance(float voiceDistanceMeters) {
    if (voiceDistanceMeters < 0.0f) {
        return 0.0f;
    }
    if (!hubDescriptionAvailable) {
        return voiceDistanceMeters;
    }
    float cap = (float)hubMaximumShout;
    if (cap <= 0.0f) {
        cap = (float)hubMaximumNormal;
    }
    if (cap > 0.0f && voiceDistanceMeters > cap) {
        return cap;
    }
    return voiceDistanceMeters;
}

int ts3_plugin_is_soundproof_muted(int localZone, int remoteZone) {
    if (remoteZone >= 0 && (size_t)remoteZone < zoneCount && zones[remoteZone].isSoundproof) {
        if (localZone != remoteZone) {
            return 1;
        }
    }
    return 0;
}

int ts3_plugin_is_soundproof_muted_at(float rx, float ry, float rz) {
    const int localZone = ts3_plugin_resolve_local_zone();
    const int remoteZone = ts3_plugin_resolve_remote_zone(rx, ry, rz);
    return ts3_plugin_is_soundproof_muted(localZone, remoteZone);
}

int ts3_plugin_client_soundproof_muted(unsigned int clientID) {
    if (pluginShuttingDown || !enableDistanceMuting || clientID == 0) {
        return 0;
    }
    return ts3_proximity_audio_soundproof_muted(clientID);
}

int ts3_plugin_zone_reverb_active(int localZone, int remoteZone) {
    if (localZone >= 0 && (size_t)localZone < zoneCount && zones[localZone].isReverb) {
        return 1;
    }
    if (remoteZone >= 0 && (size_t)remoteZone < zoneCount && zones[remoteZone].isReverb) {
        return 1;
    }
    return 0;
}

BOOL hubDescriptionHasContent(const char* description) {
    if (!description) {
        return FALSE;
    }
    for (const char* p = description; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            return TRUE;
        }
    }
    return FALSE;
}

void readHubDescription(void) { }
void parseHubDescription(const char* description) { (void)description; }
void applyDefaultSettingsIfNeeded(const char* description, mumble_connection_t connection) {
    (void)description;
    (void)connection;
}
void initializeChannelIDs(void) { }
void manageChannelBasedOnCoordinates(void) { }
void ts3_show_pending_hub_confirm(void) { }
int ts3_is_root_channel_id(uint64_t channelID) { (void)channelID; return 0; }
void hubDescriptionMonitorThread(void* arg) { (void)arg; }
void channelManagementThread(void* arg) { (void)arg; }

void plugin_ui_init(void) {
    mumbleAPI.log = compat_mumble_log;
    ownID = 1;
    InitializeCriticalSection(&overlayTextLock);
    InterlockedExchange(&overlayTextLockInitialized, 1);
    plugin_ui_sync_from_config();
    plugin_ui_sync_live_state();
    voice_mode_reset_key_tracking();
    installKeyMonitoring();
    initializeVoicePresets();
    loadPresetsFromConfigFile();
}

void plugin_ui_shutdown(void) {
    pluginShuttingDown = TRUE;
}

static volatile unsigned long g_overlayUiThreadId = 0;
static HANDLE g_overlayMonitorHandle = NULL;

void overlay_ui_mark_thread(void) {
    g_overlayUiThreadId = GetCurrentThreadId();
}

void overlay_ui_clear_thread(void) {
    g_overlayUiThreadId = 0;
}

void overlay_ui_signal_quit(void) {
    const unsigned long tid = g_overlayUiThreadId;
    if (tid != 0) {
        PostThreadMessage(tid, WM_QUIT, 0, 0);
    }
}

void overlay_start(void) {
    if (!enableVoiceOverlay) {
        return;
    }
    createVoiceOverlay();
    updateVoiceOverlay();
    installKeyMonitoring();
    if (!g_overlayMonitorHandle) {
        overlayThreadRunning = TRUE;
        g_overlayMonitorHandle = (HANDLE)_beginthreadex(NULL, 0, overlayMonitorThreadEx, NULL, 0, NULL);
        if (!g_overlayMonitorHandle) {
            overlayThreadRunning = FALSE;
            log_write("OVERLAY: monitor thread start failed");
        }
    }
}

void overlay_stop(void) {
    removeKeyMonitoring();
    overlayThreadRunning = FALSE;
    if (g_overlayMonitorHandle) {
        WaitForSingleObject(g_overlayMonitorHandle, 3000);
        CloseHandle(g_overlayMonitorHandle);
        g_overlayMonitorHandle = NULL;
    }
    overlay_ui_signal_quit();
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
    cfg.hudPosition = voiceHudPosition;
    cfg.hudSize = voiceHudSize;
    /* The legacy savedPath global holds the ACTIVE path: auto-detected in
       automatic mode, user-chosen in manual mode. Route it into the matching
       config field so pos_file resolves Pos.txt correctly. Never persist UI
       placeholders like "(Not configured)". */
    if (savedPath[0] && wcsstr(savedPath, L"(Not configured)") == NULL) {
        if (enableAutomaticPatchFind) {
            wcsncpy_s(cfg.automaticSavedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
        }
        else {
            wcsncpy_s(cfg.savedPath, CONFIG_MAX_PATH, savedPath, _TRUNCATE);
        }
    }
    config_clamp(&cfg);
    config_apply(&cfg);
    /* Canonical rewrite of plugin.cfg — restores keys the legacy writers drop
       (DefaultsAppliedServer, DebugMode, EnableVoiceOverlay). */
    config_save();
    log_set_enabled(cfg.debugMode);
    if ((VoiceMode)currentVoiceMode != voice_mode_get_current()) {
        voice_mode_apply((VoiceMode)currentVoiceMode);
    }
    cepos_invalidate_send_cache();
    cepos_signal_send_pending();
    ts3_audio_recompute_all();
    voice_overlay_refresh_theme();
    voice_overlay_refresh_position();
    voice_overlay_refresh_size();
    updateVoiceOverlayVisibility();
}

unsigned long overlay_ui_thread_id(void) {
    return g_overlayUiThreadId;
}
