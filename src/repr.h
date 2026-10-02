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
} Repr;

/* The representation of node `node`'s value. */
Repr repr_of(const Compiler *c, int node);
/* The representation of a local variable's slot. */
Repr repr_of_slot(const Compiler *c, const LocalVar *lv);
/* Called once the analysis is final (the end of analyze_program): from here
   on the flags repr_of reads no longer change. */
void repr_seal(Compiler *c);
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

#endif
