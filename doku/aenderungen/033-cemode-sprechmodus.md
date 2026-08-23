# 033 — CEMODE: Sprechmodus an die anderen Clients melden

**Phase:** V8.14 · **Version:** 8.0.4

## Ziel

Ein Mitspieler soll sehen koennen, **ob** jemand fluestert, normal spricht oder
ruft — nicht nur, wie weit dessen Stimme traegt. Dafuer gibt es ein neues
Plugin-Kommando `CEMODE:`, das **nur beim Moduswechsel** verschickt wird.

Angezeigt wird es im **TeamSpeak-Info-Panel** eines Clients (dort, wo schon die
Plugin-Version steht):

```text
Conan Exiles Proximity Voice 8.0.4 (aktuell)
Sprechmodus: Shout (60 m)
```

## Wie war es vorher?

Ueber CEPOS ging bisher nur `voiceDistance` (Meter) raus. Der Empfaenger konnte
daraus **nicht** zurueckrechnen, welcher Modus dahinter steckt: Zonen-Overrides,
Rassen-Boni und Hub-Clamps koennen aus „Normal“ und „Rufen“ die gleiche
Meterzahl machen. Die Info „welcher Modus“ existierte also nur lokal
(`voice_mode_get_current()` fuer das eigene Overlay) und wurde nie uebertragen.

## Warum ist die neue Loesung besser?

1. **CEPOS bleibt unangetastet.** Ein zusaetzliches Feld im 56-Byte-Paket haette
   alle alten Clients ausgesperrt: die pruefen `decoded != sizeof(CeposPacket)`
   und wuerden dann **jede** Position verwerfen — niemand haette den anderen
   mehr gehoert. Ein eigenes Praefix kostet ein paar Bytes und bricht nichts.
2. **Flanke statt Dauerfeuer.** Gesendet wird nur, wenn der Modus wirklich
   wechselt (Hotkey/UI), nicht 20× pro Sekunde wie Positionen.
3. **Audio bleibt unberuehrt.** Die Lautstaerkeberechnung nutzt weiterhin
   ausschliesslich `voiceDistance` aus CEPOS. Faellt CEMODE aus (alter Client,
   Paketverlust), klingt alles exakt wie vorher — nur die Anzeige fehlt.
4. **Kein Locale-Fallstrick.** Die Distanz geht als **Ganzzahl in Dezimetern**
   ueber die Leitung. Eine Kommazahl wuerde die C-Laufzeit je nach
   Laendereinstellung als `60.0` oder `60,0` schreiben — zwei Clients mit
   unterschiedlicher Windows-Sprache haetten sich nicht verstanden.

## Wie funktioniert es jetzt?

Format: `CEMODE:1;<mode>;<distanzDm>`, z. B. `CEMODE:1;2;600`
(Version 1, Modus 2 = Shout, 60,0 m). Details: `doku/module/ce-protokoll.md`.

```text
 Hotkey/UI (fremder Thread)
   voice_mode_apply()
     └─ Hook mode_changed  ──►  ts3_cemode_signal_send_pending()
                                  └─ Flag setzen + ts3_request_wakeup()
                                                    │  (keine TS-API!)
 Wakeup-Thread ──► sendPluginCommand("CEDRAIN:1")   │
                                                    ▼
 Callback-Thread: CEDRAIN-Dispatcher
   ... → CEPOS → ts3_cemode_flush() → sendPluginCommand("CEMODE:1;2;600")
                                                    │
                            TS-Server relayed       ▼
 Callback-Thread beim Peer: ts3_cemode_on_plugin_command()
   → pruefen (Praefix, Plugin-ID, Version, Wertebereich, Client-ID-Grenzen)
   → g_peers[clientID] = { mode, distance }
   → Erstkontakt? einmalig eigenen Modus zurueckmelden (koalesziert)
                                                    │
 TS oeffnet Info-Panel: ts3plugin_infoData() ───────┘
   → ts3_cemode_format_peer() haengt „Sprechmodus: …“ an
```

**Erstkontakt-Regel:** Wer ein CEMODE von einem bisher unbekannten Client
bekommt, antwortet **genau einmal** mit dem eigenen Modus (gleiches Muster wie
der Versionsaustausch `CEVER:`). Ohne das wuesste ein spaeter beigetretener
Spieler bis zum naechsten Moduswechsel nichts. Die Antwort laeuft ueber dasselbe
Pending-Flag, deshalb kosten N gleichzeitig beitretende Peers **einen**
Broadcast, nicht N.

**Thread-Vertrag:** `ts3_cemode_signal_send_pending()` darf von jedem Thread
kommen (nur Interlocked-Flag + Wakeup). **Senden und Empfangen** laufen
ausschliesslich auf dem Callback-Thread, weil sie die TS-API anfassen.

Die **Peer-Tabelle hat einen eigenen Lock** (`CRITICAL_SECTION`, einmalig
angelegt ueber `InitOnceExecuteOnce`). Grund: geschrieben wird sie zwar nur vom
Callback-Thread, **gelesen** wird sie aber vom Info-Panel — und TeamSpeak darf
`ts3plugin_infoData()` von seinem UI-Thread aufrufen. Genau dieselbe Annahme
trifft schon der Versions-Austausch (`ts3_plugin_version.c`). Der Lock wird nur
fuer das Kopieren des Structs gehalten, formatiert wird ausserhalb — so kann
die Anzeige den Audio-/Netzwerkpfad nie ausbremsen.

Anfaenger-Bild: Der Callback-Thread schreibt einen Zettel, der UI-Thread liest
ihn. Ohne Lock koennte der Leser einen halb beschriebenen Zettel erwischen
(Modus schon neu, Distanz noch alt). Der Lock sorgt dafuer, dass immer ein
vollstaendiger Zettel gelesen wird.

## Code

- **Neu:** `src/ts/proximity/ts3_cemode.{h,c}` — Senden, Empfangen,
  Peer-Tabelle (`TS3_MAX_CLIENT_ID` Slots, Bounds-Check ueber
  `ts3_client_id_valid()`, eigener Lock), Anzeige-Formatter
- **Neu:** `src/ts/proximity/ts3_cemode_wire.h` — der reine Codec
  (Meter→Dezimeter, Format, strenges Parsen). Header-only und frei von Win32/
  TS-SDK, damit ihn der Host-Unit-Test direkt pruefen kann. Gleiches Muster wie
  `wakeup_policy.h` und `ts3_cmd_ring.h`
- **Neu:** `tests/cemode_wire_test.c` + Eintrag in `tests/run_tests.sh`
- `core/voice/voice_modes.h|c`: neuer Hook `mode_changed` in `VoiceModeHooks`,
  aufgerufen am Ende von `voice_mode_apply()` (nur bei echtem Wechsel — der
  Early-Out oben filtert wiederholte Tastendruecke)
- `ts/entry/ts3_entry.c`: Hook `voice_hooks_mode_changed` verdrahtet;
  Praefix-Dispatch vor CEPOS; CEDRAIN-Schritt nach `cepos_flush()`;
  `ts3_cemode_send_pending()` in `ts3_pending_work_any()`; Broadcast bei
  Connect/Tab-Wechsel; `ts3_cemode_clear_client()` beim Verlassen;
  `ts3_cemode_reset()` in Disconnect- und Shutdown-Reset
- `ts/entry/ts3_info.c`: haengt die Peer-Zeile ans Info-Panel
- `project/Conan-Exiles-TeamSpeak.vcxproj` + `build/build_mingw.sh`:
  neue Quelldatei eingetragen
- Doku: `doku/module/ce-protokoll.md` (neu), `doku/01-architektur-v8.md`
  (Drain-Reihenfolge, Pending-Liste, Reset-Liste)

## Tests

- **Build:** `build.ps1 -SkipDeploy` → Release x64 OK (`ts3_cemode.c` uebersetzt,
  Link OK).
- **Unit-Test Codec:** `tests/cemode_wire_test.c` — 47 Pruefungen, alle gruen:
  Meter→Dezimeter inkl. NaN/Unendlich/Saettigung, Round-Trip aller drei Modi,
  exakter Wire-Text, Ablehnung von Modus 3/-1, Distanz ausserhalb 0..10000,
  kaputten Nutzlasten (`""`, `1;2`, `1;2;`, `1;x;100`, `1;2;100x`), unbekannter
  Nutzlast-Version (0 und 2), NULL-Zeigern und zu kleinen Puffern; dazu die
  additive Zusatzfeld-Vertraeglichkeit (`1;2;600;7`).
  Hinweis: `tests/run_tests.sh` braucht gcc/Bash und laeuft auf diesem Rechner
  nicht (kein WSL, kein Git-Bash). Der Test wurde deshalb direkt mit MSVC
  uebersetzt und ausgefuehrt — der Codec ist reines C ohne Win32/TS-Bezug,
  darum ist das gleichwertig:
  `cl /W4 /I. /Isrc tests\cemode_wire_test.c` (keine Warnungen).
- **Unit-Test Voice-Modes:** `tests/voice_modes_test.c` setzt die Hooks per
  `memset` auf 0, der neue Hook ist dort also NULL → No-Op; kein Testumbau
  noetig.
- **Manuell im TS-Client (noch offen):**
  1. Zwei Clients mit dem neuen DLL, gleicher Server/Ingame-Kanal.
  2. A drueckt den Whisper-/Shout-Hotkey → bei B im Client-Info-Panel von A
     steht sofort der neue Sprechmodus (nicht erst nach dem Keepalive).
  3. A bewegt sich in Conan → B hoert weiterhin distanzabhaengig (CEPOS
     unveraendert).
  4. Ein Client mit altem DLL: liefert kein CEMODE, zeigt keine Zeile — und
     Positionen/Audio funktionieren unveraendert weiter.

## Lerneffekt

Wenn ein bestehendes Netzwerkpaket „nur ein Feld mehr“ braucht, ist das fast nie
die richtige Loesung: Das Paketformat ist ein Versprechen an alle, die es schon
benutzen. Ein **neuer, klar abgegrenzter Kanal daneben** laesst alte und neue
Teilnehmer gleichzeitig weiterarbeiten. Und Zusatzinfos duerfen nie zur
Voraussetzung fuer die Kernfunktion werden — faellt CEMODE aus, klingt das
Plugin exakt wie vorher.
