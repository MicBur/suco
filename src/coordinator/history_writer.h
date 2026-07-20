#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include <sqlite3.h>

namespace suco {

class HistoryWriter {
public:
    struct Event {
        uint64_t job_id = 0;
        std::string source_file;
        std::string content_hash;
        std::string worker_name; // Leer bei Cache-Hit
        bool cache_hit = false;

        // Zeitstempel (absolute Epochen-Millisekunden via system_clock)
        int64_t queue_start_ms = 0;
        int64_t dispatch_ms = 0;    // 0 bei Cache-Hit
        int64_t compile_end_ms = 0; // 0 bei Cache-Hit
        int64_t result_ms = 0;

        // Dauern (in Millisekunden)
        int64_t queue_duration_ms = 0;
        int64_t compile_duration_ms = 0;
        int64_t total_duration_ms = 0;
        int32_t exit_code = 0;
    };

    explicit HistoryWriter(const std::string& db_path);
    ~HistoryWriter();

    // Verhindere Kopieren
    HistoryWriter(const HistoryWriter&) = delete;
    HistoryWriter& operator=(const HistoryWriter&) = delete;

    // Hot-Path API: Enqueues event. Niemals blockierend auf I/O.
    void enqueue(Event ev);

    // Berechne die gesamte Zeitersparnis für heute in Millisekunden
    int64_t get_time_saved_today_ms();

    // Liefert die letzten `limit` Build-Events (neueste zuerst) für die Dashboard-
    // Timeline/Gantt-Ansicht (D2). Öffnet eine eigene Read-Only-Verbindung, damit der
    // Writer-Thread nicht berührt wird.
    std::vector<Event> get_recent_events(int limit);

private:
    void writer_loop();
    bool init_db();
    void flush_batch(std::vector<Event>& batch);

    std::string db_path_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr;

    std::mutex queue_mutex_;
    std::deque<Event> queue_;
    std::condition_variable queue_cv_;
    std::atomic<bool> shutdown_{false};
    std::thread writer_thread_;

    static constexpr size_t BATCH_SIZE = 64;
    static constexpr int FLUSH_MS = 500;
    static constexpr size_t MAX_QUEUE = 16384;
};

} // namespace suco
