# 004 — PCM besitzt seinen Zustand exklusiv (Generation-Counter statt Cross-Thread-Reset)

Arbeitspaket aus Phase **V8.3**, Leitziel 3: *„PCM besitzt seinen Zustand
exklusiv“*. Betrifft nur `src/ts/proximity/ts3_proximity_audio.c` / `.h` (plus
ein neuer Host-Unit-Test). Kein Feature, keine hoerbare Verhaltensaenderung —
nur die Beseitigung einer Data-Race.

---

## Was wurde geaendert?

- Neuer Signal-Kanal: `g_snapGeneration[TS3_MAX_CLIENT_ID]` (`volatile LONG`).
  Der **Callback-Thread** erhoeht diesen Zaehler pro Client (`InterlockedIncrement`),
  wenn ein Client ungueltig wird (verlassen / verdraengt / weggemovt / Reset).
- Neuer, **audio-thread-privater** Merker: `g_renderGeneration[TS3_MAX_CLIENT_ID]`
  (`LONG`, ohne Atomics — nur der Audio-Thread schreibt ihn).
- `ts3_audio_process_playback` (Audio-Thread) vergleicht am **Buffer-Anfang** die
  beiden Zaehler. Sind sie unterschiedlich, setzt der Audio-Thread **selbst** die
  Rampe und den Lowpass zurueck (`g_renderGain=1.0`, `g_renderPanL/R=0.7071`,
  `g_lpf[...].initialized=0`) und merkt sich die neue Generation.
- `ts3_audio_invalidate_client` (Callback-Thread) schreibt **nicht mehr** in
  `g_renderGain/g_renderPanL/g_renderPanR`. Es veroeffentlicht wie bisher einen
  neutralen Snapshot und bumpt zusaetzlich nur noch die Generation.
- `ts3_audio_reset` (Callback-Thread) ersetzt die alte volle Schreib-Schleife
  ueber alle Client-IDs (`g_renderGain[i]=1.0; …`) durch eine Schleife, die nur
  die Generation jeder ID bumpt.
- Neue reine Hilfsfunktion `render_state_needs_reinit(lastGen, curGen)` in
  `ts3_proximity_audio.h` (ein `!=`-Vergleich) — host-unit-testbar.
- Der Thread-Vertrag steht jetzt als Kommentar oben in `ts3_proximity_audio.c`.

**Bewusst NICHT angefasst:** Cave-Reverb-Slot-Vergabe, Unmute-Maschinerie
(`g_pendingUnmute`, `g_clientUnlocked`, `g_lastUnmuteMs`), Seqlock-Publish/Read
(`snap_publish`/`snap_read`) ausser dem Generation-Bump, TS-API-Nutzung, sowie
die O(aktive)-Scan-Optimierung aus Phase 5.2 (`audio_client_has_reset_state`).

---

## Wie war es vorher (V7 / vor diesem Paket)?

Die Render-Rampen `g_renderGain/g_renderPanL/g_renderPanR` sind der **Glaettungs-
Zustand des Audio-Threads**: Er liest und schreibt sie in JEDEM PCM-Buffer, damit
Lautstaerke und Panorama sanft (ohne Knacken) auf den neuen Zielwert gleiten.

Das Problem: **zwei Threads schrieben dieselben Arrays.**

- **Audio-Thread** (PCM): pro Buffer, in `ts3_audio_process_playback`.
- **Callback-Thread**: in `ts3_audio_invalidate_client` (Disconnect eines
  Clients, Verdraengung aus der Spieler-Tabelle, Channel-Move weg) und in
  `ts3_audio_reset` (Disconnect / Tab-Wechsel / Shutdown) — dort wurden die
  Rampen genullt bzw. auf den Neutralwert gesetzt.

Diese Schreibzugriffe hatten **keine Synchronisation**. Ein `float`-Schreiben ist
nicht garantiert atomar gegenueber einem gleichzeitigen Lesen/Schreiben auf einem
anderen Thread. Trennt sich ein Spieler **waehrend** aktiver Wiedergabe, konnte
der Audio-Thread eine halb geschriebene Rampe lesen (torn write) → Knackser oder
inkonsistenter Rampen-Zustand. Besonders fies bei **Client-ID-Wiederverwendung**:
Eine frisch verbundene Person bekommt eine alte ID und erbt dabei fremden
Rampen-Muell.

---

## Warum ist die neue Loesung besser (stabiler)?

Das V8-**Besitzer-Prinzip** sagt: jeder Zustand hat **genau einen Schreiber**.
Die Render-Rampen gehoeren dem Audio-Thread — also darf **nur** der Audio-Thread
sie schreiben.

- **Keine torn writes mehr:** Es gibt nur noch einen Schreiber pro Array. Der
  Callback-Thread beruehrt die Rampen und den Lowpass ueberhaupt nicht mehr.
- **Kein Lock noetig:** Das Signal „veraltet“ ist ein einzelner ausgerichteter
  `LONG`-Zaehler. Der Bump (`InterlockedIncrement`) und das Lesen sind atomar;
  der Audio-Thread wartet auf niemanden (bleibt lock-frei, Regel 2).
- **ID-Wiederverwendung ist sauber:** Ein neuer Client mit alter ID sieht eine
  hoehere Generation → der Audio-Thread setzt die Rampe zurueck, bevor er den
  ersten Sample-Buffer glaettet. Kein Erben von Fremd-Zustand.

Wichtig: **hoerbar identisch.** Die exakt gleichen Reset-Werte werden gesetzt,
nur eben vom richtigen Thread. Ein invalidierter Client faded genauso rein/aus
wie vorher.

---

## Wie funktioniert es (anfaengertauglich)?

Stell dir zwei Kollegen vor, die sich EIN Notizbuch (die Rampe) teilen. Frueher
schrieben **beide** rein — manchmal gleichzeitig, und dann stand Unsinn drin.

Ab jetzt gilt: **Nur der Audio-Thread schreibt ins Notizbuch.** Der Callback-Thread
hat nur noch einen kleinen Zettel pro Client, auf den er eine Strichliste macht:

> „Dein Wissen ueber Client X ist veraltet.“

Der Audio-Thread schaut vor jedem Buffer kurz auf den Zettel. Hat sich die
Strichzahl seit dem letzten Mal geaendert, **raeumt er selbst auf** (setzt seine
Rampe/Filter zurueck) und merkt sich die neue Strichzahl. Danach arbeitet er
normal weiter. So schreibt nie jemand anderes in sein Notizbuch.

```
 Callback-Thread                         Audio-Thread (PCM, pro Buffer)
 ───────────────                         ──────────────────────────────
 Client verlaesst / Reset:
   snap_publish_neutral(id, 0)   ─────►  liest snap (ungueltig -> stumm)
   g_snapGeneration[id]++  ══════╗
        (nur Strichliste)        ║
                                 ╚════►  curGen = g_snapGeneration[id]
                                         needs_reinit(g_renderGeneration[id],
                                                      curGen)?
                                           ja  -> g_renderGain[id]  = 1.0
                                                  g_renderPanL/R[id] = 0.7071
                                                  g_lpf[id].initialized = 0
                                                  g_renderGeneration[id] = curGen
                                           nein -> Rampe unveraendert weiter
                                         ... danach normale Glaettung ...

 Ein Schreiber pro Array:
   Rampe/LPF   = nur Audio-Thread
   Generation  = nur Callback-Thread (Audio-Thread liest nur)
```

**Warum reicht der Generation-Bump auch beim vollen Reset (ohne PCM danach)?**
Der Render-Zustand wird **ausschliesslich** vom Audio-Thread konsumiert. Laeuft
nach einem Reset nie wieder ein PCM-Buffer fuer eine ID, ist ihr veralteter
Rampen-Wert egal — niemand liest ihn. Kommt spaeter doch ein Buffer (neue
Verbindung, wiederverwendete ID), sieht er die hoehere Generation und
reinitialisiert zuerst. Es kann also **kein** alter Zustand in eine neue
Verbindung lecken. Genau deshalb ersetzt ein einzelner atomarer Bump die alte
volle Schreib-Schleife gefahrlos.

---

## Wie wurde es getestet?

- **Neuer Host-Unit-Test** `tests/render_state_test.c`: prueft
  `render_state_needs_reinit` (gleiche Generation → kein Reinit; Bump → genau ein
  Reinit; mehrere Bumps zwischen zwei Buffern → ein Reinit; `LONG`-Ueberlauf/Wrap
  → weiterhin Reinit; „Last-Seen holt auf, kein Dauer-Reinit“).
- `bash tests/run_tests.sh` → **alle 5 Suites gruen** (hub_parser,
  proximity_math, zone_resolve, player_table, render_state).
- `bash build/build_mingw.sh` → `bin/mingw/conan_exiles.dll` linkt OK;
  `ts3_proximity_audio.c` kompiliert warnungsfrei.
- Per Code-Suche verifiziert: `g_renderGain/PanL/PanR` werden nur noch in
  `ts3_audio_process_playback` (Audio-Thread) geschrieben; die Callback-Pfade
  bumpen ausschliesslich die Generation.

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

1. **Disconnect waehrend Sprache:** Zwei Spieler in Hoerweite, Gegenueber redet
   durchgehend. Diesen Spieler trennen (oder Server-Tab wechseln), waehrend er
   spricht. Erwartung: sauberes Ausblenden, **kein Knacken/Klick**, kein Crash.
2. **ID-Wiederverwendung:** Nach dem Trennen jemand anderen verbinden lassen, bis
   die alte Client-ID neu vergeben wird. Erwartung: Die neue Person blendet
   normal von leise nach laut ein — **kein Poppen** durch fremde Alt-Lautstaerke.
3. **Tab-Wechsel / Reconnect:** Server-Tab hin- und herwechseln bzw. neu
   verbinden. Erwartung: Ton spielt danach normal, keine haengende Lautstaerke,
   kein Filter-Rest aus der alten Sitzung.

## Lerneffekt

- **Ein Besitzer pro Zustand** ist oft billiger als jede Sperre: Statt den
  Rampen-Zustand zu locken, verbietet man dem zweiten Schreiber einfach den
  Zugriff und gibt ihm ein **schmales atomares Signal** („veraltet“).
- Ein **Generation-Counter** ist das kleinstmoegliche Cross-Thread-Signal: ein
  aligned `LONG`, atomar erhoeht, atomar gelesen — kein Lock, keine Wartezeit,
  keine torn writes.
- Aufraeumen gehoert dorthin, wo der Zustand **gelesen** wird. Der Konsument
  (Audio-Thread) reinitialisiert lazy beim naechsten Zugriff; der Signalgeber
  muss nichts ueber den inneren Aufbau des Zustands wissen.
