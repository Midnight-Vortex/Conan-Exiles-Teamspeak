# 091 — Reichweiten wahlweise in Conan-Fundamenten statt Metern (`ShowRangesInFoundations`, Root-Zwang `ForceRangesInFoundations`)

**Phase:** Feature · Betrifft den F10-Einstellungsdialog (Tab "Plugin Settings"), zwei
Chat-Zeilen, den Root-Parser und die Config. **Das Spiel-Mod ist nicht betroffen.**
Neuer, rein logischer Helfer: `src/core/config/range_units.h` (Host-Test:
`tests/range_units_test.c`).

## 1. Was wurde geaendert?

Spieler und Server-Admins koennen Sprachreichweiten in **Fundamenten** statt in Metern
sehen und eingeben. **1,0 = eine Fundament-Kantenlaenge**, 6,5 = sechseinhalb Fundamente.

1. **Neue Checkbox** "Show ranges in foundations" im Block "Plugin Features" des Tabs
   "Plugin Settings" (rechte Spalte der ersten Zeile, rechts von "Enable distance-based
   muting"). Haken an = der Dialog zeigt und liest alle Reichweiten in Fundamenten.
2. **Neuer Config-Schluessel** `ShowRangesInFoundations=true|false` in `plugin.cfg`
   (Standard `false`). Er steht direkt hinter `DistanceShout`, alle anderen Schluessel
   behalten Namen und Reihenfolge.
3. **Neuer Root-Schalter** `ForceRangesInFoundations=True` im `[GLOBAL]`-Teil der
   Root-Kanal-Beschreibung (Standard `False`): Der Server **erzwingt** die Anzeige in
   Fundamenten. **Dann stehen auch alle Reichweiten der Root-Beschreibung in Fundamenten**
   (siehe Abschnitt 6).
4. **Zwei Chat-Zeilen** zeigen die Fundamente zusaetzlich: der Moduswechsel
   (`... Distance: 5.9 foundations (15.0 m)`) und die Hoerbonus-Zeile der Rasse
   (`... (listen bonus +0.8 foundations (2.0 m))`).
5. **Die Ueberschrift im Hintergrundbild** `assets/Background_Plugin_Settings.bmp` hiess
   "Voice Range Settings (meters)". Das **"(meters)" wurde aus dem Bild entfernt**
   (Handarbeit des Auftraggebers, nicht Teil des Codes) — die Einheit steht jetzt in
   den Statuszeilen unter den Feldern, nicht mehr fest im Bild.

| Datei | Aenderung |
|---|---|
| `src/core/config/range_units.h` (**neu**) | die **eine** Konstante `RANGE_FOUNDATION_EDGE_M`, Umrechnung, Grenzen-Rundung, "hat der Spieler die Zahl veraendert?", Einheiten-Namen |
| `src/core/config/config.h`, `config.c`, `config_text.h` | Feld `rangeInFoundations`; lesen (`ShowRangesInFoundations`), Standard aus, schreiben, auf 0/1 normieren |
| `src/core/hub/hub_parser.c/.h` | Schluessel `ForceRangesInFoundations` + Umrechnung der Admin-Werte in Meter am Ende des Parsens; alle Abschnitte lesen jetzt `Whisper` **und** `Wisper` (Abschnitt 6) |
| `src/ts/profile/ts3_server_profile.c/.h` | `server_profile_force_ranges_in_foundations()` (lock-frei), Hoerbonus-Chat-Zeile |
| `src/core/voice/voice_modes.c` | Moduswechsel-Chat-Zeile |
| `src/plugin.h`, `src/ui/plugin_ui_compat.c`, `src/plugin_modules.h` | Spiegel `hubForceRangesInFoundations`, Handle `hRangeUnitCheck`, `ui_range_unit_changed()` |
| `src/ui/config/ui_config_state.c/.h` | `ui_ranges_in_foundations()`, `ui_ranges_forced_by_server()`, `ui_range_unit_refresh()`, `ui_range_text()` |
| `src/ui/config/ui_config_logic.h` | `ui_dist_parse_display()` (Save-Lesen mit Einheit + Drift-Schutz), `rangeInFoundations` gehoert dem Dialog (Commit) |
| `src/ui/dialogs/ui_config_internal.h` | `IDC_LABEL_RANGEUNIT` (2014), `IDC_CHECK_RANGEUNIT` (205) |
| `ui_config_controls.c`, `ui_config_dialog.c`, `ui_config_draw.c` | Checkbox + Label anlegen, in die Tab-Listen, Label zeichnet seinen Text selbst (weiss) |
| `ui_dynamic.c`, `ui_messages.c`, `ui_config_command.c`, `ui_presets.c`, `ui_voice_presets.c` | jede Stelle, die eine Reichweite anzeigt oder liest, nutzt die Anzeige-Einheit |
| `tests/range_units_test.c` (**neu**), `config_text_test.c`, `ui_config_logic_test.c`, `hub_parser_test.c`, `server_profile_test.c`, `voice_modes_test.c`, `run_tests.sh` | Tests |

## 2. Wie war es vorher?

Alles war in Metern: Dialog, Statuszeilen, Preset-Liste, Chat. Wer in Fundamenten denkt
(Basenbau), musste im Kopf umrechnen; Server-Admins mussten ihre Reichweiten in Metern
in die Root-Beschreibung schreiben und konnten den Spielern keine andere Anzeige geben.

## 3. Warum ist die neue Loesung besser?

- **Gespeichert wird weiter in Metern — ueberall.** `plugin.cfg`, `voice_presets.cfg`,
  die Hub-Grenzen, CEPOS (was die anderen Spieler von dir erhalten), die Audio-Rechnung:
  alles unveraendert. Nur **Anzeige und Eingabe** im Dialog und zwei Chat-Zeilen
  rechnen um. Es kann daher keine Mischung "mal Meter, mal Fundamente" in den Daten
  geben, und aeltere/andere Plugin-Versionen lesen dieselben Dateien.
- **Wer den Haken nie setzt, sieht exakt das bisherige Verhalten** (gleiche Zahlen,
  gleiche Texte, gleiche Dateien). Kleine Ausnahme: `plugin.cfg` bekommt eine neue Zeile
  `ShowRangesInFoundations=false`.
- **Eine Konstante, eine Quelle.** `RANGE_FOUNDATION_EDGE_M = 2.56f` steht nur in
  `range_units.h`; alle Stellen (Dialog, Chat, Root-Parser) benutzen sie.
- **Kein stilles Verschieben von Werten** (Abschnitt 5, "Kein Drift").

## 4. Wie funktioniert es?

### 4.1 Warum 2,56 m?

Aus dem Conan-Exiles-DevKit: Die `BuildSockets` von `BP_BuildFoundation`, `_T2`, `_T3` und
`BP_HL_BuildFoundation_T3` liegen bei X/Y = ±128 um die Mitte des Bauteils. Zwei
benachbarte Fundamente sind also genau **256 Unreal-Einheiten** (1 UU = 1 cm) = **2,56 m**
voneinander entfernt — fuer alle Stufen gleich. Die Meshes sehen groesser aus (daher kommt
die "300" aus dem Internet), aber das **Raster** ist das, was Spieler beim Bauen zaehlen.

### 4.2 Umrechnung

| Meter | Fundamente (Anzeige, 1 Nachkommastelle) | | Fundamente | Meter |
|---:|---:|---|---:|---:|
| 2 m | 0,8 (genau 0,78) | | 1 | 2,56 m |
| 15 m | 5,9 (genau 5,86) | | 6 | 15,36 m |
| 40 m | 15,6 (genau 15,63) | | 16 | 40,96 m |
| 2,56 m | 1,0 | | 6,5 | 16,64 m |

Die Anzeige hat **eine** Nachkommastelle (wie bisher bei Metern). 0,1 Fundament = 0,256 m
ist **groeber** als die 0,1 m, die `plugin.cfg` speichert — eine eingetippte Zahl
(z. B. 5,9) ueberlebt deshalb "Fundamente → Meter → Datei (0,1 m) → Fundamente"
unveraendert (im Test fuer alle 0,1-Schritte bis 500 m geprueft).

### 4.3 Datenfluss

```
 eingetippt / angezeigt            gespeichert + gerechnet
 (Fundamente ODER Meter)            (IMMER Meter)
 ┌──────────────┐  range_to_meters  ┌────────────────────────────┐
 │ Feld im      │ ────────────────► │ Arbeitskopie ui_cfg()      │ ─Save─► g_config ─► plugin.cfg
 │ F10-Dialog   │ ◄──────────────── │ distanceWhisper/Normal/..  │         CEPOS, Audio, Hub-Clamp
 └──────────────┘  range_from_meters└────────────────────────────┘
 Chat-Zeilen: nur Anzeige (range_from_meters), nichts wird zurueckgerechnet.
```

**Wer entscheidet die Anzeige-Einheit?** `range_effective_unit(eigene Wahl, Server-Zwang)`:
Haken des Spielers **oder** `ForceRangesInFoundations=True` des Servers. Der Zwang wird
**nie** in die Config geschrieben (siehe 4.6).

### 4.4 Der Haken im Dialog

- Position: Haken `(330,145,20,20)` (ID 205), Text-Label `(355,147,210,20)`
  (`IDC_LABEL_RANGEUNIT` = 2014). Die vier Checkboxen des Tabs sind damit 3 links
  (Zeilen y = 145 / 165 / 185) + 1 rechts auf Zeile 1. Kein anderes Element wurde verschoben.
- Anders als die drei bestehenden Labels (deren Text **im Hintergrundbild** steht, das Label
  hat leeren Text) steht der Text dieses Labels **nicht** im Bild: das Label zeichnet ihn
  selbst — weiss (`RGB(255,255,255)`, wie die gebackenen Texte), transparenter Hintergrund,
  Schrift `hFont` (`ui_config_on_ctlcolorstatic`, gleicher Zweig wie die HUD-Labels 526-528).
- Klick auf den Text schaltet den Haken (gleiche `CheckboxLabelProc` wie die anderen),
  gleiches Haken-Icon (`BM_SETIMAGE`).
- Das Umschalten arbeitet nur auf der **Arbeitskopie** (`ui_cfg()->rangeInFoundations`):
  **kein** Dateischreiben beim Klick; **Save** speichert (Feld gehoert zu den
  Dialog-Feldern in `ui_cfg_merge_dialog_fields`), **Cancel** verwirft — wie jede andere
  Dialog-Einstellung (Aenderung 087). Der Dialog zeigt sofort alles neu an
  (`ui_range_unit_changed()`): die drei Felder, die Statuszeilen, spaeter die Preset-Liste.
  Dabei wird nichts zurueckgelesen — nur die Meter der Arbeitskopie werden neu angezeigt.

### 4.5 Jede Stelle, die eine Reichweite zeigt oder liest

| Stelle | Was dort passiert |
|---|---|
| Startwerte der 3 Felder (`ui_config_controls.c`) | `ui_range_text(Meter)` schreibt "%.1f" in der Anzeige-Einheit |
| Tippen in ein Feld (`handleDistanceEditChange`, `ui_dynamic.c`) | Text = Anzeige-Einheit. Die bestehenden Tipp-Filter (`shouldValidateValue`, `validateDistanceValue`) laufen in der Anzeige-Einheit mit umgerechneten Hub-Grenzen; gespeichert werden **Meter**; korrigierter Text wird in Anzeige-Einheit geschrieben |
| Felder neu schreiben (`updateDynamicInterface`) | wie bisher (Fokus-Feld bleibt in Ruhe), aber Anzeige-Einheit; Zonen-Werte (in der Zone) werden ebenfalls umgerechnet |
| Hub-Grenzen-Korrektur (`ui_correct_to_hub_limits`) | rechnet in Metern; in Fundamenten landet eine Korrektur auf der **angezeigten** Grenze, damit Feld, Statuszeile und Wert uebereinstimmen |
| Save (`ui_collect_distances_for_save` + `ui_dist_parse_display`) | Anzeige-Text → Meter, Hub-Grenzen in der Anzeige-Einheit geprueft, Fehlertexte nennen die Einheit (" foundations"; bei Metern bleibt der Text wie vorher) |
| Statuszeilen (`ui_messages.c`) | "Whisper: 5.9 foundations (Valid range: 0.9-5.8)"; die Woerter "Valid range", "Free range", "Auto-corrected" bleiben — `ui_config_on_ctlcolorstatic` waehlt die Farbe danach. Vergleiche (zu klein / zu gross) laufen weiter in Metern |
| Preset-Laden (`loadVoicePreset`) | Presets sind Meter; die Felder zeigen die Anzeige-Einheit; `writeFullConfiguration` bekommt weiter **Meter-Texte** |
| Preset-Liste (`ui_presets.c`) | "W:2.0 N:10.0 S:15.0 m" bzw. "W:0.8 N:3.9 S:5.9 fnd." (Kurzform `fnd.`); Label von 300 auf 325 px verbreitert (bis zum Load-Button bei x=370), damit der laengere Suffix passt; ebenso die Zahlen im Preset-Speichern-Dialog |
| Chat bei Moduswechsel (`voice_modes.c`) | Meter: wie bisher `... Distance: 15.0 m`. Fundamente: `... Distance: 5.9 foundations (15.0 m)` |
| Chat Hoerbonus (`ts3_server_profile.c`) | Meter: wie bisher `(listen bonus +2.0 m)`. Fundamente: `(listen bonus +0.8 foundations (2.0 m))` |

Nicht umgestellt (bewusst): Log-Zeilen (bleiben Meter), die "MinDist/MaxDist"-Zeile des
Positional-Audio-Hinweises (das sind Parameter des Audio-Modells, keine Sprachreichweiten),
`updateDistanceMessage()` und `showDynamicDistanceLimitMessage()` (haben keinen Aufrufer bzw.
schreiben nur ins Debug-Log).

### 4.6 Der Server-Zwang (`ForceRangesInFoundations=True`)

```
Root-Beschreibung ──parse──► HubSettings.forceRangesInFoundations
        │                              │ server_profile_apply (Callback-Thread)
        │                              ▼
        │                    atomare Zahl g_forceRangesInFoundations   ◄── server_profile_force_ranges_in_foundations()
        │                              │ (ohne Profil / nach Reset: 0)         (lock-frei, jeder Thread)
        ▼                              ▼
  Werte in Metern             hubForceRangesInFoundations (Spiegel, Callback-Thread schreibt)
                                       │
                              Dialog: ui_ranges_forced_by_server()
```

- Der Haken zeigt sich **angehakt und gesperrt**. Ein Klick (auch auf den Text) hakt ihn
  sofort wieder an und meldet "Cannot change: enforced by server" — wie beim erzwungenen
  Distanz-Muting.
- Der **eigene** Wert des Spielers in der Arbeitskopie bleibt unberuehrt; der Zwang wird
  **nicht** in `plugin.cfg` geschrieben (anders als der alte Weg bei
  `ForceDistanceBasedMuting`). Verlaesst der Spieler den Server, gilt wieder seine eigene
  Wahl.
- Der Chat (Moduswechsel, Hoerbonus) folgt derselben Regel: eigener Haken **oder** Zwang.
- **Geht der Zwang an oder aus, waehrend der Dialog offen ist** (Verbinden, Admin aendert
  die Beschreibung), zeigen die Felder noch die alte Einheit. Damit eine in der einen
  Einheit getippte Zahl nie als die andere gelesen wird, hat der Dialog **eine** Anzeige-Einheit
  (`ui_range_unit_refresh()`): Beim naechsten Tippen oder Speichern werden alle Felder neu
  angezeigt, **und Save speichert dann noch nichts** ("Display unit changed by the server -
  check the values, then click Save again").

## 5. Kein Drift — warum "40 m" nicht zu "39,936 m" wird

40 m werden in Fundamenten als `15.6` angezeigt (genau 15,625). Wuerde man beim Speichern
einfach den Text zurueckrechnen, ergaebe das 15,6 × 2,56 = **39,936 m** — der Wert haette
sich verschoben, ohne dass der Spieler etwas getan hat. Deshalb gilt:

> Steht im Feld noch genau die Zahl, die man fuer den gespeicherten Wert anzeigen wuerde
> (`range_display_unchanged`: beide auf 1 Nachkommastelle gerundet gleich), hat der Spieler
> nichts geaendert — dann bleiben die **Meter der Arbeitskopie unveraendert**.

Das gilt **in beiden Einheiten** und an drei Stellen: beim Tippen/Befuellen
(`EN_CHANGE` aendert nichts, wenn die Zahl gleich ist), beim Speichern
(`ui_dist_parse_display`) und beim Hin- und Herschalten der Einheit (es wird nur
angezeigt, nie zurueckgelesen). Erst eine **wirklich geaenderte** Zahl wird umgerechnet.
(`plugin.cfg` rundet beim Schreiben ohnehin auf 0,1 m — das war schon immer so.)

### Hub-Grenzen in Fundamenten: Minimum aufrunden, Maximum abrunden

Der Server gibt Grenzen in Metern vor. Damit **jeder** angezeigte Wert im Bereich auch in
Metern gueltig ist, werden die Grenzen **nach innen** auf 0,1 gerundet
(`range_limit_min_display` / `range_limit_max_display`):

| Grenze (Meter) | genau | angezeigt |
|---|---:|---:|
| Minimum 2,1 m | 0,82 F | **0,9** (0,8 F waeren 2,048 m = zu klein) |
| Maximum 15 m | 5,86 F | **5,8** (5,9 F waeren 15,1 m = zu gross) |
| 2,56 m (Min oder Max) | 1,0 F | **1,0** (Fliesskomma-Rauschen schiebt nicht auf 1,1) |

0 oder negativ heisst "keine Grenze" und bleibt 0; ein sehr kleines **gesetztes** Maximum
(< 0,1 F) bleibt 0,1 und wird nicht zu "keine Grenze". In Metern bleiben Grenzen
unveraendert. Liegt ein gespeicherter Wert genau auf der Grenze (z. B. 2,1 m = angezeigt
"0.8"), bleibt er gueltig und wird nicht angefasst — geprueft wird in Metern.

## 6. Server-Admins: Root-Beschreibung in Fundamenten

Mit `ForceRangesInFoundations=True` im `[GLOBAL]`-Teil gilt: **Alle Sprachreichweiten der
Root-Beschreibung sind in FUNDAMENTEN angegeben** und werden beim Parsen mit **2,56** in
Meter umgerechnet (`hub_convert_ranges_to_meters` am Ende von `hub_parse_settings`).
Ohne den Schluessel aendert sich nichts (Meter, wie bisher).

**Umgerechnet:**
- `[GLOBAL]` Minimum/Maximum Whisper (oder Wisper)/Normal/Shout (sechs Werte)
- `[RACE]` dieselben sechs Werte + `listenAddDistance`
- `[ZONES]` die Zonen-Reichweiten `Whisper=` (oder `Wisper=`), `Normal=`, `Shout=`
- `[DEFAULT_SETTINGS]` `DefaultDistanceWhisper/Normal/Shout`

**Nicht umgerechnet:** Zonen-Ecken `X1..Z4`, `GroundY`/`TopY`, `AudioMinDistance`,
`AudioMaxVolume` (auch in Zonen), `FilterIntensity`, alle `Occ*`/`Occlusion*`-Werte,
Tasten-Codes, SteamIDs, Passwoerter — alles, was keine Sprachreichweite ist.

**Der Schluessel darf irgendwo im `[GLOBAL]`-Teil stehen**, auch hinter den Werten. Deshalb
wird **nicht pro Zeile**, sondern **einmal am Ende** umgerechnet. Eine Rasse, die die
globalen Grenzen erbt, wird dabei wie alles andere genau einmal umgerechnet. Nach dem
Umrechnen gilt die bestehende Obergrenze (1000 m) erneut; `0` ("nicht gesetzt") bleibt 0.

Beispiel (Werte in Fundamenten; ergibt 2,56 / 2,56 / 15,36 / 15,36 / 40,96 / 40,96 m
und einen Hoerbonus von 10,24 m):

```
[GLOBAL]
ForceRangesInFoundations=True
ForceDistanceBasedMuting=True
MinimumWisper=1
MaximumWisper=1
MinimumNormal=6
MaximumNormal=6
MinimumShout=16
MaximumShout=16
[RACE]
Race=Wolfwesen
SteamID=(Name)76561198063092371
MinimumWhisper=1
MaximumWhisper=1
MinimumNormal=6
MaximumNormal=6
MinimumShout=16
MaximumShout=16
listenAddDistance=4
[ZONES]
Zone=Haus
...
Wisper=1
Normal=6
Shout=16
[DEFAULT_SETTINGS]
DefaultDistanceWhisper=1
DefaultDistanceNormal=6
DefaultDistanceShout=16
```

> **Schreibweise "Whisper" / "Wisper":** Das alte Plugin schrieb `Wisper` (ohne "h").
> Bisher kannte der Parser im `[GLOBAL]`-Teil **nur** `MinimumWisper` / `MaximumWisper`;
> ein Admin, der natuerlich `MinimumWhisper` tippt, wurde dort **still ueberlesen** (der Wert
> fehlte einfach, ohne Fehlermeldung). In `[RACE]` gingen schon immer beide.
> **Seit 091 gehen beide Schreibweisen ueberall:** `[GLOBAL]` (`MinimumWhisper` oder
> `MinimumWisper`, `MaximumWhisper` oder `MaximumWisper`), `[RACE]` (wie bisher),
> `[ZONES]` (`Whisper=` oder `Wisper=`) und `[DEFAULT_SETTINGS]` (`DefaultWhisperKey` oder
> `DefaultWisperKey`, `DefaultDistanceWhisper` oder `DefaultDistanceWisper`). Beide
> Schreibweisen fuellen **denselben** Wert; stehen beide im selben Abschnitt, gilt die
> **spaetere** Zeile. Alte Beschreibungen bleiben gueltig, nichts muss umgeschrieben
> werden. Grund fuer die Aenderung: Wer die Root-Beschreibung fuer Fundamente neu schreibt,
> soll nicht an einem fehlenden "h" scheitern.

## 7. Wie wurde es getestet?

Alle Host-Tests (`bash tests/run_tests.sh`): **35 Suiten**, `RESULT: ALL SUITES PASSED`
(vorher 34), +164 Pruefungen; MinGW-DLL-Build (`bash build/build_mingw.sh`) **0 Warnungen**;
jede angefasste `.c` zusaetzlich einzeln mit `-Wall -Wextra -Wconversion -Wshadow`
(naehert MSVC `/W4`) ohne Warnung uebersetzt.

| Suite | neu | Prueft |
|---|---:|---|
| `range_units_test` (neu) | 58 | 2,56-m-Konstante; 2/15/40 m → 0,78/5,86/15,63 und 1/6/16 F → 2,56/15,36/40,96 m; Rundlauf; Grenzen auf-/abrunden (2,1 m → 0,9; Vielfache von 2,56 m exakt; 2000 Grenzen als Eigenschaft); 0/negativ/NaN; "unveraendert"-Test; Einheiten-Namen |
| `config_text_test` | +12 | `ShowRangesInFoundations` lesen (true/false/1/0/Gross-Klein/Unsinn), Standard aus, Nachbarschluessel, Normierung auf 0/1 |
| `ui_config_logic_test` | +30 | Feld gehoert dem Dialog (Save uebernimmt es), `ui_dist_parse_display`: kein Drift in beiden Einheiten, Umrechnung, Grenzen in der Anzeige-Einheit, Fehlerfaelle |
| `hub_parser_test` | +45 | Schluessel an/aus/fehlt; Beispiel-Block → 2,56/15,36/40,96/10,24 m; Zone, Defaults, Rassen-Erbe; Koordinaten/Occlusion/Passwort/SteamID unberuehrt; Schluessel **nach** den Werten; ohne Schluessel unveraendert; 1000-m-Deckel; Dezimalkomma; beide Schreibweisen `Whisper`/`Wisper` in allen Abschnitten |
| `server_profile_test` | +12 | lock-freier Getter (ohne Profil 0, mit Profil 1, nach Reset 0), Werte im Profil in Metern, Hoerbonus-Zeile in beiden Einheiten |
| `voice_modes_test` | +7 | Moduswechsel-Zeile: Meter unveraendert, eigener Haken, Server-Zwang, Zwang beruehrt die eigene Einstellung nicht, nach Verlassen wieder Meter |

Der Dialogcode selbst (Win32) laeuft nicht auf dem Host; seine reine Logik steckt deshalb in
`ui_config_logic.h` / `range_units.h`.

### Manuell im TS-Client (Windows, noch offen)

1. **Standard:** F10 → "Plugin Settings": der Haken "Show ranges in foundations" steht rechts
   von "Enable distance-based muting", Text weiss und lesbar, Klick auf den **Text** schaltet
   den Haken. Alles andere sieht aus wie vorher. `plugin.cfg` enthaelt `ShowRangesInFoundations=false`.
2. **Umschalten:** Haken setzen → die drei Felder zeigen sofort Fundamente
   (3/13/26 m → `1.2` / `5.1` / `10.2`), die Statuszeilen darunter nennen "foundations".
   Wieder abhaken → `3.0` / `13.0` / `26.0`. Mehrmals hin und her: die Meter driften nicht.
3. **Kein Drift:** Shout auf `40` setzen, Save. Haken setzen → `15.6`. **Save** → in
   `plugin.cfg` steht weiter `DistanceShout=40.0`. Dann `16` tippen, Save → `DistanceShout=41.0`
   (16 F = 40,96 m).
4. **Cancel:** Haken setzen, **Cancel**, F10 neu oeffnen → Haken wieder aus, Config unveraendert.
5. **Save/Neustart:** Haken setzen, Save, TS neu starten → Dialog zeigt Fundamente,
   `plugin.cfg` hat `ShowRangesInFoundations=true`, die Distanzen stehen weiter in Metern.
6. **Chat:** Moduswechsel per Taste (Num1/2/3): Meter → `... Distance: 15.0 m`; Fundamente →
   `... Distance: 5.9 foundations (15.0 m)`.
7. **Presets:** Tab "Voice Presets": Liste zeigt `m` bzw. `fnd.`, der Text passt in die Zeile
   (auch mit langem Namen). Preset laden: die Felder zeigen die Anzeige-Einheit.
8. **Server-Zwang:** Auf einem Test-Server `ForceRangesInFoundations=True` in `[GLOBAL]` und
   die Beispielwerte aus Abschnitt 6 eintragen. Verbinden: Haken ist **angehakt und grau**;
   Klick auf Haken oder Text → bleibt angehakt, Meldung "Cannot change: enforced by server";
   die Felder zeigen z. B. Normal `6.0` (Grenzen 6.0-6.0). Moduswechsel-Chat und die
   Rassen-Zeile nennen "foundations". Server verlassen → Haken wieder frei und entsprechend
   der eigenen Wahl; in `plugin.cfg` steht weiter der eigene Wert (der Zwang wurde nie geschrieben).
9. **Grenzen:** Server mit `ForceDistanceBasedMuting=True`, `MinimumWisper=2.1`,
   `MaximumWisper=10` (Meter, ohne `ForceRangesInFoundations`) + Haken des Spielers: Statuszeile
   "Whisper: ... foundations (Valid range: 0.9-3.9)"; im Whisper-Feld `0.5` tippen → das Feld
   korrigiert sich auf `0.9` (Minimum aufgerundet, 0,8 waeren nur 2,048 m).
10. **Zwang waehrend der Dialog offen ist:** F10 auf "Plugin Settings" offen lassen, dann
    verbinden (oder die Root-Beschreibung live aendern) und **Save** klicken → Meldung
    "Display unit changed by the server ...", Felder in der neuen Einheit, noch nichts
    gespeichert; zweiter Klick speichert.

## 8. Lerneffekt

Eine Anzeige-Option darf **nie** die gespeicherten Daten veraendern: Speichern bleibt in
einer Einheit (Meter), nur die Schicht, die mit dem Menschen spricht, rechnet um — und
sie rechnet nur das um, was der Mensch wirklich geaendert hat, sonst "wandern" die Werte
durch Rundung. Und: wenn ein Feld je nach Zustand etwas anderes bedeutet (Meter oder
Fundamente), muss es genau **eine** Stelle geben, die sagt, in welcher Einheit es gerade
steht — sonst liest man eine Zahl in der falschen Einheit zurueck.
