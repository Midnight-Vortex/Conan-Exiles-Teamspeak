# 012 — 3D-Anwendung vom CEPOS-Send entkoppeln

Arbeitspaket aus Phase **V8.4** (Thread-Kern II), Teil 4c. Betrifft nur den
CEDRAIN-Handler in `src/ts/entry/ts3_entry.c`. Ein kleiner, aber hoerbar/
raeumlich relevanter Fix: entfernte Spieler standen im TS-3D-Panorama still, wenn
nur ihre Positionen ankamen, wir selbst uns aber nicht bewegten.

---

## Was wurde geaendert?

Vorher hing `ts3d_apply()` **am eigenen CEPOS-Send**:

```c
if (cepos_send_pending()) {
    cepos_flush();
    ts3d_apply();      // lief NUR wenn wir selbst senden wollten
}
```

Jetzt laeuft `ts3d_apply()` **bei jedem Proximity-Drain**, unabhaengig vom
eigenen Send:

```c
if (cepos_send_pending()) {
    cepos_flush();
}
...
if (ts3_audio_get_mode() == TS3_AUDIO_PROXIMITY) {
    ts3d_apply();      // jetzt entkoppelt, nur an den Proximity-Modus gebunden
}
```

**Bewusst NICHT angefasst:** `ts3d_apply` selbst (interne 20-Hz-Drossel
`TS3D_APPLY_MIN_MS=50` und Epsilon-Dedup bleiben), der CEPOS-Send-Pfad, die
Recompute-Logik.

---

## Wie war es vorher?

`ts3d_apply()` setzt die **Listener-Position** (wir selbst) und die
**3D-Positionen der entfernten Spieler** ueber die TS-API. Es wurde aber nur
aufgerufen, wenn `cepos_send_pending()` wahr war — also nur, wenn **unsere
eigene** Position gerade rausgeschickt werden sollte.

Folge: Bewegt sich ein anderer Spieler, waehrend **wir stillstehen**, kommen nur
**entfernte** CEPOS-Pakete an (die markieren Clients „dirty“ und triggern einen
Recompute — aber `cepos_send_pending` bleibt falsch, weil wir nichts senden
muessen). Damit lief `ts3d_apply` nicht, und die **TS-3D-Position des bewegten
Spielers blieb veraltet** stehen, bis wir uns selbst wieder bewegten.

---

## Warum ist die neue Loesung besser?

- **Korrekte Raumortung:** Die 3D-Position entfernter Spieler wird aktualisiert,
  sobald ihre CEPOS ankommen — auch wenn wir uns nicht bewegen. Kein „eingefrorenes“
  Panorama mehr.
- **Billiger Leerlauf:** `ts3d_apply` hat oben eine **20-Hz-Drossel** und danach
  eine Epsilon-Dedup pro Client. Wird es innerhalb von 50 ms erneut aufgerufen,
  steigt es sofort nach einem Zeitstempel-Vergleich wieder aus. Der zusaetzliche
  Aufruf pro Drain kostet im Leerlauf also praktisch nichts.
- **Sauber eingegrenzt:** Der Aufruf ist an den **Proximity-Modus** gebunden. Im
  Passthrough- oder Hub-Mute-Modus spielt die 3D-Ortung keine Rolle, dort laeuft
  `ts3d_apply` gar nicht erst.

Der Aufruf ist bewusst **bedingungslos** (nur Modus-gated) statt fein an
„Recompute lief || CEPOS pending || Tabelle dirty“ gekoppelt: Weil der
Leerlauf-Preis nur ein Zeitstempel-Check ist, ist die einfachste korrekte Form
auch die beste — weniger Bedingungen, weniger Fehlerquellen.

---

## Wie funktioniert es (anfaengertauglich)?

Denk an eine Landkarte mit Stecknadeln: eine Nadel bist du (Listener), die
anderen sind die Mitspieler. `ts3d_apply` steckt die Nadeln an ihre aktuellen
Orte, damit TeamSpeak den Ton raeumlich richtig mischt.

Frueher wurden die Nadeln **nur umgesteckt, wenn DU dich bewegt hast**. Lief ein
anderer an dir vorbei, waehrend du still standest, blieb seine Nadel liegen — sein
Stimme kam weiter „von der alten Stelle“.

```
 Vorher:                              Nachher:
   du bewegst dich?                     Proximity-Modus aktiv?
     ja -> Nadeln neu stecken             ja -> Nadeln neu stecken (gedrosselt)
     nein -> Nadeln bleiben liegen        (unabhaengig davon, wer sich bewegt)
   (fremde Bewegung -> veraltet)

 Kosten im Leerlauf: 1 Zeitstempel-Check (20-Hz-Drossel faengt ab)
```

---

## Wie wurde es getestet?

- `bash tests/run_tests.sh` → **alle 6 Suites gruen** (251 Checks). Reine
  Callback-Steuerfluss-Aenderung ohne neuen isolierten Rechenkern → keine neue
  Unit-Suite.
- `bash build/build_mingw.sh` → `conan_exiles.dll` linkt OK.
- Per Code-Lese verifiziert, dass `ts3d_apply` die 20-Hz-Drossel oben behaelt
  (idle = Zeitstempel-Check) und nur noch modus-gated aufgerufen wird.

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

1. **Stillstehen, Gegenueber bewegt sich:** Du bleibst stehen, ein anderer Spieler
   laeuft um dich herum und redet. Erwartung: Seine Stimme wandert **fluessig** im
   Stereobild mit — sie bleibt nicht an der alten Stelle „kleben“.
2. **Beide still:** Niemand bewegt sich. Erwartung: kein zusaetzlicher CPU-/API-
   Verbrauch (Drossel greift), Ton unveraendert.
3. **Modus-Wechsel:** Im Hub (Mute) bzw. Passthrough darf `ts3d_apply` nicht
   greifen — 3D-Ortung nur im Ingame-/Proximity-Modus.

## Lerneffekt

- **Nebeneffekte nicht an fremde Bedingungen haengen.** „3D anwenden“ und „eigene
  Position senden“ sind zwei Aufgaben; sie an eine gemeinsame `if`-Bedingung zu
  koppeln, koppelt auch ihre Fehler. Jede Aufgabe bekommt ihre **eigene**,
  passende Bedingung.
- **Wenn Leerlauf billig ist, gewinnt Einfachheit.** Eine bedingungslose (nur
  modus-gated) Anwendung mit interner Drossel ist robuster als eine fein
  verzweigte „nur wenn X oder Y oder Z“-Bedingung — weniger Zustaende, die falsch
  sein koennen.
