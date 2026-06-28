#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace suco {

/**
 * @brief Definiert den Status eines Kompilierungs-Jobs im Grid.
 */
enum class JobStatus {
    PENDING,    ///< Job wartet auf freie Slots im Grid
    RUNNING,    ///< Job wird aktuell auf einem Worker kompiliert
    DONE,       ///< Job erfolgreich abgeschlossen (Ergebnis liegt vor)
    FAILED      ///< Job fehlgeschlagen (Fehler liegt vor)
};

/**
 * @brief Kapselt alle Informationen und Zustände eines Kompilierungs-Jobs.
 */
struct Job {
    std::string id;                                     ///< Eindeutige ID des Jobs (z. B. UUID oder fortlaufender Zähler)
    std::string filename;                               ///< Name der Quelldatei (z. B. main.cpp)
    std::string hash;                                   ///< SHA-256 Hash der Präprozessor-Ausgabe und Flags
    std::string command;                                ///< Kompletter Compiler-Kommandozeilenaufruf
    std::string source;                                 ///< Der normalisierte, präprozessierte Quellcode
    JobStatus status = JobStatus::PENDING;             ///< Aktueller Status des Jobs
    std::string assigned_worker;                        ///< IP-Adresse oder Name des zugewiesenen Workers
    int attempts = 0;                                   ///< Anzahl bisheriger Zuweisungsversuche (Failover-Zähler)
    int32_t exit_code = -1;                             ///< Compiler Exit-Code (-1 bei Ausfall)
    std::string log;                                    ///< Fehlerausgabe (stderr/stdout) des Compilers
    std::vector<uint8_t> binary;                        ///< Kompilierte Binärdaten (z. B. main.o)

    // Optionale Zeitstempel für Logging, Metriken und Diagnose
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point finished_at;
};

} // namespace suco
