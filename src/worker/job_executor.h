#pragma once

#include <string>
#include <vector>
#include <stdint.h>

namespace suco::worker {

class JobExecutor {
public:
    struct Result {
        int exit_code = 0;
        std::string log;
        std::vector<uint8_t> binary;
    };

    /**
     * @brief Führt einen Kompilierungsjob aus.
     *        Baut den Befehl um, erzeugt temporäre Dateien, führt den Compiler
     *        aus, liest das Resultat-Objekt und räumt danach restlos auf.
     * @param command Der ursprüngliche Compiler-Befehl vom Client.
     * @param filename Der Name der zu kompilierenden Quelldatei.
     * @param source Der Inhalt des präprozessierten Quellcodes.
     * @param timeout_seconds Die maximale Ausführungsdauer in Sekunden.
     * @return Das Ergebnis (Exit-Code, Log-Ausgaben, Binärdaten).
     */
    static Result execute(const std::string& command, const std::string& filename, const std::string& source, int timeout_seconds);

private:
    static std::string rebuild_compiler_command(const std::string& orig_cmd, const std::string& temp_in, const std::string& temp_out, bool is_msvc, bool is_c);
    static bool check_is_msvc(const std::string& cmd);
    static std::string run_local_capture(const std::string& cmd, int& exit_code, int timeout_seconds);
    static std::string get_temp_file(const std::string& suffix);
};

} // namespace suco::worker
