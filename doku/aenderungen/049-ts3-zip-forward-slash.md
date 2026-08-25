# 049 — `.ts3_plugin` ZIP mit Forward-Slash (keine 0-Byte-DLL)

## 1. Was wurde geändert?

| Datei | Änderung |
|---|---|
| `build.ps1` (`New-Ts3PluginPackage`) | ZIP per Windows `tar.exe` (Forward-Slash `plugins/conan_exiles.dll`); nicht `.NET CreateFromDirectory` |

**Eine Funktion:** das TeamSpeak-Paket so packen, dass `package_inst.exe` die DLL wirklich entpackt.

---

## 2. Wie war es vorher?

`.NET` `CreateFromDirectory` schreibt unter Windows ZIP-Pfade mit **Backslash**: `plugins\conan_exiles.dll`. 7-Zip zeigt die Datei trotzdem mit 7 MB. Der TeamSpeak **Package Installer** folgt der ZIP-Spezifikation (nur `/`) und legt `conan_exiles.dll` im Plugins-Ordner als **0 Bytes** an — Dialog „successfully installed“ trotzdem.

---

## 3. Warum ist die neue Lösung besser/stabiler?

| Vorher | Jetzt |
|---|---|
| `plugins\conan_exiles.dll` im ZIP | `plugins/conan_exiles.dll` |
| Installer → 0-Byte-DLL | Installer schreibt die echten 7 MB |
| Nur manuelles `Copy-Item` funktionierte | Doppelklick auf `.ts3_plugin` (TS beendet) funktioniert |

---

## 4. Wie funktioniert es jetzt?

```
staging\
  package.ini
  plugins\conan_exiles.dll     (Windows-Pfad auf der Platte)
        │
        ▼  tar.exe -a -cf  (Eintragsname plugins/conan_exiles.dll)
conan_exiles-<ver>-win64.ts3_plugin
  package.ini
  plugins/conan_exiles.dll     (ZIP-Name mit / )
        │
        ▼  package_inst.exe
%AppData%\TS3Client\plugins\conan_exiles.dll
```

Alles im Build-Skript, nach dem MSVC-Rebuild. Kein TS-API, kein Plugin-Code.

---

## 5. Wie wurde es getestet?

- Vorher: ZIP-Listing `name=[plugins\conan_exiles.dll] slash=True`; nach Installer AppData-DLL 0 Bytes.
- Nach dem Fix: ZIP-Listing muss `plugins/conan_exiles.dll` und `slash=False` zeigen; AppData-Größe = Quell-DLL.
- TeamSpeak vorher beenden (Tray → Quit), dann `.ts3_plugin` doppelklicken.

---

## 6. Lerneffekt

Windows-ZIP-APIs und 7-Zip verzeihen `\`. Der Ziel-Installer oft nicht — ZIP-Einträge immer mit `/` schreiben und nach dem Packen die Eintragsnamen prüfen, nicht nur die entpackte Ansicht in 7-Zip.
