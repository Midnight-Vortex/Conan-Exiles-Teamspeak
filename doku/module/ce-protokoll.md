# CE-Protokoll — Plugin-Commands und Hub-Schema

**Gilt ab:** V8.14 · **Stand:** Review-Fixes 083 (CEPING 1 Hz, CEPOS-Sendepfad, CEDRAIN nur an sich selbst) · **Status:** eingefroren (Wire-Regeln)

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
| `CEPOS:` | 6 | Base64 von `CeposPacket` (**56 Byte, eingefroren**) | Bewegung (gedrosselt, Boden 30 ms) + 1 Hz Keepalive; im Leerlauf nur der Keepalive (4c) | `ts3_cepos.c` |
| `CEDRAIN:` | 8 | `1` (Dummy) | Intern: Aufweck-Signal an den Callback-Thread — seit 083 nur an den **eigenen** Client adressiert (siehe 2.3) | Sender `ts3_adapter.c`, Empfaenger `ts3_entry.c` |
| `CEVER:` | 6 | Versionstext, z. B. `8.0.4` | Beim Verbinden + einmalige Antwort pro Peer | `ts3_plugin_version.c` |
| `CEMODE:` | 7 | `1;<mode>;<distanzDm>` | **Nur bei Moduswechsel** (Flanke) + Connect; Sende-Boden 30 ms; ein Wechsel, der waehrend des Sendens passiert, geht nie verloren (4.2) | `ts3_cemode.c` |
| `CEPING:` | 7 | `1;<seq>` | Liveness-Heartbeat, **1× pro Sekunde** (vom Positions-Tick angestossen, im Leerlauf nichts) | `ts3_ceping.c` |
| `CEAUTH:` | 7 | `1;<steamID64>` | Weiche Identitaet: Connect + einmalige Antwort pro Peer; Sende-Boden 30 ms | `ts3_ceauth.c` |

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
  ├─ ts3_ceping_on_plugin_command    -> CEPING:
  ├─ ts3_ceauth_on_plugin_command    -> CEAUTH:
  ├─ cepos_on_plugin_command         -> CEPOS:
  └─ (Rest) CEDRAIN: -> Sammel-Abarbeitung aller offenen Arbeiten
                         (nur das CEDRAIN des eigenen Clients; fremde werden ignoriert)
```

**Rueckgabe-Vertrag:** `1` = „war mein Praefix" (auch wenn die Nachricht
verworfen wurde, z. B. kaputte Daten). `0` = „nicht mein Praefix". So wird eine
defekte Nachricht nie doppelt geparst.

### 2.3 `CEDRAIN:` — das interne Aufweck-Signal (Review-Fix 083)

`CEDRAIN:1` ist **kein Spieler-Datum**, sondern ein Trick: Das TeamSpeak-SDK hat
keinen Timer-Callback. Damit der Callback-Thread trotzdem „sofort“ Arbeit
abholt, schickt der Wakeup-Thread sich selbst ein Plugin-Command — dessen
Ankunft ruft `ts3plugin_onPluginCommandEvent` auf dem Callback-Thread auf.

```text
VORHER (bis 8.0.14)                     JETZT
Wakeup-Thread -> Server -> ALLE         Wakeup-Thread -> Server -> nur ICH
   (N Clients × 33/s relayt = N²)          (Ziel: PluginCommandTarget_CLIENT, eigene ID)
```

- **Warum:** Serverweit gesendet, relayt der Server jedes Aufwecken eines
  Clients an alle anderen (~33/s pro Client, N² im Server) — und jeder
  Empfaenger fuehrte die *fremden* Drains aus. Reiner Verlust.
- **Fallback:** Ob der TS-Server ein an den Absender selbst adressiertes
  Command zurueckschickt, ist nicht garantiert. Der Wakeup-Thread prueft das
  zur Laufzeit: Kommt ca. 2 s lang (mindestens 5 Sends) **kein** eigenes
  CEDRAIN zurueck, schaltet er automatisch auf den alten serverweiten Versand
  um (bekannt funktionierend). Neue Verbindung / Tab-Wechsel starten die
  Pruefung neu. Drains koennen also nie dauerhaft ausfallen.
- **Empfang:** `ts3_entry.c` fuehrt nur das CEDRAIN des **eigenen** Clients aus;
  das von anderen Clients (alte Plugin-Version, Fallback-Modus) wird ignoriert.
- **Wire-Format** bleibt `CEDRAIN:1`; alte Clients verstehen es unveraendert.

Umsetzung: `src/ts/adapter/ts3_adapter.c` (Wakeup-Thread),
`src/core/util/drain_target.h` (reine Entscheidungslogik),
`src/ts/entry/ts3_entry.c` (Filter). Das ist das Arbeitspaket ADAPTER; die
Details stehen in dessen Aenderungsdokument.

---

## 3. Wire-Regeln (verbindlich fuer jedes neue Kommando)

1. **Nur `sendPluginCommand`.** Keine Chat-Nachrichten, keine Poke-Nachrichten
   als Datenkanal.
2. **Senden nur vom Callback-Thread** ueber `ts3_send_plugin_command_server()`.
   Andere Threads setzen ein Flag und rufen `ts3_request_wakeup()`; gesendet
   wird spaeter im CEDRAIN-Durchlauf. (Einzige Ausnahme: der Wakeup-Thread
   selbst sendet `CEDRAIN:1` — seit 083 nur an den eigenen Client, siehe 2.3.)
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
   Zustandsdaten, auf die ein Peer sofort reagieren muss (`CEPOS`, `CEMODE`,
   `CEAUTH`), teilen sich den Boden `PLUGIN_POLL_INTERVAL_MS` (30 ms) —
   Flankenereignisse (Moduswechsel) senden zusaetzlich nur bei echter
   Aenderung. **Reine Diagnose** (`CEPING`) bekommt einen eigenen, langsamen
   Boden (1000 ms): jeder Send wird vom Server an *alle* Clients relayt (N²),
   und eine Diagnose darf nicht so viel Verkehr kosten wie die Position selbst.
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
Zwei Sends liegen mindestens `PLUGIN_POLL_INTERVAL_MS` (30 ms) auseinander —
derselbe Boden wie CEPOS/CEDRAIN, kein extra 250-ms-Warten.

**Kein verlorener Moduswechsel (083):** Der Sender setzt vor dem Senden zuerst
sein „Es ist etwas zu senden“-Flag auf 0 (*claim*) und liest **danach** den
Modus. Wechselt der Spieler genau in diesem Moment den Modus (anderer Thread),
setzt dieser Wechsel das Flag wieder auf 1 und loest einen eigenen Send aus.
Frueher wurde das Flag erst *nach* dem Senden geloescht — der Wechsel wurde
dabei mit geloescht, und die Peers behielten den alten Modus, weil CEMODE kein
periodisches Auffrischen hat. Schlaegt ein Send fehl (`sendPluginCommand`
liefert 0), wird das Flag bis zu 5× neu gesetzt, danach gibt der Sender mit
einer Logzeile auf (kein Endlos-Spin).

### 4.3 Kein Ersatz fuer CEPOS

`CEMODE` transportiert **kein Position**. Die Lautstaerkeberechnung nutzt
weiterhin ausschliesslich `voiceDistance` aus CEPOS. Faellt `CEMODE` aus
(alter Client, Paketverlust), klingt alles exakt wie vorher — nur die Anzeige
des Modus fehlt. Das ist bewusst so: **Anzeige-Feature darf Audio nie
gefaehrden.**

---

## 4a. `CEPING:` im Detail

**Zweck:** Ein Empfaenger soll merken, wenn von einem Peer **Updates verloren
gehen** oder dessen Datenstrom **veraltet** ist — reine Diagnose, kein Audio.

### 4a.1 Format

```text
CEPING:1;<seq>
        │  └── Heartbeat-Zaehler, uint32, laeuft bei 2^32 ueber
        └───── Nutzlast-Version (aktuell immer 1)
```

Beispiel: `CEPING:1;42`.

### 4a.2 Wann gesendet wird

Der Sender erhoeht bei jedem tatsaechlichen Senden seinen Zaehler. Der
Heartbeat haengt am Positions-Tick (derselbe Ausloeser wie CEPOS): jeder Tick
(~30 ms) *meldet* „ein Heartbeat waere faellig“, tatsaechlich gesendet wird aber
nur, wenn der letzte Send **mindestens 1000 ms** (`CEPING_SEND_MIN_MS`) zurueckliegt.
Ist es noch nicht so weit, wird die Meldung verworfen (kein Aufweck-Spin); der
naechste Tick meldet erneut. Bewegt sich der Spieler nicht bzw. kommen keine
Positionen, wird auch kein CEPING gesendet — im Leerlauf entsteht kein
Zusatzverkehr.

**Warum 1000 ms und nicht 30 ms (Review-Fix 083)?** Der Commit `dda9cbe` hatte
CEPING von 1000 ms auf den 30-ms-Tick gezogen („eine Uhr fuer alles“). Aber
jeder Send geht ueber den Server an *alle* Clients: bei N Spielern sind das
N² Zustellungen pro Tick. CEPING ist nur Diagnose — bei 30 ms kostete sie genauso
viel Server-Verkehr wie die Position selbst (CEPOS). Der urspruengliche Entwurf
(`doku/aenderungen/034`) sah deshalb 1 Hz vor.

Was sich dadurch aendert — und was nicht:

| Was | Wirkung |
|---|---|
| Sequenzsprung von 1 | bedeutet jetzt „eine Sekunde ohne Heartbeat“ statt „ein 30-ms-Tick fehlt“ |
| Verlusterkennung | gleich gut: ein Paketverlust ist an der Luecke in `seq` sichtbar, egal wie langsam der Zaehler laeuft |
| Schwellen / Veralten | **keine Abhaengigkeit**: der Empfaenger *loggt* nur Luecken. Die Spieler-Tabelle verfaellt unabhaengig davon nach `PLAYER_TABLE_STALE_MS` (120 s); Info-Panel und Audio lesen CEPING nicht |
| Server-Verkehr | CEPING sinkt von ~33 auf ~1 Nachricht/s pro Client (rund 97 % weniger) |

### 4a.3 Verlusterkennung (wraparound-sicher, reorder-sicher, gedrosselt)

Der Empfaenger merkt sich pro Peer die zuletzt gesehene `seq` (`CepingTrack`,
reine Logik in `ceping_track_observe`, `ts3_ceping_wire.h`). Bei der naechsten
Nachricht:

```text
Sprung um 1        -> nichts verloren
Sprung um N (>1)   -> N-1 Heartbeats verloren  -> log_debug("peer X lost N-1")
gleiche seq        -> Duplikat                 -> ignoriert
kleinere seq       -> Reihenfolge vertauscht   -> ignoriert, lastSeq bleibt!
(sehr viel kleiner -> Peer wurde neu gestartet -> neue Basis, kein Verlust)
```

Die Differenz wird als **uint32 mit Ueberlauf** gerechnet, damit der
Zaehlerwechsel von `0xFFFFFFFF` auf `0` korrekt als „consecutive“ gilt.

**Nur vorwaerts fortschreiben (Review-Fix 083).** Frueher wurde `lastSeq` bei
*jeder* Nachricht ueberschrieben — auch bei einer aelteren. Kamen die Pakete als
10, 12, 11, 13 an, meldete das System bei 12 einen Verlust, setzte `lastSeq` bei
11 *zurueck* und meldete bei 13 gleich noch einen. Jetzt rueckt `lastSeq` nur bei
echten Vorwaertsspruengen (`0 < diff <= 0x7FFFFFFF`) vor; eine verspaetete 11
ist ein stilles Nichts. (Den einen Verdacht bei 12 kann man nicht vermeiden —
in dem Moment ist 11 noch nicht da.)

**Neustart eines Peers.** Faellt die Zahl um mehr als `CEPING_REORDER_WINDOW`
(16) zurueck, hat der Peer sein Plugin neu gestartet (Zaehler beginnt bei 1).
Dann wird die neue Zahl als Basis uebernommen — ohne diese Regel wuerde ein
neu gestarteter Peer ignoriert, bis sein Zaehler den alten Wert eingeholt hat.

**Log-Drossel.** Ein boeswilliger Peer kann beliebige `seq`-Spruenge senden
(`CEPING:1;1`, `CEPING:1;999999`, ...). Jede Luecke ergab frueher eine eigene
Logzeile. Jetzt darf jeder Peer **hoechstens eine Verlust-Zeile pro Sekunde**
schreiben; die uebrigen Ereignisse werden gezaehlt und in der naechsten Zeile
als `(+N earlier loss event(s) not logged)` nachgereicht. Die Tabelle ist je
Peer getrennt (`TS3_MAX_CLIENT_ID`-Slots, nur vom Callback-Thread beschrieben).

### 4a.4 Kein Ersatz fuer irgendetwas

CEPING transportiert **keine Position und keinen Modus**. Faellt es aus (alter
Client, Paketverlust), fehlt nur die Verlust-/Liveness-Diagnose im Log — Audio,
Position und Modus laufen unveraendert weiter. Der Heartbeat ist die Grundlage
fuer die spaetere Kandidaten-/Stale-Optimierung (Hybrid-Plan), zwingt aber
heute noch nichts.

---

## 4b. `CEAUTH:` im Detail

**Zweck:** Ein Peer soll wissen, **welcher Conan-Spieler** hinter einem
TeamSpeak-Client steckt (SteamID64). Das erleichtert die **Zuordnung von
Spielern** im Info-Panel und legt die Grundlage fuer spaetere,
identitaetsabhaengige Features (z. B. Rassen/Rechte, die heute nur lokal
aufgeloest werden).

> **Weiche Identitaet — niemals Vertrauensanker.** Die SteamID ist
> selbst gemeldet, **nicht authentifiziert und trivial faelschbar**. Sie darf
> ausschliesslich zur Anzeige/Zuordnung dienen, **nie** fuer Rechte-, Trust-
> oder Anti-Cheat-Entscheidungen (Wire-Regel 7). Wo Rechte zaehlen, gilt weiter
> die vom Admin gepflegte Hub-Beschreibung.

### 4b.1 Format

```text
CEAUTH:1;<steamID64>
        │  └── oeffentliche SteamID64 (dezimal, ungleich 0)
        └───── Nutzlast-Version (aktuell immer 1)
```

Beispiel: `CEAUTH:1;76561197960265728`.

Die lokale SteamID64 stammt aus der Registry
(`server_profile_get_local_steam_id()`, `HKCU\...\ActiveProcess\ActiveUser`
plus Public-Universe-Offset). Laeuft Steam nicht (`0`), wird **nichts**
gesendet.

**Wo der Codec lebt:** `src/ts/proximity/ts3_ceauth_wire.h` — reine Funktionen
(Bauen, strenges Parsen), ohne Win32 und ohne TS-SDK. `tests/ceauth_wire_test.c`
prueft damit genau den ausgelieferten Code. Empfangene Daten gelten als
feindlich: leere/nicht-numerische/`0`-IDs und unbekannte Versionen werden
verworfen; ein additives Zusatzfeld hinten wird ignoriert.

### 4b.2 Wann gesendet wird

| Ausloeser | Grund |
|---|---|
| Verbindung hergestellt / Tab-Wechsel | Peers kennen unsere Identitaet sonst nicht |
| Erstkontakt mit einem Peer | Einmalige Antwort, damit ein *spaeter* verbundener Client uns auch kennt |

Genau wie `CEVER:`/`CEMODE:`: Wer ein `CEAUTH:` von einem bisher unbekannten
Client bekommt, antwortet **genau einmal** mit der eigenen Identitaet. Die
Antwort wird in den naechsten CEDRAIN gebuendelt — N gleichzeitig beitretende
Peers kosten **einen** Broadcast, nicht N. Zwei Sends liegen mindestens
`PLUGIN_POLL_INTERVAL_MS` (30 ms) auseinander. Da die Identitaet konstant ist,
gibt es keinen periodischen Verkehr.

### 4b.3 Kein Ersatz fuer irgendetwas

CEAUTH beeinflusst **weder Audio noch Position noch Modus**. Faellt es aus
(alter Client, Steam offline, Paketverlust), fehlt nur die SteamID-Zeile im
Info-Panel — alles andere laeuft unveraendert.

---

## 4c. `CEPOS:` — Sendepfad und Empfang (Review-Fixes 083)

Das Wire-Format ist **unveraendert** (56 Byte, siehe 3.1). Geaendert wurde, *wie
billig* der Sender entscheidet, ob er senden muss, und wie vorsichtig der
Empfaenger mit dem Namen umgeht.

### 4c.1 Billige Aenderungspruefung zuerst

Der Positions-Tick meldet etwa alle 30 ms „vielleicht ist ein Send faellig“.
Frueher baute `cepos_flush` *jedes Mal* das komplette Paket, nur um danach
festzustellen, dass sich nichts geaendert hat. Dabei fielen an: ein Aufruf der
TeamSpeak-API (`getClientSelfVariableAsString` plus `freeMemory`) fuer den
eigenen Nickname, die Berechnung der Sprechdistanz und etwas Trigonometrie —
rund 33× pro Sekunde, auch wenn der Spieler stillstand.

Jetzt in dieser Reihenfolge (`cepos_flush`):

```text
1. Schon was zu tun?   Erster Send / Keepalive (1 s) / Cache ungueltig  -> senden
2. Position bewegt?    |dx|,|dy|,|dz| >= 0,08 m gegenueber dem letzten Send -> senden
3. Nickname (max. 1×/s neu gelesen) anders als zuletzt gesendet?       -> senden
4. Sprechdistanz anders (>= 0,05 m)?                                    -> senden
sonst: nichts tun, Flag loeschen (kein Paket, kein Base64, kein TS-API-Aufruf)
```

Das Paket (`cepos_build_local`) wird erst gebaut, wenn wirklich gesendet wird —
mit der schon ermittelten Distanz und dem schon bekannten Namen.

**Eigener Nickname: hoechstens 1× pro Sekunde neu lesen.** Der Name steckt im
Cache `g_ownNick` (schon auf die 15 Zeichen des Wire-Felds gekuerzt). Neu
gelesen wird, wenn der Cache aelter als `CEPOS_NICK_REFRESH_MS` (1000 ms) ist,
beim Keepalive und nach einem Reset (neue Verbindung). Eine Umbenennung
erreicht die Peers dadurch mit **bis zu ~1 s Verzoegerung** (frueher ~30 ms) —
fuer eine Namensanzeige unkritisch. Schlaegt das Lesen fehl, bleibt der letzte
gute Name (vorher: kurzzeitig `Player`, was die Peers umschalten liess);
`Player` ist nur noch die Rueckfalloption, solange noch nie ein Name gelesen
wurde.

**Was bewusst gleich schnell bleibt:** Bewegung, Moduswechsel (Cache-Invalidierung),
Einstellungsaenderung und Distanzaenderung gehen weiterhin **im naechsten
30-ms-Tick** raus. Die Sprechdistanz wird dafuer bei jedem Flush neu ermittelt —
sie kann sich auch ohne Bewegung aendern (Hub-Beschreibung, Rasse), und das soll
sofort bei den Peers ankommen. Der Keepalive behaelt seine Periode von 1 s.

### 4c.2 Namen vom Peer sind Fremdeingabe

`playerName` kommt unbereinigt von einem anderen Client (16 Byte, evtl. ohne
Endnull). Er wird in die Spielertabelle geschrieben, im UI angezeigt und ins
Log geschrieben. Ein Name mit eingebettetem Zeilenumbruch haette eine
gefaelschte Logzeile erzeugt (*Log Injection*). Jetzt ersetzt
`cepos_wire_sanitize_name` (`ts3_cepos_wire.h`) jedes Byte `< 0x20` und `0x7F`
durch `?`, **bevor** der Name gespeichert oder geloggt wird. Bytes `>= 0x80`
bleiben (UTF-8-Namen wie „Jörg“ bleiben heil). Das Wire-Format aendert sich nicht.

### 4c.3 Was mit empfangenen Positionen passiert (3D, kein Wire-Kommando)

Nach dem Empfang schreibt `ts3_3d` die Positionen der hoerbaren Peers per
`channelset3DAttributes` in TeamSpeak (fuer die Richtung beim Hoeren). Seit 083
gilt dort:

| Regel | Warum |
|---|---|
| Schlug der API-Aufruf fuer einen Client fehl, wird er **1 s lang nicht erneut versucht** | sonst wurde er bei jedem Durchlauf neu versucht und scheiterte jedes Mal neu |
| Pro `ts3d_apply`-Durchlauf **hoechstens 64** Client-API-Aufrufe; der Rest kommt reihum im naechsten Durchlauf dran | zappelnde Peers kosteten sonst je einen Aufruf pro Durchlauf |
| `ts3d_has_pending()` meldet „Rest wartet“ (hoechstens ~500 ms lang) | damit der Sammel-Durchlauf (CEDRAIN) den Rest zuegig abholt, statt auf den naechsten Zufallstreffer zu warten |

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
unveraendert weiter. Und: **Verkehr kostet im Relay-Netz quadratisch** — alles,
was ein Server an alle verteilt, braucht eine Drossel, die zum Zweck passt
(Diagnose: 1 Hz; Position: 30 ms; Wake-up: gar nicht ueber den Server).
