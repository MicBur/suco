#include "msvc_detector.h"
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
    #include <array>
    #include <memory>
    #include <filesystem>
    #include <sstream>

    namespace {

    // Hilfsfunktion zum Ausführen eines Befehls und Erfassen von stdout
    std::string run_command_popen(const std::string& cmd) {
        std::array<char, 256> buffer;
        std::string result;
        
        // Nutze _popen unter Windows
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) {
            return "";
        }
        
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            result += buffer.data();
        }
        
        _pclose(pipe);
        return result;
    }

    // Entfernt führende und folgende Whitespaces/Newlines
    std::string trim_str(std::string str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    } // namespace
#endif

namespace suco {

bool is_msvc_env_active() {
    // Wenn INCLUDE und LIB bereits gesetzt sind, ist die MSVC-Entwicklerumgebung aktiv
    const char* inc = std::getenv("INCLUDE");
    const char* lib = std::getenv("LIB");
    return (inc != nullptr && lib != nullptr);
}

bool detect_and_setup_msvc() {
#ifndef _WIN32
    return false; // Nur für Windows relevant
#else
    if (is_msvc_env_active()) {
        return true; // Bereits aktiv, nichts zu tun
    }

    // 1. Suche nach vswhere.exe
    std::string vswhere_path;
    const char* program_files_x86 = std::getenv("ProgramFiles(x86)");
    if (program_files_x86) {
        vswhere_path = std::string(program_files_x86) + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    } else {
        vswhere_path = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    }

    std::error_code ec;
    if (!std::filesystem::exists(vswhere_path, ec)) {
        // Versuche alternativen Pfad im 64-Bit-ProgramFiles
        const char* program_files = std::getenv("ProgramFiles");
        if (program_files) {
            vswhere_path = std::string(program_files) + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        }
    }

    if (!std::filesystem::exists(vswhere_path, ec)) {
        std::cerr << "suco msvc_detector error: vswhere.exe nicht gefunden bei: " << vswhere_path << std::endl;
        return false;
    }

    // 2. Führe vswhere.exe aus, um den Installationspfad des neuesten Visual Studios zu finden
    std::string cmd_vswhere = "\"" + vswhere_path + "\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath";
    std::string vs_install_path = trim_str(run_command_popen(cmd_vswhere));

    if (vs_install_path.empty()) {
        std::cerr << "suco msvc_detector error: Keine kompatible Visual Studio Installation mit C++ Build Tools gefunden." << std::endl;
        return false;
    }

    // 3. Überprüfe, ob vcvarsall.bat existiert
    std::string vcvars_path = vs_install_path + "\\VC\\Auxiliary\\Build\\vcvarsall.bat";
    if (!std::filesystem::exists(vcvars_path, ec)) {
        std::cerr << "suco msvc_detector error: vcvarsall.bat nicht gefunden unter: " << vcvars_path << std::endl;
        return false;
    }

    // 4. Rufe vcvarsall.bat amd64 auf und gib mit "set" die resultierenden Umgebungsvariablen aus
    // Wir setzen den CodePage-Modus auf 65001 (UTF-8), um Zeichensatzkonflikte zu vermeiden
    std::string cmd_vcvars = "chcp 65001 >nul && cmd.exe /c \"call \"" + vcvars_path + "\" amd64 && set\"";
    std::string env_output = run_command_popen(cmd_vcvars);

    if (env_output.empty()) {
        std::cerr << "suco msvc_detector error: Fehler beim Ausführen von vcvarsall.bat." << std::endl;
        return false;
    }

    // 5. Parse und importiere die Umgebungsvariablen
    std::stringstream ss(env_output);
    std::string line;
    int imported_count = 0;

    while (std::getline(ss, line)) {
        line = trim_str(line);
        if (line.empty()) continue;

        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) continue;

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);

        // Wir importieren gezielt die wichtigen Variablen für Compiler, Linker und SDKs
        // (Oder setzen einfach alle, falls gewünscht, aber Filterung ist sauberer)
        static const std::vector<std::string> target_vars = {
            "PATH", "INCLUDE", "LIB", "LIBPATH", "VCINSTALLDIR", 
            "WindowsSdkDir", "WindowsSDKVersion", "UCRTVersion", 
            "ExtensionSdkDir", "FrameworkDir", "FrameworkSDKDir", "FrameworkVersion",
            "CommandPromptType", "VisualStudioVersion"
        };

        bool should_import = false;
        for (const auto& var : target_vars) {
            // Case-insensitive comparison
            if (key.size() == var.size()) {
                bool match = true;
                for (size_t i = 0; i < key.size(); ++i) {
                    if (std::tolower(key[i]) != std::tolower(var[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    should_import = true;
                    break;
                }
            }
        }

        if (should_import) {
            // Setze sowohl Win32 als auch CRT Umgebungsvariablen
            SetEnvironmentVariableA(key.c_str(), val.c_str());
            _putenv_s(key.c_str(), val.c_str());
            imported_count++;
        }
    }

    if (imported_count == 0) {
        std::cerr << "suco msvc_detector warning: Keine MSVC-Umgebungsvariablen importiert." << std::endl;
        return false;
    }

    return true;
#endif
}

} // namespace suco
