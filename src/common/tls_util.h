#pragma once
//
// C2 — optional transport encryption (TLS) for SUCO.
//
// Design goals:
//  * OPT-IN: off unless SUCO_TLS=1. When off, send_all/read_all use the exact
//    plaintext path as before (an empty registry → no behaviour change).
//  * MINIMAL surface: all payload I/O already funnels through send_all/read_all
//    (socket_util.h). Those consult a socket→SSL registry; TLS sockets use
//    SSL_write/SSL_read, everything else stays plain send/recv.
//  * NO cert distribution: servers use an ephemeral self-signed cert, clients
//    connect with verification disabled. TLS provides on-the-wire ENCRYPTION;
//    peer AUTHENTICATION stays the existing HMAC challenge-response.
//
// A server socket becomes TLS via tls_wrap_accept(); a client socket via
// tls_wrap_connect(). Call close_tls(sock) before closing the fd.
//
#include "socket_util.h"
#include <string>

namespace suco::tls {

// True if SUCO_TLS=1 (cached). Gate every wrap call on this.
bool enabled();

// Perform the server-side TLS handshake on an already-accepted socket and
// register it for SSL_read/SSL_write. Returns false on handshake failure
// (caller should drop the connection). No-op returning true if TLS disabled.
bool wrap_accept(socket_t sock);

// Client-side handshake on an already-connected socket (verification disabled).
bool wrap_connect(socket_t sock);

// Remove and free the SSL object for a socket (call before close_socket).
void close_tls(socket_t sock);

// Internal: used by socket_util send_all/read_all. Returns the SSL* for a
// socket, or nullptr if it is a plain socket. Cheap; safe from any thread.
void* ssl_for(socket_t sock);

// SSL_write/SSL_read wrappers (defined in tls_util.cpp so socket_util stays
// header-only and OpenSSL-free at include time). Return bytes or <0 on error.
long ssl_send(void* ssl, const void* data, size_t len);
long ssl_recv(void* ssl, void* data, size_t len);

// SSL_peek — read without consuming, for the coordinator's connection-type
// classification under TLS (raw recv(MSG_PEEK) would see ciphertext).
long ssl_peek(void* ssl, void* data, size_t len);

} // namespace suco::tls
