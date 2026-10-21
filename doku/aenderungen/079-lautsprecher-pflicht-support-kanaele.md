# 079 — Lautsprecher-Pflicht im Ingame-Channel, Support-Kanäle ohne Auto-Move

2026-10-21. Nur Plugin.

## 1. Wunsch

1. Spieler sollen im Ingame-Channel nicht mit stummgeschalteten Lautsprechern sitzen.
2. Kanäle mit „Support“ oder „Event“ im Namen sollen — wie der bestehende Event-Kanal — niemanden
   automatisch herausschieben.

## 2. Lautsprecher-Pflicht (Root-Schalter `ForceSpeakersOn`, Standard aus)

Den Stumm-Knopf selbst kann kein Plugin und kein TS-Server sperren (laut SDK ist der Status nur lesbar).
Mit `ForceSpeakersOn=True` im `[GLOBAL]`-Teil der Root-Beschreibung gilt:

| Fall | Wirkung |
|---|---|
| Eigene Lautsprecher in TS stumm (`CLIENT_OUTPUT_MUTED` oder `CLIENT_OUTPUTONLY_MUTED`), im Ingame-Channel | Das Plugin schiebt den Spieler in den Hub (auch wenn Auto-Move aus ist) und schreibt einmal in den Chat: „Your speakers are muted - moved to the hub. Unmute your speakers to get back ingame.“ |
| Lautsprecher wieder an | Auto-Move bringt ihn zurück in den Ingame-Channel (bei ausgeschaltetem Auto-Move: selbst wechseln) |
| Ein **anderer** Spieler im Ingame-Channel hat die Lautsprecher stumm | Unser Plugin schaltet ihn im Proximity-Modus hart stumm. Deckt alte Plugins und die Sekunde vor dem Wechsel ab |
| Hub, Event-/Support-/Plot-Kanäle | keine Wirkung |

Umsetzung: `chan_tick` (TS-Callback, ~2×/s im Spiel) liest den eigenen Status und den aller Spieler im
Ingame-Channel (`ts3_client_speakers_muted`) und setzt pro Spieler ein Flag (`ts3_audio_set_client_deaf`),
das der PCM-Pfad wie eine schalldichte Zone behandelt. Schalter aus / nicht im Ingame-Channel → alle Flags
gelöscht.

**Nicht erkennbar:** Windows-Lautstärke 0, abgezogenes Headset, TS-Wiedergaberegler ganz unten.

## 3. Event- / Support-Kanäle

`chan_name_has_ignore_keyword`: „event“ und „support“ zählen jetzt **irgendwo im Namen**, ohne Rücksicht
auf Groß-/Kleinschreibung (Event, [Event] Halle, Eventhalle, Support, Supportraum, Kundensupport). „plot“
bleibt ein ganzes Wort (Plot Nord, nicht Plotter). In solchen Kanälen: kein Auto-Move, Ton normal
(Passthrough). Hub, Ingame und Root sind nie betroffen.

## 4. Dateien

| Datei | Änderung |
|---|---|
| `src/core/hub/hub_parser.c/.h` | Schlüssel `ForceSpeakersOn` → `forceSpeakersOn` |
| `src/ts/profile/ts3_server_profile.c/.h` | `server_profile_force_speakers_on()`, Log-Zeile `forceSpeakers=` |
| `src/ts/adapter/ts3_adapter.c/.h` | `ts3_client_speakers_muted()` |
| `src/ts/proximity/ts3_proximity_audio.c/.h` | Flag pro Spieler, Stille im PCM-Pfad |
| `src/ts/channel/channel_manage.c` | Lautsprecher-Regel in `chan_tick`, Schlüsselwort „support“, Reset |
| `tests/hub_parser_test.c` | `ForceSpeakersOn` aus/an |

## 5. Test

- Unit-Tests (MSVC) grün inkl. 2 neuer Parser-Prüfungen; `check_layering.sh` OK; DLL ohne Warnungen.
- Schlüsselwort-Regel einzeln geprüft (15 Namen, u. a. Eventhalle/Supportraum/Kundensupport = ja,
  Plotter/Ingame/Hub = nein).
- **Nicht** im echten TS getestet (Lautsprecher stumm → Hub → zurück, anderer Spieler stumm).
