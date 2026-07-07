#ifndef PLUGIN_INTERNAL_H
#define PLUGIN_INTERNAL_H

/*
 * EN: Shared internal API between plugin.c modules — zone lookup, soundproof, overlay, world position.
 * FR: API interne partagée entre modules plugin.c — recherche zone, insonorisation, overlay, position monde.
 */

#include "plugin.h"
#include "proximity_math.h"
#include "plugin_modules.h"
#include <stdint.h>

extern Zone zones[MAX_ZONES];
extern size_t zoneCount;
extern int currentZoneIndex;
extern uint8_t currentVoiceMode;
extern int currentPlayerRaceIndex;
extern float currentListenAddDistance;
extern mumble_channelid_t ts3LocalChannelID;

void pluginGetLocalWorldPos(float* outX, float* outY, float* outZ);
float getVoiceDistanceForMode(uint8_t voiceMode);

int ts3_plugin_resolve_local_zone(void);
int ts3_plugin_resolve_remote_zone(float rx, float ry, float rz);
int ts3_plugin_is_soundproof_muted(int localZone, int remoteZone);
int ts3_plugin_is_soundproof_muted_at(float rx, float ry, float rz);
int ts3_plugin_client_soundproof_muted(unsigned int clientID);
int ts3_plugin_zone_reverb_active(int localZone, int remoteZone);

ProximityVolumeContext plugin_proximity_volume_context(void);

int plugin_should_show_voice_overlay(void);
void updateVoiceOverlayVisibility(void);

int plugin_overlay_text_lock_try(void);
void plugin_overlay_text_lock_release(void);

#endif
