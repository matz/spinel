/* Shared compiler state for the C Spinel compiler.
 *
 * In the single-binary design the analyzer and code generator share one
 * in-memory state object instead of serializing an IR file. This struct
 * is that object; its field set corresponds to the legacy .ir dump. It
 * grows milestone by milestone -- M2 adds method scopes (one Scope per
 * `def`, plus the top-level scope) on top of M1's node type cache.
 */
#ifndef SPINEL_COMPILER_H
#define SPINEL_COMPILER_H

#include "node_table.h"
#include "types.h"

/* require-gate (defined in spinel_parse.c). sp_feature_enabled(name) is 1 when
   feature `name` may be provided: always when the gate is off (g_require_gate
   == 0), else only if `require "name"` appeared in the program. Used to gate
   require-gated stdlib (stringio, io/console, ...) so they match CRuby's
   uninitialized-constant / NoMethodError when the require is absent. */
extern int g_require_gate;
void sp_feature_mark(const char *name);
int sp_feature_enabled(const char *name);
int        sp_feature_required(const char *name); /* require was actually written (gate-independent) */
/* Add a `-I <dir>` feature search root (see resolve_plain_requires). */
void sp_add_feature_root(const char *dir);

/* Method visibility (see ClassInfo.vis_names). Default/absent is public. */
enum { SP_VIS_PUBLIC = 0, SP_VIS_PRIVATE = 1, SP_VIS_PROTECTED = 2 };

/* Where a slot's type degraded to untyped: the answer to "why did this
   widen", which --warn-widen prints as notes under the warning and
   --emit-types carries as `why`. A poly is born at an expression and a slot
   only inherits it (#4509), so the record is the NODE whose value made the
   slot degrade; the node's own origin (Compiler.norigin) leads on from there
   to the expression the poly was born at. */
typedef struct {
  int node;      /* the argument, written value or returned value whose type degraded the slot; -1 = untraced */
  int other;     /* the node whose value last gave the slot the concrete type it had before; -1 = none */
  TyKind prev;   /* the slot's type just before it degraded (TY_UNKNOWN: it degraded from nothing) */
  TyKind then;   /* `node`'s type at that moment (its final type may differ: a transient) */
  int round;     /* the fixpoint round it happened on */
  const char *reason;  /* a rule's own words, when a rule rather than a value degraded the
                          slot (a `= nil` default, no call site, a splat); NULL otherwise.
                          `node` is then the rule's subject (the default) or -1 */
} SlotWhy;

typedef struct {
  char *name;       /* Ruby local name (without sigil) */
  TyKind type;      /* inferred type */
  SlotWhy why;      /* how `type` came to be untyped, if it is */
  int last_src;     /* the node whose value last set `type` while it was concrete; -1 = none */
  int gc_root;      /* scratch during analysis; rooting is type-derived in codegen */
  int is_param;     /* declared as a method parameter (C function param) */
  int is_block_param; /* bound by a block; typed by block-param inference */
  int proc_ret;     /* when type==TY_PROC: the proc's body return type (TyKind),
                       TY_UNKNOWN if not statically known */
  int is_cell;      /* captured by an escaping proc: lives in a heap cell
                       (sp_int *_cell_<name>) so the closure and the enclosing
                       scope share mutable storage */
  int cell_outlives; /* the cell was made for a proc that can OUTLIVE the call
                       (a proc/lambda, a Fiber/Thread body, a handler, a block
                       forwarded into a poly callee). A cell only lifted
                       iteration blocks made is consumed while the call runs,
                       so a byref parameter may still lend its slot (#4568). */
  int proc_rebinds; /* a proc or block that captures it assigns the cell:
                       any later call may run it and rebind the local, so a
                       read ahead of one is taken first (read_rebound_by) */
  int cell_shadow;  /* the cell belongs to a param of an INLINED iteration
                       block, which the loop emitters bind by writing the plain
                       C slot. Keep that slot alongside the cell and copy it in
                       at the top of the body, so a proc lifted out of the block
                       reads this iteration's value through the cell. */
  int byref_out;    /* (params) a string param the method body mutates in place
                       (`<<`/bang mutators): passed as const char** so the
                       mutation lands in the caller's variable, like CRuby's
                       shared-object semantics. Implies is_cell (body reads and
                       writes go through *_cell_<name>), but the cell is the
                       caller's slot -- no heap cell is allocated on entry. */
  int inline_alias; /* (params of a yielding method, codegen only) how many
                       inline expansions currently in progress bind this
                       parameter as an ALIAS of the caller's variable rather
                       than a copy: is_cell is held at 1 for their duration so
                       the body's reads and writes go through *_cell_<name>,
                       which the expansion points at the caller's slot. A
                       String the body appends to has to be the caller's, not
                       a copy that goes stale on the first reallocation. */
  int init_guarded; /* (consts) initialized via `CONST = Class.new(...)`: reads
                       during the init raise NameError (uninitialized constant) */
  int rbs_seeded;   /* param type pinned from an --rbs advisory seed: the
                       fixpoint must not widen it (see apply_rbs_seeds) */
  int nullable_int; /* an int local that was assigned a value which can be the
                       nil sentinel (a search miss, a pop off an empty array):
                       boxing it has to yield nil, not INTPTR_MIN */
  int nil_passed;   /* (proc params) some call passes a literal nil: an
                       Integer another call passes keeps the param an
                       Integer, which the nil then no longer overrides as it
                       does the bare-int guess (cs_type_params) */
  int site_boxed;   /* (block params) 1 when what its sites bind boxed it,
                       2 when it was boxed before they did (see
                       block_settle_types, narrow_site_boxed_block_params) */
  int obj_nilable;  /* an object-typed parameter some call site passes nil:
                       a user method called on it has to raise NoMethodError
                       for nil rather than run with a NULL self (#5088) */
  int box_nullable; /* an int parameter bound from an ivar that can be read
                       before anything assigned it: only BOXING it has to
                       yield nil. Kept apart from nullable_int, which also
                       arms the typed nil checks (optcarrot's CPU reads such
                       ivars on its hot path, #5085) */
  int arr_or_nil;   /* a poly slot proven to hold only a poly array or nil, so
                       an index read of it takes the runtime's inline array arm
                       -- which neither allocates nor re-enters Ruby code. That
                       is what lets its GC root be elided: the value stays
                       reachable from the container it was read out of. */
  int nullable_int_elem; /* the same, one level in: an int/float ARRAY local
                       some element of which can be the sentinel, so reading an
                       element or binding a block parameter from it carries it
                       out (#3505) */
  int bounded_counter; /* (codegen, lazily) 1: an Integer local that only ever
                          takes a small literal or `+= / -= <small literal>`
                          inside iterator blocks over containers (never a
                          while/until/loop), so it cannot overflow the word
                          and its adds need no check; -1: not; 0: unknown */
  int poly_dispatch_widened; /* (params) a receiver that settled on no type
                       reaches this parameter with an argument that settled on
                       none either, so the value arrives BOXED and the arm's
                       conversion would take it apart as whatever another call
                       site pinned. It must stay POLY -- and survive the
                       re-narrow reset, which clears poly params (#4294) */
  int push_widened; /* (params) a push through this parameter carried an element
                       its bound type could not hold, so it must stay the POLY
                       ARRAY: the call-site unification would otherwise collapse
                       it to the poly SCALAR and lose the container (#2989) */
  TyKind boxed_push_elem; /* (params, boxed) the element type a push through
                       this BOXED parameter carried, unified across the pushes
                       (TY_POLY if they disagree, TY_UNKNOWN if none). The
                       container is invisible at the call site when the
                       parameter arrives poly, so the caller's slot cannot be
                       checked there; this carries the evidence out to the
                       ivar-widening pass, which knows both sides. An
                       Integer-keyed element write counts as a push. */
  TyKind boxed_known_elem; /* (params, boxed) the same, of the stores whose
                       kind is known: a value decided at run time is checked
                       as it lands (#4481), and a concat of no Array raises
                       rather than stores. What a boxed argument's arrays
                       widen for (widen_boxed_array_sources). */
  TyKind boxed_store_key, boxed_store_val; /* (params, boxed) the key and the
                       value an element write through this BOXED parameter
                       stores, unified as boxed_push_elem is: the evidence
                       the binding checks a caller's hash against. */
  unsigned long long store_key_src, store_val_src; /* (params, containers)
                       bit j set: an element write through this parameter
                       stores the method's own positional parameter j as
                       the key (value). Boxed when the callers disagree, so
                       the binding checks each call's argument j against the
                       container instead. */
  int store_rest_src; /* (params, containers) one past the first element of the
                       method's rest parameter that a push, unshift or insert
                       through this parameter stores, or 0: the rest
                       collects them boxed, so the binding checks each call's
                       own rest arguments against the container. */
  int const_def_write; /* (consts) has a definite (non-or/and) assignment; an
                          or/and-write-only const is nil-defaulted (poly) so its
                          `||=` truthiness check fires on first use */
  int or_written; /* some write to this local is a `||=`, which can run before
                     any definite assignment (`v ||= 5; v += 2`, a definite
                     write in one branch only, a block local reset each
                     iteration), so its truthiness check must be able to see
                     nil. The slot is declared with its type's nil sentinel
                     rather than the zero value, so a kind whose zero is a
                     real value (sp_int 0, 0.0) can still tell the two apart
                     (mirrors ConstantVar's const_def_write). */
  int maybe_unset;  /* (Integer / Float) some read can run before any write:
                       every write to it ahead of the read is conditional (a
                       modifier `if`, one branch, a loop body), so the read
                       answers nil, and the slot starts as its nil sentinel
                       as an or-written one does */
  int str_shared;   /* (TY_STRBUF) a shared-mutable string: it is aliased
                       (`s2 = s1`) AND mutated in place, so the whole alias set
                       holds the one sp_String* handle -- reads hand out the live
                       buffer (no copy), assignment copies the handle, and
                       `equal?` is handle identity, matching CRuby's mutable
                       String object semantics (#3227). Plain TY_STRBUF (this
                       flag 0) stays the copy-on-read build-in-a-loop refinement. */
  int dyn_handle;   /* (params, TY_STRBUF) the shared handle because a Method
                       naming the method (`method(:m)`) can reach it and the
                       method appends to it (#6179). A literal or a temporary a
                       caller passes has nobody else holding it, so it goes over
                       as a handle with its bytes inside the object
                       (sp_String_new_fresh), not a separate malloc'd block. */
  int poly_lift;    /* (params, TY_POLY) POLY_LIFT_APPENDED: appended to in
                       place, by the method itself or by a method it hands
                       the parameter to (its read lifted into the handle,
                       poly_strbuf_lift), so its callers hand over the handle
                       (convert_byref_handle_params). POLY_LIFT_ZSUPER: a
                       bare `super` hands it to such a parameter and it can
                       hold a String, so the super lifts it as a marked read
                       is (emit_zsuper_arg). (#6179) */
  int str_append;   /* (TY_STRBUF) an append accumulator: appended to inside a
                       loop and never read inside one, so the growable handle
                       makes each `<<` amortized O(1) instead of copying the
                       whole accumulation. Durable, like str_shared: the type
                       is re-asserted after the fixpoint, which a later
                       assign-based pass would otherwise overwrite. */
  int poly_hash_pin; /* an empty-`{}` local handed to a TY_POLY parameter: the
                       callee writes through the reference with the boxed
                       accessors, which only persist to a PolyPoly hash, so the
                       reverse binding types the caller's slot PolyPoly (#3158).
                       Durable, like oa_pin: the slot's own element writes
                       re-derive a narrower kind every round, and the binding
                       widened it back -- to the cap. */
  int poly_array_pin; /* a local whose writes are all array literals, handed
                       to a parameter an element write widened to the general
                       Array: the reverse binding types it that Array, and the
                       type is re-asserted each round, as poly_hash_pin is,
                       since the literals re-derive the narrower kind. */
  TyKind oa_pin;    /* the pointer-array type the narrowing pass gave this slot,
                       re-asserted on every fixpoint round. infer_write_types
                       clears every local back to UNKNOWN and re-derives it from
                       the writes, which still read the poly array -- and the
                       two array kinds unify to the plain poly SCALAR, strictly
                       worse than either. TY_UNKNOWN = not narrowed. */
  unsigned char oa_grace; /* narrow_object_arrays reached no decision on this
                       slot last round and kept its own pin once; a second
                       such round drops the pin (#4962) */
  TyKind rbs_type;  /* the type an --rbs seed declared for this slot, kept
                       beside `type` because inference may narrow the slot
                       afterwards. A narrowing of an `untyped` (poly) seed is
                       not a declaration, so it must not be trusted the way a
                       concrete one is: taking it as declared made a
                       String-passing call site reinterpret the pointer, and
                       the program segfaulted with no diagnostic (#3977).
                       TY_UNKNOWN = no seed. */
  int poly_ctr;     /* (TY_POLY) a builtin Array or Hash is among the values
                       that flow into this slot. TY_POLY is a top type with no
                       member list, so a call on it cannot otherwise tell
                       "union that includes a container" from "user object the
                       fixpoint has not pinned down yet". Without the
                       distinction a user class owning a container method name
                       stripped the builtin answer from the dispatch (#3459),
                       and widening unconditionally instead poisoned classes
                       whose poly slots never hold a container (Set's @data). */
} LocalVar;
#define POLY_LIFT_APPENDED 1
#define POLY_LIFT_ZSUPER   2

typedef struct {
  char *name;       /* method name; NULL for the top-level scope */
  int def_node;     /* DefNode id; -1 for top-level */
  int body;         /* StatementsNode id (-1 if empty) */
  int class_id;     /* owning class index, or -1 for free functions */
  int yields;       /* body contains a YieldNode (inlined at call sites) */
  int reachable;    /* method name is referenced somewhere (else dead code) */
  int is_cmethod;   /* `def self.foo`: a class (singleton) method, no instance self */
  int is_module_function; /* `module_function`: ALSO a private instance method of
                             every includer. The body cannot depend on the
                             receiver (its self differs per call form), so the
                             one emitted function serves both spellings and a
                             receiverless call in an includer is rewritten onto
                             it rather than cloned (#3734). */
  int is_transplanted_source; /* method was copied into another class via include/prepend */
  int origin_module_ci;      /* +1-based module this copy came from (0 = none):
                                Method#owner names the module, not the includer */
  int is_include_copy;        /* this scope IS such a copy: a later include of a
                                 module defining the same name replaces it, and
                                 the replacement's super chains to it (#3731) */
  int is_extend_copy;         /* the class method an `extend` copied in: a later
                                 extend of a module defining the same name
                                 replaces it the same way */
  int is_proc_form;  /* a clone of a yielding method whose `yield` is a call on
                        a real &blk parameter, for the poly dispatch. Its body
                        is typed independently of the inlined original: the
                        yield answers poly, so everything it feeds widens with
                        it and one body serves every call site (#3399). */
  int is_lowered_yield; /* self-recursive yield method lowered to &block (sp_Proc) form */
  int lowered_lifted_yield; /* lowered because a yield sits in a Thread/Fiber
                               body: the method's value is its own tail, not
                               the block's, unlike the self-recursive form */
  int lowered_carries_block_value; /* the self-recursive lowering, for a method
                               whose RETURN VALUE comes out of a `yield`
                               (`return yield if ...`). Then the call site is
                               typed from the block, and the function's own C
                               type is the raw slot the proc side-channel hands
                               back. A lowered method whose tail is its own
                               expression carries THAT, and both the call site
                               and the signature follow `ret` (#4145). */
  int blk_param_value_use; /* the named &block is read for its VALUE -- handed
                            to another method, assigned to a local, captured by
                            a nested proc. Not merely unapproved for splicing:
                            `blk.nil?`, `!blk` and a bare `blk` in a condition
                            ask only whether a block was given, which an inline
                            site answers by folding. A real value use has no
                            such answer -- the block must be an sp_Proc *. */
  char *blk_param;  /* name of the `&block` parameter, or NULL (anon -> "") */

  /* Compile-time `define_method` unrolling: a method synthesized from
     `[lits].each { |v| define_method("m_#{v}") { body } }`. Within this
     method's body a read of `dm_subst_name` is the literal at node
     `dm_subst_node` (the loop value for this unrolled instance). */
  char *dm_subst_name;
  int dm_subst_node;

  /* Synthesized compiler_state method (no AST body): codegen emits a
     hand-built body looping over the owning class's compiler_state entries.
     0=not synthesized, else CS_SYNTH_* (see codegen). */
  int cs_synth;

  char **pnames;    /* parameter names, in order (requireds then optionals) */
  int *pdefault;    /* per-param default-value node id, or -1 if required */
  int nparams;
  int nrequired;    /* index past the LAST required param, not a count:
                       scope_add_param sets it to nparams on every required
                       one, so `def f(x = 1, y)` reports 2. Kept as-is because
                       three dozen sites read it that way. */
  int rest_idx;     /* index in pnames[] of *rest param, -1 if none */
  int npost_rest;   /* number of required params AFTER the rest param (Prism "posts") */
  int kwrest_idx;   /* index in pnames[] of **kwrest param, -1 if none */
  int fwd_target1;  /* a `def m(...)` the __fwd_N model binds (#1288): the
                       scope its `...` reaches, plus one; 0 if none */

  TyKind ret;       /* inferred return type */
  SlotWhy ret_why;  /* how `ret` came to be untyped, if it is */
  int ret_poly_ctr; /* the return value can be a builtin Array/Hash even
                       though `ret` collapsed to poly (see LocalVar.poly_ctr) */
  int ret_specialized; /* ret was set by specialization (inherited-cls-new copy);
                          don't overwrite it from the shared body in the fixpoint */
  TyKind ret_noblock;  /* a yielding method's value when called WITHOUT a block:
                          the type of its `return x unless block_given?`
                          returns, kept out of `ret` so a call with a block
                          (which the inliner specializes, folding the guard
                          away) reads the value of the body proper. UNKNOWN
                          when the method has no such guard. */
  int is_ext_entry;    /* designated --ext-entry: emitted non-static, a DCE
                          root, declared in the emitted extension header (#M1
                          of docs/internals/ext-design.md) */
  int ret_rbs_seeded;  /* ret pinned from an --rbs advisory seed: the fixpoint
                          must not recompute it from the body */
  int ret_rbs_nilable; /* that seed was RBS's nilable form (`Integer?`), and the
                          pinned kind is an unboxed scalar: the return can be
                          the reserved sentinel, so a caller boxing it has to
                          answer nil rather than the raw number (#3493) */
  int ret_nullable_int; /* the same property, INFERRED rather than seeded: this
                           method's scalar return can be the reserved sentinel
                           because its own return expression can be. Seeded from
                           ret_rbs_nilable, then propagated through pass-through
                           methods (`def pass(x) = x.p_`) and methods whose value
                           is their block's (`def key_of(x) = yield x`), which no
                           RBS signature covers (#3505). */
  TyKind ret_oa_pin;   /* the pointer-array return type the narrowing pass gave
                          this method, re-asserted every round for the same
                          reason LocalVar.oa_pin is. TY_UNKNOWN = not narrowed. */
  unsigned char ret_oa_grace;   /* LocalVar.oa_grace for the return slot */
  int ret_poly_array_pin; /* the method's value reaches a parameter that stores
                          elements of another kind into it, so the arrays it
                          answers are built as the general Array
                          (widen_array_sources). Re-asserted each round, as
                          LocalVar.poly_array_pin is: the values re-derive
                          their typed kinds, and a new array of another kind
                          (`Array.new(n, 0)`) is converted where it is
                          returned, which nothing else holds to notice. */
  int ret_proc_ret; /* when ret==TY_PROC: the returned proc's body return type
                       (TyKind), so a caller's `m.call` knows the result type */
  int blk_ret;      /* for a method with a &block param: the unified value type
                       its block yields across all call sites, so blocks passed
                       to it are emitted returning that (common) type */

  LocalVar *locals; /* params + body locals */
  int nlocals, clocals;
} Scope;

typedef struct {
  char *name;          /* class name ("Point"); also drives semantic checks
                          (builtin reopen, name matching) and Ruby-visible output */
  char *c_name;        /* C-identifier stem for `sp_<c_name>` emission: == name,
                          unless name collides with a runtime typedef (sp_RbVal,
                          sp_IntArray, ...) in which case it is disambiguated so a
                          user `class RbVal` does not redefine the runtime type */
  int def_node;        /* ClassNode id */
  int parent;          /* superclass index, or -1 */
  char **ivars;        /* instance variable names, incl. leading '@' */
  TyKind *ivar_types;
  unsigned char *ivar_str_shared; /* (#3227) the slot is a shared-mutable
                                     string handle (sp_String *): survives
                                     re-clears; post-fixpoint reasserts it */
  unsigned char *ivar_nullable_int; /* the slot holds a nilable scalar: some
                                     write to it can leave the nil sentinel, so
                                     a read of it (or of its attr_reader) has to
                                     box as nil rather than as the number. Set
                                     by the marking fixpoint, alongside
                                     LocalVar.nullable_int and
                                     Scope.ret_nullable_int (#3505). */
  unsigned char *ivar_arr_elem_arr_or_nil; /* every element of this container
                                     slot is a poly array or nil (see
                                     LocalVar.arr_or_nil) */
  unsigned char *ivar_nullable_int_elem; /* the same one level in: an int/float
                                     ARRAY slot some element of which can be
                                     the sentinel */
  TyKind *ivar_oa_type;  /* the homogeneous pointer-array type narrow_object_arrays
                            gave the slot (a table of int arrays or an array of one
                            user class), TY_UNKNOWN when it made no decision. Set
                            together with ivar_int_table, which is the pin every
                            re-derivation site honours; this is the type the
                            per-round re-assert restores (#4444). */
  int *ivar_oa_seed;            /* an --rbs seed asked for one of the unboxed
                                   pointer arrays: 1 + the class index for
                                   `Array[Class]`, or SEED_OA_INT_TABLE /
                                   SEED_OA_FLT_TABLE for `Array[Array[Integer]]`
                                   / `Array[Array[Float]]`. It is not a type pin:
                                   the narrowing pass decides from the uses, and
                                   a request it could not honour is reported
                                   after the fixpoint (#4444). Pinning a nested
                                   array's type instead would be worse than
                                   saying nothing -- the seed's own type would
                                   stop the pass that produces it. A nested seed
                                   is instead ELEMENT EVIDENCE inside that pass,
                                   so it supplies the row kind when the rows are
                                   empty literals, and a row of the other kind
                                   is a contradiction (#4484). */
#define SEED_OA_INT_TABLE (-1)
#define SEED_OA_FLT_TABLE (-2)
  unsigned char *ivar_oa_conflict;  /* a nested-array seed met a row of another
                                       kind in narrow_object_arrays: the
                                       signature and the program disagree,
                                       reported after the fixpoint (#4484) */
  unsigned char *ivar_int_table;  /* the slot is a table of int arrays, narrowed
                                     to TY_INT_ARRAY_ARRAY while the fixpoint
                                     runs so a parameter bound from `@t[k][j]`
                                     sees an Integer. The write it was derived
                                     from still reads TY_POLY_ARRAY, and those
                                     two array kinds unify to the plain poly
                                     SCALAR -- so without this flag the next
                                     write pass replaced the narrowed type with
                                     something strictly worse. */
  int nivars, civars;
  char **rbs_pin_ivars; /* ivar names (incl '@') pinned by an --rbs seed: the
                           fixpoint must not widen their type */
  int n_rbs_pin_ivars, c_rbs_pin_ivars;
  char **cvars;        /* class variable names, incl. leading '@@' */
  TyKind *cvar_types;
  unsigned char *cvar_nullable_int; /* the Integer or Float class variable can
                                       hold the nil sentinel, as
                                       ivar_nullable_int for an ivar */
  int ncvars, ccvars;
  char **readers;      /* attr reader method names (no '@') */
  int nreaders, creaders;
  char **writers;      /* attr writer base names (no '@', no '=') */
  int nwriters, cwriters;
  char **undefs;       /* method names removed via `undef` */
  int nundefs, cundefs;
  /* Method visibility: parallel name/kind arrays (SP_VIS_*). Covers both
     def-defined methods and attr readers/writers (writers stored as "x=").
     A name absent here is public; an explicit entry records private/protected
     (or an explicit re-`public`). Populated by register_method_visibility. */
  char **vis_names;
  int  *vis_kinds;
  int nvis, cvis;
  /* The same for class (singleton) methods: `class << self` sections and
     `private_class_method` / `public_class_method`. */
  char **cm_vis_names;
  int  *cm_vis_kinds;
  int ncm_vis, ccm_vis;
  /* class << self attr_accessor/reader/writer: singleton-level accessors
     stored in static globals (cst_<Class>_<field>), not in per-instance ivars */
  char **sg_readers;   /* singleton reader names */
  int nsg_readers, csg_readers;
  char **sg_writers;   /* singleton writer base names (no '=') */
  int nsg_writers, csg_writers;
  /* singleton accessors whose storage IS the class-level ivar: the class body
     assigns `@x` and `class << self` declares an accessor for it, so both
     must read one slot (civ_<Class>_<x>) rather than diverging (#3776) */
  char **sg_civ;
  int nsg_civ, csg_civ;
  /* singleton accessor names inherited from an ancestor's `class << self`
     (resolve_parents copies them in): reflection lists them only with all */
  char **sg_inh;
  int nsg_inh, csg_inh;
  char **alias_new;    /* `alias new old`: alias_new[i] redirects to alias_old[i] */
  char **alias_old;
  int   *alias_cls;    /* class the lookup resumes from (an alias of an
                          INHERITED method keeps naming the ancestor's body
                          even when this class redefines the name), or -1 */
  int   *alias_node;   /* the alias statement's node id, so the pass that runs
                          once superclasses are wired can fill alias_cls */
  int   *alias_builtin; /* 1: the alias captured the builtin method of a
                           reopened primitive, which the class had not
                           defined where the alias appeared */
  int naliases, caliases;
  int enum_yield_arity; /* widest `yield` arity in this class's each, so the
                           Enumerable collector packs a multi-value yield into
                           one element and `for` still binds only the first */
  int enum_yield_packed; /* its yields differ in count (or splat): an element
                            is the single value itself, several an Array */
  int is_struct;       /* defined via Struct.new(:a, :b): readers[] are the
                          positional members; the constructor takes them in
                          order and there is no user `initialize`. */
  int is_data;         /* defined via Data.define(...): a Struct-like value class
                          that additionally supports the `#with` copy-update. */
  int nmembers;        /* is_struct/is_data: ivars[0..nmembers) back the
                          members, in declaration order; any later ivar is an
                          attr's or a method's own instance variable. */
  int is_anon_struct;  /* k = Struct.new(:a, :b): synthesized for a local-held
                          anonymous struct class; #inspect omits the name. */
  int is_singleton_of; /* +1-based parent class index (0 = not one): a
                          synthesized anonymous subclass carrying a constant's
                          / local's singleton methods (def obj.m). It must
                          masquerade as its parent everywhere Ruby-visible
                          (#class, inspect, name), like CRuby's hidden
                          singleton class. See singleton_visible_ci. */
  int kw_init;         /* Struct#keyword_init?: 0 unspecified (nil), 1 true,
                          -1 explicit false. */
  /* Native-bound class (Path B typed object): C-backed, declared by a package
     via native_struct/native_new/native_method. It is a first-class object
     (ty_object(i), a runtime cls_id, GC-managed) whose methods dispatch to
     declared C symbols instead of generated bodies. c_struct is the carried C
     struct name; free_sym its optional finalizer. Method bindings live in the
     compiler's native_methods registry, keyed by this class's index. */
  int is_native_class;
  char *c_struct;      /* e.g. "sp_StringIO", or NULL */
  char *native_free;   /* finalizer C symbol, or NULL */
  int freeze_observed; /* freeze/frozen? reaches instances of this class: codegen
                          guards its ivar stores with the GC-header frozen bit */
  int is_value_type;   /* small immutable scalar-ivar class represented by value
                          (sp_X, not sp_X *): no heap alloc / GC. Set by
                          detect_value_types after analysis. */
  int ctor_reachable;  /* the early census (compute_instantiated before the type
                          fixpoint): a construction site of this class sits in
                          reachable code, or nothing can tell. Read only by the
                          poly-receiver return-type union, for native classes. */
  int instantiated;    /* a value with this exact cls_id can come into existence
                          somewhere: `.new`/`.allocate`/`raise Cls`/Struct, or a
                          Marshal.load that can mint any class. When clear, no
                          poly value is ever this class, so the poly-dispatch
                          switch can drop its `case` arm (the referenced method
                          then becomes an unreferenced static the C compiler
                          DCEs). Set by compute_instantiated. */
  /* Prepend shadow chain: when `prepend M` is called on this class,
     M's methods overwrite the active slot; the previous slot is
     renamed to a shadow `__prep_N_<m>`.  The chain maps each name
     (user name or shadow name) to the next shadow in the chain,
     so `emit_super` can follow it rather than the parent class. */
  char **prep_from;      /* source name in the chain link */
  char **prep_to;        /* target shadow name */
  int nprep_chain, cprep_chain;
  int prep_shadow_count; /* next shadow index to assign */
  int enclosing_class;   /* index of enclosing module/class, or -1 for top-level */
  char *ruby_name_cache; /* the qualified Ruby name, built once by
                            class_ruby_name. Cached so the pointer it returns
                            STAYS VALID: it used to be a shared static buffer,
                            and a caller that held the name across an
                            emit_expr got whichever class that emission asked
                            about instead. */
  /* Modules included into this class (class indices), recorded when the
     include transplants methods. Consulted by `rescue M` matching: an
     exception matches a module arm when its class (or an ancestor)
     includes that module. */
  int *included_mods;
  int nincluded_mods, cincluded_mods;
  /* Modules this class extends (class indices), in the order register_extends
     meets them: its singleton's ancestors, which `Klass.is_a?(M)` asks. */
  int *extended_mods;
  int nextended_mods, cextended_mods;
  /* Modules with no class of their own: a BUILTIN one named through its path,
     `include IO::WaitReadable`. Those carry no index into c->classes, and the
     rescue match compares module NAMES anyway, so the qualified string is
     what gets recorded. */
  char **included_mod_names;
  int nincluded_mod_names, cincluded_mod_names;
  /* compiler_state_* declared fields: parallel arrays of field name (no '@')
     and kind ("int"/"str"/"sa"/"ia"). codegen's synthesized init/dump/set
     methods iterate these. */
  char **cs_names;
  char **cs_kinds;
  int ncs, ccs;
} ClassInfo;

/* One `ffi_func` declaration. ret: "ptr"/"int"/"float"/"double"/"str"/"void"/
   "size_t"/"long"/"bool". args: malloc'd array of arg specs. */
typedef struct {
  char *mod;       /* module name */
  char *name;      /* Ruby-visible function name */
  char *csym;      /* C symbol when it differs from name (the ffi gem's
                      4-arg attach_function rename form), or NULL */
  char *ret;       /* return spec */
  char **args;     /* arg specs array (malloc'd) */
  int nargs;
  int blocking;    /* `blocking: true`: the call may block for a while and touches
                      no Ruby object, so the worker leaves the world for it
                      (sp_native_enter/leave): a collection does not wait for it */
} FfiFunc;

typedef struct { char *mod; char *name; int val; } FfiConst;     /* ffi_const */
typedef struct { char *mod; char *name; int size; } FfiBuf;      /* ffi_buffer */
typedef struct { char *mod; char *name; int offset; char *kind; } FfiReader; /* ffi_read_* ("u32"/"i32"/"ptr") */
/* ffi_callback :name, [arg_specs], ret_spec -- a C function-pointer type. A
   method(:sym) / non-capturing lambda passed to an arg of this type becomes a
   compile-time trampoline that boxes the C args, calls the compiled method, and
   converts the result back. */
typedef struct { char *mod; char *name; char **arg_specs; int nargs; char *ret_spec; } FfiCallback;
typedef struct { char *name; char *spec; } FfiField;                 /* one ffi_struct member */
typedef struct { char *mod; char *name; FfiField *fields; int nfields; } FfiStruct; /* ffi_struct */
/* ffi_struct method dispatch (see ffi_struct_method, declared below Compiler). */
enum { FFI_SM_NONE = 0, FFI_SM_NEW, FFI_SM_GET, FFI_SM_SET };
typedef struct { char *mod; char *names; } FfiLib;   /* names: ;-separated lib names, or "" */
typedef struct { char *mod; char *val; } FfiCflag;   /* val: ;-separated cflags, or "" */
typedef struct { char *mod; char *val; } FfiSource;  /* ffi_source inline C translation-unit fragment */

/* One `native_func` declaration (typed static binding to carried C). Unlike
   FfiFunc, arg/ret specs are the spinel type language ("any"/"string"/"int"/
   "float"/"bool"; ret also takes "cstring" -- a borrowed C string, e.g. a
   static buffer, that the call site dups onto the GC heap) and csym is the
   C symbol to call. feat is the require-gate feature name from the module's
   `native_lib`, or "" (always available). */
typedef struct {
  char *mod;       /* module name */
  char *name;      /* Ruby method name */
  char *ret;       /* return type spec */
  char *csym;      /* C symbol to emit */
  char *feat;      /* require-gate feature name, or "" */
  char **args;     /* arg type specs (malloc'd) */
  int nargs;
} NativeFunc;

/* One `native_obj` declaration: a carried C object the package links on demand
   (only when its module's feature is required). path is root-relative, e.g.
   "packages/json/sp_json.o"; feat is the require-gate feature or "". */
typedef struct { char *mod; char *path; char *feat; } NativeObj;

/* One `native_method`/`native_new` binding on a native class. class_id indexes
   the class table; kind 0 = instance method, 1 = constructor (class method
   `new`). Arity-keyed: several entries may share a name with different nargs.
   ret uses the spinel type language plus "string?"/"poly?" nullable specs.
   csym is the C symbol; NULL ret means the emitted call yields no value. */
typedef struct {
  int class_id;
  int kind;        /* 0 = instance, 1 = constructor */
  char *name;      /* Ruby method name */
  char *ret;       /* return type spec, or "" */
  char *csym;      /* C symbol to call */
  char **args;     /* arg type specs */
  int nargs;       /* fixed arguments, not counting a trailing :rest */
  int rest;        /* a trailing :rest takes every further argument, boxed,
                      as a count and an array after the fixed ones */
} NativeMethod;
/* Whether a binding accepts a call of argc positional arguments. */
static inline int native_takes(const NativeMethod *m, int argc) {
  return m->nargs == argc || (m->rest && argc > m->nargs);
}

typedef struct {
  const NodeTable *nt;
  struct FwdAnalysis *fwd_analysis; /* owned forwarding proofs, freed with this compiler */
  TyKind *ntype;    /* [node_cap] node id -> inferred type */
  int *norigin;     /* [node_cap] node id -> where its degraded type came from: the
                       child (or the slot's why.node, for a read) that carried the
                       poly in, itself when the poly was born here, -1 when the
                       type is not degraded. Set where ntype is, in infer_type. */
  unsigned char *strbuf_box; /* [node_cap] LocalVariableReadNode of a shared-
                          mutable string in a container-store position: the
                          read yields the sp_String* HANDLE (typed TY_STRBUF,
                          boxed SP_BUILTIN_STRBUF), not the demoted cstr
                          (#3227 phase 3) */
  unsigned char *strbuf_handle_demand; /* [node_cap] the same demand -- hand out
                          the HANDLE, not a reading of it -- carried WITHOUT
                          moving the node's type. strbuf_box above is read as
                          the type as well: infer_uncached answers TY_STRBUF
                          for a marked node and comp_ntype keeps TY_STRBUF only
                          while the mark is set. The emitters dispatch on the
                          receiver's type, so marking a receiver moves the call
                          out of the surface that answers it -- `equal?` over a
                          reader left the String arm entirely. This array says
                          only what the mark was for (#4363). */
  unsigned char *strbuf_read_raw;  /* [node_cap] a shared-handle read whose
                                      only consumer READS the bytes and keeps
                                      no pointer past the call: emit
                                      sp_String_cstr, not a whole-string copy.
                                      The node's TYPE is unchanged (#5745). */
  unsigned char *poly_strbuf_lift; /* [node_cap] a read of a POLY variable
                          handed to a parameter the callee appends to in
                          place: a plain String the variable holds becomes
                          the shared handle (an SP_BUILTIN_STRBUF box) and is
                          stored back before it is passed, so the callee's
                          append and the caller's variable are one String
                          (sp_poly_strbuf_lift). The node's TYPE is
                          unchanged. POLY_LIFT_APPENDED preserves this demand
                          through alias propagation. */
  TyKind *nilnarrow; /* [node_cap] param-read narrowed by a `return .. if p.nil?`
                        guard: the read's non-nil type (codegen unboxes the poly
                        slot at the read site); TY_UNKNOWN = not narrowed */
  int *nscope;      /* [node_cap] node id -> owning scope index */
  int *node_cbody;  /* [node_cap] node id -> enclosing class/module-body class id, or -1 */
  char *empty_arr_recv; /* [node_cap] empty `[]` used as a direct receiver/interpolation -> TY_POLY_ARRAY */
  char *empty_hash_recv; /* [node_cap] empty `{}` used as a direct receiver/interpolation -> TY_STR_POLY_HASH */
  char *empty_hash_arg;  /* [node_cap] empty `{}` passed as a user-method arg -> TY_POLY_POLY_HASH */
  char *store_misfit_arg; /* [node_cap] a typed array passed to a boxed parameter
                          the method stores an element into that the array
                          cannot hold: codegen refuses it unless the binding
                          widened it */
  char *ivar_widen_src; /* [node_cap] an ivar read, or a reader call, that a
                          caller's array comes from (through locals and method
                          values) when a parameter it reaches stores elements
                          of another kind into it: widen_ivars_from_pushed_params
                          widens the ivar to the general Array, after the
                          fixpoint, as it does one passed directly */
  TyKind *hash_want; /* [node_cap] variant a hash literal should take from its use context (#3040) */
  TyKind *arr_want;  /* [node_cap] array kind a node takes from its use context
                        rather than from its own contents: an empty `[]`
                        literal, or a `map` that narrow_object_arrays decided
                        builds a table of rows */
  TyKind *poly_builtin_ty; /* [node_cap] for a container read on a poly receiver a
                              user class also owns: the type the builtin surface
                              alone would give, so codegen can shape its arm (#3459) */
  const struct BuiltinOp **bop_inf; /* [node_cap] the builtin-op row inference
                                       answered the call with (--plan-check only) */
  int *hash_default_arg_memo; /* [node_cap] hash_new_default_arg(node) memo; INT_MIN = uncomputed */
  unsigned hash_default_arg_memo_gen; /* scope-index generation the memo was built for */
  int hash_default_arg_memo_cap;      /* allocated length of hash_default_arg_memo */
  int node_cap;     /* allocated length of ntype/nscope (>= nt->count) */

  Scope *scopes;    /* scope[0] = top level */
  int nscopes, cscopes;

  /* the subset of `classes` synthesized for an anonymous Struct/Data, so
     resolving one does not walk every user class; invalidated wherever
     is_anon_struct is set */
  int *anon_struct_ids;
  int n_anon_struct_ids, anon_struct_ids_valid;

  /* local-write-by-name index; see comp_lvw_first */
  int *lvw_head;        /* [lvw_nbuckets] first write id in each name bucket */
  int *lvw_next;        /* [lvw_count] next write id sharing the bucket */
  int lvw_nbuckets, lvw_count;
  unsigned lvw_version; /* nt->version the index was built for */
  int lvw_built;

  /* local-write-by-(scope,name) index; see comp_lvw_first_sc */
  int *lvws_head;       /* [lvws_nbuckets] first write id in each bucket */
  int *lvws_next;       /* [lvws_count] next write id sharing the bucket */
  int lvws_nbuckets, lvws_count;
  unsigned lvws_version;
  int lvws_built;

  /* node-by-kind chains; see comp_kind_first */
  int *kind_head;       /* [NK_COUNT-ish buckets] first node id of each kind */
  int *kind_next;       /* [kind_count] next node id of the same kind */
  int kind_nkinds, kind_count;
  unsigned kind_version;
  int kind_built;

  /* CallNode-by-scope chain; see comp_scall_first */
  int *scall_head;      /* [scall_nscopes] first CallNode id in each scope */
  int *scall_next;      /* [scall_count] next CallNode id in the same scope */
  int scall_nscopes, scall_count;
  unsigned scall_version;
  int scall_built;

  char **symbols;   /* interned symbol names; index = sp_sym id */
  size_t *symbol_lens;  /* each name's BYTE length: a name may hold a NUL, and
                           strlen would end it there (#nul symbols) */
  int nsymbols, csymbols;

  ClassInfo *classes;
  int nclasses, cclasses;

  LocalVar *gvars;    /* global variables ($g), name without '$' */
  int ngvars, cgvars;
  LocalVar *consts;   /* top-level constants (FOO) */
  int nconsts, cconsts;

  /* alias $copy $orig → gvar_alias_from[i]="copy", gvar_alias_to[i]="orig" */
  char **gvar_alias_from;
  char **gvar_alias_to;
  int ngvar_aliases;

  int *toplevel_includes;  /* class indices of modules included at top level */
  int ntoplevel_includes;
  int has_include_math;    /* program has `include Math`: expose bare PI/E/fns */

  /* FFI registry: ffi_func declarations */
  FfiFunc *ffi_funcs;
  int n_ffi_funcs, c_ffi_funcs;

  /* FFI registry: ffi_const declarations */
  FfiConst *ffi_consts;
  int n_ffi_consts, c_ffi_consts;

  /* FFI registry: ffi_buffer declarations */
  FfiBuf *ffi_bufs;
  int n_ffi_bufs, c_ffi_bufs;

  /* FFI registry: ffi_read_* declarations */
  FfiReader *ffi_readers;
  int n_ffi_readers, c_ffi_readers;

  /* FFI registry: ffi_callback declarations (C function-pointer types) */
  FfiCallback *ffi_callbacks;
  int n_ffi_callbacks, c_ffi_callbacks;

  /* FFI registry: ffi_struct declarations (named C structs + field accessors) */
  FfiStruct *ffi_structs;
  int n_ffi_structs, c_ffi_structs;

  /* FFI registry: ffi_write_* declarations (symmetric to ffi_read_*) */
  FfiReader *ffi_writers;
  int n_ffi_writers, c_ffi_writers;

  /* FFI library names per module (semicolon-separated) */
  FfiLib *ffi_libs;
  int n_ffi_libs, c_ffi_libs;

  /* FFI cflags per module (semicolon-separated) */
  FfiCflag *ffi_cflags;
  int n_ffi_cflags, c_ffi_cflags;

  /* FFI inline C source fragments, emitted into the generated translation unit */
  FfiSource *ffi_sources;
  int n_ffi_sources, c_ffi_sources;

  /* native-binding registry: native_func declarations (Path B) */
  NativeFunc *native_funcs;
  int n_native_funcs, c_native_funcs;

  /* native-binding registry: native_obj (carried C objects to link on demand) */
  NativeObj *native_objs;
  int n_native_objs, c_native_objs;

  /* native-binding registry: native_method/native_new on native classes */
  NativeMethod *native_methods;
  int n_native_methods, c_native_methods;

  /* a native package declared `native_obj_reflect`: it consumes the generic
     "plain object -> hash of members" reflection, so codegen emits and installs
     sp_obj_to_hash when the program defines Structs. No feature is named in the
     compiler -- the package's require is the declaration. */
  int native_obj_reflect;
  /* the program calls Kernel#Integer / Kernel#Float somewhere: codegen emits
     and installs the conversion bridge (sp_obj_conv_sw) only then, so a
     program that never converts an object carries no extra dispatch. */
  int uses_kconv;
  /* the program has a `**` operand and defines #to_hash: the bridge carries
     the #to_hash row a boxed operand converts through (sp_kw_splat_conv). */
  int uses_kw_to_hash;
  /* body-node id -> enclosing BlockNode id (lazy; emit_stmts block-local
     resets). Sized nt->count; -1 = not a block body. */
  int *blk_body_map;
  /* node id -> the number a name invented from the node carries
     (comp_node_ord), bit 0 set for a builtin's. Extended over appended
     nodes, never refilled. A builtin node counts within its base, the
     builtin def (or top-level statement) it was spliced in: per base id, the
     count so far and the text it is named by. */
  int *node_ord, *node_base;
  int node_ord_n, node_ord_prog, node_ord_parsed;
  int *bi_base_cnt;
  char **bi_base_key;
  int bi_base_cap;
} Compiler;

Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);

/* Resize per-node arrays (ntype/nscope) after the node table grew. */
void comp_grow_node_arrays(Compiler *c);
int comp_node_ord(Compiler *c, int id, int *builtin);
const char *comp_node_tag(Compiler *c, int id);

/* If `id` is a ternary `cond ? A : B` -- an IfNode whose then- and else-clauses
   are each a single value expression (the shape the ternary emitter lowers to a
   C `?:`) -- set *then_node and *else_node to A and B and return 1; else return
   0. Shared by the nullable-int recognition in analyze and codegen. */
int comp_ternary_arms(const NodeTable *nt, int id, int *then_node, int *else_node);

/* Is `v` a chain of plain local/ivar writes ending in a literal nil
   (`a = b = nil` seen from a's value)? Returns the terminal NilNode id or -1.
   Shared by analyze (write-type collection) and codegen (chain lowering). */
int comp_nil_chain_bottom(const NodeTable *nt, int v);
int comp_scalar_literal_chain_bottom(const NodeTable *nt, int v);

/* Scopes. */
Scope *comp_scope_new(Compiler *c, const char *name, int def_node);
Scope *comp_scope_of(Compiler *c, int node_id);        /* owning scope */

/* Walk the local-variable writes that bind `name`, newest id first: plain
   writes, multiple-assignment/for/rescue/pattern targets, and ||= &&= op=
   writes, so a caller filters the kinds it wants. The
   alternative -- scanning the whole node table per query -- is what made
   resolving `k = Klass; k.new` quadratic on class-heavy programs. The index is
   keyed on name alone and revalidated against nt->version, so the scope of
   each write is still read fresh at every visit. */
int comp_is_local_write(NodeKind k);
int comp_lvw_first(Compiler *c, const char *name);
int comp_class_singleton_has_module(Compiler *c, int ci, int mod);
int comp_class_extends_any(Compiler *c, int ci);
int comp_lvw_next(const Compiler *c, int w);
int comp_lvw_first_sc(Compiler *c, int scope_idx, const char *name);
int comp_lvw_next_sc(const Compiler *c, int w);
int comp_scall_first(Compiler *c, int scope_idx);
int comp_scall_next(const Compiler *c, int u);
int comp_kind_first(Compiler *c, int kind);
int comp_kind_next(const Compiler *c, int id);
int comp_bare_gets_is_argf(Compiler *c);
int    comp_method_index(Compiler *c, const char *name); /* -1 if none */
/* A receiverless call's target: the enclosing self's ancestry first, a
   top-level def (a private Object method, and so last in every ancestry) only
   as the fallback. See analyze_util.c. */
int    comp_self_call_mi(Compiler *c, int call_node, const char *name);
int    comp_cbody_call_mi(Compiler *c, int call_node, const char *name);
/* 1 iff `node` is a constant path naming an `ffi_const` declaration, with its
   value in *out. Such a name is a VALUE, not a class, wherever the two are
   told apart. */
int    comp_ffi_const_at(Compiler *c, int node, int *out);
int    comp_included_method_index(Compiler *c, const char *name, int call_id);
int    class_is_blank_slate(Compiler *c, int ci);   /* explicit `< BasicObject` descent */

/* Locals within a scope. */
LocalVar *scope_local(Scope *s, const char *name);

/* The slot-widening idiom, recorded: unify `t` (the type of `node`'s value)
   into `lv`, answer whether it changed, and note the why when the slot
   degrades. slot_set is the same with the merged type already decided. */
int slot_take(Compiler *c, LocalVar *lv, TyKind t, int node);
int slot_set(Compiler *c, LocalVar *lv, TyKind merged, TyKind t, int node);
/* A rule sets the slot to `t` and says why in its own words, with `node`
   its subject (a default's expression, an argument) or -1. The why is
   recorded when the slot degrades by it and has no why yet; the words are
   a string literal, kept by pointer. */
void slot_rule(Compiler *c, LocalVar *lv, TyKind t, int node, const char *reason);
void why_reset(SlotWhy *w);
int ty_degraded(TyKind t);   /* poly, or a container of poly */
extern int g_infer_round;    /* the fixpoint round in progress, for SlotWhy.round */
LocalVar *scope_local_intern(Scope *s, const char *name);

/* Symbol intern table. comp_sym_intern returns the symbol's id. */
int comp_sym_intern(Compiler *c, const char *name);
int comp_sym_intern_n(Compiler *c, const char *name, size_t len);

/* Look up an ffi_callback type by (module, name); returns index or -1. */
int ffi_find_callback(Compiler *c, const char *mod, const char *name);

/* The IO::Buffer native class id, or -1 when the program does not load it. */
int ffi_iobuffer_class(Compiler *c);

/* Resolve Module.<method> against ffi_struct declarations: <Name>_new,
   <Name>_get_<field>, <Name>_set_<field>. Returns an FFI_SM_* op kind and,
   via out params, the struct and field indices (field -1 for _new). */
int ffi_struct_method(Compiler *c, const char *mod, const char *method, int *si, int *fi);

/* native-binding registry (Path B): find a native_func by (module, name),
   return its index in c->native_funcs or -1; map a spec to a TyKind. */
int comp_native_find(Compiler *c, const char *mod, const char *name);
int comp_native_method_find(Compiler *c, int class_id, const char *name, int argc, int kind);
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys);
int comp_poly_arm_defines(Compiler *c, int k, const char *name);
int comp_poly_arm_defines_n(Compiler *c, int k, const char *name, int argc);
TyKind native_spec_to_ty(const char *spec);
/* IO::Buffer type-symbol table (index-compatible with lib/sp_iobuffer.h) */
int comp_iob_sym_type(const char *name);
int comp_iob_ty_is_float(int t);
int comp_iob_ty_is_64(int t);

/* Global variables and top-level constants. *_intern finds or creates. */
LocalVar *comp_gvar(Compiler *c, const char *name);
LocalVar *comp_gvar_intern(Compiler *c, const char *name);
const char *comp_resolve_gvar(Compiler *c, const char *name); /* alias resolution */
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to);
/* 1 for $VERBOSE and $DEBUG, the interpreter's flags: false before any write */
int comp_gvar_is_interp_flag(const char *name);
LocalVar *comp_const(Compiler *c, const char *name);
LocalVar *comp_const_intern(Compiler *c, const char *name);
/* 1 when `pred` is a statically-false `defined?(Const)` if-guard (optionally
   the left arm of an `&&` chain) over a constant that resolves to nothing;
   the guarded branch is compile-time dead. */
int comp_defined_guard_false(Compiler *c, int pred);
/* the dual: statically-true defined?(Const / Const::Path) guard */
int comp_defined_guard_true(Compiler *c, int pred);

/* Classes. */
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node);
int        comp_class_index(Compiler *c, const char *name);   /* -1 if none */
int        comp_is_wellknown_const(const char *cn);
int        const_name_resolves_top_level(Compiler *c, const char *cn);
/* The class Ruby sees for `ci`: a synthesized singleton subclass reports its
   parent (CRuby hides the singleton class), every other class reports itself. */
static inline int singleton_visible_ci(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses) return ci;
  return c->classes[ci].is_singleton_of ? c->classes[ci].is_singleton_of - 1 : ci;
}
int        class_var_static_ci(Compiler *c, int node);  /* local holding one class const */
int        class_recv_static_ci(Compiler *c, int node); /* constant or local naming one class */
int        dynamic_new_may_reach(Compiler *c, int call_id, int cid);  /* k.new can build cid */
int        anon_struct_ci_for_value(Compiler *c, int val);  /* k = Struct.new(...) value node */
const char *struct_call_dup_member(Compiler *c, int callnode);  /* first duplicate member sym name, or NULL */
const char *sym_static_value(Compiler *c, int node);  /* SymbolNode or sole-symbol local */
/* The String in-place mutators, as one table with a per-site mask (see
   sp_str_mutator in analyze_util.c). The demand analysis and the codegen
   re-routes used to keep four near-identical copies of this list; a mutator
   added to one and missed in another is exactly how #3307 / #3333 arrived. */
#define SP_MUT_LOCAL     1u  /* seeds local-slot promotion: every mutator */
#define SP_MUT_CONTAINER 2u  /* container-read mutation: no `[]=` */
#define SP_MUT_IVAR      4u  /* ivar slot or a reader call (no rename) */
#define SP_MUT_NARROW    8u  /* guard-narrowed poly re-route: also no append_as_bytes */
/* 1 iff `nm` is a String in-place mutator serviceable at every site in `want`. */
int sp_str_mutator(const char *nm, unsigned want);
/* 1 iff call node `id` is a String method whose value is its receiver. */
int str_self_call(const NodeTable *nt, int id);
int fiber_storage_recv(const NodeTable *nt, int recv);
int array_mutator_name(const char *nm);
/* 1 iff `nm` is a stage that keeps a lazy chain lazy -- the set
   emit_lazy_pipeline_expr can fuse, plus a re-lazy. The recognizer, the
   write-suppression walk and the pipeline walker must agree on it: #3318,
   #3323 and #3324 were each one list updated and another missed. */
int lazy_stage_name(const char *nm);
/* Zero-argument builtin methods the poly dispatch serves with a real arm --
   the `require "ostruct"` member-read catch-all must not swallow them. */
int poly_builtin_zero_arg_name(const char *m);
int chain_is_lazy_valued(Compiler *c, int node);      /* CallNode chain evaluating to a Lazy */
int lazy_alias_chain(Compiler *c, int var_read);
int lazy_method_chain(Compiler *c, int call);      /* parameterless method whose body is a lazy chain -> chain node, else -1 */
int lazy_resolve_chain(Compiler *c, int n);
int local_is_handle(Compiler *c, int a);
int        hash_new_default_arg(Compiler *c, int recv); /* Hash.new(d) literal: d node or -1 */
int        recv_hash_new_default_arg(Compiler *c, int recv); /* the same through a local or ivar READ node */
TyKind     hash_default_value_ty(Compiler *c, int dn);      /* the value type a Hash.new(d) default contributes */
int        hash_new_blockless(Compiler *c, int recv);  /* blockless Hash.new / {} literal */
int        const_owned_by_class(Compiler *c, const char *clsname, const char *constname);
/* Class index of a `class_eval`/`module_eval { defs }` reopen, else -1.
   enclosing_class resolves bare/`self.` receivers (the class whose body we are
   directly in); ignored for constant receivers. */
int        class_eval_reopen_class(Compiler *c, int id, int enclosing_class);
int        class_reopen_cmethod(Compiler *c, int recv, const char *name); /* a method the program adds to Class */
int        comp_ivar_index(ClassInfo *ci, const char *name);  /* -1 if none */
int        comp_ivar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
int        comp_member_intern(ClassInfo *ci, const char *name);
int        comp_member_index(ClassInfo *ci, const char *name);
int        comp_cvar_index(ClassInfo *ci, const char *name);  /* class var; -1 if none */
int        comp_cvar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
int        comp_cvar_owner(const Compiler *c, int cid, const char *name); /* the class whose slot holds @@name */
/* 1 iff method m's param idx is a byref string out-param (LocalVar.byref_out):
   passed as const char** so callee mutation lands in the caller's variable. */
int        comp_byref_param(Compiler *c, Scope *m, int idx);
/* Find the instance-method scope index for class_id + method name, or -1. */
int        comp_method_in_class(Compiler *c, int class_id, const char *name);
/* The instance_exec emission runs a method's block as an instance method of
   the receiver's class by moving the method's scope there (class_id and
   is_cmethod) for the length of the block. begin records the scope's own
   class before a move and end drops the latest record; the method lookups
   and scope_is_shadowed's table key a moved scope by its own class (see
   comp_scope_own_class in compiler.c). */
void       comp_scope_move_begin(Compiler *c, int s);
void       comp_scope_move_end(void);
/* the number of moves in effect, and putting back every move made since a
   depth: for the recoveries a refusal longjmps to */
int        comp_scope_move_depth(void);
void       comp_scope_move_unwind(int depth);
int        comp_scope_own_class(const Compiler *c, int s, int *is_cmethod);
/* Freeze/unfreeze the (class_id,name,is_cmethod)->scope lookup index. Frozen
   only while scope shape is fixed (the inference fixpoint); see compiler.c. */
void       comp_scope_index_set_frozen(int frozen);
/* Whether the scope-index is currently frozen (scope shape fixed). */
int        comp_scope_index_is_frozen(void);
/* Generation counter, bumped on every freeze/unfreeze transition. */
unsigned   comp_scope_index_gen(void);
/* Find the class (singleton) method scope for class_id + name, or -1 (no chain). */
int        comp_cmethod_in_class(Compiler *c, int class_id, const char *name);
/* Find the class (singleton) method scope, walking the superclass chain. */
int        comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* The IO family: File, IO and the socket classes share the IO handle type.
   io_family_class: class k is a top-level reopening of one of them. */
int        io_family_name(const char *n);
int        io_family_class(Compiler *c, int k);
/* The reopened IO-family class whose method `name` a typed IO calls, or -1. */
int        io_reopen_class(Compiler *c, const char *name);
extern int g_io_skip_reopen, g_io_skip_node;
int        io_reopen_leaves_builtin(Compiler *c, const char *name);
int        io_reopen_defs(Compiler *c, const char *name, int public_only, int *ks, int max);
int        io_reopen_ret_mixed(Compiler *c, const char *name);
int        io_family_descends(Compiler *c, int k, int owner);
/* Like comp_method_in_class but walks the superclass chain. On success,
   *def_class (if non-NULL) is set to the class that defines the method. */
int        comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        comp_builtin_kind_reopen_mi(Compiler *c, TyKind t, const char *name);
/* Record method `name`'s visibility on a class (overwrite-or-append). */
void       comp_method_vis_set(ClassInfo *ci, const char *name, int kind);
/* Record class method `name`'s visibility on a class (overwrite-or-append). */
void       comp_cmethod_vis_set(ClassInfo *ci, const char *name, int kind);
/* Visibility of class method `name` up class_id's superclass chain; the
   declaring class goes to *at. SP_VIS_PUBLIC when none records it. */
int        comp_cmethod_vis_declared(Compiler *c, int class_id, const char *name, int *at);
/* Visibility of `name` declared directly on this class (SP_VIS_PUBLIC if none). */
int        comp_method_vis(ClassInfo *ci, const char *name);
/* Visibility of `name` as resolved up class_id's ancestor chain: the first
   class with an explicit entry wins (a subclass may re-`public` an inherited
   private method), defaulting to SP_VIS_PUBLIC when none records it. */
int        comp_method_vis_in_chain(Compiler *c, int class_id, const char *name);
int        comp_method_vis_declared(Compiler *c, int class_id, const char *name, int *at);
/* Detect an instance_eval/exec trampoline (def m(args,&b); instance_eval/exec(args,&b); end).
   Returns 1 (eval) / 2 (exec) / 0; sets *def_class to the defining class. */
int        comp_trampoline_kind(Compiler *c, int class_id, const char *name, int *def_class);
/* Stage-1 fold for module singleton accessors holding a constant. */
int        comp_sg_const_binding(Compiler *c, int class_id, const char *base);
int        comp_sg_reader_const(Compiler *c, int call_id); /* const class idx for `Class.reader`, or -1 */
int        comp_sg_const_candidates(Compiler *c, int class_id, const char *base, int *out, int max);
int        comp_sg_reader_candidates(Compiler *c, int call_id, int *out, int max); /* Stage-2 distinct consts */
int        comp_is_nested_int_array_literal(Compiler *c, int node); /* `[[ints],...]` literal */
/* One class that may answer a poly-dispatched `name`: a user class with the
   method (`mi`) or a reader (`rdcls`, the class holding the attr), or a native
   class (`native`, arity checked by the consumer). See comp_poly_candidates. */
typedef struct { int cls; int mi; int rdcls; int native; } PolyCand;
const PolyCand *comp_poly_candidates(Compiler *c, const char *name, int *n);
extern unsigned comp_table_gen;
void comp_poly_candidates_reset(void);
/* The classes answering class method `name` through their chain, ascending,
   each with the scope comp_cmethod_in_chain gives (`cls`, `mi`; the other
   fields unused). Memoized under the same stamps as comp_poly_candidates. */
const PolyCand *comp_cmethod_candidates(Compiler *c, const char *name, int *n);
/* Every proper descendant of class `cid`, ascending; fixed once classes are
   collected. See comp_descendants. */
const int *comp_descendants(Compiler *c, int cid, int *n);
void comp_descendants_reset(void);
/* Walk the chain for an attr reader/writer; returns 1 and the owning class. */
int        comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
void       comp_add_reader(ClassInfo *ci, const char *name);
void       comp_add_writer(ClassInfo *ci, const char *name);
int        comp_is_reader(ClassInfo *ci, const char *name);
int        comp_is_writer(ClassInfo *ci, const char *name);
int        name_is_plain_setter(const char *name);
int        call_is_setter_assign(const NodeTable *nt, int id);
int        proc_parameters_lambda_mode(const NodeTable *nt, int argc, const int *argv);
int        self_is_main(Compiler *c, int node);
int        self_class_body(Compiler *c, int node);
int        attr_decl_call(Compiler *c, int id);
enum { VIS_DECL_NIL = 1, VIS_DECL_SYM, VIS_DECL_ARRAY, VIS_DECL_SELF };
int        vis_decl_call(Compiler *c, int id);
int        setter_base_name(const char *name, char *out, size_t cap);
void       comp_add_undef(ClassInfo *ci, const char *name);
int        comp_is_undeffed_in_chain(Compiler *c, int class_id, const char *name);
void       comp_add_sg_reader(ClassInfo *ci, const char *name);
void       comp_add_sg_writer(ClassInfo *ci, const char *name);
int        comp_is_sg_reader(ClassInfo *ci, const char *name);
int        comp_is_sg_writer(ClassInfo *ci, const char *name);
void       comp_add_alias(ClassInfo *ci, const char *new_name, const char *old_name);
void       comp_add_alias_from(ClassInfo *ci, const char *new_name, const char *old_name, int from_cls);
void       comp_add_sg_civ(ClassInfo *ci, const char *name);
int        comp_is_sg_civ(ClassInfo *ci, const char *name);
int        comp_is_sg_inh(ClassInfo *ci, const char *name);
void       comp_add_sg_inh(ClassInfo *ci, const char *name);
/* Prepend-chain helpers. */
void        comp_prep_chain_add(ClassInfo *ci, const char *from, const char *to);
const char *comp_prep_chain_target(Compiler *c, int class_id, const char *name);
/* The shadow a `super` in scope s reaches through the prep chain, or NULL. A
   class method's chain is keyed `self.<name>`, apart from the instance
   method of the same name. */
const char *comp_super_shadow(Compiler *c, const Scope *s);
void comp_cprep_chain_add(ClassInfo *ci, const char *from, const char *to);
const char *comp_prep_user_name(const char *name);
const char *comp_super_name(Compiler *c, int parent, const char *name, int is_cmethod);
int comp_super_is_class_new(Compiler *c, int id);
int comp_super_parent(Compiler *c, int class_id, int is_cmethod); /* super in `self.new` is Class#new */
/* Resolve `name` through the class's (chain-aware) alias table to the
   underlying method/attr name. Returns `name` unchanged if not aliased. */
/* What a name means on a class: nothing, an attribute (attr_reader/writer,
   Struct member, ...), or an explicit method. Both tables can own one name;
   comp_resolve_member arbitrates once for every emission site. */
enum { SP_MEMBER_NONE = 0, SP_MEMBER_ATTR, SP_MEMBER_METHOD };
/* `name` is the bare name for a read and the BASE name for a write (the method
   consulted is then `name=`). Reports the defining class and, for a method, its
   scope index. */
int         comp_resolve_member(Compiler *c, int class_id, const char *name, int want_write,
                                int *def_class, int *method_index);
const char *comp_resolve_alias(Compiler *c, int class_id, const char *name);
const char *comp_resolve_alias_at(Compiler *c, int class_id, const char *name, int *start_cls);
const char *comp_resolve_alias_ex(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin);

/* Set by codegen while a block is spliced: answers the type the block being
   inlined RIGHT HERE gives a yield, which the node cache cannot hold (one
   YieldNode, one entry, many call sites -- #3784). Returns 0 when the node is
   not a yield in a spliced context. */
extern int (*sp_yield_site_type_hook)(const Compiler *c, int id, TyKind *out);

/* Swap a node's cached type for the duration of one re-entered emission and
   answer the old one, so the caller can restore it. `x&.pred?` is inferred
   poly (its nil arm needs somewhere to live) while the value arm still
   renders a C bool: the safe-navigation emitter puts the natural type back
   while it emits that arm, because dispatches downstream read the cache to
   type their own temps (#4070). */
static inline TyKind comp_sn_retype(Compiler *c, int id, TyKind t) {
  if (id < 0 || id >= c->nt->count) return t;
  TyKind old = c->ntype[id];
  c->ntype[id] = t;
  return old;
}

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  if (sp_yield_site_type_hook) {
    TyKind yt;
    if (sp_yield_site_type_hook(c, id, &yt)) return yt;
  }
  /* TY_STRBUF is a codegen-only storage refinement (mutable sp_String for a
     `<<`-appended local). All type-directed logic treats it as a string;
     codegen consults the raw scope-local type where the distinction matters.
     Exception: a read marked strbuf_box yields the live HANDLE, so the
     mutation is observable through the container it is stored in (#3227). */
  TyKind t = c->ntype[id];
  if (t == TY_STRBUF) return c->strbuf_box[id] ? TY_STRBUF : TY_STRING;
  /* A node under a handle demand STORES as the handle -- a temp spilled from
     it has to be an sp_String *, not a const char * -- while still dispatching
     as a String, which comp_recv_type answers for. That split is the whole
     point of the second array (#4363). */
  if (c->strbuf_handle_demand[id]) return TY_STRBUF;
  return t;
}

/* 1 iff t is a user-object type whose class is represented by value (sp_X,
   not a heap pointer). See detect_value_types / reference_legacy_value_type_logic. */
static inline int comp_ty_value_obj(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type;
}

/* The sp_poly_enum_proc op for a block-carrying Enumerable name, or NULL.
   One list, two readers: codegen emits the poly dispatch's builtin arm for
   these names, and analyze keeps a block passed to such a call poly-typed
   because that arm can take it (#3409). */
/* 1 for a call that hands out one ELEMENT of a container (see compiler.c). */
int container_elem_read_p(const NodeTable *nt, int id);
const char *poly_enum_op_for(const char *name);
int poly_container_read_p(const char *name);
/* 1 for a numeric read a builtin receiver answers differently (see compiler.c). */
int poly_numeric_read_p(const char *name);
int poly_string_read_p(const char *name);

/* 1 for a Class-valued receiver whose class is only known at run time (a
   variable, or a call returning a class); 0 for a constant or accessor
   receiver, which resolve statically (#3415). */
int class_recv_is_dynamic(Compiler *c, int recv);
int self_class_static_ci(Compiler *c, int recv);  /* self.class naming one class */
int allocate_on_own_class(Compiler *c, int id);    /* allocate on the method's own class */

/* An ivar whose type came from an --rbs seed (class_pin_ivar). Codegen reads
   it to decide where a seed assertion belongs (#3412). */
int class_ivar_pinned(ClassInfo *ci, const char *name);

#endif
