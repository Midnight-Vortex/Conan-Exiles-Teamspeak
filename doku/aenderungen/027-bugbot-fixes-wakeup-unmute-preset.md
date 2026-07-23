# 027 — Bugbot-Fixes: Wakeup, Unmute-Ring, Preset-Pfad

**Phase:** V8.9 Nacharbeit · **Quelle:** Bugbot-Review gegen `tsmain`

## Worum geht es?

Drei echte Bugs aus dem Voll-Review — kein Stil, sondern Laufzeit-Risiko.

## 1. Wakeup verwirft rate-limited CEDRAIN

**Datei:** `src/ts/adapter/ts3_adapter.c`

**Problem:** Der Wakeup-Thread hat `g_wakeupPending` gelöscht, bevor klar war, ob
`sendPluginCommand("CEDRAIN:1")` gesendet wird. Bei 30-ms-Drossel oder kurzzeitig
fehlender Verbindung blieb CEDRAIN aus, bis zufällig wieder `ts3_request_wakeup()`
kam.

**Fix:** `g_wakeupPending` erst nach erfolgreichem Send löschen. Bei Rate-Limit oder
Disconnect: Pending behalten, kurz warten, `SetEvent` erneut setzen.

## 2. Unmute-Ring veröffentlicht Index zu früh

**Datei:** `src/ts/proximity/ts3_proximity_audio.c`

**Problem:** `unmute_ring_push` erhöhte `g_unmuteRingWrite` vor dem Schreiben der
Client-ID. Der Callback-Thread konnte den Slot zu früh lesen.

**Fix:** Produzenten per Spinlock serialisieren; Slot schreiben, dann Write-Index
erhöhen (Publish nach Store).

## 3. Preset-Load ignoriert Auto-Pfad

**Datei:** `src/core/config/config_files.c`

**Problem:** `loadVoicePreset` las `SavedPath=` noch direkt aus `plugin.cfg` statt aus
`g_config` (Single-Writer). Bei Auto-Pfad ohne geöffnetes F10 konnte der falsche Pfad
persistiert werden.

**Fix:** Aktiven Pfad per `config_copy()` holen (`AutomaticSavedPath` wenn Auto-Modus,
sonst `SavedPath`) — gleiche Logik wie `pos_file` und `plugin_ui_compat`.

## Wie getestet?

- `bash tests/run_tests.sh`
- `bash build/build_mingw.sh`
- Bugbot erneut über gesamten Branch
