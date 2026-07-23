#ifndef CORE_HUB_HUB_PARSER_H
#define CORE_HUB_HUB_PARSER_H

/*
 * Phase 9.2 — parse server settings from a channel description.
 *
 * Format (written by the server admin into the config channel description,
 * compatible with the old plugin's Root description):
 *
 *   [GLOBAL]
 *   AudioMaxVolume=1.0
 *   MinimumWisper=2 / MaximumWisper=10
 *   MinimumNormal=5 / MaximumNormal=30
 *   MinimumShout=20 / MaximumShout=100
 *   ForceDistanceBasedMuting=True
 *   ForceAutomaticChanelSwitching=True
 *   RealisticAudio=True
 *   FilterIntensity=100
 *   NicknameRandomizer=True
 *   IngameChannelPassword=secret
 *   [ZONES]
 *   [ZoneName=Cave1]            (also "Zone=" / "ZoneName=" without brackets)
 *   X1=.. Z1=.. .. X4=.. Z4=..  (quad corners, world meters)
 *   GroundY=.. TopY=..
 *   SoundProof=True / Reverb=True
 *   Wisper=.. Normal=.. Shout=..  (zone distance overrides)
 *
 * HTML line breaks (<br>, <br/>) are treated as newlines. Values are
 * validated (finite, clamped) before they land in the struct.
 *
 * Pure parse function — no state, no locks, no API. Any thread.
 */

#define HUB_MAX_ZONES     16
#define HUB_ZONE_NAME_LEN 64
#define HUB_PASSWORD_LEN  128
#define HUB_MAX_RACES     16
#define HUB_RACE_NAME_LEN 64
#define HUB_MAX_STEAMIDS_PER_RACE 32

typedef struct HubZone {
    char name[HUB_ZONE_NAME_LEN];
    float x1, z1, x2, z2, x3, z3, x4, z4; /* quad corners, world meters */
    float groundY, topY;                  /* 0/0 = unbounded */
    float whisperDist, normalDist, shoutDist; /* 0 = no override */
    float audioMinDistance;               /* DRR reference, 0 = use global */
    float audioMaxVolume;                 /* gain override, 0 = use global */
    int soundproof;
    int reverb;
} HubZone;

/* [RACE] entry: per-race voice limits + listener bonus, members by SteamID64.
   Format: "Race=Name" starts an entry, "SteamID=(name)7656..,(name2)7656.."
   lists members, Minimum.../Maximum... keys override the global limits. */
typedef struct HubRace {
    char name[HUB_RACE_NAME_LEN];
    unsigned long long steamIDs[HUB_MAX_STEAMIDS_PER_RACE];
    int steamIDCount;
    float minWhisper, maxWhisper;
    float minNormal, maxNormal;
    float minShout, maxShout;
    float listenAddDistance; /* meters added when LISTENING to others */
} HubRace;

/* [DEFAULT_SETTINGS]: one-time defaults applied on the first connection to
   a server. 0 = key/distance absent (leave the user's value alone). */
typedef struct HubDefaults {
    int enabled;             /* EnableDefaultSettingsOnFirstConnection */
    int whisperKey, normalKey, shoutKey, voiceToggleKey;
    float distanceWhisper, distanceNormal, distanceShout;
} HubDefaults;

typedef struct HubSettings {
    int valid;              /* 1 when a [GLOBAL] section was found */

    float audioMaxVolume;    /* gain cap 0..2 (server "130" = 1.3); 1.0 when absent */
    float audioMinDistance;  /* DRR reference distance, meters; 1.0 default */
    float minWhisper, maxWhisper;
    float minNormal, maxNormal;
    float minShout, maxShout;

    int forceDistanceMuting;     /* server forces proximity muting on */
    int forceAutoChannelSwitch;  /* server forces hub<->ingame auto-move */
    int nicknameRandomizer;      /* 1 = assign random digit nick ingame (default) */
    int realisticAudio;          /* 1 = Mumble-style spatial filter in open world */
    float filterIntensity;       /* 0..100 scales filter when realisticAudio=1 */
    char ingameChannelPassword[HUB_PASSWORD_LEN];

    int zoneCount;
    HubZone zones[HUB_MAX_ZONES];

    int raceCount;
    HubRace races[HUB_MAX_RACES];

    HubDefaults defaults;
} HubSettings;

/* Fill out from description text. Returns 1 when a [GLOBAL] section was
   found (out->valid set), 0 otherwise (out reset to defaults). */
int hub_parse_settings(const char* description, HubSettings* out);

#endif /* CORE_HUB_HUB_PARSER_H */
