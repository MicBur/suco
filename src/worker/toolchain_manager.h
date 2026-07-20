#pragma once

#include <string>
#include <vector>

namespace suco {

class ToolchainManager {
public:
    /**
     * @brief Prüft, ob eine Toolchain mit dem angegebenen Hash bereits existiert und entpackt ist.
     * @param hash Der Toolchain-Hash.
     * @return true wenn die Toolchain existiert, sonst false.
     */
    static bool has_toolchain(const std::string& hash);

    /**
     * @brief Gibt den Pfad zum entpackten Toolchain-Verzeichnis zurück.
     * @param hash Der Toolchain-Hash.
     * @return Der absolute Verzeichnispfad.
     */
    static std::string get_toolchain_path(const std::string& hash);

    /**
     * @brief Entpackt ein Toolchain-Archiv unter ~/.cache/suco/toolchains/<hash>/
     * @param hash Der Toolchain-Hash.
     * @param archive_path Der Pfad zum .tar.zst Archiv.
     * @return true bei Erfolg, sonst false.
     */
    static bool extract_toolchain(const std::string& hash, const std::string& archive_path);

    /**
     * @brief Liefert alle lokal gecachten Toolchain-Hashes.
     */
    static std::vector<std::string> list_toolchains();
};

} // namespace suco
