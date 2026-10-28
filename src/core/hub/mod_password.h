#pragma once

/*
 * mod_password.h — ingame channel password delivered by the game mod (077).
 *
 * The Conan server keeps the password (admin command "dc tsvm <password>")
 * and replicates it to logged-in players only. Their mod appends
 * " TSPW=<password>" as the LAST field of every POST /v1/position.
 * The HTTP thread cuts that field off before anything parses or logs the
 * body and stores the value here. The password lives in memory only: it is
 * never logged and never written to disk. A value counts only while the mod
 * keeps posting (MOD_PASSWORD_TTL_MS); the first read after that wipes it.
 *
 * Thread contract: take/store = HTTP listener thread; get/clear = any thread
 * (internal lock).
 */

#include "core/hub/hub_parser.h"

#define MOD_PASSWORD_MAX    HUB_PASSWORD_LEN /* incl. the terminating NUL */
#define MOD_PASSWORD_TTL_MS 10000ULL

/* Cuts " TSPW=<value>" (or "TSPW=<value>" at the very start) off a mutable,
   NUL-terminated body in place, stores the value and wipes it from the body.
   Returns the new body length (unchanged when the field is absent). */
int mod_password_take_from_body(char* body, int bodyLen, unsigned long long nowMs);

/* Stores a value (len bytes, no NUL needed). Empty = the server has no
   password (clears). Too long or control characters = ignored. Logs only
   when the value changes, never the value itself. */
void mod_password_store(const char* value, int len, unsigned long long nowMs);

/* Copies the password into out when one was received within the TTL.
   Returns 1 when out holds a non-empty password, 0 otherwise (out = "").
   A read that finds the value expired (older than the TTL) also WIPES it, so
   a stale secret does not linger in memory until the next disconnect. */
int mod_password_get(char* out, int outLen, unsigned long long nowMs);

/* Forgets the stored password (TS disconnect). */
void mod_password_clear(void);
