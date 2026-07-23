# 018 — Config-Single-Writer (plugin.cfg hat genau EINEN Schreiber)

**Phase:** V8.5b · **Besitzer-Prinzip:** `doku/01-architektur-v8.md` ·
**Lektion:** `doku/02-lessons-learned-v7.md` Lektion 4 ("zwei Besitzer = unsichtbare Bugs")

## Worum geht es? (Anfaenger-Erklaerung)

`plugin.cfg` ist die Einstellungsdatei des Plugins (Distanzen, Hotkeys,
Overlay-Optionen, Pos.txt-Pfad). Sie liegt unter
`Dokumente\Conan Exiles TeamSpeak plugin\plugin.cfg`.

In V7 gab es **zwei voellig getrennte Systeme**, die dieselbe Datei geschrieben
haben:

- **Neu (Rewrite):** `src/core/config/config.c` mit der Struktur
  `PluginConfig g_config` und den Funktionen `config_load` / `config_save`.
- **Alt (Legacy-F10-Pfad):** `src/core/config/config_files.c` mit
  `readConfigurationSettings` / `saveVoiceSettings` / `writeFullConfiguration`,
  die direkt auf den alten Globalen (`distanceWhisper`, `savedPath`,
  `voiceHudTheme`, …) arbeiteten.

Gebrueckt wurden beide durch `plugin_ui_sync_from_config` /
`plugin_ui_sync_to_config` in `src/ui/plugin_ui_compat.c`.

**Das Problem:** Wenn zwei Stellen dieselbe Datei schreiben, gewinnt mal die
eine, mal die andere. Genau deshalb hat der Legacy-Schreiber Keys wie
`DebugMode`, `EnableVoiceOverlay` und `DefaultsAppliedServer` **verloren** (er
kannte sie gar nicht). Ein Notpflaster rief am Ende des Legacy-Schreibens noch
`config_save()` hinterher — die Datei wurde also faktisch **zweimal** geschrieben.

## Datenfluss VORHER (2 Schreiber)

```
F10 "Save" (Settings-Dialog-Thread)
   │
   ├─► writeFullConfiguration()  ─┐
   │      liest plugin.cfg,        │  (1) LEGACY-SCHREIBER
   │      baut Zeilen neu,         │      _wfopen(plugin.cfg,"w")
   │      schreibt plugin.cfg  ────┘      → verliert DebugMode/Overlay/DefaultsAppliedServer
   │
   └─► danach: plugin_ui_on_settings_saved()
          Globale → g_config
          config_save() ───────────►  (2) KANONISCHER SCHREIBER
                                          _wfopen(plugin.cfg,"w")  (schreibt nochmal!)

saveVoiceSettings()  → gleiches Muster: erst Legacy-Write, dann config_save().
readConfigurationSettings() → liest plugin.cfg direkt UND legt sie bei Bedarf neu an.
```

Zwei Funktionen (`writeFullConfiguration`, `saveVoiceSettings`) plus die
"create if missing"-Zweige von `readConfigurationSettings` haben die Datei
`plugin.cfg` mit eigenem `_wfopen(..., "w")` beschrieben.

## Datenfluss NACHHER (1 Schreiber)

```
F10 "Save" / Advanced / Preset-Load
   │
   ├─► writeFullConfiguration()  → aktualisiert nur die Globalen + savedPath
   ├─► saveVoiceSettings()       → aktualisiert nur die aktive Distanz
   │        │
   │        ▼
   └─► plugin_ui_on_settings_saved()
          │
          ▼
       plugin_ui_sync_to_config()   (der EINE Sync-Besitzer)
          Globale → g_config
          config_clamp() → config_apply()
          config_save() ───────────►  EINZIGER SCHREIBER
                                          _wfopen(plugin.cfg,"w")  in config.c
```

`writeFullConfiguration` und `saveVoiceSettings` oeffnen **keine** Datei mehr.
Sie bereiten nur die Werte in den Globalen vor und rufen den einen Speicherpfad.
`readConfigurationSettings` / `loadVoiceDistancesFromConfig` sind geloescht; wo
die UI vorher `plugin.cfg` neu einlas, ruft sie jetzt `plugin_ui_sync_from_config()`
(liest `g_config`, das beim Start einmal von `config_load` gefuellt wird).

## Beweis: nur EINE Funktion schreibt plugin.cfg

```
$ grep -rn '_wfopen\|_wfopen_s\|fopen\|WriteFile' src | grep -iE 'L?"w"'
src/core/config/config.c:260:      FILE* f = _wfopen(path, L"w");            # plugin.cfg  ← der eine Schreiber
src/core/config/config_files.c:297: _wfopen_s(&file, presetFile, L"w");      # voice_presets.cfg (andere Datei)
src/ui/dialogs/ui_main.c:2134:      FILE* f = _wfopen(configFile, L"w");     # default_settings.cfg (andere Datei)
```

Der Pfad `...\plugin.cfg` wird zum Schreiben nur in `config.c` (`config_save`,
ueber `config_file_path`) gebaut. Alle anderen Stellen, die den String
`plugin.cfg` bauen, oeffnen ihn mit `"r"` (nur Lesen).

## Key-fuer-Key-Kompatibilitaet (vor der Aenderung geprueft)

Der Leser in `config.c` (`config_apply_line`) akzeptiert **alle** Keys, die
jemals ein Schreiber erzeugt hat — es geht also nichts verloren:

| Key | Legacy-Schreiber | `config_save` | `config_apply_line` liest |
|---|---|---|---|
| `SavedPath` | ja | ja | ja |
| `AutomaticSavedPath` | ja | ja | ja |
| `AutomaticPatchFind` | ja | ja | ja |
| `DistanceWhisper/Normal/Shout` | ja | ja | ja |
| `WhisperKey/NormalKey/ShoutKey` | ja | ja | ja |
| `VoiceToggleKey` | ja | ja | ja |
| `ConfigUIKey` | ja | ja | ja |
| `EnableDistanceMuting` | ja | ja | ja |
| `EnableAutomaticChannelChange` | ja | ja | ja |
| `EnableVoiceToggle` | ja | ja | ja |
| `HudTheme/HudPosition/HudSize` | teils | ja | ja |
| `EnableVoiceOverlay` | **nein** (verloren) | ja | ja |
| `DebugMode` | **nein** (verloren) | ja | ja |
| `DefaultsAppliedServer` | **nein** (verloren) | ja | ja |

**Wichtigstes Ergebnis:** Die drei zuletzt genannten Keys wurden schon vorher
**ausschliesslich** von `config_save` geschrieben und von `config_apply_line`
gelesen. Der Legacy-Schreiber hat sie nur weggeworfen. Nach der Umstellung
schreibt ausschliesslich `config_save` — die Datei wird also strikt
**vollstaendiger**, nie unvollstaendiger. Byte-Format (Reihenfolge, `%.1f`,
`true`/`false`) bleibt exakt das von `config_save` (unveraendert).

### Wo leben die Keys jetzt?

- **In `PluginConfig` (`g_config`)** — die einzige Wahrheit im Speicher. Alle
  20 Keys oben sind Felder dieser Struktur.
- **Die alten Globalen** (`distanceWhisper`, `voiceHudTheme`, `savedPath`, …)
  sind reine **Arbeitskopien** fuer den noch unveraenderten F10-Dialog. Beim
  Oeffnen des Dialogs werden sie aus `g_config` gefuellt
  (`plugin_ui_sync_from_config`), beim Speichern zurueckgeschrieben
  (`plugin_ui_sync_to_config`). Der Dialog selbst (`ui_main.c`) bleibt in diesem
  Paket bewusst auf seinen Globalen (UI-Rewrite ist erst V8.7).

### Bewusst NICHT in plugin.cfg (eigene Dateien, eigene Schreiber)

Diese wurden **nicht** angefasst — sie schreiben klar abgegrenzte, andere
Dateien und haben mit dem plugin.cfg-Single-Writer nichts zu tun:

- `voice_presets.cfg` — Voice-Presets (`savePresetsToConfigFile` /
  `loadPresetsFromConfigFile`). Keys: `CurrentPreset`, `[PresetN]`, `Name`,
  `Whisper/Normal/Shout`, `WhisperKey/NormalKey/ShoutKey`, `VoiceToggleKey`,
  `IsUsed`. Diese Felder gibt es in `PluginConfig` **nicht**; die Presets
  bleiben absichtlich eine eigene, sauber gekapselte Persistenz.
- `default_settings.cfg` — Server-Default-Tracking (`saveDefaultSettingsToConfig`
  in `ui_main.c`). Keys: `ServerConfigHash`, `HasAppliedDefaultSettings`,
  `Default*`. Ebenfalls eigene Datei, unveraendert.

## Warum ist das besser / stabiler?

- **Ein Besitzer, eine Wahrheit.** Es gibt keinen "wer gewinnt?"-Fall mehr —
  jeder Speichervorgang laeuft durch genau `config_save()`. Damit verschwindet
  die V7-Klasse "Einstellung manchmal nicht gespeichert" (Lektion 4).
- **Keine stillen Datenverluste.** Kein Schreiber wirft mehr Keys weg.
- **Weniger Code, weniger Datei-I/O.** Die Datei wird pro Save **einmal**
  geschrieben statt zweimal; die fehleranfaellige "Zeilen einlesen, einzeln
  patchen, zurueckschreiben"-Logik ist weg.

## Wie getestet?

- `bash tests/run_tests.sh` → alle Suiten gruen (inkl. `layering_guard`).
- `bash build/build_mingw.sh` → DLL linkt.
- Grep-Nachweis oben: genau ein `plugin.cfg`-Schreiber (`config_save`).
- **Offen (Mensch, im TS-Client):** F10 oeffnen, Distanzen/Hotkeys/Overlay-Theme
  aendern, "Save Configuration" → plugin.cfg pruefen (alle 20 Keys vorhanden,
  Reihenfolge wie `config_save`), TS neu starten → Werte bleiben erhalten.
  Zusaetzlich: Preset laden (schreibt ueber `writeFullConfiguration`) und
  Auto-Patch-Pfad speichern → `SavedPath`/`AutomaticSavedPath` korrekt.
