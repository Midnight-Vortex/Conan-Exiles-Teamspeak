# 081 — CI wieder grün: fehlendes `#include <stddef.h>` in `occlusion_params.c`

2026-10-28. Nur Plugin (Build/Tests), kein Verhaltens-Unterschied im TS-Client.

## 1. Was wurde geändert?

`src/core/proximity/occlusion_params.c` bindet jetzt `<stddef.h>` ein. Die Datei benutzt
`NULL` (in `occlusion_mod_param_name()`), hat den Header aber nie eingebunden.

## 2. Wie war es vorher?

- **MSVC** (Release-Build unter Windows) hat `NULL` „zufällig“ über andere Header bekommen —
  der DLL-Build lief also.
- **gcc unter Linux** (Host-Unit-Tests in `tests/run_tests.sh`) bindet nur die Header ein, die
  wirklich da stehen. Dort war `NULL` unbekannt → **`hub_parser_test` und `occlusion_test`
  ließen sich nicht kompilieren**.
- Folge im CI (`.github/workflows/ci.yml`): Der Schritt „Run host unit tests“ schlug seit
  Commit f1d0aec (Occlusion v2, Schritt 1) bei **jedem** Push auf `tsmain` fehl. Weil danach
  abgebrochen wird, lief auch der Schritt „MinGW cross-build“ nie mehr. Das Sicherheitsnetz
  („Tests grün + DLL linkt“) war also über acht Releases hinweg aus, ohne dass es auffiel.

## 3. Warum ist es jetzt besser?

Eine Datei muss die Header einbinden, deren Namen sie benutzt („include what you use“).
Dann kompiliert sie mit jedem Compiler gleich — und das CI prüft wieder wirklich etwas.

## 4. Wie funktioniert es?

`NULL` ist im C-Standard in `<stddef.h>` definiert (auch in `<stdio.h>`, `<stdlib.h>`,
`<string.h>` …). Wer `NULL` benutzt und keinen dieser Header einbindet, verlässt sich darauf,
dass ein anderer Header ihn „mitbringt“ — das ist von Compiler zu Compiler verschieden.

## 5. Test

- `bash tests/run_tests.sh` lokal (gcc 13): **alle 17 Suites grün** (vorher 2 Compile-Fehler).
- MinGW-Cross-Build (`bash build/build_mingw.sh`) linkt.

## 6. Lerneffekt

Ein rotes CI, das niemand anschaut, schützt nicht. Wenn der Windows-Build „geht“, heißt das
nicht, dass der Code portabel ist — gerade dafür gibt es den zweiten Compiler im CI.
