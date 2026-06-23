cmake_minimum_required(VERSION 3.15)
project(suco CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL for SHA-256 hashing
find_package(OpenSSL REQUIRED)

# Find hiredis for Redis client support
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis library not found! Install it via 'sudo apt install libhiredis-dev'")
endif()

include_directories(
    src/common
    ${OPENSSL_INCLUDE_DIR}
    ${HIREDIS_INCLUDE_DIR}
)

# Common sources
set(COMMON_SOURCES
    src/common/hash_util.cpp
)

# Client: suco (Compiler wrapper)
add_executable(suco
    src/client/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco
    OpenSSL::Crypto
    ${HIREDIS_LIBRARY}
    pthread
)

# Helper: suco-helper (Daemon)
add_executable(suco-helper
    src/helper/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(suco-helper
    OpenSSL::Crypto
    ${HIREDIS_LIBRARY}
    pthread
)
