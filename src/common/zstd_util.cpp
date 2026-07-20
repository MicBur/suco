#include "zstd_util.h"
#include "logging.h"
#include <zstd.h>
#include <vector>

namespace suco {

std::string compress_zstd(const std::string& input, int level) {
    if (input.empty()) return {};
    
    size_t const max_dst_size = ZSTD_compressBound(input.size());
    std::string compressed;
    compressed.resize(max_dst_size);
    
    size_t const c_size = ZSTD_compress(
        compressed.data(), max_dst_size,
        input.data(), input.size(),
        level
    );
    
    if (ZSTD_isError(c_size)) {
        SUCO_LOG_ERROR("ZSTD_compress failed: {}", ZSTD_getErrorName(c_size));
        return {};
    }
    
    compressed.resize(c_size);
    return compressed;
}

std::string decompress_zstd(const std::string& input) {
    if (input.empty()) return {};
    
    unsigned long long const r_size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (r_size == ZSTD_CONTENTSIZE_ERROR) {
        SUCO_LOG_ERROR("ZSTD decompression: content size error (not compressed or corrupted)");
        return {};
    }
    if (r_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        SUCO_LOG_ERROR("ZSTD decompression: content size unknown");
        return {};
    }
    if (r_size > 1024ULL * 1024ULL * 1024ULL) {
        SUCO_LOG_ERROR("ZSTD decompression: content size exceeds 1 GB limit: {} bytes", r_size);
        return {};
    }
    
    std::string decompressed;
    decompressed.resize(static_cast<size_t>(r_size));
    
    size_t const d_size = ZSTD_decompress(
        decompressed.data(), static_cast<size_t>(r_size),
        input.data(), input.size()
    );
    
    if (ZSTD_isError(d_size)) {
        SUCO_LOG_ERROR("ZSTD_decompress failed: {}", ZSTD_getErrorName(d_size));
        return {};
    }
    
    decompressed.resize(d_size);
    return decompressed;
}

} // namespace suco
