# 005 — Version 8.0.0-dev

## Was wurde geaendert?

- `src/ts/entry/ts3_entry.c`: `ts3plugin_version()` liefert jetzt `"8.0.0-dev"` (vorher `"7.0.4"`).
- `REWRITE_PLAN_V8.md`: Status-Spalte ergaenzt — V8.0 bis V8.3 sind umgesetzt.

## Wie war es vorher (V7)?

Die Versionsnummer stand auf 7.0.4. Der V7-Rewrite-Plan hatte fuer die Rewrite-Phase
bewusst ein `-dev`-Suffix vorgeschlagen (damals `7.0.0-dev`), damit jeder im TS-Client
sofort sieht: "Das ist ein Entwicklungsstand, kein Release."

## Warum so?

Der V8-Rewrite laeuft ueber mehrere Phasen (V8.0–V8.9). Bis alle Phasen inkl.
TS-Client-Tests und Lasttest abgeschlossen sind, bleibt das Suffix `-dev`.
Das finale `8.0.0` gibt es erst in Phase V8.9 (Abnahme).

## Wie getestet?

`bash tests/run_tests.sh` (5 Suiten gruen) + `bash build/build_mingw.sh` (DLL baut).
Die Version erscheint beim Laden im TS-Client-Log ("BOOT: plugin version 8.0.0-dev starting").

## Lerneffekt

Versionsnummern sind Kommunikation: Ein `-dev`-Suffix kostet nichts und verhindert,
dass ein halbfertiger Stand versehentlich als Release verteilt wird.
