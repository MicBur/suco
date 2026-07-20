#pragma once

#include <cstdint>

namespace suco {

constexpr uint32_t DAEMON_PROTOCOL_VERSION = 1;

// Response Type Flags
constexpr uint8_t IPC_RESP_STDOUT = 1;
constexpr uint8_t IPC_RESP_STDERR = 2;
constexpr uint8_t IPC_RESP_EXIT   = 3;

#ifdef _WIN32
    using ipc_socket_t = uintptr_t;
#else
    using ipc_socket_t = int;
#endif

extern thread_local ipc_socket_t t_client_socket;

bool send_ipc_frame(ipc_socket_t sock, uint8_t type, const std::string& payload);

} // namespace suco
