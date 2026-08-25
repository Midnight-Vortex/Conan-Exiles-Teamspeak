# 050 — Positionsquelle HTTP vs Pos.txt loggen

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/mod_file/pos_file.c` | `POS: method A -> B` bei Wechsel (`http` / `file` / `none`); Debug 1 Hz Status; Debug wenn Pos.txt wegen HTTP-Hold übersprungen wird |
| `src/core/mod_file/pos_file.h` | Thread-Kommentar: Inject/Watcher loggen nur über Log-Lock |
| `doku/module/pos-http.md` | Abschnitt Positionsquelle |

**Eine Funktion:** sichtbar machen, ob die aktuelle Position aus **HTTP** oder dem **Pos.txt-Fallback** kommt.

---

## 2. Wie war es vorher?

HTTP-POSTs und Pos.txt liefen parallel (Hold ~2 s, dann Datei). Im Log sah man `HTTP: rate …` und `POS: coordinates invalid (file stale…)`, aber **keine** klare Zeile „gerade HTTP“ vs „gerade Datei“. Bei Debug fehlte ein regelmäßiger Snapshot (Hold-Restzeit, Datei-Alter).

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Vorher | Jetzt |
|---|---|
| Quelle nur indirekt (HTTP-Zeilen vs Datei-stale) | Wechsel immer: `POS: method http -> file` |
| Kein Hold-Hinweis | `debug=1`: Pos.txt skipped (HTTP hold left=…) |
| Kein periodischer Überblick | `debug=1`: 1×/s `method=… valid=… holdLeft=… posFile=… fileAge=… seq=…` |

Kein Extra-`log_write` pro 30 ms HTTP-Sample (Änderung 047 bleibt).

---

## 4. Wie funktioniert es jetzt?

```
HTTP inject ──► pos_set_source(http)  ──► nur bei Wechsel: log_write
Pos.txt accept ─► pos_set_source(file) ──► nur bei Wechsel: log_write
ungueltig ──────► pos_set_source(none)

Watcher (debug=1, 1 Hz):
  POS: method=http valid=1 holdLeft=1800ms posFile=1 fileAge=19999ms seq=42
```

Threads: Inject = HTTP-Listener; Watcher = Pos-Thread. Logging nur Log-Lock, keine TS-API.

**Immer sichtbar (Wechsel):**

```text
POS: method none -> http
POS: method http -> file
POS: method file -> none
```

**Nur debug=1:** 1-Hz-Status, Hold-Skip, Datei-Seq (wie bisher, jetzt mit `method=file`).

---

## 5. Wie wurde es getestet?

- Nicht gebaut in diesem Paket.
- Nach Build: TS neu starten, `debug=1` in `plugin.cfg` (oder F10 Debug).
  - Mod sendet HTTP → `POS: method none -> http`, dann 1 Hz `method=http holdLeft>0`.
  - HTTP stoppen, Pos.txt frisch → `POS: method http -> file`.
  - Ohne Debug: nur die `method A -> B`-Zeilen, keine 1-Hz-Flut.

---

## 6. Lerneffekt

Zustandswechsel (welche Quelle aktiv ist) gehören in `log_write`; Hochfrequenz-Details (Hold-Rest, Dateialter) in gedrosseltes `log_debug`. Sonst ist das Log entweder leer oder unlesbar.
