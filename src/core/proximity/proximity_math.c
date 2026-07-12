#include "core/proximity/proximity_math.h"
#include "core/util/log.h"
#include "core/util/poll_interval.h"

#include <math.h>

/* Match ts3_cepos.c — movement self-test models discrete CEPOS sends. */
#define PROX_TEST_CEPOS_EPS_M       0.08f

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

float proximity_calculate_volume_with_hub(float distanceMeters, float voiceDistanceMeters,
    const ProximityVolumeContext* ctx) {
    if (!ctx) {
        return 0.0f;
    }

    float effectiveVoiceDistance = voiceDistanceMeters;
    if (ctx->currentPlayerRaceIndex != -1 && ctx->currentListenAddDistance > 0.0f) {
        effectiveVoiceDistance += ctx->currentListenAddDistance;
    }

    float maxVolumeFromServer = (float)(ctx->hubAudioMaxVolume / 100.0);
    if (ctx->currentZoneIndex != -1) {
        maxVolumeFromServer = (float)(ctx->zoneAudioMaxVolume / 100.0);
    }

    return prox_volume_from_distance(distanceMeters, effectiveVoiceDistance, maxVolumeFromServer);
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

float prox_lowpass_cutoff_hz(float distanceMeters) {
    const float MAX_CUTOFF = 8000.0f;
    const float MIN_CUTOFF = 900.0f;

    float d = distanceMeters;
    if (d < 1.0f) {
        d = 1.0f;
    }

    float cutoff = MAX_CUTOFF * expf(-0.15f * logf(d + 1.0f));
    if (cutoff < MIN_CUTOFF) {
        cutoff = MIN_CUTOFF;
    }
    if (cutoff > MAX_CUTOFF) {
        cutoff = MAX_CUTOFF;
    }
    return cutoff;
}

float prox_direct_reverb_ratio(float distanceMeters, float referenceDistanceMeters) {
    float d = distanceMeters;
    float ref = referenceDistanceMeters;
    if (ref < 0.1f) {
        ref = 0.1f;
    }
    if (d < 0.1f) {
        d = 0.1f;
    }
    float drr = 1.0f / (1.0f + (d * d) / (ref * ref));
    if (drr < 0.05f) {
        drr = 0.05f;
    }
    if (drr > 1.0f) {
        drr = 1.0f;
    }
    return drr;
}

float prox_front_back_dot(float localDirX, float localDirZ,
    float toRemoteX, float toRemoteZ) {
    const float len = sqrtf(toRemoteX * toRemoteX + toRemoteZ * toRemoteZ);
    if (len <= 1e-6f) {
        return 1.0f;
    }
    return localDirX * (toRemoteX / len) + localDirZ * (toRemoteZ / len);
}

void prox_rear_psychoacoustics(float frontBack, ProxRearPsycho* out) {
    if (!out) {
        return;
    }
    out->directionVolume = 1.0f;
    out->cutoffMul = 1.0f;
    out->drrMul = 1.0f;

    float rearFactor = 0.0f;
    if (frontBack < 0.0f) {
        rearFactor = -frontBack;
        if (rearFactor > 1.0f) {
            rearFactor = 1.0f;
        }
    }
    if (rearFactor <= 0.0f) {
        return;
    }

    out->directionVolume = 1.0f - rearFactor * 0.12f;
    out->cutoffMul = 0.75f;
    out->drrMul = 0.85f;
}

static float prox_clamp_short(float v) {
    if (v > 32767.0f) {
        return 32767.0f;
    }
    if (v < -32768.0f) {
        return -32768.0f;
    }
    return v;
}

void prox_apply_diffuse_samples(short* samples, int sampleCount, int channelCount,
    float drr) {
    if (!samples || sampleCount < 2 || channelCount < 1 || drr >= 0.99f) {
        return;
    }
    if (drr < 0.05f) {
        drr = 0.05f;
    }

    const float directGain = drr;
    const float diffuseGain = 1.0f - drr;

    if (channelCount == 1) {
        float prev = (float)samples[0];
        for (int i = 1; i < sampleCount; i++) {
            const float direct = (float)samples[i];
            const float diffuse = (prev + direct) * 0.5f;
            prev = direct;
            samples[i] = (short)prox_clamp_short(direct * directGain + diffuse * diffuseGain);
        }
    }
    else {
        float prevL = (float)samples[0];
        float prevR = (float)samples[1];
        for (int s = 1; s < sampleCount; s++) {
            const int leftIdx = s * channelCount;
            const int rightIdx = leftIdx + 1;

            const float directL = (float)samples[leftIdx];
            const float directR = (float)samples[rightIdx];
            const float diffuseL = (prevL + directL) * 0.5f;
            const float diffuseR = (prevR + directR) * 0.5f;

            prevL = directL;
            prevR = directR;

            samples[leftIdx] = (short)prox_clamp_short(
                directL * directGain + diffuseL * diffuseGain);
            samples[rightIdx] = (short)prox_clamp_short(
                directR * directGain + diffuseR * diffuseGain);
        }
    }
}

typedef struct ProxMoveProfile {
    const char* label;
    float speedMps;
} ProxMoveProfile;

/* Approximate Conan Exiles horizontal speeds (m/s). */
static const ProxMoveProfile s_moveProfiles[] = {
    { "walk",   3.0f },
    { "jog",    5.5f },
    { "sprint", 7.5f },
    { "mount",  12.0f },
    { "fly",    25.0f },
};

static void prox_test_flyby(float voiceDist, float cpaMeters) {
    const float dt = PLUGIN_POLL_INTERVAL_MS / 1000.0f;
    const float startZ = -(voiceDist * 1.15f + 10.0f);
    const float endZ = -startZ;

    log_write("PROX-TEST: fly-by CPA=%.0fm voice=%.0fm (dt=%dms eps=%.2fm)",
        cpaMeters, voiceDist, PLUGIN_POLL_INTERVAL_MS, PROX_TEST_CEPOS_EPS_M);

    for (int p = 0; p < (int)(sizeof(s_moveProfiles) / sizeof(s_moveProfiles[0])); p++) {
        const ProxMoveProfile* profile = &s_moveProfiles[p];
        float z = startZ;
        float lastSentZ = startZ - PROX_TEST_CEPOS_EPS_M * 2.0f;
        int ceposUpdates = 0;
        float audibleMs = 0.0f;
        float peakVol = 0.0f;
        float minDist = 1e9f;
        float volAtCpa = 0.0f;
        int loggedCpa = 0;

        while (z <= endZ + 1e-4f) {
            const float dist = sqrtf(cpaMeters * cpaMeters + z * z);
            const float vol = prox_volume_from_distance(dist, voiceDist, 1.0f);

            if (dist < minDist) {
                minDist = dist;
            }
            if (!loggedCpa && z >= -PROX_TEST_CEPOS_EPS_M) {
                volAtCpa = vol;
                loggedCpa = 1;
            }
            if (vol > 0.001f) {
                audibleMs += dt * 1000.0f;
            }
            if (vol > peakVol) {
                peakVol = vol;
            }
            if (fabsf(z - lastSentZ) >= PROX_TEST_CEPOS_EPS_M) {
                ceposUpdates++;
                lastSentZ = z;
            }

            z += profile->speedMps * dt;
        }

        log_write("PROX-TEST:   %-6s %4.1f m/s -> audible %4.0fms peak=%.3f cpaVol=%.3f minDist=%.1fm cepos=%d",
            profile->label, profile->speedMps, audibleMs, peakVol, volAtCpa, minDist, ceposUpdates);
    }
}

static void prox_test_approach(float voiceDist) {
    const float dt = PLUGIN_POLL_INTERVAL_MS / 1000.0f;
    const float startDist = voiceDist * 1.25f + 5.0f;

    log_write("PROX-TEST: head-on approach from %.0fm voice=%.0fm (dt=%dms eps=%.2fm)",
        startDist, voiceDist, PLUGIN_POLL_INTERVAL_MS, PROX_TEST_CEPOS_EPS_M);

    for (int p = 0; p < (int)(sizeof(s_moveProfiles) / sizeof(s_moveProfiles[0])); p++) {
        const ProxMoveProfile* profile = &s_moveProfiles[p];
        float dist = startDist;
        float lastSentDist = startDist + PROX_TEST_CEPOS_EPS_M;
        int ceposUpdates = 0;
        float timeToAudibleMs = -1.0f;
        float timeToPeakMs = 0.0f;
        float peakVol = 0.0f;
        float elapsedMs = 0.0f;

        while (dist > 0.5f) {
            const float vol = prox_volume_from_distance(dist, voiceDist, 1.0f);

            if (fabsf(dist - lastSentDist) >= PROX_TEST_CEPOS_EPS_M) {
                ceposUpdates++;
                lastSentDist = dist;
            }
            if (timeToAudibleMs < 0.0f && vol > 0.001f) {
                timeToAudibleMs = elapsedMs;
            }
            if (vol >= peakVol) {
                peakVol = vol;
                timeToPeakMs = elapsedMs;
            }

            dist -= profile->speedMps * dt;
            elapsedMs += dt * 1000.0f;
        }

        log_write("PROX-TEST:   %-6s %4.1f m/s -> toAudible %4.0fms toPeak %4.0fms peak=%.3f cepos=%d",
            profile->label, profile->speedMps,
            timeToAudibleMs < 0.0f ? 0.0f : timeToAudibleMs,
            timeToPeakMs, peakVol, ceposUpdates);
    }
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

  {
    const float normalVoice = 15.0f;
    prox_test_flyby(normalVoice, 5.0f);
    prox_test_flyby(normalVoice, 10.0f);
    prox_test_approach(normalVoice);
  }
}
