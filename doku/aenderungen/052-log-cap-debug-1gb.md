# 052 — plugin.log Cap: 250 MB normal, 1 GB Debug

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `src/core/util/log.c` | Cap hängt an `log_is_enabled()`: **250 MB** ohne Debug, **1 GB** mit Debug |
| `src/core/util/log.h` | Kommentar angepasst |
| `doku/module/pos-http.md` | Größenangabe |

**Eine Funktion:** Log-Datei-Limit an den Debug-Modus koppeln.

---

## 2. Wie war es vorher (051)?

Ein festes Cap von **100 MB** — auch mit Debug an. Lange Debug-Sessions (Rate + POST-Zeilen) liefen ins Truncate, obwohl man die Details gerade braucht. Ohne Debug war 100 MB etwas knapp als Puffer für Boot/Kanal/`POS: method`.

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Debug | Cap | Grund |
|---|---|---|
| aus | **250 MB** | Puffer für normale `log_write`-Zeilen, ohne dass die Datei endlos wächst |
| an | **1 GB** | Rate/POST-Debug darf länger laufen; 2 GB wäre unnötig groß für Documents |

Die Clear-Zeile nennt das Limit: `LOG: cleared (plugin.log exceeded 250 MB)` bzw. `1 GB`.

---

## 4. Wie funktioniert es jetzt?

Unter dem Log-Lock, vor dem Anhängen:

```
debug an?  →  Limit = 1 GB
sonst      →  Limit = 250 MB
Größe >= Limit → Datei leeren → Clear-Zeile → aktuelle Zeile
```

Kein TS-API, kein neuer Config-Schlüssel.

---

## 5. Wie wurde es getestet?

- Nicht gebaut in diesem Paket.
- Nach Build: ohne Debug bleibt das Log klein (keine 1-Hz-Rate). Mit Debug darf es bis 1 GB wachsen.
- Truncate nur prüfbar, wenn die Datei das jeweilige Limit erreicht.

---

## 6. Lerneffekt

Ein Limit muss zum Betriebsmodus passen: Debug braucht Kopf, Produktion braucht eine kleinere Bremse — nicht denselben Wert für beides.
