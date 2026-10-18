# 071 — Okklusion Mod-Seite: Wand-Faecher + Raum-Faktor

**Nur Mod-Seite** (`Vo_PosStreamer_Actor` im DevKit, nicht in Git). Das Plugin bleibt unveraendert:
es nimmt schon seit 061 Zwischenwerte `OCC=<id>:0..100` an und daempft stufenlos
(Staerke weiter ueber `OcclusionIntensity`, 069).

## 1. Was wurde geaendert?

| Teil | Aenderung |
|---|---|
| Neue Funktion `OccFan(Eye, Target) -> Frac` | 5 parallele Strahlen vom Kopf des Zuhoerers zum Kopf des Sprechers: Mitte + X-Muster (±35 cm seitlich, ±25 cm hoch, im Blick-Koordinatensystem). `Frac` = Anteil blockierter Strahlen (0 / 0,2 / … / 1) |
| Neue Funktion `OccRoom(Pos) -> Room` | 5 kurze Strahlen: hoch 6 m (Gewicht 0,4), ±X / ±Y je 8 m (je 0,15). `Room` = Summe der getroffenen Gewichte (0 = frei, 1 = Dach + 4 Waende) |
| `UpdateOcclusion` | vor der Schleife `OwnRoom = OccRoom(eigene Augen)`; fuer die **4 naechsten** Spieler `Frac = OccFan(...)` und — nur wenn etwas blockt — `SRoom = OccRoom(Sprecherkopf)`; weitere Spieler: ein Strahl wie bisher (`Frac` = 0/1, `SRoom` = 0) |
| Formel | `OCC = 100 × Frac × (0,5 + 0,5 × max(OwnRoom, SRoom))` |
| Kollisionskanal | **Camera** (`TraceTypeQuery2`) statt Visibility, fuer alle Strahlen |

## 2. Wie war es vorher?

Ein einziger Strahl auf dem Visibility-Kanal, Ergebnis nur 0 oder 100. Jeder Blocker dämpfte voll:
ein Pfosten genauso wie eine Wand. Figuren blocken Visibility (`CharacterMesh0`, Typ `HITBOX`),
also konnte auch ein Mensch zwischen zwei Spielern dämpfen. Ein geschlossener Raum dämpfte nicht
staerker als eine einzelne Wand im Freien.

## 3. Warum ist die neue Loesung besser?

| Situation | vorher | jetzt |
|---|---|---|
| Pfosten / Baumstamm / Querbalken im Freien | 100 | **10** |
| Person dazwischen | 100 | **0** (Camera-Kanal ignoriert Figuren) |
| Einzelne Wand im Freien | 100 | **50** |
| Sprecher in geschlossenem Raum, Zuhoerer draussen | 100 | **100** |
| Beide im selben Raum, freie Sicht | 0 | **0** |

- **Camera-Kanal:** Conans Kamera kollidiert mit Gebaeuden (`BUILDING_STRUCTURE`) und Gelaende, aber
  nicht mit Figuren — genau „feste Welt ohne Personen“.
- **X-Muster:** Ein senkrechter Pfosten oder ein waagerechter Balken trifft nur den Mittelstrahl;
  mit einem Plus-Muster (Hoehenstrahlen ueber dem Mittelstrahl) haette ein Pfosten 3 von 5 Strahlen geblockt.
- **Raum-Faktor:** Eine Wand im Freien zaehlt halb; erst wenn Zuhoerer oder Sprecher von Dach/Waenden
  umgeben sind, gibt es die volle Daempfung.

## 4. Wie funktioniert es? (Performance)

```
10x pro Sekunde (wie bisher):
  OwnRoom   5 Strahlen
  Spieler 1-4 (naechste zuerst):  Faecher 5 Strahlen  (+5 Raum-Strahlen nur wenn etwas blockt)
  Spieler 5-16:                   1 Strahl
```

Typisch (2–3 Spieler in der Naehe): 15–35 Strahlen pro Takt, also so viel wie vorher bei 16 Spielern.
Obergrenze: 5 + 4×10 + 12 = 57 Strahlen pro Takt. Ein Line-Trace kostet wenige Mikrosekunden.

**DevKit-Hinweis:** Die Knoten sind ohne Operator-Knoten gebaut (`+`, `×` per Skript bringen den
Editor zum Absturz): Vektor-Addition = `TransformLocation(MakeTransform(...))`, Summe der 5 Treffer =
`VSizeSquared(MakeVector(VSize(MakeVector(h1,h2,h3)), h4, h5))`, Prozent = `Round(Lerp(0, MapRange(...), Frac))`.

## 5. Wie wurde es getestet?

PIE (Dedicated Server + Client), Testklotze aus `/Engine/BasicShapes/Cube`, Figur per Server teleportiert,
`UpdateOcclusion` direkt mit einer Plugin-Antwort mit 5 Sprechern aufgerufen:

```
OccString = 11:0;12:10;13:50;14:100;15:50
  11 freie Sicht 0 | 12 Pfosten 30x30 cm 10 | 13 Wand 4 m im Freien 50 | 14 Sprecher in Huette 100
  15 (5. Spieler, Einzelstrahl hinter der Wand) 50
OccFan selber Raum 0.0 | OccFan Pfosten 0.2 | OccRoom frei 0.0 | OccRoom Huette 1.0
```

Vorher im selben Test: Camera-Kanal ignoriert die eigene Figur, Gebaeude (`InstancedBuildingComponent`)
blocken. **Noch nicht** im echten Spiel mit Mitspielern gehoert; die Mod muss neu gekocht werden.

## 6. Lerneffekt

Fuer „wie stark verdeckt“ reicht kein einzelner Strahl: Breite (Faecher) und Umgebung (Raum) sind zwei
getrennte, billige Messungen. Erst messen, welcher Kollisionskanal Figuren ignoriert, dann bauen.
