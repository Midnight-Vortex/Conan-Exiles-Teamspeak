# Doku — Conan Exiles TeamSpeak Plugin (ab Version 8)

Willkommen! Dieser Ordner erklaert das Plugin so, dass auch Einsteiger folgen koennen.
Ab V8 gilt die **Doku-Pflicht** (`.cursor/rules/05-documentation/doku-pflicht.mdc`):
Jede Code-Aenderung bekommt hier einen Eintrag — was geaendert wurde, warum es
besser/stabiler ist als vorher, und wie es funktioniert.

## Aufbau

| Datei/Ordner | Inhalt |
|---|---|
| `00-projekt-ueberblick.md` | Was ist das Plugin ueberhaupt? Wie funktioniert Proximity-Voice? |
| `01-architektur-v8.md` | Die V8-Architektur: Threads, Datenfluesse, der Thread-Vertrag — Schritt fuer Schritt erklaert |
| `02-lessons-learned-v7.md` | Was in V7 gut und was schlecht lief (Audit 2026-07-23) und was wir daraus lernen |
| `03-build-und-tests.md` | Wie man baut und testet (Windows-MSVC, Linux-Cross-Build, Unit-Tests) |
| `module/` | Eine Seite pro Modul: Aufgabe, Funktionsweise, Thread-Vertrag |
| `aenderungen/` | Chronologisches Log aller V8-Arbeitspakete (was/warum/wie/Test/Lerneffekt) |

## Fuer wen ist das?

- **Anfaenger:** Fang bei `00-projekt-ueberblick.md` an, dann `01-architektur-v8.md`.
  Fachbegriffe werden beim ersten Auftreten kurz erklaert.
- **Mitentwickler:** `02-lessons-learned-v7.md` erklaert, warum V8 so gebaut ist, wie es
  gebaut ist. `aenderungen/` zeigt jede Entscheidung einzeln.
- **KI-Agents:** `AGENTS.md` + `.cursor/rules/` sind bindend; diese Doku ist die
  menschenlesbare Begruendung dahinter.

## Grundregeln der Doku

- Sprache: **Deutsch**, einfach gehalten. Code, Kommentare und Commits bleiben Englisch.
- Doku wird im **selben Commit** wie der Code gepflegt.
- Ein Arbeitspaket ohne Doku-Eintrag gilt als **nicht fertig**.
