/* Compile-time values: what Ruby code the compiler runs ahead of time
 * computes -- a class body's macro call, a loop over a constant table, as
 * the analysis runs them (ct_eval.c) -- and the operations on them
 * the evaluators share. nil, true, false, an Integer within 64 bits, a
 * String, a Symbol, and an Array of these.
 *
 * Nothing here reads a tree: each evaluator walks its own (Prism's nodes, or
 * the node table's) and computes with these. An operation answers 0 where it
 * does not know CRuby's answer for certain (a Bignum, a non-ASCII case
 * mapping, a pattern past the few it follows), and the caller then leaves
 * the code to run when the program does. */
#ifndef SPINEL_CT_VALUE_H
#define SPINEL_CT_VALUE_H

#include <stddef.h>

typedef enum { MV_UNDEF = 0, MV_NIL, MV_TRUE, MV_FALSE, MV_INT, MV_STR, MV_SYM, MV_ARR } MvKind;

/* A String is an object with an identity. frozen: the object is frozen.
   lit: a literal of the program made it -- frozen, it is the literal of its
   bytes, the one object every such literal is; unfrozen, it is an object
   the program holds (a table's element). A String an operation answers is a
   new object, unfrozen and not a literal's; one that answers an object it
   holds (String#to_s, Array#first) answers that object. An Array's identity
   is its storage (a); each Array literal run makes a new one. */
typedef struct Mv { MvKind k; long long i; char *s; struct Mv *a; int n; int frozen, lit; } Mv;

/* a growing NUL-terminated text */
typedef struct MvBuf { char *p; size_t len, cap; } MvBuf;

void mvb_putn(MvBuf *b, const char *s, size_t n);
void mvb_puts(MvBuf *b, const char *s);

Mv mv_nil(void);
Mv mv_undef(void);
Mv mv_str(const char *s, size_t n, MvKind k);   /* a String or Symbol of those bytes */
int mv_truthy(Mv v);
int mv_eq(Mv a, Mv b);

/* `s` spells a Symbol that needs no quotes (`:name`, `:name?`) */
int mv_sym_plain(const char *s);
/* every byte of `s` is ASCII */
int mv_ascii(const char *s);

void mv_to_s(Mv v, MvBuf *b);
/* `s` as a double-quoted Ruby literal that reads back as the same bytes */
void mv_quote(const char *s, MvBuf *b);
/* v as Ruby source that reads back as v */
void mv_inspect(Mv v, MvBuf *b);
/* does mv_inspect spell v exactly as CRuby's inspect does? */
int mv_inspect_exact(Mv v);

/* ---- the patterns gsub / sub follow: plain text, one character class or
   escaped character with an optional `+`, or a run of literal characters --
   matched over ASCII bytes, as CRuby's engine matches them there */
typedef struct { int lit; char set[256]; int plus; const char *text; size_t tn; } MvPat;

/* the pattern a regexp literal's source [p, e) spells, 1 when it is one of
   those (`pt` points into the source) */
int mv_pat_regexp(const char *p, const char *e, MvPat *pt);
/* the length of a match of pt at s (0: none) */
size_t mv_pat_at(const MvPat *pt, const char *s);

/* ---- the methods an evaluator runs on a value, each answering as CRuby
   does or 0 (not known here) */
/* r.gsub(pt, rep) (all) or r.sub(pt, rep): an ASCII String receiver, and a
   replacement without a backslash (which would name a group) */
int mv_sub(Mv r, const MvPat *pt, const char *rep, int all, Mv *out);
/* r.name with no argument (argc 0) or with a0 (argc 1): nil?, !, to_s,
   to_sym, inspect, downcase / upcase / capitalize, compact, reverse,
   size / length, first / last, join, the Integer comparisons and
   arithmetic, == / !=, String and Array +, Array#[] */
int mv_call(const char *name, Mv r, int argc, Mv a0, Mv *out);

#endif
