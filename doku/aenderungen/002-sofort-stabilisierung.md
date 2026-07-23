# 002 — Sofort-Stabilisierung (V8.2)

Arbeitspaket V8.2: vier kleine, risikoarme Fixes ohne Architektur-Umbau.
Ziel: bekannte Stabilitaets-Luecken schliessen, bevor groessere Threading-Aenderungen
(V8.3+) beginnen.

---

## Fix 1 — Unmute-Batch-Cap angleichen

### Was wurde geaendert?

- `src/ts/proximity/ts3_proximity_audio.c`: `TS3_UNMUTE_BATCH_MAX` von **128** auf **64**
  gesetzt (effektives Limit pro Flush-Zyklus: **63** Clients).

### Wie war es vorher (V7)?

`ts3_audio_flush_unmutes` sammelte bis zu **127** Client-IDs pro Durchlauf und rief
`ts3_unmute_clients_for_pcm` auf. Diese Adapter-Funktion baut intern aber nur ein
**64-Element-Array** (63 IDs + Null-Terminator fuer die TS-API) und schneidet alles
darueber **still ab** (`n = count > 63 ? 63 : count`). Clients ab Index 63 wurden
aus der Batch-Liste entfernt, ihre Pending-Flags aber trotzdem geleert — sie blieben
in TeamSpeak **stumm**, obwohl das Plugin dachte, sie seien entmutet.

### Warum ist die neue Loesung besser?

Flush und Adapter haben jetzt dasselbe Limit. Es kann **kein Client mehr verloren**
gehen, weil der Flush nie mehr IDs sammelt, als der Adapter tatsaechlich sendet.
Ueberzaehlige Pending-Eintraege bleiben fuer den **naechsten CEDRAIN-Zyklus** stehen.

### Wie funktioniert es jetzt?

```
Audio-Thread setzt Pending-Flag + Ring-Eintrag
        │
        ▼
CEDRAIN ruft ts3_audio_flush_unmutes (Callback-Thread)
        │
        ├─ sammelt max. 63 IDs (TS3_UNMUTE_BATCH_MAX - 1)
        ├─ ts3_unmute_clients_for_pcm sendet alle 63 an TS-API
        ├─ nur erfolgreich entmutete IDs: Flag loeschen
        └─ noch Pending? → ts3_request_wakeup() → naechster Zyklus
```

### Wie wurde es getestet?

- `bash tests/run_tests.sh` — alle Suites gruen
- `bash build/build_mingw.sh` — `bin/mingw/conan_exiles.dll` OK
- Manuell im TS-Client (empfohlen): viele Spieler in Reichweite → niemand bleibt
  dauerhaft TS-muted, wenn Proximity-Unmute faellig waere.

### Lerneffekt

Wenn zwei Schichten dieselbe Datenmenge verarbeiten (Flush sammelt, Adapter sendet),
muessen ihre **Grenzwerte identisch** sein — sonst entstehen stille Datenverluste,
besonders wenn Erfolgs-Flags schon vor dem tatsaechlichen API-Call gesetzt werden.

---

## Fix 2 — CEDRAIN erkennt Position-Updates

### Was wurde geaendert?

- `src/core/channel/channel_manage.c`: `chan_has_pending_work()` prueft zusaetzlich
  `g_positionUpdatePending` (via `InterlockedCompareExchange`).

### Wie war es vorher (V7)?

`chan_has_pending_work` lieferte nur `g_moveInFlight != 0`. Das Flag
`g_positionUpdatePending` (gesetzt durch `chan_signal_position_update` bei neuer
Spielerposition) wurde **ignoriert**. In `ts3_entry.c` bricht CEDRAIN frueh ab,
wenn **kein** Modul Pending-Work meldet — ein Wakeup nur fuer Positions-Update konnte
so **verschluckt** werden. Hub↔Ingame Auto-Move verzoegerte sich um mindestens einen
Poll-Zyklus.

### Warum ist die neue Loesung besser?

CEDRAIN laeuft durch, wenn entweder ein Kanal-Move laeuft **oder** eine Positions-Aktualisierung
ansteht. Position-getriebene Kanal-Logik (`chan_tick_position_update`) wird nicht mehr
durch ein unvollstaendiges Early-Out blockiert.

### Wie funktioniert es jetzt?

```
Pos-Watcher / CEPOS → chan_signal_position_update()
        │ setzt g_positionUpdatePending = 1
        │ ts3_request_wakeup()
        ▼
CEDRAIN in ts3_entry.c
        │
        ├─ chan_has_pending_work()?  → JA (position pending)
        ├─ chan_tick_position_update() laeuft
        └─ g_positionUpdatePending wird dort zurueckgesetzt
```

### Wie wurde es getestet?

- `bash tests/run_tests.sh` — gruen
- `bash build/build_mingw.sh` — gruen
- Manuell: Spieler bewegt sich zwischen Hub- und Ingame-Zone → Kanalwechsel
  reagiert ohne merkliche Verzoegerung.

### Lerneffekt

Bei einem Mega-Drain mit Early-Out muessen **alle** Pending-Flags in der
„Hat noch Arbeit?“-Abfrage auftauchen — sonst verliert man Wakeups und die
Reaktionszeit haengt vom naechsten Zufalls-Poll ab.

---

## Fix 3 — Doppeltes `createPresetsCategory()` entfernt

### Was wurde geaendert?

- `src/ui/dialogs/ui_main.c`: **erster** Aufruf von `createPresetsCategory()` im
  `WM_CREATE`-Handler entfernt (~Zeile 2625). Der zweite Aufruf vor
  `loadPresetsFromConfigFile()` bleibt.

### Wie war es vorher (V7)?

`createPresetsCategory()` wurde **zweimal** in `WM_CREATE` aufgerufen. Jeder Aufruf
erzeugt per `CreateWindowW` alle Preset-Steuerelemente neu. Beim zweiten Aufruf
ueberschreiben die globalen `HWND`-Variablen die ersten — die ersten Fenster sind
**Waisen** (orphaned): doppelte HWNDs, moegliches Ueberzeichnen, unnoetiger Speicher.

Zwischen den beiden Aufrufen wurde **kein** Preset-HWND gelesen (nur Distanz-Felder
und Buttons), daher ist das Entfernen des ersten Aufrufs sicher.

### Warum ist die neue Loesung besser?

Genau **ein** Satz Preset-Controls pro Dialog-Oeffnung; `loadPresetsFromConfigFile`
laedt in die richtigen (einzigen) HWNDs.

### Wie funktioniert es jetzt?

```
WM_CREATE
  … Distanz-Felder, Buttons …
  createPresetsCategory()     ← einmal
  loadPresetsFromConfigFile() ← fuellt Preset-Felder
  ShowCategoryControls(1)
```

### Wie wurde es getestet?

- Cross-Build gruen
- Manuell: F10 → Kategorie Presets → Felder sichtbar, Werte aus Config geladen,
  kein Flackern/Doppelzeichnen.

### Lerneffekt

Doppelte `CreateWindow`-Aufrufe in Init-Pfaden sind leicht zu uebersehen und erzeugen
schwer debugbare UI-Artefakte — grep nach Funktionsnamen im selben Handler hilft.

---

## Fix 4 — Tote Legacy-Arrays und Stub-Funktionen entfernt

### Was wurde geaendert?

**Entfernte BSS-Arrays** (nur Definition + `extern`, nirgends gelesen/geschrieben):

| Array / Zaehler | Datei |
|-----------------|-------|
| `remotePlayersData[512]`, `remotePlayerCount` | `plugin_ui_compat.c`, `plugin.h` |
| `playerMuteStates[512]`, `playerMuteStateCount` | `plugin_ui_compat.c`, `plugin.h` |
| `adaptivePlayerStates[512]`, `adaptivePlayerCount` | `plugin_ui_compat.c`, `plugin.h` |
| `audioVolumeStates[512]`, `audioVolumeCount` | `plugin_ui_compat.c`, `plugin.h` |

**Entfernte Stub-Funktionen** (grep: **null Aufrufer** im gesamten Repo):

- `readHubDescription`, `parseHubDescription`, `applyDefaultSettingsIfNeeded`
- `initializeChannelIDs`, `manageChannelBasedOnCoordinates`
- `ts3_show_pending_hub_confirm`, `ts3_is_root_channel_id`
- `hubDescriptionMonitorThread`, `channelManagementThread`

Deklarationen aus `plugin_modules.h` entfernt. Struct-Typen (`CompletePositionalData`,
`AdaptivePlayerData`, …) bleiben — werden an anderer Stelle noch referenziert.

### Wie war es vorher (V7)?

Vier **512er-Arrays** (~je mehrere KB BSS) und neun **leere Stub-Funktionen** aus
dem Mumble-Erbe. Sie erzeugten falsche „es gibt noch Legacy-Pfade“-Signale, blaehten
die DLL auf und erschwerten grep-basierte Analysen.

### Warum ist die neue Loesung besser?

Weniger toter Speicher, klarere Modul-Grenzen, keine irrefuehrenden API-Deklarationen
fuer Code, der laengst durch V7-Rewrite-Module ersetzt wurde
(`hub_parser.c`, `channel_manage.c`, …).

### Wie funktioniert es jetzt?

Die echte Logik lebt in den V7/V8-Kernmodulen; `plugin_ui_compat.c` haelt nur noch
Globals, die tatsaechlich von UI/Compat-Pfaden genutzt werden.

### Wie wurde es getestet?

- Vollstaendiger grep vor dem Loeschen (keine Reads/Writes, keine Caller)
- `bash tests/run_tests.sh` + `bash build/build_mingw.sh` — gruen

### Lerneffekt

Vor dem Entfernen von Legacy-Globals immer **zwei grep-Pass**:
1. Symbol-Name (Reads/Writes)
2. Funktions-Caller (inkl. Header-Deklarationen)

Nur was beides null liefert, ist sicher tot.

---

## Gesamt-Verifikation V8.2

```text
$ bash tests/run_tests.sh
RESULT: ALL SUITES PASSED

$ bash build/build_mingw.sh
OK: built bin/mingw/conan_exiles.dll
```
