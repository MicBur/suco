#include "metrics.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace suco::worker {

Metrics::Metrics() {
#ifdef _WIN32
    init_windows_cpu_times();
#else
    m_prev_cpu_times = read_proc_stat();
#endif
}

std::string Metrics::get_host_name() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf);
    }
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
#endif
    return "unknown-host";
}

std::string Metrics::get_os_name() {
#ifdef _WIN32
    return "Windows";
#else
    return "Linux";
#endif
}

int Metrics::get_logical_cores() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return static_cast<int>(sysinfo.dwNumberOfProcessors);
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<int>(cores);
    return 1;
#endif
}

std::vector<double> Metrics::get_cpu_usages() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int total_cores = get_logical_cores();
    std::vector<double> usages(total_cores, 0.0);

#ifdef _WIN32
    double global_usage = get_windows_cpu_usage();
    for (int i = 0; i < total_cores; ++i) {
        usages[i] = global_usage;
    }
#else
    auto curr_cpu_times = read_proc_stat();
    if (curr_cpu_times.size() >= usages.size() && m_prev_cpu_times.size() == curr_cpu_times.size()) {
        for (size_t i = 0; i < usages.size(); ++i) {
            uint64_t total_diff = curr_cpu_times[i].get_total() - m_prev_cpu_times[i].get_total();
            uint64_t idle_diff = curr_cpu_times[i].get_idle() - m_prev_cpu_times[i].get_idle();
            if (total_diff > 0 && total_diff >= idle_diff) {
                usages[i] = (static_cast<double>(total_diff - idle_diff) / total_diff) * 100.0;
            }
        }
        m_prev_cpu_times = curr_cpu_times;
    } else {
        m_prev_cpu_times = curr_cpu_times;
    }
#endif

    return usages;
}

#ifdef _WIN32
void Metrics::init_windows_cpu_times() {
    GetSystemTimes(&m_prev_idle, &m_prev_kernel, &m_prev_user);
}

double Metrics::get_windows_cpu_usage() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;

    auto to_uint64 = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idle_diff = to_uint64(idle) - to_uint64(m_prev_idle);
    uint64_t kernel_diff = to_uint64(kernel) - to_uint64(m_prev_kernel);
    uint64_t user_diff = to_uint64(user) - to_uint64(m_prev_user);

    m_prev_idle = idle;
    m_prev_kernel = kernel;
    m_prev_user = user;

    uint64_t total = kernel_diff + user_diff;
    if (total == 0) return 0.0;

    if (kernel_diff < idle_diff) return 0.0;
    uint64_t active = total - idle_diff;
    return (static_cast<double>(active) / total) * 100.0;
}
#else
std::vector<Metrics::CpuTime> Metrics::read_proc_stat() {
    std::vector<CpuTime> cpu_times;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return cpu_times;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("cpu") == 0) {
            if (line.size() > 3 && line[3] != ' ') {
                std::stringstream ss(line);
                std::string name;
                CpuTime t;
                if (ss >> name >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal) {
                    cpu_times.push_back(t);
                }
            }
        }
    }
    return cpu_times;
}
#endif

} // namespace suco::worker
