/*
** shim/mruby.h - the slice of mruby's API the regexp engine reads
**
** lib/regexp holds mruby-regexp's engine as mruby carries it (re_compile.c,
** re_exec.c, re_utf8.c, re_internal.h, re_ctype.h, re_cased.h) together
** with mruby's case tables (unicase.c, unicase.h), so that a later change
** there is taken by copying the files again. The engine is written against
** mruby's API; spinel has no mruby VM, and these headers answer the calls
** the engine makes with the C library. re_spinel.c defines the functions
** declared here and gives the engine the entry points spinel calls.
*/
#ifndef SP_RE_SHIM_MRUBY_H
#define SP_RE_SHIM_MRUBY_H

#include "sp_re_names.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* the build the engine reads a string as: UTF-8, with the Unicode tables
   (RE_UNICODE_CASE, and re_ctype.h above ASCII) unless a -DMRB_USE_ASCII_CTYPE
   build leaves them out */
#ifndef MRB_UTF8_STRING
# define MRB_UTF8_STRING
#endif
#ifndef HAVE_MRUBY_REGEXP_GEM
# define HAVE_MRUBY_REGEXP_GEM
#endif
/* spinel's switches for a build without the Unicode tables: mruby has the
   one, which leaves out the case foldings and the classes above ASCII
   together */
#if (defined(RE_NO_UNICODE_CASE) || defined(RE_NO_UNICODE_CTYPE)) && !defined(MRB_USE_ASCII_CTYPE)
# define MRB_USE_ASCII_CTYPE
#endif

#ifdef __cplusplus
# define MRB_BEGIN_DECL extern "C" {
# define MRB_END_DECL }
#else
# define MRB_BEGIN_DECL
# define MRB_END_DECL
#endif

/* The backtracking engine's ceiling, as spinel's engine had it before this
   one: mruby's default of 2048 entries refuses a subject spinel matched
   (a backreference to a run of 20000, test/regexp_backtrack_heap_stack.rb),
   and a limit is not lowered because what it counts moved. */
#ifndef MRB_REGEXP_STACK_LIMIT
# define MRB_REGEXP_STACK_LIMIT 32768
#endif

typedef int64_t mrb_int;
typedef uint8_t mrb_bool;
#ifndef TRUE
# define TRUE 1
#endif
#ifndef FALSE
# define FALSE 0
#endif
#define MRB_INT_MAX INT64_MAX

/* The engine threads an mrb_state through every call it makes; nothing
   reads it here. */
typedef struct mrb_state { int unused; } mrb_state;

/* A String, as the engine uses one: a growable buffer it builds a message or
   a table in. Every one made during a compile is released when the compile
   ends (re_spinel.c), as mruby's GC would release it. */
typedef struct sp_re_str *mrb_value;
static inline mrb_value mrb_nil_value(void) { return NULL; }
static inline mrb_bool mrb_nil_p(mrb_value v) { return v == NULL; }

typedef int mrb_sym;
#define MRB_SYM(name) 0

#ifdef MRB_DEBUG
# define mrb_assert(p) assert(p)
#else
# define mrb_assert(p) ((void)0)
#endif
#define mrb_static_assert(...) _Static_assert(__VA_ARGS__)

void *mrb_malloc(mrb_state *mrb, size_t len);
void *mrb_malloc_simple(mrb_state *mrb, size_t len);
void *mrb_calloc(mrb_state *mrb, size_t nelem, size_t len);
void *mrb_realloc(mrb_state *mrb, void *p, size_t len);
void *mrb_realloc_simple(mrb_state *mrb, void *p, size_t len);
void mrb_free(mrb_state *mrb, void *p);

#endif /* SP_RE_SHIM_MRUBY_H */
