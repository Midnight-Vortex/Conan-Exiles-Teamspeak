# 085 — Review-Fixes: HTTP-Server, plugin.cfg, Hub-Parser, Pos-Datei (Paket CONFIG-IO)

Ein vollständiges Code-Review hat 16 Fehler im Bereich „Dateien, Config, HTTP“ gefunden.
Dieser Eintrag erklärt jeden einzelnen: **was**, **wie vorher**, **warum besser**, **wie es
jetzt funktioniert**, **wie getestet**, **Lerneffekt**. Der HTTP-Vertrag mit dem Spiel-Mod
(`doku/module/pos-http.md`) bleibt unverändert — URL, Felder und Antworten sind gleich.

## Übersicht

| # | Thema | Datei(en) |
|---|---|---|
| 1 | Webseiten konnten den lokalen Server steuern | `pos_http_server.c`, `http_request.h` (neu) |
| 2 | Halber Body wurde ausgewertet | `pos_http_server.c`, `http_request.h` |
| 3 | Kein Zeitlimit pro Verbindung | `pos_http_server.c` |
| 4 | `Content-Length` falsch gelesen | `http_request.h` |
| 5 | Listener-Wiederbelebung | `pos_http_server.c`, `http_request.h` |
| 6 | `SO_REUSEADDR` immer an | `pos_http_server.c` |
| 7 | Ungedrosselte Fehler-Logs | `pos_http_server.c`, `http_request.h` |
| 8 | `config_save` weder atomar noch gesperrt | `config.c`, `config.h` |
| 9 | Kodierung von `plugin.cfg` | `config.c`, `config_text.h` (neu) |
| 10 | `config_modify` (Vertrag C1) | `config.c`, `config.h` |
| 11 | NaN/Inf in der Config | `config_text.h` |
| 12 | Komma-Dezimalzahlen | `num_parse.h` (neu), `config_text.h`, `hub_parser.c` |
| 13 | Autodetect überschreibt Config | `pos_file.c`, `path_detect.c` |
| 14 | Pos.txt-Parser zu lasch | `pos_parse.h` (neu), `pos_file.c` |
| 15 | Watcher macht doppelte Arbeit | `pos_file.c` |
| 16 | Passwort bleibt nach Ablauf im Speicher | `mod_password.c` |

Neue Dateien sind **nur Header** (`static inline`-Funktionen). Warum? Der Windows-Code
(Sockets, Dateien) lässt sich auf dem Linux-Testrechner nicht ausführen, die reine Logik
(„ist diese Kopfzeile erlaubt?“, „ist diese Zahl gültig?“) schon. Also steht die Logik im
Header und wird von `pos_http_server.c` **und** den Tests benutzt — die Tests prüfen genau
den Code, der später läuft.

Tests: `tests/http_request_test.c` (98 Prüfungen), `tests/pos_parse_test.c` (45),
`tests/config_text_test.c` (57), erweitert: `tests/hub_parser_test.c`,
`tests/mod_password_test.c`.

---

## 1. Webseiten konnten den lokalen HTTP-Server steuern (HOCH)

**Was:** Neue Kopfzeilen-Prüfung (`http_request_guard` in `http_request.h`), aufgerufen in
`http_handle_client` für **jeden** Endpunkt. Bei Verstoß: HTTP 403 + gedrosselte Logzeile.

**Vorher:** Der Server schaute nur auf `Content-Length`. Jede Webseite im Browser des
Spielers konnte mit `fetch("http://127.0.0.1:52734/v1/position", {method:"POST",
mode:"no-cors", body:"… TSPW=evil"})` eine Position einschleusen (die dann an alle
Mitspieler gesendet wird), das Ingame-Passwort überschreiben oder löschen und die
Verdeckung (Occlusion) setzen. Ein `text/plain`-POST braucht keine „Preflight“-Rückfrage
des Browsers. Mit „DNS-Rebinding“ (eine fremde Domain zeigt plötzlich auf 127.0.0.1) kann
die Seite sogar die Antworten lesen — also die Positionen der Spieler in der Nähe.

**Warum besser:** Browser verraten sich selbst — und das können Webseiten nicht fälschen:

- Bei jedem fremden POST sendet der Browser `Origin: https://seite.example`.
- Moderne Browser senden `Sec-Fetch-Site: cross-site` bzw. `same-site`.
- Nach DNS-Rebinding steht im `Host:` die fremde Domain statt `127.0.0.1`.

Das Spiel (CEE „Start Http Request“) ist kein Browser: keine `Sec-Fetch-*`-Header, `Host`
kommt von CEE aus der URL (`127.0.0.1:52734`). Sendet CEE einen `Origin:`, dann seine
freigegebene Origin `http://127.0.0.1:52734` — die wird ausdrücklich erlaubt (eine
Webseite kann diese Origin nie haben, weil das Plugin keine Seiten ausliefert).

**Wie funktioniert es:**

```
Request ──► Header-Block lesen (nur bis zur Leerzeile, nie im Body!)
              │
              ├─ Origin vorhanden und nicht http://127.0.0.1:52734 / http://localhost:52734 ──► 403
              ├─ Sec-Fetch-Site = cross-site oder same-site ───────────────────────────────► 403
              ├─ Host vorhanden und nicht 127.0.0.1 / localhost / [::1] (Port egal) ────────► 403
              └─ sonst ──► normal weiter (GET /health, POST /v1/position, 404)
```

Jede 403 schreibt (max. 1×/s, mit „+N suppressed“):
`HTTP: 403 rejected header Origin="https://evil.example"` — Header-Name **und** Wert.
Falls CEE je blockiert würde, sieht man es sofort im `plugin.log`.

**Test:** `tests/http_request_test.c`: der Angriff aus dem Review (Origin fremd, `null`,
Sec-Fetch cross-site/same-site, Host `attacker.example`, Look-alike-Hosts wie
`127.0.0.1.attacker.example`, doppelte Header, „Header“ im Body) wird abgelehnt; ein
CEE-artiger POST, `Origin: http://127.0.0.1:52734`, `Host: localhost`, fehlender Host und
die Adresszeilen-Navigation (`Sec-Fetch-Site: none`) laufen durch. **Im Spiel prüfen:**
Positionen kommen an (`POS: method none -> http` im Log) und es gibt keine
`HTTP: 403 rejected header …`-Zeile.

**Lerneffekt:** „Nur localhost“ ist kein Schutz, solange ein Browser auf dem Rechner läuft.
Jeder lokale Dienst ohne Anmeldung muss Origin/Host prüfen.

---

## 2. Abgeschnittener Body wurde ausgewertet

**Was:** In `http_handle_client` wird nach dem Lesen geprüft, ob der Body so lang ist wie
angekündigt (`http_body_incomplete`); sonst HTTP 400 und Abbruch. Die Body-Länge wird auf
das begrenzt, was wirklich angekommen ist (`http_body_len`). Ein `Content-Length`, der
nicht in den Puffer passt, wird sofort abgelehnt.

**Vorher:** Kam der Body verspätet oder zu kurz, brach die Lese-Schleife einfach ab und der
**Teil** des Bodys wurde ausgewertet — ein halbes Passwort (`TSPW=Abc` statt `TSPW=Abcdef`)
wurde gespeichert, danach scheiterte der Ingame-Wechsel. Außerdem wurde als Body-Länge
die *angekündigte* Länge benutzt: waren weniger Bytes da, las das Log-Format
(`http_format_raw`) alte Bytes vom Stack mit.

**Warum besser:** Entweder der Body ist komplett, oder er wird gar nicht angefasst. Die
Länge kann nie größer sein als die tatsächlich empfangenen Bytes. Nach dem angekündigten
Body wird zusätzlich ein `\0` gesetzt, damit nichts dahinter geparst wird.

**Wie:**

```
Header komplett ──► Content-Length lesen (siehe 4.)
                     ├─ ungültig ───────────► 400 reason=content-length
                     ├─ passt nicht in 4 KB ► 400 reason=body-too-large
                     └─ gültig: genau so viele Bytes nachlesen (mit Zeitlimit, siehe 3.)
                          ├─ zu wenig angekommen ► 400 reason=body-incomplete   (Body wird NICHT geloggt)
                          └─ komplett ──► Passwort abschneiden ──► parsen ──► antworten
```

Ohne `Content-Length` (alte Clients) gilt wie bisher: was mit den Headern kam, ist der Body.

**Test:** `http_request_test`: `http_body_len` (nie mehr als angekommen, nie negativ),
`http_body_incomplete`, 3001-Byte-Body bei 3000 Platz → „too big“. **Im TS-Client:**
normales Spielen — keine Änderung sichtbar.

**Lerneffekt:** Längenangaben aus dem Netz nie blind übernehmen: immer gegen das prüfen, was
wirklich da ist.

---

## 3. Kein Gesamt-Zeitlimit pro Verbindung

**Was:** `HTTP_CONN_DEADLINE_MS` = 500 ms ab `accept()`. Beide Lese-Schleifen (Header und
Body) prüfen vor jedem `recv` die absolute Frist.

**Vorher:** Jedes `recv` durfte 100 ms warten — aber ein lokaler Client, der alle 90 ms
1 Byte schickt, hielt den **einzigen** Listener-Thread minutenlang beschäftigt. Alle
echten POSTs blieben liegen, und `pos_http_server_stop()` (wartet bis der Thread fertig
ist, notfalls unendlich) hing beim Beenden von TeamSpeak.

**Warum besser:** Nach spätestens ~600 ms (500 ms Frist + ein letztes 100-ms-`recv`) ist
die Verbindung zu Ende. Ein normaler Mod-POST über Loopback braucht wenige Millisekunden.
Neuer Abbruchgrund im Log: `HTTP: drop reason=deadline`.

**Test:** Zeitlogik ist Win32 (`GetTickCount64`), nicht auf dem Host ausführbar; geprüft
per Cross-Compile ohne Warnungen und durch Lesen. **Manuell:** ein Skript, das 1 Byte
pro 90 ms schickt, blockiert den Server nur noch 0,5 s.

**Lerneffekt:** Ein Timeout pro Lesevorgang ist kein Gesamt-Timeout. Wer einen Thread
teilt, braucht eine absolute Frist.

---

## 4. `Content-Length` wurde falsch gelesen

**Was:** `http_content_length` in `http_request.h`.

**Vorher:** `strstr(buf, "Content-Length:")` und `strstr(buf, "content-length:")` über den
**ganzen** Puffer — also auch im Body und in `X-Content-Length:`; nur zwei Schreibweisen;
`atoi` ohne Überlaufprüfung.

**Warum besser:** Nur im Header-Block, Groß-/Kleinschreibung egal, nur reine Ziffern,
`strtol` mit Bereichsprüfung. Alles andere (`abc`, `-5`, `+5`, `1e3`, `0x10`, leer, zu
groß, widersprüchliche Doppel-Header) → HTTP 400 statt stillem Raten.

**Test:** 98 Prüfungen in `http_request_test` (u. a. alle genannten Fälle).

**Lerneffekt:** Zahlen aus fremden Daten streng parsen und den Fehlerfall behandeln.

---

## 5. `accept()`-Fehler und Neustart des Listeners

**Was:** Der Listener-Thread besitzt jetzt den Listen-Socket und bindet bei Bedarf neu.
`start()` startet den Thread auch, wenn das erste `bind` scheitert.

**Vorher:** Jeder `accept`-Fehler riss den Socket ab (mindestens 250 ms Ausfall) — auch
harmlose, die nur *eine* Verbindung betreffen. Schlug das Neu-Binden dauerhaft fehl,
kamen ~8 Logzeilen pro Sekunde für immer. Schlug das erste `bind` beim Start fehl,
gab es gar keinen Thread — HTTP kam nie wieder, bis TeamSpeak neu gestartet wurde.

**Warum besser:**

```
accept() Fehler
   ├─ WSAECONNRESET / ECONNABORTED / EINTR / EWOULDBLOCK ──► weitermachen (Socket ist ok)
   │    (höchstens 50× in Folge, dann wie "anderer Fehler")
   └─ anderer Fehler ──► Socket zu ──► warten ──► neu binden
Wartezeit: 250, 500, 1000, 2000, 4000, 5000 ms (Maximum) — wieder von vorn nach einem
           erfolgreich bedienten Client. Gewartet wird auf das Stop-Event → stop() bleibt sofort.
Bind-Fehler im Log: höchstens alle 30 s (mit Zähler).
```

**Test:** `http_request_test`: Fehlerklassifizierung (`http_wsa_error_is_transient`) und die
Backoff-Reihe (`http_backoff_ms`: 250→…→5000, nie fallend). Die Socket-Schleife selbst
ist Win32 (Cross-Compile). **Manuell:** Port 52734 mit einem anderen Programm belegen,
TeamSpeak starten (Log: bind-Fehler, nur alle 30 s), Programm beenden → `HTTP: listening on`
ohne TS-Neustart.

**Lerneffekt:** Fehler nach ihrer Reichweite behandeln (eine Verbindung vs. der ganze
Socket) und Wiederholungen immer mit wachsender Wartezeit und gedrosseltem Log.

---

## 6. `SO_REUSEADDR` war immer an

**Was:** `http_open_listen_socket` bindet zuerst **ohne** `SO_REUSEADDR`.

**Vorher:** Unter Windows erlaubt `SO_REUSEADDR`, dass ein *anderes* Programm denselben
Port ebenfalls bindet (und Verbindungen abfängt).

**Warum besser (konservativ):** Plain `bind` zuerst. Nur wenn der Port als belegt gemeldet
wird (`WSAEADDRINUSE`, z. B. die eigene alte Instanz nach einem Plugin-Neustart), wird mit
`SO_REUSEADDR` erneut versucht und eine Warnung geloggt — so funktioniert der Neustart
wie früher, aber der Normalfall ist exklusiv.

**Test:** Cross-Compile; **manuell:** Plugin neu laden → ggf. `WARNING port 52734 was in use`.

**Lerneffekt:** Socket-Optionen heißen auf jedem Betriebssystem ähnlich, bedeuten aber
Unterschiedliches. `SO_REUSEADDR` ist unter Windows deutlich „offener“ als unter Linux.

---

## 7. Fehler-Logs waren nicht gedrosselt

**Was:** Gemeinsame Drosselung `http_throttle_pass` (max. 1 Zeile/s je Art, plus Zähler
„+N suppressed“) für: abgelehnte POSTs (Parse/Reject), 403, 404/unbekannte Anfragen,
Abbrüche, `accept`-Fehler.

**Vorher:** Jeder abgelehnte POST schrieb bis zu 420 Byte Rohdaten ins Log. Ein Mod im
Hauptmenü (Position `0,0,0`) macht ~33 POSTs pro Sekunde → 33 Zeilen/s. Jede 404-Anfrage
auch.

**Warum besser:** Das Log bleibt lesbar und wächst nicht mehr durch einen einzigen
Client. Die Information bleibt (letztes Beispiel + Anzahl).

**Test:** `http_request_test`: 33 Anfragen/s über 10 s → 10–11 Logzeilen; Zähler stimmt.

**Lerneffekt:** Alles, was ein Fremder auslösen kann, muss im Log gedrosselt werden.

---

## 8. `config_save` war weder atomar noch gesperrt

**Was:** `config_save` in `config.c`: eigener Speicher-Lock, schreibt `plugin.cfg.tmp`,
prüft jeden Schreibschritt, `FlushFileBuffers`, dann `MoveFileExW(…, REPLACE_EXISTING |
WRITE_THROUGH)`. Bei Fehler: Rückgabe 0, **alte Datei bleibt**.

**Vorher:** `_wfopen("w")` leert die Datei sofort. Ein Absturz oder eine volle Platte
mittendrin hinterließ eine abgeschnittene `plugin.cfg` (Rückgabewerte wurden nicht
geprüft). Außerdem riefen Dialog-, Callback- und Pos-Watcher-Thread `config_save` ohne
Sperre auf — zwei Schreiber konnten sich gegenseitig die Datei zerstören.

**Warum besser:**

```
config_save():
  Speicher-Lock ──► Snapshot von g_config (INNERHALB des Locks → der spätere Aufrufer
                    hat immer den neueren Stand und ist als Letzter fertig)
                ──► Text bauen ──► plugin.cfg.tmp schreiben + flushen
                ──► umbenennen über plugin.cfg   (ein Schritt; alt oder neu, nie halb)
```

Liegt die Datei kurz offen (Dialog liest sie, Virenscanner), wird der Tausch bis zu 5×
nach kurzer Wartezeit wiederholt. Lock-Reihenfolge immer Speicher-Lock → Config-Lock.

**Test:** Win32 (Dateien) — Cross-Compile ohne Warnungen; die Zeilenausgabe ist identisch
zu früher (CRLF, gleiche Reihenfolge). **Manuell:** Einstellungen ändern → Datei prüfen;
während des Speicherns TS beenden → Datei nie halb.

**Lerneffekt:** Dateien nie „an Ort und Stelle“ überschreiben: erst vollständig daneben
schreiben, dann in einem Schritt austauschen.

---

## 9. Kodierung von `plugin.cfg` (Kyrillisch/Chinesisch im Steam-Pfad)

**Was:** `config_load` liest die Datei als Bytes und entscheidet selbst; `config_save`
schreibt ohne BOM.

**Vorher:** `_wfopen("r"/"w")` im ANSI-Textmodus. `fwprintf` mit einem Pfad, dessen Zeichen
nicht in die ANSI-Codepage passen (Kyrillisch, Chinesisch), bricht mitten in der Zeile ab —
die Zeilen klebten zusammen.

**Warum besser:**

- **Lesen:** Sind die Bytes gültiges UTF-8 → als UTF-8 lesen, sonst als ANSI
  (`cfg_utf8_is_valid`). Alte Dateien (ANSI) lesen deshalb unverändert; ein UTF-8-BOM
  (von Notepad) wird übersprungen.
- **Schreiben:** Reine ASCII-Dateien sind **byte-identisch** zu früher. Nicht-ASCII-Pfade
  werden in der ANSI-Codepage gespeichert, **wenn das verlustfrei und eindeutig ist**
  (Umlaute usw. — so bleiben die anderen Stellen, die `plugin.cfg` noch selbst im
  ANSI-Modus lesen, weiter korrekt). Passt es nicht (Kyrillisch/Chinesisch auf einem
  westlichen System), wird UTF-8 ohne BOM geschrieben. Eine Datei, die als ANSI
  geschrieben wird, ist nie zufällig auch gültiges UTF-8 (sonst würde sie falsch
  gelesen) — das prüft `cfg_ansi_bytes_are_unambiguous`.
- Ist die Datei zwar da, aber gerade nicht lesbar (gesperrt), werden Standardwerte nur im
  Speicher benutzt und die Datei **nicht überschrieben** (vorher: Defaults über die
  Benutzerdatei).

**Test:** `config_text_test`: UTF-8-Gültigkeit (Umlaute, Kyrillisch, CJK, 4-Byte, überlange
Formen, Surrogate, abgeschnitten, einzelnes `0xFC`), Mehrdeutigkeitsregel, Parsen von
Nicht-ASCII-Pfaden. Die Codepage-Umwandlung selbst (`MultiByteToWideChar`) ist Win32.
**Manuell:** Steam-Pfad mit „ü“ und mit „Игры“ setzen, TS neu starten, Pfad bleibt.

**Lerneffekt:** „Text“ hat immer eine Kodierung. Beim Lesen eines alten Formats beide
Möglichkeiten unterstützen; beim Schreiben das kompatibelste Format wählen.

---

## 10. `config_modify` (Vertrag C1 für die anderen Pakete)

**Was:** `typedef void (*ConfigMutator)(PluginConfig*, void*)` und
`void config_modify(ConfigMutator fn, void* ctx)` in `config.h`/`config.c`; die veraltete
Thread-Beschreibung in `config.h` ist neu geschrieben.

**Vorher:** Wer nur ein Feld ändern wollte, musste `config_copy` → ändern →
`config_apply` machen. Zwischen Kopie und Rückschreiben konnte ein anderer Thread etwas
ändern — das ging verloren („Lost Update“).

**Warum besser / wie:** `config_modify` führt `fn(&g_config, ctx)` **unter dem Config-Lock**
aus, ruft danach `config_clamp` auf und speichert **nicht** (dafür `config_save()`).
Jeder Thread ändert nur seine eigenen Felder.

```
Thread A (Dialog)         Thread B (Pos-Watcher)
config_modify(feldA) ──┐   config_modify(automaticSavedPath) ──┐
                       ▼                                        ▼
                 [   g_config  unter Lock  ]   → keine verlorenen Änderungen
```

**Test:** Win32-Lock; im Pos-Watcher (Punkt 13) produktiv benutzt. **Lerneffekt:**
Teil-Updates statt „ganzes Objekt zurückschreiben“ vermeiden Lost Updates.

---

## 11. NaN/Inf in der Config machten uns stumm

**Was:** `cfg_clamp_values` (`config_text.h`, aufgerufen von `config_clamp`).

**Vorher:** `DistanceNormal=nan` (oder `inf`, `1e999`) lief durch die Bereichsprüfung
(`NaN < 0.5` ist falsch, `NaN > 500` auch) → NaN als Sprechdistanz → Mitspieler lehnen
unsere Positionsmeldung ab, wir sind unhörbar.

**Warum besser:** Nicht endliche Distanzen werden auf den Standardwert gesetzt
(3 / 13 / 26 m), erst danach auf 0,5…500 begrenzt. Die Standardwerte stehen jetzt in
`config_text.h` und werden auch von `config_set_defaults` benutzt (keine Doppelung).

**Test:** `config_text_test` (nan, inf, -inf, 1e999 in der Datei und direkt in der
Struktur; normale Werte unverändert).

**Lerneffekt:** Bei Gleitkommazahlen reicht `< min`/`> max` nicht — NaN besteht beide
Vergleiche. `isfinite` zuerst.

---

## 12. Komma-Dezimalzahlen („13,5“) wurden als 13 gelesen

**Was:** `num_strtod` in `src/core/util/num_parse.h`; benutzt von `config_text.h` (Distanzen)
und `hub_parser.c` (alle Einzelzahlen).

**Vorher:** `strtod("13,5")` hört am Komma auf → 13. Deutsche Admins/Spieler schreiben
natürlich `13,5`.

**Warum besser (und sicher):** Genau **ein** Komma, direkt zwischen zwei Ziffern und
kein Punkt in derselben Zahl → Dezimalkomma. Alles andere verhält sich exakt wie vorher
(`1,000.5` → 1, `7,2,1` → 7, `,5` → 0, Tippfehler → 0 → wie bisher begrenzt).
Geprüft wurde, welche Hub-Schlüssel Listen sind: nur `SteamID=(Name)7656…,(Name2)…`
(eigener Parser, unberührt). Zonen-Koordinaten (`X1=`…) und alle Distanzen sind
Einzelzahlen. Ganzzahl-Schlüssel (`DefaultWhisperKey`, Tasten, Hud…) bleiben `atoi`.

**Test:** `hub_parser_test` Abschnitt [10] (Distanzen, Lautstärke, Zonen, Rassen,
Occlusion-Parameter, Sonderformen, SteamID-Liste mit 3 Einträgen bleibt 3) und
`config_text_test` [1]/[5].

**Lerneffekt:** Vor einer „kleinen“ Parser-Erleichterung prüfen, wo das Zeichen schon eine
andere Bedeutung hat (hier: Listen-Trenner).

---

## 13. Pfad-Autodetect überschrieb die ganze Config (Lost Update)

**Was:** `pos_autodetect_saved_path` (`pos_file.c`) ändert nur noch `automaticSavedPath` über
`config_modify` (Compare-and-set), dann `config_save`. Die Meldung „PATH: Conan Exiles
(440900) not found in any Steam library“ steht in `path_detect.c` und wird höchstens alle
10 Minuten geloggt (erste Meldung sofort).

**Vorher:** Alle 15 s lief auf dem Pos-Watcher-Thread: `config_copy` → langsame
Registry/VDF-Suche → `config_apply` der **ganzen** Struktur + `config_save`. Hat der Spieler
währenddessen im Dialog etwas geändert, wurde es mit dem alten Stand überschrieben. Und bei
erfolgloser Suche stand alle 15 s dieselbe Zeile im Log.

**Warum besser:** Compare-and-set: Der Pfad wird nur geschrieben, wenn das Feld noch den
Wert hat, mit dem die Suche begann — hat der Dialog inzwischen einen Pfad gesetzt, bleibt
dessen Pfad. Die Such-Bedingungen und das 15-s-Intervall sind unverändert.

**Test:** Win32 (Registry/Dateien). Die gleiche Idee ist in `config_modify` dokumentiert.
**Manuell:** Steam-Pfad leeren, im Dialog gleichzeitig etwas ändern → Änderung bleibt.

**Lerneffekt:** Langsame Arbeit außerhalb von Locks, das Ergebnis aber nur als kleines,
bedingtes Update einspielen.

---

## 14. Pos.txt-Parser prüfte zu wenig

**Was:** `pos_parse_line` / `pos_sample_is_sane` / `pos_sample_is_plausible` in
`src/core/mod_file/pos_parse.h`; `pos_file_read_once` und die HTTP-Annahme benutzen sie.

**Vorher:** `yaw`/`yawY` wurden nie auf „endlich“ geprüft; `x=1e300` ist endlich, wird aber
bei `(float)(x/100)` zu „unendlich“; `strtod` ohne `endptr` → ein leeres `YAW=` zählte als
`0.0`.

**Warum besser:** Jeder Schlüssel braucht eine echte Zahl (leer/Text → abgelehnt; das
optionale `YAWY=` bleibt optional). Alle fünf Werte müssen endlich sein, Koordinaten und
Winkel höchstens `1e8` (1000 km — weit größer als jede Conan-Karte). `seq` wird ohne undefiniertes
Verhalten in `int` gewandelt (große Werte werden begrenzt). Akzeptierte Formate sonst
unverändert. Da `pos_inject_sample` dieselbe Prüfung benutzt, gilt sie auch für den
HTTP-JSON-Weg.

**Test:** `tests/pos_parse_test.c` (45 Prüfungen): akzeptierte Formate, leere/ungültige
Felder, `1e300`, `inf`, `nan`, `1e999`, Grenze `1e8`, `seq`-Überlauf, `(0,0,0)`-Platzhalter.

**Lerneffekt:** Beim Parsen immer drei Fragen: Gibt es eine Zahl? Ist sie endlich? Ist sie
sinnvoll groß?

---

## 15. Pos-Watcher: doppelte Arbeit pro Tick

**Was:** `pos_file.c`: eine Verzeichnisabfrage (`FindFirstFileW`) pro 30-ms-Tick statt zwei;
während des HTTP-Holds wird `Pos.txt` nicht mehr geöffnet/gelesen.

**Vorher:** `pos_file_write_age_ms` und `pos_get_file_write_quad` fragten dieselbe Datei
zweimal ab. Und obwohl der Kommentar sagte „Hold: Datei-Lesen wird übersprungen“, wurde
die Datei trotzdem geöffnet, gelesen und das Ergebnis verworfen (bei Sperrkonflikten mit
Wartezeiten bis in den zweistelligen Millisekundenbereich).

**Warum besser:** Weniger Systemaufrufe, kein unnötiges Blockieren des Watcher-Threads.
**Kein** Verhaltensunterschied: Während des Holds wurde ein gelesenes Beispiel immer
verworfen (`acceptRead=0`), Koordinaten bleiben durch Hold/Grace erhalten, der
Schreibzeit-Zähler (`g_lastFileWriteQuad`) wird weiter geführt. Einzige sichtbare Änderung
(nur mit `debug=1`): die Zeile `Pos.txt skipped (HTTP hold …)` hat kein `seq=` mehr, weil
die Datei nicht gelesen wird.

**Test:** Cross-Compile; **manuell:** Mit HTTP-Mod laufen lassen — `POS: method none -> http`,
keine Wechsel zu `file`.

**Lerneffekt:** Wenn ein Kommentar etwas behauptet, was der Code nicht tut, ist einer von
beiden falsch — prüfen, welcher.

---

## 16. Mod-Passwort blieb nach dem Ablauf im Speicher

**Was:** `mod_password_get` (`mod_password.c`) löscht den Wert, wenn eine Lesung ihn als
abgelaufen (älter als 10 s) erkennt.

**Vorher:** Nach Ablauf der 10 s wurde der Wert nur nicht mehr herausgegeben, blieb aber im
Speicher bis zum nächsten Trennen der TS-Verbindung.

**Warum besser:** Ein Geheimnis soll nicht länger im Speicher liegen als nötig. Eine
Uhr, die *hinter* dem Zeitstempel steht (kommt nur in Tests vor), zählt nicht als
„abgelaufen“ — der Wert bleibt dann erhalten.

**Test:** `mod_password_test` [3]: nach dem Ablauf weder mit späterer noch mit früherer Uhr
auslesbar („gelöscht, nicht nur versteckt“), neu speichern geht, zu kleiner Zielpuffer
löscht ebenfalls.

**Lerneffekt:** „Nicht mehr ausliefern“ ist nicht „gelöscht“. Geheimnisse aktiv überschreiben.

---

## 17. Ohne Pos.txt-Pfad flackerten HTTP-Positionen (beim Fixen gefunden)

**Was:** `pos_watcher_thread` in `src/core/mod_file/pos_file.c`.

**Vorher:** War `EnablePosFile=true` (Standard), aber kein Saved-Pfad gefunden, setzte der
Watcher die Koordinaten bei **jedem** 30-ms-Tick auf „ungültig“ — auch während der Mod per
HTTP gerade Positionen lieferte. Die HTTP-Injektion setzte sie wieder auf „gültig“: der
Zustand flackerte. Im reinen HTTP-Modus (`EnablePosFile=false`) gab es das nicht, dort hält
eine aktive HTTP-Haltezeit (2 s) bzw. die Kulanzzeit die Koordinaten.

**Jetzt:** Ohne bekannten Pfad verhält sich der Tick genau wie der reine HTTP-Modus. Nur wenn
weder eine HTTP-Haltezeit noch die Kulanzzeit aktiv ist, werden die Koordinaten ungültig
(einmal, mit Log-Zeile und einem Update auf der Kante gültig → ungültig).

```text
Tick: Pos.txt an?  ── nein ───────────────┐
        │ ja                               ▼
   Pfad bekannt? ── nein ──►  HTTP-only-Regel: Haltezeit/Kulanz → behalten, sonst ungültig
        │ ja
   Pos.txt lesen (wie bisher)
```

**Test:** Cross-Compile ohne Warnungen; Laufzeit nur im Spiel prüfbar: Mod über HTTP,
kein Saved-Pfad gefunden → im Debug-Log bleibt `valid=1`, kein Wechsel Hub ↔ Ingame.

**Lerneffekt:** Zwei Quellen für denselben Zustand brauchen **eine** Regel, wer gewinnt —
sonst schreiben sie sich gegenseitig um.

## Gesamt-Test und Risiken

- Cross-Compile (MinGW, `-Wall -Wextra`): `pos_http_server.c`, `config.c`, `pos_file.c`,
  `path_detect.c`, `hub_parser.c`, `mod_password.c` — ohne Warnungen.
- Host-Tests (gcc): `hub_parser_test`, `mod_password_test`, `http_request_test`,
  `pos_parse_test`, `config_text_test` (zusätzlich mit AddressSanitizer/UBSan).
- **Nicht** auf dem Linux-Host testbar (Win32): Sockets/Deadline, atomisches Speichern,
  Codepage-Umwandlung, Registry — siehe „Manuell“ in den Abschnitten.
- Risiko Punkt 1: Würde CEE einen *fremden* `Origin` senden, bekäme der Mod 403. Das steht
  dann sofort mit Header und Wert im `plugin.log`; die Regel ist an einer Stelle
  (`http_origin_is_own`) anpassbar.
- Die Datei `plugin.cfg` wird für Nicht-ASCII-Pfade unter Umständen als UTF-8 geschrieben.
  Der Einstellungsdialog las `plugin.cfg` für die Pfad-Anzeige noch selbst im ANSI-Modus;
  er liest jetzt die geladene Konfiguration (`config_copy`, Doku 087), damit solche Pfade
  auch dort richtig angezeigt werden.
