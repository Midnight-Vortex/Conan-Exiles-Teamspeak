# 062 — HRTF: echter 3D-Klang fuer Kopfhoerer (+ Diagnose der Kanal-Layouts)

**Branch:** `feature/hrtf` · **Schalter:** `EnableHrtf` in `plugin.cfg` (Standard: aus)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/proximity/hrtf.h/.c` | **Neu.** Reines Mathe-Modul: generisches HRTF nach Brown & Duda (1998) — Kopfschatten-Filter, Laufzeitunterschied (ITD), fuenf Ohrmuschel-Echos. Dazu die Richtungsberechnung aus Unreal-Yaw/Pitch und ein Helfer fuer 16-Bit-Stereo-Puffer. |
| `src/ts/proximity/ts3_proximity_audio.c/.h` | Eigener Seqlock-Snapshot `g_dirSnap` (Richtung pro Sprecher), Richtung wird in `audio_compute_client` berechnet und in `audio_publish_client` / `ts3_audio_invalidate_client` veroeffentlicht. **Neu:** `ts3_audio_process_postprocess` (HRTF im Post-Process-Schritt) und `ts3_audio_diag_flush` (Schritt-0-Diagnose). |
| `src/ts/entry/ts3_entry.c`, `ts3_exports.h` | Neuer TS-Export `ts3plugin_onEditPostProcessVoiceDataEvent`; `ts3_audio_diag_flush()` im CEDRAIN-Zyklus. |
| `src/core/config/config.h/.c` | Neuer Schluessel `EnableHrtf=true/false` (Standard `false`), wird geladen, gespeichert und im Lade-Log angezeigt. |
| `tests/hrtf_test.c`, `tests/run_tests.sh` | Neue Test-Suite (22 Checks). |
| `project/Conan-Exiles-TeamSpeak.vcxproj`, `build/build_mingw.sh` | `hrtf.c/.h` eingetragen. |
| `doku/module/hrtf.md`, `doku/module/ts3_proximity_audio.md`, `doku/03-build-und-tests.md` | Doku. |

## 2. Wie war es vorher?

- Die **Richtung** einer Stimme kam aus dem eingebauten 3D von TeamSpeak
  (`ts3d_set_listener` + Positionen pro Client, Rolloff fest 1,0). Das ist ein
  einfaches Links/Rechts-Panorama: vorne und hinten, oben und unten klingen gleich.
- Das plugin-eigene Panorama im Playback-Callback (`prox_binaural_stereo_gains`,
  "Mumble TRUE stereo") wirkt nur, wenn TeamSpeak dort **Stereo** liefert. Bei
  normalen Sprach-Codecs ist der Playback-Puffer sehr wahrscheinlich **Mono** —
  dann laeuft dieser Zweig gar nicht (`channels == 1` → nur Lautstaerke).
- Ob Mono oder Stereo ankommt, war nie gemessen.

## 3. Warum ist die neue Loesung besser?

- **HRTF** (Head-Related Transfer Function = "wie der Kopf den Schall veraendert,
  bevor er im Ohr ankommt") liefert die Hinweise, die das Gehirn wirklich benutzt:
  - abgewandtes Ohr leiser und dumpfer (Kopfschatten, bis −20 dB in den Hoehen),
  - abgewandtes Ohr hoert minimal spaeter (bis ~0,66 ms),
  - Ohrmuschel-Echos → vorne/hinten und oben/unten werden unterscheidbar.
- Es sitzt im **Post-Process-Schritt**: dort liefert TeamSpeak immer die echten
  Ausgabekanaele (Kopfhoerer: `SPEAKER_HEADPHONES_LEFT/RIGHT`), also funktioniert
  es auch, wenn die Stimme selbst Mono ist.
- **Lautstaerke bleibt gleich:** pro Block wird die Energie (L² + R²) vor und nach
  dem HRTF verglichen und angeglichen. Die Entfernungs-Lautstaerke aus Phase 6
  bleibt damit exakt erhalten.
- **Rein optional:** Standard aus. Ohne `EnableHrtf=true` laeuft alles wie bisher
  (nur die Diagnose zaehlt mit).
- **Rewrite statt Kopie:** Das Mumble-Plugin (Commit 104f0b0) hat dasselbe Modell,
  aber seine Hilfsdatei `acoustic_model.h` liegt nicht im Repo. Hier ist alles
  direkt aus dem Paper neu geschrieben, in eigener Struktur und mit Tests.

## 4. Wie funktioniert es jetzt?

```
Callback-Thread (CEPOS / Pos-Watcher)            Audio-Thread (TS)
-------------------------------------            ------------------------------------------
audio_compute_client                             onEditPlaybackVoiceDataEvent
  Distanz, Zone, Gain ... (wie bisher)             -> Gain / LPF / Hoehlenhall (wie bisher)
  hrtf_listener_angles(yaw, pitch, Abstand)      TeamSpeak 3D (Panorama)
audio_publish_client (unter g_writerLock)        onEditPostProcessVoiceDataEvent
  snap_publish(...)            (wie bisher)        -> ts3_audio_process_postprocess
  dir_publish(Azimut, Elevation) --seqlock-->         EnableHrtf? 2 Kanaele? Proximity?
                                                      dir_read (lock-frei)
                                                      L+R -> Mono -> HRTF -> L/R
                                                      Energie angleichen
```

- **Richtung:** `hrtf_listener_angles` rechnet direkt mit den Unreal-Werten der Mod
  (YAW = Blickrichtung, YAWY = Neigung, Y = Hoehe) — unabhaengig davon, wie das
  TS-3D seine Achsen sieht. Azimut 0 = vorne, +90° = rechts; Elevation +90° = oben.
- **Zustand pro Sprecher** (`g_hrtf[id]`, Filter- und Echo-Speicher) gehoert nur dem
  Audio-Thread — wie `g_lpf`. Er setzt ihn selbst zurueck, wenn der
  Generation-Zaehler (Aenderung 004) sich geaendert hat oder der Sprecher im
  letzten Block nicht gerendert wurde.
- **Richtungswechsel** werden innerhalb eines Blocks in 32-Sample-Schritten
  ueberblendet, Verzoegerungen laufen pro Sample linear mit → keine Klicks beim Drehen.
- **Nur bei 2 Kanaelen** (Stereo/Kopfhoerer). Bei 4.0/5.1-Lautsprechern bleibt das
  TS-3D unveraendert — HRTF ist fuer Kopfhoerer gedacht.

### Schritt 0: Diagnose

Der Audio-Thread merkt sich (atomar, nur bei Aenderung) die Kanalzahl im
Playback-Callback und Kanalzahl/Lautsprecher/Fuell-Maske im Post-Process-Callback.
Der Callback-Thread schreibt bei jeder Aenderung **eine** Zeile ins Log:

```
AUDIO-DIAG: playback channels=1 | postprocess channels=2 speakers=0x30000000 fill=0x3 | hrtf=0
```

`speakers=0x30000000` = Kopfhoerer links + rechts, `0x3` = Front links + rechts.
Damit ist im echten Client sichtbar, ob die Stimme Mono ankommt (dann war das alte
Plugin-Panorama wirkungslos) und welches Ausgabe-Layout TeamSpeak benutzt.

## 5. Wie wurde es getestet?

- `tests/hrtf_test.c` — 22 Checks, alle gruen (lokal mit MSVC uebersetzt, siehe Kopf
  der Testdatei): Winkel aus Yaw/Pitch, Quelle rechts → rechtes Ohr +16 dB und
  32 Samples frueher, links = Spiegel von rechts, vorne beide Ohren gleich,
  vorne ≠ hinten, keine NaN/Inf auch bei NaN/1e9-Winkeln, Reset, Lautstaerke
  bleibt (Verhaeltnis 1,000), fremde Kanaele unberuehrt, Mono/ungueltig → Puffer
  unveraendert.
- Der Test fand einen echten Fehler: die erste Winkel-Normierung lief bei sehr
  grossen Werten endlos (`a - 2π == a` in float) → jetzt `fmodf`.
- MSVC Release-Build OK, Export `ts3plugin_onEditPostProcessVoiceDataEvent` in der
  DLL vorhanden (`dumpbin /exports`), `tests/check_layering.sh` OK.
- **Offen — TS-Client-Test durch einen Menschen** (mit Kopfhoerern, zwei Clients):
  1. Plugin bauen/deployen, TS neu starten, Log pruefen: `AUDIO-DIAG`-Zeile notieren.
  2. `EnableHrtf=true` in `Dokumente\Conan Exiles TeamSpeak plugin\plugin.cfg`, TS neu starten.
  3. Mitspieler laeuft im Kreis um dich: rechts/links/hinten muessen klar
     unterscheidbar sein, Lautstaerke darf sich beim Drehen kaum aendern, kein Knacken.
  4. Gegenprobe mit `EnableHrtf=false`.

## 6. Lerneffekt

- Vor einem Audio-Umbau erst messen, **was** im Callback wirklich ankommt — ein
  Stereo-Effekt auf einem Mono-Puffer tut nichts, auch wenn der Code richtig aussieht.
- Winkel-Normierung nie mit `while (a > π) a -= 2π` — bei grossen Werten haengt das
  den Audio-Thread auf. `fmodf` ist begrenzt.
