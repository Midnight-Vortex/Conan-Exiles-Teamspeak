# 065 — Okklusion: Mod-Seite gebaut (Line Traces in der Conan-Mod)

**Branch:** `test/occlusion-prototype` · **Mod:** `Mods/teamspeak/Local/Vo_PosStreamer_Actor` (nicht in Git;
Backup vorher: `Documents\Codex\CountEmporiumDragons-Backup-20260927\Teamspeak_BeforeOcclusion_20261018\`)

## 1. Was wurde geaendert? (Blueprint `Vo_PosStreamer_Actor`)

| Teil | Aenderung |
|---|---|
| Variablen | `OccString` (String, z. B. `12:100;15:0`), `OccNextTime` (Zeit fuer die 10-Hz-Bremse) |
| Neue Funktion `UpdateOcclusion(Content)` | Liest die Plugin-Antwort, schiesst pro Probe-Punkt einen Line Trace, baut `OccString` |
| EventGraph `HTTPSuccess` | nach `HttpBusy = false`: hoechstens alle 0,1 s → `GetContentAsString` → `UpdateOcclusion` |
| `UpdatePosition` | HTTP-Text = `OutString + " OCC=" + OccString`. **Pos.txt bekommt weiter nur `OutString`.** |

Plugin-Seite: 061 (Prototyp), 064 (Root-Channel-Schalter `EnableOcclusion`).

## 2. Wie war es vorher?

Die Mod schickte nur die eigene Position. Das Plugin konnte nicht wissen, ob zwischen dir
und einem Sprecher eine Wand steht — Weltdaten hat nur das Spiel.

## 3. Warum so?

- **Nur das Spiel kennt die Welt:** Die Mod prueft Sichtlinien mit Line Traces auf dem
  Spiel-Client (Kanal **Visibility**). Im PIE gemessen: Bauteile **und** Gelaende blocken
  Visibility, Spielfiguren (Kapsel) nicht. Der eigene Building-Kanal
  (`ECC_TRACE_BUILDING_STRUCTURE`) trifft das Gelaende **nicht** — darum Visibility.
- **Keine Spieler-Zuordnung noetig:** Das Plugin schickt Punkte mit seiner TS-Client-ID, die
  Mod gibt die ID nur zurueck.
- **Sparsam:** maximal 16 Punkte, maximal 10× pro Sekunde, und nur wenn der Server
  `EnableOcclusion=True` setzt (sonst kommt keine Probe-Liste → keine Traces).

## 4. Wie funktioniert es jetzt?

```
Mod (alle 30 ms)                 POST "SEQ=.. X=.. Y=.. Z=.. YAW=.. YAWY=.. OCC=12:100;15:0"
                                          │
TS-Plugin (EnableOcclusion=True)          ├─ speichert 12→100 %, 15→0 %  → Lautstaerke/Tiefpass
                                          └─ Antwort {"ok":true,"probe":"12,x,y,z;15,x,y,z"} (Unreal-cm)
Mod HTTPSuccess (max. 10 Hz)  →  UpdateOcclusion:
   Split '"probe":"' … '"'  →  ParseIntoArray ';'  →  ForEach:
      ParseIntoArray ','  →  Ziel = (x, y, z + 60 cm Kopf)
      Start = Augen (GetActorEyesViewPoint), Ende = 1 m vor dem Ziel
      LineTraceSingle(Visibility, eigene Figur ignoriert) → Treffer ? 100 : 0
   OccString = "12:100;15:0"   → geht mit dem naechsten POST raus
```

Zwei Details, die der PIE-Test erzwungen hat:
- **Eigene Figur ignorieren:** Der Strahl startet an den Augen *in* der eigenen Figur; deren
  Skelett-Mesh blockt Visibility auf Kopfhoehe (Treffer bei 0 cm).
- **1 m vor dem Ziel aufhoeren:** sonst trifft der Strahl das Mesh des Sprechers selbst.
  (Rechnung ohne Operator-Nodes: `Vector_Distance`, `MapRangeUnclamped`, `FMax`, `VLerp`.)

## 5. Wie wurde es getestet?

- Kompiliert ohne Fehler/Warnungen; Logik-Vergleich des Exports gegen den Stand davor: nur die
  geplanten Verbindungen neu, `SaveStringToFile`/Pos.txt unveraendert.
- **PIE (Client-Welt), `UpdateOcclusion` direkt mit einer Plugin-Antwort aufgerufen:**
  - freie Luft 5 m → `0` ✔, Ziel 8 m unter dem Gelaende → `100` ✔
  - Ziel hinter einem **echten, replizierten Fundament** (`BP_BuildFoundation_C`, `InstancedBuildingComponent`,
    zur Laufzeit vom PIE-Server geladen) → `100` ✔ (Treffer 56–64 cm vor den Augen)
  - Antwort ohne Probe-Liste (Schalter aus) → `""` ✔
- **Nicht im PIE testbar: der HTTP-Rundweg.** Das Spiel blockt Mod-HTTP ohne Spieler-Freigabe:
  `NotApprovedByPlayer (Mod 'teamspeak' is not permitted to contact origin http://127.0.0.1:52734.)`.
  Im echten Spiel gibt der Spieler die Origin frei (wie fuer die bestehende Positions-Uebertragung).
- **Offen:** Mod kochen + hochladen, dann echter Test mit 2 Spielern, TS-Plugin aus diesem Branch und
  `EnableOcclusion=True` im Root-Channel.

## 6. Grenzen (Prototyp)

- Ein Strahl, Ergebnis nur 0 oder 100 % (keine Abstufung nach Wanddicke/Material).
- Andere Spieler-/NPC-Meshes auf Kopfhoehe zwischen euch koennen als Hindernis zaehlen.
- Treffer bei Bauteilen, die der eigene Client noch nicht geladen hat (> Relevanzradius), fehlen.
- Races: spaeter ueber einen Staerkefaktor in `occlusion_apply()` (Hub-Profil), Schnittstelle bleibt.

## Lerneffekt

Line Traces vom Kopf einer Figur starten *in* deren Mesh — immer die eigene Figur ignorieren und
vor dem Ziel aufhoeren, sonst misst man sich selbst.
