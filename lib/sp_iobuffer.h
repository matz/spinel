#ifndef SP_IOBUFFER_H
#define SP_IOBUFFER_H
/* IO::Buffer -- a fixed-size byte buffer with typed numeric accessors,
   following CRuby 4.x semantics (lowercase type symbols are little-endian,
   uppercase big-endian; U8/S8 are single bytes). The class surface is bound
   through the native-class DSL in packages/io/io/buffer.rb; the compiler
   splices that file whenever a program references `IO::Buffer` (the same
   implicit-require treatment `Set` gets), so the class is available without
   an explicit require, as in CRuby.

   Memory model: a buffer owns a malloc'd allocation (`data`), freed by the
   GC finalizer. A slice owns nothing: it holds its root buffer in `source`
   (marked by the GC scan hook, so the backing allocation outlives every
   view) and resolves its base pointer through `source->data + off` on every
   access -- a source resize can move the allocation and the views stay
   valid, where CRuby's stored-pointer slices would dangle. */
#include "sp_types.h"
#include "sp_gc.h"
#include "sp_alloc.h"
#include <string.h>

/* Flag bits: the public constants share CRuby's values; SLICE is CRuby's
   internal display-only flag (no public constant). */
#define SP_IOB_EXTERNAL 1u
#define SP_IOB_INTERNAL 2u
#define SP_IOB_MAPPED   4u
#define SP_IOB_SHARED   8u
#define SP_IOB_LOCKED   32u
#define SP_IOB_PRIVATE  64u
#define SP_IOB_READONLY 128u
#define SP_IOB_SLICE    (1u << 16)

typedef struct sp_IOBuffer_s sp_IOBuffer;
struct sp_IOBuffer_s {
  sp_int cls_id;             /* object header: runtime class id, compiler-stamped */
  uint8_t *data;             /* owned allocation; NULL for null buffers and slices */
  sp_IOBuffer *source;       /* slice: the root (non-slice) buffer, GC-marked */
  int64_t off;               /* slice: byte offset into source */
  int64_t size;
  uint32_t flags;
  void *map_base;            /* IO::Buffer.map: the mmap'd region `data` points into
                                (page-aligned), NULL for a malloc'd allocation */
  struct sp_String_s *borrowed_string; /* kept and unlocked by the block owner */
  size_t map_len;            /* ...and its length, for munmap */
};

/* The typed-accessor type enum. Order groups by width; *_BE are the
   uppercase (big-endian) spellings. Mirrored by src/compiler.c's
   comp_iob_sym_type, whose index the compiler's literal-symbol fast path
   emits. */
enum {
  SP_IOB_TY_U8, SP_IOB_TY_S8,
  SP_IOB_TY_u16, SP_IOB_TY_s16, SP_IOB_TY_U16, SP_IOB_TY_S16,
  SP_IOB_TY_u32, SP_IOB_TY_s32, SP_IOB_TY_U32, SP_IOB_TY_S32,
  SP_IOB_TY_u64, SP_IOB_TY_s64, SP_IOB_TY_U64, SP_IOB_TY_S64,
  SP_IOB_TY_f32, SP_IOB_TY_f64, SP_IOB_TY_F32, SP_IOB_TY_F64,
  SP_IOB_TY__COUNT
};

/* constructors (cls_id first: the compiler stamps the assigned class id) */
sp_IOBuffer *sp_IOBuffer_new(sp_int cls_id);
sp_IOBuffer *sp_IOBuffer_new_i(sp_int cls_id, sp_int size);
sp_IOBuffer *sp_IOBuffer_new_if(sp_int cls_id, sp_int size, sp_int flags);
void sp_IOBuffer_fin(void *p);

/* generic (runtime-dispatched type symbol) accessors */
sp_RbVal sp_IOBuffer_get_value(sp_IOBuffer *b, sp_RbVal type, sp_int off);
sp_int sp_IOBuffer_set_value(sp_IOBuffer *b, sp_RbVal type, sp_int off, sp_RbVal val);
sp_IOBuffer *sp_IOBuffer_check_writable(sp_IOBuffer *b);   /* set_values' up-front check */

/* typed fast paths, called by the compiler's literal-symbol lowering */
SP_COLD sp_int sp_IOBuffer_get_i(sp_IOBuffer *b, sp_int ty, sp_int off);
sp_RbVal sp_IOBuffer_get_x(sp_IOBuffer *b, sp_int ty, sp_int off);
SP_COLD double sp_IOBuffer_get_f(sp_IOBuffer *b, sp_int ty, sp_int off);

/* The same reads inline, for the generated program (which includes this
   header when it uses IO::Buffer). `ty` is a literal at every call site, so
   the width, byte order and sign fold away and a read is a bounds check and
   a load. An owned buffer and an in-range offset take that path; a slice, a
   freed or transferred buffer, or a negative or out-of-range offset calls
   sp_IOBuffer_get_i / _get_f, which raise as before. The 64-bit integers
   never come here: they read boxed, through sp_IOBuffer_get_x.
   Width, order and sign follow iob_types in sp_iobuffer.c: the integers
   after U8/S8 come in (u, s, U, S) groups, the floats are (f32, f64, F32,
   F64), and an odd integer code is a signed type. */
static inline int sp_iob_width(sp_int ty) {
  if (ty >= SP_IOB_TY_f32) return ty == SP_IOB_TY_f32 || ty == SP_IOB_TY_F32 ? 4 : 8;
  return ty <= SP_IOB_TY_S8 ? 1 : ty <= SP_IOB_TY_S16 ? 2 : ty <= SP_IOB_TY_S32 ? 4 : 8;
}
static inline int sp_iob_big_endian(sp_int ty) {
  if (ty >= SP_IOB_TY_f32) return ty >= SP_IOB_TY_F32;
  return ty >= SP_IOB_TY_u16 && ((ty - SP_IOB_TY_u16) & 2);
}
/* the value's bytes, or NULL for every case the out-of-line read handles.
   A slice's `data` is always NULL (see the struct), so the `data` test also
   sends every slice to the out-of-line read. */
static inline const uint8_t *sp_iob_inline_ptr(sp_IOBuffer *b, sp_int off, int width) {
  if (SP_LIKELY(b->data && (uint64_t)off < (uint64_t)b->size &&
                (uint64_t)b->size - (uint64_t)off >= (uint64_t)width))
    return b->data + off;
  return NULL;
}
/* the width-byte value at p, in the byte order be names: one unaligned load
   (memcpy) and a byte swap when that order is not the host's, so a cached
   loop's reads are a word each and the vectorizer can see through them. The
   width is always 1, 2, 4 or 8 (sp_iob_width). */
#define SP_IOB_HOST_BE (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static inline uint64_t sp_iob_load(const uint8_t *p, int width, int be) {
  int sw = be != SP_IOB_HOST_BE;
  switch (width) {
  case 1: return p[0];
  case 2: { uint16_t x; memcpy(&x, p, 2); return sw ? __builtin_bswap16(x) : x; }
  case 4: { uint32_t x; memcpy(&x, p, 4); return sw ? __builtin_bswap32(x) : x; }
  default: { uint64_t x; memcpy(&x, p, 8); return sw ? __builtin_bswap64(x) : x; }
  }
}
/* the value at p, its type a literal */
static inline sp_int sp_iob_decode_i(const uint8_t *p, sp_int ty) {
  int w = sp_iob_width(ty);
  uint64_t raw = sp_iob_load(p, w, sp_iob_big_endian(ty));
  if (ty & 1) { int shift = 64 - 8 * w; return (sp_int)((int64_t)(raw << shift) >> shift); }
  return (sp_int)raw;
}
static inline double sp_iob_decode_f(const uint8_t *p, sp_int ty) {
  int w = sp_iob_width(ty);
  uint64_t raw = sp_iob_load(p, w, sp_iob_big_endian(ty));
  if (w == 4) { union { uint32_t u; float f; } u; u.u = (uint32_t)raw; return u.f; }
  union { uint64_t u; double f; } u; u.u = raw; return u.f;
}
static inline sp_int sp_IOBuffer_load_i(sp_IOBuffer *b, sp_int ty, sp_int off) {
  const uint8_t *p = sp_iob_inline_ptr(b, off, sp_iob_width(ty));
  if (SP_UNLIKELY(!p)) return sp_IOBuffer_get_i(b, ty, off);
  return sp_iob_decode_i(p, ty);
}
static inline double sp_IOBuffer_load_f(sp_IOBuffer *b, sp_int ty, sp_int off) {
  const uint8_t *p = sp_iob_inline_ptr(b, off, sp_iob_width(ty));
  if (SP_UNLIKELY(!p)) return sp_IOBuffer_get_f(b, ty, off);
  return sp_iob_decode_f(p, ty);
}
/* IO::Buffer#size, inline for the same program (a slice's is its own) */
static inline sp_int sp_IOBuffer_size_inline(sp_IOBuffer *b) { return (sp_int)b->size; }
/* A loop's cached view of a buffer (the header cache, src/codegen_stmt.c):
   the bytes a read of b sees and how many -- an owned buffer's, or a valid
   slice's within its source. NULL and 0 for nil, a freed or transferred
   buffer and an invalidated slice (iob_slice_invalid), every read of which
   takes sp_IOBuffer_get_i / _get_f and raises as it does uncached. A
   cached set_value writes them too, up to sp_iob_view_wlen. */
static inline uint8_t *sp_iob_view(sp_IOBuffer *b, sp_int *len) {
  *len = 0;
  if (!b) return NULL;
  if (b->data) { *len = b->size; return b->data; }
  if (b->source && b->source->data && b->off <= b->source->size - b->size) {
    *len = b->size;
    return b->source->data + b->off;
  }
  return NULL;
}
/* Does a value of type ty at off lie within a view of len bytes? */
static inline int sp_iob_fits(sp_int len, sp_int off, sp_int ty) {
  return (uint64_t)off < (uint64_t)len && (uint64_t)len - (uint64_t)off >= (uint64_t)sp_iob_width(ty);
}
sp_int sp_IOBuffer_set_i(sp_IOBuffer *b, sp_int ty, sp_int off, sp_int v);
sp_int sp_IOBuffer_set_f(sp_IOBuffer *b, sp_int ty, sp_int off, double v);
sp_int sp_IOBuffer_set_v(sp_IOBuffer *b, sp_int ty, sp_int off, sp_RbVal v);

/* The same writes inline, as the reads above. sp_iob_put_i / _put_f store
   v at off among the d bytes of which wl are writable and answer the offset
   just past it, as CRuby's set_value does (off + width, never 0: off >= 0
   once it fits), or answer 0 when the value does not go there: off outside
   the wl bytes (a read-only buffer has none, nor an invalidated slice), or
   a 32-bit value out of range. The caller then calls sp_IOBuffer_set_i /
   _set_f, which checks in CRuby's order (AccessError, then "Buffer is
   invalid!", before the offset) and raises as before; both answer what
   the out-of-line write answers. The 64-bit integers come here too: any
   sp_int fits them. */
/* the low width bytes of v at p, as sp_iob_load reads them back */
static inline void sp_iob_store(uint8_t *p, int width, int be, uint64_t v) {
  int sw = be != SP_IOB_HOST_BE;
  switch (width) {
  case 1: p[0] = (uint8_t)v; return;
  case 2: { uint16_t x = (uint16_t)v; if (sw) x = __builtin_bswap16(x); memcpy(p, &x, 2); return; }
  case 4: { uint32_t x = (uint32_t)v; if (sw) x = __builtin_bswap32(x); memcpy(p, &x, 4); return; }
  default: { uint64_t x = sw ? __builtin_bswap64(v) : v; memcpy(p, &x, 8); return; }
  }
}
/* an 8/16-bit store wraps and a 64-bit one holds any sp_int; a 32-bit one
   takes [-2^31, 2^31-1] signed, [-2^31, 2^32-1] unsigned */
static inline int sp_iob_holds(sp_int ty, sp_int v) {
  return sp_iob_width(ty) != 4 || ((int64_t)v >= -((int64_t)1 << 31) &&
                                   (int64_t)v <= ((ty & 1) ? ((int64_t)1 << 31) - 1 : ((int64_t)1 << 32) - 1));
}
static inline sp_int sp_iob_put_i(uint8_t *d, sp_int wl, sp_int ty, sp_int off, sp_int v) {
  if (SP_UNLIKELY(!sp_iob_fits(wl, off, ty) || !sp_iob_holds(ty, v))) return 0;
  sp_iob_store(d + off, sp_iob_width(ty), sp_iob_big_endian(ty), (uint64_t)(int64_t)v);
  return off + sp_iob_width(ty);
}
static inline sp_int sp_iob_put_f(uint8_t *d, sp_int wl, sp_int ty, sp_int off, double v) {
  if (SP_UNLIKELY(!sp_iob_fits(wl, off, ty))) return 0;
  uint64_t raw;
  if (sp_iob_width(ty) == 4) { union { uint32_t u; float f; } u; u.f = (float)v; raw = u.u; }
  else { union { uint64_t u; double f; } u; u.f = v; raw = u.u; }
  sp_iob_store(d + off, sp_iob_width(ty), sp_iob_big_endian(ty), raw);
  return off + sp_iob_width(ty);
}
/* how many of a view's len bytes a write may store into: none of a
   read-only buffer's (a slice carries its root's READONLY) */
static inline sp_int sp_iob_view_wlen(sp_IOBuffer *b, sp_int len) {
  return b && !(b->flags & SP_IOB_READONLY) &&
         !(b->source && b->source->borrowed_string) ? len : 0;
}
/* an owned buffer's bytes: a slice's data is NULL (see the struct), so a
   slice takes the out-of-line write */
static inline sp_int sp_IOBuffer_store_i(sp_IOBuffer *b, sp_int ty, sp_int off, sp_int v) {
  sp_int r = sp_iob_put_i(b->data, b->data ? sp_iob_view_wlen(b, b->size) : 0, ty, off, v);
  return SP_LIKELY(r) ? r : sp_IOBuffer_set_i(b, ty, off, v);
}
static inline sp_int sp_IOBuffer_store_f(sp_IOBuffer *b, sp_int ty, sp_int off, double v) {
  sp_int r = sp_iob_put_f(b->data, b->data ? sp_iob_view_wlen(b, b->size) : 0, ty, off, v);
  return SP_LIKELY(r) ? r : sp_IOBuffer_set_f(b, ty, off, v);
}

/* strings */
const char *sp_IOBuffer_get_string0(sp_IOBuffer *b);
const char *sp_IOBuffer_get_string1(sp_IOBuffer *b, sp_int off);
const char *sp_IOBuffer_get_string2(sp_IOBuffer *b, sp_int off, sp_int len);
sp_int sp_IOBuffer_set_string1(sp_IOBuffer *b, const char *s);
sp_int sp_IOBuffer_set_string2(sp_IOBuffer *b, const char *s, sp_int off);
sp_int sp_IOBuffer_set_string3(sp_IOBuffer *b, const char *s, sp_int off, sp_int len);
sp_int sp_IOBuffer_set_string4(sp_IOBuffer *b, const char *s, sp_int off, sp_int len, sp_int soff);

/* whole-buffer operations */
sp_int sp_IOBuffer_size(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_resize(sp_IOBuffer *b, sp_int size);
sp_IOBuffer *sp_IOBuffer_clear0(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_clear1(sp_IOBuffer *b, sp_int v);
sp_IOBuffer *sp_IOBuffer_clear2(sp_IOBuffer *b, sp_int v, sp_int off);
sp_IOBuffer *sp_IOBuffer_clear3(sp_IOBuffer *b, sp_int v, sp_int off, sp_int len);
sp_int sp_IOBuffer_copy1(sp_IOBuffer *b, sp_RbVal src);
sp_int sp_IOBuffer_copy2(sp_IOBuffer *b, sp_RbVal src, sp_int off);
sp_int sp_IOBuffer_copy3(sp_IOBuffer *b, sp_RbVal src, sp_int off, sp_int len);
sp_int sp_IOBuffer_copy4(sp_IOBuffer *b, sp_RbVal src, sp_int off, sp_int len, sp_int soff);
sp_IOBuffer *sp_IOBuffer_slice0(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_slice1(sp_IOBuffer *b, sp_int off);
sp_IOBuffer *sp_IOBuffer_slice2(sp_IOBuffer *b, sp_int off, sp_int len);
sp_IOBuffer *sp_IOBuffer_transfer(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_free_m(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_dup_m(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_become_for(sp_IOBuffer *b, const char *s);
sp_IOBuffer *sp_IOBuffer_borrow_string(sp_IOBuffer *owner, const char *s);
sp_IOBuffer *sp_IOBuffer_end_borrow(sp_IOBuffer *owner);

/* comparison */
sp_int sp_IOBuffer_cmp(sp_IOBuffer *b, sp_RbVal other);
sp_bool sp_IOBuffer_eq(sp_IOBuffer *b, sp_RbVal other);

/* rendering */
const char *sp_IOBuffer_hexdump0(sp_IOBuffer *b);
const char *sp_IOBuffer_hexdump1(sp_IOBuffer *b, sp_int off);
const char *sp_IOBuffer_hexdump2(sp_IOBuffer *b, sp_int off, sp_int len);
const char *sp_IOBuffer_hexdump3(sp_IOBuffer *b, sp_int off, sp_int len, sp_int width);
const char *sp_IOBuffer_inspect(sp_IOBuffer *b);
const char *sp_IOBuffer_to_s(sp_IOBuffer *b);

/* predicates */
sp_bool sp_IOBuffer_null_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_empty_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_valid_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_external_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_internal_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_mapped_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_shared_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_locked_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_readonly_p(sp_IOBuffer *b);
sp_bool sp_IOBuffer_private_p(sp_IOBuffer *b);

/* bitwise */
sp_IOBuffer *sp_IOBuffer_and(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_or(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_xor(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_not(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_and_ip(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_or_ip(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_xor_ip(sp_IOBuffer *b, sp_RbVal other);
sp_IOBuffer *sp_IOBuffer_not_ip(sp_IOBuffer *b);

/* locked { } support (the Ruby-side `locked` wraps these) */
sp_IOBuffer *sp_IOBuffer_lock(sp_IOBuffer *b);
sp_IOBuffer *sp_IOBuffer_unlock(sp_IOBuffer *b);

/* IO integration (#4474): one read(2) / write(2) / pread(2) / pwrite(2)
   against a boxed IO handle, answering the byte count, 0 at EOF, or
   -errno as CRuby does. `length` < 0 stands for nil (the rest of the
   buffer from `offset`). The Ruby side holds the lock around them. */
sp_int sp_IOBuffer_read_io(sp_IOBuffer *b, sp_RbVal io, sp_int length, sp_int offset);
sp_int sp_IOBuffer_write_io(sp_IOBuffer *b, sp_RbVal io, sp_int length, sp_int offset);
sp_int sp_IOBuffer_pread_io(sp_IOBuffer *b, sp_RbVal io, sp_int from, sp_int length, sp_int offset);
sp_int sp_IOBuffer_pwrite_io(sp_IOBuffer *b, sp_RbVal io, sp_int from, sp_int length, sp_int offset);
/* IO::Buffer.map(file, size, offset, flags): the receiver (a fresh null
   buffer) becomes a view of the file's bytes through mmap; `size` < 0
   stands for nil (the file's size from `offset`). */
sp_IOBuffer *sp_IOBuffer_become_map(sp_IOBuffer *b, sp_RbVal io, sp_int size, sp_int offset, sp_int flags);

/* FFI: the base address C receives for a buffer in an ffi_func pointer
   argument; `writing` is 0 only for :buffer_in. The _ptr form takes a boxed
   argument and passes anything but an IO::Buffer through as its pointer. */
void *sp_IOBuffer_ffi_base(sp_IOBuffer *b, sp_int writing);
void *sp_IOBuffer_ffi_ptr(sp_RbVal v, sp_int cls_id, sp_int writing);
sp_int sp_IOBuffer_ffi_hold(sp_IOBuffer *b);
void sp_IOBuffer_ffi_release(sp_IOBuffer *b, sp_int m);
sp_int sp_IOBuffer_ffi_hold_v(sp_RbVal v, sp_int cls_id);
void sp_IOBuffer_ffi_release_v(sp_RbVal v, sp_int cls_id, sp_int m);

/* IO::Buffer::PAGE_SIZE (the mapped-allocation threshold) */
sp_int sp_IOBuffer_page_size(void);

#endif /* SP_IOBUFFER_H */
