# Modul: UI-Dialoge (`src/ui/dialogs/`)

Das F10-Einstellungsfenster des Plugins. Seit V8.7 (siehe
`doku/aenderungen/024-ui-main-split.md`) ist die frueher monolithische `ui_main.c`
(~4200 Zeilen) in themenbezogene Dateien aufgeteilt. Reiner Verschiebe-Split — kein
Verhaltensunterschied gegenueber V7/frueheren V8-Staenden.

**Thread-Vertrag:** Alle Dateien hier laufen auf dem **Einstellungs-UI-Thread**
(eigene `GetMessage`-Schleife in `showConfigInterface`). Kein TS-API-Aufruf aus diesem
Modul — Aenderungen wirken ueber `g_config`/Globals bzw. die bestehende Speicher-Logik.

## Dateien und Zustaendigkeit

| Datei | Zustaendigkeit |
|-------|----------------|
| `ui_config_internal.h` | Gemeinsame Deklarationen der Split-Dateien: geteilte GDI-Globals (`g_hHudComboBrush`, `g_configDialogDestroying`), die datei-uebergreifenden Helfer und die 7 ausgelagerten `ConfigDialogProc`-Nachrichten-Handler. |
| `ui_config_dialog.c` | **Dialog-Huelle:** `ConfigDialogProc` als schlanke Nachrichten-Weiche, `ShowCategoryControls` (Tab-Wechsel Patch/Advanced/Presets), `ApplyFontToControl`, `showConfigInterface` (COM, Fensterklasse, Nachrichtenschleife), `showPathSelectionDialogThread`, `forceWindowToForegroundNoMouse`, GDI-Aufraeumen bei `WM_DESTROY`. |
| `ui_config_controls.c` | **`WM_CREATE`-Rumpf:** legt alle Controls des Dialogs an; dazu die HUD-Theme/Position/Groesse-Combo-Helfer (`ui_populate_*`, `ui_sync_*`, `ui_read_*`). |
| `ui_config_draw.c` | **Zeichnen:** `WM_DRAWITEM` (Owner-Draw Tasten/Boxen), `WM_ERASEBKGND` (Hintergrundbild pro Kategorie), `WM_CTLCOLOR*` (Textfarben, dunkler HUD-Combo); Bitmap-Helfer `LoadBackgroundFromResource`, `DrawButtonWithBitmap`, `calculateButtonWidth`; `CheckboxLabelProc`. |
| `ui_presets.c` | **Voice-Presets:** `PresetSaveDialogProc`, `PresetRenameDialogProc`, `showPresetSaveDialog`, `createPresetsCategory` (genau einmal — die V8.2-Dublette bleibt entfernt), `updatePresetLabels`, `PresetLabelProc`. |
| `ui_path_steam.c` | **Pfad + Steam:** `browseSavedPath`/`browseFolderModern` (Ordner-Browser), `findConanExilesAutomatic`, `parseSteamLibraryFolders`, `readSteamIDFromRegistry`, `savedExistsInFolder`. |
| `ui_main.c` | **Rest-Klebstoff:** `ui_config_on_command` (der `WM_COMMAND`-Handler — Tasten, Checkboxen, Combos, Presets, Distanzfelder), `loadDefaultSettingsFromConfig`/`saveDefaultSettingsToConfig`, `processKeyCapture`. |
| `ui_dynamic.c`, `ui_messages.c` | Bestehend (nicht Teil des Splits): dynamische Interface-Updates bzw. Status-/Range-Meldungen. |

## Nachrichtenfluss (`ConfigDialogProc`)

```
ConfigDialogProc (ui_config_dialog.c)  = Weiche
  ├─ WM_CREATE         → ui_config_on_create        (ui_config_controls.c)
  ├─ WM_ERASEBKGND     → ui_config_on_erasebkgnd     (ui_config_draw.c)
  ├─ WM_CTLCOLORSTATIC → ui_config_on_ctlcolorstatic (ui_config_draw.c)
  ├─ WM_CTLCOLORLISTBOX→ ui_config_on_ctlcolorlistbox(ui_config_draw.c)
  ├─ WM_CTLCOLOREDIT   → ui_config_on_ctlcoloredit   (ui_config_draw.c)
  ├─ WM_COMMAND        → ui_config_on_command        (ui_main.c)
  ├─ WM_TIMER          → (direkt: processKeyCapture / clearStatusMessage)
  ├─ WM_DRAWITEM       → ui_config_on_drawitem       (ui_config_draw.c)
  └─ WM_DESTROY        → (direkt: GDI freigeben, PostQuitMessage)
```
