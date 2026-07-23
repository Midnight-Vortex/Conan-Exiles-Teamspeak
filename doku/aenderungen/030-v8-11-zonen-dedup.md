# 030 — V8.11 Zonen-Deduplizierung

**Phase:** V8.11 · **Plan:** `REWRITE_PLAN_V8.10.md`

## Worum geht es?

Es gab **zwei** Zonen-Implementierungen: `zone_resolve` (HubSettings, kanonisch
für Audio) und `getPlayerZone` in `validation.c` (Legacy-`zones[]`-Mirror).
V8.11 entfernt die Duplikation im Laufzeitpfad.

## Was wurde geändert?

1. **`core/validation/validation.c` gelöscht** — Hub-Distanz-Limits nach
   `ui/validation/ui_hub_validation.c` (nur F10: `shouldApplyDistanceLimits`,
   `validateDistanceValue`).
2. **`resolvePlayerZoneIndex`** nutzt `server_profile_get` + `zone_resolve`
   statt `getPlayerZone` + Legacy-Polygon in `validation.c`.
3. **`getServerHashForTracking`** entfernt (nie aufgerufen, Mumble-Rest).

**Behalten:** `zones[]` in `plugin_ui_compat.c` als **UI-Mirror** (F10-Namen,
pro-Zonen-Distanzen, Overlay-Text) — wird aus Hub synchronisiert, kein zweiter
Polygon-Test mehr.

## Warum besser?

- Eine kanonische Zonen-Geometrie (`zone_resolve`) für Soundproof/Reverb-Pfad.
- `core/` ohne validations-Blob; Hub-Limits klar in `ui/`.
- Weniger tote API-Oberfläche in `plugin_modules.h`.

## Getestet

- `bash tests/run_tests.sh`
- `bash build/build_mingw.sh`
- Manuell offen: Zonen-Overlay + Cave/Reverb im TS-Client
