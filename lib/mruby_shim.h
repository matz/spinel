/*
 * Minimal shim to compile mruby-bigint without the full mruby runtime.
 * Provides just enough types and macros for bigint.c to compile standalone.
 */
#ifndef MRUBY_SHIM_H
#define MRUBY_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* GCC < 10 doesn't predefine __has_builtin, so #if expressions like
   `defined(__GNUC__) || __has_builtin(...)` choke on the bare token.
   Provide a no-op fallback so the || short-circuits cleanly on the
   left when the compiler doesn't support __has_builtin (issue #55). */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* Basic mruby types */
typedef int64_t mrb_int;
typedef uint64_t mrb_uint;
typedef double mrb_float;
typedef int mrb_bool;
#define TRUE 1
#define FALSE 0
#define MRB_INT_MAX INT64_MAX
#define MRB_INT_MIN INT64_MIN
#define MRB_INT_BIT 64

/* mrb_state stub */
typedef struct mrb_state {
  int dummy;
} mrb_state;

/* Memory allocation */
#define mrb_malloc(mrb, sz) malloc(sz)
#define mrb_realloc(mrb, p, sz) realloc(p, sz)
#define mrb_free(mrb, p) free(p)
#define mrb_malloc_simple(mrb, sz) malloc(sz)

/* Error handling */
#define mrb_raise(mrb, cls, msg) do { fprintf(stderr, "%s\n", msg); exit(1); } while(0)
#define mrb_int_zerodiv(mrb) do { fprintf(stderr, "divided by 0\n"); exit(1); } while(0)
#define E_RANGE_ERROR 0
#define E_RUNTIME_ERROR 0
#define E_ARGUMENT_ERROR 0
#define E_TYPE_ERROR 0

/* mrb_value stub - tagged union */
typedef struct mrb_value {
  union {
    mrb_int i;
    mrb_float f;
    void *p;
  } value;
  int tt;
} mrb_value;

enum mrb_vtype {
  MRB_TT_FALSE = 0,
  MRB_TT_TRUE,
  MRB_TT_INTEGER,
  MRB_TT_FLOAT,
  MRB_TT_STRING,
  MRB_TT_BIGINT,
};

#define mrb_integer(v) ((v).value.i)
#define mrb_float(v) ((v).value.f)
#define mrb_ptr(v) ((v).value.p)
#define mrb_type(v) ((v).tt)
#define mrb_integer_p(v) ((v).tt == MRB_TT_INTEGER)
#define mrb_float_p(v) ((v).tt == MRB_TT_FLOAT)
#define mrb_bigint_p(v) ((v).tt == MRB_TT_BIGINT)
#define mrb_fixnum_value(i) ((mrb_value){{.i=(i)}, MRB_TT_INTEGER})
#define mrb_int_value(mrb, i) mrb_fixnum_value(i)

/* Object header stub */
#define MRB_OBJECT_HEADER uint32_t flags; uint32_t _pad
#define mrb_obj_ptr(v) ((struct RBasic*)mrb_ptr(v))
#define mrb_static_assert_object_size(t) /* nop */
#define mrb_assert(x) assert(x)

/* String */
#define RSTRING_PTR(s) ((const char*)mrb_ptr(s))
#define RSTRING_LEN(s) ((mrb_int)strlen(RSTRING_PTR(s)))

/* nil value */
static inline mrb_value mrb_nil_value(void) {
  mrb_value v; v.tt = MRB_TT_FALSE; v.value.i = 0; return v;
}

/* MRB_ENSURE stub - execute body, no exception handling */
#define MRB_ENSURE(mrb, exc, body, data) \
  { mrb_value exc = body(mrb, data); (void)exc; } if(0)

/* Bigint object creation */
struct RBasic {
  MRB_OBJECT_HEADER;
};

#endif /* MRUBY_SHIM_H */
