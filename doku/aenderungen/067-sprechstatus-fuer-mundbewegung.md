# 067 — Sprechstatus fuer die Mundbewegung in der Mod (+ Recherche Conan-Voicechat)

**Branch:** `feature/talk-anim` · Plugin-Teil fertig, Mod-Teil offen (braucht das DevKit)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/proximity/talk_state.h/.c` | **Neu.** Speichert pro TS-Client (atomar), ob er gerade spricht, plus den eigenen Status. Baut fuer die Mod die Liste der naechsten sprechenden Spieler. |
| `src/ts/entry/ts3_entry.c` | `onTalkStatusChangeEvent` speichert den Status **aller** Clients (vorher nur der eigene fuer das HUD); geloescht beim Verlassen des Servers und beim Reset/Trennen. |
| `src/core/http/pos_http_server.c` | Jede erfolgreiche Antwort auf `POST /v1/position` bekommt hinten `"talk":0/1,"talkers":"x,y,z;..."` angehaengt. Antwortpuffer 1024 → 2048 Byte. |
| `tests/talk_state_test.c`, `tests/run_tests.sh` | Neue Suite (13 Checks). |
| `project/...vcxproj`, `build/build_mingw.sh` | `talk_state.c/.h` eingetragen. |

## 2. Wie war es vorher?

Das Plugin wusste nur fuer die **eigene** Stimme, ob gesprochen wird (gelber HUD-Rahmen).
Die Mod wusste gar nichts davon — die Figuren standen beim Sprechen mit geschlossenem Mund da.

## 3. Warum so?

- **Tastenunabhaengig:** TeamSpeak meldet den Sprechstatus selbst (`STATUS_TALKING`), egal
  welche Push-to-Talk-Taste oder Sprachaktivierung jemand nutzt. Die Mod muss keine Taste kennen.
- **Ohne Server-Aenderung:** Jeder Client kennt ueber sein eigenes Plugin, *wer* spricht und *wo*
  (CEPOS-Positionen). Die Mod bewegt die Muender lokal — kein Replizieren ueber den Game-Server,
  keine RPCs. Wer das Plugin hat, sieht die Muender der anderen.
- **Abwaertskompatibel:** Die Felder stehen am **Ende** der Antwort. Der Okklusions-Parser der Mod
  liest die Probe-Liste nur bis zum naechsten Anfuehrungszeichen und bleibt unberuehrt; aeltere
  Mods ignorieren die Antwort ganz.

## 4. Wie funktioniert es?

```
TS-Callback-Thread                          HTTP-Thread (Antwort an die Mod)
onTalkStatusChangeEvent(client, status)     POST /v1/position  ->  {"ok":true[, "probe":"..."]
  talk_state_set(client, talking, isLocal)                          ,"talk":1
  (Verlassen/Reset -> loeschen)                                     ,"talkers":"100,0,0;300,400,150"}
                                            talk = du sprichst gerade
                                            talkers = naechste 16 sprechende Spieler <= 50 m,
                                                      Unreal-Achsen in cm (wie die Okklusions-Probes)
```

## Recherche im DevKit (Conan-eigener Voicechat)

- Conan nutzt **Vivox** (Funcom-Server), Push-to-Talk auf `T`, Fenster `Strg+V`.
- **Komplett abschaltbar ueber den Server:** C++ `AServerSettings::Get/SetServerVoiceChat` mit Enum
  `EServerVoiceChat` (`Disabled` / `Enabled`), repliziert an alle Clients
  (`OnServerVoiceChatReplicated`), im Admin-Panel als Auswahl `CBS_ServerVoiceChat`.
  `SetServerVoiceChat` ist ein UFUNCTION — eine Mod koennte es serverseitig auch selbst auf
  `Disabled` setzen. Clientseitig gibt es `UConanGameInstance::BlockVoiceChat/ActivateVoiceChat`.
- **Mund:** Conans Voicechat bewegt keinen Mund (keine Lipsync/Viseme-Daten im Code). Das
  Menschen-Skelett hat ein Bone-Gesichtsrig (`b_jaw`, `b_L/R_jaw`, `b_L/R_face_lip_*`), keine
  Mund-Morphs. Die Sprech-Emotes (`AM_emote_talkingmale1/2`, `...female1/2`, `..._angry_F/M`)
  animieren genau diese Bones. Skelett-Slots: `Additive`, `Additive1st/3rd`, `Emote`, `Fullbody`,
  `Upperbody` (kein eigener Gesichts-Slot).

### Mod-Seite (umgesetzt 2026-10-18, `Vo_PosStreamer_Actor`, nicht in Git)

**Funcom hat die Lippenbewegung schon eingebaut** — es braucht keine eigene Animation:
`AB_human_male` → `ApplyAdditive` (`MainPoseWithSpeech`) ← Blend by bool **`IsSpeaking`** ←
additive `A_face_generic_talk_loop` (`/Game/Characters/humans/animations/lip_sync/`). Der Anim-Wert
kommt jeden Frame aus der C++-Eigenschaft **`ConanCharacter.IsSpeaking`** der Figur.

- Neue Funktion `UpdateTalk(Content)`: liest `"talk":1` (eigene Figur) und die `talkers`-Punkte,
  setzt fuer jede `BasePlayerChar` `IsSpeaking` = „ein Sprecherpunkt ≤ 2 m entfernt“, zuletzt
  fuer die eigene Figur = `talk`. `IsSpeaking` ist fuer Blueprints nur lesbar → gesetzt mit
  `KismetSystemLibrary::SetBoolPropertyByName(Figur, "IsSpeaking", Wert)`.
- `HTTPSuccess` (10-Hz-Bremse wie die Okklusion): nach `UpdateOcclusion` → `UpdateTalk`.
- `HTTPFaIl`: nur wenn die Mod vorher etwas gesetzt hat (`TalkActive`) einmal `UpdateTalk("")`
  → alle Muender zu. Spieler ohne Plugin werden nie angefasst; kein Mund bleibt offen haengen.
- Backup vorher: `Documents\Codex\CountEmporiumDragons-Backup-20260927\Teamspeak_BeforeTalkAnim_20261018\`.

## 5. Wie wurde es getestet?

- `talk_state_test` (MSVC): 13/13 gruen — Flags, Reset, Reihenfolge naechster zuerst,
  Unreal-Achsen in cm, Reichweite, Obergrenze 16, Anhaengen hinter die Probe-Liste, zu kleiner Puffer.
- MSVC Release-Build OK, `check_layering.sh` OK.
- **PIE (Mod):** `IsSpeaking` direkt gesetzt → Anim-Flag folgt im naechsten Frame, Kiefer
  (`b_jaw`) bewegt sich (Roll −0,5° … +4,8° statt konstant −0,51°). `UpdateTalk` mit `"talk":1`
  → eigene Figur spricht, Kiefer bewegt sich; Antwort ohne `talk` → aus. Kaputte/leere Antworten
  ohne Fehler. Im PIE scheitert jeder Mod-HTTP-Aufruf (`NotApprovedByPlayer`) → die
  `HTTPFaIl`-Absicherung schliesst die Muender sofort wieder (fuer den Test Timer gestoppt).
- **Nicht im PIE testbar:** fremde Sprecher (braucht 2 Spieler) und der echte HTTP-Rundweg.
- Ende-zu-Ende erst mit der Mod-Seite.

## 6. Lerneffekt

Der Sprechstatus kommt am zuverlaessigsten von der Stelle, die die Stimme wirklich sendet
(TeamSpeak) — nicht vom Druecken einer Taste, die jeder anders belegt.
