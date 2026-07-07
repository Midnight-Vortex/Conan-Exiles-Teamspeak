# Plan: Skalierung 200+ Spieler (Rewrite)

**Stand:** 2026-07-07  
**Repo:** `Conan-Exiles-Teamspeak` (Rewrite)  
**Bezug:** `REWRITE_PLAN.md` (Phasen 0–14 Funktions-Rewrite)

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

## Phase 4 — Nächste Optimierungen (empfohlen)

**Ziel:** Callback-CPU halbieren+, keine Regression bei normaler Spielsituation.

| # | Maßnahme | Datei | Erwarteter Effekt |
|---|----------|-------|-------------------|
| **4.1** | `recompute_all` nur bei Positions-/Voice-/Mode-Änderung **oder** max. 5–10 Hz throttle | `pos_file.c`, `ts3_entry.c`, `ts3_proximity_audio.c` | Größter Einzelhebel: ~33×/s → ~1–10×/s bei Stillstand |
| **4.2** | CEPOS-Empfang: Dirty-Flag pro Client, Recompute in CEDRAIN batchen (wie Unmute) | `ts3_cepos.c`, `ts3_entry.c` | ~200 recompute/s → gebündelt pro Drain-Zyklus |
| **4.3** | Writer-Lock-Scope verkleinern: Berechnung außerhalb, nur Publish unter Lock | `ts3_proximity_audio.c` | Weniger Audio-Thread-Staleness |
| **4.4** | `ts3d` Dedup-State bei Eviction/Disconnect invalidieren | `ts3_3d.c`, `player_table.c` | Keine stale 3D-Positionen |
| **4.5** | Metriken: Unmute-Backlog, Ring-Overflow, Eviction-Rate (throttled log) | `ts3_proximity_audio.c`, `player_table.c` | Lasttest-Diagnose |

**Test (4.x):** 20+ Spieler 30 min → kein Crash; Callback nicht dauerhaft >50 %; Unmute <500 ms; Eviction-Log selten.

---

## Phase 5 — Optional (Worst-Case 200 aktiv in Range)

| # | Maßnahme | Anmerkung |
|---|----------|-----------|
| **5.1** | 3D apply: max. N Updates pro Tick, Rest nächster Tick | Deckel ~500 API-Calls/s |
| **5.2** | `ts3_audio_reset`: nur belegte Client-IDs scannen (Bitmap/Ring) | Schnellerer Tab-Wechsel |
| **5.3** | Player-Table Hash statt O(512) linear (nur wenn Table weiter wächst) | Aktuell 512 ausreichend |
| **5.4** | Lasttest-Automatisierung: simulierter CEPOS-Flood + Pos-Poll | CI/manuell |

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
