#include "toolchain_detector.h"

#include <cstdio>
#include <cctype>
#include <algorithm>
#include <iostream>

namespace suco::worker {
namespace {

// Führt einen Shell-Befehl aus und gibt die Ausgabe zurück
std::string exec_command(const std::string& cmd) {
    std::string result;
    char buffer[128];
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

// Extrahiert das erste Versionsmuster (X.Y.Z oder X.Y) aus einem String
std::string extract_version(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            size_t start = i;
            bool has_dot = false;
            size_t dot_count = 0;
            while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
                if (text[i] == '.') {
                    has_dot = true;
                    dot_count++;
                }
                i++;
            }
            if (has_dot && dot_count <= 3) {
                std::string version = text.substr(start, i - start);
                while (!version.empty() && version.back() == '.') {
                    version.pop_back();
                }
                return version;
            }
        } else {
            i++;
        }
    }
    return "";
}

} // namespace

ToolchainInfo ToolchainDetector::detect() {
    ToolchainInfo info;

    // 1. g++ / gcc
    std::string gxx_out = exec_command("g++ --version");
    if (!gxx_out.empty()) {
        std::string ver = extract_version(gxx_out);
        if (!ver.empty()) {
            info.compilers["g++"] = ver;
        }
    }

    // 1b. MinGW cross/target compilers. Windows clients dispatch their jobs under
    // the target-qualified name (x86_64-w64-mingw32-g++), and the scheduler matches
    // that against THIS map — so a worker serves Windows clients if and only if it
    // advertises the driver here. On Linux nodes that is the mingw-w64 package; on
    // Windows workers the toolchain ships the alias anyway. Absent → probe fails →
    // not advertised → the scheduler skips this worker for such jobs. No wire-format
    // change: the toolchain map has always been an open name→version dictionary.
    for (const char* cross : {"x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-gcc"}) {
        std::string cross_out = exec_command(std::string(cross) + " --version");
        if (!cross_out.empty()) {
            std::string ver = extract_version(cross_out);
            if (ver.empty()) {
                // Debian/Ubuntu's mingw-w64 reports a dotless version — "13-posix" /
                // "13-win32" — which extract_version drops because it requires a dot.
                // Use -dumpversion, whose output is just that bare token (no "x86_64"
                // prefix to trip a leading-digit scan), and take its leading integer.
                // The scheduler compares major versions only, so "13" vs a client's
                // "13.1.0" still matches. Only these cross drivers reach this path —
                // g++ / clang always print a dotted version, so they are unchanged.
                std::string dv = exec_command(std::string(cross) + " -dumpversion");
                for (char c : dv) {
                    if (std::isdigit(static_cast<unsigned char>(c))) ver += c;
                    else if (!ver.empty()) break;
                }
            }
            if (!ver.empty()) {
                info.compilers[cross] = ver;
            }
        }
    }

    // 2. clang++ / clang
    std::string clang_out = exec_command("clang++ --version");
    if (!clang_out.empty()) {
        std::string ver = extract_version(clang_out);
        if (!ver.empty()) {
            info.compilers["clang++"] = ver;
        }
    }

    // 3. cl.exe (MSVC)
#ifdef _WIN32
    std::string cl_out = exec_command("cl 2>&1");
    if (!cl_out.empty()) {
        size_t ver_pos = cl_out.find("Version");
        if (ver_pos != std::string::npos) {
            std::string ver = extract_version(cl_out.substr(ver_pos));
            if (!ver.empty()) {
                info.compilers["cl"] = ver;
            }
        }
    }
#endif

    // 4. cmake
    std::string cmake_out = exec_command("cmake --version");
    if (!cmake_out.empty()) {
        std::string ver = extract_version(cmake_out);
        if (!ver.empty()) {
            info.tools["cmake"] = ver;
        }
    }

    // 5. ninja
    std::string ninja_out = exec_command("ninja --version");
    if (!ninja_out.empty()) {
        // Ninja gibt direkt nur die Version aus
        std::string ver = extract_version(ninja_out);
        if (!ver.empty()) {
            info.tools["ninja"] = ver;
        }
    }

    // 6. qmake / qmake6
    // Versuche qmake6 zuerst, falls nicht vorhanden qmake
    std::string qmake_out = exec_command("qmake6 --version");
    if (qmake_out.empty() || qmake_out.find("Qt version") == std::string::npos) {
        qmake_out = exec_command("qmake --version");
    }
    if (!qmake_out.empty()) {
        size_t qt_pos = qmake_out.find("Qt version");
        if (qt_pos != std::string::npos) {
            std::string ver = extract_version(qmake_out.substr(qt_pos));
            if (!ver.empty()) {
                info.qt_versions["qmake"] = ver;
            }
        }
    }

    return info;
}

std::string ToolchainDetector::to_json(const ToolchainInfo& info) {
    std::string json = "{\n  \"version\": 1,\n";
    
    // compilers
    json += "  \"compilers\": {\n";
    bool first = true;
    for (const auto& [name, ver] : info.compilers) {
        if (!first) json += ",\n";
        json += "    \"" + name + "\": \"" + ver + "\"";
        first = false;
    }
    json += "\n  },\n";

    // tools
    json += "  \"tools\": {\n";
    first = true;
    for (const auto& [name, ver] : info.tools) {
        if (!first) json += ",\n";
        json += "    \"" + name + "\": \"" + ver + "\"";
        first = false;
    }
    json += "\n  },\n";

    // qt
    json += "  \"qt\": {\n";
    first = true;
    for (const auto& [name, ver] : info.qt_versions) {
        if (!first) json += ",\n";
        json += "    \"" + name + "\": \"" + ver + "\"";
        first = false;
    }
    json += "\n  }\n}";

    return json;
}

} // namespace suco::worker
