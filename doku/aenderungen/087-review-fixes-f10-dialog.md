# 087 — Review-Fixes F10-Einstellungsdialog: weniger Lecks, kein Speichern pro Tastendruck, geprueftes Speichern

**Phase:** Review-Fixes (Paket "UI-DIALOGS") · Betrifft `src/ui/dialogs/*`,
`src/ui/config/*` und den neuen, rein logischen Helfer
`src/ui/config/ui_config_logic.h` (Host-Test: `tests/ui_config_logic_test.c`).

Ein vollstaendiger Code-Review hat im F10-Einstellungsfenster 18 Befunde gefunden
(Speicher-/GDI-Lecks, Speichern bei jedem Tastendruck, ueberschriebene
Einstellungen, ungepruefte Eingaben, ein Absturzrisiko bei langen Pfaden, ein
Wettlauf beim Beenden). Dieses Paket behebt sie, ohne das sichtbare Aussehen des
Dialogs zu aendern. Was sich fuer den Spieler aendert, steht je Fix unter
"Sichtbar anders?".

## Begriffe, kurz erklaert

- **Arbeitskopie (working copy):** Beim Oeffnen kopiert der Dialog `g_config` in
  eine eigene Kopie (`ui_cfg()` in `ui_config_state.c`). Alles, was man im Dialog
  aendert, landet zuerst nur dort. Erst **Save** schreibt es zurueck nach
  `g_config` und in `plugin.cfg` ("Commit").
- **GDI-Objekt:** Windows-Zeichenobjekt (Bitmap, Pinsel, Schrift, Icon). Jedes
  davon kostet einen Platz in einer begrenzten Tabelle pro Prozess (Standard
  10 000). Wer sie nicht freigibt ("Leck"), macht irgendwann **das ganze
  TeamSpeak** unfaehig zu zeichnen.
- **Invalid-Parameter-Handler:** Wenn eine "sichere" CRT-Funktion wie
  `wcscpy_s` merkt, dass das Ziel zu klein ist, ruft sie diesen Handler. Ohne
  eigenen Handler **beendet Windows den ganzen Prozess** — also TeamSpeak.
- **Dialog-Thread:** Der F10-Dialog hat einen eigenen Thread mit eigener
  Nachrichtenschleife. Dort darf **keine TS-API** aufgerufen werden; Aenderungen
  laufen ueber `config_modify` / `config_save` (beide thread-sicher).

## Ueberblick

| # | Befund | Dateien |
|---|---|---|
| 1 | GDI-Leck bei jedem Oeffnen (Hintergruende, Pfad-Box, Haken-Icon) | `ui_config_draw.c`, `ui_config_controls.c`, `ui_config_dialog.c` |
| 2 | Bilder werden bei jedem Zeichnen neu dekodiert | `ui_config_draw.c`, `ui_presets.c`, `ui_config_controls.c` |
| 3 | Jeder Tastendruck in einem Distanzfeld speichert alles | `ui_dynamic.c` |
| 4 | Commit ueberschreibt neuere Werte anderer Threads | `ui_config_state.c`, `ui_config_logic.h` |
| 5 | Cursor springt / Ziffern verdreht beim Tippen | `ui_dynamic.c` |
| 6 | In einer Zone werden Zonen-Distanzen als eigene gespeichert | `ui_config_command.c` |
| 7 | Save uebernimmt leere/ungueltige Distanzen als 0,5 m | `ui_config_command.c`, `ui_voice_presets.c`, `ui_config_logic.h` |
| 8 | Tasten-Aufnahme ueberlebt das Schliessen | `ui_config_controls.c`, `ui_config_dialog.c` |
| 9 | "Rename" benennt das falsche Preset um | `ui_config_command.c` |
| 10 | HUD-"Live-Vorschau" tut nichts | `ui_config_command.c`, `ui_config_state.c` |
| 11 | Hotkey-Thread wird bei jedem Save neu gestartet (Race beim Beenden) | `ui_voice_presets.c` |
| 12 | `showStatusMessage` verliert pro Aufruf einen DC | `ui_messages.c` |
| 13 | Dialog-Guard wird doppelt freigegeben, `ui_cfg_dialog_end` fehlt auf Fehlerpfaden | `ui_config_dialog.c` |
| 14 | Doppelte Steuerelement-IDs 2001-2004 | `ui_config_internal.h`, `ui_config_controls.c`, `ui_config_dialog.c`, `ui_config_draw.c` |
| 15 | Absturz (CRT-Abbruch) bei langen Pfaden | `ui_config_controls.c`, `ui_config_command.c`, `ui_path_steam.c`, `ui_config_dialog.c` |
| 16 | `voice_presets.cfg` unsicher geschrieben / ungeprueft gelesen | `ui_voice_presets.c`, `ui_config_logic.h` |
| 17 | Fensterklasse wird nie abgemeldet | `ui_config_dialog.c` |
| 18 | Compiler-Warnungen | `ui_config_dialog.c`, `ui_config_controls.c`, `ui_config_draw.c` |

## Neue Bausteine

**`src/ui/config/ui_config_logic.h`** (nur Header, keine Windows-Includes, daher
mit gcc testbar): `ui_wcs_copy` (begrenztes Kopieren), `ui_game_folder_fits`
(Pfadlaenge), `ui_dist_parse` (strenges Lesen einer Distanz),
`ui_preset_parse_*` (Zeilen von `voice_presets.cfg`), `ui_cfg_merge_*` (welche
`PluginConfig`-Felder dem Dialog gehoeren).

**Bild-Cache** in `ui_config_draw.c`: `ui_image_cache_get(resourceId)` dekodiert
eine Bitmap-Ressource beim ersten Gebrauch und gibt danach immer dasselbe Handle
zurueck; `ui_checkmark_icon_get()` genauso fuer das Haken-Icon;
`ui_image_cache_release()` gibt alles frei, **nachdem das Dialogfenster zerstoert
ist** (am Ende von `showConfigInterface`).

```
showConfigInterface()                       (Dialog-Thread)
  ui_cfg_dialog_begin()                     Arbeitskopie anlegen, HUD-Werte merken
  showConfigInterfaceRun()
     WM_CREATE ... Nachrichtenschleife ... WM_DESTROY
  ui_image_cache_release()                  Bilder/Icon/Hintergruende freigeben
  ui_cfg_dialog_end()                       nicht gespeicherte HUD-Vorschau zuruecknehmen
  return                                    (Guard gibt der AUFRUFER frei, s. Fix 13)
```

---

## 1. GDI-Leck bei jedem Oeffnen

**Was:** `ui_image_cache_release()` (neu) loescht nach dem Schliessen die drei
Kategorie-Hintergruende (`hBackgroundBitmap`, `hBackgroundAdvancedBitmap`,
`hBackgroundPresetsBitmap`), die Pfad-Box-Bitmap (`hPathBoxBitmap`) und das
Haken-Icon (`DestroyIcon`) und setzt die Globals auf `NULL`. Das doppelte
Vorladen von `hBackgroundBitmap` in `showConfigInterface` entfaellt.

**Vorher:** `WM_CREATE` lud diese Bilder bei **jedem** Oeffnen neu
(`LoadImageW`), niemand gab sie frei (`ui_release_config_dialog_gdi` kannte nur
Schriften und den Pinsel). Der Hintergrund wurde sogar zweimal geladen. Ein
Spieler, der F10 oft oeffnet, hat so Dutzende grosse Bitmaps im Speicher
gelassen.

**Warum besser:** Jedes geladene Objekt hat jetzt genau einen Besitzer und wird
genau einmal freigegeben — erst **nach** `WM_DESTROY` und dem Ende der
Nachrichtenschleife, wenn auch alle Kind-Fenster weg sind, die die Bitmaps noch
anzeigen koennten.

**Wie:** siehe Ablauf oben. `hPathBoxBitmap` ist nur noch ein Alias auf einen
Cache-Eintrag (der Cache loescht ihn, die Variable wird nur auf `NULL` gesetzt).

**Test:** Cross-Build ohne Warnung. *Manuell (offen):* F10 20x oeffnen/schliessen,
im Task-Manager (Spalte "GDI-Objekte" von TeamSpeak) darf die Zahl nicht pro
Oeffnen steigen.

**Lerneffekt:** Wer ein Windows-Objekt erzeugt, muss in derselben Funktionsfamilie
festlegen, **wer es wann loescht** — sonst leckt es still, bis etwas anderes
ausfaellt.

## 2. Bilder werden bei jedem Zeichnen neu dekodiert

**Was:** Alle Owner-Draw-Stellen (`DrawButtonWithBitmap`, Tasten-Boxen,
Load/Rename-Buttons, Set-Key-Buttons, Pfad-Box, OK/Cancel im Rename-Dialog, Haken
in `PresetLabelProc`) holen ihr Bild aus dem Bild-Cache. Die Pfad-Box ruft in
`WM_DRAWITEM` nicht mehr `findConanExilesAutomatic()` (Registry + `libraryfolders.vdf`
parsen) auf, sondern zeichnet `displayedPathText`.

**Vorher:** Jedes `WM_DRAWITEM` rief `LoadImageW` (Datei dekodieren, DIB-Section
anlegen) und danach `DeleteObject`; der Haken wurde bei jedem `WM_PAINT` jedes
Preset-Labels geladen und zerstoert. Im Auto-Modus las jedes Neuzeichnen der
Pfad-Box die Windows-Registry und parste eine Steam-Datei.

**Warum besser:** Ein Neuzeichnen kostet nur noch BitBlt + Text. Das Ergebnis auf
dem Bildschirm ist identisch.

**Wie:** `displayedPathText` wird dort gesetzt, wo sich der Pfad wirklich aendert:
beim Oeffnen (`WM_CREATE`; im Auto-Modus **einmal** `findConanExilesAutomatic`),
beim Umschalten der Checkbox "Automatic Patch Find", nach "Browse" und nach Save.
Der Auto-Zweig im Zeichencode war damit ueberfluessig.

**Test:** Cross-Build. *Manuell (offen):* Dialog oeffnen, Tabs wechseln, Fenster
ueber den Bildschirm ziehen — Aussehen unveraendert, kein Flackern; Auto-Modus:
Pfad-Box zeigt den Steam-Ordner bzw. "(Not found)".

**Lerneffekt:** Zeichencode (`WM_PAINT`, `WM_DRAWITEM`) laeuft sehr oft — dort
gehoert nichts hin, was Datei-/Registry-Zugriffe oder Dekodieren macht.

## 3. Jeder Tastendruck in einem Distanzfeld speichert alles

**Was:** `handleDistanceEditChange` (reagiert auf `EN_CHANGE`) ruft kein
`saveVoiceSettings()` mehr auf. Auch `updateDynamicInterface` speichert nicht mehr
still, wenn es eine Distanz an die Server-Grenzen anpasst.

**Vorher:** Wer "150" tippte, loeste drei Commits aus: dreimal `plugin.cfg`
schreiben, dreimal CEPOS neu senden, dreimal Audio neu berechnen. Weil der Commit
die **ganze** Arbeitskopie uebernahm, waren auch Tasten-Aufnahmen und Checkboxen
sofort gespeichert — **Cancel konnte nichts mehr verwerfen.**

**Warum besser:** Tippen aendert nur die Arbeitskopie und die Anzeige. Gespeichert
wird bei **Save** und beim Laden eines Presets (dort hiess die Meldung schon immer
"loaded and saved"). Cancel / Fenster schliessen verwirft jetzt wirklich.

**Wie:**

```
Tippen  -> EN_CHANGE -> handleDistanceEditChange -> Arbeitskopie (+ Anzeige)
Save    -> ui_cfg_commit -> config_modify + config_save + Nebenwirkungen
Cancel  -> Fenster zu -> Arbeitskopie weg (nichts wurde geschrieben)
```

Die Server-Anpassung in `updateDynamicInterface` bleibt in der Arbeitskopie; sie
wird mit Save gespeichert. Das Audio begrenzt ohnehin zur Laufzeit auf die
Hub-Grenzen (`voice_mode_get_distance`), dadurch geht nichts verloren.

**Sichtbar anders?** Ja, gewollt: Distanzen wirken erst nach **Save Configuration**
(vorher "zufaellig" schon beim Tippen).

**Test:** Cross-Build. *Manuell (offen):* Advanced Options, Whisper auf "150"
tippen, **Cancel**, F10 erneut oeffnen -> alter Wert. Dann nochmal tippen,
**Save** -> `plugin.cfg` zeigt den neuen Wert, nur ein Schreibvorgang.

**Lerneffekt:** "Bei jeder Eingabe speichern" und "Abbrechen soll verwerfen"
schliessen sich aus. Eine Arbeitskopie hilft nur, wenn nichts an ihr vorbei
committet.

## 4. Commit ueberschreibt neuere Werte anderer Threads

**Was:** `ui_cfg_commit()` kopiert nicht mehr die ganze Arbeitskopie nach
`g_config` (`config_apply`), sondern ruft `config_modify` mit einer Funktion auf,
die **nur die Dialog-Felder** uebernimmt (`ui_cfg_merge_dialog_fields`):
`savedPath`, `automaticPatchFind`, die drei Distanzen, die fuenf Hotkeys, die drei
Schalter (Muting, Kanalwechsel, Voice-Toggle) und `hudTheme/hudPosition/hudSize`.
**Nie** angefasst: `automaticSavedPath`, `defaultsAppliedServer`,
`enableVoiceOverlay`, `debugMode`, `enablePosFile`. Ausserdem entfaellt der Block,
der `currentVoiceMode` zurueckschrieb.

**Vorher:** Die Arbeitskopie ist ein Schnappschuss vom Oeffnen. Hat in der
Zwischenzeit der Server-Profil-Code `defaultsAppliedServer` gesetzt oder der
Pos-Watcher `automaticSavedPath` gefunden, **loeschte der Commit das wieder**:
die Server-Defaults wurden beim naechsten Verbinden erneut angewendet und
ueberschrieben die Einstellungen des Spielers. Der `currentVoiceMode`-Block konnte
nur einen Modus zuruecksetzen, den der Spieler per Hotkey bei offenem Dialog
gewechselt hatte (der Dialog selbst aendert den Modus nie).

**Warum besser:** Jeder Thread aendert nur noch seine eigenen Felder
(`config_modify` haelt die Config-Sperre waehrenddessen). Zwei gleichzeitige
Schreiber koennen einander nicht mehr zuruecksetzen.

**Wie:**

```
Dialog-Thread                         g_config (unter Config-Lock)
  Save  -> config_clamp(Arbeitskopie)
        -> config_modify(merge)  ---> nur Dialog-Felder ueberschrieben
        -> config_save()
Server-Profil-Thread                  defaultsAppliedServer  bleibt erhalten
Pos-Watcher-Thread                    automaticSavedPath     bleibt erhalten
```

`automaticSavedPath` wird bewusst **nicht** kopiert. Folge (harmlos): schaltet der
Spieler erstmals von manuell auf automatisch um und `automaticSavedPath` war leer,
traegt der Pos-Watcher den Pfad innerhalb von 15 s (`POS_AUTODETECT_RETRY_MS`)
nach; bis dahin liest er Pos.txt vom manuellen Pfad.

**Test:** `tests/ui_config_logic_test.c` Abschnitt [5] (18 Pruefungen): mit
Muster-Fuellung (0x11 vs. 0x22) wird bewiesen, dass genau die Dialog-Felder
kopiert werden und `automaticSavedPath`/`defaultsAppliedServer`/`debugMode`/
`enableVoiceOverlay`/`enablePosFile` unveraendert bleiben.

**Lerneffekt:** Ein Schnappschuss darf nie als Ganzes zurueckgeschrieben werden,
wenn andere mitschreiben — nur die Felder, die einem gehoeren.

## 5. Cursor springt / Ziffern verdreht

**Was:** `updateDynamicInterface` schreibt die drei Distanzfelder nur noch dann neu
(`ui_set_distance_edit_text`), wenn das Feld **nicht den Fokus hat** und sein Text
sich vom gewuenschten `%.1f`-Text **unterscheidet**.

**Vorher:** Nach jedem akzeptierten Tastendruck wurden alle drei Felder mit
`SetWindowTextW` ueberschrieben — auch das Feld, in dem gerade getippt wurde
(aus "15" wurde "15.0", der Cursor sprang ans Ende, weitere Ziffern landeten an
der falschen Stelle).

**Warum besser:** Das Feld, das der Spieler bearbeitet, wird in Ruhe gelassen.

**Wie:** `GetFocus() == hEdit` -> nicht anfassen; sonst Text vergleichen
(`wcscmp`) und nur bei Unterschied `SetWindowTextW`.

**Test:** Cross-Build. *Manuell (offen):* In Whisper "1", "5", "0" tippen — Cursor
bleibt, Text bleibt "150" (wird erst beim Verlassen/naechsten Update zu "150.0").

**Lerneffekt:** Programmatisch den Text eines Feldes aendern, in dem der Nutzer
gerade tippt, zerstoert die Eingabe.

## 6. In einer Zone werden Zonen-Distanzen als eigene gespeichert

**Was:** `ui_collect_distances_for_save()` (neu, `ui_config_command.c`) liefert
im Zonen-Modus (`currentZoneIndex != -1`) die **persoenlichen** Distanzen aus der
Arbeitskopie zurueck, nicht den Text der Felder.

**Vorher:** In einer Zone zeigen die Felder die Distanzen der Zone. Save las den
Feldtext und speicherte die Zonen-Werte als die eigenen Distanzen des Spielers —
nach dem Verlassen der Zone galten sie weiter.

**Warum besser (konservativ):** Save im Zonen-Modus aendert die persoenlichen
Distanzen nicht mehr. Was der Spieler in einer Zone in ein Feld tippt, bleibt in
der Arbeitskopie (der Tipp-Handler schreibt dorthin) und wird so mitgespeichert —
das Verhalten ausserhalb von Zonen ist unveraendert.

**Test:** Cross-Build. *Manuell (offen, braucht Server mit Zone):* in einer Zone
Save klicken, Zone verlassen -> Distanzfelder zeigen wieder die alten eigenen Werte.

**Lerneffekt:** Eine Anzeige ist nicht automatisch die Wahrheit — wenn ein Feld
je nach Situation etwas anderes zeigt, darf Speichern nicht blind das Feld lesen.

## 7. Save uebernimmt leere/ungueltige Distanzen

**Was:** `ui_collect_distances_for_save()` prueft alle drei Felder mit
`ui_dist_parse` (streng: Zahl, endlich, `> 0`, und innerhalb der Hub-Grenzen
min/max, sofern gesetzt). Bei einem Fehler: rote Statusmeldung (z. B. "Error:
Whisper distance is not a valid number"), Piepton, Fokus auf das Feld —
**es wird nichts ueberschrieben und nichts gespeichert.** Dieselbe Pruefung nutzt
der (unerreichbare) Zweig `case 12`; `writeFullConfiguration` uebernimmt einen
nicht lesbaren Wert nicht mehr.

**Vorher:** `_wtof` machte aus "", "abc" oder "1x" still `0` bzw. eine Teilzahl;
`config_clamp` machte daraus 0,5 m — und die Meldung lautete "saved
successfully". "nan" kam sogar durch `config_clamp` (Vergleiche mit NaN sind
falsch).

**Warum besser:** Fehler werden dem Spieler gesagt, statt Daten still zu
verfaelschen. Der Parser ist unabhaengig von der Windows-Spracheinstellung
(Punkt **und** Komma als Dezimaltrenner), liest also auch den `%.1f`-Text, den der
Dialog selbst schreibt.

**Wie:** `ui_dist_parse(text, limitsActive, hubMin, hubMax, &out)` -> `UI_DIST_OK`
oder einer von `EMPTY / NOT_NUMBER / NOT_POSITIVE / OUT_OF_RANGE`. Als "gesetzt"
gilt eine Grenze nur, wenn sie `> 0` ist (gleiche Regel wie
`voice_mode_get_distance`).

**Test:** `tests/ui_config_logic_test.c` Abschnitte [3a]-[3c] (47 Pruefungen):
"", "abc", "nan", "inf", "1e5", "0x10", "2.5.1", "0", "-5", 100 Ziffern, Grenzen
inkl. Float-Randfaelle (Text gleich einer `float`-Grenze wird akzeptiert).

**Lerneffekt:** Eingaben aus Textfeldern immer **validieren, nicht "irgendwie
umwandeln"** — `atof`-artige Funktionen kennen keinen Fehler.

## 8. Tasten-Aufnahme ueberlebt das Schliessen

**Was:** `isCapturingKey` und `captureKeyTarget` werden in `WM_CREATE`
(`ui_config_on_create`) und `WM_DESTROY` (`ConfigDialogProc`) zurueckgesetzt.

**Vorher:** Klick auf "Set Key", dann Dialog schliessen: das Flag blieb gesetzt.
Beim naechsten Oeffnen band der erste gedrueckte Key (meist **F10**, mit dem man
den Dialog oeffnet) sofort an die alte Zieltaste.

**Warum besser:** Jeder Dialog startet ohne Altlasten.

**Test:** Cross-Build. *Manuell (offen):* "Set Key" klicken, Dialog mit X schliessen,
mit F10 neu oeffnen -> keine Taste wurde gebunden, "Set Key"-Buttons aktiv.

**Lerneffekt:** Zustand, der ueber die Lebensdauer eines Fensters hinaus lebt
(Globals), muss beim Erzeugen **und** Zerstoeren zurueckgesetzt werden.

## 9. "Rename" benennt das falsche Preset um

**Was:** In `ui_config_command.c` wird `renamePresetIndex` nur noch gesetzt, wenn
der Rename-Dialog wirklich **neu** erzeugt wird. Ist er schon offen, kommt er nur
in den Vordergrund (`SetForegroundWindow`).

**Vorher:** Der Index wurde vor der Pruefung gesetzt. Der Rename-Dialog ist nicht
modal: ein zweiter Klick auf "Rename" bei einem anderen Preset aenderte das Ziel
des bereits offenen Dialogs, "OK" benannte dann das falsche Preset um.

**Warum besser:** Das Ziel ist fest, solange der Dialog offen ist.

**Test:** Cross-Build. *Manuell (offen):* Rename bei Preset 1 klicken, ohne zu
schliessen Rename bei Preset 3 klicken -> der offene Dialog zeigt weiter Preset 1.

**Lerneffekt:** Globale "aktuelles Ziel"-Variablen nur dort setzen, wo man das
Ziel auch wirklich uebernimmt.

## 10. HUD-"Live-Vorschau" tat nichts

**Was:** Die HUD-Combos (Thema/Position/Groesse) schreiben ueber
`ui_cfg_hud_preview()` (neu, `ui_config_state.c`) die HUD-Werte per `config_modify`
nach `g_config` (**ohne** Datei zu schreiben) und rufen danach
`voice_overlay_refresh_theme/position/size`. **Cancel / Fenster schliessen ohne
Save** setzt die HUD-Werte vom Oeffnen zurueck (`ui_cfg_dialog_end`); **Save**
macht sie dauerhaft.

**Vorher:** Die Combo-Handler schrieben nur in die Arbeitskopie, das Overlay liest
aber `g_config.hud*` — die "Vorschau" blieb unsichtbar; erst nach Save sah man die
Aenderung (das HUD aktualisierte sich also nie live).

**Warum besser:** Der Spieler sieht die Wirkung sofort, kann sie aber noch
verwerfen.

**Wie:**

```
Combo gewaehlt -> Arbeitskopie.hud*  -> config_modify (nur hudTheme/Position/Size)
              -> voice_overlay_refresh_*   (kein config_save!)
Save  : commit merkt die HUD-Werte als "gespeichert"
Cancel: ui_cfg_dialog_end vergleicht g_config.hud* mit "gespeichert" und
        stellt bei Abweichung zurueck + frischt das Overlay auf
```

**Test:** Cross-Build; Merge-Logik in `ui_config_logic_test.c` [5]. *Manuell
(offen, HUD sichtbar nur im Ingame-Kanal bzw. mit Debug):* Thema auf "Blue" ->
HUD wird sofort blau; **Cancel** -> wieder alte Farbe; erneut aendern + **Save**,
Dialog schliessen -> bleibt blau.

**Lerneffekt:** Eine "Vorschau" muss dort wirken, wo der Verbraucher liest —
und einen definierten Rueckweg haben.

## 11. Hotkey-Thread wurde bei jedem Save neu gestartet

**Was:** In `writeFullConfiguration` entfaellt das Paar
`removeKeyMonitoring(); installKeyMonitoring();`.

**Vorher:** Jeder Save, jedes Preset-Laden und jedes Oeffnen mit Auto-Find stoppte
den Hotkey-Thread (Join, bis zu 5 s Wartezeit auf dem Dialog-Thread) und startete
einen neuen. Laeuft das Beenden des Plugins gleichzeitig, konnte der Thread **nach**
dem Join erneut gestartet werden und lief in entladenen Code.

**Warum besser:** Der Neustart war nie noetig: `key_watcher.c` liest
`g_config.configUIKey` in jedem Schleifendurchlauf und `voice_mode_hotkey_poll`
macht pro Tick ein `config_copy` — neue Tasten wirken von allein.

**Test:** Code-Lesen der beiden Stellen (siehe Begruendung); Cross-Build.
*Manuell (offen):* Whisper-Taste aendern, Save -> neue Taste funktioniert sofort
ohne TS-Neustart; Plugin waehrend offenem Dialog entladen -> kein Absturz.

**Lerneffekt:** Wenn ein Thread seine Einstellungen ohnehin ständig neu liest,
braucht man ihn nicht neu zu starten — und jeder Start/Stop ist ein Race-Risiko.

## 12. `showStatusMessage` verlor pro Aufruf einen DC

**Was:** Die beiden Zeilen `SendMessage(hStatusMessage, WM_CTLCOLORSTATIC,
(WPARAM)GetDC(hStatusMessage), ...)` sind entfernt.

**Vorher:** Jeder Aufruf holte mit `GetDC` einen Geraetekontext und gab ihn nie
mit `ReleaseDC` zurueck — und schickte die Nachricht an das Label selbst statt an
seinen Eltern-Dialog. Wirkung: keine; Kosten: ein Leck pro Statusmeldung.

**Warum besser:** Die Farbe (rot bei Fehler, gruen bei Erfolg) bestimmt der
`WM_CTLCOLORSTATIC`-Handler des Dialogs aus dem **Text** der Meldung, wenn das
Label gezeichnet wird. Das passiert weiter (`InvalidateRect` + `UpdateWindow`
bleiben). Es geht kein sichtbarer Effekt verloren.

**Test:** Code-Lesen des Handlers (`controlId == 600`); Cross-Build. *Manuell
(offen):* Fehler provozieren -> rote Meldung; Save -> gruene Meldung.

**Lerneffekt:** `GetDC` und `ReleaseDC` gehoeren in dieselbe Funktion — oder der
Aufruf ist ueberfluessig.

## 13. Dialog-Guard doppelt freigegeben / `ui_cfg_dialog_end` fehlt

**Was:** `showConfigInterface()` ruft **nie** mehr `config_dialog_close()` auf. Alle
Arbeit nach `ui_cfg_dialog_begin()` steckt in `showConfigInterfaceRun()`; danach
laufen **immer** `ui_image_cache_release()` und `ui_cfg_dialog_end()` — auch wenn
COM, `RegisterClassW` oder `CreateWindowExW` fehlschlagen.

**Vorher:** Der Guard (`config_dialog_try_open/close`) wurde in
`showConfigInterface` **und** im Aufrufer (`settings_dialog_thread`) freigegeben.
Nach dem ersten Freigeben konnte F10 einen zweiten Dialog starten, dessen Guard
das zweite `close` des ersten dann loeschte — ein dritter Dialog war moeglich.
Auf den drei Fehlerpfaden fehlte `ui_cfg_dialog_end()`: `ui_cfg()` lieferte danach
fuer immer die veraltete Arbeitskopie.

**Warum besser:** Genau ein Freigeber (`settings_dialog_thread` in
`key_watcher.c`, Vertrag C7) und garantiertes Aufraeumen ueber einen einzigen
Austrittspunkt.

**Test:** Cross-Build; Code-Lesen aller Rueckgabepfade. *Manuell (offen):* F10
mehrfach schnell druecken -> es oeffnet sich genau ein Dialog; nach dem Schliessen
oeffnet F10 wieder genau einen.

**Lerneffekt:** "Wer es erworben hat, gibt es frei" — der Besitzer einer Sperre ist
ihr einziger Freigeber.

## 14. Doppelte Steuerelement-IDs 2001-2004

**Was:** Die klickbaren Labels neben den Checkboxen haben jetzt eigene IDs
(`IDC_LABEL_AUTOPATCH 2010`, `..._DISTMUTING 2011`, `..._CHANSWITCH 2012`,
`..._VOICETOGGLE 2013` in `ui_config_internal.h`); `ShowCategoryControls` sucht sie
dort. 2001-2005 bleiben den Tasten-Boxen. Das "Automatic Patch Find"-Label ist
nicht mehr `WS_DISABLED` und bekommt seine graue Farbe vom
`WM_CTLCOLORSTATIC`-Handler.

**Vorher:** Labels und Tasten-Boxen benutzten dieselben IDs; `GetDlgItem(2001)`
konnte je nach Erstellungsreihenfolge das falsche Fenster liefern, die Anzeige-/
Versteck-Listen erreichten die Labels nicht verlaesslich, und ein Klick auf den
Label-Text schaltete die Checkbox nicht um. Das "Automatic Patch Find"-Label war
zusaetzlich deaktiviert — ein deaktiviertes Fenster bekommt keine Mausklicks.

**Warum besser:** Eine ID = ein Fenster. Klick auf den Label-Text schaltet die
Checkbox (`CheckboxLabelProc`).

**Sichtbar anders?** Nein: Positionen und Texte sind gleich; die Labels neben den
Advanced-Checkboxen haben leeren Text (der Text ist im Hintergrundbild), das
Patch-Label bleibt grau.

**Test:** Cross-Build. *Manuell (offen):* In Advanced auf den Text "Enable
distance-based muting" klicken -> Haken schaltet um; im Patch-Tab auf
"Automatic Patch Find" klicken -> Haken schaltet um.

**Lerneffekt:** IDs innerhalb eines Fensters eindeutig halten und als benannte
Konstanten fuehren statt als Zahlen quer durch den Code.

## 15. Absturz bei langen Pfaden

**Was:** Statt `wcscpy_s`/`wcscat_s`/`strcat_s` auf feste `MAX_PATH`-Puffer nutzen
die betroffenen Stellen `ui_wcs_copy` (schneidet ab statt abzubrechen) bzw. pruefen
vorher `ui_game_folder_fits`:
`ui_config_controls.c` (4 Stellen beim Lesen von `plugin.cfg`),
`ui_config_command.c` (Save im manuellen Modus haengt `\ConanSandbox\Saved` an),
`ui_path_steam.c` (Ordner-Auswahl), `ui_config_dialog.c` (`Pos.txt`-Pfad beim
Oeffnen), `ui_voice_presets.c` (`writeFullConfiguration`). Zu lange Pfade geben
eine rote Statusmeldung ("Game path is too long" / "Selected path is too long")
statt eines Absturzes.

**Vorher:** Es ist kein Invalid-Parameter-Handler installiert. Ein Pfad ab etwa
240 Zeichen (oder eine laengere Zeile in `plugin.cfg`) liess die CRT den Prozess
beenden — TeamSpeak war weg.

**Warum besser:** Eingaben, die der Nutzer (oder eine handeditierte Datei) steuert,
duerfen nie einen Prozessabbruch ausloesen.

**Test:** `ui_config_logic_test.c` [1]+[2] (18 Pruefungen): 400 Zeichen in einen
260-Puffer -> 259 Zeichen, kein Ueberlauf (Waechterwoerter), 240 Zeichen +
`\ConanSandbox\Saved` = genau 259. Zusaetzlich unter AddressSanitizer gelaufen.
*Manuell (offen):* Ordner mit sehr langem Pfad waehlen -> Meldung statt Absturz.

**Lerneffekt:** `_s`-Funktionen sind "sicher" nur im Sinne von "brechen ab" —
bei Nutzereingaben vorher die Laenge pruefen oder abschneidend kopieren.

## 16. `voice_presets.cfg` unsicher geschrieben und ungeprueft gelesen

**Was:** *Schreiben* (`savePresetsToConfigFile`): erst nach `voice_presets.cfg.tmp`,
alle Schreib-/`fflush`-/`fclose`-Ergebnisse pruefen, dann `MoveFileExW(
MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`; bei Fehler bleibt die alte
Datei. *Lesen* (`loadPresetsFromConfigFile`): `[PresetN]`-Kopf streng pruefen
(`ui_preset_parse_header`, Index 0..9), Distanzen auf 0,5..500 begrenzen und
`nan`/Text ignorieren, Tasten nur 1..255, `CurrentPreset` -1..9. Das Dateiformat
bleibt unveraendert.

**Vorher:** Die Datei wurde direkt mit `"w"` geoeffnet (leert sie sofort) —
Absturz oder volle Platte mitten im Schreiben vernichtete **alle** Presets;
Fehler wurden nie geprueft. Beim Lesen wurde ein kaputter Kopf (`[PresetX]`) mit
`_wtoi` zu Preset 0 und ueberschrieb es; Werte wurden nicht geprueft.

**Warum besser:** Es gibt immer eine vollstaendige Datei (alt oder neu), und
beschaedigte Zeilen koennen keine guten Presets mehr ueberschreiben.

**Test:** `ui_config_logic_test.c` [4a]-[4c] (45 Pruefungen), darunter die
Simulation einer beschaedigten Datei: `[PresetBROKEN]` ueberschreibt Preset 0
nicht, `nan`/Taste 300 werden ignoriert. *Manuell (offen):* Preset speichern,
`voice_presets.cfg` pruefen; eine Zeile `Whisper=abc` einfuegen, F10 oeffnen ->
Preset behaelt seinen Standardwert.

**Lerneffekt:** Dateien ersetzt man atomar (tmp + rename) und liest sie, als
koennte jemand Unsinn hineingeschrieben haben.

## 17. Fensterklasse wird nie abgemeldet

**Was:** `showConfigInterfaceRun` ruft nach Ende der Nachrichtenschleife (und beim
Fehler nach `CreateWindowExW`) `UnregisterClassW` auf. Das bisherige "erst
abmelden, dann anmelden" bleibt.

**Vorher:** `ModernConfigClass` blieb bis zum Ende von TeamSpeak registriert (mit
dem Modul-Handle der TS-Exe).

**Warum besser:** Das Plugin hinterlaesst nichts Registriertes mehr; sicher, weil
das Fenster zu diesem Zeitpunkt zerstoert ist.

**Test:** Cross-Build. *Manuell (offen):* Dialog mehrfach oeffnen/schliessen,
Plugin entladen -> kein Absturz.

**Lerneffekt:** Jedes `Register...` braucht sein `Unregister...` am selben
Lebensende.

## 18. Compiler-Warnungen

**Was:** `%08X` mit `HRESULT` -> `%08lX` + `(unsigned long)`; Schleife in
`ShowCategoryControls` mit `size_t`; unbenutzte Variable `foundAutomaticPatchFind`
entfernt; die doppelte lokale `ctrlId` in `ui_config_on_drawitem` entfernt.

**Test:** Cross-Compile jeder angefassten `.c` mit `-Wall -Wextra` (und zusaetzlich
`-Wshadow`): 0 Warnungen.

**Lerneffekt:** Warnungen sind billige, fruehe Bugmelder.

---

## Wie wurde das Ganze getestet?

- **Host-Unit-Test** `tests/ui_config_logic_test.c` (128 Pruefungen, auch unter
  AddressSanitizer/UBSan): begrenztes Kopieren, Pfadlaenge, Distanz-Parser,
  `voice_presets.cfg`-Validierung, Feld-Besitz beim Commit.
  Lauf: `bash tests/run_tests.sh` (Suite `ui_config_logic_test`, siehe
  `doku/03-build-und-tests.md`).
- **Cross-Compile** (MinGW) jeder angefassten `.c`: 0 Warnungen.
- **TS-Client-Test noetig** (Windows, nicht in dieser Umgebung moeglich): siehe die
  "Manuell (offen)"-Schritte bei den einzelnen Fixes.

## Bewusst nicht geaendert / Beobachtungen

- Der Rename- und der Preset-Speichern-Dialog melden ihre Fensterklasse bei jedem
  Oeffnen mit einem neuen `CreateSolidBrush` an (kleines GDI-Leck pro Oeffnen,
  nicht Teil dieses Reviews) — Kandidat fuer ein Folgepaket.
- Die Statusmeldungen und Texte der alten Logik (`ui_messages.c`) sind unveraendert.
