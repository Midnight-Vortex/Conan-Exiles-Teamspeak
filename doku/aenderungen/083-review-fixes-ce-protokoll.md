# 083 — Review-Fixes: CE-Protokoll (CEPING, CEMODE, CEPOS, 3D)

**Bezug:** Code-Review aller Module · **Paket:** PROTOCOL · **Modul-Doku:** `doku/module/ce-protokoll.md`

Ein vollstaendiger Code-Review hat im Protokoll-Teil sechs Probleme gefunden.
Dieses Dokument beschreibt pro Problem: **was** geaendert wurde, **wie es vorher
war**, **warum es jetzt besser ist**, **wie es funktioniert** und **wie es
getestet wurde**. Reihenfolge wie im Review.

Geaenderte Dateien (alle unter `src/ts/proximity/`):

| Datei | Fix |
|---|---|
| `ts3_ceping.c`, `ts3_ceping.h` | 1, 2 |
| `ts3_ceping_wire.h` | 2 (neue reine Logik `ceping_track_observe`) |
| `ts3_cemode.c`, `ts3_cemode.h` | 3 |
| `ts3_cepos.c`, `ts3_cepos.h` | 4, 5 |
| `ts3_cepos_wire.h` (**neu**, nur Header) | 5 (`cepos_wire_sanitize_name`) |
| `ts3_3d.c`, `ts3_3d.h` | 6 |

Tests: `tests/ceping_wire_test.c` (erweitert), `tests/cepos_wire_test.c` (neu),
`tests/proto_flush_test.c` (neu, uebersetzt die **echten** Produktionsdateien
gegen Stubs), dazu die Test-Shims `tests/support/proto_shim/`.

---

## Fix 1 — CEPING wieder nur 1× pro Sekunde

### Was wurde geaendert?

`ts3_ceping.c`: `CEPING_SEND_MIN_MS` ist wieder `1000` (statt
`PLUGIN_POLL_INTERVAL_MS` = 30). Kommentare in `ts3_ceping.h` und
`ts3_ceping_wire.h` sowie `doku/module/ce-protokoll.md` (Tabelle, Wire-Regel 6,
Abschnitt 4a) passen dazu.

### Wie war es vorher?

Der Commit `dda9cbe` („Put CEPING, CEMODE and CEAUTH sends on the 30 ms poll
tick“) hat CEPING von 1000 ms auf den 30-ms-Tick gezogen. Der urspruengliche
Entwurf (`doku/aenderungen/034`) wollte aber ausdruecklich ~1 Hz.

### Warum ist es jetzt besser?

TeamSpeak-Plugin-Commands gehen **ueber den Server an alle Clients**. Bei N
Spielern verursacht jede Nachricht N Zustellungen — also N² im Server, wenn alle
senden. CEPOS braucht diesen Takt (Positionen!), CEPING ist aber **reine
Diagnose** (Verlust-/Veraltungserkennung im Log). Bei 30 ms kostete die
Diagnose genauso viel Server-Verkehr wie die Position selbst; bei 1 Hz sind es
~97 % weniger.

Vorher geprueft (Review-Auftrag: „nichts darf auf 30 ms bauen“):

- Einziger Empfaenger-Code: `ts3_ceping_on_plugin_command` — er **loggt nur**
  Sequenzluecken. Kein Info-Panel, kein Audio, keine Tabelle liest CEPING
  (`grep` ueber `src/`: nur `ts3_ceping.*` und die Aufrufe in `ts3_entry.c`).
- Die Veraltungs-Schwelle der Spielertabelle (`PLAYER_TABLE_STALE_MS` = 120 s)
  ist unabhaengig von CEPING.
- Folglich sind **keine Schwellen anzupassen**. Einzig die *Bedeutung* eines
  Sprungs aendert sich: `seq` +2 heisst jetzt „eine Sekunde fehlt“, vorher „ein
  30-ms-Tick fehlt“.

### Wie funktioniert es jetzt?

```text
Positions-Tick (~30 ms) ──► ts3_ceping_signal_send_pending()  (Flag + Wakeup)
CEDRAIN-Durchlauf ──► ts3_ceping_flush()
      letzter Send < 1000 ms her?  ja  -> Flag verwerfen (kein Spin), naechster Tick meldet neu
                                   nein -> seq++, "CEPING:1;<seq>" senden
```

Der Positions-Tick bleibt der Ausloeser, also weiterhin **kein Verkehr im
Leerlauf**; nur der Mindestabstand ist 1 s.

### Wie wurde es getestet?

`tests/proto_flush_test.c`: 3 simulierte Sekunden mit 30-ms-Ticks ergeben 3–4
Heartbeats (statt ~100); der erste geht sofort raus (`seq` 1), der naechste hat
`seq` 2. Gegen den alten Code laeuft der Test rot.

### Lerneffekt

„Eine Uhr fuer alles“ klingt sauber, ignoriert aber die Kosten: Was der Server an
alle verteilt, muss zur Wichtigkeit der Daten passen. Diagnose darf langsam sein.

---

## Fix 2 — CEPING: keine Scheinverluste, kein Log-Spam

### Was wurde geaendert?

- `ts3_ceping_wire.h`: neue reine Logik `CepingTrack` + `ceping_track_observe()`
  (Zustand pro Peer, Entscheidung „weiter / ignorieren / neu aufsetzen“,
  Log-Drossel). Konstanten `CEPING_REORDER_WINDOW` (16) und
  `CEPING_LOSS_LOG_MIN_MS` (1000).
- `ts3_ceping.c`: die Peer-Tabelle ist jetzt `CepingTrack g_peers[]`; die
  Empfangsfunktion ruft nur noch `ceping_track_observe()` auf und loggt, wenn
  die Funktion es erlaubt.

### Wie war es vorher?

```c
missed = ceping_seq_gap(peer->lastSeq, seq);   // Luecke zur letzten
if (missed > 0) log_debug("... lost %lu", missed);
peer->lastSeq = seq;                            // IMMER, auch bei aelterer seq
```

Zwei Fehler:

1. **Scheinverluste bei vertauschter Reihenfolge.** Ankunft 10, 12, 11, 13:
   bei 12 → „1 verloren“; `lastSeq` springt bei 11 *zurueck* auf 11; bei 13
   → „1 verloren“. Zwei Meldungen, obwohl nichts fehlt.
2. **Log-Flut.** Jede Nachricht mit grosser Luecke schrieb eine Zeile; die `seq`
   waehlt der (evtl. boeswillige) Peer selbst → beliebig viele Logzeilen.

### Warum ist es jetzt besser?

- `lastSeq` rueckt **nur bei echten Vorwaertsspruengen** vor
  (`0 < diff <= 0x7FFFFFFF`, ueberlaufsicher). Eine verspaetete Nachricht aendert
  nichts. (Den *einen* Verdacht bei 12 kann niemand vermeiden: in dem Moment ist
  11 wirklich noch nicht da.)
- **Neustart-Regel:** Faellt `seq` um mehr als 16 zurueck, hat der Peer sein
  Plugin neu gestartet → die neue Zahl wird die neue Basis (ohne Verlustmeldung).
  Ohne diese Regel waere ein neu gestarteter Peer so lange „unsichtbar“, bis sein
  Zaehler den alten Wert eingeholt hat — das waere eine Regression gegenueber
  dem alten „immer ueberschreiben“.
- **Log-Drossel:** pro Peer hoechstens **eine Verlust-Zeile pro Sekunde**; die
  unterdrueckten Ereignisse werden gezaehlt und in der naechsten Zeile genannt
  (`(+18 earlier loss event(s) not logged)`).

### Wie funktioniert es jetzt?

```text
ceping_track_observe(track, seq, nowMs):
  erste Nachricht            -> Basis merken, nichts melden
  diff = (seq - lastSeq) & 0xFFFFFFFF
  diff == 0                  -> Duplikat, ignorieren
  diff > 0x7FFFFFFF (zurueck)-> <= 16 Schritte: verspaetet, ignorieren
                                > 16 Schritte: Peer-Neustart, Basis = seq
  sonst (vorwaerts)          -> lastSeq = seq, missed = diff - 1
                                missed > 0 und >= 1 s seit letzter Zeile -> LOG
                                sonst -> nur zaehlen
```

Thread-Vertrag unveraendert: nur der Callback-Thread liest/schreibt die Tabelle
(kein Lock). Die Funktion selbst ist rein (keine Globals) und deshalb im
Host-Test pruefbar.

### Wie wurde es getestet?

- `tests/ceping_wire_test.c` (Abschnitte [6]–[10]): Baseline, Duplikat,
  Vorwaertssprung, Reihenfolge 10/12/11/13, Wraparound (`FFFFFFFE → FFFFFFFF → 0`,
  Reorder ueber den Ueberlauf, groesster Vorwaertssprung), Neustart-Regel an der
  Fenstergrenze, Drossel (19 Spruenge in 1 s → 0 Zeilen, nach 1 s eine Zeile mit
  Zaehler), Uhr, die rueckwaerts laeuft.
- `tests/proto_flush_test.c` Abschnitt [2] mit dem **echten** `ts3_ceping.c`:
  10/12/11/13 → genau 1 Zeile (alt: 2); Flut → 1 Zeile + „+18 earlier“; Drossel
  ist pro Peer; Neustart wird neu aufgesetzt; ungueltige Client-IDs/Versionen/
  Plugin-Namen werden ignoriert.

### Lerneffekt

Zustand, den ein Fremder bestimmen darf (hier: die Sequenznummer), darf nie
ungefiltert Log oder Zustand steuern. „Nur vorwaerts“ und „maximal 1× pro
Sekunde“ sind die zwei Standard-Schutzregeln.

---

## Fix 3 — CEMODE: ein Moduswechsel geht nicht mehr verloren

### Was wurde geaendert?

`ts3_cemode.c` → `ts3_cemode_flush()`: das „Es ist was zu senden“-Flag wird
**zuerst auf 0 gesetzt (claim)**, **danach** wird der Modus gelesen. Schlaegt der
Send fehl, wird das Flag wieder gesetzt (bis zu 5 Wiederholungen, dann Log-Zeile
und Aufgabe). Modus und Distanz werden aus **einem** gelesenen Modus abgeleitet
(`voice_mode_get_distance(mode)`), damit beide auf dem Draht zusammenpassen.

### Wie war es vorher?

```text
Callback-Thread (flush)               Anderer Thread (Hotkey)
 liest Modus = NORMAL
                                       setzt Modus = SHOUT
                                       signal(): Flag ist noch 1 -> CAS scheitert, kein Wakeup
 sendet "NORMAL"
 loescht Flag (= 0)                    <- der SHOUT-Wechsel ist spurlos weg
```

CEMODE hat kein periodisches Auffrischen (nur Flanken). Die Peers behielten den
alten Modus bis zum naechsten, unabhaengigen Anlass. Ausserdem wurde bei einem
fehlgeschlagenen Send das Flag trotzdem geloescht (Flanke verloren).

### Warum ist es jetzt besser?

Das ist dasselbe Muster, das `audio_recompute_all_impl` (`ts3_proximity_audio.c`)
schon benutzt: **erst das Flag beanspruchen, dann die Eingaben lesen.** Ein
Wechsel, der *nach* dem Claim eintrifft, findet das Flag auf 0, setzt es auf 1 und
weckt den Dispatcher erneut → er bekommt einen eigenen Send. Ein Wechsel *vor*
dem Claim wird vom Lesen noch erfasst. Es gibt keine Luecke mehr dazwischen. Im
schlimmsten Fall wird ein Modus doppelt gesendet (harmlos, 30-ms-Boden).

### Wie funktioniert es jetzt?

```text
flush:  Flag==1? -> 30-ms-Boden ok? -> Flag = 0 (CLAIM) -> Modus lesen -> senden
                                                              |
        Wechsel von aussen NACH dem Claim:  Flag 0 -> 1 + Wakeup  ---> naechster flush
        Send fehlgeschlagen:                Flag = 1 (max. 5×)     ---> naechster flush
```

### Wie wurde es getestet?

`tests/proto_flush_test.c` Abschnitt [1] mit dem **echten** `ts3_cemode.c`: ein
Stub-`voice_mode_get_current()` simuliert den anderen Thread *genau nach dem
Lesen* des Modus. Neu: nach dem ersten Flush ist das Flag wieder 1 und der
zweite Flush sendet `CEMODE:1;2;600` (Shout, 60,0 m). Gegen den alten Code ist
der Test rot (Flag 0, Wechsel verloren). Dazu: Send-Fehler mit genau 6
Versuchen und einer Log-Zeile, Wiederherstellung nach einem einzelnen Fehler,
30-ms-Boden.

### Lerneffekt

„Lesen, dann Flag loeschen“ ist ein Race; „Flag loeschen, dann lesen“ nicht. Bei
jedem Dirty-Flag zwischen Threads gilt: **erst claimen, dann Eingaben lesen.**

---

## Fix 4 — CEPOS: Leerlauf kostet fast nichts mehr

### Was wurde geaendert?

`ts3_cepos.c` → `cepos_flush()` / `cepos_build_local()`:

- Die Aenderungspruefung laeuft **zuerst** und ohne Paket: Keepalive faellig /
  erzwungen (Cache ungueltig) / Position bewegt (`CEPOS_POS_EPS` 0,08 m) /
  Nickname geaendert / Sprechdistanz geaendert (`CEPOS_VOICE_EPS` 0,05 m).
- Das Paket wird **nur gebaut, wenn wirklich gesendet wird**, mit der schon
  ermittelten Distanz und dem schon bekannten Namen.
- Der eigene Nickname kommt aus einem Cache (`g_ownNick`) und wird hoechstens
  alle `CEPOS_NICK_REFRESH_MS` = 1000 ms neu gelesen (ausserdem beim Keepalive
  und nach `cepos_reset()`).

### Wie war es vorher?

Bei **jedem** Flush (~33× pro Sekunde, auch im Stillstand) wurde das ganze Paket
gebaut: `ts3_get_own_nickname` (TS-API-Aufruf + `freeMemory`),
`voice_mode_get_current_distance` (Hub-Profil-Kopie, Zonen-Aufloesung), Trigonometrie,
String-Vergleich — nur um dann „unveraendert“ festzustellen und alles zu verwerfen.

### Warum ist es jetzt besser?

Der teuerste Teil, der TS-API-Aufruf, faellt im Leerlauf von ~33/s auf ~1/s. Die
Paketkonstruktion entfaellt im Leerlauf ganz. **Verhalten bleibt gleich**, wo es
zaehlt: Bewegung, Moduswechsel, Einstellungen und Distanzaenderung werden im
naechsten 30-ms-Tick gesendet, der Keepalive bleibt bei 1 s.

Bewusste Abwaegungen (konservativ gewaehlt):

- **Namensaenderungen** erreichen die Peers mit bis zu ~1 s Verzoegerung (vorher
  ~30 ms).
- Die **Sprechdistanz** wird weiterhin bei jedem Flush ermittelt (eine reine
  Rechnung, kein TS-API-Aufruf), damit Aenderungen ohne Bewegung (Hub-Profil,
  Rasse) sofort rausgehen. Wer auch das einsparen will: Abfrage im Leerlauf auf
  ~4 Hz begrenzen — dann waeren solche Aenderungen bis zu 250 ms verzoegert.
- Schlaegt das Lesen des Namens fehl, bleibt der **letzte gute Name** (vorher
  flackerte er kurz auf `Player`).

### Wie funktioniert es jetzt?

```text
Tick (30 ms) -> cepos_flush
   Rate-Limit (30 ms)?              -> spaeter nochmal
   Position gueltig?                -> sonst Flag loeschen
   sendNow = erster Send || Keepalive || Position bewegt
   Nickname-Cache aelter als 1 s (oder Keepalive)?  -> 1× TS-API-Aufruf
   Name im Cache != zuletzt gesendeter Name?        -> sendNow
   Distanz != zuletzt gesendete Distanz?            -> sendNow
   !sendNow -> Flag loeschen, fertig  (kein Paket, kein Base64)
   sendNow  -> Paket bauen, Base64, senden
```

### Wie wurde es getestet?

`tests/proto_flush_test.c` Abschnitt [3] mit dem **echten** `ts3_cepos.c`, Stubs
zaehlen die API-Aufrufe: 3 s Leerlauf (100 Ticks) → nur 2 zusaetzliche Sends
(die Keepalives), Nickname-API ≤ 4 Aufrufe (alt: 101); 1 m Bewegung → Send im
naechsten Tick, 5 cm → kein Send; Distanz 30→40 m → Send im naechsten Tick;
Cache-Invalidierung → Send; Umbenennung → Send nach ≤ ~1 s mit neuem Namen;
Namens-API faellt aus → alter Name bleibt; nie ein Name → `Player`; langer Name
→ 15 Zeichen; 30-ms-Boden mit Retry-Wakeup; `cepos_reset` liest den Namen neu.

### Lerneffekt

Eine Entscheidung „muss ich etwas tun?“ soll billiger sein als die Arbeit selbst.
Erst vergleichen, dann bauen — und teure Fremdaufrufe (TS-API) hinter einen
Cache mit klarer Verfallszeit stellen.

---

## Fix 5 — Log-Injection ueber den Spielernamen

### Was wurde geaendert?

`ts3_cepos_wire.h` (neu): `cepos_wire_sanitize_name()`. `ts3_cepos.c` ruft sie in
`cepos_on_plugin_command()` auf, direkt nachdem der Name aus dem Paket kopiert und
terminiert wurde — also **bevor** er in die Spielertabelle geschrieben oder
geloggt wird. Das Wire-Format bleibt unveraendert.

### Wie war es vorher?

Der Name (16 Byte, vom Peer) war nur auf Laenge begrenzt, nicht auf Inhalt. Ein
Name mit Zeilenumbruch oder Steuerzeichen landete mit `%s` im Log
(`CEPOS: RECV from=… name='…'`) und konnte dort **gefaelschte Logzeilen**
erzeugen (oder Terminal-Steuersequenzen, wenn jemand das Log mit `cat` ansieht).

### Warum ist es jetzt besser?

Jedes Byte `< 0x20` und `0x7F` wird durch `?` ersetzt; Bytes `>= 0x80` bleiben,
damit UTF-8-Namen (Umlaute, Emoji) heil bleiben. Das ist die schmalste Regel, die
Log-Injection verhindert, ohne sichtbare Namen zu aendern.

### Wie funktioniert es jetzt?

```text
Paket-Name (16 Byte) -> in char[17] kopieren, Endnull erzwingen
   -> cepos_wire_sanitize_name(): bis zum ersten NUL, Byte<0x20 oder 0x7F -> '?'
   -> player_table_put(...) und log_debug(...)  (beide sehen nur den bereinigten Namen)
```

### Wie wurde es getestet?

- `tests/cepos_wire_test.c`: alle Bytes `0x01..0x1F` und `0x7F` einzeln, ASCII/UTF-8
  bleibt unveraendert, Stopp am ersten NUL, `size`-Grenze, `NULL`.
- `tests/proto_flush_test.c` Abschnitt [4] mit dem **echten** Empfaenger: ein Paket
  mit `\n`, `\r`, ESC und DEL im Namen → Tabelle bekommt `Ev?il??[31mX?!`, und
  **keine** gespeicherte Logzeile enthaelt ein Steuerzeichen. Alter Code → rot.

### Lerneffekt

Alles, was von einem anderen Client kommt, ist Fremdeingabe — auch ein „harmloser“
Spielername. Saeubern am Eingang, bevor es irgendwo gespeichert oder geloggt wird.

---

## Fix 6 — 3D: Fehler-Backoff und Obergrenze pro Durchlauf

### Was wurde geaendert?

`ts3_3d.c` / `ts3_3d.h`:

- **Negativ-Cache:** schlaegt `channelset3DAttributes` fuer einen Client fehl,
  wird er `TS3D_RETRY_BACKOFF_MS` (1000 ms) lang uebersprungen
  (`g_retryUntilMs[TS3_MAX_CLIENT_ID]`, grenzgeprueft ueber
  `ts3_client_id_valid`). `ts3d_invalidate_client()` und `ts3d_reset()` loeschen
  den Backoff.
- **Obergrenze:** ein `ts3d_apply()`-Durchlauf macht hoechstens
  `TS3D_MAX_SETS_PER_PASS` = 64 Client-API-Aufrufe; der Rest wird im naechsten
  Durchlauf **reihum** (Round-Robin ueber den Snapshot) abgearbeitet.
- **Neu:** `ts3d_has_pending()` — „ein begrenzter Durchlauf hat Rest hinterlassen“
  (hoechstens ~500 ms lang wahr, damit nichts im Kreis laufen kann).

### Wie war es vorher?

- Ein Client, dessen Aufruf scheitert, wurde **bei jedem Durchlauf** erneut
  versucht (es wird ja nichts gespeichert → keine Deduplizierung) und scheiterte
  jedes Mal neu — endlos.
- Bei vielen zappelnden Peers (Position springt > 0,25 m pro Durchlauf) kostete
  jeder Peer einen API-Aufruf pro Durchlauf, ohne Obergrenze.

### Warum ist es jetzt besser?

Fehler kosten hoechstens einen Aufruf pro Sekunde und Client; die Last pro
Durchlauf ist nach oben begrenzt (64), ohne dass jemand zu kurz kommt (Reihum).
Gezaehlt werden nur **tatsaechliche API-Aufrufe** (Erfolg oder Fehler);
deduplizierte oder im Backoff liegende Clients kosten nichts und zaehlen nicht.

### Wie funktioniert es jetzt?

```text
ts3d_apply (Callback-Thread, hoechstens alle 50 ms):
  Snapshot der Spielertabelle; Start = g_rrCursor
  fuer jeden Eintrag (reihum):
     ausserhalb der Hoerweite?  -> weiter
     ts3d_push_client():  unveraendert (<0,25 m)?  -> kein Aufruf
                          im Backoff?              -> kein Aufruf
                          Aufruf: ok -> Position merken | Fehler -> Backoff 1 s
     64 Aufrufe erreicht?  -> g_rrCursor merken, pending (500 ms), Schluss
  alles abgearbeitet -> Cursor 0, nicht pending
```

**Wie wird der Rest abgeholt?** Heute loest 3D nichts selbst aus: `ts3d_apply` laeuft
in jedem CEDRAIN-Durchlauf (`ts3_entry.c`), und Durchlaeufe entstehen durch
andere Arbeit (empfangene CEPOS, eigener Tick). Das bleibt so — mit vielen
Spielern kommt staendig neue Arbeit herein, der Rest wird also von selbst
nachgeholt. Damit es auch ohne weiteren Anlass zuegig geht, gibt es
`ts3d_has_pending()`; ADAPTER kann es in `ts3_pending_work_any()` eintragen
(siehe Abschnitt „Abstimmung“). Ohne diesen Eintrag ist das Verhalten gleich wie
heute (kein Rueckschritt).

### Wie wurde es getestet?

`tests/proto_flush_test.c` Abschnitt [5] mit dem **echten** `ts3_3d.c`:

- Fehlerfall: 1 s lang Durchlaeufe → genau **ein** Wiederholungsversuch (alt: ~19);
  nach der Backoff-Zeit klappt es, danach wird dedupliziert; `invalidate` hebt den
  Backoff sofort auf.
- Obergrenze: 200 Clients mit neuer Position → Durchlaeufe mit 64/64/64/8 Aufrufen,
  jeder Client **genau einmal** bedient, danach keine weiteren Aufrufe.
- `ts3d_has_pending()`: wahr nach einem begrenzten Durchlauf, falsch nach dem
  letzten, laeuft nach 500 ms von selbst ab, `ts3d_reset()` loescht es.
- 100 scheiternde Clients: 64 Versuche, dann nur die 36 noch nie versuchten, dann
  keine mehr.
- Ausserhalb der Hoerweite: kein Aufruf.

### Lerneffekt

Wer einen Fehler nicht merkt, wiederholt ihn endlos. Jede Wiederholung braucht
einen Backoff, jede Schleife ueber Fremddaten eine Obergrenze.

---

## Hinweis 7 — Sprechdistanz des Peers in 3D (nur Befund)

`ts3d_in_hear_range` (Culling in `ts3d_apply`) verwendet `voiceDistance` des Peers
**ungeklemmt**. Beim Empfang wird sie nur auf `0..1000 m` geprueft
(`cepos_on_plugin_command`). Folgen: Ein Peer mit 1000 m erzwingt nur 3D-Updates
**fuer sich selbst** (hoechstens ein Aufruf pro 0,25 m Bewegung) — durch die
64er-Obergrenze nun begrenzt. Wichtiger ist die *Konsistenz* mit dem Audio-Pfad:
verwendet AUDIO kuenftig eine **geklemmte** effektive Distanz, sollte 3D dieselbe
Funktion benutzen. Klemmt AUDIO nach unten, macht 3D unnoetige Aufrufe fuer
unhoerbare Peers (harmlos, begrenzt); klemmt es nach oben, wuerde 3D hoerbare Peers
aussortieren (veraltete Richtung). Es wurde bewusst **nichts** dupliziert; sobald
der Clamp als gemeinsame Funktion existiert, ist eine Zeile in `ts3d_in_hear_range`
zu aendern.

---

## Abstimmung mit anderen Paketen

- **CEDRAIN nur an sich selbst (Paket ADAPTER):** Der Wakeup-Thread sendet
  `CEDRAIN:1` jetzt nur an den eigenen Client (`PluginCommandTarget_CLIENT`) und
  faellt automatisch auf den serverweiten Versand zurueck, wenn der TS-Server ein
  selbst adressiertes Command ca. 2 s lang nicht zuruecksendet. Empfaenger
  ignorieren CEDRAIN anderer Clients. Beschrieben in `ce-protokoll.md` 2.3.
  Gleiche Wurzel wie Fix 1: unnoetiger N²-Verkehr ueber den Server.
- **`ts3d_has_pending()`** muss ADAPTER in `ts3_pending_work_any()` eintragen
  (`|| ts3d_has_pending()`), damit der Rest eines begrenzten 3D-Durchlaufs
  zuegig abgeholt wird. Nicht zwingend (siehe Fix 6).
- `src/core/util/poll_interval.h` nennt in seinem Kommentar noch CEPING als
  Nutzer des 30-ms-Bodens; das stimmt seit Fix 1 nicht mehr (nur Kommentar).

## Gesamt-Tests

| Suite | Was |
|---|---|
| `ceping_wire_test` | Codec + neue Empfangslogik (70 Pruefungen) |
| `cepos_wire_test` (neu) | Namens-Bereinigung |
| `proto_flush_test` (neu) | echte `ts3_cemode.c`/`ts3_ceping.c`/`ts3_cepos.c`/`ts3_3d.c` gegen Stubs, Fake-Uhr (75 Pruefungen) |
| `cemode_wire_test`, `ceauth_wire_test` | unveraendert gruen |

`proto_flush_test` gegen die **alten** Dateien aus `HEAD` uebersetzt: 29 Pruefungen
schlagen fehl — jeder der Fixes 1–6 wird also wirklich vom Test erfasst. Cross-Build
(MinGW) der vier geaenderten `.c`-Dateien: keine Warnungen.

## Manueller Test im TS-Client (nach Deploy)

1. **Zwei Clients**, gleicher Server/Ingame-Kanal, Debug-Log an.
2. **CEPING (1, 2):** Im Log von B erscheinen keine „lost“-Zeilen. Kurze
   Netzstoerung bei A → hoechstens **eine** Zeile pro Sekunde von B
   (`peer A lost N update(s)`); A neu starten (Plugin aus/an) → keine Phantom-Zeile.
3. **CEMODE (3):** A wechselt rasch mehrfach den Modus (Hotkeys Fluestern/Normal/
   Rufen); B zeigt im Info-Panel nach dem letzten Wechsel den **letzten** Modus.
4. **CEPOS (4):** A steht still: im Log keine Flut; Position von A erscheint bei B
   weiterhin 1×/s (Keepalive). A laeuft los → B sieht die Bewegung sofort. A
   benennt sich um → B zeigt den neuen Namen nach ≤ ~1 s.
5. **Namens-Injection (5):** Nur mit manipuliertem Client pruefbar; sonst deckt
   der Host-Test den Fall ab.
6. **3D (6):** Mit vielen Spielern in Reichweite bleibt die Richtung der Stimmen
   korrekt (links/rechts); im Log keine Wiederholung von 3D-Fehlern.

## Risiken

- Namensaenderung bis ~1 s verzoegert sichtbar (Fix 4, dokumentiert).
- 3D-Positionen vieler gleichzeitig zappelnder Peers werden reihum in Blocks zu 64
  aktualisiert (spaetestens nach wenigen 50-ms-Durchlaeufen alle).
- Fix 3: bei dauerhaft fehlschlagendem Senden maximal 6 Versuche, danach Log-Zeile
  und Aufgabe (bis zum naechsten Moduswechsel).
- Fix 2: Ein Peer, dessen Zaehler um mehr als 16 zurueckspringt, wird als Neustart
  gewertet (kein Verlust gemeldet) — gewollt.
