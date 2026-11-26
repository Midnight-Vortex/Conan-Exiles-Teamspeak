# 108 — Mod: kein Anfrage-Stau, kuerzere Zeitgrenze, `Pos.txt` nur noch bei HTTP-Ausfall

**Stand 2026-11-26.** Geaendert ist nur die Mod (`Vo_PosStreamer_Actor`, Funktion `UpdatePosition`) im DevKit,
gespeichert, **nicht gekocht, nicht hochgeladen**. Am Plugin wurde hier nichts geaendert. Das Gegenstueck im Plugin ist [Doku 107](107-pos-txt-nur-wenn-http-ausfaellt.md)
(8.0.26): Es fasst Pos.txt nur noch an, wenn 2 s lang keine HTTP-Position kam. Beide Teile funktionieren auch einzeln. Sicherung der alten Fassung:
`Documents\Codex\CountEmporiumDragons-Backup-20260927\teamspeak-20261126-nopileup`.

**Begriffe:** *Runde* = ein Durchlauf der Mod, 33-mal pro Sekunde. *Anfrage* = die Positions-Anfrage an das Plugin
(`POST /v1/position`). *Antwort* = die Rueckmeldung des Plugins.

## 1. Was wurde geaendert?

| Aenderung | vorher | jetzt |
|---|---|---|
| **1. Kein Stau** | Jede Runde startete eine neue Anfrage, auch wenn die letzte noch keine Antwort hatte. Im Test (Doku 106) waren so bis zu 69 Anfragen gleichzeitig offen. | Solange die letzte Anfrage noch offen ist (hoechstens 1 s), wird keine neue gestartet; die Runde macht direkt mit der Port-Suche (`ProbeTick`) weiter. Die Variable `HttpBusy` gab es schon, sie wurde nur nie abgefragt. Neu ist die Variable `HttpSentAt` (wann die letzte Anfrage losging) als Sicherung, falls einmal keine Antwort und kein Fehler zurueckkommt. |
| **2. Zeitgrenze** | 2,0 s | 0,5 s |
| **3. `Pos.txt`** | 33-mal pro Sekunde geschrieben, auch wenn HTTP funktioniert. | Nur noch, wenn seit mehr als **1 s** keine Plugin-Antwort kam **oder** gerade ein Passwort-Fenster offen ist (Doku 102, Abschnitt 9.4). |

## 2. Warum ist das besser?

- **Kein wachsender Stau.** Haengt das Plugin kurz, kostet das hoechstens eine uebersprungene Position statt einer
  Schlange von Anfragen, die alle nacheinander ablaufen.
- **Weniger Arbeit fuer das Spiel.** Im Normalbetrieb schreibt die Mod die Datei gar nicht mehr; vorher 33
  Schreibvorgaenge pro Sekunde auf die Platte.
- **Weniger Log.** Haengende Anfragen erzeugten je eine Warnzeile im Spiel-Log.
- **Der Datei-Ersatzweg bleibt.** Faellt HTTP aus, schreibt die Mod nach 1 s wieder in die Datei. Das Plugin haelt
  die letzte HTTP-Position 2 s fest und gilt sie danach noch 15 s als gueltig (`src/core/mod_file/pos_file.c`) —
  bis dahin ist die Datei laengst wieder frisch. Das Passwort-Fenster (erst nach 5 s Stille) ist nicht betroffen.

Die Ruckler vom 25.11. behebt das nicht: Doku 106 hat gezeigt, dass das Spiel nicht auf die Antwort wartet.

## 3. Wie funktioniert es?

```
jede Runde:
   Position bauen
   FilePwTick                         (unveraendert: darf das Passwort in die Datei?)
   kam seit ueber 1 s keine Antwort  ODER  Passwort-Fenster offen?
        ja   -> Pos.txt schreiben (wie bisher, mit oder ohne Passwort)
        nein -> nicht schreiben
   ist die letzte Anfrage noch offen (HttpBusy) und juenger als 1 s?
        ja   -> keine neue Anfrage
        nein -> Anfrage starten (Zeitgrenze 0,5 s), HttpBusy = wahr, HttpSentAt = jetzt
   ProbeTick                          (unveraendert: anderen Port fragen?)

Antwort oder Fehler der Anfrage -> HttpBusy = falsch   (unveraendert)
```

## 4. Wie wurde es getestet?

Im DevKit, Spiel im Editor (Server + Client). Dort lehnt das Spiel jede Anfrage sofort ab (keine Freigabe fuer den
Port) — das ist der Fall „HTTP antwortet nicht“. Den Fall „HTTP antwortet“ wurde nachgestellt, indem der Mod
direkt Antworten uebergeben wurden (`HandleReply`). `Pos.txt` wurde von aussen beobachtet (Schreibvorgaenge pro
Sekunde; der Editor lief im Hintergrund mit etwa 3 Bildern pro Sekunde):

| Abschnitt | Schreibvorgaenge pro Sekunde |
|---|---|
| keine Antworten (6 s) | 3 (jede Runde) |
| alle 0,4 s eine Antwort (10 s) | **0** |
| Antworten bleiben aus | nach etwa 1 s wieder 3 bis 4 |

Im Spiel-Log: eine Anfrage pro Runde (sie werden sofort abgelehnt, die Sperre loest sich also sofort), keine
Laufzeitfehler des Blueprints.

**Nicht geprueft** (braucht das echte Spiel mit gekochter Mod): die Stau-Sperre und die 0,5-s-Zeitgrenze bei einem
Plugin, das wirklich spaet antwortet. Dafuer nach dem Kochen:

1. TeamSpeak beenden, `powershell -ExecutionPolicy Bypass -File tests\slow_reply_standin.ps1 -DelayMs 300 -EveryMs 3000`
2. Spielen, 2 bis 3 Minuten.
3. `python tests\modhttp_timing.py "<Profil>\Logs\ConanSandbox.log"` — erwartet: „most requests open at the same
   time: 1“, Zeitgrenze `0.5s`.
4. Danach mit echtem TeamSpeak: Stimmen, Hall, Mund wie bisher; `Pos.txt` aendert sich im Normalbetrieb nicht mehr.

## 5. Absturz des DevKit beim Bauen

Beim Bauen ist der DevKit dreimal abgestuerzt (Zugriffsfehler in `BlueprintEditorLibrary` beim Einfuegen eines
Knotens per Skript). Jedes Mal war es der **zweite** Skript-Aufruf einer DevKit-Sitzung, der neue Funktionsknoten
einfuegt; der erste ging immer gut, Wertaenderungen und Speichern auch. Abhilfe hier: alle neuen Knoten in einem
einzigen Aufruf, danach nur noch Werte setzen und speichern. Die Ursache steckt im kompilierten Editor-Code und
laesst sich von hier aus nicht beheben. Verloren ging nichts, gespeichert wurde erst nach dem fertigen Umbau.
