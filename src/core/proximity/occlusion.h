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

/* Parse "OCC=<cid>:<pct>;..." out of a POST body and store the values.
   Returns the number of stored entries, -1 when the body has no "OCC=" key. */
int occlusion_store_from_text(const char* body, unsigned long long nowMs);

/* Occlusion 0..1 for a client; 0 when unknown, out of range or stale. */
float occlusion_get(unsigned int clientID, unsigned long long nowMs);

/* Apply occlusion (0..1) to a computed gain and lowpass cutoff. Cutoff is
   only ever lowered. No-op for occ <= 0. */
void occlusion_apply(float occ, float* gain, float* cutoffHz);

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

#endif /* CORE_PROXIMITY_OCCLUSION_H */
