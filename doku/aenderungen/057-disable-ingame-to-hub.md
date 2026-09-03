# 057 — Plugin-Deaktivieren: Ingame nach Hub

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `src/ts/channel/channel_manage.c` / `.h` | `chan_leave_ingame_on_plugin_disable()` — letzter `requestClientMove` nach Hub + Nick-Restore |
| `src/ts/entry/ts3_entry.c` | Aufruf als Schritt 0 in `ts3plugin_shutdown()`, solange die TS-API noch lebt |
| `doku/aenderungen/057-disable-ingame-to-hub.md` | Dieser Eintrag |

**Eine Funktion:** Wer das Plugin in den TeamSpeak-Einstellungen deaktiviert und
im Ingame-Kanal sitzt, wird **vorher** in den Hub geschoben.

---

## 2. Wie war es vorher?

`ts3plugin_shutdown()` hat Audio/Threads/Adapter beendet. Der Client blieb im
Ingame-Kanal — ohne Plugin, ohne Proximity, mit Zufalls-Nick. Andere hoerten
ihn voll (oder gar nicht), als waere er noch im Spiel.

---

## 3. Warum ist die neue Loesung besser/stabiler?

TeamSpeak ruft `ts3plugin_shutdown()` auf dem Callback-Thread, **bevor** die
DLL entladen wird. `requestClientMove` ist asynchron: der Request geht noch
raus, der Server bewegt den Client, auch wenn das Plugin danach weg ist.

Der echte Name muss **jetzt** zurueckgesetzt werden — `onClientMoveEvent`
laeuft nach dem Unload nicht mehr in unserem Code.

Cooldown/In-flight werden fuer diesen letzten Move ignoriert.

Nur der **eigene** Client (wer deaktiviert). Kein Remote-Kick.

---

## 4. Wie funktioniert es jetzt?

```
TS: Plugin deaktivieren
        │
        ▼
ts3plugin_shutdown()                (Callback-Thread, noch connected)
  chan_leave_ingame_on_plugin_disable()
        │
        ├─ nicht connected / nicht ingame → nichts
        ├─ kein Hub gefunden → Log, bleiben
        └─ ingame:
              nick_restore_in_hub()
              requestClientMove(local, hub)
              Log: plugin disable — move ingame -> hub requested
        │
        ▼
  bisherige Shutdown-Schritte 1–5
```

---

## 5. Wie wurde es getestet?

- Build + Deploy.
- Ingame-Kanal, Plugins → Conan Exiles deaktivieren → Client landet im Hub,
  echter Name wieder da. Log: `CHAN: plugin disable — move ingame -> hub requested`.
- Im Hub deaktivieren: keine Bewegung.

---

## 6. Lerneffekt

Letzte TS-API-Calls gehoeren an den **Anfang** von `ts3plugin_shutdown()`,
nicht hinter `ts3_adapter_shutdown()`.
