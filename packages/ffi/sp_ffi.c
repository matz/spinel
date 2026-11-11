/* sp_ffi.c -- the native half of the bundled `ffi` package.
 *
 * The ffi gem's API (FFI::Library, FFI::Pointer, FFI::Struct, FFI::Function,
 * ...) is Ruby in ffi.rb, the way TruffleRuby carries it. What Ruby cannot do
 * on its own is here, and nothing more: open a library and find a symbol,
 * load and store a C scalar at an address, and make a call -- or accept one --
 * whose signature is only known at run time. The last two are libffi's job,
 * exactly as they are in the gem's own C extension.
 *
 * Every address crosses into Ruby as an Integer. A pointer never becomes a
 * boxed value the collector would try to follow; ownership of foreign memory
 * stays with whoever the Ruby side says owns it (see MemoryPointer in ffi.rb,
 * whose bytes live in a GC string so the collector frees them).
 */
#include "spinel/runtime.h"
#include "sp_sched.h"   /* sp_native_enter / sp_native_leave: no-ops without threads */
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && __has_include(<ffi/ffi.h>)
#include <ffi/ffi.h>
#else
#include <ffi.h>
#endif

/* The last errno a foreign call left, read by FFI.errno / FFI::LastError. */
static SP_TLS int sp_ffi_errno_v;

sp_int sp_ffi_errno(void) { return sp_ffi_errno_v; }
void sp_ffi_set_errno(sp_int e) { sp_ffi_errno_v = (int)e; errno = (int)e; }

/* ---- libraries ---- */

static const char *sp_ffi_dl_err;

/* 0 when the library cannot be opened; the message is left for dlerror().
   An empty path opens the running program itself (FFI::CURRENT_PROCESS). */
sp_int sp_ffi_dlopen(const char *path, sp_int flags) {
  void *h = dlopen((path && *path) ? path : NULL, (int)flags);
  if (!h) { const char *e = dlerror(); sp_ffi_dl_err = e ? e : "dlopen failed"; }
  return (sp_int)(intptr_t)h;
}

const char *sp_ffi_dlerror(void) { return sp_ffi_dl_err ? sp_ffi_dl_err : ""; }

sp_int sp_ffi_dlsym(sp_int handle, const char *name) {
  void *h = (void *)(intptr_t)handle;
  void *p = dlsym(h ? h : RTLD_DEFAULT, name);
  return (sp_int)(intptr_t)p;
}

/* The address an attach_function resolved, for a program that calls the
   function through a pointer of its declared type instead of libffi (the
   compiler's via_fp entries, #11362): the program points the hook at the
   setter of its own pointers before main; with none, nothing is noted. */
void (*sp_ffi_fp_hook)(const char *, const char *, sp_int);
void sp_ffi_note_fp(const char *mod, const char *name, sp_int addr) {
  if (sp_ffi_fp_hook && mod && name) sp_ffi_fp_hook(mod, name, addr);
}

sp_int sp_ffi_dlclose(sp_int handle) {
  return handle ? (sp_int)dlclose((void *)(intptr_t)handle) : 0;
}

sp_int sp_ffi_rtld(sp_int which) {
  switch (which) {
    case 0: return RTLD_LAZY;
    case 1: return RTLD_NOW;
    case 2: return RTLD_GLOBAL;
    case 3: return RTLD_LOCAL;
    case 4: return (sp_int)(intptr_t)RTLD_DEFAULT;    /* dlsym handles, for fiddle */
    case 5: return (sp_int)(intptr_t)RTLD_NEXT;
    default: return 0;
  }
}

/* ---- raw memory ---- */

sp_int sp_ffi_malloc(sp_int n) { return (sp_int)(intptr_t)malloc(n > 0 ? (size_t)n : 1); }
sp_int sp_ffi_calloc(sp_int n) { return (sp_int)(intptr_t)calloc(1, n > 0 ? (size_t)n : 1); }
sp_int sp_ffi_realloc(sp_int p, sp_int n) { return (sp_int)(intptr_t)realloc((void *)(intptr_t)p, n > 0 ? (size_t)n : 1); }
void sp_ffi_free(sp_int p) { free((void *)(intptr_t)p); }
void sp_ffi_memcpy(sp_int dst, sp_int src, sp_int n) {
  if (n > 0) memmove((void *)(intptr_t)dst, (const void *)(intptr_t)src, (size_t)n);
}
void sp_ffi_memset(sp_int dst, sp_int c, sp_int n) {
  if (n > 0) memset((void *)(intptr_t)dst, (int)c, (size_t)n);
}
sp_int sp_ffi_strlen(sp_int p) { return p ? (sp_int)strlen((const char *)(intptr_t)p) : 0; }
sp_int sp_ffi_strnlen(sp_int p, sp_int max) {
  if (!p) return 0;
  const char *s = (const char *)(intptr_t)p;
  const void *z = memchr(s, 0, (size_t)max);
  return z ? (sp_int)((const char *)z - s) : max;
}

/* The address of a String's bytes. The String must stay reachable for as long
   as C uses the address -- the caller holds it in a local across the call, or
   (MemoryPointer) in an ivar for the pointer's whole life. The collector never
   moves an object, so the address is stable while the String lives. */
sp_int sp_ffi_str_addr(const char *s) { return (sp_int)(intptr_t)s; }

/* The most bytes one binary String can carry out through sp_ffi_bin_len,
   an int. A larger count wrapped when it was stored there, and the String
   made from it was shorter than the memory the caller went on to use. CRuby
   has no such bound; past it, the answer is the NoMemoryError a failed
   allocation gives. */
static void sp_ffi_check_bin_len(sp_int n) {
  if (n > INT_MAX) sp_raise_cls("NoMemoryError", "failed to allocate memory");
}

/* `n` bytes at `p` as a binary String, copied (embedded NULs survive). */
const char *sp_ffi_read_bytes(sp_int p, sp_int n) {
  if (n < 0) n = 0;
  sp_ffi_check_bin_len(n);
  sp_ffi_bin_len = (int)n;
  return p ? (const char *)(intptr_t)p : "";
}

void sp_ffi_write_bytes(sp_int p, const char *s, sp_int off, sp_int n) {
  if (n > 0) memcpy((void *)(intptr_t)p, s + off, (size_t)n);
}

/* A fresh, zero-filled binary String of `n` bytes on the GC heap: the backing
   store of a MemoryPointer. The `:cbinstr` return copies sp_ffi_bin_len bytes
   out of the buffer, so a failed calloc cannot answer "" with a length of
   `n`: the copy read `n` bytes from a one-byte literal. It raises
   NoMemoryError, as CRuby's ffi does for a MemoryPointer it cannot allocate
   and as the runtime's own recoverable allocations do (sp_fiber.c), and keeps
   the old buffer. */
const char *sp_ffi_zero_bytes(sp_int n) {
  if (n < 0) n = 0;
  sp_ffi_check_bin_len(n);
  static char *zeros = NULL; static sp_int zcap = 0;
  if (n > zcap) {
    char *grown = calloc(1, (size_t)n + 1);
    if (!grown) sp_raise_cls("NoMemoryError", "failed to allocate memory");
    free(zeros); zeros = grown; zcap = n;
  }
  sp_ffi_bin_len = (int)n;
  return zeros ? zeros : "";
}

/* Scalar loads and stores. The kind code is the same small vocabulary the
   Ruby side keeps in FFI::Type (see K_* in ffi.rb). */
enum { K_VOID = 0, K_I8, K_U8, K_I16, K_U16, K_I32, K_U32, K_I64, K_U64,
       K_F32, K_F64, K_PTR, K_BOOL };

sp_int sp_ffi_get_int(sp_int addr, sp_int kind) {
  const void *p = (const void *)(intptr_t)addr;
  switch (kind) {
    case K_I8:  { int8_t v;   memcpy(&v, p, 1); return v; }
    case K_U8:  { uint8_t v;  memcpy(&v, p, 1); return v; }
    case K_BOOL:{ uint8_t v;  memcpy(&v, p, 1); return v != 0; }
    case K_I16: { int16_t v;  memcpy(&v, p, 2); return v; }
    case K_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
    case K_I32: { int32_t v;  memcpy(&v, p, 4); return v; }
    case K_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
    case K_I64: { int64_t v;  memcpy(&v, p, 8); return v; }
    case K_U64: case K_PTR: { uint64_t v; memcpy(&v, p, 8); return (sp_int)v; }
    default: return 0;
  }
}

void sp_ffi_put_int(sp_int addr, sp_int kind, sp_int val) {
  void *p = (void *)(intptr_t)addr;
  switch (kind) {
    case K_I8: case K_U8: { uint8_t v = (uint8_t)val; memcpy(p, &v, 1); break; }
    case K_BOOL: { uint8_t v = val != 0; memcpy(p, &v, 1); break; }
    case K_I16: case K_U16: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); break; }
    case K_I32: case K_U32: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); break; }
    case K_I64: case K_U64: case K_PTR: { uint64_t v = (uint64_t)val; memcpy(p, &v, 8); break; }
    default: break;
  }
}

double sp_ffi_get_float(sp_int addr, sp_int kind) {
  const void *p = (const void *)(intptr_t)addr;
  if (kind == K_F32) { float f; memcpy(&f, p, 4); return (double)f; }
  double d; memcpy(&d, p, 8); return d;
}

void sp_ffi_put_float(sp_int addr, sp_int kind, double val) {
  void *p = (void *)(intptr_t)addr;
  if (kind == K_F32) { float f = (float)val; memcpy(p, &f, 4); }
  else memcpy(p, &val, 8);
}

/* ---- libffi types ---- */

static ffi_type *sp_ffi_prim(sp_int kind) {
  switch (kind) {
    case K_VOID: return &ffi_type_void;
    case K_I8:   return &ffi_type_sint8;
    case K_U8:   return &ffi_type_uint8;
    case K_BOOL: return &ffi_type_uint8;
    case K_I16:  return &ffi_type_sint16;
    case K_U16:  return &ffi_type_uint16;
    case K_I32:  return &ffi_type_sint32;
    case K_U32:  return &ffi_type_uint32;
    case K_I64:  return &ffi_type_sint64;
    case K_U64:  return &ffi_type_uint64;
    case K_F32:  return &ffi_type_float;
    case K_F64:  return &ffi_type_double;
    case K_PTR:  return &ffi_type_pointer;
    default:     return NULL;
  }
}

sp_int sp_ffi_type_prim(sp_int kind) { return (sp_int)(intptr_t)sp_ffi_prim(kind); }

/* A struct type from `n` element types stored as ffi_type* words at `elems`.
   Types live as long as the program: a Struct layout is a class-level value. */
sp_int sp_ffi_type_struct(sp_int n, sp_int elems) {
  ffi_type *t = calloc(1, sizeof(ffi_type));
  ffi_type **el = calloc((size_t)n + 1, sizeof(ffi_type *));
  if (!t || !el) return 0;
  memcpy(el, (const void *)(intptr_t)elems, sizeof(ffi_type *) * (size_t)n);
  el[n] = NULL;
  t->type = FFI_TYPE_STRUCT;
  t->elements = el;
  /* size/alignment are filled in by libffi the first time the type is laid
     out; do it now so the Ruby side can read them */
  size_t *offs = calloc((size_t)n + 1, sizeof(size_t));
  if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, t, offs) != FFI_OK) { free(offs); return 0; }
  free(offs);
  return (sp_int)(intptr_t)t;
}

sp_int sp_ffi_type_size(sp_int t) { return t ? (sp_int)((ffi_type *)(intptr_t)t)->size : 0; }
sp_int sp_ffi_type_align(sp_int t) { return t ? (sp_int)((ffi_type *)(intptr_t)t)->alignment : 0; }

/* ---- call interfaces ---- */

/* A cif for `nargs` argument types (ffi_type* words at `atypes`) returning
   `rtype`. `nfixed` < nargs makes it a variadic cif with that many fixed
   arguments. 0 when libffi refuses the signature. */
sp_int sp_ffi_cif_new(sp_int rtype, sp_int nargs, sp_int atypes, sp_int nfixed) {
  ffi_cif *cif = calloc(1, sizeof(ffi_cif));
  ffi_type **at = calloc((size_t)(nargs > 0 ? nargs : 1), sizeof(ffi_type *));
  if (!cif || !at) return 0;
  if (nargs > 0) memcpy(at, (const void *)(intptr_t)atypes, sizeof(ffi_type *) * (size_t)nargs);
  ffi_status st;
  if (nfixed >= 0 && nfixed < nargs)
    st = ffi_prep_cif_var(cif, FFI_DEFAULT_ABI, (unsigned)nfixed, (unsigned)nargs,
                          (ffi_type *)(intptr_t)rtype, at);
  else
    st = ffi_prep_cif(cif, FFI_DEFAULT_ABI, (unsigned)nargs, (ffi_type *)(intptr_t)rtype, at);
  if (st != FFI_OK) { free(cif); free(at); return 0; }
  return (sp_int)(intptr_t)cif;
}

/* Call `fn` through `cif`. `avalues` holds nargs words, each the address of
   that argument's value; the result lands at `rvalue`, which must hold at
   least max(sizeof(ffi_arg), return type size) bytes. */
void sp_ffi_call(sp_int cif, sp_int fn, sp_int avalues, sp_int rvalue) {
  errno = sp_ffi_errno_v;
  ffi_call((ffi_cif *)(intptr_t)cif, FFI_FN((void *)(intptr_t)fn),
           (void *)(intptr_t)rvalue, (void **)(intptr_t)avalues);
  sp_ffi_errno_v = errno;
}

/* The same call out of the world (`attach_function ..., blocking: true`): a
   collection another thread starts does not wait for it, as for an ffi_func
   declared blocking (#11350). The arguments are already marshaled into C
   memory; the call touches no Ruby object. */
void sp_ffi_call_blocking(sp_int cif, sp_int fn, sp_int avalues, sp_int rvalue) {
  errno = sp_ffi_errno_v;
  sp_native_enter();
  ffi_call((ffi_cif *)(intptr_t)cif, FFI_FN((void *)(intptr_t)fn),
           (void *)(intptr_t)rvalue, (void **)(intptr_t)avalues);
  int e = errno;
  sp_native_leave();
  sp_ffi_errno_v = e;
}

/* A small integer return comes back widened to a full ffi_arg register. */
sp_int sp_ffi_ret_int(sp_int rvalue, sp_int kind) {
  const void *p = (const void *)(intptr_t)rvalue;
  if (kind == K_I64 || kind == K_U64 || kind == K_PTR) { uint64_t v; memcpy(&v, p, 8); return (sp_int)v; }
  ffi_arg a; memcpy(&a, p, sizeof a);
  switch (kind) {
    case K_I8:   return (int8_t)a;
    case K_U8:   return (uint8_t)a;
    case K_BOOL: return (uint8_t)a != 0;
    case K_I16:  return (int16_t)a;
    case K_U16:  return (uint16_t)a;
    case K_I32:  return (int32_t)a;
    case K_U32:  return (uint32_t)a;
    default:     return (sp_int)a;
  }
}

/* The mirror image, for a closure writing its result back to libffi. */
void sp_ffi_put_ret_int(sp_int rvalue, sp_int kind, sp_int val) {
  void *p = (void *)(intptr_t)rvalue;
  if (kind == K_I64 || kind == K_U64 || kind == K_PTR) { uint64_t v = (uint64_t)val; memcpy(p, &v, 8); return; }
  ffi_arg a;
  switch (kind) {
    case K_I8: case K_I16: case K_I32: a = (ffi_arg)(ffi_sarg)val; break;
    case K_BOOL: a = val != 0; break;
    default: a = (ffi_arg)val; break;
  }
  memcpy(p, &a, sizeof a);
}

/* ---- closures: C calling into Ruby ---- */

/* The one Ruby entry point every closure lands in, installed by ffi.rb:
   dispatcher(closure id, address of the void *args[] array, return slot). */
static void (*sp_ffi_dispatcher)(int64_t, int64_t, int64_t);

void sp_ffi_set_dispatcher(void (*f)(int64_t, int64_t, int64_t)) { sp_ffi_dispatcher = f; }

static void sp_ffi_closure_handler(ffi_cif *cif, void *ret, void **args, void *ud) {
  (void)cif;
  int saved = errno;
  if (sp_ffi_dispatcher)
    sp_ffi_dispatcher((int64_t)(intptr_t)ud, (int64_t)(intptr_t)args, (int64_t)(intptr_t)ret);
  errno = saved;
}

/* A C-callable function pointer that, called, dispatches closure `id`. The
   closure lives for the rest of the program unless freed: C may keep the
   pointer (a registered callback) long after the Ruby call that made it. */
sp_int sp_ffi_closure_new(sp_int cif, sp_int id) {
  void *code = NULL;
  ffi_closure *cl = ffi_closure_alloc(sizeof(ffi_closure), &code);
  if (!cl) return 0;
  if (ffi_prep_closure_loc(cl, (ffi_cif *)(intptr_t)cif, sp_ffi_closure_handler,
                           (void *)(intptr_t)id, code) != FFI_OK) {
    ffi_closure_free(cl);
    return 0;
  }
  return (sp_int)(intptr_t)code;
}

/* Platform facts ffi.rb cannot learn from Ruby alone. */
sp_int sp_ffi_sizeof(sp_int which) {
  switch (which) {
    case 0: return (sp_int)sizeof(void *);
    case 1: return (sp_int)sizeof(long);
    case 2: return (sp_int)sizeof(ffi_arg);
    case 3: return (sp_int)sizeof(size_t);
    case 4: return (sp_int)sizeof(wchar_t);
    default: return 0;
  }
}

sp_int sp_ffi_little_endian(void) { uint16_t v = 1; uint8_t b; memcpy(&b, &v, 1); return b == 1; }
