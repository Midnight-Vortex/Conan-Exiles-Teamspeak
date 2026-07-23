# 028 — Bugbot-Fixes Runde 2

**Quelle:** Bugbot Re-Review nach `027`

## 1. `writeFullConfiguration` bevorzugte veraltetes F10-Label

**Fix:** `gameFolder` (vom Aufrufer, z. B. Preset-Load aus `g_config`) hat Vorrang vor
`displayedPathText`. Wenn beides leer: Fallback auf `config_copy()` statt Hardcode-only.

## 2. CEDRAIN mit veralteter Connection-ID

**Fix:** `conn_id_load()` erst **nach** dem Rate-Limit-Sleep, unmittelbar vor
`sendPluginCommand`. Pending/urgent werden erst **nach** dem Send gelöscht.

## 3. Reset vs. Unmute-Ring-Lock

**Fix:** `ts3_audio_reset` wartet auf `g_unmuteRingPushLock` (kein forcibles
Zurücksetzen während PCM-Thread pusht), leert Ring unter Lock, gibt Lock frei.

## Getestet

- `bash tests/run_tests.sh`
- `bash build/build_mingw.sh`
- Bugbot erneut
