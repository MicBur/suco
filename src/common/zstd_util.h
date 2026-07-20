#pragma once
#include <string>

namespace suco {

/**
 * @brief Compresses an input string using zstd.
 * @param input The raw input data.
 * @param level The zstd compression level (typically 1-3).
 * @return The compressed data.
 */
std::string compress_zstd(const std::string& input, int level = 1);

/**
 * @brief Decompresses an input string using zstd.
 * @param input The compressed data.
 * @return The decompressed raw data, or empty string on error.
 */
std::string decompress_zstd(const std::string& input);

} // namespace suco
