# Conan Exiles TeamSpeak Plugin — Rewrite-Plan

**Ziel:** Das Plugin von Grund auf neu aufbauen — Funktion für Funktion, langsam und stabil.
Das alte Plugin (`E:\programme\Conan-Exiles-Mumble\Conan-Exiles-TeamSpeak`) dient nur als **Vorlage/Referenz**, es wird kein Code blind kopiert.

---

## ⚠️ GOLDENE REGEL (Tipp vom Kumpel — gilt für JEDEN Schritt)

> **Immer nur die eine Funktion bauen, die gerade dran ist. NICHTS extra dazu dichten.**
>
> - Eine Funktion = ein Arbeitspaket. Erst vollständig beschreiben, dann coden, dann testen.
> - Danach klar definieren, wie die Funktionen miteinander kommunizieren (welche Daten, welche Threads).
> - Macht die KI auch nur EINE Sache, die nicht beauftragt war → verwerfen und neu machen.
> - Kein „das könnte man auch gleich mitmachen", keine vorsorglichen Helfer, keine Extra-Features.

**Jeder Schritt in diesem Plan endet mit:** Build OK → im TS-Client getestet → erst dann weiter.

---

## Lehren aus dem alten Plugin (Crash-Ursachen — was im Neubau anders sein MUSS)

Diese Regeln sind das Fundament. Sie stehen hier, damit wir sie bei jeder Funktion prüfen:

1. **Nur EIN Thread ruft die TS-API auf** (der TS-Callback-Thread).
   Alle anderen Threads (Voice-Thread, Watcher, Audio) legen nur Aufträge in eine Queue.
   → Im alten Plugin war `ts3ApiLock` (CRITICAL_SECTION) von 4+ Threads unter Dauerfeuer und ist korrumpiert (`RtlpWaitOnCriticalSection`-Crashes bei 20+ Spielern).
2. **Der Audio-Thread (PCM-Callback) ruft NIEMALS die TS-API auf und nimmt NIEMALS Locks.**
   Er liest nur lock-freie Snapshots (Seqlock/Interlocked).
3. **Logging hat einen eigenen Lock** und berührt nie den API-Pfad.
4. **Jeder API-Call wird dedupliziert/gedrosselt:** 3D-Position nur bei Änderung senden, Unmute gebatcht, Audio-Mode nur bei Wechsel.
5. **Feste Arraygrößen immer mit Bounds-Check** (alter OOB-Bug: `lastByClient[32]`).
6. **Jede Datei dokumentiert oben ihren Thread-Vertrag:** Wer ruft mich auf? Welche Locks darf ich nehmen?
7. **Shutdown zuerst:** Jedes Modul bekommt von Anfang an ein sauberes Stop/Cleanup — nicht am Ende nachgerüstet.

---

## Ordnerstruktur (identisch zum alten Plugin)

```
E:\programme\Conan-Exiles-Teamspeak\
├── project\          # .vcxproj (VS 2026, v145, x64 Release)
├── build\            # build_msvc.ps1
├── sdk\              # TeamSpeak 3 Client SDK (Kopie/Submodul wie im alten)
├── assets\           # Icons etc.
├── docs\
└── src\
    ├── plugin.h / plugin.c / plugin_internal.h / plugin_modules.h / mumble_compat.h
    ├── audio\mumble_pcm\
    ├── core\
    │   ├── channel\      # channel_manage.c
    │   ├── config\       # config_files.c
    │   ├── hub\          # hub_parser.c
    │   ├── mod_file\     # mod_watcher.c
    │   ├── proximity\    # proximity_math.c / _volume.c / _adaptive.c
    │   ├── threads\      # system_threads.c
    │   ├── util\         # util_base.c
    │   ├── validation\   # validation.c
    │   └── voice\        # voice_modes.c
    ├── lifecycle\        # cleanup.c
    ├── ts\
    │   ├── adapter\      # ts3_adapter.c/.h
    │   ├── deferred\     # ts3_deferred.c/.h
    │   ├── entry\        # ts3_entry.c / ts3_exports.h / ts3_stubs.c
    │   ├── profile\      # ts3_server_profile.c/.h
    │   └── proximity\    # ts3_proximity_apply.c / ts3_proximity_audio.c/.h
    └── ui\
        ├── dialogs\      # ui_main.c / ui_messages.c / ui_dynamic.c
        ├── input\        # key_watcher.c
        └── overlay\      # voice_overlay.c
```

---

## 📌 AKTUELLER STAND (Stand: 2026-07-07)

**Phase 0–7 sind implementiert und kompilieren fehlerfrei (Release x64).**
**Nächster Schritt: Phase 8 (Channel-Management hub ↔ ingame).**

Noch offen vor dem Weiterbauen: Phasen 0–7 im echten TS-Client testen (Plugin laden,
verbinden, mit 2 Clients Positionen austauschen, Lautstärke/Pan/Richtung prüfen).

Bisher entstandene Dateien (alle neu geschrieben, Build via `project\Conan-Exiles-TeamSpeak.vcxproj`):

| Modul | Dateien | Phase |
|-------|---------|-------|
| Entry-Points | `src\ts\entry\ts3_entry.c` / `ts3_exports.h` | 0 |
| Logging | `src\core\util\log.c/.h` (eigener Lock, nie API-Pfad) | 1 |
| Config | `src\core\config\config.c/.h` (`g_config`, plugin.cfg) | 1 |
| Pos.txt-Watcher | `src\core\mod_file\pos_file.c/.h` (50-ms-Poll, Stop-Event, atomares valid) | 2 |
| TS-API-Kern | `src\ts\adapter\ts3_adapter.c/.h` (Callback-Thread-Guard, Command-Queue, CEDRAIN-Wakeup) | 3 |
| CEPOS-Protokoll | `src\ts\proximity\ts3_cepos.c/.h` (wire-kompatibel zum alten Plugin, base64, 56-Byte-Paket) | 4 |
| Spieler-Tabelle | `src\core\proximity\player_table.c/.h` (64 Slots, 120-s-Expiry) | 4 |
| Proximity-Mathe | `src\core\proximity\proximity_math.c/.h` (gleiche Kurve wie alt, Self-Test loggt Tabelle) | 5 |
| PCM-Gain + Unmute | `src\ts\proximity\ts3_proximity_audio.c/.h` (Seqlock-Snapshots, Batch-Unmute, Gain-Ramp) | 6 |
| 3D-Audio | `src\ts\proximity\ts3_3d.c/.h` (Settings-Dedup, Listener/Client-Epsilon-Dedup, neutraler Rolloff) | 7 |

Hinweis für neuen Chat / anderen PC: Einfach sagen „Führe REWRITE_PLAN.md fort,
Phase 0–7 fertig, weiter mit Phase 8“. Build-Befehl: MSBuild auf
`project\Conan-Exiles-TeamSpeak.vcxproj` (Release|x64) oder `build\build_msvc.ps1`.

---

## Phasen-Übersicht

| Phase | Inhalt | Ergebnis (testbar) | Status |
|-------|--------|--------------------|--------|
| 0 | Projekt-Skelett | Leeres Plugin lädt/entlädt in TS ohne Crash | ✅ gebaut, Test im TS-Client offen |
| 1 | Logging + Config | Log-Datei wird geschrieben, plugin.cfg wird gelesen | ✅ gebaut, Test offen |
| 2 | Pos.txt-Watcher | Eigene Position erscheint im Log | ✅ gebaut, Test offen |
| 3 | TS-API-Kern (1 Thread) | Verbindungsstatus + Kanalliste stabil | ✅ gebaut, Test offen |
| 4 | CEPOS senden/empfangen | Zwei Clients sehen gegenseitig Positionen | ✅ gebaut, Test offen |
| 5 | Proximity-Mathe (pur) | Distanz/Volume-Rechnung, per Testwerte prüfbar | ✅ gebaut, Self-Test loggt bei DebugMode=true |
| 6 | PCM-Gain + Unmute | Lautstärke fällt mit Distanz, keine Stumm-Hänger | ✅ gebaut, Test offen |
| 7 | 3D-Audio / Stereo-Pan | Richtungshören funktioniert | ✅ gebaut (2026-07-07), Test offen |
| 8 | Channel-Management | Auto-Move hub ↔ ingame | ⏳ nächster Schritt |
| 9 | Hub-Parser / Server-Profil | Einstellungen aus Channel-Beschreibung | offen |
| 10 | Zonen-Effekte | Soundproof / Reverb / Lowpass | offen |
| 11 | Voice-Modes + Hotkeys | Whisper/Normal/Shout | offen |
| 12 | Nickname-Anonymisierung | Zufallsnummern ingame | offen |
| 13 | UI (Dialoge + Overlay) | Einstellungsfenster + Overlay | offen |
| 14 | Cleanup / Feinschliff | Sauberer Shutdown, Lasttest 20+ Spieler | offen |

**Reihenfolge ist bindend.** Keine Phase beginnt, bevor die vorherige im echten TS-Client getestet wurde.

---

## Phase 0 — Projekt-Skelett

Ziel: Ein Plugin, das TeamSpeak lädt und wieder entlädt. Mehr nicht.

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 0.1 | Ordnerstruktur + leere `project\*.vcxproj` + `build\build_msvc.ps1` | `project/`, `build/` | Nur Build-Gerüst, keine Logik |
| 0.2 | SDK übernehmen | `sdk/` | Unverändert kopieren |
| 0.3 | `ts3plugin_name` / `ts3plugin_version` / `ts3plugin_author` / `ts3plugin_apiVersion` | `ts3_entry.c` | Version startet bei `7.0.0` |
| 0.4 | `ts3plugin_init` / `ts3plugin_shutdown` (leer, nur Log-Zeile später) | `ts3_entry.c` | Kein Thread-Start, keine API-Calls |
| 0.5 | `ts3plugin_setFunctionPointers` | `ts3_entry.c` | Nur speichern |

**Test:** Plugin in TS laden, aktivieren, deaktivieren, TS beenden — kein Crash, keine Fehlermeldung.

---

## Phase 1 — Logging + Config

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 1.1 | `log_init` / `log_write` / `log_close` — eigene Log-Datei mit EIGENEM Lock | `ts3_debug_log` (Fehler: nutzte anfangs den API-Lock!) | Nie den API-Lock berühren; aus jedem Thread aufrufbar |
| 1.2 | `config_get_folder_path` | `config_files.c` | Nur Pfad ermitteln |
| 1.3 | `config_read` (plugin.cfg: Pfade, Distanzen, Hotkeys, Debug-Flag) | `config_files.c`, `readConfigurationSettings` | Nur lesen + validieren, Defaults setzen |
| 1.4 | `config_write` (gespeicherte Einstellungen) | `config_files.c` | |

**Test:** Log-Datei entsteht, Config-Werte erscheinen korrekt im Log.

---

## Phase 2 — Pos.txt-Watcher (eigene Position)

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 2.1 | `pos_file_read_once` — Pos.txt öffnen, parsen (x/y/z/yaw), schließen | `mod_watcher.c` | Reine Lese-Funktion, kein Thread |
| 2.2 | `pos_watcher_thread_start/stop` — Polling-Thread (~50 ms) | `mod_watcher.c`, `system_threads.c` | Nur lesen + globalen Zustand aktualisieren; sauberes Stop-Event |
| 2.3 | `coordinates_valid`-Logik (Stale-Erkennung: Datei zu alt → invalid) | `coordinatesValid` | Ein Flag, atomar |

**Test:** Ingame bewegen → Koordinaten im Log; Spiel beenden → nach Timeout „invalid".

---

## Phase 3 — TS-API-Kern (das Herzstück der Stabilität)

**Architektur-Entscheidung:** Es gibt GENAU EINEN Weg zur TS-API:
Callbacks (TS-Thread) dürfen direkt rufen. Alle anderen Threads legen Aufträge in die **Command-Queue**, die der Callback-Thread abarbeitet (`onServerError`-Pump o.ä. wie im alten Deferred-System — aber von Anfang an so gebaut, nicht nachgerüstet).

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 3.1 | `ts_state_on_connect_status_changed` — Verbindungsstatus atomar halten | `ts3_adapter.c` (onConnectStatusChangeEvent) | KEINE API-Calls in der Statusprüfung |
| 3.2 | `ts_cmd_queue_push` / `ts_cmd_queue_drain` — Auftrags-Queue (fester Ringpuffer) | `ts3_deferred.c` | Ein Producer-sicherer Ringpuffer, Drain NUR auf Callback-Thread |
| 3.3 | `ts_request_wakeup` — Callback-Thread aufwecken | `ts3_adapter_request_*_wakeup` | Ein Mechanismus, nicht fünf verschiedene wie im alten |
| 3.4 | `ts_get_channel_list` / `ts_get_channel_of_user` (nur Callback-Thread) | `ts3_adapter.c` | Assert/Log wenn falscher Thread |
| 3.5 | `ts_thread_contract_check` — Debug-Helfer: prüft „bin ich auf dem Callback-Thread?" | `ts3_plugin_is_on_callback_thread` | Wird in JEDER API-Funktion als erste Zeile benutzt |

**Test:** Verbinden/Trennen/Reconnect 10× — Status im Log immer korrekt, kein Crash.

---

## Phase 4 — CEPOS senden/empfangen (Positions-Protokoll)

**Protokoll bleibt kompatibel zum alten Plugin** (`ConanExiles_CompletePositional`, gleiche Struktur), damit Mischbetrieb beim Umstieg möglich ist.

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 4.1 | `cepos_build_local` — eigenes Paket aus Pos.txt-Daten bauen | `calculateLocalPositionalData` | Reine Funktion |
| 4.2 | `cepos_send` — senden über Command-Queue, on-change + 1 Hz Keepalive, max 20 Hz | `sendCompletePositionalData` | Drosselung von Anfang an (alte Version flutete den Server) |
| 4.3 | `cepos_on_receive` — Paket validieren (Größe! Absender!) und in Spieler-Tabelle schreiben | `onPluginCommandEvent` + `calculateLocalPositionalAudio` | NUR Daten ablegen, keine Lautstärke-Berechnung hier |
| 4.4 | `player_table_get/put` — feste Tabelle der bekannten Spieler (Position, VoiceDistance, Timestamp) | `adaptivePlayerStates` | Bounds-Checks, Stale-Expiry (120 s) |

**Test:** Zwei Clients: Positionen beider erscheinen gegenseitig im Log, Senderate im Log ≤ 20 Hz.

---

## Phase 5 — Proximity-Mathe (reine Funktionen, keine Threads, keine API)

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 5.1 | `prox_distance` — 3D-Distanz | `proximity_math.c` | |
| 5.2 | `prox_volume_from_distance` — Volume-Kurve (0..1) | `proximity_volume.c`, `calculateVolumeMultiplierWithHubSettings` | Exakt gleiche Kurve wie alt (gleiches Hörverhalten) |
| 5.3 | `prox_stereo_pan` — L/R-Pan aus Blickrichtung (equal power) | `voice_modes.c` (Pan-Teil) | |

**Test:** Feste Testwerte durchrechnen (Tabelle im Log): 0 m → 1.0, Maxdistanz → 0.0, seitlich → Pan.

---

## Phase 6 — PCM-Gain + Unmute (der kritischste Teil)

**Architektur:** Audio-Thread liest NUR Snapshots. TS-Unmute läuft NUR als Batch über die Command-Queue.

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 6.1 | `snap_publish_client` / `snap_read_client` — Seqlock-Snapshot pro Spieler (Gain, Pan) | `ts3_proximity_audio.c` | Lock-frei, Audio-Thread-sicher |
| 6.2 | `pcm_on_playback` — `onEditPlaybackVoiceDataEvent`: Gain + Pan auf Samples anwenden | `ts3_adapter_process_playback_audio` | KEINE Locks, KEINE API, nur Snapshot lesen |
| 6.3 | `unmute_signal` — Flag „Client braucht TS-Unmute" setzen (aus jedem Thread) | `signal_playback_unmute` | Nur Flag + Wakeup |
| 6.4 | `unmute_flush` — alle pending Unmutes in EINEM Batch auf Callback-Thread | `flush_playback_unmutes` | Erster Unmute sofort, Re-Unmute rate-limited (Lehre aus 6.5.147-Bug: Pending nie verwerfen) |
| 6.5 | Sanftes Gain-Ramping (kein Knacken) | `ts3ClientRenderGain`-Smoothing | |

**Test:** Spieler nähert sich → sofort hörbar (kein Delay, kein Burst); entfernt sich → wird leiser bis stumm; wieder nähern → sofort wieder hörbar. Mit 3 Clients testen.

---

## Phase 7 — 3D-Audio / Richtungshören

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 7.1 | `ts3d_init` — `systemset3DSettings` einmalig | `ts3_adapter_init_3d_sound` | Nur 1× pro Verbindung (Dedup-Flag) |
| 7.2 | `ts3d_set_listener` — Listener-Position/-Blickrichtung, mit Epsilon-Dedup | `ts3_adapter_set_listener_3d` | Nur senden wenn geändert (>25 cm / >0.02 fwd) |
| 7.3 | `ts3d_set_client_pos` — Spieler-Position, mit Epsilon-Dedup pro Client | `ts3_adapter_set_client_3d` | dito |
| 7.4 | `ts3d_on_custom_rolloff` — eigene Distanzkurve | `onCustom3dRolloffCalculationClientEvent` | |

**Test:** Spieler links → Ton links; umdrehen → Ton wechselt die Seite.

---

## Phase 8 — Channel-Management (hub ↔ ingame)

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 8.1 | `chan_find_hub_and_ingame` — Channel-IDs per Name finden | `initializeChannelIDs` | Nur Callback-Thread |
| 8.2 | `chan_should_be_ingame` — Entscheidung aus `coordinates_valid` | `channel_manage.c` | Reine Funktion |
| 8.3 | `chan_request_move` — Move über Command-Queue, mit Cooldown + In-Flight-Schutz | `ts3_plugin_execute_channel_move` | Nie zwei Moves parallel (alter Bug) |
| 8.4 | `chan_audio_mode` — Playback-Gate: hub = hart stumm, ingame = proximity | `ts3_adapter_set_audio_mode` | Nur bei Wechsel setzen (Dedup) |

**Test:** Spiel starten → auto-move ingame; Spiel beenden → auto-move hub; im Hub hört man niemanden aus ingame.

---

## Phase 9 — Hub-Parser / Server-Profil

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 9.1 | `hub_request_description` — Channel-Beschreibung anfordern (Queue, Rate-Limit) | `ts3_adapter_request_root_description` | Max 1 Request in-flight (alter Flood-Bug) |
| 9.2 | `hub_parse_settings` — Distanzen/Zonen/Flags aus Beschreibung parsen | `hub_parser.c` | Reine Parse-Funktion, mit Validierung |
| 9.3 | `server_profile_apply` — Profil aktivieren (welcher Server = welche Regeln) | `ts3_server_profile.c` | |

**Test:** Beschreibung im Hub ändern → neue Werte im Log, Distanzen wirken.

---

## Phase 10 — Zonen-Effekte

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 10.1 | `zone_resolve` — in welcher Zone ist Position x/y/z? | `getRemotePlayerZoneIndex` | Reine Funktion |
| 10.2 | `zone_soundproof_muted` — Hartmute zwischen getrennten Zonen | `ts3_plugin_is_soundproof_muted` | In Snapshot publizieren (Audio-Thread liest nur) |
| 10.3 | `fx_lowpass` — Distanz-Lowpass (nur in Reverb-Zonen) | `ts3_apply_lowpass` | Audio-Thread, keine Allokationen |
| 10.4 | `fx_cave_reverb` — Höhlen-Reverb (Comb/Allpass) | `Ts3CaveReverb` | Fester Speicher, kein malloc im Audio-Pfad |

**Test:** In Höhle → Hall; Zonengrenze → stumm; offenes Feld → klar ohne Effekte.

---

## Phase 11 — Voice-Modes + Hotkeys

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 11.1 | `voice_mode_get_distance` — Whisper/Normal/Shout → Distanz (Zone überschreibt global) | `getVoiceDistanceForMode` | Reine Funktion |
| 11.2 | `voice_mode_apply` — Modus wechseln + CEPOS-Cache invalidieren | `voice_mode_apply` | |
| 11.3 | `hotkey_poll` — Tasten abfragen (entprellt, TS-PTT-sicher) | `voice_mode_key_pressed` | Armed/Suppress-Logik übernehmen |

**Test:** Hotkeys wechseln Modus, Chat-Meldung erscheint, Distanz ändert sich hörbar.

---

## Phase 12 — Nickname-Anonymisierung

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 12.1 | `nick_make_random` — 8–10-stellige Zufallsnummer | `ts3_nickname_make_random_digits` | |
| 12.2 | `nick_anonymize_before_ingame` — VOR dem Move umbenennen, alten Namen merken | `apply_ingame_nickname_anonymization` | Kollisionsprüfung im Zielchannel |
| 12.3 | `nick_restore_in_hub` — beim Rückwechsel wiederherstellen | `restore_hub_nickname` | |

**Test:** Ingame = Nummer, zurück im Hub = echter Name. 2× hintereinander (Relog-Fall).

---

## Phase 13 — UI (Dialoge + Overlay)

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 13.1 | Einstellungs-Dialog (Pfad, Distanzen, Hotkeys) | `ui_main.c` | Größter Brocken — in Unterschritte zerlegen wenn dran |
| 13.2 | Chat-Meldungen (`displayInChat`) | `ui_messages.c` | Früher einbauen wenn für Tests nützlich |
| 13.3 | Voice-Overlay (aktueller Modus) | `voice_overlay.c` | |

**Test:** Dialog öffnen/schließen/speichern, Overlay zeigt Moduswechsel.

---

## Phase 14 — Cleanup / Härtetest

| # | Funktion | Referenz (alt) | Anmerkung |
|---|----------|----------------|-----------|
| 14.1 | `plugin_shutdown_sequence` — Threads stoppen → Queue leeren → Snapshots invalidieren → Log schließen | `cleanup.c`, `ts3plugin_shutdown` | Reihenfolge fest dokumentieren |
| 14.2 | Reconnect-/Serverwechsel-Härtung | verstreut im alten | Alle Caches/IDs pro Verbindung zurücksetzen |
| 14.3 | Lasttest | — | 20+ Spieler, 30+ Minuten, keine Crashes, TS-Log sauber |

---

## Arbeitsweise pro Funktion (Checkliste)

Für JEDE Funktion aus dem Plan:

1. **Beschreiben:** Was macht sie? Input/Output? Welcher Thread ruft sie? Welche Locks?
2. **Alte Version lesen** (nur die eine Funktion) — was übernehmen, was war der Bug?
3. **Neu schreiben** — nur diese Funktion, keine Nebenbaustellen.
4. **Bauen** — 0 Warnings ist das Ziel.
5. **Testen** im echten TS-Client (Testschritt steht bei jeder Phase).
6. **Abhaken** hier im Plan (✅ + Datum), erst dann die nächste Funktion.

## Offene Punkte (vor Phase 0 klären)

- [ ] GitHub-Fork vom Kumpel anlegen und `E:\programme\Conan-Exiles-Teamspeak` als Remote verbinden (zentrale Entwicklung, sein Vorschlag)
- [ ] Versionsnummer: Vorschlag `7.0.0-dev` während des Rewrites
- [ ] Mischbetrieb alt/neu auf dem Server: CEPOS bleibt kompatibel (Phase 4), trotzdem Umstiegstermin planen
