# 025 — CEPOS-Lasttest-Harness (200+ Spieler, host-seitig)

**Phase:** V8.8 · **Bezug:** `plan.md` Phase 5.4 (Lasttest-Automatisierung)

## Was wurde geaendert?

Neue Test-Suite `tests/cepos_load_test.c`, eingebunden in `tests/run_tests.sh`.
Sie simuliert den **reinen Kern-Pfad**, der bei vielen Spielern laeuft, wenn
CEPOS-Positionen ankommen — ohne TeamSpeak-Client:

1. **200 Spieler × 30 Runden** (~30 s @ 1 Hz): `player_table_put` + Listener-Recompute
   (Snapshot + `prox_distance` + `prox_volume_from_distance` fuer jeden Eintrag).
2. **600 einzigartige Clients** in einer Welle: LRU-Eviction, Tabelle bleibt bei 512.
3. **Worst-Case-Recompute:** 512 Spieler in Shout-Range, eine volle Distanz-Schleife.

Die Laufzeit wird ausgegeben (Baseline fuer Menschen), der Test **scheitert nicht** an
Timing — nur an Korrektheit (kein Crash, Eviction funktioniert, Snapshot-Groessen stimmen).

## Wie war es vorher (V7)?

Es gab `PROX-TEST` im Boot-Log (`prox_math_self_test`) — prueft Kurven und Timing,
aber **nicht** die Player-Tabelle unter Last. Ein echter 200-Spieler-Test war nur manuell
im TS-Client moeglich.

## Warum ist das besser?

```
CEPOS empfangen → player_table_put → (spaeter) snapshot + proximity_math
                      ↑                        ↑
              cepos_load_test [1][2]    cepos_load_test [1][3]
```

Jeder Commit kann jetzt pruefen: "Haelt die Tabelle 200 aktive Spieler aus?" und
"Evictiert sie korrekt bei 600?" — ohne Server mit 200 echten Clients.

## Wie getestet?

`bash tests/run_tests.sh` — Suite `cepos_load_test` muss PASS sein.
Manueller TS-Lasttest (am Ende, 30 min, 20+ Spieler): siehe `doku/aenderungen/014`
und `plan.md` Phase 4.x Metriken.

## Lerneffekt

Lasttests muessen nicht immer den ganzen Client starten. Wenn man den **teuren Kern**
identifiziert (hier: Tabelle + Mathe), kann man ihn isoliert auf dem Host faken —
das faengt Regressionen frueher ab als ein seltener Manuelltest.
