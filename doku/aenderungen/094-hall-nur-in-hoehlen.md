# 094 — Hall nur in Höhlen: Raum-Hall aus, `CaveReverb` einstellbar (Version 8.0.18)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/proximity/occlusion.h` | `OCCLUSION_REVERB_DEFAULT` 100 → **0**: der Raum-Hall (Occlusion v2, B4) ist standardmaessig aus |
| `src/core/hub/hub_parser.h/.c` | Neuer Root-Schluessel `CaveReverb` (0..200 %, Standard **70**) → Feld `caveReverb`; Doku-Kommentar der Schluessel |
| `src/ts/proximity/ts3_proximity_audio.c` | `audio_compute_client`: Hall-Staerke in einer `Reverb=True`-Zone = `CaveReverb / 100` (vorher fest 1,0); Hall-Slot nur bei Staerke > 0,01 |
| `src/core/proximity/audio_snap.h` | nur Kommentar |
| `tests/hub_parser_test.c`, `tests/audio_pcm_test.c` | neue Standardwerte, Test fuer `CaveReverb` (Parser, Skalierung, 0 = kein Hall, Zone ohne Reverb = kein Hall) |
| `src/ts/entry/ts3_entry.c` | Version `8.0.18` |

---

## 2. Wie war es vorher?

Spieler hoerten in **Haeusern und anderen Raeumen** einen Hall. Ursache war nicht die
Hoehlen-Zone, sondern der **Raum-Hall** aus Occlusion v2 (Aenderung 071/072, ab 8.0.11):
Die Mod meldet pro Sprecher einen Raum-Faktor, das Plugin machte daraus
`Hall = Raum × OcclusionReverb × 0,5`, und `OcclusionReverb` stand standardmaessig auf
**100 %**. Dazu war der Hall in Hoehlen-Zonen (`Reverb=True`) **fest** (1,0) und nicht
einstellbar. Vor 8.0.11 gab es Hall nur in diesen Zonen.

---

## 3. Warum ist die neue Loesung besser?

- Haeuser und Raeume halten nicht mehr: der Raum-Hall ist **aus, solange der Server ihn
  nicht ausdruecklich mit `OcclusionReverb=<Prozent>` einschaltet**. Hall gibt es damit
  wieder nur in den Hoehlen-Zonen (`Reverb=True` im Root-Channel).
- Die Hoehlen-Staerke ist jetzt ein eigener Regler: `CaveReverb` (Standard 70 %).
  100 = bisherige feste Staerke, 0 = kein Hall (der dumpfe Hoehlenklang bleibt).
- Die Occlusion-Daempfung durch Waende bleibt unveraendert; nur der Hall-Teil ist raus.

---

## 4. Wie funktioniert es?

```
Root-Channel-Beschreibung ([GLOBAL])
  CaveReverb=70        Hall-Staerke in Reverb=True-Zonen, 0..200 % (Standard 70)
  OcclusionReverb=0    Raum-Hall aus dem Raum-Faktor, 0..200 % (Standard 0 = aus)

audio_compute_client:
  Sprecher in Reverb=True-Zone  → reverbWet = CaveReverb / 100   (+ dumpfer Tiefpass wie bisher)
  sonst                         → reverbWet = Raum × OcclusionReverb/100 × 0,5   (Standard: 0)
  reverbWet <= 0,01             → kein Hall-Slot, kein Hall
```

**Wichtig fuer den Server-Admin:** steht in der Root-Beschreibung bereits eine Zeile
`OcclusionReverb=100` (oder ein anderer Wert > 0), bleibt der Raum-Hall an. Zeile loeschen
oder `OcclusionReverb=0` setzen. Die Mod wurde nicht geaendert (sie meldet seit Doku 076
fuer den Hall nur natuerliche Raeume).

---

## 5. Wie wurde es getestet?

- Host-Tests (`tests/run_tests.sh`, MSYS2-gcc): `hub_parser_test`, `audio_pcm_test` und alle
  anderen Suites gruen. Nur `player_table_test`, `render_state_test`, `proto_flush_test`
  lassen sich unter MinGW nicht kompilieren (Linux-Funktion `pthread_timedjoin_np`,
  SDK-Typen, `strncpy_s`-Konflikt im Test-Shim). `player_table_test` scheitert ohne meine
  Aenderungen genauso (geprueft); die Suites laufen im Linux-CI.
- Neue Pruefungen: Standard `OcclusionReverb=0` / `CaveReverb=70`, Klemmen (0..200),
  `CaveReverb=70` → Hall 0,70, `CaveReverb=0` → kein Hall-Slot, Zone ohne `Reverb=True` → kein Hall.
- `build.ps1 -SkipDeploy`: MSBuild exit 0, Paket `bin\conan_exiles-win64.ts3_plugin` mit
  `Version = 8.0.18`.
- **Nicht im Spiel gehoert.** Der Standardwert 70 % ist geschaetzt; bitte in einer Hoehle
  anhoeren und bei Bedarf `CaveReverb` im Root-Channel anpassen.

---

## 6. Lerneffekt

Zwei verschiedene Hall-Quellen (Zone vs. Raum-Faktor) brauchen zwei getrennte Regler und
einen vorsichtigen Standard: Ein neuer Effekt, der standardmaessig an ist, veraendert den
Klang auf allen Servern, ohne dass jemand ihn bestellt hat.
