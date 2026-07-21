#include "tls_util.h"
#include "logging.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace suco::tls {

namespace {

std::once_flag g_init_once;
std::atomic<bool> g_enabled{false};
SSL_CTX* g_server_ctx = nullptr;
SSL_CTX* g_client_ctx = nullptr;

// socket → SSL registry. Guarded by a shared mutex; lookups are per network op
// (not per byte) so the lock cost is negligible next to the syscall.
std::mutex g_reg_mutex;
std::unordered_map<socket_t, SSL*> g_reg;

// Generate an ephemeral self-signed cert+key for the server context. Encryption
// only — peer identity is established by the existing HMAC handshake, so we do
// not persist or distribute this cert.
bool make_self_signed(SSL_CTX* ctx) {
    EVP_PKEY* pkey = nullptr;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    pkey = EVP_RSA_gen(2048);
#else
    EVP_PKEY_CTX* pkey_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (pkey_ctx) {
        if (EVP_PKEY_keygen_init(pkey_ctx) > 0) {
            if (EVP_PKEY_CTX_set_rsa_keygen_bits(pkey_ctx, 2048) > 0) {
                EVP_PKEY_keygen(pkey_ctx, &pkey);
            }
        }
        EVP_PKEY_CTX_free(pkey_ctx);
    }
#endif
    if (!pkey) return false;
    X509* x509 = X509_new();
    if (!x509) { EVP_PKEY_free(pkey); return false; }
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 10L * 365 * 24 * 3600);  // 10y
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("suco-grid"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    bool ok = X509_sign(x509, pkey, EVP_sha256()) &&
              SSL_CTX_use_certificate(ctx, x509) == 1 &&
              SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

void init_once() {
    const char* e = std::getenv("SUCO_TLS");
    bool want = e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "true") == 0);
    if (!want) return;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    g_server_ctx = SSL_CTX_new(TLS_server_method());
    g_client_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_server_ctx || !g_client_ctx) {
        SUCO_LOG_ERROR("TLS: failed to create SSL contexts — falling back to plaintext");
        return;
    }
    SSL_CTX_set_min_proto_version(g_server_ctx, TLS1_2_VERSION);
    SSL_CTX_set_min_proto_version(g_client_ctx, TLS1_2_VERSION);
    // Client: encryption only, identity via HMAC → do not verify the cert chain.
    SSL_CTX_set_verify(g_client_ctx, SSL_VERIFY_NONE, nullptr);

    if (!make_self_signed(g_server_ctx)) {
        SUCO_LOG_ERROR("TLS: failed to generate self-signed cert — falling back to plaintext");
        return;
    }
    g_enabled = true;
    SUCO_LOG_INFO("TLS: transport encryption ENABLED (SUCO_TLS=1, self-signed, HMAC auth).");
}

bool handshake(socket_t sock, bool server) {
    SSL* ssl = SSL_new(server ? g_server_ctx : g_client_ctx);
    if (!ssl) return false;
    SSL_set_fd(ssl, static_cast<int>(sock));
    int rc = server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (rc != 1) {
        SUCO_LOG_WARNING("TLS: handshake ({}) failed: {}", server ? "accept" : "connect",
                         ERR_error_string(ERR_get_error(), nullptr));
        SSL_free(ssl);
        return false;
    }
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    g_reg[sock] = ssl;
    return true;
}

} // namespace

bool enabled() {
    std::call_once(g_init_once, init_once);
    return g_enabled.load();
}

bool wrap_accept(socket_t sock) {
    if (!enabled()) return true;
    return handshake(sock, /*server=*/true);
}

bool wrap_connect(socket_t sock) {
    if (!enabled()) return true;
    return handshake(sock, /*server=*/false);
}

void close_tls(socket_t sock) {
    if (!g_enabled.load()) return;   // fast path: close_socket() calls this on every fd
    SSL* ssl = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_reg_mutex);
        auto it = g_reg.find(sock);
        if (it == g_reg.end()) return;
        ssl = it->second;
        g_reg.erase(it);
    }
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
}

void* ssl_for(socket_t sock) {
    if (!g_enabled.load()) return nullptr;   // fast path: TLS off → no lookup cost beyond this
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    auto it = g_reg.find(sock);
    return it == g_reg.end() ? nullptr : static_cast<void*>(it->second);
}

long ssl_send(void* ssl, const void* data, size_t len) {
    return SSL_write(static_cast<SSL*>(ssl), data, static_cast<int>(len));
}
long ssl_recv(void* ssl, void* data, size_t len) {
    return SSL_read(static_cast<SSL*>(ssl), data, static_cast<int>(len));
}
long ssl_peek(void* ssl, void* data, size_t len) {
    return SSL_peek(static_cast<SSL*>(ssl), data, static_cast<int>(len));
}

} // namespace suco::tls
