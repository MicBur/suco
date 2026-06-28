#include "scheduler.h"
#include <iostream>

namespace suco {

Scheduler::Scheduler(const CoordinatorConfig& config)
    : m_config(config) {}

std::shared_ptr<WorkerNode> Scheduler::select_best_worker(
    const std::vector<std::shared_ptr<WorkerNode>>& available_workers,
    const Job& job) const {

    std::shared_ptr<WorkerNode> best_worker = nullptr;
    double best_score = -1.0;
    double selected_weight = 1.0; // Für präzises Log-Reporting

    const auto& weights = m_config.get_worker_weights();

    for (const auto& w : available_workers) {
        int free_slots = w->slots_total - w->slots_used;
        if (free_slots > 0) {
            // Bestimme das spezifische Gewicht des Workers (Name oder IP)
            double weight = 1.0;
            auto it = weights.find(w->name);
            if (it != weights.end()) {
                weight = it->second;
            } else {
                auto it_ip = weights.find(w->ip);
                if (it_ip != weights.end()) {
                    weight = it_ip->second;
                }
            }

            // Berechne den Belegungs-Score
            double score = weight / (1.0 + w->slots_used);
            if (score > best_score) {
                best_score = score;
                best_worker = w;
                selected_weight = weight;
            }
        }
    }

    if (best_worker) {
        std::cout << "suco-coordinator: Scheduler selected worker " << best_worker->name 
                  << " (" << best_worker->ip << ") for job " << job.filename 
                  << ". [Weight: " << selected_weight 
                  << ", Slots: " << best_worker->slots_used << "/" << best_worker->slots_total 
                  << ", Score: " << best_score << "]" << std::endl;
    } else {
        std::cerr << "suco-coordinator: Scheduler found no available workers in grid for job " 
                  << job.filename << std::endl;
    }

    return best_worker;
}

} // namespace suco
