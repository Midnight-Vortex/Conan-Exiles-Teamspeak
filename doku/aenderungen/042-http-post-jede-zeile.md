# 042 — HTTP-POST: jede Erfolgszeile immer sichtbar

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/http/pos_http_server.c` | Erfolgreicher POST (Inject, HTTP 200): eine `log_write`-Zeile mit seq/pos/dt/status; kein `http_log_raw_in` mehr bei 200 |
| `doku/module/pos-http.md` | Abschnitt 9: Erfolgs-POST immer sichtbar, nicht mehr nur `debug=1` |
| `doku/aenderungen/040-http-debug-log.md` | Verweis auf 042 (Erfolgszeilen wieder `log_write`) |

**Betroffen:** POST-Zweig in `http_handle_client` — nur Inject-Erfolg (status=200).

---

## 2. Wie war es vorher?

Ab Änderung 040 gingen erfolgreiche POSTs nur über `log_debug` — sichtbar erst mit `debug=1` in `plugin.cfg`. Zusätzlich schrieb `http_log_raw_in` bei HTTP 200 noch eine zweite DBG-Zeile mit dem rohen Body.

Ohne Debug-Modus sah man praktisch nur die ~1 Hz Rate-Zeile (`HTTP: rate n=…/s`). Einzelne Positionen „tauchten selten auf“, obwohl der Mod alle 30 ms sendete.

---

## 3. Warum ist die neue Lösung besser?

| Vorher | Jetzt |
|---|---|
| Erfolg nur mit `debug=1` | Erfolg immer in `plugin.log` (`log_write`) |
| Zwei Zeilen pro Erfolg (POST-Felder + raw IN) | **Eine** Zeile pro Erfolg — weniger I/O, trotzdem volle Koordinaten |
| Rate-Zeile allein unklar bei Debugging | Jeder Inject = nachvollziehbare Zeile + weiterhin 1/s Rate-Überblick |

Reject (400, parsed) und Parse-Fehler (400) bleiben unverändert — je eine `log_write`-Zeile mit `reason=reject` bzw. `reason=parse`.

---

## 4. Wie funktioniert es jetzt?

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
              log_write       log_write 400
              POST seq/pos    reason=reject
              dt status=200   + seq/pos + raw
                    │
                    ▼
              http_post_rate_tick()
              (~1/s log_write Rate-Zeile)
```

Alles läuft auf dem **HTTP-Listener-Thread** (accept → handle → close). Logging nutzt den **Log-Lock** (`log_write`), nie die TS-API.

**Erfolg (immer sichtbar, eine Zeile pro POST):**

```text
HTTP: POST seq=42 pos=X=12345.000000 Y=67890.000000 Z=200.000000 YAW=45.000000 YAWY=0.000000 dt=30ms status=200
```

GET `/health` und 404 behalten optional `http_log_raw_in` (selten).

---

## 5. Wie wurde es getestet?

- **Manuell (nach Build):** TeamSpeak neu starten; Mod POST ~30 ms an `/v1/position`.
  - `plugin.log`: ~eine `HTTP: POST … status=200`-Zeile pro Request, **ohne** `debug=1`.
  - Keine zweite `HTTP: IN POST … raw=` bei 200.
  - Weiterhin ~1×/s `HTTP: rate n=…`.
  - Ungültiger Body → `reason=parse`; Dummy-Position → `reason=reject` (je eine Zeile).
- **Nicht gebaut** in diesem Arbeitspaket (kein Build-Schritt angefordert).

---

## 6. Lerneffekt

1. **`log_debug` vs `log_write`:** Wenn Operatoren etwas *immer* sehen müssen, gehört es in `log_write` — Debug-Level ist für optionale Detailtiefe, nicht für Kern-Diagnose.
2. **Eine Zeile pro Event:** Strukturierte Felder (seq, X/Y/Z, dt) ersetzen oft eine zusätzliche Raw-Zeile; weniger Log-Zeilen = weniger Lock-Zeit bei hoher Rate.
3. **Rate + Einzelzeile kombinieren:** 1/s Zusammenfassung für Überblick, Einzelzeilen für Sample-Nachverfolgung — ohne `debug=1`-Hürde.
