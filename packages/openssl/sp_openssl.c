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
#include <limits.h>
#include <string.h>
#include <fcntl.h>

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
  int      in_use;
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

static int sp_ssl_slot(void) {
  for (int i = 0; i < SP_SSL_MAX; i++) if (!sp_ssl_tab[i].in_use) return i;
  return -1;
}

static sp_ssl_conn *sp_ssl_at(sp_int h) {
  if (h < 0 || h >= SP_SSL_MAX) return NULL;
  sp_ssl_conn *c = &sp_ssl_tab[h];
  return c->in_use ? c : NULL;
}

/* Give a slot back, whatever stage it reached. */
static void sp_ssl_release(int i) {
  sp_ssl_conn *c = &sp_ssl_tab[i];
  if (c->ssl) SSL_free(c->ssl);
  if (c->ctx) SSL_CTX_free(c->ctx);
  memset(c, 0, sizeof *c);
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
   native X509_STORE is referenced by the SSL_CTX (SSL_CTX_set1_cert_store, so the store stays the handle's and is reusable). */
static int sp_ssl_setup(sp_int fd, const char *hostname, sp_int verify, sp_RbVal cert_store) {
  int i = sp_ssl_slot();
  if (i < 0) { sp_ssl_note("too many TLS connections"); return -1; }
  sp_ssl_errbuf[0] = 0;

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) { sp_ssl_note("SSL_CTX_new"); return -1; }
  /* TLS 1.2 is the floor: 1.0 and 1.1 are withdrawn and a server offering
     only those is not one to fall back to silently. */
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
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

  SSL *ssl = SSL_new(ctx);
  if (!ssl) { sp_ssl_note("SSL_new"); SSL_CTX_free(ctx); return -1; }
  if (hostname && hostname[0]) {
    SSL_set_tlsext_host_name(ssl, hostname);     /* SNI */
    if (verify) {
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
  sp_ssl_tab[i].ssl = ssl;
  sp_ssl_tab[i].ctx = ctx;
  sp_ssl_tab[i].fd  = (int)fd;
  sp_ssl_tab[i].nonblock = 0;
  sp_ssl_tab[i].in_use = 1;
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
   and a caller waiting on the wrong one waits forever. */
static void sp_ssl_handshake_step(sp_ssl_conn *c) {
  sp_ssl_errbuf[0] = 0;
  sp_ssl_want_state = 0;
  int r = SSL_connect(c->ssl);
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
sp_int sp_ssl_connect_nb(sp_int fd, const char *hostname, sp_int verify, sp_RbVal cert_store) {
  int i = sp_ssl_setup(fd, hostname, verify, cert_store);
  if (i < 0) { sp_ssl_want_state = 4; return -1; }
  sp_ssl_conn *c = &sp_ssl_tab[i];
  int fl = fcntl(c->fd, F_GETFL, 0);
  if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
  c->nonblock = 1;
  sp_ssl_handshake_step(c);
  return i;
}

/* Resume a handshake begun by sp_ssl_connect_nb. 1 when it is done, 0 when
   it is not, with sp_ssl_want carrying which of the two waits, or the
   failure. */
sp_int sp_ssl_connect_cont(sp_int h) {
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
  c->ssl = NULL; c->ctx = NULL; c->fd = -1; c->in_use = 0;
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
   No BN, EC::Group or EC::Point is built on the Ruby side, and -- unlike the
   connection table above -- nothing here outlives a call. An EC key is fully
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

/* On success, 0x01 || plaintext; on failure, an empty string with the reason in
   last_error. A tag that does not verify is a failure like any other and must
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
  int n1 = 0, n2 = 0, ok;

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
                           (void *)tag) &&
       EVP_CipherFinal_ex(ctx, (unsigned char *)out + 1 + n1, &n2);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok || (size_t)(n1 + n2) != cn) {
    /* Never hand back a partial plaintext from a message that did not
       authenticate, not even to a caller that would discard it. */
    OPENSSL_cleanse(out, cn + 1);
    sp_ssl_note("AES-GCM decrypt failed: auth tag did not verify");
    return sp_str_from_bytes("", 0);
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

   Signatures cross the boundary as raw r || s, two field-width integers, and
   never as DER. ES256 (RFC 7518) names that encoding, and it is the one place
   the two shapes are silently interchangeable: a DER signature is a
   well-formed String that every JWT verifier rejects, so handing one out
   would fail remotely, in someone else's stack, long after the call. The
   conversion is here rather than in each caller for exactly that reason. */

static SP_TLS char sp_ecdsa_sig_buf[2 * SP_EC_MAX_FIELD];

/* Sign a digest that has already been computed: ECDSA signs a hash, and which
   hash is the caller's business (SHA-256 for ES256). A digest longer than the
   curve's order is truncated by the standard, which is OpenSSL's behaviour and
   not something to reimplement here. */
const char *sp_ecdsa_sign(const char *curve, const char *priv, const char *digest) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  BIGNUM *d = NULL;
  EC_KEY *k = NULL;
  ECDSA_SIG *sig = NULL;
  const BIGNUM *r, *s;
  int ok = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) return sp_ec_fail("unknown or unsupported curve");
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
    ECDSA_SIG_get0(sig, &r, &s);
    ok = BN_bn2binpad(r, (unsigned char *)sp_ecdsa_sig_buf, flen) == flen &&
         BN_bn2binpad(s, (unsigned char *)sp_ecdsa_sig_buf + flen, flen) == flen;
    ECDSA_SIG_free(sig);
  }
  BN_clear_free(d);
  EC_GROUP_free(g);
  if (!ok) return sp_ec_fail("ECDSA signing failed");
  sp_ffi_bin_len = 2 * flen;
  return sp_ecdsa_sig_buf;
}

/* Verify raw r || s against a public point. Answers 1 only for a signature
   that verifies: a malformed signature, a point that is not on the curve and
   a mismatch are all 0, because a caller asking "is this signature good"
   must not be able to read "the input was strange" as yes. */
sp_int sp_ecdsa_verify(const char *curve, const char *pub, const char *digest,
                       const char *sig_bytes) {
  int flen = 0;
  EC_GROUP *g = sp_ec_group(curve, &flen);
  EC_POINT *pt = NULL;
  EC_KEY *k = NULL;
  ECDSA_SIG *sig = NULL;
  BIGNUM *r = NULL, *s = NULL;
  int rc = 0;

  sp_ssl_errbuf[0] = 0;
  if (!g) { sp_ssl_note("unknown or unsupported curve"); return 0; }
  if (sp_str_byte_len(sig_bytes) != (size_t)(2 * flen)) {
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
  r = BN_bin2bn((const unsigned char *)sig_bytes, flen, NULL);
  s = BN_bin2bn((const unsigned char *)sig_bytes + flen, flen, NULL);
  sig = ECDSA_SIG_new();
  if (r && s && sig && ECDSA_SIG_set0(sig, r, s)) {
    r = NULL; s = NULL;
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if ((k = EC_KEY_new()) && EC_KEY_set_group(k, g) && EC_KEY_set_public_key(k, pt))
      rc = ECDSA_do_verify((const unsigned char *)digest,
                           (int)sp_str_byte_len(digest), sig, k) == 1;
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
  if (!rc) sp_ssl_note("signature did not verify");
  return rc;
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
