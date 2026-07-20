#pragma once

#include "compiler_command.h"
#include "client_config.h"
#include <string>
#include <vector>

namespace suco {

struct RequestContext;

class LocalPrepCache {
public:
    // Checks if a valid cache entry exists. Upon a hit, returns true and populates output strings.
    static bool try_get(
        const ClientConfig& config,
        const CompilerCommand& cmd,
        std::string& preprocessed_source,
        std::string& content_hash,
        std::string& header_set_hash,
        std::string& header_set_source,
        const RequestContext& context
    );

    // Stores the preprocessed outputs and dependency metadata in the local cache.
    static void store(
        const ClientConfig& config,
        const CompilerCommand& cmd,
        const std::string& raw_preprocessed,
        const std::string& preprocessed_source,
        const std::string& content_hash,
        const std::string& header_set_hash,
        const std::string& header_set_source,
        const RequestContext& context
    );

    // Clears the entire local preprocessor cache.
    static void clear(const ClientConfig& config);
};

// Helper to extract include files from raw preprocessed output
std::vector<std::string> extract_includes_from_preprocessed(const std::string& content, bool is_msvc);

} // namespace suco
