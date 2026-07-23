# 007 — Schichten-Fix: nick_anonymize.h zieht nicht mehr die TS-Schicht

## Was wurde geaendert?

- `src/core/nick/nick_anonymize.h`: Der Include `#include "ts/adapter/ts3_adapter.h"`
  wurde durch `#include "teamspeak/public_definitions.h"` ersetzt.
- `src/core/nick/nick_anonymize.c`: bekommt den Adapter-Include jetzt direkt
  (er ruft ja die TS-API auf — das ist ein Implementierungs-Detail der `.c`, kein
  Teil des oeffentlichen Vertrags).

## Wie war es vorher (V7)?

Die Header-Datei eines `core/`-Moduls zog die komplette `ts/`-Schicht herein
(`ts3_adapter.h`), obwohl sie davon nur den Zahlentyp `uint64` brauchte (fuer
`nick_anonymize_before_ingame(uint64 ingameChannelID)`). Jeder, der `nick_anonymize.h`
einbindet, bekam damit unnoetig die ganze Adapter-Schnittstelle mitgeliefert.

## Warum ist das besser?

V8 hat eine klare Schichtenregel (`doku/01-architektur-v8.md`):

```
ui/   → darf core/ nutzen
ts/   → darf core/ nutzen
core/ → darf NIE ts/ oder ui/ einbinden   ← das war hier verletzt
```

**Warum ist die Regel wichtig?** Nur wenn `core/`-Header nichts von `ts/`/`ui/` wissen,
lassen sich die puren Module isoliert auf jedem Rechner kompilieren und testen
(gcc-Unit-Tests). Der SDK-Typ-Header `teamspeak/public_definitions.h` ist **nicht** unsere
`ts/`-Schicht, sondern eine neutrale Fremdbibliothek — den zu nutzen ist erlaubt.

## Wie funktioniert es jetzt?

Der Typ `uint64` kommt direkt aus dem SDK-Header. Die eigentlichen TS-API-Aufrufe
stehen weiterhin nur in der `.c`, die den Adapter selbst einbindet. Am Verhalten
aendert sich **nichts** — es ist reine Include-Hygiene.

## Wie getestet?

`bash build/build_mingw.sh` — die DLL baut sauber. Alle drei Nutzer des Headers
(`ts3_entry.c`, `channel_manage.c`, `nick_anonymize.c`) kompilieren und linken weiterhin.
Kein Laufzeit-Verhalten betroffen, daher kein TS-Client-Test noetig.

## Lerneffekt

Ein Header sollte nur das einbinden, was seine oeffentliche Schnittstelle wirklich
braucht (hier: ein Zahlentyp). "Der Include war halt schon da" ist kein Grund — jede
unnoetige Abhaengigkeit in einem Header vererbt sich an alle Nutzer und macht das
Projekt schwerer testbar.
