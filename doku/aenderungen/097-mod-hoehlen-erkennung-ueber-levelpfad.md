# 097 — Mod: Höhlen-Erkennung über den Level-Pfad (kein Hall mehr in Häusern, Wald und Städten)

**Nur Mod-Seite** (`Vo_PosStreamer_Actor`, nicht in Git), 2026-10-31. Plugin unverändert (8.0.20).
Die Pak ist **noch nicht gekocht und nicht hochgeladen** (der Besitzer kocht selbst).

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| Mod `/Game/Mods/teamspeak/Vo_PosStreamer_Actor`, Funktion `OccRoom` | In allen 9 Strahlen (1 nach oben, 8 waagerecht) zaehlt ein Treffer nur noch als „natuerlich“, wenn der **Pfad des getroffenen Actors** `Art_Caves` oder `Art_Dungeon` enthaelt. Vorher: „nicht von `BuildableBase`“. |
| Pro Strahl | `GetClass → ClassIsChildOf(BuildableBase) → NOT` ersetzt durch `GetPathName(HitActor) → Contains("Art_Caves") / Contains("Art_Dungeon") → OR` (3 Knoten raus, 4 rein; 146 → 155 Knoten) |
| Alles andere | unveraendert: Gewichte (oben 0,4, je waagerecht 0,075), Raumgroessen-Faktor, `OccLastNat`, `UpdateOcclusion`, Gesamt-Raumwert fuer die Okklusion |

---

## 2. Wie war es vorher?

Die Regel aus Doku 076 hiess: natuerlich = getroffen **und nicht** von `BuildableBase`
(vom Spieler gebaut). Damit zaehlte **alles andere** als Hoehle:

- MapEdits-Burgteile, `MedCastle`, Arena, Stadtmauern, Portalhaus im Vulkan,
- Conans eigene Siedlungen wie Sepermeru,
- Baeume und Buesche im Wald (waagerechte Strahlen treffen Staemme).

Alle bekamen einen hohen „natuerlichen“ Raumwert und damit Hall (`OcclusionReverb`).

---

## 3. Warum ist die neue Loesung besser?

Die Conan-Hoehlen liegen in eigenen Teil-Leveln: `Maps/ConanSandbox/Art/Caves/Art_Caves_x*_y*`
(20 Level) und Dungeons in `Art/Dungeon/Art_Dungeon_*` bzw. `PackedLevels/Art_Dungeon_*`
(rund 100 Level). Der Pfad des getroffenen Actors enthaelt den Level-Namen
(`/Game/Maps/ConanSandbox/Art/Caves/Art_Caves_x3_y2.Art_Caves_x3_y2:PersistentLevel.…`).
Damit hallt nur noch, wer wirklich in einer Hoehle oder einem Dungeon steht — ohne dass
ein Admin Zonen einrichten muss. Haeuser (gebaut oder Map), Staedte, Burgen und Wald liegen
in anderen Leveln und bekommen 0.

---

## 4. Wie funktioniert es?

```
OccRoom(Pos): 1 Strahl nach oben (600 cm) + 8 waagerecht (800 cm), Kanal TraceTypeQuery2
  pro Treffer:   natuerlich = getroffen UND Pfad(HitActor) enthaelt "Art_Caves" ODER "Art_Dungeon"
  OccLastNat   = Summe der natuerlichen Gewichte × Raumgroesse (mittlerer Abstand 250..700 cm → 0..1)
UpdateOcclusion: room-Feld des OCC-Eintrags = max(eigener, Sprecher) OccLastNat
Plugin:         Hall = room × OcclusionReverb/100 × 0,5   (braucht EnableOcclusion=True)
```

---

## 5. Wie wurde es getestet?

Im DevKit-Editor (eigene Instanz, `execute_python`, Editor-Welt, nichts von den Test-Leveln gespeichert):

| Fall | Gesamt-Raum | natuerlich (`OccLastNat`) |
|---|---|---|
| 12-m-Raum aus Wuerfeln im Level `Art_Caves_x3_y2` | 0,700 | **0,684** |
| 4,2-m-Huette im Cave-Level | 1,000 | 0,000 (zu klein) |
| freies Feld im Cave-Level | 0,000 | 0,000 |
| derselbe 12-m-Raum im normalen Level `AlmostEmpty` (wie Map-Haus) | 0,700 | **0,000** |

Das Blueprint kompiliert ohne Fehler; gespeichert wurde nur `Mods\teamspeak\Local\Vo_PosStreamer_Actor.uasset`.

**Nicht geprueft (Grenzen):**
- Die DevKit-Level der echten Hoehlen sind leere Huellen (3 Actors, 0 Foliage-Instanzen). Ob
  die Strahlen in den **echten** Hoehlen im Spiel Actors mit diesen Pfaden treffen, zeigt erst
  ein Test im Spiel.
- Nicht eingeschlossen: Hoehlen in MapEdits (eigene Level), Siptah-Hoehlen, Hoehlen, deren
  Fels in einem anderen Level liegt. Dafuer bleiben `Reverb=True`-Zonen.
- Kein PIE-Lauf, keine gekochte Pak.

---

## 6. Was der Besitzer tun muss

1. Mod kochen (`Graphify-Teamspeak-20261018\tools\cook_teamspeak.cmd` oder Cook-Button) und die Pak hochladen.
2. Root-Channel: `EnableOcclusion=True` und `OcclusionReverb=100` (stand seit 31.10. morgens auf 0).
3. Pruefen im Spiel: in einer Hoehle, im Wald, in Sepermeru und in einem Spielerhaus
   sprechen lassen, `DebugMode=true` im Plugin, Log `OCC-DBG … room=` ansehen (Hoehle > 0, Rest 0).
   Hallt eine echte Hoehle nicht, den Pfad des getroffenen Actors loggen und das Muster anpassen.

**Sicherungen:** Mod-Ordner vor der Aenderung: `C:\Users\uwebu\Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261031`
(plus die bisherige Pak als `teamspeak.pak.2026-10-20_2117`).

---

## 7. Lerneffekt

„Nicht vom Spieler gebaut“ ist kein Merkmal von „Hoehle“. Wo die Information liegt (hier der
Level-Name im Actor-Pfad), muss die Regel ansetzen — nicht an einer Klasse, die nur ein
Teil der Welt abdeckt.
