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
    PACKET_CACHE_WAIT = 0x07
};

/**
 * @brief Returns a human-readable name for a given packet type.
 * @param type The raw packet type code.
 * @return A string view representing the packet type name.
 */
inline constexpr std::string_view to_string(PacketType type) noexcept {
    switch (type) {
        case PACKET_COMPILE_REQ:  return "COMPILE_REQ";
        case PACKET_COMPILE_RESP: return "COMPILE_RESP";
        case PACKET_HEARTBEAT:    return "HEARTBEAT";
        case PACKET_CACHE_QUERY:   return "CACHE_QUERY";
        case PACKET_CACHE_HIT:     return "CACHE_HIT";
        case PACKET_CACHE_MISS:    return "CACHE_MISS";
        case PACKET_CACHE_WAIT:    return "CACHE_WAIT";
    }
    return "UNKNOWN";
}

} // namespace suco
