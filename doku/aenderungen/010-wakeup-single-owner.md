# 010 — Wakeup-Neubau: ein einziger Besitzer des Off-Callback-Sends

Arbeitspaket aus Phase **V8.4** (Thread-Kern II), Teil 4a. Betrifft
`src/ts/adapter/ts3_adapter.c` / `.h`, den neuen reinen Header
`src/core/util/wakeup_policy.h` und einen neuen Host-Unit-Test. Kein Feature,
keine hoerbare Verhaltensaenderung — nur die Beseitigung der letzten
dokumentierten Thread-Vertrag-Ausnahme aus V7.

---

## Was wurde geaendert?

- `ts3_request_wakeup()` und `ts3_request_wakeup_urgent()` rufen **keine
  TS-API mehr**. Sie setzen nur noch ein Interlocked-Flag (`g_wakeupPending`,
  bei „urgent“ zusaetzlich `g_wakeupUrgent`) und wecken per `SetEvent` einen
  Thread. Das ist **reines Win32** und damit von **jedem** Thread erlaubt —
  auch vom PCM-Audio-Thread.
- Neuer, **einziger** Wakeup-Thread (`ts3_wakeup_thread` in `ts3_adapter.c`):
  wartet auf das Event, entprellt/drosselt die Anforderung und ruft — als
  einziger Ort ausserhalb des Callback-Threads — `sendPluginCommand("CEDRAIN:1")`.
- Start: in `ts3_adapter_set_functions` (frueheste Stelle, an der der Send
  ueberhaupt sinnvoll ist), einmalig (`ts3_wakeup_start`, idempotent).
- Shutdown: `ts3_adapter_shutdown` ruft `ts3_wakeup_stop` **als Erstes** —
  Stop-Flag setzen, Event wecken, Thread **joinen** (`WaitForSingleObject` auf
  das Thread-Handle) — **bevor** irgendetwas abgebaut wird, das der Thread liest
  (`g_connected`, `g_activeConnection`, `g_pluginID`, `g_ts3`). Danach ist
  `ts3_request_wakeup` ein No-Op (`g_wakeupStop` gesetzt).
- Neue reine Hilfsfunktion `wakeup_should_send(nowMs, lastMs, urgent, rateMs)`
  in `src/core/util/wakeup_policy.h` (die ganze Drossel-/Urgent-Entscheidung) —
  host-unit-testbar ohne Win32/TS.

**Bewusst NICHT geaendert:** die 30 ms Drossel (`PLUGIN_POLL_INTERVAL_MS`), die
Urgent-Semantik (umgeht die Drossel), das Verhalten „Wakeup verworfen, solange
nicht verbunden“ und die Signatur beider Funktionen. Die Aufrufer bleiben unberührt.

---

## Wie war es vorher (V7)?

`ts3_request_wakeup()` rief `sendPluginCommand("CEDRAIN:1")` **direkt** auf —
egal von welchem Thread (Pos-Watcher, UI, Settings, audio-nahe Pfade). Das war
im Audit als **Kernproblem 1** markiert: Der komplette Thread-Vertrag („nur der
Callback-Thread ruft die TS-API“) hatte hier eine Ausnahme. Die Stabilitaet
haengte allein daran, dass genau diese eine SDK-Funktion aus beliebigen Threads
thread-sicher ist — dieselbe Klasse Crash, die das alte Mumble-Plugin bei 20+
Spielern geplagt hat (`RtlpWaitOnCriticalSection`).

Die Drossel (max. ein Roundtrip pro 30 ms) und der Urgent-Bypass lagen direkt in
`ts3_send_wakeup`, ausgefuehrt vom jeweils aufrufenden Fremd-Thread.

---

## Warum ist die neue Loesung besser (stabiler)?

- **Ein Besitzer pro API-Zugriff:** Der Off-Callback-Send gehoert genau **einem**
  Thread. Kein Fremd-Thread ruft die TS-API mehr an — die letzte V7-Ausnahme ist
  weg.
- **Sicher vom Audio-Thread:** Weil `ts3_request_wakeup` jetzt nur Flag + Event
  ist, darf ihn auch der PCM-Thread aufrufen (frueher tabu). Das vereinfacht die
  Audio-Pfade.
- **Sauberer Shutdown:** Der Thread wird **gejoint**, bevor der Zustand
  verschwindet, den er liest. Kein „Thread setzt nach dem Teardown noch einen
  API-Call ab“ (Use-after-free-Klasse). Danach sind Wakeups garantiert stumm.
- **Verhalten identisch:** Gleiche 30 ms Drossel, gleicher Urgent-Bypass. Ein
  verworfener Nicht-Urgent-Wakeup wird — wie bisher — durch die naechste
  Anforderung (Pos-Tick / erneutes Signal) nachgeholt.

**Ehrliche Einordnung:** Volle Ein-Thread-Reinheit ist **nicht moeglich**. Das
TS-SDK bietet keinen Timer-/Wake-Callback, den wir auf dem Callback-Thread
ausfuehren koennten. Deshalb beruehren jetzt **genau zwei** Threads die TS-API:
der **Callback-Thread** (alles) und der **Wakeup-Thread** (genau eine Funktion,
`sendPluginCommand`). Das ist die minimal moegliche Angriffsflaeche.

---

## Wie funktioniert es (anfaengertauglich)?

Frueher durfte **jeder** ins „TS-Telefon“ greifen und den Server anrufen
(„bitte abarbeiten!“). Viele Haende an einem empfindlichen Geraet = Risiko.

Ab jetzt gibt es **einen einzigen Telefonisten** (den Wakeup-Thread). Wer Arbeit
hat, wirft nur einen Zettel in den Briefkasten (`Flag setzen`) und klingelt
(`SetEvent`). Der Telefonist wacht auf, schaut, ob er in den letzten 30 ms schon
angerufen hat (Drossel), und ruft ggf. **einmal** an. „Dringend“ (urgent)
überspringt die 30-ms-Wartezeit.

```
  Producer-Threads                Event         Wakeup-Thread (EINZIGER Sender)
  ────────────────                ─────         ───────────────────────────────
  Pos-Watcher  ─┐
  UI / Settings ─┤ Flag setzen +  ┌─────┐  wake  WaitForSingleObject(event)
  CEPOS/Audio  ─┼───────────────► │ SET │ ─────► Flag lesen+loeschen (coalesce)
  PCM-Thread   ─┘   SetEvent       └─────┘        urgent? / seit <30ms gesendet?
                                                    │ wakeup_should_send()
                                                    ▼ (ja senden)
                                        g_ts3.sendPluginCommand("CEDRAIN:1")
                                                    │
                                                    ▼
                              TS-Server ──► onPluginCommandEvent (Callback-Thread)
                                                    │
                                                    ▼
                                        CEDRAIN-Drain (Queue/CEPOS/3D/Unmute/…)

  Viele Anforderungen im selben 30-ms-Fenster fallen zu EINEM Send zusammen
  (ein Flag, ein Event). "urgent" umgeht das Fenster.
```

**Shutdown-Reihenfolge (wichtig):**

```
ts3_adapter_shutdown()
  └─ ts3_wakeup_stop()          # 1) Stop-Flag, Event wecken, Thread JOINEN
       (WaitForSingleObject auf das Thread-Handle)
  └─ g_connected = 0            # 2) erst DANACH Zustand abbauen,
  └─ g_activeConnection = 0     #    den der Thread gelesen hat
  └─ Queue leeren
```

---

## Wie wurde es getestet?

- **Neuer Host-Unit-Test** `tests/wakeup_policy_test.c`: prueft
  `wakeup_should_send` — urgent umgeht die Drossel immer; nicht-urgent sendet
  erst wieder nach ≥30 ms (Grenzfaelle 29/30/100 ms); ein Burst von 12
  Anforderungen ueber 55 ms faellt zu genau **2** Sends zusammen (Coalescing).
- `bash tests/run_tests.sh` → **alle 6 Suites gruen** (hub_parser,
  proximity_math, zone_resolve, player_table, render_state, **wakeup_policy**),
  251 Checks.
- `bash build/build_mingw.sh` → `bin/mingw/conan_exiles.dll` linkt OK.
- Per Code-Suche verifiziert: `sendPluginCommand` wird ausserhalb des
  Callback-Threads **nur** noch im Wakeup-Thread gerufen; `ts3_request_wakeup*`
  enthalten keine TS-API mehr.

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

1. **Normalbetrieb / Bewegung:** Zwei Spieler in Hoerweite, einer laeuft. Position
   und Lautstaerke aktualisieren sich fluessig (Wakeup → CEDRAIN funktioniert).
2. **Lasttest (Ziel dieser Phase):** Viele Spieler / CEPOS-Flut. Erwartung: kein
   Callback-Spike, kein Crash — der eine Wakeup-Thread drosselt sauber.
3. **Chat/Voice-Mode-Feedback:** Voice-Mode per Hotkey umschalten. Die
   Chat-Meldung erscheint sofort (urgent-Wakeup umgeht die Drossel).
4. **Disconnect / Tab-Wechsel / TS beenden:** sauberes Beenden, kein Haenger,
   kein Crash (Wakeup-Thread wird vor dem Teardown gejoint).

## Lerneffekt

- **Producer/Consumer statt geteilter API:** Ein „bitte arbeite“-Signal
  (Flag + Event) ist immer thread-sicher; die eigentliche empfindliche Aktion
  gehoert einem einzigen Consumer-Thread.
- **Join vor Teardown:** Ein Thread, der fremden Zustand liest, muss **gejoint**
  sein, bevor dieser Zustand verschwindet — sonst droht Use-after-free.
- **Ehrlich dokumentieren, was nicht geht:** Ohne SDK-Timer-Callback bleibt ein
  zweiter API-Thread unvermeidbar. Statt es zu verstecken, benennen wir die
  minimale Restflaeche (genau eine Funktion) klar.
- **Reine Entscheidung herausziehen:** Die Drossel-Logik als
  `wakeup_should_send(...)` im Header ist ohne Win32 testbar — Grenzfaelle
  (29/30 ms, urgent) sind so maschinell abgesichert.
