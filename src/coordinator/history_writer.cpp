#include "history_writer.h"
#include "../common/logging.h"
#include <chrono>
#include <filesystem>

namespace suco {

HistoryWriter::HistoryWriter(const std::string& db_path)
    : db_path_(db_path) {
    // Verzeichnis erstellen, falls es nicht existiert
    try {
        std::filesystem::path p(db_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (const std::exception& e) {
        SUCO_LOG_ERROR("Failed to create database directory for path {}: {}", db_path_, e.what());
    }

    if (init_db()) {
        writer_thread_ = std::thread(&HistoryWriter::writer_loop, this);
        SUCO_LOG_INFO("HistoryWriter initialized successfully with database: {}", db_path_);
    } else {
        SUCO_LOG_ERROR("HistoryWriter failed to initialize SQLite database at {}", db_path_);
    }
}

HistoryWriter::~HistoryWriter() {
    shutdown_ = true;
    queue_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    if (insert_stmt_) {
        sqlite3_finalize(insert_stmt_);
    }
    if (db_) {
        sqlite3_close(db_);
    }
    SUCO_LOG_INFO("HistoryWriter stopped.");
}

void HistoryWriter::enqueue(Event ev) {
    if (shutdown_) return;

    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= MAX_QUEUE) {
        // Backpressure: ältestes Element verwerfen
        queue_.pop_front();
        SUCO_LOG_WARNING("HistoryWriter queue overflow (size >= {}), dropping oldest event.", MAX_QUEUE);
    }
    queue_.push_back(std::move(ev));
    lock.unlock();
    queue_cv_.notify_one();
}

bool HistoryWriter::init_db() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        SUCO_LOG_ERROR("Cannot open SQLite database: {}", sqlite3_errmsg(db_));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    // WAL-Mode und andere Optimierungen
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout = 1000;", nullptr, nullptr, nullptr);

    // Tabelle anlegen
    const char* create_table_sql = 
        "CREATE TABLE IF NOT EXISTS build_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  job_id INTEGER NOT NULL,"
        "  source_file TEXT NOT NULL,"
        "  content_hash TEXT NOT NULL,"
        "  worker_name TEXT,"
        "  cache_hit INTEGER NOT NULL DEFAULT 0,"
        "  queue_start_ms INTEGER NOT NULL,"
        "  dispatch_ms INTEGER,"
        "  compile_end_ms INTEGER,"
        "  result_ms INTEGER NOT NULL,"
        "  queue_duration_ms INTEGER,"
        "  compile_duration_ms INTEGER,"
        "  total_duration_ms INTEGER NOT NULL,"
        "  exit_code INTEGER NOT NULL DEFAULT 0,"
        "  created_at TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
        ");";

    char* err_msg = nullptr;
    rc = sqlite3_exec(db_, create_table_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        SUCO_LOG_ERROR("SQL error creating table: {}", err_msg);
        sqlite3_free(err_msg);
        return false;
    }

    // Indizes anlegen
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_be_created ON build_events(created_at);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_be_hash ON build_events(content_hash);", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_be_file ON build_events(source_file);", nullptr, nullptr, nullptr);

    // SQL Statement vorbereiten
    const char* insert_sql = 
        "INSERT INTO build_events (job_id, source_file, content_hash, worker_name, cache_hit, "
        "  queue_start_ms, dispatch_ms, compile_end_ms, result_ms, queue_duration_ms, "
        "  compile_duration_ms, total_duration_ms, exit_code) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(db_, insert_sql, -1, &insert_stmt_, nullptr);
    if (rc != SQLITE_OK) {
        SUCO_LOG_ERROR("Failed to prepare insert statement: {}", sqlite3_errmsg(db_));
        return false;
    }

    // Bei Bedarf Retention durchführen (ältere als 90 Tage löschen)
    const char* retention_sql =
        "DELETE FROM build_events "
        "WHERE created_at < strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-90 days');";
    sqlite3_exec(db_, retention_sql, nullptr, nullptr, nullptr);

    return true;
}

void HistoryWriter::writer_loop() {
    std::vector<Event> local_batch;
    local_batch.reserve(BATCH_SIZE);

    while (!shutdown_) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(FLUSH_MS), [this]() {
                return !queue_.empty() || shutdown_;
            });

            if (shutdown_ && queue_.empty()) {
                break;
            }

            // Hole Batch ab
            while (!queue_.empty() && local_batch.size() < BATCH_SIZE) {
                local_batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        if (!local_batch.empty()) {
            flush_batch(local_batch);
            local_batch.clear();
        }
    }

    // Rest flushen, falls beim Shutdown noch Events übrig sind
    std::vector<Event> remaining_batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!queue_.empty()) {
            remaining_batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }
    if (!remaining_batch.empty()) {
        flush_batch(remaining_batch);
    }
}

void HistoryWriter::flush_batch(std::vector<Event>& batch) {
    if (!db_ || !insert_stmt_) return;

    // Starte Transaktion
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& ev : batch) {
        sqlite3_bind_int64(insert_stmt_, 1, ev.job_id);
        sqlite3_bind_text(insert_stmt_, 2, ev.source_file.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt_, 3, ev.content_hash.c_str(), -1, SQLITE_TRANSIENT);
        
        if (ev.worker_name.empty()) {
            sqlite3_bind_null(insert_stmt_, 4);
        } else {
            sqlite3_bind_text(insert_stmt_, 4, ev.worker_name.c_str(), -1, SQLITE_TRANSIENT);
        }
        
        sqlite3_bind_int(insert_stmt_, 5, ev.cache_hit ? 1 : 0);
        sqlite3_bind_int64(insert_stmt_, 6, ev.queue_start_ms);
        
        if (ev.cache_hit) {
            sqlite3_bind_null(insert_stmt_, 7);
            sqlite3_bind_null(insert_stmt_, 8);
        } else {
            sqlite3_bind_int64(insert_stmt_, 7, ev.dispatch_ms);
            sqlite3_bind_int64(insert_stmt_, 8, ev.compile_end_ms);
        }
        
        sqlite3_bind_int64(insert_stmt_, 9, ev.result_ms);
        sqlite3_bind_int64(insert_stmt_, 10, ev.queue_duration_ms);
        sqlite3_bind_int64(insert_stmt_, 11, ev.compile_duration_ms);
        sqlite3_bind_int64(insert_stmt_, 12, ev.total_duration_ms);
        sqlite3_bind_int(insert_stmt_, 13, ev.exit_code);

        int rc = sqlite3_step(insert_stmt_);
        if (rc != SQLITE_DONE) {
            SUCO_LOG_ERROR("Failed to execute insert statement: {}", sqlite3_errmsg(db_));
        }

        sqlite3_reset(insert_stmt_);
    }

    // Transaktion committen
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

int64_t HistoryWriter::get_time_saved_today_ms() {
    if (!db_) return 0;

    const char* query = 
        "SELECT COALESCE(SUM("
        "  CASE WHEN cache_hit = 1 THEN "
        "    COALESCE(("
        "      SELECT be2.compile_duration_ms "
        "      FROM build_events be2 "
        "      WHERE be2.content_hash = be1.content_hash "
        "        AND be2.cache_hit = 0 "
        "        AND be2.compile_duration_ms > 0 "
        "      ORDER BY be2.id DESC "
        "      LIMIT 1"
        "    ), 0) "
        "  ELSE "
        "    CASE WHEN compile_duration_ms > total_duration_ms "
        "      THEN compile_duration_ms - total_duration_ms "
        "      ELSE 0 "
        "    END "
        "  END"
        "), 0) AS saved_ms "
        "FROM build_events be1 "
        "WHERE created_at >= strftime('%Y-%m-%dT00:00:00Z', 'now');";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        SUCO_LOG_ERROR("Failed to prepare time saved query: {}", sqlite3_errmsg(db_));
        return 0;
    }

    int64_t saved_ms = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        saved_ms = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return saved_ms;
}

std::vector<HistoryWriter::Event> HistoryWriter::get_recent_events(int limit) {
    std::vector<Event> out;
    if (!db_ || limit <= 0) return out;
    if (limit > 2000) limit = 2000;

    const char* query =
        "SELECT source_file, worker_name, cache_hit, queue_start_ms, dispatch_ms, "
        "       compile_end_ms, result_ms, compile_duration_ms, total_duration_ms, exit_code "
        "FROM build_events ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr) != SQLITE_OK) {
        SUCO_LOG_ERROR("Failed to prepare timeline query: {}", sqlite3_errmsg(db_));
        return out;
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Event e;
        const unsigned char* sf = sqlite3_column_text(stmt, 0);
        const unsigned char* wn = sqlite3_column_text(stmt, 1);
        e.source_file        = sf ? reinterpret_cast<const char*>(sf) : "";
        e.worker_name        = wn ? reinterpret_cast<const char*>(wn) : "";
        e.cache_hit          = sqlite3_column_int(stmt, 2) != 0;
        e.queue_start_ms     = sqlite3_column_int64(stmt, 3);
        e.dispatch_ms        = sqlite3_column_int64(stmt, 4);
        e.compile_end_ms     = sqlite3_column_int64(stmt, 5);
        e.result_ms          = sqlite3_column_int64(stmt, 6);
        e.compile_duration_ms= sqlite3_column_int64(stmt, 7);
        e.total_duration_ms  = sqlite3_column_int64(stmt, 8);
        e.exit_code          = sqlite3_column_int(stmt, 9);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace suco
