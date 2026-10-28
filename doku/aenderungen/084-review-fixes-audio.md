# 084 — Review-Fixes Audio-Pfad (PCM, Spielertabelle, Unmute)

Arbeitspaket "Audio" der Code-Review-Runde. Betrifft:

- `src/ts/proximity/ts3_proximity_audio.c` / `.h`
- `src/core/proximity/player_table.c`
- **neue** Header-only-Helfer (rein, auf dem Host testbar):
  `src/core/proximity/audio_snap.h`, `audio_fx.h`, `voice_cap.h`
- neue Tests: `tests/audio_fx_test.c`, `tests/audio_pcm_test.c`, `tests/voice_cap_test.c`,
  Test-Shim `tests/support/audio_shim/windows.h`; erweitert: `tests/player_table_test.c`

Leitplanke der ganzen Runde: **"Behebe die Fehler, ohne zu zerstoeren, was funktioniert."**
Darum bleibt der im Spiel getestete **Klang bit-identisch** (Cave-Hall, Tiefpass), und
jede Aenderung ist mit einem Test abgesichert, der **ohne** den Fix fehlschlaegt
(Mutationstest: Fix testweise ausgebaut -> der passende Test wird rot).

## Kleines Vokabular (Anfaenger)

- **Audio-Thread / PCM-Thread:** der Thread, auf dem TeamSpeak die Sprach-Samples
  zum Bearbeiten ruft. Er ist zeitkritisch: darf **nie warten** (keine Locks, auch
  keine Spin-Locks), nie die TS-API rufen, nie Speicher anfordern, nie loggen.
- **Callback-Thread:** der Thread fuer alle TS-Ereignisse; er rechnet die Zielwerte
  (Lautstaerke, Panorama, ...) und **veroeffentlicht** sie als Snapshot.
- **Seqlock:** Der Schreiber macht einen Zaehler `seq` ungerade, schreibt, macht ihn
  wieder gerade. Der Leser kopiert die Daten und vertraut der Kopie nur, wenn `seq`
  vorher gerade war und sich nicht geaendert hat.
- **Rampe:** Die Lautstaerke wird nicht schlagartig, sondern ueber den Block hinweg
  vom alten zum neuen Wert geschoben (sonst knackt es).
- **Talk-Pause:** Solange ein Spieler schweigt, ruft TeamSpeak unsere Audio-Funktion
  fuer ihn **gar nicht** auf. Alles, was wir uns fuer ihn merken (Rampe, Filter,
  Hall), "friert" dann ein und ist beim naechsten Satz veraltet.

---

## 1. Spin-Lock auf dem Audio-Thread entfernt (Unmute-Uebergabe)

**Was:** `unmute_ring_push` (Spin-Lock `g_unmuteRingPushLock` + Ring `g_unmuteRing`)
ist komplett weg. Der Audio-Thread setzt nur noch das Flag `g_pendingUnmute[id]`
(ein atomares Compare-Exchange) und zaehlt `g_pendingUnmuteCount` hoch.
`ts3_audio_flush_unmutes` (Callback-Thread) **scannt die Flags** (4095 Eintraege,
einige Mikrosekunden) und baut daraus den Batch.

**Vorher:** Der Ring brauchte einen Lock, weil *zwei* Threads hineinschrieben (Audio-
und Callback-Thread). Der Audio-Thread konnte also in `YieldProcessor()` drehen,
waehrend der (vom Betriebssystem unterbrechbare!) Callback-Thread den Lock hielt ->
hoerbarer Aussetzer fuer **alle** Sprecher.

**Warum besser:** Es gibt keinen geteilten Lock mehr, also nichts, worauf der
Audio-Thread warten koennte. Die Semantik bleibt: erster Unmute sofort, Wieder-
Unmute hoechstens alle 500 ms, nie verworfene Anforderungen (Flag bleibt gesetzt),
Batch max. 63 Clients (API-Grenze). Ein Client im 500-ms-Fenster wird ohne Batch-
Platz uebersprungen, kann andere also nicht verdraengen.

**Wie funktioniert es jetzt?**

```
Audio-Thread:   Flag[id]=1 (CAS) + Zaehler++     (wartet auf nichts)
Callback-Thread: Zaehler>0 ? -> Flags 1..4095 scannen -> Batch -> TS-API -> Flags loeschen
```

**Test:** `audio_pcm_test` [8]: PCM setzt Flag, Flush unmutet im Batch, 500-ms-
Sperre, 100 wartende Clients werden genau einmal in 63er-Batches abgearbeitet.
**Lerneffekt:** Ein Spin-Lock ist auf einem Echtzeit-Thread nie harmlos — besser die
Datenstruktur so waehlen, dass der Echtzeit-Thread nur atomar markiert.

---

## 2. Abgelaufene Spieler-Eintraege schalten den Snapshot jetzt ab

**Vorher:** Nach `PLAYER_TABLE_STALE_MS` (120 s ohne CEPOS) wirft die Spielertabelle
einen Eintrag still weg. Der Recompute-Pass kannte aber nur noch *frische* Eintraege —
der alte Snapshot des Spielers blieb `valid` und der PCM-Pfad spielte ihn mit der
letzten Lautstaerke/Panorama **fuer immer** weiter (Peer abgestuerzt, aber noch im
Kanal). Der Kommentar im PCM-Pfad versprach "expired -> mute".

**Jetzt:** `audio_recompute_all_impl` merkt sich, welche IDs im frischen Snapshot
waren (`fresh[]`), und `audio_snap_find_orphans` (neu, `audio_snap.h`) findet unter
dem Writer-Lock alle IDs mit gueltigem Snapshot, die **nicht** mehr frisch sind.
Fuer sie wird derselbe neutrale, **ungueltige** Snapshot veroeffentlicht, den ein
Client ohne jedes CEPOS hat (Cave-Slot frei, Richtung ungueltig). Der PCM-Pfad
zeigt fuer ungueltige Snapshots schon immer: Block auf 0, Rampe 0 -> **stumm**.
Kommt wieder ein CEPOS, veroeffentlicht der normale Pfad neu -> hoerbar.
Der Recompute-All laeuft mindestens 10 Mal pro Sekunde, die Abschaltung kommt also
fast sofort nach Ablauf der 120 s. Geloggt wird erst **nach** dem Lock.

**Test:** `audio_pcm_test` [9] (abgelaufener Sprecher stumm, frischer bleibt hoerbar,
Wiederkehr funktioniert), `audio_fx_test` [6] (Orphan-Suche), `player_table_test` [8].
**Lerneffekt:** Wenn eine Datenquelle Eintraege "leise" verfallen laesst, muss jeder
Verbraucher, der daraus abgeleiteten Zustand haelt, diesen Verfall aktiv nachziehen.

---

## 3. Lautstaerke-Rampe startet am Ziel; veralteter Zustand nach Pausen

**Vorher (zwei Probleme):**

a) Nach einem Generation-Bump (Spieler weg, ID wiederverwendet, Reset) setzte der
   Audio-Thread die Rampe auf **1.0** und schob sie ueber den ersten Block zum Ziel.
   Ein weit entfernter, leiser Sprecher war in seinen ersten 10-20 ms fast mit
   voller Lautstaerke zu hoeren.
b) Nach einer **Talk-Pause** (TS ruft nicht auf) blieben Rampe, Tiefpass-Gedaechtnis,
   Hall-Puffer und HRTF-Verlauf stehen: das Hall-Ende des vorigen Satzes spielte
   beim naechsten Satz von vorn.

**Jetzt:** Der Audio-Thread merkt sich pro Client den Zeitstempel des letzten Blocks
(`g_lastBlockMs`, `GetTickCount64` ist lock-frei, das Array gehoert nur ihm).
`render_gap_exceeded` (rein, in `ts3_proximity_audio.h`) sagt: erster Block oder
Luecke > `TS3_RENDER_GAP_MS` (200 ms) = **neuer Satz**. Dann (oder nach einem
Generation-Bump) gilt `g_renderFresh`:

- Gain und Pan starten **am Zielwert** (`render_ramp_start`), nicht bei einem alten Wert,
- Tiefpass wird neu gesetzt (siehe 5),
- der Cave-Hall wird genullt (der Audio-Thread tut das selbst, siehe 7),
- der HRTF-Zustand wird genullt (`g_hrtfLastMs`, gleiche Regel in `process_postprocess`).

**Wichtig — was bewusst gleich bleibt:** Ein Wechsel `PASSTHROUGH -> PROXIMITY`
**mitten im Satz** hat keine Luecke und keinen Bump; der letzte Block lief mit
Gain 1.0 (der PASSTHROUGH-Zweig setzt `g_renderGain = 1.0` und loescht das Fresh-
Flag), also rampt der erste Proximity-Block weich von 1.0 zum Ziel — kein Knacken.
Auch wenn ein Block wegen `BUSY` (siehe 6) ausfaellt, bleibt das Fresh-Flag erhalten.

**Hoerbare Aenderung (klein, gewollt):** Der allererste Block eines Sprechers (und
der erste nach einer Pause) hat kein ~20-ms-Einblenden mehr, sondern setzt sofort mit
der richtigen Lautstaerke ein. Nach einer Pause war das vorher schon so (Rampe
startete beim alten Zielwert), jetzt ist es konsistent.

**Test:** `audio_pcm_test` [1]-[4], [11] (Peak des ersten Blocks <= Ziel, Rampe ohne Pause
weich, Moduswechsel-Kontinuitaet, kein Hall-Rest, HRTF-Reset), `audio_fx_test` [8].
**Lerneffekt:** "Zustand zuruecksetzen" heisst nicht "auf einen Standardwert setzen",
sondern "so setzen, dass der naechste Block nichts Altes hoerbar macht".

---

## 4. Cave-Hall: eigene Positionen fuer links und rechts (Klang identisch)

**Vorher:** `combPos[c]` und `apPos[a]` wurden von **linkem und rechtem** Kanal geteilt
und deshalb pro Sample um **2** statt 1 weitergeschoben. Folge: eine Kammer mit
**ungerader** Laenge (3507, 4257, Allpasses 607, 481) verzoegert wie geplant um `len`
Samples, eine mit **gerader** Laenge (2546, 5466) aber nur um `len / 2`. Der im Spiel
getestete Klang hatte also faktisch die Verzoegerungen `{1273, 3507, 4257, 2733}`.

**Jetzt:** `CaveDsp` (`audio_fx.h`) hat `combPosL/R` und `apPosL/R`; die Laengen sind
die **tatsaechlichen** Werte `{1273, 3507, 4257, 2733}` / Allpass `{607, 481}`
(`CAVE_COMB_MAX` = 4257, ein Compile-Zeit-Check verhindert Puffer-Ueberlauf).

**Beweis der Gleichheit:** `audio_fx_test` [1] enthaelt eine woertliche Kopie des
**alten** Algorithmus und vergleicht ueber 400000 Samples (Impuls, Rauschen,
Mono-Bursts mit Pause) **Bit fuer Bit** — 0 Abweichungen. (Mathematisch: bei
ungerader Laenge besucht jeder Kanal jede Position genau alle `len` Samples, bei
gerader nur die halbe Ringgroesse.)

**Fuer spaeter (Entscheidung des Nutzers):** Die *entworfenen* Laengen
`2546 / 3507 / 4257 / 5466` waeren eine echte Klangaenderung (laengerer, dichterer
Hall). Dann `CAVE_LEN_0/3` aendern und `CAVE_COMB_MAX` auf 5466 setzen. Nicht Teil
dieses Fixes.

**Lerneffekt:** Wenn zwei Kanaele einen Index teilen, ist das ein stiller Faktor 2;
beim Aufraeumen zuerst den *tatsaechlichen* Klang festschreiben, dann umbauen.

---

## 5. Tiefpass: erster Block gefiltert; toter Zweiter-Durchlauf entfernt

**a) Vorher:** `initialized` wurde erst **nach** der Schleife gesetzt; waehrend des
ersten Blocks folgte das Filtergedaechtnis `prev` der Eingabe -> der erste Block nach
jedem (Re-)Init (neuer Satz, Zonenwechsel, Reset) lief **ungefiltert** durch.
**Jetzt:** `prev` wird vor der Schleife aus dem ersten Sample gesetzt und
`initialized` sofort gesetzt (`audio_lpf_apply`, `audio_fx.h`). Gleichstrom laeuft
dadurch ohne Einschwingen von Sample 0 an durch, und der erste Block ist gefiltert.

**b) Vorher:** Der "zweite Durchlauf" `x = prev; prev += alpha*0.7*(x - prev)` addiert
immer genau 0 (`x == prev`) — der Filter war in Wahrheit schon immer **einpolig**.
**Jetzt:** der tote Code ist entfernt, die Ausgabe ist identisch. Ein *echter* zweiter
Pol (steilere Flanke) wuerde Entfernungs-/Okklusionsklang aendern — Entscheidung des
Nutzers, nicht Teil dieses Fixes.

**Test:** `audio_fx_test` [2] (alter Code laesst den ersten Block ungefiltert =
der Bug; neuer filtert; Gleichstrom konstant), [3] (mit gleichem Anfangszustand
identisch zum alten Code ueber 60 Bloecke, 4 Grenzfrequenzen, 1/2/3 Kanaele).
**Lerneffekt:** Totem Code, der "nichts tut", sollte man erst beweisen und dann loeschen.

---

## 6. Seqlock: dreiwertiges Lese-Ergebnis (OK / INVALID / BUSY)

**Vorher:** `snap_read` und `dir_read` gaben nach 3 gescheiterten Versuchen `0`
zurueck — dasselbe wie "Snapshot ungueltig". Wurde der Schreiber gerade
**unterbrochen** (Betriebssystem), schaltete der Audio-Thread den Sprecher stumm und
setzte seinen Render-Zustand zurueck -> Knacken/Aussetzer ohne echten Grund. Ausserdem
fehlte zwischen den Daten-Loads und dem pruefenden zweiten `seq`-Load eine
Compiler-Barriere (der Compiler darf die Daten-Loads sonst verschieben).

**Jetzt:** `audio_snap_read` / `audio_dir_read` (`audio_snap.h`) liefern
`AUDIO_SNAP_OK`, `AUDIO_SNAP_INVALID` oder `AUDIO_SNAP_BUSY`, mit Compiler-Barriere
(`_ReadWriteBarrier()` bzw. `__atomic_thread_fence`) vor und nach dem Kopieren.

- **INVALID:** wie bisher (stumm, Zustand zuruecksetzen).
- **BUSY:** der Audio-Thread nimmt die **zuletzt guten** Zielwerte aus einem eigenen
  Cache (`g_targetCache`, nur er schreibt) und fasst den Render-Zustand nicht an.
  Gibt es noch keinen Cache (erster Block / nach Reset), wird **dieser eine Block**
  stumm geschaltet (nie volle Lautstaerke) — ebenfalls ohne den Zustand anzufassen,
  das Fresh-Flag aus Punkt 3 bleibt erhalten.
- **HRTF-Richtung:** gleiche Logik (`g_dirCache`); ohne Cache wird HRTF fuer diesen
  Block uebersprungen (TS-3D-Panorama spielt) statt den HRTF-Zustand zu zerstoeren.
- Cache wird bei Generation-Bump, Luecke und INVALID verworfen (kann zu einem frueheren
  Besitzer der ID gehoeren). `ts3_proximity_audio_soundproof_muted` (beliebiger Thread)
  behandelt BUSY wie bisher als "unbekannt" = 0.

**Test:** `audio_fx_test` [4] (alle drei Zustaende, auch Richtung), [5] (echter
Schreiber-Thread, 3 Mio. Lesevorgaenge: nie ein zerrissener Snapshot als OK, nie
INVALID waehrend der Schreiber schreibt); `audio_pcm_test` [5]/[6]/[11].
**Lerneffekt:** "Ich konnte nicht lesen" und "es gibt nichts" sind verschiedene
Antworten und muessen verschieden behandelt werden.

---

## 7. Cave-Slot-Recycling ohne Race

**Vorher:** `cave_slot_acquire` (Callback-Thread) nullte per `memset` die ~200 KB eines
Slots, waehrend der Audio-Thread noch mitten im Block des *vorigen* Besitzers in genau
diesem Slot rechnen konnte (zwei Schreiber, hoerbarer Knackser, kein Absturz).

**Jetzt:** Der Callback-Thread fasst die Puffer **nie** mehr an. `acquire` erhoeht nur
`acquireEpoch` — **vor** dem Setzen des Besitzers. `audio_apply_cave` (Audio-Thread)
prueft Besitzer, liest die Epoche, prueft den Besitzer **noch einmal** und nullt den
Slot selbst (`cave_dsp_reset`), wenn die Epoche neu ist (oder ein neuer Satz beginnt,
Punkt 3). Die Puffer haben damit genau einen Schreiber.

**Test:** `audio_pcm_test` [10]: ein neuer Besitzer hoert keinen Rest des Vorbesitzers;
`acquire` laesst die Puffer unangetastet (Beweis fuer "Callback fasst sie nicht an").
Nebeneffekt: ein Slot ist etwa 20 % kleiner (Kammpuffer 4257 statt 5466 Samples) und damit auch das Nullen billiger.
**Lerneffekt:** Besser ein Eigentuemer, der "beim ersten Sehen" aufraeumt, als zwei
Threads, die sich ueber einen Zeitstempel abstimmen.

---

## 8. Performance: Hub-Werte einmal pro Durchlauf

**Vorher:** `audio_compute_client` rief pro Client `server_profile_get_max_volume`,
`_realistic_audio`, `_filter_intensity` (je eine ~8-KB-Kopie der HubSettings) und
`player_table_get`, obwohl der Durchlauf den Eintrag und das Profil schon hatte. Der
Dirty-Pfad (bis zu 64 Clients pro Drain) holte das Profil sogar 64 Mal.

**Jetzt:** `AudioPass` (+ `audio_pass_begin`) holt Position, Profil-Kopie und alle
Profil-Skalare **einmal** pro Durchlauf; `audio_compute_client(pass, entry, out)`
bekommt den Tabelleneintrag uebergeben. Auch der Dirty-Drain baut nur einen Pass
(und nutzt den Eintrag aus dem Snapshot). Ergebnisse unveraendert.

**Test:** `audio_pcm_test` [12] zaehlt Aufrufe: 100 Clients -> 1 Profil-Kopie, 1x je
Skalar; Dirty-Drain mit 40 Clients -> 1 Profil-Kopie.

---

## 9. Stimm-Reichweite des Gegenspielers wird begrenzt (Hub-Limits)

**Vorher:** CEPOS uebertraegt die Stimm-Reichweite des Senders (bis 1000 m). Ein
veraenderter Client konnte 1000 m melden und so `MaximumWisper/Normal/Shout` des Hubs
umgehen — der Empfaenger vertraute dem Wert.

**Jetzt:** `voice_distance_cap(hub)` (`voice_cap.h`) liefert die **groesste Reichweite,
die ein unveraenderter Client unter diesem Profil melden kann**, und der Empfaenger
begrenzt darauf (`audio_remote_hear_range`, gilt fuer Hoerbereichs-Test *und*
Lautstaerkekurve). Was ein echter Client sendet (`voice_modes.c`): Config-Wert ->
Zonen-Override -> Klemme `[min, max]` (die Klemme kommt **zuletzt**; Race-Limits
ersetzen die globalen). Daraus:

- Cap = groesstes `max*` ueber **globale Limits und alle Races** (der Sender kann in
  einer beliebigen Race sein). Zonen-Distanzen heben den Cap nur an (Vorsicht gegenueber
  aelteren Plugin-Versionen). Es gibt nie einen niedrigeren Wert als einen legitimen.
- Hat **irgendein** Modus (global oder in einer Race) kein Maximum (0 = unbegrenzt),
  gibt es keinen Cap -> Wert bleibt **unveraendert** (wie bisher). Ebenso ohne Profil.

**Test:** `voice_cap_test` ist ein **Eigenschaftstest gegen den echten
`voice_modes.c`**: 30000 Zufallsprofile (Zonen, Races, Min/Max, Nutzerwerte 0-1000) x
3 Modi -> der gesendete Wert ist nie groesser als der Cap, die Klemme schneidet nie
einen legitimen Wert ab (ein Ausbau der Race-Behandlung macht den Test rot);
`audio_fx_test` [7] (Cap-Tabelle), `audio_pcm_test` [13] (1000 m bei 500 m Abstand:
nicht hoerbar; bei 90 m gleiche Lautstaerke wie ehrlicher 100-m-Schrei; kein Limit ->
unveraendert).
**Hinweis:** `ts3_3d.c` (`ts3d_in_hear_range`) nutzt die Reichweite ungeklemmt, nur zum
Auswaehlen, welche 3D-Positionen gesetzt werden (kein Lautstaerke-Einfluss). Siehe
Bericht.

---

## 10. IDs ausserhalb des Arrays: im Hub-MUTE-Modus nicht mehr "fail open"

**Vorher:** `ts3_audio_process_playback` kehrte bei `clientID >= TS3_MAX_CLIENT_ID` (oder 0)
sofort zurueck, Samples unberuehrt. Im Hub (`TS3_AUDIO_MUTE`, "alle stumm") war so ein
Sprecher **hoerbar**.
**Jetzt:** Bei ungueltiger ID wird im MUTE-Modus der Puffer genullt; im PROXIMITY- und
PASSTHROUGH-Modus bleibt es wie bisher unberuehrt (keine Daten, keine Dampfung moeglich).
**Test:** `audio_pcm_test` [7] (4096, 5000, 60000, 0 in allen drei Modi; NULL-Puffer).

---

## 11. Spielertabelle: Name beim Slot-Wiederverwenden, Log ausserhalb des Locks

**Vorher:** (a) Ein Slot, der von einem **abgelaufenen** (aber noch `valid`) Eintrag oder
per LRU uebernommen wurde, behielt bei leerem neuen Namen den Namen des Vorgaengers.
(b) `log_debug` (Datei-I/O) lief innerhalb von `g_tableLock`.
**Jetzt:** (a) `slotReused` (frei, anderer Client, oder eigener abgelaufener Eintrag)
loescht den Namen, wenn kein neuer kommt; nur ein Update eines frischen Eintrags
behaelt den Namen wie bisher. (b) Die Log-Daten werden unter dem Lock gesammelt und erst
nach `LeaveCriticalSection` geschrieben.
**Test:** `player_table_test` [6] (Name), [7] (Lock-Probe: ein Hilfs-Thread nimmt waehrend
`log_debug` den Tabellen-Lock; haelt `put` ihn noch, schlaegt der Test fehl — mit dem
alten Code tut er das).

---

## Wie wurde getestet?

- `audio_fx_test` (rein), `voice_cap_test` (gegen echten `voice_modes.c`), erweitert
  `player_table_test` und **`audio_pcm_test`**: dieser Test bindet die **echte**
  `ts3_proximity_audio.c` auf dem Linux-Host ein (`tests/support/audio_shim`) und treibt
  `ts3_audio_process_playback`, `_postprocess`, Recompute-Passes, Unmute-Flush und
  Spielertabelle durch. Die 14 externen Funktionen (Profil, Position, Adapter, Log) sind
  Stubs, Zeit wird per `win32_shim_advance_ms` gesteuert.
- Mutationstests: 12 gezielte Ausbau-Varianten der Fixes (kein Fresh-Start, BUSY als
  INVALID, kein Orphan-Sweep, kein MUTE fuer fremde IDs, Callback-`memset`, Pro-Client-
  Getter, kein Cap, kein HRTF-Gap, ...) — jede wird von mindestens einem Test erkannt.
- `audio_fx_test` und `audio_pcm_test` zusaetzlich mit `-O0`, `-O3` und ASan+UBSan gruen; alle betroffenen
  Bestands-Suites (proximity_math, zone_resolve, player_table, occlusion, render_state, cepos_load, hrtf,
  talk_state, hub_parser, voice_modes) gruen; MinGW-Compile ohne neue Warnungen.
- **Noch noetig:** Test im echten TS-Client (siehe unten) — hier laeuft kein Windows/TS.

## TS-Client-Test (was man hoeren/pruefen soll)

1. Zwei Clients, A weit weg (nahe der Hoergrenze): A spricht nach laengerer Pause einen
   Satz an -> der erste Laut darf **nicht** kurz laut sein, dann leise werden.
2. In einer Hoehlen-Zone (Reverb=True): sprechen, 1-2 s Pause, wieder sprechen -> kein
   Hall-Nachklang des vorigen Satzes vor dem ersten Wort.
3. Spieler ohne CEPOS-Update (Spiel hart beenden, TS bleibt offen) -> nach ~2 min ist er
   **stumm**, nicht mehr mit eingefrorener Lautstaerke zu hoeren.
4. Hub (Lobby): alle stumm, auch bei grossen Client-IDs (grosser Server).
5. Viele Sprecher gleichzeitig (10+): keine Aussetzer/Knackser beim Unmute.
6. Hub-Limits setzen (MaximumShout=100): ein Client, der 1000 m meldet, ist nicht weiter
   als 100 m hoerbar.

## Risiken

- Fresh-Start nach Pause/erstem Block: Sprecher setzen ohne ~20-ms-Einblendung ein
  (sonst unveraendert).
- Spielertabelle laeuft nach 120 s ohne CEPOS ab -> stumm (so war es immer gedacht).
- Cap: Hubs, in denen irgendein Modus kein Maximum hat, bleiben unbegrenzt (bewusst).
