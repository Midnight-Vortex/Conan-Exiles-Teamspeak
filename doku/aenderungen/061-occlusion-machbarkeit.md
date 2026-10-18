# 061 – Machbarkeit: Schalldämpfung durch Wände und Fundamente (Prototyp)

> **Status:** Testbranch `test/occlusion-prototype`, **kein fertiges Feature**.
> Ziel war nur zu prüfen, ob Okklusion (= „Verdeckung“: zwischen Sprecher und
> Zuhörer liegt etwas Festes) technisch sinnvoll umsetzbar ist.

## Kurzantwort

**Machbar – bedingt.** Die Hinderniserkennung funktioniert **lokal im Spiel-Client**
über Line Traces („Sichtstrahl“, ein unsichtbarer Strahl durch die Kollisionswelt)
gegen Bauteile **und** Landschaft. Das TeamSpeak-Plugin selbst kann das **nicht**
allein, weil es keine Welt-/Gebäudedaten hat. Es braucht also:

1. eine **Mod-Erweiterung** (Blueprint in `Vo_PosStreamer_Actor`), die die Strahlen schießt, und
2. einen **kleinen Rückkanal** im vorhandenen HTTP-Weg (Plugin sagt der Mod, *wohin* sie
   prüfen soll; die Mod meldet das Ergebnis zurück).

Server-Datenbank oder Server-Mod werden **nicht** benötigt. Die Plugin-Seite dieses
Rückkanals ist als Prototyp gebaut (siehe unten); die Mod-Seite ist nur als
Blueprint-Plan beschrieben, weil Mod-Assets in diesem Test nicht gespeichert werden durften.

---

## 1. Datenquellen – was gibt es wo?

| Quelle | Gebäude/Fundamente | Felsen/natürliche Wände | Bewertung |
|---|---|---|---|
| **Client-Kollision (Line Trace in der Mod)** | Ja – Bauteile werden zum Client repliziert, inkl. Kollision | Ja – Landschaft und Level-Meshes sind auf dem Client geladen | **Empfohlen.** Nur lokal, keine Server-Änderung |
| Server-Datenbank (`game.db`, Tabelle `buildings`/`building_instances`) | Ja, aber nur Positionen/Klassen, keine Geometrie | Nein | Ungeeignet: Zugriff vom Spieler-PC nicht möglich, Geometrie müsste nachgebaut werden, Live-Änderungen (Abriss, Bau) nur verzögert |
| Server-Mod liefert Gebäudedaten | Möglich | Möglich | Unnötig aufwendig: Server müsste pro Spielerpaar tracen und an jeden Client senden → Netzlast + Serverlast |
| Heightmap (Höhenkarte) im Plugin | Nein | Nur Geländeprofil (Hügel) | Ungeeignet als Hauptquelle: keine Überhänge, Höhlen, Felsbögen, keine Gebäude; die Heightmap müsste aus den Spieldateien extrahiert und im Plugin mitgeliefert werden |

**Warum der Client die Gebäude „kennt“:** Die Bauteil-Klassen sind repliziert und haben
Kollision. Gemessen an den Klassen-Defaults im DevKit:

- `BP_BuildWall_T3`, `BP_BuildFoundation_T3`: `bReplicates = True`, `NetLoadOnClient = True`,
  `NetCullDistanceSquared = 4,0e8` → **Relevanzradius 200 m**.
- Kollision liegt auf `InstancedBuildingMesh` (Klasse `InstancedBuildingComponent`),
  Profil **`BuildingStructure`**, `QueryAndPhysics`, blockt die Kanäle **Visibility** und **Camera**.
- Zusätzliches Indiz: Spieler laufen clientseitig vorhergesagt auf Fundamenten – das geht
  nur, wenn der Client die Kollision hat.

200 m Relevanz ist deutlich größer als jede Sprechreichweite → innerhalb der
Hörweite sind die Bauteile auf dem Client vorhanden.

## 2. Was im PIE tatsächlich gemessen wurde

Umgebung: DevKit (zweite Instanz, Port 8124), PIE als Dedicated Server + Client,
Karte `/Game/Dev/AlmostEmpty`, Messung in der **Client-Welt** (`UEDPIE_1`) per Python.

**Einschränkung des Aufbaus:** Python kann in einer PIE-Welt keine Actors spawnen
(`FuncomGenericLibrary.SpawnActorFromPath` landet in der Editor-Welt und wurde sofort
wieder entfernt). Deshalb wurde ein vorhandener `StaticMeshActor` der Client-Welt
umgebaut: Mesh `SM_tier3_wall` bzw. `SM_tier3_foundation` (dieselben Meshes wie die
echten T3-Bauteile), Kollisionsprofil `BuildingStructure`. Getestet wurde damit die
**Geometrie + Kollisionsantwort** der Bauteile. Die echte Replikation eines gebauten
Bauteils wurde **nicht** im PIE nachgestellt (siehe Belege oben).

Abstand Zuhörer↔Sprecher jeweils 8 m, Ohrhöhe 1,6 m über Boden.

| Test | Ergebnis |
|---|---|
| Freies Feld | kein Treffer → frei |
| Wand T3 dazwischen (Visibility) | Treffer nach 4,04 m, Rückstrahl nach 3,73 m → **Dicke ≈ 23 cm** erkannt |
| Wand T3, Kanal Camera | Treffer nach 4,04 m (gleich) |
| Sprecher 1,5 m seitlich neben der Wandkante | frei (Strahl geht vorbei) |
| Zuhörer erhöht, Strahl streift die Wandkrone | Treffer, Dicke ≈ 9 cm (nur Kante) |
| Fundament T3, Zuhörer oben, Sprecher darunter | Treffer nach 1,62 m; Rückstrahl 0 m, weil der Sprecherpunkt **innerhalb** des 2,6 m hohen Fundaments lag (Sonderfall, siehe Grenzen) |
| Landschaft: Sprecher 3 m unter der Oberfläche („Höhle“) | Treffer `LandscapeHeightfieldCollisionComponent`; vor- und rückwärts dieselbe Fläche → Landschaft ist eine **Fläche ohne Dicke** |
| Höhenprofil per senkrechten Strahlen (4-m-Raster) | funktioniert, Karte ist aber flach (alle 2,56 m) – Hügel konnten hier nicht getestet werden |
| **Kosten** pro Strahl (inkl. Python-Overhead) | **14,3 µs** mit Treffer, **5,0 µs** ohne |

Hochrechnung: 16 Sprecher × 2 Strahlen (vor/zurück) × 10 Hz = 320 Strahlen/s ≈ **4,6 ms pro Sekunde**
im ungünstigsten Python-Fall, also < 0,5 % einer Sekunde. Native Blueprint-Traces sind
eher schneller. **Performance ist kein Problem.**

## 3. Architektur-Vorschlag

```
 Spiel-Client (Mod)                          TS-Plugin (HTTP-Thread)            TS-Plugin (Callback-Thread)
 ------------------                          -----------------------            ---------------------------
 Timer 0,03 s: POST /v1/position
   "SEQ=.. X=.. Y=.. Z=.. YAW=.. YAWY=..
    OCC=12:80;15:0"          ───────────►  occlusion_store_from_text()
                                            (pro TS-Client-ID: Prozent + Zeit)
                                            pos_inject_sample() ──────────►   recompute → audio_compute_client()
                                                                                occlusion_get() + occlusion_apply()
                                                                                (Gain runter, Tiefpass runter)
 HTTPSuccess: GetContentAsString  ◄──────── Antwort {"ok":true,
   Probe-Liste merken                         "probe":"12,300,400,150;15,..."}
 nächster Tick: pro Probe Line Trace                (nächste 16 Spieler ≤ 60 m, Unreal-Achsen, cm)
   Kopf → Probe-Punkt, Ergebnis in OCC=…
```

**Wer rechnet was?**

- **Mod:** nur Line Traces (sie hat als einzige die Welt). Keine Audio-Logik.
- **Plugin:** kennt bereits alle Sprecherpositionen (CEPOS) und die TS-Client-IDs.
  Es wählt die nahen Sprecher aus, schickt deren Positionen als „Probe“ an die Mod
  und setzt den zurückgemeldeten Wert in Lautstärke + Tiefpass um.
- **Server:** nichts.

**Zuordnung Spieler ↔ TS-Client:** Durch den Probe-Ansatz **entfällt** sie. Die Mod
muss nicht wissen, welcher Pawn welcher TS-Client ist – sie prüft einfach Punkte und
gibt die vom Plugin vergebene ID unverändert zurück. Das funktioniert sogar, wenn der
Pawn des Sprechers auf dem Client (noch) nicht repliziert ist.

**Schnittstelle (Prototyp, abwärtskompatibel):**

- Mod hängt `OCC=<cid>:<prozent>;...` an den bestehenden POST-Text an (Ganzzahlen 0–100,
  dadurch kein Komma/Punkt-Problem bei deutschem Gebietsschema). Ein leeres `OCC=`
  meldet nur „ich kann Okklusion“ an.
- **Nur** wenn `OCC=` im Request steht, antwortet das Plugin mit der Probe-Liste.
  Die heutige Mod schickt kein `OCC=` und bekommt weiter `{"ok":true}` → **keine
  Änderung am eingefrorenen Vertrag** (`doku/module/pos-http.md`) für bestehende Clients.
- Koordinaten in der Antwort: **Unreal-Achsen in cm** (X, Y, Z mit Z = Höhe), damit die
  Mod direkt `MakeVector` benutzen kann. (Das Plugin intern ist Y-up und in Metern.)

**Update-Rate / Verzögerung:** Die Mod sendet alle 30 ms. Der Strahl nutzt die Probe-Liste
der vorigen Antwort → Ergebnis ist ca. 1–2 POSTs (30–60 ms) alt, plus CEPOS-Alter der
Sprecherposition. Werte älter als **1 s** zählen als „frei“ (`OCCLUSION_STALE_MS`), damit ein
abgestürzter Mod-Kanal nie dauerhaft dämpft. Das Plugin rechnet ohnehin mindestens alle
100 ms neu (`TS3_RECOMPUTE_ALL_MIN_MS`), also greift ein neuer Wert spätestens nach 100 ms.

### Blueprint-Plan für die Mod (nicht umgesetzt, nicht gespeichert)

In `Vo_PosStreamer_Actor`:

1. **Variablen:** `ProbeIds` (Array Int), `ProbePoints` (Array Vector), `OccString` (String).
2. **`HTTPSuccess`** (Delegate liefert `SuccessResult : ModHttpSuccessResult`):
   `SuccessResult → GetContentAsString` → `Split` an `"probe":"` → rechten Teil bis `"`
   abschneiden → `ParseIntoArray` (Trenner `;`) → für jedes Element `ParseIntoArray`
   (Trenner `,`) → `[0]` `StringToInt` → `ProbeIds`, `[1..3]` `StringToFloat` → `MakeVector` → `ProbePoints`.
   Danach wie bisher `HttpBusy = false`.
3. **In `UpdatePosition` vor `FormatText`:** `OccString = "OCC="`; `ForEachLoop` über `ProbePoints`:
   - Start = `GetActorLocation(LocChar)` + (0,0,60) (Kopfhöhe; alternativ Kamera-Position),
   - Ende = `ProbePoint` + (0,0,60),
   - `LineTraceByChannel` (Channel **Visibility**, `ActorsToIgnore` = [LocChar], TraceComplex = true),
   - optional zweiter Trace Ende→Start für die Dicke,
   - Wert: kein Treffer → 0; Treffer → 100 (Prototyp) oder abgestuft nach Dicke/Anzahl,
   - `Append`: `OccString + ProbeId + ":" + Wert + ";"`.
4. `OccString` mit Leerzeichen an den bestehenden Text (`OutString`) anhängen, bevor
   `StartHttpRequest` ihn sendet. `SaveStringToFile` darf den Zusatz ignorieren.

Hinweis: Der Ziel-Pawn muss nicht ignoriert werden, wenn das Ziel ein Punkt ist; trifft der
Strahl einen anderen Spieler/NPC, wäre das eine Fehldämpfung → später einen eigenen
Trace-Kanal oder `ObjectTypes` = nur WorldStatic/Building verwenden.

## 4. Was im Plugin geändert wurde (Prototyp)

| Datei | Änderung |
|---|---|
| `src/core/proximity/occlusion.h/.c` (**neu**) | Speichert Okklusion pro TS-Client-ID lock-frei (`InterlockedExchange`), Parser für `OCC=…`, Veraltung nach 1 s, `occlusion_apply()` (Gain ×0,25 und Tiefpass bis 900 Hz bei 100 %), Probe-Liste (nächste 16 Spieler ≤ 60 m) |
| `src/core/http/pos_http_server.c` | Vor `pos_inject_sample()` `OCC=` speichern; bei Opt-in Probe-Antwort statt `{"ok":true}`; Antwortpuffer 512 → 1024 Byte |
| `src/ts/proximity/ts3_proximity_audio.c` | **Ein** Hook in `audio_compute_client()` (5 Zeilen, nach Realistic/Reverb, vor Pan): `occlusion_apply(occlusion_get(...))`, nicht bei Soundproof |
| `tests/occlusion_test.c` + `tests/run_tests.sh` | Neue Testsuite (Parser, Grenzen, Veraltung, Gain/Cutoff, Probe-Format/Achsen/Sortierung/Limit) |
| `project/…vcxproj`, `build/build_mingw.sh` | `occlusion.c` eingetragen |

**Vorher:** Die einzige Dämpfung waren Hub-Zonen (`zone_resolve.c`: soundproof/reverb),
also von Hand gezeichnete Polygone in der Kanalbeschreibung. Spielergebaute Häuser
waren unsichtbar für das Plugin.
**Neu (Prototyp):** Die Mod kann pro Sprecher einen gemessenen Verdeckungswert liefern.

**Threads (Pflichtregel):**

- HTTP-Thread: parst und schreibt Werte (atomar), baut die Probe-Liste (liest
  `player_table` mit dessen eigenem kurzen Lock). Keine TS-API.
- Callback-Thread: liest die Werte in `audio_compute_client()` (atomar), Ergebnis geht
  wie bisher über `g_writerLock` in den Seqlock-Snapshot.
- Audio-Thread: **unverändert** – er sieht nur andere Gain/Cutoff-Werte im Snapshot.
  Kein Lock, keine TS-API.

Die Änderung an `ts3_proximity_audio.c` ist bewusst nur ein Include + ein Block, damit
sie sich leicht mit dem HRTF-Umbau zusammenführen lässt.

## 5. Tests – was wirklich lief

- **PIE-Messungen** (Abschnitt 2): ausgeführt, Zahlen oben.
- **Layering-Guard** `bash tests/check_layering.sh`: **OK** (core/ bindet kein ts/ ein).
- **Unit-Tests / Build: NICHT ausgeführt.** Auf diesem PC ist kein C-Compiler vorhanden
  (kein MSVC/Visual Studio, kein gcc/MinGW, keine WSL-Distribution). Vor einem Merge:
  `bash tests/run_tests.sh` (Linux/MinGW) und `build\build_msvc.ps1` laufen lassen.
- **TS-Client-Test:** nicht möglich ohne Build **und** ohne Mod-Seite.
  Manueller Test, sobald beides da ist: zwei Spieler, einer hinter einer T3-Wand →
  Stimme deutlich leiser und dumpfer; einen Schritt seitlich neben die Wand → wieder klar.
  Ohne Mod-Update: Verhalten exakt wie bisher (kein `OCC=` → keine Werte).

## 6. Grenzen

- **Ein Strahl ist binär und grob:** offene Tür/Fenster neben dem Strahl werden nicht
  „umgangen“; Schall um Ecken (Beugung) fehlt. Abhilfe später: 3–5 Strahlen mit kleinem
  Versatz und Mittelwert, oder Dicke/Trefferzahl gewichten.
- **Punkt innerhalb von Geometrie:** Liegt ein Endpunkt im Mesh (Fundament mit Beinen,
  Sprecher auf/unter einem Bauteil), meldet der Rückstrahl 0 cm → Dicke unbrauchbar,
  Treffer selbst aber korrekt. Endpunkte daher auf Kopfhöhe legen.
- **Landschaft ist eine Fläche:** keine Dicke messbar, nur „Gelände dazwischen ja/nein“.
- **Höhenkarte allein** erkennt keine Höhlen, Überhänge, Felsbögen oder Gebäude.
- **Andere Pawns/NPCs** blocken Visibility ebenfalls → eigener Kanal/ObjectTypes nötig.
- **Relevanz:** jenseits 200 m (Netz-Relevanz der Bauteile) bzw. außerhalb des geladenen
  Streaming-Bereichs gibt es keine Kollision → Wert „frei“. Liegt weit über der Hörweite.
- **Asymmetrie:** Jeder Zuhörer misst selbst (sein Client, seine Strahlen). Zwei Spieler
  können leicht unterschiedliche Werte haben (Latenz, Kopfposition). Für Sprache unkritisch.
- **Verzögerung** 30–100 ms + CEPOS-Alter; Türen, die sich öffnen, wirken also mit kurzer Verzögerung.
- **Client-Wiederverwendung der ID** innerhalb von 1 s könnte einen alten Wert erben (Prototyp, vernachlässigbar).
- **Cheating:** Ein Spieler kann seine eigene Mod-Meldung fälschen – betrifft aber nur,
  was *er selbst* hört (z. B. Okklusion abschalten). Das kann er heute schon, indem er die
  Mod weglässt. Kein neues Risiko für andere.

## 7. Spätere Erweiterung: Races mit unterschiedlichem Gehör

Lässt sich sauber einbauen, ohne die Schnittstelle zu ändern:

- Die **Mod** kennt die Race des eigenen Charakters und könnte zusätzlich `HEAR=<faktor>`
  senden – oder besser: der **Hub/Server-Profil-Text** legt pro Race eine Tabelle fest
  (z. B. `occlusionStrength`, `fullCutoff`), und die Mod meldet nur die Race-ID.
- Im Plugin wird `occlusion_apply()` um einen Stärke-Parameter erweitert
  (`effektiv = occ × raceFactor`, Grenzen pro Race). Die Datenstrecke bleibt gleich.
- Weil der **Zuhörer** rechnet, betrifft die Race-Eigenschaft genau das, was dieser
  Spieler hört – das passt zu „diese Race hört besser durch Wände“.

Jetzt **nicht** implementiert (Goldene Regel).

## 8. Empfehlung

1. Ansatz **Client-Line-Trace + Probe-Rückkanal** weiterverfolgen. Server-DB und Heightmap verwerfen.
2. Nächster Schritt: Mod-Blueprint gemäß Abschnitt 3 in einer **Kopie** der Mod (eigener
   Mod-Ordner/Branch, nicht die Live-Mod) bauen und mit diesem Plugin-Build zu zweit testen.
3. Danach entscheiden: Strahlanzahl (1 vs. 3–5), Abstufung nach Dicke, eigener Trace-Kanal,
   Dämpfungswerte (heute fest: −12 dB / 900 Hz), Hub-Schalter zum An/Aus pro Server.
4. Vor Merge: Build + Unit-Tests auf einem Rechner mit Compiler; Vertrag in
   `doku/module/pos-http.md` um `OCC=`/`probe` ergänzen, sobald das Format feststeht.

## Lerneffekt

Wenn zwei Systeme unterschiedliche Daten haben (Plugin: wer spricht und wo; Spiel: was
dazwischen steht), ist ein **Frage-Antwort-Kanal über Punkte** einfacher als eine
Identitätszuordnung: Das Plugin fragt „ist zwischen dir und Punkt P etwas?“, und das Spiel
muss nicht wissen, wer P ist.
