# 020 — Command-Queue finalisiert: das Zwei-Kanal-Steuerdesign

Abschluss-Arbeitspaket der Phase **V8.4** (Thread-Kern II). Betrifft
`src/ts/adapter/ts3_adapter.h` / `.c`, den neuen reinen Header
`src/ts/adapter/ts3_cmd_ring.h`, einen neuen Host-Unit-Test
`tests/cmd_queue_test.c` (in `tests/run_tests.sh` verdrahtet) sowie die
Plan-/Architektur-Doku. **Kein Feature, keine hoerbare Verhaltensaenderung** —
das Steuerdesign wird explizit gemacht, der Ring wird testbar, die Doku
korrigiert.

---

## Was wurde geaendert?

1. **Das Steuerdesign wird benannt und dokumentiert (Kern-Deliverable).** Ein
   grosser Kommentarblock in `ts3_adapter.h` definiert die **Zwei-Kanal-
   Steuerplane**:
   - **(A) Koaleszierende Pending-Flags**, gebuendelt von `ts3_pending_work_any()`
     (`ts3_entry.c`), in fester Reihenfolge mit Budget gedraint.
   - **(B) Die typisierte Command-Queue** (`Ts3Command`/`Ts3CmdType`) fuer
     **diskrete Einmal-Aktionen**.
   Dazu steht, **wann welcher Kanal** zu benutzen ist (Faustregel: „200×/Tick =
   immer noch einmal tun“ → Flag; „jede Anforderung eine eigene Aktion, nie in
   Paketfrequenz“ → Kommando).

2. **Marker parallel gemacht.** Neben dem bestehenden
   `>>> ADD NEW PENDING SOURCES HERE <<<` (Kanal A, `ts3_entry.c`) gibt es jetzt
   den parallelen `>>> ADD NEW COMMAND TYPES HERE <<<` am `Ts3CmdType`-Enum
   (Kanal B, `ts3_adapter.h`). Beide Erweiterungspunkte sind auffindbar.

3. **Reiner Ring herausgezogen + host-getestet.** Die Index-Arithmetik der
   Queue (`head`/`tail`/`count`, Einreihen/Ausreihen, Overflow-Zaehler) lebt
   jetzt in `ts3_cmd_ring.h` als `static inline`-Funktionen **ohne Win32/TS** —
   genau das Muster von `wakeup_policy.h` und `render_state`. Der **Lock**
   (`CRITICAL_SECTION`) bleibt in `ts3_adapter.c` und umschliesst jeden
   Ring-Aufruf. **Kein neues `.c` im DLL-Build** (Header-only), also keine
   `vcxproj`/`build_mingw.sh`-Aenderung.

4. **Overflow-Log praeziser.** Statt „jede 100. Verwerfung“ wird nun bei
   **jeder Aenderung** des Drop-Zaehlers geloggt (aus dem Ring gelesen), damit
   keine Verwerfung stumm bleibt. Der Zaehler bleibt ueber `ts3_adapter_shutdown`
   erhalten (nur Inhalt wird geleert).

**Bewusst NICHT geaendert:** Die koaleszierenden Flags bleiben Flags
(Recompute-all/Dirty, CEPOS-Send, Unmute, Positions-Update, Voice-Notify,
Chat). Es wurde **kein** neuer Kommandotyp erfunden (siehe naechster Abschnitt).
Reihenfolge im Dispatcher, Budget (64), Wakeup-Semantik, Ringgroesse (256) —
alles unveraendert.

---

## Warum eine naive „alles wird ein Kommando“-Umstellung falsch waere

Der urspruengliche V8-Leitsatz klang nach „Pending-Flags werden **zu**
typisierten Kommandos“. Eine woertliche Umsetzung waere ein Eigentor.

Ein Audit **jeder** Off-Callback-Thread-Quelle (alle Aufrufer von
`ts3_request_wakeup*` und alle Cross-Thread-Flag-Setzer) ergab folgende
Einteilung:

| Ausloeser (Producer) | Datei | Thread | Art | Kanal |
|---|---|---|---|---|
| CEPOS-Empfang → Dirty-Bit/Recompute | `ts3_cepos.c`, `ts3_proximity_audio.c` | Callback | koaleszierend (bis 512 Clients) | **A Flag** |
| CEPOS-Send faellig | `ts3_cepos.c` `cepos_signal_send_pending` | jeder | koaleszierend (1 Send/Zyklus) | **A Flag** |
| Recompute-all (UI/Save/Preset) | `plugin_ui_compat.c`, `ts3_proximity_audio.c` | UI/Callback | koaleszierend (1 Durchlauf) | **A Flag** |
| Lokale Positions-Aenderung | `ts3_proximity_audio.c` `on_local_position_update` | Watcher | koaleszierend | **A Flag** |
| Unmute noetig | `ts3_proximity_audio.c` `signal_unmute` (+audio-thread flag-only) | Audio/Callback | koaleszierend (Batch 64) | **A Flag** |
| Channel-Positions-Update | `channel_manage.c` `chan_signal_position_update` | Watcher | koaleszierend | **A Flag** |
| Channel-Move noch offen (Re-Wake) | `channel_manage.c` `chan_tick` | Callback | koaleszierend | **A Flag** |
| Voice-Mode-Notify / Chat | `voice_modes.c`, `util_base.c` (`ts3ChatQueue`) | UI/Poller | eigener Chat-Ring, latenzkritisch | **A Flag** (+ eigener Ring) |
| End-of-Drain-Re-Wake (Budget-Rest) | `ts3_entry.c` | Callback | koaleszierend | **A Flag** |
| „Kanalliste ins Log“ (Self-Test bei Connect) | `ts3_entry.c` | Callback | **diskrete Einmal-Aktion** | **B Kommando** (`TS3_CMD_LOG_CHANNEL_LIST`) |

**Ergebnis:** Ausser dem schon vorhandenen `TS3_CMD_LOG_CHANNEL_LIST` gibt es
**keine** echte diskrete Einmal-Aktion, die heute auf einem Flag/Seitenkanal
reitet und als Kommando sauberer waere. Deshalb wurde — wie im Auftrag
gefordert — **kein kuenstlicher Kommandotyp erfunden**. Der Chat laeuft ueber
seinen eigenen kleinen Ring (`ts3ChatQueue`) mit Urgent-Wakeup; ihn in die
typisierte Queue zu zwingen waere Churn ohne Nutzen und wuerde den
Latenz-Schnellpfad gefaehrden.

---

## Warum das Zwei-Kanal-Design besser **und korrekt** ist (Koaleszenz als Feature)

Der wichtigste Verkehr im Plugin ist **hochfrequent und von Natur aus
zusammenfassbar**. Beispiel **200 Spieler @ 1 Hz CEPOS**:

- **Mit Flag (richtig):** Jedes eingehende CEPOS markiert „Client dirty“
  (Bit schon gesetzt = No-Op) und bittet um einen Wakeup (schon angefordert =
  No-Op). Egal ob 1 oder 200 Pakete ankommen — der naechste CEDRAIN rechnet die
  dirty Clients **einmal** durch, gedeckelt auf 64 pro Durchlauf, Rest im
  naechsten. Kosten: ein Bit + ein Flag.
- **Mit Kommando (falsch):** 200 Pakete → 200 `Ts3Command` in die Queue → der
  Callback-Thread arbeitet 200 Einzelauftraege ab, viele davon fuer denselben
  Client, jeder mit Writer-Lock. Genau **der Callback-Spike**, den V8
  ausschliessen will. Bei mehreren Spielern gleichzeitig laeuft die Queue voll
  und verwirft.

Ein Flag ist also nicht „die schlampige Variante einer Queue“, sondern das
**korrekte** Werkzeug fuer koaleszierenden Zustand: „es muss neu gerechnet
werden“ ist **ein** Fakt, egal wie oft er ausgeloest wurde. Die typisierte
Queue ist das korrekte Werkzeug fuer **diskrete** Aktionen, bei denen jede
Anforderung wirklich einzeln zaehlt und nie in Paketfrequenz feuert.

Weitere Vorteile der jetzt expliziten Trennung:

- **Ein Dispatcher, eine Wahrheit:** `ts3_pending_work_any()` ist die einzige
  „gibt es Arbeit?“-Checkliste (Early-Out **und** Re-Wake), der CEDRAIN-Branch
  die einzige Drain-Stelle. Vergessene Quellen (der V8.2-Bug) sind
  strukturell ausgeschlossen.
- **Ring testbar:** Die reine Ring-Mechanik hat keine Win32-Kopplung mehr und
  ist maschinell abgesichert.
- **Auffindbar erweiterbar:** Zwei parallele Marker sagen dem naechsten
  Entwickler, wo Kanal A bzw. Kanal B waechst — und der Header sagt, welchen er
  waehlen soll.

---

## Wie funktioniert es jetzt (anfaengertauglich)

Stell dir eine Kueche mit **einem** Koch (Callback-Thread) vor. Gaeste
(andere Threads) geben Wuensche auf zwei Arten ab:

- **Kanal A — die Magnettafel (Flags):** „Suppe muss nachgekocht werden.“ Egal
  wie viele Gaeste das rufen — es klebt **ein** Magnet an der Tafel. Der Koch
  sieht ihn und kocht **einmal** eine frische Suppe fuer alle.
- **Kanal B — der Bonzettel-Spiess (Queue):** „Einmal die Weinkarte vorlesen.“
  Das ist eine einzelne, einmalige Sache — sie kommt als Zettel auf den Spiess.

Beide klingeln nur an der Glocke (`Wakeup`). Der Koch schaut auf seine
**Checkliste** (`ts3_pending_work_any`) und arbeitet in **fester Reihenfolge**,
aber nur einen **begrenzten Stapel** pro Runde ab. Bleibt etwas liegen, klingelt
er sich selbst fuer die naechste Runde.

```
Producer (irgendein Thread)
   │  koaleszierend?  ("200x rufen = trotzdem einmal tun")
   ├── ja ──►  Flag / Dirty-Bit setzen ───────────┐   Kanal A
   └── nein ─► Ts3Command push (typisierter Ring) ─┤   Kanal B (diskret)
                                                    ▼
                              ts3_request_wakeup[_urgent]()   (nur Flag + SetEvent)
                                                    ▼
                        Wakeup-Thread  ──►  sendPluginCommand("CEDRAIN:1")
                                                    ▼
        CEDRAIN-Dispatcher  (Callback-Thread — die EINZIGE Drain-Stelle)
             │
             ├─ ts3_pending_work_any()?  ── nein ─►  return
             │        ja
             ▼   feste Reihenfolge + Budget (Deckel pro Runde):
          Queue(B) → Voice → Chat → CEPOS → Recompute(≤64) →
          3D → Profil/Channel → Unmutes(≤64)
             │
             ▼
          Rest offen?  ── ja ─►  erneuter Wakeup (Rest in der naechsten Runde)
```

Der Ring (Kanal B) im Detail: fester 256-Slot-Puffer, `push` aus jedem Thread
(unter Lock), voller Ring → Wunsch wird **verworfen + gezaehlt + geloggt**
(niemals ueberschrieben, niemals blockierend). `drain` laeuft **nur** auf dem
Callback-Thread (Guard loggt einmal, falls doch falsch gerufen).

---

## Wie wurde es getestet?

- **Neuer Host-Unit-Test** `tests/cmd_queue_test.c` gegen `ts3_cmd_ring.h`
  (plain gcc, `-Isdk/include` fuer den `Ts3Command`-Typ, kein Win32):
  leerer Ring, FIFO-Reihenfolge, Ablehnung von `NULL`/`TS3_CMD_NONE`,
  Overflow (voller Ring verwirft + zaehlt, aeltester Eintrag bleibt erhalten),
  und Wraparound ueber mehrere Kapazitaeten hinweg.
- `bash tests/run_tests.sh` → **alle 9 Suites gruen** (inkl. neuer
  `cmd_queue_test` und layering_guard).
- `bash build/build_mingw.sh` → `bin/mingw/conan_exiles.dll` linkt OK (der
  Ring wird im echten DLL-Build ueber den Lock benutzt).
- Per Code-Suche verifiziert: `drain` behaelt den Callback-Thread-Guard;
  `push` laeuft unter `g_cmdLock`; Kommandos werden **ausserhalb** des Locks
  ausgefuehrt; kein Flag wurde zu einem Kommando umgebaut.

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

Reines Refactoring/Doku — Verhalten unveraendert. Der ausstehende
**Lasttest** aus `doku/010`/`011` deckt diese Phase mit ab:

1. **Connect:** Beim Verbinden erscheint der Kanalisten-Self-Test einmalig im
   Client-Log (`TS-CMD: channel list ...`) — Kanal B funktioniert.
2. **Lasttest (200 Spieler / CEPOS-Flut):** fluessige Positions-/Lautstaerke-
   Updates, kein Callback-Ruckler — Kanal A koalesziert wie zuvor.
3. **Voice-Mode per Hotkey:** Chat-Feedback sofort (urgent-Wakeup).

## Lerneffekt

- **Das richtige Werkzeug haengt an der Natur der Arbeit.** Koaleszierender
  Zustand („einmal tun, egal wie oft ausgeloest“) gehoert hinter ein Flag;
  diskrete Einzel-Aktionen gehoeren in eine Queue. Beides in **ein** Muster zu
  pressen erzeugt entweder Fluten (alles Queue) oder Flag-Wildwuchs (alles Flag).
- **Explizit schlaegt implizit.** Das Design existierte faktisch schon — es war
  nur nirgends benannt. Ein Header-Kommentar + zwei parallele Marker machen aus
  „historisch so gewachsen“ eine bewusste, erweiterbare Architektur.
- **Testbarkeit durch Trennung.** Die pure Logik (Ring-Arithmetik) vom Win32-
  Lock zu trennen macht sie ohne invasiven Umbau host-testbar — dasselbe
  Rezept wie bei `wakeup_should_send` und `render_state_needs_reinit`.
- **Nichts erfinden, was nicht gebraucht wird.** Der ehrliche Befund „ausser
  dem Log-Kommando gibt es keine diskrete Aktion“ ist ein gueltiges Ergebnis —
  kuenstliche Kommandos haetten nur Komplexitaet ohne Nutzen gebracht.
