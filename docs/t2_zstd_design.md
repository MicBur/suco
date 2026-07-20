# Design-Dokument — T2: zstd-Kompression für Netzwerk-Payloads

Dieses Dokument beschreibt das technische Design für die zstd-Kompression (Standard Level 1) der Netzwerk-Payloads (präprozessierter C++-Quellcode, Header-Set-Quellcode und Objekt-Binärdateien) im verteilten Build-System SUCO.

---

## 1. Übersicht

Um die Netzwerklast bei verteilten Kompiliervorgängen drastisch zu verringern, werden große Payloads komprimiert. 
Preprocessed C++-Quellcode lässt sich typischerweise mit einem Verhältnis von 10:1 komprimieren. Fertige Objektdateien (.o/.obj) lassen sich typischerweise mit 1.5:1 bis 2:1 komprimieren.

---

## 2. Netzwerk-Protokoll-Erweiterung (Kompatibilität & Rollback-Fähigkeit)

Für jede übertragenen Payload (`preprocessed_source`, `header_set_source`, `binary`) fügen wir im Netzwerk-Stream unmittelbar vor der Länge ein Kompressions-Flag ein:

1. **compression_flag** (1 Byte, `uint8_t`):
   * `0`: Payload ist unkomprimiert (none)
   * `1`: Payload ist mit `zstd` komprimiert
2. **payload_len** (4 Bytes, `uint32_t` in Network Byte Order):
   * Die tatsächliche Länge der nachfolgenden Bytes auf der Leitung (komprimiert oder unkomprimiert).
3. **payload_data**:
   * Die Rohdaten (Länge = `payload_len`).

### Rollback-Fähigkeit & Schwellenwerte
* **Schwellenwert (~4 KB):** Payloads unter 4096 Bytes werden unkomprimiert gesendet (Flag = `0`), um Rechenzeit für kleine Übertragungen zu sparen.
* **Escape-Hatch (`SUCO_COMPRESSION=off`):** Wird diese Umgebungsvariable auf `off` gesetzt, senden Client und Worker alle Payloads unkomprimiert (Flag = `0`).
* **Sicherheit:** Empfänger dekomprimieren den Datenstrom nur, wenn das `compression_flag == 1` ist. Falls das Flag `0` ist, wird die Payload direkt als Plaintext/uncompressed eingelesen.

---

## 3. Schutz vor bösartigen Frames (Obergrenzen)

Um Speicherüberläufe oder Angriffe durch korrupte oder manipulierte zstd-Frames zu verhindern, prüft `decompress_zstd` vor der Dekompression die dekomprimierte Größe im Zstd-Header (`ZSTD_getFrameContentSize`):
* Die Obergrenze liegt bei **1 GB**.
* Frames, die diese Grenze überschreiten, ungültig sind (`ZSTD_CONTENTSIZE_ERROR`), oder deren Größe nicht bestimmbar ist (`ZSTD_CONTENTSIZE_UNKNOWN`), werden abgelehnt (Rückgabe eines leeren Buffers).
* Fehlerverhalten (präzise, je Empfänger):
  * **Worker:** Schlägt die Dekompression einer Quell-/Header-Payload fehl, bricht der Worker den Job ab und sendet eine `PACKET_COMPILE_RESP` mit Exit-Code `-99` und Fehlermeldung an den Coordinator (`worker.cpp`, Dekompressions-Fehlerpfad). Der Worker führt keinen eigenen Fallback aus.
  * **Client:** Erhält der Client eine fehlgeschlagene Antwort oder kann eine empfangene Binary-Payload nicht dekomprimieren, greift der bestehende Client-Fallback und kompiliert lokal (Fehlertoleranz Ende-zu-Ende).

---

## 4. Cache-Integration auf dem Coordinator

Der Coordinator bleibt eine effiziente Weiterleitungsstation:
* Der Coordinator dekomprimiert die Quellcodedaten nicht selbst, sondern leitet die komprimierten Bytes transparent an die Worker weiter.
* Der Coordinator-Cache (`LruCache`) speichert die Objektdatei inklusive des Compression-Flags ab. Dazu wird das Compression-Flag (1 Byte, 0 oder 1) als erstes Byte der gecachten `.o`-Datei auf der Festplatte abgelegt, gefolgt von den binären Daten.
* Bei einem Cache-Hit liest der Coordinator dieses erste Byte, trennt es ab und sendet das passende Flag sowie die Binärdaten an den Client zurück.
