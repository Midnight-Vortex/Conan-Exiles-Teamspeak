# 055 — Hoehlenmodus: DRR nicht als Diffuse-Mix

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/proximity/ts3_proximity_audio.c` | `reverbZone`-Zweig setzt `drr` nicht mehr; bleibt 1.0. LPF + Cave-Schroeder bleiben |
| `doku/aenderungen/055-cave-ohne-drr-mix.md` | Dieser Eintrag |

**Eine Funktion:** `audio_compute_client` — nur der `reverbZone`-Zweig.

---

## 2. Wie war es vorher (054)?

054 hat `gain *= DRR` entfernt und stattdessen `drr = prox_direct_reverb_ratio(...)`
gesetzt. Die PCM-Kette wendet bei `drr < 0.99` `prox_apply_diffuse_samples` an
(5 % Direkt + 95 % smeared). `AudioMinDistance` 0,5–1 m macht DRR nach wenigen
Metern ~0.05 — andere Stimmen bleiben **leise, nur wenn Reverb an ist**.

Ausserhalb der Zone bzw. `Reverb=False`: `drr=1.0`, kein Diffuse, normale Lautstaerke.

---

## 3. Warum ist die neue Loesung besser/stabiler?

DRR gehoert zum RealisticAudio-Wet/Dry, nicht zur Hoehle. Hoehlencharakter kommt
von LPF + Schroeder (`CAVE_DRY 0.92` / `CAVE_WET 0.26`). Reichweite bleibt
`prox_volume_from_distance`, ohne zweite Distanzdaempfung ueber Gain oder Diffuse.

---

## 4. Wie funktioniert es jetzt?

```
reverbZone (RealisticAudio aus)
  gain     = prox_volume_from_distance   (wie Open Field)
  cutoffHz = prox_lowpass_cutoff_hz      (dumpfer Hoehlenklang)
  drr      = 1.0                         (kein Diffuse)
  cave     = Schroeder wet               (Echo)
```

---

## 5. Wie wurde es getestet?

- Build + Deploy nach der Aenderung.
- Ingame: Reverb-Zone — andere muessen so laut sein wie ausserhalb; Echo bleibt.
- Reverb aus / ausserhalb: unveraendert.

---

## 6. Lerneffekt

Dieselbe Kennzahl an einen anderen Pfad zu haengen (Gain → Diffuse) reproduziert
denselben Bug. Cave-Echo und DRR-Mix sind zwei verschiedene Effekte.
