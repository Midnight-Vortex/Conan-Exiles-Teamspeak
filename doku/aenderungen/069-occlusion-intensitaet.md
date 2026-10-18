# 069 — Okklusions-Staerke im Root-Channel (`OcclusionIntensity`)

**Branch:** `feature/occlusion-intensity`

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/core/hub/hub_parser.h/.c` | Neuer `[GLOBAL]`-Schluessel `OcclusionIntensity` (Prozent, 0..300, Standard 100) → `HubSettings.occlusionIntensity` |
| `src/core/proximity/occlusion.h/.c` | `occlusion_set_intensity()` / `occlusion_get_strength()` (atomar wie der Schalter); `occlusion_apply()` bekommt die Staerke als Parameter; neue Untergrenze `OCCLUSION_MIN_CUTOFF` = 250 Hz |
| `src/ts/profile/ts3_server_profile.c` | Setzt die Staerke beim Anwenden des Server-Profils, beim Reset wieder 100; Logzeile `PROFILE: applied` zeigt `occIntensity=` |
| `src/ts/proximity/ts3_proximity_audio.c` | Ruft `occlusion_apply` mit der Staerke auf |
| `tests/occlusion_test.c`, `tests/hub_parser_test.c` | Neue Checks (Staerke 0/1/1.25/2, Begrenzung, Parser) |

## 2. Wie war es vorher?

Die Daempfung bei voller Verdeckung war fest: −12 dB und Tiefpass bis 900 Hz. Im Test mit
Mitspielern klang es hinter Waenden nur „etwas dumpfer“ — zu schwach.

## 3. Wie funktioniert es?

```
[GLOBAL]
EnableOcclusion=True
OcclusionIntensity=100
```

`Staerke = OcclusionIntensity / 100`. Bei voller Verdeckung:

| OcclusionIntensity | Lautstaerke | Tiefpass |
|---|---|---|
| 0 | keine Daempfung | keiner |
| 50 | −6 dB | ~4100 Hz |
| **100 (Standard = bisheriger Stand 8.0.8)** | **−12 dB** | **900 Hz** |
| 125 | −15 dB | ~420 Hz |
| 150 | −18 dB | 250 Hz (Untergrenze) |
| 200 | −24 dB | 250 Hz |
| 300 (Maximum) | −36 dB | 250 Hz |

- Lautstaerke: Der bisherige Faktor wird mit der Staerke potenziert → der dB-Verlust waechst linear.
- Tiefpass: Die bisherige Kurve 19 kHz → 900 Hz wird weitergefuehrt, aber nie unter 250 Hz
  (darunter waere die Stimme nur noch Brummen). Ab etwa 150 wird also nur noch leiser, nicht dumpfer.
- Bei 100 rechnet das Plugin **bitgenau** wie 8.0.8 (Test prueft das).
- Werte ausserhalb 0..300 werden begrenzt; fehlt der Schluessel, gilt 100.

## 4. Warum so?

Ein Wert im Root-Channel statt fester Konstanten: Die Server-Admins koennen die Staerke beim
Testen sofort aendern (Beschreibung speichern → Plugin laedt das Profil neu), ohne neues Plugin.
Der Standard 100 aendert fuer bestehende Server nichts.

## 5. Wie wurde es getestet?

- `occlusion_test` (MSVC): Staerke 0 = nichts, 1 = bitgenau alte Formel, 2 = −24 dB und
  Cutoff an der Untergrenze, 1.25 dazwischen; Setzen/Begrenzen von `occlusion_set_intensity`.
- `hub_parser_test`: fehlend → 100, `200` gelesen, `999` → 300, `-20` → 0, Gross-/Kleinschreibung
  und `<br>`.
- `hrtf_test`, `talk_state_test` gruen; `check_layering.sh` OK; MSBuild Release|x64 exit 0.
- **Noch nicht** im TeamSpeak-Client mit Mitspielern gehoert.

## 6. Lerneffekt

Klang-Parameter, die man nur im echten Test beurteilen kann, gleich als Server-Wert anlegen —
dann braucht das Feintuning keinen neuen Build.
