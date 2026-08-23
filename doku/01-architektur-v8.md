# 01 — Architektur V8

Diese Seite erklaert, wie das Plugin ab V8 aufgebaut ist — und **warum** genau so.
Die Begruendungen kommen aus den V7-Lehren (`02-lessons-learned-v7.md`).

## Das Grundproblem: viele Threads, eine empfindliche API

Ein TeamSpeak-Plugin lebt in einer feindlichen Umgebung:

- TeamSpeak ruft unsere **Callbacks** auf einem eigenen Thread auf (Verbindungsstatus,
  empfangene Pakete, Chat, …).
- Die **Sprach-Samples** kommen auf einem anderen, extrem zeitkritischen **Audio-Thread**
  (PCM). Wenn wir den blockieren, knackt oder stottert der Ton fuer ALLE.
- Wir selbst brauchen weitere Threads: einen **Watcher**, der `Pos.txt` pollt, einen
  **Overlay-Thread** (Fenster brauchen unter Windows eine eigene Message-Loop), einen
  **Hotkey-Poller** und zeitweise einen **Settings-Dialog-Thread**.
- Die **TS-API selbst ist nicht garantiert thread-sicher**. Wer sie von mehreren Threads
  gleichzeitig ruft, bekommt die Sorte Crash, die das alte Mumble-Plugin geplagt hat
  (`RtlpWaitOnCriticalSection` bei 20+ Spielern).

## Der Thread-Vertrag (die oberste Regel)

> **Regel 1:** Nur der **Callback-Thread** ruft die TS-API. Die einzige, bewusst
> in Kauf genommene Ausnahme ist der **Wakeup-Thread** (V8.4): Er ruft *genau
> eine* Funktion, `sendPluginCommand("CEDRAIN:1")`. Mehr geht nicht, weil das SDK
> keinen Timer-/Wake-Callback bietet. Alle Fremd-Threads (Watcher, UI, PCM)
> fordern einen Wakeup nur per Flag+Event an — sie rufen selbst **keine** TS-API
> mehr (das war die V7-Luecke). Damit beruehren genau **zwei** Threads die API:
> Callback-Thread (alles) + Wakeup-Thread (ein Aufruf).
>
> **Regel 2:** Der **Audio-Thread** ruft NIE die TS-API und nimmt NIE Locks. Er liest nur
> lock-freie **Snapshots** und besitzt seinen Render-Zustand (Gain-Rampen) **exklusiv**.
>
> **Regel 3:** Alle anderen Threads duerfen nur: Daten in eigene Strukturen schreiben,
> **Kommandos in die Queue legen** und einen Wakeup **anfordern** (nicht selbst senden).
>
> **Regel 4:** Logging hat einen eigenen Lock und beruehrt nie den API-Pfad.
>
> **Regel 5:** Jede Datei dokumentiert oben ihren Thread-Vertrag (wer ruft mich, welche
> Locks darf ich nehmen).

### Was heisst "Snapshot" und "Seqlock"? (Anfaenger-Erklaerung)

Der Audio-Thread braucht pro Sprecher die Werte "Lautstaerke, Pan links/rechts, Filter".
Diese Werte berechnet der Callback-Thread. Damit der Audio-Thread sie **ohne Lock** lesen
kann, benutzen wir ein **Seqlock**: Der Schreiber erhoeht vor und nach dem Schreiben einen
Zaehler. Der Leser liest den Zaehler, dann die Daten, dann den Zaehler nochmal — sind beide
gleich und gerade, waren die Daten konsistent; sonst liest er einfach nochmal. So wartet
niemand auf niemanden.

## Schichten-Modell (ab V8 strikt)

```
┌────────────────────────────────────────────────────────┐
│ ui/    Fenster, Overlay, Hotkeys (eigene Threads)      │
│        darf: core/ nutzen, Kommandos in Queue legen    │
│        darf NICHT: TS-API direkt rufen                 │
├────────────────────────────────────────────────────────┤
│ ts/    Alles was die TS-API beruehrt                   │
│        adapter = Queue + Dispatcher (Callback-Thread)  │
│        proximity = CEPOS, 3D, PCM-Audio                │
├────────────────────────────────────────────────────────┤
│ core/  PURE Logik: Mathe, Parser, Tabellen, Config     │
│        kein Win32 (wo vermeidbar), NIE ts/ oder ui/    │
│        includen → auf jedem Rechner unit-testbar       │
└────────────────────────────────────────────────────────┘
```

**Warum?** In V7 included `core/` munter `ts/`- und `ui/`-Header. Folge: nichts war isoliert
testbar, und eine UI-Aenderung konnte den Audio-Pfad brechen. In V8 gilt: Abhaengigkeiten
zeigen nur nach unten.

## Datenfluss (Soll-Bild V8)

```
Pos.txt ──(Watcher-Thread, 30ms)──▶ lokale Position (atomar)
                                       │  Kommando "Position neu" in Queue + Wakeup-Anforderung
                                       ▼
                 ┌—— Callback-Thread (einziger TS-API-Nutzer) ——┐
                 │ Dispatcher: holt Kommandos aus der Queue,     │
                 │ mit Budget (max N Arbeit pro Durchlauf):      │
                 │  • CEPOS senden (gedrosselt, on-change)       │
                 │  • empfangene CEPOS → player_table            │
                 │  • Distanz/Pan/Filter berechnen →             │
                 │    Snapshots publizieren (Seqlock)            │
                 │  • 3D-Positionen setzen (dedupliziert)        │
                 │  • Unmutes als Batch                          │
                 │  • Channel-Move hub ↔ ingame                  │
                 └───────────────────────────────────────────────┘
                                       │ Snapshots (lock-frei)
                                       ▼
                 Audio-Thread: liest Snapshot, wendet Gain/Pan/
                 Lowpass/Reverb auf die Samples an. Besitzt seine
                 Rampen selbst. Setzt hoechstens atomare Flags
                 ("Client X braucht Unmute").
```

## Die Steuerplane: zwei Kanaele (V8-Kernstueck)

**V7-Problem:** Es gab eine typisierte Queue, aber real lief fast alles ueber ein Dutzend
einzelner Pending-Flags, die ein Mega-Handler (CEDRAIN) in einem Rutsch abarbeitete. Die
Early-Out-Pruefung vergass Flags, der Wakeup selbst brach den Thread-Vertrag.

**Was der urspruengliche Plan falsch dachte:** Das Leitziel klang nach „**alles** wird ein
Kommando“. Genau das waere aber ein Fehler. Der wichtigste Verkehr im Plugin ist
**hochfrequent und von Natur aus koaleszierend**: „Lautstaerken neu berechnen“, „CEPOS
senden“, „Client X entmuten“, „Positions-Update“. Wenn 200 Spieler im Sekundentakt CEPOS
schicken, heisst „es muss neu gerechnet werden“ **einmal** neu rechnen — nicht 200 Kommandos
in eine Queue legen. Ein Flag, das man nur setzt (schon gesetzt = No-Op), fasst diese Flut
kostenlos zusammen. Eine Queue wuerde stattdessen genau die Callback-Flut erzeugen, die V8
verhindern will. **Koaleszenz ist hier ein Feature, kein Bug.**

**V8-Loesung — zwei Kanaele, ein Dispatcher.** Fremd-Threads treiben Callback-Arbeit ueber
genau zwei Wege; beide werden vom **einen** CEDRAIN-Dispatcher in fester Reihenfolge mit
Budget abgearbeitet. Die Natur der Arbeit entscheidet den Kanal:

- **(A) Koaleszierende Pending-Flags** — fuer hochfrequente Zustaende, bei denen aus „N
  Anforderungen“ „eine Sache zu tun“ wird. Producer setzt ein Interlocked-Flag (oder ein
  Dirty-Bit pro Client) und fordert einen Wakeup an. Der Dispatcher fragt **eine** zentrale
  Checkliste `ts3_pending_work_any()` und fuehrt den jeweiligen Schritt **einmal** ueber den
  aktuellen Zustand aus. Das bleiben Flags: Audio-Recompute-all / Dirty-Client-Recompute,
  CEPOS-Send, CEMODE-Send, Unmute, Channel-Positions-Update, Voice-Mode-Notify, Chat.
- **(B) Die typisierte Command-Queue** (`Ts3Command` / `Ts3CmdType`, ein Ringpuffer) — nur
  fuer **diskrete Einmal-Aktionen**, die nicht koaleszieren und **nicht** in Paketfrequenz
  auftreten koennen: z. B. das einmalige „Kanalliste ins Log“ (Self-Test) oder eine
  UI-ausgeloeste Einzel-Anforderung. Producer fuellt ein `Ts3Command` und
  `ts3_cmd_queue_push()` (aus jedem Thread sicher; volle Queue verwirft + loggt), dann
  Wakeup. Der Dispatcher leert die Queue **zuerst** in jedem Durchlauf.

**Faustregel:** Wuerde dieselbe Anforderung 200×/Tick weiterhin „nur einmal tun“ bedeuten,
gehoert sie zu **(A)** (Flag). Ist jede Anforderung eine eigene Aktion, die einzeln passieren
muss, **und** kann sie nie in Paketfrequenz feuern, gehoert sie zu **(B)** (Kommando). Im
Zweifel: Flag — Fluten ist der teure Fehler.

**Gemeinsame Eigenschaften beider Kanaele:**

- Producer (jeder Thread) legt ab / setzt ein Flag, Dispatcher (nur Callback-Thread) arbeitet ab.
- **Budget:** pro Durchlauf ein harter Deckel (z. B. 64 Dirty-Recomputes, Queue auf Ringgroesse
  begrenzt) — kein Callback-Spike, Rest laeuft im naechsten Durchlauf (`doku/011`).
- Duplikate koaleszieren beim Einreihen (Kanal A: „recompute client 5“ nur 1× pending).
- Der Wakeup sagt nur „es liegt Arbeit da“ — **ohne** selbst die TS-API zu benutzen.
  Umgesetzt in V8.4 (`doku/aenderungen/010`): `ts3_request_wakeup*` setzen nur ein
  Flag + `SetEvent`; ein einziger dedizierter **Wakeup-Thread** sendet den
  `CEDRAIN`-Befehl (gedrosselt auf 1×/30 ms, urgent umgeht die Drossel) und wird
  beim Shutdown vor dem Zustands-Teardown gejoint.

**Diskoverbarkeit im Code:** Neue Pending-Quellen traegt man an genau einem Marker ein
(`>>> ADD NEW PENDING SOURCES HERE <<<` in `ts3_entry.c`), neue Kommandotypen am
parallelen Marker (`>>> ADD NEW COMMAND TYPES HERE <<<` in `ts3_adapter.h`). Der reine
Ring (`ts3_cmd_ring.h`) ist ohne Win32 host-unit-getestet (`tests/cmd_queue_test.c`), der
Lock lebt in `ts3_adapter.c`. Details: `doku/aenderungen/020`.

```
Producer (irgendein Thread)
   │  koaleszierend?
   ├── ja ──►  Flag/Dirty-Bit setzen ────────────┐   Kanal A
   └── nein ─► Ts3Command push (typisierte Queue) ┤   Kanal B
                                                   ▼
                                     ts3_request_wakeup[_urgent]()  (Flag + SetEvent)
                                                   ▼
                            Wakeup-Thread ──► sendPluginCommand("CEDRAIN:1")
                                                   ▼
                       CEDRAIN-Dispatcher (Callback-Thread, EINZIGE Stelle):
                         ts3_pending_work_any()? ── nein ─► return
                         feste Reihenfolge + Budget:
                           Queue → Voice → Chat → CEPOS → CEMODE →
                           Recompute → 3D → Profil/Channel → Unmutes
                         Rest offen? ── ja ─► erneuter Wakeup (naechster Durchlauf)
```

## Besitzer-Prinzip (Ownership)

Jeder Zustand hat ab V8 **genau einen** schreibenden Besitzer:

| Zustand | Besitzer (Schreiber) | Leser |
|---|---|---|
| `plugin.cfg` / `g_config` | Callback-Thread (Apply/Save via Queue) | alle (Read-Only-Sicht) |
| Render-Rampen (Gain/Pan aktuell) | Audio-Thread | niemand sonst |
| Ziel-Werte (Snapshots) | Callback-Thread | Audio-Thread (Seqlock) |
| Hotkey-Zustand | EIN Poller-Thread | Callback via Queue-Kommandos |
| Kanal-IDs, Verbindungs-Epoche | Callback-Thread (atomar publiziert) | alle |
| Overlay-Fenster (HWND) | Overlay-Thread | andere nur via PostMessage |

**Warum?** Fast jeder schwer findbare V7-Bug war ein "zwei Schreiber"-Bug: zwei Config-
Writer, zwei Hotkey-Poller, Reset-Pfad schreibt in Audio-Rampen. Ein Besitzer = eine Wahrheit.

## Shutdown-Reihenfolge (fest definiert — seit V8.8 umgesetzt, "Ist")

So arbeitet `ts3plugin_shutdown()` (`src/ts/entry/ts3_entry.c`) es heute ab
(Details: `doku/aenderungen/014-shutdown-haertung.md`):

1. **Annahme stoppen:** Audio auf Passthrough, `pluginShuttingDown` gesetzt,
   verzoegerte Overlay-Starts entschaerft.
2. **Threads stoppen + joinen** (Abhaengigkeits-Reihenfolge): Hotkey-Poller →
   Settings-Dialog (WM_CLOSE + Join) → Pos-Watcher (Event + Join) →
   Overlay-Monitor (Join) + Overlay-UI (WM_QUIT + Join; zerstoert sein HWND
   selbst) → `overlayTextLock` erst nach dem Join loeschen.
3. **Modul-Zustand zuruecksetzen:** Player-Tabelle, CEPOS, CEMODE, 3D, Channel,
   Profil, Nick, Version, Audio-Snapshots (pure Resets, keine TS-API).
4. **Adapter schliessen:** joint den Wakeup-Thread, loescht die
   Verbindungs-Identitaet — danach ist jeder API-Call ein Programmierfehler
   (die Guards loggen ihn).
5. **Log zuletzt schliessen.**

**Warum diese Reihenfolge?** Ein Thread, der nach Adapter-Shutdown noch einen API-Call
absetzt, greift ins Leere (Use-after-free). V7 hat Threads teils nicht gejoint —
V8 macht jeden Thread joinbar (ueberall `_beginthreadex`-Handles), bevor
irgendetwas freigegeben wird.
