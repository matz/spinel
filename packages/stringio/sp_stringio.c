/* sp_stringio.c -- StringIO methods, a carried-C spin package (Path B), linked
   on demand when `require "stringio"` appears. The buffer is a plain malloc'd
   char array; readers hand back GC strings via the shared allocator. Compiles
   against the stable package ABI; the struct + prototypes stay in lib's
   sp_stringio.h, the interface the compiler's StringIO dispatch emits calls to. */
#include "sp_stringio.h"      /* the StringIO struct + this unit's prototypes */
#include "sp_string.h"        /* sp_String, for a boxed shared-mutable String */
#include <stdlib.h>
#include <string.h>

/* Detach a borrowed (GC-string-backed) buffer into a private malloc'd copy
   before any mutation. Spinel strings are immutable char*, so a StringIO
   constructed over one shares it read-only ("string" keeps identity) and
   copies on the first write; CRuby's write-through into the original String
   object is not representable in this string model. */
/* --share-strings: a StringIO over a shared handle (sio->str) reads its
   bytes through the handle, whose payload moves when the String grows
   through another name: the mirror fields are read again on each entry,
   and a write is published back to it (sio_publish). */
static void sio_sync(sp_StringIO *sio) {
  if (!sio->str) return;
  sio->buf = sio->str->data; sio->len = sio->str->len; sio->cap = sio->str->cap;
}
static void sio_publish(sp_StringIO *sio) {
  if (!sio->str) return;
  sio->str->len = sio->len;
  sio->buf[sio->len] = '\0';
  sp_fd_publish(sio->str);
}
static void sio_own(sp_StringIO *sio) {SP_GC_ROOT(sio);
  if (!sio->borrowed) return;
  int64_t nc = sio->len < 63 ? 63 : sio->len;
  char *nb = (char *)malloc(nc + 1);
  if (!nb) sp_oom_die();
  memcpy(nb, sio->buf, sio->len);
  nb[sio->len] = '\0';
  sio->buf = nb;
  sio->cap = nc;
  sio->borrowed = 0;
}
/* A StringIO opened on a frozen String, or handed one by #string=, takes no
   write, as CRuby's (run with frozen string literals); nor one whose String
   was frozen since, in CRuby's other words */
#define SIO_NO_READ  1
#define SIO_NO_WRITE 2
static void sio_check_write(sp_StringIO *sio) {
  if ((sio->closed & SIO_NO_WRITE) || sio->ro) sp_raise_cls("IOError", "not opened for writing");
  if (sio->str ? sp_String_is_frozen(sio->str) : sio->borrowed && sp_str_is_frozen_val(sio->buf))
    sp_raise_cls("IOError", "not modifiable string");
}
/* A closed StringIO, or one opened for output only, reads nothing: CRuby
   asks before it looks at the arguments */
static void sio_check_read(sp_StringIO *sio) {
  if (sio->closed & SIO_NO_READ) sp_raise_cls("IOError", "not opened for reading");
}
/* the sides a mode leaves closed: "r" takes no write, "w" and "a" no read,
   and a "+" takes both */
static int sio_mode_sides(const char *mode) {
  if (!mode || !mode[0] || strchr(mode, '+')) return 0;
  return mode[0] == 'r' ? SIO_NO_WRITE : mode[0] == 'w' || mode[0] == 'a' ? SIO_NO_READ : 0;
}
/* a frozen String opened for writing ("w", "a", "r+") is EACCES, as CRuby's */
static void sio_mode_check(int frozen, const char *mode) {
  if (frozen && mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+')))
    sp_raise_cls("Errno::EACCES", "Permission denied");
}
static void sio_grow(sp_StringIO *sio, int64_t need) {SP_GC_ROOT(sio);
  sio_check_write(sio);
  if (sio->str) {
    if (!sp_fd_grow(sio->str, sio->pos + need)) sp_oom_die();
    sio_sync(sio);
    return;
  }
  sio_own(sio); int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; char *nb = (char *)realloc(sio->buf, nc + 1); if (!nb) sp_oom_die(); sio->buf = nb; sio->cap = nc; }
static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_sync(sio); sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '\0'; sio_publish(sio); return dl; }

/* the bytes of a shared handle (s->str) are the handle's: it keeps them
   alive and frees them */
void sp_StringIO_free(void *p) { sp_StringIO *s = (sp_StringIO *)p; if (!s->borrowed && !s->str) free(s->buf); s->buf = NULL; }
void sp_StringIO_scan_gc(void *p) { sp_StringIO *s = (sp_StringIO *)p; if (s->borrowed && s->buf) sp_mark_string(s->buf); if (s->str) sp_gc_mark((void *)s->str); }
sp_StringIO *sp_StringIO_new(sp_int cls_id) { sp_StringIO *s = (sp_StringIO *)sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc); memset(s, 0, sizeof *s); s->cls_id = cls_id; s->buf = (char *)calloc(1, 64); if (!s->buf) sp_oom_die(); s->cap = 63; return s; }
/* Adopt the incoming GC string without copying so #string keeps identity
   with the constructor argument; the first mutation copies (sio_own). */
sp_StringIO *sp_StringIO_new_s(sp_int cls_id, const char *init) {SP_GC_ROOT_STR(init); if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String"); sp_StringIO *s = (sp_StringIO *)sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc); memset(s, 0, sizeof *s); s->cls_id = cls_id; int64_t l = (int64_t)sp_str_byte_len(init); s->buf = (char *)init; s->len = l; s->cap = l; s->borrowed = 1; s->ro = sp_str_is_frozen_val(init); return s; }
/* StringIO.new(str, mode): the mode's first char selects the initial
   content/position. "w"/"w+" truncate to empty; "a"/"a+" keep the content and
   seek to the end; "r"/"r+" and anything else keep the content at position 0.
   The mode also closes the side it does not open (sio_mode_sides). */
sp_StringIO *sp_StringIO_new_sm(sp_int cls_id, const char *init, const char *mode) {SP_GC_ROOT_STR(init);SP_GC_ROOT_STR(mode);
  if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (!mode || !mode[0]) return sp_StringIO_new_s(cls_id, init);
  sio_mode_check(sp_str_is_frozen_val(init), mode);
  char m0 = mode[0];
  sp_StringIO *s = m0 == 'w' ? sp_StringIO_new(cls_id) : sp_StringIO_new_s(cls_id, init);
  if (m0 == 'a') s->pos = s->len;
  s->closed = sio_mode_sides(mode);
  return s;
}
/* The in-place twins of the constructors (`_new` -> `_init`), for a
   subclass of StringIO (`class Buffer < StringIO`), whose instance the
   program allocates with its own instance variables after the StringIO:
   its initialize's `super(...)` sets the StringIO part up here. A second
   initialize starts over, freeing the buffer the first one owned. The
   object may be old by then, so the barrier records what it now holds. */
static void sio_init_clear(sp_StringIO *s, sp_int cls_id) {
  if (s->buf && !s->borrowed && !s->str) free(s->buf);
  memset(s, 0, sizeof *s);
  s->cls_id = cls_id;
}
void sp_StringIO_init(sp_StringIO *s, sp_int cls_id) {
  sio_init_clear(s, cls_id);
  s->buf = (char *)calloc(1, 64); if (!s->buf) sp_oom_die(); s->cap = 63;
}
void sp_StringIO_init_s(sp_StringIO *s, sp_int cls_id, const char *init) {SP_GC_ROOT(s);SP_GC_ROOT_STR(init);
  if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  sio_init_clear(s, cls_id);
  int64_t l = (int64_t)sp_str_byte_len(init);
  s->buf = (char *)init; s->len = l; s->cap = l; s->borrowed = 1; s->ro = sp_str_is_frozen_val(init);
  sp_gc_wb(s);
}
void sp_StringIO_init_sm(sp_StringIO *s, sp_int cls_id, const char *init, const char *mode) {SP_GC_ROOT(s);SP_GC_ROOT_STR(init);SP_GC_ROOT_STR(mode);
  if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (!mode || !mode[0]) { sp_StringIO_init_s(s, cls_id, init); return; }
  sio_mode_check(sp_str_is_frozen_val(init), mode);
  if (mode[0] == 'w') sp_StringIO_init(s, cls_id);
  else sp_StringIO_init_s(s, cls_id, init);
  if (mode[0] == 'a') s->pos = s->len;
  s->closed = sio_mode_sides(mode);
}
/* #string returns the buffer as a spinel String. A borrowed buffer already IS
   a spinel string (a valid sp_str_hdr precedes it), so it can be handed back
   directly and keeps identity. A written buffer is a raw malloc block with no
   header, so it must be copied into a proper String -- returning it raw let
   callers read the sp_str_hdr one block before the allocation (a String method
   or the GC string-heap walk), corrupting the heap (#3152). */
const char *sp_StringIO_string(sp_StringIO *s) {SP_GC_ROOT(s);
  /* the shared handle's bytes, as a String of their own (sp_StringIO_string_h
     answers the handle) */
  if (s->str) return sp_str_from_bytes(s->str->data, (size_t)s->str->len);
  if (!s->buf) return sp_str_empty;
  if (s->borrowed) return s->buf;
  return sp_str_from_bytes(s->buf, (size_t)s->len);
}
sp_int sp_StringIO_pos(sp_StringIO *s) { return s->pos; }
sp_int sp_StringIO_size(sp_StringIO *s) { sio_sync(s); return s->len; }
/* Binary-safe: a Ruby String may carry an embedded NUL (`[0].pack("C")`),
   so the operand is measured with its recorded length, not strlen. */
sp_int sp_StringIO_write(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)sp_str_byte_len(str)); }
void sp_StringIO_puts(sp_StringIO *s, const char *str) { int64_t l = (int64_t)sp_str_byte_len(str); sio_write(s, str, l); if (l == 0 || str[l-1] != '\n') sio_write(s, "\n", 1); }
void sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, "\n", 1); }
void sp_StringIO_print(sp_StringIO *s, const char *str) { sio_write(s, str, (int64_t)sp_str_byte_len(str)); }
sp_int sp_StringIO_putc(sp_StringIO *s, sp_int ch) { char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch; }
const char *sp_StringIO_read(sp_StringIO *s) {SP_GC_ROOT(s); sio_check_read(s); sio_sync(s); if (s->pos >= s->len) return sp_str_empty; size_t rem = s->len - s->pos; char *r = sp_str_alloc(rem); memcpy(r, s->buf + s->pos, rem); r[rem] = 0; s->pos = s->len; return r; }
/* read(n): nil at the end for a positive n, "" for read(0), and an
   ArgumentError for a negative n (CRuby) */
const char *sp_StringIO_read_n(sp_StringIO *s, sp_int n) {SP_GC_ROOT(s);
  sio_check_read(s);
  if (n < 0) sp_raise_cls("ArgumentError", sp_sprintf("negative length %lld given", (long long)n));
  sio_sync(s);
  if (s->pos >= s->len) return n > 0 ? NULL : sp_str_empty; int64_t rem = s->len - s->pos; if (n > rem) n = rem; char *r = sp_str_alloc_raw(n+1); memcpy(r, s->buf + s->pos, n); r[n] = '\0'; sp_str_set_len(r, (size_t)n); s->pos += n; return r; }
const char *sp_StringIO_gets(sp_StringIO *s) {SP_GC_ROOT(s); sio_check_read(s); sio_sync(s); if (s->pos >= s->len) return NULL; const char *st = s->buf + s->pos; const char *nl = memchr(st, '\n', s->len - s->pos); int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos; char *r = sp_str_alloc_raw(ll+1); memcpy(r, st, ll); r[ll] = '\0'; sp_str_set_len(r, (size_t)ll); s->pos += ll; s->lineno++; return r; }
/* The byte length of the character at p, n bytes available: a whole UTF-8
   sequence, or one byte where the sequence is malformed or cut short (CRuby
   hands those out a byte at a time). A binary buffer is bytes only. */
static int64_t sio_char_len(const char *p, int64_t n, int binary) {
  unsigned char c = (unsigned char)p[0];
  int64_t cl = binary ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
  if (cl > n) return 1;
  for (int64_t i = 1; i < cl; i++)
    if (((unsigned char)p[i] & 0xC0) != 0x80) return 1;
  /* an overlong form, a surrogate or a code point past U+10FFFF */
  if (cl >= 3) {
    unsigned char c2 = (unsigned char)p[1];
    if ((c == 0xE0 && c2 < 0xA0) || (c == 0xED && c2 > 0x9F) ||
        (c == 0xF0 && c2 < 0x90) || (c == 0xF4 && c2 > 0x8F)) return 1;
  }
  return cl;
}
const char *sp_StringIO_getc(sp_StringIO *s) {SP_GC_ROOT(s); sio_check_read(s); sio_sync(s); if (s->pos >= s->len) return NULL;
  /* a binary String is read a byte at a time: the shared handle's own
     mark, or a borrowed String's header */
  int binary = s->str ? (int)s->str->binary : s->borrowed && sp_str_is_binary(s->buf);
  int64_t cl = sio_char_len(s->buf + s->pos, s->len - s->pos, binary);
  char *gc = sp_str_alloc_raw(cl + 1); memcpy(gc, s->buf + s->pos, cl); gc[cl] = '\0'; sp_str_set_len(gc, (size_t)cl); s->pos += cl; return gc; }
sp_RbVal sp_StringIO_getbyte(sp_StringIO *s) { sio_check_read(s); sio_sync(s); if (s->pos >= s->len) return sp_box_nil(); return sp_box_int((int64_t)(unsigned char)s->buf[s->pos++]); }
/* readbyte and readchar: getbyte and getc that raise EOFError at the end */
sp_int sp_StringIO_readbyte(sp_StringIO *s) { sio_check_read(s); sio_sync(s); if (s->pos >= s->len) sp_raise_cls("EOFError", "end of file reached"); return (int64_t)(unsigned char)s->buf[s->pos++]; }
const char *sp_StringIO_readchar(sp_StringIO *s) {SP_GC_ROOT(s); const char *r = sp_StringIO_getc(s); if (!r) sp_raise_cls("EOFError", "end of file reached"); return r; }
sp_int sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }
sp_int sp_StringIO_seek(sp_StringIO *s, sp_int off) { if (off < 0) off = 0; s->pos = off; return 0; }
sp_int sp_StringIO_tell(sp_StringIO *s) { return s->pos; }
/* pos= moves to any offset at or past 0 (past the end reads nothing); a
   negative one is CRuby's Errno::EINVAL */
sp_int sp_StringIO_set_pos(sp_StringIO *s, sp_int p) { if (p < 0) sp_raise_cls("Errno::EINVAL", "Invalid argument"); s->pos = p; return p; }
sp_bool sp_StringIO_eof_p(sp_StringIO *s) { sio_check_read(s); sio_sync(s); return s->pos >= s->len; }
sp_int sp_StringIO_truncate(sp_StringIO *s, sp_int l) { sio_sync(s); if (l < 0) l = 0; sio_check_write(s); if (l < s->len) { sio_own(s); s->len = l; s->buf[l] = '\0'; sio_publish(s); } return 0; }
void sp_StringIO_close(sp_StringIO *s) { s->closed = SIO_NO_READ | SIO_NO_WRITE; }
sp_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed == (SIO_NO_READ | SIO_NO_WRITE); }
sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }
sp_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }
sp_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }

/* Normalized helpers so the binding stays a plain method->symbol map:
   putc with a string arg writes its first byte; lineno is a field read;
   fsync/fileno/pid are always 0 on an in-memory stream. */
/* putc(String) writes the string's first character, all of its bytes; a
   String-or-nil argument that is nil takes CRuby's path for a non-String,
   the Integer conversion, as the [:int] binding's nil does -- after the
   stream's own write check, which CRuby's putc makes first */
const char *sp_StringIO_putc_s(sp_StringIO *s, const char *str) {SP_GC_ROOT(s);SP_GC_ROOT_STR(str);
  if (!str) { sio_check_write(s); sp_raise_nil_to_int(0); }
  int64_t l = (int64_t)sp_str_byte_len(str);
  if (l > 0) sio_write(s, str, sio_char_len(str, l, sp_str_is_binary(str)));
  else sio_check_write(s);   /* putc("") writes nothing, but asks first */
  return str; }
sp_int sp_StringIO_lineno(sp_StringIO *s) { return s->lineno; }
sp_int sp_StringIO_zero(sp_StringIO *s) { (void)s; return 0; }

/* << writes and returns the receiver (chainable), unlike write's byte count. */
sp_StringIO *sp_StringIO_shl(sp_StringIO *s, const char *str) { sio_write(s, str, (int64_t)sp_str_byte_len(str)); return s; }

/* One line from the stream, read as CRuby's strio_getline reads it. `sep` NULL
   is nil (read to the end); "" is paragraph mode; a `limit` above zero ends
   the line at that many bytes, rounded up to the end of the character it
   falls in. `chomp` takes the separator off the end. nil at EOF. */
static const char *sio_getline(sp_StringIO *s, const char *sep, sp_int limit, sp_bool chomp) {SP_GC_ROOT(s);SP_GC_ROOT_STR(sep);
  sio_sync(s);
  if (s->pos >= s->len) return NULL;
  const char *st = s->buf + s->pos, *e = s->buf + s->len, *p;
  int64_t w = 0;
  if (limit > 0 && (size_t)limit < (size_t)(e - st)) {
    const char *le = st + limit;
    while (le < e && ((unsigned char)*le & 0xC0) == 0x80) le++;
    e = le;
  }
  int64_t n = sep ? (int64_t)sp_str_byte_len(sep) : 0;
  if (!sep) {
    /* nil reads up to the limit, and chomp leaves it as read */
  }
  else if (n == 0) {
    /* paragraph mode: blank lines ahead of it are skipped, and it ends after
       the run of blank lines that follows its first one, all of which it
       keeps (a "\r\n" counts as a newline there); chomp takes that run off */
    const char *pend = NULL;
    p = st;
    while (*p == '\n') { if (++p == e) return NULL; }
    st = p;
    while ((p = memchr(p, '\n', (size_t)(e - p))) != NULL && p != e) {
      p++;
      if (!((p < e && *p == '\n') || (p + 1 < e && *p == '\r' && p[1] == '\n'))) continue;
      pend = p - ((p[-2] == '\r') ? 2 : 1);
      while ((p < e && *p == '\n') || (p + 1 < e && *p == '\r' && p[1] == '\n')) p += (*p == '\r') ? 2 : 1;
      e = p;
      break;
    }
    if (chomp && pend) w = e - pend;
  }
  else if (n == 1) {
    if ((p = memchr(st, sep[0], (size_t)(e - st))) != NULL) {
      e = p + 1;
      if (chomp) w = (p > st && p[-1] == '\r') + 1;
    }
  }
  else if (n < (e - st) + (chomp ? 1 : 0)) {
    /* unless chomping, a separator that would end the stream anyway does not matter */
    for (p = st; p + n <= e; ++p) {
      if (memcmp(p, sep, (size_t)n) == 0) { e = p + n; if (chomp) w = n; break; }
    }
  }
  int64_t ll = (e - st) - w;
  char *r = sp_str_alloc_raw(ll + 1);
  memcpy(r, st, (size_t)ll);
  r[ll] = '\0';
  sp_str_set_len(r, (size_t)ll);
  s->pos = e - s->buf;
  s->lineno++;
  return r;
}

/* gets(sep): a String separator, with no limit and no chomp. */
const char *sp_StringIO_gets_sep(sp_StringIO *s, const char *sep) {SP_GC_ROOT(s);SP_GC_ROOT_STR(sep);
  sio_check_read(s);
  return sio_getline(s, sep, -1, 0);
}

/* The name CRuby's conversion errors give a value: nil, true and false
   spell themselves, Integer, Float, String and Symbol their own name, a class
   of the program, an Array and a Hash theirs, and any other builtin class
   "Object". */
static const char *sio_src_name(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_NIL: return "nil";
    case SP_TAG_BOOL: return v.v.b ? "true" : "false";
    case SP_TAG_INT: return "Integer";
    case SP_TAG_FLT: return "Float";
    case SP_TAG_STR: return "String";
    case SP_TAG_SYM: return "Symbol";
    default: break;
  }
  if (v.tag == SP_TAG_OBJ) {
    /* a String's handle box is a String */
    if (v.cls_id == SP_BUILTIN_STRBUF) return "String";
    if (v.cls_id >= 0 && sp_obj_cls_name_fn) {
      const char *cn = sp_obj_cls_name_fn((int)v.cls_id);
      if (cn) return cn;
    }
    if (sp_json_kind_fn) {
      int k = sp_json_kind_fn(v);
      if (k == 1) return "Array";
      if (k == 2) return "Hash";
    }
  }
  return "Object";
}
static void sio_type_error(sp_RbVal v, const char *into) {
  char msg[160];
  snprintf(msg, sizeof msg, "no implicit conversion of %s into %s", sio_src_name(v), into);
  sp_raise_cls("TypeError", msg);
}

/* A limit as the caller wrote it: an Integer or a Float (truncated), nil for
   none; anything else has no Integer form. */
static sp_int sio_limit_arg(sp_RbVal v) {
  if (v.tag == SP_TAG_INT) return v.v.i;
  if (v.tag == SP_TAG_FLT) return (sp_int)v.v.f;
  if (v.tag == SP_TAG_NIL) return -1;
  sio_type_error(v, "Integer");
  return -1;
}

/* A shared-mutable String held in a container is boxed as a handle; it reads
   as its live text. */
static sp_RbVal sio_arg_value(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF) return sp_box_str(sp_String_cstr((sp_String *)v.v.p));
  return v;
}

/* The arguments of gets, readline and readlines: ([sep,] [limit,] [chomp: b]).
   With one, a String or nil is the separator (nil reads to the end) and
   anything else is the limit. With two, the first is the separator and the
   second the limit (nil for none). A trailing Hash is the keywords; `chomp:`
   is the one this reader takes. */
static void sio_line_args(const sp_RbVal *a0, int n0, const char **sep, sp_int *limit, sp_bool *chomp) {
  *sep = SPL("\n"); *limit = -1; *chomp = 0;
  sp_RbVal sv[8];
  int n = 0, given = n0;
  for (int j = 0; j < n0 && j < 8; j++) sv[n++] = sio_arg_value(a0[j]);
  const sp_RbVal *a = sv;
  if (n > 0 && a[n - 1].tag == SP_TAG_OBJ && sp_json_kind_fn && sp_json_kind_fn(a[n - 1]) == 2) {
    sp_RbVal h = a[n - 1];
    sp_int hn = sp_json_len_fn(h);
    char unk[160]; int nunk = 0; size_t ul = 0;
    unk[0] = 0;
    for (sp_int i = 0; i < hn; i++) {
      sp_RbVal k, v;
      sp_json_hpair_fn(h, i, &k, &v);
      if (k.tag == SP_TAG_SYM && sp_sym_name_fn && strcmp(sp_sym_name_fn((sp_sym)k.v.i), "chomp") == 0) {
        *chomp = !(v.tag == SP_TAG_NIL || (v.tag == SP_TAG_BOOL && !v.v.b));
        continue;
      }
      if (k.tag == SP_TAG_SYM && sp_sym_name_fn && ul < sizeof unk - 64)
        ul += (size_t)snprintf(unk + ul, sizeof unk - ul, "%s:%s", nunk ? ", " : "", sp_sym_name_fn((sp_sym)k.v.i));
      nunk++;
    }
    if (nunk) {
      char msg[240];
      snprintf(msg, sizeof msg, "unknown keyword%s: %s", nunk > 1 ? "s" : "", unk);
      sp_raise_cls("ArgumentError", msg);
    }
    n--; given--;
  }
  if (given > 2) {
    char msg[80];
    snprintf(msg, sizeof msg, "wrong number of arguments (given %d, expected 0..2)", given);
    sp_raise_cls("ArgumentError", msg);
  }
  if (n == 1) {
    if (a[0].tag == SP_TAG_NIL) *sep = NULL;
    else if (a[0].tag == SP_TAG_STR) *sep = a[0].v.s ? a[0].v.s : SPL("");
    else *limit = sio_limit_arg(a[0]);
    return;
  }
  if (n == 2) {
    if (a[0].tag == SP_TAG_NIL) *sep = NULL;
    else if (a[0].tag == SP_TAG_STR) *sep = a[0].v.s ? a[0].v.s : SPL("");
    else sio_type_error(a[0], "String");
    *limit = sio_limit_arg(a[1]);
  }
}

/* gets(...), which answers "" for a zero limit and nil at the end. */
static const char *sio_gets_n(sp_StringIO *s, const sp_RbVal *a, int n) {SP_GC_ROOT(s);
  sio_check_read(s);
  const char *sep; sp_int limit; sp_bool chomp;
  sio_line_args(a, n, &sep, &limit, &chomp);
  if (limit == 0) return sp_str_empty;
  return sio_getline(s, sep, limit, chomp);
}
const char *sp_StringIO_gets_a1(sp_StringIO *s, sp_RbVal a) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a); return sio_gets_n(s, &a, 1); }
const char *sp_StringIO_gets_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b); sp_RbVal v[2] = {a, b}; return sio_gets_n(s, v, 2); }
const char *sp_StringIO_gets_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);SP_GC_ROOT_RBVAL(c); sp_RbVal v[3] = {a, b, c}; return sio_gets_n(s, v, 3); }

static const char *sio_readline_n(sp_StringIO *s, const sp_RbVal *a, int n) {SP_GC_ROOT(s);
  const char *r = sio_gets_n(s, a, n);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}
const char *sp_StringIO_readline_a1(sp_StringIO *s, sp_RbVal a) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a); return sio_readline_n(s, &a, 1); }
const char *sp_StringIO_readline_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b); sp_RbVal v[2] = {a, b}; return sio_readline_n(s, v, 2); }
const char *sp_StringIO_readline_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);SP_GC_ROOT_RBVAL(c); sp_RbVal v[3] = {a, b, c}; return sio_readline_n(s, v, 3); }

static sp_RbVal sio_readlines_n(sp_StringIO *s, const sp_RbVal *a, int n) {SP_GC_ROOT(s);
  sio_check_read(s);
  const char *sep; sp_int limit; sp_bool chomp;
  sio_line_args(a, n, &sep, &limit, &chomp);
  if (limit == 0) sp_raise_cls("ArgumentError", "invalid limit: 0 for readlines");
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  const char *l;
  while ((l = sio_getline(s, sep, limit, chomp)) != NULL) sp_PolyArray_push(r, sp_box_str(l));
  return sp_box_poly_array(r);
}
sp_RbVal sp_StringIO_readlines_a1(sp_StringIO *s, sp_RbVal a) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a); return sio_readlines_n(s, &a, 1); }
sp_RbVal sp_StringIO_readlines_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b); sp_RbVal v[2] = {a, b}; return sio_readlines_n(s, v, 2); }
sp_RbVal sp_StringIO_readlines_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);SP_GC_ROOT_RBVAL(c); sp_RbVal v[3] = {a, b, c}; return sio_readlines_n(s, v, 3); }

/* seek(off, whence): 0=SET, 1=CUR, 2=END; a negative result is EINVAL. */
sp_int sp_StringIO_seek2(sp_StringIO *s, sp_int off, sp_int whence) {SP_GC_ROOT(s);
  sio_sync(s);
  int64_t base = whence == 1 ? s->pos : whence == 2 ? s->len : 0;
  int64_t np = base + off;
  if (np < 0) sp_raise_cls("Errno::EINVAL", "Invalid argument");
  s->pos = np;
  return 0;
}

/* readline: gets that raises EOFError at end of stream (CRuby IO#readline). */
const char *sp_StringIO_readline(sp_StringIO *s) {SP_GC_ROOT(s);
  const char *r = sp_StringIO_gets(s);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}

sp_RbVal sp_StringIO_readlines(sp_StringIO *s) {SP_GC_ROOT(s);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  const char *l;
  while ((l = sp_StringIO_gets(s)) != NULL) sp_PolyArray_push(a, sp_box_str(l));
  return sp_box_poly_array(a);
}

/* Boxed-value writer behind the variadic print/puts arms. print writes the
   value's to_s; puts additionally flattens arrays (each element on its own
   line, via the generic container hooks) and terminates lines. A value kind
   this in-memory stream can't render raises rather than writing garbage. */
static int64_t sio_write_val(sp_StringIO *s, sp_RbVal v, int is_puts) {SP_GC_ROOT(s);SP_GC_ROOT_RBVAL(v);
  v = sp_rbval_str(v);
  int64_t p0 = s->pos, w = 0;
  switch (v.tag) {
    case SP_TAG_STR: { const char *t = v.v.s ? v.v.s : ""; w += sio_write(s, t, (int64_t)sp_str_byte_len(t)); break; }
    case SP_TAG_INT: { const char *t = sp_int_to_s(v.v.i); w += sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_FLT: { const char *t = sp_float_to_s(v.v.f); w += sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_BOOL: { const char *t = v.v.b ? "true" : "false"; w += sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_NIL: break;  /* puts nil -> bare newline; print nil -> nothing */
    default:
      if (is_puts && sp_json_kind_fn && sp_json_kind_fn(v) == 1) {
        sp_int n = sp_json_len_fn(v);
        for (sp_int i = 0; i < n; i++) w += sio_write_val(s, sp_json_aref_fn(v, i), 1);
        return w;  /* elements each terminated their own line */
      }
      /* anything else writes its #to_s, as IO#write and Kernel#print do. A
         user #to_s answers a Ruby String, measured by its recorded length so
         an embedded NUL survives; the generic renderer may answer a static
         class or symbol name with no length in front of it. */
      if (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && v.v.p && sp_obj_to_s_fn) {
        const char *t = sp_obj_to_s_fn((int)v.cls_id, v.v.p);
        if (t) { w += sio_write(s, t, (int64_t)sp_str_byte_len(t)); break; }
      }
      if (sp_poly_to_s_fn) {
        const char *t = sp_poly_to_s_fn(v);
        w += sio_write(s, t, (int64_t)strlen(t)); break;
      }
      sp_raise_cls("TypeError", "can't write value to StringIO");
  }
  /* terminate the line unless THIS value's bytes already ended with one */
  if (is_puts && (s->pos == p0 || s->buf[s->pos - 1] != '\n')) w += sio_write(s, "\n", 1);
  return w;
}

/* print asks for the write side before its values, which may write
   nothing (print(nil)), as CRuby's does; puts does not ask for an empty one */
void sp_StringIO_print_v1(sp_StringIO *s, sp_RbVal a) { sio_check_write(s); sio_write_val(s, a, 0); }
void sp_StringIO_print_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) { sio_check_write(s); sio_write_val(s, a, 0); sio_write_val(s, b, 0); }
void sp_StringIO_print_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2) { sio_check_write(s); sio_write_val(s, a, 0); sio_write_val(s, b, 0); sio_write_val(s, c2, 0); }
void sp_StringIO_puts_v1(sp_StringIO *s, sp_RbVal a) { sio_write_val(s, a, 1); }
void sp_StringIO_puts_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) { sio_write_val(s, a, 1); sio_write_val(s, b, 1); }
void sp_StringIO_puts_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2) { sio_write_val(s, a, 1); sio_write_val(s, b, 1); sio_write_val(s, c2, 1); }
/* print and puts past three arguments: the :rest binding hands over a count
   and the boxed arguments, each rooted while the ones before it are written */
static int64_t sio_write_vals(sp_StringIO *s, sp_int n, sp_RbVal *v, int is_puts) {SP_GC_ROOT(s);
  SP_GC_SAVE();
  for (sp_int i = 0; i < n; i++) _sp_gc_root_push((void **)((uintptr_t)&v[i] | (uintptr_t)1));
  int64_t w = 0;
  for (sp_int i = 0; i < n; i++) w += sio_write_val(s, v[i], is_puts);
  return w;
}
void sp_StringIO_print_va(sp_StringIO *s, sp_int n, sp_RbVal *v) { sio_check_write(s); sio_write_vals(s, n, v, 0); }
void sp_StringIO_puts_va(sp_StringIO *s, sp_int n, sp_RbVal *v) { sio_write_vals(s, n, v, 1); }
sp_int sp_StringIO_write_va(sp_StringIO *s, sp_int n, sp_RbVal *v) { if (n > 0) sio_check_write(s); return sio_write_vals(s, n, v, 0); }  /* write(nil) writes nothing, but asks first */
/* read(*args), when the count is only known at run time: read, read(nil)
   and read(len). A buffer to read into is not supported here. */
const char *sp_StringIO_read_va(sp_StringIO *s, sp_int n, sp_RbVal *v) {
  sio_check_read(s);
  if (n > 2) {
    char msg[80];
    snprintf(msg, sizeof msg, "wrong number of arguments (given %lld, expected 0..2)", (long long)n);
    sp_raise_cls("ArgumentError", msg);
  }
  if (n == 2) sp_raise_cls("NotImplementedError", "StringIO#read into a buffer");
  if (n == 0 || v[0].tag == SP_TAG_NIL) return sp_StringIO_read(s);
  if (v[0].tag == SP_TAG_INT) return sp_StringIO_read_n(s, v[0].v.i);
  if (v[0].tag == SP_TAG_FLT) {
    double f = v[0].v.f;
    /* a Float out of Integer's range raises as CRuby's does, not cast */
    if (!(f > -9223372036854775808.0 && f < 9223372036854775808.0)) {
      char msg[96];
      if (f != f) snprintf(msg, sizeof msg, "float NaN out of range of integer");
      else if (f == 1.0 / 0.0) snprintf(msg, sizeof msg, "float Inf out of range of integer");
      else if (f == -1.0 / 0.0) snprintf(msg, sizeof msg, "float -Inf out of range of integer");
      else snprintf(msg, sizeof msg, "float %-.10g out of range of integer", f);
      sp_raise_cls("RangeError", msg);
    }
    return sp_StringIO_read_n(s, (sp_int)f);
  }
  sio_type_error(v[0], "Integer");
  return NULL;
}

/* --share-strings: a StringIO over the String it is opened on, as CRuby's:
   the shared handle holds the bytes (sio_sync), so a write changes that
   String and a change to it through another name shows in the next read.
   The StringIO's scan keeps the handle alive, and its finalizer leaves the
   handle's bytes alone. */
sp_StringIO *sp_StringIO_new_h(sp_int cls_id, sp_String *str) {SP_GC_ROOT(str);
  if (!str) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  sp_StringIO *s = (sp_StringIO *)sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc);
  memset(s, 0, sizeof *s);
  s->cls_id = cls_id;
  s->str = str;
  s->ro = sp_String_is_frozen(str);
  sio_sync(s);
  return s;
}
/* with a mode: "w" truncates the String, "a" starts at its end */
sp_StringIO *sp_StringIO_new_hm(sp_int cls_id, sp_String *str, const char *mode) {SP_GC_ROOT(str);SP_GC_ROOT_STR(mode);
  sio_mode_check(str && sp_String_is_frozen(str), mode);
  sp_StringIO *s = sp_StringIO_new_h(cls_id, str);
  SP_GC_ROOT(s);
  char m0 = mode ? mode[0] : 0;
  if (m0 == 'w') sp_StringIO_truncate(s, 0);
  if (m0 == 'a') s->pos = s->len;
  s->closed = sio_mode_sides(mode);
  return s;
}
/* StringIO.new: over a String of its own, which #string answers */
sp_StringIO *sp_StringIO_new_hn(sp_int cls_id) {
  return sp_StringIO_new_h(cls_id, sp_String_new_fresh(sp_str_empty));
}
/* the in-place twins of the three above, for a subclass (sp_StringIO_init) */
void sp_StringIO_init_h(sp_StringIO *s, sp_int cls_id, sp_String *str) {SP_GC_ROOT(s);SP_GC_ROOT(str);
  if (!str) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  sio_init_clear(s, cls_id);
  s->str = str;
  s->ro = sp_String_is_frozen(str);
  sio_sync(s);
  sp_gc_wb(s);
}
void sp_StringIO_init_hm(sp_StringIO *s, sp_int cls_id, sp_String *str, const char *mode) {SP_GC_ROOT(s);SP_GC_ROOT(str);SP_GC_ROOT_STR(mode);
  sio_mode_check(str && sp_String_is_frozen(str), mode);
  sp_StringIO_init_h(s, cls_id, str);
  char m0 = mode ? mode[0] : 0;
  if (m0 == 'w') sp_StringIO_truncate(s, 0);
  if (m0 == 'a') s->pos = s->len;
  s->closed = sio_mode_sides(mode);
}
void sp_StringIO_init_hn(sp_StringIO *s, sp_int cls_id) {SP_GC_ROOT(s);
  sp_StringIO_init_h(s, cls_id, sp_String_new_fresh(sp_str_empty));
}
/* #string=: the StringIO keeps that String from now on, read from its start
   (a frozen one takes no write, nor one opened on a frozen String); the
   String it kept is no longer its own. The value is the String. */
static void sio_reopen(sp_StringIO *s) {
  if (!s->borrowed && !s->str) free(s->buf);
  s->buf = NULL; s->len = s->cap = s->pos = s->lineno = 0; s->borrowed = 0;
  sp_gc_wb(s);
}
const char *sp_StringIO_set_string(sp_StringIO *s, const char *str) {SP_GC_ROOT(s);SP_GC_ROOT_STR(str);
  if (!str) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  sio_reopen(s);
  s->str = NULL; s->buf = (char *)str; s->len = s->cap = (int64_t)sp_str_byte_len(str); s->borrowed = 1;
  s->ro = (s->ro & 1) | (sp_str_is_frozen_val(str) ? 2 : 0);
  return str;
}
sp_String *sp_StringIO_set_string_h(sp_StringIO *s, sp_String *str) {SP_GC_ROOT(s);SP_GC_ROOT(str);
  if (!str) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  sio_reopen(s);
  s->str = str;
  s->ro = (s->ro & 1) | (sp_String_is_frozen(str) ? 2 : 0);
  sio_sync(s);
  return str;
}
/* #string as the shared handle itself. One opened on no handle takes one
   over its bytes now, which then holds them; over a frozen String it is
   frozen, as #string answers that String itself, and takes no write. */
sp_String *sp_StringIO_string_h(sp_StringIO *s) {SP_GC_ROOT(s);
  if (!s->str) {
    sp_String *h = sp_String_new_len(s->buf ? s->buf : "", s->buf ? s->len : 0);
    if (s->borrowed && sp_str_is_frozen_val(s->buf)) sp_String_freeze(h);
    if (!s->borrowed) free(s->buf);
    s->borrowed = 0;
    /* (s can be old by now: the barrier keeps the new handle it holds) */
    sp_gc_wb(s);
    s->str = h;
    sio_sync(s);
  }
  return s->str;
}
