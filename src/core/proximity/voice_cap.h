#ifndef CORE_PROXIMITY_VOICE_CAP_H
#define CORE_PROXIMITY_VOICE_CAP_H

/*
 * voice_cap — upper bound for the voice distance a REMOTE peer announces.
 *
 * Review fix 084 (#9): CEPOS carries the peer's voiceDistance (0..1000 m, only
 * range-checked on receive). A modified client could announce 1000 m and so
 * bypass the hub's MaximumWisper / MaximumNormal / MaximumShout limits. The
 * receiver now clamps the announced value to the largest distance an
 * UNMODIFIED client could ever announce under the active hub profile.
 *
 * What an unmodified client announces (src/core/voice/voice_modes.c,
 * voice_mode_get_distance): global config distance of the mode -> zone
 * override (HubZone whisper/normal/shout) -> clamp to [min, max] of the mode,
 * where the limits come from the race the sender belongs to (we do not know
 * it, so every race counts) or else from the global hub limits. The max clamp
 * runs LAST, so with a max > 0 per mode nothing legit exceeds it. A mode
 * without a max (0 = no limit) is bounded only by the sender's own settings,
 * so no cap can be derived and the value must stay untouched.
 *
 * To never cut a legitimate value (also from older plugin versions that may
 * have exempted zone overrides from the max) the zone distances raise the cap
 * too.
 *
 * Header-only, pure, any thread (incl. the audio thread — no state).
 */

#include "core/hub/hub_parser.h"

/* Fold one (whisper, normal, shout) max triple into *cap. Returns 0 when one of
   the three has no limit (0) — then no cap exists at all. */
static inline int voice_cap_fold_limits(float* cap, float maxWhisper, float maxNormal,
    float maxShout) {
    if (!(maxWhisper > 0.0f) || !(maxNormal > 0.0f) || !(maxShout > 0.0f)) {
        return 0;
    }
    if (maxWhisper > *cap) *cap = maxWhisper;
    if (maxNormal > *cap) *cap = maxNormal;
    if (maxShout > *cap) *cap = maxShout;
    return 1;
}

/* Largest voice distance (meters) a legit peer can announce under `hub`, or
   0.0f when the profile defines no (complete) limits / no profile — "no cap". */
static inline float voice_distance_cap(const HubSettings* hub) {
    if (!hub || !hub->valid) {
        return 0.0f;
    }
    float cap = 0.0f;
    if (!voice_cap_fold_limits(&cap, hub->maxWhisper, hub->maxNormal, hub->maxShout)) {
        return 0.0f;
    }
    int races = hub->raceCount;
    if (races > HUB_MAX_RACES) races = HUB_MAX_RACES;
    for (int i = 0; i < races; i++) {
        const HubRace* r = &hub->races[i];
        if (!voice_cap_fold_limits(&cap, r->maxWhisper, r->maxNormal, r->maxShout)) {
            return 0.0f;
        }
    }
    int zones = hub->zoneCount;
    if (zones > HUB_MAX_ZONES) zones = HUB_MAX_ZONES;
    for (int i = 0; i < zones; i++) {
        const HubZone* z = &hub->zones[i];
        if (z->whisperDist > cap) cap = z->whisperDist;
        if (z->normalDist > cap) cap = z->normalDist;
        if (z->shoutDist > cap) cap = z->shoutDist;
    }
    return cap;
}

/* Apply the cap: cap <= 0 means "none" (value unchanged). */
static inline float voice_distance_clamp(float voiceDistance, float cap) {
    if (cap > 0.0f && voiceDistance > cap) {
        return cap;
    }
    return voiceDistance;
}

#endif /* CORE_PROXIMITY_VOICE_CAP_H */
