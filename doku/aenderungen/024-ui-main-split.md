# 024 — ui_main.c aufteilen (reiner Verschiebe-Split)

**Phase:** V8.7 (UI-Rewrite) ·
**Lektion:** `02-lessons-learned-v7.md` Kernproblem 5 (`ui_main.c` = 4195 Zeilen, unwartbar)

## Worum geht es? (Anfaenger-Erklaerung)

Das F10-Einstellungsfenster (Pfad, Tasten, Distanzen, Presets, HUD-Optionen) lag komplett
in **einer** Datei: `src/ui/dialogs/ui_main.c` mit **4197 Zeilen**. Diese Aenderung zerlegt
die Datei in mehrere kleinere Dateien — **ohne** dass sich am Verhalten irgendetwas aendert.
Es ist ein reiner **Verschiebe-Split**: Code wird 1:1 in neue Dateien verschoben, nichts wird
umgeschrieben, keine Logik, keine Steuerungs-IDs, keine Pixel im Dialog aendern sich.

## Wie war es vorher?

Eine einzige Riesendatei mit ~34 Funktionen und einer **1830 Zeilen langen** Fensterprozedur
`ConfigDialogProc` (der grosse `switch` ueber alle Windows-Nachrichten). Warum ist das ein Problem?

- **Review:** Niemand kann 4200 Zeilen am Stueck sinnvoll pruefen.
- **Merge-Konflikte:** Zwei Leute, die irgendetwas am Dialog aendern, kollidieren fast sicher.
- **Orientierung:** "Wo wird der HUD-Combo gezeichnet?" — man scrollt ewig.

## Was wurde geaendert?

`ui_main.c` (4197 Zeilen) → **6 `.c`-Dateien + 1 gemeinsamer Header**. Zeilen vorher/nachher:

| Datei | Zeilen | Zustaendigkeit |
|-------|-------:|----------------|
| `ui_main.c` (vorher) | **4197** | (alles) |
| `ui_config_internal.h` (neu) | 50 | geteilte Deklarationen zwischen den Split-Dateien |
| `ui_config_dialog.c` (neu) | 658 | Dialog-Huelle `ConfigDialogProc` (Dispatch), `ShowCategoryControls`, `showConfigInterface`, Thread-Einstieg, Fenster-Vordergrund, GDI-Aufraeumen |
| `ui_config_controls.c` (neu) | 736 | `WM_CREATE`-Rumpf (alle Controls anlegen) + HUD-Theme/Position/Groesse-Combo-Helfer |
| `ui_config_draw.c` (neu) | 1001 | `WM_DRAWITEM`, `WM_ERASEBKGND`, `WM_CTLCOLOR*` + Bitmap-Lade-/Zeichen-Helfer (`DrawButtonWithBitmap` …) + `CheckboxLabelProc` |
| `ui_presets.c` (neu) | 800 | Preset-Dialoge (Save/Rename), `createPresetsCategory`, Preset-Labels |
| `ui_path_steam.c` (neu) | 447 | Ordner-Browser + Steam-Auto-Erkennung (`findConanExilesAutomatic` …) |
| `ui_main.c` (nachher) | 750 | Rest-Klebstoff: `WM_COMMAND`-Handler, Default-Settings laden/speichern, Tastatur-Capture |

Jede neue Datei ist **deutlich unter ~1200 Zeilen**.

### Wie wurde die grosse Fensterprozedur aufgeteilt?

`ConfigDialogProc` war ein einziger `switch (msg)`. Damit die einzelnen Nachrichten in
verschiedene Dateien wandern koennen, wurde jeder grosse `case`-Rumpf **wortwoertlich** in eine
eigene Funktion verschoben (`ui_config_on_create`, `ui_config_on_drawitem`,
`ui_config_on_command`, `ui_config_on_erasebkgnd`, `ui_config_on_ctlcolor*`). Die verbleibende
`ConfigDialogProc` in `ui_config_dialog.c` ist nur noch eine schlanke **Weiche**, die pro
Nachricht die passende Funktion aufruft. `WM_TIMER` und `WM_DESTROY` (klein) bleiben direkt in
der Weiche.

Damit das **identisch** bleibt, wurde exakt eine mechanische Anpassung gemacht: Wo ein `case`
frueher mit `break;` endete (was am Ende der Prozedur `return 0;` bedeutete), gibt die
ausgelagerte Funktion jetzt `return 0;` zurueck. Alle inneren `break;` (z. B. im `WM_COMMAND`-
Unter-`switch`) bleiben unveraendert. Rueckgabewerte (`return 1;`, `return TRUE;`, Pinsel-Handles)
sind 1:1 uebernommen.

### Der gemeinsame Header `ui_config_internal.h`

Was in `ui_main.c` `static` (dateilokal) war und jetzt aus einer **anderen** Split-Datei
gebraucht wird, hat sein `static` verloren und ist im neuen Header deklariert. Konkret
**ent-`static`-t**:

- **2 Variablen:** `g_hHudComboBrush`, `g_configDialogDestroying`
  (angelegt in Controls, gelesen in Draw, freigegeben/gesetzt in der Dialog-Huelle) —
  jetzt in `ui_config_dialog.c` definiert, `extern` im Header.
- **4 Funktionen:** `ui_sync_hud_theme_combo`, `ui_read_hud_theme_from_combo`,
  `ui_read_hud_position_from_combo`, `ui_read_hud_size_from_combo`
  (in Controls definiert, im `WM_COMMAND`-Handler in `ui_main.c` genutzt).

Was nur **innerhalb einer** Datei benutzt wird, bleibt `static` (z. B.
`ui_hud_combo_brush`, `ui_is_hud_styled_combo_child`, `ui_release_config_dialog_gdi`,
die `ui_populate_*`- und die beiden nur intern genutzten `ui_sync_*`-Helfer). Ausserdem
deklariert der Header die 7 ausgelagerten Nachrichten-Handler und die datei-uebergreifend
genutzten, aber vorher nirgends deklarierten Helfer (`ShowCategoryControls`,
`ApplyFontToControl`, `LoadBackgroundFromResource`, `browseSavedPath`, `processKeyCapture`,
`CheckboxLabelProc`).

Jede neue `.c`-Datei nutzt denselben Include-Block wie das Original — dadurch loesen sich alle
bestehenden Symbole exakt wie vorher auf; nur die 12 oben genannten sind neu geteilt.

## Warum ist das besser?

```
VORHER:                          NACHHER:
ui_main.c  ~4200 Zeilen          ui_config_dialog.c   658   (Huelle + Lifecycle)
(alles in einem)                 ui_config_controls.c 736   (WM_CREATE + Combos)
                                 ui_config_draw.c    1001   (Zeichnen)
                                 ui_presets.c         800   (Presets)
                                 ui_path_steam.c      447   (Pfad/Steam)
                                 ui_main.c            750   (WM_COMMAND + Rest)
```

- **Wartbarkeit:** Themenbezogen — man findet "Zeichnen" in `ui_config_draw.c`.
- **Review:** Kleinere Dateien sind ueberschaubar pruefbar.
- **Merge-Konflikte:** Aenderungen an unterschiedlichen Bereichen kollidieren nicht mehr.
- **Kein Risiko:** Da rein verschoben (kein Umschreiben), kann sich das Laufzeitverhalten
  nicht aendern — die IDs, die `switch`-Reihenfolge, die Zeichen-Aufrufe sind identisch.

Das frueher doppelte `createPresetsCategory` (in V8.2 entfernt) bleibt einfach: es existiert
weiterhin **genau einmal** (in `ui_presets.c`).

## Wie getestet?

- **Cross-Build linkt:** `bash build/build_mingw.sh` baut `bin/mingw/conan_exiles.dll`
  (34 statt 29 `.c`-Dateien). Keine doppelten/fehlenden Symbole beim Linken; identisches
  Warnungs-Profil (die Warnungen stammen aus dem 1:1 verschobenen V7-Code).
- **Tests gruen:** `bash tests/run_tests.sh` — alle Suiten inkl. Layering-Wache bestehen.
- **`xmllint --noout project/Conan-Exiles-TeamSpeak.vcxproj`** ohne Fehler.
- **Integritaets-Check:** Ein Skript hat die nicht-leeren Zeilen des Originals mit der Summe
  der neuen Dateien verglichen. Ergebnis: die einzigen "verschwundenen" Zeilen sind
  Kommentar-Banner (durch Datei-Kopfzeilen ersetzt), die `switch`-Geruest-Zeilen (`case …:`
  und 8 aeussere `break;`) sowie exakt die 6 ent-`static`-ten Zeilen — **keine** Logikzeile
  fehlt.
- **Build-Wiring:** `project/Conan-Exiles-TeamSpeak.vcxproj` (ClCompile fuer die 5 neuen
  `.c`, ClInclude fuer den neuen Header) und `build/build_mingw.sh` (Quellliste) aktualisiert.
  Eine `.filters`-Datei existiert in diesem Repo nicht — daher nichts zu aendern.

UI wird nicht per Unit-Test geprueft; der TeamSpeak-Client-Test (F10 oeffnen, Tabs
wechseln, Speichern) steht am Ende der Phase auf einem Windows-Client an.

## Lerneffekt

Eine 4000-Zeilen-Datei zerlegt man **nicht**, indem man "aufraeumt" — sondern in zwei getrennten
Schritten: **erst rein mechanisch verschieben** (0 % Verhaltensrisiko, hier), **dann** ggf.
verbessern. Der Trick fuer eine Riesen-`WndProc`: jeden `case`-Rumpf in eine eigene Funktion
heben und den `switch` zur reinen Weiche machen. Die einzige erlaubte Anpassung — aeusseres
`break;` → `return 0;` — ist verhaltensgleich, weil der `switch` am Ende ohnehin `return 0;`
lieferte. Ein Zeilen-Integritaets-Check beweist hinterher, dass wirklich nur Geruest, keine
Logik, angefasst wurde.
