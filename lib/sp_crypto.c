/* sp_crypto.c -- SHA-256 / HMAC-SHA256 / PBKDF2 / Base64URL / CSPRNG.
 *
 * See sp_crypto.h for the public surface. Pure C, no spinel-runtime
 * dependency -- archived into libspinel_rt.a alongside sp_bigint.o
 * so spinel programs can ffi_func against it (or, longer term, have
 * codegen pick up `Digest::SHA256.hexdigest`-shaped Ruby calls and
 * lower them to these primitives directly).
 *
 * Implementations
 * ---------------
 * - SHA-256: compact public-domain implementation; the canonical
 *   FIPS-180-4 reference (Wikipedia pseudocode + Go's crypto/sha256
 *   match this byte-for-byte on the standard test vectors). An x86-64
 *   CPU with the SHA extensions runs the same compression on them, asked
 *   at run time; every other CPU and build runs the C.
 * - HMAC-SHA256: RFC 2104 / RFC 4231 on top of the SHA-256 above.
 * - PBKDF2-HMAC-SHA256: RFC 8018, dkLen fixed at 32 (one block).
 * - Base64URL: RFC 4648 §5 (URL/filename-safe alphabet), no padding.
 * - CSPRNG: arc4random_buf on BSD/macOS; getrandom(2) then
 *   /dev/urandom on Linux, failing closed (NULL) with no weak fallback.
 *
 * State buffers
 * -------------
 * Each public function owns a per-function SP_TLS return buffer.
 * The next call to the SAME function on the SAME thread clobbers it.
 * Distinct functions use distinct buffers, so chained calls
 * (b64url_encode then hex) don't trample each other, and distinct
 * threads never share one -- the call site copies out of the buffer
 * after the call has returned, and that window is long enough for
 * another thread to enter the same function and overwrite it.
 */
#include "sp_crypto.h"
#include "sp_alloc.h"   /* sp_str_byte_len: hash/HMAC/PBKDF2 inputs are spinel
                           Strings that can carry embedded NULs (binary keys,
                           salts, digests), so use the header byte length rather
                           than strlen, which truncates at the first NUL (#1779). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(__linux__)
#include <unistd.h>       /* syscall */
#include <sys/syscall.h>  /* SYS_getrandom (the CSPRNG's first choice) */
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <stdlib.h>  /* arc4random_buf */
#endif
#if SP_HAVE_X86_SHA
#include <immintrin.h>    /* SHA-256 on the x86 SHA extensions */
#include <cpuid.h>        /* ... where the CPU says it has them */
#endif

/* ---------- SHA-256 ----------
 * FIPS-180-4. Compact reference implementation -- public domain.
 */

static const uint32_t sp_crypto_sha256_k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SPC_ROTR(x,n)    (((x) >> (n)) | ((x) << (32 - (n))))
#define SPC_S0(x)  (SPC_ROTR(x, 2) ^ SPC_ROTR(x,13) ^ SPC_ROTR(x,22))
#define SPC_S1(x)  (SPC_ROTR(x, 6) ^ SPC_ROTR(x,11) ^ SPC_ROTR(x,25))
#define SPC_s0(x)  (SPC_ROTR(x, 7) ^ SPC_ROTR(x,18) ^ ((x) >> 3))
#define SPC_s1(x)  (SPC_ROTR(x,17) ^ SPC_ROTR(x,19) ^ ((x) >> 10))
#define SPC_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SPC_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* One round: t1 into d, the new a into h. The caller passes the eight words
 * in the order the round sees them, so instead of moving every word one place
 * along after each round, eight calls in a row each name them one place on. */
static SP_INLINE void sp_crypto_sha256_round(uint32_t a, uint32_t b, uint32_t c, uint32_t *d,
                                             uint32_t e, uint32_t f, uint32_t g, uint32_t *h,
                                             uint32_t kw) {
    uint32_t t1 = *h + SPC_S1(e) + SPC_CH(e,f,g) + kw;
    *d += t1;
    *h = t1 + SPC_S0(a) + SPC_MAJ(a,b,c);
}

/* Message word i of a block's rounds 16 * j + i: the block's own for the first
 * sixteen, then each computed in place over the sixteen before it. */
static SP_INLINE uint32_t sp_crypto_sha256_word(uint32_t w[16], int j, int i) {
    if (j)
        w[i] += SPC_s1(w[(i + 14) & 15]) + w[(i + 9) & 15] + SPC_s0(w[(i + 1) & 15]);
    return w[i];
}

/* One block in portable C. Kept a function of its own: where the CPU lacks
 * the SHA extensions below, a build runs this as it did before they were
 * here, behind one test a call. Sixteen rounds are written out a pass, over
 * a schedule of sixteen words rather than sixty-four: gcc -O2 keeps a loop
 * of one round, with its eight moves and a 256-byte schedule in memory, two
 * to three times slower. */
static SP_NOINLINE void sp_crypto_sha256_block(uint32_t H[8], const uint8_t p[64]) {
    uint32_t w[16];
    uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
    int i, j;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) |  (uint32_t)p[i*4+3];
    }
    for (j = 0; j < 4; j++) {
        const uint32_t *k = sp_crypto_sha256_k + 16 * j;
        sp_crypto_sha256_round(a, b, c, &d, e, f, g, &h, k[0]  + sp_crypto_sha256_word(w, j, 0));
        sp_crypto_sha256_round(h, a, b, &c, d, e, f, &g, k[1]  + sp_crypto_sha256_word(w, j, 1));
        sp_crypto_sha256_round(g, h, a, &b, c, d, e, &f, k[2]  + sp_crypto_sha256_word(w, j, 2));
        sp_crypto_sha256_round(f, g, h, &a, b, c, d, &e, k[3]  + sp_crypto_sha256_word(w, j, 3));
        sp_crypto_sha256_round(e, f, g, &h, a, b, c, &d, k[4]  + sp_crypto_sha256_word(w, j, 4));
        sp_crypto_sha256_round(d, e, f, &g, h, a, b, &c, k[5]  + sp_crypto_sha256_word(w, j, 5));
        sp_crypto_sha256_round(c, d, e, &f, g, h, a, &b, k[6]  + sp_crypto_sha256_word(w, j, 6));
        sp_crypto_sha256_round(b, c, d, &e, f, g, h, &a, k[7]  + sp_crypto_sha256_word(w, j, 7));
        sp_crypto_sha256_round(a, b, c, &d, e, f, g, &h, k[8]  + sp_crypto_sha256_word(w, j, 8));
        sp_crypto_sha256_round(h, a, b, &c, d, e, f, &g, k[9]  + sp_crypto_sha256_word(w, j, 9));
        sp_crypto_sha256_round(g, h, a, &b, c, d, e, &f, k[10] + sp_crypto_sha256_word(w, j, 10));
        sp_crypto_sha256_round(f, g, h, &a, b, c, d, &e, k[11] + sp_crypto_sha256_word(w, j, 11));
        sp_crypto_sha256_round(e, f, g, &h, a, b, c, &d, k[12] + sp_crypto_sha256_word(w, j, 12));
        sp_crypto_sha256_round(d, e, f, &g, h, a, b, &c, k[13] + sp_crypto_sha256_word(w, j, 13));
        sp_crypto_sha256_round(c, d, e, &f, g, h, a, &b, k[14] + sp_crypto_sha256_word(w, j, 14));
        sp_crypto_sha256_round(b, c, d, &e, f, g, h, &a, k[15] + sp_crypto_sha256_word(w, j, 15));
    }
    H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d;
    H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
}

#if SP_HAVE_X86_SHA
/* The same compression on the x86 SHA extensions (FIPS-180-4 unchanged: one
 * instruction runs two rounds, two more extend the message schedule). The
 * state is kept as the two halves the round instruction takes, ABEF and
 * CDGH, and the schedule as four registers of four words. */

/* Four rounds: words w plus their four constants, two rounds a half. */
SP_TARGET_X86_SHA static SP_INLINE void sp_crypto_sha256_ni_rounds(__m128i *abef, __m128i *cdgh, __m128i w, int i) {
    __m128i k = _mm_add_epi32(w, _mm_loadu_si128((const __m128i *)&sp_crypto_sha256_k[4 * i]));
    *cdgh = _mm_sha256rnds2_epu32(*cdgh, *abef, k);
    *abef = _mm_sha256rnds2_epu32(*abef, *cdgh, _mm_shuffle_epi32(k, 0x0E));
}

/* The next four schedule words from the sixteen before them, oldest first. */
SP_TARGET_X86_SHA static SP_INLINE __m128i sp_crypto_sha256_ni_next(__m128i w4, __m128i w3, __m128i w2, __m128i w1) {
    __m128i w = _mm_add_epi32(_mm_sha256msg1_epu32(w4, w3), _mm_alignr_epi8(w1, w2, 4));
    return _mm_sha256msg2_epu32(w, w1);
}

/* n blocks at p into H. The state stays in its two registers from the first
 * block to the last. */
SP_TARGET_X86_SHA static void sp_crypto_sha256_blocks_ni(uint32_t H[8], const uint8_t *p, size_t n) {
    const __m128i be = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);  /* big-endian words */
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&H[0]), 0xB1);     /* CDAB */
    __m128i cdgh = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&H[4]), 0x1B);  /* EFGH */
    __m128i abef = _mm_alignr_epi8(t, cdgh, 8);
    cdgh = _mm_blend_epi16(cdgh, t, 0xF0);
    for (; n > 0; n--, p += 64) {
        __m128i abef0 = abef, cdgh0 = cdgh;
        __m128i w0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)p), be);
        __m128i w1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16)), be);
        __m128i w2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 32)), be);
        __m128i w3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 48)), be);
        sp_crypto_sha256_ni_rounds(&abef, &cdgh, w0, 0);
        sp_crypto_sha256_ni_rounds(&abef, &cdgh, w1, 1);
        sp_crypto_sha256_ni_rounds(&abef, &cdgh, w2, 2);
        sp_crypto_sha256_ni_rounds(&abef, &cdgh, w3, 3);
        for (int i = 4; i < 16; i += 4) {
            w0 = sp_crypto_sha256_ni_next(w0, w1, w2, w3);
            sp_crypto_sha256_ni_rounds(&abef, &cdgh, w0, i);
            w1 = sp_crypto_sha256_ni_next(w1, w2, w3, w0);
            sp_crypto_sha256_ni_rounds(&abef, &cdgh, w1, i + 1);
            w2 = sp_crypto_sha256_ni_next(w2, w3, w0, w1);
            sp_crypto_sha256_ni_rounds(&abef, &cdgh, w2, i + 2);
            w3 = sp_crypto_sha256_ni_next(w3, w0, w1, w2);
            sp_crypto_sha256_ni_rounds(&abef, &cdgh, w3, i + 3);
        }
        abef = _mm_add_epi32(abef, abef0);
        cdgh = _mm_add_epi32(cdgh, cdgh0);
    }
    t = _mm_shuffle_epi32(abef, 0x1B);        /* FEBA */
    cdgh = _mm_shuffle_epi32(cdgh, 0xB1);     /* DCHG */
    _mm_storeu_si128((__m128i *)&H[0], _mm_blend_epi16(t, cdgh, 0xF0));
    _mm_storeu_si128((__m128i *)&H[4], _mm_alignr_epi8(cdgh, t, 8));
}

/* Whether this CPU has the SHA extensions, and the SSSE3 and SSE4.1 the
 * function above is compiled with: CPUID leaf 7 EBX bit 29, leaf 1 ECX bits
 * 9 and 19. They use the SSE registers, which every x86-64 system saves, so
 * there is no operating-system side to ask. */
static SP_NOINLINE SP_COLD int sp_crypto_sha256_ni_probe(void) {
    unsigned int a, b, c, d;
    if (__get_cpuid_max(0, 0) < 7) return 0;
    __cpuid(1, a, b, c, d);
    if ((c & (1u << 9)) == 0 || (c & (1u << 19)) == 0) return 0;
    __cpuid_count(7, 0, a, b, c, d);
    return (int)((b >> 29) & 1);
}

/* The answer, asked once a process: 0 not asked yet, 1 yes, -1 no. */
static int sp_crypto_sha256_ni_state;

/* Whether to take the function above. Threads that ask at the same moment
 * store the same value. */
static SP_INLINE int sp_crypto_sha256_ni(void) {
    int s = SP_ATOMIC_LOAD(&sp_crypto_sha256_ni_state, __ATOMIC_RELAXED);
    if (SP_UNLIKELY(s == 0)) {
        s = sp_crypto_sha256_ni_probe() ? 1 : -1;
        SP_ATOMIC_STORE(&sp_crypto_sha256_ni_state, s, __ATOMIC_RELAXED);
    }
    return s > 0;
}
#endif

/* n whole blocks at p into H: on the SHA extensions where the CPU has them,
 * by the C above everywhere else. */
static void sp_crypto_sha256_blocks(uint32_t H[8], const uint8_t *p, size_t n) {
#if SP_HAVE_X86_SHA
    if (sp_crypto_sha256_ni()) {
        sp_crypto_sha256_blocks_ni(H, p, n);
        return;
    }
#endif
    for (; n > 0; n--, p += 64) sp_crypto_sha256_block(H, p);
}

static void sp_crypto_store_be32(uint8_t *out, const uint32_t *H, int n) {
    for (int i = 0; i < n; i++) {
        out[i*4]   = (uint8_t)(H[i] >> 24);
        out[i*4+1] = (uint8_t)(H[i] >> 16);
        out[i*4+2] = (uint8_t)(H[i] >> 8);
        out[i*4+3] = (uint8_t)(H[i]);
    }
}

/* SHA-256 of len bytes: the whole blocks straight from msg, then the padded
 * tail (one block, or two when the length field does not fit beside it). */
static void sp_crypto_sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t H[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t buf[64];
    size_t i, full = len & ~((size_t)63);
    if (full) sp_crypto_sha256_blocks(H, msg, full / 64);
    size_t rem = len - full;
    for (i = 0; i < rem; i++) buf[i] = msg[full + i];
    buf[rem] = 0x80;
    if (rem >= 56) {
        for (i = rem + 1; i < 64; i++) buf[i] = 0;
        sp_crypto_sha256_blocks(H, buf, 1);
        for (i = 0; i < 56; i++) buf[i] = 0;
    }
else {
        for (i = rem + 1; i < 56; i++) buf[i] = 0;
    }
    uint64_t bits = (uint64_t)len * 8;
    for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
    sp_crypto_sha256_blocks(H, buf, 1);
    sp_crypto_store_be32(out, H, 8);
}

/* ---------- SHA-1 ----------
 * FIPS-180-4 (legacy hash, kept for WebSocket handshake -- RFC 6455
 * requires SHA-1 specifically for Sec-WebSocket-Accept). Do not use
 * for new security designs; SHA-256 is the right primitive for those.
 * Compact reference implementation -- public domain.
 */

static void sp_crypto_sha1_block(uint32_t H[5], const uint8_t b[64]) {
    uint32_t w[80], a, sa, sb, sc, sd, se, f, k;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
               ((uint32_t)b[i*4+2] << 8) |  (uint32_t)b[i*4+3];
    }
    for (i = 16; i < 80; i++) {
        a = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (a << 1) | (a >> 31);
    }
    sa = H[0]; sb = H[1]; sc = H[2]; sd = H[3]; se = H[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (sb & sc) | (~sb & sd);         k = 0x5A827999; }
        else if (i < 40) { f = sb ^ sc ^ sd;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (sb & sc) | (sb & sd) | (sc & sd); k = 0x8F1BBCDC; }
        else             { f = sb ^ sc ^ sd;                   k = 0xCA62C1D6; }
        uint32_t t = ((sa << 5) | (sa >> 27)) + f + se + k + w[i];
        se = sd; sd = sc; sc = (sb << 30) | (sb >> 2); sb = sa; sa = t;
    }
    H[0] += sa; H[1] += sb; H[2] += sc; H[3] += sd; H[4] += se;
}

static void sp_crypto_sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t H[5] = {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
    };
    uint8_t buf[64];
    size_t i, full = len & ~((size_t)63);
    for (i = 0; i < full; i += 64) sp_crypto_sha1_block(H, msg + i);
    size_t rem = len - full;
    for (i = 0; i < rem; i++) buf[i] = msg[full + i];
    buf[rem] = 0x80;
    if (rem >= 56) {
        for (i = rem + 1; i < 64; i++) buf[i] = 0;
        sp_crypto_sha1_block(H, buf);
        for (i = 0; i < 56; i++) buf[i] = 0;
    }
else {
        for (i = rem + 1; i < 56; i++) buf[i] = 0;
    }
    uint64_t bits = (uint64_t)len * 8;
    for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
    sp_crypto_sha1_block(H, buf);
    sp_crypto_store_be32(out, H, 5);
}

static SP_TLS char sp_crypto_sha256_hex_buf[65];

/* SHA-256(msg) -> 64-char lowercase hex. The `digest` spin package binds
   Digest::SHA256.hexdigest to this. Same static-buffer contract as the
   sibling helpers. */
const char *sp_crypto_sha256_hex(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[32];
    sp_crypto_sha256((const uint8_t *)msg, sp_str_byte_len(msg), out);
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        sp_crypto_sha256_hex_buf[i*2]   = H[(out[i] >> 4) & 0xf];
        sp_crypto_sha256_hex_buf[i*2+1] = H[out[i] & 0xf];
    }
    sp_crypto_sha256_hex_buf[64] = '\0';
    return sp_crypto_sha256_hex_buf;
}

/* SHA-256(msg) -> the raw 32 digest bytes. Digest::SHA256.digest binds here
   through the native binding's `:cbinstr` return mode, which reads the exact
   length from sp_ffi_bin_len rather than calling strlen -- a binary digest
   contains NUL bytes roughly one time in eight. Same static-buffer contract as
   the hex siblings. */
static SP_TLS char sp_crypto_sha256_bin_buf[32];

const char *sp_crypto_sha256_bin(const char *msg) {SP_GC_ROOT_STR(msg);
    sp_crypto_sha256((const uint8_t *)msg, sp_str_byte_len(msg),
                     (uint8_t *)sp_crypto_sha256_bin_buf);
    sp_ffi_bin_len = 32;
    return sp_crypto_sha256_bin_buf;
}

static SP_TLS char sp_crypto_sha1_hex_buf[41];

const char *sp_crypto_sha1_hex(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[20];
    sp_crypto_sha1((const uint8_t *)msg, sp_str_byte_len(msg), out);
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 20; i++) {
        sp_crypto_sha1_hex_buf[i*2]   = H[(out[i] >> 4) & 0xf];
        sp_crypto_sha1_hex_buf[i*2+1] = H[out[i] & 0xf];
    }
    sp_crypto_sha1_hex_buf[40] = '\0';
    return sp_crypto_sha1_hex_buf;
}

/* SHA-1(msg) -> the raw 20 digest bytes; see sp_crypto_sha256_bin. */
static SP_TLS char sp_crypto_sha1_bin_buf[20];

const char *sp_crypto_sha1_bin(const char *msg) {SP_GC_ROOT_STR(msg);
    sp_crypto_sha1((const uint8_t *)msg, sp_str_byte_len(msg),
                   (uint8_t *)sp_crypto_sha1_bin_buf);
    sp_ffi_bin_len = 20;
    return sp_crypto_sha1_bin_buf;
}

/* ---------- MD5 (RFC 1321) ----------
 * Legacy hash, bound for Digest::MD5: Active Storage's direct-upload
 * protocol checks a blob by the base64 MD5 of its bytes (#4631). Do not use
 * for new security designs. Compact reference implementation, public domain. */

static const uint32_t sp_crypto_md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const uint8_t sp_crypto_md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void sp_crypto_md5_block(uint32_t H[4], const uint8_t b[64]) {
    uint32_t w[16], a, bb, c, d, f, t;
    int i, g;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[i*4] | ((uint32_t)b[i*4+1] << 8) |
               ((uint32_t)b[i*4+2] << 16) | ((uint32_t)b[i*4+3] << 24);
    }
    a = H[0]; bb = H[1]; c = H[2]; d = H[3];
    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5*i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3*i + 5) & 15; }
        else             { f = c ^ (bb | ~d);        g = (7*i) & 15; }
        t = d; d = c; c = bb;
        uint32_t x = a + f + sp_crypto_md5_k[i] + w[g];
        bb = bb + ((x << sp_crypto_md5_r[i]) | (x >> (32 - sp_crypto_md5_r[i])));
        a = t;
    }
    H[0] += a; H[1] += bb; H[2] += c; H[3] += d;
}

static void sp_crypto_md5(const uint8_t *msg, size_t len, uint8_t out[16]) {
    uint32_t H[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint8_t buf[64];
    size_t i, full = len & ~((size_t)63);
    for (i = 0; i < full; i += 64) sp_crypto_md5_block(H, msg + i);
    size_t rem = len - full;
    for (i = 0; i < rem; i++) buf[i] = msg[full + i];
    buf[rem] = 0x80;
    if (rem >= 56) {
        for (i = rem + 1; i < 64; i++) buf[i] = 0;
        sp_crypto_md5_block(H, buf);
        for (i = 0; i < 56; i++) buf[i] = 0;
    }
    else {
        for (i = rem + 1; i < 56; i++) buf[i] = 0;
    }
    uint64_t bits = (uint64_t)len * 8;   /* little-endian length, unlike SHA */
    for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (8*i));
    sp_crypto_md5_block(H, buf);
    for (i = 0; i < 4; i++) {
        out[i*4]   = (uint8_t)(H[i]);
        out[i*4+1] = (uint8_t)(H[i] >> 8);
        out[i*4+2] = (uint8_t)(H[i] >> 16);
        out[i*4+3] = (uint8_t)(H[i] >> 24);
    }
}

static SP_TLS char sp_crypto_md5_hex_buf[33];

const char *sp_crypto_md5_hex(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[16];
    sp_crypto_md5((const uint8_t *)msg, sp_str_byte_len(msg), out);
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        sp_crypto_md5_hex_buf[i*2]   = H[(out[i] >> 4) & 0xf];
        sp_crypto_md5_hex_buf[i*2+1] = H[out[i] & 0xf];
    }
    sp_crypto_md5_hex_buf[32] = '\0';
    return sp_crypto_md5_hex_buf;
}

/* MD5(msg) -> the raw 16 digest bytes; see sp_crypto_sha256_bin. */
static SP_TLS char sp_crypto_md5_bin_buf[16];

const char *sp_crypto_md5_bin(const char *msg) {SP_GC_ROOT_STR(msg);
    sp_crypto_md5((const uint8_t *)msg, sp_str_byte_len(msg),
                  (uint8_t *)sp_crypto_md5_bin_buf);
    sp_ffi_bin_len = 16;
    return sp_crypto_md5_bin_buf;
}

/* ---------- Incremental hashing ----------
 * The three hashes above, fed in pieces: the Init / Update / Final shape of
 * RFC 1321 (MD5Update) and RFC 6234 (SHA256Input), over the same block
 * functions and the same padding. A state is the chaining value, the byte
 * count (8 bytes) and the partial block (64 bytes), copied in and out of a
 * binary String, so a digest object keeps no more than that between calls.
 */

static void sp_crypto_sha1_blocks(uint32_t *H, const uint8_t *p, size_t n) {
    for (; n > 0; n--, p += 64) sp_crypto_sha1_block(H, p);
}

static void sp_crypto_md5_blocks(uint32_t *H, const uint8_t *p, size_t n) {
    for (; n > 0; n--, p += 64) sp_crypto_md5_block(H, p);
}

typedef struct {
    int words;            /* chaining words: 8, 5 or 4 */
    uint32_t iv[8];
    void (*blocks)(uint32_t *H, const uint8_t *p, size_t n);
    int big_endian;       /* SHA writes the length and the digest big-endian */
} sp_crypto_stream_alg;

static const sp_crypto_stream_alg sp_crypto_stream_sha256 = {
    8, { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 },
    sp_crypto_sha256_blocks, 1
};
static const sp_crypto_stream_alg sp_crypto_stream_sha1 = {
    5, { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 },
    sp_crypto_sha1_blocks, 1
};
static const sp_crypto_stream_alg sp_crypto_stream_md5 = {
    4, { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 },
    sp_crypto_md5_blocks, 0
};

#define SP_CRYPTO_STREAM_MAX (8 * 4 + 8 + 64)

static size_t sp_crypto_stream_size(const sp_crypto_stream_alg *a) {
    return (size_t)a->words * 4 + 8 + 64;
}

/* The state in `state` into H, the byte count and the partial block; a String
 * of any other size (the empty one) is the initial state. */
static void sp_crypto_stream_load(const sp_crypto_stream_alg *a, const char *state,
                                  uint32_t H[8], uint64_t *count, uint8_t pend[64]) {
    size_t hw = (size_t)a->words * 4;
    if (sp_str_byte_len(state) == sp_crypto_stream_size(a)) {
        memcpy(H, state, hw);
        memcpy(count, state + hw, 8);
        memcpy(pend, state + hw + 8, 64);
    }
    else {
        memcpy(H, a->iv, hw);
        *count = 0;
        memset(pend, 0, 64);
    }
}

static void sp_crypto_stream_store(const sp_crypto_stream_alg *a, char *out,
                                   const uint32_t H[8], uint64_t count,
                                   const uint8_t pend[64]) {
    size_t hw = (size_t)a->words * 4;
    memcpy(out, H, hw);
    memcpy(out + hw, &count, 8);
    memcpy(out + hw + 8, pend, 64);
    sp_ffi_bin_len = (int)sp_crypto_stream_size(a);
}

/* `chunk` into the state: the partial block is filled first, the whole blocks
 * are hashed straight from the chunk, and the rest is the new partial block. */
static void sp_crypto_stream_update(const sp_crypto_stream_alg *a, const char *state,
                                    const char *chunk, char *out) {
    uint32_t H[8];
    uint64_t count;
    uint8_t pend[64];
    sp_crypto_stream_load(a, state, H, &count, pend);
    const uint8_t *p = (const uint8_t *)chunk;
    size_t len = sp_str_byte_len(chunk);
    size_t have = (size_t)(count % 64);
    count += len;
    if (have) {
        size_t take = 64 - have < len ? 64 - have : len;
        memcpy(pend + have, p, take);
        p += take;
        len -= take;
        if (have + take < 64) {
            sp_crypto_stream_store(a, out, H, count, pend);
            return;
        }
        a->blocks(H, pend, 1);
    }
    size_t full = len & ~((size_t)63);
    if (full) a->blocks(H, p, full / 64);
    if (len > full) memcpy(pend, p + full, len - full);
    sp_crypto_stream_store(a, out, H, count, pend);
}

/* The digest of what the state has seen, its partial block padded as the
 * one-shot functions pad their tail. The state does not change. */
static void sp_crypto_stream_final(const sp_crypto_stream_alg *a, const char *state,
                                   uint8_t *out) {
    uint32_t H[8];
    uint64_t count;
    uint8_t buf[64];
    sp_crypto_stream_load(a, state, H, &count, buf);
    size_t i, rem = (size_t)(count % 64);
    buf[rem] = 0x80;
    if (rem >= 56) {
        for (i = rem + 1; i < 64; i++) buf[i] = 0;
        a->blocks(H, buf, 1);
        for (i = 0; i < 56; i++) buf[i] = 0;
    }
    else {
        for (i = rem + 1; i < 56; i++) buf[i] = 0;
    }
    uint64_t bits = count * 8;
    for (i = 0; i < 8; i++)
        buf[56 + i] = (uint8_t)(a->big_endian ? bits >> (56 - 8*i) : bits >> (8*i));
    a->blocks(H, buf, 1);
    if (a->big_endian) {
        sp_crypto_store_be32(out, H, a->words);
        return;
    }
    for (i = 0; i < (size_t)a->words; i++) {
        out[i*4]   = (uint8_t)(H[i]);
        out[i*4+1] = (uint8_t)(H[i] >> 8);
        out[i*4+2] = (uint8_t)(H[i] >> 16);
        out[i*4+3] = (uint8_t)(H[i] >> 24);
    }
}

static SP_TLS char sp_crypto_sha256_state_buf[SP_CRYPTO_STREAM_MAX];
static SP_TLS char sp_crypto_sha256_final_buf[32];

const char *sp_crypto_sha256_update(const char *state, const char *chunk) {SP_GC_ROOT_STR(state);SP_GC_ROOT_STR(chunk);
    sp_crypto_stream_update(&sp_crypto_stream_sha256, state, chunk, sp_crypto_sha256_state_buf);
    return sp_crypto_sha256_state_buf;
}

const char *sp_crypto_sha256_final(const char *state) {SP_GC_ROOT_STR(state);
    sp_crypto_stream_final(&sp_crypto_stream_sha256, state, (uint8_t *)sp_crypto_sha256_final_buf);
    sp_ffi_bin_len = 32;
    return sp_crypto_sha256_final_buf;
}

static SP_TLS char sp_crypto_sha1_state_buf[SP_CRYPTO_STREAM_MAX];
static SP_TLS char sp_crypto_sha1_final_buf[20];

const char *sp_crypto_sha1_update(const char *state, const char *chunk) {SP_GC_ROOT_STR(state);SP_GC_ROOT_STR(chunk);
    sp_crypto_stream_update(&sp_crypto_stream_sha1, state, chunk, sp_crypto_sha1_state_buf);
    return sp_crypto_sha1_state_buf;
}

const char *sp_crypto_sha1_final(const char *state) {SP_GC_ROOT_STR(state);
    sp_crypto_stream_final(&sp_crypto_stream_sha1, state, (uint8_t *)sp_crypto_sha1_final_buf);
    sp_ffi_bin_len = 20;
    return sp_crypto_sha1_final_buf;
}

static SP_TLS char sp_crypto_md5_state_buf[SP_CRYPTO_STREAM_MAX];
static SP_TLS char sp_crypto_md5_final_buf[16];

const char *sp_crypto_md5_update(const char *state, const char *chunk) {SP_GC_ROOT_STR(state);SP_GC_ROOT_STR(chunk);
    sp_crypto_stream_update(&sp_crypto_stream_md5, state, chunk, sp_crypto_md5_state_buf);
    return sp_crypto_md5_state_buf;
}

const char *sp_crypto_md5_final(const char *state) {SP_GC_ROOT_STR(state);
    sp_crypto_stream_final(&sp_crypto_stream_md5, state, (uint8_t *)sp_crypto_md5_final_buf);
    sp_ffi_bin_len = 16;
    return sp_crypto_md5_final_buf;
}

/* RFC 6455 §1.3 Sec-WebSocket-Accept:
 *     base64(SHA-1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 * Standard Base64 (RFC 4648 §4, `+/` alphabet, `=` padding) -- NOT
 * the URL-safe variant. 20 bytes of SHA-1 -> 28-char output ("xxx=").
 * The GUID is the WebSocket protocol's fixed magic string.
 */
static const char SPC_B64[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static SP_TLS char sp_crypto_websocket_accept_buf[29];
static void sp_crypto_b64_strict(const uint8_t *d, size_t n, char *out);

const char *sp_crypto_websocket_accept(const char *client_key) {
    /* client_key is the 24-char base64 from the request header;
     * GUID is 36 chars. Total <= 60; cap input at 128 for safety. */
    char in[128 + 36 + 1];
    size_t klen = strlen(client_key);
    if (klen > 128) klen = 128;
    size_t i;
    for (i = 0; i < klen; i++) in[i] = client_key[i];
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    for (i = 0; i < 36; i++) in[klen + i] = guid[i];
    uint8_t digest[20];
    sp_crypto_sha1((const uint8_t *)in, klen + 36, digest);
    /* base64(20 bytes) = 28 chars: 6 full triplets + 2 leftover bytes
     * -> 3 chars + 1 padding `=`. */
    sp_crypto_b64_strict(digest, 20, sp_crypto_websocket_accept_buf);
    return sp_crypto_websocket_accept_buf;
}

/* Digest::X.base64digest: strict base64 (RFC 4648 §4, `=` padded) of the
   raw digest, which Ruby's Digest::Class defines for every digest. One
   buffer per digest so the three can be held at once. */
static void sp_crypto_b64_strict(const uint8_t *d, size_t n, char *out) {
    size_t i; int j = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i+1] << 8) | (uint32_t)d[i+2];
        out[j++] = SPC_B64[(v >> 18) & 0x3f];
        out[j++] = SPC_B64[(v >> 12) & 0x3f];
        out[j++] = SPC_B64[(v >>  6) & 0x3f];
        out[j++] = SPC_B64[ v        & 0x3f];
    }
    if (n - i == 2) {
        uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i+1] << 8);
        out[j++] = SPC_B64[(v >> 18) & 0x3f];
        out[j++] = SPC_B64[(v >> 12) & 0x3f];
        out[j++] = SPC_B64[(v >>  6) & 0x3f];
        out[j++] = '=';
    }
    else if (n - i == 1) {
        uint32_t v = ((uint32_t)d[i] << 16);
        out[j++] = SPC_B64[(v >> 18) & 0x3f];
        out[j++] = SPC_B64[(v >> 12) & 0x3f];
        out[j++] = '=';
        out[j++] = '=';
    }
    out[j] = '\0';
}

static SP_TLS char sp_crypto_md5_b64_buf[25];
static SP_TLS char sp_crypto_sha1_b64_buf[29];
static SP_TLS char sp_crypto_sha256_b64_buf[45];

const char *sp_crypto_md5_b64(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[16];
    sp_crypto_md5((const uint8_t *)msg, sp_str_byte_len(msg), out);
    sp_crypto_b64_strict(out, 16, sp_crypto_md5_b64_buf);
    return sp_crypto_md5_b64_buf;
}
const char *sp_crypto_sha1_b64(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[20];
    sp_crypto_sha1((const uint8_t *)msg, sp_str_byte_len(msg), out);
    sp_crypto_b64_strict(out, 20, sp_crypto_sha1_b64_buf);
    return sp_crypto_sha1_b64_buf;
}
const char *sp_crypto_sha256_b64(const char *msg) {SP_GC_ROOT_STR(msg);
    uint8_t out[32];
    sp_crypto_sha256((const uint8_t *)msg, sp_str_byte_len(msg), out);
    sp_crypto_b64_strict(out, 32, sp_crypto_sha256_b64_buf);
    return sp_crypto_sha256_b64_buf;
}

/* ---------- HMAC-SHA256 (RFC 2104) ---------- */

/* HMAC-SHA256 of msg under key: the inner hash over the padded key and the
 * message, the outer one over the padded key and that digest. */
static void sp_crypto_hmac_sha256(const uint8_t *key, size_t klen,
                                  const uint8_t *msg, size_t mlen,
                                  uint8_t out[32]) {
    uint8_t kpad[64], ipad[64], opad[64], inner[32];
    size_t i;
    if (klen > 64) {
        sp_crypto_sha256(key, klen, kpad);
        for (i = 32; i < 64; i++) kpad[i] = 0;
    }
else {
        for (i = 0; i < klen; i++) kpad[i] = key[i];
        for (i = klen; i < 64; i++) kpad[i] = 0;
    }
    for (i = 0; i < 64; i++) {
        ipad[i] = kpad[i] ^ 0x36;
        opad[i] = kpad[i] ^ 0x5c;
    }
    /* inner = SHA256(ipad || msg). Stream both segments through the
     * compression to avoid allocating ipad||msg. */
    {
        uint32_t H[8] = {
            0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
        };
        sp_crypto_sha256_blocks(H, ipad, 1);
        uint8_t buf[64];
        size_t full = mlen & ~((size_t)63);
        if (full) sp_crypto_sha256_blocks(H, msg, full / 64);
        size_t rem = mlen - full;
        for (i = 0; i < rem; i++) buf[i] = msg[full + i];
        buf[rem] = 0x80;
        if (rem >= 56) {
            for (i = rem + 1; i < 64; i++) buf[i] = 0;
            sp_crypto_sha256_blocks(H, buf, 1);
            for (i = 0; i < 56; i++) buf[i] = 0;
        }
else {
            for (i = rem + 1; i < 56; i++) buf[i] = 0;
        }
        uint64_t bits = (uint64_t)(64 + mlen) * 8;
        for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
        sp_crypto_sha256_blocks(H, buf, 1);
        for (i = 0; i < 8; i++) {
            inner[i*4]   = (uint8_t)(H[i] >> 24);
            inner[i*4+1] = (uint8_t)(H[i] >> 16);
            inner[i*4+2] = (uint8_t)(H[i] >> 8);
            inner[i*4+3] = (uint8_t)(H[i]);
        }
    }
    /* outer = SHA256(opad || inner) */
    {
        uint32_t H[8] = {
            0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
        };
        sp_crypto_sha256_blocks(H, opad, 1);
        uint8_t buf[64];
        for (i = 0; i < 32; i++) buf[i] = inner[i];
        buf[32] = 0x80;
        for (i = 33; i < 56; i++) buf[i] = 0;
        uint64_t bits = (uint64_t)(64 + 32) * 8;
        for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
        sp_crypto_sha256_blocks(H, buf, 1);
        sp_crypto_store_be32(out, H, 8);
    }
}

static SP_TLS char sp_crypto_hmac_hex_buf[65];

const char *sp_crypto_hmac_sha256_hex(const char *key, const char *msg) {SP_GC_ROOT_STR(key);SP_GC_ROOT_STR(msg);
    uint8_t out[32];
    sp_crypto_hmac_sha256((const uint8_t *)key, sp_str_byte_len(key),
                          (const uint8_t *)msg, sp_str_byte_len(msg),
                          out);
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        sp_crypto_hmac_hex_buf[i*2]   = H[(out[i] >> 4) & 0xf];
        sp_crypto_hmac_hex_buf[i*2+1] = H[out[i] & 0xf];
    }
    sp_crypto_hmac_hex_buf[64] = '\0';
    return sp_crypto_hmac_hex_buf;
}

/* The same MAC as its own bytes rather than as hex. Its own buffer, per
 * the file header's rule: a caller that hexes one MAC and takes the raw
 * bytes of another must not have the second clobber the first. */
static SP_TLS char sp_crypto_hmac_sha256_bin_buf[32];

const char *sp_crypto_hmac_sha256_bin(const char *key, const char *msg) {SP_GC_ROOT_STR(key);SP_GC_ROOT_STR(msg);
    sp_crypto_hmac_sha256((const uint8_t *)key, sp_str_byte_len(key),
                          (const uint8_t *)msg, sp_str_byte_len(msg),
                          (uint8_t *)sp_crypto_hmac_sha256_bin_buf);
    sp_ffi_bin_len = 32;
    return sp_crypto_hmac_sha256_bin_buf;
}

/* HMAC-SHA1 (RFC 2104). SHA-1 is here for the same reason
 * sp_crypto_websocket_accept is: an existing protocol names it, and
 * reproducing that protocol is not a new security design. Rails signs
 * its cookies with HMAC-SHA1 unless the app overrides
 * config.action_dispatch.cookies_digest, so a spinel program that reads
 * one has no choice of digest. Same streaming structure as the SHA-256
 * sibling above -- ipad/opad are compressed straight in rather than
 * concatenated with the message. */
static void sp_crypto_hmac_sha1(const uint8_t *key, size_t klen,
                                const uint8_t *msg, size_t mlen,
                                uint8_t out[20]) {
    uint8_t kpad[64], ipad[64], opad[64], inner[20];
    size_t i;
    if (klen > 64) {
        sp_crypto_sha1(key, klen, kpad);
        for (i = 20; i < 64; i++) kpad[i] = 0;
    }
else {
        for (i = 0; i < klen; i++) kpad[i] = key[i];
        for (i = klen; i < 64; i++) kpad[i] = 0;
    }
    for (i = 0; i < 64; i++) {
        ipad[i] = kpad[i] ^ 0x36;
        opad[i] = kpad[i] ^ 0x5c;
    }
    /* inner = SHA1(ipad || msg) */
    {
        uint32_t H[5] = {
            0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0
        };
        sp_crypto_sha1_block(H, ipad);
        uint8_t buf[64];
        size_t full = mlen & ~((size_t)63);
        for (i = 0; i < full; i += 64) sp_crypto_sha1_block(H, msg + i);
        size_t rem = mlen - full;
        for (i = 0; i < rem; i++) buf[i] = msg[full + i];
        buf[rem] = 0x80;
        if (rem >= 56) {
            for (i = rem + 1; i < 64; i++) buf[i] = 0;
            sp_crypto_sha1_block(H, buf);
            for (i = 0; i < 56; i++) buf[i] = 0;
        }
else {
            for (i = rem + 1; i < 56; i++) buf[i] = 0;
        }
        uint64_t bits = (uint64_t)(64 + mlen) * 8;
        for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
        sp_crypto_sha1_block(H, buf);
        for (i = 0; i < 5; i++) {
            inner[i*4]   = (uint8_t)(H[i] >> 24);
            inner[i*4+1] = (uint8_t)(H[i] >> 16);
            inner[i*4+2] = (uint8_t)(H[i] >> 8);
            inner[i*4+3] = (uint8_t)(H[i]);
        }
    }
    /* outer = SHA1(opad || inner) */
    {
        uint32_t H[5] = {
            0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0
        };
        sp_crypto_sha1_block(H, opad);
        uint8_t buf[64];
        for (i = 0; i < 20; i++) buf[i] = inner[i];
        buf[20] = 0x80;
        for (i = 21; i < 56; i++) buf[i] = 0;
        uint64_t bits = (uint64_t)(64 + 20) * 8;
        for (i = 0; i < 8; i++) buf[56 + i] = (uint8_t)(bits >> (56 - 8*i));
        sp_crypto_sha1_block(H, buf);
        sp_crypto_store_be32(out, H, 5);
    }
}

static SP_TLS char sp_crypto_hmac_sha1_hex_buf[41];

const char *sp_crypto_hmac_sha1_hex(const char *key, const char *msg) {SP_GC_ROOT_STR(key);SP_GC_ROOT_STR(msg);
    uint8_t out[20];
    sp_crypto_hmac_sha1((const uint8_t *)key, sp_str_byte_len(key),
                        (const uint8_t *)msg, sp_str_byte_len(msg),
                        out);
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 20; i++) {
        sp_crypto_hmac_sha1_hex_buf[i*2]   = H[(out[i] >> 4) & 0xf];
        sp_crypto_hmac_sha1_hex_buf[i*2+1] = H[out[i] & 0xf];
    }
    sp_crypto_hmac_sha1_hex_buf[40] = '\0';
    return sp_crypto_hmac_sha1_hex_buf;
}

/* HMAC-SHA1 as its own bytes; see the SHA-256 sibling. */
static SP_TLS char sp_crypto_hmac_sha1_bin_buf[20];

const char *sp_crypto_hmac_sha1_bin(const char *key, const char *msg) {SP_GC_ROOT_STR(key);SP_GC_ROOT_STR(msg);
    sp_crypto_hmac_sha1((const uint8_t *)key, sp_str_byte_len(key),
                        (const uint8_t *)msg, sp_str_byte_len(msg),
                        (uint8_t *)sp_crypto_hmac_sha1_bin_buf);
    sp_ffi_bin_len = 20;
    return sp_crypto_hmac_sha1_bin_buf;
}

/* ---------- Base64URL (RFC 4648 §5) ---------- */

static const char SPC_B64U[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static SP_TLS char sp_crypto_hmac_b64url_buf[44];
static void sp_crypto_b64url_bytes(const uint8_t *src, int n, char *out);

const char *sp_crypto_hmac_sha256_b64url(const char *key, const char *msg) {SP_GC_ROOT_STR(key);SP_GC_ROOT_STR(msg);
    uint8_t out[32];
    sp_crypto_hmac_sha256((const uint8_t *)key, sp_str_byte_len(key),
                          (const uint8_t *)msg, sp_str_byte_len(msg),
                          out);
    sp_crypto_b64url_bytes(out, 32, sp_crypto_hmac_b64url_buf);
    return sp_crypto_hmac_b64url_buf;
}

#define SPC_B64U_BUFSIZE (16 * 1024)
static SP_TLS char sp_crypto_b64url_buf[SPC_B64U_BUFSIZE];

const char *sp_crypto_b64url_encode(const char *src) {
    size_t n = sp_str_byte_len(src);
    size_t i = 0, j = 0;
    if (4 * ((n + 2) / 3) + 1 > SPC_B64U_BUFSIZE) {
        sp_crypto_b64url_buf[0] = '\0';
        return sp_crypto_b64url_buf;
    }
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)(uint8_t)src[i]   << 16)
                   | ((uint32_t)(uint8_t)src[i+1] << 8)
                   |  (uint32_t)(uint8_t)src[i+2];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 18) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 12) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >>  6) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[ v        & 0x3f];
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)(uint8_t)src[i] << 16;
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 18) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 12) & 0x3f];
    }
else if (rem == 2) {
        uint32_t v = ((uint32_t)(uint8_t)src[i]   << 16)
                   | ((uint32_t)(uint8_t)src[i+1] << 8);
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 18) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >> 12) & 0x3f];
        sp_crypto_b64url_buf[j++] = SPC_B64U[(v >>  6) & 0x3f];
    }
    sp_crypto_b64url_buf[j] = '\0';
    return sp_crypto_b64url_buf;
}

static SP_TLS char sp_crypto_b64u_dec_buf[SPC_B64U_BUFSIZE];

static int sp_crypto_b64u_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

const char *sp_crypto_b64url_decode(const char *src) {SP_GC_ROOT_STR(src);
    size_t n = strlen(src);
    while (n > 0 && src[n - 1] == '=') n--;  /* tolerate padded input */
    size_t i = 0, j = 0;
    if (n / 4 * 3 + 3 > SPC_B64U_BUFSIZE) {
        sp_crypto_b64u_dec_buf[0] = '\0'; sp_ffi_bin_len = 0;
        return sp_crypto_b64u_dec_buf;
    }
    while (i + 4 <= n) {
        int a = sp_crypto_b64u_val(src[i]);
        int b = sp_crypto_b64u_val(src[i+1]);
        int c = sp_crypto_b64u_val(src[i+2]);
        int d = sp_crypto_b64u_val(src[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            sp_crypto_b64u_dec_buf[0] = '\0'; sp_ffi_bin_len = 0;
            return sp_crypto_b64u_dec_buf;
        }
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12
                   | (uint32_t)c <<  6 | (uint32_t)d;
        sp_crypto_b64u_dec_buf[j++] = (v >> 16) & 0xff;
        sp_crypto_b64u_dec_buf[j++] = (v >>  8) & 0xff;
        sp_crypto_b64u_dec_buf[j++] =  v        & 0xff;
        i += 4;
    }
    size_t rem = n - i;
    if (rem == 2) {
        int a = sp_crypto_b64u_val(src[i]);
        int b = sp_crypto_b64u_val(src[i+1]);
        if (a < 0 || b < 0) { sp_crypto_b64u_dec_buf[0] = '\0'; sp_ffi_bin_len = 0; return sp_crypto_b64u_dec_buf; }
        sp_crypto_b64u_dec_buf[j++] = (a << 2) | (b >> 4);
    }
else if (rem == 3) {
        int a = sp_crypto_b64u_val(src[i]);
        int b = sp_crypto_b64u_val(src[i+1]);
        int c = sp_crypto_b64u_val(src[i+2]);
        if (a < 0 || b < 0 || c < 0) { sp_crypto_b64u_dec_buf[0] = '\0'; sp_ffi_bin_len = 0; return sp_crypto_b64u_dec_buf; }
        sp_crypto_b64u_dec_buf[j++] = (a << 2) | (b >> 4);
        sp_crypto_b64u_dec_buf[j++] = ((b & 0xf) << 4) | (c >> 2);
    }
    sp_crypto_b64u_dec_buf[j] = '\0';
    sp_ffi_bin_len = (int)j;   /* the decoded bytes may hold a NUL: bind it `:binstr` */
    return sp_crypto_b64u_dec_buf;
}

/* ---------- PBKDF2-HMAC-SHA256 (RFC 8018) ----------
 * dkLen up to 64 bytes (two HMAC-SHA256 output blocks).
 */

/* 64 derived bytes -> 86 unpadded b64url chars + NUL. */
static SP_TLS char sp_crypto_pbkdf2_b64url_buf[88];

/* Unpadded base64url of `n` bytes into `out` (NUL-terminated). */
static void sp_crypto_b64url_bytes(const uint8_t *src, int n, char *out) {
    int i, j = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16)
                   | ((uint32_t)src[i+1] << 8)
                   | (uint32_t)src[i+2];
        out[j++] = SPC_B64U[(v >> 18) & 0x3f];
        out[j++] = SPC_B64U[(v >> 12) & 0x3f];
        out[j++] = SPC_B64U[(v >> 6)  & 0x3f];
        out[j++] = SPC_B64U[v & 0x3f];
    }
    if (i < n) {
        uint32_t v = ((uint32_t)src[i] << 16)
                   | (i + 1 < n ? ((uint32_t)src[i+1] << 8) : 0);
        out[j++] = SPC_B64U[(v >> 18) & 0x3f];
        out[j++] = SPC_B64U[(v >> 12) & 0x3f];
        if (i + 1 < n) {
            out[j++] = SPC_B64U[(v >> 6) & 0x3f];
        }
    }
    out[j] = '\0';
}

const char *sp_crypto_pbkdf2_sha256_b64url_len(const char *password, const char *salt,
                                               int iters, int dklen) {SP_GC_ROOT_STR(password);
    if (iters < 1) iters = 1;
    if (dklen < 1) dklen = 32;
    if (dklen > 64) dklen = 64;
    size_t plen = sp_str_byte_len(password);
    size_t slen = sp_str_byte_len(salt);
    uint8_t salted[256];
    if (slen + 4 > sizeof(salted)) {
        sp_crypto_pbkdf2_b64url_buf[0] = '\0';
        return sp_crypto_pbkdf2_b64url_buf;
    }
    memcpy(salted, salt, slen);
    /* PBKDF2 (RFC 8018 sec 5.2): DK = T(1) || T(2) || ... , each block
       T(i) = F(password, salt, iters, i) and 32 bytes wide for SHA-256.
       Blocks are independent -- only the INT(i) suffix on the salt
       changes -- so one loop covers any dkLen. */
    uint8_t dk[64];
    int block, done = 0;
    for (block = 1; done < dklen; block++) {
        uint8_t U[32], T[32];
        int b, it, take;
        salted[slen+0] = (uint8_t)((block >> 24) & 0xff);
        salted[slen+1] = (uint8_t)((block >> 16) & 0xff);
        salted[slen+2] = (uint8_t)((block >> 8) & 0xff);
        salted[slen+3] = (uint8_t)(block & 0xff);
        sp_crypto_hmac_sha256((const uint8_t *)password, plen, salted, slen + 4, U);
        memcpy(T, U, 32);
        for (it = 1; it < iters; it++) {
            sp_crypto_hmac_sha256((const uint8_t *)password, plen, U, 32, U);
            for (b = 0; b < 32; b++) T[b] ^= U[b];
        }
        take = (dklen - done < 32) ? (dklen - done) : 32;
        memcpy(dk + done, T, take);
        done += take;
    }
    sp_crypto_b64url_bytes(dk, dklen, sp_crypto_pbkdf2_b64url_buf);
    return sp_crypto_pbkdf2_b64url_buf;
}

const char *sp_crypto_pbkdf2_sha256_b64url(const char *password, const char *salt, int iters) {
    return sp_crypto_pbkdf2_sha256_b64url_len(password, salt, iters, 32);
}

/* ---------- CSPRNG ---------- */

static SP_TLS char sp_crypto_random_b64url_buf[90];

/* Fill `out` with `nbytes` CSPRNG bytes. 1 on success, 0 when no secure
   source is available -- the ONE place the entropy decision is made, so the
   raw-bytes and base64url entry points below cannot drift apart about what
   counts as secure. */
int sp_crypto_entropy(uint8_t *out, int nbytes) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, nbytes);
    return 1;
#elif defined(__wasi__) || defined(_WIN32)
    /* the host's entropy, through WASI's random_get or (Windows, lib/win32)
       BCryptGenRandom; there is no device */
    return getentropy(out, (size_t)nbytes) == 0;
#else
    /* getrandom(2) first: kernel-sourced, works where /dev/urandom is not
       even visible (chroot, minimal containers) -- the very environments
       whose old time-based fallback made "CSPRNG" a lie. Then the device;
       then fail CLOSED, never weak randomness. */
#if defined(__linux__) && defined(SYS_getrandom)
    {
        long n = syscall(SYS_getrandom, out, (size_t)nbytes, 0);
        if (n == (long)nbytes) return 1;
    }
#endif
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) return 0;
        size_t got = fread(out, 1, (size_t)nbytes, f);
        fclose(f);
        if (got != (size_t)nbytes) return 0;
    }
    return 1;
#endif
}

/* nbytes raw CSPRNG bytes. SecureRandom's one primitive: every other method
   in the bundled `securerandom` package is a rendering of these bytes, so
   this is the only place that has to be right about entropy.

   RAISES rather than returning NULL when no secure source is available.
   The b64url sibling below predates it and keeps its NULL contract, but a
   caller reaching for random BYTES is minting a token or a key -- there is
   no answer to give, and a nil flowing into a token is the failure nothing
   notices. Length rides sp_ffi_bin_len for the `:cbinstr` return mode:
   random bytes contain NULs about one time in 256.

   SP_TLS, unlike its siblings in this file. The `:cbinstr` call site copies
   the returned pointer AFTER the call, so under -DSP_THREADS a second worker
   drawing in that window would overwrite the first one's bytes -- and here
   that means two sessions handed the same token, which is the one failure
   mode that never shows up as a crash. sp_ffi_bin_len, which carries this
   buffer's length, is already SP_TLS: a value and its length in different
   storage classes cannot both be right. sp_random.c keeps its generator
   state per-worker for the same reason. Costs SPC_RANDOM_MAX bytes per
   thread. */
static SP_TLS char sp_crypto_random_bin_buf[SPC_RANDOM_MAX];

const char *sp_crypto_random_bin(int nbytes) {
    if (nbytes < 0) sp_raise_cls("ArgumentError", "negative string size");
    if (nbytes > SPC_RANDOM_MAX)
        sp_raise_cls("ArgumentError",
                     "securerandom: request exceeds the bundled maximum draw");
    if (nbytes == 0) { sp_ffi_bin_len = 0; sp_crypto_random_bin_buf[0] = '\0'; return sp_crypto_random_bin_buf; }
    if (!sp_crypto_entropy((uint8_t *)sp_crypto_random_bin_buf, nbytes))
        sp_raise_cls("RuntimeError", "securerandom: no secure random source available");
    sp_ffi_bin_len = nbytes;
    return sp_crypto_random_bin_buf;
}

const char *sp_crypto_random_b64url(int nbytes) {
    if (nbytes < 1) nbytes = 16;
    if (nbytes > 64) nbytes = 64;
    uint8_t r[64];
    if (!sp_crypto_entropy(r, nbytes)) return NULL;
    sp_crypto_b64url_bytes(r, nbytes, sp_crypto_random_b64url_buf);
    return sp_crypto_random_b64url_buf;
}
