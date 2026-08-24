# 041 — PosSample volle Präzision + Debug-Logs

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/mod_file/pos_file.h` | `PosSample`: `x`, `y`, `z`, `yaw`, `yawY` von `float` auf `double` |
| `src/core/mod_file/pos_file.c` | Parser: `strtod` direkt in `double`, keine `(float)`-Casts; Plausibilität mit `fabs`; Debug-Logs mit `X=`/`Y=`/`Z=`/`YAW=` und `%.6f` |
| `src/core/http/pos_http_server.c` | JSON/Pos.txt-Parser auf `double`; HTTP-Debug/Reject-Logs mit `%.6f` und beschrifteten Feldern |
| `src/ts/proximity/ts3_cepos.c` | `/100`-Umrechnung in `double`, dann einmal `(float)` fürs Wire-Paket; SEND/RECV-Logs `%.6f` |

**Nicht geändert:** `CeposPacket`-Layout (56 Byte, float32), Base64, `CEPOS_POS_EPS`, Proximity-Math in anderen Modulen.

## 2. Wie war es vorher (V7/V8)?

- Pos.txt und HTTP lieferten volle Dezimalstellen, z. B. `X=359309.746376`.
- Der Parser rief `strtod` auf, castete aber sofort auf `float` → bei großen Koordinaten gehen ~0,03 cm verloren (ULP-Grenze von float32).
- Debug-Logs nutzten `%.1f` → Anzeige noch grober als der gespeicherte Wert, z. B. `359309.8` statt `359309.746376`.
- CEPOS teilte in `float` (`/ 100.0f`), bevor ins Wire-Paket geschrieben wurde.

## 3. Warum ist die neue Lösung besser?

- **Logs zeigen, was wirklich geparst wurde** — kein doppeltes Runden (Speicher + Format).
- **Lokale PosSample-Snapshot behält Pos.txt/HTTP-Ziffern**, bis bewusst für CEPOS auf float32 reduziert wird.
- **/100 in double** vermeidet unnötigen Präzisionsverlust vor dem einen Cast ins 56-Byte-Paket.
- Kein Wire-Format-Bruch: andere Clients mit altem Plugin bleiben kompatibel.

## 4. Wie funktioniert es jetzt?

```
Pos.txt / HTTP POST
        │
        ▼  strtod (Komma → Punkt)
   PosSample (double, cm / Grad)  ← volle geparste Ziffern
        │
        ├─► Debug-Log POS / HTTP: %.6f mit X= Y= Z= YAW= YAWY=
        │
        └─► cepos_build_local: sample / 100.0 in double
                    │
                    ▼  (float)-Cast pro Feld
              CeposPacket (float32, Meter) → Base64 → TS Plugin-Command
                    │
                    └─► Debug-Log CEPOS SEND/RECV: Meter, %.6f
```

**Wichtig:** Auf dem **Wire** bleibt es float32 (~7 signifikante Stellen). Bei Koordinaten um 359309 cm sind das weiterhin ~0,03 cm Genauigkeit — das ist eine **IEEE-754-Grenze**, kein Parser-Bug. Der lokale `PosSample` behält die vollen Pos.txt-Ziffern für Logs und die `/100`-Rechnung; erst das Paket wird auf float32 reduziert.

YAW aus Unreal kann Komma-Dezimal haben (`-179,774`); vor `strtod` wird `,` → `.` normalisiert.

## 5. Wie wurde es getestet?

1. Plugin bauen, TS-Client neu starten (DLL-Reload).
2. In `plugin.cfg`: `debug=1`.
3. Spiel/Mod laufen lassen (Pos.txt oder HTTP POST).
4. **Vergleich:** Rohzeile in Pos.txt bzw. HTTP-Body vs. `DBG POS:` / `DBG HTTP:` — Nachkommastellen sollten übereinstimmen (6 Stellen im Log).
5. Optional: `DBG CEPOS: SEND` — Werte in **Metern** (`/100`), ebenfalls 6 Nachkommastellen.

## 6. Lerneffekt

Float ist für große Weltkoordinaten oft zu klein; `strtod` allein reicht nicht, wenn danach sofort gecastet wird. Trenne **Speicherpräzision** (double für eingehende Daten) von **Wire-Format** (float32, wenn das Protokoll es vorschreibt) und formatiere Logs so, dass sie die gespeicherten Werte widerspiegeln — sonst debuggt man gegen gerundete Anzeige statt gegen die echten Daten.
