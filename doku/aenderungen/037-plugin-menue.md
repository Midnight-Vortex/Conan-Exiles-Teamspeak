# 037 — Plugins-Menue: Einstellungen + Plugins-Ordner

**Phase:** V8.17 · **Bezug:** TeamSpeak Menueleiste (API 26)

## Was wurde geaendert?

Unter dem bestehenden TeamSpeak-Menue **Plugins** erscheint ein Eintrag fuer
dieses Plugin mit zwei Punkten:

1. **Einstellungen** — oeffnet denselben Dialog wie F10
2. **Plugins-Ordner oeffnen** — Explorer auf `%AppData%\TS3Client\plugins`

Konkret:

- `src/ts/entry/ts3_info.c`: `ts3plugin_initMenus` + `ts3plugin_onMenuItemEvent`
  (`PLUGIN_MENU_TYPE_GLOBAL`, IDs 1/2)
- `src/ts/entry/ts3_exports.h`: beide Callbacks exportiert
- `src/ui/input/key_watcher.c`: `settings_dialog_open_async()` oeffentlich
  (vorher nur intern fuer F10)
- `src/ui/plugin_ui_compat.c/.h`: `open_ts3_plugins_folder()`
- `project/Conan-Exiles-TeamSpeak.vcxproj`: `shell32.lib` fuer `ShellExecuteW`

## Wie war es vorher?

Einstellungen gingen nur ueber **F10** oder **Extras → Erweiterungen →
Plugin konfigurieren**. Es gab keinen Menuepunkt und keinen Schnellzugriff auf
den Plugins-Ordner.

## Warum ist die neue Loesung besser?

1. **SDK-konform.** API 26 erlaubt **kein** eigenes Top-Menue neben
   Verbindungen/Extras. `PLUGIN_MENU_TYPE_GLOBAL` haengt unter **Plugins** —
   das ist der offizielle Weg.
2. **Kein Blockieren der UI-Thread.** Einstellungen starten denselben
   `_beginthreadex`-Dialog-Thread wie F10; `showConfigInterface()` laeuft
   **nicht** direkt in `onMenuItemEvent`.
3. **Gleicher Dialog-Guard.** `config_dialog_try_open()` verhindert zwei
   Dialoge gleichzeitig (F10 / Menue / Extras).

## Wie funktioniert es jetzt?

```text
Plugins (TS Menueleiste)
  └─ Conan Exiles (Plugin-Name)
       ├─ Einstellungen
       │     onMenuItemEvent(MENU_ID_SETTINGS)
       │       → config_dialog_try_open()
       │       → settings_dialog_open_async()
       │       → showConfigInterface()  (eigener Thread)
       └─ Plugins-Ordner oeffnen
             onMenuItemEvent(MENU_ID_OPEN_PLUGINS_FOLDER)
               → open_ts3_plugins_folder()
               → ShellExecuteW(%APPDATA%\TS3Client\plugins)
```

**Thread-Vertrag:** `onMenuItemEvent` = TS-UI/Callback-Thread, darf nicht
blockieren. Ordner-Oeffnen ist kurz und non-blocking. Dialog = eigener Thread
wie F10.

## Wie wurde es getestet?

- Build Release|x64 (siehe Build-Lauf).
- Manuell: TeamSpeak neu starten → Menue **Plugins** → Plugin-Eintrag →
  Einstellungen oeffnen; zweiter Klick soll zweiten Dialog nicht starten;
  „Plugins-Ordner oeffnen“ zeigt Explorer mit `conan_exiles.dll`.

## Lerneffekt

Plugin-Menues in TeamSpeak 3 sind immer Untereintraege unter **Plugins**
(oder Kontextmenues), nie neue Top-Level-Dropdowns. Lange Arbeit (modaler
Dialog) muss vom Callback-Thread weg auf einen eigenen Thread.
