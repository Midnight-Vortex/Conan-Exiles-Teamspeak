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

### Plan fuer die Mod-Seite (offen, braucht das DevKit)

1. Aus `A_emote_talking_var_01_M/F` eine **additive Animation nur fuer Kiefer/Lippen** bauen
   (Koerper-Spuren entfernen), als Montage im Slot `Additive`.
2. Pruefen, ob das Character-AnimBP den `Additive`-Slot wirklich additiv mischt (sonst Alternative).
3. `Vo_PosStreamer_Actor`: aus der HTTP-Antwort `talk` (eigene Figur) und `talkers` lesen,
   Pawns in ~1,5 m um jeden Punkt finden, Montage lokal abspielen/stoppen.

## 5. Wie wurde es getestet?

- `talk_state_test` (MSVC): 13/13 gruen — Flags, Reset, Reihenfolge naechster zuerst,
  Unreal-Achsen in cm, Reichweite, Obergrenze 16, Anhaengen hinter die Probe-Liste, zu kleiner Puffer.
- MSVC Release-Build OK, `check_layering.sh` OK.
- Ende-zu-Ende erst mit der Mod-Seite.

## 6. Lerneffekt

Der Sprechstatus kommt am zuverlaessigsten von der Stelle, die die Stimme wirklich sendet
(TeamSpeak) — nicht vom Druecken einer Taste, die jeder anders belegt.
