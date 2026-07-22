#pragma once

#include "platform_compat.h"  // ssize_t + POSIX-name shims under MSVC

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")

    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
    inline int get_socket_error() { return WSAGetLastError(); }
    inline bool is_would_block(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
    #ifndef SHUT_RDWR
        #define SHUT_RDWR SD_BOTH
    #endif
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #include <netinet/tcp.h>   // TCP_NODELAY
    #include <errno.h>

    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_VAL = -1;
    inline int get_socket_error() { return errno; }
    inline bool is_would_block(int err) { return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS; }
#endif

// Forward-decl so close_socket can free any TLS state for the fd (prevents a
// stale SSL* lingering in the registry if the fd number is later reused). No-op
// when TLS is off. Full tls interface is declared inside namespace suco below.
namespace suco { namespace tls { void close_tls(socket_t sock); } }

// Disable Nagle's algorithm. SUCO's protocol is a sequence of small
// request/response round-trips (HELLO handshake, HMAC auth, cache query,
// dispatch headers). With Nagle on, each small send waits for the peer's ACK,
// and the peer's delayed-ACK (~40 ms on Windows) stacks onto every round-trip —
// so a bare cache query cost ~90 ms instead of a few ms. Set on both connect and
// accept sides. Best-effort: a failure just leaves Nagle on, never fatal.
namespace suco {
inline void set_tcp_nodelay(socket_t sock) {
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
}
}

// Unified socket close: release TLS state first, then close the fd.
inline void close_socket(socket_t s) {
    suco::tls::close_tls(s);
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace suco {

// RAII Wrapper for Winsock initialization under Windows
class SocketInit {
public:
    SocketInit() {
#ifdef _WIN32
        WSADATA wsaData;
        int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (res != 0) {
            std::cerr << "suco error: WSAStartup failed with error " << res << std::endl;
        }
#endif
    }

    ~SocketInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

// Forward decls for the optional TLS transport (defined in tls_util.cpp). When a
// socket has been TLS-wrapped, send_all/read_all route through SSL; otherwise they
// use the unchanged plaintext path. ssl_for() is a no-op (returns nullptr) unless
// SUCO_TLS=1, so the default build behaves exactly as before.
namespace tls {
    void* ssl_for(socket_t sock);
    long ssl_send(void* ssl, const void* data, size_t len);
    long ssl_recv(void* ssl, void* data, size_t len);
}

// Send exact amount of bytes
inline bool send_all(socket_t sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    if (void* ssl = tls::ssl_for(sock)) {
        while (len > 0) {
            long sent = tls::ssl_send(ssl, ptr, len);
            if (sent <= 0) return false;
            ptr += sent;
            len -= static_cast<size_t>(sent);
        }
        return true;
    }
    while (len > 0) {
#ifdef _WIN32
        int sent = send(sock, ptr, static_cast<int>(len), 0);
#else
        ssize_t sent = send(sock, ptr, len, 0);
#endif
        if (sent <= 0) return false;
        ptr += sent;
        len -= sent;
    }
    return true;
}

// Read exact amount of bytes
inline bool read_all(socket_t sock, void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    if (void* ssl = tls::ssl_for(sock)) {
        while (len > 0) {
            long received = tls::ssl_recv(ssl, ptr, len);
            if (received <= 0) return false;
            ptr += received;
            len -= static_cast<size_t>(received);
        }
        return true;
    }
    while (len > 0) {
#ifdef _WIN32
        int received = recv(sock, ptr, static_cast<int>(len), 0);
#else
        ssize_t received = recv(sock, ptr, len, 0);
#endif
        if (received <= 0) return false;
        ptr += received;
        len -= received;
    }
    return true;
}

// Helper to set socket non-blocking
inline bool set_socket_nonblocking(socket_t sock, bool nonblocking) {
#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

// Helper to send a file over a socket
inline bool send_file(socket_t sock, const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f) return false;
    uint32_t size = static_cast<uint32_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    uint32_t size_net = htonl(size);
    if (!send_all(sock, &size_net, 4)) return false;

    std::vector<char> buf(65536);
    while (size > 0) {
        uint32_t chunk = std::min(size, static_cast<uint32_t>(buf.size()));
        f.read(buf.data(), chunk);
        if (!send_all(sock, buf.data(), chunk)) return false;
        size -= chunk;
    }
    return true;
}

// Helper to receive a file from a socket and save it to disk
inline bool receive_file(socket_t sock, const std::string& filepath) {
    uint32_t size_net = 0;
    if (!read_all(sock, &size_net, 4)) return false;
    uint32_t size = ntohl(size_net);

    std::ofstream f(filepath, std::ios::binary);
    if (!f) return false;

    std::vector<char> buf(65536);
    while (size > 0) {
        uint32_t chunk = std::min(size, static_cast<uint32_t>(buf.size()));
        if (!read_all(sock, buf.data(), chunk)) return false;
        f.write(buf.data(), chunk);
        size -= chunk;
    }
    return true;
}

inline std::string get_daemon_socket_path() {
#ifdef _WIN32
    return "\\\\.\\pipe\\suco_daemon_pipe";
#else
    std::string path;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) {
        path = std::string(xdg) + "/suco/daemon.sock";
    } else {
        uid_t uid = getuid();
        path = "/tmp/suco-" + std::to_string(uid) + "/daemon.sock";
    }
    
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
        chmod(dir.c_str(), S_IRWXU);
    }
    return path;
#endif
}

} // namespace suco
