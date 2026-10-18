# 068 — Version 8.0.9: Release (Sprechstatus fuer die Mundbewegung)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.9` (vorher `8.0.8`) |
| `README.md` | Hash-Tabelle um 8.0.9 ergaenzt (8.0.8/8.0.7/8.0.6 bleiben), Pruef-Befehle auf 8.0.9 |
| `doku/aenderungen/068-v8-0-9-release.md` | Dieser Eintrag (inkl. Changelog) |

## Changelog 8.0.8 → 8.0.9

| Nr. | Was | Standard |
|---|---|---|
| 067 | **Sprechstatus fuer die Mundbewegung:** Das Plugin merkt sich den TeamSpeak-Sprechstatus **aller** Clients (unabhaengig von Push-to-Talk-Taste oder Sprachaktivierung) und haengt an jede Antwort auf `POST /v1/position` `"talk":0/1` (du sprichst) und `"talkers":"x,y,z;..."` (bis zu 16 sprechende Spieler im Umkreis von 50 m, Unreal-Achsen in cm) an | immer an |
| 067 | Antwortpuffer des HTTP-Servers 1024 → 2048 Byte (Platz fuer Probe-Liste + Sprecherliste) | — |
| 067 | Mod-Seite: `Vo_PosStreamer_Actor` setzt damit `ConanCharacter.IsSpeaking` → Funcoms eingebaute Lippen-Animation (`A_face_generic_talk_loop`) | — |

**Wichtig:** Die Muender bewegen sich erst mit der **aktualisierten Conan-Mod**. Mit der bisherigen
Mod aendert sich nichts: Die neuen Felder stehen am Ende der Antwort, aeltere Mods ignorieren sie.
Kein neuer Schalter im Root-Channel.

Tipp fuer Server-Admins: Conans eigenen Voicechat (Vivox) im Admin-Panel bzw. mit
`serverVoiceChat=0` in `ServerSettings.ini` abschalten, damit nur TeamSpeak spricht.

## 2. Wie war es vorher?

8.0.8 kannte nur den **eigenen** Sprechstatus (gelber HUD-Rahmen). Die Figuren im Spiel standen
beim Reden mit geschlossenem Mund da.

## 3. Warum ist die neue Loesung besser?

Tastenunabhaengig (TeamSpeak meldet `STATUS_TALKING` selbst), ohne Aenderung am Game-Server
(jeder Client bewegt die Muender lokal aus seinen eigenen Plugin-Daten) und abwaertskompatibel.

## 4. Wie funktioniert der Release-Ablauf? (wie 066)

```
Version 8.0.9 (ts3_entry.c) → .\build.ps1 -SkipDeploy (DLL + .ts3_plugin)
  → SHA256SUMS erzeugen, sha256sum -c pruefen → README-Hashes → Commit/Push
  → gh release create v8.0.9 --draft <paket> SHA256SUMS → Digests pruefen → veroeffentlichen
```

Gemessen (Build 2026-10-18, Version 8.0.9):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.9-win64.ts3_plugin` (533220 Bytes) | `08628397ee61b71d1ee7ed9763f00fc460b3d2f49095c9a00f623ac8bbfa6859` |
| `plugins/conan_exiles.dll` (7380992 Bytes) | `cc246496ed7649c47583478fb91f9f5fe00d8d8efbb41aff20b5bb75d6f18e23` |

## 5. Wie wurde es getestet?

- MSBuild Release|x64 exit 0, Paket enthaelt `package.ini` (`Version = 8.0.9`) und
  `plugins/conan_exiles.dll` (Forward-Slash), DLL im Paket = `bin\conan_exiles.dll`, enthaelt `talkers`.
- `sha256sum -c SHA256SUMS`: beide Zeilen `OK`.
- Unit-Tests (MSVC): `hrtf_test`, `hub_parser_test`, `occlusion_test`, `talk_state_test` gruen;
  `check_layering.sh` OK.
- Mod-Seite in PIE fuer die eigene Figur geprueft (siehe 067).
- **Noch nicht** im TeamSpeak-Client mit Mitspielern getestet (fremde Muender brauchen 2 Spieler
  und die gekochte, aktualisierte Mod).

## 6. Lerneffekt

Neue Felder hinten an eine bestehende Antwort haengen statt das Format zu aendern: alte Mods
laufen unveraendert weiter, und Plugin und Mod koennen getrennt ausgeliefert werden.
