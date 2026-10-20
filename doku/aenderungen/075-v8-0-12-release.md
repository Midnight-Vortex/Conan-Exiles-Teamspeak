# 075 — Version 8.0.12: Release (Fixes aus dem Codex-Review der Okklusion v2)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/proximity/occlusion.c` | vier Fixes aus dem Codex-Review (Commit 51c0910, siehe Changelog) |
| `tests/occlusion_test.c` | neue Checks „new relay point is approached smoothly“, „store while off is rejected“, „missing OCC key still -1 while off“; `test_switch` laeuft zuerst, danach wird der Schalter fuer die uebrigen Suiten eingeschaltet |
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.12` |
| `README.md` | Hash-Tabelle um 8.0.12 ergaenzt, Pruef-Befehle auf 8.0.12 |
| `doku/aenderungen/075-v8-0-12-release.md` | Dieser Eintrag |

## Changelog 8.0.11 → 8.0.12

| Nr. | Fix | Auswirkung vorher |
|---|---|---|
| 1 | Float ↔ Bitmuster ueber `uint32_t` | Auf 64-Bit-Linux (Host-Tests) las `memcpy` 8 statt 4 Byte (undefiniertes Verhalten); Windows-Plugin nicht betroffen |
| 2 | Seqlock pro Spieler um einen OCC-Eintrag, Leser wiederholen | Beim Wechsel „um die Ecke“ → „durch die Wand“ konnte kurz ein Umweg-Punkt 0,0,0 gelesen werden — die Stimme zog fuer ~¼ s Richtung Weltursprung |
| 3 | Umweg-Punkt selbst geglaettet (Release-Zeit) | Wechselte die Oeffnung (z. B. linke ↔ rechte Seite), sprang die Richtung trotz Glaettung |
| 4 | Schreibsperre fuer Speichern + Loeschen, Speichern prueft den Schalter unter der Sperre | Beim Abschalten konnte ein gleichzeitig gespeicherter Eintrag das Loeschen ueberleben (bis 1 s wirksam nach schnellem Wiedereinschalten) |

Keine Aenderung an Klang, Protokoll oder Root-Channel-Schluesseln; die Mod (074) bleibt unveraendert.

## 2. Wie wurde es geprueft?

- **Codex-Review** (OpenAI Codex CLI, nur lesend, 3 Runden): Runde 1 → Fix 1–3, Runde 2 → Fix 4,
  Runde 3: „No remaining findings“, `VERDICT: APPROVED`.
- Unit-Tests (MSVC): `occlusion_test`, `hub_parser_test`, `hrtf_test`, `talk_state_test` gruen;
  `check_layering.sh` OK; MSBuild Release|x64 ohne Warnungen.
- Paket: `Version = 8.0.12`, DLL im Paket = `bin\conan_exiles.dll`, `sha256sum -c` OK.

Gemessen (Build 2026-10-20, Version 8.0.12):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.12-win64.ts3_plugin` (537991 Bytes) | `7b008c26a416ae7d9f0d94780ad3389b40cca5e605764273e904f1baf9159806` |
| `plugins/conan_exiles.dll` (7389696 Bytes) | `9bcc36e63cc18f4b4370c10fc047f6818562094ce35fb713e47c691fd7e67dff` |

**Noch nicht** mit Mitspielern im echten Spiel gehoert (wie 8.0.11).
