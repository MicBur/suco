#include "job_executor.h"
#include "logging.h"
#include "toolchain_manager.h"
#include "header_cache.h"
#include "hash_util.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <filesystem>

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

namespace {

// The compile command is a shell string ("cd <job dir> && <compiler> ..."), so the
// leading cd has to speak the shell run_local_capture uses. On Windows that is cmd.exe,
// where a bare `cd` refuses to cross volumes: with TEMP on a different drive than the
// worker's own cwd it silently stays put and the compiler then runs in the wrong
// directory, missing the input it was just handed. /d makes it switch drive too.
#ifdef _WIN32
constexpr const char* kCdCommand = "cd /d ";
#else
constexpr const char* kCdCommand = "cd ";
#endif

// Picks the name the preprocessed input is written under inside the job directory.
// It lands in the object's STT_FILE symbol and debug info, so reproducing the client's
// own (relative) path is what makes grid objects match native ones.
//
// The name comes from the network and is used as a path, so it is untrusted: reject
// anything absolute, escaping via "..", or with a root/drive component, and fall back
// to the bare basename. A job must never be able to write outside its own directory.
std::string job_source_name(const std::string& client_name, const std::string& ext, bool is_msvc) {
    std::filesystem::path p(client_name);

    bool safe = !client_name.empty() && p.is_relative() && !p.has_root_name();
    if (safe) {
        for (const auto& part : p) {
            if (part == "..") { safe = false; break; }
        }
    }
    if (!safe) {
        p = p.filename();
        if (p.empty() || p == ".." || p == ".") {
            // Nothing usable in the client's name — fall back to a neutral one.
            return std::string("suco_job") + (is_msvc ? ".cpp" : ext);
        }
    }

    std::string name = p.generic_string();
    // The payload is preprocessed text, but MSVC needs a .cpp extension to accept it.
    if (is_msvc && !name.ends_with(".cpp")) name += ".cpp";
    return name;
}

} // namespace


JobExecutor::Result JobExecutor::execute(const std::string& command, 
                                         const std::string& filename, 
                                         const std::string& source, 
                                         int timeout_seconds, 
                                         const std::string& toolchain_hash,
                                         const std::string& header_set_hash,
                                         const std::string& header_set_source,
                                         const std::vector<std::pair<std::string, std::string>>& module_cmis) {
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
    
    std::string temp_out = get_temp_file(is_msvc ? ".obj" : ".o");

    // Every job gets its own directory, and the input is written under the CLIENT's
    // original file name inside it (see compile_in_job_dir below for why). It also
    // hosts the job's C++20 module CMIs (gcm.cache/), which MUST NOT be shared: two
    // concurrent jobs may import same-named modules with different contents.
    // get_temp_file() mkstemps() a real FILE to reserve the name race-free; drop it
    // and reuse the name for the directory.
    std::string job_dir = get_temp_file(".job");
    std::error_code jec;
    std::filesystem::remove(job_dir, jec);
    jec.clear();
    std::filesystem::create_directories(job_dir, jec);
    if (jec) {
        result.exit_code = -3;
        result.log = "suco-worker error: Failed to create job directory: " + jec.message();
        return result;
    }

    // The compiler is handed the preprocessed text under a name of OUR choosing, and
    // that name ends up in the object's STT_FILE symbol and its debug info. GCC re-runs
    // the preprocessor on it (-x c++ is required: -fdirectives-only output still holds
    // directives), and re-preprocessing makes the compiler's own input path win over the
    // `# 0 "real.cpp"` markers — so a temp name here surfaces as /tmp/suco_temp_X.ii in
    // gdb and in the symbol table, even though __FILE__ itself resolves correctly via the
    // markers. Reproducing the client's own relative path makes grid objects match native
    // ones. Fall back to the bare basename for absolute or escaping paths.
    std::string rel_name = job_source_name(fn_clean, ext, is_msvc);
    std::string temp_in = job_dir + "/" + rel_name;
    {
        std::filesystem::path in_path(temp_in);
        if (in_path.has_parent_path()) {
            std::filesystem::create_directories(in_path.parent_path(), jec);
            if (jec) {
                // Cannot mirror the client's directory layout — fall back to the basename.
                rel_name = std::filesystem::path(rel_name).filename().string();
                temp_in = job_dir + "/" + rel_name;
            }
        }
    }

    bool use_header_cache = false;
    std::string pch_path;

    const char* hc_enabled_env = std::getenv("SUCO_HEADER_CACHE_ENABLED");
    bool header_cache_enabled = hc_enabled_env ? (std::string(hc_enabled_env) == "1" || std::string(hc_enabled_env) == "true") : true;

    if (header_cache_enabled && !header_set_hash.empty() && !is_msvc) {
        auto& hc = HeaderCache::get_instance();

        if (!header_set_source.empty()) {
            // Always keep the TEXT — it is a plain file write and it alone makes this and
            // every later job for this header set compilable via -include.
            hc.store_source(header_set_hash, header_set_source, command);

            // The .gch is a different deal: measured on RocksDB it costs ~2.3s to build and
            // saves only ~0.6s per TU, so it needs ~4 reuses of the SAME set on the SAME node
            // to break even. Real cold builds reuse a set ~1.6x per node (449 hits / 279
            // builds), so building one on first sight BURNED more grid slot time than it ever
            // saved. Invest only once it amortises. SUCO_PCH_MIN_USES=1 = old behaviour.
            int min_uses = 4;
            if (const char* e = std::getenv("SUCO_PCH_MIN_USES")) {
                try { min_uses = std::max(1, std::stoi(e)); } catch (...) {}
            }
            int uses = hc.note_use(header_set_hash);
            if (uses >= min_uses && !hc.has_pch(header_set_hash)) {
                SUCO_LOG_INFO("[HeaderCache] set {} hit {} uses -> building PCH (amortises now)",
                              header_set_hash, uses);
                hc.store(header_set_hash, header_set_source, command, toolchain_hash);
            }
        }

        // Usable as soon as the header text is present; GCC uses header.gch if one exists.
        if (hc.get(header_set_hash, pch_path)) {
            use_header_cache = true;
            result.header_cache_hit = true;
            SUCO_LOG_INFO("[HeaderCache] {} for hash {}",
                          hc.has_pch(header_set_hash) ? "PCH HIT" : "text HIT (no PCH yet)",
                          header_set_hash);
        } else if (header_set_source.empty()) {
            // Stale coordinator knowledge: the client was told THIS worker knows header
            // set X (so it shipped only stripped source, no headers), but the set is not
            // here — evicted, wiped by a cache clear, or lost to a subnet flap. Compiling
            // the stripped source now yields "size_t has not been declared" and, worse,
            // that looks like a REAL compile error (exit 1) which the client would adopt
            // and fail the user's build over. Refuse with a distinct sentinel instead so
            // the client recompiles this TU locally (it has the full source) — an infra
            // gap must never surface as a source error. HEADER_SET_MISSING = -5.
            SUCO_LOG_WARNING("[HeaderCache] set {} claimed known but absent and no source sent "
                             "for {} — signalling client to compile locally", header_set_hash, fn_clean);
            result.exit_code = -5;
            std::error_code rm_ec;
            std::filesystem::remove_all(job_dir, rm_ec);
            return result;
        }
    }

    // 1. Präprozessierten Quellcode in die temporäre Eingabedatei schreiben
    std::string filtered_source;
    if (!is_msvc && command.find("-fdirectives-only") != std::string::npos) {
        filtered_source = suco::strip_predefined_macros(source);
    } else {
        filtered_source = source;
    }

    std::ofstream out(temp_in, std::ios::binary);
    if (!out.is_open()) {
        result.exit_code = -3;
        result.log = "suco-worker error: Failed to create temporary input file: " + temp_in;
        return result;
    }
    out.write(filtered_source.data(), filtered_source.size());
    out.close();

    if (std::getenv("SUCO_DEBUG_PAYLOAD")) {
        SUCO_LOG_INFO("[PAYLOAD-W] {} temp_in={} bytes={} first_line=[{}]", fn_clean, temp_in,
                      filtered_source.size(),
                      filtered_source.substr(0, filtered_source.find('\n')));
    }

    // 1b. E3: stage the job's C++20 module CMIs. GCC's default module mapper resolves
    // `import foo;` to gcm.cache/foo.gcm relative to the compiler's CWD, so each job
    // gets its own directory — concurrent jobs on this worker may import same-named
    // modules with different contents, and a shared gcm.cache would cross-contaminate
    // them. temp_in/temp_out are absolute, so the `cd` below cannot disturb them.
    if (!module_cmis.empty()) {
        std::error_code mec;
        std::filesystem::create_directories(std::filesystem::path(job_dir) / "gcm.cache", mec);
        if (mec) {
            result.exit_code = -3;
            result.log = "suco-worker error: Failed to create module directory: " + mec.message();
            std::filesystem::remove_all(job_dir, mec);
            return result;
        }
        for (const auto& [mod_name, cmi_bytes] : module_cmis) {
            // Module names may be dotted (net.http) but must not escape the job dir.
            if (mod_name.empty() || mod_name.find('/') != std::string::npos ||
                mod_name.find("..") != std::string::npos) {
                result.exit_code = -3;
                result.log = "suco-worker error: Rejected unsafe module name: " + mod_name;
                std::filesystem::remove_all(job_dir, mec);
                return result;
            }
            std::filesystem::path cmi_path =
                std::filesystem::path(job_dir) / "gcm.cache" / (mod_name + ".gcm");
            std::ofstream cmi_out(cmi_path, std::ios::binary);
            if (!cmi_out.is_open()) {
                result.exit_code = -3;
                result.log = "suco-worker error: Failed to write CMI: " + cmi_path.string();
                std::filesystem::remove_all(job_dir, mec);
                return result;
            }
            cmi_out.write(cmi_bytes.data(), cmi_bytes.size());
        }
        SUCO_LOG_INFO("[Modules] Staged {} CMI(s) in {}", module_cmis.size(), job_dir);
    }

    // 2. Befehl rekonstruieren und lokal ausführen
    std::string final_cmd = rebuild_compiler_command(command, rel_name, temp_out, is_msvc, is_c, toolchain_hash, use_header_cache);
    if (use_header_cache) {
        final_cmd += " -include \"" + pch_path + "\"";
    }
    // Run from the job dir so the input is named exactly as the client named it (and so
    // GCC finds this job's gcm.cache/). temp_out and pch_path are absolute — unaffected.
    if (!is_msvc) {
        // With -g, the job dir would otherwise land in DW_AT_comp_dir, pointing gdb at a
        // worker temp path that never existed on the developer's machine. Map it to "."
        // so debug info stays relocatable (gdb resolves against the dir you debug from)
        // and, more importantly, IDENTICAL no matter which worker built it — a
        // worker-specific path in the object would make cached objects non-deterministic.
        // -fdebug-prefix-map (not -ffile-prefix-map) so only debug info is touched:
        // __FILE__ must keep resolving via the client's line markers. GCC 4.3+/clang.
        final_cmd += " \"-fdebug-prefix-map=" + job_dir + "=.\"";
    }
    final_cmd = kCdCommand + ("\"" + job_dir + "\" && " + final_cmd);

    if (std::getenv("SUCO_DEBUG_PAYLOAD")) {
        SUCO_LOG_INFO("[PAYLOAD-CMD] {}", final_cmd);
    }

    int compile_exit = 0;
    result.log = run_local_capture(final_cmd, compile_exit, timeout_seconds);
    result.exit_code = compile_exit;

    // Fallback: If compiling with PCH fails, rebuild using full unstripped source.
    if (use_header_cache && compile_exit != 0) {
        SUCO_LOG_WARNING("HeaderCache: PCH compilation failed (Exit: {}). Falling back to normal compilation for {}", compile_exit, fn_clean);
        // The client may have omitted header_set_source ("PCH already known on
        // worker" optimisation). Without it, "" + stripped_source has no system
        // headers → size_t/std::vector undeclared. Reconstruct the full TU from
        // the worker's own cached header text (same bytes that built the PCH), so
        // this fallback is always able to reproduce a native-equivalent compile.
        std::string effective_header = header_set_source;
        if (effective_header.empty() && !header_set_hash.empty()) {
            if (HeaderCache::get_instance().get_source(header_set_hash, effective_header)) {
                SUCO_LOG_INFO("HeaderCache: reconstructed full source from cached header set for {}", fn_clean);
            }
        }
        std::string full_source = effective_header + source;
        std::string filtered_fallback;
        if (!is_msvc && command.find("-fdirectives-only") != std::string::npos) {
            filtered_fallback = suco::strip_predefined_macros(full_source);
        } else {
            filtered_fallback = full_source;
        }

        std::ofstream fallback_out(temp_in, std::ios::binary);
        if (fallback_out.is_open()) {
            fallback_out.write(filtered_fallback.data(), filtered_fallback.size());
            fallback_out.close();

            std::string fallback_cmd = kCdCommand + ("\"" + job_dir + "\" && " +
                rebuild_compiler_command(command, rel_name, temp_out, is_msvc, is_c, toolchain_hash, false));
            result.log = run_local_capture(fallback_cmd, compile_exit, timeout_seconds);
            result.exit_code = compile_exit;
            result.header_cache_hit = false;
        }
    }

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
    if (!std::getenv("SUCO_DEBUG_PAYLOAD")) {
        std::remove(temp_out.c_str());
        std::error_code cleanup_ec;
        std::filesystem::remove_all(job_dir, cleanup_ec);
    }

    return result;
}

std::string JobExecutor::rebuild_compiler_command(const std::string& orig_cmd, const std::string& temp_in, const std::string& temp_out, bool is_msvc, bool is_c, const std::string& toolchain_hash, bool use_header_cache) {
    std::stringstream ss(orig_cmd);
    std::string word;
    std::string new_cmd;
    bool skip_next = false;
    
    bool is_first = true;
    
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
        
        if (is_first) {
            is_first = false;
            if (!toolchain_hash.empty()) {
                std::string tc_path = ToolchainManager::get_toolchain_path(toolchain_hash);
                if (word.starts_with("/")) {
                    word = tc_path + word;
                } else {
                    word = tc_path + "/" + word;
                }
            }
        }

        new_cmd += word + " ";
    }
    
    if (is_msvc) {
        new_cmd += " /c \"" + temp_in + "\" /Fo\"" + temp_out + "\"";
    } else {
        if (use_header_cache) {
            if (is_c) {
                new_cmd += " -x c";
            } else {
                new_cmd += " -x c++";
            }
        } else {
            if (is_c) {
                new_cmd += " -x cpp-output";
            } else {
                new_cmd += " -x c++-cpp-output";
            }
        }
        if (!is_msvc && orig_cmd.find("-fdirectives-only") != std::string::npos) {
            new_cmd += " -Wno-builtin-macro-redefined";
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

    // Run through cmd.exe, exactly as the POSIX branch runs through `sh -c`. The command
    // is shell syntax ("cd <dir> && <compiler> ..."), and CreateProcess has no shell: it
    // took the first token literally and looked for an executable named "cd", which is a
    // cmd.exe builtin and does not exist as one. Every remote job on Windows therefore
    // died with CreateProcess failed -> exit -1 -> the client silently recompiled it
    // locally, so the grid looked alive while distributing nothing.
    std::string shell_cmd = "cmd.exe /c " + cmd;

    // CreateProcess benötigt einen modifizierbaren Puffer
    std::vector<char> cmd_buf(shell_cmd.begin(), shell_cmd.end());
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
