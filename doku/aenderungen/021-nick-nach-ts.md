# 021 — nick_anonymize von core/ nach ts/ verschoben

**Phase:** V8.6 (Schichten-Sanierung) · **Muster:** exakt wie `channel_manage`
in `doku/aenderungen/015` · **Lektion:** `02-lessons-learned-v7.md` Lektion 5
(Schichten-Verletzung).

## Was wurde geaendert?

- `src/core/nick/nick_anonymize.c` → `src/ts/nick/nick_anonymize.c`
- `src/core/nick/nick_anonymize.h` → `src/ts/nick/nick_anonymize.h`
- Header-Guard umbenannt: `CORE_NICK_NICK_ANONYMIZE_H` → `TS_NICK_NICK_ANONYMIZE_H`
- Eigener Include der `.c` angepasst: `"core/nick/nick_anonymize.h"` →
  `"ts/nick/nick_anonymize.h"`
- Alle Einbinder auf den neuen Pfad umgestellt:
  `src/ts/entry/ts3_entry.c`, `src/ts/channel/channel_manage.c`
  (und der Selbst-Include in `nick_anonymize.c`).
- Build-Listen aktualisiert: `project/Conan-Exiles-TeamSpeak.vcxproj`
  (`ClCompile` + `ClInclude`) und `build/build_mingw.sh` (Quellenliste).
- `nick_anonymize.c` von der Layering-Allowlist in `tests/check_layering.sh`
  gestrichen (siehe unten).

Am Code **innerhalb** des Moduls wurde nichts geaendert (nur Header-Guard und der
eigene Include-Pfad). Verhalten identisch.

## Wie war es vorher?

Das Modul lag unter `src/core/nick/`, band aber `ts/adapter/ts3_adapter.h` ein
und rief die TS-API direkt (`ts3_get_channel_client_list`,
`ts3_set_own_nickname`, `ts3_get_own_nickname`, `ts3_is_connected`,
`ts3_thread_is_callback`, …). Es verletzte damit die V8-Schichtenregel
(`core/` darf NIE `ts/`/`ui/` einbinden) und stand deshalb mit eigener
Begruendung auf der Allowlist der Layering-Wache (`doku/017`).

## Warum umziehen (statt entkoppeln wie bei voice_modes)?

Wie bei `channel_manage` (`015`) gilt: `nick_anonymize` ist **kein** purer Kern.
Sein Thread-Vertrag steht oben im Header — "TS callback thread ONLY". Fast jede
Funktion treibt die TS-API ueber den Adapter. Die zwei wirklich puren Helfer
(`nick_make_random`, `nick_looks_anonymized`) sind Zahlen-Mathe ohne TS-Bezug,
aber sie leben in derselben `.c`, die `windows.h` und den Adapter zieht.

```
vorher: core/nick/  (falsche Schicht, band ts/adapter ein)   ← Regelverstoss
jetzt:  ts/nick/     (richtige Schicht, darf ts/ nutzen)
```

Die ehrlichste Loesung ist der **Umzug**, nicht eine kuenstliche Aufspaltung:
Wer ein zu 100 % TS-gekoppeltes Modul aufspaltet, baut nur eine Attrappe
(Hook-Schicht ohne echten puren Kern). Eine Hook-Inversion wie bei
`voice_modes` (`016`) lohnt nur, wenn wirklich ein testbarer Kern uebrig bleibt.

## Warum kein Host-Test fuer die puren Helfer?

Der Auftrag erlaubt einen Host-Test fuer `nick_make_random` /
`nick_looks_anonymized`, **wenn** er ohne erzwungene Extraktion machbar ist —
ist er hier **nicht**: beide Helfer stehen in `nick_anonymize.c`, und diese
Datei bindet `windows.h`, `InterlockedCompareExchange` und den TS-Adapter ein.
Auf einem Linux-Host (gcc) ist sie nicht kompilierbar, ohne die Helfer in eine
eigene pure `.c` herauszuloesen. Genau diese Extraktion soll laut Auftrag
**nicht** erzwungen werden — das Modul bleibt eine Datei, nur verschoben. Ein
Host-Test entfaellt daher bewusst.

## Wie funktioniert es jetzt?

Unveraendert. `nick_on_connected()`, `nick_anonymize_before_ingame()`,
`nick_restore_in_hub()` und `nick_reset()` laufen weiter auf dem
Callback-Thread (aus `channel_manage` / `ts3_entry`). Nur der Datei-Pfad und der
Include-Guard sind neu.

## Layering-Allowlist: 3 → 2

`tests/check_layering.sh` wurde entsprechend gekuerzt (`nick_anonymize.c`
gestrichen):

| vorher | nachher |
|---|---|
| config_files.c, util_base.c, nick_anonymize.c | config_files.c, util_base.c |

Nachweis nach der Aenderung:

```
$ bash tests/check_layering.sh
  known (allowed): src/core/config/config_files.c
  known (allowed): src/core/util/util_base.c
  summary: 2 known-allowed, 0 new
LAYERING: OK
```

## Wie getestet?

- `xmllint --noout project/Conan-Exiles-TeamSpeak.vcxproj` — Projektdatei wohlgeformt.
- `bash build/build_mingw.sh` — DLL baut und linkt.
- `bash tests/run_tests.sh` — alle Suiten gruen (`layering_guard`: 2 known, 0 new).

Kein Laufzeit-Verhalten betroffen (reiner Umzug), daher kein TS-Client-Test noetig.

## Lerneffekt

Ein Schichtenverstoss hat zwei moegliche Ursachen: falsche Abhaengigkeit ODER
falsche Schicht. Bei `nick_anonymize` war es — wie bei `channel_manage` — die
falsche Schicht. Dann ist der Umzug (`git mv`) die kleinste korrekte Loesung,
und die Allowlist der Layering-Wache schrumpft um genau diesen echten Eintrag.
