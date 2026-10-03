/* repr.h -- a value's C representation, read off the analysis (#7100 Phase E).

   The type of a node or a local says what Ruby value it holds; how that
   value is laid out in C is decided by a handful of flags the analysis sets
   beside the type: a shared-mutable String read as its handle, a nilable
   Integer carried in its sentinel, a value-type object held by value, a
   read narrowed past a nil guard. repr_of gathers those flags into one
   answer, so the boxers and the readers can ask one question instead of
   each consulting the flags they know about.

   R0: the answer is computed purely from the live flags and nothing reads
   it yet. */
#ifndef SPINEL_REPR_H
#define SPINEL_REPR_H

#include "compiler.h"

typedef enum {
  RK_NONE,      /* no value: unknown or void */
  RK_SCALAR,    /* an immediate: Integer, Float, true/false, Symbol, nil */
  RK_SENTINEL,  /* an Integer or Float slot that can hold nil as its sentinel */
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
  unsigned char kind;     /* ReprKind */
  unsigned may_nil:1;     /* the value can be nil in this representation */
  unsigned handle:1;      /* a read that yields the shared String handle */
  unsigned demand:1;      /* stored as the handle without moving the type */
  unsigned read_raw:1;    /* a handle read whose consumer only reads bytes */
  unsigned poly_lift:1;   /* a poly read lifted to the shared handle */
  unsigned dyn_cls:1;     /* an object of a class with subclasses: its box
                             reads the class id from the object */
  unsigned nil_scalar:1;  /* an Integer or Float whose box tests for the nil
                             sentinel */
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

/* The representation of node `node`'s value. */
Repr repr_of(const Compiler *c, int node);
/* The representation of a local variable's slot. */
Repr repr_of_slot(const Compiler *c, const LocalVar *lv);
/* Called once the analysis is final (the end of analyze_program): from here
   on the flags repr_of reads no longer change. */
void repr_seal(Compiler *c);
/* Whether an Integer or Float node's box has to test for the nil sentinel
   (emit_boxed's sp_box_int_or_nil / sp_box_float_or_nil). */
int repr_nil_scalar(const Compiler *c, int node, TyKind t);
/* Does a user object of kind t box with the class id it carries
   (sp_box_nullable_obj_dyn)? Its class has a subclass, and it is neither a
   value type nor an exception, whose object starts with its class name. */
int repr_dyn_cls(const Compiler *c, TyKind t);
/* the flag readers that decide it (old names: box_nullable_arg, the local
   arm of call_returns_nullable_int) */
int repr_box_nullable_arg(Compiler *c, int v);
int repr_local_nullable_int(Compiler *c, int node);
/* Whether repr_seal has run for the current compile. */
int repr_sealed(void);

/* R1 (--repr-check): the form a boxer gave a value, recorded at each of
   emit_boxed's and emit_boxed_text's returns, and the form repr_of predicts
   for it. */
typedef enum {
  RF_PASS,          /* already an sp_RbVal */
  RF_NIL_EFFECT,    /* evaluated for its effect, then nil */
  RF_INT,           /* sp_box_int */
  RF_INT_NIL,       /* an Integer whose sentinel boxes as nil */
  RF_FLT,           /* sp_box_float */
  RF_FLT_NIL,       /* a Float whose sentinel boxes as nil */
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
  CF_NIL_SENT,      /* the slot's nil: a nil literal, or a value with no C type
                       evaluated for its effect */
  CF_INT2BIG,       /* an Integer widened into a Bignum slot */
  CF_POLY_RHS,      /* a boxed value through its scalar conversion
                       (emit_poly_rhs_coerced) */
  CF_CHECKED_UNBOX, /* a boxed value through the checked unbox */
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

#endif
