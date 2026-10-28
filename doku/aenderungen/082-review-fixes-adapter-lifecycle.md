# 082 — Review-Fixes: Adapter, Plugin-Lebenszyklus, Log

Ein Code-Review hat im Adapter-/Entry-Bereich 14 Fehler gefunden. Dieses Paket
behebt genau diese 14 — nichts darueber hinaus. Betroffen sind
`src/ts/adapter/*`, `src/ts/entry/*`, `src/ts/info/*`, `src/plugin*.h` und
`src/core/util/log.c`. Es gibt drei neue **reine** Header (ohne Win32/TS-Abhaengigkeit,
deshalb auf dem Linux-Rechner testbar) und zwei Testdateien.

## Ueberblick

| Nr. | Problem (Kurzform) | Wo |
|---|---|---|
| 1 | `CEDRAIN` ging an den ganzen Server (N² Relays) | `ts3_adapter.c`, `ts3_entry.c`, neu `drain_target.h` |
| 2 | Plugins > Settings-Dialog beim Shutdown nicht gejoint | `ts3_info.c` |
| 3 | Hintergrund-Tab setzte Audio des aktiven Tabs auf Passthrough | `ts3_entry.c` |
| 4 | Overlay-Start-Thread konnte `overlay_stop()` ueberholen | `ts3_entry.c` |
| 5 | Versions-Tabelle verlor Eintraege nach dem Loeschen | neu `ts3_version_map.h`, `ts3_plugin_version.c` |
| 6 | Versions-Tabelle wurde bei Reconnect nicht geleert | `ts3_entry.c` |
| 7 | CEVER-Broadcast ging verloren, wenn das Plugin waehrend Verbindung aktiviert wird | `ts3_plugin_version.c`, `ts3_entry.c` |
| 8 | Timeout und Kicks wurden nicht behandelt | `ts3_entry.c`, `ts3_exports.h` |
| 9 | Fehlgeschlagene Kanalwechsel (asynchron) blieben unsichtbar | `ts3_adapter.c`, `ts3_entry.c`, neu `ts3_return_code.h` |
| 10 | Aenderungen der Kanalliste wurden nie bemerkt | `ts3_entry.c`, `ts3_exports.h` |
| 11 | `ts3_get_channel_parent` fehlte | `ts3_adapter.c/.h` |
| 12 | UI-Sync lief nie ueber CEDRAIN; veralteter Kommentar | `ts3_entry.c`, `plugin.h` |
| 13 | Kanalnamen standen im Produktiv-Log | `ts3_entry.c`, `ts3_adapter.c` |
| 14 | Log oeffnete/schloss die Datei bei jeder Zeile | `log.c`, `log.h` |

Vorab kurz erklaert (Begriffe, die unten oft vorkommen):

- **Callback-Thread**: der eine Thread, auf dem TeamSpeak unsere `ts3plugin_*`-Funktionen
  aufruft. Nur er darf die TS-API benutzen (Regel 1 in `01-architektur-v8.md`).
- **CEDRAIN**: ein kleiner „Weckruf“, den das Plugin sich selbst schickt. Er laeuft ueber
  TeamSpeaks Plugin-Command-Mechanismus und landet auf dem Callback-Thread, wo der
  Dispatcher dann die liegengebliebene Arbeit abarbeitet. Gesendet wird er vom
  **Wakeup-Thread** (der einzigen Ausnahme vom Thread-Vertrag).
- **Tombstone** („Grabstein“): Markierung in einer Hash-Tabelle fuer „hier war mal
  etwas, ist geloescht“ (Erklaerung bei Fix 5).

---

## 1. CEDRAIN nur noch an den eigenen Client

### Was wurde geaendert?

- Neu: `src/core/util/drain_target.h` (reine Entscheidungslogik, header-only).
- `ts3_adapter.c`: Der Wakeup-Thread sendet `CEDRAIN:1` mit
  `PluginCommandTarget_CLIENT` an `{eigene Client-ID, 0}` statt an den ganzen Server.
  Neu: `ts3_wakeup_note_own_echo()`, ein Epoche-Zaehler `g_drainEpoch`, ein Zeitstempel
  `g_drainEchoMs` und ein 250-ms-Watchdog im Wakeup-Thread.
- `ts3_entry.c` (`ts3plugin_onPluginCommandEvent`, CEDRAIN-Zweig): fremde CEDRAINs
  werden ignoriert; der Eingang des **eigenen** CEDRAIN wird als „Echo“ gemeldet.

### Wie war es vorher (V7/V8)?

`sendPluginCommand("CEDRAIN:1", PluginCommandTarget_SERVER)`: Der Server leitet das
Kommando an **alle** Clients weiter. Im Spiel sendet jeder Client ~33 Mal pro Sekunde
(30-ms-Takt). Bei N Spielern sind das N × 33 Pakete pro Sekunde, die jeder Client
empfaengt (N² Relays) — und **jeder** Client fuehrte auch die CEDRAINs der anderen aus
(ein kompletter Dispatcher-Durchlauf fuer nichts).

### Warum ist die neue Loesung besser/stabiler?

Jeder Client bekommt nur noch seinen eigenen Weckruf: aus N² wird N. Fremde CEDRAINs
(z. B. von aelteren Plugin-Versionen) werden sofort verworfen.

**Wichtig:** Wie TeamSpeak ein Plugin-Kommando behandelt, das an den *Absender selbst*
adressiert ist, ist **nicht verifiziert**. Deshalb gibt es eine Laufzeit-Selbstpruefung
mit Rueckfall — ein Weckruf, der nie ankommt, wuerde das ganze Plugin lahmlegen.

### Wie funktioniert es jetzt?

```
Wakeup-Thread                              Callback-Thread
  drain_target_pick()                        onPluginCommandEvent("CEDRAIN:1")
   ├─ CLIENT-Modus: sendet an {ownId,0}        ├─ Absender != ich?  -> ignorieren
   └─ SERVER-Modus: sendet an alle (alt)       └─ Absender == ich:  ts3_wakeup_note_own_echo()
          │  (Echo-Zeitstempel g_drainEchoMs)          │ (nur ein atomarer Zeitstempel)
          └◄──────────────────────────────────────────┘
```

- **Lauf („Run“)**: Jeder CLIENT-Send zaehlt in einem offenen Lauf (Anzahl + Zeit des
  ersten Sends). Kommt ein Echo, das *neuer* ist als der Laufbeginn, ist der Modus
  bestaetigt und der Lauf wird geschlossen (einmal pro Verbindung ins Log:
  `TS-WAKE: CEDRAIN addressed to own client confirmed`).
- **Rueckfall**: Hat ein Lauf **≥ 5 Sends** und der erste ist **≥ 2000 ms** alt, ohne dass
  ein Echo kam, wechselt der Thread fuer den Rest der Verbindung auf SERVER (einmal ins
  Log: `TS-WAKE: no echo for own-client CEDRAIN ... falling back`). So ist der Zustand von
  vorher wiederhergestellt.
- **Watchdog**: Solange ein Lauf offen ist, weckt sich der Thread alle 250 ms selbst und
  sendet erneut. Dadurch faellt die Entscheidung nach ~2 s auch dann, wenn gerade kein
  anderer Thread einen Weckruf anfordert („Drains stoppen nie dauerhaft“).
- **Neu pruefen**: Bei jeder neuen Verbindung und jedem Tab-Wechsel zaehlt
  `g_drainEpoch` hoch; der Wakeup-Thread setzt seinen Zustand zurueck und probiert wieder
  CLIENT.
- **Eigene ID unbekannt (0) oder Absender nicht gemeldet (0)**: Der Wakeup-Thread sendet
  dann an den Server, und der Callback-Thread akzeptiert das CEDRAIN trotzdem
  (`drain_accept_invoker` ist absichtlich „fail-open“; 0 ist nie eine echte Client-ID). Ein
  Absender 0 zaehlt aber **nicht** als Echo — die Selbstpruefung faellt dann auf SERVER
  zurueck. So kann der Filter das Plugin nie stumm schalten, selbst wenn TeamSpeak den
  Absender eigener Kommandos anders meldet als erwartet.

Kosten im schlechtesten Fall: Verbindung fuer ~2 s ohne Drain, danach wie frueher.

### Wie wurde es getestet?

`tests/wakeup_policy_test.c` (erweitert, Tests [4]–[9]): Bestaetigung per Echo, Rueckfall
exakt bei 2000 ms, duenne Sends (<5) fallen nie zurueck, Watchdog-Simulation (Rueckfall
nach genau 2000 ms mit 8 CLIENT-Sends; gesundes Echo mit 80 ms Roundtrip faellt nie
zurueck), unbekannte ID, veraltetes Echo, Reset, Filter fuer fremde CEDRAINs. Cross-Build
mit MinGW ohne Warnungen. **TS-Client-Test noetig** (siehe unten), weil das echte
TeamSpeak-Verhalten nur dort zu sehen ist.

### Lerneffekt

Wenn man sich auf ein nicht dokumentiertes Verhalten einer Fremd-API stuetzt, baut man
immer eine Laufzeit-Pruefung mit sicherem Rueckfall ein — und prueft die Entscheidung als
reine Funktion, damit sie ohne die Fremd-API testbar ist.

---

## 2. Plugins > Settings-Dialog wird gejoint

### Was wurde geaendert?

`ts3_info.c`, `ts3plugin_configure()`: statt `showConfigInterface()` direkt auf TeamSpeaks
„Configure“-Thread auszufuehren, gilt jetzt `if (config_dialog_try_open())
settings_dialog_open_async();`. Der Dialog laeuft damit auf dem plugin-eigenen,
joinbaren Dialog-Thread (`key_watcher.c`). Dessen Thread gibt die Sperre
(`config_dialog_close()`) **genau einmal** frei; `showConfigInterface()` tut das nicht
mehr. (Die Gegenseite steht in den Paketen UI-DIALOGS/UI-THREADS.)

### Wie war es vorher?

F10 und das Plugins-Menue starteten den Dialog ueber `settings_dialog_open_async()` —
`settings_dialog_shutdown()` kannte diesen Thread und konnte ihn beim Shutdown schliessen
und joinen. Der Weg „TeamSpeak-Einstellungen > Plugins > Conan Exiles > Einstellungen“
lief dagegen auf TeamSpeaks eigenem Thread. Den kannte das Plugin nicht: Wurde das Plugin
deaktiviert, waehrend dieser Dialog offen war, lief Dialog-Code weiter, obwohl die DLL
entladen wurde (Absturz-Gefahr).

### Warum ist die neue Loesung besser/stabiler?

Es gibt nur noch **einen** Weg, einen Dialog-Thread zu starten, und jeder dieser Threads
ist dem Shutdown bekannt (`WM_CLOSE` + Join). Dazu gibt `ts3plugin_configure` jetzt sofort
zurueck und blockiert TeamSpeaks Thread nicht mehr.

### Wie wurde es getestet?

Cross-Build (MinGW). Manueller TS-Test: Dialog ueber Einstellungen > Plugins oeffnen, Plugin
deaktivieren — kein Absturz, Dialog schliesst sich.

### Lerneffekt

Jeder Thread, der Plugin-Code ausfuehrt, muss dem Shutdown bekannt und joinbar sein —
auch die, die ein Fremdsystem fuer uns startet. Dann uebergibt man besser die Arbeit an
einen eigenen Thread, statt auf dem fremden zu laufen.

---

## 3. Hintergrund-Tab darf das Audio des aktiven Tabs nicht anfassen

### Was wurde geaendert?

`ts3_entry.c`, `ts3plugin_onConnectStatusChangeEvent`: der Aufruf
`ts3_audio_set_mode(TS3_AUDIO_PASSTHROUGH)` bei `CONNECTION_ESTABLISHED` steht nicht mehr
**vor**, sondern **hinter** der Pruefung, ob der Adapter das Ereignis ueberhaupt annimmt.

### Wie war es vorher?

Der Aufruf lief fuer jede `CONNECTION_ESTABLISHED`-Meldung — auch fuer einen Server-Tab im
Hintergrund. Ergebnis: Verband man einen zweiten Tab, schaltete das Plugin das Audio des
**sichtbaren** Tabs auf Passthrough (kein Proximity-Effekt mehr), obwohl der Adapter das
Ereignis danach ignorierte.

### Warum ist die neue Loesung besser/stabiler?

Ignorierte Ereignisse haben jetzt keinerlei Nebenwirkung. Fuer ein angenommenes Ereignis
passiert nichts Neues: `ts3_reset_connection_state()` setzt als erste Anweisung selbst
Passthrough (der alte Aufruf war dort also nur eine fruehere Doppelung). Das theoretische
Zeitfenster zwischen „Adapter veroeffentlicht neue Verbindung“ und „Passthrough“ umfasst nur
wenige Anweisungen; Sprache der gerade erst aufgebauten Verbindung trifft in dieser Zeit
nicht ein.

### Wie wurde es getestet?

Cross-Build. Manueller TS-Test: zwei Tabs, im aktiven Tab laeuft Proximity, zweiten Tab
im Hintergrund verbinden — Proximity im aktiven Tab bleibt.

### Lerneffekt

Erst entscheiden, ob ein Ereignis uns betrifft, dann handeln — nie umgekehrt.

---

## 4. Overlay-Start darf `overlay_stop()` nicht ueberholen

### Was wurde geaendert?

`ts3_entry.c`: zwei Flags (`g_overlayStartClosed`, `g_overlayStartInProgress`), die
Funktionen `overlay_close_start_gate()` / `overlay_wait_start_done()` und ein Guard
`if (pluginShuttingDown) return;` in `overlay_schedule_start()`. Reihenfolge im Shutdown:
Tor schliessen (Schritt 1) → … → **warten, bis ein bereits begonnener `overlay_start()`
fertig ist** → erst dann `overlay_stop()` → Overlay-Thread joinen.

### Wie war es vorher?

Der verzoegerte Start-Thread pruefte „laeuft Shutdown?“ und rief **danach**
`overlay_start()` auf. Zwischen Pruefung und Aufruf konnte der Callback-Thread im Shutdown
schon `overlay_stop()` ausfuehren. Dann baute der Start-Thread Fenster und
Monitor-Thread **nach** dem Stop auf; niemand beendete sie mehr — ein Thread lief weiter,
waehrend die DLL entladen wurde.

### Warum ist die neue Loesung besser/stabiler?

Zwei Variablen, die jede Seite erst schreibt und dann die der anderen liest (jeweils mit
vollen Speicher-Barrieren, kein Lock waehrend der Fensteraufrufe):

```
Start-Thread                              Shutdown (Callback-Thread)
 inProgress = 1                            closed = 1
 closed gesetzt?  -> ja: nichts bauen      inProgress gesetzt? -> ja: warten
 nein: overlay_start() ...                 ... erst danach overlay_stop()
 inProgress = 0
```

Mindestens eine Seite sieht die andere. Entweder baut der Start-Thread gar nicht erst, oder
der Shutdown wartet, bis er fertig ist — `overlay_stop()` laeuft also nie vor oder waehrend
des Starts. Der Guard in `overlay_schedule_start()` verhindert zusaetzlich, dass ein
Ereignis waehrend des Shutdowns noch einen neuen Start-Thread anlegt, der nach dem Join
uebrig bliebe. Zusammen mit der zusaetzlichen Pruefung innerhalb von `overlay_start()`
(UI-THREADS, Fix 088) sind beide Seiten abgesichert (Guertel und Hosentraeger).

### Wie wurde es getestet?

Reines Thread-Zusammenspiel mit Win32, kein isolierbarer Host-Test. Cross-Build. Manuell:
Plugin direkt nach dem Start (Overlay-Start noch verzoegert) mehrfach deaktivieren/aktivieren.

### Lerneffekt

„Pruefen, dann handeln“ ist bei zwei Threads nie atomar. Absichern mit dem
Ankuendigen-und-Nachschauen-Muster auf beiden Seiten.

---

## 5. Versions-Tabelle: Loeschen ohne Kette zu zerschneiden (Tombstones)

### Was wurde geaendert?

Neu: `src/ts/info/ts3_version_map.h` (reine Tabelle: `vmap_find/get/remove/clear`).
`ts3_plugin_version.c` benutzt sie; dort gibt es keine eigene Sonde/Loeschlogik mehr.

### Wie war es vorher?

Die Tabelle ist eine Hash-Tabelle mit **linearer Sondierung**: Landen zwei Client-IDs im
selben Fach, kommt die zweite in das naechste freie. Beim Suchen laeuft man vom
„Heimat-Fach“ weiter, bis man ein **leeres** Fach sieht. Der alte Code loeschte per
`memset` — das Fach wurde wieder „leer“, mitten in der Kette:

```
Fach:     1        2        3        4
Vorher:  [A]      [B]      [C]      [D]      <- A,B,C,D alle Heimat Fach 1
B loeschen (alt): [A]     [leer]    [C]      [D]
Suche C:  Fach 1 (A) -> Fach 2 leer -> "nicht vorhanden"   <- FALSCH
```

Folgen: Clients zeigten „Kein Plugin erkannt“, obwohl sie es hatten, und beim naechsten
CEVER wurden sie ein **zweites Mal** eingetragen. Dieselbe Falle steckte in der
„veraltete Eintraege entfernen“-Schleife.

### Warum ist die neue Loesung besser/stabiler?

Jedes Fach hat einen von drei Zustaenden: `EMPTY` (nie benutzt), `USED`, `TOMB` (geloescht).
Suchen **laeuft ueber Tombstones hinweg** und endet erst bei `EMPTY`. Einfuegen nutzt das
erste `TOMB`/`EMPTY` entlang des Suchwegs (der Schluessel ist nach der fehlgeschlagenen
Suche ja nachweislich nicht vorhanden). Die Purge-Schleife loescht in place per
`vmap_remove_entry` — gefahrlos waehrend sie das Array nach Index durchlaeuft. Ansammelnde
Tombstones kosten nur ein paar Vergleiche (512 Faecher), und `ts3_version_reset()` (siehe 6)
leert die Tabelle bei jeder Verbindung ohnehin vollstaendig.

```
B loeschen (neu): [A]     [TOMB]    [C]      [D]
Suche C:  Fach 1 (A) -> Fach 2 (TOMB, weiter) -> Fach 3 (C) gefunden
```

### Wie wurde es getestet?

`tests/adapter_lifecycle_test.c` (neu), u. a.: Kette mit 4 kollidierenden IDs, Mitte
loeschen → Rest erreichbar, kein Duplikat; Tombstone-Wiederverwendung; Purge in place;
volle Tabelle (Einfuegen scheitert sauber, Suche terminiert); **200 000 zufaellige
Operationen gegen ein Referenzmodell**; ein Test, der das alte `memset`-Verhalten
nachstellt und zeigt, dass die Kette dann wirklich reisst (Regressionswaechter). Mit
einer Mutation (Tombstone → `EMPTY`) schlagen 11 Pruefungen fehl — der Test erkennt den
Fehler also.

### Lerneffekt

In Hash-Tabellen mit offener Adressierung darf man nie „einfach leeren“ — ein Loeschen
braucht eine eigene Markierung. Das testet man am besten gegen ein simples Referenzmodell.

---

## 6. `ts3_version_reset()` im Reset pro Verbindung

### Was wurde geaendert?

`ts3_entry.c`, `ts3_reset_connection_state()` ruft zusaetzlich `ts3_version_reset()` auf.

### Wie war es vorher?

Der Reset pro Verbindung (Disconnect, Serverwechsel, Tab-Wechsel) leerte alle Module
**ausser** der Versions-Tabelle (die wurde nur beim Plugin-Shutdown geleert). Nach einem
Reconnect standen alte Versionen und — wichtiger — das `replied`-Flag aus der alten
Sitzung in der Tabelle. Das Flag verhindert, dass wir einem Client ein zweites Mal
antworten; im neuen Server haette er unsere Version also nie bekommen.

### Warum ist die neue Loesung besser/stabiler?

Jede Verbindung startet mit leerer Versions-Tabelle, genau wie bei allen anderen Modulen.
`ts3_version_reset()` ist ein **reiner** Reset (Lock, Tabelle nullen, Flag loeschen) ohne
TS-API und damit im Reset-Pfad unproblematisch. Der Broadcast danach
(`ts3_version_broadcast` im Bootstrap) traegt die eigene Version wieder ein.

### Wie wurde es getestet?

Cross-Build; Tabellenlogik siehe 5. Manuell: mit zwei Clients verbinden, einen
neu verbinden, im Info-Panel des Gegenstuecks erscheint die Version wieder.

### Lerneffekt

Eine „Reset“-Funktion ist nur so gut wie ihre Liste — jedes Modul mit
Verbindungs-Zustand muss darin stehen.

---

## 7. CEVER-Broadcast bei „Plugin aktiviert, waehrend schon verbunden“

### Was wurde geaendert?

`ts3_plugin_version.c`: Flag `g_broadcastRetry`, neue Funktionen
`ts3_version_broadcast_pending()` und `ts3_version_flush_broadcast()`.
`ts3_entry.c`: `ts3plugin_registerPluginID` weckt den Dispatcher, wenn ein Broadcast
vorgemerkt ist; `ts3_pending_work_any()` kennt das Flag; der CEDRAIN-Dispatcher sendet den
Broadcast.

### Wie war es vorher?

TeamSpeak vergibt die Plugin-ID **nach** `ts3plugin_init` (`ts3plugin_registerPluginID`).
Wird das Plugin aktiviert, waehrend der Client schon verbunden ist, sendet `init` den
CEVER-Broadcast — `sendPluginCommand` wird aber abgelehnt, weil noch keine ID existiert.
Der Fehler wurde stillschweigend verschluckt: die anderen Clients erfuhren unsere Version
nie (bis zum naechsten Connect).

### Warum ist die neue Loesung besser/stabiler?

Scheitert der Broadcast **nur** wegen fehlender ID, wird er vorgemerkt. Sobald die ID da
ist, weckt `registerPluginID` den Dispatcher (nur Flag + Wakeup, **keine** TS-API, weil
diese Funktion nicht zwingend auf dem Callback-Thread laeuft). Der Dispatcher sendet dann
auf dem Callback-Thread. Das Flag zaehlt erst als „Arbeit“, wenn die ID existiert — sonst
wuerde sich der Dispatcher in einer Endlosschleife selbst wecken.

```
init (verbunden):   Broadcast scheitert (keine ID) -> Flag gesetzt
registerPluginID:   ID gespeichert, Flag gesetzt?  -> ts3_request_wakeup()
CEDRAIN-Drain:      ts3_version_broadcast_pending()? -> ts3_version_flush_broadcast() -> gesendet
```

### Wie wurde es getestet?

Cross-Build. Manuell: im TS verbunden das Plugin deaktivieren und wieder aktivieren; ein
zweiter Client sieht danach im Info-Panel unsere Version.

### Lerneffekt

Reihenfolge-Annahmen („die ID gibt es ja schon“) gehoeren in Flags mit Nachholen statt in
„einmal versuchen und vergessen“.

---

## 8. Client-Timeout und Kicks

### Was wurde geaendert?

`ts3_entry.c` + `ts3_exports.h`: neue Exports `ts3plugin_onClientMoveTimeoutEvent`,
`ts3plugin_onClientKickFromServerEvent`, `ts3plugin_onClientKickFromChannelEvent`. Alle drei
rufen denselben Handler wie ein normales Move-Ereignis (`ts3_on_client_move`).

### Wie war es vorher?

Nur `onClientMoveEvent` mit `newChannelID == 0` raeumte Spielerdaten auf. Bei einem
**Timeout** oder **Server-Kick** schickt TeamSpeak aber ein anderes Ereignis — der
Client blieb als „Geisterspieler“ in Spielertabelle, CEMODE/CEPING/CEAUTH, Version und
Talk-Status stehen. Ein **Kanal-Kick** (Client landet im Standardkanal) wurde gar nicht als
Wechsel erkannt, auch nicht beim eigenen Client.

### Warum ist die neue Loesung besser/stabiler?

Ein einziger Handler fuer alle Wege: Timeout/Server-Kick = „Client ist weg“
(`newChannelID = 0`, ganzes Aufraeumen), Kanal-Kick = Wechsel in einen anderen Kanal
(`newChannelID != 0`, fuer den eigenen Client inkl. `chan_on_own_move` und Overlay-Sync).
Nur die aktive Verbindung wird beachtet, und der Callback-Thread wird markiert wie bei
allen anderen Ereignissen.

### Wie wurde es getestet?

Cross-Build. Manuell: ein zweiter Client wird per Timeout (Netzwerk trennen), Server-Kick
und Kanal-Kick entfernt; im Debug-Log verschwindet er aus der Spielertabelle.

### Lerneffekt

Wenn eine API denselben Sachverhalt ueber mehrere Ereignisse meldet, alle auf einen
gemeinsamen Handler legen.

---

## 9. Fehlgeschlagene Kanalwechsel sichtbar machen (Return-Codes)

### Was wurde geaendert?

Neu: `src/ts/adapter/ts3_return_code.h` (kleiner Ring gemerkter Return-Codes).
`ts3_adapter.c`: `ts3_request_client_move` erzeugt je Wechsel mit `createReturnCode` einen
Plugin-Return-Code, merkt ihn und uebergibt ihn an `requestClientMove`; neue
`ts3_consume_move_return_code()`. `ts3_entry.c`: neuer Export `ts3plugin_onServerErrorEvent`.

### Wie war es vorher?

`requestClientMove` wurde mit `NULL` als Return-Code aufgerufen. Der Wechsel ist
**asynchron**: das Ergebnis kommt spaeter vom Server. Ohne Return-Code konnte das Plugin
Erfolg und Fehler nicht unterscheiden — ein verweigerter Wechsel (falsches Passwort, Kanal
voll, keine Berechtigung) blieb fuer die Kanalverwaltung unsichtbar.

### Warum ist die neue Loesung besser/stabiler?

`onServerErrorEvent` bekommt den Return-Code zurueck. Gehoert er zu **unserem** Wechsel:

- `error == ERROR_ok` → Rueckgabe **1** („erledigt“, TeamSpeak druckt nichts),
- sonst → `chan_on_move_failed(error)` (Kanalverwaltung kann reagieren) und Rueckgabe
  **0** — TeamSpeak zeigt den Fehler weiterhin genau wie vorher an.

Jeder andere oder fehlende Code → **0**, also unverändertes Verhalten. Ein lokal
gescheiterter Aufruf vergisst seinen Code sofort (es kommt keine Antwort mehr). Der Ring
hat 8 Plaetze (wegen des In-Flight-Flags in `channel_manage` ist ohnehin nur ein Wechsel
offen) und ueberschreibt notfalls den aeltesten — kein Leck, kein Lock. Er wird bei jeder
Verbindungsaenderung geleert. Ohne registrierte Plugin-ID bleibt der Code `NULL` wie frueher.

### Wie wurde es getestet?

`tests/adapter_lifecycle_test.c`: Code merken/finden/nur einmal verbrauchen, unbekannte /
leere / `NULL`-Codes, zu lange Codes, Ring-Ueberlauf, Clear. Cross-Build. Manuell: Wechsel
in einen Kanal mit falschem Passwort → im Plugin-Log erscheint
`TS-EVT: requestClientMove failed err=...`, TeamSpeak zeigt den Fehler weiterhin.

### Lerneffekt

Bei asynchronen APIs gehoert zu jeder Anfrage ein Kennzeichen (Return-Code), sonst ist die
Antwort nicht zuzuordnen.

---

## 10. Aenderungen der Kanalliste bemerken

### Was wurde geaendert?

`ts3_entry.c` + `ts3_exports.h`: neue Exports `ts3plugin_onNewChannelEvent`,
`ts3plugin_onNewChannelCreatedEvent`, `ts3plugin_onDelChannelEvent`,
`ts3plugin_onChannelMoveEvent`; zusaetzlich ruft der bestehende
`ts3plugin_onUpdateChannelEditedEvent` (nach seiner bisherigen Arbeit) dieselbe Meldung.
Jedes Ereignis ruft `chan_on_channel_list_changed(channelID)` und
`server_profile_on_channel_list_changed(channelID)`; hat die Kanalverwaltung danach Arbeit
offen, wird ein Wakeup angefordert.

### Wie war es vorher?

Hub-, Ingame- und Root-Kanal-IDs werden einmal aufgeloest und gecacht. Wurde ein Kanal
umbenannt, geloescht, neu angelegt oder verschoben, merkte das Plugin nichts — die
gecachten IDs zeigten ins Leere, bis zum naechsten Reconnect.

### Warum ist die neue Loesung besser/stabiler?

Die beiden Module werden informiert und loesen beim naechsten Tick neu auf (sie behalten
die alten IDs, bis die neuen gefunden sind — kein „Loch“ dazwischen). Nur die aktive
Verbindung zaehlt; Bursts beim Verbindungsaufbau (ein Ereignis je Kanal) schluckt die
Verbindungs-ID-Pruefung bzw. das Zusammenfassen in den Modulen.

### Wie wurde es getestet?

Cross-Build. Manuell: als Admin den Ingame-Kanal umbenennen und zurueckbenennen; der
Auto-Wechsel Hub ↔ Ingame funktioniert danach weiter.

### Lerneffekt

Jeder Cache braucht ein Invalidierungs-Ereignis. Wer Daten merkt, muss wissen, wann sie
veralten.

---

## 11. `ts3_get_channel_parent`

`ts3_adapter.c/.h`: `int ts3_get_channel_parent(uint64 channelID, uint64* outParent)`
(Wrapper um `getParentChannelOfChannel`). Rueckgabe 1 bei Erfolg, `*outParent == 0` heisst
„oberste Ebene“; 0 bei Fehler. Wie alle Getter nur auf dem Callback-Thread (Guard loggt
Fehlbenutzung), prueft Verbindung und Funktionszeiger. Gebraucht vom Server-Profil, um den
Root-Kanal als Kanal ohne Eltern zu erkennen.

**Test:** Cross-Build (Win32-Wrapper ohne Logik; die Auswertung steht im aufrufenden Modul).

---

## 12. UI-Sync ueber CEDRAIN + Kommentar korrigiert

### Was wurde geaendert?

`ts3_entry.c`: `plugin_ui_has_pending_sync()` steht in `ts3_pending_work_any()`;
`plugin_ui_flush_pending_sync()` laeuft **als Erstes** im CEDRAIN-Dispatcher (vor Queue,
Audio-Recompute usw.). `plugin.h`: Kommentar zu den Kanal-Spiegeln korrigiert.

### Wie war es vorher?

`plugin_ui_on_position_tick()` lief auf dem Pos-Watcher-Thread und schrieb dabei die
Spiegel-Variablen (Hub-/Ingame-Kanal, Zonen, Koordinaten). Der Kommentar in `plugin.h`
behauptete weiterhin „einziger Schreiber ist der Callback-Thread“.

### Warum ist die neue Loesung besser/stabiler?

Ausserhalb des Callback-Threads setzt der Tick nur ein Flag (UI-THREADS, Fix 088) und
fordert einen Wakeup an; geschrieben wird ausschliesslich auf dem Callback-Thread. Dafuer
muss der Dispatcher das Flag kennen (sonst bleibt der Sync liegen) und ausfuehren. Er tut
das vor dem Audio-Recompute, weil dieser die gespiegelte Position und die Zone liest.

### Wie wurde es getestet?

Cross-Build; Thread-Zusammenspiel, kein Host-Test.

### Lerneffekt

Eine neue Pending-Quelle wird nur an **einer** Stelle eingetragen (`ts3_pending_work_any`) —
wer sie dort vergisst, hat einen Sync, der nie laeuft.

---

## 13. Self-Test-Kanalliste nur im Debug-Log

### Was wurde geaendert?

`ts3_entry.c`: das Einreihen von `TS3_CMD_LOG_CHANNEL_LIST` bei `CONNECTION_ESTABLISHED`
nur noch `if (log_is_enabled())`. `ts3_adapter.c`: die Ausgaben dieses Kommandos benutzen
`log_debug` statt `log_write`.

### Wie war es vorher?

Bei **jeder** Verbindung schrieb das Plugin als „Phase-3-Selbsttest“ die ersten 16
Kanalnamen des Servers in `plugin.log` — auch in Produktion. Servernamen gehoeren dort
nicht hin, und Nutzer schicken ihre Logdatei bei Support-Anfragen weiter.

### Warum ist die neue Loesung besser/stabiler?

Ohne Debug-Modus entsteht weder das Kommando noch die Log-Ausgabe. Zusaetzlich ist die
Ausgabe selbst `log_debug`, damit auch ein anderer kuenftiger Aufrufer keine Namen im
Normalbetrieb ausgibt.

### Wie wurde es getestet?

Cross-Build. Manuell: Debug aus → keine „channel list“-Zeilen in `plugin.log`; Debug an →
`DBG TS-CMD: channel list (...)`.

### Lerneffekt

Selbsttests und Diagnose gehoeren hinter einen Debug-Schalter, besonders wenn sie
Daten Dritter protokollieren.

---

## 14. Log: Datei offen halten statt pro Zeile oeffnen

### Was wurde geaendert?

`src/core/util/log.c` (und Kommentar in `log.h`): ein dauerhaft offenes Datei-Handle,
geschuetzt vom bestehenden Log-Lock.

### Wie war es vorher?

Fuer **jede** Logzeile: `GetFileAttributesExW` (Groesse) + `CreateFileW` + `WriteFile` +
`CloseHandle` — vier Systemaufrufe unter dem Lock, die den Logger (und jeden Thread, der
auf ihn wartet) bremsen, besonders im Debug-Betrieb mit vielen Zeilen pro Sekunde.

### Warum ist die neue Loesung besser/stabiler?

Pro Zeile nur noch ein `WriteFile`. Alle bisherigen Eigenschaften bleiben:

- **Teilen**: `FILE_SHARE_READ | WRITE | DELETE` (neu: DELETE) — Editor, Kopieren und
  Loeschen der Datei funktionieren weiter; angehaengt wird mit `FILE_APPEND_DATA`
  (atomar, auch mit einer zweiten TS-Instanz).
- **Groessen-Limit** (250 MB, bzw. 1 GB mit Debug): Das Plugin zaehlt die Groesse mit; bei
  Erreichen wird neu geoeffnet mit `CREATE_ALWAYS` (kuerzt die Datei) und dieselbe
  „LOG: cleared (plugin.log exceeded …)“-Zeile geschrieben. Das Limit haengt wie vorher bei
  jeder Zeile vom Debug-Schalter ab.
- **Datei weg?** Etwa einmal pro Sekunde prueft das Plugin per Pfad, ob die Datei noch
  existiert (und liest die echte Groesse vom Handle — das Verzeichnis meldet bei
  offenen Dateien auf NTFS verzoegert). Wurde sie geloescht oder umbenannt, wird das
  Handle geschlossen und die naechste Zeile legt `plugin.log` neu an — wie frueher.
- **Fehler**: Schreibfehler → einmal neu oeffnen und wiederholen, sonst Handle schliessen
  (naechste Zeile beginnt sauber). Kann nicht geoeffnet werden → Zeile wird wie frueher
  verworfen.
- **Format, Debug-Schalter, Log-Ordner-Menue**: unveraendert.
- `log_close()` (letzter Shutdown-Schritt) schliesst das Handle; eine spaetere Zeile
  oeffnet, schreibt und schliesst wieder.

Kleiner Unterschied: Loescht man `plugin.log` waehrend das Plugin laeuft, landen die
Zeilen der naechsten Sekunde noch in der geloeschten Datei, danach in der neuen. Bei
Absturz gehen keine Zeilen verloren (der Windows-Cache gehoert dem Betriebssystem, nicht
dem Prozess).

### Wie wurde es getestet?

Datei-Ein-/Ausgabe ist Win32-spezifisch und auf dem Linux-Host nicht ausfuehrbar (kein
Wine). MinGW-Cross-Build mit `-Wall -Wextra` ohne Warnungen; Code-Review der Pfade
(erste Zeile, Rotation, Schreibfehler, geloeschte Datei, Shutdown). Manueller Windows-Test
siehe unten.

### Lerneffekt

Ein Logger gehoert auf den schnellen Pfad: Handle offen halten, aber **jeden** Fall
bedenken, in dem die Datei unter einem wegverschwindet.

---

## Gesamttest

- Host-Unit-Tests: `wakeup_policy_test` (erweitert), `cmd_queue_test`,
  `adapter_lifecycle_test` (neu), `render_state_test`, `check_layering.sh`.
- Cross-Compile jeder geaenderten `.c`-Datei mit MinGW (`-Wall -Wextra`): ohne Warnungen.
- **Noch nicht** im echten TeamSpeak-Client getestet.

### Manuelle Tests im TS-Client

1. **CEDRAIN (Fix 1)**: Plugin laden, auf einen Server verbinden → in
   `Dokumente\Conan Exiles TeamSpeak plugin\plugin.log` steht pro Verbindung genau eine
   der beiden Zeilen `TS-WAKE: CEDRAIN addressed to own client confirmed` **oder**
   `TS-WAKE: no echo ... falling back`. In beiden Faellen muss Proximity wie vorher
   funktionieren (bei „fallback“ ~2 s Verzoegerung beim Verbinden). Mit zwei Spielern im
   Spiel: Stimmen/Positionen wie bisher. **Bitte die Log-Zeile zurueckmelden** — sie
   zeigt, ob TeamSpeak Selbst-Adressierung zustellt.
2. Einstellungen ueber TS > Einstellungen > Plugins oeffnen, Plugin deaktivieren →
   kein Absturz.
3. Zwei Server-Tabs: zweiten im Hintergrund verbinden → Proximity im aktiven Tab bleibt.
4. Plugin aktivieren, waehrend verbunden: nach kurzer Zeit zeigt das Info-Panel anderer
   Clients „(aktuell)“.
5. Reconnect: Version wird wieder angezeigt.
6. Kick/Timeout/Kanal-Kick eines Mitspielers: er verschwindet aus der Proximity.
7. Wechsel in Kanal mit falschem Passwort: Log-Zeile `requestClientMove failed`, TS zeigt
   weiter die Fehlermeldung.
8. Ingame-Kanal umbenennen und zurueck: Auto-Wechsel funktioniert weiter.
9. Debug aus/an: Kanallisten-Zeilen nur mit Debug.
10. Windows: Mit laufendem TS `plugin.log` im Editor oeffnen (Plugin schreibt weiter);
    Datei kopieren (geht); Datei loeschen → binnen ~1 s entsteht eine neue.

### Risiken

- Fix 1: unverifiziertes TS-Verhalten — durch Selbstpruefung + Rueckfall abgesichert
  (schlimmstenfalls ~2 s ohne Drain pro Verbindung, danach alter Zustand).
- Fix 14: einziges Win32-I/O ohne Laufzeittest auf diesem Rechner; Rueckfall bei jedem
  Fehler = neu oeffnen.
