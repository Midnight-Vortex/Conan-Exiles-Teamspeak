# 053 — HUD-Zone bei HTTP-Position aktualisieren

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | HTTP- und Pos.txt-Callback teilen `ts3_on_position_update_common()` inkl. `plugin_ui_on_position_tick()`; Version **8.0.6** |
| `src/ui/plugin_ui_compat.h` | Kommentar: Tick auch vom HTTP-Inject-Thread; Overlay nur per PostMessage |
| `doku/aenderungen/038-pos-http-server.md` | Kurzer Hinweis auf den behobenen Bug |

**Eine Funktion:** HUD-Zonenanzeige bei HTTP-Positionsinjektion mit dem Pos.txt-Pfad gleichziehen.

---

## 2. Wie war es vorher (038)?

`ts3_on_http_position_update` rief CEPOS, CEPING und Audio-Snapshots auf, **ohne**
`plugin_ui_on_position_tick()`. Begründung damals: Overlay/GDI vom HTTP-Thread unsicher.

Folge: `currentZoneIndex` in `plugin_ui_sync_live_state()` wurde nur vom Pos-Watcher
gesetzt. Während das HTTP-Hold-Fenster (2 s) Pos.txt-Accepts blockiert, lief der
Watcher-Tick nicht — die HUD zeigte dauerhaft **„Ausserhalb“**, obwohl Audio und
Proximity korrekt liefen (`pos_get_current` + `zone_resolve` im Audio-Pfad).

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Aspekt | Vorher | Jetzt |
|---|---|---|
| HUD-Zone bei HTTP | Stuck auf „Ausserhalb“ | Gleicher Tick wie Pos.txt |
| Callback-Drift | Zwei fast identische Bodies | Eine gemeinsame Funktion |
| Thread-Safety | Tick fälschlich weggelassen | Tick nutzt nur Atomics/Locks; `updateVoiceOverlay()` postet `WM_VOICEOVERLAY_REFRESH` wenn nicht auf Overlay-Thread |

Kein GDI/`DestroyWindow` vom HTTP-Thread — nur der bereits vorhandene PostMessage-Pfad.

---

## 4. Wie funktioniert es jetzt?

```
HTTP POST → pos_inject_sample → ts3_on_http_position_update()
                                      │
                                      ▼
                            ts3_on_position_update_common()
                                      │
                    ┌─────────────────┼─────────────────┐
                    ▼                 ▼                 ▼
      plugin_ui_on_position_tick   CEPOS/CEPING    ts3_audio_on_local_position_update
                    │
                    ▼
      plugin_ui_sync_live_state → currentZoneIndex via zone_resolve
                    │
                    ▼
      updateVoiceOverlay() → PostMessage (wenn nicht Overlay-Thread)
```

Pos-Watcher ruft dieselbe `ts3_on_position_update_common()` über
`ts3_on_local_position_update()`.

---

## 5. Wie wurde es getestet?

- Build: `.\build.ps1 -SkipDeploy` — **OK** (MSBuild Release x64, exit 0, ~15.6 s).
- Ingame (HTTP-Positionsquelle, z. B. Workshop-Mod): Zone/Box betreten — HUD muss
  Zonenname statt „Ausserhalb“ anzeigen; Kollege auf Pos.txt unverändert.
- Audio/Proximity: keine Änderung erwartet (nur UI-Tick nachgezogen).

---

## 6. Lerneffekt

Nicht jeder UI-Pfad braucht den Overlay-Thread — marshaling (`PostMessage`) reicht,
wenn der Tick nur shared State liest und Refresh anfordert. Zwei Callbacks mit
leicht unterschiedlichem Body driften leicht; eine gemeinsame Funktion verhindert das.
