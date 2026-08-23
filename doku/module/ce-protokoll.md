# CE-Protokoll — Plugin-Commands und Hub-Schema

**Gilt ab:** V8.14 · **Status:** eingefroren (Wire-Regeln)

Diese Seite beschreibt **alles, was zwei Plugins ueber den TeamSpeak-Server
austauschen**, und **alles, was ein Server-Admin dem Plugin vorgibt**. Sie ist die
verbindliche Referenz fuer neue Nachrichten — wer ein neues Kommando erfindet,
haelt sich an die Regeln hier.

---

## 1. Zwei Kanaele, ein Server — was der TeamSpeak-Server wirklich tut

Wichtig zum Verstaendnis: **Auf dem TeamSpeak-Server laeuft kein Code von uns.**
Der Server macht nur zwei Dinge fuer uns:

```text
1) Briefkasten (Relay)    Client A --sendPluginCommand--> ts3server --> Client B, C, D
2) Schwarzes Brett        Admin schreibt Kanalbeschreibung --> jeder Client liest sie
```

| Kanal | Wer schreibt | Wer liest | Wofuer |
|---|---|---|---|
| **Plugin-Command** | jeder Client | alle anderen Clients | Laufende Zustandsdaten (Position, Sprechmodus, Version) |
| **Hub-Beschreibung** | nur Server-Admin | alle Clients | Regeln/Policy (Distanzen, Zonen, Feature-Schalter) |

Merksatz: **Plugin-Commands = Daten der Spieler (faelschbar).
Hub-Beschreibung = Regeln des Servers (vertrauenswuerdig).**

---

## 2. Plugin-Command-Karte (Praefixe)

Jedes Kommando ist ein Text, der mit einem festen Praefix beginnt. Das Praefix
entscheidet, welches Modul die Nachricht bekommt.

| Praefix | Laenge | Nutzlast | Wann gesendet | Modul |
|---|---|---|---|---|
| `CEPOS:` | 6 | Base64 von `CeposPacket` (**56 Byte, eingefroren**) | Bewegung (gedrosselt) + 1 Hz Keepalive | `ts3_cepos.c` |
| `CEDRAIN:` | 8 | `1` (Dummy) | Intern: Aufweck-Signal an den Callback-Thread | `ts3_entry.c` |
| `CEVER:` | 6 | Versionstext, z. B. `8.0.4` | Beim Verbinden + einmalige Antwort pro Peer | `ts3_plugin_version.c` |
| `CEMODE:` | 7 | `1;<mode>;<distanzDm>` | **Nur bei Moduswechsel** (Flanke) + Connect | `ts3_cemode.c` |
| `CEPING:` | — | reserviert | *(noch nicht implementiert)* | — |
| `CEAUTH:` | — | reserviert | *(noch nicht implementiert)* | — |

### 2.1 Warum die Praefixe nicht kollidieren

Der Vergleich passiert mit `strncmp` ueber die **exakte Praefixlaenge**. `CEPOS:`
(6 Zeichen) und `CEMODE:` (7 Zeichen) unterscheiden sich bereits im dritten
Zeichen (`P` vs. `M`) — ein `CEMODE:`-Kommando kann also nie versehentlich im
CEPOS-Parser landen. Neue Praefixe muessen sich ebenfalls **innerhalb der
kuerzesten bestehenden Praefixlaenge** unterscheiden.

### 2.2 Reihenfolge der Zustellung

In `ts3plugin_onPluginCommandEvent` (`src/ts/entry/ts3_entry.c`) wird der Reihe
nach gefragt: *„Ist das deins?"* Der erste Handler, der `1` zurueckgibt, beendet
die Kette.

```text
onPluginCommandEvent
  ├─ ts3_version_on_plugin_command   -> CEVER:
  ├─ ts3_cemode_on_plugin_command    -> CEMODE:
  ├─ cepos_on_plugin_command         -> CEPOS:
  └─ (Rest) CEDRAIN: -> Sammel-Abarbeitung aller offenen Arbeiten
```

**Rueckgabe-Vertrag:** `1` = „war mein Praefix" (auch wenn die Nachricht
verworfen wurde, z. B. kaputte Daten). `0` = „nicht mein Praefix". So wird eine
defekte Nachricht nie doppelt geparst.

---

## 3. Wire-Regeln (verbindlich fuer jedes neue Kommando)

1. **Nur `sendPluginCommand`.** Keine Chat-Nachrichten, keine Poke-Nachrichten
   als Datenkanal.
2. **Senden nur vom Callback-Thread** ueber `ts3_send_plugin_command_server()`.
   Andere Threads setzen ein Flag und rufen `ts3_request_wakeup()`; gesendet
   wird spaeter im CEDRAIN-Durchlauf. (Einzige Ausnahme: der Wakeup-Thread
   selbst sendet `CEDRAIN:1`.)
3. **Nutzlast versionieren.** Neue Kommandos beginnen mit einer Zahl und `;`
   (z. B. `CEMODE:1;2;600`). Ein Empfaenger, der die Version nicht kennt,
   verwirft die Nachricht still. Ein **zusaetzliches Feld hinten anhaengen**
   ist erlaubt, ohne die Version zu erhoehen — Empfaenger ignorieren, was sie
   nicht kennen. Aendert sich dagegen die **Bedeutung** eines bestehenden
   Feldes, muss die Version hoch, sonst missversteht ein alter Client die
   Nachricht stillschweigend.
4. **Unbekanntes ignorieren.** Kein Praefix-Treffer = kein Fehler. So koennen
   alte und neue Clients gemischt auf einem Server spielen.
5. **Feste Puffer + Bounds-Check.** Client-IDs immer gegen
   `ts3_client_id_valid()` pruefen, Zahlen auf Wertebereich pruefen
   (`isfinite`, Min/Max), Strings terminieren.
6. **Drosseln.** Jedes Kommando braucht eine Mindestpause zwischen zwei Sends.
   Flankenereignisse (Moduswechsel) senden zusaetzlich nur bei echter Aenderung.
7. **Keine Geheimnisse.** Alles ist fuer jeden Client lesbar und faelschbar —
   also niemals Passwoerter, Tokens oder Rechte darueber transportieren.
8. **CEPOS bleibt eingefroren.** Die 56 Byte duerfen nicht wachsen. Neue
   Zustandsdaten bekommen ein **eigenes Praefix**, damit alte Clients weiter
   Positionen verstehen.

### 3.1 Warum nicht einfach CEPOS erweitern?

Weil `CeposPacket` byteweise mit dem alten Plugin (V7 / Mumble-Port)
kompatibel ist. Ein zusaetzliches Feld wuerde die Paketgroesse aendern; alte
Clients pruefen `decoded != sizeof(CeposPacket)` und wuerden **alle** Positionen
verwerfen. Ergebnis: alte Spieler hoeren niemanden mehr. Ein neues Praefix
kostet ein paar Bytes mehr Bandbreite, bricht aber nichts.

---

## 4. `CEMODE:` im Detail

**Zweck:** Der Empfaenger soll wissen, ob ein Peer **fluestert, normal spricht
oder ruft** — nicht nur, wie weit dessen Stimme reicht.

**Warum reicht CEPOS nicht?** In CEPOS steht nur `voiceDistance` (Meter). Aus
30 m laesst sich der Modus nicht zurueckrechnen: Zonen-Overrides, Rassen-Boni und
Hub-Clamps koennen dafuer sorgen, dass „Normal" auf Server A 30 m und auf
Server B ebenfalls 30 m fuer „Rufen" bedeutet. Der Modus ist also eine eigene
Information.

### 4.1 Format

```text
CEMODE:1;<mode>;<distanzDm>
        │  │        └── Dezimeter (Meter × 10), ganzzahlig, 0 .. 10000
        │  └─────────── 0 = Fluestern, 1 = Normal, 2 = Rufen
        └────────────── Nutzlast-Version (aktuell immer 1)
```

Beispiel: `CEMODE:1;2;600` = „Ich rufe, meine Stimme traegt 60,0 m."

**Warum Dezimeter statt Komma-Zahl?** Ein Dezimaltrennzeichen wird von der
C-Laufzeitumgebung nach **Laenderkennung** geschrieben und gelesen — ein
deutscher Client wuerde `60,0` senden, ein englischer `60.0` erwarten und die
Nachricht verwerfen. Ganzzahlen haben dieses Problem nicht. Eine
Nachkommastelle Genauigkeit reicht voellig: `voiceDistance` ist ohnehin nur
eine Anzeige- und Reichweitenangabe in Metern.

**Wo der Codec lebt:** `src/ts/proximity/ts3_cemode_wire.h` — reine Funktionen
(Meter→Dezimeter, Bauen, Parsen), ohne Win32 und ohne TS-SDK. Dadurch prueft
`tests/cemode_wire_test.c` **genau den Code, der auch ausgeliefert wird**, statt
einer Nachbildung. Empfangene Daten gelten dort grundsaetzlich als feindlich:
jedes Feld wird auf Zahl **und** Wertebereich geprueft, bevor es gespeichert
wird.

### 4.2 Wann gesendet wird

| Ausloeser | Grund |
|---|---|
| Moduswechsel (Hotkey, UI, Preset) | Flanke — der eigentliche Zweck |
| Verbindung hergestellt / Tab-Wechsel | Neue Peers kennen unseren Modus sonst nicht |
| Erstkontakt mit einem Peer | Einmalige Antwort, damit der Neue uns auch kennt |

Der Erstkontakt-Fall funktioniert wie beim Versions-Austausch (`CEVER:`):
Wer ein `CEMODE:` von einem bisher unbekannten Client bekommt, antwortet
**genau einmal** mit dem eigenen Modus. Danach ist der Austausch komplett.

### 4.3 Kein Ersatz fuer CEPOS

`CEMODE` transportiert **kein Position**. Die Lautstaerkeberechnung nutzt
weiterhin ausschliesslich `voiceDistance` aus CEPOS. Faellt `CEMODE` aus
(alter Client, Paketverlust), klingt alles exakt wie vorher — nur die Anzeige
des Modus fehlt. Das ist bewusst so: **Anzeige-Feature darf Audio nie
gefaehrden.**

---

## 5. Hub-Schema (Kanalbeschreibung des Root-Kanals)

Der Admin schreibt in die **Root-Kanalbeschreibung** einen INI-aehnlichen Text.
`hub_parser.c` liest ihn, `ts3_server_profile.c` wendet ihn an.

```text
[GLOBAL]        Serverweite Regeln (Distanzen, Feature-Schalter)
[ZONES]         Rechteck-Zonen: schallisoliert, Hall, eigene Distanzen
[RACE]          Fraktionen ueber SteamID-Listen, eigene Distanzen/Bonus
[DEFAULT_SETTINGS]  Vorbelegung beim allerersten Verbinden
```

### 5.1 Regeln fuer neue Keys (additiv)

1. **Nur ergaenzen, nie umbenennen.** Bestehende Server duerfen nach einem
   Plugin-Update nicht kaputtgehen — auch die historischen Schreibfehler
   (`MinimumWisper`, `ForceAutomaticChanelSwitching`) bleiben gueltig.
2. **Fehlender Key = bisheriges Verhalten.** Jeder neue Key braucht einen
   Default, der genau das tut, was das Plugin vorher schon tat.
3. **Clients lesen nur.** Das Plugin schreibt niemals in die Kanalbeschreibung.
4. **Key-Vergleich ist case-insensitiv** (`hub_value_for`), Aliase sind erlaubt
   (Beispiel: `FilterIntensity` = `hubAudioFilterIntensity`).
5. **Validierung beim Parsen.** Wertebereich sofort begrenzen, nicht erst im
   Audio-Pfad.

### 5.2 `SchemaVersion` — bewusst noch nicht eingebaut

Ein Key `SchemaVersion=<n>` in `[GLOBAL]` ist **spezifiziert, aber nicht
implementiert**. Begruendung:

- Solange die Regel „fehlender Key = altes Verhalten" gilt, ist eine
  Versionsnummer **ueberfluessig** — jeder Client kommt mit jeder Beschreibung
  klar.
- Sinnvoll wird sie erst, wenn ein Key seine **Bedeutung** aendert (nicht nur
  neu dazukommt). Dann darf ein alter Client die Beschreibung nicht mehr
  interpretieren.

Wenn dieser Fall eintritt, gilt:

| Wert | Bedeutung fuer den Client |
|---|---|
| Key fehlt | Schema 1 annehmen (heutiges Verhalten) |
| `SchemaVersion` ≤ eigener Stand | normal anwenden |
| `SchemaVersion` > eigener Stand | anwenden, was verstanden wird; Rest ignorieren; einmalig ins Log |

**Nicht** implementieren, bevor es einen echten inkompatiblen Key gibt —
sonst ist es toter Code, den niemand testet.

---

## 6. Bewusst zurueckgestellt: Whisper-Listen

Die TeamSpeak-API kennt `requestClientSetWhisperList` (`sdk/include/ts3_functions.h`).
Damit koennte ein Client sagen: „Meine Stimme geht gezielt an diese Kanaele/
Clients.“ Der Gedanke „dann routen wir Proximity darueber“ liegt nahe — er ist
aber **falsch**, und deshalb ist das Feature nicht gebaut:

| Punkt | Warum es blockiert |
|---|---|
| Wer setzt die Liste? | Nur der **Sender** fuer sich selbst. Ein Sender weiss aber nicht zuverlaessig, wer ihn hoeren darf — genau das rechnet jeder Empfaenger heute selbst aus. |
| Nebenwirkung | Eine aktive Whisper-Liste veraendert das normale Sprechen im Kanal. Ein Fehler im Setzen macht einen Spieler **stumm** — deutlich schlimmer als eine falsche Lautstaerke. |
| Rechte | Whisper braucht Server-Berechtigungen (`i_client_whisper_power` u. a.). Auf fremden Servern nicht garantiert. |
| Frequenz | Bei Bewegung muesste die Liste staendig neu gesetzt werden — TS-API-Last statt lokaler Mathematik. |

**Wenn** es spaeter kommt, dann nur als **Admin-/Global-Overlay**
(z. B. serverweite Durchsage) und mit diesen Auflagen:

1. Niemals der Proximity-Router — CEPOS + PCM bleiben der Hoerpfad.
2. Fail-safe: Beim Deaktivieren, bei Fehlern und beim Disconnect **immer** eine
   leere Liste setzen, damit niemand stumm zurueckbleibt.
3. Vorher Rechte pruefen und bei fehlender Berechtigung sauber abbrechen.
4. Nur vom Callback-Thread, wie jede TS-API.

## 7. Nie wieder: eigener ServerLib-Dienst

Auf dem Retail-`ts3server` laeuft **kein** Fremdcode. Der Gedanke, stattdessen
mit der kommerziellen TeamSpeak-SDK (`ts3client_*` / `ts3server_*`) einen
eigenen Server zu bauen, ist eine **verworfene Produktrichtung** und gehoert
nicht in diesen Arbeitsstrang. Im Quellcode existiert davon nichts (geprueft
2026-08-23: keine `ts3server_*`- oder ClientLib-Symbole in `src/`). Wer zentrale
Berechnung braucht, baut einen **eigenen Dienst neben** TeamSpeak — das waere
eine neue Produktentscheidung, keine Erweiterung dieses Plugins.

## 8. Was das Protokoll bewusst *nicht* kann

| Wunsch | Warum nicht |
|---|---|
| Der TS-Server rechnet aus, wer wen hoert | Auf dem Retail-`ts3server` laeuft kein Fremdcode |
| Sprachpakete von Unhoerbaren gar nicht erst zustellen | Der Server kennt keine Positionen; wir daempfen nur lokal im PCM |
| Position als Anti-Cheat-Wahrheit | Jeder Client kann `CEPOS` faelschen — dafuer braeuchte es einen Feed vom Conan-Dedicated-Server |
| Rechte/Rollen ueber Plugin-Commands | Faelschbar; Rechte gehoeren in die Hub-Beschreibung (nur Admin) |

Das ist die **Obergrenze des Plugin-SDK**. Alles darueber hinaus braucht einen
eigenen Serverdienst — bewusst ausserhalb dieses Arbeitsstrangs.

---

## 9. Lerneffekt

Ein Netzwerkformat ist ein **Versprechen an alle, die es schon benutzen**. Wer
neue Daten braucht, erweitert nicht das alte Paket, sondern legt einen neuen,
klar abgegrenzten Kanal daneben — dann koennen alte und neue Teilnehmer
gleichzeitig weiterarbeiten. Und Zusatzinfos duerfen nie zur Voraussetzung fuer
die Kernfunktion werden: faellt `CEMODE` aus, funktioniert Proximity-Audio
unveraendert weiter.
