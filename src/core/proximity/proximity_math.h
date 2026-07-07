#ifndef CORE_PROXIMITY_PROXIMITY_MATH_H
#define CORE_PROXIMITY_PROXIMITY_MATH_H

/*
 * Proximity math — pure functions, no state, no locks, no API calls.
 * Callable from any thread (including the audio thread).
 *
 * The volume curve is identical to the old plugin's
 * proximity_calculate_volume_with_hub so the audible behavior is unchanged.
 * Hub/zone scaling arrives in later phases via the maxVolume parameter.
 */

/* 3D distance in meters. */
float prox_distance(float x1, float y1, float z1, float x2, float y2, float z2);

/* Distance -> gain curve (0..maxVolume).
   voiceDistanceMeters = nominal range of the speaker's voice mode.
   maxVolume = upper gain bound (1.0 until hub settings exist). */
float prox_volume_from_distance(float distanceMeters, float voiceDistanceMeters,
    float maxVolume);

/* Equal-power stereo pan from listener orientation.
   localDir{X,Z} = listener look direction (horizontal plane),
   to remote = remote minus listener position.
   Outputs left/right gains in 0..1 (both 0.707 when centered). */
void prox_stereo_pan(float localDirX, float localDirZ,
    float toRemoteX, float toRemoteZ,
    float* outLeft, float* outRight);

/* Phase 10.3: low-pass cutoff vs distance (same curve as the old plugin,
   ~8 kHz close up down to 900 Hz far away). Pure, any thread. */
float prox_lowpass_cutoff_hz(float distanceMeters);

/* Direct/reverb mix vs distance (AudioMinDistance reference). Pure, any thread. */
float prox_direct_reverb_ratio(float distanceMeters, float referenceDistanceMeters);

/* Log a fixed-value table through log_write (Phase 5 test aid). */
void prox_math_self_test(void);

/* Legacy UI / proximity_volume.c — hub+zone volume context (same curve as prox_volume_from_distance). */
typedef struct ProximityVolumeContext {
    double hubAudioMinDistance;
    double hubAudioMaxVolume;
    int currentZoneIndex;
    double zoneAudioMinDistance;
    double zoneAudioMaxVolume;
    int currentPlayerRaceIndex;
    float currentListenAddDistance;
} ProximityVolumeContext;

float proximity_calculate_volume_with_hub(float distanceMeters, float voiceDistanceMeters,
    const ProximityVolumeContext* ctx);

#endif /* CORE_PROXIMITY_PROXIMITY_MATH_H */
