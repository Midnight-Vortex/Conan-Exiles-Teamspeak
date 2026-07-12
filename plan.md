# Plan: Skalierung 200+ Spieler (Rewrite)

**Stand:** 2026-07-12  
**Repo:** `Conan-Exiles-Teamspeak` (Rewrite)  
**Bezug:** `REWRITE_PLAN.md` (Phasen 0–14 Funktions-Rewrite)  
**Workflow:** `AGENTS.md` + `.cursor/rules/vibecoding-cost-efficient.mdc`

---

## Ziel

Das Plugin stabil und performant für Server mit **200+ verbundenen Spielern** betreiben — ohne Crashes, ohne Callback-Thread-Sättigung, ohne merkbare Proximity-Regression.

---

## Erledigt: Skalierungs-Phasen 1–3

| Phase | Inhalt | Status |
|-------|--------|--------|
| **1** | `PLAYER_TABLE_MAX` 64→512; CEPOS nur Wakeup (kein voller CEDRAIN pro Paket); sparse Unmute-Ring (512) statt 65535-Scan | ✅ |
| **2** | `ts3d_apply` 20 Hz; CEDRAIN early-return; `chan_has_pending_work`; Nick-Scan 256; Cave-Slots 32 | ✅ |
| **3** | Sparse Arrays 4096 (`TS3_MAX_CLIENT_ID`); spatial culling (Audio + 3D); Version-Panel Hash-Map (512); Client-ID-Validierung; Eviction-Metriken; PROX-TEST Bewegungssimulation | ✅ |

**Zusätzlich (Bugfixes):** Voice-Chat sofort via `ts3_request_wakeup_urgent`; Channel ignore `event`/`plot`; Root→Hub manuell.

---

## Crash-Risiko nach Phase 1–3: gering

| Bereich | Status |
|---------|--------|
| 65536-Arrays | Eliminiert → max. 4096 sparse + 512 Player-Table |
| TS-API nur Callback-Thread | Queue + CEDRAIN + Guards |
| Client-ID-Bounds | `ts3_client_id_valid()` in CEPOS / Audio / 3D |
| CEPOS pro Paket | Kein voller Drain, nur Wakeup |
| Audio-Thread | Lock-free Seqlock, kein TS-API |
| Unmute | Sparse Ring statt Vollscan |

**Kein offensichtlicher Crash-Pfad** bei 200 Spielern (Client-IDs < 4096).

---

## Verbleibende Risiken (Performance, nicht Crash)

### HIGH — Callback-Thread / Writer-Lock

| Risiko | Datei | Problem |
|--------|-------|---------|
| **`recompute_all` ~33×/s auch bei Stillstand** | `ts3_entry.c` → `ts3_on_local_position_update` → `ts3_audio_recompute_all` | Pos-Watcher ruft Update-Callback bei **jedem** gültigen Pos.txt-Poll (30 ms), nicht nur bei Bewegung. Bei 200 Table-Einträgen: ~6.600 Snapshot-Iterationen/s. |
| **Per-CEPOS `recompute_client` auf Callback-Thread** | `ts3_cepos.c` → `ts3_audio_recompute_client` | 200 Spieler @ 1 Hz Keepalive ≈ 200 Recomputes/s zusätzlich. |
| **Writer-Lock-Konvoi** | `ts3_proximity_audio.c` | `recompute_all` + `recompute_client` halten `g_writerLock` für volle Berechnung (Zonen, Cave-Slots). |

### MEDIUM — Funktionale Degradation

| Risiko | Datei | Problem |
|--------|-------|---------|
| **512-Player-Cap + LRU-Eviction** | `player_table.c` | >512 frische Speaker → älteste still entfernt, Proximity weg bis Re-Entry. |
| **Unmute max. 63/Zyklus** | `ts3_proximity_audio.c` | Massenweise gleichzeitig hörbar → TS-Mute kann 1–2 s nachhängen. |
| **Unmute-Ring 512, überschreibbar** | `ts3_proximity_audio.c` | Burst >512 vor Flush → Verzögerung, kein Crash (Flags bleiben). |
| **`ts3_audio_reset` O(4096)** | `ts3_proximity_audio.c` | Disconnect/Tab-Wechsel: kurzer Ruckler. |
| **Cave-Reverb 32 Slots** | `ts3_proximity_audio.c` | Viele Reverb-Zonen-Speaker gleichzeitig → Reverb fällt weg. |
| **3D-API-Last** | `ts3_3d.c` | 20 Hz × ~50 Nachbarn = bis ~1000 `set3DClient`/s; kein Cleanup bei Leave-Hear-Range. |

### LOW

| Risiko | Datei | Problem |
|--------|-------|---------|
| **Client-ID ≥ 4096 ignoriert** | `ts3_client_limits.h` | Selten auf echten Servern. |
| **Version-Map voll (512)** | `ts3_plugin_version.c` | Nur UI, kein Audio. |
| **CMD-Queue (256) / Chat-Queue (32)** | `ts3_adapter.c`, `util_base.c` | Drop + Log bei Überlauf. |
| **CEDRAIN Rate-Limit 30 ms** | `ts3_adapter.c` | Work verteilt sich auf mehrere Drain-Runden. |
| **Legacy UI-Arrays 512×4 ungenutzt** | `plugin_ui_compat.c` | ~tens of KB BSS, kein Hot-Path-Scan. |

---

## Realistische Einschätzung @ 200 Spieler

| Szenario | Einschätzung |
|----------|--------------|
| 200 connected, ~20–40 in Hörweiche | **Sollte laufen** |
| 200 alle aktiv in Shout-Range | Callback-Thread eng, Audio kann nachhinken |
| Schnelles Vorbeilaufen (Fly 25 m/s) | PROX-TEST OK (~1,3 s hörbar @ CPA 5 m, Normal 15 m) |

**PROX-TEST** (Boot-Log) prüft Lautstärke-Kurve + CEPOS-Timing (30 ms, 0,08 m eps), nicht volle 200-Client-Last.

---

## Phase 4 — Nächste Optimierungen (empfohlen) ✅ umgesetzt (7.0.1+)

**Ziel:** Callback-CPU halbieren+, keine Regression bei normaler Spielsituation.

| # | Maßnahme | Datei | Status |
|---|----------|-------|--------|
| **4.1** | `recompute_all` nur bei Positions-/Voice-/Mode-Änderung **oder** max. 10 Hz throttle | `pos_file.c`, `ts3_entry.c`, `ts3_proximity_audio.c` | ✅ |
| **4.2** | CEPOS-Empfang: Dirty-Flag pro Client, Recompute in CEDRAIN batchen | `ts3_cepos.c`, `ts3_entry.c` | ✅ |
| **4.3** | Writer-Lock-Scope verkleinern: Berechnung außerhalb, nur Publish unter Lock | `ts3_proximity_audio.c` | ✅ |
| **4.4** | `ts3d` Dedup-State bei Eviction/Disconnect invalidieren | `ts3_3d.c`, `player_table.c` | ✅ |
| **4.5** | Metriken: Unmute-Backlog, Ring-Overflow, Eviction-Rate (throttled log) | `ts3_proximity_audio.c`, `player_table.c` | ✅ |

**Zusätzlich:** Plugin-**Einstellungen**-Button (Extras → Erweiterungen) öffnet denselben Dialog wie **F10** (`ts3plugin_offersConfigure` / `ui_settings.c`).

**Test (4.x):** 20+ Spieler 30 min → kein Crash; Callback nicht dauerhaft >50 %; Unmute <500 ms; Eviction-Log selten.

---

## Phase 5 — Optional (Worst-Case 200 aktiv in Range)

**Stand Review 2026-07-12:** Phase 4 hat die **HIGH**-Risiken (`recompute_all`-Spam, CEPOS-Recompute pro Paket, Writer-Lock-Scope) bereits adressiert. Phase 5 bleibt für den **Worst-Case** (200 gleichzeitig in Shout-Range + Tab-Wechsel). Realistic Audio (Phase 6–8) belastet primär den **PCM-Pfad**, nicht den Callback-Thread.

| # | Maßnahme | Anmerkung | Review | Priorität |
|---|----------|-----------|--------|-----------|
| **5.1** | 3D apply: max. N Updates pro Tick, Rest nächster Tick | Deckel ~500 API-Calls/s | **Teilweise abgedeckt:** `ts3d_apply` 20 Hz + Hear-Range-Cull + Pos-Dedup (0,25 m). Worst-Case bleibt: 200 in Range, alle bewegen sich → bis ~4000 `set3DClient`/s theoretisch. Kein per-Tick-Cap. | **Mittel** — nur bei echtem 200-in-Range-Test |
| **5.2** | `ts3_audio_reset`: nur belegte Client-IDs scannen (Bitmap/Ring) | Schnellerer Tab-Wechsel | **Erledigt:** Scan nur `g_snap.valid`, Reverb-Slot, Pending-Unmute, Dirty-Flags + verwaiste Cave-Owner (~O(aktive) statt 4096× Lock). | **Hoch** — kleiner Diff, spürbarer Gewinn |
| **5.3** | Player-Table Hash statt O(512) linear | Nur wenn Table weiter wächst | **Nicht nötig:** 512 Slots, Lookup ~512 Iterationen — für Callback ok. | **Niedrig / zurückstellen** |
| **5.4** | Lasttest-Automatisierung: simulierter CEPOS-Flood + Pos-Poll | CI/manuell | **Noch offen:** `PROX-TEST` prüft Kurve/Timing, nicht 200-Client-Last. | **Mittel** — vor 5.1 sinnvoll (Baseline messen) |

### Ist-Zustand (Code-Referenz)

| Punkt | Datei | Heute |
|-------|-------|-------|
| 3D Rate + Cull | `ts3_3d.c` | `TS3D_APPLY_MIN_MS=50` (20 Hz), `ts3d_in_hear_range`, Dedup invalidiert bei Disconnect/Eviction (4.4) |
| 3D kein Leave-Range-Cleanup | `ts3_3d.c` | Out-of-range: kein `set3DClient`, Dedup-State bleibt — Position in TS kann veralten bis Re-Entry |
| Audio-Reset | `ts3_proximity_audio.c` | `ts3_audio_reset()` → nur Clients mit Snap/Reverb/Unmute/Dirty (~O(aktive)) |
| Player-Table | `player_table.c` | Linear O(512), LRU-Eviction mit Metrik |

### Empfohlene Reihenfolge

1. **5.4** — Lasttest-Skript / simulierter Flood → messen ob 5.1 überhaupt nötig ist  
2. **5.2** — Reset nur über `player_table_snapshot` + aktive Snap-Flags (größter UX-Gewinn)  
3. **5.1** — nur wenn Messung >~500 `set3DClient`/s sustained zeigt  
4. **5.3** — erst bei Table >512 oder messbarem Lookup-Problem

**Nicht in Phase 5:** Unmute-Batch (64/Zyklus), Pan-Glättung → siehe **Phase 9** (Audio-Qualität).

---

## Queue-Übersicht (Drop-Verhalten)

| Queue | Größe | Verhalten @200 |
|-------|-------|----------------|
| `ts3_cmd_queue` | 256 | Drop + Log (selten) |
| `ts3ChatQueue` | 32 | Drop (Voice-Chat) |
| `g_unmuteRing` | 512 | Überschreiben, Flags recover |
| `g_pendingUnmute` | 4096 flags | Nie dropped |
| CEDRAIN wakeup | 30 ms coalesce | Work über mehrere Zyklen |

---

## Memory (kein Crash, aber relevant)

- `g_cave[32]` mit Comb-Buffern: **~10+ MB** statisches BSS (`ts3_proximity_audio.c`)
- Sparse `g_snap[4096]` etc.: fest, unabhängig von Spielerzahl

---

## Prioritäten-Reihenfolge

1. **4.1** — `recompute_all` dedup/throttle *(sofort, kleiner Diff, großer Gewinn)*
2. **4.2** — CEPOS-Recompute batchen
3. **4.3** — Writer-Lock verkleinern
4. **4.4** — 3D stale cleanup
5. **4.5** — Metriken für Lasttest
6. Phase 5 nur bei Bedarf nach realem 200-Player-Test

---

## Abgrenzung

- **Nicht im Scope:** Server-`ts3server.ini`-Tuning, TS-Client-Bugs, Conan-Mod-Performance
- **Golden Rule bleibt:** TS-API nur Callback-Thread; Background → Queue + CEDRAIN

---

# Phase 6–9 — Realistic Audio (Mumble-Parität + Server-Toggle)

**Stand:** 2026-07-12 · **Plugin:** 7.0.3+ (Realistic + Binaural Stereo ✅)  
**Referenz:** `E:\programme\Conan-Exiles-Mumblee\plugin.c` (~3219–3659, ~4077–4137) — **lesen, nicht kopieren**  
**Kontext:** Spieler-Feedback („zu muffled“) vs. Mumble-Autor („zu klar“) — **dieselbe Design-Entscheidung von zwei Seiten**. Phase 4 hat Filter **nicht** entfernt; der Rewrite begrenzte Lowpass/DRR/Richtung auf Reverb-Zonen (REWRITE_PLAN Phase 10.3). **Phase 6–8 + Binaural (8.6) umgesetzt 2026-07-12.**

---

## Ziel

1. **Mumble-ähnlicher Klang** im Open World wiederherstellbar (Distanz-Lowpass, Diffuse/DRR, Richtungs-Psychoakustik).
2. **Server-Wahl** über Root-Channel-Beschreibung: Realistic **An/Aus** (+ optional Intensity 0–100).
3. **Phase-4-CPU-Fixes** unverändert lassen (Throttle, Dirty-Batch, Writer-Lock-Scope).
4. **Golden Rule:** eine Funktion pro Schritt; Rewrite aus Referenz, kein blindes `plugin.c`-Copy-Paste.

---

## Ist-Zustand vs. Mumble (Gap-Analyse)

| Feature | Mumble (`plugin.c`) | TS (aktuell) | Datei (TS) |
|---------|---------------------|--------------|------------|
| Lowpass nach Distanz | ✅ global | ✅ bei `RealisticAudio=True` | `proximity_math.c`, `ts3_proximity_audio.c` |
| Double-Pass-Lowpass | ✅ (2×, α×0.7) | ✅ | `ts3_proximity_audio.c` |
| DRR + Diffuse | ✅ | ✅ `prox_apply_diffuse_samples` | `proximity_math.c` |
| Richtung hinten (Filter) | ✅ Cut×0.75, DRR×0.85, −12 % | ✅ `prox_rear_psychoacoustics` | `proximity_math.c` |
| TRUE stereo / „leichtes HRTF“ | ✅ ~4090–4128 | ✅ **immer** im Proximity-Modus (`prox_binaural_stereo_gains`) | `proximity_math.c` |
| Hub-Toggle (schwere Filter) | ❌ | ✅ `RealisticAudio` / `FilterIntensity` | `hub_parser.c` |
| Humidity / echtes HRTF (HRIR) | ❌ in beiden Repos | ❌ nicht geplant | — |

**Realistic OFF:** klarer Klang (Bypass-Filter), aber **räumliches L/R wie Mumble**. **Realistic ON:** zusätzlich Lowpass + Diffuse + Richtungs-Filter.

---

## Hub-Konfiguration (Root-Channel)

Neue/verdrahtete Keys in `[GLOBAL]` der Root-Channel-Beschreibung:

```ini
[GLOBAL]
RealisticAudio=True
FilterIntensity=100
```

| Key | Typ | Default | Verhalten |
|-----|-----|---------|-----------|
| `RealisticAudio` | bool | `False` | `True` = Mumble-Pfad global (ingame proximity); `False` = aktuelles Verhalten (klar im Open Field) |
| `FilterIntensity` | 0–100 | `100` | Skaliert Cutoff/DRR/Richtung (100 = voller Mumble-Pfad; 0 = Bypass wie heute) |

**Alternative:** nur `FilterIntensity` (0 = aus, >0 = an mit Stärke) — bei Implementierung **eine** Variante wählen, nicht beide parallel.

**Legacy:** `hubAudioFilterIntensity` in `plugin.h` / `plugin_ui_compat.c` — entweder an `FilterIntensity` anbinden oder deprecate (nur intern, kein UI nötig).

---

## Ziel-Verhalten nach Umsetzung

| Modus | Open World | Reverb-Zone |
|-------|------------|-------------|
| **Realistic OFF** | Klar (Bypass 19 kHz) | Wie heute: Lowpass + Cave-Reverb + rear_duck |
| **Realistic ON** | Mumble-Pfad: Lowpass + Diffuse + Richtung, dann Gain/Pan | Cave-Reverb **zusätzlich** (Design: stapeln, nicht ersetzen — mit Autor abgleichen) |

**Signal-Kette (Realistic ON, wie Mumble ~3653–3662):**

```
Lowpass (ggf. double-pass) → Diffuse/DRR → Gain × directionVolume → Pan
→ (optional) Cave-Reverb wenn Reverb-Zone
```

**Thread-Regeln (unverändert):**

- Writer/CEDRAIN: `audio_compute_client` → Distanz, DRR, Cutoff, directionVolume, reverbSlot
- PCM-Callback: nur Snapshots + lock-free State — **kein TS-API, keine Locks**

---

## Phase 6 — Hub / Profil (S2)

| # | Funktion | Datei | Status |
|---|----------|-------|--------|
| **6.1** | `hub_parse_settings` — `RealisticAudio` / `FilterIntensity` parsen + clamp | `hub_parser.c`, `hub_parser.h` | ✅ |
| **6.2** | Felder in `HubSettings` + Defaults | `hub_parser.h` | ✅ |
| **6.3** | `server_profile_get_realistic_*()` Getter | `ts3_server_profile.c/.h` | ✅ |
| **6.4** | Debug-Log bei Profil-Reload (wie andere Hub-Keys) | `ts3_server_profile.c` | ✅ |

**Test:** Root-Beschreibung ändern → Log zeigt `RealisticAudio=1 FilterIntensity=100`; Getter liefert Werte. **Noch kein hörbarer Unterschied** (Audio-Pfad folgt in Phase 8).

**Off-limits:** `ui_main.c`, PCM-Pfad, `plan.md` außer Status-Update.

---

## Phase 7 — Mathe (S2, rein / rewrite)

| # | Funktion | Datei | Mumble-Ref | Status |
|---|----------|-------|------------|--------|
| **7.1** | `prox_apply_diffuse_samples` | `proximity_math.c/.h` | `applyDiffuseSimulation` ~3314 | ✅ |
| **7.2** | `prox_rear_psychoacoustics` | `proximity_math.c/.h` | ~3636–3651 | ✅ |

**7.1:** Direct/Diffuse-Mischung pro Sample (mono/stereo), `drr` 0.05–1.0, early-out bei `drr >= 0.99`.  
**7.2:** Input: `frontBack` (−1..1) → Output: `directionVolume`, `cutoffMul`, `drrMul`.

**Test:** Unit-nahe manuell (Debug-Build) oder Log in temporärem Harness — **kein** TS-Client nötig.

---

## Phase 8 — Audio-Pfad (S3)

| # | Funktion | Datei | Status |
|---|----------|-------|--------|
| **8.1** | `AudioSnap` + `AudioPublishParams` erweitern (`drr`, `directionVolume`, `realisticActive`) | `ts3_proximity_audio.c` | ✅ |
| **8.2** | `audio_compute_client` — bei Realistic ON: globale Cutoff/DRR/Richtung (nicht nur `reverbZone`) | `ts3_proximity_audio.c` | ✅ |
| **8.3** | `snap_publish` / `snap_read` — neue Felder | `ts3_proximity_audio.c` | ✅ |
| **8.4** | `audio_apply_lowpass` — optional Double-Pass (α×0.7 zweiter Pass) | `ts3_proximity_audio.c` | ✅ |
| **8.5** | `ts3_audio_process_playback` — Reihenfolge: LPF → Diffuse → Gain/Pan; Intensity skaliert | `ts3_proximity_audio.c` | ✅ |
| **8.6** | Mumble TRUE stereo — `prox_binaural_stereo_gains` (immer Proximity, nicht nur Realistic) | `proximity_math.c`, `ts3_proximity_audio.c` | ✅ |

**8.2 Details:**

- `cutoffHz = prox_lowpass_cutoff_hz(distance)` wenn Realistic ON
- `drr = prox_direct_reverb_ratio(distance, audioMinDistance)` — Referenz aus Hub/Zone wie Mumble `minDistance`
- `frontBack` aus Blickrichtung + Sprecher-Vektor (wie Mumble); `prox_rear_psychoacoustics` anwenden
- `directionVolume` in Gain einrechnen (Mumble: **nach** Filter, **vor** Pan-Multiplikation)
- Reverb-Zone: bestehende `cave_slot` + ggf. zusätzlicher Zone-Lowpass — **nicht** Realistic abschalten

**Test (hörbar):**

| Szenario | Erwartung |
|----------|-----------|
| Realistic OFF, Open Field | Klar wie 7.0.3 |
| Realistic ON, 5 m / 30 m | Näher heller, weiter dumpfer |
| Realistic ON, Sprecher hinten | Leise + dumpfer vs. vorne |
| Reverb-Zone + Realistic ON | Hall + Distanzfilter |
| Soundproof-Grenze | Hart stumm (unverändert) |
| 20 Spieler, 30 min | Kein Crash; Phase-4-Metriken normal |

---

## Phase 9 — Audio-Qualität (optional, nach 8.5)

Unabhängig von Realismus — bekannte Symptome (Knacks / zu leise nach Stunden):

| # | Maßnahme | Datei | Status |
|---|----------|-------|--------|
| **9.1** | Pan-Glättung (keine Sprünge Sample-zu-Sample) | `ts3_proximity_audio.c` | ✅ |
| **9.2** | Sanfte Mute-Grenze statt hartem `memset(0)` bei `TS3_AUDIBLE_GAIN` | `ts3_proximity_audio.c` | ✅ |
| **9.3** | Cave-Reverb: Buffer nicht mitten abbrechen | `ts3_proximity_audio.c` | ✅ |
| **9.4** | Unmute robuster (Rearm ~500 ms, Batch 128) | `ts3_proximity_audio.c` | ✅ |
| **9.5** | Pos/CEPOS `0,0,0` ablehnen (Lade-Placeholder) | `pos_file.c`, `ts3_cepos.c` | ✅ |

**Priorität:** 9.1 + 9.4 zuerst wenn Knacks/Leise gemeldet wird.

---

## Subagent-Workflow (AGENTS.md)

**Jeder Task-Prompt enthält:**

```
MANDATORY: follow .cursor/rules/vibecoding-cost-efficient.mdc
Repo: E:\programme\Conan-Exiles-Teamspeak
Golden Rule: only [one function], nothing extra, honor thread contract
Rewrite: read Conan-Exiles-Mumblee\plugin.c — rewrite, don't copy
Research: [files, thread, callers, risks]
Scope: only [file] — [function]
Off-limits: [list]
Build: yes/no
Output: changed files + manual TS test steps
```

| Schritt | Effort | Subagent | Model |
|---------|--------|----------|-------|
| 6.1–6.4 Hub | S2 | `generalPurpose` | `composer-2.5-fast` |
| 7.1–7.2 Mathe | S2 | `generalPurpose` | `composer-2.5-fast` |
| 8.1–8.6 PCM/Binaural | S3 | `generalPurpose` | `claude-sonnet-5-thinking-high` |
| Review nach 8.5 | S4 | `bugbot` | readonly |

**Reihenfolge:** 6 → 7 → 8 ✅ · 9 parallel · Phase 5 optional  
**Build/Deploy:** nur auf Anweisung (`build_msvc.ps1` → TS-Client neu starten).  
**Version:** Bump in `ts3_entry.c` nur wenn User „version“ sagt (Vorschlag: **7.1.0** Realistic/Binaural).

---

## Definition of Done (pro Funktion)

- [ ] Nur die eine Funktion im Scope geändert (Golden Rule)
- [ ] Kein TS-API im PCM-Callback; keine neuen Locks im Audio-Thread
- [ ] `ts3_client_id_valid()` / Bounds auf festen Arrays
- [ ] Build Release\|x64 OK (wenn gebaut)
- [ ] Manueller TS-Test beschrieben und durchgeführt
- [ ] Phase-4-Verhalten unverändert (Throttle, dirty reconcile, writer scope)

---

## Was wir **nicht** versprechen / nicht tun

| Punkt | Grund |
|-------|-------|
| Humidity / echtes HRTF (HRIR-Faltung) | Nie in Mumble; bewusst nicht geplant |
| Exakt identischer Klang ohne Autor-Review | TS vs. Mumble API, Sample-Rate, Client-Mixer |
| Blind-Copy aus `plugin.c` | Vibecoding Rewrite-Regel |
| `ui_settings.c` reaktivieren | F10/Extras = `ui_main.c` |
| Phase-4 rückgängig | Unabhängig vom Filter |
| `REWRITE_PLAN.md` Phase-10-Test „offenes Feld klar“ | Wird durch Hub-Toggle **ersetzt** — Doku-Update nur auf Anfrage |

---

## Collaborator / Kommunikation (Kurz)

**Kernbotschaft:** Spieler-Feedback → Klarheit im Open Field; nicht gegen seine Arbeit gemeint, schlecht kommuniziert. Mumble-Math ist noch da, globaler Pfad war im Rewrite eingeschränkt. Toggle im Root + sein Input zu Defaults.

**Technischer Satz für Mail:**  
*I checked the original Mumble source — distance low-pass, diffuse simulation, and rear attenuation are still in our math module but only applied in reverb zones in the rewrite. I'm restoring the full path as a root-channel toggle.*

**Vibecoding im Mail-Text:** nicht ironisch erwähnen — sachlich: AI für Implementation/Review, nicht als Ersatz für Design-Entscheidungen.

---

## Prioritäten-Reihenfolge (gesamt)

**Erledigt:** Phase 6–8 + Binaural 8.6 + Phase 9 (9.1–9.5) + **5.2** ✅

**Als Nächstes (wahlweise):**

1. **5.4** — Lasttest / CEPOS-Flood (Baseline vor 5.1)
2. **5.1** — 3D apply per-Tick-Cap (nur wenn Lasttest es zeigt)
3. **Version 7.1.0** — Realistic/Binaural/Phase 9 (auf Anfrage)
