/*
 * EN: Mumble API compatibility layer for TeamSpeak 3 port — types and mumbleAPI function table.
 * FR: Couche compatibilité API Mumble pour port TeamSpeak 3 — types et table de fonctions mumbleAPI.
 * Allows the original Conan Exiles proximity-voice logic to run largely unchanged.
 * Permet à la logique voix proximité Conan Exiles originale de tourner quasi inchangée.
 */

#ifndef MUMBLE_COMPAT_H
#define MUMBLE_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#define PLUGIN_CALLING_CONVENTION __cdecl
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#define PLUGIN_CALLING_CONVENTION
#endif

typedef uint32_t mumble_plugin_id_t;
typedef uint32_t mumble_userid_t;
typedef int64_t mumble_channelid_t;
typedef uint64_t mumble_connection_t;
typedef int32_t mumble_error_t;

typedef struct {
    int32_t major;
    int32_t minor;
    int32_t patch;
} mumble_version_t;

struct MumbleStringWrapper {
    const char* data;
    size_t size;
    bool needsReleasing;
};

enum Mumble_TalkingState {
    MUMBLE_TS_INVALID = -1,
    MUMBLE_TS_PASSIVE = 0,
    MUMBLE_TS_TALKING,
    MUMBLE_TS_WHISPERING,
    MUMBLE_TS_SHOUTING,
    MUMBLE_TS_TALKING_MUTED
};

typedef enum Mumble_TalkingState mumble_talking_state_t;

#define MUMBLE_STATUS_OK 0
#define MUMBLE_FEATURE_AUDIO 0x00000001

#define MUMBLE_PLUGIN_API_MAJOR_MACRO 1
#define MUMBLE_PLUGIN_API_MINOR_MACRO 0
#define MUMBLE_PLUGIN_API_PATCH_MACRO 0

static const mumble_version_t MUMBLE_PLUGIN_API_VERSION = { 1, 0, 0 };

struct MumbleAPI_v_1_0_x {
    mumble_error_t (PLUGIN_CALLING_CONVENTION *freeMemory)(mumble_plugin_id_t callerID, const void* pointer);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getActiveServerConnection)(mumble_plugin_id_t callerID, mumble_connection_t* connection);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *isConnectionSynchronized)(mumble_plugin_id_t callerID, mumble_connection_t connection, bool* synchronized);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getLocalUserID)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_userid_t* userID);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getUserName)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_userid_t userID, const char** userName);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getChannelDescription)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_channelid_t channelID, const char** description);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *findChannelByName)(mumble_plugin_id_t callerID, mumble_connection_t connection, const char* channelName, mumble_channelid_t* channelID);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getChannelOfUser)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t* channelID);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *requestUserMove)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t channelID, const char* password);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getServerHash)(mumble_plugin_id_t callerID, mumble_connection_t connection, const char** serverHash);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *getAllUsers)(mumble_plugin_id_t callerID, mumble_connection_t connection, mumble_userid_t** users, size_t* userCount);
    mumble_error_t (PLUGIN_CALLING_CONVENTION *sendData)(mumble_plugin_id_t callerID, mumble_connection_t connection, const mumble_userid_t* users, size_t userCount, const uint8_t* data, size_t dataLength, const char* dataID);
    void (PLUGIN_CALLING_CONVENTION *log)(mumble_plugin_id_t callerID, const char* message);
};

#define MUMBLE_API_CAST(ptrName) (*(struct MumbleAPI_v_1_0_x*)(ptrName))

extern struct MumbleAPI_v_1_0_x mumbleAPI;

mumble_error_t mumble_init(mumble_plugin_id_t pluginID);
void mumble_registerAPIFunctions(void* apiStruct);
void mumble_shutdown(void);
void mumble_onServerConnected(mumble_connection_t connection);
void mumble_onServerSynchronized(mumble_connection_t connection);
void mumble_onServerDisconnected(mumble_connection_t connection);
void mumble_onChannelEntered(mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t previousChannelID, mumble_channelid_t newChannelID);
void mumble_onChannelExited(mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t channelID);
void mumble_onUserTalkingStateChanged(mumble_connection_t connection, mumble_userid_t userID, mumble_talking_state_t talkingState);
int showConfigInterface(void);

#endif /* MUMBLE_COMPAT_H */
