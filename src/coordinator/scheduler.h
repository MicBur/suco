#pragma once

#include <memory>
#include <vector>
#include "worker_manager.h" // Für WorkerNode
#include "job.h"
#include "config.h"

namespace suco {

/**
 * @brief Zuständig für das Zuweisen von Kompilierungs-Jobs an die bestgeeigneten Grid-Worker.
 * Verwendet ein gewichtetes Scoring-Modell, um Lastverteilung und Hardwarestärke auszubalancieren.
 */
class Scheduler {
public:
    /**
     * @brief Konstruiert den Scheduler mit der Referenz auf die Konfiguration.
     */
    Scheduler(const CoordinatorConfig& config);
    ~Scheduler() = default;

    /**
     * @brief Wählt den am besten geeigneten Worker für den gegebenen Job aus.
     * @param available_workers Eine Liste der aktuell aktiven und erreichbaren Grid-Worker.
     * @param job Der zuzuweisende Kompilier-Job.
     * @return shared_ptr auf den ausgewählten WorkerNode, oder nullptr falls alle Worker belegt sind.
     */
    std::shared_ptr<WorkerNode> select_best_worker(
        const std::vector<std::shared_ptr<WorkerNode>>& available_workers,
        const Job& job) const;

private:
    const CoordinatorConfig& m_config;
};

} // namespace suco
