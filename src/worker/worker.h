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

    /**
     * @brief Async-signal-SAFE shutdown trigger — the ONLY thing a signal handler
     * may call. Sets the shutdown flag and closes the listening/coordinator
     * sockets so blocking accept()/recv() return; the real cleanup (heartbeat
     * stop, thread joins) then happens in normal context as the loops unwind.
     *
     * Do NOT call initiate_shutdown() from a signal handler: it stops the
     * heartbeat manager, which joins a thread — a join (and logging, which takes
     * a mutex) from a signal handler is undefined behaviour and deadlocked the
     * process, so SIGTERM hung until systemd's 90s timeout SIGKILLed it.
     */
    void signal_unblock() noexcept;

    /**
     * @brief Startup check: if SUCO_SANDBOX=1, verify the sandbox can actually be
     * built (unprivileged user namespaces available). Returns false with a clear
     * remedy in the log instead of letting every task fail closed later.
     */
    static bool sandbox_selftest_ok();

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
    void handle_compile_job(const std::string& command, 
                            const std::string& filename, 
                            const std::string& source, 
                            const std::string& toolchain_hash,
                            const std::string& header_set_hash = "",
                            const std::string& header_set_source = "",
                            int client_sock = -1,
                            const std::vector<std::pair<std::string, std::string>>& module_cmis = {});

    void run_direct_listener_loop();

    Config m_config;
    NetworkClient m_net_client;
    std::atomic<int> m_slots_used{0};
    int m_slots_total;
    std::unique_ptr<HeartbeatManager> m_heartbeat_mgr;
    std::string m_toolchains_json;

    std::string m_active_coordinator_host;
    uint16_t m_active_coordinator_port = 0;

    std::atomic<bool> m_shutdown_requested{false};
    std::mutex m_jobs_mutex;
    std::condition_variable m_jobs_cv;
    int m_active_jobs_count = 0;

    int m_direct_listener_sock = -1;
    uint16_t m_direct_port = 0;
    std::thread m_direct_listener_thread;
};

} // namespace suco::worker
