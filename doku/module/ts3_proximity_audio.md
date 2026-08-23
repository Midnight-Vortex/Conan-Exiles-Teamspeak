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
| Generation `g_snapGeneration` | Callback-Thread (`InterlockedIncrement`) | Audio-Thread (nur lesen) |
| `g_renderGeneration` (Last-Seen) | Audio-Thread | niemand sonst |
| Cave-Reverb-Slots `g_cave[].owner` | Callback-Thread (Acquire/Release unter Lock) | Audio-Thread (indexiert nur) |
| Unmute-Flags `g_pendingUnmute` u. a. | any-thread setzt (atomar), Callback flusht | — |
| Audio-Modus `g_audioMode` | any-thread (atomar), i. d. R. Callback | Audio-Thread |

**Harte Regeln (aus den alten Crashes gelernt):**

- Der **Audio-Thread** ruft NIE die TS-API und nimmt NIE einen Lock — nur
  Snapshots lesen, Rampe/Filter anwenden, atomare Flags setzen.
- Render-Rampen + LPF gehoeren dem Audio-Thread **exklusiv**. Der Callback-Thread
  signalisiert Invalidierung nur ueber `g_snapGeneration` (Generation-Counter,
  siehe `doku/aenderungen/004-pcm-besitz-generation-counter.md`).
- TS-Unmutes laufen **nur** als Batch auf dem Callback-Thread.

## Wichtigste Funktionen

- `ts3_audio_process_playback` — **Audio-Thread-Hotpath.** Liest den Snapshot,
  reinitialisiert bei Generation-Wechsel selbst seine Rampe/LPF, wendet dann
  Lowpass → Diffuse → Cave-Reverb → Gain/Pan-Rampe auf den Buffer an.
- `snap_publish` / `snap_read` — Seqlock: Schreiber (Callback) erhoeht `seq` vor
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
- `ts3_audio_flush_unmutes` — Callback-Thread: sammelt gesetzte Unmute-Flags und
  hebt die TS-Stummschaltung als **ein** API-Batch auf (rate-limitiert).
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
