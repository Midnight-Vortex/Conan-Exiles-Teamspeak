# 011 — Ein Pending-Aggregator + Arbeits-Budget pro Drain

Arbeitspaket aus Phase **V8.4** (Thread-Kern II), Teil 4b. Betrifft
`src/ts/entry/ts3_entry.c` (Aggregator + Early-Out + Re-Wake) und
`src/ts/proximity/ts3_proximity_audio.c` (Budget im Recompute-Flush). Kein
Feature, keine hoerbare Verhaltensaenderung im Normalbetrieb — nur Wartbarkeit
und ein garantierter Deckel gegen Callback-Spikes.

---

## Was wurde geaendert?

**1) Ein Aggregator statt einer handgepflegten &&-Kette.**
Neue Funktion `ts3_pending_work_any()` in `ts3_entry.c` listet **an genau einer
Stelle** alle Pending-Quellen auf:

- `ts3_cmd_queue_nonempty()` (Command-Queue)
- `voice_mode_has_pending_notify()`
- `ts3_plugin_has_pending_chat()` (Chat)
- `cepos_send_pending()` (eigener CEPOS-Send)
- `ts3_audio_has_pending_unmutes()`
- `ts3_audio_has_pending_recompute()`
- `chan_has_pending_work()` (Move in-flight + Positions-Update)

Darueber steht der Marker `>>> ADD NEW PENDING SOURCES HERE <<<`. Der
CEDRAIN-Handler nutzt den Aggregator sowohl fuer den **Early-Out** am Anfang als
auch fuer die **Re-Wake-Pruefung** am Ende.

**2) Budget im Dirty-Client-Recompute.**
`ts3_audio_flush_recomputes` verarbeitet im Dirty-Pfad jetzt maximal
`TS3_RECOMPUTE_DRAIN_BUDGET` (= 64) Clients pro Drain. Bleiben danach dirty
Clients uebrig, fordert die Funktion einen erneuten Wakeup an. Der
End-of-Drain-Re-Wake im Handler (ueber den Aggregator) faengt denselben Fall
zusaetzlich ab.

**Bewusst NICHT angefasst:** `audio_recompute_all_impl` bleibt **ein** Durchlauf
(bereits tabellen-begrenzt und gebatcht, wie im Auftrag vorgegeben). Die
Unmute-Flush-Schleife war schon auf `TS3_UNMUTE_BATCH_MAX` (64) gedeckelt und
re-waked bei Rest — unveraendert. `ts3_cmd_queue_drain` ist auf die Queue-Groesse
begrenzt. `server_profile_tick`/`chan_tick` laufen wie bisher einmal pro Drain.

---

## Wie war es vorher (V7 / V8.2)?

Der CEDRAIN-Handler hatte die Pending-Pruefung **zweimal von Hand
ausgeschrieben**: eine lange `&& !...`-Kette fuer den Early-Out und getrennt
davon die Re-Wake-Logik (nur fuer Chat). Diese Doppelpflege ist genau die
Fehlerklasse, die in **V8.2 (Fix 2)** schon einmal zugeschlagen hat:
`chan_has_pending_work` vergass das Flag `g_positionUpdatePending`, wodurch
CEDRAIN zu frueh aussteigen konnte und ein Positions-Update verlorenging.

Zusaetzlich lief der Dirty-Client-Recompute **ohne Deckel**: Bei einer
CEPOS-Flut (bis zu 512 gleichzeitig dirty markierte Clients) konnte ein einziger
CEDRAIN-Callback bis zu 512 volle Neuberechnungen (je mit Writer-Lock) in einem
Rutsch machen → Callback-Spike.

---

## Warum ist die neue Loesung besser (stabiler)?

- **Eine Wahrheit, ein Ort:** Vergessene Pending-Quellen sind ausgeschlossen —
  Early-Out und Re-Wake benutzen dieselbe Funktion. Eine neue Quelle traegt man
  genau einmal ein (am Marker).
- **Kein verlorener Tick:** Weil der End-of-Drain-Re-Wake denselben Aggregator
  nutzt, wird **jede** Rest-Arbeit (nicht nur Chat) zuverlaessig nachgeholt.
- **Kein Callback-Spike:** Das 64er-Budget begrenzt die Callback-Zeit pro Drain
  hart. Der Rest wandert per Wakeup in den naechsten Durchlauf — die Last wird
  ueber mehrere 30-ms-Fenster verteilt statt in einem Callback zu klumpen.
- **Verhalten im Normalbetrieb identisch:** Mit wenigen dirty Clients (< 64) ist
  nichts anders. Das Budget greift nur unter Last.

---

## Wie funktioniert es (anfaengertauglich)?

**Aggregator = eine Checkliste.** Frueher stand die „Gibt es Arbeit?“-Liste an
zwei Stellen im Code. Wenn man an einer Stelle einen Punkt vergisst, entscheiden
die beiden Stellen unterschiedlich → Bug. Jetzt gibt es **eine** Checkliste
(`ts3_pending_work_any`), die beide Stellen abfragen.

```
onPluginCommandEvent("CEDRAIN")
        │
        ▼
  ts3_pending_work_any()?  ── nein ─►  return (nichts zu tun)
        │ ja
        ▼
  Drain: Queue → Chat → CEPOS → Recompute → Profil/Channel → Unmutes
        │
        ▼
  ts3_plugin_has_pending_chat()?  ── ja ─►  Wakeup (urgent)
        │ nein
        ▼
  ts3_pending_work_any()?         ── ja ─►  Wakeup (normal)   ◄─ Rest aus Budget
        │ nein
        ▼
       fertig
```

**Budget = „nur ein Stapel pro Runde“.** Der Recompute-Flush arbeitet pro Drain
hoechstens 64 dirty Clients ab:

```
flush_recomputes (Dirty-Pfad)
  processed = 0
  fuer jeden Client in der Spielertabelle:
     wenn processed >= 64  ->  abbrechen (Rest bleibt dirty)
     wenn Client dirty     ->  neu berechnen; processed++
  danach: sind noch dirty Clients uebrig?  ->  Wakeup anfordern
```

So bleibt jeder CEDRAIN-Callback kurz, auch wenn 200+ Spieler gleichzeitig neue
Positionen schicken.

---

## Wie wurde es getestet?

- `bash tests/run_tests.sh` → **alle 6 Suites gruen** (251 Checks). Die geaenderte
  Logik ist Callback-Thread-Steuerfluss (kein reiner, isoliert testbarer
  Rechenkern), daher keine neue Unit-Suite — die Budget-Arithmetik ist ein
  einfacher Zaehler-Vergleich (`processed >= 64`).
- `bash build/build_mingw.sh` → `conan_exiles.dll` linkt OK.
- Per Code-Suche verifiziert: Early-Out und End-of-Drain-Re-Wake rufen **beide**
  `ts3_pending_work_any()`; im Recompute-Flush deckelt `processed` die Schleife.

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

1. **Lasttest (Ziel dieser Phase):** Server mit vielen Spielern / CEPOS-Flut.
   Erwartung: fluessige Positions-/Lautstaerke-Updates, keine Ruckler im
   TS-Client, kein Audio-Aussetzer — die Last verteilt sich ueber mehrere Drains.
2. **Normalbetrieb:** Zwei bis drei Spieler in Hoerweite bewegen sich. Erwartung:
   identisches Verhalten wie vorher (Budget greift nicht).
3. **Voice-Mode-Umschaltung unter Last:** Hotkey druecken, waehrend viele CEPOS
   ankommen. Erwartung: Chat-Feedback erscheint sofort (urgent-Wakeup), die
   Neuberechnungen laufen im Hintergrund nach.

## Lerneffekt

- **Doppelte Wahrheit ist ein Bug-Magnet.** Zwei handgepflegte Kopien derselben
  Liste driften auseinander. Eine Funktion als „Single Source of Truth“ — von
  allen Stellen aufgerufen — macht die Fehlerklasse unmoeglich.
- **Budget + Re-Wake statt Alles-auf-einmal.** Ein harter Deckel pro Durchlauf
  plus „weck mich fuer den Rest“ verteilt Spitzenlast, ohne Arbeit zu verlieren —
  dasselbe Muster wie beim Unmute-Batch.
- **Ein sichtbarer Marker** (`ADD NEW PENDING SOURCES HERE`) im Code ist billige,
  wirksame Vorsorge gegen „das naechste vergessene Flag“.
