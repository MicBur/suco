cmake_minimum_required(VERSION 3.15)
project(suco CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL for SHA-256 hashing
find_package(OpenSSL REQUIRED)

include_directories(
    src/common
    ${OPENSSL_INCLUDE_DIR}
)

# Common sources
set(COMMON_SOURCES
    src/common/hash_util.cpp
)

# Define platform-specific socket library
set(SOCKET_LIBS "")
if(WIN32)
    set(SOCKET_LIBS "ws2_32")
endif()

# Client: suco (Compiler wrapper)
add_executable(suco
    src/client/main.cpp
    src/client/compiler_command.cpp
    src/client/utils.cpp
    src/client/logging.cpp
    src/client/client_config.cpp
    src/client/local_compiler.cpp
    src/client/network_client.cpp
    src/client/suco_client.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)

# Coordinator: suco-coordinator
add_executable(suco-coordinator
    src/coordinator/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-coordinator
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)

# Worker: suco-worker
add_executable(suco-worker
    src/worker/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-worker
    OpenSSL::Crypto
    ${SOCKET_LIBS}
)
