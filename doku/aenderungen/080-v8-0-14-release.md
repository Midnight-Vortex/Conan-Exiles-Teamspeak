# 080 — Version 8.0.14: Release (Lautsprecher-Pflicht, Support-Kanäle)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| Plugin-Code (Commit 317ff33) | `ForceSpeakersOn` und Support-/Event-Kanäle, siehe 079 |
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.14` |
| `README.md` | Hash-Tabelle um 8.0.14 ergänzt, Prüf-Befehle auf 8.0.14 |
| `doku/aenderungen/080-v8-0-14-release.md` | Dieser Eintrag |

## Changelog 8.0.13 → 8.0.14

| Nr. | Änderung | Vorher |
|---|---|---|
| 1 | Neuer Root-Schalter `ForceSpeakersOn` (Standard aus): Wer im Ingame-Channel die Lautsprecher in TS stumm schaltet, kommt in den Hub (Hinweis im Chat) und mit eingeschalteten Lautsprechern zurück | Man konnte mit stummen Lautsprechern im Ingame-Channel sitzen |
| 2 | Bei `ForceSpeakersOn` hört man Mitspieler mit stummen Lautsprechern nicht mehr | — |
| 3 | Kanäle mit „Support“ oder „Event“ irgendwo im Namen (auch „Eventhalle“, „Supportraum“) schieben niemanden automatisch heraus | Nur „Event“/„Plot“ als ganzes Wort |

Keine Änderung an Klang, Okklusion, Passwort-Weg (077) oder der Mod.

## 2. Wie wurde es geprüft?

- Unit-Tests (MSVC): `hub_parser_test` (inkl. `ForceSpeakersOn`), `mod_password_test`, `occlusion_test`,
  `hrtf_test`, `talk_state_test` grün; `check_layering.sh` OK; MSBuild Release|x64 ohne Warnungen.
- Schlüsselwort-Regel für Kanalnamen einzeln geprüft (15 Namen).
- Paket: `Version = 8.0.14`, DLL im Paket = `bin\conan_exiles.dll`, `sha256sum -c` OK.

Gemessen (Build 2026-10-21, Version 8.0.14):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.14-win64.ts3_plugin` (540372 Bytes) | `b83b23656b2f1a6ff251ec1556c40d85e75770d960ba0dca97118d0e4164dd2c` |
| `plugins/conan_exiles.dll` (7393792 Bytes) | `d3839f487a960da139ec32fbb47627a81ef9311b1eb6d634a7c5e8ce41b1d2fc` |

**Noch nicht** im echten TS getestet (Lautsprecher stumm → Hub → zurück, stummer Mitspieler, Support-Kanal).
