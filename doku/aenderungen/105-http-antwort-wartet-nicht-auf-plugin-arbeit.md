# 105 — Die HTTP-Antwort an das Spiel wartet nicht mehr auf Plugin-Arbeit + Messung langsamer Antworten (nach Plugin 8.0.25)

**Stand:** 2026-11-26, Arbeitsstand nach 8.0.25 (Versionsnummer **nicht** erhoeht). Gebaut und auf dem Host getestet,
**nicht** im TeamSpeak-Client auf Windows geprueft, **nicht** verteilt: liegt auf dem Zweig
`claude/bold-cray-dbfsjh`, nicht auf `tsmain`, kein Release.

**Ausloeser:** Spieler melden Ruckler im Spiel. Das Spiel schickt seine Position etwa 33-mal pro Sekunde an
`POST http://127.0.0.1:52734/v1/position`. Normal dauert eine Anfrage rund 7 ms. Etwa alle 3 Sekunden dauerte eine
80 ms, spaeter bis 480 ms — und das Spiel stand so lange.

**Ehrlich vorweg: Den Ausloeser dieser 3-Sekunden-Haenger kennen wir noch nicht.** Die Suche hat im Plugin keinen
Auftrag gefunden, der alle 3 Sekunden laeuft. Gefunden wurden zwei Schwaechen auf dem Antwortweg. Diese Aenderung
(1) beseitigt das **eigene Warten** des Plugins vor der Antwort und (2) **misst** jetzt, wie lange das Plugin
braucht — damit man kuenftig sagen kann, ob die Zeit im Plugin oder im Spiel verloren geht. Ob das die Haenger
behebt, zeigt erst ein Test auf Windows.

**Begriffe:** *Listener* = der eine HTTP-Thread des Plugins, der Verbindungen nacheinander annimmt (annehmen →
bearbeiten → schliessen). *Watcher* = der Pos-Watcher-Thread, der alle 30 ms die Position prueft. *Inject* =
eine per HTTP erhaltene Position wird im Plugin abgelegt (`pos_inject_sample`). *Notify* = die Benachrichtigung
„neue Position da“ an Overlay, CEPOS, Kanalwechsel und Audio. *QPC* = `QueryPerformanceCounter`, der genaue
Windows-Zeitgeber.

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/mod_file/pos_file.c` | `pos_inject_sample()` legt die Position nur noch **ab** und meldet „Notify offen“ (Merker + Ereignis). Der Watcher wartet auf „Stopp“ **und** „Inject“ und fuehrt das Notify selbst aus. Sein 30-ms-Takt laeuft ueber eine feste Frist weiter. Neues Ereignis in `pos_watcher_start()`, Aufraeumen bei Fehlern und in `pos_watcher_stop()`. |
| `src/core/mod_file/pos_tick.h` (neu) | Kleine reine Funktion `pos_wait_ms()`: wie lange darf der Watcher noch bis zur naechsten Taktfrist warten. |
| `src/core/mod_file/pos_file.h`, `src/core/http/pos_http_server.c/.h`, `src/ts/entry/ts3_entry.c` | Nur Kommentare (Thread-Vertrag): das Notify laeuft jetzt auf dem Watcher-Thread und wird zusammengefasst. |
| `src/core/http/pos_http_server.c` | Zeitmessung mit QPC pro Verbindung (Annahme bis Ende der Bearbeitung; bei der Positions-Anfrage zusaetzlich `recv` / `work` / `reply` / `send` / `after`). Gedrosselte Logzeile bei langsamen Antworten. Dazu die Dauer von „zurueck auf den Hauptport“ und „Lauscher neu oeffnen“. |
| `src/core/http/http_request.h` | Reine Hilfsfunktionen: Ticks → Millisekunden, „ist das langsam?“, Text der Logzeilen. |
| `tests/pos_tick_test.c` (neu), `tests/http_request_test.c`, `tests/run_tests.sh` | Neue Suite `pos_tick_test`; `http_request_test` um Block [7] erweitert. |
| `tests/slow_reply_server.ps1` (neu) | Handwerkzeug: Platzhalter fuer das Plugin, der absichtlich langsam antwortet (Abschnitt 5). **Ungetestet.** |
| `src/ui/overlay/voice_overlay.h`, `src/ui/plugin_ui_compat.c/.h` | Nur Kommentare: `plugin_ui_on_position_tick` wird nicht mehr vom HTTP-Listener gerufen, nur noch vom Watcher. |
| `build/build_mingw.sh` | `-liphlpapi` in der Bibliotheksliste ergaenzt. Seit 102 nutzt `pos_http_server.c` die Funktion `GetExtendedTcpTable`; im vcxproj stand `iphlpapi.lib` schon, im MinGW-Skript fehlte sie. Der Pruef-Build (und damit die CI) brach seitdem beim Linken ab. |

## 2. Wie war es vorher?

`http_handle_client()` rief `pos_inject_sample()` **vor** dem Senden der Antwort. Und `pos_inject_sample()` fuehrte
das Notify **sofort auf dem Listener-Thread** aus (`ts3_on_position_update_common()`):

- `plugin_ui_on_position_tick()`: nimmt die Sperre des Server-Profils, kopiert Einstellungen, ruft user32 fuer das
  Overlay (`IsWindow`, `GetWindowThreadProcessId`, `PostMessage`),
- `chan_signal_position_update()`, `cepos_signal_send_pending()`, `ts3_ceping_signal_send_pending()`,
- `ts3_audio_on_local_position_update()`.

Blieb irgendwo darin etwas haengen (eine Sperre, ein Fensteraufruf), wartete das **Spiel** auf die Antwort. Und weil
der Listener nur **ein** Thread ist, wartete auch die **naechste** Anfrage. Ausserdem lief dasselbe Notify fuer
Pos.txt-Positionen auf dem Watcher — also von zwei Threads aus.

Gemessen hat das Plugin die eigene Antwortzeit nie. Niemand konnte sagen, ob die Zeit im Plugin oder im Spiel
verloren ging.

## 3. Warum ist die neue Loesung besser?

- **Die Antwort haengt nicht mehr an fremder Arbeit.** Der Listener legt die Position ab, setzt ein Ereignis und
  antwortet. Was das Notify kostet — auch wenn es einmal haengt — betrifft die Antwort an das Spiel nicht mehr.
- **Weniger Arbeit.** Kommen mehrere Positionen an, bevor der Watcher aufwacht, gibt es nur **einen** Notify-Lauf.
  Alle Empfaenger sind „es liegt etwas an“-Signale und vertragen das.
- **Ein Thread statt zwei.** Das Notify laeuft jetzt immer auf dem Watcher (wie schon fuer Pos.txt), nie mehr
  gleichzeitig auf zwei Threads.
- **Kein neuer Verzug.** Der Watcher wird vom Ereignis **sofort** geweckt, nicht erst beim naechsten 30-ms-Takt.
  CEPOS an die anderen Spieler wird also nicht spaeter gesendet als vorher.
- **Der 30-ms-Takt bleibt 30 ms.** Er laeuft ueber eine feste Frist; 33 Positionen pro Sekunde machen ihn nicht
  haeufiger (sonst liefen Dateipruefung und Alterspruefung bei jedem Inject mit).
- **Man sieht kuenftig, wo die Zeit bleibt.** Eine Logzeile nennt Gesamtdauer und die Teilstuecke (Abschnitt 4).
  Ist `recv` gross, kam die Anfrage spaet vom Spiel; ist `work`/`reply`/`send`/`after` gross, lag es im Plugin.

**Der eine Preis:** Das Notify haengt jetzt am Watcher. Braucht dessen eigener Takt einmal laenger (z. B. die
Pfadsuche alle 15 s, solange kein gueltiger `Pos.txt`-Pfad bekannt ist), laeuft das Notify so viel spaeter — die
eigene Position erreicht die anderen Spieler dann einen Augenblick spaeter. Das Spiel merkt davon nichts.

## 4. Wie funktioniert es?

### a) Die Threads vorher und nachher

```
VORHER
 Spiel ──POST──▶ Listener-Thread:  lesen ─▶ ablegen ─▶ NOTIFY (Overlay, CEPOS, Audio ...) ─▶ ANTWORT
                                                         ▲ haengt hier etwas, wartet das Spiel

NACHHER
 Spiel ──POST──▶ Listener-Thread:  lesen ─▶ ablegen ─▶ Merker + Ereignis setzen ─▶ ANTWORT   (sofort)
                                                              │
                                                              ▼ weckt
                 Watcher-Thread:   wartet auf { Stopp, Inject }
                                     Inject  ─▶ NOTIFY (einmal, auch bei mehreren Injects)
                                     Frist 30 ms erreicht ─▶ Takt: Pos.txt / Alter pruefen (wie bisher)
```

### b) Die Warteschleife des Watchers

```
naechsteFrist = jetzt + 30 ms
Schleife:
    warte auf { Stopp, Inject } hoechstens bis naechsteFrist          (pos_wait_ms)
    Stopp           -> Ende
    Inject          -> Notify ausfuehren;  ist die Frist noch nicht erreicht -> weiter warten
    Zeitablauf      -> (oder Frist erreicht) Takt ausfuehren, naechsteFrist = jetzt + 30 ms
```

- **Merker + Ereignis:** `pos_inject_sample()` setzt den Merker `g_injectPending` auf 1. Nur wenn er vorher 0 war,
  wird das Ereignis gesetzt (ein Systemaufruf pro Notify-Lauf, nicht pro Position). Der Watcher setzt den Merker
  **vor** dem Notify auf 0 — kommt waehrenddessen eine neue Position, wird ein neuer Lauf angestossen, nichts geht
  verloren.
- **Nicht beim Beenden:** Laeuft gerade das Stopp-Ereignis, wird kein Notify mehr ausgefuehrt.
- **Reihenfolge beim Beenden unveraendert:** zuerst `pos_http_server_stop()` (Listener beendet), dann
  `pos_watcher_stop()`. Das neue Ereignis wird wie das Stopp-Ereignis erst nach dem Join geschlossen; der Schutz
  `g_watcherRunning` bleibt.

### c) Die Messung (Listener-Thread)

Pro angenommener Verbindung: Startzeit direkt nach `accept()` (QPC), Endzeit nach der Antwort bzw. nach dem
Aufgeben der Verbindung. Bei `POST /v1/position` zusaetzlich:

| Teil | von — bis |
|---|---|
| `recv` | Annahme — Kopfzeilen **und** Body komplett |
| `work` | Passwort abschneiden, parsen, Verdeckungswerte ablegen, Inject |
| `reply` | Antwort bauen (Verdeckungs-Proben, Sprecher-Liste) |
| `send` | Senden der Antwort |
| `after` | nach dem Senden bis zum Ende der Bearbeitung: Raten-Statistik und (mit `debug=1`) die Logzeile pro Position |

Nur `recv`, `work` und `reply` verzoegern **diese** Antwort. `send` und `after` halten den Listener danach noch
fest und verzoegern so die **naechste** Anfrage. Die Teile ergeben zusammen die Gesamtdauer (`total`).

Ab **20 ms** Gesamtdauer gibt es **eine** Zeile ueber `log_write` (immer sichtbar, nicht nur mit `debug=1`),
**hoechstens eine pro Sekunde**, mit dem ueblichen ` (+N suppressed)`:

```text
HTTP: slow request position status=200 total=312ms recv=305ms work=2ms reply=1ms send=4ms after=0ms
HTTP: slow request position status=200 total=251ms recv=1ms work=0ms reply=0ms send=0ms after=250ms
HTTP: slow request health status=200 total=45ms (+3 suppressed)
HTTP: slow request drop total=500ms
```

Feste Namen, nie der Pfad der Anfrage und nie der Body (dort steht das Ingame-Passwort `TSPW=`; ein Pfad aus dem
Netz koennte das Log verfaelschen): `position`, `health`, `status`, `yield`, `forbidden`, `notfound`, `drop`
(Verbindung aufgegeben: nichts empfangen, zu langsam, unvollstaendig). Die Zeile wird **nach** der Antwort
geschrieben. Zusaetzliche Systemaufrufe: nur QPC; keine Speicherreservierung; die Log-Sperre nur, wenn wirklich eine
Zeile geschrieben wird.

Zwei Stellen, an denen der Listener ohne Kunden beschaeftigt ist, werden ebenfalls gemessen
(`http_try_return_to_primary()` und `http_open_listen_socket(1)`):

```text
HTTP: port check took 250 ms
```

Ebenfalls hoechstens eine Zeile pro Sekunde (eigene Drossel). Lange Werte sind dort teilweise gewollt (das
Plugin wartet auf die Freigabe des Hauptports), die Zeile zeigt nur, **dass** der Listener in der Zeit niemanden
bedient hat.

## 5. Wie wurde es getestet?

**Hier auf dem Host gelaufen:**

- `bash tests/run_tests.sh` → `RESULT: ALL SUITES PASSED` (36 Suiten + `layering_guard`).
  - neu `pos_tick_test` (16 Pruefungen): `pos_wait_ms` (volle Frist, Rest, faellig, ueberschritten, Deckel,
    64-Bit-Zeiten) und eine Nachbildung der Watcher-Schleife mit falscher Uhr: Takt bleibt bei 100 pro 3 s,
    auch wenn alle 30 / 7 / 1 ms ein Inject kommt; jeder Inject weckt den Watcher.
  - `http_request_test` Block [7] (120 Pruefungen insgesamt): Ticks → ms (Rundung, Frequenz 0, Ueberlauf), Schwelle
    20 ms, genauer Text der Logzeilen (mit/ohne Teile, mit/ohne `suppressed`), kleiner Puffer wird abgeschnitten,
    Drossel ~1 Zeile pro Sekunde.
- `bash build/build_mingw.sh`: baut und **linkt** die DLL wieder, 0 Warnungen (vorher brach das Linken ab, siehe
  Tabelle in Abschnitt 1). Die geaenderten `.c`-Dateien zusaetzlich streng
  (`-Wconversion -Wshadow -Wmaybe-uninitialized`): 0 Warnungen.
- `tests/http_takeover_itest.c` (stubbt `pos_inject_sample`) bleibt unveraendert und uebersetzt weiter.

**Nicht geprueft (offen):**

- **Der Ablauf im echten TeamSpeak-Client auf Windows.** Ob das Notify vom Watcher aus wirklich sofort laeuft, ob die
  Ruckler verschwinden, ob CEPOS an andere Spieler gleich schnell geht. Das Pos-Watcher-Verhalten selbst
  (`pos_file.c`) wird auf dem Host nicht ausgefuehrt (nur Windows).
- **Der MSVC-Build** (`/W4 /WX`): hier nur durch die MinGW-Naeherung abgedeckt.
- **`tests/slow_reply_server.ps1` ist UNGETESTET.** Es gibt hier keine PowerShell. Das Skript wurde nur Zeile fuer
  Zeile durchgesehen (Windows PowerShell 5.1, keine Module, nur ASCII). Wer es nutzt, prueft zuerst, ob es startet
  und auf `GET http://127.0.0.1:52734/v1/status` antwortet (z. B. im Browser).
- **Die Mod-Seite.** Ob das Spiel ueberhaupt auf die Antwort wartet und was die Mod pro Runde im Spiel-Thread tut
  (`Pos.txt` schreiben, Anfrage starten, anderen Port fragen), wird getrennt am PC mit DevKit und Logs geprueft.
  Wartet das Spiel grundsaetzlich auf jede Antwort, braucht es zusaetzlich eine Aenderung in der Mod.

**Handwerkzeug `tests/slow_reply_server.ps1`** — zur Frage „ruckelt das SPIEL, wenn eine Antwort spaet kommt?“:

```text
TeamSpeak schliessen (es haelt Port 52734) → in PowerShell:
powershell -ExecutionPolicy Bypass -File tests\slow_reply_server.ps1 -DelayMs 300 -EveryMs 3000
→ Spiel starten und spielen.
```

Das Skript antwortet auf `POST /v1/position` mit `{"ok":true,"talk":0,"talkers":"","act":1}`, auf
`GET /v1/status` mit der Plugin-Antwort (damit der Mod keinen anderen Port sucht), alles andere mit 404. Es bedient
Verbindungen nacheinander wie das Plugin und haelt **nur** Positions-Antworten zurueck: `-DelayMs` (Standard 300)
lang, einmal alle `-EveryMs` (Standard 3000; `0` = jede Antwort). Pro langsamer Antwort eine Zeile, alle 5 s eine
Zaehlerzeile, `Ctrl+C` beendet. Jede Verbindung hat eine Lese-Zeitgrenze, ein stummer Kunde haelt es nicht auf.

- **Ruckelt das Spiel genau dann, wenn eine Zeile erscheint:** eine verspaetete Antwort allein reicht, das Plugin
  muss nur schnell bleiben (dann zeigt `plugin.log`, ob es das tut).
- **Ruckelt es nicht:** der Haenger liegt woanders (andere Ursache im Spiel, im Mod oder im Netz-Stack).

## 6. Lerneffekt

Ein Antwort-Thread darf nie fremde Arbeit erledigen, bevor er antwortet: Was „schnell sein sollte“ (ein Overlay-
Aufruf, eine Sperre), ist es nicht immer, und der Schaden trifft den, der wartet — hier das Spiel. Die saubere Form
ist „ablegen, Signal setzen, antworten“; die Arbeit macht ein anderer Thread. Und: **erst messen, dann raten.**
Ohne die Zeitmessung im Plugin blieb offen, ob es das Plugin war — die Messung ist deshalb Teil derselben Aenderung.
