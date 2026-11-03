/* builtin_ops.h -- builtin methods as data, read by inference and codegen.

   A row says what a builtin call on a receiver of one TyKind answers and
   how codegen emits it. Inference reads the result kind; codegen reads the
   emitter id and runs the emitter codegen_ops.c keeps for it. One row
   replaces the matching rule in infer_call_inner and the matching arm in
   emit_call_body, which today each decide the same call separately (#7100).

   A family's lookup sits where that family's legacy branch sat in each
   chain, so everything an earlier branch claims still goes there; it moves
   up only once nothing earlier can match the same call. */
#ifndef SPINEL_BUILTIN_OPS_H
#define SPINEL_BUILTIN_OPS_H

#include "types.h"

/* Name-only properties of the boxed builtin surfaces. They do not depend
   on a call's arity, block, or the concrete receiver row. */
enum {
  BOP_READ_NUMERIC = 1u,
  BOP_READ_CONTAINER = 2u,
  BOP_READ_STRING = 4u,
  BOP_READ_ARRAY_INTACT = 8u   /* reads an Array and leaves it as it is (a template over a constant's elements) */
};
enum {
  BOP_MUT_LOCAL = 1u,
  BOP_MUT_CONTAINER = 2u,
  BOP_MUT_IVAR = 4u,
  BOP_MUT_NARROW = 8u,
  BOP_MUT_STATE = 16u
};
int bop_name_has_reader(const char *name, unsigned surface);
int bop_name_mutates(const char *name, unsigned sites);
/* Whether a lone splat into `name` on a boxed receiver reaches the builtin
   through its generic entry (an_bi_generic_splat, bi_generic_fn), and up to
   how many arguments (*most, -1 for every count the builtin takes). */
int bop_generic_entry(const char *name, int *most);

/* ---- what a builtin calls back on the values it is handed ----
   Generated from CRuby (src/builtin_callbacks.inc, tools/gen_builtin_callbacks.rb):
   for a builtin method, the methods of the objects it is handed that it calls
   without the program naming them, and in what role the object is handed over.
   A role is the position the object is in when the builtin calls it. */
enum {
  BCR_ARG = 1u,     /* an argument */
  BCR_ELEM = 2u,    /* an element of the receiver, or of an argument that is a collection */
  BCR_BLOCK = 4u,   /* a value the block answers */
  BCR_SELF = 8u,    /* the receiver, when it is a program subclass of the core class */
  BCR_CLASS = 16u,  /* a class object handed over (raise K, K as a key) */
  BCR_PORT = 32u,   /* an IO argument */
  BCR_ASKED = 64u,  /* asked through rb_check_funcall: respond_to?, respond_to_missing?, method_missing */
  BCR_LOADED = 128u,/* an object the data builds (Marshal.load, YAML.unsafe_load): any class of the program may receive it */
  BCR_GVAR = 256u,  /* $stdout or $stderr holding the object: the assignment is a write of a port global, whose row keeps it */
  BCR_HOOK = 512u,  /* the receiver, a class or module of the program: a singleton hook it defines that the builtin calls (const_get: const_missing) */
  BCR_ALL = 1023u
};
typedef struct { const char *name; unsigned roles; } BopCallback;
/* What any core method called `builtin` may call back, the union over its
   receivers: the callbacks written to *out and their count, 0 for a name that
   is no core method, -1 for a core name the table does not cover (nothing
   was probed for it: it may call anything). For a pass that runs before
   types exist. */
int bop_name_callbacks(const char *builtin, const BopCallback **out);
/* Every callback name with the roles in which some builtin calls it, sorted;
   their number */
int bop_callback_names(const BopCallback **out);
/* The roles in which some builtin calls `name` (BCR_ bits), 0 for a name none does */
unsigned bop_callback_roles(const char *name);
/* The callbacks of `builtin` on one receiver class ("Array"; "File." for a
   singleton method), for the typed passes. -1 means unknown, as it does for
   bop_name_callbacks: that receiver was not probed for it, and the caller
   treats the builtin as one that may call anything. */
int bop_class_callbacks(const char *recv_class, const char *builtin, const BopCallback **out);
/* --check-bop-callbacks: the generated tables' order and shape; answers the
   number of problems */
int builtin_ops_callbacks_check(void);

/* The runtime call behind a zero-argument IO/String row. Boxed dispatch
   owns its tag and result-slot guards; it shares the call facts with the
   typed rows without borrowing their arity or return-value policy. */
typedef struct {
  TyKind recv;
  const char *name, *fn;
  TyKind result;
  const char *tail;
} BuiltinZeroOp;
const BuiltinZeroOp *bop_zero_find(TyKind recv, const char *name);

/* What a row asks of the call's block. BF_ANY: the legacy rule ignored the
   block, so the row does too. */
typedef enum { BF_ANY, BF_NONE, BF_REQUIRED } BopBlock;

/* The emitter codegen runs for a row (codegen_ops.c). BOPE_NONE: the row
   only types the call; codegen leaves it to the legacy chain. */
typedef enum {
  BOPE_NONE,
  BOPE_RANGE_CLONE,      /* Range#clone(freeze: ...) */
  BOPE_RANGE_FREEZE,     /* Range#freeze */
  BOPE_TEMPLATE,          /* arg, its placeholders filled (codegen_ops.c) */
  BOPE_PSTATUS_SUCCESS,   /* Process::Status#success?: true, false or nil */
  BOPE_PSTATUS_EQ,        /* Process::Status#== / #eql? with no operand */
  /* the concurrency handles (codegen_call_concurrency.c) */
  BOPE_THREAD_SET_REPORT, /* Thread#report_on_exception= */
  BOPE_THREAD_RAISE,      /* Thread#raise */
  BOPE_THREAD_TLS,        /* Thread#[] / []= / key? / thread_variable_*, any key */
  BOPE_MUTEX_SLEEP,       /* Mutex#sleep */
  BOPE_CONDVAR_WAIT,      /* ConditionVariable#wait */
  BOPE_QUEUE_PUSH,        /* Queue#push / << / enq, with non_block and timeout: */
  BOPE_QUEUE_POP,         /* Queue#pop / shift / deq, likewise */
  BOPE_FIBER_RESUME,      /* Fiber#resume */
  BOPE_FIBER_TRANSFER,    /* Fiber#transfer */
  BOPE_FIBER_RAISE,       /* Fiber#raise */
  /* Complex and Rational (codegen_call_numeric.c) */
  BOPE_RATIONAL_ROUND,    /* Rational#round/floor/ceil/truncate with digits or half: */
  /* String (codegen_call_recv.c) */
  BOPE_POLY_CASE_OPTIONS, /* boxed String/Symbol case mapping with options */
  BOPE_STR_SET_N,         /* String#squeeze / #delete / #count over several sets */
  BOPE_STR_AFFIX_ANY,     /* String#start_with? / #end_with? over several candidates */
  /* Hash (codegen_call_hash.c) */
  BOPE_HASH_PATTERN,      /* any?/none?/one?/count with a pattern, no block */
  BOPE_HASH_PATTERN_ALL,  /* all? with a pattern, no block */
  BOPE_HASH_DEFAULT_PROC, /* Hash#default_proc */
  BOPE_HASH_TO_PROC,      /* Hash#to_proc */
  BOPE_HASH_AREF,         /* Hash#[] */
  BOPE_HASH_HAS_KEY,      /* has_key?/key?/include?/member? */
  BOPE_HASH_KEY,          /* Hash#key */
  BOPE_HASH_DEFAULT,      /* Hash#default, #default(key) */
  BOPE_HASH_KEYS,         /* Hash#keys */
  BOPE_HASH_FETCH,        /* fetch(key), no default, no block */
  BOPE_HASH_TO_S,         /* Hash#to_s */
  BOPE_HASH_COMPACT_BANG, /* compact! on a poly-valued variant */
  BOPE_HASH_REHASH,       /* Hash#rehash */
  BOPE_HASH_REPLACE,      /* replace(hash) of the same variant, or into PolyPoly */
  BOPE_HASH_SET_DEFAULT,  /* Hash#default= */
  BOPE_HASH_MERGE_BANG_MANY, /* merge!/update(h1, h2, ...) of the same variant */
  BOPE_HASH_SHIFT,        /* Hash#shift, no block */
  BOPE_HASH_DELETE,       /* delete(key) without a block literal */
  BOPE_HASH_INVERT,       /* Hash#invert */
  BOPE_HASH_FLATTEN,      /* Hash#flatten, #flatten(depth) */
  BOPE_HASH_TO_A,         /* to_a / entries */
  BOPE_HASH_NIL_RECV,     /* length / size / count / empty?: a receiver that may be nil is tested under the name as written */
  BOPE_HASH_SORT,         /* sort, no block */
  BOPE_HASH_FIRST,        /* first, no block */
  BOPE_HASH_TAKE,         /* first(n) / take(n), no block */
  BOPE_HASH_DROP,         /* drop(n), no block */
  BOPE_HASH_ASSOC,        /* assoc / rassoc */
  BOPE_HASH_COMPACT,      /* Hash#compact */
  /* Array (codegen_call_array.c) */
  BOPE_ARRAY_SHIFT_N,     /* Array#shift(n) / #pop(n) */
  BOPE_ARRAY_CYCLE_N,     /* Array#cycle(n) without a block, materialized */
  BOPE_ARRAY_LAST,        /* Array#last */
  BOPE_ARRAY_FIRST,       /* Array#first (no count) */
  BOPE_ARRAY_POP_SHIFT,   /* Array#pop / #shift (no count) */
  BOPE_ARRAY_JOIN,        /* Array#join, with or without a separator */
  BOPE_ARRAY_PACK_BUFFER, /* Array#pack(format, buffer: string) */
  BOPE_ARRAY_SORT_BANG,   /* Array#sort! */
  BOPE_ARRAY_SLICE_BANG_RANGE, /* Array#slice!(range) */
  BOPE_ARRAY_PLUS,        /* Array#+ */
  BOPE_ARRAY_SETOP,       /* Array#& | - and intersection / union / difference with operands */
  BOPE_ARRAY_INTERSECT_P, /* Array#intersect? */
  BOPE_ARRAY_REPLACE,     /* Array#replace */
  BOPE_ARRAY_MINMAX,      /* Array#minmax without a block */
  BOPE_ARRAY_SORT,        /* Array#sort */
  BOPE_ARRAY_UNIQ,        /* Array#uniq */
  BOPE_ARRAY_NMIN,        /* Array#min(n) / #max(n) without a block */
  BOPE_ARRAY_SUM0,        /* Array#sum without a seed or a block */
  BOPE_ARRAY_COMPACT_BANG, /* Array#compact! / #flatten! with no depth */
  BOPE_ARRAY_FLATTEN,     /* Array#flatten / #flatten(depth) / #flatten!(depth) */
  BOPE_ARRAY_PUSH,        /* Array#push / << / append of one value (poly) */
  BOPE_ARRAY_INSERT_N,    /* Array#insert(i, v...) (poly) */
  BOPE_ARRAY_TRANSPOSE,   /* Array#transpose (poly) */
  BOPE_ARRAY_NIL_RECV,    /* Array#length / size / count / empty? / to_a / entries: a receiver that may be nil is tested */
  BOPE_ARRAY_ASSOC,       /* Array#assoc / #rassoc (poly) */
  BOPE_ARRAY_COMBINATION, /* Array#combination / #permutation and the repeated forms, blockless */
  BOPE_ARRAY_PRODUCT,     /* Array#product without a block */
  BOPE_ARRAY_FETCH_VALUES0, /* Array#fetch_values with no keys (typed) */
  BOPE_ARRAY_PRED0,       /* Array#all? / any? / none? / one? with no pattern or block */
  BOPE_ARRAY_DIG_N,       /* Array#dig with two or more keys (typed) */
  BOPE_ARRAY_SUM1,        /* Array#sum(seed) without a block (poly) */
  BOPE_ARRAY_CONCAT,      /* Array#concat(*arrays) (poly) */
  BOPE_ARRAY_INDEX_V,     /* Array#index / find_index / rindex of a value (poly) */
  BOPE_ARRAY_CYCLE_ENDLESS, /* Array#cycle with no count or block (stage 3) */
  BOPE_ARRAY_SLICE_GROUPS, /* Array#slice_before / slice_after without a block (stage 3) */
  BOPE_ARRAY_JOIN_STR,    /* Array#* with a String (stage 4) */
  BOPE_ARRAY_PRED_CLASS,  /* Array#any? / all? / none? / one? with a Class (stage 5) */
  /* Method and Proc (codegen_call_method.c) */
  BOPE_METHOD_UNBIND,     /* Method#unbind */
  BOPE_METHOD_SUPER_METHOD, /* Method#super_method */
  BOPE_METHOD_BIND,       /* UnboundMethod#bind(obj) */
  BOPE_METHOD_TO_PROC,    /* Method#to_proc */
  BOPE_METHOD_ORIGINAL_NAME, /* Method#original_name */
  BOPE_METHOD_SOURCE_LOCATION, /* Method#source_location */
  BOPE_METHOD_PARAMETERS, /* Method#parameters (stage 1) */
  BOPE_METHOD_OWNER,      /* Method#owner */
  BOPE_METHOD_RECEIVER,   /* Method#receiver */
  BOPE_METHOD_ARITY,      /* Method#arity */
  BOPE_PROC_PARAMETERS,   /* Proc#parameters, #parameters(lambda: ...) */
  BOPE_PROC_SOURCE_LOCATION, /* Proc#source_location */
  BOPE_IVAR_REFLECTION,   /* reflection on a builtin value without ivar slots */
  BOPE_FLOAT_RATIONALIZE,
  BOPE_STRING_SCAN_CHECKED,
  BOPE_STRING_SLICE,     /* String#slice!: lvalue and pattern-dependent */
  BOPE_STRING_DEDUP,     /* String#dedup: -@ under the name a nil receiver is raised with */
  BOPE_STRING_SCRUB_BLOCK, /* String#scrub with a block */
  BOPE__COUNT
} BopEmit;

/* A set of argument kinds, one bit per TyKind: BOP_K(TY_INT) | BOP_K(TY_FLOAT) */
typedef unsigned long long BopKinds;
#define BOP_K(k) (1ULL << (k))
_Static_assert(TY_FLOAT_ARRAY_ARRAY < 64, "a BopKinds set holds a bit per TyKind");

typedef struct BuiltinOp {
  TyKind recv;            /* receiver kind the row applies to */
  const char *name;
  signed char argc_min, argc_max;
  BopBlock block;
  TyKind result;          /* what the call answers; TY_UNKNOWN: inference
                             leaves the call to the rules after it */
  BopEmit emit;
  const char *arg;        /* BOPE_TEMPLATE's C text */
  BopKinds arg0, arg1;    /* the kinds the first and second argument may
                             have (BOP_K), or 0 for any */
  unsigned short flags;   /* BOPF_* */
  unsigned char stage;    /* the codegen lookup that emits the row: 0, the
                             default, is the one each family has always had;
                             a family whose arms sit at several distant
                             places in the emission chain gives the rows of
                             each place their own stage. Inference reads
                             every row whatever its stage. */
} BuiltinOp;

/* The row answers for a boxed (poly) receiver too, behind a run-time class
   check: emit_poly_builtin_method emits these names unboxed, so inference
   types the call with the row's result (bop_find_boxed). A String row
   carries it for a name that answers the same kind whatever the box holds
   (inspect, include?): the String row stands for the boxed value, and
   --plan-check accepts it for the row of the kind codegen emits the call
   typed with (a yield read per call site, a face) when both state the same
   result. */
#define BOPF_BOXED 1
/* The call answers its receiver: the same object, not a copy, and so an
   instance of a subclass answers that instance (<<, push, concat, replace,
   clear, sort!, freeze, to_ary, each with a block, ...). A fact about
   CRuby's method, whatever the row's result kind says about the value's
   representation (BOPR_SELF is the receiver's kind, which a copy has too).
   A conversion that answers the receiver only when its class is exactly
   the builtin is BOPF_SELF_EXACT instead. */
#define BOPF_SELF 2
/* The call answers its receiver when it changed it, and nil when it changed
   nothing: the bang methods with a no-change contract (uniq!, compact!,
   select!, sub!, strip!, ...). Never set together with BOPF_SELF; a bang
   that answers the receiver either way (sort!, reverse!, succ!) is
   BOPF_SELF. */
#define BOPF_SELF_OR_NIL 4
/* The call answers an object of the receiver's class: the receiver itself
   or a copy of it, depending on whether it is frozen (String#+@ answers
   the receiver when it is not frozen, a copy when it is; -@ and dedup the
   receiver when it is frozen, a frozen copy when it is not). A subclass
   instance answers an instance of that subclass. */
#define BOPF_SELF_CLASS 8
/* The call answers its receiver when its class is exactly the builtin, and
   a new object of the plain builtin for a subclass instance, without the
   instance's ivars: Array#to_a, Hash#to_h without a block (the new Hash
   keeps the default and the default proc), String#to_s and #to_str. */
#define BOPF_SELF_EXACT 16
/* The call answers a new object of the receiver's class, never the
   receiver: a copy carrying its instance variables, and a Hash's default
   and default proc, with the pairs or elements the method leaves (dup and
   clone, Hash#merge and #compact). clone carries the frozen state too, the
   others answer an unfrozen copy. String#encode, which answers an
   instance of the receiver's class without its ivars, carries no flag. */
#define BOPF_COPY_CLASS 32
/* The call combines, compares or copies its arguments of the receiver's
   builtin class as that builtin: a subclass instance among them is read
   for its elements, pairs or bytes, and none of its own methods (each,
   to_ary, to_hash, to_str, ==, !=, <=>, ...) runs. Array#+ - & | <=> == != eql?
   concat replace union difference intersection intersect? product zip;
   Hash#merge merge! update replace == eql? < <= > >=; String#+ concat <<
   prepend insert replace == === eql? <=> < <= > >= between?. A method that
   stores an argument as an element, key or value (push, <<, insert and []=
   on an Array, store and []= on a Hash) or takes it as a pattern or
   separator (String#include?, #sub, #split) is not flagged; for those the
   flag's absence says nothing about how the argument is read. The rows'
   argument kinds (arg0/arg1) cannot say this: they guard which row fits a
   call, a guarded row is not found by a lookup without the arguments'
   kinds, and they cover only the first two arguments. */
#define BOPF_ARGS_BUILTIN 64
/* The row's result is TY_UNKNOWN because inference types the call in a rule
   of its own, from a fact of this call that no kind carries, and the row
   only gives codegen its emitter. Method#receiver is the type of the
   Method's bound receiver expression; a Float range's begin, first, min and
   last are the endpoint the Range literal wrote ((1..2.5).begin is the
   Integer 1); String#scan reads the pattern literal's source for capture
   groups and String#unpack1 the format literal; Rational#round, truncate,
   floor and ceil with a digit count answer an Integer or a Rational by the
   value of the Integer literal that gives it (a positive count keeps the
   Rational; a count that is no literal is boxed). A call whose answer is a
   function of the receiver's and the arguments' kinds does not belong
   here: it states its kind. --plan-check expects inference to have
   answered such a call without a row, as long as the call was typed; a
   TY_UNKNOWN row without the flag is a call the table does not type yet.

   Only those rows set it: most TY_UNKNOWN rows whose answer genuinely is
   hand-written elsewhere (`[]`, `slice`, `[]=`, `==`, `fetch`'s 2-arg
   form, ...) were never given the flag either, so its absence is not
   reliable evidence that a row is unfinished rather than a deliberate defer
   -- check what reads the row (an arm in
   infer_array_call/infer_numeric_call/infer_hash_call, or a plan-check
   expectation) before assuming a bare TY_UNKNOWN is a gap to close. */
#define BOPF_INFERRED 128

/* A blockless row that answers the call unless the call is the receiver of
   a call with a block (`arr.select.with_index { }`; `.each { }` excepted,
   which runs the method the Enumerator came from): the chain emitters own
   that shape and the inner call is answered by the rules after the lookup,
   so inference does not read the row for it. The row states what the call
   answers anywhere else, an Enumerator. Only the rows that carry the flag
   pay for the test, which walks the calls. */
#define BOPF_NOT_CHAIN_RECV 256
/* The method is private in CRuby: only a call that ignores visibility
   (`__send__`, `send`, a call with no receiver or with `self`) reaches the
   row; an explicit receiver is NoMethodError, and the call is left to the
   rules after the lookup (cplan_class_row, bop_flags). */
#define BOPF_PRIVATE 512
/* The method is Class's, not Module's (Class#inherited): a Module value
   has none. Spinel types a module as a class too, so the row answers a call
   only on a receiver proven a Class (cplan_recv_class). */
#define BOPF_CLASS_ONLY 1024
/* The method is BasicObject's (singleton_method_added): it stands after
   Object's methods, so a top-level def of the name is the one a send
   reaches (send_blind_recv_owns does not count the row). */
#define BOPF_BASIC_OBJECT 2048

/* A row's recv may name a family of kinds rather than one; a caller looks
   the family up with the family's value. Not a TyKind any value has. */
#define BOP_IVAR_LESS ((TyKind)-4)  /* ty_builtin_ivar_less, at the reflection lookup */
#define BOP_ANY_HASH  ((TyKind)-2)   /* every Hash kind (ty_is_hash) */
#define BOP_ANY_ARRAY ((TyKind)-3)   /* every Array kind (ty_is_array) */

/* A row's result may be derived from the receiver's kind (bop_result). */
#define BOPR_SELF       ((TyKind)-10)   /* the receiver's own kind */
#define BOPR_HASH_VAL   ((TyKind)-11)   /* a Hash's value kind */
#define BOPR_HASH_KEYS  ((TyKind)-12)   /* an Array of a Hash's keys */
#define BOPR_HASH_VALS  ((TyKind)-13)   /* an Array of a Hash's values */
#define BOPR_ELEM       ((TyKind)-14)   /* an Array's element kind */
#define BOPR_HASH_KEY_OF ((TyKind)-15)  /* Hash#key: a Symbol for a Symbol-keyed
                                           hash, else the boxed key or nil */
#define BOPR_HASH_INVERT ((TyKind)-16)  /* Hash#invert: a String=>String hash
                                           stays one, any other is the general hash */
#define BOPR_ARRAY_SUM   ((TyKind)-17)  /* Array#sum, no seed or block: the element
                                           kind; a String array's is boxed (it only
                                           raises, or answers 0 when empty). Float results are
                                           boxed too; Integer results defer to mode-aware inference. */
#define BOPR_ARRAY_INDEX ((TyKind)-18)  /* Array#index/find_index/rindex(v): an Int,
                                           Str or Float array's boxed (nil on a miss),
                                           any other's an Integer */
#define BOPR_ARRAY_TUPLES ((TyKind)-19) /* blockless combination & co.: a poly array
                                           whose position keeps the tuples (BOPX_KEPT)
                                           materializes them, any other call answers an
                                           Enumerator */
#define BOPR_ARRAY_PLUS  ((TyKind)-20)  /* Array#+(other): bop_result_args. An Array
                                           of the receiver's kind or an untyped one
                                           (an empty literal) keeps the receiver's
                                           kind, another Array's gives a poly array,
                                           a boxed operand is boxed; any other
                                           operand states no kind */
#define BOPR_ARRAY_SETOP ((TyKind)-21)  /* Array#& | - and union, intersection and
                                           difference, folded over every operand:
                                           the receiver's kind when each keeps it,
                                           else a poly array if each is an Array
                                           or boxed, else no kind */
#define BOPR_ASSOC       ((TyKind)-22)  /* Array#assoc / #rassoc(v): a poly array's
                                           matching sub-array is a poly array; an
                                           Integer, Float, String, Symbol or boolean
                                           array holds none to match, the boxed nil;
                                           any other array states no kind */
#define BOPR_ARG0        ((TyKind)-23)  /* the first argument's kind (Hash#default=:
                                           it answers the value it was given) */
#define BOPR_ARG1        ((TyKind)-24)  /* the second argument's kind
                                           (instance_variable_set(name, value)) */
#define BOPR_HASH_REPLACE ((TyKind)-25) /* Hash#replace(other): the receiver's kind
                                           when other is the same Hash kind, else
                                           the general hash. replace returns self,
                                           but self's slot widens to the general
                                           hash when other is another variant or a
                                           boxed one (which holds whichever variant
                                           the value really is), so a `r = h.replace(o)`
                                           capture agrees from the first fixpoint
                                           round (#2374, #3975) */
#define BOPR_HASH_STORE  ((TyKind)-26)  /* Hash#[]= and store(key, value): the
                                           value's kind unified with the Hash's
                                           value kind */
#define BOPR_ARRAY_SUM_SEED ((TyKind)-27) /* blockless Array#sum(seed): the seed
                                           and element kinds, in the build mode */

/* argc_max of a row that takes any number of arguments */
#define BOP_ARGC_ANY 127

/* The row for `name` called with `argc` arguments (and a block when
   has_block) on a receiver of kind rt, or NULL. Pure: it reads no node and
   no Compiler state.

   Rows of one name are tried narrowest arity first, and among rows of the
   same arity in the order they are written. So a codegen row for one arity
   comes before the wider row that only types the call for every arity, and
   a row with an argument guard before the unguarded row of its arity. A
   row with an argument guard never fits here: bop_find_arg checks the
   guard. */
/* the rows, in order, for a check that walks them (builtin_ops_arity_check) */
int bop_row_count(void);
const BuiltinOp *bop_row(int i);
const BuiltinOp *bop_find(TyKind rt, const char *name, int argc, int has_block);
/* The flags (BOPF_*) of the row of kind rt for `name`, 0 for none */
int bop_flags(TyKind rt, const char *name);
/* Does the builtin answer `name` on a class (or module) itself, taking argc
   arguments and a block when has_block, where no method of the program's
   of the name runs yet: Module's own, as its builtin-op rows (TY_CLASS)
   list them (`name`, `to_s`, `inspect`, `include?`)? A negative argc (no
   one count) is not answered, nor a private row: a hook (`inherited`,
   `included`, ...) whose version entry falls back to super, the core's
   no-op (ver_named_kind). */
int bop_class_answers(const char *name, int argc, int has_block);

/* bop_find, with arg_of(ud, i) answering argument i's kind for the rows
   that guard it. It is called at most once per argument, and only when
   such a row is a candidate, so a lookup that needs no argument's kind
   computes none. */
typedef TyKind (*BopArgKind)(const void *ud, int i);
const BuiltinOp *bop_find_arg(TyKind rt, const char *name, int argc, int has_block,
                              BopArgKind arg_of, const void *ud);
/* bop_find_arg over the rows of one stage only (codegen's lookups); a
   negative stage reads every row, as bop_find_arg does */
const BuiltinOp *bop_find_stage(TyKind rt, const char *name, int argc, int has_block,
                                BopArgKind arg_of, const void *ud, int stage);

/* Whether any row applies to receivers of kind rt: a caller checks this
   before reading the call's arguments, so receivers no row covers cost
   nothing. */
int bop_covers(TyKind rt);

/* The kind whose rows a receiver of kind rt reads: rt itself where a row
   covers it, else the Array or Hash family's (every variant shares them). */
TyKind bop_family_of(TyKind rt);

/* What the call answers on a receiver of kind rt: the row's result, with a
   derived result (BOPR_*) worked out from rt. */
TyKind bop_result(const BuiltinOp *op, TyKind rt);
/* bop_result for the rows whose answer also turns on the call's operands'
   kinds (BOPR_ARRAY_PLUS, BOPR_ARRAY_SETOP, BOPR_ARG0, BOPR_ARG1,
   BOPR_HASH_REPLACE, BOPR_HASH_STORE); any other row answers as bop_result
   does. arg_of(ud, i) answers argument i's kind, as for
   bop_find_arg, and is called only for those rows. TY_UNKNOWN where the row
   states no kind for these operands. It reads no node: the caller brings
   the kinds. BOPR_ARRAY_SUM_SEED reads the seed through fold_seed_kind at
   the caller and takes the integer overflow mode, and BOPR_ARRAY_TUPLES
   whether the call keeps its tuples, through facts (BOPX_*). */
TyKind bop_result_args(const BuiltinOp *op, TyKind rt, int argc, BopArgKind arg_of, const void *ud, int facts);
/* Facts about the call that no operand's kind carries, for bop_result_args */
#define BOPX_PROMOTE 1  /* --int-overflow=promote */
#define BOPX_KEPT    2  /* a blockless combination & co. whose position keeps
                           the tuples as an Array (an_blockless_tuples_kept) */

/* The BOPF_BOXED row of kind rt for `name`, or NULL. */
const BuiltinOp *bop_find_boxed(TyKind rt, const char *name, int argc, int has_block);

/* Whether the builtin `name` called with argc arguments (and a block when
   has_block) on a receiver of kind rt answers that receiver, a copy of it,
   or the plain builtin: BOPF_SELF, BOPF_SELF_OR_NIL, BOPF_SELF_CLASS,
   BOPF_SELF_EXACT or BOPF_COPY_CLASS, or 0 when it answers another value
   or no row has the call. An Array or Hash kind reads its family's rows
   (BOP_ANY_ARRAY, BOP_ANY_HASH), a String buffer the String rows. The
   flags sit on the unguarded rows, the ones a lookup without the
   arguments' kinds finds. */
int bop_answers_self(TyKind rt, const char *name, int argc, int has_block);
/* Whether that call's value is its receiver itself, or nil (a bang that
   changed nothing): a BOPF_SELF, BOPF_SELF_OR_NIL or BOPF_SELF_EXACT row,
   or, given a block, an iterator row (iter_row_of) answering IA_RECV or
   IA_BLOCKVALS_INPLACE (each, tap, map!). The value is then another name
   of the receiver's object, not a copy of it. */
int bop_answers_recv(TyKind rt, const char *name, int argc, int has_block);
/* Is `name` a method every value answers that hands its block the receiver
   itself, once (an any-receiver iterator row yielding YS_RECV: tap, then,
   yield_self)? Its answer is the receiver or the block's value. */
int iter_hands_recv(const char *name);
/* Whether that call reads its arguments of the receiver's builtin class as
   that builtin (BOPF_ARGS_BUILTIN), looked up as bop_answers_self does. */
int bop_args_as_builtin(TyKind rt, const char *name, int argc, int has_block);

/* ---- What a builtin call does with the Strings it is handed (#6765) ----
   The facts --share-strings reads (analyze_share.c): which of the call's
   values the answer can be, and where the arguments can end up. One row per
   receiver family and name. Generated pure rows cover exact observed
   names; "*" denotes an operator. A name without a row receives the
   unknown treatment. */
#define BOP_KERNEL   ((TyKind)-5)   /* a receiverless builtin (Kernel) */
#define BOP_ANY_RECV ((TyKind)-6)   /* Object's methods, on any receiver */
#define BOP_CALLABLE ((TyKind)-7)   /* a proc, a lambda or a Method */
#define BOP_CLASS_NEW ((TyKind)-8)  /* a builtin class's `new`, by the class's name */
#define BOP_FILE_CLASS ((TyKind)-9) /* a class method of File (`File.join`) */
#define BOP_ENV      ((TyKind)-10)  /* ENV's methods (`ENV["HOME"]`) */
#define BOP_FILETEST ((TyKind)-11)  /* FileTest's path tests */

typedef enum {
  BSH_PURE = 1,   /* keeps none of its arguments; answers no value it was handed
                     (a block a container's runs is handed its elements) */
  BSH_FROZEN,     /* answers a frozen String: its receiver if frozen, else a
                     frozen copy (-@, dedup); nothing can change it in place
                     any more. freeze answers its receiver (BSH_RECV) */
  BSH_RECV,       /* answers its receiver */
  BSH_CLAMP,      /* answers its receiver or one of its bounds */
  BSH_ELEM,       /* answers an element of the receiver (with a count: a SUB) */
  BSH_SUB,        /* answers a container of the receiver's elements */
  BSH_STORE_LAST, /* stores its last argument among the receiver's elements */
  BSH_STORE_ALL,  /* stores every argument among them */
  BSH_STORE_TAIL, /* stores every argument but the first (insert, fill) */
  BSH_MERGE,      /* stores the elements of its container arguments; answers
                     the receiver or a container of both */
  BSH_ARGS,       /* answers its one argument, or an Array of several (p) */
  BSH_LAST,       /* answers its last argument without retaining it */
  BSH_ARRAY_OF,   /* answers its Array argument, or an Array holding it (Array()) */
  BSH_FILL1,      /* writes into its second argument in place (IO#read(n, buf)) */
  BSH_FILL2,      /* writes into and answers its third argument (IO#pread) */
  BSH_PACK,       /* writes into and answers its buffer keyword, when given */
  /* the iterators' answers, BSH_ITER to BSH_ITER_THEN, stay together
     (iter_rows_check reads the run) */
  BSH_ITER,       /* block parameters bind elements; answers the receiver (each) */
  BSH_ITER_SEL,   /* block parameters bind elements; answers a container of
                     some of them (select, sort_by) */
  BSH_ITER_MAP,   /* block parameters bind elements; answers a new container
                     of the block's values (map) */
  BSH_ITER_MAP_BANG, /* the same, keeping the values in the receiver (map!) */
  BSH_ITER_SUB,   /* block parameters bind containers of elements (each_slice,
                     group_by's groups); answers containers of them */
  BSH_ITER_FIND,  /* block parameters bind elements; answers one of them */
  BSH_ITER_FRESH, /* block parameters bind fresh values (each_char, each_line) */
  BSH_ITER_FRESH_RECV, /* the same, answering the receiver (gsub!, sub!) */
  BSH_ITER_MEMO0, /* inject/reduce: parameter 0 the memo (argument 0), the
                     others elements; answers the memo or the block's value */
  BSH_ITER_MEMO1, /* each_with_object: parameter 1 the memo (argument 0) */
  BSH_ITER_SELF,  /* tap: parameter 0 the receiver; answers the receiver */
  BSH_ITER_THEN,  /* then: parameter 0 the receiver; answers the block's value */
  BSH_FETCH,      /* answers an element, or its last argument (fetch's default) */
  BSH_ELEM_N,     /* answers an element, or with a count a container of them
                     (first, last, pop, shift, sample) */
  BSH_CALL,       /* calls a proc or a Method with its arguments */
  BSH_METHOD_REF, /* makes a Method (or defines one) of the method its first
                     argument names: that method is called from anywhere */
  BSH_IVAR_GET,   /* answers the ivar its first argument names */
  BSH_IVAR_SET,   /* stores its second argument in the ivar its first names */
  BSH_EXEC,       /* runs its block with its arguments (instance_exec) and
                     answers the block's value */
  BSH_NEW,        /* constructs: its arguments go to initialize */
  BSH_FLATTEN,    /* answers a container of the receiver's elements and of
                     its nested containers' elements, at any depth (flatten) */
  /* the constructors (BOP_CLASS_NEW): */
  BSH_NEW_FILL,   /* a container of its second argument and of its block's
                     values (Array.new(n, s), Array.new(n) { }); also holds the
                     elements of an Array first argument (Array.new(a)) */
  BSH_NEW_DEFAULT, /* a container of its default argument and of its block's
                     values; the block is handed the container and each key a
                     lookup asks for (Hash.new) */
  BSH_NEW_YIELDER, /* a container of what its block hands its first parameter
                     (Enumerator.new's yielder) */
  BSH_NEW_FIELDS, /* a container of its Hash argument's values (OpenStruct.new) */
  BSH_SUM,        /* the initializer, receiver elements and block values can
                     contribute to the answer; the block takes elements */
  BSH_FILL,       /* fill stores arguments, or its block's values */
  BSH_QUERY,      /* an element query: a bare Enumerator retains the receiver */
  BSH_SUBST,      /* a String substitution yields its pattern, returns a fresh String */
  BSH_SUBST_BANG, /* the same, answering the receiver */
  BSH_LINE,       /* line iteration may yield the receiver for a nil separator */
  BSH_BLOCK,      /* answers its block's value; yields a fresh value */
  BSH_EMPTY_SELF, /* answers an empty receiver itself, otherwise a new String */
  BSH_UNKNOWN     /* a mixed identity contract, conservatively untracked */
} BopShare;

/* The BSH_* of `name` on receiver family fam (TY_STRING, BOP_ANY_ARRAY,
   BOP_ANY_HASH, TY_IO, BOP_KERNEL, BOP_ANY_RECV, or a scalar kind), the
   family's default row's when the name has none, or 0 when the family has
   no default either. */
int bop_share(TyKind fam, const char *name);
int builtin_ops_share_check(void);
/* Exact name lookup, including iterator contracts. */
int bop_share_named(TyKind fam, const char *name);
/* The distinct share kinds of `name` across Object's row and the String,
   Array and Hash rows, whichever of those the receiver turns out to be:
   written to out[0..max), their count answered, 0 when no row names it. */
int bop_share_kinds(const char *name, int *out, int max);
/* Hand and iterator contracts only: generated observations cover typed receivers. */
int bop_share_boxed(TyKind fam, const char *name);
/* A String method whose value is its receiver, or nil: a bang method that
   answers it, or nil when it changed nothing (`strip!`, `gsub!`), and,
   given a block, an iterator that answers it (`each_char`, `scan`, `tap`). */
int bop_share_self_answer(const char *name, int has_block);

/* ---- What a builtin iterator yields to its block, and answers (#6765) ----
   One row per receiver family, name and run of argument counts: where each
   value a step hands the block comes from, and what the call answers.
   Three readers take their facts from these rows: the share analysis
   (bop_share's BSH_ITER_* answers), the block-shape desugar
   (iter_shape_count) and ty_block_yield, which the forwarded-&callable
   desugar and the boxed-element widening (analyze_pass.c
   widen_boxed_elem_sources) ask.
   The families are TY_STRING, BOP_ANY_ARRAY, BOP_ANY_HASH, TY_RANGE,
   TY_FLOAT_RANGE, TY_INT, TY_FLOAT, BOP_KERNEL and BOP_ANY_RECV. */
typedef enum {
  YS_NONE,
  YS_ELEM,      /* an element of the receiver */
  YS_PAIR_KEY,  /* a Hash's key: the frozen copy it stored of a String */
  YS_PAIR_VAL,  /* a Hash's value */
  YS_PAIR,      /* a Hash's [key, value] pair, as one value */
  YS_SUB,       /* a run of the receiver's elements (each_slice, combination) */
  YS_TUPLE,     /* a tuple of an element and the arguments' (zip, product) */
  YS_FRESH,     /* a new String each step (each_char, a gsub match) */
  YS_NUM,       /* a number the step computes (times, step, each_byte) */
  YS_INDEX,     /* an element's index */
  YS_MEMO,      /* the accumulator (inject, each_with_object) */
  YS_RECV,      /* the receiver itself (tap, then) */
  YS_ARG0       /* the first argument (catch's tag) */
} IterYield;
typedef enum {
  IA_RECV,              /* the receiver (or nil, for a bang that changed nothing) */
  IA_SOME,              /* a new container of some of the elements (select, sort) */
  IA_ONE,               /* one of the elements (find, min_by) */
  IA_BLOCKVALS,         /* a new container of the block's values (map) */
  IA_BLOCKVALS_INPLACE, /* the receiver, holding the block's values (map!) */
  IA_BLOCKVAL,          /* the block's value (then) */
  IA_MEMO,              /* the accumulator */
  IA_GROUPS,            /* a Hash of containers of the elements (group_by) */
  IA_PARTS,             /* containers of the elements (partition, minmax) */
  IA_FRESH,             /* a new String (gsub) */
  IA_OTHER              /* a value the call computes (count, any?) or nil */
} IterAnswer;
/* Legacy coverage, to be cleared: today's readers do not all read every row
   (builtin_ops.c lists the gaps at the table). A PR that closes a gap clears
   its bit, with a test for the C it changes. */
#define IRF_GAP_SHARE     1   /* the share analysis does not read the row */
#define IRF_GAP_SHAPE     2   /* the block-shape desugar does not read it */
#define IRF_GAP_FWD       4   /* ty_block_yield does not: the forwarded-
                                 &callable desugar, and the boxed-element
                                 widening, which asks it whether an Array
                                 iterator's first parameter is an element */
#define IRF_GAP_RUN_BOXED 8   /* the block-shape desugar types the run a step
                                 yields as a boxed Array, not the receiver's kind */
#define IRF_SHAPE_BOXED  16   /* the block-shape desugar reads the row on a
                                 boxed receiver too (of the Array and Hash
                                 rows, only these) */
typedef struct IterRow {
  TyKind fam;
  const char *name;
  signed char argc_min, argc_max;
  unsigned char nyield;     /* values per step */
  unsigned char yield[3];   /* IterYield per position */
  unsigned char answer;     /* IterAnswer */
  unsigned char flags;      /* IRF_* */
} IterRow;

/* The first row of family fam for `name` that takes argc arguments (any
   count when argc < 0) and carries none of the flags in `skip`, or NULL. */
const IterRow *iter_row(TyKind fam, const char *name, int argc, unsigned skip);
/* Has some receiver family a row for `name`: is it a builtin iterator,
   one that yields to its block before it answers? (The load-phase record
   asks it of a block whose receiver is not typed yet.) */
int iter_row_named(const char *name);
/* Does Object's builtin of `name` run its block before it answers, on
   any receiver: an any-receiver iterator row of a name Object's surface
   answers (tap, then, yield_self)? */
int object_iter_runs_now(const char *name);
/* The iterator row a call with a block reads on a receiver of kind rt:
   any receiver's (tap, then, each_with_object), else its family's (an
   Array's, a Hash's, a Range's, ...), or NULL. */
const IterRow *iter_row_of(TyKind rt, const char *name, int argc);
/* The kind the value at position k of a step has, on a receiver of kind rt
   (TY_POLY: a boxed receiver, whose elements are boxed). */
TyKind iter_yield_kind(const IterRow *r, int k, TyKind rt);
/* The block-shape desugar's reading (analyze_desugar.c): how many values the
   builtin iterator `nm` yields its block per step (0: not one it handles),
   the kind a single yielded value has, and whether that value is a Hash's
   [key, value] pair. */
int iter_shape_count(TyKind rt, const char *nm, int argc, TyKind *elem, int *hash_pair);
/* --plan-check: the rows are well formed, no two rows of a family and name
   take the same argument count, and every hand share row with an
   iterator's answer overrides a row the share analysis reads with another
   answer. Prints a "plan-check: iter-row-error" line per fault. */
void iter_rows_check(void);

#endif
