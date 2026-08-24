# 040 — HTTP-Debug-Logging (POST /v1/position)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/http/pos_http_server.c` | POST-Handler: Parse und Inject getrennt; Rate-Zusammenfassung ~1/s; stufenweises Logging (log_write vs log_debug) |
| `doku/module/pos-http.md` | Abschnitt 9 aktualisiert (Rate-Zeile, DBG-Felder, 400-Gründe) |

**Funktionen betroffen:** `http_handle_client` (POST-Zweig), neu `http_format_raw`, `http_post_rate_tick`; `http_log_raw_in` nutzt jetzt `log_debug` bei Status 200.

---

## 2. Wie war es vorher (V7/V8)?

Jeder POST auf `/v1/position` schrieb **eine volle Zeile** mit `log_write`:

```text
HTTP: IN POST /v1/position status=200 cl=72 raw="{...}"
```

Bei ~30 ms Intervall (33 POST/s) entstanden **33 Log-Zeilen pro Sekunde** — das Log wurde unlesbar und I/O-lastig.

Zusätzlich war **Parse-Fehler** und **Inject-Ablehnung** (z. B. Koordinaten nahe 0,0,0 oder Pos-Watcher aus) in einem gemeinsamen HTTP-400 versteckt; man sah nur `status=400`, nicht *warum*.

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Problem vorher | Lösung jetzt |
|---|---|
| Log-Flut bei hoher POST-Rate | Erfolgreiche Samples nur `log_debug`; **eine** `log_write`-Rate-Zeile pro Sekunde |
| 400 ohne Unterscheidung | `reason=parse` (Body unlesbar) vs `reason=reject` (parsed, aber Inject abgelehnt) |
| Kein Timing-Überblick | Rate-Zeile mit min/avg/max Intervall (`dt`) und ok/fail-Zähler |

Alles bleibt auf dem **HTTP-Listener-Thread** — keine TS-API, keine extra Locks (sequentiell accept+handle).

---

## 4. Wie funktioniert es jetzt?

### Ablauf pro POST

```
Mod POST /v1/position
        │
        ▼
http_parse_body() ──► parsed?
        │                    │
        │ nein               │ ja
        ▼                    ▼
log_write 400          pos_inject_sample()
reason=parse                │
+ raw                       ▼
                       injected?
                    ja /        \ nein
                    ▼              ▼
              log_debug       log_write 400
              POST fields     reason=reject
              + log_debug     + seq/x/y/z + raw
              raw (optional)
                    │
                    ▼
              http_post_rate_tick()
              (~1/s log_write Rate-Zeile)
```

### Log-Zeilen (Beispiele)

**Rate (immer sichtbar, ~1×/s bei aktivem Mod):**

```text
HTTP: rate n=33/s dt=min/avg/max=28/30/35ms ok=33 fail=0
```

**Erfolg (nur mit `debug=1` in plugin.cfg):**

```text
DBG HTTP: POST seq=42 x=12345.0 y=67890.0 z=200.0 yaw=45.0 yawY=0.0 dt=30ms status=200
```

**Parse-Fehler (immer sichtbar):**

```text
HTTP: POST status=400 reason=parse cl=12 raw="not json"
```

**Inject-Ablehnung (immer sichtbar):**

```text
HTTP: POST status=400 reason=reject seq=1 x=0.0 y=0.0 z=0.0 cl=58 raw="{...}"
```

(`reject` = plausible Felder geparst, aber `pos_inject_sample` lehnt ab — z. B. Dummy-Position oder Pos-Watcher nicht aktiv.)

---

## 5. Wie wurde es getestet?

- **Manuell (nach Build):** TeamSpeak + Plugin starten; Mod POST ~30 ms senden.
  - Log: ~1 Rate-Zeile/s, keine Flut von `HTTP: IN … raw=` bei 200.
  - Mit `debug=1`: DBG-Zeilen mit seq/x/y/z pro Sample.
  - Ungültiger Body → `reason=parse`; Body mit 0,0,0 → `reason=reject`.
- **Nicht gebaut** in diesem Arbeitspaket (kein Build-Schritt angefordert).

---

## 6. Lerneffekt

1. **High-frequency Events** gehören nicht in `log_write` — Rate-Limits oder Debug-Level verhindern Log-Spam und Lock-Contention.
2. **Fehlerursachen trennen** (parse vs reject) spart Stunden Debug-Zeit, auch wenn der HTTP-Status gleich bleibt (400).
3. **Thread-Statistik ohne Lock** ist OK, wenn ein Thread sequentiell arbeitet (hier: ein Request nach dem anderen auf dem Listener-Thread).
