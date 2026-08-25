# 045 — HTTP recv fail-fast (kein 5-s-Block bei toten Sockets)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/http/pos_http_server.c` | `HTTP_RECV_TMO_MS` 5000 → **100**; `TCP_NODELAY` pro Client; gedrosseltes Drop-Log bei `recv`-Fehler / unvollständigen Headern |

**Betroffen:** `http_handle_client()` — nur Annahme/Recv eines Clients, kein Worker-Pool, kein Keep-Alive.

---

## 2. Wie war es vorher?

Der HTTP-Listener arbeitet **sequentiell**: `accept` → `http_handle_client` → `closesocket`. CEE `Start Http Request` wartet **nicht** auf den vorherigen Request — der Mod kann POSTs überlappen.

Pro Verbindung galt `SO_RCVTIMEO` = **5000 ms**. Bricht CEE ab, bleibt der Socket hängen oder sendet nie vollständig, blockierte der Listener bis zu **5 Sekunden** pro totem Socket. Alle weiteren POSTs mussten warten.

`recv` mit `n <= 0` oder unvollständigen Headern endete **still** (kein Log) — im Log sah man nur seltene POST-Zeilen und große `dt`-Spitzen, obwohl der Mod gar nicht lieferte.

---

## 3. Warum ist die neue Lösung besser?

| Vorher | Jetzt |
|---|---|
| Bis 5 s Block pro totem Socket | Max. ~100 ms, dann weiter zum nächsten `accept` |
| Stille Drops | Gedrosseltes `HTTP: drop …` (~1 Hz) für Diagnose |
| Kein TCP_NODELAY | Kleine localhost-POSTs (~200 B) ohne Nagle-Verzögerung |

**Wichtig:** Ein `dt=197000ms` in der Rate-Zeile bedeutet weiterhin „Mod hat lange **keinen** erfolgreichen POST geliefert“ — das Plugin erfindet keine 30-ms-Samples. Diese Änderung behebt nur **Listener-Blockaden** durch abgebrochene/halboffene Verbindungen, nicht fehlende Mod-Daten.

---

## 4. Wie funktioniert es jetzt?

```
CEE POST (evtl. überlappend, je Request eigener Socket)
        │
        ▼
accept() ──► http_handle_client()
        │         SO_RCVTIMEO = 100 ms
        │         TCP_NODELAY = 1
        │         recv bis \r\n\r\n
        │              │
        │         n<=0 ─┴─► HTTP: drop reason=recv err=… (max 1/s)
        │         kein Header-Ende ─► HTTP: drop reason=incomplete (max 1/s)
        ▼
POST parse/inject ──► log_write Erfolg (unverändert)
        │
        ▼
Connection: close ──► closesocket ──► accept (nächster Client)
```

Alles auf dem **HTTP-Listener-Thread** — kein extra Lock für Drop-Log (nur `g_httpLastDropLog`, sequentiell).

---

## 5. Wie wurde es getestet?

- **Manuell (nach Build):** TeamSpeak neu starten; Mod mit ~30 ms POST an `/v1/position`.
  - Erfolg: weiterhin `HTTP: POST … dt≈30ms status=200` und `HTTP: rate … dt=min/avg/max≈30/30/35ms`.
  - Bei abgebrochenen/leeren Verbindungen: höchstens ~1×/s `HTTP: drop reason=recv err=…` oder `reason=incomplete` — **keine** 5-s-Pausen mehr zwischen POSTs.
  - Überlappende CEE-Requests: zweiter POST blockiert nicht mehr hinter einem 5-s-Timeout.
- **Nicht committed** in diesem Arbeitspaket.

---

## 6. Lerneffekt

Bei **sequentiellen** Servern ist das recv-Timeout der kritische Engpass — nicht die POST-Verarbeitung selbst. Kurzes Timeout + sichtbare (aber gedrosselte) Drop-Logs trennen „Mod sendet nicht“ (`dt` groß, keine POST-Zeilen) von „Listener hing an totem Socket“ (`HTTP: drop`, früher 5 s Stille).
