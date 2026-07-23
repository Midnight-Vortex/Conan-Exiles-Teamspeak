# 019 — Legacy-Abbau (Duplikate entfernt, Schichten aufgeraeumt)

**Phase:** V8.5b (Fortsetzung von `018`) · **Lektion:** `02-lessons-learned-v7.md`
Lektion 4 (Doppel-Besitzer) und 5 (Schichten-Verletzung)

Nachdem der einzige plugin.cfg-Schreiber steht (`018`), wurde der drumherum
liegende Legacy-Ballast abgebaut — dort, wo Aufrufer sauber 1:1 umgeleitet
werden konnten. Was zu verzahnt war, blieb bewusst stehen (unten begruendet).

## Was wurde geloescht / umgeleitet?

### 1. `config_files.c` — Legacy-Schreib-/Leselogik

- **geloescht:** `readConfigurationSettings()`, `loadVoiceDistancesFromConfig()`,
  `saveConfigurationChange()` (letztere hatte gar keinen Aufrufer mehr).
- **entkernt:** `saveVoiceSettings()` und `writeFullConfiguration()` oeffnen
  keine Datei mehr; sie aktualisieren nur die Globalen und rufen
  `plugin_ui_on_settings_saved()` → `config_save()`.
- **umgeleitet:** die zwei UI-Stellen, die vorher `plugin.cfg` neu einlasen
  (`readConfigurationSettings` / `loadVoiceDistancesFromConfig` in `ui_main.c`),
  rufen jetzt `plugin_ui_sync_from_config()` (liest `g_config`).

### 2. `util_base.c` — doppelter Config-Pfad + tote Distanzrechnung

- **`getConfigFolderPath()` geloescht.** Sie war ein exaktes Duplikat von
  `config_get_folder_path()` in `config.c` (beide bauen
  `Dokumente\Conan Exiles TeamSpeak plugin` und legen den Ordner an). Alle
  Aufrufer (in `util_base.c`, `config_files.c`, `ui_main.c`) zeigen jetzt auf
  `config_get_folder_path()`. Das war eine der "2× Config-Pfad"-Dubletten aus
  Lektion 5.
- **`calculateDistance()` / `calculateDistance3D()` geloescht.** Beide hatten
  **keinen einzigen Aufrufer** (grep-bewiesen) — reine tote 3D-Euklid-Helfer
  aus der Mumble-Zeit. Die lebende Distanzrechnung liegt in
  `core/proximity/proximity_math.c` (`prox_distance`, getestet).

### 3. `proximity_volume.c` — komplette Datei geloescht

Nach `018` war der einzige noch benutzte Teil `applyDistanceToAllPlayers()`,
und der ist nur eine duenne Huelle:

```c
void applyDistanceToAllPlayers() {
    if (!enableDistanceMuting) return;
    ...
    ts3_plugin_apply_proximity_volumes_force();   // beide Zweige tun dasselbe
}
```

- **inlined:** an allen 6 Aufrufern (`validation.c`, `config_files.c`,
  `ui_main.c` ×2, `ui_dynamic.c`) steht jetzt direkt
  `if (enableDistanceMuting) { ts3_plugin_apply_proximity_volumes_force(); }` —
  **verhaltensgleich** (der `enableDistanceMuting`-Waechter bleibt erhalten).
- **tot & mit-geloescht:** `calculateVolumeMultiplier`,
  `calculateVolumeMultiplierWithHubSettings`, `plugin_proximity_volume_context`.
  Der echte TS-Audio-Pfad benutzt sie nicht (grep: keine Nutzung in `src/ts/`);
  die Volumen-Kurve `proximity_calculate_volume_with_hub` lebt und wird in
  `proximity_math` getestet.
- Datei aus `build/build_mingw.sh` und `project/…vcxproj` entfernt
  (`xmllint --noout` gruen). 29 → 28 `.c`-Dateien.

### 4. `validation.c` — tote Funktionen raus, Schicht bereinigt

- **geloescht:** `checkConnectionStatus()` und `validatePlayerDistances()` —
  beide ohne Aufrufer (grep-bewiesen).
- **entkoppelt:** Danach referenziert `validation.c` keinen einzigen `ts3_*`-
  oder `plugin_ui_*`-Namen mehr. Die `#include "ui/…"`- und `#include "ts/…"`-
  Zeilen sind entfernt; alle verbliebenen Globalen (`isConnectedToServer`,
  `hubMinimumWhisper`, `zones`, `mumbleAPI`, …) kommen aus `plugin.h`. Damit
  faellt `validation.c` **von der Layering-Allowlist** (siehe unten).

### 5. `plugin_modules.h` — 30 verwaiste Deklarationen entfernt (Stage 3)

Reine Kopfzeilen-Schrumpfung: Deklarationen, die weder eine Definition noch
einen Aufrufer irgendwo in `src/` hatten (grep-bewiesen, Mumble-Reste) —
ganze Gruppen `proximity_adaptive.c`, `mod_watcher.c`, `system_threads.c`,
`cleanup.c` plus einzelne tote `voice_modes`/`channel_manage`/`ui`-Eintraege.
Die DLL linkt danach **byte-identisch** — Beweis, dass niemand sie brauchte.

## Was blieb bewusst stehen (und warum)?

- **`validation.c` bleibt als Datei** (nur kleiner). Ihre Zonen-Geometrie
  (`getPlayerZone` + `isPointInPolygon`/`zoneContainsPoint`) ist **kein**
  sauberes 1:1 zu `zone_resolve`: `getPlayerZone` arbeitet auf den Legacy-
  Globalen `zones[]` und probiert mehrere Skalen (Meter/cm) und zwei Layouts,
  waehrend `zone_resolve` auf `HubSettings` mit ±1 m-Hoehenband laeuft. Ein
  Umbiegen waere eine Verhaltensaenderung im Audio-/Soundproof-Pfad — zu
  riskant fuer dieses Paket. Auch die UI-Eingabe-Validierer
  (`shouldValidateValue`, `validateDistanceValue`, `shouldApplyDistanceLimits`)
  bleiben: sie werden von `ui_dynamic.c`/`ui_messages.c` genutzt und sind
  Heuristik gegen Hub-Grenzen, nicht `config_clamp` (feste 0.5..500-Grenzen).
- **`config_files.c` und `util_base.c` bleiben auf der Allowlist.** Beide binden
  weiter `ui/`-/`ts/`-Header ein: `config_files.c` fuer den F10-Save-Brueckenpfad
  (`plugin_ui_on_settings_saved`, `ts3_plugin_apply_proximity_volumes_force`,
  Presets), `util_base.c` fuer die geteilten Shims (`ts3_queue_chat_message`,
  `displayInChat`, `mumbleAPI`). Ihr vollstaendiger Umzug nach `ts/`/`ui/` ist
  Sache von V8.6/V8.7 (wie bei `channel_manage` in `015`).

## Layering-Allowlist: 5 → 3

`tests/check_layering.sh` (siehe `017`) wurde entsprechend gekuerzt:

| vorher | nachher |
|---|---|
| config_files.c, validation.c, util_base.c, proximity_volume.c, nick_anonymize.c | config_files.c, util_base.c, nick_anonymize.c |

- `proximity_volume.c` — Datei geloescht.
- `validation.c` — von `ui/`/`ts/` entkoppelt.

Nachweis nach der Aenderung:

```
$ bash tests/check_layering.sh
  known (allowed): src/core/config/config_files.c
  known (allowed): src/core/nick/nick_anonymize.c
  known (allowed): src/core/util/util_base.c
  summary: 3 known-allowed, 0 new
LAYERING: OK
```

## Warum ist das besser / stabiler?

- **Weniger Doppel-Besitzer, weniger tote Pfade** — jede geloeschte Dublette
  ist ein "welche Stelle ist die richtige?"-Bug weniger (Lektion 4/5).
- **Kleinere, ehrlichere Allowlist** — die Schichten-Wache zeigt genau die
  zwei echten Rest-Altlasten, ohne aufgeraeumte Eintraege mitzuschleppen.
- **Verhalten unveraendert** — Inlines sind 1:1 (Waechter erhalten), geloeschte
  Funktionen waren tot; die DLL linkt, alle Tests bleiben gruen.

## Wie getestet?

- `bash tests/run_tests.sh` → alle Suiten gruen (`layering_guard`: 3 known, 0 new).
- `bash build/build_mingw.sh` → DLL linkt (28 `.c`-Dateien).
- `xmllint --noout project/Conan-Exiles-TeamSpeak.vcxproj` → OK.
- **Offen (Mensch, im TS-Client):** wie `018` — plus Gegenprobe, dass
  Distanz-Apply beim Speichern/Preset-Load weiterhin greift (Lautstaerke
  aktualisiert sich), da `applyDistanceToAllPlayers` jetzt inline steht.
