#pragma once

#include "compiler_command.h"
#include <string>

namespace suco {

/**
 * @brief Berechnet einen stabilen SHA-256 Hash für ein Header-Set.
 */
class HeaderSetHasher {
public:
    /**
     * @brief Berechnet den Hash des Header-Sets für den übergebenen Kompilierbefehl.
     * @param cmd Der CompilerCommand-Job.
     * @return Der berechnete Hash als Hex-String.
     */
    static std::string compute_hash(CompilerCommand& cmd);
};

} // namespace suco
