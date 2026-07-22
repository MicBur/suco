cmake_minimum_required(VERSION 3.15)
project(suco VERSION 0.10.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate a version header from the single source of truth (project VERSION above).
# Every binary links this so `--version` is self-identifying across the grid.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/suco_version.h"
    @ONLY
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    link_libraries(-static-libstdc++ -static-libgcc)
endif()

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:preprocessor")
endif()

# Find OpenSSL for SHA-256 hashing
find_package(OpenSSL REQUIRED)
if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
endif()

# Find zstd for compression
if(WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6")
        # Locally built static zstd (see brain-win.md) — preferred on dev machines.
        add_library(zstd::libzstd_static STATIC IMPORTED)
        set_target_properties(zstd::libzstd_static PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6/build/cmake/build/lib/libzstd.a"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6/lib"
        )
    else()
        # System-installed zstd (MSYS2/MinGW, CI runners). Try the CMake config
        # first, then a plain library search, then the legacy hardcoded paths of
        # the original dev machine as a last resort.
        find_package(zstd CONFIG QUIET)
        if(NOT TARGET zstd::libzstd_static AND TARGET zstd::libzstd)
            add_library(zstd::libzstd_static INTERFACE IMPORTED)
            set_target_properties(zstd::libzstd_static PROPERTIES
                INTERFACE_LINK_LIBRARIES zstd::libzstd
            )
        endif()
        if(NOT TARGET zstd::libzstd_static)
            find_path(ZSTD_INCLUDE_DIR zstd.h)
            find_library(ZSTD_LIBRARY NAMES zstd libzstd zstd_static)
            if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
                add_library(zstd::libzstd_static UNKNOWN IMPORTED)
                set_target_properties(zstd::libzstd_static PROPERTIES
                    IMPORTED_LOCATION "${ZSTD_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
                )
            else()
                add_library(zstd::libzstd_static STATIC IMPORTED)
                set_target_properties(zstd::libzstd_static PROPERTIES
                    IMPORTED_LOCATION "D:/zstd-build/lib/Release/zstd_static.lib"
                    INTERFACE_INCLUDE_DIRECTORIES "D:/zstd-src-dir/zstd-1.5.6/lib"
                )
            endif()
        endif()
    endif()
else()
    find_package(zstd CONFIG REQUIRED)
    if(NOT TARGET zstd::libzstd_static)
        if(TARGET zstd::libzstd)
            add_library(zstd::libzstd_static INTERFACE IMPORTED)
            set_target_properties(zstd::libzstd_static PROPERTIES
                INTERFACE_LINK_LIBRARIES zstd::libzstd
            )
        endif()
    endif()
endif()

include_directories(
    src/common
    src/client
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${OPENSSL_INCLUDE_DIR}
)

# Common sources
set(COMMON_SOURCES
    src/common/hash_util.cpp
    src/common/logging.cpp
    src/common/msvc_detector.cpp
    src/common/zstd_util.cpp
    src/common/tls_util.cpp
)

# OpenSSL::SSL for the optional TLS transport (C2). Provided via find_package;
# add a fallback imported target if the config didn't create it.
if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
endif()

# Define platform-specific socket library
set(SOCKET_LIBS "")
if(WIN32)
    set(SOCKET_LIBS "ws2_32")
endif()

# Client Sources Shared Variable
set(CLIENT_SOURCES
    src/client/main.cpp
    src/client/compiler_command.cpp
    src/client/utils.cpp
    src/client/client_config.cpp
    src/client/local_compiler.cpp
    src/client/network_client.cpp
    src/client/suco_client.cpp
    src/client/pipeline_orchestrator.cpp
    src/client/preprocessing_pool.cpp
    src/client/job_queue.cpp
    src/client/job_sender.cpp
    src/client/batch_collector.cpp
    src/client/batch_sender.cpp
    src/client/toolchain_packer.cpp
    src/client/header_set_hasher.cpp
    src/client/local_prep_cache.cpp
    ${COMMON_SOURCES}
)

# Compiler Wrapper CC: suco-cl
add_executable(suco-cl ${CLIENT_SOURCES})
target_link_libraries(suco-cl
    OpenSSL::Crypto
    OpenSSL::SSL
    zstd::libzstd_static
    ${SOCKET_LIBS}
)

# Compiler Wrapper CXX: suco-cl++
add_executable(suco-cl++ ${CLIENT_SOURCES})
target_link_libraries(suco-cl++
    OpenSSL::Crypto
    OpenSSL::SSL
    zstd::libzstd_static
    ${SOCKET_LIBS}
)

# CLI Wrapper Tool: suco
add_executable(suco
    src/suco_wrapper/main.cpp
    src/client/client_config.cpp
    src/common/logging.cpp
    src/common/msvc_detector.cpp
    src/common/zstd_util.cpp
    src/common/hash_util.cpp
    src/common/tls_util.cpp
)
target_link_libraries(suco
    zstd::libzstd_static
    OpenSSL::Crypto
    OpenSSL::SSL
    ${SOCKET_LIBS}
)

# LD_PRELOAD I/O tracer for `suco run` auto-I/O-tracking (A4). Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(suco-trace SHARED src/suco_trace/suco_trace.c)
    set_target_properties(suco-trace PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(suco-trace ${CMAKE_DL_LIBS} pthread)
endif()

find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
    if(SQLite3_INCLUDE_DIRS)
        set(SQLITE3_INC ${SQLite3_INCLUDE_DIRS})
    else()
        set(SQLITE3_INC ${SQLite3_INCLUDE_DIR})
    endif()
    set_target_properties(SQLite3::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INC}"
    )
endif()


# Coordinator: suco-coordinator
add_executable(suco-coordinator
    src/coordinator/main.cpp
    src/coordinator/config.cpp
    src/coordinator/worker_manager.cpp
    src/coordinator/scheduler.cpp
    src/coordinator/job_queue.cpp
    src/coordinator/coordinator.cpp
    src/coordinator/network_server.cpp
    src/coordinator/client_handler.cpp
    src/coordinator/batch_processor.cpp
    src/coordinator/worker_handler.cpp
    src/coordinator/history_writer.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-coordinator
    OpenSSL::Crypto
    OpenSSL::SSL
    zstd::libzstd_static
    SQLite3::SQLite3
    ${SOCKET_LIBS}
)

# Worker: suco-worker
add_executable(suco-worker
    src/worker/main.cpp
    src/worker/config.cpp
    src/worker/metrics.cpp
    src/worker/network_client.cpp
    src/worker/heartbeat_manager.cpp
    src/worker/job_executor.cpp
    src/worker/worker.cpp
    src/worker/toolchain_detector.cpp
    src/worker/toolchain_manager.cpp
    src/worker/header_cache.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-worker
    OpenSSL::Crypto
    OpenSSL::SSL
    zstd::libzstd_static
    ${SOCKET_LIBS}
)

# Daemon: suco-daemon
set(DAEMON_SOURCES
    src/daemon/main.cpp
    src/client/compiler_command.cpp
    src/client/utils.cpp
    src/client/client_config.cpp
    src/client/local_compiler.cpp
    src/client/network_client.cpp
    src/client/suco_client.cpp
    src/client/pipeline_orchestrator.cpp
    src/client/preprocessing_pool.cpp
    src/client/job_queue.cpp
    src/client/job_sender.cpp
    src/client/batch_collector.cpp
    src/client/batch_sender.cpp
    src/client/toolchain_packer.cpp
    src/client/header_set_hasher.cpp
    src/client/local_prep_cache.cpp
    ${COMMON_SOURCES}
)
add_executable(suco-daemon ${DAEMON_SOURCES})
target_link_libraries(suco-daemon
    OpenSSL::Crypto
    OpenSSL::SSL
    zstd::libzstd_static
    ${SOCKET_LIBS}
)

# ---------------------------------------------------------------------------
# Install rules + packaging (Linux). `cmake --install` and `cpack` both use these.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(GNUInstallDirs)

    install(TARGETS
        suco suco-cl suco-cl++ suco-coordinator suco-worker suco-daemon
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    if(TARGET suco-trace)
        install(TARGETS suco-trace LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    install(FILES dashboard.html
        DESTINATION ${CMAKE_INSTALL_DATADIR}/suco
    )
    # systemd units ship disabled; the admin enables what a node should run
    # (see debian/postinst below — install does NOT auto-start services).
    install(FILES
        packaging/systemd/suco-coordinator.service
        packaging/systemd/suco-worker.service
        DESTINATION lib/systemd/system
    )

    set(CPACK_PACKAGE_NAME "suco")
    set(CPACK_PACKAGE_VENDOR "SUCO")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SUCO — distributed C/C++ compiler with content-addressed caching")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CONTACT "micbur1488@gmail.com")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SUCO <micbur1488@gmail.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/micbur/suco")
    set(CPACK_PACKAGE_DESCRIPTION "SUCO is a distributed C/C++ compiler for LANs. It dispatches\n compile jobs across a worker grid with content-addressed caching, so warm\n rebuilds are served from cache and cold builds fan out over every node.\n Ships the suco-cl/suco-cl++ compiler wrappers, the coordinator and worker\n daemons, and a web dashboard.")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, gcc, g++")
    # postinst runs `systemctl daemon-reload`; prerm stops any enabled units.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm"
    )
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    include(CPack)
endif()
