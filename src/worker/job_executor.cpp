#include "job_executor.h"
#include "logging.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #define getpid _getpid
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <poll.h>
    #include <errno.h>
#endif

namespace suco::worker {

JobExecutor::Result JobExecutor::execute(const std::string& command, const std::string& filename, const std::string& source, int timeout_seconds) {
    Result result;

    bool is_msvc = check_is_msvc(command);
    
    // Dateinamen bereinigen (entferne abschließende NUL-Bytes, falls vorhanden)
    std::string fn_clean = filename;
    while (!fn_clean.empty() && fn_clean.back() == '\0') {
        fn_clean.pop_back();
    }
    
    bool is_c = (fn_clean.size() >= 2 && fn_clean.compare(fn_clean.size() - 2, 2, ".c") == 0);
    
    std::string ext = ".ii";
    if (is_msvc) {
        ext = ".cpp";
    } else {
        if (is_c) {
            ext = ".i";
        }
    }
    
    std::string temp_in = get_temp_file(ext);
    std::string temp_out = get_temp_file(is_msvc ? ".obj" : ".o");
    
    // 1. Präprozessierten Quellcode in die temporäre Eingabedatei schreiben
    std::ofstream out(temp_in, std::ios::binary);
    if (!out.is_open()) {
        result.exit_code = -3;
        result.log = "suco-worker error: Failed to create temporary input file: " + temp_in;
        return result;
    }
    out.write(source.data(), source.size());
    out.close();

    // 2. Befehl rekonstruieren und lokal ausführen
    std::string final_cmd = rebuild_compiler_command(command, temp_in, temp_out, is_msvc, is_c);
    int compile_exit = 0;
    result.log = run_local_capture(final_cmd, compile_exit, timeout_seconds);
    result.exit_code = compile_exit;

    // 3. Wenn die Kompilierung erfolgreich war, binäre Objektdatei auslesen
    if (compile_exit == 0) {
        std::ifstream in(temp_out, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            result.binary.resize(size);
            in.read(reinterpret_cast<char*>(result.binary.data()), size);
            in.close();
        } else {
            result.exit_code = -2;
            result.log += "\nsuco-worker error: Failed to read output object file: " + temp_out;
        }
    }

    // 4. Temporäre Dateien sauber aufräumen
    std::remove(temp_in.c_str());
    std::remove(temp_out.c_str());

    return result;
}

std::string JobExecutor::rebuild_compiler_command(const std::string& orig_cmd, const std::string& temp_in, const std::string& temp_out, bool is_msvc, bool is_c) {
    std::stringstream ss(orig_cmd);
    std::string word;
    std::string new_cmd;
    bool skip_next = false;
    
    while (ss >> word) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        
        if (is_msvc) {
            if (word.find("/Fo") == 0) {
                if (word == "/Fo") {
                    skip_next = true;
                }
                continue;
            }
            if (word == "/c" || word == "-c") {
                continue;
            }
            if (word == "-") {
                continue;
            }
        } else {
            if (word == "-o") {
                skip_next = true;
                continue;
            }
            if (word == "-c") {
                continue;
            }
            if (word == "-") {
                continue;
            }
        }
        
        new_cmd += word + " ";
    }
    
    if (is_msvc) {
        new_cmd += " /c \"" + temp_in + "\" /Fo\"" + temp_out + "\"";
    } else {
        if (is_c) {
            new_cmd += " -x cpp-output";
        } else {
            new_cmd += " -x c++-cpp-output";
        }
        new_cmd += " -c \"" + temp_in + "\" -o \"" + temp_out + "\"";
    }
    
    return new_cmd;
}

bool JobExecutor::check_is_msvc(const std::string& cmd) {
    std::stringstream ss(cmd);
    std::string first_word;
    ss >> first_word;
    
    size_t last_slash = first_word.find_last_of("\\/");
    std::string exe_name = (last_slash == std::string::npos) ? first_word : first_word.substr(last_slash + 1);
    
    std::transform(exe_name.begin(), exe_name.end(), exe_name.begin(), ::tolower);
    return exe_name == "cl" || exe_name == "cl.exe";
}

#ifdef _WIN32
std::string JobExecutor::run_local_capture(const std::string& cmd, int& exit_code, int timeout_seconds) {
    std::string output;
    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // Erstelle Pipe
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        exit_code = -1;
        return "suco-worker error: Failed to create pipes for stdout/stderr.";
    }

    // Sicherstellen, dass das Lesende nicht vererbt wird
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    si.hStdError = hWrite;
    si.hStdOutput = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES; // Nutze Std Handles

    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    // CreateProcess benötigt einen modifizierbaren Puffer
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        exit_code = -1;
        return "suco-worker error: Failed to start compiler process (CreateProcess failed).";
    }

    // Das Schreibende der Pipe muss im Parent sofort geschlossen werden, da sonst ReadFile nie blockiert/EOF erkennt
    CloseHandle(hWrite);

    // Schleife zum zeitüberwachten Lesen, um Deadlocks zu vermeiden
    DWORD start_time = GetTickCount();
    DWORD timeout_ms = static_cast<DWORD>(timeout_seconds) * 1000;
    bool is_timeout = false;
    
    char buffer[4096];
    DWORD bytes_read = 0;

    while (true) {
        // Prüfen, ob Daten zum Lesen bereitstehen
        DWORD bytes_avail = 0;
        if (PeekNamedPipe(hRead, NULL, 0, NULL, &bytes_avail, NULL) && bytes_avail > 0) {
            if (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
                buffer[bytes_read] = '\0';
                output += buffer;
            }
        }

        // Prüfen, ob der Prozess beendet ist
        DWORD wait_res = WaitForSingleObject(pi.hProcess, 50); // 50ms blockieren
        if (wait_res == WAIT_OBJECT_0) {
            break;
        }

        // Timeout prüfen
        if (GetTickCount() - start_time > timeout_ms) {
            is_timeout = true;
            break;
        }
    }

    if (is_timeout) {
        // Prozess hart terminieren
        TerminateProcess(pi.hProcess, -1);
        WaitForSingleObject(pi.hProcess, 1000); // Kurz warten bis er stirbt
        
        SUCO_LOG_ERROR("Job exceeded timeout of {}s and was terminated.", timeout_seconds);
        output += "\nsuco-worker error: Compiler process exceeded timeout of " + std::to_string(timeout_seconds) + "s and was terminated.";
        exit_code = -4;
    } else {
        DWORD exit_code_val = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code_val);
        exit_code = static_cast<int>(exit_code_val);
    }

    // Verbleibende Daten aus der Pipe lesen (falls noch vorhanden)
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        output += buffer;
    }

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return output;
}
#else
std::string JobExecutor::run_local_capture(const std::string& cmd, int& exit_code, int timeout_seconds) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        exit_code = -1;
        return "suco-worker error: Failed to create pipe.";
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        exit_code = -1;
        return "suco-worker error: Failed to fork process.";
    }

    if (pid == 0) {
        // Child-Prozess
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        const char* args[] = { "sh", "-c", cmd.c_str(), nullptr };
        execvp("sh", const_cast<char* const*>(args));
        // Falls execvp scheitert
        std::exit(127);
    }

    // Parent-Prozess
    close(pipefd[1]); // Schreibende schließen

    std::string output;
    char buffer[4096];
    
    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;

    auto start_time = std::chrono::steady_clock::now();
    int timeout_ms = timeout_seconds * 1000;
    bool is_timeout = false;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        int remaining = timeout_ms - elapsed;
        if (remaining <= 0) {
            is_timeout = true;
            break;
        }

        int poll_res = poll(&pfd, 1, std::min(remaining, 50)); // maximal 50ms blockieren
        if (poll_res < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (poll_res > 0) {
            if (pfd.revents & POLLIN) {
                ssize_t bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    output += buffer;
                } else if (bytes_read == 0) {
                    break; // EOF
                }
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                break;
            }
        }

        // Prüfen, ob der Prozess bereits beendet ist
        int status = 0;
        pid_t wait_res = waitpid(pid, &status, WNOHANG);
        if (wait_res > 0) {
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            } else {
                exit_code = -1;
            }
            break;
        }
    }

    if (is_timeout) {
        // Prozess hart abbrechen
        kill(pid, SIGKILL);
        
        // Zombie-Prozess vermeiden
        int status = 0;
        waitpid(pid, &status, 0);
        
        SUCO_LOG_ERROR("Job exceeded timeout of {}s and was terminated.", timeout_seconds);
        output += "\nsuco-worker error: Compiler process exceeded timeout of " + std::to_string(timeout_seconds) + "s and was terminated.";
        exit_code = -4;
    } else {
        // Sicherstellen, dass waitpid aufgerufen wurde, falls EOF erreicht aber waitpid in Schleife noch 0 lieferte
        int status = 0;
        pid_t wait_res = waitpid(pid, &status, WNOHANG);
        if (wait_res == 0) {
            waitpid(pid, &status, 0);
        }
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code = 128 + WTERMSIG(status);
        }
    }

    // Restliche Bytes aus der Pipe lesen (falls vorhanden)
    ssize_t bytes_read = 0;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        output += buffer;
    }

    close(pipefd[0]);
    return output;
}
#endif

std::string JobExecutor::get_temp_file(const std::string& suffix) {
    static std::mutex temp_mutex;
    std::lock_guard<std::mutex> lock(temp_mutex);
    
#ifdef _WIN32
    char temp_path[MAX_PATH];
    DWORD path_len = GetTempPathA(MAX_PATH, temp_path);
    std::string dir = (path_len > 0) ? std::string(temp_path) : ".\\";
    std::string path = dir + "suco_temp_" + std::to_string(rand()) + "_" + std::to_string(GetCurrentProcessId()) + suffix;
    return path;
#else
    std::string temp_str = "/tmp/suco_temp_XXXXXX" + suffix;
    std::vector<char> temp_chars(temp_str.begin(), temp_str.end());
    temp_chars.push_back('\0');
    int fd = mkstemps(temp_chars.data(), suffix.size());
    if (fd >= 0) {
        close(fd);
        return std::string(temp_chars.data());
    }
    return "/tmp/suco_temp_" + std::to_string(rand()) + "_" + std::to_string(getpid()) + suffix;
#endif
}

} // namespace suco::worker
