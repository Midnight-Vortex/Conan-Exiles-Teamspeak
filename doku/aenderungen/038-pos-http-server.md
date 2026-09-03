# 038 — HTTP-Positionsbrücke (pos_http_server)

## 1. Was wurde geändert?

### Neue Dateien
| Datei | Inhalt |
|---|---|
| `src/core/http/pos_http_server.h` | Öffentliche API: `pos_http_server_start/stop`, Port-Konstante, URL-Konstante |
| `src/core/http/pos_http_server.c` | Vollständiger HTTP-Server: Winsock-Listener, Request-Parser, Route-Handler |

### Geänderte Dateien
| Datei | Änderung |
|---|---|
| `src/core/mod_file/pos_file.h` | +`pos_inject_sample()`, +`pos_inject_set_notify_callback()` |
| `src/core/mod_file/pos_file.c` | Inject-Implementierung + HTTP-Hold-Fenster im Watcher |
| `src/ts/entry/ts3_entry.c` | Include, HTTP-Callback `ts3_on_http_position_update`, Lifecycle-Verdrahtung |
| `project/Conan-Exiles-TeamSpeak.vcxproj` | ClCompile/ClInclude für neue Dateien, `ws2_32.lib` in beiden Configs |
| `build/build_mingw.sh` | Neue Quelldatei, `-lws2_32` |

---

## 2. Wie war es vorher (V7/V8 vor dieser Änderung)?

Das Plugin konnte Positionsdaten ausschließlich aus `Pos.txt` lesen — einer Datei,
die der **Conan-Exiles-Mod** im Spielverzeichnis schreibt. Der Polling-Thread
(`pos_watcher`) liest diese Datei alle 100 ms.

**Problem:** Ein Workshop-Mod (Blueprint-Mod) kann **keine Dateien schreiben** —
er läuft im Sandbox-Kontext von Unreal Engine und hat keinen direkten Dateizugriff.
Damit war eine Blueprint-Mod-Integration bisher unmöglich.

---

## 3. Warum ist die neue Lösung besser/stabiler?

### Zweiter Kanal: HTTP auf localhost
Der HTTP-Server läuft auf `http://127.0.0.1:52734/` — eine **feste URL**, die in den
Workshop-Mod eingebaut werden kann. Der Mod sendet seine Positionsdaten per
HTTP-POST; das Plugin empfängt sie auf dem Listener-Thread.

### Dual-Source-Design: HTTP hat Vorrang
Sobald eine gültige HTTP-Position eintrifft, wird ein **Hold-Fenster** von 2 s gesetzt
(`g_httpHoldUntilMs`). Solange das Fenster aktiv ist:
- überschreibt der `pos_watcher` die Koordinaten **nicht** (kein Pos.txt-Accept)
- werden die Koordinaten **nicht** invalidiert (auch wenn Pos.txt fehlt)

Das verhindert, dass ein gleichzeitig laufender Datei-Watcher die HTTP-Daten
überschreibt.

### Thread-Safety: alles getrennt
| Thread | Darf was |
|---|---|
| TS-Callback-Thread | `start/stop` aufrufen, Lifecycle |
| Watcher-Thread | Pos.txt lesen, `g_httpHoldUntilMs` lesen |
| HTTP-Listener-Thread | `pos_inject_sample()` aufrufen, **nie** TS-API |
| PCM-Audio-Thread | Lock-freie Snapshots (unverändert) |

`pos_inject_sample` ruft `g_injectNotifyCallback` auf. Diese ist
`ts3_on_http_position_update` — identisch mit `ts3_on_local_position_update`
**aber ohne** `plugin_ui_on_position_tick()`, da UI-Overlay-Zugriff vom HTTP-Thread
nicht erlaubt ist.

> **Hinweis (053):** Das Auslassen des Position-Ticks war ein Bug — die HUD-Zone
> blieb auf „Ausserhalb“, solange HTTP die Positionsquelle war. Ab **8.0.6** rufen
> beide Callbacks dieselbe gemeinsame Funktion inkl. Tick auf; Overlay-Refresh
> läuft per `PostMessage`, kein GDI vom HTTP-Thread.

---

## 4. Wie funktioniert es jetzt?

### Datenfluss

```
Workshop-Mod (UE Blueprint)
    │  POST http://127.0.0.1:52734/v1/position
    │  Body: {"seq":1,"x":12345.0,"y":67890.0,"z":200.0,"yaw":45.0}
    ▼
HTTP-Listener-Thread (pos_http_server.c)
    │  recv → parse → pos_inject_sample()
    ▼
pos_file.c :: pos_inject_sample()
    │  EnterCriticalSection(g_posLock)
    │  g_currentSample = sample
    │  LeaveCriticalSection
    │  g_coordinatesValid = 1
    │  g_httpHoldUntilMs = now + 2000 ms
    │  g_injectNotifyCallback()
    ▼
ts3_entry.c :: ts3_on_http_position_update()
    │  chan_signal_position_update()       ← AtomicFlag setzen
    │  cepos_signal_send_pending()         ← AtomicFlag setzen
    │  ts3_ceping_signal_send_pending()    ← AtomicFlag setzen
    │  ts3_audio_on_local_position_update() ← Snapshot unter WriterLock
    ▼
CEDRAIN (nächster TS-Callback-Tick)
    → CEPOS-Paket an andere Spieler
    → 3D-Positions-Update
    → Audio-Recompute
```

### Routen

| Methode | Pfad | Antwort |
|---|---|---|
| GET | `/health` | `200 {"ok":true,"service":"conan_exiles_ts","url":"http://127.0.0.1:52734"}` |
| POST | `/v1/position` | `200 {"ok":true}` oder `400 {"ok":false,"error":"invalid position"}` |
| POST | `/position` | Alias für `/v1/position` |

### Body-Formate

**Vertragsformat für Workshop-/CEE-Blueprint-Mods (eingefroren):** siehe
`doku/module/pos-http.md`. Kurzfassung:

```json
{"seq":1,"x":12345.0,"y":67890.0,"z":200.0,"yaw":45.0,"yawY":2.0}
```

- Einheiten: `x/y/z` in **Zentimetern**, `yaw`/`yawY` in **Grad**
- CEE Origin exakt: `http://127.0.0.1:52734`
- Blueprint: `POST`, `StringContent` = JSON; `Content-Type: application/json` empfohlen
- Antwort auswerten mit CEE `IsOK` / `GetResponseCode` (auch 400 kommt in OnSuccess)

**Legacy (Pos.txt-Zeile)** wird vom Plugin noch akzeptiert, ist aber **kein**
Vertragsformat für neue Mods:

```
SEQ=1 X=12345.0 Y=67890.0 Z=200.0 YAW=45.0 YAWY=2.0
```
- Deutsches Dezimalkomma (`,` statt `.`) wird automatisch normalisiert

### Lifecycle im Plugin

```
ts3plugin_init():
  pos_watcher_start()                         ← Watcher läuft zuerst (Lock existiert)
  pos_inject_set_notify_callback(ts3_on_http_position_update)
  pos_http_server_start()                     ← HTTP danach

ts3plugin_shutdown():
  pos_http_server_stop()                      ← HTTP ZUERST stoppen
  pos_watcher_stop()                          ← dann Watcher (löscht den Lock)
```

Diese Reihenfolge ist zwingend: `pos_inject_sample` prüft `g_watcherRunning` und
benötigt `g_posLock` — beides wird in `pos_watcher_stop` freigegeben.

### Stop-Mechanismus

Der HTTP-Listener blockiert auf `accept()`. Beim Stop:
1. `SetEvent(g_stopEvent)` — Thread-Flag
2. `closesocket(g_listenSocket)` — entsperrt `accept()` sofort
3. `WaitForSingleObject(g_httpThread, 5000)` — wartet auf sauberen Exit

---

## 5. Wie wurde es getestet?

**Build-Test:**
```powershell
pwsh -NoProfile -File build\build_msvc.ps1 -SkipDeploy
# → Build OK: conan_exiles.dll, 7.02 MB, 0 Fehler, 0 Warnungen
```

**Rohdaten im Plugin-Log (Debugging Workshop-Mod):**
Jeder eingehende Request schreibt eine Zeile:
```text
HTTP: IN POST /v1/position status=200 cl=72 raw="{"seq":1,"x":12345.0,...}"
```
- `status` = Antwortcode (200/400/404)
- `cl` = Body-Länge
- `raw` = empfangener Body (einzeilig, ggf. gekürzt)
Pfad: übliches Plugin-Log unter dem TS3-Log-Ordner / konfigurierter Log-Pfad.

**Manueller Funktionstest (nach Deploy + TS-Neustart):**
```powershell
# Health-Check
Invoke-RestMethod -Uri "http://127.0.0.1:52734/health"
# Erwartetes Ergebnis: ok=true, service=conan_exiles_ts

# Position senden (JSON)
Invoke-RestMethod -Method POST -Uri "http://127.0.0.1:52734/v1/position" `
  -ContentType "application/json" `
  -Body '{"seq":1,"x":12345.0,"y":67890.0,"z":200.0,"yaw":45.0}'
# Erwartetes Ergebnis: ok=true
# Im TS-Log: "HTTP: listening on http://127.0.0.1:52734"
# Im TS-Plugin: Proximity-Audio aktiv (andere Spieler hören Entfernung)

# Fehlerfall (ungültige Koordinaten 0,0,0)
Invoke-RestMethod -Method POST -Uri "http://127.0.0.1:52734/v1/position" `
  -ContentType "application/json" `
  -Body '{"seq":2,"x":0,"y":0,"z":0,"yaw":0}'
# Erwartetes Ergebnis: 400 ok=false, error=invalid position

# Pos.txt-Format
Invoke-RestMethod -Method POST -Uri "http://127.0.0.1:52734/position" `
  -ContentType "text/plain" `
  -Body "SEQ=3 X=12345.0 Y=67890.0 Z=200.0 YAW=45.0"
# Erwartetes Ergebnis: ok=true
```

---

## 6. Lerneffekt

1. **Winsock2 vor windows.h:** `<winsock2.h>` muss immer VOR `<windows.h>` stehen —
   sonst gibt es Redefinitionsfehler. Deshalb wird Winsock nur in der `.c`-Datei
   includiert, nicht im Header.

2. **Statische Funktion vor Verwendung:** MSVC `/W4 /WX` behandelt eine implizite
   extern-Deklaration (fehlende Vorwärtsdeklaration) als Fehler. Auch statische
   C-Hilfsfunktionen müssen vor ihrem ersten Aufruf definiert oder deklariert sein.

3. **accept() entsperren:** Unter Windows wird ein blockierendes `accept()` sofort
   freigegeben, wenn das Socket per `closesocket()` geschlossen wird. Kein
   `select`-Timeout-Loop nötig — einfacher und zuverlässiger.
