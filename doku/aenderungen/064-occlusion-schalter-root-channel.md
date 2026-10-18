# 064 — Okklusion (Prototyp) per Root-Channel ein-/ausschaltbar

**Branch:** `test/occlusion-prototype` (enthaelt 061 Okklusion, 062/063 HRTF)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/hub/hub_parser.h/.c` | Neuer `[GLOBAL]`-Schluessel `EnableOcclusion=True/False` → `HubSettings.enableOcclusion` (Standard 0). |
| `src/core/proximity/occlusion.h/.c` | Server-Schalter als Atom: `occlusion_set_enabled()` / `occlusion_is_enabled()`. Ausschalten loescht alle gespeicherten Werte. |
| `src/ts/profile/ts3_server_profile.c` | `server_profile_apply` setzt den Schalter aus dem Root-Channel, `server_profile_reset` schaltet aus; Logzeile `PROFILE: applied ... occlusion=`. |
| `src/core/http/pos_http_server.c` | `OCC=`-Werte werden nur gespeichert und Probe-Punkte nur zurueckgegeben, wenn der Schalter an ist. |
| `src/ts/proximity/ts3_proximity_audio.c` | Daempfung nur bei eingeschaltetem Schalter. |
| `tests/occlusion_test.c`, `tests/hub_parser_test.c` | Neue Checks (Standard aus, an/aus, Ausschalten vergisst Werte, unabhaengig von `EnableHrtf`). |

## 2. Wie war es vorher?

Der Prototyp aus 061 war immer aktiv, sobald die Mod `OCC=` mitschickte. Ein Server
konnte ihn nicht abschalten.

## 3. Warum ist die neue Loesung besser?

- Der **Server-Admin** entscheidet zentral (Root-Channel), genau wie bei `EnableHrtf`.
- **Aus = wirklich aus:** Das Plugin antwortet ohne Probe-Liste. Die Mod bekommt dann
  keine Punkte und schiesst keine Strahlen — auch die Spiel-CPU wird nicht belastet.
- Alte Server ohne Eintrag: aus.
- Layering bleibt sauber: `core/http` darf `ts/profile` nicht einbinden, deshalb haelt
  das `core`-Modul `occlusion` das Atom, und das Server-Profil (ts/) setzt es.

## 4. Wie funktioniert es jetzt?

```
Root-Channel [GLOBAL]        Callback-Thread                 HTTP-Thread                  Callback (Recompute)
EnableOcclusion=True  --->   server_profile_apply  --->      POST mit OCC=...             audio_compute_client
                             occlusion_set_enabled(1)        occlusion_is_enabled()?      occlusion_is_enabled()?
                                                               ja: Werte speichern,          ja: Gain/Tiefpass daempfen
                                                               Probe-Liste antworten
                                                               nein: nur {"ok":true}
```

Beispiel Root-Channel-Beschreibung:

```
[GLOBAL]
EnableHrtf=True
EnableOcclusion=True
```

## 5. Wie wurde es getestet?

- `occlusion_test` (MSVC): alle Checks gruen, inkl. neuem Schalter-Block.
- `hub_parser_test` (MSVC): alle gruen, inkl. `EnableOcclusion`.
- MSVC Release-Build des Branches OK.
- Ende-zu-Ende erst mit der Mod-Seite moeglich (Probe lesen, Line Traces, `OCC=` senden).

## 6. Lerneffekt

Ein Schalter, der Arbeit auf einem anderen System (hier: Line Traces in der Mod)
ausloest, sollte am Anfang der Kette sitzen — wer keine Probe-Punkte bekommt, rechnet
nichts.
