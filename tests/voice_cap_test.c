/* Property test for src/core/proximity/voice_cap.h (review fix 084 #9).

   Claim: voice_distance_cap(hub) is never lower than a voice distance an
   UNMODIFIED client can announce. The test drives the REAL decision code of the
   sender, voice_modes.c (voice_mode_get_distance: config distance -> zone
   override -> min/max clamp, race limits instead of the global ones), with many
   random hub profiles, zones, races and user settings, and checks that
     announced <= cap          (so voice_distance_clamp never cuts a legit value)
   whenever the profile defines a cap at all. It also checks that a cap exists
   exactly when every mode has a max, and that a modified client's 1000 m is cut.

   Build/run on Linux: bash tests/run_tests.sh (plain gcc + the Win32 shim;
   voice_modes.c and zone_resolve.c are linked for real, config/pos/log are
   stubbed here like in voice_modes_test.c). Exit code 0 = all checks passed. */

#include "core/voice/voice_modes.h"
#include "core/proximity/voice_cap.h"
#include "core/config/config.h"
#include "core/mod_file/pos_file.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- stubs for voice_modes' core dependencies ------------------------------ */

PluginConfig g_config;
void config_copy(PluginConfig* out) { *out = g_config; }
void log_write(const char* fmt, ...) { (void)fmt; }

static PosSample g_testPos;
static int g_testPosValid = 0;
int pos_get_current(PosSample* out) {
    if (!g_testPosValid) return 0;
    *out = g_testPos;
    return 1;
}

static VoiceModeProfile g_fake;
static int g_fakeActive = 0;
static int fake_get_profile(VoiceModeProfile* out) {
    if (!g_fakeActive) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = g_fake;
    return 1;
}

static unsigned int g_rng = 20240601u;
static unsigned int rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
/* 0 with probability ~1/4, else uniform in (0, maxV]. */
static float rnd_dist(float maxV) {
    if (rnd() % 4 == 0) return 0.0f;
    return 1.0f + (float)(rnd() % (unsigned)(maxV * 10.0f)) / 10.0f;
}

static void random_profile(int allLimited) {
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.active = 1;
    HubSettings* h = &g_fake.hub;
    h->valid = 1;
    h->minWhisper = rnd_dist(60.0f); h->minNormal = rnd_dist(60.0f); h->minShout = rnd_dist(60.0f);
    h->maxWhisper = rnd_dist(300.0f); h->maxNormal = rnd_dist(300.0f); h->maxShout = rnd_dist(300.0f);
    if (allLimited) {
        if (h->maxWhisper <= 0.0f) h->maxWhisper = 1.0f + (float)(rnd() % 200);
        if (h->maxNormal <= 0.0f) h->maxNormal = 1.0f + (float)(rnd() % 200);
        if (h->maxShout <= 0.0f) h->maxShout = 1.0f + (float)(rnd() % 200);
    }
    h->zoneCount = (int)(rnd() % 4);
    for (int i = 0; i < h->zoneCount; i++) {
        HubZone* z = &h->zones[i];
        z->x1 = -50; z->z1 = -50; z->x2 = 50; z->z2 = -50;
        z->x3 = 50;  z->z3 = 50;  z->x4 = -50; z->z4 = 50;   /* local player stands inside */
        z->whisperDist = rnd_dist(500.0f);
        z->normalDist = rnd_dist(500.0f);
        z->shoutDist = rnd_dist(500.0f);
    }
    h->raceCount = (int)(rnd() % 3);
    for (int i = 0; i < h->raceCount; i++) {
        HubRace* r = &h->races[i];
        /* parser default: inherit the global limits, then maybe override */
        r->minWhisper = h->minWhisper; r->maxWhisper = h->maxWhisper;
        r->minNormal = h->minNormal;   r->maxNormal = h->maxNormal;
        r->minShout = h->minShout;     r->maxShout = h->maxShout;
        if (rnd() % 2) r->maxWhisper = rnd_dist(400.0f);
        if (rnd() % 2) r->maxNormal = rnd_dist(400.0f);
        if (rnd() % 2) r->maxShout = rnd_dist(400.0f);
        if (rnd() % 2) r->minNormal = rnd_dist(80.0f);
        if (allLimited) {
            if (r->maxWhisper <= 0.0f) r->maxWhisper = 1.0f + (float)(rnd() % 300);
            if (r->maxNormal <= 0.0f) r->maxNormal = 1.0f + (float)(rnd() % 300);
            if (r->maxShout <= 0.0f) r->maxShout = 1.0f + (float)(rnd() % 300);
        }
    }
    /* The sender belongs to one of the races - or to none. */
    g_fake.hasRace = (h->raceCount > 0 && rnd() % 2) ? 1 : 0;
    if (g_fake.hasRace) {
        g_fake.race = h->races[rnd() % (unsigned)h->raceCount];
    }
}

static void test_property(void) {
    printf("[1] property: no legit announced distance is above the cap (real voice_modes.c)\n");
    VoiceModeHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.get_profile = fake_get_profile;
    voice_mode_set_hooks(&hooks);
    g_fakeActive = 1;
    g_testPosValid = 1;
    g_testPos.x = 0.0; g_testPos.y = 0.0; g_testPos.z = 0.0;

    long cases = 0, capped = 0, uncapped = 0, violations = 0, cutByClamp = 0;
    for (int iter = 0; iter < 30000; iter++) {
        random_profile(iter % 3 != 0);
        g_testPosValid = (rnd() % 4) != 0;   /* sometimes no position (no zone override) */
        memset(&g_config, 0, sizeof(g_config));
        g_config.distanceWhisper = (float)(rnd() % 1001);
        g_config.distanceNormal = (float)(rnd() % 1001);
        g_config.distanceShout = (float)(rnd() % 1001);

        const float cap = voice_distance_cap(&g_fake.hub);
        int allMaxSet = g_fake.hub.maxWhisper > 0.0f && g_fake.hub.maxNormal > 0.0f
            && g_fake.hub.maxShout > 0.0f;
        for (int i = 0; i < g_fake.hub.raceCount; i++) {
            const HubRace* r = &g_fake.hub.races[i];
            if (!(r->maxWhisper > 0.0f && r->maxNormal > 0.0f && r->maxShout > 0.0f)) allMaxSet = 0;
        }
        if ((cap > 0.0f) != allMaxSet) violations++;   /* a cap exists exactly when every mode is limited */

        for (int m = 0; m < 3; m++) {
            const float announced = voice_mode_get_distance((VoiceMode)m);
            cases++;
            if (cap > 0.0f) {
                capped++;
                if (announced > cap) violations++;
                if (voice_distance_clamp(announced, cap) != announced) cutByClamp++;
            }
            else {
                uncapped++;
                if (voice_distance_clamp(announced, cap) != announced) cutByClamp++;
            }
        }
    }
    printf("       (info) %ld announced values: %ld under a cap, %ld without cap\n",
        cases, capped, uncapped);
    CHECK(violations == 0, "cap exists exactly when all modes are limited; announced <= cap always");
    CHECK(cutByClamp == 0, "the clamp never cuts a value an unmodified client announces");
    CHECK(capped > 10000 && uncapped > 1000, "the random profiles covered both cases");
}

static void test_modified_client(void) {
    printf("[2] a modified client's 1000 m is cut to the hub limit\n");
    random_profile(1);
    g_fake.hub.zoneCount = 0;
    g_fake.hub.raceCount = 0;
    g_fake.hasRace = 0;
    g_fake.hub.maxWhisper = 10.0f; g_fake.hub.maxNormal = 30.0f; g_fake.hub.maxShout = 100.0f;
    const float cap = voice_distance_cap(&g_fake.hub);
    CHECK(cap == 100.0f, "cap = largest hub max");
    CHECK(voice_distance_clamp(1000.0f, cap) == 100.0f, "announced 1000 m -> 100 m");
}

int main(void) {
    test_property();
    test_modified_client();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
