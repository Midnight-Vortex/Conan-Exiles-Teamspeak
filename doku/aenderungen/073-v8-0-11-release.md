# 073 — Version 8.0.11: Release (Okklusion v2, Plugin-Seite)

Am 2026-10-19 als **Vorabversion** veroeffentlicht, am **2026-10-20 auf Wunsch des Owners zur regulaeren
Version („Latest“)** gemacht, zusammen mit der gekochten Mod (Mod-Seite: 074). Gleiche Dateien und Hashes;
nur Titel („v8.0.11 — Occlusion v2“), Release-Notizen und der Pre-release-Schalter wurden geaendert.

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.11` (vorher `8.0.10`) |
| `README.md` | Hash-Tabelle um 8.0.11 ergaenzt, Pruef-Befehle auf 8.0.11 |
| `doku/aenderungen/073-v8-0-11-release.md` | Dieser Eintrag (inkl. Changelog) |

## Changelog 8.0.10 → 8.0.11 (Plan und Protokoll: 072)

| Was | Wirkt mit bisheriger Mod? | Standard |
|---|---|---|
| **Glaettung (B1):** Dumpfer werden in ~0,5 s, klarer werden in ~0,25 s statt Sprung — „kurz um die Ecke“ springt nicht mehr | **ja** | `OcclusionAttackMs=500`, `OcclusionReleaseMs=250` |
| **Beugung (B2):** eigene Klangkurve „um die Ecke“ (−3 dB, Tiefpass 2,5 kHz) | nein, braucht neue Mod | `OcclusionDiffraction=100` |
| **Richtung aus der Oeffnung (B3):** Stimme kommt aus Richtung Tuer/Ecke | nein, braucht neue Mod | — |
| **Hall drinnen (B4):** Raum-Faktor steuert einen Raumhall | nein, braucht neue Mod | `OcclusionReverb=100` (0 = aus) |
| **Mod-Stellschrauben (B5):** 12 Werte `OccFanEdge` … `OccFast` im Root-Channel, das Plugin reicht sie als `occcfg` an die Mod weiter | — (die neue Mod liest sie) | siehe 072 |
| **Debug (C1):** mit `DebugMode` in `plugin.cfg` eine Zeile `OCC-DBG` pro Sprecher und Sekunde | ja | aus |

Abwaertskompatibel: liest das alte OCC-Format weiter; ohne neue Schluessel verhaelt sich alles wie 8.0.10
plus Glaettung.

## 2. Wie war es vorher?

Okklusionswerte wirkten sofort (nur die Klick-Rampe pro Audiopuffer). Drinnen sprang die Stimme beim Gang
um eine Ecke in 100–200 ms von klar auf voll gedaempft.

## 3. Release-Ablauf (wie 070, aber `--prerelease`)

```
Version 8.0.11 → .\build.ps1 -SkipDeploy → SHA256SUMS + sha256sum -c → README → Merge/Commit/Push
  → gh release create v8.0.11 --target <volle SHA> --draft --prerelease → Digests pruefen → veroeffentlichen
```

Gemessen (Build 2026-10-19, Version 8.0.11):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.11-win64.ts3_plugin` (537325 Bytes) | `1af09c3a707d040a86130052f3052892f1a7659a40d541069e570d9d910b31e6` |
| `plugins/conan_exiles.dll` (7388672 Bytes) | `5dcde1ce3762dfeaf976949cfff254e4accfcb8b303eba0db72a31c0a1f770a7` |

## 4. Wie wurde es getestet?

- Unit-Tests (MSVC): `occlusion_test` (v2-Format, occcfg, Parameter, Beugung, Glaettung, Raum-Glaettung),
  `hub_parser_test` (neue Schluessel, exakter Schluesselvergleich), `hrtf_test`, `talk_state_test` gruen;
  `check_layering.sh` OK; MSBuild Release|x64 ohne Warnungen.
- Paket: `Version = 8.0.11`, DLL im Paket = `bin\conan_exiles.dll`, `sha256sum -c` OK.
- **Noch nicht** im TeamSpeak-Client gehoert (Glaettung mit bisheriger Mod testbar; Beugung/Richtung/Hall
  erst mit der neuen Mod).
