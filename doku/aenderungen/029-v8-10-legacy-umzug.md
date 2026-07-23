# 029 — V8.10 Legacy-Umzug aus core/

**Phase:** V8.10 · **Plan:** `REWRITE_PLAN_V8.10.md`

## Worum geht es?

Die letzten zwei `core/`-Dateien mit `ui/`/`ts/`-Includes (`config_files.c`,
`util_base.c`) und der WM_COMMAND-Rest in `ui_main.c` gehörten schichtentechnisch
nicht nach `core/`. V8.10 verschiebt sie mechanisch — ohne Verhaltensänderung.

## Was wurde geändert?

| Vorher | Nachher |
|--------|---------|
| `core/config/config_files.c` | `ui/config/ui_voice_presets.c` |
| `core/util/util_base.c` | `ui/util/ui_key_util.c`, `ui_display_util.c`, `ui_ts_chat_queue.c` |
| `ui_main.c` WM_COMMAND (~560 Zeilen) | `ui/dialogs/ui_config_command.c` |

`tests/check_layering.sh`: Allowlist ist **leer** — `core/` ist wieder rein.

## Warum besser?

- Layering-Regel gilt ohne Ausnahme (CI `layering_guard` ohne known-allowed).
- Presets/Chat/Key-Helfer liegen bei der UI-Schicht, wo sie hingehören.
- F10-Dialog: jede Nachrichtenart in eigener Datei (V8.7 + V8.10).

## Getestet

- `bash tests/run_tests.sh` — alle Suiten grün
- `bash build/build_mingw.sh` — DLL linkt
