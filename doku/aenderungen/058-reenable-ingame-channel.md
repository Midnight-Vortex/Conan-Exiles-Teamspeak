# 058 — Plugin wieder aktivieren: zurueck nach Ingame

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/adapter/ts3_adapter.c` / `.h` | `ts3_adopt_current_connection()` — bestehenden TS-Tab nach Plugin-Reload uebernehmen |
| `src/ts/entry/ts3_entry.c` | `ts3_bootstrap_connected_session()`; Aufruf in `ts3plugin_init` wenn schon connected |
| `doku/aenderungen/058-reenable-ingame-channel.md` | Dieser Eintrag |

**Eine Funktion:** Nach Deaktivieren (057, Hub) und erneutem Aktivieren sitzt der
Client wieder im Ingame-Kanal, sobald eine gueltige Position da ist.

---

## 2. Wie war es vorher?

`ts3plugin_init` startete Watcher/HTTP, hat aber die laufende Verbindung **nicht**
uebernommen. `g_connected` blieb 0, weil kein `CONNECTION_ESTABLISHED` kommt
(der User ist schon auf dem Server). `chan_tick` und Moves liefen nie — der
Client blieb im Hub, obwohl Conan noch Positionen sendet.

---

## 3. Warum ist die neue Loesung besser/stabiler?

Init uebernimmt den aktuellen Tab (`getCurrentServerConnectionHandlerID` +
derselbe Adopt-Pfad wie Tab-Wechsel). Danach: Root-Profil anfordern, `chan_tick`.
Sobald HTTP/Pos.txt die Koordinaten als ingame markiert, schiebt Auto-Move
Hub → Ingame (wie nach einem normalen Connect).

---

## 4. Wie funktioniert es jetzt?

```
Plugin an (schon im TS-Server, sitzt im Hub nach 057)
        │
        ▼
ts3plugin_init
  pos_watcher + HTTP start
  ts3_adopt_current_connection()  → g_connected=1
  ts3_bootstrap_connected_session()
        server_profile_tick + chan_tick
        │
        ├─ noch keine Pos → bleibt Hub
        └─ HTTP/Pos.txt valid → chan_should_be_ingame
              → requestClientMove(ingame)
```

---

## 5. Wie wurde es getestet?

- Build + Deploy.
- Ingame, Plugin aus (landet Hub), Plugin an, weiter im Spiel:
  Log `BOOT: already connected` und `CHAN: move requested -> … (ingame)`.

---

## 6. Lerneffekt

Reload ohne Connect-Event muss die Session selbst adoptieren. Sonst ist die
TS-API „offline“, obwohl der Client sichtbar auf dem Server sitzt.
