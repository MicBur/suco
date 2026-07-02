#pragma once

#include "config.h"
#include "network_client.h"
#include "heartbeat_manager.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace suco::worker {

class Worker {
public:
    /**
     * @brief Konstruiert den Worker mit der übergebenen Konfiguration.
     * @param config Die Konfiguration des Workers.
     */
    explicit Worker(Config config);
    ~Worker();

    // Verhindere Kopieren
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /**
     * @brief Startet den Haupt-Lebenszyklus des Workers.
     *        Führt Discovery aus, stellt Verbindung her, registriert den Worker,
     *        startet den Heartbeat-Thread und geht in den blockierenden Compile-Loop.
     * @return 0 bei erfolgreichem regulärem Ende, 1 bei Fehlern.
     */
    int run();

    /**
     * @brief Initiiert das geordnete Herunterfahren des Workers.
     *        Setzt die Shutdown-Flagge, schließt Sockets und stoppt Subsysteme.
     */
    void initiate_shutdown();

private:
    /**
     * @brief Führt den blockierenden Empfangs-Loop für Kompilieraufträge aus.
     */
    void run_worker_compile_loop();

    /**
     * @brief Verarbeitet eine einzelne Kompilierungsanfrage asynchron in einem Thread.
     * @param command Der Compiler-Befehl.
     * @param filename Der Dateiname der Quelldatei.
     * @param source Der präprozessierte Quellcode.
     */
    void handle_compile_job(const std::string& command, const std::string& filename, const std::string& source);

    Config m_config;
    NetworkClient m_net_client;
    std::atomic<int> m_slots_used{0};
    int m_slots_total;
    std::unique_ptr<HeartbeatManager> m_heartbeat_mgr;

    std::atomic<bool> m_shutdown_requested{false};
    std::mutex m_jobs_mutex;
    std::condition_variable m_jobs_cv;
    int m_active_jobs_count = 0;
};

} // namespace suco::worker
