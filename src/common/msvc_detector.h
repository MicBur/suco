#pragma once

#include <string>

namespace suco {

/**
 * @brief Prüft, ob eine MSVC-Entwicklerumgebung (z.B. INCLUDE, LIB) bereits aktiv ist.
 * @return true wenn die Umgebung aktiv ist, false sonst.
 */
bool is_msvc_env_active();

/**
 * @brief Findet die neueste installierte Visual Studio / MSVC Version,
 *        ruft vcvarsall.bat auf und übernimmt alle Umgebungsvariablen in den aktuellen Prozess.
 * @return true bei Erfolg, false bei Fehlern.
 */
bool detect_and_setup_msvc();

} // namespace suco
