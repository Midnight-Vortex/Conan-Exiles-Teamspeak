#include "core/proximity/proximity_math.h"
#include "core/util/log.h"

#include <math.h>

float prox_distance(float x1, float y1, float z1, float x2, float y2, float z2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float dz = z2 - z1;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

float prox_volume_from_distance(float distanceMeters, float voiceDistanceMeters,
    float maxVolume) {
    float effectiveVoiceDistance = voiceDistanceMeters;
    if (effectiveVoiceDistance < 1.0f) {
        effectiveVoiceDistance = 1.0f;
    }

    if (maxVolume < 0.1f) {
        maxVolume = 0.1f;
    }

    float distance = distanceMeters;
    if (distance < 0.1f) {
        distance = 0.1f;
    }

    /* norm=1.0 = nominal mode range; soft tail ends at fadeEnd. */
    const float fadeEnd = 1.12f;
    const float norm = distance / effectiveVoiceDistance;
    if (norm >= fadeEnd) {
        return 0.0f;
    }

    /* Inverse-square body: audible rolloff from ~25% of range. */
    const float refNorm = 0.42f;
    const float n = norm / refNorm;
    float volume = maxVolume / (1.0f + n * n);

    /* Progressive knee from mid-range — avoids staying loud until the edge. */
    const float kneeStart = 0.52f;
    if (norm > kneeStart) {
        float t = (norm - kneeStart) / (1.0f - kneeStart);
        if (t > 1.0f) {
            t = 1.0f;
        }
        const float smooth = t * t * (3.0f - 2.0f * t);
        volume *= 1.0f - smooth * 0.92f;
    }

    /* Soft tail past nominal range — no hard cut at norm=1.0. */
    if (norm > 1.0f) {
        float t = (norm - 1.0f) / (fadeEnd - 1.0f);
        if (t > 1.0f) {
            t = 1.0f;
        }
        const float smooth = t * t * (3.0f - 2.0f * t);
        volume *= 1.0f - smooth;
    }

    /* Slight near-field lift within the first 15% of range. */
    if (norm < 0.15f) {
        const float nearNorm = norm / 0.15f;
        volume *= 1.0f + 0.06f * (1.0f - nearNorm);
    }

    if (volume < 0.0f) {
        volume = 0.0f;
    }
    if (volume > maxVolume) {
        volume = maxVolume;
    }
    return volume;
}

void prox_stereo_pan(float localDirX, float localDirZ,
    float toRemoteX, float toRemoteZ,
    float* outLeft, float* outRight) {
    if (!outLeft || !outRight) {
        return;
    }

    /* Normalize the horizontal to-remote vector. */
    float len = sqrtf(toRemoteX * toRemoteX + toRemoteZ * toRemoteZ);
    if (len > 1e-6f) {
        toRemoteX /= len;
        toRemoteZ /= len;
    }
    else {
        /* Same spot — centered. */
        *outLeft = 0.7071f;
        *outRight = 0.7071f;
        return;
    }

    /* Cross product Y component: positive = speaker on the left. */
    const float leftRight = localDirX * toRemoteZ - localDirZ * toRemoteX;

    const float PAN_SPREAD = 1.45f;
    float pan = leftRight * PAN_SPREAD;
    if (pan > 1.0f) {
        pan = 1.0f;
    }
    else if (pan < -1.0f) {
        pan = -1.0f;
    }

    /* pan +1 = left, -1 = right; equal-power law. */
    const float angle = (1.0f - pan) * 0.5f * 1.5707963f;
    *outLeft = cosf(angle);
    *outRight = sinf(angle);
}

void prox_math_self_test(void) {
    const float voiceDist = 13.0f;
    log_write("PROX-TEST: volume curve (voiceDistance=%.1f, maxVolume=1.0)", voiceDist);

    static const float testDistances[] = { 0.0f, 1.0f, 3.0f, 6.5f, 10.0f, 13.0f, 14.5f, 20.0f };
    for (int i = 0; i < 8; i++) {
        const float d = testDistances[i];
        log_write("PROX-TEST:   d=%5.1fm -> vol=%.3f",
            d, prox_volume_from_distance(d, voiceDist, 1.0f));
    }

    /* Listener looks along -X (yaw 0 in CEPOS convention: dir=(-1,0,0)). */
    float l, r;
    prox_stereo_pan(-1.0f, 0.0f, 0.0f, 1.0f, &l, &r);
    log_write("PROX-TEST: pan remote at +Z (right of -X gaze): L=%.3f R=%.3f", l, r);
    prox_stereo_pan(-1.0f, 0.0f, 0.0f, -1.0f, &l, &r);
    log_write("PROX-TEST: pan remote at -Z (left  of -X gaze): L=%.3f R=%.3f", l, r);
    prox_stereo_pan(-1.0f, 0.0f, -1.0f, 0.0f, &l, &r);
    log_write("PROX-TEST: pan remote straight ahead:           L=%.3f R=%.3f", l, r);

    log_write("PROX-TEST: distance (0,0,0)->(3,4,0) = %.1f (expect 5.0)",
        prox_distance(0, 0, 0, 3, 4, 0));
}
