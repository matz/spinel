/* sp_json.c -- JSON.generate serialization, split out of spinel_rt.h.

   This is a standalone translation unit: it owns no typed-array/hash structs.
   It reaches the generated program's containers only through the generic hooks
   in sp_gc.h (sp_json_kind/len/aref/hpair + sp_sym_name_fn), which the generated
   TU installs at startup. Result strings are built in an off-heap scratch buffer
   and finalized onto the shared GC string heap (sp_alloc.h), so a nested
   allocation can't free a piece already copied in. */
#include "spinel/runtime.h"  /* sp_RbVal, SP_TAG_*, hooks, sp_str_alloc, sp_int_to_s, sp_float_to_s */
#include "sp_json.h"          /* this package's sp_json_str / sp_json_val API */
#include <string.h>
#include <stdlib.h>           /* strtoll, strtod */
#include <errno.h>            /* ERANGE past int64 */
#include <math.h>             /* isinf, isnan */
#include <float.h>            /* FLT_EVAL_METHOD (jp_float_exact) */
#include <stdio.h>            /* snprintf (sp_json_float) */
sp_Bigint *sp_bigint_new_str(const char *s, int base);   /* the runtime archive (sp_bigint.c) */
const char *sp_poly_to_s(sp_RbVal v);                   /* the runtime archive (sp_poly_cold.c) */

/* A 0xff-marked rodata literal, so sp_str_byte_len reads its length correctly
   (matches spinel_rt.h's SPL). Used for the fixed tokens true/false/null. */
#define JSPL(s) (&("\xff" s)[1])

/* Growable buffer, finalized into a right-sized GC string at the end.

   It starts off the GC heap -- in the caller's stack array (JB_ON_STACK),
   which holds a short document such as an object's own #to_json with no
   allocation at all, and in a malloc'd block once it outgrows that -- and it
   has to: the escaper below reads an unrooted source through it -- one that
   may be a rodata literal, whose [-1] is not a marker byte for the collector
   to read -- and that is only safe while nothing in here can collect.

   A document walk, though, does not always return through its frame. A user
   #to_json can raise, throw, or return from a proc, and every one of those
   longjmps over the frame, leaving a malloc'd block owned by nothing at all.
   So a walk MOVES its buffer onto the GC string heap before it calls user code
   (jb_to_heap), into a slot it has rooted, and from then on the collector owns
   it: the landing frame drops that root along with every other root the walk
   was holding, whichever way the walk was left. Once moved, growing the buffer
   allocates -- so an append whose source is a GC string roots that source too
   (jb_gcs). The move happens at most once per walk, and not at all for a
   document with no user objects in it, which is why it costs nothing to
   measure. */
typedef struct {
  char *p; size_t len, cap;
  char *heap;   /* NULL while p is malloc'd; == p once moved, and the rooted slot */
  /* JSON.dump's options, zero for generate: a nesting limit of the
     caller's (0: none) in place of generate's 100, and NaN / Infinity
     written out rather than refused */
  int dump, dump_limit;
  int stk;      /* p is the caller's stack array: grown by a copy, never freed */
} jbuf;
#define JB_STACK_N 512
#define JB_ON_STACK(b, arr) do { memset(&(b), 0, sizeof (b)); (b).p = (arr); (b).cap = sizeof (arr); (b).stk = 1; } while (0)
/* what a walk that ends early, or the end of one, gives back: a malloc'd
   block; the stack array is the caller's, and a moved one the collector's */
static inline void jb_release(jbuf *b) { if (!b->heap && !b->stk) free(b->p); }
/* A moved buffer's grow, out of line: jb_add is inlined into loops that append
   a byte at a time, and a second allocator inside its body stops that. With
   the grow written inline, nm gives jb_add a symbol of its own and the
   generate benchmark goes from 1.98 s to 2.58 s. */
SP_COLD static SP_NOINLINE void jb_grow_heap(jbuf *b, size_t cap) {
  char *q = sp_str_alloc(cap);   /* collects; b->heap is the walk's root */
  if (b->len) memcpy(q, b->p, b->len);
  b->p = b->heap = q;
  b->cap = cap;
}
static void jb_add(jbuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = (b->len + n + 1) * 2;
    if (b->heap) jb_grow_heap(b, cap);
    else if (b->stk) {
      char *q = (char *)malloc(cap);
      if (!q) sp_oom_die();
      if (b->len) memcpy(q, b->p, b->len);
      b->p = q; b->stk = 0;
      b->cap = cap;
    } else {
      b->p = (char *)realloc(b->p, cap);
      if (!b->p) sp_oom_die();
      b->cap = cap;
    }
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
}
/* one byte, the walk's punctuation: stored directly while it fits (with
   jb_add's room for a terminator), where a one-byte copy through jb_add was a
   call and a memcpy per comma and colon */
static inline void jb_c(jbuf *b, char c) {
  if (b->len + 2 <= b->cap) { b->p[b->len++] = c; return; }
  jb_add(b, &c, 1);
}
/* Hand the buffer to the collector before running code that may not come back.
   The allocation here can collect, and that is safe at exactly this moment:
   b->heap is still NULL, so the walk's root marks nothing, and what it is
   copying out of is a malloc'd block the collector cannot touch. */
static void jb_to_heap(jbuf *b) {
  if (b->heap) return;
  size_t cap = b->cap ? b->cap : 1;
  char *q = sp_str_alloc(cap);
  if (b->len) memcpy(q, b->p, b->len);
  if (!b->stk) free(b->p);
  b->stk = 0;
  b->p = b->heap = q;
  b->cap = cap;
}
/* Append a GC string to a buffer that has moved: growing it allocates now, and
   the source is held in nothing but this argument slot -- the same reason
   sp_json_parse roots its input. A source with no marker byte in front of it must not come through
   here: the collector reads that byte. Every caller's does -- a fresh heap
   string, sp_str_empty, or a JSPL literal. */
static void jb_gcs_rooted(jbuf *b, const char *s, size_t n) {
  SP_GC_ROOT_STR(s);
  jb_add(b, s, n);
}
/* Every GC string a walk appends comes through here, once per scalar and once
   per key. Before the buffer moves nothing in jb_add can collect, so no root
   is pushed; the push is in its own function above to keep the cleanup it
   declares out of the path that runs when there is nothing to root. */
static inline void jb_gcs(jbuf *b, const char *s, size_t n) {
  if (b->heap) { jb_gcs_rooted(b, s, n); return; }
  jb_add(b, s, n);
}
static const char *jb_finish(jbuf *b) {
  char *r = sp_str_alloc(b->len);
  if (b->len) memcpy(r, b->p, b->len);
  sp_str_set_len(r, b->len);
  jb_release(b);
  return r;
}

/* An object's #to_s, the way sp_poly_to_s renders it: the class's own method
   through the generated dispatcher, a built-in object's (a Time) through
   sp_poly_to_s, and otherwise CRuby's default form. The generated TU's
   renderer is a static inline in spinel_rt.h, which this TU deliberately does
   not include (see the note at the top), so these arms are reached through
   the hooks the generated program installs. */
static const char *sp_json_obj_to_s(sp_RbVal v) {
  if (v.cls_id >= 0 && sp_obj_to_s_fn) {
    const char *us = sp_obj_to_s_fn(v.cls_id, v.v.p);
    if (us) return us;
  }
  if (v.cls_id < 0 && sp_poly_to_s_fn) return sp_poly_to_s_fn(v);
  return sp_sprintf("#<%s:0x%016llx>",
                    sp_obj_cls_name_fn ? sp_obj_cls_name_fn(v.cls_id) : "Object",
                    (unsigned long long)(uintptr_t)v.v.p);
}

/* The escape a byte takes in a JSON string: 0 for none, 'u' for \u00XX, and
   otherwise the letter after the backslash. */
static const char jb_esc[256] = {
  'u','u','u','u','u','u','u','u','b','t','n','u','f','r','u','u',
  'u','u','u','u','u','u','u','u','u','u','u','u','u','u','u','u',
  0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  ['\\'] = '\\',
};
/* A String quoted and escaped straight into the buffer, the bytes between two
   escapes copied as one run. S is read while the buffer grows, and a moved
   buffer's growth can collect: every caller's S is held by the document the
   walk has rooted (a value, a key, a Symbol's name) or is not on the GC heap,
   and the collector moves nothing, so it is still there to read. */
static void jb_strn(jbuf *b, const char *s, size_t n) {
  jb_c(b, '"');
  if (s) {
    const char *run = s, *p = s, *end = s + n;
    unsigned char c;
    while (p < end) {
      c = (unsigned char)*p;
      char e = jb_esc[c];
      if (!e) { p++; continue; }
      if (p > run) jb_add(b, run, (size_t)(p - run));
      if (e == 'u') {
        char u[6] = { '\\', 'u', '0', '0', "0123456789abcdef"[c >> 4], "0123456789abcdef"[c & 15] };
        jb_add(b, u, 6);
      }
      else { char t[2] = { '\\', e }; jb_add(b, t, 2); }
      run = ++p;
    }
    if (p > run) jb_add(b, run, (size_t)(p - run));
  }
  jb_c(b, '"');
}
/* A C string with no header to read a length from: a Symbol's name, a bare
   literal. It ends at its NUL. */
static void jb_str(jbuf *b, const char *s) { jb_strn(b, s, s ? strlen(s) : 0); }
/* A Spinel String, which may hold a NUL: its bytes run to its own length, and
   a NUL among them is written as \u0000, as CRuby's json writes it. */
static void jb_rstr(jbuf *b, const char *s) { jb_strn(b, s, sp_str_byte_len(s)); }
const char *sp_json_str(const char *s) {
  char stk[JB_STACK_N]; jbuf b; JB_ON_STACK(b, stk);
  jb_str(&b, s);
  return jb_finish(&b);
}
static const char *sp_json_rstr(const char *s) {
  char stk[JB_STACK_N]; jbuf b; JB_ON_STACK(b, stk);
  jb_rstr(&b, s);
  return jb_finish(&b);
}
/* An Integer's digits into the buffer, as sp_int_to_s writes them */
static void jb_int(jbuf *b, sp_int n) {
  uint64_t u = n < 0 ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
  char t[24]; size_t len = (size_t)sp_u64_ndigits(u) + (n < 0);
  sp_u64_write_back(t + len, u);
  if (n < 0) t[0] = '-';
  jb_add(b, t, len);
}

static const char *sp_json_key(sp_RbVal k) {
  k = sp_rbval_str(k);
  if (k.tag == SP_TAG_STR)  return sp_json_rstr(k.v.s);
  if (k.tag == SP_TAG_SYM)  return sp_json_str(sp_sym_name_fn ? sp_sym_name_fn((sp_sym)k.v.i) : "");
  if (k.tag == SP_TAG_INT)  return sp_json_str(sp_int_to_s(k.v.i));
  if (k.tag == SP_TAG_BOOL) return sp_json_str(k.v.b ? "true" : "false");
  /* any other key is its #to_s, as CRuby's json generates it: a Float, nil,
     an Array, an object of the program's own */
  return sp_poly_to_s_fn ? sp_json_rstr(sp_poly_to_s_fn(k)) : sp_json_str("");
}
/* A key that is an object may run a #to_s of the program's own, which may
   raise, throw, or return past the walk: the buffer moves to the heap first,
   as before any user code. */
static void sp_json_key_b(jbuf *b, sp_RbVal k) {
  /* the usual keys straight into the buffer, as sp_json_key would write them */
  sp_RbVal ks = sp_rbval_str(k);
  switch (ks.tag) {
    case SP_TAG_STR: jb_rstr(b, ks.v.s); return;
    case SP_TAG_SYM: jb_str(b, sp_sym_name_fn ? sp_sym_name_fn((sp_sym)ks.v.i) : ""); return;
    case SP_TAG_INT: jb_c(b, '"'); jb_int(b, ks.v.i); jb_c(b, '"'); return;
    default: break;
  }
  if (k.tag == SP_TAG_OBJ) jb_to_heap(b);
  const char *jk = sp_json_key(k);
  jb_gcs(b, jk, (size_t)sp_str_byte_len(jk));
}

/* A String held as its handle (SP_BUILTIN_STRBUF): a String the program
   changes in place, or an instance of a String subclass, which is boxed as
   its String (#7449). */
static int sp_json_is_strbuf(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p;
}
/* The value the user #to_json arm dispatches on: a String subclass instance
   under its own class, read back through the generated program's hook, so
   the subclass's #to_json is called as CRuby's json calls it. */
static sp_RbVal sp_json_user_view(sp_RbVal v) {
  if (sp_json_is_strbuf(v) && sp_bsub_cls_fn) {
    int k = sp_bsub_cls_fn(v);
    if (k >= 0) v.cls_id = k;
  }
  return v;
}

/* What an object with no #to_json of its own generates: a String held as
   its handle is a JSON string of its bytes, as a String literal is (CRuby's
   json reads a String subclass instance's bytes, not its #to_s); any other
   object is its #to_s, as Object#to_json is. */
static const char *sp_json_obj_text(sp_RbVal v) {
  if (sp_json_is_strbuf(v)) return sp_json_rstr(sp_poly_to_s(v));
  return sp_json_rstr(sp_json_obj_to_s(v));
}

/* CRuby's json refuses to serialize past 100 levels of nesting, which is how
   it answers a structure that contains itself: `a << a; JSON.generate(a)` is a
   JSON::NestingError, not an endless document. The limit is on DEPTH, not on
   identity -- 100 levels of distinct arrays serialize and the 101st raises,
   whether or not anything repeats. */
#define SP_JSON_MAX_NESTING 100
SP_COLD static SP_NOINLINE SP_NORETURN void sp_json_too_deep(jbuf *b) {
  /* The walk's own raise, so the walk can hand it the buffer to release (#4355).
     One it has already moved onto the GC heap needs nothing: the collector
     takes that with the root the landing frame drops. */
  jb_release(b);
  if (b->dump && b->dump_limit > 0) sp_raise_cls("ArgumentError", "exceed depth limit");
  if (b->dump) sp_raise_cls("SystemStackError", "stack level too deep");
  sp_raise_cls("JSON::NestingError",
               "nesting of 100 is too deep. Did you try to serialize objects with circular references?");
}
/* The deepest container a walk enters: generate's 100, a JSON.dump limit,
   or for a dump with none a guard on the C stack, past which CRuby's own
   recursion overflows too */
#define SP_JSON_DUMP_GUARD 10000
static int sp_json_max_depth(const jbuf *b) {
  if (!b->dump) return SP_JSON_MAX_NESTING;
  return b->dump_limit > 0 ? b->dump_limit : SP_JSON_DUMP_GUARD;
}
/* A Float as the json generator writes it (its vendored fpconv): the
   shortest digits that read back as the value, then a whole number with a
   ".0" while the exponent is under 15, a plain decimal for a fraction down
   to 1e-7 (or one whose exponent is under 10), and scientific notation
   otherwise, the exponent unpadded -- "1e+20", "0.00000015", "1e-10".
   Float#to_s writes some of these differently ("1.0e+20", "1.5e-07"). */
static int sp_json_float_fmt(double d, char out[64]) {
  /* the shortest digits that read back as d (Float#to_s's), and the decimal
     exponent of the first: one integer computation where probing
     snprintf("%.*e") with strtod at each precision took up to 17 of each,
     which was most of a float-heavy JSON.generate */
  int neg = signbit(d) != 0;
  char digits[32]; int nd = 1, e10 = 0;
  digits[0] = '0';
  if (d != 0.0) e10 = sp_float_shortest(neg ? -d : d, digits, &nd);
  while (nd > 1 && digits[nd - 1] == '0') nd--;   /* "1000" at 1e20 -> "1" */
  int K = e10 - (nd - 1);              /* value = digits * 10^K */
  int ex = K + nd - 1; if (ex < 0) ex = -ex;
  int o = 0;
  if (neg) out[o++] = '-';
  if (K >= 0 && ex < 15) {
    memcpy(out + o, digits, nd); o += nd;
    for (int i = 0; i < K; i++) out[o++] = '0';
    out[o++] = '.'; out[o++] = '0';
  }
  else if (K < 0 && (K > -7 || ex < 10)) {
    int off = nd + K;
    if (off <= 0) {
      out[o++] = '0'; out[o++] = '.';
      for (int i = 0; i < -off; i++) out[o++] = '0';
      memcpy(out + o, digits, nd); o += nd;
    }
    else {
      memcpy(out + o, digits, off); o += off;
      out[o++] = '.';
      memcpy(out + o, digits + off, nd - off); o += nd - off;
    }
  }
  else {
    out[o++] = digits[0];
    if (nd > 1) { out[o++] = '.'; memcpy(out + o, digits + 1, nd - 1); o += nd - 1; }
    o += snprintf(out + o, (size_t)(64 - o), "e%c%d", K + nd - 1 < 0 ? '-' : '+', ex);
  }
  return o;
}
static const char *sp_json_float(double d) {
  char out[64]; int o = sp_json_float_fmt(d, out);
  char *r = sp_str_alloc((size_t)o);
  memcpy(r, out, (size_t)o);
  sp_str_set_len(r, (size_t)o);
  return r;
}

/* Everything but a container: the leaf arms, answering one GC string. */
static const char *sp_json_scalar_o(sp_RbVal v, int allow_nan) {
  v = sp_rbval_str(v);
  switch (v.tag) {
    case SP_TAG_INT:  return sp_int_to_s(v.v.i);
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_FLT:
      /* Infinity and NaN are not JSON: CRuby refuses them rather than write
         a document no parser reads back -- except JSON.dump, which writes
         them (its allow_nan) */
      if (allow_nan && isnan(v.v.f)) return JSPL("NaN");
      if (allow_nan && isinf(v.v.f)) return v.v.f > 0 ? JSPL("Infinity") : JSPL("-Infinity");
      if (isinf(v.v.f))
        sp_raise_cls("JSON::GeneratorError", v.v.f > 0 ? "Infinity not allowed in JSON" : "-Infinity not allowed in JSON");
      if (isnan(v.v.f)) sp_raise_cls("JSON::GeneratorError", "NaN not allowed in JSON");
      return sp_json_float(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? JSPL("true") : JSPL("false");
    case SP_TAG_NIL:  return JSPL("null");
    case SP_TAG_STR:  return sp_json_rstr(v.v.s);
    case SP_TAG_SYM:  return sp_json_str(sp_sym_name_fn ? sp_sym_name_fn((sp_sym)v.v.i) : "");
    default: return JSPL("null");
  }
}
static const char *sp_json_scalar(sp_RbVal v) { return sp_json_scalar_o(v, 0); }
static void sp_json_val_b(jbuf *b, sp_RbVal v, int depth);
/* A compact document is appended into ONE buffer for the whole walk, as the
   pretty form below already does, so the walk has exactly one allocation to
   account for.

   That buffer is released whichever way the walk is left. Its own nesting
   error is handed it to free. The raises it cannot be handed -- a user
   #to_json that raises, throws or returns from a proc, and a #to_json answer
   that does not parse -- leave this frame without coming back to it, so the
   walk moves the buffer to the collector before it calls any of that code. */
const char *sp_json_val(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_json_scalar(v);
  SP_GC_ROOT_RBVAL(v);   /* see sp_json_pretty below */
  char stk[JB_STACK_N]; jbuf b; JB_ON_STACK(b, stk);
  SP_GC_ROOT_STR(b.heap);
  sp_json_val_b(&b, v, 0);
  return jb_finish(&b);
}
/* JSON.dump(obj[, limit]): generate with dump's options -- no nesting cap
   but the caller's LIMIT (0 for none; past it, ArgumentError "exceed depth
   limit"), and NaN / Infinity written out */
const char *sp_json_dump(sp_RbVal v, sp_int limit) {
  if (v.tag != SP_TAG_OBJ) return sp_json_scalar_o(v, 1);
  SP_GC_ROOT_RBVAL(v);
  char stk[JB_STACK_N]; jbuf b; JB_ON_STACK(b, stk);
  b.dump = 1;
  b.dump_limit = limit > 0 ? (int)(limit > SP_JSON_DUMP_GUARD ? SP_JSON_DUMP_GUARD : limit) : 0;
  SP_GC_ROOT_STR(b.heap);
  sp_json_val_b(&b, v, 0);
  return jb_finish(&b);
}
/* A leaf in the walk: the common ones written into the buffer, where each
   used to be a GC string of its own (sp_json_scalar_o's) copied in and left
   for the collector -- a dozen per small object; the rest (a Bigint,
   NaN and the Infinities, which raise or are dump's) as before. */
static void sp_json_leaf_b(jbuf *b, sp_RbVal v) {
  v = sp_rbval_str(v);
  switch (v.tag) {
    case SP_TAG_INT: jb_int(b, v.v.i); return;
    case SP_TAG_FLT:
      if (isfinite(v.v.f)) { char out[64]; jb_add(b, out, (size_t)sp_json_float_fmt(v.v.f, out)); return; }
      break;
    case SP_TAG_STR: jb_rstr(b, v.v.s); return;
    case SP_TAG_SYM: jb_str(b, sp_sym_name_fn ? sp_sym_name_fn((sp_sym)v.v.i) : ""); return;
    case SP_TAG_BOOL: if (v.v.b) jb_add(b, "true", 4); else jb_add(b, "false", 5); return;
    case SP_TAG_NIL: jb_add(b, "null", 4); return;
    default: break;
  }
  const char *sc = sp_json_scalar_o(v, b->dump);
  jb_gcs(b, sc, strlen(sc));
}
/* `depth` counts the containers already entered, so a container reached at
   depth 100 would be the 101st level. */
static void sp_json_val_b(jbuf *b, sp_RbVal v, int depth) {
  if (v.tag != SP_TAG_OBJ) { sp_json_leaf_b(b, v); return; }
  int kind = sp_json_kind_fn ? sp_json_kind_fn(v) : 0;
  if (kind == 1) {  /* array */
    if (depth >= sp_json_max_depth(b)) sp_json_too_deep(b);
    sp_int n = sp_json_len_fn(v);
    jb_c(b, '[');
    for (sp_int i = 0; i < n; i++) {
      if (i) jb_c(b, ',');
      sp_json_val_b(b, sp_json_aref_fn(v, i), depth + 1);
    }
    jb_c(b, ']');
    return;
  }
  if (kind == 2) {  /* hash */
    if (depth >= sp_json_max_depth(b)) sp_json_too_deep(b);
    sp_int n = sp_json_len_fn(v);
    jb_c(b, '{');
    for (sp_int i = 0; i < n; i++) {
      /* a key's own #to_s may have emptied the Hash */
      if (i >= sp_json_len_fn(v)) break;
      if (i) jb_c(b, ',');
      sp_RbVal k, val;
      sp_json_hpair_fn(v, i, &k, &val);
      /* the pair outlives the Hash slot it came from while the key's #to_s
         runs, should that #to_s take it out of the Hash */
      SP_GC_ROOT_RBVAL(k);
      SP_GC_ROOT_RBVAL(val);
      sp_json_key_b(b, k);
      jb_c(b, ':');
      sp_json_val_b(b, val, depth + 1);
    }
    jb_c(b, '}');
    return;
  }
  /* An object serializes the way CRuby's json does, and CRuby's json is method
     dispatch: `require "json"` mixes #to_json into Object and into each core
     class, and JSON.generate calls it. So a class with its own #to_json wins,
     and everything else lands on Object#to_json -- which is `to_s.to_json`, a
     JSON *string*. Struct has no override of its own there (the method's owner
     for a Struct is the Object module), which is why a Struct reads as
     "#<struct S x=1>" rather than as its members.

     Spinel reaches the same two answers by a different road, since it has no
     runtime method table to mix a module into: the user arm is the
     sp_obj_to_json_fn hook the generated program installs, and the fallback is
     sp_poly_to_s, which dispatches the class's own #to_s and otherwise renders
     the default #<Name:0x..>. The observable result is CRuby's for both.

     It used to reflect an object into a hash of its members here instead, so
     JSON.generate(Struct.new(:x).new(1)) answered {"x":1} where CRuby answers
     "#<struct S x=1>" -- and a plain object with no reflection answered null,
     which is not any Ruby's answer (#4387). */
  /* the answer is a Ruby String, appended by its own length: a NUL inside it
     is the user's to keep */
  if (sp_obj_to_json_fn) {
    jb_to_heap(b);   /* the user's method may raise, throw, or return past us */
    const char *uj = sp_obj_to_json_fn(sp_json_user_view(v));
    if (uj) { jb_gcs(b, uj, (size_t)sp_str_byte_len(uj)); return; }
  }
  { jb_to_heap(b);   /* the class's own #to_s can raise or allocate */
    const char *js = sp_json_obj_text(v);
    jb_gcs(b, js, (size_t)sp_str_byte_len(js)); }
}

/* ---------- JSON.pretty_generate ----------
   CRuby's pretty format: two-space indent per level, ": " key separator,
   containers multiline with the closer at the parent's indent; empty
   containers stay "{}" / "[]". Scalars delegate to the flat serializer. */

static void sp_json_indent(jbuf *b, int depth) {
  for (int i = 0; i < depth; i++) jb_add(b, "  ", 2);
}

static void sp_json_pretty_val(jbuf *b, sp_RbVal v, int depth) {
  if (v.tag == SP_TAG_OBJ) {
    int kind = sp_json_kind_fn ? sp_json_kind_fn(v) : 0;
    if (kind == 1) {  /* array */
      if (depth >= SP_JSON_MAX_NESTING) sp_json_too_deep(b);
      sp_int n = sp_json_len_fn(v);
      if (n == 0) { jb_add(b, "[]", 2); return; }
      jb_c(b, '[');
      for (sp_int i = 0; i < n; i++) {
        if (i) jb_c(b, ',');
        jb_c(b, '\n');
        sp_json_indent(b, depth + 1);
        sp_json_pretty_val(b, sp_json_aref_fn(v, i), depth + 1);
      }
      jb_c(b, '\n');
      sp_json_indent(b, depth);
      jb_c(b, ']');
      return;
    }
    if (kind == 2) {  /* hash */
      if (depth >= SP_JSON_MAX_NESTING) sp_json_too_deep(b);
      sp_int n = sp_json_len_fn(v);
      if (n == 0) { jb_add(b, "{}", 2); return; }
      jb_c(b, '{');
      for (sp_int i = 0; i < n; i++) {
        if (i >= sp_json_len_fn(v)) break;   /* as in the compact walk */
        if (i) jb_c(b, ',');
        jb_c(b, '\n');
        sp_json_indent(b, depth + 1);
        sp_RbVal k, val;
        sp_json_hpair_fn(v, i, &k, &val);
        SP_GC_ROOT_RBVAL(k);
        SP_GC_ROOT_RBVAL(val);
        sp_json_key_b(b, k);
        jb_add(b, ": ", 2);
        sp_json_pretty_val(b, val, depth + 1);
      }
      jb_c(b, '\n');
      sp_json_indent(b, depth);
      jb_c(b, '}');
      return;
    }
    /* the user's #to_json answers a compact document; re-read it so it lays
       out with the surrounding indentation instead of on one line */
    if (sp_obj_to_json_fn) {
      jb_to_heap(b);   /* the user's method may raise, throw, or return past us */
      const char *uj = sp_obj_to_json_fn(sp_json_user_view(v));
      if (uj) {
        /* the re-parsed document is this walk's only reference to it */
        sp_RbVal d = sp_json_parse(uj);
        SP_GC_ROOT_RBVAL(d);
        sp_json_pretty_val(b, d, depth);
        return;
      }
    }
    /* Same fallback as the flat walk: Object#to_json is to_s.to_json, and a
       JSON string has no interior structure for the pretty printer to lay out,
       so it renders identically either way. */
    { jb_to_heap(b);
      const char *js = sp_json_obj_text(v);
      jb_gcs(b, js, (size_t)sp_str_byte_len(js)); }
    return;
  }
  { const char *sc = sp_json_val(v); jb_gcs(b, sc, strlen(sc)); }
}

/* The document is rooted for the walk, for the reason sp_json_parse roots its
   input and the reflection arms root theirs: a caller that hands over an
   expression -- JSON.generate(Foo.new) -- holds it in nothing but the C
   argument slot, and everything the walk reaches is reached through it. The
   walk allocates from its first escaped string onward, and moving the buffer
   to the heap allocates too, so an unrooted document is read after it is
   freed. */
const char *sp_json_pretty(sp_RbVal v) {
  SP_GC_ROOT_RBVAL(v);
  char stk[JB_STACK_N]; jbuf b; JB_ON_STACK(b, stk);
  SP_GC_ROOT_STR(b.heap);
  sp_json_pretty_val(&b, v, 0);
  return jb_finish(&b);
}

/* ---------- JSON.parse ----------
   A recursive-descent parser producing boxed poly values: scalars and arrays
   are built directly from the package ABI (sp_box_*, sp_PolyArray); objects use
   the installed hash-builder hooks (the generated TU owns the hash type). Each
   in-progress container is GC-rooted so a nested allocation can't collect it. */

#define JP_MAX_DEPTH 200

/* An object's keys repeat across the objects of an array (`[{"x": ..}, {"x": ..}]`):
   a key read inside an array is kept, as the frozen key a Hash stores as is
   (sp_hash_key_from_bytes), and handed out again for the same bytes. It is
   ruby/json's per-parse name cache: a sorted array searched by length and
   bytes, for keys of at most JP_KEY_MAX bytes that begin with a letter (an
   identifier, where an id or a number key would only fill it), with no
   eviction once it is full. It lives for one parse, in the reader, and
   nothing in the parse runs the program's code. A Hash need not keep a key
   the cache hands it (one with the same bytes written with an escape came
   first; symbolize_names: makes a Symbol of it), so the cache holds its keys
   itself: `held`, an array the parse roots, made at the first key cached. */
#define JP_KEYS 63
#define JP_KEY_MAX 55
typedef struct { const char *key; size_t len; } jkey;
typedef struct {
  const char *p, *end;
  int in_array;                 /* arrays open around the read */
  int nkeys; jkey keys[JP_KEYS];   /* by length, then bytes */
  sp_RbVal *held;               /* the cached keys, kept alive for the parse */
} jrd;

static SP_NORETURN void jp_err(const char *msg) {
  sp_raise_cls("JSON::ParserError", msg);
}
static void jp_ws(jrd *j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
    else break;
  }
}
static sp_RbVal jp_value(jrd *j, int depth);

static void jp_hex4(jrd *j, jbuf *b, unsigned *out) {
  if (j->end - j->p < 4) { free(b->p); jp_err("incomplete \\u escape"); }
  unsigned cp = 0;
  for (int i = 0; i < 4; i++) {
    char h = *j->p++;
    cp <<= 4;
    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
    else { free(b->p); jp_err("invalid \\u hex digit"); }
  }
  *out = cp;
}
static void jp_utf8(jbuf *b, unsigned cp) {
  if (cp < 0x80) jb_c(b, (char)cp);
  else if (cp < 0x800) { jb_c(b, (char)(0xC0 | (cp >> 6))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { jb_c(b, (char)(0xE0 | (cp >> 12))); jb_c(b, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
  else { jb_c(b, (char)(0xF0 | (cp >> 18))); jb_c(b, (char)(0x80 | ((cp >> 12) & 0x3F))); jb_c(b, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
}
/* Parse a "..." string (cursor at the opening quote) into a GC string. */
static const char *jp_string(jrd *j) {
  if (j->p >= j->end || *j->p != '"') jp_err("expected a string");
  j->p++;
  /* the common string has no escape: its bytes are the input's, copied once
     into a string of their length, where a byte-at-a-time buffer grew by
     realloc and was copied again. An escape or a control character leaves
     the scan to the loop below, from the string's start. */
  const char *q = j->p;
  while (q < j->end && *q != '"' && *q != '\\' && (unsigned char)*q >= 0x20) q++;
  if (q < j->end && *q == '"') {
    size_t n = (size_t)(q - j->p);
    char *r = sp_str_alloc(n);
    memcpy(r, j->p, n);
    sp_str_set_len(r, n);
    j->p = q + 1;
    return r;
  }
  jbuf b; memset(&b, 0, sizeof b);
  while (j->p < j->end && *j->p != '"') {
    char c = *j->p++;
    /* RFC 8259 s7: control chars U+0000..U+001F must be escaped */
    if ((unsigned char)c < 0x20) { free(b.p); jp_err("unescaped control character in string"); }
    if (c != '\\') { jb_c(&b, c); continue; }
    if (j->p >= j->end) { free(b.p); jp_err("unterminated escape"); }
    char e = *j->p++;
    switch (e) {
      case '"': jb_c(&b, '"'); break;   case '\\': jb_c(&b, '\\'); break;
      case '/': jb_c(&b, '/'); break;   case 'n': jb_c(&b, '\n'); break;
      case 't': jb_c(&b, '\t'); break;  case 'r': jb_c(&b, '\r'); break;
      case 'b': jb_c(&b, '\b'); break;  case 'f': jb_c(&b, '\f'); break;
      case 'u': {
        unsigned cp; jp_hex4(j, &b, &cp);
        /* a high surrogate needs a low one after it, as CRuby's parser has
           it ("\ud800x" and "\ud800A" raise); a lone low surrogate is
           written as its bytes, as there */
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (j->end - j->p < 2 || j->p[0] != '\\' || j->p[1] != 'u') { free(b.p); jp_err("incomplete surrogate pair"); }
          j->p += 2; unsigned lo; jp_hex4(j, &b, &lo);
          if (lo < 0xDC00 || lo > 0xDFFF) { free(b.p); jp_err("invalid surrogate pair"); }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        jp_utf8(&b, cp);
        break;
      }
      default: free(b.p); jp_err("invalid escape");
    }
  }
  if (j->p >= j->end) { free(b.p); jp_err("unterminated string"); }
  j->p++;  /* closing quote */
  return jb_finish(&b);
}
/* An object's key: the frozen key the Hash stores as is, made from the
   input's bytes in one allocation where a String read for it was copied
   again by the Hash, and shared through the reader's cache (jrd) inside an
   array. A key with an escape or a control character is read as a String,
   which the Hash copies. */
static const char *jp_key(jrd *j) {
  if (j->p >= j->end || *j->p != '"') jp_err("expected a string");
  const char *s = j->p + 1, *q = s;
  while (q < j->end && *q != '"' && *q != '\\' && (unsigned char)*q >= 0x20) q++;
  if (q >= j->end || *q != '"') return jp_string(j);
  size_t n = (size_t)(q - s);
  j->p = q + 1;
  int lead = n ? (unsigned char)s[0] : 0;
  if (!j->in_array || n > JP_KEY_MAX || !((lead >= 'a' && lead <= 'z') || (lead >= 'A' && lead <= 'Z')))
    return sp_hash_key_from_bytes(s, n, 0);
  int lo = 0, hi = j->nkeys;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    const jkey *e = &j->keys[mid];
    int c = e->len < n ? -1 : e->len > n ? 1 : memcmp(e->key, s, n);
    if (c == 0) return e->key;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  if (j->nkeys < JP_KEYS && j->held->tag == SP_TAG_NIL) *j->held = sp_box_poly_array(sp_PolyArray_new());
  const char *k = sp_hash_key_from_bytes(s, n, 0);
  if (j->nkeys < JP_KEYS) {
    SP_GC_ROOT_STR(k);
    sp_PolyArray_push((sp_PolyArray *)j->held->v.p, sp_box_str(k));
    memmove(&j->keys[lo + 1], &j->keys[lo], sizeof(jkey) * (size_t)(j->nkeys - lo));
    j->keys[lo].key = k; j->keys[lo].len = n;
    j->nkeys++;
  }
  return k;
}
/* A decimal of at most 15 significant digits (an integer below 2^53, so an
   exact double) scaled by a power of ten within 10^22 (each an exact double)
   is one IEEE division or product of two exact doubles, which is correctly
   rounded: the value strtod answers, without its general multi-precision
   path (Clinger's fast path, "How to Read Floating Point Numbers
   Accurately", 1990; Go's strconv tries the same test first). Arithmetic
   carried in a wider type rounds twice, so a build that does not evaluate
   doubles as doubles (x87, FLT_EVAL_METHOD != 0) leaves every number to
   strtod. The digits come as jp_number found them: the integer part, the
   fraction (empty without one) and the exponent's (empty without one), so
   a number with too many digits is turned away by its lengths alone. */
static int jp_float_exact(int neg, const char *ip, const char *ie, const char *fp, const char *fe,
                          int xneg, const char *xp, const char *xe, double *out) {
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
  static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
  const char *f0 = fp;
  /* a zero integer part and the zeros that follow it are not significant:
     0.000123 holds three digits */
  if (ie - ip == 1 && *ip == '0') { ip = ie; while (f0 < fe && *f0 == '0') f0++; }
  if ((ie - ip) + (fe - f0) > 15 || xe - xp > 3) return 0;
  uint64_t m = 0;
  for (; ip < ie; ip++) m = m * 10 + (uint64_t)(*ip - '0');
  for (; f0 < fe; f0++) m = m * 10 + (uint64_t)(*f0 - '0');
  int x = 0;
  for (; xp < xe; xp++) x = x * 10 + (*xp - '0');
  ptrdiff_t k = (ptrdiff_t)(xneg ? -x : x) - (fe - fp);
  if (k < -22 || k > 22) return 0;
  /* the sign goes on first, so a directed rounding mode (fesetround through
     FFI) rounds the signed value, as strtod does */
  double r = neg ? -(double)m : (double)m;
  *out = k < 0 ? r / p10[-k] : r * p10[k];
  return 1;
#else
  (void)neg; (void)ip; (void)ie; (void)fp; (void)fe; (void)xneg; (void)xp; (void)xe; (void)out;
  return 0;
#endif
}
static sp_RbVal jp_number(jrd *j) {
  const char *start = j->p;
  int is_float = 0;
  if (j->p < j->end && *j->p == '-') j->p++;
  const char *ds = j->p;
  /* integer part (RFC 8259 s6): a lone 0, or 1-9 then digits -- no leading zeros */
  if (j->p < j->end && *j->p == '0') {
    j->p++;
    if (j->p < j->end && *j->p >= '0' && *j->p <= '9') jp_err("leading zero in number");
  }
  else {
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == ds) jp_err("expected a digit in number");
  }
  /* the integer digits end at de; the fraction's and the exponent's are
     empty (at de) without one */
  const char *de = j->p, *fs = de, *fe = de, *es = de, *ee = de;
  int xneg = 0;
  /* fraction: '.' then at least one digit */
  if (j->p < j->end && *j->p == '.') {
    is_float = 1; j->p++;
    fs = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == fs) jp_err("expected a digit after '.'");
    fe = j->p;
  }
  /* exponent: e/E, optional sign, then at least one digit */
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    is_float = 1; j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) xneg = *j->p++ == '-';
    es = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == es) jp_err("expected a digit in exponent");
    ee = j->p;
  }
  size_t n = (size_t)(j->p - start);
  if (n == 0) jp_err("invalid number");
  /* an Integer of at most 18 digits cannot pass int64: its value is read
     straight off the input, with no copy for strtoll */
  if (!is_float && n - (*start == '-') <= 18) {
    long long v = 0;
    for (const char *d = start + (*start == '-'); d < j->p; d++) v = v * 10 + (*d - '0');
    return sp_box_i64((int64_t)(*start == '-' ? -v : v));
  }
  if (is_float) {
    double f;
    if (jp_float_exact(*start == '-', ds, de, fs, fe, xneg, es, ee, &f))
      return sp_box_float(f);
  }
  /* the digits as a C string: a stack buffer for the usual width, the heap
     for an integer longer than that, which is a Bignum and has no limit */
  char stk[64];
  char *tmp = n < sizeof stk ? stk : (char *)malloc(n + 1);
  if (!tmp) jp_err("invalid number");
  memcpy(tmp, start, n); tmp[n] = 0;
  sp_RbVal r;
  if (is_float) r = sp_box_float(strtod(tmp, NULL));
  else {
    /* past int64 strtoll clamps with ERANGE and the value was answered
       clamped (9223372036854775808 read back as ...807); the digits
       themselves make the Bignum instead */
    errno = 0;
    long long v = strtoll(tmp, NULL, 10);
    if (errno == ERANGE) r = sp_box_bigint(sp_bigint_new_str(tmp, 10));
    else r = sp_box_i64((int64_t)v);   /* a Bignum past a 32-bit sp_int */
  }
  if (tmp != stk) free(tmp);
  return r;
}
static sp_RbVal jp_array(jrd *j, int depth) {
  j->p++;  /* '[' */
  sp_RbVal box = sp_box_poly_array(sp_PolyArray_new());
  SP_GC_ROOT_RBVAL(box);
  jp_ws(j);
  if (j->p < j->end && *j->p == ']') { j->p++; return box; }
  for (;;) {
    j->in_array++;
    sp_RbVal v = jp_value(j, depth + 1);
    j->in_array--;
    sp_PolyArray_push((sp_PolyArray *)box.v.p, v);
    jp_ws(j);
    if (j->p >= j->end) jp_err("unterminated array");
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == ']') { j->p++; break; }
    jp_err("expected ',' or ']' in array");
  }
  return box;
}
static sp_RbVal jp_object(jrd *j, int depth) {
  j->p++;  /* '{' */
  sp_RbVal box = sp_json_mk_hash_fn();
  SP_GC_ROOT_RBVAL(box);
  jp_ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; return box; }
  for (;;) {
    jp_ws(j);
    const char *key = jp_key(j);
    SP_GC_ROOT_STR(key);   /* survive the value parse; the hash stores the ptr */
    jp_ws(j);
    if (j->p >= j->end || *j->p != ':') jp_err("expected ':' in object");
    j->p++;
    sp_RbVal v = jp_value(j, depth + 1);
    sp_json_hash_set_fn(box, key, v);
    jp_ws(j);
    if (j->p >= j->end) jp_err("unterminated object");
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; break; }
    jp_err("expected ',' or '}' in object");
  }
  return box;
}
static sp_RbVal jp_value(jrd *j, int depth) {
  if (depth > JP_MAX_DEPTH) jp_err("nesting too deep");
  jp_ws(j);
  if (j->p >= j->end) jp_err("unexpected end of input");
  char c = *j->p;
  if (c == '{') return jp_object(j, depth);
  if (c == '[') return jp_array(j, depth);
  if (c == '"') return sp_box_str(jp_string(j));
  if (c == 't') { if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4))  { j->p += 4; return sp_box_bool(1); } jp_err("expected 'true'"); }
  if (c == 'f') { if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return sp_box_bool(0); } jp_err("expected 'false'"); }
  if (c == 'n') { if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4))  { j->p += 4; return sp_box_nil(); } jp_err("expected 'null'"); }
  if (c == '-' || (c >= '0' && c <= '9')) return jp_number(j);
  jp_err("unexpected character");
}

sp_RbVal sp_json_parse(const char *s) {
  /* The reader walks `s` in place while the parse allocates every container
     and string it builds, so the input has to stay rooted for the whole walk:
     a caller passing it straight in (`JSON.parse File.read path`) holds it in
     nothing but the C argument slot, and the first collection under it leaves
     the cursor reading freed memory -- a parse error whose message varies by
     run on a small document, a segfault on a large one. */
  SP_GC_ROOT_STR(s);
  sp_RbVal held = sp_box_nil();
  SP_GC_ROOT_RBVAL(held);
  jrd j;
  j.in_array = 0; j.nkeys = 0; j.held = &held;
  j.p = s ? s : "";
  j.end = j.p + (s ? sp_str_byte_len(s) : 0);
  sp_RbVal v = jp_value(&j, 0);
  jp_ws(&j);
  if (j.p != j.end) jp_err("unexpected trailing characters");
  return v;
}
