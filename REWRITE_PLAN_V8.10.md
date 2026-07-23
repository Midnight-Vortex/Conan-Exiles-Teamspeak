# V8.10 / V8.11 — Legacy-Abbau (Schichten + Zonen)

**Stand:** 2026-07-23 · **Basis:** V8.0.0 (`cursor/v8-rewrite-ca5c`) · **Ziel:** 8.0.1

## Ausgangslage

Nach V8.9 bleiben vier bewusst tolerierte Altlasten:

| Altlast | Problem | Lösung |
|---------|---------|--------|
| `core/config/config_files.c` | F10/Preset-Bridge in `core/`, inkl. `ui/`/`ts/` | → `ui/config/ui_voice_presets.c` |
| `core/util/util_base.c` | Chat-Queue (TS) + UI-Helfer in `core/` | Split: `ts/adapter/ts3_chat_queue.c`, `ui/util/*` |
| `core/validation/validation.c` | Duplikat-Zonenlogik (`zones[]`) neben `zone_resolve` | → `ui/validation/ui_hub_validation.c` (nur Hub-Limits) |
| `ui_main.c` WM_COMMAND | 560 Zeilen Rest nach V8.7-Split | → `ui_config_command.c` |

`plugin.h`-Globals bleiben **Phase V8.12** (F10-Migration auf `g_config` — großes Verhaltenstest-Risiko).

---

## V8.10 — Mechanische Schichten-Sanierung

**Golden Rule:** reine Verschiebe-/Split-Pakete, null Verhaltensänderung.

| Paket | Aktion | Gate |
|-------|--------|------|
| 8.10.1 | `ui_config_on_command` → `ui_config_command.c` | Cross-Build |
| 8.10.2 | `config_files.c` → `ui/config/ui_voice_presets.c` | Cross-Build + Tests |
| 8.10.3 | `util_base.c` split (key util, display util, chat queue) | Cross-Build + Tests |
| 8.10.4 | Layering-Allowlist leeren (`check_layering.sh`) | layering_guard |

**Nicht in V8.10:** `plugin.h`-Globals abbauen, `zones[]`-Mirror entfernen.

---

## V8.11 — Zonen-Deduplizierung + Toter Code

| Paket | Aktion | Gate |
|-------|--------|------|
| 8.11.1 | Hub-Distanz-Limits → `ui/validation/ui_hub_validation.c` | Tests |
| 8.11.2 | `resolvePlayerZoneIndex` nutzt `zone_resolve` + `server_profile_get` | Tests |
| 8.11.3 | `getPlayerZone` + Polygon-Code aus `core/` entfernen | Cross-Build |
| 8.11.4 | `getServerHashForTracking` löschen (never called) | Cross-Build |

**Bewusst behalten:** `zones[]` in `plugin_ui_compat.c` als **UI-Mirror** (F10-Namen, pro-Zonen-Distanzen, Overlay-Text). Wird aus Hub synchronisiert; kein zweiter Polygon-Test mehr.

---

## Agent-Routing

| Phase | Effort | Subagent |
|-------|--------|----------|
| V8.10 | S2 | generalPurpose (mechanical moves) |
| V8.11 | S3 | generalPurpose (zone path — behavior-sensitive) |
| Abschluss | S4 | bugbot |

---

## Verifikation

```bash
bash tests/run_tests.sh
bash build/build_mingw.sh
```

Manuell (weiterhin offen): TS-Client F10, Presets, Zonen-Overlay.
