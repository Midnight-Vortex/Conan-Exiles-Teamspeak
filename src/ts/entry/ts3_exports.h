#ifndef TS3_EXPORTS_H
#define TS3_EXPORTS_H

/*
 * TeamSpeak SDK plugin export declarations.
 *
 * Phase 0: only the exports TeamSpeak requires to load/unload the plugin.
 * Further ts3plugin_* callbacks are added here in the phase that implements them.
 *
 * Thread contract: all functions declared here are called by TeamSpeak on its
 * own threads (UI/callback thread). None of them may block.
 */

#include "sdk/include/ts3_functions.h"
#include "sdk/include/teamspeak/public_definitions.h"
#include "sdk/include/plugin_definitions.h"

#if defined(WIN32) || defined(__WIN32__) || defined(_WIN32)
#define PLUGINS_EXPORTDLL __declspec(dllexport)
#else
#define PLUGINS_EXPORTDLL __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

PLUGINS_EXPORTDLL const char* ts3plugin_name(void);
PLUGINS_EXPORTDLL const char* ts3plugin_version(void);
PLUGINS_EXPORTDLL int ts3plugin_apiVersion(void);
PLUGINS_EXPORTDLL const char* ts3plugin_author(void);
PLUGINS_EXPORTDLL const char* ts3plugin_description(void);
PLUGINS_EXPORTDLL void ts3plugin_setFunctionPointers(const struct TS3Functions funcs);
PLUGINS_EXPORTDLL int ts3plugin_init(void);
PLUGINS_EXPORTDLL void ts3plugin_shutdown(void);
PLUGINS_EXPORTDLL void ts3plugin_registerPluginID(const char* id);
PLUGINS_EXPORTDLL void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber);
PLUGINS_EXPORTDLL void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID);
PLUGINS_EXPORTDLL void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity);

#ifdef __cplusplus
}
#endif

#endif /* TS3_EXPORTS_H */
