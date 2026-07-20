#pragma once

#include "compiler_command.h"
#include <utility>
#include <string>
#include <vector>

namespace suco {

/**
 * @brief Kapselt die lokale Ausführung des Compilers als Ausweichlösung (Fallback).
 */
class LocalCompiler {
public:
    /**
     * @brief Führt die Kompilierung lokal mit den Original-Argumenten aus.
     * @param cmd Das Compiler-Kommando.
     * @return Den Exit-Code des lokalen Compiler-Prozesses.
     */
    static int compile(const CompilerCommand& cmd, const std::string& cwd = "");
    static int execute_direct(const std::vector<std::string>& args, const std::string& cwd = "");

private:
    /**
     * @brief Hilfsfunktion zum Ausführen eines lokalen Prozesses und Capturen seiner Ausgaben.
     * @param args Die Argumentliste für den Prozess (inklusive des Programms als erstes Element).
     * @return Ein Paar bestehend aus Exit-Code und dem gemeinsamen Output von stdout und stderr.
     */
    static std::pair<int, std::string> run_and_capture(const std::vector<std::string>& args, const std::string& cwd = "");
};

} // namespace suco
