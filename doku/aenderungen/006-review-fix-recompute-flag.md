# 006 — Review-Fix: Recompute-Anforderung konnte verloren gehen

## Was wurde geaendert?

`src/ts/proximity/ts3_proximity_audio.c`, Funktion `audio_recompute_all_impl()`:
Das Pending-Flag `g_recomputeAllPending` wird jetzt **am Anfang** der Berechnung
geloescht ("claimed"), nicht mehr am Ende.

## Wie war es vorher?

Der Ablauf war: Eingaben lesen → alle Spieler durchrechnen → **dann** Flag loeschen →
publizieren. Kam waehrend des Durchrechnens eine NEUE Anforderung herein (z. B. der
Nutzer speichert im F10-Dialog neue Distanzen, waehrend gerade gerechnet wird), setzte
sie das Flag auf 1 — und das Loeschen am Ende **wischte genau diese neue Anforderung
weg**. Der naechste Aufwach-Zyklus sah "nichts zu tun", und die Lautstaerken blieben
veraltet, bis irgendein anderes Ereignis zufaellig einen Recompute ausloeste.

Gefunden hat das der **Bugbot-Review nach Phase V8.3** (gemaess Regel: S4-Review nach
jeder Phase). Durch unsere V8.3-Aenderung (UI loest Recompute nur noch per Flag aus)
lief mehr Verkehr ueber dieses Flag — der alte Fehler wurde dadurch wahrscheinlicher.

## Warum ist die neue Loesung besser?

Das ist das Standard-Muster fuer "Arbeit anfordern per Flag" (claim-then-work):

```
Anforderer:                Arbeiter:
Flag := 1                  Flag := 0        ← zuerst beanspruchen!
wecke Arbeiter             Eingaben lesen
                           rechnen + publizieren
```

Kommt waehrend des Rechnens eine neue Anforderung, setzt sie das Flag wieder auf 1 —
und der naechste Durchlauf rechnet mit den neuen Eingaben. Es kann hoechstens einmal
"zu viel" gerechnet werden, aber nie eine Anforderung verloren gehen.

## Wie getestet?

`bash tests/run_tests.sh` (5 Suiten gruen) + `bash build/build_mingw.sh` (DLL baut).
Das Zeitfenster selbst ist nur mit echtem TS-Client + Last provozierbar; die Korrektheit
folgt aus dem Muster (Flag-Uebergaenge sind atomar via `InterlockedExchange`).

## Lerneffekt

Bei "Pending-Flag + Arbeiter"-Mustern gilt immer: **Flag zuerst loeschen, dann arbeiten.**
Wer das Flag erst nach der Arbeit loescht, loescht moeglicherweise eine Anforderung,
die er noch gar nicht bearbeitet hat. Und: Reviews nach jeder Phase lohnen sich —
dieser Fehler existierte schon in V7, wurde aber erst sichtbar, als V8 den Pfad
staerker nutzte.
