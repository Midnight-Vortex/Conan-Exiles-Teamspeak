#include "ts/proximity/ts3_3d.h"
#include "ts/adapter/ts3_adapter.h"
#include "core/proximity/player_table.h"
#include "core/proximity/proximity_math.h"
#include "core/mod_file/pos_file.h"
#include "core/util/log.h"
#include "ts/profile/ts3_server_profile.h"

#include "ts/proximity/ts3_client_limits.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#define TS3D_POS_EPS     0.25f  /* meters — skip API call below this move */
#define TS3D_FWD_EPS     0.02f  /* direction component change threshold */
#define TS3D_PI          3.14159265f
#define TS3D_APPLY_MIN_MS 50     /* 20 Hz cap */
#define TS3D_CULL_MARGIN  1.25f
#define TS3D_CULL_PAD_M   2.0f

/* A channelset3DAttributes call that failed is not retried for this long. */
#define TS3D_RETRY_BACKOFF_MS   1000
/* Max client-position API calls (successful or failed) per ts3d_apply pass.
   The rest continues round-robin on the next pass. */
#define TS3D_MAX_SETS_PER_PASS  64
/* ts3d_has_pending() stays true at most this long after a capped pass, so a
   dispatcher that never gets to run ts3d_apply again cannot spin on it. */
#define TS3D_PENDING_WINDOW_MS  500

/* All state below is TS callback thread only — no locks needed. */

/* 7.1 settings dedup */
static int g_settingsApplied = 0;

/* 7.2 listener dedup */
static struct {
    int valid;
    float x, y, z;
    float fx, fy, fz;
} g_lastListener;

/* 7.3 per-client dedup */
static float g_lastClientX[TS3_MAX_CLIENT_ID];
static float g_lastClientY[TS3_MAX_CLIENT_ID];
static float g_lastClientZ[TS3_MAX_CLIENT_ID];
static char  g_clientValid[TS3_MAX_CLIENT_ID];

/* 7.3b negative cache: tick (ms) until which a client whose last API call
   failed is skipped. 0 = no backoff. */
static ULONGLONG g_retryUntilMs[TS3_MAX_CLIENT_ID];

/* 7.5 per-pass cap bookkeeping */
static int g_rrCursor = 0;                 /* snapshot index where the next pass resumes */
static volatile long g_applyPending = 0;   /* a capped pass left work behind */
static ULONGLONG g_applyPendingUntilMs = 0;

/* ---- 7.1 one-time settings -------------------------------------------------- */

void ts3d_init(void) {
    if (g_settingsApplied) {
        return;
    }
    /* distanceFactor=1 -> positions are meters (Conan world coordinates).
       rolloffScale=0 -> distance curve comes from ts3d_on_custom_rolloff. */
    if (ts3_set_3d_settings(1.0f, 0.0f)) {
        g_settingsApplied = 1;
        log_write("TS-3D: settings applied (distanceFactor=1, rolloffScale=0)");
    }
}

/* ---- 7.2 listener ------------------------------------------------------------ */

/* Orthonormal basis from a look direction (TS wants unit forward ⟂ unit up). */
static void ts3d_basis_from_forward(float* fx, float* fy, float* fz,
    float* ux, float* uy, float* uz) {
    float fxx = *fx, fyy = *fy, fzz = *fz;
    float flen = sqrtf(fxx * fxx + fyy * fyy + fzz * fzz);
    if (flen > 0.0001f) {
        fxx /= flen; fyy /= flen; fzz /= flen;
    }
    else {
        fxx = 0.0f; fyy = 0.0f; fzz = 1.0f;
    }

    /* right = forward x worldUp; fall back to world X when looking straight up */
    float wx = 0.0f, wy = 1.0f, wz = 0.0f;
    float rx = fyy * wz - fzz * wy;
    float ry = fzz * wx - fxx * wz;
    float rz = fxx * wy - fyy * wx;
    float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rlen < 0.0001f) {
        wx = 1.0f; wy = 0.0f; wz = 0.0f;
        rx = fyy * wz - fzz * wy;
        ry = fzz * wx - fxx * wz;
        rz = fxx * wy - fyy * wx;
        rlen = sqrtf(rx * rx + ry * ry + rz * rz);
    }
    if (rlen > 0.0001f) {
        rx /= rlen; ry /= rlen; rz /= rlen;
    }

    /* up = right x forward */
    float uxx = ry * fzz - rz * fyy;
    float uyy = rz * fxx - rx * fzz;
    float uzz = rx * fyy - ry * fxx;
    float ulen = sqrtf(uxx * uxx + uyy * uyy + uzz * uzz);
    if (ulen > 0.0001f) {
        uxx /= ulen; uyy /= ulen; uzz /= ulen;
    }
    else {
        uxx = 0.0f; uyy = 1.0f; uzz = 0.0f;
    }

    *fx = fxx; *fy = fyy; *fz = fzz;
    *ux = uxx; *uy = uyy; *uz = uzz;
}

void ts3d_set_listener(float x, float y, float z,
    float fwdX, float fwdY, float fwdZ) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    float fx = fwdX, fy = fwdY, fz = fwdZ;
    float ux, uy, uz;
    ts3d_basis_from_forward(&fx, &fy, &fz, &ux, &uy, &uz);

    if (g_lastListener.valid
        && fabsf(x - g_lastListener.x) < TS3D_POS_EPS
        && fabsf(y - g_lastListener.y) < TS3D_POS_EPS
        && fabsf(z - g_lastListener.z) < TS3D_POS_EPS
        && fabsf(fx - g_lastListener.fx) < TS3D_FWD_EPS
        && fabsf(fy - g_lastListener.fy) < TS3D_FWD_EPS
        && fabsf(fz - g_lastListener.fz) < TS3D_FWD_EPS) {
        return;
    }

    const TS3_VECTOR position = { x, y, z };
    const TS3_VECTOR forward = { fx, fy, fz };
    const TS3_VECTOR up = { ux, uy, uz };
    if (!ts3_set_3d_listener(&position, &forward, &up)) {
        return;
    }

    g_lastListener.x = x;
    g_lastListener.y = y;
    g_lastListener.z = z;
    g_lastListener.fx = fx;
    g_lastListener.fy = fy;
    g_lastListener.fz = fz;
    g_lastListener.valid = 1;

    static ULONGLONG s_lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastLog >= 10000) {
        s_lastLog = now;
        log_debug("TS-3D: listener pos=X=%.6f Y=%.6f Z=%.6f fwd=X=%.6f Y=%.6f Z=%.6f",
            (double)x, (double)y, (double)z, (double)fx, (double)fy, (double)fz);
    }
}

/* ---- 7.3 client positions ----------------------------------------------------- */

/* Returns 1 when an API call was made (success OR failure) — the per-pass cap
   counts those; deduped / backed-off clients cost nothing and return 0. */
static int ts3d_push_client(anyID clientID, float x, float y, float z) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()
        || !ts3_client_id_valid(clientID)) {
        return 0;
    }
    if (g_clientValid[clientID]
        && fabsf(x - g_lastClientX[clientID]) < TS3D_POS_EPS
        && fabsf(y - g_lastClientY[clientID]) < TS3D_POS_EPS
        && fabsf(z - g_lastClientZ[clientID]) < TS3D_POS_EPS) {
        return 0;
    }

    /* Negative cache: after a failed call, leave this client alone for a
       moment instead of retrying (and re-failing) on every pass. */
    if (g_retryUntilMs[clientID] != 0) {
        if (GetTickCount64() < g_retryUntilMs[clientID]) {
            return 0;
        }
        g_retryUntilMs[clientID] = 0;
    }

    const TS3_VECTOR position = { x, y, z };
    if (!ts3_set_3d_client(clientID, &position)) {
        g_retryUntilMs[clientID] = GetTickCount64() + TS3D_RETRY_BACKOFF_MS;
        return 1;
    }
    g_lastClientX[clientID] = x;
    g_lastClientY[clientID] = y;
    g_lastClientZ[clientID] = z;
    g_clientValid[clientID] = 1;
    return 1;
}

void ts3d_set_client_pos(anyID clientID, float x, float y, float z) {
    (void)ts3d_push_client(clientID, x, y, z);
}

/* ---- 7.4 custom rolloff (audio thread — pure) ---------------------------------- */

void ts3d_on_custom_rolloff(anyID clientID, float distance, float* volume) {
    (void)clientID;
    (void)distance;
    if (!volume) {
        return;
    }
    /* Distance attenuation runs in the PCM path (Phase 6). A non-neutral curve
       here would stack with the PCM gain and double-mute (old plugin lesson). */
    *volume = 1.0f;
}

/* ---- apply (callback thread driver) --------------------------------------------- */

static int ts3d_in_hear_range(const PosSample* local, const PlayerEntry* remote) {
    const float lx = (float)(local->x / 100.0);
    const float ly = (float)(local->y / 100.0);
    const float lz = (float)(local->z / 100.0);
    const float dist = prox_distance(lx, ly, lz, remote->x, remote->y, remote->z);
    const float hearRange = remote->voiceDistance + server_profile_get_listen_add_distance();
    return dist <= hearRange * TS3D_CULL_MARGIN + TS3D_CULL_PAD_M;
}

void ts3d_apply(void) {
    if (!ts3_thread_is_callback() || !ts3_is_connected()) {
        return;
    }

    static ULONGLONG s_lastApplyMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastApplyMs < TS3D_APPLY_MIN_MS) {
        return;
    }
    s_lastApplyMs = now;

    PosSample local;
    if (!pos_get_current(&local)) {
        InterlockedExchange(&g_applyPending, 0);
        return; /* no fresh own position — keep last known 3D state */
    }

    ts3d_init();

    /* Same yaw -> direction mapping as the CEPOS packet (cepos_build_local). */
    const float yawRad = (float)(local.yaw * TS3D_PI / 180.0);
    const float fwdX = -cosf(yawRad);
    const float fwdY = sinf((float)(-local.yawY * TS3D_PI / 180.0));
    const float fwdZ = -sinf(yawRad);

    ts3d_set_listener((float)(local.x / 100.0), (float)(local.y / 100.0), (float)(local.z / 100.0),
        fwdX, fwdY, fwdZ);

    PlayerEntry players[PLAYER_TABLE_MAX_PLAYERS];
    const int count = player_table_snapshot(players, PLAYER_TABLE_MAX_PLAYERS);

    /* At most TS3D_MAX_SETS_PER_PASS API calls per pass (jittering peers would
       otherwise cost one call each, every pass). A capped pass resumes at the
       next unvisited snapshot index, so every client gets its turn. */
    int sets = 0;
    int visited = 0;
    const int start = (count > 0 && g_rrCursor >= 0 && g_rrCursor < count) ? g_rrCursor : 0;
    while (visited < count) {
        const int i = (start + visited) % count;
        visited++;
        if (!ts3_client_id_valid(players[i].clientID)) {
            continue;
        }
        if (!ts3d_in_hear_range(&local, &players[i])) {
            continue;
        }
        sets += ts3d_push_client(players[i].clientID,
            players[i].x, players[i].y, players[i].z);
        if (sets >= TS3D_MAX_SETS_PER_PASS) {
            break;
        }
    }

    if (visited < count) {
        g_rrCursor = (start + visited) % count;
        g_applyPendingUntilMs = now + TS3D_PENDING_WINDOW_MS;
        InterlockedExchange(&g_applyPending, 1);
    }
    else {
        g_rrCursor = 0;
        InterlockedExchange(&g_applyPending, 0);
    }
}

int ts3d_has_pending(void) {
    return InterlockedCompareExchange(&g_applyPending, 0, 0) != 0
        && GetTickCount64() < g_applyPendingUntilMs;
}

/* ---- reset ------------------------------------------------------------------------ */

void ts3d_invalidate_client(anyID clientID) {
    if (!ts3_client_id_valid(clientID)) {
        return;
    }
    g_clientValid[clientID] = 0;
    g_retryUntilMs[clientID] = 0;
    g_lastClientX[clientID] = 0.0f;
    g_lastClientY[clientID] = 0.0f;
    g_lastClientZ[clientID] = 0.0f;
}

void ts3d_reset(void) {
    g_settingsApplied = 0;
    memset(&g_lastListener, 0, sizeof(g_lastListener));
    memset(g_clientValid, 0, sizeof(g_clientValid));
    memset(g_retryUntilMs, 0, sizeof(g_retryUntilMs));
    g_rrCursor = 0;
    g_applyPendingUntilMs = 0;
    InterlockedExchange(&g_applyPending, 0);
}
