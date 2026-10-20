# 076 — Okklusion: Fixes nach dem Spieltest (nah an der Wand, Hall nur in natuerlichen Raeumen)

**Nur Mod-Seite** (`Vo_PosStreamer_Actor`, nicht in Git), 2026-10-20 abends. Plugin unveraendert (8.0.12).

## 1. Rueckmeldung aus dem Test

1. Die Okklusion griff nicht, wenn der Sprecher „zu nah“ an einer Wand stand.
2. Der Raumhall wirkte in kleinen gebauten Huetten (3×3×1) merkwuerdig.
3. Wer von aussen nah an einer Wand stand, wurde teils als „im Raum“ erkannt und bekam Hall.
4. Frage: Hall nach Raumgroesse statt nur „ringsum zu“?

## 2. Ursachen und Aenderungen

| Nr. | Ursache | Aenderung |
|---|---|---|
| 1 | Die Distanzgewichtung A3 (unter `OccFar` = 4 m schwaecher) galt auch fuer Waende — bei 2,2 m Abstand blieben ~28 %. Ausserdem endete der Einzelstrahl (ab dem 5. Sprecher) noch 1 m **vor** dem Sprecher und uebersah eine Wand direkt bei ihm | A3 gilt nur noch fuer Art D (um die Ecke). Der Einzelstrahl endet am Kopf des Sprechers (der Kamera-Kanal ignoriert Figuren, der 1-m-Abstand ist nicht mehr noetig) |
| 2, 3 | Das Raum-Feld des OCC-Eintrags (das Plugin nutzt es nur fuer den Hall) zaehlte jede Wand, auch gebaute | `OccRoom` misst zusaetzlich den **natuerlichen** Raum: Treffer auf Conan-Bauteilen (`BuildableBase` = Gebaeudeteile + Platzierbares) zaehlen nicht. Ergebnis in `OccLastNat`; das Raum-Feld = max(eigener, Sprecher-) natuerlicher Raum |
| 4 | — | Der natuerliche Raum wird mit der **Raumgroesse** gewichtet: mittlerer Abstand der 8 waagerechten Strahlen (Fehlstrahl = 8 m), 0 bei ≤ 2,5 m, 1 ab 7 m |

Die Okklusion selbst (Daempfung) nutzt weiter den vollen Raum-Faktor inkl. Gebaeuden — drinnen in einem Haus
bleibt es dumpfer als draussen.

## 3. PIE-Test

| Fall | Raum (Daempfung) | Hall-Wert |
|---|---|---|
| kleine Huette 4,2 m (Kloetze) | 1,0 | **0** (zu klein) |
| grosse Hoehle 12 m (Kloetze) | 0,7 | **0,68** |
| freies Feld | 0 | 0 |
| echtes Conan-Gebaeude (`BP_BuildFoundation_C_0`) | 0,6 | **0** (gebaut) |
| Huetten-Sprecher an Station A | T:80 | Raum-Feld 0 (vorher 100) |
| du 1,2 m vor der Wand, Sprecher 1 m dahinter | — | **T:49** (vorher ~11) |
| 5. Sprecher 1 m hinter der Wand (Einzelstrahl) | — | **T:61** (vorher N:0) |

**Noch nicht** im echten Spiel gehoert. Mod gekocht (Doku 076, Pak siehe Memory).

## 4. Hinweise

- `OcclusionReverb` bleibt Standard 100: gebaute Raeume hallen jetzt nicht mehr, nur natuerliche (Hoehlen) und
  nach Groesse. Mit `OcclusionReverb=0` ist der Hall ganz aus.
- Die Nachhallzeit selbst ist weiter die der Hoehlen-Zonen; eine groessenabhaengige Nachhallzeit braeuchte eine
  Plugin-Aenderung (eigenes Protokollfeld fuer die Groesse).
