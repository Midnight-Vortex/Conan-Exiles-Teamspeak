# 056 — HTTP-Listener nach Stoerung wiederbeleben

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/http/pos_http_server.c` | `accept()`-Fehler beendet den Listener nicht mehr; Rebind + weiter horchen. Bind/Listen in `http_open_listen_socket()` |
| `doku/module/pos-http.md` | Abschnitt 10: File-Notfall, automatisch zurueck auf HTTP |
| `doku/aenderungen/056-http-listener-recover.md` | Dieser Eintrag |

**Eine Funktion:** HTTP-Listener nach `accept`-Fehler wieder oeffnen, damit Inject
(`pos_set_source(http)`) nach Pos.txt-Fallback wieder greift.

---

## 2. Wie war es vorher?

HTTP hat Vorrang (Hold 2 s). Faellt HTTP aus, uebernimmt Pos.txt — das klappt.

Ein einmaliger `accept()`-Fehler (Socket-Stoerung, nicht nur Shutdown) hat den
Listener-Thread **beendet**. Danach kamen keine POSTs mehr an. Das Plugin blieb
auf `file`, auch wenn der Mod wieder sendet — es gab niemanden, der zuhoert.

`pos_inject_sample` wechselt bereits `file -> http`; ohne Listener passiert das nie.

---

## 3. Warum ist die neue Loesung besser/stabiler?

| Vorher | Jetzt |
|---|---|
| Ein `accept`-Fehler = HTTP tot bis Plugin-Reload | Rebind, `HTTP: listening again` |
| File-Fallback ohne Rueckweg nach Listener-Tod | Naechster POST → `POS: method file -> http` |
| Shutdown | unveraendert: Stop-Event + Socket-Close, Thread bricht sauber ab |

Kein Health-Client noetig: „HTTP geht wieder“ = erfolgreicher Inject.

Shutdown: nach Rebind Stop pruefen und den neuen Socket schliessen; `stop()`
schliesst `g_listenSocket` nochmal nach dem Join (kein Port-Leak auf 52734).

---

## 4. Wie funktioniert es jetzt?

```
HTTP tot (~2 s kein Inject) → Watcher akzeptiert Pos.txt → method http -> file
        │
        ├─ Listener lebt: naechster POST → inject → method file -> http
        └─ accept() fehlgeschlagen:
              Stop? → Ende
              sonst close + 250 ms + http_open_listen_socket()
              → HTTP: listening again
              → naechster POST → inject → method file -> http
```

---

## 5. Wie wurde es getestet?

- Build nach der Aenderung.
- Manuell: HTTP stoppen → `method http -> file`; Mod wieder senden →
  `method file -> http`. Bei Listener-Log `accept failed` muss
  `listening again` folgen, nicht `server stopped`.

---

## 6. Lerneffekt

„Quelle wechselt zurueck“ braucht einen lebenden Empfaenger. Fallback ohne
Recovery des Primaerkanals ist eine Einbahnstrasse.
