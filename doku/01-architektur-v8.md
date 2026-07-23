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

## Die Command-Queue (V8-Kernstueck)

**V7-Problem:** Es gab eine typisierte Queue, aber real lief fast alles ueber ein Dutzend
einzelner Pending-Flags, die ein Mega-Handler (CEDRAIN) in einem Rutsch abarbeitete. Die
Early-Out-Pruefung vergass Flags, der Wakeup selbst brach den Thread-Vertrag.

**V8-Loesung:**

- Jede Arbeit ist ein **typisiertes Kommando** (enum + kleine Payload) in EINEM Ringpuffer.
- Producer (jeder Thread) legt ab, Dispatcher (nur Callback-Thread) arbeitet ab.
- **Budget:** pro Durchlauf maximal N Kommandos / Zeitscheibe — kein Callback-Spike mehr,
  Rest laeuft im naechsten Durchlauf weiter.
- Duplikate werden beim Einreihen zusammengefasst (z. B. "recompute client 5" nur 1× pending).
- Der Wakeup sagt nur noch "es liegt Arbeit da" — **ohne** selbst die TS-API zu benutzen.
  Umgesetzt in V8.4 (`doku/aenderungen/010`): `ts3_request_wakeup*` setzen nur ein
  Flag + `SetEvent`; ein einziger dedizierter **Wakeup-Thread** sendet den
  `CEDRAIN`-Befehl (gedrosselt auf 1×/30 ms, urgent umgeht die Drossel) und wird
  beim Shutdown vor dem Zustands-Teardown gejoint.

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

## Shutdown-Reihenfolge (fest definiert)

1. **Annahme stoppen:** Queue nimmt nichts mehr an, Wakeups werden ignoriert.
2. **Threads joinen:** Watcher, Hotkey-Poller, Overlay (WM_QUIT + Join), Settings-Dialog.
3. **Audio neutralisieren:** Passthrough-Modus, Snapshots invalidieren (Generation++).
4. **Adapter schliessen:** danach ist jeder API-Call ein Programmierfehler (Assert im Debug).
5. **Log zuletzt schliessen.**

**Warum diese Reihenfolge?** Ein Thread, der nach Adapter-Shutdown noch einen API-Call
absetzt, greift ins Leere (Use-after-free). V7 hat Threads teils nicht gejoint —
V8 macht jeden Thread joinbar, bevor irgendetwas freigegeben wird.
