# 009 — CI: Sicherheitsnetz automatisch erzwingen

## Was wurde geaendert?

Neue Datei `.github/workflows/ci.yml`: Bei jedem Push und jedem Pull-Request laeuft
auf einem Ubuntu-Runner automatisch:

1. `bash tests/run_tests.sh` — alle gcc-Unit-Tests (aktuell 5 Suiten, 241 Checks)
2. `bash build/build_mingw.sh` — der komplette Windows-DLL-Cross-Build

Faellt eines davon durch, wird der Lauf rot.

## Wie war es vorher (V7)?

Es gab keinerlei automatische Pruefung. Tests liefen nur manuell auf einem einzigen
Windows-PC (mit hartkodiertem Visual-Studio-Pfad). Ob ein Commit ueberhaupt baut,
sah man erst, wenn jemand ihn zufaellig auf dem richtigen Rechner oeffnete.

## Warum ist das besser?

V8-Leitziel 6 ist "Testbarkeit als Fundament". Ein Sicherheitsnetz nuetzt nur, wenn
es **automatisch** laeuft — sonst vergisst man es unter Zeitdruck. Die CI macht die
zwei maschinellen Gates (Tests gruen + DLL linkt) fuer jeden Beitrag verbindlich,
auch fuer KI-Agents. Der eigentliche Release-Build bleibt der MSVC-Build unter
Windows (`build.ps1`); die CI ersetzt ihn nicht, sie **schuetzt** ihn vor kaputten Commits.

## Wie funktioniert es?

```
Push / PR ──▶ GitHub Actions (Ubuntu)
                 │ apt: gcc + MinGW-w64
                 │ tests/run_tests.sh   ─┐
                 │ build/build_mingw.sh ─┴─▶ beide gruen? ── ja ─▶ Haken ✓
                                                            └ nein ─▶ rot ✗
```

## Wie getestet?

Lokal wurde exakt derselbe Ablauf ausgefuehrt (`run_tests.sh` = 5 Suiten gruen,
`build_mingw.sh` = DLL baut). Der Workflow ruft dieselben Skripte auf.

Hinweis: Falls der Push dieser Workflow-Datei vom GitHub-Token abgelehnt wird
(App-Token ohne `workflows`-Recht), muss ein Mensch die Datei einmalig ueber die
GitHub-Oberflaeche hinzufuegen — Inhalt siehe oben.

## Lerneffekt

Ein Test, den man von Hand starten muss, wird irgendwann nicht mehr gestartet.
Erst die Automatisierung (CI) macht aus "wir koennten testen" ein "es wird immer getestet".
