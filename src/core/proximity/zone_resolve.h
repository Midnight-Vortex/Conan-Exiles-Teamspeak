#ifndef CORE_PROXIMITY_ZONE_RESOLVE_H
#define CORE_PROXIMITY_ZONE_RESOLVE_H

/*
 * Phase 10.1/10.2 — zone lookup, pure functions.
 *
 * Zones are 3D boxes: a horizontal 4-corner polygon plus an optional
 * GroundY..TopY height band (0/0 = unbounded). Positions from Pos.txt/CEPOS
 * are meters, but admins historically entered corners in UE centimeters or
 * meters — the resolver tries both layouts and scales like the old plugin.
 *
 * No state, no locks, no API. Any thread.
 */

#include "core/hub/hub_parser.h"

/* Zone index containing the point (world meters), or -1. */
int zone_resolve(const HubSettings* settings, float x, float y, float z);

/* 10.2 one-way soundproof (TeamSpeak build):
   - Speaker INSIDE a SoundProof zone, listener OUTSIDE → muted (outside cannot hear in).
   - Speaker OUTSIDE, listener INSIDE that zone → NOT muted (inside can hear out).
   - Both in the same zone → normal proximity.
   localZone = listener, remoteZone = speaker. Returns 1 = hard mute. */
int zone_soundproof_muted(const HubSettings* settings, int localZone, int remoteZone);

/* 1 when listener or speaker is inside a Reverb=true zone. */
int zone_reverb_active(const HubSettings* settings, int localZone, int remoteZone);

#endif /* CORE_PROXIMITY_ZONE_RESOLVE_H */
