#pragma once

#include <stdint.h>
#include <string_view>

namespace suco {

// Default port configurations for the SUCO grid services.
constexpr uint16_t DEFAULT_PORT = 9000;      // TCP port for coordinator/worker and client/coordinator communication.
constexpr uint16_t DEFAULT_WEB_PORT = 9001;  // TCP port for the HTTP dashboard.
constexpr uint16_t DEFAULT_UDP_PORT = 9002;  // UDP port for worker auto-discovery broadcasts.

// Packet types prefixing network messages (4-byte header field).
enum PacketType : uint32_t {
    PACKET_COMPILE_REQ = 0x01,
    PACKET_COMPILE_RESP = 0x02,
    PACKET_HEARTBEAT = 0x03,
    PACKET_CACHE_QUERY = 0x04,
    PACKET_CACHE_HIT = 0x05,
    PACKET_CACHE_MISS = 0x06,
    PACKET_CACHE_WAIT = 0x07,
    PACKET_TOOLCHAIN_REQUEST = 0x08,
    PACKET_TOOLCHAIN_TRANSFER = 0x09,
    PACKET_TOOLCHAIN_ACK = 0x0A,
    PACKET_COMPILE_BATCH_REQ = 13,
    PACKET_COMPILE_BATCH_RESP = 14,
    PACKET_HEADER_SET_REQUEST = 15,
    PACKET_TOOLCHAIN_DOWNLOAD = 16,
    PACKET_CACHE_CLEAR = 17,
    PACKET_HELLO = 18,
    PACKET_DIRECT_COMPILE_REQ = 19,
    PACKET_CACHE_STORE = 20,
    PACKET_RUN_REQ = 21,          // Generic distributed task: run an arbitrary command on a worker.
    PACKET_RUN_RESP = 22,         // Worker's response: exit code, log, and produced output files.
    PACKET_BLOB_QUERY = 23,       // Generic content-addressed blob cache (team-wide suco run cache).
    PACKET_BLOB_STORE = 24,       // Store a blob under a hash in the coordinator cache.
    // E3: like DIRECT_COMPILE_REQ, plus a trailing block of C++20 module CMIs
    // (gcm.cache/<name>.gcm bytes) the TU imports. Sent ONLY for module TUs, so
    // pre-E3 workers stay wire-compatible: they reject the unknown type and the
    // client falls back to compiling locally instead of desyncing the stream.
    PACKET_DIRECT_COMPILE_REQ_V2 = 25
};

/**
 * @brief Returns a human-readable name for a given packet type.
 * @param type The raw packet type code.
 * @return A string view representing the packet type name.
 */
inline constexpr std::string_view to_string(PacketType type) noexcept {
    switch (type) {
        case PACKET_COMPILE_REQ:        return "COMPILE_REQ";
        case PACKET_COMPILE_RESP:       return "COMPILE_RESP";
        case PACKET_HEARTBEAT:          return "HEARTBEAT";
        case PACKET_CACHE_QUERY:        return "CACHE_QUERY";
        case PACKET_CACHE_HIT:          return "CACHE_HIT";
        case PACKET_CACHE_MISS:         return "CACHE_MISS";
        case PACKET_CACHE_WAIT:         return "CACHE_WAIT";
        case PACKET_TOOLCHAIN_REQUEST:  return "TOOLCHAIN_REQUEST";
        case PACKET_TOOLCHAIN_TRANSFER: return "TOOLCHAIN_TRANSFER";
        case PACKET_TOOLCHAIN_ACK:      return "TOOLCHAIN_ACK";
        case PACKET_COMPILE_BATCH_REQ:  return "COMPILE_BATCH_REQ";
        case PACKET_COMPILE_BATCH_RESP: return "COMPILE_BATCH_RESP";
        case PACKET_HEADER_SET_REQUEST: return "HEADER_SET_REQUEST";
        case PACKET_TOOLCHAIN_DOWNLOAD: return "TOOLCHAIN_DOWNLOAD";
        case PACKET_CACHE_CLEAR:        return "CACHE_CLEAR";
        case PACKET_HELLO:              return "HELLO";
        case PACKET_DIRECT_COMPILE_REQ: return "DIRECT_COMPILE_REQ";
        case PACKET_DIRECT_COMPILE_REQ_V2: return "DIRECT_COMPILE_REQ_V2";
        case PACKET_CACHE_STORE:        return "CACHE_STORE";
        case PACKET_RUN_REQ:            return "RUN_REQ";
        case PACKET_RUN_RESP:           return "RUN_RESP";
        case PACKET_BLOB_QUERY:         return "BLOB_QUERY";
        case PACKET_BLOB_STORE:         return "BLOB_STORE";
    }
    return "UNKNOWN";
}

} // namespace suco
