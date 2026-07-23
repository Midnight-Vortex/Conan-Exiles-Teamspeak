# 00 — Projekt-Ueberblick: Was macht dieses Plugin?

## Die Idee in einem Satz

Spieler von **Conan Exiles** sollen sich in **TeamSpeak 3** nur dann (und nur so laut)
hoeren, wie sie im Spiel wirklich beieinander stehen — sogenannter **Proximity-Voice-Chat**
(Naeherungs-Sprachchat).

## Wie funktioniert das grob?

```
Conan Exiles (Spiel)                TeamSpeak 3 (Sprache)
┌──────────────────┐               ┌─────────────────────────┐
│ Mod schreibt      │              │ Plugin (diese DLL)       │
│ Position in       │──Pos.txt──▶ │ 1. liest eigene Position │
│ eine Textdatei    │              │ 2. sendet sie an andere  │
└──────────────────┘               │    Clients (CEPOS-Paket) │
                                   │ 3. empfaengt fremde      │
                                   │    Positionen            │
                                   │ 4. rechnet Distanz →     │
                                   │    Lautstaerke + Richtung│
                                   │ 5. veraendert den Ton    │
                                   │    (leiser, links/rechts,│
                                   │    Hall in Hoehlen …)    │
                                   └─────────────────────────┘
```

1. **Eine Spiel-Mod** schreibt die eigene Spielerposition (x/y/z + Blickrichtung)
   mehrmals pro Sekunde in eine Datei (`Pos.txt`).
2. **Das Plugin** (eine DLL, die TeamSpeak laedt) liest diese Datei, verpackt die Position
   in ein kleines Datenpaket (**CEPOS**) und schickt es ueber TeamSpeak an alle anderen.
3. Jeder Client kennt so die Positionen aller Mitspieler und rechnet daraus fuer jeden
   Sprecher: **Wie weit weg? Also wie laut? Von links oder rechts? In einer Hoehle (Hall)?
   Hinter einer Schallschutz-Grenze (stumm)?**
4. Das Ergebnis wird direkt auf die Sprach-Samples angewendet, bevor TeamSpeak sie abspielt.

## Die wichtigsten Begriffe

| Begriff | Bedeutung |
|---|---|
| **CEPOS** | Unser Positions-Paket ("Conan Exiles POSition"), 56 Bytes, base64-kodiert, verschickt per TeamSpeak-Plugin-Command. Wire-kompatibel seit V7 — alte und neue Plugin-Versionen verstehen sich. |
| **Hub** | Ein TeamSpeak-Kanal als "Lobby". Wer nicht im Spiel ist, sitzt im Hub und redet normal. Wer spielt, wird automatisch in den Ingame-Kanal verschoben. |
| **Hub-Settings** | Der Server-Admin schreibt Einstellungen (Distanzen, Zonen, Realistic-Audio-Schalter) in die **Beschreibung des Root-Kanals**. Das Plugin liest und parst sie (`hub_parser`). |
| **Zonen** | Rechteckige Gebiete auf der Karte mit Sonderregeln: **Soundproof** (durch die Grenze hoert man nichts) oder **Reverb** (Hoehlen-Hall + dumpfer Klang). |
| **Voice-Modes** | Fluestern / Normal / Rufen — per Hotkey umschaltbar, aendert die Hoer-Distanz. |
| **Overlay (HUD)** | Kleines Fenster ueber dem Spiel, zeigt aktuellen Voice-Mode und Zone. |
| **PCM / Audio-Thread** | Der Thread, in dem TeamSpeak uns die rohen Sprach-Samples gibt (`onEditPlaybackVoiceDataEvent`). Hier wird Lautstaerke/Pan/Filter angewendet. Zeitkritisch! |
| **Callback-Thread** | Der Thread, auf dem TeamSpeak unsere Plugin-Callbacks aufruft. **Nur dieser Thread darf die TS-API benutzen** — das ist die wichtigste Stabilitaetsregel des Projekts. |

## Geschichte des Projekts

| Version | Was war das? |
|---|---|
| bis 6.x | Ursprüngliches **Mumble**-Plugin (ein Franzose hat es gebaut, Branch `main`). Ein einziges `plugin.c` mit ~10.000 Zeilen. |
| **V7** (Branch `tsmain`) | Kompletter Rewrite fuer **TeamSpeak 3**: modulare Struktur (`src/core`, `src/ts`, `src/ui`), Thread-Vertrag, Skalierung auf 200+ Spieler, Realistic Audio. Funktioniert, aber Audit fand strukturelle Schwaechen (siehe `02-lessons-learned-v7.md`). |
| **V8** (dieser Stand) | Erneuter, gezielter Rewrite: Thread-Vertrag ohne Ausnahmen, eine Command-Queue, ein Besitzer pro Zustand, Tests + Cross-Build als Sicherheitsnetz, Doku-Pflicht. Plan: `REWRITE_PLAN_V8.md`. |
