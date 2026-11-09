/* sp_types.h -- core value-type definitions split out of spinel_rt.h.
 *
 * Holds the primitive typedefs, the small leaf value structs, the GC
 * header, and the typed array / non-poly hash structs. Both
 * spinel_rt.h (which includes this near the top) and libspinel_rt.a
 * sources can include it to see the layouts without pulling in the
 * header's static/inline function bodies. Pure type/macro definitions
 * only (plus the sp_oint / sp_ofloat constructors, which are the types'
 * literals) -- no other function definitions, no global state.
 *
 * Reorganisation step toward slimming spinel_rt.h; sp_RbVal, the poly
 * containers, and the conditional Proc/Fiber/etc. types still live in
 * spinel_rt.h pending a later pass.
 */
#ifndef SP_TYPES_H
#define SP_TYPES_H

#ifdef __APPLE__
/* _DARWIN_C_SOURCE re-enables Darwin extensions (MAP_ANON, used by the fiber
   stack mmap in sp_fiber.c) that a strict POSIX build would otherwise hide.
   Define it here -- the lowest shared base header -- so every TU that pulls
   sp_types.h in sets it before the first system header.
   No _XOPEN_SOURCE / -Wdeprecated-declarations gate is needed: the portable
   asm context switch runs on every Darwin arch (x86_64 / arm64), so the only
   <ucontext.h> include (sp_fiber_ctx.h's non-asm fallback) is never compiled
   there, and PR #1563 removed the stale unconditional include from
   spinel_rt.h. */
#define _DARWIN_C_SOURCE
#endif

#include "sp_compat.h"   /* the GNU extensions behind macros; first, it sets them all */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <float.h>   /* DBL_MAX / DBL_MIN / DBL_EPSILON for Float::* constants */

#include "sp_setjmp.h"   /* the platform's setjmp/longjmp, or the abort stand-ins */

/* Per-worker storage under true parallelism. In the -DSP_THREADS runtime
   variant (and the generated TU when the program uses threads, compiled with
   the same define) the per-thread execution state -- the GC root stack, the
   current fiber, the match registers -- is thread-local so each OS worker keeps
   its own. The single-threaded build leaves these plain globals, byte-identical
   to before. Both sides must agree on the storage class, so SP_TLS lives in
   this shared base header. */
#ifdef SP_THREADS
# ifdef SP_NO_THREAD_LOCAL
#  error "spinel: the threaded runtime (SP_THREADS) needs thread-local storage"
# endif
# define SP_TLS SP_THREAD_LOCAL
#else
# define SP_TLS
#endif

/* Worker identity, shared by the allocators (per-worker string/object heaps) and
   the scheduler that owns it. In the base header so sp_gc.h's SP_GC_HEAP_PUSH --
   included ahead of sp_alloc.h -- can index the per-worker object heap. */
#ifdef SP_THREADS
# ifndef SP_MAX_WORKERS
#  define SP_MAX_WORKERS 256
# endif
extern SP_TLS int sp_worker_id;      /* this OS worker's slot (0 = main); sp_sched.c */
extern int sp_active_workers;        /* live worker count; sp_sched.c */
#endif

/* Branch-hint / hot-cold attributes (SP_LIKELY, SP_UNLIKELY, SP_COLD,
   SP_NOINLINE, SP_INLINE, SP_NORETURN) live in sp_compat.h. Static
   approximation of PGO: marking rare paths (raise, dispatch fallbacks) cold
   lets the C compiler split them out of the hot path's i-cache footprint. */

/* sp_int follows pointer width (decided at compile time via intptr_t):
   int64_t on 64-bit hosts -- PCs, no behavior change -- and int32_t on
   32-bit embedded targets, where it gives native-word arithmetic, half
   the memory for every integer/array/hash slot, and a pointer-width
   sp_RbVal union. The two paths differ only in that the 32-bit build
   overflows / narrows foreign 64-bit values (e.g. Time#to_i) at
   INT32 limits; see the overflow-mode handling. */
#if INTPTR_MAX == INT64_MAX
#elif INTPTR_MAX == INT32_MAX
#else
#error "spinel: unsupported intptr_t width (need 32- or 64-bit)"
#endif
typedef intptr_t sp_int;
typedef double sp_float;
typedef bool sp_bool;

/* CRuby's nil.object_id, false.object_id and true.object_id. */
#define SP_NIL_OBJECT_ID 4
#define SP_FALSE_OBJECT_ID 0
#define SP_TRUE_OBJECT_ID 20
/* A Symbol's object_id: its interned id * 16 + 12, low bits no other
   kind's id has (CRuby tags a static Symbol's likewise). Every one is
   even (no Integer's 2n+1), 4 mod 8 (no 8-aligned heap object's address,
   no false's 0) and 12 mod 16 (no nil's 4, no true's 20), and two
   Symbols' differ. The id is a non-negative index, so a 32-bit sp_int
   holds 2**27 Symbols' ids. */
#define SP_SYM_OBJECT_ID(id) ((sp_int)(id) * 16 + 12)

/* A nullable Integer or Float in flight (a local, a parameter, a return, a
   temp, a container read that can miss): the value and its nil live side by
   side. No bit pattern of sp_int or sp_float means nil -- -2**63 and every
   NaN payload are ordinary values in every --int-overflow mode. A slot that
   holds many of them (an Integer Array, an ivar set) keeps the nil beside
   the values as a bit (nilbits below) rather than widening each one.
   `nil` is 0 or 1; `v` is 0 / 0.0 when nil is set, so a zero-initialized
   pair is the Integer 0, not nil. */
typedef struct { sp_int   v; sp_bool nil; } sp_oint;
typedef struct { sp_float v; sp_bool nil; } sp_ofloat;
static inline sp_oint   sp_oint_of(sp_int v)     { sp_oint o = { v, 0 }; return o; }
static inline sp_oint   sp_oint_nil(void)        { sp_oint o = { 0, 1 }; return o; }
static inline sp_ofloat sp_ofloat_of(sp_float v) { sp_ofloat o = { v, 0 }; return o; }
static inline sp_ofloat sp_ofloat_nil(void)      { sp_ofloat o = { 0.0, 1 }; return o; }
/* The constant forms, for a file-scope initializer (a static ivar, a global). */
#define SP_OINT_OF(v)   { (v), 0 }
#define SP_OINT_NIL     { 0, 1 }
#define SP_OFLOAT_OF(v) { (v), 0 }
#define SP_OFLOAT_NIL   { 0.0, 1 }
/* The nil of an Integer out of a Float slot and back: the value converts, the
   nil carries. */
static inline sp_ofloat sp_oint_to_ofloat(sp_oint o) { sp_ofloat r = { o.nil ? 0.0 : (sp_float)o.v, o.nil }; return r; }

/* sp_sym is defined per-program in emit_sym_runtime, but poly helpers
   below need to reference it by forward declaration. */
typedef sp_int sp_sym;

#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif

/* ---- Leaf value structs ---- */
/* `step` carries the iteration direction/stride for the enumerator forms that a
   plain ascending range cannot express: n.downto(m) is {first:n,last:m,step:-1}.
   A zero step (every range built by the literal `a..b` / sp_range_new path) is
   treated as +1, so existing constructions need no change. */
/* An Integer begin with a Float end, (1..2.5): CRuby iterates it from the
   Integer, so first / last / excl are the bounds of that walk (1..2), and the
   end as written is kept beside them -- fe 1 for an inclusive Float end, 2
   for an excluded one, fend its value; fe 0 (every other range, and the
   zero-initialized one) has none. Read through sp_range_end_* (sp_range.h).
   All three Range kinds default to frozen; dup sets their unfrozen flag.
   Store it beside a byte-sized flag (fe 0..2, omitted 0..63, excl 0..1)
   in the existing trailing word, keeping the by-value layouts the same size. */
/* An Integer Range's open sides are the flags `nobeg` / `noend` (a beginless
   `..5`, an endless `1..`), not a pattern of the bound: a begin of exactly
   -2**63 is a begin. An open side still STORES the extreme bound
   (SP_RANGE_NO_BEGIN / SP_RANGE_NO_END) so a walk that clamps against the
   bounds reads the same number it always did; only the flag says whether
   the side is there. */
typedef struct{sp_int first;sp_int last;sp_int excl;sp_int step;sp_float fend;unsigned char fe;unsigned char unfrozen;unsigned char nobeg;unsigned char noend;}sp_Range;
#define SP_RANGE_NO_BEGIN ((sp_int)INTPTR_MIN)
#define SP_RANGE_NO_END   ((sp_int)INTPTR_MAX)
/* A Float range (1.0..3.0): endpoints kept as sp_float so cover?/include?/begin/
   end are exact (an int-backed sp_Range truncated them). Iteration is a TypeError
   in Ruby (only #step traverses a Float range), so no step/iteration state here.
   beginless/endless ends use -HUGE_VAL / +HUGE_VAL. */
/* `omitted` records which bound was WRITTEN as absent (`(..5.0)`, `(1.0..)`),
   as opposed to an explicit infinity (`1..Float::INFINITY`): both are +/-HUGE_VAL
   in the value, and only #inspect / #to_s tell them apart -- CRuby prints
   "1.0.." for the first and "1.0..Infinity" for the second (#3670).
   bit 1 = begin omitted, bit 2 = end omitted. */
typedef struct{sp_float first;sp_float last;sp_int excl;unsigned char omitted;unsigned char unfrozen;}sp_FloatRange;
#define SP_FRANGE_NO_BEGIN 1
#define SP_FRANGE_NO_END   2
/* A mixed literal (1.5..5) is a Float range whose END was written as an
   Integer -- and CRuby renders each endpoint as the user wrote it. The bits
   record that, so #inspect prints "1.5..5" rather than "1.5..5.0" (#3896). */
#define SP_FRANGE_INT_BEGIN 4
#define SP_FRANGE_INT_END   8
/* A String range ("a".."e"): endpoints kept as GC-managed strings so #begin,
   #end, #to_s and #inspect answer as Ruby does. Every traversal (each/to_a/
   include?/...) materializes the element array through
   sp_StrArray_from_string_range, which is how the range behaved before it was
   a value of its own. */
/* hf / hl (--share-strings, #8321): the shared handles (sp_String *) the
   endpoints were made from, or NULL. A Range keeps the String object, so a
   change in place to it shows through #begin; sp_srange_live reads each
   handle's bytes as they are now. sym: a Range of Symbols, its ends the
   Symbols' names; Symbol#succ is String#succ on the name, so the walk and
   the comparisons are the String Range's, and the readers that answer an
   end or a member make the Symbol of it (sp_srange_box_end). */
/* (one representation: the endpoints' handles typed, which the face macros read as the handle) */
typedef struct{const char *first;const char *last;unsigned char excl;unsigned char unfrozen;unsigned char sym;struct sp_String_s *hf;struct sp_String_s *hl;}sp_StrRange;
/* A class value. `name`, when non-NULL, is a rodata class name carried by a
   class whose cls_id table entry may not exist (an exception's class -- the
   Errno:: family and many builtin error classes have no assigned cls_id). It
   takes precedence over cls_id for to_s / boxing / equality. */
typedef struct{sp_int cls_id;const char *name;}sp_Class;
/* The nil-class sentinel: a TY_CLASS value can be nil (BasicObject#superclass),
   carried in-band via a reserved cls_id with no name (a spare pattern of the
   class-id space; Integer and Float have no spare pattern, hence sp_oint /
   sp_ofloat above). It is distinct from every real class id
   (user >= 0, builtins -100..-146, SP_CLASS_BY_NAME 0x7F000000). It MUST stay
   negative: the class-chain walks route a non-negative cls_id to the user-class
   table (`cur.cls_id>=0 ? sp_class_superclass : sp_builtin_superclass`), so a
   positive sentinel would be looked up as a user class. */
#define SP_CLASS_NIL_ID ((sp_int)-900)
#define sp_class_nil_p(c) ((c).name == NULL && (c).cls_id == SP_CLASS_NIL_ID)
#define SP_CLASS_NIL ((sp_Class){SP_CLASS_NIL_ID, NULL})
/* The singleton class of class id `id` (`Foo.singleton_class`, self in a
   `class << Foo` body): the id moved past every real one, a user class's up
   by SP_SCLASS_OFF and a builtin's down by it, so it stays clear of
   SP_CLASS_NIL_ID and SP_CLASS_BY_NAME and fits a 32-bit sp_int. */
#define SP_SCLASS_OFF ((sp_int)1 << 20)
#define sp_sclass_id(id) ((id) >= 0 ? (id) + SP_SCLASS_OFF : (id) - SP_SCLASS_OFF)
#define sp_sclass_id_p(id) (((id) >= SP_SCLASS_OFF && (id) < 2 * SP_SCLASS_OFF) || \
                            ((id) <= -SP_SCLASS_OFF && (id) > -2 * SP_SCLASS_OFF))
#define sp_sclass_base_id(id) ((id) >= 0 ? (id) - SP_SCLASS_OFF : (id) + SP_SCLASS_OFF)
#define sp_class_sclass_p(c) ((c).name == NULL && sp_sclass_id_p((c).cls_id))
/* A class known only by name (see sp_box_class_name in sp_alloc.h): marks a
   boxed SP_TAG_CLASS value whose v.s carries the name instead of a resolved
   cls_id. */
#define SP_CLASS_BY_NAME 0x7F000000
/* fl marks a component as Float-classed (renders "2.0"); clear bits keep the
   Integer-style rendering for whole values. Zero-init (every positional
   compound literal) is the Integer-classed default. */
typedef struct{sp_float re;sp_float im;unsigned char fl;}sp_Complex;
#define SP_CPLX_RE_F 1
#define SP_CPLX_IM_F 2
typedef struct{sp_int num;sp_int den;}sp_Rational;
typedef struct{const char *name;}sp_Encoding;

/* ---- GC headers ---- */
/* `marked` is a mark-generation STAMP, not a boolean: an object is live in
   the current collection iff marked == sp_gc_mark_gen. Bumping the generation
   at collect entry unmarks the whole heap in O(1), removing the two
   full-list walks (old unmark + minor re-mark) that made every minor
   collection O(live) even when the live set was untouched. 0 never equals a
   generation (the counter skips it), so freshly-calloc'd headers are unmarked. */
typedef struct sp_gc_hdr { struct sp_gc_hdr *next; void (*finalize)(void *); void (*scan)(void *); size_t size; unsigned marked : 27; unsigned frozen : 1; unsigned pinned : 1;
   /* `old` says the object has survived a sweep and lives on the old list, so
      a reference stored into it may point at a younger object a minor mark
      would not reach on its own; `dirty` says it is already on the remembered
      set, so the write barrier's push happens once per collection rather than
      once per store. `marked` gives up a bit for them: it is a wrapping
      generation counter whose wrap path re-clears every stamp, and 2^27
      collections between wraps is far past anything real.

      `pinned` says the object is on sp_gc_pinned, the STICKY half of the
      remembered set. `dirty` records a store the barrier saw and is cleared
      every cycle; `pinned` records a holder whose stores the barrier cannot
      see at all, so it is never cleared while the object lives. The one
      producer is a by-reference String parameter: the callee stores through a
      cell it cannot name the owner of, and the owner is only nameable at the
      lending call site, which is BEFORE the store rather than after (#4391).

      `aged` says the object has already survived one minor cycle while young.
      A survivor is promoted on its SECOND survival, not its first: an object
      that dies within a cycle or two of its birth then dies young, where the
      next minor frees it, instead of being promoted to sit in the old
      generation until a full cycle -- promoted garbage was a third of an
      interpreter's heap and a third of its wall. */
   unsigned old : 1; unsigned dirty : 1; unsigned aged : 1; void (*recycle)(struct sp_gc_hdr *); } sp_gc_hdr;
/* size/len packed to uint32 (4 GB per-string cap, far beyond any real
   string) so the cached FNV `hash` fits without growing the 24-byte
   header -- i.e. zero per-string RSS cost vs the pre-cache layout.
   `size` is vestigial (written by sp_str_alloc, never read back; the
   str-heap sweep no longer folds string bytes into sp_gc_bytes).
   `hash` caches sp_str_hash for heap/heap-frozen keys; 0 == not yet
   computed, invalidated to 0 on any in-place mutation (setbyte/set_len). */
typedef struct sp_str_hdr { struct sp_str_hdr *next; uint32_t size; uint32_t len; uint64_t hash; } sp_str_hdr;
/* One String representation, U2 (sp_str_alloc_born): a heap String's link
   to the handle it was born in, bit 1 set when the runtime keeps the String
   (sp_str_retained) */
#define SP_STR_BORN_KEPT ((uintptr_t)2)
#define SP_STR_BORN_LINK(h) ((void *)((uintptr_t)(h)->next & ~SP_STR_BORN_KEPT))

/* ---- Typed arrays ---- */
#define SP_STRARR_INLINE 4
/* nilbits: which elements are nil, one bit per PHYSICAL slot of `data` (bit
   start+i for element i of an IntArray, bit i of a FloatArray), NULL until
   the first nil is stored. When non-NULL it spans `cap` bits (the slot
   allocators resize it with `data`), and every bit outside the live window
   [start, start+len) is clear, so a push needs no bitmap write and a plain
   store into a slot that was never nil needs none either. Set where a nil
   lands -- a gap filled past the end, Array.new(n), a converted boxed nil, an
   element a builtin copies -- and carried to copies. A whole-array read that
   must know (sum, min, max, sort, include?(nil), the boxing into a mixed
   array) asks the pointer first: an array that never held a nil pays one
   NULL test. compact!, delete(nil), clear and replace drop it. It follows
   `frozen`, which the push / [] / []= paths read, in the same word the
   may_nil flag used, so the IntArray block stays in its slab class. */
/* frozen is a flag (32 bits); nil_lo shares its word: the lowest physical
   slot that can hold a nil while nilbits is set (SP_NILBITS_NEW_A, sp_array.h) */
typedef struct{sp_int*data;sp_int start;sp_int len;sp_int cap;int32_t frozen;uint32_t nil_lo;uint64_t*nilbits;}sp_IntArray;
/* A shift on a Poly, Ptr, Str or Float array steps `data` past the first
   element instead of moving the rest down, and counts the slots it stepped
   past in `head` (sp_PtrArray_shift, sp_array.h). The element buffer then
   starts at data - head and holds cap + head slots. Every site that frees,
   reallocs or reuses the buffer takes its start and size from SP_ARR_BUF /
   SP_ARR_BUF_CAP, never from data and cap; SP_ARR_REBASE moves the elements
   back to the start, which gives the slots stepped past back to cap. A new
   array comes zeroed, so its head is 0. */
#define SP_ARR_BUF(a)     ((a)->data - (a)->head)
#define SP_ARR_BUF_CAP(a) ((a)->cap + (a)->head)
#define SP_ARR_REBASE(a)  do { memmove(SP_ARR_BUF(a), (a)->data, sizeof *(a)->data * (size_t)(a)->len); \
                               (a)->data = SP_ARR_BUF(a); (a)->cap = SP_ARR_BUF_CAP(a); (a)->head = 0; } while (0)
/* FloatArray's head (SP_ARR_BUF): its word is full, and a FloatArray block
   is in the same slab class at 48 bytes as at 40. nilbits count from data,
   not from the buffer. */
typedef struct{sp_float*data;sp_int len;sp_int cap;int32_t frozen;uint32_t nil_lo;uint64_t*nilbits;int32_t head;}sp_FloatArray;
_Static_assert(sizeof(void *) != 8 || (sizeof(sp_IntArray) == 48 && sizeof(sp_FloatArray) == 48),
               "the Int / Float array blocks keep their slab class");
/* A push reads `len` once, as one load of that field alone, and stores
   l + 1 without reading it back (sp_IntArray_push, sp_FloatArray_push,
   sp_PolyArray_push, sp_PtrArray_push, sp_StrArray_push_e). A compiler otherwise loads len together with the
   field beside it in one wide load -- clang on arm64 len and cap, gcc on
   x86 start and len -- right after the previous push stored len alone, and
   a load wider than a store still in flight to the same bytes waits for it
   to reach the cache: a loop that pushes ran about twice as long. The grow
   calls never change len, so l stays right across them. The volatile only
   keeps that one load narrow; it is not there for threads. */
#define SP_ARR_LEN_ONCE(a) (*(volatile sp_int *)&(a)->len)
/* the array may hold a nil (the fast whole-array test; exact bits are read
   per element through sp_IntArray_elem_nil / sp_FloatArray_elem_nil) */
#define SP_MAY_NIL(a) ((a)->nilbits != NULL)
#define sp_nilbits_words(cap) ((size_t)(((cap) + 63) >> 6))
#define sp_nilbit_get(bits, i) ((int)(((bits)[(size_t)(i) >> 6] >> ((i) & 63)) & 1u))
#define sp_nilbit_set(bits, i) ((bits)[(size_t)(i) >> 6] |= (uint64_t)1 << ((i) & 63))
#define sp_nilbit_clr(bits, i) ((bits)[(size_t)(i) >> 6] &= ~((uint64_t)1 << ((i) & 63)))
/* elem_kind/elem_cls: what the pointers are, stamped when the array is boxed by
   reference (the one cls_id for pointer arrays is type-erased, #4486). 0 until
   then; a stamp never changes since a typed array holds one kind. */
/* head: the slots a shift stepped past (SP_ARR_BUF, above); it shares
   frozen's word, as sp_PolyArray's does */
typedef struct{void**data;sp_int len;sp_int cap;void(*scan_elem)(void*);int32_t frozen;int32_t head;int elem_kind;int elem_cls;}sp_PtrArray;
#define SP_PTR_ELEM_UNKNOWN  0
#define SP_PTR_ELEM_OBJ      1   /* user objects, elem_cls = the static class (-1: read each object's own) */
#define SP_PTR_ELEM_INT_ROWS 2   /* sp_IntArray rows (Array[Array[Integer]]) */
#define SP_PTR_ELEM_FLT_ROWS 3   /* sp_FloatArray rows (Array[Array[Float]]) */
/* the element of a String Array (and a String Hash's value): the handle
   (SP_SEB spells the bytes face) */
/* the shared String handle (sp_string.h) */
struct sp_String_s { char *data; int64_t len; int64_t cap; unsigned binary; };
typedef struct sp_String_s *sp_strelem;
/* head: the slots a shift stepped past (SP_ARR_BUF, above) */
typedef struct{sp_strelem*data;sp_int len;sp_int cap;int32_t frozen;int32_t head;sp_strelem inline_data[SP_STRARR_INLINE];}sp_StrArray;
/* One representation (string-one-repr-design.md): an element
   of a String container -- an Array's, a Hash's value -- is the String handle,
   so a change through any name shows in the container. The element's faces:
     SP_SEB(e)  the bytes of element e (NULL for nil), what the byte-level
                builtins read; (e)->data for an element the caller has tested
                is not nil;
     SP_SEBOX(e) element e boxed (nil for NULL);
   and the other direction, spelled where it is used: sp_String_new_shared(b)
   for an element made of fresh bytes b a builtin produced,
   sp_String_key_handle(k) for the element a Hash key k (frozen bytes) stands
   for, sp_strelem_mark(e) to mark an element for the collector, and
   SP_GC_ROOT(e) to root it across an allocation. */
/* sp_String_new_shared and sp_String_key_handle are sp_string.h's: a file
   that uses either includes it (sp_str.h does) */
/* (each evaluates its operand once: `SP_SEBOX(sp_StrArray_pop_e(a))` popped
   twice and boxed the second, empty, answer) */
#define SP_SEB(e) ({ struct sp_String_s *_seb_e = (e); _seb_e ? _seb_e->data : (const char *)NULL; })
/* the same for a header ahead of sp_string.h (sp_string.c) */
struct sp_String_s *sp_strelem_of(const char *b);
#define SP_SEBOX(e) ({ struct sp_String_s *_sebox_e = (e); _sebox_e ? sp_box_obj((void *)_sebox_e, SP_BUILTIN_STRBUF) : sp_box_nil(); })

/* ---- Non-poly typed hashes ---- */
/* An Integer-valued hash's miss answers its default: `default_nil` set is a
   hash with no default ({} / {k=>v}), whose miss is nil; clear, default_v is
   Hash.new(N)'s N. Keys are never nil in a typed hash (a hash that stores
   one is poly). A nil VALUE is a bit in `vnil`, one per slot (the
   open-addressing index), NULL until the first nil value is stored; the
   slot's vals word is 0 then (DESIGN.md D3b-ii). */
typedef struct{const char**keys;sp_int*vals;const char**order;sp_int len;sp_int cap;sp_int mask;sp_int default_v;sp_bool default_nil;uint64_t*vnil;}sp_StrIntHash;
typedef struct{const char**keys;sp_strelem*vals;const char**order;sp_int len;sp_int cap;sp_int mask;sp_strelem default_v;}sp_StrStrHash;
typedef struct{sp_int*keys;sp_strelem*vals;sp_int*order;sp_bool*used;sp_int len;sp_int cap;sp_int mask;sp_strelem default_v;}sp_IntStrHash;
typedef struct{sp_int*keys;sp_int*vals;sp_int*order;sp_bool*used;sp_int len;sp_int cap;sp_int mask;sp_int default_v;sp_bool default_nil;uint64_t*vnil;}sp_IntIntHash;

/* Signal table bound (0..64): shared by the trap state in the generated TU
   and the trap machinery in lib/sp_cold.c. */
#define SP_SIG_MAX 65

/* The default SIGINT / SIGTERM (#9222) raises where it arrives only inside a
   blocking call the runtime marks with SP_SIG_BLOCKING (a read, a wait, a
   sleep: nothing of the runtime is half-updated there, and the program would
   otherwise wait on). Anywhere else the handler records the signal, and the
   compiler's safe points and the runtime's waits raise it. */
extern volatile int sp_sig_async_ok;
#define SP_SIG_BLOCKING(expr) \
  __extension__ ({ sp_sig_async_ok++; __typeof__(expr) _sp_sb_r = (expr); sp_sig_async_ok--; _sp_sb_r; })

#endif
