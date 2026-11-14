# 104 — Hoer-Bonus: Reichweite wieder die einfache Summe, nur die Lautstaerke wird gebremst (Plugin 8.0.25)

**Plugin 8.0.25**, 2026-11-14. Gebaut und getestet, **nicht verteilt** (kein Commit, kein Release, nichts in
TeamSpeak kopiert). Loest die Regel aus [Doku 103](103-hoer-bonus-fluestern-anteilig.md) (8.0.24) ab.

Rueckmeldung nach 8.0.24: *„Eigentlich ging es mir um Lautstaerke und nicht Reichweite“* und vom Besitzer: *„Dann
haut die Fundament-Berechnung nicht mehr hin.“*

**Begriffe:** *Root-Channel* = der oberste TeamSpeak-Channel; in seiner Beschreibung stehen die Server-Vorgaben.
*Hoer-Rasse* = ein `[RACE]`-Eintrag dort mit `listenAddDistance` (der Hoer-Bonus). *Reichweite* = wie weit die
Stimme eines Sprechers in seinem Modus (Fluestern, Normal, Rufen) traegt. *Fundament* = Laengenmass der Vorgaben,
2,56 m.

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/proximity/proximity_math.c/.h` | Neue reine Funktion `prox_volume_listen_bonus`: Lautstaerke fuer einen Hoerer mit Bonus. |
| `src/core/proximity/voice_cap.h` | `voice_listen_bonus` (Reichweite kuerzen, 103) entfaellt. Neu `voice_listen_share`: welchen Anteil der Bonus-Lautstaerke ein Sprecher bekommt. `voice_listen_reference` (Bezugswert) bleibt. |
| `src/ts/proximity/ts3_proximity_audio.c` | `audio_remote_hear_range` ist wieder „gemeldete Reichweite + Bonus“. Neu `audio_remote_gain`: Lautstaerke ueber `prox_volume_listen_bonus`. |
| `src/ts/proximity/ts3_3d.c` | Kommentar aus 103 entfernt (trifft nicht mehr zu). |
| `src/ts/entry/ts3_entry.c` | Version 8.0.25. |
| `tests/audio_fx_test.c`, `tests/proximity_math_test.c`, `tests/audio_pcm_test.c` | Tests auf die neue Regel umgestellt (42 Pruefungen). |

## 2. Wie war es vorher?

- **Bis 8.0.23:** Reichweite = gemeldete Reichweite + Bonus, und die Lautstaerke lief ueber diese ganze Strecke.
  Ein Fluestern (1 Fundament) trug mit Bonus 4 also 5 Fundamente — richtig gerechnet —, war aber in 2,56 m Abstand
  82 % laut statt 1 % wie bei allen anderen. Das war die Meldung.
- **8.0.24 (Doku 103):** Die Reichweite des Fluesterns wurde gekuerzt (4,3 m statt 12,8 m). Damit war es leiser,
  aber „1 + 4 = 5 Fundamente“ stimmte nicht mehr, und die Hoer-Rassen hoerten ein Fluestern kaum noch weiter als
  alle anderen.

## 3. Warum ist die neue Loesung besser?

- **Die Fundament-Rechnung stimmt wieder.** Fluestern 1 + Bonus 4 = 5 Fundamente, wie im Root-Channel eingestellt.
  Das gilt fuer alle Modi und alle Rassen.
- **Es wird nur die Lautstaerke korrigiert** — das, worum es in der Meldung ging.
- **Ganz nah (bis etwa 1,9 m) hoert die Hoer-Rasse ein Fluestern genauso laut wie alle anderen.** Danach etwas
  lauter als die anderen (in 2 m 15 % statt 11 %, in 2,56 m 14 % statt 1 %) — aber nicht mehr „fast wie normales
  Reden“ (vorher 82 %).
- **Aus der Ferne bleibt es leise hoerbar** bis zur vollen Reichweite. „Weiter hoeren“ heisst damit: weiter, nicht
  lauter.
- **Normal und Rufen sind bis aufs Bit gleich wie vor 8.0.24** (im Test ueber 0,1 bis 60 m verglichen).

## 4. Wie funktioniert es?

```
Reichweite          = gemeldete Reichweite des Sprechers + Bonus            (einfache Summe)

Lautstaerke         = die lautere von zwei Kurven:
   Kurve A            ueber die eigene Reichweite des Sprechers             (das hoeren alle)
   Kurve B            ueber Reichweite + Bonus, mal Anteil                  (das bringt der Bonus)

Anteil              = 1                                 wenn die Reichweite >= Bezugswert ist (Normal, Rufen)
                      Reichweite / Bezugswert           wenn sie kuerzer ist (Fluestern 1 von 6 -> 1/6)
```

- Bei **Normal und Rufen** ist der Anteil 1. Kurve B ist dann ueberall die lautere — genau die alte Lautstaerke.
- Beim **Fluestern** ist der Anteil klein. Nah am Sprecher gewinnt Kurve A (so laut wie fuer alle). Ab etwa 1,9 m
  wird die leise Kurve B die lautere (Kurve A liegt dort noch bei rund 14 % und faellt dann schnell auf null) und
  traegt bis zur vollen Reichweite.
- Beide Kurven fallen mit der Entfernung, die Lautstaerke springt also nirgends nach oben.

**Der Bezugswert** ist die kuerzeste Reichweite, die normales Reden auf dem Server haben kann: das kleinste
`MinimumNormal` aus den allgemeinen Vorgaben und allen Rassen (hier 6 Fundamente = 15,36 m). Hat ein Server gar
kein `MinimumNormal`, gilt das kleinste `MaximumNormal` — ein Normal-Wert, den ein Spieler dort kleiner einstellt,
wird dann wie ein kurzer Modus behandelt. Ist auch kein Maximum gesetzt, bleibt alles wie vor 8.0.24.

**Wo es gerechnet wird** (im Plugin des Hoerenden, auf TeamSpeaks Rueckruf-Thread; die Audio-Threads bekommen nur
das fertige Ergebnis):

```
Beschreibung des Root-Channels ──► Vorgaben in Metern (Grenzen, Rassen, Bonus des eigenen Spielers)
Positions-Paket eines Sprechers ─► seine gemeldete Reichweite
        │
        ▼
audio_pass_begin          liest pro Durchgang einmal: Bonus und Bezugswert
audio_remote_hear_range   Reichweite = gemeldete Reichweite + Bonus      ──► Hoergrenze, Diagnose
audio_remote_gain         Lautstaerke = prox_volume_listen_bonus(...)    ──► was der Spieler hoert
```

### Reichweiten (1 Fundament = 2,56 m)

| Rasse | Fluestern | Normal | Rufen |
|---|---|---|---|
| ohne Bonus | 2,6 m (1 F) | 15,4 m (6 F) | 41,0 m (16 F) |
| WeiterHoehrend (+4) | 12,8 m (5 F) | 25,6 m (10 F) | 51,2 m (20 F) |
| TierischHoehrend (+6) | 17,9 m (7 F) | 30,7 m (12 F) | 56,3 m (22 F) |
| AdminHoehrend (+10) | 28,2 m (11 F) | 41,0 m (16 F) | 66,6 m (26 F) |

Das sind dieselben Werte wie bis 8.0.23. In 8.0.24 waren es beim Fluestern 4,3 / 5,1 / 6,8 m.

### Lautstaerke eines Fluesternden fuer eine Hoererin mit Bonus 4 (in % des Maximums)

| Abstand | alle anderen | bis 8.0.23 | 8.0.24 | jetzt 8.0.25 |
|---|---|---|---|---|
| 1 m | 54 % | 99 % | 76 % | 54 % |
| 2 m | 11 % | 88 % | 45 % | 15 % |
| 2,56 m | 1 % | 82 % | 31 % | 14 % |
| 4 m | 0 % | 64 % | 2 % | 11 % |
| 6 m | 0 % | 45 % | 0 % | 7 % |
| 8 m | 0 % | 28 % | 0 % | 5 % |
| 10 m | 0 % | 11 % | 0 % | 2 % |
| 12 m | 0 % | 2 % | 0 % | unter 1 % |

Bei Bonus 6 liegt das Fluestern jenseits von 2 m bei 16 bis 1 % (bis 17,9 m), bei Bonus 10 bei 17 bis 1 % (bis
28,2 m).

## 5. Wie wurde es getestet?

- `proximity_math_test` (12 Pruefungen) mit der echten Lautstaerke-Kurve: vorher ueber 80 % bei 2,56 m, jetzt rund
  14 %; weiterhin besser als ohne Bonus; in 1 m exakt so laut wie fuer alle; in 8 m und 12 m noch hoerbar, in 15 m
  stumm; **Normal und Rufen ueber 0,1 bis 60 m bitgleich mit der alten Kurve**; ohne Bonus und mit Anteil 0 die
  einfache Kurve; Anteil ueber 1 wird als 1 behandelt; ueber 0,1 bis 30 m an jeder Stelle: nie leiser als ohne
  Bonus, nie lauter als normales Reden, nie lauter bei groesserem Abstand.
- `audio_fx_test` (16 Pruefungen): Bezugswert in allen Faellen (ohne Profil, nur Maximum, mit Minimum, Rasse mit
  kleinerem Minimum, ganz ohne Minimum, Minimum gleich Maximum); Anteil 1/6 fuer Fluestern, 1 fuer Normal und
  Rufen, 1/2 bei halber Reichweite, 1 ohne Bezugswert, 0 ohne Reichweite; ueber 0 bis 100 m bleibt der Anteil
  zwischen 0 und 1 und waechst mit der Reichweite.
- `audio_pcm_test` (14 Pruefungen) im **echten Rechenweg** des Plugins: Reichweiten 12,8 / 25,6 / 51,2 m (einfache
  Summe); 1000 m gemeldet → erst gekappt, dann Bonus; veroeffentlichte Lautstaerke: Fluestern in 1 m wie fuer
  alle, in 2,56 m ein Sechstel der alten, in 8 m leise hoerbar, in 15 m stumm; Normal in 20 m und Rufen in 45 m mit
  der vollen alten Lautstaerke; Server mit Normal 10 bis 20 m: 10 m und 13 m werden nicht wie Fluestern behandelt;
  ohne Profil alles wie frueher.
- Alle Host-Tests (`tests/run_tests.sh`, MSYS2-gcc): 33 Gruppen gruen; die drei bekannten kompilieren auf dem
  Windows-gcc weiterhin nicht (betrifft keine geaenderte Datei).
- MSVC-Release-Build fehlerfrei.

**Nicht geprueft:** im echten TeamSpeak mit zwei Spielern. Ob 14 % in 2,56 m als „angenehm leise, aber
verstaendlich“ empfunden wird, kann nur ein Hoertest zeigen.

## 6. Was man wissen muss

- Die Regel rechnet im Plugin des **Hoerenden**. Es reicht, wenn die Spieler mit Hoer-Rasse 8.0.25 haben.
- Wer 8.0.24 hat, hoert Fluestern nur 4,3 / 5,1 / 6,8 m weit. Mit 8.0.25 sind es wieder 12,8 / 17,9 / 28,2 m.
- Ein Fluestern ist fuer Hoer-Rassen wieder ueber die volle Strecke mitzuhoeren — leise. Das ist die Folge von
  „Reichweite wie eingestellt“.
- Die Staerke haengt am Verhaeltnis Fluestern zu Normal im Root-Channel (hier 1 zu 6). Wird Fluestern dort
  groesser gestellt, wird der leise Teil lauter.
- Diagnose: Plugins → Conan Exiles → „Diagnose-Schnappschuss ins Log schreiben“. `range` ist wieder
  `announced + listenAdd`; `gain` zeigt die tatsaechliche Lautstaerke, `listenRef` den Bezugswert.

## 7. Lerneffekt

Die erste Korrektur hat an der Zahl gedreht, die am leichtesten zu aendern war (Reichweite), nicht an der, um die
es ging (Lautstaerke). Wenn zwei Groessen an einer Zahl haengen, zuerst klaeren, welche davon gemeint ist.
