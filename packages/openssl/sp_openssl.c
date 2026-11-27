/* sp_openssl.c -- the C half of the `openssl` spin package, linked on demand
   when `require "openssl"` appears.

   Spinel does not implement TLS. This is glue over the system libssl: the
   handshake, the record-layer reads and writes, and the certificate check.
   Everything cryptographic, every protocol state machine, and the trust
   decision itself belong to OpenSSL, and the trust ANCHORS belong to the
   operating system -- SSL_CTX_set_default_verify_paths reads whatever the
   distribution's ca-certificates package installed, so a revoked CA stops
   being trusted on an OS update rather than on a Spinel release.

   The SSL * never reaches Ruby. Connections live in a table here and Ruby
   holds an int handle, the same shape sp_net gives a socket fd: no raw
   pointer is handed to a garbage-collected world, and a stale handle is a
   bounds check rather than a use-after-free.

   Layered over an fd the caller already owns (sp_net or a TCPSocket), so the
   plaintext socket surface stays exactly as it was; this unit only adds the
   record layer on top of a descriptor. Closing a connection here does not
   close the fd -- Ruby's IO owns that. */
#include "spinel/runtime.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/ecdsa.h>
#include <openssl/rsa.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
# include <openssl/decoder.h>
#endif
#include <limits.h>
#include <string.h>
#include <fcntl.h>

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
# define SP_MD_FETCH(name) EVP_MD_fetch(NULL, name, NULL)
# define SP_MD_FREE(md) EVP_MD_free((EVP_MD *)(md))
#else
# define SP_MD_FETCH(name) EVP_get_digestbyname(name)
# define SP_MD_FREE(md) ((void)(md))
#endif

static const EVP_MD *sp_openssl_md(const char *name) {
  const EVP_MD *md;
  if (!name) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  ERR_clear_error();
  md = SP_MD_FETCH(name);
  if (!md) {
    char message[1024];
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
    const char *data = NULL;
    int flags = 0;
    unsigned long error = ERR_get_error_all(NULL, NULL, NULL, &data, &flags);
    const char *reason = ERR_reason_error_string(error);
    snprintf(message, sizeof message, "unsupported digest algorithm: %s: %s%s%s%s",
             name, reason ? reason : "unknown object name",
             data && (flags & ERR_TXT_STRING) ? " (" : "",
             data && (flags & ERR_TXT_STRING) ? data : "",
             data && (flags & ERR_TXT_STRING) ? ")" : "");
#else
    snprintf(message, sizeof message, "Unsupported digest algorithm (%s).: unknown object name", name);
#endif
    ERR_clear_error();
    sp_raise_cls("OpenSSL::Digest::DigestError", message);
  }
  return md;
}

static SP_TLS unsigned char sp_openssl_digest_bytes[EVP_MAX_MD_SIZE];

typedef struct { const EVP_MD *md; int owned; } sp_verify_digest;

sp_int sp_openssl_verify_digest(const char *name) {
  if (!name) return 0;
  ERR_clear_error();
  const EVP_MD *md = EVP_get_digestbyname(name);
  if (!md) {
    ASN1_OBJECT *oid = OBJ_txt2obj(name, 0);
    md = EVP_get_digestbyobj(oid);
    ASN1_OBJECT_free(oid);
  }
  int owned = !md;
  if (!md) md = sp_openssl_md(name);
  sp_verify_digest *handle = malloc(sizeof *handle);
  if (!handle) { if (owned) SP_MD_FREE(md); sp_oom_die(); }
  *handle = (sp_verify_digest){md, owned};
  return (sp_int)(intptr_t)handle;
}

void sp_openssl_verify_digest_free(sp_int ref) {
  sp_verify_digest *handle = (sp_verify_digest *)(intptr_t)ref;
  if (!handle) return;
  if (handle->owned) SP_MD_FREE(handle->md);
  free(handle);
}

const char *sp_openssl_digest(const char *name, const char *data) {
  if (!data) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  const EVP_MD *md = sp_openssl_md(name);
  unsigned int size = 0;
  int ok = EVP_Digest(data, sp_str_byte_len(data), sp_openssl_digest_bytes, &size, md, NULL);
  SP_MD_FREE(md);
  if (!ok) sp_raise_cls("OpenSSL::Digest::DigestError", "EVP_Digest failed");
  sp_ffi_bin_len = (int)size;
  return (const char *)sp_openssl_digest_bytes;
}

static SP_TLS unsigned char sp_openssl_hmac_bytes[EVP_MAX_MD_SIZE];

const char *sp_openssl_hmac(const char *name, const char *key, const char *data) {
  if (!key || !data) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  const EVP_MD *md = sp_openssl_md(name);
  unsigned int size = 0;
  unsigned char *out;
  size_t key_size = sp_str_byte_len(key);
  if (key_size > INT_MAX) {
    SP_MD_FREE(md);
    sp_raise_cls("ArgumentError", "key is too long");
  }
  out = HMAC(md, key, (int)key_size, (const unsigned char *)data,
             sp_str_byte_len(data), sp_openssl_hmac_bytes, &size);
  SP_MD_FREE(md);
  if (!out) sp_raise_cls("OpenSSL::Digest::DigestError", "HMAC failed");
  sp_ffi_bin_len = (int)size;
  return (const char *)sp_openssl_hmac_bytes;
}

sp_int sp_openssl_fixed_length_secure_compare(const char *a, const char *b) {
  /* Byte lengths include embedded NULs; libcrypto owns the constant-time loop. */
  size_t n = sp_str_byte_len(a);
  if (n != sp_str_byte_len(b))
    sp_raise_cls("ArgumentError", "inputs must be of equal length");
  return CRYPTO_memcmp(a, b, n);
}

/* Version floor and the one API that moved. SSL_get1_peer_certificate is
   OpenSSL 3.0's name for what 1.1 called SSL_get_peer_certificate, and
   LibreSSL keeps the old one; the rest of what this file uses
   (TLS_client_method, SSL_set1_host, SSL_CTX_set_min_proto_version) arrived in
   OpenSSL 1.1.0 and LibreSSL 3.5. Below that the file will not compile, and a
   #error says so rather than leaving a page of diagnostics about missing
   declarations. */
#if defined(LIBRESSL_VERSION_NUMBER)
# if LIBRESSL_VERSION_NUMBER < 0x3010000fL
#  error "sp_openssl.c needs LibreSSL 3.1 or newer"
# endif
# define SP_SSL_PEER_CERT(ssl) SSL_get_peer_certificate(ssl)
#elif OPENSSL_VERSION_NUMBER < 0x10100000L
# error "sp_openssl.c needs OpenSSL 1.1.0 or newer"
#elif OPENSSL_VERSION_NUMBER < 0x30000000L
# define SP_SSL_PEER_CERT(ssl) SSL_get_peer_certificate(ssl)
#else
# define SP_SSL_PEER_CERT(ssl) SSL_get1_peer_certificate(ssl)
#endif

/* EVP_CTRL_AEAD_SET_IVLEN is the generic spelling of what older headers call
   EVP_CTRL_GCM_SET_IVLEN, and for GCM the two are the same control: OpenSSL
   defines the GCM name AS the AEAD one. A header set that has only the older
   name (LibreSSL 3.1.5) gets it here rather than failing to compile (#4253). */
#if !defined(EVP_CTRL_AEAD_SET_IVLEN) && defined(EVP_CTRL_GCM_SET_IVLEN)
# define EVP_CTRL_AEAD_SET_IVLEN EVP_CTRL_GCM_SET_IVLEN
#endif
#if !defined(EVP_CTRL_AEAD_GET_TAG) && defined(EVP_CTRL_GCM_GET_TAG)
# define EVP_CTRL_AEAD_GET_TAG EVP_CTRL_GCM_GET_TAG
#endif
#if !defined(EVP_CTRL_AEAD_SET_TAG) && defined(EVP_CTRL_GCM_SET_TAG)
# define EVP_CTRL_AEAD_SET_TAG EVP_CTRL_GCM_SET_TAG
#endif

#define SP_SSL_MAX 256          /* concurrent TLS connections per process */
#define SP_SSL_BUF 65536

typedef struct {
  SSL     *ssl;
  SSL_CTX *ctx;
  int      fd;
  int      in_use;        /* 0 free, 2 claimed and being filled, 1 live */
  int      nonblock;      /* O_NONBLOCK armed by the first non-blocking read */
} sp_ssl_conn;

static sp_ssl_conn sp_ssl_tab[SP_SSL_MAX];
/* Per-thread, like sp_ssl_want_state below: this is a "what happened on MY
   last call" slot, and spinel runs native calls on parallel workers. As a
   process-global it let one thread's success clear another's failure -- and
   two threads doing TLS could already read each other's reasons. */
static SP_TLS char sp_ssl_errbuf[512];
/* Per-thread for the same reason: SSL_read decrypts into this before the
   bytes are copied out, and two threads reading two connections at once
   would decrypt into one buffer. */
static SP_TLS char *sp_ssl_rdbuf;

/* Record the most recent failure so the Ruby side can raise with a reason
   rather than a bare "connect failed". OpenSSL keeps its own per-thread queue;
   this flattens the top entry, or the caller's own message when there is
   none (a would-block, an EOF, a bad handle). */
static void sp_ssl_note(const char *what) {
  unsigned long e = ERR_get_error();
  if (e) {
    char b[256];
    ERR_error_string_n(e, b, sizeof b);
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s: %s", what, b);
    while (ERR_get_error()) { }          /* drain, so the next call starts clean */
  }
else {
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s", what);
  }
}

const char *sp_ssl_last_error(void) {
  return sp_str_dup_external(sp_ssl_errbuf[0] ? sp_ssl_errbuf : "");
}

/* A free slot, claimed in the same step that finds it. Threads run native
   calls on parallel workers, so two connections set up at the same moment
   are two searches at once, and a search that only looked left both of them
   the same slot: the second connection overwrote the first, and two
   SSLSockets drove one SSL. A claimed slot reads 2 until its connection is
   in it and it is published as 1, so sp_ssl_at never answers one half
   filled. */
static int sp_ssl_slot(void) {
  for (int i = 0; i < SP_SSL_MAX; i++) {
    int free_slot = 0;
    if (SP_ATOMIC_CAS(&sp_ssl_tab[i].in_use, &free_slot, 2, 0,
                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return i;
  }
  return -1;
}

static sp_ssl_conn *sp_ssl_at(sp_int h) {
  if (h < 0 || h >= SP_SSL_MAX) return NULL;
  sp_ssl_conn *c = &sp_ssl_tab[h];
  return SP_ATOMIC_LOAD(&c->in_use, __ATOMIC_ACQUIRE) == 1 ? c : NULL;
}

/* Empty a slot and make it free again, the free last: a slot another thread
   can claim has nothing of this connection left in it. */
static void sp_ssl_vacate(sp_ssl_conn *c) {
  c->ssl = NULL;
  c->ctx = NULL;
  c->fd = -1;
  c->nonblock = 0;
  SP_ATOMIC_STORE(&c->in_use, 0, __ATOMIC_RELEASE);
}

/* Give a slot back, whatever stage it reached. */
static void sp_ssl_release(int i) {
  sp_ssl_conn *c = &sp_ssl_tab[i];
  if (c->ssl) SSL_free(c->ssl);
  if (c->ctx) SSL_CTX_free(c->ctx);
  sp_ssl_vacate(c);
}

/* ---- X509::Store ---- */
/* Ruby-side handle over an OpenSSL X509_STORE, so a program can load a
   trust store of its own (typically the system CAs) and hand it to
   sp_ssl_connect_nb. SSL_CTX_set1_cert_store increments
   the reference count, so the caller retains ownership and the handle
   remains reusable. */
struct sp_X509_Store_s {
  sp_int cls_id;
  X509_STORE *store;
};
typedef struct sp_X509_Store_s sp_X509_Store;

void sp_X509_Store_free(void *p) {
  sp_X509_Store *s = (sp_X509_Store *)p;
  if (s && s->store) {
    X509_STORE_free(s->store);
    s->store = NULL;
  }
}

sp_X509_Store *sp_X509_Store_new(sp_int cls_id) {
  sp_X509_Store *s = (sp_X509_Store *)sp_gc_alloc(sizeof(sp_X509_Store),
                                                  sp_X509_Store_free, NULL);
  if (!s) return NULL;
  s->cls_id = cls_id;
  s->store = X509_STORE_new();
  if (!s->store) {
    return NULL;
  }
  return s;
}

/* store.set_default_paths() - loads system CA certificates */
int sp_X509_Store_set_default_paths(sp_X509_Store *s) {
  if (!s || !s->store) return -1;
  return X509_STORE_set_default_paths(s->store);
}

/* store.add_cert's half: the certificate as the DER a Ruby
   X509::Certificate holds, and the store takes a reference of its own. 1
   when it is in the store, 0 with the reason in sp_ssl_last_error. */
sp_int sp_x509_store_add(sp_RbVal store, const char *der) {
  sp_X509_Store *s = store.tag != SP_TAG_NIL ? (sp_X509_Store *)store.v.p : NULL;
  const unsigned char *p = (const unsigned char *)der;
  size_t len = sp_str_byte_len(der);
  X509 *x;
  int ok;

  sp_ssl_errbuf[0] = 0;
  if (!s || !s->store) { sp_ssl_note("X509_STORE"); return 0; }
  if (len > (size_t)INT_MAX || !(x = d2i_X509(NULL, &p, (long)len))) {
    sp_ssl_note("certificate");
    return 0;
  }
  ok = X509_STORE_add_cert(s->store, x);
  X509_free(x);
  if (!ok) sp_ssl_note("X509_STORE_add_cert");
  return ok ? 1 : 0;
}

/* ---- X509::Certificate ---- */
/* A certificate is its DER, the way a key is its bytes: PEM is parsed here,
   where it crosses the boundary, and no X509 * outlives the call. Ruby holds
   the DER, and to_pem encodes it again when asked. */

/* Each entry point answers into a buffer of its own, grown to fit. A chain
   is a few kilobytes and nothing bounds it, so the fixed-size buffers the EC
   calls use do not fit here. */
typedef struct { char *p; size_t cap; } sp_x509_out;
static SP_TLS sp_x509_out sp_x509_der_out, sp_x509_load_out, sp_x509_pem_out;

static char *sp_x509_room(sp_x509_out *o, size_t n) {
  if (n > o->cap) {
    char *q = (char *)realloc(o->p, n);
    if (!q) return NULL;
    o->p = q;
    o->cap = n;
  }
  return o->p;
}

/* An empty answer plus a reason in sp_ssl_last_error, as the EC calls answer;
   the Ruby side raises OpenSSL::X509::CertificateError with it. No DER or PEM
   is empty, so "empty" is unambiguously the failure. */
static const char *sp_x509_fail(const char *what) {
  sp_ssl_note(what);
  sp_ffi_bin_len = 0;
  return "";
}

/* BIO_new_mem_buf takes an int length and sp_ffi_bin_len publishes one, so
   input past half of INT_MAX is refused here rather than narrowed. Half
   leaves room for what the answers add to it: a 4-byte length per
   certificate, and PEM's growth over the DER it encodes. */
static int sp_x509_too_big(const char *data) {
  return sp_str_byte_len(data) > (size_t)INT_MAX / 2;
}

/* DER first, then PEM, the order CRuby's Certificate.new and .load try them
   in: DER is binary and can hold bytes that look like a PEM header, while
   PEM never parses as DER. */
static X509 *sp_x509_from_der(const char *data) {
  const unsigned char *p = (const unsigned char *)data;
  X509 *x = d2i_X509(NULL, &p, (long)sp_str_byte_len(data));
  if (!x) ERR_clear_error();
  return x;
}

/* Append one certificate as a 4-byte big-endian length and its DER. */
static int sp_x509_append(sp_x509_out *o, size_t *at, X509 *x) {
  int n = i2d_X509(x, NULL);
  unsigned char *p;
  if (n <= 0 || !sp_x509_room(o, *at + 4 + (size_t)n)) return 0;
  p = (unsigned char *)o->p + *at;
  p[0] = (unsigned char)(n >> 24);
  p[1] = (unsigned char)(n >> 16);
  p[2] = (unsigned char)(n >> 8);
  p[3] = (unsigned char)n;
  p += 4;
  if (i2d_X509(x, &p) != n) return 0;
  *at += 4 + (size_t)n;
  return 1;
}

/* Certificate.new: one certificate, PEM or DER, answered as its DER. */
const char *sp_x509_der(const char *data) {
  X509 *x;
  BIO *bio;
  size_t at = 0;
  int ok;

  sp_ssl_errbuf[0] = 0;
  if (sp_x509_too_big(data)) return sp_x509_fail("certificate data too large");
  if (!(x = sp_x509_from_der(data))) {
    if (!(bio = BIO_new_mem_buf(data, (int)sp_str_byte_len(data))))
      return sp_x509_fail("BIO_new_mem_buf");
    x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!x) return sp_x509_fail("PEM_read_bio_X509");
  }
  ok = sp_x509_append(&sp_x509_der_out, &at, x);
  X509_free(x);
  if (!ok) return sp_x509_fail("i2d_X509");
  sp_ffi_bin_len = (int)at - 4;
  return sp_x509_der_out.p + 4;
}

/* Certificate.load: every certificate in a PEM string, in order, or the one
   in a DER string, each as a 4-byte length and its DER. As in CRuby, the PEM
   read ends at the first place no further BEGIN line is found; any other
   failure is a certificate that did not parse, and the whole load fails
   rather than answering the ones before it. */
const char *sp_x509_load(const char *data) {
  X509 *x;
  BIO *bio;
  size_t at = 0;
  int ok = 1;

  sp_ssl_errbuf[0] = 0;
  if (sp_x509_too_big(data)) return sp_x509_fail("certificate data too large");
  if ((x = sp_x509_from_der(data))) {
    ok = sp_x509_append(&sp_x509_load_out, &at, x);
    X509_free(x);
    if (!ok) return sp_x509_fail("i2d_X509");
    sp_ffi_bin_len = (int)at;
    return sp_x509_load_out.p;
  }
  if (!(bio = BIO_new_mem_buf(data, (int)sp_str_byte_len(data))))
    return sp_x509_fail("BIO_new_mem_buf");
  while (ok && (x = PEM_read_bio_X509(bio, NULL, NULL, NULL))) {
    ok = sp_x509_append(&sp_x509_load_out, &at, x);
    X509_free(x);
  }
  BIO_free(bio);
  if (!ok) return sp_x509_fail("i2d_X509");
  if (at == 0) {
    ERR_clear_error();
    return sp_x509_fail("Could not detect format of certificate data!");
  }
  if (ERR_GET_REASON(ERR_peek_last_error()) != PEM_R_NO_START_LINE)
    return sp_x509_fail("PEM_read_bio_X509");
  ERR_clear_error();
  sp_ffi_bin_len = (int)at;
  return sp_x509_load_out.p;
}

/* Certificate#to_pem: the DER Ruby holds, written as OpenSSL writes it. */
const char *sp_x509_pem(const char *der) {
  X509 *x = sp_x509_from_der(der);
  BIO *bio = NULL;
  char *mem = NULL;
  long n = 0;

  sp_ssl_errbuf[0] = 0;
  if (!x) return sp_x509_fail("d2i_X509");
  if ((bio = BIO_new(BIO_s_mem())) && PEM_write_bio_X509(bio, x))
    n = BIO_get_mem_data(bio, &mem);
  X509_free(x);
  if (n <= 0 || !sp_x509_room(&sp_x509_pem_out, (size_t)n)) {
    BIO_free(bio);
    return sp_x509_fail("PEM_write_bio_X509");
  }
  memcpy(sp_x509_pem_out.p, mem, (size_t)n);
  BIO_free(bio);
  sp_ffi_bin_len = (int)n;
  return sp_x509_pem_out.p;
}

/* Everything a client connection needs EXCEPT the handshake: the slot, the
   context, the SSL object and the descriptor. Kept apart from the handshake,
   which Ruby drives one step at a time, so the verify and SNI setup has one
   place to live and no second copy for a security default to drift out of
   step with. Answers the slot index with the slot claimed, or -1 with the
   reason in sp_ssl_last_error.
   `cert_store` is an optional Ruby X509::Store object; if non-NIL, its
   native X509_STORE is referenced by the SSL_CTX (SSL_CTX_set1_cert_store, so the store stays the handle's and is reusable).
   `chain`, `curve` and `priv` are the client's own certificate and key, laid
   out as for a server (sp_ssl_use_chain below), and all three empty when the
   context has none; OpenSSL sends them only to a server that asks. */
static int sp_ssl_use_chain(SSL_CTX *ctx, const char *chain, const char *curve, const char *priv);

/* The protocol bounds SSLContext#min_version= and #max_version= set, 0 where
   the program set none. TLS 1.2 is the floor a context without a minimum
   gets: 1.0 and 1.1 are withdrawn, and a server offering only those is not
   one to fall back to silently. A program that sets a lower minimum has
   asked for it, which is not silent. The bounds were checked when they were
   set (sp_ssl_proto_check), so this fails only as SSL_CTX_new could. */
static int sp_ssl_bounds(SSL_CTX *ctx, sp_int min, sp_int max) {
  if (!SSL_CTX_set_min_proto_version(ctx, min ? (int)min : TLS1_2_VERSION)) {
    sp_ssl_note("SSL_CTX_set_min_proto_version");
    return 0;
  }
  if (!SSL_CTX_set_max_proto_version(ctx, (int)max)) {
    sp_ssl_note("SSL_CTX_set_max_proto_version");
    return 0;
  }
  return 1;
}

/* min_version= and max_version='s check, made when they are called, as
   CRuby makes it on its context: 0 when OpenSSL takes both bounds, 1 when it
   refuses the minimum, 2 the maximum. The context is built and dropped, as
   sp_ssl_server_check's is. */
sp_int sp_ssl_proto_check(sp_int min, sp_int max) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  sp_int r = 0;

  if (!ctx) return 1;
  if (!SSL_CTX_set_min_proto_version(ctx, (int)min)) r = 1;
  else if (!SSL_CTX_set_max_proto_version(ctx, (int)max)) r = 2;
  SSL_CTX_free(ctx);
  ERR_clear_error();
  return r;
}

static int sp_ssl_setup(sp_int fd, const char *hostname, sp_int verify, sp_int verify_hostname, sp_RbVal cert_store,
                        const char *chain, const char *curve, const char *priv, sp_int min, sp_int max) {
  sp_ssl_errbuf[0] = 0;

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) { sp_ssl_note("SSL_CTX_new"); return -1; }
  if (!sp_ssl_bounds(ctx, min, max)) { SSL_CTX_free(ctx); return -1; }
  /* A write that had to wait is retried from Ruby with the same bytes, and
     nothing promises they are at the same address; OpenSSL refuses a retry
     from a moved buffer unless this is set. */
  SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (verify) {
    /* The trust anchors are the operating system's. Nothing is bundled here,
       so a CA the OS stops trusting stops being trusted with it. */
    if (!SSL_CTX_set_default_verify_paths(ctx)) {
      sp_ssl_note("no system trust store");
      SSL_CTX_free(ctx);
      return -1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  }
else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  /* Apply custom cert store if provided (Ruby X509::Store object) */
  if (cert_store.tag != SP_TAG_NIL) {
    sp_X509_Store *store = (sp_X509_Store *)cert_store.v.p;
    if (store && store->store) {
      /* SSL_CTX_set1_cert_store increments the reference count,
       * so the caller (SSLContext#cert_store) retains ownership and
       * remains reusable. */
      SSL_CTX_set1_cert_store(ctx, store->store);
    }
  }
  if (!sp_ssl_use_chain(ctx, chain, curve, priv)) { SSL_CTX_free(ctx); return -1; }

  SSL *ssl = SSL_new(ctx);
  if (!ssl) { sp_ssl_note("SSL_new"); SSL_CTX_free(ctx); return -1; }
  /* The handshake step is SSL_do_handshake, which a server's connection
     shares; this is what makes it a client's. */
  SSL_set_connect_state(ssl);
  if (hostname && hostname[0]) {
    SSL_set_tlsext_host_name(ssl, hostname);     /* SNI */
    if (verify && verify_hostname) {
      /* Without this the chain validates and the NAME does not, which is the
         classic way to have TLS and no security. */
      SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      if (!SSL_set1_host(ssl, hostname)) {
        sp_ssl_note("SSL_set1_host");
        SSL_free(ssl); SSL_CTX_free(ctx);
        return -1;
      }
    }
  }
  if (!SSL_set_fd(ssl, (int)fd)) {
    sp_ssl_note("SSL_set_fd");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  /* The slot is claimed last, once there is a connection to put in it, so
     no failure above has one to give back. */
  int i = sp_ssl_slot();
  if (i < 0) {
    sp_ssl_note("too many TLS connections");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  sp_ssl_tab[i].ssl = ssl;
  sp_ssl_tab[i].ctx = ctx;
  sp_ssl_tab[i].fd  = (int)fd;
  sp_ssl_tab[i].nonblock = 0;
  SP_ATOMIC_STORE(&sp_ssl_tab[i].in_use, 1, __ATOMIC_RELEASE);
  return i;
}

/* Why a non-blocking read needs a status and not just bytes: TLS is a record
   layer, so "nothing to read yet" and "I must WRITE before I can read" are
   different answers -- a renegotiating peer makes SSL_read ask for the socket
   to become writable. The caller has to wait on the right direction, so the
   reason rides in a separate slot rather than being flattened into "".
     0 ok   1 want-read   2 want-write   3 eof   4 error */
static SP_TLS int sp_ssl_want_state = 0;

sp_int sp_ssl_want(void) { return sp_ssl_want_state; }

/* Drive the handshake ONE step against a non-blocking descriptor, and say in
   sp_ssl_want what it wants next: 0 finished, 1 wait until readable, 2 wait
   until writable, 4 a real failure with the reason in sp_ssl_last_error.
   The two directions are separate because a handshake genuinely needs both,
   and a caller waiting on the wrong one waits forever. The same step drives
   a client's handshake and a server's: the connection was set up as one or
   the other. */
static void sp_ssl_handshake_step(sp_ssl_conn *c) {
  sp_ssl_errbuf[0] = 0;
  sp_ssl_want_state = 0;
  int r = SSL_do_handshake(c->ssl);
  if (r == 1) return;
  switch (SSL_get_error(c->ssl, r)) {
    case SSL_ERROR_WANT_READ:  sp_ssl_want_state = 1; break;
    case SSL_ERROR_WANT_WRITE: sp_ssl_want_state = 2; break;
    default:                   sp_ssl_want_state = 4;
                               sp_ssl_note("TLS handshake failed"); break;
  }
}

/* Begin a client connection without waiting for the handshake to finish.
   Both of Ruby's connects start here, the blocking one included: it waits
   between steps on the descriptor, where a green thread parks, and not
   inside SSL_connect, where a quiet peer would hold the OS worker in the
   kernel and every other thread at the next collection.

   This arms O_NONBLOCK on the descriptor itself: the handshake is the FIRST
   thing on the connection, so there is no earlier call to have done it, and
   a blocking fd here would put the wait back in the kernel, which is the
   whole point of the call. Answers the handle as soon as the connection
   exists -- the handshake may well be unfinished, and sp_ssl_want says so --
   or -1 when the connection could not be built. The handle is kept across
   calls so the handshake can be resumed. */
sp_int sp_ssl_connect_nb(sp_int fd, const char *hostname, sp_int verify, sp_int verify_hostname, sp_RbVal cert_store,
                         const char *chain, const char *curve, const char *priv, sp_int min, sp_int max) {
  int i = sp_ssl_setup(fd, hostname, verify, verify_hostname, cert_store, chain, curve, priv, min, max);
  if (i < 0) { sp_ssl_want_state = 4; return -1; }
  sp_ssl_conn *c = &sp_ssl_tab[i];
  int fl = fcntl(c->fd, F_GETFL, 0);
  if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
  c->nonblock = 1;
  sp_ssl_handshake_step(c);
  return i;
}

/* Resume a handshake begun by sp_ssl_connect_nb or sp_ssl_accept_nb. 1 when
   it is done, 0 when it is not, with sp_ssl_want carrying which of the two
   waits, or the failure. */
sp_int sp_ssl_handshake_cont(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) { sp_ssl_want_state = 4; sp_ssl_note("closed TLS connection"); return 0; }
  sp_ssl_handshake_step(c);
  return sp_ssl_want_state == 0 ? 1 : 0;
}

/* Up to `maxlen` bytes without blocking. The fd must already be non-blocking;
   this does not set it, because the descriptor belongs to the caller's IO.
   Answers "" whenever it did not read, with the reason in sp_ssl_want. */
const char *sp_ssl_read_nb(sp_int h, sp_int maxlen) {
  sp_ssl_conn *c = sp_ssl_at(h);
  sp_ssl_want_state = 0;
  if (!c) { sp_ssl_want_state = 4; sp_ssl_note("closed TLS connection"); return sp_str_dup_external(""); }
  if (!sp_ssl_rdbuf && !(sp_ssl_rdbuf = (char *)malloc(SP_SSL_BUF))) {
    sp_ssl_want_state = 4;
    return sp_str_dup_external("");
  }
  if (maxlen <= 0 || maxlen >= SP_SSL_BUF) maxlen = SP_SSL_BUF - 1;
  sp_ssl_errbuf[0] = 0;
  /* CRuby's sysread_nonblock arms O_NONBLOCK on the descriptor itself rather
     than making the caller do it (rb_io_set_nonblock), so this does too --
     once, on the first non-blocking read, and never back off again, which is
     also what CRuby leaves behind. */
  if (!c->nonblock) {
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    c->nonblock = 1;
  }
  /* Bytes already decrypted answer immediately: the descriptor can be quiet
     while a whole record sits in the record layer, so asking the fd first
     would park a caller that already has data. */
  int n = SSL_read(c->ssl, sp_ssl_rdbuf, (int)maxlen);
  if (n > 0) {
    char *out = sp_str_alloc((size_t)n);
    memcpy(out, sp_ssl_rdbuf, (size_t)n);
    out[n] = 0;
    sp_str_set_len(out, (size_t)n);
    /* Bytes off the wire, as IO#sysread's: a String that read as UTF-8 made the
       buffer behind #read and #readpartial cut by characters, not bytes. */
    sp_str_mark_binary(out);
    return out;
  }
  switch (SSL_get_error(c->ssl, n)) {
    case SSL_ERROR_WANT_READ:   sp_ssl_want_state = 1; break;
    case SSL_ERROR_WANT_WRITE:  sp_ssl_want_state = 2; break;
    case SSL_ERROR_ZERO_RETURN: sp_ssl_want_state = 3; break;
    default:                    sp_ssl_want_state = 4; sp_ssl_note("SSL_read"); break;
  }
  return sp_str_dup_external("");
}

/* One record-layer write that never blocks, the mirror of sp_ssl_read_nb: the
   reason for writing nothing rides in sp_ssl_want, because a TLS write can
   need the socket to become READABLE first -- the peer's side of a
   renegotiation -- and a caller waiting on the wrong direction waits forever.
   Answers the byte count, or 0 with the reason set. */
sp_int sp_ssl_write_nb(sp_int h, const char *data, sp_int n) {
  sp_ssl_conn *c = sp_ssl_at(h);
  sp_ssl_want_state = 0;
  if (!c) { sp_ssl_want_state = 4; sp_ssl_note("closed TLS connection"); return 0; }
  if (n <= 0) return 0;
  if (!c->nonblock) {
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    c->nonblock = 1;
  }
  sp_ssl_errbuf[0] = 0;
  int w = SSL_write(c->ssl, data, (int)n);
  if (w > 0) return w;
  switch (SSL_get_error(c->ssl, w)) {
    case SSL_ERROR_WANT_READ:   sp_ssl_want_state = 1; break;
    case SSL_ERROR_WANT_WRITE:  sp_ssl_want_state = 2; break;
    case SSL_ERROR_ZERO_RETURN: sp_ssl_want_state = 3; break;
    default:                    sp_ssl_want_state = 4; sp_ssl_note("SSL_write"); break;
  }
  return 0;
}

/* Bytes already decrypted and waiting in the record layer. An event loop that
   selects on the fd alone will miss these: a whole record can arrive in one
   read, leaving the descriptor quiet while the application still has data. */
sp_int sp_ssl_pending(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  return c ? (sp_int)SSL_pending(c->ssl) : 0;
}

/* The peer's certificate subject, for a caller that wants to report it.
   "" when there is none. */
const char *sp_ssl_peer_subject(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) return sp_str_dup_external("");
  X509 *cert = SP_SSL_PEER_CERT(c->ssl);
  if (!cert) return sp_str_dup_external("");
  char buf[512];
  X509_NAME_oneline(X509_get_subject_name(cert), buf, (int)sizeof buf);
  X509_free(cert);
  return sp_str_dup_external(buf);
}

/* SSLSocket#peer_cert: the peer's leaf certificate as its DER, which Ruby
   makes an X509::Certificate of, as it makes one of any DER. "" when the
   peer sent none -- a client a server did not ask, or no handshake yet --
   and SSLSocket#peer_cert answers nil for that, as CRuby's does. */
static SP_TLS sp_x509_out sp_ssl_peer_out;

const char *sp_ssl_peer_cert(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  X509 *cert;
  size_t at = 0;
  int ok;

  sp_ffi_bin_len = 0;
  if (!c || !(cert = SP_SSL_PEER_CERT(c->ssl))) return "";
  ok = sp_x509_append(&sp_ssl_peer_out, &at, cert);
  X509_free(cert);
  if (!ok) return sp_x509_fail("i2d_X509");
  sp_ffi_bin_len = (int)at - 4;
  return sp_ssl_peer_out.p + 4;
}

/* SSLSocket#export_keying_material: `length` bytes both ends of the
   connection derive alike from the label and the context, the RFC 5705
   exporter (RFC 8446 7.5 under TLS 1.3), which DTLS-SRTP and channel
   binding take their keys from. `use_context` is 0 for no context at all,
   which TLS 1.2 tells apart from an empty one.
   Answers 0x01 || the bytes, or "" with the reason in sp_ssl_last_error: a
   zero-length export is a legitimate answer, so the status rides in the
   answer itself, as sp_aes_gcm_decrypt's does. */
const char *sp_ssl_export_keying_material(sp_int h, const char *label, sp_int length,
                                          const char *context, sp_int use_context) {
  sp_ssl_conn *c = sp_ssl_at(h);
  char *out;

  sp_ssl_errbuf[0] = 0;
  if (!c || length < 0 || (size_t)length > (size_t)INT_MAX - 2) {
    sp_ssl_note("SSL_export_keying_material");
    return sp_str_from_bytes("", 0);
  }
  out = sp_str_alloc((size_t)length + 1);
  out[0] = 1;
  if (SSL_export_keying_material(c->ssl, (unsigned char *)out + 1, (size_t)length,
                                 label, sp_str_byte_len(label),
                                 (const unsigned char *)context, sp_str_byte_len(context),
                                 use_context ? 1 : 0) != 1) {
    sp_ssl_note("SSL_export_keying_material");
    return sp_str_from_bytes("", 0);
  }
  out[length + 1] = 0;
  sp_str_set_len(out, (size_t)length + 1);
  return sp_str_as_binary(out);
}

const char *sp_ssl_version(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  return sp_str_dup_external(c ? SSL_get_version(c->ssl) : "");
}

const char *sp_ssl_cipher(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  const SSL_CIPHER *ci = c ? SSL_get_current_cipher(c->ssl) : NULL;
  return sp_str_dup_external(ci ? SSL_CIPHER_get_name(ci) : "");
}

/* Send close_notify and release the slot. The fd is NOT closed: Ruby's IO
   owns it and will close it itself, and closing it here would pull the
   descriptor out from under a still-live IO object. */
sp_int sp_ssl_close(sp_int h) {
  sp_ssl_conn *c = sp_ssl_at(h);
  if (!c) return -1;
  SSL_shutdown(c->ssl);
  SSL_free(c->ssl);
  SSL_CTX_free(c->ctx);
  sp_ssl_vacate(c);
  return 0;
}

/* Release a connection whose handshake failed. No close_notify: OpenSSL
   says SSL_shutdown must not follow a fatal error, and there is no session
   to close. The error queue goes with it, so the next call's reason is its
   own. */
sp_int sp_ssl_discard(sp_int h) {
  if (!sp_ssl_at(h)) return -1;
  sp_ssl_release((int)h);
  ERR_clear_error();
  return 0;
}

/* ---------- EC over the named prime curves ----------

   The shape is the one #4221 settled on: one-shot functions over raw bytes.
   BN and EC::Point wrappers on the Ruby side hold values, and -- unlike the
   connection table above -- no native handle outlives a call. An EC key is fully
   described by its two byte strings (the private scalar and the uncompressed
   point), so Ruby holds those and hands them back, rather than holding a
   handle into a table with a slot somebody has to remember to release. A key
   abandoned mid-protocol costs nothing.

   Everything below is the EC_GROUP / EC_POINT / BIGNUM API rather than
   EC_KEY or ECDH_compute_key: those two are deprecated in OpenSSL 3.0, and
   their replacements (EVP_PKEY_get_bn_param and friends) did not exist at
   this file's 1.1.0 floor. The low-level three are current in 3.x and present
   in every version this file compiles against.

   Points cross the boundary in X9.62 uncompressed form, 0x04 || X || Y, which
   is what every protocol that names raw EC keys means by "the public key" --
   RFC 8291 Web Push, JWK's x and y halves, and WebAuthn all use it. It also
   makes the ECDH answer a slice: the shared secret is the X coordinate, which
   is bytes 1..flen of the encoded product point. */

/* P-521, the widest curve anyone names: 66 bytes of field, 133 of point. */
#define SP_EC_MAX_FIELD 66
#define SP_EC_MAX_POINT (1 + 2 * SP_EC_MAX_FIELD)

/* Each entry point gets its own buffer, the same rule sp_crypto.c states for
   its digests: a caller holding the bytes of one result must not have the
   next call clobber them. `key.public_key_bytes` and `key.dh_compute_key(x)`
   are exactly that pair. */
static SP_TLS char sp_ec_gen_buf[SP_EC_MAX_FIELD];
static SP_TLS char sp_ec_pub_buf[SP_EC_MAX_POINT];
static SP_TLS char sp_ec_dh_buf[SP_EC_MAX_FIELD];

/* An empty answer plus a reason in sp_ssl_last_error; the Ruby side turns it
   into OpenSSL::PKey::ECError. Nothing here answers a short-but-nonempty
   string, so "empty" is unambiguously the failure. */
static const char *sp_ec_fail(const char *what) {
  sp_ssl_note(what);
  sp_ffi_bin_len = 0;
  return "";
}

static EC_GROUP *sp_ec_group(const char *curve, int *flen) {
  int nid = OBJ_txt2nid(curve);
  EC_GROUP *g;
  if (nid == NID_undef) return NULL;
  if (!(g = EC_GROUP_new_by_curve_name(nid))) return NULL;
  *flen = (EC_GROUP_get_degree(g) + 7) / 8;
  if (*flen <= 0 || *flen > SP_EC_MAX_FIELD) { EC_GROUP_free(g); return NULL; }
  return g;
}

/* A private key is a scalar in [1, n-1], written field-width big-endian.
   Out-of-range is rejected here rather than multiplied into a point that is
   not the caller's key: d == 0 gives the point at infinity, and d >= n is a
   silent alias for d - n. */
static BIGNUM *sp_ec_scalar(const EC_GROUP *g, const char *priv, int flen) {
  BIGNUM *d, *order;
  if ((int)sp_str_byte_len(priv) != flen) return NULL;
  if (!(d = BN_bin2bn((const unsigned char *)priv, flen, NULL))) return NULL;
  if (!(order = BN_new())) { BN_free(d); return NULL; }
  if (!EC_GROUP_get_order(g, order, NULL) ||
      BN_is_zero(d) || BN_cmp(d, order) >= 0) {
    BN_free(order); BN_clear_free(d); return NULL;
  }
  BN_free(order);
  return d;
}

/* Read a peer's uncompressed point, refusing anything that is not a usable
   public key on this curve. EC_POINT_oct2point already rejects a point off
   the curve, and the explicit check after it is belt and braces: an ECDH
   against an attacker-chosen off-curve point leaks the private scalar a few
   bits at a time (the invalid-curve attack), and the whole defence is this
   one test. Small-subgroup confinement needs no separate check on the prime
   curves OBJ_txt2nid resolves, whose cofactor is 1 -- every on-curve point
   other than infinity generates the full group. */
static EC_POINT *sp_ec_peer(const EC_GROUP *g, const char *pub, int flen) {
  EC_POINT *p;
  size_t want = (size_t)(1 + 2 * flen);
  if (sp_str_byte_len(pub) != want || (unsigned char)pub[0] != 0x04) return NULL;
  if (!(p = EC_POINT_new(g))) return NULL;
  if (!EC_POINT_oct2point(g, p, (const unsigned char *)pub, want, NULL) ||
      !EC_POINT_is_on_curve(g, p, NULL) ||
      EC_POINT_is_at_infinity(g, p)) {
    EC_POINT_free(p);
    return NULL;
  }
  return p;
}

const char *sp_ec_curve_name(const char *curve, sp_int aliases) {
  int flen = 0;
  /* EC keys accept OBJ_txt2nid spellings; Group.new accepts short names. */
  EC_GROUP *group = aliases ? sp_ec_group(curve, &flen) : EC_GROUP_new_by_curve_name(OBJ_sn2nid(curve));
  const char *name = group ? OBJ_nid2sn(EC_GROUP_get_curve_name(group)) : NULL;
  if (!group) ERR_clear_error();
  EC_GROUP_free(group);
  return name ? name : "";
}

/* The same boxed Integer boundary used by the JSON package: BN values must
   stay exact even when ordinary Ruby arithmetic uses machine integers. */
sp_Bigint *sp_bigint_new_str(const char *s, int base);
static sp_RbVal sp_openssl_bn_answer(BIGNUM *bn) {
  char *hex = bn ? BN_bn2hex(bn) : NULL;
  BN_free(bn);
  if (!hex) {
    ERR_clear_error();
    sp_raise_cls("OpenSSL::BNError", "BIGNUM conversion failed");
  }
  sp_RbVal result = sp_box_bigint(sp_bigint_new_str(hex, 16));
  OPENSSL_free(hex);
  return result;
}

static void sp_openssl_bn_radix_error(sp_int base) {
  char message[80];
  snprintf(message, sizeof message, "invalid radix %ld", (long)base);
  sp_raise_cls("ArgumentError", message);
}

/* Numeric formatting shared by BN and ASN.1, without Ruby method dispatch. */
static const char *sp_openssl_integer_decimal(sp_RbVal value) {
  SP_GC_ROOT_RBVAL(value);
  const char *decimal = value.tag == SP_TAG_INT ? sp_int_to_s(value.v.i) :
    value.tag == SP_TAG_BIGINT ? sp_bigint_to_s((sp_Bigint *)value.v.p) : NULL;
  if (!decimal) sp_raise_cls("TypeError", "Cannot convert into OpenSSL::BN");
  return decimal;
}

sp_int sp_openssl_bn_compare(sp_RbVal left_value, sp_RbVal right_value) {
  SP_GC_ROOT_RBVAL(left_value);
  SP_GC_ROOT_RBVAL(right_value);
  const char *left = sp_openssl_integer_decimal(left_value); SP_GC_ROOT_STR(left);
  const char *right = sp_openssl_integer_decimal(right_value); SP_GC_ROOT_STR(right);
  BIGNUM *a = NULL, *b = NULL;
  int ok = left && right && BN_dec2bn(&a, left) && BN_dec2bn(&b, right);
  int result = ok ? BN_cmp(a, b) : 0;
  BN_free(a);
  BN_free(b);
  if (!ok) {
    ERR_clear_error();
    sp_raise_cls("OpenSSL::BNError", "BIGNUM comparison failed");
  }
  return (result > 0) - (result < 0);
}

sp_RbVal sp_openssl_bn_integer(const char *bytes, sp_int base) {SP_GC_ROOT_STR(bytes);
  BIGNUM *bn = NULL;
  size_t length = bytes ? sp_str_byte_len(bytes) : 0;
  ERR_clear_error();
  if (!bytes) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (length > INT_MAX) sp_raise_cls("RangeError", "BIGNUM input too large");
  if (base == 2) bn = BN_bin2bn((const unsigned char *)bytes, (int)length, NULL);
  else if (base == 0) bn = BN_mpi2bn((const unsigned char *)bytes, (int)length, NULL);
  else if (base == 10) BN_dec2bn(&bn, bytes);
  else if (base == 16) BN_hex2bn(&bn, bytes);
  else sp_openssl_bn_radix_error(base);
  return sp_openssl_bn_answer(bn);
}

static SP_TLS sp_x509_out sp_openssl_bn_out;
const char *sp_openssl_bn_string(sp_RbVal value, sp_int base) {
  SP_GC_ROOT_RBVAL(value);
  const char *decimal = sp_openssl_integer_decimal(value); SP_GC_ROOT_STR(decimal);
  BIGNUM *bn = NULL;
  char *text = NULL;
  int length = -1;
  ERR_clear_error();
  if (decimal && BN_dec2bn(&bn, decimal)) {
    if (base == 10 || base == 16) {
      text = base == 10 ? BN_bn2dec(bn) : BN_bn2hex(bn);
      if (text && strlen(text) <= INT_MAX) length = (int)strlen(text);
    }
    else if (base == 2) length = BN_num_bytes(bn);
    else if (base == 0) length = BN_bn2mpi(bn, NULL);
    else {
      BN_free(bn);
      sp_openssl_bn_radix_error(base);
    }
  }
  if (length >= 0 && sp_x509_room(&sp_openssl_bn_out, (size_t)length + 1)) {
    if (text) memcpy(sp_openssl_bn_out.p, text, (size_t)length);
    else if (base == 2) BN_bn2bin(bn, (unsigned char *)sp_openssl_bn_out.p);
    else BN_bn2mpi(bn, (unsigned char *)sp_openssl_bn_out.p);
  }
  else length = -1;
  BN_free(bn);
  OPENSSL_free(text);
  if (length < 0) {
    ERR_clear_error();
    sp_raise_cls("OpenSSL::BNError", "BIGNUM conversion failed");
  }
  sp_ffi_bin_len = length;
  return sp_openssl_bn_out.p;
}

sp_RbVal sp_ec_group_order(const char *curve) {SP_GC_ROOT_STR(curve);
  EC_GROUP *group = curve ? EC_GROUP_new_by_curve_name(OBJ_txt2nid(curve)) : NULL;
  BIGNUM *order = BN_new();
  int ok = group && order && EC_GROUP_get_order(group, order, NULL);
  EC_GROUP_free(group);
  if (!ok) {
    BN_free(order);
    ERR_clear_error();
    sp_raise_cls("OpenSSL::PKey::EC::Group::Error", "EC group order unavailable");
  }
  return sp_openssl_bn_answer(order);
}

static SP_TLS sp_x509_out sp_ec_octets_out;
const char *sp_ec_point_octets(const char *curve, const char *bytes, sp_int form) {
  EC_GROUP *group = curve ? EC_GROUP_new_by_curve_name(OBJ_txt2nid(curve)) : NULL;
  EC_POINT *point = group ? EC_POINT_new(group) : NULL;
  size_t length = 0;
  sp_ssl_errbuf[0] = 0;
  if (point && bytes && (form == 2 || form == 4 || form == 6) &&
      EC_POINT_oct2point(group, point, (const unsigned char *)bytes, sp_str_byte_len(bytes), NULL) &&
      EC_POINT_is_on_curve(group, point, NULL) == 1) {
    length = EC_POINT_point2oct(group, point, (point_conversion_form_t)form, NULL, 0, NULL);
    if (length && length <= INT_MAX && sp_x509_room(&sp_ec_octets_out, length))
      length = EC_POINT_point2oct(group, point, (point_conversion_form_t)form,
                                (unsigned char *)sp_ec_octets_out.p, length, NULL);
    else length = 0;
  }
  EC_POINT_free(point);
  EC_GROUP_free(group);
  if (!length) return sp_ec_fail("invalid EC point");
  sp_ffi_bin_len = (int)length;
  return sp_ec_octets_out.p;
}

static SP_TLS sp_x509_out sp_ec_export_out;

/* SPKI public DER, PKCS#8 private DER or PEM. EC_KEY is confined to the
   same compatibility boundary as ECDSA below (OpenSSL 1.1.0 through 3.x). */
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
const char *sp_ec_export(const char *curve, const char *priv, const char *pub,
                       sp_int format, sp_int point_form, sp_int encoding_flags) {
  int flen = 0, ok = 0;
  EC_GROUP *group = sp_ec_group(curve, &flen);
  EC_POINT *point = group ? sp_ec_peer(group, pub, flen) : NULL;
  BIGNUM *scalar = group && format ? sp_ec_scalar(group, priv, flen) : NULL;
  EC_KEY *ec = EC_KEY_new();
  EVP_PKEY *key = EVP_PKEY_new();
  BIO *bio = BIO_new(BIO_s_mem());
  char *data = NULL;
  long length = 0;
  sp_ssl_errbuf[0] = 0;
  if (format >= 0 && format <= 2 && ec && key && bio && group && point &&
      (!format || scalar) && EC_KEY_set_group(ec, group) && EC_KEY_set_public_key(ec, point) &&
      (!format || EC_KEY_set_private_key(ec, scalar)) && EC_KEY_check_key(ec)) {
    EC_KEY_set_asn1_flag(ec, OPENSSL_EC_NAMED_CURVE);
    EC_KEY_set_conv_form(ec, (point_conversion_form_t)point_form);
    EC_KEY_set_enc_flags(ec, (unsigned)encoding_flags);
    if (!EVP_PKEY_set1_EC_KEY(key, ec)) goto done;
    if (format == 0) ok = i2d_PUBKEY_bio(bio, key);
    else if (format == 1) ok = i2d_PKCS8PrivateKey_bio(bio, key, NULL, NULL, 0, NULL, NULL);
    else ok = PEM_write_bio_PKCS8PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL);
    length = BIO_get_mem_data(bio, &data);
    ok = ok && length > 0 && length <= INT_MAX && sp_x509_room(&sp_ec_export_out, (size_t)length);
    if (ok) memcpy(sp_ec_export_out.p, data, (size_t)length);
  }
done:
  BIO_free(bio);
  EVP_PKEY_free(key);
  EC_KEY_free(ec);
  BN_clear_free(scalar);
  EC_POINT_free(point);
  EC_GROUP_free(group);
  if (!ok) return sp_ec_fail("EC key export failed");
  sp_ffi_bin_len = (int)length;
  return sp_ec_export_out.p;
}
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif

/* A fresh private scalar, uniform in [1, n-1]. BN_rand_range draws from the
   same CSPRNG the rest of libcrypto uses; the retry is for the zero it can
   return, which is not a key. */
const char *sp_ec_generate(const char *curve) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *order = NULL, *d = NULL;
  int ok = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) return sp_ec_fail("unknown or unsupported curve");
  if ((order = BN_new()) && (d = BN_new()) && EC_GROUP_get_order(g, order, NULL)) {
    for (int tries = 0; tries < 16 && !ok; tries++)
      if (BN_rand_range(d, order) && !BN_is_zero(d)) ok = 1;
  }
  if (ok) ok = BN_bn2binpad(d, (unsigned char *)sp_ec_gen_buf, flen) == flen;

  BN_free(order);
  BN_clear_free(d);
  EC_GROUP_free(g);
  if (!ok) return sp_ec_fail("EC key generation failed");
  sp_ffi_bin_len = flen;
  return sp_ec_gen_buf;
}

/* dG for a private scalar d: the public half of a key the caller already
   holds, which is why generate() answers only the scalar. */
const char *sp_ec_public_bytes(const char *curve, const char *priv) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *d = NULL;
  EC_POINT *pub = NULL;
  size_t n = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) return sp_ec_fail("unknown or unsupported curve");
  if (!(d = sp_ec_scalar(g, priv, flen))) {
    EC_GROUP_free(g);
    return sp_ec_fail("private key is not a scalar on this curve");
  }
  if ((pub = EC_POINT_new(g)) && EC_POINT_mul(g, pub, d, NULL, NULL, NULL))
    n = EC_POINT_point2oct(g, pub, POINT_CONVERSION_UNCOMPRESSED,
                           (unsigned char *)sp_ec_pub_buf,
                           (size_t)(1 + 2 * flen), NULL);

  EC_POINT_free(pub);
  BN_clear_free(d);
  EC_GROUP_free(g);
  if (n != (size_t)(1 + 2 * flen)) return sp_ec_fail("EC_POINT_mul");
  sp_ffi_bin_len = (int)n;
  return sp_ec_pub_buf;
}

/* ECDH: the X coordinate of d * peer, field-width. That is the raw shared
   secret every protocol here means -- ECDH_compute_key's default KDF is the
   identity, so this is the same answer CRuby's dh_compute_key returns. */
const char *sp_ec_dh(const char *curve, const char *priv, const char *peer) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *d = NULL;
  EC_POINT *pt = NULL, *shared = NULL;
  unsigned char oct[SP_EC_MAX_POINT];
  size_t n = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) return sp_ec_fail("unknown or unsupported curve");
  if (!(d = sp_ec_scalar(g, priv, flen))) {
    EC_GROUP_free(g);
    return sp_ec_fail("private key is not a scalar on this curve");
  }
  if (!(pt = sp_ec_peer(g, peer, flen))) {
    BN_clear_free(d); EC_GROUP_free(g);
    return sp_ec_fail("peer public key is not a point on this curve");
  }
  if ((shared = EC_POINT_new(g)) && EC_POINT_mul(g, shared, NULL, pt, d, NULL) &&
      !EC_POINT_is_at_infinity(g, shared))
    n = EC_POINT_point2oct(g, shared, POINT_CONVERSION_UNCOMPRESSED,
                           oct, sizeof oct, NULL);

  EC_POINT_free(shared);
  EC_POINT_free(pt);
  BN_clear_free(d);
  EC_GROUP_free(g);
  if (n != (size_t)(1 + 2 * flen)) return sp_ec_fail("ECDH failed");
  memcpy(sp_ec_dh_buf, oct + 1, (size_t)flen);
  OPENSSL_cleanse(oct, sizeof oct);
  sp_ffi_bin_len = flen;
  return sp_ec_dh_buf;
}

/* ---------- AES-GCM, one shot ----------

   Stateless by construction, which is the whole design (#4221): the Ruby
   Cipher above this holds the key, iv, aad and the buffered plaintext as
   ordinary ivars, and calls in here exactly once, at #final, when every input
   exists at the same moment. Nothing on this side survives the call, so there
   is no context to free and an abandoned Cipher -- an exception mid-stream, a
   decrypt raising on a bad tag from attacker input -- costs nothing. It is the
   same property the EC surface has, arrived at the same way.

   Both entry points clear the shared error slot on the way in and leave a
   reason in it on the way out; the Ruby side raises on a non-empty
   last_error rather than on an empty result, because an empty result is a
   legitimate answer here (the plaintext of an empty message).

   The tag rides at the END of the encrypt result, ciphertext || tag, so one
   return value carries both and the Ruby side splits at -16. GCM's tag is
   always 16 bytes here; CRuby can be asked for a shorter one, and truncated
   tags weaken the authentication for no benefit this package has a use for. */

#define SP_GCM_TAG 16

/* aes-128-gcm / aes-256-gcm only, by name, per #4221: a mode that has not been
   run against vectors must not start working by accident because EVP knows it.
   The Ruby side refuses other names before reaching here; this is the second
   half of the same gate, for a caller that arrives another way. */
static const EVP_CIPHER *sp_gcm_by_name(const char *name) {
  /* strcmp, not a length-aware compare: a cipher name is a C string by
     construction -- it has no embedded NUL and the literals here have none. */
  if (strcmp(name, "aes-128-gcm") == 0) return EVP_aes_128_gcm();
  if (strcmp(name, "aes-256-gcm") == 0) return EVP_aes_256_gcm();
  return NULL;
}

/* Shared setup: the cipher, the key and an IV of any length GCM accepts.
   EVP_CTRL_AEAD_SET_IVLEN is set unconditionally rather than only for a
   non-default length -- it is free, and it means a 12-byte IV takes the same
   path as any other rather than a second one that is never tested. */
static EVP_CIPHER_CTX *sp_gcm_begin(const char *name, const char *key,
                                    const char *iv, int enc) {
  const EVP_CIPHER *c = sp_gcm_by_name(name);
  EVP_CIPHER_CTX *ctx;
  if (!c) { sp_ssl_note("unsupported cipher"); return NULL; }
  if ((int)sp_str_byte_len(key) != EVP_CIPHER_key_length(c)) {
    sp_ssl_note("key is not the cipher's key length");
    return NULL;
  }
  if (sp_str_byte_len(iv) == 0) { sp_ssl_note("iv must not be empty"); return NULL; }
  if (!(ctx = EVP_CIPHER_CTX_new())) { sp_ssl_note("EVP_CIPHER_CTX_new"); return NULL; }
  if (!EVP_CipherInit_ex(ctx, c, NULL, NULL, NULL, enc) ||
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                           (int)sp_str_byte_len(iv), NULL) ||
      !EVP_CipherInit_ex(ctx, NULL, NULL, (const unsigned char *)key,
                         (const unsigned char *)iv, enc)) {
    sp_ssl_note("EVP_CipherInit_ex");
    EVP_CIPHER_CTX_free(ctx);
    return NULL;
  }
  return ctx;
}

/* Additional authenticated data: covered by the tag, absent from the output.
   An empty aad is not "no aad" -- it is an aad of zero bytes, and GCM treats
   the two identically, so no branch is needed. */
static int sp_gcm_aad(EVP_CIPHER_CTX *ctx, const char *aad) {
  size_t n = sp_str_byte_len(aad);
  int out = 0;
  if (n == 0) return 1;
  return EVP_CipherUpdate(ctx, NULL, &out, (const unsigned char *)aad, (int)n);
}

/* ciphertext || tag. GCM is a stream mode, so the ciphertext is exactly as
   long as the plaintext and the allocation is known before any work. */
const char *sp_aes_gcm_encrypt(const char *name, const char *key, const char *iv,
                               const char *aad, const char *plain) {
  size_t pn = sp_str_byte_len(plain);
  EVP_CIPHER_CTX *ctx;
  char *out;
  int n1 = 0, n2 = 0, ok;

  sp_ssl_errbuf[0] = 0;
  if (!(ctx = sp_gcm_begin(name, key, iv, 1))) return sp_str_from_bytes("", 0);

  out = sp_str_alloc(pn + SP_GCM_TAG);
  ok = sp_gcm_aad(ctx, aad) &&
       EVP_CipherUpdate(ctx, (unsigned char *)out, &n1,
                        (const unsigned char *)plain, (int)pn) &&
       EVP_CipherFinal_ex(ctx, (unsigned char *)out + n1, &n2) &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, SP_GCM_TAG,
                           (unsigned char *)out + pn);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok || (size_t)(n1 + n2) != pn) {
    sp_ssl_note("AES-GCM encrypt failed");
    return sp_str_from_bytes("", 0);
  }
  out[pn + SP_GCM_TAG] = 0;
  sp_str_set_len(out, pn + SP_GCM_TAG);
  return sp_str_as_binary(out);
}

/* On success, 0x01 || plaintext; 0x00 for a tag failure; empty for other errors.
   A tag that does not verify is a failure like any other and must
   not answer plaintext: EVP_CipherFinal_ex performs that check, which is why
   the tag is set before it and the result of that call is not ignored.

   THE STATUS BYTE IS WHY THIS DOES NOT ANSWER THE PLAINTEXT PLAINLY. An empty
   string is a legitimate plaintext -- the message of zero bytes -- so "empty"
   cannot also mean "forged", and a verdict read from a separate call is a
   verdict that can be wrong: another thread's success can clear the error slot
   between the two, and a forged message then arrives as valid empty plaintext.
   Making the answer carry its own verdict removes the window rather than
   narrowing it, and one byte is a cheap price for an authentication result
   that cannot be read out of order. (The encrypt side needs no such byte: its
   answer always carries a 16-byte tag, so empty is unambiguous there.) */
const char *sp_aes_gcm_decrypt(const char *name, const char *key, const char *iv,
                               const char *aad, const char *ct, const char *tag) {
  size_t cn = sp_str_byte_len(ct);
  EVP_CIPHER_CTX *ctx;
  char *out;
  int n1 = 0, n2 = 0, ok, auth_failed = 0;

  sp_ssl_errbuf[0] = 0;
  if (sp_str_byte_len(tag) != SP_GCM_TAG) {
    sp_ssl_note("auth tag must be 16 bytes");
    return sp_str_from_bytes("", 0);
  }
  if (!(ctx = sp_gcm_begin(name, key, iv, 0))) return sp_str_from_bytes("", 0);

  out = sp_str_alloc(cn + 1);
  out[0] = 0x01;
  ok = sp_gcm_aad(ctx, aad) &&
       EVP_CipherUpdate(ctx, (unsigned char *)out + 1, &n1,
                        (const unsigned char *)ct, (int)cn) &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, SP_GCM_TAG,
                           (void *)tag);
  if (ok) {
    ok = EVP_CipherFinal_ex(ctx, (unsigned char *)out + 1 + n1, &n2);
    auth_failed = !ok;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok || (size_t)(n1 + n2) != cn) {
    /* Never hand back a partial plaintext from a message that did not
       authenticate, not even to a caller that would discard it. */
    OPENSSL_cleanse(out, cn + 1);
    sp_ssl_note("AES-GCM decrypt failed: auth tag did not verify");
    return sp_str_from_bytes(auth_failed ? "\0" : "", auth_failed);
  }
  out[cn + 1] = 0;
  sp_str_set_len(out, cn + 1);
  return sp_str_as_binary(out);
}

/* ---------- ECDSA over the same curves ----------

   Signing is the one place in this file where no single API spans its version
   floor. ECDSA_do_sign is OSSL_DEPRECATEDIN_3_0, and the 3.0 replacement --
   EVP_PKEY_fromdata into EVP_DigestSign -- did not exist at 1.1.0, so a shim
   would mean two signing paths of which only one is ever compiled on a given
   host. A crypto path that never runs where the tests run is a crypto path
   nobody has checked, so this takes the deprecated call on every version and
   silences the warning at the one site that earns it. If the floor moves to
   3.0 the EVP form is a small, single-path replacement.

   Everything either side of that call is current: ECDSA_SIG_get0 and
   BN_bn2binpad are not deprecated in 3.x and are present at 1.1.0.

   sign_raw signatures cross the boundary as raw r || s, two field-width integers.
   dsa_sign_asn1 uses DER instead. ES256 (RFC 7518) names the raw encoding, and it is the one place
   the two shapes are silently interchangeable: a DER signature is a
   well-formed String that every JWT verifier rejects, so handing one out
   would fail remotely, in someone else's stack, long after the call. The
   conversion is here rather than in each caller for exactly that reason. */

static SP_TLS char sp_ecdsa_sig_buf[2 * SP_EC_MAX_FIELD + 10];

/* Sign a digest that has already been computed: ECDSA signs a hash, and which
   hash is the caller's business (SHA-256 for ES256). A digest longer than the
   curve's order is truncated by the standard, which is OpenSSL's behaviour and
   not something to reimplement here. */
const char *sp_ecdsa_sign(const char *curve, const char *priv, const char *digest, sp_int der) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *d = NULL;
  EC_KEY *k = NULL;
  ECDSA_SIG *sig = NULL;
  const BIGNUM *r, *s;
  int ok = 0, length = 2 * flen;

  sp_ssl_errbuf[0] = 0;
  if (!g) return sp_ec_fail("unknown or unsupported curve");
  if (sp_str_byte_len(digest) > INT_MAX) { EC_GROUP_free(g); return sp_ec_fail("digest too long"); }
  if (!(d = sp_ec_scalar(g, priv, flen))) {
    EC_GROUP_free(g);
    return sp_ec_fail("private key is not a scalar on this curve");
  }

#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if ((k = EC_KEY_new()) && EC_KEY_set_group(k, g) && EC_KEY_set_private_key(k, d)) {
    EC_POINT *pub = EC_POINT_new(g);
    if (pub && EC_POINT_mul(g, pub, d, NULL, NULL, NULL) && EC_KEY_set_public_key(k, pub))
      sig = ECDSA_do_sign((const unsigned char *)digest,
                          (int)sp_str_byte_len(digest), k);
    EC_POINT_free(pub);
  }
  if (k) EC_KEY_free(k);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif

  if (sig) {
    if (der) {
      unsigned char *out = (unsigned char *)sp_ecdsa_sig_buf;
      length = i2d_ECDSA_SIG(sig, NULL);
      ok = length > 0 && (size_t)length <= sizeof sp_ecdsa_sig_buf && i2d_ECDSA_SIG(sig, &out) == length;
    } else {
      ECDSA_SIG_get0(sig, &r, &s);
      ok = BN_bn2binpad(r, (unsigned char *)sp_ecdsa_sig_buf, flen) == flen &&
           BN_bn2binpad(s, (unsigned char *)sp_ecdsa_sig_buf + flen, flen) == flen;
    }
    ECDSA_SIG_free(sig);
  }
  BN_clear_free(d);
  EC_GROUP_free(g);
  if (!ok) return sp_ec_fail("ECDSA signing failed");
  sp_ffi_bin_len = length;
  return sp_ecdsa_sig_buf;
}

/* Verify raw r || s or DER against a public point. Only 1 verifies; 0 is a
   mismatch, and a DER decode or verification error returns -1 so Ruby can
   raise. None of those failures may leave errors queued for another call. */
sp_int sp_ecdsa_verify(const char *curve, const char *pub, const char *digest,
                       const char *sig_bytes, sp_int der) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  EC_POINT *pt = NULL;
  EC_KEY *k = NULL;
  ECDSA_SIG *sig = NULL;
  BIGNUM *r = NULL, *s = NULL;
  int rc = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) { sp_ssl_note("unknown or unsupported curve"); return 0; }
  if (sp_str_byte_len(digest) > INT_MAX || sp_str_byte_len(sig_bytes) > INT_MAX) {
    EC_GROUP_free(g);
    ERR_clear_error();
    sp_ssl_note(der ? "invalid DER signature" : "signature did not verify");
    return der ? -1 : 0;
  }
  if (!der && sp_str_byte_len(sig_bytes) != (size_t)(2 * flen)) {
    sp_ssl_note("signature must be r || s, two field-width integers");
    EC_GROUP_free(g);
    return 0;
  }
  if (!(pt = sp_ec_peer(g, pub, flen))) {
    sp_ssl_note("public key is not a point on this curve");
    EC_GROUP_free(g);
    return 0;
  }

  /* ECDSA_SIG_set0 takes ownership of both BIGNUMs on success, so they are
     freed through the signature and not again here. */
  if (!der) {
    r = BN_bin2bn((const unsigned char *)sig_bytes, flen, NULL);
    s = BN_bin2bn((const unsigned char *)sig_bytes + flen, flen, NULL);
    sig = ECDSA_SIG_new();
    if (r && s && sig && ECDSA_SIG_set0(sig, r, s)) { r = NULL; s = NULL; }
    else { ECDSA_SIG_free(sig); sig = NULL; }
  }
  if (der || sig) {
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if ((k = EC_KEY_new()) && EC_KEY_set_group(k, g) && EC_KEY_set_public_key(k, pt))
      /* ECDSA_verify checks the complete canonical DER, including its end. */
      rc = der ? ECDSA_verify(0, (const unsigned char *)digest, (int)sp_str_byte_len(digest),
                             (const unsigned char *)sig_bytes, (int)sp_str_byte_len(sig_bytes), k)
               : ECDSA_do_verify((const unsigned char *)digest, (int)sp_str_byte_len(digest), sig, k) == 1;
    if (k) EC_KEY_free(k);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  }

  if (sig) ECDSA_SIG_free(sig);
  BN_free(r);
  BN_free(s);
  EC_POINT_free(pt);
  EC_GROUP_free(g);
  if (rc < 0) {
    ERR_clear_error();
    sp_ssl_note("invalid DER signature");
  }
  else if (!rc) sp_ssl_note("signature did not verify");
  return rc;
}

/* Ed25519 is present from OpenSSL 1.1.1. Older supported libraries report
   the missing algorithm without referring to an API they cannot link. */
const char *sp_ed25519_sign(const char *seed, const char *data) {
#if defined(EVP_PKEY_ED25519)
  static SP_TLS unsigned char signature[64];
  EVP_PKEY *key;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  size_t length = sizeof signature;
  int ok;
  sp_ssl_errbuf[0] = 0;
  key = seed && sp_str_byte_len(seed) == 32 ?
    EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, (const unsigned char *)seed, 32) : NULL;
  ok = key && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, key) == 1 &&
       EVP_DigestSign(ctx, signature, &length, (const unsigned char *)data, sp_str_byte_len(data)) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  if (!ok || length != sizeof signature) return sp_ec_fail("Ed25519 signing failed");
  sp_ffi_bin_len = (int)length;
  return (const char *)signature;
#else
  return sp_ec_fail("Ed25519 is unavailable in the linked OpenSSL");
#endif
}

sp_int sp_ed25519_verify(const char *seed, const char *signature, const char *data, sp_int digest) {
#if defined(EVP_PKEY_ED25519)
  sp_ssl_errbuf[0] = 0;
  const EVP_MD *md = digest ? ((sp_verify_digest *)(intptr_t)digest)->md : NULL;
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                            (const unsigned char *)seed, sp_str_byte_len(seed));
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  int result = -1;
  int initialized = key && ctx && EVP_DigestVerifyInit(ctx, NULL, md, NULL, key) == 1;
  if (initialized)
    result = EVP_DigestVerify(ctx, (const unsigned char *)signature, sp_str_byte_len(signature),
                              (const unsigned char *)data, sp_str_byte_len(data));
  else {
    const char *reason, *detail = NULL;
    int flags = 0;
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
    unsigned long error = ERR_get_error_all(NULL, NULL, NULL, &detail, &flags);
#else
    unsigned long error = ERR_get_error_line_data(NULL, NULL, &detail, &flags);
#endif
    reason = ERR_reason_error_string(error);
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "EVP_DigestVerifyInit: %s%s%s%s",
             reason ? reason : "verification failed",
             detail && (flags & ERR_TXT_STRING) ? " (" : "",
             detail && (flags & ERR_TXT_STRING) ? detail : "",
             detail && (flags & ERR_TXT_STRING) ? ")" : "");
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  ERR_clear_error();
  if (result < 0 && initialized) sp_ssl_note("Ed25519 verification failed");
  return result;
#else
  sp_ssl_note("Ed25519 is unavailable in the linked OpenSSL");
  return -1;
#endif
}

/* Is this a usable public key on this curve? The same test sp_ec_dh makes
   before it multiplies, exposed so a public-only key can be refused where it
   is built rather than at first use. */
sp_int sp_ec_check_point(const char *curve, const char *pub) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  EC_POINT *pt;

  sp_ssl_errbuf[0] = 0;
  if (!g) { sp_ssl_note("unknown or unsupported curve"); return 0; }
  pt = sp_ec_peer(g, pub, flen);
  EC_POINT_free(pt);
  EC_GROUP_free(g);
  if (!pt) { sp_ssl_note("public key is not a point on this curve"); return 0; }
  return 1;
}

/* ---------- PKey.read: an EC or RSA key out of PEM or DER ----------
   The key is parsed here, where it crosses the boundary, and what comes back
   is what the Ruby key is made of. For an EC key that is the curve's name and
   the key's bytes: one byte for the half it carries (1 private, 0 public),
   one for the length of the curve's name, one for its point-conversion form,
   one for its SEC1 encoding flags, the name, and then the private scalar,
   field-width, or the public point in uncompressed form. An RSA key
   answers as sp_pkey_rsa_answer lays it out. openssl/pkey.rb builds the key
   from those, so a key read here is the same object as one built from
   bytes. */
static SP_TLS char sp_pkey_read_buf[4 + 64 + SP_EC_MAX_POINT];

/* No passphrase is ever supplied. OpenSSL's default callback would prompt on
   the terminal for an encrypted PEM; this makes that PEM a key that does not
   parse. */
static int sp_pkey_no_pass(char *buf, int size, int rwflag, void *u) {
  (void)buf; (void)size; (void)rwflag; (void)u;
  return -1;
}

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
/* One decoder try, as CRuby's ossl_pkey_read makes it: a decoder of its own
   for the input type and selection, moved on PEM block by PEM block. */
static EVP_PKEY *sp_pkey_decode(BIO *bio, const char *type, int selection) {
  EVP_PKEY *k = NULL;
  OSSL_DECODER_CTX *d = OSSL_DECODER_CTX_new_for_pkey(&k, type, NULL, NULL,
                                                      selection, NULL, NULL);
  int pos = 0, at;

  if (d) {
    OSSL_DECODER_CTX_set_pem_password_cb(d, sp_pkey_no_pass, NULL);
    while (OSSL_DECODER_from_bio(d, bio) != 1 && !BIO_eof(bio)) {
      at = BIO_tell(bio);
      if (at < 0 || at <= pos) break;
      pos = at;
    }
    OSSL_DECODER_CTX_free(d);
  }
  (void)BIO_reset(bio);
  return k;
}
#endif

/* CRuby's last tries, for input none of the above reads: parameters with no
   key in them (DH PARAMETERS, EC PARAMETERS), and a public key in another
   form (a PKCS#1 RSA PUBLIC KEY). CRuby answers a key for those, so they are
   read the same way here, to be named in the error rather than called
   unparsable. On OpenSSL 3.0 and later CRuby asks the decoder for parameters
   and then for a public key, never for any selection at once, which fails on
   some 3.0 and 3.1 releases. Before 3.0 its last try is
   PEM_read_bio_Parameters, which does not read an RSA PUBLIC KEY, so that is
   read here with PEM_read_bio_RSAPublicKey: the error names RSA on every
   version. */
static EVP_PKEY *sp_pkey_parse_any(const char *data, long n) {
  EVP_PKEY *k = NULL;
  BIO *bio = BIO_new_mem_buf(data, (int)n);

  if (!bio) return NULL;
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  static const char *const types[] = { "DER", "PEM" };
  for (int i = 0; i < 2 && !k; i++)
    if (!(k = sp_pkey_decode(bio, types[i], EVP_PKEY_KEY_PARAMETERS)))
      k = sp_pkey_decode(bio, types[i], EVP_PKEY_PUBLIC_KEY);
#else
  RSA *rsa;

  if (!(k = PEM_read_bio_Parameters(bio, NULL))) {
    (void)BIO_reset(bio);
    if ((rsa = PEM_read_bio_RSAPublicKey(bio, NULL, sp_pkey_no_pass, NULL)) &&
        (!(k = EVP_PKEY_new()) || EVP_PKEY_assign_RSA(k, rsa) != 1)) {
      EVP_PKEY_free(k);
      k = NULL;
      RSA_free(rsa);
    }
  }
#endif
  BIO_free(bio);
  return k;
}

/* DER before PEM, and a private key before a public one: the order CRuby's
   PKey.read tries them in. *priv says which half was found, and is -1 for
   what sp_pkey_parse_any found, where the key has to be asked. */
static EVP_PKEY *sp_pkey_parse(const char *data, int *priv) {
  long n = (long)sp_str_byte_len(data);
  const unsigned char *p = (const unsigned char *)data;
  EVP_PKEY *k = NULL;
  BIO *bio;

  if ((k = d2i_AutoPrivateKey(NULL, &p, n))) { *priv = 1; return k; }
  p = (const unsigned char *)data;
  if ((k = d2i_PUBKEY(NULL, &p, n))) { *priv = 0; return k; }
  if ((bio = BIO_new_mem_buf(data, (int)n))) {
    if ((k = PEM_read_bio_PrivateKey(bio, NULL, sp_pkey_no_pass, NULL))) *priv = 1;
    BIO_free(bio);
  }
  if (!k && (bio = BIO_new_mem_buf(data, (int)n))) {
    if ((k = PEM_read_bio_PUBKEY(bio, NULL, sp_pkey_no_pass, NULL))) *priv = 0;
    BIO_free(bio);
  }
  if (!k && (k = sp_pkey_parse_any(data, n))) *priv = -1;
  return k;
}

/* An RSA key for PKey.read: a byte for the half it carries (3 private, 2
   public), the modulus and the exponent each as a 4-byte big-endian length
   and its bytes, and for a private key its PKCS#1 DER after them, which holds
   the CRT components and is what sp_rsa_sign and an accept read back. Into a
   buffer of its own, grown to fit: an RSA key does not fit the EC answer's.
   *priv is -1 for a key sp_pkey_parse_any found, and the key is asked. */
static SP_TLS sp_x509_out sp_pkey_rsa_out;

static const char *sp_pkey_rsa_answer(EVP_PKEY *k, int priv) {
  const BIGNUM *n = NULL, *e = NULL, *d = NULL;
  int nl, el, dl = 0;
  unsigned char *o, *q;
  size_t total;
  RSA *rsa;

#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (!(rsa = EVP_PKEY_get1_RSA(k))) return sp_ec_fail("RSA key without its key bytes");
  RSA_get0_key(rsa, &n, &e, &d);
  if (priv < 0) priv = d != NULL;
  if (!n || !e || (priv && !d)) { RSA_free(rsa); return sp_ec_fail("RSA key without its key bytes"); }
  nl = BN_num_bytes(n);
  el = BN_num_bytes(e);
  if (priv && (dl = i2d_RSAPrivateKey(rsa, NULL)) <= 0) {
    RSA_free(rsa);
    return sp_ec_fail("i2d_RSAPrivateKey");
  }
  total = 1 + 4 + (size_t)nl + 4 + (size_t)el + (size_t)dl;
  if (!(o = (unsigned char *)sp_x509_room(&sp_pkey_rsa_out, total))) {
    RSA_free(rsa);
    return sp_ec_fail("out of memory");
  }
  o[0] = (unsigned char)(priv ? 3 : 2);
  o[1] = (unsigned char)(nl >> 24); o[2] = (unsigned char)(nl >> 16);
  o[3] = (unsigned char)(nl >> 8);  o[4] = (unsigned char)nl;
  BN_bn2bin(n, o + 5);
  q = o + 5 + nl;
  q[0] = (unsigned char)(el >> 24); q[1] = (unsigned char)(el >> 16);
  q[2] = (unsigned char)(el >> 8);  q[3] = (unsigned char)el;
  BN_bn2bin(e, q + 4);
  q += 4 + el;
  if (dl && i2d_RSAPrivateKey(rsa, &q) != dl) {
    OPENSSL_cleanse(o, total);
    RSA_free(rsa);
    return sp_ec_fail("i2d_RSAPrivateKey");
  }
  RSA_free(rsa);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  sp_ffi_bin_len = (int)total;
  return (const char *)o;
}

/* What CRuby would have answered for a key this does not read, for the
   message: its class name where it has one of its own. */
static const char *sp_pkey_kind(int id) {
  switch (id) {
    case EVP_PKEY_RSA: return "RSA";
    case EVP_PKEY_DSA: return "DSA";
    case EVP_PKEY_DH:  return "DH";
    default: { const char *sn = OBJ_nid2sn(id); return sn ? sn : "unknown"; }
  }
}

/* A provider-backed key's legacy EC_KEY copy may normalize its point form.
   Read the flag from its SPKI instead, through APIs also present in 1.1.0. */
static int sp_pkey_point_form(EVP_PKEY *key) {
  X509_PUBKEY *spki = NULL;
  const unsigned char *point = NULL;
  int length = 0, form = 0;
  if (X509_PUBKEY_set(&spki, key) &&
      X509_PUBKEY_get0_param(NULL, &point, &length, NULL, spki) && length > 0)
    form = point[0] & ~1;
  X509_PUBKEY_free(spki);
  return form;
}

const char *sp_pkey_read(const char *data) {
  unsigned char *out = (unsigned char *)sp_pkey_read_buf;
  const EC_KEY *ec;
  const EC_GROUP *g = NULL;
  const char *curve = NULL;
  size_t clen = 0, n = 0;
  int priv = 0, flen = 0, nid = NID_undef, point_form = 0;
  EVP_PKEY *k;

  sp_ssl_errbuf[0] = 0;
  /* BIO_new_mem_buf takes an int length: refused rather than narrowed, the
     bound the X509 calls use. */
  if (sp_str_byte_len(data) > (size_t)INT_MAX / 2) return sp_ec_fail("key data too large");
  if (!(k = sp_pkey_parse(data, &priv))) {
    ERR_clear_error();
    return sp_ec_fail("Could not parse PKey");
  }
  if (EVP_PKEY_base_id(k) == EVP_PKEY_RSA) {
    const char *r = sp_pkey_rsa_answer(k, priv);
    EVP_PKEY_free(k);
    ERR_clear_error();
    return r;
  }
  if (EVP_PKEY_base_id(k) != EVP_PKEY_EC) {
    char why[128];
    snprintf(why, sizeof why, "unsupported key type: %s (this package reads EC and RSA keys)",
             sp_pkey_kind(EVP_PKEY_base_id(k)));
    EVP_PKEY_free(k);
    ERR_clear_error();
    return sp_ec_fail(why);
  }
  /* EVP_PKEY_get0_EC_KEY and the EC_KEY getters are deprecated in OpenSSL
     3.0, and the replacements (EVP_PKEY_get_bn_param) are not in the 1.1.0
     this file compiles against; the same trade sp_rsa_verify makes. */
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if ((ec = EVP_PKEY_get0_EC_KEY(k)) && (g = EC_KEY_get0_group(ec)))
    nid = EC_GROUP_get_curve_name(g);
  if (priv < 0) priv = ec && EC_KEY_get0_private_key(ec) ? 1 : 0;
  if (nid != NID_undef && (curve = OBJ_nid2sn(nid)) && (clen = strlen(curve)) < 64) {
    flen = (EC_GROUP_get_degree(g) + 7) / 8;
    if (flen <= 0 || flen > SP_EC_MAX_FIELD) curve = NULL;
  }
  if (curve && (point_form = sp_pkey_point_form(k))) {
    out[0] = (unsigned char)priv;
    out[1] = (unsigned char)clen;
    out[2] = (unsigned char)point_form;
    out[3] = (unsigned char)(EC_KEY_get_enc_flags(ec) & EC_PKEY_NO_PUBKEY);
    memcpy(out + 4, curve, clen);
    if (priv) {
      const BIGNUM *d = EC_KEY_get0_private_key(ec);
      if (d && BN_bn2binpad(d, out + 4 + clen, flen) == flen) n = 4 + clen + (size_t)flen;
    }
    else {
      const EC_POINT *q = EC_KEY_get0_public_key(ec);
      size_t want = (size_t)(1 + 2 * flen);
      if (q && EC_POINT_point2oct(g, q, POINT_CONVERSION_UNCOMPRESSED,
                                  out + 4 + clen, want, NULL) == want)
        n = 4 + clen + want;
    }
  }
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  EVP_PKEY_free(k);
  if (!n) {
    OPENSSL_cleanse(sp_pkey_read_buf, sizeof sp_pkey_read_buf);
    ERR_clear_error();
    return sp_ec_fail(curve ? "EC key without its key bytes" : "EC key on a curve without a name");
  }
  ERR_clear_error();
  sp_ffi_bin_len = (int)n;
  return sp_pkey_read_buf;
}

/* RSA signature verification over a public key given as its two halves, the
   big-endian modulus and exponent bytes: the form every protocol that names
   an RSA key carries it in (a COSE key's n and e, a JWK's). `md_name` is the
   digest name ("SHA256", "sha384", ...) and the data is hashed here.
   `pss` 0 is PKCS#1 v1.5; 1 is PSS with MGF1 over `mgf1_name` and the salt
   length `saltlen` (-2 read from the signature, -1 the digest's length, else
   that many bytes), which is CRuby's
   `verify_pss(md, sig, data, salt_length:, mgf1_hash:)`.
   Answers 1 for a good signature, 0 otherwise (the reason in last_error),
   -1 for an unknown digest and -2 for an unknown MGF1 digest. */
sp_int sp_rsa_verify(const char *n_bytes, const char *e_bytes, const char *md_name,
                     const char *data, const char *sig, sp_int pss,
                     const char *mgf1_name, sp_int saltlen) {
  const EVP_MD *md = EVP_get_digestbyname(md_name);
  const EVP_MD *mgf1 = pss ? EVP_get_digestbyname(mgf1_name) : md;
  BIGNUM *n = NULL, *e = NULL;
  RSA *rsa = NULL;
  EVP_PKEY *pkey = NULL;
  EVP_MD_CTX *ctx = NULL;
  EVP_PKEY_CTX *pctx = NULL;
  int rc = 0;

  sp_ssl_errbuf[0] = 0;
  if (!md) { sp_ssl_note("unsupported digest"); return -1; }
  if (!mgf1) { sp_ssl_note("unsupported MGF1 digest"); return -2; }
  n = BN_bin2bn((const unsigned char *)n_bytes, (int)sp_str_byte_len(n_bytes), NULL);
  e = BN_bin2bn((const unsigned char *)e_bytes, (int)sp_str_byte_len(e_bytes), NULL);
  if (!n || !e || BN_is_zero(n) || BN_is_zero(e)) {
    BN_free(n); BN_free(e);
    sp_ssl_note("RSA key needs a modulus and an exponent");
    return 0;
  }
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  /* RSA_set0_key takes ownership of n and e on success. */
  if ((rsa = RSA_new()) && RSA_set0_key(rsa, n, e, NULL)) {
    n = NULL; e = NULL;
    if ((pkey = EVP_PKEY_new()) && EVP_PKEY_assign_RSA(pkey, rsa)) rsa = NULL;
  }
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  if (pkey && !rsa && (ctx = EVP_MD_CTX_new()) &&
      EVP_DigestVerifyInit(ctx, &pctx, md, NULL, pkey) == 1 &&
      (!pss || (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) > 0 &&
                EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, (int)saltlen) > 0 &&
                EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf1) > 0))) {
    rc = EVP_DigestVerify(ctx, (const unsigned char *)sig, sp_str_byte_len(sig),
                          (const unsigned char *)data, sp_str_byte_len(data)) == 1;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  RSA_free(rsa);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  BN_free(n);
  BN_free(e);
  ERR_clear_error();
  if (!rc) sp_ssl_note("signature did not verify");
  return rc;
}

/* An EVP_PKEY from the PKCS#1 DER a private PKey::RSA holds, or NULL: what
   sp_rsa_sign signs with, and what a context's RSA key is loaded from. */
static EVP_PKEY *sp_rsa_evp_private(const char *der) {
  const unsigned char *p = (const unsigned char *)der;
  size_t len = sp_str_byte_len(der);

  if (len == 0 || len > (size_t)INT_MAX) return NULL;
  return d2i_PrivateKey(EVP_PKEY_RSA, NULL, &p, (long)len);
}

/* RSA signing, the mirror of sp_rsa_verify, with the private key as the
   PKCS#1 DER PKey.read answers: PKCS#1 v1.5 (pss 0), or PSS with MGF1 over
   `mgf1_name` and the salt length `saltlen` (-1 the digest's length, -3 the
   most that fits, else that many bytes), which are CRuby's
   `sign(md, data)` and `sign_pss(md, data, salt_length:, mgf1_hash:)`.
   Answers the signature, or "" with the reason in last_error, which starts
   "unsupported digest algorithm" when a digest name resolves to nothing. */
static SP_TLS sp_x509_out sp_rsa_sig_out;

const char *sp_rsa_sign(const char *der, const char *md_name, const char *data, sp_int pss,
                        const char *mgf1_name, sp_int saltlen) {
  const EVP_MD *md = EVP_get_digestbyname(md_name);
  const EVP_MD *mgf1 = pss ? EVP_get_digestbyname(mgf1_name) : md;
  EVP_PKEY *pkey = NULL;
  EVP_MD_CTX *ctx = NULL;
  EVP_PKEY_CTX *pctx = NULL;
  size_t n = 0;
  int ok = 0;

  sp_ssl_errbuf[0] = 0;
  if (!md || !mgf1) {
    snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "unsupported digest algorithm: %s",
             md ? mgf1_name : md_name);
    sp_ffi_bin_len = 0;
    return "";
  }
  if (!(pkey = sp_rsa_evp_private(der))) { ERR_clear_error(); return sp_ec_fail("RSA private key"); }
  if ((ctx = EVP_MD_CTX_new()) &&
      EVP_DigestSignInit(ctx, &pctx, md, NULL, pkey) == 1 &&
      (!pss || (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) > 0 &&
                EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, (int)saltlen) > 0 &&
                EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, mgf1) > 0)) &&
      EVP_DigestSign(ctx, NULL, &n, (const unsigned char *)data, sp_str_byte_len(data)) == 1 &&
      n <= (size_t)INT_MAX && sp_x509_room(&sp_rsa_sig_out, n) &&
      EVP_DigestSign(ctx, (unsigned char *)sp_rsa_sig_out.p, &n,
                     (const unsigned char *)data, sp_str_byte_len(data)) == 1)
    ok = 1;
  if (!ok) sp_ssl_note("EVP_DigestSign");
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  ERR_clear_error();
  if (!ok) { sp_ffi_bin_len = 0; return ""; }
  sp_ffi_bin_len = (int)n;
  return sp_rsa_sig_out.p;
}

/* A public RSA key's to_der, as CRuby answers it: the SubjectPublicKeyInfo
   DER a PUBLIC KEY PEM holds, from the modulus and exponent bytes. "" with
   the reason in last_error. */
static SP_TLS sp_x509_out sp_rsa_pub_out;

const char *sp_rsa_public_der(const char *n_bytes, const char *e_bytes) {
  BIGNUM *n = BN_bin2bn((const unsigned char *)n_bytes, (int)sp_str_byte_len(n_bytes), NULL);
  BIGNUM *e = BN_bin2bn((const unsigned char *)e_bytes, (int)sp_str_byte_len(e_bytes), NULL);
  unsigned char *q;
  RSA *rsa = NULL;
  int len = 0;

  sp_ssl_errbuf[0] = 0;
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (n && e && (rsa = RSA_new()) && RSA_set0_key(rsa, n, e, NULL)) {
    n = NULL; e = NULL;                  /* the RSA owns them now */
    if ((len = i2d_RSA_PUBKEY(rsa, NULL)) > 0 && sp_x509_room(&sp_rsa_pub_out, (size_t)len)) {
      q = (unsigned char *)sp_rsa_pub_out.p;
      if (i2d_RSA_PUBKEY(rsa, &q) != len) len = 0;
    }
    else len = 0;
  }
  RSA_free(rsa);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  BN_free(n);
  BN_free(e);
  if (len <= 0) { ERR_clear_error(); return sp_ec_fail("i2d_RSA_PUBKEY"); }
  sp_ffi_bin_len = len;
  return sp_rsa_pub_out.p;
}

/* ---------- The server half ----------

   A server's SSL_CTX is built per accept from the certificate and key bytes
   the Ruby SSLContext holds (#9341, 2a), the way a client's is built per
   connect: it lives in the connection's slot and goes with it at close, so
   nothing here outlives a connection. That costs about 120 us an accept on
   an M1, near what the handshake's own P-256 arithmetic costs, and a session
   cannot resume across two accepts, a limit the client side already has.

   `chain` is the certificates as sp_x509_load lays them out, a 4-byte
   big-endian length before each DER: the leaf, then what is sent with it.
   `curve` and `priv` are an EC key's curve name and private scalar, or for
   an RSA key an empty name and its PKCS#1 DER. All three are empty for a
   context with no certificate and key, and the handshake then fails as
   CRuby's does there, with "no shared cipher". */

/* An EVP_PKEY for SSL_CTX_use_PrivateKey, from the two things a PKey::EC
   holds. The public point is derived here, as sp_ec_public_bytes derives
   it, rather than trusted from the caller. */
static EVP_PKEY *sp_ec_evp_private(const char *curve, const char *priv) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *d = NULL;
  EC_POINT *q = NULL;
  EC_KEY *ec = NULL;
  EVP_PKEY *k = NULL;
  int ok = 0;

  if (!g) return NULL;
  if ((d = sp_ec_scalar(g, priv, flen)) && (q = EC_POINT_new(g)) &&
      EC_POINT_mul(g, q, d, NULL, NULL, NULL)) {
    /* EC_KEY is deprecated in OpenSSL 3.0, and EVP_PKEY_fromdata, which
       replaces it, is not in the 1.1.0 this file compiles against; the same
       trade sp_pkey_read makes. */
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if ((ec = EC_KEY_new()) && EC_KEY_set_group(ec, g) &&
        EC_KEY_set_private_key(ec, d) && EC_KEY_set_public_key(ec, q) &&
        (k = EVP_PKEY_new()) && EVP_PKEY_assign_EC_KEY(k, ec)) {
      ec = NULL;                       /* the EVP_PKEY owns it now */
      ok = 1;
    }
    EC_KEY_free(ec);
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
  }
  if (!ok) { EVP_PKEY_free(k); k = NULL; }
  EC_POINT_free(q);
  BN_clear_free(d);
  EC_GROUP_free(g);
  return k;
}

/* ---- Building an X509::Certificate, and X509::Name ----

   The same shape as the rest of the X509 calls: a certificate is its DER, a
   name is its DER, and each call parses what Ruby holds, changes or reads
   it, and answers bytes; no X509 * or X509_NAME * outlives the call.

   A certificate that has not been signed yet cannot be encoded (its
   signature algorithm is empty, which DER refuses), so Ruby holds its
   fields until #sign, and sp_x509_build makes the certificate from them in
   the call that signs it. A certificate that has a DER takes a field change
   as a build over that DER without a key: the old signature stays, as it
   does in CRuby, until the certificate is signed again. */

/* The reason of the last error OpenSSL queued, as CRuby words it: the
   caller's name for the step, the reason, and the error's data. */
static void sp_x509_reason(const char *what) {
  const char *data = NULL, *reason;
  int flags = 0;
  unsigned long e;
#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
  e = ERR_peek_last_error_all(NULL, NULL, NULL, &data, &flags);
#else
  e = ERR_peek_last_error_line_data(NULL, NULL, &data, &flags);
#endif
  reason = e ? ERR_reason_error_string(e) : NULL;
  if (!(flags & ERR_TXT_STRING) || (data && !*data)) data = NULL;
  if (!reason) snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s", what ? what : "unknown error");
  else if (what && data) snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s: %s (%s)", what, reason, data);
  else if (what) snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s: %s", what, reason);
  else snprintf(sp_ssl_errbuf, sizeof sp_ssl_errbuf, "%s", reason);
  ERR_clear_error();
}

static SP_TLS sp_x509_out sp_x509_build_out, sp_x509_field_out, sp_x509_name_out;

static X509_NAME *sp_x509_name_from_der(const char *der) {
  const unsigned char *p = (const unsigned char *)der;
  size_t len = sp_str_byte_len(der);
  if (len == 0) return X509_NAME_new();
  if (len > (size_t)INT_MAX) return NULL;
  return d2i_X509_NAME(NULL, &p, (long)len);
}

static const char *sp_x509_name_answer(X509_NAME *name, const char *what) {
  int n = name ? i2d_X509_NAME(name, NULL) : -1;
  unsigned char *p;
  if (n <= 0 || !sp_x509_room(&sp_x509_name_out, (size_t)n)) {
    X509_NAME_free(name);
    sp_x509_reason(what);
    sp_ffi_bin_len = 0;
    return "";
  }
  p = (unsigned char *)sp_x509_name_out.p;
  i2d_X509_NAME(name, &p);
  X509_NAME_free(name);
  sp_ffi_bin_len = n;
  return sp_x509_name_out.p;
}

/* Name.new(der): the name, encoded again, or "" if it does not parse. */
const char *sp_x509_name_der(const char *der) {
  sp_ssl_errbuf[0] = 0;
  return sp_x509_name_answer(sp_x509_name_from_der(der), NULL);
}

/* Name#add_entry: the name with one more entry at its end, of ASN.1 string
   type `type` (CRuby passes the type as it is, UTF8STRING by default). */
const char *sp_x509_name_add(const char *der, const char *field, const char *value, sp_int type) {
  X509_NAME *name;
  size_t len = sp_str_byte_len(value);

  sp_ssl_errbuf[0] = 0;
  ERR_clear_error();
  if (!(name = sp_x509_name_from_der(der))) return sp_x509_name_answer(NULL, "d2i_X509_NAME");
  if (len > (size_t)INT_MAX ||
      !X509_NAME_add_entry_by_txt(name, field, (int)type, (const unsigned char *)value, (int)len, -1, 0)) {
    X509_NAME_free(name);
    sp_x509_reason("X509_NAME_add_entry_by_txt");
    sp_ffi_bin_len = 0;
    return "";
  }
  return sp_x509_name_answer(name, "i2d_X509_NAME");
}

/* Name#to_s: X509_NAME_oneline's "/CN=x/O=y", as CRuby's to_s with no
   format. */
const char *sp_x509_name_oneline(const char *der) {
  X509_NAME *name = sp_x509_name_from_der(der);
  char *s;
  size_t n;

  sp_ssl_errbuf[0] = 0;
  if (!name || !(s = X509_NAME_oneline(name, NULL, 0))) {
    X509_NAME_free(name);
    sp_x509_reason("X509_NAME_oneline");
    sp_ffi_bin_len = 0;
    return "";
  }
  X509_NAME_free(name);
  n = strlen(s);
  if (!sp_x509_room(&sp_x509_name_out, n + 1)) { OPENSSL_free(s); sp_ffi_bin_len = 0; return ""; }
  memcpy(sp_x509_name_out.p, s, n);
  OPENSSL_free(s);
  sp_ffi_bin_len = (int)n;
  return sp_x509_name_out.p;
}

static int sp_x509_put(sp_x509_out *o, size_t *at, const void *bytes, size_t n) {
  unsigned char *p;
  if (n > (size_t)INT_MAX || !sp_x509_room(o, *at + 4 + n)) return 0;
  p = (unsigned char *)o->p + *at;
  p[0] = (unsigned char)(n >> 24);
  p[1] = (unsigned char)(n >> 16);
  p[2] = (unsigned char)(n >> 8);
  p[3] = (unsigned char)n;
  if (n) memcpy(p + 4, bytes, n);
  *at += 4 + n;
  return 1;
}

/* Name#to_a: each entry as three length-prefixed parts, its short name (or
   its OID, for one OpenSSL has no name for), its value's bytes, and its
   ASN.1 type as decimal digits. The name is found as CRuby finds it: the
   long name OBJ_obj2txt gives, and the short name of that. */
const char *sp_x509_name_entries(const char *der) {
  X509_NAME *name = sp_x509_name_from_der(der);
  size_t at = 0;
  int count;

  sp_ssl_errbuf[0] = 0;
  if (!name) { sp_x509_reason("d2i_X509_NAME"); sp_ffi_bin_len = 0; return ""; }
  count = X509_NAME_entry_count(name);
  if (!sp_x509_room(&sp_x509_name_out, 1)) { X509_NAME_free(name); sp_ffi_bin_len = 0; return ""; }
  for (int i = 0; i < count; i++) {
    const X509_NAME_ENTRY *e = X509_NAME_get_entry(name, i);
    const ASN1_STRING *v = X509_NAME_ENTRY_get_data(e);
    char longname[256], type[16];
    const char *label = longname;
    int nid;
    if (OBJ_obj2txt(longname, sizeof longname, X509_NAME_ENTRY_get_object(e), 0) <= 0) longname[0] = 0;
    if ((nid = OBJ_ln2nid(longname)) != NID_undef) label = OBJ_nid2sn(nid);
    snprintf(type, sizeof type, "%d", ASN1_STRING_type(v));
    if (!sp_x509_put(&sp_x509_name_out, &at, label, strlen(label)) ||
        !sp_x509_put(&sp_x509_name_out, &at, ASN1_STRING_get0_data(v), (size_t)ASN1_STRING_length(v)) ||
        !sp_x509_put(&sp_x509_name_out, &at, type, strlen(type))) {
      X509_NAME_free(name);
      sp_ffi_bin_len = 0;
      return "";
    }
  }
  X509_NAME_free(name);
  sp_ffi_bin_len = (int)at;
  return sp_x509_name_out.p;
}

/* Name#cmp: X509_NAME_cmp's order, as -1, 0 or 1; -2 if either does not
   parse. */
sp_int sp_x509_name_cmp(const char *a, const char *b) {
  X509_NAME *x = sp_x509_name_from_der(a), *y = sp_x509_name_from_der(b);
  int r = -2;
  if (x && y) {
    r = X509_NAME_cmp(x, y);
    r = r < 0 ? -1 : r > 0 ? 1 : 0;
  }
  X509_NAME_free(x);
  X509_NAME_free(y);
  ERR_clear_error();
  return r;
}

/* Seconds since the epoch of a civil UTC date (proleptic Gregorian), so no
   timegm is needed: Windows has none. */
static int64_t sp_x509_days_from_civil(int64_t y, unsigned m, unsigned d) {
  int64_t era;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int sp_x509_time_epoch(const ASN1_TIME *t, int64_t *out) {
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  if (!t || ASN1_STRING_length(t) == 0) return 0;
#if defined(LIBRESSL_VERSION_NUMBER)
  if (ASN1_time_parse((const char *)ASN1_STRING_get0_data(t), (size_t)ASN1_STRING_length(t), &tm, 0) == -1)
    return 0;
#elif OPENSSL_VERSION_NUMBER >= 0x10101000L
  if (!ASN1_TIME_to_tm(t, &tm)) return 0;
#else
  {
    int days = 0, secs = 0, ok;
    ASN1_TIME *epoch = ASN1_TIME_set(NULL, 0);
    ok = epoch && ASN1_TIME_diff(&days, &secs, epoch, t);
    ASN1_TIME_free(epoch);
    if (!ok) return 0;
    *out = (int64_t)days * 86400 + secs;
    return 1;
  }
#endif
  *out = sp_x509_days_from_civil((int64_t)tm.tm_year + 1900, (unsigned)tm.tm_mon + 1, (unsigned)tm.tm_mday) * 86400 +
         tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  return 1;
}

/* One field record of what Ruby hands sp_x509_build: a code byte, a 4-byte
   big-endian length and the field's bytes. */
static int sp_x509_apply(X509 *x, char code, const unsigned char *v, size_t n) {
  char num[64];
  const unsigned char *p = v;
  int ok = 0;

  if ((code == 'v' || code == 's' || code == 'b' || code == 'a') && n >= sizeof num) return 0;
  if (code == 'v' || code == 's' || code == 'b' || code == 'a') {
    memcpy(num, v, n);
    num[n] = 0;
  }
  switch (code) {
  case 'v':
    return X509_set_version(x, strtol(num, NULL, 10));
  case 's': {
    BIGNUM *bn = NULL;
    ASN1_INTEGER *ai = NULL;
    ok = BN_dec2bn(&bn, num) > 0 && (ai = BN_to_ASN1_INTEGER(bn, NULL)) && X509_set_serialNumber(x, ai);
    ASN1_INTEGER_free(ai);
    BN_free(bn);
    return ok;
  }
  case 'S':
  case 'I': {
    X509_NAME *name = n <= (size_t)INT_MAX ? d2i_X509_NAME(NULL, &p, (long)n) : NULL;
    ok = name && (code == 'S' ? X509_set_subject_name(x, name) : X509_set_issuer_name(x, name));
    X509_NAME_free(name);
    return ok;
  }
  case 'b':
  case 'a': {
    long long secs = strtoll(num, NULL, 10);
    ASN1_TIME *t = code == 'b' ? X509_getm_notBefore(x) : X509_getm_notAfter(x);
    return ASN1_TIME_set(t, (time_t)secs) != NULL;
  }
  case 'k': {
    EVP_PKEY *k = n <= (size_t)INT_MAX ? d2i_PUBKEY(NULL, &p, (long)n) : NULL;
    ok = k && X509_set_pubkey(x, k);
    EVP_PKEY_free(k);
    return ok;
  }
  }
  return 0;
}

/* The certificate in `base` (a new one when it is empty) with the fields in
   `fields` set on it, signed with the key when one is given (an EC key as
   its curve and scalar, an RSA key as an empty curve and its PKCS#1 DER,
   as SSLContext hands keys to the C) over the digest `md_name`. Answers
   the DER, or "" with the reason: an unsigned new certificate cannot be
   encoded, and answers DER's own reason for that, as CRuby's to_der does. */
const char *sp_x509_build(const char *base, const char *fields, const char *curve,
                          const char *key, const char *md_name) {
  const unsigned char *f = (const unsigned char *)fields;
  size_t flen = sp_str_byte_len(fields), at = 0;
  int signing = sp_str_byte_len(key) > 0;
  const EVP_MD *md = NULL;
  X509 *x;

  sp_ssl_errbuf[0] = 0;
  ERR_clear_error();
  /* first, as it raises DigestError for a name OpenSSL does not know, and
     nothing is allocated yet */
  if (signing) md = sp_openssl_md(md_name);
  if (sp_str_byte_len(base) == 0) x = X509_new();
  else if (!(x = sp_x509_from_der(base))) { SP_MD_FREE(md); return sp_x509_fail("d2i_X509"); }
  if (!x) { SP_MD_FREE(md); return sp_x509_fail("X509_new"); }
  while (at < flen) {
    size_t n;
    char code;
    if (flen - at < 5) { X509_free(x); SP_MD_FREE(md); return sp_x509_fail("malformed certificate fields"); }
    code = (char)f[at];
    n = ((size_t)f[at + 1] << 24) | ((size_t)f[at + 2] << 16) | ((size_t)f[at + 3] << 8) | f[at + 4];
    at += 5;
    if (n > flen - at) { X509_free(x); SP_MD_FREE(md); return sp_x509_fail("malformed certificate fields"); }
    if (!sp_x509_apply(x, code, f + at, n)) {
      X509_free(x);
      SP_MD_FREE(md);
      sp_x509_reason(NULL);
      sp_ffi_bin_len = 0;
      return "";
    }
    at += n;
  }
  if (signing) {
    EVP_PKEY *k = curve[0] ? sp_ec_evp_private(curve, key) : sp_rsa_evp_private(key);
    int ok = k && X509_sign(x, k, md) > 0;
    SP_MD_FREE(md);
    EVP_PKEY_free(k);
    if (!ok) { X509_free(x); sp_x509_reason("X509_sign"); sp_ffi_bin_len = 0; return ""; }
  }
  /* A field changed on a certificate that was read, and not signed again:
     OpenSSL 3.0 keeps the TBS encoding it read and i2d_X509 writes that one
     out, the change dropped, where 3.2 and later write the change. Encoding
     it again here gives the changed fields under the old signature on every
     version, as CRuby shows them. X509_sign does this itself. */
  else if (flen > 0 && sp_str_byte_len(base) > 0 && i2d_re_X509_tbs(x, NULL) <= 0) {
    X509_free(x);
    sp_x509_reason("i2d_re_X509_tbs");
    sp_ffi_bin_len = 0;
    return "";
  }
  {
    int n = i2d_X509(x, NULL);
    unsigned char *p;
    if (n <= 0 || !sp_x509_room(&sp_x509_build_out, (size_t)n)) {
      X509_free(x);
      sp_x509_reason(NULL);
      sp_ffi_bin_len = 0;
      return "";
    }
    p = (unsigned char *)sp_x509_build_out.p;
    i2d_X509(x, &p);
    X509_free(x);
    sp_ffi_bin_len = n;
    return sp_x509_build_out.p;
  }
}

/* One field of the certificate in `der`, for the readers: version, serial
   and the two times as decimal digits (a time OpenSSL cannot read answers
   ""), the subject and the issuer as their DER, the public key as its
   SubjectPublicKeyInfo DER. "" with the reason when the DER does not parse. */
const char *sp_x509_field(const char *der, sp_int code) {
  X509 *x = sp_x509_from_der(der);
  char num[512];
  const char *out = num;
  size_t n = 0;

  sp_ssl_errbuf[0] = 0;
  if (!x) return sp_x509_fail("d2i_X509");
  num[0] = 0;
  switch ((char)code) {
  case 'v':
    snprintf(num, sizeof num, "%ld", X509_get_version(x));
    n = strlen(num);
    break;
  case 's': {
    BIGNUM *bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(x), NULL);
    char *dec = bn ? BN_bn2dec(bn) : NULL;
    if (dec) snprintf(num, sizeof num, "%s", dec);
    OPENSSL_free(dec);
    BN_free(bn);
    n = strlen(num);
    break;
  }
  case 'b':
  case 'a': {
    int64_t secs;
    if (sp_x509_time_epoch(code == 'b' ? X509_get0_notBefore(x) : X509_get0_notAfter(x), &secs))
      snprintf(num, sizeof num, "%lld", (long long)secs);
    n = strlen(num);
    break;
  }
  case 'S':
  case 'I': {
    X509_NAME *name = (X509_NAME *)(code == 'S' ? X509_get_subject_name(x) : X509_get_issuer_name(x));
    int len = i2d_X509_NAME(name, NULL);
    unsigned char *p;
    if (len <= 0 || !sp_x509_room(&sp_x509_field_out, (size_t)len)) { X509_free(x); return sp_x509_fail("i2d_X509_NAME"); }
    p = (unsigned char *)sp_x509_field_out.p;
    i2d_X509_NAME(name, &p);
    X509_free(x);
    sp_ffi_bin_len = len;
    return sp_x509_field_out.p;
  }
  case 'k': {
    EVP_PKEY *k = X509_get0_pubkey(x);
    int len = k ? i2d_PUBKEY(k, NULL) : -1;
    unsigned char *p;
    if (len <= 0 || !sp_x509_room(&sp_x509_field_out, (size_t)len)) {
      X509_free(x);
      sp_x509_reason("X509_get_pubkey");
      sp_ffi_bin_len = 0;
      return "";
    }
    p = (unsigned char *)sp_x509_field_out.p;
    i2d_PUBKEY(k, &p);
    X509_free(x);
    sp_ffi_bin_len = len;
    return sp_x509_field_out.p;
  }
  default:
    X509_free(x);
    return sp_x509_fail("unknown certificate field");
  }
  X509_free(x);
  ERR_clear_error();
  if (!sp_x509_room(&sp_x509_field_out, n + 1)) return sp_x509_fail("out of memory");
  memcpy(sp_x509_field_out.p, out, n);
  sp_ffi_bin_len = (int)n;
  return sp_x509_field_out.p;
}

/* Certificate#verify: 1 when the key verifies the signature, 0 when it does
   not, -1 with the reason when the DER or the key does not parse. */
sp_int sp_x509_verify(const char *der, const char *spki) {
  X509 *x = sp_x509_from_der(der);
  const unsigned char *p = (const unsigned char *)spki;
  EVP_PKEY *k = NULL;
  int r;

  sp_ssl_errbuf[0] = 0;
  if (!x) { sp_x509_reason("d2i_X509"); return -1; }
  if (sp_str_byte_len(spki) > (size_t)INT_MAX ||
      !(k = d2i_PUBKEY(NULL, &p, (long)sp_str_byte_len(spki)))) {
    X509_free(x);
    sp_x509_reason("d2i_PUBKEY");
    return -1;
  }
  r = X509_verify(x, k);
  EVP_PKEY_free(k);
  X509_free(x);
  if (r < 0) { sp_x509_reason(NULL); return -1; }
  ERR_clear_error();
  return r > 0 ? 1 : 0;
}

/* A context's own certificates and key, a server's or a client's: the
   certificate before the key, so SSL_CTX_use_PrivateKey is the call that
   finds a key that is not the certificate's, with CRuby's reason for it.
   Empty is no certificate. Answers 1, or 0 with the reason in
   sp_ssl_last_error. */
static int sp_ssl_use_chain(SSL_CTX *ctx, const char *chain, const char *curve, const char *priv) {
  size_t len = sp_str_byte_len(chain), at = 0;

  while (at < len) {
    const unsigned char *p = (const unsigned char *)chain + at, *der = p + 4;
    size_t n;
    X509 *x = NULL;
    int ok;

    if (len - at < 4) break;
    n = ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | p[3];
    if (n > len - at - 4 || n > (size_t)INT_MAX || !(x = d2i_X509(NULL, &der, (long)n))) {
      sp_ssl_note("certificate");
      return 0;
    }
    if (at == 0) {
      ok = SSL_CTX_use_certificate(ctx, x);      /* takes its own reference */
      X509_free(x);
      if (ok && (curve[0] || sp_str_byte_len(priv))) {
        EVP_PKEY *k = curve[0] ? sp_ec_evp_private(curve, priv) : sp_rsa_evp_private(priv);
        ok = k && SSL_CTX_use_PrivateKey(ctx, k);
        EVP_PKEY_free(k);
        if (!ok) { sp_ssl_note("SSL_CTX_use_PrivateKey"); return 0; }
        /* A key of another kind than the certificate's goes into a slot of
           its own rather than failing, and this is the call that finds it:
           the slot the key went into has no certificate. */
        if (!SSL_CTX_check_private_key(ctx)) { sp_ssl_note("SSL_CTX_check_private_key"); return 0; }
      }
    }
    else {
      ok = SSL_CTX_add_extra_chain_cert(ctx, x);  /* owns x once it succeeds */
      if (!ok) X509_free(x);
    }
    if (!ok) { sp_ssl_note("SSL_CTX_use_certificate"); return 0; }
    at += 4 + n;
  }
  if (at != len) { sp_ssl_note("certificate"); return 0; }
  return 1;
}

/* Answers the context, or NULL with the reason in sp_ssl_last_error. It asks
   for no client certificate; sp_ssl_accept_nb sets what verify_mode says. */
static SSL_CTX *sp_ssl_server_ctx(const char *chain, const char *curve, const char *priv) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

  if (!ctx) { sp_ssl_note("SSL_CTX_new"); return NULL; }
  /* The client side's floor and mode, for the same reasons. */
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  if (!sp_ssl_use_chain(ctx, chain, curve, priv)) { SSL_CTX_free(ctx); return NULL; }
  return ctx;
}

/* Begin a server connection on an accepted descriptor without waiting for
   the handshake, the mirror of sp_ssl_connect_nb: O_NONBLOCK is armed here,
   the first step is taken, and sp_ssl_want says what the next one waits for.
   Ruby's accept and accept_nonblock both start here, so a client that
   connects and never sends a ClientHello parks a green thread rather than
   holding an OS worker. Answers the handle, or -1 when the connection could
   not be built.
   `verify` is the SSL_VERIFY_* bits a server's verify_mode holds, 0 unless
   the program set it (4a in #9341): with SSL_VERIFY_PEER the server asks
   for a client certificate and validates one against `cert_store`, which
   is empty when the context has none, as CRuby's is. `min` and `max` are
   the context's protocol bounds, as for a client (sp_ssl_bounds). */
sp_int sp_ssl_accept_nb(sp_int fd, const char *chain, const char *curve, const char *priv,
                        sp_int verify, sp_RbVal cert_store, sp_int min, sp_int max) {
  SSL_CTX *ctx;
  SSL *ssl;
  int i;

  sp_ssl_want_state = 4;
  sp_ssl_errbuf[0] = 0;
  if (!(ctx = sp_ssl_server_ctx(chain, curve, priv))) return -1;
  if (!sp_ssl_bounds(ctx, min, max)) { SSL_CTX_free(ctx); return -1; }
  SSL_CTX_set_verify(ctx, (int)verify, NULL);
  if (cert_store.tag != SP_TAG_NIL) {
    sp_X509_Store *store = (sp_X509_Store *)cert_store.v.p;
    if (store && store->store) SSL_CTX_set1_cert_store(ctx, store->store);
  }
  if (!(ssl = SSL_new(ctx))) { sp_ssl_note("SSL_new"); SSL_CTX_free(ctx); return -1; }
  SSL_set_accept_state(ssl);
  if (!SSL_set_fd(ssl, (int)fd)) {
    sp_ssl_note("SSL_set_fd");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  /* Claimed last, as sp_ssl_setup claims it. */
  if ((i = sp_ssl_slot()) < 0) {
    sp_ssl_note("too many TLS connections");
    SSL_free(ssl); SSL_CTX_free(ctx);
    return -1;
  }
  sp_ssl_conn *c = &sp_ssl_tab[i];
  c->ssl = ssl;
  c->ctx = ctx;
  c->fd  = (int)fd;
  c->nonblock = 1;
  SP_ATOMIC_STORE(&c->in_use, 1, __ATOMIC_RELEASE);
  int fl = fcntl(c->fd, F_GETFL, 0);
  if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
  sp_ssl_handshake_step(c);
  return i;
}

/* add_certificate's check, made when it is called, as CRuby makes it: the
   context an accept would build is built and dropped. 1 when the key is the
   certificate's, 0 with the reason in sp_ssl_last_error. */
sp_int sp_ssl_server_check(const char *chain, const char *curve, const char *priv) {
  SSL_CTX *ctx;

  sp_ssl_errbuf[0] = 0;
  if (!(ctx = sp_ssl_server_ctx(chain, curve, priv))) return 0;
  SSL_CTX_free(ctx);
  return 1;
}

#include "sp_asn1.h"
