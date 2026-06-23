#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <hiredis/hiredis.h>

namespace suco {

struct CompileJob {
    std::string id;
    std::string client_ip;
    std::string filename;
    std::string command;
    std::chrono::steady_clock::time_point start_time;
};

struct HelperStats {
    std::mutex mutex;
    std::vector<CompileJob> active_jobs;
    uint64_t total_requests = 0;

    struct CompletedJob {
        std::string filename;
        std::string client_ip;
        int exit_code;
        int duration_ms;
        bool cache_hit;
    };
    std::vector<CompletedJob> recent_jobs;
    std::vector<double> cpu_cores_usage;
};

// C++17 inline global variable to avoid multiple definitions
inline HelperStats g_stats;

// CPU monitor parser helper
struct CpuStats {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
};

inline std::vector<CpuStats> read_cpu_stats() {
    std::vector<CpuStats> stats;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return stats;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("cpu", 0) == 0) {
            // Skip the aggregate 'cpu' line
            if (line.compare(0, 4, "cpu ") == 0) continue;
            std::stringstream ss(line);
            std::string cpu_name;
            CpuStats s;
            ss >> cpu_name >> s.user >> s.nice >> s.system >> s.idle >> s.iowait >> s.irq >> s.softirq >> s.steal;
            stats.push_back(s);
        }
    }
    return stats;
}

inline void start_cpu_monitor() {
    std::thread monitor([]() {
        std::vector<CpuStats> prev = read_cpu_stats();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::vector<CpuStats> curr = read_cpu_stats();
            if (curr.size() != prev.size()) {
                prev = curr;
                continue;
            }
            std::vector<double> usage;
            for (size_t i = 0; i < curr.size(); ++i) {
                unsigned long long prev_active = prev[i].user + prev[i].nice + prev[i].system + prev[i].irq + prev[i].softirq + prev[i].steal;
                unsigned long long curr_active = curr[i].user + curr[i].nice + curr[i].system + curr[i].irq + curr[i].softirq + curr[i].steal;

                unsigned long long prev_total = prev_active + prev[i].idle + prev[i].iowait;
                unsigned long long curr_total = curr_active + curr[i].idle + curr[i].iowait;

                unsigned long long diff_active = curr_active - prev_active;
                unsigned long long diff_total = curr_total - prev_total;

                double pct = 0.0;
                if (diff_total > 0) {
                    pct = (static_cast<double>(diff_active) / diff_total) * 100.0;
                }
                usage.push_back(pct);
            }
            {
                std::lock_guard<std::mutex> lock(g_stats.mutex);
                g_stats.cpu_cores_usage = usage;
            }
            prev = curr;
        }
    });
    monitor.detach();
}

// Fallback HTML Dashboard embedded directly
const std::string FALLBACK_DASHBOARD_HTML = R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <title>SUCO Grid Monitor</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&family=JetBrains+Mono&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #080b11;
            --card-bg: rgba(17, 24, 39, 0.7);
            --primary: #00f2fe;
            --secondary: #4facfe;
            --purple: #9b51e0;
            --green: #10b981;
            --red: #ef4444;
            --text: #e2e8f0;
            --text-muted: #64748b;
            --border: rgba(255, 255, 255, 0.08);
        }
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            font-family: 'Outfit', sans-serif;
            background-color: var(--bg-color);
            color: var(--text);
            overflow-x: hidden;
            background-image: radial-gradient(circle at 10% 20%, rgba(0, 242, 254, 0.05) 0%, transparent 40%),
                              radial-gradient(circle at 90% 80%, rgba(155, 81, 224, 0.05) 0%, transparent 40%);
            min-height: 100vh;
        }
        .container {
            max-width: 1400px;
            margin: 0 auto;
            padding: 2rem;
        }
        header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 2rem;
            border-bottom: 1px solid var(--border);
            padding-bottom: 1.5rem;
        }
        .logo-area {
            display: flex;
            align-items: center;
            gap: 1rem;
        }
        h1 {
            font-size: 2rem;
            font-weight: 800;
            background: linear-gradient(135deg, var(--primary) 0%, var(--secondary) 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: -0.5px;
        }
        .live-indicator {
            display: flex;
            align-items: center;
            gap: 0.5rem;
            font-size: 0.875rem;
            color: var(--green);
            background: rgba(16, 185, 129, 0.1);
            padding: 0.25rem 0.75rem;
            border-radius: 9999px;
            font-weight: 600;
            border: 1px solid rgba(16, 185, 129, 0.2);
        }
        .live-dot {
            width: 8px;
            height: 8px;
            background-color: var(--green);
            border-radius: 50%;
            animation: pulse 1.5s infinite;
        }
        @keyframes pulse {
            0% { transform: scale(0.9); opacity: 1; }
            50% { transform: scale(1.3); opacity: 0.4; }
            100% { transform: scale(0.9); opacity: 1; }
        }
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
            gap: 1.5rem;
            margin-bottom: 2rem;
        }
        .card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 1rem;
            padding: 1.5rem;
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            transition: all 0.3s ease;
            box-shadow: 0 4px 30px rgba(0, 0, 0, 0.2);
        }
        .card:hover {
            border-color: rgba(0, 242, 254, 0.2);
            transform: translateY(-2px);
            box-shadow: 0 8px 30px rgba(0, 242, 254, 0.05);
        }
        .card-title {
            font-size: 0.875rem;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 0.5rem;
            font-weight: 600;
        }
        .card-value {
            font-size: 2.25rem;
            font-weight: 800;
            margin-bottom: 0.5rem;
        }
        .card-trend {
            font-size: 0.875rem;
            display: flex;
            align-items: center;
            gap: 0.25rem;
        }
        .cpu-card {
            grid-column: 1 / -1;
        }
        .cpu-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(80px, 1fr));
            gap: 0.75rem;
            margin-top: 1rem;
        }
        .cpu-core {
            border: 1px solid var(--border);
            border-radius: 0.5rem;
            padding: 0.75rem;
            text-align: center;
            background: rgba(255, 255, 255, 0.02);
            transition: all 0.3s ease;
        }
        .cpu-label {
            font-size: 0.75rem;
            color: var(--text-muted);
            margin-bottom: 0.25rem;
            font-weight: 600;
        }
        .cpu-val {
            font-family: 'JetBrains Mono', monospace;
            font-size: 1rem;
            font-weight: 700;
        }
        .main-section {
            display: grid;
            grid-template-columns: 2fr 1fr;
            gap: 1.5rem;
        }
        @media(max-width: 1024px) {
            .main-section { grid-template-columns: 1fr; }
        }
        h2 {
            font-size: 1.25rem;
            margin-bottom: 1rem;
            font-weight: 600;
            display: flex;
            align-items: center;
            gap: 0.5rem;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            text-align: left;
        }
        th {
            color: var(--text-muted);
            font-weight: 600;
            font-size: 0.875rem;
            padding: 0.75rem 1rem;
            border-bottom: 1px solid var(--border);
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        td {
            padding: 1rem;
            border-bottom: 1px solid var(--border);
            font-size: 0.925rem;
        }
        tr:last-child td {
            border-bottom: none;
        }
        .code {
            font-family: 'JetBrains Mono', monospace;
            font-size: 0.875rem;
            background: rgba(255, 255, 255, 0.04);
            padding: 0.15rem 0.4rem;
            border-radius: 0.25rem;
            border: 1px solid rgba(255, 255, 255, 0.05);
        }
        .badge {
            display: inline-block;
            padding: 0.15rem 0.5rem;
            border-radius: 0.25rem;
            font-size: 0.75rem;
            font-weight: 600;
            text-transform: uppercase;
        }
        .badge-hit {
            background: rgba(16, 185, 129, 0.15);
            color: var(--green);
            border: 1px solid rgba(16, 185, 129, 0.3);
        }
        .badge-miss {
            background: rgba(239, 68, 68, 0.15);
            color: var(--red);
            border: 1px solid rgba(239, 68, 68, 0.3);
        }
        .empty-state {
            text-align: center;
            color: var(--text-muted);
            padding: 3rem;
            font-style: italic;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div class="logo-area">
                <h1>SUCO Grid Monitor</h1>
                <div class="live-indicator">
                    <div class="live-dot"></div>
                    <span>LIVE</span>
                </div>
            </div>
            <div style="font-size: 0.9rem; color: var(--text-muted);">
                Compile Helper Daemon (Port 9000 & 9001)
            </div>
        </header>

        <div class="stats-grid">
            <div class="card">
                <div class="card-title">Aktive Jobs</div>
                <div class="card-value" id="val-active">0</div>
                <div class="card-trend" style="color: var(--primary);">Kompilierung läuft</div>
            </div>
            <div class="card">
                <div class="card-title">Gesamte Anfragen</div>
                <div class="card-value" id="val-total">0</div>
                <div class="card-trend" style="color: var(--text-muted);">Seit Systemstart</div>
            </div>
            <div class="card">
                <div class="card-title">Cache Hit Rate</div>
                <div class="card-value" id="val-hitrate">0%</div>
                <div class="card-trend" id="val-hits-misses" style="color: var(--green);">0 Hits / 0 Misses</div>
            </div>
            
            <div class="card cpu-card">
                <div class="card-title">CPU-Kerne Auslastung</div>
                <div class="cpu-grid" id="cpu-container">
                    <!-- Dynamic CPU cores -->
                </div>
            </div>
        </div>

        <div class="main-section">
            <div class="card">
                <h2>Aktive Kompilierungen</h2>
                <div style="overflow-x: auto; margin-top: 1rem;">
                    <table id="active-table">
                        <thead>
                            <tr>
                                <th>Client IP</th>
                                <th>Datei</th>
                                <th>Compiler-Befehl</th>
                                <th>Dauer</th>
                            </tr>
                        </thead>
                        <tbody id="active-body">
                            <!-- Dynamic content -->
                        </tbody>
                    </table>
                    <div id="active-empty" class="empty-state">Aktuell keine aktiven Kompilierungen</div>
                </div>
            </div>

            <div class="card">
                <h2>Verlauf (Letzte Jobs)</h2>
                <div style="overflow-x: auto; margin-top: 1rem;">
                    <table id="recent-table">
                        <thead>
                            <tr>
                                <th>Datei</th>
                                <th>Ergebnis</th>
                                <th>Typ</th>
                            </tr>
                        </thead>
                        <tbody id="recent-body">
                            <!-- Dynamic content -->
                        </tbody>
                    </table>
                    <div id="recent-empty" class="empty-state">Noch keine Jobs verarbeitet</div>
                </div>
            </div>
        </div>
    </div>

    <script>
        function updateDashboard() {
            fetch('/api/stats')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('val-active').innerText = data.active_jobs_count;
                    document.getElementById('val-total').innerText = data.total_requests;

                    const totalHits = data.cache_hits || 0;
                    const totalMisses = data.cache_misses || 0;
                    const totalCacheStats = totalHits + totalMisses;
                    const hitRate = totalCacheStats > 0 ? Math.round((totalHits / totalCacheStats) * 100) : 0;
                    
                    document.getElementById('val-hitrate').innerText = hitRate + '%';
                    document.getElementById('val-hits-misses').innerText = `${totalHits} Hits / ${totalMisses} Misses`;

                    // Update CPU cores
                    const cpuContainer = document.getElementById('cpu-container');
                    cpuContainer.innerHTML = '';
                    if (data.cpu_cores_usage && data.cpu_cores_usage.length > 0) {
                        data.cpu_cores_usage.forEach((usage, idx) => {
                            const coreDiv = document.createElement('div');
                            coreDiv.className = 'cpu-core';
                            
                            // Visual color coding based on load
                            let shadowGlow = 'rgba(0, 242, 254, 0.05)';
                            let borderGlow = 'var(--border)';
                            let textColor = 'var(--text)';
                            if (usage > 80) {
                                shadowGlow = 'rgba(239, 68, 68, 0.2)';
                                borderGlow = 'rgba(239, 68, 68, 0.4)';
                                textColor = 'var(--red)';
                            } else if (usage > 30) {
                                shadowGlow = 'rgba(0, 242, 254, 0.15)';
                                borderGlow = 'rgba(0, 242, 254, 0.3)';
                                textColor = 'var(--primary)';
                            }
                            
                            coreDiv.style.boxShadow = `inset 0 0 10px ${shadowGlow}`;
                            coreDiv.style.borderColor = borderGlow;
                            coreDiv.style.color = textColor;

                            coreDiv.innerHTML = `
                                <div class="cpu-label">KERN ${idx}</div>
                                <div class="cpu-val">${Math.round(usage)}%</div>
                            `;
                            cpuContainer.appendChild(coreDiv);
                        });
                    } else {
                        cpuContainer.innerHTML = '<div style="color: var(--text-muted); font-style: italic;">Keine CPU-Auslastungsdaten verfügbar</div>';
                    }

                    // Update Active Table
                    const activeBody = document.getElementById('active-body');
                    activeBody.innerHTML = '';
                    if (data.active_jobs && data.active_jobs.length > 0) {
                        document.getElementById('active-empty').style.display = 'none';
                        data.active_jobs.forEach(job => {
                            const tr = document.createElement('tr');
                            tr.innerHTML = `
                                <td>${job.client_ip}</td>
                                <td><span class="code" style="color: var(--primary);">${job.filename}</span></td>
                                <td><span class="code">${job.command}</span></td>
                                <td style="font-family: 'JetBrains Mono', monospace; font-weight: 600;">${(job.duration_ms / 1000).toFixed(1)}s</td>
                            `;
                            activeBody.appendChild(tr);
                        });
                    } else {
                        document.getElementById('active-empty').style.display = 'block';
                    }

                    // Update Recent Table
                    const recentBody = document.getElementById('recent-body');
                    recentBody.innerHTML = '';
                    if (data.recent_jobs && data.recent_jobs.length > 0) {
                        document.getElementById('recent-empty').style.display = 'none';
                        data.recent_jobs.forEach(job => {
                            const tr = document.createElement('tr');
                            const statusColor = job.exit_code === 0 ? 'var(--green)' : 'var(--red)';
                            const typeBadge = job.cache_hit 
                                ? '<span class="badge badge-hit">Cache Hit</span>' 
                                : '<span class="badge badge-miss">Compile</span>';
                            
                            tr.innerHTML = `
                                <td><span class="code">${job.filename}</span></td>
                                <td style="color: ${statusColor}; font-weight: 600;">${job.exit_code === 0 ? 'Erfolg' : 'Fehler ('+job.exit_code+')'}</td>
                                <td>${typeBadge}</td>
                            `;
                            recentBody.appendChild(tr);
                        });
                    } else {
                        document.getElementById('recent-empty').style.display = 'block';
                    }
                })
                .catch(err => console.error('Dashboard-Update fehlgeschlagen:', err));
        }

        // Poll every 500ms
        setInterval(updateDashboard, 500);
        updateDashboard();
    </script>
</body>
</html>)HTML";

inline void run_web_server(int port) {
    // Start CPU monitor thread
    start_cpu_monitor();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "suco-web-server: Failed to create socket." << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-web-server: Bind failed on port " << port << std::endl;
        close(server_fd);
        return;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "suco-web-server: Listen failed." << std::endl;
        close(server_fd);
        return;
    }

    std::cout << "suco-web-server: Dashboard web interface listening on port " << port << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) continue;

        // Spawn a light thread or handle connection inline (since it's low traffic)
        std::thread([client_sock]() {
            char buffer[2048];
            std::memset(buffer, 0, sizeof(buffer));
            ssize_t read_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
            if (read_bytes <= 0) {
                close(client_sock);
                return;
            }

            std::string req(buffer);
            std::stringstream ss(req);
            std::string method, path;
            ss >> method >> path;

            if (method == "GET") {
                if (path == "/" || path == "/index.html" || path == "/dashboard.html") {
                    // Try to read dashboard.html from file
                    std::string html_content = FALLBACK_DASHBOARD_HTML;
                    std::ifstream file("../dashboard.html"); // Check in parent directory (workspace root)
                    if (!file.is_open()) {
                        file.open("dashboard.html"); // Check in current working dir
                    }
                    if (file.is_open()) {
                        std::stringstream file_ss;
                        file_ss << file.rdbuf();
                        html_content = file_ss.str();
                        file.close();
                    }

                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                           std::to_string(html_content.size()) + "\r\nConnection: close\r\n\r\n" + html_content;
                    send(client_sock, response.c_str(), response.size(), 0);
                } else if (path == "/api/stats") {
                    // Query Redis stats if available
                    long long cache_hits = 0;
                    long long cache_misses = 0;

                    // Setup connection to Redis
                    std::string redis_host = "127.0.0.1";
                    int redis_port = 6379;
                    if (const char* env_host = std::getenv("SUCO_REDIS_REPLICA_HOST")) redis_host = env_host;
                    if (const char* env_port = std::getenv("SUCO_REDIS_REPLICA_PORT")) redis_port = std::stoi(env_port);

                    struct timeval redis_timeout = { 0, 100000 }; // 100ms max query delay
                    redisContext* redis = redisConnectWithTimeout(redis_host.c_str(), redis_port, redis_timeout);
                    if (redis && !redis->err) {
                        redisReply* r_hits = (redisReply*)redisCommand(redis, "GET suco:stats:cache_hits");
                        if (r_hits && r_hits->type == REDIS_REPLY_STRING) {
                            cache_hits = std::atoll(r_hits->str);
                        }
                        redisReply* r_misses = (redisReply*)redisCommand(redis, "GET suco:stats:cache_misses");
                        if (r_misses && r_misses->type == REDIS_REPLY_STRING) {
                            cache_misses = std::atoll(r_misses->str);
                        }
                        if (r_hits) freeReplyObject(r_hits);
                        if (r_misses) freeReplyObject(r_misses);
                        redisFree(redis);
                    }

                    // Build JSON response
                    std::stringstream json;
                    json << "{\n";

                    {
                        std::lock_guard<std::mutex> lock(g_stats.mutex);
                        json << "  \"active_jobs_count\": " << g_stats.active_jobs.size() << ",\n";
                        json << "  \"total_requests\": " << g_stats.total_requests << ",\n";
                        json << "  \"cache_hits\": " << cache_hits << ",\n";
                        json << "  \"cache_misses\": " << cache_misses << ",\n";
                        json << "  \"cpu_cores_count\": " << g_stats.cpu_cores_usage.size() << ",\n";
                        
                        // CPU usage array
                        json << "  \"cpu_cores_usage\": [";
                        for (size_t i = 0; i < g_stats.cpu_cores_usage.size(); ++i) {
                            json << g_stats.cpu_cores_usage[i];
                            if (i + 1 < g_stats.cpu_cores_usage.size()) json << ", ";
                        }
                        json << "],\n";

                        // Active jobs array
                        json << "  \"active_jobs\": [\n";
                        auto now = std::chrono::steady_clock::now();
                        for (size_t i = 0; i < g_stats.active_jobs.size(); ++i) {
                            const auto& job = g_stats.active_jobs[i];
                            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.start_time).count();
                            json << "    {\n";
                            json << "      \"id\": \"" << job.id << "\",\n";
                            json << "      \"client_ip\": \"" << job.client_ip << "\",\n";
                            json << "      \"filename\": \"" << job.filename << "\",\n";
                            json << "      \"command\": \"" << job.command << "\",\n";
                            json << "      \"duration_ms\": " << elapsed_ms << "\n";
                            json << "    }";
                            if (i + 1 < g_stats.active_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ],\n";

                        // Recent jobs array
                        json << "  \"recent_jobs\": [\n";
                        for (size_t i = 0; i < g_stats.recent_jobs.size(); ++i) {
                            const auto& job = g_stats.recent_jobs[i];
                            json << "    {\n";
                            json << "      \"filename\": \"" << job.filename << "\",\n";
                            json << "      \"client_ip\": \"" << job.client_ip << "\",\n";
                            json << "      \"exit_code\": " << job.exit_code << ",\n";
                            json << "      \"duration_ms\": " << job.duration_ms << ",\n";
                            json << "      \"cache_hit\": " << (job.cache_hit ? "true" : "false") << "\n";
                            json << "    }";
                            if (i + 1 < g_stats.recent_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ]\n";
                    }

                    json << "}";

                    std::string json_str = json.str();
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                                           std::to_string(json_str.size()) + "\r\nConnection: close\r\n\r\n" + json_str;
                    send(client_sock, response.c_str(), response.size(), 0);
                } else {
                    std::string err_404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
                    send(client_sock, err_404.c_str(), err_404.size(), 0);
                }
            }
            close(client_sock);
        }).detach();
    }
    close(server_fd);
}

} // namespace suco
