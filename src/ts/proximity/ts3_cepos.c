#include "ts/proximity/ts3_cepos.h"
#include "ts/proximity/ts3_cepos_wire.h"
#include "ts/proximity/ts3_proximity_audio.h"
#include "ts/adapter/ts3_adapter.h"
#include "ts/profile/ts3_server_profile.h"
#include "core/voice/voice_modes.h"
#include "core/mod_file/pos_file.h"
#include "core/proximity/player_table.h"
#include "ts/proximity/ts3_client_limits.h"
#include "core/config/config.h"
#include "core/util/log.h"
#include "core/util/poll_interval.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CEPOS_CMD_PREFIX          "CEPOS:"
#define CEPOS_SEND_MIN_MS         PLUGIN_POLL_INTERVAL_MS
#define CEPOS_KEEPALIVE_MS        1000
#define CEPOS_NICK_REFRESH_MS     1000   /* own nickname re-read at most this often */
#define CEPOS_POS_EPS             0.08f
#define CEPOS_VOICE_EPS           0.05f
#define CEPOS_PI                  3.14159265f

/* Send state — TS callback thread only (no lock needed). */
static CeposPacket g_lastSent;
static int g_lastSentValid = 0;
static ULONGLONG g_lastSendMs = 0;

/* Own nickname cache. Reading it costs a TS API call + freeMemory, so it is
   refreshed at most every CEPOS_NICK_REFRESH_MS instead of on every 30 ms
   flush. Stored already truncated to the packet field (15 chars + NUL) so it
   compares 1:1 with g_lastSent.playerName. A rename therefore reaches the
   peers with up to CEPOS_NICK_REFRESH_MS delay. */
static char g_ownNick[sizeof(((CeposPacket*)0)->playerName)];
static int g_ownNickValid = 0;
static ULONGLONG g_ownNickRefreshMs = 0;

/* Set from any thread (voice mode switch), consumed on the callback thread. */
static volatile long g_sendCacheInvalid = 0;

void cepos_invalidate_send_cache(void) {
    InterlockedExchange(&g_sendCacheInvalid, 1);
}

/* Set from any thread, consumed on the callback thread. */
static volatile long g_sendPending = 0;

/* ---- base64 (wire-compatible with the old plugin) ------------------------ */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t* input, size_t inputLen, char* output, size_t outputMax) {
    size_t i = 0, j = 0;
    while (i < inputLen) {
        size_t remaining = inputLen - i;
        uint32_t a = input[i++];
        uint32_t b = remaining > 1 ? input[i++] : 0;
        uint32_t c = remaining > 2 ? input[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        if (j + 4 >= outputMax) {
            return 0;
        }
        output[j++] = b64_table[(triple >> 18) & 0x3F];
        output[j++] = b64_table[(triple >> 12) & 0x3F];
        output[j++] = (remaining > 1) ? b64_table[(triple >> 6) & 0x3F] : '=';
        output[j++] = (remaining > 2) ? b64_table[triple & 0x3F] : '=';
    }
    if (j < outputMax) {
        output[j] = '\0';
    }
    return j;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t base64_decode(const char* input, uint8_t* output, size_t outputMax) {
    size_t len = strlen(input);
    size_t j = 0;
    uint32_t val = 0;
    int valb = -8;

    for (size_t k = 0; k < len; k++) {
        if (input[k] == '=') {
            break;
        }
        int v = b64_value(input[k]);
        if (v < 0) {
            continue;
        }
        val = (val << 6) + (uint32_t)v;
        valb += 6;
        if (valb >= 0) {
            if (j >= outputMax) {
                return 0;
            }
            output[j++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return j;
}

/* ---- 4.1 build local packet ----------------------------------------------- */

/* Pure: Pos.txt sample (cm + yaw degrees) -> wire packet (meters + vectors).
   Voice distance and name are passed in: the caller already has them from its
   change check, so building the packet never calls the TS API again. */
static void cepos_build_local(const PosSample* sample, float voiceDistance,
    const char* name, CeposPacket* out) {
    memset(out, 0, sizeof(*out));

    out->x = (float)(sample->x / 100.0);
    out->y = (float)(sample->y / 100.0);
    out->z = (float)(sample->z / 100.0);

    const float yawRad = (float)(sample->yaw * CEPOS_PI / 180.0);
    out->dirX = -cosf(yawRad);
    out->dirY = sinf((float)(-sample->yawY * CEPOS_PI / 180.0));
    out->dirZ = -sinf(yawRad);

    out->axisX = 0.0f;
    out->axisY = 1.0f;
    out->axisZ = 0.0f;

    /* Active voice mode (Phase 11): zone override + profile clamp included. */
    out->voiceDistance = voiceDistance;

    strncpy_s(out->playerName, sizeof(out->playerName), name, _TRUNCATE);
}

/* Re-read the own nickname when the cache is older than CEPOS_NICK_REFRESH_MS
   (or empty, or `force`). TS callback thread. A failed read keeps the last
   good name; "Player" is only the fallback when none was ever read (same
   fallback text as before). */
static void cepos_refresh_own_nick(ULONGLONG now, int force) {
    if (!force && g_ownNickValid
        && now - g_ownNickRefreshMs < CEPOS_NICK_REFRESH_MS) {
        return;
    }
    g_ownNickRefreshMs = now;

    char nick[64] = "";
    if (ts3_get_own_nickname(nick, sizeof(nick)) && nick[0]) {
        strncpy_s(g_ownNick, sizeof(g_ownNick), nick, _TRUNCATE);
        g_ownNickValid = 1;
    }
    else if (!g_ownNickValid) {
        strcpy_s(g_ownNick, sizeof(g_ownNick), "Player");
        g_ownNickValid = 1;
    }
}

/* Own position moved far enough (vs the last packet that went out)? Compares
   in meters exactly like the packet fields, so the result equals the old
   packet-vs-packet check — without building a packet first. */
static int cepos_position_changed(const PosSample* sample) {
    if (!g_lastSentValid) {
        return 1;
    }
    return fabsf((float)(sample->x / 100.0) - g_lastSent.x) >= CEPOS_POS_EPS
        || fabsf((float)(sample->y / 100.0) - g_lastSent.y) >= CEPOS_POS_EPS
        || fabsf((float)(sample->z / 100.0) - g_lastSent.z) >= CEPOS_POS_EPS;
}

/* ---- 4.2 send --------------------------------------------------------------- */

void cepos_signal_send_pending(void) {
    /* Only one server-wide CEDRAIN round trip per pending cycle — re-armed
       from cepos_flush when rate-limited or keepalive still due. */
    if (InterlockedCompareExchange(&g_sendPending, 1, 0) == 0) {
        ts3_request_wakeup();
    }
}

int cepos_send_pending(void) {
    return InterlockedCompareExchange(&g_sendPending, 0, 0) != 0;
}

void cepos_flush(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }
    if (InterlockedCompareExchange(&g_sendPending, 0, 0) == 0) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSendMs < CEPOS_SEND_MIN_MS) {
        ts3_request_wakeup(); /* retry after rate limit (already throttled) */
        return;
    }

    PosSample sample;
    if (!pos_get_current(&sample)) {
        InterlockedExchange(&g_sendPending, 0);
        return;
    }

    if (InterlockedExchange(&g_sendCacheInvalid, 0)) {
        g_lastSentValid = 0; /* voice mode switched — force this send out */
    }

    /* Cheap change check FIRST. The old flow built the whole packet (own
       nickname via the TS API + freeMemory, string compare, trig) on every
       30 ms flush just to find out that nothing had changed. Now: keepalive /
       forced / position first (pure arithmetic), then the voice distance, and
       the nickname only when its 1 s cache is stale. A packet is built only
       when a send really goes out. */
    const int keepaliveDue = (g_lastSendMs == 0 || now - g_lastSendMs >= CEPOS_KEEPALIVE_MS);
    int sendNow = !g_lastSentValid || keepaliveDue || cepos_position_changed(&sample);

    cepos_refresh_own_nick(now, keepaliveDue);
    if (!sendNow && strncmp(g_ownNick, g_lastSent.playerName, sizeof(g_ownNick)) != 0) {
        sendNow = 1; /* rename noticed by the 1 s refresh */
    }

    /* Evaluated on every flush on purpose: hub/profile/race changes alter the
       distance without any movement, and those must still go out at once. */
    const float voiceDistance = voice_mode_get_current_distance();
    if (!sendNow && fabsf(voiceDistance - g_lastSent.voiceDistance) >= CEPOS_VOICE_EPS) {
        sendNow = 1;
    }

    if (!sendNow) {
        InterlockedExchange(&g_sendPending, 0);
        return;
    }

    CeposPacket packet;
    cepos_build_local(&sample, voiceDistance, g_ownNick, &packet);

    char encoded[128];
    if (base64_encode((const uint8_t*)&packet, sizeof(packet), encoded, sizeof(encoded)) == 0) {
        InterlockedExchange(&g_sendPending, 0);
        return;
    }

    char command[160];
    snprintf(command, sizeof(command), "%s%s", CEPOS_CMD_PREFIX, encoded);
    if (ts3_send_plugin_command_server(command)) {
        g_lastSent = packet;
        g_lastSentValid = 1;
        g_lastSendMs = now;

        static ULONGLONG s_lastSendLog = 0;
        if (now - s_lastSendLog >= 10000) {
            s_lastSendLog = now;
            log_debug("CEPOS: SEND name='%.15s' pos=X=%.6f Y=%.6f Z=%.6f voiceDist=%.6f",
                packet.playerName, packet.x, packet.y, packet.z, packet.voiceDistance);
        }
    }
    InterlockedExchange(&g_sendPending, 0);
}

/* ---- 4.3 receive ------------------------------------------------------------ */

static int cepos_plugin_name_matches(const char* pluginName) {
    if (!pluginName) {
        return 0;
    }
    if (strcmp(pluginName, ts3_get_plugin_id()) == 0) {
        return 1;
    }
    /* Accept the old plugin's IDs so mixed versions still exchange positions. */
    return strcmp(pluginName, "conan_exiles") == 0
        || strcmp(pluginName, "conan_exiles_ts") == 0;
}

int cepos_on_plugin_command(const char* pluginName, const char* pluginCommand,
    anyID invokerClientID) {
    if (!pluginCommand || strncmp(pluginCommand, CEPOS_CMD_PREFIX, 6) != 0) {
        return 0;
    }
    if (!cepos_plugin_name_matches(pluginName)) {
        return 1; /* CEPOS from a foreign plugin id — handled (ignored) */
    }

    /* Own broadcast comes back to us too — skip it. */
    if (invokerClientID != 0 && invokerClientID == ts3_get_local_client_id()) {
        return 1;
    }
    if (!ts3_client_id_valid(invokerClientID)) {
        return 1;
    }

    uint8_t buffer[128];
    size_t decoded = base64_decode(pluginCommand + 6, buffer, sizeof(buffer));
    if (decoded != sizeof(CeposPacket)) {
        static ULONGLONG s_lastDecodeFailLog = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastDecodeFailLog > 10000) {
            s_lastDecodeFailLog = now;
            log_debug("CEPOS: RECV decode fail from=%u expected=%zu got=%zu",
                (unsigned)invokerClientID, sizeof(CeposPacket), decoded);
        }
        return 1;
    }

    const CeposPacket* packet = (const CeposPacket*)buffer;

    /* playerName from an untrusted peer may lack the NUL terminator. */
    char safeName[PLAYER_NAME_LEN];
    memcpy(safeName, packet->playerName, sizeof(packet->playerName));
    safeName[PLAYER_NAME_LEN - 1] = '\0';
    /* ...and may carry control bytes (newline, ESC ...): strip them before the
       name is stored or logged, so a peer cannot forge log lines. */
    cepos_wire_sanitize_name(safeName, sizeof(safeName));

    /* Reject broken values before they reach the table. */
    if (!isfinite(packet->x) || !isfinite(packet->y) || !isfinite(packet->z)
        || !isfinite(packet->voiceDistance)
        || packet->voiceDistance < 0.0f || packet->voiceDistance > 1000.0f) {
        return 1;
    }
    if (fabsf(packet->x) < 0.01f && fabsf(packet->y) < 0.01f && fabsf(packet->z) < 0.01f) {
        return 1;
    }

    unsigned short evicted = 0;
    if (!player_table_put(invokerClientID, safeName,
            packet->x, packet->y, packet->z, packet->voiceDistance, &evicted)) {
        static ULONGLONG s_lastTableFullLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastTableFullLog > 10000) {
            s_lastTableFullLog = now;
            log_write("CEPOS: player table full - dropped client %u", (unsigned)invokerClientID);
        }
        return 1;
    }

    if (evicted != 0) {
        ts3_audio_invalidate_client(evicted);
    }

    /* Queue batched recompute in CEDRAIN (Phase 4.2). */
    ts3_audio_mark_client_dirty(invokerClientID);

    {
        /* Per-client throttled receive log (fixed slots, no overflow). */
        enum { RECV_LOG_SLOTS = 64 };
        static struct {
            anyID clientID;
            ULONGLONG lastLog;
        } s_recvLog[RECV_LOG_SLOTS];
        const ULONGLONG now = GetTickCount64();
        const size_t slot = (size_t)(invokerClientID % RECV_LOG_SLOTS);
        if (s_recvLog[slot].clientID != invokerClientID
            || now - s_recvLog[slot].lastLog >= 10000) {
            s_recvLog[slot].clientID = invokerClientID;
            s_recvLog[slot].lastLog = now;
            log_debug("CEPOS: RECV from=%u name='%s' pos=X=%.6f Y=%.6f Z=%.6f voiceDist=%.6f",
                (unsigned)invokerClientID, safeName,
                packet->x, packet->y, packet->z, packet->voiceDistance);
        }
    }
    return 1;
}

void cepos_reset(void) {
    g_lastSentValid = 0;
    g_lastSendMs = 0;
    g_ownNickValid = 0; /* new connection: re-read the nickname on the next send */
    g_ownNickRefreshMs = 0;
    InterlockedExchange(&g_sendPending, 0);
    player_table_clear();
}
