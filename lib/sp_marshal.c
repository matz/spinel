/* sp_marshal.c -- Marshal.dump / Marshal.load, split out of spinel_rt.h.

   A standalone translation unit. The read side uses the generic sp_json_* hooks
   (sp_gc.h); the construction side uses the sp_marshal_v vtable the generated TU
   installs at startup (sp_tu_init). Result containers are built through vtable
   wrappers and rooted with SP_GC_ROOT (now shared via sp_gc.h) so a nested
   allocation during the parse can't free a partially-built value. CRuby 4.8 wire
   format; see sp_marshal.h. */
#include "sp_marshal.h"   /* sp_gc.h: sp_RbVal, hooks, SP_GC_ROOT, cls_ids */
#include "sp_alloc.h"     /* sp_str_alloc_raw, sp_str_set_len, sp_str_byte_len, sp_float_to_s */
#include "sp_dtoa.h"      /* sp_format_float / sp_read_float (locale-independent) */
#include "sp_string.h"    /* sp_String: a shared String handle in a box */
#include "sp_range.h"     /* the three Range kinds, their boxes and constructors */
#include "sp_exc.h"       /* sp_Exception: an exception's message and backtrace */
#include <string.h>
#include <strings.h>   /* strcasecmp: an encoding name is matched ignoring case */
#include "sp_setjmp.h"
#include <math.h>

/* Bignum codec lives in lib/sp_bigint.c. */
typedef struct sp_Bigint sp_Bigint;
int sp_bigint_sign(sp_Bigint *b);
size_t sp_bigint_byte_len(sp_Bigint *b);
size_t sp_bigint_to_le_bytes(sp_Bigint *b, unsigned char *out, size_t cap);
sp_Bigint *sp_bigint_from_le_bytes(int negative, const unsigned char *bytes, size_t n);
int sp_bigint_mag_u64(sp_Bigint *b, uint64_t *out);

/* Inline sp_RbVal constructors (state-free; avoid pulling spinel_rt.h). Heap
   value types (array/hash/complex/rational) go through the vtable instead. */
static sp_RbVal mk_nil(void)        { sp_RbVal r; r.tag = SP_TAG_NIL;  r.cls_id = 0; r.v.i = 0; return r; }
static sp_RbVal mk_bool(sp_bool b) { sp_RbVal r; r.tag = SP_TAG_BOOL; r.cls_id = 0; r.v.i = (b != 0); return r; }  /* full union word: bool equality reads v.i */
static sp_RbVal mk_int(sp_int n)   { sp_RbVal r; r.tag = SP_TAG_INT;  r.cls_id = 0; r.v.i = n; return r; }
static sp_RbVal mk_float(sp_float f){ sp_RbVal r; r.tag = SP_TAG_FLT;  r.cls_id = 0; r.v.f = f; return r; }
/* A Complex part as Marshal writes it: an Integer part (no Float flag) that
   fits the slot as an Integer, anything else as a Float */
static sp_RbVal sp_mar_cplx_part(sp_float v, int is_f) {
  if (!is_f && v == floor(v) && v >= -9223372036854775808.0 && v < 9223372036854775808.0 &&
      (sp_float)(sp_int)(int64_t)v == v)
    return mk_int((sp_int)(int64_t)v);
  return mk_float(v);
}
static sp_RbVal mk_sym(sp_sym s)    { sp_RbVal r; r.tag = SP_TAG_SYM;  r.cls_id = 0; r.v.i = (sp_int)s; return r; }
static sp_RbVal mk_strbuf(void *h)  { sp_RbVal r; r.tag = SP_TAG_OBJ; r.cls_id = SP_BUILTIN_STRBUF; r.v.p = h; return r; }
static sp_RbVal mk_bigint(void *p)  { sp_RbVal r; r.tag = SP_TAG_BIGINT; r.cls_id = 0; r.v.p = p; return r; }
static sp_float poly_f(sp_RbVal v) { return v.tag == SP_TAG_FLT ? v.v.f : (sp_float)v.v.i; }
static sp_int   poly_i(sp_RbVal v) { return v.tag == SP_TAG_INT ? v.v.i : (sp_int)v.v.f; }

static void mar_raise(const char *cls, const char *msg) {
  if (sp_marshal_v.raise) sp_marshal_v.raise(cls, msg);
}
/* The exception hooks the generated unit defines (lib/spinel_rt.h). */
void sp_exc_arm(jmp_buf b);
void sp_exc_disarm(void);
const char *sp_exc_cur_cls(void);
const char *sp_exc_cur_msg(void);
void *sp_exc_cur_obj(void);
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);

/* ---- dump ---- */
static void sp_mar_raw(sp_mar_buf *b, const char *s, size_t n) {SP_GC_ROOT_STR(s); for (size_t i = 0; i < n; i++) sp_mar_b(b, (unsigned char)s[i]); }
void sp_mar_b(sp_mar_buf *b, unsigned char c) {
  if (b->len >= b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->p = (char *)realloc(b->p, b->cap); if (!b->p) sp_oom_die(); }
  b->p[b->len++] = (char)c;
}
/* CRuby's variable-length signed long encoding (Fixnums + every length/count). */
void sp_mar_long(sp_mar_buf *b, long n) {
  if (n == 0) { sp_mar_b(b, 0); return; }
  if (0 < n && n < 123) { sp_mar_b(b, (unsigned char)(n + 5)); return; }
  if (-124 < n && n < 0) { sp_mar_b(b, (unsigned char)((n - 5) & 0xff)); return; }
  unsigned char buf[1 + sizeof(long)];
  long v = n; int i;
  for (i = 1; i <= (int)sizeof(long); i++) {
    buf[i] = (unsigned char)(v & 0xff);
    v >>= 8;
    if (v == 0) { buf[0] = (unsigned char)i; break; }
    if (v == -1) { buf[0] = (unsigned char)(256 - i); break; }
  }
  sp_mar_raw(b, (char *)buf, (size_t)i + 1);
}
static void sp_mar_bytes(sp_mar_buf *b, const char *s, size_t n) {SP_GC_ROOT_STR(s); sp_mar_long(b, (long)n); sp_mar_raw(b, s, n); }
void sp_mar_sym(sp_mar_buf *b, const char *name) {SP_GC_ROOT_STR(name);
  for (int i = 0; i < b->nws; i++)
    if (!strcmp(b->wsyms[i], name)) { sp_mar_b(b, ';'); sp_mar_long(b, i); return; }
  if (b->nws >= b->cws) { b->cws = b->cws ? b->cws * 2 : 8; b->wsyms = (char **)realloc(b->wsyms, sizeof(char *) * (size_t)b->cws); if (!b->wsyms) sp_oom_die(); }
  char *dup = strdup(name);
  if (!dup) sp_oom_die();
  b->wsyms[b->nws++] = dup;
  sp_mar_b(b, ':'); sp_mar_bytes(b, name, strlen(name));
}
/* CRuby's Marshal float text: the shortest %.g that round-trips, exponent
   normalized ("1e+02" -> "1e2"); 0.0 is "0", 100.0 is "1e2", 0.1 is "0.1". */
static const char *sp_mar_float_str(double f, char *buf, size_t bufsz) {SP_GC_ROOT_STR(buf);
  for (int prec = 1; prec <= 17; prec++) {
    sp_format_float(f, buf, bufsz, 'g', prec, '\0');   /* locale-independent */
    double rt = 0.0; sp_read_float(buf, NULL, &rt);
    if (rt == f) break;
  }
  char *e = strchr(buf, 'e');
  if (e) {                       /* drop '+' and leading exponent zeros */
    char *d = e + 1; int neg = 0;
    if (*d == '+') d++;
    else if (*d == '-') { neg = 1; d++; }
    while (*d == '0' && d[1]) d++;
    char tail[32]; snprintf(tail, sizeof tail, "%s%s", neg ? "-" : "", d);
    snprintf(e + 1, bufsz - (size_t)(e + 1 - buf), "%s", tail);
  }
  return buf;
}
/* If `ptr` (object identity; NULL for an identity-less linkable like Float) was
   already written, emit `@id` and return 1; else reserve the next id, return 0. */
static int sp_mar_seen(sp_mar_buf *b, void *ptr) {
  if (ptr) for (int i = 0; i < b->nl; i++) if (b->lptr[i] == ptr) { sp_mar_b(b, '@'); sp_mar_long(b, b->lid[i]); return 1; }
  int id = b->link_next++;
  if (ptr) {
    if (b->nl >= b->cl) {
      b->cl = b->cl ? b->cl * 2 : 16;
      b->lptr = (void **)realloc(b->lptr, sizeof(void *) * (size_t)b->cl);
      b->lid = (int *)realloc(b->lid, sizeof(int) * (size_t)b->cl);
      if (!b->lptr || !b->lid) sp_oom_die();
    }
    b->lptr[b->nl] = ptr; b->lid[b->nl] = id; b->nl++;
  }
  return 0;
}
/* A Hash: `{` and its pairs, or, with a default value, `}`, the pairs and
   the default after them, as CRuby writes it; a default proc cannot be
   written (CRuby's TypeError). */
static void sp_mar_w_hash(sp_mar_buf *b, sp_RbVal v) {
  int has_proc = 0;
  sp_RbVal d = sp_marshal_v.hash_default ? sp_marshal_v.hash_default(v, &has_proc) : mk_nil();
  if (has_proc) mar_raise("TypeError", "can't dump hash with default proc");
  SP_GC_ROOT_RBVAL(d);
  sp_mar_b(b, d.tag == SP_TAG_NIL ? '{' : '}');
  sp_int n = sp_json_len_fn(v);
  sp_mar_long(b, n);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, val; sp_json_hpair_fn(v, i, &k, &val);
    sp_mar_w(b, k); sp_mar_w(b, val);
  }
  if (d.tag != SP_TAG_NIL) sp_mar_w(b, d);
}
static void sp_mar_w_string(sp_mar_buf *b, const char *bytes, size_t len, int binary, void *identity);

static void sp_mar_w_range_bound(sp_mar_buf *b, sp_RbVal end, int symbol_name) {
  if (symbol_name && end.tag != SP_TAG_NIL) {
    /* A Symbol bound is written as a String record. Its spelling shares no
       object link with the String returned by Symbol#name. */
    sp_String *name = end.v.p;
    sp_mar_w_string(b, name->data, (size_t)name->len, name->binary, NULL);
    return;
  }
  sp_mar_w(b, end);
}

/* A Range (Integer, Float or String), as CRuby writes one: an `o` record of
   class Range with its exclude-end flag and its two ends, in that order. An
   absent end (beginless, endless) is nil. Each kind reads its own layout:
   an Integer Range flags an absent end (nobeg / noend) and keeps a
   Float end beside an Integer begin in fend; a Float Range records which
   ends it was written without, or as Integers (sp_types.h); a String Range
   writes its endpoint handles and holds NULL for an absent end. */
static void sp_mar_w_range(sp_mar_buf *b, sp_RbVal v) {
  sp_RbVal bg = mk_nil(), ed = mk_nil();
  int excl, symbol_names = 0;
  SP_GC_ROOT_RBVAL(bg); SP_GC_ROOT_RBVAL(ed);
  if (v.cls_id == SP_BUILTIN_STR_RANGE) {
    sp_StrRange s = sp_srange_value(v.v.p);
    if (s.first) bg = mk_strbuf(s.first);
    if (s.last) ed = mk_strbuf(s.last);
    excl = s.excl != 0;
    symbol_names = s.sym;
  }
  else if (v.cls_id == SP_BUILTIN_FLOAT_RANGE) {
    sp_FloatRange *f = (sp_FloatRange *)v.v.p;
    if (!(f->omitted & 1)) bg = (f->omitted & SP_FRANGE_INT_BEGIN) ? mk_int((sp_int)f->first) : mk_float(f->first);
    if (!(f->omitted & 2)) ed = (f->omitted & SP_FRANGE_INT_END) ? mk_int((sp_int)f->last) : mk_float(f->last);
    excl = f->excl != 0;
  }
  else {
    sp_Range *q = (sp_Range *)v.v.p;
    if (!q->nobeg) bg = mk_int(q->first);
    if (q->fe) ed = mk_float(q->fend);
    else if (!q->noend) ed = mk_int(q->last);
    excl = sp_range_excl_end(*q) != 0;
  }
  sp_mar_b(b, 'o'); sp_mar_sym(b, "Range"); sp_mar_long(b, 3);
  sp_mar_sym(b, "excl"); sp_mar_w(b, mk_bool(excl));
  sp_mar_sym(b, "begin"); sp_mar_w_range_bound(b, bg, symbol_names);
  sp_mar_sym(b, "end"); sp_mar_w_range_bound(b, ed, symbol_names);
}
/* An Integer: a Fixnum record `i` while it fits CRuby's 31-bit marshal
   Fixnum (-2**30 ... 2**30 - 1), else the Bignum record `l` -- a sign, a
   count of 16-bit words and the magnitude's bytes, least significant first --
   as CRuby writes a Fixnum past that range. The value takes a link id, as
   the loader gives one to every `l` it reads. */
static void sp_mar_w_int(sp_mar_buf *b, sp_int n) {
  int64_t w = (int64_t)n;
  if (w >= -((int64_t)1 << 30) && w < ((int64_t)1 << 30)) { sp_mar_b(b, 'i'); sp_mar_long(b, (long)n); return; }
  sp_mar_seen(b, NULL);
  uint64_t m = w < 0 ? (uint64_t)0 - (uint64_t)w : (uint64_t)w;
  size_t nbytes = 0;
  for (uint64_t t = m; t; t >>= 8) nbytes++;
  size_t nwords = (nbytes + 1) / 2;
  sp_mar_b(b, 'l'); sp_mar_b(b, w < 0 ? '-' : '+'); sp_mar_long(b, (long)nwords);
  for (size_t i = 0; i < nwords * 2; i++) sp_mar_b(b, (unsigned char)(i < 8 ? m >> (8 * i) : 0));
}
static void sp_mar_w_string(sp_mar_buf *b, const char *bytes, size_t len, int binary, void *identity) {
  if (sp_mar_seen(b, identity)) return;
  if (!binary) sp_mar_b(b, 'I');
  sp_mar_b(b, '"'); sp_mar_bytes(b, bytes, len);
  if (!binary) { sp_mar_long(b, 1); sp_mar_sym(b, "E"); sp_mar_b(b, 'T'); }
}
/* An exception, as CRuby writes one: `o`, its class, then `mesg` (the
   message, nil when none was given) and `bt` (the backtrace set on it, else
   nil). `nivar` more ivars follow: a user subclass's own, written by its
   sp_marshal_obj_dump arm. */
void sp_mar_w_exc_head(sp_mar_buf *b, const char *cls, void *p, long nivar) {
  sp_Exception *e = (sp_Exception *)p;
  const char *m = sp_exc_msg_text(e);
  sp_mar_b(b, 'o'); sp_mar_sym(b, cls); sp_mar_long(b, 2 + nivar);
  sp_mar_sym(b, "mesg");
  if (m) sp_mar_w_string(b, m, sp_str_byte_len(m), sp_str_is_binary(m), e->msg_h ? e->msg_h : (void *)m);
  else sp_mar_b(b, '0');
  sp_mar_sym(b, "bt"); sp_mar_w(b, e->backtrace ? sp_box_str_array(e->backtrace) : mk_nil());
}
void sp_mar_w_body(sp_mar_buf *b, sp_RbVal v) {
  if (sp_json_kind_fn(v) == 2) { sp_mar_w_hash(b, v); return; }
  sp_mar_b(b, '['); sp_int n = sp_json_len_fn(v); sp_mar_long(b, n);
  for (sp_int i = 0; i < n; i++) sp_mar_w(b, sp_json_aref_fn(v, i));
}
/* The head of a builtin subclass instance's record, as CRuby writes it:
   `I` when ivars follow, `C`, the class, the builtin's body, and the count
   of the ivars after it, which the caller writes. A String's body is its
   bytes (`"`), and its encoding is one more ivar, `:E` true, written first
   unless the String is binary; an Array's or a Hash's is its elements. */
void sp_mar_w_bsub(sp_mar_buf *b, const char *nm, sp_RbVal v, long niv) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF) {
    sp_String *h = (sp_String *)v.v.p;
    int bin = h && h->binary;
    long tot = niv + (bin ? 0 : 1);
    if (tot) sp_mar_b(b, 'I');
    sp_mar_b(b, 'C'); sp_mar_sym(b, nm);
    sp_mar_b(b, '"'); sp_mar_bytes(b, h ? h->data : "", h ? (size_t)h->len : 0);
    if (tot) sp_mar_long(b, tot);
    if (!bin) { sp_mar_sym(b, "E"); sp_mar_b(b, 'T'); }
    return;
  }
  if (niv) sp_mar_b(b, 'I');
  sp_mar_b(b, 'C'); sp_mar_sym(b, nm);
  sp_mar_w_body(b, v);
  if (niv) sp_mar_long(b, niv);
}
void sp_mar_w(sp_mar_buf *b, sp_RbVal v) {
  /* an Array or Hash subclass instance, boxed as its builtin (#7449): its
     class's own record, `C` with the class, the builtin's body and the ivars
     (sp_marshal_v.obj_dump), so it loads back as an instance of the class */
  if (v.tag == SP_TAG_OBJ && sp_bsub_cls_fn) {
    int k = sp_bsub_cls_fn(v);
    if (k >= 0) {
      if (sp_mar_seen(b, v.v.p)) return;
      if (!sp_marshal_v.obj_dump || !sp_marshal_v.obj_dump(b, k, v.v.p))
        mar_raise("TypeError", "no marshal_dump is defined for this object");
      return;
    }
  }
  switch (v.tag) {
    case SP_TAG_NIL:  sp_mar_b(b, '0'); break;
    case SP_TAG_BOOL: sp_mar_b(b, v.v.b ? 'T' : 'F'); break;
    case SP_TAG_INT:  sp_mar_w_int(b, v.v.i); break;
    case SP_TAG_FLT: {
      if (sp_mar_seen(b, NULL)) break;
      sp_mar_b(b, 'f');
      sp_float f = v.v.f; const char *fs; char fbuf[64];
      if (isnan(f)) fs = "nan";
      else if (isinf(f)) fs = f < 0 ? "-inf" : "inf";
      else fs = sp_mar_float_str((double)f, fbuf, sizeof fbuf);
      sp_mar_bytes(b, fs, strlen(fs));
      break;
    }
    case SP_TAG_SYM:  sp_mar_sym(b, sp_sym_name_fn ? sp_sym_name_fn((sp_sym)v.v.i) : ""); break;
    case SP_TAG_OBJ:
      if (v.cls_id == SP_BUILTIN_COMPLEX) {
        if (sp_mar_seen(b, v.v.p)) break;
        const sp_Complex *c = (const sp_Complex *)v.v.p;
        sp_mar_b(b, 'U'); sp_mar_sym(b, "Complex");
        sp_mar_seen(b, NULL);
        sp_mar_b(b, '['); sp_mar_long(b, 2);
        /* each part as its own kind, as CRuby writes it: an Integer part as
           an Integer, so Complex(1, 2) loads back as (1+2i), eql? to it */
        sp_mar_w(b, sp_mar_cplx_part(c->re, c->fl & SP_CPLX_RE_F));
        sp_mar_w(b, sp_mar_cplx_part(c->im, c->fl & SP_CPLX_IM_F));
      }
      else if (v.cls_id == SP_BUILTIN_RATIONAL) {
        if (sp_mar_seen(b, v.v.p)) break;
        sp_int *q = (sp_int *)v.v.p;  /* sp_Rational = {num, den} */
        sp_mar_b(b, 'U'); sp_mar_sym(b, "Rational");
        sp_mar_seen(b, NULL);
        sp_mar_b(b, '['); sp_mar_long(b, 2);
        sp_mar_w(b, mk_int(q[0])); sp_mar_w(b, mk_int(q[1]));
      }
      else if (v.cls_id == SP_BUILTIN_RANGE || v.cls_id == SP_BUILTIN_FLOAT_RANGE || v.cls_id == SP_BUILTIN_STR_RANGE) {
        if (sp_mar_seen(b, v.v.p)) break;
        sp_mar_w_range(b, v);
      }
      else {
        int kind = sp_json_kind_fn ? sp_json_kind_fn(v) : 0;
        if (kind == 1) {  /* array */
          if (sp_mar_seen(b, v.v.p)) break;
          sp_mar_w_body(b, v);
        }
        else if (kind == 2) {  /* hash */
          if (sp_mar_seen(b, v.v.p)) break;
          sp_mar_w_hash(b, v);
        }
        else if (v.cls_id == SP_BUILTIN_STRBUF) {
          sp_String *h = (sp_String *)v.v.p;
          sp_mar_w_string(b, h ? h->data : "", h ? (size_t)h->len : 0, h && h->binary, h);
        }
        else if (v.cls_id == SP_BUILTIN_EXCEPTION) {
          if (sp_mar_seen(b, v.v.p)) break;
          sp_mar_w_exc_head(b, sp_exc_class_name((sp_Exception *)v.v.p), v.v.p, 0);
        }
        else if (v.cls_id >= 0) {  /* user object */
          if (sp_mar_seen(b, v.v.p)) break;
          if (!sp_marshal_v.obj_dump || !sp_marshal_v.obj_dump(b, v.cls_id, v.v.p))
            mar_raise("TypeError", "no marshal_dump is defined for this object");
        }
        else mar_raise("TypeError", "no marshal_dump is defined for this object");
      }
      break;
    case SP_TAG_BIGINT: {
      if (sp_mar_seen(b, v.v.p)) break;
      sp_Bigint *bg = (sp_Bigint *)v.v.p;
      size_t cap = sp_bigint_byte_len(bg) + 1;
      unsigned char *buf = (unsigned char *)calloc(cap + 1, 1);
      if (!buf) sp_oom_die();
      size_t nbytes = sp_bigint_to_le_bytes(bg, buf, cap);
      size_t nwords = (nbytes + 1) / 2; if (nwords == 0) nwords = 1;
      sp_mar_b(b, 'l');
      sp_mar_b(b, sp_bigint_sign(bg) < 0 ? '-' : '+');
      sp_mar_long(b, (long)nwords);
      for (size_t i = 0; i < nwords * 2; i++) sp_mar_b(b, i < nbytes ? buf[i] : 0);  /* LE, padded */
      free(buf);
      break;
    }
    default:
      mar_raise("TypeError", "unsupported value for Marshal.dump");
  }
}
/* Free a writer's heap buffers (output bytes, symbol table, object-link table)
   and the writer struct itself. */
static void sp_mar_buf_done(sp_mar_buf *b) {
  for (int i = 0; i < b->nws; i++) free(b->wsyms[i]);
  free(b->wsyms);
  free(b->p); free(b->lptr); free(b->lid);
  free(b);
}
const char *sp_marshal_dump(sp_RbVal v) {
  SP_GC_ROOT_RBVAL(v);   /* a Float component allocates via sp_float_to_s -> may GC */
  /* A raise inside the dump (unsupported object, default-proc Hash, singleton,
     anonymous class) longjmps past normal cleanup. Heap-allocate the writer
     after sp_exc_arm through a volatile pointer so its fields have allocated
     storage duration across setjmp/longjmp, -O2 cannot clobber the pointer,
     and a SystemStackError from sp_exc_arm leaks nothing. */
  sp_mar_buf *volatile b = NULL;
  int nroots = sp_gc_nroots;
  jmp_buf jb;
  if (setjmp(jb) == 0) {
    sp_exc_arm(jb);
    sp_mar_buf *w = (sp_mar_buf *)calloc(1, sizeof *w);
    if (!w) sp_oom_die();
    b = w;
    sp_mar_b(w, 4); sp_mar_b(w, 8);  /* major.minor = 4.8 */
    sp_mar_w(w, v);
    char *out = sp_str_alloc_raw(w->len + 1);
    memcpy(out, w->p, w->len); out[w->len] = 0;
    sp_str_set_len(out, w->len);
    /* a marshalled stream is bytes, and CRuby names it ASCII-8BIT: without the
       tag #length counts UTF-8 units over binary data and [] slices by them */
    sp_str_mark_binary(out);
    sp_exc_disarm();
    sp_mar_buf_done(w);
    return out;
  }
  const char *cls = sp_exc_cur_cls(), *msg = sp_exc_cur_msg();
  void *obj = sp_exc_cur_obj();
  sp_exc_disarm();
  sp_gc_nroots = nroots;   /* the dump's own roots unwound with it */
  if (b) sp_mar_buf_done(b);
  sp_fiber_reraise(cls, msg, obj);
  return "";
}

/* ---- load ---- */
typedef struct sp_mar_rd_s {
  const char *s; size_t pos, len;
  char **syms; int nsym, csym;        /* symbol-link table (`;`) */
  sp_RbVal *objs; int nobj, cobj;     /* object-link table (`@`) */
  struct sp_mar_rd_s *prev;           /* enclosing load, for a reentrant one */
} sp_mar_rd;
/* The object-link table holds every object built so far, so a `@` back-
   reference can return it -- and it is a malloc array, which the collector
   does not walk. Without this the parse is a long run of allocations with its
   own results reachable from nothing: under a minor mark they are freed and
   the next back-reference reads a corpse. Published here and marked through
   the collector's globals hook, which is the same treatment the regex globals
   get. Reentrant loads chain through `prev`, and so do the loads other
   threads run at the same time: one list for the process, which the
   collector walks whole. A reader is linked and unlinked under this lock,
   and leaves by unlinking itself, not by setting the head back to its
   prev, which dropped (or brought back, freed) another thread's reader
   (#11845). Nothing in the locked regions allocates, so no thread waits
   there on a collection. */
static sp_mar_rd *sp_mar_active = NULL;
#ifdef SP_THREADS
static pthread_mutex_t sp_mar_lock = PTHREAD_MUTEX_INITIALIZER;
#define SP_MAR_LOCK()   pthread_mutex_lock(&sp_mar_lock)
#define SP_MAR_UNLOCK() pthread_mutex_unlock(&sp_mar_lock)
#else
#define SP_MAR_LOCK()   ((void)0)
#define SP_MAR_UNLOCK() ((void)0)
#endif
void sp_marshal_mark_active(void) {
  for (sp_mar_rd *r = sp_mar_active; r; r = r->prev) {
    sp_mark_string(r->s);
    for (int i = 0; i < r->nobj; i++) sp_mark_rbval(r->objs[i]);
  }
}
static unsigned char sp_mar_rb(sp_mar_rd *r) { return r->pos < r->len ? (unsigned char)r->s[r->pos++] : 0; }
static long sp_mar_rlong(sp_mar_rd *r) {
  int c = (signed char)sp_mar_rb(r);
  if (c == 0) return 0;
  if (c > 0) {
    if (4 < c && c < 128) return (long)c - 5;
    long x = 0;
    for (int i = 0; i < c && i < (int)sizeof(long); i++) x |= (long)sp_mar_rb(r) << (8 * i);
    return x;
  }
  if (-129 < c && c < -4) return (long)c + 5;
  c = -c;
  long x = -1;
  for (int i = 0; i < c && i < (int)sizeof(long); i++) {
    x &= ~((long)0xff << (8 * i));
    x |= (long)sp_mar_rb(r) << (8 * i);
  }
  return x;
}
static char *sp_mar_rstr(sp_mar_rd *r, long n) {
  if (n < 0) n = 0;
  char *out = sp_str_alloc_raw((size_t)n + 1);
  for (long i = 0; i < n; i++) out[i] = (char)sp_mar_rb(r);
  out[n] = 0;
  sp_str_set_len(out, (size_t)n);
  return out;
}
/* Reserve the next object-link id (nil placeholder so a self-ref resolves). */
static int sp_mar_reg(sp_mar_rd *r) {
  if (r->nobj >= r->cobj) {
    r->cobj = r->cobj ? r->cobj * 2 : 16;
    r->objs = (sp_RbVal *)realloc(r->objs, sizeof(sp_RbVal) * (size_t)r->cobj);
    if (!r->objs) sp_oom_die();
  }
  int id = r->nobj; r->objs[r->nobj++] = mk_nil(); return id;
}
static sp_sym mar_intern(const char *name) { return sp_marshal_v.sym_intern ? sp_marshal_v.sym_intern(name) : 0; }
static sp_RbVal sp_mar_r(sp_mar_rd *r);
/* `C`: an instance of a user subclass of Array or Hash (#7449), its class,
   the builtin's record, and with `ivars` (an `I` around it) the ivars after
   it. The class's generated loader (sp_marshal_v.obj_load, as for an `o`
   object) makes the instance, which takes the record's link id before the
   elements are read, so one that holds itself loads holding itself; the
   elements go into its builtin through the kind's own push or store, and
   the loader then sets the ivars by name. */
static sp_RbVal sp_mar_r_bsub(sp_mar_rd *r, int ivars) {
  sp_RbVal clssym = sp_mar_r(r);
  const char *cn = (clssym.tag == SP_TAG_SYM && sp_sym_name_fn) ? sp_sym_name_fn((sp_sym)clssym.v.i) : "";
  sp_RbVal iv = sp_marshal_v.arr_new(); SP_GC_ROOT_RBVAL(iv);
  int ok = 0;
  sp_RbVal v = sp_marshal_v.obj_load ? sp_marshal_v.obj_load(cn, mk_nil(), iv, &ok) : mk_nil();
  if (!ok || !sp_bsub_cls_fn || sp_bsub_cls_fn(v) < 0) mar_raise("ArgumentError", "undefined class/module in Marshal.load");
  SP_GC_ROOT_RBVAL(v);
  int kind = sp_json_kind_fn ? sp_json_kind_fn(v) : 0;
  int str = v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF;
  unsigned char t = sp_mar_rb(r);
  if (!((t == '[' && kind == 1) || (t == '{' && kind == 2) || (t == '"' && str)))
    mar_raise("ArgumentError", "dump format error (user class)");
  int id = sp_mar_reg(r);
  r->objs[id] = v;
  long len = sp_mar_rlong(r);
  if (str) {
    /* a String subclass's bytes, binary until an `:E` ivar says text */
    if (len < 0 || (size_t)len > r->len - r->pos) mar_raise("ArgumentError", "marshal data too short");
    sp_String *h = (sp_String *)v.v.p;
    sp_String_force_encoding(h, 1);
    h->len = 0;
    sp_fd_append_len(h, r->s + r->pos, (int64_t)len);
    r->pos += (size_t)len;
  }
  else
    for (long i = 0; i < len; i++) {
      if (kind == 1) sp_marshal_v.any_push(v, sp_mar_r(r));
      else { sp_RbVal k = sp_mar_r(r); SP_GC_ROOT_RBVAL(k); sp_marshal_v.hash_set(v, k, sp_mar_r(r)); }
    }
  long n = ivars ? sp_mar_rlong(r) : 0;
  for (long i = 0; i < n; i++) {
    sp_RbVal k = sp_mar_r(r); SP_GC_ROOT_RBVAL(k);
    sp_RbVal val = sp_mar_r(r); SP_GC_ROOT_RBVAL(val);
    const char *kn = str && k.tag == SP_TAG_SYM && sp_sym_name_fn ? sp_sym_name_fn((sp_sym)k.v.i) : NULL;
    /* (a String's encoding is no ivar: :E, or :encoding with its name) */
    if (kn && (!strcmp(kn, "E") || !strcmp(kn, "encoding"))) {
      const char *enc = !strcmp(kn, "encoding") ? sp_rbval_string_bytes(val) : NULL;
      int text = enc ? !(strcasecmp(enc, "ASCII-8BIT") == 0 || strcasecmp(enc, "BINARY") == 0)
                     : (val.tag == SP_TAG_BOOL && val.v.b);
      if (text) sp_String_force_encoding((sp_String *)v.v.p, 0);
      continue;
    }
    sp_marshal_v.arr_push(iv, k);     /* ivar symbol */
    sp_marshal_v.arr_push(iv, val);   /* ivar value  */
  }
  if (n) sp_marshal_v.obj_load(cn, v, iv, &ok);
  return v;
}
/* `o:Range`: the Range's ivars (excl, begin, end) read by name, then the Range
   of the kind its ends make: a String Range for a String end, a Float Range
   for a Float end (an Integer begin with a Float end is an Integer Range that
   keeps the Float, as a literal 1..2.5 is), else an Integer Range, nil ends
   standing for an absent one. Any other end class has no Range kind here. */
static sp_RbVal sp_mar_r_range(sp_mar_rd *r) {
  sp_RbVal bg = mk_nil(), ed = mk_nil();
  int excl = 0;
  SP_GC_ROOT_RBVAL(bg); SP_GC_ROOT_RBVAL(ed);
  long n = sp_mar_rlong(r);
  for (long i = 0; i < n; i++) {
    sp_RbVal k = sp_mar_r(r);
    const char *kn = (k.tag == SP_TAG_SYM && sp_sym_name_fn) ? sp_sym_name_fn((sp_sym)k.v.i) : "";
    sp_RbVal val = sp_mar_r(r);
    if (!strcmp(kn, "excl")) excl = val.tag == SP_TAG_BOOL && val.v.i != 0;
    else if (!strcmp(kn, "begin")) bg = val;
    else if (!strcmp(kn, "end")) ed = val;
  }
  int bs = sp_rbval_is_string(bg), es = sp_rbval_is_string(ed);
  int bf = bg.tag == SP_TAG_FLT, ef = ed.tag == SP_TAG_FLT;
  int bi = bg.tag == SP_TAG_INT, ei = ed.tag == SP_TAG_INT;
  int bn = bg.tag == SP_TAG_NIL, en = ed.tag == SP_TAG_NIL;
  if ((bs || es) && (bs || bn) && (es || en))
    return sp_box_srange(sp_srange_new_box(bg, ed, excl));
  if (bi && ef) return sp_box_range(sp_range_new_fend(bg.v.i, ed.v.f, excl));
  if ((bf || ef) && (bf || bi || bn) && (ef || ei || en)) {
    int om = (bn ? 1 : bi ? SP_FRANGE_INT_BEGIN : 0) | (en ? 2 : ei ? SP_FRANGE_INT_END : 0);
    return sp_box_frange(sp_frange_new_o(bf ? bg.v.f : bi ? (sp_float)bg.v.i : -HUGE_VAL,
                                         ef ? ed.v.f : ei ? (sp_float)ed.v.i : HUGE_VAL, excl, om));
  }
  if ((bi || bn) && (ei || en))
    return sp_box_range(sp_range_new_o(bi ? sp_oint_of(bg.v.i) : sp_oint_nil(), ei ? sp_oint_of(ed.v.i) : sp_oint_nil(), excl));
  mar_raise("ArgumentError", "unsupported Range ends in Marshal.load");
  return mk_nil();
}
static sp_RbVal sp_mar_r(sp_mar_rd *r) {
  unsigned char t = sp_mar_rb(r);
  switch (t) {
    case '0': return mk_nil();
    case 'T': return mk_bool(TRUE);
    case 'F': return mk_bool(FALSE);
    case 'i': return mk_int((sp_int)sp_mar_rlong(r));
    case 'f': {
      int id = sp_mar_reg(r);
      long n = sp_mar_rlong(r);
      char *s = sp_mar_rstr(r, n); SP_GC_ROOT(s);
      sp_float f;
      if (!strcmp(s, "inf")) f = (sp_float)INFINITY;
      else if (!strcmp(s, "-inf")) f = -(sp_float)INFINITY;
      else if (!strcmp(s, "nan")) f = (sp_float)NAN;
      else f = strtod(s, NULL);
      sp_RbVal v = mk_float(f); r->objs[id] = v; return v;
    }
    case ':': {
      long n = sp_mar_rlong(r);
      char *name = sp_mar_rstr(r, n); SP_GC_ROOT(name);
      if (r->nsym >= r->csym) {
        r->csym = r->csym ? r->csym * 2 : 8;
        r->syms = (char **)realloc(r->syms, sizeof(char *) * (size_t)r->csym);
        if (!r->syms) sp_oom_die();
      }
      char *dup = strdup(name);
      if (!dup) sp_oom_die();
      r->syms[r->nsym++] = dup;
      return mk_sym(mar_intern(name));
    }
    case ';': {
      long idx = sp_mar_rlong(r);
      const char *name = (idx >= 0 && idx < r->nsym) ? r->syms[idx] : "";
      return mk_sym(mar_intern(name));
    }
    case '"': {
      /* a bare String record is ASCII-8BIT: CRuby writes every other
         encoding as an ivar of an `I` around it, which the `I` arm reads */
      long n = sp_mar_rlong(r);
      if (n < 0) {
        mar_raise("ArgumentError", "negative string size (or size too big)");
        return mk_nil();
      }
      if ((size_t)n > r->len - r->pos) {
        mar_raise("ArgumentError", "marshal data too short");
        return mk_nil();
      }
      int id = sp_mar_reg(r);
      sp_String *s = sp_String_new_inline_len(r->s + r->pos, n);
      sp_String_force_encoding(s, 1);
      r->pos += (size_t)n;
      return r->objs[id] = mk_strbuf(s);
    }
    case 'I': {
      if (r->pos < r->len && r->s[r->pos] == 'C') { r->pos++; return sp_mar_r_bsub(r, 1); }
      sp_RbVal inner = sp_mar_r(r); SP_GC_ROOT_RBVAL(inner);
      long nivar = sp_mar_rlong(r);
      for (long i = 0; i < nivar; i++) {
        sp_RbVal k = sp_mar_r(r);
        sp_RbVal val = sp_mar_r(r);
        if (!sp_rbval_is_string(inner) || k.tag != SP_TAG_SYM || !sp_sym_name_fn) continue;
        /* The encoding ivar is :E for text or :encoding with a name. */
        const char *kn = sp_sym_name_fn((sp_sym)k.v.i);
        if (strcmp(kn, "E")) {
          if (strcmp(kn, "encoding")) continue;
          const char *enc = sp_rbval_string_bytes(val);
          if (enc && (!strcasecmp(enc, "ASCII-8BIT") || !strcasecmp(enc, "BINARY"))) continue;
        }
        if (inner.tag == SP_TAG_OBJ) sp_String_force_encoding((sp_String *)inner.v.p, 0);
        else sp_str_as_text(sp_rbval_string_bytes(inner));
      }
      return inner;
    }
    case 'C': return sp_mar_r_bsub(r, 0);
    case '@': {
      long id = sp_mar_rlong(r);
      return (id >= 0 && id < r->nobj) ? r->objs[id] : mk_nil();
    }
    case 'U': {
      int id = sp_mar_reg(r);
      sp_RbVal sym = sp_mar_r(r);
      sp_RbVal data = sp_mar_r(r); SP_GC_ROOT_RBVAL(data);
      const char *cn = (sym.tag == SP_TAG_SYM && sp_sym_name_fn) ? sp_sym_name_fn((sp_sym)sym.v.i) : "";
      sp_RbVal a0 = mk_nil(), a1 = mk_nil();
      if (data.tag == SP_TAG_OBJ) { a0 = sp_json_aref_fn(data, 0); a1 = sp_json_aref_fn(data, 1); }
      sp_RbVal v;
      if (!strcmp(cn, "Complex")) v = sp_marshal_v.box_complex(poly_f(a0), poly_f(a1), (a0.tag == SP_TAG_FLT ? SP_CPLX_RE_F : 0) | (a1.tag == SP_TAG_FLT ? SP_CPLX_IM_F : 0));
      else if (!strcmp(cn, "Rational")) v = sp_marshal_v.box_rational(poly_i(a0), poly_i(a1));
      else { mar_raise("ArgumentError", "unsupported user class in Marshal.load"); v = mk_nil(); }
      r->objs[id] = v; return v;
    }
    case 'o': {
      /* the object is made and registered first, so an ivar that links back
         to it loads as the object itself; then its ivars are set */
      int id = sp_mar_reg(r);
      sp_RbVal clssym = sp_mar_r(r);
      const char *cn = (clssym.tag == SP_TAG_SYM && sp_sym_name_fn) ? sp_sym_name_fn((sp_sym)clssym.v.i) : "";
      if (!strcmp(cn, "Range")) { sp_RbVal rv = sp_mar_r_range(r); r->objs[id] = rv; return rv; }
      sp_RbVal iv = sp_marshal_v.arr_new(); SP_GC_ROOT_RBVAL(iv);
      int ok = 0;
      sp_RbVal v = sp_marshal_v.obj_load(cn, mk_nil(), iv, &ok);
      if (!ok) mar_raise("ArgumentError", "undefined class/module in Marshal.load");
      SP_GC_ROOT_RBVAL(v);
      r->objs[id] = v;
      long n = sp_mar_rlong(r);
      for (long i = 0; i < n; i++) {
        sp_marshal_v.arr_push(iv, sp_mar_r(r));   /* ivar symbol */
        sp_marshal_v.arr_push(iv, sp_mar_r(r));   /* ivar value  */
      }
      if (n) sp_marshal_v.obj_load(cn, v, iv, &ok);
      return v;
    }
    case 'l': {
      int id = sp_mar_reg(r);
      char sign = (char)sp_mar_rb(r);
      long nwords = sp_mar_rlong(r);
      size_t nbytes = (size_t)(nwords > 0 ? nwords : 0) * 2;
      unsigned char *buf = (unsigned char *)malloc(nbytes ? nbytes : 1);
      if (!buf) sp_oom_die();
      for (size_t i = 0; i < nbytes; i++) buf[i] = sp_mar_rb(r);
      sp_Bigint *bn = sp_bigint_from_le_bytes(sign == '-', buf, nbytes);
      free(buf);
      /* CRuby loads a Bignum record whose value fits a Fixnum as that Fixnum
         (it also writes one past its 31-bit marshal Fixnum), so a value that
         fits an Integer here is an Integer, not a Bignum box */
      uint64_t m;
      if (sp_bigint_mag_u64(bn, &m)) {
        int neg = sign == '-';
        if (neg ? m <= (uint64_t)INT64_MAX + 1 : m <= (uint64_t)INT64_MAX) {
          int64_t w = neg ? (int64_t)(0 - m) : (int64_t)m;
          if ((int64_t)(sp_int)w == w) { sp_RbVal iv = mk_int((sp_int)w); r->objs[id] = iv; return iv; }
        }
      }
      sp_RbVal v = mk_bigint(bn); r->objs[id] = v; return v;
    }
    case '[': {
      int id = sp_mar_reg(r);
      long n = sp_mar_rlong(r);
      sp_RbVal box = sp_marshal_v.arr_new(); SP_GC_ROOT_RBVAL(box);
      r->objs[id] = box;
      for (long i = 0; i < n; i++) sp_marshal_v.arr_push(box, sp_mar_r(r));
      return box;
    }
    case '{': case '}': {   /* `}`: the pairs, then the default value */
      int id = sp_mar_reg(r);
      long n = sp_mar_rlong(r);
      sp_RbVal box = sp_marshal_v.hash_new(); SP_GC_ROOT_RBVAL(box);
      r->objs[id] = box;
      for (long i = 0; i < n; i++) { sp_RbVal k = sp_mar_r(r); sp_RbVal val = sp_mar_r(r); sp_marshal_v.hash_set(box, k, val); }
      if (t == '}') {
        sp_RbVal d = sp_mar_r(r);
        if (sp_marshal_v.hash_set_default) sp_marshal_v.hash_set_default(box, d);
      }
      return box;
    }
    default: mar_raise("ArgumentError", "unsupported type in Marshal.load"); return mk_nil();
  }
}
/* A reader leaves the active chain and frees its tables. */
static void sp_mar_rd_done(sp_mar_rd *r) {
  SP_MAR_LOCK();
  for (sp_mar_rd **pp = &sp_mar_active; *pp; pp = &(*pp)->prev)
    if (*pp == r) { *pp = r->prev; break; }
  SP_MAR_UNLOCK();
  for (int i = 0; i < r->nsym; i++) free(r->syms[i]);
  free(r->syms); free(r->objs); free(r);
}
sp_RbVal sp_marshal_load(const char *s, sp_int len) {
  /* The whole parse reads out of this buffer, and every object it builds
     allocates -- so the source string has to stay rooted for the duration.
     r->s is a plain field, not a root slot; the collector walks registered
     slots, so the parameter is what has to be registered. */
  SP_GC_ROOT_STR(s);
  /* A raise inside the parse (an unknown class, a format error, a value a
     loader cannot convert) longjmps past the return below, and the reader
     stayed on the active chain: the next collection walked its freed
     frame. It is caught here, the reader taken off the chain, and raised
     on to the program's own handler. Allocate and link the reader after
     sp_exc_arm so a SystemStackError from sp_exc_arm leaks nothing and
     leaves no dangling entry on sp_mar_active. */
  sp_mar_rd *volatile rd = NULL;
  int nroots = sp_gc_nroots;
  jmp_buf jb;
  if (setjmp(jb) == 0) {
    sp_exc_arm(jb);
    sp_mar_rd *r = (sp_mar_rd *)calloc(1, sizeof *r);
    if (!r) sp_oom_die();
    r->s = s ? s : ""; r->len = s ? (size_t)len : 0;
    SP_MAR_LOCK();
    r->prev = sp_mar_active; sp_mar_active = r;
    SP_MAR_UNLOCK();
    rd = r;
    if (r->len >= 2) r->pos = 2;  /* skip the 4.8 version header */
    sp_RbVal v = sp_mar_r(r);
    sp_exc_disarm();
    sp_mar_rd_done(r);
    return v;
  }
  const char *cls = sp_exc_cur_cls(), *msg = sp_exc_cur_msg();
  void *obj = sp_exc_cur_obj();
  sp_exc_disarm();
  sp_gc_nroots = nroots;   /* the parse's own roots unwound with it */
  if (rd) sp_mar_rd_done(rd);
  sp_fiber_reraise(cls, msg, obj);
  return mk_nil();
}
