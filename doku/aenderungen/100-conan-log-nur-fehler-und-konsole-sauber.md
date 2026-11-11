# 100 — Conan-Log: Mod stellt `LogModHttp` auf „nur Fehler“, `TSMOUTH` nicht mehr in der Konsole

**Nur Mod** (`Vo_PosStreamer_Actor`, nicht in Git), 2026-11-10. Das Plugin ist unveraendert (8.0.22).
Die Mod-Pak ist **noch nicht gekocht und nicht hochgeladen** (der Besitzer kocht selbst).

> **Stand 2026-11-11:** Der Befehl beim Start (`BeginPlay`) hat im Spiel-Client **nicht gewirkt**. Die Mod sendet
> ihn jetzt erst, wenn die eigene Figur da ist, und schreibt Markierungszeilen ins Log. Siehe Abschnitt 9.
> Die Konsolen-Korrektur (`Log String`) ist davon nicht betroffen.

Meldungen des Besitzers:
1. *„Log spam fixen das ist zu viel“* (`ConanSandbox.log` des Spiel-Clients).
2. *„Das soll da nicht stehen“* — die `TSMOUTH`-Zeilen in der Admin-Konsole im Spiel.

Vorgabe dazu: Die 33 Anfragen pro Sekunde der Mod bleiben, wie sie sind. Voice darf sich nicht aendern,
es geht nur um das Log.

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| Mod `Vo_PosStreamer_Actor`, `EventGraph` | Neuer Knoten `Execute Console Command` mit dem Befehl `Log LogModHttp Warning`. Er sitzt auf dem Client-Pfad zwischen `Branch (IsDedicatedServer)` → `else` und dem 30-ms-Timer, laeuft also einmal beim Start, **bevor** die erste Anfrage gesendet wird. Auf dem dedizierten Server laeuft er nicht. |
| Mod `Vo_PosStreamer_Actor`, Funktion `UpdateTalk` | Der Knoten `Print String` (Zeile `TSMOUTH …`) ist durch `Log String` ersetzt. Text, Drossel (hoechstens alle 2 s) und Reihenfolge sind gleich. |
| PC des Besitzers (nicht Teil der Mod) | Der Log-Ordner des Trebuchet-Profils `MainBlacki` ist NTFS-komprimiert (482 MB → 151 MB). |

---

## 2. Wie war es vorher?

- **Log-Menge:** Das Spiel protokolliert jede HTTP-Anfrage einer Mod mit zwei Zeilen der Kategorie
  `LogModHttp` („Starting request …“ und „Request … succeeded …“). Die Mod sendet alle 30 ms eine Anfrage
  ans Plugin, das ergibt 66 Zeilen pro Sekunde und rund 35 MB pro Stunde. In jeder Log-Datei waren
  92–97 % der Zeilen `LogModHttp`; der Log-Ordner (etwa fuenf Tage) belegte 485 MB. Das war schon immer
  so und kommt nicht vom `TSMOUTH`-Log (das ist gedrosselt).
- **Konsole:** `Print String` schreibt eine Zeile immer in das Log **und** in die Konsole im Spiel. Wer
  die Admin-Konsole oeffnete, sah deshalb laufend `TSMOUTH name=… dist_cm=…`.

---

## 3. Warum ist die neue Loesung besser?

- **Voice bleibt gleich.** An den Anfragen aendert sich nichts: gleiche Anzahl, gleicher Inhalt. Nur
  das Spiel schreibt die Erfolgsmeldungen nicht mehr mit.
- **Fehler bleiben sichtbar.** Fehlgeschlagene Anfragen schreibt das Spiel als `Warning`
  (`Request … failed with error …TimedOut/ConnectionError`). Genau diese Stufe bleibt an. Zum Debuggen
  der Verbindung Mod ↔ Plugin fehlt also nichts.
- **Gilt fuer alle Nutzer der Mod**, ohne dass jemand etwas einstellen muss.
- **Konsole sauber, Log bleibt.** `Log String` schreibt nur in die Log-Datei. Die `TSMOUTH`-Zeile steht
  weiter in `ConanSandbox.log`, aber nicht mehr in der Konsole.

**Nebenwirkungen, die man kennen muss:**

- Die Log-Stufe gilt fuer die **ganze Kategorie**. Auch die Erfolgsmeldungen **anderer Mods** (z. B.
  ModAdmin) verschwinden bei jedem Spieler, bis er das Spiel neu startet. Fehler anderer Mods bleiben.
- Beim Start erscheint voraussichtlich **eine** Bestaetigungszeile `LogModHttp … Warning` in Log und Konsole.
- Die `TSMOUTH`-Zeile hat im Log nicht mehr den Vorsatz `[Vo_PosStreamer_Actor_C_…]`.
- Wer die Erfolgsmeldungen zum Debuggen wieder braucht, tippt in der Konsole `Log LogModHttp Log`.

---

## 4. Wie funktioniert es?

```
BeginPlay
  └─ Branch: IsDedicatedServer?
       ├─ ja   → nichts (Server sendet nichts)
       └─ nein → Execute Console Command "Log LogModHttp Warning"     (neu)
                 → Set Timer "UpdatePosition", alle 0,03 s             (wie bisher)

UpdateTalk (pro Sprecherpunkt, wenn eine Figur gewinnt, hoechstens alle 2 s)
  └─ Log String "TSMOUTH name=<Charaktername> dist_cm=<Abstand>"      (vorher Print String)
```

**Log-Stufen kurz erklaert:** Jede Log-Kategorie hat eine Stufe. `Log` heisst „alles schreiben“,
`Warning` heisst „nur Warnungen und Fehler schreiben“. Der Konsolenbefehl `Log <Kategorie> <Stufe>`
stellt das fuer die laufende Sitzung um; nach einem Neustart gilt wieder der Standard.

---

## 5. Wie wurde es getestet?

**Im Spiel-Client (Shipping, von Hand):** Der Besitzer hat `Log LogModHttp Warning` am 10.11. um 19:48:06
in die Konsole getippt. Davor 66 `LogModHttp`-Zeilen pro Sekunde, danach in zwei Minuten **keine einzige**.
Das Plugin bekam weiter Positionen (`AUDIO-STAT … localPos=ok`), Voice lief unveraendert.

**Im DevKit-Editor (`execute_python`):**
- Beide Graphen kompilieren ohne Fehler und Warnungen.
- Verdrahtung geprueft: `Branch.else → Execute Console Command → Set Timer`, Befehlstext stimmt;
  `Branch.then → Log String → Set TalkLogNext`, Text kommt aus derselben Verkettung wie vorher.
- Editor-Test der Mundbewegung (9 Faelle wie in Doku 098) unveraendert bestanden.
- Die Zeile erscheint im DevKit-Log als `LogBlueprintUserMessages: TSMOUTH name=… dist_cm=…`.
- Gespeichert wurde nur `Vo_PosStreamer_Actor.uasset`. In der gespeicherten Datei steht kein
  `PrintString` mehr, dafuer `LogString`, `ExecuteConsoleCommand` und der Befehlstext.
- Knotenzahl: `EventGraph` 21 → 22, `UpdateTalk` 72 → 72 (einer weg, einer neu), alle anderen Graphen gleich.

**Nicht geprueft:**
- **Kein Spieltest im Editor (PIE).** Absicht: Der Besitzer war waehrenddessen im Spiel. Eine Test-Figur im
  Editor haette Positionen an sein laufendes Plugin geschickt.
- Ob der Befehl im Spiel-Client auch wirkt, wenn ihn **die Mod** ausloest (statt von Hand getippt).
- Ob die `TSMOUTH`-Zeile in der Konsole im Spiel wirklich fehlt.
- Die gekochte Pak.

Pruefung nach dem Kochen: kurz nach dem Einloggen steht in `ConanSandbox.log` eine Zeile
`LogModHttp … Warning`, und es gibt keine Zeilen „Starting request from mod 'teamspeak'“ mehr.
**Ergebnis am 11.11.: nicht bestanden** — der Befehl aus `BeginPlay` wirkte im Spiel-Client nicht. Der aktuelle
Stand der Mod und die neue Pruefung stehen in Abschnitt 9.

---

## 6. Was der Besitzer tun muss

1. Mod kochen (`cook_teamspeak.cmd`) und die Pak hochladen.
2. Spiel starten, Konsole oeffnen: keine `TSMOUTH`-Zeilen mehr. Danach das Log ansehen (siehe oben).
3. Falls es nicht gewuenscht ist: Die Sicherung vor der Aenderung liegt unter
   `C:\Users\uwebu\Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261110-konsole`.

---

## 7. Was die Logs sonst gezeigt haben (Auswertung 2026-11-10)

- **Verbindung Mod ↔ Plugin:** stabil. Die 89 221 Fehler im Log-Ordner sind fast alle Timeouts und liegen
  in zwei Zeitraeumen, in denen kein Plugin lief (06.11. 17:14–17:50, 08.11. 09:59–10:09). Mit laufendem
  Plugin gab es keinen einzigen.
- **Mundbewegung:** Die `TSMOUTH`-Zeile erscheint im Spiel-Client (in Doku 098 noch offen). Bei 272 Zeilen
  und sieben verschiedenen Figuren war der Charaktername nie leer. Der Sprecherpunkt lag im Mittel 0,7 cm
  neben der Figur, in 99 % der Faelle unter 28 cm, hoechstens 97 cm. Die 200 cm Radius reichen.
- **Plugin 8.0.22:** keine Anti-Flood-Fehler, keine Warnung zu veralteten Positionen.
- **NTFS-Kompression** einer echten Log-Datei: 3,2 : 1.

**Bewusst nicht gebaut:** seltener senden, nur bei Aenderung senden, Position nur per `Pos.txt`. Alle drei
greifen in den Ablauf der 33 Anfragen pro Sekunde ein, und der soll bleiben.

---

## 8. Lerneffekt

Wer ein Log verkleinern will, muss zuerst wissen, **wer** die Zeilen schreibt. Hier war es nicht unser
Blueprint, sondern das Spiel selbst — also hilft kein Umbau am eigenen Code, sondern die Log-Stufe der
fremden Kategorie. Und: `Print String` ist zum Anzeigen gedacht, `Log String` zum Mitschreiben.

---

## 9. Nachtrag 2026-11-11 — Befehl beim Start wirkte im Spiel nicht; jetzt erst, wenn die Figur da ist

### Was beobachtet wurde

Mit der am 11.11. um 05:41 gekochten Pak liefen drei Sitzungen: zweimal Einzelspieler, einmal der echte Server.
In allen dreien schrieb das Spiel weiter 66 `LogModHttp`-Zeilen pro Sekunde. Es gab **keine** Bestaetigungszeile
`LogModHttp … Warning` und auch keine Fehlermeldung („Command not recognized“). Der Timer nach dem Knoten lief,
die Positionen kamen an. Der Befehl wurde also weder abgelehnt noch hat er etwas zerstoert — das Spiel hat ihn
schlicht nicht verarbeitet.

### Was dazu geprueft wurde

| Frage | Ergebnis |
|---|---|
| Steckt die Aenderung in der geladenen Pak? | Ja. Der Befehlstext steht in der gekochten Datei, und die vom Spiel entpackte Pak ist bytegleich mit der gekochten. |
| Filtert das Spiel Konsolenbefehle aus Mods? | In der Spieldatei fand sich kein Hinweis darauf (Einschraenkungen fuer Mods gibt es dort nur bei HTTP). Ausgeschlossen ist es damit nicht. |
| Wirkt der Knoten im DevKit? | Ja, in jedem Fall: ohne Spieler-Controller, beim Start der Spielwelt und spaeter. Jedes Mal erscheint die Bestaetigungszeile. |
| Lag es am Einzelspieler? | Nein, auf dem Server war es genauso. |

Die Ursache im fertigen Spiel-Client ist damit **nicht belegt**. Zwei Erklaerungen bleiben: Beim Start des
Mod-Actors ist der Spieler-Controller im Spiel noch nicht vollstaendig eingerichtet (dann verwirft die Engine den
Befehl stillschweigend), oder der Spiel-Client fuehrt Konsolenbefehle aus Blueprints gar nicht aus.

### Was geaendert wurde

| Wo | Aenderung |
|---|---|
| `EventGraph` | Der Knoten `Execute Console Command` ist wieder entfernt. An seiner Stelle schreibt `Log String` einmal `TSLOG teamspeak mod started pc=<true/false>` (ob beim Start schon ein Spieler-Controller da war). |
| `UpdatePosition` | Hinter der vorhandenen Pruefung „eigene Figur gueltig?“ sitzt eine Einmal-Weiche (neue Variable `LogCmdDone`). Beim ersten Durchlauf: `Log String` mit `TSLOG teamspeak mod sets LogModHttp to Warning (undo: Log LogModHttp Log) pc=<true/false>`, dann `Execute Console Command` mit `Log LogModHttp Warning` und dem eigenen Spieler-Controller. Danach geht es weiter wie bisher. |

Der Ablauf der Anfragen ist unveraendert: Bei jedem weiteren Durchlauf kommt nur eine einzelne Abfrage der
Variable dazu, die sofort zum alten Weg fuehrt.

```
UpdatePosition (alle 0,03 s)
  └─ eigene Figur gueltig?
       └─ ja → LogCmdDone?
                 ├─ ja   → weiter wie bisher (Position, Pos.txt, HTTP)
                 └─ nein → LogCmdDone = true → Log String (TSLOG …)
                           → Execute Console Command → weiter wie bisher
```

### Wie es getestet wurde (DevKit)

- Beide Graphen kompilieren ohne Fehler und Warnungen; die Verdrahtung wurde Pin fuer Pin geprueft.
- **Spieltest im Editor (PIE):** `TSLOG teamspeak mod started pc=true`, 1,4 s spaeter
  `TSLOG teamspeak mod sets LogModHttp to Warning … pc=true`, direkt danach die Bestaetigungszeile
  `LogModHttp … Warning`. Die Markierung erschien genau einmal. Die Anfragen liefen danach weiter.
- Die Anfragen aus dem Editor erreichen das echte Plugin nicht: Das DevKit lehnt sie mangels Freigabe ab
  (`NotApprovedByPlayer`). Der Test stoert also niemanden, der gerade spielt.
- Editor-Test der Mundbewegung (9 Faelle) unveraendert bestanden.
- Gespeichert wurde nur `Vo_PosStreamer_Actor.uasset`. Knotenzahl: `EventGraph` 22 → 26, `UpdatePosition` 77 → 86.

**Nicht geprueft:** der Spiel-Client. Genau dort ist die erste Variante gescheitert.

### So liest man das Ergebnis nach dem Kochen (`ConanSandbox.log`)

Ob der Befehl wirkt, zeigen die Zeilen des Spiels selbst: die Bestaetigungszeile `LogModHttp … Warning` und ob
die Anfragezeilen aufhoeren. Die `TSLOG`-Zeilen sind nur Zusatz. Ob die neue Pak geladen wurde, steht in den
Zeilen `Mounting mod pak file` und `… correct hash` am Anfang des Logs.

| Im Log steht | Bedeutung |
|---|---|
| Bestaetigungszeile `LogModHttp … Warning`, und keine Zeilen „Starting request from mod 'teamspeak'“ mehr | Es wirkt. |
| `TSLOG … sets LogModHttp to Warning … pc=true`, aber keine Bestaetigungszeile, und die Anfragezeilen laufen weiter | Der Spiel-Client fuehrt Konsolenbefehle aus Blueprints nicht aus. Dann kommt der Knoten wieder heraus, und es bleibt nur der Weg ueber Plugin oder Filter. |
| Gar keine `TSLOG`-Zeile, obwohl die neue Pak geladen wurde | `Log String` schreibt im Spiel-Client nicht ins Log. Dann fehlt dort auch die `TSMOUTH`-Zeile, und sie muss zurueck auf `Print String` (mit Anzeige in der Konsole) oder ganz entfallen. |

**Noch offen:** `Log String` wurde im Spiel-Client bisher nicht beobachtet. In den Sitzungen vom 11.11. hat
niemand in der Naehe gesprochen, es gab also keinen Anlass fuer eine `TSMOUTH`-Zeile. Die `TSLOG`-Zeilen klaeren
das im selben Test mit.

**Reihenfolge fuer den Test:** kochen, im Einzelspieler mit der lokalen Pak starten, Log pruefen lassen, und erst
bei vorhandener Bestaetigungszeile in den Workshop hochladen. (Im Einzelspieler bleibt man im Root- oder
Hub-Channel, siehe Nebenbefund; fuer den Log-Befehl spielt das keine Rolle.)

Sicherung vor dieser Aenderung:
`C:\Users\uwebu\Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261111-befehl`.

### Nebenbefund: kein Channel-Wechsel im Einzelspieler

Am 11.11. morgens blieb der Besitzer im Einzelspieler im Root-Channel haengen. Das lag nicht an der Mod-Aenderung:
Das Ingame-Passwort liegt im Savegame des Servers (`dc tsvm <Passwort>`, Doku 077). Im Einzelspieler gibt es
dieses Savegame nicht, also sendet die Mod kein Passwort, und das Plugin wartet. Auf dem echten Server kam das
Passwort mit der neuen Pak um 06:09:18 an, eine halbe Sekunde spaeter folgte der Wechsel in den Ingame-Channel.

Offen (Plugin, nicht gebaut): Wer bei laufendem Spiel neu auf den TeamSpeak-Server kommt und kein Passwort
bekommt, bleibt im Root-Channel statt im Hub, bis das Spiel beendet wird.
