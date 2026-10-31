# 096 — Raum-Hall wieder Standard 100: Höhlen hallen auch ohne Zone (Version 8.0.20)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/proximity/occlusion.h` | `OCCLUSION_REVERB_DEFAULT` wieder **100** (094 hatte 0) |
| `src/core/hub/hub_parser.h` | Kommentare zu `OcclusionReverb` (Standard 100, braucht `EnableOcclusion=True`) |
| `src/ts/proximity/ts3_proximity_audio.c` | nur Kommentar in `audio_compute_client` |
| `tests/hub_parser_test.c` | Standardwert `OcclusionReverb` wieder 100 |
| `src/ts/entry/ts3_entry.c` | Version `8.0.20` |

`CaveReverb` aus 094 bleibt (Zonen mit `Reverb=True`, Standard 70).

---

## 2. Wie war es vorher?

094 hatte den Raum-Hall (`OcclusionReverb`) auf Standard 0 gestellt, damit Haeuser nicht
hallen. Dadurch gab es **keinen Hall mehr in Hoehlen ohne Zone**. Der Besitzer will aber
Hall in den normalen Conan-Hoehlen, ohne dass dafuer eine Zone eingerichtet wird.

---

## 3. Warum ist die neue Loesung besser?

Es gibt zwei getrennte Hall-Quellen, jetzt mit passenden Standards:

| Sprecher steht … | Hall | Regler |
|---|---|---|
| in einer Zone mit `Reverb=True` | `CaveReverb / 100` (Standard 0,70) | `CaveReverb` |
| ausserhalb einer Zone | Raum-Faktor der Mod × `OcclusionReverb / 100` × 0,5 | `OcclusionReverb` (Standard 100, 0 = aus) |

Natuerliche Hoehlen hallen also ueber den Raum-Faktor der Mod, ohne Zone.

---

## 4. Wie funktioniert es?

Voraussetzungen fuer den Hall ohne Zone:

1. Root-Channel: `EnableOcclusion=True` (der Raum-Faktor kommt aus dem Okklusions-Pfad).
2. Die Mod (`Vo_PosStreamer`) meldet im OCC-Eintrag das Feld `raum` (0..100).
3. `OcclusionReverb` > 0 (Standard 100).

**Haeuser:** Ob ein gebautes Haus hallt, entscheidet allein die Mod: laut Doku 076 meldet
sie fuer gebaute Raeume `raum = 0` und fuer natuerliche Raeume einen Wert nach Groesse.
Das Plugin kann Haus und Hoehle nicht selbst unterscheiden. Hallt ein Haus trotzdem, meldet
die Mod dort einen Raum-Wert > 0 (Mod-Erkennung oder aeltere Mod-Version auf dem Server).
Notbehelf ohne Mod-Aenderung: `OcclusionReverb=0` im Root-Channel (dann auch keine Hoehle
ohne Zone mit Hall; Zonen mit `Reverb=True` hallen weiter mit `CaveReverb`).

---

## 5. Wie wurde es getestet?

- Host-Tests (`tests/run_tests.sh`, MSYS2-gcc): alle Suites gruen ausser den drei, die unter
  MinGW nicht kompilieren (`player_table_test`, `render_state_test`, `proto_flush_test`,
  siehe 094; laufen im Linux-CI).
- `build.ps1 -SkipDeploy`: MSBuild exit 0, `bin\conan_exiles-win64.ts3_plugin` mit `Version = 8.0.20`.
- **Nicht im Spiel gehoert** und die **Mod nicht geprueft**: ConanMCP (Port 8123) war nicht
  erreichbar, `ensure_devkit.ps1` brachte ihn nicht hoch; es wurde nichts neu gestartet.

---

## 6. Lerneffekt

Ein Standardwert, der einen Effekt ausschaltet, nimmt ihn auch dort weg, wo er gewollt ist.
Wenn zwei Effekte aus derselben Quelle kommen (Raum-Faktor), muss die Unterscheidung
„Haus / Hoehle“ dort fallen, wo die Information liegt: in der Mod.
