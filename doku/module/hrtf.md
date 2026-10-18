# Modul: `core/proximity/hrtf`

Dateien: `src/core/proximity/hrtf.c` / `.h` · Tests: `tests/hrtf_test.c`

## Aufgabe

Rendert eine Mono-Stimme fuer **zwei Ohren** (binaural), so dass die Richtung des
Sprechers ueber Kopfhoerer hoerbar wird — inklusive vorne/hinten und oben/unten.
Modell: Brown & Duda, *A Structural Model for Binaural Sound Synthesis* (1998).

## Bausteine (pro Ohr)

| Baustein | Was er tut | Werte |
|---|---|---|
| Kopfschatten | 1-Pol/1-Null-Filter: Hoehen werden am abgewandten Ohr gedaempft, am zugewandten leicht angehoben | +6 dB (zum Ohr) … −20 dB (150°), Kopfradius 8,75 cm |
| ITD (Woodworth) | abgewandtes Ohr hoert spaeter | bis ~0,66 ms (≈ 32 Samples bei 48 kHz) |
| Ohrmuschel | 5 kurze Echos, Verzoegerung haengt von seitlichem Winkel und Polarwinkel ab | Tabelle aus dem Paper, auf 48 kHz skaliert |

Reihenfolge: Mono → Kopfschatten → Echos → ITD-Verzoegerung → Ohr.

## Funktionen

- `hrtf_listener_angles(yaw, pitch, dx, dy, dz, &az, &el)` — Richtung aus den
  Unreal-Werten der Mod. `dx/dy/dz` in Plugin-Koordinaten (y = Hoehe).
  Azimut 0 = vorne, +π/2 = rechts, ±π = hinten; Elevation +π/2 = oben.
- `hrtf_render(state, mono, n, rate, az, el, outL, outR)` — rendert einen Block;
  die Richtung gleitet vom letzten Block zum neuen (32-Sample-Schritte).
- `hrtf_process_interleaved_s16(state, samples, n, channels, li, ri, rate, az, el)` —
  fuer den Plugin-Puffer: Paar (li, ri) → Mono → HRTF → zurueck, Energie des
  Blocks bleibt gleich. Arbeitet in Stuecken von max. 256 Frames (Stack-Puffer).
- `hrtf_reset(state)` — Speicher loeschen (neuer Sprecher / nach Pause).

## Thread-Vertrag

Reine Funktionen auf einem vom Aufrufer gehaltenen `HrtfState`. Keine Globals
(ausser konstanten Tabellen), keine Locks, keine Allokation. Ein `HrtfState`
gehoert genau einem Thread — im Plugin dem TS-Audio-Thread
(`ts3_audio_process_postprocess`).

## Grenzen

- Generischer Kopf (keine persoenliche HRTF). Wirkt am besten mit Kopfhoerern.
- Keine Raum-/Schulter-Reflexion, kein Doppler.
- Nur fuer 2-Kanal-Ausgabe gedacht; Lautsprecher-Layouts nutzen weiter TS-3D.
