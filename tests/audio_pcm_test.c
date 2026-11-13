/* Host test of the REAL proximity PCM path (review fix 084).

   The test #includes src/ts/proximity/ts3_proximity_audio.c itself (white box:
   it may look at the file's statics) and runs it on Linux:
     - tests/support/audio_shim/windows.h (+ the shared win32_shim) replaces the
       Win32 names the file uses (CRITICAL_SECTION, Interlocked*, GetTickCount64),
     - the 14 external functions (server profile, position, adapter, 3D, log) are
       stubbed below,
     - hrtf.c, player_table.c, proximity_math.c, zone_resolve.c and occlusion*.c
       are linked for real.
   So the hot path (ts3_audio_process_playback / _postprocess), the recompute
   passes, the unmute flush and the player table are exercised end to end, not
   just their helpers. Time is controlled with win32_shim_advance_ms().

   Build/run: bash tests/run_tests.sh (see the run_suite block for the sources).
   Exit code 0 = all checks passed. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef AUDIO_UNDER_TEST
#define AUDIO_UNDER_TEST "ts/proximity/ts3_proximity_audio.c"
#endif
#include AUDIO_UNDER_TEST

static int g_failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  OK   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_failures++; } \
} while (0)

/* ---- stubs for the module's external dependencies -------------------------- */

static PosSample g_testPos;
static int g_testPosValid = 1;
static HubSettings g_testHub;
static int g_testHubActive = 0;
static int g_testRealistic = 0;
static int g_testHrtf = 0;
static int g_cntProfileGet, g_cntMaxVolume, g_cntRealistic, g_cntFilter, g_cntListenAdd;
static int g_wakeups;
static anyID g_unmuteCalls[8][64];
static int g_unmuteCallLen[8];
static int g_unmuteCallCount;

/* 099: log_write is captured (the diagnostics tests read the lines back);
   log_debug stays silent - the whole point is that the evidence needs no DebugMode. */
static char g_logLines[256][320];
static int g_logCount;
void log_write(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_logCount < 256) {
        vsnprintf(g_logLines[g_logCount++], sizeof(g_logLines[0]), fmt, ap);
    }
    va_end(ap);
}
void log_debug(const char* fmt, ...) { (void)fmt; }

int pos_get_current(PosSample* out) {
    if (!g_testPosValid) return 0;
    *out = g_testPos;
    return 1;
}

int server_profile_get(HubSettings* out) {
    g_cntProfileGet++;
    if (!g_testHubActive) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = g_testHub;
    return 1;
}
float server_profile_get_max_volume(void) {
    g_cntMaxVolume++;
    return g_testHubActive && g_testHub.audioMaxVolume > 0.0f ? g_testHub.audioMaxVolume : 1.0f;
}
static float g_testListenAdd = 0.0f; /* 103: listener bonus of the local player (meters) */
float server_profile_get_listen_add_distance(void) { g_cntListenAdd++; return g_testListenAdd; }
int server_profile_get_realistic_audio(void) { g_cntRealistic++; return g_testRealistic; }
float server_profile_get_filter_intensity(void) { g_cntFilter++; return 100.0f; }
int server_profile_get_hrtf_enabled(void) { return g_testHrtf; }

int ts3_is_connected(void) { return 1; }
int ts3_thread_is_callback(void) { return 1; }
void ts3_request_wakeup(void) { g_wakeups++; }
void ts3d_invalidate_client(anyID clientID) { (void)clientID; }

int ts3_unmute_clients_for_pcm(const anyID* clients, int count) {
    const int n = count > 63 ? 63 : count;
    if (g_unmuteCallCount < 8) {
        for (int i = 0; i < n; i++) g_unmuteCalls[g_unmuteCallCount][i] = clients[i];
        g_unmuteCallLen[g_unmuteCallCount] = n;
    }
    g_unmuteCallCount++;
    return n;
}

/* ---- helpers --------------------------------------------------------------- */

#define FRAMES 960
static short g_buf[FRAMES * 2];

static void t_fill(short* b, int frames, int channels, short v) {
    for (int i = 0; i < frames * channels; i++) b[i] = v;
}

static float t_peak(const short* b, int count) {
    float p = 0.0f;
    for (int i = 0; i < count; i++) {
        const float a = fabsf((float)b[i]);
        if (a > p) p = a;
    }
    return p;
}

static void t_set_local(double xCm, double yCm, double zCm) {
    g_testPos.seq++;
    g_testPos.x = xCm; g_testPos.y = yCm; g_testPos.z = zCm;
    g_testPos.yaw = 0.0; g_testPos.yawY = 0.0;
}

static void t_world_reset(void) {
    ts3_audio_reset();
    player_table_clear();
    memset(&g_testHub, 0, sizeof(g_testHub));
    g_testHubActive = 0;
    g_testRealistic = 0;
    g_testHrtf = 0;
    g_testPosValid = 1;
    t_set_local(0.0, 0.0, 0.0);
    ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);
    ts3_audio_clear_client_deaf();
}

/* Remote player `distM` meters in front, voice range `voice`, then publish. */
static void t_put_and_publish(unsigned id, float distM, float voice) {
    player_table_put((unsigned short)id, "p", distM, 0.0f, 0.0f, voice, NULL);
    ts3_audio_recompute_all_force();
}

static int t_target(unsigned id, AudioTarget* t) {
    return audio_snap_read(&g_snap[id], t) == AUDIO_SNAP_OK;
}

/* Process one constant stereo block (L = R = 10000). */
static void t_block(unsigned id, short level) {
    t_fill(g_buf, FRAMES, 2, level);
    ts3_audio_process_playback((anyID)id, g_buf, FRAMES, 2);
}

static float t_ampl(const AudioTarget* t) {
    return t->panL > t->panR ? t->panL : t->panR;
}

/* ===========================================================================
 * 3: gain ramp restart / stale state
 * ======================================================================== */
static void test_no_leak_after_generation_bump(void) {
    printf("[1] fix 3: first block after a generation bump starts at the target (no 1.0 leak)\n");
    t_world_reset();
    const unsigned id = 20;
    t_put_and_publish(id, 35.0f, 40.0f);          /* far speaker: low gain */
    AudioTarget t;
    CHECK(t_target(id, &t) && t.gain > 0.001f && t.gain < 0.5f, "far speaker has a low target gain");

    t_block(id, 10000);                            /* warm-up block (first ever) */
    ts3_audio_invalidate_client((anyID)id);        /* client left ... */
    t_put_and_publish(id, 35.0f, 40.0f);           /* ... ID reused by a far speaker */
    CHECK(t_target(id, &t), "snapshot valid again after reuse");

    t_block(id, 10000);
    const float limit = t.gain * 10000.0f * t_ampl(&t) + 2.0f;
    CHECK(t_peak(g_buf, FRAMES * 2) <= limit, "no sample of the first block exceeds the target level");
    CHECK(fabsf((float)g_buf[0]) <= limit, "first sample already at target level (old code: ~1.0)");
}

static void test_gap_restart(void) {
    printf("[2] fix 3: after a talk pause the block starts at the new target; no pause -> smooth ramp\n");
    t_world_reset();
    const unsigned id = 21;
    t_put_and_publish(id, 5.0f, 40.0f);            /* close: high gain */
    AudioTarget near;
    CHECK(t_target(id, &near) && near.gain > 0.8f, "close speaker: high gain");
    t_block(id, 10000);
    t_block(id, 10000);

    /* Without a pause the ramp continues from the previous gain (continuity). */
    t_put_and_publish(id, 36.0f, 40.0f);
    AudioTarget far;
    CHECK(t_target(id, &far) && far.gain < 0.3f, "speaker moved far away: low gain");
    t_block(id, 10000);
    CHECK(fabsf((float)g_buf[0]) > 0.5f * 10000.0f * near.gain * near.panL,
        "no pause: block starts near the previous (high) level and ramps down");
    CHECK(fabsf((float)g_buf[FRAMES * 2 - 2]) <= far.gain * 10000.0f * t_ampl(&far) * 1.05f + 2.0f,
        "no pause: block ends at the new target");

    /* With a pause (TS stopped calling) the new utterance starts at the target. */
    t_put_and_publish(id, 5.0f, 40.0f);
    t_block(id, 10000);                            /* back to loud, settled */
    t_block(id, 10000);
    t_put_and_publish(id, 36.0f, 40.0f);
    win32_shim_advance_ms(500);
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) <= far.gain * 10000.0f * t_ampl(&far) + 2.0f,
        "after a pause the very first block is at the target level (no ramp from the old loud value)");
}

static void test_mode_switch_continuity(void) {
    printf("[3] fix 3: PASSTHROUGH -> PROXIMITY in mid-utterance still ramps from 1.0\n");
    t_world_reset();
    const unsigned id = 22;
    t_put_and_publish(id, 36.0f, 40.0f);
    AudioTarget t;
    CHECK(t_target(id, &t) && t.gain < 0.3f, "far speaker: low gain");

    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    static short mono[FRAMES];
    t_fill(mono, FRAMES, 1, 10000);
    ts3_audio_process_playback((anyID)id, mono, FRAMES, 1);   /* plays unchanged at gain 1.0 */
    CHECK(mono[0] == 10000 && mono[FRAMES - 1] == 10000, "passthrough leaves the voice untouched");

    ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);                  /* switch mid-utterance */
    t_fill(mono, FRAMES, 1, 10000);
    ts3_audio_process_playback((anyID)id, mono, FRAMES, 1);
    CHECK(mono[0] > 9000, "continuity: first sample still ~1.0 (no click down to the target)");
    CHECK(fabsf((float)mono[FRAMES - 1] - t.gain * 10000.0f) < 100.0f, "ramp ends at the target gain");
    int monotonic = 1;
    for (int i = 1; i < FRAMES; i++) {
        if (mono[i] > mono[i - 1]) { monotonic = 0; break; }
    }
    CHECK(monotonic, "smooth monotonic ramp down");
}

static void test_cave_tail_not_replayed(void) {
    printf("[4] fix 3 + 7: cave tail / lowpass history of the previous utterance is not replayed\n");
    t_world_reset();
    const unsigned id = 50;
    g_testHubActive = 1;
    g_testHub.valid = 1;
    g_testHub.zoneCount = 1;
    g_testHub.zones[0].x1 = -1000; g_testHub.zones[0].z1 = -1000;
    g_testHub.zones[0].x2 = 1000;  g_testHub.zones[0].z2 = -1000;
    g_testHub.zones[0].x3 = 1000;  g_testHub.zones[0].z3 = 1000;
    g_testHub.zones[0].x4 = -1000; g_testHub.zones[0].z4 = 1000;
    g_testHub.zones[0].reverb = 1;
    g_testHub.caveReverb = 100.0f; /* 095: echo strength = CaveReverb / 100 */
    t_put_and_publish(id, 5.0f, 40.0f);
    AudioTarget t;
    CHECK(t_target(id, &t) && t.reverbSlot >= 0 && t.reverbWet > 0.9f, "speaker is in the cave (slot assigned)");
    CHECK(t.cutoffHz < 19000.0f, "cave lowpass active");

    /* Utterance 1: noise burst, then silence blocks WITHOUT a pause -> tail audible. */
    static short noise[FRAMES * 2];
    unsigned int r = 7u;
    for (int i = 0; i < FRAMES * 2; i++) { r = r * 1664525u + 1013904223u; noise[i] = (short)((int)((r >> 8) % 20001u) - 10000); }
    memcpy(g_buf, noise, sizeof(noise));
    ts3_audio_process_playback((anyID)id, g_buf, FRAMES, 2);
    float tail = 0.0f;
    for (int b = 0; b < 3; b++) { t_block(id, 0); tail += t_peak(g_buf, FRAMES * 2); }
    CHECK(tail > 0.0f, "without a pause the reverb tail is audible (reverb works)");

    /* Utterance 2 after a pause: the stale tail must be gone. */
    win32_shim_advance_ms(500);
    float tail2 = 0.0f;
    for (int b = 0; b < 3; b++) { t_block(id, 0); tail2 += t_peak(g_buf, FRAMES * 2); }
    CHECK(tail2 == 0.0f, "after a pause no reverb tail of the previous utterance replays");

    /* The lowpass is re-seeded after a pause: a DC block (previous memory ~0)
       passes with a constant level from the very first sample - a stale filter
       memory would make the block start low and climb. */
    win32_shim_advance_ms(500);
    t_block(id, 8000);
    const float first = (float)g_buf[0];
    const float last = (float)g_buf[(FRAMES - 1) * 2];
    CHECK(first > 100.0f && fabsf(first - last) <= 2.0f, "lowpass memory re-seeded: DC level constant from sample 0");
}

/* ===========================================================================
 * 6: seqlock BUSY
 * ======================================================================== */
static void test_busy_uses_cache(void) {
    printf("[5] fix 6: a busy (mid-publish) snapshot neither mutes nor resets the render state\n");
    t_world_reset();
    const unsigned id = 23;
    t_put_and_publish(id, 20.0f, 40.0f);
    AudioTarget t;
    CHECK(t_target(id, &t), "snapshot valid");
    t_block(id, 10000);                                       /* fills the cache */
    t_block(id, 10000);
    const float levelOk = (float)g_buf[FRAMES * 2 - 2];
    CHECK(levelOk > 100.0f, "audible at steady state");
    const float renderGain = g_renderGain[id];
    const int lpfInit = g_lpf[id].initialized;

    g_snap[id].seq += 1;                                       /* writer entered mid-publish */
    t_block(id, 10000);
    const float levelBusy = (float)g_buf[FRAMES * 2 - 2];
    CHECK(fabsf(levelBusy - levelOk) < 2.0f, "BUSY: still rendered with the last good target (not muted)");
    CHECK(g_renderGain[id] == renderGain, "BUSY: render gain untouched");
    CHECK(g_lpf[id].initialized == lpfInit, "BUSY: lowpass state untouched");
    g_snap[id].seq += 1;                                       /* writer done */

    /* INVALID is different: that mutes and resets. */
    audio_snap_publish(&g_snap[id], &t, 0);
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "INVALID: block muted");
    CHECK(g_renderGain[id] == 0.0f, "INVALID: render gain reset");
    audio_snap_publish(&g_snap[id], &t, 1);
}

static void test_busy_without_cache(void) {
    printf("[6] fix 6: BUSY with no usable cache silences the block but keeps the render state\n");
    t_world_reset();
    const unsigned id = 24;
    t_put_and_publish(id, 36.0f, 40.0f);
    AudioTarget t;
    CHECK(t_target(id, &t), "snapshot valid");
    g_snap[id].seq += 1;                                       /* busy from the very first block */
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "no cache: block silenced (never full volume)");
    /* The reset at the start of the block left gain 1.0 (reinit value); the
       INVALID/mute paths would have written 0.0 - BUSY must not. */
    CHECK(g_renderGain[id] == 1.0f, "render gain not zeroed by the busy block");
    g_snap[id].seq += 1;
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) <= t.gain * 10000.0f * t_ampl(&t) + 2.0f,
        "fresh start survived the busy block: first real block is at the target (no 1.0 leak)");
}

/* ===========================================================================
 * 10: out-of-range IDs
 * ======================================================================== */
static void test_out_of_range_ids(void) {
    printf("[7] fix 10 / 099: ID 0 (no per-client state) is silenced in MUTE and PROXIMITY mode, untouched in PASSTHROUGH; every 16-bit ID gets the distance gain\n");
    t_world_reset();
    {
        const anyID id = 0;
        ts3_audio_set_mode(TS3_AUDIO_MUTE);
        t_fill(g_buf, FRAMES, 2, 10000);
        ts3_audio_process_playback(id, g_buf, FRAMES, 2);
        CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "MUTE: id 0 silenced");

        ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);
        t_fill(g_buf, FRAMES, 2, 10000);
        ts3_audio_process_playback(id, g_buf, FRAMES, 2);
        CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "PROXIMITY: id 0 silenced (fail closed, no full-volume leak)");

        ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
        t_fill(g_buf, FRAMES, 2, 10000);
        ts3_audio_process_playback(id, g_buf, FRAMES, 2);
        CHECK(g_buf[0] == 10000, "PASSTHROUGH: id 0 unchanged");
    }
    /* 099: the former cap (4096) made these IDs invisible: no CEPOS, voice untouched = full volume. */
    const anyID high[] = { 4096, 5000, 60000, 65535 };
    for (int k = 0; k < 4; k++) {
        const unsigned id = high[k];
        char name[112];

        t_world_reset();                                        /* PROXIMITY */
        t_fill(g_buf, FRAMES, 2, 10000);
        ts3_audio_process_playback((anyID)id, g_buf, FRAMES, 2);
        snprintf(name, sizeof(name), "id %u: no position data yet -> silent", id);
        CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, name);

        t_put_and_publish(id, 60.0f, 40.0f);                    /* outside his 40 m range */
        t_block(id, 10000);
        snprintf(name, sizeof(name), "id %u: 60 m away (range 40 m) -> silent, not full volume", id);
        CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, name);

        t_put_and_publish(id, 5.0f, 40.0f);                     /* close by */
        AudioTarget tg;
        memset(&tg, 0, sizeof(tg));
        snprintf(name, sizeof(name), "id %u: snapshot published", id);
        CHECK(t_target(id, &tg) && tg.gain > 0.5f, name);
        t_block(id, 10000);
        snprintf(name, sizeof(name), "id %u: block follows the snapshot (attenuated, not passed through)", id);
        CHECK(t_peak(g_buf, FRAMES * 2) > 0.0f
            && t_peak(g_buf, FRAMES * 2) <= tg.gain * 10000.0f * t_ampl(&tg) + 2.0f, name);
    }
    ts3_audio_set_mode(TS3_AUDIO_MUTE);
    ts3_audio_process_playback(4097, NULL, FRAMES, 2);        /* must not crash */
    ts3_audio_process_playback(4097, g_buf, 0, 2);
    CHECK(1, "NULL / empty buffers are ignored");
}

/* ===========================================================================
 * 1: unmute hand-over without a ring / lock
 * ======================================================================== */
static void test_unmute_flags(void) {
    printf("[8] fix 1: the audio thread only sets the flag; the callback thread scans and batches\n");
    t_world_reset();
    g_unmuteCallCount = 0;
    const unsigned id = 25;
    t_put_and_publish(id, 10.0f, 40.0f);
    /* recompute (callback side) flags audible speakers as well */
    ts3_audio_reset();                                          /* clean flags/counter */
    CHECK(!ts3_audio_has_pending_unmutes(), "reset clears the unmute flags");
    t_put_and_publish(id, 10.0f, 40.0f);
    ts3_audio_reset();
    t_put_and_publish(id, 10.0f, 40.0f);
    /* reset wipes player table state of audio only; make sure the flag comes from the PCM path */
    g_pendingUnmute[id] = 0;
    g_pendingUnmuteCount = 0;

    t_block(id, 10000);                                         /* audio thread flags the client */
    CHECK(g_pendingUnmute[id] == 1 && ts3_audio_has_pending_unmutes(), "PCM path set the flag + counter");

    ts3_audio_flush_unmutes();
    CHECK(g_unmuteCallCount == 1 && g_unmuteCallLen[0] == 1 && g_unmuteCalls[0][0] == id,
        "flush found the flagged client and unmuted it in one batch");
    CHECK(g_pendingUnmute[id] == 0 && !ts3_audio_has_pending_unmutes(), "flag + counter cleared");

    /* Re-arm limit: flagged again at once, but not unmuted again within 500 ms. */
    t_block(id, 10000);
    ts3_audio_flush_unmutes();
    CHECK(g_unmuteCallCount == 1, "re-unmute is rate-limited (500 ms)");
    CHECK(g_pendingUnmute[id] == 1, "the request is not discarded");
    win32_shim_advance_ms(600);
    ts3_audio_flush_unmutes();
    CHECK(g_unmuteCallCount == 2 && g_unmuteCalls[1][0] == id, "unmuted again after the window");

    /* More than one batch worth of pending clients: nobody is lost, nobody starves. */
    g_unmuteCallCount = 0;
    for (unsigned c = 100; c < 200; c++) ts3_audio_signal_unmute((anyID)c);
    ts3_audio_flush_unmutes();
    ts3_audio_flush_unmutes();
    ts3_audio_flush_unmutes();
    int seen[200] = { 0 };
    int total = 0;
    for (int i = 0; i < g_unmuteCallCount && i < 8; i++) {
        for (int j = 0; j < g_unmuteCallLen[i]; j++) {
            if (g_unmuteCalls[i][j] >= 100 && g_unmuteCalls[i][j] < 200) { seen[g_unmuteCalls[i][j]]++; total++; }
        }
    }
    int allOnce = 1;
    for (int c = 100; c < 200; c++) if (seen[c] != 1) allOnce = 0;
    CHECK(allOnce && total == 100, "100 pending clients are all unmuted exactly once over the batches");
    CHECK(g_unmuteCallLen[0] == 63, "a batch is capped at 63 clients (API limit)");

    /* The PCM function must not wait on anything: it returns even while the
       callback side is "inside" ts3_audio_signal_unmute — there is no lock to hold. */
    ts3_audio_signal_unmute(30);
    t_block(id, 10000);
    CHECK(1, "no ring, no push lock");
}

/* ===========================================================================
 * 2: expired player-table entries
 * ======================================================================== */
static void test_expired_entry_mutes(void) {
    printf("[9] fix 2: an expired player-table entry withdraws the audio snapshot (mute)\n");
    t_world_reset();
    const unsigned a = 30, b = 31;
    t_put_and_publish(a, 10.0f, 40.0f);
    player_table_put((unsigned short)b, "b", 12.0f, 0.0f, 0.0f, 40.0f, NULL);
    ts3_audio_recompute_all_force();
    AudioTarget t;
    CHECK(t_target(a, &t) && t_target(b, &t), "both speakers audible");
    t_block(a, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) > 100.0f, "speaker a is heard");

    /* b keeps sending CEPOS, a goes silent (crashed / quit, still in the channel). */
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2);
    player_table_put((unsigned short)b, "b", 12.0f, 0.0f, 0.0f, 40.0f, NULL);
    win32_shim_advance_ms(PLAYER_TABLE_STALE_MS / 2 + 1000);
    player_table_put((unsigned short)b, "b", 12.0f, 0.0f, 0.0f, 40.0f, NULL);
    ts3_audio_recompute_all_force();

    CHECK(g_snap[a].valid == 0, "expired speaker a: snapshot withdrawn");
    CHECK(g_snap[b].valid == 1, "fresh speaker b: snapshot kept");
    t_block(a, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "expired speaker a is muted");
    t_block(b, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) > 100.0f, "fresh speaker b still audible");

    /* The peer comes back: normal CEPOS path publishes again. */
    t_put_and_publish(a, 10.0f, 40.0f);
    t_block(a, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) > 100.0f, "speaker a is audible again after new CEPOS");
}

/* ===========================================================================
 * 7: cave slot recycle
 * ======================================================================== */
static void t_cave_hub(void) {
    g_testHubActive = 1;
    g_testHub.valid = 1;
    g_testHub.zoneCount = 1;
    g_testHub.zones[0].x1 = -1000; g_testHub.zones[0].z1 = -1000;
    g_testHub.zones[0].x2 = 1000;  g_testHub.zones[0].z2 = -1000;
    g_testHub.zones[0].x3 = 1000;  g_testHub.zones[0].z3 = 1000;
    g_testHub.zones[0].x4 = -1000; g_testHub.zones[0].z4 = 1000;
    g_testHub.zones[0].reverb = 1;
    g_testHub.caveReverb = 100.0f; /* 095: echo strength = CaveReverb / 100 */
}

static int t_dsp_is_zero(const CaveDsp* d) {
    const unsigned char* p = (const unsigned char*)d;
    for (size_t i = 0; i < sizeof(*d); i++) if (p[i]) return 0;
    return 1;
}

static void test_cave_slot_recycle(void) {
    printf("[10] fix 7: the callback thread never writes a slot's buffers; the audio thread zeroes on a new owner\n");
    t_world_reset();
    t_cave_hub();
    const unsigned a = 40, b = 41;
    t_put_and_publish(a, 5.0f, 40.0f);                         /* A takes the first free slot */
    player_table_put((unsigned short)b, "b", 6.0f, 0.0f, 0.0f, 40.0f, NULL);
    AudioTarget ta;
    CHECK(t_target(a, &ta) && ta.reverbSlot >= 0, "A is in the cave");
    const int slot = ta.reverbSlot;
    static short noise[FRAMES * 2];
    unsigned int r = 99u;
    for (int i = 0; i < FRAMES * 2; i++) { r = r * 1664525u + 1013904223u; noise[i] = (short)((int)((r >> 8) % 20001u) - 10000); }
    for (int k = 0; k < 3; k++) { memcpy(g_buf, noise, sizeof(noise)); ts3_audio_process_playback((anyID)a, g_buf, FRAMES, 2); }
    CHECK(!t_dsp_is_zero(&g_cave[slot].dsp), "A's speech filled the slot with reverb history");

    /* The cave switches off: both lose their reverb, the slot is released. B is
       already talking (warm, no fresh start) when the cave comes back later. */
    g_testHubActive = 0;
    ts3_audio_recompute_all_force();
    t_block(b, 10000);
    t_block(b, 10000);
    player_table_remove((unsigned short)a);                   /* A left */

    /* Cave back on: B (only one left in the table) is handed A's old slot. */
    t_cave_hub();
    ts3_audio_recompute_all_force();
    AudioTarget tb;
    CHECK(t_target(b, &tb) && tb.reverbSlot == slot, "B was handed the just released slot");
    CHECK(!t_dsp_is_zero(&g_cave[slot].dsp),
        "acquire did NOT memset the buffers on the callback thread (audio thread may still be in them)");

    /* B's next block (silence in, NO talk pause): no residue of A's reverb may come out. */
    float tail = 0.0f;
    for (int k = 0; k < 3; k++) { t_block(b, 0); tail += t_peak(g_buf, FRAMES * 2); }
    CHECK(tail == 0.0f, "B starts with a clean reverb (zeroed by the audio thread on the new epoch)");
    CHECK(g_cave[slot].seenEpoch == g_cave[slot].acquireEpoch, "audio thread consumed the new epoch");
}

/* ===========================================================================
 * 3 (HRTF) + 6: post-process
 * ======================================================================== */
static void test_hrtf_gap_and_busy(void) {
    printf("[11] fix 3 + 6: HRTF history is reset after a pause; a busy direction does not tear the state down\n");
    t_world_reset();
    g_testHrtf = 1;
    const unsigned id = 60;
    t_put_and_publish(id, 10.0f, 40.0f);
    AudioDir dir;
    CHECK(audio_dir_read(&g_dirSnap[id], &dir) == AUDIO_SNAP_OK, "direction published");

    unsigned int speakers[2] = { SPEAKER_HEADPHONES_LEFT, SPEAKER_HEADPHONES_RIGHT };
    unsigned int fill = 0x3;
    #define PP_BLOCK() do { t_fill(g_buf, FRAMES, 2, 6000); fill = 0x3; \
        ts3_audio_process_postprocess((anyID)id, g_buf, FRAMES, 2, speakers, &fill); } while (0)

    PP_BLOCK();
    CHECK(g_hrtfActive[id] == 1 && g_hrtf[id].pos == FRAMES, "block 1: HRTF running");
    PP_BLOCK();
    CHECK(g_hrtf[id].pos == 2u * FRAMES, "block 2 (no pause): history continues");

    win32_shim_advance_ms(500);
    PP_BLOCK();
    CHECK(g_hrtf[id].pos == FRAMES, "after a pause the HRTF state starts fresh (old code: kept the stale history)");

    /* Busy direction: reuse the cached one, keep the state. */
    g_dirSnap[id].seq += 1;
    PP_BLOCK();
    CHECK(g_hrtfActive[id] == 1 && g_hrtf[id].pos == 2u * FRAMES, "BUSY: processed with the cached direction, state kept");
    g_dirSnap[id].seq += 1;

    /* Invalid direction switches HRTF off for the block (as before). */
    audio_dir_publish(&g_dirSnap[id], 0.0f, 0.0f, 0);
    PP_BLOCK();
    CHECK(g_hrtfActive[id] == 0, "INVALID: HRTF off");
    #undef PP_BLOCK
}

/* ===========================================================================
 * 8: hub values once per pass
 * ======================================================================== */
static void test_once_per_pass(void) {
    printf("[12] fix 8: hub values are fetched once per pass, not once per client\n");
    t_world_reset();
    g_testRealistic = 1;
    for (unsigned id = 200; id < 300; id++) {
        player_table_put((unsigned short)id, "p", 5.0f + (float)(id - 200) * 0.2f, 0.0f, 0.0f, 40.0f, NULL);
    }
    g_cntProfileGet = g_cntMaxVolume = g_cntRealistic = g_cntFilter = g_cntListenAdd = 0;
    ts3_audio_recompute_all_force();
    CHECK(g_cntProfileGet == 1, "recompute-all: HubSettings copied once for 100 clients");
    CHECK(g_cntMaxVolume == 1 && g_cntRealistic == 1 && g_cntFilter == 1 && g_cntListenAdd == 1,
        "recompute-all: scalar getters called once for 100 clients");
    AudioTarget t;
    CHECK(t_target(250, &t) && t.cutoffHz < 19000.0f, "realistic audio still applied (lowpass)");

    /* Dirty drain: 40 CEPOS-dirty clients. */
    for (unsigned id = 200; id < 240; id++) ts3_audio_mark_client_dirty((anyID)id);
    g_cntProfileGet = g_cntMaxVolume = g_cntRealistic = g_cntFilter = g_cntListenAdd = 0;
    ts3_audio_flush_recomputes();
    CHECK(g_cntProfileGet == 1 && g_cntMaxVolume == 1 && g_cntRealistic == 1 && g_cntFilter == 1,
        "dirty drain of 40 clients: one profile fetch");
    CHECK(!ts3_audio_has_pending_recompute(), "dirty flags cleared");
}

/* ===========================================================================
 * 9: voice distance cap
 * ======================================================================== */
static void t_limits_hub(void) {
    g_testHubActive = 1;
    g_testHub.valid = 1;
    g_testHub.maxWhisper = 10.0f;
    g_testHub.maxNormal = 30.0f;
    g_testHub.maxShout = 100.0f;
}

static void test_voice_cap(void) {
    printf("[13] fix 9: a peer's announced voice distance is capped by the hub limits\n");
    t_world_reset();
    t_limits_hub();

    /* Modified client announces 1000 m but sits 500 m away: out of range with the cap. */
    t_put_and_publish(70, 500.0f, 1000.0f);
    AudioTarget t;
    CHECK(!t_target(70, &t), "1000 m announced at 500 m distance: not audible (cap 100 m)");

    /* At 90 m with 1000 m announced: same gain as an honest 100 m shout. */
    t_put_and_publish(71, 90.0f, 1000.0f);
    t_put_and_publish(72, 90.0f, 100.0f);
    AudioTarget cheat, honest;
    CHECK(t_target(71, &cheat) && t_target(72, &honest), "both audible at 90 m");
    CHECK(fabsf(cheat.gain - honest.gain) < 1e-6f, "cheater's gain == honest shout gain (no bypass)");

    /* Legit values are never cut. */
    t_put_and_publish(73, 25.0f, 30.0f);
    AudioTarget legit;
    CHECK(t_target(73, &legit), "legit normal voice audible");
    CHECK(fabsf(legit.gain - prox_volume_from_distance(25.0f, 30.0f, 1.0f)) < 1e-6f,
        "legit 30 m voice: gain unchanged by the cap");

    /* No limits in the profile -> unchanged behaviour (1000 m is honored). */
    g_testHub.maxWhisper = g_testHub.maxNormal = g_testHub.maxShout = 0.0f;
    t_put_and_publish(74, 500.0f, 1000.0f);
    CHECK(t_target(74, &t), "no hub limits: announced 1000 m is honored as before");

    /* No profile at all -> unchanged. */
    g_testHubActive = 0;
    t_put_and_publish(75, 500.0f, 1000.0f);
    CHECK(t_target(75, &t), "no profile: unchanged");
}

/* 103: the listener bonus in the LIVE path (audio_pass_begin ->
   audio_remote_hear_range -> gain). 1 foundation = 2.56 m: whisper 1 F, normal
   6 F, shout 16 F, bonus 4 F. Fails when the rule is taken out of the path. */
static void test_listen_bonus_live(void) {
    printf("[13b] 103: listener bonus in the live path - whisper gets its share, normal and shout the full bonus\n");
    const float F = 2.56f, whisper = 1.0f * F, normal = 6.0f * F, shout = 16.0f * F, add = 4.0f * F;
    t_world_reset();
    t_limits_hub();
    g_testHub.minWhisper = g_testHub.maxWhisper = whisper;
    g_testHub.minNormal = g_testHub.maxNormal = normal;
    g_testHub.minShout = g_testHub.maxShout = shout;
    g_testListenAdd = add;

    AudioPass pass;
    audio_pass_begin(&pass);
    CHECK(fabsf(pass.listenRef - normal) < 1e-4f && fabsf(pass.listenAdd - add) < 1e-4f,
        "pass carries the bonus and the normal range as reference");

    PlayerEntry p;
    memset(&p, 0, sizeof(p));
    p.voiceDistance = whisper;
    CHECK(fabsf(audio_remote_hear_range(&pass, &p) - 4.266667f) < 1e-3f, "whisper: 4.27 m (was 12.8 m)");
    p.voiceDistance = normal;
    CHECK(fabsf(audio_remote_hear_range(&pass, &p) - 25.6f) < 1e-3f, "normal: 25.6 m, unchanged");
    p.voiceDistance = shout;
    CHECK(fabsf(audio_remote_hear_range(&pass, &p) - 51.2f) < 1e-3f, "shout: 51.2 m, unchanged");
    p.voiceDistance = 1000.0f;
    CHECK(fabsf(audio_remote_hear_range(&pass, &p) - 51.2f) < 1e-3f, "1000 m announced: cut to the cap first, then the full bonus");

    /* The published gain follows: a whisperer 5 m away is silent now, a normal speaker is not. */
    AudioTarget t;
    t_put_and_publish(80, 5.0f, whisper);
    CHECK(!t_target(80, &t) || t.gain < 0.001f, "whisper at 5 m: not audible (old rule: 54 %)");
    t_put_and_publish(81, 2.56f, whisper);
    CHECK(t_target(81, &t) && fabsf(t.gain - prox_volume_from_distance(2.56f, 4.266667f, 1.0f)) < 1e-4f,
        "whisper at 2.56 m: gain of a 4.27 m range");
    t_put_and_publish(82, 20.0f, normal);
    CHECK(t_target(82, &t) && fabsf(t.gain - prox_volume_from_distance(20.0f, 25.6f, 1.0f)) < 1e-4f,
        "normal at 20 m: gain of the full 25.6 m range");

    /* A server with a range of allowed normal distances: the MINIMUM is the reference,
       so a player who set his normal voice to the minimum still gets the full bonus. */
    g_testHub.minNormal = 10.0f;
    g_testHub.maxNormal = 20.0f;
    audio_pass_begin(&pass);
    p.voiceDistance = 10.0f;
    CHECK(fabsf(pass.listenRef - 10.0f) < 1e-4f && fabsf(audio_remote_hear_range(&pass, &p) - (10.0f + add)) < 1e-3f,
        "min 10 / max 20: normal speech at the minimum keeps the full bonus");
    p.voiceDistance = 13.0f;
    CHECK(fabsf(audio_remote_hear_range(&pass, &p) - (13.0f + add)) < 1e-3f, "... and so does 13 m (plugin default)");

    /* A race whose normal range is shorter lowers the reference for everybody. */
    g_testHub.raceCount = 1;
    g_testHub.races[0].minNormal = g_testHub.races[0].maxNormal = 6.0f;
    audio_pass_begin(&pass);
    p.voiceDistance = 6.0f;
    CHECK(fabsf(pass.listenRef - 6.0f) < 1e-4f && fabsf(audio_remote_hear_range(&pass, &p) - (6.0f + add)) < 1e-3f,
        "race with normal 6 m: its normal speech keeps the full bonus");
    g_testHub.raceCount = 0;

    /* No profile: the bonus is never reduced. */
    g_testHubActive = 0;
    audio_pass_begin(&pass);
    p.voiceDistance = whisper;
    CHECK(pass.listenRef == 0.0f && fabsf(audio_remote_hear_range(&pass, &p) - (whisper + add)) < 1e-4f,
        "no profile: full bonus as before");
    g_testListenAdd = 0.0f;
}

/* ===========================================================================
 * misc: deaf, reset
 * ======================================================================== */
static void test_misc(void) {
    printf("[14] regression: deaf speaker, reset, mode MUTE\n");
    t_world_reset();
    const unsigned id = 80;
    t_put_and_publish(id, 10.0f, 40.0f);
    ts3_audio_set_client_deaf((anyID)id, 1);
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "ForceSpeakersOn deaf speaker silenced");
    ts3_audio_set_client_deaf((anyID)id, 0);
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) > 100.0f, "audible again");

    ts3_audio_set_mode(TS3_AUDIO_MUTE);
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "hub MUTE silences everyone");
    ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH);
    t_block(id, 10000);
    CHECK(g_buf[0] == 10000, "passthrough leaves the voice alone");
    ts3_audio_set_mode(TS3_AUDIO_PROXIMITY);

    ts3_audio_reset();
    t_block(id, 10000);
    CHECK(t_peak(g_buf, FRAMES * 2) == 0.0f, "after reset the speaker has no data -> muted");
    CHECK(ts3_proximity_audio_soundproof_muted(id) == 0, "soundproof query still works");
}

/* 095: the echo in a Reverb=True zone follows CaveReverb; a room outside any zone
   gets no echo unless OcclusionReverb is set (houses must not echo). */
static void test_cave_reverb_strength(void) {
    printf("[095] CaveReverb scales the cave echo; 0 = no echo slot; outside a zone no echo\n");
    t_world_reset();
    const unsigned id = 60;
    t_cave_hub();
    g_testHub.caveReverb = 70.0f;
    t_put_and_publish(id, 5.0f, 40.0f);
    AudioTarget t;
    CHECK(t_target(id, &t) && t.reverbSlot >= 0 && fabsf(t.reverbWet - 0.7f) < 0.001f,
        "CaveReverb=70 -> cave echo at 0.70 of the old fixed strength");
    CHECK(t.cutoffHz < 19000.0f, "the dull cave lowpass is independent of CaveReverb");

    g_testHub.caveReverb = 0.0f;
    t_put_and_publish(id, 5.0f, 40.0f);
    CHECK(t_target(id, &t) && t.reverbSlot < 0 && t.reverbWet < 0.001f,
        "CaveReverb=0 -> no echo slot");
    CHECK(t.cutoffHz < 19000.0f, "CaveReverb=0 keeps the dull cave lowpass");

    g_testHub.zones[0].reverb = 0;
    g_testHub.caveReverb = 100.0f;
    t_put_and_publish(id, 5.0f, 40.0f);
    CHECK(t_target(id, &t) && t.reverbSlot < 0 && t.reverbWet < 0.001f,
        "zone without Reverb=True -> no echo (house / room)");
}

static int t_log_count(const char* needle) {
    int n = 0;
    for (int i = 0; i < g_logCount; i++) {
        if (strstr(g_logLines[i], needle) != NULL) n++;
    }
    return n;
}

/* ===========================================================================
 * 099: evidence in plugin.log without DebugMode
 * ======================================================================== */
static void test_diagnostics(void) {
    printf("[099] AUDIO-STAT / AUDIO-SNAP / stale warning are written without DebugMode\n");
    t_world_reset();
    g_logCount = 0;
    t_put_and_publish(30, 5.0f, 40.0f);     /* near: audible */
    t_put_and_publish(31, 70.0f, 40.0f);    /* beyond his range: muted */

    /* user asks (Plugins > Conan Exiles menu) */
    ts3_audio_request_diag_snapshot();
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-SNAP[menu]") == 1, "menu request writes one AUDIO-SNAP header");
    CHECK(t_log_count("cid=30 'p' dist=5.0 m announced=40.0 m range=40.0 m gain=0.") == 1,
        "the near speaker is listed with distance, announced range, range and gain");
    CHECK(t_log_count("cid=31 'p' dist=70.0 m announced=40.0 m range=40.0 m gain=muted") == 1,
        "the far speaker is listed as muted");
    CHECK(ts3_audio_take_menu_snapshot_done() == 1 && ts3_audio_take_menu_snapshot_done() == 0,
        "the menu snapshot is reported done exactly once (chat feedback)");

    /* status line every 30 s, counting the CEPOS that arrived in between */
    win32_shim_advance_ms(31000);
    t_put_and_publish(30, 5.0f, 40.0f);
    t_put_and_publish(31, 70.0f, 40.0f);
    g_logCount = 0;
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-STAT: 31 s | cepos_rx=2 ") == 1 && t_log_count("speakers=2 audible=1 ") == 1,
        "AUDIO-STAT after 30 s: 2 positions received, 2 speakers, 1 audible");
    g_logCount = 0;
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-STAT") == 0, "no second status line within the interval");

    /* an audible speaker whose last position is older than 5 s */
    win32_shim_advance_ms(6000);
    ts3_audio_recompute_all_force();
    CHECK(t_log_count("client 30 is audible") == 1 && t_log_count("last position is 6.0 s old") == 1,
        "audible speaker with a 6 s old position -> warning (always on)");
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-SNAP[stale-audible]") == 1, "the warning asks for an automatic snapshot");

    /* the server's anti-flood refused commands */
    g_logCount = 0;
    ts3_audio_note_flood();
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-SNAP[flood]") == 0, "second automatic snapshot within 2 minutes is suppressed");
    win32_shim_advance_ms(121000);
    t_put_and_publish(30, 5.0f, 40.0f);
    t_put_and_publish(31, 70.0f, 40.0f);
    ts3_audio_note_flood();
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-SNAP[flood]") == 1, "after 2 minutes a flood writes a snapshot again");
    CHECK(t_log_count("flood_errors=2") == 1, "AUDIO-STAT counts the flood errors of the interval");

    /* a menu request is never suppressed */
    g_logCount = 0;
    ts3_audio_request_diag_snapshot();
    ts3_audio_diag_flush();
    CHECK(t_log_count("AUDIO-SNAP[menu]") == 1, "a user request always writes a block");

    /* voice of a client ID without state: silenced, counted, logged */
    g_logCount = 0;
    t_fill(g_buf, FRAMES, 2, 10000);
    ts3_audio_process_playback(0, g_buf, FRAMES, 2);
    ts3_audio_diag_flush();
    CHECK(t_log_count("voice blocks of a client ID without state were silenced") == 1,
        "silenced blocks of ID 0 are reported");
}

int main(void) {
    printf("audio PCM path host tests (084)\n");
    test_no_leak_after_generation_bump();
    test_gap_restart();
    test_mode_switch_continuity();
    test_cave_tail_not_replayed();
    test_busy_uses_cache();
    test_busy_without_cache();
    test_out_of_range_ids();
    test_unmute_flags();
    test_expired_entry_mutes();
    test_cave_slot_recycle();
    test_cave_reverb_strength();
    test_hrtf_gap_and_busy();
    test_once_per_pass();
    test_voice_cap();
    test_listen_bonus_live();
    test_misc();
    test_diagnostics();
    printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
