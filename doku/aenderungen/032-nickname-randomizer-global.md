# 032 — NicknameRandomizer in [GLOBAL]

**Phase:** V8.13 · **Version:** 8.0.3

## Ziel

Server-Admin kann im Root-Kanal unter `[GLOBAL]` steuern, ob Spieler beim
Wechsel in den Ingame-Kanal weiterhin eine zufällige Ziffern-Nickname bekommen
(bisheriges Verhalten) oder der echte Name erhalten bleibt.

## Konfiguration

In der **Root**-Kanalbeschreibung:

```
[GLOBAL]
NicknameRandomizer=True
```

| Wert | Verhalten |
|---|---|
| `True` / `1` | Wie bisher: 8–10 Zufallsziffern vor dem Ingame-Move |
| `False` / `0` | Keine Umbenennung — Nickname bleibt unverändert |
| *(Key fehlt)* | **`True`** (Abwärtskompatibel zu bestehenden Servern) |

## Code

- `HubSettings.nicknameRandomizer` in `hub_parser.h` / `hub_parser.c`
- Getter `server_profile_get_nickname_randomizer()` in `ts3_server_profile.c`
- `nick_anonymize_before_ingame()` bricht sofort ab, wenn deaktiviert
- Legacy-Spiegel `hubNicknameRandomizer` in `plugin_ui_compat.c`

## Tests

- `hub_parser_test`: Default `1`, explizit `True`/`False`
