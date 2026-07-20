#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>

namespace suco {

/**
 * @brief Hält alle Konfigurationsparameter für den SUCO Coordinator.
 * Lädt die Konfiguration aus Umgebungsvariablen oder greift auf robuste Standardwerte zurück.
 */
class CoordinatorConfig {
public:
    CoordinatorConfig() = default;

    // Getters
    uint16_t get_coordinator_port() const { return m_coordinator_port; }
    uint32_t get_heartbeat_interval_ms() const { return m_heartbeat_interval_ms; }
    uint32_t get_worker_timeout_ms() const { return m_worker_timeout_ms; }
    uint32_t get_job_timeout_ms() const { return m_job_timeout_ms; }
    int get_max_retries_per_job() const { return m_max_retries_per_job; }
    const std::unordered_map<std::string, double>& get_worker_weights() const { return m_worker_weights; }
    const std::string& get_cache_directory() const { return m_cache_directory; }
    size_t get_prefetch_jobs() const { return m_prefetch_jobs; }
    size_t get_coordinator_batch_threads() const { return m_coordinator_batch_threads; }

    // Setters
    void set_coordinator_port(uint16_t port) { m_coordinator_port = port; }
    void set_heartbeat_interval_ms(uint32_t interval) { m_heartbeat_interval_ms = interval; }
    void set_worker_timeout_ms(uint32_t timeout) { m_worker_timeout_ms = timeout; }
    void set_job_timeout_ms(uint32_t timeout) { m_job_timeout_ms = timeout; }
    void set_max_retries_per_job(int retries) { m_max_retries_per_job = retries; }
    void set_worker_weights(const std::unordered_map<std::string, double>& weights) { m_worker_weights = weights; }
    void set_cache_directory(const std::string& dir) { m_cache_directory = dir; }
    void set_prefetch_jobs(size_t count) { m_prefetch_jobs = count; }
    void set_coordinator_batch_threads(size_t count) { m_coordinator_batch_threads = count; }

    /**
     * @brief Lädt die Konfiguration aus Umgebungsvariablen mit entsprechenden Fallback-Defaults.
     */
    static CoordinatorConfig load();

    const std::string& get_history_db_path() const { return m_history_db_path; }
    void set_history_db_path(const std::string& path) { m_history_db_path = path; }

private:
    uint16_t m_coordinator_port = 9000;
    uint32_t m_heartbeat_interval_ms = 3000;
    uint32_t m_worker_timeout_ms = 8000;
    uint32_t m_job_timeout_ms = 120000;
    int m_max_retries_per_job = 3;
    std::unordered_map<std::string, double> m_worker_weights;
    std::string m_cache_directory;
    std::string m_history_db_path;
    size_t m_prefetch_jobs = 8;
    size_t m_coordinator_batch_threads = 12;
};

} // namespace suco
