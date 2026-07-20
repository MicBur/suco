#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include "socket_util.h"
#include "config.h"

#include <map>

namespace suco {

struct ToolchainInfo {
    std::map<std::string, std::string> compilers;
    std::map<std::string, std::string> tools;
    std::map<std::string, std::string> qt_versions;
};

/**
 * @brief Repräsentiert einen registrierten Worker-Knoten im Grid.
 */
struct WorkerNode {
    socket_t socket;                                        ///< TCP-Verbindungssocket
    std::mutex write_mutex;                                 ///< Mutex zur Serialisierung von Schreibzugriffen auf den Socket
    mutable std::mutex known_header_sets_mutex;             ///< Mutex zur Absicherung des PCH-Cache des Workers (mutable für const-Kontext)
    std::string ip;                                         ///< IP-Adresse des Workers
    std::string name;                                       ///< Name des Workers (Hostname)
    std::string os;                                         ///< Betriebssystem des Workers (z. B. Linux, Windows)
    int slots_total;                                        ///< Gesamte Anzahl paralleler CPU-Slots
    int slots_used;                                         ///< Aktuell belegte CPU-Slots
    std::vector<double> cpu_cores_usage;                    ///< Auslastung der einzelnen CPU-Kerne (0.0 - 100.0)
    std::atomic<double> avg_cpu_load{0.0};                  ///< Mittlere CPU-Last (0.0-100.0), im Heartbeat gesetzt, race-frei vom Scheduler gelesen
    std::atomic<uint64_t> last_assigned_seq{0};             ///< Sequence of last assignment for round-robin tie-breaking
    std::chrono::steady_clock::time_point last_heartbeat;   ///< Zeitstempel des letzten empfangenen Heartbeats
    ToolchainInfo toolchains;                               ///< Erkannte Toolchains des Workers
    std::string toolchains_raw_json;                        ///< Rohes JSON der Toolchains
    uint16_t direct_port = 0;                               ///< Direct compile listener port of the worker
    std::unordered_set<std::string> known_header_sets;      ///< PCH hashes compiled on this worker
};

/**
 * @brief Verwaltet die Worker-Knoten im Grid threadsicher.
 * Ist zuständig für Registrierung, Deregistrierung, Heartbeats und Ausfallerkennung.
 */
class WorkerManager {
public:
    /**
     * @brief Konstruiert den WorkerManager mit der CoordinatorConfig.
     */
    WorkerManager(const CoordinatorConfig& config);
    ~WorkerManager() = default;

    /**
     * @brief Registriert einen neuen Worker-Knoten.
     */
    void register_worker(std::shared_ptr<WorkerNode> worker);

    /**
     * @brief Entfernt einen Worker-Knoten anhand seines Sockets und schließt ihn.
     */
    void deregister_worker(socket_t socket);

    /**
     * @brief Aktualisiert die Auslastungsdaten und den Heartbeat-Zeitstempel eines Workers.
     */
    void update_heartbeat(socket_t socket, int active_slots, int total_slots, const std::vector<double>& cpu_usage);

    /**
     * @brief Prüft alle Worker auf Timeouts. Entfernt inaktive Worker.
     * @return Liste der IPs der entfernten Worker (für Rescheduling).
     */
    std::vector<std::string> cleanup_inactive_workers();

    /**
     * @brief Liefert eine Kopie der Liste aller aktiven Worker.
     */
    std::vector<std::shared_ptr<WorkerNode>> get_active_workers();

    /**
     * @brief Ermittelt den besten Worker-Knoten für einen Job mittels gewichtetem Scheduling.
     * @return Index des bestgeeigneten Workers in der aktiven Liste, oder -1 falls kein Worker Slots frei hat.
     */
    std::shared_ptr<WorkerNode> get_best_worker(const std::unordered_map<std::string, double>& weights);

private:
    const CoordinatorConfig& m_config;                      ///< Referenz auf die Konfiguration (für Timeouts)
    std::mutex m_mutex;                                      ///< Mutex zur Absicherung der Worker-Liste
    std::vector<std::shared_ptr<WorkerNode>> m_workers;      ///< Liste der aktiven Worker-Knoten
};

} // namespace suco
