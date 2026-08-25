# 048 — Package Installer nicht mehr nach jedem Deploy starten

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `build.ps1` | Standard: `.ts3_plugin` schreiben, **Installer nicht starten**; `-OpenPackage` nur zusammen mit `-SkipDeploy`; Deploy bricht ab, wenn Quelle oder Ziel-DLL **0 Bytes** hat |
| `doku/03-build-und-tests.md` | Installer-Start nicht mehr als Default beschrieben |
| `doku/aenderungen/046-ts3-plugin-paket.md` | Hinweis auf 048 |

**Eine Funktion:** lokalen Build nicht mehr den TeamSpeak-Package-Installer auf eine bereits kopierte (ggf. gesperrte) DLL loslassen.

---

## 2. Wie war es vorher?

Ab Änderung 046 startete `.\build.ps1` nach dem Paket **automatisch** den TeamSpeak 3 Package Installer (`Start-Process` auf der `.ts3_plugin`-Datei) **und** kopierte parallel die DLL nach `%AppData%\TS3Client\plugins\`.

Wenn TeamSpeak die alte `conan_exiles.dll` noch hielt, erzeugte der Installer eine **0-Byte-Datei**. Der Client zeigte trotzdem „Add-On successfully installed“ — das Plugin lud nicht mehr.

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Vorher (046) | Jetzt (048) |
|---|---|
| Jeder Build öffnet den Installer | Paket liegt in `bin\`; Doppelklick nur wenn der Nutzer das will |
| Installer + Deploy schreiben dieselbe DLL | Deploy kopiert die DLL; Installer nur mit `-SkipDeploy -OpenPackage` |
| 0-Byte-DLL unbemerkt | Deploy prüft Größe und bricht bei 0 Bytes ab |

Endnutzer-Weg bleibt: `.ts3_plugin` per Doppelklick / Download — **TeamSpeak vorher beenden (Tray → Quit)**.

---

## 4. Wie funktioniert es jetzt?

```
.\build.ps1
    MSVC → bin\conan_exiles.dll
         → bin\conan_exiles-<ver>-win64.ts3_plugin
         → Copy nach %AppData%\TS3Client\plugins\   (kein Installer)

.\build.ps1 -SkipDeploy -OpenPackage
    nur Paket + Package Installer  (TS muss beendet sein)
```

`-SkipInstaller` bleibt als No-Op-Schalter erhalten (Installer startet ohnehin nicht mehr per Default).

---

## 5. Wie wurde es getestet?

- 0-Byte-DLL in AppData durch Kopie der guten `bin\conan_exiles.dll` (7,3 MB) ersetzt, während TS beendet war.
- ZIP-Inhalt des Pakets geprüft: `package.ini` + `plugins\conan_exiles.dll` (7 365 632 Bytes) — Paket war nie leer, nur die Installer-Kopie.
- Build-Skript: Default startet keinen Installer mehr (manuell am Schalter-Logik geprüft). Vollständiger Rebuild in diesem Paket nicht nötig.

**Manuell:** TeamSpeak starten → Plugin in Extras → Plugins aktiv → F10.

---

## 6. Lerneffekt

Ein Installer, der in eine vom laufenden Prozess gesperrte Datei schreibt, kann die Datei auf 0 Bytes kürzen und trotzdem „Erfolg“ melden. Entwickler-Deploy (Copy) und Endnutzer-Installer dürfen nicht dieselbe Datei gleichzeitig anfassen.
