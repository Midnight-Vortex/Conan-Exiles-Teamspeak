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

/* ---- 103 / 104: listener bonus ("weiter hoeren", race key listenAddDistance) --
 *
 * The bonus is a fixed distance the LISTENER adds to a speaker's range. The
 * loudness curve is scaled by the range (prox_volume_from_distance), so the
 * same bonus did very different things to a short and to a long range: with
 * whisper 2.56 m, normal 15.36 m and a bonus of 10.24 m a whisper carried
 * 12.8 m (x5) and was almost as loud as normal speech at whisper distance,
 * while normal only grew x1.7 (player report 2026-11-13).
 *
 * 103 (plugin 8.0.24) shortened the RANGE of a whisper for such listeners.
 * That broke the admin's sum "whisper 1 + bonus 4 = 5 foundations", and the
 * report was about loudness, not range. 104 (8.0.25): the range is the plain
 * sum again; only the LOUDNESS of the bonus part is reduced for short ranges
 * (prox_volume_listen_bonus in proximity_math.c): a listener with the bonus
 * hears a whisperer as loud as everybody else nearby, and quietly on top of
 * that out to the full range. The share below decides how quiet.
 *
 * The reference is the SHORTEST range normal speech can have on this server, so
 * that normal speech (and shout) is never affected: an unmodified client
 * announces at least the MinimumNormal of its limits (global or its race, the
 * min clamp in voice_mode_get_distance runs after the zone override). We do not
 * know the speaker's race, so the smallest MinimumNormal counts. A server
 * without any MinimumNormal lets players pick any normal range; then the
 * smallest MaximumNormal is the best reference left (a normal range chosen
 * below it is treated like a short range - there is nothing to tell it from a
 * whisper).
 *
 * Header-only, pure, any thread. */

/* Smallest value > 0 of `a` and `b` (0 = none of them is set). */
static inline float voice_min_positive(float a, float b) {
    if (!(a > 0.0f)) {
        return (b > 0.0f) ? b : 0.0f;
    }
    return (b > 0.0f && b < a) ? b : a;
}

/* The range the listener bonus is measured against (see above), 0 = unknown:
   then the bonus loudness is never reduced (old behaviour). */
static inline float voice_listen_reference(const HubSettings* hub) {
    if (!hub || !hub->valid) {
        return 0.0f;
    }
    float minRef = (hub->minNormal > 0.0f) ? hub->minNormal : 0.0f;
    float maxRef = (hub->maxNormal > 0.0f) ? hub->maxNormal : 0.0f;
    int races = hub->raceCount;
    if (races > HUB_MAX_RACES) races = HUB_MAX_RACES;
    for (int i = 0; i < races; i++) {
        minRef = voice_min_positive(minRef, hub->races[i].minNormal);
        maxRef = voice_min_positive(maxRef, hub->races[i].maxNormal);
    }
    return (minRef > 0.0f) ? minRef : maxRef;
}

/* How much of the bonus LOUDNESS one speaker gets, 0..1: 1 when his announced
   range reaches the reference (normal speech, shout) or no reference is known,
   announced / reference below it (whisper 1 of 6 foundations -> 1/6). */
static inline float voice_listen_share(float announcedRange, float normalRange) {
    if (!(normalRange > 0.0f) || !(announcedRange < normalRange)) {
        return 1.0f;
    }
    if (!(announcedRange > 0.0f)) {
        return 0.0f;
    }
    return announcedRange / normalRange;
}

#endif /* CORE_PROXIMITY_VOICE_CAP_H */
