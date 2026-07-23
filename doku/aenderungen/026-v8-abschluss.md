# 026 — V8-Abschluss: Version 8.0.0

**Phase:** V8.9

## Was wurde geaendert?

- **Version:** `ts3plugin_version()` liefert jetzt `"8.0.0"` (war `8.0.0-dev`).
- **plugin.h:** Legacy-Kommentar oben (wohin neuer Code gehoert); tote Symbole entfernt:
  `TEMP`, `f9CoordinateBroadcastActive`, `infoText1/2/3`, `lastHubDescriptionCache`
  (grep-bewiesen: nur Definition, kein Leser).
- **Alle V8-Phasen** in `REWRITE_PLAN_V8.md` als erledigt markiert.

## Was V8 insgesamt gebracht hat (Kurzueberblick)

| Bereich | V7-Problem | V8-Loesung |
|---------|------------|------------|
| Threads | Wakeup von jedem Thread | Eigener Wakeup-Thread (einziger Off-Callback-TS-API-Call) |
| PCM | Zwei Schreiber auf Rampen | Generation-Counter, Audio-Thread besitzt Zustand |
| Steuerung | Flag-Labyrinth | Zwei-Kanal-Design: koaleszierende Flags + typisierte Queue (doku/020) |
| Config | Zwei Writer auf plugin.cfg | Ein Writer (`g_config` / `config_save`) |
| Schichten | core/ included ts/ui | channel_manage + nick nach ts/, voice_modes Hooks, Layering-Wache |
| UI | 4197-Zeilen-Monolith | 6 Dateien + internes Header |
| Shutdown | Threads nicht gejoint | Feste Reihenfolge, alle Threads joinbar |
| Tests | 1 Test, nur Windows | 9 Host-Suiten + CI + CEPOS-Load-Sim |
| Doku | Keine | 26 Aenderungs-Eintraege + Architektur/Lessons |

## Was bewusst fuer den TS-Client-Test offen bleibt

Hoertest, F10-Speichern unter Last, 30-min-Lasttest mit echten Clients — maschinell
koennen wir Build + Unit-Tests + Load-Sim abdecken; Audio und TS-API-Verhalten
brauchen den echten Client (am Ende testen und fixen, wie vereinbart).

## Lerneffekt

Ein Rewrite ist "fertig", wenn Plan + Gates + Doku stimmen — nicht wenn jeder
Manuelltest schon gelaufen ist. Die Gates verhindern Regressionen; der Client-Test
validiert Erlebnis und Timing.
