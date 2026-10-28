# 090 — Paketname ohne Version: `conan_exiles-win64.ts3_plugin`

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `build.ps1` | `New-Ts3PluginPackage` schreibt immer `bin\conan_exiles-win64.ts3_plugin` (Ausweichname bei gesperrter Datei: `conan_exiles-win64-new.ts3_plugin`) |
| `doku/03-build-und-tests.md` | Hinweis auf den neuen Dateinamen |
| `doku/aenderungen/090-paketname-ohne-version.md` | Dieser Eintrag |

**Eine Funktion:** nur der Paketname in `New-Ts3PluginPackage`. Die Version steht weiter in
`package.ini` (`Version = x.y.z`) und in `ts3plugin_version()`.

---

## 2. Wie war es vorher?

Der Name enthielt die Version: `conan_exiles-8.0.15-win64.ts3_plugin`. Mit jeder neuen
Version entstand eine neue Datei in `bin\`, und jeder Download-Link, jede Anleitung
und jeder Befehl (`Get-FileHash`, `sha256sum`) musste den Namen mitaendern.

---

## 3. Warum ist die neue Loesung besser?

- Ein fester Name: Anleitungen und Befehle bleiben gleich.
- `bin\` sammelt nicht mehr pro Version eine weitere Paketdatei; der naechste Build
  ersetzt einfach die alte.
- Fuer GitHub: Der Link `…/releases/latest/download/conan_exiles-win64.ts3_plugin`
  zeigt immer auf die aktuelle Version.

Die TeamSpeak-Installation haengt nicht am Dateinamen: Der Package Installer liest
Name und Version aus `package.ini` im Paket.

---

## 4. Wie funktioniert es?

```
build.ps1 → liest Version aus ts3plugin_version() → package.ini (Version = x.y.z)
          → bin\conan_exiles-win64.ts3_plugin        (ZIP: package.ini + plugins/conan_exiles.dll)
```

**Gilt ab dem naechsten Build.** Bereits veroeffentlichte Releases (bis v8.0.15) behalten
ihre versionierten Dateinamen (GitHub-Releases sind unveraenderlich). Die README-Tabelle
und die Befehle nennen deshalb noch den Namen `conan_exiles-8.0.15-win64.ts3_plugin`;
sie werden mit dem naechsten Release auf den neuen Namen umgestellt (die Versionsspalte
bleibt, nur der Dateiname faellt weg). Alte versionierte Pakete in `bin\` bleiben liegen
und koennen von Hand geloescht werden.

---

## 5. Wie wurde es getestet?

- Den geaenderten Code von `New-Ts3PluginPackage` in einem Test-Ordner ausgefuehrt
  (mit einer Kopie von `bin\conan_exiles.dll`, ohne neu zu bauen):
  Ergebnis `conan_exiles-win64.ts3_plugin` (554580 Bytes), entpackt: `package.ini` +
  `plugins/conan_exiles.dll`.
- Kein kompletter `build.ps1`-Lauf (haette die DLL neu gebaut und die Hashes des
  Releases v8.0.15 verlassen). Kein Test im TS-Client.

---

## 6. Lerneffekt

Dateiname = stabile Adresse, Version = Inhalt. Die Version gehoert in `package.ini`
und in die Release-Tags, nicht in den Namen, auf den Anleitungen zeigen.
