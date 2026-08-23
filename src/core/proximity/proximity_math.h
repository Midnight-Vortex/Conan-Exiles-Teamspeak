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

/* Enter/exit hysteresis for the audible-range cull decision (pure, no state).
   A speaker becomes an audible candidate at enterDistanceMeters and is only
   culled again once past exitDistanceMeters; inside [enter, exit] the previous
   decision (prevInRange, 0/1) is held. This stops a speaker hovering at the
   boundary from flipping compute/neutral (and mute/unmute) every recompute.
   exitDistanceMeters is expected >= enterDistanceMeters. Returns 0 or 1. */
int prox_hysteresis_in_range(float distanceMeters, float enterDistanceMeters,
    float exitDistanceMeters, int prevInRange);

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

/* Phase 6: horizontal front/back dot (+1 = ahead, -1 = behind). Pure. */
float prox_front_back_dot(float localDirX, float localDirZ,
    float toRemoteX, float toRemoteZ);

/* 3D front/back dot using full look vector (+1 ahead, -1 behind). Pure. */
float prox_front_back_dot3d(float localDirX, float localDirY, float localDirZ,
    float toRemoteX, float toRemoteY, float toRemoteZ);

/* Listener look direction from Conan yaw / yawY (degrees). Pure. */
void prox_listener_forward(float yawDeg, float yawYDeg,
    float* outDirX, float* outDirY, float* outDirZ);

/* Mumble "TRUE stereo" — asymmetric L/R gains with rear attenuation (not HRIR).
   toRemote* = remote minus listener offset in meters. Outputs 0.15..1.0. */
void prox_binaural_stereo_gains(float localDirX, float localDirY, float localDirZ,
    float toRemoteX, float toRemoteY, float toRemoteZ,
    float* outLeft, float* outRight);

/* Phase 6: rear attenuation multipliers from frontBack dot (Mumble filter path). */
typedef struct ProxRearPsycho {
    float directionVolume; /* 1.0 .. 0.88 */
    float cutoffMul;       /* 1.0 or 0.75 when behind */
    float drrMul;          /* 1.0 or 0.85 when behind */
} ProxRearPsycho;

void prox_rear_psychoacoustics(float frontBack, ProxRearPsycho* out);

/* Phase 6: diffuse/DRR mix on PCM (in-place, audio thread safe). */
void prox_apply_diffuse_samples(short* samples, int sampleCount, int channelCount,
    float drr);

/* Log a fixed-value table through log_write (Phase 5 test aid). */
void prox_math_self_test(void);

/* Hub+zone volume context (same curve as prox_volume_from_distance); kept for
   the proximity_math unit tests. The legacy plugin-side builder was removed in
   V8.5b (doku/019-legacy-abbau.md). */
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
