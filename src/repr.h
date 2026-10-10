/* repr.h -- a value's C representation, read off the analysis (#7100 Phase E).

   The type of a node or a local says what Ruby value it holds; how that
   value is laid out in C is decided by a handful of flags the analysis sets
   beside the type: a shared-mutable String read as its handle, a nilable
   Integer holding its nil beside the value, a value-type object held by
   value, a read narrowed past a nil guard. repr_of gathers those flags into
   one answer, so the boxers and the readers can ask one question instead of
   each consulting the flags they know about.

   R0: the answer is computed purely from the live flags and nothing reads
   it yet. */
#ifndef SPINEL_REPR_H
#define SPINEL_REPR_H

#include "compiler.h"

typedef enum {
  RK_NONE,      /* no value: unknown or void */
  RK_SCALAR,    /* an immediate: Integer, Float, true/false, Symbol, nil */
  RK_OPT,       /* an Integer or Float that can hold nil beside its value: an
                   sp_oint / sp_ofloat, or an ivar with a byte in iv__nilb */
  RK_STRUCT,    /* a by-value builtin struct: Range, Time, Complex, Rational,
                   Process::Tms, a Class */
  RK_VOBJ,      /* a user object of a value-type class, held by value */
  RK_PTR,       /* a heap pointer, NULL for nil */
  RK_STRBUF,    /* a shared-mutable String: the sp_String * handle */
  RK_BOXED      /* an sp_RbVal */
} ReprKind;

typedef struct {
  TyKind ty;              /* the node's or slot's type as inferred */
  TyKind as_ty;           /* the type it is stored as (comp_ntype for a node:
                             the handle under a strbuf mark or demand) */
  TyKind narrowed;        /* a read narrowed past a nil guard: its non-nil
                             type, or TY_UNKNOWN */
  TyKind elem;            /* an Array: the type its C container holds each
                             element as (an IntArray's Integer, a PolyArray's
                             box, an object array's class); else TY_UNKNOWN */
  TyKind key, val;        /* a Hash: the types its C table holds the keys
                             and the values as (a StrPolyHash's String and
                             box); else TY_UNKNOWN */
  TyKind range;           /* a Range: the type its C struct holds its bounds
                             as (an sp_Range's Integer, a Float end kept
                             beside them; an sp_FloatRange's Float; an
                             sp_StrRange's String); else TY_UNKNOWN */
  unsigned char kind;     /* ReprKind */
  unsigned may_nil:1;     /* the value can be nil in this representation:
                             an Integer or Float that holds its nil (an
                             oint, a nil byte); a user object or a
                             String, Array, Hash or IO the nil fact
                             (analyze_nil.c, #7444) says may be; any other
                             pointer whose NULL is its nil */
  unsigned handle:1;      /* a read that yields the shared String handle */
  unsigned demand:1;      /* stored as the handle without moving the type */
  unsigned read_raw:1;    /* a handle read whose consumer only reads bytes */
  unsigned poly_lift:1;   /* a poly read lifted to the shared handle */
  unsigned dyn_cls:1;     /* an object of a class with subclasses: its box
                             reads the class id from the object */
  unsigned nil_scalar:1;  /* an Integer or Float whose box reads its nil
                             (sp_box_oint / sp_box_ofloat) */
  unsigned nil_tested:1;  /* a call's nil arm has tested this receiver for
                             nil (VR_NIL_TESTED, a view around the call) */
  unsigned nil_cold:1;    /* ... in the out-of-range branch of a cached
                             array read, which writes the test
                             (VR_NIL_TESTED 2) */
  unsigned head_held:1;   /* a call's nil arm head ran this operand into a
                             temp of its own ahead of the call (VR_HEAD_HELD,
                             a view around the call): it reads as that temp */
  unsigned big:1;         /* an Integer held as an sp_Bigint * */
  unsigned elems_handle:1; /* a container slot whose String elements are
                              boxed shared handles (--share-strings) */
  unsigned share:1;       /* the shared String handle the --share-strings rule
                             assigned (#6765): a slot that is the handle
                             (repr_of_slot, repr_of_ivar, repr_of_cvar), or a
                             read or write of a global, a constant or a class
                             variable that is one (repr_of). repr_of answers
                             it for those nodes only: a local's read answers
                             0, whatever its slot holds (ask repr_of_slot).
                             Never set without the rule: a slot that is
                             master's own shared-mutable handle (#3227) is
                             `handle`, and any sp_String * slot is kind
                             RK_STRBUF */
  unsigned untyped:1;     /* meaningful only with RK_NONE: no type was
                             inferred (TY_UNKNOWN, or no node or slot at
                             all), as against a value-less void. A flag
                             beside the kind, not a kind of its own, so no
                             switch on the kind changes */
  unsigned elem_nil_marked:1; /* an Array the analysis saw a nil stored
                             into. An Integer or Float one
                             (nullable_int_elem): its elements can be nil
                             (their nil bits), so a loop's cached read of
                             it takes the element with its nil; an unmarked
                             one can still hold one, which its nil bitmap
                             answers for. A pointer one (an
                             object's, a String's, an Array's: the nil
                             fact's obj_elem_may_nil): a nil stored, or a
                             gap a write past the end leaves; an element
                             read out of it or a block parameter bound from
                             it may be NULL (NFW_ELEM_NIL), and a call on
                             one tests it. An unmarked one is taken to hold
                             none */
  unsigned arr_or_nil:1;  /* a boxed local proven to hold only a PolyArray
                             or nil (arr_or_nil), and a read of it: an index
                             read takes the runtime's inline array arm,
                             which neither allocates nor needs a root */
  unsigned volatile_str:1; /* a String local live across a setjmp
                             (borrowed_volatile): its C slot, and a slot
                             that borrows it, is `const char * volatile` */
  unsigned char cell;     /* ReprCell: where a local's value lives, for the
                             slot and for a read of it */
  unsigned char strbuf_src; /* ReprStrSrc: where a shared String's box comes
                               from */
} Repr;

/* Where a shared-mutable String's boxed form comes from. */
typedef enum {
  RS_NONE,
  RS_HANDLE,     /* a variable's own handle: a local, an ivar, an ivar write */
  RS_DEMANDED,   /* a call that renders the handle itself (a reader, a call
                    answering its receiver) under a handle mark or demand */
  RS_FRESH,      /* a String value wrapped in a fresh handle where one is
                    demanded */
  RS_ELEM,       /* an element a boxed container hands out, already a box */
  RS_SLOT_POLY   /* a handle-marked read of a slot that settled poly */
} ReprStrSrc;

/* Where a local's value lives. */
typedef enum {
  RC_NONE,       /* its own C variable, lv_<name> */
  RC_HEAP,       /* is_cell: a heap cell an escaping proc shares with the
                    scope, read and written through *_cell_<name> */
  RC_BYREF,      /* byref_out: a String parameter the method mutates in
                    place, the caller's own slot passed as const char ** */
  RC_ALIAS       /* inline_alias: a parameter an inline expansion binds to
                    the caller's variable for its duration */
} ReprCell;

/* Does a call's settled return route hand back a shared String handle? */
int repr_call_returns_handle(Compiler *c, int v);
/* A builtin receiver conversion's String operand, or -1. */
int repr_string_conversion_operand(Compiler *c, int v);
/* An ENV store's value operand whose identity the result can keep, or -1. */
int repr_env_store_operand(Compiler *c, int v);
/* A boxed to_s that keeps its String receiver beside fresh user returns. */
int repr_boxed_to_s_operand(Compiler *c, int v);
/* A boxed call whose reader arms all hold shared String handles. */
int repr_boxed_reader_handle(Compiler *c, int v);
/* The representation of node `node`'s value. */
Repr repr_of(const Compiler *c, int node);
int repr_self_handle(const Compiler *c, int scope);
int repr_self_shared(const Compiler *c, int node);
/* The representation of a local variable's slot (a global's and a
   constant's LocalVar too). */
Repr repr_of_slot(const Compiler *c, const LocalVar *lv);
/* The representation of class cid's ivar slot iv, of its class variable
   slot idx, and of method scope sc's value. */
Repr repr_of_ivar(const Compiler *c, int cid, int iv);
Repr repr_of_cvar(const Compiler *c, int cid, int idx);
Repr repr_of_ret(const Compiler *c, const Scope *sc);
/* repr_of_slot(c, lv).kind and repr_of_cvar(c, cid, idx).kind alone,
   without the rest (dyn_cls scans the classes): what inference asks of a
   global's, a constant's or a class variable's slot on every read. */
ReprKind repr_slot_kind(const Compiler *c, const LocalVar *lv);
ReprKind repr_cvar_kind(const Compiler *c, int cid, int idx);
/* Is r a Hash that holds its keys as `key` and its values as `val`? */
int repr_hash_is(Repr r, TyKind key, TyKind val);
/* Called once the analysis is final (the end of analyze_program): from here
   on the flags repr_of reads no longer change. */
void repr_seal(Compiler *c);
/* Whether an Integer or Float node's box has to read its nil (emit_boxed's
   sp_box_oint / sp_box_ofloat): the node has an sp_oint form of its own. */
int repr_nil_scalar(const Compiler *c, int node, TyKind t);
/* Does a user object of kind t box with the class id it carries
   (sp_box_nullable_obj_dyn)? Its class has a subclass, and it is neither a
   value type nor an exception, whose object starts with its class name. */
int repr_dyn_cls(const Compiler *c, TyKind t);
/* the flag readers that decide it (old names: box_nullable_arg, the local
   arm of call_returns_nullable_int) */
int repr_box_nullable_arg(Compiler *c, int v);
int repr_local_nullable_int(Compiler *c, int node);
/* Does a container of type t hold its Strings as `const char *` (a typed
   String Array or a Hash with String values), so none can be the shared
   handle? */
int repr_typed_str_container(TyKind t);
/* A retained typed literal whose String elements need shared handles. */
int repr_str_literal_shares(Compiler *c, int node);
/* Whether repr_seal has run for the current compile. */
int repr_sealed(void);
/* Does the share rule decide which Strings are the shared handle
   (--share-strings, #6765)? Codegen asks this, not the flag: where it is
   0, every emitter takes master's form. */
int repr_share_rule(const Compiler *c);

/* R1 (--repr-check): the form a boxer gave a value, recorded at each of
   emit_boxed's and emit_boxed_text's returns, and the form repr_of predicts
   for it. */
typedef enum {
  RF_PASS,          /* already an sp_RbVal */
  RF_NIL_EFFECT,    /* evaluated for its effect, then nil */
  RF_INT,           /* sp_box_int */
  RF_INT_NIL,       /* an Integer boxed with its nil: sp_box_oint */
  RF_FLT,           /* sp_box_float */
  RF_FLT_NIL,       /* a Float boxed with its nil: sp_box_ofloat */
  RF_BIGINT,        /* a Bignum, NULL as nil */
  RF_STR,           /* sp_box_str */
  RF_BOOL,
  RF_SYM,
  RF_STRUCT,        /* a by-value struct: a Range, Time, Complex, Rational,
                       Process::Tms, Class */
  RF_NULLABLE,      /* a pointer, NULL as nil, with its static class id */
  RF_NULLABLE_DYN,  /* a pointer whose class id is read from the object */
  RF_VOBJ,          /* a value-type object, boxed by copy */
  RF_STRBUF_HANDLE, /* the shared String's existing handle */
  RF_STRBUF_FRESH,  /* a fresh handle around a String value */
  RF_STRBUF_ELEM,   /* an element read that is already a boxed handle */
  RF_PTR_ARRAY,     /* a nested table or object array, stamped */
  RF_YIELD,         /* a yield lowered to a proc call answering boxed */
  RF_SPECIAL,       /* a shape-specific box: a splat, an empty literal,
                       Hash.new, a Regexp, a refusal */
  RF__COUNT
} ReprForm;

/* the form a value of representation r is boxed in */
ReprForm repr_box_form(const Compiler *c, Repr r);
const char *repr_form_name(int form);
/* --repr-check is on */
extern int g_repr_check;
/* --repr-check: ask repr_of of a node codegen is about to emit, whose
   answer is dropped; the C must not change (repr_of changes nothing) */
void repr_check_ask(const Compiler *c, int node);
/* The settled return walk's leaf decisions and the emitter's observations.
   All callers guard these with g_repr_check; no shadow exists otherwise. */
enum { RCH_NONE, RCH_PUBLISH, RCH_CLEAR, RCH_NIL, RCH_HANDLE, RCH_BYTES, RCH_BOXED };
void repr_channel_predict(Compiler *c, int node, int mi, int form);
void repr_channel_clearing(Compiler *c, int delta);
int repr_channel_begin(Compiler *c, int node);
int repr_channel_ensure(Compiler *c, int node);
void repr_channel_note(Compiler *c, int node, int form);
void repr_channel_end(Compiler *c, int frame);
void repr_channel_boxed(Compiler *c, int node, int frame);
void repr_channel_call(Compiler *c, int node, int mi);
void repr_channel_pickup(Compiler *c, int node, int nil_guard);
void repr_channel_report(Compiler *c);
void repr_channel_free(Compiler *c);
/* --dump-repr is on (#7501) */
extern int g_dump_repr;
/* --dump-repr: each slot's representation, one sorted line per slot, as
   the final analysis gives it (malloc'd text) */
char *repr_dump(const Compiler *c);

/* ---- Stores (R6) ----
   The C value class of a kind, what C allows between two of them: a store
   of one class into a slot of another converts, or does not fit. */
enum { SC_NONE, SC_ARITH, SC_PTR, SC_STRUCT, SC_BOXED };
int repr_store_class(const Compiler *c, TyKind t);
/* Does a value of kind `from`, written as it is, keep its value in a slot
   of kind `to` (store_fits)? */
int repr_store_fits(Compiler *c, TyKind from, TyKind to);
/* A nil literal written as it is into a slot of kind `slot` (store_nil_fits) */
int repr_store_nil_fits(Compiler *c, int node, TyKind slot, int how);

/* The form emit_coerce stores a value into a typed slot in. */
typedef enum {
  CF_FIT,           /* written as it is */
  CF_BOX,           /* boxed into a poly slot */
  CF_EMPTY_LIT,     /* an empty [] / {} / Array.new / Hash.new built at the slot's kind */
  CF_NIL_SENT,      /* the slot's nil (sp_oint_nil(), NULL, a boxed nil): a nil
                       literal, or a value with no C type evaluated for its
                       effect */
  CF_INT2BIG,       /* an Integer widened into a Bignum slot */
  CF_POLY_RHS,      /* a boxed value through its scalar conversion
                       (emit_poly_rhs_coerced) */
  CF_CHECKED_UNBOX, /* a boxed value through the checked unbox */
  CF_STRBUF_HANDLE, /* --share-strings: a String into a shared String handle
                       slot, as the handle it is or a fresh one
                       (emit_strbuf_ivar_store) */
  CF_CONVERT,       /* a conversion Ruby makes itself: truthiness, a Bignum
                       or Rational to a Float */
  CF_REFUSE,        /* no conversion keeps the value: refused */
  CF__COUNT
} CoerceForm;
/* the form emit_coerce stores `node` into a `slot` slot in (how: CO_HOLD
   or CO_CONVERT) */
int repr_coerce_form(Compiler *c, int node, TyKind slot, int how);
/* the same, with the kind of the value as stored (store_value_kind) through
   from_out: emit_coerce's plan */
int repr_coerce_plan(Compiler *c, int node, TyKind slot, int how, TyKind *from_out);
/* the form emit_coerce_text stores an already-rendered `from` value in */
int repr_coerce_text_form(Compiler *c, int node, TyKind from, TyKind slot, int how);
const char *repr_coerce_form_name(int form);

/* ---- --share-strings (#6765) ----
   The one rule: under the flag, a String holder (share.h) is the shared
   handle unless the analysis proves it local. Proven local: no in-place
   mutation reaches its class, or the class has this one holder and every
   mutation goes through it, so the new pointer can be written back into
   that slot. The analysis applies the answer to the flags repr_of reads
   (share_default_apply); codegen follows repr_of. */
int repr_str_shares(const Compiler *c, int holder);
/* the same rule for the elements of holder h's containers */
int repr_str_elems_share(const Compiler *c, int holder);
/* the rule over a class's facts (SHF_*, the count of its holders) */
int repr_str_class_shares(unsigned flags, int holders);
/* repr_of(c, node).share alone, without the rest of repr_of: whether a
   read or write node of a global, a constant or a class variable names a
   slot that holds the handle the rule assigned. The analysis's loops and
   an emitter that needs only this bit ask it. */
int repr_static_share(const Compiler *c, int node);
/* Does write node `node` -- a local's, an ivar's, a global's or a class
   variable's `=`, `||=` or `&&=` -- store into a slot that holds the
   handle the rule assigned (that slot's Repr's `share`)? In value position
   its value is then the handle, as the slot's read is (repr_of:
   RS_HANDLE). */
int repr_write_share(const Compiler *c, int node);
/* a read such a slot can be: a global's, a constant's (bare or `A::B`), a
   class variable's */
int repr_static_read_kind(NodeKind k);

#endif
