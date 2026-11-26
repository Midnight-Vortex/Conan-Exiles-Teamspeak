# 106 — Ruckler am 25.11.: Wartet das Spiel auf die Antwort des Plugins? (Messwerkzeuge, keine Mod-Aenderung)

**Stand 2026-11-26.** Neu sind nur zwei Test-Werkzeuge im Ordner `tests/`. Am Plugin und an der Mod wurde
**nichts** geaendert, gekocht oder hochgeladen. Die Haertung des Plugins selbst laeuft getrennt (Doku 105, Branch
`claude/bold-cray-dbfsjh`).

**Begriffe:** *Mod-Anfrage* = die Positions-Anfrage `POST http://127.0.0.1:52734/v1/position`, die die Mod 33-mal
pro Sekunde an das Plugin schickt. *Bild* (Frame) = ein Durchlauf des Spiels; im `ConanSandbox.log` steht die
Bildnummer in der zweiten eckigen Klammer, z. B. `[709]`. *Spiel-Thread* = der Teil des Spiels, der die Welt
rechnet und die Mod-Anfragen startet und auswertet.

## 1. Ausgangslage

Spieler meldeten am Abend des 25.11. Ruckler etwa alle 3 s (80 bis 480 ms). Eine andere Sitzung hatte gemessen:
Jeder Ruckler ueber 80 ms lag zwischen „Anfrage gestartet“ und „Antwort erhalten“ einer Mod-Anfrage.

Offen war: Hat das Plugin zu spaet geantwortet und das Spiel darauf gewartet — oder stand der Spiel-Thread aus
einem anderen Grund, und eine gerade laufende Anfrage sah deshalb langsam aus?

## 2. Was die Spuren zeigen

**Auf diesem PC gibt es keine Logs aus der Ruckel-Zeit (22:24 bis 23:46).** TeamSpeak lief nur bis 21:31, das Spiel-
Log endet um 21:20. Die Messung der anderen Sitzung stammt von einem anderen Spieler.

**Wie das Spiel die Anfragen protokolliert** (aus dem Log vom 25.11., 16:36 bis 21:20, 405 035 Anfragen):

```
[2026.11.25-16.56.49:190][709]LogModHttp: Starting request from mod 'teamspeak' ... 2.0s timeout to: .../v1/position
[2026.11.25-16.56.49:516][709]LogModHttp: Request ... succeeded with code 200 and content length 104.
```

- Beide Zeilen schreibt der **Spiel-Thread**. Die Zeit bei „succeeded“ ist die Zeit, zu der das Spiel die Antwort
  **verarbeitet** hat — nicht, wann das Plugin sie geschickt hat.
- 99,6 % der Antworten werden **im selben Bild** verarbeitet, in dem die Anfrage startete (Mittelwert 9 ms).
- **Alle 30 Antworten ueber 150 ms lagen im selben Bild** — dieses eine Bild hat also so lange gedauert. Fast alle
  fallen mit etwas zusammen, das das Spiel selbst gerade tat: Teleport („PrepareForTeleportClient“), Nachladen der
  Welt („Waiting for streaming completed“), NVIDIA-Bildgenerierung schaltet um („DLSS-G interpolation state
  changed“, „Frame rate over 100.00ms“), Mod-Skripte anderer Mods.
- Hoechstens **3** Anfragen waren gleichzeitig offen; jede hatte 2,0 s Zeitgrenze.

**Folgerung:** Mit dieser Messmethode laesst sich „das Plugin war langsam“ nicht von „das Spiel stand“ trennen. In
jedem Fall, in dem das Log einen Grund zeigt, war es das Spiel. Ob das am 25.11. bei dem anderen Spieler auch so
war, zeigen nur dessen Logs (siehe Abschnitt 5).

## 3. Wie die Mod die Anfrage stellt (Stand der Mod vom 11.11., unveraendert seit Build 47)

Aus dem Blueprint `Vo_PosStreamer_Actor`, Funktion `UpdatePosition`, so wie er beim Bau von Doku 102 ausgelesen
wurde (die Datei ist seitdem unveraendert; der DevKit lief heute nicht, eine neue Pruefung war nicht moeglich):

| Frage | Antwort |
|---|---|
| Wartet etwas auf die Antwort? | Im Blueprint nicht: „Start Http Request“ ist ein normaler Aufruf, danach laeuft die Runde sofort weiter; die Antwort kommt spaeter ueber die Ereignisse `HTTPSuccess` / `HTTPFaIl`. Ob das Spiel **intern** wartet, kann nur der Messtest zeigen. |
| Startet jede Runde eine neue Anfrage, auch wenn die letzte noch offen ist? | **Ja.** Die Variable `HttpBusy` wird nach dem Start auf wahr und in beiden Antwort-Ereignissen auf falsch gesetzt, aber vor dem Start nirgends abgefragt. Bei einem haengenden Plugin koennten so bis zu 66 Anfragen gleichzeitig offen sein (33 pro s x 2 s). Am 25.11. waren es hoechstens 3. |
| Zeitgrenze der Positions-Anfrage | **2,0 s** (die Status-Frage an den Ausweich-Port hat 1 s). |
| Wird `Pos.txt` jede Runde geschrieben? | **Ja**, mit `SaveStringToFile` auf dem Spiel-Thread, 33-mal pro Sekunde, auch wenn HTTP funktioniert. Wie lange ein Schreibvorgang dauert (Virenscanner, gleichzeitiges Lesen durch das Plugin alle 30 ms), ist nicht gemessen. |

## 4. Die neuen Werkzeuge

| Datei | Wozu |
|---|---|
| `tests/slow_reply_standin.ps1` | Ersatz fuer das Plugin auf Port 52734. Antwortet wie das Plugin (`{"ok":true,"talk":0,"talkers":"","act":1}`, Status mit `act 1`) und laesst alle `-EveryMs` Millisekunden **eine** Antwort `-DelayMs` Millisekunden warten. Bedient wie das Plugin eine Anfrage nach der anderen. Beendet sich nach `-Minutes` Minuten. |
| `tests/modhttp_timing.py` | Wertet ein `ConanSandbox.log` aus: Antwortzeiten, wie viele Anfragen gleichzeitig offen waren, und fuer jede Antwort ueber 150 ms, ob sie **im selben Bild** (das Bild dauerte so lange) oder **in einem spaeteren Bild** (das Spiel lief weiter) verarbeitet wurde, dazu die anderen Log-Zeilen aus dieser Zeit. Am Ende ein Urteil. |

Getestet ohne Spiel: Der Ersatz-Server beantwortete 120 Anfragen im 30-ms-Takt, alle 1 s eine davon 300 ms spaet,
und die Status-Frage; danach war der Port wieder frei. Das Auswerte-Skript lieferte auf dem Log vom 25.11. die
Zahlen aus Abschnitt 2.

## 5. Was noch zu tun ist

**Messtest (5 Minuten, braucht jemanden am Spiel):**

1. TeamSpeak ganz beenden (alle `ts3client_win64.exe`).
2. Ersatz-Server starten:
   `powershell -ExecutionPolicy Bypass -File tests\slow_reply_standin.ps1 -DelayMs 300 -EveryMs 3000`
3. Spiel starten, auf den Server, 2 bis 3 Minuten herumlaufen. Ruckelt es alle 3 s?
4. Spiel beenden, auswerten:
   `python tests\modhttp_timing.py "<Profil>\Logs\ConanSandbox.log"`

- **Laeuft es fluessig** und das Skript meldet „in a LATER frame“: Das Spiel wartet nicht auf die Antwort. Dann
  kommen die Ruckler vom 25.11. nicht vom Plugin, und an der Mod wird nichts geaendert.
- **Ruckelt es im 3-s-Takt** und das Skript meldet „SAME frame“: Das Spiel wartet. Dann die drei kleinen Aenderungen
  an der Mod, einzeln und jeweils mit diesem Test geprueft: hoechstens eine offene Anfrage (`HttpBusy` abfragen),
  kurze Zeitgrenze (z. B. 0,25 s), `Pos.txt` seltener schreiben, solange HTTP antwortet — Letzteres nur mit Blick
  auf die Datei-Ersatzwege in Doku 102 Abschnitt 9.4 und `src/core/mod_file/pos_file.c` (Positionen gelten 5 s,
  HTTP-Vorrang 2 s).

### Ergebnis des Messtests (26.11.2026, 05:10 bis 05:14 Ortszeit)

Der Besitzer hat mit dem Ersatz-Server (`-DelayMs 300 -EveryMs 3000`) gespielt: **Es hat nicht geruckelt.**
`modhttp_timing.py` auf dem Log dieses Laufs (3913 Anfragen):

- Von 510 Antworten, die 150 ms oder laenger brauchten, verarbeitete das Spiel **507 in einem spaeteren Bild**
  (bis zu 140 Bilder spaeter) — das Spiel lief waehrend des Wartens weiter.
- Weil der Ersatz-Server eine Anfrage nach der anderen bedient, stauten sich die Anfragen hinter jeder spaeten
  Antwort: bis zu **69** gleichzeitig offen, 248 liefen in die 2-s-Zeitgrenze. Auch das ruckelte nicht.
- Urteil des Skripts: „the game kept rendering while answers were late -> it does NOT wait for the answer.“

**Folgerung:** Das Spiel wartet nicht auf die Antwort des Plugins. Eine langsame Antwort — selbst 2 s und ein Stau
von 69 Anfragen — erzeugt keinen Ruckler. Die Ruckler vom 25.11. haben eine andere Ursache; die Mod-Anfrage war
nur zufaellig gerade offen. **An der Mod wird deshalb nichts geaendert.** Der fehlende Schutz gegen gestaute
Anfragen (`HttpBusy` wird nicht abgefragt) bleibt bekannt, verursacht aber nachweislich kein Ruckeln.

**Logs des betroffenen Spielers:** sein `ConanSandbox.log` und `plugin.log` vom 25.11. Mit `modhttp_timing.py
<log> 21:24 22:46` (UTC) zeigt sich, was das Spiel in den langen Bildern sonst getan hat.

## 6. Lerneffekt

Ein Zeitstempel im Log sagt, wann eine Zeile **geschrieben** wurde, nicht wann das Ereignis passiert ist. Die
Bildnummer daneben zeigt, ob das Spiel dazwischen weitergelaufen ist.
