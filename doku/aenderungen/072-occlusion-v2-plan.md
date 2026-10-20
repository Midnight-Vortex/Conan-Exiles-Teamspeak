# 072 — Okklusion v2: Plan, Protokoll und Parameter (Plugin + Mod)

**Status (2026-10-20 05:50):** Plugin-Seite umgesetzt (8.0.11, siehe 073), Mod-Seite umgesetzt und in PIE getestet (siehe 074). Offen: Mod kochen, Test mit Mitspielern, 8.0.11 zur „Latest“ machen.
Auftrag: alle Ideen aus der Liste umsetzen **ausser A5 (Tueren leichter) und A8 (Material)**.

| Nr. | Idee | Seite |
|---|---|---|
| A1 | Umweg-Pruefung (Schallweg um die Ecke / durch offene Tuer) | Mod |
| A2 | Weichere Kante (Faecher am Sprecher breiter) | Mod |
| A3 | Entfernungsgewichtung (nah = kaum Daempfung) | Mod |
| A4 | Gleiches Gebaeude (Innenwaende gedeckelt) | Mod |
| A6 | Decken/Boeden leichter | Mod |
| A7 | Wanddicke (duenne Einzelwand leichter) | Mod |
| A9 | Raum-Faktor mit 8 Richtungen | Mod |
| A10 | 20 Hz fuer die naechsten Sprecher | Mod |
| B1 | Zeitliche Glaettung | Plugin |
| B2 | Beugung ≠ Durchdringung (eigene Klangkurve) | beide |
| B3 | Richtung aus der Oeffnung (HRTF/Panorama vom Umweg-Punkt) | beide |
| B4 | Hall drinnen aus dem Raum-Faktor | beide |
| B5 | Stellschrauben im Root-Channel, Weitergabe an die Mod | beide |
| C1 | Debug-Log (Standard aus) | Plugin |
| C2 | Testparcours (PIE-Kloetze, spaeter auf dem Server) | Test |

## 1. Abwaertskompatibilitaet

- Ein Plugin **8.0.10** liest einen erweiterten Eintrag nur bis zum ersten `:` nach dem Prozentwert und bricht
  dann ab — es wuerde nur den ersten Spieler uebernehmen. Deshalb sendet die Mod das v2-Format **nur**, wenn die
  Plugin-Antwort `"occcfg":"2,` enthaelt. Sonst bleibt sie beim v1-Format `cid:pct`.
- Ein Plugin **8.0.11** liest beide Formate (v1-Eintraege zaehlen als Art `T`, Raum 0).

## 2. Plugin → Mod: `occcfg` (in jeder Antwort mit Probes)

```
{"ok":true,"probe":"...","occcfg":"2,80,150,400,150,300,30,60,60,70,80,60,2","talk":0,"talkers":"..."}
```

Feste Reihenfolge (Ganzzahlen):

| Index | Root-Channel-Schluessel | Standard | Bereich | Bedeutung |
|---|---|---|---|---|
| 0 | — | 2 | — | Protokollversion |
| 1 | `OccFanEdge` | 80 | 0..200 cm | seitlicher Versatz der Aussenstrahlen **am Sprecher** (am Zuhoerer bleiben 35 cm) |
| 2 | `OccNear` | 150 | 0..1000 cm | darunter keine Daempfung |
| 3 | `OccFar` | 400 | 0..2000 cm | ab hier volle Daempfung (dazwischen linear) |
| 4 | `OccDetourNear` | 150 | 50..500 cm | seitlicher Versatz der nahen Umweg-Punkte |
| 5 | `OccDetourFar` | 300 | 50..800 cm | seitlicher Versatz der fernen Umweg-Punkte |
| 6 | `OccDetourNearPct` | 30 | 0..100 % | Beugungsstaerke bei nahem Umweg |
| 7 | `OccDetourFarPct` | 60 | 0..100 % | Beugungsstaerke bei fernem Umweg |
| 8 | `OccSameBuilding` | 60 | 0..100 % | Deckel fuer Innenwaende im selben Gebaeude |
| 9 | `OccFloor` | 70 | 0..100 % | Gewicht, wenn der Blocker Boden/Decke ist |
| 10 | `OccThin` | 80 | 0..100 % | Gewicht fuer eine duenne Einzelwand (≤ 40 cm) |
| 11 | `OccBudget` | 60 | 20..200 | Strahlen pro Takt (Rest wird verschoben) |
| 12 | `OccFast` | 2 | 0..4 | so viele naechste Sprecher mit 20 Hz |

## 3. Mod → Plugin: OCC v2

```
OCC=<cid>:<pct>:<art>:<raum>:<x>,<y>,<z>;...
```

| Feld | Bedeutung |
|---|---|
| `pct` | 0..100 Endstaerke (Art T: Durchdringung inkl. A3/A4/A6/A7 und Raum-Faktor; Art D: Beugung inkl. A3) |
| `art` | `N` frei · `T` durch die Wand · `D` um die Ecke / durch eine Oeffnung |
| `raum` | 0..100 = 100 × max(eigener Raum, Raum des Sprechers) — fuer den Hall (B4) |
| `x,y,z` | nur bei `D`: Umweg-Punkt in Unreal-Achsen (cm); sonst `0,0,0` |

## 4. Plugin: Klang

| Art | Lautstaerke | Tiefpass | Richtung |
|---|---|---|---|
| N | — | — | Sprecher |
| T | wie 8.0.10 (`OcclusionIntensity`) | wie 8.0.10 | Sprecher |
| D | bis −3 dB × Staerke | 19 kHz → 2,5 kHz (nie unter 1,2 kHz) | **Umweg-Punkt** (B3) |

- **B1 Glaettung:** Ziel-Lautstaerkefaktor und Ziel-Tiefpass werden zeitlich angenaehert — dumpfer werden mit
  `OcclusionAttackMs` (Standard 500), klarer werden mit `OcclusionReleaseMs` (Standard 250). Damit sind auch
  Wechsel zwischen T und D weich.
- **B4 Hall:** Hall-Anteil = Raum × `OcclusionReverb` (Standard 100 %, 0 = aus), mit dem vorhandenen Hall der
  Hoehlen-Zonen.
- **C1 Debug:** `plugin.cfg` → `OcclusionDebug=1` loggt pro Sprecher einmal pro Sekunde Art, Staerke, Raum,
  geglaetteten Faktor und Tiefpass. Standard aus.

Root-Channel-Schluessel des Plugins: `OcclusionIntensity` (069), `OcclusionAttackMs`, `OcclusionReleaseMs`,
`OcclusionDiffraction` (0..300 %, Standard 100), `OcclusionReverb` (0..200 %, Standard 100).

## 5. Mod: Ablauf pro Takt (Ziel)

```
OwnRoom = OccRoom(Augen)            8 Richtungen (A9)
fuer Sprecher i (naechste zuerst, Budget beachten):
  Frac = OccFan(...)                 Aussenstrahlen am Sprecher ±OccFanEdge (A2)
  Frac == 0            -> N, pct 0
  sonst Umweg suchen (A1, nur i < 4): Punkte bei 1/3 und 2/3 der Strecke, ±DetourNear / ±DetourFar seitlich,
                                       je 2 Strahlen, Abbruch beim ersten freien Weg
     gefunden          -> D, pct = DetourPct × Distanzgewicht (A3), x,y,z = Umweg-Punkt
     nicht gefunden    -> T, pct = 100 × Frac × (0,5 + 0,5 × Raum) × Distanzgewicht (A3)
                                 × Boden-Gewicht (A6) × Duenn-Gewicht (A7), gedeckelt durch A4
```

## 6. Umsetzungsreihenfolge

1. Plugin: OCC-v2-Parser + Speicher (Art, Raum, Umweg-Punkt), `occcfg` in der Antwort, neue Root-Schluessel, Tests.
2. Plugin: Glaettung, Beugungskurve, Richtung vom Umweg-Punkt, Hall, Debug-Log, Tests, Build.
3. Mod (DevKit mit geladener Teamspeak-Mod): Parameter aus `occcfg`, A1–A10 ohne A5/A8, v2-Format.
4. PIE-Testparcours (Ecke, offene/geschlossene Tuer, Stockwerk, duenne/dicke Wand), Performance messen.
5. Release 8.0.11 + Mod kochen (Owner).
