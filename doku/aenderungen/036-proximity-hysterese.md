# 036 — Proximity: Enter/Exit-Hysterese fuer die Hoerreichweite

**Phase:** V8.16 · **Bezug:** `ts3_sdk26_proximity_plan.md` §8 (10-Meter-Puffer)

## Was wurde geaendert?

Die **Cull-Entscheidung** (welcher Sprecher wird ueberhaupt raeumlich berechnet
und welcher bekommt ein stummes „neutrales" Snapshot) bekommt eine echte
**Enter/Exit-Hysterese** statt einer einzelnen Schwelle.

Konkret:

- **Neu (rein, testbar):** `prox_hysteresis_in_range()` in
  `src/core/proximity/proximity_math.{h,c}` — zustandslose Entscheidung:
  `dist <= enter` → hoerbar, `dist > exit` → gecullt, dazwischen wird die
  **vorige Entscheidung gehalten**.
- `src/ts/proximity/ts3_proximity_audio.c`:
  - Neue Konstante `TS3_AUDIO_EXIT_MARGIN_M 10.0f` ersetzt die alte feste
    Cull-Grenze (`TS3_AUDIO_CULL_MARGIN 1.25f` + `TS3_AUDIO_CULL_PAD_M 2.0f`).
  - Neuer Zustands-Latch `g_hearInRange[TS3_MAX_CLIENT_ID]` (Callback-Thread
    only, wie `g_clientUnlocked`).
  - `audio_in_hear_range()` nimmt jetzt die `clientID`, rechnet
    `enter = voiceDistance + listenAdd`, `exit = enter + 10 m` und ruft die reine
    Funktion mit dem gemerkten Zustand.
  - **Beide** Recompute-Pfade (`audio_recompute_all_impl` und
    `audio_recompute_client_impl` / CEPOS-Dirty) nutzen denselben Cull — sonst
    wuerde ein stillstehender Listener mit dirty CEPOS weiterhin Unmutes im
    Enter/Exit-Band ausloesen (Bugbot-Finding).
  - Latch wird in `ts3_audio_invalidate_client()` (pro Client) und
    `ts3_audio_reset()` (komplett) genullt — eine wiederverwendete Client-ID
    startet „ausser Reichweite".
- **Test:** `tests/proximity_math_test.c` — neuer Abschnitt `[2b]` (Annaeherung/
  Entfernung ohne Flackern, Band-Halten, Grenzfaelle). Laeuft im vorhandenen
  `proximity_math_test`-Suite mit, keine `run_tests.sh`-Aenderung noetig.

## Wie war es vorher?

`audio_in_hear_range()` nutzte **eine** Grenze: `dist <= voiceDistance * 1.25 +
2 m`. Ein Sprecher, der genau an dieser Grenze pendelte (Grenzwert-Zittern durch
Bewegung/Positionsrauschen), kippte bei jedem `recompute_all` zwischen
„berechnet" und „neutral/stumm" — und loeste dabei wiederholt Mute/Unmute-
Zyklen (TS-API) aus. Gedaempft wurde das nur indirekt ueber den Soft-Tail der
Lautstaerkekurve (`fadeEnd = 1.12x`) und die 500-ms-Unmute-Entprellung.

## Warum ist die neue Loesung besser?

1. **Deterministisch statt nur gedaempft.** Ein einmal hoerbarer Sprecher bleibt
   hoerbar, bis er **10 m ueber** seine nominale Reichweite hinaus ist. Erst dann
   wird wieder gecullt. Das Pendeln an der Grenze ist damit ausgeschlossen, nicht
   nur abgemildert.
2. **Kein Eingriff in den Audio-Hotpath.** Die Hysterese sitzt ausschliesslich im
   Cull-/Recompute-Pfad auf dem Callback-Thread. Der PCM-Callback und die
   Lautstaerkekurve bleiben **unveraendert** — gleiche Lautstaerke bei gleicher
   Distanz, nur die An/Aus-Flanke ist stabilisiert.
3. **Reine, getestete Kernlogik.** Die eigentliche Entscheidung ist eine
   seiteneffektfreie Funktion in `proximity_math` (gleiche Konvention wie
   `prox_volume_from_distance` & Co.) und damit host-testbar — der Latch im
   Audio-Modul ist nur noch Buchhaltung.

## Wie funktioniert es jetzt?

`enter = voiceDistance + listenAdd` (nominale Reichweite des Sprechers),
`exit = enter + 10 m`.

```text
 Callback-Thread: audio_recompute_all_impl()  (Bewegung, ~10 Hz)
   fuer jeden Sprecher i:
     dist = |local - remote|
     inRange = prox_hysteresis_in_range(dist, enter, exit, g_hearInRange[i])
                 dist <= enter  -> 1
                 dist >  exit   -> 0
                 sonst          -> g_hearInRange[i]   (Band halten)
     g_hearInRange[i] = inRange
     inRange ? audio_compute_client(...) : neutrales (stummes) Snapshot
```

Beispiel (Reichweite 50 m → enter 50, exit 60), Plan §8:

```text
Annaeherung: 70→stumm 60→stumm 51→stumm 50→hoerbar
Entfernung:  50→hoerbar 55→hoerbar 60→hoerbar 61→stumm
```

**Thread-Vertrag:** `prox_hysteresis_in_range()` ist rein (jeder Thread).
`g_hearInRange` wird nur auf dem **Callback-Thread** geschrieben
(`audio_recompute_all_impl`, `invalidate`, `reset`) — kein Lock noetig, gleiche
Konvention wie `g_clientUnlocked`/`g_lastUnmuteMs`. Der per-Client-Recompute-Pfad
(`audio_recompute_client_impl`) cullt nicht und laesst den Latch unberuehrt; die
Lautstaerkekurve mutet dort weit entfernte Sprecher ohnehin (Gain ~0).

## Wie wurde es getestet?

- **Build:** `build_msvc.ps1` → Release x64 OK (exit 0), `proximity_math.c` und
  `ts3_proximity_audio.c` uebersetzt, Link OK.
- **Unit-Test:** `tests/proximity_math_test.c` Abschnitt `[2b]` deckt ab:
  harte Entscheidung innerhalb/ausserhalb des Bandes unabhaengig vom Vorzustand,
  Grenzfaelle genau bei `enter` und `exit`, Band-Halten in beide Richtungen und
  die volle Annaeherungs-/Entfernungssequenz ohne Flackern. Auf diesem Rechner
  ist kein gcc/Bash fuer `run_tests.sh` vorhanden (nur WSL ohne Distro); die
  Kernlogik ist reines C und wurde beim MSVC-Build mituebersetzt.
- **Manuell im TS-Client (noch offen):**
  1. Zwei Clients mit neuem DLL, gleicher Ingame-Kanal, Debug-Log an.
  2. Ein Sprecher bewegt sich langsam an der Reichweitengrenze hin und her.
  3. Erwartung: **kein** wiederholtes „unmuted client"-Zittern im Log; Stimme
     schaltet erst ~10 m hinter der Reichweite ab und erst bei erneutem
     Unterschreiten der Reichweite wieder an.

## Lerneffekt

Eine An/Aus-Grenze am selben Wert erzeugt an Grenzwerten immer Flattern —
egal wie sanft die nachgelagerte Kurve ist. Zwei getrennte Schwellen (Enter <
Exit) mit gemerktem Zustand loesen das strukturell. Wichtig ist, die
**Kernentscheidung rein** zu halten (host-testbar) und den **Zustand
getrennt** in genau einem Thread zu fuehren — so bleibt der Audio-Hotpath
unberuehrt und die Thread-Konvention konsistent.
