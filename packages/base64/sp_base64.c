/* sp_base64.c -- Base64 for the `base64` spin package (Path B carried C),
   linked on demand when `require "base64"` appears.

   CRuby-compatible surface: encode64 (RFC 2045: "+/" alphabet, padded, a
   newline every 60 output chars and after the tail), strict_encode64
   (RFC 4648: padded, no newlines), urlsafe_encode64 ("-_" alphabet, padded
   like CRuby's default padding: true), and the matching decoders. Binary
   safe on both sides: input length comes from the string header
   (sp_str_byte_len), and decoded output is length-set so embedded NULs
   survive. Results live on the GC string heap. */
#include "spinel/runtime.h"
#include <string.h>

static const char B64_STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* encode `n` bytes of `src` with `alpha`; a positive `wrap` inserts a newline
   every `wrap` output chars and after the tail (CRuby encode64 wraps at 60). */
/* `alpha` is one of the two static tables below, never a GC string: rooting
   it makes the collector read the marker byte in front of a plain global
   (ASAN: global-buffer-overflow) and treat whatever it finds there as a
   string header. Only `src` is a real string. */
static const char *b64_enc(const char *src, size_t n, const char *alpha, int wrap) {SP_GC_ROOT_STR(src);
  size_t groups = (n + 2) / 3;
  size_t outlen = groups * 4;
  size_t nl = wrap > 0 ? (outlen + (size_t)wrap - 1) / (size_t)wrap : 0;
  char *r = sp_str_alloc_raw(outlen + nl + 1);
  size_t o = 0, col = 0, i = 0;
  const unsigned char *u = (const unsigned char *)src;
  /* whole groups of three bytes, four chars each: when the line width is a
     multiple of four (CRuby's 60, or none) a newline falls between groups */
  if (wrap % 4 == 0) {
    for (; i + 3 <= n; i += 3) {
      unsigned v = (unsigned)u[i] << 16 | (unsigned)u[i + 1] << 8 | u[i + 2];
      r[o] = alpha[v >> 18];
      r[o + 1] = alpha[(v >> 12) & 63];
      r[o + 2] = alpha[(v >> 6) & 63];
      r[o + 3] = alpha[v & 63];
      o += 4;
      if (wrap > 0 && (col += 4) == (size_t)wrap) { r[o++] = '\n'; col = 0; }
    }
  }
  for (; i < n; i += 3) {
    unsigned v = (unsigned char)src[i] << 16;
    if (i + 1 < n) v |= (unsigned char)src[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned char)src[i + 2];
    char q[4];
    q[0] = alpha[(v >> 18) & 63];
    q[1] = alpha[(v >> 12) & 63];
    q[2] = i + 1 < n ? alpha[(v >> 6) & 63] : '=';
    q[3] = i + 2 < n ? alpha[v & 63] : '=';
    for (int k = 0; k < 4; k++) {
      r[o++] = q[k];
      if (wrap > 0 && ++col == (size_t)wrap) { r[o++] = '\n'; col = 0; }
    }
  }
  if (wrap > 0 && col > 0) r[o++] = '\n';
  r[o] = '\0';
  sp_str_set_len(r, o);
  return r;
}

/* A byte's value in an alphabet, XX for every byte outside it (whitespace and
   `=` included): one load per byte in place of a chain of range tests. The
   urlsafe table takes `+` and `/` beside `-` and `_`, as CRuby's
   urlsafe_decode64 does: it turns `-_` into `+/` and decodes the standard
   alphabet. */
enum { XX = 0xFF };
static const unsigned char B64_DEC_STD[256] = {
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, 62, XX, XX, XX, 63,
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, XX, XX, XX, XX, XX, XX,
  XX,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, XX, XX, XX, XX, XX,
  XX, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX
};
static const unsigned char B64_DEC_URL[256] = {
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, 62, XX, 62, XX, 63,
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, XX, XX, XX, XX, XX, XX,
  XX,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, XX, XX, XX, XX, 63,
  XX, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
  XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX
};

/* decode, skipping whitespace, `=` and any byte outside the alphabet, as
   CRuby's decode64 does. */
static const char *b64_dec(const char *src, size_t n, const unsigned char *tbl) {SP_GC_ROOT_STR(src);
  char *r = sp_str_alloc_raw(n / 4 * 3 + 4);
  size_t o = 0;
  unsigned acc = 0;
  int nb = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned v = tbl[(unsigned char)src[i]];
    if (v == XX) continue;   /* CRuby decode64 ignores out-of-alphabet bytes */
    acc = (acc << 6) | v;
    if (++nb == 4) { r[o++] = (char)(acc >> 16); r[o++] = (char)(acc >> 8); r[o++] = (char)acc; acc = 0; nb = 0; }
  }
  if (nb == 3) { acc <<= 6; r[o++] = (char)(acc >> 16); r[o++] = (char)(acc >> 8); }
  else if (nb == 2) { acc <<= 12; r[o++] = (char)(acc >> 16); }
  r[o] = '\0';
  sp_str_set_len(r, o);
  sp_str_mark_binary(r);   /* the bytes, as CRuby's decoders answer them */
  return r;
}

/* Strict decode (CRuby's `unpack1("m0")`): the input length must be a multiple
   of 4, every byte must be an alphabet char or valid trailing `=` padding, and
   any stray byte (whitespace, newline, out-of-alphabet) raises ArgumentError --
   unlike b64_dec, which silently skips them. `pad` more `=` are read after the
   `n` bytes, for urlsafe_decode64's unpadded input, without copying it. */
static const char *b64_dec_strict(const char *src, size_t n, size_t pad, const unsigned char *tbl) {SP_GC_ROOT_STR(src);
  size_t len = n;
  n += pad;
  if (n % 4 != 0) sp_raise_cls("ArgumentError", "invalid base64");
  char *r = sp_str_alloc_raw(n / 4 * 3 + 4);
  size_t o = 0, i = 0;
  const unsigned char *u = (const unsigned char *)src;
  /* every block but the last: four alphabet chars, as `=` is padding only
     at the end; an XX in any of them sets the high bit of their union (the
     read `=` are all in the last block) */
  for (; i + 4 < n; i += 4) {
    unsigned a = tbl[u[i]], b = tbl[u[i + 1]], c = tbl[u[i + 2]], d = tbl[u[i + 3]];
    if ((a | b | c | d) & 0x80) sp_raise_cls("ArgumentError", "invalid base64");
    unsigned acc = a << 18 | b << 12 | c << 6 | d;
    r[o] = (char)(acc >> 16);
    r[o + 1] = (char)(acc >> 8);
    r[o + 2] = (char)acc;
    o += 3;
  }
  for (; i < n; i += 4) {
    unsigned q[4]; int npad = 0;
    for (int k = 0; k < 4; k++) {
      char c = i + k < len ? src[i + k] : '=';
      if (c == '=') {
        /* `=` is legal only as trailing padding of the final block (position
           2 with a following `=`, or position 3). */
        if (i + 4 != n || k < 2) sp_raise_cls("ArgumentError", "invalid base64");
        npad++; q[k] = 0;
      }
      else {
        if (npad) sp_raise_cls("ArgumentError", "invalid base64");  /* data after pad */
        unsigned v = tbl[(unsigned char)c];
        if (v == XX) sp_raise_cls("ArgumentError", "invalid base64");
        q[k] = v;
      }
    }
    /* the bits padding drops must be zero, as CRuby's m0 has them: "8V=="
       carries a 1 past its one byte */
    if ((npad == 2 && (q[1] & 0xF)) || (npad == 1 && (q[2] & 0x3)))
      sp_raise_cls("ArgumentError", "invalid base64");
    unsigned acc = (q[0] << 18) | (q[1] << 12) | (q[2] << 6) | q[3];
    r[o++] = (char)(acc >> 16);
    if (npad < 2) r[o++] = (char)(acc >> 8);
    if (npad < 1) r[o++] = (char)acc;
  }
  r[o] = '\0';
  sp_str_set_len(r, o);
  sp_str_mark_binary(r);
  return r;
}

const char *sp_base64_encode64(const char *s)        { return b64_enc(s, sp_str_byte_len(s), B64_STD, 60); }
const char *sp_base64_strict_encode64(const char *s) { return b64_enc(s, sp_str_byte_len(s), B64_STD, 0); }
const char *sp_base64_urlsafe_encode64(const char *s){ return b64_enc(s, sp_str_byte_len(s), B64_URL, 0); }
const char *sp_base64_decode64(const char *s)        { return b64_dec(s, sp_str_byte_len(s), B64_DEC_STD); }
const char *sp_base64_strict_decode64(const char *s) { return b64_dec_strict(s, sp_str_byte_len(s), 0, B64_DEC_STD); }
/* CRuby's urlsafe_decode64: input that does not end in `=` is padded to a
   multiple of 4, and the result decoded strictly, `-_` read as `+/` */
const char *sp_base64_urlsafe_decode64(const char *s) {
  size_t n = sp_str_byte_len(s);
  size_t pad = n % 4 != 0 && s[n - 1] != '=' ? 4 - n % 4 : 0;
  return b64_dec_strict(s, n, pad, B64_DEC_URL);
}
