# 070 — Version 8.0.10: Release (Okklusions-Staerke `OcclusionIntensity`)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.10` (vorher `8.0.9`) |
| `README.md` | Hash-Tabelle um 8.0.10 ergaenzt (aeltere bleiben), Pruef-Befehle auf 8.0.10 |
| `doku/aenderungen/070-v8-0-10-release.md` | Dieser Eintrag (inkl. Changelog) |

## Changelog 8.0.9 → 8.0.10

| Nr. | Was | Standard |
|---|---|---|
| 069 | **Okklusions-Staerke im Root-Channel:** `[GLOBAL]` → `OcclusionIntensity=100` (Prozent, 0..300). 100 = bisherige Daempfung (−12 dB, Tiefpass 900 Hz bei voller Verdeckung), 150 = −18 dB / 250 Hz, 200 = −24 dB / 250 Hz, 300 = −36 dB. Tiefpass nie unter 250 Hz. | 100 |

Beispiel fuer Server-Admins (Root-Channel-Beschreibung):

```
[GLOBAL]
EnableOcclusion=True
OcclusionIntensity=150
```

Die Daempfung rechnet das Plugin **beim Zuhoerer** — jeder, der die neue Staerke hoeren soll,
braucht 8.0.10. Die Mod bleibt unveraendert (wie 8.0.8/8.0.9). Ohne den neuen Schluessel
verhaelt sich alles genau wie 8.0.9.

## 2. Wie war es vorher?

Die Daempfung bei voller Verdeckung war fest (−12 dB, 900 Hz). Im Test klang es hinter Waenden
nur „etwas dumpfer“ — zu schwach (siehe 069).

## 3. Warum ist die neue Loesung besser?

Die Staerke laesst sich im laufenden Test im Root-Channel aendern, ohne neues Plugin. Der
Standard 100 rechnet bitgenau wie vorher.

## 4. Wie funktioniert der Release-Ablauf? (wie 068)

```
Version 8.0.10 (ts3_entry.c) → .\build.ps1 -SkipDeploy (DLL + .ts3_plugin)
  → SHA256SUMS erzeugen, sha256sum -c pruefen → README-Hashes → Commit/Push
  → gh release create v8.0.10 --target <volle SHA> --draft <paket> SHA256SUMS
  → Digests pruefen → veroeffentlichen
```

Gemessen (Build 2026-10-18, Version 8.0.10):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.10-win64.ts3_plugin` (533325 Bytes) | `c9fcc2e56c4eb47c594075830e8de4d1057226fab0c648e22022f784c782818d` |
| `plugins/conan_exiles.dll` (7381504 Bytes) | `f243b9765080132f5bd8df25b7d06e6ff9fa4bf9070b40b0d43ec1268c79bb4b` |

## 5. Wie wurde es getestet?

- MSBuild Release|x64 exit 0, Paket enthaelt `package.ini` (`Version = 8.0.10`) und
  `plugins/conan_exiles.dll` (Forward-Slash), DLL im Paket = `bin\conan_exiles.dll`.
- `sha256sum -c SHA256SUMS`: beide Zeilen `OK`.
- Unit-Tests (MSVC): `hrtf_test`, `hub_parser_test`, `occlusion_test`, `talk_state_test` gruen;
  `check_layering.sh` OK.
- **Noch nicht** im TeamSpeak-Client mit Mitspielern gehoert.

## 6. Lerneffekt

`gh release create --target` braucht die volle Commit-SHA; eine kurze SHA lehnt GitHub mit
HTTP 422 ab (bei 8.0.9 passiert).
