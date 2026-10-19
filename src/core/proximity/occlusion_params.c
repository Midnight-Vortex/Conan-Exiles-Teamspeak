/*
 * Occlusion v2 mod parameters (doku 072): root-channel key names, defaults and
 * ranges. No dependencies (the hub parser links it without the occlusion store).
 */
#include "core/proximity/occlusion.h"

#include <math.h>

typedef struct OccModParam {
    const char* name;
    int def, min, max;
} OccModParam;

/* Order = index in "occcfg" after the version (doku 072 section 2). */
static const OccModParam k_occModParams[OCCLUSION_MOD_PARAM_COUNT] = {
    { "OccFanEdge",        80,   0,  200 },
    { "OccNear",          150,   0, 1000 },
    { "OccFar",           400,   0, 2000 },
    { "OccDetourNear",    150,  50,  500 },
    { "OccDetourFar",     300,  50,  800 },
    { "OccDetourNearPct",  30,   0,  100 },
    { "OccDetourFarPct",   60,   0,  100 },
    { "OccSameBuilding",   60,   0,  100 },
    { "OccFloor",          70,   0,  100 },
    { "OccThin",           80,   0,  100 },
    { "OccBudget",         60,  20,  200 },
    { "OccFast",            2,   0,    4 },
};

const char* occlusion_mod_param_name(int index) {
    return (index >= 0 && index < OCCLUSION_MOD_PARAM_COUNT) ? k_occModParams[index].name : NULL;
}

int occlusion_mod_param_default(int index) {
    return (index >= 0 && index < OCCLUSION_MOD_PARAM_COUNT) ? k_occModParams[index].def : 0;
}

int occlusion_mod_param_clamp(int index, double value) {
    if (index < 0 || index >= OCCLUSION_MOD_PARAM_COUNT) {
        return 0;
    }
    const OccModParam* m = &k_occModParams[index];
    if (!(value == value)) { /* NaN */
        return m->def;
    }
    if (value < (double)m->min) {
        return m->min;
    }
    if (value > (double)m->max) {
        return m->max;
    }
    return (int)lround(value);
}
