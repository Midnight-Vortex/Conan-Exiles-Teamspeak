# Conan Exiles TeamSpeak Plugin — Rewrite-Plan **Version 8**

**Stand:** 2026-07-23 · **Basis:** V7 (7.0.3, Branch `tsmain`) · **Ziel-Version:** 8.0.0
**Workflow:** `AGENTS.md` + `.cursor/rules/vibecoding-cost-efficient.mdc` (Golden Rule, Subagent-Routing)
**Neu ab V8:** Doku-Pflicht — `.cursor/rules/05-documentation/doku-pflicht.mdc` + Ordner `doku/`

---

## Warum ein erneuter Rewrite?

V7 hat das Plugin funktionsfaehig gemacht (Phasen 0–14, Skalierung 1–5, Realistic Audio 6–9).
Aber: 13+ Bugbot-Runden und viele Hotfixes haben gezeigt, wo die Architektur **doch noch**
wackelt. Ein Audit (2026-07-23, vier parallele Research-Agents) hat die Schwachstellen
systematisch erfasst — siehe `doku/02-lessons-learned-v7.md` fuer die Langfassung.

### Die 6 Kernprobleme von V7 (Audit-Ergebnis)

| # | Problem | Wo | Risiko |
|---|---------|-----|--------|
| 1 | **Wakeup verletzt den eigenen Thread-Vertrag:** `sendPluginCommand("CEDRAIN:1")` wird von JEDEM Thread gerufen (Watcher, UI, Settings). Stabilitaet haengt allein an der SDK-Thread-Sicherheit dieser einen Funktion. | `ts3_adapter.c` | Crash-Klasse, die der V7-Rewrite eigentlich ausschliessen sollte |
| 2 | **Flag-Labyrinth statt Command-Queue:** Die typisierte Queue hat nur noch 1 Kommando; echte Arbeit laeuft ueber ~10 Pending-Flags + einen Mega-Drain (CEDRAIN macht Queue, Chat, CEPOS, 3D, Recompute, Profil, Channel, Unmute in EINEM Event). Early-Out-Prüfung ist unvollstaendig (`chan_has_pending_work` ignoriert `g_positionUpdatePending`). | `ts3_adapter.c`, `ts3_entry.c` | Callback-Spikes, verlorene Ticks, schwer wartbar |
| 3 | **PCM-Race:** `g_renderGain`/`g_renderPanL/R` schreiben Audio-Thread UND Callback-Thread (Reset/Invalidate) ohne Atomics. Cave-Slots koennen mitten im Buffer neu vergeben werden. | `ts3_proximity_audio.c` | Data-Race, Knackser, potenzieller Crash bei Disconnect waehrend PCM |
| 4 | **Mehrfach-Besitzer pro Zustand:** 2 Config-Writer (`config.c` + `config_files.c` auf dieselbe plugin.cfg), 2 Hotkey-Poller (`key_watcher` + Pos-Watcher-Tick), doppelte Kanal-IDs, `g_ts3Functions`-Duplikat, Legacy-Globals-Beutel (`plugin.h`) quer durch alle Threads ohne Sync. | core/ + ui/ | Races, inkonsistente Saves, "wer gewinnt?"-Bugs |
| 5 | **Schichten-Verletzung + Legacy-Ballast:** `core/` included `ts/` und `ui/`; `validation.c`/`util_base.c`/`proximity_volume.c`/`config_files.c` haengen am Mumble-Erbe (`plugin.h`, `mumble_compat.h`, `plugin_internal.h`); 4 tote 512er-Arrays; `ui_settings.c` gar nicht im Build; `ui_main.c` = 4195 Zeilen. | src/ gesamt | Unwartbar, Aenderungen haben unabsehbare Fernwirkung |
| 6 | **Kein Sicherheitsnetz:** 1 Unit-Test (nur auf dem Windows-PC baubar), committete `.obj`/`.exe`, Build nur via MSVC auf einem Rechner. Kein automatisches "Build OK"-Gate. | tests/, build/ | Jede Aenderung ist ein Blindflug |

---

## V8-Leitziele (was V8 anders/besser macht)

1. **Thread-Vertrag ohne Ausnahme:** KEIN TS-API-Call von fremden Threads — auch kein
   Wakeup-`sendPluginCommand`. Wakeup wird neu gebaut (Callback-eigener Tick/Timer-Pfad).
2. **Eine echte Command-Queue als einziger Steuerkanal:** Pending-Flags werden zu typisierten
   Kommandos; CEDRAIN wird ein schlanker Dispatcher mit Arbeits-Budget pro Durchlauf.
3. **PCM besitzt seinen Zustand exklusiv:** Render-Ramps nur vom Audio-Thread beschrieben;
   Invalidierung ueber Generation-Counter (PCM erkennt "Snapshot veraltet" selbst).
4. **Ein Besitzer pro Zustand:** genau EIN Config-Writer, EIN Hotkey-Poller, EINE
   Kanal-ID-Quelle, EINE Verbindungs-Epoche (atomar).
5. **Saubere Schichten:** `core/` = pur (kein Win32 wo vermeidbar, nie `ts/`/`ui/`-Includes),
   `ts/` = TS-API nur Callback-Thread, `ui/` = eigene Threads, kommuniziert nur ueber Queue.
6. **Testbarkeit als Fundament:** Pure Module laufen als gcc-Unit-Tests auf jedem Rechner;
   der komplette DLL-Build laeuft zusaetzlich via MinGW-Cross-Build auf Linux → jedes
   Arbeitspaket hat ein maschinelles "Build OK + Tests gruen"-Gate, BEVOR der TS-Client-Test kommt.
7. **Legacy-Abbau:** `plugin.h`-Globals-Beutel, `mumble_compat.h`, tote Arrays, Stubs und
   Duplikat-Funktionen werden schrittweise entfernt (messbar: Zeilen/Globals-Zaehler sinkt).
8. **Shutdown zuerst (wie V7 geplant, jetzt konsequent):** alle Threads joinbar, feste
   Reihenfolge, nach Adapter-Shutdown kein API-Call mehr moeglich.
9. **Doku-Pflicht:** jede Aenderung wird in `doku/` erklaert (was/warum/wie, anfaengertauglich).

**Golden Rule bleibt unveraendert:** eine Funktion pro Arbeitspaket, nichts extra.
**Rewrite heisst rewrite:** V7-Code ist Referenz, wird verstanden und neu geschrieben, nicht blind kopiert.

---

## Phasen-Uebersicht V8

Jede Phase endet mit: **gcc-Tests gruen + MinGW-Cross-Build OK** → Commit → (wo noetig) TS-Client-Test durch den Menschen.

| Phase | Inhalt | Verifikation | TS-Client noetig? | Status |
|-------|--------|--------------|-------------------|--------|
| **V8.0** | Fundament: committete Binaries raus, `.gitignore` haerten, `compat_crt.h` (portable `_s`-Shims), gcc-Test-Runner, MinGW-Cross-Build-Skript | Tests laufen, DLL baut auf Linux | Nein | ✅ 2026-07-23 (`doku/aenderungen/001`) |
| **V8.1** | Pure-Core absichern: Unit-Tests fuer `hub_parser`, `zone_resolve`, `proximity_math`, `player_table` (portabel machen) | gcc-Tests | Nein | ✅ 2026-07-23 (4 Suiten, 144 Checks) |
| **V8.2** | Sofort-Stabilisierung (kleine risikoarme Fixes): Unmute-Batch-Caps angleichen, `chan_has_pending_work` vervollstaendigen, doppeltes `createPresetsCategory()` raus, 4 tote 512er-Arrays + Stub-Funktionen entfernen | Cross-Build + Review | Kurztest | ✅ 2026-07-23 (`doku/aenderungen/002`) |
| **V8.3** | Thread-Kern I: PCM-Besitz (Generation-Counter statt Cross-Thread-Reset), EIN Hotkey-Poller, UI-Recompute nur noch Flag+Wakeup (nie `recompute_all` vom UI-Thread) | Cross-Build + Tests | Ja | ✅ gebaut 2026-07-23 (`doku/aenderungen/003`+`004`) — **TS-Client-Test offen** |
| **V8.4** | Thread-Kern II: Zwei-Kanal-Steuerplane (koaleszierende Flags via `ts3_pending_work_any` + typisierte Command-Queue fuer diskrete Aktionen), Wakeup-Neubau ohne Off-Thread-API, CEDRAIN-Dispatcher mit Budget, `ts3d_apply` von `cepos_send_pending` entkoppelt, Ring in `ts3_cmd_ring.h` host-getestet | Cross-Build + Tests | Ja (Lasttest) | ✅ 2026-07-23 (`doku/aenderungen/010`+`011`+`012`+`020`) — **TS-Client-Lasttest offen** |
| **V8.5** | Ein-Besitzer-Zustand: Config-Single-Writer (F10 → `g_config`, `config_files.c` stirbt), Verbindungs-Epoche atomar, Kanal-ID-Quelle vereinheitlichen | Cross-Build + Tests | Ja | ✅ 2026-07-23: ID atomar (`013`), Config-Single-Writer (`018`/`019`), Kanal-ID single source (`023`) |
| **V8.6** | Schichten-Sanierung: channel_manage/nick → `ts/`, voice_modes Hooks, Layering-Wache, validation reduziert, Legacy-Blob (`config_files`, `util_base`) allowlisted | Cross-Build + Tests | Kurztest | ✅ 2026-07-23 (`015`–`017`, `021`–`022`) |
| **V8.7** | UI-Rewrite: `ui_main.c` in 6 Dateien gesplittet (~4197 → ~750 Zeilen Kern) | Cross-Build | Ja | ✅ 2026-07-23 (`024`) |
| **V8.8** | Shutdown-Haertung + CEPOS-Load-Harness (host, 200+600 Spieler-Sim) | Tests + Harness | Ja (30 min TS) | ✅ Shutdown (`014`); Load-Sim (`025`) — **TS-Lasttest manuell offen** |
| **V8.9** | Doku, `plugin.h`-Restabbau, Version **8.0.0** | Alles gruen | Abnahme | ✅ 2026-07-23 (`026`) |

**V8 Code-Rewrite: ABGESCHLOSSEN** (9 Host-Test-Suiten + CI + MinGW-Cross-Build gruen).
**V8.10/V8.11 Legacy-Abbau: ABGESCHLOSSEN** (`029`, `030`) — `core/` ohne Allowlist.
Offen: **manueller TS-Client-Hoertest** und **30-min-Lasttest** auf echtem Server.
**V8.12 (geplant):** `plugin.h`-Globals → vollstaendige `g_config`-F10-Migration.

---

## Agent-Routing pro Phase (gemaess vibecoding-Regel)

| Phase | Effort | Subagent | Modell (Regel) | Anmerkung |
|-------|--------|----------|----------------|-----------|
| Audit/Research | S1 | `explore` (readonly) | `composer-2.5-fast` | vor JEDEM Paket ab S2 |
| V8.0–V8.2 | S2 | `generalPurpose` | `composer-2.5-fast` | kleine, klar umrissene Pakete |
| V8.3–V8.5 | S3 | `generalPurpose` | `claude-sonnet-5-thinking-high`* | Audio/Threading |
| V8.4 Architektur, V8.6 | S4 | `generalPurpose` | `claude-opus-4-8-thinking-high` | Queue-Neubau, Schichten |
| Review nach jeder Phase | S4 | `bugbot` (readonly) | — | vor dem Merge |

\* Ist `claude-sonnet-5-thinking-high` in der Umgebung nicht verfuegbar, wird das
Vererbungs-Modell (`inherit`) genutzt und das im Verlauf vermerkt — kein stilles Substituieren.

**Jeder Task-Prompt enthaelt** (unveraendert aus V7-Workflow):

```
MANDATORY: follow .cursor/rules/vibecoding-cost-efficient.mdc
Golden Rule: only [one function/package], nothing extra, honor thread contract
Rewrite: read V7 reference — rewrite, don't copy
Research: [files, thread, callers, risks]
Scope: only [files] — [functions]
Off-limits: [list]
Doku: doku/aenderungen/<nr>-<name>.md mitliefern (Pflicht, s. doku-pflicht.mdc)
Build: gcc tests + mingw cross build
Output: changed files + test evidence + manual TS test steps
```

---

## Definition of Done (pro Arbeitspaket, V8)

- [ ] Nur die beauftragte Funktion/das Paket geaendert (Golden Rule)
- [ ] Thread-Vertrag eingehalten (kein TS-API ausser Callback; PCM lock-frei; Bounds-Checks)
- [ ] gcc-Unit-Tests gruen (`tests/run_tests.sh`)
- [ ] MinGW-Cross-Build OK (`build/build_mingw.sh`)
- [ ] **Doku-Eintrag in `doku/aenderungen/` geschrieben** (was/warum/wie/Test/Lerneffekt)
- [ ] Manueller TS-Test beschrieben (wenn hoerbar/sichtbar relevant)
- [ ] Kein Scope-Creep in Plan-Dateien ohne Auftrag

---

## Abgrenzung (nicht im V8-Scope)

- Keine neuen Features (Realistic Audio, Zonen, Voice-Modes bleiben funktional wie 7.0.3)
- Kein Server-Tuning, keine TS-Client-Bugs, keine Conan-Mod-Aenderungen
- CEPOS-Wire-Protokoll bleibt kompatibel (Mischbetrieb V7/V8 auf demselben Server moeglich)
- `ui_settings.c` wird NICHT reaktiviert (Entscheidung V7 bleibt: `showConfigInterface()`)
