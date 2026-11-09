# 098 — Mundbewegung: nur die nächste Figur, nur bei hörbarer Stimme (Version 8.0.21)

**Plugin 8.0.21 + Mod** (`Vo_PosStreamer_Actor`, nicht in Git), 2026-11-09.
Die Mod-Pak ist **noch nicht gekocht und nicht hochgeladen** (der Besitzer kocht selbst).

Meldung aus dem Spiel: *„Wenn jemand spricht, kann es in seltenen Fällen passieren, dass auch der
Mund von anderen Spielern animiert wird, als würde er sprechen, auch ohne dass er spricht.“*

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/proximity/talk_state.h/.c` | Neu: `talk_state_note_voice()` merkt sich pro Client, wann zuletzt ein Stueck seiner Stimme abgespielt wurde (ein atomarer Wert, Audio-Thread). Ein **fremder** Client zaehlt nur noch als sprechend, wenn TeamSpeak „spricht“ meldet **und** seine Stimme in den letzten 250 ms (`TALK_VOICE_HOLD_MS`) abgespielt wurde. `talk_state_append_json()` bekommt dafuer die Uhrzeit (`nowMs`). |
| `src/ts/entry/ts3_entry.c` | `onEditPlaybackVoiceDataEvent` ruft `talk_state_note_voice()` auf. `onTalkStatusChangeEvent` zaehlt ein **empfangenes TS-Fluestern** (`isReceivedWhisper`) nicht mehr als Sprechen (vorher ignoriert). Version 8.0.21. |
| `src/core/http/pos_http_server.c` | reicht `GetTickCount64()` an `talk_state_append_json()` weiter. |
| `tests/talk_state_test.c` | Alte Pruefungen auf die neue Signatur umgestellt, neue Gruppe [3] „Stimme muss spielen“ (9 Pruefungen). |
| Mod `Vo_PosStreamer_Actor`, Funktion `UpdateTalk` | **Nur die naechste Figur** (innerhalb 200 cm) pro Sprecherpunkt bekommt `IsSpeaking`, nicht mehr jede Figur im Umkreis. Dazu ein Log im Spiel (siehe 4). Neue Variable `TalkLogNext` (Zeitpunkt der naechsten Logzeile), neue lokale Variablen `BestDist`, `BestPawn`. |

---

## 2. Wie war es vorher?

- **Mod:** Fuer jede Spielerfigur galt „`IsSpeaking`, wenn **irgendein** Sprecherpunkt ≤ 200 cm entfernt
  ist“. Stand ein Spieler hoechstens 2 m neben einem Sprecher, bewegte **auch sein Mund** — die Mod
  konnte nicht unterscheiden, welche der beiden Figuren spricht. Das trifft vor allem Gruppen
  dicht beieinander (Lagerfeuer, Taverne).
- **Plugin:** „spricht“ kam allein aus dem TeamSpeak-Ereignis. Ging das „hat aufgehoert“-Ereignis
  verloren (z. B. waehrend ein anderer Server-Tab aktiv war), blieb der Mund offen, bis ein neues
  Ereignis kam. Ein TS-Fluestern (z. B. Support) zaehlte wie Sprechen in der Naehe.

---

## 3. Warum ist die neue Loesung besser?

- **Nur eine Figur pro Sprecher.** Pro Sprecherpunkt gewinnt die naechste Figur (≤ 200 cm). Der
  Nachbar bleibt still. Der Radius blieb bei 200 cm, damit ein leicht hinterherhinkender Punkt
  (Position kommt per Plugin-Nachricht, Mod fragt 10-mal pro Sekunde ab) die Figur weiter findet.
- **Mund nur bei hoerbarer Stimme.** „Spricht“ heisst jetzt: TeamSpeak meldet es **und** das Plugin
  hat gerade Audio dieses Spielers zum Abspielen bekommen. Ein verlorenes „hat aufgehoert“, ein
  stumm geschalteter Spieler oder ein Fluestern kann keinen Mund mehr offen halten. Es gibt nie
  mehr Sprecher als vorher, hoechstens weniger.
- **Kein Lock auf dem Audio-Thread.** Eine einzige atomare Schreiboperation pro Audio-Block.

---

## 4. Wie funktioniert es?

```
Audio-Thread:   onEditPlaybackVoiceDataEvent(client) -> talk_state_note_voice(client, jetzt)
Callback-Thread: onTalkStatusChangeEvent(client, status, isWhisper)
                    -> talking = !isWhisper && (status == TALKING ...)
HTTP-Thread:    Antwort an die Mod: "talkers" = Spieler, fuer die gilt
                    TS sagt "spricht"  UND  Stimme vor <= 250 ms abgespielt  UND  <= 50 m entfernt
Mod UpdateTalk: 1. alle Figuren IsSpeaking = false
                2. pro Sprecherpunkt: naechste Figur (<= 200 cm) -> IsSpeaking = true
                3. eigene Figur: IsSpeaking = ("talk":1)   (wie bisher)
```

**Log in der Mod** (nur wenn gerade jemand eine Figur zugewiesen bekommt, hoechstens alle 2 s):
`TSMOUTH name=<Charaktername> dist_cm=<Abstand zum Sprecherpunkt>`
in `ConanSandbox\Saved\Logs\ConanSandbox.log` (Zeile `LogBlueprintUserMessages`). Der Name der
Figur zeigt, wer den Mund bekommen hat und ob der Charaktername bei fremden Figuren gefuellt ist.

---

## 5. Wie wurde es getestet?

- **Host-Tests:** `talk_state_test` 22/22 gruen (davon 9 neu). Gesamtlauf `tests/run_tests.sh`
  mit MSYS2-gcc: alle Suiten gruen **ausser** `player_table_test`, `render_state_test`,
  `proto_flush_test` — diese drei kompilieren auf dem Windows-gcc nicht (Linux-only
  `pthread_timedjoin_np`, SDK-Header-Typen, `strncpy_s`); die Fehler betreffen keine geaenderte Datei.
- **MSVC-Release-Build** (`build.ps1 -SkipDeploy`) fehlerfrei. Es wurde **nichts deployt**.
- **Mod, DevKit-Editor (eigene Instanz, `execute_python`):** `UpdateTalk` baut ohne Fehler und
  Warnungen (54 Knoten), gespeichert wurde nur `Vo_PosStreamer_Actor.uasset`. Test mit drei
  gespawnten Spielerfiguren (A, B 100 cm neben A, C allein):

  | Fall | Ergebnis |
  |---|---|
  | Punkt auf A (B 100 cm daneben) | nur A (vorher A **und** B) |
  | Punkt 70 cm von A / 30 cm von B | nur B |
  | Punkt 250 cm von der naechsten Figur | keine |
  | Punkt 199 cm von A | A |
  | Punkte auf A und auf C | A und C, B bleibt still |
  | keine Sprecher / leerer Inhalt | alle zu |

  Das Log `TSMOUTH …` erschien einmal (Drossel wirkt).

**Nicht geprueft:** kein Test mit echten Spielern, kein PIE, keine gekochte Pak, `deploy` des
Plugins nicht gemacht.

---

## 6. Was der Besitzer tun muss

1. Plugin 8.0.21 bauen/verteilen (`build.ps1`, Setup) — die Plugin-Seite wirkt allein schon gegen
   haengende Muender und Fluestern.
2. Mod kochen (`cook_teamspeak.cmd`) und die Pak hochladen — erst damit gilt „nur die naechste Figur“.
3. Test mit zwei Spielern nebeneinander (1 m): A spricht → nur A bewegt den Mund. Danach die
   `TSMOUTH`-Zeile im Conan-Log ansehen.

**Sicherung vor der Mod-Aenderung:**
`C:\Users\uwebu\Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261109`.

---

## 7. Erkenntnisse (fuer eine spaetere Zuordnung ueber Namen/IDs)

- Die **SteamID** fremder Spieler kann ein Blueprint **nicht** als Text lesen:
  `PlayerState.BP_GetUniqueId` liefert nur eine `UniqueNetIdRepl`-Struktur ohne Umwandlung in Text,
  `ConanPlayerController.GetPlayerNetID` (Text) gibt es nur am eigenen Controller.
- Auch die **Charakter-Nummer** (`StableId`, `UniqueID`) ist im Blueprint undurchsichtig (kein
  Aufbrech-Knoten, keine Umwandlung in Text) — nur vergleichen ist moeglich.
- Lesbar ist der **Charaktername** (`ConanCharacter.GetCharacterName`). Eine exakte Zuordnung
  Sprecher ↔ Figur muesste also ueber den Charakternamen laufen: Mod meldet den eigenen Namen
  ans Plugin, das Plugin verteilt ihn (wie `CEAUTH`), `talkers` traegt den Namen mit. Das
  `TSMOUTH`-Log zeigt vorab, ob der Name bei fremden Figuren im Spiel gefuellt ist.

---

## 8. Lerneffekt

Ein Ereignis („hat aufgehoert zu sprechen“) kann verloren gehen; die Frage „kommt gerade Audio an?“
nicht. Wer nur eine Eigenschaft einer Sache anzeigen will, sollte sie von der Quelle ableiten, die
nicht ausfallen kann — und eine Zuordnung nach Abstand braucht einen **Gewinner**, nicht einen Radius.
