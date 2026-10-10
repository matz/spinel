/* sp_iobuffer.c -- IO::Buffer, CRuby 4.x semantics (see sp_iobuffer.h for
   the memory model). Bound through packages/io/io/buffer.rb; that file is
   spliced into any program referencing `IO::Buffer`, so this object is
   pulled from libspinel_rt.a only when the class is actually used.

   Conversion edges follow CRuby's NUM2* family: 8/16-bit stores wrap any
   64-bit value; 32-bit stores range-check ([-2^31, 2^32-1] unsigned,
   [-2^31, 2^31-1] signed); 64-bit stores accept the full two's-complement
   range plus Bignums up to the unsigned bound. Messages mirror CRuby's,
   including the "integer ... to" (fixnum) / "bignum ... into" wording. */
#include "sp_iobuffer.h"
#include "sp_io.h"       /* the IO integration: sp_File, the readiness parks */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

extern const char *sp_sprintf(const char *fmt, ...);
extern SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);

/* sp_bigint.c helpers (sp_bigint.h is mruby-shim territory; the u64 pair is
   added beside the other serialization helpers there) */
extern sp_Bigint *sp_bigint_new_u64(uint64_t v);
extern int sp_bigint_mag_u64(sp_Bigint *b, uint64_t *out);   /* 1 iff |b| < 2^64 */
extern int sp_bigint_sign(sp_Bigint *b);
extern double sp_bigint_to_double(sp_Bigint *b);

/* ---- raising ---- */

static SP_NORETURN void iob_arg(const char *msg) { sp_raise_cls("ArgumentError", msg); }

/* The lock does NOT gate data access: reads and writes (and slice/dup/
   copy/each/...) all run inside locked { }. Only ownership-shaped
   operations refuse -- free / resize / transfer / a second locked { } --
   each with its own message (matched against CRuby empirically; note the
   lock also survives an exception out of the block). */
static void iob_readable(sp_IOBuffer *b) { (void)b; }
static void iob_writable(sp_IOBuffer *b) {
  if (b->flags & SP_IOB_READONLY) sp_raise_cls("IO::Buffer::AccessError", "Buffer is not writable!");
}

/* the value's Ruby spelling for TypeError messages (nil/true/false read as
   themselves, like CRuby's) */
static const char *iob_val_name(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_NIL:  return "nil";
    case SP_TAG_BOOL: return v.v.i ? "true" : "false";
    case SP_TAG_INT: case SP_TAG_BIGINT: return "Integer";
    case SP_TAG_FLT:  return "Float";
    case SP_TAG_STR:  return "String";
    case SP_TAG_SYM:  return "Symbol";
    case SP_TAG_CLASS: return "Class";
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:
        case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_PTR_ARRAY:
        case SP_BUILTIN_SYM_ARRAY: case SP_BUILTIN_POLY_ARRAY:
          return "Array";
        case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
        case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_SYM_INT_HASH:
        case SP_BUILTIN_SYM_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
        case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
          return "Hash";
        case SP_BUILTIN_RANGE: return "Range";
        case SP_BUILTIN_TIME:  return "Time";
        case SP_BUILTIN_PROC:  return "Proc";
        case SP_BUILTIN_IO:    return "IO";
        default: return "Object";
      }
    default: return "Object";
  }
}

/* ---- base pointer + bounds ---- */

static uint8_t *iob_base(sp_IOBuffer *b) {
  if (b->source) return b->source->data ? b->source->data + b->off : NULL;
  return b->data;
}

/* A slice whose root was freed, transferred, or shrunk below the view's
   range must not be read through: CRuby raises InvalidatedError there, and
   the stale base would be NULL or past the live allocation. */
static int iob_slice_invalid(sp_IOBuffer *b) {
  return b->source && (!b->source->data || b->off > b->source->size - b->size);
}
static uint8_t *iob_ptr(sp_IOBuffer *b) {
  if (iob_slice_invalid(b))
    sp_raise_cls("IO::Buffer::InvalidatedError", "Buffer has been invalidated!");
  return iob_base(b);
}

/* Overflow-safe: `off + len > size` wraps for large operands, so compare
   against the remaining room instead (both are non-negative here). */
static void iob_range(sp_IOBuffer *b, int64_t off, int64_t len) {
  if (off < 0) iob_arg("Offset can't be negative!");
  if (len < 0) iob_arg("Length can't be negative!");
  if (off > b->size || len > b->size - off) iob_arg("Specified offset+length is bigger than the buffer size!");
}

/* ---- the type table ---- */

typedef struct { const char *name; int8_t width; int8_t be; int8_t sign; int8_t flt; } IobType;
static const IobType iob_types[SP_IOB_TY__COUNT] = {
  [SP_IOB_TY_U8]  = {"U8", 1, 0, 0, 0},  [SP_IOB_TY_S8]  = {"S8", 1, 0, 1, 0},
  [SP_IOB_TY_u16] = {"u16", 2, 0, 0, 0}, [SP_IOB_TY_s16] = {"s16", 2, 0, 1, 0},
  [SP_IOB_TY_U16] = {"U16", 2, 1, 0, 0}, [SP_IOB_TY_S16] = {"S16", 2, 1, 1, 0},
  [SP_IOB_TY_u32] = {"u32", 4, 0, 0, 0}, [SP_IOB_TY_s32] = {"s32", 4, 0, 1, 0},
  [SP_IOB_TY_U32] = {"U32", 4, 1, 0, 0}, [SP_IOB_TY_S32] = {"S32", 4, 1, 1, 0},
  [SP_IOB_TY_u64] = {"u64", 8, 0, 0, 0}, [SP_IOB_TY_s64] = {"s64", 8, 0, 1, 0},
  [SP_IOB_TY_U64] = {"U64", 8, 1, 0, 0}, [SP_IOB_TY_S64] = {"S64", 8, 1, 1, 0},
  [SP_IOB_TY_f32] = {"f32", 4, 0, 1, 1}, [SP_IOB_TY_f64] = {"f64", 8, 0, 1, 1},
  [SP_IOB_TY_F32] = {"F32", 4, 1, 1, 1}, [SP_IOB_TY_F64] = {"F64", 8, 1, 1, 1},
};

int sp_IOBuffer_type_of(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < SP_IOB_TY__COUNT; i++)
    if (strcmp(iob_types[i].name, name) == 0) return i;
  return -1;
}

static int iob_decode_type(sp_RbVal type) {
  if (type.tag == SP_TAG_SYM && sp_sym_name_fn) {
    int t = sp_IOBuffer_type_of(sp_sym_name_fn((sp_sym)type.v.i));
    if (t >= 0) return t;
  }
  iob_arg("Invalid type name!");
  return -1;
}

static uint8_t *iob_value_ptr(sp_IOBuffer *b, int64_t off, int width) {
  if (off < 0) iob_arg("Offset can't be negative!");
  if (b->size < width || off > b->size - width)
    iob_arg(sp_sprintf("Type extends beyond end of buffer! (offset=%lld > size=%lld)",
                       (long long)off, (long long)b->size));
  return iob_ptr(b) + off;
}

/* ---- raw loads/stores (byte-assembled: host-endian agnostic) ---- */

static uint64_t iob_load(const uint8_t *p, int width, int be) {
  uint64_t v = 0;
  if (be) for (int i = 0; i < width; i++) v = (v << 8) | p[i];
  else    for (int i = 0; i < width; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}
static void iob_store(uint8_t *p, int width, int be, uint64_t v) {
  if (be) for (int i = 0; i < width; i++) p[i] = (uint8_t)(v >> (8 * (width - 1 - i)));
  else    for (int i = 0; i < width; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ---- value conversion (CRuby NUM2* semantics + messages) ---- */

static int64_t iob_float_to_i64(double f) {
  if (isnan(f)) sp_raise_cls("FloatDomainError", "NaN");
  if (isinf(f)) sp_raise_cls("FloatDomainError", f > 0 ? "Infinity" : "-Infinity");
  f = trunc(f);
  if (f < -9223372036854775808.0 || f >= 9223372036854775808.0)
    sp_raise_cls("RangeError", "float out of range of integer");
  return (int64_t)f;
}

/* Convert `val` to the store word for integer type `ty`. */
static uint64_t iob_int_operand(int ty, sp_RbVal val) {
  const IobType *t = &iob_types[ty];
  const char *cname =
    t->width == 8 ? (t->sign ? "long long" : "unsigned long long")
    : t->width == 4 ? (t->sign ? "int" : "unsigned int")
                    : (t->sign ? "long" : "unsigned long");
  int64_t iv;
  if (val.tag == SP_TAG_INT) iv = val.v.i;
  else if (val.tag == SP_TAG_FLT) iv = iob_float_to_i64(val.v.f);
  else if (val.tag == SP_TAG_BIGINT) {
    sp_Bigint *bg = (sp_Bigint *)val.v.p;
    uint64_t mag;
    int fits = sp_bigint_mag_u64(bg, &mag);
    int neg = sp_bigint_sign(bg) < 0;
    if (t->width == 8 && !t->sign) {
      /* u64: [-2^63, 2^64-1] */
      if (!neg) {
        if (!fits) sp_raise_cls("RangeError", sp_sprintf("bignum too big to convert into '%s'", cname));
        return mag;
      }
      if (!fits || mag > (uint64_t)1 << 63)
        sp_raise_cls("RangeError", sp_sprintf("bignum out of range of %s", cname));
      return (uint64_t)0 - mag;
    }
    if (t->width <= 2) {
      /* 8/16-bit: any value representable in 64 bits wraps */
      if (!neg) {
        if (!fits) sp_raise_cls("RangeError", sp_sprintf("bignum too big to convert into '%s'", cname));
        return mag;
      }
      if (!fits || mag > (uint64_t)1 << 63)
        sp_raise_cls("RangeError", sp_sprintf("bignum too small to convert into '%s'", cname));
      return (uint64_t)0 - mag;
    }
    /* s64: a Bignum is out of range except at the negative bound (any heap
       Bignum a 64-bit build makes has |v| >= 2^63; smaller ones stay sp_int). */
    if (t->width == 8 && t->sign) {
      if (fits && !neg && mag <= (uint64_t)INT64_MAX) return mag;
      if (fits && neg && mag <= (uint64_t)1 << 63) return (uint64_t)0 - mag;
    }
    /* 32-bit: the type's own width decides, not how the value is held. On a
       32-bit build sp_int is 32 bits wide and every value past 2^31-1 is a
       heap Bignum, 0xCAFEBABE included, which is most of the range a U32 is
       asked for (#4647). A magnitude that fits the field is stored. */
    if (t->width == 4 && fits) {
      uint64_t hi = t->sign ? ((uint64_t)1 << 31) - 1 : ((uint64_t)1 << 32) - 1;
      if (!neg && mag <= hi) return mag;
      if (neg && mag <= (uint64_t)1 << 31) return (uint64_t)0 - mag;
    }
    sp_raise_cls("RangeError", sp_sprintf(neg ? "bignum too small to convert into '%s'"
                                              : "bignum too big to convert into '%s'", cname));
    return 0;
  }
  else {
    sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into Integer", iob_val_name(val)));
    return 0;
  }
  if (t->width == 4) {
    int64_t lo = -((int64_t)1 << 31);
    int64_t hi = t->sign ? ((int64_t)1 << 31) - 1 : ((int64_t)1 << 32) - 1;
    if (iv < lo) sp_raise_cls("RangeError", sp_sprintf("integer %lld too small to convert to '%s'", (long long)iv, cname));
    if (iv > hi) sp_raise_cls("RangeError", sp_sprintf("integer %lld too big to convert to '%s'", (long long)iv, cname));
  }
  return (uint64_t)iv;
}

static double iob_float_operand(sp_RbVal val) {
  if (val.tag == SP_TAG_FLT) return val.v.f;
  if (val.tag == SP_TAG_INT) return (double)val.v.i;
  if (val.tag == SP_TAG_BIGINT) return sp_bigint_to_double((sp_Bigint *)val.v.p);
  if (val.tag == SP_TAG_OBJ && val.cls_id == SP_BUILTIN_RATIONAL && val.v.p) {
    sp_Rational *r = (sp_Rational *)val.v.p;
    return (double)r->num / (double)r->den;
  }
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", iob_val_name(val)));
  return 0;
}

/* ---- boxed load/store cores shared by the generic and typed entries ---- */

static sp_RbVal iob_get_core(sp_IOBuffer *b, int ty, int64_t off) {
  const IobType *t = &iob_types[ty];
  iob_readable(b);
  const uint8_t *p = iob_value_ptr(b, off, t->width);
  if (t->flt) {
    uint64_t raw = iob_load(p, t->width, t->be);
    if (t->width == 4) { union { uint32_t u; float f; } u; u.u = (uint32_t)raw; return sp_box_float(u.f); }
    union { uint64_t u; double f; } u; u.u = raw; return sp_box_float(u.f);
  }
  uint64_t raw = iob_load(p, t->width, t->be);
  /* A loaded value goes out as a machine int when it fits sp_int, and as a
     Bignum otherwise. That is by the WIDTH OF sp_int, not of the field: on a
     32-bit build a U32 above 2**31-1 is a Bignum the same way a u64 above
     2**63-1 is on a 64-bit one (#4647). */
  if (t->sign) {
    /* sign-extend from width */
    int shift = 64 - 8 * t->width;
    int64_t sv = (int64_t)(raw << shift) >> shift;
    if (sv < (int64_t)INTPTR_MIN || sv > (int64_t)INTPTR_MAX) {
      SP_GC_ROOT(b);
      return sp_box_bigint(sp_bigint_new_int(sv));
    }
    return sp_box_int((sp_int)sv);
  }
  if (raw > (uint64_t)INTPTR_MAX) {
    SP_GC_ROOT(b);
    return sp_box_bigint(sp_bigint_new_u64(raw));
  }
  return sp_box_int((sp_int)raw);
}

static sp_int iob_set_core(sp_IOBuffer *b, int ty, int64_t off, sp_RbVal val) {
  const IobType *t = &iob_types[ty];
  iob_writable(b);
  uint8_t *p = iob_value_ptr(b, off, t->width);
  if (t->flt) {
    double f = iob_float_operand(val);
    uint64_t raw;
    if (t->width == 4) { union { uint32_t u; float f; } u; u.f = (float)f; raw = u.u; }
    else { union { uint64_t u; double f; } u; u.f = f; raw = u.u; }
    iob_store(p, t->width, t->be, raw);
  }
  else {
    iob_store(p, t->width, t->be, iob_int_operand(ty, val));
  }
  return t->width;
}

/* ---- constructors ---- */

static void iob_scan(void *p) {
  sp_IOBuffer *b = (sp_IOBuffer *)p;
  if (b->source) sp_gc_mark(b->source);
}
/* Give the allocation back: munmap for a mapped file, free otherwise. */
static void iob_release(sp_IOBuffer *b) {
  if (b->map_base) { munmap(b->map_base, b->map_len); b->map_base = NULL; b->map_len = 0; }
  else free(b->data);
  b->data = NULL;
}
void sp_IOBuffer_fin(void *p) {
  iob_release((sp_IOBuffer *)p);
}

static sp_IOBuffer *iob_alloc_obj(sp_int cls_id) {
  sp_IOBuffer *b = (sp_IOBuffer *)sp_gc_alloc(sizeof(sp_IOBuffer), sp_IOBuffer_fin, iob_scan);
  memset(b, 0, sizeof *b);
  b->cls_id = cls_id;
  return b;
}

sp_int sp_IOBuffer_page_size(void) {
  long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? (sp_int)ps : 4096;
}

static void iob_allocate(sp_IOBuffer *b, int64_t size, uint32_t flags, int flags_given) {
  if (size < 0) iob_arg("Size can't be negative!");
  if (!flags_given)
    flags = size == 0 ? 0
          : size >= (int64_t)sp_IOBuffer_page_size() ? SP_IOB_MAPPED : SP_IOB_INTERNAL;
  else if (size > 0 && !(flags & (SP_IOB_INTERNAL | SP_IOB_MAPPED)))
    sp_raise_cls("IO::Buffer::AllocationError", "Could not allocate buffer!");
  b->flags = flags;
  b->size = size;
  if (size > 0) {
    b->data = (uint8_t *)calloc(1, (size_t)size);
    if (!b->data) sp_oom_die();
  }
}

sp_IOBuffer *sp_IOBuffer_new(sp_int cls_id) {
  sp_IOBuffer *b = iob_alloc_obj(cls_id);
  iob_allocate(b, 65536, 0, 0);
  return b;
}
sp_IOBuffer *sp_IOBuffer_new_i(sp_int cls_id, sp_int size) {
  sp_IOBuffer *b = iob_alloc_obj(cls_id);
  iob_allocate(b, size, 0, 0);
  return b;
}
sp_IOBuffer *sp_IOBuffer_new_if(sp_int cls_id, sp_int size, sp_int flags) {
  sp_IOBuffer *b = iob_alloc_obj(cls_id);
  iob_allocate(b, size, (uint32_t)flags, 1);
  return b;
}

sp_IOBuffer *sp_IOBuffer_become_for(sp_IOBuffer *b, const char *s) {
  size_t n = sp_str_byte_len(s);
  uint8_t *d = (uint8_t *)malloc(n ? n : 1);   /* non-NULL even for "": for("") is not a null buffer */
  if (!d) sp_oom_die();
  memcpy(d, s, n);
  iob_release(b);
  b->data = d;
  b->source = NULL;
  b->off = 0;
  b->size = (int64_t)n;
  b->flags = SP_IOB_EXTERNAL | SP_IOB_READONLY | SP_IOB_SLICE;
  return b;
}

/* ---- generic + typed accessors ---- */

sp_RbVal sp_IOBuffer_get_value(sp_IOBuffer *b, sp_RbVal type, sp_int off) {
  return iob_get_core(b, iob_decode_type(type), off);
}
sp_int sp_IOBuffer_set_value(sp_IOBuffer *b, sp_RbVal type, sp_int off, sp_RbVal val) {
  return iob_set_core(b, iob_decode_type(type), off, val);
}

sp_int sp_IOBuffer_get_i(sp_IOBuffer *b, sp_int ty, sp_int off) {
  const IobType *t = &iob_types[ty];
  iob_readable(b);
  const uint8_t *p = iob_value_ptr(b, off, t->width);
  uint64_t raw = iob_load(p, t->width, t->be);
  if (t->sign) { int shift = 64 - 8 * t->width; return (int64_t)(raw << shift) >> shift; }
  return (sp_int)raw;
}
sp_RbVal sp_IOBuffer_get_x(sp_IOBuffer *b, sp_int ty, sp_int off) {
  return iob_get_core(b, (int)ty, off);
}
double sp_IOBuffer_get_f(sp_IOBuffer *b, sp_int ty, sp_int off) {
  const IobType *t = &iob_types[ty];
  iob_readable(b);
  const uint8_t *p = iob_value_ptr(b, off, t->width);
  uint64_t raw = iob_load(p, t->width, t->be);
  if (t->width == 4) { union { uint32_t u; float f; } u; u.u = (uint32_t)raw; return u.f; }
  union { uint64_t u; double f; } u; u.u = raw; return u.f;
}
sp_int sp_IOBuffer_set_i(sp_IOBuffer *b, sp_int ty, sp_int off, sp_int v) {
  return iob_set_core(b, ty, off, sp_box_int(v));
}
sp_int sp_IOBuffer_set_f(sp_IOBuffer *b, sp_int ty, sp_int off, double v) {
  return iob_set_core(b, ty, off, sp_box_float(v));
}
/* The typed lowering with a BOXED value: the generic path minus the symbol
   decode. A boxed value cannot go through set_i -- an integer type takes a
   Float (truncating, as CRuby does) and u64/s64 take a Bignum -- so it
   reaches iob_set_core unexamined, exactly as set_value leaves it. */
sp_int sp_IOBuffer_set_v(sp_IOBuffer *b, sp_int ty, sp_int off, sp_RbVal v) {
  return iob_set_core(b, ty, off, v);
}

/* ---- strings ---- */

/* The bare-offset arm gets its own message only when the LENGTH was
   defaulted (CRuby: get_string(9) names the offset, get_string(9, 0) the
   offset+length). */
static const char *iob_get_string(sp_IOBuffer *b, int64_t off, int64_t len, int len_given) {
  SP_GC_ROOT(b);
  iob_readable(b);
  if (off < 0) iob_arg("Offset can't be negative!");
  if (!len_given) {
    if (off > b->size) iob_arg("The given offset is bigger than the buffer size!");
    len = b->size - off;
  }
  if (len < 0) iob_arg("Length can't be negative!");
  if (off > b->size || len > b->size - off) iob_arg("Specified offset+length is bigger than the buffer size!");
  char *r = sp_str_alloc_raw((size_t)len + 1);
  if (len > 0) memcpy(r, iob_ptr(b) + off, (size_t)len);
  r[len] = '\0';
  sp_str_set_len(r, (size_t)len);
  sp_str_mark_binary(r);
  return r;
}
const char *sp_IOBuffer_get_string0(sp_IOBuffer *b) { return iob_get_string(b, 0, 0, 0); }
const char *sp_IOBuffer_get_string1(sp_IOBuffer *b, sp_int off) { return iob_get_string(b, off, 0, 0); }
const char *sp_IOBuffer_get_string2(sp_IOBuffer *b, sp_int off, sp_int len) { return iob_get_string(b, off, len, 1); }

static sp_int iob_set_string(sp_IOBuffer *b, const char *s, int64_t off, int64_t len, int64_t soff, int len_given) {
  iob_writable(b);
  int64_t slen = (int64_t)sp_str_byte_len(s);
  if (off < 0) iob_arg("Offset can't be negative!");
  if (soff < 0) iob_arg("Source offset can't be negative!");
  if (soff > slen) iob_arg("The given source offset is bigger than the source itself!");
  if (!len_given) len = slen - soff;
  if (len < 0) iob_arg("Length can't be negative!");
  /* destination range before source range, as CRuby orders them */
  if (off > b->size || len > b->size - off) iob_arg("Specified offset+length is bigger than the buffer size!");
  if (len > slen - soff) iob_arg("The computed source range exceeds the size of the source buffer!");
  if (len > 0) memmove(iob_ptr(b) + off, s + soff, (size_t)len);
  return len;
}
sp_int sp_IOBuffer_set_string1(sp_IOBuffer *b, const char *s) { return iob_set_string(b, s, 0, 0, 0, 0); }
sp_int sp_IOBuffer_set_string2(sp_IOBuffer *b, const char *s, sp_int off) { return iob_set_string(b, s, off, 0, 0, 0); }
sp_int sp_IOBuffer_set_string3(sp_IOBuffer *b, const char *s, sp_int off, sp_int len) { return iob_set_string(b, s, off, len, 0, 1); }
sp_int sp_IOBuffer_set_string4(sp_IOBuffer *b, const char *s, sp_int off, sp_int len, sp_int soff) { return iob_set_string(b, s, off, len, soff, 1); }

/* ---- whole-buffer operations ---- */

sp_int sp_IOBuffer_size(sp_IOBuffer *b) { return b->size; }

sp_IOBuffer *sp_IOBuffer_resize(sp_IOBuffer *b, sp_int size) {
  if (b->flags & SP_IOB_LOCKED)
    sp_raise_cls("IO::Buffer::LockedError", "Cannot resize locked buffer!");
  if (b->flags & SP_IOB_EXTERNAL) sp_raise_cls("IO::Buffer::AccessError", "Cannot resize external buffer!");
  /* a file mapping has the file's length; growing it would detach the
     view from the file it was made for, so it is refused as EXTERNAL is */
  if (b->map_base) sp_raise_cls("IO::Buffer::AccessError", "Cannot resize mapped file buffer!");
  if (size < 0) iob_arg("Size can't be negative!");
  if (b->source) {
    /* a slice detaches into its own (internal) allocation */
    uint8_t *base = iob_ptr(b);
    uint8_t *d = NULL;
    if (size > 0) {
      d = (uint8_t *)calloc(1, (size_t)size);
      if (!d) sp_oom_die();
      int64_t keep = b->size < size ? b->size : size;
      if (base && keep > 0) memcpy(d, base, (size_t)keep);
    }
    b->source = NULL;
    b->off = 0;
    b->data = d;
    b->size = size;
    b->flags = SP_IOB_INTERNAL | (b->flags & SP_IOB_READONLY);
    return b;
  }
  if (size == 0) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
    return b;
  }
  uint8_t *d = (uint8_t *)realloc(b->data, (size_t)size);
  if (!d) sp_oom_die();
  if (size > b->size) memset(d + b->size, 0, (size_t)(size - b->size));
  b->data = d;
  b->size = size;
  if (!(b->flags & (SP_IOB_INTERNAL | SP_IOB_MAPPED))) b->flags |= SP_IOB_INTERNAL;
  return b;
}

static sp_IOBuffer *iob_clear(sp_IOBuffer *b, int64_t v, int64_t off, int64_t len) {
  iob_writable(b);
  iob_range(b, off, len);
  if (len > 0) memset(iob_ptr(b) + off, (int)(uint8_t)v, (size_t)len);
  return b;
}
sp_IOBuffer *sp_IOBuffer_clear0(sp_IOBuffer *b) { return iob_clear(b, 0, 0, b->size); }
sp_IOBuffer *sp_IOBuffer_clear1(sp_IOBuffer *b, sp_int v) { return iob_clear(b, v, 0, b->size); }
sp_IOBuffer *sp_IOBuffer_clear2(sp_IOBuffer *b, sp_int v, sp_int off) {
  return iob_clear(b, v, off, off <= b->size ? b->size - off : 0);
}
sp_IOBuffer *sp_IOBuffer_clear3(sp_IOBuffer *b, sp_int v, sp_int off, sp_int len) { return iob_clear(b, v, off, len); }

static sp_IOBuffer *iob_other(sp_IOBuffer *b, sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == (int)b->cls_id) return (sp_IOBuffer *)v.v.p;
  sp_raise_cls("TypeError", sp_sprintf("wrong argument type %s (expected IO::Buffer)", iob_val_name(v)));
  return NULL;
}

static sp_int iob_copy(sp_IOBuffer *b, sp_RbVal srcv, int64_t off, int64_t len, int64_t soff, int len_given) {
  sp_IOBuffer *src = iob_other(b, srcv);
  iob_writable(b);
  iob_readable(src);
  if (off < 0) iob_arg("Offset can't be negative!");
  if (soff < 0) iob_arg("Source offset can't be negative!");
  if (soff > src->size) iob_arg("The given source offset is bigger than the source itself!");
  if (!len_given) len = src->size - soff;
  if (len < 0) iob_arg("Length can't be negative!");
  /* destination range before source range, as CRuby orders them */
  if (off > b->size || len > b->size - off) iob_arg("Specified offset+length is bigger than the buffer size!");
  if (len > src->size - soff)
    iob_arg("The computed source range exceeds the size of the source buffer!");
  if (len > 0) memmove(iob_ptr(b) + off, iob_ptr(src) + soff, (size_t)len);
  return len;
}
sp_int sp_IOBuffer_copy1(sp_IOBuffer *b, sp_RbVal src) { return iob_copy(b, src, 0, 0, 0, 0); }
sp_int sp_IOBuffer_copy2(sp_IOBuffer *b, sp_RbVal src, sp_int off) { return iob_copy(b, src, off, 0, 0, 0); }
sp_int sp_IOBuffer_copy3(sp_IOBuffer *b, sp_RbVal src, sp_int off, sp_int len) { return iob_copy(b, src, off, len, 0, 1); }
sp_int sp_IOBuffer_copy4(sp_IOBuffer *b, sp_RbVal src, sp_int off, sp_int len, sp_int soff) { return iob_copy(b, src, off, len, soff, 1); }

static sp_IOBuffer *iob_slice(sp_IOBuffer *b, int64_t off, int64_t len) {
  SP_GC_ROOT(b);
  iob_readable(b);
  if (iob_slice_invalid(b))
    sp_raise_cls("IO::Buffer::InvalidatedError", "Buffer has been invalidated!");
  iob_range(b, off, len);
  sp_IOBuffer *s = iob_alloc_obj(b->cls_id);
  s->source = b->source ? b->source : b;
  s->off = (b->source ? b->off : 0) + off;
  s->size = len;
  s->flags = SP_IOB_SLICE | (b->flags & SP_IOB_READONLY);
  return s;
}
sp_IOBuffer *sp_IOBuffer_slice0(sp_IOBuffer *b) { return iob_slice(b, 0, b->size); }
sp_IOBuffer *sp_IOBuffer_slice1(sp_IOBuffer *b, sp_int off) {
  return iob_slice(b, off, off <= b->size ? b->size - off : 0);
}
sp_IOBuffer *sp_IOBuffer_slice2(sp_IOBuffer *b, sp_int off, sp_int len) { return iob_slice(b, off, len); }

sp_IOBuffer *sp_IOBuffer_transfer(sp_IOBuffer *b) {
  SP_GC_ROOT(b);
  if (b->flags & SP_IOB_LOCKED)
    sp_raise_cls("IO::Buffer::LockedError", "Cannot transfer ownership of locked buffer!");
  sp_IOBuffer *t = iob_alloc_obj(b->cls_id);
  t->data = b->data;
  t->source = b->source;
  t->off = b->off;
  t->size = b->size;
  t->flags = b->flags;
  t->map_base = b->map_base;
  t->map_len = b->map_len;
  b->data = NULL;
  b->source = NULL;
  b->off = 0;
  b->size = 0;
  b->map_base = NULL;
  b->map_len = 0;
  /* the drained buffer keeps its flags (CRuby inspects as "NULL INTERNAL") */
  return t;
}

sp_IOBuffer *sp_IOBuffer_free_m(sp_IOBuffer *b) {
  if (b->flags & SP_IOB_LOCKED)
    sp_raise_cls("IO::Buffer::LockedError", "Buffer is locked!");
  iob_release(b);
  b->source = NULL;
  b->off = 0;
  b->size = 0;
  b->flags = 0;
  return b;
}

sp_IOBuffer *sp_IOBuffer_dup_m(sp_IOBuffer *b) {
  SP_GC_ROOT(b);
  iob_readable(b);
  sp_IOBuffer *d = iob_alloc_obj(b->cls_id);
  uint8_t *base = b->source ? iob_ptr(b) : b->data;
  if (base) {
    d->data = (uint8_t *)malloc(b->size > 0 ? (size_t)b->size : 1);
    if (!d->data) sp_oom_die();
    if (b->size > 0) memcpy(d->data, base, (size_t)b->size);
    d->size = b->size;
    d->flags = SP_IOB_INTERNAL;
  }
  return d;
}

/* ---- comparison ---- */

static sp_int iob_cmp_core(sp_IOBuffer *b, sp_IOBuffer *o) {
  iob_readable(b);
  iob_readable(o);
  int64_t n = b->size < o->size ? b->size : o->size;
  if (n > 0) {
    int c = memcmp(iob_ptr(b), iob_ptr(o), (size_t)n);
    if (c) return c < 0 ? -1 : 1;
  }
  return b->size < o->size ? -1 : b->size > o->size ? 1 : 0;
}
sp_int sp_IOBuffer_cmp(sp_IOBuffer *b, sp_RbVal other) { return iob_cmp_core(b, iob_other(b, other)); }
sp_bool sp_IOBuffer_eq(sp_IOBuffer *b, sp_RbVal other) {
  sp_IOBuffer *o = iob_other(b, other);
  if (b->size != o->size) return 0;
  return iob_cmp_core(b, o) == 0;
}

/* ---- rendering ---- */

/* One hexdump line into `out` (which has room): "0x%08x  " + width slots of
   "%02x " (or three spaces past the end) + the printable-ASCII column. */
static size_t iob_hexline(char *out, const uint8_t *base, int64_t addr, int64_t end, int64_t width) {
  char *w = out;
  w += sprintf(w, "0x%08llx ", (unsigned long long)addr);
  for (int64_t i = 0; i < width; i++) {
    if (addr + i < end) w += sprintf(w, " %02x", base[addr + i]);
    else { memcpy(w, "   ", 3); w += 3; }
  }
  *w++ = ' ';
  for (int64_t i = 0; i < width && addr + i < end; i++) {
    uint8_t c = base[addr + i];
    *w++ = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
  }
  *w = '\0';
  return (size_t)(w - out);
}

static const char *iob_hexdump(sp_IOBuffer *b, int64_t off, int64_t len, int64_t width, int len_given) {
  SP_GC_ROOT(b);
  iob_readable(b);
  if (b->source && iob_slice_invalid(b))
    sp_raise_cls("IO::Buffer::InvalidatedError", "Buffer has been invalidated!");
  if (!iob_base(b)) return NULL;
  if (off < 0) iob_arg("Offset can't be negative!");
  if (width < 1) iob_arg("Width must be at least 1!");
  if (!len_given) len = b->size - off;
  if (len < 0) iob_arg("Length can't be negative!");
  if (off > b->size) off = b->size;
  if (len > b->size - off) len = b->size - off;
  int64_t end = off + len;
  /* ceiling division without the additive form: len + width - 1 itself
     overflows for a near-INT64_MAX width */
  int64_t nlines = len / width + (len % width != 0);
  /* per line: the address prefix ("0x" + up to 16 hex digits + two spaces),
     width * 3 hex slots, a separator, width ASCII chars, the newline and
     NUL -- and `width` is caller data, so every derived size is checked
     before it reaches malloc (a huge width made the product wrap and the
     sprintf write past a too-small block). */
  uint64_t line_max, total;
  if (sp_ckd_mul_u64((uint64_t)width, (uint64_t)4, &line_max) ||
      sp_ckd_add_u64(line_max, (uint64_t)40, &line_max) ||
      sp_ckd_mul_u64((uint64_t)(nlines > 0 ? nlines : 1), line_max, &total) ||
      total > (uint64_t)1 << 40)
    sp_oom_die();
  char *tmp = (char *)malloc((size_t)total);
  if (!tmp) sp_oom_die();
  size_t used = 0;
  const uint8_t *base = iob_base(b);
  for (int64_t a = off; a < end; a += width) {
    if (a > off) tmp[used++] = '\n';
    used += iob_hexline(tmp + used, base, a, end, width);
  }
  char *r = sp_str_alloc_raw(used + 1);
  memcpy(r, tmp, used);
  r[used] = '\0';
  sp_str_set_len(r, used);
  free(tmp);
  return r;
}
const char *sp_IOBuffer_hexdump0(sp_IOBuffer *b) { return iob_hexdump(b, 0, 0, 16, 0); }
const char *sp_IOBuffer_hexdump1(sp_IOBuffer *b, sp_int off) { return iob_hexdump(b, off, 0, 16, 0); }
const char *sp_IOBuffer_hexdump2(sp_IOBuffer *b, sp_int off, sp_int len) { return iob_hexdump(b, off, len, 16, 1); }
const char *sp_IOBuffer_hexdump3(sp_IOBuffer *b, sp_int off, sp_int len, sp_int width) { return iob_hexdump(b, off, len, width, 1); }

static size_t iob_header(sp_IOBuffer *b, char *out, size_t cap) {
  uint8_t *base = iob_base(b);
  size_t n = (size_t)snprintf(out, cap, "#<IO::Buffer 0x%016llx+%lld",
                              (unsigned long long)(uintptr_t)base, (long long)b->size);
  static const struct { uint32_t bit; const char *name; } FLAGS[] = {
    {0, "NULL"}, {SP_IOB_EXTERNAL, "EXTERNAL"}, {SP_IOB_INTERNAL, "INTERNAL"},
    {SP_IOB_MAPPED, "MAPPED"}, {SP_IOB_SHARED, "SHARED"}, {SP_IOB_LOCKED, "LOCKED"},
    {SP_IOB_PRIVATE, "PRIVATE"}, {SP_IOB_READONLY, "READONLY"}, {SP_IOB_SLICE, "SLICE"},
  };
  for (size_t i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++) {
    int on = i == 0 ? base == NULL : (b->flags & FLAGS[i].bit) != 0;
    if (on) n += (size_t)snprintf(out + n, cap - n, " %s", FLAGS[i].name);
  }
  n += (size_t)snprintf(out + n, cap - n, ">");
  return n;
}

const char *sp_IOBuffer_to_s(sp_IOBuffer *b) {
  SP_GC_ROOT(b);
  char hd[256];
  size_t n = iob_header(b, hd, sizeof hd);
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, hd, n + 1);
  sp_str_set_len(r, n);
  return r;
}

const char *sp_IOBuffer_inspect(sp_IOBuffer *b) {
  SP_GC_ROOT(b);
  char hd[256];
  size_t n = iob_header(b, hd, sizeof hd);
  int64_t shown = b->size < 256 ? b->size : 256;
  if (!iob_base(b) || iob_slice_invalid(b) || shown == 0) {
    char *r0 = sp_str_alloc_raw(n + 1);
    memcpy(r0, hd, n + 1);
    sp_str_set_len(r0, n);
    return r0;
  }
  const char *dump = iob_hexdump(b, 0, shown, 16, 1);
  SP_GC_ROOT_STR(dump);
  size_t dn = sp_str_byte_len(dump);
  char tail[64];
  tail[0] = '\0';
  if (b->size > shown)
    snprintf(tail, sizeof tail, "\n(and %lld more bytes not printed)", (long long)(b->size - shown));
  size_t tn = strlen(tail);
  char *r = sp_str_alloc_raw(n + 1 + dn + tn + 1);
  memcpy(r, hd, n);
  r[n] = '\n';
  memcpy(r + n + 1, dump, dn);
  memcpy(r + n + 1 + dn, tail, tn + 1);
  sp_str_set_len(r, n + 1 + dn + tn);
  return r;
}

/* ---- predicates ---- */

/* a slice's own base pointer is never the null buffer, even when its root
   was freed (CRuby: null? false, valid? false there) */
sp_bool sp_IOBuffer_null_p(sp_IOBuffer *b) { return b->source ? 0 : b->data == NULL; }
sp_bool sp_IOBuffer_empty_p(sp_IOBuffer *b) { return b->size == 0; }
sp_bool sp_IOBuffer_valid_p(sp_IOBuffer *b) { return !iob_slice_invalid(b); }
sp_bool sp_IOBuffer_external_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_EXTERNAL) != 0; }
sp_bool sp_IOBuffer_internal_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_INTERNAL) != 0; }
sp_bool sp_IOBuffer_mapped_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_MAPPED) != 0; }
sp_bool sp_IOBuffer_shared_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_SHARED) != 0; }
sp_bool sp_IOBuffer_locked_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_LOCKED) != 0; }
sp_bool sp_IOBuffer_readonly_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_READONLY) != 0; }
sp_bool sp_IOBuffer_private_p(sp_IOBuffer *b) { return (b->flags & SP_IOB_PRIVATE) != 0; }

/* ---- bitwise ---- */

typedef uint8_t (*iob_bop)(uint8_t, uint8_t);
static uint8_t bop_and(uint8_t a, uint8_t c) { return a & c; }
static uint8_t bop_or(uint8_t a, uint8_t c) { return a | c; }
static uint8_t bop_xor(uint8_t a, uint8_t c) { return a ^ c; }

/* The mask (second operand) TILES: the result is self-sized, and the mask
   repeats over it. An empty mask has nothing to repeat: MaskError. */
static sp_IOBuffer *iob_bitop(sp_IOBuffer *b, sp_RbVal otherv, iob_bop op) {
  sp_IOBuffer *o = iob_other(b, otherv);
  SP_GC_ROOT(b);
  SP_GC_ROOT(o);
  iob_readable(b);
  iob_readable(o);
  if (o->size == 0) sp_raise_cls("IO::Buffer::MaskError", "Zero-length mask given!");
  int64_t n = b->size;
  sp_IOBuffer *r = iob_alloc_obj(b->cls_id);
  if (n > 0) {
    r->data = (uint8_t *)malloc((size_t)n);
    if (!r->data) sp_oom_die();
    const uint8_t *pa = iob_ptr(b), *pb = iob_ptr(o);
    for (int64_t i = 0; i < n; i++) r->data[i] = op(pa[i], pb[i % o->size]);
    r->size = n;
    r->flags = SP_IOB_INTERNAL;
  }
  return r;
}
sp_IOBuffer *sp_IOBuffer_and(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop(b, other, bop_and); }
sp_IOBuffer *sp_IOBuffer_or(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop(b, other, bop_or); }
sp_IOBuffer *sp_IOBuffer_xor(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop(b, other, bop_xor); }
sp_IOBuffer *sp_IOBuffer_not(sp_IOBuffer *b) {
  SP_GC_ROOT(b);
  iob_readable(b);
  sp_IOBuffer *r = iob_alloc_obj(b->cls_id);
  if (b->size > 0 && (b->source ? iob_ptr(b) : b->data)) {
    r->data = (uint8_t *)malloc((size_t)b->size);
    if (!r->data) sp_oom_die();
    const uint8_t *p = iob_base(b);
    for (int64_t i = 0; i < b->size; i++) r->data[i] = (uint8_t)~p[i];
    r->size = b->size;
    r->flags = SP_IOB_INTERNAL;
  }
  return r;
}

static sp_IOBuffer *iob_bitop_ip(sp_IOBuffer *b, sp_RbVal otherv, iob_bop op) {
  sp_IOBuffer *o = iob_other(b, otherv);
  iob_writable(b);
  iob_readable(o);
  if (o->size == 0) sp_raise_cls("IO::Buffer::MaskError", "Zero-length mask given!");
  uint8_t *pa = iob_ptr(b);
  const uint8_t *pb = iob_ptr(o);
  for (int64_t i = 0; i < b->size; i++) pa[i] = op(pa[i], pb[i % o->size]);
  return b;
}
sp_IOBuffer *sp_IOBuffer_and_ip(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop_ip(b, other, bop_and); }
sp_IOBuffer *sp_IOBuffer_or_ip(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop_ip(b, other, bop_or); }
sp_IOBuffer *sp_IOBuffer_xor_ip(sp_IOBuffer *b, sp_RbVal other) { return iob_bitop_ip(b, other, bop_xor); }
sp_IOBuffer *sp_IOBuffer_not_ip(sp_IOBuffer *b) {
  iob_writable(b);
  uint8_t *p = b->size > 0 ? iob_ptr(b) : NULL;
  for (int64_t i = 0; i < b->size; i++) p[i] = (uint8_t)~p[i];
  return b;
}

/* ---- locked { } ---- */

sp_IOBuffer *sp_IOBuffer_lock(sp_IOBuffer *b) {
  if (b->flags & SP_IOB_LOCKED) sp_raise_cls("IO::Buffer::LockedError", "Buffer already locked!");
  b->flags |= SP_IOB_LOCKED;
  return b;
}
sp_IOBuffer *sp_IOBuffer_unlock(sp_IOBuffer *b) {
  b->flags &= ~SP_IOB_LOCKED;
  return b;
}

/* ---- FFI ----
   An IO::Buffer passed to an ffi_func pointer argument hands C its base
   address, as CRuby's rb_io_buffer_get_bytes_for_reading / _for_writing do:
   NULL for a null buffer (freed, or zero-sized), InvalidatedError for a
   slice whose source was freed or shrunk under it, and AccessError for a
   read-only buffer in a slot other than :buffer_in. A nil (a buffer-typed
   local that holds none) is NULL, as it is for :ptr. */
void *sp_IOBuffer_ffi_base(sp_IOBuffer *b, sp_int writing) {
  if (!b) return NULL;
  if (writing) iob_writable(b);
  uint8_t *p = iob_ptr(b);          /* an invalidated slice raises first */
  return b->size > 0 ? p : NULL;    /* a zero-size view is a null buffer too */
}
/* Held across a `blocking: true` call, during which other threads run: the
   buffer, and a slice's source, are locked so that their free or resize
   raises LockedError instead of releasing memory C is still using. Answers
   what it locked (a buffer the program already holds locked stays so), for
   sp_IOBuffer_ffi_release to undo exactly that. */
sp_int sp_IOBuffer_ffi_hold(sp_IOBuffer *b) {
  sp_int m = 0;
  if (!b) return 0;
  if (!(b->flags & SP_IOB_LOCKED)) { b->flags |= SP_IOB_LOCKED; m |= 1; }
  if (b->source && !(b->source->flags & SP_IOB_LOCKED)) { b->source->flags |= SP_IOB_LOCKED; m |= 2; }
  return m;
}
void sp_IOBuffer_ffi_release(sp_IOBuffer *b, sp_int m) {
  if (!b) return;
  if (m & 1) b->flags &= ~SP_IOB_LOCKED;
  if ((m & 2) && b->source) b->source->flags &= ~SP_IOB_LOCKED;
}
/* the same for a boxed argument, which may not be a buffer at all */
sp_int sp_IOBuffer_ffi_hold_v(sp_RbVal v, sp_int cls_id) {
  return v.tag == SP_TAG_OBJ && v.cls_id == cls_id ? sp_IOBuffer_ffi_hold((sp_IOBuffer *)v.v.p) : 0;
}
void sp_IOBuffer_ffi_release_v(sp_RbVal v, sp_int cls_id, sp_int m) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == cls_id) sp_IOBuffer_ffi_release((sp_IOBuffer *)v.v.p, m);
}
/* a boxed pointer argument: an IO::Buffer (class `cls_id`) gives its base,
   anything else its raw pointer as before */
void *sp_IOBuffer_ffi_ptr(sp_RbVal v, sp_int cls_id, sp_int writing) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == cls_id && v.v.p)
    return sp_IOBuffer_ffi_base((sp_IOBuffer *)v.v.p, writing);
  return v.v.p;
}

/* ---- IO integration (#4474) ----
   Spinel's IO is a stdio FILE* (sp_File), so a raw descriptor read has to
   stay coherent with the stdio buffer: bytes stdio already holds for the
   stream are served first (the readpartial / read_nonblock discipline), and
   a raw write flushes what stdio still holds so the bytes do not reorder.
   A read that can block parks the green thread on readiness first, as every
   IO read does, and a write on a pipe or socket parks on writability; the
   Ruby side holds the buffer's lock across the call so a concurrent resize
   or free cannot move the bytes a parked syscall is about to touch. One
   syscall per call, answering its count, 0 at EOF, or -errno, as CRuby. */
static sp_File *iob_io(sp_RbVal io) {
  if (io.tag == SP_TAG_OBJ && io.cls_id == SP_BUILTIN_IO && io.v.p) {
    sp_File *f = (sp_File *)io.v.p;
    if (!f->fp || f->closed) sp_raise_cls("IOError", "closed stream");
    return f;
  }
  sp_raise_cls("TypeError", sp_sprintf("wrong argument type %s (expected IO)", iob_val_name(io)));
}
/* the span [offset, offset+length) of the buffer, length < 0 meaning nil */
static uint8_t *iob_io_span(sp_IOBuffer *b, sp_int *length, sp_int offset, int writing) {
  if (writing) iob_writable(b);
  if (*length < 0) *length = b->size - offset;
  iob_range(b, offset, *length);
  uint8_t *base = iob_ptr(b);
  if (!base) sp_raise_cls("IO::Buffer::AccessError", "Buffer is not allocated!");
  return base + offset;
}
sp_int sp_IOBuffer_read_io(sp_IOBuffer *b, sp_RbVal io, sp_int length, sp_int offset) {
  SP_GC_ROOT(b); SP_GC_ROOT_RBVAL(io);
  sp_File *f = iob_io(io);
  uint8_t *at = iob_io_span(b, &length, offset, 1);
  if (length == 0) return 0;
  /* what stdio already holds is data the stream has delivered: take it
     first, or a raw read would step over it */
  size_t pend = sp_io_stdio_buffered(f->fp);
  if (pend > 0) {
    size_t want = (size_t)length < pend ? (size_t)length : pend;
    return (sp_int)fread(at, 1, want, f->fp);
  }
  sp_io_wait_readable(f);
  at = iob_io_span(b, &length, offset, 1);   /* the park can raise, never move a locked buffer */
  ssize_t n;
  do { n = read(fileno(f->fp), at, (size_t)length); } while (n < 0 && errno == EINTR);
  return n < 0 ? -(sp_int)errno : (sp_int)n;
}
sp_int sp_IOBuffer_write_io(sp_IOBuffer *b, sp_RbVal io, sp_int length, sp_int offset) {
  SP_GC_ROOT(b); SP_GC_ROOT_RBVAL(io);
  sp_File *f = iob_io(io);
  uint8_t *at = iob_io_span(b, &length, offset, 0);
  if (length == 0) return 0;
  /* a buffered write still in stdio would land after this one */
  if (fflush(f->fp) != 0) return -(sp_int)errno;
  int fd = fileno(f->fp);
  for (;;) {
    ssize_t n = write(fd, at, (size_t)length);
    if (n >= 0) return (sp_int)n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) { sp_io_wait_writable(f); continue; }
    return -(sp_int)errno;
  }
}
sp_int sp_IOBuffer_pread_io(sp_IOBuffer *b, sp_RbVal io, sp_int from, sp_int length, sp_int offset) {
  SP_GC_ROOT(b); SP_GC_ROOT_RBVAL(io);
  sp_File *f = iob_io(io);
  if (from < 0) iob_arg("Position can't be negative!");
  uint8_t *at = iob_io_span(b, &length, offset, 1);
  if (length == 0) return 0;
  ssize_t n;
  do { n = pread(fileno(f->fp), at, (size_t)length, (off_t)from); } while (n < 0 && errno == EINTR);
  return n < 0 ? -(sp_int)errno : (sp_int)n;
}
sp_int sp_IOBuffer_pwrite_io(sp_IOBuffer *b, sp_RbVal io, sp_int from, sp_int length, sp_int offset) {
  SP_GC_ROOT(b); SP_GC_ROOT_RBVAL(io);
  sp_File *f = iob_io(io);
  if (from < 0) iob_arg("Position can't be negative!");
  uint8_t *at = iob_io_span(b, &length, offset, 0);
  if (length == 0) return 0;
  if (fflush(f->fp) != 0) return -(sp_int)errno;
  ssize_t n;
  do { n = pwrite(fileno(f->fp), at, (size_t)length, (off_t)from); } while (n < 0 && errno == EINTR);
  return n < 0 ? -(sp_int)errno : (sp_int)n;
}

/* IO::Buffer.map(file, size = nil, offset = 0, flags = READONLY): a view of
   the file's bytes through mmap. The mapping is page-aligned below the
   requested offset and `data` points at the offset within it; PRIVATE maps
   copy-on-write, SHARED (the default) writes through, READONLY maps
   PROT_READ. The finalizer munmaps; a slice keeps its source alive as for
   any buffer; resize is refused (see sp_IOBuffer_resize). */
sp_IOBuffer *sp_IOBuffer_become_map(sp_IOBuffer *b, sp_RbVal io, sp_int size, sp_int offset, sp_int flags) {
  SP_GC_ROOT(b); SP_GC_ROOT_RBVAL(io);
  sp_File *f = iob_io(io);
  if (offset < 0) iob_arg("Offset can't be negative!");
  int fd = fileno(f->fp);
  fflush(f->fp);
  if (size < 0) {
    struct stat st;
    if (fstat(fd, &st) != 0) sp_raise_cls("SystemCallError", sp_sprintf("%s @ IO::Buffer.map", strerror(errno)));
    size = (sp_int)st.st_size - offset;
    if (size < 0) size = 0;
  }
  uint32_t fl = (uint32_t)flags;
  int prot = (fl & SP_IOB_READONLY) ? PROT_READ : (PROT_READ | PROT_WRITE);
  int mflags = (fl & SP_IOB_PRIVATE) ? MAP_PRIVATE : MAP_SHARED;
  iob_release(b);
  b->source = NULL; b->off = 0;
  b->size = size;
  b->flags = SP_IOB_MAPPED | (fl & (SP_IOB_READONLY | SP_IOB_PRIVATE)) | ((fl & SP_IOB_PRIVATE) ? 0 : SP_IOB_SHARED);
  if (size == 0) { b->data = NULL; return b; }
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  off_t page = (off_t)(offset - offset % ps);
  size_t len = (size_t)(size + (offset - page));
  void *m = mmap(NULL, len, prot, mflags, fd, page);
  if (m == MAP_FAILED) {
    int e = errno;
    b->size = 0; b->flags = 0;
    sp_raise_cls(e == EACCES ? "Errno::EACCES" : e == EINVAL ? "Errno::EINVAL" : "SystemCallError",
                 sp_sprintf("%s @ IO::Buffer.map", strerror(e)));
  }
  b->map_base = m;
  b->map_len = len;
  b->data = (uint8_t *)m + (offset - page);
  return b;
}
