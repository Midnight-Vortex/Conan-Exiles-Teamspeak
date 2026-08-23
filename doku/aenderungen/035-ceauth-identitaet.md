# 035 — CEAUTH: weiche Spieler-Identitaet (SteamID64)

**Phase:** V8.16 · **Version:** 8.0.4

## Was wurde geaendert?

Neues Plugin-Kommando `CEAUTH:`, mit dem jeder Client seine **SteamID64** als
*weiche Identitaet* mitteilt. Der Empfaenger speichert sie pro Peer und zeigt
sie im TeamSpeak-Info-Panel („SteamID: 7656…"). Das erleichtert die **Zuordnung
von Spielern** und legt die Grundlage fuer spaetere, identitaetsabhaengige
Features.

Konkret:

- **Neu:** `src/ts/proximity/ts3_ceauth_wire.h` — reiner Codec (Bauen, strenges
  Parsen), header-only, ohne Win32/TS-SDK, damit ihn der Host-Unit-Test direkt
  prueft. Gleiches Muster wie `ts3_cemode_wire.h` / `ts3_ceping_wire.h`.
- **Neu:** `src/ts/proximity/ts3_ceauth.{h,c}` — Senden (ratenbegrenzt +
  Erstkontakt-Antwort), Empfangen, gesperrte Peer-Tabelle
  (`TS3_MAX_CLIENT_ID` Slots, Bounds-Check ueber `ts3_client_id_valid()`),
  Info-Panel-Formatter.
- **Neu:** `tests/ceauth_wire_test.c` + Eintrag in `tests/run_tests.sh`.
- `ts/entry/ts3_entry.c`: Praefix-Dispatch nach CEPING/vor CEPOS;
  CEDRAIN-Schritt nach `ts3_ceping_flush()`; `ts3_ceauth_send_pending()` in
  `ts3_pending_work_any()`; `ts3_ceauth_signal_send_pending()` beim Verbinden
  und beim Tab-Wechsel; `ts3_ceauth_clear_client()` beim Verlassen;
  `ts3_ceauth_reset()` in Disconnect- und Shutdown-Reset.
- `ts/entry/ts3_info.c`: SteamID-Zeile ins Peer-Info-Panel (nach der
  CEMODE-Zeile).
- `project/Conan-Exiles-TeamSpeak.vcxproj` + `build/build_mingw.sh`: neue
  Quell-/Header-Dateien eingetragen.
- Doku: `doku/module/ce-protokoll.md` (Praefix-Karte + Abschnitt 4b),
  `doku/01-architektur-v8.md` (Drain-Reihenfolge, Pending-Liste, Reset-Liste).

## Wie war es vorher?

Ein Peer war im Info-Panel nur ueber TeamSpeak-Name/UID und den (im CEPOS
enthaltenen) Spielernamen erkennbar. Die **stabile Spiel-Identitaet** (SteamID64)
kannte nur der eigene Client lokal (fuer die Rassen-Zuordnung); sie wurde nie an
Peers uebermittelt. Eine Zuordnung „TeamSpeak-Client ↔ Conan-Account" war von
aussen nicht moeglich.

## Warum ist die neue Loesung besser?

1. **Eigenes Praefix statt CEPOS-Erweiterung.** CEPOS bleibt eingefroren
   (56 Byte, alte Clients). Die SteamID kommt als klar abgegrenztes Kommando —
   bricht nichts.
2. **Kein Dauerverkehr.** Die Identitaet ist konstant, also wird sie nur beim
   Verbinden und als **einmalige Erstkontakt-Antwort** an spaeter beitretende
   Peers gesendet — nicht periodisch. N gleichzeitig beitretende Peers kosten
   dank CEDRAIN-Buendelung **einen** Broadcast.
3. **Fail-safe.** Laeuft Steam nicht (SteamID `0`), wird nichts gesendet und das
   Pending-Flag geloescht — kein Callback-Spin. Faellt CEAUTH aus, fehlt nur die
   Anzeige, nie Audio/Position/Modus.
4. **Sicher gegenueber Fremd-Eingaben.** Der Codec verwirft leere,
   nicht-numerische oder `0`-IDs und unbekannte Versionen; ein additives
   Zusatzfeld hinten wird ignoriert (vorwaertskompatibel).

## Wie funktioniert es jetzt?

Format: `CEAUTH:1;<steamID64>`, z. B. `CEAUTH:1;76561197960265728`. Details:
`doku/module/ce-protokoll.md` Abschnitt 4b.

```text
 Verbinden / Tab-Wechsel / Erstkontakt (irgendein Ausloeser)
   ts3_ceauth_signal_send_pending()
     └─ Flag setzen + ts3_request_wakeup()            (keine TS-API!)
 Wakeup-Thread ──► sendPluginCommand("CEDRAIN:1")
                                                       ▼
 Callback-Thread: CEDRAIN-Dispatcher
   ... → CEPING → ts3_ceauth_flush()
        (steamID aus Registry; nur >=250 ms her: "CEAUTH:1;<id>" senden)
                                                       │
                           TS-Server relayed           ▼
 Callback-Thread beim Peer: ts3_ceauth_on_plugin_command()
   → pruefen (Praefix, Plugin-ID, Version, id != 0, Client-ID-Grenzen)
   → SteamID in gesperrte Peer-Tabelle schreiben
   → Erstkontakt? einmalige eigene Antwort anfordern
                                                       │
 UI-Thread: ts3plugin_infoData()  ──► ts3_ceauth_format_peer()
   → "SteamID: <id>" (Lesen unter Lock)
```

**Thread-Vertrag:** `ts3_ceauth_signal_send_pending()` /
`ts3_ceauth_send_pending()` duerfen von jedem Thread kommen (nur
Interlocked-Flag + Wakeup). `ts3_ceauth_flush()` und
`ts3_ceauth_on_plugin_command()` laufen **nur** auf dem Callback-Thread
(TS-API). `clear_client` / `reset` / `format_peer` duerfen von jedem Thread
kommen — die Peer-Tabelle hat einen eigenen `CRITICAL_SECTION`, weil das
Info-Panel vom **UI-Thread** liest (gleiche Begruendung wie bei CEMODE/CEVER).

> **Weiche Identitaet.** Die SteamID ist selbst gemeldet und faelschbar — nur
> Anzeige/Zuordnung, nie Rechte oder Anti-Cheat.

## Wie wurde es getestet?

- **Unit-Test Codec:** `tests/ceauth_wire_test.c` — Round-Trip (Basis-SteamID64,
  realistische ID, max uint64), Ablehnung von `0`-ID (Format **und** Parse),
  kaputten Nutzlasten (`""`, `1`, `1;`, `x;5`, `1;x`, `1;123x`), unbekannter
  Version (0 und 2), NULL, zu kleinem Puffer, additivem Zusatzfeld.
- **Build:** Release x64 (`ts3_ceauth.c` uebersetzt, Link OK).
- **Manuell im TS-Client (noch offen):**
  1. Zwei Clients mit dem neuen DLL, gleicher Server/Ingame-Kanal, Steam laeuft.
  2. Info-Panel von Peer B beim Client A → Zeile „SteamID: 7656…" erscheint.
  3. Dritter Client tritt spaeter bei → sieht dank Erstkontakt-Antwort ebenfalls
     die SteamIDs der bereits Verbundenen.
  4. Client mit altem DLL: sendet kein CEAUTH, erzeugt keine Zeile — Audio/
     Position laufen unveraendert.

## Lerneffekt

Identitaet und Regeln gehoeren getrennt: Eine **weiche** Kennung
(hier SteamID64) darf bequem ueber den faelschbaren Spielerkanal reisen, solange
sie ausschliesslich der Anzeige/Zuordnung dient. Sobald daraus eine Rechte- oder
Trust-Entscheidung wuerde, waere derselbe Wert wertlos — dafuer bleibt allein die
vom Admin gepflegte Hub-Beschreibung zustaendig. Ein konstantes Datum wird
zudem am guenstigsten „event-getrieben + Erstkontakt-Antwort" verteilt, nicht
periodisch.
