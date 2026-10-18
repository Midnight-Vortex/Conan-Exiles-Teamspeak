#ifndef CORE_PROXIMITY_HRTF_H
#define CORE_PROXIMITY_HRTF_H

/*
 * hrtf — generic structural HRTF (binaural rendering for headphones).
 *
 * Model: Brown & Duda, "A Structural Model for Binaural Sound Synthesis"
 * (IEEE Trans. Speech and Audio Processing, 1998), rewritten from the paper:
 *   - head shadow: one-pole/one-zero filter per ear, depends on the angle
 *     between the source and that ear's axis,
 *   - interaural time difference: Woodworth delay per ear,
 *   - pinna: five short echoes whose delays depend on lateral angle and
 *     elevation (front/back + up/down cues).
 * No Doppler, no pitch shift, no room model.
 *
 * Thread contract: pure functions + caller-owned state. One HrtfState belongs
 * to exactly one thread (the TS audio thread in the plugin). No globals, no
 * locks, no allocation — safe on the real-time audio path.
 */

#define HRTF_PINNA_ECHOES     5
#define HRTF_PINNA_HISTORY    32   /* power of two; max pinna delay ~20 samples @48k */
#define HRTF_ITD_HISTORY      64   /* power of two; max ITD ~32 samples @48k */
#define HRTF_MAX_CHUNK_FRAMES 256  /* stack buffer size of the interleaved helper */

typedef struct HrtfEar {
    float x1, y1;                       /* head-shadow filter memory */
    float pinnaHist[HRTF_PINNA_HISTORY];
    float itdHist[HRTF_ITD_HISTORY];
} HrtfEar;

typedef struct HrtfState {
    HrtfEar left;
    HrtfEar right;
    unsigned int pos;       /* shared write index (both histories, both ears) */
    float lastAzimuth;      /* direction at the end of the previous block */
    float lastElevation;
    float energyGain;       /* loudness match of the interleaved helper */
    int initialized;        /* 0 = first block jumps straight to the target */
} HrtfState;

/* Clear all filter memory and histories (new speaker / after silence). */
void hrtf_reset(HrtfState* state);

/* Listener-relative direction of a source, from Unreal conventions.
   yawDeg/pitchDeg: the listener's Unreal yaw (turning right is positive) and
   pitch (looking up is positive; 0..360 is accepted). dx/dy/dz: source minus
   listener in PLUGIN coordinates (x = UE X, y = height = UE Z, z = UE Y).
   Outputs radians: azimuth 0 = front, +pi/2 = right, +-pi = behind;
   elevation +pi/2 = above. Returns 0 (outputs 0) when the source sits on the
   listener. */
int hrtf_listener_angles(float yawDeg, float pitchDeg,
    float dx, float dy, float dz,
    float* azimuthRad, float* elevationRad);

/* Render a mono block to two ears. Direction moves smoothly from the previous
   block's direction to (azimuthRad, elevationRad) across this block.
   outLeft/outRight receive frameCount samples each. */
void hrtf_render(HrtfState* state, const float* mono, int frameCount,
    float sampleRate, float azimuthRad, float elevationRad,
    float* outLeft, float* outRight);

/* Plugin helper for interleaved 16-bit buffers: downmix the channel pair
   (leftIndex, rightIndex) to mono, render it binaurally and write it back to
   that pair. The block's loudness (sum of L^2 + R^2) is kept, so the distance
   gain applied earlier is preserved. Other channels are not touched. */
void hrtf_process_interleaved_s16(HrtfState* state, short* samples,
    int frameCount, int channels, int leftIndex, int rightIndex,
    float sampleRate, float azimuthRad, float elevationRad);

#endif
