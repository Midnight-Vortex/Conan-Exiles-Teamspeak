# 014 — Shutdown-Haertung: jeder Plugin-Thread joinbar, feste Reihenfolge (V8.8)

**Datum:** 2026-07-23 · **Phase:** V8.8 (Shutdown-Haertung)

## Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ui/input/key_watcher.c` | Key-Monitor- und Settings-Dialog-Thread auf `_beginthreadex` umgestellt (joinbare Handles), Stop-Flag atomar, neue Funktion `settings_dialog_shutdown()` (WM_CLOSE + Join) |
| `src/plugin.h` | `keyMonitorThreadRunning`: `BOOL` → `volatile LONG` (nur Interlocked-Zugriff) |
| `src/ui/plugin_ui_compat.c` / `.h` | `overlay_stop()` aufgeteilt: stoppt+joint nur noch den Monitor-Thread und postet WM_QUIT; neues `overlay_finalize()` loescht `overlayTextLock` erst NACH dem Join des Overlay-Threads |
| `src/ts/entry/ts3_entry.c` | Overlay-UI-Thread-Handle wird behalten (`g_overlayThread`) und beim Shutdown gejoint (`overlay_join_ui_thread()`); der Overlay-Thread zerstoert sein Fenster jetzt selbst; `ts3plugin_shutdown()` mit nummerierter, dokumentierter Reihenfolge |
| `src/plugin_modules.h` | Deklaration `settings_dialog_shutdown()` |
| `doku/01-architektur-v8.md` | Shutdown-Abschnitt von "Soll" auf "Ist" aktualisiert |

**Gerade gelassen (war schon korrekt):** `pos_watcher_stop()` in
`src/core/mod_file/pos_file.c` signalisiert bereits ein Stop-Event, **joint**
den Watcher-Thread (`WaitForSingleObject`, 10 s → dann unbegrenzt) und loescht
seinen Lock erst nach dem Join. Der Update-Callback kann nach der Rueckkehr
nicht mehr feuern, weil er ausschliesslich vom (jetzt beendeten) Watcher-Thread
gerufen wird. Kein Umbau noetig — nur die Aufruf-Position im Shutdown wurde
festgelegt (nach dem Settings-Dialog-Join, siehe unten).

## Wie war es vorher (V7 / bis V8.7)?

Vier Luecken, alle vom Typ "Thread lebt noch, waehrend sein Zustand abgebaut wird":

1. **Overlay-UI-Thread nie gejoint:** `overlay_schedule_start()` hat das
   Thread-Handle sofort mit `CloseHandle` weggeworfen. `overlay_stop()` hat
   WM_QUIT gepostet und das Fenster per Cross-Thread-`SendMessage` zerstoert —
   ob der Thread beim DLL-Unload wirklich fertig war, war Glueckssache. Ein
   Thread, der nach `FreeLibrary` noch Code der DLL ausfuehrt, ist ein
   sofortiger Crash.
2. **`keyMonitorThreadRunning` war ein nicht-atomares `BOOL`** — und schlimmer:
   der Thread kam von `_beginthread`. Dessen Handle schliesst die CRT
   automatisch beim Thread-Ende; das vorhandene
   `WaitForSingleObject`/`CloseHandle` wartete also potenziell auf ein bereits
   recyceltes Handle (Use-after-free auf Handle-Ebene). Ausserdem: Join mit
   2-Sekunden-Timeout, danach wurde das Handle geschlossen, egal ob der Thread
   noch lief.
3. **Settings-Dialog-Thread (F10)** lief per `_beginthread` ohne Handle und
   wurde beim Shutdown weder geschlossen noch gejoint. War der Dialog offen,
   pumpte sein `GetMessage`-Loop munter weiter, waehrend TeamSpeak die DLL
   entlud.
4. **`overlayTextLock` wurde geloescht**, waehrend der Overlay-Thread noch
   leben und in WM_PAINT `TryEnterCriticalSection` darauf rufen konnte
   (`DeleteCriticalSection` auf eine benutzte CS = undefiniertes Verhalten).

## Warum ist die neue Loesung besser/stabiler?

- **Join statt Hoffnung:** Nach `ts3plugin_shutdown()` existiert kein
  Plugin-Thread mehr. `WaitForSingleObject` auf ein eigenes
  `_beginthreadex`-Handle ist die einzige Win32-Garantie dafuer, dass ein
  Thread wirklich fertig ist. Erst danach darf Zustand (Locks, HWNDs, Module)
  abgebaut werden.
- **Fenster-Besitz respektiert:** Win32-Regel: ein Fenster gehoert dem Thread,
  der es erstellt hat. Der Overlay-Thread zerstoert sein HWND jetzt selbst am
  Ende seiner Message-Loop — kein Cross-Thread-`SendMessage`-Destroy mehr, das
  mit der gerade endenden Loop racen kann.
- **Robuster Quit:** `PostThreadMessage(WM_QUIT)` geht verloren, wenn der
  Thread seine Message-Queue noch nicht hat. Der Join-Loop postet das Quit
  deshalb alle 500 ms erneut, bis der Thread beendet ist.
- **Dialog-Schliessen deterministisch:** Der F10-Dialog beendet seine Loop nur
  ueber WM_CLOSE → WM_DESTROY → `PostQuitMessage`. `settings_dialog_shutdown()`
  postet WM_CLOSE (wiederholt, falls das Fenster beim ersten Versuch noch nicht
  existiert) und joint dann. Ein Join **ohne** Close wuerde ewig auf den
  Benutzer warten — deshalb gehoeren beide zusammen.

## Wie funktioniert es jetzt? Die feste Shutdown-Reihenfolge

`ts3plugin_shutdown()` (Callback-Thread) arbeitet exakt diese Liste ab:

1. **Annahme stoppen:** Audio auf Passthrough (PCM-Pfad inert),
   `pluginShuttingDown = TRUE` (UI-/Overlay-Code fasst keine HWNDs/GDI mehr
   an), verzoegerte Overlay-Starts entschaerft (`g_overlay_armed = 0`).
2. **Alle Plugin-Threads stoppen + joinen**, in Abhaengigkeits-Reihenfolge:
   1. `removeKeyMonitoring()` — Key-Monitor zuerst, denn er spawnt
      Dialog-Threads (Flag atomar auf 0, Join, Handle schliessen).
   2. `settings_dialog_shutdown()` — WM_CLOSE an den offenen F10-Dialog,
      Join des Dialog-Threads.
   3. `pos_watcher_stop()` — Stop-Event, Join, Lock-Abbau nach Join. Kommt
      NACH dem Dialog-Join, weil der Dialog-Thread `pos_get_current()` liest
      (sonst Lock-Loeschung unter einem lebenden Leser).
   4. `overlay_stop()` (Monitor-Thread joinen, WM_QUIT an Overlay-Thread) →
      `overlay_join_ui_thread()` (Join; der Overlay-Thread zerstoert sein
      HWND selbst) → `overlay_finalize()` (`overlayTextLock` loeschen —
      erst jetzt sicher).
3. **Modul-Zustand zuruecksetzen** (`player_table_clear`, `cepos_reset`,
   `ts3d_reset`, `chan_reset`, `server_profile_reset`, `nick_reset`,
   `ts3_version_reset`, `ts3_audio_reset`) — pure Resets, keine TS-API.
4. **`ts3_adapter_shutdown()`** — joint den Wakeup-Thread, loescht die
   Verbindungs-Identitaet (siehe `doku/013`), leert die Command-Queue.
   Danach ist JEDER TS-API-Aufruf ein Programmierfehler; die
   Callback-Thread-Guards loggen ihn.
5. **`log_close()` zuletzt**, damit jeder Schritt davor noch loggen kann.

```
Shutdown (Callback-Thread)
  │ 1. Passthrough + pluginShuttingDown
  │ 2a. Key-Monitor ──── join ──▶ beendet
  │ 2b. F10-Dialog  ── WM_CLOSE + join ──▶ beendet
  │ 2c. Pos-Watcher ── Event + join ──▶ beendet (Lock erst danach weg)
  │ 2d. Overlay-Monitor ─ join ─▶ beendet
  │     Overlay-UI ── WM_QUIT + join ─▶ zerstoert eigenes HWND, beendet
  │     overlayTextLock loeschen (kein Thread kann sie mehr betreten)
  │ 3. Modul-Resets (pure)
  │ 4. Adapter: Wakeup-Thread join, Identitaet + Queue leeren
  ▼ 5. log_close()
```

## Wie wurde es getestet?

- `bash tests/run_tests.sh` — alle 6 Suiten gruen (251 Checks).
- `bash build/build_mingw.sh` — DLL linkt fehlerfrei, keine neuen Warnungen.
- Reines Lifecycle-/Win32-Verhalten — kein neuer Host-Unit-Test sinnvoll
  (keine pure Logik entstanden).

**Manuelle TS-Client-Tests (durch den Menschen, Pflicht vor Abnahme):**

1. **10× Reconnect:** Verbinden → trennen, zehnmal hintereinander (auch
   schnell). Erwartung: kein Haenger, Overlay erscheint nach jedem Connect
   wieder, Proximity-Audio funktioniert.
2. **Tab-Wechsel:** Zwei Server-Tabs, waehrend jemand spricht mehrfach
   wechseln. Erwartung: kein Knacken/Einfrieren, HUD folgt dem aktiven Tab.
3. **TS beenden, waehrend Sprache laeuft:** Im Ingame-Channel bei aktivem
   Proximity-Audio (jemand spricht gerade) TeamSpeak schliessen. Erwartung:
   sauberes Beenden, kein Crash-Dialog, im Log `SHUTDOWN: plugin stopping` …
   `SHUTDOWN: done`.
4. **TS beenden bei offenem F10-Dialog:** Dialog offen lassen und TS
   schliessen. Erwartung: Dialog verschwindet, TS beendet sich sauber.

## Lerneffekt

Ein Thread ist erst dann "weg", wenn `WaitForSingleObject` auf sein Handle
zurueckkehrt — Flags, Timeouts und WM_QUIT sind nur Bitten. Und:
`_beginthread` gibt einem KEIN besitzbares Handle (die CRT schliesst es
selbst); wer joinen will, braucht `_beginthreadex`. Locks und Fenster duerfen
erst abgebaut werden, wenn alle Threads, die sie benutzen koennten,
nachweislich beendet sind.
