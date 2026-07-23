# 016 — voice_modes von ts/ und ui/ entkoppelt (Hook-Inversion)

## Was wurde geaendert?

`src/core/voice/voice_modes.c` band bisher die ts/- und ui/-Schicht ein und
verletzte damit die V8-Schichtenregel. Jetzt kommt es mit **null** ts/- und
ui/-Includes aus. Die Mode-/Distanz-Logik ist purer Kern und bleibt an Ort
und Stelle — nur die Seiteneffekte und ein Datenzugriff laufen ueber Hooks.

Entfernte Includes:

- `ui/overlay/voice_overlay.h`
- `ts/adapter/ts3_adapter.h`
- `ts/profile/ts3_server_profile.h`
- `ts/proximity/ts3_cepos.h`
- `plugin_modules.h` (zog die alte Mumble-Blob-Schicht mit)

## Die Steckdosen-Idee (fuer Anfaenger)

```
   core/voice/voice_modes.c            ts/entry/ts3_entry.c  (beim Start)
   ┌───────────────────────┐          ┌──────────────────────────────┐
   │  reine Logik:          │          │  echte Funktionen:            │
   │  Mode -> Distanz       │          │  displayInChat, CEPOS,        │
   │                        │          │  Overlay, Server-Profil       │
   │   [ Steckdose ]        │◀── steckt│  ── Stecker rein ─────────────│
   │   VoiceModeHooks       │  hooks   │  voice_mode_set_hooks(&hooks) │
   └───────────────────────┘          └──────────────────────────────┘
```

- **core stellt eine Steckdose bereit** (`typedef struct VoiceModeHooks` +
  `voice_mode_set_hooks(...)`).
- **ts/ui steckt beim Start den Stecker rein** (`voice_mode_wire_hooks()` in
  `ts3plugin_init`).
- Ist **kein Stecker** drin (Hook == `NULL`, z. B. im Unit-Test), passiert
  gefahrlos nichts (No-Op). Genau das macht das Modul testbar.

Die Abhaengigkeit **zeigt nach der Umkehr wieder nach unten**: `ts/` kennt
`core/`, aber `core/` kennt `ts/` nicht mehr — es kennt nur seine eigene
Steckdose.

## Welche ts/ui-Symbole nutzte voice_modes — und was wurde daraus?

| genutztes Symbol (Schicht)                         | jetzt: Hook                         |
|----------------------------------------------------|-------------------------------------|
| `displayInChat` (util) + `ts3_is_connected` (ts)   | `notify_chat(msg)`                  |
| `cepos_invalidate_send_cache` (ts)                 | `invalidate_cepos_cache()`          |
| `cepos_signal_send_pending` (ts)                   | `signal_send_pending()`             |
| `updateVoiceOverlay` (ui)                          | `overlay_sync()`                    |
| `server_profile_get` + `server_profile_get_local_race` (ts) | `get_profile(VoiceModeProfile*)` |
| `ts3_plugin_has_pending_chat` (util)               | `has_pending_chat()`                |
| `ts3_thread_is_callback` (ts) + `ts3_plugin_flush_pending_chat` (util) | `flush_pending_chat()` |

Der **Datenzugriff** (Server-Profil fuer den Distanz-Clamp) ist der einzige
Sonderfall: statt die ts/-Funktion zu rufen, liefert der Hook `get_profile`
eine **Kopie** aus reinen core-Typen (`VoiceModeProfile` mit `HubSettings`
und `HubRace` aus `core/hub/hub_parser.h`). So bleiben `zone_resolve` und die
komplette Clamp-Rechnung im Kern.

## Wie funktioniert es jetzt konkret?

- `voice_mode_apply()` ruft (falls gesteckt) `notify_chat`,
  `invalidate_cepos_cache`, `signal_send_pending`, `overlay_sync`.
- `voice_mode_get_distance()` holt das Profil per `get_profile` und rechnet
  Zone-Override > globale Config, danach Min/Max-Clamp (Rasse schlaegt Hub) —
  exakt wie vorher, nur ohne direkten ts/-Aufruf.
- `voice_mode_flush_notify()` / `_has_pending_notify()` gehen ueber
  `flush_pending_chat` / `has_pending_chat`.

Verhalten ist identisch zur vorherigen Version. Die echte Verkabelung sitzt in
`ts3_entry.c` (`voice_mode_wire_hooks`), die den Connected-Check und die
Profil-Lesefunktionen kapselt.

## Warum ist das besser?

- `core/` ist wieder frei von ts/ui — die Schichtenregel gilt.
- `voice_mode_get_distance` ist jetzt **auf jedem Rechner testbar** (siehe
  `tests/voice_modes_test.c`): Zone schlaegt global, Profil-Clamp greift,
  Whisper < Normal < Shout.
- Kein `plugin_modules.h` mehr im Kern → keine versteckte Mumble-Alt-Last.

## Wie getestet?

- `bash build/build_mingw.sh` — DLL baut und linkt (Hooks in `ts3_entry.c`).
- `bash tests/run_tests.sh` — neue Suite `voice_modes_test` (13 Checks) gruen,
  alle Suiten gruen.
- Grep-Nachweis: `src/core/voice/voice_modes.c` enthaelt keinen `ts/`- oder
  `ui/`-Include mehr (Layering-Wache 017 prueft das automatisch).

Manueller TS-Test (wenn ein Client verfuegbar ist): Voice-Mode per Hotkey
umschalten → Chat-Zeile "Voice mode: …" erscheint, Overlay aktualisiert sich,
und die neue Distanz erreicht andere Clients (CEPOS). Das deckt alle vier
Aktions-Hooks ab.

## Lerneffekt

Wenn ein Modul echte pure Logik enthaelt, aber ein paar Aussenwirkungen
braucht, **verschiebt man es nicht** (wie `channel_manage`), sondern **kehrt
die Abhaengigkeit um**: core definiert einen schmalen Hook-Vertrag, die obere
Schicht fuellt ihn beim Start. NULL-Hooks als sichere No-Ops halten den Kern
gleichzeitig eigenstaendig kompilier- und testbar.
