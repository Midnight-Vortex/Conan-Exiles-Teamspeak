# Pos-HTTP — Vertrag für den Workshop-Mod (CEE Blueprint)

**Status:** eingefroren (Änderung 085 härtet nur die Plugin-Seite, der Vertrag bleibt gleich) · **Plugin-Seite:** `src/core/http/pos_http_server.c` (+ reine Hilfsfunktionen in `http_request.h`)  
**Bezug:** CEE *Modding HTTP Requests — Blueprint Reference* (Inflexion / Funcom)

Dieses Dokument ist die **einzige** Spezifikation, die der Workshop-Mod und das
TeamSpeak-Plugin gemeinsam nutzen. Felder und URL hier ändern = Breaking Change.

---

## 1. Genehmigte Origin (CEE Pflicht)

Der Mod muss die Origin **exakt** freigeben / freigeben lassen:

```text
http://127.0.0.1:52734
```

Hinweise aus der CEE-Doku:

- Origin-Match ist **streng** (Port, `http` vs `https`, Host).
- Abweichungen → `NotApprovedByPlayer`.
- Keine Credentials in der URL (`http://user:pass@…` → `BlockedByPolicy`).
- URL max. 8000 Zeichen, druckbares ASCII, kein Whitespace.

---

## 2. Endpunkte

| Methode | URL | Zweck |
|---|---|---|
| `GET` | `http://127.0.0.1:52734/health` | Plugin erreichbar? |
| `POST` | `http://127.0.0.1:52734/v1/position` | **Positionsdaten senden (Vertrags-Endpoint)** |

Alias (nicht für neue Mods empfohlen): `POST …/position`.

---

## 3. Eingefrorenes Body-Format (JSON)

**Einziger offizieller Body für den Workshop-Mod.** UTF-8, ein JSON-Objekt.

### Pflichtfelder

| Feld | Typ | Einheit | Bedeutung |
|---|---|---|---|
| `x` | number | **Zentimeter** | Welt-X |
| `y` | number | **Zentimeter** | Welt-Y |
| `z` | number | **Zentimeter** | Welt-Z |
| `yaw` | number | Grad | Blickrichtung horizontal |

### Optionale Felder

| Feld | Typ | Einheit | Default | Bedeutung |
|---|---|---|---|---|
| `seq` | number (int) | — | `0` | Monoton steigende Sequenz (hilft Dedup) |
| `yawY` | number | Grad | `0` | Blickrichtung vertikal / Pitch |

### Beispiel (StringContent im Blueprint)

```json
{"seq":42,"x":12345.0,"y":67890.0,"z":200.0,"yaw":45.0,"yawY":0.0}
```

### Zusatzfeld `TSPW=` – Ingame-Channel-Passwort (077)

Die Teamspeak-Mod (Zeilenformat `SEQ=… X=… … OCC=…`) hängt **als letztes Feld**
` TSPW=<Passwort>` an, wenn der Conan-Server ein Passwort kennt (`dc tsvm <Passwort>`).
Alles hinter `TSPW=` gehört zum Passwort (Leerzeichen erlaubt). Das Plugin schneidet das
Feld ab und löscht es aus dem Puffer, **bevor** der Body geparst oder geloggt wird. Es
hält das Passwort nur im Speicher und gilt nur, solange die Mod sendet (10 s).
`TSPW=` ohne Wert = „Server hat kein Passwort“. In `Pos.txt` steht es nie.

### Ablehnung (HTTP 400)

Das Plugin lehnt u. a. ab:

- fehlende Pflichtfelder
- nicht-finite Werte (`x`, `y`, `z`, `yaw`, `yawY` — alle fünf, seit 085)
- unsinnig große Werte: Koordinaten oder Winkel mit Betrag über `1e8` (seit 085;
  ein endlicher Wert wie `1e300` würde sonst beim Umrechnen auf `float` zu „unendlich“)
- „Dummy“-Position nahe `(0,0,0)` cm (wie bei `Pos.txt`)
- ungültiges oder unvollständiges HTTP (seit 085): ungültiger `Content-Length`,
  Body größer als der Empfangspuffer (4 KB), Body kommt nicht vollständig innerhalb
  der Zeit an. Ein **halber** Body wird nie ausgewertet (sonst wäre z. B. ein halbes
  Passwort gespeichert).

Antwortkörper bei Fehler:

```json
{"ok":false,"error":"invalid position"}
```

(bei kaputtem HTTP stattdessen `{"ok":false,"error":"invalid request"}`)

### Ablehnung (HTTP 403) — Schutz vor Browser-Seiten (085)

Der Server hört nur auf `127.0.0.1`, hat aber keine Anmeldung. Ein **Browser auf
demselben PC** erreicht diese Adresse auch: jede Webseite konnte per
`fetch("http://127.0.0.1:52734/v1/position", {method:"POST", mode:"no-cors", body:"…"})`
eine Position einschleusen oder das Ingame-Passwort überschreiben (ein
`text/plain`-POST braucht keine „Preflight“-Rückfrage). Mit „DNS-Rebinding“ kann eine
Seite sogar die Antworten lesen. Darum prüft das Plugin bei **jedem** Endpunkt
(`/health`, `/v1/position`, `/position`, unbekannte Pfade) die Kopfzeilen — und
**nur** im Header-Block, nie im Body:

| Kopfzeile | Ergebnis |
|---|---|
| `Origin:` fehlt | erlaubt |
| `Origin: http://127.0.0.1:52734` oder `http://localhost:52734` (Schema/Host egal ob groß/klein, Port exakt) | erlaubt |
| jeder andere `Origin:`-Wert, auch `null` | **403** |
| `Sec-Fetch-Site:` fehlt, `none` oder `same-origin` | erlaubt |
| `Sec-Fetch-Site: cross-site` oder `same-site` | **403** |
| `Host:` fehlt | erlaubt |
| `Host:` = `127.0.0.1`, `localhost` oder `[::1]` (Port optional, beliebig) | erlaubt |
| jeder andere `Host:` | **403** |

Antwort: `{"ok":false,"error":"forbidden"}`. Jede 403 steht (gedrosselt, max. 1 Zeile
pro Sekunde mit „+N suppressed“) im `plugin.log`, mit Header-Name und Wert:

```text
HTTP: 403 rejected header Origin="https://evil.example"
```

**Warum der Mod (CEE „Start Http Request“) weiter funktioniert:**

- CEE ist **kein Browser**: es sendet keine `Sec-Fetch-*`-Kopfzeilen.
- `Host` setzt CEE selbst aus der URL (`http://127.0.0.1:52734/…` → `Host: 127.0.0.1:52734`).
  Das ist in der Liste der reservierten Header (Abschnitt 4) — der Mod setzt `Host` nicht selbst.
- Sendet CEE überhaupt einen `Origin:`, dann seine freigegebene Origin
  `http://127.0.0.1:52734` (Abschnitt 1) — genau die wird akzeptiert. (Die CEE-Doku sagt
  nicht, ob CEE `Origin:` sendet; deshalb ist beides abgedeckt.)
- Eine Webseite kann diese eigene Origin nie haben: das Plugin liefert keine Webseiten
  aus, und Browser erlauben einer Seite nicht, `Origin` zu fälschen.
- Mod-Autoren: **keine** eigenen `Origin`-/`Sec-Fetch-*`-Header setzen.

**Prüfen im Spiel:** TeamSpeak + Spiel starten, einen Charakter laden. Erwartet:
im `plugin.log` erscheint `POS: method none -> http` (Positionen kommen an), mit
`debug=1` zusätzlich `HTTP: POST … status=200`, und es erscheint **keine** Zeile
`HTTP: 403 rejected header …`. Steht dort doch eine 403-Zeile, nennt sie sofort den
Header und seinen Wert (dann ist CEE blockiert, nicht der Mod-Code).
Manuell: `http://127.0.0.1:52734/health` in die Adresszeile des Browsers tippen
funktioniert weiterhin (`Sec-Fetch-Site: none`).

Bei Erfolg:

```json
{"ok":true}
```

---

## 4. Blueprint-Aufruf (CEE `Start Http Request`)

| Pin | Wert |
|---|---|
| **URL** | `http://127.0.0.1:52734/v1/position` |
| **Verb** | `POST` |
| **StringContent** | JSON wie oben (UTF-8) |
| **Headers** | empfohlen: `Content-Type` → `application/json` |

### Header-Regeln (aus CEE-PDF)

**Nicht setzen** (reserviert / vom Spiel gesetzt — sonst `BlockedByPolicy`):

`Host`, `Content-Length`, `Transfer-Encoding`, `TE`, `Trailer`, `Expect`,
`Connection`, `Proxy-Connection`, `Keep-Alive`, `Upgrade`,
`Proxy-Authorization`, `User-Agent`, `X-Request-Id`

Außerdem **nicht setzen:** `Origin` und `Sec-Fetch-*` (das Plugin lässt nur die eigene
Origin zu, siehe „Ablehnung (HTTP 403)“ oben).

Der Server liest `Content-Length` nur aus dem Header-Block (Groß-/Kleinschreibung egal,
nur Ziffern, Wert muss in den 4-KB-Puffer passen) und wartet insgesamt höchstens
**500 ms** nach dem Verbindungsaufbau auf Header **und** Body (jedes einzelne `recv` max.
100 ms). Ein normaler Mod-POST über Loopback ist in wenigen Millisekunden komplett.

Wenn du **keinen** `Content-Type` setzt, ergänzt CEE automatisch  
`text/plain; charset=utf-8`. Das Plugin akzeptiert den JSON-Body trotzdem
(Parser schaut nur auf den Body, nicht auf den Header). Für Klarheit trotzdem
`application/json` setzen.

### Erfolg vs. Fehler (CEE)

- **OnSuccess** = irgendeine HTTP-Antwort (auch 400/404/500). Immer  
  `IsOK` bzw. `GetResponseCode` prüfen (`200`–`206` = OK).
- **OnFailure** = Request ging nicht raus / Verbindung tot / abgebrochen  
  (`ConnectionError`, `Cancelled`, …).

Redirects (3xx) werden **nicht** automatisch gefolgt — unser Server sendet keine.

---

## 5. Health-Check (optional)

```text
GET http://127.0.0.1:52734/health
```

Beispielantwort:

```json
{"ok":true,"service":"conan_exiles_ts","url":"http://127.0.0.1:52734"}
```

Im Blueprint: `IsOK` und/oder `GetContentAsString` auswerten.

---

## 6. Was absichtlich *nicht* Vertragsformat ist

| Format | Status |
|---|---|
| Pos.txt-Zeile `SEQ=… X=…` | Legacy-Kompatibilität im Plugin — **nicht** für neue Blueprint-Mods |
| HTTPS / Port 443 | nicht unterstützt |
| Auth-Token / API-Key | nicht vorhanden (nur localhost; Schutz vor Browser-Seiten über die Header-Prüfung, siehe „Ablehnung (HTTP 403)“) |
| Extra-Felder (`steamId`, …) | noch nicht definiert; unbekannte JSON-Felder werden ignoriert |

---

## 7. Einheiten-Hinweis

Intern und auf dem Wire: **Zentimeter**, wie bisher bei `Pos.txt`.  
Das Plugin rechnet für Proximity intern `/ 100` → Meter. Der Mod soll **nicht**
Meter senden.

---

## 8. Kurz-Checkliste für den Mod-Autor

1. Origin freigeben: `http://127.0.0.1:52734`
2. TeamSpeak + Plugin laufen (sonst `ConnectionError`)
3. `POST` + JSON mit `x,y,z,yaw` (cm / Grad)
4. Optional `Content-Type: application/json`
5. Keine reservierten Header setzen, auch kein `Origin` / `Sec-Fetch-*`
6. In `OnSuccess`: `IsOK` prüfen (`403` = Header-Prüfung des Plugins hat angeschlagen, Details im `plugin.log`)

### HTTP-only-Test (Pos.txt abschalten)

Für reine HTTP-Tests ohne Pos.txt-Fallback in `Documents\Conan Exiles TeamSpeak plugin\plugin.cfg`:

```ini
EnablePosFile=false
```

TS-Client danach neu starten. Der Pos-Watcher liest dann keine `Pos.txt` mehr; Koordinaten kommen nur noch über `POST /v1/position` (siehe Änderung 044).

## 9. Rohdaten im Plugin-Log

Bei hoher POST-Rate (~30 ms) antwortet der Server **zuerst** (HTTP 200/400), **danach** wird geloggt (Änderung 047). Erfolgreiche Proben und die **Rate-Zeile bei fail=0** sind nur `log_debug` (Änderung 051). `plugin.log` wird bei **250 MB** (Debug aus) bzw. **1 GB** (Debug an) geleert (Änderung 052).

| Stufe | Wann | Sichtbar |
|---|---|---|
| **Einzel-Sample (Erfolg)** | jeder erfolgreicher Inject (HTTP 200) | nur mit `debug=1` (`log_debug`) |
| **Rate-Zusammenfassung** | ~1× pro Sekunde bei aktivem POST | `fail=0`: nur `debug=1`; `fail>0`: immer (`log_write`) |
| **Fehler** | jeder 400 (Parse oder Reject) | immer (`log_write`, nach Send) |

### Erfolg — nur mit debug=1 (eine Zeile pro POST)

```text
HTTP: POST seq=42 pos=X=12345.000000 Y=67890.000000 Z=200.000000 YAW=45.000000 YAWY=0.000000 dt=30ms status=200
```

Koordinaten intern als `double`; Log zeigt `%.6f` mit `X=`/`Y=`/`Z=`/`YAW=`/`YAWY=` (Änderung 041).

### Rate-Zeile (~1×/s)

Mit `debug=1` und mindestens einem erfolgreichen Inject im Fenster (`fail=0`):

```text
HTTP: rate n=33/s dt=min/avg/max=28/30/35ms ok=33 fail=0 last_seq=42 last=X=12345.000000 Y=67890.000000 Z=200.000000
```

Nur Fehler im Fenster (`ok=0`):

```text
HTTP: rate n=5/s dt=min/avg/max=28/30/35ms ok=0 fail=5
```

- `n` — POST-Anzahl in diesem 1-Sekunden-Fenster  
- `dt` — Abstand zwischen zwei POSTs (Minimum / Durchschnitt / Maximum in ms)  
- `ok` / `fail` — wie viele Inject-Erfolge vs. Fehler (400)  
- `last_seq` / `last=X/Y/Z` — **letzter** erfolgreicher POST **in diesem Fenster**; fehlt wenn `ok=0`
- Ohne Debug und `fail=0` erscheint **keine** Rate-Zeile (Änderung 051)

### Fehler — sichtbar, aber gedrosselt (nach HTTP-Antwort, seit 085)

Alle Fehlerzeilen (Parse/Reject-400, 403, 404, Verbindungsabbrüche) erscheinen **höchstens
1× pro Sekunde je Art**; was dazwischen ausfiel, steht als Zähler an der nächsten Zeile
(`… (+32 suppressed)`). Vorher schrieb ein Mod im Hauptmenü (Position `0,0,0`, ~33×/s)
33 Zeilen pro Sekunde mit bis zu 420 Byte Rohdaten ins Log.

**Body nicht parsebar** (fehlende Pflichtfelder, kein JSON/Pos-Zeile):

```text
HTTP: POST status=400 reason=parse cl=12 raw="..."
```

**Body parsebar, aber Plugin lehnt ab** (z. B. Koordinaten nahe `(0,0,0)` oder Pos-Watcher nicht aktiv):

```text
HTTP: POST status=400 reason=reject seq=1 pos=X=0.000000 Y=0.000000 Z=0.000000 cl=58 raw="..."
```

Kaputtes HTTP (ungültiger `Content-Length`, zu großer oder unvollständiger Body) loggt
**ohne** Rohdaten (der Body kann noch das Passwort `TSPW=` enthalten):

```text
HTTP: POST status=400 reason=content-length cl=0
HTTP: POST status=400 reason=body-too-large cl=5000
HTTP: POST status=400 reason=body-incomplete cl=120
```

Abgebrochene Verbindungen: `HTTP: drop reason=recv|incomplete|deadline` (`deadline` = die
500 ms nach dem Verbindungsaufbau sind um).

GET `/health` und unbekannte Pfade (404) loggen weiterhin eine `HTTP: IN … raw=` Zeile
(jetzt ebenfalls gedrosselt). Erfolgreiche POSTs brauchen **`debug=1`** für Einzelzeilen (seit Änderung 047; vorher 042: immer sichtbar).

## 10. Positionsquelle (HTTP vs Pos.txt)

Der Pos-Watcher schreibt bei jedem **Wechsel** immer (`log_write`, Änderung 050):

```text
POS: method none -> http
POS: method http -> file
POS: method file -> none
```

`http` = letzter Inject über `POST /v1/position` (Hold ~2 s, Datei wird nicht überschrieben).  
`file` = Pos.txt-Notfall, nur wenn HTTP ~2 s nichts liefert.  
`none` = Koordinaten ungültig (stale / Grace abgelaufen).

Nach einer Störung: der nächste gültige POST setzt wieder `file -> http` (Inject
hat Vorrang). Stirbt `accept()`, bindet der Listener neu statt dauerhaft auf
Pos.txt zu bleiben (Änderung 056).

Mit **`debug=1`** zusätzlich ~1×/s:

```text
DBG POS: method=http valid=1 holdLeft=1800ms posFile=1 fileAge=19999ms seq=42
```

und wenn eine frische Pos.txt während des HTTP-Holds liegt:

```text
DBG POS: Pos.txt skipped (HTTP hold left=1800ms age=200ms)
```

Seit 085 wird die Datei während des Holds wirklich **nicht mehr geöffnet/gelesen**
(nur ihr Zeitstempel wird verfolgt) — deshalb steht dort kein `seq=` mehr.

### Listener-Wiederbelebung (085)

Der Listener-Thread besitzt den Listen-Socket und bindet bei Bedarf neu:

```text
Start: bind fehlgeschlagen?  → Thread startet trotzdem und versucht es weiter
accept()-Fehler, der nur EINE Verbindung betrifft
   (WSAECONNRESET / ECONNABORTED / EINTR / EWOULDBLOCK) → einfach weiter, Socket bleibt
anderer accept()-Fehler → Socket schließen, Wartezeit, neu binden
Wartezeit (Backoff): 250 → 500 → 1000 → 2000 → 4000 → 5000 ms (Maximum),
   nach der nächsten erfolgreich bedienten Verbindung wieder von vorn
```

Alle Wartezeiten sind Wartezeiten auf das Stop-Event: `pos_http_server_stop()` bleibt
sofort. Dauerhafte Bind-Fehler werden nur alle 30 s geloggt.

Gebunden wird zuerst **ohne** `SO_REUSEADDR` (unter Windows würde sonst ein *anderes*
Programm denselben Port mitbenutzen dürfen). Nur wenn der Port als „belegt“ gemeldet wird
(z. B. alte Plugin-Instanz nach einem Neustart), wird mit `SO_REUSEADDR` erneut
versucht und `HTTP: WARNING port 52734 was in use - bound with SO_REUSEADDR` geloggt.
