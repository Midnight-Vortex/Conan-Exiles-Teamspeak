# 074 — Okklusion v2: Mod-Seite (`Vo_PosStreamer_Actor`, nicht in Git)

Umsetzung von 072 Abschnitt 5 im DevKit (2026-10-20). Plugin-Seite: 073 (8.0.11).
Umgesetzt: A1, A2, A3, A4, A6, A7, A9, A10, Budget, v2-Format. Nicht gewuenscht: A5, A8.

## 1. Was wurde geaendert?

| Teil | Aenderung |
|---|---|
| Neue Variablen | `OccCfg` (Werte aus `occcfg`), `OccV2` (Plugin kann v2), `OccTick` (Takt 0/1) |
| Neue Funktion `OccCfgVal(Index, Default)` (pure) | Wert `i` aus `occcfg`, sonst der Standard — so bleibt die Mod auch mit einem 8.0.10-Plugin lauffaehig |
| `OccFan(Eye, Target, Edge) -> Frac, Thick, FloorZ` | **A2:** Aussenstrahlen enden am Sprecher bei ±`OccFanEdge` (am Zuhoerer ±35 cm). **A7:** zusaetzlicher Strahl vom Sprecher zurueck, `Thick` = Abstand der beiden Treffer (999 = unbekannt). **A6:** `FloorZ` = \|Normale.Z\| des Mitteltreffers |
| `OccRoom(Pos)` | **A9:** 9 Strahlen (hoch 0,4 + 8 waagerecht je 0,075) |
| Neue Funktion `OccDetour(Eye, Target, Near, Far) -> Found, Relay` | **A1:** 8 Hilfspunkte (⅓ und ⅔ der Strecke, ±Near, dann ±Far seitlich), je 2 Strahlen, Abbruch beim ersten freien Weg |
| `UpdateOcclusion` | liest `occcfg`; eigenes Gebaeude unter den Fuessen (`OwnFloor`); Faecher fuer so viele Sprecher wie das Budget erlaubt (`OccBudget` 60 → 4, 15 → 1); Umweg ab Faecher ≥ 0,6; **A4** Gebaeude unter dem Sprecher; **A3** Distanzgewicht; Art N/T/D; v2-Eintrag (v1 ohne `occcfg`) |
| EventGraph | Okklusions-Tor 0,1 s → 0,05 s (**A10:** 20 Hz; Positionstimer laeuft alle 30 ms). In jedem zweiten Takt nur die `OccFast` naechsten Sprecher |

Formel (Art T): `100 × Faecher × (0,5 + 0,5 × Raum) × Distanzgewicht × Boden-Gewicht × Duenn-Gewicht`,
im selben Gebaeude gedeckelt auf `OccSameBuilding`. Art D: `OccDetourNearPct` bzw. `OccDetourFarPct` × Distanzgewicht.

## 2. DevKit-Hinweise

- Ohne Operator-Knoten gebaut (Absturzgefahr beim Anlegen per Skript): Vektor + Vektor =
  `TransformLocation(MakeTransform(...))`, Summen = `VSizeSquared(MakeVector(√a, √b, √c))`, a × b = `Lerp(0, a, b)`.
- „BP Assist“ setzt nach dem Speichern Umleitungsknoten — Skripte muessen Verbindungen durch `K2Node_Knot` verfolgen.
- Eine Signaturaenderung (neue Pins) macht bestehende Aufrufknoten ungueltig; es gibt keine Neuaufbau-API →
  alten Aufruf loeschen, speichern, neu setzen.
- Funktionen, die nur als Wert genutzt werden, muessen pure sein (sonst wird der Aufruf verworfen + Warnung).

## 3. Wie wurde es getestet? (PIE, Kloetze aus `/Engine/BasicShapes/Cube`)

```
Station A (Zuhoerer im Freien):
  21 Ecke 2 m breit           D:30 (Umweg 1,5 m, Punkt 3150,-3600,418)
  22 Pfosten 30 cm             T:8   (Faecher 0,2 × 0,5 × duenn 0,8)
  23 Wand 8 m breit            T:43  (40 + Raum 7 % durch eine Gelaendewelle)
  24 Huette geschlossen        T:80  Raum 100
  25 frei (5. Sprecher)        N:0   (Einzelstrahl)
  26 hinter der Wand (6.)      T:50  (Einzelstrahl, Raum 0)
  Takt 1: nur 21, 22 (OccFast 2) · Takt 2: alle 6          -> A10
  ohne occcfg: "21:30;22:8;..."                            -> v1-Rueckfall
  OccFan unter einem Dach: Frac 1, Thick 20, FloorZ 1      -> A6
Station B (Zuhoerer und Sprecher auf derselben Bodenplatte, Sprecher in der Huette):
  31 T:60 statt 80                                         -> A4
```

**Laufzeit** (Editor-PIE, Blueprint-VM, inkl. Python-Aufruf): **0,40 ms** pro Aufruf mit 6 Sprechern
(4 Faecher + Umweg-Suche), 0,17 ms im Zwischentakt mit 2 Sprechern. Im ausgelieferten Spiel eher weniger.

**Noch nicht** im echten Spiel mit Mitspielern gehoert. Mod muss gekocht werden; danach 8.0.11 zur „Latest“ machen.
