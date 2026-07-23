# 001 — Build- und Test-Fundament (V8.0 + V8.1)

## Was wurde geaendert?

**Neue Dateien:**

- `build/build_mingw.sh` — Cross-Build-Skript: baut die komplette Plugin-DLL
  auf Linux mit MinGW (einem GCC-Compiler, der Windows-Programme erzeugt).
- `src/core/util/compat_crt.h` — Portabilitaets-Header fuer die MSVC-eigenen
  CRT-Funktionen (`strncpy_s`, `strtok_s`, `_strnicmp` usw.).
- `tests/run_tests.sh` — Test-Runner: kompiliert und startet alle Unit-Tests
  mit normalem gcc auf Linux, meldet PASS/FAIL pro Suite.
- `tests/proximity_math_test.c` — Tests fuer die Audio-Mathematik
  (Distanz, Lautstaerke-Kurve, Stereo-Pan, Lowpass, DRR, Rear-Daempfung).
- `tests/zone_resolve_test.c` — Tests fuer die Zonen-Logik
  (Punkt-in-Zone, Hoehenband, Soundproof-Einbahnregel, Reverb-Flag).
- `tests/player_table_test.c` — Tests fuer die Spieler-Tabelle
  (put/get, Verfall, LRU-Verdraengung, Grenzwerte).
- `tests/support/win32_shim/` — Test-eigener Mini-Ersatz fuer `<windows.h>`
  (`CRITICAL_SECTION` → pthread-Mutex, `GetTickCount64` → monotone Uhr mit
  vorspulbarem Offset, `InterlockedIncrement` → Atomic-Builtin).

**Geloescht:**

- `tests/*.obj`, `tests/*.exe` — versehentlich committete Build-Artefakte.
- `tests/run_test.bat` — hart auf einen VS-Pfad verdrahtet, ersetzt durch
  `run_tests.sh` (Windows baut weiterhin ueber MSVC/`build_msvc.ps1`).
- `src/ui/dialogs/ui_settings.c` + `.h` — tote Datei, war nicht im Build
  (per grep verifiziert: keine einzige Code-Referenz ausser sich selbst;
  der F10-Dialog laeuft ueber `showConfigInterface()` in `ui_main.c`).

**Minimal angepasst (rein mechanisch, kein Verhalten geaendert):**

- `.gitignore` — jetzt auch `*.obj`, `*.exe`, `*.dll`, `*.pdb`, `tests/out/`.
- `src/core/hub/hub_parser.c`, `src/core/proximity/player_table.c` —
  je eine Zeile `#include "core/util/compat_crt.h"` (auf Windows ein No-Op).
- `src/core/mod_file/pos_file.c` — fehlendes `#include <math.h>` ergaenzt
  (nutzte `isfinite`/`fabsf` ohne Header; MSVC hat das stillschweigend
  aufgeloest, GCC bricht beim Linken ab).
- `src/ui/dialogs/ui_main.c` — `#pragma comment(lib, "uxtheme.lib")` in
  `#ifdef _MSC_VER` eingepackt (GCC kennt dieses Pragma nicht; die Lib wird
  im MinGW-Build ueber `-luxtheme` gelinkt).
- `tests/hub_parser_test.c` — Include von `compat_crt.h`, damit `strcat_s`
  auch mit gcc baut.

## Wie war es vorher (V7)?

Kernproblem 6 aus dem V8-Audit: **kein Sicherheitsnetz.** Es gab genau einen
Unit-Test, der nur auf dem einen Windows-PC mit Visual Studio baubar war
(`run_test.bat` mit hart codiertem VS-Pfad). Kompilierte `.obj`/`.exe`-Dateien
lagen im Git. Ob eine Aenderung ueberhaupt baut, konnte man nur auf diesem
einen Rechner pruefen — jede Aenderung war ein Blindflug.

## Warum ist die neue Loesung besser/stabiler?

1. **Jeder Rechner kann pruefen:** Die Tests laufen mit normalem gcc auf
   Linux, der komplette DLL-Build laeuft per MinGW-Cross-Compiler. Ein Agent
   oder CI-Server kann damit VOR jedem Commit maschinell feststellen:
   "baut + Tests gruen".
2. **Vier Module sind jetzt abgesichert:** `hub_parser`, `zone_resolve`,
   `proximity_math`, `player_table` — genau die puren Kernmodule, auf die
   sich die spaeteren V8-Phasen (Threading-Umbau!) stuetzen. Faellt dort
   etwas um, schlaegt sofort ein Test fehl statt erst im TS-Client.
3. **Keine Binaries mehr im Git:** `.gitignore` verhindert, dass Artefakte
   wieder einsickern (Binaries in Git blaehen das Repo auf und veralten).
4. **MSVC bleibt unangetastet:** `compat_crt.h` ist auf MSVC und MinGW ein
   No-Op (dort existieren die `_s`-Funktionen nativ). Nur der reine
   Linux-gcc-Testbuild bekommt duenne Wrapper. Der Windows-Release-Build
   erzeugt also weiterhin exakt denselben Code wie vorher.

## Wie funktioniert es jetzt?

```
                    Linux-Rechner (oder CI)
                    ┌──────────────────────────────────────┐
  Quellcode ───────▶│ tests/run_tests.sh   (gcc, Host)     │──▶ PASS/FAIL je Suite
   src/core/...     │   nutzt compat_crt.h + win32_shim    │
                    ├──────────────────────────────────────┤
  alle 29 .c ──────▶│ build/build_mingw.sh (MinGW, Cross)  │──▶ bin/mingw/conan_exiles.dll
   + Resource.rc    │   gleiche Quellliste wie .vcxproj    │
                    └──────────────────────────────────────┘
  Windows-PC:  build/build_msvc.ps1 (MSVC)  ──▶ Release-DLL  (unveraendert)
```

- **`compat_crt.h`** hat drei Zweige: MSVC → nichts tun; MinGW → nichts tun
  (der Build setzt `-DMINGW_HAS_SECURE_API=1`, dann liefert die MinGW-CRT die
  `_s`-Funktionen selbst); reiner gcc → kleine `static inline`-Wrapper, die
  z. B. `strncpy_s(dst, size, src, _TRUNCATE)` auf sicheres Kopieren mit
  Abschneiden abbilden.
- **`win32_shim`** ist ein Fake-`<windows.h>`, das NUR der Test-Runner per
  Include-Pfad einblendet. `player_table.c` kompiliert damit unveraendert:
  sein `CRITICAL_SECTION`-Lock wird zum pthread-Mutex, `GetTickCount64()`
  liefert eine monotone Uhr, die der Test per `win32_shim_advance_ms()`
  vorspulen kann — so laesst sich der 120-Sekunden-Verfall in Millisekunden
  testen statt echt zu warten.
- **`build_mingw.sh`** kompiliert exakt die 29 `.c`-Dateien aus der
  `.vcxproj` mit denselben Defines (Unicode usw.), wandelt `Resource.rc`
  (liegt als UTF-16 vor, windres liest nur 8-Bit) in eine Build-lokale
  UTF-8-Kopie um und linkt die DLL inklusive aller 28 `ts3plugin_*`-Exporte.
  `Msimg32.lib` aus der vcxproj wird nicht gelinkt — es gibt keinen einzigen
  Aufruf (`GradientFill`/`AlphaBlend`/`TransparentBlt`) im Quellcode.

## Wie wurde es getestet?

- `bash tests/run_tests.sh` → 4 Suiten, 144 Checks, alle gruen.
- `bash build/build_mingw.sh` → `OK: built bin/mingw/conan_exiles.dll`,
  per `objdump` verifiziert: PE32+-DLL mit allen 28 `ts3plugin_*`-Exporten.
- Kein TS-Client-Test noetig: kein Plugin-Verhalten geaendert (nur ein
  fehlendes `#include <math.h>` und ein Pragma-Guard, beides ohne Effekt
  auf den MSVC-Build).

## Lerneffekt

Ein Sicherheitsnetz baut man ZUERST, nicht nachtraeglich: Jede weitere
V8-Phase kann jetzt maschinell verifiziert werden, bevor ein Mensch den
TS-Client anwerfen muss. Und: Portabilitaet erzwingt Sauberkeit — der
Cross-Build fand sofort einen echten Schlamper (fehlendes `math.h`), den
MSVC jahrelang stillschweigend verdeckt hat.
