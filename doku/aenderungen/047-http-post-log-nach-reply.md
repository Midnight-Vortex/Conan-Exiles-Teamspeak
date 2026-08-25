# 047 — HTTP-POST: Antwort vor Log (Latenz-Fix)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/http/pos_http_server.c` | POST-Erfolg (200): `http_send()` zuerst, danach `log_debug` (nicht mehr `log_write`); POST-Fehler (400): `http_send()` zuerst, danach `log_write`; Rate-Zeile (~1 Hz) um `last_seq` + `last=X/Y/Z` ergänzt |
| `doku/module/pos-http.md` | Abschnitt 9: Logging-Stufen angepasst (200 = debug, Rate mit Last-Sample) |
| `doku/aenderungen/047-http-post-log-nach-reply.md` | Dieser Eintrag |

**Betroffen:** POST-Zweig in `http_handle_client`, Funktion `http_post_rate_tick`, Datei-Kopf-Kommentar (Thread/Logging).

---

## 2. Wie war es vorher (V7 / Änderung 042)?

Ab Änderung 042 schrieb jeder **erfolgreiche** POST (~30/s) eine `log_write`-Zeile **bevor** `http_send()` die HTTP-Antwort schickte.

`log_write()` öffnet, schreibt und schließt `plugin.log` unter Lock — pro Zeile echtes Datei-I/O. Bei ~30 POST/s blockierte das den HTTP-Listener-Thread ~30× pro Sekunde **vor** der Antwort. Der CEE-Mod wartete auf die Reply; der nächste POST konnte erst starten, wenn `handle` zurück war — sichtbar als Latenz/Stau.

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Vorher (042) | Jetzt (047) |
|---|---|
| ~30×/s `log_write` vor jeder Antwort | Antwort sofort; Erfolg nur `log_debug` (optional mit `debug=1`) |
| CEE wartet auf Log-I/O + HTTP | CEE bekommt JSON-Reply ohne Log-Lock davor |
| Rate-Zeile ohne Positions-Sample | Rate-Zeile zeigt **letztes ok-Sample** im 1-s-Fenster |
| 400-Fehler: Log vor Send | 400: Send zuerst, dann Fehler-Log (Mod bekommt schneller 400) |

Operatoren sehen weiterhin **~1 Zeile/s** mit Zähler, Timing und (bei Erfolgen) der letzten Position — ohne 30 Log-Zeilen/s im Normalbetrieb.

---

## 4. Wie funktioniert es jetzt?

```
Mod POST /v1/position
        │
        ▼
http_parse_body() ──► pos_inject_sample()
        │
        ▼
injected?
   ja │                    nein (400)
      ▼                         ▼
http_send 200            http_send 400
      │                         │
      ▼                         ▼
http_post_rate_tick()     ← Statistik; ~1/s log_write Rate-Zeile
      │                         │
      ▼                         ▼
log_debug (seq/pos/dt)   log_write (parse/reject + raw)
```

Alles auf dem **HTTP-Listener-Thread** (accept → handle → close). Keine TS-API. Logging nur über Log-Lock.

**Erfolg (nur mit `debug=1` in `plugin.cfg`):**

```text
HTTP: POST seq=42 pos=X=12345.000000 Y=67890.000000 Z=200.000000 YAW=45.000000 YAWY=0.000000 dt=30ms status=200
```

**Rate-Zeile (immer, ~1×/s) — mit letztem ok-Sample im Fenster:**

```text
HTTP: rate n=33/s dt=min/avg/max=28/30/35ms ok=33 fail=0 last_seq=42 last=X=12345.000000 Y=67890.000000 Z=200.000000
```

Wenn im 1-Sekunden-Fenster **kein** Inject gelang (`ok=0`), entfallen `last_seq` und `last=…` — nur die Basis-Rate-Zeile.

`last_*` = **letzter erfolgreicher POST in genau diesem Fenster** (nicht aus vorherigen Sekunden übernommen).

**Fehler (immer sichtbar, nach Send):** unverändert `reason=parse` / `reason=reject` mit raw-Body.

---

## 5. Wie wurde es getestet?

- **Manuell (nach Build):** TeamSpeak neu starten; Mod POST ~30 ms an `/v1/position`.
  - Ohne `debug=1`: **keine** `HTTP: POST … status=200`-Zeilen; weiterhin ~1×/s `HTTP: rate …` mit `last_seq`/`last=X/Y/Z`.
  - Mit `debug=1`: jede Erfolgszeile als `log_debug`, **nach** dem Mod-Empfang der 200-Antwort spürbar schneller als vor 047.
  - Ungültiger Body → 400 + `reason=parse`; Dummy-Position → `reason=reject`.
  - Proximity/Inject-Verhalten unverändert (Statuscodes 200/400).
- **Nicht gebaut** in diesem Arbeitspaket.

---

## 6. Lerneffekt

1. **Hot path zuerst antworten:** Bei hoher Request-Rate gehört synchrones Datei-Logging **hinter** die Netzwerk-Antwort — sonst wartet der Client auf I/O, das er gar nicht braucht.
2. **`log_write` vs `log_debug`:** Volle Sichtbarkeit jeder Probe kostet ~30 Datei-Opens/s; Aggregat (1/s) + optionales Debug ist der gängige Kompromiss.
3. **Last-Sample in der Rate-Zeile:** Ein Feld `last_seq` + Koordinaten ersetzt 30 Einzelzeilen für Operatoren, die nur „lebt es?“ prüfen wollen.
