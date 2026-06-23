cmake_minimum_required(VERSION 3.15)
project(ag-grid-build CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL for SHA-256 hashing
find_package(OpenSSL REQUIRED)

# Find hiredis for Redis client support
# We can link it directly or look for it
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

# Client: ag-build
add_executable(ag-build
    src/client/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(ag-build
    OpenSSL::Crypto
    ${HIREDIS_LIBRARY}
    pthread
)

# Helper: ag-helper
add_executable(ag-helper
    src/helper/main.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(ag-helper
    OpenSSL::Crypto
    pthread
)
