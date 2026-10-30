# Build und Tests (V8)

Seit V8.0 gibt es drei Wege, das Projekt zu bauen bzw. zu pruefen. Nur der
erste erzeugt die DLL, die wirklich ausgeliefert wird — die anderen beiden
sind das **Sicherheitsnetz**, damit Aenderungen nicht blind passieren.

| Weg | Wo | Wofuer |
|---|---|---|
| MSVC-Build | Windows-PC | Release-DLL fuer den TS-Client (unveraendert wie V7) |
| MinGW-Cross-Build | Linux (jeder Rechner/CI) | Gate: "kompiliert + linkt die komplette DLL?" |
| gcc-Unit-Tests | Linux (jeder Rechner/CI) | Gate: "stimmt die Logik der puren Kernmodule?" |

## 1. Windows: Release-Build mit MSVC (unveraendert)

```powershell
powershell -File build\build_msvc.ps1
# oder direkt: .\build.ps1
# Deploy: kopiert die DLL nach %AppData%\TS3Client\plugins\conan_exiles.dll
# Paket:  schreibt bin\conan_exiles-win64.ts3_plugin (ohne Version im Dateinamen)
```

Baut `project/Conan-Exiles-TeamSpeak.vcxproj` (Release|x64) mit Visual
Studio. Nach jedem erfolgreichen Rebuild liegt zusätzlich ein
**`.ts3_plugin`** in `bin\` — ein ZIP mit `package.ini` und
`plugins/conan_exiles.dll` (Forward-Slash im ZIP, Änderung 049). Der Build
**startet den Package Installer nicht** (Änderung 048).

Endnutzer: `.ts3_plugin` doppelklicken, **TeamSpeak vorher beenden**
(Tray → Quit). Entwickler: `.\build.ps1` kopiert die DLL direkt.
`-OpenPackage` startet den Installer nur zusammen mit `-SkipDeploy`.
`-SkipPackage` unterdrückt nur das Paket, nicht den DLL-Build.

## 2. Linux: Cross-Build mit MinGW

```bash
bash build/build_mingw.sh
# Erfolg: "OK: built bin/mingw/conan_exiles.dll (... bytes)"
```

**Was passiert:** Das Skript kompiliert exakt dieselben 28 `.c`-Dateien wie
die `.vcxproj` (Liste steht im Skript) mit `x86_64-w64-mingw32-gcc`, einem
GCC, der Windows-DLLs erzeugt. Auch die Ressourcen (`project/Resource.rc`,
Bitmaps/Icons) werden per `windres` eingebunden. Ergebnis ist eine echte
PE-DLL mit allen `ts3plugin_*`-Exporten.

**Wichtig zu wissen:**

- Das ist ein **Build-Gate, kein Release-Build**. Die DLL aus `bin/mingw/`
  wird nicht ausgeliefert — sie beweist nur, dass der Code vollstaendig
  kompiliert und linkt.
- Warnungen sind erlaubt (V7-Code hat bekannte Warnungen), Fehler nicht:
  das Skript bricht beim ersten Fehler ab (`set -euo pipefail`).
- Benoetigt: `gcc-mingw-w64-x86-64` (Debian/Ubuntu-Paketname).
- `Resource.rc` liegt als UTF-16 im Repo (Visual-Studio-Standard); das
  Skript erzeugt sich eine Build-lokale UTF-8-Kopie, die Repo-Datei bleibt
  unangetastet.

## 3. Linux: Unit-Tests mit gcc

```bash
bash tests/run_tests.sh
# Erfolg: "RESULT: ALL SUITES PASSED" (Exit-Code 0)
```

**Was passiert:** Fuenf Test-Suiten werden mit normalem Host-gcc gebaut und
ausgefuehrt — ganz ohne TS-SDK und ohne Windows:

| Suite | Modul | Checks | Prueft |
|---|---|---:|---|
| `hub_parser_test` | `src/core/hub/hub_parser.c` | 97 | Parsen der Server-Beschreibung ([GLOBAL]/[ZONES]/[RACE]/[DEFAULT_SETTINGS]), Defaults, Clamping, kaputte Eingaben, Limits |
| `proximity_math_test` | `src/core/proximity/proximity_math.c` | 59 | Distanz, Lautstaerke-Kurve (Monotonie, Soft-Tail, Randfaelle), Equal-Power-Pan, Lowpass, DRR, Rear-Psycho, Binaural-Gains, Hub-Helfer, Diffuse-PCM |
| `zone_resolve_test` | `src/core/proximity/zone_resolve.c` | 46 | Punkt-in-Zone (innen/aussen/Kante/Ecke), Hoehenband ±1m, Skalen-Sonden, UE-Layout, Soundproof-Einbahnregel, Reverb, Ueberlappung |
| `player_table_test` | `src/core/proximity/player_table.c` | 30 | put/get, Namens-Abschneidung, Snapshot, 120-s-Verfall, LRU-Verdraengung bei voller Tabelle |
| `render_state_test` | `ts3_proximity_audio.h` (inline) | 9 | PCM-Generation-Counter: wann Render-State neu initialisiert wird |
| `hrtf_test` | `src/core/proximity/hrtf.c` | 22 | Richtung aus Yaw/Pitch, Pegel- und Laufzeitunterschied, Symmetrie, vorne≠hinten, NaN-Sicherheit, Lautstaerke-Erhalt, fremde Kanaele unberuehrt (Aenderung 062; laeuft auch mit MSVC, Befehl im Testkopf) |

**Review-Fixes 2026-10-28 (Aenderungen 081–088)** — neue Suiten, alle in `tests/run_tests.sh`:

| Suite | Modul | Prueft |
|---|---|---|
| `adapter_lifecycle_test` | `ts3_version_map.h`, `ts3_return_code.h` | CEVER-Tabelle mit Tombstones (Zufallslauf gegen Referenzmodell), Move-Return-Codes (082) |
| `wakeup_policy_test` (erweitert) | `drain_target.h` | CEDRAIN an den eigenen Client, Echo-Pruefung, Fallback nach 2 s, Fremd-Filter (082) |
| `cepos_wire_test` | `ts3_cepos_wire.h` | Steuerzeichen im empfangenen Namen (083) |
| `proto_flush_test` | echte `ts3_cemode/ceping/cepos/3d.c` gegen Stubs | CEMODE-Flanke, CEPING 1 Hz + Verlust-Zaehlung, CEPOS-Leerlauf, 3D-Backoff/Deckel (083) |
| `audio_fx_test` | `audio_fx.h`, `audio_snap.h`, `voice_cap.h` | Hall bit-identisch zum alten Code, Lowpass, Seqlock OK/INVALID/BUSY mit Stress-Thread (084) |
| `audio_pcm_test` | echte `ts3_proximity_audio.c` auf dem Host | PCM-Pfad, Neustart nach Pause, Verfall → stumm, Unmute ohne Spin-Lock (084) |
| `voice_cap_test` | `voice_cap.h` + echte `voice_modes.c` | Kein legaler Wert ueberschreitet die Empfangs-Obergrenze (084) |
| `http_request_test` | `http_request.h` | Browser-Schutz (Origin/Host/Sec-Fetch-Site), Content-Length, Drossel, Backoff (085) |
| `pos_parse_test` | `pos_parse.h` | Pos.txt-/HTTP-Zeilen, leere/nicht-endliche/absurde Werte (085) |
| `config_text_test` | `config_text.h` | `plugin.cfg` parsen, NaN/Inf, Dezimal-Komma, UTF-8/ANSI (085) |
| `chan_move_backoff_test` | `chan_move_backoff.h` | 5/10/30/60-s-Wiederholplan nach abgelehntem Move (086) |
| `channel_manage_test` | echte `channel_manage.c` gegen Stubs | Move-Fehler, Cooldown, Passwort-Regel, Neu-Aufloesen der Kanal-IDs (086) |
| `server_profile_test` | echte `ts3_server_profile.c` gegen Stubs | Root-Wahl, atomare Getter, Passwort wischen (086) |
| `nick_anonymize_test` | echte `nick_anonymize.c` gegen Stubs | Bindung an die Verbindung, UTF-8-Puffer, kein Klarname im Log (086) |
| `voice_keys_test` | `voice_modes.c` | Tasten-Reset und Hotkey-Meldung nur auf dem Poller-Thread (086) |
| `ui_config_logic_test` | `ui_config_logic.h` | Pfad-Kopien, Distanz-Parser, `voice_presets.cfg`-Pruefung, Dialog-eigene Felder (087) |
| `ui_chat_ring_test` | `ui_chat_ring.h` + `ui_spin_lock.h` | Mehrere Erzeuger gleichzeitig (pthreads, TSan sauber) (088) |
| `ui_worker_slot_test` | `ui_worker_slot.h` | Overlay-Monitor wird genau einmal gejoint, auch bei Start/Stop-Rennen (088) |
| `range_units_test` | `range_units.h` | Reichweiten in Conan-Fundamenten (1 = 2,56 m): Umrechnung, Hub-Grenzen nach innen runden, "Zahl veraendert?"-Test, Einheiten-Namen (091) |

**091 (Reichweite in Fundamenten)** erweitert ausserdem `config_text_test`
(`ShowRangesInFoundations`), `ui_config_logic_test` (`ui_dist_parse_display`, kein Drift),
`hub_parser_test` (`ForceRangesInFoundations` rechnet die Root-Werte in Meter um),
`server_profile_test` (Getter, Hoerbonus-Zeile) und `voice_modes_test` (Chat-Zeile).

Stand nach den Review-Fixes: **34 Suiten**, alle gruen; MinGW-DLL-Build ohne Warnungen.
Stand nach 091: **35 Suiten** (+ `range_units_test`), +212 Pruefungen, weiter alle gruen,
MinGW-Build ohne Warnungen.

**Warum `-ffp-contract=off` in `tests/run_tests.sh`?** Auf einem Prozessor mit FMA
(„fused multiply-add“) fasst gcc `a*b + c` standardmaessig zu **einer** Instruktion zusammen,
die nur einmal rundet. Das CI-Image tut das, der lokale Build nicht — `audio_fx_test`
(neuer Hall/Lowpass muss **bitgleich** zum alten Code klingen) schlug deshalb nur im CI fehl:
alter und neuer Code wurden an unterschiedlichen Stellen zusammengefasst und rundeten
verschieden. Der echte Release-Build (MSVC, `/fp:precise`) fasst **nicht** zusammen; mit
dem Flag rechnen die Tests genauso wie das ausgelieferte Plugin.

**Stand Checks:** 241 gesamt (V8.1-Erweiterung in `doku/aenderungen/008`).

**Wie das ohne Windows geht:**

- `src/core/util/compat_crt.h` bildet die MSVC-Spezialfunktionen
  (`strncpy_s`, `strtok_s`, `_strnicmp` …) fuer den reinen gcc-Build auf
  POSIX-Aequivalente ab. Auf MSVC und MinGW ist der Header ein No-Op —
  der Windows-Build bleibt byte-identisch.
- `tests/support/win32_shim/` stellt ein Test-eigenes Mini-`<windows.h>`
  bereit (Mutex, Uhr, Atomics), damit `player_table.c` unveraendert auf
  Linux kompiliert. Die Uhr laesst sich per `win32_shim_advance_ms()`
  vorspulen — so testet man den 2-Minuten-Verfall in Millisekunden.
- Test-Binaries landen in `tests/out/` (gitignored, nie committen).

## Warum dieses Sicherheitsnetz existiert

V7-Lektion (Audit-Kernproblem 6): Es gab nur einen Test, baubar auf genau
einem Windows-PC — jede Aenderung war ein Blindflug, Fehler fielen erst im
laufenden TS-Client auf. Ab V8 gilt fuer **jedes Arbeitspaket** als
Definition of Done:

```
bash tests/run_tests.sh   →  RESULT: ALL SUITES PASSED
bash build/build_mingw.sh →  OK: built bin/mingw/conan_exiles.dll
```

Erst wenn beides gruen ist, folgt (wo hoerbar/sichtbar relevant) der
manuelle Test im TS-Client durch einen Menschen.
