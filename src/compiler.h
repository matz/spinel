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
#include "builtin_names.h"
#include "builtin_ops.h"

/* strict-require (defined in spinel_parse.c). sp_feature_enabled(name) is 1 when
   feature `name` may be provided: always when the gate is off (g_strict_require
   == 0), else only if `require "name"` appeared in the program. Used to gate
   require-only stdlib (stringio, io/console, ...) so they match CRuby's
   uninitialized-constant / NoMethodError when the require is absent. */
extern int g_strict_require;
/* the program may load a file the compiler did not read (spinel_parse.c,
   above resolve_requires): what that file would define is not in the node table */
extern int g_require_unread;
extern const char **g_required_features;   /* spinel_parse.c */
extern int g_nrequired_features;
/* SPINEL_SHARE_STRINGS is on: set, not empty and not "0" (spinel_parse.c) */
int sp_share_strings_env(void);
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
/* The tables infer_type reads are written only through setters that count
   each change in comp_ty_edits, so that its memo can tell whether an
   earlier query's answer still holds (#8852). Their fields are const under
   SP_TY_GUARD (`make ty-setter-check`), so a direct write does not compile
   there. */
#ifdef SP_TY_GUARD
#define SP_TY_FIELD const
#else
#define SP_TY_FIELD
#endif
extern unsigned long comp_ty_edits;
/* Each slot has an owner: the scope of a local, of a node or of a method's
   value, the class of an ivar or a class variable, or the program itself
   for a global, a constant, a slot picked through a pointer and a new
   class. An owner's version is the edit count of the last write to one of
   its slots (comp_touch_*). infer_type's memo keeps, per answer, the owners
   the inference read (imemo_dep_*, noted by the lookups that reach a slot:
   a scope by its node, a local by name, a method by its chain, an ivar by
   its index, and the program for the rest), and an answer stands while
   none of them has moved since it was derived (analyze_infer.c). */
extern unsigned long *comp_scope_ver, *comp_class_ver, comp_global_ver;
extern int comp_scope_ver_cap, comp_class_ver_cap;
/* (infer_write_types, while it re-derives the locals it reset) per scope, the
   writes that moved it, and those that were a local's type or proc return:
   NULL outside it */
extern int *comp_touch_count, *comp_lv_touch_count;
/* (diagnostics, SPINEL_IMEMO_WHY) the owner each edit moved, by edit count */
#define COMP_TOUCH_LOG_CAP (1 << 22)
extern int *comp_touch_log;
static inline void comp_touch_note(int key) { if (comp_touch_log) comp_touch_log[comp_ty_edits & (COMP_TOUCH_LOG_CAP - 1)] = key; }
static inline void comp_touch_global_r(int why) { (void)why; comp_global_ver = ++comp_ty_edits; comp_touch_note(-1); }
static inline void comp_touch_global(void) { comp_touch_global_r(0); }
static inline void comp_touch_scope(int idx) {
  if (idx >= 0 && idx < comp_scope_ver_cap) { comp_scope_ver[idx] = ++comp_ty_edits; comp_touch_note(idx); if (comp_touch_count) comp_touch_count[idx]++; }
  else comp_touch_global();
}
static inline void comp_touch_class(int idx) {
  if (idx >= 0 && idx < comp_class_ver_cap) { comp_class_ver[idx] = ++comp_ty_edits; comp_touch_note(-(idx + 2)); }
  else comp_touch_global();
}
extern int imemo_dep_on;   /* an inference that records what it reads is open */
void imemo_dep_scope(int idx);
void imemo_dep_class(int idx);
void imemo_dep_global(void);
#define IMEMO_DEP_SCOPE(i) do { if (imemo_dep_on) imemo_dep_scope(i); } while (0)
#define IMEMO_DEP_CLASS(i) do { if (imemo_dep_on) imemo_dep_class(i); } while (0)
#define IMEMO_DEP_GLOBAL() do { if (imemo_dep_on) imemo_dep_global(); } while (0)
/* One of those fields, for code that picks the slot first and writes it
   after: written through ty_ref_set (below, after Compiler). */
typedef SP_TY_FIELD TyKind *TyRef;
/* an owner by key: a scope's index, a class as -(index + 2), the program as -1 */
static inline void comp_touch_key(int key) {
  if (key >= 0) comp_touch_scope(key);
  else if (key == -1) comp_touch_global_r(3);
  else comp_touch_class(-(key + 2));
}

/* The user classes a value can hold (analyze_cls.c, #8530): `all` for every
   class, else a set of class indices with no bound on how many: a sparse
   bitset, its nw nonzero words w at word indices wi (ascending), n members.
   A member at or past nclasses is the Class object of (member - nclasses),
   which a call dispatches to class methods. `bv`: the value may also be a
   builtin (a container, a Proc), whose own methods then answer too; `os`
   an OpenStruct; `pr` (with bv) a Proc, whose `[]` is its call. Read with cls_set_has and cls_set_next. */
typedef struct { unsigned char all, bv, os, pr; int n, nw; unsigned long long *w; int *wi; } ClsSet;
/* the position of the first word at word index q or past it */
static inline int cls_set_word_from(const ClsSet *s, int q) {
  int lo = 0, hi = s->nw;
  while (lo < hi) { int m = (lo + hi) >> 1; if (s->wi[m] < q) lo = m + 1; else hi = m; }
  return lo;
}
static inline int cls_set_has(const ClsSet *s, int k) {
  if (k < 0) return 0;
  int p = cls_set_word_from(s, k >> 6);
  return p < s->nw && s->wi[p] == (k >> 6) && ((s->w[p] >> (k & 63)) & 1);
}
/* the member after k (k = -1 for the first), -1 past the last */
static inline int cls_set_next(const ClsSet *s, int k) {
  int i = k + 1, q = i >> 6;
  for (int p = cls_set_word_from(s, q); p < s->nw; p++) {
    unsigned long long m = s->w[p];
    if (s->wi[p] == q) m &= ~0ULL << (i & 63);
    if (m) return s->wi[p] * 64 + __builtin_ctzll(m);
  }
  return -1;
}
void cls_pool_free(ClsSet *pool, int n);   /* a pool of sets and their words */

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

/* The bits of nullable_int_elem / ivar_nullable_int_elem. NIE_MARKED types
   the elements read out (an oint where it is set). NIE_REACH: a nil can be in
   the array at run time through some other name of it -- a store through a
   parameter it was passed as, a local assigned from it, what a method
   answered, or a gap a write past the end left -- which the mark of this
   name does not see; the loop read hc_index_in_range proves in range asks
   it (repr elem_nil_reach) before it reads the element untested. NIE_STORED:
   such a nil can be stored THROUGH this name, so the names it was assigned
   or bound from reach it too (mark_elem_slot). */
enum { NIE_MARKED = 1, NIE_STORED = 2, NIE_REACH = 4 };
typedef struct {
  char *name;       /* Ruby local name (without sigil) */
  SP_TY_FIELD TyKind type; /* inferred type, written by lv_set_type */
  SlotWhy why;      /* how `type` came to be untyped, if it is */
  int last_src;     /* the node whose value last set `type` while it was concrete; -1 = none */
  int gc_root;      /* scratch during analysis; rooting is type-derived in codegen */
  int is_param;     /* declared as a method parameter (C function param) */
  int is_block_param; /* bound by a block; typed by block-param inference */
  SP_TY_FIELD int proc_ret;    /* when type==TY_PROC: the proc's body return type (TyKind),
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
  int borrowed_volatile; /* codegen: this String slot, or a slot borrowed here,
                           is live across setjmp; propagate through lending */
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
  int rbs_box_held; /* (params) a box reached the container seed while the
                       fixpoint ran and the seed held: it may have been a
                       transient, so the settled types decide whether the
                       seed gives way (param_seed_holds, an_settle_seed_boxes) */
  int nullable_int; /* an int local that was assigned a value which can be
                       nil (a search miss, a pop off an empty array): its
                       slot holds its nil beside the value, and boxing it
                       has to yield nil, not the number */
  int nil_passed;   /* (proc params) some call passes a literal nil: an
                       Integer another call passes keeps the param an
                       Integer, which the nil then no longer overrides as it
                       does the bare-int guess (cs_type_params) */
  int site_boxed;   /* (block params) 1 when what its sites bind boxed it,
                       2 when it was boxed before they did (see
                       block_settle_types, narrow_site_boxed_block_params) */
  int obj_nilable;  /* an object-typed parameter some call site passes nil
                       (#5088): a seed of the nil fact (analyze_nil.c) */
  int obj_may_nil;  /* (object-typed slots: locals, parameters, block
                       parameters, globals, constants) the nil fact
                       (analyze_nil.c, #7444): a read of the slot may answer
                       nil -- a write stores a value that may be nil, a read
                       can run before any write, a call site binds nil. Read
                       through repr_of_slot's may_nil. Nonzero: where the
                       nil comes from (NFW_*, analyze.h). */
  int obj_elem_may_nil; /* (Array slots whose elements are pointers: an
                       object's, a String's, an Array's) the nil fact one
                       level in (analyze_nil.c): an element read out of it,
                       or a block parameter an iteration over it binds, may
                       be nil -- a write stored one, or left a gap a write
                       past the end fills with nil. Nonzero when so; its
                       NF_EL_STORED bit says a nil was stored through this
                       name, which reaches the slots the Array came from.
                       Read through repr_of_slot's elem_nil_marked. */
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
                       some element of which can be nil (its nil bit), so
                       reading an element or binding a block parameter from
                       it carries the nil out (#3505): NIE_MARKED. The other
                       NIE_ bits say where a nil can come from at run time. */
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
  const char *widen_from_name; /* (params) set when the reverse binding widened this
                       parameter on behalf of a callee's push-widened parameter
                       (widen_from_scope, widen_from_name): the widening is
                       retracted when that parameter stops being a general Array */
  int widen_from_scope;
  unsigned char widen_blocked; /* (params) retracted once: the reverse binding does not widen it again */
  unsigned char range_held; /* a value that may be a Range was written into it
                       (slot_set, value_may_be_range): a boxed index read
                       from it may be a Range at run time */
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
  TyKind push_fill_elem; /* (params, untyped or array) the element type a
                       push through this parameter stores, unified as
                       boxed_push_elem is. The binding gives a caller's empty
                       `[]`, which the push fills, that element
                       (fill_empty_array_arg). */
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
  unsigned long long store_elems_src; /* (params, arrays) bit j set: a splice
                       through this parameter stores the ELEMENTS of the
                       method's own positional parameter j (`a[i, n] = src`),
                       boxed when its callers disagree: the binding checks
                       each call's argument j's elements the same way. */
  unsigned long long store_merge_src; /* (params, hashes) bit j set: a merge!,
                       update or replace into this parameter takes the method's
                       own parameter j, positional or keyword, boxed when its callers
                       disagree: the binding checks each call's hash for it,
                       its keys and values, the same way. */
  int store_merge_blocks[4], store_merge_nblocks; /* (params, hashes) the merge!
                       or update calls through this parameter with a conflict
                       block and a boxed source: the binding checks each
                       block's value against each call's hash as well. A fifth
                       is left to the store. */
  int store_rest_src; /* (params, containers) one past the first element of the
                       method's rest parameter that a push, unshift or insert
                       through this parameter stores, or 0: the rest
                       collects them boxed, so the binding checks each call's
                       own rest arguments against the container. */
  int const_def_write; /* (consts) has a definite (non-or/and) assignment; an
                          or/and-write-only const is nil-defaulted (poly) so its
                          `||=` truthiness check fires on first use */
  int const_early; /* (consts) some read can run before an assignment of it: the
                      slot has a presence flag (cst_NAME__set) that every
                      assignment sets and every read, defined?, const_defined?
                      and const_get tests (analyze_const.c) */
  int gvar_seeded; /* (globals) gvar_seeded_before_read's answer, asked once:
                      0 not asked yet, 1 a read can see its first nil, 2 a
                      non-nil value is written before anything can read it */
  int or_written; /* some write to this local is a `||=`, which can run before
                     any definite assignment (`v ||= 5; v += 2`, a definite
                     write in one branch only, a block local reset each
                     iteration), so its truthiness check must be able to see
                     nil. The slot starts as its type's nil (an Integer or
                     Float one an oint with its nil flag set) rather than the
                     zero value, so a kind whose zero is a real value (sp_int
                     0, 0.0) can still tell the two apart (mirrors
                     ConstantVar's const_def_write). */
  int nil_only;     /* only ever saw nil: the late widening made it a boxed
                       nil (TY_POLY), so an Integer or Float parameter it is
                       handed to has to hold nil (nullable_int_value) */
  int maybe_unset;  /* (Integer / Float) some read can run before any write:
                       every write to it ahead of the read is conditional (a
                       modifier `if`, one branch, a loop body), so the read
                       answers nil, and the slot starts nil as an
                       or-written one does */
  int elems_shared; /* --share-strings: a String container whose elements
                       the rule shares settled in its poly form, and its
                       elements are boxed handles (repr_of_slot's
                       elems_handle) */
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
  TyKind shim_ty;   /* while a shared-handle shim emits its arm, the type this
                       local had before the shim typed it String for the arm
                       (the handle's); TY_UNKNOWN otherwise. A proc or fiber
                       made inside the arm sees the handle (sb_shim_lift). */
  int shim_lift;    /* how many sb_shim_lift calls hold the local at shim_ty */
  unsigned char plain_int; /* (TY_INT) int_value_plain's memo: 0 not asked, 1 being
                       asked, 2 never holds nil, 3 may */
  signed char hcp_int; /* (--int-overflow=promote, codegen) hcp_compute's fact: 1 the
                       boxed slot only ever holds an Integer or nil, -1 not proven */
  TyKind body_write; /* (parameter) the join of what its own body assigns it, as
                        infer_write_types last folded the writes; TY_UNKNOWN when
                        nothing does */
  int owner;        /* the scope that holds the slot (Scope.idx), -1 for a global
                       or a constant: the owner its writes move (comp_touch_scope) */
} LocalVar;

static inline TyKind lv_set_type(LocalVar *lv, TyKind t) {
  if (lv->type != t) {
    *(TyKind *)&lv->type = t;
    if (comp_lv_touch_count && lv->owner >= 0 && lv->owner < comp_scope_ver_cap) comp_lv_touch_count[lv->owner]++;
    comp_touch_scope(lv->owner);
  }
  return t;
}
static inline int lv_set_proc_ret(LocalVar *lv, int t) {
  if (lv->proc_ret != t) {
    *(int *)&lv->proc_ret = t;
    if (comp_lv_touch_count && lv->owner >= 0 && lv->owner < comp_scope_ver_cap) comp_lv_touch_count[lv->owner]++;
    comp_touch_scope(lv->owner);
  }
  return t;
}
#define POLY_LIFT_APPENDED 1
#define POLY_LIFT_ZSUPER   2
/* Compiler.poly_strbuf_lift's mark for a boxed call's new String a
   container store lifts (any other mark is 1) */
#define STRBUF_LIFT_FRESH_CALL 2

enum { RBS_CONT_HASH = 1, RBS_CONT_ARRAY = 2 };   /* Scope.ret_rbs_cont: the class a container return seed checks */
enum { SHADOW_NONE = 0, SHADOW_INC, SHADOW_PREP };   /* Scope.shadow: the kind of hidden copy */
typedef struct {
  char *name;       /* method name; NULL for the top-level scope */
  int idx;          /* its index in Compiler.scopes: the owner its slots' writes move */
  char *c_name;     /* optional unique C symbol for a method without changing its Ruby name */
  int def_node;     /* DefNode id; -1 for top-level */
  int body;         /* StatementsNode id (-1 if empty) */
  int class_id;     /* owning class index, or -1 for free functions */
  int yields;       /* body contains a YieldNode (inlined at call sites) */
  int reachable;    /* method name is referenced somewhere (else dead code) */
  /* a method's versions (#9898), from the load record once it is made:
     versioned, the record holds more than one version of the name (in any
     class), so call_target binds a call of it to the version it runs;
     replaced, a later version replaces this one in its class's history, so
     only the calls bound to it reach it; version_n, codegen's number for a
     replaced version in its C name (mc_method), 0 for none; entry, the
     method a call whose version is known only at run time reaches
     (call_plan.c's version entries): it calls the version the run-time
     cells name, one per parameter count the calls need (version_n
     numbers the later ones in C). It has the method's name, as a replaced
     version does; ENTRY_DISPATCH marks this argument-specific forwarding
     entry. No lookup of the method by name answers it (sm_owns),
     only the calls call_target binds to it reach it. entry == ENTRY_TABLE is a
     general prepend guard: the table answers it under the ordinary name,
     and its guard_behind names the hidden copy; ENTRY_NAMED the entry of a
     history the load phase leaves open (elab_history_open), which is the
     method by name: the table answers it, and the versions it arms are
     replaced; main_n, codegen's
     number for a method of main's own whose name Object's methods have
     too (both are top-level functions in C) */
  unsigned char versioned, replaced;
  enum { ENTRY_NONE, ENTRY_DISPATCH, ENTRY_TABLE, ENTRY_NAMED } entry;
  int version_n, main_n;
  unsigned char cand_bound; /* a call on a poly receiver bound its arguments
                               here as one of the methods it may reach
                               (analyze_pass.c): what its parameters hold is
                               what that call hands, should it reach this one */
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
  int is_prepend_copy;        /* the instance method a `prepend` copied in, in
                                 front of the class's own (its super reaches
                                 that one -- the builtin's, for a builtin) */
  int mixin_guarded;          /* such a copy of a prepend a later top-level
                                 reopening makes: the number of the cell that
                                 statement sets (n_prepend_cells), 0 for
                                 none. A call reaches it through its guard
                                 entry (call_plan.c), which runs the method
                                 behind it, guard_behind, until the cell is
                                 set */
  int guard_behind;
  /* a hidden copy: a body the compiler moved behind another method of its
     name. A SHADOW_PREP copy is named apart from every method the program
     names (comp_shadow_name); a SHADOW_INC copy's name is not checked against
     them. SHADOW_PREP: a prepend's `__prep <n> m` (the class's own body
     behind the module's copy, or a later prepend's guarded copy); SHADOW_INC:
     an include's, extend's or alias's `__inc <n> m`. shadow_user is m, the
     name the program wrote (a proc-form clone's with its "#pf"). SHADOW_NONE
     and NULL for every method the program names, however it is spelled. */
  unsigned char shadow;
  char *shadow_user;
  int is_extend_copy;         /* the class method an `extend` copied in: a later
                                 extend of a module defining the same name
                                 replaces it the same way */
  int is_proc_form;  /* a clone of a yielding method whose `yield` is a call on
                        a real &blk parameter, for the poly dispatch. Its body
                        is typed independently of the inlined original: the
                        yield answers poly, so everything it feeds widens with
                        it and one body serves every call site (#3399). */
  int is_lowered_yield; /* self-recursive yield method lowered to &block (sp_Proc) form */
  int ctor_cycle;       /* a yielding initialize on a cycle of `new` sites that
                           splice it, or a yielding method on such a cycle
                           (mark_ctor_cycles): its strongly connected
                           component, numbered from 1; 0 on none */
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
  /* What the def's parameter list says, read off its ParametersNode by
     scope_param_shape (codegen_fold.c) and kept while the node table has
     no edit: a call's layout asks it of every candidate the call may reach. */
  const void *ps_nt;      /* the table it was read from; NULL until read */
  unsigned long ps_edits; /* ... at its edit count */
  int ps_def;             /* ... for this def_node */
  int ps_pn;              /* the parameters node, or -1 */
  int ps_nkw;             /* the declared keywords, ps_kws */
  const int *ps_kws;
  int ps_fwd;             /* a `...` keyword rest: every name binds by keyword */
  int ps_obr;             /* an optional with posts beside it; -1: no list says */

  SP_TY_FIELD TyKind ret;     /* inferred return type */
  SlotWhy ret_why;  /* how `ret` came to be untyped, if it is */
  int ret_poly_ctr; /* the return value can be a builtin Array/Hash even
                       though `ret` collapsed to poly (see LocalVar.poly_ctr) */
  int ret_specialized; /* ret was set by specialization (inherited-cls-new copy);
                          don't overwrite it from the shared body in the fixpoint */
  SP_TY_FIELD TyKind ret_noblock;  /* a yielding method's value when called WITHOUT a block:
                          the type of its `return x unless block_given?`
                          returns, kept out of `ret` so a call with a block
                          (which the inliner specializes, folding the guard
                          away) reads the value of the body proper. UNKNOWN
                          when the method has no such guard. */
  int ret_pin_guard;   /* a method whose `ret` was pinned to the Enumerator its
                          `return enum_for(:m) unless block_given?` answers: the
                          node of that call, whose value a call WITH a block
                          does not answer (method_call_ret reads the other
                          values instead). -1 otherwise. */
  int is_ext_entry;    /* designated --ext-entry: emitted non-static, a DCE
                          root, declared in the emitted extension header (#M1
                          of docs/internals/ext-design.md) */
  int ret_rbs_seeded;  /* ret pinned from an --rbs advisory seed: the fixpoint
                          must not recompute it from the body */
  int ret_rbs_box_held; /* a box reached that container seed while the
                          fixpoint ran and the seed held: the settled types
                          decide (ret_seed_holds, an_settle_seed_boxes) */
  int ret_elems_shared; /* --share-strings: that seed was Array[String], and the
                          Strings the method answers are shared, so the return
                          settled in its poly form, the boxes holding the
                          handles (share_default_apply), as a local's does
                          (LocalVar.elems_shared); the same Ruby type in its
                          shared C form, not a contradiction of the seed (#9626) */
  int ret_pub_fresh;   /* --share-strings: a deep-return pickup takes this
                          method's value, and a tail of it answers a fresh
                          String (share_node_fresh) beside the handle reads
                          the others publish: that tail clears the side
                          channel (emit_tail_value), so the caller wraps it */
  unsigned char ret_rbs_cont; /* a container return seed (RBS_CONT_HASH, a
                          Hash; RBS_CONT_ARRAY, an Array) gave way to the box
                          the body answers (ret_seed_holds): the slot stays
                          boxed, the return is checked to be a container of
                          that class in any form, once and always
                          (emit_rbs_cont_ret_wrapper), and a call of the
                          method answers no proc or Method */
  unsigned char ret_rbs_cont_nil; /* that seed was nilable (`Hash[..]?`): nil passes */
  int ret_rbs_nilable; /* that seed was RBS's nilable form (`Integer?`), and the
                          pinned kind is an unboxed scalar: the return can be
                          nil (an oint), so a caller boxing it has to answer
                          nil rather than the raw number (#3493) */
  int ret_nullable_int; /* the same property, INFERRED rather than seeded: this
                           method's scalar return can be nil because its own
                           return expression can be. Seeded from
                           ret_rbs_nilable, then propagated through pass-through
                           methods (`def pass(x) = x.p_`) and methods whose value
                           is their block's (`def key_of(x) = yield x`), which no
                           RBS signature covers (#3505). */
  unsigned char ret_handle; /* --share-strings: every return path reads a String
                               the rule shares, so a call's value is that
                               String's handle, which the callee's tail read
                               publishes (_sp_ret_strbuf); set once the
                               analysis settles (an_mark_handle_returns) */
  unsigned char ret_pub_bang; /* --share-strings: ret_handle, and a tail is a bang
                                 on a handle (`a.upcase!`), whose value form
                                 answers its bytes and publishes nothing: each
                                 tail is emitted by itself (emit_tail_value),
                                 the bang's as its handle's read, as for
                                 ret_pub_fresh */
  unsigned char ret_channel_check; /* --repr-check: the earlier pickup proof succeeded */
  unsigned char ret_unread; /* a String method no call reads the value of: every
                               call of its name drops it (an_mark_unread_returns),
                               so its body runs as statements and builds no value
                               -- a tail `@out << x` read the whole buffer out */
  unsigned char ret_fresh; /* the same return-tail walk proves a new String
                              (or nil) on every path; no incoming handle */
  int ret_param;       /* --share-strings: every non-fresh return reads this
                          unchanged parameter; -1 when no such proof */
  int ret_obj_may_nil; /* the nil fact for the method's value (analyze_nil.c,
                          #7444): its body's value or a `return` may be nil;
                          nonzero, where the nil comes from (NFW_*) */
  unsigned char ret_nil_pickup; /* --share-strings: a `return` of the method
                          answers nil and the deep-return pickup takes it
                          (an_returns_shared_handles, an_tail_answers_nil) */
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
  unsigned char ret_range; /* a Range can be the method's value: its tail or a
                       `return` (value_may_be_range), set by the return pass */
  SP_TY_FIELD int ret_proc_ret; /* when ret==TY_PROC: the returned proc's body return type
                       (TyKind), so a caller's `m.call` knows the result type */
  SP_TY_FIELD int blk_ret;     /* for a method with a &block param: the unified value type
                       its block yields across all call sites, so blocks passed
                       to it are emitted returning that (common) type */

  LocalVar *locals; /* params + body locals */
  int nlocals, clocals;
  int *lidx;        /* scope_local's index of `locals` by name: open
                       addressing over the names' hashes, slot + 1 per
                       entry, 0 empty. NULL until the table is long enough
                       to want one; dropped by whatever removes a local
                       (scope_local_intern keeps it over an append) */
  int lidx_cap;     /* a power of two, at least twice nlocals */
} Scope;

static inline TyKind scope_set_ret(Scope *s, TyKind t) {
  if (s->ret != t) { *(TyKind *)&s->ret = t; comp_touch_scope(s->idx); }
  return t;
}
static inline TyKind scope_set_ret_noblock(Scope *s, TyKind t) {
  if (s->ret_noblock != t) { *(TyKind *)&s->ret_noblock = t; comp_touch_scope(s->idx); }
  return t;
}
static inline int scope_set_ret_pin_guard(Scope *s, int t) {
  if (s->ret_pin_guard != t) { *(int *)&s->ret_pin_guard = t; comp_touch_scope(s->idx); }
  return t;
}
static inline int scope_set_ret_proc_ret(Scope *s, int t) {
  if (s->ret_proc_ret != t) { *(int *)&s->ret_proc_ret = t; comp_touch_scope(s->idx); }
  return t;
}
static inline int scope_set_blk_ret(Scope *s, int t) {
  if (s->blk_ret != t) { *(int *)&s->blk_ret = t; comp_touch_scope(s->idx); }
  return t;
}

/* A hashed index over one of a class's append-only name lists (ivars,
   readers, writers): the position of a name. It indexes the list as it
   grows and starts over when the list is another (a rebuild) or shorter.
   A class with N attributes asked N names took N*N compares (#10325). */
typedef struct {
  char *const *list;   /* the list it indexes */
  int n;               /* how many of its entries are in the table */
  int cap;             /* table slots, a power of two, or 0 */
  int *slot;           /* entry index + 1, 0 empty */
} NameIdx;
int name_idx_find(NameIdx *x, char *const *list, int n, const char *name);

typedef struct {
  char *name;          /* class name ("Point"); also drives semantic checks
                          (builtin reopen, name matching) and Ruby-visible output */
  int idx;             /* its index in Compiler.classes: the owner its slots' writes move */
  char *c_name;        /* C-identifier stem for `sp_<c_name>` emission: == name,
                          unless name collides with a runtime typedef (sp_RbVal,
                          sp_IntArray, ...) in which case it is disambiguated so a
                          user `class RbVal` does not redefine the runtime type */
  int def_node;        /* ClassNode id */
  unsigned char presence_read; /* constant pass: a read consumes the creation flag */
  int parent;          /* superclass index, or -1 */
  char **ivars;        /* instance variable names, incl. leading '@' */
  NameIdx ivars_ix;    /* comp_ivar_index's index of ivars */
  SP_TY_FIELD TyKind *ivar_types;
  unsigned char *ivar_str_shared; /* (#3227) the slot is a shared-mutable
                                     string handle (sp_String *): survives
                                     re-clears; post-fixpoint reasserts it */
  unsigned char *ivar_elems_shared; /* --share-strings: the TY_POLY_ARRAY slot
                                       holds its Strings as handles, as a
                                       LocalVar's elems_shared */
  unsigned char *ivar_nullable_int; /* the slot holds a nilable scalar: some
                                     write to it can leave nil (its nil byte),
                                     so a read of it (or of its attr_reader)
                                     has to box as nil rather than as the
                                     number. Set by the marking fixpoint,
                                     alongside LocalVar.nullable_int and
                                     Scope.ret_nullable_int (#3505). */
  unsigned char *ivar_arr_elem_arr_or_nil; /* every element of this container
                                     slot is a poly array or nil (see
                                     LocalVar.arr_or_nil) */
  unsigned char *ivar_nullable_int_elem; /* the same one level in: an int/float
                                     ARRAY slot some element of which can be
                                     nil (its nil bit) */
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
  unsigned char *ivar_alias_wide;  /* the alias propagation widened the slot
                                      to the general Array because another
                                      name of its Array is one
                                      (propagate_alias_widening): the
                                      re-narrow keeps it, as its own
                                      pushes do not show why */
  int nivars, civars;
  unsigned char *ivar_obj_may_nil; /* the nil fact per ivar (analyze_nil.c,
                                     #7444), indexed as ivars was when the
                                     analysis computed it, so read by name
                                     (nil_fact_ivar); n_ivar_obj_may_nil
                                     entries */
  int n_ivar_obj_may_nil;
  unsigned char *ivar_elem_may_nil; /* the same one level in, per ivar (an
                                     Array whose elements may be nil,
                                     LocalVar.obj_elem_may_nil), as many
                                     entries, indexed the same way */
  char **rbs_pin_ivars; /* ivar names (incl '@') pinned by an --rbs seed: the
                           fixpoint must not widen their type */
  int n_rbs_pin_ivars, c_rbs_pin_ivars;
  int ivar_box_held;    /* a box reached one of those container pins while the
                           fixpoint ran and the pin held: the settled types
                           decide (class_ivar_pin_holds, an_settle_seed_boxes) */
  char **cvars;        /* class variable names, incl. leading '@@' */
  SP_TY_FIELD TyKind *cvar_types;
  unsigned char *cvar_nullable_int; /* the Integer or Float class variable can
                                       hold nil (an oint), as
                                       ivar_nullable_int for an ivar */
  unsigned char *cvar_str_shared;   /* --share-strings: the TY_STRBUF slot is
                                       the shared handle, as ivar_str_shared */
  unsigned char *cvar_elems_shared; /* --share-strings: the TY_POLY_ARRAY slot
                                       holds its Strings as handles, as a
                                       LocalVar's elems_shared */
  int ncvars, ccvars;
  char **readers;      /* attr reader method names (no '@') */
  int nreaders, creaders;
  NameIdx readers_ix;
  char **writers;      /* attr writer base names (no '@', no '=') */
  int nwriters, cwriters;
  NameIdx writers_ix;
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
  /* The visibility an `extend` copy brings from its module. The class body's
     own entry above wins over it, whichever comes first. */
  char **xcm_vis_names;
  int  *xcm_vis_kinds;
  int nxcm_vis, cxcm_vis;
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
  /* the singleton accessors this class's own body declares, by method name
     (`x`, `x=`): an inherited one of the same name is in the lists too */
  char **sg_decl;
  int nsg_decl, csg_decl;
  int *sg_decl_node;     /* by entry of sg_decl: the declaring call, the latest */
  char **alias_new;    /* `alias new old`: alias_new[i] redirects to alias_old[i] */
  char **alias_old;
  int   *alias_cls;    /* class the lookup resumes from (an alias of an
                          INHERITED method keeps naming the ancestor's body
                          even when this class redefines the name), or -1 */
  int   *alias_node;   /* the alias statement's node id, so the pass that runs
                          once superclasses are wired can fill alias_cls */
  int   *alias_sg;       /* 1: the alias is a singleton one (`class << self; alias a b`):
                            only these name class methods (chain_lookup) */
  int   *alias_builtin; /* 1: the alias captured the builtin method of a
                           reopened primitive, which the class had not
                           defined where the alias appeared.
                           2: a captured builtin singleton method */
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
  int cext_layout; /* a TypedData family has this prefix, including ancestors */
  int cext_extension; /* +1 extension index for a TypedData allocator */
  int cext_allocator; /* recorder registration slot */
  int is_native_class;
  int is_builtin_const; /* builtin class/module name, classified at registration */
  /* --share-strings: a native class whose binding declares that its object
     keeps a String (`native_share ... "keeps"`): its objects are holders */
  int native_share_keeps;
  /* An Array subclass (#7449): its instances ARE Arrays -- the struct starts
     with the Array by value, so a pointer to one is a pointer to its Array --
     and Array's methods dispatch on them. ary_root is the class right below
     Array in the chain, plus one (0: not an Array subclass); ary_kind, read
     on that root (comp_ary_kind), is the kind of the Array every instance of
     the chain embeds, folded from the elements the program puts in.
     A Hash subclass is the same with its Hash: ary_root names the class
     right below Hash, ary_kind is a Hash kind, and ary_hash is set on every
     class of the chain. The comp_ary_* / comp_arysub_* functions answer for
     both; the Array-only ones say so. A String subclass is the same with its
     String, a mutable handle (sp_String) by value: ary_str is set on every
     class of the chain and ary_kind is TY_STRBUF. */
  int ary_root;
  TyKind ary_kind;
  int ary_hash;
  int ary_str;
  char *c_struct;      /* e.g. "sp_StringIO", or NULL */
  char *native_free;   /* finalizer C symbol, or NULL */
  /* native_subclass "header", "scan": the class may be subclassed (#9269);
     the header defines c_struct, and scan is what a subclass's scan calls */
  char *native_sub_header;
  char *native_sub_scan;
  /* A subclass of such a class: 1 + the native class its chain reaches. Its
     instances embed that class's struct ahead of their own ivars, so a
     pointer to one is a pointer to the native object */
  int nsub_root;
  int freeze_observed; /* freeze/frozen? reaches instances of this class: codegen
                          guards its ivar stores with the GC-header frozen bit */
  int computed_ivar_set; /* an instance_variable_set with a computed name can reach
                          instances of this class (#8219): the value's type joined
                          every declared ivar, codegen stores by name, and names
                          the class does not declare live in the object-keyed table */
  int presence_observed; /* a read that tells an assigned ivar from one never
                          assigned reaches instances of this class
                          (an_presence_observe): ivar_set_kind may give
                          its ivars an assigned flag */
  int presence_family; /* 1 + the class that stands for the classes sharing
                          this one's layout or methods (those above and below
                          it, the modules they include or that include them),
                          which agree on an ivar's assigned flag; 0 for none
                          or one shared with Object (an_presence_families) */
  int is_value_type;   /* small immutable scalar-ivar class represented by value
                          (sp_X, not sp_X *): no heap alloc / GC. Set by
                          detect_value_types after analysis. */
  int ctor_reachable;  /* the early census (compute_instantiated before the type
                          fixpoint): a construction site of this class sits in
                          reachable code, or nothing can tell. Read only by the
                          poly-receiver return-type union, for native classes. */
  int ctor_site;       /* a construction of this class (`.new`, `.allocate`, a
                          bare `new` in its class method, `raise Cls`) sits in
                          reachable code, or nothing can tell: what the census
                          saw before a native or Struct class is held live
                          regardless. Read for a native class's Ruby-side defs
                          (comp_native_binding_class). */
  int instantiated;    /* a value with this exact cls_id can come into existence
                          somewhere: `.new`/`.allocate`/`raise Cls`/Struct, or a
                          Marshal.load that can mint any class. When clear, no
                          poly value is ever this class, so the poly-dispatch
                          switch can drop its `case` arm (the referenced method
                          then becomes an unreferenced static the C compiler
                          DCEs). Set by compute_instantiated. */
  int unreferenced;    /* a class the program opens with `class` that no reached
                          code names (compute_reachable, #10592): none of its
                          methods is reached and it has no instance */
  int ctor_late;       /* the census after the last reachability pass found
                          an object of it built where only that pass reached:
                          it gets a constructor, no dispatch arm (#10649) */
  int no_instance;     /* such a class whose instance side no reached code
                          opens: its singleton methods may run, but nothing
                          builds one, and its instance methods stay unreached */
  int inst_below;      /* some proper descendant of this class is instantiated
                          (compute_instantiated): the class has no objects of
                          its own, but the methods that descendant inherits
                          run on it */
  int live_sides;      /* the sides an earlier compute_reachable opened: 1 its
                          singleton methods, 2 its instances; kept open */
  /* Prepend shadow chain: when `prepend M` is called on this class,
     M's methods overwrite the active slot; the previous slot is
     renamed to a shadow `__prep N <m>`.  The chain maps each name
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
  int *prepended_mods;   /* the modules it prepends (a program module of the class table) */
  int nprepended_mods, cprepended_mods;
  /* Where Enumerable sits among this class's own includes: 0 when its bodies
     do not include it, else 1 + the number of program modules included
     before it (a later include goes in front in Ruby's lookup). Set where
     the includes are processed (process_include_body). */
  int enum_inc_at;
  int cmp_inc_at;       /* Comparable uses the same include order facts;
                          1 also marks a builtin Comparable superclass. */
  int cmp_prepended;    /* 1: direct prepend Comparable; 2: a prepended module
                          reaches it, behind that module's own methods. */
  int enum_prepended;   /* its bodies `prepend Enumerable`: in front of the class itself */
  /* The attr_reader / attr_accessor names this class's own bodies declare,
     as against readers[], which also holds those a module it includes and a
     superclass bring. */
  char **own_readers;
  int nown_readers, cown_readers;
  NameIdx own_readers_ix;
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

/* The bits of ClassInfo.ivar_elems_shared and cvar_elems_shared */
enum {
  ELEMS_POLY = 1      /* the TY_POLY_ARRAY slot holds its Strings as handles */
};
static inline TyKind ivar_set_type(ClassInfo *ci, int k, TyKind t) {
  if (ci->ivar_types[k] != t) { ((TyKind *)ci->ivar_types)[k] = t; comp_touch_class(ci->idx); }
  return t;
}
static inline TyKind cvar_set_type(ClassInfo *ci, int k, TyKind t) {
  if (ci->cvar_types[k] != t) { ((TyKind *)ci->cvar_types)[k] = t; comp_touch_class(ci->idx); }
  return t;
}

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

typedef struct { char *name, *init; int slots, constants; } CextExtension;
typedef struct { char *key; int kind, argc, node; } CextStub;
typedef struct { char *path; int module, extension, allocator; } CextClass;

/* One `native_func` declaration (typed static binding to carried C). Unlike
   FfiFunc, arg/ret specs are the spinel type language ("any"/"string"/"int"/
   "float"/"bool"; ret also takes "cstring" -- a borrowed C string, e.g. a
   static buffer, that the call site dups onto the GC heap) and csym is the
   C symbol to call. feat is the strict-require feature name from the module's
   `native_lib`, or "" (always available). */
typedef struct {
  char *mod;       /* module name */
  char *name;      /* Ruby method name */
  char *ret;       /* return type spec */
  char *csym;      /* C symbol to emit */
  char *feat;      /* strict-require feature name, or "" */
  char **args;     /* arg type specs (malloc'd) */
  int nargs;
} NativeFunc;

/* One `native_obj` declaration: a carried C object the package links on demand
   (only when its module's feature is required). path is root-relative, e.g.
   "packages/json/sp_json.o"; feat is the strict-require feature or "". */
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
  /* --share-strings: what the binding does with the String its object
     keeps, as the package declares it (`native_share`): NSH_* bits, and
     the C symbol of the form that takes or answers it as the shared
     handle (sp_String *), or NULL */
  unsigned share;
  char *share_csym;
} NativeMethod;
enum {
  NSH_KEEPS   = 1,   /* a constructor: the object keeps its first String
                        argument, or with none a String of its own */
  NSH_ANSWERS = 2,   /* answers the String the object keeps */
  NSH_CHANGES = 4,   /* changes the String the object keeps */
  NSH_FRESH   = 8    /* answers a new String, one no other name holds */
};
/* Whether a binding accepts a call of argc positional arguments. */
static inline int native_takes(const NativeMethod *m, int argc) {
  return m->nargs == argc || (m->rest && argc > m->nargs);
}

/* A user-method call as inference bound it (--plan-check, #7100): the
   method scope, the class whose chain the binding arm searched (-1 for a
   top-level def), and which arm bound it. via 0 (UC_NONE): no binding. */
enum { UC_NONE, UC_TOP, UC_INST, UC_CMETH, UC_SUPER, UC_SEND_BLIND, UC_IE,
       UC_INCLUDED, UC_REOPEN,
       UC_POLY };   /* a boxed receiver's dispatch: the first user candidate
                       stands for the union the call was typed over */
typedef struct { int mi; short owner_ci; unsigned char via; } UCallInf;

typedef struct {
  const NodeTable *nt;
  AutoloadRegistration *autoloads; int nautoloads, auto_nodes;
  int *auto_at; /* parsed node -> first registration + 1; names/paths borrowed from nt */
  SP_TY_FIELD TyKind *ntype;    /* [node_cap] node id -> inferred type */
  int *norigin;     /* [node_cap] node id -> where its degraded type came from: the
                       child (or the slot's why.node, for a read) that carried the
                       poly in, itself when the poly was born here, -1 when the
                       type is not degraded. Set where ntype is, in infer_type. */
  SP_TY_FIELD unsigned char *strbuf_box; /* [node_cap] LocalVariableReadNode of a shared-
                          mutable string in a container-store position: the
                          read yields the sp_String* HANDLE (typed TY_STRBUF,
                          boxed SP_BUILTIN_STRBUF), not the demoted cstr
                          (#3227 phase 3) */
  /* --share-strings: the names of the calls whose answer is kept (not
     dropped), sorted, built once on first ask (strbuf_tail_answer_kept) */
  const char **kept_call_names; int nkept_call_names, kept_call_built;
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
                          unchanged. STRBUF_LIFT_FRESH_CALL: a boxed call's
                          new String that a container store lifts into a
                          handle (strbuf_demand_store_leaf). */
  unsigned char *nil_tested; /* [node_cap] a builtin call's receiver whose nil
                          its nil arm has already tested (cplan_nil, #7444),
                          or an object's under its kind query's nil arm
                          (emit_object_kind_nil): set only as a view
                          (VR_NIL_TESTED) around the call's own emission,
                          so the call is armed once; 2 when a cached array
                          read tests it in its out-of-range branch
                          (emit_nil_target_cold) */
  int args_in_call;     /* the receiver of the call an arm last emitted with
                          every operand a C argument of the one runtime call
                          that raises (sp_poly_add, sp_poly_shl, ...), for
                          the nil arm's trial (nil_target_runs_in_call) */
  int args_in_call_trial; /* nonzero while that trial emits: a call nested
                          in it takes its head without a trial of its own,
                          so nested operators are not emitted 2^depth times */
  unsigned char *head_held; /* [node_cap] an operand the nil arm's head
                          (emit_nil_target_head) ran into a temp of its own
                          ahead of the call: set only as a view
                          (VR_HEAD_HELD) around the call's own emission,
                          where an arm that holds the operand reads that
                          temp (hold_operand) */
  SP_TY_FIELD TyKind *nilnarrow; /* [node_cap] param-read narrowed by a `return .. if p.nil?`
                        guard: the read's non-nil type (codegen unboxes the poly
                        slot at the read site); TY_UNKNOWN = not narrowed */
  /* What a statement binds, for codegen's question whether anything in the
     statement around an argument temp can rebind the local the temp copies
     (stmt_may_rebind_local, codegen_util.c). Filled per statement on its first
     query and kept: stmt_wr_state[stmt] is 0 (not yet), 1 (computed) or 2
     (opaque: a binding the walk cannot name); stmt_wr_names[stmt] the
     NULL-terminated local names its subtree writes, targets or takes as
     block parameters; stmt_wr_mark[node] the last statement whose walk
     reached the node. [stmt_wr_cap] each, grown with the node table, NULL
     until a program asks. */
  unsigned char *stmt_wr_state;
  const char ***stmt_wr_names;
  int *stmt_wr_mark;
  int stmt_wr_cap;
  unsigned char *nil_fact; /* [nil_fact_n] the nil fact per node (analyze_nil.c,
                        #7444): NF_MAY_NIL when the node's value may be nil,
                        NF_NOT_NIL when it cannot; read through nil_fact_node */
  unsigned char *nil_elem_fact; /* [nil_fact_n] per node: an Array value whose
                        elements are pointers some of which may be nil
                        (LocalVar.obj_elem_may_nil); read through repr_of's
                        elem_nil_marked */
  int nil_fact_n;
  int *cls_fact;    /* [cls_fact_n] per node: the index in cls_pool of the
                       set of user classes its value can hold (analyze_cls.c,
                       #8530); 0 is every class. Read through cls_fact_node. */
  ClsSet *cls_pool; int ncls_pool, cls_fact_n;
  unsigned char *cls_kind; /* [cls_fact_n] per node, in a program that calls
                       compare_by_identity: the CK_* kinds its value, and
                       (shifted by CK_CONTENTS) the elements of the
                       containers it holds, can be (cls_fact_kinds) */
  int cls_kind_n;
  int range_unheld; /* while the arguments of a call on a receiver of no known
                       class bind to every method of the name: a Range among
                       them makes no parameter range_held (slot_set) */
  /* an_send_name_is_computed's memo of the parameters a caller hands a
     computed name (#8592): one byte per (scope, parameter) slot, 0 unknown,
     1 no, 2 yes; rebuilt when the node table grew */
  unsigned char *send_param_memo; int send_param_memo_n, send_param_memo_nodes;
  int *nscope;      /* [node_cap] node id -> owning scope index */
  int *node_cbody;  /* [node_cap] node id -> enclosing class/module-body class id, or -1 */
  SP_TY_FIELD char *empty_arr_recv; /* [node_cap] empty `[]` used as a direct receiver/interpolation -> TY_POLY_ARRAY */
  SP_TY_FIELD char *empty_hash_recv; /* [node_cap] empty `{}` used as a direct receiver/interpolation -> TY_STR_POLY_HASH */
  SP_TY_FIELD char *empty_hash_arg;  /* [node_cap] empty `{}` passed as a user-method arg -> TY_POLY_POLY_HASH */
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
  SP_TY_FIELD TyKind *hash_want; /* [node_cap] variant a hash literal should take from its use context (#3040) */
  SP_TY_FIELD TyKind *arr_want;  /* [node_cap] array kind a node takes from its use context
                        rather than from its own contents: an empty `[]`
                        literal, or a `map` that narrow_object_arrays decided
                        builds a table of rows */
  int *oa_walked_node; TyKind *oa_walked_kind; int oa_walked_n; /* the tables that
                        oa_stamp_walked_tables stamped last round, and their kinds */
  TyKind *lw_joined; /* [node_cap] for a local write: the type its value had when
                        the slot was last joined with it (infer_write_types,
                        rejoin_local_writes). A write whose value reads
                        differently since is what a late re-join follows. */
  SP_TY_FIELD TyKind *poly_builtin_ty; /* [node_cap] for a container read on a poly receiver a
                              user class also owns: the type the builtin surface
                              alone would give, so codegen can shape its arm (#3459) */
  const struct BuiltinOp **bop_inf; /* [node_cap] the builtin-op row inference
                                       answered the call with (--plan-check only) */
  UCallInf *ucall_inf; /* [node_cap] the user method inference bound the call
                          to (--plan-check only) */
  int *hash_default_arg_memo; /* [node_cap] hash_new_default_arg(node) memo; INT_MIN = uncomputed */
  unsigned hash_default_arg_memo_gen; /* scope-index generation the memo was built for */
  int hash_default_arg_memo_cap;      /* allocated length of hash_default_arg_memo */
  int node_cap;     /* allocated length of ntype/nscope (>= nt->count) */

  Scope *scopes;    /* scope[0] = top level */
  int nscopes, cscopes;
  /* the top-level scope of each file root (nt_root), by graph id: 0 for the
     entry (analyze_program.c) */
  int *file_scope;
  /* the load-phase record: where each statement of the load phase runs
     (analyze_elab.c), rebuilt when the node table changes */
  struct LoadRecord *lrec;
  /* the program module's memo of prog_load_sets_flag: [node] a load that
     sets its loaded flag where its value is read by nothing, since another
     load reads that flag (analyze_program.c); NULL until first asked */
  unsigned char *load_sets_flag;
  /* the program names or makes a method by something its text does not
     spell -- a send, a method object or a define_method of a computed name,
     text it evaluates -- as an_note_written_names reads the tree; a module
     mixed in and a file loaded are not counted (elab_runs_unseen) */
  int names_by_value;

  /* the subset of `classes` synthesized for an anonymous Struct/Data, so
     resolving one does not walk every user class; invalidated wherever
     is_anon_struct is set */
  int *anon_struct_ids;
  int n_anon_struct_ids, anon_struct_ids_valid;
  int has_anonymous_classes; /* Class.new identities with execution-time names */
  int user_define_method;    /* 0 not asked yet, 1 none, 2 the program defines or aliases its own define_method */

  /* local-write-by-name index; see comp_lvw_first */
  int *lvw_head;        /* [lvw_nbuckets] first write id in each name bucket */
  int *lvw_next;        /* [lvw_count] next write id sharing the bucket */
  int lvw_nbuckets, lvw_count;
  unsigned lvw_version; /* nt->version the index was built for */
  NtStamp lvw_stamp;
  int lvw_built;

  /* local-write-by-(scope,name) index; see comp_lvw_first_sc */
  int *lvws_head;       /* [lvws_nbuckets] first write id in each bucket */
  int *lvws_next;       /* [lvws_count] next write id sharing the bucket */
  int lvws_nbuckets, lvws_count;
  unsigned lvws_version;
  NtStamp lvws_stamp;
  int lvws_built;

  /* node-by-kind chains; see comp_kind_first */
  int *kind_head;       /* [NK_COUNT-ish buckets] first node id of each kind */
  int *kind_next;       /* [kind_count] next node id of the same kind */
  int kind_nkinds, kind_count;
  unsigned kind_version;
  int kind_built;

  /* ReturnNode-by-scope chain; see comp_sret_first */
  int *sret_head;       /* [sret_nscopes] first ReturnNode id in each scope */
  int *sret_next;       /* [sret_count] next ReturnNode id in the same scope */
  int sret_nscopes, sret_count;
  unsigned sret_version;
  int sret_built;

  /* CallNode-by-scope chain; see comp_scall_first */
  int *scall_head;      /* [scall_nscopes] first CallNode id in each scope */
  int *scall_next;      /* [scall_count] next CallNode id in the same scope */
  int scall_nscopes, scall_count;
  unsigned scall_version;
  int scall_built;
  /* CallNode-with-a-literal-block chain, by the block's scope; see
     comp_bcall_first */
  int *bcall_head;      /* [bcall_nscopes] first such CallNode id per scope */
  int *bcall_next;      /* [bcall_count] next one whose block is in the same scope */
  int bcall_nscopes, bcall_count;
  unsigned bcall_version;
  int bcall_built;
  /* ReturnNode chain, by the scope the return is in; see comp_ret_first */
  int *ret_head;        /* [ret_nscopes] first ReturnNode id per scope */
  int *ret_next;        /* [ret_count] next one in the same scope */
  int ret_nscopes, ret_count;
  unsigned ret_version;
  int ret_built;

  /* (CallNode, ivar-read argument)-by-ivar-name index; see comp_ivarg_first */
  int *ivarg_head;      /* [ivarg_nbuckets] first entry in each name bucket */
  int *ivarg_next;      /* [ivarg_count] next entry sharing the bucket */
  int *ivarg_call;      /* [ivarg_count] an entry's CallNode */
  int *ivarg_arg;       /* [ivarg_count] its InstanceVariableReadNode argument */
  int ivarg_nbuckets, ivarg_count;
  unsigned long ivarg_stamp;   /* the table's edits + version it was built at */
  int ivarg_built;

  /* variable-site chains by (kind, variable); see comp_vsite_first */
  int *vs_head;         /* [vs_nbuckets] first entry of each (kind, variable) bucket */
  int *vs_site;         /* [vs_count] an entry's site node */
  int *vs_var;          /* [vs_count] the read or write naming its variable */
  int *vs_next;         /* [vs_count] the next entry sharing the bucket */
  unsigned char *vs_kind; /* [vs_count] an entry's site kind (VsKind) */
  int *vs_rparent;      /* [vs_nodes] the call whose receiver a node is, or -1 */
  int *vs_whead, *vs_wnext; /* [vs_nodes] the local writes whose value a node is, in node order */
  int vs_nbuckets, vs_count, vs_cap, vs_nodes, vs_toplevel;
  unsigned vs_version, vs_gen;
  int vs_built;

  char **symbols;   /* interned symbol names; index = sp_sym id */
  size_t *symbol_lens;  /* each name's BYTE length: a name may hold a NUL, and
                           strlen would end it there (#nul symbols) */
  int nsymbols, csymbols;

  int core_cls[CORE_N];     /* first class of each core name, -1 before creation */
  ClassInfo *classes;
  int nclasses, cclasses;
  int toplevel_class;
  int *class_head, *class_next, class_buckets;   /* name -> class index (comp_class_index) */
  int *class_def_head, *class_def_next;   /* def node -> class index (comp_class_of_def), same buckets */
  unsigned char class_hash_check;   /* SPINEL_ELAB_CHECK: each lookup also scans the classes */
  int object_standin;   /* +1-based index (0 = none) of the empty root class an
                          `o = Object.new` binding's singleton subclass stands
                          on: Object has no user class to subclass. It answers
                          as the builtin Object wherever Ruby can see it (its
                          name, #class, instance_of?: visible_cls_id). */
  int has_arysub;      /* some class is an Array or Hash subclass (ClassInfo.ary_root, #7449) */
  /* the nodes infer_type answered as an Array subclass instance's Array
     (ary_operand, an_ary_viewed_mark), indexed by node; NULL until one is */
  unsigned char *ary_viewed;
  int ary_viewed_cap;

  LocalVar *gvars;    /* global variables ($g), name without '$' */
  int ngvars, cgvars;
  int *gvar_ix, gvar_ix_cap;   /* open addressing over the names, by index into gvars (-1 free); a program with thousands of globals asks by name on every read */
  LocalVar *consts;   /* top-level constants (FOO) */
  int nconsts, cconsts;

  /* alias $copy $orig → gvar_alias_from[i]="copy", gvar_alias_to[i]="orig" */
  char **gvar_alias_from;
  char **gvar_alias_to;
  int ngvar_aliases;

  int *toplevel_includes;  /* class indices of modules included at top level */
  int ntoplevel_includes;
  unsigned char *toplevel_extends;  /* per entry: 1 for `extend` (main alone), 0 for `include` */
  int has_include_math;    /* program has `include Math`: expose bare PI/E/fns */
  /* some store in the program can put a nil VALUE into a typed Hash of this
     Integer-valued kind (hash_vals_nullable, DESIGN.md D3b-ii) */
  unsigned char hash_vnil_str_int, hash_vnil_int_int;

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

  CextStub *cext_stubs;
  int n_cext_stubs;
  CextExtension *cext_extensions;
  int n_cext_extensions;
  CextClass *cext_classes;
  int n_cext_classes;

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
  int gen_comparable; /* program needs the boxed Comparable membership callback */
  /* the program has a `**` operand and defines #to_hash: the bridge carries
     the #to_hash row a boxed operand converts through (sp_kw_splat_conv). */
  int uses_kw_to_hash;
  /* the methods CRuby calls that the program does not name, as the last
     reachability run found them (implicit_calls.h); freed with the Compiler */
  struct IcState *ic;
  int uses_exc_text_handle;  /* emitted shared exception text routes, to_s/message bits */
  char *exc_text_late;  /* the plain exception text dispatchers, defined after the globals (emit_exc_text_dispatch) */
  int exc_text_super;        /* exc_text_override_calls_super's memo: 0 unasked, 1 no, 2 yes */
  /* body-node id -> enclosing BlockNode id (lazy; emit_stmts block-local
     resets). Covers the first blk_body_map_n nodes, extended as the table
     grows; -1 = not a block body. */
  int *blk_body_map;
  int blk_body_map_n;
  struct ReprChannelCheck *repr_channel_check; /* --repr-check return-channel shadow */
  /* parsed node id -> the number a name invented from the node carries
     (comp_node_tag), bit 0 set for one counted in a base, and that base. A
     node counts within its base, the def, class or module it is in or the
     builtin def it was spliced in: per base id, the count so far and the
     text it is named by. */
  int *node_ord, *node_base;
  int node_ord_prog, node_ord_parsed;
  /* appended node id - node_ord_parsed -> its tag (appended_tag), and how
     many were named with no origin */
  char **node_tag; int node_tag_cap, node_tag_lost;
  /* the texts of the last eight parsed nodes named, and the next to reuse */
  char node_tag_ring[8][64]; int node_tag_next;
  int *bi_base_cnt;
  char **bi_base_key;
  int bi_base_cap;
  /* a parsed program node's base: the nearest def, class or module it is in
     (itself, for one of those), or -1 at the top level (comp_node_pbases) */
  int *node_pbase; int node_pbase_n;
  char **pbase_keys; int *pbase_key_cnt; int pbase_key_slots, pbase_key_n;   /* key -> times seen */
  /* --share-strings (SPINEL_SHARE_STRINGS): every String slot is the
     shared handle (#6765); the share classes (share.h) are rebuilt as the
     analysis goes for the questions still asked of them. */
  int share_borrows;    /* arguments share_mark_borrows lets borrow the bytes */
  /* the program's parent map, for the emitter's questions about where a node
     sits (codegen_util.c's codegen_parents); rebuilt when the node table
     changed, freed with the Compiler */
  int *cg_parent;
  int cg_parent_n;
  unsigned cg_parent_ver;
  /* the last eight method C names mc_method spelled, and the next to reuse:
     each grown to its name (mc_ring_setf), which no length bounds */
  char *mc_ring[8]; size_t mc_ring_cap[8]; int mc_ring_next;
  /* --share-strings: does the program make an Enumerator over a String
     handle (str_enum_h_made): 0 not asked yet, 1 no, 2 yes */
  unsigned char str_enum_h;
  /* does any call find no method where it runs (cplan_program_missing):
     0 not asked yet, 1 no, 2 yes */
  unsigned char missing_calls;
  /* per node, once asked: the node cplan_missing_in_order answers for it,
     + 2 (1: none; 0: not asked yet) */
  int *missing_order;
  int missing_order_n;
  /* per local write node, once asked: the source lazy_alias_src_hoist
     answers for it, + 2 (1: none; 0: not asked yet) */
  int *lazy_hoist;
  int lazy_hoist_n;
  int share_strings;
  int has_cm_builtin_alias;   /* a singleton alias captured a builtin's class method (alias_builtin 2) */
  /* the scope + 1 whose block a yield under emit_proc_yield hands its
     arguments to (a proc form, or a method with a `&blk` it calls), or 0 */
  int proc_arg_src;
  struct ShareFacts *share;
  /* the String plan's memo (share_plan.c): per node, its need, the role of
     its consumer, and its type when the need was asked, filled at seal under
     --share-strings, and the node table's version then */
  unsigned char *share_plan;
  TyKind *share_plan_ty;
  int share_plan_n;
  unsigned share_plan_ver;
  /* the String plan's supply along chains of self-answering calls, per
     link, for the whole compile; an entry is reused only while the view
     epoch, the node table's version and its deciding node's answer are
     unchanged (share_plan_sup_valid) */
  struct SharePlanSup *share_sup;
  int share_sup_n;
  /* --share-strings: the walk deciding the methods' answers while
     an_mark_handle_returns runs (analyze.c), else NULL: a method's answer
     asked meanwhile is decided where it is asked (scope_ret_handle,
     scope_ret_fresh, scope_ret_param) */
  struct RetHandles *ret_walk;
  /* per class, whether its class object can be a value
     (comp_class_may_be_value), built for the node and class counts kept */
  unsigned char *cls_value;
  int cls_value_n, cls_value_nodes;
  /* --share-strings: each node's parent at seal, for where a new String is
     boxed (share_plan_box_plain) */
  int *share_parent;
  int share_parent_n;
  /* strbuf_bang_self_local's answers within one outermost query
     (codegen_util.c): per node, the query it was answered in and the
     answer. Its recursion over a receiver chain reaches each link by
     several paths; within a query nothing it reads changes. A refusal
     closes an open query before it unwinds (strbuf_bang_query_close). */
  unsigned *bsl_gen;
  unsigned char *bsl_val;
  int bsl_n, bsl_depth;
  unsigned bsl_cur;
  struct HandleArgTab *share_args; /* final callers, while sealing share facts */
  struct SbArgIndex *sb_args; /* settled parameter sources, built once for storage */
  unsigned share_sig;   /* the types the facts were last applied over */
  unsigned share_in_sig;   /* the types the last application started from */
  /* the methods compute_byref_out_params let take a lent slot
     (an_byref_eligible_scopes), kept for the share facts built after it
     (nbyref_elig scopes; NULL before it runs) */
  char *byref_elig;
  int nbyref_elig;
  /* the names the program's global writes assign, as of the node table it
     was made for (analyze_infer.c, gvar_has_write; NULL before the first ask) */
  struct GvarWrites *gvar_writes;
  /* the inline hint's call counts (codegen.c, mih_count_calls): the first
     scope of each name, hashed (mih_head, mih_cap slots), and each scope's
     first scope of its name (mih_first) */
  int *mih_head, *mih_first, mih_cap;
  /* ...and its counts: the AST nodes of each scope (mih_nodes) and the call
     sites naming each first scope's method (mih_calls), for the node table
     mih_nt held mih_ntcount nodes of and mih_nscopes scopes */
  int *mih_nodes, *mih_calls, mih_nscopes, mih_ntcount;
  const NodeTable *mih_nt;
  /* the classes a boxed receiver can be an instance of, per call, and the
     name indexes the walk that answers it reads (analyze_scope.c's
     poly_ivar_set_reaches), built on first use and freed with the compiler */
  struct PivsFacts *pivs;
  unsigned mixin_gen;   /* bumped when a class records an include or a prepend */
  struct BuiltinGiven *builtin_given; /* the names the program gives to builtin scopes (analyze_util.c) */
  /* an ivar of a builtin value can be written (desugar_builtin_ivars): a
     reflective read, list or copy of an Array, a Hash or a Random asks the
     runtime's map (sp_bivar_*), and the boxed set gains its builtin arm */
  int bivar_table;
  /* per-arm facts (#7483, analyze_arm.c): a block's body typed under one
     arm's type for its first parameter, recorded once by the analysis */
  struct ArmFact *arm_facts; int narm_facts, carm_facts;
  /* the ivar names whose writes or removals an assigned flag cannot follow
     (an_presence_observe, read by ivar_set_kind): PRES_UNMARKED, a write no
     emitter marks reaches one (`for @x in`, `rescue => @x`, a writer reached
     by its name); PRES_REMOVED, the program
     removes one. pres_any: an instance_variable_set of a computed name
     reaches every name. pres_seen: a read of presence reaches some class
     (an_presence_class). pres_obs: per method scope, whether the code of
     its body may see the object's ivars (ivs_body_may_observe; the answer
     does not depend on the ivar asked about), sized pres_obs_n. */
  char **pres_names;
  unsigned char *pres_flags;
  int npres, cpres;
  int pres_any;
  int pres_seen;
  unsigned char *pres_obs;
  int pres_obs_n;
  /* ivar_set_kind's answers, 64 ivars per class: the final kinds, then the
     base kinds (ivs_base_kind); pres_memo_n counts both, -1 before the first */
  int *pres_memo;
  int pres_memo_n;
  /* per presence family (1 + its index): whether its ivars keep the rank of
     their first assignment (ivar_ranked); 0 not yet asked, 1 no, 2 yes */
  unsigned char *pres_rank;
  int pres_rank_n;
  /* the program makes objects without initialize (allocate, Marshal.load),
     so the order initialize assigns the ivars in says nothing of theirs; and
     the listing order of each class (ivar_listing_order_new) */
  int pres_noinit;
  int **pres_ord;
  int pres_ord_n;
  /* the cells of the later prepends whose copies a guard entry chooses
     (Scope.mixin_guarded, ver_prepend_cell_name): 1 .. n_prepend_cells */
  int n_prepend_cells;
  /* the version entries (call_plan.c, ver_entries_build): one method per
     (class, side, name, parameter count) for the calls whose version is
     known only at run time, or with guard, for the calls that reach a
     later prepend's copy (Scope.mixin_guarded), and each of its arms'
     calls pinned to the method it runs */
  struct VerEntry { const char *name; int guard, cls, sg, ar, scope; } *ver_entries;
  int nver_entries;
  int nver_refl;   /* the named entries whose name reflection does not answer (call_plan.c, ver_refl_named) */
  int ncond_disp;  /* the dispatchers defs under a run-time condition are lowered to (analyze_desugar.c; call_plan.c, ver_list_held) */
  int ncond_lone_disp;  /* those of them a lone def is lowered to (call_plan.c, feature_limit) */
  /* the open histories no reader names yet, sorted by name (call_plan.c,
     ver_named_one): made once a call names one, taken 1 when asked for and
     2 once made (ver_entries_demand) */
  struct VerDefer { const char *name; int cls, sg, taken; } *ver_defer;
  int nver_defer;
  struct VerChain *ver_chain;   /* how many versions a class's chain holds of a name, by (class, side, name) (call_plan.c, ver_chain_count) */
  struct VerHold *ver_hold;   /* the holders of a name worth asking, by name (call_plan.c, ver_holders) */
  /* ...by name: ver_ix chains the entries of one (class, side, name, parameter
     count) and ver_names one scope-holding entry of each name (ver_entries_index) */
  int *ver_ix, *ver_ix_next, *ver_names, *ver_names_next;
  unsigned ver_ix_nb;
  struct VerPin { int node, scope; } *ver_pins;
  int nver_pins;
  /* the entries are made (ver_entries_build): the versions the calls get
     are kept, and a call that reaches a later prepend's copy binds its
     guard entry */
  int ver_built;
  /* the scopes that are hidden copies (Scope.shadow), which
     comp_name_is_shadow_copy reads */
  int *shadow_copies, nshadow_copies, cshadow_copies;
  /* the version each call asked on a class runs, once the entries are
     made (call_plan.c, ct_version): open addressing, name NULL for a free
     slot; dc the class it answers, -2 for the table's */
  struct VerMemo { const char *name; int id, cls; unsigned flags; int vi, dc; } *ver_memo;
  int ver_memo_cap, ver_memo_n;
  /* the operand-local reads splat_dispatch_on_length marked splat_view,
     which desugar_splat_view_scalars rewrites once typed (-1 once seen) */
  int *splat_views;
  int nsplat_views;
  /* the slot names desugar_reassigned_block_params invents, each with the
     length of the name it stands for (reassigned_param_written_len) */
  char **rbp_slot_names;
  size_t *rbp_slot_lens;
  int nrbp_slots;
  /* the names the shadow rename invents for block parameters, each with the
     length of the name the program wrote (block_param_written_len) and the
     block it was invented for: an open table of mask+1 slots, nbp used */
  char **bp_name;
  size_t *bp_len;
  int *bp_blk;    /* the block each was invented for (block_param_made_for) */
  unsigned bp_mask;
  int nbp;
  /* the names the program wrote that hold `__bp` or `__bq`, collected when
     the first invented name is made (bp_user_uses) */
  char **bp_user;
  int nbp_user, bp_user_built;
  /* the method names a definition query (`method_defined?(:m)`, its public
     form) asked before the walk, where a fold answered it from the program's
     text (desugar_engine_branches, desugar_def_unless_method_defined): a
     method the walk defines at its point under one of them would have
     answered the query the other way, so a form that would declines */
  char **def_asked;
  int ndef_asked, cdef_asked;
} Compiler;
enum { PRES_UNMARKED = 1, PRES_REMOVED = 2 };

AutoloadRegistration *comp_autoloads(const Compiler *c, int node, int *count);
Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);
void ic_free(Compiler *c);   /* implicit_calls.c */
/* Is `name` one of the n strings in `list`? (0 for a NULL name) */
int name_list_has(char **list, int n, const char *name);
void comp_def_asked_add(Compiler *c, const char *name);   /* a fold answered a definition query of name (def_asked) */

/* Resize per-node arrays (ntype/nscope) after the node table grew. */
void comp_grow_node_arrays(Compiler *c);
void comp_node_pbases(Compiler *c);   /* the parsed program nodes' bases (comp_new) */
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
/* Can a call's block `blk` (a literal or a `&blk` argument) assign the
   variable argument node `arg` reads while the call runs? A local: the
   literal's body writes it (of the same scope, by any write kind, at any
   depth), or a proc that captures it assigns it. An instance variable:
   the literal's body may write it (subtree_may_write_ivar: a write, a self
   call whose method may, anything else that runs code), and a `&blk`
   argument always may. 0 for any other argument, or a call with no block. */
int comp_block_rebinds_arg(Compiler *c, int blk, int arg);
/* Can the subtree at id write ivar `iv` of class cls's object: a write of
   it, a self call whose method may, or anything else that runs code
   (codegen_util.c) */
int subtree_may_write_ivar(Compiler *c, int id, const char *iv, int cls, int depth);
/* Does the subtree under node n assign the variable argument node `arg`
   reads (a local of the same scope, or an instance variable), by any write
   kind, at any depth? 0 when `arg` is no variable read. */
int comp_node_writes_var(Compiler *c, int n, int arg);
int comp_lvw_first(Compiler *c, const char *name);
int comp_module_includes(Compiler *c, int m, int mod, int depth);
int comp_class_singleton_has_module(Compiler *c, int ci, int mod);
int comp_class_extends_any(Compiler *c, int ci);
int comp_lvw_next(const Compiler *c, int w);
int comp_lvw_first_sc(Compiler *c, int scope_idx, const char *name);
int comp_lvw_next_sc(const Compiler *c, int w);
int comp_lvw_any_sc(Compiler *c, int scope_idx, const char *name);
int comp_scall_first(Compiler *c, int scope_idx);
int comp_scall_next(const Compiler *c, int u);
int comp_bcall_first(Compiler *c, int scope_idx);
int comp_bcall_next(const Compiler *c, int u);
/* the ReturnNodes of a scope, in node order (one index per node-table
   version) */
int comp_ret_first(Compiler *c, int scope_idx);
int comp_ret_next(const Compiler *c, int u);
int comp_ivarg_first(Compiler *c, const char *name);
void comp_ivarg_invalidate(Compiler *c);
int comp_ivarg_next(const Compiler *c, int e);
int comp_ivarg_call(const Compiler *c, int e);
int comp_ivarg_arg(const Compiler *c, int e);
/* The owning class of an ivar read or write node, or -1. */
int comp_ivar_owner(Compiler *c, int node);
/* The PRES_* facts of an ivar name (Compiler.pres_names), and adding one */
int comp_pres_flags(const Compiler *c, const char *ivn);
void comp_pres_note(Compiler *c, const char *ivn, int flag);
typedef enum { VS_READ, VS_WRITE, VS_MUT, VS_RECV, VS_STORE, VS_NKINDS } VsKind;
/* Variable-site chains (compiler.c, see vsite_build): the entries of one
   site kind of the variable named by read kind `kind`
   (NK_LocalVariableReadNode, NK_InstanceVariableReadNode,
   NK_GlobalVariableReadNode, NK_ClassVariableReadNode, NK_ConstantReadNode),
   `name` (a global's resolved) and `key` (a local's scope index, an ivar's
   owning class, -1 for a global, a class variable or a constant), in node
   order: for (e = comp_vsite_first(c, VS_READ, kind, name, key); e >= 0;
   e = comp_vsite_next(c, e)). An entry's site is comp_vsite_node; chains
   carry hash collisions, so check comp_vsite_var, the read or write naming
   the variable. */
int comp_vsite_first(Compiler *c, VsKind k, NodeKind kind, const char *name, int key);
int comp_vsite_next(const Compiler *c, int e);
int comp_vsite_node(const Compiler *c, int e);
int comp_vsite_var(const Compiler *c, int e);
/* The call whose receiver node `n` is (through parentheses), or -1. */
int comp_recv_parent(Compiler *c, int n);
/* The local-variable writes whose value is node n, ascending: for (w =
   comp_lwrite_of_value(c, n); w >= 0; w = comp_lwrite_next(c, w)). */
int comp_lwrite_of_value(Compiler *c, int n);
int comp_lwrite_next(const Compiler *c, int w);
/* Is node n a statement the next statement follows (through parentheses),
   so its value is dropped? (analyze_util.c, on vp_climb) */
int comp_value_dropped(Compiler *c, int n);
int comp_kind_first(Compiler *c, int kind);
int comp_kind_next(const Compiler *c, int id);
int comp_sret_first(Compiler *c, int scope_idx);
int comp_sret_next(const Compiler *c, int r);
int comp_bare_gets_is_argf(Compiler *c);
int    comp_method_index(Compiler *c, const char *name); /* -1 if none */
int    comp_main_method(Compiler *c, const char *name);  /* main's singleton method (`def self.k` at the top level); -1 if none */
/* A receiverless call's target: the enclosing self's ancestry first, a
   top-level def (a private Object method, and so last in every ancestry) only
   as the fallback. See analyze_util.c. */
int    comp_self_call_mi(Compiler *c, int call_node, const char *name);
int    comp_class_self_inst_mi(Compiler *c, int call, int class_id, const char *name);
/* A definition the compiler keeps for its own lowering, which CRuby has no
   method for: a builtins/ file's own helper (is_builtin_helper_name, the
   enumerator's `__enumw_map`) or a desugar's per-call-site copy (marked
   `private_helper` where it is made, enumerable.rb's `__enum_count__0`).
   Only the lowering that names it calls it. comp_method_is_private_helper:
   the method an instance of class `cid` (-1: main) reaches by `m` is one;
   comp_name_is_private_helper: every scope of the name is one, for a list
   that knows no receiver. */
int    comp_scope_is_private_helper(Compiler *c, int si);
int    comp_method_is_private_helper(Compiler *c, int cid, const char *m);
int    comp_name_is_private_helper(Compiler *c, const char *m);
/* Does no Ruby method answer to `m`, though a scope has the name: a
   private helper or another synthesized method? Reflection and a computed
   send's dispatch leave it out. See analyze_util.c. */
int    name_is_synth_method(Compiler *c, const char *m);
int    comp_cbody_call_mi(Compiler *c, int call_node, const char *name);
/* Does the receiver of a retargeted `recv.send(:name)` (send_blind), call
   `call`, answer name itself -- an instance's method or reader, a class
   constant's class method -- rather than through a top-level def? srt is
   recv's type. */
int    send_blind_recv_owns(Compiler *c, int call, int recv, TyKind srt, const char *name);
/* What a dispatch of `name` over cid's subtree answers: r (the base method
   base_mi's answer) unified with the return of every other implementation a
   class in the subtree runs -- its chain's, so a module a subclass includes
   counts -- class methods when cmeth. A yielding one answers call_id's
   block (method_call_ret) when call_id >= 0. Inference's object and
   implicit-self calls and codegen's dispatch switch share it. */
TyKind dispatch_ret_over(Compiler *c, int cid, const char *name, int cmeth, int base_mi, TyKind r,
                         int call_id);
/* 1 iff `node` is a constant path naming an `ffi_const` declaration, with its
   value in *out. Such a name is a VALUE, not a class, wherever the two are
   told apart. */
int    comp_ffi_const_at(Compiler *c, int node, int *out);
int    comp_included_method_index(Compiler *c, const char *name, int call_id);
int    class_is_blank_slate(Compiler *c, int ci);   /* explicit `< BasicObject` descent */
int    class_lacks_kernel(Compiler *c, int ci);     /* blank slate with no `include Kernel` */
int    comp_toplevel_mod_defines(Compiler *c, const char *name, int extend);

/* Locals within a scope. */
LocalVar *scope_local(Scope *s, const char *name);

/* The slot-widening idiom, recorded: unify `t` (the type of `node`'s value)
   into `lv`, answer whether it changed, and note the why when the slot
   degrades. slot_set is the same with the merged type already decided. */
int slot_take(Compiler *c, LocalVar *lv, TyKind t, int node);
int local_range_held(Compiler *c, int node);
int value_may_be_range(Compiler *c, int node);
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
void scope_locals_appended(Scope *s);  /* scope_local's index, after an append */
void scope_locals_removed(Scope *s);   /* ... after a local is taken out of `locals` */

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
int comp_scope_native_twin(Compiler *c, int mi);
int comp_native_method_find(Compiler *c, int class_id, const char *name, int argc, int kind);
int comp_native_answer_class(Compiler *c, int cid, const char *name);
int comp_native_binding_class(Compiler *c, int k, const char *name);
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys);
int comp_poly_arm_defines(Compiler *c, int k, const char *name);
int comp_poly_arm_defines_n(Compiler *c, int k, const char *name, int argc);
TyKind native_spec_to_ty(const char *spec);
TyKind comp_native_ret_ty(Compiler *c, int nmi, int k);
/* IO::Buffer type-symbol table (index-compatible with lib/sp_iobuffer.h) */
int comp_iob_sym_type(const char *name);
int comp_iob_ty_is_float(int t);
int comp_iob_ty_width(int t);
int comp_iob_ty_be(int t);
int comp_iob_ty_signed(int t);
int comp_iob_ty_is_64(int t);

/* Global variables and top-level constants. *_intern finds or creates. */
LocalVar *comp_gvar(Compiler *c, const char *name);
LocalVar *comp_gvar_intern(Compiler *c, const char *name);
const char *comp_resolve_gvar(Compiler *c, const char *name); /* alias resolution */
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to);
/* 1 for $VERBOSE and $DEBUG, the interpreter's flags: false before any write */
int comp_gvar_is_interp_flag(const char *name);
LocalVar *comp_const(Compiler *c, const char *name);
int comp_const_tail_cands(Compiler *c, const char *nm, int **out);
int comp_class_owned_const(Compiler *c, int k, const char *nm, const int *cands, int ncands);
int comp_const_write_valued(const Compiler *c, const LocalVar *cv);
LocalVar *comp_const_intern(Compiler *c, const char *name);
/* 1 when `pred` is a statically-false `defined?(Const)` if-guard (optionally
   the left arm of an `&&` chain) over a constant that resolves to nothing;
   the guarded branch is compile-time dead. */
int comp_defined_guard_false(Compiler *c, int pred);
/* Does constant (path) node v name nothing at compile time, so that
   `defined?(v)` is nil and a read of it raises NameError (a segment no
   program constant, class or builtin resolves)? */
int comp_const_undefined(Compiler *c, int v);
/* the dual: statically-true defined?(Const / Const::Path) guard */
int comp_defined_guard_true(Compiler *c, int pred);
int comp_defined_early(Compiler *c, int pred, const char **early, int cap);
int comp_defined_reads(Compiler *c, int pred, int *reads, int cap);
/* a constant path whose leaf is no member of the class or module its parent
   names (analyze_util.c) */
int const_path_misses_member(Compiler *c, int id);
int const_node_class_index(Compiler *c, int id);
void builtin_given_free(Compiler *c);
int const_path_class(Compiler *c, int id);
/* a segment naming a builtin class, module or constant (analyze_util.c) */
int const_seg_builtin(Compiler *c, int id);

/* Classes. */
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node);
int comp_toplevel_class_make(Compiler *c);
int        comp_class_index(Compiler *c, const char *name);   /* -1 if none */
/* The first-created class for a CORE_* slot, -1 if no class has been made
   or the slot is negative. */
int        comp_core_class(const Compiler *c, int k);   /* CORE_* or a negative slot; -1 if none */
/* The classes whose def_node is node, lowest index first: a builder write
   makes several. -1 if none (or node < 0). */
int        comp_class_of_def(const Compiler *c, int node);
int        comp_class_def_next(const Compiler *c, int k);   /* the next class of k's def_node, -1 */
void       comp_class_set_def(Compiler *c, int k, int node);   /* the one writer after comp_class_new */
int        comp_any_computed_ivar_set(Compiler *c);
int        comp_is_wellknown_const(const char *cn);
int        const_name_resolves_top_level(Compiler *c, const char *cn);
/* Does constant segment node seg (a ConstantReadNode or a path's segment)
   resolve: by its name, as a builtin, or as a program class the path names? */
int        const_seg_resolves(Compiler *c, int seg);
/* The class Ruby sees for `ci`: a synthesized singleton subclass reports its
   parent (CRuby hides the singleton class), every other class reports itself. */
static inline int singleton_visible_ci(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses) return ci;
  return c->classes[ci].is_singleton_of ? c->classes[ci].is_singleton_of - 1 : ci;
}
int        comp_class_anonymous(Compiler *c, int ci);
int        comp_has_sclass_values(Compiler *c);
const char *comp_class_display_fn(Compiler *c);
const char *comp_class_base_display_fn(Compiler *c);
int cls_user_display_mi(Compiler *c, int k, const char *nm);
int prog_has_cls_user_display(Compiler *c, const char *nm);
int        class_var_static_ci(Compiler *c, int node);  /* local holding one class const */
int        class_recv_static_ci(Compiler *c, int node); /* constant or local naming one class */
int        const_class_value_only(Compiler *c, int node); /* constant slot that may hold a class known only at run time */
int        class_recv_user_cmethod(Compiler *c, int call, int node, const char *name); /* call's user class method of the name on it */
int        dynamic_new_may_reach(Compiler *c, int call_id, int cid);  /* k.new can build cid */
int        anon_struct_ci_for_value(Compiler *c, int val);  /* k = Struct.new(...) value node */
const char *struct_call_dup_member(Compiler *c, int callnode);  /* first duplicate member sym name, or NULL */
const char *sym_static_value(Compiler *c, int node);  /* SymbolNode or sole-symbol local */
/* The String in-place mutators, as one table with a per-site mask in
   builtin_ops.c. The demand analysis and the codegen
   re-routes used to keep four near-identical copies of this list; a mutator
   added to one and missed in another is exactly how #3307 / #3333 arrived. */
#define SP_MUT_LOCAL     BOP_MUT_LOCAL      /* seeds local-slot promotion: every mutator */
#define SP_MUT_CONTAINER BOP_MUT_CONTAINER  /* container-read mutation */
#define SP_MUT_IVAR      BOP_MUT_IVAR       /* ivar slot or a reader call (no rename) */
#define SP_MUT_NARROW    BOP_MUT_NARROW     /* guard-narrowed poly re-route: no append_as_bytes */
/* 1 iff `nm` is a String in-place mutator serviceable at every site in `want`. */
int sp_str_mutator(const char *nm, unsigned want);
/* 1 iff call node `id` is a String method whose value is its receiver. */
int str_self_call(const NodeTable *nt, int id);
/* The RegularExpressionNode a Regexp local read at `read` always holds, or -1 (analyze_util.c). */
int an_regex_local_lit(Compiler *c, int read);
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
int ostruct_member_name(const char *n);
int chain_is_lazy_valued(Compiler *c, int node);      /* CallNode chain evaluating to a Lazy */
int poly_str_iter_enum(Compiler *c, int id);        /* boxed String iterator answers an Enumerator (analyze_infer_recv.c) */
int poly_io_codepoint_enum(Compiler *c, int id);   /* boxed each_codepoint takes the IO arm (analyze_infer_recv.c) */
int lazy_alias_chain(Compiler *c, int var_read);
int lazy_method_chain(Compiler *c, int call);      /* parameterless method whose body is a lazy chain -> chain node, else -1 */
int lazy_resolve_chain(Compiler *c, int n);
int local_is_handle(Compiler *c, int a);
int ivar_read_is_handle(Compiler *c, int a);
int        hash_new_default_arg(Compiler *c, int recv); /* Hash.new(d) literal: d node or -1 */
int        recv_hash_new_default_arg(Compiler *c, int recv); /* the same through a local or ivar READ node */
TyKind     hash_default_value_ty(Compiler *c, int dn);      /* the value type a Hash.new(d) default contributes */
int        hash_new_blockless(Compiler *c, int recv);  /* blockless Hash.new / {} literal */
/* The receiver of a const_get family call, as a class id: >= 0 a class of the
   program, CG_OBJECT Object when no class reopens it (the same number as
   comp_core_class's none), CG_NONE no receiver, CG_UNTABLED a named receiver
   the class table lacks (a builtin module, an unknown name). */
enum { CG_OBJECT = -1, CG_NONE = -2, CG_UNTABLED = -3 };
int        const_owned_by_class(Compiler *c, int rk, const char *constname);
int        const_get_recv_class(Compiler *c, int call, int recv);
enum { CS_NONE, CS_SLOT, CS_AMBIG };            /* const_set_slot's answers */
int        const_set_slot(Compiler *c, int call, const char *n, const char **key);
const char *const_get_resolve(Compiler *c, int rk, const char *cgn, int *cls);
void       const_get_leaves_begin(Compiler *c);  /* index the class leaves while the class table holds still */
void       const_get_leaves_end(void);
int        const_defined_known(Compiler *c, int rk, const char *cgn);
int        const_defined_from(Compiler *c, int rk, const char *cgn);   /* ... and seen from R: R, its ancestors, their modules, or top level (#9876) */
int        const_seen_by_class(Compiler *c, int k, const char *cgn);
int        qc_const_qualified(const char *qn);
int        qc_leaf_qualified(const char *leaf);
const char *time_strict_parser_call(Compiler *c, int id);
/* Class index of a `class_eval`/`module_eval { defs }` reopen, else -1.
   enclosing_class resolves bare/`self.` receivers (the class whose body we are
   directly in); ignored for constant receivers. */
int        class_eval_reopen_class(Compiler *c, int id, int enclosing_class);
int        class_reopen_cmethod(Compiler *c, int call, int recv, const char *name); /* a method the program adds to Class, run by `call` (-1: no call) */
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
/* Propagate codegen's setjmp-slot qualifiers through borrowed String calls. */
void       propagate_borrowed_volatile(Compiler *c);
/* Find the instance-method scope index for class_id + method name, or -1. */
int        comp_method_in_class(Compiler *c, int class_id, const char *name);
int        comp_method_overridden(Compiler *c, int cid, const char *name, int cmeth);
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
/* An alias or a class's parent changed: drops what the chain lookups memoized. */
void       comp_class_shape_changed(void);
/* ... and how many times it has: a memo over the chain's answers stamps it. */
unsigned   comp_class_shape_gen(void);
/* Find the class (singleton) method scope for class_id + name, or -1 (no chain). */
int        comp_cmethod_in_class(Compiler *c, int class_id, const char *name);
/* Find the class (singleton) method scope, walking the superclass chain. */
int        comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        class_any_cmethod_eqq(Compiler *c, int ci);
int        class_own_cmethod_eqq(Compiler *c, int ci);
int        class_eqq_answers_value(Compiler *c);
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
const char *io_super_name(const char *k);
/* Like comp_method_in_class but walks the superclass chain. On success,
   *def_class (if non-NULL) is set to the class that defines the method. */
int        comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* The method version recorded at def node def_node: the scope of that def
   named name on class_id's superclass chain (class side when is_cm; a
   top-level method when class_id < 0), the nearest class's when copies
   share the node, under another name when an alias that captured the def
   renamed its scope; *def_class (when asked) the class that holds it. -1
   when no scope there has that def node. */
int        comp_method_version(Compiler *c, int class_id, const char *name, int is_cm, int def_node, int *def_class);
/* The classes whose chain answers (name, is_cm) -- comp_method_in_chain's
   or comp_cmethod_in_chain's -- ascending, two ints per class (the class,
   the scope its chain finds), *n their count. NULL with *n -1 while the
   chain memo cannot be trusted (the index unfrozen, a scope moved): the
   caller then asks class by class. A pass asking whether any class's
   method of a name answers so-and-so walked every class and hashed the
   name for each; at every boxed comparison site, every round, that was a
   third of a Rails program's re-narrow round (1,299 classes). */
const int *comp_name_responders(Compiler *c, const char *name, int is_cm, int *n);
/* Whether some class's chain answers `name` with a reader
   (comp_reader_in_chain), kept per name while the reader and alias tables
   and the class shape stand still. A retargeted send on a boxed receiver
   asked it class by class at every resolution of its plan. */
int        comp_name_reader_any(Compiler *c, const char *name);
/* The classes whose own chain hop defines a reader of `name` (the ones
   comp_reader_in_chain answers with def_class the class itself), ascending,
   *n of them; kept as comp_name_reader_any's answers are. The kinds walk
   asked it class by class for every argument-less call on a receiver that
   can be anything, at every solve. */
const int *comp_name_reader_owners(Compiler *c, const char *name, int *n);
/* does class_id's chain define the method call id names, and does the call reach it? A call
   marked builtin_only (the super of a program's own reflection, rewritten to
   self's builtin) does not. */
static inline int comp_object_defines(Compiler *c, const char *name) {
  int oc = comp_core_class(c, CORE_OBJECT);
  return oc >= 0 && comp_method_in_chain(c, oc, name, NULL) >= 0;
}
/* does Object's program method of the name answer call id for the receivers no class arm of its
   dispatch took (builtin values, instances of classes without a method of their own)? */
static inline int comp_call_reaches_own_any(Compiler *c, int id, const char *name) {
  return !nt_int(c->nt, id, "builtin_only", 0) && comp_object_defines(c, name);
}
static inline int comp_call_reaches_own(Compiler *c, int id, int class_id, const char *name) {
  if (nt_int(c->nt, id, "builtin_only", 0)) return 0;
  return comp_method_in_chain(c, class_id, name, NULL) >= 0 || comp_object_defines(c, name);
}
int        send_def_just_supers(Compiler *c, int mi);          /* analyze_desugar.c */
int        send_family_program_owns(Compiler *c, const char *name);
/* Array subclasses (#7449, ClassInfo.ary_root): the root of class cid's
   Array chain or -1; the same for an object type (-1 for any other type);
   the kind of the Array the chain's instances embed, TY_POLY_ARRAY once the
   inference is past its optimistic stage with no element seen. */
int        comp_ary_root(Compiler *c, int cid);
int        comp_implicit_to_ary_mi(Compiler *c, int cid);
int        comp_ty_ary_root(Compiler *c, TyKind t);
int        comp_ty_arysub_array(Compiler *c, TyKind t);
const int *comp_strsub_own_classes(Compiler *c, const char *name, int *n);
TyKind     comp_ary_kind(Compiler *c, int cid);
/* Whether Array answers a call named n on an instance of Array subclass
   cid: no method, reader or writer of the class chain takes the name, it
   asks nothing about the object itself, and Array has it. */
int        comp_arysub_name_is_array(Compiler *c, int cid, const char *n);
/* Call `id` on rt (an Array subclass instance) is Array's, answered as the
   embedded Array's kind *kind; what Array answers, as the builtin-op rows
   say (bop_answers_self: BOPF_SELF, BOPF_SELF_OR_NIL, BOPF_SELF_EXACT...);
   whether that answer is the receiver itself (BOPF_SELF, or
   BOPF_SELF_OR_NIL where it can be nil); and whether the call reads an
   Array argument as an Array (BOPF_ARGS_BUILTIN). */
int        comp_arysub_call(Compiler *c, int id, TyKind rt, TyKind *kind);
int        comp_arysub_answer(Compiler *c, int id, int hash);
int        comp_arysub_self_result(Compiler *c, int id, int hash);
int        comp_arysub_args_viewed(Compiler *c, int id, TyKind rt);
int        comp_arysub_kernel_array(Compiler *c, int id);
int        comp_array_method_name(const char *n);
/* the class chain of cid embeds a Hash, not an Array */
int        comp_ary_is_hash(Compiler *c, int cid);
int        comp_ary_is_str(Compiler *c, int cid);
int        comp_arysub_emb(Compiler *c, int cid);
int        comp_arysub_str_boxed(const char *n);
int        comp_arysub_builtin(Compiler *c, int cid);
/* a name the builtin cid's chain embeds answers (comp_array_method_name, or Hash's) */
int        comp_arysub_builtin_name(Compiler *c, int cid, const char *n);
int        comp_arysub_reopen(Compiler *c, int cid);
int        comp_builtin_kind_reopen_mi(Compiler *c, TyKind t, const char *name);
int        comp_builtin_name_reopened(Compiler *c, const char *name);
int        comp_yield_chain_reopened(Compiler *c, int call);
/* Record method `name`'s visibility on a class (overwrite-or-append). */
void       comp_method_vis_set(ClassInfo *ci, const char *name, int kind);
/* Record class method `name`'s visibility on a class (overwrite-or-append). */
void       comp_cmethod_vis_set(ClassInfo *ci, const char *name, int kind);
/* The same for a class method an `extend` copies in; a later extend overwrites. */
void       comp_cmethod_extend_vis_set(ClassInfo *ci, const char *name, int kind);
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
/* The classes node's value can hold, or NULL for every class (a node the
   pass did not see, a value it cannot bound). A class the set leaves out
   is one no instance of which reaches the node (analyze_cls.c, #8530). */
const ClsSet *cls_fact_node(Compiler *c, int node);
/* Is class k one the fact bounds: a class the program defines, whose
   instances the pass follows from their construction? A reopened builtin's,
   a native class's and a module's are not. */
int cls_fact_bounded(Compiler *c, int k);
/* Does the fact rule class k out as the class of recv's value: the set is
   known, k is bounded, and k is not in it? A call on recv then never
   reaches k's method, so the binding and the dispatch may leave it out. */
int cls_fact_excludes(Compiler *c, int recv, int k);
/* May node's value be an OpenStruct (an OpenStruct.new reaches it, or the
   walk has no answer for it), in a program that requires "ostruct"? */
int cls_fact_may_ostruct(Compiler *c, int node);
/* the fact's own index for recv's set, which names the set uniquely: 0 or -1
   for every class (nothing to filter by) */
int cls_fact_idx(Compiler *c, int node);
/* node, made after the pass, holds the value node `as` holds (a desugar's
   read of a temporary it stored as into): it takes as's fact */
void cls_fact_same(Compiler *c, int node, int as);
/* Does node's fact say nothing of its classes: every class, or the closed
   world's value from anywhere (every class, Class object and builtin)? */
int cls_fact_unbounded(Compiler *c, int node);
/* What else than an object of a class of the program's node's value can be,
   as the same walk follows it (analyze_cls.c), in a program that calls
   compare_by_identity: CK_UNSAFE a value whose identity is not CRuby's here
   or one the walk cannot name (a number, a Range, a builtin's answer, a
   value from code it does not see), CK_LIT a String literal (one object
   per text here), CK_STR a String made afresh (an interpolation, a dup, a
   unary plus, String.new). A Symbol, nil, true, false, a container literal
   or a Proc adds nothing. cls_fact_kinds answers for the value,
   cls_fact_contents_kinds for the elements of the containers it holds;
   CK_UNSAFE for a node the walk has no answer for. */
enum { CK_UNSAFE = 1, CK_LIT = 2, CK_STR = 4, CK_CONTENTS = 3 };
void an_cls_kinds(Compiler *c);   /* the walk, once: before the first ask */
int cls_fact_kinds(Compiler *c, int node);
int cls_fact_contents_kinds(Compiler *c, int node);

typedef struct { int cls; int mi; int rdcls; int native; } PolyCand;
const PolyCand *comp_poly_candidates(Compiler *c, const char *name, int *n);
/* the same for the call `id`, less the classes its receiver never holds
   (cls_fact_excludes, #8530): what its arms are counted, typed and emitted from */
const PolyCand *comp_poly_candidates_at(Compiler *c, int id, const char *name, int *n);
extern unsigned comp_table_gen;
void comp_poly_candidates_reset(void);
/* The classes answering class method `name` through their chain, ascending,
   each with the scope comp_cmethod_in_chain gives (`cls`, `mi`; the other
   fields unused). Memoized under the same stamps as comp_poly_candidates. */
const PolyCand *comp_cmethod_candidates(Compiler *c, const char *name, int *n);
/* Can class `k`'s class object be a value: held by a variable, a box or a
   container, so that a call on a receiver of no fixed class may reach its
   class methods? Not when the program only ever names it to call it, define
   it or test against it (see compiler.c). */
int comp_class_may_be_value(Compiler *c, int k);
/* Every proper descendant of class `cid`, ascending; fixed once classes are
   collected. See comp_descendants. */
const int *comp_descendants(Compiler *c, int cid, int *n);
void comp_descendants_reset(void);
/* Walk the chain for an attr reader/writer; returns 1 and the owning class. */
int        comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* A strict subclass answers `name` (a method or a reader in its chain). */
int        comp_subclass_answers(Compiler *c, int class_id, const char *name);
void       comp_add_reader(ClassInfo *ci, const char *name);
void       comp_add_writer(ClassInfo *ci, const char *name);
int        comp_is_reader(ClassInfo *ci, const char *name);
void       comp_add_own_reader(ClassInfo *ci, const char *name);
int        comp_is_own_reader(ClassInfo *ci, const char *name);
/* Whether a call named `name` on an instance of class cid, which has an
   #each, is Enumerable's: no method or reader of the program's comes before
   Enumerable in Ruby's lookup order. 0 when the program's answers first. */
int        comp_enum_answers_first(Compiler *c, int cid, const char *name);
int        comp_comparable_answers_first(Compiler *c, int cid, const char *name);
int        comp_comparable_reaches(Compiler *c, int cid);
int        comp_enum_reaches(Compiler *c, int cid);   /* Enumerable among its ancestors */
int        class_builtin_parent(Compiler *c, int cid);   /* codegen.c: the builtin class above the user chain of the class */
int        class_is_exc_subclass(Compiler *c, int ci);   /* analyze.c */
int        comp_plain_class(Compiler *c, int cid);    /* only Object's and Kernel's builtin methods behind the class: no core class or core module among its ancestors */
int        comp_enum_behind(Compiler *c, int cid);    /* Enumerable's methods behind the class: Enumerable, or an Array, Struct or native ancestor */
int        core_method_vis(const char *cls, const char *m, int inherit);   /* codegen_call_class.c */
int        comp_is_writer(ClassInfo *ci, const char *name);
int        name_is_plain_setter(const char *name);
int        call_is_setter_assign(const NodeTable *nt, int id);
int        proc_parameters_lambda_mode(const NodeTable *nt, int argc, const int *argv);
int        self_is_main(Compiler *c, int node);
int        self_is_main_raw(Compiler *c, int node);
int        self_class_body(Compiler *c, int node);
int        attr_decl_call(Compiler *c, int id);
enum { VIS_DECL_NIL = 1, VIS_DECL_SYM, VIS_DECL_ARRAY, VIS_DECL_SELF };
int        vis_decl_call(Compiler *c, int id);
int        setter_base_name(const char *name, char *out, size_t cap);
char      *name_dup(const char *name);
char      *setter_name_of(const char *attr);
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
void       comp_add_sg_inh(ClassInfo *ci, const char *name);
void       comp_add_sg_decl(ClassInfo *ci, const char *base, int writer, int node);
/* Prepend-chain helpers. */
void        comp_prep_chain_add(ClassInfo *ci, const char *from, const char *to);
const char *comp_prep_chain_target(Compiler *c, int class_id, const char *name);
/* The shadow a `super` in scope s reaches through the prep chain, or NULL. A
   class method's chain is keyed `self.<name>`, apart from the instance
   method of the same name. */
const char *comp_super_shadow(Compiler *c, const Scope *s);
void comp_cprep_chain_add(ClassInfo *ci, const char *from, const char *to);
/* The hidden copies (Scope.shadow): the name a new `__prep <n> <name>` copy of
   class ci's gets, set on the scope where the copy is made, carried by the
   clones, and read back as the name the program wrote. */
void comp_shadow_name(Compiler *c, int ci, const char *name, char *out, size_t size);
void comp_scope_set_shadow(Compiler *c, int si, int kind, const char *user);
void comp_scope_copy_shadow(Compiler *c, int dst, int src, const char *suffix);
const char *comp_scope_user_name(const Scope *s);
int comp_name_is_shadow_copy(Compiler *c, const char *name, int kind);
const char *comp_super_name(Compiler *c, int parent, const char *name, int is_cmethod);
const char *comp_scope_def_name(Compiler *c, const Scope *s);
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
const char *comp_resolve_cm_alias_at(Compiler *c, int class_id, const char *name, int *start_cls);
int comp_sg_writer_reached(Compiler *c, int k, const char *name, char *base, size_t cap);
/* Does class d declare the `class << self` accessor `name` (`x`, or `x=`)
   itself, rather than inherit it? */
int comp_sg_accessor_own(Compiler *c, int d, const char *name);
/* The node of class d's own latest declaration of that accessor, or -1 */
int comp_sg_accessor_decl_node(Compiler *c, int d, const char *name);
void rewrite_builtin_cm_alias_call(Compiler *c, int id);   /* analyze.c */
const char *comp_resolve_cm_alias_ex(Compiler *c, int class_id, const char *name, int *builtin);
int comp_name_is_sg_alias_only(Compiler *c, int class_id, const char *name);
const char *comp_resolve_alias_ex(Compiler *c, int class_id, const char *name, int *start_cls, int *builtin);

/* Set by codegen while a block is spliced: answers the type the block being
   inlined RIGHT HERE gives a yield, which the node cache cannot hold (one
   YieldNode, one entry, many call sites -- #3784). Returns 0 when the node is
   not a yield in a spliced context. */
extern int (*sp_yield_site_type_hook)(const Compiler *c, int id, TyKind *out);

/* A node's type: see comp_ty_edits. The node's slots belong to its scope. */
static inline void comp_touch_node(Compiler *c, int id) {
  comp_touch_scope(id >= 0 && id < c->node_cap && c->nscope ? c->nscope[id] : -1);
}
/* The owner of a slot picked through a pointer (compiler.c): an ivar's or a
   class variable's class, a local's scope, the program (-1) for a global or
   a constant, and COMP_REF_NONE for a pointer in no table: a value of the
   caller's own, which no inference reads, so its write moves no owner. */
#define COMP_REF_NONE (-0x7fffffff)   /* (below every class key) */
int comp_ref_owner(Compiler *c, TyRef r);
static inline TyKind ty_ref_set(Compiler *c, TyRef r, TyKind t) {
  if (*r != t) { *(TyKind *)r = t; int o = comp_ref_owner(c, r); if (o != COMP_REF_NONE) comp_touch_key(o); }
  return t;
}
static inline TyKind comp_set_ntype(Compiler *c, int id, TyKind t) {
  if (c->ntype[id] != t) { ((TyKind *)c->ntype)[id] = t; comp_touch_node(c, id); }
  return t;
}
static inline char comp_set_empty_arr_recv(Compiler *c, int id, char v) {
  if (c->empty_arr_recv[id] != v) { ((char *)c->empty_arr_recv)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline char comp_set_empty_hash_recv(Compiler *c, int id, char v) {
  if (c->empty_hash_recv[id] != v) { ((char *)c->empty_hash_recv)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline char comp_set_empty_hash_arg(Compiler *c, int id, char v) {
  if (c->empty_hash_arg[id] != v) { ((char *)c->empty_hash_arg)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline TyKind comp_set_hash_want(Compiler *c, int id, TyKind v) {
  if (c->hash_want[id] != v) { ((TyKind *)c->hash_want)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline TyKind comp_set_arr_want(Compiler *c, int id, TyKind v) {
  if (c->arr_want[id] != v) { ((TyKind *)c->arr_want)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline TyKind comp_set_poly_builtin_ty(Compiler *c, int id, TyKind v) {
  if (c->poly_builtin_ty[id] != v) { ((TyKind *)c->poly_builtin_ty)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline unsigned char comp_set_strbuf_box(Compiler *c, int id, unsigned char v) {
  if (c->strbuf_box[id] != v) { ((unsigned char *)c->strbuf_box)[id] = v; comp_touch_node(c, id); }
  return v;
}
static inline TyKind comp_set_nilnarrow(Compiler *c, int id, TyKind t) {
  if (c->nilnarrow[id] != t) { ((TyKind *)c->nilnarrow)[id] = t; comp_touch_node(c, id); }
  return t;
}

/* Swap a node's cached type for the duration of one re-entered emission and
   answer the old one, so the caller can restore it. `x&.pred?` is inferred
   poly (its nil arm needs somewhere to live) while the value arm still
   renders a C bool: the safe-navigation emitter puts the natural type back
   while it emits that arm, because dispatches downstream read the cache to
   type their own temps (#4070). */
static inline TyKind comp_sn_retype(Compiler *c, int id, TyKind t) {
  if (id < 0 || id >= c->nt->count) return t;
  TyKind old = c->ntype[id];
  comp_set_ntype(c, id, t);
  return old;
}

/* repr.c: the representation decisions the inline readers below defer to */
TyKind repr_stored_type(const Compiler *c, int id, TyKind t);
int repr_value_obj(const Compiler *c, TyKind t);

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  /* (the node's type is its scope's slot: noted for infer_type's memo) */
  if (imemo_dep_on) imemo_dep_scope(id < c->node_cap && c->nscope ? c->nscope[id] : -1);
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
  /* the String-handle refinement is repr.c's (repr_stored_type) */
  if (t == TY_STRBUF || c->strbuf_handle_demand[id]) return repr_stored_type(c, id, t);
  return t;
}

/* 1 iff t is a user-object type whose class is represented by value (sp_X,
   not a heap pointer). See detect_value_types / reference_legacy_value_type_logic. */
static inline int comp_ty_value_obj(const Compiler *c, TyKind t) {
  return repr_value_obj(c, t);   /* repr.c */
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
int self_class_base_ci(Compiler *c, int recv);    /* self.class: the instance method's own class */
int allocate_on_own_class(Compiler *c, int id);    /* allocate on the method's own class */

/* An ivar whose type came from an --rbs seed (class_pin_ivar). Codegen reads
   it to decide where a seed assertion belongs (#3412). */
int class_ivar_pinned(ClassInfo *ci, const char *name);

#endif
