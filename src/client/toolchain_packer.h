#pragma once

#include <string>
#include <vector>

namespace suco {

struct ToolchainInfo {
    std::string compiler_type;          // e.g. "g++"
    std::string compiler_version;       // e.g. "16.1.1"
    std::string hash;                   // deterministisch berechneter Hash
    std::string archive_path;           // Pfad zu .tar.zst
    std::string json_path;              // Pfad zu .json
    std::vector<std::string> contents;  // Liste der gepackten Pfade (relativ/kurz)
    std::string resolved_compiler_path;// Absoluter aufgelöster Compiler-Pfad
    bool success = false;
};

class ToolchainPacker {
public:
    /**
     * @brief Packt eine Compiler-Toolchain in ein Archiv.
     * @param compiler_path Der Pfad zum Compiler-Executable (z.B. "/usr/bin/g++").
     * @param is_qt Ob Qt-Tools mitgepackt werden sollen.
     * @return ToolchainInfo Struktur mit Details zum erstellten Archiv.
     */
    static ToolchainInfo pack(const std::string& compiler_path, bool is_qt);
};

} // namespace suco
