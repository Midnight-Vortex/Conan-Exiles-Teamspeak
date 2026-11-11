# 101 — Debug-Schalter `dc tsdebug on|off` fuer das Mund-Log (nur Mod)

**Nur Mod**, 2026-11-11. Plugin unveraendert (8.0.22).
Die Mod-Pak ist **noch nicht gekocht und nicht hochgeladen** (der Besitzer kocht selbst).

Wunsch des Besitzers: *ein Konsolenbefehl, der einen Debug-Modus an- oder ausschaltet, standardmaessig aus;
das Log wird nur geschrieben, wenn man ihn einschaltet.*

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| Mod `Vo_PosStreamer_Actor` | Neue Variable `DebugLog` (Wahrheitswert, Standard **aus**, nicht repliziert) und neue Funktion `SetDebug(On)`, die sie setzt. |
| Mod `Vo_PosStreamer_Actor`, Funktion `UpdateTalk` | Die `TSMOUTH`-Zeile wird nur noch geschrieben, wenn `DebugLog` an ist. Bedingung der vorhandenen Weiche: *Zeit ≥ `TalkLogNext`* **und** *`DebugLog`*. |
| Mod, neu: `DataCmd_TS_Debug` | Befehls-Actor (Elternklasse `DataActorCommand`) fuer `dc tsdebug`. |
| Mod `DT_TS_ConsoleCommands` | Neue Zeile `tsdebug` (laeuft **auf dem Client**, nicht auf dem Server, ohne Admin-Pflicht in der Tabelle). Die Zeile `tsvm` ist unveraendert. |

---

## 2. Wie war es vorher?

Die Zeile `TSMOUTH name=<Charaktername> dist_cm=<Abstand>` wurde bei jedem Spieler immer geschrieben, hoechstens
alle zwei Sekunden (Doku 098 und 100). Abschalten konnte man sie nicht.

---

## 3. Warum ist die neue Loesung besser?

- **Standard aus.** Ohne Befehl schreibt die Mod keine eigene Zeile mehr ins Log.
- **Einschalten ohne neue Pak.** Wer etwas untersuchen will, tippt den Befehl in die Konsole.
- **Kein Risiko fuer das Ingame-Passwort.** Der Schalter ist rein lokal: keine neue replizierte Variable, keine
  Aenderung an `Vo_TsVmHolder`, `DataCmd_TS_Vm`, `BP_ModController` oder `SG_TS_Vm`. Diese vier Dateien sind
  bytegleich mit vorher (Hash geprueft), die Tabellenzeile `tsvm` ebenfalls.

**Was der Schalter nicht kann:** Er steuert nur die `TSMOUTH`-Zeile der Mod. Die `LogModHttp`-Zeilen schreibt das
Spiel selbst (zwei pro Anfrage); die lassen sich aus der Mod nicht abstellen (Doku 100, Abschnitt 10).

---

## 4. Wie funktioniert es?

```
Konsole:  dc tsdebug on        (auch: DataCmd tsdebug on)
            └─ Spiel sucht "tsdebug" in der Befehlstabelle → DataCmd_TS_Debug, laeuft beim Spieler selbst
                 ├─ An = (Parameter == "on", Gross-/Kleinschreibung egal)      alles andere = aus
                 ├─ sucht den Mod-Actor Vo_PosStreamer_Actor
                 ├─ schreibt IMMER eine Rueckmeldung in Konsole und Log:
                 │     "Teamspeak debug log: on"  /  "… off"  /  "… - mod actor not found"
                 └─ wenn gefunden: SetDebug(An)

UpdateTalk:  … Figur gefunden → Weiche: (Zeit ≥ TalkLogNext) UND DebugLog → Log String "TSMOUTH …"
```

- Der Schalter gilt nur fuer den Spieler, der den Befehl tippt, und nur bis er die Spielwelt verlaesst
  (danach wieder aus).
- `dc tsdebug` ohne Angabe oder mit einem anderen Wort schaltet aus.
- Kommt statt der Rueckmeldung `CustomCommandsDataTable does not contain tsdebug`, kennt das Spiel den Befehl
  nicht (alte Pak).

---

## 5. Wie wurde es getestet? (DevKit)

- Alle geaenderten Blueprints kompilieren ohne Fehler und Warnungen. Gespeichert wurden genau drei Dateien:
  `Vo_PosStreamer_Actor`, `DataCmd_TS_Debug`, `DT_TS_ConsoleCommands`.
- **Befehlsweg zuerst bewiesen:** Im Spielmodus des Editors (eigener Server plus Client) lief ein Probe-Befehl
  ueber den lokalen Spieler-Controller des Clients: `[DataCmd_TS_Debug_C_0] Client 1: …`. Ein unbekannter
  Befehl ergibt `CustomCommandsDataTable does not contain …`.
- **Schalter:**

  | Befehl | `DebugLog` danach |
  |---|---|
  | (nichts getippt) | aus |
  | `dc tsdebug on` | an |
  | `dc tsdebug off` | aus |
  | `dc tsdebug ON` | an |
  | `dc tsdebug` | aus |
  | `DataCmd tsdebug on` | an |
  | `dc tsdebug quatsch` | aus |

- **Log-Weiche** (je ein frischer Mod-Actor, Sprecherpunkt auf einer zweiten Figur):

  | Fall | Figur spricht | `TSMOUTH`-Zeile |
  |---|---|---|
  | nie eingeschaltet | ja | nein |
  | eingeschaltet | ja | ja |
  | ein- und wieder ausgeschaltet | ja | nein |

- Knotenzahl: `UpdateTalk` 72 → 74, `UpdatePosition` unveraendert 77, `EventGraph` 21, neu `SetDebug` 2.

**Nicht geprueft:**
- der Spiel-Client. Der Editor hat am 11.11. zweimal bestanden, wo das fertige Spiel scheiterte (Doku 100).
- ob Spieler ohne Admin-Rechte den Befehl `dc` ueberhaupt benutzen duerfen.
- der Passwort-Befehl `dc tsvm` im Editor. Der Versuch, ihn per Skript im Spielmodus auszuloesen, hat den Editor
  zum Absturz gebracht (kein Datenverlust). Abgesichert ist der Passwort-Weg deshalb ueber die unveraenderten
  Dateien und die unveraenderte Tabellenzeile, nicht ueber einen Lauf.

---

## 6. Was der Besitzer tun muss

1. Mod kochen und hochladen.
2. Im Spiel `dc tsdebug on` tippen. Erwartet: `Teamspeak debug log: on` in der Konsole. Danach erscheinen die
   `TSMOUTH`-Zeilen in `ConanSandbox.log`, sobald jemand in der Naehe spricht. Mit `dc tsdebug off` wieder aus.
3. Einmal pruefen, ob der Wechsel in den Ingame-Channel auf dem Server weiter funktioniert.

Sicherung vor der Aenderung (Mod-Datei, Befehlstabelle, `modinfo.json`):
`C:\Users\uwebu\Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261111-debug`.

---

## 7. Lerneffekt

Einen neuen Weg zuerst mit dem kleinsten moeglichen Stueck beweisen: Erst lief nur ein Befehl, der eine Zeile
druckt. Erst als der ankam, kamen Variable, Funktion und Weiche dazu. Und ein Schalter, der nichts mit dem
Passwort zu tun hat, soll auch keine Datei des Passwort-Wegs anfassen.
