/* sp_crypto.h -- Compact SHA-256 / HMAC / PBKDF2 / Base64URL / CSPRNG
 *
 * Pure C, no spinel-runtime dependency. Intended for spinel programs
 * that need a small in-tree crypto surface without dragging in
 * OpenSSL or libsodium. Sibling to sp_bigint.{h,c} in scope: a
 * vendored, audit-sized C helper that ships with spinel so apps
 * don't each reinvent it.
 *
 * The seven functions below are the canonical surface tep ended up
 * with after a year of building on the same primitives -- if you
 * find yourself wanting a different shape, please file an issue
 * before adding to this list. The point is to keep the surface
 * small enough to read in one sitting.
 *
 * Naming
 * ------
 * All exported symbols use the `sp_crypto_` prefix (matches the
 * `sp_bigint_` convention). State buffers are per-function statics
 * -- the next call to the same function clobbers the buffer, so
 * copy on the caller side if the value must outlive the next call.
 *
 * Inputs
 * ------
 * String inputs are NUL-terminated. Length is taken via strlen()
 * inside each function. For binary-safe inputs (containing embedded
 * NULs), use the explicit-length internal API exposed by including
 * this header in your own .c file -- see sp_crypto.c for the
 * declarations.
 *
 * Thread safety
 * -------------
 * The return buffers are SP_TLS, so an answer belongs to the thread
 * that asked for it. The clobber rule above still holds, per thread:
 * the next call to the same function on the SAME thread overwrites
 * it, and a caller that needs the value to outlive that call still
 * copies.
 *
 * They were process-global until #4174, whose SecureRandom draw made
 * the cost visible -- eight threads minting tokens got about a third
 * of them from another thread's buffer. Every function here has the
 * same window, because every call site copies out of the buffer after
 * the call has RETURNED. "Caller-side serialize" was not a contract
 * a caller could keep: Digest::SHA256.hexdigest is an ordinary Ruby
 * method with nothing at the call site to serialize against.
 *
 * SP_TLS is __thread only in the -DSP_THREADS runtime variant, so a
 * program that never threads pays nothing: its sp_crypto.o is the
 * byte-identical object it was. The threaded variant pays the buffers
 * per WORKER -- ~32 KiB of that is the two 16 KiB base64url buffers
 * and everything else together is under 600 bytes -- and the worker
 * pool is bounded by min(cores, SPINEL_WORKERS), not by the number of
 * Ruby threads the program starts.
 */
#ifndef SP_CRYPTO_H
#define SP_CRYPTO_H

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-1(msg) -> 40-char lowercase hex. Legacy hash, kept for
 * WebSocket handshake (RFC 6455 §1.3 requires SHA-1). Do NOT
 * use for new security designs; SHA-256 is the right primitive. */
const char *sp_crypto_sha1_hex(const char *msg);

/* SHA-256(msg) -> 64-char lowercase hex. */
const char *sp_crypto_sha256_hex(const char *msg);

/* SHA-384(msg) -> 96-char lowercase hex; SHA-512(msg) -> 128-char lowercase hex. */
const char *sp_crypto_sha384_hex(const char *msg);
const char *sp_crypto_sha512_hex(const char *msg);

/* Raw digest bytes: 32 for SHA-256, 48 for SHA-384, 64 for SHA-512, 20 for SHA-1.
 * The result is NOT NUL-terminated data -- it is arbitrary bytes -- so the length
 * is published in sp_ffi_bin_len (sp_alloc.h) for the `:cbinstr` / `:binstr` return
 * modes to pick up. Callers reading these directly must use the same length. */
const char *sp_crypto_sha256_bin(const char *msg);
const char *sp_crypto_sha384_bin(const char *msg);
const char *sp_crypto_sha512_bin(const char *msg);
const char *sp_crypto_sha1_bin(const char *msg);

/* MD5(msg) -> 32-char lowercase hex / the raw 16 digest bytes. Legacy hash,
 * bound for Digest::MD5 (Active Storage's direct-upload checksum). Do NOT
 * use for new security designs. */
const char *sp_crypto_md5_hex(const char *msg);
const char *sp_crypto_md5_bin(const char *msg);

/* Digest::X.base64digest: strict base64 of the raw digest (24, 28, 44, 64
 * and 88 chars, `=` padded). */
const char *sp_crypto_md5_b64(const char *msg);
const char *sp_crypto_sha1_b64(const char *msg);
const char *sp_crypto_sha256_b64(const char *msg);
const char *sp_crypto_sha384_b64(const char *msg);
const char *sp_crypto_sha512_b64(const char *msg);

/* Incremental hashing, for a digest object fed in pieces. The state is a
 * binary String of a fixed size (104, 92, 88, and 208 bytes for SHA-256,
 * SHA-1, MD5, and SHA-384/SHA-512): the chaining value, the byte count and
 * the partial block. An empty String is the initial state. `_update` answers
 * the state after `chunk`; `_final` answers the raw digest of what the state
 * has seen and leaves it usable. Both lengths ride sp_ffi_bin_len, as for
 * sp_crypto_sha256_bin. */
const char *sp_crypto_sha256_update(const char *state, const char *chunk);
const char *sp_crypto_sha256_final(const char *state);
const char *sp_crypto_sha384_update(const char *state, const char *chunk);
const char *sp_crypto_sha384_final(const char *state);
const char *sp_crypto_sha512_update(const char *state, const char *chunk);
const char *sp_crypto_sha512_final(const char *state);
const char *sp_crypto_sha1_update(const char *state, const char *chunk);
const char *sp_crypto_sha1_final(const char *state);
const char *sp_crypto_md5_update(const char *state, const char *chunk);
const char *sp_crypto_md5_final(const char *state);

/* Sec-WebSocket-Accept = base64(SHA-1(client_key + GUID)) per
 * RFC 6455 §1.3. Returns a 28-char string ending in `=`. The
 * only modern use case for SHA-1 in this codebase; sugars the
 * concat+sha1+base64 dance into one call. */
const char *sp_crypto_websocket_accept(const char *client_key);

/* HMAC-SHA256(key, msg) -> 64-char lowercase hex. */
const char *sp_crypto_hmac_sha256_hex(const char *key, const char *msg);

/* HMAC-SHA256(key, msg) -> 43-char unpadded base64url. */
const char *sp_crypto_hmac_sha256_b64url(const char *key, const char *msg);

/* HMAC-SHA256(key, msg) / HMAC-SHA1(key, msg) -> the raw digest bytes
 * (32 and 20). Same buffer contract as sp_crypto_sha256_bin above: the
 * result is not NUL-terminated, and its length rides sp_ffi_bin_len.
 *
 * The hex spellings beside these are the right primitive when a human or
 * a protocol header reads the answer. These are for when it feeds another
 * primitive: HKDF (RFC 5869) is HMAC over raw bytes twice, and hex-then-
 * decode would be a lossless detour with a chance to get the decode
 * wrong. */
const char *sp_crypto_hmac_sha256_bin(const char *key, const char *msg);
const char *sp_crypto_hmac_sha1_bin(const char *key, const char *msg);

/* HMAC-SHA1(key, msg) -> 40-char lowercase hex. Here for the same
 * reason sp_crypto_websocket_accept is -- an existing protocol names
 * SHA-1 and reproducing it is not a new security design. Rails signs
 * cookies with HMAC-SHA1 unless the app sets
 * config.action_dispatch.cookies_digest, so a program reading one has
 * no choice of digest. Do NOT reach for this when you do have a
 * choice; sp_crypto_hmac_sha256_hex is the right primitive. */
const char *sp_crypto_hmac_sha1_hex(const char *key, const char *msg);

/* Base64URL (RFC 4648 §5, no padding) encode/decode. Max input
 * length ~12 KiB (encode) / ~16 KiB (decode), bump the buffer in
 * sp_crypto.c if your callers need more. The decode answers bytes that
 * may hold a NUL and sets sp_ffi_bin_len to their count (0 on bad input):
 * bind it `:binstr`, as a `:str` return stops at the first NUL. */
const char *sp_crypto_b64url_encode(const char *src);
const char *sp_crypto_b64url_decode(const char *src);

/* PBKDF2-HMAC-SHA256(password, salt, iters) -> 43-char unpadded
 * base64url (32 bytes derived -- the one-block case). */
const char *sp_crypto_pbkdf2_sha256_b64url(const char *password, const char *salt, int iters);

/* Same, with an explicit derived length in BYTES (clamped to [1, 64]);
 * 64 bytes -> 86 unpadded b64url chars. Rails derives its signed-cookie
 * and signed-id keys at dkLen 64 (ActiveSupport::KeyGenerator#
 * generate_key's default key_size), so a spinel program that reads or
 * mints one needs the two-block form. */
const char *sp_crypto_pbkdf2_sha256_b64url_len(const char *password, const char *salt,
                                               int iters, int dklen);

/* Largest single draw the raw-bytes entry point below serves. Covers
 * every SecureRandom shape a program reaches for (a v4 uuid is 16, a
 * session token 24, a 256-bit key 32); a larger request is an
 * ArgumentError rather than a silently short answer. */
#define SPC_RANDOM_MAX 256

/* nbytes raw CSPRNG bytes, with the count published in sp_ffi_bin_len
 * for the `:cbinstr` return mode (random bytes contain NULs). Uses
 * arc4random_buf on BSD/macOS; on Linux/POSIX getrandom(2), then
 * /dev/urandom. RAISES when no secure source is available -- a caller
 * asking for random bytes is minting a token, and there is no answer
 * to give. The bundled `securerandom` package binds this. */
/* Fill `out` with `nbytes` of kernel-sourced entropy. 1 on success, 0 when
   there is no secure source -- callers fail closed rather than fall back to
   weak randomness. The one place that decides what counts as secure. */
int sp_crypto_entropy(unsigned char *out, int nbytes);
const char *sp_crypto_random_bin(int nbytes);

/* CSPRNG: nbytes random bytes (clamped to [1, 64]) as unpadded
 * base64url. Uses arc4random_buf on BSD/macOS; on Linux/POSIX
 * getrandom(2), then /dev/urandom. Returns NULL when no secure source
 * is available (fails closed -- never degrades to weak randomness). */
const char *sp_crypto_random_b64url(int nbytes);

#ifdef __cplusplus
}
#endif

#endif /* SP_CRYPTO_H */
