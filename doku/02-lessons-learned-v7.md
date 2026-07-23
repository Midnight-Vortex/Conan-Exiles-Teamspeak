# 02 — Lessons Learned: Was V7 uns beigebracht hat

**Quelle:** Vier parallele Audit-Agents (Threading, Core-Module, UI, Build/Tests) am
2026-07-23 auf Stand `tsmain` @ 7.0.3. Dieses Dokument ist die anfaengertaugliche
Zusammenfassung — jede Erkenntnis mit "Was war los? Warum ist das schlimm? Was lernen wir?".

---

## Lektion 1: Eine einzige Vertrags-Ausnahme kann den ganzen Vertrag entwerten

**Was war los?** V7s eiserne Regel lautete "nur der Callback-Thread ruft die TS-API".
Aber der Aufweck-Mechanismus (CEDRAIN-Wakeup) rief `sendPluginCommand("CEDRAIN:1")`
von **jedem beliebigen Thread** — Watcher, UI, Settings. Das war als "dokumentierte
Ausnahme" markiert.

**Warum ist das schlimm?** Die ganze Stabilitaet hing damit an der (nicht garantierten!)
Thread-Sicherheit genau dieser einen SDK-Funktion. Genau diese Art Annahme hat schon das
alte Mumble-Plugin crashen lassen.

**Lerneffekt:** Ein Sicherheitsvertrag mit "einer kleinen Ausnahme" ist kein Vertrag.
Wenn eine Regel eine Ausnahme braucht, ist das Design an der Stelle noch nicht fertig.

---

## Lektion 2: Wenn die offizielle Struktur unbequem ist, entsteht eine Schattenstruktur

**Was war los?** V7 hatte eine saubere typisierte Command-Queue geplant (Phase 3.2).
Am Ende steckte darin genau **ein** Kommandotyp (Kanal-Liste loggen). Die echte Arbeit
lief ueber ~10 einzelne Pending-Flags (`g_recomputeAllPending`, `g_positionUpdatePending`,
Unmute-Ring, Dirty-Arrays …), die ein Mega-Handler in `onPluginCommandEvent` abarbeitete.

**Warum ist das schlimm?**
- Die Early-Out-Pruefung (`chan_has_pending_work`) vergass ein Flag → Channel-Ticks
  konnten verschluckt werden.
- Ein einziger Wakeup konnte eine riesige Arbeitslast am Stueck ausloesen (Queue + Chat +
  CEPOS + 3D + Recompute-All + Profil + Channel + Unmute) → Callback-Spikes bei vielen Spielern.
- Niemand konnte mehr auf einen Blick sagen, was "pending" bedeutet.

**Lerneffekt:** Wenn Entwickler (oder KI-Agents) die vorgesehene Struktur umgehen, ist die
Struktur zu unbequem. Dann die Struktur reparieren — nicht weitere Flags stapeln.

---

## Lektion 3: "Lock-frei" gilt nur, wenn wirklich EINER schreibt

**Was war los?** Der PCM-Pfad war fast vorbildlich: keine Locks, Seqlock-Snapshots. Aber
die Render-Rampen (`g_renderGain`, `g_renderPanL/R`) wurden vom Audio-Thread **und**
vom Callback-Thread (bei Reset/Disconnect/Eviction) beschrieben — ohne Atomics. Auch
Cave-Reverb-Slots konnten mitten in einem laufenden Buffer neu vergeben werden.

**Warum ist das schlimm?** Das ist ein klassisches Data-Race: meistens unsichtbar, dann
ploetzlich Knacksen oder Absturz genau beim Disconnect/Tab-Wechsel — die Sorte Bug, die
man nie reproduzieren kann.

**Lerneffekt:** Lock-freies Design heisst nicht "keine Regeln", sondern **strengere**
Regeln: pro Variable genau ein Schreiber. Invalidierung loest man mit Generation-Zaehlern
(der Leser merkt selbst "veraltet"), nicht mit Reinschreiben von aussen.

---

## Lektion 4: Zwei Besitzer fuer denselben Zustand = unsichtbare Bugs

**Was war los?** Gleich mehrfach:
- **Zwei Config-Writer:** `config.c` (Rewrite) und `config_files.c` (Legacy-F10-Pfad)
  schrieben beide dieselbe `plugin.cfg`, gebrueckt ueber Sync-Funktionen.
- **Zwei Hotkey-Poller:** `key_watcher`-Thread UND Pos-Watcher-Tick riefen beide
  `voice_mode_hotkey_poll()` — unsynchronisierte Doppel-Schreiber auf dem Arming-Zustand.
- Doppelte Kanal-IDs (channel_manage + Legacy-Globals), doppelte Funktionstabelle
  (`g_ts3` + ungenutzte Kopie `g_ts3Functions`), Positions-Zustand in 6 Kopien.

**Warum ist das schlimm?** "Wer gewinnt?"-Bugs: Einstellungen, die manchmal nicht
gespeichert werden; Hotkeys, die doppelt oder gar nicht feuern. Kaum zu debuggen, weil
timing-abhaengig.

**Lerneffekt:** Fuer jeden Zustand VOR dem Coden festlegen: Wer ist der eine Besitzer?
Alle anderen sind Leser oder schicken Kommandos.

---

## Lektion 5: Schichten-Verletzungen machen Tests unmoeglich

**Was war los?** `core/` (eigentlich pure Logik) included `ts/`- und `ui/`-Header:
`voice_modes.c` → `ts/*` + `ui/overlay`; `channel_manage.c` → `ts/*`; dazu der
Legacy-Blob (`validation.c`, `util_base.c`, `proximity_volume.c`, `config_files.c`)
am Mumble-Erbe (`plugin.h` mit `#include <windows.h>` und ~100 Globals).

**Warum ist das schlimm?** Nichts davon laesst sich isoliert kompilieren oder testen.
Duplikate entstanden (3× Distanzrechnung, 2× Zonen-Logik, 2× Config-Pfad), weil niemand
die "richtige" Stelle fand.

**Lerneffekt:** Schichtregeln ("core kennt nie ts/ui") sind keine Aesthetik — sie sind
die Voraussetzung dafuer, dass man Logik ueberhaupt automatisiert testen kann.

---

## Lektion 6: Ohne maschinelles Sicherheitsnetz wird jede Aenderung zum Blindflug

**Was war los?** V7 hatte genau einen Unit-Test (hub_parser), der nur auf dem einen
Windows-PC mit installiertem Visual Studio lief (`run_test.bat` mit hartkodiertem Pfad).
Im `tests/`-Ordner lagen kompilierte `.obj`/`.exe` im Git. Der DLL-Build lief nur via
MSVC auf demselben PC. 13+ Bugbot-Runden mussten Fehler finden, die Tests haetten
finden koennen.

**Warum ist das schlimm?** Jede Aenderung (auch von KI-Agents) konnte erst "auf dem
einen PC" verifiziert werden. Commits wie `#` und `'` in der Historie zeigen, wie
muehsam das Iterieren war.

**Lerneffekt:** Erst das Sicherheitsnetz (Tests + ueberall lauffaehiger Build), dann der
Umbau. Deshalb ist V8-Phase 0 das Build-/Test-Fundament, nicht ein Feature.

---

## Lektion 7: UI waechst unkontrolliert, wenn man es laesst

**Was war los?** `ui_main.c` = 4195 Zeilen: Dialog-Erzeugung (~600 Zeilen WM_CREATE),
Owner-Draw-Painting (~450 Zeilen), Preset-Dialoge, Steam-Pfad-Erkennung, Registry-Lesen —
alles in einer Datei. Dazu: `createPresetsCategory()` wurde in WM_CREATE **doppelt**
aufgerufen (doppelte Fenster-Handles), `ui_settings.c` lag als tote Datei daneben
(nie im Build), und 4 ungenutzte 512er-Arrays belegten BSS.

**Lerneffekt:** Dateigroesse ist ein Fruehwarnsignal. Tote Dateien sofort loeschen —
"vielleicht brauchen wir es noch" kostet jeden spaeteren Leser Zeit und fuehrt zu
Verwechslungen (welcher Settings-Dialog ist echt?).

---

## Was V7 GUT gemacht hat (und V8 beibehaelt)

Fairerweise — das Fundament von V7 war richtig und bleibt:

- **PCM-Pfad ohne TS-API und ohne Locks** (Seqlock-Snapshots) — Konzept korrekt,
  nur die Besitzer-Regel wurde verletzt.
- **Client-ID-Bounds-Checks** (`ts3_client_id_valid`, 4096er sparse Arrays) — ueberall benutzt.
- **Dedup/Throttle von API-Calls** (3D-Epsilon, 20 Hz, Unmute-Batches).
- **CEPOS-Wire-Kompatibilitaet** — Mischbetrieb alter/neuer Clients funktioniert.
- **Passthrough-first bei Disconnect** — Audio wird neutral, bevor Zustand geleert wird.
- **Modulare Ordnerstruktur** core/ts/ui — die Idee stimmt, nur die Einhaltung fehlte.
- **Phasen-Vorgehen mit Golden Rule** — eine Funktion pro Paket hat den V7-Rewrite
  ueberhaupt erst moeglich gemacht.
