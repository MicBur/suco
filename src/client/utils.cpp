#define _GNU_SOURCE
#include "utils.h"
#include "logging.h"

#include <iostream>
#include <cstdio>
#include <array>

#ifdef _WIN32
    #include <windows.h>
    #define popen _popen
    #define pclose _pclose
#else
    #include <sys/wait.h>
    #include <unistd.h>
    #include <spawn.h>
    #include <cerrno>
    #include <cstring>
    extern char **environ;
#endif

namespace suco {

void append_flag_tokens(std::vector<std::string>& out, const std::string& unit) {
    size_t start = 0;
    while (start < unit.size()) {
        size_t sp = unit.find(' ', start);
        if (sp == std::string::npos) {
            out.push_back(unit.substr(start));
            return;
        }
        if (sp > start) out.push_back(unit.substr(start, sp - start));
        start = sp + 1;
    }
}

std::pair<int, std::string> run_local_capture(const std::vector<std::string>& args, const std::string& cwd) {
    if (args.empty()) {
        return { -1, "" };
    }

#ifndef _WIN32
    // POSIX fast path: spawn the compiler directly via posix_spawnp — no shell.
    // popen() runs "sh -c <cmd>", which adds one extra process per invocation
    // (measured ~0.9ms vs ~0.4ms per spawn on native Linux; considerably more
    // under WSL2) and requires fragile manual quoting. This function sits on the
    // per-file hot path (preprocessing + local compilation), so the shell is
    // pure overhead. posix_spawnp is also safe in multithreaded processes.
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return { -1, "" };
    }

    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto& a : args) {
        c_args.push_back(const_cast<char*>(a.c_str()));
    }
    c_args.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    }
    // Redirect child stdout AND stderr into our pipe (same semantics as the
    // previous "2>&1" shell redirection), then close inherited pipe fds.
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, c_args[0], &fa, nullptr, c_args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);

    if (spawn_rc != 0) {
        SUCO_LOG_ERROR("posix_spawnp failed for compiler '{}' in directory '{}': {} (rc={})", c_args[0], cwd, strerror(spawn_rc), spawn_rc);
        close(pipefd[0]);
        return { -1, "" };
    }

    std::string output;
    char buffer[65536];
    for (;;) {
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return { -1, output };
    }
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    return { exit_code, output };
#else
    // Windows: keep the established _popen path (Windows focus comes later).
    // Reconstruct the shell command with proper argument quoting
    std::string cmd;
    if (!cwd.empty()) {
        cmd = "cd /d \"" + cwd + "\" && ";
    }
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            cmd += " ";
        }
        const std::string& arg = args[i];
        
        // Quote arguments containing spaces or special characters
        if (arg.find(' ') != std::string::npos || arg.find('\"') != std::string::npos) {
            std::string escaped_arg;
            escaped_arg.reserve(arg.size() + 2);
            escaped_arg += '\"';
            for (char c : arg) {
                if (c == '\"') escaped_arg += "\\\"";
                else escaped_arg += c;
            }
            escaped_arg += '\"';
            cmd += escaped_arg;
        } else {
            cmd += arg;
        }
    }

    // Redirect stderr to stdout so we capture compilation logs/warnings as well
    cmd += " 2>&1";

    std::string output;
    std::array<char, 4096> buffer{};

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return { -1, "" };
    }

    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    int exit_code = status;

    return { exit_code, output };
#endif
}

} // namespace suco
