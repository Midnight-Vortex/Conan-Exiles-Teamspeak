# 107 — Pos.txt wird nur noch angefasst, wenn HTTP ausfaellt (Plugin 8.0.26)

**Plugin 8.0.26** (dieselbe Version wie [Doku 105](105-http-antwort-wartet-nicht-auf-plugin-arbeit.md), noch nicht
gebaut fuer Windows und **nicht** verteilt). Auf dem Host getestet, **nicht** im TeamSpeak-Client auf Windows geprueft.

**Ausloeser:** Das Spiel schickt die Position auf zwei Wegen: per HTTP (`POST /v1/position`, etwa 33-mal pro Sekunde)
und zusaetzlich als Datei `Pos.txt` (gleiche Rate). Gewollt ist: **Die Datei zaehlt nur, wenn HTTP gar nicht
funktioniert — nie beide gleichzeitig.**

**Abgrenzung:** Dass die **Mod** die Datei nicht mehr schreibt, solange HTTP antwortet, ist eine **eigene Aufgabe im
DevKit des Spiels** und **nicht** Teil dieser Aenderung. Die Plugin-Aenderung hier ist **auch allein nuetzlich** und
funktioniert mit der heutigen Mod: Das Plugin hoert auf, die Datei anzufassen, solange HTTP-Positionen ankommen — egal
ob die Mod sie weiter schreibt oder nicht.

**Begriffe:** *Watcher* = der Pos-Watcher-Thread, der alle 30 ms die Position prueft. *Hold* = das 2-Sekunden-Fenster,
das jede angenommene HTTP-Position neu startet („HTTP ist gerade die Quelle“). *Stat* = Abfrage von Zeitstempel und
Alter einer Datei beim Betriebssystem (`FindFirstFileW`).

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/mod_file/pos_tick.h` | Neue kleine reine Funktion `pos_tick_wants_file()`: „Soll dieser Takt ueberhaupt an `Pos.txt`?“ — ja nur, wenn die Datei eingeschaltet ist **und** kein Hold aktiv ist. |
| `src/core/mod_file/pos_file.c` (`pos_watcher_thread`) | Der Takt liest `g_httpHoldUntilMs` einmal am Anfang. Ist ein Hold aktiv, wird **weder** der Pfad aufgeloest (`pos_resolve_file_path`) **noch** die Datei abgefragt (Stat) **noch** gelesen. Der Takt geht den gleichen Weg wie im reinen HTTP-Betrieb (Koordinaten behalten). Die Debug-Zeile „Pos.txt skipped“ am Anfang des Datei-Zweigs entfaellt (kann nicht mehr vorkommen); stattdessen eine neue Zeile (nur `debug=1`, hoechstens 1/s). Kommentare angepasst. |
| `src/core/mod_file/pos_file.h` | Nur Kommentar: der Hold sperrt jetzt **jeden** Dateizugriff, nicht nur das Ueberschreiben. |
| `tests/pos_tick_test.c` | Neuer Block [3] mit 12 Pruefungen (insgesamt 28). |
| `doku/module/pos-http.md`, `doku/03-build-und-tests.md` | Nachgezogen. |

Nicht angefasst: Gnadenzeiten, Alterstest der Datei, Pfad-Autoerkennung (alle 15 s), Passwort-Behandlung,
Inject/Notify aus 105, HTTP-Server, Version, UI.

## 2. Wie war es vorher?

Solange HTTP-Positionen ankamen, **las** das Plugin `Pos.txt` nicht (der Hold verhinderte nur das **Lesen**). Aber der
Watcher fasste die Datei in **jedem** 30-ms-Takt trotzdem an:

- `pos_resolve_file_path()` kopierte die Einstellungen und fragte per `GetFileAttributesW()` den Saved-Ordner ab,
- `pos_get_file_write_quad()` fragte per `FindFirstFileW()` Zeitstempel und Alter von `Pos.txt` ab,
- im Debug-Betrieb schrieb eine Zeile „Pos.txt skipped (HTTP hold ...)“ einmal pro Sekunde, dass die Datei uebergangen wurde.

Das sind rund 33 Dateisystem-Abfragen pro Sekunde fuer eine Datei, die gar nicht gebraucht wird.

## 3. Warum ist die neue Loesung besser?

- **Eine Quelle zur Zeit.** Solange HTTP funktioniert, gibt es im Plugin keinen Dateizugriff mehr. Die Datei ist
  wirklich nur noch der Notfallweg, wie vom Besitzer gewollt.
- **Weniger Arbeit.** Die rund 33 Abfragen pro Sekunde (Ordner pruefen + Datei abfragen) entfallen, solange HTTP laeuft.
- **Kein neues Risiko.** Waehrend des Holds endeten beide Zweige schon vorher im selben Zustand („Koordinaten
  behalten“); der Datei-Zweig hat nur zusaetzlich Dateisystem-Aufrufe gemacht. Jetzt wird der kuerzere Weg genommen.
- **Kein Positionsloch.** Sobald der Hold vorbei ist (2 s ohne HTTP-Position), laeuft der Takt wieder genau wie
  vorher und die Datei uebernimmt.

## 4. Wie funktioniert es?

Am Anfang jedes Takts:

```
holdBis = g_httpHoldUntilMs            (einmal lesen)
dateiArbeit = Pos.txt eingeschaltet  UND  NICHT (jetzt < holdBis)       <- pos_tick_wants_file()

dateiArbeit ?  Pfad aufloesen, Datei abfragen, ggf. lesen   (wie bisher)
sonst          nichts an der Datei; Koordinaten behalten (Hold / Gnadenzeit)
```

Der Hold ist aktiv, solange `jetzt < holdBis`; bei `jetzt == holdBis` ist er **vorbei**. `holdBis = 0` heisst „nie
gehalten“.

**Zeitleiste:**

```
Zeit ───────────────────────────────────────────────────────────────────────────▶
HTTP:     ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│                        │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
          (Positionen kommen an)    HTTP faellt aus           HTTP kommt zurueck
Hold:     ═════════════════════════════════╗(2 s nach der letzten HTTP-Position)
Datei:    nicht angefasst           nicht angefasst   ╚══▶ Datei liest/uebernimmt    nicht angefasst
Quelle:   http                      http (Hold laeuft)     file                       http
```

1. **HTTP funktioniert:** jeder Takt sieht den Hold, fasst die Datei nicht an, behaelt die Koordinaten.
2. **HTTP faellt aus:** die letzte angenommene Position haelt noch 2 s.
3. **Nach 2 s uebernimmt die Datei:** der Hold ist vorbei, der Takt loest den Pfad auf, fragt die Datei ab und liest
   sie (Quelle wechselt `http -> file`).
4. **HTTP kehrt zurueck:** die naechste angenommene Position startet den Hold neu, die Datei ruht wieder
   (`file -> http`).

**Randfaelle, die bewusst unveraendert bleiben:**

- **Hold startet mitten im Takt** (der HTTP-Thread laeuft ja parallel): Im Datei-Zweig wird der Hold vor dem Lesen und
  nach dem Lesen noch einmal geprueft. Ein Datei-Muster, das in so einem Takt gelesen wurde, wird verworfen.
- **Alte Datei aus einer frueheren Sitzung:** die Pruefung „hat sich die Datei geaendert / ist sie ganz frisch / hat
  sich die Nummer geaendert?“ gilt wie vorher. Nach dem Hold vergleicht der erste Zugriff mit dem zuletzt gemerkten
  Zeitstempel; wurde die Datei inzwischen geschrieben, ist „geaendert“ wahr. Das macht die Pruefung nur **grosszuegiger
  fuer eine Datei, die sich wirklich geaendert hat** — das ist richtig so.
- **Pfad-Autoerkennung** (alle 15 s) laeuft weiter, auch waehrend des Holds: sie ist billig, solange der Pfad gueltig
  ist, und der Ausweichweg braucht den bekannten Pfad in dem Moment, in dem HTTP ausfaellt.

**Debug (`debug=1`):** hoechstens einmal pro Sekunde (dieselbe Drossel wie die zweite Zeile):

```text
DBG POS: Pos.txt not checked while HTTP positions arrive (hold left=1800ms)
```

Die Statuszeile (1x/s) zeigt waehrend des Holds `fileAge=n/a`, weil die Datei nicht abgefragt wird — das ist gewollt.
Die Zeile „Pos.txt skipped (HTTP hold left=... seq=... age=...)“ nach einem erfolgreichen Lesen bleibt (fuer einen Hold,
der mitten im Takt begann).

## 5. Wie wurde es getestet?

**Hier auf dem Host gelaufen:**

- `bash tests/run_tests.sh` → `RESULT: ALL SUITES PASSED` (36 Suiten + `layering_guard`).
  - `pos_tick_test` Block [3] `pos_tick_wants_file` (12 neue Pruefungen, insgesamt 28): eingeschaltet / ausgeschaltet,
    Hold aktiv (1999 ms uebrig, noch 1 ms uebrig), `jetzt == holdBis` (vorbei), Hold seit 1 ms vorbei, lange vorbei,
    `holdBis = 0` (nie gehalten), Uhr bei 0, 64-Bit-Zeiten.
- `bash build/build_mingw.sh`: baut und linkt die DLL, 0 Warnungen.
- `pos_file.c` zusaetzlich streng uebersetzt (`-Wall -Wextra -Wconversion -Wshadow -Wmaybe-uninitialized`): 0 Warnungen.

**Nicht geprueft (offen):**

- **Der Ablauf im echten TeamSpeak-Client auf Windows.** `pos_file.c` laeuft auf dem Host nicht (nur Windows); die
  Takt-Schleife selbst ist also nur durch die getestete Entscheidung und Lesen des Codes abgedeckt.
- **Der MSVC-Build** (`/W4 /WX`): nur durch die MinGW-Naeherung abgedeckt.
- **Die Mod-Seite** (die Mod hoert auf, `Pos.txt` zu schreiben, solange HTTP antwortet): eigene Aufgabe im DevKit.

**Vor dem Verteilen im TeamSpeak-Client pruefen (Windows, `debug=1`):**

1. Spiel laeuft, HTTP kommt an: `plugin.log` zeigt etwa einmal pro Sekunde `POS: Pos.txt not checked while HTTP
   positions arrive (hold left=...)` und **keine** Zeile `POS: Pos.txt skipped (HTTP hold left=... age=...)` mehr aus
   dem Anfang des Takts; die Statuszeile zeigt `method=http ... fileAge=n/a`.
2. HTTP blockieren (z. B. per Firewall-Regel fuer den Plugin-Port), Mod schreibt `Pos.txt` weiter: nach etwa
   2 s erscheint `POS: method http -> file`, die Stimmen der anderen folgen weiter ohne Luecke.
3. HTTP wieder freigeben: `POS: method file -> http`.
4. Spiel beenden, TeamSpeak laeuft weiter: Mit der heutigen Mod (schreibt die Datei bis zum Schluss) darf fuer
   wenige Sekunden `POS: method http -> file` erscheinen — die Datei ist dann noch frisch, das war schon immer so.
   Danach `POS: coordinates invalid`, der Spieler geht in den Hub.
5. TeamSpeak neu starten, waehrend das Spiel aus ist und die alte `Pos.txt` noch daliegt: **keine** Koordinaten aus
   der alten Datei (Regel „Rest aus frueherer Sitzung“).

## 6. Lerneffekt

„Nicht lesen“ ist nicht dasselbe wie „nicht anfassen“. Der alte Hold verhinderte nur den teuren Teil (Lesen); die
billigen Abfragen liefen weiter — dafuer 33-mal pro Sekunde, fuer nichts. Wenn eine Quelle ausdruecklich nur der
Notfallweg sein soll, gehoert die ganze Kette hinter die Bedingung, und die Bedingung als kleine reine Funktion macht
sie testbar, auch wenn der Rest des Watchers nur auf Windows laeuft.
