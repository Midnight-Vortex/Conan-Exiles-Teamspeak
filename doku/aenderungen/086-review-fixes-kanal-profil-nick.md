# 086 — Review-Fixes: Kanalwechsel, Server-Profil, Nickname, Hotkeys

2026-10-28. Nur Plugin (Paket "CHANNEL" des Review-Fix-Laufs; andere Pakete: Adapter, Protokoll, Audio,
Config-IO, UI). Betrifft `src/ts/channel/*`, `src/ts/profile/*`, `src/ts/nick/*`, `src/core/voice/*`.

Ein Code-Review hat 15 Fehler in diesem Bereich gefunden. Dieses Paket behebt genau diese 15 — nichts
darüber hinaus. Die Fixes 1-3, 9 und 11 brauchen kleine Gegenstücke im Adapter (`ts3_entry.c`); die liefert
das Adapter-Paket (siehe "Zusammenspiel mit anderen Paketen" ganz unten).

Kurze Begriffe vorweg:

- **Callback-Thread**: der eine Thread, in dem TeamSpeak unser Plugin aufruft. Nur er darf die TS-API
  benutzen.
- **CEDRAIN**: unsere "Weck-Schleife". Das Plugin schickt sich selbst alle ~30 ms eine Nachricht, damit der
  Callback-Thread weiterarbeitet. Sie läuft nur, solange irgendwo "Arbeit wartet"
  (`chan_has_pending_work()` und Kollegen).
- **Atomic / Interlocked**: eine Zahl, die man in einem Schritt lesen oder schreiben kann, ohne dass ein
  anderer Thread dazwischenfunkt. Braucht kein Lock.
- **Dirty-Flag**: ein "Hier hat sich etwas geändert"-Zettel. Viele Änderungen setzen denselben Zettel; wer
  aufräumt, tut es einmal.

---

## 1. Abgelehnte Kanalwechsel werden erkannt (Backoff)

**Was:** `channel_manage.c`: neue Funktion `chan_on_move_failed(error)` (der Adapter ruft sie aus
`ts3plugin_onServerErrorEvent`). Neuer Header `chan_move_backoff.h` (reine Rechenlogik).

**Vorher:** `requestClientMove` liefert sofort "OK" — ob der Server den Wechsel später ablehnt (falsches
Passwort, Kanal voll, keine Berechtigung), kommt erst als eigene Fehlermeldung hinterher. Das Plugin hat das
nie gelesen: Nach 5 s ohne Antwort galt der Wechsel als "hängt", wurde gelöscht und sofort **neu gesendet** —
endlos, jedes Mal mit sichtbarem TS-Fehler. Außerdem blieb der Spieler mit der Zufallszahl als Namen
("48291057") im Hub sitzen, weil der Name schon vor dem Wechsel anonymisiert wurde.

**Warum besser:** Der Fehler wird sofort bemerkt, der echte Name kommt zurück, und das Plugin wartet
zunehmend länger, bevor es denselben Kanal noch einmal versucht.

**Wie funktioniert es:**

```
chan_request_move(ingame) ──► TS: requestClientMove (Antwort "OK")
        │                              │ später, asynchron
        │                              ▼
        │            Server lehnt ab ──► onServerErrorEvent ──► chan_on_move_failed(err)
        ▼                                                         │
  g_moveInFlight = 1                                              ▼
                                       in-flight löschen · nick_restore_in_hub() ·
                                       Backoff für DIESES Ziel · EINE Log-Zeile
```

- Wartezeit nach dem n-ten Fehlschlag zum selben Ziel: **5 s, 10 s, 30 s, 60 s**, danach bleibt es bei 60 s.
  Ein anderes Ziel (z. B. Hub statt Ingame) fängt wieder bei 5 s an und wird nicht vom Backoff des anderen
  blockiert.
- Zurückgesetzt wird der Backoff bei **jedem** eigenen Kanalwechsel (`chan_on_own_move`) und bei
  `chan_reset` (neue Verbindung).
- Der alte "5 s ohne Antwort"-Zweig behandelt den Fall jetzt genauso (Name zurück + Backoff, eine Log-Zeile).
  Er läuft außerdem jeden Tick, auch wenn gar kein Wechsel mehr gewünscht ist — so kann ein vergessenes
  "in-flight" die CEDRAIN-Schleife nie dauerhaft am Leben halten.
- Pro Fehlversuch genau **eine** Log-Zeile ("rejected by the server (error 0x030d) - next attempt in 5 s").
  Die 5-Sekunden-Zeile "auto-move blocked" schweigt, solange ein Backoff läuft.

**Test:** `tests/chan_move_backoff_test.c` (Zeitplan, Grenzen, Zielwechsel, Reset, Überlauf) und
`tests/channel_manage_test.c` Abschnitte [1]-[3b] (echter `channel_manage.c`-Code gegen ein Fake-TS).

**Lerneffekt:** "Befehl abgeschickt" heißt bei asynchronen APIs nicht "Befehl geklappt". Wer einen Befehl
wiederholt, braucht eine Pause, die mit jedem Fehlversuch wächst — sonst ist die Wiederholung selbst der
Fehler.

---

## 2. Auch ein sofort gescheiterter Wechsel startet die Pause

**Was:** `channel_manage.c`, `chan_request_move_impl`: `g_lastMoveMs = now` steht jetzt **vor** dem Senden.

**Vorher:** Der Zeitstempel für die 2-Sekunden-Pause wurde nur bei Erfolg gesetzt. Scheiterte der Aufruf
sofort (z. B. ungültige Kanal-ID), probierte das Plugin es beim nächsten Tick (alle 500 ms) wieder — und der
Adapter schrieb jedes Mal eine Fehlerzeile.

**Warum besser:** Die Pause gilt für jeden Versuch, egal wie er ausgeht.

**Test:** `channel_manage_test.c` [4]: nach einem sofortigen Fehlschlag kein zweiter Versuch vor Ablauf von
2 s.

**Lerneffekt:** Eine Ratenbegrenzung muss den *Versuch* zählen, nicht den *Erfolg*.

---

## 3. Gemerkte Kanal-IDs (Hub / Ingame / Root) werden aufgefrischt

**Was:** Neue Funktionen `chan_on_channel_list_changed(channelID)` (`channel_manage.c`) und
`server_profile_on_channel_list_changed(channelID)` (`ts3_server_profile.c`). Der Adapter ruft beide, wenn
ein Kanal angelegt, gelöscht, verschoben oder bearbeitet wird.

**Vorher:** Hub-, Ingame- und Root-ID wurden einmal gefunden und nie wieder geprüft. Löscht ein Admin
"Ingame" und legt ihn neu an, hat der neue Kanal eine neue ID: `chan_is_ingame_channel(neueId)` war falsch,
alle im echten Ingame-Kanal bekamen "Passthrough" statt Proximity-Ton, und Umzüge gingen an eine tote ID.
Beim Root-Kanal galt dasselbe (Profil-Updates des neuen Root wurden ignoriert).

**Warum besser:** Nach jeder Kanaländerung wird **einmal** neu gesucht, ohne dass bis dahin etwas
wegbricht.

**Wie funktioniert es:**

```
TS: onNewChannelEvent × 300 beim Connect, später Create/Delete/Edit
        │  (jedes Mal nur: Dirty-Flag setzen + Wecken)
        ▼
nächster chan_tick / server_profile_tick  ──►  EIN Rescan der Kanalliste
```

- Das Dirty-Flag wird erst **nach** der Tick-Drosselung (500 ms) verbraucht, ein Tick, der nichts tut, "frisst"
  die Änderung also nicht. Es hält die CEDRAIN-Schleife am Leben, bis es verarbeitet ist.
- Die alten IDs bleiben gültig, bis der Rescan neue liefert. Kann die Kanalliste nicht gelesen werden (TS-API
  meldet Fehler), bleiben sie unverändert — Proximity fällt nie nur deshalb auf Passthrough zurück, weil ein
  Aufruf mal scheitert.
- Wird für den Root-Kanal eine **neue** ID gefunden, wird seine Beschreibung sofort neu angefordert, auch
  wenn schon ein Profil aktiv ist (sonst bliebe das alte Profil für immer stehen).
- Die Event-Handler (`onUpdateChannel…`) verbrauchen das Flag **nicht** — sonst gäbe es beim Connect wieder
  einen Rescan pro Event (siehe Fix 5).

**Test:** `channel_manage_test.c` [6] (50 Events → genau 1 Rescan, neue Ingame-ID wird erkannt, Fehlschlag
behält Cache), `server_profile_test.c` [5]+[6] (Root gelöscht und neu angelegt).

**Lerneffekt:** Wer IDs cached, braucht einen Weg, sie wieder loszuwerden — am besten "merken, dass etwas
nicht mehr stimmt" und später einmal aufräumen, statt bei jeder Meldung sofort.

---

## 4. Kanal-Grenze 128 → 1024

**Was:** `CHAN_MAX_CHANNELS` (`channel_manage.c`) und `PROFILE_MAX_CHANNELS` (`ts3_server_profile.c`) von 128
auf 1024. Der Suchpuffer ist ein `static` Array (8 KB), kein Stack-Array.

**Vorher:** Auf Servern mit mehr als 128 Kanälen wurde die Liste abgeschnitten; stand "Hub", "Ingame" oder
"Root" dahinter, fand das Plugin sie nie.

**Warum besser:** 8× mehr Platz. Statische Puffer sind erlaubt, weil beide Suchen nur im Callback-Thread
laufen und sich nicht selbst aufrufen. `ts3_get_channel_list(out, max)` kopiert höchstens `max` Einträge (im
Adapter nachgelesen). Ist die Liste doch voll (≥ 1024) und der gesuchte Kanal nicht dabei, bleibt die
gemerkte ID erhalten statt gelöscht zu werden.

**Test:** `channel_manage_test.c` [7] und `server_profile_test.c` [7]: Hub/Ingame/Root an Position 900/1000
werden gefunden; liegt einer hinter der Grenze, bleibt die alte ID.

**Lerneffekt:** Feste Grenzen immer zusammen mit "was passiert, wenn sie erreicht werden" denken.

---

## 5. Keine ungedrosselten Kanal-Scans mehr

**Was:** `chan_tick` / `server_profile_tick` / die Profil-Event-Handler.

**Vorher:** Gab es keinen "Root"-Kanal (z. B. jeder Nicht-Conan-Server), suchte das Plugin bei **jedem**
`onUpdateChannel`-/`onChannelDescriptionUpdate`-/`onUpdateChannelEdited`-Event die ganze Liste ab (plus
ein Namensabruf pro Kanal) — beim Connect grob Kanäle² Aufrufe. In `chan_tick` riefen drei Zweige
`chan_find_hub_and_ingame()` ohne jede Drosselung auf.

**Warum besser:** Es gibt nur noch **einen** Ort, der sucht, und der benutzt die vorhandenen Wiederhol-
Zeiten (5 s). Ausnahme ist das Dirty-Flag aus Fix 3: ein erzwungener Rescan pro Tick.

**Wie funktioniert es:** `chan_tick_run` prüft am Anfang "Dirty-Flag oder (ID fehlt und 5 s seit dem letzten
Versuch)" — die drei Zweige verlassen sich auf dieses Ergebnis. Im Profil macht `profile_resolve_root()`
dasselbe; die Event-Handler dürfen nur die 5-s-Drosselung nutzen, das Dirty-Flag nicht verbrauchen.

**Test:** `channel_manage_test.c` [8] (40 Ticks ohne Hub/Ingame → ca. 5 Scans) und `server_profile_test.c`
[5] (100 Events in 10 s → höchstens 3 Scans).

**Lerneffekt:** "Bei jedem Event suchen" fühlt sich harmlos an — bis ein Server 300 Events am Stück schickt.

---

## 6. Root-Kanal: der oberste "Root" gewinnt

**Was:** `profile_find_root_channel` (`ts3_server_profile.c`) nutzt `ts3_get_channel_parent`.

**Vorher:** Der erste Kanal mit dem Namen "Root" war "der" Root. Wer Kanäle anlegen darf, konnte einen
Unterkanal "Root" mit gefälschter `[GLOBAL]`-Beschreibung anlegen, der in der Liste vor dem echten steht —
und damit allen Spielern Einstellungen unterschieben.

**Warum besser:** Ein "Root" auf oberster Ebene (Eltern-ID 0) wird bevorzugt. Gibt es keinen, gilt wie
bisher der erste Treffer — bestehende Server ohne Top-Level-Root laufen also weiter. (Das ist die
vorsichtige Variante; ein zusätzlicher Top-Level-Kanal "Root" würde damit nicht abgefangen.)

**Test:** `server_profile_test.c` [4]: gefälschter Unterkanal vor dem echten → der echte wird genommen;
nur Unterkanal → Fallback; Eltern-Abfrage nicht verfügbar → Fallback.

**Lerneffekt:** Namen sind keine Identität. Zusätzliche Eigenschaften (hier: "ganz oben") machen Fälschungen
schwerer.

---

## 7. Profil-Einzelwerte ohne 8-KB-Kopie

**Was:** `ts3_server_profile.c`: `server_profile_get_max_volume`, `_force_auto_channel`,
`_force_speakers_on`, `_get_nickname_randomizer`, `_get_realistic_audio`, `_get_filter_intensity` lesen
Atomics. Neu: `server_profile_force_distance_muting()` (statt `server_profile_get(&hub)` in `chan_tick`).
Signaturen der bisherigen Getter unverändert.

**Vorher:** Jeder dieser Aufrufe kopierte die ganze `HubSettings` (~8 KB, mit Zonen und Rassen) unter dem
Profil-Lock — pro sprechendem Spieler und Neuberechnung, in jedem `chan_tick`, in jedem Positions-Tick.

**Warum besser:** Ein Wert = eine Atomic-Zahl; kein Lock, keine Kopie, auch aus dem Audio-Thread unkritisch.

**Wie funktioniert es:** Beim Anwenden eines Profils (`server_profile_apply`) werden die Werte
**vor** dem Aktiv-Flag veröffentlicht, beim Reset zurück auf die "kein Profil"-Standardwerte
(Max-Lautstärke 1.0, Filter 100, Force-Flags 0, Nickname-Randomizer **1**, Realistic 0). Die Atomics halten
also immer den *wirksamen* Wert; die Getter müssen den Aktiv-Status nicht mehr prüfen. Kommazahlen
(`float`) werden als Bitmuster in einer `long` abgelegt; die Umwandlung geht jetzt über `int32_t`, damit
sie auch auf 64-Bit-Linux-Tests stimmt (das gleiche Muster nutzt nun auch der Listen-Bonus; unter Windows
ändert sich nichts).

**Test:** `server_profile_test.c` [1] (Standardwerte, Profilwerte, Reset) und [8] (Leser-Thread gegen
3000× apply/reset — nie ein "halber" Wert).

**Lerneffekt:** Wer nur ein Feld braucht, sollte nicht das ganze Objekt kopieren.

---

## 8. Config-Änderungen ohne verlorene Updates

**Was:** `profile_apply_defaults_once` und `profile_apply_hub_forces` (`ts3_server_profile.c`) benutzen
`config_modify` + `config_save` (Vorgabe aus dem Config-Paket).

**Vorher:** "Config kopieren → ändern → komplett zurückschreiben → speichern". Ändert ein anderer Thread
(z. B. Einstellungsdialog) zwischen Kopie und Zurückschreiben etwas, ist seine Änderung weg.

**Warum besser:** `config_modify(fn, ctx)` führt `fn` direkt auf der echten Config **unter dem Config-Lock**
aus; es ändert nur die Felder, die das Profil besitzt. Die Prüfung "Server-Defaults schon angewendet?" und das
Schreiben sind dadurch ein Schritt. Gespeichert wird danach mit `config_save()` (kann aus jedem Thread).

**Test:** `server_profile_test.c` [3]: Force-Flags, Defaults einmal pro Server (UID), Benutzer-Änderungen
bleiben erhalten, ohne Server-UID wird nichts angewendet.

**Lerneffekt:** "Lesen-Ändern-Schreiben" über ein ganzes Objekt ist ein Lost-Update-Rezept. Besser: die
Änderung dorthin schicken, wo die Daten sind.

---

## 9. Altes Root-Passwort verschwindet beim Reset aus dem Speicher

**Was:** `server_profile_reset` löscht (`SecureZeroMemory`) `g_ingamePassword`, den Beschreibungs-Puffer,
den geparsten `HubSettings`-Puffer und die aktive Profilkopie `g_active`. Dafür wurden die beiden bisherigen
Funktions-`static`-Puffer zu Datei-`static`-Puffern.

**Vorher:** Das (veraltete) `IngameChannelPassword` aus der Root-Beschreibung blieb nach dem Trennen im
Speicher liegen — in mindestens vier Kopien.

**Warum besser:** Nach dem Trennen/Serverwechsel ist es weg. (Das Mod-Passwort ist ein anderes Modul und
wird dort geleert.)

**Test:** `server_profile_test.c` [2] prüft, dass nach `server_profile_reset` alle vier Puffer nur noch
Nullen enthalten.

**Lerneffekt:** Löschen heißt alle Kopien löschen — und das Löschen darf der Compiler nicht wegoptimieren
(deshalb `SecureZeroMemory`).

---

## 10. Das Mod-Passwort geht nur an einen Kanal, der wirklich eins hat

**Was:** `chan_request_move_impl`: `ts3_channel_has_password(ziel)` wird zuerst gefragt.

**Vorher:** Bei jedem Wechsel in "Ingame" wurde das Passwort mitgeschickt — auch wenn der Kanal gar kein
Passwort hat (geprüft wurde es nur im Fall "kein Passwort bekannt").

**Warum besser:** Ein Geheimnis wird nur dahin geschickt, wo es gebraucht wird. 1 (geschützt) = Passwort
senden; 0 (ungeschützt) = leeres Passwort; -1 (unbekannt) = wie bisher.

**Test:** `channel_manage_test.c` [5]: ungeschützt → leer; geschützt → Passwort; unbekannt → wie früher;
geschützt ohne Passwort → warten, ohne die Weck-Schleife zu belasten.

**Lerneffekt:** Prinzip der geringsten Weitergabe: nicht senden, was der Empfänger nicht braucht.

---

## 11. "Weiter versuchen, bis der Wechsel klappt" funktioniert jetzt wirklich

**Was:** `channel_manage.c`: `chan_tick` und `chan_has_pending_work`.

**Vorher (zwei Fehler):**
1. `chan_tick` löschte das "Position hat sich geändert"-Flag **vor** der 500-ms-Drosselung. Ein gedrosselter
   Tick schluckte die Änderung, ohne etwas zu tun.
2. Der Wunsch "Wechsel noch offen" war nur ein einmaliger `ts3_request_wakeup()`. Die CEDRAIN-Schleife
   läuft aber nur, wenn `chan_has_pending_work()` etwas meldet — und das zählte nur "in flight" und
   "Position". Ein durch die 2-s-Pause blockierter Wechsel ohne Positions-Updates (Spiel zu) wurde nie
   wiederholt.

**Warum besser:** (1) Erst drosseln, dann Flag löschen: der Tick läuft später nach. (2) Neues Flag
`g_moveWanted`: gesetzt, wenn ein Wechsel gewünscht ist und nur an einer **kurzen** Wartezeit hängt (laufender
Wechsel, 2-s-Pause, Backoff bis 10 s); gelöscht, sobald nichts mehr gewünscht ist oder der Wechsel geklappt
hat. Bei langen Wartezeiten (30 s / 60 s Backoff) hält das Plugin die Schleife **nicht** am Leben — sie
schickt alle 30 ms eine Nachricht an den Server, das wäre eine Minute Dauerfeuer. Dann entscheidet das
nächste TS-Ereignis über den Neuversuch. Auch Warten auf das Root-Profil oder auf das Passwort der Mod hält
die Schleife nicht an (wie bisher: dort wartet das Plugin auf das nächste Ereignis).

**Test:** `channel_manage_test.c` [4], [9]: Pause blockiert → Schleife aktiv; 30-s-Backoff → Schleife aus;
gedrosselter Tick behält das Positions-Flag.

**Lerneffekt:** Ein Flag darf erst gelöscht werden, wenn die Arbeit wirklich erledigt wurde — nicht schon,
wenn man sie "gesehen" hat.

---

## 12. Nickname-Zustand gehört zur Verbindung (Server-Tabs)

**Was:** `nick_anonymize.c`: der gemerkte echte Name hängt an `NickBinding` (Verbindungs-Handle +
Server-UID).

**Vorher:** Es gab nur *einen* globalen "gemerkten Namen". Tab A anonymisiert ("Alice" gemerkt), Wechsel auf
Tab B überschreibt mit "Bob", zurück auf A: "Bob" wurde auf Server A gesetzt.

**Warum besser:** Beim Wiederherstellen wird geprüft, ob der gemerkte Name zu **dieser** Verbindung gehört.
Passt er nicht, wird er verworfen (mit Log-Zeile) statt auf den falschen Server zu gehen. Der übliche Fall
(selber Tab, selber Server, Neuverbindung mit noch gesetzter Zahl) funktioniert weiter. Ist die Server-UID
auf einer Seite nicht verfügbar, entscheidet das Handle allein.

**Grenze:** Es gibt weiter nur einen Speicherplatz. Wer in zwei Tabs gleichzeitig anonymisiert ist, bekommt
nach dem Tab-Wechsel den echten Namen des ersten Tabs nicht mehr zurück (er bleibt bei der Zahl) — aber nie
mehr den *falschen* Namen. `ts3_entry.c` muss dafür **nicht** `nick_reset()` pro Verbindung aufrufen (das
würde die Wiederherstellung nach einem Relog kaputt machen).

**Test:** `tests/nick_anonymize_test.c` [4]-[6] (Tab-Wechsel, gleicher Tab, Serverwechsel im Tab).

**Lerneffekt:** Zustand, der zu einer Verbindung gehört, braucht den Verbindungsnamen als Schlüssel.

---

## 13. Lange Namen (CJK / Emoji) bleiben heil

**Was:** `nick_anonymize.c`: alle Namens-Puffer von 64 auf `4 * 64 + 1` = 257 Bytes.

**Vorher:** TeamSpeak erlaubt 64 **Zeichen**; als UTF-8 sind das bis zu 256 Byte. Der 64-Byte-Puffer schnitt
lange Namen mitten im Zeichen ab, der "wiederhergestellte" Name war kaputt.

**Warum besser:** Der Puffer passt zur TS-Grenze. Ein 256-Byte-Name kommt Byte für Byte zurück.

**Test:** `nick_anonymize_test.c` [2]: 64 chinesische Zeichen (192 Byte) und 64 Emoji (256 Byte).

**Lerneffekt:** "Zeichen" und "Bytes" sind bei UTF-8 zwei verschiedene Zahlen.

---

## 14. Datenschutz: der echte Name steht nicht mehr im Log

**Was:** `nick_anonymize.c`: die Log-Zeilen enthalten nur noch die anonymisierte Zahl und die Länge in Byte
("pre-ingame anonymized -> 48291057 (real name saved, 5 bytes)").

**Vorher:** Das Log hatte bei jeder Anonymisierung/Wiederherstellung "… (was 'Alice')". Logdateien werden
gern in Support-Tickets angehängt.

**Warum besser:** Das Log verrät den Klarnamen nie, hilft aber beim Debuggen weiter (Zahl + Länge).

**Test:** `nick_anonymize_test.c` [3]: alle Pfade (anonymisieren, wiederverwenden, wiederherstellen,
Fehlschlag, "keine Zahl vergeben") durchlaufen; kein Log-Eintrag enthält den Namen oder ein Stück davon.

**Lerneffekt:** Logs sind eine Datenabfluss-Stelle. Im Zweifel nur loggen, was man auch in ein Support-Ticket
schreiben würde.

---

## 15. Hotkey-Zustand hat genau einen Schreiber

**Was:** `voice_modes.c/.h`.

**Vorher (zwei Fehler):**
1. `g_keyArmed` / `g_keySuppressUntil` gehören dem Tastatur-Thread, aber `voice_mode_reset_key_tracking()`
   (ein `memset`) und `voice_mode_notify_hotkey()` wurden aus dem Callback-Thread bzw. UI-Threads
   aufgerufen: zwei Threads schrieben dieselben Felder.
2. `voice_mode_apply` prüfte "ist der Modus schon so?" und setzte ihn danach — zwei Threads gleichzeitig
   sahen beide "geändert" → doppelte Chat-Zeile und doppeltes CEMODE.

**Warum besser:**
1. Beide Funktionen setzen nur noch ein **Anfrage-Flag** (`g_keyResetRequested`,
   `g_keyNotifyRequested[vk]`). Der Tastatur-Thread erledigt es im nächsten `voice_mode_hotkey_poll` — der
   Zustand hat wieder genau einen Schreiber. Aufrufbar bleibt es aus jedem Thread (C8).
2. `InterlockedExchange` tauscht den Modus in einem Schritt; nur der Aufrufer, der den *alten* Wert (≠ neu)
   sieht, löst die Meldungen aus.
Der Kommentar im Header ("Pos watcher thread") stimmte nicht und ist korrigiert.

**Test:** `voice_modes_test.c` [5]+[6] (eine Änderung → genau eine Meldung; zwei Threads × 3000 Runden:
nie doppelt, nie verloren; gegen den alten Code getestet: der alte Code fällt durch) und
`tests/voice_keys_test.c` (Reset/Notify wirken erst im Poll; Grundverhalten bleibt).

**Lerneffekt:** "Prüfen, dann setzen" ist bei mehreren Threads zwei Schritte — ein atomarer Tausch ist einer.

---

## Neue und geänderte Dateien

| Datei | Änderung |
|---|---|
| `src/ts/channel/chan_move_backoff.h` (neu) | Reine Backoff-Rechnung (5/10/30/60 s) |
| `src/ts/channel/channel_manage.c/.h` | Fix 1, 2, 3, 4, 5, 10, 11; neue Funktionen `chan_on_move_failed`, `chan_on_channel_list_changed` |
| `src/ts/profile/ts3_server_profile.c/.h` | Fix 3, 4, 5, 6, 7, 8, 9; neue Funktionen `server_profile_on_channel_list_changed`, `server_profile_force_distance_muting` |
| `src/ts/nick/nick_anonymize.c/.h` | Fix 12, 13, 14 |
| `src/core/voice/voice_modes.c/.h` | Fix 15 |
| `tests/chan_move_backoff_test.c` (neu) | Backoff-Zeitplan |
| `tests/channel_manage_test.c` (neu) | `channel_manage.c` gegen Fake-TS (Fix 1, 2, 3, 4, 5, 10, 11) |
| `tests/server_profile_test.c` (neu) | `ts3_server_profile.c` gegen Fake-TS (Fix 3-9) |
| `tests/nick_anonymize_test.c` (neu) | Fix 12-14 |
| `tests/voice_keys_test.c` (neu) | Fix 15 (Hotkey-Anfragen) |
| `tests/voice_modes_test.c` | + Fix 15 (atomarer Modus-Wechsel, 2-Thread-Test) |

Die neuen Tests binden die `.c`-Datei per `#include` ein und ersetzen alle TS-/Win32-Aufrufe durch kleine
Attrappen. So läuft der **echte** Plugin-Code auf einem Linux-Rechner, ohne TeamSpeak.

## Zusammenspiel mit anderen Paketen

- **Adapter** (`ts3_entry.c`): ruft `chan_on_move_failed` aus `onServerErrorEvent`,
  `chan_on_channel_list_changed` und `server_profile_on_channel_list_changed` bei Kanal-Events; liefert
  `ts3_get_channel_parent`.
- **Config-IO**: `config_modify` / atomares `config_save` (Fix 8).
- **Audio** (`ts3_proximity_audio.c`): ruft an zwei Stellen noch `server_profile_get(&hub)` (8-KB-Kopie).
  Das gehört dem Audio-Paket; die Einzelwert-Getter oben sind dafür ausreichend, falls es nur Flags braucht.

## Was ein Mensch im echten TS-Client prüfen sollte

1. **Falsches Passwort:** Kanal "Ingame" mit Passwort, Mod-Passwort absichtlich falsch → im Log höchstens
   eine "rejected"-Zeile pro Versuch (Abstand 5, 10, 30, 60 s), der echte Nickname kommt im Hub sofort
   zurück, keine TS-Fehlerflut.
2. **Ingame löschen + neu anlegen** (Admin): Spieler im neuen Kanal hören sofort wieder Proximity-Ton, ohne
   Plugin-Neustart. Dasselbe mit "Root" (Beschreibung des neuen Root wird geladen).
3. **Server mit > 128 Kanälen:** Hub/Ingame/Root hinter Platz 128 werden gefunden (Log:
   "CHAN: resolved hub=… ingame=…").
4. **Nicht-Conan-Server:** keine Dauer-Suche im Log, kein Ruckeln beim Verbinden.
5. **Zwei Server-Tabs:** Tab A ingame (Zahl als Name), zu Tab B wechseln, zurück → Tab A bekommt **nie** den
   Namen von Tab B.
6. **Langer Name** (64 Zeichen, z. B. nur Kanji): ingame → Hub: Name kommt unverändert zurück; `plugin.log`
   enthält den echten Namen nicht.
7. **Spiel schließen** (Pos.txt wird alt) bei gesperrtem Hub-Wechsel: das Plugin versucht es weiter, ohne dass
   ein TS-Ereignis nötig ist.
8. **Hotkeys:** Preset laden / Tasten ändern, danach Flüstern/Normal/Schreien: kein doppelter Chat-Eintrag.
