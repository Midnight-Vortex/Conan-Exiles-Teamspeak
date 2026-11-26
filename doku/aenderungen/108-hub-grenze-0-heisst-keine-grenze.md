# 108 — F10-Dialog: Eine Server-Grenze von 0 heisst "keine Grenze" — jetzt auch bei der Sofort-Korrektur

**Fehlerbehebung**, kein Versionssprung (die Version aendert sich nur auf Zuruf). Betrifft nur den
F10-Einstellungsdialog (Tab "Plugin Settings"). **Audio, Spiel-Mod und Root-Parser sind nicht betroffen.**
Auf dem Host getestet, **nicht** mit MSVC gebaut und **nicht** im TeamSpeak-Client auf Windows geprueft.

**Ausloeser:** Beim Gegenlesen von [Doku 091](091-reichweite-in-fundamenten.md) fiel ein alter Fehler auf, der
nichts mit den Fundamenten zu tun hat. Er stand dort als "Bekannt, aber nicht Teil von 091". Im Auftrag hiess
dieser Eintrag noch "092"; die Nummer war inzwischen vergeben (092 = Version 8.0.16), deshalb **108**.

**Begriffe:** *Root-Channel* = der oberste TeamSpeak-Channel; in seiner Beschreibung stehen die Vorgaben des
Servers. *Hub-Grenze* = kleinste / groesste Reichweite, die der Server pro Sprachmodus erlaubt
(`MinimumShout`, `MaximumShout` …). *Einseitige Grenze* = der Admin hat nur eine der beiden Seiten gesetzt.
*Arbeitskopie* = die Werte, die der Dialog im Speicher haelt, solange er offen ist; erst **Save** schreibt sie
in `plugin.cfg`. *Sofort-Korrektur* = der Dialog zieht einen Wert schon beim Tippen und bei jeder
Aktualisierung in die Hub-Grenzen, nicht erst beim Speichern.

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ui/config/ui_config_logic.h` | Drei neue reine Funktionen: `ui_hub_below_min`, `ui_hub_above_max` (die eine Stelle, an der der Dialog fragt "liegt der Wert ausserhalb einer Hub-Grenze?") und `ui_hub_required_digits` (wie viele Ziffern getippt sein muessen, bevor ein Wert beurteilt wird). |
| `src/ui/validation/ui_hub_validation.c` | `validateDistanceValue` vergleicht ueber die beiden neuen Funktionen statt direkt mit `<` und `>`. `shouldValidateValue` holt die noetige Ziffernzahl aus `ui_hub_required_digits`. |
| `src/ui/dialogs/ui_messages.c` | Die sechs Vergleiche der Statuszeilen (3-mal "unter Minimum", 3-mal "ueber Maximum" in `updateConsolidatedDistanceMessages`) benutzen dieselben beiden Funktionen. |
| `tests/ui_config_logic_test.c` | 35 neue Pruefungen (Abschnitte `[3g]` und `[3h]`). |
| `doku/aenderungen/091-reichweite-in-fundamenten.md` | Der Hinweis "Bekannt, aber nicht Teil von 091" verweist jetzt hierher. |
| `doku/03-build-und-tests.md` | Stand der Tests nachgezogen. |

Sonst wurde nichts angefasst: kein Text einer Meldung, kein Config-Schluessel, keine Reihenfolge.

## 2. Wie war es vorher?

Steht in der Root-Beschreibung `ForceDistanceBasedMuting=True`, darf der Spieler seine Reichweiten nur
innerhalb der Hub-Grenzen waehlen. Fehlt ein Schluessel, kommt die Grenze als **0** an. Beispiel:

```
[GLOBAL]
ForceDistanceBasedMuting=True
MinimumShout=10
                      <- kein MaximumShout: das Maximum ist 0
```

Drei Stellen lesen diese 0 richtig als "**nicht gesetzt**":

- das Spiel selbst (`voice_mode_get_distance` in `voice_modes.c`),
- die Pruefung beim Speichern (`ui_dist_parse`),
- die Klemme des Fundamente-Modus (`range_clamp_meters`).

Die Sofort-Korrektur des Dialogs (`validateDistanceValue`) tat das **nicht**. Sie verglich stur
`value > maximum` — und jede Zahl ist groesser als 0. Folgen (vor der Aenderung so nachgestellt, siehe
Abschnitt 5):

| Situation | Was passierte |
|---|---|
| Nur `MinimumShout=10`, eigener Wert 30, Dialog offen | Der Wert sprang bei **jeder** Aktualisierung: 30 → 0 → 10 → 0 → 10 … (0 ist "unter dem Minimum", 10 ist "ueber dem Maximum 0") |
| Im Feld eine Zahl ab dem Minimum tippen (10, 30, 150) | Wurde sofort zu `0.0` "korrigiert" |
| `ForceDistanceBasedMuting=True` ganz ohne Min/Max-Schluessel | Alle drei Reichweiten wurden 0; Save lehnte ab ("must be greater than 0") |
| Statuszeile unter dem Feld | "Auto-corrected: above maximum 0.0" |
| Fundamente-Modus, nur ein Minimum (aus dem Code abgeleitet, nicht nachgestellt) | Kein Springen, aber jeder Wert wurde auf das Minimum gezogen |

Dazu kam ein zweiter, kleinerer Fehler in `shouldValidateValue`. Diese Funktion laesst den Dialog **warten**,
bis genug Ziffern getippt sind — sonst wuerde bei einem Minimum von 5 schon die "1" von "15" zu "5.0"
korrigiert. Wie viele Ziffern noetig sind, las sie an den beiden Grenzen ab. Ein fehlendes Maximum (0)
zaehlte dabei wie ein **einstelliges** Maximum. Bei `MinimumNormal=5` ohne Maximum wurde die "1" deshalb
sofort korrigiert, und "15" liess sich nicht tippen. (Solange Fehler 1 da war, fiel das nicht auf — es wurde
ohnehin alles zu 0.)

Workaround bisher: immer Minimum **und** Maximum setzen.

## 3. Warum ist die neue Loesung besser?

- **Eine Regel, eine Stelle.** "0 = nicht gesetzt" stand vorher an drei Stellen richtig und an zwei Stellen
  (Korrektur, Statuszeile) gar nicht. Jetzt fragen Korrektur **und** Statuszeile dieselbe Funktion. Sie
  koennen sich nicht mehr widersprechen.
- **Der Dialog erzeugt keine ungueltige 0 mehr.** Eine Reichweite von 0 ist nie erlaubt; vorher hat der
  Dialog sie selbst hergestellt und dann beim Speichern abgelehnt.
- **Zweiseitige Grenzen verhalten sich exakt wie vorher.** Das ist nicht nur behauptet: Alte und neue Datei
  wurden fuer 384 000 Kombinationen aus Minimum, Maximum und Wert verglichen — jede Ausgabe ist gleich.
- **Testbar.** `ui_hub_validation.c` haengt an den Fenster-Variablen des Dialogs und laesst sich auf dem
  Host nicht bauen. Der Vergleich liegt deshalb in einem Header ohne Windows, den der Host-Test direkt
  einbindet. Der Test prueft also genau den Code, den das Plugin ausfuehrt.

## 4. Wie funktioniert es?

Alles laeuft auf dem **UI-Thread** des Dialogs. Die neuen Funktionen sind rein: Sie rechnen nur mit den
uebergebenen Zahlen, haben keinen Zustand und rufen nichts von TeamSpeak auf.

```
Root-Beschreibung            hub_parser.c                Dialog (UI-Thread)
MinimumShout=10       --->   minShout = 10        --->   hubMinimumShout = 10
(kein MaximumShout)          maxShout = 0                hubMaximumShout = 0   (0 = nicht gesetzt)
                                                                |
        +-------------------------------+-----------------------+----------------------+
        |                               |                                              |
  Tippen im Feld                  Aktualisierung                                 Statuszeile
  handleDistanceEditChange        updateDynamicInterface                         updateConsolidated-
        |                               |                                        DistanceMessages
  shouldValidateValue             ui_correct_to_hub_limits                             |
  (genug Ziffern?)                      |                                              |
        |                               |                                              |
        +------> validateDistanceValue <+                                              |
                        |                                                              |
                        +---------> ui_hub_below_min / ui_hub_above_max <--------------+
                                    (ui_config_logic.h, die EINE Regel)
```

Die Regel selbst sind zwei Zeilen:

```c
static inline int ui_hub_below_min(double value, double hubMin) {
    return hubMin > 0.0 && value < hubMin;     /* 0 = kein Minimum */
}
static inline int ui_hub_above_max(double value, double hubMax) {
    return hubMax > 0.0 && value > hubMax;     /* 0 = kein Maximum */
}
```

"Nicht gesetzt" ist alles, was keine positive Zahl ist: 0, negativ oder NaN (keine gueltige Zahl). Die
Parameter sind `double`, weil die Statuszeilen mit den `double`-Variablen `hubMinimumX` vergleichen und die
Korrektur mit `float`. Ein `float` laesst sich verlustfrei in ein `double` umwandeln, deshalb vergleichen
beide Aufrufer genau wie vorher und brauchen keinen Cast.

Was der Dialog jetzt tut:

| Hub-Grenzen | Eigener Wert | Ergebnis |
|---|---|---|
| 5 bis 15 | 3 / 10 / 20 | 5 / 10 / 15 (wie vorher) |
| nur Minimum 10 | 30 | bleibt 30 (vorher: sprang 0 ↔ 10) |
| nur Minimum 10 | 5 | 10 |
| nur Maximum 15 | 1 / 20 | 1 / 15 (wie vorher) |
| keine Grenze | 2 / 10 / 30 | bleiben (vorher: alle 0) |

**Die Ziffern-Regel** (`ui_hub_required_digits`), nur fuer das Tippen:

| Hub-Grenzen | noetige Ziffern | |
|---|---|---|
| beide gesetzt, oder nur ein Maximum | wie vorher | unveraendert |
| nur Minimum 10 | 2 | wie vorher |
| nur Minimum 5 (einstellig) | **2** (vorher 1) | ohne Maximum sind auch 10 und mehr erlaubt, also reicht eine Ziffer nicht |
| nur Minimum 100 | 3 | wird nicht gesenkt |
| keine Grenze | 1 | es kann nichts korrigiert werden, also muss nichts warten |

Beispiel `MinimumNormal=5`, kein Maximum, der Spieler tippt "15": Die "1" wartet (vorher: sofort "5.0"),
"15" wird angenommen. Wer eine einstellige Zahl tippt (z. B. "7"), sieht sie erst mit **Save** uebernommen —
genau wie schon immer bei zweiseitigen Grenzen mit einem Maximum ab 10. Save liest immer den Text des Feldes.

**Bewusst nicht geaendert** (stand nicht im Auftrag, "nur die eine Funktion"):

- Die Statuszeile zeigt bei einer einseitigen Grenze jetzt `Valid range: 10.0-0.0` statt
  "Auto-corrected: above maximum 0.0". Der Wert stimmt, die "0.0" ist aber noch missverstaendlich. Der Text
  selbst wurde nicht umformuliert.
- `updateDistanceMessage` in `ui_messages.c` hat denselben ungeschuetzten Vergleich, wird aber von
  nirgendwo aufgerufen (toter Code).
- Steht ein Minimum **ueber** dem Maximum (Tippfehler des Admins, z. B. 20 und 10), springt der Wert weiter
  zwischen beiden. Das war vorher so und ist ein anderer Fall.

## 5. Wie wurde es getestet?

**Host-Tests** (`bash tests/run_tests.sh`): weiter 36 Suiten + `layering_guard`, alle gruen.
`ui_config_logic_test` hat 212 Pruefungen (+35):

- `[3g]` (22): beide Grenzen wie vorher; nur Minimum; nur Maximum; gar keine Grenze; negative Grenze und
  NaN; sechs Aktualisierungen hintereinander lassen 30 bei 30. Dazu ein Durchlauf ueber alle
  Kombinationen von 5 Minima, 5 Maxima und 240 Werten: Die Sofort-Korrektur liefert dasselbe wie
  `range_clamp_meters`; ein korrigierter Wert wird nie noch einmal korrigiert (kein Springen) und besteht
  immer die Save-Pruefung; ein Wert, den Save annimmt, wird von der Sofort-Korrektur nie angefasst.
- `[3h]` (13): die Ziffern-Regel aus der Tabelle oben und "15" Taste fuer Taste.

**Gegenprobe:** Mit der alten Regel (ohne `> 0`) schlagen 17 der neuen Pruefungen fehl. Der Test wuerde den
Fehler also bemerken, falls er zurueckkommt.

**Die echte Datei, vorher und nachher.** `ui_hub_validation.c` wurde einmal im alten und einmal im neuen
Stand zusammen mit der echten `countSignificantDigits` gegen ein Wegwerf-Geruest gebaut (nicht im Repo) und
mit den gemeldeten Faellen gefuettert:

| Fall | vorher | nachher |
|---|---|---|
| nur Minimum 10, Wert 30, fuenf Aktualisierungen | 30 → 0 → 10 → 0 → 10 → 0 | 30 → 30 → 30 → 30 → 30 → 30 |
| nur Minimum 10, getippt 10 / 30 / 150 | 0.0 / 0.0 / 0.0 | 10 / 30 / 150 |
| keine Grenze, 2 / 10 / 30 | 0 / 0 / 0 | 2 / 10 / 30 |
| nur Minimum 5, getippt "1", dann "15" | 5.0, dann 0.0 | wartet, dann 15 |
| nur Maximum 5, getippt 3 / 9 | 3 / 5 | 3 / 5 |
| zweiseitig: 64 Grenz-Paare × 6000 Werte | — | alle 384 000 Ausgaben gleich |

**Cross-Build** (`bash build/build_mingw.sh`): baut und linkt, 0 Warnungen.

**Nicht geprueft:** der MSVC-Build (`/W4 /WX`) und der Dialog im TeamSpeak-Client. Dafuer am Windows-PC:

1. Root-Beschreibung: `ForceDistanceBasedMuting=True`, `MinimumShout=10`, **kein** `MaximumShout`.
2. F10, Tab "Plugin Settings": Der Shout-Wert bleibt stehen, auch nach Klick auf eine Checkbox.
3. Im Shout-Feld "30" tippen: bleibt 30. "5" tippen und Save: Meldung "must be at least 10.0".
4. `MinimumShout` auch entfernen: Alle drei Werte bleiben, Save klappt.
5. Gegenprobe mit Minimum **und** Maximum: alles wie gewohnt.

## 6. Lerneffekt

Wenn ein besonderer Wert eine Bedeutung traegt (hier: 0 = "nicht gesetzt"), gehoert diese Bedeutung in
**eine** Funktion, die alle benutzen — nicht als `> 0` verstreut an jede Stelle, die daran denken muss. Drei
Stellen hatten daran gedacht, zwei nicht, und genau die zwei hat kein Test abgedeckt. Und: Ein Fehler kann
einen zweiten verdecken; nach einer Korrektur lohnt der Blick auf die Nachbarfunktion.
