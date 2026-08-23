#ifndef PLUGIN_MODULES_H
#define PLUGIN_MODULES_H

/*
 * EN: Module index and cross-module function declarations (see comments in each .c file).
 * FR: Index des modules et déclarations inter-modules (voir commentaires dans chaque .c).
 *
 * V8.10+ layout:
 *   core/  hub, proximity, config.c, zone_resolve (pure — no ts/ui includes)
 *   ts/    adapter, proximity audio, channel, nick, chat queue
 *   ui/    F10 dialog, presets, overlay, hub validation, display/key utils
 *
 * Historical Mumble module numbers (reference only):
 *   4  hub_parser.c
   5  channel_manage.c     Hub / ingame channel moves
   8  proximity_adaptive.c Per-player volume state
  12  voice_modes.c        Voice modes and positional send
  13  voice_overlay.c      In-game HUD overlay
  14  ui_main.c            F10 settings dialog
  15  ui_messages.c        UI status messages
  16  ui_dynamic.c         Dynamic UI controls
  17  key_watcher.c        Hotkey watcher
  18  mod_watcher.c        Pos.txt file watcher
  19  system_threads.c     Background threads
  20  cleanup.c            Shutdown cleanup
  21  plugin.c             Lifecycle and Mumble API callbacks
  TS-only: src/ts/adapter, deferred, proximity, entry, profile */

#include "plugin.h"
#include "core/proximity/proximity_math.h"
#include "core/voice/voice_modes.h"
#include "mumble_compat.h"
#include <windows.h>

/* Zone helpers (plugin.c) */
int getLocalPlayerZoneIndex(void);
int getRemotePlayerZoneIndex(float rx, float ry, float rz);
int resolvePlayerZoneIndex(float px, float py, float pz);

/* ui/util/ui_key_util.c */
const char* getKeyName(int vkCode);
int countSignificantDigits(float value);

/* ui/util/ui_display_util.c */
void displayInChat(const char* message);
void displayHubParametersConfirmation(BOOL globalSuccess, BOOL racesSuccess, BOOL playerInRace, BOOL zonesSuccess);
int isPatchAlreadySaved(void);

/* ui/util/ui_ts_chat_queue.c */
void ts3_queue_chat_message(const char* message);
int ts3_plugin_has_pending_chat(void);
void ts3_plugin_clear_pending_chat(void);
void ts3_plugin_flush_pending_chat(void);

/* ui/config/ui_voice_presets.c (was config_files.c) */
void saveVoiceSettings(void);
void initializeVoicePresets(void);
void saveVoicePreset(int presetIndex, const char* presetName);
void loadVoicePreset(int presetIndex);
BOOL renameVoicePreset(int presetIndex, const char* newName);
void savePresetsToConfigFile(void);
void loadPresetsFromConfigFile(void);
void writeFullConfiguration(const wchar_t* gameFolder, const wchar_t* distWhisper, const wchar_t* distNormal, const wchar_t* distShout);

/* ui/validation/ui_hub_validation.c (hub distance limits for F10) */
BOOL shouldApplyDistanceLimits(void);
BOOL shouldValidateValue(float value, float minimum, float maximum, const char* modeName);
float validateDistanceValue(float value, float minimum, float maximum, const char* modeName);

/* voice_modes.c */
float getVoiceDistanceForMode(uint8_t voiceMode);
float plugin_clamp_remote_voice_distance(float voiceDistanceMeters);
void voice_mode_notify_hotkey(int vkCode);
void voice_mode_reset_key_tracking(void);

/* overlay */
void createVoiceOverlay(void);
void destroyVoiceOverlay(void);
void plugin_destroy_voice_overlay_safely(void);
unsigned __stdcall overlayMonitorThreadEx(void* arg);
void refreshOverlayForFullscreen(void);
void repositionVoiceOverlay(void);
void updateVoiceOverlay(void);
void voice_overlay_refresh_theme(void);
void voice_overlay_refresh_position(void);
void voice_overlay_refresh_size(void);
void voice_overlay_get_dimensions(int* outWidth, int* outHeight);
void overlayMonitorThread(void* arg);
void setOverlayHighlightState(mumble_userid_t userID, mumble_connection_t connection, BOOL highlight);

/* ui */
void updateDynamicInterface(void);
void forceInterfaceRefresh(void);
void handleDistanceEditChange(int editId);
void updateConsolidatedDistanceMessages(void);
void updateServerLimitMessages(void);
void updateDistanceMutingMessage(void);
void updateChannelSwitchingMessage(void);
void updatePositionalAudioMessage(void);
void showStatusMessage(const wchar_t* message, BOOL isError);
void showConfigSavedNotice(HWND parent, const wchar_t* statusText);
void clearStatusMessage(void);
void loadDefaultSettingsFromConfig(void);
void saveDefaultSettingsToConfig(void);
void showPathSelectionDialogThread(void* arg);
INT_PTR CALLBACK ConfigDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
int showConfigInterface(void);
void createPresetsCategory(void);
void updatePresetLabels(void);
void showPresetSaveDialog(void);
LRESULT CALLBACK PresetSaveDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PresetRenameDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK VoiceOverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL findConanExilesAutomatic(wchar_t* outPath, size_t pathSize);
BOOL readSteamIDFromRegistry(uint64_t* outSteamID);
BOOL parseSteamLibraryFolders(const wchar_t* vdfPath, wchar_t* outConanPath, size_t pathSize);
void DrawButtonWithBitmap(LPDRAWITEMSTRUCT lpDIS);

/* key_watcher.c */
void installKeyMonitoring(void);
void removeKeyMonitoring(void);
void startKeyMonitorThread(void);
void stopKeyMonitorThread(void);
/* Open settings on a dedicated UI thread (F10 / Plugins menu). Caller must
   have already succeeded at config_dialog_try_open(). */
void settings_dialog_open_async(void);
/* Close the F10 dialog (WM_CLOSE) and join its thread — shutdown only. */
void settings_dialog_shutdown(void);

#ifdef CONAN_EXILES_TS_EXPORTS
extern volatile long ts3HubDescriptionChanged;
extern volatile long ts3ChatFlushThreadArmed;
extern ULONGLONG ts3ChannelMoveAllowedAfter;
extern ULONGLONG ts3LastHubRetryTick;
extern ULONGLONG ts3LocalMoveQuietUntil;
extern volatile long ts3ManualChannelFreedom;
extern volatile long ts3ForceIngameMovePending;
extern volatile long ts3InClientMoveCallback;
extern volatile long ts3ChannelMoveExecutionAllowed;
extern BOOL lastHubConfirmGlobal;
extern BOOL lastHubConfirmRaces;
extern BOOL lastHubConfirmPlayerInRace;
extern BOOL lastHubConfirmZones;
#endif

#endif
