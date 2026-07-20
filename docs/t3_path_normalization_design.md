# Design-Dokument — T3: Pfad-Normalisierung für teamweite Cache-Hits

Dieses Dokument beschreibt das technische Design und die Funktionsweise der Pfad-Normalisierung im verteilten Build-System SUCO.

---

## 1. Motivation und Ziel

Standardmäßig enthalten preprozessierte Quellcodedateien und Compiler-Optionen absolute Pfade des lokalen Checkout-Verzeichnisses (z. B. in Include-Pfaden, `-I` Flags oder Makros). 
Kompilieren zwei Entwickler den exakt gleichen Code in unterschiedlichen Verzeichnissen (z. B. `/home/user1/project` and `/home/user2/project`), erzeugt das unterschiedliche Cache-Hashes und verhindert cache-hits.

Die Pfad-Normalisierung löst dieses Problem, indem absolute Pfade des Checkout-Roots vor der Cache-Schlüssel-Berechnung durch einen universellen Platzhalter ersetzt werden.

---

## 2. Funktionsweise

1. **Checkout-Root-Erkennung (`detect_checkout_root`)**:
   Der Client bestimmt das Root-Verzeichnis des Checkouts, indem er vom aktuellen Arbeitsverzeichnis nach oben wandert und nach Steuerordnern (`.git` oder `.hg`) sucht. Falls kein Repository-Ordner gefunden wird, dient das CWD als Fallback.
   Zur Optimierung der Performance wird dieses Ergebnis einmalig pro Prozess gecached (`static` Variable).
2. **Ersetzung (`normalize_paths`)**:
   Jeder Pfad in den Hashing-Eingaben (z. B. Include-Pfade, Flags, präprozessierter Quelltext), der mit dem Checkout-Root beginnt, wird durch den Platzhalter `{SUCO_ROOT}` ersetzt.
3. **Compiler-Unterstützung (`-ffile-prefix-map`)**:
   Wenn `path_normalization` aktiv ist (Standard: `true`), fügt der Client dem Remote-Kompilierbefehl für GCC und Clang den Parameter `"-ffile-prefix-map=<root>=."` hinzu. Dadurch bildet der Compiler auch in den DWARF-Debug-Symbolen der Objektdateien das absolute Verzeichnis auf `.` ab.

---

## 3. Escape-Hatch und Einschränkungen (Trade-off)

* **Escape-Hatch (`SUCO_PATH_NORMALIZATION=off`)**:
  Kann über die Umgebungsvariable `SUCO_PATH_NORMALIZATION=off` (oder `0` / `false`) komplett deaktiviert werden.
* **Trade-off & Kompromiss-Modell**:
  Wie beim Vorbild ccache (Option `CCACHE_BASEDIR`) garantiert ein identischer Cache-Hash *keine* bit-identischen Objektdateien, wenn der Quellcode selbst absolute Pfade über String-Literale oder das `__FILE__`-Makro einbettet, da diese beim Preprocessing nicht verändert werden. 
  Die Verwendung von `-ffile-prefix-map` fängt dies für Debug-Symbole ab, aber eingebettete Strings im ausführbaren Code verbleiben lokal unterschiedlich.
