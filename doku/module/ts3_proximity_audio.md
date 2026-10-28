# Modul: `ts/proximity/ts3_proximity_audio`

Dateien: `src/ts/proximity/ts3_proximity_audio.c` / `.h`

## Aufgabe

Der **Proximity-PCM-Pfad**: Er wendet Entfernungs-Lautstaerke, Stereo-Panorama,
Distanz-Lowpass und Hoehlen-Reverb auf die eingehenden Sprach-Samples an
(TeamSpeak-Callback `onEditPlaybackVoiceDataEvent`). Dazu gehoert die
Bruecke zwischen dem rechnenden **Callback-Thread** und dem abspielenden
**Audio-Thread**: lock-freie **Seqlock-Snapshots** (Ziel-Werte) sowie das
gebatchte **Unmute** von Clients ueber die TS-API.

## Thread-Vertrag

Dieses Modul lebt an der Grenze zweier Threads. Wer was darf:

| Zustand | Besitzer (einziger Schreiber) | Leser |
|---|---|---|
| Snapshots `g_snap` (Ziel: Gain/Pan/Cutoff/…) | Callback-Thread (`snap_publish` unter `g_writerLock`) | Audio-Thread (`snap_read`, lock-frei) |
| Render-Rampen `g_renderGain/PanL/PanR` | **Audio-Thread** | niemand sonst |
| Lowpass-Zustand `g_lpf` | **Audio-Thread** | niemand sonst |
| Letzter-Block-Zeitstempel `g_lastBlockMs`, Fresh-Flag `g_renderFresh` (084) | **Audio-Thread** | niemand sonst |
| Cache der letzten guten Zielwerte `g_targetCache` (084, fuer BUSY) | **Audio-Thread** | niemand sonst |
| Generation `g_snapGeneration` | Callback-Thread (`InterlockedIncrement`) | Audio-Thread (nur lesen) |
| `g_renderGeneration` (Last-Seen) | Audio-Thread | niemand sonst |
| Cave-Reverb-Slots `g_cave[].owner`, `acquireEpoch` | Callback-Thread (Acquire/Release unter Lock; **fasst die Puffer nicht an**) | Audio-Thread |
| Cave-Reverb-Puffer `g_cave[].dsp`, `seenEpoch` (084) | **Audio-Thread** (nullt den Slot selbst bei neuer Epoche / neuem Satz) | niemand sonst |
| Unmute-Flags `g_pendingUnmute` + Zaehler | any-thread setzt (atomar, auch der Audio-Thread), Callback scannt + flusht | — |
| Audio-Modus `g_audioMode` | any-thread (atomar), i. d. R. Callback | Audio-Thread |

**Harte Regeln (aus den alten Crashes gelernt):**

- Der **Audio-Thread** ruft NIE die TS-API und nimmt NIE einen Lock (auch keinen
  Spin-Lock, 084) — nur Snapshots lesen, Rampe/Filter anwenden, atomare Flags setzen.
- Render-Rampen + LPF gehoeren dem Audio-Thread **exklusiv**. Der Callback-Thread
  signalisiert Invalidierung nur ueber `g_snapGeneration` (Generation-Counter,
  siehe `doku/aenderungen/004-pcm-besitz-generation-counter.md`).
- TS-Unmutes laufen **nur** als Batch auf dem Callback-Thread.

### HRTF-Erweiterung (Aenderung 062)

| Zustand | Besitzer (einziger Schreiber) | Leser |
|---|---|---|
| Richtungs-Snapshot `g_dirSnap` (Azimut/Elevation) | Callback-Thread (`dir_publish` unter `g_writerLock`) | Audio-Thread (`dir_read`, lock-frei) |
| HRTF-Zustand `g_hrtf`, `g_hrtfGeneration`, `g_hrtfActive` | **Audio-Thread** (Post-Process) | niemand sonst |
| Diagnose `g_diag*` (Kanal-Layouts) | Audio-Thread (atomar, nur bei Aenderung) | Callback-Thread (`ts3_audio_diag_flush`, loggt) |

- `ts3_audio_process_postprocess` — **Audio-Thread**, aufgerufen von
  `onEditPostProcessVoiceDataEvent` (nach dem TS-3D). Bei `EnableHrtf=True` in der
  Root-Channel-Beschreibung (lock-frei ueber `server_profile_get_hrtf_enabled`, 063), genau
  2 Kanaelen und Proximity-Modus: Stereo-Paar → Mono → HRTF → zurueck, Lautstaerke
  bleibt gleich. Sonst nur Diagnose. Details: `doku/module/hrtf.md`.

### Review-Fixes 084 (Kurzfassung, Details: `doku/aenderungen/084-review-fixes-audio.md`)

Neue Header-only-Helfer (rein, auf dem Host getestet, kein Win32/TS):

| Header | Inhalt |
|---|---|
| `core/proximity/audio_snap.h` | `AudioSnap`/`AudioDirSnap`, Publish, **dreiwertiger Seqlock-Leser** (`OK`/`INVALID`/`BUSY`) mit Compiler-Barriere, `audio_snap_find_orphans` (Ablauf-Sweep) |
| `core/proximity/audio_fx.h` | Einpoliger Tiefpass `audio_lpf_apply`, Cave-Hall `CaveDsp` (eigene L/R-Positionen) |
| `core/proximity/voice_cap.h` | `voice_distance_cap` / `voice_distance_clamp`: Obergrenze fuer die vom Peer gemeldete Stimm-Reichweite |
| `ts3_proximity_audio.h` | `render_gap_exceeded` (Talk-Pause erkennen), `render_ramp_start` (Fresh-Start) |

Ablauf pro Block (`ts3_audio_process_playback`):

```text
ID ausserhalb des Arrays?  -> im MUTE-Modus nullen, sonst unberuehrt (084 #10)
Luecke > 200 ms ODER Generation gebumpt?  -> g_renderFresh = 1, LPF/Cache zuruecksetzen
Modus PASSTHROUGH: Gain bleibt 1.0, Fresh weg (Kontinuitaet beim Wechsel)
Modus MUTE / deaf: nullen
Snapshot lesen:  INVALID -> stumm, Zustand zuruecksetzen
                 BUSY    -> letzte guten Zielwerte (oder 1 Block stumm), Zustand unangetastet
                 OK      -> Cache aktualisieren
Tiefpass -> Diffuse -> Cave-Hall (Slot-Epoche/Fresh -> Audio-Thread nullt) -> Rampe
(Fresh: Rampe startet am Ziel statt bei einem alten Wert)
```

## Wichtigste Funktionen

- `ts3_audio_process_playback` — **Audio-Thread-Hotpath.** Liest den Snapshot,
  reinitialisiert bei Generation-Wechsel selbst seine Rampe/LPF, wendet dann
  Lowpass → Diffuse → Cave-Reverb → Gain/Pan-Rampe auf den Buffer an.
- `snap_publish` / `audio_snap_read` (Header `audio_snap.h`) — Seqlock: Schreiber (Callback) erhoeht `seq` vor
  und nach dem Schreiben; Leser (Audio) liest bei ungerader/geaenderter `seq`
  einfach neu. So wartet niemand.
- `audio_compute_client` / `audio_publish_client` — Callback-Seite: Gain/Pan/
  Filter aus Position + Zone rechnen (ausserhalb des Locks) und unter
  `g_writerLock` publizieren (Phase-4.3-Muster).
- `ts3_audio_recompute_all_force` / `_flush_recomputes` / `_mark_client_dirty` —
  Recompute-Steuerung: UI/CEPOS markieren „dirty“ + Wakeup, der Callback-Thread
  rechnet spaeter im CEDRAIN-Zyklus. Der Dirty-Pfad hat ein **Budget** von
  `TS3_RECOMPUTE_DRAIN_BUDGET` (64) Clients pro Drain und fordert bei Rest einen
  erneuten Wakeup an (V8.4, `doku/aenderungen/011`); `recompute_all` bleibt ein
  tabellen-begrenzter Durchlauf.
- `ts3_audio_flush_unmutes` — Callback-Thread: **scannt** die gesetzten Unmute-Flags
  (kein Ring und kein Spin-Lock mehr, 084 #1) und hebt die TS-Stummschaltung als
  **ein** API-Batch auf (rate-limitiert, max. 63 Clients).
- `AudioPass` / `audio_pass_begin` — Callback-Thread: holt Position, Hub-Profil und
  Profil-Skalare **einmal pro Durchlauf** (Recompute-All und Dirty-Drain, 084 #8);
  `audio_compute_client(pass, entry, out)` bekommt den Tabelleneintrag uebergeben.
- Recompute-All zieht zusaetzlich **abgelaufene** Spieler-Eintraege (kein CEPOS seit
  `PLAYER_TABLE_STALE_MS`) zurueck: gueltiger Snapshot ohne frischen Spieler -> neutraler,
  ungueltiger Snapshot -> der PCM-Pfad schaltet stumm (084 #2).
- `ts3_audio_invalidate_client` / `ts3_audio_reset` — Callback-Thread:
  veroeffentlichen neutrale Snapshots und **bumpen die Generation** (schreiben
  die Render-Rampen nicht mehr selbst). `ts3_audio_reset` behaelt den
  O(aktive)-Scan aus Phase 5.2.
- `render_state_needs_reinit` (Header, `static inline`) — reine Entscheidung
  „Generation geaendert?“; host-unit-getestet in `tests/render_state_test.c`.

## Hub-Key `FilterIntensity` — Stand geprueft 2026-08-23

Aeltere Notizen (u. a. `vergleichs.md`, Projektregeln) behaupten, der Hub-Key
sei zwar geparst, aber **nicht** mit dem Audio-Pfad verbunden. Das stimmt so
**nicht mehr**. Der Weg ist vollstaendig:

```text
[GLOBAL] FilterIntensity=…   (Root-Kanalbeschreibung)
  → hub_parser.c            → HubSettings.filterIntensity
  → ts3_server_profile.c    → server_profile_apply()
  → server_profile_get_filter_intensity()
  → ts3_proximity_audio.c   → filterIntensity (0…1) blendet Lowpass-Cutoff,
                              Direkt-/Reverb-Verhaeltnis und Richtungs-
                              lautstaerke zwischen „aus“ und „voll“
```

Bei `RealisticAudio=0` oder `FilterIntensity=0` bleibt der realistische Zweig
inaktiv — das ist gewollt, kein fehlender Anschluss.

**Was wirklich tot ist:** die Legacy-Globale `hubAudioFilterIntensity`
(`src/plugin.h`, `src/ui/plugin_ui_compat.c`). Sie wird nirgends geschrieben und
nirgends gelesen; `hub_parser.c` kennt den Namen nur noch als **Alias** beim
Parsen. Wer den Audio-Pfad sucht, darf sich davon nicht in die Irre fuehren
lassen — die Globale ist ein Ueberbleibsel, kein Schalter.
