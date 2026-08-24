# 039 — Plugins-Menue: Log-Ordner oeffnen

**Phase:** V8.17 · **Bezug:** TeamSpeak Menueleiste (API 26)

## Was wurde geaendert?

Unter **Plugins → Conan Exiles** gibt es jetzt einen dritten Menuepunkt:

3. **Log-Ordner oeffnen** — Explorer auf `Documents\Conan Exiles TeamSpeak plugin`
   (Ordner der Datei `plugin.log`)

Konkret:

- `src/ts/entry/ts3_info.c`: `MENU_ID_OPEN_LOG_FOLDER = 3`, dritter Eintrag in
  `ts3plugin_initMenus`, Handler in `ts3plugin_onMenuItemEvent`
- `src/ui/plugin_ui_compat.c/.h`: `open_plugin_log_folder()` — Pfad aus
  `log_get_path()`, Dateiname abgeschnitten, dann `ShellExecuteW`

## Wie war es vorher?

Die Log-Datei lag unter `Documents\Conan Exiles TeamSpeak plugin\plugin.log`,
war aber nur indirekt erreichbar (Explorer manuell oeffnen). Im Plugins-Menue
gab es nur **Einstellungen** und **Plugins-Ordner oeffnen**.

## Warum ist die neue Loesung besser?

1. **Schneller Support.** Nutzer und Admins finden Logs ohne Pfad nachschlagen.
2. **Gleiches Muster wie Plugins-Ordner.** Ein Klick, Explorer zeigt den Ordner
   (nicht die Log-Datei in einem Editor).
3. **Thread-sicher.** Kein TS-API-Aufruf, kein Blockieren — nur kurzes
   `ShellExecuteW` vom Callback-Thread, wie beim Plugins-Ordner.

## Wie funktioniert es jetzt?

```text
Plugins (TS Menueleiste)
  └─ Conan Exiles (Plugin-Name)
       ├─ Einstellungen
       ├─ Plugins-Ordner oeffnen
       └─ Log-Ordner oeffnen
             onMenuItemEvent(MENU_ID_OPEN_LOG_FOLDER)
               → open_plugin_log_folder()
               → log_get_path()  →  …\plugin.log
               → letztes '\' entfernen  →  Ordnerpfad
               → ShellExecuteW(open, Ordner)
```

**Datenfluss:** `log_get_path()` liefert den vollen Pfad zur Log-Datei (wird
beim ersten Log-Schreiben unter Dokumente angelegt). `open_plugin_log_folder`
kopiert ihn in einen Puffer, schneidet `\plugin.log` ab und oeffnet den
uebergeordneten Ordner im Explorer.

## Wie wurde es getestet?

- Build nicht ausgefuehrt (Nutzer-Anfrage).
- Manuell nach Build/Deploy: TeamSpeak neu starten → **Plugins** → **Conan Exiles**
  → **Log-Ordner oeffnen** → Explorer zeigt `Documents\Conan Exiles TeamSpeak plugin`
  mit `plugin.log` (sofern das Plugin schon einmal geloggt hat).

## Lerneffekt

Wenn eine API einen **Dateipfad** liefert, Explorer aber einen **Ordner**
braucht, muss der Dateiname abgetrennt werden — sonst oeffnet Windows die
Datei statt des Ordners. Wiederverwendung von `log_get_path()` vermeidet
doppelte Pfad-Logik.
