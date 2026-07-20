#include "scheduler.h"
#include "logging.h"
#include "version_utils.h"
#include <iostream>
#include <string>
#include <cstdlib>

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
        // PCH Check: If header_set_source is empty but hash is not empty,
        // we MUST select a worker that already compiled this PCH.
        if (!job.header_set_hash.empty() && job.header_set_source.empty()) {
            std::lock_guard<std::mutex> lock(w->known_header_sets_mutex);
            if (w->known_header_sets.count(job.header_set_hash) == 0) {
                continue; // Skip this worker
            }
        }

        // Compiler-Kompatibilitäts-Check (Phase 2 / 2.5)
        if (!job.required_compiler.empty()) {
            auto it = w->toolchains.compilers.find(job.required_compiler);
            if (it == w->toolchains.compilers.end()) {
                SUCO_LOG_DEBUG("Scheduler: Skipping worker {} ({}) for job {} — missing compiler '{}' (available: {})",
                               w->name, w->ip, job.filename, job.required_compiler,
                               [&]() {
                                   std::string avail;
                                   for (const auto& [k, v] : w->toolchains.compilers) {
                                       if (!avail.empty()) avail += ", ";
                                       avail += k + "=" + v;
                                   }
                                   return avail.empty() ? "none" : avail;
                               }());
                continue;
            }

            // Versions-Kompatibilitäts-Check (Phase 3)
            if (!job.required_compiler_version.empty()) {
                const std::string& worker_version = it->second;
                if (!is_compiler_version_compatible(worker_version, job.required_compiler_version)) {
                    SUCO_LOG_DEBUG("Scheduler: Skipping worker {} ({}) for job {} — compiler '{}' version mismatch "
                                   "(worker: {}, required: {}, major: {} vs {})",
                                   w->name, w->ip, job.filename, job.required_compiler,
                                   worker_version, job.required_compiler_version,
                                   extract_major_version(worker_version),
                                   extract_major_version(job.required_compiler_version));
                    continue;
                }
            }
        }

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

            // Load-aware Score (Tier C): Belegung × Hardware-Gewicht × CPU-Headroom.
            // avg_cpu_load (0..100) kommt aus dem Heartbeat; ohne Daten (0.0) ist
            // headroom == 1.0 und der Score ist identisch zum früheren reinen
            // Belegungs-Score — vollständig rückwärtskompatibel. Der Floor (0.05)
            // verhindert, dass ein voll ausgelasteter Worker verhungert, wenn er der
            // einzige verfügbare ist. Escape-Hatch: SUCO_SCHED_LOAD_AWARE=0/off.
            static const bool load_aware = []() {
                const char* e = std::getenv("SUCO_SCHED_LOAD_AWARE");
                return !(e && (std::string(e) == "0" || std::string(e) == "off" ||
                               std::string(e) == "false" || std::string(e) == "OFF"));
            }();
            double headroom = 1.0;
            if (load_aware) {
                double load = w->avg_cpu_load.load(std::memory_order_relaxed);
                if (load < 0.0) load = 0.0;
                if (load > 100.0) load = 100.0;
                headroom = 1.0 - (load / 100.0);
                if (headroom < 0.05) headroom = 0.05;
            }
            double score = (weight * headroom) / (1.0 + w->slots_used);
            bool select_this = false;
            if (score > best_score) {
                select_this = true;
            } else if (score == best_score && best_worker) {
                if (w->last_assigned_seq.load(std::memory_order_relaxed) <
                    best_worker->last_assigned_seq.load(std::memory_order_relaxed)) {
                    select_this = true;
                }
            }

            if (select_this) {
                best_score = score;
                best_worker = w;
                selected_weight = weight;
            }
        }
    }

    if (best_worker) {
        static std::atomic<uint64_t> global_seq{0};
        best_worker->last_assigned_seq.store(++global_seq, std::memory_order_relaxed);

        std::cout << "suco-coordinator: Scheduler selected worker " << best_worker->name 
                  << " (" << best_worker->ip << ") for job " << job.filename 
                  << ". [Weight: " << selected_weight 
                  << ", Slots: " << best_worker->slots_used << "/" << best_worker->slots_total 
                  << ", Score: " << best_score << "]" << std::endl;
    } else {
        std::cerr << "suco-coordinator: Scheduler found no available/compatible workers in grid for job " 
                  << job.filename << " (Required: '" 
                  << (job.required_compiler.empty() ? "any" : job.required_compiler)
                  << (job.required_compiler_version.empty() ? "" : " v" + job.required_compiler_version)
                  << "')" << std::endl;
    }

    return best_worker;
}

} // namespace suco
