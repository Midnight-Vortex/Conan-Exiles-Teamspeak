# 088 — Review-Fixes UI-Threads: ein Schreiber fuer den HUD-Zustand, joinbare Threads, sichere Chat-Queue

**Phase:** Review-Fixes (Paket "UI-THREADS") · Betrifft nur die UI-Schicht (`src/ui/`).

Ein vollstaendiger Code-Review hat in den UI-Threads mehrere Race-Conditions und
Lifecycle-Luecken gefunden (acht Befunde). Dieses Paket behebt sie, ohne
sichtbares Verhalten zu aendern: HUD, F10-Dialog, Chat-Meldungen und
Shutdown arbeiten fuer den Spieler wie vorher — nur eben ohne die seltenen
Ausreisser (leerer HUD-Text, haengende Threads beim Beenden, verlorene
Chat-Zeilen).

## Ueberblick: welche Dateien wurden geaendert?

| Datei | Befund |
|---|---|
| `src/ui/plugin_ui_compat.c` / `.h` | 1 (Live-State-Sync), 5 (Overlay Start/Stop), 6 (Fensterklasse), 8a (Warnung) |
| `src/ui/input/key_watcher.c` | 2 (Key-Monitor Start/Stop), 3 (Dialog-Guard, Dialog-Thread) |
| `src/ui/util/ui_ts_chat_queue.c` | 4 (Chat-Queue) |
| `src/ui/overlay/voice_overlay.c` / `.h` | 5 (Monitor-Thread-Flag), 6 (Fensterklasse), 7 (Thread-Vertrag), 8b (Warnung) |
| **neu** `src/ui/util/ui_spin_lock.h` | kleines Lock ohne Init/Teardown (fuer 2, 3, 4) |
| **neu** `src/ui/util/ui_chat_ring.h` | der Chat-Ring (rein, host-testbar) (fuer 4) |
| **neu** `src/ui/util/ui_worker_slot.h` | Start/Stop-Uebergabe eines Thread-Handles (fuer 5) |
| **neu** `tests/ui_chat_ring_test.c`, `tests/ui_worker_slot_test.c` | Host-Unit-Tests mit pthreads |

Begriffe, kurz erklaert:

- **Callback-Thread:** der eine Thread, auf dem TeamSpeak unsere Plugin-Funktionen
  aufruft. Nur er darf die TS-API benutzen (Thread-Vertrag, `doku/01-architektur-v8.md`).
- **Race (Wettlauf):** zwei Threads greifen ohne Absprache auf dasselbe zu; das
  Ergebnis haengt vom Zufall des Timings ab.
- **Join:** auf das Ende eines Threads warten (`WaitForSingleObject` auf sein
  Handle). Erst danach darf man seinen Zustand abbauen.
- **CEDRAIN:** unser Dispatcher auf dem Callback-Thread, der die angeforderte
  Arbeit abarbeitet (`doku/01-architektur-v8.md`).

---

## 1. Live-State-Sync nur noch auf dem Callback-Thread (C6)

### Was wurde geaendert?

`src/ui/plugin_ui_compat.c`: `plugin_ui_sync_live_state()` laeuft jetzt nur noch
auf dem Callback-Thread. Von jedem anderen Thread aus setzt sie lediglich ein
Flag (`g_pendingSync`) und fordert einen Wakeup an. Zwei neue Funktionen
(`src/ui/plugin_ui_compat.h`):

- `int plugin_ui_has_pending_sync(void)` — "wartet ein Sync?" (fuer `ts3_pending_work_any()`)
- `void plugin_ui_flush_pending_sync(void)` — fuehrt den Sync aus (vom CEDRAIN-Dispatcher)

Ausserdem: das falsche Kommentar-Versprechen in `plugin_ui_sync_live_state`
("refreshed on the callback thread") stimmt jetzt wirklich, und `races[i]`
wird in einer lokalen Variable gebaut und mit einer Struct-Kopie veroeffentlicht.

### Wie war es vorher?

`plugin_ui_on_position_tick()` wird vom **Pos-Watcher-Thread** und vom
**HTTP-Listener-Thread** aufgerufen (alle ~30 ms) und fuehrte dort direkt
`plugin_ui_sync_live_state()` aus. Die Funktion baut aber gemeinsam genutzte
Daten neu auf: `zones[]`, `races[]` (inklusive `memset` und Neubefuellung),
`zoneCount`, `raceCount`, `currentZoneIndex`, `currentPlayerRaceIndex` und die
`hub*`-Werte. Gleichzeitig schrieb der Callback-Thread dieselben Daten
(`plugin_ui_on_hub_profile_updated`, Bootstrap), und Overlay-Thread und
F10-Dialog **lasen** sie. Drei Schreiber, mehrere Leser, kein Lock:
gelegentlich zerrissener oder leerer HUD-Text (z. B. "Rasse: " ohne Namen,
weil das `memset` den Namen gerade geloescht hatte). Dazu kamen pro Tick zwei
grosse Profil-Kopien (je ~8 KB) auf den Fremd-Threads.

### Warum ist die neue Loesung besser?

Es gibt jetzt **genau einen Schreiber** (Callback-Thread) — das ist das
Besitzer-Prinzip aus der Architektur. Wo es keinen zweiten Schreiber gibt,
braucht man kein Lock, und der Zustand kann nicht mehr von zwei Threads
gleichzeitig umgebaut werden. Das Flag ist **koaleszierend** (Kanal A der
Steuerebene): 100 Ticks, bevor der Dispatcher laeuft, ergeben nur **einen**
Sync. Die `races[i]`-Aenderung verkleinert das verbleibende Lese-Fenster des
Overlays auf eine einzige Struct-Kopie statt "erst loeschen, dann fuellen".

### Wie funktioniert es jetzt?

```
Pos-Watcher / HTTP-Thread              Callback-Thread (CEDRAIN)
  plugin_ui_on_position_tick()
    plugin_ui_sync_live_state()
      -> nicht Callback-Thread?
         g_pendingSync = 1  (0->1: ts3_request_wakeup())
    localVoiceData.voiceDistance = ...
    updateVoiceOverlay()  (nur PostMessage, wie bisher)
                                         ts3_pending_work_any()  <- has_pending_sync
                                         plugin_ui_flush_pending_sync()
                                           g_pendingSync = 0
                                           Sync (zones/races/hub*...)  <- einziger Schreiber
                                           updateVoiceOverlay()  (HUD zeigt neue Zone)
```

Auf dem Callback-Thread (Bootstrap, Kanalwechsel, Hub-Profil) laeuft der Sync
weiter **sofort und direkt** — und loescht dabei das Flag, denn er hat den
aktuellen Stand ja gerade gelesen. Das Flag wird vor dem Lesen geloescht: ein
Tick, der waehrenddessen kommt, setzt es neu, im schlimmsten Fall folgt ein
zusaetzlicher (idempotenter) Sync. Das Overlay-Repaint-Verhalten
(`updateVoiceOverlay`, 400-ms-Drossel) ist **unveraendert**.

**Wichtig fuer den ADAPTER (Verdrahtung):** `plugin_ui_has_pending_sync()` muss in
`ts3_pending_work_any()`, und `plugin_ui_flush_pending_sync()` gehoert in den
CEDRAIN-Dispatcher — am besten **vor** `ts3_audio_flush_recomputes()`, weil der
Recompute die gespiegelte Position (`axe_x/y/z`) und `currentZoneIndex` benutzt.
Ohne diese Verdrahtung wuerde der Sync nie laufen.

### Wie wurde es getestet?

Reines Thread-Zusammenspiel mit Win32 (Callback-Thread-Erkennung, Wakeup) —
kein isolierbarer Host-Unit-Test. Cross-Compile (MinGW, `-Wall -Wextra`) ohne
Warnungen. Manueller Test im TS-Client siehe unten.

### Lerneffekt

Gemeinsame Daten sollen genau einen Schreiber haben. Wenn viele Threads
"aktualisieren" wollen, sollen sie nur **anfordern** (Flag) — die Arbeit macht
der Besitzer.

---

## 2. Key-Monitor: Start/Stop sicher gegeneinander

### Was wurde geaendert?

`src/ui/input/key_watcher.c`: `startKeyMonitorThread()` / `stopKeyMonitorThread()`
laufen unter einem kleinen Lock (`g_keyMonitorLock`, `ui_spin_lock.h`).
`startKeyMonitorThread()` startet nach Shutdown-Beginn (`pluginShuttingDown`)
nichts mehr.

### Wie war es vorher?

Der Handle `keyMonitorThread` ist eine globale Variable ohne Schutz. Rief ein
Thread `stop` und ein anderer gleichzeitig `start` (z. B. Shutdown und ein
Einstellungsdialog, der die Tastenueberwachung neu startet), konnte:

- derselbe Handle zweimal geschlossen werden (`CloseHandle` auf einen
  recycelten Handle = Zugriff auf einen fremden Thread/Datei),
- ein neuer Thread starten, waehrend der alte noch endet — und dessen
  Abschluss-Schreibzugriff auf das Laufflag den neuen Thread sofort wieder
  stoppte,
- nach dem Shutdown-Join noch ein Thread gestartet werden, den niemand mehr
  joint (er laeuft dann beim Entladen der DLL noch).

### Warum ist es besser?

`stop` **joint unter dem Lock**: Wer `stop` zurueckkehren sieht, weiss, dass
der Thread wirklich weg ist; ein gleichzeitiges `start` wartet, bis das erledigt
ist. Der Key-Thread selbst nimmt dieses Lock nie, daher gibt es keinen
Zyklus (kein Deadlock). Nach `pluginShuttingDown` startet `start` nicht mehr —
das letzte Wort hat immer der Shutdown.

### Wie funktioniert es jetzt?

```
start:  Lock -> Shutdown? raus | schon laufend? raus | alter, schon beendeter Handle? joinen
             -> Laufflag=1 -> _beginthreadex -> Handle merken -> Unlock
stop :  Lock -> Laufflag=0 -> Join (5 s, dann unbegrenzt) -> CloseHandle -> Handle=NULL -> Unlock
```

### Wie wurde es getestet?

Lifecycle/Win32 — kein Host-Test. Cross-Compile ohne Warnungen.

### Lerneffekt

Ein Handle, den mehrere Threads aendern, braucht einen Besitzer-Lock. "Stop"
muss bedeuten "ist weg", nicht "habe es angestossen".

---

## 3. Settings-Dialog: eine Stelle gibt den Dialog-Guard frei (C7)

### Was wurde geaendert?

`src/ui/input/key_watcher.c`:

- `settings_dialog_thread` ist die **einzige** Stelle, die den Guard
  (`config_dialog_close()`) freigibt — nachdem `showConfigInterface()` zurueckkehrte.
  (Die zusaetzliche Freigabe in `showConfigInterface` entfernt das Paket UI-DIALOGS.)
- `settings_dialog_open_async()` darf jetzt von mehreren Threads aus aufgerufen
  werden — Hotkey-Thread, TS-Menue/Configure — jeweils **nachdem** der Aufrufer
  `config_dialog_try_open()` gewonnen hat. Der Thread-Handle liegt unter
  `g_settingsDialogLock`; bei Shutdown wird nichts mehr gestartet, und der
  Guard wird dann zurueckgegeben.
- `settings_dialog_shutdown()` schliesst (WM_CLOSE) und joint einen Dialog,
  egal wer ihn gestartet hat.

### Wie war es vorher?

Der Guard wurde zweimal freigegeben: einmal innerhalb von `showConfigInterface`
(sobald das Fenster zu war) und nochmal am Thread-Ende. In der Luecke dazwischen
konnte ein neuer F10-Druck den Guard gewinnen und einen zweiten Dialog-Thread
starten — dessen Guard dann vom Ende des ersten Threads **wieder geloescht**
wurde. Folge: Guard "frei" obwohl ein Dialog offen ist, also ein zweiter Dialog
moeglich. Der Handle `g_settingsDialogThread` wurde ausserdem ohne Schutz von
mehreren Threads geschrieben und gelesen.

### Warum ist es besser?

Ein Guard, der von genau einer Stelle freigegeben wird, kann nicht "doppelt
freigegeben" werden. Der Lock um den Handle verhindert, dass `open_async` und
`shutdown` gleichzeitig denselben Handle joinen oder ueberschreiben.

### Wie funktioniert es jetzt?

```
F10 / Menue:  if (config_dialog_try_open())        <- Guard gewonnen
                  settings_dialog_open_async()     <- Lock, Shutdown? Guard zurueck | alten Thread aufraeumen
                                                      | _beginthreadex | Handle ablegen
Dialog-Thread: showConfigInterface(); config_dialog_close();   <- EINZIGE Freigabe
Shutdown:      Lock -> Handle nehmen -> Unlock; WM_CLOSE (wiederholt) + Join; CloseHandle
```

### Wie wurde es getestet?

Lifecycle/Win32 — kein Host-Test. Cross-Compile ohne Warnungen.

### Lerneffekt

Ressourcen-Besitz klar machen: wer etwas nimmt, gibt es auch wieder her — an
genau einer Stelle.

---

## 4. Chat-Queue: Mehrere Erzeuger sicher (Multi-Producer)

### Was wurde geaendert?

`src/ui/util/ui_ts_chat_queue.c` benutzt jetzt den neuen, reinen Ring
`src/ui/util/ui_chat_ring.h` (mit `ui_spin_lock.h`). Die Funktionen
`ts3_queue_chat_message`, `ts3_plugin_has_pending_chat`,
`ts3_plugin_clear_pending_chat` und `ts3_plugin_flush_pending_chat` behalten
ihre Namen und ihr Verhalten.

### Wie war es vorher?

Die Queue war fuer **einen** Erzeuger gebaut: Tail lesen, Slot beschreiben,
Tail veroeffentlichen — ohne Absprache. Erzeuger sind aber mehrere Threads
(Key-Monitor, Dialog-Thread, Pos-Watcher, ...). Zwei Threads konnten denselben
Slot erwischen (eine Zeile ueberschrieb die andere, oder beide Texte vermischten
sich), und `ts3_plugin_clear_pending_chat` (beim Verbinden) setzte Head/Tail
zurueck, waehrend ein Erzeuger gerade schrieb.

### Warum ist es besser?

Alle Zustandsaenderungen (Head, Tail, Zaehler, Slot-Inhalt) passieren unter
einem winzigen Lock: Index-Rechnung plus eine begrenzte `memcpy` — Mikrosekunden.
Eine volle Queue verwirft weiterhin die **neue** Zeile (und meldet das) —
nichts wird ueberschrieben. Der Verbraucher (Callback-Thread) kopiert **eine**
Zeile unter dem Lock heraus, gibt das Lock frei und ruft **erst dann** die
TS-API (`ts3_adapter_print_chat_force`). Es wird also nie die TS-API betreten,
waehrend ein Lock gehalten wird.

### Wie funktioniert es jetzt?

```
Key-Thread  \
Dialog-Thread-> ui_chat_ring_push()  (Lock: Slot belegen, kopieren, Tail++ ; Unlock)
Pos-Thread  /
                                   Callback-Thread (CEDRAIN):
                                     while (ui_chat_ring_pop(&line))   (Lock: Zeile heraus ; Unlock)
                                        ts3_adapter_print_chat_force(line)   <- TS-API ausserhalb des Locks
```

Das Lock ist ein statisch mit 0 initialisiertes Spin-Lock (`ui_spin_lock.h`):
kein Init-Aufruf, kein Abbau, daher kein "Lock geloescht, waehrend benutzt"
beim Entladen. Bei Konkurrenz dreht es kurz, gibt dann die Zeitscheibe ab und
schlaeft zuletzt 1 ms (so kommt auch ein niedriger priorisierter
Lock-Halter voran).

### Wie wurde es getestet?

`tests/ui_chat_ring_test.c` (Host, gcc + pthreads, das **echte** Lock und der
**echte** Ring): Reihenfolge ueber den Ringumbruch, Kuerzung langer Zeilen,
volle Queue, `clear`, danach 4 Erzeuger x 30 000 Zeilen mit parallelem
Verbraucher — geprueft wird: keine zerrissene Zeile, keine Dopplung, pro
Erzeuger richtige Reihenfolge, "angenommen + verworfen = gesendet", und das
Ganze nochmal mit einem Thread, der dauernd `clear()` aufruft. Zur Kontrolle:
ersetzt man das Lock durch ein leeres, schlaegt der Test fehl (er erkennt den
Fehler also wirklich). ThreadSanitizer meldet am Ring nichts.

### Lerneffekt

"Einer schreibt, einer liest" muss man *wissen*, nicht hoffen. Sobald ein
zweiter Erzeuger dazukommt, ist ein Lock um den winzigen kritischen Abschnitt
die einfachste korrekte Loesung — und Fremdcode (TS-API) gehoert nie unter ein Lock.

---

## 5. Overlay: verzoegerter Start gegen Shutdown

### Was wurde geaendert?

`src/ui/plugin_ui_compat.c`: `overlay_start()` / `overlay_stop()` uebergeben den
Handle des Overlay-Monitor-Threads ueber den neuen `UiWorkerSlot`
(`src/ui/util/ui_worker_slot.h`). `src/ui/overlay/voice_overlay.c`: der
Monitor-Thread setzt `overlayThreadRunning` nicht mehr selbst auf TRUE.

### Wie war es vorher?

Der verzoegerte Overlay-Thread (`ts3_entry.c`) prueft kurz vor `overlay_start()`
"laeuft Shutdown?", und startet dann. Zwischen Pruefung und Start kann der
Shutdown aber schon `overlay_stop()` aufrufen. `overlay_start()` legt den
Monitor-Thread (Schleife mit `Sleep(2000)`) an und speichert sein Handle in
`g_overlayMonitorHandle` — ohne Absprache mit `overlay_stop()`. Sah `stop`
das Handle noch als `NULL`, jointe es nichts; der Monitor ueberlebte den
Shutdown, `overlay_finalize` loeschte den `overlayTextLock`, die DLL wurde
entladen — mit einem noch laufenden Thread darin (Absturz). Zusaetzlich
schrieb der Monitor-Thread beim Start `overlayThreadRunning = TRUE` und konnte
damit einen gerade erfolgten Stop wieder aufheben.

### Warum ist es besser?

Egal wie sich Start und Stop verzahnen: **genau eine Seite** besitzt am Ende das
Handle und joint es — nie keine, nie beide. Dazu wird *kein* Lock gehalten,
waehrend das Fenster gebaut wird (sonst wuerde der Callback-Thread auf die
Fenster-Aufrufe des Overlay-Threads warten — das kann bei
Fenster-/Hook-Nachrichten zu einem Deadlock werden).

### Wie funktioniert es jetzt?

Beide Seiten schreiben erst ihre **eigene** Variable und lesen dann die der
anderen — mit vollen Speicher-Barrieren. Dadurch sieht mindestens eine Seite die
andere (so wie zwei Leute, die sich gleichzeitig "ich gehe" und "ich komme"
zurufen und danach noch einmal hinschauen):

```
Overlay-Thread (start)                   Callback-Thread (stop)
 may_start? (kein Stop angefordert)       Stop anfordern  (Flag = 1)
 Fenster + Monitor-Thread bauen            Handle nehmen (atomar tauschen)
 Handle veroeffentlichen (atomar)          -> Handle da?  joinen + schliessen
 Stop inzwischen angefordert?              -> kein Handle: der Start raeumt selbst auf
   -> Handle zurueckholen,
      Monitor selbst joinen
```

`ts3plugin_shutdown` joint danach den Overlay-Thread (der `overlay_start`
ausfuehrt) und ruft erst dann `overlay_finalize()` — daher ist jeder Pfad
fertig, bevor Locks und Fensterklasse abgebaut werden. **Am Shutdown-Ablauf in
`ts3_entry.c` ist dafuer nichts zu aendern.**

### Wie wurde es getestet?

`tests/ui_worker_slot_test.c` (Host, pthreads): das echte Protokoll mit
20 000 Wettlaeufen "Starter gegen Stopper" mit zufaelligem Timing. Geprueft:
jeder erzeugte Worker wird **genau einmal** aufgeraeumt (von Stopper oder
Starter), nie verloren, nie doppelt; der Slot ist am Ende leer. Der Test hat
beide Pfade tatsaechlich getroffen. Zur Kontrolle: mit dem alten Protokoll
("Handle einfach speichern") schlaegt der Test fehl. ThreadSanitizer ist sauber.

### Lerneffekt

Beim Hand-over zwischen zwei Threads genuegt "ich pruefe zuerst" nie — man
braucht ein Protokoll, bei dem beide Seiten den jeweils anderen sicher sehen.

---

## 6. Fensterklasse des Overlays wird abgemeldet

### Was wurde geaendert?

`src/ui/overlay/voice_overlay.c`: die Klasse `VoiceOverlayClass` hat einen
festen Namen (`g_overlayClassName`) und eine neue Funktion
`voice_overlay_unregister_class()` (in `voice_overlay.h` deklariert).
`overlay_finalize()` in `plugin_ui_compat.c` ruft sie auf — **nach** dem Join
des Overlay-Threads und der Fenster-Zerstoerung. Die Reihenfolge
"vor dem Registrieren erst abmelden" in `createVoiceOverlay` bleibt.

### Wie war es vorher?

Die Klasse wurde mit dem `HINSTANCE` der Host-EXE (`GetModuleHandleW(NULL)`)
registriert und nie abgemeldet. Sie ueberlebt damit das Entladen der DLL — mit
einem Zeiger auf die `WndProc` im entladenen Code.

### Warum ist es besser?

Nach dem Entladen bleibt kein Eintrag mehr, der in unseren Code zeigt. Das
Abmelden ist ausserdem harmlos, wenn die Klasse nie registriert wurde.

### Wie funktioniert es jetzt?

`ts3plugin_shutdown`: Overlay-Thread joinen (zerstoert sein Fenster selbst) →
`overlay_finalize()`: Fallback-Aufraeumen → `UnregisterClassW` → Text-Lock loeschen.

### Wie wurde es getestet?

Win32 — kein Host-Test. Cross-Compile ohne Warnungen.

### Lerneffekt

Alles, was man bei Windows registriert (Klassen, Hooks, Timer), muss in der
umgekehrten Reihenfolge wieder abgemeldet werden, bevor der Code verschwindet.

---

## 7. `voice_overlay.h`: der Thread-Vertrag stimmt wieder

### Was wurde geaendert?

Nur der Kommentar in `src/ui/overlay/voice_overlay.h`.

### Wie war es vorher?

Der Kopf-Kommentar beschrieb Dinge, die es nicht gibt oder die falsch sind: einen
250-ms-Timer (es gibt keinen — das Zeichnen ist ereignisgesteuert),
`overlay_start` laufe "auf dem TS-Callback-Thread" (es laeuft auf dem
verzoegerten Overlay-Thread), "alles andere intern im Overlay-Thread"
(tatsaechlich rufen Watcher, Key-Monitor, Dialog und Callback-Thread
`updateVoiceOverlay` und die `voice_overlay_refresh_*`-Funktionen auf), und
`overlay_stop` blockiere, bis der UI-Thread beendet ist (es joint nur den
Monitor und sendet WM_QUIT).

### Warum ist es besser?

Ein Thread-Vertrag, der falsch ist, schadet mehr als keiner: Wer sich
darauf verlaesst, schreibt den naechsten Race. Der neue Text sagt, wer
welche Funktion aufruft, und dass fremde Threads nur per `PostMessage` mit dem
Fenster reden.

### Wie funktioniert es / wie getestet?

Reiner Kommentar (Compile-Check genuegt).

### Lerneffekt

Kommentare zu Threads sind Teil der Schnittstelle — bei jeder Aenderung des
Ablaufs mitpflegen.

---

## 8. Compiler-Warnungen

- `plugin_ui_compat.c`: `compat_mumble_log` lieferte `mumble_error_t`, der
  Funktionszeiger `mumbleAPI.log` (in `src/mumble_compat.h`) liefert aber `void`.
  Die Funktion gibt jetzt ebenfalls `void` zurueck, die Deklaration bleibt
  unveraendert; kein Aufrufer wertet den Rueckgabewert aus.
- `voice_overlay.c`: Vergleich `VoiceMode` (vorzeichenlos) gegen `int`
  (`g_overlayLastMode`, startet bei -1) — jetzt mit explizitem `(int)`-Cast,
  Verhalten gleich.

Warum besser: ein Typ-Mismatch bei Funktionszeigern ist undefiniertes Verhalten
nach C-Standard (auch wenn es hier zufaellig funktioniert), und das Projekt
baut mit Warnstufe 4 + "Warnungen als Fehler".

---

## Test-Uebersicht

- Cross-Compile (MinGW, `-Wall -Wextra`) aller geaenderten `.c` ohne Warnungen.
- Neue Host-Tests: `tests/ui_chat_ring_test.c` und `tests/ui_worker_slot_test.c`
  (Eintrag in `tests/run_tests.sh` durch den Parent).
- ThreadSanitizer-Lauf beider Tests ohne Befund (lokal, nicht Teil der Suite).

**Manuelle TS-Client-Tests (Pflicht vor Abnahme):**

1. **HUD folgt der Position:** Im Spiel ueber eine Zonengrenze laufen. Erwartung:
   Zonenname im HUD wechselt ohne merkliche Verzoegerung; kein leeres
   "Rasse: ", kein Flackern des Textes.
2. **Rasse im HUD:** Spieler mit Rasse einloggen, Hub-Beschreibung im Root
   aendern (live). Erwartung: Rasse bleibt sichtbar, Text stabil.
3. **Chat-Meldungen:** Schnell nacheinander Stimmmodus-Hotkeys druecken
   (Fluestern/Normal/Rufen), parallel F10 oeffnen/schliessen. Erwartung: alle
   Mode-Meldungen erscheinen im TS-Chat, keine Kauderwelsch-Zeilen.
4. **F10 mehrfach:** F10 schnell mehrfach hintereinander, Dialog schliessen,
   wieder oeffnen, ueber das TS-Plugin-Menue oeffnen. Erwartung: immer genau
   ein Dialog, nie zwei.
5. **Beenden mit offenem F10-Dialog** und **beenden direkt nach dem Verbinden**
   (innerhalb der ersten ~4 s, wenn das Overlay noch startet): TS schliesst
   sauber, kein Crash-Dialog; im Log `SHUTDOWN: done`.
6. **10x Plugin aus-/einschalten** (Einstellungen -> Plugins): Overlay erscheint
   jedes Mal wieder, kein Haenger.

## Lerneffekt (gesamt)

Die meisten dieser Fehler sind "zwei Threads, ein Zustand": Ein Schreiber pro
Datum, Anforderungen als Flag statt direkter Arbeit, und jeder Thread hat einen
klar definierten Anfang **und** Ende (Handle behalten, joinen, erst dann
abbauen). Wo zwei Threads ein Handle uebergeben, braucht es ein kleines,
getestetes Protokoll statt "ich pruefe vorher".
