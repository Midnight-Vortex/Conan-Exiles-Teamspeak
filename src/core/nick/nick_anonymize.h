#ifndef CORE_NICK_NICK_ANONYMIZE_H
#define CORE_NICK_NICK_ANONYMIZE_H

/*
 * Phase 12 — ingame nickname anonymization.
 *
 * While ingame the own client shows a random 8-10 digit number instead of
 * the real name; back in the hub the real name is restored. The rename
 * happens BEFORE the move to ingame so other ingame clients never see the
 * "realName -> digits" rename event.
 *
 * Thread contract: everything here runs on the TS callback thread ONLY
 * (all functions call the TS API via the adapter). No locks needed.
 *
 * Layering (V8.6): this is a core/ header, so it must NOT pull the ts/ layer.
 * It needs only the SDK integer type uint64; the TS adapter is included by
 * nick_anonymize.c directly (implementation detail, not part of this contract).
 */

#include "teamspeak/public_definitions.h"

/* 12.1 fill out with digitCount random digits (first digit 1-9). Pure,
   any thread. digitCount is clamped to 8..10. */
void nick_make_random(char* out, int outSize, int digitCount);

/* 1 when the nickname is a plausible anonymized name (8-10 digits). Pure. */
int nick_looks_anonymized(const char* nick);

/* Capture the connect-time nickname as the hub-restore fallback (only when
   it does not already look anonymized). Call on CONNECTION_ESTABLISHED /
   tab switch, after the adapter knows the connection. Resets all state
   first. TS callback thread ONLY. */
void nick_on_connected(void);

/* 12.2 rename to a random number BEFORE moving into the ingame channel.
   Saves the current name for the hub restore and checks the target channel
   for collisions. Safe to call repeatedly (no-op when already anonymized). */
void nick_anonymize_before_ingame(uint64 ingameChannelID);

/* 12.3 restore the saved name (arrival in hub / auto-move back).
   No-op when nothing was anonymized. */
void nick_restore_in_hub(void);

/* Drop saved state without touching the TS API (disconnect / shutdown). */
void nick_reset(void);

#endif /* CORE_NICK_NICK_ANONYMIZE_H */
