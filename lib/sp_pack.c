/*
 * sp_pack.c — Array#pack / String#unpack for Spinel
 *
 * Implements the common Perl/Ruby pack format specifiers. Built
 * as a separate translation unit and linked into libspinel_rt.a;
 * it allocates GC-managed strings and PolyArrays directly through
 * the shared headers (sp_alloc.h), so no sp_ext_* shim is needed.
 *
 * Supported specifiers (initial):
 *   C / c   unsigned / signed 8-bit
 *   n / N   unsigned 16 / 32-bit big-endian
 *   v / V   unsigned 16 / 32-bit little-endian
 *   s / S   signed / unsigned 16-bit native
 *   l / L   signed / unsigned 32-bit native
 *   q / Q   signed / unsigned 64-bit native
 *   a       binary string (NUL-padded on pack)
 *   A       text string (space-padded on pack, space-trimmed on unpack)
 *   Z       NUL-terminated string
 *   x       null byte
 *
 * Counts:  `<spec>N` packs N items; `<spec>*` consumes all remaining.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* The shared runtime types (sp_RbVal, sp_IntArray, sp_sym, mrb_*, SP_TAG_*) and
   the string allocator (sp_str_alloc / _set_len / _byte_len / sp_str_empty) come
   straight from the shared headers: this separate TU can now allocate GC strings
   directly onto the one shared heap, so no sp_ext_str_* shim is needed. */
#include "sp_alloc.h"   /* string + object allocation, sp_box_*, sp_PolyArray */
#include "sp_str.h"     /* sp_nil_recv for the nil-receiver unpack raise */

/* ---------- Helpers ---------- */

static void pk_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
  if (*len + n + 1 > *cap) {
    size_t nc = (*len + n + 1) * 2;
    char *nb = (char *)realloc(*buf, nc);
    if (!nb) { perror("realloc"); exit(1); }
    *buf = nb;
    *cap = nc;
  }
  memcpy(*buf + *len, src, n);
  *len += n;
}

/* Encode codepoint `v` as UTF-8 (the `U` directive) into out[0..5].
   Mirrors CRuby's permissive scheme: up to 6 bytes for values through
   0x7FFFFFFF. Negative / out-of-range values encode nothing (CRuby
   raises RangeError; spinel has no exception path here). Returns the
   byte count written. */
static int pk_utf8(unsigned char *out, int64_t v) {
  if (v < 0) return 0;
  uint64_t uv = (uint64_t)v;
  if (uv <= 0x7F) {
    out[0] = (unsigned char)uv;
    return 1;
  }
  if (uv <= 0x7FF) {
    out[0] = (unsigned char)(0xC0 | (uv >> 6));
    out[1] = (unsigned char)(0x80 | (uv & 0x3F));
    return 2;
  }
  if (uv <= 0xFFFF) {
    out[0] = (unsigned char)(0xE0 | (uv >> 12));
    out[1] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[2] = (unsigned char)(0x80 | (uv & 0x3F));
    return 3;
  }
  if (uv <= 0x1FFFFF) {
    out[0] = (unsigned char)(0xF0 | (uv >> 18));
    out[1] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (uv & 0x3F));
    return 4;
  }
  if (uv <= 0x3FFFFFF) {
    out[0] = (unsigned char)(0xF8 | (uv >> 24));
    out[1] = (unsigned char)(0x80 | ((uv >> 18) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[3] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[4] = (unsigned char)(0x80 | (uv & 0x3F));
    return 5;
  }
  if (uv <= 0x7FFFFFFF) {
    out[0] = (unsigned char)(0xFC | (uv >> 30));
    out[1] = (unsigned char)(0x80 | ((uv >> 24) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 18) & 0x3F));
    out[3] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[4] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[5] = (unsigned char)(0x80 | (uv & 0x3F));
    return 6;
  }
  return 0;
}

/* Consume any '<' '>' '!' '_' modifiers after a directive letter, then the
   optional count. Without this, `pack('l<l<l<l<')` treated each '<' as its
   own directive -- and since the element cursor advances per directive, every
   other value was silently dropped (doom's SDL_Rect bytes came out as
   {x,w,0,0}). '!'/'_' pick the native size (what the plain directive
   already is here); '<'/'>' set *big. CRuby raises RangeError on a
   repeated endian modifier ("l><"); spinel's pack has no exception
   path, so the last one wins instead. */
static int64_t pk_parse_count_mods(const char **pp, int *big) {
  const char *p = *pp;
  while (*p == '<' || *p == '>' || *p == '!' || *p == '_') {
    if (big) {
      if (*p == '>') *big = 1;
      else if (*p == '<') *big = 0;
    }
    p++;
  }
  if (*p == '*') { *pp = p + 1; return -1; }
  if (*p < '0' || *p > '9') { *pp = p; return 1; }
  int64_t n = 0;
  while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
  *pp = p;
  return n;
}
static int64_t pk_parse_count(const char **pp) {
  return pk_parse_count_mods(pp, NULL);
}
/* Serialize / deserialize an integer as `n` bytes, little- or
   big-endian, via shifts (host-endianness independent, like the
   n/N/v/V paths). pk_put_bits takes the raw unsigned pattern so the
   float directives can pass IEEE-754 bits without a narrowing
   uint64 -> int64 conversion (implementation-defined for values with
   the sign bit set). */
static void pk_put_bits(char *out, uint64_t uv, size_t n, int big) {
  for (size_t i = 0; i < n; i++) {
    size_t sh = big ? (n - 1 - i) : i;
    out[i] = (char)((uv >> (8 * sh)) & 0xff);
  }
}
static void pk_put_int(char *out, int64_t v, size_t n, int big) {
  pk_put_bits(out, (uint64_t)v, n, big);
}
static uint64_t pk_get_int(const unsigned char *u, size_t n, int big) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sh = big ? (n - 1 - i) : i;
    v |= (uint64_t)u[i] << (8 * sh);
  }
  return v;
}

/* Float/double directives: D/d/F/f native byte order, E/e explicit
   little-endian, G/g explicit big-endian (double for the uppercase-native/
   E/G forms, single precision for f/e/g). The IEEE-754 bit pattern round-
   trips through an integer so the endian-explicit forms can reuse the
   shift-based byte writer (host-endianness independent). */
static int pk_is_flt_spec(char spec) {
  return spec == 'D' || spec == 'd' || spec == 'F' || spec == 'f' ||
         spec == 'E' || spec == 'e' || spec == 'G' || spec == 'g';
}
static void pk_flt_directive(char spec, double v, char **buf, size_t *len, size_t *cap) {
  char tmp[8];
  switch (spec) {
    case 'D': case 'd':
      memcpy(tmp, &v, 8);
      pk_append(buf, len, cap, tmp, 8);
      break;
    case 'E': case 'G': {
      uint64_t bits; memcpy(&bits, &v, 8);
      pk_put_bits(tmp, bits, 8, spec == 'G');
      pk_append(buf, len, cap, tmp, 8);
      break;
    }
    case 'F': case 'f': {
      float f = (float)v;
      memcpy(tmp, &f, 4);
      pk_append(buf, len, cap, tmp, 4);
      break;
    }
    case 'e': case 'g': {
      float f = (float)v;
      uint32_t bits; memcpy(&bits, &f, 4);
      pk_put_bits(tmp, bits, 4, spec == 'g');
      pk_append(buf, len, cap, tmp, 4);
      break;
    }
  }
}
/* Decode one float/double element on unpack (4 bytes for f/F/e/g, 8 for
   d/D/E/G -- the caller sizes the read via fsize). */
static double uk_get_flt(char spec, const unsigned char *u) {
  if (spec == 'D' || spec == 'd') { double v; memcpy(&v, u, 8); return v; }
  if (spec == 'F' || spec == 'f') { float v; memcpy(&v, u, 4); return (double)v; }
  if (spec == 'E' || spec == 'G') {
    uint64_t bits = pk_get_int(u, 8, spec == 'G');
    double v; memcpy(&v, &bits, 8); return v;
  }
  uint32_t bits = (uint32_t)pk_get_int(u, 4, spec == 'g');
  float v; memcpy(&v, &bits, 4); return (double)v;
}

/* Truncate a double toward zero for the integer directives with CRuby's
   Array#pack semantics: NaN / +-Infinity raise FloatDomainError, and finite
   values outside the int64 range wrap modulo 2^64 (CRuby coerces through an
   exact Integer, and the directive's byte mask keeps only the low bits). A
   plain (int64_t) cast is undefined C for anything outside
   [INT64_MIN, INT64_MAX], including NaN. */
static int64_t pk_flt_to_int(double dv) {
  if (isnan(dv)) sp_raise_cls("FloatDomainError", "NaN");
  if (isinf(dv)) sp_raise_cls("FloatDomainError", dv > 0 ? "Infinity" : "-Infinity");
  if (dv >= -9223372036854775808.0 && dv < 9223372036854775808.0)
    return (int64_t)dv;
  /* Out-of-range finite: |dv| >= 2^63, so the mantissa is shifted left by
     its (positive) binary exponent; the unsigned shift wraps mod 2^64. */
  double a = dv < 0 ? -dv : dv;
  uint64_t bits; memcpy(&bits, &a, 8);
  int sh = (int)((bits >> 52) & 0x7FF) - 1075;   /* a = mant * 2^sh */
  uint64_t mant = (bits & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
  uint64_t u = sh >= 64 ? 0 : mant << sh;
  return (int64_t)(dv < 0 ? 0 - u : u);
}

static int64_t pk_poly_to_int(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT:  return v.v.i;
    case SP_TAG_BOOL: return v.v.b ? 1 : 0;
    case SP_TAG_FLT:  return pk_flt_to_int(v.v.f);
    case SP_TAG_STR:  return v.v.s ? strtoll(v.v.s, NULL, 0) : 0;
    case SP_TAG_NIL:  return 0;
    default:          return 0;
  }
}

static double pk_poly_to_flt(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_FLT:  return v.v.f;
    case SP_TAG_INT:  return (double)v.v.i;
    case SP_TAG_BOOL: return v.v.b ? 1.0 : 0.0;
    case SP_TAG_STR:  return v.v.s ? strtod(v.v.s, NULL) : 0.0;
    default:          return 0.0;
  }
}

static const char *pk_poly_to_str(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_STR: return v.v.s ? v.v.s : "";
    case SP_TAG_NIL: return "";
    default:         return "";
  }
}

/* ---------- Base64 (`m`) and quoted-printable (`M`) encoders ---------- */

/* Standard Base64 (RFC 4648 §4): `+/` alphabet, `=` padding. Encodes one
   run of `n` input bytes as ceil(n/3)*4 output chars into the growable buf. */
static void pk_b64_run(char **buf, size_t *len, size_t *cap,
                       const unsigned char *s, size_t n) {
  static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char o[4];
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = ((uint32_t)s[i] << 16) | ((uint32_t)s[i + 1] << 8) | s[i + 2];
    o[0] = B64[(v >> 18) & 63]; o[1] = B64[(v >> 12) & 63];
    o[2] = B64[(v >> 6) & 63];  o[3] = B64[v & 63];
    pk_append(buf, len, cap, o, 4);
  }
  size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)s[i] << 16;
    o[0] = B64[(v >> 18) & 63]; o[1] = B64[(v >> 12) & 63]; o[2] = '='; o[3] = '=';
    pk_append(buf, len, cap, o, 4);
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)s[i] << 16) | ((uint32_t)s[i + 1] << 8);
    o[0] = B64[(v >> 18) & 63]; o[1] = B64[(v >> 12) & 63];
    o[2] = B64[(v >> 6) & 63];  o[3] = '=';
    pk_append(buf, len, cap, o, 4);
  }
}

/* `m` directive, matching CRuby pack.c: count 0 (`m0`) = one unbroken base64
   string, no trailing newline; a bare `m` (count 1) or `m1`/`m2` wraps every
   45 input bytes; `mN` (N>=3) wraps every N/3*3 bytes. Each wrapped line, and
   the final one, ends with `\n`. An empty input yields the empty string. */
static void pk_emit_base64(char **buf, size_t *len, size_t *cap,
                           const unsigned char *s, size_t sl, int64_t count) {
  if (count == 0) { pk_b64_run(buf, len, cap, s, sl); return; }
  size_t chunk = (count <= 2) ? 45 : (size_t)((count / 3) * 3);
  if (chunk < 3) chunk = 45;
  for (size_t off = 0; off < sl; off += chunk) {
    size_t n = (sl - off < chunk) ? (sl - off) : chunk;
    pk_b64_run(buf, len, cap, s + off, n);
    pk_append(buf, len, cap, "\n", 1);
  }
}

/* `M` directive (quoted-printable), faithful to CRuby pack.c qpencode: a bare
   `M` (count 1) or `M0`/`M1` uses a 72-char soft-wrap; `MN` wraps at N. */
static void pk_emit_qp(char **buf, size_t *len, size_t *cap,
                       const unsigned char *s, size_t sl, int64_t count) {
  static const char HEX[] = "0123456789ABCDEF";
  int64_t line = (count <= 1) ? 72 : count;
  long n = 0;
  int prev = -1;
  for (size_t k = 0; k < sl; k++) {
    unsigned char ch = s[k];
    if (ch > 126 || (ch < 32 && ch != '\n' && ch != '\t') || ch == '=') {
      char o[3] = { '=', HEX[ch >> 4], HEX[ch & 0x0f] };
      pk_append(buf, len, cap, o, 3); n += 3; prev = -1;
    } else if (ch == '\n') {
      if (prev == ' ' || prev == '\t') { char e[2] = { '=', '\n' }; pk_append(buf, len, cap, e, 2); }
      pk_append(buf, len, cap, "\n", 1); n = 0; prev = ch;
    } else {
      pk_append(buf, len, cap, (const char *)&ch, 1); n++; prev = ch;
    }
    if (n > line) { char e[2] = { '=', '\n' }; pk_append(buf, len, cap, e, 2); n = 0; prev = '\n'; }
  }
  if (n > 0) { char e[2] = { '=', '\n' }; pk_append(buf, len, cap, e, 2); }
}

/* Shared m/M dispatch for the string-directive branch of both pack paths. */
static void pk_str_directive(char spec, int64_t count, const char *s, size_t sl,
                             char **buf, size_t *len, size_t *cap) {
  if (spec == 'm') pk_emit_base64(buf, len, cap, (const unsigned char *)s, sl, count);
  else             pk_emit_qp(buf, len, cap, (const unsigned char *)s, sl, count);
}

/* Pack a string element under a byte-producing directive: H/h (hex nibbles),
   B/b (bit string), u (uuencode). Shared by the typed-array and poly-array pack
   entry points. `sl` is the source byte length; `count` (< 0 means `*`) is a
   nibble/bit count for H/h/B/b and unused for u. */
static void pk_str_bytes_directive(char spec, int64_t count, const char *s, size_t sl,
                                   char **buf, size_t *len, size_t *cap) {
  if (spec == 'H' || spec == 'h') {
    size_t n = (count < 0) ? sl : (size_t)count;
    for (size_t bi = 0; bi < (n + 1) / 2; bi++) {
      char c0 = (2 * bi < n && 2 * bi < sl) ? s[2 * bi] : '0';
      char c1 = (2 * bi + 1 < n && 2 * bi + 1 < sl) ? s[2 * bi + 1] : '0';
      int hi = (c0 >= '0' && c0 <= '9') ? c0 - '0' : (c0 | 0x20) - 'a' + 10;
      int lo = (c1 >= '0' && c1 <= '9') ? c1 - '0' : (c1 | 0x20) - 'a' + 10;
      char byte = (char)((spec == 'H') ? ((hi << 4) | lo) : ((lo << 4) | hi));
      pk_append(buf, len, cap, &byte, 1);
    }
  } else if (spec == 'B' || spec == 'b') {
    size_t n = (count < 0) ? sl : (size_t)count;
    for (size_t bi = 0; bi < (n + 7) / 8; bi++) {
      unsigned char byte = 0;
      for (int j = 0; j < 8; j++) {
        size_t k = bi * 8 + j;
        int bit = (k < n && k < sl) ? (s[k] & 1) : 0;
        byte |= (unsigned char)(bit << ((spec == 'B') ? (7 - j) : j));
      }
      pk_append(buf, len, cap, (char *)&byte, 1);
    }
  } else if (spec == 'u') {
    size_t pos = 0;
    while (pos < sl) {
      size_t ll = sl - pos; if (ll > 45) ll = 45;
      char lc = (char)(ll + 0x20); pk_append(buf, len, cap, &lc, 1);
      for (size_t i = 0; i < ll; i += 3) {
        unsigned char b0 = (unsigned char)s[pos + i];
        unsigned char b1 = (i + 1 < ll) ? (unsigned char)s[pos + i + 1] : 0;
        unsigned char b2 = (i + 2 < ll) ? (unsigned char)s[pos + i + 2] : 0;
        int e[4] = { b0 >> 2, ((b0 << 4) | (b1 >> 4)) & 0x3F, ((b1 << 2) | (b2 >> 6)) & 0x3F, b2 & 0x3F };
        for (int j = 0; j < 4; j++) { char ch = (char)(e[j] ? e[j] + 0x20 : 0x60); pk_append(buf, len, cap, &ch, 1); }
      }
      char nl = '\n'; pk_append(buf, len, cap, &nl, 1);
      pos += ll;
    }
  }
}


/* ---------- Pack entry points ---------- */

const char *sp_IntArray_pack(sp_IntArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    /* w: BER-compressed integers (base-128, high bit = continuation). */
    if (spec == 'w') {
      int64_t wc = count < 0 ? arr->len - idx : count;
      for (int64_t k = 0; k < wc; k++) {
        int64_t sv = (idx < arr->len) ? arr->data[arr->start + idx] : 0; idx++;
        if (sv < 0) sp_raise_cls("ArgumentError", "can't compress negative numbers");
        uint64_t v = (uint64_t)sv;
        unsigned char tmp[10]; int ti = 0;
        tmp[ti++] = (unsigned char)(v & 0x7F); v >>= 7;
        while (v > 0) { tmp[ti++] = (unsigned char)((v & 0x7F) | 0x80); v >>= 7; }
        for (int j = ti - 1; j >= 0; j--) pk_append(&buf, &len, &cap, (char *)&tmp[j], 1);
      }
      continue;
    }
    if (count < 0) count = arr->len - idx;
    if (count < 0) count = 0;
    if (pk_is_flt_spec(spec)) {
      for (int64_t k = 0; k < count; k++) {
        double dv = (idx < arr->len) ? (double)arr->data[arr->start + idx] : 0.0;
        idx++;
        pk_flt_directive(spec, dv, &buf, &len, &cap);
      }
      continue;
    }
    for (int64_t k = 0; k < count; k++) {
      int64_t v = (idx < arr->len) ? arr->data[arr->start + idx] : 0;
      idx++;
      char tmp[8];
      switch (spec) {
        case 'C': case 'c':
          tmp[0] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 1);
          break;
        case 'n':
          tmp[0] = (char)((v >> 8) & 0xff); tmp[1] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'N':
          tmp[0] = (char)((v >> 24) & 0xff); tmp[1] = (char)((v >> 16) & 0xff);
          tmp[2] = (char)((v >> 8) & 0xff);  tmp[3] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'v':
          tmp[0] = (char)(v & 0xff); tmp[1] = (char)((v >> 8) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'V':
          tmp[0] = (char)(v & 0xff);        tmp[1] = (char)((v >> 8) & 0xff);
          tmp[2] = (char)((v >> 16) & 0xff); tmp[3] = (char)((v >> 24) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'i': case 'I':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 's': case 'S':
          pk_put_int(tmp, v, 2, big);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'l': case 'L':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'q': case 'Q':
          pk_put_int(tmp, v, 8, big);
          pk_append(&buf, &len, &cap, tmp, 8);
          break;
        case 'x':
          tmp[0] = 0;
          pk_append(&buf, &len, &cap, tmp, 1);
          idx--;
          break;
        case 'U': {
          unsigned char ub[6];
          int ulen = pk_utf8(ub, v);
          pk_append(&buf, &len, &cap, (const char *)ub, (size_t)ulen);
          break;
        }
        default:
          break;
      }
    }
  }
  /* Hand back via GC-tracked sp_str_alloc so the main file's GC
     can free the buffer. */
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

/* Pack a Float array: the float/double directives pack the element's
   IEEE-754 encoding directly; the integer directives truncate toward zero
   via pk_flt_to_int (CRuby coerces Float through an exact Integer the same
   way, so [1.5].pack("C") is "\x01" and NaN/Infinity raise
   FloatDomainError). Mirrors sp_IntArray_pack otherwise -- note
   sp_FloatArray has no `start` offset field. */
const char *sp_FloatArray_pack(sp_FloatArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    if (count < 0) count = arr->len - idx;
    if (count < 0) count = 0;
    if (pk_is_flt_spec(spec)) {
      for (int64_t k = 0; k < count; k++) {
        double dv = (idx < arr->len) ? arr->data[idx] : 0.0;
        idx++;
        pk_flt_directive(spec, dv, &buf, &len, &cap);
      }
      continue;
    }
    for (int64_t k = 0; k < count; k++) {
      int64_t v = (idx < arr->len) ? pk_flt_to_int(arr->data[idx]) : 0;
      idx++;
      char tmp[8];
      switch (spec) {
        case 'C': case 'c':
          tmp[0] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 1);
          break;
        case 'n':
          tmp[0] = (char)((v >> 8) & 0xff); tmp[1] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'N':
          tmp[0] = (char)((v >> 24) & 0xff); tmp[1] = (char)((v >> 16) & 0xff);
          tmp[2] = (char)((v >> 8) & 0xff);  tmp[3] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'v':
          tmp[0] = (char)(v & 0xff); tmp[1] = (char)((v >> 8) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'V':
          tmp[0] = (char)(v & 0xff);        tmp[1] = (char)((v >> 8) & 0xff);
          tmp[2] = (char)((v >> 16) & 0xff); tmp[3] = (char)((v >> 24) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 's': case 'S':
          pk_put_int(tmp, v, 2, big);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'l': case 'L':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'q': case 'Q':
          pk_put_int(tmp, v, 8, big);
          pk_append(&buf, &len, &cap, tmp, 8);
          break;
        case 'x':
          tmp[0] = 0;
          pk_append(&buf, &len, &cap, tmp, 1);
          idx--;
          break;
        case 'U': {
          unsigned char ub[6];
          int ulen = pk_utf8(ub, v);
          pk_append(&buf, &len, &cap, (const char *)ub, (size_t)ulen);
          break;
        }
        default:
          break;
      }
    }
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

const char *sp_PolyArray_pack(sp_PolyArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    if (spec == 'a' || spec == 'A' || spec == 'Z') {
      const char *s = (idx < arr->len) ? pk_poly_to_str(arr->data[idx]) : "";
      idx++;
      size_t sl = strlen(s);
      size_t want = (count < 0) ? sl : (size_t)count;
      if (spec == 'Z' && count < 0) want = sl + 1;
      size_t take = sl < want ? sl : want;
      pk_append(&buf, &len, &cap, s, take);
      if (take < want) {
        char pad = (spec == 'A') ? ' ' : 0;
        for (size_t pi = 0; pi < want - take; pi++) pk_append(&buf, &len, &cap, &pad, 1);
      }
      continue;
    }
    if (spec == 'm' || spec == 'M') {
      const char *s = ""; size_t sl = 0;
      if (idx < arr->len) {
        sp_RbVal e = arr->data[idx];
        if (e.tag == SP_TAG_STR && e.v.s) { s = e.v.s; sl = sp_str_byte_len(s); }
      }
      idx++;
      pk_str_directive(spec, count, s, sl, &buf, &len, &cap);
      continue;
    }
    /* H/h (hex), B/b (bit), u (uuencode): consume one string element. */
    if (spec == 'H' || spec == 'h' || spec == 'B' || spec == 'b' || spec == 'u') {
      const char *s = (idx < arr->len) ? pk_poly_to_str(arr->data[idx]) : ""; idx++;
      pk_str_bytes_directive(spec, count, s, strlen(s), &buf, &len, &cap);
      continue;
    }
    /* w: BER-compressed integers (base-128, high bit = continuation). */
    if (spec == 'w') {
      int64_t wc = count < 0 ? arr->len - idx : count;
      for (int64_t k = 0; k < wc; k++) {
        int64_t sv = (idx < arr->len) ? pk_poly_to_int(arr->data[idx]) : 0; idx++;
        if (sv < 0) sp_raise_cls("ArgumentError", "can't compress negative numbers");
        uint64_t v = (uint64_t)sv;
        unsigned char tmp[10]; int ti = 0;
        tmp[ti++] = (unsigned char)(v & 0x7F); v >>= 7;
        while (v > 0) { tmp[ti++] = (unsigned char)((v & 0x7F) | 0x80); v >>= 7; }
        for (int j = ti - 1; j >= 0; j--) pk_append(&buf, &len, &cap, (char *)&tmp[j], 1);
      }
      continue;
    }
    if (count < 0) count = arr->len - idx;
    if (count < 0) count = 0;
    if (pk_is_flt_spec(spec)) {
      for (int64_t k = 0; k < count; k++) {
        double dv = (idx < arr->len) ? pk_poly_to_flt(arr->data[idx]) : 0.0;
        idx++;
        pk_flt_directive(spec, dv, &buf, &len, &cap);
      }
      continue;
    }
    for (int64_t k = 0; k < count; k++) {
      int64_t v = (idx < arr->len) ? pk_poly_to_int(arr->data[idx]) : 0;
      idx++;
      char tmp[8];
      switch (spec) {
        case 'C': case 'c':
          tmp[0] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 1);
          break;
        case 'n':
          tmp[0] = (char)((v >> 8) & 0xff); tmp[1] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'N':
          tmp[0] = (char)((v >> 24) & 0xff); tmp[1] = (char)((v >> 16) & 0xff);
          tmp[2] = (char)((v >> 8) & 0xff);  tmp[3] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'v':
          tmp[0] = (char)(v & 0xff); tmp[1] = (char)((v >> 8) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'V':
          tmp[0] = (char)(v & 0xff);        tmp[1] = (char)((v >> 8) & 0xff);
          tmp[2] = (char)((v >> 16) & 0xff); tmp[3] = (char)((v >> 24) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 's': case 'S':
          pk_put_int(tmp, v, 2, big);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'l': case 'L':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'q': case 'Q':
          pk_put_int(tmp, v, 8, big);
          pk_append(&buf, &len, &cap, tmp, 8);
          break;
        case 'x':
          tmp[0] = 0;
          pk_append(&buf, &len, &cap, tmp, 1);
          idx--;
          break;
        case 'U': {
          unsigned char ub[6];
          int ulen = pk_utf8(ub, v);
          pk_append(&buf, &len, &cap, (const char *)ub, (size_t)ulen);
          break;
        }
        default:
          break;
      }
    }
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

/* Pack a String array. Only the string directives are meaningful for String
   elements (CRuby raises TypeError for numeric directives on a String), so we
   handle a/A/Z padding and m/M base64/quoted-printable; other directives emit
   nothing. Reads the real byte length (embedded NULs survive base64). */
const char *sp_StrArray_pack(sp_StrArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    const char *s = (idx < arr->len) ? sp_StrArray_get(arr, idx) : NULL;
    size_t sl = s ? sp_str_byte_len(s) : 0;
    if (!s) s = "";
    idx++;
    if (spec == 'a' || spec == 'A' || spec == 'Z') {
      size_t want = (count < 0) ? sl : (size_t)count;
      if (spec == 'Z' && count < 0) want = sl + 1;
      size_t take = sl < want ? sl : want;
      pk_append(&buf, &len, &cap, s, take);
      if (take < want) {
        char pad = (spec == 'A') ? ' ' : 0;
        for (size_t pi = 0; pi < want - take; pi++) pk_append(&buf, &len, &cap, &pad, 1);
      }
    } else if (spec == 'm' || spec == 'M') {
      pk_str_directive(spec, count, s, sl, &buf, &len, &cap);
    } else if (spec == 'H' || spec == 'h' || spec == 'B' || spec == 'b' || spec == 'u') {
      pk_str_bytes_directive(spec, count, s, sl, &buf, &len, &cap);
    }
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

/* ---------- Unpack entry point ---------- */

/* base64 sextet value for an ASCII byte, or -1 for a non-alphabet char. */
static int uk_b64val(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}
/* Decode `n` base64 bytes from `src` into a fresh GC string (the `m` directive).
   Non-alphabet bytes (whitespace, newlines) are skipped; `=` ends the data. In
   `strict` mode (the `m0` count) any non-alphabet byte is an ArgumentError, as
   RFC 4648 requires. */
static char *uk_b64_decode_strict(const char *src, size_t n, int strict) {
  char *out = sp_str_alloc(n);   /* decoded size <= input size */
  size_t o = 0;
  int quad[4], qi = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)src[i];
    if (ch == '=') break;
    int val = uk_b64val(ch);
    if (val < 0) { if (strict) sp_raise_cls("ArgumentError", "invalid base64"); continue; }
    quad[qi++] = val;
    if (qi == 4) {
      out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
      out[o++] = (char)((quad[1] << 4) | (quad[2] >> 2));
      out[o++] = (char)((quad[2] << 6) | quad[3]);
      qi = 0;
    }
  }
  if (qi >= 2) {
    out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
    if (qi >= 3) out[o++] = (char)((quad[1] << 4) | (quad[2] >> 2));
  }
  out[o] = 0; sp_str_set_len(out, o);
  return out;
}
static char *uk_b64_decode(const char *src, size_t n) { return uk_b64_decode_strict(src, n, 0); }
/* Decode `n` quoted-printable bytes into a fresh GC string (the `M` directive):
   `=XX` is a hex byte, `=\n` (soft line break) is dropped, everything else is
   literal. */
static char *uk_qp_decode(const char *src, size_t n) {
  char *out = sp_str_alloc(n);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)src[i];
    if (ch == '=' && i + 2 < n) {
      char h1 = src[i + 1], h2 = src[i + 2];
      int d1 = (h1 >= '0' && h1 <= '9') ? h1 - '0' : (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10
             : (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10 : -1;
      int d2 = (h2 >= '0' && h2 <= '9') ? h2 - '0' : (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10
             : (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10 : -1;
      if (d1 >= 0 && d2 >= 0) { out[o++] = (char)((d1 << 4) | d2); i += 2; continue; }
    }
    if (ch == '=' && i + 1 < n && (src[i + 1] == '\n')) { i += 1; continue; }
    if (ch == '=' && i + 2 < n && src[i + 1] == '\r' && src[i + 2] == '\n') { i += 2; continue; }
    out[o++] = (char)ch;
  }
  out[o] = 0; sp_str_set_len(out, o);
  return out;
}

sp_PolyArray *sp_str_unpack_off(const char *str, const char *fmt, mrb_int byteoff);
sp_PolyArray *sp_str_unpack(const char *str, const char *fmt) { return sp_str_unpack_off(str, fmt, 0); }

/* String#unpack(fmt, offset: n): decode starting at byte offset n. A negative
   offset or one past the end raises ArgumentError, matching MRI. */
sp_PolyArray *sp_str_unpack_off(const char *str, const char *fmt, mrb_int byteoff) {
  if (!str) sp_nil_recv("unpack");
  /* Root the source string across every allocation below: `str` is very often a
     fresh, otherwise-unrooted substring (`data[4, 4].unpack1('V')` in doom's
     binary WAD parsing). sp_PolyArray_new and the per-element boxes/pushes can
     each trigger a GC that would sweep the input out from under us (a
     use-after-free read of the freed bytes). Root the result array too. */
  SP_GC_ROOT_STR(str);
  /* `fmt` is walked (via `p`) across the very same allocations, so it is just as
     exposed as `str`: a fresh, unrooted format string (e.g. `unpack1("V" * n)`)
     could be swept mid-parse, leaving `p` dangling. Root it too. NUL-safe. */
  SP_GC_ROOT_STR(fmt);
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  if (!str || !fmt) return out;
  /* sp_ext_str_byte_len honors the heap-string header so embedded
     NULs (binary data) don't truncate the source. */
  size_t slen = sp_str_byte_len(str);
  if (byteoff < 0) sp_raise_cls("ArgumentError", "offset can't be negative");
  if ((size_t)byteoff > slen) sp_raise_cls("ArgumentError", "offset outside of string");
  size_t off = (size_t)byteoff;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    size_t fsize = 0;
    switch (spec) {
      case 'C': case 'c': case 'x': fsize = 1; break;
      case 'n': case 'v': case 's': case 'S': fsize = 2; break;
      case 'N': case 'V': case 'l': case 'L': fsize = 4; break;
      case 'f': case 'F': case 'e': case 'g': fsize = 4; break;
      case 'q': case 'Q': fsize = 8; break;
      case 'd': case 'D': case 'E': case 'G': fsize = 8; break;
      default: fsize = 0; break;
    }
    if (spec == 'a' || spec == 'A' || spec == 'Z') {
      size_t take;
      if (count < 0) {
        take = slen - off;
      }
else {
        take = (size_t)count;
        if (off + take > slen) take = slen - off;
      }
      const char *src = str + off;
      if (spec == 'Z' && count < 0) {
        size_t z = 0;
        while (off + z < slen && src[z]) z++;
        char *s = sp_str_alloc(z);
        memcpy(s, src, z); s[z] = 0; sp_str_set_len(s, z);
        sp_PolyArray_push(out, sp_box_str(s));
        off += z;
        if (off < slen && str[off] == 0) off++;
      }
else {
        char *s = sp_str_alloc(take);
        memcpy(s, src, take); s[take] = 0;
        size_t real = take;
        if (spec == 'A') {
          while (real > 0 && (s[real - 1] == ' ' || s[real - 1] == 0)) real--;
          s[real] = 0;
        }
else if (spec == 'Z') {
          size_t z = 0;
          while (z < take && s[z]) z++;
          s[z] = 0;
          real = z;
        }
        sp_str_set_len(s, real);
        sp_PolyArray_push(out, sp_box_str(s));
        off += take;
      }
      continue;
    }
    /* H/h: hex-nibble string (H high nibble first, h low first). `count` is a
       nibble (hex-digit) count; `*` takes every remaining nibble. */
    if (spec == 'H' || spec == 'h') {
      size_t avail = (slen - off) * 2;
      size_t n = count < 0 ? avail : (size_t)count;
      if (n > avail) n = avail;
      char *s = sp_str_alloc(n);
      for (size_t i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)str[off + i / 2];
        int nib = (spec == 'H') ? ((i & 1) ? (byte & 15) : (byte >> 4))
                                : ((i & 1) ? (byte >> 4) : (byte & 15));
        s[i] = "0123456789abcdef"[nib];
      }
      s[n] = 0; sp_str_set_len(s, n);
      sp_PolyArray_push(out, sp_box_str(s));
      off += (n + 1) / 2;
      continue;
    }
    /* B/b: bit string (B MSB first, b LSB first). `count` is a bit count. */
    if (spec == 'B' || spec == 'b') {
      size_t avail = (slen - off) * 8;
      size_t n = count < 0 ? avail : (size_t)count;
      if (n > avail) n = avail;
      char *s = sp_str_alloc(n);
      for (size_t i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)str[off + i / 8];
        int bit = (spec == 'B') ? ((byte >> (7 - (i & 7))) & 1) : ((byte >> (i & 7)) & 1);
        s[i] = (char)('0' + bit);
      }
      s[n] = 0; sp_str_set_len(s, n);
      sp_PolyArray_push(out, sp_box_str(s));
      off += (n + 7) / 8;
      continue;
    }
    /* m: base64, M: quoted-printable. Both decode the whole remaining input into
       one string (the count/`0` modifier only tweaks strictness, not framing). */
    if (spec == 'm' || spec == 'M') {
      size_t avail = slen - off;
      /* `m0` is strict RFC 4648: reject any non-alphabet byte. */
      char *s = (spec == 'm') ? uk_b64_decode_strict(str + off, avail, count == 0)
                              : uk_qp_decode(str + off, avail);
      sp_PolyArray_push(out, sp_box_str(s));
      off = slen;
      continue;
    }
    /* u: uuencode. Each line begins with a length byte ((c-0x20)&0x3F decoded
       bytes) followed by 4-char groups that each yield 3 bytes; a zero length
       (or end of input) terminates. */
    if (spec == 'u') {
      char *s = sp_str_alloc(slen - off);   /* decoded <= input */
      size_t o = 0, i = off;
      while (i < slen) {
        int L = (((unsigned char)str[i++]) - 0x20) & 0x3F;
        if (L == 0) break;
        int produced = 0;
        while (produced < L && i < slen) {
          int c[4];
          for (int j = 0; j < 4; j++) c[j] = i < slen ? ((((unsigned char)str[i++]) - 0x20) & 0x3F) : 0;
          if (produced < L) { s[o++] = (char)((c[0] << 2) | (c[1] >> 4)); produced++; }
          if (produced < L) { s[o++] = (char)((c[1] << 4) | (c[2] >> 2)); produced++; }
          if (produced < L) { s[o++] = (char)((c[2] << 6) | c[3]); produced++; }
        }
        while (i < slen && str[i] != '\n') i++;   /* skip to end of line */
        if (i < slen) i++;
      }
      s[o] = 0; sp_str_set_len(s, o);
      sp_PolyArray_push(out, sp_box_str(s));
      off = slen;
      continue;
    }
    /* U: UTF-8 codepoints as integers. `count` codepoints, `*` all remaining. */
    if (spec == 'U') {
      int64_t got = 0;
      while ((count < 0 || got < count) && off < slen) {
        unsigned char b0 = (unsigned char)str[off];
        int len = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 0;
        /* A lead byte with no valid length, or a truncated / mis-continued
           sequence, is malformed UTF-8 -- ArgumentError, as in MRI. */
        if (len == 0 || off + (size_t)len > slen)
          sp_raise_cls("ArgumentError", "malformed UTF-8 character");
        int64_t cp = len == 1 ? b0 : (b0 & (0x7F >> len));
        for (int j = 1; j < len; j++) {
          unsigned char cb = (unsigned char)str[off + j];
          if ((cb & 0xC0) != 0x80) sp_raise_cls("ArgumentError", "malformed UTF-8 character");
          cp = (cp << 6) | (cb & 0x3F);
        }
        sp_PolyArray_push(out, sp_box_int((mrb_int)cp));
        off += len; got++;
      }
      continue;
    }
    /* w: BER-compressed integers (base-128, high bit = continuation). */
    if (spec == 'w') {
      int64_t got = 0;
      while ((count < 0 || got < count) && off < slen) {
        int64_t v = 0;
        while (off < slen) {
          unsigned char c = (unsigned char)str[off++];
          v = (v << 7) | (c & 0x7F);
          if (!(c & 0x80)) break;
        }
        sp_PolyArray_push(out, sp_box_int((mrb_int)v));
        got++;
      }
      continue;
    }
    if (fsize == 0) continue;
    /* `*` count is exactly what remains; an explicit count that runs off the end
       pads the remaining slots with nil (MRI: "a".unpack("CC") == [97, nil]). */
    int star = count < 0;
    if (star) count = (slen - off) / fsize;
    for (int64_t k = 0; k < count; k++) {
      if (off + fsize > slen) {
        if (spec != 'x') sp_PolyArray_push(out, sp_box_nil());
        continue;
      }
      const unsigned char *u = (const unsigned char *)(str + off);
      if (pk_is_flt_spec(spec)) {
        sp_PolyArray_push(out, sp_box_float((mrb_float)uk_get_flt(spec, u)));
        off += fsize;
        continue;
      }
      int64_t v = 0;
      switch (spec) {
        case 'C': v = u[0]; break;
        case 'c': v = (int8_t)u[0]; break;
        case 'n': v = ((int64_t)u[0] << 8) | u[1]; break;
        case 'N': v = ((int64_t)u[0] << 24) | ((int64_t)u[1] << 16) | ((int64_t)u[2] << 8) | u[3]; break;
        case 'v': v = ((int64_t)u[1] << 8) | u[0]; break;
        case 'V': v = ((int64_t)u[3] << 24) | ((int64_t)u[2] << 16) | ((int64_t)u[1] << 8) | u[0]; break;
        case 's': v = (int16_t)pk_get_int(u, 2, big); break;
        case 'S': v = (uint16_t)pk_get_int(u, 2, big); break;
        case 'l': v = (int32_t)pk_get_int(u, 4, big); break;
        case 'L': v = (uint32_t)pk_get_int(u, 4, big); break;
        case 'q': v = (int64_t)pk_get_int(u, 8, big); break;
        case 'Q': v = (int64_t)pk_get_int(u, 8, big); break;
        case 'x': break;
      }
      off += fsize;
      if (spec != 'x') sp_PolyArray_push(out, sp_box_int((mrb_int)v));
    }
  }
  return out;
}
