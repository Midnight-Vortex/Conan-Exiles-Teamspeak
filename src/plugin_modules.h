#ifndef PLUGIN_MODULES_H
#define PLUGIN_MODULES_H

/*
 * EN: Module index and cross-module function declarations (see comments in each .c file).
 * FR: Index des modules et déclarations inter-modules (voir commentaires dans chaque .c).
 *
 * Source layout mirrors the original Mumble plugin (Dino_Rex) module order:
   1  util_base.c          Base utilities
   2  config_files.c       Configuration and files
   3  validation.c         Validation, limits, zones (3D polygons)
   4  hub_parser.c         Root channel description parsing
   5  channel_manage.c     Hub / ingame channel moves
   6  proximity_volume.c   Volume calculations
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

/* util_base.c */
const char* getKeyName(int vkCode);
void displayInChat(const char* message);
void displayHubParametersConfirmation(BOOL globalSuccess, BOOL racesSuccess, BOOL playerInRace, BOOL zonesSuccess);
wchar_t* getConfigFolderPath(void);
int isPatchAlreadySaved(void);
float calculateDistance(float x1, float y1, float z1, float x2, float y2, float z2);
float calculateDistance3D(Vector3* a, Vector3* b);
int countSignificantDigits(float value);
BOOL getServerHashForTracking(mumble_connection_t connection, char* outHash, size_t hashSize);
void ts3_queue_chat_message(const char* message);
int ts3_plugin_has_pending_chat(void);
void ts3_plugin_flush_pending_chat(void);

/* config_files.c */
void loadVoiceDistancesFromConfig(void);
void readConfigurationSettings(void);
void saveConfigurationChange(const char* key, const wchar_t* value);
void saveVoiceSettings(void);
void initializeVoicePresets(void);
void saveVoicePreset(int presetIndex, const char* presetName);
void loadVoicePreset(int presetIndex);
BOOL renameVoicePreset(int presetIndex, const char* newName);
void savePresetsToConfigFile(void);
void loadPresetsFromConfigFile(void);
void writeFullConfiguration(const wchar_t* gameFolder, const wchar_t* distWhisper, const wchar_t* distNormal, const wchar_t* distShout);

/* validation.c */
BOOL shouldApplyDistanceLimits(void);
void checkConnectionStatus(void);
BOOL shouldValidateValue(float value, float minimum, float maximum, const char* modeName);
float validateDistanceValue(float value, float minimum, float maximum, const char* modeName);
void validatePlayerDistances(void);
BOOL isPointInPolygon(float px, float pz, float x1, float z1, float x2, float z2, float x3, float z3, float x4, float z4);
BOOL zoneContainsPoint(const Zone* z, float px, float py, float pz, int xzFloor);
int getPlayerZone(float playerX, float playerY, float playerZ);

/* hub_parser.c */
void applyDefaultSettingsIfNeeded(const char* description, mumble_connection_t connection);
void parseHubDescription(const char* description);
void readHubDescription(void);
BOOL hubDescriptionHasContent(const char* description);
void ts3_show_pending_hub_confirm(void);
int ts3_is_root_channel_id(uint64_t channelID);
void hubDescriptionMonitorThread(void* arg);

/* channel_manage.c */
void initializeChannelIDs(void);
void manageChannelBasedOnCoordinates(void);
void channelManagementThread(void* arg);
int ts3_plugin_should_send_position(void);

/* proximity_volume.c */
float calculateVolumeMultiplier(float distance, float maxDistance);
float calculateVolumeMultiplierWithHubSettings(float distance, float voiceDistance);
void applyDistanceToAllPlayers(void);
ProximityVolumeContext plugin_proximity_volume_context(void);

/* proximity_adaptive.c */
void setUserAdaptiveVolume(mumble_userid_t userID, float targetVolume);
void setUserAdaptiveVolumeWithSpatial(mumble_userid_t userID, float baseVolume, float leftVol, float rightVol);
void cleanupAudioVolumeStates(void);
void cleanupAdaptivePlayerStates(void);
AdaptivePlayerData* findOrCreateAdaptivePlayerState(mumble_userid_t userID, const char* playerName);

/* voice_modes.c */
float getVoiceDistanceForMode(uint8_t voiceMode);
float plugin_clamp_remote_voice_distance(float voiceDistanceMeters);
void getLocalPlayerName(void);
void broadcastPlayerCoordinates(void);
void cycleVoiceMode(void);
void updateVoiceMode(void);
void voice_mode_notify_hotkey(int vkCode);
void voice_mode_reset_key_tracking(void);
void calculateLocalPositionalData(CompletePositionalData* localData);
void sendCompletePositionalData(void);
void ts3_plugin_invalidate_cepos_send_cache(void);
void applyHubSettingsAfterDescriptionRead(void);
void calculateLocalPositionalAudio(const CompletePositionalData* remoteData, mumble_userid_t userID);
void ts3_plugin_clear_deferred(void);
void ts3_plugin_process_deferred(void);
void ts3_plugin_process_deferred_ex(int allowChannelMove);

/* overlay */
void createVoiceOverlay(void);
void destroyVoiceOverlay(void);
void plugin_destroy_voice_overlay_safely(void);
void plugin_destroy_ui_window_safely(HWND* phwnd);
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

/* mod_watcher.c */
BOOL checkModFileActive(void);
BOOL readModFileRaw(char* buffer, size_t bufferSize, size_t* outBytes);
BOOL readModFileData(struct ModFileData* data);
void checkCurrentZone(void);
void modFileWatcherThread(void* arg);
void mod_file_on_server_connected(void);
void mod_file_drain_pending_updates(void);

/* system_threads.c */
void voiceSystemThread(void* arg);
void ts3_proximity_heartbeat_thread(void* arg);
void forceCompleteInitialization(void);

/* cleanup.c */
void cleanupPlayerMuteStates(void);

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
