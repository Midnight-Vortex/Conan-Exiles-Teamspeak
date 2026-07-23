/*
Mozilla Public License Version 2.0 + Network Use Clause

Copyright(c) 2025 Dino_Rex

This software is licensed under the Mozilla Public License 2.0 (MPL - 2.0),
available at : https://www.mozilla.org/MPL/2.0/

Additional Network Use Clause :
If you modify this software and deploy or use it to provide a service accessible
to others over a network(including via web, API, or remote access),
you must make the complete corresponding source code of your modified version
publicly available under this same license(MPL - 2.0 + Network Use Clause),
including all changes and additions you made.

All other terms and conditions of the Mozilla Public License 2.0 remain unchanged.
*/

// V8.12: F10 persisted settings live in g_config (ui_cfg() working copy while dialog
// open). Legacy globals below are mirrors for overlay/HWND state only — do not write
// them for config fields; use ui_cfg() / g_config instead.

#ifndef PLUGIN_H
#define PLUGIN_H

/*
 * EN: Shared types, globals, and structures for Conan Exiles proximity voice plugin.
 * FR: Types, globaux et structures partagés pour le plugin voix proximité Conan Exiles.
 */

// ============================================================================
// INCLUDES
// ============================================================================
#include "mumble_compat.h"
#include <stdint.h>
#include <windows.h>
#include <stdbool.h>

// ============================================================================
// MACROS
// ============================================================================
#define CONFIG_FILE L"plugin.cfg"
#ifndef _UNICODE
#define _UNICODE
#endif

// ============================================================================
// STRUCTURES
// ============================================================================

// Mod file data structure | Structure des données du fichier mod
struct ModFileData {
    int seq;
    float x, y, z, yaw, yawY;
    BOOL valid;
};

// Complete positional data | Données positionnelles complètes
#pragma pack(push, 1)
typedef struct {
    float x, y, z;
    float dirX, dirY, dirZ;
    float axisX, axisY, axisZ;
    float voiceDistance;
    char playerName[16];
} CompletePositionalData;
#pragma pack(pop)

// Vector3 structure | Structure Vector3
typedef struct {
    float x, y, z;
} Vector3;

// Adaptive player data | Données par joueur pour le volume adaptatif
typedef struct {
    mumble_userid_t userID;
    char playerName[64];
    Vector3 position;
    float voiceDistance;
    float currentVolume;
    int zoneIndex; /* cached [ZONES] index from last CEPOS (-1 = unknown) */
    bool isValid;
    ULONGLONG lastVolumeUpdate;   /* last spatial recompute (cache refresh or CEPOS) */
    ULONGLONG lastCeposRecvMs;    /* last network CEPOS packet only */
} AdaptivePlayerData;

// Audio volume state structure | Structure pour stocker les volumes par utilisateur
typedef struct {
    mumble_userid_t userID;
    float targetVolume;
    float currentVolume;
    float leftVolume;
    float rightVolume;
    ULONGLONG lastUpdate;
    bool isValid;
} AudioVolumeState;

// Player mute state | État de mute d'un joueur
typedef struct {
    mumble_userid_t userID;
    char playerName[64];
    bool currentlyMuted;
    ULONGLONG lastMuteCheck;
} PlayerMuteState;

// ============================================================================
// VOICE RANGE PRESETS | PRESETS DE PORTÉE VOCALE
// ============================================================================

#define MAX_VOICE_PRESETS 10
#define PRESET_NAME_MAX_LENGTH 64

// Voice range preset structure | Structure de preset de portée vocale
typedef struct {
    char name[PRESET_NAME_MAX_LENGTH];
    float whisperDistance;
    float normalDistance;
    float shoutDistance;
    int whisperKey;
    int normalKey;
    int shoutKey;
    int voiceToggleKey;
    BOOL isUsed;
} VoiceRangePreset;

// Voice preset global variables | Variables globales pour les presets vocaux
extern VoiceRangePreset voicePresets[MAX_VOICE_PRESETS];
extern int currentPresetIndex;
extern HWND hCategoryPresets;

// ============================================================================
// EXTERN VARIABLES
// ============================================================================

// Plugin control variables
extern BOOL enableGetPlayerCoordinates;
extern BOOL enableAutomaticPatchFind;
extern HWND hAutomaticPatchFindCheck;
extern ULONGLONG lastCoordinateBroadcast;

// Log control variables
extern BOOL enableLogCoordinates;
extern BOOL enableLogModFile;
extern BOOL enableLogConfig;
extern BOOL enableLogGeneral;

// Mumble API interface
extern struct MumbleAPI_v_1_0_x mumbleAPI;
extern mumble_plugin_id_t ownID;

// Audio distance variables
extern double serverMaximumAudioDistance;
extern BOOL maxAudioDistanceRetrieved;
extern ULONGLONG lastMaxDistanceCheck;

// Overlay border highlight variables
extern BOOL overlayBorderHighlight;
extern mumble_userid_t overlayHighlightUserID;
extern char overlaySpeakerText[128];
extern CRITICAL_SECTION overlayTextLock;

// Overlay variables
extern HWND hVoiceOverlay;
extern HWND hVoiceText;
/* Voice HUD color theme indices — persisted as HudTheme=N in plugin.cfg
   Indices de thème couleur du HUD vocal — enregistrés HudTheme=N dans plugin.cfg */
#define VOICE_HUD_THEME_GRAY   0
#define VOICE_HUD_THEME_PURPLE 1
#define VOICE_HUD_THEME_BLUE   2
#define VOICE_HUD_THEME_GREEN  3
#define VOICE_HUD_THEME_AMBER  4
#define VOICE_HUD_THEME_RED    5
#define VOICE_HUD_THEME_CYAN   6
#define VOICE_HUD_THEME_PINK   7
#define VOICE_HUD_THEME_ORANGE 8
#define VOICE_HUD_THEME_TEAL   9
#define VOICE_HUD_THEME_WHITE  10
#define VOICE_HUD_THEME_LIME   11
#define VOICE_HUD_THEME_COUNT  12

/* Voice HUD screen position — persisted as HudPosition=N in plugin.cfg
   Position à l'écran du HUD — enregistrée HudPosition=N dans plugin.cfg */
#define VOICE_HUD_POSITION_TOP_LEFT     0
#define VOICE_HUD_POSITION_TOP_RIGHT    1
#define VOICE_HUD_POSITION_BOTTOM_LEFT  2
#define VOICE_HUD_POSITION_TOP_CENTER   3
#define VOICE_HUD_POSITION_BOTTOM_RIGHT 4
#define VOICE_HUD_POSITION_COUNT        5

/* Voice HUD size — persisted as HudSize=N in plugin.cfg (0=small, 1=medium, 2=big)
   Taille du HUD — enregistrée HudSize=N dans plugin.cfg (0=petit, 1=moyen, 2=grand) */
#define VOICE_HUD_SIZE_SMALL  0
#define VOICE_HUD_SIZE_MEDIUM 1
#define VOICE_HUD_SIZE_BIG    2
#define VOICE_HUD_SIZE_COUNT  3

extern int voiceHudTheme; /* Current HUD palette index; saved as HudTheme in plugin.cfg
                            Index de palette HUD actuel ; sauvegardé HudTheme dans plugin.cfg */
extern int voiceHudPosition; /* HUD corner/anchor index; saved as HudPosition in plugin.cfg
                                Index position HUD ; sauvegardé HudPosition dans plugin.cfg */
extern int voiceHudSize; /* HUD scale index; saved as HudSize in plugin.cfg
                            Index taille HUD ; sauvegardé HudSize dans plugin.cfg */
extern BOOL enableVoiceOverlay;
extern HFONT hOverlayFont;
extern BOOL overlayThreadRunning;

// Channel management variables
// V8.5: hubChannelID / ingameChannelID are DERIVED MIRRORS of the canonical
// IDs owned by ts/channel/channel_manage (chan_get_hub_channel_id /
// chan_get_ingame_channel_id). UI/overlay read only; the single writer is
// plugin_ui_sync_live_state (callback thread). Do not assign elsewhere.
extern mumble_channelid_t hubChannelID;
extern mumble_channelid_t rootChannelID;
extern mumble_channelid_t ingameChannelID;
extern mumble_channelid_t lastTargetChannel;
extern mumble_channelid_t lastValidChannel;
extern BOOL channelManagementActive;
extern BOOL enableAutomaticChannelChange;
extern ULONGLONG lastChannelCheck;

// Player position variables
extern float axe_x;
extern float axe_y;
extern float axe_z;
extern float avatarAxisX;
extern float avatarAxisY;
extern float avatarAxisZ;

// Adaptive mod system variables
extern time_t lastFileCheck;
extern time_t LastFileModification;
extern int lastSeq;
extern BOOL modDataValid;
extern char modFilePath[MAX_PATH];
extern BOOL coordinatesValid;
extern struct ModFileData currentModData;
extern ULONGLONG lastModDataTick;

// Global variables for the interface
extern HWND hConfigDialog;
extern HWND hWhisperKeyEdit, hNormalKeyEdit, hShoutKeyEdit, hConfigKeyEdit;
extern HWND hWhisperButton, hNormalButton, hShoutButton, hConfigButton;
extern HWND hEnableDistanceMutingCheck, hEnableAutomaticChannelChangeCheck;
extern HWND hDistanceWhisperEdit, hDistanceNormalEdit, hDistanceShoutEdit;
extern HWND hSavedPathEdit, hSavedPathButton, hSavedPathBg;
extern HBITMAP hPathBoxBitmap;
extern HFONT hPathFont;
extern HBITMAP hBackgroundBitmap;
extern HBITMAP hBackgroundAdvancedBitmap;
extern HBITMAP hBackgroundPresetsBitmap;
extern HBITMAP hBackgroundSavePresetBitmap;
extern HBITMAP hBackgroundRenamePresetBitmap;
extern BOOL backgroundDrawn;
extern HWND hPresetLabels[MAX_VOICE_PRESETS];
extern HWND hPresetLoadButtons[MAX_VOICE_PRESETS];
extern HWND hPresetRenameButtons[MAX_VOICE_PRESETS];
extern HWND hPresetSaveDialog;
extern HWND hPresetRenameDialog;
extern char renameBuffer[PRESET_NAME_MAX_LENGTH];
extern int renamePresetIndex;
extern HWND hCategoryPatch, hCategoryAdvanced;
extern HWND hEnableVoiceToggleCheck, hVoiceToggleKeyEdit, hVoiceToggleButton;
extern HWND hHudThemeLabel, hHudThemeCombo; /* F10 Advanced Options: HUD theme picker
                                               F10 Options avancées : sélecteur de thème HUD */
extern HWND hHudPositionLabel, hHudPositionCombo; /* F10 Advanced Options: HUD position picker
                                                     F10 Options avancées : sélecteur position HUD */
extern HWND hHudSizeLabel, hHudSizeCombo; /* F10 Advanced Options: HUD size picker
                                             F10 Options avancées : sélecteur taille HUD */
extern HWND hStatusMessage;
extern HWND hDistanceLimitMessage;
extern HFONT hFont, hFontBold, hFontLarge, hFontEmoji;

// Interface message controls
extern HWND hDistanceWhisperMessage;
extern HWND hDistanceNormalMessage;
extern HWND hDistanceShoutMessage;
extern HWND hDistanceMutingMessage;
extern HWND hChannelSwitchingMessage;
extern HWND hPositionalAudioMessage;
extern HWND hDistanceServerLimitWhisper;
extern HWND hDistanceServerLimitNormal;
extern HWND hDistanceServerLimitShout;

// Interface state variables
extern int currentCategory;
extern BOOL isCapturingKey;
extern int captureKeyTarget;
extern wchar_t savedPath[MAX_PATH];
extern wchar_t displayedPathText[MAX_PATH];
extern BOOL isUpdatingInterface;
extern ULONGLONG lastInterfaceUpdate;

// Interface text constants (removed unused infoText1/2/3 in V8.9)

// Voice toggle variables
extern int voiceToggleKey;
extern BOOL enableVoiceToggle;
extern ULONGLONG lastVoiceTogglePress;

// Key bindings
extern int whisperKey;
extern int normalKey;
extern int shoutKey;
extern int configUIKey;

// Key monitoring variables
int config_dialog_try_open(void);
void config_dialog_close(void);
int config_dialog_is_open(void);
extern DWORD lastKeyPressTime;
/* Key monitor stop flag — Interlocked access only (set by start/stop on the
   controlling thread, polled by the key monitor thread). */
extern volatile LONG keyMonitorThreadRunning;
extern HANDLE keyMonitorThread;
extern BOOL lastKeyState;

// Connection and hub state variables
extern BOOL isConnectedToServer;
extern BOOL hubDescriptionAvailable;
extern BOOL hubLimitsActive;
extern ULONGLONG lastConnectionCheck;

// Hub audio parameters
extern double hubAudioMinDistance;
extern double hubAudioMaxDistance;
extern double hubAudioMaxVolume;
extern double hubAudioBloom;
extern double hubAudioFilterIntensity;
extern BOOL hubForcePositionalAudio;
extern ULONGLONG lastHubDescriptionCheck;

// Hub distance limits
extern double hubMinimumWhisper;
extern double hubMaximumWhisper;
extern double hubMinimumNormal;
extern double hubMaximumNormal;
extern double hubMinimumShout;
extern double hubMaximumShout;
extern BOOL hubForceDistanceBasedMuting;
extern BOOL hubForceAutomaticChannelSwitching;

// Voice system variables
extern CompletePositionalData localVoiceData;
extern ULONGLONG lastVoiceDataSent;
extern ULONGLONG lastKeyCheck;

// Voice distance settings
extern float distanceWhisper;
extern float distanceNormal;
extern float distanceShout;

// Voice features
extern BOOL enableDistanceMuting;

// Mute system variables
extern ULONGLONG lastDistanceCheck;

// Refresh variables
extern BOOL forceGlobalMuteRefresh;
extern ULONGLONG lastGlobalRefresh;

// Automatic audio settings variables
extern BOOL enableAutoAudioSettings;
extern ULONGLONG lastAudioSettingsApply;

extern BOOL voiceSystemRunning;
extern BOOL channelManagementRunning;
extern BOOL modFileWatcherRunning;
extern BOOL hubDescriptionMonitorRunning;
/* Set during mumble_shutdown so overlay/UI code stops touching HWNDs and GDI.
   Activé pendant mumble_shutdown pour que l'overlay/UI n'utilise plus les HWND/GDI. */
extern BOOL pluginShuttingDown;

// Race system (shared with hub parser)
#define MAX_RACES 32
#define MAX_STEAMIDS_PER_RACE 100
typedef struct {
    char name[64];
    uint64_t steamIDs[MAX_STEAMIDS_PER_RACE];
    size_t steamIDCount;
    double minimumWhisper;
    double maximumWhisper;
    double minimumNormal;
    double maximumNormal;
    double minimumShout;
    double maximumShout;
    float listenAddDistance;
    BOOL isActive;
} Race;

extern uint64_t steamID;
extern Race races[MAX_RACES];
extern size_t raceCount;

// First-connection defaults
extern char serverConfigHash[256];
extern BOOL hasAppliedDefaultSettings;
extern BOOL enableDefaultSettingsOnFirstConnection;
extern int defaultWhisperKey;
extern int defaultNormalKey;
extern int defaultShoutKey;
extern int defaultVoiceToggleKey;
extern float defaultDistanceWhisper;
extern float defaultDistanceNormal;
extern float defaultDistanceShout;

/* Zone system: 3D quadrilateral from Root [ZONES] block (shared with proximity path).
   Horizontal footprint uses X1..X4 and Z1..Z4; GroundY/TopY are vertical bounds.
   SoundProof=True: one-way — outsiders cannot hear speakers inside the zone;
   listeners inside can still hear speakers outside (see zone_soundproof_muted). */
#define MAX_ZONES 32
typedef struct {
    char name[64];
    float x1, z1, x2, z2, x3, z3, x4, z4; /* horizontal polygon corners */
    float groundY;
    float topY;                             /* vertical cube limits */
    double audioMinDistance;
    double audioMaxDistance;
    double audioMaxVolume;
    float whisperDist;
    float normalDist;
    float shoutDist;
    BOOL isSoundproof;
    BOOL isReverb;
} Zone;

extern Zone zones[MAX_ZONES];
extern size_t zoneCount;
extern int currentZoneIndex;
extern uint8_t currentVoiceMode;
extern int currentPlayerRaceIndex;
extern float currentListenAddDistance;

#ifdef CONAN_EXILES_TS_EXPORTS
extern mumble_channelid_t ts3LocalChannelID;
extern BOOL ts3ProximityHeartbeatRunning;
extern volatile long ts3PendingJoinHubRead;
extern volatile long ts3HubConfirmPending;
extern volatile long ts3HubReadBypassThrottle;
int ts3_plugin_compute_audio_mode(void);
#endif

// Adaptive system variables
extern Vector3 localPlayerPosition;

#endif // PLUGIN_H
