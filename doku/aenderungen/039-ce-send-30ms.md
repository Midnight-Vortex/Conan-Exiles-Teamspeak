# 039 — Wichtige CE-Sends auf dem 30-ms-Tick

**Phase:** V8.17 · **Bezug:** `PLUGIN_POLL_INTERVAL_MS`, `doku/module/ce-protokoll.md`

## Was wurde geaendert?

Die Sende-Drossel von `CEPING`, `CEMODE` und `CEAUTH` nutzt jetzt denselben
Boden wie CEPOS, Pos-Watcher und CEDRAIN-Wakeup: **`PLUGIN_POLL_INTERVAL_MS`
(30 ms)**. Keine eigenen 1000-ms- bzw. 250-ms-Intervalle mehr.

Konkret:

- `src/ts/proximity/ts3_ceping.c`: `CEPING_SEND_MIN_MS` = `PLUGIN_POLL_INTERVAL_MS`
- `src/ts/proximity/ts3_cemode.c`: `CEMODE_SEND_MIN_MS` = `PLUGIN_POLL_INTERVAL_MS`
- `src/ts/proximity/ts3_ceauth.c`: `CEAUTH_SEND_MIN_MS` = `PLUGIN_POLL_INTERVAL_MS`
- `src/core/util/poll_interval.h`: Kommentar nennt die CE-Sends als Mitbenutzer
  des 30-ms-Bodens
- Doku: `doku/module/ce-protokoll.md` (Praefix-Karte, Wire-Regel 6, Abschnitte
  4.2 / 4a.2 / 4b.2)

Ausloeser sind unveraendert: CEPING nur bei Positions-Ticks, CEMODE nur bei
Flanke/Connect/Erstkontakt, CEAUTH nur bei Connect/Erstkontakt. Geaendert ist
nur, **wie schnell** ein bereits gesetztes Pending-Flag wirklich rausgeht.

## Wie war es vorher (V7 / fruehes V8)?

CEPOS, der Pos-Watcher und der Wakeup liefen schon auf 30 ms. Die drei neueren
Plugin-Commands hatten **eigene, langsamere Boeden**:

| Kommando | Vorher | Folge |
|---|---|---|
| `CEPING` | 1000 ms | Sequenzsprung 1 = eine verpasste **Sekunde**, nicht ein Tick |
| `CEMODE` | 250 ms | Moduswechsel konnte bis 250 ms hinter CEPOS hinterherhinken |
| `CEAUTH` | 250 ms | Identitaet nach Connect/Erstkontakt bis 250 ms verzoegert |

Die Flags wurden zwar alle 30 ms gesetzt (CEPING vom Watcher), der Flush hat
sie aber bewusst liegen gelassen.

## Warum ist die neue Loesung besser/stabiler?

1. **Eine Uhr fuer wichtigen Verkehr.** Wer „laeuft das Plugin noch?“ oder
   „welchen Modus hat der Peer?“ fragt, bekommt die Antwort auf demselben Tick
   wie die Position — nicht auf einem zweiten, langsameren Takt.
2. **CEPING diagnostiziert CEPOS-Verluste tickgenau.** Ein Sprung in `seq`
   bedeutet jetzt „ein 30-ms-Update fehlt“, passend zum CEPOS-Send-Boden.
3. **Keine Extra-Logik.** Die Module bleiben Flanke/Connect/Tick; nur die
   Konstante zeigt auf `PLUGIN_POLL_INTERVAL_MS`. Wer den Boden aendert, aendert
   alle wichtigen Sends zusammen.
4. **Idle bleibt idle.** Ohne Positions-Tick geht weiterhin kein CEPING raus.
   CEMODE/CEAUTH bleiben Einmal-Ereignisse.

## Wie funktioniert es jetzt?

```text
  30-ms-Tick (Pos-Watcher / CEDRAIN-Wakeup)
        │
        ├─ CEPOS flush     (schon immer >= 30 ms)
        ├─ CEMODE flush    (nur wenn Flag; jetzt >= 30 ms statt 250)
        ├─ CEPING flush    (nur nach Positions-Tick; jetzt >= 30 ms statt 1000)
        └─ CEAUTH flush    (nur Connect/Erstkontakt; jetzt >= 30 ms statt 250)
```

**Thread-Vertrag unveraendert:** `*_signal_send_pending()` von jedem Thread
(Flag + Wakeup, keine TS-API). `*_flush()` nur Callback-Thread.

CEPING loescht das Pending-Flag weiterhin, wenn der 30-ms-Boden noch nicht
erreicht ist (kein Callback-Spin). Der Watcher setzt es am naechsten Tick neu.

## Wie wurde es getestet?

- Host-Unit-Tests: `tests/run_tests.sh` (Wire-Codecs unveraendert; Intervall
  ist eine Konstante, kein Parser-Verhalten).
- MinGW-Cross-Build, falls in der Umgebung verfuegbar.
- Manuell im TS-Client (nach Deploy): zwei Clients, Bewegung in Conan —
  Debug-Log zeigt CEPING im ~30-ms-Takt statt ~1 Hz; Moduswechsel erscheint
  ohne spuerbare Verzoegerung im Info-Panel des Peers.

## Lerneffekt

Ein zweiter, langsamerer Takt fuer „weniger wichtiges“ Wire-Zeug klingt nach
Sparsamkeit, zerlegt aber die Diagnose: Heartbeat und Positionsstrom laufen
dann nicht mehr im Gleichschritt. Wichtige Zustaende gehoeren auf **eine**
Uhr — hier 30 ms — und sparen Verkehr durch **seltene Ausloeser**, nicht durch
eine Extra-Drossel.
