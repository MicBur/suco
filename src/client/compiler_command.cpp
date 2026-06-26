#include "compiler_command.h"
#include "hash_util.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <string_view>

#ifdef _WIN32
    #include <process.h>
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace {

// Helper to check if a string ends with a specific suffix
bool ends_with(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() && 
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Platform-independent helper to capture process output
std::string run_local_capture(const std::string& cmd, int& exit_code) {
    std::string result;
    char buffer[4096];
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = _pclose(pipe);
    exit_code = status;
#else
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);
#endif
    return result;
}

// Normalizes multi-line toolchain outputs (e.g. MSVC /Bv) into a single line
std::string normalize_version_output(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    bool last_was_space = false;
    for (char c : input) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!last_was_space && !result.empty()) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = (c == ' ');
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

} // namespace

CompilerCommand CompilerCommand::parse(int argc, char** argv) {
    CompilerCommand cmd;
    
    // Copy command line args, omitting suco binary itself
    for (int i = 1; i < argc; ++i) {
        cmd.raw_args.push_back(argv[i]);
    }

    if (cmd.raw_args.empty()) {
        return cmd;
    }

    // Check for dashboard monitor CLI request
    if (cmd.raw_args.size() == 1 && (cmd.raw_args[0] == "--monitor" || cmd.raw_args[0] == "--dashboard")) {
        cmd.is_monitor_request = true;
        return cmd;
    }

    cmd.compiler_path = cmd.raw_args[0];
    
    // Check if MSVC is used (ends with cl or cl.exe)
    cmd.is_msvc = ends_with(cmd.compiler_path, "cl.exe") || ends_with(cmd.compiler_path, "cl");

    for (size_t i = 1; i < cmd.raw_args.size(); ++i) {
        const std::string& arg = cmd.raw_args[i];

        if (cmd.is_msvc) {
            if (arg == "/c" || arg == "-c") {
                // Compile-only indicator (handled in is_compilation_step)
            } else if (arg.rfind("/Fo", 0) == 0) {
                cmd.output_file = arg.substr(3);
                if (cmd.output_file.empty() && i + 1 < cmd.raw_args.size()) {
                    cmd.output_file = cmd.raw_args[++i];
                }
            } else if (arg.rfind("-Fo", 0) == 0) {
                cmd.output_file = arg.substr(3);
                if (cmd.output_file.empty() && i + 1 < cmd.raw_args.size()) {
                    cmd.output_file = cmd.raw_args[++i];
                }
            } else if (arg.rfind("/D", 0) == 0 || arg.rfind("-D", 0) == 0) {
                if (arg.size() == 2) {
                    if (i + 1 < cmd.raw_args.size()) {
                        cmd.defines.push_back(arg + cmd.raw_args[i + 1]);
                        i++;
                    } else {
                        cmd.defines.push_back(arg);
                    }
                } else {
                    cmd.defines.push_back(arg);
                }
            } else if (arg.rfind("/I", 0) == 0 || arg.rfind("-I", 0) == 0) {
                if (arg.size() == 2) {
                    if (i + 1 < cmd.raw_args.size()) {
                        cmd.include_paths.push_back(arg + cmd.raw_args[i + 1]);
                        i++;
                    } else {
                        cmd.include_paths.push_back(arg);
                    }
                } else {
                    cmd.include_paths.push_back(arg);
                }
            } else if (arg.rfind("/std:", 0) == 0 || arg.rfind("-std:", 0) == 0) {
                cmd.language_standard = arg;
            } else if (arg.rfind("/Yu", 0) == 0 || arg.rfind("/Yc", 0) == 0 || arg.rfind("/Fp", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.other_flags.push_back(arg);
            } else if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                cmd.source_file = arg;
            } else {
                cmd.other_flags.push_back(arg);
            }
        } else {
            // GCC / Clang
            if (arg == "-c") {
                // Compile-only indicator (handled in is_compilation_step)
            } else if (arg == "-o" && i + 1 < cmd.raw_args.size()) {
                cmd.output_file = cmd.raw_args[++i];
            } else if (arg.rfind("-D", 0) == 0) {
                if (arg.size() == 2) {
                    if (i + 1 < cmd.raw_args.size()) {
                        cmd.defines.push_back(arg + cmd.raw_args[i + 1]);
                        i++;
                    } else {
                        cmd.defines.push_back(arg);
                    }
                } else {
                    cmd.defines.push_back(arg);
                }
            } else if (arg.rfind("-I", 0) == 0) {
                if (arg.size() == 2) {
                    if (i + 1 < cmd.raw_args.size()) {
                        cmd.include_paths.push_back(arg + cmd.raw_args[i + 1]);
                        i++;
                    } else {
                        cmd.include_paths.push_back(arg);
                    }
                } else {
                    cmd.include_paths.push_back(arg);
                }
            } else if (arg.rfind("-std=", 0) == 0) {
                cmd.language_standard = arg;
            } else if (arg == "-include-pch" || arg.rfind("-fpch-preprocess", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.other_flags.push_back(arg);
            } else if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                cmd.source_file = arg;
            } else {
                cmd.other_flags.push_back(arg);
            }
        }
    }

    // Sort options to guarantee order independence for cache hashing
    std::sort(cmd.defines.begin(), cmd.defines.end());
    std::sort(cmd.include_paths.begin(), cmd.include_paths.end());
    std::sort(cmd.other_flags.begin(), cmd.other_flags.end());

    return cmd;
}

bool CompilerCommand::is_compilation_step() const {
    bool has_c = false;
    for (const auto& flag : raw_args) {
        if (flag == "-c" || (is_msvc && flag == "/c")) {
            has_c = true;
            break;
        }
    }
    return has_c && !source_file.empty() && !output_file.empty() && !is_monitor_request;
}

std::string CompilerCommand::get_compiler_name() const {
    size_t pos = compiler_path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return compiler_path;
    }
    return compiler_path.substr(pos + 1);
}

std::string CompilerCommand::get_target_architecture() const {
    static std::string cached_arch;
    static std::once_flag flag;

    std::call_once(flag, [this]() {
        int exit_code = 0;
        if (is_msvc) {
            std::string out = run_local_capture("\"" + compiler_path + "\" /Bv 2>&1", exit_code);
            if (exit_code == 0 && !out.empty()) {
                if (out.find("ARM64") != std::string::npos || out.find("arm64") != std::string::npos) {
                    cached_arch = "arm64";
                } else if (out.find("x86") != std::string::npos || out.find("X86") != std::string::npos) {
                    cached_arch = "x86";
                } else {
                    cached_arch = "x64";
                }
            } else {
                cached_arch = "x64";
            }
        } else {
            std::string out = run_local_capture("\"" + compiler_path + "\" -dumpmachine 2>&1", exit_code);
            if (exit_code == 0 && !out.empty()) {
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
                    out.pop_back();
                }
                cached_arch = out;
            } else {
                cached_arch = "x86_64";
            }
        }
    });

    return cached_arch;
}

std::string CompilerCommand::get_compiler_version() const {
    static std::string cached_version;
    static std::once_flag flag;

    std::call_once(flag, [this]() {
        int exit_code = 0;
        if (is_msvc) {
            std::string out = run_local_capture("\"" + compiler_path + "\" /Bv 2>&1", exit_code);
            if (exit_code == 0 && !out.empty()) {
                cached_version = normalize_version_output(out);
            } else {
                cached_version = "MSVC Unknown";
            }
        } else {
            std::string out = run_local_capture("\"" + compiler_path + "\" -dumpfullversion 2>&1", exit_code);
            if (exit_code == 0 && !out.empty()) {
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
                    out.pop_back();
                }
                cached_version = out;
            } else {
                cached_version = "GCC Unknown";
            }
        }
    });

    return cached_version;
}

std::string CompilerCommand::get_hash_input() const {
    std::string input = "v1:\x1F";
    input += get_target_architecture() + "\x1F";
    input += get_compiler_version() + "\x1F";
    input += language_standard + "\x1F";

    // Delimit Defines and Paths with \x1F unit separators
    for (const auto& d : defines) {
        input += d + "\x1F";
    }
    input += "\x1F"; // Separator between defines and include paths

    for (const auto& p : include_paths) {
        input += p + "\x1F";
    }
    input += "\x1F"; // Separator between include paths and other flags

    for (size_t i = 0; i < other_flags.size(); ++i) {
        if (i > 0) input += "\x1F";
        input += other_flags[i];
    }
    input += "\x1F"; // Separator between flags and preprocessed source

    input += normalize_preprocessed_source(preprocessed_source);

    return input;
}

std::string CompilerCommand::normalize_preprocessed_source(const std::string& input) {
    return suco::normalize_preprocessed_source(input);
}
