#include "core/proximity/zone_resolve.h"

#include <math.h>

/* Ray casting (even-odd rule) — robust for convex quads of any winding. */
static int zone_point_in_quad(float px, float pz,
    float x1, float z1, float x2, float z2,
    float x3, float z3, float x4, float z4) {
    const float vx[4] = { x1, x2, x3, x4 };
    const float vz[4] = { z1, z2, z3, z4 };
    int crossings = 0;

    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        const float zi = vz[i], zj = vz[j];
        if ((zi <= pz && zj > pz) || (zj <= pz && zi > pz)) {
            const float dz = zj - zi;
            if (fabsf(dz) > 1e-6f) {
                const float intersectX = vx[i] + (pz - zi) * (vx[j] - vx[i]) / dz;
                if (px < intersectX) {
                    crossings++;
                }
            }
        }
    }
    return (crossings & 1) != 0;
}

/* xzFloor=1: X-Z floor + Y height (legacy layout).
   xzFloor=0: X-Y floor + Z height (Conan/UE: Z1..Z4 hold world Y). */
static int zone_contains_point(const HubZone* zone, float px, float py, float pz,
    int xzFloor) {
    const float hEps = 1.0f;
    const float height = xzFloor ? py : pz;
    const float horizA = px;
    const float horizB = xzFloor ? pz : py;

    if (zone->groundY != 0.0f || zone->topY != 0.0f) {
        const float hMin = zone->groundY < zone->topY ? zone->groundY : zone->topY;
        const float hMax = zone->groundY > zone->topY ? zone->groundY : zone->topY;
        if (height < hMin - hEps || height > hMax + hEps) {
            return 0;
        }
    }
    return zone_point_in_quad(horizA, horizB,
        zone->x1, zone->z1, zone->x2, zone->z2,
        zone->x3, zone->z3, zone->x4, zone->z4);
}

static int zone_resolve_at_scale(const HubSettings* settings,
    float x, float y, float z) {
    for (int i = 0; i < settings->zoneCount; i++) {
        const HubZone* zone = &settings->zones[i];
        /* Skip zones without corners (name-only entries). */
        if (zone->x1 == 0.0f && zone->x2 == 0.0f && zone->x3 == 0.0f && zone->x4 == 0.0f
            && zone->z1 == 0.0f && zone->z2 == 0.0f && zone->z3 == 0.0f && zone->z4 == 0.0f) {
            continue;
        }
        if (zone_contains_point(zone, x, y, z, 1)
            || zone_contains_point(zone, x, y, z, 0)) {
            return i;
        }
    }
    return -1;
}

int zone_resolve(const HubSettings* settings, float x, float y, float z) {
    if (!settings || settings->zoneCount <= 0) {
        return -1;
    }
    /* Meters first, then UE centimeters, then legacy /100 corners. */
    static const float scales[] = { 1.0f, 100.0f, 0.01f };
    for (int s = 0; s < 3; s++) {
        const int idx = zone_resolve_at_scale(settings,
            x * scales[s], y * scales[s], z * scales[s]);
        if (idx >= 0) {
            return idx;
        }
    }
    return -1;
}

int zone_soundproof_muted(const HubSettings* settings, int localZone, int remoteZone) {
    if (!settings || remoteZone < 0 || remoteZone >= settings->zoneCount) {
        return 0;
    }
    return settings->zones[remoteZone].soundproof && localZone != remoteZone;
}

int zone_reverb_active(const HubSettings* settings, int localZone, int remoteZone) {
    if (!settings) {
        return 0;
    }
    if (localZone >= 0 && localZone < settings->zoneCount
        && settings->zones[localZone].reverb) {
        return 1;
    }
    if (remoteZone >= 0 && remoteZone < settings->zoneCount
        && settings->zones[remoteZone].reverb) {
        return 1;
    }
    return 0;
}
