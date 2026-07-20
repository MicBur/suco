#include "worker_manager.h"
#include "logging.h"
#include <iostream>
#include <algorithm>
#include <cstdlib>

static const bool g_slot_dbg_wm = (std::getenv("SUCO_SLOT_DEBUG") != nullptr);

namespace suco {

WorkerManager::WorkerManager(const CoordinatorConfig& config) 
    : m_config(config) {}

void WorkerManager::register_worker(std::shared_ptr<WorkerNode> worker) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_workers.push_back(worker);
}

void WorkerManager::deregister_worker(socket_t socket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it) {
        if ((*it)->socket == socket) {
            SUCO_LOG_INFO("Worker offline: {} ({})", (*it)->name, (*it)->ip);
            close_socket((*it)->socket);
            m_workers.erase(it);
            break;
        }
    }
}

void WorkerManager::update_heartbeat(socket_t socket, int active_slots, int total_slots, const std::vector<double>& cpu_usage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& w : m_workers) {
        if (w->socket == socket) {
            if (g_slot_dbg_wm && w->slots_used != active_slots) {
                SUCO_LOG_INFO("[SLOT] HB-RESYNC worker={} slots_used {}->{} (delta={})",
                              w->name, w->slots_used, active_slots, active_slots - w->slots_used);
            }
            w->slots_used = active_slots;
            w->slots_total = total_slots;
            w->cpu_cores_usage = cpu_usage;
            // Precompute the mean core load here (under m_mutex) so the scheduler
            // can read a single atomic without racing on the cpu_cores_usage vector.
            double sum = 0.0;
            for (double u : cpu_usage) sum += u;
            w->avg_cpu_load.store(cpu_usage.empty() ? 0.0 : sum / static_cast<double>(cpu_usage.size()),
                                  std::memory_order_relaxed);
            w->last_heartbeat = std::chrono::steady_clock::now();
            break;
        }
    }
}

std::vector<std::string> WorkerManager::cleanup_inactive_workers() {
    std::vector<std::string> disconnected_ips;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - (*it)->last_heartbeat).count();
        if (elapsed_ms > static_cast<long long>(m_config.get_worker_timeout_ms())) {
            SUCO_LOG_WARNING("Worker {} ({}) disconnected (heartbeat timeout).", (*it)->name, (*it)->ip);
            
            disconnected_ips.push_back((*it)->ip);
            close_socket((*it)->socket);
            it = m_workers.erase(it);
        } else {
            ++it;
        }
    }
    return disconnected_ips;
}

std::vector<std::shared_ptr<WorkerNode>> WorkerManager::get_active_workers() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers;
}

std::shared_ptr<WorkerNode> WorkerManager::get_best_worker(const std::unordered_map<std::string, double>& weights) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::shared_ptr<WorkerNode> best_worker = nullptr;
    double best_score = -1.0;

    for (const auto& w : m_workers) {
        int free_slots = w->slots_total - w->slots_used;
        if (free_slots > 0) {
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

            double score = weight / (1.0 + w->slots_used);
            if (score > best_score) {
                best_score = score;
                best_worker = w;
            }
        }
    }
    return best_worker;
}

} // namespace suco
