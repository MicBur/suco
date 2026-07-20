# SUCO Security — Shared-Secret Authentication

SUCO supports optional **authentication** of clients and workers against the coordinator using a
shared secret and an HMAC-SHA256 challenge-response handshake. It blocks unauthorized parties on
the network from submitting compile jobs, joining the grid as a worker (and thereby seeing source
payloads or serving poisoned objects), or otherwise participating.

## Scope — what it does and does not do

- **Does:** access control. Only parties that know `SUCO_SECRET` can use the grid. The secret is
  never sent over the wire; each connection answers a fresh random nonce with
  `HMAC-SHA256(secret, nonce)`, compared in constant time. Not bypassable via `SUCO_IGNORE_VERSION`.
- **Does NOT:** encrypt traffic. Preprocessed source and object files still travel the LAN in
  plaintext (visible to a passive eavesdropper). Confidentiality requires **TLS** (planned, separate).

Use auth when the network is semi-trusted (unauthorized *participation* is the concern). Add TLS on
top when the network itself is untrusted (eavesdropping is the concern).

## Enabling it (opt-in, backward compatible)

With `SUCO_SECRET` **unset**, behaviour is exactly as before (no auth) — so deploying auth-capable
binaries is safe and changes nothing until you set the secret.

**Requirement:** the same `SUCO_SECRET` must be set on the coordinator, every worker, and every
client. A mismatch → that peer is rejected and (for clients) falls back to local compilation; builds
never break, they just stop using the grid.

### Safe rollout (avoids locking yourself out of the grid)

1. **Deploy** the auth-capable binaries everywhere (coordinator, workers, clients) **without** setting
   `SUCO_SECRET`. The grid keeps working unchanged (auth inactive).
2. Pick a strong secret (e.g. `openssl rand -hex 32`), store it where each component reads its env.
3. **Set `SUCO_SECRET` on the workers and coordinator first**, restart them. They now require auth
   from each other; unauthenticated clients fall back to local (build still succeeds, just slower).
4. **Set `SUCO_SECRET` on the clients last.** Now the whole grid is authenticated.
5. Verify: coordinator log shows authenticated connections and no `AUTH FAILED`; a client/worker
   started **without** the secret is rejected (`AUTH FAILED`, falls back to local / registration fails).

To disable: unset `SUCO_SECRET` everywhere and restart.

## Implementation notes

- Crypto: `suco::hmac_sha256_hex`, `generate_nonce` (`RAND_bytes`), `constant_time_equals`
  (`CRYPTO_memcmp`), `get_shared_secret` in `src/common/hash_util.*` (OpenSSL::Crypto, already linked).
- Handshake: client `src/client/network_client.cpp` ↔ coordinator `src/coordinator/client_handler.cpp`;
  worker `src/worker/network_client.cpp` ↔ coordinator `src/coordinator/worker_handler.cpp`. Workers
  receive a 1-byte accept ACK so a rejected worker reports failure instead of looping on a false
  "registered" state.
- Verified: correct secret → grid compile + worker registers; wrong/absent secret → `AUTH FAILED`
  + fallback (rogue worker excluded from the grid); no secret → smoke test green (unchanged).
