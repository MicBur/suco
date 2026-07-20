#include "job_queue.h"
#include <iostream>
#include <algorithm>

namespace suco {

void JobQueue::add_job(std::shared_ptr<Job> job) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.push_back(job);
}

std::shared_ptr<Job> JobQueue::get_next_pending_job() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& job : m_jobs) {
        if (job->status == JobStatus::PENDING) {
            return job;
        }
    }
    return nullptr;
}

void JobQueue::mark_as_running(const std::string& job_id, const std::string& worker_ip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& job : m_jobs) {
        if (job->id == job_id) {
            job->status = JobStatus::RUNNING;
            job->assigned_worker = worker_ip;
            job->started_at = std::chrono::steady_clock::now();
            break;
        }
    }
}

void JobQueue::mark_as_done(const std::string& job_id, int32_t exit_code, const std::string& log, const std::vector<uint8_t>& binary) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& job : m_jobs) {
        if (job->id == job_id) {
            job->status = JobStatus::DONE;
            job->exit_code = exit_code;
            job->log = log;
            job->binary = binary;
            job->finished_at = std::chrono::steady_clock::now();
            break;
        }
    }
}

void JobQueue::mark_as_failed(const std::string& job_id, const std::string& error_log) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& job : m_jobs) {
        if (job->id == job_id) {
            job->status = JobStatus::FAILED;
            job->exit_code = -1;
            job->log = error_log;
            job->finished_at = std::chrono::steady_clock::now();
            break;
        }
    }
}

std::vector<std::shared_ptr<Job>> JobQueue::reschedule_worker_jobs(const std::string& worker_ip) {
    std::vector<std::shared_ptr<Job>> rescheduled_jobs;
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& job : m_jobs) {
        if (job->status == JobStatus::RUNNING && job->assigned_worker == worker_ip) {
            job->status = JobStatus::PENDING;
            job->attempts++;
            job->assigned_worker = "";
            rescheduled_jobs.push_back(job);

            std::cout << "suco-coordinator: Rescheduling job " << job->filename 
                      << " [Attempt " << job->attempts << "] due to worker disconnect of " 
                      << worker_ip << std::endl;
        }
    }
    return rescheduled_jobs;
}

bool JobQueue::has_active_jobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& job : m_jobs) {
        if (job->status == JobStatus::PENDING || job->status == JobStatus::RUNNING) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<Job> JobQueue::get_job(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& job : m_jobs) {
        if (job->id == job_id) {
            return job;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<Job>> JobQueue::get_all_jobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs;
}

} // namespace suco
