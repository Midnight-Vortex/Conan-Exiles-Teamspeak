# 043 — TS-3D Listener-Log volle Präzision

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/ts/proximity/ts3_3d.c` | Throttled Debug-Log (10 s): `pos=X=%.6f Y=%.6f Z=%.6f fwd=X=…` statt `pos=(%.1f,…)` / `fwd=(%.2f,…)` |

**Nicht geändert:** 3D-API-Aufrufe, Einheiten (Meter, bereits `/100`), Throttle-Intervall.

## 2. Wie war es vorher (V7/V8)?

- `log_debug("TS-3D: listener pos=(%.1f,…) fwd=(%.2f,…)")` — Position 1 Nachkommastelle, Forward 2.
- POS/CEPOS/HTTP-Logs nutzen seit Änderung 041 bereits `X=`/`Y=`/`Z=` mit `%.6f`.
- TS-3D-Werte sind **Meter** (Proximity rechnet cm→m); die grobe Formatierung ließ Vergleich mit anderen Logs schwer.

## 3. Warum ist die neue Lösierung besser?

- **Einheitliches Log-Format** mit POS, HTTP und CEPOS — gleiche Feldnamen und 6 Nachkommastellen.
- **Kein Einheitenwechsel** — weiter Meter, kein Zurückrechnen in cm.
- Debug-Vergleich Listener-Position vs. PosSample/HTTP ohne mentalen Umrechnungs- oder Rundungsfehler.

## 4. Wie funktioniert es jetzt?

```
CEPOS / PosSample (cm) → Proximity /100 → ts3d_set_listener (Meter)
                                              │
                                              └─► alle 10 s (debug=1):
                                                  TS-3D: listener pos=X=… Y=… Z=… fwd=X=… Y=… Z=…
```

Cast auf `(double)` vor `%.6f` — konsistent mit anderen Modulen.

## 5. Wie wurde es getestet?

1. Plugin bauen, TS-Client neu starten.
2. `plugin.cfg`: `DebugMode=true`.
3. Proximity aktiv, Spielposition vorhanden.
4. Log: `DBG TS-3D: listener pos=X=3593.097464 …` — 6 Stellen, beschriftete Achsen, Werte in Metern.

## 6. Lerneffekt

Logs aus verschiedenen Pipeline-Stufen sollten dasselbe Format nutzen — sonst vergleicht man gerundete Anzeigen statt echte Werte. Einheiten im Log-Text explizit lassen (hier Meter), nicht stillschweigend mischen.
