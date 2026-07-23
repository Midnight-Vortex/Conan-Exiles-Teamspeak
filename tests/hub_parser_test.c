/* Standalone test for hub_parse_settings — compiles hub_parser.c directly.
   Build/run on Linux: bash tests/run_tests.sh (plain gcc, no TS SDK needed).
   Exit code 0 = all checks passed. */

#include "core/util/compat_crt.h"

#include "core/hub/hub_parser.h"
#include "core/proximity/zone_resolve.h"
#include "core/proximity/proximity_math.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

void log_write(const char* fmt, ...) {
    (void)fmt;
}

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* The user's real Root channel description, TS-style with <br> breaks. */
static const char* real_description =
    "[GLOBAL]<br>"
    "ForceAutomaticChanelSwitching=True<br>"
    "ForcePositionelleAudio=True<br>"
    "AudioMinDistance=0<br>"
    "AudioMaxDistance=50<br>"
    "AudioMaxVolume=130<br>"
    "IngameChannelPassword=5V88FWWME615<br>"
    "ForceDistanceBasedMuting=True<br>"
    "MinimumWisper=2<br>"
    "MaximumWisper=2<br>"
    "MinimumNormal=15<br>"
    "MaximumNormal=15<br>"
    "MinimumShout=40<br>"
    "MaximumShout=40<br>"
    "[DEFAULT_SETTINGS]<br>"
    "EnableDefaultSettingsOnFirstConnection=true<br>"
    "DefaultWhisperKey=97<br>"
    "DefaultNormalKey=98<br>"
    "DefaultShoutKey=99<br>"
    "DefaultVoiceToggleKey=6<br>"
    "DefaultDistanceWhisper=5<br>"
    "DefaultDistanceNormal=15<br>"
    "DefaultDistanceShout=40<br>"
    "[RACE]<br>"
    "Race=Wolfwesen<br>"
    "SteamID=(cain)76561198063092371,<br>"
    "MinimumWhisper=2<br>"
    "MaximumWhisper=2<br>"
    "MinimumNormal=15<br>"
    "MaximumNormal=15<br>"
    "MinimumShout=40<br>"
    "MaximumShout=40<br>"
    "listenAddDistance=5<br>"
    "Race=Faun<br>"
    "SteamID=(nyla),<br>"
    "MinimumWhisper=2<br>"
    "MaximumWhisper=2<br>"
    "MinimumNormal=15<br>"
    "MaximumNormal=15<br>"
    "MinimumShout=40<br>"
    "MaximumShout=40<br>"
    "listenAddDistance=10<br>"
    "[ZONES]<br>"
    "Zone=testzone<br>"
    "SoundProof=True<br>"
    "Reverb=True<br>"
    "X1=152460.296875<br>"
    "Z1=-187758.921875<br>"
    "X2=152456.65625<br>"
    "Z2=-187037.203125<br>"
    "X3=153178.4375<br>"
    "Z3=-187033.296875<br>"
    "X4=153182.1875<br>"
    "Z4=-187755.171875<br>"
    "GroundY=2098.171875<br>"
    "TopY=2613.649902<br>"
    "AudioMinDistance=0.5<br>"
    "AudioMaxDistance=50.0<br>"
    "AudioMaxVolume=130.0<br>"
    "Wisper=2<br>"
    "Normal=15<br>"
    "Shout=40";

static void test_real_description(void) {
    printf("[1] real Root description\n");
    HubSettings s;
    CHECK(hub_parse_settings(real_description, &s) == 1, "parse returns valid");
    CHECK(s.valid == 1, "[GLOBAL] found");
    CHECK(s.forceDistanceMuting == 1, "ForceDistanceBasedMuting");
    CHECK(s.forceAutoChannelSwitch == 1, "ForceAutomaticChanelSwitching");
    CHECK(strcmp(s.ingameChannelPassword, "5V88FWWME615") == 0, "ingame password");
    CHECK(s.audioMaxVolume == 1.3f, "AudioMaxVolume=130 -> gain 1.3");
    CHECK(s.minWhisper == 2.0f && s.maxWhisper == 2.0f, "whisper limits 2..2");
    CHECK(s.minNormal == 15.0f && s.maxNormal == 15.0f, "normal limits 15..15");
    CHECK(s.minShout == 40.0f && s.maxShout == 40.0f, "shout limits 40..40");

    CHECK(s.defaults.enabled == 1, "[DEFAULT_SETTINGS] enabled");
    CHECK(s.defaults.whisperKey == 97, "default whisper key 97");
    CHECK(s.defaults.normalKey == 98, "default normal key 98");
    CHECK(s.defaults.shoutKey == 99, "default shout key 99");
    CHECK(s.defaults.voiceToggleKey == 6, "default voice toggle key 6");
    CHECK(s.defaults.distanceWhisper == 5.0f, "default whisper dist 5");
    CHECK(s.defaults.distanceNormal == 15.0f, "default normal dist 15");
    CHECK(s.defaults.distanceShout == 40.0f, "default shout dist 40");

    CHECK(s.raceCount == 2, "2 races parsed");
    CHECK(strcmp(s.races[0].name, "Wolfwesen") == 0, "race[0] name");
    CHECK(s.races[0].steamIDCount == 1, "race[0] 1 steam id");
    CHECK(s.races[0].steamIDs[0] == 76561198063092371ULL, "race[0] steam id value");
    CHECK(s.races[0].listenAddDistance == 5.0f, "race[0] listen bonus 5");
    CHECK(s.races[0].minWhisper == 2.0f && s.races[0].maxShout == 40.0f, "race[0] limits");
    CHECK(strcmp(s.races[1].name, "Faun") == 0, "race[1] name");
    CHECK(s.races[1].steamIDCount == 0, "race[1] has no valid id ((nyla) without digits)");
    CHECK(s.races[1].listenAddDistance == 10.0f, "race[1] listen bonus 10");

    CHECK(s.zoneCount == 1, "1 zone parsed");
    CHECK(strcmp(s.zones[0].name, "testzone") == 0, "zone name");
    CHECK(s.zones[0].soundproof == 1 && s.zones[0].reverb == 1, "zone flags");
    CHECK(s.zones[0].whisperDist == 2.0f && s.zones[0].normalDist == 15.0f
        && s.zones[0].shoutDist == 40.0f, "zone distances");
    CHECK(s.zones[0].groundY == 2098.171875f, "zone groundY");
    CHECK(s.zones[0].audioMaxVolume == 1.3f, "zone AudioMaxVolume=130 -> 1.3");
    CHECK(s.zones[0].audioMinDistance == 0.5f, "zone AudioMinDistance");
}

static void test_zone_audio_rules(void) {
    printf("[1b] zone soundproof / reverb rules\n");
    HubSettings s;
    hub_parse_settings(real_description, &s);

    CHECK(zone_soundproof_muted(&s, -1, 0) == 1, "soundproof: outside cannot hear inside");
    CHECK(zone_soundproof_muted(&s, 0, -1) == 0, "soundproof: inside can hear outside");
    CHECK(zone_soundproof_muted(&s, 0, 0) == 0, "soundproof: same zone");
    CHECK(zone_reverb_active(&s, -1, 0) == 1, "reverb: remote zone active");
    CHECK(zone_reverb_active(&s, 0, -1) == 1, "reverb: local zone active");
    CHECK(zone_reverb_active(&s, -1, -1) == 0, "reverb: open world off");

    CHECK(prox_direct_reverb_ratio(10.0f, 0.5f) < prox_direct_reverb_ratio(2.0f, 0.5f),
        "DRR falls off with distance");
}

static void test_malformed(void) {
    printf("[2] malformed / hostile input\n");
    HubSettings s;

    CHECK(hub_parse_settings(NULL, &s) == 0, "NULL description");
    CHECK(hub_parse_settings("", &s) == 0, "empty description");
    CHECK(hub_parse_settings("random text no sections", &s) == 0, "no sections");

    /* Broken SteamID list must not loop or crash. */
    CHECK(hub_parse_settings(
        "[GLOBAL]<br>MinimumWisper=1<br>[RACE]<br>Race=X<br>"
        "SteamID=(unclosed,(a)abc,,,()(),765abc,(b)123,<br>", &s) == 1,
        "broken steam id list parses");
    CHECK(s.raceCount == 1, "1 race despite broken ids");
    /* "765abc" -> 765, "(b)123" -> 123 are the only valid numeric ids. */
    CHECK(s.races[0].steamIDCount == 2, "2 salvageable ids");

    /* Race without [RACE] section must be ignored. */
    CHECK(hub_parse_settings("[GLOBAL]<br>Race=Ghost<br>SteamID=(x)111<br>", &s) == 1,
        "race outside [RACE] parses");
    CHECK(s.raceCount == 0, "race outside [RACE] ignored");

    /* More races than the cap must clamp, not overflow. */
    char big[8192] = "[GLOBAL]<br>[RACE]<br>";
    for (int i = 0; i < 30; i++) {
        char one[64];
        snprintf(one, sizeof(one), "Race=R%d<br>SteamID=(p)%d<br>", i, 1000 + i);
        strcat_s(big, sizeof(big), one);
    }
    CHECK(hub_parse_settings(big, &s) == 1, "30 races parse");
    CHECK(s.raceCount == HUB_MAX_RACES, "race count clamped to HUB_MAX_RACES");
}

static void test_newline_variant(void) {
    printf("[3] plain newline separators\n");
    HubSettings s;
    CHECK(hub_parse_settings(
        "[GLOBAL]\nMinimumWisper=3\n[DEFAULT_SETTINGS]\nDefaultWhisperKey=65\n"
        "[RACE]\nRace=Elf\nSteamID=(a)42\nlistenAddDistance=7\n", &s) == 1,
        "newline variant parses");
    CHECK(s.minWhisper == 3.0f, "global value");
    CHECK(s.defaults.whisperKey == 65, "default key");
    CHECK(s.raceCount == 1 && s.races[0].steamIDs[0] == 42ULL
        && s.races[0].listenAddDistance == 7.0f, "race values");
}

static void test_global_defaults_and_clamping(void) {
    printf("[4] global defaults and clamping\n");
    HubSettings s;

    CHECK(hub_parse_settings("[GLOBAL]\n", &s) == 1, "minimal [GLOBAL] valid");
    CHECK(s.audioMaxVolume == 1.0f, "default audioMaxVolume 1.0");
    CHECK(s.audioMinDistance == 1.0f, "default audioMinDistance 1.0");
    CHECK(s.filterIntensity == 100.0f, "default filterIntensity 100");
    CHECK(s.realisticAudio == 0, "realisticAudio off by default");

    CHECK(hub_parse_settings(
        "[GLOBAL]\n"
        "  AudioMinDistance=0  \n"
        "RealisticAudio=TRUE\n"
        "FilterIntensity=150\n"
        "hubAudioFilterIntensity=5\n"
        "MaximumShout=5000\n", &s) == 1,
        "whitespace + alias keys parse");
    CHECK(s.audioMinDistance == 0.1f, "AudioMinDistance=0 clamped to 0.1");
    CHECK(s.realisticAudio == 1, "RealisticAudio case-insensitive");
    CHECK(s.filterIntensity == 5.0f, "hubAudioFilterIntensity alias wins last");
    CHECK(s.maxShout == 1000.0f, "distance clamped to HUB_DIST_MAX (1000)");

    CHECK(hub_parse_settings("[GLOBAL]\nAudioMaxVolume=250\n", &s) == 1,
        "percent volume parses");
    CHECK(s.audioMaxVolume == 2.0f, "AudioMaxVolume=250 -> gain 2.0 (cap)");

    CHECK(hub_parse_settings("[GLOBAL]\nAudioMaxVolume=2.5\n", &s) == 1,
        "direct gain parses");
    CHECK(s.audioMaxVolume == 2.0f, "direct gain 2.5 clamped to 2.0");

    /* Boundary: raw > 3 uses percent semantics; raw <= 3 is direct gain. */
    CHECK(hub_parse_settings("[GLOBAL]\nAudioMaxVolume=3.0\n", &s) == 1,
        "AudioMaxVolume=3.0 parses");
    CHECK(s.audioMaxVolume == 2.0f, "3.0 treated as direct gain, capped to 2.0");
    CHECK(hub_parse_settings("[GLOBAL]\nAudioMaxVolume=3.1\n", &s) == 1,
        "AudioMaxVolume=3.1 parses");
    CHECK(s.audioMaxVolume == 0.031f, "3.1 uses percent semantics (3.1/100)");
}

static void test_defaults_and_race_inheritance(void) {
    printf("[5] [DEFAULT_SETTINGS] keys and race inheritance\n");
    HubSettings s;

    CHECK(hub_parse_settings(
        "[GLOBAL]\nMinimumNormal=10\nMaximumNormal=20\n"
        "[DEFAULT_SETTINGS]\nDefaultWhisperKey=0\nDefaultVoiceToggleKey=300\n"
        "[RACE]\nRace=Orc\nSteamID=(x)99\n", &s) == 1,
        "defaults + race parse");
    CHECK(s.defaults.whisperKey == 0, "invalid key 0 -> 0 (absent)");
    CHECK(s.defaults.voiceToggleKey == 0, "key >= 256 -> 0 (absent)");
    CHECK(s.raceCount == 1, "one race");
    CHECK(s.races[0].minNormal == 10.0f && s.races[0].maxNormal == 20.0f,
        "race inherits global limits before overrides");
    CHECK(s.races[0].steamIDs[0] == 99ULL, "race steam id");
}

static void test_multiple_zones(void) {
    printf("[6] multiple zones and headers\n");
    HubSettings s;

    CHECK(hub_parse_settings(
        "[GLOBAL]\n"
        "[ZONES]\n"
        "[ZoneName=Alpha]\nX1=1\nZ1=1\nX2=2\nZ2=1\nX3=2\nZ3=2\nX4=1\nZ4=2\n"
        "SoundProof=true\nWisper=3\n"
        "[Zone=Beta]\n"
        "X1=10\nZ1=10\nX2=11\nZ2=10\nX3=11\nZ3=11\nX4=10\nZ4=11\n"
        "Reverb=1\n", &s) == 1,
        "two zones with bracket headers");
    CHECK(s.zoneCount == 2, "two zones parsed");
    CHECK(strcmp(s.zones[0].name, "Alpha") == 0, "zone[0] [ZoneName=] header");
    CHECK(s.zones[0].soundproof == 1 && s.zones[0].whisperDist == 3.0f,
        "zone[0] flags and distance");
    CHECK(strcmp(s.zones[1].name, "Beta") == 0, "zone[1] [Zone=] header");

    CHECK(hub_parse_settings(
        "[GLOBAL]\n[ZONES]\nZoneName=Plain\nX1=0\n", &s) == 1,
        "unbracketed ZoneName= header");
    CHECK(s.zoneCount == 1 && strcmp(s.zones[0].name, "Plain") == 0,
        "plain ZoneName= parsed");

    /* More zones than HUB_MAX_ZONES must clamp, not overflow. */
    char big[16384] = "[GLOBAL]\n[ZONES]\n";
    for (int i = 0; i < HUB_MAX_ZONES + 4; i++) {
        char one[96];
        snprintf(one, sizeof(one), "[ZoneName=Z%d]\nX1=%d\n", i, i + 1);
        strcat_s(big, sizeof(big), one);
    }
    CHECK(hub_parse_settings(big, &s) == 1, "overflow zone list parses");
    CHECK(s.zoneCount == HUB_MAX_ZONES, "zone count clamped to HUB_MAX_ZONES");
}

static void test_partial_and_unknown_sections(void) {
    printf("[7] partial descriptions and unknown sections\n");
    HubSettings s;

    CHECK(hub_parse_settings("[GLOBAL]\nMinimumWisper=4\n[UNKNOWN]\nFoo=Bar\n", &s) == 1,
        "[GLOBAL] + unknown section still valid");
    CHECK(s.minWhisper == 4.0f, "global key before unknown section kept");

    CHECK(hub_parse_settings(
        "[GLOBAL]\nMinimumWisper=1\n[ZONES]\n[ZoneName=Later]\n"
        "[RACE]\nRace=Late\n", &s) == 1,
        "section switch clears zone context");
    CHECK(s.zoneCount == 1, "zone parsed before [RACE]");
    CHECK(s.raceCount == 1 && strcmp(s.races[0].name, "Late") == 0,
        "race parsed after leaving [ZONES]");
    CHECK(strcmp(s.zones[0].name, "Later") == 0, "zone name preserved");
}

static void test_nonfinite_values(void) {
    printf("[8] non-finite numeric values\n");
    HubSettings s;

    CHECK(hub_parse_settings(
        "[GLOBAL]\nMinimumWisper=nan\nMaximumNormal=inf\nAudioMinDistance=-5\n", &s) == 1,
        "non-finite globals parse");
    CHECK(s.minWhisper == 0.0f, "NaN whisper -> clamp min 0");
    CHECK(s.maxNormal == 0.0f, "inf normal -> clamp min 0");
    CHECK(s.audioMinDistance == 0.1f, "negative AudioMinDistance -> clamp 0.1");
}

int main(void) {
    test_real_description();
    test_zone_audio_rules();
    test_malformed();
    test_newline_variant();
    test_global_defaults_and_clamping();
    test_defaults_and_race_inheritance();
    test_multiple_zones();
    test_partial_and_unknown_sections();
    test_nonfinite_values();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
