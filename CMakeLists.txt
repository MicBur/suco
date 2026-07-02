cmake_minimum_required(VERSION 3.15)
project(suco CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    link_libraries(-static-libstdc++ -static-libgcc)
endif()

# Find OpenSSL for SHA-256 hashing
find_package(OpenSSL REQUIRED)

include_directories(
    src/common
    ${OPENSSL_INCLUDE_DIR}
)

# Common sources
set(COMMON_SOURCES
    src/common/hash_util.cpp
    src/common/logging.cpp
)

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
    ${COMMON_SOURCES}
)

# Compiler Wrapper CC: suco-cl
add_executable(suco-cl ${CLIENT_SOURCES})
target_link_libraries(suco-cl
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)

# Compiler Wrapper CXX: suco-cl++
add_executable(suco-cl++ ${CLIENT_SOURCES})
target_link_libraries(suco-cl++
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)

# CLI Wrapper Tool: suco
add_executable(suco
    src/suco_wrapper/main.cpp
    src/client/client_config.cpp
)

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
    src/coordinator/worker_handler.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-coordinator
    OpenSSL::Crypto
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
    ${COMMON_SOURCES}
)
target_link_libraries(suco-worker
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)
