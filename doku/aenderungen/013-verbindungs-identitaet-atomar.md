# 013 — Verbindungs-Identitaet atomar publiziert (V8.5)

**Datum:** 2026-07-23 · **Phase:** V8.5 (Ein-Besitzer-Zustand: Verbindungs-Epoche atomar)

## Was wurde geaendert?

Nur eine Datei: `src/ts/adapter/ts3_adapter.c`.

1. Die Verbindungs-Identitaet (`g_activeConnection`, `g_localClientID`, `g_connected`)
   hat jetzt einen **dokumentierten Vertrag** direkt an den Variablen (Kommentarblock).
2. Alle **thread-uebergreifenden** Zugriffe auf die 64-Bit-Verbindungs-ID laufen ueber
   zwei kleine Helfer: `conn_id_store()` (`InterlockedExchange64`) und
   `conn_id_load()` (`InterlockedCompareExchange64`).
3. Die **Publish-Reihenfolge** ist jetzt in allen drei Schreib-Pfaden gleich und erzwungen:
   - **Connect** (`ts3_on_connect_status_changed`, ESTABLISHED): erst ID + lokale
     Client-ID setzen, **`g_connected = 1` als LETZTES**.
   - **Disconnect** (DISCONNECTED-Zweig und `ts3_adapter_shutdown`):
     **`g_connected = 0` als ERSTES**, dann ID + lokale Client-ID loeschen.
   - **Tab-Wechsel** (`ts3_on_active_server_changed`): erst `g_connected = 0`
     (alte Identitaet ist ab sofort tabu), dann neue ID, dann — nur wenn der neue
     Tab wirklich verbunden ist — `g_connected = 1`.
4. Der **Wakeup-Thread** liest die ID jetzt einmal mit Barriere in eine lokale
   Variable (`conn`) und ueberspringt den Send bei `conn == 0`.

## Wie war es vorher (V7 / bis V8.4)?

- `g_activeConnection` war ein `volatile uint64` mit **einfachen Zuweisungen**
  (`g_activeConnection = ...;`). Auf x64 wird ein ausgerichteter 64-Bit-Wert zwar
  nicht "zerrissen" (kein Torn Read), aber `volatile` garantiert unter MinGW/GCC
  **keine Reihenfolge** gegenueber anderen Variablen. Der Compiler oder die CPU
  durfte den ID-Store gegenueber dem `g_connected`-Store umsortieren.
- Beim **Tab-Wechsel** war die Reihenfolge sogar logisch falsch: erst wurde die
  **neue** ID gespeichert, dann erst `g_connected` angepasst. Wenn der alte Tab
  verbunden war (`g_connected == 1`) und der neue noch nicht, konnte der
  Wakeup-Thread in diesem Fenster "verbunden" + "neue ID" sehen und
  `sendPluginCommand` auf einen **noch nicht aufgebauten** Tab absetzen.
- Leser wie der PCM-Pfad (`onEditPlaybackVoiceDataEvent`) und der Wakeup-Thread
  laufen auf fremden Threads — genau die Konstellation, in der so eine
  Umsortierung real sichtbar wird.

## Warum ist die neue Loesung besser/stabiler?

- `Interlocked*`-Funktionen sind auf x86/x64 **volle Speicherbarrieren**: was vor
  dem Interlocked-Store geschrieben wurde, sieht jeder Thread, der den Wert
  danach liest. Damit ist die Reihenfolge "ID zuerst, connected zuletzt"
  (Connect) bzw. "connected zuerst, ID danach" (Disconnect) fuer ALLE Leser
  verbindlich — nicht nur zufaellig auf dieser CPU.
- Die Leser-Regel ist einfach und ueberall gleich: **erst `ts3_is_connected()`
  pruefen, dann die ID benutzen** — oder die ID exakt vergleichen (ID 0 matcht
  nie einen echten Handler). Weil `g_connected` als letztes auf 1 und als erstes
  auf 0 geht, kann kein Leser je "verbunden" mit einer halb-publizierten
  Identitaet kombinieren.
- Bewusst **kein** Umbau auf eine gepackte Epoch-Struktur: die geordnete
  Publikation reicht hier aus (kleinste korrekte Aenderung gewinnt).

## Wie funktioniert es jetzt?

```
Callback-Thread (einziger Schreiber)          fremde Leser
─────────────────────────────────────         ─────────────────────────────
CONNECT:                                      Wakeup-Thread:
  g_connected = 0      (Interlocked)            if (!ts3_is_connected()) skip
  ID = neu             (Interlocked64)           conn = conn_id_load()
  localClientID = neu  (Interlocked)             if (conn == 0) skip
  g_connected = 1  ◀── LETZTES                   sendPluginCommand(conn, …)

DISCONNECT / Shutdown:                        PCM-Thread (Playback-Event):
  g_connected = 0  ◀── ERSTES                    event-ID == conn_id_load()?
  localClientID = 0                              nein → Samples unangetastet
  ID = 0                                         (ID 0 matcht nie)
```

Der bestehende "Passthrough zuerst"-Reset bleibt unveraendert gueltig:
`ts3_reset_connection_state()` (in `ts3_entry.c`) schaltet den Audio-Pfad
**vor** dem Leeren der Tabellen auf Passthrough, und beim Connect-Event wird
Passthrough gesetzt, **bevor** der Adapter die neue Identitaet publiziert.

Hinweis zur Reihenfolge beim Disconnect: die ID wird bereits im Adapter (also
vor den Modul-Resets in `ts3_entry.c`) genullt. Das ist die sichere Richtung —
eine ID von 0 matcht kein Event, der PCM-Pfad faellt dadurch sogar **frueher**
in den Passthrough-Zweig. Kein Modul-Reset benoetigt die alte ID.

Die vielen `g_ts3.*(g_activeConnection, …)`-Aufrufstellen bleiben absichtlich
einfache Reads: sie laufen ausschliesslich auf dem Callback-Thread — demselben
Thread, der die Variable schreibt. Gleicher Thread = keine Race, keine Barriere
noetig.

## Wie wurde es getestet?

- `bash tests/run_tests.sh` — alle 6 Suiten gruen (251 Checks).
- `bash build/build_mingw.sh` — DLL linkt fehlerfrei (`-Wall -Wextra`, keine
  neuen Warnungen).
- Manuelle TS-Client-Tests (durch den Menschen, siehe auch `doku/014`):
  1. 10× schnell verbinden/trennen — kein Haenger, kein Crash, Proximity
     funktioniert nach jedem Reconnect.
  2. Zwei Server-Tabs: waehrend Sprache laeuft mehrfach den Tab wechseln —
     Audio des inaktiven Tabs bleibt unangetastet (Passthrough).

## Lerneffekt

`volatile` ist unter GCC/MinGW **kein** Ersatz fuer Atomics: es verhindert nur,
dass der Compiler den Zugriff wegoptimiert — es ordnet nichts. Wenn zwei
Variablen zusammen eine Wahrheit bilden ("verbunden" + "welche Verbindung"),
braucht man eine definierte Publish-Reihenfolge mit Barrieren, und alle Leser
muessen dieselbe Gate-Variable zuerst pruefen.
