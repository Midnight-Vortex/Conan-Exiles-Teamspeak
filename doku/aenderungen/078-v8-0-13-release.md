# 078 — Version 8.0.13: Release (Ingame-Channel-Passwort vom Conan-Server)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| Plugin-Code (Commit d85d301) | Ingame-Channel-Passwort kommt von der Mod / dem Conan-Server, siehe 077 |
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.13` |
| `README.md` | Hash-Tabelle um 8.0.13 ergaenzt, Pruef-Befehle auf 8.0.13 |
| `doku/aenderungen/078-v8-0-13-release.md` | Dieser Eintrag |

## Changelog 8.0.12 → 8.0.13

| Nr. | Aenderung | Vorher |
|---|---|---|
| 1 | Das Plugin nimmt das Ingame-Channel-Passwort von der Teamspeak-Mod an (` TSPW=` im Positions-POST); ein Admin setzt es im Spiel mit `dc tsvm <Passwort>` | Passwort stand im Klartext in der Root-Channel-Beschreibung (`IngameChannelPassword`) |
| 2 | Das Passwort wird vor jedem Parsen/Loggen aus dem Body geschnitten und ueberschrieben, nur im Speicher gehalten (10 s), nie geloggt | — |
| 3 | Passwortgeschuetzter Ingame-Channel ohne Passwort: das Plugin wartet, statt mit leerem Passwort zu wechseln | TS zeigte eine Fehlermeldung „falsches Passwort“ |

`IngameChannelPassword` funktioniert weiter (das Passwort der Mod hat Vorrang). Klang, Okklusion und die
uebrigen Root-Channel-Schluessel sind unveraendert. Die Mod braucht die neue Pak vom 2026-10-20 21:17
(`DataCmd_TS_Vm`, `Vo_TsVmHolder`, ...), sonst kommt kein Passwort.

## 2. Wie wurde es geprueft?

- Unit-Tests (MSVC): `mod_password_test` (21 Pruefungen, neu), `occlusion_test`, `hub_parser_test`,
  `hrtf_test`, `talk_state_test` gruen; `check_layering.sh` OK; MSBuild Release|x64 ohne Warnungen.
- Mod im DevKit-PIE (dedizierter Server + Client), siehe 077: Halter repliziert, Passwort am Client und im
  POST-Teil, uebersteht einen Neustart, Loeschen wirkt.
- Paket: `Version = 8.0.13`, DLL im Paket = `bin\conan_exiles.dll`, `sha256sum -c` OK.

Gemessen (Build 2026-10-20, Version 8.0.13):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.13-win64.ts3_plugin` (539020 Bytes) | `fabdd28ab5009a0ac3e68a4291b029ee72f6fe6386d01f52d9ba759b9aab8368` |
| `plugins/conan_exiles.dll` (7391744 Bytes) | `6cc2430ee4875b51e8e958276d1488ff0c0de4d0f454478c63338e8c0c7cfc84` |

**Noch nicht** im echten Spiel: `dc tsvm` ueber die Konsole und der Wechsel in den Ingame-Channel mit dem
Passwort vom Server.
