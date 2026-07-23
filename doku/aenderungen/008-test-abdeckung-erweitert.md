# 008 — Test-Abdeckung erweitert (V8.1 Fortsetzung)

## Was wurde geaendert?

In den **bestehenden** gcc-Unit-Test-Suiten unter `tests/` wurden zusaetzliche
`CHECK`-Assertions ergaenzt — **keine** Aenderungen an Produktions-`.c`/`.h`-Dateien,
keine neuen Module, kein `.vcxproj`-Edit.

| Suite | Checks vorher | Checks jetzt | Neu |
|---|---:|---:|---:|
| `hub_parser_test` | 55 | 97 | +42 |
| `proximity_math_test` | 32 | 59 | +27 |
| `zone_resolve_test` | 30 | 46 | +16 |
| `player_table_test` | 30 | 30 | — |
| `render_state_test` | 9 | 9 | — |
| **Summe** | **158** | **241** | **+83** |

Verifikation: `bash tests/run_tests.sh` → `RESULT: ALL SUITES PASSED`

## Warum?

V8.1 hat die puren Kernmodule erstmalig abgesichert (144 Checks laut Plan;
zwischenzeitlich 158 inkl. `render_state_test`). Jede Regression in Parser-,
Mathe- oder Zonen-Logik soll **sofort** auffallen, bevor der TS-Client
gestartet wird — besonders vor den Thread-Umbauten ab V8.4.

Die neuen Checks zielen auf **Randfaelle und Invarianten**, die echte Bugs
fangen wuerden (Clamping, Monotonie, Einbahn-Soundproof, Skalen-Erkennung),
nicht auf triviale Duplikate.

## Wie — welche Eigenschaften werden jetzt geprueft?

### hub_parser_test (+42)

- **Standardwerte:** leeres `[GLOBAL]` → `audioMaxVolume=1.0`, `audioMinDistance=1.0`,
  `filterIntensity=100`, `realisticAudio=0`.
- **Clamping:** `AudioMinDistance=0` → `0.1`; Distanzen > 1000 → `1000`;
  `FilterIntensity` / Alias `hubAudioFilterIntensity` → `0..100`.
- **AudioMaxVolume-Semantik:** Prozent (`130` → `1.3`), direkter Gain (`2.5` → cap `2.0`),
  Grenze `3.0` vs. `3.1` (unterschiedliche Interpretation — siehe Auffaelligkeiten).
- **RealisticAudio / Whitespace / Gross-Kleinschreibung** bei Keys und Booleans.
- **Mehrere Zonen:** `[ZoneName=…]`, `[Zone=…]`, ungeklammertes `ZoneName=`;
  Overflow > `HUB_MAX_ZONES` wird gekappt.
- **Rassen:** globale Limits werden vererbt; ungueltige Hotkeys (`0`, `>=256`) → `0`.
- **Partielle Beschreibungen:** unbekannte Sektionen, Sektionswechsel `[ZONES]` → `[RACE]`.
- **Robustheit:** `NaN`/`inf`/negative Werte crashen nicht, liefern geklemmte Defaults.

### proximity_math_test (+27)

- **Lautstaerke-Kurve:** dichte Monotonie-Probe `0..1.12×` Reichweite; Soft-Tail
  bei `1.11×` noch hoerbar, bei `1.12×` exakt `0`; negative Distanz / kleine
  `voiceDistance` / kleines `maxVolume` abgesichert.
- **Pan:** unveraendert Equal-Power (`L²+R²≈1`) — zusaetzlich **Front/Back-Dot**,
  **3D-Dot**, **Listener-Forward** aus Yaw (CEPOS-Konvention).
- **Rear-Psychoakustik:** schwach hinten (`frontBack=-0.01`) — `directionVolume`
  nur leicht reduziert, aber `cutoffMul`/`drrMul` sofort voll aktiv.
- **Binaural-Gains:** teilweise hinten (~60°) daempft beide Kanaele; Spiegel-
  symmetrie und Bereich `0.0825..1.0` bleiben abgedeckt.
- **Hub-Lautstaerke-Helfer:** `proximity_calculate_volume_with_hub` mit Hub-130%,
  Zonen-Cap 50%, `listenAddDistance`-Bonus; `prox_apply_diffuse_samples` (Mono/Stereo,
  `drr`-Grenzen, zu kurze Puffer).

### zone_resolve_test (+16)

- **Ueberlappende Zonen:** erste passende Zone (niedrigster Index) gewinnt.
- **Ecken/Kanten:** SW-Ecke innen; exakte Max-Kante (`z=30`) **ausserhalb**
  (Half-Open-Raycast); inset nahe Max-Kante innen.
- **Hoehenband ±1 m Toleranz** (`hEps`): `49.0` innen, `48.9` aussen, `61.0` innen,
  `61.1` aussen.
- **Skalen-Sonden:** Spieler in Metern, Ecken in UE-Zentimetern (`×100`-Probe);
  rohe cm-Koordinaten bei `1×`.
- **UE-Layout** (`xzFloor=0`): X-Y-Bodenflaeche, Hoehe ueber `pz`; Reverb/Soundproof-
  Kombination (kein Hard-Mute ohne `SoundProof`-Flag).

## Entdeckte Auffaelligkeiten (Code-Verhalten, nicht gefixt)

Diese Punkte sind **bewusst als Ist-Verhalten** getestet — Entscheidung dem Menschen:

1. **AudioMaxVolume-Sprung bei 3.0:** Werte `≤3.0` sind direkter Gain (cap 2.0),
   ab `3.1` Prozent-Semantik (`3.1` → Gain `0.031`). Diskontinuitaet an der Grenze.
2. **prox_rear_psychoacoustics:** Schon minimal negativer `frontBack` setzt
   `cutoffMul=0.75` und `drrMul=0.85` — nur `directionVolume` skaliert graduell.
   (Header suggeriert „wenn hinten“, Code schaltet bei jedem `frontBack < 0`.)
3. **prox_binaural_stereo_gains:** Direkt von hinten (`frontBack=-1`) ergibt
   **keine** Daempfung (`backAtt=1.0`); staerkste Absenkung liegt bei teilweiser
   Hinten-Position (~`frontBack≈-0.2`).
4. **Lautstaerke-Soft-Tail:** Kurz vor `1.12×` Reichweite faellt die Hoerbarkeit
   durch den Soft-Tail praktisch auf null — nicht erst exakt an `fadeEnd`.
5. **Ungeklammertes `ZoneName=`** in `[ZONES]` startet immer eine **neue** Zone
   (auch direkt nach `[Zone=…]`).

## Lerneffekt

- **Tests gegen echten Code schreiben**, nicht gegen Kommentare: Mehrere Checks
  haengen am **tatsaechlichen** Verhalten (z. B. Binaural-Hinten, Volume-Tail).
- **Half-Open-Geometrie** bei Zonen: Min-Kanten innen, Max-Kanten aussen — Ecken
  auf Max-Koordinaten sind oft **draußen**.
- **`memset`-Fallen:** `ProximityVolumeContext.currentZoneIndex=0` nach `memset`
  aktiviert sofort Zonen-Gain — Tests muessen `-1` setzen, wenn die Zone ignoriert
  werden soll.
- **Skalen-Sonden:** `zone_resolve` skaliert die **Spielerposition**, nicht die
  Ecken — cm-Ecken + m-Position erfordern die `×100`-Probe.
