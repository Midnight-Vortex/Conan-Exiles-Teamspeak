#ifndef TS3_PLUGIN_VERSION_H
#define TS3_PLUGIN_VERSION_H

/*
 * Plugin version exchange (CEVER:) and TeamSpeak info-panel text.
 *
 * Thread contract:
 *  - ts3_version_broadcast / ts3_version_on_plugin_command / ts3_version_clear_client:
 *    TS callback thread ONLY (TS API for sendPluginCommand).
 *  - ts3_version_reset: any thread (disconnect cleanup).
 *  - ts3_version_format_info: any thread (TS UI info panel); uses an internal
 *    lock and never calls the TS API.
 */

#include "sdk/include/teamspeak/public_definitions.h"

void ts3_version_reset(void);
void ts3_version_clear_client(anyID clientID);
void ts3_version_broadcast(void);

/* Returns 1 when command was a CEVER message (handled or ignored). */
int ts3_version_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID, const char* invokerDisplayName, const char* invokerUniqueIdentity);

/* Fill info panel buffer. Returns bytes written (excluding NUL), 0 on failure. */
int ts3_version_format_info(anyID clientID, char* buf, size_t bufSize);

#endif /* TS3_PLUGIN_VERSION_H */
