# 066 — Version 8.0.8: Release (HRTF, Okklusions-Prototyp, Root-Channel-Schalter)

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.8` (vorher `8.0.7`) |
| `README.md` | Hash-Tabelle um 8.0.8 ergaenzt (8.0.7/8.0.6 bleiben), Pruef-Befehle auf 8.0.8 |
| `doku/aenderungen/066-v8-0-8-release.md` | Dieser Eintrag (inkl. Changelog) |

## Changelog 8.0.7 → 8.0.8

| Nr. | Was | Standard |
|---|---|---|
| 062 | **HRTF fuer Kopfhoerer:** echter 3D-Klang (links/rechts, vorne/hinten, oben/unten) nach Brown & Duda, im Post-Process-Schritt nach dem TS-3D; Lautstaerke bleibt gleich | aus |
| 062 | **Diagnose:** Logzeile `AUDIO-DIAG:` zeigt einmalig, welche Kanalzahl/Lautsprecher TeamSpeak liefert | immer (nur Log) |
| 063 | **HRTF-Schalter im Root-Channel:** `[GLOBAL]` → `EnableHrtf=True` (nicht mehr in `plugin.cfg`) | aus |
| 061/064 | **Okklusion durch Waende/Fundamente (Prototyp, Plugin-Seite):** nimmt `OCC=`-Werte der Mod an, antwortet mit Probe-Punkten, daempft Lautstaerke (bis −12 dB) + Tiefpass (bis 900 Hz); Schalter `[GLOBAL]` → `EnableOcclusion=True` | aus |
| 065 | Mod-Seite der Okklusion dokumentiert (Line Traces in `Vo_PosStreamer_Actor`) | — |

Beispiel fuer Server-Admins (Root-Channel-Beschreibung):

```
[GLOBAL]
EnableHrtf=True
EnableOcclusion=True
```

**Wichtig:** Die Okklusion braucht zusaetzlich die **aktualisierte Conan-Mod** (sendet `OCC=`).
Mit der bisherigen Mod passiert bei `EnableOcclusion=True` nichts (kein Fehler).

## 2. Wie war es vorher?

8.0.7 war inhaltlich gleich 8.0.6 (nur Versionsnummer + `SHA256SUMS`). Richtung kam nur
vom einfachen TS-3D-Panorama; Waende hatten keinen Einfluss.

## 3. Warum ist die neue Loesung besser?

Neue Funktionen sind **standardmaessig aus** und werden **zentral vom Server** im
Root-Channel eingeschaltet. Bestehende Server verhalten sich nach dem Update genauso wie
vorher.

## 4. Wie funktioniert der Release-Ablauf? (wie 060)

```
Version 8.0.8 (ts3_entry.c) → .\build.ps1 (DLL + .ts3_plugin + Deploy)
  → SHA256SUMS erzeugen, sha256sum -c pruefen → README-Hashes → Commit/Push
  → gh release create v8.0.8 --draft <paket> SHA256SUMS → Digests pruefen → veroeffentlichen
```

Gemessen (Build 2026-10-18, Version 8.0.8):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.8-win64.ts3_plugin` (532130 Bytes) | `fa9b076b2aa35231a1f1e9787c7a8fdc3b490899cc1058cd05120fc50180deec` |
| `plugins/conan_exiles.dll` (7378944 Bytes) | `77e72008f15703032e9b5659197c0dae3bf9078b0be99ae9139b52fda1d43b68` |

## 5. Wie wurde es getestet?

- MSBuild Release|x64 exit 0, Paket enthaelt `package.ini` (`Version = 8.0.8`) und
  `plugins/conan_exiles.dll` (Forward-Slash), DLL im Paket = `bin\conan_exiles.dll` = deployte DLL.
- `sha256sum -c SHA256SUMS`: beide Zeilen `OK`.
- Unit-Tests (MSVC): `hrtf_test`, `hub_parser_test`, `occlusion_test` gruen; `check_layering.sh` OK.
- DLL-Export `ts3plugin_onEditPostProcessVoiceDataEvent` vorhanden.
- **Noch nicht** im TeamSpeak-Client mit Mitspielern getestet (HRTF hoeren, Okklusion mit neuer Mod).

## 6. Lerneffekt

Neue Audio-Funktionen hinter Server-Schalter mit Standard „aus“ ausliefern: so kann ein
Release frueh raus, ohne bestehende Server zu veraendern.
