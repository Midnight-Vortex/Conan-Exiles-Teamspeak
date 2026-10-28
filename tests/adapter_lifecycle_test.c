/* Unit tests for two pure helpers of the adapter/lifecycle review fixes:
 *
 *  1. ts3_version_map.h  — the CEVER client table (linear probing). The old
 *     delete wiped a slot to EMPTY, which cut the probe chain: later entries
 *     of the chain became unreachable ("Kein Plugin erkannt") and were
 *     inserted a second time. Tombstones fix it.
 *  2. ts3_return_code.h  — the remembered plugin return codes of
 *     requestClientMove (matched in onServerErrorEvent).
 *
 * Both are header-only with NO Win32/TS coupling (ts3_version_map.h only needs
 * the SDK typedef anyID, hence -Isdk/include). The CEDRAIN addressing logic is
 * tested in tests/wakeup_policy_test.c.
 *
 * Build/run: bash tests/run_tests.sh   Exit 0 = all checks passed. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ts/info/ts3_version_map.h"
#include "ts/adapter/ts3_return_code.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

static VersionMapEntry g_map[VERSION_MAP_SLOTS];

static int count_used(const VersionMapEntry* map) {
    int n = 0;
    for (int i = 0; i < VERSION_MAP_SLOTS; i++) {
        if (map[i].state == VMAP_USED) {
            n++;
        }
    }
    return n;
}

/* Insert an id and tag it so a later find can prove it got the SAME slot. */
static void put(anyID id, unsigned char tag) {
    VersionClientSlot* slot = vmap_get(g_map, id, 1);
    if (slot) {
        slot->hasVersion = 1;
        slot->replied = tag;
    }
}

/* ids 1, 513, 1025, 1537, 2049 ... all start probing at slot 1 (id % 512). */
#define COLLIDE(n) ((anyID)(1 + 512 * (n)))

static void test_basic(void) {
    printf("[1] empty map, invalid ids, insert + find\n");
    vmap_clear(g_map);
    CHECK(vmap_find(g_map, 5) == NULL, "empty map: find -> NULL");
    CHECK(vmap_find(g_map, 0) == NULL, "id 0 is never found");
    CHECK(vmap_get(g_map, 0, 1) == NULL, "id 0 is never inserted");
    CHECK(vmap_get(g_map, 5, 0) == NULL, "get without create on a miss -> NULL");

    put(7, 70);
    VersionMapEntry* e = vmap_find(g_map, 7);
    CHECK(e != NULL && e->clientID == 7 && e->state == VMAP_USED, "inserted entry found");
    CHECK(e != NULL && e->slot.replied == 70, "slot data preserved");
    CHECK(vmap_get(g_map, 7, 0) == &e->slot, "get(no create) returns the same slot");
    CHECK(vmap_get(g_map, 7, 1) == &e->slot, "get(create) of an existing id does not duplicate");
    CHECK(count_used(g_map) == 1, "exactly one used entry");
}

static void test_delete_keeps_chain(void) {
    printf("[2] deleting the MIDDLE of a probe chain keeps the rest reachable\n");
    vmap_clear(g_map);
    for (int n = 0; n < 4; n++) {
        put(COLLIDE(n), (unsigned char)(10 + n));
    }
    CHECK(count_used(g_map) == 4, "4 colliding ids stored");

    vmap_remove(g_map, COLLIDE(1));
    CHECK(vmap_find(g_map, COLLIDE(1)) == NULL, "deleted id is gone");
    VersionMapEntry* e2 = vmap_find(g_map, COLLIDE(2));
    VersionMapEntry* e3 = vmap_find(g_map, COLLIDE(3));
    CHECK(e2 != NULL && e2->slot.replied == 12, "later entry (2) still reachable, data intact");
    CHECK(e3 != NULL && e3->slot.replied == 13, "later entry (3) still reachable, data intact");
    CHECK(vmap_find(g_map, COLLIDE(0)) != NULL, "earlier entry (0) still reachable");

    /* The old bug inserted the same client a second time here. */
    CHECK(vmap_get(g_map, COLLIDE(2), 1) == &e2->slot, "get(create) returns the existing slot");
    CHECK(count_used(g_map) == 3, "no duplicate was created");
}

static void test_reuse_tombstone(void) {
    printf("[3] a tombstone is reused by the next insert\n");
    vmap_clear(g_map);
    for (int n = 0; n < 4; n++) {
        put(COLLIDE(n), (unsigned char)(10 + n));
    }
    vmap_remove(g_map, COLLIDE(1));
    put(COLLIDE(1), 99);   /* back again */
    VersionMapEntry* e1 = vmap_find(g_map, COLLIDE(1));
    CHECK(e1 != NULL && e1->slot.replied == 99, "re-inserted id found with fresh data");
    CHECK(e1 == &g_map[2], "it reused the tombstone slot (home slot 1 + 1)");
    CHECK(count_used(g_map) == 4, "still 4 entries, nothing leaked");
    CHECK(vmap_find(g_map, COLLIDE(3)) != NULL, "tail of the chain reachable");

    /* head and tail removal */
    vmap_remove(g_map, COLLIDE(0));
    vmap_remove(g_map, COLLIDE(3));
    CHECK(vmap_find(g_map, COLLIDE(1)) != NULL && vmap_find(g_map, COLLIDE(2)) != NULL,
        "remaining middle entries reachable after head+tail removal");
    CHECK(vmap_find(g_map, COLLIDE(0)) == NULL && vmap_find(g_map, COLLIDE(3)) == NULL,
        "removed head and tail are gone");
    vmap_remove(g_map, 4000);   /* absent id: harmless */
    CHECK(count_used(g_map) == 2, "removing an absent id changes nothing");
}

/* ts3_plugin_version.c's purge loop removes entries IN PLACE while walking the
   array by index — that must not disturb the entries it has not visited. */
static void test_purge_in_place(void) {
    printf("[4] in-place removal while iterating (purge loop)\n");
    vmap_clear(g_map);
    for (int n = 0; n < 8; n++) {
        put(COLLIDE(n), (unsigned char)(20 + n));
    }
    for (int i = 0; i < VERSION_MAP_SLOTS; i++) {
        VersionMapEntry* entry = &g_map[i];
        if (entry->state == VMAP_USED && (entry->slot.replied % 2) == 0) {
            vmap_remove_entry(entry);
        }
    }
    int ok = 1;
    for (int n = 0; n < 8; n++) {
        const int expectPresent = ((20 + n) % 2) != 0;
        const int present = vmap_find(g_map, COLLIDE(n)) != NULL;
        if (present != expectPresent) {
            ok = 0;
        }
    }
    CHECK(ok, "odd-tagged entries survive, even-tagged are gone, every lookup correct");
    CHECK(count_used(g_map) == 4, "4 entries left");
}

/* Documents the bug: the OLD deletion (memset the slot to all-zero = EMPTY)
   cuts the chain. If this check ever fails the regression test above would be
   meaningless. */
static void test_old_bug_demonstration(void) {
    printf("[5] the old in-place memset really broke the chain (regression guard)\n");
    vmap_clear(g_map);
    for (int n = 0; n < 3; n++) {
        put(COLLIDE(n), (unsigned char)(30 + n));
    }
    VersionMapEntry* mid = vmap_find(g_map, COLLIDE(1));
    memset(mid, 0, sizeof(*mid));   /* old behaviour: EMPTY, not a tombstone */
    CHECK(vmap_find(g_map, COLLIDE(2)) == NULL,
        "old-style wipe: later entry becomes unreachable (the bug)");
    CHECK(vmap_find(g_map, COLLIDE(0)) != NULL, "entries before the wipe stay reachable");
}

static void test_full_table(void) {
    printf("[6] full table: insert fails cleanly, find terminates, space is reusable\n");
    vmap_clear(g_map);
    int inserted = 0;
    for (int id = 1; id <= VERSION_MAP_SLOTS; id++) {
        if (vmap_get(g_map, (anyID)id, 1)) {
            inserted++;
        }
    }
    CHECK(inserted == VERSION_MAP_SLOTS, "512 distinct ids fill the table");
    CHECK(vmap_get(g_map, 2000, 1) == NULL, "513th insert -> NULL (full)");
    CHECK(vmap_find(g_map, 2000) == NULL, "find of an absent id terminates with NULL");
    vmap_remove(g_map, 100);
    VersionClientSlot* again = vmap_get(g_map, 2000, 1);
    CHECK(again != NULL, "after one delete the insert succeeds");
    CHECK(vmap_find(g_map, 2000) != NULL, "the new id is findable");
    CHECK(vmap_find(g_map, 100) == NULL, "the deleted id is gone");
    CHECK(vmap_find(g_map, 511) != NULL && vmap_find(g_map, 512) != NULL,
        "other entries unaffected");
}

/* Differential test: random insert/remove/lookup over ids that all collide
   into a few home slots, compared against a plain presence array. */
static void test_random_vs_model(void) {
    printf("[7] randomised differential test against a reference model\n");
    static unsigned char model[65536];
    memset(model, 0, sizeof(model));
    vmap_clear(g_map);

    unsigned long long rng = 88172645463325252ull;
    int mismatches = 0;
    int live = 0;
    for (int step = 0; step < 200000; step++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        /* 24 home slots x 20 ids each = 480 candidate ids (< 512 slots). */
        const unsigned home = (unsigned)(rng % 24) + 1;
        const unsigned lap = (unsigned)((rng >> 8) % 20);
        const anyID id = (anyID)(home + 512u * lap);
        const unsigned op = (unsigned)((rng >> 16) % 3);

        if (op == 0) {
            VersionClientSlot* slot = vmap_get(g_map, id, 1);
            if (!slot) {
                mismatches++;   /* never full: <= 480 live of 512 */
            }
            else if (!model[id]) {
                model[id] = 1;
                live++;
            }
        }
        else if (op == 1) {
            vmap_remove(g_map, id);
            if (model[id]) {
                model[id] = 0;
                live--;
            }
        }
        else if ((vmap_find(g_map, id) != NULL) != (model[id] != 0)) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "200000 random operations agree with the model");
    CHECK(count_used(g_map) == live, "used-entry count equals the model's live count");
    int allMatch = 1;
    for (unsigned home = 1; home <= 24 && allMatch; home++) {
        for (unsigned lap = 0; lap < 20; lap++) {
            const anyID id = (anyID)(home + 512u * lap);
            if ((vmap_find(g_map, id) != NULL) != (model[id] != 0)) {
                allMatch = 0;
                break;
            }
        }
    }
    CHECK(allMatch, "final full sweep agrees with the model");
}

static void test_clear(void) {
    printf("[8] vmap_clear\n");
    put(5, 1);
    put(6, 2);
    vmap_clear(g_map);
    CHECK(count_used(g_map) == 0 && vmap_find(g_map, 5) == NULL, "clear empties the table");
    put(5, 3);
    CHECK(vmap_find(g_map, 5) != NULL, "usable after clear");
}

/* ---- return code tracker ---------------------------------------------------- */

static void test_rc_basic(void) {
    printf("[9] return codes: add / take / consume once\n");
    Ts3RcTracker t;
    ts3_rc_track_clear(&t);

    CHECK(ts3_rc_track_take(&t, "abc") == 0, "empty tracker: no match");
    CHECK(ts3_rc_track_add(&t, "move-1") == 1, "add accepted");
    CHECK(ts3_rc_track_add(&t, "move-2") == 1, "second add accepted");
    CHECK(ts3_rc_track_take(&t, "other") == 0, "unknown code: no match (unchanged TS handling)");
    CHECK(ts3_rc_track_take(&t, "move-2") == 1, "known code matches");
    CHECK(ts3_rc_track_take(&t, "move-2") == 0, "consumed: a code answers only once");
    CHECK(ts3_rc_track_take(&t, "move-1") == 1, "other code still pending");

    CHECK(ts3_rc_track_take(&t, NULL) == 0, "NULL code -> 0");
    CHECK(ts3_rc_track_take(&t, "") == 0, "empty code -> 0");
    CHECK(ts3_rc_track_add(&t, NULL) == 0, "add(NULL) rejected");
    CHECK(ts3_rc_track_add(&t, "") == 0, "add(empty) rejected");

    char tooLong[TS3_RC_MAX + 8];
    memset(tooLong, 'x', sizeof(tooLong) - 1);
    tooLong[sizeof(tooLong) - 1] = '\0';
    CHECK(ts3_rc_track_add(&t, tooLong) == 0, "too-long code rejected (a truncated copy could never match)");
}

static void test_rc_ring_and_clear(void) {
    printf("[10] return codes: oldest-first overwrite, clear\n");
    Ts3RcTracker t;
    ts3_rc_track_clear(&t);
    char code[32];
    for (int i = 0; i < TS3_RC_TRACK_SLOTS + 1; i++) {   /* 9 codes into 8 slots */
        snprintf(code, sizeof(code), "rc-%d", i);
        ts3_rc_track_add(&t, code);
    }
    CHECK(ts3_rc_track_take(&t, "rc-0") == 0, "the oldest code was overwritten");
    int allPresent = 1;
    for (int i = 1; i < TS3_RC_TRACK_SLOTS + 1; i++) {
        snprintf(code, sizeof(code), "rc-%d", i);
        if (!ts3_rc_track_take(&t, code)) {
            allPresent = 0;
        }
    }
    CHECK(allPresent, "the 8 newest codes are all still known");

    ts3_rc_track_add(&t, "keep-me");
    ts3_rc_track_clear(&t);
    CHECK(ts3_rc_track_take(&t, "keep-me") == 0, "clear forgets everything (connection change)");
}

int main(void) {
    printf("adapter lifecycle helper tests (version map, return codes)\n");
    test_basic();
    test_delete_keeps_chain();
    test_reuse_tombstone();
    test_purge_in_place();
    test_old_bug_demonstration();
    test_full_table();
    test_random_vs_model();
    test_clear();
    test_rc_basic();
    test_rc_ring_and_clear();
    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
