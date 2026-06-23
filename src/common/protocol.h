#ifndef SUCO_PROTOCOL_H
#define SUCO_PROTOCOL_H

#include <stdint.h>
#include <string>
#include <vector>

namespace suco {

constexpr uint16_t DEFAULT_PORT = 9000;

// Simple protocol structures for TCP serialization

struct CompileRequest {
    std::string compiler_command; // e.g. "g++ -O3 -std=c++17"
    std::string source_content;   // Preprocessed source code (.ii)
};

struct CompileResponse {
    int32_t exit_code;            // Compiler exit code (0 = success)
    std::string compiler_output;  // stdout + stderr output
    std::vector<uint8_t> object_data; // Compiled .o file bytes
};

} // namespace suco

#endif // SUCO_PROTOCOL_H
