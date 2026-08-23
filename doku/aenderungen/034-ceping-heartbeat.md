# 034 — CEPING: Liveness-Heartbeat + Verlusterkennung

**Phase:** V8.15 · **Version:** 8.0.4

## Was wurde geaendert?

Neues Plugin-Kommando `CEPING:` als **niedrigfrequenter Heartbeat mit
Sequenznummer**. Jeder Client schickt ~1× pro Sekunde (angetrieben vom
Positions-Tick) eine fortlaufende Zahl mit. Der Empfaenger erkennt daran, wenn
von einem Peer **Updates verloren gehen** — reine Diagnose, kein Audio.

Konkret:

- **Neu:** `src/ts/proximity/ts3_ceping.{h,c}` — Senden (ratenbegrenzt),
  Empfangen, Peer-Sequenztabelle (`TS3_MAX_CLIENT_ID` Slots, Bounds-Check ueber
  `ts3_client_id_valid()`), Verlust-Logging.
- **Neu:** `src/ts/proximity/ts3_ceping_wire.h` — reiner Codec (Bauen, strenges
  Parsen, `ceping_seq_gap()` mit uint32-Ueberlauf). Header-only, ohne Win32/
  TS-SDK, damit ihn der Host-Unit-Test direkt prueft. Gleiches Muster wie
  `ts3_cemode_wire.h` / `wakeup_policy.h`.
- **Neu:** `tests/ceping_wire_test.c` + Eintrag in `tests/run_tests.sh`.
- `ts/entry/ts3_entry.c`: Praefix-Dispatch nach CEMODE/vor CEPOS;
  CEDRAIN-Schritt nach `ts3_cemode_flush()`; `ts3_ceping_send_pending()` in
  `ts3_pending_work_any()`; `ts3_ceping_signal_send_pending()` im Positions-Tick;
  `ts3_ceping_clear_client()` beim Verlassen; `ts3_ceping_reset()` in Disconnect-
  und Shutdown-Reset.
- `project/Conan-Exiles-TeamSpeak.vcxproj` + `build/build_mingw.sh`: neue
  Quelldatei eingetragen.
- Doku: `doku/module/ce-protokoll.md` (Praefix-Karte + Abschnitt 4a),
  `doku/01-architektur-v8.md` (Drain-Reihenfolge, Pending-Liste, Reset-Liste).

## Wie war es vorher?

Es gab keinen Weg, verlorene Positions-/Zustands-Updates eines Peers zu
erkennen. Die Player-Tabelle laesst Eintraege nur nach `PLAYER_TABLE_STALE_MS`
(120 s) rein zeitbasiert verfallen — ob dazwischen Pakete fehlten, war unsichtbar.

## Warum ist die neue Loesung besser?

1. **CEPOS bleibt eingefroren.** Eine Sequenznummer im 56-Byte-Paket haette es
   groesser gemacht und alle alten Clients ausgesperrt (`decoded != sizeof`).
   Ein eigenes Praefix kostet ~12 Byte pro Sekunde und bricht nichts.
2. **Kein Zusatzverkehr im Leerlauf.** Der Heartbeat haengt am Positions-Tick;
   ohne Bewegung/Positionen wird nichts gesendet.
3. **Kein Callback-Spin.** Der Sender setzt sein Pending-Flag bei jedem Tick
   (~30 ms), `ts3_ceping_flush()` sendet aber nur alle 1000 ms. Ist es noch
   nicht so weit, **loescht** es das Flag (statt einen neuen Wakeup anzufordern)
   — so bleibt `ts3_pending_work_any()` nicht dauerhaft „true" und der
   Callback-Thread dreht nicht fuer einen Heartbeat leer. Der naechste Tick
   setzt das Flag ohnehin neu.
4. **Ueberlaufsicher.** `ceping_seq_gap()` rechnet die Differenz als uint32 mit
   Wrap; der Zaehlerwechsel `0xFFFFFFFF → 0` zaehlt korrekt als „kein Verlust".

## Wie funktioniert es jetzt?

Format: `CEPING:1;<seq>` (Version 1, uint32-Zaehler). Details:
`doku/module/ce-protokoll.md` Abschnitt 4a.

```text
 Pos-Watcher-Tick (fremder Thread)
   ts3_on_local_position_update()
     └─ ts3_ceping_signal_send_pending()
          └─ Flag setzen + ts3_request_wakeup()   (keine TS-API!)
 Wakeup-Thread ──► sendPluginCommand("CEDRAIN:1")
                                                   ▼
 Callback-Thread: CEDRAIN-Dispatcher
   ... → CEPOS → CEMODE → ts3_ceping_flush()
        (nur wenn >= 1000 ms her: seq++, "CEPING:1;<seq>" senden)
                                                   │
                           TS-Server relayed       ▼
 Callback-Thread beim Peer: ts3_ceping_on_plugin_command()
   → pruefen (Praefix, Plugin-ID, Version, Client-ID-Grenzen)
   → Luecke = ceping_seq_gap(lastSeq, seq)
   → Luecke > 0 ? log_debug("peer X lost N update(s)")
   → lastSeq speichern
```

**Thread-Vertrag:** `ts3_ceping_signal_send_pending()` darf von jedem Thread
kommen (nur Interlocked-Flag + Wakeup). Senden, Empfangen, Peer-Tabelle, Reset
und Clear laufen **ausschliesslich** auf dem Callback-Thread; deshalb braucht
die Sequenztabelle **keinen Lock** (genau wie der CEPOS-Sendezustand — anders
als CEMODE, das eine Info-Panel-Leseseite vom UI-Thread hat).

## Wie wurde es getestet?

- **Build:** `build.ps1 -SkipDeploy` → Release x64 OK (`ts3_ceping.c` uebersetzt,
  Link OK, 505 Funktionen).
- **Unit-Test Codec:** `tests/ceping_wire_test.c` — alle Pruefungen gruen:
  Round-Trip (seq 0, 42, max uint32), Ablehnung kaputter Nutzlasten
  (`""`, `1`, `1;`, `x;5`, `1;x`, `1;5x`), unbekannter Version (0 und 2),
  NULL-Zeiger, zu kleiner Puffer, additives Zusatzfeld (`1;77;extra`) und vor
  allem `ceping_seq_gap` inkl. Duplikat, Reorder und uint32-Wrap
  (`0xFFFFFFFE→0xFFFFFFFF`, `0xFFFFFFFF→0`, sowie die uebersprungenen Faelle).
  Direkt mit MSVC uebersetzt/ausgefuehrt (`cl /W4 /I. /Isrc`, keine Warnungen),
  da auf diesem Rechner kein gcc/Bash fuer `run_tests.sh` vorhanden ist; der
  Codec ist reines C, damit gleichwertig.
- **Manuell im TS-Client (noch offen):**
  1. Zwei Clients mit dem neuen DLL, gleicher Server/Ingame-Kanal, Debug-Log an.
  2. A bewegt sich in Conan → im Log von B tauchen keine „lost"-Zeilen auf,
     solange Pakete ankommen.
  3. Kurzzeitige Netzstoerung/Reconnect → nach Wiederkehr erscheint bei B ggf.
     eine „peer A lost N update(s)"-Zeile.
  4. Client mit altem DLL: sendet kein CEPING, erzeugt keine Log-Zeilen —
     Positionen/Audio laufen unveraendert.

## Lerneffekt

Ein Heartbeat mit Sequenznummer ist das kleinste Werkzeug, um „ist der Peer
noch da und komplett?" zu beantworten — ohne das eigentliche Datenpaket
anzufassen. Zwei Fallen lauern dabei: der **Zaehlerueberlauf** (immer als
vorzeichenlose Differenz mit Maske rechnen, nie `a < b` annehmen) und die
**Drossel-Rueckkopplung** (ein ratenbegrenzter Sender darf das Pending-Flag
nicht dauernd neu anfordern, sonst haelt er den Dispatcher wach). Beides ist
hier bewusst geloest und im Test bzw. Kommentar festgehalten.
