# 023 — Kanal-IDs: eine einzige Quelle (Single Source of Truth)

**Phase:** V8.5 (Abschluss "Ein Besitzer pro Zustand") ·
**Lektion:** `02-lessons-learned-v7.md` Lektion 4 (zwei Besitzer = unsichtbare Bugs)

## Worum geht es? (Anfaenger-Erklaerung)

Das Plugin verschiebt dich automatisch zwischen zwei TeamSpeak-Kanaelen:
dem **Hub** (Lobby) und dem **Ingame**-Kanal. Dafuer muss es zwei Kanal-IDs kennen:
`hubChannelID` und `ingameChannelID`. Zusaetzlich merkt es sich, in welchem Kanal du
**gerade** sitzt (`ts3LocalChannelID`) — das braucht das HUD-Overlay, um zu entscheiden,
ob es angezeigt wird.

## Wie war es vorher (V7)?

Laut Audit (Lektion 4) gab es die Kanal-IDs **doppelt**: einmal im Modul
`channel_manage` und einmal als Legacy-Globals (`hubChannelID`, `ingameChannelID`)
aus der Mumble-Zeit. Zwei Stellen, die dasselbe wissen sollen, laufen irgendwann
auseinander — mal stimmt die eine, mal die andere ("wer gewinnt?").

## Was wurde geaendert / bestaetigt?

Nach der V8.6-Verlagerung von `channel_manage` nach `ts/channel/` ist die Lage jetzt
sauber, und dieser Schritt **verriegelt** sie:

- **Einzige Quelle:** `ts/channel/channel_manage.c` besitzt `g_hubChannelID` und
  `g_ingameChannelID` (nur der Callback-Thread schreibt sie). Nach aussen nur lesbar
  ueber `chan_get_hub_channel_id()` / `chan_get_ingame_channel_id()`.
- **Abgeleitete Spiegel:** Die Legacy-Globals `hubChannelID` / `ingameChannelID`
  werden an **genau einer** Stelle gesetzt (`plugin_ui_sync_live_state` in
  `plugin_ui_compat.c`, Callback-Thread) — direkt aus den kanonischen Gettern. Sie
  existieren nur noch, weil das Overlay-HUD sie liest. Kommentare an der Schreibstelle
  und an den `extern`-Deklarationen in `plugin.h` markieren sie jetzt klar als
  "abgeleiteter Spiegel — nirgends sonst zuweisen".
- **`ts3LocalChannelID`** (aktuell besetzter Kanal — ein *anderes* Konzept als hub/ingame)
  hat ebenfalls genau einen Schreiber: `ts3_sync_overlay_channel_state` in `ts3_entry.c`
  (Callback-Thread).

Mit grep bestaetigt: es gibt fuer jede dieser Variablen **genau einen** Schreiber.

## Warum ist das besser?

```
VORHER (Risiko):                    JETZT (Single Source):
channel_manage: g_hubChannelID      channel_manage: g_hubChannelID  ← EINZIGE Quelle
Legacy-Global:  hubChannelID   ?         │ chan_get_hub_channel_id()
(zwei unabhaengige Wahrheiten)           ▼
                                    Overlay-Spiegel: hubChannelID (nur abgeleitet, 1 Schreiber)
```

Es kann keine zwei widersprechenden Wahrheiten mehr geben: Der Spiegel ist immer eine
Kopie der Quelle, kein eigenstaendiger Zustand.

## Wie getestet?

`bash tests/run_tests.sh` (alle Suiten gruen inkl. Layering-Wache) +
`bash build/build_mingw.sh` (DLL baut). Reine Kommentar-/Doku-Aenderung ohne
Verhaltensaenderung — der Auto-Move und die HUD-Sichtbarkeit funktionieren wie zuvor.
Manueller TS-Test (am Ende): Spiel starten → Auto-Move ingame; beenden → Auto-Move hub;
HUD nur im Ingame-Kanal sichtbar.

## Lerneffekt

"Single Source of Truth" heisst nicht, dass eine Info nur an einer Stelle *stehen*
darf — Spiegel/Caches sind ok. Es heisst: es gibt nur **einen Schreiber**, und alle
anderen Kopien sind nachweislich davon **abgeleitet**. Ein Kommentar + grep-Beweis
("nur ein Schreiber") macht diese Regel ueberpruefbar.
