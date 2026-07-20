#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "job.h"

namespace suco {

/**
 * @brief Eine threadsichere Warteschlange und Verwaltung von Kompilier-Jobs.
 * Kapselt alle Statusänderungen und Zuweisungen der Jobs im Grid.
 */
class JobQueue {
public:
    JobQueue() = default;
    ~JobQueue() = default;

    /**
     * @brief Fügt einen neuen Job zur Warteschlange hinzu.
     */
    void add_job(std::shared_ptr<Job> job);

    /**
     * @brief Holt den nächsten ausstehenden (PENDING) Job.
     * @return shared_ptr auf den Job, oder nullptr falls kein Job aussteht.
     */
    std::shared_ptr<Job> get_next_pending_job();

    /**
     * @brief Markiert einen Job als laufend und weist ihm einen Worker zu.
     */
    void mark_as_running(const std::string& job_id, const std::string& worker_ip);

    /**
     * @brief Markiert einen Job als erfolgreich abgeschlossen.
     */
    void mark_as_done(const std::string& job_id, int32_t exit_code, const std::string& log, const std::vector<uint8_t>& binary);

    /**
     * @brief Markiert einen Job als fehlgeschlagen.
     */
    void mark_as_failed(const std::string& job_id, const std::string& error_log);

    /**
     * @brief Setzt alle laufenden Jobs eines ausgefallenen Workers zurück auf PENDING.
     * Erhöht die Versuche (attempts) der betroffenen Jobs.
     * @param worker_ip Die IP des ausgefallenen Workers.
     * @return Liste der zurückgesetzten Jobs.
     */
    std::vector<std::shared_ptr<Job>> reschedule_worker_jobs(const std::string& worker_ip);

    /**
     * @brief Gibt an, ob noch aktive Jobs (PENDING oder RUNNING) vorhanden sind.
     */
    bool has_active_jobs() const;

    /**
     * @brief Holt einen Job anhand seiner ID.
     * @return shared_ptr auf den Job, oder nullptr falls nicht gefunden.
     */
    std::shared_ptr<Job> get_job(const std::string& job_id);

    /**
     * @brief Liefert eine Kopie der Liste aller Jobs (z. B. für Dashboard/Statistik).
     */
    std::vector<std::shared_ptr<Job>> get_all_jobs() const;

private:
    mutable std::mutex m_mutex;                      ///< Mutex zur Absicherung der Job-Liste
    std::vector<std::shared_ptr<Job>> m_jobs;        ///< Liste aller verwalteten Jobs
};

} // namespace suco
