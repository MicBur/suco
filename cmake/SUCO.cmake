# SUCO.cmake - Integration helper for CMake projects
#
# Um SUCO in ein CMake-Projekt einzubinden, füge folgende Zeile in die
# oberste CMakeLists.txt Datei (nach project(...) aber vor add_executable/library):
#
#   include(cmake/SUCO.cmake)
#
# Dieses Modul sucht nach den SUCO-Compiler-Wrapper-Binaries (suco-cl und suco-cl++)
# und registriert sie als Compiler-Launcher für C und C++.

option(WITH_SUCO "Build with SUCO distributed compiler cache" ON)
if(NOT WITH_SUCO)
    message(STATUS "SUCO: Integration deaktiviert (WITH_SUCO=OFF).")
    unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
    unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
    return()
endif()

find_program(SUCO_C_LAUNCHER 
    NAMES suco-cl suco-cl.exe
    DOC "Pfad zum SUCO C Compiler Launcher"
)

find_program(SUCO_CXX_LAUNCHER 
    NAMES suco-cl++ suco-cl++.exe
    DOC "Pfad zum SUCO C++ Compiler Launcher"
)

# Falls nicht im System-PATH gefunden, suchen wir nach relativen Entwicklungs-Pfaden
if(NOT SUCO_C_LAUNCHER OR NOT SUCO_CXX_LAUNCHER)
    find_program(SUCO_C_LAUNCHER 
        NAMES suco-cl suco-cl.exe
        PATHS "${CMAKE_CURRENT_LIST_DIR}/../build" "${CMAKE_CURRENT_LIST_DIR}/../build/Release" "${CMAKE_CURRENT_LIST_DIR}/../build/Debug"
        NO_DEFAULT_PATH
    )
    find_program(SUCO_CXX_LAUNCHER 
        NAMES suco-cl++ suco-cl++.exe
        PATHS "${CMAKE_CURRENT_LIST_DIR}/../build" "${CMAKE_CURRENT_LIST_DIR}/../build/Release" "${CMAKE_CURRENT_LIST_DIR}/../build/Debug"
        NO_DEFAULT_PATH
    )
endif()

# 1. Automatisch compile_commands.json aktivieren für VS Code / clangd Unterstützung
if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for IDEs" FORCE)
endif()

# Warnung bei Inkompatibilität des Generators (Visual Studio Generator exportiert keine compile_commands)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND CMAKE_GENERATOR MATCHES "Visual Studio")
    message(WARNING "SUCO: CMAKE_EXPORT_COMPILE_COMMANDS wird vom Visual Studio Generator nicht unterstützt. "
                    "Für VS Code / IDE-Integration wird dringend die Verwendung des Ninja Generators empfohlen: "
                    "cmake -G Ninja ...")
endif()

if(SUCO_C_LAUNCHER AND SUCO_CXX_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${SUCO_C_LAUNCHER}" CACHE STRING "SUCO compiler launcher for C" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SUCO_CXX_LAUNCHER}" CACHE STRING "SUCO compiler launcher for C++" FORCE)
    message(STATUS "SUCO: Integration erfolgreich!")
    message(STATUS "  C Compiler-Launcher:   ${CMAKE_C_COMPILER_LAUNCHER}")
    message(STATUS "  C++ Compiler-Launcher: ${CMAKE_CXX_COMPILER_LAUNCHER}")
    if(CMAKE_EXPORT_COMPILE_COMMANDS)
        message(STATUS "  IDE-Unterstützung:     compile_commands.json ist aktiv.")
        
        # Post-Processing Script für compile_commands.json generieren
        set(SUCO_CLEAN_SCRIPT "${CMAKE_BINARY_DIR}/SUCOClean.cmake")
        file(WRITE "${SUCO_CLEAN_SCRIPT}" [[
            if(EXISTS "${BUILD_DIR}/compile_commands.json")
                file(READ "${BUILD_DIR}/compile_commands.json" CONTENT)
                
                # Markieren mit suco_used und suco_build
                if(NOT CONTENT MATCHES "suco_used")
                    string(REPLACE "\"directory\":" "\"suco_used\": true, \"suco_build\": true, \"directory\":" CONTENT "${CONTENT}")
                endif()
                
                # Bereinige suco-cl/suco-cl++ launcher Pfade
                string(REGEX REPLACE "\"command\": *\"[^\"]*suco-cl(\\+\\+)?(\\.exe)? " "\"command\": \"" CONTENT "${CONTENT}")
                string(REGEX REPLACE "\"arguments\": *\\[ *\"[^\"]*suco-cl(\\+\\+)?(\\.exe)?\",?" "\"arguments\": [" CONTENT "${CONTENT}")
                
                file(WRITE "${BUILD_DIR}/compile_commands.json" "${CONTENT}")
            endif()
        ]])
        
        # Custom Target hinzufügen, das am Ende des Builds läuft
        if(NOT TARGET suco_clean_compile_commands)
            add_custom_target(suco_clean_compile_commands ALL
                COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -P ${SUCO_CLEAN_SCRIPT}
                COMMENT "SUCO: Bereinige compile_commands.json für IDEs..."
                VERBATIM
            )
        endif()
    endif()
else()
    message(WARNING "SUCO: Compiler-Launcher (suco-cl/suco-cl++) wurden nicht gefunden!")
    message(WARNING "  Stelle sicher, dass SUCO kompiliert und im System-PATH registriert ist.")
endif()
