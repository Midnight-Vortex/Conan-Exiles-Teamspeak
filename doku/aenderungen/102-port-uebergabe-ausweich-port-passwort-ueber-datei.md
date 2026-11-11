# 102 — Zwei TeamSpeak-Prozesse: Port-Uebergabe, Ausweich-Port, Passwort ueber die Datei (Plugin 8.0.23)

**Plugin 8.0.23**, 2026-11-11. Gebaut und getestet, **nicht verteilt** (kein Release, nichts in TeamSpeak kopiert).
Der **Mod-Teil ist gebaut** (Abschnitt 9, im DevKit getestet, **nicht gekocht**, im Spiel ungetestet).

Meldung des Besitzers: *„Es kommt vereinzelt vor, dass Spieler nicht gemoved werden, aber das Passwort bekommen
sie ja vom Server eigentlich.“*

## 1. Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `src/core/http/http_ports.h` (neu) | Reine Hilfsfunktionen ohne Sockets: Port-Liste (52734, Ausweich-Port 42734), Status-Text, Entscheidung „um Freigabe bitten oder Ausweich-Port“, Feld `act` in der Antwort. |
| `src/core/http/pos_http_server.c/.h` | Der Lauscher belegt den Port **exklusiv**. Ist 52734 besetzt, fragt er den Halter (`GET /v1/status`). Ist das eine nicht verbundene Instanz dieses Plugins, bittet er um Freigabe (`POST /v1/yield`) und uebernimmt den Port. Sonst nimmt er den Ausweich-Port 42734 und wechselt zurueck, sobald 52734 frei ist. Eine **aktive** Instanz auf dem Ausweich-Port fragt alle 5 s erneut nach. Antwortet der Halter nicht und ist er ein eingefrorenes TeamSpeak, wird er beendet (enge Regel, Abschnitt 4). Neue Routen `GET /v1/status` und `POST /v1/yield`; jede Positions-Antwort endet mit `"act":0` oder `"act":1`. |
| `src/ts/entry/ts3_entry.c` | Meldet dem Lauscher, ob **diese** Instanz mit einem Conan-Server verbunden ist („aktiv“), und zeigt einen Hinweis im TeamSpeak-Chat, wenn der Port von etwas Fremdem belegt ist oder gar kein Port frei ist. Version 8.0.23. |
| `src/core/mod_file/pos_file.c`, `src/core/hub/mod_password.c/.h` | Der `Pos.txt`-Leser nimmt ein angehaengtes ` TSPWU=<Passwort, URL-kodiert>` an (Datei-Variante mit Passwort). Es wird nur aus einer **vollstaendigen** Zeile (endet mit Zeilenumbruch) uebernommen. Das Herausschneiden und Zurueckwandeln macht `mod_password_take_from_file_line`. |
| Mod: `Vo_PosStreamer_Actor` (DevKit, nicht im Repo) | Port-Suche, Wechsel auf den Ausweich-Port, Passwort-Zeitfenster in der `Pos.txt` (Abschnitt 9). |
| `project/…vcxproj` | Neue Kopfdatei eingetragen, `iphlpapi.lib` dazu (Abfrage „welcher Prozess haelt den Port“). |
| `tests/http_ports_test.c` (neu), `tests/http_takeover_itest.c/.ps1` (neu), `tests/run_tests.sh` | Einzelpruefungen und ein Test mit zwei echten Prozessen. |

---

## 2. Wie war es vorher? (Befund vom 11.11.2026)

Im Log eines betroffenen Spielers schrieben **zwei Plugin-Instanzen gleichzeitig** — also zwei
TeamSpeak-Prozesse auf einem PC. Nur eine kann `127.0.0.1:52734` belegen:

- Die **aeltere** Instanz hielt den Port. Sie war nicht mehr mit dem Server verbunden, beantwortete aber alle
  38 508 Anfragen der Mod (Antwortlaenge 33 = Instanz ohne Server-Profil) und nahm ihr Position und Passwort ab.
- Die Instanz, mit der der Spieler **wirklich verbunden** war, meldete den ganzen Abend
  `HTTP: bind() failed (10013)`. Sie bekam die Position nur aus der `Pos.txt` und **nie das Passwort**, denn das
  reiste nur in der HTTP-Anfrage. Ergebnis: 39-mal „waiting for the password“, 364-mal „auto-move to ingame
  blocked“ — der Spieler blieb im Hub.

Im Chat stand dazu nichts. Erkennbar war es nur im `plugin.log`.

---

## 3. Warum ist die neue Loesung besser?

- **Der Port geht an die richtige Instanz.** Eine Instanz ohne Conan-Server gibt den Port auf Anfrage ab. Eine
  Instanz **mit** Conan-Server behaelt ihn immer.
- **Die Mod kann die richtige Instanz erkennen.** Jede Antwort sagt mit `act`, ob diese Instanz die mit dem
  Conan-Server ist. (Die Mod wertet das aus, Abschnitt 9.)
- **Ein besetzter Port ist kein Ende mehr.** Haelt ein fremdes Programm 52734, lauscht das Plugin auf 42734 und
  kehrt von selbst zurueck, wenn 52734 frei wird.
- **Nie zwei auf einem Port.** Die Belegung ist exklusiv; der alte Versuch „noch einmal mit `SO_REUSEADDR`“
  ist entfernt, weil sich damit zwei Prozesse denselben Port teilen konnten.
- **Der Spieler erfaehrt es.** Bei fremder Belegung oder ganz ohne freien Port steht ein Hinweis im Chat,
  mit der Prozessnummer des Halters. Im Log steht zusaetzlich der Programmname.
- **Letzte Stufe:** Kommt die Position nur noch ueber die Datei, kommt das Passwort mit (kurze Zeitfenster,
  Abschnitt 9).

**Grenzen:**
- Gegen eine **alte** Instanz (bis 8.0.22) hilft die Uebergabe nicht: Die kennt die neuen Routen nicht. Sie wird
  wie ein fremdes Programm behandelt (Ausweich-Port plus Hinweis). Nach einem Plugin-Update kann auf einem PC
  aber keine alte Instanz mehr laufen, weil TeamSpeak dafuer neu gestartet werden muss.
- Die **veroeffentlichte** Mod (Build 46) kennt nur 52734. Erst mit der neu gekochten Mod (Abschnitt 9) nuetzt
  ihr der Ausweich-Port; bis dahin haelt er nur die Rueckkehr auf 52734 offen.

---

## 4. Wie funktioniert es?

```
Plugin startet
  └─ 52734 frei? ──ja──► lauschen auf 52734
        │nein
        ▼   (Lauscher-Thread, nie der TeamSpeak-Thread)
     GET /v1/status an den Halter
        ├─ antwortet als dieses Plugin, act=0  ──► POST /v1/yield
        │        Halter schliesst 52734, geht auf 42734 (15 s Sperre fuer 52734)
        │        ──► neue Instanz belegt 52734
        ├─ antwortet als dieses Plugin, act=1  ──► Halter behaelt; neue Instanz auf 42734, kein Hinweis
        └─ antwortet nicht / ist etwas anderes ──► neue Instanz auf 42734, Hinweis im Chat
     42734 auch besetzt ──► kein Port: Position nur ueber Pos.txt, Hinweis im Chat

Solange eine Instanz auf 42734 sitzt, alle 5 s:
  - nicht aktiv: nur zurueckwechseln, wenn 52734 frei ist
  - aktiv:       den Halter erneut fragen (er gibt ab, wenn er nicht aktiv ist)
```

**Warum fragt nur die aktive Instanz nach?** Wuerden zwei unverbundene Instanzen sich gegenseitig fragen,
wanderte der Port alle paar Sekunden hin und her. So bekommt ihn am Ende immer die Instanz mit dem Conan-Server,
auch wenn sie erst spaeter verbunden wird.

**Eingefrorenes TeamSpeak beenden** (Entscheidung des Besitzers, 11.11.2026). Der Halter des Hauptports wird nur
beendet, wenn **alles** zutrifft:

1. Diese Instanz ist aktiv, braucht den Port also fuer einen Conan-Server.
2. Der Halter hat die Status-Frage dreimal hintereinander nicht beantwortet, ueber mindestens 8 Sekunden.
3. Der Halter laeuft aus **derselben Programmdatei** wie dieses TeamSpeak. Ein fremdes Programm wird nie angefasst.
4. Er hat kein sichtbares Fenster mehr, oder eines seiner Fenster reagiert nicht.

Ein gesundes TeamSpeak erfuellt Punkt 2 nie: Sein Plugin antwortet in Millisekunden (ein aelteres Plugin
antwortet mit „nicht gefunden“, auch das ist eine Antwort). Nach dem Beenden steht eine Zeile im Chat und im Log.

**„aktiv“** heisst: Diese Instanz ist verbunden und hat ein gueltiges Conan-Server-Profil (Root-Channel
erkannt). Das setzt der TeamSpeak-Thread bei jedem Durchlauf.

**Neue Routen** (nur von `127.0.0.1`, derselbe Browser-Schutz wie bisher):

| Route | Antwort |
|---|---|
| `GET /v1/status` | `{"ok":true,"service":"conan_exiles_ts","act":0\|1,"pid":N,"port":N,"ver":"8.0.23"}` |
| `POST /v1/yield` | `{"ok":true,"yield":1}` wenn die Instanz den Hauptport haelt und nicht aktiv ist, sonst `"yield":0` |
| `POST /v1/position` | wie bisher, am Ende zusaetzlich `,"act":0\|1` |

**Passwort ueber die Datei:** Steht in der `Pos.txt` hinter der Position ` TSPWU=<Passwort>` und endet die Zeile
mit einem Zeilenumbruch, uebernimmt das Plugin das Passwort. Das Passwort ist dabei **URL-kodiert** (aus `ä`
wird `%C3%A4`, aus einem Leerzeichen `%20`), das Plugin wandelt es zurueck. Grund (gemessen): Schon ein einziger
Umlaut liess das Spiel die ganze Datei als UTF-16 schreiben; dann war weder Position noch Passwort lesbar. Die
Datei wird 33-mal pro Sekunde neu geschrieben; eine halb geschriebene Zeile (ohne Zeilenumbruch) oder ein
kaputtes `%`-Zeichen wird deshalb nie als Passwort genommen. Das Komma-zu-Punkt-Umwandeln der Zahlen passiert
erst **nach** dem Herausschneiden. Ein altes Plugin (bis 8.0.22) ueberliest das Feld; die Position liest es
weiter.

---

## 5. Wie wurde es getestet?

- **Einzelpruefungen** `http_ports_test`: 42/42 gruen (Port-Liste, Status-Text hin und zurueck, Entscheidung,
  Freigabe-Regel, `act` anhaengen samt Puffergrenzen).
- **Zwei echte Prozesse** (`tests/http_takeover_itest.ps1`): Das Testprogramm ist der echte Lauscher mit
  Platzhaltern fuer die Nachbarn. Es laeuft auf den **Test-Ports 47834/47835**, damit ein laufendes TeamSpeak
  nicht beruehrt wird. 13 Ablaeufe, 53 Pruefungen, zweimal hintereinander gruen:

  | Ablauf | Ergebnis |
  |---|---|
  | eine Instanz | Hauptport; Status und `act` in der Antwort stimmen |
  | zwei unverbundene Instanzen | die neue bekommt den Hauptport, die alte geht auf den Ausweich-Port, kein Hinweis |
  | aktive Instanz haelt den Port | sie behaelt ihn, die neue nimmt den Ausweich-Port, kein Hinweis |
  | unverbundener Halter, aktive neue Instanz | die aktive uebernimmt, die Mod erreicht `act=1` |
  | fremdes Programm haelt den Hauptport | Ausweich-Port, Hinweis; nach Freiwerden Rueckkehr auf den Hauptport |
  | beide Ports fremd belegt | kein Port, Hinweis „kein Port frei“ |
  | Instanz auf dem Ausweich-Port wird spaeter aktiv | sie holt sich den Hauptport von der unverbundenen zurueck; danach stabil |
  | Neustart direkt nach 200 Anfragen | die neue Instanz belegt den Hauptport sofort |
  | Uebergabe direkt nach 200 Anfragen | die aktive neue Instanz bekommt den Hauptport |
  | Plugin aus und wieder an im selben Prozess, nach 150 Anfragen | wieder Hauptport, kein Ausweich-Port, `act` wird neu gesetzt |
  | eingefrorener Halter (gleiches Programm), aktive Instanz | nach 4 s noch nichts beendet; danach beendet, Hauptport uebernommen, eine Meldung |
  | eingefrorener Halter, **nicht** aktive Instanz | nach 16 s nichts beendet |
  | fremdes Programm antwortet nie, aktive Instanz, 16 s | nichts beendet |

- **Gegenpruefung** durch unabhaengige Pruefer (drei Blickwinkel, jeder Befund einzeln nachgeprueft): 11 Befunde,
  7 bestaetigt, 4 widerlegt. Behoben:
  1. Eine Instanz, die erst auf dem Ausweich-Port aktiv wird, holte sich den Hauptport nicht zurueck — das waere
     der urspruengliche Fehler in der Haelfte der Startreihenfolgen gewesen. Jetzt fragt sie alle 5 s nach.
  2. Der Merker „aktiv“ ueberlebte ein Aus- und Einschalten des Plugins. Er wird beim Start zurueckgesetzt.
  3. Die Frage an den Halter hatte keine Gesamt-Zeitgrenze; ein Programm, das Byte fuer Byte antwortet, haette
     den Lauscher und damit das Beenden von TeamSpeak aufhalten koennen. Jetzt gilt eine feste Grenze.
  4. Nach einer zugesagten Freigabe nimmt die fragende Instanz nicht mehr den Ausweich-Port (dorthin zieht ja
     gerade der Halter), sondern wartet auf den Hauptport.
  
  Ein Einwand (exklusive Belegung scheitere, solange alte Verbindungen ausklingen) liess sich in den drei
  Last-Ablaeufen nicht nachstellen; die exklusive Belegung bleibt.

- **Uebrige Host-Tests** (`tests/run_tests.sh`, MSYS2-gcc): 33 Gruppen gruen, darunter `layering_guard`. Wie
  bisher kompilieren `player_table_test`, `render_state_test` und `proto_flush_test` auf dem Windows-gcc nicht;
  das betrifft keine geaenderte Datei.
- **MSVC-Release-Build** (`build.ps1 -SkipDeploy -SkipInstaller`) fehlerfrei.

**Nicht geprueft:** das Plugin in einem echten TeamSpeak, zwei echte TeamSpeak-Prozesse, der Chat-Hinweis im
Client, das Zusammenspiel echte Mod + echtes Plugin (Abschnitt 9 nennt, was davon im DevKit ging).

---

## 6. Was der Besitzer tun muss

1. Die Mod kochen und zuerst **im Einzelspieler mit der lokalen pak** pruefen (Abschnitt 9.6), erst dann in den
   Workshop.
2. Entscheiden, ob 8.0.23 als Release herausgehen soll. Die Reihenfolge ist egal: Alte Mod + neues Plugin
   verhaelt sich wie bisher. Neue Mod + altes Plugin verhaelt sich wie bisher, **solange TeamSpeak laeuft und
   antwortet**.
   **Wichtig fuer die Entscheidung:** Laeuft TeamSpeak gar nicht, schreibt die neue Mod — egal mit welchem
   Plugin — auf einem Server mit Passwort alle 30 s fuer 3 s das Passwort in die `Pos.txt` und fragt alle 10 s
   den Ausweich-Port. Das ist die Vorgabe woertlich („nur wenn HTTP nicht geht“), trifft aber den haeufigen
   Fall „Spieler ohne TeamSpeak im Spiel“, nicht nur den seltenen „alle Ports gesperrt“.
3. Bis dahin hilft betroffenen Spielern: im Task-Manager alle `ts3client_win64.exe` beenden und TeamSpeak einmal
   neu starten.

---

## 7. Was noch fehlt

- Der Mod-Teil ist gebaut (Abschnitt 9). Offen ist dort nur der Test im echten Spiel.
- Zwei Faelle ohne jede Meldung, beim Lesen des Codes gefunden (nicht beobachtet): Mit `ForceSpeakersOn` wird
  nicht verschoben, wer die Lautsprecher in TeamSpeak stumm hat und im Hub steht; und in Channels mit „event“,
  „support“ oder „plot“ im Namen wird absichtlich nicht verschoben.

---

## 8. Lerneffekt

„Die Anfrage wurde beantwortet“ heisst nicht „vom Richtigen beantwortet“. Erst die Antwortlaenge und die
doppelten Zeilen im Log haben gezeigt, dass eine zweite Instanz mitredet. Seitdem sagt jede Antwort, wer sie
gibt.

---

## 9. Nachtrag — Mod-Teil gebaut (11.11.2026, abends)

**Stand:** im DevKit gebaut, ohne Warnung kompiliert und gespeichert (`Vo_PosStreamer_Actor.uasset`,
2 394 686 Bytes, Stand nach der Gegenpruefung). **Nicht gekocht, im echten Spiel ungetestet.** Sicherung vor der Aenderung:
`Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261111-ports`.

### 9.1 Was wurde geaendert?

| Wo | Aenderung |
|---|---|
| `Vo_PosStreamer_Actor`, neue Variablen | `UseFallback` (sendet die Mod an den Ausweich-Port?), `StreamInactive` (Antwort kommt, aber von einer Instanz ohne Conan-Server), `LastHttpOk` (wann kam zuletzt eine Plugin-Antwort), `ProbeNext` / `ProbeGap` (wann wird der andere Port wieder gefragt), `ProbeAlive` (hat der andere Port zuletzt geantwortet), `PwNext` / `PwUntil` / `PwInFile` (Zeitfenster fuer das Passwort in der Datei), `LastTick` (wann lief die letzte Runde). Alle starten mit „nein“ bzw. 0. |
| neue Funktion `HandleReply(Content)` | Wertet die Antwort auf die Positions-Anfrage aus. |
| neue Funktion `HandleProbe(Content)` | Wertet die Antwort auf die Status-Frage an den anderen Port aus und schaltet um. |
| neue Funktion `ProbeTick()` | Entscheidet, ob der andere Port gefragt wird. |
| neue Funktion `FilePwTick()` | Entscheidet, ob das Passwort in dieser Runde in die Datei darf. |
| neue Funktion `FilePwClose()` | Nimmt das Passwort sofort aus der Datei (schreibt die Zeile ohne Passwort), falls gerade ein Fenster offen ist. |
| Ereignisgraph | Neues Ereignis `SendProbe` (schickt `GET /v1/status` an den anderen Port, 1 s Zeitgrenze) mit zwei eigenen Antwort-Ereignissen. Diese beiden heissen `CustomEvent` und `CustomEvent_0` (liessen sich per Skript nicht umbenennen; im Editor von Hand moeglich, aendert nichts an der Funktion). `HandleReply` haengt zwischen „Antwort lesen“ und der Schall-Auswertung. Neu `Event End Play`: ruft `FilePwClose`, wenn der Spieler die Welt verlaesst. |
| `UpdatePosition` | Fuenf Stellen: `FilePwTick` vor dem Schreiben der Datei; der Dateitext; die Adresse der Positions-Anfrage haengt an `UseFallback`; `ProbeTick` ganz am Ende; hat der Spieler gerade keine Figur (bisher: nichts tun), wird `FilePwClose` gerufen. |
| `modinfo.json` | Die Adresse `http://127.0.0.1:42734` steht unter `requestedHttpOrigins`. **Das hat der Besitzer selbst eingetragen** (Datei vom 11.11. 19:09), daran wurde nichts geaendert. |
| Plugin: `mod_password.c/.h`, `pos_file.c`, `tests/mod_password_test.c` | Die Datei-Variante liest jetzt das Feld `TSPWU=` (URL-kodiert) statt `TSPW=`. |

**Unveraendert:** eine Positions-Anfrage pro Runde (der 0,03-s-Timer, also 33 pro Sekunde), ihr Inhalt
(` TSPW=` im HTTP-Body wie bisher), die Schall- und Mund-Auswertung, `Vo_TsVmHolder`, `DataCmd_TS_Vm`,
`BP_ModController`, `SG_TS_Vm`, die Befehls-Tabelle.

### 9.2 Wie war es vorher?

Die Mod sendete immer an `127.0.0.1:52734`. Antwortete dort die falsche Instanz oder niemand, merkte sie es
nicht. In der `Pos.txt` stand nur die Position — ein Plugin ohne Port bekam das Passwort nie.

### 9.3 Warum ist es besser?

- **Die Mod findet die richtige Instanz.** Sagt die Antwort `act=0` oder bleibt sie aus, fragt die Mod den
  anderen Port. Meldet sich dort die Instanz mit dem Conan-Server, sendet die Mod ab sofort dorthin.
- **Kein Hin-und-her-Springen.** Umgeschaltet wird auf eine Antwort mit `act=1` — oder, wenn der eigene Port
  gerade schweigt, auf jede Instanz des Plugins, die sich meldet (auch eine noch nicht verbundene ist besser als
  niemand: Sie bekommt Position und Passwort und kann den Spieler verschieben, sobald sie verbunden ist). Die
  Antwort sagt selbst, auf welchem Port sie steht (`"port":42734`); zweimal dieselbe Antwort schaltet nicht
  zweimal. Antwortet der eigene Port (auch mit `act=0`), zieht eine zweite unverbundene Instanz die Mod nicht weg.
- **Kein Haengenbleiben auf dem Ausweich-Port.** Der Hauptport ist der Ruhezustand: Antwortet der Ausweich-Port
  5 s lang nicht mehr, sendet die Mod wieder an 52734 und fragt 42734 nur noch nach.
- **Der normale Weg bleibt, wie er ist.** Solange eine brauchbare Antwort kommt, wird nichts gefragt und nichts
  in die Datei geschrieben. Ein altes Plugin (Antwort ohne `act`) gilt als brauchbar.
- **Das Log bleibt ruhig.** Die Suche wird langsamer, solange sie nichts findet: nach 2 s, 4 s, 8 s, dann alle
  10 s. Antwortet der eigene Port, nur eben von einer nicht verbundenen Instanz (TeamSpeak offen, aber nicht auf
  dem Conan-Server), geht es weiter bis auf alle 60 s. Das Spiel schreibt zu jeder Anfrage Zeilen ins
  `ConanSandbox.log`; ohne diese Bremse waeren es dauerhaft alle 2 s welche.
- **Letzte Stufe wie gewuenscht:** Geht HTTP gar nicht, steht das Passwort **kurz** in der Datei — und nur dann.
- **Umlaute im Passwort gehen.** Das Passwort steht URL-kodiert in der Datei (Abschnitt 4).

### 9.4 Wie funktioniert es?

```
jede Runde (unveraendert 33-mal pro Sekunde):
   Position bauen
   FilePwTick   ── darf das Passwort in dieser Runde in die Datei?
   Pos.txt schreiben   (Position; im Zeitfenster dahinter " TSPWU=<Passwort>" + Zeilenende)
   Positions-Anfrage an den Port, auf dem die Mod gerade sendet (52734 oder 42734)
   ProbeTick    ── muss der andere Port gefragt werden?
hat der Spieler gerade keine Figur (Tod, Laden): keine Runde — nur FilePwClose (Passwort aus der Datei)

Antwort auf die Positions-Anfrage ──► HandleReply
   kommt sie vom Plugin (enthaelt "ok":)?            ── Zeit merken (LastHttpOk)
   enthaelt sie "act":0 oder kein "ok":true?         ── StreamInactive = ja, sonst nein

ProbeTick:  StreamInactive  ODER  seit ueber 1 s keine Plugin-Antwort?
   nein ── nichts tun, Abstand der Suche zurueck auf Anfang; eine noch weit entfernt geplante Frage
           (aus einer langsamen Suchphase) wird auf „spaetestens in 2 s“ vorgezogen
   ja   ── sendet die Mod an 42734 und kam dort seit 5 s nichts? dann zurueck auf 52734
           GET /v1/status an den ANDEREN Port; naechste Frage nach 2 s, 4 s, 8 s ...
           ... hoechstens alle 10 s, solange der eigene Port schweigt
           ... hoechstens alle 60 s, solange er antwortet (nur eben ohne Conan-Server)
           wird der eigene Port still, waehrend die Suche schon langsam laeuft: sofort fragen

Antwort auf die Status-Frage ──► HandleProbe
   kommt sie vom Plugin ("service":"conan_exiles_ts")?   ── ProbeAlive = ja
   enthaelt sie "act":1  ODER  schweigt der eigene Port seit ueber 1 s?
                           ── umschalten: UseFallback = steht "port":42734 in der Antwort?
Status-Frage gescheitert   ── ProbeAlive = nein
```

**Passwort in der Datei — die Regeln:**

1. HTTP gilt als ausgefallen, wenn seit **5 s** keine Plugin-Antwort auf die Positions-Anfrage kam **und** die
   letzte Status-Frage an den anderen Port gescheitert ist.
2. Nur dann, und nur wenn der Server ein Passwort hat, oeffnet ein Fenster: **3 s** lang steht das Passwort in
   der Datei, danach **27 s** lang nicht. Das wiederholt sich, solange HTTP ausfaellt. (Das Plugin liest die
   Datei alle 30 ms und merkt sich das Passwort 10 s lang — 3 s reichen.)
3. Kommt wieder eine Antwort, ist das Passwort in der naechsten Runde (30 ms) aus der Datei.
4. Verlaesst der Spieler die Welt waehrend eines Fensters oder hat er ploetzlich keine Figur mehr (Tod, Laden),
   schreibt `FilePwClose` die Datei sofort ohne Passwort.
5. **Pausen zaehlen nicht als Ausfall.** Lief ueber 1 s keine Runde (keine Figur, Ladebild, Haenger), wurde in
   der Zeit auch nichts gesendet — also konnte keine Antwort kommen. Die Uhr „seit wann keine Antwort“ startet
   dann neu. Ohne diese Regel haette nach jedem Respawn fuer einen Augenblick das Passwort in der Datei
   gestanden, obwohl HTTP geht (Befund der Gegenpruefung).

| Zahl | Wert | Wo |
|---|---|---|
| Antwort gilt als ausgeblieben | nach 1 s | `ProbeTick` |
| Abstand der Status-Fragen | 2 s, verdoppelt bis 10 s (eigener Port schweigt) bzw. bis 60 s (eigener Port antwortet mit `act=0`) | `ProbeTick` |
| Zeitgrenze der Status-Frage | 1 s | `SendProbe` |
| HTTP gilt als ausgefallen | nach 5 s | `FilePwTick` |
| Passwort-Fenster | 3 s, alle 30 s | `FilePwTick` |
| Runden gelten als pausiert | nach 1 s ohne Runde | `FilePwTick` |
| Ausweich-Port gilt als tot | nach 5 s ohne Antwort (geprueft, wenn eine Status-Frage faellig ist) | `ProbeTick` |

### 9.5 Wie wurde es getestet?

Im DevKit, Spiel im Editor mit eigenem Server und Client. Dort lehnt das Spiel jede Anfrage der Mod ab („nicht
vom Spieler erlaubt“) und nennt dabei die Adresse — der Zustand „HTTP geht nicht“ ist also von selbst da, und
im Log steht, wohin die Mod senden wollte.

- **Auswertung der Antworten** (frische Figuren, feste Texte, 14 Faelle, alle richtig): aktive Instanz, altes
  Plugin ohne `act`, unverbundene Instanz, abgelehnte Position, fremder Webserver, leere Antwort; Status-Antwort
  unverbunden / aktiv auf dem Ausweich-Port / dieselbe noch einmal / aktiv auf dem Hauptport / altes Plugin /
  ohne `act` / fremder Text mit `act` und `port`.
- **Adressen im Log** (drei Abschnitte eines Laufs):

  | Abschnitt | Positions-Anfragen gingen an | Status-Fragen gingen an |
  |---|---|---|
  | Start | 52734 | 42734, Abstaende 2,0 / 4,0 / 8,0 / 10,1 / 10,0 / 10,0 s |
  | nach einer Status-Antwort „aktiv auf 42734“ | 42734 | 52734 |
  | nach einer Status-Antwort „aktiv auf 52734“ | 52734 | 42734, Abstaende wieder 2,0 / 4,0 / 8,0 / 10,7 s |

  In allen drei Abschnitten genau **eine Positions-Anfrage pro Bild** (der Editor lief im Hintergrund mit 3
  Bildern pro Sekunde; die 33 pro Sekunde selbst lassen sich so nicht messen).
- **`Pos.txt` von aussen mitgeschnitten** (alle 15 ms gelesen):
  - ohne Passwort auf dem Server: nie ein Passwort-Feld, auch nach 20 s Ausfall nicht (kein Fenster verbraucht);
  - mit Passwort: Fenster von 3,005 s und 3,006 s, die Zeile endet mit dem Zeilenende (Byte 0x0A), davor und
    danach die Zeile wie heute (58 Bytes, kein Zeilenende);
  - solange der andere Port als „antwortet“ galt, blieb das faellige Fenster zu; nach der naechsten
    gescheiterten Status-Frage ging es auf;
  - **Gegentest:** 34 s lang kamen (nachgestellte) Plugin-Antworten, darin lag ein faelliges Fenster — kein
    Passwort in der Datei. 5,0 s nach der letzten Antwort ging das Fenster auf;
  - Spiel im offenen Fenster beendet: rund 0,5 s spaeter stand die Datei ohne Passwort da (zweimal geprueft).
- **Umlaut-Fall:** Mit dem ersten Bau (` TSPW=` im Klartext) schrieb das Spiel bei einem Passwort mit `ä` die
  Datei als UTF-16 (erste Bytes `FF FE`, 154 statt 75 Bytes) — unlesbar. Nach dem Umbau steht dort reines ASCII:
  `… YAWY=0 TSPWU=P%C3%A4ss%2Cw%C3%B6rt%201` + Zeilenende. Genau diese Zeile und die Zeile mit einem
  Passwort ohne Umlaut sind als Testfaelle im Plugin-Test.
- **Plugin:** `mod_password_test` 45/45 (neu: 15 Pruefungen fuer die Datei-Zeile — Umlaute, Komma, Leerzeichen,
  fehlendes Zeilenende, abgeschnittene Zeile, kaputtes `%`, Steuerzeichen, Feld mitten in einem anderen Wert,
  nichts im Log). Uebrige Host-Tests 33 Gruppen gruen (die drei bekannten kompilieren auf dem Windows-gcc
  weiterhin nicht), MSVC-Release-Build fehlerfrei.
- **Laufzeitfehler des Blueprints** im DevKit-Log ueber alle sechs Laeufe: 0.

- **Gegenpruefung** durch unabhaengige Pruefer (zwei Blickwinkel: Ablauf im Blueprint; Plugin-Seite und
  Absprache Mod/Plugin; jeder Befund einzeln nachgeprueft): 5 Befunde, 2 bestaetigt, 3 widerlegt. Behoben:
  1. **Passwort trotz funktionierendem HTTP.** Solange der Spieler keine Figur hat, laeuft keine Runde und
     kommt keine Antwort. Nach mehr als 5 s (Tod, Ladebild) sah die erste Runde danach „seit 5 s keine Antwort“
     und haette das Passwort fuer ein bis zwei Runden in die Datei geschrieben und unnoetig den anderen Port
     gefragt. Jetzt Regel 5. Zusaetzlich nimmt `FilePwClose` das Passwort heraus, sobald die Figur fehlt —
     sonst waere es bei einem Tod mitten im Fenster bis zum Respawn in der Datei geblieben.
  2. **Haengen auf dem Ausweich-Port.** Beendete sich die Instanz auf 42734 und stand auf 52734 ein altes
     Plugin oder eine nicht verbundene Instanz, blieb die Mod bis zum Neuverbinden auf dem toten Port. Jetzt
     kehrt sie nach 5 s Stille auf den Hauptport zurueck.

  Nachgetestet im Editor-Spiel: Runden 8 s angehalten, kurz bevor ein Fenster faellig war — nach dem
  Weiterlaufen kein Fenster (am Schalter `PwInFile` abgelesen) und keine sofortige Status-Frage (im Log die
  naechste erst 1,7 s spaeter); das Fenster oeffnete erst 5 s spaeter (dort geht HTTP ja wirklich nicht). Runden im offenen Fenster angehalten und `FilePwClose` gerufen: 0,4 s nach dem
  Oeffnen stand die Datei ohne Passwort da und blieb so. Auf dem Ausweich-Port ohne Antwort: nach 9,3 s von
  selbst zurueck auf 52734. Auf dem Ausweich-Port mit laufenden Antworten (22 Stueck ueber 15 s): bleibt dort.
  Die 14 Auswertungs-Faelle erneut alle richtig.

  **Zweite Runde** (drei Pruefer nur auf die Korrekturen und den Gesamtablauf): 7 Meldungen, 3 bestaetigt,
  4 widerlegt. Behoben:
  3. **Erreichbare, aber noch nicht verbundene Instanz wurde nicht uebernommen.** War der eigene Port tot und
     antwortete auf dem anderen eine Instanz mit `act=0`, blieb die Mod auf dem toten Port — und weil „der andere
     Port antwortet“, kam auch das Passwort nicht in die Datei. Jetzt wechselt die Mod in dem Fall dorthin.
  4. **Pendeln im 2-s-Takt.** Ein Port, der den Status mit `act=1` beantwortet, aber keine brauchbaren
     Positions-Antworten liefert, setzte den Such-Abstand immer wieder auf 2 s zurueck. `HandleProbe` setzt den
     Abstand nicht mehr zurueck (das macht `ProbeTick`, sobald der Strom laeuft). In diesem Sonderzustand wechselt
     die Mod noch hoechstens alle 10 s kurz hin und her — hingenommen.
  5. **Dauer-Suche bei „TeamSpeak offen, aber nicht auf dem Conan-Server“.** Dann sagt jede Antwort `act=0`,
     und die Mod haette fuer immer alle 10 s vergeblich den Ausweich-Port gefragt (360 Warnzeilen pro Stunde im
     Spiel-Log). Jetzt wird die Suche in dem Fall bis auf alle 60 s gestreckt.

  **Dritte Runde** (zwei Pruefer nur auf die Punkte 3 bis 5): 4 Meldungen, 2 bestaetigt (dieselbe Luecke),
  2 widerlegt. Behoben:
  6. **Erste Frage bis zu 60 s zu spaet.** Folge von Punkt 5: Nach einer langsamen Suchphase stand die naechste
     Frage bis zu 60 s in der Zukunft. Verband sich die Instanz dann (Antworten in Ordnung) und fiel kurz darauf
     aus, wartete die Mod diese Zeit ab, bevor sie den anderen Port fragte. Jetzt wird die geplante Zeit im
     Normalbetrieb auf „spaetestens in 2 s“ gehalten. Nachgetestet: langsame Suche bis Abstand 16 s, dann 4 s
     gute Antworten, dann Stille — erste Frage 1,3 s nach der letzten Antwort, danach 2 s, 4 s.

  Nachgetestet: eigener Port antwortet 40 s lang mit `act=0` — Abstaende 2, 4, 8, 16, 32 s; Antworten hoeren
  auf — sofort eine Frage, Abstand wieder 10 s. Eigener Port still, Status-Antwort `act=0` von 42734 — die Mod
  sendet dorthin, bleibt, solange von dort Antworten kommen, und ist 11 s nach deren Ende wieder auf 52734.
  16 Auswertungs-Faelle (zwei neue) alle richtig, weiterhin 0 Laufzeitfehler.

  Widerlegt (bleibt so): Eine Antwort mit `"ok":false` loest die Suche aus (gewollt, endet mit der ersten guten
  Antwort). Ein **altes** Plugin auf 52734 neben einem neuen aktiven auf 42734 wird nicht erkannt (die Antwort
  ohne `act` gilt laut Vorgabe als in Ordnung; siehe Grenzen in Abschnitt 3). Das Passwort kommt auch dann in
  die Datei, wenn das Plugin die Datei gar nicht liest (die Mod kann das nicht wissen).

**Im DevKit nicht pruefbar** (und deshalb offen): eine **echte** Antwort des Plugins auf die Mod. Eine
Freigabe-Datei nur fuer 42734 wurde vom laufenden Editor nicht gelesen; neu gestartet wurde er dafuer nicht. Die
Testdateien (Freigabe-Datei, Test-Spielstand) sind wieder entfernt.

### 9.6 Was der Besitzer im echten Spiel pruefen muss

Der Editor hat in dieser Sitzung zweimal „geht“ gezeigt, wo das Spiel „geht nicht“ sagte (Doku 100). Deshalb
zuerst **Einzelspieler mit der lokalen pak**, dann erst Workshop:

1. Kochen. Fragt das Spiel beim Start einmal, ob die Mod `http://127.0.0.1:42734` ansprechen darf? Erlauben.
   Wichtig: Kommt die Frage fuer 52734 **nicht** noch einmal?
2. Normalfall mit laufendem TeamSpeak: Voice, Hall und Mund wie bisher. Im `ConanSandbox.log` duerfen keine
   Zeilen mit `42734` stehen, in der `Pos.txt` nie `TSPWU`.
3. TeamSpeak geschlossen, auf einem Server mit Passwort: In der `Pos.txt` steht alle 30 s fuer 3 s ` TSPWU=…`,
   im Log alle 10 s eine gescheiterte Anfrage an `42734`.
4. **Tod und Wiedereinstieg** mit laufendem TeamSpeak (der Zweig „Spieler hat keine Figur“ wurde im Editor nur
   ueber den Schaltplan und einen direkten Aufruf geprueft): danach weiterhin kein `TSPWU` in der `Pos.txt` und
   keine Zeile mit `42734` im Log.
5. **Ausweich-Port** (braucht Plugin 8.0.23 in TeamSpeak): TeamSpeak beenden, in einer PowerShell den Hauptport
   belegen, dann TeamSpeak und das Spiel starten:

   ```powershell
   $a = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 52734); $a.Start(); Read-Host "52734 belegt - Enter gibt ihn frei"; $a.Stop()
   ```

   Erwartet: Hinweis im TeamSpeak-Chat (Port belegt, Plugin nimmt 42734), im `plugin.log` der Ausweich-Port, die
   Mod folgt nach wenigen Sekunden, der Spieler wird in den Ingame-Channel verschoben. Nach Enter kehrt das
   Plugin binnen etwa 5 s auf 52734 zurueck, die Mod folgt.
6. **Kein Port frei** — der Fall „alle Ports gesperrt“ von Anfang bis Ende (braucht Plugin 8.0.23): wie Schritt 5,
   aber beide Ports belegen:

   ```powershell
   $a = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 52734); $b = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 42734); $a.Start(); $b.Start(); Read-Host "beide Ports belegt - Enter gibt sie frei"; $a.Stop(); $b.Stop()
   ```

   Erwartet: Hinweis „kein Port frei“ im Chat; die Position kommt ueber die `Pos.txt`; der Spieler wird trotzdem
   verschoben, spaetestens etwa 35 s nach dem Betreten des Servers (erstes Passwort-Fenster nach 5 s, dann alle
   30 s).

Die `Pos.txt` liegt im Profilordner des Spiels (bei Trebuchet `…\ClientProfiles\<Profil>\Pos.txt`). Mitlesen,
ob das Passwort-Feld auftaucht (Pfad anpassen):

```powershell
while ($true) { if ((Get-Content "<Pfad>\Pos.txt" -Raw) -match 'TSPWU') { "$(Get-Date -Format HH:mm:ss.fff) Passwort-Feld in der Datei" }; Start-Sleep -Milliseconds 20 }
```

### 9.7 Grenzen

- Ohne TeamSpeak fragt die Mod alle 10 s den Ausweich-Port. Das sind wenige Log-Zeilen gegenueber den
  Positions-Anfragen, die in dem Fall ohnehin alle scheitern. Mit TeamSpeak, das nicht auf dem Conan-Server ist,
  sind es hoechstens eine Frage pro Minute.
- Sonderzustand: Ein Port beantwortet die Status-Frage mit `act=1`, liefert aber keine Positions-Antworten.
  Dann wechselt die Mod hoechstens alle 10 s kurz zum anderen Port und zurueck.
- Lehnt ein Spieler die Adresse 42734 ab, bleibt fuer ihn alles wie bisher (nur Hauptport); die Datei-Variante
  greift trotzdem.
- Das Passwort steht waehrend der Fenster lesbar (URL-kodiert ist keine Verschluesselung) auf der Platte des
  Spielers. Stuerzt das Spiel genau in einem Fenster ab oder steht es dabei still (keine Runde, kein
  `End Play`), bleibt es so lange bzw. bis zum naechsten Spielstart in der Datei.
- Unter 1 Bild pro Sekunde gilt jede Runde als „nach einer Pause“: Dann wird weder gesucht noch das Passwort
  in die Datei geschrieben.
- Sonderfall ohne Loesung: fremdes Programm auf 52734 **und** eine unverbundene zweite Instanz auf 42734
  **und** eine aktive dritte ohne Port. Dann antwortet auf 42734 ein Plugin, HTTP gilt als vorhanden, und das
  Passwort kommt nicht in die Datei.
- Ob das Spiel die Erfolgs- oder die Fehler-Antwort ruft, wenn das Plugin mit „400“ antwortet, ist nicht
  bekannt. Beides ist abgedeckt (eine Antwort mit `"ok":false` zaehlt als „Plugin da, aber nicht brauchbar“).
