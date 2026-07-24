#include "compiler_command.h"
#include "hash_util.h"
#include "utils.h"

#include <algorithm>
#include <set>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string_view>
#include <cstdlib>

namespace {

// GCC/Clang options whose VALUE is a separate argv token ("-include foo.h").
// They must be kept as one unit: other_flags is sorted for cache-key stability
// (order must not change the key), and a naive sort tears "-include" away from
// "foo.h" — the compiler then reads whatever flag sorted next as the filename
// ("-include -march=native") and the real value becomes a stray input file.
// That silently breaks preprocessing, which downgrades the whole build to local
// compiles: correct results, zero distribution, and no error anyone sees.
bool takes_separate_value(const std::string& a) {
    static const std::set<std::string> kFlags = {
        "-include", "-imacros", "-include-pch", "-isystem", "-iquote", "-idirafter",
        "-iprefix", "-isysroot", "-imultilib", "-imultiarch",
        "-Xpreprocessor", "-Xassembler", "-Xlinker", "-Xclang",
        "-MT", "-MQ", "-MF", "--param", "-aux-info", "-b", "-V", "-x"
    };
    return kFlags.contains(a);
}

} // namespace

#include <cstring>
#include <fstream>
#include <sstream>

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

// Helper to check if a string starts with a specific prefix
bool starts_with(std::string_view str, std::string_view prefix) {
    return str.size() >= prefix.size() && 
           str.compare(0, prefix.size(), prefix) == 0;
}

// Helper to check if a path looks like a compiler binary
bool looks_like_compiler(const std::string& path) {
    if (path.empty()) return false;
    
    std::string base = path;
    size_t last_slash = base.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        base = base.substr(last_slash + 1);
    }
    
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);

    // Reject obvious non-compiler arguments early:
    // flags (-O2 etc.) and files with source/header/object/archive extensions.
    // Without this, an existing file like "x.cpp" as first argument would be
    // misclassified as a compiler by the filesystem check below.
    if (path[0] == '-') return false;
    static const char* non_compiler_exts[] = {
        ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".hh",
        ".o", ".obj", ".gch", ".pch", ".a", ".lib", ".so", ".dll",
        ".d", ".json", ".txt", ".rsp"
    };
    for (const char* ext : non_compiler_exts) {
        if (ends_with(base, ext)) return false;
    }
    bool has_executable_ext = ends_with(base, ".exe") || ends_with(base, ".bat") ||
                              ends_with(base, ".cmd") || ends_with(base, ".com");

    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".exe") == 0) {
        base = base.substr(0, base.size() - 4);
    }
    
    std::vector<std::string> compilers = {
        "gcc", "g++", "cc", "c++", "clang", "clang++", "cl"
    };
    for (const auto& c : compilers) {
        if (base == c) return true;
    }
    
    for (const auto& prefix : std::vector<std::string>{"gcc-", "g++-", "clang-", "clang++-"}) {
        if (starts_with(base, prefix)) {
            std::string suffix = base.substr(prefix.size());
            if (!suffix.empty()) {
                bool all_digits_or_dot = true;
                for (char ch : suffix) {
                    if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') {
                        all_digits_or_dot = false;
                        break;
                    }
                }
                if (all_digits_or_dot) return true;
            }
        }
    }
    
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
#ifndef _WIN32
        if (::access(path.c_str(), X_OK) == 0) {
            return true;
        }
#else
        // std::filesystem has no X_OK equivalent on Windows — require a known
        // executable extension so arbitrary existing files don't count as compilers.
        if (has_executable_ext && std::filesystem::is_regular_file(path, ec)) {
            return true;
        }
#endif
    }
    
    return false;
}

// Windows ships two unrelated toolchains and only one of them can use the grid.
// MSVC is the conventional default, but cl.exe exists only inside a Developer
// Command Prompt, and an MSVC job cannot be cross-dispatched to a Linux worker —
// no Linux box can run cl.exe. MinGW is the toolchain a Linux grid CAN serve, via
// the x86_64-w64-mingw32 cross compilers. Defaulting blindly to cl.exe therefore
// failed twice over: on a MinGW-only machine it exited 127 with no explanation,
// and on an MSVC machine it silently gave up all distribution. See #20.
//
// vcvars sets INCLUDE, and that is the signal we key on: it is exactly the
// condition under which cl.exe is usable, and reading an env var costs nothing.
// Probing PATH with `where` would spawn a shell on every single invocation —
// ~10-20ms per TU, which is the same order as the Nagle latency we went to some
// trouble to remove.
std::string default_windows_compiler(bool is_cpp) {
    const char* inc = std::getenv("INCLUDE");
    if (inc && *inc) return "cl.exe";
    return is_cpp ? "g++" : "gcc";
}

// Shared helper to resolve compiler path and adjust raw_args
std::string resolve_compiler_and_adjust_args(bool is_cpp, bool called_as_wrapper, std::vector<std::string>& raw_args) {
    if (called_as_wrapper) {
        bool has_compiler = false;
        if (!raw_args.empty() && looks_like_compiler(raw_args[0])) {
            has_compiler = true;
        }
        
        if (has_compiler) {
            // Launcher mode: compiler is passed in raw_args[0]
            std::string compiler_path = raw_args[0];
            
            // Env override takes precedence if set
            const char* env_var = is_cpp ? "SUCO_REAL_CXX" : "SUCO_REAL_CC";
            const char* env_val = std::getenv(env_var);
            if (env_val && std::strlen(env_val) > 0) {
                compiler_path = env_val;
                raw_args[0] = compiler_path;
            }
            return compiler_path;
        } else {
            // Directly called as compiler driver (suco-cl++ set as compiler)
            const char* env_var = is_cpp ? "SUCO_REAL_CXX" : "SUCO_REAL_CC";
            const char* env_val = std::getenv(env_var);
            std::string compiler_path;
            if (env_val && std::strlen(env_val) > 0) {
                compiler_path = env_val;
            } else {
                #ifdef _WIN32
                compiler_path = default_windows_compiler(is_cpp);
                #else
                compiler_path = is_cpp ? "g++" : "gcc";
                #endif
            }
            raw_args.insert(raw_args.begin(), compiler_path);
            return compiler_path;
        }
    } else {
        // Called directly via CLI suco (e.g. suco g++ ...)
        if (!raw_args.empty()) {
            return raw_args[0];
        }
        // Fallback fallback
        #ifdef _WIN32
        return default_windows_compiler(is_cpp);
        #else
        return is_cpp ? "g++" : "gcc";
        #endif
    }
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

std::string get_home_dir() {
    const char* home_env = std::getenv("HOME");
    if (home_env) return home_env;
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) return userprofile;
    return "/tmp";
}

struct CompilerMetadata {
    std::string version;
    std::string architecture;
};

static std::mutex g_metadata_mutex;

CompilerMetadata query_compiler_metadata(const std::string& compiler_path, bool is_msvc) {
    std::lock_guard<std::mutex> lock(g_metadata_mutex);
    std::string canonical_path = compiler_path;
    std::error_code ec;
    if (std::filesystem::exists(compiler_path, ec)) {
        auto abs_p = std::filesystem::absolute(compiler_path, ec);
        if (!ec) {
            auto can_p = std::filesystem::canonical(abs_p, ec);
            if (!ec) canonical_path = can_p.string();
        }
    }
    
    uint64_t mtime_epoch = 0;
    uint64_t size = 0;
    auto mtime = std::filesystem::last_write_time(canonical_path, ec);
    if (!ec) {
        mtime_epoch = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
        size = std::filesystem::file_size(canonical_path, ec);
    }
    
    std::string cache_dir = get_home_dir() + "/.cache/suco";
    std::filesystem::create_directories(cache_dir, ec);
    std::string cache_file = cache_dir + "/compiler_metadata_cache.txt";
    std::string cache_prefix = canonical_path + "|" + std::to_string(mtime_epoch) + "|" + std::to_string(size) + "|";
    
    std::ifstream in(cache_file);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.starts_with(cache_prefix)) {
                size_t pos = line.find('|', cache_prefix.size());
                if (pos != std::string::npos) {
                    std::string ver = line.substr(cache_prefix.size(), pos - cache_prefix.size());
                    std::string arch = line.substr(pos + 1);
                    if (!ver.empty() && !arch.empty()) {
                        return {ver, arch};
                    }
                }
            }
        }
    }
    
    int exit_code = 0;
    std::string version;
    std::string architecture;
    
    if (is_msvc) {
        std::string out = run_local_capture("\"" + compiler_path + "\" /Bv 2>&1", exit_code);
        if (exit_code == 0 && !out.empty()) {
            version = normalize_version_output(out);
            if (out.find("ARM64") != std::string::npos || out.find("arm64") != std::string::npos) {
                architecture = "arm64";
            } else if (out.find("x86") != std::string::npos || out.find("X86") != std::string::npos) {
                architecture = "x86";
            } else {
                architecture = "x64";
            }
        } else {
            version = "MSVC Unknown";
            architecture = "x64";
        }
    } else {
        std::string out_ver = run_local_capture("\"" + compiler_path + "\" -dumpfullversion 2>&1", exit_code);
        if (exit_code == 0 && !out_ver.empty()) {
            while (!out_ver.empty() && (out_ver.back() == '\n' || out_ver.back() == '\r')) out_ver.pop_back();
            version = out_ver;
        } else {
            std::string out_ver2 = run_local_capture("\"" + compiler_path + "\" -dumpversion 2>&1", exit_code);
            if (exit_code == 0 && !out_ver2.empty()) {
                while (!out_ver2.empty() && (out_ver2.back() == '\n' || out_ver2.back() == '\r')) out_ver2.pop_back();
                version = out_ver2;
            } else {
                version = "GCC Unknown";
            }
        }
        
        std::string out_arch = run_local_capture("\"" + compiler_path + "\" -dumpmachine 2>&1", exit_code);
        if (exit_code == 0 && !out_arch.empty()) {
            while (!out_arch.empty() && (out_arch.back() == '\n' || out_arch.back() == '\r')) out_arch.pop_back();
            architecture = out_arch;
        } else {
            architecture = "x86_64";
        }
    }
    
    std::ofstream out_cache(cache_file, std::ios::app);
    if (out_cache) {
        out_cache << cache_prefix << version << "|" << architecture << "\n";
    }
    
    return {version, architecture};
}

} // namespace

CompilerCommand CompilerCommand::parse(int argc, char** argv) {
    CompilerCommand cmd;
    
    // Copy command line args, omitting suco binary itself
    for (int i = 1; i < argc; ++i) {
        cmd.raw_args.push_back(argv[i]);
    }

    if (cmd.raw_args.empty()) {
        // Falls als Wrapper aufgerufen und ohne Argumente, leeres Kommando zurückliefern
        // (Wird in LocalCompiler / main.cpp abgefangen)
    }

    // Check for dashboard monitor CLI request
    if (cmd.raw_args.size() == 1 && (cmd.raw_args[0] == "--monitor" || cmd.raw_args[0] == "--dashboard")) {
        cmd.is_monitor_request = true;
        return cmd;
    }

    std::string binary_name = argv[0];
    size_t last_slash = binary_name.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        binary_name = binary_name.substr(last_slash + 1);
    }
    std::transform(binary_name.begin(), binary_name.end(), binary_name.begin(), ::tolower);
    #ifdef _WIN32
    if (ends_with(binary_name, ".exe")) {
        binary_name = binary_name.substr(0, binary_name.size() - 4);
    }
    #endif

    bool called_as_wrapper = (binary_name == "suco-cl" || binary_name == "suco-cl++");
    bool is_cpp = (binary_name == "suco-cl++");

    cmd.compiler_path = resolve_compiler_and_adjust_args(is_cpp, called_as_wrapper, cmd.raw_args);
    
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
            } else if (arg.rfind("/Yc", 0) == 0 || arg.rfind("-Yc", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.is_pch_creation = true;
                cmd.other_flags.push_back(arg);
            } else if (arg.rfind("/Yu", 0) == 0 || arg.rfind("-Yu", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.is_pch_usage = true;
                cmd.other_flags.push_back(arg);
            } else if (arg.rfind("/Y-", 0) == 0 || arg.rfind("-Y-", 0) == 0) {
                cmd.other_flags.push_back(arg);
            } else if (arg.rfind("/Fp", 0) == 0 || arg.rfind("-Fp", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.other_flags.push_back(arg);
                if (arg.size() > 3) {
                    cmd.pch_file = arg.substr(3);
                } else if (i + 1 < cmd.raw_args.size()) {
                    cmd.pch_file = cmd.raw_args[i + 1];
                    cmd.other_flags.push_back(cmd.raw_args[++i]);
                }
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
            } else if (arg == "-isystem" || arg == "-iquote" || arg == "-idirafter" || arg == "-iprefix") {
                if (i + 1 < cmd.raw_args.size()) {
                    cmd.include_paths.push_back(arg + cmd.raw_args[i + 1]);
                    i++;
                } else {
                    cmd.include_paths.push_back(arg);
                }
            } else if (arg.starts_with("-isystem") || arg.starts_with("-iquote") || arg.starts_with("-idirafter") || arg.starts_with("-iprefix")) {
                if (takes_separate_value(arg) && i + 1 < cmd.raw_args.size()) {
                    // "-isystem /path" (two tokens) — glue, or sorting separates them.
                    cmd.include_paths.push_back(arg + " " + cmd.raw_args[++i]);
                } else {
                    cmd.include_paths.push_back(arg);   // "-isystem/path" (one token)
                }
            } else if (arg.rfind("-std=", 0) == 0) {
                cmd.language_standard = arg;
            } else if (arg == "-include") {
                if (i + 1 < cmd.raw_args.size()) {
                    cmd.other_flags.push_back(arg + " " + cmd.raw_args[++i]);
                } else {
                    cmd.other_flags.push_back(arg);
                }
            } else if (arg == "-include-pch") {
                cmd.is_precompiled_header = true;
                cmd.is_pch_usage = true;
                if (i + 1 < cmd.raw_args.size()) {
                    cmd.pch_file = cmd.raw_args[i + 1];
                    cmd.other_flags.push_back(arg + " " + cmd.raw_args[++i]);
                } else {
                    cmd.other_flags.push_back(arg);
                }
            } else if (arg.rfind("-fpch-preprocess", 0) == 0) {
                cmd.is_precompiled_header = true;
                cmd.is_pch_usage = true;
                cmd.other_flags.push_back(arg);
            } else if (ends_with(arg, ".gch") || ends_with(arg, ".pch")) {
                cmd.is_precompiled_header = true;
                cmd.is_pch_usage = true;
                cmd.pch_file = arg;
                cmd.other_flags.push_back(arg);
            } else if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                cmd.source_file = arg;
            } else if (ends_with(arg, ".h") || ends_with(arg, ".hpp") || ends_with(arg, ".hxx") || ends_with(arg, ".hh")) {
                cmd.source_file = arg;
                cmd.is_precompiled_header = true;
                cmd.is_pch_creation = true;
            // Dependency-generation flags: strip them entirely.
            // These are only used by make/ninja for incremental rebuilds and are
            // irrelevant for remote compilation, cache hashing, and preprocessing.
            // They remain in raw_args for the local fallback path.
            } else if (arg == "-MD" || arg == "-MMD" || arg == "-MP") {
                // Standalone flags — just skip
            } else if (arg == "-MF" || arg == "-MT" || arg == "-MQ") {
                // These take a following argument — skip both
                if (i + 1 < cmd.raw_args.size()) ++i;
            } else if (arg.starts_with("-MF") || arg.starts_with("-MT") || arg.starts_with("-MQ")) {
                // Combined form (e.g. -MFfile.d, -MTtarget.o) — skip
            } else if (takes_separate_value(arg) && i + 1 < cmd.raw_args.size()) {
                // Same reason as -include above: sorting must not separate a flag from
                // the value that follows it in argv.
                cmd.other_flags.push_back(arg + " " + cmd.raw_args[++i]);
            } else {
                cmd.other_flags.push_back(arg);
            }
        }
    }

    // Sort options to guarantee order independence for cache hashing
    std::sort(cmd.defines.begin(), cmd.defines.end());
    std::sort(cmd.include_paths.begin(), cmd.include_paths.end());
    std::sort(cmd.other_flags.begin(), cmd.other_flags.end());

    // Bestimme required_compiler mittels zentraler Normalisierungsfunktion
    cmd.required_compiler = suco::normalize_compiler_name(cmd.compiler_path);

    // Ermittle die tatsächliche Compiler-Version für Versions-Matching (Phase 3)
    if (!cmd.compiler_path.empty() && cmd.is_compilation_step()) {
        cmd.required_compiler_version = cmd.get_compiler_version();
    }

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
    auto meta = query_compiler_metadata(compiler_path, is_msvc);
    return meta.architecture;
}

std::string CompilerCommand::get_remote_compiler_name() const {
    if (is_msvc) return compiler_path;

    // -dumpmachine, e.g. "x86_64-w64-mingw32" or "x86_64-linux-gnu". Only MinGW targets
    // are qualified — see the header for why Linux ones deliberately are not.
    const std::string triple = get_target_architecture();
    if (triple.find("mingw") == std::string::npos) return compiler_path;

    std::string name = get_compiler_name();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
        name.resize(name.size() - 4);
    }
    if (name.starts_with(triple + "-")) return name;  // already target-qualified

    // Only the ambiguous driver names. Anything else is either already specific or
    // something we should not be second-guessing.
    if (name == "g++" || name == "c++") return triple + "-g++";
    if (name == "gcc" || name == "cc")  return triple + "-gcc";
    return compiler_path;
}

std::string CompilerCommand::get_dispatch_compiler_id() const {
    if (is_msvc) return required_compiler;
    const std::string triple = get_target_architecture();
    if (triple.find("mingw") == std::string::npos) return required_compiler;
    // Same mapping as get_remote_compiler_name: only the ambiguous GCC driver
    // names get qualified; anything else is passed through untouched.
    if (required_compiler == "g++" || required_compiler == "c++") return triple + "-g++";
    if (required_compiler == "gcc" || required_compiler == "cc")  return triple + "-gcc";
    return required_compiler;
}

std::string CompilerCommand::get_compiler_version() const {
    auto meta = query_compiler_metadata(compiler_path, is_msvc);
    return meta.version;
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

namespace {
std::string get_output_filename(const std::string& source, const std::string& out_flag, bool is_msvc) {
    size_t last_slash = source.find_last_of("\\/");
    std::string base = (last_slash == std::string::npos) ? source : source.substr(last_slash + 1);
    
    size_t dot = base.find_last_of(".");
    std::string ext = is_msvc ? ".obj" : ".o";
    std::string out_name = (dot == std::string::npos) ? base + ext : base.substr(0, dot) + ext;
    
    if (out_flag.empty()) {
        return out_name;
    }
    
    if (out_flag.back() == '/' || out_flag.back() == '\\') {
        return out_flag + out_name;
    }
    
    std::error_code ec;
    if (std::filesystem::is_directory(out_flag, ec)) {
        std::filesystem::path p(out_flag);
        return (p / out_name).string();
    }
    
    return out_flag;
}
} // namespace

std::vector<CompilerCommand> CompilerCommand::parse_all(int argc, char** argv) {
    std::vector<std::string> source_files;
    std::string output_flag;
    bool is_msvc = false;
    std::string compiler_path;
    
    std::string binary_name = argv[0];
    size_t last_slash = binary_name.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        binary_name = binary_name.substr(last_slash + 1);
    }
    std::transform(binary_name.begin(), binary_name.end(), binary_name.begin(), ::tolower);
    #ifdef _WIN32
    if (ends_with(binary_name, ".exe")) {
        binary_name = binary_name.substr(0, binary_name.size() - 4);
    }
    #endif
    bool called_as_wrapper = (binary_name == "suco-cl" || binary_name == "suco-cl++");
    
    std::vector<std::string> temp_raw_args;
    for (int i = 1; i < argc; ++i) {
        temp_raw_args.push_back(argv[i]);
    }
    
    bool is_cpp = (binary_name == "suco-cl++");
    compiler_path = resolve_compiler_and_adjust_args(is_cpp, called_as_wrapper, temp_raw_args);
    is_msvc = ends_with(compiler_path, "cl.exe") || ends_with(compiler_path, "cl");

    for (size_t i = 1; i < temp_raw_args.size(); ++i) {
        std::string arg = temp_raw_args[i];
        if (is_msvc) {
            if (arg.rfind("/Fo", 0) == 0 || arg.rfind("-Fo", 0) == 0) {
                output_flag = arg.substr(3);
                if (output_flag.empty() && i + 1 < temp_raw_args.size()) {
                    output_flag = temp_raw_args[++i];
                }
            } else if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                source_files.push_back(arg);
            }
        } else {
            if (takes_separate_value(arg) && i + 1 < temp_raw_args.size()) {
                // Skip the VALUE of "-include foo.h" and friends. Source detection below
                // is extension-based, so without this the forced header is mistaken for a
                // second source file and gets compiled as its own job — while the real TU
                // ends up with "-include" bound to whatever token follows.
                ++i;
            } else if (arg == "-o" && i + 1 < temp_raw_args.size()) {
                output_flag = temp_raw_args[++i];
            } else if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                source_files.push_back(arg);
            } else if (ends_with(arg, ".h") || ends_with(arg, ".hpp") || ends_with(arg, ".hxx") || ends_with(arg, ".hh")) {
                source_files.push_back(arg);
            }
        }
    }

    if (source_files.size() <= 1) {
        return { parse(argc, argv) };
    }

    std::vector<CompilerCommand> commands;
    for (const auto& src : source_files) {
        std::vector<std::string> new_args;
        new_args.push_back(argv[0]);
        // Preserve the resolved compiler (temp_raw_args[0]) in the reconstructed
        // command line. Without it, the nested parse() call would fall back to the
        // default compiler (g++/cl.exe) and discard e.g. a launcher-passed clang++.
        if (!temp_raw_args.empty()) {
            new_args.push_back(temp_raw_args[0]);
        }

        std::string resolved_out = get_output_filename(src, output_flag, is_msvc);

        for (size_t i = 1; i < temp_raw_args.size(); ++i) {
            std::string arg = temp_raw_args[i];
            if (is_msvc) {
                if (arg.rfind("/Fo", 0) == 0 || arg.rfind("-Fo", 0) == 0) {
                    if (arg.size() == 3 && i + 1 < temp_raw_args.size()) {
                        i++;
                    }
                    continue;
                }
                if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c")) {
                    continue;
                }
            } else {
                if (arg == "-o") {
                    if (i + 1 < temp_raw_args.size()) i++;
                    continue;
                }
                if (ends_with(arg, ".cpp") || ends_with(arg, ".cc") || ends_with(arg, ".cxx") || ends_with(arg, ".c") ||
                    ends_with(arg, ".h") || ends_with(arg, ".hpp") || ends_with(arg, ".hxx") || ends_with(arg, ".hh")) {
                    continue;
                }
            }
            new_args.push_back(arg);
        }

        if (is_msvc) {
            new_args.push_back("/Fo" + resolved_out);
            new_args.push_back(src);
        } else {
            new_args.push_back("-o");
            new_args.push_back(resolved_out);
            new_args.push_back(src);
        }

        std::vector<char*> parse_argv;
        for (const auto& a : new_args) {
            parse_argv.push_back(const_cast<char*>(a.c_str()));
        }
        
        CompilerCommand cmd = parse(static_cast<int>(parse_argv.size()), parse_argv.data());
        commands.push_back(std::move(cmd));
    }

    return commands;
}
