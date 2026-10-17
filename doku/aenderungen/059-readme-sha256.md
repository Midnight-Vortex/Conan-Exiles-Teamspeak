# 059 — SHA-256-Pruefsumme im GitHub-README

## 1. Was wurde geaendert?

| Datei | Aenderung |
|---|---|
| `README.md` | Neuer Abschnitt „Verify your download (SHA-256)“: Hashes von Version 8.0.6 (Paket + DLL) und Pruefbefehle fuer PowerShell, `certutil`, `sha256sum` |
| `doku/aenderungen/059-readme-sha256.md` | Dieser Eintrag |

**Kein Code geaendert** — nur Dokumentation. Die Plugin-Version bleibt 8.0.6.

---

## 2. Wie war es vorher?

Das README nannte keine Pruefsumme. Wer `conan_exiles-8.0.6-win64.ts3_plugin`
heruntergeladen hat, konnte nicht pruefen, ob die Datei unveraendert ist
(Uebertragungsfehler, ausgetauschte Datei auf einem Spiegel/Fremdserver).

---

## 3. Warum ist die neue Loesung besser?

Eine SHA-256-Summe ist ein „Fingerabdruck“ der Datei: Aendert sich auch nur ein
Bit, kommt ein voellig anderer Wert heraus. Wer den Wert im README (im
Repository, nicht auf der Download-Seite eines Dritten) mit dem eigenen
Download vergleicht, erkennt Manipulation oder kaputte Downloads, bevor die DLL
in den TeamSpeak-Client geladen wird. Das ist wichtig, weil das Plugin als DLL
im TS-Prozess laeuft.

Grenze: Der Hash beweist nur „identisch mit dem veroeffentlichten Stand“, keine
Vertrauenswuerdigkeit. Deshalb steht im README auch der Hinweis, nur Dateien
aus den Releases dieses Repos zu verwenden.

---

## 4. Wie funktioniert es?

```
build.ps1  →  bin\conan_exiles-<version>-win64.ts3_plugin   (ZIP: package.ini + plugins/conan_exiles.dll)
                 │
                 ├─ Get-FileHash  → Hash des Pakets   (README Zeile 1)
                 └─ Hash der DLL im Paket             (README Zeile 2, = bin\conan_exiles.dll)
```

Gemessen am Stand von Commit `0f83ad2` (Build vom 2026-09-03 21:05):

| Datei | SHA-256 |
|---|---|
| `conan_exiles-8.0.6-win64.ts3_plugin` (526056 Bytes) | `5241183845dcda022b105141cf84036c5092455f3105ecc2efed22afa290c138` |
| `plugins/conan_exiles.dll` (7367680 Bytes) | `71c853e493abf3f7dc60510bc246812a010cc9fb14ef0bfdf283df28e1b36d60` |

**Wichtig fuer jede neue Version / jeden Neubau:**

- Der Build ist **nicht reproduzierbar** (DLL enthaelt Zeitstempel, das ZIP ebenfalls).
  Jeder Neubau ergibt andere Hashes, auch ohne Code-Aenderung.
- Der README-Hash muss deshalb zur Datei passen, die als **Release-Asset auf GitHub**
  hochgeladen wird. Reihenfolge: bauen → Hash berechnen → genau diese Datei
  hochladen → Hash ins README → committen.
- Nach einem erneuten `build.ps1` ist der alte Hash im README veraltet, solange
  die neue Datei nicht hochgeladen wird.

Release-Ablauf fuer 8.0.6 (Tag `v8.0.6` auf dem Commit mit diesem README):

```
gh release create v8.0.6 bin\conan_exiles-8.0.6-win64.ts3_plugin --target tsmain \
   --title "v8.0.6" --notes-file <Release-Notes mit den beiden Hashes>
```

Die Release-Notes enthalten dieselbe Hash-Tabelle wie das README. Hochgeladen wird
nur das `.ts3_plugin` (die DLL steckt darin).

Hash berechnen:

```powershell
Get-FileHash .\bin\conan_exiles-8.0.6-win64.ts3_plugin -Algorithm SHA256
```

---

## 5. Wie wurde es getestet?

- Hash des Pakets in `bin\` mit `sha256sum` und `Get-FileHash` berechnet — beide
  Werte stimmen ueberein.
- Paket mit `tar.exe` entpackt: `package.ini` (Version = 8.0.6) und
  `plugins/conan_exiles.dll`; der DLL-Hash im Paket ist identisch mit
  `bin\conan_exiles.dll`.
- Es wurde kein neuer Build ausgefuehrt: die Hashes gehoeren zum vorhandenen Build
  in `bin\` (nach Commit `0f83ad2`, Quellcode seitdem unveraendert).

---

## 6. Lerneffekt

Ohne reproduzierbaren Build gehoert der Hash zum **hochgeladenen** Artefakt, nicht
zum Quellcode-Stand. Ein Hash im README ist ein Teil des Release-Ablaufs, kein
dauerhafter Wert.
