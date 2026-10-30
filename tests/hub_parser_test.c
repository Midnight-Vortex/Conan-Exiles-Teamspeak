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
    CHECK(s.nicknameRandomizer == 1, "nicknameRandomizer on by default");

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

    CHECK(hub_parse_settings("[GLOBAL]\nNicknameRandomizer=False\n", &s) == 1,
        "NicknameRandomizer=False parses");
    CHECK(s.nicknameRandomizer == 0, "NicknameRandomizer disabled");

    CHECK(hub_parse_settings("[GLOBAL]\nNicknameRandomizer=True\n", &s) == 1,
        "NicknameRandomizer=True parses");
    CHECK(s.nicknameRandomizer == 1, "NicknameRandomizer enabled");
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

static void test_hrtf_switch(void) {
    printf("[9] EnableHrtf root-channel switch (062/063)\n");
    HubSettings s;

    hub_parse_settings("[GLOBAL]\nRealisticAudio=True\n", &s);
    CHECK(s.enableHrtf == 0, "absent EnableHrtf -> off");
    hub_parse_settings("[GLOBAL]\nEnableHrtf=True\n", &s);
    CHECK(s.enableHrtf == 1, "EnableHrtf=True -> on");
    hub_parse_settings("[GLOBAL]<br>enablehrtf = 1<br>", &s);
    CHECK(s.enableHrtf == 1, "case-insensitive, '1', <br> line breaks");
    hub_parse_settings("[GLOBAL]\nEnableHrtf=False\n", &s);
    CHECK(s.enableHrtf == 0, "EnableHrtf=False -> off");

    CHECK(s.forceSpeakersOn == 0, "absent ForceSpeakersOn -> off (079)");
    hub_parse_settings("[GLOBAL]<br>forcespeakerson = 1<br>", &s);
    CHECK(s.forceSpeakersOn == 1, "ForceSpeakersOn=1 -> on");

    hub_parse_settings("[GLOBAL]\nEnableHrtf=True\n", &s);
    CHECK(s.enableOcclusion == 0, "absent EnableOcclusion -> off (064)");
    hub_parse_settings("[GLOBAL]\nEnableOcclusion=True\nEnableHrtf=False\n", &s);
    CHECK(s.enableOcclusion == 1 && s.enableHrtf == 0, "EnableOcclusion=True independent of EnableHrtf");

    CHECK(s.occlusionIntensity == 100.0f, "absent OcclusionIntensity -> 100 (069)");
    hub_parse_settings("[GLOBAL]\nEnableOcclusion=True\nOcclusionIntensity=200\n", &s);
    CHECK(s.enableOcclusion == 1 && s.occlusionIntensity == 200.0f, "OcclusionIntensity=200 parsed");
    hub_parse_settings("[GLOBAL]<br>occlusionintensity = 999<br>", &s);
    CHECK(s.occlusionIntensity == 300.0f && s.enableOcclusion == 0, "OcclusionIntensity clamped to 300, switch untouched");
    hub_parse_settings("[GLOBAL]\nOcclusionIntensity=-20\n", &s);
    CHECK(s.occlusionIntensity == 0.0f, "negative OcclusionIntensity -> 0");

    /* 072 */
    CHECK(s.occAttackMs == 500.0f && s.occReleaseMs == 250.0f && s.occDiffraction == 100.0f && s.occReverb == 100.0f
        && s.occModParams[0] == 80 && s.occModParams[11] == 2, "072 defaults");
    hub_parse_settings("[GLOBAL]\nOcclusionAttackMs=800\nOcclusionReleaseMs=99999\nOcclusionDiffraction=150\n"
        "OcclusionReverb=0\nOccDetourNearPct=45\nOccDetourNear=200\noccfar = 600\nOccBudget=5\n", &s);
    CHECK(s.occAttackMs == 800.0f && s.occReleaseMs == 5000.0f && s.occDiffraction == 150.0f && s.occReverb == 0.0f,
        "072 sound keys parsed + clamped");
    CHECK(s.occModParams[5] == 45 && s.occModParams[3] == 200, "OccDetourNearPct and OccDetourNear kept apart (exact keys)");
    CHECK(s.occModParams[2] == 600 && s.occModParams[10] == 20, "case-insensitive mod key, OccBudget clamped to 20");
    CHECK(s.occlusionIntensity == 100.0f && s.enableOcclusion == 0, "new keys leave the other settings alone");
}

static void test_decimal_comma(void) {
    printf("[10] decimal comma in single-number values (German admins)\n");
    HubSettings s;

    CHECK(hub_parse_settings(
        "[GLOBAL]\nMaximumNormal=13,5\nMinimumShout=20,25\nAudioMaxVolume=1,3\n"
        "AudioMinDistance=0,5\nOcclusionIntensity=150,5\n", &s) == 1, "comma values parse");
    CHECK(s.maxNormal == 13.5f, "MaximumNormal=13,5 -> 13.5 (was 13)");
    CHECK(s.minShout == 20.25f, "MinimumShout=20,25 -> 20.25");
    CHECK(fabsf(s.audioMaxVolume - 1.3f) < 1e-6f, "AudioMaxVolume=1,3 -> gain 1.3");
    CHECK(s.audioMinDistance == 0.5f, "AudioMinDistance=0,5 -> 0.5");
    CHECK(s.occlusionIntensity == 150.5f, "OcclusionIntensity=150,5 -> 150.5");

    /* Dot values unchanged; ambiguous / odd shapes keep the old strtod result. */
    CHECK(hub_parse_settings("[GLOBAL]\nMaximumNormal=13.5\nMaximumShout=1,000.5\n"
        "MaximumWisper=7,2,1\nMinimumNormal=,5\nMinimumWisper=9,\n", &s) == 1, "odd shapes parse");
    CHECK(s.maxNormal == 13.5f, "13.5 unchanged");
    CHECK(s.maxShout == 1.0f, "1,000.5 (comma AND dot) untouched -> old result 1");
    CHECK(s.maxWhisper == 7.0f, "7,2,1 (two commas) untouched -> old result 7");
    CHECK(s.minNormal == 0.0f, ",5 (no digit before) untouched -> 0");
    CHECK(s.minWhisper == 9.0f, "9, (trailing comma) -> 9");

    /* Typos keep today's fallback (0 -> clamp). */
    CHECK(hub_parse_settings("[GLOBAL]\nMaximumNormal=abc\nAudioMinDistance=x,y\n", &s) == 1, "typos parse");
    CHECK(s.maxNormal == 0.0f && s.audioMinDistance == 0.1f, "unparsable text keeps the old fallback");

    /* Zone coordinates and race values are single numbers too. */
    CHECK(hub_parse_settings(
        "[GLOBAL]\n[ZONES]\nZone=z\nX1=152460,296875\nZ1=-187758,5\nNormal=12,5\n"
        "[RACE]\nRace=Faun\nMaximumNormal=15,5\nlistenAddDistance=2,5\n", &s) == 1, "zone/race parse");
    CHECK(s.zoneCount == 1 && fabsf(s.zones[0].x1 - 152460.296875f) < 0.01f && s.zones[0].z1 == -187758.5f,
        "zone coordinates with comma");
    CHECK(s.zones[0].normalDist == 12.5f, "zone Normal=12,5");
    CHECK(s.raceCount == 1 && s.races[0].maxNormal == 15.5f && s.races[0].listenAddDistance == 2.5f,
        "race values with comma");

    /* The comma separated SteamID list must stay a list. */
    CHECK(hub_parse_settings(
        "[GLOBAL]\n[RACE]\nRace=Wolf\nSteamID=(a)76561198063092371,(b)76561198063092372, 76561198063092373\n", &s) == 1,
        "steamid list parses");
    CHECK(s.raceCount == 1 && s.races[0].steamIDCount == 3
        && s.races[0].steamIDs[0] == 76561198063092371ULL
        && s.races[0].steamIDs[1] == 76561198063092372ULL
        && s.races[0].steamIDs[2] == 76561198063092373ULL, "SteamID list unaffected by the comma rule");

    /* Occlusion mod parameter (integer after clamp): "45,5" -> 45.5 -> 46 */
    hub_parse_settings("[GLOBAL]\nOccDetourNearPct=45,5\n", &s);
    CHECK(s.occModParams[5] == 46, "OccDetourNearPct=45,5 -> 46 (rounded)");
}

/* ---- 091: ForceRangesInFoundations ------------------------------------------ */

static int fnear(float a, float b) {
    return fabsf(a - b) < 0.0005f;
}

/* The ranges of this Root description are written in FOUNDATIONS (the user's
   example block).  [GLOBAL] uses the old "...Wisper" spelling here; since 091
   [GLOBAL], [RACE] and [ZONES] accept both spellings (see test_whisper_spelling). */
#define FOUNDATION_RANGES_BODY \
    "MinimumWisper=1\nMaximumWisper=1\n" \
    "MinimumNormal=6\nMaximumNormal=6\n" \
    "MinimumShout=16\nMaximumShout=16\n"

/* [GLOBAL] keys that are NOT voice ranges. */
static const char* foundation_global_rest =
    "AudioMaxVolume=130\nAudioMinDistance=2\nFilterIntensity=55\n"
    "OcclusionIntensity=200\nOcclusionAttackMs=800\nOccFar=600\n"
    "IngameChannelPassword=5V88FWWME615\n";

/* The sections that follow [GLOBAL]. */
static const char* foundation_sections =
    "[DEFAULT_SETTINGS]\n"
    "EnableDefaultSettingsOnFirstConnection=true\n"
    "DefaultWhisperKey=97\nDefaultNormalKey=98\nDefaultShoutKey=99\nDefaultVoiceToggleKey=6\n"
    "DefaultDistanceWhisper=1\nDefaultDistanceNormal=6\nDefaultDistanceShout=16\n"
    "[RACE]\n"
    "Race=Wolf\nSteamID=(cain)76561198063092371\n"
    "MinimumWhisper=1\nMaximumWhisper=1\nMinimumNormal=6\nMaximumNormal=6\n"
    "MinimumShout=16\nMaximumShout=16\nlistenAddDistance=4\n"
    "Race=Faun\nSteamID=76561198063092372\n"
    "[ZONES]\n"
    "Zone=cave\nX1=152460.5\nZ1=-187758.5\nX2=152456.5\nZ2=-187037.5\n"
    "X3=153178.5\nZ3=-187033.5\nX4=153182.5\nZ4=-187755.5\n"
    "GroundY=2098.5\nTopY=2613.5\nAudioMinDistance=0.5\nAudioMaxVolume=130\n"
    "Wisper=1\nNormal=6\nShout=16\n"
    "Zone=plain\nX1=1\nZ1=1\nX2=2\nZ2=1\nX3=2\nZ3=2\nX4=1\nZ4=2\n";

static void test_force_ranges_in_foundations(void) {
    printf("[11] ForceRangesInFoundations (091): Root ranges written in foundations\n");
    HubSettings s;
    char text[4096];

    /* the key itself: absent / True / False / 1 / case */
    hub_parse_settings("[GLOBAL]\nAudioMaxVolume=1\n", &s);
    CHECK(s.forceRangesInFoundations == 0, "absent key -> off");
    hub_parse_settings("[GLOBAL]\nForceRangesInFoundations=True\n", &s);
    CHECK(s.forceRangesInFoundations == 1, "ForceRangesInFoundations=True -> on");
    hub_parse_settings("[GLOBAL]\nForceRangesInFoundations=False\n", &s);
    CHECK(s.forceRangesInFoundations == 0, "ForceRangesInFoundations=False -> off");
    hub_parse_settings("[GLOBAL]<br>forcerangesinfoundations = 1<br>", &s);
    CHECK(s.forceRangesInFoundations == 1, "case-insensitive, '1', <br> line breaks");
    hub_parse_settings("[GLOBAL]\nForceSpeakersOn=True\n", &s);
    CHECK(s.forceRangesInFoundations == 0 && s.forceSpeakersOn == 1, "independent of the other Force* keys");

    /* key FIRST, values in foundations -> meters (the user's example block) */
    snprintf(text, sizeof(text), "[GLOBAL]\nForceRangesInFoundations=True\n%s%s%s",
        FOUNDATION_RANGES_BODY, foundation_global_rest, foundation_sections);
    CHECK(hub_parse_settings(text, &s) == 1 && s.forceRangesInFoundations == 1, "example block parses, flag set");
    CHECK(fnear(s.minWhisper, 2.56f) && fnear(s.maxWhisper, 2.56f), "global whisper 1 / 1 F -> 2.56 / 2.56 m");
    CHECK(fnear(s.minNormal, 15.36f) && fnear(s.maxNormal, 15.36f), "global normal 6 / 6 F -> 15.36 / 15.36 m");
    CHECK(fnear(s.minShout, 40.96f) && fnear(s.maxShout, 40.96f), "global shout 16 / 16 F -> 40.96 / 40.96 m");

    CHECK(s.raceCount == 2, "two races");
    CHECK(fnear(s.races[0].minWhisper, 2.56f) && fnear(s.races[0].maxWhisper, 2.56f), "race whisper limits converted");
    CHECK(fnear(s.races[0].minNormal, 15.36f) && fnear(s.races[0].maxNormal, 15.36f), "race normal limits converted");
    CHECK(fnear(s.races[0].minShout, 40.96f) && fnear(s.races[0].maxShout, 40.96f), "race shout limits converted");
    CHECK(fnear(s.races[0].listenAddDistance, 10.24f), "race listenAddDistance 4 F -> 10.24 m");
    CHECK(fnear(s.races[1].minWhisper, 2.56f) && fnear(s.races[1].maxNormal, 15.36f) && fnear(s.races[1].maxShout, 40.96f),
        "a race that inherits the global limits ends up in meters too (converted once, not twice)");
    CHECK(s.races[1].listenAddDistance == 0.0f, "race without listen bonus stays 0");

    CHECK(s.zoneCount == 2, "two zones");
    CHECK(fnear(s.zones[0].whisperDist, 2.56f) && fnear(s.zones[0].normalDist, 15.36f)
        && fnear(s.zones[0].shoutDist, 40.96f), "zone voice overrides Wisper/Normal/Shout converted");
    CHECK(s.zones[1].whisperDist == 0.0f && s.zones[1].normalDist == 0.0f && s.zones[1].shoutDist == 0.0f,
        "zone without overrides: 0 (= no override) stays 0");

    CHECK(s.defaults.enabled == 1, "[DEFAULT_SETTINGS] enabled flag untouched");
    CHECK(fnear(s.defaults.distanceWhisper, 2.56f) && fnear(s.defaults.distanceNormal, 15.36f)
        && fnear(s.defaults.distanceShout, 40.96f), "default distances 1 / 6 / 16 F -> meters");
    CHECK(s.defaults.whisperKey == 97 && s.defaults.normalKey == 98 && s.defaults.shoutKey == 99
        && s.defaults.voiceToggleKey == 6, "default KEYS are not distances: untouched");

    /* everything that is not a voice range keeps its value */
    CHECK(fnear(s.zones[0].x1, 152460.5f) && fnear(s.zones[0].z1, -187758.5f)
        && fnear(s.zones[0].x4, 153182.5f) && fnear(s.zones[0].z4, -187755.5f), "zone corners untouched");
    CHECK(fnear(s.zones[0].groundY, 2098.5f) && fnear(s.zones[0].topY, 2613.5f), "GroundY / TopY untouched");
    CHECK(fnear(s.zones[0].audioMinDistance, 0.5f) && fnear(s.zones[0].audioMaxVolume, 1.3f),
        "zone AudioMinDistance / AudioMaxVolume untouched");
    CHECK(fnear(s.audioMaxVolume, 1.3f) && fnear(s.audioMinDistance, 2.0f), "global AudioMaxVolume / AudioMinDistance untouched");
    CHECK(fnear(s.filterIntensity, 55.0f), "FilterIntensity untouched");
    CHECK(fnear(s.occlusionIntensity, 200.0f) && fnear(s.occAttackMs, 800.0f) && s.occModParams[2] == 600,
        "Occlusion* / Occ* untouched");
    CHECK(strcmp(s.ingameChannelPassword, "5V88FWWME615") == 0, "password untouched");
    CHECK(s.races[0].steamIDCount == 1 && s.races[0].steamIDs[0] == 76561198063092371ULL
        && s.races[1].steamIDs[0] == 76561198063092372ULL, "SteamIDs untouched");

    /* the key may stand AFTER the values it changes (and after other keys) */
    snprintf(text, sizeof(text), "[GLOBAL]\n%s%sForceRangesInFoundations=True\n%s",
        FOUNDATION_RANGES_BODY, foundation_global_rest, foundation_sections);
    CHECK(hub_parse_settings(text, &s) == 1 && s.forceRangesInFoundations == 1, "key after the values: flag set");
    CHECK(fnear(s.minWhisper, 2.56f) && fnear(s.maxNormal, 15.36f) && fnear(s.maxShout, 40.96f),
        "key after the values: globals still converted");
    CHECK(fnear(s.races[0].listenAddDistance, 10.24f) && fnear(s.zones[0].normalDist, 15.36f)
        && fnear(s.defaults.distanceShout, 40.96f), "key after the values: races, zones, defaults converted");

    /* without the key (or =False) the same numbers are meters, exactly as before */
    snprintf(text, sizeof(text), "[GLOBAL]\n%s%s%s", FOUNDATION_RANGES_BODY, foundation_global_rest, foundation_sections);
    CHECK(hub_parse_settings(text, &s) == 1 && s.forceRangesInFoundations == 0, "no key: flag off");
    CHECK(s.minWhisper == 1.0f && s.maxNormal == 6.0f && s.maxShout == 16.0f, "no key: global values are meters");
    CHECK(s.races[0].listenAddDistance == 4.0f && s.races[0].maxNormal == 6.0f, "no key: race values are meters");
    CHECK(s.zones[0].whisperDist == 1.0f && s.zones[0].normalDist == 6.0f && s.zones[0].shoutDist == 16.0f,
        "no key: zone overrides are meters");
    CHECK(s.defaults.distanceWhisper == 1.0f && s.defaults.distanceShout == 16.0f, "no key: default distances are meters");
    snprintf(text, sizeof(text), "[GLOBAL]\nForceRangesInFoundations=False\n%s%s%s",
        FOUNDATION_RANGES_BODY, foundation_global_rest, foundation_sections);
    hub_parse_settings(text, &s);
    CHECK(s.minNormal == 6.0f && s.races[0].listenAddDistance == 4.0f && s.zones[0].shoutDist == 16.0f,
        "=False: everything stays in meters");

    /* ceiling and unset values */
    CHECK(hub_parse_settings("[GLOBAL]\nForceRangesInFoundations=True\nMaximumNormal=1000\nMaximumShout=500\n"
        "MinimumNormal=390\nMinimumShout=0\n", &s) == 1, "huge foundation values parse");
    CHECK(s.maxNormal == 1000.0f && s.maxShout == 1000.0f, "1000 F / 500 F are cut at the existing 1000 m ceiling");
    CHECK(fnear(s.minNormal, 998.4f), "390 F = 998.4 m is below the ceiling and kept");
    CHECK(s.minShout == 0.0f && s.minWhisper == 0.0f, "0 / absent (= not set) stays 0");

    /* decimal comma still works in front of the conversion: 6,5 F = 16.64 m */
    hub_parse_settings("[GLOBAL]\nMaximumNormal=6,5\nForceRangesInFoundations=True\n", &s);
    CHECK(fnear(s.maxNormal, 16.64f), "decimal comma: 6,5 F -> 16.64 m");

    /* sections without [GLOBAL] never set the flag, nothing is converted */
    CHECK(hub_parse_settings("[ZONES]\nZone=z\nNormal=6\nForceRangesInFoundations=True\n", &s) == 0
        && s.forceRangesInFoundations == 0, "no [GLOBAL]: invalid profile, flag off");
}

/* 091: every section accepts "Whisper" as well as the old "Wisper" ([RACE]
   already did).  Before 091, [GLOBAL] silently ignored MinimumWhisper, and
   [DEFAULT_SETTINGS] / [ZONES] knew only one spelling each. */
static void test_whisper_spelling(void) {
    printf("[12] Whisper / Wisper spelling (091)\n");
    HubSettings s;

    CHECK(hub_parse_settings("[GLOBAL]\nMinimumWhisper=2\nMaximumWhisper=3\n", &s) == 1
        && s.minWhisper == 2.0f && s.maxWhisper == 3.0f, "[GLOBAL] MinimumWhisper / MaximumWhisper read");
    CHECK(hub_parse_settings("[GLOBAL]\nMinimumWisper=2\nMaximumWisper=3\n", &s) == 1
        && s.minWhisper == 2.0f && s.maxWhisper == 3.0f, "[GLOBAL] old MinimumWisper / MaximumWisper still read");
    CHECK(hub_parse_settings("[GLOBAL]<br>minimumwhisper = 1.5<br>", &s) == 1
        && s.minWhisper == 1.5f, "[GLOBAL] new spelling: case-insensitive, <br>");
    CHECK(hub_parse_settings("[GLOBAL]\nMinimumWhisper=2\nMinimumNormal=15\nMaximumShout=40\n", &s) == 1
        && s.minWhisper == 2.0f && s.minNormal == 15.0f && s.maxShout == 40.0f,
        "[GLOBAL] new spelling does not swallow the other keys");

    CHECK(hub_parse_settings("[GLOBAL]\n[ZONES]\nZone=a\nWhisper=3\nZone=b\nWisper=4\n", &s) == 1
        && s.zoneCount == 2 && s.zones[0].whisperDist == 3.0f && s.zones[1].whisperDist == 4.0f,
        "[ZONES] Whisper= and old Wisper= both read");

    CHECK(hub_parse_settings("[GLOBAL]\n[DEFAULT_SETTINGS]\nDefaultWisperKey=97\nDefaultDistanceWisper=5\n", &s) == 1
        && s.defaults.whisperKey == 97 && s.defaults.distanceWhisper == 5.0f,
        "[DEFAULT_SETTINGS] DefaultWisperKey / DefaultDistanceWisper read");
    CHECK(hub_parse_settings("[GLOBAL]\n[DEFAULT_SETTINGS]\nDefaultWhisperKey=98\nDefaultDistanceWhisper=6\n", &s) == 1
        && s.defaults.whisperKey == 98 && s.defaults.distanceWhisper == 6.0f,
        "[DEFAULT_SETTINGS] DefaultWhisperKey / DefaultDistanceWhisper still read");
    CHECK(hub_parse_settings("[GLOBAL]\n[RACE]\nRace=R\nMinimumWisper=1\nMaximumWhisper=2\n", &s) == 1
        && s.raceCount == 1 && s.races[0].minWhisper == 1.0f && s.races[0].maxWhisper == 2.0f,
        "[RACE] both spellings (mixed) read");
    /* both spellings for the same value in one section: the later line wins */
    CHECK(hub_parse_settings("[GLOBAL]\nMinimumWisper=2\nMinimumWhisper=3\n", &s) == 1
        && s.minWhisper == 3.0f, "same value twice (both spellings): last line wins");

    /* together with ForceRangesInFoundations: the new spelling is converted too */
    CHECK(hub_parse_settings("[GLOBAL]\nForceRangesInFoundations=True\nMinimumWhisper=1\nMaximumWhisper=1\n"
        "[ZONES]\nZone=a\nWhisper=1\n", &s) == 1
        && fnear(s.minWhisper, 2.56f) && fnear(s.maxWhisper, 2.56f) && fnear(s.zones[0].whisperDist, 2.56f),
        "new spelling + ForceRangesInFoundations: 1 F -> 2.56 m");
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
    test_hrtf_switch();
    test_decimal_comma();
    test_force_ranges_in_foundations();
    test_whisper_spelling();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
