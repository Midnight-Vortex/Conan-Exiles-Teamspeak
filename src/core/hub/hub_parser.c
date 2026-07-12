#include "core/hub/hub_parser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HUB_DESC_MAX 16384
#define HUB_DIST_MAX 1000.0f

/* ---- helpers ----------------------------------------------------------- */

/* Copy description into buf with <br>/<br/> converted to '\n'. */
static void hub_normalize_html(const char* description, char* buf, size_t bufLen) {
    size_t j = 0;
    for (const char* src = description; *src && j + 1 < bufLen;) {
        if (_strnicmp(src, "<br/>", 5) == 0) {
            buf[j++] = '\n';
            src += 5;
        }
        else if (_strnicmp(src, "<br>", 4) == 0) {
            buf[j++] = '\n';
            src += 4;
        }
        else {
            buf[j++] = *src++;
        }
    }
    buf[j] = '\0';
}

static char* hub_trim(char* line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    char* end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return line;
}

/* "Key=value" -> value pointer after '=', trimmed; NULL when key mismatch. */
static const char* hub_value_for(const char* line, const char* key) {
    const size_t keyLen = strlen(key);
    if (_strnicmp(line, key, keyLen) != 0) {
        return NULL;
    }
    const char* p = strchr(line + keyLen, '=');
    if (!p) {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static int hub_parse_bool(const char* value) {
    return _strnicmp(value, "true", 4) == 0 || value[0] == '1';
}

/* Distance/volume value: finite and clamped to a sane range. */
static float hub_parse_clamped(const char* value, float minV, float maxV) {
    float f = (float)strtod(value, NULL);
    if (!isfinite(f)) {
        return minV;
    }
    if (f < minV) f = minV;
    if (f > maxV) f = maxV;
    return f;
}

static void hub_defaults(HubSettings* out) {
    memset(out, 0, sizeof(*out));
    out->audioMaxVolume = 1.0f;
    out->audioMinDistance = 1.0f;
    out->filterIntensity = 100.0f;
}

/* ---- [GLOBAL] section ---------------------------------------------------- */

/* AudioMaxVolume uses the OLD plugin's percent semantics: 130 means gain 1.3
   (the deployed Root descriptions are written that way). Values <= 3 are
   treated as a direct gain factor. Result clamped to 0..2. */
static float hub_parse_max_volume(const char* value) {
    float raw = hub_parse_clamped(value, 0.0f, 1000.0f);
    float gain = (raw > 3.0f) ? raw / 100.0f : raw;
    if (gain > 2.0f) {
        gain = 2.0f;
    }
    return gain;
}

static void hub_parse_global_line(const char* line, HubSettings* out) {
    const char* v;

    if ((v = hub_value_for(line, "AudioMaxVolume")) != NULL) {
        out->audioMaxVolume = hub_parse_max_volume(v);
    }
    else if ((v = hub_value_for(line, "AudioMinDistance")) != NULL) {
        out->audioMinDistance = hub_parse_clamped(v, 0.1f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MinimumWisper")) != NULL) {
        out->minWhisper = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumWisper")) != NULL) {
        out->maxWhisper = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MinimumNormal")) != NULL) {
        out->minNormal = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumNormal")) != NULL) {
        out->maxNormal = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MinimumShout")) != NULL) {
        out->minShout = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumShout")) != NULL) {
        out->maxShout = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "ForceDistanceBasedMuting")) != NULL) {
        out->forceDistanceMuting = hub_parse_bool(v);
    }
    else if ((v = hub_value_for(line, "ForceAutomaticChanelSwitching")) != NULL) {
        out->forceAutoChannelSwitch = hub_parse_bool(v);
    }
    else if ((v = hub_value_for(line, "RealisticAudio")) != NULL) {
        out->realisticAudio = hub_parse_bool(v);
    }
    else if ((v = hub_value_for(line, "FilterIntensity")) != NULL
        || (v = hub_value_for(line, "hubAudioFilterIntensity")) != NULL) {
        out->filterIntensity = hub_parse_clamped(v, 0.0f, 100.0f);
    }
    else if ((v = hub_value_for(line, "IngameChannelPassword")) != NULL) {
        strncpy_s(out->ingameChannelPassword, sizeof(out->ingameChannelPassword), v, _TRUNCATE);
    }
}

/* ---- [RACE] section -------------------------------------------------------- */

/* "SteamID=(name)7656..., (name2)7656..." — the "(name)" prefix is optional
   and ignored; IDs are comma-separated SteamID64 values. */
static void hub_parse_race_steamids(const char* list, HubRace* race) {
    const char* p = list;
    while (*p && race->steamIDCount < HUB_MAX_STEAMIDS_PER_RACE) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '(') {
            const char* close = strchr(p, ')');
            if (!close) {
                break; /* malformed "(name" without ')' — stop safely */
            }
            p = close + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        char* end = NULL;
        const unsigned long long id = strtoull(p, &end, 10);
        if (end == p) {
            p++; /* no digits here — skip one char, never loop forever */
            continue;
        }
        if (id > 0ULL) {
            race->steamIDs[race->steamIDCount++] = id;
        }
        p = end;
    }
}

static void hub_parse_race_line(const char* line, HubRace* race) {
    const char* v;

    if ((v = hub_value_for(line, "SteamID")) != NULL) {
        hub_parse_race_steamids(v, race);
    }
    else if ((v = hub_value_for(line, "MinimumWhisper")) != NULL
        || (v = hub_value_for(line, "MinimumWisper")) != NULL) {
        race->minWhisper = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumWhisper")) != NULL
        || (v = hub_value_for(line, "MaximumWisper")) != NULL) {
        race->maxWhisper = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MinimumNormal")) != NULL) {
        race->minNormal = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumNormal")) != NULL) {
        race->maxNormal = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MinimumShout")) != NULL) {
        race->minShout = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "MaximumShout")) != NULL) {
        race->maxShout = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "listenAddDistance")) != NULL) {
        race->listenAddDistance = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
}

/* ---- [DEFAULT_SETTINGS] section --------------------------------------------- */

static int hub_parse_key(const char* value) {
    const int key = atoi(value);
    return (key > 0 && key < 256) ? key : 0;
}

static void hub_parse_defaults_line(const char* line, HubDefaults* def) {
    const char* v;

    if ((v = hub_value_for(line, "EnableDefaultSettingsOnFirstConnection")) != NULL) {
        def->enabled = hub_parse_bool(v);
    }
    else if ((v = hub_value_for(line, "DefaultWhisperKey")) != NULL) {
        def->whisperKey = hub_parse_key(v);
    }
    else if ((v = hub_value_for(line, "DefaultNormalKey")) != NULL) {
        def->normalKey = hub_parse_key(v);
    }
    else if ((v = hub_value_for(line, "DefaultShoutKey")) != NULL) {
        def->shoutKey = hub_parse_key(v);
    }
    else if ((v = hub_value_for(line, "DefaultVoiceToggleKey")) != NULL) {
        def->voiceToggleKey = hub_parse_key(v);
    }
    else if ((v = hub_value_for(line, "DefaultDistanceWhisper")) != NULL) {
        def->distanceWhisper = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "DefaultDistanceNormal")) != NULL) {
        def->distanceNormal = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
    else if ((v = hub_value_for(line, "DefaultDistanceShout")) != NULL) {
        def->distanceShout = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    }
}

/* ---- [ZONES] section ------------------------------------------------------ */

/* Zone header: "[ZoneName=X]", "[Zone=X]", "ZoneName=X" or "Zone=X".
   Returns name start + length, or NULL. */
static const char* hub_zone_header_name(const char* line, size_t* outLen) {
    const char* nameStart = NULL;
    if (line[0] == '[') {
        if (_strnicmp(line, "[ZoneName=", 10) == 0) {
            nameStart = line + 10;
        }
        else if (_strnicmp(line, "[Zone=", 6) == 0) {
            nameStart = line + 6;
        }
        if (nameStart) {
            const char* end = strchr(nameStart, ']');
            *outLen = end ? (size_t)(end - nameStart) : strlen(nameStart);
        }
    }
    else if (_strnicmp(line, "ZoneName=", 9) == 0) {
        nameStart = line + 9;
        *outLen = strlen(nameStart);
    }
    else if (_strnicmp(line, "Zone=", 5) == 0) {
        nameStart = line + 5;
        *outLen = strlen(nameStart);
    }
    return (nameStart && *outLen > 0) ? nameStart : NULL;
}

static void hub_parse_zone_line(const char* line, HubZone* zone) {
    const char* v;

    if ((v = hub_value_for(line, "X1")) != NULL) zone->x1 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "Z1")) != NULL) zone->z1 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "X2")) != NULL) zone->x2 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "Z2")) != NULL) zone->z2 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "X3")) != NULL) zone->x3 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "Z3")) != NULL) zone->z3 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "X4")) != NULL) zone->x4 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "Z4")) != NULL) zone->z4 = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "AudioMinDistance")) != NULL) zone->audioMinDistance = hub_parse_clamped(v, 0.1f, HUB_DIST_MAX);
    else if ((v = hub_value_for(line, "AudioMaxVolume")) != NULL) zone->audioMaxVolume = hub_parse_max_volume(v);
    else if ((v = hub_value_for(line, "GroundY")) != NULL) zone->groundY = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "TopY")) != NULL) zone->topY = hub_parse_clamped(v, -1e7f, 1e7f);
    else if ((v = hub_value_for(line, "Wisper")) != NULL) zone->whisperDist = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    else if ((v = hub_value_for(line, "Normal")) != NULL) zone->normalDist = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    else if ((v = hub_value_for(line, "Shout")) != NULL) zone->shoutDist = hub_parse_clamped(v, 0.0f, HUB_DIST_MAX);
    else if ((v = hub_value_for(line, "SoundProof")) != NULL) zone->soundproof = hub_parse_bool(v);
    else if ((v = hub_value_for(line, "Reverb")) != NULL) zone->reverb = hub_parse_bool(v);
}

/* ---- main entry -------------------------------------------------------------- */

int hub_parse_settings(const char* description, HubSettings* out) {
    if (!out) {
        return 0;
    }
    hub_defaults(out);
    if (!description || description[0] == '\0') {
        return 0;
    }

    static const char delims[] = "\n\r";
    char buf[HUB_DESC_MAX];
    hub_normalize_html(description, buf, sizeof(buf));

    enum { SEC_NONE, SEC_GLOBAL, SEC_ZONES, SEC_RACE, SEC_DEFAULTS, SEC_OTHER } section = SEC_NONE;
    HubZone* zone = NULL;
    HubRace* race = NULL;

    char* context = NULL;
    for (char* raw = strtok_s(buf, delims, &context); raw != NULL;
        raw = strtok_s(NULL, delims, &context)) {
        char* line = hub_trim(raw);
        if (line[0] == '\0') {
            continue;
        }

        /* Section switches. Zone headers ([Zone=..]) also start with '[' and
           must not be mistaken for a new section. */
        size_t zoneNameLen = 0;
        const char* zoneName = (section == SEC_ZONES)
            ? hub_zone_header_name(line, &zoneNameLen) : NULL;

        if (line[0] == '[' && !zoneName) {
            if (_strnicmp(line, "[GLOBAL]", 8) == 0) {
                section = SEC_GLOBAL;
                out->valid = 1;
            }
            else if (_strnicmp(line, "[ZONES]", 7) == 0) {
                section = SEC_ZONES;
                zone = NULL;
            }
            else if (_strnicmp(line, "[RACE]", 6) == 0) {
                section = SEC_RACE;
                race = NULL;
            }
            else if (_strnicmp(line, "[DEFAULT_SETTINGS]", 18) == 0) {
                section = SEC_DEFAULTS;
            }
            else {
                section = SEC_OTHER;
                zone = NULL;
                race = NULL;
            }
            continue;
        }

        if (section == SEC_GLOBAL) {
            hub_parse_global_line(line, out);
        }
        else if (section == SEC_ZONES) {
            if (!zoneName) {
                zoneName = hub_zone_header_name(line, &zoneNameLen);
            }
            if (zoneName) {
                if (out->zoneCount >= HUB_MAX_ZONES) {
                    zone = NULL;
                    continue;
                }
                zone = &out->zones[out->zoneCount++];
                memset(zone, 0, sizeof(*zone));
                if (zoneNameLen >= sizeof(zone->name)) {
                    zoneNameLen = sizeof(zone->name) - 1;
                }
                memcpy(zone->name, zoneName, zoneNameLen);
                zone->name[zoneNameLen] = '\0';
            }
            else if (zone) {
                hub_parse_zone_line(line, zone);
            }
        }
        else if (section == SEC_RACE) {
            const char* raceName = hub_value_for(line, "Race");
            /* "Race=Name" starts a new race entry; every other line belongs
               to the current one. */
            if (raceName && _strnicmp(line, "Race", 4) == 0 && line[4] == '=') {
                if (out->raceCount >= HUB_MAX_RACES) {
                    race = NULL;
                    continue;
                }
                race = &out->races[out->raceCount++];
                memset(race, 0, sizeof(*race));
                strncpy_s(race->name, sizeof(race->name), raceName, _TRUNCATE);
                /* Race limits default to the global hub limits. */
                race->minWhisper = out->minWhisper;
                race->maxWhisper = out->maxWhisper;
                race->minNormal = out->minNormal;
                race->maxNormal = out->maxNormal;
                race->minShout = out->minShout;
                race->maxShout = out->maxShout;
            }
            else if (race) {
                hub_parse_race_line(line, race);
            }
        }
        else if (section == SEC_DEFAULTS) {
            hub_parse_defaults_line(line, &out->defaults);
        }
    }

    return out->valid;
}
