#ifndef TS3_PROXIMITY_TS3_CEPOS_H
#define TS3_PROXIMITY_TS3_CEPOS_H

/*
 * CEPOS — positional data protocol, wire-compatible with the old plugin.
 *
 * Wire format: plugin command "CEPOS:<base64 of CeposPacket>", plugin data ID
 * "ConanExiles_CompletePositional". Packet layout must match the old plugin
 * exactly (pack(1), 56 bytes) so mixed old/new clients keep working.
 *
 * Thread contract:
 *  - cepos_signal_send_pending: any thread (pos watcher). Sets a flag and
 *    requests a wakeup; never touches the TS API.
 *  - cepos_flush / cepos_on_plugin_command: TS callback thread ONLY.
 *  - cepos_reset: TS callback thread (disconnect).
 */

#include "sdk/include/teamspeak/public_definitions.h"

#pragma pack(push, 1)
typedef struct CeposPacket {
    float x, y, z;             /* meters */
    float dirX, dirY, dirZ;    /* look direction */
    float axisX, axisY, axisZ; /* up vector */
    float voiceDistance;       /* meters */
    char playerName[16];
} CeposPacket;
#pragma pack(pop)

/* Mark that a send may be due (position changed or keepalive). Any thread. */
void cepos_signal_send_pending(void);

/* Send own position if pending/keepalive and changed; on-change + 1 Hz
   keepalive, min 50 ms between sends. TS callback thread ONLY. */
void cepos_flush(void);

/* Handle an incoming plugin command. Returns 1 when it was a CEPOS command
   (handled), 0 otherwise. TS callback thread ONLY. */
int cepos_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID);

/* Clear send cache (disconnect / reconnect). */
void cepos_reset(void);

#endif /* TS3_PROXIMITY_TS3_CEPOS_H */
