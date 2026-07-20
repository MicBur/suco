# T11 — Dashboard-Ausbau: Design-Dokument

> Dieses Dokument beantwortet die 3 offenen Punkte aus dem T11-Plan-Review
> (brain-claude.md, 2026-07-08). Es ist das verbindliche Referenzdokument
> für die T11-Implementierung.

---

## 1. SQLite-Schema + Schreibpfad

### 1.1 Datenbankpfad

```
Default:   <cache>/history.db       (z. B. ~/.cache/suco/history.db)
Override:  SUCO_HISTORY_DB=/pfad    (absoluter Pfad, überschreibt Default)
```

Wird beim Coordinator-Start angelegt (`CREATE TABLE IF NOT EXISTS`).

### 1.2 Schema

```sql
-- Jeder abgeschlossene (oder fehlgeschlagene) Compile-Job wird hier gespeichert.
CREATE TABLE IF NOT EXISTS build_events (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id              INTEGER NOT NULL,        -- Coordinator-interne Job-ID
    source_file         TEXT    NOT NULL,         -- z. B. "main.cpp"
    content_hash        TEXT    NOT NULL,         -- SHA-256 (Preprocessed + Flags)
    worker_name         TEXT,                     -- Worker-Hostname oder NULL bei Cache-Hit
    cache_hit           INTEGER NOT NULL DEFAULT 0,

    -- Zeitstempel (Unix-Millisekunden, Coordinator-Clock)
    queue_start_ms      INTEGER NOT NULL,         -- Job von Client empfangen
    dispatch_ms         INTEGER,                  -- Job an Worker gesendet (NULL bei Cache-Hit)
    compile_end_ms      INTEGER,                  -- Worker meldet Ergebnis zurück
    result_ms           INTEGER NOT NULL,         -- Coordinator hat Ergebnis an Client gesendet

    -- Dauern (abgeleitet, für schnelle Queries)
    queue_duration_ms   INTEGER,                  -- dispatch_ms − queue_start_ms
    compile_duration_ms INTEGER,                  -- Worker-gemessene reine Compile-Zeit
    total_duration_ms   INTEGER NOT NULL,         -- result_ms − queue_start_ms
    exit_code           INTEGER NOT NULL DEFAULT 0,

    created_at          TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_be_created   ON build_events(created_at);
CREATE INDEX IF NOT EXISTS idx_be_hash      ON build_events(content_hash);
CREATE INDEX IF NOT EXISTS idx_be_file      ON build_events(source_file);
```

**Bewusste Entscheidungen:**
- **Eine Tabelle `build_events`**, kein separates `events`-Table. Worker-Join/Leave
  wird nicht persistiert (nur als SSE-Event gestreamt, s. §2).
- **Keine `build_id`-Spalte** — auf Coordinator-Ebene gibt es keine Build-Invokation,
  nur Einzel-Jobs. Das Dashboard gruppiert per Zeitfenster (z. B. 5s-Cluster).
- **`compile_duration_ms`** kommt direkt aus dem Worker-Result-Paket
  (`BatchProcessor::total_compilation_ms_`), ist also Worker-lokal gemessen.

### 1.3 Schreibpfad: Gepufferte Queue + Writer-Thread

```
                           ┌──────────────────┐
  BatchProcessor           │  bounded deque   │    Writer-Thread
  (Job-Hot-Path)  ──push──▶│  buffer (16384)  │──▶  batch INSERT
                           └──────────────────┘     alle 500ms oder
                                                     wenn ≥64 Events
```

**Garantie: Der Hot-Path blockiert NIEMALS auf SQLite.**

#### Architektur im Detail

```cpp
// Neues Header-File: src/coordinator/history_writer.h

class HistoryWriter {
public:
    struct Event {
        uint64_t    job_id;
        std::string source_file;
        std::string content_hash;
        std::string worker_name;     // leer bei Cache-Hit
        bool        cache_hit;
        int64_t     queue_start_ms;
        int64_t     dispatch_ms;     // 0 bei Cache-Hit
        int64_t     compile_end_ms;
        int64_t     result_ms;
        int64_t     compile_duration_ms;
        int32_t     exit_code;
    };

    explicit HistoryWriter(const std::string& db_path);
    ~HistoryWriter();                     // join() auf Writer-Thread

    // Aufgerufen im Hot-Path — NIEMALS blockierend.
    // Schreibt in eine lock-free SPSC-Queue (oder mutex-geschützte
    // std::deque mit try_lock — Fallback: Event droppen + Warnung).
    void enqueue(Event ev);

private:
    void writer_loop();                   // Läuft in eigenem Thread
    void flush_batch(std::vector<Event>& batch);  // BEGIN; INSERT x N; COMMIT;

    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr; // Prepared Statement (wiederverwendet)

    // Gepufferte Queue
    std::mutex queue_mutex_;
    std::deque<Event> queue_;
    std::condition_variable queue_cv_;
    std::atomic<bool> shutdown_{false};
    std::thread writer_thread_;

    // Konfiguration
    static constexpr size_t BATCH_SIZE   = 64;    // Flush wenn ≥64 Events
    static constexpr int    FLUSH_MS     = 500;   // Flush spätestens alle 500ms
    static constexpr size_t MAX_QUEUE    = 16384;  // Backpressure: älteste droppen
};
```

#### Flush-Algorithmus

```
writer_loop():
    WHILE NOT shutdown:
        wait_for(queue_cv_, FLUSH_MS)           // 500ms Timeout
        lock(queue_mutex_)
        swap(queue_, local_batch)               // O(1), Queue sofort frei
        unlock(queue_mutex_)

        IF local_batch nicht leer:
            BEGIN TRANSACTION
            FOR EACH event IN local_batch:
                sqlite3_bind_*(...) + sqlite3_step()
                sqlite3_reset(insert_stmt_)
            COMMIT
```

#### Backpressure

Wenn `queue_.size() >= MAX_QUEUE` (16384): älteste Events werden
verworfen + `SUCO_LOG_WARN("History queue overflow, dropping oldest events")`.
In der Praxis unmöglich: selbst bei 1000 Jobs/s fasst die Queue 16 Sekunden.

#### Integration in BatchProcessor

```cpp
// In batch_processor.cpp, nach dem Senden des Ergebnisses an den Client:
// (NACH dem send(), also NICHT im kritischen Pfad)

HistoryWriter::Event ev;
ev.job_id           = state_.next_job_id;
ev.source_file      = job.source_file;
ev.content_hash     = job.content_hash;
ev.cache_hit        = result.cache_hit;
ev.queue_start_ms   = queue_start;   // steady_clock bei Job-Empfang
ev.dispatch_ms      = dispatch_time; // steady_clock bei Worker-Dispatch
ev.compile_end_ms   = compile_end;   // steady_clock bei Worker-Result
ev.result_ms        = now_ms();      // steady_clock jetzt
ev.compile_duration_ms = worker_compile_ms;  // aus Worker-Antwort
ev.exit_code        = result.exit_code;
ev.worker_name      = assigned_worker_name;

history_writer_->enqueue(std::move(ev));  // non-blocking
```

#### SQLite-Konfiguration

```cpp
// Beim Öffnen der DB:
sqlite3_exec(db_, "PRAGMA journal_mode = WAL", ...);   // Write-Ahead Log
sqlite3_exec(db_, "PRAGMA synchronous = NORMAL", ...); // Kein fsync pro Commit
sqlite3_exec(db_, "PRAGMA busy_timeout = 1000", ...);  // Falls Dashboard liest
```

WAL-Modus erlaubt gleichzeitiges Lesen (Dashboard-HTTP-Requests) und Schreiben
(Writer-Thread) ohne Blocking.

---

## 2. SSE-Design

### 2.1 Endpoint

```
GET /api/events
```

Response-Header:
```http
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
X-Accel-Buffering: no
```

### 2.2 Event-Format

Jedes Event folgt dem SSE-Standard (W3C Server-Sent Events):

```
id: 42
event: job_complete
data: {"job_id":42,"file":"main.cpp","worker":"node1","cache_hit":false,"queue_ms":12,"compile_ms":340,"total_ms":387,"exit_code":0}

```

#### Event-Typen und Payloads

| Event-Typ | Trigger | Payload-Felder |
|---|---|---|
| `job_complete` | Job-Ergebnis an Client gesendet | `job_id, file, worker, cache_hit, queue_ms, compile_ms, total_ms, exit_code` |
| `job_failed` | Compile-Fehler oder Worker-Ausfall | `job_id, file, worker, exit_code, error` |
| `worker_join` | Worker registriert sich | `name, ip, slots, os, compilers` |
| `worker_leave` | Worker-Heartbeat-Timeout | `name, reason` |
| `stats` | Alle 5s (Polling-Ersatz) | `active_jobs, total_requests, cache_hits, cache_misses, workers, slots_used, slots_total` |

#### Heartbeat (Keepalive)

```
: keepalive

```

Alle **15 Sekunden**. SSE-Kommentar (beginnt mit `:`), wird vom Browser ignoriert
aber hält die TCP-Verbindung offen.

### 2.3 Reconnect-Verhalten

**Strategie: Vollbild-Refresh, kein Event-Replay.**

Begründung:
- Event-Replay erfordert einen Event-Ringbuffer mit ID-Tracking — unnötige Komplexität
- Das Dashboard zeigt ohnehin Live-Daten; nach einem Reconnect braucht es den aktuellen
  Zustand, nicht verpasste Events
- SQLite liefert die historischen Daten bei Bedarf

#### Ablauf bei Reconnect

1. Browser setzt `EventSource` automatisch nach Verbindungsabbruch fort
   (Default: 3s Retry, konfigurierbar via `retry:` Feld)
2. Coordinator sendet **sofort** ein `stats`-Event mit dem aktuellen Vollbild
3. Danach normale Event-Streams

```
retry: 3000

event: stats
data: {"active_jobs":2,"total_requests":142,"cache_hits":89,...}

```

Der `retry: 3000`-Header wird einmalig beim Verbindungsaufbau gesendet.

**`Last-Event-ID` wird NICHT ausgewertet** — bewusste Vereinfachung.
Falls ein Client die `Last-Event-ID` sendet, wird sie ignoriert.

### 2.4 Max-Clients

```cpp
static constexpr size_t MAX_SSE_CLIENTS = 8;
```

- Bei `GET /api/events` wird geprüft ob `sse_clients_.size() < MAX_SSE_CLIENTS`
- Falls voll: `HTTP 503 Service Unavailable` mit `Retry-After: 5`
- In der Praxis: 1–2 Dashboards, 8 ist großzügig
- Dead-Connection-Detection: Beim Schreiben eines Events prüft `send()` den
  Rückgabewert. Bei Fehler (EPIPE/ECONNRESET) wird der Client aus der Liste entfernt.

### 2.5 Implementation in web_server.h

```cpp
// Bestehender HTTP-Handler erweitern:
// if (path == "/api/events") → SSE-Handshake, Socket in sse_clients_ eintragen

// Neues Member in WebServer / Coordinator:
std::mutex sse_mutex_;
std::vector<socket_t> sse_clients_;

// broadcast_event(type, json_payload):
//   lock(sse_mutex_)
//   for each client in sse_clients_:
//     result = send(client, formatted_sse_message)
//     if result < 0: mark for removal
//   remove dead clients
```

---

## 3. Time-Saved-Formel

### 3.1 Exakte Formel

```
time_saved_per_job =
    CASE
        cache_hit:     native_estimate(content_hash)
        grid_compile:  compile_duration_ms − total_duration_ms
                       (nur wenn > 0, sonst 0 — Grid war langsamer)
    END

time_saved_today = Σ time_saved_per_job
                   für alle Jobs mit created_at ≥ Tagesbeginn (UTC)
```

### 3.2 Herkunft der Nativ-Referenzzeit (`native_estimate`)

#### Bei Cache-Hits

Die Nativ-Referenzzeit für einen Cache-Hit ist die **letzte bekannte
`compile_duration_ms` desselben `content_hash`** aus `build_events`:

```sql
SELECT compile_duration_ms
FROM build_events
WHERE content_hash = ? AND cache_hit = 0 AND compile_duration_ms > 0
ORDER BY id DESC
LIMIT 1
```

- Falls kein vorheriger Non-Cache-Hit existiert: **0 ms** (kein Saving zählen).
  Das ist konservativ — lieber unterschätzen als übertreiben.
- Die Compile-Dauer kommt vom **Worker** (nicht vom Client). Annahme:
  Worker-CPU ≈ Client-CPU für Compile-Last. Das ist eine Vereinfachung.

#### Bei Grid-Compiles

Hypothetische Ersparnis = Worker-Compile-Zeit minus Grid-Overhead:

```
saving = compile_duration_ms − total_duration_ms
```

- `compile_duration_ms` = Worker-gemessene reine Compile-Dauer (CPU-Zeit)
- `total_duration_ms` = Ende-zu-Ende im Coordinator (Queue + Transfer + Compile + Return)
- Wenn `saving ≤ 0`: Grid war langsamer als lokal → **0 ms** (kein negativer Saving)
- Effektiv misst das die **Parallelisierungs-Ersparnis**: Weil mehrere Jobs
  gleichzeitig auf verschiedenen Workern laufen, ist die Wall-Clock-Zeit kürzer
  als sequentiell.

> **Wichtig:** Bei j1 (Single-Job) ist `saving` typischerweise negativ
> (Grid-Overhead). Time-Saved wird erst bei j4+ positiv, weil Parallelismus
> den Overhead kompensiert. Das ist korrekt und wird im UI erklärt.

### 3.3 SQL-Query für Dashboard

```sql
-- "Time saved today" (in Millisekunden)
SELECT
    COALESCE(SUM(
        CASE
            WHEN cache_hit = 1 THEN
                -- Letzte bekannte Compile-Zeit des gleichen Hashes
                COALESCE(
                    (SELECT be2.compile_duration_ms
                     FROM build_events be2
                     WHERE be2.content_hash = be1.content_hash
                       AND be2.cache_hit = 0
                       AND be2.compile_duration_ms > 0
                     ORDER BY be2.id DESC
                     LIMIT 1),
                    0
                )
            ELSE
                -- Grid-Compile: nur positives Saving zählen
                MAX(0, compile_duration_ms - total_duration_ms)
        END
    ), 0) AS saved_ms,
    COUNT(*) AS total_jobs,
    SUM(cache_hit) AS cache_hits
FROM build_events be1
WHERE created_at >= strftime('%Y-%m-%dT00:00:00Z', 'now');
```

### 3.4 Ehrlichkeits-Kennzeichnung im UI

Das Dashboard zeigt **drei** Transparenz-Elemente:

#### (a) Label mit Qualifizierung

```
╔═══════════════════════════════════════════════╗
║  Time saved today:  ~12m 34s  (estimated)    ║
║  Cache hit rate:    67% (201/300 jobs)        ║
╚═══════════════════════════════════════════════╝
```

Das **Tilde-Zeichen (`~`)** und das Wort **"(estimated)"** signalisieren,
dass es sich um eine Schätzung handelt.

#### (b) Tooltip bei Hover

> **How is this calculated?**
>
> • Cache hits: Uses the compile time from the last non-cached build of
>   the same source hash as a proxy for "time you would have spent."
>
> • Grid compiles: Counts the difference between worker compile time and
>   total grid overhead. Only positive savings are counted.
>
> • Negative savings (grid slower than local) are capped at zero.
>
> • If no prior compile exists for a cache hit, 0ms is assumed.
>
> This is a best-effort estimate, not a precise measurement.

#### (c) Footer-Hinweis

Im Dashboard-Footer (permanent sichtbar):

```
Metrics are estimates based on worker compile times. See docs/t11_dashboard_design.md for methodology.
```

---

## Anhang: Retention (Vorschlag)

```sql
-- Beim Coordinator-Start: Events älter als 90 Tage löschen
DELETE FROM build_events
WHERE created_at < strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-90 days');
```

Konfigurierbar via `SUCO_HISTORY_RETENTION_DAYS` (Default: 90).
