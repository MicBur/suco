#pragma once

#include <stdint.h>

namespace suco {

// Standard ports
constexpr uint16_t DEFAULT_PORT = 9000;      // TCP coordinator/worker & client/coordinator
constexpr uint16_t DEFAULT_WEB_PORT = 9001;  // HTTP API for Dashboard
constexpr uint16_t DEFAULT_UDP_PORT = 9002;  // UDP Discovery Broadcast

// Packet types (4 bytes prefix)
enum PacketType : uint32_t {
    PACKET_COMPILE_REQ = 0x01,
    PACKET_COMPILE_RESP = 0x02,
    PACKET_HEARTBEAT = 0x03,
    PACKET_CACHE_QUERY = 0x04,
    PACKET_CACHE_HIT = 0x05,
    PACKET_CACHE_MISS = 0x06
};

} // namespace suco
