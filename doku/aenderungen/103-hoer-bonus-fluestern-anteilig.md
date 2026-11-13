# 103 — Hoer-Bonus („weiter hoeren“): Fluestern bekommt nur noch seinen Anteil (Plugin 8.0.24)

**Plugin 8.0.24**, 2026-11-13. Gebaut und getestet, **nicht verteilt** (kein Commit, kein Release, nichts in
TeamSpeak kopiert).

Meldung einer Spielerin mit der Rasse „WeiterHoehrend“: *„Durch das weitere Hoeren hoere ich Leute auf Fluestern
lauter, als wenn die auf Normal stehen.“*

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/proximity/voice_cap.h` | Neue reine Funktionen: `voice_listen_reference` (die kuerzeste Normal-Reichweite des Servers, an der der Bonus gemessen wird) und `voice_listen_bonus` (wie viel Bonus ein Sprecher bekommt). |
| `src/ts/proximity/ts3_proximity_audio.c` | `audio_remote_hear_range` rechnet die Reichweite eines Sprechers jetzt mit `voice_listen_bonus` statt mit dem vollen Bonus. Das ist die eine Stelle, aus der Lautstaerke, Hoergrenze und Diagnose-Schnappschuss ihre Reichweite nehmen. Die Diagnose-Zeilen zeigen zusaetzlich den Bezugswert (`listenRef`). |
| `src/ts/proximity/ts3_3d.c` | Nur ein Kommentar: Die aeussere Grenze fuer TeamSpeaks 3D-Positionen rechnet absichtlich weiter mit dem vollen Bonus (zu weit schadet dort nicht; die Lautstaerke kommt nicht von dort). |
| `src/core/proximity/proximity_math.c` | Nur ein Kommentar an einer alten, vom Plugin nicht mehr benutzten Funktion: Sie rechnet noch mit dem vollen Bonus. |
| `src/ts/entry/ts3_entry.c` | Version 8.0.24. |
| `tests/audio_fx_test.c`, `tests/proximity_math_test.c`, `tests/audio_pcm_test.c` | 38 neue Pruefungen. |

Die Werte im Root-Channel (`listenAddDistance`, `MinimumWisper` …) bleiben, wie sie sind.

**Begriffe:** *Root-Channel* = der oberste TeamSpeak-Channel; in seiner Beschreibung stehen die Server-Vorgaben
fuer das Plugin. *Hoer-Rasse* = ein `[RACE]`-Eintrag dort mit `listenAddDistance`; wer per SteamID dazugehoert,
hoert weiter. *Reichweite* = wie weit die Stimme eines Sprechers in seinem Modus (Fluestern, Normal, Rufen) traegt.
*Fundament* = Laengenmass der Vorgaben, 2,56 m.

## 2. Wie war es vorher?

`listenAddDistance` ist ein **fester Zuschlag**: Der Hoerende rechnet auf die Reichweite jedes Sprechers dieselbe
Strecke drauf. Die Lautstaerke haengt aber davon ab, wie weit jemand **im Verhaeltnis zur Reichweite** entfernt
steht — am Rand leise, in der Mitte laut. Derselbe Zuschlag wirkt deshalb bei einer kurzen Reichweite viel
staerker als bei einer langen.

Mit den Werten des Servers (1 Fundament = 2,56 m; Fluestern 1, Normal 6, Rufen 16 Fundamente; Bonus 4):

| Modus | ohne Bonus | mit Bonus (alt) | Reichweite wurde |
|---|---|---|---|
| Fluestern | 2,56 m | 12,8 m | 5-mal so gross |
| Normal | 15,4 m | 25,6 m | 1,7-mal so gross |
| Rufen | 41 m | 51 m | 1,25-mal so gross |

Ein Fluesternder in 2,56 m Abstand stand fuer alle anderen am Rand seiner Reichweite (1 % Lautstaerke). Fuer die
Spielerin stand er mitten in 12,8 m — sie hoerte ihn mit 82 %, fast wie normales Reden. Ein Rechenfehler war das
nicht: Am selben Platz war Normal nie leiser als Fluestern. Der Eindruck „lauter“ entstand, weil Fluesternde nah
stehen und Normal-Sprecher meist weiter weg.

## 3. Warum ist die neue Loesung besser?

- **Fluestern bleibt Fluestern.** Wer mit Bonus hoert, hoert ein Fluestern weiterhin besser als alle anderen,
  aber nicht mehr fast in voller Lautstaerke.
- **Normal und Rufen aendern sich nicht.** Dort gilt der volle Bonus wie bisher.
- **Keine neue Einstellung.** Der Anteil ergibt sich aus den Werten, die der Server ohnehin vorgibt.
- **Wer fluestert, ist wieder geschuetzt.** Neben einer Hoer-Rasse trug ein Fluestern bisher 12,8 m (Bonus 4),
  17,9 m (Bonus 6) oder 28,2 m (Bonus 10). Jetzt sind es 4,3 m, 5,1 m und 6,8 m.

## 4. Wie funktioniert es?

```
Bonus fuer einen Sprecher =
   voller Bonus                              wenn seine Reichweite >= Bezugswert ist
   Bonus x (seine Reichweite / Bezugswert)   wenn sie kuerzer ist
```

**Der Bezugswert** ist die kuerzeste Reichweite, die normales Reden auf dem Server haben kann: das kleinste
`MinimumNormal` aus den allgemeinen Vorgaben und allen Rassen. So verliert normales Reden (und Rufen) nie Bonus,
auch wenn ein Server einen Bereich erlaubt (z. B. Normal 10 bis 20 m) und ein Spieler den kleinsten Wert waehlt.
Hat ein Server gar kein `MinimumNormal`, gilt das kleinste `MaximumNormal` — ein Normal-Wert, den ein Spieler dort
kleiner einstellt, bekommt dann nur seinen Anteil (er ist von einem Fluestern nicht zu unterscheiden). Ist auch
kein Maximum gesetzt, bleibt es beim vollen Bonus wie frueher. „Seine Reichweite“ ist die, die der Sprecher fuer seinen aktuellen Modus meldet.

Auf diesem Server sind Minimum und Maximum gleich (6 Fundamente), der Bezugswert ist also 15,36 m.

**Wo es gerechnet wird** (alles im Plugin des Hoerenden, auf TeamSpeaks Rueckruf-Thread; die Audio-Threads
bekommen nur das fertige Ergebnis):

```
Beschreibung des Root-Channels ──► Vorgaben in Metern (Grenzen, Rassen, Bonus des eigenen Spielers)
Positions-Paket eines Sprechers ─► seine gemeldete Reichweite
        │
        ▼
audio_pass_begin        liest pro Durchgang einmal: Bonus und Bezugswert
audio_remote_hear_range Reichweite = gemeldete Reichweite + voice_listen_bonus(...)
        │
        ├─► Hoergrenze (ist der Sprecher ueberhaupt in Reichweite?)
        ├─► Lautstaerke (Abstand im Verhaeltnis zur Reichweite)
        └─► Diagnose-Schnappschuss im plugin.log
```

Beispiel Fluestern: 1 Fundament von 6 → ein Sechstel des Bonus.

| Rasse | Modus | vorher | nachher |
|---|---|---|---|
| WeiterHoehrend (+4) | Fluestern | 12,8 m | **4,3 m** |
| | Normal / Rufen | 25,6 m / 51,2 m | unveraendert |
| TierischHoehrend (+6) | Fluestern | 17,9 m | **5,1 m** |
| | Normal / Rufen | 30,7 m / 56,3 m | unveraendert |
| AdminHoehrend (+10) | Fluestern | 28,2 m | **6,8 m** |
| | Normal / Rufen | 41,0 m / 66,6 m | unveraendert |

Lautstaerke eines Fluesternden fuer eine Hoererin mit Bonus 4 (in % des Maximums):

| Abstand | ohne Bonus | vorher | nachher | zum Vergleich: Normal |
|---|---|---|---|---|
| 1 m | 54 % | 99 % | 76 % | 100 % |
| 2 m | 11 % | 88 % | 45 % | 99 % |
| 2,56 m | 1 % | 82 % | 31 % | 97 % |
| 4 m | 0 % | 64 % | 2 % | 88 % |
| 6 m | 0 % | 45 % | 0 % | 76 % |

Stellt der Server Fluestern spaeter auf 2 Fundamente, bekommt es zwei Sechstel des Bonus — der Anteil folgt den
Werten von selbst.

## 5. Wie wurde es getestet?

- `audio_fx_test` (19 neue Pruefungen): Bezugswert ohne Profil / ohne Grenzen / nur Maximum / mit Minimum /
  Rasse mit kleinerem Minimum / ganz ohne Minimum / Minimum gleich Maximum; ein Sechstel fuer Fluestern (4,27 m
  statt 12,8 m); voller Bonus bei Normal und Rufen; halbe Reichweite → halber Bonus; kein Bezugswert → voller
  Bonus; Bonus 0 oder negativ → 0; Sprecher ohne Reichweite → 0; ueber 0 bis 100 m bleibt der Bonus zwischen 0 und
  dem eingestellten Wert, und eine laengere Reichweite ergibt nie eine kuerzere Gesamt-Reichweite.
- `audio_pcm_test` (12 neue Pruefungen) im **echten Rechenweg** des Plugins (schlaegt fehl, wenn die Regel dort
  wieder herausgenommen wird): Fluestern 4,27 m, Normal 25,6 m, Rufen 51,2 m; ein gemeldeter Wert von 1000 m wird
  erst gekappt, dann kommt der Bonus dazu; die veroeffentlichte Lautstaerke folgt (Fluestern in 5 m stumm, Normal
  in 20 m wie bei 25,6 m Reichweite); Server mit Normal 10 bis 20 m: 10 m und 13 m behalten den vollen Bonus;
  Rasse mit kuerzerem Normal; ohne Profil voller Bonus wie frueher.
- `proximity_math_test` (7 neue Pruefungen) mit der echten Lautstaerke-Kurve: alte Regel ueber 80 % bei 2,56 m (der
  gemeldete Effekt), neue Regel unter 35 %; mit Bonus weiterhin besser als ohne; Normal am selben Platz ueber 95 %;
  Normal behaelt 25,6 m; Fluestern bei 5 m stumm; ueber 0,1 bis 30 m gilt an jeder Stelle: Fluestern mit Bonus >=
  ohne Bonus und <= Normal mit Bonus.
- Alle Host-Tests (`tests/run_tests.sh`, MSYS2-gcc): 33 Gruppen gruen; die drei bekannten kompilieren auf dem
  Windows-gcc weiterhin nicht (betrifft keine geaenderte Datei).
- MSVC-Release-Build fehlerfrei.
- **Gegenpruefung** durch unabhaengige Pruefer (Logik und betroffene Stellen; Tests und Zahlen dieser Doku; jeder
  Befund einzeln nachgeprueft): 7 Befunde, 4 bestaetigt, 3 widerlegt. Eingearbeitet: der Bezugswert (vorher
  `MaximumNormal` — ein Spieler mit Normal unter dem Maximum haette Bonus verloren), der Test im echten
  Rechenweg, der Bezugswert in den Diagnose-Zeilen, die Erklaerungen in dieser Doku. Alle Zahlen dieser Doku
  wurden dabei nachgerechnet und stimmen.

**Nicht geprueft:** im echten TeamSpeak mit zwei Spielern. Ein Messprotokoll der Spielerin lag nicht vor; die
Ursache ist aus dem Code und den Root-Werten nachgerechnet.

## 6. Was man wissen muss

- Die Regel rechnet im Plugin des **Hoerenden**. Es reicht, wenn die Spieler mit Hoer-Rasse 8.0.24 haben; fuer
  alle anderen aendert sich nichts.
- Die Regel kennt keine Zonen. Sie misst nur, was der Sprecher meldet. Die Zonen-Werte (`Wisper`, `Normal`,
  `Shout`) stellt der Sprecher selbst ein und begrenzt sie danach auf seine `Minimum…`/`Maximum…`-Vorgaben; auf
  diesem Server sind diese gleich (1 / 6 / 16), die Zonen-Werte aendern die gemeldete Reichweite also nicht.
- Spieler mit unterschiedlichen Plugin-Versionen hoeren ein Fluestern unterschiedlich weit: mit 8.0.24 nach der
  neuen Regel, mit aelteren nach der alten. Am Sprecher aendert sich nichts.
- Wer nachmessen will: Plugins → Conan Exiles → „Diagnose-Schnappschuss ins Log schreiben“. Die Kopfzeile zeigt
  Bonus (`listenAdd`) und Bezugswert (`listenRef`), die Zeile je Sprecher gemeldete Reichweite (`announced`),
  Reichweite mit Bonus (`range`) und Lautstaerke (`gain`). Bei einem Fluesternden ist `range` jetzt kleiner als
  `announced + listenAdd` — das ist die neue Regel, kein Fehler.

## 7. Lerneffekt

Ein fester Zuschlag ist nur auf dem Papier „fuer alle gleich“. Sobald das Ergebnis im Verhaeltnis zu einer Groesse
gerechnet wird, wirkt er auf kleine Werte um ein Vielfaches staerker.
