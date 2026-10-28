# Modul: UI-Dialoge (`src/ui/dialogs/`, `src/ui/config/`)

Das F10-Einstellungsfenster des Plugins. Seit V8.7 (siehe
`doku/aenderungen/024-ui-main-split.md`) ist die frueher monolithische `ui_main.c`
(~4200 Zeilen) in themenbezogene Dateien aufgeteilt. Seit V8.12
(`doku/aenderungen/031-v8-12-g-config-f10.md`) arbeitet der Dialog auf einer
**Arbeitskopie** von `g_config`. Die Review-Fixes aus
`doku/aenderungen/087-review-fixes-f10-dialog.md` (Stand dieses Dokuments) haben
Lecks, Speichern pro Tastendruck, Eingabepruefung und mehr behoben.

**Thread-Vertrag:** Alle Dateien hier laufen auf dem **Einstellungs-UI-Thread**
(eigene `GetMessage`-Schleife in `showConfigInterface`, gestartet von
`settings_dialog_thread` in `src/ui/input/key_watcher.c`). **Kein TS-API-Aufruf**
aus diesem Modul — Aenderungen wirken ueber die Arbeitskopie, dann
`config_modify` / `config_save` (beide thread-sicher, `src/core/config/config.h`)
bzw. Flags/Queues (`ts3_audio_request_recompute_all`, `displayInChat`).
Den Dialog-Guard (`config_dialog_try_open` / `config_dialog_close`) gibt **nur**
`settings_dialog_thread` frei — `showConfigInterface` fasst ihn nie an.

## Dateien und Zustaendigkeit

| Datei | Zustaendigkeit |
|-------|----------------|
| `ui_config_internal.h` | Gemeinsame Deklarationen der Split-Dateien: geteilte GDI-Globals (`g_hHudComboBrush`, `g_configDialogDestroying`), die datei-uebergreifenden Helfer, die 7 ausgelagerten `ConfigDialogProc`-Nachrichten-Handler, die **eindeutigen IDs der Checkbox-Labels** (`IDC_LABEL_*`, 2010-2013) und die Bild-Cache-Funktionen. |
| `ui_config_dialog.c` | **Dialog-Huelle:** `ConfigDialogProc` als schlanke Nachrichten-Weiche, `ShowCategoryControls` (Tab-Wechsel Patch/Advanced/Presets), `ApplyFontToControl`, `showConfigInterface` (Huelle: `ui_cfg_dialog_begin` -> `showConfigInterfaceRun` -> `ui_image_cache_release` -> `ui_cfg_dialog_end`), `showConfigInterfaceRun` (Auto-Find, COM, Fensterklasse, Fenster, Nachrichtenschleife, `UnregisterClassW`), `showPathSelectionDialogThread` (ungenutzt), `forceWindowToForegroundNoMouse`, GDI-Aufraeumen (Schriften, Pinsel) bei `WM_DESTROY`. |
| `ui_config_controls.c` | **`WM_CREATE`-Rumpf:** legt alle Controls des Dialogs an, setzt die Tasten-Aufnahme zurueck, laedt die Kategorie-Hintergruende und loest den angezeigten Pfad auf; dazu die HUD-Theme/Position/Groesse-Combo-Helfer (`ui_populate_*`, `ui_sync_*`, `ui_read_*`). |
| `ui_config_command.c` | **`WM_COMMAND`-Handler** `ui_config_on_command`: Kategorie-Tabs, Tasten-Aufnahme starten, Checkboxen, HUD-Combos (Live-Vorschau), Save (Patch-Tab und Advanced-Tab), Preset Load/Rename, Distanzfelder (`EN_CHANGE`). Dazu `ui_collect_distances_for_save` (Distanzfelder lesen **und pruefen**). |
| `ui_config_draw.c` | **Zeichnen:** `WM_DRAWITEM` (Owner-Draw Tasten/Boxen), `WM_ERASEBKGND` (Hintergrundbild pro Kategorie), `WM_CTLCOLOR*` (Textfarben, dunkler HUD-Combo); Bitmap-Helfer `LoadBackgroundFromResource`, `DrawButtonWithBitmap`, `calculateButtonWidth`; `CheckboxLabelProc`; der **Bild-Cache** (`ui_image_cache_get`, `ui_checkmark_icon_get`, `ui_image_cache_release`). |
| `ui_presets.c` | **Voice-Presets:** `PresetSaveDialogProc`, `PresetRenameDialogProc`, `showPresetSaveDialog`, `createPresetsCategory` (genau einmal — die V8.2-Dublette bleibt entfernt), `updatePresetLabels`, `PresetLabelProc`. |
| `ui_path_steam.c` | **Pfad + Steam:** `browseSavedPath`/`browseFolderModern` (Ordner-Browser, lehnt zu lange Pfade mit Meldung ab), `findConanExilesAutomatic`, `parseSteamLibraryFolders`, `readSteamIDFromRegistry`, `savedExistsInFolder`. |
| `ui_main.c` | **Rest-Klebstoff:** `loadDefaultSettingsFromConfig`/`saveDefaultSettingsToConfig`, `processKeyCapture` (Tasten-Aufnahme, Timer 1). **Nicht** hier: `ui_config_on_command` — der liegt seit V8.10 in `ui_config_command.c`. |
| `ui_dynamic.c`, `ui_messages.c` | Dynamische Interface-Updates (Distanzfeld-Eingabe, Server-Grenzen, Zonen-Anzeige) bzw. Status-/Range-Meldungen (`showStatusMessage`). |
| `../config/ui_config_state.c/.h` | **Arbeitskopie** (`ui_cfg()`), `ui_cfg_dialog_begin/end`, `ui_cfg_commit` (uebernimmt **nur die Dialog-Felder** per `config_modify`), HUD-Live-Vorschau `ui_cfg_hud_preview`, Pfad-Helfer, Legacy-Spiegel. |
| `../config/ui_voice_presets.c` | Voice-Presets-Datei (`voice_presets.cfg`: atomar schreiben, validiert lesen), `writeFullConfiguration`, `saveVoiceSettings`, `saveVoicePreset`/`loadVoicePreset`/`renameVoicePreset`. |
| `../config/ui_config_logic.h` | **Reine Logik** (nur Header, host-testbar, `tests/ui_config_logic_test.c`): `ui_wcs_copy`, `ui_game_folder_fits`, `ui_dist_parse`, `ui_preset_parse_*`, `ui_cfg_merge_*`. |

## Nachrichtenfluss (`ConfigDialogProc`)

```
ConfigDialogProc (ui_config_dialog.c)  = Weiche
  ├─ WM_CREATE         → ui_config_on_create        (ui_config_controls.c)
  ├─ WM_ERASEBKGND     → ui_config_on_erasebkgnd     (ui_config_draw.c)
  ├─ WM_CTLCOLORSTATIC → ui_config_on_ctlcolorstatic (ui_config_draw.c)
  ├─ WM_CTLCOLORLISTBOX→ ui_config_on_ctlcolorlistbox(ui_config_draw.c)
  ├─ WM_CTLCOLOREDIT   → ui_config_on_ctlcoloredit   (ui_config_draw.c)
  ├─ WM_COMMAND        → ui_config_on_command        (ui_config_command.c)
  ├─ WM_TIMER          → (direkt: processKeyCapture / clearStatusMessage)
  ├─ WM_DRAWITEM       → ui_config_on_drawitem       (ui_config_draw.c)
  └─ WM_DESTROY        → (direkt: Tasten-Aufnahme zuruecksetzen, Schriften/Pinsel
                          freigeben, ui_cfg_dialog_end, PostQuitMessage)
```

## Lebenslauf eines Dialogs (087)

```
settings_dialog_thread (key_watcher.c)
  showConfigInterface()
     ui_cfg_dialog_begin()        config_copy -> Arbeitskopie; HUD-Werte merken
     showConfigInterfaceRun()
        Auto-Find (optional), CoInitializeEx, RegisterClassW, CreateWindowExW
        WM_CREATE  -> Controls, Hintergruende, Pfad aufloesen
        Nachrichtenschleife ... Save / Cancel / Fenster schliessen
        UnregisterClassW, CoUninitialize
     ui_image_cache_release()     Bilder, Haken-Icon, Hintergruende freigeben
     ui_cfg_dialog_end()          unveraenderte HUD-Vorschau zuruecknehmen
  config_dialog_close()           <- der EINZIGE Guard-Release
```

## Was wird wann gespeichert? (087)

| Aktion | Wirkung |
|---|---|
| Tippen in ein Distanzfeld, Checkbox, Taste aufnehmen | nur **Arbeitskopie** (+ Anzeige) |
| HUD-Combo waehlen | Arbeitskopie **und** `g_config.hud*` (Vorschau, **keine** Datei); Cancel stellt zurueck |
| **Save Configuration** (Patch- oder Advanced-Tab) | Distanzfelder **pruefen** (leer/ungueltig/ausserhalb Server-Grenzen = Fehlermeldung, nichts wird geschrieben) -> `ui_cfg_commit`: `config_modify` (nur Dialog-Felder) + `config_save` + CEPOS/Audio neu |
| **Load** eines Presets | Distanzen/Tasten in die Arbeitskopie, danach wie Save ("loaded and saved") |
| **Cancel** / Fenster schliessen | Arbeitskopie wird verworfen; ungespeicherte HUD-Vorschau wird zurueckgenommen |

Die **Dialog-Felder** (nur diese uebernimmt der Commit): `savedPath`,
`automaticPatchFind`, `distanceWhisper/Normal/Shout`, `whisperKey`, `normalKey`,
`shoutKey`, `voiceToggleKey`, `configUIKey`, `enableDistanceMuting`,
`enableAutomaticChannelChange`, `enableVoiceToggle`, `hudTheme`, `hudPosition`,
`hudSize`. **Nie** vom Dialog geschrieben: `automaticSavedPath` (Pos-Watcher),
`defaultsAppliedServer` (Server-Profil), `enableVoiceOverlay`, `debugMode`,
`enablePosFile`.

## Wichtige Eigenschaften / Fallen

- **GDI-Besitz:** Schriften und der HUD-Combo-Pinsel werden in `WM_DESTROY`
  freigegeben; Bilder/Icon/Hintergruende **nach** der Nachrichtenschleife
  (`ui_image_cache_release`). Wer ein neues Bild im Zeichencode braucht, holt es
  mit `ui_image_cache_get(ID)` — **nie** selbst `LoadImageW` pro Zeichnen und
  **nie** `DeleteObject` auf das Ergebnis.
- **Steuerelement-IDs:** 2001-2005 = Tasten-Boxen (Owner-Draw), 2010-2013 =
  Checkbox-Labels (`IDC_LABEL_*`). IDs im Dialog muessen eindeutig bleiben
  (`GetDlgItem`).
- **Pfadlaengen:** Kein `wcscpy_s`/`wcscat_s` auf `MAX_PATH`-Puffer mit
  Nutzer-/Dateiinhalt (bricht den ganzen TS-Prozess ab). `ui_wcs_copy` /
  `ui_game_folder_fits` benutzen.
- **Eingaben:** Distanzen nur ueber `ui_dist_parse` lesen (kein `_wtof`).
- **Hotkey-Thread:** `writeFullConfiguration` startet ihn **nicht** mehr neu — er
  liest `g_config` ohnehin in jedem Durchlauf.
- **Presets-Datei:** `savePresetsToConfigFile` schreibt `.tmp` + Austausch,
  `loadPresetsFromConfigFile` validiert jede Zeile (Format unveraendert).
