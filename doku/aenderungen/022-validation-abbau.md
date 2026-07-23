# 022 — validation.c: Legacy-Abbau, Stand geprueft und Rest-Surface verkleinert

**Phase:** V8.6 (Fortsetzung des Legacy-Abbaus aus `doku/019`) · **Muster:**
wie der Teardown in `019` — nur dort umleiten/loeschen, wo es 1:1 sauber ist;
verzahnte Teile bewusst stehen lassen (begruendet). **Lektion:**
`02-lessons-learned-v7.md` Lektion 4 (Doppel-Besitzer) + 5 (Schichten).

## Ausgangslage (wichtig)

`validation.c` wurde bereits in V8.5b (`doku/019`) von der `ts/`-/`ui/`-Schicht
**entkoppelt** und von der Layering-Allowlist gestrichen. Die urspruengliche
Annahme dieses Pakets ("`validation.c` bindet noch `ts/`/`ui/` ein, steht
deshalb auf der Allowlist") war zum Startzeitpunkt also schon ueberholt. Dieses
Paket hat darum:

1. jede verbliebene Funktion erneut gegen ihre Aufrufer und gegen
   `zone_resolve`/`config_clamp` geprueft (Recherche unten),
2. die einzige noch sichere Verkleinerung durchgefuehrt: zwei nur intern
   benutzte Helfer wurden `static` gemacht und ihre toten oeffentlichen
   Deklarationen aus `plugin_modules.h` entfernt.

## Recherche: jede Funktion + ihre Aufrufer

Grep ueber `src/` (Stand vor diesem Paket):

| Funktion                   | Aufrufer                                   | Duplikat von …?                         |
|----------------------------|--------------------------------------------|-----------------------------------------|
| `shouldApplyDistanceLimits`| `ui_dynamic.c` (2×), `ui_messages.c` (6×)  | nein — Hub-/Verbindungs-Zustands-Gate   |
| `shouldValidateValue`      | `ui_dynamic.c` (1×)                         | nein — Ziffern-Heuristik                |
| `validateDistanceValue`    | `ui_dynamic.c` (4×)                         | nein — Hub-Min/Max-Clamp (nicht `config_clamp`) |
| `isPointInPolygon`         | nur intern in `validation.c`               | Algorithmus = `zone_resolve`-Helfer, aber nur intern genutzt |
| `zoneContainsPoint`        | nur intern in `validation.c`               | dito                                    |
| `getPlayerZoneAtScale`     | nur intern (`static`)                       | dito                                    |
| `getPlayerZone`            | `plugin_ui_compat.c` (4×)                   | Algorithmus = `zone_resolve`, aber liest Legacy-`zones[]` |

## Disposition pro Funktion

| Funktion                   | Entscheidung | Begruendung |
|----------------------------|--------------|-------------|
| `shouldApplyDistanceLimits`| **behalten** | Lebt (8 Aufrufer). Prueft `isConnectedToServer`, `rootChannelID`, `hubDescriptionAvailable`, `hubForceDistanceBasedMuting` und loggt via `mumbleAPI` — reines Hub-/Verbindungs-Gate, kein Gegenstueck in `zone_resolve`/`config_clamp`. Ohne TS-Client-Test nicht sicher umzubiegen. |
| `shouldValidateValue`      | **behalten** | Lebt (`ui_dynamic.c`). Ziffern-Heuristik ("hat der Eingabewert genug signifikante Stellen?") — kein Duplikat, keine 1:1-Umleitung moeglich. |
| `validateDistanceValue`    | **behalten** | Lebt (`ui_dynamic.c`). Clamp gegen **Hub**-Min/Max (`hubMinimumWhisper` …), gewaechtert durch `shouldApplyDistanceLimits`. Das ist NICHT `config_clamp` (feste 0.5..500) — Umbiegen waere eine Verhaltensaenderung. |
| `isPointInPolygon`         | **static gemacht + Deklaration geloescht** | Nur intern von `zoneContainsPoint` genutzt (grep-bewiesen kein externer Aufrufer). Die oeffentliche Deklaration in `plugin_modules.h` war tot. |
| `zoneContainsPoint`        | **static gemacht + Deklaration geloescht** | Nur intern von `getPlayerZoneAtScale` genutzt. Oeffentliche Deklaration tot. |
| `getPlayerZoneAtScale`     | **behalten (bereits `static`)** | Interner Helfer von `getPlayerZone`. |
| `getPlayerZone`            | **behalten** | Lebt (`plugin_ui_compat.c`, 4×). Der Algorithmus ist inzwischen deckungsgleich mit `zone_resolve`, ABER er liest die Legacy-Globalen `zones[]`/`zoneCount` (Typ `Zone`), waehrend `zone_resolve` eine `HubSettings`/`HubZone`-Struktur nimmt. Eine Umleitung waere **kein** 1:1 am Aufrufer: man muesste an jeder Stelle eine `HubSettings` aus den Legacy-Globalen bauen — Struktur-/Verhaltensrisiko im Soundproof-/Reverb-Pfad. Bleibt bewusst stehen (wie in `019` begruendet). |

**Ergebnis:** kein toter Code loeschbar (alle Funktionen leben), keine saubere
1:1-Umleitung auf `zone_resolve`/`config_clamp` moeglich. Die einzige sichere,
verhaltensneutrale Verkleinerung war die Sichtbarkeit: zwei Helfer sind jetzt
`static`, zwei tote Deklarationen sind aus `plugin_modules.h` weg.

## Was wurde konkret geaendert?

- `src/core/validation/validation.c`: `isPointInPolygon` und `zoneContainsPoint`
  von externer Bindung auf `static` umgestellt (nur intern genutzt).
- `src/plugin_modules.h`: die beiden dazugehoerigen Deklarationen entfernt
  (waren "dangling" — kein externer Aufrufer, genau wie die 30 Zeilen in
  `019` Stage 3).

Kein Verhalten veraendert: die internen Aufruf-Ketten
(`getPlayerZone` → `getPlayerZoneAtScale` → `zoneContainsPoint` →
`isPointInPolygon`) bleiben unveraendert; die DLL linkt weiter.

## Warum wird `validation.c` nicht ganz geloescht?

Weil noch vier lebende, **nicht** 1:1-ersetzbare Funktionen drin stehen
(`shouldApplyDistanceLimits`, `shouldValidateValue`, `validateDistanceValue`,
`getPlayerZone`). Ein Loeschen oder Umbiegen ohne TS-Client-Hoertest waere ein
Verhaltensrisiko im Audio-/Soundproof-Pfad — genau das ist laut Auftrag
verboten (nur reine Relocation, Tot-/Duplikat-Abbau, 1:1-Umleitungen).

## Layering-Allowlist

`validation.c` steht bereits seit `019` **nicht** mehr auf der Allowlist und
bindet keinen `ts/`-/`ui/`-Header ein. Dieses Paket aendert daran nichts — die
Datei bleibt off-allowlist. Endstand der Allowlist (nach `021` + `022`):

```
$ bash tests/check_layering.sh
  known (allowed): src/core/config/config_files.c
  known (allowed): src/core/util/util_base.c
  summary: 2 known-allowed, 0 new
LAYERING: OK
```

## Wie getestet?

- `bash tests/run_tests.sh` → alle Suiten gruen (`layering_guard`: 2 known, 0 new).
  Die Zonen-Geometrie ist zusaetzlich in `zone_resolve_test` abgedeckt — das ist
  die **kanonische** Implementierung; `getPlayerZone` teilt denselben Algorithmus,
  laeuft aber auf den Legacy-Globalen und wird darum nicht gesondert host-getestet.
- `bash build/build_mingw.sh` → DLL linkt weiter (Groesse minimal kleiner, weil
  die zwei Symbole keine externe Bindung mehr haben — Beweis, dass sie niemand
  von aussen brauchte).
- `xmllint --noout project/Conan-Exiles-TeamSpeak.vcxproj` → unveraendert OK
  (keine Datei hinzugefuegt/entfernt).

## Lerneffekt

Nicht jeder Abbau endet mit einer geloeschten Datei. Wenn die Recherche zeigt,
dass alle Rest-Funktionen leben **und** kein sauberes 1:1-Gegenstueck haben, ist
die ehrliche Antwort: dokumentieren, warum sie bleiben — und wenigstens die tote
oeffentliche Surface (nur-intern-Helfer als `static`, tote Deklarationen weg)
verkleinern. Ein "erzwungenes" Umbiegen auf `zone_resolve` waere eine stille
Verhaltensaenderung gewesen; die vermeidet man, indem man Semantik **und**
Datenquelle vergleicht, nicht nur den Algorithmus.
