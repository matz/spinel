/* call_plan.h -- the user method a call binds, resolved from the tables.

   cplan_user(c, id) answers which user method the call node `id` reaches
   -- the method scope, the class whose chain was searched, the arm kind
   (UC_*, compiler.h) and whether the class tables already decide the
   dispatch (one method, or a switch over overrides) -- from the scope and
   class tables and the settled node types alone. It never runs inference
   and never emits, so asking it changes nothing (#7100, Phase B).

   A plan is memoized per node, but only where the node is read as itself:
   no view, no instance_exec scope move or class, no inline splice. Inside
   one of those it is computed afresh and not kept. No plan ask hands out
   shared storage: every ask returns its plan by value, and the arms and
   message a plan points to are kept, unchanged, for the compile. So the
   caller's copy is its own: no later ask, of this call or another
   (resolving one reads Reprs and the analysis, which ask other plans), can
   change it under the caller. --plan-check compares it with inference's
   record and with the binding codegen made. */
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
  CRF_TOP_SELF,    /* a receiverless call in a top-level def to a name main has a singleton method for (diagnose_unsupported_call) */
  CRF_CAPTURE,     /* a record class's class method captured where its def may not have run, or a module's absent one where its NameError's class is not told (emit_call_method_obj_arms) */
  CRF_RECORD_NEW   /* a record class's own `new` or `[]` called where its def may not have run (emit_new_call_arms, the bare new) */
} CplanRefuseFrom;

/* CallPlan.absent: the side a `method(:sym)` that finds no method names in
   CRuby's NameError: an instance side ('K'), a class side ('#<Class:K>'),
   or a module's class side before its singleton class is made ('Module');
   untold: a module's, where the record cannot tell which of the last two,
   whose capture the plan refuses (cplan_refuse_resolve) */
enum { CP_ABSENT_NONE, CP_ABSENT_INST, CP_ABSENT_SG, CP_ABSENT_MODULE, CP_ABSENT_UNTOLD };

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
  unsigned char absent;    /* a `method(:sym)` that finds no method of
                              owner_ci's side (CP_ABSENT_*): CRuby's
                              NameError, where it runs (mi -1) or while no
                              version of the entry mi is live
                              (cplan_capture_absent) */
  unsigned char rkind;     /* CP_REFUSE: CplanRefuse */
  const char *msg;         /* CP_REFUSE: the message, as the compiler prints it */
  unsigned char rfrom;     /* CP_REFUSE: CplanRefuseFrom */
  unsigned char core_hook; /* a super in a class method that reaches only a core hook's no-op: nothing of the program's answers it, and emit_super answers nil */
  unsigned char answers_receiver; /* mi is a class method of an ancestor of owner_ci that answers an instance of owner_ci (cmethod_receiver_class): the function returns a pointer to its own class's type, the call is cast to owner_ci's */
  const char *delegate;     /* the protocol method name, before alias resolution */
} CallPlan;

/* Is plan p a program's method on a root of the builtin classes (Object,
   or Numeric or NilClass ahead of it), one that answers every receiver the
   lookup reaches it for (cplan_root_reopen)? The kinds of reopening below
   that, a builtin class's own, bind by the receiver's type. */
int cplan_root_reopen_owner(Compiler *c, const CallPlan *p);
/* ...and is call id's plan p such a binding that answers the call ahead
   of the builtin arms (not an alias that captured the builtin's method,
   nor an exception accessor picked at run time, nor a private or
   protected method of a name a builtin has, nor one closed to the call)?
   The analysis asks cplan_user_fresh's plan, its types still moving. */
int cplan_root_reopen_call(Compiler *c, int id, const CallPlan *p);
/* The program's Object method call `call` reaches in a poly dispatch's
   builtin arm, or -1: on an instance of builtin class cls (ArmCtx.arm_cls),
   the one cplan_root_reopen binds, none where cls answers the name ahead
   of Object (a String's bytesize); in an arm of any builtin's value (cls
   NULL), Object's method of the name, where the arm's re-entered call
   lands (emit_root_reopen_call). */
int cplan_arm_object_target(Compiler *c, int call, const char *cls);
/* The method of the program's that p binds directly to a call on a builtin
   value (a root class's or a reopened builtin class's), -1 where p binds
   none: its answer is the call's. */
int cplan_reopen_binding(CallPlan p);
/* ...of those, a root class's or a yielding one, -1 otherwise: its answer,
   a yield's block value included, is the call's ahead of the builtin's own
   lowering of the name. */
int cplan_reopen_target(Compiler *c, CallPlan p);
/* Does any scope define an instance method `name` every object answers: the
   top level's, or Object's, Kernel's or BasicObject's own? The name-level
   twin of cplan_root_reopen, for the desugars that run before types exist
   (it holds for a receiver whose builtin class owns the name too). */
int cplan_object_method_named(Compiler *c, const char *name);
/* Does call `call`, which takes a block, throw away every value its block
   answers? A builtin iterator of the each family does
   (is_block_value_dropped_iter: each, times, loop, each_char, ...), on any
   receiver but an Enumerator; a call a desugar wrote for the builtin's own
   (cplan_builtin_call) runs it. So does each method of the program's the
   call enters (cplan_entries; every method of the name where the plan lists
   none, a method_missing too) that reads none: whose yields' values are each
   thrown away (`def each = @n.times { |i| yield i }`, not `def each =
   p(yield)`), that hands its block to no super and hands its block
   parameter, if any, on only to such a call (`def each(&b) = @a.each(&b)`). */
int cplan_block_value_ignored(Compiler *c, int call);
int cplan_accessor_classes(Compiler *c, int id, int *out, int cap);
/* ...and the methods a super in s that no call stands for may run, into
   to[VER_MAX_ARMS]: that one, or each version a layer of a mixin's copies
   holds below a later one. How many, 0 for none */
int cplan_super_runs(Compiler *c, Scope *s, int *to);
/* A conversion performed by a helper, separately from the enclosing call. */
CallPlan cplan_conversion(Compiler *c, int ci, const char *name);
CallPlan cplan_method_call(Compiler *c, int id);
/* The Method a capture (`k.method(:a)`) makes where it finds no method of
   the name and k's class answers respond_to_missing?: the method
   desugar_missing_method_obj made for it (its scope on the capture), whose
   call runs method_missing(:a, ...); -1 for none. method_obj_lookup binds it in
   call_target's miss. */
int cplan_capture_missing(Compiler *c, int id);
/* May capture id (`recv.method(:sym)` on an object of program class k)
   make a method_missing Method: k's chain holds no method of the name and
   no accessor of it (a stand-in a later include or prepend puts in front
   of none aside), no builtin does or may (one of Object's instance
   methods as the oracle lists them, of a builtin module k includes, or of
   a native class; any name where the chain holds a builtin class other
   than Object, an Array's, an Exception's or a Struct's subclass, whose
   methods the tables do not all list: ver_builtin_chain), no class under
   k has one, and k's method_missing and respond_to_missing? are each a
   def the record shows surely live where id runs (ct_met_version, in the
   call plan's frame)? 0 where the record cannot tell: the capture keeps
   its NameError. desugar_missing_method_obj makes the Method's methods
   where it answers 1. */
int cplan_capture_misses(Compiler *c, int id, int k, const char *sym);
/* The test a capture of target t runs first,
   respond_to_missing?(:a, include_private), which raises NameError when
   false (the method desugar_missing_method_obj made, its scope on t's
   def); -1 for a target no such test guards. */
int cplan_missing_guard(Compiler *c, int t);
/* What capture id binds for t, an arm of its target's guard: the
   method_missing Method (cplan_capture_missing) where t is the stand-in a
   later include put in front of no method (inc_super_behind), else t. */
int cplan_capture_arm(Compiler *c, int id, int t);
CallPlan cplan_send_arm(Compiler *c, int id);
CallPlan cplan_exception_render(Compiler *c, int id, int ci);
CallPlan cplan_user(Compiler *c, int id);
/* The program's def a builtin's wrapper, method mi, stands for in place of
   the builtin where its Method is made (a reopen's def of the name), -1
   where it stands for the builtin, -2 where the record cannot tell */
int cplan_bam_reopen_target(Compiler *c, int mi);
/* The program's def of sym that the Method maker node site meets on the
   ancestors of builtin class cls up to own (NULL: all of them), the walk
   cplan_bam_reopen_target asks of a wrapper's classes, asked before there
   is a wrapper (desugar_builtin_method_obj) and for a boxed receiver's
   classes (emit_method_params_by_recv). The def's scope where a reopen
   gives it one live at the maker, -1 where none does, -2 where the record
   cannot tell. */
int cplan_builtin_reopen_def(Compiler *c, int site, const char *cls, const char *sym, const char *own);
/* Is the def mi (cplan_builtin_reopen_def's answer) of sym public where the
   Method maker node site runs: the visibility the record gives it there, not
   the final one (a `public :sym` that follows does not count)? Not where the
   record cannot tell it. */
int cplan_reopen_def_public(Compiler *c, int site, int mi, const char *sym);
/* How a Method of `sym` that node mn makes stands for each class of the
   program a boxed receiver may be of at run time, in ascending class order:
   CP_CARM_METHOD the def's scope the capture meets there (call_target's
   capture on the class); CP_CARM_ENTRY the scope of a version entry, where
   the capture finds one a run-time test of its cells picks (the arms of
   cplan_method_arms); CP_CARM_READER or CP_CARM_WRITER the attribute the
   capture met, or the class's own where no def is. A class with none has no
   arm (the receiver of it raises NameError). *n their count; the caller
   frees the list. */
typedef enum { CP_CARM_METHOD, CP_CARM_READER, CP_CARM_WRITER, CP_CARM_ENTRY } CplanClassArmKind;
typedef struct { int cls, mi; CplanClassArmKind kind; } CplanClassArm;
CplanClassArm *cplan_capture_class_arms(Compiler *c, int mn, const char *sym, int *n);
/* The method scope call `id` runs when it asks for `name`: on cls's
   instance side through its chain, CT_CMETHOD on its class side, CT_OWN
   in cls's own table only, CT_TOP among the top-level functions (cls not
   read; main's singleton methods first for a call on main); ml->def_class
   (when asked) the class whose table holds it, left as it was where none is
   found; -1 for none. The one place a site asks "what does this call run": it answers
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
   when the constructor must select an arm at run time. CT_IMPLIED asks for
   a name the node does not itself call but makes CRuby call (a row of
   tools/implied_calls.yml), which passes the gate on the node's own names.
   CT_INCLUDED asks the method a module the top level includes brings, as
   seen from node id (cls is not read); the record is not asked for it yet.
   CT_PUBLIC marks a call that enforces visibility (an explicit receiver
   that is not self, public_send): where the arms of the entry it binds
   differ in visibility, it binds the face entry that raises for the
   private ones (VF_PRIVATE), or with CT_PROTECTED also for the protected
   ones (VF_PUBLIC: public_send, or a caller to which a protected owner is
   not open). */
enum { CT_CMETHOD = 1, CT_OWN = 2, CT_TOP = 4, CT_REACH = 8, CT_CAPTURE = 16, CT_IMPLIED = 32, CT_INCLUDED = 64, CT_PUBLIC = 128, CT_PROTECTED = 256 };
/* The face of a version entry (VerEntry.face): 0 the entry every arm of
   which runs its version, VF_PRIVATE one whose arms of a private version
   raise, VF_PUBLIC one whose arms of a private or a protected version do */
enum { VF_NONE, VF_PRIVATE, VF_PUBLIC };
/* The names call node id itself calls, into out: a call's or an attribute
   target's name, a `recv.m op= v`'s reader and writer; how many (0 for any
   other node). call_target binds a call of one of them to the method
   version it runs; a method the language calls for the node keeps the
   last version. Super contributes the enclosing method's original name.
   A send of a literal name (`send`, `__send__`, `public_send`) calls that
   name too, a second one. */
int call_node_names(Compiler *c, int id, const char *out[2]);
/* The calls node id makes CRuby make that the plan binds by version: an
   implied call whose lowering asks the plan which method it runs
   (cplan_implied_target), as its row of tools/implied_calls.yml says
   (NtImplied.planned), with the operand that receives it. Into buf[0..cap)
   when they fit, else an array it allocates, which the caller frees; their
   count in *count. The collectors that read call_node_names
   for a call's own names read these too: the entries' survey and
   reachability's version marks (ct_implied_classes), and the open
   histories' asks (ver_named_facts). A builtin's callbacks are not among
   them yet: their rows do not say which operand an asked callback lands on,
   nor with what arguments. */
struct IcCall;
struct IcCall *call_node_implied_names(Compiler *c, int id, struct IcCall *buf, int cap, int *count);
/* The name call node id finally calls: its own, or a send-family call's
   literal first argument (a send of a send unwound); *used is the number
   of arguments the sends took. NULL when that name is computed or absent. */
const char *call_sent_name(const NodeTable *nt, int id, int *used);
/* What call_target answers besides the scope. kind: ML_METHOD, a method the
   record proves (a placed version, or a name with one version that no call
   runs ahead of); ML_ENTRY, the scope of an entry (or the copies' entry)
   whose cells tell at run time; ML_NONE, no method, or a call the record
   proves runs before every version (and the builtin an entry's else arm
   pins); ML_OBJECT, a call on main's ask (CT_TOP) that meets a version of
   Object's (ct_top_chain): no method of its own, as the call's ask on
   Object binds it, with self as receiver; ML_TABLE, the record gave no
   proof and the answer is the table's
   last definition, for the reason in why. n: how many versions the call's
   window reaches (elab_method_span), 0 when no window was asked, -1 when
   the answer is the table's for want of a version or an entry, or because
   the call is in a body that never runs; copies: the answer is the copies'
   entry. survey: set by the two passes that ask which entries the calls
   need (ver_need, cplan_sg_version), whose answer is not kept. def_class:
   the class whose table holds the method, written only where one is found,
   so a caller that presets it finds it as it was (not so for
   cplan_super_target and cplan_super_chain_mi, which set *ml whole). from:
   the class a super's lookup started from, written by those two only.
   kind is read by cplan_resolve_super, cplan_presence, cplan_implied_target,
   cplan_boxed_to_ary_classes and cplan_main_singleton_call, and ML_OBJECT by
   cplan_missing and method_obj_lookup; why only by the method-table check
   (mtab_table). ML_SINGLETON: a call on main that main's own singleton
   answers by the core's method it has from the start (`to_s`, `inspect`:
   elab_main_singleton_at), ahead of Object's table; no method of the
   program's. */
typedef enum { ML_METHOD, ML_ENTRY, ML_NONE, ML_OBJECT, ML_TABLE, ML_SINGLETON } MlKind;
/* why the table answers: no window, or an unplaced capture (UNPLACED); the
   chain moves inside the window (MOVES); two or more versions, or a window
   with no version, and no entry (MANY); a version the plan cannot map
   (UNMAPPED); the record says no version while the table answers one,
   or the window's version is not the one an explicit call of the name
   binds (DISAGREE); a body that never runs (NEVER); a versioned name answered
   without asking the record, as for a call with no node or a name the node
   does not itself call (UNASKED) */
typedef enum { MLW_UNPLACED = 1, MLW_MOVES, MLW_MANY, MLW_UNMAPPED, MLW_DISAGREE, MLW_NEVER, MLW_UNASKED } MlWhy;
typedef struct { MlKind kind; unsigned char why, copies, survey; short n; int def_class, from; } MethodLookup;
/* ml (NULL for a caller that wants the scope only) */
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, MethodLookup *ml);
/* A super's parent-chain target, asked from class par under name; ml (NULL
   for the scope only) is call_target's, with from par (the lookup's start,
   whatever class answered: ml->def_class) */
int cplan_super_chain_mi(Compiler *c, int id, const Scope *s, int par, const char *name, MethodLookup *ml);
/* The method `super` in s reaches (its prepend chain's next, else the parent
   chain's); ml as call_target's, and ml->from the class the lookup started
   from (-1: a prepend chain with no next) */
int cplan_super_target(Compiler *c, int id, const Scope *s, MethodLookup *ml);
int cplan_kernel_def(Compiler *c, const char *name);   /* a version of the name's top-level history is a program Kernel def */
/* Does call id run Kernel's raise: a receiverless `raise` or `fail` that no
   method of the program answers (a class's own `raise`, a top-level def)? */
int cplan_kernel_raise(Compiler *c, int id);
/* Does the Class row for call id, named name, on the class value recv answer
   the call? Not where a class method of the program's does (the plan binds
   it a target), nor where the row is a private method's and the call names
   an explicit receiver and does not ignore visibility (a send does): that is
   NoMethodError. */
int cplan_class_row(Compiler *c, int id, int recv, const char *name);
/* How many positionals call id's trailing keyword hash adds for a callee
   whose keyword fact is kwpos (builtin_kw_positional): 0 for none, a hash
   that spreads nothing (`**{}`, `**nil`) or keywords the callee takes; 1 for
   one it passes as a Hash (a key, or the full branch split_kwsplat_calls
   made, whose hash is not empty); -1 where the run time decides (an open
   `**` the split left: under defined?, beside a splat, or one whose callee
   no fact speaks for). 0 for a call with no keyword hash. */
int cplan_hash_positionals(Compiler *c, int id, int kwpos);
/* The same for builtin cls's method `name` (its class method where cside),
   by its keyword fact (builtin_kw_positional). A method that takes the one
   keyword builtin_kw_taken names and passes a hash with any other key on
   counts that hash: 1 where a literal key is another or the hash is the
   full branch of a split (split_kwsplat_calls tests for another key), 0
   where every element is that keyword or spreads nothing or the hash is
   the split's other branch, -1 where a `**` operand is the run time's
   (the split, which runs from the method's most positionals on, gives each
   of its branches a count). */
int cplan_builtin_hash_positionals(Compiler *c, int id, const char *cls, const char *name, int cside);
/* Does the keyword hash or Hash literal h hold a key other than `key`: 1
   where a literal key is another (in a `**{...}` literal too); 0 where
   every element is `key:` or spreads nothing; -1 where a key or a `**`
   operand is the run time's (`k => v`, `**h`). */
int cplan_hash_holds_key_past(Compiler *c, int h, const char *key);
/* The class-side row (bop_class_find) call id takes on builtin class cls,
   its constant receiver: NULL where none of cls's singleton chain has one,
   or where a class method the program gives a class of the chain, up to
   the row's own, stands in front of it (`class IO; def self.select` is
   what File.select runs). */
const BuiltinClassOp *cplan_builtin_class_row(Compiler *c, int id, const char *cls);
/* A call that builds an instance of an Array or Hash subclass (or, with
   `[]`, of a Struct or Data class: cplan_as_new) from its arguments through
   a Class value known only at run time (a parameter, an element, a call's
   answer): `k[..]` while no class of the program defines a class method
   `[]`, or `k.new(..)` with arguments. The one place that says
   so; inference, the kind evidence, the instantiation pass and the emitters
   read it. */
typedef enum { CVC_NONE, CVC_AREF, CVC_NEW } CvCtor;
CvCtor cplan_cv_ctor(Compiler *c, int id);
/* ...the same on the call's shape alone, for the census ahead of the type
   fixpoint (compute_instantiated): it reads no type (a constant receiver is
   judged by its writes, class_recv_may_be_dynamic), and a `[]` it answers
   CVC_AREF is one whose receiver may hold a Class value, which
   cplan_cv_ctor then asks the receiver's type to settle */
CvCtor cplan_cv_ctor_shape(Compiler *c, int id);
/* ...whether a call of form f builds an instance of class k when its
   receiver holds k: a subclass of Array or Hash that runs no constructor of
   its own for the call (`new` reaches an initialize or a self.new; `[]`
   skips an initialize, as CRuby's does) */
int cplan_cv_ctor_class(Compiler *c, CvCtor f, int k);
/* ...and whether class k is one call id builds: one cplan_cv_ctor_class
   takes that the receiver may hold (poly_cls_value_reaches) */
int cplan_cv_ctor_builds(Compiler *c, int id, int k);
/* Does call id construct class k, when its receiver holds k, as `new`
   does (the class's initialize or a record's generated constructor taking
   the arguments)? A `new`, and a Class value's `[]` (cplan_cv_ctor) on a
   Struct or Data class, whose `[]` is its `new` (CRuby's Struct::[] and
   Data::[], which never run a `new` the program defines). With k < 0: on
   some class of the program. Which classes the receiver may hold is the
   reach facts' (dynamic_new_may_reach, new_recv_classes). */
int cplan_as_new(Compiler *c, int id, int k);
/* Does call `id`, which has no receiver, take the BOP_KERNEL row of its name: an internal row for a call the compiler made, any other where no method of the program's own (a top-level function or the enclosing class's) answers the name? */
int cplan_kernel_row(Compiler *c, int id, const char *name);
/* Does the call id on a known class (recv) run the program's own class
   method of a Class or Module reflection name (superclass, ancestors,
   instance_methods, ...) in place of the builtin? Not when that method's
   `super` reaches a builtin only the builtin path answers. */
int cplan_own_class_reflection(Compiler *c, int id, int recv, const char *name);
/* Is class value recv, of call id, proven a Class and not a Module: a program
   class (not a module) it names, a core class, `Class.new`, `self` in a class
   body, or a superclass reference of a class made (the fired hook's receiver)?
   A Class-only row (BOPF_CLASS_ONLY) answers a call on such a receiver only. */
int cplan_recv_class(Compiler *c, int id, int recv);
/* the classes a call of a versioned name (ct_version_named) may run on,
   each handed to fn with the flags call_target is asked with */
int ct_version_named(Compiler *c, const char *nm);
int ct_version_marked(Compiler *c, const char *nm);
void ver_hold_free(Compiler *c);
void ver_chain_free(Compiler *c);
void new_inits_free(Compiler *c);
void new_sites_free(Compiler *c);
/* the method call id reaches when it can reach only main's own method of
   name, -1 for none (a call on main where main's method is defined) */
int ct_main_target(Compiler *c, int id, const char *name);
typedef void (*CtClassFn)(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg);
void ct_version_classes(Compiler *c, int id, const char *nm, CtClassFn fn, void *arg);
/* ...for each call node id makes that the plan binds by version
   (call_node_implied_names) of a name `versioned` answers (ct_version_named,
   ct_version_marked): the classes the operand its row names may be, on the
   instance side, asked with CT_IMPLIED. The receiver walk above and this one
   resolve where a call lands in one place (ct_landing_classes) */
void ct_implied_classes(Compiler *c, int id, int (*versioned)(Compiler *, const char *), CtClassFn fn, void *arg);
/* the version entries a call whose version is known only at run time
   reaches, and the run-time cells they read (#9898) */
void ver_entries_build(Compiler *c);
void ver_prepend_cell_name(int cell, char out[32]);
/* `cell = v` put in statement list `list` at its statement `at` (after it
   when after): the write, for the caller to record (elab_record_write) */
int ver_cell_put(Compiler *c, const char *cell, int list, int at, int after, int v);
void ver_cells_write(Compiler *c);
/* forgets the versions the calls were answered (Compiler.ver_memo), for a
   fact they read that changed */
void ver_memo_drop(Compiler *c);
int ver_entries_demand(Compiler *c);
/* Is call id the builtin an entry's else arm runs on self (VK_BUILTIN)? It
   renders the class by the builtin's own table, not by a class method the
   program defines: that method is the one the entry has not reached */
int cplan_pinned_builtin(const Compiler *c, int id);
/* Is call id an entry's arm (or else arm) call, pinned to the method it
   runs or to the builtin (ver_entry_targets lists the methods)? */
int cplan_pinned(const Compiler *c, int id);
/* ...and the visibility (SP_VIS_PRIVATE or SP_VIS_PROTECTED) that a face
   entry's arm for that version raises with, instead of running it; 0 for
   an arm that runs, and for any other node */
int cplan_pinned_vis(const Compiler *c, int id);
/* flags = 0 (a call's instance-chain ask) with CT_PUBLIC (and CT_PROTECTED)
   added where call id enforces visibility (an explicit receiver that is not
   self, public_send) and has no block; any other flags as they are. call_target asks with
   them, so that every reader that asks which method such a call runs gets
   the entry the plan made for it (ct_face_for), and the survey of the
   entries (ver_need) asks the same. */
unsigned ct_vis_flags(Compiler *c, int id, unsigned flags);
/* the methods entry scope e's arms call, its else arm's last: at most
   VER_MAX_ARMS + 1; an open history's else arm, which calls none, as its
   last version (the method a Method object of it captured before) */
enum { VER_MAX_ARMS = 64 };
int ver_entry_targets(const Compiler *c, int e, int *out, int *tests);
/* Does target t of a call run method mi: t itself, or one of the methods
   the arms of t call where t is a version entry (ver_entry_targets)? */
int cplan_target_runs(const Compiler *c, int t, int mi);
/* The methods a Method made of entry e can hold, one for each arm that
   may run at the bind (call_plan.c); tests, when asked: 2k and 2k + 1 the
   tests of arm k and of its inner arm, -1 for none. cap: the capture the
   Method came from (CallPlan.method_node), -1 for none known. An arm
   where that capture finds no method is CPLAN_ARM_NONE: no Method is
   made there, the capture raises CRuby's NameError */
enum { CPLAN_ARM_NONE = -1 };
int cplan_method_arms(Compiler *c, int e, int cap, int *out, int *tests);
/* ...those of them a Method made there holds: all but CPLAN_ARM_NONE's
   (entry e itself where no other is left) */
int cplan_method_held(Compiler *c, int e, int cap, int *out);
int cplan_bool_sides(Compiler *c, int id, int *tmi, int *fmi);
void cplan_check_missing(Compiler *c, int id, int mi, int ci, const char *name, int cmeth, const char *site);
/* The same plan resolved afresh and never kept, for a reader that runs in
   the analysis, before the memo is codegen's to fill (the nil facts,
   analyze_nil.c): it answers from the types as they stand. */
CallPlan cplan_user_fresh(Compiler *c, int id);
/* Does call id bind the builtin's own method of its name, whatever the program
   defines? A call a desugar wrote for the compiler's own use, or an alias that
   captured the builtin (nt_mark_builtin_call). It reads the node's record only,
   no types and no resolution, so every phase may ask it. The plan of such a
   call is empty. */
int cplan_builtin_call(const Compiler *c, int id);
/* Does main's own singleton answer call id by the core's `to_s` or
   `inspect` (ML_SINGLETON), so that the call is main's text, not a method of
   Object's table? For `method(:to_s)`, does it answer the capture? */
int cplan_main_singleton_call(Compiler *c, int id);
/* Is call id main's own `to_s` or `inspect`, as desugar_main_singleton_calls
   recorded the plan's answer (cplan_main_singleton_call): its receiver is
   main, so its value is main's text. Every phase may ask it. */
int cplan_main_text_call(const Compiler *c, int id);
/* Does Object's program method of the name answer call id for the receivers no
   class arm of its dispatch took (builtin values, instances of classes without
   a method of their own)? The second also asks class_id's chain. A call that
   binds the builtin's own method (the super of a program's own reflection,
   rewritten to self's builtin) is not answered by either. */
int comp_call_reaches_own_any(Compiler *c, int id, const char *name);
int comp_call_reaches_own(Compiler *c, int id, int class_id, const char *name);
/* A call on a constant receiver recv: the program's singleton method it
   binds (fresh: resolved as cplan_user_fresh is), or, where it binds none,
   a plan of no method (mi -1). */
CallPlan cplan_const_user(Compiler *c, int id, int recv, int fresh);
/* The user methods whose return is call node id's value, into out (at
   most cap): its plan's method and, for a switch, each member. A reader of
   what the call answers asks this; a reader of what the call enters (the
   methods its arguments and block reach) asks cplan_entries, which differ
   for a builtin `new`: it enters initialize but answers the object it
   builds, which no method returns (0 here). Answered from the types
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
/* The method the implied call `name` node id makes on operand op (a row
   of tools/implied_calls.yml: a multiple assignment's to_ary of its value)
   runs on an instance of cls: the version the record proves there, else an
   entry whose every state finds a method; -1 otherwise, with *ml (may be
   NULL) saying ML_TABLE (the reader keeps the table's answer) or ML_NONE
   (no row, or an undef on the chain) */
int cplan_implied_target(Compiler *c, int id, int op, int cls, const char *name, MethodLookup *ml);
/* The initialize the builtin Class#new runs when node id (a `new` the
   program's `new` does not answer, a `raise K`) builds an instance of
   class ci: the version the record proves where id runs (*ml ML_METHOD),
   else the table's method (ML_TABLE; ML_NONE and -1 where ci has no
   initialize of the program's). Every reader of a construction site asks
   it, so the site's arity, arguments, block and call name one method. ml
   may be NULL. */
int cplan_new_initialize(Compiler *c, int id, int ci, MethodLookup *ml);
/* Every initialize a builtin `new` of class ci may run, wherever it runs:
   the table's method, then each other version of it the record holds (a
   replaced one, in the history of the class that defines it) that an
   instance of ci may meet, into out[0..cap). How many: 0 where ci has none
   of the program's. *whole says whether that is every one: it is 0 where
   the record cannot list the versions, a met version is a removal, an
   undef or one the record does not map to a method, or past cap, and
   then out holds the table's method and the versions it could map. A
   fact of every construction of ci (which ivars it leaves set) holds
   where *whole is 1 and it holds for each; a flow into every initialize a
   construction binds (analyze_cls.c, cf_inits) takes the list either way.
   The record is made by the time the analysis asks (elab_program runs
   before the passes that ask these facts); asked before, it answers the
   table's method alone with *whole 0, which a memo of the facts
   (ivar_assigned_in_initialize) would keep as "not set". */
int cplan_new_initializers(Compiler *c, int ci, int *out, int cap, int *whole);
/* The initialize a Kernel `raise K` or `raise K, msg` (call id) runs, K a
   constant naming an exception class of the program's: K.new's there
   (cplan_new_initialize); -1 for any other call */
int cplan_raise_initialize(Compiler *c, int id);
/* The to_ary of the program's through which a value of type t converts
   where node `at` takes it apart (the implied to_ary call a row makes on
   operand op: a multiple assignment's value, the element a nested target
   takes), rb_check_array_type's, where the lowering can take its answer
   typed: the version the plan answers (cplan_implied_target) when it and
   the table's method answer an Array type; -1 otherwise. Only the one the
   load record proves the object answers there: a to_ary defined, or
   brought in by an include, after the assignment is not answered yet.
   Where the plan answers a version that may give another value (nil, a
   non-Array, or Arrays of another type than the table's), *boxed (may be
   NULL) is 1: the site converts the value boxed
   (cplan_boxed_to_ary_classes), its targets taking what that conversion
   gives. */
int cplan_typed_to_ary(Compiler *c, TyKind t, int at, int op, int *boxed);
/* How a class's boxed to_ary runs where the plan answers it: the method
   table's method (the one sp_obj_to_ary calls), the version entry, or
   another version its window proves */
typedef enum { CTA_TABLE, CTA_ENTRY, CTA_VERSION } CtaHow;
typedef struct CplanToAry { int mi; CtaHow how; } CplanToAry;
/* The classes whose to_ary that call reaches on a boxed operand, where
   the record proves it: into out[0..cap), with the method each runs
   there and how into ans[0..cap) (may be NULL); the count */
int cplan_boxed_to_ary_classes(Compiler *c, int id, int op, int *out, CplanToAry *ans, int cap);
/* The classes a boxed site's implied to_ary may answer by their version
   entry, through the program's one switch of them (sp_obj_to_ary_entry):
   each class a box can hold whose to_ary has an entry made and reachable,
   cls[0..n) ascending, with that entry in scope[0..n). The entry is the
   same function at every site, as its key holds the class, the name and
   an arity no node gives, so the set is the program's: built on the first
   ask, after the analysis (codegen asks), and freed with the Compiler. */
typedef struct CplanToAryEntries { int n; int *cls; int *scope; } CplanToAryEntries;
const CplanToAryEntries *cplan_to_ary_entries(Compiler *c);
void cplan_to_ary_entries_free(Compiler *c);
/* The program's `[]=` an index store on an instance of class cls calls:
   its method scope, with the class defining it in *cdef when cdef is
   given; -1 when the chain has none that takes a key and a value. */
int cplan_index_writer(Compiler *c, int cls, int *cdef);
/* What storing into attribute or index target tgt (`o.x`, `a[i]` of a
   multiple assignment) does besides the store: CPS_CODE when it may call
   a method of the program -- a store on a receiver typed at run time, a
   writer that is a method of the program (a plain attr_writer is one
   field store), the program's `[]=` (cplan_index_writer), or a builtin
   `[]=` whose callbacks on the keys it is handed (the builtin-callback
   rows: a Hash's #hash and #eql?, an Array's #to_int) reach a method of
   the program; CPS_CHANGES_ARRAY when it is a builtin `[]=` on an Array,
   which changes its receiver in place (the name's mutation row). 0 for
   any other target. */
enum { CPS_CODE = 1u, CPS_CHANGES_ARRAY = 2u };
unsigned cplan_target_store_effect(Compiler *c, int tgt);
/* cplan_entries, for a reader that wants the code the call's emission
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
/* The user methods call node id enters, into out (at most cap): the
   methods its arguments bind to and its block is handed to, which a call
   graph, a parameter's or a block's facts follow. For a builtin `new`
   (no program `new` answers it), the initialize methods it runs: the class
   it builds and, for a `new` on the class its method runs for, each
   subclass's; none for a builtin class; CPT_UNKNOWN for a receiver the
   plan cannot name a class of, which may run any initialize of the
   program. So for a super, bare or not, in a program `new` that reaches
   the builtin Class#new: the initialize methods of the method's class and
   its subclasses, CPT_UNKNOWN for a module's `new`. For any other call,
   its cplan_targets. Same answers as cplan_targets otherwise (n >= 0,
   CPT_UNKNOWN). Never what the call answers: that is cplan_targets'. */
int cplan_entries(Compiler *c, int id, int *out, int cap);
/* Where cplan_entries cannot list them (CPT_UNKNOWN), the names whose
   program methods hold every method call node id may enter, into names (at
   most 2), and how many: the call's own name (a super's, its method's),
   and for a `new` initialize too (cplan_name_bound_names). A superset, for a reader that must see every body the
   call may run (a call graph, the blocks or arguments a method may be
   handed); a reader that needs the exact set takes CPT_UNKNOWN as it is. */
int cplan_entries_bound_names(Compiler *c, int id, const char *names[2]);
/* The same bound for a call of method name `name` that no node shows (a
   Method or an UnboundMethod of it, a send): name, and initialize for
   `new`. */
int cplan_name_bound_names(const char *name, const char *names[2]);
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
/* Can class method mi be the arm of call id, a call with argc arguments
   (splat of them splats) on a Class value known only at run time? A
   candidate that cannot is not a possible receiver for THAT call site --
   CRuby raises ArgumentError there -- so it must not veto the whole dispatch
   (#4129). Without a splat this is cplan_target_binds' judgement, which
   takes a trailing keyword hash as the callee's keywords: counted as one
   more positional argument against parameters that count the keywords,
   `def self.answer(id:, input:)` refused `k.answer(id: 1, input: {})`. With
   a splat the count is known only at run time: each may spread to nothing,
   so a candidate the other arguments leave room in may take it, and its arm
   binds through the gather, which judges the count there. Codegen's arms
   and the inference of their answer both ask this. */
int cplan_cls_arm_takes(Compiler *c, int id, int mi, int argc, int splat);
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
   read as itself; elsewhere computed afresh. Codegen raises the plan's
   refusal where it meets it; --plan-check holds the plan against the
   refusals codegen reports (refuse_observe, codegen_util.c). */
CallPlan cplan_refuse(Compiler *c, int id);
/* the documented-limit family: the node's message or NULL; *stop 0 when
   a limit down the receiver chain is the one to report */
const char *cplan_feature_why(Compiler *c, int id, int *stop, char *buf, size_t cap);
int cplan_define_method_early(Compiler *c, int id);
int cplan_record_sclass_early(Compiler *c, int id);

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
CallPlan cplan_user_in(Compiler *c, int id, int self_ci, int flags);
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
/* A bare call in an instance method of class scls whose own chain has no
   method of `name`, so it reaches root_mi: a top-level def or a method the
   program adds to Object. When a descendant of scls has a method of the
   name, CRuby runs that method for an instance of the descendant. 1 when the
   call is then a switch on self's class whose default arm runs root_mi
   (CP_PER_ARM: each arm binds the arguments by its own parameters). Only
   for a bare call or `self.m` without a block (inside a block or a lambda
   too, not one instance_exec runs), in an instance method of a class (not
   a module), and a root_mi that does not yield; a descendant's reader or
   yielding method keeps the old path, and so does a call in a block when
   a descendant's method calls super into a top-level def. */
int cplan_self_root_override(Compiler *c, int id, int scls, int root_mi);
/* Is call id's plan that switch on class scls (cplan_self_root_override),
   its default arm a top-level def or Object's method? Read off the plan. */
int cplan_root_switch(Compiler *c, int id, int scls);

/* The most candidate classes the statement-level block dispatch takes
   (poly_block_dispatch_cands refuses a program with more). */
enum { CPB_MAX_CANDS = 64 };

/* The methods a boxed native-block call's statement-level dispatch reaches
   (each arm's method and its proc form), asked once for a call and kept
   here: a caller testing many methods against one plan owns one, zeroed
   in `state`, and passes it to every cplan_virtual_member, so the poly-block
   plan is built once, not once per method. */
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
  PB_ND_UPTO,
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
  unsigned char unsettled;  /* a class-value trial arm's: what of its class
                               the record cannot prove settled before the
                               call (ELAB_GAIN_*, elab_class_unsettled),
                               which an emitter reading it cannot answer from */
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
/* a class-value trial arm's class (cplan_poly_cmeth: the builtin Module /
   Class answer for a Class value no class method takes), or -1 */
static inline int pa_class_value_trial(const PolyArm *a) {
  return a->kind == PA_TRIAL && a->key >= PA_KEY_CLASS_VALUE && a->key < PA_KEY_BUILTIN
         ? a->key - PA_KEY_CLASS_VALUE : -1;
}

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
PolyPlan cplan_poly(Compiler *c, int id);
/* The same plan, as far as emission reads it: the class arms (and the
   receiver form). Cheaper: the builtin families and trials, which only the
   --plan-check shadow compares, are left out. */
PolyPlan cplan_poly_arms(Compiler *c, int id);
int cplan_send_split_owners(Compiler *c, int recv, const char *name, const char **out, int max);   /* the classes a boxed send of name tests: the builtin ones recv may be, and the program's under them */
/* Resolve the complete plan without reading or writing the per-node memo.
   Use while inference facts are still moving. */
PolyPlan cplan_poly_fresh(Compiler *c, int id);
int cplan_struct_aset(Compiler *c, int cid, const char *name, int argc);
/* The arms of the statement-level block dispatch (#2448) of the same call:
   each candidate class's method spliced with the block, and its default.
   Computed afresh: the same node may take this dispatch or the method
   dispatch. */
/* The name index bounds a query for a native Ruby literal-block target. */
int cplan_native_block_possible(Compiler *c, int id, const PolyCand *pc, int n);
int cplan_cand_target(Compiler *c, int id, const char *name, const PolyCand *pc, unsigned flags);
/* Is mi a method an arm of call id's boxed dispatch on name reaches: a
   candidate class's table method or, for a name with versions, the
   version or entry call_target binds on that class, which the arm calls
   (cplan_cand_target); a reader candidate's; or the same for a class
   value's class methods. inst 0 asks the class-method arms alone. */
int cplan_cand_arm(Compiler *c, int id, const char *name, int mi, int inst);
PolyPlan cplan_poly_block(Compiler *c, int id);
/* A boxed receiver's `each { }` that the run-time dispatch serves because a
   class of the program takes a block for `each`: one arm, PB_ENUM_INLINE,
   when the call splits on the receiver's kind, the builtin kinds walking
   inline and any other value taking the dispatch; none when it does not. */
PolyPlan cplan_each_split(Compiler *c, int id);
/* The arms of the class-method tag switch of a call on a poly receiver
   that may hold a Class (emit_unresolved_call): each class's class method
   of the name, or its proc form. Computed afresh. */
int cplan_cmethod_arm(Compiler *c, int call, int k, const char *nm, MethodLookup *ml, int *pfk);
PolyPlan cplan_poly_cmeth(Compiler *c, int id);
/* cplan_poly_cmeth's class-value trial arms alone (pa_class_value_trial),
   without the cost of asking every class for a class method of the name.
   Computed afresh. */
PolyPlan cplan_poly_cls_values(Compiler *c, int id);
int cplan_visibility_class(Compiler *c, int id, int ci);
int cplan_has_method_missing(Compiler *c, int id, int ci, int cmethod);
int cplan_super_needs_method_missing(Compiler *c, int id);
int cplan_cmethod_visible(Compiler *c, int id, int ci, const char *name);
TyKind cplan_poly_arg_storage(Compiler *c, int id);
/* The re-entry of a call on an object whose static class cannot answer the
   name while one of its subclasses can (#4023). Computed afresh. */
PolyPlan cplan_poly_sub(Compiler *c, int id);
/* a nil-able String/Integer/Float receiver whose name the program's
   NilClass answers, and for the other values nothing or Object's public
   method: dispatched boxed (PB_SUBDISPATCH) */
int cplan_nil_scalar_reach(Compiler *c, int id);
/* Does a class value (recv, typed Class) answer `name` with a method of
   CRuby's before Object's instance method: its class's singleton chain
   (File.basename), or Class's and Module's own? A constant names the class;
   a class held in a variable could be any builtin one. */
int cplan_class_value_answers(Compiler *c, int recv, const char *name);
/* ...and the analysis' reading of it, before the nullability settles: call
   id of name on recv (type rt) is answered boxed, NilClass's method for nil
   and Object's for the rest (cplan_nil_scalar_form_ok, NilClass's public
   method, and a String, an Integer or Float beside Object's public one or
   that may be nil) */
int cplan_nil_scalar_boxed(Compiler *c, int id, const char *name, int recv, TyKind rt);
/* the most splats the boxed default arm spreads into Object's method */
#define POLY_OBJ_SPREAD_MAX 8
int cplan_nil_scalar_form_ok(Compiler *c, int id, const char *name);
/* How a receiverless call id in a builtin class's reopening hands its
   block to mi, the reopening's own method it reaches (emit_reopen_own_call):
   ROB_NONE, it has none; ROB_PASS, as mi's `&blk` argument, as the
   explicit-receiver reopen call passes it; ROB_DROP, unused by mi, which
   takes none (a literal block, a forwarded local); ROB_DECLINE, not by that
   call: a yielding mi is reached through its proc form, and a forwarded
   expression is evaluated by the general call. */
enum { ROB_NONE, ROB_PASS, ROB_DROP, ROB_DECLINE };
int cplan_reopen_own_block(Compiler *c, int id, int mi);
/* Does the enumerable driver (sp_poly_enum_proc), handed a callable call
   id forwards on a boxed receiver, answer the call: is no object of a class
   of the program with an `each` of its own among the values the receiver
   may hold, and is the callable never nil (an_fwd_block_may_be_nil)? */
int cplan_driver_walks_recv(Compiler *c, int id);
/* The re-entry of a call on a poly receiver as an Array call (#2935).
   Computed afresh. */
PolyPlan cplan_poly_redispatch(Compiler *c, int id);
/* The face arms of a call on a poly receiver (emit_poly_call): one owner's
   re-entry, a switch over several, or a String's value-form mutator.
   Computed afresh. */
PolyPlan cplan_poly_face(Compiler *c, int id);

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
void pa_end(Compiler *c, int frame, PolyPlan p);
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
  CN_ANSWER,   /* nil has it (is_nil_method): NilClass answers, where its
                  answer has the call's type (nil_answer_text); a call whose
                  answer has not is emitted as before, untested */
  CN_PROGRAM   /* the program gives nil the method (NilClass's or Object's,
                  cplan_nil_program_method), and the receiver's class runs
                  another or none: nil runs nil's, where its answer has the
                  call's type (cplan_nil_program_served) */
} CplanNil;
const char *cplan_call_name(const NodeTable *nt, int id);
int cplan_nil(Compiler *c, int id);
int cplan_nil_answer_served(Compiler *c, int id);   /* codegen_call.c: nil's answer has the call's type */
/* The program's method call `call` runs for a nil receiver: NilClass's,
   else Object's (-1 for none, or one only Kernel, BasicObject or the top
   level gives it); *nilcls whether it is NilClass's. */
int cplan_nil_program_method(Compiler *c, int call, const char *name, int *nilcls);
/* nil's answer to respond_to?(name) by the first method of the program's
   on NilClass, Object, Kernel or BasicObject (-1 for none; 0 when an undef
   hides it). include_all is 1 or 0 when known, -1 when only the run time
   knows it: a private method then gives 2 (the include_all decides). */
int cplan_nil_program_responds(Compiler *c, const char *name, int include_all);
/* The program's method CRuby's lookup of `name` on an instance of
   builtin class cls reaches first at the roots (NilClass, Numeric,
   Object -- a top-level def is Object's from where it runs, and the later
   of the two defs answers --, Kernel, BasicObject; a root's def the record
   proves runs after the call is passed over), for call `call`: its
   scope, with *vis (may be NULL) its visibility at the call (-1 where the
   record cannot tell), or -1 when a builtin's method answers first (*vis
   then its visibility), a reopened builtin below the roots has one, none
   does, or the record cannot order a top-level def and Object's. */
int cplan_root_method(Compiler *c, int call, const char *cls, const char *name, int *vis);
/* nil's method for the call when it is closed to it (private, or
   protected to this caller): cplan_root_method's from NilClass; -1
   otherwise, or where a method_missing answers */
int cplan_nil_closed_method(Compiler *c, int call, const char *name, int *vis);
/* Does an instance of class k answer `name` ahead of Object's method:
   by a method of its chain's other than Object's, a reader or a writer
   (attr_*, a Struct member), a visibility the chain declares below Object
   (`public :top`), or a method_missing? 0 for Object itself. */
int cplan_class_answers_ahead_of_object(Compiler *c, int call, int k, const char *name);
/* The method an explicit-receiver call id reaches, when it is the
   program's method at the roots (cplan_root_method, from the receiver's
   builtin class) and it, or a builtin's own method found ahead of it, is
   closed to the call: private, or protected to this caller (public_send's
   too); *vis gets which. -1 where the receiver's own class answers first
   (cplan_class_answers_ahead_of_object: a program object's class or a
   subclass the value may be of, a builtin value's reopened class), the
   method is open, a call skips visibility or calls on self, or a
   method_missing of every object answers. A boxed receiver's lookup is
   Object's: its arms ask the value's own class at run time (the program's
   classes by cplan_class_answers_ahead_of_object, the builtin ones by
   cplan_send_split_owners). The plan binds no method for such a call; its
   arms raise CRuby's NoMethodError. It binds and builds nothing and makes
   no version demand (cplan_root_method asks the top-level def of the
   tables), but the lookups it reads record the inference's memo
   dependencies (a class's method_missing, its chain), so a type that
   rests on its answer is asked again when they move. */
int cplan_root_closed(Compiler *c, int id, int *vis);
int cplan_nil_program_served(Compiler *c, int id);  /* codegen_call.c: nil's method's answer has the call's type */
/* Is a nil fact's source `why` (nil_fact_why) a nil the program writes --
   a literal, a missing else, a safe navigation, a slot read before its
   write, an element of an Array the program stores nil into -- which a
   receiver's nil test answers for? One the fact cannot bound (any element,
   a global, an ivar, an unseen caller) is not. */
int cplan_nil_written(int why);

/* ---- CB_*: what a call's `&` argument does ----
   A `&fe` argument is bound as written (a Proc, Method, Symbol, Hash, an
   object with to_proc, nil), or cannot become a block and raises TypeError
   after the receiver and the arguments have run (a value whose type is
   none of those and cannot be nil), or may be either, known where it runs
   (a boxed value, or a typed one that may be nil). Only the builtins
   convert: a program that defines to_proc, respond_to? or
   respond_to_missing? binds every one as written. *cn is the class the
   TypeError names ("" for a Boolean), set for CB_RAISE and for a typed
   CB_CHECK; NULL for a boxed one. */
typedef enum {
  CB_BINDS,    /* bound as written */
  CB_RAISE,    /* TypeError: no implicit conversion of *cn into Proc */
  CB_CHECK     /* decided where it runs */
} CplanBlockArg;
int cplan_block_arg(Compiler *c, int id, const char **cn);

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
/* ---- CU_*: a builtin name CRuby answers that no route of the compiler took ----
   cplan_missing's mirror: that one proves CRuby raises for a program's
   name; this one reads CRuby's tables for a builtin's. Whether call id --
   receiverless, on self, on a constant or on a typed value -- names a method
   its receiver's class answers with the visibility the call's shape reaches
   (an explicit receiver reaches public methods, send and a receiverless call
   any, public_send public), at an argument count the arity facts have a row
   for and admit, when no builtin-op row takes the call (a row's decline is
   that route's rejection of an operand, which CRuby raises over too):
   CU_GAP, with owner the CRuby method ("Kernel#local_variables",
   "Module#instance_methods", "Kernel.global_variables"). CU_RAISES: the
   tables hold the receiver and it does not answer the name that way, so
   the gate's raise stands. CU_NONE: the plan cannot tell the receiver's
   class (a boxed value, a self the record does not settle, a module's
   instance side), or the arity facts have no row, and the gate behaves as
   before. *nil_answers (when asked) is whether NilClass has the method too:
   a receiver that may be nil is then one gap, else nil is the NoMethodError
   for nil. CU_RAISES_NAME: a lookup by a literal name (Module#instance_method,
   method) whose name the record proves is nowhere, which raises CRuby's
   NameError, with owner holding its message. Not a CP_REFUSE: read only where codegen's last fallback is
   reached (emit_unresolved_call), as cplan_nil's answers are. Pure. */
typedef enum { CU_NONE, CU_RAISES, CU_RAISES_NAME, CU_GAP } CplanUnrouted;
CplanUnrouted cplan_unrouted(Compiler *c, int id, char *owner, size_t cap, int *nil_answers);
/* The visibility bit (1 public, 2 protected, 4 private) CRuby gives
   instance method m of a builtin class where reflection call node runs,
   for a name the program's own methods of the class leave open: of class
   cls by name when ci < 0 (core_method_vis), of program class ci from the
   builtin classes and modules in its chain otherwise (refl_builtin_vis);
   with inherit 0 its own only. For Object itself, main's own method of
   the name where node runs counts first (elab_main_method_vis_at): a
   top-level def is Object's; where main's history and Object's are one
   entry, its later version (cplan_program_method_vis), an alias in Object
   of a builtin's name among them. 0 for none; -1 where the rows do not
   hold the class. */
int cplan_builtin_method_vis(Compiler *c, int node, int ci, const char *cls, const char *m, int inherit);
/* The visibility (SP_VIS_*) a reflection call node that finds program
   method m of class ci (its own, or along its chain with inherit) reads:
   for Object's reopening, where main's history of m and Object's are one
   entry (elab_main_object_one_entry), the later version of either, as the
   load record orders them; else the class's table's. */
int cplan_program_method_vis(Compiler *c, int node, int ci, const char *m, int inherit);
/* The raise of call id's dispatch on self from class cid when cid's chain
   holds no method of its name and only descendants do: the switch's arms
   are theirs, and self of any other class (cid's own instance, a
   descendant without one, an object no singleton definition reached) gets
   CRuby's NameError for a bare name, NoMethodError otherwise. CM_NONE when
   cid's chain, a builtin of every object or a builtin module cid includes
   (Comparable, Enumerable) answers, a builtin superclass may, or the
   program defines a method_missing anywhere: spinel does not dispatch to
   one, and the call keeps the answer it had. Pure. */
CplanMissing cplan_dispatch_miss(Compiler *c, int id, int cid);
/* Does that dispatch's default arm take a builtin Kernel row (BOP_KERNEL) of
   the call's name in place of the raise: the row answers self of any class
   without a method of the name (Kernel#respond_to_missing?'s false)? */
int cplan_dispatch_row(Compiler *c, int id);
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
   know; *core (when asked) is the core class's slot when the name stands
   for a builtin class or module, also one the program reopens, else -1 */
int cplan_const_recv_class(Compiler *c, int id, int recv, int *core);
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
   whose chain the record does not know. The respond_to? a `defined?(C.z)`
   is answered by (desugar_defined_method_call) asks at the point of the
   call `C.z` (the defined?'s). */
int cplan_cmethod_later(Compiler *c, int id, int cls, const char *name);
/* The class an alias that node id meets as cls's `name` (singleton when sg)
   was made in, as Method#owner names it (*own_sg: on its singleton); -1 for
   none or not known. It asks for the versions ct_version_at does
   (elab_call_meet) and answers only where that meets one. */
int cplan_alias_holder(Compiler *c, int id, int cls, const char *name, int sg, int *own_sg);
/* Does a method no version of the program's gives answer `name` on cls's side
   sg (LR_MAIN's, -1, is Object's): a builtin's, a Struct or Data member, a
   builtin mixin's? may (certain 0) or must (certain 1) an answer exist: see the
   definition. Once the accessors are registered. */
int cplan_other_answers(Compiler *c, int cls, int sg, const char *name, int certain);
/* What node id, a Method's maker, meets as an instance of cls's accessor
   `name` (a writer's as `x=`), as its Method answers #owner and
   #source_location: node, the attr_* call it meets (-1: none); owner,
   the class or module #owner names (-1: not known); pos, 1 for node's
   line, 0 for none (nil). Where the record cannot tell which accessor
   id meets, node is the last attr_* call on cls's chain sure to run
   (elab_accessor_node) and owner the class whose body makes it. */
typedef struct { int node, owner, pos; } CplanAccessor;
void cplan_accessor_met(Compiler *c, int id, int cls, const char *name, CplanAccessor *a);
/* What call id of class method `name` on program class ci runs where ci
   has a `class << self` accessor of the name: CP_SG_NONE where it has none
   (the call is an ordinary class method's); CP_SG_SLOT, the slot, named
   into slot (the writer's base, the reader's name past an alias), where
   the call binds to no method or to the accessor's own version (one a
   later def replaces, Scope.attr_version) or where the record tells no
   version; CP_SG_METHOD where it binds to that def or to the entry that
   chooses between them at run time, which the call's emitter calls (an
   accessor no def follows has no method: its calls take the slot). */
enum { CP_SG_NONE, CP_SG_SLOT, CP_SG_METHOD, CP_SG_UNTOLD };
int cplan_sg_accessor(Compiler *c, int id, int ci, const char *name, char *slot, size_t cap);
/* For call node id of `name` on class ci, where a later def replaces ci's
   class-side accessor of the name (elab_attr_replaced): CP_SG_SLOT where
   the call binds to the accessor's own version, CP_SG_METHOD where it binds
   to the def or the entry between them, CP_SG_UNTOLD where the record tells
   no version (each emitter then keeps its answer without versions).
   CP_SG_NONE where no def replaces the accessor. */
int cplan_sg_version(Compiler *c, int id, int ci, const char *name);
/* Does call node id, written with `name` (not a send or a capture of it),
   run no method on class ci: the plan's answer for it is the class's
   accessor slot (cplan_sg_accessor). The reach pass then reaches no scope
   from it, though the version it binds to is the accessor's own. */
int cplan_sg_slot_call(Compiler *c, int id, int ci, const char *name);
/* The class whose own class methods `respond_to?` call id asks about: a
   constant receiver's, or self's in a class method or a class body; -1 for
   any other receiver, an instance's, and for a call of another shape
   (respond_to_cells_shape). */
int cplan_presence_class(Compiler *c, int id);
/* The version cells that answer whether class cls has its class method
   `name` where respond_to? call id runs: the cells of the version entry a
   call of the name there would take, where the record cannot order the
   call against a version (a method body's def, which may run any number
   of times). Each cell holds one of the chain's versions while that
   version is the method; only public ones unless include_all; and els,
   whether the entry's else arm, which the call runs while none is live,
   finds the method so (cplan_entry_finds). 1 with the
   cells in *out, 0 where the answer is the table's or the record's at the
   point (cplan_cmethod_later), for a chain whose entry arms an undef or
   a remove, and in a program that defines respond_to_missing?
   (respond_to_missing_defined). */
typedef struct { int n; char cell[VER_MAX_ARMS][32]; int val[VER_MAX_ARMS]; int els; } CplanCells;
int cplan_cmethod_presence(Compiler *c, int id, int cls, const char *name, int include_all, int own, CplanCells *out);
/* The class whose own or inherited class methods class-side list call id lists, when the cells may answer it
   (a `C.singleton_methods` on a constant, list_cells_shape; *own for the `(false)` form); -1 for any other. The
   survey asks each name it may list the entry a call there would take (cplan_presence_asks), so that
   cplan_list_cells gives its cells. */
int cplan_list_class(Compiler *c, int id, int *own);
/* The cells that answer whether class-side list id lists class method `name` of cls (own: its own versions): the
   public versions' cells, where cplan_cmethod_presence gives some and every version is public (a list shows
   public and protected methods); 0 where the table answers it. */
int cplan_list_cells(Compiler *c, int id, int cls, const char *name, int own, CplanCells *out);
/* Whether name answers on cls's chain where presence site id runs, as a
   call of the name there finds it, from call_target with CT_REACH (side: 0
   for an instance's chain, CT_CMETHOD for the class side, with CT_OWN for
   cls's own methods alone). PR_PRESENT: a method or an accessor; PR_ABSENT:
   none (the builtin tiers may answer), as the tables have it or because the
   record proves none is live where id runs; PR_TABLE: the record gives no
   proof and the reader keeps the final tables' answer, as in a block a
   call on a receiver of no one type may run later. out->table is that
   answer for every kind (a hidden method, a Struct's or Data class's
   generated iterator and, under CT_OWN, a module's copy are no method of the
   class's; an alias, a prepend's renamed body and a class-side accessor are,
   as the class's own entries); out->vis the visibility the call checks of
   it: the final table's, public where it declares none, until visibility is
   per version; the class's own entry's under CT_OWN. */
typedef enum { PR_PRESENT, PR_ABSENT, PR_TABLE } PresenceKind;
typedef struct { int table, vis; } CplanPresence;
PresenceKind cplan_presence(Compiler *c, int id, int cls, unsigned side, const char *name, CplanPresence *out);
/* The table part of that answer alone, with no site (table and vis; the
   record is not asked): what a list holds before any point is asked. */
void cplan_table_presence(Compiler *c, int cls, unsigned side, const char *name, CplanPresence *out);
/* Does method-list call id leave out name, a candidate the final tables
   list for cls's side (side as cplan_presence's: CT_CMETHOD for
   singleton_methods, CT_OWN for a `false` form): where the question proves
   that no method of it is live there although the tables hold one? A name
   with no version on cls's chain and side is a row's or a member's
   (version 0), which the row answers: it is not asked. Every other answer
   is the tables', the list's own visibility and undef filter among it,
   until visibility is per version and an undef answers at the point. On
   the instance side, a list call keeps every name exempt from the record's
   order (refl_any, lr_late_asked), so call_target answers there from the
   tables, the list with it: only the class side answers at the point. */
int cplan_list_omits(Compiler *c, int id, int cls, unsigned side, const char *name);
/* The names a method list on cls's side asks about, as the final tables
   hold them (a class's scopes, aliases and accessors, and for the class side
   those of its superclasses and extended modules unless CT_OWN), in the
   order a list shows them, a name once per sighting (the caller keeps its
   first). Each is asked cplan_presence's question with no site
   (cplan_table_presence) and handed on, with the visibility the question gives it
   (SP_VIS_*), where the question says cls holds it as a method: under CT_OWN
   its own entry. Instance side without CT_OWN: one class's methods, the
   copies included; a chain's walk is its caller's, which lists the core
   surfaces between classes. What a list leaves out of what it holds (the
   compiler's synthesized helpers, a class-side private method) is the
   list's, not the question's. */
typedef void (*CplanCandFn)(void *ud, const char *name, int vis);
void cplan_list_candidates(Compiler *c, int cls, unsigned side, CplanCandFn fn, void *ud);
/* What version entry scope e's else arm finds while no version of its
   chain is live, as respond_to? and a Method's maker see it
   (VerEntry.finds): a public method, a private or protected one, or none
   (NoMethodError, method_missing); -1 for a scope that is no entry */
enum { VER_FINDS_NONE, VER_FINDS_HIDDEN, VER_FINDS_PUBLIC };
/* What a version entry tells apart (Compiler.ver_entries, VerEntry.guard):
   the versions a chain holds for a call whose version is known only at
   run time; a later mixin's guarded copies (Scope.mixin_guarded); an open
   history's versions, for a call that may meet none yet; the versions of
   a layer of a mixin's copies that a super reaches (super_layer_entry) */
enum { VER_ENTRY_CHAIN, VER_ENTRY_GUARD, VER_ENTRY_HISTORY, VER_ENTRY_LAYER };
int cplan_entry_finds(const Compiler *c, int e);
/* The program's `self.new` a `new` call id on class ci runs, -1 for
   none: for an arm of a version entry, its pin (call_target); for a
   Struct or Data class, the one call_target finds where the
   call runs (its version entry, or the def where the record proves it made
   there, or made over all the statements a call in a method or a block may
   run in), its builtin constructor answering a call before the def and one
   the record cannot place (as the constructor answered every call
   before); for any other class the one call_target finds where the call
   runs, its version entry where the call may reach two or more of the
   class's `self.new`, so that inference, the emitter and the call plan
   name one method. */
int cplan_user_new(Compiler *c, int id, int ci);
/* The class method of program class ci that call id, named name, runs:
   for a `new` the one cplan_user_new answers (none where the builtin
   constructor answers a call before the class's `def self.new`), for any
   other name the one call_target finds there. */
int cplan_cmethod_target(Compiler *c, int id, int ci, const char *name);
/* How a NameError or NoMethodError of call id names self, into buf:
   "class U" or "module M" in a class or module body and "main" at the top
   level (the record's self where the call runs, elab_self_at), else "an
   instance of" the class ci the body is emitted for, else "main". NULL
   for an anonymous class, which names itself at run time. */
const char *cplan_self_text(Compiler *c, int id, int ci, char *buf, size_t cap);

int cplan_reopen_runs_builtin(Compiler *c, int id, int mi);
/* Does call id run, as the load record proves it, ahead of the def node dn
   at the program's top level (not in a method body, a block or a lambda
   run later)? */
int cplan_runs_before_def_node(Compiler *c, int id, int dn);

#endif
