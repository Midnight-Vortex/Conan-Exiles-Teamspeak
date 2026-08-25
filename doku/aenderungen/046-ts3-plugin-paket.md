# 046 — `.ts3_plugin`-Paket nach jedem Release-Build

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `packaging/package.ini.in` | Vorlage für TeamSpeak-Paket (`Name`, `Type=Plugin`, `Platforms=win64`, Version-Platzhalter) |
| `build.ps1` | Nach erfolgreichem Rebuild: ZIP mit `package.ini` + `plugins/conan_exiles.dll` → `bin/conan_exiles-<Version>-win64.ts3_plugin` |
| `doku/03-build-und-tests.md` | Paket-Schritt und Installer-Hinweis |

**Eine Funktion:** nach dem Build ein TeamSpeak-Installer-Paket erzeugen. Kein Versions-Bump, keine DLL-Logik.

Neue Schalter:

- `-SkipPackage` — kein `.ts3_plugin` (nur DLL wie bisher)
- `-SkipInstaller` — Paket schreiben, aber den **TeamSpeak 3 Package Installer** nicht starten

Nach jedem neuen Rebuild startet das Skript den Installer automatisch (wie Doppelklick / Firefox „Öffnen mit“). TeamSpeak sollte vorher beendet sein, sonst sperrt die alte DLL.

---

## 2. Wie war es vorher?

Der Build kopierte nur `conan_exiles.dll` nach `bin\` und optional nach `%AppData%\TS3Client\plugins\`. Es gab keine `.ts3_plugin`-Datei. Installation per Doppelklick (wie bei Addons von addons.teamspeak.com, z. B. Soundboard) war nicht möglich.

---

## 3. Warum ist die neue Lösung besser?

`.ts3_plugin` ist intern ein **ZIP**, das der TeamSpeak-**Package Installer** (`package_inst.exe`) kennt. Windows/Firefox öffnen die Datei damit — gleicher Weg wie offizielle Plugins. Kein manuelles Kopieren der DLL nötig, sobald man die Paketdatei ausführt.

Die DLL-Kopie nach AppData bleibt für schnelle Entwickler-Runden erhalten.

---

## 4. Wie funktioniert es jetzt?

```
MSVC Rebuild  →  bin\conan_exiles.dll
                      │
                      ▼
              staging\
                package.ini          (Version aus ts3plugin_version() in ts3_entry.c)
                plugins\
                  conan_exiles.dll
                      │
                      ▼  ZIP, Inhalt auf Archive-Root (kein Extra-Ordner)
              bin\conan_exiles-8.0.4-win64.ts3_plugin
                      │
                      ▼  Doppelklick / -OpenPackage
              TeamSpeak 3 Package Installer
```

**Wichtig:** Im ZIP müssen `package.ini` und `plugins/` direkt im Wurzelverzeichnis liegen — nicht in einem Unterordner `conan_exiles-…\`. Sonst findet der Installer die Dateien nicht.

`bin/` bleibt gitignored; das Paket ist ein Build-Artefakt, kein Commit.

---

## 5. Wie wurde es getestet?

- [ ] `.\build.ps1 -SkipDeploy` → `bin\conan_exiles-*-win64.ts3_plugin` existiert
- [ ] ZIP-Inhalt (umbenennen zu `.zip` und öffnen): `package.ini` + `plugins\conan_exiles.dll` auf oberster Ebene
- [ ] Doppelklick → TeamSpeak 3 Package Installer (TS vorher beenden, sonst sperrt die alte DLL)
- [ ] `.\build.ps1 -SkipDeploy` startet denselben Installer; `-SkipInstaller` unterdrückt nur den Start

---

## 6. Lerneffekt

TeamSpeak-Plugins verteilt man nicht als nackte DLL an Endnutzer, sondern als `.ts3_plugin` (ZIP + `package.ini`). Die Dateiendung ist die Assoziation zum Package Installer — nicht der MIME-Typ, den der Browser anzeigt.
