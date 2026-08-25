# 044 — EnablePosFile (HTTP-only Testmodus)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/config/config.h` | `PluginConfig.enablePosFile` — 1 = Pos.txt (Default), 0 = HTTP-only |
| `src/core/config/config.c` | Default `1`, Parse `EnablePosFile=`, Save, Load-Log `posFile=%d` |
| `src/core/mod_file/pos_file.c` | Watcher: bei `enablePosFile==0` kein Pos.txt-I/O, kein Invalidate wegen fehlendem Pfad |

**Nicht geändert:** F10-UI, HTTP-Server, CEPOS, Pos.txt-Parser bei aktivem Modus.

## 2. Wie war es vorher (V7/V8)?

- Pos-Watcher las **immer** Pos.txt, wenn ein Saved-Pfad konfiguriert war.
- Fehlender Pfad → sofort `g_coordinatesValid = 0` und `continue` — **wischte HTTP-Injections weg**.
- Bei langsamem HTTP (POST alle 18–55 s) füllte Pos.txt die Lücken → schwer zu erkennen, ob nur HTTP aktiv ist.

## 3. Warum ist die neue Lösung besser?

- **Gezielter HTTP-only-Test:** Pos.txt-Fallback abschaltbar per `plugin.cfg`.
- HTTP-Koordinaten bleiben gültig, solange Inject + Grace (`POS_COORD_GRACE_MS`) laufen — auch ohne Saved-Pfad.
- Kein Autodetect-/Datei-I/O im Testmodus → weniger Rauschen im Log und auf der Platte.

## 4. Wie funktioniert es jetzt?

```
plugin.cfg: EnablePosFile=false
        │
        ▼
config_load → g_config.enablePosFile = 0
        │
        ▼
pos_watcher_thread
   ├─ Log einmal: "Pos.txt disabled (EnablePosFile=false, HTTP-only)"
   ├─ kein pos_autodetect_saved_path
   ├─ kein pos_file_read_once / kein Invalidate wegen leerem Pfad
   ├─ Gültigkeit: httpHold (2 s nach Inject) + Grace (15 s nach last valid)
   └─ g_tickCallback läuft weiter

HTTP POST → pos_inject_sample → g_lastValidTick, g_httpHoldUntilMs
        │
        └─ Watcher hält coords bis Grace abläuft (HTTP stoppt)
```

Bei `EnablePosFile=true` (Default): unverändertes bisheriges Verhalten.

## 5. Wie wurde es getestet?

1. `Documents\Conan Exiles TeamSpeak plugin\plugin.cfg`:
   ```ini
   EnablePosFile=false
   DebugMode=true
   ```
2. TS-Client neu starten (DLL-Reload).
3. Log beim Start: `CONFIG: loaded … posFile=0` und `POS: Pos.txt disabled …`.
4. Nur HTTP POST senden (Mod/Blueprint) — **keine** `DBG POS: seq=… age=…` aus Pos.txt.
5. `HTTP: POST seq=…` und Proximity/CEPOS weiter aktiv.
6. Pos.txt manuell ändern — Position darf **nicht** springen (Datei wird ignoriert).
7. HTTP stoppen → nach Grace: `POS: coordinates invalid (HTTP-only, grace=…ms)`.

## 6. Lerneffekt

Zwei Positionsquellen (Datei + HTTP) brauchen einen Schalter für Isolationstests — sonst maskiert die robustere Quelle Fehler der anderen. Watcher-Logik darf bei fehlendem Pfad nicht blind invalidieren, wenn eine zweite Quelle (HTTP) die Koordinaten liefert.
