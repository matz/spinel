/* Spinel Runtime Library */
#ifndef SP_RUNTIME_H
#define SP_RUNTIME_H

/* Platform feature-test macros (_DARWIN_C_SOURCE for MAP_ANON) live at the top
   of sp_types.h so every translation unit that includes it defines them before
   the first system header. Must precede <stdio.h>. */
#include "sp_types.h"
#include "sp_alloc.h"   /* shared string-heap state + allocators (extern; see sp_alloc.c) */
#include "sp_marshal.h" /* Marshal.dump/load (lib/sp_marshal.c) + the sp_marshal_v vtable */
#include "sp_format.h"  /* cold value-type display helpers (lib/sp_format.c) */
#include "sp_string.h"  /* sp_String builder (hot core inline; cold mutators in lib/sp_string.c) */
#include "sp_inspect.h" /* generic container #inspect (lib/sp_inspect.c) */
#include "sp_array.h"   /* typed arrays: hot core inline + cold ops in lib/sp_array.c */
#include "sp_re.h"      /* regexp wrappers + MatchData (lib/sp_re.c); engine = build/regexp/*.o */
#include "sp_str.h"     /* cold string transforms (lib/sp_str.c); hot/utf8 core stays here */
#include "sp_hash.h"    /* StrInt/StrStr/IntStr/IntInt hash cold ops (lib/sp_hash.c) */
#include "sp_range.h"   /* Range helpers: hot inline core + sp_range_include/str cold (lib/sp_cold.c) */
#include "sp_argf.h"    /* ARGV/ARGF state (extern; defined by main()) + cold ops (lib/sp_cold.c) */
#include "sp_proc.h"    /* sp_Proc/sp_Curry + cold ops (lib/sp_proc.c) */
static const char *sp_method_desc_cstr(sp_BoundMethod *m);
#include "sp_exc.h"     /* sp_Exception + cold ops (lib/sp_exc.c) */
sp_RbVal sp_env_shift(void);
sp_int sp_env_size(void);
sp_StrStrHash *sp_env_to_h(void);
sp_StrStrHash *sp_env_clear(void);
sp_StrStrHash *sp_env_update_h(sp_StrStrHash *h, int replace);
sp_StrIntHash*sp_gc_stat(void);
const char *sp_str_setbyte_cow(const char *s, sp_int i, sp_int v);
#include "sp_random.h"  /* shared Kernel-level PRNG stream (lib/sp_random.c) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/times.h>   /* struct tms for Process.times */

/* Opt-in native backtrace (spinel --debug). In a -g, non-inlined build the
   sp_<method> symbols are present, so sp_raise_cls can snapshot the live C
   stack at raise time and Exception#backtrace / caller format it into a
   Ruby-style backtrace — no per-method shadow frames needed. Off unless the
   generated main() sets sp_bt_enabled (debug builds), so non-debug behaviour
   and cost are unchanged. execinfo is not quite POSIX-ish; it's absent on
   Windows and other platforms. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    define HAVE_EXECINFO_H 1
#  endif
#elif defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#  define HAVE_EXECINFO_H 1
#endif

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#else
/* No execinfo.h: provide no-op shims so the formatting code below compiles
   and links unchanged. backtrace_symbols returns NULL, which the formatter
   treats as "nothing to format" -- the backtrace is simply empty. */
#define backtrace_symbols(buf, n) ((char **)0)
#define backtrace(buf, sz) 0
#endif
#define SP_BT_AVAILABLE 1
extern int sp_bt_enabled;          /* set to 1 by debug-build main(); defined in lib/sp_cold.c */
extern const char *sp_bt_srcfile;  /* toplevel .rb path, set by debug main() */
#if SP_BT_AVAILABLE
static void *sp_bt_buf[256];       /* frames captured at the last raise */
static int sp_bt_n = 0;
#endif
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>   /* PRIO_* selectors for Process.getpriority */
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <malloc.h>
#else
/* Darwin's libc has no malloc_trim; make it a no-op so call sites stay portable. */
#define malloc_trim(x) ((void)0)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* Core value-type definitions (primitives, leaf structs, GC headers,
   typed arrays, non-poly hashes) live in sp_types.h so libspinel_rt.a
   sources can share them without the function bodies below. */
#include "sp_types.h"
/* sp_RbVal, the collector globals/entry points, and the hot inline mark
   helpers; the collector body lives in libspinel_rt.a (lib/sp_gc.c). */
#include "sp_gc.h"
/* sp_Fiber + the Fiber API; the bodies live in libspinel_rt.a (lib/sp_fiber.c). */
#include "sp_fiber.h"
/* sp_thread + the cooperative scheduler (Phase 0); bodies in lib/sp_sched.c. */
#include "sp_sched.h"

/* Every SP_BUILTIN_* cls_id must name exactly one kind. They are declared in
   TWO headers -- sp_alloc.h and sp_gc.h, the latter because the inline mark
   helper has to see FOREIGN_PTR and REGEX -- and neither block can see the
   other, so three pairs had silently landed on the same value: an Addrinfo
   answered String from every tag-keyed switch, a Socket::Option answered
   OpenStruct, and an Enumerator read as a foreign pointer, which the collector
   deliberately does not trace (#4158).

   This is the first point where both blocks are in scope. A switch is the
   cheapest total check C has -- two labels of the same value is a hard error,
   not a warning -- and it costs nothing at run time. Add every new id here. */
static inline void sp_builtin_cls_ids_distinct(int id) {
  switch (id) {
    case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:
    case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_PTR_ARRAY:
    case SP_BUILTIN_SYM_ARRAY: case SP_BUILTIN_PROC:
    case SP_BUILTIN_RANGE: case SP_BUILTIN_TIME:
    case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_STR_INT_HASH:
    case SP_BUILTIN_STR_STR_HASH: case SP_BUILTIN_INT_STR_HASH:
    case SP_BUILTIN_SYM_INT_HASH: case SP_BUILTIN_SYM_STR_HASH:
    case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH:
    case SP_BUILTIN_POLY_POLY_HASH: case SP_BUILTIN_INT_INT_HASH:
    case SP_BUILTIN_OBJECT: case SP_BUILTIN_BASIC_OBJECT:
    case SP_BUILTIN_FIBER: case SP_BUILTIN_IO: case SP_BUILTIN_METHOD:
    case SP_BUILTIN_ENUMERATOR: case SP_BUILTIN_EXCEPTION:
    case SP_BUILTIN_THREAD: case SP_BUILTIN_QUEUE: case SP_BUILTIN_MUTEX:
    case SP_BUILTIN_CONDVAR: case SP_BUILTIN_STRBUF: case SP_BUILTIN_DIR:
    case SP_BUILTIN_TMS: case SP_BUILTIN_ADDRINFO: case SP_BUILTIN_SOCKOPT:
    case SP_BUILTIN_CURRY: case SP_BUILTIN_MATCHDATA:
    case SP_BUILTIN_FOREIGN_PTR: case SP_BUILTIN_REGEX:
    case SP_BUILTIN_COMPLEX: case SP_BUILTIN_RATIONAL:
    case SP_BUILTIN_BIG_RATIONAL: case SP_BUILTIN_FLOAT_RANGE:
    case SP_BUILTIN_STR_RANGE: case SP_BUILTIN_OPENSTRUCT:
    case SP_BUILTIN_PROCESS_STATUS:   /* agentwm/dvtm: Process.waitpid2's boxed
                                          return value, dispatches signaled?/exited?/
                                          termsig/... via runtime type tag. */
    default: break;
  }
}
#if defined(SPINEL_EXT_HOST) || defined(SPINEL_EXT_KERNEL)
const char *sp_sym_to_s(sp_sym id);
#else
static const char *sp_sym_to_s(sp_sym id);
#endif
/* Capacity of the runtime symbol-intern pool the generated TU declares
   (sp_dyn_syms). 8 bytes/entry, so the default is a 64 KB static buffer holding
   symbols minted at runtime (String#to_sym, :"#{interp}"). Embedded targets that
   intern few or no symbols at runtime can shrink it with -DSP_DYN_SYMS_MAX=<n>. */
#ifndef SP_DYN_SYMS_MAX
#define SP_DYN_SYMS_MAX 8192
#endif

/* sp_raise_cls forward decl — defined later in this header (line ~1017).
   Used by the integer-division helpers below to match CRuby semantics:
   `a / 0`, `a % 0`, `a.divmod(0)`, `a.ceildiv(0)`, and `a.pow(e, 0)` all
   raise ZeroDivisionError instead of triggering C undefined behaviour
   (SIGFPE on x86) or silently returning 0. */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);

/* The unresolved-call gate (codegen_call.c) raises NoMethodError through this
   single recognizable token under SPINEL_GATE_RAISE, so coercion sites can
   detect and coerce it (sp_poly_to_i(sp_raise_nomethod(...)) etc.) rather than
   parse a comma-expression. Returns sp_RbVal so it composes in a poly slot;
   NORETURN, so the value is never produced. Unused when the gate stays silent. */
/* Deliberately an extern, NOT declared noreturn (lib/sp_core.c): gate arms
   sit inside hot dispatch functions (PPU update_scroll_address_line), and a
   noreturn call there restructures the CFG -- blocks reorder and optcarrot
   pays ~5-7% fps. As a plain value-returning extern call the arm keeps the
   exact shape of the sp_box_nil() it replaced; the raise still never
   returns at runtime (sp_raise_cls longjmps). */
sp_RbVal sp_raise_nomethod(const char *msg);

/* An int slot holds SP_INT_NIL when a container read missed, and that value is
   nil, not an Integer: arithmetic on it raises the way CRuby's nil does
   (NoMethodError for nil on the left, the coercion TypeError on the right)
   instead of computing on INTPTR_MIN. */
SP_NORETURN void sp_raise_nil_int_op(sp_int a, sp_int b, const char *op);
#define SP_INT_NIL_CK(a, b, op) \
  if (SP_UNLIKELY((a) == SP_INT_NIL || (b) == SP_INT_NIL)) sp_raise_nil_int_op((a), (b), op)

static inline sp_int sp_idiv(sp_int a, sp_int b) {
  SP_INT_NIL_CK(a, b, "/");
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  sp_int q = a / b; sp_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) q--;
  return q;
}
/* Integer#abs: nil-checked (and free of the -INTPTR_MIN overflow the inline
   ternary had on the sentinel). */
static inline sp_int sp_int_abs(sp_int a) {
  SP_INT_NIL_CK(a, (sp_int)0, "abs");
  return a < 0 ? -a : a;
}
static inline sp_int sp_imod(sp_int a, sp_int b) {
  SP_INT_NIL_CK(a, b, "%");
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  sp_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) r += b;
  return r;
}
/* Float#% (and Integer % Float): floored modulo whose result takes the sign of
   the divisor, unlike C fmod which follows the dividend (-5.5 % 2 == 0.5). */
/* float %% with an INTEGER zero divisor raises in CRuby (5.0 %% 0), while a
   float zero divisor yields NaN (5.0 %% 0.0) -- the int-divisor emit routes
   here so the check costs nothing on the float-divisor path. */
static inline double sp_fmod_intdiv(double a, sp_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  double r = fmod(a, (double)b);
  if (r != 0 && ((r < 0) != (b < 0))) r += (double)b;
  return r;
}
static inline double sp_fmod(double a, double b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");  /* 5.0 % 0.0 raises in CRuby */
  double r = fmod(a, b);
  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
  return r;
}
/* Float#remainder: plain C fmod, but a zero divisor raises the way every other
   Float division-derived operation does (#3649). */
static inline double sp_fremainder(double a, double b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return fmod(a, b);
}
/* Integer#remainder: truncated remainder (sign follows the dividend, i.e. plain
   C `%`), unlike the floored sp_imod. Zero divisor raises like sp_imod/sp_idiv. */
static inline sp_int sp_iremainder(sp_int a, sp_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return a % b;
}
/* Overflow-checked integer arithmetic (BIGINT.md option β: raise on
   overflow, keep locals at native sp_int width).

   Build modes (preprocessor toggles):
     default                 : overflow raises RangeError
     SP_NO_OVERFLOW_CHECK    : bare `+ - *`, silent wrap (UB on signed
                               overflow, but matches spinel's prior
                               behaviour where the user asks for
                               max speed and accepts the risk)

   The actual overflow check is wrapped through
   `sp_int_*_overflow_p` helpers so non-gcc/clang compilers fall back
   to a portable unsigned-arithmetic implementation. The shape
   mirrors mruby's mrb_int_*_overflow in include/mruby/numeric.h.

   The outer sp_int_add/sub/mul/neg are GCC statement-expression
   macros (`({ ... })`) rather than functions: an earlier inline-
   function variant produced wrong optcarrot output (checksum
   diverged 59662→4096) even with __attribute__((always_inline)),
   apparently from a UB-assumption interaction between the
   optimizer and the function-call-shaped expression that the
   macro form bypasses by keeping every operand in its surrounding
   expression context. The macros also let the compiler fold
   constant-operand cases at every call site. */

/* Three modes selected by `--int-overflow=raise|wrap|promote` on the
   spinel wrapper, which passes -DSP_INT_OVERFLOW_MODE_{RAISE,WRAP,
   PROMOTE}. WRAP skips the check entirely. PROMOTE still raises at
   this layer -- the actual promotion semantics is implemented in the
   analyzer by rewriting every int local as bigint, so the helpers
   below are only reached on the few residual sites that stay int
   even in promote mode (FFI argument coercion, etc.). */
#ifdef SP_INT_OVERFLOW_MODE_WRAP
#  define sp_int_add(a, b) ((a) + (b))
#  define sp_int_sub(a, b) ((a) - (b))
#  define sp_int_mul(a, b) ((a) * (b))
#  define sp_int_neg(a)    (-(a))
#else
#  define sp_int_add(a, b) ({ sp_int _sp_a = (a), _sp_b = (b), _sp_r; \
    SP_INT_NIL_CK(_sp_a, _sp_b, "+"); \
    if (sp_int_add_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in +"); \
    _sp_r; })
#  define sp_int_sub(a, b) ({ sp_int _sp_a = (a), _sp_b = (b), _sp_r; \
    SP_INT_NIL_CK(_sp_a, _sp_b, "-"); \
    if (sp_int_sub_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -"); \
    _sp_r; })
#  define sp_int_mul(a, b) ({ sp_int _sp_a = (a), _sp_b = (b), _sp_r; \
    SP_INT_NIL_CK(_sp_a, _sp_b, "*"); \
    if (sp_int_mul_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in *"); \
    _sp_r; })
#  define sp_int_neg(a)    ({ sp_int _sp_a = (a), _sp_r; \
    if (sp_int_sub_overflow_p((sp_int)0, _sp_a, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -@"); \
    _sp_r; })
#endif

/* sp_gcd / sp_lcm / sp_powmod / sp_ceildiv / sp_int_clamp / sp_int_sqrt
   now live in libspinel_rt.a (lib/sp_core.c); declared via sp_core.h. */
static inline char *sp_str_alloc_raw(size_t total_with_null);  /* fwd decl */
const char *sp_sprintf(const char *fmt, ...);                  /* fwd decl */
/* sp_ipow10 / sp_int_round / sp_int_ceil / sp_int_floor /
   sp_int_truncate / sp_str_oct now live in libspinel_rt.a
   (lib/sp_core.c); declared via sp_core.h. */
/* Narrow a foreign 64-bit value (time_t / off_t, etc.) to a Ruby
   Integer. On 64-bit sp_int this is the identity. On 32-bit, a value
   outside the sp_int range can't be represented; rather than silently
   truncating a clock/size value the program never computed (which
   `int-overflow=wrap` is NOT meant to license -- wrap is about the
   user's own arithmetic), raise RangeError. promote-mode bigint
   promotion of these boundary values is a follow-up. */
static inline sp_int sp_i64_to_int(int64_t v){
#if INTPTR_MAX == INT64_MAX
  return (sp_int)v;
#else
  if(v < (int64_t)INTPTR_MIN || v > (int64_t)INTPTR_MAX)
    sp_raise_cls("RangeError","value out of range for 32-bit Integer");
  return (sp_int)v;
#endif
}

/* Forward decls for helpers used across this header (and by the
   string->number parsers that now live in libspinel_rt.a). */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);
const char *sp_sprintf(const char *fmt, ...);

/* String -> number parsers now live in libspinel_rt.a (lib/sp_core.c). */
#include "sp_core.h"
/* system()/backtick ($?) support now lives in libspinel_rt.a (lib/sp_system.c). */
#include "sp_system.h"

/* Math.* wrappers that raise Math::DomainError on out-of-domain input, per
   CRuby. The runtime exception name is the flattened "Math::DomainError"
   (the codegen maps a `rescue Math::DomainError` path to that form, matching
   the StringScanner::Error -> StringScanner_Error precedent). Only the
   domain-restricted methods get wrappers; cos/sin/tan/atan/sinh/cosh/tanh/
   asinh/exp/cbrt/erf/erfc/atan2/hypot accept all reals and call libc
   directly from codegen. log(0) is -Infinity in CRuby (no raise); only a
   negative argument raises. atanh's endpoints (|x| == 1) yield ±Infinity and
   do NOT raise in CRuby -- only |x| > 1 is out of domain. CRuby's Math.*
   message names the function WITHOUT quotes (e.g. `... out of domain - sqrt`),
   unlike Integer.sqrt which quotes "isqrt". */
static inline sp_float sp_math_sqrt(sp_float x){if(x<0.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - sqrt");return x==0.0?0.0:sqrt(x);}  /* normalize sqrt(-0.0) to +0.0 like CRuby */
static inline sp_float sp_math_log(sp_float x){if(x<0.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - log");return log(x);}
static inline sp_float sp_math_log2(sp_float x){if(x<0.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - log2");return log2(x);}
static inline sp_float sp_math_log10(sp_float x){if(x<0.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - log10");return log10(x);}
static inline sp_float sp_math_acos(sp_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - acos");return acos(x);}
static inline sp_float sp_math_asin(sp_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - asin");return asin(x);}
static inline sp_float sp_math_acosh(sp_float x){if(x<1.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - acosh");return acosh(x);}
static inline sp_float sp_math_atanh(sp_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math::DomainError","Numerical argument is out of domain - atanh");return atanh(x);}
/* gamma has poles at the non-positive integers, but CRuby only raises at the
   NEGATIVE integers (gamma(0.0) is +Infinity, gamma(-0.0) -Infinity, which
   tgamma already yields); negative non-integers are in-domain. */
static inline sp_float sp_math_gamma(sp_float x){if(x<0.0&&x==floor(x))sp_raise_cls("Math::DomainError","Numerical argument is out of domain - gamma");return tgamma(x);}


/* ---- Class object ----
   Value-type Class reference: a single class id that indexes into
   the per-program sp_class_names[] table emitted by codegen. Lets
   `c = Foo` produce a runtime value (`(sp_Class){<id>}`) instead of
   a bare C identifier, and `c.to_s` lower to a names-table lookup.
   Other Class methods (`.name`, `.inspect`, `.==`, `.!=`,
   `.superclass`, `.ancestors`, dynamic `is_a?(c)` against a
   variable, etc.) are not yet supported. */

/* ---- Complex runtime ---- */
/* Value-type Cartesian Complex: 16 bytes, passed by value. Used by
   optcarrot's nestopia palette generator; the palette is precomputed
   in the default code path so this is exercised only with
   `--nestopia-palette`. */
/* Complex arithmetic + inspect/to_s moved to lib/sp_format.c (cold; optcarrot
   touches Complex only under --nestopia-palette). */

/* ---- Rational runtime ---- */
/* Value-type Rational: 16 bytes (two sp_ints), passed by value.
   Stored in reduced form -- the parser hands us the already-reduced
   numerator/denominator from the literal; Integer#quo / arithmetic
   normalizes via sp_rational_reduce. Issue #841. */
/* Rational construction + arithmetic + inspect/to_s moved to lib/sp_format.c
   (cold; only reached when a program actually uses Rational). */

/* ---- Time runtime ---- */
/* sp_Time and the libc-backed accessors / formatters live in
   lib/sp_time.{c,h} (compiled into libspinel_rt.a). What stays here
   are the GC-aware wrappers — sp_box_time copies the value onto the
   GC heap, and the *_gc string forwarders allocate a small stack buf,
   call the libspinel_rt format helper, then sp_str_dup_external the
   result into the GC string heap. is_utc distinguishes UTC-coerced
   times from local-zone times; the underlying epoch is the same. */
#include "sp_time.h"

/* sp_ProcessStatus is a small heap-allocated struct (pid + raw status
   word) boxed for `result[1].signaled?` dispatch. The codegen
   dereferences it in the poly-receiver path, so the layout must be
   visible to every program. */
#include "sp_process_status.h"
/* Process.spawn / Process.waitpid2 live in lib/sp_process.c, which cannot
   include this header (see the note there). Declare them here rather than
   emitting the prototypes into every generated program: neither returns int,
   so an implicit declaration mistypes the result, and a program that never
   spawns should not carry two lines about it. */
sp_int sp_process_spawn(sp_RbVal cmd, sp_RbVal args, sp_RbVal opts);
sp_PolyArray *sp_process_waitpid2(sp_int pid);


/* `recycle`: optional sweep hook. If non-NULL, sp_gc_collect calls
   recycle(h) on the unmarked object instead of finalize+free. The
   hook is responsible for deciding whether to keep the storage
   (pool push) or free it. Used by class-instance free-list pools. */
/* sp_gc_heap / sp_gc_bytes are defined in lib/sp_gc.c (extern via sp_gc.h). */
/* sp_gc_threshold moved to sp_alloc.c (extern, shared) */

/* ---- GC verify: opt-in mark-path validation (release-neutral) ------------
 * SPINEL_GC_VERIFY=1  before the collector invokes an object's scan hook,
 *   check that the object is a currently-registered heap allocation. If a
 *   raw/aliased pointer (e.g. into a string or builder buffer) has been put
 *   on the mark path, abort with a diagnostic at that point instead of
 *   calling through a bogus scan-function pointer (which otherwise faults at
 *   a garbage address and is near-impossible to attribute).
 * Default OFF; with it unset, behavior and codegen are unchanged. */
/* GC verify state + helpers live in lib/sp_gc.c. */

/* ---- String GC ---- */
/* The string heap state (sp_str_heap, sp_str_heap_bytes, sp_str_threshold...),
   the allocators (sp_str_alloc / _raw / byte_len / set_len / from_bytes /
   dup_external), sp_str_empty, and the UTF-8 length cache now live in
   sp_alloc.h / sp_alloc.c, shared (extern) so standalone lib C files can
   allocate onto the same heap. sp_str_sweep moved to sp_alloc.c. */

/* RUBY_PLATFORM string -- host arch + OS. Detected at C compile time
   so cross-builds report the target platform. Issue #890. */
#if defined(__x86_64__) || defined(_M_X64)
#  define SP_RUBY_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define SP_RUBY_ARCH "aarch64"
#elif defined(__i386__) || defined(_M_IX86)
#  define SP_RUBY_ARCH "i686"
#elif defined(__arm__)
#  define SP_RUBY_ARCH "arm"
#else
#  define SP_RUBY_ARCH "unknown"
#endif
#if defined(__linux__)
#  define SP_RUBY_OS "linux"
#elif defined(__APPLE__)
#  define SP_RUBY_OS "darwin"
#elif defined(__FreeBSD__)
#  define SP_RUBY_OS "freebsd"
#else
#  define SP_RUBY_OS "unknown"
#endif
static const char sp_ruby_platform_data[] = "\xff" SP_RUBY_ARCH "-" SP_RUBY_OS;
static inline const char *sp_ruby_platform_str(void) { return sp_ruby_platform_data + 1; }

/* Process.ppid wrapper. */
static inline sp_int sp_process_ppid(void) {
  return (sp_int)getppid();
}

static inline double sp_process_clock_gettime(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
#else
  return 0.0;
#endif
}

/* Nanoseconds from the given clock id, for Process.clock_gettime with an
   integer unit (the caller scales to us/ms/s or a float). */
static inline int64_t sp_process_clock_ns(sp_int clock_id) {
  struct timespec ts;
  if (clock_gettime((clockid_t)clock_id, &ts) != 0) ts.tv_sec = ts.tv_nsec = 0;
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* Clock resolution in nanoseconds, for Process.clock_getres (#3045). */
static inline int64_t sp_process_clock_res_ns(sp_int clock_id) {
  struct timespec ts;
  if (clock_getres((clockid_t)clock_id, &ts) != 0) ts.tv_sec = ts.tv_nsec = 0;
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ---- Encoding runtime ----
   Spinel currently assumes UTF-8 source and string data. Keep Encoding
   as a tiny value type so `__ENCODING__` and `String#encoding` can
   answer Ruby's Encoding-shaped protocol without carrying full
   transcoding state. */
/* The name is a spinel rodata-literal string (0xff marker prefix), not a bare
   C literal: it flows into sp_str_hash / sp_str_byte_len etc. as a hash key
   and String value, all of which read the marker byte at s[-1]. A bare literal
   has no controlled preceding byte, so that read is a global-buffer-overflow
   (ASAN-caught) and could spuriously hit the heap-header cache path (#282). */
static inline sp_Encoding sp_encoding_utf8(void){return(sp_Encoding){&("\xff" "UTF-8")[1]};}
static inline sp_Encoding sp_encoding_us_ascii(void){return(sp_Encoding){&("\xff" "US-ASCII")[1]};}
static inline sp_Encoding sp_encoding_binary(void){return(sp_Encoding){&("\xff" "ASCII-8BIT")[1]};}

/* sp_str_alloc / _raw / byte_len / set_len / from_bytes / dup_external moved to
   sp_alloc.h (shared inline over extern heap state). */

/* ---- UTF-8 helpers (used throughout the string runtime below) ---- */
/* Bytes to advance past the codepoint at p (caller guarantees *p != 0).
   Caps at NUL and validates the continuation-byte pattern, so malformed or
   truncated UTF-8 never advances past the terminator. */
/* Direct-mapped pointer-keyed cache for (byte_len, char_len). Populated lazily
   by sp_str_length / sp_utf8_byte_offset; the same entries unlock both calls
   so iterating a single string with `s.length` + `s[i]` walks UTF-8 once
   instead of per access. ASCII strings (char_len == byte_len) take an O(1)
   byte_offset fast path. Cleared by sp_str_sweep so freed-and-reused heap
   addresses can't leak stale entries. Skipped for sp_String wrappers (marker
   0xfd) whose buffers can move on realloc. */
/* SP_STR_LCACHE_*, sp_str_lcache, and sp_str_lcache_clear live in sp_alloc.h /
   sp_alloc.c (shared so the archive-side sweep flushes this TU's cache). */

/* Count UTF-8 code points in s[0..bl). The 8-byte ASCII-detect prologue skips
   bytes 8 at a time while the high bit stays clear, so pure-ASCII strings (the
   common case in the JSON / CSV / template benchmarks) run vastly faster than
   the per-byte advance loop they used to fall through. */

/* True when `s` carries one of spinel's own string markers in the
   preceding byte (0xfe / 0xfc heap, 0xff rodata literal). FFI returns
   a bare `const char *` whose preceding byte is whatever C variable
   sits before the buffer in memory — using the pointer as a cache
   key without this gate aliased subsequent FFI calls into the prior
   call's cached length (#611). 0xfd (sp_String wrapper) is excluded
   too because its buffer can move on append. */

/* Issue #762: check malloc/realloc returns. On OOM, return an empty
   array rather than dereferencing NULL. */

/* Issue #858: expand `a-z` range notation in a String#delete /
   String#tr / String#count character set. `^abc` negation is
   NOT handled (separate v1 scope). Result is a malloc'd flat
   codepoint array — caller frees. */

/* sp_mark_string is an inline helper in sp_gc.h. sp_str_sweep moved to
   sp_alloc.c (single definition, registered with the GC there). */

/* Time formatters (strftime / iso8601 / zone / inspect_v) and the cold
   value ops (cmp / add_f / add_i / sub_i / sub_t) live in lib/sp_time.c;
   the formatters now return GC-heap strings directly, so no trampoline
   is needed here. */

/* SP_GC_STACK_MAX, sp_gc_roots, sp_gc_nroots come from sp_gc.h / lib/sp_gc.c. */
/* Cooperative-fiber GC root storage (issue #636).
   sp_gc_roots[] holds the CURRENT fiber's active roots. When a fiber
   yields, its roots get copied out to the fiber's saved_roots buffer
   and the resuming fiber's saved_roots are copied back in — so the
   per-fiber stacks never clobber each other through interleaved
   pushes the way they did when a single global stack was shared by
   every fiber. sp_gc_mark_all calls the hook below (installed once
   the Fiber section's setup runs) to walk every live fiber's
   saved_roots in addition to the current view. */
/* SP_GC_ROOT / SP_GC_SAVE / SP_GC_RESTORE and the _sp_gc_root_push/pop helpers
   moved to sp_gc.h (shared so lib/sp_marshal.c can root its in-flight objects).
   sp_re_mark_globals is defined below (with the regex globals it marks) and
   carries external linkage so the collector body can reach it. */
#define SP_GC_MARK_STACK_MAX (1024*64)
#define SP_GC_NBUCKETS 32
static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];
static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}
/* sp_gc_cycle / sp_gc_old_bytes are in lib/sp_gc.c (extern via sp_gc.h);
   sp_gc_old_heap is collector-private to lib/sp_gc.c. */

/* GC verify support + sp_gc_collect live in lib/sp_gc.c. */
/* sp_gc_threshold_init moved to sp_alloc.c */
/* sp_oom_die + the SPINEL_MAX_HEAP_MB governor (sp_gc_enforce_mem_limit)
   live in lib/sp_gc.c. */
/* SPINEL_GC_STRESS=1: shrink the collection threshold to a few KB so a
   cycle runs at almost every allocation. A rooting hole that normal
   thresholds hide (the GC rarely lands inside the vulnerable window)
   becomes a deterministic failure; pair with SPINEL_GC_VERIFY=1. */
/* sp_gc_stress_checked moved to sp_alloc.c */
/* sp_gc_alloc / sp_gc_alloc_nogc moved to sp_alloc.h (shared inline over the
   extern heap + threshold state). */
/* GC-header frozen bit — used for containers whose mutators are NOT
   on a hot path (hashes), so the extra cache line vs. a struct field
   doesn't matter. Arrays co-locate `frozen` in the struct instead
   (see sp_IntArray); strings use the 0xff marker / wrapper bit. */
static inline sp_bool sp_gc_is_frozen(void *p) { if (!p) return FALSE; return ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen; }
static inline void *sp_gc_freeze(void *p) { if (p) ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen = 1; return p; }
/* `Queue#freeze` raises rather than freezing: a frozen queue could never be
   pushed to again, so Ruby refuses it outright. Names the receiver the way
   CRuby's message does. */
SP_NORETURN SP_COLD void sp_raise_cannot_freeze(const char *cls, void *p);
/* marker-prefixed message: see sp_raise_frozen_array (lib/sp_alloc.h) */
static void __attribute__((noinline,cold)) sp_raise_frozen_hash(void){sp_raise_cls("FrozenError",(&("\xff" "can't modify frozen Hash")[1]));}
/* Pool-aware alloc. The recycle hook is stored in the gc_hdr; sweep
   calls it on unmarked objects instead of finalize+free. The hook
   decides whether to push the storage onto a per-class free-list or
   actually free it. */
static void *sp_gc_alloc_pool(size_t sz, void(*scn)(void*), void(*recycle)(sp_gc_hdr*)) {
  void *p = sp_gc_alloc(sz, NULL, scn);
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr));
  h->recycle = recycle;
  return p;
}
/* Re-link a previously-pooled slot back into sp_gc_heap so the next
   GC cycle visits it. Called by class _new functions when reusing
   a pooled instance. The storage was kept alive by the pool
   free-list since the last sweep unlinked it from sp_gc_heap. */
/* The heap list is shared across workers; a pooled re-link used to push onto
   it bare, racing sp_gc_alloc's push at N>1 (a lost link is a leaked-then-UAF
   header). SP_GC_HEAP_PUSH is a lock-free CAS push in the threaded build --
   the pool hit stays off the heap mutex -- and the exact plain push in the
   single-threaded one (see sp_gc.h). */
static void sp_gc_pool_relink(sp_gc_hdr *h) {
  h->marked = 0; h->old = 0; h->dirty = 0;
  SP_GC_HEAP_PUSH(h);
  sp_gc_bytes_add(h->size);
}

/* Per-class free-list pool boilerplate. SP_POOL_DEFINE(CLS) goes at
   file scope, near the class _new function. SP_POOL_NEW(CLS, scan)
   replaces the body of an `sp_gc_alloc(sizeof(sp_CLS), NULL, scan)`
   call, popping from the per-class free-list if non-empty.
   Default cap can be overridden at runtime via SP_POOL_MAX envvar
   (uniform across classes). SP_POOL_REPORT=1 dumps per-class stats
   at exit. */
#define SP_POOL_DEFAULT_MAX 1048576L
/* Pool concurrency (threaded build). The free lists stay SHARED across
   workers. Pushes (recycle) happen only during the stop-the-world sweep,
   where every mutator is parked -- but the sweep itself is PARALLEL
   (sp_sched_par_sweep: every parked worker sweeps its own slot), so several
   workers push onto one list at once, and a push is a CAS loop with atomic
   counters, like a pop. Pops run concurrently on any worker through the
   lock-free CAS below. Pushes and pops never overlap each other: a popped
   node can only reappear on the list via the sweep, and no sweep can run
   while a mutator is mid-pop (it is not at a safepoint), so the Treiber
   stack has no ABA window in either direction. A shared list also keeps
   recycled storage visible to every worker -- per-worker (TLS) lists would
   strand the whole recycle crop on whichever worker ran the collection.
   Single-threaded: the macros expand to the exact plain code this had. */
#ifdef SP_THREADS
static inline sp_gc_hdr *sp_pool_try_pop(sp_gc_hdr **head) {
  sp_gc_hdr *old = __atomic_load_n(head, __ATOMIC_ACQUIRE);
  while (old) {
    /* A stale `old` may already belong to another worker, which is
       concurrently rewriting old->next (its relink push). The atomic load
       keeps that defined; the CAS below then fails and retries with a
       fresh head, discarding the stale next. */
    sp_gc_hdr *nxt = __atomic_load_n(&old->next, __ATOMIC_RELAXED);
    if (__atomic_compare_exchange_n(head, &old, nxt, 1,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) break;
  }
  return old;
}
#define SP_POOL_CTR_INC(c) __atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
#define SP_POOL_CTR_DEC(c) __atomic_fetch_sub(&(c), 1, __ATOMIC_RELAXED)
/* The cap is reserved before the push, not checked beside it: a load-then-add
   lets every concurrent sweeper see room and all of them push, overshooting
   pool_max by up to their number. Add first; over the cap, give the slot back
   and free the storage. The count rises and falls by one per over-cap sweeper
   in between, which nothing reads for anything but the same cap. */
#define SP_POOL_RECYCLE_BODY(CLS, h) \
    long _c = __atomic_add_fetch(&sp_##CLS##_pool_count, 1, __ATOMIC_RELAXED); \
    if (_c > sp_##CLS##_pool_max) { \
      __atomic_fetch_sub(&sp_##CLS##_pool_count, 1, __ATOMIC_RELAXED); \
      free(h); __atomic_fetch_add(&sp_##CLS##_pool_freed, 1, __ATOMIC_RELAXED); return; \
    } \
    { sp_gc_hdr *_old; \
      do { _old = __atomic_load_n(&sp_##CLS##_pool_head, __ATOMIC_ACQUIRE); (h)->next = _old; \
      } while (!__atomic_compare_exchange_n(&sp_##CLS##_pool_head, &_old, (h), \
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)); } \
    { __atomic_fetch_add(&sp_##CLS##_pool_pushes, 1, __ATOMIC_RELAXED); \
      long _hwm = __atomic_load_n(&sp_##CLS##_pool_hwm, __ATOMIC_RELAXED); \
      while (_c > _hwm && !__atomic_compare_exchange_n(&sp_##CLS##_pool_hwm, &_hwm, _c, \
                                                       1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {} }
#else
static inline sp_gc_hdr *sp_pool_try_pop(sp_gc_hdr **head) {
  sp_gc_hdr *old = *head;
  if (old) *head = old->next;
  return old;
}
#define SP_POOL_CTR_INC(c) ((c)++)
#define SP_POOL_CTR_DEC(c) ((c)--)
#define SP_POOL_RECYCLE_BODY(CLS, h) \
    if (sp_##CLS##_pool_count >= sp_##CLS##_pool_max) { \
      free(h); sp_##CLS##_pool_freed++; return; \
    } \
    (h)->next = sp_##CLS##_pool_head; \
    sp_##CLS##_pool_head = (h); \
    sp_##CLS##_pool_count++; \
    sp_##CLS##_pool_pushes++; \
    if (sp_##CLS##_pool_count > sp_##CLS##_pool_hwm) sp_##CLS##_pool_hwm = sp_##CLS##_pool_count;
#endif
#define SP_POOL_DEFINE(CLS) \
  static sp_gc_hdr *sp_##CLS##_pool_head = NULL; \
  static long sp_##CLS##_pool_count = 0; \
  static long sp_##CLS##_pool_max = SP_POOL_DEFAULT_MAX; \
  static long sp_##CLS##_pool_pushes = 0; \
  static long sp_##CLS##_pool_pops = 0; \
  static long sp_##CLS##_pool_freed = 0; \
  static long sp_##CLS##_pool_hwm = 0; \
  __attribute__((constructor)) static void sp_##CLS##_pool_init(void) { \
    const char *m = getenv("SP_POOL_MAX"); \
    if (m && *m) { long v = atol(m); if (v >= 0) sp_##CLS##_pool_max = v; } \
  } \
  /* Runs only from the sweep, so no pop can run concurrently -- but the sweep
     is PARALLEL in the threaded build (sp_sched_par_sweep: every parked worker
     sweeps its own slot), so several workers push onto this one list at once.
     A plain push there loses nodes and tears the counters; the threaded arm
     pushes with the same CAS loop sp_PolyArray_pool_recycle uses, and counts
     atomically. Single-threaded: the exact plain code this always had. */ \
  static void sp_##CLS##_pool_recycle(sp_gc_hdr *h) { \
    SP_POOL_RECYCLE_BODY(CLS, h) \
  } \
  __attribute__((destructor)) static void sp_##CLS##_pool_report(void) { \
    const char *e = getenv("SP_POOL_REPORT"); \
    if (!e || !e[0] || e[0] == '0') return; \
    fprintf(stderr, #CLS " pool: pops=%ld pushes=%ld over_cap_freed=%ld hwm=%ld retained=%ld cap=%ld\n", \
      sp_##CLS##_pool_pops, sp_##CLS##_pool_pushes, sp_##CLS##_pool_freed, \
      sp_##CLS##_pool_hwm, sp_##CLS##_pool_count, sp_##CLS##_pool_max); \
  }

#define SP_POOL_NEW(CLS, SCAN) (__extension__ ({ \
  sp_##CLS *_p; \
  sp_gc_hdr *_h = sp_pool_try_pop(&sp_##CLS##_pool_head); \
  if (_h) { \
    SP_POOL_CTR_DEC(sp_##CLS##_pool_count); \
    SP_POOL_CTR_INC(sp_##CLS##_pool_pops); \
    sp_gc_pool_relink(_h); \
    _h->recycle = sp_##CLS##_pool_recycle; \
    _p = (sp_##CLS *)((char *)_h + sizeof(sp_gc_hdr)); \
    if (sp_alloc_report_on) sp_alloc_report_count((void *)(SCAN), sizeof(sp_##CLS)); \
  } \
  else { \
    _p = (sp_##CLS *)sp_gc_alloc_pool(sizeof(sp_##CLS), SCAN, sp_##CLS##_pool_recycle); \
  } \
  _p; \
}))

/* `Object.new` — a sentinel object whose only meaningful property is
   identity. Each call returns a fresh GC-managed allocation, so two
   `Object.new` results compare as `!=` via their pointer addresses. */
typedef struct sp_Object_s { uint8_t _pad; } sp_Object;
static sp_Object *sp_Object_new(void){return(sp_Object*)sp_gc_alloc(sizeof(sp_Object),NULL,NULL);}

/* Integer#[start, len]: the len-bit field starting at bit `start`, i.e.
   (n >> start) & ((1 << len) - 1) with Ruby's shift semantics (a negative
   count shifts the other way), clamped to the 64-bit word so an out-of-range
   start/len can't trigger an undefined shift. The receiver shift is arithmetic
   so a negative `n`'s high bits read as 1. */
/* n[i]: one bit, with the shift clamped so an out-of-range index never
   emits an undefined C shift (i >= 64 reads the sign fill; negative is 0,
   matching CRuby's infinite two's-complement view). */
static inline sp_int sp_int_bit(sp_int n, sp_int i) {
  if (i < 0) return 0;
  if (i >= 64) return n < 0 ? 1 : 0;
  return (n >> i) & 1;
}
/* sp_FloatArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). */

/* sp_PtrArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). */

/* sp_StrArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). sp_StrArray_from_string_range stays here because it needs
   sp_str_succ (a string-batch helper defined further down). */
/* Forward decl — sp_str_succ is defined further down (it uses
   sp_utf8_decode); the StrArray_from_string_range loop below needs
   it visible early. */
/* String range to_a — single-char and multi-char ASCII ranges via
   sp_str_succ. The 4096-iteration cap stops a pathological prepend-
   style infinite loop before it eats memory. */
/* Case-insensitive string compare. Portable across glibc / MinGW
   (avoids strcasecmp which lives in strings.h on POSIX and is named
   stricmp on Windows). Returns -1 / 0 / 1 like CRuby's String#casecmp. */


/* GC.stat snapshot: String=>Integer hash over the collector globals.
   full_runs derives from sp_gc_cycle / SP_GC_FULL_INTERVAL (the major
   collection cadence). */





/* sp_float_to_s now lives in sp_alloc.h (shared). Float#inspect is aliased. */
#define sp_float_inspect sp_float_to_s

/* Bound-checked clamp. The arithmetic lives in the compiled-once leaf
   helpers (sp_core.c); the validation rides here because it needs the
   error-message machinery (sp_sprintf) and Ruby float inspect, neither of
   which the decoupled sp_core.c TU links against. CRuby's Comparable#clamp
   compares min<=>max first (so a NaN bound names max), then self against
   each bound (so a NaN receiver names min); a non-NaN min>max is the
   ordinary ArgumentError. */
static inline sp_int sp_int_clamp_ck(sp_int v,sp_int lo,sp_int hi){
  if(lo>hi)sp_raise_cls("ArgumentError","min argument must be less than or equal to max argument");
  return sp_int_clamp(v,lo,hi);
}
static inline sp_float sp_float_clamp_ck(sp_float v,sp_float lo,sp_float hi){
  if(lo!=lo||hi!=hi)sp_raise_cls("ArgumentError",sp_sprintf("comparison of Float with %s failed",sp_float_to_s(hi)));
  if(lo>hi)sp_raise_cls("ArgumentError","min argument must be less than or equal to max argument");
  if(v!=v)sp_raise_cls("ArgumentError",sp_sprintf("comparison of Float with %s failed",sp_float_to_s(lo)));
  return sp_float_clamp(v,lo,hi);
}
/* clamp(range): an exclusive range with a real end cannot clamp (CRuby); an
   exclusive ENDLESS range (`1...`, last is the INTPTR_MAX sentinel) can. The
   beginless/endless sentinels satisfy sp_int_clamp_ck's bounds naturally. */
static inline sp_int sp_int_clamp_range_ck(sp_int v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  /* Reaching here with excl means r.last is the INTPTR_MAX endless sentinel
     (a real exclusive end raised above); pass it through unchanged -- it means
     "no upper bound", which sp_int_clamp_ck handles, not INTPTR_MAX-1. */
  return sp_int_clamp_ck(v, r.first, r.last);
}
/* `:name`, or `:"name"` when the name needs quoting -- shares the
   name-string predicates in lib/sp_str.c with the hash-key short form. */
static const char *sp_sym_inspect(sp_sym id) { if (id == (sp_sym)-1) return SPL("nil"); /* nilable-symbol sentinel */ return sp_sym_inspect_name(sp_sym_to_s(id)); }
static const char*sp_gets(void){char buf[4096];if(!fgets(buf,sizeof(buf),stdin))return NULL;size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);return r;}
static sp_StrArray*sp_readlines(void){sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);char buf[4096];while(fgets(buf,sizeof(buf),stdin)){size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);sp_StrArray_push(a,r);}return a;}
#ifdef SPINEL_EXT_HOST
const char*sp_sprintf(const char*fmt,...);
#else
const char*sp_sprintf(const char*fmt,...){char _sp_tmp[4096];va_list ap;va_start(ap,fmt);int _sp_n=vsnprintf(_sp_tmp,sizeof(_sp_tmp),fmt,ap);va_end(ap);if(_sp_n<0)_sp_n=0;char*b=sp_str_alloc((size_t)_sp_n);if(_sp_n<(int)sizeof(_sp_tmp)){memcpy(b,_sp_tmp,(size_t)_sp_n);}
else{/* result didn't fit the stack temp; re-render at full width (sp_str_alloc gives _sp_n bytes + NUL) so long string interpolations aren't truncated. re-arm the va_list rather than va_copy so the common fast path pays nothing */va_start(ap,fmt);vsnprintf(b,(size_t)_sp_n+1,fmt,ap);va_end(ap);}return b;}
#endif
/* Use a temp pointer for realloc so the original buffer is not leaked
   on allocation failure. Match the perror+exit pattern used elsewhere
   (see sp_IntArray_replace) instead of returning a partial result. */

/* String#count with multiple args: intersection of charsets.
   Each additional arg further restricts which chars to count.
   sp_str_count_n(s, args[], n) computes the intersection. */
/* Issue #800: clamp l*n so a malicious input can't allocate a tiny
   buffer through size_t overflow. */
/* Issue #836: bound the multiplier so a wildly oversized request
   raises ArgumentError rather than segfaulting when malloc returns
   NULL and memcpy walks it. 1 GiB cap covers realistic use. */
/* Issue #903: String#codepoints -- one IntArray entry per UTF-8
   codepoint (not byte). Replacement-character behaviour mirrors
   sp_utf8_decode (returns the leading byte for malformed seqs). */
/* Issue #902: String#tr_s -- translate AND squeeze consecutive
   identical results into one. Walks codepoint-by-codepoint and
   collapses adjacent duplicates only among the translated bytes
   (untranslated runs keep their original characters). */
/* Build into a malloc temp and read all of `s` BEFORE the result sp_str_alloc:
   that allocation can now trigger a string-heap collection, which would sweep
   an unrooted `s` mid-copy (the read-first pattern sp_str_tr/sp_str_format use). */
/* Issue #921: shrink the heap-string header length to match the
   squeezed payload — the alloc gives bl+1 bytes, the squeezed
   write fills n<=bl, leaving the header's stored length stale.
   `bytes` / `length` consult the header (not strlen), so callers
   would see the alloc size and trailing NULs. */
/* String#squeeze(chars) — only squeeze chars listed in the charset
   (same charset syntax as tr: a-z, ^x, etc.). Consecutive runs of
   non-listed chars pass through untouched. */
/* Multi-arg delete/squeeze: delete (or squeeze runs of) characters that
   are in the INTERSECTION of all n charset args, mirroring
   sp_str_count_n. Each arg is a charset spec (^negation, a-z ranges). */

/* Forward decl from sp_crypto.h (libspinel_rt.a). Used by
   sp_str_crypt below to provide a deterministic crypt-like
   hash without dragging in libc crypt(3). */
const char *sp_crypto_hmac_sha256_b64url(const char *key, const char *msg);

/* String#crypt — Spinel's crypt is NOT the libc DES crypt. It
   returns `salt[0..1] || hmac_sha256(salt, password)[0..10]` as
   a 13-char string (same length as DES crypt, deterministic,
   stronger primitive). CRuby's spec says String#crypt is impl-
   defined and "should not be used for security"; this matches
   that contract while keeping outputs reproducible across
   spinel builds. Short salts get padded with '.' so the result
   still has the canonical first-2-chars-are-salt shape. */

/* String#scrub — walk the bytes; for each valid UTF-8 lead +
   continuation sequence, copy through. For invalid bytes (lone
   continuation, truncated multi-byte, overlong, etc.), emit the
   replacement string and skip one byte. NULL replacement uses
   U+FFFD (3 UTF-8 bytes: EF BF BD), matching CRuby. */

/* String#setbyte: mutate s[i] = v in place. Spinel adopts
   `# frozen_string_literal: true` semantics globally — all
   string literals are frozen, mutation requires a heap-allocated
   buffer (e.g. via .dup or string concatenation). The runtime
   marker byte at s[-1] tells us the provenance:
     0xfe / 0xfc -> sp_str_alloc heap (writable, GC unmarked / marked)
     0xfd        -> sp_String wrapper buffer (writable)
     0xff        -> rodata literal (frozen -> FrozenError)
     other       -> FFI / unknown provenance, treated as frozen
   Returns the byte value (CRuby setbyte return). */
static inline sp_int sp_str_getbyte(const char *s, sp_int i) {
  if (!s) sp_nil_recv("getbyte");
  sp_int bl = (sp_int)sp_str_byte_len(s);
  if (i < 0) i += bl;
  if (i < 0 || i >= bl) return 0;
  return (sp_int)(unsigned char)s[i];
}
/* String#getbyte: a negative index counts from the end, and an out-of-range
   index is nil (SP_INT_NIL, a nullable int) -- not 0 or an adjacent byte. */
static inline sp_int sp_str_getbyte_opt(const char *s, sp_int i) {
  if (!s) sp_nil_recv("getbyte");
  sp_int bl = (sp_int)sp_str_byte_len(s);
  if (i < 0) i += bl;
  if (i < 0 || i >= bl) return SP_INT_NIL;
  return (sp_int)(unsigned char)s[i];
}

static inline sp_int sp_str_setbyte(const char *s, sp_int i, sp_int v) {
  if (!s) sp_nil_recv("setbyte");
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    (((sp_str_hdr *)(s - 1)) - 1)->hash = 0;  /* invalidate cached key hash */
      (((sp_str_hdr *)(s - 1)) - 1)->size &= ~SP_STR_SIZE_ASCII7;  /* and the 7-bit answer */
    ((char *)s)[i] = (char)v;
    return v;
  }
  if (m == 0xfd) {
    ((char *)s)[i] = (char)v;
    return v;
  }
  sp_raise_frozen_str(s);
  return v;
}

/* String#-@: an already-frozen receiver (or an immutable rodata literal)
   returns ITSELF -- CRuby's uminus is an interning hint, and `(-a).equal?(a)`
   is true for a frozen `a`. Only a mutable string takes the freeze-copy. */
static inline const char *sp_str_uminus_val(const char *s);
static inline const char *sp_str_freeze_val(const char *s) {
  if (!s) return s;
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    ((unsigned char *)s)[-1] = 0xf1;
    return s;
  }
  if (m == 0xff || m == 0xf1 || m != 0xfd) {
    /* rodata literal or already frozen: copy to heap and freeze */
    if (m == 0xf1) return s;  /* already heap-frozen */
    size_t n = strlen(s);
    char *r = sp_str_alloc(n);
    memcpy(r, s, n);
    ((unsigned char *)r)[-1] = 0xf1;
    return r;
  }
  return s;
}
/* Frozen-string dedup (String#-@ / #dedup) interning: equal-content dedups
   must return the SAME immortal frozen object, like CRuby's fstring table, so
   `-"x".equal?(-"x")` is true. An interned string is a 0xf1 frozen heap string,
   which sp_str_sweep keeps across every collection, so the table needs no GC
   rooting; it only grows (interned strings are permanent, as in CRuby). The
   table is guarded by the heap lock; the frozen copy is allocated OUTSIDE the
   lock (allocators take the heap lock themselves), then a re-check under the
   lock keeps a single canonical entry per content. */
static const char **sp_fstr_tab = NULL;
static size_t sp_fstr_cap = 0, sp_fstr_len = 0;
static const char *sp_fstr_lookup(const char *s) {  /* caller holds the heap lock */
  if (!sp_fstr_cap) return NULL;
  size_t mask = sp_fstr_cap - 1, idx = (size_t)(sp_str_hash(s) & mask);
  while (sp_fstr_tab[idx]) {
    if (sp_str_eq(sp_fstr_tab[idx], s)) return sp_fstr_tab[idx];
    idx = (idx + 1) & mask;
  }
  return NULL;
}
static void sp_fstr_insert(const char *f) {  /* caller holds the heap lock */
  if (sp_fstr_cap == 0 || sp_fstr_len * 2 >= sp_fstr_cap) {
    size_t nc = sp_fstr_cap ? sp_fstr_cap * 2 : 64;
    const char **ntab = (const char **)calloc(nc, sizeof(const char *));
    if (!ntab) return;
    for (size_t i = 0; i < sp_fstr_cap; i++) {
      const char *k = sp_fstr_tab[i];
      if (!k) continue;
      size_t idx = (size_t)(sp_str_hash(k) & (nc - 1));
      while (ntab[idx]) idx = (idx + 1) & (nc - 1);
      ntab[idx] = k;
    }
    free(sp_fstr_tab); sp_fstr_tab = ntab; sp_fstr_cap = nc;
  }
  size_t mask = sp_fstr_cap - 1, idx = (size_t)(sp_str_hash(f) & mask);
  while (sp_fstr_tab[idx]) idx = (idx + 1) & mask;
  sp_fstr_tab[idx] = f; sp_fstr_len++;
}
static const char *sp_str_dedup(const char *s) {
  SP_HEAP_LOCK();
  const char *hit = sp_fstr_lookup(s);
  SP_HEAP_UNLOCK();
  if (hit) return hit;
  /* byte_len-aware copy so an embedded NUL is preserved (sp_str_dup_external
     would truncate at the first NUL), then freeze it to the immortal 0xf1. */
  const char *f = sp_str_freeze_val(sp_str_from_bytes(s, sp_str_byte_len(s)));
  SP_HEAP_LOCK();
  const char *hit2 = sp_fstr_lookup(f);  /* another thread may have won the race */
  if (hit2) { SP_HEAP_UNLOCK(); return hit2; }
  sp_fstr_insert(f);
  SP_HEAP_UNLOCK();
  return f;
}
static inline const char *sp_str_uminus_val(const char *s) {
  if (!s) return s;
  /* An already-frozen receiver (0xf1) still interns by content (CRuby
     rb_fstring): the first frozen holder of a content registers itself,
     later dedups of equal contents return that same object -- even when
     every literal occurrence is its own frozen object under fsl. Frozen
     entries are immortal, so registering the receiver is copy-free. A
     0xff rodata value string (frozen? false by design) takes the plain
     dedup below, which hands back a frozen interned copy. */
  if (((const unsigned char *)s)[-1] == 0xf1) {
    SP_HEAP_LOCK();
    const char *hit = sp_fstr_lookup(s);
    if (!hit) { sp_fstr_insert(s); hit = s; }
    SP_HEAP_UNLOCK();
    return hit;
  }
  /* otherwise intern by content so two dedups of equal contents share one
     frozen object (#2462). The result reports frozen? (0xf1). */
  return sp_str_dedup(s);
}
/* String#clone: a copy that preserves the frozen state, unlike #dup which always
   returns an unfrozen copy (CRuby semantics). Carries the 0xf1 heap-frozen marker
   across to the fresh buffer. */
static inline const char *sp_str_clone_val(const char *s) {
  if (!s) return NULL;
  const char *r = sp_str_dup(s);  /* byte_len-aware: clone carries embedded NULs */
  if (r && sp_str_is_frozen_val(s)) ((unsigned char *)r)[-1] = 0xf1;
  return r;
}
/* s[from, n] = val (char-based splice): prefix + val + suffix. A negative
   `from` counts from the end; out-of-range raises (RangeError for the range
   form, IndexError for the (start, len) form); an explicit negative length
   raises IndexError. Over-long spans clamp to the tail. */
/* sp_str_splice_at: moved to lib/sp_cold.c */
const char *sp_str_splice_at(const char *s, sp_int from, sp_int n, const char *val, int range_form);

/* sp_String (mutable-String builder) moved to sp_string.h / lib/sp_string.c:
   the hot construction/append core is inline in the header, the cold in-place
   mutators (prepend/insert/replace/dup) compile once in the archive. */

/* `File.open(path, mode)` without a block returns an sp_File * — a
   GC-managed wrapper around `FILE *fp`. The finalizer fclose()s any
   still-open fp so a dropped file handle doesn't leak. `path` and
   `mode` are kept live for `f.path` / `f.mode` introspection; they
   come from the caller's already-live string slot so no extra mark
   is needed. */
/* File / IO handle ops live in libspinel_rt.a (lib/sp_io.c); the sp_File
   struct and the allocation-free op prototypes come from sp_io.h. The
   string-returning readers below (gets / read / read_n / path) stay
   inline here because they allocate via the hot static sp_str_alloc,
   whose per-TU sp_str_heap can't be shared across translation units. */
#include "sp_io.h"
static inline const char *sp_File_gets(sp_File *f) {
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  /* heap scratch, NOT a stack buffer: a green thread runs on a 64KB fiber
     stack (SP_FIBER_STACK_SIZE), which a 64KB local overran straight into
     the guard page -- `Thread.new { f.gets }` was an instant segfault. */
  char *buf = (char *)malloc(65536);
  if (!buf) return NULL;
  if (!fgets(buf, 65536, f->fp)) { free(buf); return NULL; }
  size_t n = strlen(buf);
  char *r = sp_str_alloc(n);
  memcpy(r, buf, n);
  free(buf);
  f->lineno++;
  return r;
}
/* Read a line into a caller-provided buffer without allocating. The returned
   pointer is `buf` (or NULL at EOF); valid only until the next call. Used by
   each_line loops where the line does not escape the loop body. */
static inline const char *sp_File_gets_buf(sp_File *f, char *buf, int size) {
  if (!f || !f->fp) return NULL;
  if (!fgets(buf, size, f->fp)) return NULL;
  return buf;
}
/* Like gets_buf, but into a reusable HEAP line string: the buffer carries a
   real string header (marker + length), so it is a first-class spinel string
   -- runtime helpers may read its marker or root it safely. Every string
   crossing the runtime API must be spinel-marked (or a 0xff literal); a raw
   stack buffer whose [-1] byte is arbitrary memory breaks the GC mark. */
static inline const char *sp_File_gets_into(sp_File *f, char *s, int cap) {
  if (!f || !f->fp) return NULL;
  if (!fgets(s, cap, f->fp)) return NULL;
  sp_str_set_len(s, strlen(s));
  f->lineno++;
  return s;
}
/* IO#gets with separator / limit / chomp (#2809, #2810, #2820). sep NULL
   reads to EOF; limit <= 0 means unlimited. NULL at EOF. */
const char *sp_File_gets_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp);
/* IO#readlines with separator / chomp (#2820) */
sp_StrArray *sp_File_readlines_sep(sp_File *f, const char *sep, sp_bool chomp);
sp_StrArray *sp_file_readlines_sep(const char *path, const char *sep, sp_bool chomp);
/* IO#readline: gets or EOFError (#2817) */
const char *sp_File_readline_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp);
/* IO#getc: one (UTF-8) character, nil (NULL) at EOF */
const char *sp_File_getc(sp_File *f);
const char *sp_File_readchar(sp_File *f);
sp_int sp_File_getbyte(sp_File *f);
/* IO#ungetc: push back the (first byte of the) argument */
sp_RbVal sp_File_ungetc(sp_File *f, sp_RbVal v);
/* IO#readpartial / #sysread: up to n bytes, EOFError at EOF (#2812) */
const char *sp_File_readpartial(sp_File *f, sp_int n);
/* IO#pread(len, offset): read without moving the file position. Inline
   because it allocates from this TU's string heap (#3038). */
static inline const char *sp_File_pread(sp_File *f, sp_int len, sp_int off) {
  SP_IO_OPEN(f);
  if (len < 0) len = 0;
  char *buf = (char *)sp_str_alloc((size_t)len);
  ssize_t got = pread(fileno(f->fp), buf, (size_t)len, (off_t)off);
  if (got < 0) sp_raise_cls("IOError", "pread failed");
  if (got == 0 && len > 0) sp_raise_cls("EOFError", "end of file reached");
  buf[got] = '\0';
  sp_str_set_len(buf, (size_t)got);
  return buf;
}
sp_int sp_File_sysseek(sp_File *f, sp_int off, sp_int whence);
sp_int sp_File_flock(sp_File *f, sp_int op);
sp_int sp_File_fsync(sp_File *f);
/* IO#putc: write one character (Integer byte or a String's first char),
   returning the argument */
sp_RbVal sp_File_putc(sp_File *f, sp_RbVal v);
/* IO.copy_stream(src, dst): both path strings (#2815); returns bytes copied */
/* IO.copy_stream(src_path, dst_path): defined out-of-line in sp_cold.c. */
sp_int sp_io_copy_stream(const char *src, const char *dst);
const char *sp_slurp_stream(FILE *fp);
static inline const char *sp_File_read(sp_File *f) {
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  /* A handle whose read can block fills to EOF through the parking slurp: the
     plain one asks fread for a whole buffer and sits in the kernel between a
     pipe writer's chunks, holding the OS worker (#4307). */
  if (f->park == 2) return sp_slurp_stream_parked(f);
  /* One reader for every stream: the seek size is a hint, so a seekable file
     reporting 0 (a /proc entry) and a non-seekable one (a pipe end, a socket,
     a FIFO) both read to EOF (#3411). */
  return sp_slurp_stream(f->fp);
}
/* IO#read(n): read up to n bytes from the current position. Returns NULL
   (nil) at EOF for a positive n, "" for n == 0, and the whole rest for a
   negative n (treated as the no-count read). A short read produces a
   string of the bytes actually read. */
static inline const char *sp_File_read_n(sp_File *f, sp_int n) {
  SP_IO_OPEN(f);
  sp_io_wait_readable(f);
  if (n < 0) return sp_File_read(f);
  if (n == 0) return sp_str_empty;
  char *r = sp_str_alloc((size_t)n);
  size_t got = fread(r, 1, (size_t)n, f->fp);
  if (got == 0) return NULL;
  /* record the byte count read: without it an embedded NUL truncated every
     later length/slice, which read the bytes back through strlen (#3540) */
  if ((sp_int)got == n) { r[got] = 0; sp_str_set_len(r, got); return r; }
  char *s = sp_str_alloc(got);
  memcpy(s, r, got);
  s[got] = 0;
  sp_str_set_len(s, got);
  return s;
}
static inline const char *sp_File_path(sp_File *f) { return f && f->path ? f->path : sp_str_empty; }
/* sp_file_join: moved to lib/sp_cold.c */
const char *sp_file_join(const char **parts, int n);
static inline sp_StrArray *sp_File_readlines(sp_File *f) {
  sp_StrArray *a = sp_StrArray_new();
  const char *line;
  while ((line = sp_File_gets(f)) != NULL) sp_StrArray_push(a, line);
  return a;
}
/* sp_file_readlines: moved to lib/sp_cold.c */
sp_StrArray *sp_file_readlines(const char *path);
/* sp_file_readlines_chomp: moved to lib/sp_cold.c */
sp_StrArray *sp_file_readlines_chomp(const char *path);

/* Array#inspect for each typed array: `[elem1, elem2, ...]` with each
   element rendered via its own primitive inspect. Matches CRuby's
   Array#inspect output byte-for-byte. Returns a GC-managed C string. */
/* The inspect helpers NULL-guard their receiver: a NULL array can
   reach here when an unresolved call (e.g. an unsupported Array method)
   emits a 0 placeholder that flows into `.inspect`, and dereferencing
   a->len would segfault. Rendering "[]" stops the crash and degrades to
   the same shape as the empty-array case. */
/* Symbol arrays share the IntArray representation (sp_sym = sp_int),
   but each element is rendered as ":name" via sp_sym_to_s. */
static inline const char*sp_SymArray_inspect(sp_IntArray*a){return a?sp_inspect_container(sp_box_obj(a,SP_BUILTIN_SYM_ARRAY)):"[]";}
/* PtrArray elements are object pointers without a per-element class
   tag, so we render them as `#<Object>` rather than recursing. */
/* Nested-array inspect: when codegen knows the ptr_array's element
   type is one of the four built-in T_array shapes, recurse into the
   matching primitive inspect . */
static const char*sp_IntArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_IntArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return sp_str_dup(s->data);}
/* Issue #742: Array#combination(k) on int_array -- emit all
   k-element ordered combinations as a PtrArray of IntArrays. */
/* Array#combination / repeated_combination / permutation / repeated_permutation
   over an int array: defined out-of-line in sp_cold.c (lib-only helpers). */
sp_PtrArray *sp_IntArray_combination(sp_IntArray *a, sp_int k);
sp_PtrArray *sp_IntArray_repeated_combination(sp_IntArray *a, sp_int k);
sp_PtrArray *sp_IntArray_permutation(sp_IntArray *a, sp_int k);
sp_PtrArray *sp_IntArray_repeated_permutation(sp_IntArray *a, sp_int k);
static const char*sp_FloatArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_FloatArray_inspect((sp_FloatArray*)a->data[i]));}sp_String_append(s,"]");return sp_str_dup(s->data);}
static const char*sp_StrArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_StrArray_inspect((sp_StrArray*)a->data[i]));}sp_String_append(s,"]");return sp_str_dup(s->data);}
/* sp_PolyArrayPtrArray_inspect lives below sp_PolyArray_inspect's
   forward declaration (sp_PolyArray isn't defined until ~2542). */
static const char*sp_SymArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_SymArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return sp_str_dup(s->data);}
/* issue #526: join for a sp_PtrArray of sp_String* (mutable_str_ptr_array).
   Sibling to sp_StrArray_join, but takes advantage of sp_String's known
   length: two-pass — sum the exact total, sp_str_alloc once, then memcpy
   each element by its s->len (preserves embedded NULs). Avoids the
   realloc-grow loop's leak-on-failure and long-separator overflow
   risks, and skips an intermediate malloc'd buffer. NULL entries
   contribute zero length. */

#ifdef __FreeBSD__

#define re_exec spinel_re_exec

#define MAP_NORESERVE 0

#endif

/* Regexp engine (link with libspre.a from lib/regexp/) */

/* Regexp globals: $1-$9 captures */
/* NULL (not "") so sp_mark_string's null-guard handles the unset case
   without reaching the `s[-1]` access. The rodata `""` literal would
   trigger -Wstringop-overflow under -O3 + sp_mark_string inlining at
   the call site in sp_re_mark_globals — gcc proves the `s[-1] = 0xfc`
   write would be out-of-bounds even though the runtime guard
   `s[-1] == 0xfe` (always false for rodata) prevents it from firing. */

/* Symbolic back-references populated alongside the numbered captures.
   Read by codegen's BackReferenceReadNode arm:
     $&  -> sp_re_match_str (the whole matched substring)
     $`  -> sp_re_match_pre  (substring before the match)
     $'  -> sp_re_match_post (substring after the match)
   $~ falls back to $& since Spinel has no MatchData wrapper. */

/* ARGV runtime: argv[i] strings are dup'd via sp_str_dup_external on
   main() entry, which allocates from the str-heap with mark byte
   0xfe. Without explicit marking they get reaped on the first
   sp_str_sweep, leaving sp_argv.data[i] as a dangling pointer that
   later `ARGV[i]` reads dereference. The exact length boundary
   triggering the segfault depends on malloc's reuse pattern (so the
   bug surfaces non-deterministically by string length), but the
   underlying issue is unconditional. */
#ifdef SPINEL_EXT_HOST
extern sp_Argv sp_argv;
#else
sp_Argv sp_argv;   /* type in sp_argf.h; storage here, populated by main() */
#endif
static const char *sp_program_name = SPL("");

/* ARGF: a pseudo-IO that reads the files named in ARGV in sequence, or stdin
   when ARGV is empty (a `-` filename also means stdin). The state is a single
   global; the ARGF constant is a marker pointer to it. */
#ifdef SPINEL_EXT_HOST
extern sp_Argf sp_argf_obj;
#else
sp_Argf sp_argf_obj = {0, NULL, 0, NULL};   /* type in sp_argf.h */
#endif

/* Mark active in-flight exception messages. Most raises pass string
   literals (rodata, marker byte ≠ 0xfe → no-op for sp_mark_string),
   but raises that build messages dynamically via sp_sprintf (e.g.
   sp_str_to_i_strict's `"invalid value for Integer(): \"%s\""`)
   hand a heap-allocated string to sp_raise_cls. Without marking, a
   GC cycle between the raise and the rescue handler reading the
   message would sweep the message and leave sp_exc_msg[i] dangling.
   The helper itself is defined further down (after sp_exc_msg /
   sp_exc_top are declared); only the prototype lives here. */
static void sp_mark_in_flight_exceptions(void);
/* Mark the current fiber's in-flight proc-return values; defined with the
   proc-return machinery (sp_proc_home) further down. */
static void sp_mark_proc_homes(void);
/* Mark in-flight valued-break values; defined with the sp_brk machinery. */
static void sp_mark_brk_vals(void);
/* Mark the registered at_exit hooks; defined with the hook table further down. */
static void sp_mark_at_exit_hooks(void);

/* Mark the regex globals as live during GC. Each holds a pointer to a
   string allocated via sp_str_alloc_raw on the str-heap; without this
   sp_str_sweep would reap them on the next collect, leaving dangling
   pointers in $1..$9, $&, $`, $'. sp_mark_string is null-safe and
   no-ops on non-heap strings (the empty-string default of
   sp_re_last_str), so it's safe to call unconditionally. */
/* sp_fiber_root.storage needs marking here because sp_fiber_root is
   a static (not sp_gc_alloc'd), so its normal sp_Fiber_scan never
   runs via the heap walker. Without this, top-level `Fiber[:k] = v`
   writes get prematurely collected. The forward declaration is
   needed because sp_fiber_root is defined further down in the
   Fiber runtime block. */
/* External linkage: lib/sp_gc.c's sp_gc_mark_all reaches this by name. */
static void sp_re_mark_globals(void) {
  /* The sub-markers below are static and inline away, so a fault in one of
     them reports as this frame with nothing to distinguish them. Under verify,
     name the group being walked: `phase=globals` alone cannot say whether the
     bad pointer came from the regex registers, the exception stack, a
     proc-return home, a break value or fiber storage (#3404). Costs a store
     per group and only when verify is on. */
#define SP_GLB_PHASE(x) do { if (sp_gc_verify_on()) sp_gc_dbg_phase = (x); } while (0)
  SP_GLB_PHASE("globals:regex");
  sp_mark_string(sp_re_last_str);
  for (int i = 0; i < 10; i++) sp_mark_string(sp_re_captures[i]);
  sp_mark_string(sp_re_match_str);
  sp_mark_string(sp_re_match_pre);
  sp_mark_string(sp_re_match_post);
  SP_GLB_PHASE("globals:argv");
  for (sp_int i = 0; i < sp_argv.len; i++) sp_mark_string(sp_argv.data[i]);
  if (sp_argv_array_cache) sp_gc_mark(sp_argv_array_cache);
  SP_GLB_PHASE("globals:exceptions");
  sp_mark_in_flight_exceptions();
  SP_GLB_PHASE("globals:proc-homes");
  sp_mark_proc_homes();
  SP_GLB_PHASE("globals:break-values");
  sp_mark_brk_vals();
  SP_GLB_PHASE("globals:at-exit");
  sp_mark_at_exit_hooks();
  SP_GLB_PHASE("globals:fiber-storage");
  sp_mark_fiber_root_storage();
  SP_GLB_PHASE("globals");
#undef SP_GLB_PHASE
}

/* Hand the collector (lib/sp_gc.c) this TU's root-marking and string-heap
   sweep. Runs before main, so the hooks are set before the first
   allocation can trigger a collection. */
__attribute__((constructor)) static void sp_gc_install_tu_hooks(void) {
  sp_gc_mark_globals_hook = sp_re_mark_globals;
  /* sp_gc_str_sweep_hook is installed by sp_alloc.c's constructor. */
}

/* `$+` / `$LAST_PAREN_MATCH` — contents of the highest-indexed group
   that participated in the match. Walks sp_re_captures[] from 9 down
   and returns the first non-NULL entry. NULL when no group matched
   (codegen ternary falls back to ""). Matches CRuby's behaviour:
   for /(a)(b)?/ matching "a", $+ is "a"; for /(a)(b)/ matching "ab",
   $+ is "b". */


/* `=~` returns the match position (0-indexed) or -1 on miss.
   Codegen's regex truthy check (regex_match_call_node? arm in
   compile_cond_expr) compares against -1 so match-at-position-0
   is correctly truthy. Direct value use lines up with CRuby's
   `String#=~` int semantics: "abc" =~ /b/ -> 1, not 2. */

/* `s.rindex(regex)` — last match start, in BYTE offset (matches
   the way sp_str_rindex reports indices for plain-string search;
   codepoint translation would require a UTF-8 walk and the
   handful of call sites that consume rindex don't need it).
   Walks forward through successive matches and remembers the
   latest start. Issue #504: previously the codegen routed
   `s.rindex(/re/)` to sp_str_rindex(s, 0) and SEGV'd at
   strlen(NULL). Returns -1 on no match. */

/* `s.rpartition(regex)` -> [before, last_match, after]. On no match Ruby
   returns ["", "", s] (the whole string lands in the last slot). Walks
   forward to the final match span, mirroring sp_re_rindex. */


/* Issue #869: Regexp#match?(str, pos) starts matching at byte
   offset `pos`. Negative pos counts from the end (CRuby compat).
   Out-of-range pos returns false. */

/* Issue #855: expand `\1`..`\9` / `\&` / `\0` backreferences in
   the replacement string against the current caps[] array. `\\`
   is a literal backslash. Writes to *out_io at *olen_io, growing
   *out_io / *cap_io as needed. */








/* NaN-boxed polymorphic value */
typedef uint64_t sp_RbValue;
#define SP_TAG_INT 0
#define SP_TAG_STR 1
#define SP_TAG_FLT 2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL 4
#define SP_TAG_OBJ 5
#define SP_TAG_SYM 6
/* Class values boxed into a poly slot (e.g.
   as ancestors-array elements). cls_id of the sp_RbVal carries
   the boxed sp_Class's cls_id directly so unboxing is just a
   field read. */
#define SP_TAG_CLASS 7
/* Encoding values boxed into a poly slot. v.s carries the canonical
   encoding name (`"UTF-8"` in Spinel's current runtime model). */
#define SP_TAG_ENCODING 8
/* Arbitrary-precision integer boxed into a poly slot. v.p is a
   GC-allocated sp_Bigint* (so a heterogeneous Array/Hash can hold a
   bignum, and --int-overflow=promote can box an overflow result). */
#define SP_TAG_BIGINT 9
/* Negative cls_id values let SP_TAG_OBJ also carry built-in pointer
   types (IntArray, FloatArray, ...) — avoids minting a new SP_TAG_*
   per type. Non-negative cls_id stays an index into the user-class
   table as before. The element-type tag and the array cls_id are
   paired by `array_cls_id = -element_tag - 1`. */
struct sp_Exception_s;
/* Hash variant cls_ids — boxed into the cls_id of a poly slot so
   Hash#dig can recover the concrete hash type at runtime. */
/* SP_BUILTIN_FOREIGN_PTR (-25), SP_BUILTIN_COMPLEX (-26) and
   SP_BUILTIN_RATIONAL (-27) are defined in sp_gc.h (shared with lib readers). */
/* sp_RbVal is defined in sp_gc.h (the mark helpers dispatch on its tag). */
/* Forward declarations for the bigint API the poly helpers below call; the
   full prototypes live further down (near the bigint runtime block). */
typedef struct sp_Bigint sp_Bigint;
const char *sp_bigint_to_s(sp_Bigint *b);
const char *sp_bigint_to_s_base(sp_Bigint *b, sp_int base);
int sp_bigint_even_p(sp_Bigint *b);
sp_Bigint *sp_bigint_abs_v(sp_Bigint *b);
sp_int sp_bigint_bit_length(sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
double sp_bigint_to_double(sp_Bigint *b);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_gcd(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_lcm(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);   /* sp_box_brat reduces via div */
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
sp_Bigint *sp_bigint_round_prec(sp_Bigint *b, int64_t ndigits, int mode);
sp_Bigint *sp_bigint_isqrt(sp_Bigint *a);
sp_int sp_bigint_digits_buf(sp_Bigint *a, sp_int base, sp_int **out);
int sp_bigint_sign(sp_Bigint *b);
size_t sp_bigint_byte_len(sp_Bigint *b);
size_t sp_bigint_to_le_bytes(sp_Bigint *b, unsigned char *out, size_t cap);
sp_Bigint *sp_bigint_from_le_bytes(int negative, const unsigned char *bytes, size_t n);
/* Ruby integer shifts: a negative count shifts the other way, and a count at or
   beyond the word width saturates (arithmetic for a right shift of a negative).
   A bare C shift by a negative or >= width count is undefined behaviour, so any
   non-constant / possibly-negative shift routes through these. (A left shift
   past the word width truly overflows to a Bignum in Ruby; int mode can't hold
   it, so it saturates to 0 -- the Bignum-promotion path handles the real case.) */
static inline sp_int sp_int_shl(sp_int a, sp_int n) {
  if (n < 0) { sp_int s = -n; return s >= 64 ? (a < 0 ? -1 : 0) : (a >> s); }
#ifdef SP_INT_OVERFLOW_MODE_WRAP
  return n >= 64 ? 0 : (sp_int)((uintptr_t)a << n);
#else
  /* Ruby promotes to Bignum here; under raise mode that is an overflow, and a
     result of SP_INT_NIL (INTPTR_MIN) is unrepresentable even when the shift
     itself fits (it aliases the tagged nil sentinel). Shift in unsigned space:
     a signed shift into the sign bit is C UB. */
  if (n >= 64) {
    if (a != 0) sp_raise_cls("RangeError", "integer overflow in <<");
    return 0;
  }
  sp_int r = (sp_int)((uintptr_t)a << n);
  if ((r >> n) != a || r == SP_INT_NIL) sp_raise_cls("RangeError", "integer overflow in <<");
  return r;
#endif
}
static inline sp_int sp_int_shr(sp_int a, sp_int n) {
  if (n < 0) { sp_int s = -n; return s >= 64 ? 0 : (a << s); }
  return n >= 64 ? (a < 0 ? -1 : 0) : (a >> n);
}
/* A class known only by name (an exception's cls_name -- the id table covers
   only a few exception classes, but the name is complete for all of them,
   including the open-ended Errno:: family). Marked with a sentinel cls_id so
   the SP_TAG_CLASS arms read the name from v.s instead of resolving v.i. The
   name points into rodata (see sp_exc_gc_scan), so no GC marking is needed.
   SP_CLASS_BY_NAME itself now lives in sp_types.h (needed by sp_box_class_name
   in sp_alloc.h). */
/* A poly value as a sp_Bigint*, so the bigint comparison/arith helpers can take
   a mixed operand uniformly. Total (never NULL, since sp_bigint_cmp derefs):
   a float truncates (harmless -- a bignum is always outside float-fraction
   range) and a non-numeric collapses to 0 (a type mismatch Ruby would raise
   on; Spinel yields a defined result instead of crashing). */
/* fwd: the boxed division family below needs these, and their declarations
   come later in this header. */
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_remainder(sp_Bigint *a, sp_Bigint *b);
static sp_Bigint *sp_poly_as_bigint(sp_RbVal v) {
  if (v.tag == SP_TAG_BIGINT) return (sp_Bigint *)v.v.p;
  if (v.tag == SP_TAG_INT) return sp_bigint_new_int(v.v.i);
  if (v.tag == SP_TAG_FLT) return sp_bigint_new_int((int64_t)v.v.f);
  return sp_bigint_new_int(0);
}

/* every non-value-type sp_<C> starts
   with `sp_int cls_id`. Read it back from a void* when
   the static type at the call site has lost the cls_id (e.g.
   sp_PtrArray_get returning void*). NULL-safe via the guard
   on p; expects p to actually point at a user-class struct
   when non-NULL. */
static inline sp_int sp_obj_cls_id_of(void *p) { return p ? *(sp_int *)p : 0; }
/* int? siblings of the String#index family. Same not-found
   semantics as the _poly variants, but the result is the int?
   sentinel (SP_INT_NIL) rather than a boxed sp_RbVal — keeps the
   `i = s.index(sub); return ... if i.nil?; i + 1` idiom on the
   direct integer arithmetic path. */
/* `s.index(regex)` -- first match start (byte offset, as sp_re_match reports;
   matches the rindex regex variant, which also reports bytes). sp_re_match
   sets $~ as a side effect, which CRuby's String#index(regex) also does.
   Boxed Integer | nil. The plain-string path would lower the regex pattern to
   0 and feed a bogus arg to sp_str_index_opt. */
/* CRuby-compatible =~ wrapper: SP_TAG_INT(pos) on match, SP_TAG_NIL
   on miss. Codegen routes the `=~` operator here so
   `("abc" =~ /xyz/).nil?` answers true and `puts("abc" =~ /xyz/)`
   prints an empty line, matching CRuby. The raw `sp_re_match`
   (returning -1) stays available for internal callers needing the
   sentinel form. */

/* renderers live in sp_re.c; declared through void* to avoid the engine header */
const char *sp_re_inspect_str(void *pat);
const char *sp_re_to_s_str(void *pat);
const char *sp_re_source(void *pat);
uint32_t sp_re_source_len(void *pat);
/* Regexp#source as a spinel string. pat->source is a plain malloc'd C buffer,
   so handing it to the Ruby side directly ends the value at an embedded NUL:
   the length comes from the pattern instead. */
static const char *sp_re_source_str(void *pat) {
  const char *s = sp_re_source(pat);
  return s ? sp_str_from_bytes(s, (size_t)sp_re_source_len(pat)) : sp_str_empty;
}
sp_int sp_re_options(void *pat);
sp_bool sp_re_eq(void *a, void *b);
extern SP_TLS const mrb_regexp_pattern *sp_re_last_pat;
sp_bool sp_re_casefold_p(void *pat);
uint32_t sp_re_raw_flags(void *pat);
uint32_t sp_re_opts_to_flags(sp_int o);
#ifdef SPINEL_EXT_HOST
sp_RbVal sp_box_proc(void *p);
#else
sp_RbVal sp_box_proc(void *p)        { return sp_box_obj(p, SP_BUILTIN_PROC); }
#endif

/* CRuby-compatible Array#index / #rindex / #find_index: returns
   sp_RbVal (nil tag for not-found, int tag with the position when
   found). spinel's raw `_index` helpers return the -1 sentinel,
   which diverges from CRuby's nil. Codegen routes
   `arr.index(x)` / `arr.find_index(x)` / `arr.rindex(x)` through
   these `_poly` wrappers so `.nil?` / `== nil` / `inspect` etc.
   on the result behave the CRuby way. Sibling to
   `sp_str_index_poly` above; same widening rationale.
   Issue raised during #585 follow-up: spinel positions itself
   as a Ruby SUBSET, so documented Ruby APIs must match CRuby
   behavior. */
/* int? siblings of the *_index_poly wrappers above. Same not-found
   semantics, but return the int? sentinel (SP_INT_NIL) instead of
   boxing into sp_RbVal. Used when the call site's static type
   tracking carries the result as int? rather than poly — eliminates
   the box/unbox round-trip for the common `i = arr.index(x);
   i.nil? ? ... : <use i as int>` idiom. */
/* #step(n) over a Float range -> a Float array toward last. A negative step walks
   descending; a wrong-direction step yields an empty array; the count is derived
   so accumulated float rounding does not drift. */
sp_FloatArray *sp_frange_step(sp_FloatRange r, sp_float st) __attribute__((unused));
/* sp_frange_step: moved to lib/sp_cold.c */
sp_FloatArray *sp_frange_step(sp_FloatRange r, sp_float st);

/* Big Rational: a Rational whose numerator/denominator do not fit sp_int, so
   it holds two sp_Bigint* instead of the by-value int Rational (#2469). The two
   representations coexist -- an int Rational stays the fast value type, a big
   Rational is a boxed object that flows through the poly paths. */
/* real ** complex = exp(e * clog(base)): base>0 uses a real log, base<0 the
   principal branch (ln|base| + i*pi), base==0 is 0. Both result components are
   Float-classed (fl = 3). */
static sp_Complex sp_real_pow_complex(sp_float base, sp_Complex e) {
  double lr, li;
  if (base > 0)      { lr = log(base);  li = 0; }
  else if (base < 0) { lr = log(-base); li = M_PI; }
  else               { return (sp_Complex){0, 0, 3}; }
  double wr = e.re * lr - e.im * li;
  double wi = e.re * li + e.im * lr;
  double m = exp(wr);
  return (sp_Complex){ m * cos(wi), m * sin(wi), 3 };
}
/* sp_Range_inspect moved to lib/sp_format.c (cold). */
/* sp_Time_inspect moved to lib/sp_format.c (cold). */
#if defined(SPINEL_EXT_HOST) || defined(SPINEL_EXT_KERNEL)
const char *sp_class_to_s(sp_Class c);
#else
static const char *sp_class_to_s(sp_Class c); /* fwd decl: sp_poly_puts' SP_TAG_CLASS arm */
#endif
/* Name of a boxed SP_TAG_CLASS value: a name-backed box carries it in v.s,
   otherwise resolve the cls_id through the generated id->name table. */
static inline const char *sp_class_val_name(sp_RbVal v) {
  if (v.cls_id == SP_CLASS_BY_NAME) return v.v.s ? v.v.s : "";
  sp_Class _c = {v.v.i, NULL};
  return sp_class_to_s(_c);
}
/* Class identity: a name-backed class compares by its (complete) name, so it
   equals the id-backed class of the same name. */
static inline sp_bool sp_class_eq(sp_Class a, sp_Class b) {
  if (!a.name && !b.name) return a.cls_id == b.cls_id;
  const char *an = sp_class_to_s(a), *bn = sp_class_to_s(b);
  return (an && bn) ? strcmp(an, bn) == 0 : an == bn;
}
static inline const char *sp_poly_to_s(sp_RbVal v);   /* defined below; used by the user-object arm */
static inline sp_File *sp_poly_to_file(sp_RbVal v); /* defined below; IO.select's user-#to_io unwrap */
static inline void sp_poly_puts(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '\n') putchar('\n'); }
    else putchar('\n'); break;
    case SP_TAG_FLT: { fputs(sp_float_to_s(v.v.f), stdout); putchar('\n'); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    case SP_TAG_ENCODING: { const char *_es = v.v.s ? v.v.s : sp_str_empty; fputs(_es, stdout); putchar('\n'); break; }
    case SP_TAG_CLASS: { fputs(sp_class_val_name(v), stdout); putchar('\n'); break; }
    case SP_TAG_BIGINT: { const char *_bs = sp_bigint_to_s((sp_Bigint *)v.v.p); if (_bs) fputs(_bs, stdout); putchar('\n'); break; }
    case SP_TAG_OBJ: {
      /* MRI's `puts arr` iterates an Array, printing one element per
         line (using to_s on each); a non-Array OBJ falls back to
         inspect / class-name. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++)
            printf("%lld\n", (long long)_a->data[_a->start + _i]);
          break;
        }
        case SP_BUILTIN_FLT_ARRAY: {
          sp_FloatArray *_a = (sp_FloatArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            fputs(sp_float_to_s(_a->data[_i]), stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_STR_ARRAY: {
          sp_StrArray *_a = (sp_StrArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            const char *_s = _a->data[_i];
            if (_s) { fputs(_s, stdout); if (!*_s || _s[strlen(_s)-1] != '\n') putchar('\n'); }
            else putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_SYM_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            const char *_s = sp_sym_to_s((sp_sym)_a->data[_a->start + _i]);
            fputs(_s, stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_RANGE: puts(sp_Range_inspect((sp_Range *)v.v.p)); break;
        case SP_BUILTIN_FLOAT_RANGE: puts(sp_frange_inspect(*(sp_FloatRange *)v.v.p)); break;
        case SP_BUILTIN_TIME: puts(sp_Time_to_s((sp_Time *)v.v.p)); break;
        case SP_BUILTIN_STRBUF: puts(sp_String_cstr((sp_String *)v.v.p)); break;
        case SP_BUILTIN_COMPLEX: puts(sp_complex_to_s(*(sp_Complex *)v.v.p)); break;
        case SP_BUILTIN_RATIONAL: puts(sp_rational_to_s(*(sp_Rational *)v.v.p)); break;
        case SP_BUILTIN_BIG_RATIONAL: puts(sp_brat_to_s((sp_BigRational *)v.v.p)); break;
        case SP_BUILTIN_REGEX: puts(sp_re_to_s_str(v.v.p)); break;
        case SP_BUILTIN_POLY_ARRAY: {
          /* puts flattens arrays recursively, one element per line -- and one
             that holds itself gets CRuby's single `[...]` line instead of an
             endless flattening. */
          sp_PolyArray *_a = (sp_PolyArray *)v.v.p;
          if (sp_poly_recur_seen(SP_POLY_RECUR_PUTS, _a, NULL)) { puts("[...]"); break; }
          int _pm = sp_poly_recur_push(SP_POLY_RECUR_PUTS, _a, NULL);
          for (sp_int _i = 0; _i < _a->len; _i++) sp_poly_puts(_a->data[_i]);
          sp_poly_recur_pop(_pm);
          break;
        }
        /* A user object (or any non-array OBJ) prints via to_s: delegate to
           sp_poly_to_s, which dispatches the class's user #to_s (falling back to
           the default #<Name:0x..>). The bare #<Object> print skipped the user
           to_s for an object read from a collection (#3189). */
        default: { fputs(sp_poly_to_s(v), stdout); putchar('\n'); break; }
      }
      break;
    }
    default: printf("%lld\n", (long long)v.v.i); break;
  }
}
static sp_bool sp_poly_nil_p(sp_RbVal v) { return v.tag == SP_TAG_NIL; }
static sp_bool sp_poly_truthy(sp_RbVal v) { return !(v.tag == SP_TAG_NIL || (v.tag == SP_TAG_BOOL && !v.v.b)); }
/* Regexp.new's option argument where its type is not known until run time: an
   Integer is option bits, and anything else truthy is IGNORECASE. CRuby makes
   that choice from the VALUE, so a caller that cannot see the type statically
   has to make it here rather than settling on the truthy arm -- which read
   every Integer, 0 included, as IGNORECASE. A nil out of an Integer slot is
   the sentinel, and nil is no options at all. */
/* ...and as flag LETTERS, which is what a String argument carries: "mix" in
   any order and any repetition, and nothing else. An unknown letter is an
   ArgumentError naming it, as in CRuby, where `n`, `u` and `o` are unknown
   too. The empty string is no options. */
static uint32_t sp_re_opts_from_str(const char *s) {
  uint32_t f = 0;
  if (!s) return 0u;
  for (const char *p = s; *p; p++) {
    switch (*p) {
    case 'm': f |= 4u; break;   /* the PUBLIC bits, translated below */
    case 'i': f |= 1u; break;
    case 'x': f |= 2u; break;
    default:
      sp_raise_cls("ArgumentError",
                   sp_sprintf("unknown regexp option: %c", *p));
    }
  }
  return sp_re_opts_to_flags((sp_int)f);
}

static uint32_t sp_re_opts_from_poly(sp_RbVal v) {
  if (v.tag == SP_TAG_INT) return v.v.i == SP_INT_NIL ? 0u : sp_re_opts_to_flags(v.v.i);
  if (v.tag == SP_TAG_STR) return sp_re_opts_from_str(v.v.s);
  return sp_poly_truthy(v) ? 1u : 0u;
}
/* poly & / | / ^ dispatch on the receiver's runtime tag: nil/false/true take
   the BOOLEAN ops (nil & x == false, nil | x == truthy(x), ...), integers take
   the bitwise ops (#2401). */
static sp_int sp_poly_to_i(sp_RbVal v);  /* defined below */
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);   /* fwd: bignum bitops */
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
static sp_Bigint *sp_poly_as_bigint(sp_RbVal v);
static sp_RbVal sp_poly_binop_bad(const char *op, sp_RbVal recv, sp_RbVal arg);  /* fwd */
static inline int sp_poly_is_array_kind(int cls_id);            /* fwd: array set ops */
static sp_PolyArray *sp_poly_to_poly_array(sp_RbVal v);
/* `zip`'s arguments must respond to :each -- an Array, a Range, any Enumerable
   -- and CRuby names the class of one that does not. The emitters read the
   argument as a container regardless, so a nil quietly became a column of nils
   and an Integer stopped the C build. */
static sp_PolyArray *sp_zip_arg(sp_RbVal v);
static sp_PolyArray *sp_PolyArray_intersect(sp_PolyArray *a, sp_PolyArray *b);
static sp_PolyArray *sp_PolyArray_union(sp_PolyArray *a, sp_PolyArray *b);
static sp_RbVal sp_poly_bitop(sp_RbVal a, sp_RbVal b, int op) {  /* 0:& 1:| 2:^ */
  /* a user class's own &/|/^ comes first: coercing the object to an integer
     answered a builtin result for a method the class had written (#3501) */
  if (a.tag == SP_TAG_OBJ && a.cls_id >= 0)
    return sp_poly_binop_bad(op == 0 ? "&" : op == 1 ? "|" : "^", a, b);
  if (a.tag == SP_TAG_NIL || a.tag == SP_TAG_BOOL) {
    sp_bool av = sp_poly_truthy(a), bv = sp_poly_truthy(b);
    return sp_box_bool(op == 0 ? (av && bv) : op == 1 ? (av || bv) : (av != bv));
  }
  /* Two ARRAYS: `&` and `|` are Array's set operations, as `+` and `-` already
     are on this path. Read as integer arithmetic they answered 0, so a fold
     over an array of arrays lost its result silently (#3966). */
  if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id) &&
      b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id) && op != 2) {
    SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
    sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa);
    sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb);
    return sp_box_poly_array(op == 0 ? sp_PolyArray_intersect(pa, pb)
                                     : sp_PolyArray_union(pa, pb));
  }
  /* Integer's own reading of the operator, and Integer's alone: a Float,
     Rational or Complex receiver has no `&`, `|` or `^` in Ruby at all, and
     reading it as an integer answered a bitwise result for the truncated
     value. This has to come BEFORE the bignum arm below, which converts BOTH
     sides with sp_poly_as_bigint -- a Bignum on either side otherwise carried
     a Float or a String receiver into it as a zero. Everything with a meaning
     of its own for the operator was answered above. */
  if (a.tag != SP_TAG_INT && a.tag != SP_TAG_BIGINT)
    return sp_poly_binop_bad(op == 0 ? "&" : op == 1 ? "|" : "^", a, b);
  /* A bignum operand keeps its width: truncating both sides to int64 first
     turned `x ^ (x << 17)` into a negative int once the shift had promoted,
     which is how a masked xorshift diverged from CRuby for good (#3371). `&`
     is the exception the other way -- masking a bignum with a 64-bit constant
     is the truncation idiom, and its result fits an int by construction, so it
     still goes through the bignum path and lands back in range. */
  if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) {
    sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b);
    if (ba && bb) {
      sp_Bigint *r = op == 0 ? sp_bigint_and(ba, bb)
                   : op == 1 ? sp_bigint_or(ba, bb) : sp_bigint_xor(ba, bb);
      return sp_box_bigint(r);
    }
  }
  sp_int ai = sp_poly_to_i(a), bi = sp_poly_to_i(b);
  return sp_box_int(op == 0 ? (ai & bi) : op == 1 ? (ai | bi) : (ai ^ bi));
}
/* forward-declare the program-emitted class
   name lookup so sp_poly_to_s's SP_TAG_CLASS arm resolves.
   The codegen emits a 1-line stub when no class const is used,
   or the real body when @needs_class_table fires. The forward
   decl always needs a definition somewhere because -Werror
   trips on "used but never defined" otherwise. */
#if !defined(SPINEL_EXT_HOST) && !defined(SPINEL_EXT_KERNEL)
static const char *sp_class_to_s(sp_Class c);
#endif
static const char *sp_poly_class_name(sp_RbVal v);  /* fwd: user-object to_s default */
static const char *sp_convert_src_name(sp_RbVal v);  /* fwd: nil/true/false spell themselves */
static sp_int sp_poly_Integer_ex(sp_RbVal v, sp_int base, int raise);  /* fwd: Kernel#Integer / #Float on a user object */
static sp_float sp_poly_Float_ex(sp_RbVal v, int raise);
static inline int sp_poly_is_hash_kind(int cls_id);
static inline const char *sp_poly_inspect(sp_RbVal v);
static const char *sp_PolyArray_inspect(sp_PolyArray *a);  /* fwd: Array#to_s == inspect */
static inline const char *sp_poly_to_s(sp_RbVal v) {
  switch (v.tag) {
    /* int-typed nil (SP_INT_NIL) is Ruby nil; nil.to_s is "" -- match it. */
    case SP_TAG_INT: return v.v.i == SP_INT_NIL ? sp_str_empty : sp_int_to_s(v.v.i);
    case SP_TAG_STR: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_FLT: return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL: return sp_str_empty;
    case SP_TAG_SYM: return sp_sym_to_s((sp_sym)v.v.i);
    case SP_TAG_CLASS: return sp_class_val_name(v);
    case SP_TAG_ENCODING: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        /* Array#to_s is Array#inspect; a boxed PolyArray element had no arm
           and fell through to "" (#3007) */
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE: return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_FLOAT_RANGE: return sp_frange_inspect(*(sp_FloatRange *)v.v.p);
        case SP_BUILTIN_TIME: return sp_Time_to_s((sp_Time *)v.v.p);
        case SP_BUILTIN_STRBUF: return sp_String_cstr((sp_String *)v.v.p);   /* live buffer (#3227) */
        case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
        case SP_BUILTIN_COMPLEX: return sp_complex_to_s(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL: return sp_rational_to_s(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_BIG_RATIONAL: return sp_brat_to_s((sp_BigRational *)v.v.p);
        case SP_BUILTIN_REGEX: return sp_re_to_s_str(v.v.p);
        /* MatchData#to_s is the whole match (#3641) */
        case SP_BUILTIN_MATCHDATA: { const char *_m0 = sp_MatchData_aref((sp_MatchData *)v.v.p, 0); return _m0 ? _m0 : sp_str_empty; }
        case SP_BUILTIN_EXCEPTION: return sp_exc_message((volatile struct sp_Exception_s *)v.v.p);
        /* Object#to_s on a boxed handle of the IO family, as the typed arm
           renders it: a boxed one fell through to "" (`puts [f, d]`) */
        case SP_BUILTIN_IO: return sp_io_to_s((sp_File *)v.v.p);
        case SP_BUILTIN_DIR: return sp_Dir_to_s((sp_Dir *)v.v.p);
        default:
          if ((v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT) && v.v.p) {
            /* a class with a user #to_s renders through the generated
               dispatcher; the rest (bare Object.new included) get CRuby's
               default #<Name:0xADDR> */
            if (v.cls_id >= 0 && sp_obj_to_s_fn) {
              const char *us = sp_obj_to_s_fn(v.cls_id, v.v.p);
              if (us) return us;
            }
            return sp_sprintf("#<%s:0x%016llx>", sp_poly_class_name(v),
                              (unsigned long long)(uintptr_t)v.v.p);
          }
          /* a builtin container kind with no explicit arm above (the hash
             variants): to_s is its inspect */
          if (sp_poly_is_hash_kind(v.cls_id)) return sp_poly_inspect(v);
          return sp_str_empty;
      }
    default: return sp_str_empty;
  }
}
/* Unwrap a boxed IO value to its sp_File *. The user-#to_io dispatch
   asks this for a poly return type, because the generated arm is forced
   to box the answer through sp_RbVal (the codegen of a poly body has
   no other channel). CRuby's IO.select only ever sees the IO itself;
   the protocol is: a user class with #to_io is fine, anything else
   is a TypeError raised by the caller of the hook (it never reaches
   this function). A builtin IO is a sp_File directly; a boxed user
   class is a pointer the user-defined #to_io just returned, and the
   dispatch already vetted it. NULL is the "not an IO" answer the
   dispatch turns into the caller's TypeError. */
/* IO#write of an operand whose class is only known at run time. A String
   writes through the binary entry so an embedded NUL survives; anything else
   stringifies through the plain one, which it must -- a static class or symbol
   name has no marker byte, and the _bin entry would read s[-1] out of bounds.
   The typed-receiver arm chose by the operand's STATIC type, so a poly operand
   took the plain entry and a NUL truncated the write; the poly-receiver arm
   already tested the tag, and now both do. */
static SP_INLINE sp_int sp_File_write_poly(sp_File *f, sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return sp_File_write_bin(f, v.v.s);
  return sp_File_write(f, sp_poly_to_s(v));
}
static inline sp_File *sp_poly_to_file(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_IO) return (sp_File *)v.v.p;
  return NULL;
}
/* Class name of a boxed value, for `x.class` where x is poly. Returns a
   .rodata or names-table string (never GC-managed). */
static const char *sp_poly_class_name(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: return SPL("Integer");
    case SP_TAG_STR: return SPL("String");
    case SP_TAG_FLT: return SPL("Float");
    case SP_TAG_BOOL: return v.v.b ? SPL("TrueClass") : SPL("FalseClass");
    case SP_TAG_NIL: return SPL("NilClass");
    case SP_TAG_SYM: return SPL("Symbol");
    case SP_TAG_ENCODING: return SPL("Encoding");
    case SP_TAG_CLASS: return SPL("Class");
    case SP_TAG_BIGINT: return SPL("Integer");
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
        case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
        case SP_BUILTIN_PTR_ARRAY: case SP_BUILTIN_POLY_ARRAY: return SPL("Array");
        case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
        case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_SYM_INT_HASH:
         case SP_BUILTIN_INT_INT_HASH:
        case SP_BUILTIN_SYM_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
        case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH: return SPL("Hash");
        case SP_BUILTIN_RANGE: return SPL("Range");
        case SP_BUILTIN_FLOAT_RANGE: case SP_BUILTIN_STR_RANGE: return SPL("Range");
        case SP_BUILTIN_TIME: return SPL("Time");
    case SP_BUILTIN_STRBUF: return SPL("String");   /* (#3227) */
        case SP_BUILTIN_COMPLEX: return SPL("Complex");
        case SP_BUILTIN_RATIONAL: return SPL("Rational");
        case SP_BUILTIN_BIG_RATIONAL: return SPL("Rational");
        case SP_BUILTIN_REGEX: return SPL("Regexp");
        case SP_BUILTIN_MATCHDATA: return SPL("MatchData");   /* (#3641) */
        case SP_BUILTIN_OBJECT: return SPL("Object");   /* a bare Object.new instance */
        case SP_BUILTIN_BASIC_OBJECT: return SPL("BasicObject");
        case SP_BUILTIN_PROC: return SPL("Proc");
        /* a curried proc IS a Proc to Ruby (#3885) */
        case SP_BUILTIN_CURRY: return SPL("Proc");
        case SP_BUILTIN_METHOD:
          return ((sp_BoundMethod *)v.v.p)->unbound ? SPL("UnboundMethod") : SPL("Method");   /* (#3692) */
        case SP_BUILTIN_ENUMERATOR: return SPL("Enumerator");
        case SP_BUILTIN_IO: {
          /* the handle kind names the class, through the same authority the
             typed .class emit uses -- a boxed socket must not report plain IO */
          sp_File *_iof = (sp_File *)v.v.p;
          if (_iof && _iof->mode &&
              (strcmp(_iof->mode, "stat") == 0 || strcmp(_iof->mode, "lstat") == 0))
            return SPL("File::Stat");
          return sp_io_kind_name(_iof);
        }
        /* The concurrency handles: a boxed one reached the default arm and
           answered an empty name, so `(:ok && queue).class` printed nothing
           and a method on it raised NoMethodError with a blank class (#3484).
           Mutex and Queue name themselves through the same helpers the typed
           `.class` emit uses, so a Monitor and a SizedQueue stay distinct. */
        case SP_BUILTIN_MUTEX: return sp_Mutex_class_name((sp_mutex *)v.v.p);
        case SP_BUILTIN_QUEUE: return sp_Queue_class_name((sp_queue *)v.v.p);
        case SP_BUILTIN_CONDVAR: return SPL("Thread::ConditionVariable");
        case SP_BUILTIN_FIBER: return SPL("Fiber");
        case SP_BUILTIN_THREAD: return SPL("Thread");
        case SP_BUILTIN_TMS: return SPL("Process::Tms");
        case SP_BUILTIN_OPENSTRUCT: return SPL("OpenStruct");
        /* These two had no arm of their own. They went unnoticed because their
           cls_ids collided with STRBUF and OPENSTRUCT, so a boxed one answered
           String / OpenStruct rather than nothing (#4158). */
        case SP_BUILTIN_ADDRINFO: return SPL("Addrinfo");
        case SP_BUILTIN_SOCKOPT: return SPL("Socket::Option");
        case SP_BUILTIN_PROCESS_STATUS: return SPL("Process::Status");
        case SP_BUILTIN_EXCEPTION: return sp_exc_class_name((volatile struct sp_Exception_s *)v.v.p);
        default: { sp_Class c = {v.cls_id}; return sp_class_to_s(c); }
      }
    default: return SPL("Object");
  }
}
static sp_bool sp_poly_responds_builtin(sp_RbVal v, const char *m) {
  static const char *const uni[] = {
    "to_s", "inspect", "class", "nil?", "dup", "clone", "freeze", "frozen?",
    "hash", "==", "!=", "equal?", "eql?", "object_id", "respond_to?", "is_a?",
    "kind_of?", "instance_of?", "itself", "tap", "then", "send", "__send__",
    "public_send", "method", "methods", "display", "yield_self", NULL };
  static const char *const enumm[] = {
    "each", "each_with_index", "each_with_object", "map", "collect", "select",
    "filter", "reject", "find", "detect", "reduce", "inject", "to_a", "size",
    "length", "count", "include?", "first", "min", "max", "sort", "sort_by",
    "sum", "any?", "all?", "none?", "one?", "group_by", "partition", "zip",
    "flat_map", "each_slice", "each_cons", "take", "drop", "empty?", NULL };
  static const char *const arrm[] = {
    "[]", "[]=", "push", "<<", "pop", "shift", "unshift", "concat", "join",
    "flatten", "compact", "uniq", "reverse", "last", "index", "delete",
    "delete_at", "delete_if", "insert", "fetch", "sample", "shuffle",
    "rotate", "slice", "fill", "dig", "+", "-", "*", "&", "|", NULL };
  static const char *const hashm[] = {
    "[]", "[]=", "keys", "values", "fetch", "store", "delete", "key?",
    "has_key?", "member?", "value?", "has_value?", "each_pair", "each_key",
    "each_value", "merge", "merge!", "update", "to_h", "invert", "dig",
    "default", "key", "transform_keys", "transform_values", NULL };
  static const char *const strm[] = {
    "[]", "[]=", "+", "*", "%", "<=>", "<", ">", "<=", ">=", "=~", "length",
    "size", "empty?", "upcase", "downcase", "capitalize", "swapcase", "strip",
    "lstrip", "rstrip", "chomp", "chop", "chars", "bytes", "lines", "split",
    "sub", "gsub", "sub!", "gsub!", "index", "rindex", "include?",
    "start_with?", "end_with?", "replace", "concat", "<<", "reverse", "succ",
    "next", "to_i", "to_f", "to_sym", "to_str", "center", "ljust", "rjust",
    "tr", "delete", "squeeze", "count", "each_char", "each_line", "slice",
    "unpack", "encoding", "force_encoding", "bytesize", "ord", "hex", "oct",
    "match", "match?", "scan", "format", "freeze", NULL };
  static const char *const numm[] = {
    "+", "-", "*", "/", "%", "**", "<=>", "<", ">", "<=", ">=", "abs",
    "to_i", "to_int", "to_f", "to_r", "to_c", "zero?", "positive?",
    "negative?", "coerce", "divmod", "fdiv", "round", "ceil", "floor",
    "truncate", "between?", "clamp", "step", NULL };
  static const char *const intm[] = {
    "times", "upto", "downto", "succ", "next", "pred", "even?", "odd?",
    "gcd", "lcm", "digits", "bit_length", "chr", "ord", "pow", "&", "|",
    "^", "<<", ">>", "~", "integer?", NULL };
  static const char *const fltm[] = {
    "nan?", "infinite?", "finite?", "integer?", NULL };
  static const char *const rngm[] = {
    "begin", "end", "first", "last", "min", "max", "step", "cover?",
    "exclude_end?", "to_a", "each", "size", "sum", "include?", "===", NULL };
  static const char *const symm[] = {
    "to_proc", "to_sym", "id2name", "name", "length", "size", "succ", "next",
    "upcase", "downcase", "capitalize", "swapcase", "empty?", "start_with?",
    "end_with?", "<=>", "[]", NULL };
  static const char *const procm[] = {
    "call", "()", "[]", "yield", "arity", "lambda?", "curry", "to_proc",
    "parameters", "<<", ">>", NULL };
  static const char *const excm[] = {
    "message", "to_s", "full_message", "backtrace", "cause", "exception",
    "name", "key", "receiver", "result", NULL };
  const char *cn;
  if (!m) return 0;
  if (sp_str_in_list(m, uni)) return 1;
  cn = sp_poly_class_name(v);
  if (!cn) return 0;
  if (strcmp(cn, "Array") == 0)
    return sp_str_in_list(m, enumm) || sp_str_in_list(m, arrm);
  if (strcmp(cn, "Hash") == 0)
    return sp_str_in_list(m, enumm) || sp_str_in_list(m, hashm);
  if (strcmp(cn, "String") == 0) return sp_str_in_list(m, strm);
  if (strcmp(cn, "Integer") == 0)
    return sp_str_in_list(m, numm) || sp_str_in_list(m, intm);
  if (strcmp(cn, "Float") == 0)
    return sp_str_in_list(m, numm) || sp_str_in_list(m, fltm);
  if (strcmp(cn, "Range") == 0)
    return sp_str_in_list(m, enumm) || sp_str_in_list(m, rngm);
  if (strcmp(cn, "Symbol") == 0) return sp_str_in_list(m, symm);
  if (strcmp(cn, "Proc") == 0) return sp_str_in_list(m, procm);
  /* nil answers a small surface of its own; the literal receiver folded it at
     compile time, so only a nil that arrived through a slot came here and got
     a flat false (#3815). */
  if (strcmp(cn, "NilClass") == 0) {
    static const char *const nilm[] = {
      "to_a", "to_h", "to_i", "to_f", "to_r", "to_c", "&", "|", "^",
      "inspect", "nil?", "instance_variables", NULL };
    return sp_str_in_list(m, nilm);
  }
  if (strcmp(cn, "TrueClass") == 0 || strcmp(cn, "FalseClass") == 0) {
    static const char *const boolm[] = { "&", "|", "^", "to_s", "inspect", NULL };
    return sp_str_in_list(m, boolm);
  }
  /* an Enumerator answers the Enumerable face (#3625) */
  if (strcmp(cn, "Enumerator") == 0) return sp_str_in_list(m, enumm);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_EXCEPTION)
    return sp_str_in_list(m, excm);
  return 0;
}
/* ---- socket support (#2922): thin layer over lib/sp_net.c + sp_io_fdopen_sock.
   A TCPServer/TCPSocket IS an sp_File whose ->mode labels the kind; every IO
   method (gets/read/write/puts/each_line/...) works unchanged, with writes
   bypassing stdio (see sp_sock_write in lib/sp_io.c). */
int sp_net_listen_host(const char *host, int port, int backlog);
int sp_net_connect(const char *host, int port);
int sp_net_accept(int sfd);
int sp_net_sock_ip(int fd, int peer, char *ipbuf, int cap);
/* TCPSocket#addr / #peeraddr: ["AF_INET", port, ip, ip], CRuby's numeric form.
   These belong to the socket classes, not to IO: a plain File answers
   NoMethodError, as CRuby does, rather than an empty address. */
static sp_PolyArray *sp_sock_addr(sp_File *f, int peer) __attribute__((unused));
static sp_PolyArray *sp_sock_addr(sp_File *f, int peer) {
  if (!f || !f->is_sock)
    sp_raise_cls("NoMethodError", sp_sprintf("undefined method '%s' for an instance of %s",
                                             peer ? "peeraddr" : "addr", sp_io_kind_name(f)));
  SP_IO_OPEN(f);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  char ip[64];
  int port = sp_net_sock_ip(fileno(f->fp), peer, ip, (int)sizeof ip);
  if (port < 0) { ip[0] = '\0'; port = 0; }
  const char *fam = strchr(ip, ':') ? "AF_INET6" : "AF_INET";
  sp_PolyArray_push(a, sp_box_str(sp_sprintf("%s", fam)));
  sp_PolyArray_push(a, sp_box_int((sp_int)port));
  const char *ips = sp_sprintf("%s", ip);
  sp_PolyArray_push(a, sp_box_str(ips));
  sp_PolyArray_push(a, sp_box_str(ips));
  return a;
}
/* Process.times -> Process::Tms: four cumulative CPU times in seconds.
   An unboxed value like sp_Range/sp_Class -- there is nothing to mutate. */
static sp_Tms sp_process_times(void) {
  sp_Tms t;
  struct tms b;
  double hz = (double)sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100.0;
  if (times(&b) == (clock_t)-1) { t.utime = t.stime = t.cutime = t.cstime = 0.0; return t; }
  t.utime  = (sp_float)b.tms_utime  / hz;
  t.stime  = (sp_float)b.tms_stime  / hz;
  t.cutime = (sp_float)b.tms_cutime / hz;
  t.cstime = (sp_float)b.tms_cstime / hz;
  return t;
}
/* Class/Module#freeze / #frozen?: a class value is an unboxed {cls_id, name},
   so the frozen flag lives in a global per-class map -- user ids from 0 up,
   builtins (-100..-163) mapped to the top of the range (#3101). */
/* .class as a first-class value: name-backed for every receiver kind, so it
   compares via sp_class_eq (name identity) and prints via sp_class_to_s. */
static sp_Class sp_poly_class_val(sp_RbVal v) {
  sp_Class r;
  /* Carry the real cls_id for a user object: a dispatch that switches on it --
     `s.class.new(**h)` -- could never match an arm while this answered -1, so
     the switch fell through and the call returned nil (#4020). The name still
     leads for display; sp_class_to_s reads it first. */
  r.cls_id = (v.tag == SP_TAG_OBJ && v.cls_id >= 0) ? (int)v.cls_id : -1;
  r.name = sp_poly_class_name(v);
  return r;
}
/* Raise TypeError "no implicit conversion of <class> into String" for a poly
   value, naming its actual runtime class (the statically-typed path bakes the
   class name into a literal; the poly path resolves it here). */
SP_NORETURN SP_COLD static void sp_raise_no_str_conversion(sp_RbVal v) {
  static char buf[128];
  snprintf(buf, sizeof buf, "no implicit conversion of %s into String", sp_poly_class_name(v));
  sp_raise_cls("TypeError", buf);
}
/* ENV[key] = value with a boxed RHS: a String sets, nil deletes, anything
   else raises CRuby's TypeError. Returns the assigned string (NULL = nil),
   matching the statically-string path's expression type. */
static const char *sp_env_aset(const char *k, sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) { unsetenv(k); return NULL; }
  if (v.tag != SP_TAG_STR) sp_raise_no_str_conversion(v);
  if (v.v.s) setenv(k, v.v.s, 1); else unsetenv(k);
  return v.v.s;
}
static sp_bool sp_PolyArray_eq(sp_PolyArray *a, sp_PolyArray *b);
static sp_float sp_poly_to_f(sp_RbVal v);  /* defined below; used by the bigint+float arms */
static sp_int sp_poly_to_i(sp_RbVal v);    /* defined below; used by the rational helper */
/* A boxed Rational operand in poly arithmetic. An Integer promotes to n/1; a
   Float mix is handled by the dedicated Rational+Float arm, which coerces both
   sides through sp_poly_to_f_with_rational rather than consulting these. */
static inline int sp_poly_is_rational(sp_RbVal v) { return v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL; }
static inline sp_Rational sp_poly_as_rational(sp_RbVal v) {
  if (sp_poly_is_rational(v) && v.v.p) return *(sp_Rational *)v.v.p;
  return sp_rational_new(sp_poly_to_i(v), 1);
}
/* Kernel#Rational on a boxed argument: a Rational passes through exactly, a
   Float converts to its exact value the way Rational(2.5) does, a String
   parses, and anything else reads as an integer. */
static inline sp_Rational sp_poly_kernel_rational(sp_RbVal v) {
  if (sp_poly_is_rational(v) && v.v.p) return *(sp_Rational *)v.v.p;
  if (v.tag == SP_TAG_FLT) return sp_float_to_rational(v.v.f);
  if (v.tag == SP_TAG_STR) return sp_str_to_r(v.v.s ? v.v.s : sp_str_empty);
  return sp_rational_new(sp_poly_to_i(v), 1);
}
/* Unbox a boxed Complex (a real number becomes re+0i). Used to keep a Complex
   reduce accumulator typed when the block folds through the poly `+`. */
static inline sp_Complex sp_poly_as_complex(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX && v.v.p) return *(sp_Complex *)v.v.p;
  return (sp_Complex){sp_poly_to_f(v), 0.0, 0};
}
/* Coerce to a C double, understanding boxed Rational (sp_poly_to_f does not).
   Used by the Rational+Float arms, where CRuby yields a Float. */
static inline sp_float sp_poly_to_f_with_rational(sp_RbVal v) {
  if (sp_poly_is_rational(v) && v.v.p) return sp_rational_to_f(*(sp_Rational *)v.v.p);
  return sp_poly_to_f(v);
}
static inline int sp_poly_is_array_kind(int cls_id);                       /* defined below; used by the poly `+` array arm */
static sp_PolyArray *sp_poly_to_poly_array(sp_RbVal v);                    /* defined below */
static sp_PolyArray *sp_PolyArray_concat(sp_PolyArray *a, sp_PolyArray *b); /* defined below */
static sp_PolyArray *sp_PolyArray_difference(sp_PolyArray *a, sp_PolyArray *b); /* defined below */
static sp_PolyArray *sp_PolyArray_intersect(sp_PolyArray *a, sp_PolyArray *b);  /* defined below */
static sp_PolyArray *sp_PolyArray_union(sp_PolyArray *a, sp_PolyArray *b);      /* defined below */
/* int+int that auto-promotes to bigint on overflow in --int-overflow=promote;
   plain (wrapping) C arithmetic otherwise, matching the sp_int_* macro policy. */
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
#  define SP_POLY_INT_OP(op, x, y) ({ sp_int _r; sp_int_##op##_overflow_p((x), (y), &_r) \
     ? sp_box_bigint(sp_bigint_##op(sp_bigint_new_int(x), sp_bigint_new_int(y))) : sp_box_int(_r); })
#else
#  define SP_POLY_INT_OP(op, x, y) sp_box_int(sp_int_c_##op((x), (y)))
#endif
static inline sp_int sp_int_c_add(sp_int x, sp_int y) { return x + y; }
static inline sp_int sp_int_c_sub(sp_int x, sp_int y) { return x - y; }
static inline sp_int sp_int_c_mul(sp_int x, sp_int y) { return x * y; }
/* big Rational arithmetic (#2469): coerce every numeric operand to a num/den
   sp_Bigint* pair, run the cross-multiplied formula, and reduce via sp_box_brat.
   Used when one operand is already a big Rational. */
static inline int sp_poly_is_brat(sp_RbVal v) { return v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_BIG_RATIONAL; }
static void sp_poly_to_brat(sp_RbVal v, sp_Bigint **num, sp_Bigint **den) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_BIG_RATIONAL) { sp_BigRational *r = (sp_BigRational *)v.v.p; *num = r->num; *den = r->den; return; }
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL) { sp_Rational *r = (sp_Rational *)v.v.p; *num = sp_bigint_new_int(r->num); *den = sp_bigint_new_int(r->den); return; }
  if (v.tag == SP_TAG_BIGINT) { *num = (sp_Bigint *)v.v.p; *den = sp_bigint_new_int(1); return; }
  *num = sp_bigint_new_int(v.tag == SP_TAG_INT ? v.v.i : 0); *den = sp_bigint_new_int(1);
}
static sp_RbVal sp_brat_add_poly(sp_RbVal a, sp_RbVal b) { sp_Bigint *an,*ad,*bn,*bd; sp_poly_to_brat(a,&an,&ad); sp_poly_to_brat(b,&bn,&bd); return sp_box_brat(sp_bigint_add(sp_bigint_mul(an,bd), sp_bigint_mul(bn,ad)), sp_bigint_mul(ad,bd)); }
static sp_RbVal sp_brat_sub_poly(sp_RbVal a, sp_RbVal b) { sp_Bigint *an,*ad,*bn,*bd; sp_poly_to_brat(a,&an,&ad); sp_poly_to_brat(b,&bn,&bd); return sp_box_brat(sp_bigint_sub(sp_bigint_mul(an,bd), sp_bigint_mul(bn,ad)), sp_bigint_mul(ad,bd)); }
static sp_RbVal sp_brat_mul_poly(sp_RbVal a, sp_RbVal b) { sp_Bigint *an,*ad,*bn,*bd; sp_poly_to_brat(a,&an,&ad); sp_poly_to_brat(b,&bn,&bd); return sp_box_brat(sp_bigint_mul(an,bn), sp_bigint_mul(ad,bd)); }
static sp_RbVal sp_brat_div_poly(sp_RbVal a, sp_RbVal b) { sp_Bigint *an,*ad,*bn,*bd; sp_poly_to_brat(a,&an,&ad); sp_poly_to_brat(b,&bn,&bd); if (sp_bigint_sign(bn) == 0) sp_raise_cls("ZeroDivisionError", "divided by 0"); return sp_box_brat(sp_bigint_mul(an,bd), sp_bigint_mul(ad,bn)); }
static int sp_brat_cmp_poly(sp_RbVal a, sp_RbVal b) { sp_Bigint *an,*ad,*bn,*bd; sp_poly_to_brat(a,&an,&ad); sp_poly_to_brat(b,&bn,&bd); return sp_bigint_cmp(sp_bigint_mul(an,bd), sp_bigint_mul(bn,ad)); }
/* A failed boxed arithmetic dispatch must not manufacture Integer zero. Ruby
   distinguishes receivers with no such method from methods that reject the
   operand, so preserve that distinction at the dynamic fallback. */
/* The ops a Complex receiver takes through the coerce protocol. CRuby's
   Complex has `+ - * / ** <=> quo fdiv` and nothing else -- the ordered
   comparisons, % and the named division family raise NoMethodError with no
   coerce call, so admitting them here answered where CRuby raises. quo and
   fdiv are left out on the other grounds: their boxed entries have no
   Complex arm, so routing the pair through them answered a wrong number
   ((0/1), Infinity) where master's refusal at least failed loudly. */
static sp_bool sp_complex_coerce_op_p(const char *op) {
  static const char *const set[] = {
    "+", "-", "*", "/", "**", "<=>", NULL};
  for (int i = 0; set[i]; i++) {
    if (strcmp(op, set[i]) == 0) return TRUE;
  }
  return FALSE;
}
/* `& | ^ << >>` are Integer's alone: CRuby answers NoMethodError for a Float,
   Rational or Complex receiver, not the coercion TypeError the arithmetic four
   report. Array and String keep the ones they define. */
static sp_bool sp_poly_bitop_name_p(const char *op) {
  if (op[0] && !op[1]) return op[0] == '&' || op[0] == '|' || op[0] == '^';
  return strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
}
static sp_bool sp_poly_has_binop(sp_RbVal recv, const char *op) {
  if (sp_poly_bitop_name_p(op)) {
    if (recv.tag == SP_TAG_INT || recv.tag == SP_TAG_BIGINT) return TRUE;
    if (recv.tag == SP_TAG_STR) return strcmp(op, "<<") == 0;   /* String#<< appends */
    /* Array has `&`, `|` and `<<`, and none of `^ >> `. */
    return recv.tag == SP_TAG_OBJ && recv.v.p &&
           sp_poly_is_array_kind(recv.cls_id) &&
           (op[0] == '&' || op[0] == '|' || strcmp(op, "<<") == 0);
  }
  if (recv.tag == SP_TAG_INT || recv.tag == SP_TAG_FLT || recv.tag == SP_TAG_BIGINT)
    return TRUE;
  if (recv.tag == SP_TAG_STR)
    return strcmp(op, "+") == 0 || strcmp(op, "*") == 0;
  if (recv.tag == SP_TAG_OBJ && recv.v.p) {
    /* Time has `+` and `-`; whatever it still refuses is a coercion failure,
       not a missing method. */
    if (recv.cls_id == SP_BUILTIN_TIME)
      return strcmp(op, "+") == 0 || strcmp(op, "-") == 0;
    if (sp_poly_is_rational(recv) || sp_poly_is_brat(recv)) return TRUE;
    /* the same admitted set: saying no to the rest turns the failure into
       CRuby's NoMethodError, saying yes to these turns an operand Complex
       cannot coerce into CRuby's TypeError. */
    if (recv.cls_id == SP_BUILTIN_COMPLEX) return sp_complex_coerce_op_p(op);
    /* Array's arithmetic is `+`, `-` and `*` and stops there: `/`, `%` and
       `**` are NoMethodError, which is what FALSE here reports. A Range has
       none of these at all and reaches the same answer by having no arm. */
    if (sp_poly_is_array_kind(recv.cls_id))
      return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0;
  }
  return FALSE;
}
/* User-defined binary operators on boxed operands. The generated TU installs
   a cls_id-switch dispatcher; sp_poly_binop_bad consults it before raising,
   so a fold whose accumulator widened to poly still reaches Money#+ (#2886). */
/* Method/UnboundMethod #inspect/#to_s: the compile-time rendering stamped on
   the object at construction; a target-unknown Method falls back to the name. */
static const char *sp_method_desc_cstr(sp_BoundMethod *m) {
  if (!m) return SPL("#<Method>");
  if (m->desc) return m->desc;
  return sp_sprintf("#<Method: %s>", m->name ? m->name : "?");
}
/* A shared-mutable string handle (#3227 phase 3) behaves as its live string
   VALUE for any non-mutating op: deref to a plain boxed string and retry. */
static inline sp_RbVal sp_poly_strbuf_deref(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF)
    return sp_box_str(sp_String_cstr((sp_String *)v.v.p));
  return v;
}
/* The shared handle behind a boxed value: a strbuf box directly, a plain
   string box via a fresh handle (an unanalyzed flow -- conservative). */
static inline sp_String *sp_poly_as_strbuf(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF) return (sp_String *)v.v.p;
  if (v.tag == SP_TAG_STR && v.v.s) return sp_String_new(v.v.s);
  return sp_String_new((&("\xff")[1]));
}
static inline sp_bool sp_poly_is_strbuf(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF;
}
/* The object pointer a boxed value carries, for a slot that holds pointers
   rather than sp_RbVal (a PtrArray of one user class). nil is a NULL element,
   which is how that slot spells nil already. */
static inline void *sp_poly_obj_ptr(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ ? v.v.p : NULL;
}
typedef sp_RbVal (*sp_user_binop_fn)(const char *op, sp_RbVal a, sp_RbVal b, sp_bool *handled);
static sp_user_binop_fn sp_user_binop_hook = NULL;
/* The numeric coerce protocol from the other side: `5 + obj` asks obj for
   `coerce(5)` and applies the operator to the pair it answers. The generated
   TU installs a cls_id switch over the classes that define #coerce. The static
   path takes this route whenever the object's type is known at the call site;
   this is the same protocol for an operand that only reads poly (#3960). */
typedef sp_RbVal (*sp_user_coerce_fn)(const char *op, sp_RbVal recv, sp_RbVal obj, sp_bool *handled);
static sp_user_coerce_fn sp_user_coerce_hook = NULL;
static sp_bool sp_poly_numeric_p(sp_RbVal v);  /* fwd: the coerce guard below is numeric-only */
SP_COLD static const char *sp_nomethod_msg(const char *m, sp_RbVal v);  /* fwd: spells nil/true/false as themselves */
/* The numeric tower: the kinds an arithmetic arm below may convert. NOT
   sp_poly_numeric_p, which guards the coerce protocol and has to stay
   int/float/bigint. */
static inline sp_bool sp_poly_tower_p(sp_RbVal v) {
  if (v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT || v.tag == SP_TAG_BIGINT) return TRUE;
  return v.tag == SP_TAG_OBJ && v.v.p &&
         (v.cls_id == SP_BUILTIN_RATIONAL || v.cls_id == SP_BUILTIN_BIG_RATIONAL ||
          v.cls_id == SP_BUILTIN_COMPLEX);
}
/* The tower kinds that OWN an arm: a Bignum, Rational, BigRational or Complex
   on EITHER side sends the pair to that arm, which converts the other side
   with sp_poly_as_bigint / as_rational / as_complex -- and those read a
   String, Array, Hash, nil or Symbol as ZERO. So `10**30 + "x"` answered the
   Bignum where Ruby raises. An int or a float owns no such arm (every int and
   float branch tests both tags), so a plain number beside a String already
   falls through to the operator's own failure. */
static inline sp_bool sp_poly_tower_arm_p(sp_RbVal v) {
  if (v.tag == SP_TAG_BIGINT) return TRUE;
  return v.tag == SP_TAG_OBJ && v.v.p &&
         (v.cls_id == SP_BUILTIN_RATIONAL || v.cls_id == SP_BUILTIN_BIG_RATIONAL ||
          v.cls_id == SP_BUILTIN_COMPLEX);
}
/* An arm-owning operand beside something that is no number at all: there is no
   arm to reach, and the failure belongs to the operator. sp_poly_binop_bad
   words it as Ruby does -- "String can't be coerced into Integer" from the
   Bignum's side, "no implicit conversion of Integer into String" from the
   String's. Asked once at the head of each of the six arithmetic ops. */
static inline sp_bool sp_poly_tower_mismatch(sp_RbVal a, sp_RbVal b) {
  return (sp_poly_tower_arm_p(a) || sp_poly_tower_arm_p(b)) &&
         (!sp_poly_tower_p(a) || !sp_poly_tower_p(b));
}
static sp_RbVal sp_poly_binop_bad(const char *op, sp_RbVal recv, sp_RbVal arg) {
  if (recv.tag == SP_TAG_OBJ && recv.cls_id >= 0 && sp_user_binop_hook) {
    sp_bool _h = FALSE;
    sp_RbVal _r = sp_user_binop_hook(op, recv, arg, &_h);
    if (_h) return _r;
  }
  /* and only a NUMBER asks: String#+ and Array#+ raise for an operand they
     cannot convert, however willing it is to coerce. The check matters more
     now that a #coerce answering a homogeneously-typed pair gets a dispatch
     arm, which is what let `"abc" + obj` reach this at all. */
  if (arg.tag == SP_TAG_OBJ && arg.cls_id >= 0 && sp_user_coerce_hook &&
      (sp_poly_numeric_p(recv) || sp_poly_is_rational(recv) || sp_poly_is_brat(recv) ||
       (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_COMPLEX &&
        sp_complex_coerce_op_p(op))) &&
      !(recv.tag == SP_TAG_OBJ && recv.cls_id >= 0)) {
    sp_bool _h = FALSE;
    sp_RbVal _r = sp_user_coerce_hook(op, recv, arg, &_h);
    if (_h) return _r;
  }
  const char *rc = sp_poly_class_name(recv);
  /* CRuby names nil, true and false as THEMSELVES here, not as instances of
     their classes -- `[1, 2].reduce(nil, :+)` reads "undefined method '+' for
     nil". sp_nomethod_msg is where that wording already lives. */
  if (!sp_poly_has_binop(recv, op))
    sp_raise_cls("NoMethodError", sp_nomethod_msg(op, recv));
  /* CRuby names the offending value by its class, EXCEPT nil/true/false, which
     it spells as themselves -- and the two messages disagree about Symbol:
     "no implicit conversion" says Symbol, "can't be coerced" says :sym. */
  if (recv.tag == SP_TAG_STR || (recv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(recv.cls_id)))
    sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into %s",
                                         sp_convert_src_name(arg), rc));
  sp_raise_cls("TypeError", sp_sprintf("%s can't be coerced into %s",
                                       arg.tag == SP_TAG_SYM ? sp_poly_inspect(arg)
                                                             : sp_convert_src_name(arg), rc));
}
/* A user object on the left of a binary operator: its own method is the
   answer, and sp_poly_binop_bad is where the dispatch hook lives. Only
   `+`, `-` and `*` reached it -- every other operator coerced the object to
   an integer (0) or failed the comparison, so `money / 5` inside a block
   answered a builtin result and `money < other` raised (#3501, #3502). */
static inline int sp_poly_is_user_obj(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ && v.cls_id >= 0;
}
static int sp_poly_user_cmp(const char *op, sp_RbVal a, sp_RbVal b, sp_RbVal *out);  /* fwd */
static sp_RbVal sp_poly_add(sp_RbVal a, sp_RbVal b) { /* Two plain numbers are what a boxed arithmetic loop actually holds, and the tower checks below cannot match either tag: answer them first rather than after eight of them (#3984). */ if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(add, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (sp_float)b.v.i); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i + b.v.f); /* a user object on either side belongs to the binop hook and the coerce protocol, not to the tower branches below -- those match on the RECEIVER kind and would convert the object to a number of that kind */ if (SP_UNLIKELY(sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b))) return sp_poly_binop_bad("+", a, b); /* A shared-mutable string handle behaves as its live string VALUE for every non-mutating operator, so it has to become one BEFORE the rules below read its kind -- reached as a handle it is neither a String nor a number, and the guard reported a missing method for an operator String has. */ if (SP_UNLIKELY(sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b))) return sp_poly_add(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b)); /* Time arithmetic takes any real number of the tower -- an offset may be a Rational or a Bignum -- so it answers before the guard below, which would otherwise call the pair a coercion failure. */ if (SP_UNLIKELY(a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_TIME && a.v.p)) { if (b.tag == SP_TAG_INT) return sp_box_time(sp_time_add_i(*(sp_Time *)a.v.p, b.v.i)); if (sp_poly_tower_p(b) && !(b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_time(sp_time_add_f(*(sp_Time *)a.v.p, sp_poly_to_f_with_rational(b))); } if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("+", a, b); if ((a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX) || (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_complex(sp_complex_add(sp_poly_as_complex(a), sp_poly_as_complex(b))); if ((sp_poly_is_brat(a) || sp_poly_is_brat(b))) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) + sp_poly_to_f(b)); return sp_brat_add_poly(a, b); } if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && a.tag != SP_TAG_FLT && b.tag != SP_TAG_FLT) return sp_box_rational(sp_rational_add(sp_poly_as_rational(a), sp_poly_as_rational(b))); if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT)) return sp_box_float(sp_poly_to_f_with_rational(a) + sp_poly_to_f_with_rational(b)); if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) + sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_add(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(add, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i + b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (sp_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) return sp_box_str(sp_str_concat(a.v.s, b.v.s)); if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id) && b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id)) { SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b); sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa); sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb); return sp_box_poly_array(sp_PolyArray_concat(pa, pb)); } return sp_poly_binop_bad("+", a, b); }
static sp_RbVal sp_poly_sub(sp_RbVal a, sp_RbVal b) { /* Two plain numbers are what a boxed arithmetic loop actually holds, and the tower checks below cannot match either tag: answer them first rather than after eight of them (#3984). */ if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(sub, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f - (sp_float)b.v.i); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i - b.v.f); /* a user object on either side belongs to the binop hook and the coerce protocol, not to the tower branches below -- those match on the RECEIVER kind and would convert the object to a number of that kind */ if (SP_UNLIKELY(sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b))) return sp_poly_binop_bad("-", a, b); /* A shared-mutable string handle behaves as its live string VALUE for every non-mutating operator, so it has to become one BEFORE the rules below read its kind -- reached as a handle it is neither a String nor a number, and the guard reported a missing method for an operator String has. */ if (SP_UNLIKELY(sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b))) return sp_poly_sub(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b)); /* see sp_poly_add: Time answers before the guard, and Time - Time is a Float count of seconds. */ if (SP_UNLIKELY(a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_TIME && a.v.p)) { if (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_TIME && b.v.p) return sp_box_float(sp_time_sub_t(*(sp_Time *)a.v.p, *(sp_Time *)b.v.p)); if (b.tag == SP_TAG_INT) return sp_box_time(sp_time_sub_i(*(sp_Time *)a.v.p, b.v.i)); if (sp_poly_tower_p(b) && !(b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_time(sp_time_add_f(*(sp_Time *)a.v.p, -sp_poly_to_f_with_rational(b))); } if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("-", a, b); if ((a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX) || (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_complex(sp_complex_sub(sp_poly_as_complex(a), sp_poly_as_complex(b))); if ((sp_poly_is_brat(a) || sp_poly_is_brat(b))) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) - sp_poly_to_f(b)); return sp_brat_sub_poly(a, b); } if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && a.tag != SP_TAG_FLT && b.tag != SP_TAG_FLT) return sp_box_rational(sp_rational_sub(sp_poly_as_rational(a), sp_poly_as_rational(b))); if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT)) return sp_box_float(sp_poly_to_f_with_rational(a) - sp_poly_to_f_with_rational(b)); if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) - sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_sub(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(sub, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i - b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f - (sp_float)b.v.i);
  /* two Arrays: the set difference, mirroring the concat branch sp_poly_add
     has. Without it a poly-carried Array pair reached the failure message,
     which then read "no implicit conversion of Array into Array" (#3475). */
  if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id) && b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id)) { SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b); sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa); sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb); return sp_box_poly_array(sp_PolyArray_difference(pa, pb)); }
  return sp_poly_binop_bad("-", a, b); }
static sp_RbVal sp_poly_mul(sp_RbVal a, sp_RbVal b) { /* Two plain numbers are what a boxed arithmetic loop actually holds, and the tower checks below cannot match either tag: answer them first rather than after eight of them (#3984). */ if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(mul, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (sp_float)b.v.i); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i * b.v.f); /* a user object on either side belongs to the binop hook and the coerce protocol, not to the tower branches below -- those match on the RECEIVER kind and would convert the object to a number of that kind */ if (SP_UNLIKELY(sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b))) return sp_poly_binop_bad("*", a, b); /* A shared-mutable string handle behaves as its live string VALUE for every non-mutating operator, so it has to become one BEFORE the rules below read its kind -- reached as a handle it is neither a String nor a number, and the guard reported a missing method for an operator String has. */ if (SP_UNLIKELY(sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b))) return sp_poly_mul(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b)); if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("*", a, b); if ((a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX) || (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_complex(sp_complex_mul(sp_poly_as_complex(a), sp_poly_as_complex(b))); if ((sp_poly_is_brat(a) || sp_poly_is_brat(b))) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) * sp_poly_to_f(b)); return sp_brat_mul_poly(a, b); } if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && a.tag != SP_TAG_FLT && b.tag != SP_TAG_FLT) return sp_box_rational(sp_rational_mul(sp_poly_as_rational(a), sp_poly_as_rational(b))); if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT)) return sp_box_float(sp_poly_to_f_with_rational(a) * sp_poly_to_f_with_rational(b)); if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) * sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_mul(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(mul, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((sp_float)a.v.i * b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (sp_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_INT) return a.v.s ? sp_box_str(sp_str_repeat(a.v.s, b.v.i)) : a; /* String#*; NULL is the empty string */ return sp_poly_binop_bad("*", a, b); }
static SP_NOINLINE sp_int sp_poly_to_i_cold(sp_RbVal v);
/* Int and float are what an unboxed integer slot is fed in a hot loop; every
   other kind -- bigint, a numeric string, a Rational, a Time -- goes out of
   line. Inlining those too put a strtoll call, the bigint reader and the
   BigRational conversion inside PPU#render_pixel, 1.7KB of code the pixel
   loop walks past on its way through. */
static SP_INLINE sp_int sp_poly_to_i(sp_RbVal v) {
  if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return v.v.i;
  if (v.tag == SP_TAG_FLT) return (sp_int)v.v.f;
  return sp_poly_to_i_cold(v);
}
static SP_NOINLINE sp_int sp_poly_arg_int_obj(sp_RbVal v);   /* the object arm, below */
static SP_NOINLINE sp_int sp_poly_to_i_cold(sp_RbVal v) {
  if (v.tag == SP_TAG_BIGINT) return (sp_int)sp_bigint_to_int((sp_Bigint *)v.v.p);
  if (v.tag == SP_TAG_STR) return (sp_int)strtoll(v.v.s ? v.v.s : sp_str_empty, NULL, 10);
  if (v.tag == SP_TAG_BOOL) return v.v.b ? 1 : 0;
  /* a boxed Rational truncates toward zero, as Rational#to_i does */
  if (sp_poly_is_rational(v) && v.v.p) { sp_Rational _r = *(sp_Rational *)v.v.p; return _r.den ? _r.num / _r.den : 0; }
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_BIG_RATIONAL) return (sp_int)sp_brat_to_f((sp_BigRational *)v.v.p);
  /* a Time read out of a container: its to_i is the epoch second (#3699) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_TIME && v.v.p) return (sp_int)((sp_Time *)v.v.p)->tv_sec;
  /* A USER object (builtin-backed ones carry a negative cls_id and are handled
     above) converts through CRuby's implicit conversion protocol: its class's
     compiled #to_int, or a TypeError. It reads as a plain 0 nowhere -- that
     was the silent wrong answer. Living in the COLD half is what makes this
     free: an object never reaches sp_poly_to_i's inlined arms anyway, so the
     per-iteration cost of an index narrowing is unchanged. Callers that fill
     an int slot SPECULATIVELY -- a curry filling the scalar slots its target
     might read, a reduce seeding an int accumulator -- must not raise for a
     value that was never meant to be a number, and use sp_poly_slot_i. */
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) return sp_poly_arg_int_obj(v);
  return 0;
}

/* sp_poly_to_i without the conversion protocol: this slot MIGHT not be read as
   a number, so an object is not an error here and reads as 0. Only for slots
   filled speculatively, never for one the program really indexes by. */
static sp_int sp_poly_slot_i(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) return 0;
  return sp_poly_to_i(v);
}

/* CRuby's implicit conversion protocol on a BOXED user object: it converts
   through the class's compiled #to_int / #to_str (the generated bridge), and a
   class defining neither is CRuby's TypeError -- where the object read as 0 or
   as its #to_s rendering and the caller answered a wrong result in silence,
   which is the one outcome these boundaries must not have. Builtin-backed
   objects carry a NEGATIVE cls_id and never reach here; the callers test for
   that, and the ordinary conversions already know them (Time, Rational, ...).
   Both are cold by construction: the object case is the rare one, and keeping
   it off sp_poly_to_i's inlined fast path is worth 5-7% on optcarrot. */
static SP_NOINLINE sp_int sp_poly_arg_int_obj(sp_RbVal v) {
  if (v.v.p && sp_obj_to_int_fn) {
    int ok = 0;
    sp_int r = sp_obj_to_int_fn((int)v.cls_id, v.v.p, &ok);
    if (ok) return r;
  }
  sp_raise_cls("TypeError",
               sp_sprintf("no implicit conversion of %s into Integer",
                          sp_obj_cls_name_fn ? sp_obj_cls_name_fn((int)v.cls_id) : "Object"));
  return 0;
}
static SP_NOINLINE const char *sp_poly_arg_str_obj(sp_RbVal v) {
  if (v.v.p && sp_obj_to_str_fn) {
    const char *r = sp_obj_to_str_fn((int)v.cls_id, v.v.p);
    if (r) return r;
  }
  sp_raise_cls("TypeError",
               sp_sprintf("no implicit conversion of %s into String",
                          sp_obj_cls_name_fn ? sp_obj_cls_name_fn((int)v.cls_id) : "Object"));
  return sp_str_empty;
}
/* A boxed value entering a `const char *` ARGUMENT slot. sp_poly_to_s cannot
   carry the protocol the way sp_poly_to_i does: its object arm renders through
   #to_s, which is right for interpolation and for `puts`, so the argument
   boundary needs its own form. Every other tag keeps the slot's existing
   looseness (an Integer argument still stringifies) -- a separate
   compatibility question from this protocol, deliberately left alone. */
static SP_NOINLINE const char *sp_poly_arg_str_slow(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) return sp_poly_arg_str_obj(v);
  return sp_poly_to_s(v);
}
static SP_INLINE const char *sp_poly_arg_str(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s;
  return sp_poly_arg_str_slow(v);
}
/* The object half of the check below, split out and kept off the inlined path
   for the reason sp_poly_arg_str_obj is: the object case is the rare one.

   The value is rooted across the bridge call. #to_str is user Ruby code and
   allocates, and an object reaching a boxed comparison is often held in
   nothing but the by-value sp_RbVal the caller passed -- a copy of the
   caller's, but the collector is non-moving and marks through whatever slot it
   is handed, so rooting this copy keeps that object alive. Both callers rely
   on this one root: sp_poly_cmp_to_str below, and the poly casecmp arm the
   emitter renders. */
static SP_NOINLINE const char *sp_poly_check_str_obj(sp_RbVal v) {
  SP_GC_ROOT_RBVAL(v);
  return sp_obj_to_str_fn((int)v.cls_id, v.v.p);
}
/* CRuby's rb_check_string_type, which the String COMPARISONS ask where the
   argument slots above ask the raising forms: a String answers itself, a user
   object whose #to_str the conversion bridge carries answers its conversion,
   and every other value answers NULL without raising. "Not a string" is a real
   answer here -- it is `"abc" <=> obj`'s nil and `"abc" < obj`'s comparison
   error -- where in an argument slot it is a TypeError.

   The bridge carries only a #to_str whose static return is a String
   (conv_bridge_callee), so an object whose #to_str the analysis could pin no
   further than poly answers NULL here and converts on the TYPED side alone.
   That asymmetry is deliberate for now, not an oversight to tidy away: giving
   the bridge a boxed-answer entry is its own change. */
static SP_INLINE const char *sp_poly_check_str(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s;
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && v.v.p && sp_obj_to_str_fn)
    return sp_poly_check_str_obj(v);
  return NULL;
}
/* The ANSWER of a #to_str the analysis could pin no further than poly, on its
   way into a String comparison. CRuby's rb_check_string_type checks the answer
   as well as the method: nil is "no conversion", which is the comparison's own
   nil or its ArgumentError and which the NULL here hands back to the caller's
   refusal arm; any other non-String is a TypeError naming both classes. `obj`
   is the operand itself, for that message -- a class name, so it is read
   twice. A String answer that is the NULL string is nil to Ruby, and takes the
   no-conversion path too. */
static SP_NOINLINE const char *sp_str_cmp_conv(sp_RbVal r, sp_RbVal obj) {
  if (r.tag == SP_TAG_STR) return r.v.s;
  if (r.tag == SP_TAG_NIL) return NULL;
  sp_raise_cls("TypeError",
               sp_sprintf("can't convert %s to String (%s#to_str gives %s)",
                          sp_poly_class_name(obj), sp_poly_class_name(obj),
                          sp_poly_class_name(r)));
  return NULL;
}

/* The strict argument forms: nil / true / false in a typed String or Integer
   slot are CRuby's TypeError (File.join("a", nil), [1].take(nil)), where the
   loose forms above read them as "" / 0. Codegen emits these at the slots
   CRuby itself rejects nil in; a slot CRuby accepts nil in ("x".split(nil),
   StringIO#read(nil)) keeps the loose form. CRuby words the nil-to-Integer
   case differently from every other pairing ("from nil to integer"). */
static SP_NOINLINE const char *sp_poly_arg_str_chk_slow(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL)
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (v.tag == SP_TAG_BOOL)
    sp_raise_cls("TypeError", v.v.b ? "no implicit conversion of true into String"
                                    : "no implicit conversion of false into String");
  /* the run-time half of the static contract: a wrongly-classed scalar in a
     strict String slot raises the class-naming TypeError, exactly as the
     emitter does for a value whose class is known at compile time */
  if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT)
    sp_raise_cls("TypeError", "no implicit conversion of Integer into String");
  if (v.tag == SP_TAG_FLT)
    sp_raise_cls("TypeError", "no implicit conversion of Float into String");
  if (v.tag == SP_TAG_SYM)
    sp_raise_cls("TypeError", "no implicit conversion of Symbol into String");
  /* a boxed BUILTIN value (negative cls_id) has no #to_str either -- name
     its class, as CRuby does. A boxed shared-string handle IS a String and
     passes through (the slow path dereferences it). */
  if (v.tag == SP_TAG_OBJ && v.cls_id < 0 && !sp_poly_is_strbuf(v))
    sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into String",
                                         sp_poly_class_name(v)));
  return sp_poly_arg_str_slow(v);
}
static SP_INLINE const char *sp_poly_arg_str_chk(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s;
  return sp_poly_arg_str_chk_slow(v);
}
/* String#split's separator slot: the one String slot CRuby documents nil in
   (nil = whitespace mode). NULL preserves that answer, where the loose form
   above stringifies nil to "" and turns the call into a character split; a
   non-nil value takes the strict conversion, so an Integer separator still
   raises CRuby's TypeError (#4223). */
static SP_INLINE const char *sp_poly_arg_str_or_null(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return NULL;
  return sp_poly_arg_str_chk(v);
}
/* A boxed value entering a PATH slot (File, Dir and IO's path arguments).
   CRuby's rb_get_path asks #to_path before #to_str, which is how a Pathname,
   or any user class that names a file, is accepted wherever a String path is.
   Everything else is the strict String slot's protocol, unchanged. */
static SP_NOINLINE const char *sp_poly_arg_path_slow(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && v.v.p && sp_obj_to_path_fn) {
    const char *r = sp_obj_to_path_fn((int)v.cls_id, v.v.p);
    if (r) return r;
  }
  return sp_poly_arg_str_chk_slow(v);
}
static SP_INLINE const char *sp_poly_arg_path(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s;
  return sp_poly_arg_path_slow(v);
}
static SP_NOINLINE sp_int sp_poly_arg_int_chk_slow(sp_RbVal v) {
  /* a boxed int slot's nil sentinel is nil, not a number */
  if (v.tag == SP_TAG_INT && v.v.i == SP_INT_NIL)
    sp_raise_cls("TypeError", "no implicit conversion from nil to integer");
  if (v.tag == SP_TAG_NIL)
    sp_raise_cls("TypeError", "no implicit conversion from nil to integer");
  if (v.tag == SP_TAG_BOOL)
    sp_raise_cls("TypeError", v.v.b ? "no implicit conversion of true into Integer"
                                    : "no implicit conversion of false into Integer");
  /* a Float converts (CRuby's to_int truncation); String and Symbol do not */
  if (v.tag == SP_TAG_STR)
    sp_raise_cls("TypeError", "no implicit conversion of String into Integer");
  if (v.tag == SP_TAG_SYM)
    sp_raise_cls("TypeError", "no implicit conversion of Symbol into Integer");
  /* boxed builtins: Rational, BigRational, and Complex own #to_int in CRuby
     and keep converting below; a shared-string handle is a String; every
     other builtin kind (Array, Hash, Time, Regexp, ...) has no #to_int */
  if (v.tag == SP_TAG_OBJ && v.cls_id < 0 &&
      !sp_poly_is_rational(v) && !sp_poly_is_brat(v) &&
      v.cls_id != SP_BUILTIN_COMPLEX) {
    if (sp_poly_is_strbuf(v))
      sp_raise_cls("TypeError", "no implicit conversion of String into Integer");
    sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into Integer",
                                         sp_poly_class_name(v)));
  }
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) return sp_poly_arg_int_obj(v);
  return sp_poly_to_i(v);
}
static SP_INLINE sp_int sp_poly_arg_int_chk(sp_RbVal v) {
  if (v.tag == SP_TAG_INT && v.v.i != SP_INT_NIL) return v.v.i;
  return sp_poly_arg_int_chk_slow(v);
}
/* File.open's permission slot: nil is CRuby's default (SP_INT_NIL to the
   open entries), anything else converts as an Integer argument does. */
static SP_INLINE sp_int sp_poly_arg_perm(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL || (v.tag == SP_TAG_INT && v.v.i == SP_INT_NIL)) return SP_INT_NIL;
  return sp_poly_arg_int_chk(v);
}

static sp_float sp_poly_to_f(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return v.v.f; if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return (sp_float)v.v.i; if (v.tag == SP_TAG_BIGINT) return sp_bigint_to_double((sp_Bigint *)v.v.p); if (v.tag == SP_TAG_STR) return (sp_float)atof(v.v.s ? v.v.s : sp_str_empty); if (v.tag == SP_TAG_BOOL) return v.v.b ? 1.0 : 0.0; if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL) return sp_rational_to_f(*(sp_Rational *)v.v.p); if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_BIG_RATIONAL) return sp_brat_to_f((sp_BigRational *)v.v.p); if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_TIME && v.v.p) { sp_Time _tt = *(sp_Time *)v.v.p; return (sp_float)_tt.tv_sec + (sp_float)_tt.tv_nsec / 1e9; } return 0.0; }  /* STR arm mirrors sp_poly_to_i's strtoll and the typed String#to_f (atof) */
/* The same conversions, but a boxed nil lands on the type's sentinel instead
   of the type's zero. A method whose declared return is `Integer?`/`Float?`
   narrows a boxed body into the unboxed slot here, and the plain conversions
   answer 0 / 0.0 for nil -- an ordinary value in that slot, so the caller
   cannot tell it from a real zero. Both sentinels are what every consumer of a
   nullable int/float already tests for (#3458). */
static sp_int sp_poly_to_i_or_nil(sp_RbVal v) { return v.tag == SP_TAG_NIL ? SP_INT_NIL : sp_poly_to_i(v); }
static sp_float sp_poly_to_f_or_nil(sp_RbVal v) { return v.tag == SP_TAG_NIL ? sp_float_nil() : sp_poly_to_f(v); }
static inline const char *sp_poly_to_s_or_nil(sp_RbVal v) { return v.tag == SP_TAG_NIL ? NULL : sp_poly_to_s(v); }
/* Unbox to float? preserving nil as the float-nil sentinel. Used by the
   unpack1 literal-float-directive fast path: sp_str_unpack pads short input
   with nil, which must stay nil through the unboxed TY_FLOAT result (CRuby
   returns nil there) instead of coercing to 0.0 like sp_poly_to_f. */
static sp_float sp_poly_to_f_opt(sp_RbVal v) { return v.tag == SP_TAG_NIL ? sp_float_nil() : sp_poly_to_f(v); }
/* Case conversions / succ preserve the receiver's class: a Symbol converts
   through its name and re-interns, a String stays a String (CRuby). */
/* String#to_c: CRuby's lenient complex parse -- a leading real part, an
   optional [+-]imag i tail, or a bare "Ni"; unparseable input is (0+0i). */
/* sp_str_to_c: moved to lib/sp_cold.c */
sp_Complex sp_str_to_c(const char *s);
sp_Complex sp_str_to_c_strict(const char *s);
/* lib/sp_cold.c: while sp_convert_soft is set, an unparseable Complex/Rational
   string sets sp_convert_failed instead of raising (Kernel's exception: false). */
extern sp_bool sp_convert_soft;
extern sp_bool sp_convert_failed;
/* Hash subset/superset comparisons (boxed, any variant pairing): every pair
   of `a` present in `b` with an equal value; strict adds len <. */
static void sp_poly_hash_pair(sp_RbVal v, sp_int i, sp_RbVal *k, sp_RbVal *out);
static sp_bool sp_poly_eq(sp_RbVal a, sp_RbVal b);
static sp_int sp_poly_length(sp_RbVal v);
static sp_bool sp_OpenStruct_eq(sp_OpenStruct *a, sp_OpenStruct *b);   /* defined with OpenStruct below */
static sp_RbVal sp_poly_hash_get_pair_val(sp_RbVal h, sp_RbVal key, sp_bool *found) {
  sp_int n = sp_poly_length(h);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(h, i, &k, &v);
    if (sp_poly_eq(k, key)) { *found = TRUE; return v; }
  }
  *found = FALSE;
  return sp_box_nil();
}
SP_NORETURN SP_COLD static void sp_raise_poly_nomethod(const char *m, sp_RbVal v);  /* fwd */
/* `length` on a boxed receiver: nil, a number and a user object have none, and
   sp_poly_length answers 0 for all three -- so `v.length` on a nil read out of
   a hash miss answered 0 instead of raising NoMethodError (#3974). */
static sp_int sp_poly_length_m(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL || v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT ||
      v.tag == SP_TAG_BIGINT || v.tag == SP_TAG_BOOL || sp_poly_is_user_obj(v))
    sp_raise_poly_nomethod("length", v);
  /* String#length is CHARACTERS. sp_poly_length answers the byte count, which
     is what its container and iteration callers want and what a boxed string
     reaching THIS entry must not get: the two agree on ASCII and part company
     at the first wider character, so a poly-carried string reported 7 for the
     five characters of "a - b" with an em dash (#4251). */
  if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_length(v.v.s) : 0;
  if (v.tag == SP_TAG_OBJ && sp_poly_is_strbuf(v))
    return sp_str_length(sp_String_cstr((sp_String *)v.v.p));
  return sp_poly_length(v);
}
/* `size` on a boxed receiver: a collection answers its length, but an Integer
   answers the bytes of its machine representation, which sp_poly_length has no
   arm for and reported as 0. nil and a user object have no size at all. */
static sp_int sp_poly_size(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL || v.tag == SP_TAG_BOOL || v.tag == SP_TAG_FLT ||
      sp_poly_is_user_obj(v))
    sp_raise_poly_nomethod("size", v);
  if (v.tag == SP_TAG_INT) return (sp_int)sizeof(sp_int);
  if (v.tag == SP_TAG_BIGINT) {
    sp_Bigint *bg = (sp_Bigint *)v.v.p;
    sp_int bits = bg ? (sp_int)sp_bigint_bit_length(bg) : 0;
    sp_int bytes = (bits + 7) / 8;
    return bytes < (sp_int)sizeof(sp_int) ? (sp_int)sizeof(sp_int) : bytes;
  }
  /* String#size is String#length: characters, not bytes (#4251) */
  if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_length(v.v.s) : 0;
  if (v.tag == SP_TAG_OBJ && sp_poly_is_strbuf(v))
    return sp_str_length(sp_String_cstr((sp_String *)v.v.p));
  return sp_poly_length(v);
}
static sp_bool sp_poly_hash_subset(sp_RbVal a, sp_RbVal b, int strict) {
  sp_int na = sp_poly_length(a), nb = sp_poly_length(b);
  if (strict ? (na >= nb) : (na > nb)) return FALSE;
  for (sp_int i = 0; i < na; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(a, i, &k, &v);
    sp_bool found = FALSE;
    sp_RbVal bv = sp_poly_hash_get_pair_val(b, k, &found);
    if (!found || !sp_poly_eq(v, bv)) return FALSE;
  }
  return TRUE;
}
/* A poly RECEIVER for a String method. The program named a String method, so a
   receiver that is not a string is CRuby's NoMethodError -- where the #to_s
   rendering answered it instead and `obj.upcase` returned "#<BARE:0X...>" in
   silence. Implicit conversion does NOT apply here: an object defining #to_str
   is still NoMethodError, because the protocol converts an ARGUMENT into a
   String slot and never rescues a receiver (CRuby raises for `Pathname#upcase`
   even though Pathname defines #to_str). A shared-mutable string is a builtin
   OBJ box, not SP_TAG_STR, and is a string. */
static const char *sp_poly_recv_s(sp_RbVal v, const char *meth) {
  if (v.tag == SP_TAG_STR) return v.v.s ? v.v.s : sp_str_empty;
  if (sp_poly_is_strbuf(v)) return sp_poly_to_s(v);
  sp_raise_nomethod(sp_nomethod_msg(meth, v));
  return sp_str_empty;
}

static sp_RbVal sp_poly_case_conv(sp_RbVal v, const char *(*fn)(const char *), const char *meth) {
  if (v.tag == SP_TAG_SYM && sp_sym_name_fn && sp_json_sym_intern_fn)
    return sp_box_sym(sp_json_sym_intern_fn(fn(sp_sym_name_fn((sp_sym)v.v.i))));
  return sp_box_str(fn(sp_poly_recv_s(v, meth)));
}
static sp_bool sp_poly_numeric_p(sp_RbVal v) { return v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT || v.tag == SP_TAG_BIGINT; }
/* Display form of a value in a `can't convert %s into ...` TypeError:
   nil/true/false render lowercase, everything else by class name (CRuby). */
static const char *sp_convert_src_name(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return SPL("nil");
  if (v.tag == SP_TAG_BOOL) return v.v.b ? SPL("true") : SPL("false");
  return sp_poly_class_name(v);
}
/* Kernel#Integer / Kernel#Float: STRICT coercion. Unlike the lenient
   sp_poly_to_i / sp_poly_to_f (which treat nil as 0 for `nil.to_i`), these
   raise TypeError on nil / a non-numeric object and ArgumentError on an
   unparseable String, matching CRuby's conversion methods. */
static sp_int sp_poly_Integer(sp_RbVal v) {
  if (v.tag == SP_TAG_INT) return v.v.i;
  if (v.tag == SP_TAG_BIGINT) {
    /* the Integer slot cannot carry a Bignum: the value when it fits, a
       loud RangeError otherwise, never a number cut to 64 bits */
    sp_Bigint *bg = (sp_Bigint *)v.v.p;
    sp_int n = (sp_int)sp_bigint_to_int(bg);
    if (sp_bigint_bit_length(bg) <= 63 && n != SP_INT_NIL) return n;
    sp_raise_cls("RangeError", "bignum too big to convert into 'long'");
  }
  if (v.tag == SP_TAG_FLT) {
    if (!isfinite(v.v.f))
      sp_raise_cls("FloatDomainError", isnan(v.v.f) ? "NaN" : v.v.f > 0 ? "Infinity" : "-Infinity");
    return (sp_int)v.v.f;
  }
  if (v.tag == SP_TAG_STR) return (sp_int)sp_str_to_i_strict(v.v.s ? v.v.s : sp_str_empty);
  /* a user object converts through its own #to_int / #to_str / #to_i */
  if (sp_poly_is_user_obj(v)) return sp_poly_Integer_ex(v, 0, 1);
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Integer", sp_convert_src_name(v)));
  return 0;
}
static sp_float sp_poly_Float(sp_RbVal v) {
  if (v.tag == SP_TAG_FLT) return v.v.f;
  if (v.tag == SP_TAG_INT) return (sp_float)v.v.i;
  if (v.tag == SP_TAG_BIGINT) return sp_poly_to_f(v);
  /* a Rational is a real number Kernel#Float converts (Float(1/2r) is 0.5);
     without an arm it reached the raise below as "can't convert Rational" */
  if (sp_poly_is_rational(v) || sp_poly_is_brat(v)) return sp_poly_to_f(v);
  if (v.tag == SP_TAG_STR) return sp_str_to_f_strict(v.v.s ? v.v.s : sp_str_empty);
  /* a user object converts through its own #to_f */
  if (sp_poly_is_user_obj(v)) return sp_poly_Float_ex(v, 1);
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", sp_convert_src_name(v)));
  return 0.0;
}
/* Coerce a value to a C double for a Math.<fn> argument. Math accepts only a
   real Numeric (no String parsing, unlike Kernel#Float), so nil / String /
   any non-numeric raises TypeError -- where the lenient sp_poly_to_f coerced
   nil to 0.0 and a String cast was a C compile error. */
static sp_float sp_num_to_f(sp_RbVal v) {
  if (v.tag == SP_TAG_FLT) return v.v.f;
  if (v.tag == SP_TAG_INT) return (sp_float)v.v.i;
  if (v.tag == SP_TAG_BIGINT) return (sp_float)sp_bigint_to_int((sp_Bigint *)v.v.p);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL && v.v.p)
    return sp_rational_to_f(*(sp_Rational *)v.v.p);
  const char *w = v.tag == SP_TAG_NIL ? "nil"
                : v.tag == SP_TAG_BOOL ? (v.v.b ? "true" : "false")
                : sp_poly_class_name(v);
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", w));
  return 0.0;
}
/* Exception introspection staging: the runtime raisers stage the receiver /
   key / carried value here just before raising, and sp_raise_cls materializes
   a carried exception exposing them via #receiver / #key / #tag / #reason /
   #exit_value / #value / #args (#2753-#2756, #2770). Cleared on every raise.
   The bridges are extern so the cold lib TUs (frozen-string, nil-receiver
   raisers) can stage too. */
#ifdef SPINEL_EXT_HOST
extern SP_TLS sp_RbVal sp_pending_exc_recv, sp_pending_exc_key, sp_pending_exc_val;
#else
SP_TLS sp_RbVal sp_pending_exc_recv, sp_pending_exc_key, sp_pending_exc_val;
#endif
#ifdef SPINEL_EXT_HOST
extern SP_TLS unsigned char sp_pending_exc_flags;
#else
SP_TLS unsigned char sp_pending_exc_flags = 0;
#endif
/* Signal.trap state (defined with the Signal machinery below; declared here
   so the GC mark hook can keep installed proc handlers live). */
struct sp_Proc;
#ifdef SPINEL_EXT_HOST
extern const char *sp_trap_state[SP_SIG_MAX];
#else
const char *sp_trap_state[SP_SIG_MAX];
#endif
#ifdef SPINEL_EXT_HOST
extern struct sp_Proc *sp_trap_proc[SP_SIG_MAX];
#else
struct sp_Proc *sp_trap_proc[SP_SIG_MAX];
#endif
#ifdef SPINEL_EXT_HOST
SP_COLD void sp_exc_stage_recv(sp_RbVal v);
#else
SP_COLD void sp_exc_stage_recv(sp_RbVal v) { sp_pending_exc_recv = v; sp_pending_exc_flags |= 1; }
#endif
#ifdef SPINEL_EXT_HOST
SP_COLD void sp_exc_stage_key(sp_RbVal v);
#else
SP_COLD void sp_exc_stage_key(sp_RbVal v)  { sp_pending_exc_key = v;  sp_pending_exc_flags |= 2; }
#endif
#ifdef SPINEL_EXT_HOST
SP_COLD void sp_exc_stage_val(sp_RbVal v);
#else
SP_COLD void sp_exc_stage_val(sp_RbVal v)  { sp_pending_exc_val = v;  sp_pending_exc_flags |= 4; }
#endif
/* frozen-Hash raise carrying the receiver (identity-preserving) (#3119) */
static void __attribute__((noinline,cold)) sp_raise_frozen_hash_at(void *h, int cls_id) {
  sp_exc_stage_recv(sp_box_obj(h, cls_id));
  sp_raise_cls("FrozenError", (&("\xff" "can't modify frozen Hash")[1]));
}
/* Numeric queries / rounding on a poly value: dispatch on the runtime tag the
   way CRuby dispatches on the class. A tag whose class does not define the
   method raises CRuby's NoMethodError (e.g. `1.nan?`, `"x".abs`). */
SP_NORETURN SP_COLD static void sp_raise_poly_nomethod(const char *m, sp_RbVal v) {
  sp_exc_stage_recv(v);
  /* nil, true and false read as themselves in CRuby's message, as the sibling
     builder below already spells them */
  const char *d = (v.tag == SP_TAG_NIL) ? SPL("nil")
                : (v.tag == SP_TAG_BOOL) ? (v.v.i ? SPL("true") : SPL("false"))
                : sp_sprintf("an instance of %s", sp_poly_class_name(v));
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method '%s' for %s", m, d));
}
/* CRuby-shaped NoMethodError text for an unresolved call on a runtime value:
   nil/true/false read as themselves, anything else as "an instance of
   <Class>". Only builds the message -- the caller keeps the recognizable
   sp_raise_nomethod(...) form the coercion paths key on. */
SP_COLD static const char *sp_nomethod_msg(const char *m, sp_RbVal v) {
  sp_exc_stage_recv(v);
  const char *d = (v.tag == SP_TAG_NIL) ? "nil"
                : (v.tag == SP_TAG_BOOL) ? (v.v.i ? "true" : "false")
                : sp_sprintf("an instance of %s", sp_poly_class_name(v));
  return sp_sprintf("undefined method '%s' for %s", m, d);
}
/* #succ / #next on a boxed receiver. Every kind that answers them has its own
   answer: an Integer counts up, a String or Symbol takes the succ sequence,
   and an Enumerator pulls its next value (#next only). Routing all of them
   through the string succ answered "" for an Enumerator and a string for an
   Integer (#3843). */
static sp_RbVal sp_enum_next_boxed(sp_RbVal v);      /* defined below, after sp_enum.h */
static sp_RbVal sp_poly_succ_m(sp_RbVal v, sp_bool allow_enum) {
  if (v.tag == SP_TAG_INT) return sp_box_int(v.v.i + 1);
  if (v.tag == SP_TAG_BIGINT) return sp_box_bigint(sp_bigint_add((sp_Bigint *)v.v.p,
                                                                 sp_bigint_new_int(1)));
  if (v.tag == SP_TAG_STR) return sp_box_str(sp_str_succ(v.v.s));
  /* a shared-string handle succeeds as its live value (#4279) */
  if (sp_poly_is_strbuf(v)) return sp_box_str(sp_str_succ(sp_poly_to_s(v)));
  if (v.tag == SP_TAG_SYM && sp_sym_name_fn && sp_json_sym_intern_fn)
    return sp_box_sym(sp_json_sym_intern_fn(sp_str_succ(sp_sym_name_fn((sp_sym)v.v.i))));
  if (allow_enum && v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_ENUMERATOR && v.v.p)
    return sp_enum_next_boxed(v);
  sp_raise_nomethod(sp_nomethod_msg(allow_enum ? "next" : "succ", v));
  return sp_box_nil();
}
/* An EXPLICIT `.to_i` on a boxed receiver, as opposed to a slot that WANTS an
   Integer: the program asked for the method by name, so a user object whose
   class does not define it is CRuby's NoMethodError -- not the implicit
   conversion protocol's TypeError, which sp_poly_to_i raises for the slot
   case. A class that does define #to_i never reaches here (poly dispatch gives
   it its own arm), and a builtin receiver carries a negative cls_id. */
static sp_int sp_poly_to_i_meth(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) sp_raise_nomethod(sp_nomethod_msg("to_i", v));
  return sp_poly_to_i(v);
}

/* Like sp_nomethod_msg, but also stages the failed call's argument list for
   NoMethodError#args (#2837). */
SP_COLD static const char *sp_nomethod_msg_args(const char *m, sp_RbVal v, sp_int n, sp_RbVal *args) {
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(a, args[i]);
  sp_exc_stage_key(sp_box_poly_array(a));
  return sp_nomethod_msg(m, v);
}
/* The statically-typed gate arms keep their literal message; this stages the
   argument list beside it. */
SP_COLD static const char *sp_stage_args_msg(const char *msg, sp_int n, sp_RbVal *args) {
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(a, args[i]);
  sp_exc_stage_key(sp_box_poly_array(a));
  return msg;
}
/* Like sp_stage_args_msg but also stages the receiver for NoMethodError#receiver
   when the gate arm knows the receiver value at compile time (#3068). */
SP_COLD static const char *sp_stage_recv_args_msg(const char *msg, sp_RbVal recv, sp_int n, sp_RbVal *args) {
  sp_exc_stage_recv(recv);
  return sp_stage_args_msg(msg, n, args);
}
SP_COLD static const char *sp_stage_recv_msg(const char *msg, sp_RbVal recv) {
  sp_exc_stage_recv(recv);
  return msg;
}

/* floor/ceil/round/truncate on a non-finite Float: casting NaN/Inf to an
   integer is C UB; CRuby raises FloatDomainError naming the value. */
static inline void sp_poly_flo_domain_ck(sp_float f) {
  if (!isfinite(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : f > 0 ? "Infinity" : "-Infinity");
}
static sp_bool sp_poly_nan_p(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isnan(v.v.f) != 0; sp_raise_poly_nomethod("nan?", v); }
static sp_bool sp_poly_finite_p(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isfinite(v.v.f) != 0; if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return TRUE; sp_raise_poly_nomethod("finite?", v); }
static sp_RbVal sp_poly_infinite(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isinf(v.v.f) ? sp_box_int(v.v.f > 0 ? 1 : -1) : sp_box_nil(); if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return sp_box_nil(); sp_raise_poly_nomethod("infinite?", v); }
/* Complex-projection queries on a poly value read out of a container (#2882):
   a Complex yields its stored component (int- or float-classed per its flags),
   and any real number is its own real part with a zero imaginary part. */
static sp_RbVal sp_poly_real(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX) {
    sp_Complex *c = (sp_Complex *)v.v.p;
    return sp_complex_comp_v(c->re, c->fl & SP_CPLX_RE_F);
  }
  if (sp_poly_numeric_p(v) || sp_poly_is_rational(v)) return v;
  sp_raise_poly_nomethod("real", v);
}
static sp_RbVal sp_poly_imaginary(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX) {
    sp_Complex *c = (sp_Complex *)v.v.p;
    return sp_complex_comp_v(c->im, c->fl & SP_CPLX_IM_F);
  }
  if (sp_poly_numeric_p(v) || sp_poly_is_rational(v)) return sp_box_int(0);
  sp_raise_poly_nomethod("imaginary", v);
}
/* Sign of a boxed Rational / big Rational: its numerator's, both being kept
   with a positive denominator. Read exactly rather than through a double --
   a ratio smaller than DBL_MIN would round to 0.0 and lose its sign. */
static int sp_poly_rat_sign(sp_RbVal v) {
  if (sp_poly_is_rational(v)) { sp_int n = ((sp_Rational *)v.v.p)->num; return n > 0 ? 1 : (n < 0 ? -1 : 0); }
  return sp_bigint_sign(((sp_BigRational *)v.v.p)->num);
}
static inline int sp_poly_is_rat_kind(sp_RbVal v) { return (sp_poly_is_rational(v) || sp_poly_is_brat(v)) && v.v.p; }
/* Complex#zero? is `self == 0`: both parts zero. It has no #positive? /
   #negative?, so only this one gets the arm. */
static sp_bool sp_poly_zero_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i == 0; if (v.tag == SP_TAG_FLT) return v.v.f == 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) == 0; if (sp_poly_is_rat_kind(v)) return sp_poly_rat_sign(v) == 0; if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX && v.v.p) { sp_Complex *_cz = (sp_Complex *)v.v.p; return _cz->re == 0.0 && _cz->im == 0.0; } sp_raise_poly_nomethod("zero?", v); }
/* Complex#conjugate / #conj on a boxed value: negate the imaginary part; a real
   number (numeric/rational) is its own conjugate. */
static sp_RbVal sp_poly_conjugate(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX)
    return sp_box_complex(sp_complex_conjugate(*(sp_Complex *)v.v.p));
  if (sp_poly_numeric_p(v) || sp_poly_is_rational(v)) return v;
  sp_raise_poly_nomethod("conjugate", v);
}
/* Range#begin / #end on a boxed value (an int-backed sp_Range read out of a
   poly container): the endpoint as an Integer. */
static sp_int sp_poly_range_begin(sp_RbVal v) { if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE) return ((sp_Range *)v.v.p)->first; sp_raise_poly_nomethod("begin", v); }
static sp_int sp_poly_range_end(sp_RbVal v) { if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE) return ((sp_Range *)v.v.p)->last; sp_raise_poly_nomethod("end", v); }
static sp_bool sp_poly_positive_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i > 0; if (v.tag == SP_TAG_FLT) return v.v.f > 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) > 0; if (sp_poly_is_rat_kind(v)) return sp_poly_rat_sign(v) > 0; sp_raise_poly_nomethod("positive?", v); }
static sp_bool sp_poly_negative_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i < 0; if (v.tag == SP_TAG_FLT) return v.v.f < 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) < 0; if (sp_poly_is_rat_kind(v)) return sp_poly_rat_sign(v) < 0; sp_raise_poly_nomethod("negative?", v); }
/* abs of a negative int goes through SP_POLY_INT_OP(sub, 0, x): plain -x is
   UB for INT_MIN; promote mode boxes it as a bigint, wrap mode keeps the
   documented wrapping C arithmetic. fabs covers -0.0 -> 0.0 too. */
static sp_RbVal sp_poly_abs(sp_RbVal v) { if (v.tag == SP_TAG_INT) { if (v.v.i >= 0) return v; return SP_POLY_INT_OP(sub, (sp_int)0, v.v.i); } if (v.tag == SP_TAG_FLT) return sp_box_float(fabs(v.v.f)); if (v.tag == SP_TAG_BIGINT) { sp_Bigint *b = (sp_Bigint *)v.v.p; return sp_bigint_sign(b) < 0 ? sp_box_bigint(sp_bigint_sub(sp_bigint_new_int(0), b)) : v; } if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX) return sp_complex_abs_v(*(sp_Complex *)v.v.p); if (sp_poly_is_rational(v)) return sp_box_rational(sp_rational_abs(sp_poly_as_rational(v))); sp_raise_poly_nomethod("abs", v); }
static sp_RbVal sp_poly_abs2(sp_RbVal v) { if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX) return sp_complex_abs2_v(*(sp_Complex *)v.v.p); if (sp_poly_numeric_p(v)) { sp_RbVal a = sp_poly_abs(v); return sp_poly_mul(a, a); } sp_raise_poly_nomethod("abs2", v); }
/* No-arg floor/ceil/round/truncate return Integer in Ruby: an int/bigint tag
   is already its own floor (returned unchanged, lossless for bigints), a
   float converts through the matching libm rounding. */
static sp_RbVal sp_poly_floor(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((sp_int)floor(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; if (sp_poly_is_rational(v)) return sp_box_int(sp_rational_floor_i(sp_poly_as_rational(v))); sp_raise_poly_nomethod("floor", v); }
/* a NULL char* carried under SP_TAG_STR is the empty string (as in
   sp_poly_to_i / sp_poly_eq): bytesize 0, ord raises CRuby's ArgumentError. */
static sp_int sp_poly_bytesize(sp_RbVal v) { if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_bytesize_m(v.v.s) : 0; sp_raise_poly_nomethod("bytesize", v); }
static sp_int sp_poly_ord(sp_RbVal v) { if (v.tag == SP_TAG_STR) { if (!v.v.s) sp_raise_cls("ArgumentError", "empty string"); return sp_str_ord(v.v.s); } if (v.tag == SP_TAG_INT) return v.v.i; sp_raise_poly_nomethod("ord", v); }
static sp_int sp_poly_bit_length(sp_RbVal v) { if (v.tag == SP_TAG_INT) return sp_int_bit_length(v.v.i); sp_raise_poly_nomethod("bit_length", v); }
/* Rational#numerator / #denominator on a boxed value: a Rational reports its
   reduced parts; an Integer is n/1. Both commit to sp_int (analyze's TY_INT),
   matching the typed Rational path. */
static sp_int sp_poly_numerator(sp_RbVal v) { if (sp_poly_is_rational(v)) return sp_poly_as_rational(v).num; if (v.tag == SP_TAG_INT) return v.v.i; sp_raise_poly_nomethod("numerator", v); }
static sp_int sp_poly_denominator(sp_RbVal v) { if (sp_poly_is_rational(v)) return sp_poly_as_rational(v).den; if (v.tag == SP_TAG_INT) return 1; sp_raise_poly_nomethod("denominator", v); }
/* String#getbyte on a poly value; nil (not 0) for an out-of-range index, per
   CRuby, so the result is boxed. */
static sp_RbVal sp_poly_getbyte(sp_RbVal v, sp_int i) { if (v.tag != SP_TAG_STR) sp_raise_poly_nomethod("getbyte", v); const char *s = v.v.s; if (!s) return sp_box_nil(); sp_int bl = (sp_int)sp_str_byte_len(s); if (i < 0) i += bl; if (i < 0 || i >= bl) return sp_box_nil(); return sp_box_int((sp_int)(unsigned char)s[i]); }
static sp_RbVal sp_poly_ceil(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((sp_int)ceil(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; if (sp_poly_is_rational(v)) return sp_box_int(sp_rational_ceil_i(sp_poly_as_rational(v))); sp_raise_poly_nomethod("ceil", v); }
static sp_RbVal sp_poly_round(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((sp_int)round(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; if (sp_poly_is_rational(v)) return sp_box_int(sp_rational_round_i(sp_poly_as_rational(v))); sp_raise_poly_nomethod("round", v); }
/* Numeric#round(ndigits): a Float stays Float when n > 0 (rounded to n decimal
   places) and becomes Integer when n <= 0; an Integer is unchanged for n >= 0
   and rounded to a power of ten for n < 0. Mirrors the scalar Float#round(n)
   codegen path, dispatched on the runtime tag. */
static sp_RbVal sp_poly_round_n(sp_RbVal v, sp_int n) {
  if (v.tag == SP_TAG_FLT) {
    double x = v.v.f;
    if (n > 0) { double f = pow(10, (double)n); if (isinf(f)) return sp_box_float(x);
      double r = round(x * f) / f; return sp_box_float((x != 0.0 && r == 0.0) ? 0.0 : r); }  /* +0.0 normalize (#3235) */
    sp_poly_flo_domain_ck(x);
    double f = pow(10, (double)(-n));
    return sp_box_int(isinf(f) ? 0 : (sp_int)(round(x / f) * f));
  }
  if (v.tag == SP_TAG_INT) {
    if (n >= 0) return v;
    double f = pow(10, (double)(-n));
    return sp_box_int(isinf(f) ? 0 : (sp_int)(round((double)v.v.i / f) * f));
  }
  if (v.tag == SP_TAG_BIGINT) return v;  /* n < 0 on a bignum is out of scope */
  /* Rational#round(n): a positive precision keeps the Rational, n <= 0 lands
     on an Integer, matching the typed path. */
  if (sp_poly_is_rational(v)) {
    sp_Rational r = sp_poly_as_rational(v);
    if (n > 0) return sp_box_rational(sp_rational_round_prec(r, n));
    if (n == 0) return sp_box_int(sp_rational_round_i(r));
    sp_Rational q = sp_rational_round_prec(r, n);
    return sp_box_int(q.num / q.den);
  }
  sp_raise_poly_nomethod("round", v);
}
static sp_RbVal sp_poly_truncate(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((sp_int)trunc(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; if (sp_poly_is_rational(v)) { sp_Rational _r = sp_poly_as_rational(v); return sp_box_int(_r.num / _r.den); } sp_raise_poly_nomethod("truncate", v); }
/* forward: generic array length/element (defined later in this header) and
   the array-kind predicate for cross-kind value equality. */
static sp_int sp_poly_length(sp_RbVal v);
static sp_RbVal sp_poly_arr_get(sp_RbVal a, sp_int i);
static sp_PolyArray *sp_poly_user_elems(sp_RbVal v);   /* fwd: user Enumerable elements */
/* poly-valued hash variants are defined later in this header */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef struct sp_SymPolyHash sp_SymPolyHash;
typedef struct sp_PolyPolyHash sp_PolyPolyHash;
static sp_bool sp_StrPolyHash_eq(sp_StrPolyHash *a, sp_StrPolyHash *b);
static sp_bool sp_SymPolyHash_eq(sp_SymPolyHash *a, sp_SymPolyHash *b);
static sp_bool sp_PolyPolyHash_eq(sp_PolyPolyHash *a, sp_PolyPolyHash *b);
static inline int sp_poly_is_array_kind(int cls_id) {
  return cls_id == SP_BUILTIN_INT_ARRAY || cls_id == SP_BUILTIN_STR_ARRAY ||
         cls_id == SP_BUILTIN_FLT_ARRAY || cls_id == SP_BUILTIN_SYM_ARRAY ||
         cls_id == SP_BUILTIN_POLY_ARRAY;
}
static inline int sp_poly_is_hash_kind(int cls_id) {
  return cls_id == SP_BUILTIN_STR_INT_HASH || cls_id == SP_BUILTIN_STR_STR_HASH ||
         cls_id == SP_BUILTIN_INT_STR_HASH || cls_id == SP_BUILTIN_INT_INT_HASH || cls_id == SP_BUILTIN_STR_POLY_HASH ||
         cls_id == SP_BUILTIN_SYM_POLY_HASH || cls_id == SP_BUILTIN_POLY_POLY_HASH;
}
/* Cross-variant hash equality (defined after every hash type below): hashes
   compare by VALUE across storage variants, like arrays -- Ruby has one Hash
   and the variants are a storage optimization that must not leak into ==
   (a JSON.parse StrPolyHash equals the same pairs written as a literal
   StrIntHash). */
static sp_bool sp_poly_hash_eq_cross(sp_RbVal a, sp_RbVal b);
/* User Struct/Data instances compare by VALUE inside containers (Array/Hash
   equality, include?/index/uniq, nested members). The generated TU installs
   sp_obj_eq_hook to dispatch a field-wise == by cls_id; sp_poly_eq consults it
   for two same-class user objects that no builtin arm handles. */
typedef sp_bool (*sp_obj_eq_fn)(sp_RbVal a, sp_RbVal b);
static sp_obj_eq_fn sp_obj_eq_hook = NULL;
/* The == arms that can walk back into the pair they started from (below). */
static sp_bool sp_poly_eq_deep(sp_RbVal a, sp_RbVal b);
static sp_bool sp_poly_eq(sp_RbVal a, sp_RbVal b) {
  /* a user class's own == answers before any builtin reading, the way its
     other operators now do; the field-wise hook below stays the default for
     a class that does not define one (#3501) */
  { sp_RbVal _u; if (sp_poly_user_cmp("==", a, b, &_u)) return sp_poly_truthy(_u); }
  /* shared-mutable string handle (#3227): == is content equality, against
     either another handle or a plain string */
  { const char *_sa = (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_STRBUF) ? sp_String_cstr((sp_String *)a.v.p) : NULL;
    const char *_sb = (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_STRBUF) ? sp_String_cstr((sp_String *)b.v.p) : NULL;
    if (_sa || _sb) {
      if (!_sa) { if (a.tag != SP_TAG_STR) return FALSE; _sa = a.v.s; }
      if (!_sb) { if (b.tag != SP_TAG_STR) return FALSE; _sb = b.v.s; }
      return sp_str_eq(_sa, _sb);
    } } if (sp_poly_is_brat(a) || sp_poly_is_brat(b)) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_poly_to_f(a) == sp_poly_to_f(b); int _oka = sp_poly_is_brat(a) || sp_poly_is_rational(a) || a.tag == SP_TAG_INT || a.tag == SP_TAG_BIGINT; int _okb = sp_poly_is_brat(b) || sp_poly_is_rational(b) || b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT; return (_oka && _okb) ? (sp_brat_cmp_poly(a, b) == 0) : FALSE; } /* A boxed Rational against a number: sp_poly_numeric_p covers only int/float/bigint, so without this a Rational operand fell through to the tag-equality test below and `Rational(1,1) == 1` answered false (#3382). The BigRational clause above already covers the mixed big cases. */ if (sp_poly_is_rational(a) || sp_poly_is_rational(b)) { int _qa = sp_poly_is_rational(a) || sp_poly_numeric_p(a); int _qb = sp_poly_is_rational(b) || sp_poly_numeric_p(b); if (!(_qa && _qb)) return FALSE; if (sp_poly_is_rational(a) && sp_poly_is_rational(b) && a.v.p && b.v.p) return sp_rational_eq(*(sp_Rational *)a.v.p, *(sp_Rational *)b.v.p); return sp_poly_to_f(a) == sp_poly_to_f(b); } if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b); if (ba && bb) return sp_bigint_cmp(ba, bb) == 0; if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); return FALSE; } /* Two integers compare AS INTEGERS. The numeric fallback below converts both to double, which above 2^53 is lossy: -3145750702635002333 and -3145750702635002395 land on the same double and compared equal, so a poly-slotted Integer `==` answered true for two different numbers. The fallback is for the MIXED int/float case and keeps it. */ if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return a.v.i == b.v.i; if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); if (a.tag != b.tag) return FALSE; switch (a.tag) { case SP_TAG_INT: return a.v.i == b.v.i; case SP_TAG_STR: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (sp_str_cmp_bytes(a.v.s, b.v.s) == 0); case SP_TAG_FLT: return a.v.f == b.v.f; case SP_TAG_BOOL: return a.v.b == b.v.b; case SP_TAG_NIL: return TRUE; case SP_TAG_SYM: return a.v.i == b.v.i; case SP_TAG_ENCODING: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_OBJ: /* Arrays compare by VALUE across storage kinds: [1,2] boxed as an IntArray equals the same numbers rebuilt as a PolyArray (a splat-rest, a mapped run). Ruby has one Array; the kinds are a storage optimization and must not leak into ==. */ if (sp_poly_is_array_kind(a.cls_id) && sp_poly_is_array_kind(b.cls_id)) { if (a.cls_id == b.cls_id && a.v.p == b.v.p) return TRUE; return sp_poly_eq_deep(a, b); } if (sp_poly_is_hash_kind(a.cls_id) && sp_poly_is_hash_kind(b.cls_id) && a.cls_id != b.cls_id) return sp_poly_eq_deep(a, b); if (a.cls_id != b.cls_id) return FALSE; if (a.v.p == b.v.p) return TRUE; switch (a.cls_id) { case SP_BUILTIN_INT_ARRAY: return sp_IntArray_eq((sp_IntArray*)a.v.p,(sp_IntArray*)b.v.p); case SP_BUILTIN_STR_ARRAY: return sp_StrArray_eq((sp_StrArray*)a.v.p,(sp_StrArray*)b.v.p); case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_eq((sp_FloatArray*)a.v.p,(sp_FloatArray*)b.v.p); case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_eq((sp_PolyArray*)a.v.p,(sp_PolyArray*)b.v.p); case SP_BUILTIN_TIME: { sp_Time *ta = (sp_Time*)a.v.p, *tb = (sp_Time*)b.v.p; /* two Times read out of containers compare by instant, not by box (#3699) */ return (ta && tb) ? (ta->tv_sec == tb->tv_sec && ta->tv_nsec == tb->tv_nsec) : (ta == tb); } case SP_BUILTIN_RATIONAL: { sp_Rational *ra = (sp_Rational*)a.v.p, *rb = (sp_Rational*)b.v.p; return (ra && rb) ? sp_rational_eq(*ra, *rb) : (ra == rb); } case SP_BUILTIN_TMS: { sp_Tms *ta = (sp_Tms*)a.v.p, *tb = (sp_Tms*)b.v.p; /* Process::Tms is a Struct: field-wise, like its other by-value siblings here */ return (ta && tb) ? (ta->utime == tb->utime && ta->stime == tb->stime && ta->cutime == tb->cutime && ta->cstime == tb->cstime) : (ta == tb); } /* the structural heap handles (Object's identity arm routes a poly operand here: emit_native_object_protocol) */ case SP_BUILTIN_OPENSTRUCT: return sp_OpenStruct_eq((sp_OpenStruct*)a.v.p, (sp_OpenStruct*)b.v.p); case SP_BUILTIN_EXCEPTION: return sp_exc_eq((sp_Exception*)a.v.p, (sp_Exception*)b.v.p); case SP_BUILTIN_IO: return sp_io_eq((sp_File*)a.v.p, (sp_File*)b.v.p); case SP_BUILTIN_COMPLEX: { sp_Complex *ca = (sp_Complex*)a.v.p, *cb = (sp_Complex*)b.v.p; return (ca && cb) ? (ca->re == cb->re && ca->im == cb->im) : (ca == cb); } case SP_BUILTIN_BIG_RATIONAL: { sp_BigRational *ra = (sp_BigRational*)a.v.p, *rb = (sp_BigRational*)b.v.p; return (ra && rb) ? (sp_bigint_cmp(ra->num, rb->num) == 0 && sp_bigint_cmp(ra->den, rb->den) == 0) : (ra == rb); } /* boxed hashes of the same variant compare by value like every other
     container -- the arm was simply missing, so [h] == [h-literal] was
     pointer identity and always false. */ case SP_BUILTIN_STR_INT_HASH: return sp_StrIntHash_eq((sp_StrIntHash*)a.v.p,(sp_StrIntHash*)b.v.p); case SP_BUILTIN_STR_STR_HASH: return sp_StrStrHash_eq((sp_StrStrHash*)a.v.p,(sp_StrStrHash*)b.v.p); case SP_BUILTIN_INT_STR_HASH: return sp_IntStrHash_eq((sp_IntStrHash*)a.v.p,(sp_IntStrHash*)b.v.p); case SP_BUILTIN_INT_INT_HASH: return sp_IntIntHash_eq((sp_IntIntHash*)a.v.p,(sp_IntIntHash*)b.v.p); case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH: return sp_poly_eq_deep(a, b); case SP_BUILTIN_RANGE: return sp_range_eq(*(sp_Range*)a.v.p,*(sp_Range*)b.v.p); case SP_BUILTIN_FLOAT_RANGE: { sp_FloatRange *fa=(sp_FloatRange*)a.v.p,*fb=(sp_FloatRange*)b.v.p; return (fa&&fb)?(fa->first==fb->first&&fa->last==fb->last&&fa->excl==fb->excl):(fa==fb); } case SP_BUILTIN_STR_RANGE: { sp_StrRange *sa=(sp_StrRange*)a.v.p,*sb=(sp_StrRange*)b.v.p; if(!sa||!sb)return sa==sb; return sa->excl==sb->excl&&sp_str_eq(sa->first,sb->first)&&sp_str_eq(sa->last,sb->last); } default: return sp_obj_eq_hook ? sp_poly_eq_deep(a, b) : FALSE; } case SP_TAG_CLASS: { const char *an = sp_class_val_name(a), *bn = sp_class_val_name(b); return (an && bn) ? strcmp(an, bn) == 0 : an == bn; } default: return FALSE; } }
/* The == arms that can walk back into the pair they started from: two arrays
   of any storage kinds, two hashes with poly values, and two user objects
   compared field-wise through the generated hook. A pair the comparison is
   already inside answers "equal", which is what lets `a << a; b << b; a == b`
   finish and is the answer CRuby's rb_exec_recursive_paired gives.

   Split out rather than guarded at the top of sp_poly_eq so the value types --
   Time, Rational, Range, Complex, IO, the primitive-keyed hashes -- never push
   a frame: none of them can contain a Ruby object, so none of them can meet
   itself, and sp_poly_eq is hot enough that an unconditional push would be
   felt by every comparison in a program that has no cycles at all. */
static sp_bool sp_poly_eq_deep(sp_RbVal a, sp_RbVal b) {
  if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) return TRUE;
  int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
  sp_bool r;
  if (sp_poly_is_array_kind(a.cls_id)) {
    sp_int n = sp_poly_length(a);
    r = (n == sp_poly_length(b));
    for (sp_int i = 0; r && i < n; i++)
      r = sp_poly_eq(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i));
  }
  else if (sp_poly_is_hash_kind(a.cls_id)) {
    /* The same dispatch the caller had, only guarded. Two hashes of DIFFERENT
       variants are compared by the generic cross-variant walk whatever their
       key types are; two of the same variant only reach here when that variant
       has poly values, since the primitive-keyed ones keep their own cases in
       sp_poly_eq and cannot hold a container at all. Each variant is named;
       one added later and not listed here takes the cross-variant walk, which
       is right for any two variants, only slower. */
    if (a.cls_id != b.cls_id)                      r = sp_poly_hash_eq_cross(a, b);
    else if (a.cls_id == SP_BUILTIN_STR_POLY_HASH) r = sp_StrPolyHash_eq((sp_StrPolyHash *)a.v.p, (sp_StrPolyHash *)b.v.p);
    else if (a.cls_id == SP_BUILTIN_SYM_POLY_HASH) r = sp_SymPolyHash_eq((sp_SymPolyHash *)a.v.p, (sp_SymPolyHash *)b.v.p);
    else if (a.cls_id == SP_BUILTIN_POLY_POLY_HASH) r = sp_PolyPolyHash_eq((sp_PolyPolyHash *)a.v.p, (sp_PolyPolyHash *)b.v.p);
    else                                           r = sp_poly_hash_eq_cross(a, b);
  }
  else r = sp_obj_eq_hook(a, b);   /* non-NULL: sp_poly_eq's object arm checks it first */
  sp_poly_recur_pop(mark);
  return r;
}
/* sp_sym_name_fn is now an extern hook (sp_gc.h / sp_gc.c) so cold lib readers
   like sp_json.c can resolve symbol names too; the generated TU still sets it. */
static sp_int sp_poly_arr_cmp(sp_RbVal a, sp_RbVal b, sp_bool *comparable);
/* A user class that mixes in Comparable defines `<=>` in generated code, which
   the runtime archive cannot call directly. The generated TU installs
   sp_obj_cmp_hook to dispatch `<=>` by cls_id; sp_poly_cmp consults it for two
   user objects so no-block sort/min/max/clamp compare correctly. A nil `<=>`
   result clears *comparable, which the callers turn into an ArgumentError. */
typedef sp_int (*sp_obj_cmp_fn)(sp_RbVal a, sp_RbVal b, sp_bool *comparable);
static sp_obj_cmp_fn sp_obj_cmp_hook = NULL;
#define SP_IS_BUILTIN_ARRAY(id) ((id) == SP_BUILTIN_INT_ARRAY || (id) == SP_BUILTIN_STR_ARRAY || \
                                 (id) == SP_BUILTIN_FLT_ARRAY || (id) == SP_BUILTIN_POLY_ARRAY)
/* A String compared with a user object that answers #to_str: CRuby's
   String#<=> converts the operand and compares the strings, and every ordered
   operator and #between? is built on that one method -- so the boxed side of
   the rule lands here, once, rather than in each of their entries. The
   receiver is rooted across the conversion: #to_str allocates, and the string
   reaching a boxed comparison is often a fresh one the caller holds in
   nothing but the argument. The OPERAND's root is one level down, inside
   sp_poly_check_str_obj, which is where both boxed callers meet. */
static int sp_poly_cmp_to_str(sp_RbVal a, sp_RbVal b, sp_int *out) {
  if (a.tag != SP_TAG_STR || !a.v.s || b.tag != SP_TAG_OBJ || b.cls_id < 0) return 0;
  SP_GC_ROOT_RBVAL(a);
  const char *s = sp_poly_check_str(b);
  if (!s) return 0;
  *out = sp_str_cmp_bytes(a.v.s, s);
  return 1;
}
static sp_int sp_poly_cmp(sp_RbVal a, sp_RbVal b, sp_bool *comparable) { /* same reasoning as the arithmetic fast paths: two plain numbers match no branch below until the numeric one, eight tests later */ if ((a.tag == SP_TAG_FLT || a.tag == SP_TAG_INT) && (b.tag == SP_TAG_FLT || b.tag == SP_TAG_INT)) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) { *comparable = TRUE; return (a.v.i > b.v.i) - (a.v.i < b.v.i); } sp_float _fa = a.tag == SP_TAG_FLT ? a.v.f : (sp_float)a.v.i; sp_float _fb = b.tag == SP_TAG_FLT ? b.v.f : (sp_float)b.v.i; *comparable = TRUE; return (_fa > _fb) - (_fa < _fb); } /* A user class's own `<=>` comes first, exactly as its own &/|/^ does (#3501): the Rational arm below answers for the RECEIVER's sake and a user object is not one of its operands, so `Fixed.new(1) < Rational(1,2)` reported the comparison as failed even though the class compares them perfectly well (#4038). */ if (a.tag == SP_TAG_OBJ && a.cls_id >= 0 && sp_obj_cmp_hook) return sp_obj_cmp_hook(a, b, comparable); if (sp_poly_is_brat(a) || sp_poly_is_brat(b) || sp_poly_is_rational(a) || sp_poly_is_rational(b)) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) { sp_float _af = sp_poly_to_f(a), _bf = sp_poly_to_f(b); *comparable = TRUE; return (_af > _bf) - (_af < _bf); } int _oka = sp_poly_is_brat(a) || sp_poly_is_rational(a) || a.tag == SP_TAG_INT || a.tag == SP_TAG_BIGINT; int _okb = sp_poly_is_brat(b) || sp_poly_is_rational(b) || b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT; if (_oka && _okb) { *comparable = TRUE; return sp_brat_cmp_poly(a, b); } *comparable = FALSE; return 0; } if (a.tag == SP_TAG_OBJ && b.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(a.cls_id) && SP_IS_BUILTIN_ARRAY(b.cls_id)) return sp_poly_arr_cmp(a, b, comparable); if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b); if (ba && bb) { *comparable = TRUE; return sp_bigint_cmp(ba, bb); } if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { sp_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } *comparable = FALSE; return 0; } if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { sp_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) { if (a.v.s == NULL || b.v.s == NULL) { *comparable = (a.v.s == b.v.s); return 0; } *comparable = TRUE; return sp_str_cmp_bytes(a.v.s, b.v.s); } { sp_int _sc; if (sp_poly_cmp_to_str(a, b, &_sc)) { *comparable = TRUE; return _sc; } } if (a.tag == SP_TAG_SYM && b.tag == SP_TAG_SYM) { *comparable = TRUE; if (sp_sym_name_fn) { /* byte-exact: a name may hold a NUL, and strcmp would call `:"a\0b"` equal to `:a` and order them arbitrarily (#nul symbols) */ const char *_na = sp_sym_name_fn((sp_sym)a.v.i), *_nb = sp_sym_name_fn((sp_sym)b.v.i); int _r = strcmp(_na, _nb); /* strcmp decides whenever the names differ before any NUL, and agrees with a byte-exact compare when it does. Only a TIE can hide a difference past an embedded NUL -- `:"a\0b"` against `:a` -- so the byte-exact compare runs just there (#nul symbols), keeping the ordinary comparison one pass. */ if (_r == 0) _r = sp_str_cmp_bytes(_na, _nb); return _r; } return (a.v.i > b.v.i) - (a.v.i < b.v.i); } if (sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b)) return sp_poly_cmp(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b), comparable); if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_TIME && b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_TIME && a.v.p && b.v.p) { *comparable = TRUE; return sp_time_cmp(*(sp_Time *)a.v.p, *(sp_Time *)b.v.p); } /* a boxed handle of the IO family answers as its typed <=> does: identity, or the mtime order of two stats (#sort over stats) */ if (a.tag == SP_TAG_OBJ && (a.cls_id == SP_BUILTIN_IO || a.cls_id == SP_BUILTIN_DIR)) { sp_RbVal _r = a.cls_id == SP_BUILTIN_IO ? sp_io_cmp((sp_File *)a.v.p, b) : sp_Dir_cmp((sp_Dir *)a.v.p, b); *comparable = _r.tag == SP_TAG_INT; return *comparable ? _r.v.i : 0; } if (a.tag == SP_TAG_OBJ && sp_obj_cmp_hook) return sp_obj_cmp_hook(a, b, comparable); *comparable = FALSE; return 0; }
/* Lexicographic <=> between two boxed int arrays (Array#<=> over int elems),
   so Array#max/min/sort work on an array of int pairs ([delta, idx] tuples). */
/* sp_poly_cmp_int_arrays: moved to lib/sp_cold.c */
sp_int sp_poly_cmp_int_arrays(sp_RbVal a, sp_RbVal b, sp_bool *comparable);
/* CRuby's Comparable operators raise on incomparable operands ("comparison
   of Float with nil failed"), they do not answer false. */
static const char *sp_cmperr_desc(sp_RbVal v);
static const char *sp_poly_class_name(sp_RbVal v);
static void sp_poly_cmp_fail(sp_RbVal a, sp_RbVal b) {
  sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed",
                                           sp_poly_class_name(a), sp_cmperr_desc(b)));
}
/* The `<=>` operator: sp_poly_cmp when the operands are mutually comparable,
   else Object#<=> -- 0 when they are the same object, nil otherwise. nil and
   the booleans are value-identity singletons, so `nil <=> nil` is 0 (while
   `nil < nil` still raises, since NilClass does not mix in Comparable). */
/* A user class's own comparison, when it defines one. sp_poly_cmp knows only
   the builtin orderings, so a boxed user object failed the comparison instead
   of calling the method the class wrote (#3501). */
static int sp_poly_user_cmp(const char *op, sp_RbVal a, sp_RbVal b, sp_RbVal *out) {
  if (!sp_poly_is_user_obj(a) || !sp_user_binop_hook) return 0;
  sp_bool h = FALSE;
  sp_RbVal r = sp_user_binop_hook(op, a, b, &h);
  if (!h) return 0;
  *out = r;
  return 1;
}
#define SP_POLY_USER_CMP(OP) do { sp_RbVal _u; if (sp_poly_user_cmp(OP, a, b, &_u)) return sp_poly_truthy(_u); } while (0)
/* The numeric coerce protocol from the ARGUMENT side: `5 < obj` is CRuby's
   `a, b = obj.coerce(5); a < b`, and `5.div(obj)` the same with div. Only the
   arithmetic operators consulted the hook (through sp_poly_binop_bad, #3960):
   sp_poly_cmp knows no ordering that pairs a number with a user object, and
   the named methods below converted the object to a number, so both answered
   where CRuby coerces. The receiver must NOT itself be a user object: that
   side is sp_poly_user_cmp's, and taking both here would let a coerce that
   answers its own operand recurse. */
static int sp_poly_coerce_binop(const char *op, sp_RbVal a, sp_RbVal b, sp_RbVal *out) {
  /* Only a NUMBER asks. CRuby reaches coerce from Numeric's own operators, so
     `"abc" < money` is a failed comparison however willing the object is to
     coerce -- and these entries serve every boxed value, not just numbers.
     Without this the protocol answered for a String, Symbol, Time or Array
     receiver: exactly the silent wrong answer it exists to remove. */
  if (!(sp_poly_numeric_p(a) || sp_poly_is_rational(a) || sp_poly_is_brat(a) ||
        (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX &&
         sp_complex_coerce_op_p(op)))) return 0;
  if (sp_poly_is_user_obj(a) || !sp_poly_is_user_obj(b) || !sp_user_coerce_hook) return 0;
  sp_bool h = FALSE;
  sp_RbVal r = sp_user_coerce_hook(op, a, b, &h);
  if (!h) return 0;
  *out = r;
  return 1;
}
#define SP_POLY_COERCE_CMP(OP) do { sp_RbVal _u; if (sp_poly_coerce_binop(OP, a, b, &_u)) return sp_poly_truthy(_u); } while (0)
/* the same guard for a method whose answer is the boxed value itself */
#define SP_POLY_COERCE_NUM(OP) do { sp_RbVal _u; if (sp_poly_coerce_binop(OP, a, b, &_u)) return _u; } while (0)
static sp_int sp_poly_spaceship(sp_RbVal a, sp_RbVal b) {
  { sp_RbVal _u; if (sp_poly_user_cmp("<=>", a, b, &_u))
      return _u.tag == SP_TAG_NIL ? SP_INT_NIL : sp_poly_to_i(_u); }
  { sp_RbVal _u; if (sp_poly_coerce_binop("<=>", a, b, &_u))
      return _u.tag == SP_TAG_NIL ? SP_INT_NIL : sp_poly_to_i(_u); }
  sp_bool comparable; sp_int cmp = sp_poly_cmp(a, b, &comparable);
  if (comparable) return cmp;
  if (a.tag == b.tag &&
      (a.tag == SP_TAG_NIL || (a.tag == SP_TAG_BOOL && a.v.b == b.v.b)))
    return 0;
  /* the default Object#<=> answers 0 when the operands are ==, nil otherwise
     -- so an object compared with itself is 0, not nil (#3017) */
  if (sp_poly_eq(a, b)) return 0;
  return SP_INT_NIL;
}
static sp_bool sp_poly_lt(sp_RbVal a, sp_RbVal b) { SP_POLY_USER_CMP("<"); SP_POLY_COERCE_CMP("<"); sp_bool comparable; sp_int cmp = sp_poly_cmp(a, b, &comparable); if (!comparable) sp_poly_cmp_fail(a, b); return cmp < 0; }
static sp_bool sp_poly_le(sp_RbVal a, sp_RbVal b) { SP_POLY_USER_CMP("<="); SP_POLY_COERCE_CMP("<="); sp_bool comparable; sp_int cmp = sp_poly_cmp(a, b, &comparable); if (!comparable) sp_poly_cmp_fail(a, b); return cmp <= 0; }
static sp_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { SP_POLY_USER_CMP(">"); SP_POLY_COERCE_CMP(">"); sp_bool comparable; sp_int cmp = sp_poly_cmp(a, b, &comparable); if (!comparable) sp_poly_cmp_fail(a, b); return cmp > 0; }
static sp_bool sp_poly_ge(sp_RbVal a, sp_RbVal b) { SP_POLY_USER_CMP(">="); SP_POLY_COERCE_CMP(">="); sp_bool comparable; sp_int cmp = sp_poly_cmp(a, b, &comparable); if (!comparable) sp_poly_cmp_fail(a, b); return cmp >= 0; }
/* Comparable#between? is defined on `<=>` alone: CRuby computes
   `(self <=> min) >= 0 && (self <=> max) <= 0` and raises "comparison failed"
   when either answers nil. Lowering it to `>=` and `<=` instead would
   answer for a class that defines those two and no `<=>`, where CRuby raises. */
static sp_bool sp_poly_cmp_ge0(sp_RbVal a, sp_RbVal b) {
  sp_int c = sp_poly_spaceship(a, b);
  if (c == SP_INT_NIL) sp_poly_cmp_fail(a, b);
  return c >= 0;
}
static sp_bool sp_poly_cmp_le0(sp_RbVal a, sp_RbVal b) {
  sp_int c = sp_poly_spaceship(a, b);
  if (c == SP_INT_NIL) sp_poly_cmp_fail(a, b);
  return c <= 0;
}
/* ORDERING (min / max / sort and their _by forms), as distinct from the
   Comparable OPERATORS above. nil defines #<=> and not #<=, so CRuby answers
   `nil <=> nil` with 0 -- [nil, nil].min is nil, .sort is the identity, and a
   sort key that is always nil ties everywhere -- while `nil <= nil` is a
   NoMethodError. Only the ordering entry may accept the pair (#4006). */
static sp_bool sp_poly_order_lt(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_NIL && b.tag == SP_TAG_NIL) return FALSE;
  return sp_poly_lt(a, b);
}
static sp_bool sp_poly_order_gt(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_NIL && b.tag == SP_TAG_NIL) return FALSE;
  return sp_poly_gt(a, b);
}
/* Float ** Float: CRuby promotes a negative base with a fractional exponent
   to a Complex. Spinel's float type cannot carry that, so the case raises
   loudly (Math::DomainError, the same class Math.sqrt(-1) uses) instead of
   returning C's silent NaN. See docs/limitations.md. */
static inline sp_float sp_float_pow(sp_float a, sp_float b) {
  if (a < 0 && b != (sp_float)(long long)b)
    sp_raise_cls("Math::DomainError",
                 "negative Float raised to a fractional power is a Complex (unsupported; use Complex(x) if needed)");
  return pow(a, b);
}
/* rb_cmperr operand description: special constants and Floats read as their
   inspect (3, 1.5, nil, true, :sym), everything else as its class name --
   "comparison of VerN with 3 failed", not "... with Integer failed". */
static const char *sp_cmperr_desc(sp_RbVal v) __attribute__((unused));
static const char *sp_cmperr_desc(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: case SP_TAG_FLT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return sp_poly_inspect(v);
    default: return sp_poly_class_name(v);
  }
}
/* rb_cmpint-checked comparison: an incomparable pair (nil `<=>`) raises the
   Comparable ArgumentError. Backs the object <,<=,>,>=,between? emitters when
   the user `<=>` can return nil (a TY_INT `<=>` keeps the inline fast path). */
static sp_int sp_poly_cmp_ck(sp_RbVal a, sp_RbVal b) __attribute__((unused));
static sp_int sp_poly_cmp_ck(sp_RbVal a, sp_RbVal b) {
  sp_bool ok = FALSE; sp_int r = sp_poly_cmp(a, b, &ok);
  if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a), sp_cmperr_desc(b)));
  return r;
}
/* Comparable#==: identity is equal; a nil `<=>` makes it false, never an
   error (CRuby cmp_equal). */
static sp_bool sp_poly_cmp_eq(sp_RbVal a, sp_RbVal b) __attribute__((unused));
static sp_bool sp_poly_cmp_eq(sp_RbVal a, sp_RbVal b) {
  if (a.tag == b.tag && a.v.p == b.v.p) return TRUE;
  sp_bool ok = FALSE; sp_int r = sp_poly_cmp(a, b, &ok);
  return ok ? (r == 0) : FALSE;
}
/* Comparable#clamp(lo, hi) for user objects, mirroring CRuby cmp_clamp: a nil
   bound clamps one-sided; both bounds present and lo > hi (or incomparable)
   raise ArgumentError; the result is the receiver or the applied bound. The
   user `<=>` dispatches through sp_obj_cmp_hook (via sp_poly_cmp). */
static sp_RbVal sp_obj_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) __attribute__((unused));
static sp_RbVal sp_obj_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  /* Each operand can be a heap object and sp_poly_cmp_ck dispatches the user
     `<=>` (which allocates), so root all three -- v is live but unused across
     the first lo<=>hi comparison, lo/hi across the later ones. */
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT_RBVAL(lo); SP_GC_ROOT_RBVAL(hi);
  if (lo.tag != SP_TAG_NIL && hi.tag != SP_TAG_NIL && sp_poly_cmp_ck(lo, hi) > 0)
    sp_raise_cls("ArgumentError", "min argument must be less than or equal to max argument");
  if (lo.tag != SP_TAG_NIL) {
    sp_int c1 = sp_poly_cmp_ck(v, lo);
    if (c1 == 0) return v;
    if (c1 < 0) return lo;
  }
  if (hi.tag != SP_TAG_NIL && sp_poly_cmp_ck(v, hi) > 0) return hi;
  return v;
}
/* Comparable#clamp(range) for user objects: an exclusive range with a real
   end cannot clamp (CRuby); beginless/endless endpoints (the INTPTR_MIN/MAX
   range sentinels) clamp one-sided as nil bounds. Integer endpoints are boxed
   and flow to the user `<=>` like any operand. */
static sp_RbVal sp_obj_clamp_range(sp_RbVal v, sp_Range r) __attribute__((unused));
static sp_RbVal sp_obj_clamp_range(sp_RbVal v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  sp_RbVal lo = r.first == INTPTR_MIN ? sp_box_nil() : sp_box_int(r.first);
  sp_RbVal hi = r.last == INTPTR_MAX ? sp_box_nil() : sp_box_int(r.last);
  return sp_obj_clamp(v, lo, hi);
}
/* Stable ascending sort of idx[0..n) by the poly key keys[idx[k]], leaving equal
   (or incomparable) keys in their original relative order. Backs sort_by's
   Schwartzian lowering: keys are precomputed once per element, so the comparator
   never re-runs the block. Bottom-up merge sort -- stable by construction, O(n log
   n), and (unlike qsort) carries the key array without a non-portable qsort_r. */
static void sp_sort_idx_by_poly(sp_int *idx, const sp_RbVal *keys, sp_int n) {
  if (n < 2) return;
  sp_int *tmp = (sp_int *)malloc(sizeof(sp_int) * (size_t)n);
  if (!tmp) sp_oom_die();
  sp_int *src = idx, *dst = tmp;
  for (sp_int width = 1; width < n; width *= 2) {
    for (sp_int lo = 0; lo < n; lo += 2 * width) {
      sp_int mid = lo + width < n ? lo + width : n;
      sp_int hi = lo + 2 * width < n ? lo + 2 * width : n;
      sp_int i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        sp_bool ok; sp_int c = sp_poly_cmp(keys[src[i]], keys[src[j]], &ok);
        if (!ok || c <= 0) dst[k++] = src[i++];   /* left wins ties -> stable */
        else dst[k++] = src[j++];
      }
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    sp_int *t = src; src = dst; dst = t;   /* ping-pong buffers; no per-level copy */
  }
  if (src != idx) for (sp_int x = 0; x < n; x++) idx[x] = src[x];   /* odd #levels: result is in tmp */
  free(tmp);
}
static sp_RbVal sp_poly_div(sp_RbVal a, sp_RbVal b) { /* Two plain numbers first, as add/sub/mul already do (#3984): none of the checks below can match either tag, and this is what a boxed arithmetic loop actually holds. */ if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(sp_idiv(a.v.i, b.v.i)); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f / b.v.f); /* before the tower branches, which match on the receiver kind and would convert a user object to a number of that kind */ if (SP_UNLIKELY(sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b))) return sp_poly_binop_bad("/", a, b); if (SP_UNLIKELY(sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b))) return sp_poly_div(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b)); if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("/", a, b); if ((sp_poly_is_brat(a) || sp_poly_is_brat(b))) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) / sp_poly_to_f(b)); return sp_brat_div_poly(a, b); } if ((sp_poly_is_rational(a) || sp_poly_is_rational(b)) && a.tag != SP_TAG_FLT && b.tag != SP_TAG_FLT) return sp_box_rational(sp_rational_div(sp_poly_as_rational(a), sp_poly_as_rational(b))); if ((a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX) || (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)) return sp_box_complex(sp_complex_div(sp_poly_as_complex(a), sp_poly_as_complex(b))); if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f_with_rational(a) / sp_poly_to_f_with_rational(b)); if ((a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT)) return sp_box_bigint(sp_bigint_div(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); return sp_box_int(sp_idiv(sp_poly_to_i(a), sp_poly_to_i(b))); }
static sp_RbVal sp_poly_str_mod(sp_RbVal a, sp_RbVal b);  /* fwd: defined beside the format helper */
static sp_RbVal sp_poly_mod(sp_RbVal a, sp_RbVal b) { /* Two plain numbers first, as add/sub/mul already do (#3984). */ if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(sp_imod(a.v.i, b.v.i)); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(sp_fmod(a.v.f, b.v.f)); if (a.tag == SP_TAG_STR || sp_poly_is_strbuf(a)) return sp_poly_str_mod(sp_poly_strbuf_deref(a), b); /* the user-object arm has to come before the float one: a Float on either side otherwise converted the object to a number (0.0) and answered a division by zero where CRuby coerces. */ if (sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b)) return sp_poly_binop_bad("%", a, b); /* a strbuf RECEIVER already returned through sp_poly_str_mod above */ if (SP_UNLIKELY(sp_poly_is_strbuf(b))) return sp_poly_mod(a, sp_poly_strbuf_deref(b)); if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("%", a, b); if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_fmod(sp_poly_to_f(a), sp_poly_to_f(b))); if (sp_poly_is_rational(a) || sp_poly_is_rational(b)) return sp_box_rational(sp_rational_mod(sp_poly_as_rational(a), sp_poly_as_rational(b))); if ((a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT)) return sp_box_bigint(sp_bigint_mod(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); return sp_box_int(sp_imod(sp_poly_to_i(a), sp_poly_to_i(b))); }  /* sp_fmod: CRuby divisor-sign result + zero-divisor raise */
/* divmod / quo on a boxed receiver. The typed paths build these inline per
   receiver kind; the poly path had neither, so an exact Rational reaching them
   through a block parameter raised NoMethodError on a method it answers (#3512).
   Exactness is the point: `quo` is Ruby's exact division, and a Rational
   divmod's remainder is a Rational. */
static sp_PolyArray *sp_PolyArray_new(void);                     /* fwd */
static void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v);      /* fwd */
/* Numeric#fdiv: both operands as Floats, always a Float result (#3767). */
static sp_float sp_poly_fdiv(sp_RbVal a, sp_RbVal b) {
  { sp_RbVal _u; if (sp_poly_coerce_binop("fdiv", a, b, &_u)) return sp_poly_to_f(_u); }
  if (!sp_poly_numeric_p(a)) sp_raise_poly_nomethod("fdiv", a);
  return sp_poly_to_f_with_rational(a) / sp_poly_to_f_with_rational(b);
}
static sp_RbVal sp_poly_divmod(sp_RbVal a, sp_RbVal b) {
  SP_POLY_COERCE_NUM("divmod");
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  if (sp_poly_is_rational(a) || sp_poly_is_rational(b)) {
    sp_Rational ra = sp_poly_as_rational(a), rb = sp_poly_as_rational(b);
    sp_int q = sp_rational_floor_i(sp_rational_div(ra, rb));
    sp_Rational rem = sp_rational_sub(ra, sp_rational_mul(sp_rational_new(q, 1), rb));
    sp_PolyArray_push(out, sp_box_int(q));
    sp_PolyArray_push(out, sp_box_rational(rem));
    return sp_box_poly_array(out);
  }
  if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) {
    sp_float fa = sp_poly_to_f(a), fb = sp_poly_to_f(b);
    if (fb == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
    sp_float q = floor(fa / fb);
    /* CRuby answers the quotient as an Integer (7.0.divmod(3) => [2, 1.0]);
       only one that no Integer can hold stays a Float. */
    sp_PolyArray_push(out, (q >= -9.2e18 && q <= 9.2e18) ? sp_box_int((sp_int)q) : sp_box_float(q));
    sp_PolyArray_push(out, sp_box_float(sp_fmod(fa, fb)));
    return sp_box_poly_array(out);
  }
  /* a Bignum pair: sp_poly_to_i below truncates it to 64 bits and answered a
     quotient seven orders of magnitude out, with a remainder of 0. */
  if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) {
    sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b);
    sp_PolyArray_push(out, sp_box_bigint(sp_bigint_div(ba, bb)));
    sp_PolyArray_push(out, sp_box_bigint(sp_bigint_mod(ba, bb)));
    return sp_box_poly_array(out);
  }
  {
    sp_int ia = sp_poly_to_i(a), ib = sp_poly_to_i(b);
    sp_PolyArray_push(out, sp_box_int(sp_idiv(ia, ib)));
    sp_PolyArray_push(out, sp_box_int(sp_imod(ia, ib)));
    return sp_box_poly_array(out);
  }
}
/* Numeric#div: the floor of the quotient, always an Integer, whatever the
   operands are (7.0.div(3) => 2). Distinct from `/`, which keeps the operand
   kind, and from #fdiv, which is always a Float (#3800). */
static sp_RbVal sp_poly_div_m(sp_RbVal a, sp_RbVal b) {
  SP_POLY_COERCE_NUM("div");
  if (!sp_poly_numeric_p(a) && !sp_poly_is_rational(a) && !sp_poly_is_brat(a))
    sp_raise_poly_nomethod("div", a);
  if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT ||
      sp_poly_is_rational(a) || sp_poly_is_rational(b)) {
    sp_float fb = sp_poly_to_f_with_rational(b);
    if (fb == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
    sp_float q = floor(sp_poly_to_f_with_rational(a) / fb);
    return (q >= -9.2e18 && q <= 9.2e18) ? sp_box_int((sp_int)q) : sp_box_float(q);
  }
  /* a Bignum operand: sp_poly_to_i truncates it to 64 bits, so this answered a
     number seven orders of magnitude out. sp_bigint_div floors toward -inf,
     which is what Integer#div does. */
  if ((a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT)) return sp_box_bigint(sp_bigint_div(sp_poly_as_bigint(a), sp_poly_as_bigint(b)));
  return sp_box_int(sp_idiv(sp_poly_to_i(a), sp_poly_to_i(b)));
}
/* Numeric#remainder: the remainder with the sign of the RECEIVER, which is
   what distinguishes it from #modulo ((-7).remainder(3) is -1, not 2). */
static sp_RbVal sp_poly_remainder(sp_RbVal a, sp_RbVal b) {
  SP_POLY_COERCE_NUM("remainder");
  if (!sp_poly_numeric_p(a) && !sp_poly_is_rational(a) && !sp_poly_is_brat(a))
    sp_raise_poly_nomethod("remainder", a);
  if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT ||
      sp_poly_is_rational(a) || sp_poly_is_rational(b)) {
    sp_float fa = sp_poly_to_f_with_rational(a), fb = sp_poly_to_f_with_rational(b);
    if (fb == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
    return sp_box_float(fa - fb * trunc(fa / fb));
  }
  if ((a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT)) return sp_box_bigint(sp_bigint_remainder(sp_poly_as_bigint(a), sp_poly_as_bigint(b)));
  { sp_int ia = sp_poly_to_i(a), ib = sp_poly_to_i(b);
    if (ib == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
    return sp_box_int(ia - ib * (ia / ib)); }
}
/* Numeric#coerce: [other, self], both lifted to the wider of the two kinds.
   The numeric protocol's entry point, so a boxed receiver has to answer it. */
static sp_RbVal sp_poly_coerce(sp_RbVal a, sp_RbVal b) {
  if (!sp_poly_numeric_p(a)) sp_raise_poly_nomethod("coerce", a);
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) {
    sp_PolyArray_push(out, sp_box_float(sp_poly_to_f_with_rational(b)));
    sp_PolyArray_push(out, sp_box_float(sp_poly_to_f_with_rational(a)));
  }
  else if (sp_poly_numeric_p(b)) {
    sp_PolyArray_push(out, b);
    sp_PolyArray_push(out, a);
  }
  else {
    /* A non-numeric OPERAND coerces through Float(), which is where CRuby's
       messages come from: `can't convert nil into Float`, and a String's
       `invalid value for Float(): "x"`. Pushing it unchanged answered a pair
       containing the string itself (#4011). */
    sp_PolyArray_push(out, sp_box_float(sp_poly_Float(b)));
    sp_PolyArray_push(out, sp_box_float(sp_poly_to_f_with_rational(a)));
  }
  return sp_box_poly_array(out);
}
static sp_RbVal sp_poly_quo(sp_RbVal a, sp_RbVal b) {
  SP_POLY_COERCE_NUM("quo");
  if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT)
    return sp_box_float(sp_poly_to_f_with_rational(a) / sp_poly_to_f_with_rational(b));
  return sp_box_rational(sp_rational_div(sp_poly_as_rational(a), sp_poly_as_rational(b)));
}
/* Comparable#clamp on boxed numerics, faithful to CRuby: the result is the
   applied operand returned UNCHANGED, so an in-range Integer receiver stays
   Integer while a Float bound that clamps stays Float (5.clamp(1.0, 3.0) is
   3.0 but 5.clamp(1.0, 10.0) is 5). The lo<=>hi then self<=>bound checks
   mirror CRuby's NaN/ordering ArgumentErrors. The operand class and value in
   the message go through the generic sp_poly_class_name / sp_poly_to_s helpers
   (as the sort/min/max comparison errors already do), so a non-numeric poly
   bound names its real class and renders its value rather than reinterpreting
   the union as an integer. */
static sp_RbVal sp_num_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  sp_float dv = sp_poly_to_f(v), dlo = sp_poly_to_f(lo), dhi = sp_poly_to_f(hi);
  if (dlo != dlo || dhi != dhi)
    sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(lo), sp_poly_to_s(hi)));
  if (dlo > dhi)
    sp_raise_cls("ArgumentError", "min argument must be less than or equal to max argument");
  if (dv != dv)
    sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(v), sp_poly_to_s(lo)));
  if (dv < dlo) return lo;
  if (dv > dhi) return hi;
  return v;
}
/* clamp(lo, hi) where a nil bound is an open (unbounded) side: a nil lo skips
   the lower comparison, a nil hi the upper. Returns the chosen boxed operand so
   its Integer/Float class is preserved. Both bounds present falls back to the
   checked sp_num_clamp (which raises on lo > hi). */
static sp_RbVal sp_num_clamp_open(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  int has_lo = lo.tag != SP_TAG_NIL, has_hi = hi.tag != SP_TAG_NIL;
  if (has_lo && has_hi) return sp_num_clamp(v, lo, hi);
  sp_float dv = sp_poly_to_f(v);
  if (has_lo) return dv < sp_poly_to_f(lo) ? lo : v;
  if (has_hi) return dv > sp_poly_to_f(hi) ? hi : v;
  return v;
}
/* clamp on a boxed value: numerics route through sp_num_clamp so the returned
   operand keeps its own Integer/Float class; a user object anywhere in the
   triple routes through sp_obj_clamp (the user `<=>` via the cmp hook) instead
   of being reinterpreted as a float. */
static sp_RbVal sp_poly_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  if ((v.tag == SP_TAG_OBJ && !sp_poly_numeric_p(v)) ||
      (lo.tag == SP_TAG_OBJ && !sp_poly_numeric_p(lo)) ||
      (hi.tag == SP_TAG_OBJ && !sp_poly_numeric_p(hi)))
    return sp_obj_clamp(v, lo, hi);
  return sp_num_clamp(v, lo, hi);
}
/* clamp(range) on a boxed value: an exclusive range with a real end cannot
   clamp (CRuby); the INTPTR_MIN/MAX beginless/endless sentinels act as
   unbounded sides for numerics and nil bounds for user objects. */
static sp_RbVal sp_poly_clamp_range(sp_RbVal v, sp_Range r) __attribute__((unused));
static sp_RbVal sp_poly_clamp_range(sp_RbVal v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  if (v.tag == SP_TAG_OBJ && !sp_poly_numeric_p(v)) return sp_obj_clamp_range(v, r);
  return sp_num_clamp(v, sp_box_int(r.first), sp_box_int(r.last));
}
/* Integer #** : Spinel has no Rational, so a negative integer exponent --
   which CRuby evaluates to a Rational like (1/2) -- raises RangeError rather
   than silently truncating toward 0. Float ** negative stays a float
   (CRuby-compatible: 2.0 ** -1 == 0.5). See docs/limitations.md. */
/* Integer#round(ndigits, half: mode): mode 0 = :even (banker's),
   1 = :up (default), 2 = :down. Positive ndigits are a no-op for ints. */
sp_int sp_int_round_half(sp_int v, sp_int nd, int mode) __attribute__((unused));
/* sp_int_round_half: moved to lib/sp_cold.c */
sp_int sp_int_round_half(sp_int v, sp_int nd, int mode);
static sp_RbVal sp_poly_pow(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) {
    /* CRuby: a negative integer exponent yields a Rational. The non-negative
       path squares-and-multiplies exactly (the old pow(double) round-trip was
       lossy past 2^53); an overflowing result promotes to Bignum under
       --int-overflow=promote, else sp_int_pow raises/wraps per mode. */
    if (b.v.i < 0) return sp_box_rational(sp_rational_pow(sp_rational_new(a.v.i, 1), b.v.i));
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
    sp_int r = 1, base = a.v.i, exp = b.v.i;
    int ovf = 0;
    while (exp > 0 && !ovf) {
      if (exp & 1) ovf |= sp_int_mul_overflow_p(r, base, &r);
      exp >>= 1;
      if (exp) ovf |= sp_int_mul_overflow_p(base, base, &base);
    }
    if (ovf || r == SP_INT_NIL)
      return sp_box_bigint(sp_bigint_pow(sp_bigint_new_int(a.v.i), b.v.i));
    return sp_box_int(r);
#else
    return sp_box_int(sp_int_pow(a.v.i, b.v.i));
#endif
  }
  if (a.tag == SP_TAG_BIGINT && (b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT)) {
    sp_int e = sp_poly_to_i(b);
    if (e < 0) sp_raise_cls("RangeError", "negative exponent");
    return sp_box_bigint(sp_bigint_pow((sp_Bigint *)a.v.p, e));
  }
  if (sp_poly_is_user_obj(a) || sp_poly_is_user_obj(b)) return sp_poly_binop_bad("**", a, b);
  if (SP_UNLIKELY(sp_poly_is_strbuf(a) || sp_poly_is_strbuf(b)))
    return sp_poly_pow(sp_poly_strbuf_deref(a), sp_poly_strbuf_deref(b));
  if (SP_UNLIKELY(sp_poly_tower_mismatch(a, b))) return sp_poly_binop_bad("**", a, b);
  /* An exact receiver keeps its class through `**`, as it does on the typed
     path: a Rational raised to an integer is a Rational, and a Complex is a
     Complex. Falling to pow(double, double) evaluated them in floats, which is
     a different answer as well as a different class (#3510). */
  if (sp_poly_is_rational(a)) {
    sp_Rational ra = sp_poly_as_rational(a);
    if (b.tag == SP_TAG_INT) return sp_box_rational(sp_rational_pow(ra, b.v.i));
    if (sp_poly_is_rational(b)) {
      sp_Rational rb = sp_poly_as_rational(b);
      if (rb.den == 1) return sp_box_rational(sp_rational_pow(ra, rb.num));
    }
  }
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX) {
    sp_Complex ca = sp_poly_as_complex(a);
    if (b.tag == SP_TAG_INT) return sp_box_complex(sp_complex_pow(ca, b.v.i));
    if (sp_poly_is_rational(b)) return sp_box_complex(sp_complex_pow_rational(ca, sp_poly_as_rational(b)));
    if (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_COMPLEX)
      return sp_box_complex(sp_complex_pow_c(ca, sp_poly_as_complex(b)));
    if (b.tag == SP_TAG_FLT)
      return sp_box_complex(sp_complex_pow_c(ca, (sp_Complex){b.v.f, 0.0, SP_CPLX_RE_F | SP_CPLX_IM_F}));
  }
  double r = pow((double)sp_poly_to_f(a), (double)sp_poly_to_f(b));
  return sp_box_float((sp_float)r);
}
/* sp_poly_shl is defined after sp_PolyArray_push (below) so the
   push-dispatch path can call it directly. The Integer-bit-shift
   semantics fall through when the recv isn't an array. */
static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b);
/* Proc#>> / #<< compose rather than shift. sp_Proc is not declared yet here,
   so go through a void* shim defined beside sp_proc_compose (#2880). */
static void *sp_proc_compose_v(void *outer, void *inner);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);   /* fwd */
static sp_RbVal sp_poly_shr(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_PROC &&
      b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_PROC)
    return sp_box_proc(sp_proc_compose_v(b.v.p, a.v.p));
  /* a bignum shifts in bignum space: truncating to int64 first turned a
     positive value past 2^63 negative, so the shift became arithmetic and a
     masked xorshift diverged from CRuby (#3371) */
  if (a.tag == SP_TAG_BIGINT)
    return sp_box_bigint(sp_bigint_shr((sp_Bigint *)a.v.p, (int64_t)sp_poly_to_i(b)));
  if (sp_poly_is_user_obj(a)) return sp_poly_binop_bad(">>", a, b);
  /* Integer#>> is Integer's alone (see sp_poly_bitop) */
  if (a.tag != SP_TAG_INT) return sp_poly_binop_bad(">>", a, b);
  return sp_box_int(sp_poly_to_i(a) >> sp_poly_to_i(b));
}
/* & | ^ are boolean operators on a nil/boolean receiver (NilClass#& is
   always false, | and ^ test the operand's truthiness) and bitwise on an
   integer receiver. */
/* Two boxed ARRAYS: `&` and `|` are Array's set operations, as `+` and `-`
   already were on this path. Read as bitwise integer arithmetic they answered
   0 -- a fold over an array of arrays lost its result silently (#3966). */
static int sp_poly_both_arrays(sp_RbVal a, sp_RbVal b) {
  return a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id) &&
         b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id);
}
static sp_RbVal sp_poly_band(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_NIL) return sp_box_bool(0);
  if (a.tag == SP_TAG_BOOL) return sp_box_bool(a.v.b && sp_poly_truthy(b));
  if (sp_poly_both_arrays(a, b)) {
    SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
    sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa);
    sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb);
    return sp_box_poly_array(sp_PolyArray_intersect(pa, pb));
  }
  if (sp_poly_is_user_obj(a)) return sp_poly_binop_bad("&", a, b);
  /* Integer#& is Integer's alone (see sp_poly_bitop) */
  if (a.tag != SP_TAG_INT && a.tag != SP_TAG_BIGINT) return sp_poly_binop_bad("&", a, b);
  return sp_box_int(sp_poly_to_i(a) & sp_poly_to_i(b));
}
static sp_RbVal sp_poly_bor(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_NIL) return sp_box_bool(sp_poly_truthy(b));
  if (a.tag == SP_TAG_BOOL) return sp_box_bool(a.v.b || sp_poly_truthy(b));
  if (sp_poly_both_arrays(a, b)) {
    SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
    sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa);
    sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb);
    return sp_box_poly_array(sp_PolyArray_union(pa, pb));
  }
  if (sp_poly_is_user_obj(a)) return sp_poly_binop_bad("|", a, b);
  /* Integer#| is Integer's alone (see sp_poly_bitop) */
  if (a.tag != SP_TAG_INT && a.tag != SP_TAG_BIGINT) return sp_poly_binop_bad("|", a, b);
  return sp_box_int(sp_poly_to_i(a) | sp_poly_to_i(b));
}
static sp_RbVal sp_poly_bxor(sp_RbVal a, sp_RbVal b) {
  if (a.tag == SP_TAG_NIL) return sp_box_bool(sp_poly_truthy(b));
  if (a.tag == SP_TAG_BOOL) return sp_box_bool(a.v.b != sp_poly_truthy(b));
  if (sp_poly_is_user_obj(a)) return sp_poly_binop_bad("^", a, b);
  /* Integer#^ is Integer's alone (see sp_poly_bitop) */
  if (a.tag != SP_TAG_INT && a.tag != SP_TAG_BIGINT) return sp_poly_binop_bad("^", a, b);
  return sp_box_int(sp_poly_to_i(a) ^ sp_poly_to_i(b));
}
/* Unary minus on a boxed value. Only Float and Integer were handled, and
   everything else fell to sp_poly_to_i -- which answers 0 for a Rational or a
   Complex, so `[Rational(1,10)].map { |r| -r }` answered [0] with nothing
   said (#4299). Each kind negates through its own helper, the way sp_poly_mul
   dispatches its tower one line at a time. */
static sp_RbVal sp_poly_neg(sp_RbVal a) {
  if (a.tag == SP_TAG_FLT) return sp_box_float(-a.v.f);
  if (a.tag == SP_TAG_INT) return sp_box_int(-a.v.i);
  if (a.tag == SP_TAG_BIGINT)
    return sp_box_bigint(sp_bigint_sub(sp_bigint_new_int(0), (sp_Bigint *)a.v.p));
  if (sp_poly_is_rational(a)) return sp_box_rational(sp_rational_neg(sp_poly_as_rational(a)));
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_COMPLEX)
    return sp_box_complex(sp_complex_neg(sp_poly_as_complex(a)));
  if (sp_poly_is_brat(a)) return sp_brat_sub_poly(sp_box_int(0), a);
  return sp_box_int(-sp_poly_to_i(a));
}

/* sp_mark_rbval: inline helper in sp_gc.h. */
/* Definition of the root-entry marker forward-declared near
   sp_gc_mark_all: a low-bit-tagged slot is an sp_RbVal* root. */
/* sp_gc_mark_root_entry is an inline helper in sp_gc.h. */
static sp_RbVal sp_PolyArray_pop(sp_PolyArray *a) { if (!a || a->len <= 0) return sp_box_nil(); if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return sp_box_nil(); } return a->data[--a->len]; }
/* log(|Gamma(x)|) for x > 0, via the Stirling asymptotic series pushed into
   its accurate region (x >= 12) by the recurrence Gamma(x) = Gamma(x+1)/x. We
   compute it ourselves rather than calling the platform `lgamma_r`, whose
   last-ULP result varies between libm implementations (macOS vs glibc) and
   would make the golden test output machine-specific. The Bernoulli series
   below carries it to ~1e-15. Gamma(1) and Gamma(2) are exactly 1, so their
   logs are returned as an exact 0 rather than a rounding-noise residual. */
/* sp_lgamma_pos: moved to lib/sp_cold.c */
double sp_lgamma_pos(double x);
/* Math.lgamma(x) -> [log(|gamma(x)|), sign of gamma(x)]. */
/* sp_math_lgamma: moved to lib/sp_cold.c */
sp_PolyArray *sp_math_lgamma(double x);
static sp_RbVal sp_PolyArray_shift(sp_PolyArray *a) { if (!a || a->len <= 0) return sp_box_nil(); if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return sp_box_nil(); } sp_RbVal v = a->data[0]; memmove(a->data, a->data+1, (size_t)(--a->len)*sizeof(sp_RbVal)); return v; }
static sp_RbVal sp_PolyArray_delete_at(sp_PolyArray *a, sp_int i) {sp_gc_wb((void*)a);  if (!a) return sp_box_nil(); if (i < 0) i += a->len; if (i < 0 || i >= a->len) return sp_box_nil(); sp_RbVal v = a->data[i]; for (sp_int j = i; j < a->len - 1; j++) a->data[j] = a->data[j+1]; a->len--; return v; }
static void sp_PolyArray_insert(sp_PolyArray *a, sp_int i, sp_RbVal v) {sp_gc_wb((void*)a);  if (!a) return; if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return; } sp_int orig = i; if (i < 0) i += a->len + 1; if (i < 0) sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld", (long long)orig, (long long)(-(a->len + 1)))); while (i > a->len) sp_PolyArray_push(a, sp_box_nil()); /* CRuby pads with nils past the end */ sp_PolyArray_push(a, sp_box_nil()); for (sp_int j = a->len - 1; j > i; j--) a->data[j] = a->data[j-1]; a->data[i] = v; }
/* Array#delete(v): removes every element sp_poly_eq to v, returns v (or
   nil if not found). Was missing for TY_POLY_ARRAY -- only TY_INT_ARRAY/
   TY_STR_ARRAY had it -- which blocked the array-backed Set package's
   #delete (doom's `@secret_sectors.delete(sector_idx)`). Lives here (not
   sp_array.c, home of sp_IntArray_delete et al) because it needs
   sp_poly_eq, which is inline-per-TU in this file, not linkable from the
   separately-compiled cold array library. */
static sp_RbVal sp_PolyArray_delete(sp_PolyArray *a, sp_RbVal v) {sp_gc_wb((void*)a); 
  if (a && a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return sp_box_nil(); }
  if (!a) return sp_box_nil();
  /* sp_poly_eq can allocate (bigint promotion) and so trigger a collection
     mid-loop; a and v may be reachable only through the call expression. */
  SP_GC_ROOT(a); SP_GC_ROOT_RBVAL(v);
  sp_int w = 0;
  sp_bool found = FALSE;
  for (sp_int i = 0; i < a->len; i++) {
    if (!sp_poly_eq(a->data[i], v)) { a->data[w] = a->data[i]; w++; }
    else found = TRUE;
  }
  a->len = w;
  return found ? v : sp_box_nil();
}

/* MatchData — holds the source string and the per-group byte offsets
   the engine produced. `[]`/captures extract substrings on demand;
   offset/begin/end report CHARACTER offsets (CRuby semantics), so
   byte offsets are converted via sp_str_count_chars. Group i occupies
   caps[2i] (begin) and caps[2i+1] (end); -1 marks a non-participating
   group. Issue #974. */

static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b) {
  /* Proc#<< composes the other way round: `f << g` calls g then f (#2880) */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_PROC &&
      b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_PROC)
    return sp_box_proc(sp_proc_compose_v(a.v.p, b.v.p));
  /* a user class's own #<< comes before every builtin reading of the
     operator -- the object is not an array and not an integer (#3502) */
  if (sp_poly_is_user_obj(a)) return sp_poly_binop_bad("<<", a, b);
  /* Dispatch by recv cls_id: an IntArray / PolyArray / etc. boxed
     into a poly slot still wants Array#<< (push), not Integer#<<
     (bit-shift). Falls through to bit-shift only when the recv is
     a non-array. Returns the recv (matching `<<`s chainability). */
  if (a.tag == SP_TAG_OBJ) {
    if (a.cls_id == SP_BUILTIN_INT_ARRAY) {
      sp_IntArray_push((sp_IntArray *)a.v.p, b.tag == SP_TAG_INT ? b.v.i : sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_POLY_ARRAY) {
      sp_PolyArray_push((sp_PolyArray *)a.v.p, b);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_PTR_ARRAY) {
      sp_PtrArray_push((sp_PtrArray *)a.v.p, b.v.p);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_FLT_ARRAY) {
      sp_FloatArray_push((sp_FloatArray *)a.v.p, b.tag == SP_TAG_FLT ? b.v.f : (sp_float)sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_STR_ARRAY) {
      /* a shared-mutable handle pushed into a typed string array stores its
         CONTENTS (the typed storage cannot hold the handle); an empty push
         here silently blanked the element (#3327) */
      const char *_es = b.tag == SP_TAG_STR ? (const char *)b.v.p
                      : (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_STRBUF && b.v.p)
                          ? sp_String_cstr((sp_String *)b.v.p)
                          : sp_str_empty;
      sp_StrArray_push((sp_StrArray *)a.v.p, _es);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_STRBUF && a.v.p) {
      /* String#<< on a shared-mutable handle appends IN PLACE: the mutation
         must stay visible to every alias of the handle (#3227 phase 3) */
      if (b.tag == SP_TAG_STR || sp_poly_is_strbuf(b)) {
        sp_RbVal _bs = sp_poly_strbuf_deref(b);
        sp_String_append_bin((sp_String *)a.v.p, _bs.v.s ? _bs.v.s : sp_str_empty);
        return a;
      }
      /* an Integer operand appends its CODEPOINT, and one outside the range is
         a RangeError -- the fallthrough read the receiver as a number and
         answered an integer shift instead (#4015) */
      if (b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT) {
        sp_String_append_bin((sp_String *)a.v.p, sp_int_codepoint_to_str(sp_poly_to_i(b)));
        return a;
      }
      return sp_poly_binop_bad("<<", a, b);
    }
    if (a.cls_id == SP_BUILTIN_IO && a.v.p) {
      /* IO#<< through a boxed handle writes and chains (#2802) */
      /* A String operand writes its own byte count; only a stringified value
         needs strlen (sp_poly_to_s can answer an unmarked static name). Without
         the split, `io << binary` truncated or not depending on whether
         inference typed the operand -- the same bug, but intermittent. */
      if (b.tag == SP_TAG_STR && b.v.s) sp_File_write_bin((sp_File *)a.v.p, b.v.s);
      else sp_File_write((sp_File *)a.v.p, sp_poly_to_s(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_QUEUE && a.v.p) {
      /* Queue#<< (and #push / #enq, which lower to it) appends and chains.
         Without an arm the boxed handle fell past every case to the Integer
         bit-shift fallback below, so a push through a yielded Queue was
         silently discarded and the queue stayed empty. */
      sp_Queue_push((sp_queue *)a.v.p, b);
      return a;
    }
    /* A user object with a `<<` method (a Set held as a Hash value, #3174):
       dispatch through the user-binop hook, which mutates it in place and
       returns the receiver -- not the Integer#<< bit-shift fallback below. */
    if (a.cls_id >= 0) return sp_poly_binop_bad("<<", a, b);
  }
  /* String#<< appends (sp_str_concat treats NULL as the empty string) */
  if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR)
    return sp_box_str(sp_str_concat(a.v.s, b.v.s));
  if (a.tag == SP_TAG_STR && sp_poly_is_strbuf(b))
    return sp_box_str(sp_str_concat(a.v.s, sp_String_cstr((sp_String *)b.v.p)));
  /* A String receiver with an Integer operand appends that CODEPOINT. Reading
     the receiver as a number instead answered an integer shift, so `s << -1`
     gave 0 where CRuby raises RangeError (#4015). */
  if (a.tag == SP_TAG_STR && (b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT))
    return sp_box_str(sp_str_concat(a.v.s, sp_int_codepoint_to_str(sp_poly_to_i(b))));
  /* Integer#<< is Integer's alone (see sp_poly_bitop); Array, String, IO,
     Queue, Proc and a user class were all answered above. */
  if (a.tag != SP_TAG_INT && a.tag != SP_TAG_BIGINT) return sp_poly_binop_bad("<<", a, b);
  /* Integer#<<. A Bignum receiver shifts as a Bignum; an int receiver whose
     result escapes the word promotes under --int-overflow=promote and
     raises/wraps per mode otherwise (sp_int_shl carries those semantics). */
  if (a.tag == SP_TAG_BIGINT)
    return sp_box_bigint(sp_bigint_shl((sp_Bigint *)a.v.p, sp_poly_to_i(b)));
  {
    sp_int x = sp_poly_to_i(a), n = sp_poly_to_i(b);
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
    if (n >= 0 && x != 0) {
      sp_int r = n >= 64 ? 0 : (sp_int)((uintptr_t)x << n);
      if (n >= 64 || (r >> n) != x || r == SP_INT_NIL)
        return sp_box_bigint(sp_bigint_shl(sp_bigint_new_int(x), n));
    }
#endif
    return sp_box_int(sp_int_shl(x, n));
  }
}
/* Fold an operator named by a runtime symbol (`arr.reduce(sym)` where sym is
   not statically known, e.g. a block param): dispatch the common arithmetic,
   bitwise, and shift operators on the boxed operands. An unknown operator is a
   NoMethodError, like sending it. */
static sp_RbVal sp_poly_binop_sym(sp_RbVal a, sp_sym op, sp_RbVal b) {
  const char *s = sp_sym_name_fn ? sp_sym_name_fn(op) : "";
  if (s && s[0] && !s[1]) {
    switch (s[0]) {
      case '+': return sp_poly_add(a, b);
      case '-': return sp_poly_sub(a, b);
      case '*': return sp_poly_mul(a, b);
      case '/': return sp_poly_div(a, b);
      case '%': return sp_poly_mod(a, b);
      case '&': return sp_poly_band(a, b);
      case '|': return sp_poly_bor(a, b);
      case '^': return sp_poly_bxor(a, b);
      default: break;
    }
  }
  if (s && strcmp(s, "**") == 0) return sp_poly_pow(a, b);
  if (s && strcmp(s, "<<") == 0) return sp_poly_shl(a, b);
  if (s && strcmp(s, ">>") == 0) return sp_poly_shr(a, b);
  return sp_poly_binop_bad(s ? s : "", a, b);
}
/* Apply a binary operator named by string to two boxed operands. Used by the
   generated coerce dispatch to finish the protocol on the pair #coerce
   answered; an unknown operator answers nil rather than raising, and the
   caller's original TypeError stands. */
static sp_RbVal sp_poly_binop_apply(const char *op, sp_RbVal a, sp_RbVal b) {
  if (!op) return sp_box_nil();
  if (op[1] == 0) {
    switch (op[0]) {
      case '+': return sp_poly_add(a, b);
      case '-': return sp_poly_sub(a, b);
      case '*': return sp_poly_mul(a, b);
      case '/': return sp_poly_div(a, b);
      case '%': return sp_poly_mod(a, b);
      case '<': return sp_box_bool(sp_poly_lt(a, b));
      case '>': return sp_box_bool(sp_poly_gt(a, b));
      default: break;
    }
  }
  if (strcmp(op, "**") == 0) return sp_poly_pow(a, b);
  /* The ordered comparisons and the named numeric methods finish the protocol
     the same way the operators do -- CRuby routes all of them through coerce,
     and each already has a boxed helper here. Left out, the pair #coerce
     answered was computed and then thrown away for every operation but the
     six arithmetic ones. */
  if (strcmp(op, "<=") == 0) return sp_box_bool(sp_poly_le(a, b));
  if (strcmp(op, ">=") == 0) return sp_box_bool(sp_poly_ge(a, b));
  if (strcmp(op, "<=>") == 0) {
    sp_int _c = sp_poly_spaceship(a, b);
    return _c == SP_INT_NIL ? sp_box_nil() : sp_box_int(_c);
  }
  if (strcmp(op, "div") == 0) return sp_poly_div_m(a, b);
  if (strcmp(op, "modulo") == 0) return sp_poly_mod(a, b);
  if (strcmp(op, "remainder") == 0) return sp_poly_remainder(a, b);
  if (strcmp(op, "quo") == 0) return sp_poly_quo(a, b);
  if (strcmp(op, "fdiv") == 0) return sp_box_float(sp_poly_fdiv(a, b));
  if (strcmp(op, "divmod") == 0) return sp_poly_divmod(a, b);
  return sp_box_nil();
}
static sp_int sp_PolyArray_length(sp_PolyArray *a) { if (!a) return 0; return a->len; }
/* Helpers for iterating over a poly value that holds a boxed array. */
static sp_int sp_poly_arr_len(sp_RbVal a) {
  if (a.tag != SP_TAG_OBJ) return 0;
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: return ((sp_IntArray *)a.v.p)->len;
    case SP_BUILTIN_FLT_ARRAY: return ((sp_FloatArray *)a.v.p)->len;
    case SP_BUILTIN_STR_ARRAY: return ((sp_StrArray *)a.v.p)->len;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)a.v.p)->len;
    case SP_BUILTIN_STR_INT_HASH: return ((sp_StrIntHash *)a.v.p)->len;
    case SP_BUILTIN_STR_STR_HASH: return ((sp_StrStrHash *)a.v.p)->len;
    case SP_BUILTIN_INT_STR_HASH: return ((sp_IntStrHash *)a.v.p)->len;
    case SP_BUILTIN_INT_INT_HASH: return ((sp_IntIntHash *)a.v.p)->len;
    default: return 0;
  }
}
/* n-way Cartesian product: given `n` array values, return a poly array of the
   `len(arrs[0]) * len(arrs[1]) * ...` tuples, each a poly array with one element
   drawn from each input in order (an odometer over the index vector). The caller
   keeps `arrs[i]` rooted across this call; `res` and the current `tuple` are
   rooted here so a push-triggered collection cannot reclaim a partial tuple.
   Any empty input yields an empty product. */
static sp_PolyArray *sp_poly_product(sp_RbVal *arrs, sp_int n) {
  sp_PolyArray *res = sp_PolyArray_new(); SP_GC_ROOT(res);
  sp_int total = 1;
  for (sp_int i = 0; i < n; i++) {
    sp_int len = sp_poly_arr_len(arrs[i]);
    if (len == 0) { total = 0; break; }
    /* guard the running product against sp_int overflow, which would wrap to a
       bogus (small/negative) total and silently truncate the result. */
    if (total > INTPTR_MAX / len) sp_raise_cls("RangeError", "too big to product");
    total *= len;
  }
  if (total <= 0) return res;
  /* small stack buffer for the odometer, heap only for an implausibly large
     arity -- avoids a calloc that a raise/longjmp from the loop would leak. */
  sp_int idx_stack[16];
  sp_int *idx = idx_stack;
  if (n > (sp_int)(sizeof idx_stack / sizeof idx_stack[0])) {
    idx = (sp_int *)calloc((size_t)n, sizeof(sp_int));
    if (!idx) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  else {
    memset(idx, 0, (size_t)n * sizeof(sp_int));
  }
  sp_PolyArray *tuple = NULL; SP_GC_ROOT(tuple);
  for (sp_int t = 0; t < total; t++) {
    tuple = sp_PolyArray_new();
    for (sp_int i = 0; i < n; i++)
      sp_PolyArray_push(tuple, sp_poly_arr_get(arrs[i], idx[i]));
    sp_PolyArray_push(res, sp_box_poly_array(tuple));
    for (sp_int i = n - 1; i >= 0; i--) {
      if (++idx[i] < sp_poly_arr_len(arrs[i])) break;
      idx[i] = 0;
    }
  }
  if (idx != idx_stack) free(idx);
  return res;
}
/* `when *arr`: does any element of arr match the scrutinee? Value equality
   via sp_poly_eq (the splat form is used with value lists; Class/Regexp
   elements inside a splat are not dispatched through #=== here). */
static sp_bool sp_case_splat_match(sp_RbVal scrut, sp_RbVal arr) {
  sp_int n = sp_poly_length(arr);
  for (sp_int i = 0; i < n; i++)
    if (sp_poly_eq(scrut, sp_poly_arr_get(arr, i))) return TRUE;
  return FALSE;
}
/* `break *x` / `next *x`: Ruby's splat-to-array -- nil becomes [], an array
   stays itself, any other value wraps in a one-element array. */
static sp_RbVal sp_splat_to_array(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_box_poly_array(sp_PolyArray_new());
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) return v;
  { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); sp_PolyArray_push(r, v); return sp_box_poly_array(r); }
}
static sp_RbVal sp_poly_arr_get(sp_RbVal a, sp_int i) {
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  /* The poly array is the common case -- a boxed pair being destructured, an
     element read out of a container -- so answer it before the switch: the
     cls_ids are negative and scattered, which the compiler turns into a chain
     of compares rather than a jump table. */
  if (a.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *ar = (sp_PolyArray *)a.v.p;
    if (!ar) return sp_box_nil();
    if (i < 0) i += ar->len;
    if (i < 0 || i >= ar->len) return sp_box_nil();
    return ar->data[i];
  }
  /* Resolve a negative index to the tail (Ruby semantics). Most reads reach
     here already resolved by the codegen, but the chained-index paths
     (sp_poly_slot_set / _op for `a[-1][j] = v`) pass the raw negative index --
     without this they read nil and nil out the whole element (#3168). */
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: { sp_IntArray *ar=(sp_IntArray*)a.v.p; if(!ar) return sp_box_nil(); if(i<0)i+=ar->len; if(i<0||i>=ar->len) return sp_box_nil(); return sp_box_int(ar->data[ar->start+i]); }
    case SP_BUILTIN_SYM_ARRAY: { sp_IntArray *ar=(sp_IntArray*)a.v.p; if(!ar) return sp_box_nil(); if(i<0)i+=ar->len; if(i<0||i>=ar->len) return sp_box_nil(); return sp_box_sym((sp_sym)ar->data[ar->start+i]); }
    case SP_BUILTIN_FLT_ARRAY: { sp_FloatArray *ar=(sp_FloatArray*)a.v.p; if(!ar) return sp_box_nil(); if(i<0)i+=ar->len; if(i<0||i>=ar->len) return sp_box_nil(); return sp_float_is_nil(ar->data[i]) ? sp_box_nil() : sp_box_float(ar->data[i]); }  /* a gap filled by `a[n] = v` past the end is nil (#3836) */
    case SP_BUILTIN_STR_ARRAY: { sp_StrArray *ar=(sp_StrArray*)a.v.p; if(!ar) return sp_box_nil(); if(i<0)i+=ar->len; if(i<0||i>=ar->len) return sp_box_nil(); return sp_box_str(ar->data[i]); }
    case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *ar=(sp_PolyArray*)a.v.p; if(!ar) return sp_box_nil(); if(i<0)i+=ar->len; if(i<0||i>=ar->len) return sp_box_nil(); return ar->data[i]; }
    default: return sp_box_nil();
  }
}
/* Coerce a poly value that holds an array (any builtin array kind) into an
   sp_PolyArray of boxed elements. A poly-array value is returned as-is; nil or a
   non-array yields an empty array. Lets block methods (flat_map/each/...) run on
   a poly receiver whose array-ness is only known at runtime. */
static sp_PolyArray *sp_poly_to_poly_array(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)v.v.p;
  /* v is read AFTER the allocation below, and every caller hands it over as a
     bare temporary -- a freshly built typed array whose only root was the
     frame that returned it. Without this the new array's allocation collected
     the source, the pool handed the block on, and the copy read a different
     object. The early return above stays free of the root. */
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_int n = sp_poly_arr_len(v);
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(r, sp_poly_arr_get(v, i));
  return r;
}

/* The ARGUMENT of an Array set operation (`&` `|` `-`), arriving through a poly
   slot: an Array at run time becomes the poly array the set-op primitives take,
   anything else raises CRuby's TypeError. A statically poly argument had no
   route to those primitives at all -- `&` and `|` had no arm and would not
   compile, and `-` fell through to the generic poly binop, whose failure
   message read "no implicit conversion of Array into Array" on two real
   Arrays (#3475). */
static sp_PolyArray *sp_poly_set_operand(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id))
    return sp_poly_to_poly_array(v);
  sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into Array",
                                       sp_convert_src_name(v)));
  return NULL;  /* unreachable: sp_raise_cls is noreturn */
}
/* Coerce a poly value that a container-read Array method (find/reject/sort/
   each_index) was called on to a poly array, raising CRuby's NoMethodError when
   it is not an Array at run time (e.g. the method reached a nil ivar). (#2928) */
/* The receiver of an Integer-only iterator (times/upto/downto) reached through
   a boxed value. The sibling of sp_poly_arr_recv above: a value that is not an
   Integer answers NoMethodError naming the method, rather than being coerced
   to some number and running the loop anyway. */
static sp_int sp_poly_int_recv(sp_RbVal v, const char *m) {
  if (v.tag == SP_TAG_INT) return v.v.i;
  sp_raise_nomethod(sp_nomethod_msg(m, v));
  return 0;  /* unreachable: sp_raise_nomethod does not return */
}
/* A mutated element that the typed original has no representation for: a
   String written into an Array of Integers through a boxed receiver. CRuby's
   Array holds anything; the typed original cannot, and it cannot be re-laid
   from here (the container slot that holds it is not in reach), so the
   write-back refuses rather than coerce the value into something it was not
   -- the earlier map! write-back answered 0 for such a String. */
SP_NORETURN SP_COLD static void sp_raise_writeback_kind(sp_RbVal v, const char *into) {
  sp_raise_cls("TypeError", sp_sprintf("can't store %s in an Array of %s through a boxed receiver",
                                       sp_poly_class_name(v), into));
}
/* A mutator's write-back through a boxed receiver: sp_poly_arr_recv
   normalizes a TYPED array to a poly COPY, so in-place rewrites never reached
   the original (#3234). Replace the original's contents with the mutated
   elements, in its own representation; a POLY_ARRAY receiver shares storage
   and needs nothing. The length follows the work array -- reject!, uniq! and
   slice! shrink it, fill(n) can grow it. Frozenness was refused at the
   coercion, before the mutator ran. */
static void sp_poly_arr_writeback(sp_RbVal orig, sp_PolyArray *work) {
  if (orig.tag != SP_TAG_OBJ || !work) return;
  switch (orig.cls_id) {
    case SP_BUILTIN_INT_ARRAY: {
      sp_IntArray *a = (sp_IntArray *)orig.v.p;
      for (sp_int i = 0; i < work->len; i++)
        if (work->data[i].tag != SP_TAG_INT) sp_raise_writeback_kind(work->data[i], "Integer");
      a->start = 0; a->len = 0;
      for (sp_int i = 0; i < work->len; i++) sp_IntArray_push(a, work->data[i].v.i);
      return;
    }
    case SP_BUILTIN_SYM_ARRAY: {
      sp_IntArray *a = (sp_IntArray *)orig.v.p;
      for (sp_int i = 0; i < work->len; i++)
        if (work->data[i].tag != SP_TAG_SYM) sp_raise_writeback_kind(work->data[i], "Symbol");
      a->start = 0; a->len = 0;
      for (sp_int i = 0; i < work->len; i++) sp_IntArray_push(a, work->data[i].v.i);
      return;
    }
    case SP_BUILTIN_FLT_ARRAY: {
      sp_FloatArray *a = (sp_FloatArray *)orig.v.p;
      for (sp_int i = 0; i < work->len; i++)
        if (work->data[i].tag != SP_TAG_FLT && work->data[i].tag != SP_TAG_INT)
          sp_raise_writeback_kind(work->data[i], "Float");
      a->len = 0;
      for (sp_int i = 0; i < work->len; i++) sp_FloatArray_push(a, sp_poly_to_f(work->data[i]));
      return;
    }
    case SP_BUILTIN_STR_ARRAY: {
      sp_StrArray *a = (sp_StrArray *)orig.v.p;
      for (sp_int i = 0; i < work->len; i++)
        if (work->data[i].tag != SP_TAG_STR && !sp_poly_is_strbuf(work->data[i]))
          sp_raise_writeback_kind(work->data[i], "String");
      sp_gc_wb((void *)a);
      a->len = 0;
      for (sp_int i = 0; i < work->len; i++) sp_StrArray_push(a, sp_poly_to_s(work->data[i]));
      return;
    }
    default: return;   /* POLY_ARRAY: same storage; others: nothing to do */
  }
}
static sp_PolyArray *sp_enum_items_from(sp_RbVal v);   /* fwd: hash -> [key, value] pairs */
static sp_PolyArray *sp_poly_arr_recv(sp_RbVal v, const char *m) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) return sp_poly_to_poly_array(v);
  /* A boxed Hash enumerates as its [key, value] pairs, which is what every
     Enumerable name reaching here wants (#3449). The few whose Hash result is
     itself a Hash rebuild one from the pairs at their own call site. */
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) return sp_enum_items_from(v);
  /* a user object with #to_a (a container-read Set): iterate its elements
     through the generated hook (#3234) */
  if (v.tag == SP_TAG_OBJ && sp_obj_to_a_fn) {
    sp_RbVal a = sp_obj_to_a_fn(v);
    if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id)) return sp_poly_to_poly_array(a);
  }
  sp_raise_nomethod(sp_nomethod_msg(m, v));
  return NULL;  /* unreachable: sp_raise_nomethod does not return */
}

/* Is the array a boxed value holds frozen? The flag rides in each array
   struct, not in the GC header. */
static sp_bool sp_poly_array_frozen(sp_RbVal v) {
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:
    case SP_BUILTIN_SYM_ARRAY:  return ((sp_IntArray *)v.v.p)->frozen != 0;
    case SP_BUILTIN_FLT_ARRAY:  return ((sp_FloatArray *)v.v.p)->frozen != 0;
    case SP_BUILTIN_STR_ARRAY:  return ((sp_StrArray *)v.v.p)->frozen != 0;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)v.v.p)->frozen != 0;
  }
  return FALSE;
}
/* The receiver of an Array-only method (sort!, fill, transpose, ...) reached
   through a boxed value: an Array at run time, as a poly array, and anything
   else -- a Hash, whose pairs sp_poly_arr_recv would have offered, a nil, a
   String -- CRuby's NoMethodError naming the method. A mutator (`mut`) asks
   about frozenness here, before any work: the typed emitter it re-enters
   works on the poly copy, so only the write-back would notice, after a
   block had already run over every element. */
static sp_PolyArray *sp_poly_array_recv(sp_RbVal v, const char *m, int mut) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    if (mut && sp_poly_array_frozen(v)) sp_raise_frozen_array_at(v.v.p, v.cls_id);
    return sp_poly_to_poly_array(v);
  }
  sp_raise_nomethod(sp_nomethod_msg(m, v));
  return NULL;  /* unreachable: sp_raise_nomethod does not return */
}
/* The receiver of an Enumerable method reached through a boxed value: its
   elements (a hash's pairs, a range's members) as a poly array, and for a
   value that is no collection at all -- an Integer, a String, nil --
   CRuby's NoMethodError, where the bare materialization answered an empty
   list and the call went on as if over nothing. */
static sp_PolyArray *sp_poly_enum_recv(sp_RbVal v, const char *m) {
  if (v.tag != SP_TAG_OBJ) sp_raise_nomethod(sp_nomethod_msg(m, v));
  return sp_enum_items_from(v);
}

/* inject/reduce with a symbol op over a poly iterable (a container-read row
   or a to_a-bearing user object like Set, #3234): fold through the numeric-
   tower poly ops. Only the arithmetic four dispatch; anything else raises
   like an unresolved method. */
static sp_RbVal sp_poly_inject_sym(sp_RbVal v, sp_sym op) {
  sp_PolyArray *a = sp_poly_arr_recv(v, "inject");
  if (!a || a->len == 0) return sp_box_nil();
  const char *nm = sp_sym_name_fn ? sp_sym_name_fn(op) : "";
  sp_RbVal acc = sp_PolyArray_get(a, 0);
  SP_GC_ROOT_RBVAL(acc);
  for (sp_int i = 1; i < a->len; i++) {
    sp_RbVal e = sp_PolyArray_get(a, i);
    if (nm[0] == '+' && !nm[1]) acc = sp_poly_add(acc, e);
    else if (nm[0] == '-' && !nm[1]) acc = sp_poly_sub(acc, e);
    else if (nm[0] == '*' && !nm[1]) acc = sp_poly_mul(acc, e);
    else if (nm[0] == '/' && !nm[1]) acc = sp_poly_div(acc, e);
    else sp_raise_nomethod(sp_nomethod_msg(nm, acc));
  }
  return acc;
}
/* The argument vector for `format`/`String#%` from a single poly right-hand
   side: an Array value is spread across the directives, any other value formats
   as a one-element list. Used when the `%` RHS is statically poly so the array
   vs scalar distinction is only known at runtime. */
static sp_PolyArray *sp_format_args(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) return sp_poly_to_poly_array(v);
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  sp_PolyArray_push(a, v);
  return a;
}
/* Array#<=> across any pair of builtin array kinds: lexicographic element-wise
   compare via sp_poly_cmp, breaking ties on length. `*comparable` is cleared
   when an element pair is not mutually comparable (CRuby yields nil there). */
static sp_int sp_poly_arr_cmp(sp_RbVal a, sp_RbVal b, sp_bool *comparable) {
  /* Same object compares equal in O(1); this also terminates self-referential
     arrays (a contains a), which would otherwise recurse without bound. */
  if (a.v.p == b.v.p) { *comparable = TRUE; return 0; }
  sp_int la = sp_poly_arr_len(a), lb = sp_poly_arr_len(b);
  sp_int n = la < lb ? la : lb;
  /* Two DISTINCT arrays that each contain themselves meet the same pair again
     one level down. CRuby counts a repeated pair as equal and lets the lengths
     decide -- `a <=> [a, 1]` is -1 -- so skipping the element loop is exactly
     that answer, not a shortcut around it. */
  if (!sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) {
    int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
    for (sp_int i = 0; i < n; i++) {
      sp_bool ec; sp_int r = sp_poly_cmp(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i), &ec);
      if (!ec) { sp_poly_recur_pop(mark); *comparable = FALSE; return 0; }
      if (r != 0) { sp_poly_recur_pop(mark); *comparable = TRUE; return r < 0 ? -1 : 1; }
    }
    sp_poly_recur_pop(mark);
  }
  *comparable = TRUE;
  return (la > lb) - (la < lb);
}
/* Kernel#Array(x): nil -> []; an array -> its elements; anything else -> [x].
   Array-typed arguments are passed through directly in codegen, so this runs
   for nil/scalars and for a poly value that may hold an array at run time. */
static sp_PolyArray *sp_kernel_array(sp_RbVal x) {
  if (x.tag == SP_TAG_NIL) return sp_PolyArray_new();
  if (x.tag == SP_TAG_OBJ) {
    /* An existing poly array is returned as-is, preserving object identity as
       CRuby's Array(arr) does. A typed array cannot be returned directly (the
       result is a poly array), so its elements are copied. */
    if (x.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)x.v.p;
    if (x.cls_id == SP_BUILTIN_INT_ARRAY || x.cls_id == SP_BUILTIN_STR_ARRAY ||
        x.cls_id == SP_BUILTIN_FLT_ARRAY) {
      sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
      sp_int n = sp_poly_arr_len(x);
      for (sp_int i = 0; i < n; i++) sp_PolyArray_push(r, sp_poly_arr_get(x, i));
      return r;
    }
    /* Array(hash) is the pair list and Array(range) enumerates -- the answers
       the statically-typed arm gives; through an untyped parameter the hash
       was wrapped whole instead (#4187). */
    if (sp_poly_is_hash_kind(x.cls_id) ||
        x.cls_id == SP_BUILTIN_RANGE || x.cls_id == SP_BUILTIN_STR_RANGE)
      return sp_enum_items_from(x);
  }
  /* A USER object is asked for #to_ary, then #to_a, before being wrapped --
     the same order the statically-typed arm uses (#3721). The static arm only
     runs when the call site can see the class; an argument arriving through
     an untyped parameter reached here and was silently wrapped instead, so
     one object answered two different arrays depending on the route (#4187).
     A poly-array answer is returned as itself (identity, as CRuby returns
     to_ary's array); a typed one materializes, as the array cases above do. */
  if (x.tag == SP_TAG_OBJ && x.cls_id >= 0) {
    sp_RbVal a = sp_box_nil();
    if (sp_obj_to_ary_fn) a = sp_obj_to_ary_fn(x);
    if (!(a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id)) && sp_obj_to_a_fn)
      a = sp_obj_to_a_fn(x);
    if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id))
      return sp_poly_to_poly_array(a);
  }
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_PolyArray_push(r, x);
  return r;
}
/* Issues #770, #789: NULL + bounds guard. Out-of-range set no-ops. */
static void sp_PolyArray_set(sp_PolyArray *a, sp_int i, sp_RbVal v) { if (!a) return; sp_gc_wb((void*)a); if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return; } sp_int orig=i; if (i < 0) i += a->len; if (i < 0) sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len)); if (i > a->len) { /* a gap fills with nil, as in every typed array (#3615) */ sp_RbVal _nil = sp_box_nil(); while (a->len < i) sp_PolyArray_push(a, _nil); } if (i == a->len) { sp_PolyArray_push(a, v); return; } a->data[i] = v; }
static sp_PolyArray *sp_PolyArray_slice(sp_PolyArray *a, sp_int start, sp_int len) { SP_GC_ROOT(a); if (start < 0) start += a->len; if (start < 0) start = 0; sp_PolyArray *b = sp_PolyArray_new(); if (start >= a->len || len <= 0) return b; if (len > a->len - start) len = a->len - start; for (sp_int i = 0; i < len; i++) sp_PolyArray_push(b, a->data[start + i]); return b; }
static sp_PolyArray *sp_PolyArray_slice_range(sp_PolyArray *a, sp_int start, sp_int end_, sp_int excl) { if (end_ < 0) end_ += a->len; if (start < 0) start += a->len; sp_int n = end_ - start + (excl ? 0 : 1); if (n < 0 || start < 0) n = 0; return sp_PolyArray_slice(a, start, n); }
/* 2-arg slice on a poly receiver: dispatch to the typed slice functions. */
static sp_RbVal sp_poly_slice(sp_RbVal a, sp_int start, sp_int len) {
  if (a.tag == SP_TAG_STR) return sp_box_nullable_str(sp_str_sub_range(a.v.s ? a.v.s : "", start, len));
  /* A shared-string handle is a String: slicing is non-mutating, so it answers
     as its live value rather than falling through to the array kinds and out
     the nil default, which is what `s = +""; s << "abc"; s[0, 2]` did (#4279). */
  if (sp_poly_is_strbuf(a)) return sp_poly_slice(sp_poly_strbuf_deref(a), start, len);
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  /* arr[start, negative] is nil in CRuby (the slice helpers would return []) */
  if (len < 0 && sp_poly_is_array_kind(a.cls_id)) return sp_box_nil();
  /* bm[a, b]: a boxed bound Method called with two int arguments (optcarrot's
     store dispatch table: `@store[addr][addr, value]`). */
  if (a.cls_id == SP_BUILTIN_METHOD) {
    sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
    return sp_box_int(((sp_int (*)(void *, sp_int, sp_int))(uintptr_t)m->fn)((void *)m->self, start, len));
  }
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  return sp_box_int_array(sp_IntArray_slice((sp_IntArray*)a.v.p, start, len));
    case SP_BUILTIN_FLT_ARRAY:  return sp_box_float_array(sp_FloatArray_slice((sp_FloatArray*)a.v.p, start, len));
    case SP_BUILTIN_STR_ARRAY:  return sp_box_str_array(sp_StrArray_slice((sp_StrArray*)a.v.p, start, len));
    case SP_BUILTIN_POLY_ARRAY: return sp_box_poly_array(sp_PolyArray_slice((sp_PolyArray*)a.v.p, start, len));
    default: return sp_box_nil();
  }
}
/* True when a boxed value is one of the builtin array kinds. */
static int sp_rbval_is_array(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ &&
    (v.cls_id == SP_BUILTIN_INT_ARRAY || v.cls_id == SP_BUILTIN_FLT_ARRAY ||
     v.cls_id == SP_BUILTIN_STR_ARRAY || v.cls_id == SP_BUILTIN_POLY_ARRAY);
}
/* 3-arg []= on a poly receiver whose runtime object is a builtin array. Matches
   CRuby: a POLY_ARRAY splices directly (sp_PolyArray_splice already inserts a
   nil/scalar src as one element, splats an array src, and nil-fills a gap past
   the end). A typed array stays typed only when the result provably remains
   homogeneous -- an empty ([]) src, a same-kind array, or a matching scalar, AND
   no nil-fill (start <= len). Otherwise the array is promoted to a poly array
   (boxing its elements) and spliced there. Returns the possibly-new boxed array
   so the caller stores it back into the receiver's slot. */
static sp_RbVal sp_poly_splice(sp_RbVal recv, sp_int start, sp_int len, sp_RbVal src) {
  /* `s[start, len] = v` through a poly receiver: spinel strings splice into a
     fresh buffer, so a plain string box answers the new value for the caller to
     store back, while a shared handle absorbs it in place -- which is the only
     form an element receiver can use, and the write was silently dropped
     before (#3940). */
  if (recv.tag == SP_TAG_STR || sp_poly_is_strbuf(recv)) {
    const char *cur = (recv.tag == SP_TAG_STR) ? (recv.v.s ? recv.v.s : sp_str_empty)
                                               : sp_String_cstr((sp_String *)recv.v.p);
    const char *rep = (src.tag == SP_TAG_STR) ? (src.v.s ? src.v.s : sp_str_empty)
                                              : sp_poly_to_s(src);
    SP_GC_ROOT(cur); SP_GC_ROOT(rep);
    SP_GC_ROOT_RBVAL(recv);
    const char *out = sp_str_splice_at(cur, start, len, rep, 0);
    if (recv.tag == SP_TAG_STR) return sp_box_str(out);
    sp_String_set_bin((sp_String *)recv.v.p, out);
    return recv;
  }
  if (recv.tag != SP_TAG_OBJ) return recv;
  sp_int alen = sp_poly_arr_len(recv);
  sp_int s = start < 0 ? start + alen : start;
  /* Validate frozen/length/index UP FRONT -- before any delegate roots the
     array -- so a raise never longjmps with a GC root live (an inline rescue
     does not restore sp_gc_nroots, so such a root would dangle). The delegates
     re-check the same conditions but, being pre-satisfied, never raise. The
     order matches CRuby: modify-check first, then negative length, then the
     too-small index. */
  if (sp_typed_arr_frozen(recv)) sp_raise_frozen_array_v(recv);
  if (len < 0) sp_raise_cls("IndexError", sp_sprintf("negative length (%lld)", (long long)len));
  if (s < 0) sp_raise_cls("IndexError",
                          sp_sprintf("index %lld too small for array; minimum: %lld", (long long)start, (long long)-alen));
  if (recv.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray_splice((sp_PolyArray *)recv.v.p, start, len, src);
    return recv;
  }
  int nofill = s <= alen;   /* a start past the end needs a nil-fill -> promote */
  int is_empty_arr = sp_rbval_is_array(src) && sp_poly_arr_len(src) == 0;
  switch (recv.cls_id) {
    case SP_BUILTIN_INT_ARRAY: {
      sp_IntArray *a = (sp_IntArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_IntArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_INT_ARRAY) {
        sp_IntArray *sa = (sp_IntArray *)src.v.p;
        sp_IntArray_splice(a, start, len, sa->data + sa->start, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_INT) { sp_int v = src.v.i; sp_IntArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    case SP_BUILTIN_FLT_ARRAY: {
      sp_FloatArray *a = (sp_FloatArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_FloatArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_FLT_ARRAY) {
        sp_FloatArray *sa = (sp_FloatArray *)src.v.p;
        sp_FloatArray_splice(a, start, len, sa->data, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_FLT) { sp_float v = src.v.f; sp_FloatArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    case SP_BUILTIN_STR_ARRAY: {
      sp_StrArray *a = (sp_StrArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_StrArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_STR_ARRAY) {
        sp_StrArray *sa = (sp_StrArray *)src.v.p;
        sp_StrArray_splice(a, start, len, sa->data, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_STR) { const char *v = src.v.s; sp_StrArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    default: return recv;
  }
  /* promote to poly and splice there (handles nil / heterogeneous / nil-fill).
     Index/length/frozen were validated up front, so nothing below raises; the
     GC roots are pushed only around the actual allocation and pop normally. */
  SP_GC_ROOT_RBVAL(src);
  /* recv is read element-by-element inside the conversion's push loop, each of
     which can collect; a temporary receiver held by no rooted container would
     otherwise dangle mid-loop. */
  SP_GC_ROOT_RBVAL(recv);
  sp_PolyArray *p = sp_poly_to_poly_array(recv);
  SP_GC_ROOT(p);
  sp_PolyArray_splice(p, start, len, src);
  return sp_box_poly_array(p);
}
/* `arr[range] = src` on a poly receiver: resolve beginless (INTPTR_MIN -> 0) and
   endless (INTPTR_MAX -> length) endpoints and negative endpoints against the
   runtime length, then splice. A begin index below -length raises RangeError
   (CRuby uses RangeError here, not the (start,len) form's IndexError). */
static sp_RbVal sp_poly_splice_range(sp_RbVal recv, sp_Range r, sp_RbVal src) {
  /* a string receiver measures the range against its CHARACTER length, which
     sp_poly_arr_len answers 0 for (#3940) */
  if (recv.tag == SP_TAG_STR || sp_poly_is_strbuf(recv)) {
    const char *cur = (recv.tag == SP_TAG_STR) ? (recv.v.s ? recv.v.s : sp_str_empty)
                                               : sp_String_cstr((sp_String *)recv.v.p);
    sp_int slen = (sp_int)sp_str_length(cur);
    sp_int sfirst = r.first;
    if (sfirst == INTPTR_MIN) sfirst = 0;
    else if (sfirst < 0) sfirst += slen;
    sp_int slen2;
    if (r.last == INTPTR_MAX) { slen2 = slen - sfirst; if (slen2 < 0) slen2 = 0; }
    else {
      sp_int last = r.last < 0 ? r.last + slen : r.last;
      slen2 = last - sfirst + (r.excl ? 0 : 1);
      if (slen2 < 0) slen2 = 0;
    }
    return sp_poly_splice(recv, sfirst, slen2, src);
  }
  /* frozen precedes range validation (CRuby's modify-check runs first) */
  if (recv.tag == SP_TAG_OBJ && sp_typed_arr_frozen(recv)) sp_raise_frozen_array_v(recv);
  sp_int alen = sp_poly_arr_len(recv);
  sp_int first = r.first;
  if (first == INTPTR_MIN) first = 0;      /* beginless */
  else if (first < 0) {
    if (first < -alen) sp_raise_cls("RangeError", sp_sprintf("%s out of range", sp_range_str(r)));
    first += alen;
  }
  sp_int len;
  if (r.last == INTPTR_MAX) { len = alen - first; if (len < 0) len = 0; }  /* endless */
  else {
    sp_int last = r.last < 0 ? r.last + alen : r.last;
    len = last - first + (r.excl ? 0 : 1);
    if (len < 0) len = 0;
  }
  return sp_poly_splice(recv, first, len, src);
}
/* arr[Range] / arr.slice(Range) on a poly receiver. A Range index used to fall
   through the poly `[]` as index 0, so every range answered the first element
   -- the array read out of a nested Array or Hash is exactly the shape that
   lands here (#3464). The string receiver was already handled (#3175). */
static sp_RbVal sp_poly_arr_range(sp_RbVal recv, sp_Range r) {
  sp_int alen = sp_poly_arr_len(recv);
  sp_int first = r.first;
  if (first == INTPTR_MIN) first = 0;          /* beginless */
  else if (first < 0) first += alen;
  if (first < 0 || first > alen) return sp_box_nil();   /* CRuby: out of range */
  sp_int len;
  if (r.last == INTPTR_MAX) len = alen - first;         /* endless */
  else {
    sp_int last = r.last < 0 ? r.last + alen : r.last;
    len = last - first + (r.excl ? 0 : 1);
  }
  if (len < 0) len = 0;
  if (len > alen - first) len = alen - first;
  return sp_poly_slice(recv, first, len);
}
/* Array#replace(other): replace recv's contents with other's, returning recv.
   recv keeps the same boxed pointer (the underlying array is mutated in place),
   so a nullable-array slot typed poly stays valid. A nil/non-array recv is a
   no-op (the call sites that reach a nil poly are dead-guarded in Ruby). */
/* sp_poly_replace: moved to lib/sp_cold.c */
sp_RbVal sp_poly_replace(sp_RbVal recv, sp_RbVal src);
static sp_PolyArray *sp_PolyArray_slice_bang(sp_PolyArray *a, sp_int from, sp_int n) {sp_gc_wb((void*)a); 
  if (!a) return sp_PolyArray_new();
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return sp_PolyArray_new(); }
  /* a start past the end is nil, not an empty slice (#3607) */
  if (from < 0) from += a->len;
  if (from < 0 || from > a->len) return NULL;
  if (n < 0) return NULL;   /* slice!(start, -1) is nil, not an empty slice */
  if (from + n > a->len) n = a->len - from;
  sp_PolyArray *r = sp_PolyArray_new();
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(r, a->data[from + i]);
  for (sp_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
/* combination/permutation over boxed elements (any array kind, via
   sp_poly_to_poly_array). Each emitted row is a boxed PolyArray. */
/* sp_poly_combination_recur: moved to lib/sp_cold.c */
void sp_poly_combination_recur(sp_PolyArray *src, sp_int start, sp_int k, sp_PolyArray *acc, sp_PolyArray *out);
static sp_PolyArray *sp_PolyArray_combination(sp_PolyArray *a, sp_int k) {
  SP_GC_ROOT(a);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (!a || k < 0 || k > a->len) return out;
  sp_PolyArray *acc = sp_PolyArray_new(); SP_GC_ROOT(acc);
  sp_poly_combination_recur(a, 0, k, acc, out);
  return out;
}
/* sp_poly_repeated_combination_recur: moved to lib/sp_cold.c */
void sp_poly_repeated_combination_recur(sp_PolyArray *src, sp_int start, sp_int k, sp_PolyArray *acc, sp_PolyArray *out);
static sp_PolyArray *sp_PolyArray_repeated_combination(sp_PolyArray *a, sp_int k) {
  SP_GC_ROOT(a);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (!a || k < 0) return out;
  sp_PolyArray *acc = sp_PolyArray_new(); SP_GC_ROOT(acc);
  sp_poly_repeated_combination_recur(a, 0, k, acc, out);
  return out;
}
/* sp_poly_permutation_recur: moved to lib/sp_cold.c */
void sp_poly_permutation_recur(sp_PolyArray *src, sp_int k, sp_IntArray *used, sp_PolyArray *acc, sp_PolyArray *out);
/* sp_poly_repeated_permutation_recur: moved to lib/sp_cold.c */
void sp_poly_repeated_permutation_recur(sp_PolyArray *src, sp_int k, sp_PolyArray *acc, sp_PolyArray *out);
static sp_PolyArray *sp_PolyArray_repeated_permutation(sp_PolyArray *a, sp_int k) {
  SP_GC_ROOT(a);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (!a || k < 0) return out;
  sp_PolyArray *acc = sp_PolyArray_new(); SP_GC_ROOT(acc);
  sp_poly_repeated_permutation_recur(a, k, acc, out);
  return out;
}
static sp_PolyArray *sp_PolyArray_permutation(sp_PolyArray *a, sp_int k) {
  SP_GC_ROOT(a);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (!a || k < 0 || k > a->len) return out;
  sp_IntArray *used = sp_IntArray_new(); SP_GC_ROOT(used);
  for (sp_int i = 0; i < a->len; i++) sp_IntArray_push(used, 0);
  sp_PolyArray *acc = sp_PolyArray_new(); SP_GC_ROOT(acc);
  sp_poly_permutation_recur(a, k, used, acc, out);
  return out;
}
static sp_PolyArray *sp_PolyArray_dup(sp_PolyArray *a) { SP_GC_ROOT(a); sp_PolyArray *b = sp_PolyArray_new(); for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(b, a->data[i]); return b; }
static sp_PolyArray *sp_PolyArray_replace(sp_PolyArray *dst, sp_PolyArray *src) { if (!dst || !src) return dst; if (dst == src) return dst; if (dst->frozen) { sp_raise_frozen_array_at(dst, SP_BUILTIN_POLY_ARRAY); return dst; } dst->len = 0; for (sp_int i = 0; i < src->len; i++) sp_PolyArray_push(dst, src->data[i]); return dst; }
/* Array#replace where the SOURCE is of another kind -- `[1, 2].replace(["x"])`
   is ordinary Ruby, and the receiver becomes a copy of whatever the source
   holds. The typed arms can only serve a source of their own kind, so a poly
   receiver (which is what the widening leaves behind) reads the source through
   the boxed accessors, exactly as concat_into does. Frozen is checked before
   the truncation: dst->len = 0 would otherwise clear the array and only then
   let the first push raise. */
static sp_PolyArray *sp_PolyArray_replace_from(sp_PolyArray *dst, sp_RbVal src) {
  if (!dst) return dst;
  SP_GC_ROOT(dst);
  SP_GC_ROOT_RBVAL(src);
  if (src.tag == SP_TAG_OBJ && src.v.p == (void *)dst) return dst;
  if (dst->frozen) { sp_raise_frozen_array_at(dst, SP_BUILTIN_POLY_ARRAY); return dst; }
  { sp_int n = sp_poly_arr_len(src);
    dst->len = 0;
    for (sp_int i = 0; i < n; i++) sp_PolyArray_push(dst, sp_poly_arr_get(src, i)); }
  return dst;
}
/* Array#+ : a fresh (unfrozen) array of a's then b's elements. */
/* Array#concat: in-place append of another (any-kind) array's elements */
static sp_PolyArray *sp_PolyArray_concat_into(sp_PolyArray *a, sp_RbVal other) {
  SP_GC_ROOT(a);
  sp_int n = sp_poly_arr_len(other);
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(a, sp_poly_arr_get(other, i));
  return a;
}
static sp_PolyArray *sp_PolyArray_concat(sp_PolyArray *a, sp_PolyArray *b) { SP_GC_ROOT(a); SP_GC_ROOT(b); sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, a->data[i]); if (b) for (sp_int i = 0; i < b->len; i++) sp_PolyArray_push(r, b->data[i]); return r; }
/* Array#concat: append b's elements onto a IN PLACE, returning a (unlike the
   fresh-array sp_PolyArray_concat above). Snapshot b's length first so `a` and
   `b` aliasing the same array still terminates. */
static sp_PolyArray *sp_PolyArray_append_all(sp_PolyArray *a, sp_PolyArray *b) { if (!a || !b) return a; SP_GC_ROOT(a); SP_GC_ROOT(b); sp_int bn = b->len; if (a == b) { /* self-concat: a push may realloc a->data, dangling b->data (same buffer) -- snapshot b first. */ sp_PolyArray *bc = sp_PolyArray_new(); SP_GC_ROOT(bc); for (sp_int i = 0; i < bn; i++) sp_PolyArray_push(bc, b->data[i]); b = bc; } for (sp_int i = 0; i < bn; i++) sp_PolyArray_push(a, b->data[i]); return a; }
/* Array#rindex(obj): index of the LAST element == obj, or -1 (arm maps to nil). */
static sp_int sp_PolyArray_rindex(sp_PolyArray *a, sp_RbVal v) { if (!a) return -1; for (sp_int i = a->len - 1; i >= 0; i--) if (sp_poly_eq(a->data[i], v)) return i; return -1; }
/* Array#index / #rindex with a VALUE argument, over any array storage kind:
   the first (or last) position comparing equal, or SP_INT_NIL for none. Used
   by the poly-receiver arm, where the element kind is only known at run time. */
/* include? over a user Enumerable read out of a container: its elements,
   compared the way the array arms compare (#3761). Answers -1 when the
   receiver is not one, so a caller can fall through. */
static int sp_poly_user_include(sp_RbVal recv, sp_RbVal x);
static sp_int sp_poly_arr_index_val(sp_RbVal a, sp_RbVal v, int rev) {
  sp_int n = sp_poly_arr_len(a);
  if (rev) { for (sp_int i = n - 1; i >= 0; i--) if (sp_poly_eq(sp_poly_arr_get(a, i), v)) return i; }
  else     { for (sp_int i = 0; i < n; i++)      if (sp_poly_eq(sp_poly_arr_get(a, i), v)) return i; }
  return SP_INT_NIL;
}
static sp_bool sp_PolyArray_include_val(sp_PolyArray *a, sp_RbVal v) { if (!a) return FALSE; for (sp_int i = 0; i < a->len; i++) if (sp_poly_eq(a->data[i], v)) return TRUE; return FALSE; }
static sp_PolyArray *sp_PolyArray_intersect(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (!a || !b) return r; for (sp_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (sp_PolyArray_include_val(b, v) && !sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } return r; }
/* intersect? predicate: early-exit, no allocation (matches CRuby's non-building Array#intersect?). */
static sp_bool sp_PolyArray_intersect_p(sp_PolyArray *a, sp_PolyArray *b) { if (!a || !b) return 0; for (sp_int i = 0; i < a->len; i++) if (sp_PolyArray_include_val(b, a->data[i])) return 1; return 0; }
static sp_PolyArray *sp_PolyArray_union(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (!sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } if (b) for (sp_int i = 0; i < b->len; i++) { sp_RbVal v = b->data[i]; if (!sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } return r; }
static sp_PolyArray *sp_PolyArray_difference(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (!a) return r; for (sp_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (!sp_PolyArray_include_val(b, v)) sp_PolyArray_push(r, v); } return r; }
/* Array#compact for poly_array: keep elements whose tag is not SP_TAG_NIL. */
static sp_PolyArray *sp_PolyArray_compact(sp_PolyArray *a) { SP_GC_ROOT(a); sp_PolyArray *b = sp_PolyArray_new(); SP_GC_ROOT(b); if (!a) return b; for (sp_int i = 0; i < a->len; i++) { if (a->data[i].tag != SP_TAG_NIL) sp_PolyArray_push(b, a->data[i]); } return b; }
static sp_PolyArray *sp_PolyArray_compact_bang(sp_PolyArray *a) {sp_gc_wb((void*)a);  if (!a) return a; sp_int w = 0; for (sp_int i = 0; i < a->len; i++) { if (a->data[i].tag != SP_TAG_NIL) a->data[w++] = a->data[i]; } a->len = w; return a; }
/* An unlimited flatten (or a join) that meets an array it is already inside
   has no answer to give: the flat run would be infinite, so CRuby raises. The
   walk's own frames go first (sp_poly_recur_drop_kind). Off the hot path by
   construction -- they never return. */
SP_NORETURN SP_COLD static __attribute__((noinline)) void sp_poly_recur_raise(int kind, const char *msg) {
  sp_poly_recur_drop_kind(kind);
  sp_raise_cls("ArgumentError", msg);
}
/* Array#flatten -- walk into nested array values recursively. Each
   array-tagged element (IntArray / StrArray / SymArray / FloatArray /
   PolyArray) is expanded inline; scalars are appended as-is. Issue
   #739. */
static void sp_PolyArray_flatten_into(sp_PolyArray *dst, sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (sp_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (sp_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_SYM_ARRAY) { sp_IntArray *ya = (sp_IntArray *)v.v.p; for (sp_int i = 0; i < ya->len; i++) sp_PolyArray_push(dst, sp_box_sym((sp_sym)ya->data[ya->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_FLT_ARRAY) { sp_FloatArray *fa = (sp_FloatArray *)v.v.p; for (sp_int i = 0; i < fa->len; i++) sp_PolyArray_push(dst, sp_box_float(fa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *pa = (sp_PolyArray *)v.v.p;
    if (sp_poly_recur_seen(SP_POLY_RECUR_FLATTEN, pa, NULL))
      sp_poly_recur_raise(SP_POLY_RECUR_FLATTEN, "tried to flatten recursive array");
    int mark = sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, pa, NULL);
    for (sp_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into(dst, pa->data[i]);
    sp_poly_recur_pop(mark);
    return;
  }
  /* Other array variants fall through as opaque elements; rare for
     deep-flatten use cases. */
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten_bang(sp_PolyArray *a);  /* below */
static sp_PolyArray *sp_PolyArray_flatten(sp_PolyArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_new(); SP_GC_ROOT(b);
  if (!a) return b;
  /* the receiver is on the path as much as any nested array is: `a << a;
     a.flatten` has to raise, and only this frame can catch that */
  int mark = sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, a, NULL);
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into(b, a->data[i]);
  sp_poly_recur_pop(mark);
  return b;
}
/* depth-limited flatten: depth counts how many nesting levels unwrap
   (CRuby's flatten(1)); a negative depth flattens fully. */
static void sp_PolyArray_flatten_into_d(sp_PolyArray *out, sp_RbVal v, sp_int depth);
static void sp_PolyArray_flatten_into_d(sp_PolyArray *out, sp_RbVal v, sp_int depth) {
  if (depth != 0 && v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    /* Only the unlimited walk (a negative depth) can be trapped by a cycle. A
       counted one ends by counting down, and CRuby prints [[[...]]] for it
       rather than raising, so it carries no path. */
    if (depth < 0 && sp_poly_recur_seen(SP_POLY_RECUR_FLATTEN, v.v.p, NULL))
      sp_poly_recur_raise(SP_POLY_RECUR_FLATTEN, "tried to flatten recursive array");
    sp_PolyArray *in = sp_poly_to_poly_array(v); SP_GC_ROOT(in);
    int mark = depth < 0 ? sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, v.v.p, NULL)
                         : sp_poly_recur_save();
    for (sp_int i = 0; i < in->len; i++)
      sp_PolyArray_flatten_into_d(out, in->data[i], depth - 1);
    sp_poly_recur_pop(mark);
    return;
  }
  sp_PolyArray_push(out, v);
}
static sp_PolyArray *sp_PolyArray_flatten_depth(sp_PolyArray *a, sp_int d) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_new(); SP_GC_ROOT(b);
  if (!a) return b;
  int mark = d < 0 ? sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, a, NULL) : sp_poly_recur_save();
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into_d(b, a->data[i], d);
  sp_poly_recur_pop(mark);
  return b;
}
/* Array#flatten!: replace the receiver's contents with the flattened run
   (aliases observe the mutation). */
static sp_PolyArray *sp_PolyArray_flatten_bang(sp_PolyArray *a) {sp_gc_wb((void*)a); 
  if (!a) return NULL;
  if (a->frozen) sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY);
  SP_GC_ROOT(a);
  sp_PolyArray *f = sp_PolyArray_flatten(a); SP_GC_ROOT(f);
  a->len = 0;
  for (sp_int i = 0; i < f->len; i++) sp_PolyArray_push(a, f->data[i]);
  return a;
}

/* Box-into-poly converters used by the printf-with-array codegen
   (`"%fmt" % typed_array`). The format helper expects sp_RbVal
   slots so it can dispatch per-element. */

/* String#% with a poly_array argument. Walks the format and for
   each spec ("%s", "%d", "%f", "%x", "%o", etc.) pulls the next
   array element. Width / flag chars between `%` and the conv
   letter (`-+0 #`, digits, `.`) are copied verbatim so printf
   does the substitution work. */
/* Ruby-style %b / %B binary conversion (C printf has no binary conversion).
   Handles the -, +, space, #, 0 flags, width, and precision. A negative value
   uses Ruby's two's-complement ".." notation: the leading run of 1 bits is
   collapsed to a single 1 (e.g. -5 -> "..1011"), and precision/`#`/width apply
   around that body. Writes into out (osz bytes) and returns the length. */
/* "%b"/"%B" binary formatting (definition in lib/sp_cold.c). */
int sp_fmt_binary(const char *spec, size_t sl, char conv, long long val,
                  char *out, size_t osz);

static sp_RbVal sp_fmt_named_ref(sp_PolyArray *a, const char *nm, char nclose, char *own);  /* defined after the hash structs */
/* Splat arguments into the print builtins: puts(*a) / print(*a) / p(*a)
   expand each element of the (any-kind) array as its own argument. puts
   recurses into array elements, as CRuby does. */
/* puts over an argument list: arrays flatten, an empty one writes nothing */
static void sp_puts_elems(sp_RbVal a) {
  sp_int n = sp_poly_arr_len(a);
  if (sp_poly_recur_seen(SP_POLY_RECUR_PUTS, a.v.p, NULL)) { puts("[...]"); return; }
  int pmark = sp_poly_recur_push(SP_POLY_RECUR_PUTS, a.v.p, NULL);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_arr_get(a, i);
    if (e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(e.cls_id)) { sp_puts_elems(e); continue; }
    const char *s = sp_poly_to_s(e);
    if (s) fputs(s, stdout);
    if (!s || !*s || s[strlen(s) - 1] != 10) putchar(10);
  }
  sp_poly_recur_pop(pmark);
}
/* `puts *arr`: an empty array is a bare `puts` */
static void sp_splat_puts(sp_RbVal a) {
  if (sp_poly_arr_len(a) == 0) { putchar(10); return; }
  sp_puts_elems(a);
}
static void sp_splat_print(sp_RbVal a) {
  sp_int n = sp_poly_arr_len(a);
  for (sp_int i = 0; i < n; i++) {
    const char *s = sp_poly_to_s(sp_poly_arr_get(a, i));
    if (s) fputs(s, stdout);
  }
}
static void sp_splat_p(sp_RbVal a) {
  sp_int n = sp_poly_arr_len(a);
  for (sp_int i = 0; i < n; i++) {
    const char *s = sp_poly_inspect(sp_poly_arr_get(a, i));
    fputs(s ? s : "nil", stdout); putchar(10);
  }
}
/* format(*args): the first element is the format string, the rest its args */
static const char *sp_str_format_polyarr(const char *fmt, sp_PolyArray *a);
/* `fmt % args` where the format arrived boxed (a Fiber#resume value, a
   container read): the same format the typed String path runs. A non-Array
   operand is the one-element list, per String#%. Without this the String fell
   into the numeric arm of sp_poly_mod, where sp_poly_to_i read it as 0 and the
   divisor 0 raised ZeroDivisionError. */
static sp_RbVal sp_poly_str_mod(sp_RbVal a, sp_RbVal b) {
  SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
  sp_PolyArray *ar = (b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id))
                       ? sp_poly_to_poly_array(b) : sp_PolyArray_new();
  SP_GC_ROOT(ar);
  if (!(b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id))) sp_PolyArray_push(ar, b);
  return sp_box_str(sp_str_format_polyarr(a.v.s ? a.v.s : (&("\xff")[1]), ar));
}
static const char *sp_str_format_splat(sp_RbVal a) {
  sp_int n = sp_poly_arr_len(a);
  const char *fmt = n > 0 ? sp_poly_to_s(sp_poly_arr_get(a, 0)) : "";
  sp_PolyArray *rest = sp_PolyArray_new(); SP_GC_ROOT(rest);
  for (sp_int i = 1; i < n; i++) sp_PolyArray_push(rest, sp_poly_arr_get(a, i));
  return sp_str_format_polyarr(fmt ? fmt : "", rest);
}
static inline const char *sp_poly_inspect(sp_RbVal v);            /* %p; defined after the container types */
static const char *sp_str_format_polyarr(const char *fmt, sp_PolyArray *a) {
  size_t cap = strlen(fmt) + 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t out = 0; sp_int idx = 0; const char *p = fmt;
  /* CRuby refuses to mix numbered (%1$s), sequential (%s) and named (%<a>d)
     references in one format string (#3723) */
  sp_bool used_numbered = FALSE, used_sequential = FALSE, used_named = FALSE;
  while (*p) {
    if (*p != '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = *p++; continue;
    }
    if (p[1] == '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = '%'; p += 2; continue;
    }
    char spec[64]; size_t sl = 0; spec[sl++] = *p++;
    sp_bool this_numbered = FALSE;
    /* positional argument reference: %N$conv selects the Nth (1-based) arg */
    {
      const char *q = p; sp_int argnum = 0; sp_bool overflow = FALSE;
      while (*q >= '0' && *q <= '9') {
        if (sp_int_mul_overflow_p(argnum, 10, &argnum) ||
            sp_int_add_overflow_p(argnum, *q - '0', &argnum)) { overflow = TRUE; break; }
        q++;
      }
      if (!overflow && argnum > 0 && *q == '$') {
        idx = argnum - 1; p = q + 1;
        if (used_sequential || used_named) { free(buf); sp_raise_cls("ArgumentError", "numbered(1) after unnumbered(1)"); }
        used_numbered = TRUE; this_numbered = TRUE;
      }
    }
    /* %<name>conv / %{name}: named reference into the format's hash argument.
       %{name} interpolates the value's to_s directly (no conversion spec);
       %<name> substitutes the value for the following conversion. */
    sp_RbVal named_v = sp_box_nil(); sp_bool have_named = FALSE;
    if (*p == '<' || *p == '{') {
      char nclose = (*p == '<') ? '>' : '}';
      char nm[64]; size_t nl = 0; const char *q = p + 1;
      while (*q && *q != nclose && nl < sizeof nm - 1) nm[nl++] = *q++;
      nm[nl] = 0;
      if (*q == nclose) {
        p = q + 1;
        named_v = sp_fmt_named_ref(a, nm, nclose, buf);
        have_named = TRUE;
        if (used_numbered || used_sequential) { free(buf); sp_raise_cls("ArgumentError", "named after unnumbered(1)"); }
        used_named = TRUE;
        if (nclose == '}') {
          const char *sv2 = sp_poly_to_s(named_v);
          size_t svl = sv2 ? strlen(sv2) : 0;
          if (out + svl + 1 >= cap) { cap = (out + svl) * 2 + 64; buf = (char *)realloc(buf, cap); }
          if (svl) memcpy(buf + out, sv2, svl);
          out += svl;
          continue;
        }
      }
    }
    while (*p && sl < sizeof(spec) - 8) {
      char c = *p;
      if (c == '*') {
        /* dynamic width / precision: the next positional argument supplies the
           number (a negative width left-justifies, like printf) */
        if (idx >= a->len) { free(buf); sp_raise_cls("ArgumentError", "too few arguments"); }
        sp_RbVal wv = a->data[idx]; idx++;
        long long wnum = (wv.tag == SP_TAG_INT) ? (long long)wv.v.i
                       : (wv.tag == SP_TAG_FLT) ? (long long)wv.v.f : 0;
        /* a negative PRECISION is ignored (a negative width left-justifies) */
        if (wnum < 0 && sl > 0 && spec[sl - 1] == '.') { sl--; p++; continue; }
        char wbuf[24]; int wl = snprintf(wbuf, sizeof wbuf, "%lld", wnum);
        if (sl + (size_t)wl < sizeof(spec) - 8) { memcpy(spec + sl, wbuf, (size_t)wl); sl += (size_t)wl; }
        p++;
      }
      else if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '.' || (c >= '0' && c <= '9')) { spec[sl++] = c; p++; }
      else break;
    }
    /* a trailing bare `%` is a malformed format, not a literal (#3723) */
    if (!*p) { free(buf); sp_raise_cls("ArgumentError", "incomplete format specifier; use %% (double %) instead"); }
    char conv = *p++; spec[sl++] = conv;
    /* Ruby's %u is %d: it prints a negative as -N rather than wrapping */
    if (conv == 'u') { conv = 'd'; spec[sl - 1] = 'd'; }
    char fmt_use[80];
    if (conv == 'd' || conv == 'i') {
      memcpy(fmt_use, spec, sl - 1);
      fmt_use[sl - 1] = 'l'; fmt_use[sl] = 'l'; fmt_use[sl + 1] = conv; fmt_use[sl + 2] = 0;
    }
else {
      memcpy(fmt_use, spec, sl); fmt_use[sl] = 0;
    }
    char tmp[256]; int wn = 0;
    sp_RbVal v;
    if (have_named) v = named_v;
    else {
      /* CRuby raises rather than padding a missing argument with nil/0 */
      if (idx >= a->len) { free(buf); sp_raise_cls("ArgumentError", "too few arguments"); }
      if (!this_numbered) {
        if (used_named) { free(buf); sp_raise_cls("ArgumentError", "unnumbered(1) mixed with named"); }
        if (used_numbered) { free(buf); sp_raise_cls("ArgumentError", "unnumbered(1) mixed with numbered"); }
        used_sequential = TRUE;
      }
      v = a->data[idx]; idx++;
    }
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o' ||
        conv == 'b' || conv == 'B') {
      long long lv = 0;
      /* A BIGNUM does not fit the long long the conversions below format, and
         truncating it printed 0 for every value past 64 bits. Render its own
         digits in the requested base and pass them through the spec as a
         string, which keeps the width and left-justify flags (#4010). */
      if (v.tag == SP_TAG_BIGINT && v.v.p) {
        sp_int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o') ? 8
                    : (conv == 'b' || conv == 'B') ? 2 : 10;
        const char *bs = sp_bigint_to_s_base((sp_Bigint *)v.v.p, base);
        if (!bs) bs = "0";
        /* The sign and base flags belong to the digits, not to the %s that
           carries the width: compose them in, then keep only the width and
           left-justify flags in the spec printf sees. */
        int neg_b = bs[0] == '-';
        char pre[8]; int pl = 0;
        if (!neg_b) for (int fi = 1; fi < sl - 1; fi++) {
          if (spec[fi] == '+') { pre[pl++] = '+'; break; }
          if (spec[fi] == ' ') { pre[pl++] = ' '; break; }
          if (spec[fi] != '-' && spec[fi] != '#' && spec[fi] != '0') break;
        }
        for (int fi = 1; fi < sl - 1; fi++) {
          if (spec[fi] == '#') {
            if (base == 16) { pre[pl++] = '0'; pre[pl++] = (conv == 'X') ? 'X' : 'x'; }
            else if (base == 8) pre[pl++] = '0';
            else if (base == 2) { pre[pl++] = '0'; pre[pl++] = (conv == 'B') ? 'B' : 'b'; }
            break;
          }
          if (spec[fi] < '0' || spec[fi] > '9') continue;
          break;
        }
        pre[pl] = 0;
        char digits[512];
        snprintf(digits, sizeof digits, "%s%s%s", neg_b ? "-" : "", pre, bs + (neg_b ? 1 : 0));
        char bfmt[80]; int bl = 0;
        bfmt[bl++] = '%';
        for (int fi = 1; fi < sl - 1; fi++)
          if (spec[fi] == '-' || (spec[fi] >= '0' && spec[fi] <= '9' && !(bl == 1 && spec[fi] == '0')))
            bfmt[bl++] = spec[fi];
        bfmt[bl++] = 's'; bfmt[bl] = 0;
        wn = snprintf(tmp, sizeof(tmp), bfmt, digits);
        if (conv == 'X') for (char *q = tmp; *q; q++) if (*q >= 'a' && *q <= 'f') *q -= 32;
      }
      else {
      if (v.tag == SP_TAG_INT) lv = (long long)v.v.i;
      else if (v.tag == SP_TAG_FLT) lv = (long long)v.v.f;
      /* a String argument converts the way Integer() does, so unparseable text
         is an ArgumentError rather than a silent zero (#3554) */
      else if (v.tag == SP_TAG_STR && v.v.s) lv = (long long)sp_str_to_i_strict(v.v.s);
      /* Anything else is not a number at all, and formatting it as 0 was a
         quiet wrong answer where CRuby refuses (#4010). Same wording and same
         value spelling as Integer() uses. */
      else { free(buf); sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Integer", sp_convert_src_name(v))); }
      /* the non-decimal bases go through our own formatter: C's printf drops
         the '+' and ' ' flags on them and has no two's-complement form */
      if (conv == 'd' || conv == 'i') wn = snprintf(tmp, sizeof(tmp), fmt_use, lv);
      else wn = sp_fmt_binary(spec, sl, conv, lv, tmp, sizeof(tmp));
      }
    }
else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G' ||
         conv == 'a' || conv == 'A') {
      double dv = 0;
      if (v.tag == SP_TAG_FLT) dv = v.v.f;
      else if (v.tag == SP_TAG_INT) dv = (double)v.v.i;
      else if (sp_poly_is_rational(v)) dv = sp_poly_to_f_with_rational(v);
      else if (v.tag == SP_TAG_BIGINT) dv = sp_poly_to_f(v);
      /* a String converts the way Float() does; anything else is not a number
         and CRuby refuses rather than formatting a zero (#4010) */
      else if (v.tag == SP_TAG_STR) dv = (double)sp_str_to_f_strict(v.v.s ? v.v.s : sp_str_empty);
      else { free(buf); sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", sp_convert_src_name(v))); }
      /* Ruby prints non-finite floats as Inf/-Inf/NaN (C printf lowercases) */
      if (!isfinite(dv)) wn = snprintf(tmp, sizeof(tmp), "%s", isnan(dv) ? "NaN" : dv > 0 ? "Inf" : "-Inf");
      /* pinned "C" locale so the decimal point is always '.', not the process
         locale's separator (the printf field/flag machinery stays libc's) */
      else wn = sp_snprintf_ruby_float(tmp, sizeof(tmp), fmt_use, dv);
    }
else if (conv == 's' || conv == 'p') {
      /* %s is to_s, %p is inspect -- for every tag (symbols, arrays, hashes,
         booleans, user objects), not just the scalar three. Hash/Array to_s
         is its inspect, which sp_poly_to_s already returns. %p formats through
         a 's' conversion (C's %p is a pointer). A value longer than the spec
         buffer is appended raw (width padding on it is a non-case). */
      const char *sv = (conv == 's') ? sp_poly_to_s(v) : sp_poly_inspect(v);
      if (!sv) sv = "";
      fmt_use[sl - 1] = 's';
      if (strlen(sv) + 8 >= sizeof(tmp)) {
        size_t svl = strlen(sv);
        if (out + svl + 1 >= cap) { cap = (out + svl) * 2 + 64; buf = (char *)realloc(buf, cap); }
        memcpy(buf + out, sv, svl); out += svl;
        continue;
      }
      wn = snprintf(tmp, sizeof(tmp), fmt_use, sv);
    }
else if (conv == 'c') {
      /* an Integer is a codepoint (UTF-8 encoded), a String contributes its
         first whole character -- not a single truncated byte (#3083) */
      char cbuf[8]; int clen = 0;
      if (v.tag == SP_TAG_INT) clen = sp_utf8_encode((uint32_t)v.v.i, cbuf);
      else if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) {
        clen = sp_utf8_advance(v.v.s);
        if (clen > 8) clen = 8;
        memcpy(cbuf, v.v.s, (size_t)clen);
      }
      /* the character occupies one column; honor a width and the '-' flag */
      int left = 0, width = 0;
      for (size_t fi = 1; fi + 1 < sl; fi++) {
        char fc = spec[fi];
        if (fc == '-') left = 1;
        else if (fc >= '0' && fc <= '9') width = width * 10 + (fc - '0');
      }
      int pad = width > 1 ? width - 1 : 0;
      if ((size_t)(clen + pad) >= sizeof(tmp)) pad = (int)sizeof(tmp) - clen - 1;
      if (pad < 0) pad = 0;
      int o2 = 0;
      if (!left) for (int i = 0; i < pad; i++) tmp[o2++] = ' ';
      memcpy(tmp + o2, cbuf, (size_t)clen); o2 += clen;
      if (left) for (int i = 0; i < pad; i++) tmp[o2++] = ' ';
      tmp[o2] = 0;
      wn = o2;
    }
else {
      /* CRuby: an unknown / truncated conversion is a hard error */
      free(buf);
      if (conv)
        sp_raise_cls("ArgumentError", sp_sprintf("malformed format string - %%%c", conv));
      else
        sp_raise_cls("ArgumentError", "incomplete format specifier; use %% (double %) instead");
      return "";
    }
    if (wn < 0) continue;
    if (out + (size_t)wn + 1 >= cap) { cap = ((out + wn) * 2) + 64; buf = (char *)realloc(buf, cap); }
    memcpy(buf + out, tmp, wn); out += wn;
  }
  buf[out] = 0;
  char *r = sp_str_alloc(out); memcpy(r, buf, out); free(buf); return r;
}


/* Box any array-kind element into a PolyArray so assoc/rassoc can return it
   through their PolyArray* type regardless of the matched pair's own kind
   (a like-typed pair such as [1, 2] is an IntArray, not a PolyArray). */
static sp_PolyArray *sp_pair_to_poly(sp_RbVal el) {
  if (el.tag == SP_TAG_OBJ && el.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)el.v.p;
  sp_int n = sp_array_kind_len(el);
  if (n < 0) n = 0;
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  for (sp_int j = 0; j < n; j++) sp_PolyArray_push(r, sp_poly_arr_get(el, j));
  return r;
}

/* Array#assoc — return the first sub-array whose first element equals `key`.
   Returns NULL when no match so the caller's `.inspect` round-trips to "nil".
   Each pair may be any array kind, so compare element 0 via sp_poly_arr_get;
   a pair with no element 0 (a non-array or empty array) is skipped. */
static sp_PolyArray *sp_PolyArray_assoc(sp_PolyArray *a, sp_RbVal key) {
  if (!a) return NULL;
  for (sp_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (sp_array_kind_len(el) >= 1 && sp_poly_eq(sp_poly_arr_get(el, 0), key))
      return sp_pair_to_poly(el);
  }
  return NULL;
}

/* Array#rassoc — same as assoc but matches against the second
   element of each sub-array (a pair with fewer than 2 elements is skipped). */
static sp_PolyArray *sp_PolyArray_rassoc(sp_PolyArray *a, sp_RbVal val) {
  if (!a) return NULL;
  for (sp_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (sp_array_kind_len(el) >= 2 && sp_poly_eq(sp_poly_arr_get(el, 1), val))
      return sp_pair_to_poly(el);
  }
  return NULL;
}
/* Depth-bounded variant. depth==0 returns a shallow copy; each
   recursive step decrements depth, and a negative depth means
   "unlimited" (same as flatten without arg). Used by
   `Array#flatten(n)`. */
static void sp_PolyArray_flatten_into_n(sp_PolyArray *dst, sp_RbVal v, sp_int depth) {
  if (depth == 0 || v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (sp_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (sp_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_SYM_ARRAY) { sp_IntArray *ya = (sp_IntArray *)v.v.p; for (sp_int i = 0; i < ya->len; i++) sp_PolyArray_push(dst, sp_box_sym((sp_sym)ya->data[ya->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_FLT_ARRAY) { sp_FloatArray *fa = (sp_FloatArray *)v.v.p; for (sp_int i = 0; i < fa->len; i++) sp_PolyArray_push(dst, sp_box_float(fa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *pa = (sp_PolyArray *)v.v.p;
    /* A negative depth is the "no limit" form, and the only one a cycle can
       trap; it stays negative rather than counting down from INT64_MAX, so the
       walk that carries the path is the one the code says it is. */
    if (depth < 0) {
      if (sp_poly_recur_seen(SP_POLY_RECUR_FLATTEN, pa, NULL))
        sp_poly_recur_raise(SP_POLY_RECUR_FLATTEN, "tried to flatten recursive array");
      int mark = sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, pa, NULL);
      for (sp_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into_n(dst, pa->data[i], depth);
      sp_poly_recur_pop(mark);
      return;
    }
    for (sp_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into_n(dst, pa->data[i], depth - 1);
    return;
  }
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten_n(sp_PolyArray *a, sp_int depth) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_new();
  SP_GC_ROOT(b);
  if (!a) return b;
  int mark = depth < 0 ? sp_poly_recur_push(SP_POLY_RECUR_FLATTEN, a, NULL) : sp_poly_recur_save();
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into_n(b, a->data[i], depth);
  sp_poly_recur_pop(mark);
  return b;
}
/* Transpose a poly-array of typed arrays (each row becomes a column).
   Handles rows that are IntArray, FloatArray, or StrArray.
   Result: a PolyArray of boxed typed column arrays. */
/* sp_poly_array_transpose: moved to lib/sp_cold.c */
sp_PolyArray *sp_poly_array_transpose(sp_PolyArray *rows);
/* Keep old name as alias for backward compat with existing generated code. */
#define sp_int_array_transpose sp_poly_array_transpose
/* Sum the integer-tagged elements of a poly_array. Used by
   `Array#sum` on a poly_array whose runtime tags are uniform int
   (e.g. the result of `arr.map { _1[:int_key] }`). Non-int tags
   contribute zero. */
static sp_int sp_PolyArray_sum_int(sp_PolyArray *a) { if (!a) return 0; sp_int s = 0; for (sp_int i = 0; i < a->len; i++) { if (a->data[i].tag == SP_TAG_INT) s += a->data[i].v.i; } return s; }
/* Array#sum with the default (Integer 0) initial value, folding via sp_poly_add
   so the result promotes to the element class (Float for any Float element,
   Rational/Bignum likewise) rather than dropping non-Integer elements. */
static sp_RbVal sp_PolyArray_sum_poly(sp_PolyArray *a) { sp_RbVal s = sp_box_int(0); if (!a) return s; for (sp_int i = 0; i < a->len; i++) s = sp_poly_add(s, a->data[i]); return s; }
/* Array#sum with a String initial value: concatenate the string elements onto
   the initial (["a","b"].sum("") == "ab"). */
static const char *sp_PolyArray_sum_str(sp_PolyArray *a, const char *init) { const char *s = init ? init : ""; if (!a) return s; for (sp_int i = 0; i < a->len; i++) { if (a->data[i].tag == SP_TAG_STR && a->data[i].v.s) s = sp_str_concat(s, a->data[i].v.s); } return s; }
/* Array#sum with a Float initial value: numeric fold over Integer and Float
   elements, accumulating as double (the result is a Float). */
static sp_float sp_PolyArray_sum_float(sp_PolyArray *a) { if (!a) return 0.0; sp_float s = 0.0; for (sp_int i = 0; i < a->len; i++) { if (a->data[i].tag == SP_TAG_INT) s += (sp_float)a->data[i].v.i; else if (a->data[i].tag == SP_TAG_FLT) s += a->data[i].v.f; } return s; }
/* Bignum#downto(hi)/#upto(hi) materialized: a poly array of Bignums from `lo`
   to `hi` inclusive (descending for downto, ascending for upto) (#2305). */
static sp_PolyArray *sp_bigint_range_array(sp_Bigint *lo, sp_Bigint *hi, int up) {
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  sp_Bigint *one = sp_bigint_new_int(1);
  sp_Bigint *cur = lo;
  while (up ? sp_bigint_cmp(cur, hi) <= 0 : sp_bigint_cmp(cur, hi) >= 0) {
    sp_PolyArray_push(a, sp_box_bigint(cur));
    cur = up ? sp_bigint_add(cur, one) : sp_bigint_sub(cur, one);
  }
  return a;
}
/* Array#sum with an Array initial value over an array of arrays: one-level
   concatenation into a fresh poly array ([[1],[2]].sum([]) == [1, 2]). */
static sp_PolyArray *sp_PolyArray_sum_concat(sp_PolyArray *a, sp_RbVal init) {
  SP_GC_ROOT(a);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_PolyArray_flatten_into_n(r, init, 1);
  if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into_n(r, a->data[i], 1);
  return r;
}
/* A widened copy is still the same Ruby object, so it carries the frozen bit:
   without it `a = [1, 2].freeze` followed by any call that widens a -- push of
   a String, replace by another kind -- quietly mutated a frozen array. The
   flag is set AFTER the pushes, which would otherwise raise on it. */
static sp_PolyArray *sp_PolyArray_from_int_array(sp_IntArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (sp_int i = 0; i < a->len; i++) { sp_int v = a->data[a->start+i]; sp_PolyArray_push(p, v == SP_INT_NIL ? sp_box_nil() : sp_box_int(v)); } p->frozen = a->frozen; return p; }
static sp_PolyArray *sp_PolyArray_from_str_array(sp_StrArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(p, sp_box_str(a->data[i])); p->frozen = a->frozen; return p; }
static sp_PolyArray *sp_PolyArray_from_float_array(sp_FloatArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(p, sp_box_float(a->data[i])); p->frozen = a->frozen; return p; }
/* Reverse coercions: materialize a concrete typed array from a poly array by
   unboxing each element to the declared element type. Used to honor a typed-array
   return annotation (e.g. RBS `-> Array[String]`) when the body produced a poly
   array -- the element boxes carry the runtime values, so this is a per-element
   unbox, not a reinterpret. */
static sp_StrArray *sp_StrArray_from_poly_array(sp_PolyArray *a) { sp_StrArray *r = sp_StrArray_new(); if (!a) return r; SP_GC_ROOT(a); SP_GC_ROOT(r); for (sp_int i = 0; i < a->len; i++) sp_StrArray_push(r, sp_poly_to_s(a->data[i])); return r; }
static sp_IntArray *sp_IntArray_from_poly_array(sp_PolyArray *a) { sp_IntArray *r = sp_IntArray_new(); if (!a) return r; SP_GC_ROOT(a); SP_GC_ROOT(r); for (sp_int i = 0; i < a->len; i++) sp_IntArray_push(r, sp_poly_to_i(a->data[i])); return r; }
static sp_FloatArray *sp_FloatArray_from_poly_array(sp_PolyArray *a) { sp_FloatArray *r = sp_FloatArray_new(); if (!a) return r; SP_GC_ROOT(a); SP_GC_ROOT(r); for (sp_int i = 0; i < a->len; i++) sp_FloatArray_push(r, sp_poly_to_f(a->data[i])); return r; }
static void sp_PolyArray_reverse_bang(sp_PolyArray *a) {sp_gc_wb((void*)a);  if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return; } for (sp_int i = 0, j = a->len - 1; i < j; i++, j--) { sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
static void sp_PolyArray_shuffle_bang(sp_PolyArray *a) {sp_gc_wb((void*)a);  if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return; } for (sp_int i = a->len - 1; i > 0; i--) { sp_int j = sp_krand_below(i + 1); sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
/* poly.reverse: `reverse` is both Array#reverse and String#reverse, so a poly
   receiver dispatches on the runtime kind -- an array yields a reversed poly
   array, anything else reverses its string form (#2905). */
static sp_RbVal sp_poly_reverse(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    SP_GC_ROOT_RBVAL(v);
    sp_PolyArray *r = sp_PolyArray_dup(sp_poly_to_poly_array(v));
    sp_PolyArray_reverse_bang(r);
    return sp_box_poly_array(r);
  }
  return sp_box_str(sp_str_reverse(sp_poly_recv_s(v, "reverse")));
}
static void sp_PolyArray_rotate_bang(sp_PolyArray*a,sp_int n){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY);return;}
  if(a->len<=0)return;
  n=((n%a->len)+a->len)%a->len;
  if(n==0)return;
  sp_RbVal*d=a->data;
  sp_int rest=a->len-n;
  sp_int keep=n<rest?n:rest;
  sp_RbVal stackbuf[32];
  sp_RbVal*t=keep<=32?stackbuf:(sp_RbVal*)malloc(sizeof(sp_RbVal)*(size_t)keep);
  if(!t)sp_oom_die();
  if(n<=rest){
    memcpy(t,d,sizeof(sp_RbVal)*(size_t)n);
    memmove(d,d+n,sizeof(sp_RbVal)*(size_t)rest);
    memcpy(d+rest,t,sizeof(sp_RbVal)*(size_t)n);
  }
  else{
    memcpy(t,d+n,sizeof(sp_RbVal)*(size_t)rest);
    memmove(d+rest,d,sizeof(sp_RbVal)*(size_t)n);
    memcpy(d,t,sizeof(sp_RbVal)*(size_t)rest);
  }
  if(t!=stackbuf)free(t);
}
static sp_PolyArray *sp_PolyArray_shuffle(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_shuffle_bang(b); return b; }
/* When sort hits an incomparable pair the result is discarded and we raise
   ArgumentError, matching CRuby. The comparator cannot raise (it would longjmp
   out of the sort), so it records the offending pair and sort_bang raises after. */
/* The comparison ORDERING uses: sp_poly_cmp with the int-array fallback, plus
   the nil pair. nil defines #<=> and not #<=, so CRuby answers `nil <=> nil`
   with 0 -- [nil, nil].min is nil and .sort is the identity -- while
   `nil <= nil` stays a NoMethodError on the operator path (#4006). */
static sp_int sp_poly_order_cmp(sp_RbVal a, sp_RbVal b, sp_bool *ok) {
  sp_int r = sp_poly_cmp(a, b, ok);
  if (!*ok) r = sp_poly_cmp_int_arrays(a, b, ok);
  if (!*ok && a.tag == SP_TAG_NIL && b.tag == SP_TAG_NIL) { *ok = TRUE; r = 0; }
  return r;
}
static int _sp_sort_incomparable;
static sp_RbVal _sp_sort_inc_a, _sp_sort_inc_b;
static int _sp_poly_cmp_rec(const void *pa, const void *pb) {
  if (_sp_sort_incomparable) return 0;
  sp_bool ok = FALSE;
  sp_int r = sp_poly_order_cmp(*(const sp_RbVal *)pa, *(const sp_RbVal *)pb, &ok);
  if (!ok) { _sp_sort_incomparable = 1; _sp_sort_inc_a = *(const sp_RbVal *)pa; _sp_sort_inc_b = *(const sp_RbVal *)pb; return 0; }
  return (int)r;
}
/* Bottom-up merge sort over boxed elements. libc qsort visits pairs in an
   implementation-defined order, so which pair gets recorded as incomparable
   (and hence the ArgumentError message operands) would vary by platform; a
   fixed merge schedule keeps it identical everywhere, with the left/earlier
   element as the `<=>` receiver like CRuby. Both `a` and the scratch copy
   are rooted PolyArrays, so every element stays GC-reachable while the
   comparator (which can allocate) runs; stale slots in the buffer being
   overwritten are still valid boxed values, so scanning them is safe. */
static void _sp_poly_msort(sp_PolyArray *a, int (*cmp)(const void *, const void *)) {
  if (!a || a->len < 2) return;
  SP_GC_ROOT(a);
  sp_PolyArray *tmp = sp_PolyArray_dup(a);
  SP_GC_ROOT(tmp);
  sp_RbVal *src = a->data, *dst = tmp->data;
  for (sp_int w = 1; w < a->len; w *= 2) {
    for (sp_int lo = 0; lo < a->len; lo += 2 * w) {
      sp_int mid = lo + w < a->len ? lo + w : a->len;
      sp_int hi = lo + 2 * w < a->len ? lo + 2 * w : a->len;
      sp_int i = lo, j = mid, k = lo;
      while (i < mid && j < hi)
        dst[k++] = cmp(&src[i], &src[j]) <= 0 ? src[i++] : src[j++];
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    sp_RbVal *t = src; src = dst; dst = t;
  }
  if (src != a->data) memcpy(a->data, src, (size_t)a->len * sizeof(sp_RbVal));
}
/* max/min over boxed elements: numerics/strings via sp_poly_cmp, int arrays
   lexicographically. Returns nil for an empty array. */
static sp_RbVal sp_PolyArray_max(sp_PolyArray *a) {sp_gc_wb((void*)a); 
  if (!a || a->len == 0) return sp_box_nil();
  SP_GC_ROOT(a);  /* sp_poly_cmp can allocate; keep a (and best, which is one of
                     its elements) reachable across the comparisons. */
  sp_RbVal best = a->data[0];
  for (sp_int i = 1; i < a->len; i++) {
    sp_bool ok = FALSE;
    sp_int r = sp_poly_order_cmp(a->data[i], best, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a->data[i]), sp_cmperr_desc(best)));
    if (r > 0) best = a->data[i];
  }
  return best;
}
static sp_RbVal sp_PolyArray_min(sp_PolyArray *a) {sp_gc_wb((void*)a); 
  if (!a || a->len == 0) return sp_box_nil();
  SP_GC_ROOT(a);  /* sp_poly_cmp can allocate; keep a (and best, which is one of
                     its elements) reachable across the comparisons. */
  sp_RbVal best = a->data[0];
  for (sp_int i = 1; i < a->len; i++) {
    sp_bool ok = FALSE;
    sp_int r = sp_poly_order_cmp(a->data[i], best, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a->data[i]), sp_cmperr_desc(best)));
    if (r < 0) best = a->data[i];
  }
  return best;
}
static void sp_PolyArray_sort_bang(sp_PolyArray *a) {
  if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return; }
  /* Root the array across the sort: the comparator runs sp_poly_cmp, which can
     allocate (e.g. a bigint temp) and trigger GC; without this, a precise sweep
     could collect a (and its elements) mid-sort. This also roots the transient
     copy made by sp_PolyArray_sort. */
  SP_GC_ROOT(a);
  if (a->len > 1) {
    /* save/restore the flag and the offending pair so a comparison that
       re-enters sort cannot clobber this call's state. */
    int prev = _sp_sort_incomparable;
    sp_RbVal prev_a = _sp_sort_inc_a, prev_b = _sp_sort_inc_b;
    _sp_sort_incomparable = 0;
    _sp_poly_msort(a, _sp_poly_cmp_rec);
    int inc = _sp_sort_incomparable;
    sp_RbVal ia = _sp_sort_inc_a, ib = _sp_sort_inc_b;
    _sp_sort_incomparable = prev;
    _sp_sort_inc_a = prev_a;
    _sp_sort_inc_b = prev_b;
    if (inc) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(ia), sp_cmperr_desc(ib)));
  }
}
static sp_PolyArray *sp_PolyArray_sort(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_sort_bang(b); return b; }
/* Object-array (sp_PtrArray of one user class, TY_OBJ_ARRAY) comparison
   family: box the elements with the statically-known cls_id (tag assembly,
   no allocation) and reuse the PolyArray comparator machinery -- the user
   `<=>` via the cmp hook, incomparable pairs raising the Comparable
   ArgumentError. Only the fresh result containers allocate; everything live
   across an allocation is rooted. */
static sp_PolyArray *sp_ptr_array_box(sp_PtrArray *a, int cls_id) {
  sp_PolyArray *p = sp_PolyArray_new(); SP_GC_ROOT(p);
  if (a) for (sp_int i = 0; i < a->len; i++)
    sp_PolyArray_push(p, sp_box_nullable_obj(a->data[i], cls_id));
  return p;
}
static sp_PtrArray *sp_PtrArray_sort_obj(sp_PtrArray *a, int cls_id) __attribute__((unused));
static sp_PtrArray *sp_PtrArray_sort_obj(sp_PtrArray *a, int cls_id) {
  SP_GC_ROOT(a);
  sp_PolyArray *p = sp_ptr_array_box(a, cls_id);
  SP_GC_ROOT(p);   /* box's own root is popped on its return; sort + new_scan below allocate */
  sp_PolyArray_sort_bang(p);
  sp_PtrArray *r = a ? sp_PtrArray_new_scan(a->scan_elem) : sp_PtrArray_new();
  SP_GC_ROOT(r);
  for (sp_int i = 0; i < p->len; i++) sp_PtrArray_push(r, p->data[i].v.p);
  return r;
}
static void sp_PtrArray_sort_obj_bang(sp_PtrArray *a, int cls_id) __attribute__((unused));
static void sp_PtrArray_sort_obj_bang(sp_PtrArray *a, int cls_id) {sp_gc_wb((void*)a); 
  if (!a) return;
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_PTR_ARRAY); return; }
  SP_GC_ROOT(a);
  sp_PolyArray *p = sp_ptr_array_box(a, cls_id);
  SP_GC_ROOT(p);   /* box's own root is popped on its return; sort_bang runs the user <=> (allocates) */
  sp_PolyArray_sort_bang(p);
  for (sp_int i = 0; i < a->len; i++) a->data[i] = p->data[i].v.p;
}
/* min/max over an object array; empty -> NULL (the object-typed nil). */
static void *sp_PtrArray_minmax_obj(sp_PtrArray *a, int cls_id, int want_max) __attribute__((unused));
static void *sp_PtrArray_minmax_obj(sp_PtrArray *a, int cls_id, int want_max) {sp_gc_wb((void*)a); 
  if (!a || a->len == 0) return NULL;
  SP_GC_ROOT(a);
  void *best = a->data[0];
  for (sp_int i = 1; i < a->len; i++) {
    sp_bool ok = FALSE;
    sp_RbVal bi = sp_box_nullable_obj(a->data[i], cls_id);
    sp_RbVal bb = sp_box_nullable_obj(best, cls_id);
    sp_int r = sp_poly_order_cmp(bi, bb, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(bi), sp_cmperr_desc(bb)));
    if (want_max ? (r > 0) : (r < 0)) best = a->data[i];
  }
  return best;
}
/* Compare two boxed arrays element-wise (Array#<=> semantics): first differing
   comparable element decides, else the shorter array sorts first. Used to order
   Hash#sort's [key, value] pairs. */
static int _sp_pair_cmp_incomparable;  /* set when two pairs cannot be ordered */
static int _sp_poly_pair_cmp(const void *pa, const void *pb) {
  /* Once a pair is found incomparable the sort result is discarded and we
     raise, so skip the remaining comparisons (and any work they would do). */
  if (_sp_pair_cmp_incomparable) return 0;
  sp_RbVal a = *(const sp_RbVal *)pa, b = *(const sp_RbVal *)pb;
  sp_int na = sp_poly_arr_len(a), nb = sp_poly_arr_len(b);
  sp_int n = na < nb ? na : nb;
  for (sp_int i = 0; i < n; i++) {
    sp_bool ok = FALSE;
    sp_int r = sp_poly_cmp(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i), &ok);
    if (!ok) { _sp_pair_cmp_incomparable = 1; return 0; }
    if (r != 0) return r < 0 ? -1 : 1;
  }
  return (na > nb) - (na < nb);
}
static sp_PolyArray *sp_PolyArray_sort_pairs(sp_PolyArray *a) {
  /* `a` is the caller's transient pair array, unrooted at the call site; root
     it before sp_PolyArray_dup allocates (and may collect). */
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_dup(a);
  if (b && b->len > 1) {
    /* Save/restore the flag around the sort so a comparison that re-enters
       sort_pairs (e.g. via a nested sort) cannot clobber this call's state. */
    int prev = _sp_pair_cmp_incomparable;
    _sp_pair_cmp_incomparable = 0;
    _sp_poly_msort(b, _sp_poly_pair_cmp);
    int incomparable = _sp_pair_cmp_incomparable;
    _sp_pair_cmp_incomparable = prev;
    if (incomparable)
      sp_raise_cls("ArgumentError", "comparison of Array with Array failed");
  }
  return b;
}
/* Schwartzian helper for Hash#sort_by: `a` is an array of [sort_key, value]
   tuples; sort it by the comparable sort_key and return the values in order. */
static int _sp_poly_first_cmp(const void *pa, const void *pb) {
  sp_bool ok = FALSE;
  sp_int r = sp_poly_cmp(sp_poly_arr_get(*(const sp_RbVal *)pa, 0),
                          sp_poly_arr_get(*(const sp_RbVal *)pb, 0), &ok);
  return ok ? (r < 0 ? -1 : (r > 0 ? 1 : 0)) : 0;
}
static sp_PolyArray *sp_PolyArray_sort_by_first(sp_PolyArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_dup(a); SP_GC_ROOT(b);
  if (b && b->len > 1) _sp_poly_msort(b, _sp_poly_first_cmp);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  for (sp_int i = 0; b && i < b->len; i++) sp_PolyArray_push(r, sp_poly_arr_get(b->data[i], 1));
  return r;
}
/* value-form bangs: CRuby returns self when the call CHANGED the receiver
   and nil when it was a no-op. The plain *_bang statements stay void. */
static void sp_PolyArray_uniq_bang(sp_PolyArray *a);
static sp_RbVal sp_PolyArray_uniq_bangq(sp_PolyArray *a) {
  if (!a) return sp_box_nil();
  sp_int n = a->len;
  sp_PolyArray_uniq_bang(a);
  return a->len != n ? sp_box_poly_array(a) : sp_box_nil();
}
static sp_RbVal sp_PolyArray_compact_bangq(sp_PolyArray *a) {
  if (!a) return sp_box_nil();
  sp_int n = a->len;
  sp_PolyArray_compact_bang(a);
  return a->len != n ? sp_box_poly_array(a) : sp_box_nil();
}
static sp_RbVal sp_PolyArray_flatten_bangq(sp_PolyArray *a) {
  if (!a) return sp_box_nil();
  int ch = 0;
  for (sp_int i = 0; i < a->len && !ch; i++)
    if (a->data[i].tag == SP_TAG_OBJ && sp_poly_is_array_kind(a->data[i].cls_id)) ch = 1;
  if (!ch) return sp_box_nil();
  sp_PolyArray_flatten_bang(a);
  return sp_box_poly_array(a);
}
static sp_RbVal sp_PolyArray_flatten_bangq_depth(sp_PolyArray *a, sp_int d) {sp_gc_wb((void*)a); 
  if (!a) return sp_box_nil();
  if (a->frozen) sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY);
  SP_GC_ROOT(a);
  int ch = 0;
  for (sp_int i = 0; i < a->len && !ch; i++)
    if (a->data[i].tag == SP_TAG_OBJ && sp_poly_is_array_kind(a->data[i].cls_id)) ch = 1;
  if (!ch || d == 0) return sp_box_nil();
  sp_PolyArray *f = sp_PolyArray_flatten_depth(a, d); SP_GC_ROOT(f);
  a->len = 0;
  for (sp_int i = 0; i < f->len; i++) sp_PolyArray_push(a, f->data[i]);
  return sp_box_poly_array(a);
}
static sp_RbVal sp_IntArray_uniq_bangq(sp_IntArray *a) {
  if (!a) return sp_box_nil();
  sp_int n = a->len;
  sp_IntArray_uniq_bang(a);
  return a->len != n ? sp_box_int_array(a) : sp_box_nil();
}
/* uniq dedups with eql? (class-strict: 1 and 1.0 both survive), as CRuby. */
static sp_bool sp_poly_eql(sp_RbVal a, sp_RbVal b);
static void sp_PolyArray_uniq_bang(sp_PolyArray*a){sp_gc_wb((void*)a); if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY);return;}for(sp_int i=0;i<a->len;){int dup=0;for(sp_int j=0;j<i;j++){if(sp_poly_eql(a->data[j],a->data[i])){dup=1;break;}}if(dup){for(sp_int k2=i;k2<a->len-1;k2++)a->data[k2]=a->data[k2+1];a->len--;}
else i++;}}
static sp_RbVal sp_PolyArray_sample(sp_PolyArray *a) { if (a->len <= 0) return sp_box_nil(); return a->data[sp_krand_below(a->len)]; }

/* Forward decl: sp_poly_inspect dispatches into sp_PolyArray_inspect
   for nested poly arrays (under promote, an `each_cons` chain's outer
   accumulator boxes each inner poly_array element), but the
   sp_PolyArray_inspect body lives a few lines below. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a);
static const char*sp_PolyArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_PolyArray_inspect((sp_PolyArray*)a->data[i]));}sp_String_append(s,"]");return sp_str_dup(s->data);}

/* Poly-key/value hash inspect helpers are defined after sp_poly_inspect
   (they call back into it for their elements), so forward-declare them
   here for the SP_TAG_OBJ hash arms below. The struct typedefs also live
   further down, so forward-declare those tags too. */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef struct sp_SymPolyHash sp_SymPolyHash;
typedef struct sp_PolyPolyHash sp_PolyPolyHash;
static const char *sp_StrPolyHash_inspect(sp_StrPolyHash *h);
static const char *sp_SymPolyHash_inspect(sp_SymPolyHash *h);
static const char *sp_PolyPolyHash_inspect(sp_PolyPolyHash *h);

/* Object#inspect for a tagged sp_RbVal. Dispatches on the runtime tag;
   each branch reuses the matching primitive inspect helper. Falls back
   to "#<Object>" for SP_TAG_OBJ because the runtime has no class-name
   table yet (follow-up PR). Returns a GC-managed C string. */
struct sp_OpenStruct_s;
static const char *sp_OpenStruct_inspect(struct sp_OpenStruct_s *o);
static inline const char *sp_poly_inspect(sp_RbVal v) {
  switch (v.tag) {
    /* An int-typed nil (unfilled int block param, nullable-int miss) carries
       the SP_INT_NIL sentinel; render it as nil, not the raw INT64_MIN. */
    case SP_TAG_INT:  return v.v.i == SP_INT_NIL ? SPL("nil") : sp_int_to_s(v.v.i);
    case SP_TAG_STR:  return sp_str_inspect(v.v.s);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL:  return SPL("nil");
    case SP_TAG_SYM:  return sp_sym_inspect((sp_sym)v.v.i);
    case SP_TAG_ENCODING: return sp_sprintf("#<Encoding:%s>", v.v.s ? v.v.s : "");
    case SP_TAG_CLASS: return sp_class_val_name(v);
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
 /* Built-in container / value-type tags get their typed inspect
    helper. Matches the dispatch shape in sp_poly_to_s above and the
    `puts` poly arm earlier in this file; without it, a Range / Time
    / typed Array stored as an sp_RbVal value (e.g. a sym_poly_hash
    that mixes `200..299` and `404`) reported "#<Object>" from
    `.inspect`, which was both wrong for CRuby parity and useless
    for debugging. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE:     return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_FLOAT_RANGE: return sp_frange_inspect(*(sp_FloatRange *)v.v.p);
        case SP_BUILTIN_TIME:      return sp_Time_inspect((sp_Time *)v.v.p);
      case SP_BUILTIN_STRBUF: return sp_str_inspect(sp_String_cstr((sp_String *)v.v.p));   /* (#3227) */
      case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
        case SP_BUILTIN_COMPLEX:   return sp_complex_inspect(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL:  return sp_rational_inspect(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_BIG_RATIONAL:  return sp_brat_inspect((sp_BigRational *)v.v.p);
        case SP_BUILTIN_REGEX:     return sp_re_inspect_str(v.v.p);
        case SP_BUILTIN_MATCHDATA: return sp_MatchData_inspect((sp_MatchData *)v.v.p);
        case SP_BUILTIN_EXCEPTION: return sp_sprintf("#<%s: %s>", sp_exc_class_name((volatile struct sp_Exception_s *)v.v.p), sp_exc_message((volatile struct sp_Exception_s *)v.v.p));
        case SP_BUILTIN_STR_INT_HASH:  return sp_StrIntHash_inspect((sp_StrIntHash *)v.v.p);
        case SP_BUILTIN_STR_STR_HASH:  return sp_StrStrHash_inspect((sp_StrStrHash *)v.v.p);
        case SP_BUILTIN_INT_STR_HASH:  return sp_IntStrHash_inspect((sp_IntStrHash *)v.v.p);
        case SP_BUILTIN_INT_INT_HASH:  return sp_IntIntHash_inspect((sp_IntIntHash *)v.v.p);
        case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_inspect((sp_StrPolyHash *)v.v.p);
        case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_inspect((sp_SymPolyHash *)v.v.p);
        case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_inspect((sp_PolyPolyHash *)v.v.p);
        case SP_BUILTIN_OPENSTRUCT: return sp_OpenStruct_inspect((struct sp_OpenStruct_s *)v.v.p);
        default:
          /* a user object: the generated per-class ivar walk renders
             #<Name:0x... @a=..., ...> like CRuby's default inspect */
          if (v.cls_id >= 0 && sp_obj_inspect_fn && v.v.p)
            return sp_obj_inspect_fn(v.cls_id, v.v.p);
          if ((v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT) && v.v.p)
            return sp_sprintf("#<%s:0x%016llx>", sp_poly_class_name(v),
                              (unsigned long long)(uintptr_t)v.v.p);
          /* a builtin handle with no inspect of its own (a Fiber, a Queue, a
             Mutex): name it rather than answering the useless "#<Object>" --
             the tag knows what it is, and CRuby's default inspect is this
             shape too */
          if (v.v.p) {
            const char *bn = sp_poly_class_name(v);
            if (bn && bn[0])
              return sp_sprintf("#<%s:0x%016llx>", bn, (unsigned long long)(uintptr_t)v.v.p);
          }
          return SPL("#<Object>");
      }
    default:          return sp_str_empty;
  }
}
/* FrozenError for a frozen user instance: "can't modify frozen <Name>: <inspect>".
   `what` carries the class-bearing prefix ("can't modify frozen C"), rodata
   marker-prefixed at the emit site; the receiver renders through the full
   poly inspect (per-class ivar walk). */
static void __attribute__((noinline,cold)) sp_raise_frozen_obj(sp_RbVal v, const char *what) {
  const char *ins = sp_poly_inspect(v);
  SP_GC_ROOT_STR(ins);
  const char *msg = sp_str_concat3(what, (&("\xff" ": ")[1]), ins);
  SP_GC_ROOT_STR(msg);
  sp_exc_stage_recv(v);   /* FrozenError#receiver = the frozen object (#3119) */
  sp_raise_cls("FrozenError", msg);
}
/* Raise Hash#fetch's KeyError with MRI's "key not found: <key.inspect>" text.
   Boxing the key lets one helper serve every key type (symbol, string, int,
   ...) via sp_poly_inspect. */
SP_NORETURN static void sp_raise_key_not_found(sp_RbVal key) {
  sp_exc_stage_key(key);
  sp_raise_cls("KeyError", sp_sprintf("key not found: %s", sp_poly_inspect(key)));
}
/* Array#inspect for heterogeneous poly arrays. Each element dispatches
   through sp_poly_inspect, so a mixed `[1, "x", :y]` renders
   `[1, "x", :y]` byte-for-byte identical to CRuby. NULL renders
   "nil" so callers that store a nil-returning slot (assoc/rassoc
   miss, etc.) round-trip cleanly through `.inspect`. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a) {
  if (!a) { char *r = sp_str_alloc(3); r[0] = 'n'; r[1] = 'i'; r[2] = 'l'; r[3] = 0; sp_str_set_len(r, 3); return r; }
  return sp_inspect_container(sp_box_poly_array(a));
}
/* Array#join for a mixed-element (poly) array: to_s each element via
   sp_poly_to_s and concatenate with sep. Mirrors sp_StrArray_join for
   the boxed-element case. */
static const char *sp_poly_join(sp_RbVal a, const char *sep);  /* mutual recursion below */
static const char *sp_PolyArray_join(sp_PolyArray *a, const char *sep) {
  if (!a) return sp_str_empty;
  SP_GC_ROOT(a); SP_GC_ROOT(sep);
  /* A run that contains itself has no finite text, so CRuby raises here rather
     than printing an ellipsis the way #inspect does. Nested arrays come back
     through sp_poly_join into this same function, so one check covers them. */
  if (sp_poly_recur_seen(SP_POLY_RECUR_JOIN, a, NULL))
    sp_poly_recur_raise(SP_POLY_RECUR_JOIN, "recursive array join");
  int rmark = sp_poly_recur_push(SP_POLY_RECUR_JOIN, a, NULL);
  sp_String *s = sp_String_new("");
  SP_GC_ROOT(s);
  for (sp_int i = 0; i < a->len; i++) {
    if (i > 0 && sep) sp_String_append_bin(s, sep);   /* byte-exact: a separator may hold a NUL */
    sp_RbVal e = a->data[i];
    /* a nested array joins recursively with the same separator (CRuby) */
    if (e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(e.cls_id))
      sp_String_append_bin(s, sp_poly_join(e, sep));
    else
      sp_String_append_bin(s, sp_poly_to_s(e));
  }
  /* Copy out of the sp_String builder: `s` is unrooted once this returns, so a
     later GC would sweep it and its finalizer free the fd-buffer, leaving a
     caller that stored the result (a StrArray element joined later) with a
     dangling pointer. The fd-buffer's 0xfd marker also makes sp_mark_string a
     no-op, so the buffer cannot be kept alive by marking it. Return a standalone
     heap string instead (#3151). */
  sp_poly_recur_pop(rmark);
  return sp_str_from_bytes(s->data, (size_t)s->len);
}
/* join on a boxed array (poly value holding any array kind) */
static const char *sp_poly_join(sp_RbVal a, const char *sep) {
  if (a.tag != SP_TAG_OBJ) return sp_poly_to_s(a);
  /* `.join` on a poly value is Array#join here, but a Thread carried in a poly
     slot (e.g. `threads.each(&:join)`, where the thread array boxed to poly)
     means Thread#join: run it to completion. Its result (the thread) is almost
     always discarded, so yield the empty string for the const char* surface. */
  if (a.cls_id == SP_BUILTIN_THREAD) { sp_Thread_join((sp_thread *)a.v.p); return sp_str_empty; }
  switch (a.cls_id) {
    case SP_BUILTIN_STR_ARRAY: return sp_StrArray_join((sp_StrArray *)a.v.p, sep);
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_join((sp_PolyArray *)a.v.p, sep);
    case SP_BUILTIN_INT_ARRAY: {
      sp_IntArray *ar = (sp_IntArray *)a.v.p;
      if (!ar || ar->len == 0) return sp_str_empty;
      sp_String *s = sp_String_new(""); SP_GC_ROOT(s);
      for (sp_int i = 0; i < ar->len; i++) {
        if (i > 0 && sep) sp_String_append(s, sep);
        sp_String_append(s, sp_int_to_s(ar->data[ar->start + i]));
      }
      return sp_str_from_bytes(s->data, (size_t)s->len);  /* standalone copy (#3151) */
    }
    default: return sp_poly_to_s(a);
  }
}
static sp_bool sp_PolyArray_eq(sp_PolyArray *a, sp_PolyArray *b) {
  if (!a || !b) return a == b;
  /* An array is == to itself in O(1), which also ends `x == x` for one that
     holds itself -- CRuby's rb_ary_equal starts the same way. */
  if (a == b) return TRUE;
  if (a->len != b->len) return FALSE;
  /* Reached directly from generated code as well as through sp_poly_eq, so it
     carries the pair guard itself: two distinct self-containing arrays walk
     into the same pair one level down, and a repeated pair is equal. */
  if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a, b)) return TRUE;
  int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a, b);
  sp_bool r = TRUE;
  for (sp_int i = 0; r && i < a->len; i++) r = sp_poly_eq(a->data[i], b->data[i]);
  sp_poly_recur_pop(mark);
  return r;
}
/* Box a typed (int/str/float) array into a fresh poly array element-wise.
   `kind` is the typed array's SP_BUILTIN_* tag. */
static sp_PolyArray *sp_typed_to_poly(void *tp, int kind) {
  sp_PolyArray *tb = sp_PolyArray_new();
  if (!tp) return tb;
  if (kind == SP_BUILTIN_STR_ARRAY) {
    sp_StrArray *a = (sp_StrArray *)tp;
    for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_str(sp_StrArray_get(a, i)));
  }
  else if (kind == SP_BUILTIN_FLT_ARRAY) {
    sp_FloatArray *a = (sp_FloatArray *)tp;
    for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_float(sp_FloatArray_get(a, i)));
  }
  else {
    sp_IntArray *a = (sp_IntArray *)tp;
    for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_int(sp_IntArray_get(a, i)));
  }
  return tb;
}
/* Compare a poly array against a typed (int/str/float) array by boxing the
   typed side element-wise. `kind` is the typed array's SP_BUILTIN_* tag. */
static sp_bool sp_PolyArray_eq_typed(sp_PolyArray *pa, void *tp, int kind) {
  if (!pa || !tp) return FALSE;
  SP_GC_ROOT(pa); SP_GC_ROOT(tp);  /* sp_typed_to_poly allocates */
  return sp_PolyArray_eq(pa, sp_typed_to_poly(tp, kind));
}
static sp_bool sp_PolyArray_include(sp_PolyArray *a, sp_RbVal v) {
  if (!a) return FALSE;
  for (sp_int i = 0; i < a->len; i++) {
    if (sp_poly_eq(a->data[i], v)) return TRUE;
  }
  return FALSE;
}

/* Mark the embedded GC reference inside an sp_RbVal (string or obj).
   Used as the scan hook for containers that store polymorphic values. */
/* sp_mark_rbval is an inline helper in sp_gc.h. */

/* StrPolyHash: string keys, sp_RbVal values — for hashes with mixed value types. */
/* `dproc` holds a Hash.new{} default block, lowered to a dedicated C
   fn `sp_RbVal (*)(sp_StrPolyHash *self, const char *key)` with typed
   params (codegen emits it). Called by _get on a miss. Issue #912. */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef sp_RbVal (*sp_strpoly_dproc_t)(sp_StrPolyHash *, const char *, void *);
struct sp_StrPolyHash{const char**keys;sp_RbVal*vals;const char**order;sp_int len;sp_int cap;sp_int mask;sp_RbVal default_v;sp_strpoly_dproc_t dproc;void *dproc_self;};
static void sp_StrPolyHash_fin(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrPolyHash_scan(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;for(sp_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);if(h->dproc_self)sp_gc_mark(h->dproc_self);}
static sp_StrPolyHash*sp_StrPolyHash_new(void){sp_StrPolyHash*h=(sp_StrPolyHash*)sp_gc_alloc(sizeof(sp_StrPolyHash),sp_StrPolyHash_fin,sp_StrPolyHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc((size_t)h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_with_default(sp_RbVal d){sp_StrPolyHash*h=sp_StrPolyHash_new();h->default_v=d;return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_dproc(sp_strpoly_dproc_t fn,void*self){sp_StrPolyHash*h=sp_StrPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
static void sp_StrPolyHash_grow(sp_StrPolyHash*h){ sp_gc_wb((void*)h);sp_int oc=h->cap;const char**ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc((size_t)h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(sp_int i=0;i<oc;i++){if(ok[i]){sp_int idx=(sp_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_StrPolyHash_get(sp_StrPolyHash*h,const char*k){if(!h)return sp_box_nil();sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static void sp_StrPolyHash_set(sp_StrPolyHash*h,const char*k,sp_RbVal v){sp_gc_wb((void*)h); if(!k){sp_raise_cls("TypeError","no implicit conversion of nil into String");return;} if(h->len*2>=h->cap)sp_StrPolyHash_grow(h);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static sp_bool sp_StrPolyHash_has_key(sp_StrPolyHash*h,const char*k){if(!h)return FALSE;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static sp_int sp_StrPolyHash_length(sp_StrPolyHash*h){return h->len;}
static sp_StrArray*sp_StrPolyHash_keys(sp_StrPolyHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);if(!h)return a;for(sp_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_PolyArray*sp_StrPolyHash_values(sp_StrPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_StrPolyHash_get(h,h->order[i]));return a;}
static sp_bool sp_StrPolyHash_has_value(sp_StrPolyHash*h,sp_RbVal v){if(!h)return FALSE;for(sp_int i=0;i<h->len;i++)if(sp_poly_eq(sp_StrPolyHash_get(h,h->order[i]),v))return TRUE;return FALSE;}
static void sp_StrPolyHash_delete(sp_StrPolyHash*h,const char*k){ sp_gc_wb((void*)h);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=sp_box_nil();h->len--;sp_int j=(idx+1)&h->mask;while(h->keys[j]){sp_int nj=(sp_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=sp_box_nil();idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
/* Hash#merge for str_poly_hash. Same shape as the
   StrIntHash / SymPolyHash siblings -- copy recv's entries into a
   fresh hash, then overlay other's. */
static sp_StrPolyHash*sp_StrPolyHash_merge(sp_StrPolyHash*a,sp_StrPolyHash*b){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++)sp_StrPolyHash_set(r,a->order[i],sp_StrPolyHash_get(a,a->order[i]));for(sp_int i=0;i<b->len;i++)sp_StrPolyHash_set(r,b->order[i],sp_StrPolyHash_get(b,b->order[i]));return r;}
static sp_StrPolyHash*sp_StrPolyHash_dup(sp_StrPolyHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_StrPolyHash_set(r,h->order[i],sp_StrPolyHash_get(h,h->order[i]));return r;}
static sp_StrPolyHash*sp_StrPolyHash_replace(sp_StrPolyHash*h,sp_StrPolyHash*o){ sp_gc_wb((void*)h);if(!h)return h;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_StrPolyHash_set(h,o->order[i],sp_StrPolyHash_get(o,o->order[i]));return h;}
static void sp_StrPolyHash_clear(sp_StrPolyHash*h){ sp_gc_wb((void*)h);if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}
static sp_bool sp_StrPolyHash_eq(sp_StrPolyHash*a,sp_StrPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_StrPolyHash_get(a,k),sp_StrPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Issue #851: inspect for str_poly_hash. */
static const char*sp_StrPolyHash_inspect(sp_StrPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_POLY_HASH)):SPL("nil");}
/* Convert a narrower StrStrHash to a StrPolyHash. Needed when the
   analyzer widens an LV slot to sp_StrPolyHash* (e.g. later poly-value
   writes) but the initial RHS is a sibling narrower hash variant —
   raw pointer assignment would mix incompatible struct layouts
   (vals[] of const char** vs sp_RbVal*). See issue #614. */
static sp_StrPolyHash*sp_StrPolyHash_from_str_str_hash(sp_StrStrHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;if(h->default_v)r->default_v=sp_box_str(h->default_v);for(sp_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_str(sp_StrStrHash_get(h,k)));}return r;}
/* MatchData#named_captures: {String name => group substring | nil}. A
   non-participating named group maps to nil, so the value side is poly. Lives
   here (not sp_re.c) because the typed-hash machinery is TU-coupled. */
static sp_StrPolyHash *sp_md_named_captures(sp_MatchData *m) {
  sp_StrPolyHash *h = sp_StrPolyHash_new();
  if (!m) return h;
  SP_GC_ROOT(h);      /* sp_str_dup and the aref below allocate */
  int n = re_num_named(m->pat);
  for (int i = 0; i < n; i++) {
    int g = -1;
    const char *nm = re_named_name(m->pat, i, &g);
    if (nm) sp_StrPolyHash_set(h, sp_str_dup(nm), sp_box_nullable_str(sp_MatchData_aref(m, g)));
  }
  return h;
}
/* MatchData#match_length(n): the byte length of group n's match, or nil when
   the group did not participate (#2501). */
static sp_RbVal sp_MatchData_match_length(sp_MatchData *m, sp_int i) {
  if (!m) return sp_box_nil();
  if (i < 0) i += m->ncap;
  if (i < 0 || i >= m->ncap || m->caps[i * 2] < 0) return sp_box_nil();
  return sp_box_int(m->caps[i * 2 + 1] - m->caps[i * 2]);
}
/* #match / #match_length given a group NAME: resolve it through the pattern
   rather than reading the symbol's id as an index (#3630). */
static sp_RbVal sp_MatchData_match_length_name(sp_MatchData *m, const char *name) {
  if (!m || !name) return sp_box_nil();
  int g = re_named_group(m->pat, name);
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %s", name));
  return sp_MatchData_match_length(m, g);
}
static sp_StrPolyHash*sp_StrPolyHash_from_str_int_hash(sp_StrIntHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;r->default_v=sp_box_int(h->default_v);for(sp_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_int(sp_StrIntHash_get(h,k)));}return r;}

/* SymPolyHash: symbol keys, sp_RbVal values — same shape as SymStrHash but with poly values. */
/* Named struct so lib/sp_fiber.c can forward-declare it for sp_Fiber's
   `storage` member without pulling in the full poly-hash machinery. */
/* dproc holds a Hash.new{} default block (symbol-keyed), lowered to a C fn
   with a typed sp_sym key param. Called by _get on a miss. */
typedef sp_RbVal (*sp_sympoly_dproc_t)(sp_SymPolyHash *, sp_sym, void *);
typedef struct sp_SymPolyHash{sp_sym*keys;sp_RbVal*vals;sp_sym*order;sp_int len;sp_int cap;sp_int mask;sp_RbVal default_v;sp_sympoly_dproc_t dproc;void *dproc_self;}sp_SymPolyHash;
static void sp_SymPolyHash_fin(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_SymPolyHash_scan(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;for(sp_int i=0;i<h->cap;i++){if(h->keys[i]>=0)sp_mark_rbval(h->vals[i]);}sp_mark_rbval(h->default_v);if(h->dproc_self)sp_gc_mark(h->dproc_self);}
static sp_SymPolyHash*sp_SymPolyHash_new(void){sp_SymPolyHash*h=(sp_SymPolyHash*)sp_gc_alloc(sizeof(sp_SymPolyHash),sp_SymPolyHash_fin,sp_SymPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*(size_t)h->cap);for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)malloc(sizeof(sp_sym)*(size_t)h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_SymPolyHash*sp_SymPolyHash_new_with_default(sp_RbVal d){sp_SymPolyHash*h=sp_SymPolyHash_new();h->default_v=d;return h;}
static sp_SymPolyHash*sp_SymPolyHash_new_dproc(sp_sympoly_dproc_t fn,void*self){sp_SymPolyHash*h=sp_SymPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
static void sp_SymPolyHash_grow(sp_SymPolyHash*h){ sp_gc_wb((void*)h);sp_int oc=h->cap;sp_sym*ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;if(h->cap<=0||h->cap>((sp_int)1<<40))sp_oom_die();h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*(size_t)h->cap);for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*(size_t)h->cap);h->len=0;for(sp_int i=0;i<oc;i++){if(ok[i]>=0){sp_int idx=(sp_int)(((sp_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
/* miss path split out cold+noinline: the dproc check must not sit inline in
   _get -- the extra branch/code pushed the hot inlined lookup over the inline
   threshold and cost optcarrot ~35% fps (same lesson as the string-hash cache:
   hot-path inline branches regress; SP_NOINLINE the cold side). */
static SP_NOINLINE sp_RbVal sp_SymPolyHash_miss(sp_SymPolyHash*h,sp_sym k){if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static sp_RbVal sp_SymPolyHash_get(sp_SymPolyHash*h,sp_sym k){if(!h)return sp_box_nil();sp_int idx=(sp_int)(((sp_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return sp_SymPolyHash_miss(h,k);}
static void sp_SymPolyHash_set(sp_SymPolyHash*h,sp_sym k,sp_RbVal v){sp_gc_wb((void*)h); if(h->len*2>=h->cap)sp_SymPolyHash_grow(h);sp_int idx=(sp_int)(((sp_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static sp_bool sp_SymPolyHash_has_key(sp_SymPolyHash*h,sp_sym k){sp_int idx=(sp_int)(((sp_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static sp_int sp_SymPolyHash_length(sp_SymPolyHash*h){return h->len;}
static sp_IntArray*sp_SymPolyHash_keys(sp_SymPolyHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_IntArray_push(a,(sp_int)h->order[i]);return a;}
static sp_PolyArray*sp_SymPolyHash_values(sp_SymPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_SymPolyHash_get(h,h->order[i]));return a;}
static sp_bool sp_SymPolyHash_has_value(sp_SymPolyHash*h,sp_RbVal v){if(!h)return FALSE;for(sp_int i=0;i<h->len;i++)if(sp_poly_eq(sp_SymPolyHash_get(h,h->order[i]),v))return TRUE;return FALSE;}
static sp_sym sp_SymPolyHash_key(sp_SymPolyHash*h,sp_RbVal v){if(!h)return (sp_sym)-1;for(sp_int i=0;i<h->len;i++)if(sp_poly_eq(sp_SymPolyHash_get(h,h->order[i]),v))return h->order[i];return (sp_sym)-1;}
static sp_SymPolyHash*sp_SymPolyHash_merge(sp_SymPolyHash*a,sp_SymPolyHash*b){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++)sp_SymPolyHash_set(r,a->order[i],sp_SymPolyHash_get(a,a->order[i]));for(sp_int i=0;i<b->len;i++)sp_SymPolyHash_set(r,b->order[i],sp_SymPolyHash_get(b,b->order[i]));return r;}
static void sp_SymPolyHash_update(sp_SymPolyHash*a,sp_SymPolyHash*b){if(!a||!b||a==b)return;SP_GC_ROOT(a);SP_GC_ROOT(b);for(sp_int i=0;i<b->len;i++)sp_SymPolyHash_set(a,b->order[i],sp_SymPolyHash_get(b,b->order[i]));}
/* OpenStruct: a dynamic-member object (#3135). Members are named at run time
   (JSON keys, CLI args, ...) so they cannot be static C fields; the backing is
   a SymPolyHash of symbol -> boxed value. Reads of an absent member are nil,
   writes create the member, and to_h / [] / respond_to? / == all read the
   hash. It is boxed as SP_BUILTIN_OPENSTRUCT and never masquerades as a
   Struct -- a distinct dynamic type outside static field access. */
typedef struct sp_OpenStruct_s { sp_SymPolyHash *tbl; } sp_OpenStruct;
static void sp_OpenStruct_scan(void *p){ sp_OpenStruct *o=(sp_OpenStruct*)p; if(o->tbl) sp_gc_mark(o->tbl); }
static sp_OpenStruct *sp_OpenStruct_new(void){
  sp_OpenStruct *o=(sp_OpenStruct*)sp_gc_alloc(sizeof(sp_OpenStruct),NULL,sp_OpenStruct_scan);
  o->tbl=sp_SymPolyHash_new(); return o;
}
static sp_RbVal sp_OpenStruct_get(sp_OpenStruct *o, sp_sym k){
  if(!o||!o->tbl||!sp_SymPolyHash_has_key(o->tbl,k)) return sp_box_nil();
  return sp_SymPolyHash_get(o->tbl,k);
}
static void sp_OpenStruct_set(sp_OpenStruct *o, sp_sym k, sp_RbVal v){
  if(o&&sp_gc_is_frozen(o)) sp_raise_cls("FrozenError","can't modify frozen OpenStruct");
  if(o&&o->tbl) sp_SymPolyHash_set(o->tbl,k,v);
}
static sp_bool sp_OpenStruct_has(sp_OpenStruct *o, sp_sym k){
  return o&&o->tbl&&sp_SymPolyHash_has_key(o->tbl,k);
}
/* new(a: 1, b: 2): fill from a keyword/symbol-keyed hash literal built by the
   emit. The hash's ownership transfers -- OpenStruct wraps it directly. */
static sp_OpenStruct *sp_OpenStruct_new_from(sp_SymPolyHash *h){
  sp_OpenStruct *o=(sp_OpenStruct*)sp_gc_alloc(sizeof(sp_OpenStruct),NULL,sp_OpenStruct_scan);
  o->tbl=h?h:sp_SymPolyHash_new(); return o;
}
static sp_SymPolyHash *sp_OpenStruct_to_h(sp_OpenStruct *o){
  /* a fresh copy so mutating the returned hash does not alter the object */
  sp_SymPolyHash *r=sp_SymPolyHash_new(); SP_GC_ROOT(r);
  if(o&&o->tbl) for(sp_int i=0;i<o->tbl->len;i++){ sp_sym k=o->tbl->order[i]; sp_SymPolyHash_set(r,k,sp_SymPolyHash_get(o->tbl,k)); }
  return r;
}
static sp_bool sp_OpenStruct_eq(sp_OpenStruct *a, sp_OpenStruct *b){
  if(a==b) return 1;
  if(!a||!b||!a->tbl||!b->tbl) return 0;
  if(a->tbl->len!=b->tbl->len) return 0;
  for(sp_int i=0;i<a->tbl->len;i++){ sp_sym k=a->tbl->order[i];
    if(!sp_SymPolyHash_has_key(b->tbl,k)) return 0;
    if(!sp_poly_eq(sp_SymPolyHash_get(a->tbl,k), sp_SymPolyHash_get(b->tbl,k))) return 0; }
  return 1;
}
/* OpenStruct#eql? is the member table's eql?: class-strict per member, so
   OpenStruct.new(x: 1) is == but not eql? to OpenStruct.new(x: 1.0). */
static sp_bool sp_OpenStruct_eql(sp_OpenStruct *a, sp_OpenStruct *b){
  if(a==b) return 1;
  if(!a||!b||!a->tbl||!b->tbl) return 0;
  if(a->tbl->len!=b->tbl->len) return 0;
  for(sp_int i=0;i<a->tbl->len;i++){ sp_sym k=a->tbl->order[i];
    if(!sp_SymPolyHash_has_key(b->tbl,k)) return 0;
    if(!sp_poly_eql(sp_SymPolyHash_get(a->tbl,k), sp_SymPolyHash_get(b->tbl,k))) return 0; }
  return 1;
}
/* #inspect: #<OpenStruct a=1, b="hi"> */
static const char *sp_OpenStruct_inspect(sp_OpenStruct *o){
  sp_String *s=sp_String_new(""); SP_GC_ROOT(s);
  sp_String_append(s,"#<OpenStruct");
  if(o&&o->tbl) for(sp_int i=0;i<o->tbl->len;i++){
    sp_sym k=o->tbl->order[i];
    sp_String_append(s, i==0?" ":", ");
    sp_String_append(s, sp_sym_to_s(k));
    sp_String_append(s, "=");
    sp_String_append(s, sp_poly_inspect(sp_SymPolyHash_get(o->tbl,k)));
  }
  sp_String_append(s,">");
  /* an independent GC heap string, not the sp_String's own buffer: the wrapper
     is unrooted on return and its data would dangle if the result is stored. */
  return sp_str_concat(sp_String_cstr(s), (&("\xff")[1]));
}
/* MatchData#named_captures(symbolize_names: true) and #deconstruct_keys: the
   named captures as a symbol-keyed hash (#2503, #2530). */
#if defined(SPINEL_EXT_HOST) || defined(SPINEL_EXT_KERNEL)
sp_sym sp_sym_intern(const char *s);
#else
static sp_sym sp_sym_intern(const char *s);
#endif
static sp_SymPolyHash *sp_md_named_captures_sym(sp_MatchData *m) {
  sp_SymPolyHash *h = sp_SymPolyHash_new();
  if (!m) return h;
  SP_GC_ROOT(h);      /* sp_sym_intern and the aref below allocate */
  int n = re_num_named(m->pat);
  for (int i = 0; i < n; i++) {
    int g = -1;
    const char *nm = re_named_name(m->pat, i, &g);
    if (nm) sp_SymPolyHash_set(h, sp_sym_intern(nm), sp_box_nullable_str(sp_MatchData_aref(m, g)));
  }
  return h;
}
/* MatchData#deconstruct_keys(keys): nil returns all named captures; an array
   returns just the requested keys (in order, missing ones skipped), but the
   empty hash right away when more keys are asked for than exist (#3015). */
static sp_SymPolyHash *sp_md_deconstruct_keys(sp_MatchData *m, sp_RbVal keys) {
  if (keys.tag == SP_TAG_NIL) return sp_md_named_captures_sym(m);
  /* only an Array of Symbols selects keys; anything else is a TypeError (#3643) */
  if (!(keys.tag == SP_TAG_OBJ && sp_poly_is_array_kind(keys.cls_id)))
    sp_raise_cls("TypeError", sp_sprintf("wrong argument type %s (expected Array or nil)",
                                         sp_poly_class_name(keys)));
  sp_SymPolyHash *h = sp_SymPolyHash_new();
  if (!m) return h;
  SP_GC_ROOT(h);
  int nnamed = re_num_named(m->pat);
  sp_int klen = sp_poly_length(keys);
  for (sp_int i = 0; i < klen; i++) {
    sp_RbVal ck = sp_poly_arr_get(keys, i);
    if (ck.tag != SP_TAG_SYM)
      sp_raise_cls("TypeError", sp_sprintf("wrong argument type %s (expected Symbol)",
                                           sp_poly_class_name(ck)));
  }
  if (klen > nnamed) return h;
  for (sp_int i = 0; i < klen; i++) {
    sp_RbVal k = sp_poly_arr_get(keys, i);
    if (k.tag != SP_TAG_SYM) continue;
    const char *kn = sp_sym_to_s((sp_sym)k.v.i);
    for (int j = 0; j < nnamed; j++) {
      int g = -1;
      const char *nm = re_named_name(m->pat, j, &g);
      if (nm && strcmp(nm, kn) == 0) {
        sp_SymPolyHash_set(h, (sp_sym)k.v.i, sp_box_nullable_str(sp_MatchData_aref(m, g)));
        break;
      }
    }
  }
  return h;
}
/* A `**hash` forwarded into a method with fixed keyword params (and no
   keyword-rest to absorb extras) must carry only declared keys; CRuby raises
   ArgumentError otherwise. `allowed` is a NULL-terminated array of the callee's
   keyword names, in declaration order. The message matches CRuby:
   `unknown keyword: :x` for one, `unknown keywords: :x, :y` for several. */
static void sp_kwargs_check(sp_SymPolyHash *h, const char *const *allowed) {
  if (!h) return;
  char list[256]; int n = 0, cnt = 0;
  for (sp_int i = 0; i < h->len; i++) {
    const char *nm = sp_sym_to_s(h->order[i]);
    int ok = 0;
    for (const char *const *a = allowed; *a; a++) if (!strcmp(nm, *a)) { ok = 1; break; }
    if (ok) continue;
    if (n < (int)sizeof(list) - 1) {
      int w = snprintf(list + n, sizeof(list) - n, "%s:%s", cnt ? ", " : "", nm);
      /* snprintf returns the untruncated length; clamp so n never runs past the
         buffer and sizeof(list)-n stays a valid size on the next iteration. */
      n = (w > 0 && n + w < (int)sizeof(list)) ? n + w : (int)sizeof(list) - 1;
    }
    cnt++;
  }
  if (cnt == 0) return;
  sp_raise_cls("ArgumentError", sp_sprintf("unknown keyword%s: %s", cnt > 1 ? "s" : "", list));
}
/* Hash#delete for sym_poly_hash. Removes key and re-tombstones the
   slot, shifting probe-chain successors backward and dropping the
   key from the insertion-order list. Issue #510. */
static void sp_SymPolyHash_delete(sp_SymPolyHash*h,sp_sym k){ sp_gc_wb((void*)h);sp_int idx=(sp_int)(((sp_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->keys[idx]=-1;h->vals[idx]=sp_box_nil();h->len--;sp_int j=(idx+1)&h->mask;while(h->keys[j]>=0){sp_int nj=(sp_int)(((sp_int)h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=-1;h->vals[j]=sp_box_nil();idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(h->order[oi]==k){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_SymPolyHash*sp_SymPolyHash_dup(sp_SymPolyHash*h){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_SymPolyHash_set(r,h->order[i],sp_SymPolyHash_get(h,h->order[i]));return r;}
static sp_SymPolyHash*sp_SymPolyHash_replace(sp_SymPolyHash*h,sp_SymPolyHash*o){if(!h)return h;for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_SymPolyHash_set(h,o->order[i],sp_SymPolyHash_get(o,o->order[i]));return h;}
static void sp_SymPolyHash_clear(sp_SymPolyHash*h){if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;}
static sp_bool sp_SymPolyHash_eq(sp_SymPolyHash*a,sp_SymPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){sp_sym k=a->order[i];if(!sp_SymPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_SymPolyHash_get(a,k),sp_SymPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Hash#inspect for sym_poly_hash. CRuby 4.0 renders symbol keys
   in shorthand: `{a: 1, b: "x"}` rather than `{:a => 1, :b => "x"}`. */
static const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_SYM_POLY_HASH)):SPL("nil");}

/* poly_val[sym_key]: runtime dispatch for poly receiver `[]` with symbol arg. */
/* sp_poly_get_sym moved below PolyPolyHash so it can dispatch to it. */
/* poly_val[str_key]: runtime dispatch for poly receiver `[]` with string arg.
   Defined after PolyPolyHash. */
/* PolyPolyHash: heterogeneous keys + values (both sp_RbVal). For
   primitives the hash/eql is tag-based (value equality); for OBJ tag
   the default is pointer identity. Codegen patches sp_obj_hash_hook /
   sp_obj_eql_hook with class-aware overrides (e.g. Method#eql? compares
   bound receiver + fn_ptr) when the program needs them — null hooks
   leave the runtime at identity, which is the right default for typed
   pointers like IntArray. */
/* FNV-1a over a fixed byte range -- gives value-type user objects (by-value
   structs with no pointer identity) a stable Integer #hash / #object_id from
   their content, so `v.hash == v.hash` holds (#2284, #2283). */
/* Integer#<< with a RUNTIME shift amount: a negative count shifts right
   (floor semantics via arithmetic shift), a count past the word raises
   instead of C undefined behavior (#2423). */
static inline sp_int sp_int_shl_ck(sp_int a, sp_int b) {
  if ((uint64_t)b < 63u) return a << b;   /* the hot, predictable path */
  if (b < 0) return (b <= -63) ? (a < 0 ? -1 : 0) : (a >> (-b));
  sp_raise_cls("RangeError", "shift width too big for a 64-bit Integer (use --int-overflow=promote)");
  return 0;
}
static inline sp_int sp_int_shr_ck(sp_int a, sp_int b) {
  if ((uint64_t)b < 63u) return a >> b;
  if (b < 0) return sp_int_shl_ck(a, -b);
  return a < 0 ? -1 : 0;                  /* >>63.. : sign fill */
}
static sp_int sp_bytes_hash(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
  return (sp_int)(h & 0x7fffffffffffffffULL);
}
typedef sp_int  (*sp_obj_hash_fn)(int cls_id, void *p);
typedef sp_bool (*sp_obj_eql_fn)(int cls_id, void *a, void *b);
static sp_obj_hash_fn sp_obj_hash_hook = NULL;
static sp_obj_eql_fn  sp_obj_eql_hook  = NULL;
/* The bucket index takes the LOW bits of a key's hash, and several key kinds
   leave their information out of those: a Float's mantissa bits are zero for
   every whole number, an array of small coordinates folds to h*31+x, a Struct
   folds its fields the same way. Those keys then land on a few slots however
   large the table grows and the probe sequences collapse into linear scans (a
   40k-bucket group_by ran for minutes, #3984). Mixing at the INDEX, not in the
   key hash, keeps every #hash value as it was. */
/* CRuby's rb_hash_start mixes a per-TYPE seed into every container hash, which
   is what keeps `{}.hash` away from `[].hash` -- both are empty and both would
   otherwise fold to their length, 0. Odd, so a container never folds a member's
   hash back onto the seed itself. */
#define SP_HASH_SEED_ARRAY ((uint64_t)0x5d2a3f1b9c4e7a61ULL)
#define SP_HASH_SEED_HASH  ((uint64_t)0x2c1b8f3d6e5a9047ULL)
static sp_int sp_hash_slot(sp_int hk) {
  uint64_t h = (uint64_t)hk;
  h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (sp_int)h;
}
static sp_int sp_rbval_hash_key(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return (sp_int)v.v.i;
    case SP_TAG_BIGINT:
      return (sp_int)sp_bigint_to_int((sp_Bigint *)v.v.p);
    case SP_TAG_STR:
      return v.v.s ? (sp_int)sp_str_hash(v.v.s) : 0;
    case SP_TAG_ENCODING:
      return v.v.s ? (sp_int)sp_str_hash(v.v.s) : 0;
    /* A Class value keys by NAME, the way `==` on two of them compares: a
       class box carries either a cls_id or a name, so hashing the box itself
       made `h[Integer] = 1; h[Integer] = 2` two entries (and `group_by(&:class)`
       one bucket per element). */
    case SP_TAG_CLASS: {
      const char *cn = sp_class_val_name(v);
      return cn ? (sp_int)sp_str_hash(cn) : 0;
    }
    /* -0.0 and 0.0 are eql?, so they must hash alike: normalize the sign of
       zero away before hashing the bits (#3651). */
    case SP_TAG_FLT: { double f = v.v.f == 0.0 ? 0.0 : v.v.f;
                       uint64_t b; memcpy(&b, &f, sizeof(b)); return (sp_int)b; }
    case SP_TAG_OBJ:
      /* A shared-string handle is `==` to the immediate string with the same
         bytes, so it has to hash alike or a Hash keyed by one misses the
         other -- the same rule as the array kinds just below (#4279). */
      if (sp_poly_is_strbuf(v)) { const char *hs = sp_poly_to_s(v);
                                  return hs ? (sp_int)sp_str_hash(hs) : 0; }
      /* Arrays hash by value across storage kinds: an IntArray [0, 0] and a
         PolyArray [0, 0] (e.g. one built by Array#product) are `==` and must
         hash alike to collide in a Hash, so hash each element through
         sp_rbval_hash_key rather than the raw IntArray words (#2911). */
      if (sp_poly_is_array_kind(v.cls_id)) {
        /* unsigned accumulator: the rolling h*31+x is meant to wrap, but on a
           signed type that is UB rather than wraparound -- and this is a
           static inline shared between the archive and every generated TU, so
           two copies compiled under different flags could disagree and a Hash
           lookup would silently miss. Same rule as the Rational key below. */
        sp_int n = sp_poly_length(v);
        /* A container reached from inside itself contributes a fixed value and
           stops, the way CRuby's rb_exec_recursive_outer answers. It replaces a
           depth counter that gave up on the WHOLE walk once it was 24 deep, so
           `a << a` hashed like [] at every level. */
        if (sp_poly_recur_seen(SP_POLY_RECUR_HASH, v.v.p, NULL)) return SP_POLY_RECUR_HASH_VALUE;
        /* Seeded with the length (CRuby's rb_hash_start(len)) rather than 0:
           from 0 the rolling h*31+x gave [], [0] and [nil] one hash, and it is
           the length that keeps `a << a` apart from []. The salt is CRuby's
           per-type seed: without one an empty Array and an empty Hash both
           start (and end) at 0 and hash alike. Both array kinds take the SAME
           salt, so an IntArray [0, 0] still collides with the PolyArray the
           same numbers were rebuilt as. */
        uint64_t h = (uint64_t)n ^ SP_HASH_SEED_ARRAY;
        int mark = sp_poly_recur_push(SP_POLY_RECUR_HASH, v.v.p, NULL);
        for (sp_int i = 0; i < n; i++)
          h = (h * 31) + (uint64_t)sp_rbval_hash_key(sp_poly_arr_get(v, i));
        sp_poly_recur_pop(mark);
        return (sp_int)h;
      }
      if (sp_poly_is_hash_kind(v.cls_id)) {
        /* Hashes hash by content, and Hash#hash does not depend on insertion
           order, so the per-entry terms are combined with a commutative
           operation: two `==` hashes must agree, whatever order they were
           built in. Unsigned for the same reason the array accumulator is. */
        sp_int n = sp_poly_length(v);
        if (sp_poly_recur_seen(SP_POLY_RECUR_HASH, v.v.p, NULL)) return SP_POLY_RECUR_HASH_VALUE;
        uint64_t h = (uint64_t)n ^ SP_HASH_SEED_HASH;  /* length + salt, as the array above */
        int mark = sp_poly_recur_push(SP_POLY_RECUR_HASH, v.v.p, NULL);
        for (sp_int i = 0; i < n; i++) {
          sp_RbVal k, val;
          sp_poly_hash_pair(v, i, &k, &val);
          /* Mix each pair before combining: the combination has to be
             commutative (Hash#hash ignores insertion order), and a plain
             xor-then-sum let {a: 2, b: 2} and {a: 7, b: 7} collide. */
          uint64_t t = ((uint64_t)sp_rbval_hash_key(k) * 0x9E3779B97F4A7C15ULL) +
                       (uint64_t)sp_rbval_hash_key(val);
          t ^= t >> 33; t *= 0xff51afd7ed558ccdULL; t ^= t >> 33;
          h += t;
        }
        sp_poly_recur_pop(mark);
        return (sp_int)h;
      }
      /* two patterns with the same source are one Hash key, whichever way each
         was built (a literal or Regexp.new) (#3681) */
      if (v.cls_id == SP_BUILTIN_REGEX)
        return v.v.p ? (sp_int)sp_str_hash(sp_re_source(v.v.p)) : 0;
      if (v.cls_id == SP_BUILTIN_METHOD) {
        /* Method keys hash/eql by (bound self, fn ptr, name), so two
           `obj.method(:m)` instances collapse to one entry (optcarrot
           @peeks/@pokes dedup). The name disambiguates class methods, whose
           fn slot is 0 (no resolvable callable address). */
        sp_BoundMethod *m = (sp_BoundMethod *)v.v.p;
        if (!m) return 0;
        return (sp_int)(((uintptr_t)m->self * 31) + m->fn) +
               (m->name ? (sp_int)sp_str_hash(m->name) : 0);
      }
      if (v.cls_id == SP_BUILTIN_RATIONAL) {
        /* value-based so equal Rationals (reduced to lowest terms) hash alike
           and can serve as Hash keys; a fresh box otherwise hashes by pointer. */
        sp_Rational *r = (sp_Rational *)v.v.p;
        /* unsigned arithmetic: signed sp_int multiply/add could overflow (UB) */
        return r ? (sp_int)(((uintptr_t)r->num * 31) + (uintptr_t)r->den) : 0;
      }
      if (v.cls_id == SP_BUILTIN_TIME) {
        /* value-based so equal instants hash alike (Time#== compares the
           instant), serving both Time#hash and Time-keyed Hash buckets. */
        sp_Time *t = (sp_Time *)v.v.p;
        return t ? (sp_int)sp_time_hash(*t) : 0;
      }
      if (v.cls_id == SP_BUILTIN_RANGE) {
        /* value-based: two Ranges with the same bounds are one Hash key, and a
           fresh box per lookup hashed by pointer and never found it (#3669) */
        sp_Range *rg = (sp_Range *)v.v.p;
        return rg ? (sp_int)((((uintptr_t)rg->first * 31u) + (uintptr_t)rg->last) * 2u
                              + (uintptr_t)(rg->excl ? 1 : 0)) : 0;
      }
      if (v.cls_id == SP_BUILTIN_STR_RANGE) {
        /* a String or Float Range is a key by its bounds too: only the int
           Range had a value hash, so ("a".."c") never found its own entry */
        sp_StrRange *sr = (sp_StrRange *)v.v.p;
        if (!sr) return 0;
        uint64_t h = sr->first ? sp_str_hash(sr->first) : 0;
        h = h * 31u + (sr->last ? sp_str_hash(sr->last) : 0);
        return (sp_int)(h * 2u + (uint64_t)(sr->excl ? 1 : 0));
      }
      if (v.cls_id == SP_BUILTIN_FLOAT_RANGE) {
        sp_FloatRange *fr = (sp_FloatRange *)v.v.p;
        if (!fr) return 0;
        uint64_t bf, bl;
        double f = fr->first == 0.0 ? 0.0 : fr->first, l = fr->last == 0.0 ? 0.0 : fr->last;
        memcpy(&bf, &f, sizeof bf); memcpy(&bl, &l, sizeof bl);
        return (sp_int)(((bf * 31u) ^ bl) * 2u + (uint64_t)(fr->excl ? 1 : 0));
      }
      if (v.cls_id == SP_BUILTIN_COMPLEX) {
        /* value-based: a fresh box per .hash call would hash by pointer and
           break a.hash == a.hash (unsigned mixing avoids signed overflow) */
        sp_Complex *cx = (sp_Complex *)v.v.p;
        if (!cx) return 0;
        uint64_t br, bi;
        memcpy(&br, &cx->re, sizeof br);
        memcpy(&bi, &cx->im, sizeof bi);
        return (sp_int)((uintptr_t)(br * 31u) ^ (uintptr_t)bi);
      }
      if (sp_obj_hash_hook) {
        /* A Struct that holds itself hashes its own members through here; the
           generated hook has no path of its own, so this is where it stops. */
        if (sp_poly_recur_seen(SP_POLY_RECUR_HASH, v.v.p, NULL)) return SP_POLY_RECUR_HASH_VALUE;
        int mark = sp_poly_recur_push(SP_POLY_RECUR_HASH, v.v.p, NULL);
        sp_int oh = sp_obj_hash_hook(v.cls_id, v.v.p);
        sp_poly_recur_pop(mark);
        return oh;
      }
      return (sp_int)((uintptr_t)v.v.p);
  }
  return 0;
}
static sp_bool sp_rbval_eql_key(sp_RbVal a, sp_RbVal b) {
  if (a.tag != b.tag) return FALSE;
  switch (a.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return a.v.i == b.v.i;
    case SP_TAG_BIGINT:
      return sp_bigint_cmp((sp_Bigint *)a.v.p, (sp_Bigint *)b.v.p) == 0;
    case SP_TAG_STR:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return sp_str_cmp_bytes(a.v.s, b.v.s) == 0;
    case SP_TAG_ENCODING:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return strcmp(a.v.s, b.v.s) == 0;
    case SP_TAG_CLASS: {   /* by name, paired with the name-based hash above */
      const char *an = sp_class_val_name(a), *bn = sp_class_val_name(b);
      return (an && bn) ? strcmp(an, bn) == 0 : an == bn;
    }
    case SP_TAG_FLT:
      /* a NaN is not == to itself, but CRuby's container lookups fall back on
         identity, so the very same NaN is still found by its own key (#3650) */
      if (a.v.f != a.v.f && b.v.f != b.v.f)
        return memcmp(&a.v.f, &b.v.f, sizeof a.v.f) == 0;
      return a.v.f == b.v.f;
    case SP_TAG_OBJ:
      /* Arrays are eql across storage kinds and element-wise eql (so 1 and 1.0
         stay distinct): an IntArray [0, 0] key and a PolyArray [0, 0] lookup
         (e.g. one from Array#product) must collide, so this runs BEFORE the
         cls_id inequality check below (their kinds differ). Also covers
         same-kind PolyArray keys. Paired with the value-based array hash
         above (#2911). */
      if (sp_poly_is_array_kind(a.cls_id) && sp_poly_is_array_kind(b.cls_id)) {
        if (a.v.p == b.v.p && a.cls_id == b.cls_id) return TRUE;
        sp_int n = sp_poly_length(a);
        if (n != sp_poly_length(b)) return FALSE;
        /* The same pair guard sp_poly_eql carries: this is the eql? a Hash
           LOOKUP runs, so `h = {a => 1}; h[b]` with two distinct arrays that
           each hold themselves walks into the same pair for ever without it. */
        if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) return TRUE;
        {
          int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
          sp_bool r = TRUE;
          for (sp_int i = 0; r && i < n; i++)
            r = sp_rbval_eql_key(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i));
          sp_poly_recur_pop(mark);
          return r;
        }
      }
      if (a.cls_id != b.cls_id) return FALSE;
      if (a.v.p == b.v.p) return TRUE;
      if (a.cls_id == SP_BUILTIN_REGEX)
        return sp_re_eq(a.v.p, b.v.p);   /* same source, same pattern (#3681) */
      if (a.cls_id == SP_BUILTIN_METHOD) {
        sp_BoundMethod *ma = (sp_BoundMethod *)a.v.p, *mb = (sp_BoundMethod *)b.v.p;
        if (!ma || !mb) return ma == mb;
        if (ma->self != mb->self || ma->fn != mb->fn) return FALSE;
        if (ma->name == mb->name) return TRUE;
        return ma->name && mb->name && strcmp(ma->name, mb->name) == 0;
      }
      if (a.cls_id == SP_BUILTIN_RATIONAL) {
        /* value-based so equal Rationals serve as one Hash key (paired with the
           value-based hash above). */
        sp_Rational *ra = (sp_Rational *)a.v.p, *rb = (sp_Rational *)b.v.p;
        return (ra && rb) ? (ra->num == rb->num && ra->den == rb->den) : (ra == rb);
      }
      if (a.cls_id == SP_BUILTIN_TIME) {
        /* instant equality, paired with the value-based hash above */
        sp_Time *ta = (sp_Time *)a.v.p, *tb = (sp_Time *)b.v.p;
        return (ta && tb) ? (ta->tv_sec == tb->tv_sec && ta->tv_nsec == tb->tv_nsec) : (ta == tb);
      }
      if (a.cls_id == SP_BUILTIN_RANGE) {
        /* same bounds, same exclusivity -- paired with the hash above (#3669) */
        sp_Range *ra = (sp_Range *)a.v.p, *rb = (sp_Range *)b.v.p;
        return (ra && rb) ? (ra->first == rb->first && ra->last == rb->last &&
                             (!ra->excl) == (!rb->excl)) : (ra == rb);
      }
      if (a.cls_id == SP_BUILTIN_STR_RANGE) {
        sp_StrRange *ra = (sp_StrRange *)a.v.p, *rb = (sp_StrRange *)b.v.p;
        if (!ra || !rb) return ra == rb;
        return (!ra->excl) == (!rb->excl) && sp_str_eq(ra->first, rb->first) &&
               sp_str_eq(ra->last, rb->last);
      }
      if (a.cls_id == SP_BUILTIN_FLOAT_RANGE) {
        sp_FloatRange *ra = (sp_FloatRange *)a.v.p, *rb = (sp_FloatRange *)b.v.p;
        return (ra && rb) ? (ra->first == rb->first && ra->last == rb->last &&
                             (!ra->excl) == (!rb->excl)) : (ra == rb);
      }
      if (a.cls_id == SP_BUILTIN_COMPLEX) {
        /* value-based so an equal Complex serves as one Hash key (#2615),
           paired with the value-based hash above */
        sp_Complex *ca = (sp_Complex *)a.v.p, *cb = (sp_Complex *)b.v.p;
        return (ca && cb) ? (ca->re == cb->re && ca->im == cb->im) : (ca == cb);
      }
      if (sp_obj_eql_hook) {
        /* a Struct key that holds itself reaches its own eql? through the hook */
        if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) return TRUE;
        {
          int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
          sp_bool r = sp_obj_eql_hook(a.cls_id, a.v.p, b.v.p);
          sp_poly_recur_pop(mark);
          return r;
        }
      }
      return FALSE;
  }
  return FALSE;
}
/* dproc holds a Hash.new{} default block (arbitrary keys), lowered to a C fn
   with a boxed sp_RbVal key param. Called by _get on a miss. */
typedef sp_RbVal (*sp_polypoly_dproc_t)(sp_PolyPolyHash *, sp_RbVal, void *);
typedef struct sp_PolyPolyHash{sp_RbVal*keys;sp_RbVal*vals;sp_int*hs;sp_int*order;sp_bool*occ;sp_int len;sp_int cap;sp_int mask;sp_RbVal default_v;sp_polypoly_dproc_t dproc;void *dproc_self;}sp_PolyPolyHash;
static void sp_PolyPolyHash_fin(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;free(h->keys);free(h->vals);free(h->hs);free(h->order);free(h->occ);}
static void sp_PolyPolyHash_scan(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;for(sp_int i=0;i<h->cap;i++){if(h->occ[i]){sp_mark_rbval(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);if(h->dproc_self)sp_gc_mark(h->dproc_self);}
static sp_PolyPolyHash*sp_PolyPolyHash_new(void){sp_PolyPolyHash*h=(sp_PolyPolyHash*)sp_gc_alloc(sizeof(sp_PolyPolyHash),sp_PolyPolyHash_fin,sp_PolyPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->hs=(sp_int*)malloc(sizeof(sp_int)*(size_t)h->cap);h->order=(sp_int*)malloc(sizeof(sp_int)*(size_t)h->cap);h->occ=(sp_bool*)calloc((size_t)h->cap,sizeof(sp_bool));h->len=0;h->default_v=sp_box_nil();return h;}
static sp_PolyPolyHash*sp_PolyPolyHash_new_with_default(sp_RbVal d){sp_PolyPolyHash*h=sp_PolyPolyHash_new();h->default_v=d;return h;}
static sp_PolyPolyHash*sp_PolyPolyHash_new_dproc(sp_polypoly_dproc_t fn,void*self){sp_PolyPolyHash*h=sp_PolyPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
/* The table keeps each key's hash beside it, as CRuby's does, so the
   layout a growing table does, and a delete's shift, read it rather than
   ask the key again: a key with a hash hook of its own reaches generated
   code, which can allocate -- and collect the entries a half-moved table no
   longer showed the collector -- or store into this very hash, from the
   middle of its own layout. Neither runs user code; the hook runs once,
   when the key arrives, or when Hash#rehash asks again. */
static void sp_PolyPolyHash_grow(sp_PolyPolyHash*h){ sp_gc_wb((void*)h);sp_RbVal*ok=h->keys;sp_RbVal*ov=h->vals;sp_int*ohs=h->hs;sp_bool*oo=h->occ;sp_int*oord=h->order;sp_int olen=h->len;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc((size_t)h->cap,sizeof(sp_RbVal));h->hs=(sp_int*)malloc(sizeof(sp_int)*(size_t)h->cap);h->order=(sp_int*)malloc(sizeof(sp_int)*(size_t)h->cap);h->occ=(sp_bool*)calloc((size_t)h->cap,sizeof(sp_bool));for(sp_int i=0;i<olen;i++){sp_int oi=oord[i];sp_int idx=(sp_int)(ohs[oi]&h->mask);while(h->occ[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[oi];h->vals[idx]=ov[oi];h->hs[idx]=ohs[oi];h->occ[idx]=TRUE;h->order[i]=idx;}free(ok);free(ov);free(ohs);free(oo);free(oord);}
/* Miss path returns default_v, which is nil unless set via Hash.new(d) /
   Hash#default= -- so plain {} hashes keep surfacing Ruby nil (#801). */
/* miss path cold+noinline, same reason as sp_SymPolyHash_miss above. */
static SP_NOINLINE sp_RbVal sp_PolyPolyHash_miss(sp_PolyPolyHash*h,sp_RbVal k){if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static sp_RbVal sp_PolyPolyHash_get(sp_PolyPolyHash*h,sp_RbVal k){if(!h)return sp_box_nil();SP_GC_ROOT(h);SP_GC_ROOT_RBVAL(k);sp_int hs=sp_hash_slot(sp_rbval_hash_key(k));sp_int idx=(sp_int)(hs&h->mask);while(h->occ[idx]){if(h->hs[idx]==hs&&sp_rbval_eql_key(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return sp_PolyPolyHash_miss(h,k);}
static sp_bool sp_PolyPolyHash_has_value(sp_PolyPolyHash*h,sp_RbVal v){if(!h)return FALSE;for(sp_int i=0;i<h->len;i++)if(sp_poly_eq(h->vals[h->order[i]],v))return TRUE;return FALSE;}
/* defined with the curry machinery below; the key-typed reads and the cold
   index path all apply a curried receiver through it */
static sp_RbVal sp_curry_call_poly(sp_Curry *c, sp_int argc, const sp_RbVal *args);
static sp_RbVal sp_poly_get_sym(sp_RbVal v, sp_sym key) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_CURRY: return sp_curry_call_poly((sp_Curry *)v.v.p, 1, (sp_RbVal[]){sp_box_sym(key)});
    case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_get((sp_SymPolyHash*)v.v.p, key);
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_get((sp_PolyPolyHash*)v.v.p, sp_box_sym(key));
    /* an OpenStruct read as poly (e.g. returned from a method that can also
       return an int): `o[:k]` is the member value (#3193). */
    case SP_BUILTIN_OPENSTRUCT: return sp_OpenStruct_get((sp_OpenStruct*)v.v.p, key);
    default: break;
  }
  /* A Struct / Data read out of a poly container still answers `o[:member]`:
     the member table is the same one #to_h walks, so go through that rather
     than returning nil for every subscript (#3369). */
  if (v.cls_id >= 0 && sp_obj_to_h_fn) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ && h.cls_id == SP_BUILTIN_SYM_POLY_HASH)
      return sp_SymPolyHash_get((sp_SymPolyHash *)h.v.p, key);
  }
  return sp_box_nil();
}
static void sp_PolyPolyHash_set(sp_PolyPolyHash*h,sp_RbVal k,sp_RbVal v){sp_gc_wb((void*)h); SP_GC_ROOT(h);SP_GC_ROOT_RBVAL(k);SP_GC_ROOT_RBVAL(v);sp_int hs=sp_hash_slot(sp_rbval_hash_key(k));if(h->len*2>=h->cap)sp_PolyPolyHash_grow(h);sp_int idx=(sp_int)(hs&h->mask);while(h->occ[idx]){if(h->hs[idx]==hs&&sp_rbval_eql_key(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->hs[idx]=hs;h->occ[idx]=TRUE;h->order[h->len]=idx;h->len++;}
/* Marshal.dump/load hash vtable slots (sp_marshal_v.hash_new/hash_set):
   kept here (not moved to lib/sp_cold.c with the rest of the sp_marv_*
   vtable fns) since they'd otherwise force sp_PolyPolyHash_new/set --
   hot, called dozens of times elsewhere via sp_PolyArray_tally -- to
   become non-static just to save two one-line wrappers. */
static sp_RbVal sp_marv_hash_new(void) { return sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH); }
/* Hash#update / #merge! on the poly-keyed variant. Every other variant had it;
   this one did not, so a `merge` the emitter lowered here left a reference to a
   function the build never emits and the link failed (#4020). */
static void sp_PolyPolyHash_update(sp_PolyPolyHash *a, sp_PolyPolyHash *b) {
  if (!a || !b || a == b) return;
  SP_GC_ROOT(a); SP_GC_ROOT(b);
  for (sp_int i = 0; i < b->len; i++) {
    sp_int idx = b->order[i];
    sp_PolyPolyHash_set(a, b->keys[idx], b->vals[idx]);
  }
}
static void sp_marv_hash_set(sp_RbVal h, sp_RbVal k, sp_RbVal v) { sp_PolyPolyHash_set((sp_PolyPolyHash *)h.v.p, k, v); }
/* Array#tally over a poly array keys the count hash by the ELEMENT VALUE (any
   type), matching CRuby's `#eql?`/`#hash` bucketing — not by symbol identity.
   Defined here so the PolyPolyHash helpers above are already in scope. */
static sp_PolyPolyHash *sp_PolyArray_tally(sp_PolyArray *a) { if (!a) return sp_PolyPolyHash_new(); SP_GC_ROOT(a); sp_PolyPolyHash *h = sp_PolyPolyHash_new(); SP_GC_ROOT(h); for (sp_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; sp_RbVal cur = sp_PolyPolyHash_get(h, v); sp_int c = (cur.tag == SP_TAG_INT) ? cur.v.i : 0; sp_PolyPolyHash_set(h, v, sp_box_int(c + 1)); } return h; }
/* order[] holds slot indices (not keys), so iterate keys/vals by the stored
   index; merge inherits the LEFT receiver's default per CRuby. */
static sp_PolyPolyHash*sp_PolyPolyHash_merge(sp_PolyPolyHash*a,sp_PolyPolyHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);if(a){r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++){sp_int idx=a->order[i];sp_PolyPolyHash_set(r,a->keys[idx],a->vals[idx]);}}if(b){for(sp_int i=0;i<b->len;i++){sp_int idx=b->order[i];sp_PolyPolyHash_set(r,b->keys[idx],b->vals[idx]);}}return r;}
static sp_bool sp_PolyPolyHash_has_key(sp_PolyPolyHash*h,sp_RbVal k){if(!h)return FALSE;SP_GC_ROOT(h);SP_GC_ROOT_RBVAL(k);sp_int hs=sp_hash_slot(sp_rbval_hash_key(k));sp_int idx=(sp_int)(hs&h->mask);while(h->occ[idx]){if(h->hs[idx]==hs&&sp_rbval_eql_key(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
/* Hash#rehash: a key changed since it was stored is under the hash it was
   stored with. Every entry is stored anew into a fresh table, in order,
   which asks each key again and folds keys that have become eql? into the
   first of them with the last value, as CRuby's does; the arrays are then
   swapped in whole, so a hook that raises leaves the hash as it was. The
   shell they came in is emptied for its finalizer. */
static sp_PolyPolyHash*sp_PolyPolyHash_rehash(sp_PolyPolyHash*h){if(!h)return h;if(sp_gc_is_frozen(h))sp_raise_frozen_hash_at(h,SP_BUILTIN_POLY_POLY_HASH);sp_gc_wb((void*)h);SP_GC_ROOT(h);sp_PolyPolyHash*t=sp_PolyPolyHash_new();SP_GC_ROOT(t);for(sp_int i=0;i<h->len;i++){sp_int oi=h->order[i];sp_PolyPolyHash_set(t,h->keys[oi],h->vals[oi]);}free(h->keys);free(h->vals);free(h->hs);free(h->order);free(h->occ);h->keys=t->keys;h->vals=t->vals;h->hs=t->hs;h->order=t->order;h->occ=t->occ;h->len=t->len;h->cap=t->cap;h->mask=t->mask;t->keys=NULL;t->vals=NULL;t->hs=NULL;t->order=NULL;t->occ=NULL;t->len=0;t->cap=0;return h;}
/* Hash#delete in place: the backward shift the other hash kinds delete
   with, closing the hole the entry leaves so a probe that ran through it
   still reaches what lies beyond. order[] holds slot indexes here, not keys,
   so an entry the shift moves is renumbered where the order holds it. The
   shift reads the hashes the table keeps and runs no user code, so a hook
   that raises, or stores into this hash, cannot catch it half done.
   Rebuilding the table instead -- every remaining key hashed and inserted
   anew -- would cost a delete what filling the table costs. */
static void sp_PolyPolyHash_delete(sp_PolyPolyHash*h,sp_RbVal k){
  if(!h)return;
  sp_gc_wb((void*)h);
  SP_GC_ROOT(h);SP_GC_ROOT_RBVAL(k);
  sp_int hs=sp_hash_slot(sp_rbval_hash_key(k));
  sp_int idx=(sp_int)(hs&h->mask);
  while(h->occ[idx]){
    if(h->hs[idx]==hs&&sp_rbval_eql_key(h->keys[idx],k)){
      for(sp_int oi=0;oi<h->len;oi++)if(h->order[oi]==idx){h->len--;memmove(h->order+oi,h->order+oi+1,sizeof(sp_int)*(size_t)(h->len-oi));break;}
      h->occ[idx]=FALSE;h->keys[idx]=sp_box_nil();h->vals[idx]=sp_box_nil();
      sp_int j=(idx+1)&h->mask;
      while(h->occ[j]){
        sp_int nj=(sp_int)(h->hs[j]&h->mask);
        if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){
          h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->hs[idx]=h->hs[j];h->occ[idx]=TRUE;
          h->occ[j]=FALSE;h->keys[j]=sp_box_nil();h->vals[j]=sp_box_nil();
          for(sp_int p=0;p<h->len;p++)if(h->order[p]==j){h->order[p]=idx;break;}
          idx=j;
        }
        j=(j+1)&h->mask;
      }
      return;
    }
    idx=(idx+1)&h->mask;
  }
}
/* format's %<name> / %{name}: find the key in the trailing hash argument by
   its name (a keyword hash boxes as SymPolyHash; string-keyed and fully-poly
   hashes are accepted too). A missing name is CRuby's KeyError. */
/* `own` is the caller's format buffer: a missing name raises out of here, and
   the raise would otherwise leak it (a plain malloc, not a GC string).
   `nclose` spells the message the way the directive was written -- CRuby says
   `key<a>` for %<a> and `key{a}` for %{a}. */
static sp_RbVal sp_fmt_named_ref(sp_PolyArray *a, const char *nm, char nclose, char *own) {
  sp_RbVal h = (a && a->len > 0) ? a->data[a->len - 1] : sp_box_nil();
  if (h.tag == SP_TAG_OBJ && h.v.p) {
    if (h.cls_id == SP_BUILTIN_SYM_POLY_HASH) {
      sp_SymPolyHash *sh = (sp_SymPolyHash *)h.v.p;
      for (sp_int i = 0; i < sh->len; i++)
        if (sp_str_eq_cstr(sp_sym_to_s(sh->order[i]), nm))
          return sp_SymPolyHash_get(sh, sh->order[i]);
    }
    else if (h.cls_id == SP_BUILTIN_STR_POLY_HASH) {
      sp_StrPolyHash *th = (sp_StrPolyHash *)h.v.p;
      for (sp_int i = 0; i < th->len; i++)
        if (sp_str_eq_cstr(th->order[i], nm)) return sp_StrPolyHash_get(th, th->order[i]);
    }
    else if (h.cls_id == SP_BUILTIN_POLY_POLY_HASH) {
      sp_PolyPolyHash *ph = (sp_PolyPolyHash *)h.v.p;
      for (sp_int i = 0; i < ph->len; i++) {
        sp_RbVal k = ph->keys[ph->order[i]];
        if ((k.tag == SP_TAG_SYM && sp_str_eq_cstr(sp_sym_to_s((sp_sym)k.v.i), nm)) ||
            (k.tag == SP_TAG_STR && k.v.s && sp_str_eq_cstr(k.v.s, nm)))
          return ph->vals[ph->order[i]];
      }
    }
  }
  { const char *m = sp_sprintf(nclose == '}' ? "key{%s} not found" : "key<%s> not found", nm);
    free(own);
    sp_raise_cls("KeyError", m); }
}
static sp_int sp_PolyPolyHash_length(sp_PolyPolyHash*h){return h->len;}
static void sp_PolyPolyHash_clear(sp_PolyPolyHash*h){if(!h)return;for(sp_int i=0;i<h->cap;i++)h->occ[i]=0;h->len=0;}
/* `#clear` on a poly value (a mixed Array/Hash collection element reached via
   `&:clear`): empty the container in place, dispatching on its runtime kind and
   returning the receiver (#3199). */
static sp_RbVal sp_poly_clear(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ || !v.v.p) return v;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:      ((sp_IntArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_FLT_ARRAY:      ((sp_FloatArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_STR_ARRAY:      ((sp_StrArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_POLY_ARRAY:     ((sp_PolyArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_STRBUF: {
      sp_String *_m = (sp_String *)v.v.p;
      if (sp_String_is_frozen(_m)) { sp_raise_frozen_str(_m->data); break; }
      _m->len = 0; _m->data[0] = 0; sp_fd_publish(_m);
      break;
    }
    case SP_BUILTIN_STR_INT_HASH:   sp_StrIntHash_clear((sp_StrIntHash *)v.v.p); break;
    case SP_BUILTIN_STR_STR_HASH:   sp_StrStrHash_clear((sp_StrStrHash *)v.v.p); break;
    case SP_BUILTIN_INT_STR_HASH:   sp_IntStrHash_clear((sp_IntStrHash *)v.v.p); break;
    case SP_BUILTIN_INT_INT_HASH:   sp_IntIntHash_clear((sp_IntIntHash *)v.v.p); break;
    case SP_BUILTIN_STR_POLY_HASH:  sp_StrPolyHash_clear((sp_StrPolyHash *)v.v.p); break;
    case SP_BUILTIN_SYM_POLY_HASH:  sp_SymPolyHash_clear((sp_SymPolyHash *)v.v.p); break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_clear((sp_PolyPolyHash *)v.v.p); break;
    default: sp_raise_nomethod(sp_nomethod_msg("clear", v)); break;
  }
  return v;
}
/* Array#pop / #shift on a poly value (an array-kind box reaching a poly
   parameter, e.g. one call site passes a StrArray and another a PolyArray):
   mutate the underlying container in place, dispatching on its runtime kind,
   and return the removed element boxed (nil when empty). */
/* Array#pop(n) / #shift(n) on a boxed array: the last (or first) n elements,
   removed, as an Array (#3613). */
static sp_RbVal sp_poly_pop(sp_RbVal v);
static sp_RbVal sp_poly_shift(sp_RbVal v);
static sp_PolyArray *sp_poly_pop_n(sp_RbVal v, sp_int n, int from_front) {
  SP_GC_ROOT_RBVAL(v);
  if (n < 0) sp_raise_cls("ArgumentError", "negative array size");
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = from_front ? sp_poly_shift(v) : sp_poly_pop(v);
    if (e.tag == SP_TAG_NIL && sp_poly_length(v) == 0) break;
    sp_PolyArray_push(out, e);
  }
  if (!from_front) sp_PolyArray_reverse_bang(out);
  return out;
}
static sp_RbVal sp_poly_pop(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.v.p) {
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY: {
        sp_IntArray *a = (sp_IntArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_int(sp_IntArray_pop(a));
      }
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_float(sp_FloatArray_pop(a));
      }
      case SP_BUILTIN_STR_ARRAY: {
        sp_StrArray *a = (sp_StrArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_str(sp_StrArray_pop(a));
      }
      case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_pop((sp_PolyArray *)v.v.p);
      default: break;
    }
  }
  sp_raise_nomethod(sp_nomethod_msg("pop", v));
  return sp_box_nil();
}
/* Array#insert on a poly value: in-place insertion of one value at an index
   through the runtime kind dispatch; returns the receiver. */
/* Replace the contents a boxed String stands for, and answer the result. A
   shared handle mutates its buffer so every alias observes it; a plain string
   box has no handle to write through, so it follows the value-form contract
   the typed emitter uses for the same case (frozen check, then the new
   string). */
static sp_RbVal sp_poly_str_become(sp_RbVal v, const char *s) {
  if (sp_poly_is_strbuf(v)) { sp_String_set_bin((sp_String *)v.v.p, s); return v; }
  sp_str_check_mutable(v.v.s);
  return sp_box_str(s);
}
/* The same through a receiver that is no variable -- an element read, a Hash
   value: a shared handle absorbs the new contents and every alias observes
   the change; a plain string box has nowhere to send them, and the call
   raises the NoMethodError it raised before the row existed, rather than a
   mutation that silently goes nowhere. */
static sp_RbVal sp_poly_str_become_handle(sp_RbVal v, const char *s, const char *name) {
  if (sp_poly_is_strbuf(v)) { sp_String_set_bin((sp_String *)v.v.p, s); return v; }
  sp_raise_nomethod(sp_nomethod_msg(name, v));
  return v;
}
/* Are the contents a mutator answered the receiver's own? Then it changed
   nothing, and a mutator that writes only when it changed something (scrub!)
   writes nothing -- and raises no FrozenError for a write that never happens,
   as CRuby does not. Only such a row asks: sub! with no match answers its
   receiver's own contents too, and CRuby raises for it. */
static int sp_poly_str_is_own(sp_RbVal v, const char *s) {
  if (v.tag == SP_TAG_STR) return v.v.s == s;
  if (sp_poly_is_strbuf(v)) return ((sp_String *)v.v.p)->data == s;
  return 0;
}
static sp_RbVal sp_poly_insert(sp_RbVal v, sp_int i, sp_RbVal x) {
  /* String#insert on a boxed receiver: splice at a character index, where a
     negative index counts from the end AFTER the last character (#3445). */
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v)) {
    const char *s = sp_poly_strbuf_deref(v).v.s;
    if (!s) s = (&("\xff")[1]);
    if (i < 0) i += (sp_int)sp_str_length(s) + 1;
    return sp_poly_str_become(v, sp_str_splice_at(s, i, 0, sp_poly_to_s(x), 0));
  }
  if (v.tag == SP_TAG_OBJ && v.v.p) {
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY:
        sp_IntArray_insert((sp_IntArray *)v.v.p, i, sp_poly_to_i(x));
        return v;
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p;
        sp_int orig = i, i2 = i < 0 ? i + a->len + 1 : i;
        if (i2 < 0)
          sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",
                                                (long long)orig, (long long)(-(a->len + 1))));
        while (i2 > a->len) sp_FloatArray_push(a, (sp_float)0);
        { sp_float fv = sp_poly_to_f(x); sp_FloatArray_splice(a, i2, 0, &fv, 1); }
        return v;
      }
      case SP_BUILTIN_STR_ARRAY:
        sp_StrArray_insert((sp_StrArray *)v.v.p, i, sp_poly_to_s(x));
        return v;
      case SP_BUILTIN_POLY_ARRAY:
        sp_PolyArray_insert((sp_PolyArray *)v.v.p, i, x);
        return v;
      default: break;
    }
  }
  sp_raise_nomethod(sp_nomethod_msg("insert", v));
  return sp_box_nil();
}
/* Array#delete_at on a poly value: in-place removal at an index through the
   runtime kind dispatch; the removed element boxed, nil when out of range. */
static sp_RbVal sp_poly_delete_at(sp_RbVal v, sp_int i) {
  if (v.tag == SP_TAG_OBJ && v.v.p) {
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY: {
        sp_IntArray *a = (sp_IntArray *)v.v.p;
        sp_int r = sp_IntArray_delete_at(a, i);
        return r == SP_INT_NIL ? sp_box_nil() : sp_box_int(r);
      }
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p;
        sp_int i2 = i < 0 ? i + a->len : i;
        if (i2 < 0 || i2 >= a->len) return sp_box_nil();
        return sp_box_float(sp_FloatArray_delete_at(a, i));
      }
      case SP_BUILTIN_STR_ARRAY: {
        const char *r = sp_StrArray_delete_at((sp_StrArray *)v.v.p, i);
        return r ? sp_box_str(r) : sp_box_nil();
      }
      case SP_BUILTIN_POLY_ARRAY: {
        sp_PolyArray *a = (sp_PolyArray *)v.v.p;
        sp_int i2 = i < 0 ? i + a->len : i;
        if (i2 < 0 || i2 >= a->len) return sp_box_nil();
        if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY); return sp_box_nil(); }
        sp_RbVal r = a->data[i2];
        memmove(a->data + i2, a->data + i2 + 1,
                (size_t)(a->len - i2 - 1) * sizeof(sp_RbVal));
        a->len--;
        return r;
      }
      default: break;
    }
  }
  sp_raise_nomethod(sp_nomethod_msg("delete_at", v));
  return sp_box_nil();
}
static sp_RbVal sp_poly_shift(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.v.p) {
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY: {
        sp_IntArray *a = (sp_IntArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_int(sp_IntArray_shift(a));
      }
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_float(sp_FloatArray_shift(a));
      }
      case SP_BUILTIN_STR_ARRAY: {
        sp_StrArray *a = (sp_StrArray *)v.v.p;
        if (a->len <= 0) return sp_box_nil();
        return sp_box_str(sp_StrArray_shift(a));
      }
      case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_shift((sp_PolyArray *)v.v.p);
      default: break;
    }
  }
  sp_raise_nomethod(sp_nomethod_msg("shift", v));
  return sp_box_nil();
}
static sp_RbVal sp_poly_get_str(sp_RbVal v, const char *key) {
  /* `s["sub"]` is String#[str]: the substring itself when present, else nil.
     Both string representations answer it, and neither did here -- an
     immediate string was rejected by the tag test on the next line and a
     shared handle fell out of the switch below (#4279). */
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v)) {
    const char *s = v.tag == SP_TAG_STR ? (v.v.s ? v.v.s : sp_str_empty) : sp_poly_to_s(v);
    return (key && sp_str_include(s, key)) ? sp_box_str(key) : sp_box_nil();
  }
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_CURRY: return sp_curry_call_poly((sp_Curry *)v.v.p, 1, (sp_RbVal[]){sp_box_str(key)});
    case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_get((sp_StrPolyHash*)v.v.p, key);
    case SP_BUILTIN_STR_STR_HASH: { const char *s = sp_StrStrHash_get((sp_StrStrHash*)v.v.p, key); return s ? sp_box_str(s) : sp_box_nil(); }
    case SP_BUILTIN_STR_INT_HASH: { sp_int i = sp_StrIntHash_get_opt((sp_StrIntHash*)v.v.p, key); return i == SP_INT_NIL ? sp_box_nil() : sp_box_int(i); }
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_get((sp_PolyPolyHash*)v.v.p, sp_box_str(key));
    default: break;
  }
  /* Struct#["member"] names the member, like the symbol form (#3369) */
  if (v.cls_id >= 0 && sp_obj_to_h_fn && key)
    return sp_poly_get_sym(v, sp_sym_intern(key));
  return sp_box_nil();
}
/* Extend sp_poly_arr_len for hash types defined after the initial declaration. */
/* ---- widening one hash storage kind into another --------------------------
   Ruby has one Hash; this runtime has seven storage kinds, and they do not
   share a layout. So a body that builds a string-VALUED hash cannot simply be
   returned through a signature declaring a poly-valued one -- the pointer
   would be reinterpreted and every read would come back as garbage, silently
   on a compiler that only warns about the type mismatch (#3420). Convert
   instead, entry by entry, the way the array side already does
   (sp_PolyArray_from_str_array and friends).

   Written against the generic pair reader so one function covers every source
   kind: sp_poly_each_elem renders a Hash entry as a boxed [k, v] pair for all
   of them. */
static sp_StrPolyHash *sp_StrPolyHash_from_poly(sp_RbVal src);
static sp_SymPolyHash *sp_SymPolyHash_from_poly(sp_RbVal src);
static sp_PolyPolyHash *sp_PolyPolyHash_from_poly(sp_RbVal src);
/* A poly `each` receiver that is no collection has no `each` at all, which is
   CRuby's NoMethodError. Iterating it zero times instead answered the receiver
   silently, and the chain carried on: `5.each { }.size` printed 8, Integer#size
   of the receiver the loop handed back (#3987). A user object is let through --
   it may define its own each, and the to_a normalization ahead of this runs
   first. */
static void sp_poly_iter_check(sp_RbVal v, const char *m) {
  if (v.tag == SP_TAG_OBJ &&
      (v.cls_id >= 0 || sp_poly_is_array_kind(v.cls_id) ||
       sp_poly_is_hash_kind(v.cls_id) || v.cls_id == SP_BUILTIN_RANGE ||
       v.cls_id == SP_BUILTIN_ENUMERATOR))
    return;
  sp_raise_poly_nomethod(m, v);
}
static sp_int sp_poly_arr_len_ex(sp_RbVal a) {
  if (a.tag != SP_TAG_OBJ) return 0;
  switch (a.cls_id) {
    case SP_BUILTIN_RANGE: { sp_Range *r = (sp_Range *)a.v.p; sp_int n = r->last - r->first + (r->excl ? 0 : 1); return n > 0 ? n : 0; }
    default:
      /* EVERY hash kind, not just the poly-valued three. sp_poly_each_elem
         renders a pair for all of them, and this length gated the loop: a
         String-keyed Hash reaching a poly `each` iterated zero times and the
         block simply never ran, silently (#4041). */
      if (sp_poly_is_hash_kind(a.cls_id)) return sp_poly_length(a);
      return sp_poly_arr_len(a);
  }
}
/* sp_poly_each_elem: return the i-th element for sequential each-iteration.
   For arrays: element at index i. For hashes: the i-th insertion-order
   key-value pair as a 2-element PolyArray so |k, v| block splat works. */
static sp_RbVal sp_poly_each_elem(sp_RbVal a, sp_int i) {
  SP_GC_ROOT_RBVAL(a);   /* the boxing arms below allocate */
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_POLY_ARRAY:
      return sp_poly_arr_get(a, i);
    case SP_BUILTIN_RANGE: { sp_Range *r = (sp_Range *)a.v.p; return sp_box_int(r->first + i); }
    case SP_BUILTIN_STR_INT_HASH: {
      sp_StrIntHash *h = (sp_StrIntHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      sp_PolyArray_push(pair, sp_box_int(sp_StrIntHash_get(h, h->order[i])));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_STR_STR_HASH: {
      sp_StrStrHash *h = (sp_StrStrHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      const char *sv = sp_StrStrHash_get(h, h->order[i]);
      sp_PolyArray_push(pair, sv ? sp_box_str(sv) : sp_box_nil());
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_STR_POLY_HASH: {
      sp_StrPolyHash *h = (sp_StrPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      sp_PolyArray_push(pair, sp_StrPolyHash_get(h, h->order[i]));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_SYM_POLY_HASH: {
      sp_SymPolyHash *h = (sp_SymPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_sym(h->order[i]));
      sp_PolyArray_push(pair, sp_SymPolyHash_get(h, h->order[i]));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_INT_STR_HASH: {
      sp_IntStrHash *h = (sp_IntStrHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_int(h->order[i]));
      const char *iv = sp_IntStrHash_get(h, h->order[i]);
      sp_PolyArray_push(pair, iv ? sp_box_str(iv) : sp_box_nil());
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_INT_INT_HASH: {
      sp_IntIntHash *h = (sp_IntIntHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_int(h->order[i]));
      sp_PolyArray_push(pair, sp_box_int(sp_IntIntHash_get(h, h->order[i])));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_POLY_POLY_HASH: {
      sp_PolyPolyHash *h = (sp_PolyPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_int idx = h->order[i];
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, h->keys[idx]);
      sp_PolyArray_push(pair, h->vals[idx]);
      return sp_box_poly_array(pair); }
    default: return sp_box_nil();
  }
}
/* Array#zip with no arguments: each element alone in a one-element array
   ([1, 2].zip -> [[1], [2]]), the degenerate case of zipping nothing (#3612). */
static sp_PolyArray *sp_poly_zip_none(sp_RbVal a) {
  SP_GC_ROOT_RBVAL(a);
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  sp_int n = sp_poly_arr_len(a);
  for (sp_int i = 0; i < n; i++) {
    sp_PolyArray *e = sp_PolyArray_new();
    SP_GC_ROOT(e);
    sp_PolyArray_push(e, sp_poly_each_elem(a, i));
    sp_PolyArray_push(r, sp_box_poly_array(e));
  }
  return r;
}
/* `f[a, b]` where f is a Proc: Proc#[] IS #call, and its arguments are whatever
   the proc takes. Read as the two-integer slice it looks like, an Array operand
   raised TypeError before any dispatch could happen (#4333). Only a pair of
   Integers can be a slice, so the emitter sends anything else here; everything
   that is not a callable delegates to sp_poly_slice, which keeps the bound
   Method arm it already had. */
static sp_RbVal sp_poly_callable_call(sp_RbVal v, sp_int n, const sp_int *args);
static sp_RbVal sp_poly_slice_or_call(sp_RbVal v, sp_RbVal a, sp_RbVal b) {
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id == SP_BUILTIN_PROC || v.cls_id == SP_BUILTIN_CURRY)) {
    _sp_proc_poly_args[0] = a;
    _sp_proc_poly_args[1] = b;
    sp_int slots[16];
    slots[0] = sp_poly_slot_i(a);
    slots[1] = sp_poly_slot_i(b);
    return sp_poly_callable_call(v, 2, slots);
  }
  return sp_poly_slice(v, sp_poly_arg_int_chk(a), sp_poly_arg_int_chk(b));
}
/* `a.zip(*xs)` and `a.product(*xs)`: the splat spreads across the ARGUMENT
   LIST, one operand per element, and its length is only known at run time --
   which is why the emitters, whose arms read a splat as a single operand,
   refused these shapes rather than answer wrongly (#4322, #4323). `ops` holds
   the spread operand list; the receiver is not in it. */
static sp_PolyArray *sp_poly_zip_n(sp_RbVal a, sp_PolyArray *ops) {
  SP_GC_ROOT_RBVAL(a);
  SP_GC_ROOT(ops);
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  /* Convert each operand once: sp_zip_arg raises CRuby's TypeError for one
     that answers no :each, and every row reads the conversion again. */
  sp_PolyArray *cols = sp_PolyArray_new();
  SP_GC_ROOT(cols);
  for (sp_int j = 0; j < ops->len; j++)
    sp_PolyArray_push(cols, sp_box_poly_array(sp_zip_arg(ops->data[j])));
  sp_int n = sp_poly_arr_len(a);
  for (sp_int i = 0; i < n; i++) {
    sp_PolyArray *row = sp_PolyArray_new();
    SP_GC_ROOT(row);
    sp_PolyArray_push(row, sp_poly_each_elem(a, i));
    for (sp_int j = 0; j < cols->len; j++) {
      sp_PolyArray *cj = (sp_PolyArray *)cols->data[j].v.p;
      sp_PolyArray_push(row, (cj && i < cj->len) ? cj->data[i] : sp_box_nil());
    }
    sp_PolyArray_push(r, sp_box_poly_array(row));
  }
  return r;
}
static sp_PolyArray *sp_poly_set_operand(sp_RbVal v);   /* fwd */
static sp_PolyArray *sp_poly_product_n(sp_RbVal a, sp_PolyArray *ops) {
  SP_GC_ROOT_RBVAL(a);
  SP_GC_ROOT(ops);
  /* the receiver is operand 0, so the tuples are drawn in source order */
  sp_PolyArray *all = sp_PolyArray_new();
  SP_GC_ROOT(all);
  sp_PolyArray_push(all, a);
  for (sp_int i = 0; i < ops->len; i++) {
    (void)sp_poly_set_operand(ops->data[i]);   /* CRuby's TypeError for a non-Array */
    sp_PolyArray_push(all, ops->data[i]);
  }
  return sp_poly_product(all->data, all->len);
}
/* Kernel#warn's per-message rendering: an Array contributes one line per
   element (recursively, so a nested array flattens and an empty one
   contributes nothing); anything else is its to_s on a line of its own. A
   message that already ends in a newline does not get a second one. */
static void sp_poly_warn_line(sp_RbVal v, FILE *f) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    sp_int n = sp_poly_arr_len(v);
    for (sp_int i = 0; i < n; i++) sp_poly_warn_line(sp_poly_each_elem(v, i), f);
    return;
  }
  const char *s = sp_poly_to_s(v);
  if (!s) s = sp_str_empty;
  fputs(s, f);
  if (!*s || s[strlen(s) - 1] != '\n') fputc('\n', f);
}

static sp_StrPolyHash *sp_StrPolyHash_from_poly(sp_RbVal src) {
  sp_StrPolyHash *h = sp_StrPolyHash_new(); SP_GC_ROOT(h);
  sp_int n = sp_poly_arr_len_ex(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_each_elem(src, i);
    sp_RbVal k = sp_poly_arr_get(e, 0), v = sp_poly_arr_get(e, 1);
    sp_StrPolyHash_set(h, k.tag == SP_TAG_STR ? k.v.s : sp_poly_to_s(k), v);
  }
  return h;
}
static sp_SymPolyHash *sp_SymPolyHash_from_poly(sp_RbVal src) {
  sp_SymPolyHash *h = sp_SymPolyHash_new(); SP_GC_ROOT(h);
  sp_int n = sp_poly_arr_len_ex(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_each_elem(src, i);
    sp_RbVal k = sp_poly_arr_get(e, 0), v = sp_poly_arr_get(e, 1);
    sp_SymPolyHash_set(h, (sp_sym)k.v.i, v);
  }
  return h;
}
static sp_PolyPolyHash *sp_PolyPolyHash_from_poly(sp_RbVal src) {
  sp_PolyPolyHash *h = sp_PolyPolyHash_new(); SP_GC_ROOT(h);
  sp_int n = sp_poly_arr_len_ex(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_each_elem(src, i);
    sp_PolyPolyHash_set(h, sp_poly_arr_get(e, 0), sp_poly_arr_get(e, 1));
  }
  return h;
}
/* A boxed hash entering a slot of a CONCRETE variant. The variants are
   separate C structs, so the pointer cast that served here reinterpreted one
   as another: a Hash[String, String] read as a Hash[String, poly] kept its
   KEYS -- both are str-keyed, and the key array lines up -- while every VALUE
   read as the zero of some other type, silently (#3998). The matching variant
   is still the pointer itself, so a hash that is already right keeps its
   identity and its mutations; only a mismatch pays for a rebuild. */
static sp_StrPolyHash *sp_poly_as_str_poly_hash(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STR_POLY_HASH) return (sp_StrPolyHash *)v.v.p;
  if (v.tag == SP_TAG_NIL || !sp_poly_is_hash_kind(v.cls_id)) return (sp_StrPolyHash *)0;
  return sp_StrPolyHash_from_poly(v);
}
static sp_SymPolyHash *sp_poly_as_sym_poly_hash(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_SYM_POLY_HASH) return (sp_SymPolyHash *)v.v.p;
  if (v.tag == SP_TAG_NIL || !sp_poly_is_hash_kind(v.cls_id)) return (sp_SymPolyHash *)0;
  return sp_SymPolyHash_from_poly(v);
}
static sp_PolyPolyHash *sp_poly_as_poly_poly_hash(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_POLY_HASH) return (sp_PolyPolyHash *)v.v.p;
  if (v.tag == SP_TAG_NIL || !sp_poly_is_hash_kind(v.cls_id)) return (sp_PolyPolyHash *)0;
  return sp_PolyPolyHash_from_poly(v);
}

/* poly_arr_get/set for PolyPolyHash with integer index key. */
/* multi-assign element read: `a, b = v` destructures only when the boxed
   value is an Array (Ruby's to_ary semantics); any other runtime kind is a
   scalar -- the first target takes the whole value, the rest nil-fill.
   sp_poly_arr_get_hash is NOT that (Integer#[i] reads a bit, String#[i] a
   char, Hash#[i] a lookup). */
static sp_RbVal sp_poly_massign_get(sp_RbVal v, sp_int i) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_FLT_ARRAY:
      return sp_poly_arr_get(v, i);
    default: break;
  }
  return i == 0 ? v : sp_box_nil();
}
/* The poly array is the common receiver here (an element read out of a
   container, a destructured pair), and reaching it through a call cost more
   than the read itself: keeping only that arm inline and pushing every other
   receiver kind out of line is worth 4% on optcarrot, whose inner loops index
   poly arrays per pixel. The cold half carries the arms this path never wants
   -- Struct member order, Integer bit, String character, the hash kinds. */
static SP_NOINLINE sp_RbVal sp_poly_arr_get_hash_cold(sp_RbVal a, sp_int i);

/* The same read for a receiver analyze has proved holds only a poly array or
   nil: no hash, string or Struct arm can be reached, so the cls_id test and the
   cold call behind it are dead. The nil case still has to answer nil, which is
   what the null check does. */
static SP_NOINLINE sp_RbVal sp_poly_arr_get_aon_cold(sp_RbVal a, sp_int i);
static SP_INLINE sp_RbVal sp_poly_arr_get_aon(sp_RbVal a, sp_int i) {
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *ar = (sp_PolyArray *)a.v.p;
    if (!ar) return sp_box_nil();
    sp_int k = i < 0 ? ar->len + i : i;
    if (k < 0 || k >= ar->len) return sp_box_nil();
    return ar->data[k];
  }
  return sp_poly_arr_get_aon_cold(a, i);
}
/* A typed array boxed into a poly slot -- `h["k"] = [7, 8, 9]` stores an
   sp_IntArray -- has a different layout, and reading it as an sp_PolyArray
   answered nil for every index (#3542). That arm belongs out of line for the
   same reason sp_poly_arr_get_hash's does: it is the rare receiver, and
   inlining it grows every hot index site by the whole element-kind switch. */
static SP_NOINLINE sp_RbVal sp_poly_arr_get_aon_cold(sp_RbVal a, sp_int i) {
  if (a.tag != SP_TAG_OBJ || !a.v.p) return sp_box_nil();
  if (!sp_poly_is_array_kind(a.cls_id)) return sp_box_nil();
  sp_int n = sp_poly_arr_len(a);
  sp_int k2 = i < 0 ? n + i : i;
  if (k2 < 0 || k2 >= n) return sp_box_nil();
  return sp_poly_each_elem(a, k2);
}
static SP_INLINE sp_RbVal sp_poly_arr_get_hash(sp_RbVal a, sp_int i) {
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *ar = (sp_PolyArray *)a.v.p;
    if (!ar) return sp_box_nil();
    sp_int k = i < 0 ? ar->len + i : i;
    if (k < 0 || k >= ar->len) return sp_box_nil();
    return ar->data[k];
  }
  return sp_poly_arr_get_hash_cold(a, i);
}

static SP_NOINLINE sp_RbVal sp_poly_arr_get_hash_cold(sp_RbVal a, sp_int i) {
  /* MatchData#[n] is the nth group, and a match stored in a container reaches
     the generic index path (#3641) */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_MATCHDATA)
    return sp_box_nullable_str(sp_MatchData_aref((sp_MatchData *)a.v.p, i));
  /* Struct#[n] is the nth MEMBER in declaration order, not an array index --
     a Struct read out of a poly container reaches here (#3369). */
  if (a.tag == SP_TAG_OBJ && a.cls_id >= 0 && sp_obj_to_h_fn) {
    sp_RbVal hh = sp_obj_to_h_fn(a);
    if (hh.tag == SP_TAG_OBJ && hh.cls_id == SP_BUILTIN_SYM_POLY_HASH) {
      sp_SymPolyHash *sh = (sp_SymPolyHash *)hh.v.p;
      sp_int n = sh->len, k = i < 0 ? n + i : i;
      if (k < 0 || k >= n) return sp_box_nil();
      return sh->vals[sh->order[k]];
    }
  }
  if (a.tag == SP_TAG_INT) return sp_box_int((a.v.i >> i) & 1);
  /* ...and a shared-string handle is a String, so it takes the same arm: it
     is a non-mutating read, and without this it fell past the arm below and
     returned nil exactly as that comment describes (#4279). */
  if (sp_poly_is_strbuf(a)) return sp_poly_arr_get_hash_cold(sp_poly_strbuf_deref(a), i);
  /* String#[int]: return the single character at i (a 1-char string), or nil
     when out of range. A String that widened to poly (e.g. a method with
     multiple return paths) reaches this generic index path; without this arm
     it fell through to sp_poly_arr_get and silently returned nil. */
  if (a.tag == SP_TAG_STR) {
    const char *s = a.v.s ? a.v.s : "";
    sp_int cl = sp_str_length(s);
    if (i < 0) i += cl;
    if (i < 0 || i >= cl) return sp_box_nil();
    return sp_box_str(sp_str_sub_range(s, i, 1));
  }
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_POLY_POLY_HASH)
    return sp_PolyPolyHash_get((sp_PolyPolyHash*)a.v.p, sp_box_int(i));
  /* These two read `i` as a symbol id (and as that id's name): an emitter that
     lowered a symbol key to an int relies on it. That makes a genuine Integer
     key on a symbol-keyed hash answer whatever symbol sits at that number
     rather than nil -- sp_poly_index_poly, which is handed the key boxed and
     can tell the two apart, resolves it before reaching here. */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_SYM_POLY_HASH)
    return sp_SymPolyHash_get((sp_SymPolyHash*)a.v.p, (sp_sym)i);
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_STR_POLY_HASH)
    return sp_StrPolyHash_get((sp_StrPolyHash*)a.v.p, sp_sym_name_fn ? sp_sym_name_fn((sp_sym)i) : "");

  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_INT_INT_HASH) {
    sp_IntIntHash *h = (sp_IntIntHash *)a.v.p;
    return sp_IntIntHash_has_key(h, i) ? sp_box_int(sp_IntIntHash_get(h, i)) : sp_box_nil();
  }
  /* The other integer-keyed variants read the same way: a nested hash whose
     keys are Integers answered nil through a boxed #[] because only the
     int->int one had an arm (#3822). */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_INT_STR_HASH) {
    sp_IntStrHash *h = (sp_IntStrHash *)a.v.p;
    return sp_IntStrHash_has_key(h, i) ? sp_box_str(sp_IntStrHash_get(h, i)) : sp_box_nil();
  }
  /* a curried Proc read out of a container: [] applies the argument,
     realizing once the accumulator reaches its count */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_CURRY)
    return sp_curry_call_poly((sp_Curry *)a.v.p, 1, (sp_RbVal[]){sp_box_int(i)});
  /* bm[arg]: a boxed bound Method called with the (single) int argument. */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_METHOD) {
    sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
    return sp_box_int(((sp_int (*)(void *, sp_int))(uintptr_t)m->fn)((void *)m->self, i));
  }
  return sp_poly_arr_get(a, i);
}
/* dig chain step: containers index, nil short-circuits, anything else (an
   Integer/String/... intermediate with keys remaining) is CRuby's TypeError
   "<Class> does not have #dig method" (#2983). */
static sp_RbVal sp_poly_dig_step(sp_RbVal a, sp_int i) {
  if (a.tag == SP_TAG_NIL) return sp_box_nil();
  if (a.tag == SP_TAG_OBJ && a.v.p &&
      (sp_poly_is_array_kind(a.cls_id) || sp_poly_is_hash_kind(a.cls_id)))
    return sp_poly_arr_get_hash(a, i);
  sp_raise_cls("TypeError",
               sp_sprintf("%s does not have #dig method", sp_poly_class_name(a)));
  return sp_box_nil();
}
/* The same step with a boxed key, so a Struct member can be named as well as
   offset: a Struct read out of a container is diggable in CRuby and was being
   refused, and a String/Symbol key reached the offset slot as a pointer
   (#3574/#3575). */
static sp_RbVal sp_poly_index_poly(sp_RbVal recv, sp_RbVal idx);
static sp_RbVal sp_poly_dig_step_key(sp_RbVal a, sp_RbVal k) {
  if (a.tag == SP_TAG_NIL) return sp_box_nil();
  if (a.tag == SP_TAG_OBJ && a.v.p &&
      (sp_poly_is_array_kind(a.cls_id) || sp_poly_is_hash_kind(a.cls_id) ||
       (a.cls_id >= 0 && sp_obj_to_h_fn &&
        !(sp_obj_is_data_fn && sp_obj_is_data_fn(a.cls_id)))))   /* Data has no #dig (#3919) */
    return sp_poly_index_poly(a, k);
  sp_raise_cls("TypeError",
               sp_sprintf("%s does not have #dig method", sp_poly_class_name(a)));
  return sp_box_nil();
}
/* dig(*keys): the key list is a runtime array, so walk it one step at a time.
   A nil at any step stops, as CRuby's #dig does. */
static sp_RbVal sp_poly_index_poly(sp_RbVal recv, sp_RbVal idx);
static sp_RbVal sp_poly_dig_list(sp_RbVal recv, sp_PolyArray *keys) {
  if (!keys) return sp_box_nil();
  SP_GC_ROOT(keys);
  sp_RbVal cur = recv;
  for (sp_int i = 0; i < keys->len; i++) {
    if (cur.tag == SP_TAG_NIL) return sp_box_nil();
    cur = sp_poly_index_poly(cur, keys->data[i]);
  }
  return cur;
}
/* poly[poly_key]: dispatch on key tag at runtime. */
static sp_RbVal sp_poly_index_poly(sp_RbVal recv, sp_RbVal idx) {
  /* a curried Proc applies its [] argument whatever the key kind -- claimed
     here, before the key-typed dispatch below coerces it to an index */
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_CURRY)
    return sp_curry_call_poly((sp_Curry *)recv.v.p, 1, &idx);
  /* Reading through a shared-string handle is non-mutating, so it answers as
     its live value: the String arms below all test SP_TAG_STR, and a handle
     fell past every one of them to the trailing nil (#4279). */
  if (sp_poly_is_strbuf(recv)) return sp_poly_index_poly(sp_poly_strbuf_deref(recv), idx);
  /* an Integer index into an array is the common read, and it matches nothing
     below until the very last line (array kinds are builtin, so the Struct arm
     with its cls_id >= 0 test cannot claim it) */
  if (idx.tag == SP_TAG_INT && recv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(recv.cls_id))
    return sp_poly_arr_get_hash(recv, idx.v.i);
  /* heterogeneous-key hash: any key kind (incl. Method) looks up directly. */
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_POLY_POLY_HASH)
    return sp_PolyPolyHash_get((sp_PolyPolyHash *)recv.v.p, idx);
  if (idx.tag == SP_TAG_STR) return sp_poly_get_str(recv, idx.v.s);
  if (idx.tag == SP_TAG_SYM) return sp_poly_get_sym(recv, (sp_sym)idx.v.i);
  /* a Range index on a poly STRING is a substring (String#[Range]); without
     this a Range fell through as i=0 and returned char 0 (#3175). */
  if (idx.tag == SP_TAG_OBJ && idx.cls_id == SP_BUILTIN_RANGE && recv.tag == SP_TAG_STR) {
    sp_Range *rg = (sp_Range *)idx.v.p;
    return sp_box_str(sp_str_sub_range_r(recv.v.s ? recv.v.s : sp_str_empty,
                                         rg->first, rg->last, (int)rg->excl));
  }
  /* the same for a poly ARRAY: a sub-array, not element 0 (#3464) */
  if (idx.tag == SP_TAG_OBJ && idx.cls_id == SP_BUILTIN_RANGE &&
      recv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(recv.cls_id))
    return sp_poly_arr_range(recv, *(sp_Range *)idx.v.p);
  sp_int i = (idx.tag == SP_TAG_INT) ? idx.v.i : 0;
  /* Struct#[n] is the nth MEMBER, in declaration order -- the order #to_h
     preserves -- not an array index (#3369). */
  if (idx.tag == SP_TAG_INT && recv.tag == SP_TAG_OBJ && recv.cls_id >= 0 && sp_obj_to_h_fn) {
    sp_RbVal hh = sp_obj_to_h_fn(recv);
    if (hh.tag == SP_TAG_OBJ && hh.cls_id == SP_BUILTIN_SYM_POLY_HASH) {
      sp_SymPolyHash *sh = (sp_SymPolyHash *)hh.v.p;
      sp_int n = sh->len;
      sp_int k = i < 0 ? n + i : i;
      if (k < 0 || k >= n) return sp_box_nil();
      return sh->vals[sh->order[k]];
    }
  }
  /* An Integer key is an Integer key. The generic read below takes a bare
     sp_int and, for a symbol- or string-keyed hash, reads it as that kind's
     key -- so `h[0]` on `{a: 1}` came back as whatever symbol 0 happens to be
     rather than nil (#3509). Those storages cannot hold an Integer key at all,
     so the answer is nil; the two that can look it up. */
  if (idx.tag == SP_TAG_INT && recv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(recv.cls_id)) {
    if (recv.cls_id == SP_BUILTIN_INT_INT_HASH) {
      sp_IntIntHash *h = (sp_IntIntHash *)recv.v.p;
      return sp_IntIntHash_has_key(h, i) ? sp_box_int(sp_IntIntHash_get(h, i)) : sp_box_nil();
    }
    if (recv.cls_id == SP_BUILTIN_INT_STR_HASH) {
      sp_IntStrHash *h = (sp_IntStrHash *)recv.v.p;
      return sp_IntStrHash_has_key(h, i) ? sp_box_str(sp_IntStrHash_get(h, i)) : sp_box_nil();
    }
    return sp_box_nil();
  }
  return sp_poly_arr_get_hash(recv, i);
}

/* Presence check for a Hash reached through a poly value, keyed by a poly key.
   The dispatch mirrors sp_poly_index_poly's storage kinds so `fetch` can tell a
   present key (return the value) from an absent one (default / KeyError) --
   sp_poly_index_poly alone returns nil on a miss, indistinguishable from a key
   legitimately mapped to nil. A key whose tag does not match the storage's key
   kind can never be present, so it reports FALSE. */
static sp_bool sp_poly_has_key(sp_RbVal recv, sp_RbVal key) {
  if (recv.tag != SP_TAG_OBJ) return FALSE;
  switch (recv.cls_id) {
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_has_key((sp_PolyPolyHash *)recv.v.p, key);
    case SP_BUILTIN_STR_POLY_HASH:  return key.tag == SP_TAG_STR && sp_StrPolyHash_has_key((sp_StrPolyHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_STR_STR_HASH:   return key.tag == SP_TAG_STR && sp_StrStrHash_has_key((sp_StrStrHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_STR_INT_HASH:   return key.tag == SP_TAG_STR && sp_StrIntHash_has_key((sp_StrIntHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_SYM_POLY_HASH:  return key.tag == SP_TAG_SYM && sp_SymPolyHash_has_key((sp_SymPolyHash *)recv.v.p, (sp_sym)key.v.i);
    case SP_BUILTIN_INT_STR_HASH:   return key.tag == SP_TAG_INT && sp_IntStrHash_has_key((sp_IntStrHash *)recv.v.p, key.v.i);
    case SP_BUILTIN_INT_INT_HASH:   return key.tag == SP_TAG_INT && sp_IntIntHash_has_key((sp_IntIntHash *)recv.v.p, key.v.i);
    default: return FALSE;
  }
}
/* `"%{name}" % hash` / `"%<name>s" % hash`: a missing key is CRuby's KeyError,
   not a nil that renders as the empty string (#3554). */
static sp_RbVal sp_fmt_hash_fetch(sp_RbVal h, sp_sym k, const char *nm) {
  if (!sp_poly_has_key(h, sp_box_sym(k)))
    sp_raise_cls("KeyError", sp_sprintf("key%s not found", nm));
  return sp_poly_index_poly(h, sp_box_sym(k));
}

/* Kind-dispatching `delete`, `dig` and `values_at` for a poly receiver. Each
   name is one a user class can own, and owning it replaces the whole dispatch
   with that class's arms -- a Hash or Array arriving at the same call then
   matched nothing and raised NoMethodError naming its own class. The receiver
   answers for itself here instead. */
static sp_RbVal sp_poly_delete_key(sp_RbVal recv, sp_RbVal key) {
  if (recv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(recv.cls_id)) {
    if (!sp_poly_has_key(recv, key)) return sp_box_nil();
    sp_RbVal was = sp_poly_index_poly(recv, key);
    switch (recv.cls_id) {
      case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_delete((sp_PolyPolyHash *)recv.v.p, key); break;
      case SP_BUILTIN_STR_POLY_HASH:  sp_StrPolyHash_delete((sp_StrPolyHash *)recv.v.p, key.v.s); break;
      case SP_BUILTIN_STR_STR_HASH:   sp_StrStrHash_delete((sp_StrStrHash *)recv.v.p, key.v.s); break;
      case SP_BUILTIN_STR_INT_HASH:   sp_StrIntHash_delete((sp_StrIntHash *)recv.v.p, key.v.s); break;
      case SP_BUILTIN_SYM_POLY_HASH:  sp_SymPolyHash_delete((sp_SymPolyHash *)recv.v.p, (sp_sym)key.v.i); break;
      case SP_BUILTIN_INT_STR_HASH:   sp_IntStrHash_delete((sp_IntStrHash *)recv.v.p, key.v.i); break;
      case SP_BUILTIN_INT_INT_HASH:   sp_IntIntHash_delete((sp_IntIntHash *)recv.v.p, key.v.i); break;
      default: return sp_box_nil();
    }
    return was;
  }
  if (recv.tag == SP_TAG_OBJ && recv.cls_id != SP_BUILTIN_POLY_ARRAY &&
      sp_poly_is_array_kind(recv.cls_id)) {
    /* A typed array has to be deleted from in place: sp_poly_to_poly_array
       COPIES one into a PolyArray, so the removal never reached the array the
       caller holds. */
    switch (recv.cls_id) {
      case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY: {
        if (key.tag != (recv.cls_id == SP_BUILTIN_INT_ARRAY ? SP_TAG_INT : SP_TAG_SYM))
          return sp_box_nil();
        sp_int r = sp_IntArray_delete((sp_IntArray *)recv.v.p, key.v.i);
        return r == SP_INT_NIL ? sp_box_nil() : key;
      }
      case SP_BUILTIN_STR_ARRAY: {
        if (key.tag != SP_TAG_STR) return sp_box_nil();
        const char *r = sp_StrArray_delete((sp_StrArray *)recv.v.p, key.v.s);
        return r ? sp_box_str(r) : sp_box_nil();
      }
      case SP_BUILTIN_FLT_ARRAY: {
        if (key.tag != SP_TAG_FLT && key.tag != SP_TAG_INT) return sp_box_nil();
        sp_float r = sp_FloatArray_delete((sp_FloatArray *)recv.v.p,
                                          key.tag == SP_TAG_FLT ? key.v.f : (sp_float)key.v.i);
        return sp_float_is_nil(r) ? sp_box_nil() : sp_box_float(r);  /* the element, not the key: -0.0 differs */
      }
      default: break;
    }
  }
  if (recv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(recv.cls_id)) {
    sp_PolyArray *a = sp_poly_to_poly_array(recv);
    sp_int w = 0, found = 0;
    for (sp_int i = 0; a && i < a->len; i++) {
      if (sp_poly_eq(a->data[i], key)) { found = 1; continue; }
      a->data[w++] = a->data[i];
    }
    if (a) a->len = w;
    return found ? key : sp_box_nil();
  }
  /* String#delete(chars): the same name on a string receiver, which reaches
     here whenever the value only turns out to be a string at run time. */
  if (recv.tag == SP_TAG_STR || sp_poly_is_strbuf(recv)) {
    sp_RbVal s = sp_poly_strbuf_deref(recv);
    if (key.tag != SP_TAG_STR) sp_raise_cls("TypeError", "no implicit conversion into String");
    return sp_box_str(sp_str_delete(s.v.s ? s.v.s : sp_str_empty, key.v.s ? key.v.s : sp_str_empty));
  }
  sp_raise_nomethod(sp_nomethod_msg("delete", recv));
  return sp_box_nil();
}
/* The kinds Ruby's #dig walks through: Array, Hash, Struct and anything the
   runtime models as one of those. A String or a number has no #dig. */
static int sp_poly_diggable(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return 0;
  if (v.cls_id >= 0 && sp_obj_is_data_fn && sp_obj_is_data_fn(v.cls_id))
    return 0;              /* Data defines no #dig (Struct does) (#3919) */
  return sp_poly_is_hash_kind(v.cls_id) || sp_poly_is_array_kind(v.cls_id) ||
         v.cls_id >= 0;   /* a user object: its own #dig answers, or NoMethodError does */
}
/* One step of a dig has landed on `v`: nil ends the walk, a container
   continues it, and anything else is the TypeError CRuby raises. */
static void sp_poly_dig_check(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL || sp_poly_diggable(v)) return;
  sp_raise_cls("TypeError", sp_sprintf("%s does not have #dig method",
                                       sp_poly_class_name(v)));
}
static sp_RbVal sp_poly_dig_n(sp_RbVal recv, sp_int n, const sp_RbVal *keys) {
  sp_RbVal cur = recv;
  for (sp_int i = 0; i < n; i++) {
    if (cur.tag == SP_TAG_NIL) return cur;
    /* a step onto something that cannot be dug is a TypeError naming the
       class, not a quiet nil: only nil short-circuits (#3567) */
    if (!sp_poly_diggable(cur))
      sp_raise_cls("TypeError", sp_sprintf("%s does not have #dig method",
                                           sp_poly_class_name(cur)));
    cur = sp_poly_index_poly(cur, keys[i]);
  }
  return cur;
}
static sp_RbVal sp_poly_values_at_n(sp_RbVal recv, sp_int n, const sp_RbVal *keys) {
  sp_PolyArray *out = sp_PolyArray_new();
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(out, sp_poly_index_poly(recv, keys[i]));
  return sp_box_poly_array(out);
}

/* Kind-dispatching `fetch` for a poly receiver. The emitted switch enumerates
   the receiver kinds someone thought to add, so a receiver of any other kind
   reached no arm and raised NoMethodError naming Hash. Here the receiver
   answers for itself: a Hash misses into the caller's default or a KeyError,
   an Array into the default or an IndexError, and anything with no `fetch`
   still says so. `has_dflt` separates `fetch(k)` from `fetch(k, nil)`, which
   are different calls. */
static sp_RbVal sp_poly_fetch(sp_RbVal recv, sp_RbVal key, int has_dflt, sp_RbVal dflt) {
  if (recv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(recv.cls_id)) {
    if (sp_poly_has_key(recv, key)) return sp_poly_index_poly(recv, key);
    if (has_dflt) return dflt;
    sp_raise_key_not_found(key);
  }
  if (recv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(recv.cls_id)) {
    sp_int n = sp_poly_length(recv), i = sp_poly_to_i(key);
    if (i < 0) i += n;
    if (i >= 0 && i < n) return sp_poly_index_poly(recv, key);
    if (has_dflt) return dflt;
    sp_raise_cls("IndexError",
                 sp_sprintf("index %lld outside of array bounds: %lld...%lld",
                            (long long)sp_poly_to_i(key), (long long)-n, (long long)n));
  }
  sp_raise_nomethod(sp_nomethod_msg("fetch", recv));
  return sp_box_nil();
}

/* Integer-returning counterpart of sp_poly_index_poly for `poly[int]` where
   the poly element holds an int-returning callable/container -- a bound
   method (called with the int arg, int ABI) or an int array. Used when
   inference proves the double index `@table[i][j]` yields an int (a method
   dispatch table). Falls back to coercing the generic poly result. */
/* frozen? on a poly value: scalars (int/float/sym/bool/nil/bigint) are always
   frozen in Ruby; a string checks its frozen flag; a heap object checks its GC
   header bit. Used when a receiver widened to poly under promote. */
/* Object#dup / #clone on a boxed value. A plain user object (cls_id >= 0)
   or a bare Object (SP_BUILTIN_OBJECT) shallow-copies through its GC header
   (fresh allocation, payload memcpy: ivars copy as references, matching the
   typed dup arm). Containers and value tags return as-is -- their memcpy
   would alias the backing store (and double-free through the finalizer);
   they keep their dedicated copy paths. clone preserves the frozen bit. */
static sp_RbVal sp_poly_dup(sp_RbVal v, int keep_frozen) {
  /* Array#dup/#clone on a boxed array (read out of a poly container): a shallow
     copy of the same kind. A raw struct memcpy (the user-object path below)
     would share the element buffer, so the copy would alias -- mutating it
     would corrupt the original. */
  if (v.tag == SP_TAG_OBJ && v.v.p && sp_poly_is_array_kind(v.cls_id)) {
    switch (v.cls_id) {
      case SP_BUILTIN_POLY_ARRAY: v.v.p = sp_PolyArray_dup((sp_PolyArray *)v.v.p); break;
      case SP_BUILTIN_INT_ARRAY: {
        sp_IntArray *a = (sp_IntArray *)v.v.p; SP_GC_ROOT(a);
        sp_IntArray *r = sp_IntArray_new();
        for (sp_int i = 0; i < a->len; i++) sp_IntArray_push(r, a->data[a->start + i]);
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
      case SP_BUILTIN_STR_ARRAY: {
        sp_StrArray *a = (sp_StrArray *)v.v.p; SP_GC_ROOT(a);
        sp_StrArray *r = sp_StrArray_new();
        for (sp_int i = 0; i < a->len; i++) sp_StrArray_push(r, a->data[i]);
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p; SP_GC_ROOT(a);
        sp_FloatArray *r = sp_FloatArray_new();
        for (sp_int i = 0; i < a->len; i++) sp_FloatArray_push(r, a->data[i]);
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
    }
    return v;
  }
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT)) {
    sp_gc_hdr *h = (sp_gc_hdr *)((char *)v.v.p - sizeof(sp_gc_hdr));
    size_t payload = h->size - sizeof(sp_gc_hdr);
    void *src = v.v.p;
    SP_GC_ROOT(src);
    void *n = sp_gc_alloc(payload, h->finalize, h->scan);
    memcpy(n, src, payload);
    if (keep_frozen && h->frozen)
      ((sp_gc_hdr *)((char *)n - sizeof(sp_gc_hdr)))->frozen = 1;
    v.v.p = n;
  }
  return v;
}
/* clone(freeze: ...) on a boxed value. An immutable immediate (nil/bool/int/
   float/sym/bigint) can't be unfrozen, so `freeze: false` raises ArgumentError
   like CRuby; `freeze: true`/nil returns it (already frozen). A mutable value
   copies, keeping the frozen flag only when freeze isn't explicitly false.
   fz: 0 = false, 1 = true, -1 = nil/default (#3033). */
static inline sp_RbVal sp_poly_freeze(sp_RbVal v);  /* fwd */
static inline sp_RbVal sp_poly_clone_freeze(sp_RbVal v, int fz) {
  int immutable = v.tag != SP_TAG_OBJ && v.tag != SP_TAG_STR;
  if (immutable) {
    if (fz == 0)
      sp_raise_cls("ArgumentError", sp_sprintf("can't unfreeze %s", sp_poly_class_name(v)));
    return v;
  }
  sp_RbVal r = sp_poly_dup(v, fz != 0);
  if (fz == 1) return sp_poly_freeze(r);
  return r;
}
/* Object#freeze on a boxed value: heap objects flip the GC-header bit
   (sp_gc_is_frozen reports it back), heap strings flip the string marker,
   every immutable tag (int/float/sym/nil/bool/...) is already frozen. */
static inline sp_RbVal sp_poly_freeze(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) sp_gc_freeze(v.v.p);
  else if (v.tag == SP_TAG_STR && v.v.s) sp_str_freeze_val(v.v.s);
  return v;
}
static inline sp_bool sp_poly_frozen(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_is_frozen_val(v.v.s) : TRUE;
  if (v.tag == SP_TAG_OBJ) return sp_gc_is_frozen(v.v.p);
  return TRUE;
}
/* eql? for a poly value: like == but without cross-kind numeric coercion, so
   1.eql?(1.0) is false while 1 == 1.0 is true. Every other type answers as ==.
   Backs the universal `x.should.eql?(y)` matcher on a poly receiver. */
static sp_bool sp_poly_eql(sp_RbVal a, sp_RbVal b) {
  int a_int = (a.tag == SP_TAG_INT || a.tag == SP_TAG_BIGINT);
  int b_int = (b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT);
  if ((a_int && b.tag == SP_TAG_FLT) || (a.tag == SP_TAG_FLT && b_int)) return FALSE;
  /* Array#eql? recurses per element with eql? (not ==), so [1, 2] is not
     eql? to [1, 2.0] even though they are ==. */
  if (a.tag == SP_TAG_OBJ && b.tag == SP_TAG_OBJ &&
      sp_poly_is_array_kind(a.cls_id) && sp_poly_is_array_kind(b.cls_id)) {
    if (a.v.p == b.v.p) return TRUE;  /* same object: eql? to itself (O(1)) */
    sp_int n = sp_poly_length(a);
    if (n != sp_poly_length(b)) return FALSE;
    /* Same pair guard as ==, and the same kind of frame: both answer TRUE for a
       pair they are already inside, so a nest that mixes the two still ends. */
    if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) return TRUE;
    int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
    sp_bool r = TRUE;
    for (sp_int i = 0; r && i < n; i++)
      r = sp_poly_eql(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i));
    sp_poly_recur_pop(mark);
    return r;
  }
  /* Two user objects of the same class: eql? (used by uniq / Set / Hash keys,
     unlike ==) honors a user-defined eql? via the cls_id hook. Identity is the
     default when the class defines none (the hook returns FALSE for such a
     cls_id, matching BasicObject#eql?). User class ids are non-negative; builtin
     value objects (Rational/Complex/Time/...) keep sp_poly_eq's own handling. */
  if (a.tag == SP_TAG_OBJ && b.tag == SP_TAG_OBJ && a.cls_id == b.cls_id &&
      a.cls_id >= 0) {
    if (a.v.p == b.v.p) return TRUE;
    if (sp_obj_eql_hook) {
      /* a Struct that holds itself reaches its own eql? through the hook */
      if (sp_poly_recur_seen(SP_POLY_RECUR_EQ, a.v.p, b.v.p)) return TRUE;
      int mark = sp_poly_recur_push(SP_POLY_RECUR_EQ, a.v.p, b.v.p);
      sp_bool r = sp_obj_eql_hook(a.cls_id, a.v.p, b.v.p);
      sp_poly_recur_pop(mark);
      return r;
    }
    /* No eql? hook (so no Struct/Data value key exists to need field-wise eql?):
       a user object's eql? is identity, NOT its `==`. Falling through to
       sp_poly_eq would pick up a class's user-defined `==` (which uniq / Set /
       Hash keys must not use), so a class defining only `==` would wrongly
       dedupe here (#2884). */
    return FALSE;
  }
  /* Range#eql? compares endpoints with eql?, so an Integer bound is not eql?
     to the same Float bound. A mixed literal like (0..1.0) is deliberately
     carried on the integer representation (see the range literal rule in the
     analyzer), which cannot tell that pair apart, so an integer Range answers
     eql? only for itself; a String or Float Range has no such ambiguity and
     compares by value through ==. */
  if (a.tag == SP_TAG_OBJ && b.tag == SP_TAG_OBJ &&
      a.cls_id == SP_BUILTIN_RANGE && b.cls_id == SP_BUILTIN_RANGE)
    return a.v.p == b.v.p;
  return sp_poly_eq(a, b);
}
/* equal? for a poly value: object identity. Immediates (int, symbol, nil,
   bool, flonum) are their own identity by value; everything heap-backed
   (string buffer, boxed object, bignum) compares by pointer. */
static sp_bool sp_poly_equal(sp_RbVal a, sp_RbVal b) {
  if (a.tag != b.tag) return FALSE;
  switch (a.tag) {
    case SP_TAG_INT: return a.v.i == b.v.i;
    case SP_TAG_SYM: return a.v.i == b.v.i;
    case SP_TAG_BOOL: return a.v.b == b.v.b;
    case SP_TAG_NIL: return TRUE;
    case SP_TAG_FLT: return a.v.f == b.v.f;
    case SP_TAG_STR: return a.v.s == b.v.s;
    /* Class objects are their own identity: two references to Object are the
       same object. Spinel's classes are name-backed, so compare by name. */
    case SP_TAG_CLASS: { const char *an = sp_class_val_name(a), *bn = sp_class_val_name(b);
                         return (an && bn) ? strcmp(an, bn) == 0 : an == bn; }
    default: return a.v.p == b.v.p;
  }
}
/* is_a?/kind_of? for a poly value against a BUILTIN class named `cn`. The
   caller (codegen) routes here only when `cn` is a known builtin; a user-class
   target is resolved inline via sp_class_le on the boxed object's cls_id. */
static sp_int sp_exc_is_a(volatile struct sp_Exception_s *ve, const char *cn);  /* fwd (#3096) */
static sp_bool sp_poly_kind_of_builtin(sp_RbVal v, const char *cn) {
  if (!cn) return FALSE;
  if (strcmp(cn, "Object") == 0 || strcmp(cn, "BasicObject") == 0 || strcmp(cn, "Kernel") == 0)
    return TRUE;
  if (strcmp(sp_poly_class_name(v), cn) == 0) return TRUE;  /* exact builtin class */
  /* a boxed IO handle walks its own kind chain (a socket read back out of a
     poly array must still answer BasicSocket / IO) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_IO)
    return sp_io_is_a((sp_File *)v.v.p, cn);
  int is_int = (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT);
  int is_flt = (v.tag == SP_TAG_FLT);
  int is_rat = (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL);
  int is_cpx = (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX);
  int is_arr = (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id));
  int is_range = (v.tag == SP_TAG_OBJ && (v.cls_id == SP_BUILTIN_RANGE ||
                                          v.cls_id == SP_BUILTIN_FLOAT_RANGE ||
                                          v.cls_id == SP_BUILTIN_STR_RANGE));
  int is_hash = (v.tag == SP_TAG_OBJ &&
                 (v.cls_id == SP_BUILTIN_POLY_POLY_HASH || v.cls_id == SP_BUILTIN_SYM_POLY_HASH ||
                  v.cls_id == SP_BUILTIN_STR_POLY_HASH || v.cls_id == SP_BUILTIN_STR_STR_HASH ||
                  v.cls_id == SP_BUILTIN_STR_INT_HASH || v.cls_id == SP_BUILTIN_INT_STR_HASH || v.cls_id == SP_BUILTIN_INT_INT_HASH));
  if (strcmp(cn, "Numeric") == 0) return is_int || is_flt || is_rat || is_cpx;
  if (strcmp(cn, "Integer") == 0) return is_int;
  if (strcmp(cn, "Float") == 0) return is_flt;
  if (strcmp(cn, "Comparable") == 0) return is_int || is_flt || is_rat ||
                                             v.tag == SP_TAG_STR || v.tag == SP_TAG_SYM;
  if (strcmp(cn, "Enumerable") == 0) return is_arr || is_range || is_hash;
  /* a boxed exception (e.g. rescued into a poly-union local) walks the
     exception hierarchy: StopIteration is_a? StandardError etc. (#3096) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_EXCEPTION && v.v.p)
    return (sp_bool)sp_exc_is_a((volatile struct sp_Exception_s *)v.v.p, cn);
  return FALSE;
}
/* is_a?/instance_of? for a poly value against a RUNTIME class value `cls` (a
   method param or other non-literal). The class's name drives the check: exact
   name match for instance_of? (and same-class is_a?), plus the builtin-ancestry
   table for is_a?. A dynamic user-class ancestor is out of reach here, so this
   under-reports at worst (never a false positive) -- the safe direction. */
static sp_bool sp_poly_is_a_dyn(sp_RbVal v, sp_RbVal cls, int exact) {
  const char *cn = sp_poly_to_s(cls);
  if (!cn) return FALSE;
  if (strcmp(sp_poly_class_name(v), cn) == 0) return TRUE;
  return exact ? FALSE : sp_poly_kind_of_builtin(v, cn);
}
static inline sp_int sp_poly_index_int(sp_RbVal a, sp_int i) {
  if (a.tag == SP_TAG_INT) return (a.v.i >> i) & 1;
  if (a.tag == SP_TAG_OBJ) {
    if (a.cls_id == SP_BUILTIN_METHOD) {
      sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
      /* promote: methods are poly-signatured, so invoke through the poly ABI
         and unbox the result rather than the legacy sp_int ABI. */
      return sp_poly_to_i(((sp_RbVal (*)(void *, sp_RbVal))(uintptr_t)m->fn)((void *)m->self, sp_box_int(i)));
#else
      return ((sp_int (*)(void *, sp_int))(uintptr_t)m->fn)((void *)m->self, i);
#endif
    }
    if (a.cls_id == SP_BUILTIN_INT_ARRAY) return sp_IntArray_get((sp_IntArray *)a.v.p, i);
  }
  return sp_poly_to_i(sp_poly_arr_get_hash(a, i));
}
static sp_RbVal sp_poly_arr_set_hash(sp_RbVal v, sp_int idx, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  sp_IntArray_set((sp_IntArray*)v.v.p, idx,
                                                val.tag == SP_TAG_INT ? val.v.i : (sp_int)val.v.f); break;
    case SP_BUILTIN_FLT_ARRAY:  sp_FloatArray_set((sp_FloatArray*)v.v.p, idx,
                                                   val.tag == SP_TAG_FLT ? val.v.f : (sp_float)val.v.i); break;
    case SP_BUILTIN_STR_ARRAY:  sp_StrArray_set((sp_StrArray*)v.v.p, idx,
                                                 val.tag == SP_TAG_STR ? val.v.s : NULL); break;
    case SP_BUILTIN_POLY_ARRAY: {
      sp_PolyArray *_pa = (sp_PolyArray*)v.v.p;
      if (_pa && !_pa->frozen) {
        sp_gc_wb((void*)_pa);
        while (_pa->len <= idx) sp_PolyArray_push(_pa, sp_box_nil());
        _pa->data[idx] = val;
      }
      break;
    }
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_int(idx), val); break;
    default: break;
  }
  return val;
}
/* poly_val[str_key] = val: runtime dispatch for poly recv `[]=` with string key. */
static sp_RbVal sp_poly_set_str(sp_RbVal v, const char *key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  /* An Array indexed by a String is a TypeError, not a write to be dropped:
     the static path raises it, and a boxed receiver reaching the same call
     used to absorb the assignment instead (#3925). */
  if (sp_poly_is_array_kind(v.cls_id))
    sp_raise_cls("TypeError", SPL("no implicit conversion of String into Integer"));
  switch (v.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH: sp_StrPolyHash_set((sp_StrPolyHash*)v.v.p, key, val); break;
    case SP_BUILTIN_STR_STR_HASH:
      if (val.tag == SP_TAG_STR) { sp_StrStrHash_set((sp_StrStrHash*)v.v.p, key, val.v.s); } break;
    case SP_BUILTIN_STR_INT_HASH:
      if (val.tag == SP_TAG_INT) { sp_StrIntHash_set((sp_StrIntHash*)v.v.p, key, val.v.i); } break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_str(key), val); break;
    default: break;
  }
  return val;
}
/* Merge every pair of one boxed hash into another, whatever variants the two
   hold: the destination's variant decides how each key is stored, and a key
   it cannot represent is skipped rather than mistyped. Backs the splatted
   `h.merge!(*hs)`, where the sources are only known at run time (#3848). */
static void sp_poly_hash_merge_into(sp_RbVal dst, sp_RbVal src) {
  if (dst.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(dst.cls_id)) return;
  if (src.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(src.cls_id)) return;
  sp_int n = sp_poly_length(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(src, i, &k, &v);
    switch (dst.cls_id) {
      case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash *)dst.v.p, k, v); break;
      case SP_BUILTIN_SYM_POLY_HASH:
        if (k.tag == SP_TAG_SYM) sp_SymPolyHash_set((sp_SymPolyHash *)dst.v.p, (sp_sym)k.v.i, v);
        break;
      case SP_BUILTIN_STR_POLY_HASH:
        if (k.tag == SP_TAG_STR) sp_StrPolyHash_set((sp_StrPolyHash *)dst.v.p, k.v.s, v);
        break;
      case SP_BUILTIN_STR_STR_HASH:
        if (k.tag == SP_TAG_STR && v.tag == SP_TAG_STR)
          sp_StrStrHash_set((sp_StrStrHash *)dst.v.p, k.v.s, v.v.s);
        break;
      case SP_BUILTIN_STR_INT_HASH:
        if (k.tag == SP_TAG_STR && v.tag == SP_TAG_INT)
          sp_StrIntHash_set((sp_StrIntHash *)dst.v.p, k.v.s, v.v.i);
        break;
      case SP_BUILTIN_INT_INT_HASH:
        if (k.tag == SP_TAG_INT && v.tag == SP_TAG_INT)
          sp_IntIntHash_set((sp_IntIntHash *)dst.v.p, k.v.i, v.v.i);
        break;
      case SP_BUILTIN_INT_STR_HASH:
        if (k.tag == SP_TAG_INT && v.tag == SP_TAG_STR)
          sp_IntStrHash_set((sp_IntStrHash *)dst.v.p, k.v.i, v.v.s);
        break;
      default: break;
    }
  }
}
/* poly_val[sym_key] = val: runtime dispatch for poly recv `[]=` with symbol key. */
static sp_RbVal sp_poly_set_sym(sp_RbVal v, sp_sym key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  if (sp_poly_is_array_kind(v.cls_id))
    sp_raise_cls("TypeError", SPL("no implicit conversion of Symbol into Integer"));
  switch (v.cls_id) {
    case SP_BUILTIN_SYM_POLY_HASH:  sp_SymPolyHash_set((sp_SymPolyHash*)v.v.p, key, val); break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_sym(key), val); break;
    /* OpenStruct#[]= (`os[:name] = v`) routes to the member table, matching the
       sp_OpenStruct_get reader (without this the poly-dispatch write was dropped
       and the reader kept the old value) (#3201). */
    case SP_BUILTIN_OPENSTRUCT:     sp_OpenStruct_set((sp_OpenStruct*)v.v.p, key, val); break;
    default: break;
  }
  return val;
}
/* poly_val[int_idx] = val: runtime dispatch for poly recv `[]=` with int index. */
static sp_RbVal sp_poly_arr_set(sp_RbVal v, sp_int idx, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  sp_IntArray_set((sp_IntArray*)v.v.p, idx,
                                                val.tag == SP_TAG_INT ? val.v.i : (sp_int)val.v.f); break;
    case SP_BUILTIN_FLT_ARRAY:  sp_FloatArray_set((sp_FloatArray*)v.v.p, idx,
                                                   val.tag == SP_TAG_FLT ? val.v.f : (sp_float)val.v.i); break;
    case SP_BUILTIN_STR_ARRAY:  sp_StrArray_set((sp_StrArray*)v.v.p, idx,
                                                 val.tag == SP_TAG_STR ? val.v.s : NULL); break;
    case SP_BUILTIN_POLY_ARRAY: sp_PolyArray_set((sp_PolyArray*)v.v.p, idx, val); break;
    default: break;
  }
  return val;
}
static sp_RbVal sp_poly_set_poly(sp_RbVal v, sp_RbVal key, sp_RbVal val);   /* fwd: hash []= from widen_and_set */
/* Like sp_poly_arr_set but widens a typed array to a PolyArray when val does not
   match its element kind (int<-non-int incl. float, flt<-non-float, str<-non-str),
   so the value is stored exactly as CRuby does (e.g. a Float into a former int
   array). Returns the (possibly new) boxed array so the caller updates the slot. */
static sp_RbVal sp_poly_arr_widen_and_set(sp_RbVal v, sp_int idx, sp_RbVal val) {
  if (v.tag == SP_TAG_OBJ &&
      ((v.cls_id == SP_BUILTIN_INT_ARRAY && val.tag != SP_TAG_INT) ||
       (v.cls_id == SP_BUILTIN_FLT_ARRAY && val.tag != SP_TAG_FLT) ||
       (v.cls_id == SP_BUILTIN_STR_ARRAY && val.tag != SP_TAG_STR))) {
    if (sp_typed_arr_frozen(v)) sp_raise_frozen_array_v(v);
    SP_GC_ROOT_RBVAL(val);
    sp_PolyArray *pa = sp_poly_to_poly_array(v);
    SP_GC_ROOT(pa);
    sp_PolyArray_set(pa, idx, val);
    return sp_box_poly_array(pa);
  }
  /* `h[i] = v` on a poly value that is actually a HASH (a param widened to poly
     from an empty `{}`): an integer key is a hash key, not an array index. Set
     it in place through the hash so the mutation lands on the caller's hash
     (sp_poly_arr_set treats a hash as no-op) (#2871). */
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) {
    sp_poly_set_poly(v, sp_box_int(idx), val);
    return v;
  }
  /* `s[i] = v` on a poly value that is actually a STRING (a param widened to
     poly by another call site): String#[]= replaces the char at i with the
     value string. Spinel strings splice to a fresh buffer, so return it for the
     caller to reassign to the poly slot (sp_poly_arr_set is a no-op on a
     string, silently dropping the mutation) (#3172). */
  if (v.tag == SP_TAG_STR) {
    const char *rep = (val.tag == SP_TAG_STR) ? (val.v.s ? val.v.s : sp_str_empty)
                                              : sp_poly_to_s(val);
    return sp_box_str(sp_str_splice_at(v.v.s ? v.v.s : sp_str_empty, idx, 1, rep, 0));
  }
  /* the same on a SHARED string: the handle absorbs the spliced contents, so
     the write lands on the container's own element rather than on a fresh box
     the caller would have to store back -- which an element receiver has no
     slot for, and the mutation was lost or refused (#3940). */
  if (sp_poly_is_strbuf(v)) {
    sp_String *sh = (sp_String *)v.v.p;
    const char *rep = (val.tag == SP_TAG_STR) ? (val.v.s ? val.v.s : sp_str_empty)
                                              : sp_poly_to_s(val);
    SP_GC_ROOT_RBVAL(v);
    SP_GC_ROOT(rep);
    sp_String_set_bin(sh, sp_str_splice_at(sp_String_cstr(sh), idx, 1, rep, 0));
    return v;
  }
  sp_poly_arr_set(v, idx, val);
  return v;
}
/* poly_val[poly_key] = val: fully dynamic dispatch for poly recv + poly key. */
static sp_RbVal sp_poly_set_poly(sp_RbVal v, sp_RbVal key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  /* Every array arm below wants an integer index. A Float converts through
     #to_int, and anything else is the TypeError the static path raises rather
     than a write to drop on the floor (#3926). */
  if (sp_poly_is_array_kind(v.cls_id)) {
    if (key.tag == SP_TAG_FLT) key = sp_box_int((sp_int)key.v.f);
    else if (key.tag != SP_TAG_INT)
      sp_raise_cls("TypeError", key.tag == SP_TAG_NIL
                   ? SPL("no implicit conversion from nil to integer")
                   : sp_sprintf("no implicit conversion of %s into Integer",
                                sp_poly_class_name(key)));
  }
  switch (v.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH:
      if (key.tag == SP_TAG_STR) sp_StrPolyHash_set((sp_StrPolyHash*)v.v.p, key.v.s, val);
      break;
    case SP_BUILTIN_STR_STR_HASH:
      if (key.tag == SP_TAG_STR && val.tag == SP_TAG_STR)
        sp_StrStrHash_set((sp_StrStrHash*)v.v.p, key.v.s, val.v.s);
      break;
    case SP_BUILTIN_STR_INT_HASH:
      if (key.tag == SP_TAG_STR && val.tag == SP_TAG_INT)
        sp_StrIntHash_set((sp_StrIntHash*)v.v.p, key.v.s, val.v.i);
      break;
    case SP_BUILTIN_INT_INT_HASH:
      if (key.tag == SP_TAG_INT && val.tag == SP_TAG_INT)
        sp_IntIntHash_set((sp_IntIntHash*)v.v.p, key.v.i, val.v.i);
      break;
    case SP_BUILTIN_INT_STR_HASH:
      if (key.tag == SP_TAG_INT && val.tag == SP_TAG_STR)
        sp_IntStrHash_set((sp_IntStrHash*)v.v.p, key.v.i, val.v.s);
      break;
    case SP_BUILTIN_SYM_POLY_HASH:
      if (key.tag == SP_TAG_SYM) sp_SymPolyHash_set((sp_SymPolyHash*)v.v.p, (sp_sym)key.v.i, val);
      break;
    case SP_BUILTIN_INT_ARRAY:
      if (key.tag == SP_TAG_INT) sp_IntArray_set((sp_IntArray*)v.v.p, key.v.i,
                                                  val.tag == SP_TAG_INT ? val.v.i : (sp_int)val.v.f);
      break;
    case SP_BUILTIN_POLY_ARRAY:
      if (key.tag == SP_TAG_INT) sp_PolyArray_set((sp_PolyArray*)v.v.p, key.v.i, val);
      break;
    /* Str/Float arrays were missing here, so `grid[r][c] = v` on a boxed inner
       Str/Float array (a nested write reached through a poly index) silently
       dropped the assignment. */
    case SP_BUILTIN_STR_ARRAY:
      if (key.tag == SP_TAG_INT) sp_StrArray_set((sp_StrArray*)v.v.p, key.v.i,
                                                  val.tag == SP_TAG_STR ? val.v.s : NULL);
      break;
    case SP_BUILTIN_FLT_ARRAY:
      if (key.tag == SP_TAG_INT) sp_FloatArray_set((sp_FloatArray*)v.v.p, key.v.i,
                                                    val.tag == SP_TAG_FLT ? val.v.f : (sp_float)val.v.i);
      break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, key, val); break;
    default: break;
  }
  return val;
}

/* The multi-set forms of String#count/#delete/#squeeze, through a value only
   known at run time: the typed receiver resolves to sp_str_*_n, and a boxed
   String answers the same. Anything that is not a string raises the
   NoMethodError the unresolved-call gate raised, receiver named (#4195, the
   multi-argument rows of #4149's table). op: 0 count, 1 delete, 2 squeeze. */
static sp_RbVal sp_poly_str_setop_n(sp_RbVal v, sp_int op, sp_int n, sp_RbVal *args) {
  const char *nm = op == 0 ? "count" : op == 1 ? "delete" : "squeeze";
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v)) {
    /* the operands arrive in the call site's rooted temps; only the
       receiver needs a root of its own across the helper's allocations */
    SP_GC_ROOT_RBVAL(v);
    sp_RbVal sv = sp_poly_is_strbuf(v) ? sp_poly_strbuf_deref(v) : v;
    const char *s = sv.v.s ? sv.v.s : sp_str_empty;
    const char **sets = (const char **)malloc(sizeof(char *) * (size_t)(n > 0 ? n : 1));
    for (sp_int i = 0; i < n; i++) {
      sp_RbVal a = sp_poly_is_strbuf(args[i]) ? sp_poly_strbuf_deref(args[i]) : args[i];
      if (a.tag != SP_TAG_STR) {
        free(sets);
        sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into String",
                                             sp_poly_class_name(args[i])));
      }
      sets[i] = a.v.s ? a.v.s : sp_str_empty;
    }
    sp_RbVal r;
    if (op == 0) r = sp_box_int(sp_str_count_n(s, sets, n));
    else if (op == 1) r = sp_box_str(sp_str_delete_n(s, sets, n));
    else r = sp_box_str(sp_str_squeeze_n(s, sets, n));
    free(sets);
    return r;
  }
  sp_raise_nomethod(sp_nomethod_msg_args(nm, v, n, args));
  return sp_box_nil();
}
/* Hash#store, the method form of []=, through a value only known at run
   time (#4195). Answers the value, as CRuby does. */
static sp_RbVal sp_poly_store(sp_RbVal v, sp_RbVal k, sp_RbVal val) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) {
    if (sp_gc_is_frozen(v.v.p)) sp_raise_frozen_hash_at(v.v.p, v.cls_id);
    SP_GC_ROOT_RBVAL(v); SP_GC_ROOT_RBVAL(k); SP_GC_ROOT_RBVAL(val);
    sp_poly_set_poly(v, k, val);
    return val;
  }
  sp_raise_nomethod(sp_nomethod_msg_args("store", v, 2, (sp_RbVal[]){k, val}));
  return sp_box_nil();
}
/* `outer[oidx][start,len] = src` where the splice receiver is itself an index
   expression: read the inner array, promoting-splice it, and store the possibly
   promoted result back into outer's slot so a typed->poly promotion survives.
   outer is a POLY_ARRAY for an array-of-arrays; sp_poly_set_poly also covers a
   hash outer. Codegen yields the rhs separately, so the return is advisory. */
/* The element at outer[oidx], read the way the container itself indexes: an
   integer names a KEY in a hash, not a position, and reading one as a position
   answered nil -- the write-back below then replaced the value with it (#3940). */
static sp_RbVal sp_poly_slot_inner(sp_RbVal outer, sp_int oidx) {
  if (outer.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(outer.cls_id))
    return sp_poly_index_poly(outer, sp_box_int(oidx));
  return sp_poly_arr_get(outer, oidx);
}
static sp_RbVal sp_poly_slot_splice(sp_RbVal outer, sp_int oidx, sp_int start, sp_int len, sp_RbVal src) {
  sp_RbVal inner = sp_poly_slot_inner(outer, oidx);
  sp_RbVal res = sp_poly_splice(inner, start, len, src);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return res;
}
static sp_RbVal sp_poly_slot_splice_range(sp_RbVal outer, sp_int oidx, sp_Range r, sp_RbVal src) {
  sp_RbVal inner = sp_poly_slot_inner(outer, oidx);
  sp_RbVal res = sp_poly_splice_range(inner, r, src);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return res;
}
/* `outer[oidx][ikey] = val` single-index assign through an index-expression
   receiver: read inner, widen-and-set (promoting on element-kind mismatch), and
   store the possibly promoted result back into outer's slot. No GC root spans
   the calls: outer stays live via the caller's rooted local, and src/val are
   rooted inside the callee only where an allocation (promotion) happens, after
   every raise condition has been checked. */
static sp_RbVal sp_poly_slot_set(sp_RbVal outer, sp_int oidx, sp_int ikey, sp_RbVal val) {
  sp_RbVal inner = sp_poly_slot_inner(outer, oidx);
  /* A String inner splices into a FRESH buffer, so the result has to be stored
     back; widen_and_set has no string arm and dropped the write (#4067). */
  if (inner.tag == SP_TAG_STR || sp_poly_is_strbuf(inner)) {
    sp_RbVal sres = sp_poly_splice(inner, ikey, 1, val);
    sp_poly_set_poly(outer, sp_box_int(oidx), sres);
    return val;
  }
  sp_RbVal res = sp_poly_arr_widen_and_set(inner, ikey, val);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return val;
}
/* `outer[oidx][key] = val` where the KEY is boxed: the same store-back as
   sp_poly_slot_set, for an index whose static type stayed poly (a destructured
   block parameter, an element read). Without it the write went straight to
   sp_poly_set_poly on the inner value, which has no String arm at all, so a
   character assignment into a String held in a container was silently dropped
   (#4067). A container inner mutates in place and needs no store-back, but
   going through the same helper keeps one rule rather than two. */
static sp_RbVal sp_poly_slot_set_key(sp_RbVal outer, sp_int oidx, sp_RbVal key, sp_RbVal val) {
  sp_RbVal inner = sp_poly_slot_inner(outer, oidx);
  if (inner.tag == SP_TAG_STR || sp_poly_is_strbuf(inner)) {
    /* the index of a character assignment is an Integer, and anything else is
       the TypeError the typed path raises rather than a write to drop */
    if (key.tag == SP_TAG_FLT) key = sp_box_int((sp_int)key.v.f);
    else if (key.tag != SP_TAG_INT)
      sp_raise_cls("TypeError", key.tag == SP_TAG_NIL
                   ? SPL("no implicit conversion from nil to integer")
                   : sp_sprintf("no implicit conversion of %s into Integer",
                                sp_poly_class_name(key)));
    sp_RbVal sres = sp_poly_splice(inner, key.v.i, 1, val);
    sp_poly_set_poly(outer, sp_box_int(oidx), sres);
    return val;
  }
  return sp_poly_set_poly(inner, key, val);
}
/* Hash#compare_by_identity? for a poly-carried receiver: spinel hashes are
   always value-keyed (the mutating variant is a compile error), so any hash
   answers false; anything else raises CRuby's NoMethodError. */
sp_bool sp_poly_cbi_p(sp_RbVal v) __attribute__((unused));
/* sp_poly_cbi_p: moved to lib/sp_cold.c */
sp_bool sp_poly_cbi_p(sp_RbVal v);
/* boxed-array count(v): value-equality element count (0 for non-arrays) */
/* String#index / #rindex on a boxed receiver: the byte offset of a substring,
   SP_INT_NIL when absent. The receiver switch that serves the array kinds
   dispatches on cls_id, which a String box does not carry, so its default arm
   routes here before reporting a missing method (#3445). */
static sp_int sp_poly_str_index_val(sp_RbVal v, sp_RbVal sub, int from_end) {
  sp_RbVal s = sp_poly_strbuf_deref(v), a = sp_poly_strbuf_deref(sub);
  if (a.tag != SP_TAG_STR) return SP_INT_NIL;
  const char *sp = s.v.s ? s.v.s : (&("\xff")[1]);
  const char *ap = a.v.s ? a.v.s : (&("\xff")[1]);
  return from_end ? sp_str_rindex_opt(sp, ap) : sp_str_index_opt(sp, ap);
}
/* the two-argument String#index/#rindex on a boxed receiver: the search starts
   at (index) or stops at (rindex) the given offset. Array owns neither form --
   its index takes one argument -- so this arm is String's alone (#4149). */
static sp_int sp_poly_str_index_from_val(sp_RbVal v, sp_RbVal sub, sp_int start,
                                         int from_end) {
  sp_RbVal s = sp_poly_strbuf_deref(v), a = sp_poly_strbuf_deref(sub);
  if (a.tag != SP_TAG_STR) return SP_INT_NIL;
  const char *sp = s.v.s ? s.v.s : (&("\xff")[1]);
  const char *ap = a.v.s ? a.v.s : (&("\xff")[1]);
  if (!from_end) return sp_str_index_from_opt(sp, ap, start);
  { sp_int n = sp_str_rindex_from(sp, ap, start); return n < 0 ? SP_INT_NIL : n; }
}
static sp_int sp_poly_count_val(sp_RbVal v, sp_RbVal x) {
  /* String#count on a boxed receiver counts characters from a set, not
     elements; without this arm it fell through the array test and answered 0
     for every argument (#3446). */
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v)) {
    sp_RbVal s = sp_poly_strbuf_deref(v), a = sp_poly_strbuf_deref(x);
    if (a.tag != SP_TAG_STR) return 0;
    return sp_str_count(s.v.s ? s.v.s : (&("\xff")[1]), a.v.s ? a.v.s : (&("\xff")[1]));
  }
  if (v.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(v.cls_id)) return 0;
  sp_int n = sp_poly_length(v), cnt = 0;
  for (sp_int i = 0; i < n; i++) if (sp_poly_eq(sp_poly_arr_get(v, i), x)) cnt++;
  return cnt;
}
static sp_int sp_poly_length(sp_RbVal v){if(v.tag==SP_TAG_STR)return v.v.s?(sp_int)sp_str_byte_len(v.v.s):0;   /* the header length, so an embedded NUL counts (#3540) */if(v.tag==SP_TAG_SYM)return sp_sym_name_fn?(sp_int)strlen(sp_sym_name_fn((sp_sym)v.v.i)):0;if(v.tag!=SP_TAG_OBJ)return 0;switch(v.cls_id){case SP_BUILTIN_INT_ARRAY:return sp_IntArray_length((sp_IntArray*)v.v.p);case SP_BUILTIN_FLT_ARRAY:return sp_FloatArray_length((sp_FloatArray*)v.v.p);case SP_BUILTIN_STR_ARRAY:return sp_StrArray_length((sp_StrArray*)v.v.p);case SP_BUILTIN_SYM_ARRAY:return sp_IntArray_length((sp_IntArray*)v.v.p);case SP_BUILTIN_POLY_ARRAY:return sp_PolyArray_length((sp_PolyArray*)v.v.p);case SP_BUILTIN_STR_INT_HASH:return sp_StrIntHash_length((sp_StrIntHash*)v.v.p);case SP_BUILTIN_STR_STR_HASH:return sp_StrStrHash_length((sp_StrStrHash*)v.v.p);case SP_BUILTIN_INT_STR_HASH:return sp_IntStrHash_length((sp_IntStrHash*)v.v.p);case SP_BUILTIN_INT_INT_HASH:return sp_IntIntHash_length((sp_IntIntHash*)v.v.p);case SP_BUILTIN_STR_POLY_HASH:return sp_StrPolyHash_length((sp_StrPolyHash*)v.v.p);case SP_BUILTIN_SYM_POLY_HASH:return sp_SymPolyHash_length((sp_SymPolyHash*)v.v.p);case SP_BUILTIN_POLY_POLY_HASH:return sp_PolyPolyHash_length((sp_PolyPolyHash*)v.v.p);
/* File#size / File::Stat#size on a boxed handle: the file's byte size via its
   path (a poly-held stat handle's .size read this as container length 0) (#3041) */
case SP_BUILTIN_IO:{sp_int sp_file_size(const char*);sp_File*_f=(sp_File*)v.v.p;if(_f&&sp_File_path(_f)[0]&&sp_File_path(_f)[0]!='<')return sp_file_size(sp_File_path(_f));return 0;}
case SP_BUILTIN_STRBUF: return (sp_int)((sp_String *)v.v.p)->len;   /* live length (#3227) */
/* a user object with #to_a (a container-read Set, #3234): its element count */
default: if (sp_obj_to_a_fn) { sp_RbVal _a = sp_obj_to_a_fn(v); if (_a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_a.cls_id)) return sp_poly_length(_a); } return 0;}}

/* NilClass-aware conversions for a boxed receiver (a nil-holding local widens
   to poly): nil converts per NilClass, a value already of the target kind is
   itself, anything else raises NoMethodError like CRuby. */
static sp_RbVal sp_poly_to_a_m(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_box_poly_array(sp_PolyArray_new());
  /* normalize any array kind into a PolyArray so callers can rely on the
     unboxed representation (the nil case dominates; identity is not kept) */
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id))
    return sp_box_poly_array(sp_poly_to_poly_array(v));
  { sp_PolyArray *ue = sp_poly_user_elems(v);
    if (ue) return sp_box_poly_array(ue); }
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method 'to_a' for %s", sp_poly_class_name(v)));
}
static sp_RbVal sp_poly_to_h_m(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_box_obj(sp_SymPolyHash_new(), SP_BUILTIN_SYM_POLY_HASH);
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) return v;
  /* an OpenStruct (an OpenStruct|nil union reaches here boxed): its member
     table is already a symbol-keyed hash (#3282) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_OPENSTRUCT)
    return sp_box_obj(sp_OpenStruct_to_h((sp_OpenStruct *)v.v.p), SP_BUILTIN_SYM_POLY_HASH);
  /* a Struct/Data read out of a container: dispatch its symbol-keyed to_h by
     cls_id through the generated hook (#2906). */
  if (v.tag == SP_TAG_OBJ && sp_obj_to_h_fn) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ) return h;
  }
  /* an array of [k, v] pairs -> a hash keyed by whatever the pairs hold: a
     Symbol-keyed one where every key is a Symbol (the Hash#partition
     sub-array and Enumerable pair lists this was written for), the general
     boxed hash otherwise -- reading an Integer key as a symbol id built a
     hash whose keys were other programs' symbols (#3972) */
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    sp_int n = sp_poly_length(v);
    int all_sym = 1;
    for (sp_int i = 0; i < n && all_sym; i++) {
      sp_RbVal pair = sp_poly_arr_get(v, i);
      if (!(pair.tag == SP_TAG_OBJ && sp_poly_is_array_kind(pair.cls_id) && sp_poly_length(pair) == 2))
        sp_raise_cls("TypeError", "wrong element type (expected a [key, value] pair)");
      if (sp_poly_arr_get(pair, 0).tag != SP_TAG_SYM) all_sym = 0;
    }
    if (!all_sym) {
      sp_PolyPolyHash *ph = sp_PolyPolyHash_new();
      SP_GC_ROOT(ph);
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal pair = sp_poly_arr_get(v, i);
        sp_PolyPolyHash_set(ph, sp_poly_arr_get(pair, 0), sp_poly_arr_get(pair, 1));
      }
      return sp_box_obj(ph, SP_BUILTIN_POLY_POLY_HASH);
    }
    sp_SymPolyHash *h = sp_SymPolyHash_new();
    SP_GC_ROOT(h);
    for (sp_int i = 0; i < n; i++) {
      sp_RbVal pair = sp_poly_arr_get(v, i);
      sp_RbVal k = sp_poly_arr_get(pair, 0);
      sp_SymPolyHash_set(h, (sp_sym)k.v.i, sp_poly_arr_get(pair, 1));
    }
    return sp_box_obj(h, SP_BUILTIN_SYM_POLY_HASH);
  }
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method 'to_h' for %s", sp_poly_class_name(v)));
}
/* Data#with on a poly receiver (a Data read out of a container): dispatch by
   cls_id to a copy-update constructor, `ov` a symbol-keyed hash of the members
   to override (#2890). */
static sp_RbVal sp_poly_with_m(sp_RbVal v, sp_RbVal ov) {
  if (v.tag == SP_TAG_OBJ && sp_obj_with_fn) {
    sp_RbVal r = sp_obj_with_fn(v, ov);
    if (r.tag == SP_TAG_OBJ) return r;
  }
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method 'with' for %s", sp_poly_class_name(v)));
}
static sp_RbVal sp_poly_to_r_m(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_box_rational(sp_rational_new(0, 1));
  if (v.tag == SP_TAG_INT) return sp_box_rational(sp_rational_new(v.v.i, 1));
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL) return v;
  /* Float#to_r is exact: the rational the binary value really is (#3800). */
  if (v.tag == SP_TAG_FLT) return sp_box_rational(sp_float_to_rational(v.v.f));
  /* Time#to_r is the exact epoch time, the same value the typed receiver
     answers; a Time read out of a container reached here (#3866). */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_TIME && v.v.p) {
    sp_Time *t = (sp_Time *)v.v.p;
    return sp_box_rational(sp_rational_new((sp_int)t->tv_sec * 1000000000 + t->tv_nsec,
                                           1000000000));
  }
  /* String#to_r reads a leading rational and answers (0/1) when there is
     none. sp_poly_to_c_m has carried the matching String arm all along; this
     one raised NoMethodError for a String the receiver happened to widen to
     poly, and `v&.to_r` is exactly that shape. */
  if (v.tag == SP_TAG_STR) return sp_box_rational(sp_str_to_r(v.v.s ? v.v.s : sp_str_empty));
  if (sp_poly_is_strbuf(v)) return sp_poly_to_r_m(sp_poly_strbuf_deref(v));
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method 'to_r' for %s", sp_poly_class_name(v)));
}
static sp_RbVal sp_poly_to_c_m(sp_RbVal v) {
  /* fl carries the per-component int/float flag inspect renders from, so it has
     to be set, not left as whatever the stack held. The typed path is the
     oracle here: `n.to_c` emits `(sp_Complex){n, 0, <1 for a Float, 0 for an
     Integer>}`, and a nil receiver answers the all-integer (0+0i). */
  if (v.tag == SP_TAG_NIL) { sp_Complex z = {0, 0, 0}; return sp_box_complex(z); }
  if (v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT) {
    sp_Complex z = {sp_poly_to_f(v), 0, (unsigned char)(v.tag == SP_TAG_FLT ? SP_CPLX_RE_F : 0)};
    return sp_box_complex(z);
  }
  if (v.tag == SP_TAG_STR) return sp_box_complex(sp_str_to_c(v.v.s ? v.v.s : sp_str_empty));
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX) return v;
  sp_raise_cls("NoMethodError", sp_sprintf("undefined method 'to_c' for %s", sp_poly_class_name(v)));
}
/* Array-reduction methods on a boxed array value -- an element of a poly array,
   e.g. a run produced by chunk_while / slice_when. Each switches on the boxed
   element's cls_id and returns a boxed result, so `runs.map { |r| r.sum }` and
   friends work without statically knowing the run's array type. first/last reuse
   the generic boxed-element accessors. */
/* A user object whose class mixes in Enumerable is opaque to the poly ops --
   its cls_id is its own, and every switch below falls to a default that
   answers the empty-collection value (0, nil, false). The generated TU
   installs sp_obj_to_a_fn for every class with an #each, so its elements are
   reachable; the ops consult it rather than answering for an empty one.
   sp_poly_length already did this; the rest did not (#3761). */
static sp_PolyArray *sp_poly_user_elems(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ || v.cls_id < 0 || !sp_obj_to_a_fn) return NULL;
  sp_RbVal a = sp_obj_to_a_fn(v);
  if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id)) return sp_poly_to_poly_array(a);
  return NULL;
}
static int sp_poly_user_include(sp_RbVal recv, sp_RbVal x) {
  sp_PolyArray *ue = sp_poly_user_elems(recv);
  if (!ue) return -1;
  for (sp_int i = 0; i < ue->len; i++) if (sp_poly_eq(ue->data[i], x)) return 1;
  return 0;
}
static sp_RbVal sp_poly_sum(sp_RbVal v) {
  /* String#sum is a byte checksum, not a container fold: a boxed String fell
     past the switch below and answered 0 (#3446). */
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v))
    return sp_box_int(sp_str_sum_bits(sp_poly_strbuf_deref(v).v.s, 16));
  if (v.tag != SP_TAG_OBJ) return sp_box_int(0);
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  return sp_box_int(sp_IntArray_sum((sp_IntArray *)v.v.p, 0));
    case SP_BUILTIN_FLT_ARRAY:  return sp_box_float(sp_FloatArray_sum((sp_FloatArray *)v.v.p, 0.0));
    /* Accumulate a poly array through sp_poly_add, not sum_int: a container-read
       row of Rationals/Floats/Bignums summed as ints returned 0 (#3159). */
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_sum_poly((sp_PolyArray *)v.v.p);
    /* a container-read int Range sums like the typed path (#3234) */
    case SP_BUILTIN_RANGE: {
      sp_IntArray *ia = sp_range_to_ia(*(sp_Range *)v.v.p);
      SP_GC_ROOT(ia);
      return sp_box_int(sp_IntArray_sum(ia, 0));
    }
    default: {
      sp_PolyArray *ue = sp_poly_user_elems(v);
      return ue ? sp_PolyArray_sum_poly(ue) : sp_box_int(0);
    }
  }
}
static sp_PolyArray *sp_poly_to_a_arr(sp_RbVal v);  /* defined below; hash -> pairs */
/* Range#sum(seed). CRuby's range_sum adds the closed form to an INTEGER seed
   and, for a seed of any other class, converts it with Kernel#Float and
   answers a Float -- so a Rational seed makes the whole sum a Float and a
   String seed raises ArgumentError from the conversion, not from the addition.
   An EMPTY range never touches the seed and answers it unchanged. */
static sp_RbVal sp_range_sum_seed(sp_Range r, sp_RbVal seed) {
  if (sp_range_count(r) <= 0) return seed;
  SP_GC_ROOT_RBVAL(seed);
  sp_IntArray *ia = sp_range_to_ia(r);
  SP_GC_ROOT(ia);
  sp_int total = sp_IntArray_sum(ia, 0);
  /* through sp_poly_add so a Bignum seed keeps its digits instead of wrapping
     into the sp_int the typed emitter used to hand this path */
  if (seed.tag == SP_TAG_INT || seed.tag == SP_TAG_BIGINT)
    return sp_poly_add(sp_box_int(total), seed);
  return sp_box_float(sp_poly_Float(seed) + (sp_float)total);
}
/* Is this a value CRuby's Array#sum keeps in its EXACT accumulation phase --
   the Integer/Rational family, which adds without dropping a digit? A Float is
   not one: it opens the compensated phase below instead. */
static sp_bool sp_poly_sum_exact_p(sp_RbVal v) {
  return v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT ||
         sp_poly_is_rational(v) || sp_poly_is_brat(v);
}
/* One element of the summed collection: an array reads its own storage, every
   other receiver had its elements materialized once by the caller. */
static sp_RbVal sp_poly_sum_item(sp_RbVal v, sp_PolyArray *items, sp_int i) {
  return items ? sp_PolyArray_get(items, i) : sp_poly_arr_get(v, i);
}
/* sum(seed): CRuby's Array#sum, which is also the code Enumerable#sum runs for
   a Hash's pairs and for a String-bounded Range. Three phases, in order: an
   EXACT one while the seed and the elements are Integers or Rationals, so a
   Bignum seed keeps its digits and a Rational one stays a Rational; from the
   first Float onward the compensated summation sp_FloatArray_sum uses, started
   at the exact total; and plain `+` for the rest -- which is where the raise
   CRuby answers for a nil, String or Array seed comes from. A seed of no
   numeric class has no exact phase at all and takes that last one from the
   first element (#3234). */
static sp_RbVal sp_poly_sum_seed(sp_RbVal v, sp_RbVal seed) {
  /* On a String the argument is the bit width of the checksum, not a seed. */
  if (v.tag == SP_TAG_STR || sp_poly_is_strbuf(v))
    return sp_box_int(sp_str_sum_bits(sp_poly_strbuf_deref(v).v.s, sp_poly_to_i(seed)));
  /* An Integer Range sums by its own rule, which the element walk below cannot
     express -- and with no arm at all it simply handed the seed back. */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE && v.v.p)
    return sp_range_sum_seed(*(sp_Range *)v.v.p, seed);
  SP_GC_ROOT_RBVAL(v);
  SP_GC_ROOT_RBVAL(seed);
  /* The elements Enumerable#sum folds: an array's own, a Hash's [k, v] pairs,
     a String range's members, an Enumerator's or a user Enumerable's. A
     receiver that is no collection at all adds nothing and answers the seed. */
  sp_PolyArray *items = NULL;
  SP_GC_ROOT(items);
  sp_int n = 0;
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) n = sp_poly_length(v);
  else if (v.tag == SP_TAG_OBJ &&
           (sp_poly_is_hash_kind(v.cls_id) || v.cls_id == SP_BUILTIN_STR_RANGE ||
            v.cls_id == SP_BUILTIN_ENUMERATOR)) {
    items = sp_poly_to_a_arr(v);
    n = items ? items->len : 0;
  }
  else if (v.tag == SP_TAG_OBJ && v.cls_id >= 0) {
    items = sp_poly_user_elems(v);
    if (!items) return seed;
    n = items->len;
  }
  else return seed;
  sp_RbVal acc = seed;
  SP_GC_ROOT_RBVAL(acc);
  sp_int i = 0;
  /* Only a seed of the numeric tower has an exact or a compensated phase at
     all: nil, a String, an Array start at plain `+` from the first element,
     which is the whole point -- that is the operator whose failure CRuby
     reports. */
  /* ...and only an EXACT seed reaches the compensated phase. CRuby enters
     the float loop from the exact phase, so a seed that is already a Float
     has neither: it runs plain `+` from the first element, which is why
     `[0.1, 0.2, 0.3].sum(0.0)` is 0.6000000000000001 while `.sum(0)` and
     `.sum` are 0.6. Reading a Float seed as "numeric, so compensate" made
     the two agree, which they do not. */
  sp_bool numeric_seed = sp_poly_sum_exact_p(acc);
  if (sp_poly_sum_exact_p(acc)) {
    for (; i < n; i++) {
      sp_RbVal e = sp_poly_sum_item(v, items, i);
      if (!sp_poly_sum_exact_p(e)) break;
      acc = sp_poly_add(acc, e);
    }
  }
  /* The compensated phase, entered from the first Float element that ends the
     exact phase -- and running the very step sp_FloatArray_sum runs, NaN and
     Infinity arms included. `e` is carried across the turn boundary so the
     element that decided the entry is not read a second time. */
  if (numeric_seed && i < n) {
    sp_RbVal e = sp_poly_sum_item(v, items, i);
    if (e.tag == SP_TAG_FLT) {
      sp_float f = sp_poly_to_f(acc), c = 0.0;
      while (i < n && (e.tag == SP_TAG_FLT || sp_poly_sum_exact_p(e))) {
        sp_float_sum_step(&f, &c, sp_poly_to_f(e));
        if (++i < n) e = sp_poly_sum_item(v, items, i);
      }
      /* CRuby folds the compensation back only when the run reaches the END of
         the elements: its `not_float:` label takes DBL2NUM(f) alone, so
         `[0.1, 0.2, 0.3, Complex(0, 1)].sum(0.0)` keeps the uncompensated
         0.6000000000000001 before the Complex is added. */
      acc = sp_box_float(i < n ? f : f + c);
    }
  }
  for (; i < n; i++) acc = sp_poly_add(acc, sp_poly_sum_item(v, items, i));
  return acc;
}
static sp_PolyArray *sp_enum_to_a_boxed(sp_RbVal v);  /* defined below, after sp_enum.h */
/* The count-taking reads of Array's surface, for a receiver carried in a poly
   slot -- an array read out of a nested Array or Hash answers Array to #class
   but had no arm for these, so they raised NoMethodError (#3464). Each is the
   contiguous-slice form where one exists, so a typed array stays typed. */
/* The element list these reads work over: a hash contributes its [k, v] pairs,
   exactly as Enumerable sees it. */
static sp_RbVal sp_poly_arr_span(sp_RbVal v, sp_int from, sp_int n) {
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *p = sp_poly_to_a_arr(v); SP_GC_ROOT(p);
  return sp_box_poly_array(sp_PolyArray_slice(p, from, n));
}
/* The span helpers below read ITEMS, and a generator-backed Enumerator has
   none until it runs: `e.take(1)` on one answered [] where CRuby answers its
   first element. Materialize the enumerator first; every other receiver is
   already its own subject. */
static sp_PolyArray *sp_enum_to_a_boxed(sp_RbVal v);   /* fwd: drain an enumerator */
static sp_RbVal sp_poly_span_subject(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_ENUMERATOR && v.v.p)
    return sp_box_poly_array(sp_enum_to_a_boxed(v));
  return v;
}
static sp_RbVal sp_poly_arr_take(sp_RbVal v, sp_int n) {
  if (n < 0) sp_raise_cls("ArgumentError", "negative array size");
  v = sp_poly_span_subject(v);
  sp_int alen = sp_poly_length(v);
  return sp_poly_arr_span(v, 0, n > alen ? alen : n);
}
static sp_RbVal sp_poly_arr_last_n(sp_RbVal v, sp_int n) {
  if (n < 0) sp_raise_cls("ArgumentError", "negative array size");
  v = sp_poly_span_subject(v);
  sp_int alen = sp_poly_length(v);
  if (n > alen) n = alen;
  return sp_poly_arr_span(v, alen - n, n);
}
static sp_RbVal sp_poly_arr_drop(sp_RbVal v, sp_int n) {
  if (n < 0) sp_raise_cls("ArgumentError", "attempt to drop negative size");
  v = sp_poly_span_subject(v);
  sp_int alen = sp_poly_length(v);
  if (n > alen) n = alen;
  return sp_poly_arr_span(v, n, alen - n);
}
static sp_RbVal sp_poly_arr_rotate(sp_RbVal v, sp_int n) {
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *r = sp_PolyArray_dup(sp_poly_to_a_arr(v));
  SP_GC_ROOT(r);
  sp_PolyArray_rotate_bang(r, n);
  return sp_box_poly_array(r);
}
static sp_RbVal sp_poly_arr_sample_n(sp_RbVal v, sp_int n) {
  if (n < 0) sp_raise_cls("ArgumentError", "negative sample number");
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *r = sp_PolyArray_dup(sp_poly_to_a_arr(v));
  SP_GC_ROOT(r);
  sp_PolyArray_shuffle_bang(r);
  if (n > r->len) n = r->len;
  return sp_box_poly_array(sp_PolyArray_slice(r, 0, n));
}
/* Array#values_at indexes; Hash#values_at looks the keys up. */
static sp_RbVal sp_poly_arr_values_at(sp_RbVal v, sp_PolyArray *idx) {
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT(idx);
  int is_hash = v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id);
  sp_int alen = is_hash ? 0 : sp_poly_arr_len(v);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  for (sp_int i = 0; idx && i < idx->len; i++) {
    if (is_hash) { sp_PolyArray_push(out, sp_poly_index_poly(v, idx->data[i])); continue; }
    sp_int k = sp_poly_to_i(idx->data[i]);
    if (k < 0) k += alen;
    sp_PolyArray_push(out, (k < 0 || k >= alen) ? sp_box_nil() : sp_poly_arr_get(v, k));
  }
  return sp_box_poly_array(out);
}
static sp_RbVal sp_poly_min(sp_RbVal v) {
  /* A receiver that is not a container (nil, an Integer, a String) has no
     #min in CRuby; answering nil hid the call entirely (#4192 follow-up). */
  if (v.tag != SP_TAG_OBJ) return sp_raise_nomethod(sp_nomethod_msg("min", v));
  /* Enumerable#min on a boxed hash: the least [k, v] pair by pair comparison. */
  if (sp_poly_is_hash_kind(v.cls_id)) return sp_PolyArray_min(sp_poly_to_a_arr(v));
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  { sp_IntArray *a = (sp_IntArray *)v.v.p; return (a && a->len) ? sp_box_int(sp_IntArray_min(a)) : sp_box_nil(); }
    case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)v.v.p; return (a && a->len) ? sp_box_float(sp_FloatArray_min(a)) : sp_box_nil(); }
    /* a String array reached here through the default and answered nil (#3464) */
    case SP_BUILTIN_STR_ARRAY:  { const char *m = sp_StrArray_min((sp_StrArray *)v.v.p); return m ? sp_box_str(m) : sp_box_nil(); }
    case SP_BUILTIN_SYM_ARRAY:  return sp_PolyArray_min(sp_poly_to_poly_array(v));
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_min((sp_PolyArray *)v.v.p);
    /* Time#min is the MINUTE, not an enumerable minimum. A boxed Time has no
       user elements, so it fell to the default and answered nil -- silently,
       and only for `min`: every sibling accessor (hour, sec, ...) reaches its
       own arm in emit_poly_builtin_method, which `min` never gets to because
       the enumerable fast path claims the name first (#4192). */
    case SP_BUILTIN_TIME:       return sp_box_int(sp_time_min(*(sp_Time *)v.v.p));
    /* a boxed int Range enumerates like the typed path; empty answers nil */
    case SP_BUILTIN_RANGE: { sp_IntArray *ia = sp_range_to_ia(*(sp_Range *)v.v.p);
                             SP_GC_ROOT(ia);
                             return ia->len ? sp_box_int(sp_IntArray_min(ia)) : sp_box_nil(); }
    default: { sp_PolyArray *ue = sp_poly_user_elems(v);
               return ue ? sp_PolyArray_min(ue) : sp_raise_nomethod(sp_nomethod_msg("min", v)); }
  }
}
static sp_RbVal sp_poly_max(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_raise_nomethod(sp_nomethod_msg("max", v));
  if (sp_poly_is_hash_kind(v.cls_id)) return sp_PolyArray_max(sp_poly_to_a_arr(v));
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  { sp_IntArray *a = (sp_IntArray *)v.v.p; return (a && a->len) ? sp_box_int(sp_IntArray_max(a)) : sp_box_nil(); }
    case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)v.v.p; return (a && a->len) ? sp_box_float(sp_FloatArray_max(a)) : sp_box_nil(); }
    case SP_BUILTIN_STR_ARRAY:  { const char *m = sp_StrArray_max((sp_StrArray *)v.v.p); return m ? sp_box_str(m) : sp_box_nil(); }
    case SP_BUILTIN_SYM_ARRAY:  return sp_PolyArray_max(sp_poly_to_poly_array(v));
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_max((sp_PolyArray *)v.v.p);
    case SP_BUILTIN_RANGE: { sp_IntArray *ia = sp_range_to_ia(*(sp_Range *)v.v.p);
                             SP_GC_ROOT(ia);
                             return ia->len ? sp_box_int(sp_IntArray_max(ia)) : sp_box_nil(); }
    default: { sp_PolyArray *ue = sp_poly_user_elems(v);
               return ue ? sp_PolyArray_max(ue) : sp_raise_nomethod(sp_nomethod_msg("max", v)); }
  }
}
/* Hash#first / #last answer a [key, value] pair; sp_poly_arr_get indexes the
   array kinds only, so a boxed hash answered nil (#3450). sp_enum_items_from
   renders the pairs the same way `each` does. */
static sp_PolyArray *sp_poly_hash_pairs_or_null(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) return sp_enum_items_from(v);
  return NULL;
}
/* Blockless Hash#sum with an Array init: Enumerable#sum folds each [k, v]
   pair into the init with `+`, which for an Array init is concatenation, so
   the keys and values land flat in order (#3571). */
static sp_PolyArray *sp_poly_hash_sum_arr(sp_RbVal v, sp_PolyArray *init) {
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT(init);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (init) for (sp_int i = 0; i < init->len; i++) sp_PolyArray_push(out, init->data[i]);
  sp_PolyArray *ps = sp_poly_hash_pairs_or_null(v);
  if (ps) {
    SP_GC_ROOT(ps);
    for (sp_int i = 0; i < ps->len; i++) {
      sp_PolyArray *pair = sp_poly_to_poly_array(ps->data[i]);
      if (!pair) continue;
      for (sp_int j = 0; j < pair->len; j++) sp_PolyArray_push(out, pair->data[j]);
    }
  }
  return out;
}
/* Time#deconstruct_keys(nil): every field, for a hash pattern to match
   against (#3702). The symbols are interned at run time because these names
   are synthesized after the static symbol table is written. */
static sp_SymPolyHash *sp_time_deconstruct_all(sp_Time t) {
  sp_SymPolyHash *h = sp_SymPolyHash_new(); SP_GC_ROOT(h);
  sp_SymPolyHash_set(h, sp_sym_intern("year"), sp_box_int(sp_time_year(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("month"), sp_box_int(sp_time_mon(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("mon"), sp_box_int(sp_time_mon(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("day"), sp_box_int(sp_time_mday(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("mday"), sp_box_int(sp_time_mday(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("hour"), sp_box_int(sp_time_hour(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("min"), sp_box_int(sp_time_min(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("sec"), sp_box_int(sp_time_sec(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("wday"), sp_box_int(sp_time_wday(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("yday"), sp_box_int(sp_time_yday(t)));
  sp_SymPolyHash_set(h, sp_sym_intern("subsec"),
                     t.tv_nsec == 0 ? sp_box_int(0)
                                    : sp_box_rational(sp_rational_new((sp_int)t.tv_nsec, 1000000000)));
  sp_SymPolyHash_set(h, sp_sym_intern("dst"), sp_box_bool(sp_time_isdst(t) != 0));
  sp_SymPolyHash_set(h, sp_sym_intern("zone"), sp_box_str(sp_time_zone(t)));
  return h;
}
static sp_RbVal sp_poly_first(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  { sp_PolyArray *ps = sp_poly_hash_pairs_or_null(v);
    if (ps) return ps->len > 0 ? ps->data[0] : sp_box_nil(); }
  /* a user class that includes Enumerable answers from its own elements; the
     array read below only knows containers, so it answered nil (#3875) */
  /* Range#first is the begin, with no emptiness check: (5..1).first is 5.
     Before the user_elems read, which materializes a Range and would answer
     nil for an empty one. */
  if (v.cls_id == SP_BUILTIN_RANGE) return sp_box_int(((sp_Range *)v.v.p)->first);
  { sp_PolyArray *ue = sp_poly_user_elems(v);
    if (ue) return ue->len > 0 ? ue->data[0] : sp_box_nil(); }
  return sp_poly_arr_get(v, 0);
}
static sp_RbVal sp_poly_last(sp_RbVal v) {
  /* Range#last is the end, exclusivity untouched: (1...5).last is 5. A
     stepped range's last is the last element it enumerates. Before the
     user_elems read, which materializes the Range and would answer the
     exclusivity-adjusted element instead. */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE) {
    sp_Range *rg = (sp_Range *)v.v.p;
    if (rg->step == 0 || rg->step == 1) return sp_box_int(rg->last);
    sp_IntArray *ia = sp_range_to_ia(*rg);
    SP_GC_ROOT(ia);
    return ia->len ? sp_box_int(ia->data[ia->start + ia->len - 1]) : sp_box_nil();
  }
  { sp_PolyArray *ue = v.tag == SP_TAG_OBJ ? sp_poly_user_elems(v) : NULL;
    if (ue) return ue->len > 0 ? ue->data[ue->len - 1] : sp_box_nil(); }
  sp_int n = sp_poly_length(v);
  return n > 0 ? sp_poly_arr_get(v, n - 1) : sp_box_nil();
}
/* Array#first(n) / #last(n) on a value only known at run time: the n-element
   prefix or suffix, as a fresh array. A Hash walks its [k, v] pairs, the way
   every other Enumerable name here does. */
static sp_RbVal sp_poly_first_n(sp_RbVal v, sp_int n) {
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (n < 0) sp_raise_cls("ArgumentError", "negative array size");
  sp_int len = sp_poly_arr_len_ex(v);
  if (n > len) n = len;
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(out, sp_poly_each_elem(v, i));
  return sp_box_poly_array(out);
}
static sp_RbVal sp_poly_last_n(sp_RbVal v, sp_int n) {
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (n < 0) sp_raise_cls("ArgumentError", "negative array size");
  sp_int len = sp_poly_arr_len_ex(v);
  if (n > len) n = len;
  for (sp_int i = len - n; i < len; i++) sp_PolyArray_push(out, sp_poly_each_elem(v, i));
  return sp_box_poly_array(out);
}
static sp_RbVal sp_poly_sample(sp_RbVal v) {
  sp_int n = sp_poly_length(v);
  return n > 0 ? sp_poly_arr_get(v, sp_krand_below(n)) : sp_box_nil();
}
/* Thread#value / #join through a poly slot. A Thread is modelled as a Fiber run
   to completion (single-threaded); when one is carried in a poly value -- e.g.
   an array of Threads, `(1..n).map { Thread.new { ... } }` -- #value/#join must
   dispatch at runtime on the boxed Fiber. value/resume return the block's
   result; join runs it and returns the thread (self). A non-Fiber poly returns
   nil, matching the NoMethodError gate for an unknown poly method. */
static sp_RbVal sp_poly_fiber_value(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_THREAD)
    return sp_Thread_value((sp_thread *)v.v.p);   /* a green thread carried in a poly slot */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_FIBER) {
    sp_Fiber *f = (sp_Fiber *)v.v.p;
    if (f->state == 3) return f->yielded_value;   /* already run: cached result, idempotent */
    return sp_Fiber_resume(f, sp_box_nil());
  }
  return sp_box_nil();
}
/* Forward declarations for sp_sched.c functions used by codegen. The
   codegen emits direct symbol references (sp_Thread_join_timeout) so
   the signatures must be visible here. */
sp_thread *sp_Thread_join_timeout(sp_thread *t, double seconds);

/* `poly.join(<number>)` can only be Thread#join(limit): Array#join with a
   numeric separator is a TypeError in CRuby, so a numeric argument leaves no
   other reading. Answers the thread (boxed) when it finished inside the
   limit and nil when it did not, which is why this returns sp_RbVal where
   sp_poly_join returns the joined string. A non-Thread receiver gets the
   TypeError the separator slot would have raised. */
static sp_RbVal sp_poly_join_timeout(sp_RbVal v, double seconds, const char *argcls) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_THREAD)
    return sp_Thread_join_timeout((sp_thread *)v.v.p, seconds) ? v : sp_box_nil();
  /* the separator slot's own refusal, naming the class the argument was
     WRITTEN as -- the double this took it as would say Float for an Integer */
  sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into String",
                                       argcls ? argcls : "Numeric"));
  return sp_box_nil();
}

static sp_RbVal sp_poly_fiber_join(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_THREAD) {
    sp_Thread_join((sp_thread *)v.v.p);
    return v;
  }
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_FIBER) {
    sp_Fiber *f = (sp_Fiber *)v.v.p;
    if (f->state != 3) sp_Fiber_resume(f, sp_box_nil());
  }
  return v;
}
static sp_PolyArray*sp_PolyPolyHash_keys(sp_PolyPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->keys[h->order[i]]);return a;}
static sp_PolyArray*sp_PolyPolyHash_values(sp_PolyPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->vals[h->order[i]]);return a;}

/* Hash#keys / #values on a poly receiver -- e.g. an evidence-free empty `{}`
   that stayed poly, or a hash read back out of a poly slot. Dispatch on the
   runtime hash variant, returning a poly array of the (boxed) keys or values.
   A non-hash poly value raises NoMethodError, as CRuby does for keys/values.
   Each typed intermediate is rooted before the converter allocates. */
static sp_PolyArray *sp_poly_keys(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH:  { sp_StrArray *k = sp_StrIntHash_keys((sp_StrIntHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_STR_STR_HASH:  { sp_StrArray *k = sp_StrStrHash_keys((sp_StrStrHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_INT_STR_HASH:  { sp_IntArray *k = sp_IntStrHash_keys((sp_IntStrHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_int_array(k); }
    case SP_BUILTIN_INT_INT_HASH:  { sp_IntArray *k = sp_IntIntHash_keys((sp_IntIntHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_int_array(k); }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrArray *k = sp_StrPolyHash_keys((sp_StrPolyHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_IntArray *k = sp_SymPolyHash_keys((sp_SymPolyHash*)v.v.p); SP_GC_ROOT(k); sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a); for (sp_int i = 0; i < k->len; i++) sp_PolyArray_push(a, sp_box_sym((sp_sym)k->data[k->start + i])); return a; }
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_keys((sp_PolyPolyHash*)v.v.p);
  }
  sp_raise_cls("NoMethodError", "undefined method 'keys'");
  return NULL;  /* unreachable: sp_raise_cls is noreturn */
}
static sp_PolyArray *sp_poly_values(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH:  { sp_IntArray *vv = sp_StrIntHash_values((sp_StrIntHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_int_array(vv); }
    case SP_BUILTIN_STR_STR_HASH:  { sp_StrArray *vv = sp_StrStrHash_values((sp_StrStrHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_str_array(vv); }
    case SP_BUILTIN_INT_STR_HASH:  { sp_StrArray *vv = sp_IntStrHash_values((sp_IntStrHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_str_array(vv); }
    case SP_BUILTIN_INT_INT_HASH:  { sp_IntArray *vv = sp_IntIntHash_values((sp_IntIntHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_int_array(vv); }
    case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_values((sp_StrPolyHash*)v.v.p);
    case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_values((sp_SymPolyHash*)v.v.p);
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_values((sp_PolyPolyHash*)v.v.p);
  }
  sp_raise_cls("NoMethodError", "undefined method 'values'");
  return NULL;  /* unreachable: sp_raise_cls is noreturn */
}
static sp_PolyPolyHash*sp_PolyPolyHash_replace(sp_PolyPolyHash*h,sp_PolyPolyHash*o){if(!h||h==o)return h;SP_GC_ROOT(h);SP_GC_ROOT(o);for(sp_int i=0;i<h->cap;i++)h->occ[i]=FALSE;h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_PolyPolyHash_set(h,o->keys[o->order[i]],o->vals[o->order[i]]);return h;}
static sp_PolyPolyHash*sp_PolyPolyHash_dup(sp_PolyPolyHash*h){SP_GC_ROOT(h);sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,h->keys[h->order[i]],h->vals[h->order[i]]);return r;}
/* Issue #738: poly_poly_hash inspect using sp_poly_inspect on each
   k,v. Output mirrors Ruby's `{k => v, ...}` for non-symbol keys and
   `{k: v, ...}` shorthand for symbol keys. */
static const char *sp_poly_inspect(sp_RbVal v);
static const char*sp_PolyPolyHash_inspect(sp_PolyPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_POLY_POLY_HASH)):SPL("nil");}
/* Issue #738: Hash#invert -- swap keys and values. Returns a
   poly_poly_hash so any (key, value) pair shape is uniformly
   representable. str_str_hash_invert lives above (line ~1132)
   and stays as a same-type round-trip. */
static sp_PolyPolyHash*sp_StrIntHash_invert_poly(sp_StrIntHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(sp_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_int(sp_StrIntHash_get(h,h->order[i])),sp_box_str(h->order[i]));return r;}
static sp_PolyPolyHash*sp_IntStrHash_invert(sp_IntStrHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(sp_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_str(sp_IntStrHash_get(h,h->order[i])),sp_box_int(h->order[i]));return r;}
static sp_bool sp_PolyPolyHash_eq(sp_PolyPolyHash*a,sp_PolyPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){sp_RbVal k=a->keys[a->order[i]];if(!sp_PolyPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_PolyPolyHash_get(a,k),sp_PolyPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* --- cross-variant hash equality ------------------------------------------
   Boxed key/value of the i-th insertion-ordered pair, per variant. */
static void sp_poly_hash_pair_i(sp_RbVal h, sp_int i, sp_RbVal *k, sp_RbVal *v) {
  switch (h.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *x=(sp_StrIntHash*)h.v.p; *k=sp_box_str(x->order[i]); *v=sp_box_int(sp_StrIntHash_get(x,x->order[i])); return; }
    case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *x=(sp_StrStrHash*)h.v.p; *k=sp_box_str(x->order[i]); *v=sp_box_str(sp_StrStrHash_get(x,x->order[i])); return; }
    case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *x=(sp_IntStrHash*)h.v.p; *k=sp_box_int(x->order[i]); *v=sp_box_str(sp_IntStrHash_get(x,x->order[i])); return; }
    case SP_BUILTIN_INT_INT_HASH: { sp_IntIntHash *x=(sp_IntIntHash*)h.v.p; *k=sp_box_int(x->order[i]); *v=sp_box_int(sp_IntIntHash_get(x,x->order[i])); return; }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *x=(sp_StrPolyHash*)h.v.p; *k=sp_box_str(x->order[i]); *v=sp_StrPolyHash_get(x,x->order[i]); return; }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *x=(sp_SymPolyHash*)h.v.p; *k=sp_box_sym(x->order[i]); *v=sp_SymPolyHash_get(x,x->order[i]); return; }
    case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *x=(sp_PolyPolyHash*)h.v.p; *k=x->keys[x->order[i]]; *v=sp_PolyPolyHash_get(x,*k); return; }
    default: *k = sp_box_nil(); *v = sp_box_nil(); return;
  }
}
/* i-th iteration element of a poly receiver: for a hash, the boxed [key, value]
   pair (so a block sees `|pair|` or auto-splats to `|k, val|`, matching CRuby);
   for anything else, the plain element read. Hash pairs must enumerate by
   insertion index, not integer-key lookup, which sp_poly_arr_get_hash does
   (#2873). */
static sp_RbVal sp_poly_iter_elem(sp_RbVal recv, sp_int i) {
  if (recv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(recv.cls_id)) {
    sp_RbVal k, v;
    sp_poly_hash_pair_i(recv, i, &k, &v);
    SP_GC_ROOT_RBVAL(k); SP_GC_ROOT_RBVAL(v);
    sp_PolyArray *pr = sp_PolyArray_new(); SP_GC_ROOT(pr);
    sp_PolyArray_push(pr, k);
    sp_PolyArray_push(pr, v);
    sp_RbVal out; out.tag = SP_TAG_OBJ; out.cls_id = SP_BUILTIN_POLY_ARRAY; out.v.p = pr;
    return out;
  }
  /* a user object with #to_a (a container-read Set, #3234) */
  if (recv.tag == SP_TAG_OBJ && !sp_poly_is_array_kind(recv.cls_id) && sp_obj_to_a_fn) {
    sp_RbVal _a = sp_obj_to_a_fn(recv);
    if (_a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_a.cls_id)) return sp_poly_arr_get(_a, i);
  }
  return sp_poly_arr_get_hash(recv, i);
}
/* Probe a boxed key in a hash of any variant; *found distinguishes a missing
   key from a nil value (Hash#== requires presence). A key whose boxed kind
   cannot exist in the variant (an int key in a string-keyed hash) is absent. */
static sp_RbVal sp_poly_hash_probe(sp_RbVal h, sp_RbVal k, sp_bool *found) {
  *found = FALSE;
  switch (h.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *x=(sp_StrIntHash*)h.v.p; if (k.tag!=SP_TAG_STR||!k.v.s) return sp_box_nil(); if (!sp_StrIntHash_has_key(x,k.v.s)) return sp_box_nil(); *found=TRUE; return sp_box_int(sp_StrIntHash_get(x,k.v.s)); }
    case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *x=(sp_StrStrHash*)h.v.p; if (k.tag!=SP_TAG_STR||!k.v.s) return sp_box_nil(); if (!sp_StrStrHash_has_key(x,k.v.s)) return sp_box_nil(); *found=TRUE; return sp_box_str(sp_StrStrHash_get(x,k.v.s)); }
    case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *x=(sp_IntStrHash*)h.v.p; if (k.tag!=SP_TAG_INT) return sp_box_nil(); if (!sp_IntStrHash_has_key(x,k.v.i)) return sp_box_nil(); *found=TRUE; return sp_box_str(sp_IntStrHash_get(x,k.v.i)); }
    case SP_BUILTIN_INT_INT_HASH: { sp_IntIntHash *x=(sp_IntIntHash*)h.v.p; if (k.tag!=SP_TAG_INT) return sp_box_nil(); if (!sp_IntIntHash_has_key(x,k.v.i)) return sp_box_nil(); *found=TRUE; return sp_box_int(sp_IntIntHash_get(x,k.v.i)); }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *x=(sp_StrPolyHash*)h.v.p; if (k.tag!=SP_TAG_STR||!k.v.s) return sp_box_nil(); if (!sp_StrPolyHash_has_key(x,k.v.s)) return sp_box_nil(); *found=TRUE; return sp_StrPolyHash_get(x,k.v.s); }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *x=(sp_SymPolyHash*)h.v.p; if (k.tag!=SP_TAG_SYM) return sp_box_nil(); if (!sp_SymPolyHash_has_key(x,(sp_sym)k.v.i)) return sp_box_nil(); *found=TRUE; return sp_SymPolyHash_get(x,(sp_sym)k.v.i); }
    case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *x=(sp_PolyPolyHash*)h.v.p; if (!sp_PolyPolyHash_has_key(x,k)) return sp_box_nil(); *found=TRUE; return sp_PolyPolyHash_get(x,k); }
    default: return sp_box_nil();
  }
}
/* Enumerable#tally(hash): count each element INTO the given accumulator hash
   (any variant, held as a boxed value) and return it. Missing keys start from the
   hash's current count (0 if absent), matching CRuby. (#2533) */
static sp_RbVal sp_array_tally_into_poly(sp_RbVal arr, sp_RbVal hash) {
  SP_GC_ROOT_RBVAL(arr); SP_GC_ROOT_RBVAL(hash);
  sp_int n = sp_poly_length(arr);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_arr_get(arr, i);
    sp_bool found = FALSE;
    sp_RbVal cur = sp_poly_hash_probe(hash, e, &found);
    sp_int c = (found && cur.tag == SP_TAG_INT) ? cur.v.i : 0;
    sp_poly_set_poly(hash, e, sp_box_int(c + 1));
  }
  return hash;
}
static sp_bool sp_poly_hash_eq_cross(sp_RbVal a, sp_RbVal b) {
  if (!a.v.p || !b.v.p) return a.v.p == b.v.p;
  SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
  if (sp_poly_length(a) != sp_poly_length(b)) return FALSE;
  sp_int n = sp_poly_length(a);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, va;
    sp_poly_hash_pair_i(a, i, &k, &va);
    sp_bool found = FALSE;
    sp_RbVal vb = sp_poly_hash_probe(b, k, &found);
    if (!found) return FALSE;
    if (!sp_poly_eq(va, vb)) return FALSE;
  }
  return TRUE;
}

/* JSON serialization now lives in lib/sp_json.c. It owns no container types and
   reaches them only through these generic readers, registered into the sp_json_*
   hooks below. sp_json_kind classifies a boxed value (1=array, 2=hash, 0=other);
   sp_poly_hash_pair yields a hash's boxed (key,value) at insertion index i.
   SYM_INT/SYM_STR hashes are not listed -> kind 0 -> null, as before. */
/* sp_json_kind: moved to lib/sp_cold.c */
int sp_json_kind(sp_RbVal v);
static void sp_poly_hash_pair(sp_RbVal v, sp_int i, sp_RbVal *k, sp_RbVal *out) {
  *k = sp_box_nil(); *out = sp_box_nil();
  if (v.tag != SP_TAG_OBJ) return;
  switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *h=(sp_StrIntHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_box_int(sp_StrIntHash_get(h,key)); break; }
    case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *h=(sp_StrStrHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_box_str(sp_StrStrHash_get(h,key)); break; }
    case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *h=(sp_IntStrHash*)v.v.p; sp_int key=h->order[i]; *k=sp_box_int(key); *out=sp_box_str(sp_IntStrHash_get(h,key)); break; }
    case SP_BUILTIN_INT_INT_HASH: { sp_IntIntHash *h=(sp_IntIntHash*)v.v.p; sp_int key=h->order[i]; *k=sp_box_int(key); *out=sp_box_int(sp_IntIntHash_get(h,key)); break; }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *h=(sp_StrPolyHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_StrPolyHash_get(h,key); break; }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *h=(sp_SymPolyHash*)v.v.p; sp_sym key=h->order[i]; *k=sp_box_sym(key); *out=sp_SymPolyHash_get(h,key); break; }
    case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *h=(sp_PolyPolyHash*)v.v.p; sp_int oi=h->order[i]; *k=h->keys[oi]; *out=h->vals[oi]; break; }
    default: break;
  }
}
/* JSON.parse object builders: a string-keyed poly hash (the parser owns the
   key strings; StrPolyHash stores the pointer without copying). */
static sp_RbVal sp_json_new_strhash(void) {
  return sp_box_obj(sp_StrPolyHash_new(), SP_BUILTIN_STR_POLY_HASH);
}
static void sp_json_strhash_set(sp_RbVal h, const char *k, sp_RbVal v) {
  sp_StrPolyHash_set((sp_StrPolyHash *)h.v.p, k, v);
}
/* JSON.parse(symbolize_names: true): deep-convert every parsed object's
   string keys to symbols. Objects become fresh SymPolyHashes (values
   recursed); arrays recurse in place (the parse output is freshly owned);
   scalars pass through. Interning goes through sp_json_sym_intern_fn (the
   symbol table lives in the generated TU; sp_re_init installs the hook
   before any user code runs). */
static sp_RbVal sp_json_symbolize(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return v;
  if (v.cls_id == SP_BUILTIN_STR_POLY_HASH) {
    sp_StrPolyHash *h = (sp_StrPolyHash *)v.v.p;
    SP_GC_ROOT(h);
    sp_SymPolyHash *r = sp_SymPolyHash_new();
    SP_GC_ROOT(r);
    if (h) for (sp_int i = 0; i < h->len; i++) {
      const char *k = h->order[i];
      sp_RbVal sv = sp_json_symbolize(sp_StrPolyHash_get(h, k));
      SP_GC_ROOT_RBVAL(sv);
      sp_SymPolyHash_set(r, sp_json_sym_intern_fn(k), sv);
    }
    return sp_box_obj(r, SP_BUILTIN_SYM_POLY_HASH);
  }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *a = (sp_PolyArray *)v.v.p;
    SP_GC_ROOT(a);
    /* in place: the recursion answers a freshly built hash for every object
       element, so an already-old array takes a young reference here */
    if (a) { sp_gc_wb((void *)a);
      for (sp_int i = 0; i < a->len; i++) a->data[i] = sp_json_symbolize(a->data[i]); }
    return v;
  }
  return v;
}
__attribute__((constructor)) static void sp_json_install_hooks(void) {
  sp_json_kind_fn = sp_json_kind;
  sp_json_len_fn = sp_poly_length;
  sp_json_aref_fn = sp_poly_arr_get;
  sp_json_hpair_fn = sp_poly_hash_pair;
  /* sp_poly_inspect renders symbols and class names, so taking its address
     forces the generated TU to define sp_sym_to_s / sp_class_to_s. A program
     with no poly-renderable value defines SP_TU_NO_POLY_RENDER before the
     include and skips the hook (the archive-side consumers null-check it). */
#ifndef SP_TU_NO_POLY_RENDER
  sp_poly_inspect_fn = sp_poly_inspect;
  sp_poly_to_s_fn = sp_poly_to_s;
#endif
  /* JSON.parse builds objects as string-keyed hashes (CRuby returns String
     keys, and this matches a `{ "k" => v }` literal for equality). */
  sp_json_mk_hash_fn = sp_json_new_strhash;
  sp_json_hash_set_fn = sp_json_strhash_set;
}

#include <setjmp.h>
#define SP_EXC_STACK_MAX 64
/* Per-worker (SP_TLS) in the threaded build: this is the active exception/ensure
   handler stack of the thread currently executing. It is swapped per fiber by
   sp_exc_ctx_save/load, which assumes a single active stack -- true at N=1, but
   with N>1 every OS worker runs a fiber concurrently, so each needs its own.
   Empty (plain static, byte-identical) in the single-threaded build. */
static SP_TLS jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];
static SP_TLS const char *sp_exc_msg[SP_EXC_STACK_MAX];
/* GC-root watermark at each handler's entry: a raise longjmps past the
   __attribute__((cleanup)) pops of SP_GC_ROOT locals in the unwound frames,
   so the landing restores sp_gc_nroots from the popped slot. A side array
   (not a per-region C local) so protected regions add no stack locals --
   an extra local per region measurably shifts hot-function frames. */
static SP_TLS int sp_exc_rootmark[SP_EXC_STACK_MAX];
static SP_TLS volatile int sp_exc_top = 0;
static SP_TLS const char *sp_exc_cls[SP_EXC_STACK_MAX];
static SP_TLS volatile const char *sp_last_exc_cls = sp_str_empty;
/* The raised exception OBJECT, carried alongside (cls,msg) so a user
   exception subclass keeps its ivars across raise -> rescue (#1415).
   NULL for a bare string/builtin raise, which reconstructs on catch.
   sp_pending_exc_obj is set by sp_raise_exc just before the longjmp and
   consumed into the per-frame slot by sp_raise_cls. */
static SP_TLS void *sp_exc_obj[SP_EXC_STACK_MAX];
static SP_TLS void *sp_pending_exc_obj = NULL;
/* The exception currently being handled (set by a rescue body), and the cause
   captured for the next raised exception -- Exception#cause threads the former
   into a newly raised exception's `cause` field. */
static SP_TLS void *sp_pending_cause = NULL;
/* The exception unwinding through an `ensure` body: a raise from inside that
   body takes it as its cause, the way a raise inside a rescue takes $! (#3745). */
static SP_TLS void *sp_inflight_cause = NULL;
/* A bare `raise` re-raises the handled exception itself, keeping the cause it
   already carries rather than becoming its own cause (#3745). */
static SP_TLS int sp_reraise_current = 0;
/* `raise ..., cause: exc`: the explicit cause overrides the implicit
   currently-handled exception for exactly one raise. The `_set` flag records
   that a cause: was given at all, so `cause: nil` suppresses the implicit cause
   rather than reading as "no cause given" (#2990). */
static SP_TLS void *sp_explicit_cause = NULL;
static SP_TLS int sp_explicit_cause_set = 0;
/* The exception handled at each active rescue-body depth (CRuby's per-rescue
   errinfo). The "currently handled" exception -- what Exception#cause threads --
   is the innermost: sp_rescue_sp>0 ? sp_exc_handling[sp_rescue_sp-1] : NULL. A
   depth stack, NOT indexed by sp_exc_top: a rescue body runs with its begin frame
   already popped, so nested rescue bodies share an sp_exc_top. Pushed on rescue
   entry; popped on every exit -- fall-through, return/break/next/retry (codegen
   pops sp_rescue_sp), and raise-out (the landing restores sp_rescue_mark). */
static SP_TLS void *sp_exc_handling[SP_EXC_STACK_MAX];
static SP_TLS int sp_rescue_sp = 0;
/* sp_rescue_sp captured at each begin frame's arm, restored on that frame's
   exception landing so a rescue body that exits by raising doesn't leak its push
   (a side array beside the handler stack, mirroring sp_exc_rootmark). */
static SP_TLS int sp_rescue_mark[SP_EXC_STACK_MAX];
/* The container-walk path's depth (sp_inspect.h) at each handler's arm. Any
   non-local exit out of a guarded walk -- a raise, a throw, a break out of a
   block, a proc's non-local return -- longjmps past the sp_poly_recur_pop calls,
   and the frames left behind would make the NEXT walk over the same objects
   print "[...]", call two different arrays equal, or raise "recursive array
   join" for a structure that never repeated. So EVERY stack that can be landed
   on records the depth where its arm was armed, and every longjmp into one
   restores it: this array beside sp_exc_stack, sp_catch_recur_mark beside the
   catch stack, sp_brk_recur_mark beside the break scopes, and a field on the
   proc-return home node. The exception, catch and break arms are all opened by
   a runtime call (sp_exc_check_depth, sp_catch_check_depth, sp_brk_push), so
   only the proc-return home -- a struct the method builds on its own C stack --
   needs a line in generated code. */
static SP_TLS int sp_poly_recur_mark[SP_EXC_STACK_MAX];
/* Called immediately before every longjmp into sp_exc_stack: the landing frame
   is the one on top, and the walk frames pushed since it armed are about to be
   jumped over, so give the path back the depth that frame recorded. */
static inline void sp_poly_recur_unwind(void) {
  if (sp_exc_top > 0) sp_poly_recur_pop(sp_poly_recur_mark[sp_exc_top - 1]);
}
#define sp_cur_handled() (sp_rescue_sp > 0 ? sp_exc_handling[sp_rescue_sp-1] : NULL)
/* Push a handled exception. sp_rescue_sp grows with recursion *through* rescue
   bodies (the handler stays pushed across the recursive call it makes, unlike a
   begin frame which is popped first), so a fixed SP_EXC_STACK_MAX can be reached;
   fail loudly rather than write past sp_exc_handling. */
static void sp_rescue_push(void *e) {
  if (sp_rescue_sp >= SP_EXC_STACK_MAX) {
    fprintf(stderr, "rescue nesting too deep (> %d)\n", SP_EXC_STACK_MAX);
    exit(1);
  }
  sp_exc_handling[sp_rescue_sp++] = e;
}
/* Each of the fixed-depth handler stacks below fails the same way when a
   program nests deeper than its array holds: CRuby's SystemStackError words,
   on stderr, and out. One copy of them, called from each stack's check. Not a
   Ruby SystemStackError -- catching one needs a handler slot, which is exactly
   what has run out. (sp_rescue_push above keeps its own older wording, which
   names the nesting that overflowed.) */
SP_NORETURN SP_COLD static void sp_stack_too_deep(void) {
  fputs("stack level too deep (SystemStackError)\n", stderr);
  exit(1);
}
/* Guard an exception-frame arm. Every arm stores into sp_exc_rootmark
   [sp_exc_top] (or sp_exc_msg[sp_exc_top]) before it bumps sp_exc_top, so what
   the check must precede is that first indexed store, not the ++; it opens the
   frame-arming sequence at every arm the codegen emits. The depth is not a
   lexical property: a method whose body is a begin/rescue keeps its frame armed
   across the calls that body makes, so ordinary recursion 65 deep reaches the
   end of the array just as 65 nested begins do. Inside a fiber body, and inside
   a thread's, one slot is already spent on the frame the runtime arms around it
   (sp_exc_arm below), so 63 of the body's own is the last that fits. */
static inline void sp_exc_check_depth(void) {
  if (SP_UNLIKELY(sp_exc_top >= SP_EXC_STACK_MAX)) sp_stack_too_deep();
  sp_poly_recur_mark[sp_exc_top] = sp_poly_recur_top;
}
/* ---- Native backtrace formatting (spinel --debug) ---------------------- */
/* True for sp_<name> symbols that are runtime helpers, not user Ruby methods.
   A denylist of the lowercase runtime prefixes; user methods are sp_<rubyname>
   (top-level) or sp_<Class>_<method>, which don't match. Heuristic — a leaf
   runtime frame may occasionally slip through; refine with an emitted
   user-method allowlist later. */
/* sp_bt_format (symbol->Ruby-frame formatting) moved to lib/sp_cold.c. */
sp_StrArray *sp_bt_format(void **buf, int n);

/* Backtrace captured at the most recent raise (for Exception#backtrace). */
static sp_StrArray *sp_backtrace_captured(void) {
#if SP_BT_AVAILABLE
  return sp_bt_format(sp_bt_buf, sp_bt_n);
#else
  return sp_StrArray_new();
#endif
}

/* The current stack, captured now (for Kernel#caller). */
static sp_StrArray *sp_caller_now(void) {
#if SP_BT_AVAILABLE
  if (!sp_bt_enabled) return sp_StrArray_new();
  void *buf[256];
  int n = backtrace(buf, 256);
  return sp_bt_format(buf, n);
#else
  return sp_StrArray_new();
#endif
}

/* Kernel#caller(start=1, length=nil): the current call stack, dropping the
   first `start` frames (the running method itself at index 0, like CRuby's
   `caller` == `caller(1)`), then up to `length` frames. Method-granularity
   (no :lineno:), same substrate as Exception#backtrace; release builds (no
   frame symbols) return []. */
static sp_StrArray *sp_caller(sp_int start, sp_bool have_len, sp_int len) {
  sp_StrArray *full = sp_caller_now();
  SP_GC_ROOT(full);
  sp_StrArray *r = sp_StrArray_new();
  SP_GC_ROOT(r);
  sp_int from = start < 0 ? 0 : start;
  sp_int to = have_len ? from + (len < 0 ? 0 : len) : full->len;
  for (sp_int i = from; i < to && i < full->len; i++) sp_StrArray_push(r, full->data[i]);
  return r;
}

/* Non-local-unwind state (a proc `return` or `throw` in flight while it runs the
   ensures it passes over). Declared here, before sp_raise_cls, so a real raise
   can clear it -- an exception raised inside an ensure during an unwind
   supersedes that unwind. The machinery that uses it lives further down. */
enum { SP_UNWIND_NONE, SP_UNWIND_PROCRET, SP_UNWIND_THROW, SP_UNWIND_BREAK };
struct sp_proc_home;
static SP_TLS int sp_unwind_kind = SP_UNWIND_NONE, sp_unwind_target = -1, sp_unwind_exc_top = 0;  /* per-worker (see sp_exc_stack) */
static SP_TLS struct sp_proc_home *sp_unwind_home = NULL;  /* PROCRET target (THROW uses sp_unwind_target) */
/* forward: materialize the exception object for a raise that carried none
   (a bare `raise "msg"` / `raise Cls, msg` / builtin runtime raise), so `$!`
   and `rescue => e` bind one real object with a stable identity. The struct
   tag is used because the sp_Exception typedef is defined further down; the
   result is stored as a void* in sp_exc_obj[]. */
struct sp_Exception_s;
static void *sp_exc_recover_named(const char *cls, const char *msg);
/* forward: drop the handler-stack entries a raise unwinds past. Defined far
   below, beside the three stacks it pops -- all of them declared after this
   function, which is why the declaration has to come up here. */
static void sp_handler_stacks_unwind(void);
/* forward: run the registered at_exit hooks and answer the exit status the
   program should end with -- `status` is what the path that called it would
   have used, and a hook that exits or raises replaces it (see the definition,
   next to sp_proc_call). Every path below that ends the process the way
   CRuby's `exit`, `abort` or an uncaught raise does calls it first.
   The name keeps the table's own sp_at_exit_ prefix on purpose: mc_top
   (src/codegen_util.c) reserves the first underscore-separated segment of
   every runtime symbol against a top-level `def`, and the table already
   reserves `at`. Naming this one after the verb instead would have newly
   reserved that verb, renaming the emitted symbol for every program with a
   top-level method of that name. The printer below is named for the same
   reason: `exc` is reserved already, where a name built on its verb would
   newly reserve that verb. The list is built by scanning these sources for
   sp_ tokens, comments included, so a comment must not spell one either. */
static int sp_at_exit_run(int status);
/* Print an uncaught exception in CRuby's tail format. Factored out because two
   places print it now: the end of sp_raise_cls, and a hook whose own exception
   reached the drain's protect frame. */
static void sp_exc_print_uncaught(const char *cls, const char *msg);
/* CRuby's tail format "<message> (<ClassName>)", prefixed by the raising frame
   and followed by its callers when the backtrace substrate is live (a --debug
   build). Without it there is no location to print, and an uncaught raise in a
   multi-file program said only what went wrong, never where (#3974). Frames
   are method-granularity, so there is no `:line:`. */
static void sp_exc_print_uncaught(const char *cls, const char *msg) {
#if SP_BT_AVAILABLE
  if (sp_bt_enabled && sp_bt_n > 0) {
    sp_StrArray *bt = sp_bt_format(sp_bt_buf, sp_bt_n);
    if (bt && bt->len > 0) {
      fprintf(stderr, "%s: %s (%s)\n", sp_StrArray_get(bt, 0),
              (msg && *msg) ? msg : cls, cls);
      for (sp_int _i = 1; _i < bt->len; _i++)
        fprintf(stderr, "\tfrom %s\n", sp_StrArray_get(bt, _i));
      return;
    }
  }
#endif
  fprintf(stderr, "%s (%s)\n", (msg && *msg) ? msg : cls, cls);
}
#ifdef SPINEL_EXT_HOST
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);
#else
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg) {
  /* Launder the message onto the string heap and root the copy before anything
     below allocates. `msg` is the caller's own pointer and comes in one of two
     shapes, neither of which survives a collection here. A raiser that formats
     its text -- sp_sprintf, as sp_raise_poly_nomethod and its kin do -- hands
     over a heap string nothing roots, so the sweep inside sp_exc_recover_named,
     inside sp_exc_apply_staged, or inside the slot store below freed it under
     the reads that follow. A bare C literal, the other shape and by far the commoner one, cannot simply
     be rooted instead: it has no marker byte, so sp_mark_string would read -- and sometimes write -- the
     byte in front of it. Copying first and rooting the copy is the idiom
     lib/sp_exc.c documents above sp_exc_new; sp_msg_heapify allocates without
     giving the collector a turn, so there is no window between the two. The
     no-message sentinel keeps its identity: that identity is what tells an
     empty message apart from no message at all (#3711).
     The root is never popped, because this function longjmps -- and that costs
     nothing: every exception landing restores sp_gc_nroots to the watermark it
     recorded before its own setjmp (sp_exc_rootmark, or a C local in the
     Kernel#loop and enumerator landings), which discards this entry, and a
     fiber's trampoline hands the whole root stack back to its resumer. */
  if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg);
  SP_GC_ROOT_STR(msg);
#if SP_BT_AVAILABLE
  if (sp_bt_enabled) sp_bt_n = backtrace(sp_bt_buf, 256);
#endif
  /* A real exception supersedes any non-local unwind in flight (e.g. raised from
     inside an `ensure` running during a proc-return / throw): clear the unwind so
     an outer handler treats this as an exception, not a continued unwind. */
  sp_unwind_kind = SP_UNWIND_NONE;
  /* a bare `raise` inside a rescue re-raises the handled exception itself, so
     it keeps the cause it already has (#3745) */
  if (sp_reraise_current) {
    sp_reraise_current = 0;
    if (!sp_pending_exc_obj && sp_cur_handled()) sp_pending_exc_obj = sp_cur_handled();
  }
  /* NameError/NoMethodError#name: the runtime raisers' messages carry the
     offending name as the first quoted token ("undefined method 'foo' for
     ..."); materialize the carried object with #name set so a rescue binding
     reads it (#2758). Keyed on the exact runtime message shapes. */
  if (!sp_pending_exc_obj && msg && cls && sp_exc_top > 0 &&
      (strcmp(cls, "NoMethodError") == 0 || strcmp(cls, "NameError") == 0) &&
      (strncmp(msg, "undefined method '", 18) == 0 ||
       strncmp(msg, "undefined local variable or method '", 36) == 0 ||   /* 36 = prefix len; 37 compared the NUL too (#3121) */
       strncmp(msg, "private method '", 16) == 0 ||      /* visibility refusals carry the name too */
       strncmp(msg, "protected method '", 18) == 0 ||
       strncmp(msg, "uninitialized constant ", 23) == 0))   /* const_get / bad const (#3034) */
    sp_pending_exc_obj = sp_exc_recover_named(cls, msg);
  /* the introspection staging (receiver/key/value) rides the carried object */
  if (sp_pending_exc_flags && msg && cls && sp_exc_top > 0)
    sp_pending_exc_obj = sp_exc_apply_staged(cls, msg, sp_pending_exc_obj);
  sp_pending_exc_flags = 0;
  /* The slot takes the laundered copy made at the top of this function (or the
     bare-raise sentinel, or NULL) as it stands: it is already a string-heap
     one with a marker byte, so sp_mark_in_flight_exceptions can mark it from
     here to the landing, and copying it a second time would only reintroduce
     an allocation between the last read of the caller's pointer and this
     store. */
  /* Uncaught: CRuby's tail format "<message> (<ClassName>)", prefixed by the
     raising frame and followed by its callers when the backtrace substrate is
     live (a --debug build). Without it there is no location to print, and an
     uncaught raise in a multi-file program said only what went wrong, never
     where (#3974). Frames are method-granularity, so there is no `:line:`. */
  if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_exc_obj[sp_exc_top-1] = sp_pending_exc_obj; sp_pending_exc_obj = NULL; sp_pending_cause = sp_explicit_cause_set ? sp_explicit_cause : (sp_cur_handled() ? sp_cur_handled() : sp_inflight_cause); sp_inflight_cause = NULL; sp_explicit_cause = NULL; sp_explicit_cause_set = 0; sp_last_exc_cls = cls; sp_handler_stacks_unwind(); sp_poly_recur_unwind(); longjmp(sp_exc_stack[sp_exc_top-1], 1); }
  /* Uncaught SystemExit terminates silently with its status (Kernel#exit).
     Read the status BEFORE the hooks run: it lives in the pending exception
     object, which nothing roots once the hooks start allocating. */
  if (strcmp(cls, "SystemExit") == 0) exit(sp_at_exit_run(sp_exc_exit_status(sp_pending_exc_obj)));
  /* An uncaught exception prints its text AFTER the hooks have run, which is
     the order CRuby prints them in, and the status is theirs to change (a hook
     that calls `exit 5` makes an uncaught raise exit 5, still printing the
     error). `msg` survives the hooks because it was laundered onto the string
     heap and rooted at the top of this function and that root is never popped;
     `cls` is a codegen-emitted literal the collector never touches. The
     backtrace substrate is not so lucky: a hook that raises overwrites it, so
     a debug build keeps its own copy across the drain. */
#if SP_BT_AVAILABLE
  void *_bt_keep[256];
  int _bt_keep_n = (sp_bt_enabled && sp_bt_n > 0) ? sp_bt_n : 0;
  if (_bt_keep_n > 0) memcpy(_bt_keep, sp_bt_buf, sizeof(void *) * (size_t)_bt_keep_n);
#endif
  { int status = sp_at_exit_run(1);
#if SP_BT_AVAILABLE
    if (_bt_keep_n > 0) { memcpy(sp_bt_buf, _bt_keep, sizeof(void *) * (size_t)_bt_keep_n); sp_bt_n = _bt_keep_n; }
#endif
    sp_exc_print_uncaught(cls, msg);
    exit(status); } }
#endif
static void sp_raise(const char *msg) { sp_raise_cls("RuntimeError", msg); }

/* Unwrap a boxed Proc -- one read out of a container (#3655). */
static sp_Proc *sp_curry_to_proc(sp_Curry *cy);   /* with the curry machinery below */
static sp_Proc *sp_poly_to_proc(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_PROC) return (sp_Proc *)v.v.p;
  /* a curried Proc converts to the deferring wrapper, so &partial drives a
     block wherever a proc would (#3864) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_CURRY)
    return sp_curry_to_proc((sp_Curry *)v.v.p);
  sp_raise_cls("TypeError", "callable object is expected");
  return NULL;
}

/* A Method read back out of a container has only its sp_BoundMethod: #owner
   comes from the compile-time rendering it carries ("#<Method: Owner#name>"),
   and #receiver from the object's own class id (#3692). */
static const char *sp_bm_owner_name(sp_BoundMethod *m) {
  if (!m || !m->desc) return NULL;
  const char *p = strstr(m->desc, ": ");
  if (!p) return NULL;
  p += 2;
  const char *e = p;
  while (*e && *e != '#' && *e != '.' && *e != '>') e++;
  if (e == p) return NULL;
  { size_t n = (size_t)(e - p);
    char *r = sp_str_alloc(n);
    memcpy(r, p, n); r[n] = 0;
    return r; }
}
/* #parameters from the same rendering: `a`, `b=...`, `*r`, `**k` (#3692). */
static sp_PolyArray *sp_bm_parameters(sp_BoundMethod *m) {
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  if (!m || !m->desc) return out;
  const char *o = strchr(m->desc, '(');
  if (!o) return out;
  const char *p = o + 1;
  while (*p && *p != ')') {
    while (*p == ' ' || *p == ',') p++;
    if (!*p || *p == ')') break;
    const char *kind = "req";
    if (p[0] == '*' && p[1] == '*') { kind = "keyrest"; p += 2; }
    else if (p[0] == '*') { kind = "rest"; p++; }
    const char *ns = p;
    while (*p && *p != ',' && *p != ')' && *p != '=') p++;
    { size_t n = (size_t)(p - ns);
      char nm[128]; if (n >= sizeof nm) n = sizeof nm - 1;
      memcpy(nm, ns, n); nm[n] = 0;
      if (*p == '=') { kind = "opt"; while (*p && *p != ',' && *p != ')') p++; }
      if (n) {
        sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
        sp_PolyArray_push(pair, sp_box_sym(sp_sym_intern(kind)));
        sp_PolyArray_push(pair, sp_box_sym(sp_sym_intern(nm)));
        sp_PolyArray_push(out, sp_box_poly_array(pair));
      }
    }
  }
  return out;
}
static sp_RbVal sp_bm_receiver(sp_BoundMethod *m) {
  if (!m || !m->self) return sp_box_nil();
  if (m->self_kind == SP_BM_SELF_STR) return sp_box_str((const char *)m->self);
  if (m->self_kind == SP_BM_SELF_OBJ) return sp_box_obj(m->self, *(sp_int *)m->self);
  return sp_box_nil();
}

/* Float#round(half: mode) where the mode is only known at run time (#3646).
   A nil mode is the default (round half up, away from zero). */
static double sp_round_half_mode(double x, sp_sym mode) {
  const char *m = (mode == (sp_sym)-1) ? NULL : sp_sym_to_s(mode);
  if (!m || !m[0]) return round(x);
  if (strcmp(m, "even") == 0) return sp_round_half_even(x);
  if (strcmp(m, "down") == 0) return sp_round_half_down(x);
  if (strcmp(m, "up") == 0) return round(x);
  sp_raise_cls("ArgumentError", sp_sprintf("invalid rounding mode: %s", m));
  return 0.0;
}

/* `rescue *list`: the clause matches when the raised class is (or descends
   from) one named in the list. A non-class element is a TypeError, and an
   empty list matches nothing, so the exception keeps propagating (#3712). */
static sp_bool sp_exc_matches_splat(const char *raised, sp_RbVal list) {
  if (!(list.tag == SP_TAG_OBJ && sp_poly_is_array_kind(list.cls_id))) {
    if (list.tag == SP_TAG_CLASS)
      return sp_exc_cls_matches(raised, sp_class_val_name(list));
    sp_raise_cls("TypeError", "class or module required for rescue clause");
  }
  { sp_int n = sp_poly_length(list);
    for (sp_int i = 0; i < n; i++) {
      sp_RbVal e = sp_poly_arr_get(list, i);
      if (e.tag != SP_TAG_CLASS)
        sp_raise_cls("TypeError", "class or module required for rescue clause");
      if (sp_exc_cls_matches(raised, sp_class_val_name(e))) return TRUE;
    }
  }
  return FALSE;
}

/* Issue #781: bridge between the regex compile-error path (which lives
   in the .a library and can't see the user program's static-inline
   sp_raise_cls) and the user's Ruby-level exception machinery. The
   library calls sp_re_set_error_handler at startup -- codegen emits
   the install call after the exception infrastructure is set up. */
/* Issue #846: during sp_re_init (before main()'s setjmp scope is
   active), a bad literal `Regexp.new("[invalid")` pattern would
   route through sp_raise_cls -> "unhandled exception" + exit
   because sp_exc_top is 0. Install a startup handler that longjmps
   back to sp_re_init's wrapping setjmp; the codegen-emitted loop
   then stashes the error per slot for a deferred raise from the
   first use site (where the user's begin/rescue is active). The
   re_compile contract requires the error callback to not return
   (otherwise the library's fall-through fprintf+exit fires) so
   we longjmp out. */
static jmp_buf sp_re_startup_jmp;
static void sp_re_startup_error_handler(const char *msg) {
  if (msg) {
    size_t n = strlen(msg);
    char *buf = sp_str_alloc_raw(n + 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    sp_re_startup_err = buf;
  }
  longjmp(sp_re_startup_jmp, 1);
}
/* A LITERAL pattern the engine refuses is CRuby's parse-time error: the file
   does not run at all. Startup compiles every literal, so report there and
   stop, rather than leaving the slot NULL for the first match to dereference
   (a segfault, and the message lost with it). */
static SP_NOINLINE void sp_re_startup_fail(void) {
  fprintf(stderr, "%s (RegexpError)\n",
          sp_re_startup_err ? sp_re_startup_err : "invalid regexp literal");
  exit(1);
}
extern void sp_re_set_error_handler(void (*fn)(const char *msg));
static void sp_mark_in_flight_exceptions(void) {
  /* <=: a rescue arm pops its frame (sp_exc_top--) BEFORE materializing the
     exception from the popped slot, and that materialization allocates -- the
     just-consumed message/object at [sp_exc_top] must survive it. */
  for (int i = 0; i <= sp_exc_top && i < SP_EXC_STACK_MAX; i++) {
    sp_mark_string(sp_exc_msg[i]);
    if (sp_exc_obj[i]) sp_gc_mark(sp_exc_obj[i]);  /* carried subclass object + its ivars */
  }
  /* Everything ABOVE the window is dead by definition, so drop it here rather
     than trust every pop site to. The window follows sp_exc_top up and down,
     and a slot that leaves it keeps its pointer: pop below a used slot, collect
     (the object is now unreferenced and is freed), then push back over it, and
     the window covers a dangling pointer again. Clearing at the one place that
     defines the window makes that sequence impossible -- after any collection,
     a slot outside it holds nothing (#3404). */
  for (int i = sp_exc_top + 1; i < SP_EXC_STACK_MAX; i++) {
    sp_exc_obj[i] = NULL;
    sp_exc_msg[i] = NULL;
  }
  if (sp_pending_exc_obj) sp_gc_mark(sp_pending_exc_obj);
  if (sp_pending_cause) sp_gc_mark(sp_pending_cause);
  if (sp_inflight_cause) sp_gc_mark(sp_inflight_cause);
  for (int i = 0; i < sp_rescue_sp; i++)
    if (sp_exc_handling[i]) sp_gc_mark(sp_exc_handling[i]);  /* handled exceptions */
}

/* sp_Exception: first-class exception object. cls_name is a pointer
   to the per-class const string literal emitted by codegen
   (sp_class_names[] entry; not GC-managed). msg is GC-managed
   (sp_str_alloc'd). */
/* Registered by the generated program to provide user exception hierarchy. */
#ifdef SPINEL_EXT_HOST
extern const char *(*sp_user_exc_parent_fn)(const char *);
#else
const char *(*sp_user_exc_parent_fn)(const char *) = NULL;
#endif
/* Build the carried NameError/NoMethodError with #name recovered from the
   message's first quoted token (see sp_raise_cls, #2758). */
static void *sp_exc_recover_named(const char *cls, const char *msg) {
  char nb[128];
  const char *q1 = strchr(msg, '\'');
  const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
  if (q2 && q2 > q1 + 1 && (size_t)(q2 - q1) <= 128) {
    size_t n = (size_t)(q2 - q1 - 1);
    memcpy(nb, q1 + 1, n); nb[n] = 0;
  }
  else {
    /* "uninitialized constant NAME": the name is the unquoted trailing token,
       the constant the message reports as missing (#3034) */
    const char *pfx = "uninitialized constant ";
    if (strncmp(msg, pfx, 23) != 0) return NULL;
    const char *p = msg + 23;
    size_t n = 0;
    while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == ':') && n < 127) n++;
    if (n == 0) return NULL;
    memcpy(nb, p, n); nb[n] = 0;
  }
  sp_Exception *e = sp_exc_new(cls, msg);   /* launders msg into the GC heap */
  SP_GC_ROOT(e);
  e->xname = sp_box_sym(sp_sym_intern(nb));
  return e;
}
/* Kernel#exit: raises a rescuable SystemExit carrying the status; with no
   handler in scope it terminates directly (#2761). exit! stays direct. */
static void sp_raise_exc(volatile sp_Exception *ve);   /* defined below */
SP_NORETURN SP_COLD static void sp_exit_raise(int status) {
  if (sp_exc_top > 0) {
    sp_Exception *e = sp_exc_new("SystemExit", "exit");
    SP_GC_ROOT(e);
    e->result = sp_box_int((sp_int)status);
    sp_raise_exc((volatile sp_Exception *)e);
  }
  exit(sp_at_exit_run(status));
}
/* Kernel#abort: write the message to stderr, then raise a rescuable
   SystemExit with status 1 carrying that message (#3077). */
SP_NORETURN SP_COLD static void sp_abort_raise(const char *msg) {
  if (msg) { fputs(msg, stderr); fputc('\n', stderr); }
  if (sp_exc_top > 0) {
    sp_Exception *e = sp_exc_new("SystemExit", msg ? msg : "exit");
    SP_GC_ROOT(e);
    e->result = sp_box_int((sp_int)1);
    sp_raise_exc((volatile sp_Exception *)e);
  }
  exit(sp_at_exit_run(1));
}
static void sp_raise_exc(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e) sp_raise("(nil exception)");
  /* Carry the object so a user subclass keeps its ivars across the
     longjmp; sp_raise_cls moves it into the current frame's slot. */
  sp_pending_exc_obj = (void *)e;
  sp_raise_cls(e->cls_name, e->msg);
}
/* Runtime class table: set of cls_ids that are user exception subclasses.
   Populated by the codegen (src/codegen.c) for every class that
   `class_is_exc_subclass` returns true. Used by sp_raise_poly to
   verify a boxed object is an exception subclass before re-raising
   it -- reading the sp_Exception prefix on a non-exception user
   object is a wrong-offset read (segfault). */
extern const sp_int sp_exc_subclass_ids[];
extern const sp_int sp_exc_subclass_count;

static inline int sp_is_exc_subclass_cls(sp_int cls_id) {
  for (sp_int i = 0; i < sp_exc_subclass_count; i++)
    if (sp_exc_subclass_ids[i] == cls_id) return 1;
  return 0;
}

/* Kernel#raise with a runtime-typed (poly) operand: a String raises
   RuntimeError with it, a carried exception object re-raises as itself,
   anything else is CRuby's TypeError. */
SP_NORETURN SP_COLD static void sp_raise_poly(sp_RbVal v) {
  if (v.tag == SP_TAG_STR && v.v.s) sp_raise(v.v.s);
  if (v.tag == SP_TAG_OBJ && v.v.p) {
    /* A carried exception object re-raises as itself. The base
     * sp_Exception uses cls_id SP_BUILTIN_EXCEPTION; a user subclass
     * has a positive cls_id registered in sp_exc_subclass_ids. Both
     * shapes are re-raised here. Any other boxed object (a non-exception
     * user class) would have a different struct layout -- reading
     * the sp_Exception prefix at this offset is undefined and segfaults
     * under clang's aggressive stack hoisting (#XXXX). */
    if (v.cls_id == SP_BUILTIN_EXCEPTION)
      sp_raise_exc((volatile sp_Exception *)v.v.p);
    else if (sp_is_exc_subclass_cls(v.cls_id))
      sp_raise_exc((volatile sp_Exception *)v.v.p);
  }
  /* A Class VALUE naming an exception class raises that class, exactly as the
     constant does: `k = App::Failed; raise k` and `[A, B].each { |k| raise k }`
     are the same raise as `raise App::Failed`. Only the literal-constant form
     was recognized, so reaching the class through a variable answered TypeError.
     The empty message is what the literal form emits too; sp_raise_cls fills in
     the class name, which is what CRuby uses as the message. */
  if (v.tag == SP_TAG_CLASS) {
    const char *cn = sp_class_val_name(v);
    /* Must be a KNOWN exception class. sp_exc_cls_matches(x, "Exception") is
       not the test: a name with no recorded parent is assumed to be a user
       exception rooted at Exception (the open-namespace rule Errno:: relies
       on), so it would answer yes for String and raise it. Ask for a parent
       instead, which only a registered exception class has. */
    if (cn && *cn && (!strcmp(cn, "Exception") ||
                      (sp_user_exc_parent_fn && sp_user_exc_parent_fn(cn)) ||
                      sp_exc_parent_of_name(cn)))
      sp_raise_cls(cn, sp_str_empty);
  }
  sp_raise_cls("TypeError", "exception class/object expected");
}
/* Raise StopIteration carrying the iteration's return value as #result. Built as
   a carried object so a `rescue StopIteration => e` binding reads e.result; a
   generator supplies the value, a plain past-the-end #next raises with nil. */
#ifdef SPINEL_EXT_HOST
SP_NORETURN void sp_raise_stop_iteration(sp_RbVal result);
#else
SP_NORETURN void sp_raise_stop_iteration(sp_RbVal result) {
  /* Marker-prefixed message: sp_mark_string reads msg[-1] as the GC marker, so a
     bare rodata literal would be an out-of-bounds read at a section edge. */
  const char *msg = (&("\xff" "iteration reached an end")[1]);
  sp_Exception *e = sp_exc_new("StopIteration", msg);
  e->result = result;
  sp_pending_exc_obj = (void *)e;
  sp_raise_cls("StopIteration", msg);
}
#endif
/* Exception#is_a?(ClassName): checks class name and known hierarchy. */
static sp_int sp_exc_is_a(volatile sp_Exception *ve, const char *cn) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e || !cn) return 0;
  /* one authority for "does this level answer to cn", modules included: the
     matcher rescue arms use. Without it #is_a?(SomeModule) said false where
     `rescue SomeModule` said yes (#3366 follow-up). */
  cn = sp_exc_canonical_name(cn);
  if (sp_exc_cls_matches(e->cls_name, cn)) return 1;
  /* find the exception's class chain and check if cn appears in it */
  const char *cls = e->cls_name;
  int used_parent = 0;
  for (int depth = 0; depth < 20 && cls; depth++) {
    if (!strcmp(cls, cn)) return 1;
    const char *parent = sp_exc_parent_of_name(cls);
    if (!parent) {
      /* unknown (user) class: try user hierarchy first */
      if (sp_user_exc_parent_fn) { parent = sp_user_exc_parent_fn(cls); }
      if (!parent) {
        if (!used_parent && e->parent_cls_name) {
          cls = e->parent_cls_name;
          used_parent = 1;
          continue;
        }
        if (!strcmp(cn, "Exception")) return 1;
        if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject")) return 1;
        break;
      }
    }
    cls = parent;
  }
  if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject") || !strcmp(cn, "Kernel")) return 1;
  return 0;
}

/* a reason/tag staged as a plain string interns back to the Symbol it names */
static sp_RbVal sp_exc_sym_slot(sp_RbVal v) {
  if (v.tag == SP_TAG_STR && v.v.s) return sp_box_sym(sp_sym_intern(v.v.s));
  return v;
}
static sp_RbVal sp_poly_exc_acc(sp_RbVal v, const char *which) {
  if (!(v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_EXCEPTION && v.v.p))
    sp_raise_cls("NoMethodError",
                 sp_sprintf("undefined method '%s' for an instance of %s", which, sp_poly_class_name(v)));
  sp_Exception *e = (sp_Exception *)v.v.p;
  if (!strcmp(which, "message")) return sp_box_str(sp_exc_message(e));
  if (!strcmp(which, "result"))  return sp_exc_result(e);
  if (!strcmp(which, "key"))     return sp_exc_key_acc(e);
  if (!strcmp(which, "receiver")) return sp_exc_receiver_acc(e);
  if (!strcmp(which, "name"))    return sp_exc_name_acc(e);
  return sp_box_nil();
}
static sp_RbVal sp_exc_reason_acc(sp_Exception *e) {
  sp_exc_acc_gate(e, "LocalJumpError", "reason");
  return sp_exc_sym_slot(e->xkey);
}
static sp_RbVal sp_exc_tag_acc(sp_Exception *e) {
  sp_exc_acc_gate(e, "UncaughtThrowError", "tag");
  return sp_exc_sym_slot(e->xkey);
}


/* Cross-TU bridge for sp_bigint.c (compiled as a separate translation
   unit; can't see static helpers in this header). Defined non-static
   so sp_bigint.c's mrb_raise macro can dispatch into spinel's
   longjmp-based rescue net rather than fprintf+exit. */
#ifdef SPINEL_EXT_HOST
void sp_bigint_raise_zerodiv(const char *msg);
#else
void sp_bigint_raise_zerodiv(const char *msg) { sp_raise_cls("ZeroDivisionError", msg); }
#endif
/* sp_exc_is_a: see earlier definition (takes volatile sp_Exception *) */

/* A non-local control-flow unwind -- a proc `return` or a `throw` -- runs the
   `ensure` blocks it passes over before delivering to its target, like an
   exception does. It first longjmps through the sp_exc_stack handler chain (each
   handler runs its ensure), bounded by the exception depth recorded at the
   target's entry, then delivers to the target. sp_unwind_resume drives each step;
   the codegen-emitted exception handlers call it when an unwind is in flight.
   The SP_UNWIND_* enum and sp_unwind_* state are declared earlier (before
   sp_raise_cls, which clears them when a real exception supersedes an unwind). */

#define SP_CATCH_STACK_MAX 64
static SP_TLS jmp_buf sp_catch_stack[SP_CATCH_STACK_MAX];   /* per-worker (see sp_exc_stack) */
static SP_TLS const char *sp_catch_tag[SP_CATCH_STACK_MAX];
/* 0 = name tag (symbol/string, matched by content); 1 = object tag (matched
   by pointer identity, CRuby's non-symbol tag semantics) */
static SP_TLS unsigned char sp_catch_tag_kind[SP_CATCH_STACK_MAX];
/* boxed value channel, like sp_brk_val: any thrown value carries faithfully */
static SP_TLS sp_RbVal sp_catch_val[SP_CATCH_STACK_MAX];
static SP_TLS int sp_catch_exc_top[SP_CATCH_STACK_MAX];  /* exception depth at each catch's entry */
static SP_TLS int sp_catch_rootmark[SP_CATCH_STACK_MAX]; /* GC-root watermark at entry (see sp_exc_rootmark) */
static SP_TLS int sp_catch_recur_mark[SP_CATCH_STACK_MAX]; /* walk-path depth at entry (see sp_poly_recur_mark) */
static SP_TLS volatile int sp_catch_top = 0;
/* Guard a catch push, like sp_exc_check_depth: the arm's first store is
   sp_catch_tag[sp_catch_top], so this runs in front of it -- which also makes
   it the one place every catch arm passes through, so the walk path's depth is
   recorded here rather than in the emitted arm. */
static inline void sp_catch_check_depth(void) {
  if (SP_UNLIKELY(sp_catch_top >= SP_CATCH_STACK_MAX)) sp_stack_too_deep();
  sp_catch_recur_mark[sp_catch_top] = sp_poly_recur_top;
}
/* shared counter (not SP_TLS) so `catch { |tag| }` autotags are globally
   unique; see sp_brk_seq for the same shape */
static sp_int sp_catch_seq = 0;
static void sp_throw(const char *tag, int kind, sp_RbVal val) {
  int i = sp_catch_top - 1;
  while (i >= 0) {
    if (sp_catch_tag_kind[i] == kind &&
        (kind ? sp_catch_tag[i] == tag : strcmp(sp_catch_tag[i], tag) == 0)) {
      sp_catch_val[i] = val; sp_catch_top = i + 1;
      if (sp_exc_top > sp_catch_exc_top[i]) {       /* run intervening ensures first */
        sp_unwind_kind = SP_UNWIND_THROW; sp_unwind_target = i; sp_unwind_exc_top = sp_catch_exc_top[i];
        sp_poly_recur_unwind();
        longjmp(sp_exc_stack[sp_exc_top - 1], 1);
      }
      sp_poly_recur_pop(sp_catch_recur_mark[i]);
      longjmp(sp_catch_stack[i], 1);
    }
    i--;
  }
  sp_exc_stage_val(val);
  if (kind) sp_raise_cls("UncaughtThrowError", "uncaught throw");
  sp_exc_stage_key(sp_box_str(tag));   /* #tag; the accessor interns it back to a symbol */
  sp_raise_cls("UncaughtThrowError", sp_sprintf("uncaught throw :%s", tag));
}

/* ---- valued `break` from a block (CRuby's TAG_BREAK) ----------------------
   A `break <v>` inside a block makes the call that received the block return
   <v>, through any number of intervening frames. Every wrapped block-taking
   call pushes a break scope inside a setjmp; the scope's SERIAL (a fresh,
   never-reused id, like the proc-return home ids) is what a break addresses,
   because nested live scopes make top-of-stack targeting ambiguous: in
   `def m; [1].map { yield }; end; m { break 5 }` the spliced outer block
   executes inside the inner map's scope, yet its break belongs to m's call.
   Non-lambda procs capture their creation scope's serial the same way; a miss
   (dead or foreign scope) is CRuby's LocalJumpError "break from proc-closure".
   A break with intervening exception frames routes through sp_exc_stack first
   (SP_UNWIND_BREAK) so `ensure` bodies run, exactly like sp_throw and
   sp_proc_return above. The value channel is poly so any break value carries
   faithfully. Per-worker (SP_TLS) and fiber-context-saved like the catch
   arrays; the push is bounded like sp_exc_arm / the catch push.
   The backing storage is heap-allocated on the first push (like
   sp_gc_mark_stack), NOT inline TLS arrays: ~14KB of TLS (a jmp_buf is
   ~200 bytes) shifts every hot TLS variable's layout and cost optcarrot
   ~8% fps in a program that never breaks from a block. TLS holds only
   the pointers, so break-free programs pay nothing. */
#define SP_BRK_STACK_MAX 64
static SP_TLS jmp_buf *sp_brk_stack;              /* lazily allocated */
static SP_TLS sp_RbVal *sp_brk_val;
static SP_TLS sp_int *sp_brk_serial;
static SP_TLS int *sp_brk_exc_top;                /* sp_exc_top at scope entry */
static SP_TLS int *sp_brk_recur_mark;            /* walk-path depth at scope entry (see sp_poly_recur_mark) */
static SP_TLS volatile int sp_brk_top = 0;
/* shared counter (not SP_TLS) so serials are globally unique; see
   sp_proc_home_seq for the same shape */
static sp_int sp_brk_seq = 1;
static sp_int sp_brk_push(void) {
  /* Fixed-depth stack like sp_exc / sp_catch: guard the push so pathological
     nesting (e.g. deep recursion through .each/.map) fails loudly instead of
     writing past the array. */
  if (SP_UNLIKELY(sp_brk_top >= SP_BRK_STACK_MAX)) sp_stack_too_deep();
  if (!sp_brk_stack) {
    sp_brk_stack = (jmp_buf *)malloc(sizeof(jmp_buf) * SP_BRK_STACK_MAX);
    sp_brk_val = (sp_RbVal *)malloc(sizeof(sp_RbVal) * SP_BRK_STACK_MAX);
    sp_brk_serial = (sp_int *)malloc(sizeof(sp_int) * SP_BRK_STACK_MAX);
    sp_brk_exc_top = (int *)malloc(sizeof(int) * SP_BRK_STACK_MAX);
    sp_brk_recur_mark = (int *)malloc(sizeof(int) * SP_BRK_STACK_MAX);
    if (!sp_brk_stack || !sp_brk_val || !sp_brk_serial || !sp_brk_exc_top ||
        !sp_brk_recur_mark)
      sp_oom_die();
  }
#ifdef SP_THREADS
  sp_int s = __atomic_fetch_add(&sp_brk_seq, 1, __ATOMIC_RELAXED);
#else
  sp_int s = sp_brk_seq++;
#endif
  sp_brk_serial[sp_brk_top] = s;
  sp_brk_exc_top[sp_brk_top] = sp_exc_top;
  sp_brk_recur_mark[sp_brk_top] = sp_poly_recur_top;
  sp_brk_val[sp_brk_top] = sp_box_nil();
  sp_brk_top++;
  return s;
}
static void __attribute__((noreturn)) sp_brk_throw(sp_int serial, sp_RbVal v) {
  for (int i = sp_brk_top - 1; i >= 0; i--) {
    if (sp_brk_serial[i] != serial) continue;
    sp_brk_val[i] = v;
    sp_brk_top = i + 1;                    /* drop scopes above the target */
    if (sp_exc_top > sp_brk_exc_top[i]) {  /* run intervening ensures first */
      sp_unwind_kind = SP_UNWIND_BREAK; sp_unwind_target = i;
      sp_unwind_exc_top = sp_brk_exc_top[i];
      sp_poly_recur_unwind();
      longjmp(sp_exc_stack[sp_exc_top - 1], 1);
    }
    sp_poly_recur_pop(sp_brk_recur_mark[i]);
    longjmp(sp_brk_stack[i], 1);
  }
  /* no live scope carries the serial: an escaped/foreign proc's break. An
     inlined block's serial is always live, so this is unreachable for those. */
  sp_exc_stage_key(sp_box_str((&("\xff" "break")[1])));
  sp_exc_stage_val(v);   /* LocalJumpError#exit_value = the valued break (#3024) */
  sp_raise_cls("LocalJumpError", "break from proc-closure");
}
/* GC: the in-flight break values -- ensures between the throw and the landing
   can allocate and collect. Called from sp_re_mark_globals. */
static void sp_mark_brk_vals(void) {
  for (int i = 0; i < sp_brk_top; i++) sp_mark_rbval(sp_brk_val[i]);
  /* in-flight catch/throw values: same throw-to-landing lifetime story
     (slots are nil-initialized at catch entry, so every entry is valid) */
  for (int i = 0; i < sp_catch_top; i++) sp_mark_rbval(sp_catch_val[i]);
  /* cause chain in flight between a raise and its handler */
  if (sp_pending_cause) sp_gc_mark(sp_pending_cause);
  if (sp_inflight_cause) sp_gc_mark(sp_inflight_cause);
  if (sp_explicit_cause) sp_gc_mark(sp_explicit_cause);
  /* introspection values staged between a raise and its handler */
  if (sp_pending_exc_flags & 1) sp_mark_rbval(sp_pending_exc_recv);
  if (sp_pending_exc_flags & 2) sp_mark_rbval(sp_pending_exc_key);
  if (sp_pending_exc_flags & 4) sp_mark_rbval(sp_pending_exc_val);
  /* installed Signal.trap proc handlers stay live */
  for (int si = 0; si < SP_SIG_MAX; si++)
    if (sp_trap_proc[si]) sp_gc_mark(sp_trap_proc[si]);
}

/* ---- non-lambda proc `return` (non-local return to the home method) -------
   A non-lambda proc's `return` returns from the method that created the proc.
   The home method declares a `sp_proc_home` node on its own C stack and links it
   onto a per-fiber chain (sp_proc_ret_head), capturing the node's fresh,
   never-reused id into every returning proc it creates. The proc's `return`
   walks the chain for that id and longjmps to the node's setjmp target, so the
   home method returns the boxed value.

   This mirrors CRuby's EC_PUSH_TAG tag chain: each node rides the home method's
   C-stack frame (so depth is bounded by the C stack, like ordinary recursion,
   not a fixed array), and the single chain head is swapped per fiber via
   sp_exc_ctx, so fibers are isolated by construction. An escaped proc whose home
   has returned -- or one called from another fiber, whose home node is on a
   different (inactive) chain -- finds no matching id and raises LocalJumpError,
   matching CRuby. Like sp_throw, a proc return runs the `ensure` blocks it passes
   over (see the unwind machinery above); when there are none it longjmps straight
   home. The `exc_top` field records the exception-handler depth at the method's
   entry so intervening ensures run and so an exception that unwinds the home pops
   the node (sp_handler_stacks_unwind). */
typedef struct sp_proc_home {
  jmp_buf jb;                 /* the home method's setjmp target (on its C stack) */
  sp_RbVal val;               /* the in-flight return value (nil until delivered) */
  int exc_top;                /* sp_exc_top at the method's entry */
  int catch_top;              /* sp_catch_top at the method's entry */
  int recur_mark;             /* walk-path depth at the method's entry (see sp_poly_recur_mark) */
  sp_int id;                 /* fresh id captured by the home's returning procs */
  struct sp_proc_home *prev;  /* enclosing home, forming the per-fiber chain */
} sp_proc_home;
/* sp_proc_ret_head is the current fiber's chain of in-flight proc-return homes,
   swapped per fiber by sp_exc_ctx_save/load -- per-worker (SP_TLS) under N>1 like
   the exception stack. sp_proc_home_seq stays a single shared counter so home ids
   are globally unique across workers (a lambda may be called on a different
   worker than it was created on); the bump is atomic in the threaded build. */
static SP_TLS sp_proc_home *sp_proc_ret_head = NULL;
static sp_int sp_proc_home_seq = 0;
static sp_int sp_proc_home_next(void) {
#ifdef SP_THREADS
  return __atomic_fetch_add(&sp_proc_home_seq, 1, __ATOMIC_RELAXED);
#else
  return sp_proc_home_seq++;
#endif
}
static void sp_proc_return(sp_int id, sp_RbVal v) {
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev) {
    if (h->id == id) {
      h->val = v;
      if (sp_exc_top > h->exc_top) {   /* run intervening ensures first */
        sp_unwind_kind = SP_UNWIND_PROCRET; sp_unwind_home = h; sp_unwind_exc_top = h->exc_top;
        sp_poly_recur_unwind();
        longjmp(sp_exc_stack[sp_exc_top - 1], 1);
      }
      sp_poly_recur_pop(h->recur_mark);
      longjmp(h->jb, 1);
    }
  }
  sp_exc_stage_key(sp_box_str((&("\xff" "return")[1])));
  sp_exc_stage_val(v);   /* LocalJumpError#exit_value carries the returned value (#3024) */
  sp_raise_cls("LocalJumpError", "unexpected return");
}
/* Drop the catch entries, break scopes and proc-return homes a raise unwinds
   past. sp_raise_cls calls this just before it longjmps to sp_exc_stack
   [sp_exc_top-1]; that handler is the frame the exception lands in, and every
   entry opened INSIDE it dies with the C frames the longjmp skips over.

   Each of the three records the exception depth at its own entry, so "opened
   inside the landing frame" reads the same on all of them: the landing frame
   was armed at sp_exc_top-1, so anything entered after it recorded sp_exc_top
   or more, and anything entered before it recorded less.

   The raise is the one place this can be done once. A landing pops the same
   entries with `> sp_exc_top` after its own sp_exc_top--, but only six of the
   ten emitted landings ever did so, and the four that did not -- Kernel#loop
   in either form, the enumerator fold and the ext-host try -- leaked every
   entry that unwound through them. Here it also costs the emitted code
   nothing.

   Not called for a proc return, a throw or a valued break: those longjmp to
   sp_exc_stack themselves, and each trims its own target stack first
   (sp_throw sets sp_catch_top, sp_brk_throw sets sp_brk_top, sp_proc_return
   walks to a live home), so the entries between here and the target are still
   the ones they mean to deliver to. */
static void sp_handler_stacks_unwind(void) {
  /* a dead catch entry is worse than a leak: sp_catch_top is fixed at 64, so a
     raise out of a catch in a loop walks it off the end -- and a later `throw`
     that matches one longjmps into a finished frame */
  while (sp_catch_top > 0 && sp_catch_exc_top[sp_catch_top - 1] >= sp_exc_top)
    sp_catch_top--;
  /* a home node whose method the raise unwinds past: a later proc-return to it
     must miss and raise LocalJumpError, not longjmp into a freed C frame */
  while (sp_proc_ret_head && sp_proc_ret_head->exc_top >= sp_exc_top)
    sp_proc_ret_head = sp_proc_ret_head->prev;
  /* and a break scope, addressed by serial, for the same reason */
  while (sp_brk_top > 0 && sp_brk_exc_top[sp_brk_top - 1] >= sp_exc_top)
    sp_brk_top--;
}
/* GC: mark the current fiber's chain of in-flight return values (suspended fibers
   are handled by sp_exc_ctx_mark). Each node's val is sp_box_nil() until a return
   is delivered, so this is cheap and safe when no return is in flight. */
static void sp_mark_proc_homes(void) {
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev) sp_mark_rbval(h->val);
}

#ifdef SP_THREADS
/* Stop-the-world support: push this worker's per-worker in-flight GC roots --
   pending exception objects and proc-return home values, both thread-local --
   onto its shadow stack so the collector marks them while the worker is parked
   (sp_safepoint_publish_hook, sp_sched.c). The caller snapshots and then restores
   the root-stack depth, exactly as sp_re_push_match_roots does for the regex
   globals. Only in the threaded build (the single-threaded one never parks). */
static void sp_publish_worker_roots(void) {
  for (int i = 0; i < sp_exc_top; i++) if (sp_exc_obj[i]) _sp_gc_root_push((void **)&sp_exc_obj[i]);
  if (sp_pending_exc_obj) _sp_gc_root_push((void **)&sp_pending_exc_obj);
  if (sp_pending_cause) _sp_gc_root_push((void **)&sp_pending_cause);
  for (int i = 0; i < sp_rescue_sp; i++)
    if (sp_exc_handling[i]) _sp_gc_root_push((void **)&sp_exc_handling[i]);
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev)
    _sp_gc_root_push((void **)((uintptr_t)&h->val | (uintptr_t)1));   /* sp_RbVal root */
  for (int i = 0; i < sp_brk_top; i++)
    _sp_gc_root_push((void **)((uintptr_t)&sp_brk_val[i] | (uintptr_t)1));
  /* The boxed proc calling-convention channel is per-worker too, and the
     globals hook marks only the collecting worker's copy. Whatever a boxed
     call last left in another worker's slots -- an argument, or a returned
     object nothing else names any more -- is a root on that worker until its
     next boxed call, exactly as it is on one worker (where the globals hook
     reaches it every cycle). Unpublished, a collection elsewhere frees it and
     the owning worker's next collection marks a freed pointer. */
  _sp_gc_root_push((void **)((uintptr_t)&_sp_proc_poly_ret | (uintptr_t)1));
  for (int i = 0; i < 16; i++)
    _sp_gc_root_push((void **)((uintptr_t)&_sp_proc_poly_args[i] | (uintptr_t)1));
}
__attribute__((constructor)) static void sp_install_safepoint_publish(void) {
  sp_safepoint_publish_hook = sp_publish_worker_roots;
}
#endif
/* Continue a non-local unwind after a handler has run its ensure: if more ensure
   handlers lie between here and the target, longjmp to the next; otherwise
   deliver to the target (the proc-return home node, or the matched catch slot). */
static void sp_unwind_resume(void) {
  if (sp_exc_top > sp_unwind_exc_top) { sp_poly_recur_unwind(); longjmp(sp_exc_stack[sp_exc_top - 1], 1); }
  int kind = sp_unwind_kind;
  sp_unwind_kind = SP_UNWIND_NONE;
  /* the ensures are done; deliver, giving the walk path back the depth the
     target arm recorded (the intervening exception frames restored their own on
     the way here) */
  if (kind == SP_UNWIND_PROCRET) {
    sp_poly_recur_pop(sp_unwind_home->recur_mark);
    longjmp(sp_unwind_home->jb, 1);
  }
  if (kind == SP_UNWIND_BREAK) {
    sp_poly_recur_pop(sp_brk_recur_mark[sp_unwind_target]);
    longjmp(sp_brk_stack[sp_unwind_target], 1);
  }
  sp_poly_recur_pop(sp_catch_recur_mark[sp_unwind_target]);
  longjmp(sp_catch_stack[sp_unwind_target], 1);
}

/* ---- Per-fiber exception/catch handler context (#1474) -------------------
   The handler stacks above are process-global, but begin/rescue handlers and
   catch tags are stack-frame-bound: a fiber that yields while holding one must
   not let another fiber's raise/throw longjmp into its suspended frame. So
   sp_fiber.c saves the live prefix [0..top] of every handler array into the
   outgoing fiber's context and loads the incoming fiber's at each switch; the
   arrays then never alias across fibers. Only the active prefix is copied
   (top == 0 for a fiber that never rescues, e.g. optcarrot's PPU, so it is
   free there). These are non-static: reached by name from libspinel_rt.a. */
typedef struct {
  jmp_buf *es; const char **em; const char **ec; void **eo; int en, ecap;
  jmp_buf *cs; const char **ct; unsigned char *ctk; sp_RbVal *cv; int *cet;  int cn, ccap;
  jmp_buf *bs; sp_RbVal *bv; sp_int *bser; int *bet;     int bn, bcap;  /* break scopes */
  sp_proc_home *prhead;  /* this fiber's proc-return chain head (nodes on its C stack) */
  int uk, ut, ue; sp_proc_home *uh;  /* transient unwind state (in flight only while running ensures) */
  void **shand; int rn, rcap;        /* sp_exc_handling prefix [0..sp_rescue_sp) */
  void *pcause;                      /* sp_pending_cause */
  sp_poly_recur_frame *rrf; int rrn, rrcap;  /* sp_poly_recur_stack prefix [0..sp_poly_recur_top) */
  int *rrem, *rrcm, *rrbm;           /* the walk-path marks of the exception, catch and break arms */
} sp_exc_ctx_t;

#ifdef SPINEL_EXT_HOST
void *sp_exc_ctx_new(void);
#else
void *sp_exc_ctx_new(void) { return calloc(1, sizeof(sp_exc_ctx_t)); }
#endif
#ifdef SPINEL_EXT_HOST
void sp_exc_ctx_free(void *p);
#else
void sp_exc_ctx_free(void *p) {
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
  free(x->es); free(x->em); free(x->ec); free(x->eo);
  free(x->cs); free(x->ct); free(x->ctk); free(x->cv); free(x->cet);
  free(x->bs); free(x->bv); free(x->bser); free(x->bet); free(x->shand);
  free(x->rrf); free(x->rrem); free(x->rrcm); free(x->rrbm); free(x);
}
#endif
#ifdef SPINEL_EXT_HOST
void sp_exc_ctx_save(void *p);
#else
void sp_exc_ctx_save(void *p) {            /* current globals -> ctx */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  int n = sp_exc_top;
  if (n > x->ecap) { x->ecap = n;
    x->es = (jmp_buf *)realloc(x->es, sizeof(jmp_buf) * n);
    x->em = (const char **)realloc(x->em, sizeof(char *) * n);
    x->ec = (const char **)realloc(x->ec, sizeof(char *) * n);
    x->eo = (void **)realloc(x->eo, sizeof(void *) * n);
    x->rrem = (int *)realloc(x->rrem, sizeof(int) * n); }
  for (int i = 0; i < n; i++) { memcpy(x->es[i], sp_exc_stack[i], sizeof(jmp_buf));
    x->em[i] = sp_exc_msg[i]; x->ec[i] = sp_exc_cls[i]; x->eo[i] = sp_exc_obj[i];
    /* the arm's walk-path depth travels with the arm: two green threads whose
       handlers sit at the same index would otherwise share one slot, and a
       raise in one would restore the other's depth -- a HIGHER one resurrects
       frames that belong to the other context (see sp_poly_recur_mark) */
    x->rrem[i] = sp_poly_recur_mark[i]; }
  x->en = n;
  int m = sp_catch_top;
  if (m > x->ccap) { x->ccap = m;
    x->cs = (jmp_buf *)realloc(x->cs, sizeof(jmp_buf) * m);
    x->ct = (const char **)realloc(x->ct, sizeof(char *) * m);
    x->ctk = (unsigned char *)realloc(x->ctk, sizeof(unsigned char) * m);
    x->cv = (sp_RbVal *)realloc(x->cv, sizeof(sp_RbVal) * m);
    x->cet = (int *)realloc(x->cet, sizeof(int) * m);
    x->rrcm = (int *)realloc(x->rrcm, sizeof(int) * m); }
  for (int i = 0; i < m; i++) { memcpy(x->cs[i], sp_catch_stack[i], sizeof(jmp_buf));
    x->ct[i] = sp_catch_tag[i]; x->ctk[i] = sp_catch_tag_kind[i];
    x->cv[i] = sp_catch_val[i]; x->cet[i] = sp_catch_exc_top[i];
    x->rrcm[i] = sp_catch_recur_mark[i]; }
  x->cn = m;
  int bn = sp_brk_top;
  if (bn > x->bcap) { x->bcap = bn;
    x->bs = (jmp_buf *)realloc(x->bs, sizeof(jmp_buf) * bn);
    x->bv = (sp_RbVal *)realloc(x->bv, sizeof(sp_RbVal) * bn);
    x->bser = (sp_int *)realloc(x->bser, sizeof(sp_int) * bn);
    x->bet = (int *)realloc(x->bet, sizeof(int) * bn);
    x->rrbm = (int *)realloc(x->rrbm, sizeof(int) * bn);
    if (!x->bs || !x->bv || !x->bser || !x->bet || !x->rrbm) sp_oom_die(); }
  for (int i = 0; i < bn; i++) { memcpy(x->bs[i], sp_brk_stack[i], sizeof(jmp_buf));
    x->bv[i] = sp_brk_val[i]; x->bser[i] = sp_brk_serial[i]; x->bet[i] = sp_brk_exc_top[i];
    x->rrbm[i] = sp_brk_recur_mark[i]; }
  x->bn = bn;
  x->prhead = sp_proc_ret_head;
  x->uk = sp_unwind_kind; x->ut = sp_unwind_target; x->ue = sp_unwind_exc_top; x->uh = sp_unwind_home;
  int rn = sp_rescue_sp;
  if (rn > x->rcap) { x->rcap = rn;
    x->shand = (void **)realloc(x->shand, sizeof(void *) * rn);
    if (!x->shand) sp_oom_die(); }
  for (int i = 0; i < rn; i++) x->shand[i] = sp_exc_handling[i];
  x->rn = rn; x->pcause = sp_pending_cause;
  /* The container-walk path travels with the green thread, like the handler
     stack above it: a fiber suspended in the middle of an #inspect resumes
     still knowing what it was inside, and the fiber that runs meanwhile starts
     from an empty path rather than reading this one's frames. Almost always
     nothing to copy -- a yield lands between walks, not inside one. */
  int rrn = sp_poly_recur_top;
  if (rrn > x->rrcap) { x->rrcap = rrn;
    x->rrf = (sp_poly_recur_frame *)realloc(x->rrf, sizeof(sp_poly_recur_frame) * rrn);
    if (!x->rrf) sp_oom_die(); }
  for (int i = 0; i < rrn; i++) x->rrf[i] = sp_poly_recur_stack[i];
  x->rrn = rrn;
}
#endif
#ifdef SPINEL_EXT_HOST
void sp_exc_ctx_load(void *p);
#else
void sp_exc_ctx_load(void *p) {            /* ctx -> current globals */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  for (int i = 0; i < x->en; i++) { memcpy(sp_exc_stack[i], x->es[i], sizeof(jmp_buf));
    sp_exc_msg[i] = x->em[i]; sp_exc_cls[i] = x->ec[i]; sp_exc_obj[i] = x->eo[i];
    sp_poly_recur_mark[i] = x->rrem[i]; }
  sp_exc_top = x->en;
  /* the marker covers one slot past top (handler consumption window); zero
     it so a stale entry from another fiber's context is never chased */
  if (x->en < SP_EXC_STACK_MAX) { sp_exc_msg[x->en] = 0; sp_exc_obj[x->en] = 0; }
  for (int i = 0; i < x->cn; i++) { memcpy(sp_catch_stack[i], x->cs[i], sizeof(jmp_buf));
    sp_catch_tag[i] = x->ct[i]; sp_catch_tag_kind[i] = x->ctk[i];
    sp_catch_val[i] = x->cv[i]; sp_catch_exc_top[i] = x->cet[i];
    sp_catch_recur_mark[i] = x->rrcm[i]; }
  sp_catch_top = x->cn;
  for (int i = 0; i < x->bn; i++) { memcpy(sp_brk_stack[i], x->bs[i], sizeof(jmp_buf));
    sp_brk_val[i] = x->bv[i]; sp_brk_serial[i] = x->bser[i]; sp_brk_exc_top[i] = x->bet[i];
    sp_brk_recur_mark[i] = x->rrbm[i]; }
  sp_brk_top = x->bn;
  sp_proc_ret_head = x->prhead;
  sp_unwind_kind = x->uk; sp_unwind_target = x->ut; sp_unwind_exc_top = x->ue; sp_unwind_home = x->uh;
  for (int i = 0; i < x->rn; i++) sp_exc_handling[i] = x->shand[i];
  sp_rescue_sp = x->rn; sp_pending_cause = x->pcause;
  if (x->rrn > sp_poly_recur_cap) sp_poly_recur_grow(x->rrn);
  for (int i = 0; i < x->rrn; i++) sp_poly_recur_stack[i] = x->rrf[i];
  sp_poly_recur_top = x->rrn;
  sp_poly_recur_ixtop = 0;   /* the index described the other context's frames */
}
#endif
#ifdef SPINEL_EXT_HOST
void sp_exc_ctx_mark(void *p);
#else
void sp_exc_ctx_mark(void *p) {            /* GC: mark a suspended fiber's carried exc objects */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
  for (int i = 0; i < x->en; i++) if (x->eo[i]) sp_gc_mark(x->eo[i]);
  /* a suspended fiber's proc-return chain (nodes on its preserved C stack) may
     carry an in-flight return value; mark each so it survives a GC during yield. */
  for (sp_proc_home *h = x->prhead; h; h = h->prev) sp_mark_rbval(h->val);
  for (int i = 0; i < x->bn; i++) sp_mark_rbval(x->bv[i]);   /* carried break scopes */
  for (int i = 0; i < x->rn; i++) if (x->shand[i]) sp_gc_mark(x->shand[i]);  /* handled excs */
}
#endif
/* Trampoline base handler (#1474): the fiber trampoline arms a copy of its own
   setjmp buffer as the fiber's lowest handler, so an otherwise-unhandled raise
   in the fiber body unwinds back to the trampoline (on the fiber's own stack)
   instead of exiting or long-jumping across to the resumer. */
#ifdef SPINEL_EXT_HOST
void sp_exc_arm(jmp_buf b);
#else
void sp_exc_arm(jmp_buf b)     { sp_exc_check_depth(); memcpy(sp_exc_stack[sp_exc_top], b, sizeof(jmp_buf)); sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++; }
#endif
#ifdef SPINEL_EXT_HOST
void sp_exc_disarm(void);
#else
void sp_exc_disarm(void)       { if (sp_exc_top > 0) sp_exc_top--; }
#endif
#ifdef SPINEL_EXT_HOST
const char *sp_exc_cur_cls(void);
#else
const char *sp_exc_cur_cls(void) { return sp_exc_top > 0 ? sp_exc_cls[sp_exc_top-1] : sp_str_empty; }
#endif
#ifdef SPINEL_EXT_HOST
const char *sp_exc_cur_msg(void);
#else
const char *sp_exc_cur_msg(void) { return sp_exc_top > 0 ? sp_exc_msg[sp_exc_top-1] : sp_str_empty; }
#endif
#ifdef SPINEL_EXT_HOST
void *sp_exc_cur_obj(void);
#else
void *sp_exc_cur_obj(void)       { return sp_exc_top > 0 ? sp_exc_obj[sp_exc_top-1] : NULL; }
#endif
/* Re-raise a fiber's unhandled exception in the resumer's context (the fiber
   trampoline caught it on the fiber's stack, then returned cooperatively). */
#ifdef SPINEL_EXT_HOST
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);
#else
void sp_fiber_reraise(const char *cls, const char *msg, void *obj) {
  if (obj) sp_pending_exc_obj = obj;
  sp_raise_cls(cls, msg);
}
#endif


/* File metadata predicates (sp_file_directory/file/exist/delete) moved to
   lib/sp_io.c (libspinel_rt.a); prototypes come from sp_io.h. */

/* Text mode ("r") matches CRuby's File.read: on Windows, CRLF is
   normalized to LF on read, which cancels out fopen("w")'s
   LF→CRLF on write. Without this, content from File.read passed to
   puts goes through stdout's text-mode translation a second time
   and `\r\n` becomes `\r\r\n`. fread's actual byte count drives
   null-termination because text mode shrinks the byte count below
   ftell's raw-file size. */
/* sp_file_read: moved to lib/sp_cold.c */
const char *sp_file_read(const char *path);

/* sp_file_write: moved to lib/sp_cold.c */
sp_int sp_file_write(const char *path, const char *data);
/* sp_file_mtime: moved to lib/sp_cold.c */
sp_Time sp_file_mtime(const char *path);
sp_Time sp_file_atime(const char *path);
sp_Time sp_file_ctime(const char *path);
sp_Time sp_file_birthtime(const char *path);
sp_int sp_process_getpriority(sp_int which, sp_int who);
sp_IntArray *sp_process_groups(void);
/* ---- the File stat/predicate and IO-op surface (#2774-#2778, #2782) ---- */
const char *sp_file_ftype(const char *path);
sp_bool sp_file_readable(const char *path);
sp_bool sp_file_writable(const char *path);
sp_bool sp_file_executable(const char *path);
sp_bool sp_file_readable_real(const char *path);
sp_bool sp_file_writable_real(const char *path);
sp_bool sp_file_executable_real(const char *path);
const char *sp_file_realdirpath(const char *path);
sp_File *sp_io_for_fd(sp_int fd, const char *mode, sp_bool autoclose);
sp_Addrinfo *sp_addrinfo_new(const char *ip, sp_int port, sp_int stype, sp_int is_unix);
const char *sp_addrinfo_inspect(sp_Addrinfo *a);
sp_SockOpt *sp_sockopt_new(sp_int family, sp_int level, sp_int optname, sp_int value);
const char *sp_sockopt_inspect(sp_SockOpt *o);
sp_File *sp_io_wait_events(sp_File *f, double timeout, sp_int kind);
sp_RbVal sp_io_select(sp_PolyArray *rd, sp_PolyArray *wr, sp_PolyArray *er, double timeout);
sp_int sp_file_size_q(const char *path);
sp_bool sp_file_pipe(const char *path);
sp_bool sp_file_identical(const char *a, const char *b);
const char *sp_file_realpath(const char *path);
sp_bool sp_file_absolute_path_p(const char *path);
sp_int sp_file_chown(const char *path, sp_int uid, sp_int gid);
const char *sp_file_read_len(const char *path, sp_int n);
sp_int sp_file_chmod(sp_int mode, const char *path);
sp_int sp_file_truncate(const char *path, sp_int n);
sp_int sp_file_write_at(const char *path, const char *data, sp_int off);
sp_int sp_file_write_mode(const char *path, const char *data, const char *mode);
/* File.open with integer open(2) flags: open the fd, then wrap it in the
   stdio handle the sp_File surface expects (#2788). */
sp_File *sp_File_open_flags(const char *path, sp_int fl);
sp_File *sp_File_open_flags_perm(const char *path, sp_int fl, sp_int perm);
sp_File *sp_File_open_perm(const char *path, const char *mode, sp_int perm);
/* File.stat(path) / File#stat: a path-carrying handle whose metadata methods
   (size/mtime/atime/ctime/ftype/mode) stat the path -- the pragmatic subset of
   File::Stat this backend models (#2775, #2790). */
void sp_file_stat_scan(void *p);
sp_File *sp_file_stat_handle(const char *path);
sp_File *sp_io_stat_handle(sp_File *f);   /* IO#stat: by path, or fstat(2) for a descriptor handle */
sp_File *sp_file_lstat_handle(const char *path);
sp_bool sp_stat_nofollow(sp_File *f);
sp_int sp_stat_size(sp_File *f);
sp_int sp_stat_field(sp_File *f, sp_int which);   /* uid/gid/nlink/dev/ino/blksize/blocks/rdev */
sp_int sp_stat_pred(sp_File *f, sp_int kind);     /* pipe?/zero?/readable?/... /size? */
sp_int sp_stat_mode(sp_File *f);
const char *sp_stat_ftype(sp_File *f);
sp_int sp_file_stat_mode(const char *path);
/* IO#puts with an Array argument: one element per line, recursively (#2813) */
static void sp_File_puts_val(sp_File *f, sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    sp_int n = sp_poly_length(v);
    for (sp_int i = 0; i < n; i++) sp_File_puts_val(f, sp_poly_arr_get(v, i));
    return;
  }
  {
    const char *sv = (v.tag == SP_TAG_NIL) ? "" : sp_poly_to_s(v);
    sp_File_puts(f, sv ? sv : "");
  }
}
/* A boxed IO handle read back out of a poly slot (an IO.pipe element, a
   poly-widened local): unbox for the IO instance dispatch, NoMethodError on
   any other tag. */
static sp_File *sp_poly_as_io(sp_RbVal v, const char *m) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_IO && v.v.p) return (sp_File *)v.v.p;
  sp_raise_poly_nomethod(m, v);
  return NULL;
}
/* File.fnmatch: shell-glob match with CRuby's pathname/dot-file defaults */
sp_bool sp_file_fnmatch(const char *pat, const char *path);
const char *sp_file_dirname(const char *path);
const char *sp_file_basename(const char *path);
/* File.split(path) -> [dirname, basename] */
sp_StrArray *sp_file_split(const char *path);
/* File.size(path) -> byte size. Raises Errno::ENOENT on a missing path,
   matching MRI (and sp_file_read / sp_file_mtime, which stat/open the
   same way). */
/* sp_file_size: moved to lib/sp_cold.c */
sp_int sp_file_size(const char *path);
/* File.zero?/empty?: a zero-length non-directory (regular file, /dev/null,
   ...); false when the path is missing (#2783). */
sp_bool sp_file_zero(const char *path);
/* sp_backtick: moved to lib/sp_cold.c */
const char *sp_backtick(const char *cmd);
/* sp_file_basename: moved to lib/sp_cold.c */
const char *sp_file_basename(const char *path);
const char *sp_file_basename2(const char *path, const char *suffix);
/* File.join with runtime-typed components: strings pass through, arrays
   flatten recursively (#2786), nil is CRuby's TypeError. */
/* The flattened components are held in a GC-scanned array, not a raw
   pointer array: a #to_path or #to_str may build its answer, and the next
   component's conversion (or the join's own allocation) can collect a
   String nothing else holds. */
static void sp_file_join_flat(sp_RbVal v, sp_StrArray *parts, int depth) {
  if (v.tag == SP_TAG_STR) { sp_StrArray_push(parts, v.v.s ? v.v.s : ""); return; }
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    /* an Array nested past any sane depth is one that contains itself */
    if (depth >= 64) sp_raise_cls("ArgumentError", "recursive array");
    sp_int n = sp_poly_length(v);
    for (sp_int i = 0; i < n; i++) sp_file_join_flat(sp_poly_arr_get(v, i), parts, depth + 1);
    return;
  }
  /* everything else takes the path slot's protocol: nil / bool / a
     wrongly-classed scalar is CRuby's TypeError, a user object converts
     through #to_path or #to_str, or raises */
  sp_StrArray_push(parts, sp_poly_arg_path(v));
}
static const char *sp_file_join_vals(sp_RbVal *vals, int n) {
  sp_StrArray *parts = sp_StrArray_new();
  SP_GC_ROOT(parts);
  for (int i = 0; i < n; i++) sp_file_join_flat(vals[i], parts, 0);
  return sp_file_join(parts->data, (int)parts->len);
}
/* Issue #892: File.dirname / File.extname / Dir.pwd. */
const char *sp_file_dirname(const char *path);
/* sp_file_extname: moved to lib/sp_cold.c */
const char *sp_file_extname(const char *path);
const char *sp_dir_pwd(void);
/* Dir singleton methods. mkdir/rmdir/chdir use the platform call (the
   Windows _-prefixed variants take a single path argument); each returns
   0 on success, matching CRuby's `Dir.mkdir` etc. */
sp_int sp_dir_mkdir(const char *path);
sp_int sp_dir_rmdir(const char *path);
sp_int sp_dir_chdir(const char *path);
sp_int sp_dir_chdir0(const char *path);  /* the block form's label */
const char *sp_dir_home(void);
/* Wildcard match for a single path component: `*` (any run, no `/`),
   `?` (one char). Recursive over `*`; adequate for the common
   single-directory glob patterns. */
/* sp_fnmatch1: moved to lib/sp_cold.c */
int sp_fnmatch1(const char *pat, const char *str);
/* Recursive helper for a double-star glob: visit `fsdir` and every descendant,
   and in each match `tail` (a single-component pattern) against the directory's
   entries. `outprefix` is prepended to a match to reproduce the path shape Ruby
   returns (the text before the double-star is preserved verbatim; a cwd-anchored
   pattern yields bare names). Symlinked directories are not traversed, matching
   CRuby's default and avoiding cycles. Hidden entries are skipped unless `tail`
   starts with a dot; hidden directories are never descended. */
/* sp_dir_glob_rec: moved to lib/sp_cold.c */
void sp_dir_glob_rec(const char *fsdir, const char *outprefix,
                            const char *tail, sp_StrArray *a);
/* Dir.glob(pattern): list directory entries matching the last component
   of `pattern` (an optional leading `dir/` selects the directory). A recursive
   double-star component walks that subtree (the tail after it matches per
   directory). Hidden entries match only when the pattern itself begins with
   `.`. Results are sorted, matching Ruby 3.0+ default glob ordering. */
/* sp_dir_glob: moved to lib/sp_cold.c */
sp_StrArray *sp_dir_glob(const char *pattern);
sp_StrArray *sp_dir_glob_dot(const char *pattern);
/* Dir.entries / Dir.children: every entry of one directory, dotfiles
   included; children drops "." / "..". Sorted for determinism (CRuby
   leaves readdir order unspecified). A missing directory raises like
   CRuby, not the glob-style empty result. */
/* sp_dir_entries_impl: moved to lib/sp_cold.c */
sp_StrArray *sp_dir_entries_impl(const char *path, int children);
/* ---- Dir handles (#2821): an open directory stream with its path ---- */
/* sp_Dir moved to sp_io.h (used by the Dir ops in lib/sp_cold.c). */
void sp_Dir_fin(void *p);
void sp_Dir_scan(void *p);
sp_Dir *sp_Dir_new(const char *path);
sp_Dir *sp_Dir_for_fd(sp_int fd);
sp_StrArray *sp_Dir_entries_h(sp_Dir *d, sp_int children);
sp_int sp_Dir_fchdir(sp_int fd);
const char *sp_Dir_read(sp_Dir *d);
const char *sp_Dir_path(sp_Dir *d);
sp_RbVal sp_Dir_close(sp_Dir *d);
sp_Dir *sp_Dir_rewind(sp_Dir *d);
sp_int sp_Dir_tell(sp_Dir *d);
sp_Dir *sp_Dir_seek(sp_Dir *d, sp_int pos);
sp_int sp_Dir_fileno(sp_Dir *d);
sp_StrArray *sp_dir_entries(const char *path);
/* Dir.empty?(path): a directory with no non-dot entries; a non-directory is
   false, a missing path CRuby's Errno::ENOENT (#2823). */
sp_bool sp_dir_empty(const char *path);
sp_bool sp_dir_empty(const char *path);
/* Dir.home(user): the named user's home from the passwd db (#2830). */
const char *sp_dir_home_user(const char *user);
const char *sp_dir_home_user(const char *user);
/* Dir.glob([pat, ...]): each pattern globbed in order, results concatenated
   (#2828). */
static sp_PolyArray *sp_enum_items_from(sp_RbVal v);   /* defined below */
static sp_StrArray *sp_dir_glob_multi(sp_RbVal pats) __attribute__((unused));
static sp_StrArray *sp_dir_glob_multi(sp_RbVal pats) {
  sp_StrArray *out = sp_StrArray_new();
  SP_GC_ROOT(out);
  sp_PolyArray *ps = sp_enum_items_from(pats);
  SP_GC_ROOT(ps);
  for (sp_int i = 0; ps && i < ps->len; i++) {
    sp_RbVal pv = ps->data[i];
    if (pv.tag != SP_TAG_STR || !pv.v.s) continue;
    sp_StrArray *one = sp_dir_glob(pv.v.s);
    SP_GC_ROOT(one);
    for (sp_int j = 0; one && j < one->len; j++)
      sp_StrArray_push(out, sp_StrArray_get(one, j));
  }
  return out;
}
sp_StrArray *sp_dir_children(const char *path);

/* File.expand_path(path[, base]) -- CRuby-compatible pure-string
   expansion (does NOT require the path to exist). A leading `~` / `~/`
   becomes $HOME; a relative path is resolved against `base` (itself
   expanded; NULL means the current working directory); and `.` / `..` /
   duplicate-slash segments are collapsed. `~user` is unsupported and is
   left as-is. */
/* File.expand_path (definition in lib/sp_cold.c). */
const char *sp_file_expand_path(const char *path, const char *base);

/* Read a file's bytes into a fresh IntArray. Distinct from
   `sp_str_bytes(sp_file_read(path))` because plain sp_str_bytes uses
   null-termination and stops at the first 0x00 byte — wrong for
   binary data (e.g. .nes ROM files). */
/* sp_file_binread_bytes: moved to lib/sp_cold.c */
sp_IntArray *sp_file_binread_bytes(const char *path);

/* `arr.slice!(from, n)` — returns a fresh array of `n` elements
   starting at `from` and removes them from `a`. IntArray uses its
   `start` field for an O(1) head peel (from == 0); the others
   shift the tail down to fill the hole. */
/* at_exit hooks: a static LIFO of registered procs. Initialized
   zero-len in BSS; sp_at_exit_run (below, with sp_proc_call)
   drains it in reverse-registration order on every path that ends
   the program the way CRuby runs the hooks on.
   The table is a GC root: the registering expression stores the Proc
   here and drops it, so between `at_exit { }` and the hook actually
   running this array is the only reference to the Proc and to the
   environment it captured. */
#define SP_AT_EXIT_MAX 256
struct sp_Proc;
static struct sp_Proc *sp_at_exit_hooks[SP_AT_EXIT_MAX];
static sp_int sp_at_exit_count = 0;
/* Threaded build: the table is shared by every worker, and on the exit, abort
   and uncaught-raise paths the drain runs on main while the helper workers are
   still executing green threads (CRuby keeps them alive across the hooks too),
   so a thread registering a hook at that moment pushes while main pops. One
   mutex around the push and the pop; the hook itself is called outside it,
   since it may register another. No-op in the single-threaded build, the way
   SP_HEAP_LOCK is, so that build compiles the code it did. */
#ifdef SP_THREADS
static pthread_mutex_t sp_at_exit_lock = PTHREAD_MUTEX_INITIALIZER;
#define SP_AT_EXIT_LOCK()   pthread_mutex_lock(&sp_at_exit_lock)
#define SP_AT_EXIT_UNLOCK() pthread_mutex_unlock(&sp_at_exit_lock)
#else
#define SP_AT_EXIT_LOCK()   ((void)0)
#define SP_AT_EXIT_UNLOCK() ((void)0)
#endif
/* `at_exit { }`: the registering expression evaluates to the proc. */
static inline struct sp_Proc *sp_at_exit_push(struct sp_Proc *p) {
  SP_AT_EXIT_LOCK();
  sp_at_exit_hooks[sp_at_exit_count++] = p;
  SP_AT_EXIT_UNLOCK();
  return p;
}
static void sp_mark_at_exit_hooks(void) {
  for (sp_int i = 0; i < sp_at_exit_count; i++)
    if (sp_at_exit_hooks[i]) sp_gc_mark(sp_at_exit_hooks[i]);
}



/* External Enumerator: a cursor over a snapshot of a collection's elements
   (boxed into a PolyArray at creation). #next / #peek walk the cursor and raise
   StopIteration past the end; #rewind resets it. Block-form chains (each.map,
   each.with_index, ...) are handled by codegen and never build this object. */
/* Two flavors: a materialized snapshot (items + cursor, from a collection's
   blockless #each) or a fiber-backed generator (Enumerator.new { |y| ... },
   where `y << v` is a Fiber.yield). The fiber is created lazily on first #next
   and re-created on #rewind. */
#include "sp_enum.h"   /* sp_Enumerator; cursor/generator ops in lib/sp_cold.c */
/* Enumerator#dup / #clone: a shallow struct copy is a distinct object (its
   cursor rewinds independently; == compares by pointer, so dup != original). */
void sp_Enumerator_scan(void *p);
sp_Enumerator *sp_Enumerator_dup(sp_Enumerator *e);
static sp_PolyArray *sp_enum_items_from(sp_RbVal v) {
  SP_GC_ROOT_RBVAL(v);   /* the hash arm allocates before reading v again */
  if (v.tag == SP_TAG_OBJ) {
    void *p = v.v.p;
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY:  return sp_IntArray_to_poly((sp_IntArray *)p);
      case SP_BUILTIN_STR_ARRAY:  return sp_StrArray_to_poly_fmt((sp_StrArray *)p);
      case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *a = (sp_PolyArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, a->data[i]); return r; }
      case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_float(a->data[i])); return r; }
      case SP_BUILTIN_SYM_ARRAY:  { sp_IntArray *a = (sp_IntArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_sym((sp_sym)a->data[a->start + i])); return r; }
      /* an int range iterates its members; keeps the range itself printable
         as the enumerator's #inspect source */
      case SP_BUILTIN_RANGE: { sp_Range *rg = (sp_Range *)p; sp_IntArray *ia = sp_range_to_ia(*rg); SP_GC_ROOT(ia); return sp_IntArray_to_poly(ia); }
      /* a string range iterates its members too (#3619) */
      case SP_BUILTIN_STR_RANGE: { sp_StrRange *sr = (sp_StrRange *)p; sp_StrArray *sa = sp_srange_to_a(*sr); SP_GC_ROOT(sa); return sp_StrArray_to_poly_fmt(sa); }
      /* A hash iterates as its [key, value] pairs, in insertion order --
         sp_poly_each_elem builds the i-th pair for any of the variants. Each
         freshly built pair is rooted across the push, whose array-grow may
         trigger a collection. */
      case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
      case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
      case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
      case SP_BUILTIN_INT_INT_HASH: {
        sp_int n = sp_poly_length(v);
        sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
        for (sp_int i = 0; i < n; i++) {
          sp_RbVal pair = sp_poly_each_elem(v, i); SP_GC_ROOT_RBVAL(pair);
          sp_PolyArray_push(r, pair);
        }
        return r;
      }
    }
    /* A user object materializes through its own #to_a (or the __enum_to_a
       synthesized for an Enumerable that defines #each), the way sp_poly_arr_recv
       already does. Without the arm every such object fell to the empty array
       below, so an external enumerator over one yielded nothing (#4022). */
    if (v.cls_id >= 0 && sp_obj_to_a_fn) {
      sp_RbVal a = sp_obj_to_a_fn(v);
      if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id)) return sp_poly_to_poly_array(a);
    }
  }
  return sp_PolyArray_new();
}
/* Poly-receiver #to_a: nil is the empty array, arrays and hashes materialize
   through sp_enum_items_from (a hash yields its [key, value] pairs), and any
   other value raises CRuby's NoMethodError. */
static sp_PolyArray *sp_poly_to_a_arr(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_PolyArray_new();
  /* Array#to_a returns self (identity), so a poly array is returned as-is
     rather than copied; typed arrays and hashes must materialize a new
     PolyArray to reach the poly representation. */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY)
    return (sp_PolyArray *)v.v.p;
  if (v.tag == SP_TAG_OBJ &&
      (sp_poly_is_array_kind(v.cls_id) || sp_poly_is_hash_kind(v.cls_id) ||
       v.cls_id == SP_BUILTIN_RANGE || v.cls_id == SP_BUILTIN_STR_RANGE))
    return sp_enum_items_from(v);   /* Range#to_a -> its element array (#3162) */
  /* a Struct/Data read out of a container: Struct#to_a is its member values in
     order, which the symbol-keyed to_h (via the generated hook) preserves. */
  if (v.tag == SP_TAG_OBJ && sp_obj_to_h_fn) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ && h.cls_id == SP_BUILTIN_SYM_POLY_HASH)
      return sp_SymPolyHash_values((sp_SymPolyHash *)h.v.p);
  }
  /* an Enumerator read out of a container drains like the typed receiver does */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_ENUMERATOR && v.v.p)
    return sp_enum_to_a_boxed(v);
  /* a user class that includes Enumerable: its elements, through the
     materializer the generated to_a dispatch calls (#3761) */
  { sp_PolyArray *ue = sp_poly_user_elems(v);
    if (ue) return ue; }
  sp_raise_nomethod(sp_nomethod_msg("to_a", v));
  return NULL;
}

/* `to_h` on a boxed receiver. A Hash answers ITSELF -- CRuby returns self, so
   there is nothing to build and no copy to make. Every other kind goes through
   its pair list: an Array of two-element Arrays is the pair form CRuby accepts,
   and a Struct/Data answers its member table. Anything else raises the
   NoMethodError the dispatch would have raised (#4170). */
static sp_RbVal sp_poly_to_h_val(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) return v;
  if (v.tag == SP_TAG_OBJ && sp_obj_to_h_fn && v.cls_id >= 0) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(h.cls_id)) return h;
  }
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    sp_PolyArray *a = sp_poly_to_a_arr(v); SP_GC_ROOT(a);
    sp_PolyPolyHash *out = sp_PolyPolyHash_new(); SP_GC_ROOT(out);
    for (sp_int i = 0; a && i < a->len; i++) {
      sp_RbVal e = a->data[i];
      if (e.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(e.cls_id) ||
          sp_poly_length(e) != 2) {
        sp_raise_cls("TypeError", "wrong element type (expected array of size 2)");
        return sp_box_nil();
      }
      sp_PolyPolyHash_set(out, sp_poly_arr_get(e, 0), sp_poly_arr_get(e, 1));
    }
    return sp_box_obj(out, SP_BUILTIN_POLY_POLY_HASH);
  }
  sp_raise_nomethod(sp_nomethod_msg("to_h", v));
  return sp_box_nil();
}
/* Hash#merge on a poly hash (a hash read out of a container, any variant):
   fold both operands' pairs into a general PolyPoly hash, the receiver first
   then the argument overriding (#3162). */
typedef struct sp_poly_hash_dproc_ctx { sp_RbVal source; } sp_poly_hash_dproc_ctx;
static sp_RbVal sp_penum_call2(sp_Proc *blk, sp_RbVal v, sp_RbVal w);
static void sp_poly_hash_dproc_ctx_scan(void *p) {
  sp_poly_hash_dproc_ctx *ctx = (sp_poly_hash_dproc_ctx *)p;
  sp_mark_rbval(ctx->source);
}
static sp_RbVal sp_poly_hash_dproc_bridge(sp_PolyPolyHash *h, sp_RbVal key, void *self) {
  sp_poly_hash_dproc_ctx *ctx = (sp_poly_hash_dproc_ctx *)self;
  sp_RbVal source = ctx->source;
  (void)h;
  if (source.tag != SP_TAG_OBJ || !source.v.p) return sp_box_nil();
  if (source.cls_id == SP_BUILTIN_STR_POLY_HASH && key.tag == SP_TAG_STR) {
    sp_StrPolyHash *sh = (sp_StrPolyHash *)source.v.p;
    /* default_proc= stores the Proc in dproc_self; pass the merged result to
       it, rather than the typed source retained by this bridge. */
    if (sh->dproc_self)
      return sp_penum_call2((sp_Proc *)sh->dproc_self,
                            sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH), key);
    return sh->dproc(sh, key.v.s, sh->dproc_self);
  }
  if (source.cls_id == SP_BUILTIN_SYM_POLY_HASH && key.tag == SP_TAG_SYM) {
    sp_SymPolyHash *sh = (sp_SymPolyHash *)source.v.p;
    if (sh->dproc_self)
      return sp_penum_call2((sp_Proc *)sh->dproc_self,
                            sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH), key);
    return sh->dproc(sh, (sp_sym)key.v.i, sh->dproc_self);
  }
  if (source.cls_id == SP_BUILTIN_POLY_POLY_HASH) {
    sp_PolyPolyHash *ph = (sp_PolyPolyHash *)source.v.p;
    return ph->dproc(ph, key, ph->dproc_self);
  }
  return sp_box_nil();
}
static sp_PolyPolyHash *sp_poly_hash_merge(sp_RbVal a, sp_RbVal b) {
  /* Before the allocation, not after: the operands are read for the whole
     loop below and the new hash is the first thing that can collect them.
     Copying them into hs[] does not root them -- the collector walks the
     registered root slots, not the stack. */
  SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
  sp_PolyPolyHash *r = sp_PolyPolyHash_new();
  SP_GC_ROOT(r);
  /* merge inherits the receiver's default; cross-layout receivers arrive boxed */
  if (a.tag == SP_TAG_OBJ && a.v.p) {
    switch (a.cls_id) {
      case SP_BUILTIN_STR_INT_HASH: r->default_v = sp_box_int_or_nil(((sp_StrIntHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_STR_STR_HASH: r->default_v = sp_box_nullable_str(((sp_StrStrHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_INT_STR_HASH: r->default_v = sp_box_nullable_str(((sp_IntStrHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_INT_INT_HASH: r->default_v = sp_box_int_or_nil(((sp_IntIntHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_STR_POLY_HASH: r->default_v = ((sp_StrPolyHash *)a.v.p)->default_v; break;
      case SP_BUILTIN_SYM_POLY_HASH: r->default_v = ((sp_SymPolyHash *)a.v.p)->default_v; break;
      case SP_BUILTIN_POLY_POLY_HASH: r->default_v = ((sp_PolyPolyHash *)a.v.p)->default_v; break;
      default: break;
    }
    int has_dproc = 0;
    switch (a.cls_id) {
      case SP_BUILTIN_STR_POLY_HASH: has_dproc = ((sp_StrPolyHash *)a.v.p)->dproc != NULL; break;
      case SP_BUILTIN_SYM_POLY_HASH: has_dproc = ((sp_SymPolyHash *)a.v.p)->dproc != NULL; break;
      case SP_BUILTIN_POLY_POLY_HASH: has_dproc = ((sp_PolyPolyHash *)a.v.p)->dproc != NULL; break;
      default: break;
    }
    if (has_dproc) {
      sp_poly_hash_dproc_ctx *ctx = (sp_poly_hash_dproc_ctx *)sp_gc_alloc(
          sizeof(*ctx), NULL, sp_poly_hash_dproc_ctx_scan);
      ctx->source = a;
      r->dproc = sp_poly_hash_dproc_bridge;
      r->dproc_self = ctx;
    }
  }
  sp_RbVal hs[2]; hs[0] = a; hs[1] = b;
  for (int h = 0; h < 2; h++) {
    if (hs[h].tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(hs[h].cls_id)) continue;
    sp_PolyArray *pairs = sp_poly_to_a_arr(hs[h]);
    SP_GC_ROOT(pairs);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal pair = pairs->data[i];
      sp_PolyPolyHash_set(r, sp_poly_arr_get(pair, 0), sp_poly_arr_get(pair, 1));
    }
  }
  return r;
}
/* A boxed hash as the concrete symbol-keyed variant: itself when it already is
   one, rebuilt when every key is a Symbol (a hash folded through the general
   merge path is a PolyPolyHash regardless of its keys), and a TypeError only
   when a key really is not a Symbol (#3452). */
/* The same conversion at a parameter boundary an RBS seed pinned to a
   Symbol-keyed hash: the caller's value may be the boxed-key hash, which is a
   different C struct. A key the declared type cannot hold is the seed being
   wrong about the program, so say that rather than naming to_h (#3994). */
static sp_SymPolyHash *sp_poly_as_sym_hash(sp_RbVal v);
static sp_SymPolyHash *sp_seed_sym_hash_arg(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) {
    sp_PolyArray *pairs = sp_enum_items_from(v);
    SP_GC_ROOT(pairs);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal k = sp_poly_arr_get(pairs->data[i], 0);
      if (k.tag != SP_TAG_SYM)
        sp_raise_cls("TypeError",
                     "a Hash[Symbol, ...] parameter was passed a hash with a non-Symbol key");
    }
  }
  return sp_poly_as_sym_hash(v);
}
static sp_SymPolyHash *sp_poly_as_sym_hash(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_SYM_POLY_HASH)
    return (sp_SymPolyHash *)v.v.p;
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) {
    sp_PolyArray *pairs = sp_enum_items_from(v);
    SP_GC_ROOT(pairs);
    sp_SymPolyHash *h = sp_SymPolyHash_new();
    SP_GC_ROOT(h);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal pair = pairs->data[i], k = sp_poly_arr_get(pair, 0);
      if (k.tag != SP_TAG_SYM) {
        sp_raise_cls("TypeError", "to_h on a non-symbol-keyed boxed hash");
        return NULL;
      }
      sp_SymPolyHash_set(h, (sp_sym)k.v.i, sp_poly_arr_get(pair, 1));
    }
    return h;
  }
  sp_raise_cls("TypeError", "to_h on a non-symbol-keyed boxed hash");
  return NULL;
}
/* Hash#slice(*keys) on a boxed receiver: the sub-hash of the keys that are
   present, in the order given (#3449). */
static sp_RbVal sp_poly_hash_slice(sp_RbVal v, int n, sp_RbVal *keys) {
  sp_PolyPolyHash *h = sp_PolyPolyHash_new();
  SP_GC_ROOT(h);
  for (int i = 0; i < n; i++)
    if (sp_poly_has_key(v, keys[i]))
      sp_PolyPolyHash_set(h, keys[i], sp_poly_index_poly(v, keys[i]));
  return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
}
/* The result of filtering a boxed receiver: a Hash receiver answers a Hash
   rebuilt from the surviving [key, value] pairs, anything else the poly array
   the loop collected. Only the runtime value can decide, so select/reject/
   filter on a boxed receiver route their result through here (#3449). */
static sp_RbVal sp_poly_kept_result(sp_RbVal orig, sp_PolyArray *kept) {
  if (!(orig.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(orig.cls_id)))
    return sp_box_poly_array(kept);
  sp_PolyPolyHash *h = sp_PolyPolyHash_new();
  SP_GC_ROOT(h);
  for (sp_int i = 0; kept && i < kept->len; i++) {
    sp_RbVal pair = kept->data[i];
    sp_PolyPolyHash_set(h, sp_poly_arr_get(pair, 0), sp_poly_arr_get(pair, 1));
  }
  return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
}
/* The general boxed-key/value form of any boxed hash: the shape the read-only
   Hash/Enumerable face is compiled against when the receiver's variant is not
   known at the call site (#3449). A hash that is already general is handed back
   as-is; every other variant materializes, so this must stay read-only. A
   receiver that is not a hash at all raises the NoMethodError the call site
   would otherwise have raised. */
static sp_PolyPolyHash *sp_poly_as_pp_hash(sp_RbVal v, const char *nm) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_POLY_HASH)
    return (sp_PolyPolyHash *)v.v.p;
  if (v.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(v.cls_id)) {
    sp_raise_nomethod(sp_nomethod_msg(nm, v));
    return NULL;
  }
  return sp_poly_hash_merge(v, sp_box_nil());
}
/* A boxed HANDLE back as its own pointer, for the exclusive-name face: the
   call site compiles the handle's own body against the result, so the runtime
   kind has to be checked first. A value of any other kind raises the
   NoMethodError the call site would otherwise have raised (#4158). */
static void *sp_poly_as_handle(sp_RbVal v, int cls_id, const char *nm) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == cls_id && v.v.p) return v.v.p;
  sp_raise_nomethod(sp_nomethod_msg(nm, v));
  return NULL;
}
/* The receiver of a Hash method reached through a boxed value, as the
   general hash (sp_poly_as_pp_hash); a mutator (`mut`) refuses a frozen
   original here, before the typed emitter works on the copy. */
static sp_PolyPolyHash *sp_poly_hash_recv(sp_RbVal v, const char *m, int mut) {
  if (mut && v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id) && sp_gc_is_frozen(v.v.p))
    sp_raise_frozen_hash_at(v.v.p, v.cls_id);
  return sp_poly_as_pp_hash(v, m);
}
/* A key or value the typed original has no representation for: the
   sibling of sp_raise_writeback_kind for a hash. */
SP_NORETURN SP_COLD static void sp_raise_hash_writeback_kind(sp_RbVal v, const char *what, const char *into) {
  sp_raise_cls("TypeError", sp_sprintf("can't store %s as a %s in a Hash of %s through a boxed receiver",
                                       v.tag == SP_TAG_BIGINT ? "a bignum" : sp_poly_class_name(v), what, into));
}
/* A String- or Integer-valued variant holds nil natively (a NULL, the int
   nil sentinel), so a nil the original already held is its own to keep. A
   shared-mutable handle is a String too -- the value a local stored here and
   appended to afterwards -- and goes in by its contents, as it does through
   sp_poly_arr_writeback. */
static void sp_hash_wb_want(sp_RbVal v, int tag, int nil_ok, const char *what, const char *into) {
  if (v.tag == tag || (nil_ok && v.tag == SP_TAG_NIL)) return;
  if (tag == SP_TAG_STR && sp_poly_is_strbuf(v)) return;
  sp_raise_hash_writeback_kind(v, what, into);
}
/* A mutator's write-back through a boxed hash: sp_poly_as_pp_hash gives a
   typed variant as a general COPY, so a merge! or compact! that ran on the
   copy never reached the original. Replace the original's entries with the
   copy's, in its own representation; a general hash shares storage and needs
   nothing. An entry the variant cannot hold -- a String key merged into a
   Symbol-keyed hash, a String value into an Integer-valued one -- is refused
   rather than coerced into a key or value it was not. Frozenness was refused
   at the coercion, before the mutator ran, and is refused again here for an
   original frozen since -- by its own argument, say. */
static void sp_poly_hash_writeback(sp_RbVal orig, sp_PolyPolyHash *work) {
  if (orig.tag != SP_TAG_OBJ || !work || !orig.v.p) return;
  if (orig.cls_id == SP_BUILTIN_POLY_POLY_HASH) return;
  if (!sp_poly_is_hash_kind(orig.cls_id)) return;
  if (sp_gc_is_frozen(orig.v.p)) sp_raise_frozen_hash_at(orig.v.p, orig.cls_id);
  SP_GC_ROOT_RBVAL(orig); SP_GC_ROOT(work);
  for (sp_int i = 0; i < work->len; i++) {
    sp_int j = work->order[i];
    sp_RbVal k = work->keys[j], v = work->vals[j];
    switch (orig.cls_id) {
      case SP_BUILTIN_STR_INT_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); sp_hash_wb_want(v, SP_TAG_INT, 1, "value", "Integer values"); break;
      case SP_BUILTIN_STR_STR_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); sp_hash_wb_want(v, SP_TAG_STR, 1, "value", "String values"); break;
      case SP_BUILTIN_INT_STR_HASH: sp_hash_wb_want(k, SP_TAG_INT, 0, "key", "Integer keys"); sp_hash_wb_want(v, SP_TAG_STR, 1, "value", "String values"); break;
      case SP_BUILTIN_INT_INT_HASH: sp_hash_wb_want(k, SP_TAG_INT, 0, "key", "Integer keys"); sp_hash_wb_want(v, SP_TAG_INT, 1, "value", "Integer values"); break;
      case SP_BUILTIN_STR_POLY_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); break;
      case SP_BUILTIN_SYM_POLY_HASH: sp_hash_wb_want(k, SP_TAG_SYM, 0, "key", "Symbol keys"); break;
      default: return;
    }
  }
  switch (orig.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: {
      sp_StrIntHash *h = (sp_StrIntHash *)orig.v.p;
      sp_StrIntHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrIntHash_set(h, sp_poly_to_s(work->keys[j]), sp_poly_to_i_or_nil(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_STR_STR_HASH: {
      sp_StrStrHash *h = (sp_StrStrHash *)orig.v.p;
      sp_StrStrHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrStrHash_set(h, sp_poly_to_s(work->keys[j]), sp_poly_to_s_or_nil(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_INT_STR_HASH: {
      sp_IntStrHash *h = (sp_IntStrHash *)orig.v.p;
      sp_IntStrHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_IntStrHash_set(h, sp_poly_to_i(work->keys[j]), sp_poly_to_s_or_nil(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_INT_INT_HASH: {
      sp_IntIntHash *h = (sp_IntIntHash *)orig.v.p;
      sp_IntIntHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_IntIntHash_set(h, sp_poly_to_i(work->keys[j]), sp_poly_to_i_or_nil(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_STR_POLY_HASH: {
      sp_StrPolyHash *h = (sp_StrPolyHash *)orig.v.p;
      sp_StrPolyHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrPolyHash_set(h, sp_poly_to_s(work->keys[j]), work->vals[j]);
      }
      return;
    }
    case SP_BUILTIN_SYM_POLY_HASH: {
      sp_SymPolyHash *h = (sp_SymPolyHash *)orig.v.p;
      sp_SymPolyHash_clear(h);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_SymPolyHash_set(h, (sp_sym)work->keys[j].v.i, work->vals[j]);
      }
      return;
    }
    default: return;
  }
}
/* OpenStruct.new(hash) where hash is a runtime value (not a literal): seed the
   member table from the hash's entries, keys coerced to symbols. Copies, so
   mutating the OpenStruct does not alter the source hash (#3194). */
static sp_OpenStruct *sp_openstruct_from_poly(sp_RbVal h) {
  sp_SymPolyHash *tbl = sp_SymPolyHash_new();
  SP_GC_ROOT(tbl);
  if (h.tag == SP_TAG_OBJ && h.cls_id == SP_BUILTIN_SYM_POLY_HASH) {
    sp_SymPolyHash *s = (sp_SymPolyHash *)h.v.p;
    for (sp_int i = 0; i < s->len; i++)
      sp_SymPolyHash_set(tbl, s->order[i], sp_SymPolyHash_get(s, s->order[i]));
  }
  else if (h.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(h.cls_id)) {
    sp_PolyArray *pairs = sp_poly_to_a_arr(h);
    SP_GC_ROOT(pairs);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal pair = pairs->data[i], k = sp_poly_arr_get(pair, 0);
      sp_sym sym = (k.tag == SP_TAG_SYM) ? (sp_sym)k.v.i : sp_sym_intern(sp_poly_to_s(k));
      sp_SymPolyHash_set(tbl, sym, sp_poly_arr_get(pair, 1));
    }
  }
  return sp_OpenStruct_new_from(tbl);
}
/* Struct#members on a boxed value: the field-name symbols in order (the keys of
   the symbol-keyed to_h). */
static sp_PolyArray *sp_poly_struct_members(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_obj_to_h_fn) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ && h.cls_id == SP_BUILTIN_SYM_POLY_HASH) {
      sp_IntArray *k = sp_SymPolyHash_keys((sp_SymPolyHash *)h.v.p);
      SP_GC_ROOT(k);
      sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
      for (sp_int i = 0; i < k->len; i++) sp_PolyArray_push(a, sp_box_sym((sp_sym)k->data[k->start + i]));
      return a;
    }
  }
  sp_raise_nomethod(sp_nomethod_msg("members", v));
  return NULL;
}
/* Enumerable#sort on a boxed value (an array or hash read out of a poly
   container): a Hash sorts its [k, v] pairs like the typed Hash#sort path, an
   array sorts its elements; any other tag is CRuby's NoMethodError. */
static sp_PolyArray *sp_poly_sort(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id))
    return sp_PolyArray_sort_pairs(sp_poly_to_a_arr(v));
  return sp_PolyArray_sort(sp_poly_arr_recv(v, "sort"));
}
/* Enumerable#uniq on a boxed value (an array read out of a poly container or
   an ivar that widened): the distinct elements, in first-seen order. Any
   non-array tag is CRuby's NoMethodError, through sp_poly_arr_recv. */
/* compact / flatten on a value only known to be an Array at run time -- one
   read out of a Hash value or an Array element. The typed-receiver forms are
   keyed on the storage kind, so these never reached them and the call fell
   through to the NoMethodError default, naming Array (#3423). Same shape as
   sp_poly_uniq below. */
static sp_PolyArray *sp_poly_compact(sp_RbVal v) {
  sp_PolyArray *src = sp_poly_arr_recv(v, "compact");
  SP_GC_ROOT(src);
  return sp_PolyArray_compact(src);
}
/* #compact keeps the receiver's kind: a Hash drops the entries whose VALUE is
   nil and stays a Hash, an Array drops its nil elements (#3449). */
static sp_RbVal sp_poly_compact_val(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) {
    sp_PolyArray *pairs = sp_enum_items_from(v);
    SP_GC_ROOT(pairs);
    sp_PolyPolyHash *h = sp_PolyPolyHash_new();
    SP_GC_ROOT(h);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal pair = pairs->data[i], val = sp_poly_arr_get(pair, 1);
      if (val.tag != SP_TAG_NIL) sp_PolyPolyHash_set(h, sp_poly_arr_get(pair, 0), val);
    }
    return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
  }
  return sp_box_poly_array(sp_poly_compact(v));
}
static sp_PolyArray *sp_poly_flatten(sp_RbVal v) {
  sp_PolyArray *src = sp_poly_arr_recv(v, "flatten");
  SP_GC_ROOT(src);
  return sp_PolyArray_flatten(src);
}
static sp_PolyArray *sp_poly_uniq(sp_RbVal v) {
  sp_PolyArray *src = sp_poly_arr_recv(v, "uniq");
  SP_GC_ROOT(src);
  sp_PolyArray *out = sp_PolyArray_dup(src);
  SP_GC_ROOT(out);
  sp_PolyArray_uniq_bang(out);
  return out;
}
void sp_Enumerator_scan(void *p);
/* Blockless Hash#each_value / #each_key: an external Enumerator over the
   hash's values / keys (each pair from the generic walker, second/first
   element taken). */
static sp_PolyArray *sp_enum_hash_side(sp_RbVal h, int keyside);
sp_Enumerator *sp_Enumerator_new_gen(void (*gen)(sp_Fiber *), void *cap, sp_RbVal size);
/* Lazy stepping for a blockless #each over an endless integer range: rather
   than materialize (impossible -- sp_enum_items_from raises RangeError), yield
   first, first+step, ... forever through the fiber-generator path, so #next /
   #peek step one value at a time and #rewind restarts from `first` (#3229). */
typedef struct { sp_int first; sp_int step; } sp_endless_range_cap;
static void sp_endless_range_gen(sp_Fiber *f) {
  sp_endless_range_cap *cap = (sp_endless_range_cap *)f->user_data;
  sp_int v = cap->first;
  for (;;) { sp_Fiber_yield(sp_box_int(v)); v += cap->step; }
}
/* Blockless Kernel#loop: an infinite Enumerator yielding nil forever (#3236).
   Defined out-of-line in sp_cold.c (one linked copy, not per generated TU). */
sp_Enumerator *sp_loop_enum(void);
static sp_Enumerator *sp_Enumerator_new_from(sp_RbVal arr) {
  /* The receiver is the caller's temporary -- `(11..55).each` passes it straight
     in, unreachable from anywhere else -- and BOTH arms below allocate before
     they are done with it, then keep it as the enumerator's `source`. Root it
     for the whole function: the materialized arm reads it back after the
     enumerator's own allocation, and the endless arm reads `first` and `step`
     out of it after the capture's. */
  SP_GC_ROOT_RBVAL(arr);
  if (arr.tag == SP_TAG_OBJ && arr.cls_id == SP_BUILTIN_RANGE && arr.v.p &&
      ((sp_Range *)arr.v.p)->last == INTPTR_MAX) {
    sp_Range *r = (sp_Range *)arr.v.p;
    sp_endless_range_cap *cap = (sp_endless_range_cap *)sp_gc_alloc(sizeof *cap, NULL, NULL);
    /* The capture is the second slot: it dies inside sp_Enumerator_new_gen and
       is read later still, when the fiber first runs sp_endless_range_gen. */
    SP_GC_ROOT(cap);
    cap->first = r->first; cap->step = r->step ? r->step : 1;
    sp_Enumerator *e = sp_Enumerator_new_gen(sp_endless_range_gen, cap, sp_box_nil());
    e->source = arr; e->meth = "each";
    return e;
  }
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE; e->size = sp_box_nil(); e->feed = sp_box_nil(); e->has_feed = FALSE; e->gen_result = sp_box_nil(); e->source = arr; e->meth = "each";
  return e;
}
/* Stamp the iterated receiver and creating method onto a fresh Enumerator so
   #inspect shows the true origin (`#<Enumerator: "abc":each_char>`), not the
   materialized snapshot. Returns the enumerator for ctor-expression chaining. */
sp_Enumerator *sp_enum_with_src(sp_Enumerator *e, sp_RbVal src, const char *meth);
sp_Enumerator *sp_enum_with_src(sp_Enumerator *e, sp_RbVal src, const char *meth);
/* Mark an eagerly-materialized enumerator to #inspect as a Generator wrapper
   (what CRuby shows for chunk_while / slice_when / chunk without a terminal). */
sp_Enumerator *sp_enum_as_gen(sp_Enumerator *e);
sp_Enumerator *sp_enum_as_gen(sp_Enumerator *e);
/* Enumerator#with_index block form returns whatever the enumerator's each
   returns with that block: for an `each`/`each_with_index` enumerator that is
   the source receiver itself. A stored collector enumerator (blockless map/
   select/...) would have to rebuild its collected result here -- reject that
   loudly rather than hand back the wrong value. */
sp_RbVal sp_enum_with_index_value(sp_Enumerator *e);
sp_RbVal sp_enum_with_index_value(sp_Enumerator *e);
/* Enumerator#with_index block-form return value, given the block results collected
   as `mapped`: a map/collect enumerator returns the mapped array; each/
   each_with_index returns the source receiver; anything else is unsupported. */
sp_RbVal sp_enum_with_index_result(sp_Enumerator *e, sp_PolyArray *mapped);
sp_RbVal sp_enum_with_index_result(sp_Enumerator *e, sp_PolyArray *mapped);
static sp_PolyArray *sp_enum_hash_side(sp_RbVal h, int keyside) {
  sp_int n = sp_poly_length(h);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal pair = sp_poly_each_elem(h, i); SP_GC_ROOT_RBVAL(pair);
    sp_PolyArray_push(r, sp_poly_arr_get(pair, keyside ? 0 : 1));
  }
  return r;
}

/* Like sp_Enumerator_new_from but over a reversed snapshot, for a blockless
   Array#reverse_each. sp_enum_items_from always returns a fresh, owned poly
   array (every arm allocates a new one), so reversing it in place does not
   touch the receiver's backing store. (The reverse_each-doesn't-mutate test
   guards this invariant if sp_enum_items_from is ever changed to share.) */
static sp_Enumerator *sp_Enumerator_new_from_rev(sp_RbVal arr) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  if (items) {
    for (sp_int i = 0, j = items->len - 1; i < j; i++, j--) {
      sp_RbVal t = items->data[i]; items->data[i] = items->data[j]; items->data[j] = t;
    }
  }
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE; e->size = sp_box_nil(); e->feed = sp_box_nil(); e->has_feed = FALSE; e->gen_result = sp_box_nil(); e->source = arr; e->meth = "reverse_each";
  return e;
}
/* Wrap an already-built poly array as an Enumerator, taking ownership of it
   (no re-snapshot). Lets callers that already hold a fresh array skip the
   sp_enum_items_from copy in sp_Enumerator_new_from. */
sp_Enumerator *sp_Enumerator_new_from_items(sp_PolyArray *items);
sp_Enumerator *sp_enum_of_one(sp_RbVal v, const char *meth);
/* Enumerable#chain(*others) / Enumerator#+ : the sources are materialized and
   concatenated by the caller (the desugar builds `recv.to_a + other.to_a ...`),
   so the chain is a snapshot enumerator that reports as Enumerator::Chain. */
static sp_Enumerator *sp_enum_chain_new(sp_RbVal arr) __attribute__((unused));
static sp_Enumerator *sp_enum_chain_new(sp_RbVal arr) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  sp_Enumerator *e = sp_Enumerator_new_from_items(items);
  e->is_chain = TRUE;
  return e;
}
/* A blockless Array#each_with_index enumerator: an [element, index] pair for
   each element (index offset by `off`, as Enumerator#with_index(off) allows). */
static sp_Enumerator *sp_Enumerator_new_ewi(sp_RbVal arr, sp_int off) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  sp_PolyArray *pairs = sp_PolyArray_new();
  SP_GC_ROOT(pairs);
  sp_int n = items ? items->len : 0;
  for (sp_int i = 0; i < n; i++) {
    sp_PolyArray *pair = sp_PolyArray_new();
    SP_GC_ROOT(pair);
    sp_PolyArray_push(pair, items->data[i]);
    sp_PolyArray_push(pair, sp_box_int(i + off));
    sp_PolyArray_push(pairs, sp_box_poly_array(pair));
  }
  { sp_Enumerator *e = sp_Enumerator_new_from_items(pairs); e->source = arr; e->meth = "each_with_index"; return e; }
}
/* A blockless Array#each_index enumerator: the indices 0..len-1. */
static sp_Enumerator *sp_Enumerator_new_indices(sp_RbVal arr) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  sp_PolyArray *idx = sp_PolyArray_new();
  SP_GC_ROOT(idx);
  sp_int n = items ? items->len : 0;
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(idx, sp_box_int(i));
  { sp_Enumerator *e = sp_Enumerator_new_from_items(idx); e->source = arr; e->meth = "each_index"; return e; }
}
/* Array#each_slice(n) with no block: a materialized Enumerator whose items are
   the consecutive non-overlapping slices of length n (the last may be short).
   `slice` is block-scoped, so its GC root pops each iteration; `out` keeps the
   pushed slices alive. */
/* arr.cycle(n) with no block: the elements repeated n whole times. */
static sp_Enumerator *sp_Enumerator_new_cycle(sp_RbVal arr, sp_int n) {
  sp_PolyArray *items = sp_enum_items_from(arr); SP_GC_ROOT(items);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  sp_int len = items ? items->len : 0;
  for (sp_int r = 0; r < n; r++)
    for (sp_int i = 0; i < len; i++) sp_PolyArray_push(out, items->data[i]);
  { sp_Enumerator *e = sp_Enumerator_new_from_items(out); e->source = arr; return e; }
}
/* slice_before/slice_after with a pattern VALUE: start a new group before
   (after) each element == pattern. Groups are poly arrays. */
/* Generic `pattern === element` on boxed values (#2847): a Class pattern
   dispatches through the generated class machinery (installed as a hook by
   sp_re_init when the program carries it); Regexp matches a String; a Range
   covers numerics; everything else is value equality. */
static int (*sp_poly_is_a_hook)(sp_RbVal, sp_Class) = NULL;
static sp_bool sp_poly_case_eq(sp_RbVal pat, sp_RbVal e) {
  if (pat.tag == SP_TAG_CLASS)
    return sp_poly_is_a_hook ? (sp_bool)(sp_poly_is_a_hook(e, sp_unbox_class(pat)) != 0) : 0;
  if (pat.tag == SP_TAG_OBJ && pat.cls_id == SP_BUILTIN_REGEX)
    return e.tag == SP_TAG_STR && e.v.s && sp_re_match_p(pat.v.p, e.v.s);
  if (pat.tag == SP_TAG_OBJ && pat.cls_id == SP_BUILTIN_RANGE) {
    sp_Range *r = (sp_Range *)pat.v.p;
    if (e.tag == SP_TAG_INT) return sp_range_include(r, e.v.i);
    if (e.tag == SP_TAG_FLT)
      return e.v.f >= (sp_float)r->first &&
             (r->excl ? e.v.f < (sp_float)r->last : e.v.f <= (sp_float)r->last);
    return 0;
  }
  if (pat.tag == SP_TAG_OBJ && pat.cls_id == SP_BUILTIN_FLOAT_RANGE) {
    if (e.tag != SP_TAG_INT && e.tag != SP_TAG_FLT) return 0;
    return sp_frange_cover(*(sp_FloatRange *)pat.v.p, sp_poly_to_f(e));
  }
  /* ("a".."e") === "c": a string range covers by string comparison (#3963) */
  if (pat.tag == SP_TAG_OBJ && pat.cls_id == SP_BUILTIN_STR_RANGE) {
    sp_RbVal ed = sp_poly_strbuf_deref(e);
    if (ed.tag != SP_TAG_STR) return 0;
    return sp_srange_cover(*(sp_StrRange *)pat.v.p, ed.v.s ? ed.v.s : sp_str_empty);
  }
  /* a shared-mutable string on either side behaves as its value (#3227) */
  if (sp_poly_is_strbuf(pat) || sp_poly_is_strbuf(e))
    return sp_poly_eq(sp_poly_strbuf_deref(pat), sp_poly_strbuf_deref(e));
  if (pat.tag == SP_TAG_OBJ && pat.cls_id == SP_BUILTIN_REGEX && e.tag == SP_TAG_SYM)
    return sp_re_case_eq((mrb_regexp_pattern *)pat.v.p, e);
  return sp_poly_eq(pat, e);
}
static sp_PolyArray *sp_poly_slice_groups(sp_RbVal arr, sp_RbVal pat, int after) {
  sp_PolyArray *items = sp_enum_items_from(arr); SP_GC_ROOT(items);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  sp_PolyArray *cur = sp_PolyArray_new(); SP_GC_ROOT(cur);
  sp_int len = items ? items->len : 0;
  for (sp_int i = 0; i < len; i++) {
    sp_RbVal e = items->data[i];
    if (!after && cur->len > 0 && sp_poly_case_eq(pat, e)) {
      sp_PolyArray_push(out, sp_box_poly_array(cur));
      cur = sp_PolyArray_new();
    }
    sp_PolyArray_push(cur, e);
    if (after && sp_poly_case_eq(pat, e)) {
      sp_PolyArray_push(out, sp_box_poly_array(cur));
      cur = sp_PolyArray_new();
    }
  }
  if (cur->len > 0) sp_PolyArray_push(out, sp_box_poly_array(cur));
  return out;
}
static sp_Enumerator *sp_Enumerator_new_slices(sp_RbVal arr, sp_int n) {
  SP_GC_ROOT_RBVAL(arr);   /* published into the enumerator below, after several allocations */
  if (n < 1) sp_raise_cls("ArgumentError", "invalid slice size");
  sp_PolyArray *items = sp_enum_items_from(arr); SP_GC_ROOT(items);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  sp_int len = items ? items->len : 0;
  /* `len - i` and the `i + n <= len` guard keep every sum in range, so a large
     n can't overflow sp_int (undefined behavior). */
  for (sp_int i = 0; i < len; ) {
    sp_PolyArray *slice = sp_PolyArray_new(); SP_GC_ROOT(slice);
    sp_int limit = len - i < n ? len : i + n;
    for (sp_int j = i; j < limit; j++) sp_PolyArray_push(slice, items->data[j]);
    sp_PolyArray_push(out, sp_box_poly_array(slice));
    if (len - i <= n) break;
    i += n;
  }
  { sp_Enumerator *e = sp_Enumerator_new_from_items(out); e->source = arr; e->meth = sp_sprintf("each_slice(%lld)", (long long)n); return e; }
}
/* Array#each_cons(n) with no block: a materialized Enumerator whose items are
   the sliding windows of length n (none when len < n). */
static sp_Enumerator *sp_Enumerator_new_cons(sp_RbVal arr, sp_int n) {
  SP_GC_ROOT_RBVAL(arr);   /* published into the enumerator below, after several allocations */
  if (n < 1) sp_raise_cls("ArgumentError", "invalid size");
  sp_PolyArray *items = sp_enum_items_from(arr); SP_GC_ROOT(items);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  sp_int len = items ? items->len : 0;
  /* Guarding on len >= n and iterating to len - n keeps i + n <= len, so a
     large n can't overflow sp_int (undefined behavior). */
  if (len >= n) {
    for (sp_int i = 0; i <= len - n; i++) {
      sp_PolyArray *win = sp_PolyArray_new(); SP_GC_ROOT(win);
      for (sp_int j = i; j < i + n; j++) sp_PolyArray_push(win, items->data[j]);
      sp_PolyArray_push(out, sp_box_poly_array(win));
    }
  }
  { sp_Enumerator *e = sp_Enumerator_new_from_items(out); e->source = arr; e->meth = sp_sprintf("each_cons(%lld)", (long long)n); return e; }
}
/* Blockless <enum>.with_index(off): a materialized Enumerator whose items are
   the [element, off + i] pairs of the source enumerator's items. The source is
   always a materialized enumerator here (each / each_char / each_slice / ...);
   a generator enumerator never reaches this path. */
sp_Enumerator *sp_Enumerator_with_index(sp_Enumerator *e, sp_int off);
/* A string's characters as a fresh poly array of one-char Strings, built
   directly. Used by a blockless String#each_char enumerator, avoiding the
   intermediate sp_StrArray that sp_str_chars + sp_enum_items_from would
   allocate and then copy. */
/* sp_str_chars_poly: moved to lib/sp_cold.c */
sp_PolyArray *sp_str_chars_poly(const char *s);
sp_Enumerator *sp_Enumerator_new_gen(void (*gen)(sp_Fiber *), void *cap, sp_RbVal size);

/* Enumerator#inspect: the CRuby "#<Enumerator: <source>:<method>>" for a
   materialized enumerator with a known source; a generator has no printable
   receiver and renders a Generator placeholder (CRuby shows its address). */
static const char *sp_enum_inspect(sp_Enumerator *e) {
  if (!e) return SPL("nil");
  if (e->gen || e->gen_label)
    return sp_sprintf("#<Enumerator: #<Enumerator::Generator:0x%016llx>:each>",
                      (unsigned long long)(uintptr_t)e);
  sp_RbVal src = (e->has_src || e->source.tag != SP_TAG_NIL) ? e->source
               : sp_box_poly_array(e->items ? e->items : sp_PolyArray_new());
  return sp_sprintf("#<Enumerator: %s:%s>", sp_poly_inspect(src), e->meth ? e->meth : "each");
}
/* Pull the next value from the generator fiber, or raise StopIteration when it
   has run to completion. A resume that ends the body terminates the fiber and
   returns the body value, which is discarded in favor of StopIteration. */
sp_RbVal sp_enum_gen_pull(sp_Enumerator *e);
/* Enumerator.product(a, b[, c]): an Enumerator over the cartesian product,
   materialized as poly-array tuples in row-major order (#2484). */
static sp_Enumerator *sp_Enumerator_product2(sp_RbVal a, sp_RbVal b) {
  sp_int na = sp_poly_length(a), nb = sp_poly_length(b);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  for (sp_int i = 0; i < na; i++)
    for (sp_int j = 0; j < nb; j++) {
      sp_PolyArray *t = sp_PolyArray_new();
      sp_PolyArray_push(t, sp_poly_arr_get(a, i));
      sp_PolyArray_push(t, sp_poly_arr_get(b, j));
      sp_PolyArray_push(out, sp_box_poly_array(t));
    }
  return sp_Enumerator_new_from(sp_box_poly_array(out));
}
static sp_Enumerator *sp_Enumerator_product3(sp_RbVal a, sp_RbVal b, sp_RbVal cc) {
  sp_int na = sp_poly_length(a), nb = sp_poly_length(b), nc = sp_poly_length(cc);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  for (sp_int i = 0; i < na; i++)
    for (sp_int j = 0; j < nb; j++)
      for (sp_int k = 0; k < nc; k++) {
        sp_PolyArray *t = sp_PolyArray_new();
        sp_PolyArray_push(t, sp_poly_arr_get(a, i));
        sp_PolyArray_push(t, sp_poly_arr_get(b, j));
        sp_PolyArray_push(t, sp_poly_arr_get(cc, k));
        sp_PolyArray_push(out, sp_box_poly_array(t));
      }
  return sp_Enumerator_new_from(sp_box_poly_array(out));
}
sp_RbVal sp_Enumerator_next(sp_Enumerator *e);
sp_RbVal sp_Enumerator_peek(sp_Enumerator *e);
/* #next_values / #peek_values return the yielded value(s) as an array. A yield of
   several values (already a poly array element) is returned as-is; a single value
   is wrapped in a one-element array. (#2482) */
sp_PolyArray *sp_enum_values_wrap(sp_RbVal v);
sp_PolyArray *sp_Enumerator_next_values(sp_Enumerator *e);
sp_PolyArray *sp_Enumerator_peek_values(sp_Enumerator *e);
sp_Enumerator *sp_Enumerator_rewind(sp_Enumerator *e);
/* Enumerator#size, defined after sp_proc_call so a callable size can be driven
   through the boxed-return channel. */
sp_RbVal sp_Enumerator_size(sp_Enumerator *e);
/* Enumerator#feed(v): sets the value the generator's next Fiber.yield returns
   (inside `y.yield`). Raises TypeError if a feed value is already pending, per
   CRuby; consumed by the next #next. Returns nil. */
sp_RbVal sp_Enumerator_feed(sp_Enumerator *e, sp_RbVal v);
/* Enumerator#take(n) / #first(n): collect up to n values from a fresh run of the
   source (independent of the #next cursor), matching CRuby. */
sp_PolyArray *sp_Enumerator_take(sp_Enumerator *e, sp_int n);
/* Enumerator#to_a / #entries: drain the whole source into an array (a fresh run
   of the generator, independent of the #next cursor), matching CRuby. */
sp_PolyArray *sp_Enumerator_to_a(sp_Enumerator *e);
static sp_PolyArray *sp_zip_arg(sp_RbVal v) {
  /* every OBJ tag that carries an each: the array kinds, a Range, the hashes
     (which enumerate as their [key, value] pairs), an Enumerator, and a user
     object with #to_a -- exactly what sp_poly_arr_recv already walks. */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_ENUMERATOR)
    return sp_Enumerator_to_a((sp_Enumerator *)v.v.p);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE)
    return sp_poly_to_poly_array(v);
  if (v.tag == SP_TAG_OBJ) return sp_poly_arr_recv(v, "each");
  sp_raise_cls("TypeError", sp_sprintf("wrong argument type %s (must respond to :each)",
                                       sp_poly_class_name(v)));
  return sp_PolyArray_new();
}
/* The boxed-receiver entry points the poly surface above forwards to: an
   Enumerator read out of a container answers #to_a / #next like the typed
   receiver does. Separate thunks because sp_Enumerator is only a type from
   here down (#3843). */
static sp_PolyArray *sp_enum_to_a_boxed(sp_RbVal v) {
  return sp_Enumerator_to_a((sp_Enumerator *)v.v.p);
}
static sp_RbVal sp_enum_next_boxed(sp_RbVal v) {
  return sp_Enumerator_next((sp_Enumerator *)v.v.p);
}
/* Universal proc return channel: every first-class proc publishes its result
   here, boxed, and the .call site unboxes it back to the call's inferred type
   (CRuby's uniform boxed-VALUE proc ABI). A file-static per TU, like the
   sp_exc_stack machinery -- the compose/curry trampolines below and every
   generated proc body live in the same TU and share this slot. Per-worker
   (SP_TLS): a concurrent Proc#call would otherwise race, and no safepoint poll
   lies between a body's store and the call site's read. */
#ifdef SPINEL_EXT_HOST
extern SP_TLS sp_RbVal _sp_proc_poly_ret;
#else
SP_TLS sp_RbVal _sp_proc_poly_ret;
#endif
/* The name a method was CALLED by, for __callee__ to answer when it differs
   from the definition's (an alias shares the definition's one function, so the
   name cannot come from the body). Written by the call site just before the
   call and consumed -- and cleared -- by the callee's prologue, so a path that
   does not write it leaves __callee__ on its static answer (#3729). */
#ifdef SPINEL_EXT_HOST
extern SP_TLS const char *sp_callee_name;
#else
SP_TLS const char *sp_callee_name = NULL;
#endif
/* Boxed-argument side-channel of the same ABI: a poly (or float) proc
   parameter reads its argument back from here, since it does not fit the
   sp_int[] slot. Declared here so the compose/curry/to_proc trampolines
   below can publish through it like every generated call site does. */
#ifdef SPINEL_EXT_HOST
extern SP_TLS sp_RbVal _sp_proc_poly_args[16];
#else
SP_TLS sp_RbVal _sp_proc_poly_args[16];
#endif
/* The block passed to a first-class proc's .call { }: the caller publishes it
   here just before sp_proc_call, and the callee's &block-param prologue
   consumes (and clears) it. Same discipline as _sp_proc_poly_args (#2648). */
static SP_TLS sp_Proc *_sp_proc_blk;
/* ---- --rbs seed assertions (-DSP_RBS_CHECK) ----------------------------
   A seed is trusted, never verified (docs/rbs-extract.md): the analyzer pins
   the slot and codegen narrows whatever arrives into it, so a signature the
   program contradicts silently reinterprets the value rather than degrading to
   the inferred type. Under -DSP_RBS_CHECK every narrowing of a boxed value
   into a seeded slot carries a tag assertion and aborts at the store instead.
   Without the define the macro is the value itself, so a release build is
   unchanged -- which is the point: the check costs nothing to leave emitted.

   Tag level only. A nil always passes: an unset slot reads nil, and a
   non-nullable seed still sees `@x = nil` in an initialize. Object identity
   is not checked either, since a subclass in an ancestor-typed slot is
   correct and layout-compatible. What this catches is the reinterpretation
   family -- a pointer landing in a scalar slot and the reverse. */
#ifdef SP_RBS_CHECK
static sp_RbVal sp_rbs_check(sp_RbVal v, int want, const char *slot, const char *wantname) {
  if (v.tag == want || v.tag == SP_TAG_NIL) return v;
  fprintf(stderr,
          "spinel: --rbs seed violated: %s is declared %s but holds %s\n"
          "  The signature is trusted and the emitted code reinterprets the value.\n"
          "  Fix the signature or the program; this build was made with -DSP_RBS_CHECK.\n",
          slot, wantname, sp_poly_class_name(v));
  abort();
}
#define SP_RBS_CHECK_TAG(v, want, slot, wantname) sp_rbs_check((v), (want), (slot), (wantname))
#else
#define SP_RBS_CHECK_TAG(v, want, slot, wantname) (v)
#endif

#ifdef SPINEL_EXT_HOST
sp_int sp_proc_call(sp_Proc *p, sp_int argc, sp_int *args);
#else
sp_int sp_proc_call(sp_Proc *p, sp_int argc, sp_int *args) { if (!p || !p->fn) return 0; if (!args) { sp_int noargs[16] = {0}; return ((sp_int (*)(void *, sp_int, sp_int *))p->fn)(p->cap, 0, noargs); } return ((sp_int (*)(void *, sp_int, sp_int *))p->fn)(p->cap, argc, args); }
#endif

/* Run the at_exit hooks, most recently registered first, and answer the status
   the process should end with. `status` in is what the terminating path would
   have used on its own (0 falling off main's end, N for `exit N`, 1 for abort
   or an uncaught raise); a hook that ends the program replaces it, and the
   last such hook wins -- which is CRuby's rule: `exit 7` in a hook makes the
   program exit 7, and a hook raising anything else makes it exit 1.

   Each hook runs under its own protect frame -- the same sequence the ext-host
   `<init>_try` helper arms (src/codegen.c) -- because that is what CRuby does:
   every end proc runs inside its own rb_protect, so a hook that exits, aborts
   or raises ends THAT hook and the ones after it still run. Without the frame
   `exit` inside a hook would take sp_exit_raise's direct branch and kill the
   process from inside this loop, and an uncaught error waiting to be printed
   after the drain would be lost with it. On a landing the frame's own exception
   is read out of the slot it was stored in: a SystemExit gives the new status,
   anything else is printed where it happened -- CRuby prints a hook's error at
   once, before the remaining hooks run -- and leaves the status 1.

   Each hook is also popped off the table BEFORE it is called, so a hook that
   registers another one during the drain gets it run (CRuby does too), and no
   hook can run twice. The popped hook is rooted for the duration of its call:
   the table is the collector's only handle on it, and the pop takes that handle
   away. */
static int sp_at_exit_run(int status) {
  /* The hooks run at top level. A raise that reached here unhandled, or an exit
     from inside a walk, abandoned whatever walk it was inside; drop the frames
     it left, or the first hook to inspect or compare that same object is told
     it is already inside it. */
  sp_poly_recur_pop(0);
  /* The hooks run at termination, where the path that got here has left its own
     raise in the pending slots. Clear them, or the first `raise` inside a hook
     binds the TERMINATING exception instead of its own: sp_raise_cls' recovery
     guards read sp_pending_exc_obj and take it as the object being raised. */
  sp_pending_exc_obj = NULL; sp_pending_cause = NULL; sp_inflight_cause = NULL;
  sp_explicit_cause = NULL; sp_explicit_cause_set = 0;
  /* setjmp: `st` is written on the landing path and read after it. */
  volatile int st = status;
  sp_int args[16] = {0};   /* sp_proc_call's fixed slot convention */
  for (;;) {
    SP_AT_EXIT_LOCK();
    sp_int n = sp_at_exit_count;
    sp_Proc *h = n > 0 ? sp_at_exit_hooks[--sp_at_exit_count] : NULL;
    SP_AT_EXIT_UNLOCK();
    if (n <= 0) break;
    SP_GC_ROOT(h);
    if (!h) continue;
    /* Every hook starts from an empty proc-return / break / catch state, the
       way CRuby's end procs do: it has unwound the whole program before the
       first one runs, and a hook that ends by raising has unwound itself
       before the next one does. So a `break` or `throw` from a hook, or a
       `return` through a proc that escaped a method an earlier hook raised
       out of, MISSES and raises what CRuby raises -- LocalJumpError,
       UncaughtThrowError -- instead of longjmping into a C frame that has
       already returned. (A `return` written in the hook itself is compiled
       as the block's own return and never reaches these stacks.) Only the
       exception stack is left alone: this loop's own frame is on it. */
    sp_proc_ret_head = NULL; sp_brk_top = 0; sp_catch_top = 0;
    /* The check every other arm of this stack has. It cannot fire here --
       every path into the drain arrives with the exception stack empty -- but
       an arm without it is the one a reader has to reason about. */
    sp_exc_check_depth();
    sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;
    sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;
    if (setjmp(sp_exc_stack[sp_exc_top - 1]) == 0) {
      sp_proc_call(h, 0, args);
      sp_exc_top--;
      continue;
    }
    sp_exc_top--;
    sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];
    { const char *ecls = sp_exc_cls[sp_exc_top];
      const char *emsg = sp_exc_msg[sp_exc_top];
      void *eobj = sp_exc_obj[sp_exc_top];
      SP_GC_ROOT_STR(emsg);   /* printing formats a backtrace, which allocates */
      SP_GC_ROOT(eobj);
      if (ecls && strcmp(ecls, "SystemExit") == 0) st = sp_exc_exit_status(eobj);
      else {
        sp_exc_print_uncaught(ecls ? ecls : "RuntimeError", emsg);
        st = 1;
      }
    }
  }
  return st;
}
/* ---- Kernel#Integer / Kernel#Float on a user object ----
   CRuby's rb_convert_to_integer / rb_convert_to_float for an object of a
   user class: its #to_int, #to_str and #to_i (Integer) or its #to_f
   (Float), reached through the generated bridge (sp_obj_conv_fn) whatever
   each method's static type, every answer judged here. One path for a
   statically typed object and for one read out of a container, for the
   strict and the `exception: false` forms; the typed entry points at the
   end unbox for the slot the call site has. */
enum { SP_CONV_TO_INT, SP_CONV_TO_I, SP_CONV_TO_F, SP_CONV_TO_STR };

/* rb_protect: run fn(ctx) under a frame of its own and answer 1 when it
   did not return -- the at_exit drain's frame shape. A raise lands here with
   the exception discarded; so does a `throw`, a `break` or a proc `return`
   started inside fn, which CRuby's rb_protect catches the same way and
   rb_convert_to_integer discards. Their initiators cut the catch and break
   stacks down to the target and leave the unwind in flight for the ensures
   on the way, so the landing puts every handler stack back to what it was
   when the frame was armed and takes the unwind out of flight; otherwise
   the next ensure epilogue would resume it into a frame that has returned.
   Kernel#Integer probes #to_int this way, and the `exception: false` forms
   probe every conversion this way. */
static int sp_exc_protect(void (*fn)(void *), void *ctx) {
  int catch_top = sp_catch_top, brk_top = sp_brk_top;
  sp_proc_home *ret_head = sp_proc_ret_head;
  sp_exc_check_depth();
  sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;
  sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;
  if (setjmp(sp_exc_stack[sp_exc_top - 1]) == 0) { fn(ctx); sp_exc_top--; return 0; }
  sp_exc_top--;
  sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];
  sp_catch_top = catch_top; sp_brk_top = brk_top; sp_proc_ret_head = ret_head;
  sp_unwind_kind = SP_UNWIND_NONE;
  /* a `raise x, cause: expr` stages its cause before evaluating expr; an
     unwind out of expr that lands here must not leave it staged for the next
     raise */
  sp_explicit_cause = NULL; sp_explicit_cause_set = 0;
  return 1;
}
typedef struct { sp_RbVal obj; int which; int had; sp_RbVal ans; } sp_obj_conv_probe;
static void sp_obj_conv_probe_run(void *p) {
  sp_obj_conv_probe *c = (sp_obj_conv_probe *)p;
  c->had = sp_obj_conv_fn((int)c->obj.cls_id, c->obj.v.p, c->which, &c->ans);
}
/* One conversion method of the object: 0 when its class has none (asked of
   the bridge first, so no frame is armed for nothing), 1 with the boxed
   answer in *ans, -1 when the call did not return (only under `protect`;
   *ans is left alone). The caller roots the object across the call and the
   answer after it. */
static int sp_obj_conv(sp_RbVal obj, int which, int protect, sp_RbVal *ans) {
  sp_obj_conv_probe c;
  if (!sp_obj_conv_fn || !sp_obj_conv_fn((int)obj.cls_id, obj.v.p, which, NULL)) return 0;
  c.obj = obj; c.which = which; c.had = 0; c.ans = sp_box_nil();
  if (!protect) sp_obj_conv_probe_run(&c);
  else if (sp_exc_protect(sp_obj_conv_probe_run, &c)) return -1;
  *ans = c.ans;
  return c.had;
}
static int sp_obj_conv_is_integer(sp_RbVal a) { return a.tag == SP_TAG_INT || a.tag == SP_TAG_BIGINT; }
/* The bytes of a String answer, or NULL for an answer of any other kind. */
static const char *sp_obj_conv_str_of(sp_RbVal a) {
  if (a.tag == SP_TAG_STR) return a.v.s ? a.v.s : sp_str_empty;
  if (sp_poly_is_strbuf(a)) return sp_String_cstr((sp_String *)a.v.p);
  return NULL;
}
/* Integer("...") on a String: strict, or nil-answering for the
   `exception: false` form. `base` 0 is the bare form. */
static sp_RbVal sp_obj_conv_str_Integer(const char *s, sp_int base, int raise) {
  sp_int r;
  if (raise) return sp_box_int(base ? sp_str_to_i_strict_base(s, base) : sp_str_to_i_strict(s));
  r = sp_str_to_i_lenient_base(s, base);
  return r == SP_INT_NIL ? sp_box_nil() : sp_box_int(r);
}
static sp_RbVal sp_obj_Integer_val(sp_RbVal v, sp_int base, int raise) {
  sp_RbVal a = sp_box_nil();
  const char *cn, *s;
  int had;
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT_RBVAL(a);
  cn = sp_poly_class_name(v);
  if (!base) {
    /* #to_int, protected: an Integer answer wins; nil, another kind, or a
       raise inside it are all swallowed and the search goes on */
    had = sp_obj_conv(v, SP_CONV_TO_INT, 1, &a);
    if (had > 0 && sp_obj_conv_is_integer(a)) return a;
  }
  /* #to_str, bare: a String parses as Integer("...") does, with the base if
     one was given, and an answer of any other kind is the conversion's own
     TypeError, in the `exception: false` form too */
  had = sp_obj_conv(v, SP_CONV_TO_STR, 0, &a);
  if (had > 0 && (s = sp_obj_conv_str_of(a)) != NULL) return sp_obj_conv_str_Integer(s, base, raise);
  if (had > 0 && a.tag != SP_TAG_NIL)
    sp_raise_cls("TypeError", sp_sprintf("can't convert %s to String (%s#to_str gives %s)",
                                         cn, cn, sp_poly_class_name(a)));
  if (base) {
    /* with a base only a String converts */
    if (raise) sp_raise_cls("ArgumentError", "base specified for non string value");
    return sp_box_nil();
  }
  /* #to_i: bare in the strict form, so a raise inside it propagates;
     protected in the `exception: false` form, which answers nil for every
     failure */
  had = sp_obj_conv(v, SP_CONV_TO_I, !raise, &a);
  if (had > 0 && sp_obj_conv_is_integer(a)) return a;
  if (!raise) return sp_box_nil();
  if (had == 0) sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Integer", cn));
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s to Integer (%s#to_i gives %s)",
                                       cn, cn, sp_poly_class_name(a)));
  return sp_box_nil();
}
static sp_RbVal sp_obj_Float_val(sp_RbVal v, int raise) {
  sp_RbVal a = sp_box_nil();
  const char *cn;
  int had;
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT_RBVAL(a);
  cn = sp_poly_class_name(v);
  /* #to_f alone (never #to_str, never #to_int): bare in the strict form,
     protected and nil-answering under `exception: false` */
  had = sp_obj_conv(v, SP_CONV_TO_F, !raise, &a);
  if (had > 0 && a.tag == SP_TAG_FLT) return a;
  if (!raise) return sp_box_nil();
  if (had == 0) sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", cn));
  sp_raise_cls("TypeError", sp_sprintf("can't convert %s to Float (%s#to_f gives %s)",
                                       cn, cn, sp_poly_class_name(a)));
  return sp_box_nil();
}
/* Kernel#Integer's answer, boxed: a user object through its conversions; a
   plain value through the strict arms (sp_poly_Integer), or, under
   `exception: false`, nil for everything those arms raise for. */
static sp_RbVal sp_kernel_Integer_val(sp_RbVal v, sp_int base, int raise) {
  const char *s;
  if (sp_poly_is_user_obj(v)) return sp_obj_Integer_val(v, base, raise);
  /* an Integer or a Bignum is its own answer in either form: through the
     strict arm below a Bignum would be cut to 64 bits before the slot's own
     check could refuse it */
  if (!base && (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT)) return v;
  if (raise && !base) return sp_box_int(sp_poly_Integer(v));
  /* with a base only a String converts (a plain one or a shared handle) */
  if (base) {
    if ((s = sp_obj_conv_str_of(v)) != NULL) return sp_obj_conv_str_Integer(s, base, raise);
    if (raise) sp_raise_cls("ArgumentError", "base specified for non string value");
    return sp_box_nil();
  }
  if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v;
  if (v.tag == SP_TAG_FLT) return isnan(v.v.f) || isinf(v.v.f) ? sp_box_nil() : sp_box_int((sp_int)v.v.f);
  if (v.tag == SP_TAG_STR) return sp_obj_conv_str_Integer(v.v.s ? v.v.s : sp_str_empty, 0, 0);
  return sp_box_nil();
}
/* The entry points the Kernel#Integer / Kernel#Float emits call, one per
   slot kind: the Integer slot, the Bignum slot the analysis types a call as
   when the object's #to_int or #to_i answers one (or answers a boxed value
   that may be one), and the Float slot. An Integer slot cannot carry a
   Bignum: the one a boxed object's conversion answers is the value when it
   fits, and a loud RangeError otherwise (nil under `exception: false`) --
   never a truncated number in silence. */
static sp_int sp_poly_Integer_ex(sp_RbVal v, sp_int base, int raise) {
  sp_RbVal r = sp_kernel_Integer_val(v, base, raise);
  if (r.tag == SP_TAG_NIL) return SP_INT_NIL;
  if (r.tag == SP_TAG_BIGINT) {
    sp_Bigint *bg = (sp_Bigint *)r.v.p;
    sp_int n = (sp_int)sp_bigint_to_int(bg);
    if (sp_bigint_bit_length(bg) <= 63 && n != SP_INT_NIL) return n;
    if (raise) sp_raise_cls("RangeError", "bignum too big to convert into 'long'");
    return SP_INT_NIL;
  }
  return sp_poly_to_i(r);
}
static sp_Bigint *sp_poly_Integer_big(sp_RbVal v, sp_int base, int raise) {
  sp_RbVal r = sp_kernel_Integer_val(v, base, raise);
  return r.tag == SP_TAG_NIL ? NULL : sp_poly_as_bigint(r);
}
static sp_float sp_poly_Float_ex(sp_RbVal v, int raise) {
  if (sp_poly_is_user_obj(v)) {
    sp_RbVal r = sp_obj_Float_val(v, raise);
    return r.tag == SP_TAG_NIL ? sp_float_nil() : r.v.f;
  }
  if (raise) return sp_poly_Float(v);
  if (v.tag == SP_TAG_FLT) return v.v.f;
  if (v.tag == SP_TAG_INT) return (sp_float)v.v.i;
  if (v.tag == SP_TAG_BIGINT) return sp_poly_to_f(v);
  if (v.tag == SP_TAG_STR) return sp_str_to_f_lenient(v.v.s ? v.v.s : sp_str_empty);
  return sp_float_nil();
}


/* ---- Enumerable on a builtin Array receiver, driven by a real sp_Proc ----

   A poly dispatch normally serves a builtin receiver from a pre-arm that
   splices the block inline. It cannot when a user class defines a YIELDING
   method of the same name: the block is then materialized once as a proc and
   shared by every arm, and a second, spliced copy of the body would both
   duplicate the code and disagree with the arm that ran. These drive the same
   materialized proc over the elements instead, so an Array receiver reaching a
   dispatch built for `Relation#map` still runs Array#map (#3409).

   Only the one-value-per-element family lives here; a name whose block takes a
   second argument (each_with_object, inject) is not dispatched this way. */
enum {
  SP_PENUM_EACH, SP_PENUM_MAP, SP_PENUM_SELECT, SP_PENUM_REJECT,
  SP_PENUM_FIND, SP_PENUM_GROUP_BY, SP_PENUM_SORT_BY, SP_PENUM_MIN_BY,
  SP_PENUM_MAX_BY, SP_PENUM_FLAT_MAP, SP_PENUM_COUNT, SP_PENUM_SUM,
  SP_PENUM_ANY, SP_PENUM_ALL, SP_PENUM_NONE, SP_PENUM_PARTITION,
  SP_PENUM_FIND_INDEX, SP_PENUM_TAKE_WHILE, SP_PENUM_DROP_WHILE,
  SP_PENUM_EACH_WITH_INDEX, SP_PENUM_FILTER_MAP
};
/* Call `blk` with one element. Both channels are filled, as every other
   proc-driving site does: a poly parameter reads the boxed side-channel, a
   concrete-typed one reads the sp_int slot -- as a pointer for a heap value,
   since its truncated int projection would be garbage (#2650). */
static sp_RbVal sp_penum_call2(sp_Proc *blk, sp_RbVal v, sp_RbVal w);
static sp_RbVal sp_penum_call1(sp_Proc *blk, sp_RbVal v) {
  /* Autosplat, as a block (never a lambda) does: a 2-parameter block over a
     pair element -- a Hash entry, or an array of pairs -- binds |k, v|. */
  if (blk && blk->arity == 2 && !blk->lambda_p &&
      v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id) && sp_poly_arr_len(v) == 2)
    return sp_penum_call2(blk, sp_poly_arr_get(v, 0), sp_poly_arr_get(v, 1));
  sp_int a[16] = {0};
  a[0] = (v.tag == SP_TAG_OBJ || v.tag == SP_TAG_STR) ? (sp_int)(uintptr_t)v.v.p
                                                      : sp_poly_to_i(v);
  _sp_proc_poly_args[0] = v;
  _sp_proc_poly_ret = sp_box_nil();
  sp_proc_call(blk, 1, a);
  return _sp_proc_poly_ret;
}
static sp_RbVal sp_penum_call2(sp_Proc *blk, sp_RbVal v, sp_RbVal w) {
  sp_int a[16] = {0};
  a[0] = (v.tag == SP_TAG_OBJ || v.tag == SP_TAG_STR) ? (sp_int)(uintptr_t)v.v.p
                                                      : sp_poly_to_i(v);
  a[1] = (w.tag == SP_TAG_OBJ || w.tag == SP_TAG_STR) ? (sp_int)(uintptr_t)w.v.p
                                                      : sp_poly_to_i(w);
  _sp_proc_poly_args[0] = v;
  _sp_proc_poly_args[1] = w;
  _sp_proc_poly_ret = sp_box_nil();
  sp_proc_call(blk, 2, a);
  return _sp_proc_poly_ret;
}
static sp_RbVal sp_poly_enum_proc(sp_RbVal recv, int op, sp_Proc *blk) {
  SP_GC_ROOT_RBVAL(recv);
  /* The block is this loop's only handle on its own captures: the caller's
     temp holding it can be dead by now, and everything the block
     accumulates into hangs off its capture cells. Rooted here rather than
     in sp_penum_call1, which runs per element. */
  SP_GC_ROOT(blk);
  /* sp_poly_arr_len_ex / sp_poly_each_elem, the pair the spliced poly-each
     loop uses: they render a Hash entry as a boxed [k, v] pair, so a hash
     receiver walks its entries here exactly as it would there. */
  sp_int n = sp_poly_arr_len_ex(recv);
  sp_PolyArray *src = sp_PolyArray_new(); SP_GC_ROOT(src);
  for (sp_int i = 0; i < n; i++) sp_PolyArray_push(src, sp_poly_each_elem(recv, i));
  switch (op) {
    case SP_PENUM_EACH:
      for (sp_int i = 0; i < n; i++) sp_penum_call1(blk, src->data[i]);
      return recv;   /* Array#each answers the receiver */
    case SP_PENUM_EACH_WITH_INDEX:
      for (sp_int i = 0; i < n; i++) sp_penum_call2(blk, src->data[i], sp_box_int(i));
      return recv;
    case SP_PENUM_MAP: case SP_PENUM_FLAT_MAP: case SP_PENUM_FILTER_MAP: {
      sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal r = sp_penum_call1(blk, src->data[i]);
        /* flat_map splices a returned array one level deep */
        if (op == SP_PENUM_FLAT_MAP && r.tag == SP_TAG_OBJ && sp_poly_is_array_kind(r.cls_id)) {
          sp_int m = sp_poly_arr_len(r);
          for (sp_int j = 0; j < m; j++) sp_PolyArray_push(out, sp_poly_arr_get(r, j));
        }
        /* filter_map is map then compact: only a truthy value is kept */
        else if (op == SP_PENUM_FILTER_MAP) { if (sp_poly_truthy(r)) sp_PolyArray_push(out, r); }
        else sp_PolyArray_push(out, r);
      }
      return sp_box_poly_array(out);
    }
    case SP_PENUM_SELECT: case SP_PENUM_REJECT: {
      /* Hash#select / #reject answer a Hash, not the pair array every other
         name here answers. */
      if (recv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(recv.cls_id)) {
        sp_PolyPolyHash *h = sp_PolyPolyHash_new(); SP_GC_ROOT(h);
        for (sp_int i = 0; i < n; i++) {
          sp_RbVal e = src->data[i];
          if (sp_poly_truthy(sp_penum_call1(blk, e)) != (op == SP_PENUM_SELECT)) continue;
          sp_PolyPolyHash_set(h, sp_poly_arr_get(e, 0), sp_poly_arr_get(e, 1));
        }
        return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
      }
    }
    /* fall through to the array form */
    /* FALLTHROUGH */
    case SP_PENUM_TAKE_WHILE:
    case SP_PENUM_DROP_WHILE: {
      sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
      int dropping = 1;
      for (sp_int i = 0; i < n; i++) {
        int t = sp_poly_truthy(sp_penum_call1(blk, src->data[i]));
        if (op == SP_PENUM_TAKE_WHILE) { if (!t) break; sp_PolyArray_push(out, src->data[i]); continue; }
        if (op == SP_PENUM_DROP_WHILE) {
          if (dropping && t) continue;
          dropping = 0;
          sp_PolyArray_push(out, src->data[i]);
          continue;
        }
        if (t == (op == SP_PENUM_SELECT)) sp_PolyArray_push(out, src->data[i]);
      }
      return sp_box_poly_array(out);
    }
    case SP_PENUM_PARTITION: {
      sp_PolyArray *yes = sp_PolyArray_new(); SP_GC_ROOT(yes);
      sp_PolyArray *no = sp_PolyArray_new(); SP_GC_ROOT(no);
      for (sp_int i = 0; i < n; i++)
        sp_PolyArray_push(sp_poly_truthy(sp_penum_call1(blk, src->data[i])) ? yes : no, src->data[i]);
      sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
      sp_PolyArray_push(out, sp_box_poly_array(yes));
      sp_PolyArray_push(out, sp_box_poly_array(no));
      return sp_box_poly_array(out);
    }
    case SP_PENUM_FIND:
      for (sp_int i = 0; i < n; i++)
        if (sp_poly_truthy(sp_penum_call1(blk, src->data[i]))) return src->data[i];
      return sp_box_nil();
    case SP_PENUM_FIND_INDEX:
      for (sp_int i = 0; i < n; i++)
        if (sp_poly_truthy(sp_penum_call1(blk, src->data[i]))) return sp_box_int(i);
      return sp_box_nil();
    case SP_PENUM_ANY: case SP_PENUM_ALL: case SP_PENUM_NONE: {
      for (sp_int i = 0; i < n; i++) {
        int t = sp_poly_truthy(sp_penum_call1(blk, src->data[i]));
        if (op == SP_PENUM_ANY && t) return sp_box_bool(TRUE);
        if (op == SP_PENUM_ALL && !t) return sp_box_bool(FALSE);
        if (op == SP_PENUM_NONE && t) return sp_box_bool(FALSE);
      }
      return sp_box_bool(op != SP_PENUM_ANY);
    }
    case SP_PENUM_COUNT: {
      sp_int k = 0;
      for (sp_int i = 0; i < n; i++) if (sp_poly_truthy(sp_penum_call1(blk, src->data[i]))) k++;
      return sp_box_int(k);
    }
    case SP_PENUM_SUM: {
      sp_RbVal acc = sp_box_int(0);
      for (sp_int i = 0; i < n; i++) acc = sp_poly_add(acc, sp_penum_call1(blk, src->data[i]));
      return acc;
    }
    case SP_PENUM_GROUP_BY: {
      sp_PolyPolyHash *h = sp_PolyPolyHash_new(); SP_GC_ROOT(h);
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal k = sp_penum_call1(blk, src->data[i]);
        sp_RbVal cur = sp_PolyPolyHash_get(h, k);
        sp_PolyArray *bucket;
        if (cur.tag == SP_TAG_OBJ && cur.cls_id == SP_BUILTIN_POLY_ARRAY) bucket = (sp_PolyArray *)cur.v.p;
        else { bucket = sp_PolyArray_new(); sp_PolyPolyHash_set(h, k, sp_box_poly_array(bucket)); }
        sp_PolyArray_push(bucket, src->data[i]);
      }
      return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
    }
    case SP_PENUM_MIN_BY: case SP_PENUM_MAX_BY: {
      sp_RbVal best = sp_box_nil(), bestk = sp_box_nil();
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal k = sp_penum_call1(blk, src->data[i]);
        if (i == 0) { best = src->data[i]; bestk = k; continue; }
        sp_bool ok = FALSE;
        sp_int cmp = sp_poly_cmp(k, bestk, &ok);
        if (!ok) sp_poly_cmp_fail(k, bestk);
        if (op == SP_PENUM_MIN_BY ? cmp < 0 : cmp > 0) { best = src->data[i]; bestk = k; }
      }
      return best;
    }
    case SP_PENUM_SORT_BY: {
      /* keys computed once per element, as CRuby does, then a stable insertion
         sort carrying keys and elements together. This is a runtime array
         reached through a dispatch, not a hot path. */
      sp_PolyArray *keys = sp_PolyArray_new(); SP_GC_ROOT(keys);
      sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal k = sp_penum_call1(blk, src->data[i]);
        sp_int pos = out->len;
        while (pos > 0) {
          sp_bool ok = FALSE;
          sp_int cmp = sp_poly_cmp(keys->data[pos - 1], k, &ok);
          if (!ok) sp_poly_cmp_fail(keys->data[pos - 1], k);
          if (cmp <= 0) break;   /* <= keeps equal keys in input order */
          pos--;
        }
        sp_PolyArray_insert(keys, pos, k);
        sp_PolyArray_insert(out, pos, src->data[i]);
      }
      return sp_box_poly_array(out);
    }
  }
  return sp_box_nil();
}
/* <proc>.call(*arr): spread a runtime array into the sp_int[16] / boxed
   side-channel ABI. Each element rides the side-channel (a poly parameter reads
   it there) and its unboxed projection fills the sp_int slot (a concrete-typed
   parameter reads that -- a pointer for a heap value, the int otherwise). The
   proc publishes its result through _sp_proc_poly_ret. #2691 */
/* ---- Signal (#2735-#2738, #2749, #2750) --------------------------------
   The name<->number table mirrors CRuby's Signal.list (canonical name first
   where numbers alias, so signame answers CHLD/ABRT). Trap state lives in
   per-signal slots; a proc handler installs a real C handler that invokes the
   stored proc with the signal number. The EXIT pseudo-signal (0) registers an
   atexit dispatcher so the handler runs on normal termination too. */
/* Marker-framed names: they land in sp_StrIntHash keys (Signal.list) and
   boxed-string slots (Signal.signame, the trap return), whose runtimes read
   the [-1] marker byte -- a bare literal is an out-of-bounds read. */
#define SP_SIGN(n) (&("\xff" n)[1])
static const struct { const char *name; int no; } sp_sig_table[] = {
  {SP_SIGN("EXIT"), 0}, {SP_SIGN("HUP"), SIGHUP}, {SP_SIGN("INT"), SIGINT}, {SP_SIGN("QUIT"), SIGQUIT},
  {SP_SIGN("ILL"), SIGILL}, {SP_SIGN("TRAP"), SIGTRAP}, {SP_SIGN("ABRT"), SIGABRT}, {SP_SIGN("IOT"), SIGABRT},
  {SP_SIGN("BUS"), SIGBUS}, {SP_SIGN("FPE"), SIGFPE}, {SP_SIGN("KILL"), SIGKILL}, {SP_SIGN("USR1"), SIGUSR1},
  {SP_SIGN("SEGV"), SIGSEGV}, {SP_SIGN("USR2"), SIGUSR2}, {SP_SIGN("PIPE"), SIGPIPE}, {SP_SIGN("ALRM"), SIGALRM},
  {SP_SIGN("TERM"), SIGTERM}, {SP_SIGN("CHLD"), SIGCHLD}, {SP_SIGN("CLD"), SIGCHLD}, {SP_SIGN("CONT"), SIGCONT},
  {SP_SIGN("STOP"), SIGSTOP}, {SP_SIGN("TSTP"), SIGTSTP}, {SP_SIGN("TTIN"), SIGTTIN}, {SP_SIGN("TTOU"), SIGTTOU},
  {SP_SIGN("URG"), SIGURG}, {SP_SIGN("XCPU"), SIGXCPU}, {SP_SIGN("XFSZ"), SIGXFSZ}, {SP_SIGN("VTALRM"), SIGVTALRM},
  {SP_SIGN("PROF"), SIGPROF}, {SP_SIGN("WINCH"), SIGWINCH}, {SP_SIGN("IO"), SIGIO}, {SP_SIGN("SYS"), SIGSYS},
#ifdef SIGPWR
  {SP_SIGN("PWR"), SIGPWR},
#endif
#ifdef SIGSTKFLT
  {SP_SIGN("STKFLT"), SIGSTKFLT},
#endif
#ifdef SIGPOLL
  {SP_SIGN("POLL"), SIGPOLL},
#endif
  {NULL, 0}
};
static sp_StrIntHash *sp_signal_list(void) {
  sp_StrIntHash *h = sp_StrIntHash_new();
  SP_GC_ROOT(h);
  for (int i = 0; sp_sig_table[i].name; i++)
    sp_StrIntHash_set(h, sp_sig_table[i].name, (sp_int)sp_sig_table[i].no);
  return h;
}
#ifdef SPINEL_EXT_HOST
const char *sp_signal_signame(sp_int no);
#else
const char *sp_signal_signame(sp_int no) {
  for (int i = 0; sp_sig_table[i].name; i++)
    if (sp_sig_table[i].no == (int)no) return sp_sig_table[i].name;
  return NULL;   /* nil for an unknown number, as in CRuby 3.4+ */
}
#endif
/* Resolve a signal designator (String/Symbol name with optional SIG prefix,
   or Integer) to its number; CRuby's errors for the invalid forms. */
#ifdef SPINEL_EXT_HOST
SP_COLD int sp_signal_resolve(sp_RbVal sig);
#else
SP_COLD int sp_signal_resolve(sp_RbVal sig) {
  const char *nm = NULL;
  if (sig.tag == SP_TAG_STR) nm = sig.v.s;
  else if (sig.tag == SP_TAG_SYM) nm = sp_sym_to_s((sp_sym)sig.v.i);
  else if (sig.tag == SP_TAG_INT) {
    if (sp_signal_signame(sig.v.i)) return (int)sig.v.i;
    sp_raise_cls("ArgumentError",
                 sp_sprintf("invalid signal number (%lld)", (long long)sig.v.i));
  }
  if (!nm)
    sp_raise_cls("ArgumentError", "bad signal type");
  if (strncmp(nm, "SIG", 3) == 0) nm += 3;
  for (int i = 0; sp_sig_table[i].name; i++)
    if (strcmp(sp_sig_table[i].name, nm) == 0) return sp_sig_table[i].no;
  sp_raise_cls("ArgumentError", sp_sprintf("unsupported signal 'SIG%s'", nm));
}
#endif
void sp_sig_c_handler(int no);
void sp_sig_exit_dispatch(void);
sp_RbVal sp_signal_trap(sp_RbVal sig, sp_RbVal handler);
/* Process.kill: send `sig` to one pid; raises the errno family on failure and
   counts 1 on success (the emitter sums per-pid calls). Signal 0 probes. */
sp_int sp_process_kill1(sp_RbVal sig, sp_int pid);
/* SignalException.new(sig) / Interrupt.new(msg?): the message is the SIG-name
   and #signo rides the xkey slot (#2762, #2763). */

/* ENV.delete_if/keep_if/select!/reject!/filter!: the proc judges each
   snapshot pair; `keep` selects which verdict survives (#2832). */
static sp_int sp_env_filter_core(sp_Proc *p, int keep) { SP_GC_ROOT(p);
  sp_StrStrHash *snap = sp_env_to_h();
  SP_GC_ROOT(snap);
  sp_int removed = 0;
  for (sp_int i = 0; i < snap->len; i++) {
    const char *k = snap->order[i];
    const char *v = sp_StrStrHash_get(snap, k);
    _sp_proc_poly_args[0] = sp_box_str(k);
    _sp_proc_poly_args[1] = sp_box_str(v ? v : "");
    sp_int slots[16] = { (sp_int)(uintptr_t)k, (sp_int)(uintptr_t)(v ? v : "") };
    /* a bool/poly-returning proc publishes through the boxed channel and
       returns raw 0; pre-clear it so a typed proc's raw return still reads */
    _sp_proc_poly_ret = sp_box_nil();
    sp_int r = sp_proc_call(p, 2, slots);
    int truthy = (_sp_proc_poly_ret.tag != SP_TAG_NIL)
                   ? sp_poly_truthy(_sp_proc_poly_ret) : (r != 0);
    if ((truthy != 0) != (keep != 0)) { unsetenv(k); removed++; }
  }
  return removed;
}
static sp_StrStrHash *sp_env_filter_bang(sp_Proc *p, int keep) {
  sp_env_filter_core(p, keep);
  return sp_env_to_h();
}
/* ENV.update/merge!(hash) { |key, old, new| } -- the block resolves a key that
   is already set; its (stringified) result becomes the value (#2998). */
static sp_StrStrHash *sp_env_update_h_blk(sp_StrStrHash *h, sp_Proc *p) { SP_GC_ROOT(p);
  if (h) {
    SP_GC_ROOT(h);
    for (sp_int i = 0; i < h->len; i++) {
      const char *k = h->order[i];
      const char *nv = sp_StrStrHash_get(h, k);
      const char *ov = getenv(k);
      if (ov && p) {
        const char *ovh = sp_str_dup_external(ov);  /* environ may move */
        SP_GC_ROOT(ovh);
        _sp_proc_poly_args[0] = sp_box_str(k);
        _sp_proc_poly_args[1] = sp_box_str(ovh);
        _sp_proc_poly_args[2] = sp_box_str(nv ? nv : "");
        sp_int slots[16] = { (sp_int)(uintptr_t)k, (sp_int)(uintptr_t)ovh,
                              (sp_int)(uintptr_t)(nv ? nv : "") };
        _sp_proc_poly_ret = sp_box_nil();
        sp_int r = sp_proc_call(p, 3, slots);
        const char *rv = (_sp_proc_poly_ret.tag != SP_TAG_NIL)
                           ? sp_poly_to_s(_sp_proc_poly_ret) : (const char *)(uintptr_t)r;
        if (rv) setenv(k, rv, 1); else unsetenv(k);
      }
      else if (nv) setenv(k, nv, 1);
      else unsetenv(k);
    }
  }
  return sp_env_to_h();
}
/* reject!/select!/filter!: nil when nothing changed (#2844) */
static sp_RbVal sp_env_filter_bang_opt(sp_Proc *p, int keep) {
  if (sp_env_filter_core(p, keep) == 0) return sp_box_nil();
  return sp_box_obj(sp_env_to_h(), SP_BUILTIN_STR_STR_HASH);
}
static void sp_proc_call_spread(sp_Proc *p, sp_RbVal arr) { SP_GC_ROOT(p);
  if (!p || !p->fn) return;
  sp_int n = sp_poly_length(arr);
  if (n > 16) n = 16;
  sp_int slots[16];
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal e = sp_poly_arr_get(arr, i);
    _sp_proc_poly_args[i] = e;
    slots[i] = (e.tag == SP_TAG_OBJ || e.tag == SP_TAG_STR)
             ? (sp_int)(uintptr_t)e.v.p : sp_poly_to_i(e);
  }
  sp_proc_call(p, n, slots);
}
/* Enumerator#size (CRuby's ary2sv-independent size protocol): a materialized
   enumerator reports its snapshot length; a generator reports its stored size --
   calling it (no args) when it is a Proc and publishing through the boxed-return
   channel, else returning the stored value (nil when none was supplied). */
sp_RbVal sp_Enumerator_size(sp_Enumerator *e);

/* Proc#<< / Proc#>> composition. The composed proc captures the two
   operands and, on call, threads its single argument through inner
   then outer: `(f << g).call(x)` == f(g(x)). For `>>` the codegen
   swaps the operands so `(f >> g).call(x)` == g(f(x)). */
typedef struct { sp_Proc *outer; sp_Proc *inner; } sp_ProcCompose;
/* Also marks the capture itself: sp_Proc_scan calls a proc's cap_scan on the
   capture WITHOUT marking it first, so a cap_scan that only walks the fields
   leaves the capture to be swept out from under the proc that holds it. Every
   other cap_scan (generated closures, bound methods, hash procs) opens with
   the same mark; when this one runs as the capture's OWN scan hook the mark
   is already set and the call returns at once. */
static void sp_proc_compose_scan(void *p) { sp_gc_mark(p); sp_ProcCompose *c = (sp_ProcCompose *)p; if (c->outer) sp_gc_mark(c->outer); if (c->inner) sp_gc_mark(c->inner); }
static sp_int sp_proc_compose_fn(void *cap, sp_int argc, sp_int *args) {
  sp_ProcCompose *c = (sp_ProcCompose *)cap;
  /* The composed proc is usually anonymous at its call site -- `(f >> g).call`
     holds it in no variable -- so nothing keeps the capture alive while this
     body runs, and a collection triggered by the inner proc's own allocations
     freed it before c->outer was read. Rooting the capture marks outer and
     inner through its scan hook. */
  SP_GC_ROOT(c);
  /* CRuby enforces the FIRST-CALLED function's arity on the composed call
     (`(f << g).call(x)` runs g first, so g's arity governs) -- for a LAMBDA;
     a plain proc adjusts, as everywhere else. */
  if (c->inner && c->inner->lambda_p && c->inner->arity >= 0 && argc != c->inner->arity)
    sp_raise_cls("ArgumentError", sp_sprintf("wrong number of arguments (given %lld, expected %lld)",
                                             (long long)argc, (long long)c->inner->arity));
  sp_int inner_args[16] = {0};
  sp_int inner_argc = argc > 16 ? 16 : argc;
  for (sp_int _i = 0; _i < inner_argc; _i++) inner_args[_i] = args ? args[_i] : 0;
  /* the caller already published the boxed argument(s) to the side-channel, so
     the inner proc reads them back regardless of its parameters' static types */
  sp_proc_call(c->inner, inner_argc, inner_args);
  /* the inner proc publishes its (boxed) result through the return slot;
     thread it to the outer proc on BOTH channels -- a poly parameter reads
     the side-channel, a concrete one reads the sp_int slot. */
  sp_RbVal mid = _sp_proc_poly_ret;
  /* the outer function always receives the single threaded value; CRuby
     raises when a lambda's arity disagrees (`(f >> g)` reaching a 2-ary g). */
  if (c->outer && c->outer->lambda_p && c->outer->arity >= 0 && c->outer->arity != 1)
    sp_raise_cls("ArgumentError", sp_sprintf("wrong number of arguments (given 1, expected %lld)",
                                             (long long)c->outer->arity));
  sp_int outer_args[16] = {0};
  /* Thread the intermediate on the sp_int slot too: a concrete-typed outer
     parameter reads it there, so a heap value (string/array/object) must pass
     as its pointer, not its truncated int projection (#2650). */
  outer_args[0] = (mid.tag == SP_TAG_OBJ || mid.tag == SP_TAG_STR)
                ? (sp_int)(uintptr_t)mid.v.p : sp_poly_to_i(mid);
  _sp_proc_poly_args[0] = mid;
  /* the outer proc publishes the composed result into the slot; our own raw
     return is unread (the call site reads the slot). */
  return sp_proc_call(c->outer, 1, outer_args);
}
static sp_Proc *sp_proc_compose(sp_Proc *outer, sp_Proc *inner) { SP_GC_ROOT(outer); SP_GC_ROOT(inner);
  /* Both operands are usually freshly built at the call site -- `f >> g` emits
     sp_proc_compose(sp_proc_new_meta(...), sp_proc_new_meta(...)) -- and C does
     not order those two, so whichever runs first is held by nothing while the
     second allocates. Rooting the parameters at entry is too late for that: the
     collection happens before the call. Publish them into the capture FIRST,
     with the capture itself rooted, so the allocation in sp_proc_new_meta below
     has something to find them through. */
  SP_GC_ROOT(outer);
  SP_GC_ROOT(inner);
  sp_ProcCompose *c = (sp_ProcCompose *)sp_gc_alloc(sizeof(sp_ProcCompose), NULL, sp_proc_compose_scan);
  SP_GC_ROOT(c);
  c->outer = outer;
  c->inner = inner;
  /* CRuby (4.0): the composed proc's lambda? follows the FIRST-CALLED proc
     (the receiver for >>, the argument for <<) -- our inner; arity is -1
     regardless (#3051). */
  return sp_proc_new_meta((void *)sp_proc_compose_fn, c, sp_proc_compose_scan,
                          -1, inner ? inner->lambda_p : FALSE, 1, NULL, NULL);
}
static void *sp_proc_compose_v(void *outer, void *inner) {
  return (void *)sp_proc_compose((sp_Proc *)outer, (sp_Proc *)inner);
}
/* Proc#curry: an immutable argument accumulator over an sp_Proc target.
   `proc.curry` makes an empty accumulator; each `[arg]` returns a fresh
   accumulator with `arg` appended; the fully-applied value is realized
   by calling the target with the collected (sp_int) arguments. Spinel
   defers the call to the point of use (sp_curry_to_int), so a partial
   curry behaves as a deferred call rather than auto-invoking at arity. */
/* The int slots feed a target with scalar-int params, which reads them
   directly rather than from the boxed side-channel. */
static void sp_curry_int_slots(sp_Curry *c, sp_int *slots) {
  for (sp_int i = 0; i < c->nargs && i < 16; i++) slots[i] = sp_poly_slot_i(c->args[i]);
}
static sp_int sp_curry_to_int(sp_Curry *c) {
  if (!c || !c->target) return 0;
  SP_GC_ROOT(c);  /* c->args is read during the call; the target can allocate */
  sp_int slots[16];
  sp_curry_int_slots(c, slots);
  sp_curry_publish_args(c);
  /* the target publishes its (boxed) result through the return slot */
  sp_proc_call(c->target, c->nargs, slots);
  return sp_poly_to_i(_sp_proc_poly_ret);
}
/* Realization for a target whose return is not statically int: the boxed
   result flows through unchanged. */
static sp_RbVal sp_curry_realize_poly(sp_Curry *c) {
  if (!c || !c->target) return sp_box_nil();
  SP_GC_ROOT(c);
  sp_int slots[16];
  sp_curry_int_slots(c, slots);
  sp_curry_publish_args(c);
  sp_proc_call(c->target, c->nargs, slots);
  return _sp_proc_poly_ret;
}

/* `curry.call(a, ...)` where the base proc's arity is NOT statically known --
   the curry arrived through a parameter, a container, an untyped slot. Whether
   this application saturates the curry is then a run-time property of the
   accumulator, so decide it here: apply each argument, realize when the count
   reaches the target's arity, and answer the new curry boxed otherwise. The
   static paths kept answering a Curry for a saturating call, so a method taking
   a curried Proc returned a Proc where CRuby returns the value (#4068). */
/* Proc#curry with a count that arrives BOXED (an untyped slot, a container
   read): nil is no count, everything else converts through the Integer
   argument protocol -- CRuby's to_int, the user-object bridge and its exact
   TypeErrors included. */
static sp_Curry *sp_curry_new_v(sp_Proc *p, sp_RbVal n, sp_int max) {
  if (n.tag == SP_TAG_NIL || (n.tag == SP_TAG_INT && n.v.i == SP_INT_NIL))
    return sp_curry_new(p);
  return sp_curry_new_n(p, sp_poly_arg_int_chk(n), max);
}
static sp_RbVal sp_curry_call_poly(sp_Curry *c, sp_int argc, const sp_RbVal *args) {
  if (!c) return sp_box_nil();
  SP_GC_ROOT(c);
  for (sp_int i = 0; i < argc; i++) c = sp_curry_apply(c, args[i]);
  /* the accumulator carries its own completion count -- curry(n)'s n, else
     the target's required count, stamped at creation */
  if (c->nargs >= c->arity) return sp_curry_realize_poly(c);
  return sp_box_obj((void *)c, SP_BUILTIN_CURRY);
}

/* Proc#>> / #<< over a curried Proc: composition threads sp_Proc values, and a
   curry is an argument accumulator rather than one. Wrap it in a Proc whose
   trampoline applies the call's arguments and realizes the result, so a curry
   composes exactly like the proc it stands for (#3864). */
static sp_int sp_curry_proc_fn(void *cap, sp_int argc, sp_int *args) {
  sp_Curry *cy = (sp_Curry *)cap;
  (void)args;
  /* apply-and-decide, like every other application path: a call that does
     not reach the accumulator's count answers the next curry, not the
     target called short */
  _sp_proc_poly_ret = sp_curry_call_poly(cy, argc < 16 ? argc : 16, _sp_proc_poly_args);
  return sp_poly_to_i(_sp_proc_poly_ret);
}
/* sp_Proc_scan calls cap_scan WITHOUT marking the capture first, so this hook
   marks the curry itself before walking it (see sp_proc_compose_scan). */
static void sp_curry_proc_scan(void *p) { sp_gc_mark(p); sp_curry_scan(p); }
static sp_Proc *sp_curry_to_proc(sp_Curry *cy) {
  SP_GC_ROOT(cy);
  return sp_proc_new_meta((void *)sp_curry_proc_fn, cy, sp_curry_proc_scan, -1,
                          (cy && cy->target) ? cy->target->lambda_p : TRUE, 1, NULL, NULL);
}

/* Call a boxed callable. A Proc runs; a curried Proc (which a poly slot now
   carries, #3885) takes the arguments and realizes once it has them, the way
   the typed `curry[x]` path does. */
static sp_RbVal sp_poly_callable_call(sp_RbVal v, sp_int n, const sp_int *args) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_CURRY)
    return sp_curry_call_poly((sp_Curry *)v.v.p, n < 16 ? n : 16, _sp_proc_poly_args);
  /* anything else that is not a Proc is CRuby's NoMethodError -- the raw
     cast below read a boxed Integer as a proc pointer and crashed (a
     realized curry applied once more used to reach it) */
  if (v.tag != SP_TAG_OBJ || !v.v.p || v.cls_id != SP_BUILTIN_PROC)
    sp_raise_cls("NoMethodError", sp_nomethod_msg("call", v));
  sp_int slots[16];
  for (sp_int i = 0; i < n && i < 16; i++) slots[i] = args[i];
  sp_proc_call((sp_Proc *)v.v.p, n, slots);
  return _sp_proc_poly_ret;
}

/* Hash#to_proc cap-scan: the proc's `cap` field IS the source hash
   (a single GC pointer), so marking it keeps the hash alive for the
   proc's lifetime. The per-variant lookup fn is emitted by codegen
   alongside the hash type it closes over. */
static void sp_hashproc_cap_scan(void *p) { sp_gc_mark(p); }

/* Random instance methods + Kernel#srand moved to lib/sp_random.c
   (see sp_random.h for the sp_Random type and prototypes). */

/* StringIO is a native-bound spin package (packages/stringio). */

/* (the unused sp_Val lambda-closure runtime was removed: it was dead code   and its `sp_Val` typedef collided with a user class named Val -- issue #1774) */


/* Bigint (linked from sp_bigint.o) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_gcd(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_lcm(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_remainder(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_powmod(sp_Bigint *base, sp_int exp, sp_Bigint *mod);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
sp_Bigint *sp_bigint_round_prec(sp_Bigint *b, int64_t ndigits, int mode);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);

/* Bitwise ops on bigint operands. Used by --int-overflow=promote
   mode where all int slots widen to sp_Bigint *. Implemented via
   int64 round-trip: bigint values produced by promotion are
   almost always derived from small ints (counters, masks, bit-
   width-sized values <= 64 bits), so the int64 path preserves
   the full Ruby-side semantics for any value that fits. Values
   exceeding int64 lose precision through these helpers -- those
   are extremely rare in practice (Ruby bitops on integers >
   2^63), and proper mpz_and / mpz_or / mpz_xor support can be
   added later in lib/sp_bigint.c if a real workload needs it. */
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);


/* Bigint (linked from libspinel_rt.a) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
/* Bignum == Float, exactly. A double carrying an integral value is turned
   into the bignum it names -- "%.0f" prints every finite double's integral
   value exactly -- so `1.0e100 == 10 ** 100` answers false the way CRuby's
   own exact comparison does, rather than true from a lossy double round-trip.
   Ordering does compare as doubles (sp_poly_cmp does the same); only equality
   can be decided by a single ulp. */
static int sp_bigint_eq_f(sp_Bigint *a, double d) {
  if (!isfinite(d) || d != floor(d)) return 0;   /* inf/nan/fraction is no integer */
  char buf[400];
  snprintf(buf, sizeof buf, "%.0f", d);
  sp_Bigint *bd = sp_bigint_new_str(buf, 10);
  return bd && a && sp_bigint_cmp(a, bd) == 0;
}
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_gcd(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_lcm(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_remainder(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_powmod(sp_Bigint *base, sp_int exp, sp_Bigint *mod);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
sp_Bigint *sp_bigint_round_prec(sp_Bigint *b, int64_t ndigits, int mode);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);

/* ---- Pack / Unpack (linked from sp_pack.o) ----
   Implementation lives in libspinel_rt.a; the entry points
   below call into the static GC helpers in this header via the
   sp_ext_* shims defined further down. */
const char *sp_IntArray_pack(sp_IntArray *arr, const char *fmt);
const char *sp_FloatArray_pack(sp_FloatArray *arr, const char *fmt);
const char *sp_PolyArray_pack(sp_PolyArray *arr, const char *fmt);
const char *sp_StrArray_pack(sp_StrArray *arr, const char *fmt);
sp_PolyArray *sp_str_unpack(const char *str, const char *fmt);
sp_PolyArray *sp_str_unpack_off(const char *str, const char *fmt, sp_int byteoff);

/* Array#pack on a poly (nullable-array) receiver: dispatch on the runtime tag.
   A nil/non-array recv packs to the empty string. */
static inline const char *sp_poly_pack(sp_RbVal recv, const char *fmt) {
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_INT_ARRAY)
    return sp_IntArray_pack((sp_IntArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_FLT_ARRAY)
    return sp_FloatArray_pack((sp_FloatArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_POLY_ARRAY)
    return sp_PolyArray_pack((sp_PolyArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_STR_ARRAY)
    return sp_StrArray_pack((sp_StrArray *)recv.v.p, fmt);
  /* Marked, like every other string this can return: a bare "" literal
     has no 0xff marker byte, and a caller that roots the result would
     have the collector mark a rodata pointer. */
  return sp_str_empty;
}

/* StringScanner is a native-bound spin package (packages/strscan). */

/* The sp_ext_* shim wrappers are gone: string/object allocation, sp_box_*, and
   sp_PolyArray now live in the shared headers (sp_alloc.h / sp_gc.h), so lib C
   files (sp_pack.c, sp_strscan.c, sp_marshal.c, ...) allocate directly. */

/* A generated TU that contains no poly-renderable value defines
   SP_TU_NO_POLY_RENDER and omits the symbol runtime and the class-name table.
   The header's render helpers still reference sp_sym_to_s / sp_class_to_s
   syntactically (they are unreachable in such a program), and GCC warns on a
   referenced-but-undefined static even when the referring function is dead --
   so supply inert fallbacks that the optimizer prunes with everything else. */
#ifdef SP_TU_NO_POLY_RENDER
static const char *sp_sym_to_s(sp_sym id) { (void)id; return sp_str_empty; }
static const char *sp_class_to_s(sp_Class c) { return c.name ? c.name : sp_str_empty; }
static sp_sym sp_sym_intern(const char *s) { (void)s; return (sp_sym)0; }
#endif

#endif /* SP_RUNTIME_H */
