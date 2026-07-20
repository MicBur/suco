#pragma once

#include <string>
#include <vector>
#include <utility>
#include <stdint.h>

namespace suco::worker {

class JobExecutor {
public:
    struct Result {
        int exit_code = 0;
        bool header_cache_hit = false;
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
     * @param toolchain_hash Der optionale Toolchain-Hash für die Pfad-Umschreibung.
     * @param header_set_hash Der optionale Header-Set-Hash.
     * @param header_set_source Der optionale Header-Set-Quellcode.
     * @param module_cmis Optionale C++20-Modul-CMIs (Name, .gcm-Bytes), die der Job importiert.
     * @return Das Ergebnis (Exit-Code, Log-Ausgaben, Binärdaten).
     */
    static Result execute(const std::string& command, 
                          const std::string& filename, 
                          const std::string& source, 
                          int timeout_seconds, 
                          const std::string& toolchain_hash = "",
                          const std::string& header_set_hash = "",
                          const std::string& header_set_source = "",
                          const std::vector<std::pair<std::string, std::string>>& module_cmis = {});

    static std::string run_local_capture(const std::string& cmd, int& exit_code, int timeout_seconds);
    static std::string get_temp_file(const std::string& suffix);

private:
    static std::string rebuild_compiler_command(const std::string& orig_cmd, const std::string& temp_in, const std::string& temp_out, bool is_msvc, bool is_c, const std::string& toolchain_hash = "", bool use_header_cache = false);
    static bool check_is_msvc(const std::string& cmd);
};

} // namespace suco::worker
