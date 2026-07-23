# 003 — UI-Recompute auf den Callback-Thread + genau EIN Hotkey-Poller (V8.3, Teilpaket)

Arbeitspaket aus Phase V8.3: zwei Threading-Fixes, kein Architektur-Umbau.
Ziel: Der Settings-Speichern-Pfad darf keine schwere Audio-Arbeit mehr auf dem
UI-Thread ausfuehren, und der Hotkey-Zustand hat nur noch **einen** Schreiber-Thread.

---

## Fix A — Settings speichern loest den Audio-Recompute nur noch aus, statt ihn selbst zu rechnen

### Was wurde geaendert?

- `src/ts/proximity/ts3_proximity_audio.c` / `.h`: neue kleine Funktion
  `ts3_audio_request_recompute_all()`. Sie setzt nur das schon vorhandene
  Pending-Flag (`g_recomputeAllPending`, atomar per `InterlockedExchange`) und
  ruft `ts3_request_wakeup()` — mehr nicht.
- `src/ui/plugin_ui_compat.c`: drei UI-Pfade rufen jetzt diese Request-Funktion
  statt `ts3_audio_recompute_all()` synchron:
  - `plugin_ui_on_settings_saved()` — F10-Dialog "Save" (Settings-Dialog-Thread)
  - `ts3_plugin_apply_proximity_volumes_force()` — Dialog-Apply/Presets
    (`applyDistanceToAllPlayers`, `loadVoicePreset`, alles Dialog-Threads)
  - `plugin_ui_sync_to_config()` — UI-Sync-Pfad (aktuell ohne Aufrufer, aber
    ein UI-Pfad, darum genauso umgestellt)
- **Bewusst unveraendert:** `plugin_ui_on_hub_profile_updated()` ruft weiterhin
  synchron `ts3_audio_recompute_all()`. Alle seine Aufrufer sind TS-Callback-Events
  (`onChannelDescriptionUpdateEvent`, `onUpdateChannelEvent`,
  `onUpdateChannelEditedEvent` in `ts3_entry.c`) — dort ist der synchrone
  Recompute erlaubt, weil er schon auf dem richtigen Thread laeuft.

### Wie war es vorher (V7)?

Beim Speichern der Einstellungen (F10 → Save) lief `audio_recompute_all_impl`
**direkt auf dem Settings-Dialog-Thread**. Diese Funktion iteriert ueber die
komplette Spieler-Tabelle und veroeffentlicht die Ergebnisse unter dem
`g_writerLock`. Gleichzeitig will der TS-Callback-Thread (CEDRAIN) denselben
Lock nehmen. Ergebnis unter Last: Der Callback-Thread wartet auf einen
UI-Thread — genau das, was das "Rechnen auf dem Callback-Thread"-Design
(Phase 4.3) verhindern soll.

### Warum ist die neue Loesung besser?

- Der UI-Thread macht nur noch zwei atomare Operationen (Flag + Wakeup) —
  er kann den Callback-Thread nicht mehr am `g_writerLock` blockieren.
- Der eigentliche Recompute laeuft ueber die **bereits vorhandene** Maschinerie
  aus Phase 4.1/4.2: CEDRAIN prueft `ts3_audio_has_pending_recompute()` und
  ruft `ts3_audio_flush_recomputes()` auf dem Callback-Thread.
- Einzige Verhaltensaenderung: Der Recompute passiert wenige Millisekunden
  spaeter (naechster CEDRAIN-Zyklus) statt sofort — hoerbar ist das nicht.

### Wie funktioniert es jetzt?

```
Settings-Dialog-Thread (F10 Save)          TS-Callback-Thread
────────────────────────────────           ─────────────────────────────
plugin_ui_on_settings_saved()
  └─ ts3_audio_request_recompute_all()
       ├─ g_recomputeAllPending = 1  ──┐
       └─ ts3_request_wakeup()         │   "CEDRAIN:1" Plugin-Command trifft ein
                                       │   ts3plugin_onPluginCommandEvent
                                       │     └─ ts3_audio_has_pending_recompute()? ja
                                       └──►     ts3_audio_flush_recomputes()
                                                  └─ audio_recompute_all_impl()
                                                     (rechnen ausserhalb des Locks,
                                                      publish unter g_writerLock)
```

---

## Fix B — genau EIN Hotkey-Poller (Key-Watcher-Thread ist der einzige Besitzer)

### Was wurde geaendert?

- `src/ts/entry/ts3_entry.c`: `ts3_on_watcher_tick()` (rief nur
  `voice_mode_hotkey_poll()`) und die Registrierung
  `pos_watcher_set_tick_callback(...)` entfernt. Der Pos-Watcher pollt
  **keine** Hotkeys mehr.
- `src/core/voice/voice_modes.h` / `.c`: Ein-Zeilen-Vertrag am Poller
  dokumentiert: **einziger erlaubter Aufrufer ist der Key-Watcher-Thread**
  (`keyMonitorThreadFunction` in `src/ui/input/key_watcher.c`).

### Wie war es vorher (V7)?

`voice_mode_hotkey_poll()` wurde von **zwei** Threads gleichzeitig aufgerufen:

1. Key-Watcher-Thread (`keyMonitorThreadFunction`) — alle `PLUGIN_POLL_INTERVAL_MS`
2. Pos-Watcher-Tick (`ts3_on_watcher_tick`) — ebenfalls jede Schleifenrunde

Der Entprell-Zustand (`g_keyArmed[256]`, `g_keySuppressUntil[256]`) ist aber
**nicht synchronisiert** (einfache Arrays, keine Atomics). Zwei Poller heisst:
zwei Schreiber. Je nach Timing feuerte ein Tastendruck **doppelt** (beide Poller
sehen "gedrueckt + scharf", bevor der jeweils andere entschaerft) oder ging
**verloren** (der eine entschaerft, der andere kommt zu spaet).

### Warum ist die neue Loesung besser — und warum der Key-Watcher-Thread?

Beide Kandidaten laufen zwar unconditional (der Pos-Watcher-Tick feuerte auch
ohne gueltige Pos.txt), aber der Key-Watcher-Thread ist der natuerliche Besitzer:

- Er wird in `ts3plugin_init()` → `plugin_ui_init()` → `installKeyMonitoring()`
  gestartet — **unabhaengig vom Spiel/Pos.txt** — und laeuft bis zum
  Plugin-Shutdown (`overlay_stop()` → `removeKeyMonitoring()`). Hotkeys
  funktionieren also weiterhin im Hub und bei geschlossenem Spiel.
- Er existiert genau fuer Tastatur-Arbeit (pollt auch den F10-Config-Key) und
  wird nach jedem Settings-Save neu installiert (`writeFullConfiguration`).
- Einfachste Loesung: einen Aufruf loeschen, kein Owner-Flag, kein neuer
  Mechanismus — ein Schreiber-Thread reicht, dann braucht der Zustand keine
  Synchronisation.

### Wie funktioniert es jetzt?

```
Key-Watcher-Thread (einziger Poller)        Pos-Watcher-Thread
────────────────────────────────────        ──────────────────────────
alle PLUGIN_POLL_INTERVAL_MS:               liest nur noch Pos.txt und
  ├─ F10/Config-Key pruefen                 meldet Positions-Updates —
  └─ voice_mode_hotkey_poll()               pollt KEINE Hotkeys mehr
       └─ g_keyArmed/g_keySuppressUntil
          (nur noch EIN Schreiber-Thread)
```

---

## Wie wurde es getestet?

- `bash tests/run_tests.sh` — alle 4 Suites gruen (hub_parser, proximity_math,
  zone_resolve, player_table)
- `bash build/build_mingw.sh` — `bin/mingw/conan_exiles.dll` linkt OK
- Verifiziert per Code-Suche: `ts3_audio_recompute_all()` hat nur noch einen
  Aufrufer (Callback-Thread), `voice_mode_hotkey_poll()` nur noch einen
  (Key-Watcher-Thread).

### Manuelle TS-Testschritte (bitte im echten Client pruefen)

1. **Recompute nach Save:** Mit TS verbinden, ins Spiel (Pos.txt aktiv), einen
   zweiten Spieler in Hoerweite stellen. F10 oeffnen, z. B. die Normal-Distanz
   deutlich aendern, **Save** klicken. Erwartung: Die Lautstaerke des anderen
   Spielers passt sich praktisch sofort an (Verzoegerung < 1 Sekunde), der
   TS-Client bleibt fluessig — kein Ruckeln/Einfrieren beim Speichern.
2. **Hotkeys im Spiel:** Whisper-/Normal-/Shout-Taste je einmal druecken.
   Erwartung: Pro Tastendruck **genau eine** Chat-Meldung
   "[Conan Exiles] Voice mode: …" — keine Doppel-Meldung, kein verschluckter
   Druck. Voice-Toggle-Taste mehrfach: Modus wechselt pro Druck genau einmal.
3. **Hotkeys ohne Spiel (Hub / Spiel geschlossen):** Conan Exiles beenden
   (keine gueltige Pos.txt). Hotkeys erneut druecken. Erwartung: Modus wechselt
   weiterhin zuverlaessig — der Key-Watcher-Thread laeuft unabhaengig vom Spiel.
4. **Nach Settings-Save:** Direkt nach F10-Save die Hotkeys erneut testen
   (der Key-Watcher wird beim Save neu installiert) — muessen sofort wieder
   reagieren.

## Lerneffekt

- Unsynchronisierter Zustand braucht **genau einen Schreiber-Thread**. Die
  billigste "Synchronisation" ist, den zweiten Schreiber zu loeschen — kein
  Lock, kein Atomic-Umbau noetig, wenn ein Besitzer reicht.
- Wenn es schon eine Flag+Wakeup-Maschinerie fuer den Callback-Thread gibt
  (Phase 4.1/4.2), sollen UI-Pfade sie **benutzen**, statt die schwere Arbeit
  selbst zu machen. "Nur ein Flag setzen" ist fast immer die richtige Antwort
  auf "wie bekomme ich Arbeit vom UI-Thread auf den Callback-Thread?".
