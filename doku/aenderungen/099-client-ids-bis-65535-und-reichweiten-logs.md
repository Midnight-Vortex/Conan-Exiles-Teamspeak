# 099 — Alle Client-IDs unterstuetzt, Unbekannte stumm statt voll laut, Logs fuer „Reichweite nicht respektiert“ (Version 8.0.22)

**Plugin 8.0.22**, 2026-11-09. Anlass: In einer Gruppe von ca. 20 Spielern wurde ein
Spieler, der fluesterte, von anderen **in voller Lautstaerke ueber gut 16 Fundamente** (~41 m) gehoert.
Betroffen war Version 8.0.20 (8.0.21 war noch nicht im Einsatz; alle Dateien der Lautstaerke-Berechnung
sind in 8.0.20 und 8.0.21 identisch).

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `ts3_client_limits.h`, `occlusion.h`, `talk_state.h` | Obergrenze fuer Client-IDs von **4096 auf 65536** (TeamSpeak-`anyID` ist 16 Bit). |
| `ts3_proximity_audio.c` | Eine Stimme ohne Pro-Client-Zustand (nur noch ID 0) wird im Ingame-Modus **stumm** geschaltet statt unveraendert abgespielt; Zaehler + Logzeile. HRTF-Zustand (0,8 KB pro Client) bleibt auf die ersten 4096 IDs begrenzt; hoehere IDs spielen mit dem TeamSpeak-Pan. Aufraeum-Suche (`audio_snap_find_orphans`) geht nur bis zur hoechsten bisher benutzten ID. |
| `ts3_proximity_audio.c` | **Diagnose, immer an (ohne DebugMode):** eine Statuszeile `AUDIO-STAT` alle 30 s, ein Schnappschuss-Block `AUDIO-SNAP` (alle Eingaben jedes Sprechers) auf Knopfdruck oder automatisch, Warnung bei hoerbarem Sprecher mit altem Stand, `AUDIO-DBG` mit allen Rechen-Eingaben im DebugMode (siehe 4). |
| `player_table.c/.h` | `player_table_put_count()`: Zaehler der empfangenen Positionspakete (fuer `AUDIO-STAT`). |
| `ts3_info.c` | Neuer Menuepunkt **Plugins → Conan Exiles → „Diagnose-Schnappschuss ins Log schreiben“** (setzt nur ein Flag; der Callback-Thread schreibt den Block und meldet es im TS-Chat). |
| `ts3_cepos.c` | Logzeile (hoechstens 1×/min), wenn ein Paket von einer ungueltigen Client-ID verworfen wird. |
| `ts3_entry.c` | `onServerErrorEvent` protokolliert **Flood-Fehler** (`ERROR_client_is_flooding`) des Servers (hoechstens alle 10 s, mit Zaehler), zaehlt sie fuer `AUDIO-STAT` und loest einen Schnappschuss aus. Version 8.0.22. |
| Tests | `audio_pcm_test` [7] neu (ID 0 stumm, hohe IDs bekommen die Entfernungs-Daempfung) und [099] neu (Schnappschuss, Statuszeile, Warnung, Flood, Rate-Limit), `talk_state_test`, `occlusion_test` an die neue Grenze angepasst. Das Test-Log wird jetzt mitgeschrieben (`log_write` aufgefangen). |

---

## 2. Wie war es vorher?

- Das Plugin legte Zustand pro Client in Feldern der Groesse **4096** ab. Ein Sprecher mit TeamSpeak-
  Client-ID **ab 4096** hatte keinen Platz: sein Positionspaket wurde ohne Logzeile verworfen, und seine
  Stimme wurde im Ingame-Modus **unveraendert** abgespielt — fuer alle Hoerer, egal ob er floesterte,
  wie weit weg er stand oder in welchem Modus er war. Der Code-Kommentar sagte „servers in practice stay
  well below 4096“.
- Das war die **einzige** Stelle, an der ein unbekannter Sprecher laut statt stumm blieb; fehlt dagegen
  die Position (kein CEPOS), wird er stumm geschaltet.
- Verlorene Positionspakete (Server-Flood-Schutz, Paketverlust) und alte Eintraege blieben unsichtbar.

---

## 3. Warum ist die neue Loesung besser?

- Jede 16-Bit-ID ist jetzt gueltig; nur ID 0 („kein Client“) nicht. So faellt kein Spieler mehr aus der
  Entfernungs-Daempfung heraus.
- Wo es doch einen Sprecher ohne Zustand gibt, ist das Ergebnis **stumm** (geschlossen), nicht „voll laut“.
- Beim naechsten Vorfall zeigt das Log, ob eine ID zu hoch war, ob der Server Befehle abgelehnt hat oder ob
  ein Sprecher mit altem Stand gehoert wurde — vorher war das nicht herauszufinden.

---

## 4. Wie funktioniert es?

**Wichtig: alles Folgende steht im normalen `plugin.log` — der DebugMode muss dafuer nicht an sein**
(`Documents\Conan Exiles TeamSpeak plugin\plugin.log` beim **Hoerer**; Ordner ueber das Plugin-Menue
„Log-Ordner oeffnen“). Das Log wird nicht von mir „ausgelesen“: ich kann nur die Datei lesen, die auf
diesem Rechner liegt; die Logs anderer Spieler muessen sie schicken. Das Plugin sendet nichts selbststaendig.

| Zeile | Bedeutung |
|---|---|
| `AUDIO-STAT: 30 s \| cepos_rx=… (…/s) speakers=… audible=… oldest_audible=… s oldest_entry=… s \| flood_errors=… \| localPos=ok` | (immer an, alle 30 s, nur wenn Spieler bekannt sind) Wie viele Positionspakete in den 30 s ankamen, wie viele Sprecher bekannt/hoerbar sind, wie alt die Daten der hoerbaren sind, wie oft der Server Befehle abgelehnt hat. Zu wenige Pakete oder ein altes `oldest_audible` = Verlust. |
| `AUDIO-SNAP[menu\|stale-audible\|flood]: mode=… localPos=… hub=… zone=… listenAdd=… maxVolume=… cap=… speakers=n` + je Sprecher `AUDIO-SNAP:   cid=… 'Name' dist=… announced=… range=… gain=… age=…` | (immer an) Schnappschuss mit allen Eingaben der Lautstaerke pro Sprecher. Ausgeloest **per Menue** („Diagnose-Schnappschuss ins Log schreiben“, wenn man gerade etwas Falsches hoert) oder **automatisch** (hoerbarer Sprecher mit altem Stand / Server-Flood; hoechstens alle 2 Minuten). |
| `AUDIO: client N is audible (gain …) but his last position is X s old - CEPOS keepalive missing` | (immer an, je Sprecher hoechstens alle 30 s) Der Sprecher ist hoerbar, sein letztes Positionspaket ist aelter als 5 s. Sein Lebenszeichen kommt sonst jede Sekunde → Pakete gehen verloren oder seine Positionsquelle haengt. Die Lautstaerke stammt dann vom **alten Stand**. |
| `AUDIO-DBG cid=… dist=… announced=… cap=… listenAdd=… -> range=… gain=… age=…` | (nur DebugMode, je Sprecher alle 5 s) Wie der Schnappschuss, laufend. |
| `TS-EVT: server anti-flood refused a command (n in the last interval)` | Der TeamSpeak-Server hat Plugin-Befehle (z. B. Positionen) wegen Flood-Schutz abgelehnt. |
| `CEPOS: dropped a packet from client ID …` | Ein Positionspaket kam von einer ungueltigen ID. |
| `AUDIO: n voice blocks of a client ID without state were silenced` | Stimme einer ID ohne Zustand wurde stumm geschaltet (sollte nie vorkommen). |

---

## 5. Wie wurde es getestet?

- Host-Tests (`tests/run_tests.sh`, MSYS2-gcc): alle Suiten gruen **ausser** `player_table_test`,
  `render_state_test`, `proto_flush_test` — diese drei kompilieren auf dem Windows-gcc nicht (Linux-only
  Funktion, SDK-Header-Typen, `strncpy_s`); die Fehler betreffen keine geaenderte Datei.
- `audio_pcm_test` [7]: ID 0 stumm (MUTE und PROXIMITY), unveraendert nur im PASSTHROUGH; die IDs 4096,
  5000, 60000, 65535: ohne Position stumm, 60 m entfernt (Reichweite 40 m) stumm, 5 m entfernt gedaempft
  nach dem Snapshot (nicht unveraendert).
- MSVC-Release-Build (`build.ps1 -SkipDeploy`) fehlerfrei; `.data` waechst auf ~30,8 MB **virtuell**
  (nur Nullseiten, die Datei bleibt 7,1 MB gross).
- **Nicht geprueft:** kein Test mit echten Spielern, nichts deployt. Ob eure Server-IDs tatsaechlich ueber
  4095 steigen, ist nicht belegt (siehe 6).

---

## 6. Was jetzt zu tun ist / Grenzen

1. Bei einem Vorfall: im Moment des falschen Hoerens im Menue **„Diagnose-Schnappschuss ins Log schreiben“**
   waehlen (das TS-Chatfenster bestaetigt es), danach das `plugin.log` des **Hoerers** und des **Sprechers**
   sichern. Ohne DebugMode, ohne Neustart.
2. Pruefen, ob der Server hohe Client-IDs vergibt: ServerQuery `clientlist` zeigt pro Client die `clid`.
3. Anti-Flood-Werte des Servers ansehen (`virtualserver_antiflood_points_*`, vor allem
   `…_needed_plugin_block`): 20 Spieler senden zusammen bis zu ~600 Positionen pro Sekunde.
4. Weiter offen (nicht Teil dieser Aenderung): Eintraege bleiben bis zu **120 s** gueltig
   (`PLAYER_TABLE_STALE_MS`) und die Obergrenze fuer die gemeldete Reichweite fehlt, sobald im Hub einer der
   drei `Maximum…`-Werte fehlt. Das Log zeigt, ob eines davon zutraf.

---

## 7. Lerneffekt

Ein „sicher ignoriert“-Pfad fuer unbekannte Eingaben muss **schliessen** (stumm), nicht **durchlassen**:
Wer nicht zugeordnet werden kann, darf nicht ungedaempft bei allen ankommen. Und eine Annahme wie „Server
bleiben unter 4096“ gehoert nicht in einen Kommentar, sondern in eine Pruefung mit Logzeile.
