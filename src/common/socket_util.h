#pragma once

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
    inline void close_socket(socket_t s) { closesocket(s); }
    inline int get_socket_error() { return WSAGetLastError(); }
    inline bool is_would_block(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #include <errno.h>

    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_VAL = -1;
    inline void close_socket(socket_t s) { ::close(s); }
    inline int get_socket_error() { return errno; }
    inline bool is_would_block(int err) { return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS; }
#endif

#include <iostream>

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

// Send exact amount of bytes
inline bool send_all(socket_t sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
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

} // namespace suco
