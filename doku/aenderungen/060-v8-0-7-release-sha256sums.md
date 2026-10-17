# 060 — Version 8.0.7: Release mit SHA256SUMS-Datei

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/entry/ts3_entry.c` | `ts3plugin_version()` liefert `8.0.7` (vorher `8.0.6`) |
| `README.md` | Hash-Tabelle um 8.0.7 ergaenzt (8.0.6 bleibt als aeltere Zeile), Befehle auf 8.0.7, Hinweis auf `SHA256SUMS` |
| `doku/aenderungen/060-v8-0-7-release-sha256sums.md` | Dieser Eintrag |

**Kein Funktionscode geaendert:** Der Quellcode ist bis auf die Versionsnummer identisch
mit 8.0.6.

---

## 2. Wie war es vorher?

Release `v8.0.6` enthielt nur das `.ts3_plugin`. Eine `SHA256SUMS`-Datei zum
Herunterladen sollte nachtraeglich dazu — das geht nicht: GitHub-Releases dieses
Repos sind **unveraenderlich** (immutable). Nach dem Veroeffentlichen lassen sich
weder Dateien hinzufuegen noch der Tag wiederverwenden (Upload-Fehler HTTP 422
„Cannot upload assets to an immutable release“).

---

## 3. Warum ist die neue Loesung besser/stabiler?

Jeder Release enthaelt jetzt neben dem Paket eine `SHA256SUMS`-Datei im
Standardformat von `sha256sum`. Anwender koennen mit `sha256sum -c --ignore-missing
SHA256SUMS` pruefen, ohne Hashes abzutippen. Und der Ablauf ist so, dass der
Fehler von 8.0.6 nicht wieder passiert (siehe 4.).

---

## 4. Wie funktioniert es?

```
Version anheben (ts3_entry.c)
   → .\build.ps1 -SkipDeploy      (DLL + .ts3_plugin in bin\, AppData bleibt unberuehrt)
   → sha256sum --text <paket> conan_exiles.dll > SHA256SUMS ; sha256sum -c pruefen
   → README-Hashes aktualisieren, committen, pushen
   → gh release create vX.Y.Z --draft <paket> SHA256SUMS ...   (Draft: noch aenderbar)
   → Digest der Draft-Dateien mit README vergleichen
   → gh release edit vX.Y.Z --draft=false                      (ab jetzt unveraenderlich)
```

Gemessen (Build 2026-10-17, Version 8.0.7):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.7-win64.ts3_plugin` (526056 Bytes) | `4345ab9d6b243ef280f5bab0afaa4372fe432d2f48da36d931dd2ac8a5ce6969` |
| `plugins/conan_exiles.dll` (7367680 Bytes) | `9debfd6974ff80b15cad0e0ae1a97593b11b5c18c35410e22a781545f89a0124` |

Der Build ist nicht reproduzierbar, die Hashes unterscheiden sich daher von 8.0.6,
obwohl der Code gleich ist. Die Hashes im README gelten fuer die hochgeladene Datei.

---

## 5. Wie wurde es getestet?

- `build.ps1 -SkipDeploy`: MSBuild exit 0, Paket erzeugt, `package.ini` enthaelt `Version = 8.0.7`.
- Paket entpackt: DLL im Paket hat denselben Hash wie `bin\conan_exiles.dll`.
- `sha256sum -c SHA256SUMS`: beide Zeilen `OK`.
- **Nicht** im TeamSpeak-Client getestet; es wurde nicht nach `%APPDATA%` deployed.

---

## 6. Lerneffekt

Releases mit allen Dateien als **Draft** vorbereiten und erst danach veroeffentlichen.
Ein veroeffentlichter Release ist bei diesem Repo nicht mehr aenderbar; ein
vergessenes Asset kostet eine neue Versionsnummer.
