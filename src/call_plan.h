/* call_plan.h -- the user method a call binds, resolved from the tables.

   cplan_user(c, id) answers which user method the call node `id` reaches
   -- the method scope, the class whose chain was searched, the arm kind
   (UC_*, compiler.h) and whether the class tables already decide the
   dispatch (one method, or a switch over overrides) -- from the scope and
   class tables and the settled node types alone. It never runs inference
   and never emits, so asking it changes nothing (#7100, Phase B).

   A plan is memoized per node, but only where the node is read as itself:
   no view, no instance_exec scope move or class, no inline splice. Inside
   one of those it is computed afresh and not kept. Nothing reads it to
   emit yet; --plan-check compares it with inference's record and with the
   binding codegen made. */
#ifndef SPINEL_CALL_PLAN_H
#define SPINEL_CALL_PLAN_H

#include "compiler.h"

typedef enum {
  CP_NONE,      /* no user method */
  CP_DIRECT,    /* one method, whatever the receiver's runtime class */
  CP_SWITCH,    /* a switch on the runtime class: a descendant has its own
                   implementation, or a boxed receiver */
  CP_PER_ARM,   /* a switch whose arms take the call's arguments differently,
                   so each arm lays them out for itself (an instance dispatch:
                   dispatch_arms_disagree) */
  CP_REFUSE     /* the compiler refuses the call (cplan_refuse): rkind, msg */
} CplanDispatch;

/* what a refusal says the call is */
typedef enum {
  CR_NONE,
  CR_FEATURE,    /* a documented limit (unsupported_feature) */
  CR_NOMETHOD,   /* the program's NoMethodError, proved at compile time */
  CR_NAMEERROR,  /* the program's NameError (an undefined bare name) */
  CR_GAP         /* a codegen gap (unsupported) */
} CplanRefuse;

/* which decision refuses it, in the order codegen meets them: the prepasses
   before any emission, then the emitters' own */
typedef enum {
  CRF_NONE,
  CRF_SEND,        /* a send with a runtime name (reject_runtime_send) */
  CRF_CONST_GET,   /* a const_get with a runtime name (reject_runtime_const_get) */
  CRF_BINDING,     /* a bare binding (reject_binding) */
  CRF_LIMIT,       /* a documented limit (diagnose_unsupported_call) */
  CRF_EVAL,        /* Kernel#eval of a runtime string (diagnose_eval_call) */
  CRF_IO_REOPEN,   /* an IO reopening that yields (emit_io_reopen_call) */
  CRF_BIND_CALL,   /* a bind_call on a Class value with an armless class */
  CRF_TOP_SELF     /* a receiverless call in a top-level def to a name main has a singleton method for (diagnose_unsupported_call) */
} CplanRefuseFrom;

typedef struct {
  int mi;                  /* the method scope, or -1 */
  int method_node;         /* bound Method source node, or -1 */
  int send_fallback;       /* boxed send: top-level def if no class arm, or -1 */
  short owner_ci;          /* the class whose chain was searched, or -1 */
  unsigned char via;       /* UC_* */
  unsigned char dispatch;  /* CplanDispatch */
  unsigned char by_name;   /* a switch over every class with a class method of
                              the name (a class held in a variable) */
  unsigned char chain;     /* mi is the receiver class's own lookup of the
                              name (comp_method_in_chain(owner_ci, name)), not
                              a method reached another way (an operator's
                              stand-in, a subclass's override of a reader, a
                              reopen) */
  unsigned char method_call; /* a Method invocation, even with no source node */
  unsigned char rkind;     /* CP_REFUSE: CplanRefuse */
  const char *msg;         /* CP_REFUSE: the message, as the compiler prints it */
  unsigned char rfrom;     /* CP_REFUSE: CplanRefuseFrom */
  const char *delegate;     /* the protocol method name, before alias resolution */
} CallPlan;

int cplan_accessor_classes(Compiler *c, int id, int *out, int cap);
int cplan_super_chain_mi(Compiler *c, int id, Scope *s, int par, const char *name);
/* ...and the method `super` in s reaches (its prepend chain's next, else
   that), *at the class it is found from */
int cplan_super_target(Compiler *c, int id, Scope *s, int *at);
/* A conversion performed by a helper, separately from the enclosing call. */
CallPlan cplan_conversion(Compiler *c, int ci, const char *name);
CallPlan cplan_method_call(Compiler *c, int id);
CallPlan cplan_send_arm(Compiler *c, int id);
CallPlan cplan_exception_render(Compiler *c, int id, int ci);
const CallPlan *cplan_user(Compiler *c, int id);
/* The method scope call `id` runs when it asks for `name`: on cls's
   instance side through its chain, CT_CMETHOD on its class side, CT_OWN
   in cls's own table only, CT_TOP among the top-level functions (cls not
   read; main's singleton methods first for a call on main); *def_class
   (when asked) the class whose table holds it; -1 for none. The one place a site asks "what does this call run": it answers
   from the tables by name (a redefined method's entry is its last
   definition), and binding a call to one of a method's versions (#9898)
   is decided here. `id` is -1 where a helper that serves calls is asked
   with no call (class_reopen_cmethod for a respond_to? test): the name's
   last definition. A question about a name rather than one call (is it
   defined, which classes answer it) and a method the language calls by a
   fixed name (`initialize`, `<=>`) ask the tables directly. A later
   prepend's general guard is that table answer; the copy has a shadow
   name reached only by arms and captures. */
/* CT_REACH asks the same lookup for an explicit route such as super.
   CT_CAPTURE selects the definition live at construction, or its entry
   when the constructor must select an arm at run time. */
enum { CT_CMETHOD = 1, CT_OWN = 2, CT_TOP = 4, CT_REACH = 8, CT_CAPTURE = 16 };
/* The names call node id itself calls, into out: a call's or an attribute
   target's name, a `recv.m op= v`'s reader and writer; how many (0 for any
   other node). call_target binds a call of one of them to the method
   version it runs; a method the language calls for the node keeps the
   last version. Super contributes the enclosing method's original name. */
int call_node_names(Compiler *c, int id, const char *out[2]);
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, int *def_class);
/* the classes a call of a versioned name (ct_version_named) may run on,
   each handed to fn with the flags call_target is asked with */
int ct_version_named(Compiler *c, const char *nm);
int ct_version_marked(Compiler *c, const char *nm);
void ver_hold_free(Compiler *c);
void ver_chain_free(Compiler *c);
/* the method call id reaches when it can reach only main's own method of
   name, -1 for none (a call on main where main's method is defined) */
int ct_main_target(Compiler *c, int id, const char *name);
typedef void (*CtClassFn)(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg);
void ct_version_classes(Compiler *c, int id, const char *nm, CtClassFn fn, void *arg);
/* the version entries a call whose version is known only at run time
   reaches, and the run-time cells they read (#9898) */
void ver_entries_build(Compiler *c);
void ver_prepend_cell_name(int cell, char out[32]);
/* `cell = v` put in statement list `list` at its statement `at` (after it
   when after): the write, for the caller to record (elab_record_write) */
int ver_cell_put(Compiler *c, const char *cell, int list, int at, int after, int v);
void ver_cells_write(Compiler *c, int from);
int ver_entries_demand(Compiler *c);
/* the methods entry scope e's arms call, its else arm's last: at most
   VER_MAX_ARMS + 1; an open history's else arm, which calls none, as its
   last version (the method a Method object of it captured before) */
enum { VER_MAX_ARMS = 64 };
int ver_entry_targets(const Compiler *c, int e, int *out, int *tests);
int cplan_bool_sides(Compiler *c, int id, int *tmi, int *fmi);
void cplan_check_missing(Compiler *c, int id, int mi, int ci, const char *name, int cmeth, const char *site);
/* The same plan resolved afresh and never kept, for a reader that runs in
   the analysis, before the memo is codegen's to fill (the nil facts,
   analyze_nil.c): it answers from the types as they stand. The answer
   lasts until the next call. */
const CallPlan *cplan_user_fresh(Compiler *c, int id);
const CallPlan *cplan_const_user(Compiler *c, int id, int recv, int fresh);
/* A constant receiver's builtin new reaches this initialize, or -1. */
int cplan_initialize(Compiler *c, int id);
int cplan_initializers(Compiler *c, int id, int *out, int cap);
/* Every user method the call node id may reach, into out (at most cap):
   its plan's method and, for a switch, each member. Answered from the types
   as they stand, never kept past cplan_targets_drop.
     n >= 0      the methods, 0 for none (a builtin, a CP_REFUSE plan, a
                 node that is no call): follow nothing
     CPT_UNKNOWN too many members (more than cap or CPT_MAX), or a call
                 whose receiver type is not settled: the caller must treat
                 the call as reaching code it cannot see, never as a
                 shorter list
   The answers are held until cplan_targets_drop, which the caller calls
   when a type may have changed (once per round of a fixpoint); a held
   answer is not carried across a drop, because a later round may widen a
   receiver and add members, and nothing here promises a set only grows. */
enum { CPT_UNKNOWN = -1, CPT_MAX = 64 };
int cplan_targets(Compiler *c, int id, int *out, int cap);
/* cplan_targets, for a reader that wants the code the call's emission
   names (reachability): the same answer, but for a call on a boxed receiver
   the methods of the arms its dispatch has, which the receiver's class
   facts bound, not every class's method of the name; and for a send on one,
   the top-level method it falls back to (send_fallback). */
int cplan_emitted_targets(Compiler *c, int id, int *out, int cap);
/* Why cplan_targets answers CPT_UNKNOWN for call id: its members are more
   than cap (CPT_WHY_CAP), or its receiver is of a type the analysis did not
   settle (CPT_WHY_RECV_UNTYPED) -- the two ct_resolve gives up on, whose
   members are still the program's methods of the call's name or a
   builtin's. CPT_WHY_NONE where it lists the members. */
enum { CPT_WHY_NONE = 0, CPT_WHY_CAP, CPT_WHY_RECV_UNTYPED };
int cplan_targets_unknown_why(Compiler *c, int id, int cap);
void cplan_targets_drop(void);
/* Counts the drops (and a new compiler's first ask): an answer held at one
   count is the answer while it stands. */
unsigned cplan_targets_drops(void);
/* Can target method mi of call id bind the call's arguments? 0 only when
   their count is known at compile time -- plain positional arguments and a
   trailing keyword hash, no splat or `...` -- and mi's parameters refuse
   them (poly_arm_count, whose keyword plan judges the hash): that target
   can only raise ArgumentError, and answers nothing. */
TyKind cplan_poly_operator_ret(Compiler *c, int id);
int cplan_target_binds(Compiler *c, int id, int mi);
/* Can method mi run with argc plain positional arguments (no keyword hash)?
   A def the program writes says (cplan_target_binds' judgement); anything
   else -- a define_method block, an accessor, a wrapper the compiler made --
   takes every count. */
int cplan_def_takes_argc(Compiler *c, int mi, int argc);
/* Is call id a class body's call on self (receiverless or `self.`) that
   the plan binds to one method of the body's class (UC_CMETH, CP_DIRECT)?
   That binding is the one method the call runs, so reachability follows it
   instead of the name: a same-name method the class's lookup does not find
   there (a module method a singleton def shadows) is no target of it. */
int cplan_cbody_direct(Compiler *c, int id);
int cplan_boxed_cmethod(Compiler *c, int id, const char *name);
/* Object fallback behind a class-gated exception accessor, or -1. */
int cplan_exc_object_method(Compiler *c, int call, const char *name);

/* ---- CP_REFUSE: a call the compiler refuses ----
   Whether codegen refuses the call node id, and in which words, decided
   from the node, scope and class tables and the settled types alone. The
   plan names the node the refusal is reported at: a refusal codegen
   raises for an outer call at its receiver is the receiver's plan. The
   answer is dispatch CP_REFUSE with rkind, rfrom and msg, or CP_NONE (no
   refusal the plan can decide; codegen may still refuse on its own state).
   It never runs inference and never emits. Kept per node where the node is
   read as itself; elsewhere computed afresh, and its msg lasts until the
   next such call. Codegen raises the plan's refusal where it meets it;
   --plan-check holds the plan against the refusals codegen reports
   (refuse_observe, codegen_util.c). */
const CallPlan *cplan_refuse(Compiler *c, int id);
/* the documented-limit family: the node's message or NULL; *stop 0 when
   a limit down the receiver chain is the one to report */
const char *cplan_feature_why(Compiler *c, int id, int *stop, char *buf, size_t cap);
int cplan_define_method_early(Compiler *c, int id);

/* The plan of the same call read in a context the node does not carry
   itself: its self, or its receiver, is an instance of self_ci. That is an
   instance_exec self (CPX_IE), a body emitted for an inheriting class
   (CPX_EMIT), or a poly arm's receiver class (CPX_ARM). The plan is that
   class's own lookup of the name (chain, UC_INST, a switch when a
   descendant overrides it); a miss is no plan, except under CPX_IE, where
   the call resolves as its own (the instance_exec receiver is asked first).
   It is computed afresh and never kept: only cplan_user memoizes. A
   self_ci < 0 is the node's own context, cplan_user. */
/* CPX_TARGET reads only the direct chain target under its name view, respecting builtin-only aliases. */
enum { CPX_IE = 1, CPX_EMIT = 2, CPX_ARM = 4, CPX_TARGET = 8 };
const CallPlan *cplan_user_in(Compiler *c, int id, int self_ci, int flags);
/* Is the node codegen is at read as itself: no view, no instance_exec scope
   move or class, no inline splice? Only there may a memo per node answer
   (cplan_user's, the String plan's in share_plan.c). */
int cplan_plain_ctx(void);
/* --plan-check: a codegen site that took its target from a plan counts it
   (site: a short constant name); cplan_served_report prints the counts */
void cplan_served(const char *site);
void cplan_served_report(void);

/* The form call `call`'s dispatch of instance method `name` on class cid
   takes (call -1: a dispatch no call makes): CP_DIRECT for one
   implementation, CP_SWITCH when cid's subtree has more than one (or any,
   without a base method: has_base 0), CP_PER_ARM for a switch whose arms
   disagree on the argument layout; CP_NONE for neither a base method nor a
   descendant's. */
/* Whether a descendant defines its own method of this name. */
int cplan_overridden(Compiler *c, int cid, const char *name, int cmeth);
int cplan_dispatch_form(Compiler *c, int call, int cid, const char *name, int has_base);

/* The most candidate classes the statement-level block dispatch takes
   (poly_block_dispatch_cands refuses a program with more). */
enum { CPB_MAX_CANDS = 64 };

/* The methods a boxed native-block call's statement-level dispatch reaches
   (each arm's method and its proc form), asked once for a call and kept
   here: a caller testing many methods against one plan owns one, zeroed
   in `state`, and passes it to every cplan_virtual_member, so the poly-block
   plan is built once, not once per method. It holds copies: the plan
   cplan_poly_block answers is replaced by that function's next call. */
typedef struct {
  int state;       /* 0: not asked, 1: no native-block dispatch, 2: mi[0..n), 3: too many to hold, ask the plan */
  int n;
  /* a method and its proc form for each candidate's arm, and room for
     the two a default or trial arm would add if one ever had a method */
  int mi[2 * CPB_MAX_CANDS + 2];
} PolyBlockMembers;

/* whether mi is the plan's method or, for a switch, one of its arms; blk
   is the caller's PolyBlockMembers for one plan tested against many
   methods, or NULL for a single test */
int cplan_virtual_member(Compiler *c, int id, const CallPlan *p, int mi, PolyBlockMembers *blk);

/* ---- CP_POLY: the arms of a dispatch on a boxed (poly) receiver ----
   One arm per runtime class (or, later, builtin kind) the switch can take:
   what answers there, the arm's value type, and how that value reaches the
   call's own type. Nothing reads the list to emit yet; --plan-check holds
   it against the arms emit_poly_method_dispatch writes (pa_begin /
   pa_observe / pa_end, codegen_poly_plan.c). */
typedef enum {
  PA_USER,        /* the class's method */
  PA_PROC_FORM,   /* a yielding method, through its proc-form clone */
  PA_READER,      /* an attr reader's ivar load */
  PA_NATIVE,      /* a native class's C binding */
  PA_ARITY,       /* the call's count is refused: ArgumentError */
  PA_VIS,         /* the target's visibility refuses the explicit receiver: NoMethodError */
  PA_SYNTH_ENUM,  /* a Struct's synthesized each/each_pair: an Enumerator */
  PA_STRUCT_SET,  /* a Struct's builtin member write */
  PA_BUILTIN,     /* a builtin value's arm (key PA_KEY_BUILTIN + its PolyFamily) */
  PA_TRIAL        /* an arm only an emission can decide: the call re-entered as the
                     builtin it is, kept unless it raises (key PA_KEY_TRIAL + its
                     PolyTrial); the plan offers it, codegen observes the outcome
                     in conv (1 kept, 0 dropped; the generic default's tail: what
                     answered) */
} PolyArmKind;

/* the trial arms */
typedef enum {
  PT_STR,             /* a String in a slot a user class's name owns (emit_poly_str_prearm) */
  PT_CONTAINER,       /* a container read, zero arguments (emit_poly_cases0) */
  PT_ARRAY_FALLBACK,  /* the Array transforms' default, for another builtin */
  PT_DEFAULT0,        /* the builtin surface, the zero-argument dispatch's last default */
  PT_GENERIC_TAIL,    /* the generic default's block arm, element assignment, builtin surface */
  PT_DEFAULT_N,       /* the builtin surface, the last default with arguments */
  PT_BD_DEFAULT       /* the block dispatch's default: a String or IO iterator (1), the
                         builtin surface (2), or the raise (0) */
} PolyTrial;

/* The builtin arm families of a poly dispatch, each one the arm (or run
   of arms) one condition in emit_poly_method_dispatch writes. */
typedef enum {
  /* the tag pre-arms of a zero-argument dispatch (emit_poly_prearms0) */
  PB_LEN, PB_EMPTY, PB_CLASS_NAMED, PB_CLASS_REFLECT, PB_OSTRUCT, PB_TO_A, PB_IO_REWIND,
  PB_IO_PUTS, PB_IOZ, PB_REDUCE, PB_INT_CHR, PB_STRT, PB_SPLIT,
  /* ... and the rest of its pre-arms (emit_poly_prearms0_blk) */
  PB_ENUM_PROC, PB_SYNC, PB_CALLABLE, PB_CLS_MEMBERS,
  /* its builtin cases after the class arms (emit_poly_cases0) */
  PB_LEN_CASES, PB_CLEAR, PB_EMPTY_CASES, PB_CMP_BY_ID,
  /* its builtin `default:` arms, the first that applies, and the named
     cases after them (emit_poly_defaults0) */
  PB_D_ENUM_EACH, PB_D_TO_S, PB_D_CASE_CONV, PB_D_NUM, PB_D_DIGITS, PB_D_ARRAY_TRANSFORM, PB_D_PRED,
  PB_D_TO_IF, PB_D_ANY_NONE, PB_D_TO_H, PB_D_DISPLAY, PB_D_HASH,
  PB_N_EACH_INDEX, PB_N_JOIN, PB_N_ALIVE, PB_N_KILL, PB_N_STATUS, PB_N_QUEUE, PB_N_IO_READ,
  PB_N_IO_FLUSH, PB_N_IO_CLOSE, PB_N_ENUM_TO_A,
  /* the tag pre-arms of a dispatch with arguments (emit_poly_prearms_n) */
  PB_COVER, PB_TRY_CONVERT, PB_GCDLCM, PB_UNPACK1, PB_INCLUDE, PB_STR_DELETE, PB_STR_PARTITION,
  PB_STR_SETOP, PB_STORE, PB_STR_ENCODE, PB_STR_SPLIT_N, PB_INT_BITREF,
  /* a String's element store, ahead of the switch (emit_poly_str_aset_prearm) */
  PB_STR_ASET,
  /* its builtin cases after the class arms (emit_poly_cases_n) */
  PB_INDEX_CASES, PB_IO_READ_NB, PB_IO_READPARTIAL, PB_IO_READPARTIAL_BUF, PB_IO_SETSOCKOPT, PB_IO_WRITE, PB_IO_SYSWRITE, PB_IO_PRINT, PB_IO_PUTC, PB_IO_SEEK_READ,
  PB_UNSHIFT, PB_PUSH, PB_PACK, PB_JOIN_N, PB_INCLUDE_CASES, PB_ARR_INDEX, PB_INTERSECT, PB_STRFTIME,
  PB_AREF_STR, PB_AREF_SYM, PB_AREF_POLY, PB_PRED_N,
  /* its `default:` arm (emit_poly_defaults_n): the generic one and what it
     answers first, or a name's own */
  PB_ND_GENERIC, PB_ND_REPLACE, PB_ND_ROUND, PB_ND_NUM, PB_ND_STR_INDEX, PB_ND_FIND_INDEX,
  PB_ND_FIRSTN, PB_ND_KEYS, PB_ND_MERGE, PB_ND_AREF2, PB_ND_AREF,
  /* the statement-level block dispatch's builtin default
     (emit_poly_recv_block_dispatch): map!/collect! over a builtin array */
  PB_BD_MAP_BANG,
  /* a call on an object whose static class cannot answer the name while a
     subclass can: re-entered with the receiver boxed (#4023) */
  PB_SUBDISPATCH,
  /* a sort_by, comparator, grouping or zip call re-entered as an Array call
     over the receiver's elements (emit_array_call, #2935), answering the
     receiver itself for the block forms of each_slice and its kin */
  PB_REDISPATCH, PB_REDISPATCH_RECV,
  /* a String value-form mutator through the face table (emit_face_str_bang) */
  PB_FACE_STR_BANG,
  PB_IO_TIMEOUT,
  /* a boxed receiver's `each { }` beside a program's own each: the kinds
     PB_ENUM_PROC serves walk inline at the call (cplan_each_split) */
  PB_ENUM_INLINE,
  PB_NFAMILIES
} PolyFamily;

typedef enum {
  PC_SAME,        /* the value as it is */
  PC_BOX,         /* boxed into a poly result */
  PC_UNBOX,       /* a poly value unboxed into a scalar result */
  PC_VOID,        /* no value (a void method, a raise) */
  PC_NUM,         /* a Bignum converted into an Integer or Float result */
  PC_COPY,        /* a shared-mutable String copied into a String result */
  PC_BOX_OR_NIL,  /* an Integer ivar boxed with its nil (its nil byte) */
  PC_BOX_HANDLE   /* emission observation: PC_BOX retains the String handle */
} PolyConv;

typedef struct {
  unsigned char kind;   /* PolyArmKind */
  unsigned char conv;   /* PolyConv */
  unsigned char vty;    /* the arm's value TyKind */
  short key;            /* the runtime class id, or PA_KEY_DEFAULT */
  short def;            /* the class the arm reads from: the method's defining
                           class (an exception reopening's for its definer), the
                           reader's ivar class; -1 for others */
  unsigned char vis;    /* a PA_VIS arm's SP_VIS_*: the visibility that refuses */
  int mi;               /* the user method scope, or -1 */
} PolyArm;

/* the switch's `default:` arm (any receiver no class arm took) */
#define PA_KEY_DEFAULT 0x7fff
/* a builtin family's arm: above every class id */
#define PA_KEY_BUILTIN 0x4000
/* a trial arm */
#define PA_KEY_TRIAL 0x6000
/* a face owner's arm (the face table, types.h): key PA_KEY_FACE + the
   owner bit's index (face_kind_index); a probe, so a trial, or an
   argument's misfit TypeError (PA_BUILTIN) */
#define PA_KEY_FACE 0x5000
static inline int face_kind_index(unsigned kind) {
  int i = 0;
  while (kind > 1) { kind >>= 1; i++; }
  return i;
}
/* a class value's class-side arm, by its class (the pre-arm ahead of the
   instance switch) */
#define PA_KEY_CLASS_VALUE 0x2000

/* how the dispatch holds its receiver and keys its switch */
enum {
  PPF_ROOT     = 1,   /* the receiver temp is rooted: an arm can allocate */
  PPF_DEREF    = 2,   /* read through sp_poly_strbuf_deref: a String arm reads the value */
  PPF_KEY_CLS0 = 4,   /* class 0 has an arm: a non-object value is kept off it */
  PPF_KEY_PRIM = 8,   /* a primitive reopening has an arm: the tag maps to its class */
  PPF_KEY_EXC  = 16,  /* a user exception class answers: the exception is re-keyed */
  PPF_KEY_CLSV = 32,  /* a class whose Class object may be a value has an arm: a Class
                         value, which carries its class's index, is kept off it */
  PPF_SEEN     = 256  /* (--plan-check) the flags were observed */
};

typedef struct PolyPlan_s {
  TyKind ret;           /* the call's type the arms answer into */
  TyKind ntype;         /* the node's type it was resolved for */
  unsigned long edits;  /* comp_ty_edits when it was resolved: the arms read
                           the receiver's type and the candidates' fields,
                           which change with the tables, not with the node */
  unsigned flags;       /* PPF_* */
  int native_block;     /* a block plan contains a native Ruby user arm */
  int full;             /* every arm, not only the class arms (cplan_poly_arms) */
  int n;
  PolyArm *arm;
} PolyPlan;

/* The user-class arms of a call on a poly receiver, with or without
   arguments, the Object reopening's default arm, and how the receiver is
   held and keyed (the plan's slice of emit_poly_method_dispatch so far).
   Pure; kept per node where the node is read as itself. */
const PolyPlan *cplan_poly(Compiler *c, int id);
/* The same plan, as far as emission reads it: the class arms (and the
   receiver form). Cheaper: the builtin families and trials, which only the
   --plan-check shadow compares, are left out. */
const PolyPlan *cplan_poly_arms(Compiler *c, int id);
/* Resolve the complete plan without reading or writing the per-node memo.
   Use while inference facts are still moving; the scratch plan is overwritten
   by the next fresh resolve, so finish reading it before another planner call. */
const PolyPlan *cplan_poly_fresh(Compiler *c, int id);
int cplan_struct_aset(Compiler *c, int cid, const char *name, int argc);
/* A plan the caller keeps across emissions that may resolve others (a
   resolve outside the memo reuses one buffer); cplan_poly_free drops it. */
PolyPlan *cplan_poly_copy(const PolyPlan *p);
void cplan_poly_free(PolyPlan *p);
/* The arms of the statement-level block dispatch (#2448) of the same call:
   each candidate class's method spliced with the block, and its default.
   Computed afresh: the same node may take this dispatch or the method
   dispatch. */
/* The name index bounds a query for a native Ruby literal-block target. */
int cplan_native_block_possible(Compiler *c, int id, const PolyCand *pc, int n);
int cplan_cand_target(Compiler *c, int id, const char *name, const PolyCand *pc, unsigned flags);
const PolyPlan *cplan_poly_block(Compiler *c, int id);
/* A boxed receiver's `each { }` that the run-time dispatch serves because a
   class of the program takes a block for `each`: one arm, PB_ENUM_INLINE,
   when the call splits on the receiver's kind, the builtin kinds walking
   inline and any other value taking the dispatch; none when it does not. */
const PolyPlan *cplan_each_split(Compiler *c, int id);
/* The arms of the class-method tag switch of a call on a poly receiver
   that may hold a Class (emit_unresolved_call): each class's class method
   of the name, or its proc form. Computed afresh. */
int cplan_cmethod_arm(Compiler *c, int call, int k, const char *nm, int *dc, int *pfk);
const PolyPlan *cplan_poly_cmeth(Compiler *c, int id);
int cplan_visibility_class(Compiler *c, int id, int ci);
int cplan_has_method_missing(Compiler *c, int id, int ci, int cmethod);
int cplan_cmethod_visible(Compiler *c, int id, int ci, const char *name);
TyKind cplan_poly_arg_storage(Compiler *c, int id);
/* The re-entry of a call on an object whose static class cannot answer the
   name while one of its subclasses can (#4023). Computed afresh. */
const PolyPlan *cplan_poly_sub(Compiler *c, int id);
/* The re-entry of a call on a poly receiver as an Array call (#2935).
   Computed afresh. */
const PolyPlan *cplan_poly_redispatch(Compiler *c, int id);
/* The face arms of a call on a poly receiver (emit_poly_call): one owner's
   re-entry, a switch over several, or a String's value-form mutator.
   Computed afresh. */
const PolyPlan *cplan_poly_face(Compiler *c, int id);

/* --plan-check: the arms one emitted switch wrote, held against the plan.
   pa_resume(frame) drops frames a probe abandoned above it, before the
   switch observes again. */
int  pa_begin(int id);
int  pa_current(void);
void pa_resume(int frame);
void pa_drop(int frame);     /* a dispatch that declined after opening its frame */
void pa_flags(unsigned flags);
void pa_box_text(TyKind ty);
void pa_observe(int kind, int key, int mi, TyKind vty, int conv);
/* the same, only into node id's own frame: for a helper more than one
   dispatch shares */
void pa_observe_at(int id, int kind, int key, int mi, TyKind vty, int conv);
void pa_end(Compiler *c, int frame, const PolyPlan *p);
void pa_report(void);

/* ---- CN_*: a call's nil target (#7444) ----
   Whether call id's receiver may be nil where the call is emitted for a
   builtin of its type, and what nil answers there. Decided from the
   settled types, the representation (repr_of) and the nil fact
   (analyze_nil.c) alone; pure. A receiver qualifies when it is a typed
   pointer of a String, an Array, a Hash or an IO, and the fact says it may
   be nil from a nil the program writes (NFW_NIL, NFW_NO_ELSE,
   NFW_SAFE_NAV, NFW_UNSET). An ivar keeps ivar_nil_recv_guard's policy;
   a local or a global qualifies when its slot holds the pointer or a
   shared String's handle, where the test reads it. Inside the call's own
   emission the receiver is seen as tested (Repr.nil_tested, a view), so
   the call is armed once. */
typedef enum {
  CN_NONE,     /* not nil here, a `&.` call, or a method the program gives nil */
  CN_RAISE,    /* nil has no such method: NoMethodError, after the operands */
  CN_ANSWER    /* nil has it (is_nil_method): NilClass answers, where its
                  answer has the call's type (nil_answer_text); a call whose
                  answer has not is emitted as before, untested */
} CplanNil;
const char *cplan_call_name(const NodeTable *nt, int id);
int cplan_nil(Compiler *c, int id);
int cplan_nil_answer_served(Compiler *c, int id);   /* codegen_call.c: nil's answer has the call's type */
/* Is a nil fact's source `why` (nil_fact_why) a nil the program writes --
   a literal, a missing else, a safe navigation, a slot read before its
   write, an element of an Array the program stores nil into -- which a
   receiver's nil test answers for? One the fact cannot bound (any element,
   a global, an ivar, an unseen caller) is not. */
int cplan_nil_written(int why);

/* ---- CM_*: a call that finds no method where it runs ----
   Whether call id -- receiverless, on self, or on a constant naming a
   program class -- runs at a load point where the program's method of its
   name is not defined yet (call_target answers none there, from the load
   record) and no builtin of self's answers it: the raise CRuby makes,
   NameError for a bare name, NoMethodError otherwise; self_text how the
   message names the receiver ("class U", "module M", "main"), or, for an
   anonymous class, which names itself at run time, *anon its index (-1
   otherwise; anon may be NULL). Pure. */
typedef enum { CM_NONE, CM_NAMEERROR, CM_NOMETHOD } CplanMissing;
int cplan_missing(Compiler *c, int id, char *self_text, size_t cap, int *anon);
/* Does the subtree at id hold a call cplan_missing answers, in a block or
   an operand alike? */
int cplan_subtree_missing(Compiler *c, int id);
/* Does any call of the program find no method where it runs
   (cplan_missing)? Asked once, then remembered. */
int cplan_program_missing(Compiler *c);
/* The node value v runs last when it holds a call cplan_missing answers,
   walking v in CRuby's order for a call (the receiver, then the
   arguments left to right, into nested calls; an operator is a call too):
   that call, or the operand holding it that runs whole -- one that is no
   call (an Array, a Hash, an interpolation), or a call whose order this
   does not follow (`&.`, a splat, keyword, forwarded or block argument, a
   literal block). -1 when v holds no such call. This is evaluation
   order, which the value-position walker does not answer: it says where
   a value comes from. Asked once per node, then remembered. */
int cplan_missing_in_order(Compiler *c, int v);
/* The class call id's constant receiver recv names where the call runs, as
   the load record resolves the reference there (elab_class_ref_at): -1 for
   none of the program's (Object's top level), below -1 for one it does not
   know */
int cplan_const_recv_class(Compiler *c, int id, int recv);
/* The self where call id runs, as the load record has it there
   (elab_self_at), asked in the plan's frame: a program class or module,
   -1 for main, below -1 for one it does not know */
int cplan_self_class(Compiler *c, int id);
/* Does the plan route call id, a class body's call on self the desugar
   spelled with the class (`rand(2)` -> `U.rand(2)`), to Kernel's module
   function of the name: the class's own method of it is not defined where
   it runs (call_target), as in a class with no def of it. Kernel's are
   private, so route_self_builtin_calls makes such a call bare again; a
   builtin the class itself answers (`U.name`) keeps its receiver and
   reaches the builtin through call_target's answer. */
int cplan_self_builtin(Compiler *c, int id);
/* Is every class method `name` of program class cls defined after call id,
   as the record proves at the call's load point (call_target's answer for
   a call there)? A reflection asked there (`C.respond_to?(:z)` before
   `def C.z`) then finds none. 0 unless the call has a point and runs when
   its statement does: not in a method body, a lambda or an END body, and
   in a block only when every block around it is certain to run at once
   (elab_blocks_run_now: a builtin iterator on a literal); 0 too for a class
   whose chain the record does not know. */
int cplan_cmethod_later(Compiler *c, int id, int cls, const char *name);
/* How a NameError or NoMethodError of call id names self, into buf:
   "class U" or "module M" in a class or module body and "main" at the top
   level (the record's self where the call runs, elab_self_at), else "an
   instance of" the class ci the body is emitted for, else "main". NULL
   for an anonymous class, which names itself at run time. */
const char *cplan_self_text(Compiler *c, int id, int ci, char *buf, size_t cap);

int cplan_reopen_runs_builtin(Compiler *c, int id, int mi);

#endif
