/* The Spinel type lattice, as a C enum.
 *
 * The legacy compiler represents types as string tags ("int", "string",
 * "poly", "int_array", ...). We port the closed core as an enum and will
 * grow parameterized containers (arrays/hashes) into a richer struct as
 * later milestones need them. For now scalars + the poly top suffice.
 */
#ifndef SPINEL_TYPES_H
#define SPINEL_TYPES_H

/* Fast equality for short strings. The iterated analysis/codegen passes compare
   huge numbers of short keys / type names / method names; glibc's strcmp
   resolves to __strcmp_avx2, whose per-call AVX setup dominates for strings
   this short (and is paid in full even when the first byte already differs). An
   inline byte loop is several times cheaper here -- strcmp self-time was ~47% of
   a profiled optcarrot compile. Returns 1 if equal. */
#ifdef SP_WORK_COUNT
extern unsigned long long g_nt_work;   /* see NT_WORK in node_table.h */
#endif
static inline int sp_streq(const char *a, const char *b) {
#ifdef SP_WORK_COUNT
  g_nt_work++;   /* a name compare is the unit of every by-name scan */
#endif
  for (;; a++, b++) {
    if (*a != *b) return 0;
    if (*a == 0) return 1;
  }
}
static inline unsigned sp_strhash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

/* ---- The boxed-receiver face table ----

   A call on a receiver typed TY_POLY has no emitter of its own for most of
   the builtin surface. What it has is a pretence: unbox the value to ONE
   concrete kind, retype the receiver node as that kind, and re-enter the
   typed emitter, which is then the implementation. This table says which
   kinds own which names, and both halves of the compiler read it -- the
   inference answers as the typed call would, under the same pin codegen
   installs, so the result slot and the emission agree by construction
   (#3449 introduced the shape for the Hash face; the String, Integer and
   Enumerable lists in the poly emitter grew separately and are folded in).

   A name several kinds own is dispatched on the receiver's run-time kind,
   one re-entry per owner (see the poly emitter); a name none owns falls
   through to whatever arm comes next, so this is not a catch-all and a name
   without a typed emitter still reports the missing method.

   A row marked PF_LAST answers only once no poly-receiver emitter of its own
   has claimed the name, because a claimed name's face answer is a type
   nothing renders (find and detect were such names -- their emitter walks
   the elements and answers one of them). The read-only Hash face is all
   PF_LAST; a Hash row ahead of it either reads alone (assoc) or carries
   PF_MUT and writes back, because the normalization copies every variant
   but the general one and a plain write would be lost. The table itself is
   in types.c. */
enum {
  PF_STRING = 1 << 0,  /* re-enter as TY_STRING (sp_poly_recv_s) */
  PF_ARRAY  = 1 << 1,  /* TY_POLY_ARRAY, an Array at run time only */
  PF_ENUM   = 1 << 2,  /* TY_POLY_ARRAY over any collection's elements (a hash's pairs) */
  PF_HASH   = 1 << 3,  /* TY_POLY_POLY_HASH */
  PF_INT    = 1 << 4,  /* TY_INT */
  PF_FLOAT  = 1 << 5,  /* TY_FLOAT: an owner beside Integer for the names both have (step) */
  PF_OWNERS = 0x3f,
  PF_MUT      = 1 << 8,  /* mutates the receiver: the result is written back through the box */
  PF_STR_BANG = 1 << 9,  /* String value-form bang: re-enter the plain name, nil when unchanged */
  PF_STR_SELF = 1 << 10, /* ... but a bang that answers self (succ!/next!): never nil */
  PF_ARGS_OWN = 1 << 11, /* the arguments must be of the owner's own kind (concat) */
  PF_VAL_SELF = 1 << 12, /* a mutator whose value is the receiver: the box itself, or for a String the box its variable holds after the write */
  PF_SAME_OK  = 1 << 14, /* ... and contents that are the receiver's own mean no write, so no frozen check (scrub!) */
  PF_LAST     = 1 << 13  /* answers only once no poly-receiver emitter of its own has claimed the name */
};
typedef struct {
  const char *name;
  unsigned short flags;
  signed char argc_min, argc_max;  /* argc_max -1: any count */
  signed char blk;                 /* 1 block required, 0 none, -1 either */
} PolyFace;
/* The owners of `name` at a call of `argc` arguments with or without a block:
   the OR of every matching row's flags (an owner bit per kind, plus the
   write-back flags of the rows that carry them), the last-resort rows
   included or not. A call whose arguments are not all positional (a splat,
   a block argument -- see nt_call_args_plain) is no count a
   bounded row can match: only a row open to any count and any block shape
   answers it, which is how the name lists this table replaced answered.
   0 when no row matches. */
unsigned ty_poly_face_owners(const char *name, int argc, int has_blk, int plain, int with_last);
/* The flags of the rows that give `owner` the name at this call shape: the
   owner's own write-back and argument rules, apart from the other owners'. */
unsigned ty_poly_face_owner_flags(const char *name, int argc, int has_blk, int plain, unsigned owner);
/* Does the read-only Hash face answer `name` in some form? The face sites
   ask by name alone, before the call's shape is known. */
int ty_poly_hash_face_name(const char *nm);
/* A String value-form bang -- a PF_STR_BANG row of the face table, the one
   list of them: its PF_STR_BANG and PF_STR_SELF flags, or 0 for any other
   name. Its plain name is the bang without the '!' (str_bang_plain). */
unsigned ty_str_bang_flags(const char *name);
/* ty_str_bang_flags for a receiver known to be a String, which reverse!
   joins */
unsigned ty_str_typed_bang_flags(const char *name);
void str_bang_plain(const char *bang, char *out, int n);

typedef enum {
  TY_UNKNOWN = 0,  /* not yet inferred, or an unsupported construct */
  TY_VOID,         /* a statement with no usable value */
  TY_NIL,
  TY_INT,
  TY_BIGINT,       /* arbitrary-precision integer (sp_Bigint *) */
  TY_FLOAT,
  TY_STRING,
  TY_STRBUF,       /* a mutable string (sp_String *); a storage refinement of
                      TY_STRING used for locals repeatedly appended via `<<`.
                      Demoted to TY_STRING on read, so it never escapes. */
  TY_SYMBOL,
  TY_BOOL,
  TY_RANGE,
  TY_FLOAT_RANGE,  /* (1.0..3.0): endpoints are sp_float (sp_FloatRange) */
  TY_STR_RANGE,    /* ("a".."e"): endpoints are strings (sp_StrRange) */
  TY_TIME,
  TY_COMPLEX,      /* Cartesian Complex value (sp_Complex: re, im) */
  TY_RATIONAL,     /* Rational value (sp_Rational: num, den) */
  TY_MATCHDATA,
  TY_REGEX,
  TY_EXCEPTION,
  TY_INT_ARRAY,
  TY_FLOAT_ARRAY,
  TY_STR_ARRAY,
  TY_POLY_ARRAY,
  TY_INT_ARRAY_ARRAY, /* array of int-arrays (sp_PtrArray of sp_IntArray*):
                         the typed nested counterpart of TY_POLY_ARRAY for a
                         monomorphic array-of-int-array. Produced ONLY by the
                         post-fixpoint narrow_object_arrays pass, like
                         TY_OBJ_ARRAY, so it never cascades through the
                         fixpoint. Indexing yields a typed sp_IntArray* with no
                         per-element boxing (#3145 / BabyStark ExtField).
                         Its float mirror is TY_FLOAT_ARRAY_ARRAY, last. */
  TY_STR_INT_HASH,
  TY_STR_STR_HASH,
  TY_INT_INT_HASH,
  TY_INT_STR_HASH,
  TY_SYM_POLY_HASH,  /* symbol keys, boxed (poly) values */
  TY_STR_POLY_HASH,  /* string keys, boxed (poly) values */
  TY_POLY_POLY_HASH, /* heterogeneous keys and values (both sp_RbVal) */
  TY_PROC,         /* a first-class Proc/lambda value (sp_Proc *) */
  TY_CURRY,        /* a curried Proc argument accumulator (sp_Curry *) */
  TY_FIBER,        /* a cooperative Fiber (sp_Fiber *) */
  TY_THREAD,       /* a green Thread (sp_thread *) */
  TY_QUEUE,        /* a thread-safe Queue (sp_queue *) */
  TY_MUTEX,        /* a Mutex (sp_mutex *) */
  TY_CONDVAR,      /* a ConditionVariable (sp_condvar *) */
  TY_RANDOM,       /* a per-instance PRNG (sp_Random *) */
  TY_DIR,          /* an open directory handle (sp_Dir *) */
  TY_ADDRINFO,     /* one resolved endpoint (sp_Addrinfo *) */
  TY_SOCKOPT,      /* Socket::Option (sp_SockOpt *) */
  TY_TMS,          /* Process.times -> Process::Tms (sp_Tms, four Floats) */
  TY_PROCESS_STATUS, /* Process::Status (sp_ProcessStatus, one pid + one int) */
  TY_OPENSTRUCT,   /* OpenStruct: dynamic symbol->value members (#3135) */
  TY_METHOD,       /* a bound Method object (sp_BoundMethod *) */
  TY_IO,           /* a File/IO handle (sp_File *) */
  TY_ARGF,         /* the ARGF pseudo-IO singleton (sp_Argf *) */
  TY_ENUMERATOR,   /* an external Enumerator (sp_Enumerator *) */
  TY_CLASS,        /* a Class/Module value (sp_Class, carries cls_id) */
  TY_POLY,         /* union / top: a value whose static type widened */
  /* Array of float-arrays (sp_PtrArray of sp_FloatArray*): the mirror of
     TY_INT_ARRAY_ARRAY for a numeric table, `Array.new(r) { Array.new(c, 0.0) }`.
     Same container representation and the same post-fixpoint-only production;
     only the element cast on index differs. Kept last, not beside its sibling:
     abi_sig_token encodes a kind as 100000 + (int)TyKind, so inserting mid-enum
     renumbers every kind after it. */
  TY_FLOAT_ARRAY_ARRAY
} TyKind;

/* The receiver type an owner bit re-enters the typed emitter with. */
static inline TyKind ty_poly_face_kind(unsigned owner) {
  switch (owner) {
    case PF_STRING: return TY_STRING;
    case PF_ARRAY:
    case PF_ENUM:   return TY_POLY_ARRAY;
    case PF_HASH:   return TY_POLY_POLY_HASH;
    case PF_INT:    return TY_INT;
    case PF_FLOAT:  return TY_FLOAT;
  }
  return TY_UNKNOWN;
}

const char *ty_name(TyKind t);         /* legacy string tag, for diagnostics */
int ty_is_numeric(TyKind t);           /* INT or FLOAT */
int ty_never_callable(TyKind t);       /* kind can never answer #call */
TyKind ty_promote_numeric(TyKind a, TyKind b); /* fold-accumulator numeric promotion */
/* Can a seeded fold (sum(seed), inject(seed, :op)) keep the element's C type?
   Ruby's accumulator is the SEED's object and every step is the seed's own
   operator, so only a seed of the element's own class stays in that slot --
   plus an Integer seed over Floats, which widens exactly and answers a Float
   in Ruby too. Every other seed folds boxed. Read by the inference and by the
   emitters, so the two can never disagree about which fold is emitted. */
int fold_seed_typed(TyKind seed, TyKind elem);
/* The seed kind a fold decides by, from the kind already resolved for the node
   and the node's own type name. An empty `[]` or `{}` literal resolves to
   TY_UNKNOWN so that a later push can narrow it, and TY_UNKNOWN is the one
   kind fold_seed_typed lets through to the typed accumulator -- but as a fold
   SEED the literal is already an Array or a Hash and belongs in the boxed
   fold. The inference and the emitter each pass the kind they resolved, so the
   rule itself is written once and they cannot answer differently. */
TyKind fold_seed_kind(TyKind resolved, const char *node_type);
int ty_is_array(TyKind t);
/* Set while the type fixpoint iterates; defined in analyze.c. Declared here
   because ty_array_of consults it -- see the TY_UNKNOWN case. */
extern int g_infer_optimistic;
/* Set while infer_write_types is re-deriving local types. That pass resets
   every non-param local to UNKNOWN at the top of each round and re-derives in
   node order, so a read reached BEFORE its own write answers UNKNOWN -- and
   ty_unify drops UNKNOWN. The previous round's answer is stashed in gc_root;
   the local-read rule falls back to it while this is set. */
extern int g_infer_write_round;
/* The highest round the inference fixpoint reached. 128 means it hit the cap
   and stopped because the cap said so, not because it converged -- which costs
   compile time and, where the cap lands mid-oscillation, can decide which of
   two typings gets emitted. SP_FIXPOINT_LOG=1 prints it. (#4116) */
extern int g_fixpoint_rounds;

TyKind ty_array_of(TyKind elem);       /* element type -> array kind */
TyKind ty_array_elem(TyKind arr);      /* array kind -> element type */
/* The type a builtin call answers on a receiver of kind `recv`, for the
   builtins whose result follows the receiver (yield.first, yield.dup, ...);
   0 when the pair is not one of them. The analyzer widens a diverging yield
   only where every site's kind is answered here, and codegen types each
   site's call from it, so the two sides read one table (see types.c). */
int ty_recv_builtin_result(const char *name, int argc, TyKind arg0, TyKind recv, TyKind *out);
int ty_is_hash(TyKind t);
/* Object's identity protocol on the native kinds (=== == != equal? eql?
   frozen? freeze, and on the IO family to_s and <=> as well), answered by
   emit_native_object_protocol and typed by infer_call from this ONE decision
   so the two can never drift. kind: 1 a GC heap handle (its pointer is its
   identity), 2 a by-value struct (Time, Process::Tms, a String range: equal
   values, no identity), 0 neither. answers: whether the arm claims `name` on
   a receiver of kind rt with an operand of kind at (argc 0 or 1); the
   cross-family tier for a Range, Array or Bignum receiver lives here too. */
int ty_object_protocol_kind(TyKind t);
int ty_object_protocol_family(TyKind t);
int ty_object_protocol_answers(TyKind rt, TyKind at, const char *name, int argc);
TyKind ty_hash_of(TyKind key, TyKind val); /* (key,val) -> hash kind (UNKNOWN if unsupported) */
TyKind ty_hash_key(TyKind h);
TyKind ty_hash_val(TyKind h);
const char *ty_hash_cname(TyKind h);   /* "StrInt" etc, for sp_<X>Hash_* */

/* Block-yield protocol for builtin iterators. The params a block bound to
   `recv.<name>` receives, expressed purely against the receiver's
   element/key/value -- i.e. the context-free iterators whose yield depends only
   on the receiver shape, not on call arguments or a receiver chain. Writes up
   to `max` param types into out[] and returns the count, or 0 if `name` is not
   such an iterator on `recv`. The single source of truth for builtin block
   protocols (forwarded-`&callable` desugar arity, builtin yield-stubs); keeps
   that knowledge from being re-encoded as scattered method-name lists. */
int ty_block_yield(TyKind recv, const char *name, TyKind *out, int max);

/* Iterator collection shape -- what a value-producing iterator BUILDS, as
   opposed to ty_block_yield's block-param types. One source of truth for the
   map/collect, select/filter, reject classifications codegen dispatches on. */
typedef enum { TY_ITER_NONE, TY_ITER_MAP, TY_ITER_SELECT, TY_ITER_REJECT } TyIterShape;
TyIterShape ty_iter_shape(const char *name);
/* Merge two observed types into the narrowest type covering both.
   Equal -> same; UNKNOWN acts as identity; otherwise widen to POLY
   (numeric int+float stays POLY for now -- mixed-numeric vars are rare
   and handled when they appear). */
TyKind ty_unify(TyKind a, TyKind b);

/* User-defined object types are encoded above the built-in range so the
   flat TyKind node-type array still works: a class with index i has type
   TY_OBJECT_BASE + i. The class table (names, ivars) lives in Compiler. */
#define TY_OBJECT_BASE 1000
/* A homogeneous array of object class i (every element an instance of one
   user class) is encoded as TY_OBJ_ARRAY_BASE + i, above the object range.
   It maps to the runtime sp_PtrArray (unboxed void* elements), so indexing
   yields a typed `sp_X *` directly with no per-element boxing/dispatch --
   the typed counterpart of TY_POLY_ARRAY for monomorphic object arrays.
   Produced ONLY by the conservative post-fixpoint narrow_object_arrays pass,
   never by forward inference, so it cannot cascade through the fixpoint. */
#define TY_OBJ_ARRAY_BASE 1000000
static inline int    ty_is_object(TyKind t)   { return (int)t >= TY_OBJECT_BASE && (int)t < TY_OBJ_ARRAY_BASE; }
static inline TyKind ty_object(int class_id)  { return (TyKind)(TY_OBJECT_BASE + class_id); }
static inline int    ty_object_class(TyKind t){ return (int)t - TY_OBJECT_BASE; }
static inline int    ty_is_obj_array(TyKind t)   { return (int)t >= TY_OBJ_ARRAY_BASE; }
static inline TyKind ty_obj_array(int class_id)  { return (TyKind)(TY_OBJ_ARRAY_BASE + class_id); }
static inline int    ty_obj_array_class(TyKind t){ return (int)t - TY_OBJ_ARRAY_BASE; }
/* Names a boxed HANDLE answers by being unboxed back to its own type. Each
   one belongs to exactly one builtin handle class and to no other class in
   the language, so the name alone identifies the receiver: a value of any
   other kind was going to raise NoMethodError anyway, and the emitted arm
   checks the runtime cls_id before it dereferences. Every one of them is a
   reader that takes no arguments, so a call that passes any is not one of
   them, whatever its name.

   This is the exclusive-name mechanism, and exclusivity is the whole safety
   argument -- a SHARED name (Dir#path is also IO#path, Dir#read is also
   IO#read) must never come here, or a poly IO receiver gets a sp_Dir body
   compiled against it. That family needs a runtime tag arm instead.

   Answers TY_UNKNOWN for a name that is not one of these. */
static inline TyKind ty_poly_handle_face(const char *nm) {
  if (!nm) return TY_UNKNOWN;
  {
    static const char *const ADDRINFO[] = {
      "ip_address", "unix_path", "afamily", "pfamily", "afamily_name",
      "ip_port", "socktype", "protocol", "ipv4?", "ipv6?", "unix?", "ip?",
      "to_sockaddr", 0 };
    for (int i = 0; ADDRINFO[i]; i++) if (sp_streq(nm, ADDRINFO[i])) return TY_ADDRINFO;
  }
  {
    static const char *const SOCKOPT[] = {
      "int", "bool", "level", "optname", "family", 0 };
    for (int i = 0; SOCKOPT[i]; i++) if (sp_streq(nm, SOCKOPT[i])) return TY_SOCKOPT;
  }
  return TY_UNKNOWN;
}

/* TY_OBJ_ARRAY, TY_INT_ARRAY_ARRAY and TY_FLOAT_ARRAY_ARRAY are all backed by
   sp_PtrArray (unboxed void* elements): code that only cares about the container
   representation (new, push, length, box, GC-root) uses this; the element cast on
   index differs. ty_ptr_array_elem_ctype names that cast. */
static inline int    ty_is_ptr_array(TyKind t)   { return ty_is_obj_array(t) ||
                                                          t == TY_INT_ARRAY_ARRAY ||
                                                          t == TY_FLOAT_ARRAY_ARRAY; }
/* The scalar-array element of a nested ptr-array kind ("sp_IntArray" /
   "sp_FloatArray"), or NULL for an object array, whose element C type is the
   class's own struct and has to come from the class table. */
static inline const char *ty_ptr_array_elem_ctype(TyKind t) {
  if (t == TY_INT_ARRAY_ARRAY)   return "sp_IntArray";
  if (t == TY_FLOAT_ARRAY_ARRAY) return "sp_FloatArray";
  return 0;   /* types.h has no stddef.h; NULL is not in scope here */
}

/* ---- ty_traits: a builtin kind's spellings (R7, #7100) ----
   One row per builtin TyKind, one column per spelling the compiler writes
   for a value of that kind, each read off one existing function (named
   beside it); where two functions spell the same thing differently, each
   has its own column. A user object's kind (TY_OBJECT_BASE + class) has no
   row: its spellings depend on its class. ty_traits_check.c compares every
   cell with its function (spinel --check-traits). In the renderings, $e is
   the expression and $t the first temp the form takes. */
typedef struct {
  const char *ctype;          /* c_type_name */
  const char *zero;           /* default_value */
  const char *zero_tail;      /* raise_tail_value: an untyped slot's dead value */
  const char *nil;            /* nil_value */
  const char *box;            /* ty_box_fn: emit_boxed, a value that is never nil */
  const char *box_nil;        /* ty_box_nil_fn: emit_boxed, a slot that holds nil */
  const char *box_text;       /* emit_boxed_text's rendering, or NULL: no box */
  const char *box_id;         /* ty_nullable_builtin_id */
  const char *hash_id;        /* hash_box_cls */
  const char *unbox;          /* emit_unbox_text's rendering */
  const char *unbox_nil;      /* emit_unbox_nilable_text's rendering */
  const char *unbox_rhs;      /* poly_rhs_unbox_fn: emit_poly_rhs_coerced */
  const char *unbox_sink;     /* poly_sink_unbox_fn: emit_typed_sink_text */
  const char *unbox_token;    /* token_unbox_fmt: emit_unresolved_coerced */
  const char *nil_test_local; /* local_nil_test for a slot with no flags, or NULL */
  unsigned char null_is_nil;  /* ty_null_is_nil */
  unsigned char needs_root;   /* needs_root */
  unsigned char struct_valued;/* ty_is_struct_valued */
  unsigned char scalar_ret;   /* is_scalar_ret */
  unsigned char store_class;  /* repr_store_class (SC_*) */
  unsigned char box_form;     /* the form --repr-check records for box_text (RF_*) */
} TyTraits;
#define TY_TRAITS_N ((int)TY_FLOAT_ARRAY_ARRAY + 1)
extern const TyTraits ty_traits[TY_TRAITS_N];
/* the row of builtin kind t, or NULL (a user object's kind) */
static inline const TyTraits *ty_traits_of(TyKind t) {
  return (int)t >= 0 && (int)t < TY_TRAITS_N ? &ty_traits[t] : (const TyTraits *)0;
}

#endif
