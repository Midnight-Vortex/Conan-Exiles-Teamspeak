#ifndef CORE_PROXIMITY_OCCLUSION_H
#define CORE_PROXIMITY_OCCLUSION_H

/*
 * PROTOTYPE (branch test/occlusion-prototype) — wall / foundation occlusion
 * reported by the game mod. Feasibility test only, not a finished feature.
 * See doku/aenderungen/061-occlusion-machbarkeit.md.
 *
 * Round trip over the existing HTTP bridge (port 52734):
 *   1. Mod POSTs its position and appends  "OCC=<cid>:<pct>;<cid>:<pct>"
 *      (pct = 0..100, 0 = free line of sight, 100 = fully blocked).
 *      An empty "OCC=" opts in without values.
 *   2. Only for opted-in requests the plugin answers with probe points:
 *        {"ok":true,"probe":"<cid>,<ueX>,<ueY>,<ueZ>;..."}
 *      (Unreal axes, centimeters, nearest players first). The mod line-traces
 *      from its own head to every probe and reports the result in the next
 *      POST. The mod never needs to know which pawn belongs to which TS client.
 *
 * Thread contract:
 *   - occlusion_store_from_text / occlusion_format_probes: HTTP listener
 *     thread only (format uses a module-static snapshot buffer).
 *   - occlusion_get: any thread (single atomic reads, no lock).
 *   - occlusion_apply: pure function.
 *   Never calls the TS API, never takes a lock held by the audio thread.
 */

#define OCCLUSION_MAX_CLIENT_ID 4096   /* mirrors TS3_MAX_CLIENT_ID (core must not include ts/) */
#define OCCLUSION_STALE_MS      1000   /* older values count as "no occlusion" */
#define OCCLUSION_MAX_PROBES    16
#define OCCLUSION_PROBE_RANGE_M 60.0f
#define OCCLUSION_FULL_GAIN     0.25f  /* gain multiplier at 100 % (~ -12 dB) */
#define OCCLUSION_FULL_CUTOFF   900.0f /* lowpass cutoff (Hz) at 100 % */
#define OCCLUSION_MIN_CUTOFF    250.0f /* lowest cutoff, also with OcclusionIntensity > 100 */
#define OCCLUSION_INTENSITY_DEFAULT 100 /* [GLOBAL] OcclusionIntensity, percent (069) */
#define OCCLUSION_INTENSITY_MAX     300

/* Parse "OCC=<cid>:<pct>;..." out of a POST body and store the values.
   Also reads the v2 entries "<cid>:<pct>:<kind>:<room>:<x>,<y>,<z>" (072):
   kind N/T/D, room 0..100, relay point in Unreal centimeters (kind D only).
   v1 entries count as kind T (N when pct is 0) with room 0.
   Returns the number of stored entries, -1 when the body has no "OCC=" key. */
int occlusion_store_from_text(const char* body, unsigned long long nowMs);

/* Occlusion 0..1 for a client; 0 when unknown, out of range or stale. */
float occlusion_get(unsigned int clientID, unsigned long long nowMs);

/* ---- v2 (072) ------------------------------------------------------------ */
#define OCCLUSION_PROTOCOL_VERSION 2
#define OCCLUSION_KIND_NONE      0  /* free line of sight */
#define OCCLUSION_KIND_TRANSMIT  1  /* through a wall */
#define OCCLUSION_KIND_DIFFRACT  2  /* around a corner / through an opening */

typedef struct OcclusionInfo {
    float amount;      /* 0..1 */
    int kind;          /* OCCLUSION_KIND_* */
    float room;        /* 0..1 how enclosed listener/speaker are (reverb, B4) */
    int relayValid;    /* 1 for kind DIFFRACT with a relay point */
    float relayX, relayY, relayZ; /* relay point, plugin coordinates (meters, Y-up) */
} OcclusionInfo;

/* Everything stored for a client. Stale or unknown -> zeroed (kind NONE).
   Returns 1 when fresh values exist. Any thread (single atomic reads). */
int occlusion_get_info(unsigned int clientID, unsigned long long nowMs, OcclusionInfo* out);

/* Mod parameters from the root channel, forwarded to the mod in every probe
   reply as "occcfg":"2,<p1>,...,<p12>" (072 section 2). */
#define OCCLUSION_MOD_PARAM_COUNT 12
const char* occlusion_mod_param_name(int index);  /* root-channel key, NULL when out of range */
int occlusion_mod_param_default(int index);
int occlusion_mod_param_clamp(int index, double value); /* rounded + clamped to the key's range */
/* values: OCCLUSION_MOD_PARAM_COUNT entries (already clamped), NULL = defaults. */
void occlusion_set_mod_params(const int* values);
/* Insert ,"occcfg":"..." before the closing brace of a JSON object in out.
   Returns 1 when appended. HTTP listener thread. */
int occlusion_append_cfg_json(char* out, int outSize);

/* Plugin-side sound parameters from the root channel (072 section 4). */
#define OCCLUSION_ATTACK_MS_DEFAULT    500
#define OCCLUSION_RELEASE_MS_DEFAULT   250
#define OCCLUSION_SMOOTH_MS_MAX        5000
#define OCCLUSION_DIFFRACTION_DEFAULT  100
#define OCCLUSION_DIFFRACTION_MAX      300
/* Echo from the room factor the mod reports (natural caves without a zone; the mod sends
   0 for built rooms, doku 076). Needs EnableOcclusion=True. Default 100 again (096): caves
   must echo without a Reverb=True zone. 0 = off. Zones use CaveReverb instead. */
#define OCCLUSION_REVERB_DEFAULT       100
#define OCCLUSION_REVERB_MAX           200
/* Around-the-corner curve (kind D, B2): mild loss of highs, little volume loss.
   strength = OcclusionDiffraction / 100. At amount 1 / strength 1: -3 dB and
   a lowpass at 2.5 kHz (never below 1.2 kHz). Cutoff is only ever lowered. */
#define OCCLUSION_DIFFRACT_GAIN_LOSS  0.3f
#define OCCLUSION_DIFFRACT_CUTOFF     2500.0f
#define OCCLUSION_DIFFRACT_MIN_CUTOFF 1200.0f
void occlusion_apply_diffraction(float amount, float strength, float* gain, float* cutoffHz);

/* B1 smoothing per client: moves the gain factor, the cutoff (on a log scale)
   and the relay-direction weight towards their targets - attack time constant
   when getting more muffled, release when getting clearer. Keeps the last
   relay point so the direction can fade back after a D entry ended.
   First call for a client (or after 2 s without calls) jumps to the target. */
typedef struct OcclusionSmoothed {
    float gain;         /* factor 0..1 */
    float cutoffHz;
    float relayWeight;  /* 0 = speaker direction, 1 = relay point */
    float room;         /* smoothed room factor 0..1 (indoor reverb, B4) */
    int relayKnown;
    float relayX, relayY, relayZ; /* plugin coordinates */
} OcclusionSmoothed;
void occlusion_smooth(unsigned int clientID, unsigned long long nowMs, const OcclusionInfo* info,
    float targetGain, float targetCutoffHz, OcclusionSmoothed* out);

void occlusion_set_sound_params(float attackMs, float releaseMs, float diffractionPct, float reverbPct);
float occlusion_get_attack_ms(void);
float occlusion_get_release_ms(void);
float occlusion_get_diffraction(void); /* percent / 100 */
float occlusion_get_reverb(void);      /* percent / 100 */

/* Apply occlusion (0..1) to a computed gain and lowpass cutoff. Cutoff is
   only ever lowered. No-op for occ <= 0 or strength <= 0.
   strength = OcclusionIntensity / 100: 1 = the 8.0.8 curve (-12 dB, 900 Hz
   at full occlusion), 2 = twice the dB loss and the cutoff sweep continued
   (clamped at OCCLUSION_MIN_CUTOFF). */
void occlusion_apply(float occ, float strength, float* gain, float* cutoffHz);

/* Write the probe JSON response for a listener at plugin coordinates
   (meters, Y-up). Returns bytes written (0 on error / too small buffer). */
int occlusion_format_probes(char* out, int outSize,
    float localX, float localY, float localZ);

/* Forget all stored values (unit tests, disconnect). */
void occlusion_clear(void);

/* Server switch from the root channel ([GLOBAL] EnableOcclusion=True, 064).
   Set by the server profile (callback thread) on apply/reset; read from any
   thread (single atomic). Off = the HTTP bridge neither stores OCC values nor
   answers with probes (the mod then traces nothing) and the audio path ignores
   occlusion. Switching off also forgets all stored values. Default: off. */
void occlusion_set_enabled(int enabled);
int occlusion_is_enabled(void);

/* Strength from the root channel ([GLOBAL] OcclusionIntensity=100, 069),
   percent, clamped to 0..OCCLUSION_INTENSITY_MAX. Same thread rules as the
   switch. occlusion_get_strength returns percent / 100 for occlusion_apply. */
void occlusion_set_intensity(float percent);
float occlusion_get_strength(void);

#endif /* CORE_PROXIMITY_OCCLUSION_H */
