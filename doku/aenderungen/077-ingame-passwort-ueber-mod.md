# 077 — Ingame-Channel-Passwort kommt vom Conan-Server statt aus dem Root-Channel

2026-10-20. Plugin + Mod (`teamspeak`, nicht in Git).

## 1. Ausgangslage

`IngameChannelPassword=<pw>` stand bisher im Klartext in der Beschreibung des Root-Channels. Jeder auf dem
TS-Server konnte es lesen und sich in den Ingame-Channel setzen, auch ohne im Spiel zu sein.

## 2. Neuer Weg

1. Ein Admin setzt das Passwort **im Spiel** über die Konsole:
   `dc tsvm <Passwort>` (auch `DataCommand tsvm <Passwort>`). `dc tsvm` ohne Passwort löscht es.
2. Der Conan-Server speichert es (Savegame-Slot `TeamspeakVm`, übersteht Neustarts) und repliziert es nur an
   eingeloggte Spieler.
3. Die Mod hängt es an jeden Positions-POST ans Plugin an (` TSPW=<Passwort>`, letztes Feld; nie in `Pos.txt`).
4. Das Plugin nutzt es für den Wechsel in den Ingame-Channel.

Danach `IngameChannelPassword` aus der Root-Beschreibung löschen und das Channel-Passwort im TS **einmal
ändern** (das alte war öffentlich) — auf denselben Wert wie `dc tsvm`.

## 3. Plugin

| Datei | Änderung |
|---|---|
| `src/core/hub/mod_password.c/.h` (neu) | Schneidet ` TSPW=…` vom Body ab und überschreibt es im Puffer, **bevor** geparst oder geloggt wird (die 400-Pfade loggen den Rohtext). Hält das Passwort nur im Speicher (Lock), gültig 10 s nach dem letzten POST. Leerer Wert = „Server hat keins“. Zu lang (≥ 128) oder Steuerzeichen = verworfen. Log nur bei Änderung, nur die Länge |
| `src/core/http/pos_http_server.c` | Ruft `mod_password_take_from_body` direkt nach dem Empfang |
| `src/ts/profile/ts3_server_profile.c/.h` | `server_profile_copy_ingame_password`: Mod-Passwort vor `IngameChannelPassword`; Reset beim Trennen löscht es |
| `src/ts/channel/channel_manage.c` | Kopie in einen lokalen Puffer, nach dem Move gelöscht. **Neu:** Ist der Ingame-Channel passwortgeschützt und kein Passwort da, wartet das Plugin (Log-Zeile höchstens 1×/min) statt einen Move mit leerem Passwort zu schicken (keine TS-Fehlermeldung) |
| `src/ts/adapter/ts3_adapter.c/.h` | `ts3_channel_has_password` (`CHANNEL_FLAG_PASSWORD`) |
| `tests/mod_password_test.c` (neu) | 21 Prüfungen: Abschneiden, Leerzeichen/`X=` im Passwort, Marker in fremdem Wert, CR/LF, leer = löschen, TTL, Länge, Steuerzeichen, UTF-8, **Log enthält nie das Passwort** |

Rückwärts verträglich: Alte Mod (kein `TSPW=`) + Passwort in der Beschreibung = wie bisher.

## 4. Mod (`/Game/Mods/teamspeak/`)

| Asset | Inhalt |
|---|---|
| `DT_TS_ConsoleCommands` (neu) | Zeile `tsvm`: `DataCmd_TS_Vm`, RunOnServer, RequireAdmin |
| `DataCmd_TS_Vm` (neu, `DataActorCommand`) | `DoCommand`: IsValid(PC) → `IsAdmin` → `GetActorOfClass(Vo_TsVmHolder)` → `SetPw(JoinStringArray(Parameter, " "))` |
| `Vo_TsVmHolder` (neu, Actor) | Repliziert, immer relevant, 1 Hz. `VmPw` (repliziert). BeginPlay (nur Server): lädt Slot `TeamspeakVm`. `SetPw`: setzt `VmPw` + speichert |
| `SG_TS_Vm` (neu, SaveGame) | `Pw` |
| `BP_ModController` | `ModDataTableOperations` → `MergeDataTables(CustomConsoleCommandsDataTable, DT_TS_ConsoleCommands)`; BeginPlay: Server spawnt einen Halter, falls keiner da |
| `Vo_PosStreamer_Actor` | `UpdatePosition`: Halter suchen/merken (`TsVmHolder`), `TsPwPart` = ` TSPW=<VmPw>` oder leer, an den HTTP-Body gehängt |
| `Local/CookInfo.ini` | 4 neue Zeilen |

## 5. Test (PIE, dedizierter Server + Client)

| Prüfung | Ergebnis |
|---|---|
| Server spawnt genau einen Halter, Client hat die Kopie | 1 / 1 |
| Zeile `tsvm` zur Laufzeit in der Spieltabelle | ja (26 Zeilen) |
| `SetPw("Geheim Test1")` am Server | Server + Client `VmPw` gleich, Streamer `TsPwPart = " TSPW=Geheim Test1"`, Savegame geschrieben |
| PIE neu gestartet | Server lädt `Geheim Test1` aus dem Savegame, Client bekommt es |
| `SetPw("")` | überall leer, `TsPwPart` leer |
| Blueprint-Laufzeitfehler im Log | keine |
| Plugin-Unit-Tests (MSVC) | alle grün, Layering OK, DLL ohne Warnung |

**Nicht getestet:** der echte Konsolenweg `dc tsvm …` (Python kann den Befehls-Actor in UE5 nicht in die
PIE-Welt spawnen; der Befehl folgt demselben Muster wie Funcoms `dc`-Befehle und `dc BarrierDelete` aus
MapEdits), und das Plugin mit echter Mod im Spiel.

## 6. Grenzen

- Wer rechtmäßig im Spiel ist, kann das Passwort auf seinem eigenen PC mitlesen (localhost, Speicher) und
  weitergeben. Das verhindert nur ein ständig wechselndes Passwort mit einem TS-Bot.
- Das Passwort liegt auf dem Conan-Server im Savegame (`Saved/SaveGames/TeamspeakVm.sav`).
- Conan protokolliert Admin-Konsolenbefehle evtl. im Server-Log (dann steht `dc tsvm <Passwort>` dort).
