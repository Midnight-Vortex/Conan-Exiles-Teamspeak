# 105 — Die HTTP-Antwort an das Spiel wartet nicht mehr auf Plugin-Arbeit + Messung langsamer Antworten (Plugin 8.0.26)

**Plugin 8.0.26**, 2026-11-26. Gebaut und auf dem Host getestet, **nicht** im TeamSpeak-Client auf Windows geprueft,
**nicht** verteilt (kein Release; die Hashes im README kommen erst mit dem Windows-Build).

> **Nachtrag vom selben Tag — wichtig fuer die Einordnung:** Der Messtest des Besitzers
> ([Doku 106](106-ruckler-messung-mod-anfrage.md)) hat gezeigt: **Das Spiel wartet nicht auf die Antwort des
> Plugins.** Mit absichtlich 300 ms verspaeteten Antworten ruckelte es nicht, selbst bei 69 gestauten Anfragen.
> Die Ruckler vom 25.11. kommen also **nicht** von einer langsamen Plugin-Antwort, und **diese Aenderung behebt sie
> nicht.** Was sie bringt: Die Antwort haengt nicht mehr an fremder Plugin-Arbeit, und das `plugin.log` eines
> betroffenen Spielers zeigt kuenftig schwarz auf weiss, ob das Plugin spaet war (`HTTP: slow request ...`) oder
> nicht (keine solche Zeile).

**Ausloeser:** Spieler melden Ruckler im Spiel. Das Spiel schickt seine Position etwa 33-mal pro Sekunde an
`POST http://127.0.0.1:52734/v1/position`. Normal dauert eine Anfrage rund 7 ms. Etwa alle 3 Sekunden dauerte eine
80 ms, spaeter bis 480 ms — und laut der ersten Meldung stand das Spiel so lange.

**Ehrlich vorweg: Den Ausloeser dieser 3-Sekunden-Haenger kennen wir noch nicht.** Die Suche hat im Plugin keinen
Auftrag gefunden, der alle 3 Sekunden laeuft. Gefunden wurden zwei Schwaechen auf dem Antwortweg. Diese Aenderung
(1) beseitigt das **eigene Warten** des Plugins vor der Antwort und (2) **misst** jetzt, wie lange das Plugin
braucht — damit man kuenftig sagen kann, ob die Zeit im Plugin oder im Spiel verloren geht. (Die Annahme beim
Schreiben war noch „das Spiel wartet auf die Antwort“; siehe Nachtrag oben.)

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
| `src/ts/entry/ts3_entry.c` | Version 8.0.26. |
| `src/ui/overlay/voice_overlay.h`, `src/ui/plugin_ui_compat.c/.h` | Nur Kommentare: `plugin_ui_on_position_tick` wird nicht mehr vom HTTP-Listener gerufen, nur noch vom Watcher. |
| `build/build_mingw.sh` | `-liphlpapi` in der Bibliotheksliste ergaenzt. Seit 102 nutzt `pos_http_server.c` die Funktion `GetExtendedTcpTable`; im vcxproj stand `iphlpapi.lib` schon, im MinGW-Skript fehlte sie. Der Pruef-Build (und damit die CI) brach seitdem beim Linken ab. |

## 2. Wie war es vorher?

`http_handle_client()` rief `pos_inject_sample()` **vor** dem Senden der Antwort. Und `pos_inject_sample()` fuehrte
das Notify **sofort auf dem Listener-Thread** aus (`ts3_on_position_update_common()`):

- `plugin_ui_on_position_tick()`: nimmt die Sperre des Server-Profils, kopiert Einstellungen, ruft user32 fuer das
  Overlay (`IsWindow`, `GetWindowThreadProcessId`, `PostMessage`),
- `chan_signal_position_update()`, `cepos_signal_send_pending()`, `ts3_ceping_signal_send_pending()`,
- `ts3_audio_on_local_position_update()`.

Blieb irgendwo darin etwas haengen (eine Sperre, ein Fensteraufruf), kam die Antwort an das **Spiel** entsprechend
spaeter. Und weil der Listener nur **ein** Thread ist, wartete auch die **naechste** Anfrage. Ausserdem lief dasselbe Notify fuer
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
                                                         ▲ haengt hier etwas, kommt die Antwort spaet

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

- **Der Ablauf im echten TeamSpeak-Client auf Windows.** Ob das Notify vom Watcher aus wirklich sofort laeuft und
  ob CEPOS an andere Spieler gleich schnell geht (Pruefliste unten). Das Pos-Watcher-Verhalten selbst
  (`pos_file.c`) wird auf dem Host nicht ausgefuehrt (nur Windows).
- **Der MSVC-Build** (`/W4 /WX`): hier nur durch die MinGW-Naeherung abgedeckt.
- **Die Mod-Seite** wurde getrennt am PC geprueft: Ergebnis in [Doku 106](106-ruckler-messung-mod-anfrage.md).
  Das Spiel wartet nicht auf die Antwort; an der Mod wurde nichts geaendert.

**Vor dem Verteilen im TeamSpeak-Client pruefen (Windows):**

1. Verbinden, ins Spiel: Der Spieler wird in den Ingame-Channel verschoben, andere hoeren ihn richtungs- und
   entfernungsabhaengig (das Notify laeuft jetzt ueber den Watcher — kommt es nicht an, bleiben die Positionen bei
   den anderen stehen).
2. Laufen und stehen bleiben: Die Stimme der anderen folgt ohne spuerbaren Verzug; das HUD zeigt Zone und
   Sprechart wie vorher; die Mundbewegung im Spiel laeuft.
3. `plugin.log`: im normalen Betrieb **keine** Zeile `HTTP: slow request`. Zum Gegentest TeamSpeak kurz anhalten
   (z. B. ein Einstellungsfenster verschieben) — es darf hoechstens eine Zeile pro Sekunde erscheinen.
4. Zwei TeamSpeak-Instanzen: Port-Uebergabe wie in Doku 102; dabei darf `HTTP: port check took N ms` erscheinen.
5. TeamSpeak beenden waehrend das Spiel laeuft: kein Haenger beim Beenden.

**Werkzeug fuer die Frage „ruckelt das SPIEL, wenn eine Antwort spaet kommt?“:** `tests/slow_reply_standin.ps1`
und `tests/modhttp_timing.py` aus [Doku 106](106-ruckler-messung-mod-anfrage.md) (vom Besitzer am 26.11. benutzt).
Ein zuerst hier mitgeliefertes, ungetestetes Skript mit demselben Zweck (`tests/slow_reply_server.ps1`) wurde
deshalb wieder entfernt.

## 6. Lerneffekt

Ein Antwort-Thread darf nie fremde Arbeit erledigen, bevor er antwortet: Was „schnell sein sollte“ (ein Overlay-
Aufruf, eine Sperre), ist es nicht immer, und der Schaden trifft den, der auf die Antwort wartet. Die saubere Form
ist „ablegen, Signal setzen, antworten“; die Arbeit macht ein anderer Thread. Und: **erst messen, dann raten.**
Genau das hat sich hier gezeigt: Die Vermutung „das Spiel wartet auf das Plugin“ war plausibel, aber falsch — der
Messtest (Doku 106) hat sie in fuenf Minuten widerlegt.
Ohne die Zeitmessung im Plugin blieb offen, ob es das Plugin war — die Messung ist deshalb Teil derselben Aenderung.
