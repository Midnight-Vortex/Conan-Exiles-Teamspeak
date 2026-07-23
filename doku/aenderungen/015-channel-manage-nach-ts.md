# 015 — channel_manage von core/ nach ts/ verschoben

## Was wurde geaendert?

- `src/core/channel/channel_manage.c` → `src/ts/channel/channel_manage.c`
- `src/core/channel/channel_manage.h` → `src/ts/channel/channel_manage.h`
- Header-Guard umbenannt: `CORE_CHANNEL_CHANNEL_MANAGE_H` → `TS_CHANNEL_CHANNEL_MANAGE_H`
- Eigener Include der `.c` angepasst: `"core/channel/channel_manage.h"` →
  `"ts/channel/channel_manage.h"`
- Alle Einbinder auf den neuen Pfad umgestellt:
  `src/ts/entry/ts3_entry.c`, `src/ui/plugin_ui_compat.c`.
- Build-Listen aktualisiert: `project/Conan-Exiles-TeamSpeak.vcxproj`
  (ClCompile + ClInclude) und `build/build_mingw.sh` (Quellenliste).

Am Code **innerhalb** des Moduls wurde nichts geaendert (nur Header-Guard und der
eigene Include-Pfad). Verhalten identisch.

## Wie war es vorher?

Das Modul lag unter `src/core/`, band aber `ts/adapter`, `ts/proximity` und
`ts/profile` ein — es verletzte damit die V8-Schichtenregel (`core/` darf NIE
`ts/`/`ui/` einbinden).

## Warum ist das besser (statt Entkoppeln wie bei voice_modes)?

Ein Audit hat gezeigt: `channel_manage` ist **kein** purer Kern. Jede Funktion
hier ist laut Thread-Vertrag "TS callback thread ONLY" und ruft direkt die
TS-API (`ts3_get_channel_list`, `ts3_request_client_move`, `ts3_audio_set_mode`,
…). Das Modul lebte also nur an der falschen Stelle. Die ehrlichste Loesung ist
darum **umziehen**, nicht kuenstlich aufspalten:

```
vorher: core/channel/  (falsche Schicht, band ts/ ein)   ← Regelverstoss
jetzt:  ts/channel/     (richtige Schicht, darf ts/ nutzen)
```

Ergebnis: `core/` bleibt frei von diesem Modul, und die Schichtenregel gilt
wieder. Wer entkoppelt statt umzieht, wenn ein Modul zu 100 % TS-gekoppelt ist,
baut nur eine Attrappe (Hook-Schicht ohne echten puren Kern).

## Wie funktioniert es jetzt?

Unveraendert. `chan_tick()` laeuft weiter aus dem CEDRAIN-Drain (Callback-Thread),
`chan_signal_position_update()` wird weiter vom Pos-Watcher aufgerufen und fordert
nur einen Wakeup an. Nur der Datei-Pfad und der Include-Guard sind neu.

## Wie getestet?

- `xmllint --noout project/Conan-Exiles-TeamSpeak.vcxproj` — Projektdatei wohlgeformt.
- `bash build/build_mingw.sh` — DLL baut und linkt.
- `bash tests/run_tests.sh` — alle Suiten gruen (251 Checks).

Kein Laufzeit-Verhalten betroffen (reiner Umzug), daher kein TS-Client-Test noetig.

## Lerneffekt

Ein Schichtenverstoss hat zwei moegliche Ursachen: falsche Abhaengigkeit ODER
falsche Schicht. Bei `channel_manage` war es die falsche Schicht — dann ist der
Umzug (`git mv`) die kleinste korrekte Loesung. Hooks/Inversion (siehe `016`)
lohnen sich nur, wenn wirklich ein purer Kern uebrig bleibt.
