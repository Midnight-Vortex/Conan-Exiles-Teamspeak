# 051 — HTTP-Rate nur Debug + plugin.log 100-MB-Cap

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/http/pos_http_server.c` | Erfolgs-Rate (`fail=0`) nur noch `log_debug`; Rate mit `fail>0` bleibt `log_write` |
| `src/core/mod_file/pos_file.c` | `POS: method A -> B` bleibt `log_write` (Wechsel selten; Nutzer 050) |
| `src/core/util/log.c` / `log.h` | `plugin.log` wird geleert, sobald die Datei **100 MB** erreicht |

**Eine Funktion:** Log-Spam im Normalbetrieb stoppen und die Datei begrenzen.

---

## 2. Wie war es vorher?

Jeder erfolgreiche HTTP-Tick schrieb ~1×/s `HTTP: rate n=33/s …` per `log_write` — bei einer Session von Stunden viele Megabyte, obwohl nichts schiefging. Positionsquellen-Wechsel (050) waren ebenfalls immer sichtbar. `plugin.log` wuchs unbegrenzt (jede Zeile = Datei öffnen/anhängen/schließen).

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Vorher | Jetzt |
|---|---|
| 1 Rate-Zeile/s immer | Rate nur mit `debug=1`; Fehlerfenster (`fail>0`) weiter immer |
| `POS: method` immer | `POS: method` weiter immer (selten, nur Wechsel); Rate-Spam weg |
| Datei ohne Grenze | Hartes Cap **100 MB**, dann Truncate + eine `LOG: cleared …`-Zeile |

100 MB statt 1 GB: liegt unter typischen Documents-Limits und reicht für lange Debug-Sessions. Kein neuer Config-Schlüssel.

---

## 4. Wie funktioniert es jetzt?

```
POST 200, Fenster fail=0  →  log_debug HTTP: rate …
POST-Fenster fail>0       →  log_write  HTTP: rate …   (immer)
POS method-Wechsel        →  log_write (selten)
400 / Boot / Kanal        →  log_write (unverändert)

Jede log_write/log_debug-Zeile (unter Log-Lock):
  Dateigröße >= 100 MB?  →  CREATE_ALWAYS (Datei leer)
                         →  LOG: cleared (plugin.log exceeded 100 MB)
                         →  eigentliche Zeile
```

Kein TS-API. Nur Log-Lock.

---

## 5. Wie wurde es getestet?

- Nicht gebaut in diesem Paket.
- Nach Build, **ohne** Debug: keine `HTTP: rate … ok=33 fail=0`-Flut; 400er und Kanal-Moves bleiben.
- Mit `debug=1`: Rate + 1-Hz-Status; `POS: method` immer.
- Cap: siehe Änderung 052 (250 MB / 1 GB).

---

## 6. Lerneffekt

Periodische „alles ok“-Zeilen gehören ins Debug. Eine harte Dateigröße ist die Absicherung, falls jemand Debug dauerhaft anlässt — nicht ein Ersatz für weniger Log im Default.
