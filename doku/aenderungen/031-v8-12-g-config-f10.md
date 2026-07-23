# V8.12 — F10 auf g_config (Single Source of Truth)

**Phase:** V8.12 · **Version:** 8.0.2

## Worum geht es?

Bisher: F10 las/schrieb **Legacy-Globals** (`distanceWhisper`, `whisperKey`, … in
`plugin.h`), und `plugin_ui_sync_to_config()` kopierte sie erst beim Speichern nach
`g_config`. Das war Split-Brain während der Dialog-Session offen war.

V8.12 macht **`g_config` zur einzigen Quelle** für persistierte Einstellungen — über
eine F10-Working-Copy.

## Architektur

```
F10 öffnen  → ui_cfg_dialog_begin()  → config_copy → s_f10Cfg
Bearbeiten  → ui_cfg()->feld         → Working copy (UI thread)
Speichern   → ui_cfg_commit()        → config_apply + config_save + Side effects
            → ui_cfg_publish_legacy_mirrors()  → Overlay/Key-Watcher-Spiegel
F10 zu      → ui_cfg_dialog_end()
```

Neue Dateien:
- `src/ui/config/ui_config_state.h`
- `src/ui/config/ui_config_state.c`

## Was wurde geändert?

| Bereich | Vorher | Nachher |
|---------|--------|---------|
| F10 UI (`ui_config_*.c`, `ui_dynamic`, `ui_messages`, `ui_voice_presets`) | Legacy-Globals | `ui_cfg()->…` |
| Overlay | `voiceHudTheme`, … | `g_config.hudTheme`, … |
| Key-Watcher | `configUIKey` | `g_config.configUIKey` |
| Proximity gate | `enableDistanceMuting` | `g_config.enableDistanceMuting` |
| Pfad-Speichern | `savedPath` global + plugin.cfg parse | `ui_cfg_set/get_active_saved_path` |
| `plugin_ui_sync_*` | Volle Kopie hin/her | Delegiert an publish/commit |

**Legacy-Globals bleiben** als schreibgeschützte Spiegel (Overlay-Kompatibilität), werden
aber nur noch via `ui_cfg_publish_legacy_mirrors()` aktualisiert — nicht mehr direkt vom
F10 beschrieben.

## Warum besser?

- Kein Split-Brain zwischen F10-Edits und `g_config` mehr.
- `voice_modes.c`, `channel_manage.c`, `ts3_entry.c` lasen schon `g_config` — F10 ist
  jetzt auf demselben Stand **vor** dem Save-Klick (innerhalb der Working copy).
- Weniger `plugin.cfg`-Direktparse in UI-Handlern.

## Getestet

- `bash tests/run_tests.sh`
- `bash build/build_mingw.sh`
- Manuell offen: F10 öffnen, Werte ändern, speichern, Preset laden, HUD live preview
