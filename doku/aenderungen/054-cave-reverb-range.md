# 054 — Hoehlenmodus: Reichweite nicht mehr durch DRR gedrosselt

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/proximity/ts3_proximity_audio.c` | In `audio_compute_client`, Zweig `reverbZone`: DRR nur noch als `out->drr` (Wet/Dry), nicht mehr `gain *=` |
| `doku/aenderungen/054-cave-reverb-range.md` | Dieser Eintrag |

**Eine Funktion:** `audio_compute_client` — nur der `reverbZone`-Zweig.

---

## 2. Wie war es vorher?

Im Hoehlenmodus (Reverb-Zone, `RealisticAudio=0`) wurde die Lautstaerke zweimal
entfernungsabhaengig reduziert:

1. `gain = prox_volume_from_distance(...)` — normale Proximity-Reichweite
2. `gain *= prox_direct_reverb_ratio(...)` — DRR-Formel `1/(1+(d/ref)^2)` mit
   `ref = AudioMinDistance` (oft 0,5–1 m)

Bei wenigen Metern Entfernung sank `gain` damit auf 5–20 % — Stimmen wirkten
„zu nah“ abgeschnitten. Reverb aus (`RealisticAudio` oder Zone ohne Reverb)
stellte sofort die erwartete Reichweite wieder her.

Der RealisticAudio-Zweig macht das korrekt: DRR steuert nur `out->drr` (Diffuse-
Mix im PCM-Pfad), `gain` bleibt bei `prox_volume_from_distance` (plus optional
Richtungslautstaerke). Hoehlen-Hall laeuft ohnehin separat ueber Schroeder-Slots
(`reverbSlot` / `audio_apply_cave`).

---

## 3. Warum ist die neue Loesung besser/stabiler?

| Aspekt | Vorher | Jetzt |
|---|---|---|
| Reichweite in Reverb-Zonen | Doppelt gedrosselt (Volume + DRR auf Gain) | Nur `prox_volume_from_distance` |
| Hoehleneffekt | Hall + zu leise | Hall + LPF + DRR-Wet/Dry wie RealisticAudio |
| Paritaet RealisticAudio / Cave | Unterschiedliche Semantik fuer DRR | Gleiche Rolle: `drr` = Mix, nicht Gain |

DRR beschreibt das Verhaeltnis direkt/reflektiert — nicht die Horweite. Die
Proximity-Skalierung bleibt eine Aufgabe von `prox_volume_from_distance`.

---

## 4. Wie funktioniert es jetzt?

```
audio_compute_client (Callback-Thread)
        │
        ├─ gain ← prox_volume_from_distance (immer, ausser soundproof)
        │
        └─ reverbZone?
              ├─ cutoffHz ← prox_lowpass_cutoff_hz   (dumpfer Hoehlenklang)
              └─ (drr bleibt 1.0 — siehe 055; DRR-Mix macht Stimmen wieder leise)

ts3_audio_process_playback (Audio-Thread)
        Lowpass → prox_apply_diffuse_samples(drr) → Cave-Schroeder → Gain/Pan-Rampe
```

Reverb-Zonen behalten LPF (Muffling) und Diffuse-Mix; die Reichweite folgt wieder
dem Hub-`VoiceDistance` / `MaxVolume` wie ausserhalb der Zone.

---

## 5. Wie wurde es getestet?

- Kein dedizierter Unit-Test fuer `audio_compute_client` (TS-Callback-Kontext).
- Manuell im TS-Client: Reverb-Zone (Hoehlenmodus) betreten — Stimmen muessen
  dieselbe Hoer-Reichweite haben wie ohne Reverb; Echo/Hall und leicht dumpfer
  Klang (LPF) bleiben hoerbar.

---

## 6. Lerneffekt

Ein Kennwert darf nur **eine** Rolle haben: DRR als Wet/Dry-Mix und gleichzeitig
als Gain-Multiplikator erzeugt unbeabsichtigte Reichweiten-Bugs. Vor dem
Wiederverwenden einer Formel pruefen, ob der Ziel-Pfad sie schon anders interpretiert
(RealisticAudio vs. Cave).
