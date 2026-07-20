#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace suco::worker {

class Metrics {
public:
    Metrics();
    ~Metrics() = default;

    // Kopieren/Verschieben verhindern wegen Mutex und Systemressourcen
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    Metrics(Metrics&&) = delete;
    Metrics& operator=(Metrics&&) = delete;

    /**
     * @brief Gibt den Hostnamen der Maschine zurück.
     */
    static std::string get_host_name();

    /**
     * @brief Gibt den Namen des Betriebssystems zurück (z. B. "Windows" oder "Linux").
     */
    static std::string get_os_name();

    /**
     * @brief Ermittelt die Anzahl der logischen CPU-Kerne.
     */
    static int get_logical_cores();

    /**
     * @brief Liefert die prozentuale CPU-Auslastung pro logischem Kern.
     *        Unter Windows wird die globale CPU-Auslastung für alle Kerne zurückgegeben.
     *        Unter Linux wird die Auslastung pro logischem Kern ermittelt.
     * @return Ein Vektor mit den Auslastungswerten (0.0 bis 100.0) pro Kern.
     */
    std::vector<double> get_cpu_usages();

private:
    std::mutex m_mutex;

#ifdef _WIN32
    FILETIME m_prev_idle;
    FILETIME m_prev_kernel;
    FILETIME m_prev_user;
    
    void init_windows_cpu_times();
    double get_windows_cpu_usage();
#else
    struct CpuTime {
        uint64_t user = 0;
        uint64_t nice = 0;
        uint64_t system = 0;
        uint64_t idle = 0;
        uint64_t iowait = 0;
        uint64_t irq = 0;
        uint64_t softirq = 0;
        uint64_t steal = 0;

        uint64_t get_total() const {
            return user + nice + system + idle + iowait + irq + softirq + steal;
        }
        uint64_t get_idle() const {
            return idle + iowait;
        }
    };

    std::vector<CpuTime> m_prev_cpu_times;
    
    std::vector<CpuTime> read_proc_stat();
#endif
};

} // namespace suco::worker
