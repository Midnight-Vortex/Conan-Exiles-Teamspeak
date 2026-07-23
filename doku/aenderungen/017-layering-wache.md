# 017 — Layering-Wache im CI (core/ ohne ts/ und ui/)

## Was wurde geaendert?

- Neues Skript `tests/check_layering.sh`.
- `tests/run_tests.sh` ruft es als zusaetzliche Suite `layering_guard` auf; ein
  **neuer** Verstoss laesst den kompletten Testlauf fehlschlagen.

## Was prueft die Wache?

Die V8-Schichtenregel (`doku/01-architektur-v8.md`):

```
ui/   → darf core/ nutzen
ts/   → darf core/ nutzen
core/ → darf NIE  ts/  oder  ui/  einbinden
```

Konkret: kein `.c`/`.h` unter `src/core/` darf `#include "ts/…"` oder
`#include "ui/…"` enthalten. Der SDK-Typ-Header
`teamspeak/public_definitions.h` ist **keine** ts/-Schicht (neutrale
Fremdbibliothek) und bleibt erlaubt.

## Warum eine automatische Wache?

Solche Regeln zerbroeseln ohne Wache: Beim naechsten schnellen Fix zieht jemand
"nur mal eben" einen ts/-Header in ein core/-Modul, und die isolierte
Testbarkeit ist wieder weg (genau die V7-Krankheit). Die Wache macht den
Rueckfall **sofort und maschinell sichtbar** — im selben grunen/roten Gate wie
die Unit-Tests.

## Die Allowlist (heutiger Stand, mit Nachweis)

Die Wache blockiert **neue** Verstoesse. Fuenf Dateien verletzen die Regel
heute noch — sie stehen mit Begruendung auf der Allowlist:

| Datei                                  | Grund |
|----------------------------------------|-------|
| `src/core/config/config_files.c`       | Legacy-Blob, faellt mit V8.5b |
| `src/core/validation/validation.c`     | Legacy-Blob, faellt mit V8.5b |
| `src/core/util/util_base.c`            | Legacy-Blob, faellt mit V8.5b |
| `src/core/proximity/proximity_volume.c`| Legacy-Blob, faellt mit V8.5b |
| `src/core/nick/nick_anonymize.c`       | TS-gekoppelt (ruft `ts3_*` direkt); Umzug nach `ts/` wie `channel_manage` fuer ein spaeteres V8.6-Teilpaket vorgesehen |

Nachweis (Grep zum Zeitpunkt dieser Aenderung — nach 6a/6b):

```
$ grep -rlE '#include[[:space:]]*"(ts|ui)/' src/core --include='*.c' --include='*.h'
src/core/config/config_files.c
src/core/nick/nick_anonymize.c
src/core/proximity/proximity_volume.c
src/core/util/util_base.c
src/core/validation/validation.c
```

Die ersten vier sind der alte Mumble-Blob (verschwindet mit V8.5b). Die fuenfte,
`nick_anonymize.c`, ist der gleiche Fall wie `channel_manage` (siehe `015`):
komplett callback-thread-/TS-gekoppelt, gehoert eigentlich nach `ts/`. Der
Umzug ist aber **nicht** Teil dieses Pakets, darum steht die Datei mit eigener
Begruendung auf der Allowlist statt unter "Legacy-Blob".

`channel_manage.c` und `voice_modes.c` stehen bewusst **nicht** mehr auf der
Liste — sie wurden in 6a/6b bereinigt.

### Update: aktueller Stand der Allowlist (nach V8.5b / V8.6)

Die Allowlist ist seit diesem Eintrag zweimal geschrumpft und enthaelt heute nur
noch **zwei** Dateien:

| Datei                                  | Grund |
|----------------------------------------|-------|
| `src/core/config/config_files.c`       | Legacy F10-Save-Bruecke, Umzug fuer V8.6/V8.7 vorgesehen |
| `src/core/util/util_base.c`            | Legacy geteilte Shims, Umzug fuer V8.6/V8.7 vorgesehen |

Weggefallen sind:

- `proximity_volume.c` — Datei geloescht (V8.5b, `doku/019`).
- `validation.c` — von `ts/`/`ui/` entkoppelt (V8.5b, `doku/019`); die
  Rest-Funktionen (Zonen-Geometrie + Hub-Heuristik-Validierer) bleiben, sind
  aber pur genug, dass kein `ts/`/`ui/`-Include mehr noetig ist (`doku/022`).
- `nick_anonymize.c` — nach `ts/nick/` verschoben (V8.6, `doku/021`), wie
  `channel_manage` in `015`.

Nachweis:

```
$ bash tests/check_layering.sh
  known (allowed): src/core/config/config_files.c
  known (allowed): src/core/util/util_base.c
  summary: 2 known-allowed, 0 new
LAYERING: OK
```

## Wie funktioniert das Skript?

1. Sucht alle core/-Dateien mit einem `ts/`- oder `ui/`-Include.
2. Jeder Treffer, der auf der Allowlist steht → "known (allowed)".
3. Jeder Treffer, der **nicht** drauf steht → "NEW VIOLATION" + Zeilennummer,
   und das Skript endet mit Exit-Code 1 (Gate rot).
4. Zusatz: steht ein Allowlist-Eintrag drauf, der gar nicht mehr verletzt
   (z. B. nach dem V8.5b-Abbau), warnt das Skript ("stale") — ohne das Gate
   rot zu faerben, damit das Aufraeumen nicht blockiert.

## Wie getestet?

- `bash tests/run_tests.sh` → Suite `layering_guard` PASS (5 known, 0 new),
  alle Suiten gruen.
- Gegenprobe: testweise `#include "ts/adapter/ts3_adapter.h"` in ein nicht
  gelistetes core/-Modul (`proximity_math.c`) → Skript meldet "NEW VIOLATION"
  und Exit-Code 1. Nach dem Zuruecknehmen wieder gruen.

## Lerneffekt

Eine Architektur-Regel ist nur so stark wie ihre Wache. Ein winziges
Grep-Skript im Gate verhindert dauerhaft den schleichenden Rueckfall — und die
dokumentierte Allowlist macht ehrlich sichtbar, welche Altlasten noch offen
sind, statt sie zu verstecken.
