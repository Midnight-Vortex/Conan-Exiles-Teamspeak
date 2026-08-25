# Pos-HTTP — Vertrag für den Workshop-Mod (CEE Blueprint)

**Status:** eingefroren · **Plugin-Seite:** `src/core/http/pos_http_server.c`  
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

### Ablehnung (HTTP 400)

Das Plugin lehnt u. a. ab:

- fehlende Pflichtfelder
- nicht-finite Werte
- „Dummy“-Position nahe `(0,0,0)` cm (wie bei `Pos.txt`)

Antwortkörper bei Fehler:

```json
{"ok":false,"error":"invalid position"}
```

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
| Auth-Token / API-Key | nicht vorhanden (nur localhost) |
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
5. Keine reservierten Header setzen
6. In `OnSuccess`: `IsOK` prüfen

### HTTP-only-Test (Pos.txt abschalten)

Für reine HTTP-Tests ohne Pos.txt-Fallback in `Documents\Conan Exiles TeamSpeak plugin\plugin.cfg`:

```ini
EnablePosFile=false
```

TS-Client danach neu starten. Der Pos-Watcher liest dann keine `Pos.txt` mehr; Koordinaten kommen nur noch über `POST /v1/position` (siehe Änderung 044).

## 9. Rohdaten im Plugin-Log

Bei hoher POST-Rate (~30 ms) schreibt das Plugin **eine Zeile pro erfolgreichem POST** plus **eine Rate-Zusammenfassung pro Sekunde** — kein zweites raw-IN bei HTTP 200 (siehe Änderung 042).

| Stufe | Wann | Sichtbar |
|---|---|---|
| **Einzel-Sample (Erfolg)** | jeder erfolgreicher Inject (HTTP 200) | immer (`log_write`) |
| **Rate-Zusammenfassung** | ~1× pro Sekunde bei aktivem POST | immer (`log_write`) |
| **Fehler** | jeder 400 (Parse oder Reject) | immer (`log_write`) |

### Erfolg — immer sichtbar (eine Zeile pro POST)

```text
HTTP: POST seq=42 pos=X=12345.000000 Y=67890.000000 Z=200.000000 YAW=45.000000 YAWY=0.000000 dt=30ms status=200
```

Koordinaten intern als `double`; Log zeigt `%.6f` mit `X=`/`Y=`/`Z=`/`YAW=`/`YAWY=` (Änderung 041).

### Rate-Zeile (immer, ~1×/s)

```text
HTTP: rate n=33/s dt=min/avg/max=28/30/35ms ok=33 fail=0
```

- `n` — POST-Anzahl in diesem 1-Sekunden-Fenster  
- `dt` — Abstand zwischen zwei POSTs (Minimum / Durchschnitt / Maximum in ms)  
- `ok` / `fail` — wie viele Inject-Erfolge vs. Fehler (400)

### Fehler — immer sichtbar

**Body nicht parsebar** (fehlende Pflichtfelder, kein JSON/Pos-Zeile):

```text
HTTP: POST status=400 reason=parse cl=12 raw="..."
```

**Body parsebar, aber Plugin lehnt ab** (z. B. Koordinaten nahe `(0,0,0)` oder Pos-Watcher nicht aktiv):

```text
HTTP: POST status=400 reason=reject seq=1 pos=X=0.000000 Y=0.000000 Z=0.000000 cl=58 raw="..."
```

GET `/health` und unbekannte Pfade (404) loggen weiterhin optional eine `HTTP: IN … raw=` Zeile (selten). Erfolgreiche POSTs brauchen **kein** `debug=1` mehr (seit Änderung 042).
