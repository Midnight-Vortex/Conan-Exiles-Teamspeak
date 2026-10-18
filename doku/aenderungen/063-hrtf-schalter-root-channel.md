# 063 — HRTF-Schalter in den Root-Channel statt `plugin.cfg`

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/hub/hub_parser.h/.c` | Neuer `[GLOBAL]`-Schluessel `EnableHrtf=True/False` → `HubSettings.enableHrtf` (Standard 0). |
| `src/ts/profile/ts3_server_profile.c/.h` | Neues Atom `g_hrtfEnabled` (gesetzt in `server_profile_apply`, geloescht in `server_profile_reset`), lock-freier Getter `server_profile_get_hrtf_enabled()`; Wert steht in der `PROFILE: applied`-Logzeile (`hrtf=`). |
| `src/ts/proximity/ts3_proximity_audio.c` | Post-Process-HRTF und `AUDIO-DIAG` lesen den Server-Schalter statt `g_config.enableHrtf`. |
| `src/core/config/config.h/.c` | Schluessel `EnableHrtf` wieder **entfernt** (Stand vor 062). |
| `tests/hub_parser_test.c` | Neuer Block `[9]` (4 Checks). |

## 2. Wie war es vorher?

In 062 war der Schalter ein Eintrag in der **lokalen** `plugin.cfg` jedes Spielers.
Damit haette jeder Spieler selbst entschieden, und der Server-Betreiber konnte das
Verhalten auf seinem Server nicht einheitlich festlegen.

## 3. Warum ist die neue Loesung besser?

- Der **Server-Admin** steuert es zentral in der Root-Channel-Beschreibung — wie
  `RealisticAudio`, `FilterIntensity` und die Zonen. Alle Spieler auf dem Server
  hoeren gleich.
- Ohne Eintrag (alte Server) bleibt es **aus** → keine Ueberraschung nach dem Update.
- Der Audio-Thread liest nur ein Atom (`g_hrtfEnabled`) — kein Lock, keine Kopie der
  ganzen `HubSettings` pro Audioblock. Gleiches Muster wie `g_listenAddBits`.

## 4. Wie funktioniert es jetzt?

```
Root-Channel-Beschreibung          Callback-Thread                     Audio-Thread
[GLOBAL]                           hub_parse_settings                  ts3_audio_process_postprocess
EnableHrtf=True        ------->    server_profile_apply                  server_profile_get_hrtf_enabled()
                                     g_hrtfEnabled = 1 (atomar)  --->    = profileActive && g_hrtfEnabled
                                   server_profile_reset (Trennen)
                                     g_hrtfEnabled = 0
```

Beispiel fuer den Admin (Root-Channel-Beschreibung):

```
[GLOBAL]
RealisticAudio=True
EnableHrtf=True
```

## 5. Wie wurde es getestet?

- `hub_parser_test` mit MSVC: alle Checks gruen, neu: fehlt → aus, `True` → an,
  `enablehrtf = 1` mit `<br>`-Umbruechen → an, `False` → aus.
- MSVC Release-Build OK.
- TS-Client-Test: `EnableHrtf=True` in die Root-Beschreibung, Log zeigt
  `PROFILE: applied ... hrtf=1` und `AUDIO-DIAG ... | hrtf=1`.

## 6. Lerneffekt

Einstellungen, die das gemeinsame Spielerlebnis auf einem Server bestimmen, gehoeren
in die Server-Konfiguration, nicht in die lokale Datei jedes Spielers.
