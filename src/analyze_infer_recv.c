/* analyze_infer_recv.c -- receiver-typed call inference, split out of
   infer_call. Pure code movement, no logic change: each helper holds the arms
   for one receiver kind, in their original order, and infer_call calls it at
   the point those arms occupied.

   The helpers report `1 = handled` with the type through `out` rather than
   returning a TyKind, because TY_UNKNOWN is a real answer here -- a String
   range's whole traversal face deliberately answers UNKNOWN so the element
   array serves it -- and a bare TyKind return could not tell that from
   "declined". */
#include "analyze_internal.h"
#include <stdio.h>
#include <string.h>

/* infer_type through fold_seed_kind, which owns the rule (see types.c). */
static TyKind fold_seed_infer_ty(Compiler *c, int node) {
  return fold_seed_kind(infer_type(c, node), nt_type(c->nt, node));
}

/* The operator a seeded fold applies, whichever spelling was used.
   `reduce(seed, :op)` carries the symbol as the trailing argument, with the
   seed in front of it; `reduce(seed, &:op)` carries it in the block, which
   leaves the seed as the only argument. Both are the same fold and take the
   same accumulator rule -- reading only the argument list typed the &:op
   spelling from the seed and then dropped it. */
static const char *fold_seed_op_sym(Compiler *c, int id, int argc, const int *argv) {
  const NodeTable *nt = c->nt;
  if (argc >= 2 && argv) return sym_static_value(c, argv[argc - 1]);
  if (argc != 1) return NULL;
  { int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) return NULL;
    { int ex = nt_ref(nt, blk, "expression");
      if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode"))
        return nt_str(nt, ex, "value"); } }
  return NULL;
}

/* True when `id` is the receiver of an enclosing call that carries a block:
   the chain emitters own that shape (arr.map.with_index { }), so the inner
   blockless call must keep its legacy typing. */
static int call_is_chain_receiver_with_block(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (nt_ref(nt, n, "receiver") != id) continue;
    if (nt_ref(nt, n, "block") < 0) return 0;
    /* `<blockless>.each { blk }` is the one chained consumer that does not take
       the block for ITSELF: Enumerator#each runs the method the Enumerator came
       from, which the desugar rewrites back to `<blockless> { blk }` -- and it
       recognises that shape by this call being typed an Enumerator. Left in the
       guard, `arr.map.each { }` typed neither and answered NoMethodError at run
       time (#4331). */
    { const char *cn = nt_str(nt, n, "name");
      if (cn && sp_streq(cn, "each")) return 0; }
    return 1;
  }
  return 0;
}

/* Range receivers: the Float and String range faces, and the Integer-range
   arms that answer without materializing. The redispatch that rewrites `rt`
   to the int array stays in infer_call: it changes the receiver kind for
   every arm after it rather than answering. */
int infer_range_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv;
  if (!name) return 0;
  /* A Float range (1.0..3.0) is a distinct type with float endpoints; it is
     not iterable, so its whole method face reduces to endpoint queries,
     membership tests, and the sole materializing method, step. */
  /* String range ("a".."e"): the endpoints answer natively; every traversal
     rides the materialized element array (#3064). */
  if (rt == TY_STR_RANGE) {
    if (sp_streq(name, "begin") || sp_streq(name, "end") ||
        sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "to_s") || sp_streq(name, "inspect"))
      { *out = argc == 0 ? TY_STRING : TY_STR_ARRAY; return 1; }
    if ((sp_streq(name, "first") || sp_streq(name, "last")))
      { *out = argc == 0 ? TY_STRING : TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "cover?") || sp_streq(name, "include?") ||
        sp_streq(name, "member?") || sp_streq(name, "===") ||
        sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "eql?") ||
        sp_streq(name, "exclude_end?") || sp_streq(name, "frozen?") ||
        sp_streq(name, "nil?") || sp_streq(name, "is_a?") ||
        sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?") ||
        sp_streq(name, "equal?") || sp_streq(name, "respond_to?"))
      { *out = TY_BOOL; return 1; }
    /* step(n) / %(n): an Enumerator over every nth member (#3671) */
    if ((sp_streq(name, "step") || sp_streq(name, "%")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0)
      { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "class")) { *out = TY_CLASS; return 1; }
    if (sp_streq(name, "hash")) { *out = TY_INT; return 1; }
    /* Range#size counts INTEGER elements, so a string range has none: nil
       (CRuby), not the materialized array's length. */
    if (sp_streq(name, "size") && argc == 0) { *out = TY_NIL; return 1; }
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0)
      { *out = TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
        sp_streq(name, "dup") || sp_streq(name, "clone"))
      { *out = TY_STR_RANGE; return 1; }
    /* everything else is served by the element array (see the desugar) */
    { *out = TY_UNKNOWN; return 1; }
  }
  if (rt == TY_FLOAT_RANGE) {
    /* #size counts the integers the range enumerates: a Float answer, since an
       unbounded end makes it Infinity (#3670). Only an Integer begin has an
       enumeration at all; the emitter checks that and leaves the rest to the
       TypeError CRuby raises. */
    if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int rq3 = nt_ref(nt, id, "receiver");
      while (rq3 >= 0 && nt_kind(nt, rq3) == NK_ParenthesesNode) {
        int pb3 = nt_ref(nt, rq3, "body"); int pn3 = 0;
        const int *pd3 = pb3 >= 0 ? nt_arr(nt, pb3, "body", &pn3) : NULL;
        rq3 = (pn3 == 1 && pd3) ? pd3[0] : -1;
      }
      int lo3 = (rq3 >= 0 && nt_kind(nt, rq3) == NK_RangeNode) ? nt_ref(nt, rq3, "left") : -1;
      if (lo3 >= 0 && infer_type(c, lo3) == TY_INT) { *out = TY_FLOAT; return 1; }
    }
    if (sp_streq(name, "begin") || sp_streq(name, "end") ||
        sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "min") || sp_streq(name, "max")) {
      /* A range is a Float range when EITHER endpoint is one, and the endpoint
         methods answer the endpoint the caller wrote: `(-Float::INFINITY..5).max`
         is the Integer 5 (#3837). */
      if (argc == 0) {
        int rnode = recv;
        for (int g = 0; g < 8 && rnode >= 0 && nt_kind(nt, rnode) == NK_ParenthesesNode; g++) {
          int pb2 = nt_ref(nt, rnode, "body");
          int pn2 = 0; const int *ps2 = pb2 >= 0 ? nt_arr(nt, pb2, "body", &pn2) : NULL;
          rnode = (pn2 == 1 && ps2) ? ps2[0] : -1;
        }
        if (rnode >= 0 && nt_kind(nt, rnode) == NK_RangeNode) {
          int side = (sp_streq(name, "begin") || sp_streq(name, "first") ||
                      sp_streq(name, "min")) ? nt_ref(nt, rnode, "left") : nt_ref(nt, rnode, "right");
          if (side >= 0 && infer_type(c, side) == TY_INT) { *out = TY_INT; return 1; }
        }
      }
      { *out = argc == 0 ? TY_FLOAT : TY_POLY; return 1; }   /* first(n)/last(n) raise anyway */
    }
    if (sp_streq(name, "cover?") || sp_streq(name, "include?") ||
        sp_streq(name, "member?") || sp_streq(name, "===") ||
        sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "eql?") ||
        sp_streq(name, "exclude_end?") || sp_streq(name, "frozen?") ||
        sp_streq(name, "respond_to?") || sp_streq(name, "nil?") ||
        sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?") || sp_streq(name, "equal?"))
      { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "minmax") && argc == 0) { *out = TY_FLOAT_ARRAY; return 1; }  /* the endpoints (#3690) */
    if (sp_streq(name, "step")) { *out = TY_FLOAT_ARRAY; return 1; }
    if (sp_streq(name, "bsearch") && nt_ref(nt, id, "block") >= 0) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "class")) { *out = TY_CLASS; return 1; }
    if (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
        sp_streq(name, "dup") || sp_streq(name, "clone"))
      { *out = TY_FLOAT_RANGE; return 1; }
    /* each/map/sum/to_a/... raise "can't iterate from Float" at run time; a
       poly result keeps the boxed-nil slot the raise leaves behind valid (and
       lets respond_to? report these Enumerable methods as present, like CRuby).
       A name outside this set is genuinely undefined, so leave it UNKNOWN: the
       respond_to? probe reads that as "not dispatchable" (false), matching an
       ordinary int range, and a real call errors like any unknown method. */
    {
      static const char *const iter[] = {
        "each", "map", "collect", "select", "filter", "reject", "to_a", "to_h",
        "entries", "find", "detect", "find_index", "count", "sum", "sort",
        "sort_by", "min_by", "max_by", "reduce", "inject", "each_with_index",
        "flat_map", "collect_concat", "any?", "all?", "none?", "one?", "take",
        "drop", "take_while", "drop_while", "filter_map", "partition",
        "group_by", "each_with_object", "tally", "find_all", "zip", "grep",
        "grep_v", "uniq", "reverse", "minmax", "join", "index", "size", "lazy",
        "each_cons", "each_slice", "chunk", "chunk_while", "cycle", NULL };
      for (int k = 0; iter[k]; k++) if (sp_streq(name, iter[k])) { *out = TY_POLY; return 1; }
    }
    { *out = TY_UNKNOWN; return 1; }
  }
  /* endless literal range: size is the Float infinity; take/first with a
     count materialize just the counted prefix (nothing else can) */
  /* (1..5.5): the end readers answer the Float the caller wrote; the integer
     representation truncated it (#3896). */
  if (rt == TY_RANGE && recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "end") || sp_streq(name, "last") || sp_streq(name, "max")) &&
      range_lit_float_end(c, recv) >= 0)
    { *out = TY_FLOAT; return 1; }
  if (rt == TY_RANGE && recv >= 0) {
    int rnA = recv;
    while (rnA >= 0 && nt_type(nt, rnA) && sp_streq(nt_type(nt, rnA), "ParenthesesNode")) {
      int pbA = nt_ref(nt, rnA, "body"); int pnA = 0;
      const int *ppA = pbA >= 0 ? nt_arr(nt, pbA, "body", &pnA) : NULL;
      rnA = pnA == 1 ? ppA[0] : -1;
    }
    /* a local holding only such a literal counts too (sole-assignment) */
    if (rnA >= 0 && nt_type(nt, rnA) && !sp_streq(nt_type(nt, rnA), "RangeNode")) {
      int slA = local_sole_range_node(c, rnA);
      if (slA >= 0) rnA = slA;
    }
    if (rnA >= 0 && nt_type(nt, rnA) && sp_streq(nt_type(nt, rnA), "RangeNode") &&
        (nt_ref(nt, rnA, "right") < 0 ||
         infer_end_is_float_inf(c, nt_ref(nt, rnA, "right"))) &&
        nt_ref(nt, rnA, "left") >= 0) {
      if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0 &&
          nt_ref(nt, id, "block") < 0)
        { *out = TY_FLOAT; return 1; }   /* an endless range counts forever: Infinity (#3668) */
      if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1)
        { *out = TY_INT_ARRAY; return 1; }
      /* the block forms that walk up from the bounded end rather than
         materializing: the elements are the range's own ints (#3863) */
      if (nt_ref(nt, id, "block") >= 0 && argc == 0) {
        if (sp_streq(name, "find") || sp_streq(name, "detect")) { *out = TY_INT; return 1; }
      }
    }
  }
  /* min(n) / max(n) on an Integer Range answer an Array of its ints, however
     the Range is bounded (#3665) */
  if (rt == TY_RANGE && recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "min") || sp_streq(name, "max")))
    { *out = TY_INT_ARRAY; return 1; }
  /* Range#sum(seed): CRuby adds the closed form to an INTEGER seed and answers
     a Float for a seed of any other class (it runs the seed through
     Kernel#Float first), so only an Integer or Float seed lands in a scalar
     slot -- a Bignum seed wrapped in one, and a Rational one did not compile
     at all. */
  if (rt == TY_RANGE && sp_streq(name, "sum") && argc == 1 &&
      nt_ref(nt, id, "block") < 0) {
    TyKind st = fold_seed_infer_ty(c, argv[0]);
    if (st == TY_FLOAT) { *out = TY_FLOAT; return 1; }
    *out = fold_seed_typed(st, TY_INT) ? TY_INT : TY_POLY;
    return 1;
  }
  /* each_slice(n) { } / each_cons(n) { } answer the receiver, which the value
     emitter yields; the materializing redispatch below would otherwise type
     them as the int array it walks, and an assigned result then printed the
     elements instead of the Range (#3920). */
  if (rt == TY_RANGE && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode") &&
      ((argc == 1 && (sp_streq(name, "each_slice") || sp_streq(name, "each_cons"))) ||
       (argc == 0 && (sp_streq(name, "reverse_each") || sp_streq(name, "each_with_index")))))
    { *out = TY_RANGE; return 1; }
  return 0;
}

/* Numeric receivers: the Complex and Rational faces, the mixed Integer/Float x Complex operators, and the curried-Proc accumulator */
int infer_numeric_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv; (void)nt;
  if (!name) return 0;
  /* A numeric receiver and a coercing user object: the answer is whatever the
     pair #coerce hands back computes, and that is a run-time class -- CRuby's
     own `def coerce(v) = [2.0, v]` turns an Integer receiver's #modulo into a
     Float. Typed from the receiver instead, the named methods reinterpreted
     the boxed result under the wrong tag: 5.modulo(obj) read the bits of 2.0
     as an Integer and printed 4611686018427387904. The comparisons keep their
     bool and `<=>` its int, which the protocol cannot widen. This mirrors
     emit_numeric_coerce_call's guard so the two agree on every shape. */
  if (argc == 1 && recv >= 0 && is_numeric_coerce_op(name) &&
      !is_cmp_op(name) && !sp_streq(name, "<=>") &&
      nt_ref(nt, id, "block") < 0 &&
      (rt == TY_INT || rt == TY_FLOAT || rt == TY_RATIONAL || rt == TY_BIGINT)) {
    TyKind ac = comp_ntype(c, argv[0]);
    if (ty_is_object(ac) && class_has_coerce_shape(c, ty_object_class(ac))) {
      *out = TY_POLY;
      return 1;
    }
  }
  if (rt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "==") || sp_streq(name, "!=")) { *out = TY_BOOL; return 1; }
  }
  if (rt == TY_FLOAT && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "==") || sp_streq(name, "!=")) { *out = TY_BOOL; return 1; }
  }
  if (rt == TY_RATIONAL && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX &&
      (sp_streq(name, "+") || sp_streq(name, "-") ||
       sp_streq(name, "*") || sp_streq(name, "/"))) { *out = TY_COMPLEX; return 1; }
  if (rt == TY_COMPLEX) {
    if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) { *out = TY_FLOAT; return 1; }
    /* real/imaginary/abs/abs2 box to poly: each component keeps its CRuby
       class (Integer or Float) -- the class is a runtime property. */
    if (sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag") ||
        sp_streq(name, "abs") || sp_streq(name, "magnitude") || sp_streq(name, "abs2")) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "polar") || sp_streq(name, "rect") || sp_streq(name, "rectangular"))
      { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "conjugate") || sp_streq(name, "conj") || sp_streq(name, "to_c") ||
        sp_streq(name, "-@") || sp_streq(name, "+@") ||
        sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
        sp_streq(name, "/") || sp_streq(name, "quo")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "**")) { *out = TY_COMPLEX; return 1; }
    /* Complex is not Comparable and has no modulo: these raise NoMethodError
       (typed Complex only so the raise expression has a consistent slot) (#2618) */
    if (sp_streq(name, "%") || sp_streq(name, "modulo")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "==") || sp_streq(name, "!=")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
        sp_streq(name, "denominator")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "to_f")) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "to_r")) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "numerator")) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "zero?") || sp_streq(name, "real?") ||
        sp_streq(name, "integer?") || sp_streq(name, "finite?") ||
        sp_streq(name, "eql?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "nonzero?")) { *out = TY_POLY; return 1; }   /* self (Complex) or nil */
    if (sp_streq(name, "infinite?")) { *out = TY_INT; return 1; }      /* 1 or nil (sentinel) */
    if (sp_streq(name, "<=>") && argc == 1) { *out = TY_INT; return 1; }  /* -1/0/1 or nil (sentinel) */
    if (sp_streq(name, "rationalize") && (argc == 0 || argc == 1)) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "fdiv") && argc == 1) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "coerce") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
  }
  /* Proc#curry and curry application via []. A curried call stays TY_CURRY until
     it reaches the proc's arity, when it realizes to the proc's return type (the
     runtime accumulates int args, so completion typing covers int-returning
     procs; partial applications and other returns remain TY_CURRY). */
  if (rt == TY_PROC && sp_streq(name, "curry")) { *out = TY_CURRY; return 1; }
  if (rt == TY_CURRY && (sp_streq(name, "[]") || sp_streq(name, "call") || sp_streq(name, "()"))) {
    int complete = 0; TyKind cret = TY_UNKNOWN;
    int traced = curry_apply_info(c, id, &complete, &cret);
    /* an untraceable base saturates (or not) at RUN time, so the call answers
       either the realized value or another curry: poly holds both (#4068) */
    if (!traced) { *out = TY_POLY; return 1; }
    if (complete) { *out = cret == TY_INT ? TY_INT : TY_POLY; return 1; }
    { *out = TY_CURRY; return 1; }
  }
  /* A curried proc reports as a lambda Proc (#2651). */
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "arity")) { *out = TY_INT; return 1; }
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "lambda?")) { *out = TY_BOOL; return 1; }
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "to_proc")) { *out = TY_PROC; return 1; }
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "parameters")) { *out = TY_POLY_ARRAY; return 1; }

  /* clamp(lo, hi) with a nil (open) bound returns the receiver or the applied
     bound unchanged, boxed to preserve its class (#2588). */
  if ((rt == TY_INT || rt == TY_FLOAT) && sp_streq(name, "clamp") && argc == 2 &&
      (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL))
    { *out = TY_POLY; return 1; }
  /* clamp(lo, hi) with a Rational bound: the applied bound decides the result
     class at runtime, so the result is boxed (#3232). */
  if ((rt == TY_INT || rt == TY_FLOAT) && sp_streq(name, "clamp") && argc == 2 &&
      (infer_type(c, argv[0]) == TY_RATIONAL || infer_type(c, argv[1]) == TY_RATIONAL))
    { *out = TY_POLY; return 1; }
  if (rt == TY_INT && sp_streq(name, "clamp") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode") &&
      ((nt_ref(nt, argv[0], "left") >= 0 && infer_type(c, nt_ref(nt, argv[0], "left")) == TY_FLOAT) ||
       (nt_ref(nt, argv[0], "right") >= 0 && infer_type(c, nt_ref(nt, argv[0], "right")) == TY_FLOAT)))
    { *out = TY_POLY; return 1; }
  /* a non-float bound can be returned as-is: a float receiver's mixed
     2-arg clamp is boxed (0.5.clamp(1, 3) is the Integer 1) */
  if (rt == TY_FLOAT && sp_streq(name, "clamp") && argc == 2 &&
      !(infer_type(c, argv[0]) == TY_FLOAT && infer_type(c, argv[1]) == TY_FLOAT) &&
      (infer_type(c, argv[0]) == TY_INT || infer_type(c, argv[0]) == TY_FLOAT) &&
      (infer_type(c, argv[1]) == TY_INT || infer_type(c, argv[1]) == TY_FLOAT)) { *out = TY_POLY; return 1; }
  if (rt == TY_INT && sp_streq(name, "divmod") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) { *out = TY_POLY_ARRAY; return 1; }
  if (rt == TY_INT && sp_streq(name, "modulo") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
  if (rt == TY_INT && sp_streq(name, "remainder") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
  if (rt == TY_FLOAT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL &&
      (sp_streq(name, "%") || sp_streq(name, "modulo"))) { *out = TY_FLOAT; return 1; }
  if (rt == TY_FLOAT && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_RANGE) { *out = TY_POLY; return 1; }
  /* clamp(Float range): the clamped-to bound keeps its Float class; the result
     is boxed (Float, or Int for an in-range Int receiver) -> poly. */
  if ((rt == TY_FLOAT || rt == TY_INT) && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) { *out = TY_POLY; return 1; }
  if (rt == TY_INT && sp_streq(name, "round") && argc >= 1 &&
      nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) { *out = TY_INT; return 1; }
  if (rt == TY_INT && sp_streq(name, "quo") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
  if (rt == TY_INT && sp_streq(name, "quo")) { *out = TY_RATIONAL; return 1; }
  /* Float#quo is float division (Numeric#quo via /; no Rational) */
  if (rt == TY_FLOAT && sp_streq(name, "quo")) { *out = TY_FLOAT; return 1; }
  /* Float <op> Rational (either side) coerces to Float; comparisons bool */
  if (argc == 1 &&
      ((rt == TY_FLOAT && comp_ntype(c, argv[0]) == TY_RATIONAL) ||
       (rt == TY_RATIONAL && comp_ntype(c, argv[0]) == TY_FLOAT))) {
    if (is_arith_op(name) || sp_streq(name, "quo") || sp_streq(name, "fdiv"))
      { *out = TY_FLOAT; return 1; }
    if (is_cmp_op(name) || sp_streq(name, "==")) { *out = TY_BOOL; return 1; }
  }
  /* Integer <op> Rational coerces the Integer to Rational (result Rational for
     arithmetic, Bool/Int for comparisons). */
  if (rt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "%") || sp_streq(name, "modulo") || sp_streq(name, "remainder")) { *out = TY_RATIONAL; return 1; }
    if (sp_streq(name, "divmod")) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") || sp_streq(name, ">=") ||
        sp_streq(name, "==") || sp_streq(name, "!=")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "<=>")) { *out = TY_INT; return 1; }
  }
  if (rt == TY_RATIONAL) {
    /* step walks the sequence yielding Rational/Integer values: with a block it
       returns the receiver (self), without one it materializes a poly array of
       the boxed values (#2566). */
    if (sp_streq(name, "step")) {
      if (nt_ref(nt, id, "block") >= 0) { *out = rt; return 1; }
      { *out = TY_POLY_ARRAY; return 1; }
    }
    if (sp_streq(name, "numerator") || sp_streq(name, "denominator")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "to_f") || sp_streq(name, "fdiv")) { *out = TY_FLOAT; return 1; }
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "div")) { *out = TY_INT; return 1; }
    /* round/truncate: no digits (or a literal <= 0) is an Integer, a literal
       positive precision keeps the Rational, and a non-literal precision boxes
       to poly so the class is chosen from the runtime value. */
    if (sp_streq(name, "round") || sp_streq(name, "truncate") ||
        sp_streq(name, "floor") || sp_streq(name, "ceil")) {
      /* a trailing `half:` keyword only picks the tie-break mode; peel it off
         the positional count for the class choice, as the Float rule does.
         Read as a positional argument it made `r.round(1, half: :even)` an
         Integer, and with no arm answering that shape the call fell through
         to a NoMethodError. */
      int rr_argc = argc;
      if (rr_argc >= 1 && nt_type(nt, argv[rr_argc - 1]) &&
          sp_streq(nt_type(nt, argv[rr_argc - 1]), "KeywordHashNode")) rr_argc--;
      if (rr_argc == 1) {
        if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode"))
          { *out = nt_int(nt, argv[0], "value", 0) > 0 ? TY_RATIONAL : TY_INT; return 1; }
        { *out = TY_POLY; return 1; }
      }
      { *out = TY_INT; return 1; }   /* no digits -> Integer */
    }
    if (sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
        sp_streq(name, "negative?") || sp_streq(name, "finite?") ||
        sp_streq(name, "integer?") || sp_streq(name, "real?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "infinite?") || sp_streq(name, "imaginary") ||
        sp_streq(name, "imag")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "nonzero?")) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "to_c")) { *out = TY_COMPLEX; return 1; }
    /* Rational#i -> Complex(0, self). spinel's Complex holds two floats, so the
       imaginary part renders as a float where CRuby keeps the exact Rational
       (see docs/limitations.md). #2706 */
    if (sp_streq(name, "i") && argc == 0) { *out = TY_COMPLEX; return 1; }
    if (sp_streq(name, "rectangular") || sp_streq(name, "rect") || sp_streq(name, "polar")) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "coerce") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "to_r") || sp_streq(name, "rationalize") ||
        sp_streq(name, "-@") || sp_streq(name, "+@") || sp_streq(name, "abs") ||
        sp_streq(name, "real") || sp_streq(name, "conjugate") || sp_streq(name, "conj") ||
        sp_streq(name, "abs2") || sp_streq(name, "magnitude")) { *out = TY_RATIONAL; return 1; }
    TyKind a0r = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    /* a coercing user object on the right: coerce answers a pair of THAT
       class, so the result is its own operator's return -- the rule the
       Integer and Float arms already follow. Typing it Rational handed the
       user object's result to sp_rational_to_s (#3489). */
    if (argc == 1 && is_arith_op(name) && ty_is_object(a0r) &&
        comp_method_in_chain(c, ty_object_class(a0r), "coerce", NULL) >= 0) {
      int op_mi_r = comp_method_in_chain(c, ty_object_class(a0r), name, NULL);
      if (op_mi_r >= 0) { *out = (TyKind)c->scopes[op_mi_r].ret; return 1; }
    }
    if (argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                      sp_streq(name, "/") || sp_streq(name, "quo")))
      /* a boxed operand folds through sp_poly_<op>, whose value is boxed: the
         operand's runtime class picks the result class, so it stays poly
         (typing it Rational handed an sp_RbVal to sp_rational_inspect) */
      { *out = a0r == TY_FLOAT ? TY_FLOAT : a0r == TY_POLY ? TY_POLY : TY_RATIONAL; return 1; }
    if (argc == 1 && sp_streq(name, "**")) { *out = a0r == TY_INT ? TY_RATIONAL : TY_FLOAT; return 1; }
    if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
                      sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=") ||
                      sp_streq(name, "==="))) { *out = TY_BOOL; return 1; }
    if (argc == 1 && sp_streq(name, "<=>")) { *out = TY_INT; return 1; }
    if (argc == 2 && sp_streq(name, "between?")) { *out = TY_BOOL; return 1; }
    if (argc == 2 && sp_streq(name, "clamp") &&
        infer_type(c, argv[0]) == TY_RATIONAL && infer_type(c, argv[1]) == TY_RATIONAL) { *out = TY_RATIONAL; return 1; }
    /* clamp with a non-Rational (Integer/Float) bound: the applied bound keeps
       its own class, so the result is boxed (#3233). */
    if (argc == 2 && sp_streq(name, "clamp")) { *out = TY_POLY; return 1; }
    if (argc == 1 && (sp_streq(name, "%") || sp_streq(name, "modulo") ||
                      sp_streq(name, "remainder")))
      { TyKind _a0 = infer_type(c, argv[0]);
        *out = _a0 == TY_FLOAT ? TY_FLOAT : _a0 == TY_POLY ? TY_POLY : TY_RATIONAL; return 1; }
    if (argc == 1 && sp_streq(name, "divmod")) { *out = TY_POLY_ARRAY; return 1; }
  }
  return 0;
}

/* The array a map-shaped call `id` answers: the tail value of its `block`
   is the element type. */
TyKind infer_map_block_ty(Compiler *c, int id, int block) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  TyKind bt = bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN;
  /* A value-carrying next widens the element type past the tail
     (e.g. `next "s"` string vs trailing `x` int -> poly array), so the
     collected value is boxed rather than assigned to a typed temp. */
  TyKind bnt = ie_block_break_next_ty(c, body);
  if (bnt != TY_UNKNOWN) bt = (bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
  /* A table of rows: ty_array_of has no kind for "array of int/float
     array" -- those exist only once narrow_object_arrays has decided a
     slot carries one -- so that pass records its decision here and this
     reads it back, the way the empty row literal of an
     `Array.new(n) { [] }` table already does (#4484). Without it a
     mapped table was built as a poly array, boxing every row on the way
     in only to unbox it again on each read. */
  if (c->arr_want && id < c->node_cap && ty_is_ptr_array(c->arr_want[id]))
    return c->arr_want[id];
  return ty_array_of(bt);
}

/* Hash receivers: the hash face of infer_call */
int infer_hash_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv; (void)nt;
  if (!name) return 0;
  if (recv >= 0 && ty_is_hash(rt)) {
    /* a blockless each/each_pair/each_key/each_value/each_with_index is an
       external Enumerator (the block forms iterate and are handled below). */
    if (nt_ref(nt, id, "block") < 0 && argc == 0 &&
        (sp_streq(name, "each") || sp_streq(name, "each_pair") ||
         sp_streq(name, "each_key") || sp_streq(name, "each_value") ||
         sp_streq(name, "each_with_index") ||
         /* blockless Enumerable methods are external Enumerators over the pairs */
         sp_streq(name, "map") || sp_streq(name, "collect") ||
         sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "reject") || sp_streq(name, "find") ||
         sp_streq(name, "detect") || sp_streq(name, "find_all") ||
         sp_streq(name, "flat_map") || sp_streq(name, "filter_map") ||
         sp_streq(name, "sort_by") || sp_streq(name, "min_by") ||
         sp_streq(name, "max_by") || sp_streq(name, "group_by") ||
         sp_streq(name, "partition")))
      { *out = TY_ENUMERATOR; return 1; }
    if (argc <= 1 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "any?") || sp_streq(name, "none?") ||
         sp_streq(name, "all?") || sp_streq(name, "one?")))
      { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "deconstruct_keys") && argc == 1) { *out = rt; return 1; }
    if (sp_streq(name, "compact!") && argc == 0) { *out = TY_POLY; return 1; }  /* self or nil */
    /* chunk { |k, v| key } is an enumerator of [key, [[k, v], ...]] pairs;
       the .to_a consumer arm types the materialized chain as a poly array. */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "chunk")) { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "to_proc")) { *out = TY_PROC; return 1; }
    if (sp_streq(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) { *out = TY_SYMBOL; return 1; }
    if (sp_streq(name, "key") && argc == 1) { *out = TY_POLY; return 1; }  /* the key (boxed) or nil */
    /* Enumerable first/take/drop over the [key, value] pair list */
    if (sp_streq(name, "first") && argc == 0 && nt_ref(nt, id, "block") < 0) { *out = TY_POLY; return 1; }
    if ((sp_streq(name, "first") || sp_streq(name, "take") || sp_streq(name, "drop")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) { *out = rt; return 1; }  /* identity */
    if (sp_streq(name, "slice") && argc >= 1) { *out = rt; return 1; }  /* key-subset hash */
    if (sp_streq(name, "[]"))     { *out = ty_hash_val(rt); return 1; }
    if (sp_streq(name, "[]=") || sp_streq(name, "store"))
      { *out = argc >= 2 ? ty_unify(infer_type(c, argv[1]), ty_hash_val(rt)) : ty_hash_val(rt); return 1; }
    if (sp_streq(name, "fetch")) {
      TyKind vt = ty_hash_val(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        /* A hash literal default `{}` infers TY_UNKNOWN but is still a hash value
           -- incompatible with a non-hash hash-val type like TY_INT. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(nt, argv[1]);
          if (atn && (sp_streq(atn, "HashNode") || sp_streq(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        if (ty_unify(vt, dt) == TY_POLY) { *out = TY_POLY; return 1; }
      }
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        TyKind bvt = bn > 0 ? infer_type(c, bb[bn - 1]) : vt;
        if (bvt != vt) { *out = TY_POLY; return 1; }
      }
      { *out = vt; return 1; }
    }
    if (sp_streq(name, "delete")) { *out = ty_hash_val(rt); return 1; }
    if (sp_streq(name, "dig") && argc >= 1) {
      /* dig(*keys) walks a runtime key list: the depth, and so the value's
         type, is not known here */
      if (argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode) { *out = ty_hash_val(rt); return 1; }
      { *out = TY_POLY; return 1; }
    }
    if (sp_streq(name, "default") && argc <= 1) { *out = TY_POLY; return 1; }  /* default(key) too (#2409) */
    if (sp_streq(name, "length") || sp_streq(name, "size") ||
        sp_streq(name, "count")) { *out = TY_INT; return 1; }
    if ((sp_streq(name, "<") || sp_streq(name, "<=") ||
         sp_streq(name, ">") || sp_streq(name, ">=")) && argc == 1)
      { *out = TY_BOOL; return 1; }  /* subset/superset comparisons */
    if (sp_streq(name, "delete") && argc == 1 && nt_ref(nt, id, "block") >= 0)
      { *out = TY_POLY; return 1; }  /* deleted value, or the block's fallback */
    if (sp_streq(name, "keys"))   { *out = ty_array_of(ty_hash_key(rt)); return 1; }
    if (sp_streq(name, "values")) { *out = ty_array_of(ty_hash_val(rt)); return 1; }
    if (sp_streq(name, "values_at") || sp_streq(name, "fetch_values")) { *out = TY_POLY_ARRAY; return 1; }
    int block = nt_ref(nt, id, "block");
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries") || sp_streq(name, "sort")) && block < 0)
      { *out = TY_POLY_ARRAY; return 1; }
    if (block >= 0 && (sp_streq(name, "find") || sp_streq(name, "detect")))
      { *out = TY_POLY_ARRAY; return 1; }   /* the winning [k, v] pair, or nil */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "sort_by"))
      { *out = TY_POLY_ARRAY; return 1; }   /* [k, v] pairs ordered by the block value */
    if (nt_ref(nt, id, "block") >= 0 && (sp_streq(name, "all?") || sp_streq(name, "any?")))
      { *out = TY_BOOL; return 1; }
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "sum"))
      { *out = TY_POLY; return 1; }   /* boxed accumulation via sp_poly_add */
    /* blockless Hash#sum: folds each [k, v] pair into the init, which is only
       well-defined for an empty hash (else `init + [k,v]` raises TypeError).
       The result is the init's type -- int by default. */
    if (nt_ref(nt, id, "block") < 0 && sp_streq(name, "sum") && argc == 1 &&
        (ty_is_array(infer_type(c, argv[0])) ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode"))))
      { *out = TY_POLY_ARRAY; return 1; }   /* an Array init concatenates the pairs (#3571) */
    /* Every other blockless seed keeps its own class: Enumerable#sum adds the
       first [k, v] pair to it with Ruby's `+`, so an Integer seed raises and
       a nil / false one raises from nil itself -- neither is an Integer, and
       an EMPTY hash answers the seed unchanged (`{}.sum(nil)` is nil). */
    if (nt_ref(nt, id, "block") < 0 && sp_streq(name, "sum") && argc == 1)
      { *out = fold_seed_typed(fold_seed_infer_ty(c, argv[0]), TY_INT) ? TY_INT : TY_POLY; return 1; }
    if (nt_ref(nt, id, "block") < 0 && sp_streq(name, "sum") && argc == 0)
      { *out = TY_INT; return 1; }
    {
      if (block >= 0 && (ty_iter_shape(name) == TY_ITER_MAP))
        { *out = infer_map_block_ty(c, id, block); return 1; }
      if (block >= 0 &&
          (sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "reject!")))
        { *out = TY_POLY; return 1; }  /* self, or nil when nothing was removed */
      if (block >= 0 && (sp_streq(name, "keep_if") || sp_streq(name, "delete_if")))
        { *out = rt; return 1; }  /* always self */
      if (block >= 0 && (ty_iter_shape(name) == TY_ITER_SELECT || sp_streq(name, "reject"))) { *out = rt; return 1; }
      if (block >= 0 && sp_streq(name, "transform_keys")) {
        /* a PolyPoly receiver boxes any key: the variant is stable */
        if (rt == TY_POLY_POLY_HASH) { *out = rt; return 1; }
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nkt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        /* Symbol keys have no scalar-valued hash variant, so keys becoming
           symbols (e.g. transform_keys(&:to_sym)) yield a SymPolyHash regardless
           of the value type. */
        if (nkt == TY_SYMBOL) { *out = TY_SYM_POLY_HASH; return 1; }
        TyKind r = ty_hash_of(nkt, ty_hash_val(rt));
        { *out = r != TY_UNKNOWN ? r : rt; return 1; }
      }
      if (block >= 0 && sp_streq(name, "transform_values")) {
        if (rt == TY_POLY_POLY_HASH) { *out = rt; return 1; }
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nvt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind kt = ty_hash_key(rt);
        TyKind r = ty_hash_of(kt, nvt);
        if (r != TY_UNKNOWN) { *out = r; return 1; }
        /* No concrete (key, block-result) hash variant exists (e.g. a Float or
           object value: there is no StrFloat hash). Falling back to the input
           type truncated the value (#3173); use a poly-valued hash of the same
           key kind so the block result is stored boxed, not coerced. */
        if (nvt != TY_UNKNOWN && nvt != ty_hash_val(rt)) {
          if (kt == TY_STRING) { *out = TY_STR_POLY_HASH; return 1; }
          if (kt == TY_SYMBOL) { *out = TY_SYM_POLY_HASH; return 1; }
          { *out = TY_POLY_POLY_HASH; return 1; }
        }
        { *out = rt; return 1; }
      }
    }
    /* merge(*hashes) folds through the universal boxed merge (#3561) */
    if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SplatNode"))
      { *out = TY_POLY_POLY_HASH; return 1; }
    if (sp_streq(name, "merge") && argc == 1) {
      TyKind at = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
      if (at == rt) { *out = rt; return 1; }  /* same type: trivial */
      /* cross-variant str-keyed merge: promote to str_poly_hash */
      if (ty_hash_key(rt) == TY_STRING && ty_is_hash(at) && ty_hash_key(at) == TY_STRING)
        { *out = TY_STR_POLY_HASH; return 1; }
      /* cross-variant sym-keyed merge: both sym → sym_poly (only sym_poly exists) */
      if (ty_hash_key(rt) == TY_SYMBOL && ty_is_hash(at) && ty_hash_key(at) == TY_SYMBOL)
        { *out = TY_SYM_POLY_HASH; return 1; }
      /* any other cross-variant merge (mismatched key or value layout) folds
         into the universal PolyPoly hash -- passing one specialized layout to
         another layout's merge helper read garbage / segfaulted (#3261) */
      if (ty_is_hash(at) && at != rt) { *out = TY_POLY_POLY_HASH; return 1; }
      /* a BOXED argument holds whichever variant the value really is, which the
         receiver's layout cannot be assumed to match: the same fold (#3975) */
      if (at == TY_POLY) { *out = TY_POLY_POLY_HASH; return 1; }
      { *out = rt; return 1; }
    }
    /* replace(other) returns self, but self's SLOT widens to the universal
       PolyPoly hash when other is a different variant (the receiver-mutation
       evidence in infer_write_types does the same to the local) -- mirror it
       here so a `r = h.replace(o)` capture agrees from the first fixpoint
       iteration instead of lagging one phase behind (#2374 hang: a SymPoly-
       declared r holding a PolyPoly pointer made inspect walk garbage). */
    if (sp_streq(name, "replace") && argc == 1) {
      TyKind ot = infer_type(c, argv[0]);
      if (ty_is_hash(ot) && ot != rt) { *out = TY_POLY_POLY_HASH; return 1; }
      { *out = rt; return 1; }
    }
    if (sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "merge")) { *out = rt; return 1; }
    /* #2340/#2349/#2351: no-arg merge / slice / clear / to_hash / rehash keep
       the receiver's variant (an emptied or copied hash of the same shape) */
    if ((sp_streq(name, "merge") || sp_streq(name, "slice")) && argc == 0) { *out = rt; return 1; }
    if ((sp_streq(name, "clear") || sp_streq(name, "to_hash") || sp_streq(name, "rehash")) && argc == 0)
      { *out = rt; return 1; }
    /* Hash#shift -> [key, value] pair, or nil (boxed poly) (#2349) */
    if (sp_streq(name, "shift") && argc == 0) { *out = TY_POLY; return 1; }
    /* Hash#key(value) -> a key, or nil: poly (the key type, nullable) (#2352) */
    if (sp_streq(name, "key") && argc == 1) { *out = TY_POLY; return 1; }
    /* blockless one? -> bool (exactly one pair) (#2354) */
    if (sp_streq(name, "one?") && argc == 0 && nt_ref(nt, id, "block") < 0) { *out = TY_BOOL; return 1; }
    /* in-place merge mutates and returns the receiver (its variant is fixed) */
    if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc >= 1) { *out = rt; return 1; }
    if (sp_streq(name, "has_key?") || sp_streq(name, "key?") ||
        sp_streq(name, "include?") || sp_streq(name, "member?") ||
        sp_streq(name, "has_value?") || sp_streq(name, "value?") ||
        sp_streq(name, "empty?")) { *out = TY_BOOL; return 1; }
    /* blockless each_with_object -> Enumerator (#2540); the blocked form is a
       Ruby method by now (builtins/enumerable.rb) and types as one */
    if (sp_streq(name, "each_with_object") && argc > 0 && argv && nt_ref(nt, id, "block") < 0)
      { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "flatten") && argc <= 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "invert") && argc == 0) {
      /* swap key/value types where we have a typed variant */
      if (rt == TY_STR_STR_HASH) { *out = TY_STR_STR_HASH; return 1; }
      { *out = TY_POLY_POLY_HASH; return 1; }
    }
    if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "compact") && argc == 0) { *out = rt; return 1; }
    if (sp_streq(name, "except")) { *out = rt; return 1; }  /* a copy minus the given keys */
  }
  return 0;
}

/* Array receivers: the array face of infer_call */
int infer_array_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv; (void)nt;
  if (!name) return 0;
  /* infer_call computes this once for every arm below it; the arms moved here
     read it. infer_type is memoized, so recomputing it is the same answer. */
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
  (void)a0;
  if (recv >= 0 && ty_is_array(rt)) {
    int block = nt_ref(nt, id, "block");
    /* arr.each with no block -> an external Enumerator (#next/#peek/#rewind).
       Block-form chains (each.with_index, each.map) are matched as the outer
       call above and never reach this. */
    if (block < 0 && argc == 0 &&
        (sp_streq(name, "each") || sp_streq(name, "reverse_each") ||
         sp_streq(name, "each_entry") ||
         sp_streq(name, "each_with_index") || sp_streq(name, "each_index"))) { *out = TY_ENUMERATOR; return 1; }
    /* a blockless map/collect is a usable Enumerator too (size/class/next);
       chained block forms (map.with_index { }) are typed by their own arms
       before this one. */
    if (block < 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "map") || sp_streq(name, "collect") ||
         sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "find_all") || sp_streq(name, "reject") ||
         /* the rest of the block-taking Enumerables answer one too (#3757) */
         sp_streq(name, "sort_by") || sp_streq(name, "group_by") ||
         sp_streq(name, "min_by") || sp_streq(name, "max_by") ||
         sp_streq(name, "find") || sp_streq(name, "detect") ||
         sp_streq(name, "flat_map") || sp_streq(name, "collect_concat") ||
         sp_streq(name, "filter_map") || sp_streq(name, "partition") ||
         sp_streq(name, "take_while") || sp_streq(name, "drop_while") ||
         sp_streq(name, "find_index") || sp_streq(name, "chunk_while") ||
         sp_streq(name, "minmax_by")) &&
        !call_is_chain_receiver_with_block(c, id)) { *out = TY_ENUMERATOR; return 1; }
    /* arr.each_slice(n) / arr.each_cons(n) with no block -> a materialized
       Enumerator of slices / windows. The direct-block form has block >= 0 and
       is excluded; a .map/.collect chain consumer is typed by its own arm above
       (which accepts this TY_ENUMERATOR receiver and keeps the array result).
       cycle(n) and slice_before/slice_after(pattern) materialize the same way. */
    if (block < 0 && argc == 1 &&
        (sp_streq(name, "each_slice") || sp_streq(name, "each_cons") ||
         sp_streq(name, "cycle") ||
         sp_streq(name, "slice_before") || sp_streq(name, "slice_after"))) { *out = TY_ENUMERATOR; return 1; }
    /* chunk { } with no chained consumer is an enumerator of [key, run]
       pairs; the desugar interposes to_a so .map/.count chains compose. */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "chunk")) { *out = TY_ENUMERATOR; return 1; }
    if (block >= 0) {
      if (ty_iter_shape(name) == TY_ITER_MAP)
        { *out = infer_map_block_ty(c, id, block); return 1; }
      if (sp_streq(name, "to_h") && argc == 0) {
        /* array.to_h { |x| [k, v] } -> a boxed-value hash, keyed by the
           block's [k, v] tail-pair key type (string/symbol get their own
           hash kind; anything else falls back to a fully boxed hash). */
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        int tail = bn > 0 ? bb[bn - 1] : -1;
        const char *tty = tail >= 0 ? nt_type(nt, tail) : NULL;
        if (tty && sp_streq(tty, "ArrayNode")) {
          int en = 0; const int *el = nt_arr(nt, tail, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, el[0]);
            if (kt == TY_SYMBOL) { *out = TY_SYM_POLY_HASH; return 1; }
            if (kt == TY_STRING) { *out = TY_STR_POLY_HASH; return 1; }
            /* the key's type hasn't settled yet (mid-fixpoint) -- stay
               unknown rather than locking in poly_poly_hash prematurely,
               which would then never narrow once kt resolves. */
            if (kt == TY_UNKNOWN) { *out = TY_UNKNOWN; return 1; }
          }
        }
        { *out = TY_POLY_POLY_HASH; return 1; }
      }
      if (sp_streq(name, "select") || sp_streq(name, "reject") ||
          sp_streq(name, "filter") || sp_streq(name, "find_all") ||
          sp_streq(name, "sort_by") ||
          sp_streq(name, "sort_by!"))
        { *out = rt; return 1; }
      if ((sp_streq(name, "find") || sp_streq(name, "detect")) && argc >= 1)
        { *out = TY_POLY; return 1; }  /* find(ifnone): the element or the proc's value */
      if (sp_streq(name, "find") || sp_streq(name, "detect"))
        { *out = ty_array_elem(rt); return 1; }  /* returns an element */
    }
    /* grep/grep_v without a block filter by `pattern === e`, preserving the
       receiver's array type. */
    if ((sp_streq(name, "grep") || sp_streq(name, "grep_v")) && argc == 1) {
      int gblk = nt_ref(nt, id, "block");
      if (gblk < 0) { *out = rt; return 1; }
      /* block form: an array of the block's results */
      int gbd = nt_ref(nt, gblk, "body");
      int gbn = 0; const int *gbb = gbd >= 0 ? nt_arr(nt, gbd, "body", &gbn) : NULL;
      TyKind gbt = gbn > 0 ? infer_type(c, gbb[gbn - 1]) : TY_UNKNOWN;
      { *out = gbt == TY_INT ? TY_INT_ARRAY : gbt == TY_FLOAT ? TY_FLOAT_ARRAY
           : gbt == TY_STRING ? TY_STR_ARRAY : TY_POLY_ARRAY; return 1; }
    }
    if (sp_streq(name, "[]")) {
      /* arr[range] / arr[start, len] -> a subarray; arr[i] -> an element. The
         range index may be a literal RangeNode or a variable/param typed
         TY_RANGE. */
      if (argc == 2) { *out = rt; return 1; }
      if (argc == 1 && ((nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) ||
                        a0 == TY_RANGE)) { *out = rt; return 1; }
      { *out = ty_array_elem(rt); return 1; }
    }
    if (sp_streq(name, "at") && argc == 1) { *out = ty_array_elem(rt); return 1; }  /* like [i] */
    if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
      TyKind et = ty_array_elem(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        { *out = (dt == TY_UNKNOWN || dt == et) ? et : TY_POLY; return 1; }
      }
      int fblk = nt_ref(nt, id, "block");
      if (fblk >= 0) {
        int fb = nt_ref(nt, fblk, "body");
        int fn = 0; const int *fbb = fb >= 0 ? nt_arr(nt, fb, "body", &fn) : NULL;
        TyKind bt = fn > 0 ? infer_type(c, fbb[fn - 1]) : TY_NIL;
        { *out = (bt == TY_UNKNOWN || bt == et) ? et : TY_POLY; return 1; }
      }
      { *out = et; return 1; }
    }
    if (sp_streq(name, "dig") && argc >= 1) {
      /* dig(*keys) walks a runtime key list: the depth is unknown here */
      if (argc == 1 && nt_kind(nt, argv[0]) != NK_SplatNode) { *out = ty_array_elem(rt); return 1; }
      { *out = TY_POLY; return 1; }
    }
    /* index returns nil on a miss -> poly (int-or-nil) */
    if ((sp_streq(name, "index") || sp_streq(name, "find_index") || sp_streq(name, "rindex")) &&
        (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) { *out = TY_POLY; return 1; }
    if (sp_streq(name, "length") || sp_streq(name, "size") ||
        sp_streq(name, "count") || sp_streq(name, "index") || sp_streq(name, "find_index")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "sum")) {
      int blk = nt_ref(nt, id, "block");
      /* Strings summed from anything but a String seed only ever raise, and an
         EMPTY receiver answers the seed itself -- so the call is that union,
         boxed. Typing it from the seed put an Integer 0 in a String slot, and
         the codegen arm below emitted a call to an sp_StrArray_sum that does
         not exist (#4327). */
      if (rt == TY_STR_ARRAY && blk < 0 &&
          !(argc == 1 && infer_type(c, argv[0]) == TY_STRING))
        { *out = TY_POLY; return 1; }
      /* A blockless seed keeps its OWN class through the whole fold: CRuby's
         accumulator is the seed object and every step is Ruby's `+` on it. Only
         a seed of the element's own class can live in the element's C slot --
         plus an Integer seed over Floats, and a Float seed over Integers, both
         of which CRuby answers as a Float too. Everything else folds boxed and
         the call IS that boxed value: a Rational total, a Bignum that no longer
         wraps, or the raise the generic `+` produces. Only the three
         concretely-typed kinds are decided here; a poly array keeps the
         sp_PolyArray_sum_* typings its own emitter arms expect. */
      if (argc == 1 && blk < 0 &&
          (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
        TyKind st = fold_seed_infer_ty(c, argv[0]);
        TyKind et = ty_array_elem(rt);
        /* the String-seed concatenation; the rule above took every other seed */
        if (rt == TY_STR_ARRAY) { *out = TY_STRING; return 1; }
        if (et == TY_INT && st == TY_FLOAT) { *out = TY_FLOAT; return 1; }
        *out = fold_seed_typed(st, et) ? et : TY_POLY;
        return 1;
      }
      /* a float initial value promotes the whole sum to Float (e.g.
         ints.sum(0.0) or ints.sum(0.0) { |x| x }), regardless of the block. */
      if (argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) { *out = TY_FLOAT; return 1; }
      /* a String / Array initial value folds by concatenation */
      if (argc == 1 && infer_type(c, argv[0]) == TY_STRING) { *out = TY_STRING; return 1; }
      if (argc == 1 && blk < 0) {
        TyKind sit = infer_type(c, argv[0]);
        if (sit == TY_STRING) { *out = TY_STRING; return 1; }
        if (ty_is_array(sit) ||
            (sit == TY_UNKNOWN && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")))
          { *out = TY_POLY_ARRAY; return 1; }
      }
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind st = bn > 0 ? infer_type(c, bb[bn - 1]) : ty_array_elem(rt);
        /* A block whose value is nil accumulates BOXED: the sum is the init
           (0) for an empty receiver and a TypeError for a non-empty one --
           never nil. Typing it nil let the call constant-fold away, so
           `[].sum {}` printed "nil" instead of 0 (#4006). */
        if (st == TY_NIL || st == TY_VOID) st = TY_POLY;
        /* A block value that has no `+` at all -- true, a Symbol -- can only
           raise: CRuby answers TypeError from `0 + true`. The sum still folds,
           boxed, so that the raise happens; typing the CALL as the block's kind
           put the sp_RbVal accumulator in a Boolean slot and the C compiler
           refused it (#4327). */
        if (st == TY_BOOL || st == TY_SYMBOL) st = TY_POLY;
        { *out = st; return 1; }
      }
      { *out = ty_array_elem(rt); return 1; }
    }
    if (sp_streq(name, "inject") || sp_streq(name, "reduce")) {
      /* inject(&:&|:||:-) over a literal array of int arrays: set operation
         folding the inner arrays -> an int array. */
      if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
        int blk = nt_ref(nt, id, "block");
        const char *sop = NULL;
        if (blk >= 0 && nt_type(nt, blk) && sp_streq(nt_type(nt, blk), "BlockArgumentNode")) {
          int ex = nt_ref(nt, blk, "expression");
          if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
        }
        if (sop && (sp_streq(sop, "&") || sp_streq(sop, "|") || sp_streq(sop, "-"))) { *out = TY_INT_ARRAY; return 1; }
      }
      /* When an init argument is provided, the return type matches the init type.
         inject(:op) is the no-init operator form -- the sole symbol arg is the
         operator, NOT an init value, so skip the "return argv[0] type" path. */
      if (argc > 0 && argv) {
        /* A runtime (non-literal) symbol operator -- `reduce(sym)` or
           `reduce(init, sym)` where sym is a `|sym|` block param -- folds
           through sp_poly_binop_sym, yielding a boxed poly regardless of the
           init type. (The block-fold form uses the block, not a sym operator.) */
        if (nt_ref(nt, id, "block") < 0 && !sym_static_value(c, argv[argc - 1])) {
          TyKind opt = infer_type(c, argv[argc - 1]);
          if (opt == TY_SYMBOL || opt == TY_POLY) { *out = TY_POLY; return 1; }
        }
        /* `reduce(id, :>>)` composes callables: the fold runs through the
           generic symbol-operator helper, whose result is boxed, so typing it
           from the Proc seed left `.call` reading a boxed value as an
           sp_Proc * (#3884). */
        if (argc == 2 && nt_ref(nt, id, "block") < 0) {
          const char *sv2 = sym_static_value(c, argv[1]);
          if (sv2 && (sp_streq(sv2, ">>") || sp_streq(sv2, "<<")) &&
              infer_type(c, argv[0]) == TY_PROC) { *out = TY_POLY; return 1; }
        }
        const char *a0ty = nt_type(nt, argv[0]);
        /* a symbol literal, or a local statically holding one (s = :+) -- and
           only when there is no block: `inject(:s, &:+)` gives the operator in
           the block, which makes the lone Symbol the SEED, not the operator
           (Enumerable#inject's own rule). Read as an operator it typed the
           call from the elements while the emitter folded from the seed. */
        int is_sym_op = argc == 1 && nt_ref(nt, id, "block") < 0 &&
                        sym_static_value(c, argv[0]) != NULL;
        (void)a0ty;
        /* `reduce(nil) { |acc, t| acc.nil? ? t : acc + t }` -- the idiom for a
           fold with no natural identity. The seed says nothing about the
           accumulator except that it starts empty, so the fold is boxed: a
           TY_NIL accumulator has no C representation at all (#3356). */
        if (!is_sym_op && nt_ref(nt, id, "block") >= 0 &&
            nt_kind(nt, argv[0]) == NK_NilNode)
          { *out = TY_POLY; return 1; }
        if (!is_sym_op) {
          TyKind it = infer_type(c, argv[0]);
          /* An empty array-literal seed accumulates an array: poly, since the
             block decides the element mix (`reduce([]) { |a, x| a << x }`). */
          if (it == TY_UNKNOWN && a0ty && sp_streq(a0ty, "ArrayNode")) {
            int sen = 0; nt_arr(nt, argv[0], "elements", &sen);
            if (sen == 0) it = TY_POLY_ARRAY;
          }
          /* An empty `{}` seed accumulates a general boxed-key/value hash, like
             each_with_object({}); the block decides the key/value mix (#2958). */
          else if (it == TY_UNKNOWN && a0ty && sp_streq(a0ty, "HashNode")) {
            int sen = 0; nt_arr(nt, argv[0], "elements", &sen);
            if (sen == 0) it = TY_POLY_POLY_HASH;
          }
          if (it != TY_UNKNOWN) {
            /* The accumulator is reassigned to the block body each iteration,
               so an int seed folded over floats accumulates float. An array
               seed never numeric-promotes (`a << x` pre-types as an int shift
               before the accumulator's type shadows the block param). */
            int rblk = nt_ref(nt, id, "block");
            int rbody = rblk >= 0 ? nt_ref(nt, rblk, "body") : -1;
            int rbn = 0; const int *rbb = rbody >= 0 ? nt_arr(nt, rbody, "body", &rbn) : NULL;
            if (rbn > 0 && !ty_is_array(it) && !ty_is_hash(it)) {
              TyKind bt = infer_type(c, rbb[rbn - 1]);
              if (ty_is_numeric(bt)) it = ty_promote_numeric(it, bt);
              /* A boxed block result (a poly element mixing into the fold, as
                 on a user Enumerable's materialized element array) cannot be
                 narrowed back to the seed's type without truncating: an int
                 seed folded over floats came out an Integer (#2982). */
              else if (it != TY_POLY && ty_array_elem(rt) == TY_POLY &&
                       (bt == TY_POLY || ty_is_object(bt) || bt == TY_RATIONAL ||
                        bt == TY_COMPLEX || bt == TY_BIGINT)) it = TY_POLY;
              /* a block yielding a boxed value (a Rational/Complex, or poly
                 because a fold OPERAND is poly -- a parameter called with
                 Integer and Rational call sites) cannot fold back into a
                 scalar numeric seed slot, even over an int/float array
                 (#3220, #3308) */
              else if (it != TY_POLY && ty_is_numeric(it) &&
                       (bt == TY_RATIONAL || bt == TY_COMPLEX ||
                        bt == TY_POLY || bt == TY_BIGINT)) it = TY_POLY;
            }
            /* An ARRAY seed is reassigned to the block's value too. A boxed
               result -- `a + r` over a poly element -- cannot go back into the
               seed's typed array slot, and the C compiler rejected the whole
               program; an empty `[]` seed escaped only because it already types
               poly (#3854). */
            if (rbn > 0 && ty_is_array(it) && it != TY_POLY_ARRAY) {
              TyKind abt = infer_type(c, rbb[rbn - 1]);
              if (abt == TY_POLY) it = TY_POLY;
              else if (ty_is_array(abt) && abt != it) it = TY_POLY_ARRAY;
            }
            /* reduce(init, :op) symbol-operator form has no block, so the block
               promotion above cannot fire: an int seed folded over floats with
               `:+` still came out Integer. Promote a numeric seed by the element
               type when the operator is arithmetic (#3181). */
            else if (rbn == 0 && ty_is_numeric(it)) {
              const char *sop = fold_seed_op_sym(c, id, argc, argv);
              TyKind et = ty_array_elem(rt);
              int arith = sop && (sp_streq(sop, "+") || sp_streq(sop, "-") || sp_streq(sop, "*") ||
                                  sp_streq(sop, "/") || sp_streq(sop, "%") || sp_streq(sop, "**"));
              /* A seed of another class is not an accumulator this element type
                 can hold at all: `[1, 2, 3].reduce(0.5, :+)` accumulates Float
                 and a Bignum seed does not fit an sp_int. Those fold boxed, and
                 the promotion below then only ever widens Integer to Float. An
                 ERASED element type says nothing about the seed's class, so it
                 is left to the two rules below, which have their own answer. */
              if (sop && et != TY_POLY && !fold_seed_typed(it, et)) it = TY_POLY;
              else if (arith && ty_is_numeric(et))
                it = ty_promote_numeric(it, et);
              /* fold over a literal array of boxed Rationals/Complex with an
                 arithmetic operator: the accumulator promotes to the boxed
                 element and the result is a boxed poly (#3220). A plain poly
                 array's element type is unknown, so gate on the literal. */
              else if (arith && rt == TY_POLY_ARRAY && recv >= 0 &&
                       nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
                int ren = 0; const int *rev = nt_arr(nt, recv, "elements", &ren);
                int boxed_num = 0;
                for (int e = 0; rev && e < ren; e++) {
                  TyKind eet = infer_type(c, rev[e]);
                  if (eet == TY_RATIONAL || eet == TY_COMPLEX) { boxed_num = 1; break; }
                }
                if (boxed_num) it = TY_POLY;
              }
              /* fold over a poly array whose element type is erased (e.g. from
                 a zip.map): the elements may be Float/Rational/etc., so an int
                 seed cannot unbox the boxed accumulator without reinterpreting
                 the bits. Keep the result boxed (#3238). */
              else if (arith && et == TY_POLY) it = TY_POLY;
            }
            /* The same rule for a seed that is not a number at all -- a String,
               nil, true/false, a Rational. `[1, 2].reduce("x", :*)` accumulates
               a String ("xx"), and `[1, 2].reduce(nil, :+)` raises from nil's
               own missing `+`; both were read out of an sp_int slot, so nil
               came back as the sum and a String stopped the C build. */
            else if (rbn == 0 && fold_seed_op_sym(c, id, argc, argv) &&
                     ty_array_elem(rt) != TY_POLY &&
                     !fold_seed_typed(it, ty_array_elem(rt)))
              it = TY_POLY;
            /* A hash/array/object seed whose block body reassigns the
               accumulator to a boxed poly value (e.g. a method that returns
               poly because it is also used in a poly context elsewhere) widens
               the fold to poly: the concrete seed slot cannot hold the boxed
               block result each iteration (#3240). A body that simply returns
               the accumulator param (`h[x]=...; h`) is the seed's own type --
               it only infers poly because the block param is poly -- so it must
               not trip the widening. */
            if (rbn > 0 && it != TY_POLY &&
                (ty_is_hash(it) || ty_is_array(it) || ty_is_object(it))) {
              int rblk2 = nt_ref(nt, id, "block");
              const char *accp = rblk2 >= 0 ? block_param_name(c, rblk2, 0) : NULL;
              if (!reduce_tail_from_acc(c, rbb[rbn - 1], accp) &&
                  infer_type(c, rbb[rbn - 1]) == TY_POLY)
                it = TY_POLY;
            }
            { *out = it; return 1; }
          }
        }
      }
      /* empty array literal `[]` with sym op: codegen treats as int_array → returns int */
      if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; nt_arr(nt, recv, "elements", &en);
        if (en == 0) { *out = TY_INT; return 1; }
      }
      /* Block body last expression determines the return type when available. */
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn > 0) {
          TyKind bt = infer_type(c, bb[bn - 1]);
          /* With no init the accumulator starts as the first element, so a
             block value shaped differently than that element rides boxed: a
             fold of Hashes through #merge cannot store a hash pointer in the
             boxed slot the first element occupies. */
          if (bt != TY_UNKNOWN && bt != TY_POLY && ty_array_elem(rt) == TY_POLY) { *out = TY_POLY; return 1; }
          if (bt != TY_UNKNOWN) { *out = bt; return 1; }
          /* Settled and still unknown: the body is an unresolved call, whose
             value is the boxed NoMethodError raise (`inject(:nope)` desugars
             to one). The element-typed accumulator cannot take that, and the
             program did not build (#3831). While inference is still
             optimistic this only means "not yet typed". */
          if (!g_infer_optimistic) { *out = TY_POLY; return 1; }
        }
      }
      { *out = ty_array_elem(rt); return 1; }
    }
    /* blockless each_with_object -> Enumerator (#2540); the blocked form is a
       Ruby method by now (builtins/enumerable.rb) and types as one */
    if (sp_streq(name, "each_with_object") && argc > 0 && argv && nt_ref(nt, id, "block") < 0)
      { *out = TY_ENUMERATOR; return 1; }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) { *out = rt; return 1; }  /* first(n)/last(n) -> subarray */
    /* `arr.take(n)`/`drop(n)` is a subarray, but `arr.lazy.take(n)` stays a lazy
       stage -- let the lazy pipeline (below) type the forced chain, not this
       eager subarray arm. */
    {
      int rcv_is_lazy = recv >= 0 && nt_type(nt, recv) &&
                        sp_streq(nt_type(nt, recv), "CallNode") &&
                        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "lazy");
      if ((sp_streq(name, "drop") || sp_streq(name, "take")) && argc == 1 && !rcv_is_lazy)
        { *out = rt; return 1; }  /* subarray */
    }
    /* min(n)/max(n) (no comparator block) take the n extreme elements -> a
       subarray; sample(n) likewise. With a comparator block the n-arg form is
       not lowered, so don't type it as an array (that would mis-drive codegen
       into returning a scalar through an array type) -- leave it to reject. */
    /* sample(random: rng) is the single-element form, not a count -- a sole
       keyword-hash arg selects one element (falls to the element arm) (#2970) */
    int sample_kw = sp_streq(name, "sample") && argc == 1 && argv &&
                    nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode");
    if (((sp_streq(name, "min") || sp_streq(name, "max")) && block < 0 && argc == 1) ||
        (sp_streq(name, "sample") && argc == 1 && !sample_kw))
      { *out = rt; return 1; }  /* n-arg form -> subarray */
    if (sp_streq(name, "slice") && argc == 2) { *out = rt; return 1; }
    if ((sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 1)
      { *out = rt; return 1; }  /* pop(n)/shift(n): the removed subarray */
    /* a countless blockless cycle is an Enumerator too (#3758) */
    if (sp_streq(name, "cycle") && argc == 0 && nt_ref(nt, id, "block") < 0 &&
        !call_is_chain_receiver_with_block(c, id))
      { *out = TY_ENUMERATOR; return 1; }
    if (sp_streq(name, "cycle") && argc == 1 && nt_ref(nt, id, "block") < 0)
      { *out = rt; return 1; }  /* blockless cycle(n): the receiver repeated n times */
    if (sp_streq(name, "cycle") && nt_ref(nt, id, "block") >= 0)
      { *out = TY_NIL; return 1; }  /* the block form returns nil (a valued break widens) */
    if (sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "sample") ||
        sp_streq(name, "pop") || sp_streq(name, "shift")) { *out = ty_array_elem(rt); return 1; }
    if (sp_streq(name, "minmax")) { *out = rt; return 1; }  /* [min, max], same element kind */
    if (sp_streq(name, "join"))                        { *out = TY_STRING; return 1; }
    if (sp_streq(name, "pack") && argc == 1)           { *out = TY_STRING; return 1; }
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "empty?") || sp_streq(name, "include?")) { *out = TY_BOOL; return 1; }
    if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
         sp_streq(name, "none?") || sp_streq(name, "one?")) && argc <= 1) { *out = TY_BOOL; return 1; }
    if ((sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0 && argc >= 1)
      { *out = TY_POLY; return 1; }  /* find(ifnone): the element or the proc's value */
    if ((sp_streq(name, "bsearch") || sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0)
      { *out = ty_array_elem(rt); return 1; }  /* element or nil */
    if (sp_streq(name, "bsearch_index") && block >= 0) { *out = TY_INT; return 1; }  /* index, or nil */
    if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && block >= 0) {
      /* Typed arrays (int/str/float): in-place mutation preserves element type.
         The block param may be widened to TY_POLY when shared with other blocks,
         but the array type is determined by the receiver, not the block body. */
      if (ty_array_elem(rt) != TY_POLY)
        { *out = rt; return 1; }
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind bt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
      { *out = bt != TY_UNKNOWN ? ty_array_of(bt) : rt; return 1; }
    }
    if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "reject!")) &&
        block >= 0) { *out = TY_POLY; return 1; }  /* self, or nil when nothing was removed */
    if (sp_streq(name, "flatten!") && argc == 1) { *out = TY_POLY; return 1; }   /* self or nil */
    if (sp_streq(name, "flatten") && argc == 1)
      { *out = rt == TY_POLY_ARRAY ? TY_POLY_ARRAY : rt; return 1; }  /* typed arrays have no nesting */
    if ((sp_streq(name, "uniq!") || sp_streq(name, "compact!") || sp_streq(name, "flatten!")) &&
        argc == 0 && block < 0) { *out = TY_POLY; return 1; }  /* self, or nil when a no-op */
    if ((sp_streq(name, "keep_if") || sp_streq(name, "delete_if")) && block >= 0)
      { *out = rt; return 1; }  /* always self */
    if (sp_streq(name, "find_index") || sp_streq(name, "index") || sp_streq(name, "rindex")) { *out = TY_INT; return 1; }  /* int or nil */
    if (sp_streq(name, "each_index")) { *out = rt; return 1; }
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append") ||
         sp_streq(name, "unshift") || sp_streq(name, "prepend")) &&
        argc >= 1 && argv && rt != TY_POLY_ARRAY && ty_array_elem(rt) != TY_UNKNOWN) {
      /* Heterogeneous push/unshift on a typed-array literal: lift to poly. */
      TyKind elem_t = ty_array_elem(rt);
      const char *rty = nt_type(nt, recv);
      if (rty && sp_streq(rty, "ArrayNode")) {
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = infer_type(c, argv[ai]);
          if (at != TY_UNKNOWN && at != elem_t) { *out = TY_POLY_ARRAY; return 1; }
        }
      }
      { *out = rt; return 1; }
    }
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append") ||
        sp_streq(name, "reverse") || sp_streq(name, "sort") || sp_streq(name, "uniq") ||
        sp_streq(name, "to_a") || sp_streq(name, "to_ary") || sp_streq(name, "deconstruct") ||
        sp_streq(name, "entries") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "compact") || sp_streq(name, "flatten") || sp_streq(name, "clear") ||
        sp_streq(name, "transpose") ||
        sp_streq(name, "shuffle") ||
        (sp_streq(name, "union") && argc == 0) ||
        sp_streq(name, "reverse!") || sp_streq(name, "sort!") || sp_streq(name, "shuffle!") ||

        sp_streq(name, "rotate!") || sp_streq(name, "rotate") || sp_streq(name, "insert") || sp_streq(name, "unshift") || sp_streq(name, "prepend") || sp_streq(name, "concat") || sp_streq(name, "freeze") ||
        (sp_streq(name, "fill") && ((block < 0 && argc >= 1 && argc <= 3 &&
                                     /* a fill VALUE incompatible with the element type
                                        makes the result a poly array (see below) */
                                     ({ TyKind _fv = infer_type(c, argv[0]);
                                        TyKind _fe = ty_array_elem(rt);
                                        _fe == TY_POLY || _fv == TY_UNKNOWN || _fv == TY_POLY || _fv == _fe ||
                                        (ty_is_numeric(_fv) && ty_is_numeric(_fe)); })) ||
                                    (block >= 0 && argc <= 2))) ||
        sp_streq(name, "replace") ||
        sp_streq(name, "values_at") ||
        (sp_streq(name, "fetch_values") && block < 0)) { *out = rt; return 1; }
    /* the block form mixes fallback values in -> poly array (#2368) */
    if (sp_streq(name, "fetch_values") && block >= 0) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "fill") && block < 0 && argc >= 1 && argc <= 3)
      { *out = TY_POLY_ARRAY; return 1; }   /* the incompatible-value fill fell through above */
    if (sp_streq(name, "zip") && block < 0) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "zip") && block >= 0) { *out = TY_NIL; return 1; }  /* block form returns nil */
    if (sp_streq(name, "product") && argc >= 1)
      { *out = nt_ref(nt, id, "block") >= 0 ? rt : TY_POLY_ARRAY; return 1; }  /* block form returns self */
    if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) { *out = TY_POLY_ARRAY; return 1; }
    /* blockless: an Enumerator over the tuples, as CRuby answers. A poly-array
       receiver keeps the materialized Array -- chains reached through a
       container read it directly (#3614). */
    { TyKind cmb = rt == TY_POLY_ARRAY ? TY_POLY_ARRAY : TY_ENUMERATOR;
      if (sp_streq(name, "combination") && argc == 1 && block < 0) { *out = cmb; return 1; }
      if (sp_streq(name, "permutation") && (argc == 1 || argc == 0) && block < 0) { *out = cmb; return 1; }
      if (sp_streq(name, "repeated_permutation") && argc == 1 && block < 0) { *out = cmb; return 1; }
      if (sp_streq(name, "repeated_combination") && argc == 1 && block < 0) { *out = cmb; return 1; } }
    /* block forms: combination family returns self, each_slice/cons nil */
    if (block >= 0 &&
        (sp_streq(name, "combination") || sp_streq(name, "permutation") ||
         sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation")))
      { *out = rt; return 1; }
    if (block >= 0 && (sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && argc == 1)
      { *out = rt; return 1; }  /* Ruby >= 3.1: block form returns self */
    if (sp_streq(name, "frozen?")) { *out = TY_BOOL; return 1; }
    /* delete(v) { fallback }: the not-found block's value mixes in -> poly */
    if (sp_streq(name, "delete") && argc == 1 && nt_ref(nt, id, "block") >= 0)
      { *out = TY_POLY; return 1; }
    if ((sp_streq(name, "delete_at") || sp_streq(name, "delete")) && argc == 1)
      { *out = ty_array_elem(rt); return 1; }
    if (sp_streq(name, "shift") && argc == 0) { *out = ty_array_elem(rt); return 1; }
    if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) { *out = rt; return 1; }  /* removed subarray */
    if (sp_streq(name, "slice") && argc == 1 && nt_ref(nt, id, "block") < 0)
      /* slice(range) is a subarray; slice(i) one element (mirrors #[]) */
      { *out = infer_type(c, argv[0]) == TY_RANGE ? rt : ty_array_elem(rt); return 1; }
    if (sp_streq(name, "slice!") && argc == 2) { *out = rt; return 1; }  /* removed subarray */
    if (sp_streq(name, "slice!") && argc == 1)
      /* slice!(range) removes a subarray; slice!(i) removes one element */
      { *out = infer_type(c, argv[0]) == TY_RANGE ? rt : ty_array_elem(rt); return 1; }
    /* a[i] = v -> v's type (== element type); a[range] = v / a[s,l] = v is a
       splice and returns the RHS as written. The poly-array splice emitter
       yields the RHS BOXED (its `_t` temp is an sp_RbVal), so a poly receiver
       infers TY_POLY; a typed receiver's emitter yields the raw RHS value. */
    if (sp_streq(name, "[]=") && argc == 2) {
      if (infer_type(c, argv[0]) == TY_RANGE)
        { *out = rt == TY_POLY_ARRAY ? TY_POLY : infer_type(c, argv[1]); return 1; }
      { *out = ty_array_elem(rt); return 1; }
    }
    if (sp_streq(name, "[]=") && argc == 3)
      { *out = rt == TY_POLY_ARRAY ? TY_POLY : infer_type(c, argv[2]); return 1; }
    if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && rt == TY_POLY_ARRAY)
      { *out = TY_POLY_ARRAY; return 1; }  /* the matching sub-array, or nil (NULL ptr) */
    if (sp_streq(name, "to_h") && argc == 0 && block < 0) {
      /* Infer hash type from the first pair element of an array literal */
      if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, recv, "elements", &en);
        if (en > 0 && nt_type(nt, els[0]) && sp_streq(nt_type(nt, els[0]), "ArrayNode")) {
          int en2 = 0; const int *els2 = nt_arr(nt, els[0], "elements", &en2);
          if (en2 >= 2) {
            TyKind kt = infer_type(c, els2[0]);
            TyKind vt = infer_type(c, els2[1]);
            if (kt == TY_SYMBOL) { *out = TY_SYM_POLY_HASH; return 1; }
            if (kt == TY_STRING) {
              TyKind h = ty_hash_of(TY_STRING, vt);
              { *out = h != TY_UNKNOWN ? h : TY_STR_POLY_HASH; return 1; }
            }
            TyKind h = ty_hash_of(kt, vt);
            if (h != TY_UNKNOWN) { *out = h; return 1; }
          }
        }
      }
      /* Non-literal receiver: the pair element types are not statically known
         (e.g. `a.to_h` for a method param), so a fully boxed hash preserves
         whatever keys/values the pairs hold instead of mis-typing them. */
      { *out = TY_POLY_POLY_HASH; return 1; }
    }
  }
  return 0;
}

/* Object receivers: the user-object face of infer_call */
int infer_object_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv; (void)nt;
  if (!name) return 0;
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    /* The class HAS subclasses and cannot answer this name, while some subclass
       can: a slot typed as that class is only its STATIC type -- an inherited
       method storing `self` types it as the class that DEFINED the method --
       so the call dispatches at run time and its value is that union (#4023).
       The emitter routes the same shape through the poly dispatch. */
    if (comp_method_in_chain(c, cid, name, NULL) < 0 &&
        !comp_reader_in_chain(c, cid, name, NULL) && !cls->is_native_class) {
      int sub_answers = 0;
      for (int k = 0; k < c->nclasses && !sub_answers; k++) {
        if (k == cid) continue;
        int anc = 0;
        for (int p2 = c->classes[k].parent; p2 >= 0; p2 = c->classes[p2].parent)
          if (p2 == cid) { anc = 1; break; }
        if (anc && (comp_method_in_chain(c, k, name, NULL) >= 0 ||
                    comp_reader_in_chain(c, k, name, NULL))) sub_answers = 1;
      }
      if (sub_answers) { *out = TY_POLY; return 1; }
    }
    if (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?") ||
        sp_streq(name, "respond_to?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "nil?") || sp_streq(name, "equal?") || sp_streq(name, "frozen?")) { *out = TY_BOOL; return 1; }
    /* Object#hash default (no user hash in the chain): value/pointer int.
       Structs keep their dedicated value-based hash arm. */
    if (sp_streq(name, "hash") && argc == 0 && !cls->is_struct &&
        comp_method_in_chain(c, cid, "hash", NULL) < 0) { *out = TY_INT; return 1; }
    /* native class (C-backed): a declared instance method returns its spec type */
    if (cls->is_native_class) {
      /* IO::Buffer#get_value with a LITERAL type symbol lowers to a typed
         accessor (the codegen fold); the site's type follows the symbol:
         floats are Float, u64 stays boxed (a value above 2^63-1 is a
         Bignum), every other integer type is a machine int. The offset may
         be boxed (the fold unboxes it), which is what every int local is
         under --int-overflow=promote. The conditions mirror the fold's
         exactly (a literal the fold declines keeps the generic binding's
         boxed type). */
      if (cls->c_struct && sp_streq(cls->c_struct, "sp_IOBuffer") &&
          sp_streq(name, "get_value") && argc == 2 &&
          nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "SymbolNode") &&
          (infer_type(c, argv[1]) == TY_INT || infer_type(c, argv[1]) == TY_POLY)) {
        int it = comp_iob_sym_type(nt_str(c->nt, argv[0], "value"));
        if (it >= 0) {
          *out = comp_iob_ty_is_float(it) ? TY_FLOAT
               : comp_iob_ty_is_64(it) ? TY_POLY : TY_INT;
          return 1;
        }
      }
      TyKind natys[8];
      int nta = argc < 8 ? argc : 8;
      for (int a = 0; a < nta; a++) natys[a] = infer_type(c, argv[a]);
      int nm = comp_native_method_find_typed(c, cid, name, argc, 0, nta == argc ? natys : NULL);
      if (nm >= 0) {
        if (sp_streq(c->native_methods[nm].ret, "self")) { *out = rt; return 1; }  /* returns the receiver's class */
        { *out = native_spec_to_ty(c->native_methods[nm].ret); return 1; }
      }
    }
    /* Comparable#clamp returns self or the APPLIED BOUND: the receiver's
       class only when each bound is statically that class or nil (a nil
       bound clamps one-sided and is never returned); a mixed-class or
       Integer-endpoint (range form) bound can be returned as-is, so the
       result is boxed. */
    if (sp_streq(name, "clamp") && argc == 2 &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) {
      TyKind lo = infer_type(c, argv[0]), hi = infer_type(c, argv[1]);
      { *out = ((lo == rt || lo == TY_NIL) && (hi == rt || hi == TY_NIL)) ? rt : TY_POLY; return 1; }
    }
    if (sp_streq(name, "clamp") && argc == 1 && infer_type(c, argv[0]) == TY_RANGE &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) {
      /* a literal range with same-class endpoints unfolds to the object
         clamp, whose result is one of the three same-class values */
      int rn2 = argv[0];
      while (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "ParenthesesNode")) {
        int pb2 = nt_ref(nt, rn2, "body"); int pbn2 = 0;
        const int *pbb2 = pb2 >= 0 ? nt_arr(nt, pb2, "body", &pbn2) : NULL;
        rn2 = pbn2 == 1 ? pbb2[0] : -1;
      }
      if (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "RangeNode")) {
        int rlo2 = nt_ref(nt, rn2, "left"), rhi2 = nt_ref(nt, rn2, "right");
        /* two-sided, beginless (`..hi`), and endless (`lo..`) object ranges all
           unfold to sp_obj_clamp, whose result is one of the same-class values;
           the missing side becomes a nil bound. Mirror the codegen guard. */
        int has_lo2 = rlo2 >= 0 && !(nt_type(nt, rlo2) && sp_streq(nt_type(nt, rlo2), "NilNode"));
        int has_hi2 = rhi2 >= 0 && !(nt_type(nt, rhi2) && sp_streq(nt_type(nt, rhi2), "NilNode"));
        int lo_obj2 = has_lo2 && infer_type(c, rlo2) == rt;
        int hi_obj2 = has_hi2 && infer_type(c, rhi2) == rt;
        if ((lo_obj2 || hi_obj2) && (!has_lo2 || lo_obj2) && (!has_hi2 || hi_obj2)) { *out = rt; return 1; }
      }
      { *out = TY_POLY; return 1; }
    }
    /* Comparable#between?(lo, hi) on an object with `<=>` is a boolean. */
    if (sp_streq(name, "between?") && argc == 2 &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) { *out = TY_BOOL; return 1; }
    /* default Object#<=> (no user method): 0 or nil, so poly (#2686). */
    if (sp_streq(name, "<=>") && argc == 1 &&
        comp_method_in_chain(c, cid, "<=>", NULL) < 0) { *out = TY_POLY; return 1; }
    /* instance_variable_get(:@x) yields @x's declared type; instance_variable_set
       yields the field type too (C `lvalue = v` evaluates to the lvalue). The
       codegen lowers both to a direct iv_ field access on the known layout. */
    if ((sp_streq(name, "instance_variable_get") || sp_streq(name, "instance_variable_set") ||
         sp_streq(name, "remove_instance_variable")) && argc >= 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) {
        const char *sym = sp_streq(a0ty, "SymbolNode")
                            ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
        /* A name in the layout yields its declared type; an undefined-but-valid
           `@`-name reads as nil and a bad name (no `@`) raises NameError -- both poly. */
        /* Data/Struct members are not @-ivars in CRuby: read as nil (#2849) */
        int iv = (sym && sym[0] == '@' && !cls->is_struct) ? comp_ivar_index(cls, sym) : -1;
        if (iv >= 0) { *out = ivar_value_ty(cls, iv); return 1; }
        { *out = TY_POLY; return 1; }
      }
    }
    /* attr reader (resolve alias so `alias v access_token` returns @access_token type) */
    { int rdcls = -1, mdcls = -1;
      /* An explicit `def x` at an equal-or-more-derived class overrides the
         attribute, and the read emitter already calls it. Take the type from
         whichever member wins the same arbitration; typing the call as the
         ivar while emitting a call to the override reinterprets the returned
         value as the attr's type (#3909). */
      int reader_wins = comp_resolve_member(c, cid, name, 0, &rdcls, NULL) == SP_MEMBER_ATTR;
      (void)mdcls;
      if (reader_wins) {
        const char *rname = comp_resolve_alias(c, cid, name);
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", rname);
        ClassInfo *rci = (rdcls >= 0 && rdcls < c->nclasses) ? &c->classes[rdcls] : cls;
        int iv = comp_ivar_index(rci, ivn);
        if (iv >= 0) { *out = ivar_value_ty(rci, iv); return 1; }
      }
    }
    /* attr writer: obj.x= returns the assigned value */
    size_t ln = strlen(name);
    if (ln >= 2 && name[ln - 1] == '=') {
      char base[256];
      if (ln - 1 < sizeof base) {
        memcpy(base, name, ln - 1); base[ln - 1] = '\0';
        int wdefc = -1;
        if (comp_writer_in_chain(c, cid, base, &wdefc) && argc >= 1) {
          TyKind rhsk = infer_type(c, argv[0]);
          /* codegen boxes a scalar rhs into a poly ivar slot, so the assignment
             expression's C value is that boxed poly -- report poly to match. */
          char wivn[258]; snprintf(wivn, sizeof wivn, "@%s", base);
          int wcid = wdefc < 0 ? cid : wdefc;
          int wivx = comp_ivar_index(&c->classes[wcid], wivn);
          TyKind wivt = wivx >= 0 ? c->classes[wcid].ivar_types[wivx] : TY_UNKNOWN;
          if (wivt == TY_POLY && rhsk != TY_POLY) { *out = TY_POLY; return 1; }
          { *out = rhsk; return 1; }
        }
        /* A hand-written `def x=(v)` is an assignment expression as well: its
           value is the argument as written, whatever the body returns (codegen
           passes the argument through a temp that is the expression's value).
           An argument not yet typed leaves the call to the method rule below,
           which the next round revisits. */
        if (argc == 1 && call_is_setter_assign(c->nt, id) &&
            comp_method_in_chain(c, cid, name, NULL) >= 0) {
          TyKind rhsk = infer_type(c, argv[0]);
          if (rhsk != TY_UNKNOWN) { *out = rhsk; return 1; }
        }
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      TyKind r = method_call_ret(c, mi, id);
      /* Unify with descendant direct overrides: codegen dispatch emits a
         cls_id switch over all overrides, so the result type must cover all. */
      int nd = 0; const int *ds = comp_descendants(c, cid, &nd);
      for (int di = 0; di < nd; di++) {
        int k = ds[di];
        int dmi = comp_method_in_class(c, k, name);
        if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
      }
      { *out = r; return 1; }
    }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) { *out = TY_STRING; return 1; }
  }
  return 0;
}

/* Boxed (poly) receivers: the run of poly-face arms of infer_call */
int infer_poly_call(Compiler *c, int id, TyKind rt, TyKind *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  (void)argv; (void)recv; (void)nt;
  if (!name) return 0;
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      !an_user_defines_method(c, name))
    { *out = TY_STRING; return 1; }
  /* #hash on a boxed receiver is always the Integer sp_rbval_hash_key
     answers -- a user #hash in the program is reached through
     sp_obj_hash_hook inside it and its answer is folded to the key -- so the
     static type is int whatever a user #hash of the program returns. Left to
     the user-method union it was the union's poly (every int is poly under
     --int-overflow=promote), and `h ^= x.hash` handed an sp_int to
     sp_poly_bitop's sp_RbVal parameter: `require "set"` did not build there,
     since Set#hash is exactly that loop (#4728). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && sp_streq(name, "hash"))
    { *out = TY_INT; return 1; }
  /* #name is a class name (a String) for a boxed Class and the method name (a
     Symbol) for a boxed Method, so where the program builds Method objects at
     all the static result is poly (#3692) */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && sp_streq(name, "name") &&
      !sp_feature_required("ostruct") && !an_user_defines_or_reads(c, name)) {
    if (an_program_builds_methods(c)) { *out = TY_POLY; return 1; }
    { *out = TY_STRING; return 1; }
  }
  /* The rest of the Method surface on a BOXED receiver: #unbind answers another
     Method-carrying value, #receiver the bound object and #to_proc a Proc --
     each boxed. Without a type the boxed result was dropped and read as nil
     (#3692). Guarded on the program building Method objects at all, since
     these names also belong to exceptions and procs. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "unbind") || sp_streq(name, "receiver")) &&
      an_program_builds_methods(c) && !an_user_defines_method(c, name))
    { *out = TY_POLY; return 1; }
  /* Numeric#fdiv on a boxed receiver is a Float whatever the operands are
     (#3767); without a type the boxed result was dropped and read as nil. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "fdiv") && !an_user_defines_or_reads(c, name))
    { *out = TY_FLOAT; return 1; }
  /* Complex#real / #imaginary on a poly value (a Complex read out of a
     container): the component is int- or float-classed at runtime, so the
     static result is poly. Without this the call typed nil and the boxed
     result was discarded (#2882). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag")) &&
      !an_user_defines_method(c, name))
    { *out = TY_POLY; return 1; }
  /* poly.delete_prefix / #delete_suffix answer a String, like the zero-arg
     transforms beside them (#3436). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix") ||
       sp_streq(name, "squeeze")) &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_STRING; return 1; }
  /* The String-only surface on a boxed receiver: the names no other class
     answers, so the result type is the one the typed String path gives. Names
     Array or Enumerable share (index, count, sum) stay untyped here and go
     through their runtime kind dispatch instead. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_or_reads(c, name)) {
    /* The Module reflection a class-tagged boxed value answers. Without a type
       here the call was UNKNOWN and everything chained onto it reported the
       method as undefined for "unknown" (#4018). */
    if (argc == 0 && (sp_streq(name, "ancestors") || sp_streq(name, "included_modules")))
      { *out = TY_POLY_ARRAY; return 1; }
    if (argc == 0 && sp_streq(name, "superclass")) { *out = TY_CLASS; return 1; }
    /* The class-side names the generated sp_cls_* answer: subclasses a class
       array; allocate an instance of a class only known at run time, and
       keyword_init? nil, true or false, boxed values. A class method of the
       name in the program keeps the type the dispatch joins from it. */
    if (argc == 0 && (sp_streq(name, "subclasses") || sp_streq(name, "allocate") ||
                      sp_streq(name, "keyword_init?"))) {
      int ncc = 0;
      comp_cmethod_candidates(c, name, &ncc);
      if (ncc == 0) { *out = sp_streq(name, "subclasses") ? TY_POLY_ARRAY : TY_POLY; return 1; }
    }
    if (argc == 0 && (sp_streq(name, "hex") || sp_streq(name, "oct"))) { *out = TY_INT; return 1; }
    if (argc == 0 && sp_streq(name, "squeeze")) { *out = TY_STRING; return 1; }
    /* casecmp / casecmp? are nil-or-answer, and which one is decided by the
       ARGUMENT: the emitter boxes the result when the argument is poly (a
       string compares, anything else is nil), so typing the call Integer/bool
       there handed the consumer an sp_RbVal to read unboxed and the C compiler
       reported it against generated code (#4004). Mirrors the typed-receiver
       rule in analyze_infer.c. */
    if (argc == 1 && (sp_streq(name, "casecmp") || sp_streq(name, "casecmp?"))) {
      TyKind at0 = argv ? infer_type(c, argv[0]) : TY_UNKNOWN;
      if (at0 == TY_POLY) { *out = TY_POLY; return 1; }
      /* an operand that answers #to_str converts and compares, and answers
         nil when its #to_str does -- the boxed result the emitter's arm
         gives. Same shape test, same answer, as the typed-receiver rule. */
      if (ty_is_object(at0) && class_has_to_str_shape(c, ty_object_class(at0))) {
        *out = TY_POLY;
        return 1;
      }
      if (at0 != TY_STRING && at0 != TY_UNKNOWN) { *out = TY_NIL; return 1; }
      *out = sp_streq(name, "casecmp") ? TY_INT : TY_BOOL;
      return 1;
    }
    if (argc <= 2 && argc >= 1 &&
        (sp_streq(name, "byteindex") || sp_streq(name, "byterindex"))) { *out = TY_INT; return 1; }
    if (argc == 1 && (sp_streq(name, "partition") || sp_streq(name, "rpartition")))
      { *out = TY_STR_ARRAY; return 1; }
    if (argc == 2 && sp_streq(name, "tr_s")) { *out = TY_STRING; return 1; }
    if (argc == 1 && sp_streq(name, "crypt")) { *out = TY_STRING; return 1; }
    /* #slice on a boxed receiver is answered by one emitter for ANY key count:
       it branches at run time between Hash#slice(*keys) and the #[] re-entry,
       and both arms hand back an sp_RbVal. This rule said so only for one or
       two keys, so a three-key `attributes.slice("a", "b", "c")` fell through
       to a typing that named a concrete hash kind, and the comparison site then
       boxed a value that was already boxed:
       `sp_box_nullable_obj((void *)<sp_RbVal>)`, which clang refused (#4429).
       The arity guard matches the emitter's now. */
    if (argc >= 1 && sp_streq(name, "slice")) { *out = TY_POLY; return 1; }
  }
  /* The String value-form mutators on a boxed receiver answer the mutated
     string (NULL for the no-change bang contract), like the typed path. */
  if (recv >= 0 && rt == TY_POLY && !an_user_defines_or_reads(c, name)) {
    static const char *const PBANGN[] = {
      "gsub!", "sub!", "upcase!", "downcase!", "capitalize!", "swapcase!",
      "strip!", "lstrip!", "rstrip!", "chomp!", "chop!", "squeeze!", "tr!",
      "delete!", "tr_s!", "delete_prefix!", "delete_suffix!", "succ!", "next!",
      NULL };
    for (int i = 0; PBANGN[i]; i++) if (sp_streq(name, PBANGN[i])) { *out = TY_STRING; return 1; }
  }
  /* The names Regexp alone owns, on a boxed receiver: the emitter unboxes the
     pattern and dispatches through the typed emitter, so the answer is the
     typed one. Untyped, the call boxed nil and `re.source` on a block
     parameter answered nil (#3961). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_or_reads(c, name)) {
    if (sp_streq(name, "source")) { *out = TY_STRING; return 1; }
    if (sp_streq(name, "options")) { *out = TY_INT; return 1; }
    if (sp_streq(name, "casefold?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "names")) { *out = TY_STR_ARRAY; return 1; }
    if (sp_streq(name, "named_captures")) { *out = TY_STR_POLY_HASH; return 1; }
  }
  /* the match forms with a boxed operand answer what the typed ones do */
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_or_reads(c, name) &&
      (rt == TY_POLY || ((rt == TY_STRING || rt == TY_STRBUF) &&
                         infer_type(c, argv[0]) == TY_POLY))) {
    if (sp_streq(name, "match?")) { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "match")) { *out = TY_MATCHDATA; return 1; }
    if (sp_streq(name, "=~")) { *out = TY_POLY; return 1; }
  }
  /* The Integer surface on a boxed value that may hold a Bignum (#4665):
     an answer that can itself be a Bignum stays boxed (pred, pow, ceildiv,
     gcd, lcm; gcdlcm is a pair of them), one that fits an Integer is one
     (bit_length; digits is an array of them). The runtime helpers of the
     same names answer by the box's tag; #[] is the boxed index, which
     answers a bit for an Integer receiver. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_or_reads(c, name)) {
    if (sp_streq(name, "digits") && argc <= 1) { *out = TY_INT_ARRAY; return 1; }
    if (sp_streq(name, "gcdlcm") && argc == 1) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "bit_length") && argc == 0) { *out = TY_INT; return 1; }
    if (sp_streq(name, "pred") && argc == 0) { *out = TY_POLY; return 1; }
    if ((sp_streq(name, "ceildiv") || sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1)
      { *out = TY_POLY; return 1; }
    if (sp_streq(name, "pow") && (argc == 1 || argc == 2)) { *out = TY_POLY; return 1; }
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) && argc == 1)
      { *out = TY_BOOL; return 1; }
  }
  /* The Enumerable names a boxed receiver shares with Array: the emitter
     materializes the elements and re-dispatches, so the answer is the
     poly-array one. */
  if (recv >= 0 && rt == TY_POLY && !an_user_defines_or_reads(c, name)) {
    int has_blk = nt_ref(nt, id, "block") >= 0;
    if (!has_blk && argc == 0 && sp_streq(name, "minmax")) { *out = TY_POLY_ARRAY; return 1; }
    if (!has_blk && sp_streq(name, "product")) { *out = TY_POLY_ARRAY; return 1; }
    if (!has_blk && (sp_streq(name, "combination") || sp_streq(name, "permutation")))
      { *out = TY_POLY_ARRAY; return 1; }
    /* the runs only when a `.to_a` terminal materializes them; on its own the
       call answers an Enumerator, exactly as it does for a typed receiver.
       Answering the array either way declared the slot sp_PolyArray * and put
       the emitter's sp_Enumerator * in it. */
    if (has_blk && argc == 0 &&
        (sp_streq(name, "chunk_while") || sp_streq(name, "slice_when") ||
         sp_streq(name, "chunk") ||
         sp_streq(name, "slice_before") || sp_streq(name, "slice_after")))
      { *out = an_chunk_family_to_a(c, id) ? TY_POLY_ARRAY : TY_ENUMERATOR; return 1; }
  }
  /* poly.tr / the String-pattern sub / gsub: same shape with two arguments. */
  if (recv >= 0 && rt == TY_POLY && argc == 2 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "tr") ||
       ((sp_streq(name, "sub") || sp_streq(name, "gsub")) &&
        infer_type(c, argv[0]) == TY_STRING && infer_type(c, argv[1]) == TY_STRING)) &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_STRING; return 1; }
  /* poly.sub / poly.gsub with a block: like the typed String receiver's block
     form, this always answers a String (the rebuilt receiver), whatever the
     boxed value's runtime tag turns out to be. Without a rule here the call
     fell through unresolved, and codegen's own poly gsub/sub arm only covers
     the two-argument (blockless) shape above -- so this stayed unresolved on
     both sides of the same gap. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "sub") || sp_streq(name, "gsub")) &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_STRING; return 1; }
  /* poly.compact / poly.flatten: an Array read out of a container answers a
     generic Array either way (#3423). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "flatten") &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_POLY_ARRAY; return 1; }
  /* #compact answers the receiver's own kind -- Hash#compact is a Hash -- and
     only the runtime value says which, so it rides boxed (#3449). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "compact") && !an_user_defines_or_reads(c, name))
    { *out = TY_POLY; return 1; }
  /* poly.find_index/index/rindex { } : the matching element's index, or nil
     when the block never answers truthy -- so poly, not a bare int. Every
     sibling block name had a poly rule; this family had none, and the call
     fell through untyped to the unresolved-call raise (#3409). */
  /* The two-argument form is String's alone (a start offset for index, a stop
     for rindex); it answered nothing and raised (#4149). */
  if (recv >= 0 && rt == TY_POLY &&
      (argc == 0 ? nt_ref(nt, id, "block") >= 0
                 : ((argc == 1 ||
                     (argc == 2 && !sp_streq(name, "find_index"))) &&
                    nt_ref(nt, id, "block") < 0)) &&
      (sp_streq(name, "find_index") || sp_streq(name, "index") || sp_streq(name, "rindex")) &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_POLY; return 1; }
  /* String#chars on a poly value (a String read out of a container / pair):
     an array of single-char strings (#2909). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "chars") || sp_streq(name, "lines")) &&
      nt_ref(nt, id, "block") < 0)
    { *out = an_user_defines_or_reads(c, name) ? TY_POLY : TY_STR_ARRAY; return 1; }
  /* A blockless grouping enumerator on a boxed Array -- an Array read out of a
     container -- materializes to the groups themselves, an Array of Arrays. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "each_cons") || sp_streq(name, "each_slice") ||
       sp_streq(name, "combination") || sp_streq(name, "permutation")) &&
      !an_user_defines_or_reads(c, name))
    { *out = (sp_streq(name, "each_cons") || sp_streq(name, "each_slice"))
             ? TY_ENUMERATOR : TY_POLY_ARRAY; return 1; }   /* as the typed array answers */
  /* A blockless `each` / `each_entry` / `each_with_index` on a boxed receiver
     (an Array read out of a container, a block parameter) is an external
     Enumerator, exactly as it is for a typed receiver. Without a type it
     stayed unresolved and every chained method reported "for unknown" (#3584).
     `reverse_each` is the same Enumerator over the elements reversed, and
     stayed unresolved the same way. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "each") || sp_streq(name, "each_entry") ||
       sp_streq(name, "reverse_each")) &&
      !an_user_defines_or_reads(c, name))
    { *out = TY_ENUMERATOR; return 1; }
  /* A blockless each_char / each_line / each_byte / each_codepoint on the same
     boxed String is CRuby's Enumerator; spinel materializes the elements, so
     it answers exactly what chars / lines / bytes do. Without this the
     enumerator stayed untyped and reduce/to_a/sum on it all failed. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "each_char") || sp_streq(name, "each_line") ||
       sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) &&
      !an_user_defines_or_reads(c, name)) {
    if (sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) { *out = TY_INT_ARRAY; return 1; }
    { *out = TY_STR_ARRAY; return 1; }
  }
  /* poly.each_char { |c| }: the block param is a one-char String and the call
     answers the receiver's string, as String#each_char answers self (#3402). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && sp_streq(name, "each_char") &&
      nt_ref(nt, id, "block") >= 0 && !an_user_defines_or_reads(c, "each_char") &&
      !an_user_defines_or_reads(c, "chars")) {
    int eb = nt_ref(nt, id, "block");
    const char *ebp = block_param_name(c, eb, 0);
    Scope *ebs = ebp ? comp_scope_of(c, eb) : NULL;
    LocalVar *ebl = (ebs && ebp) ? scope_local(ebs, ebp) : NULL;
    if (ebl && ebl->type != TY_STRING) ebl->type = TY_STRING;
    { *out = TY_STRING; return 1; }
  }
  /* `poly.empty?`: the dispatch carries builtin String / Array / Hash arms, so
     the call answers a boolean even when a user class defines the name too --
     without a type the enclosing method came out void and the value was nil.
     What it must NOT do is pin the C temp to a bool when that user arm answers
     something a bool cannot hold: `def empty? = 0` came back as false, and 0
     is truthy in Ruby (#4083). Poly holds both, and the method still has a
     type, which was the point. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "empty?") && argc == 0 &&
      nt_ref(nt, id, "block") < 0)
    { *out = an_user_ret_disagrees(c, name, TY_BOOL) ? TY_POLY : TY_BOOL; return 1; }
  /* poly.merge(other) { |k, old, new| } -- a Hash reached through a container.
     The conflict-block form builds the same general boxed-key/value hash the
     blockless one does; without a type it stayed unresolved. */
  if (recv >= 0 && (rt == TY_POLY || ty_is_hash(rt)) && sp_streq(name, "merge") && argc == 1 &&
      nt_ref(nt, id, "block") >= 0 && !an_user_defines_or_reads(c, "merge"))
    { *out = TY_POLY_POLY_HASH; return 1; }   /* the conflict block decides each value */
  /* `x.to_json` -- CRuby's json defines it on every core class. A user class
     that defines its own wins (the dispatch below sees it); everything else
     serializes through the generator, exactly as JSON.generate(x) does. */
  /* A boxed receiver is included: the generator handles whatever it holds, and
     a user class's own #to_json still wins through the runtime hook. Excluding
     it left `JSON.parse(s).to_json` unresolved (#4385). */
  if (recv >= 0 && sp_streq(name, "to_json") && nt_ref(nt, id, "block") < 0 &&
      sp_feature_required("json") && rt != TY_UNKNOWN &&
      (!ty_is_object(rt) ||
       (ty_object_class(rt) >= 0 &&
        comp_method_in_chain(c, ty_object_class(rt), "to_json", NULL) < 0)))
    { *out = TY_STRING; return 1; }

  /* poly.scan(re): a String read out of a container. Same shape as the
     rt==TY_STRING rule -- captures give an array of arrays (#3368). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "scan") &&
      nt_ref(nt, id, "block") < 0 && !an_user_defines_or_reads(c, name)) {
    const char *rsrc = an_regex_lit_src(c, argv[0]);
    if (rsrc) { *out = an_re_has_captures(rsrc) ? TY_POLY_ARRAY : TY_STR_ARRAY; return 1; }
    /* A pattern only known at run time takes the shape that answers either
       way, exactly as the String-receiver rule does: the poly arm was narrower
       than the String one for no reason of its own (#3392). */
    if (infer_type(c, argv[0]) == TY_REGEX) { *out = TY_POLY_ARRAY; return 1; }
    if (infer_type(c, argv[0]) == TY_STRING) { *out = TY_STR_ARRAY; return 1; }
    /* a BOXED pattern (a Regexp read out of a table) is as unknowable as a
       run-time Regexp value: same shape */
    if (infer_type(c, argv[0]) == TY_POLY) { *out = TY_POLY_ARRAY; return 1; }
  }
  /* Array#find / #detect over a poly value that is an array at runtime (an
     inner array read out of a poly container): the matched element (or nil) is
     boxed, so the result is poly (#2904). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "find") || sp_streq(name, "detect")) &&
      nt_ref(nt, id, "block") >= 0 && !an_user_defines_method(c, name))
    { *out = TY_POLY; return 1; }
  /* exception accessors on a poly receiver (an exception rescued into a
     union-typed local) delegate at runtime; message is a String, the rest
     carry boxed values (#3120, #3122). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "message") || sp_streq(name, "result") ||
       sp_streq(name, "errno") ||
       sp_streq(name, "key") || sp_streq(name, "receiver")))
    { *out = sp_streq(name, "message") ? TY_STRING : TY_POLY; return 1; }
  /* Integer / Time accessors, Proc#arity on a poly value read out of a
     container: an int-returning builtin the poly-builtin dispatch handles at
     runtime; type it int so the result is not boxed to nil (#3162). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "arity") || sp_streq(name, "year") || sp_streq(name, "mon") ||
       sp_streq(name, "month") || sp_streq(name, "mday") ||
       sp_streq(name, "hour") || sp_streq(name, "sec") ||
       sp_streq(name, "wday") || sp_streq(name, "yday") ||
       /* tv_sec is the epoch second, the same read #to_i answers (#3866) */
       sp_streq(name, "tv_sec")))
    { *out = TY_INT; return 1; }
  /* The Time methods that answer a TIME rather than a number, on the same
     boxed receiver. Only the scalar half of the surface was here, so
     `value.utc` on a value narrowed by is_a?(Time) typed as nothing and the
     chained read raised NoMethodError at run time with a clean C build
     (#4109). The result stays boxed, so a further `.year` dispatches through
     this same surface. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      ((argc == 0 && (sp_streq(name, "utc") || sp_streq(name, "gmtime") ||
                      sp_streq(name, "getutc") || sp_streq(name, "localtime") ||
                      sp_streq(name, "getlocal") || sp_streq(name, "round"))) ||
       (argc == 1 && (sp_streq(name, "localtime") || sp_streq(name, "getlocal"))) ||
       (argc == 0 && sp_streq(name, "getgm"))))
    { *out = TY_POLY; return 1; }
  /* The rest of the Time surface on a boxed receiver, typed the way the typed
     emitter answers them: the scalar reads, the predicates, and the string
     formatters were all absent, so each was a run-time NoMethodError with a
     clean C build (#4109). subsec answers Integer 0 or a Rational, so it stays
     boxed. Only the names Time alone owns are here -- min / round / floor /
     ceil belong to the collections and the numbers too, and claiming a type
     for them would answer for every boxed receiver, not just a Time. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name)) {
    if (sp_streq(name, "tv_usec") || sp_streq(name, "usec") ||
        sp_streq(name, "tv_nsec") || sp_streq(name, "nsec") ||
        sp_streq(name, "utc_offset") || sp_streq(name, "gmt_offset") ||
        sp_streq(name, "gmtoff"))
      { *out = TY_INT; return 1; }
    if (sp_streq(name, "utc?") || sp_streq(name, "gmt?") || sp_streq(name, "dst?") ||
        sp_streq(name, "isdst") || sp_streq(name, "sunday?") || sp_streq(name, "monday?") ||
        sp_streq(name, "tuesday?") || sp_streq(name, "wednesday?") ||
        sp_streq(name, "thursday?") || sp_streq(name, "friday?") ||
        sp_streq(name, "saturday?"))
      { *out = TY_BOOL; return 1; }
    if (sp_streq(name, "zone") ||
        ((sp_streq(name, "iso8601") || sp_streq(name, "xmlschema")) &&
         sp_feature_enabled("time")))
      { *out = TY_STRING; return 1; }
    if (sp_streq(name, "subsec")) { *out = TY_POLY; return 1; }
  }
  /* iso8601(n) / xmlschema(n) on a boxed Time: the fraction-digits form the
     typed emitter serves, absent here (campfire's
     `message.created_at.utc.iso8601(3)` over a nilable column). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "iso8601") || sp_streq(name, "xmlschema")) &&
      sp_feature_enabled("time"))
    { *out = TY_STRING; return 1; }
  /* Range#to_a on a poly value: its element array. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "to_a"))
    { *out = TY_POLY_ARRAY; return 1; }
  /* uniq on a poly value that is an array at runtime: a poly array (#3341). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "uniq"))
    { *out = TY_POLY_ARRAY; return 1; }
  /* String#split on a poly value (a string param widened to poly): a string
     array, so a following `.map` / multiple assignment narrows (#3186/#3164). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "split") &&
      argc <= 2 && nt_ref(nt, id, "block") < 0 && !an_user_defines_method(c, name))
    { *out = TY_STR_ARRAY; return 1; }
  /* blockless each_index / each_with_index on a poly value (an inner array read
     out of a container): a chained Enumerator, so `.map`/`.to_a` re-dispatch on
     it materializes (#3160). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "each_index") || sp_streq(name, "each_with_index")))
    { *out = TY_ENUMERATOR; return 1; }
  /* Hash#merge on a poly value: a general PolyPoly hash. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "merge"))
    { *out = TY_POLY_POLY_HASH; return 1; }
  /* When ostruct is in the program, a bare reader on a poly value may be an
     OpenStruct member (any name, boxed value). The runtime dispatch checks the
     tag; type it poly so the member is not truncated to a class-name string
     (#3197). Length/size/predicate readers keep their own arms. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_feature_required("ostruct") &&
      name[0] && name[strlen(name) - 1] != '?' && name[strlen(name) - 1] != '!' &&
      !poly_builtin_zero_arg_name(name))
    { *out = TY_POLY; return 1; }

  /* #clear on a poly value returns the (emptied) receiver, itself poly. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "clear"))
    { *out = TY_POLY; return 1; }
  /* String#bytesplice on a poly value: the new contents, boxed */
  if (recv >= 0 && rt == TY_POLY && argc == 3 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "bytesplice"))
    { *out = TY_POLY; return 1; }
  /* String#replace/prepend/concat on a poly value: self, boxed */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && argc >= 1 &&
      (sp_streq(name, "replace") || sp_streq(name, "prepend") ||
       sp_streq(name, "concat")))
    { *out = TY_POLY; return 1; }
  /* in-place string mutators on a poly value: self (boxed) or nil */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && name[0] && strlen(name) > 1 &&
      name[strlen(name) - 1] == '!') {
    static const char *const PBN[] = {
      "upcase!","downcase!","capitalize!","swapcase!","strip!","lstrip!",
      "rstrip!","chomp!","chop!","squeeze!","reverse!","succ!","next!",
      "delete_prefix!","delete_suffix!","delete!","gsub!","sub!","tr!","tr_s!",
      NULL };
    for (int q = 0; PBN[q]; q++)
      if (sp_streq(name, PBN[q])) { *out = TY_POLY; return 1; }
  }
  /* Array#delete_at on a poly value: the removed element, boxed. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "delete_at"))
    { *out = TY_POLY; return 1; }
  /* Array#pop / #shift on a poly value: the removed element, boxed. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "pop") || sp_streq(name, "shift")))
    { *out = TY_POLY; return 1; }
  /* Array#insert on a poly value: in-place, returns the receiver (boxed). */
  if (recv >= 0 && rt == TY_POLY && argc == 2 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && sp_streq(name, "insert"))
    { *out = TY_POLY; return 1; }
  /* Time accessors on a poly value (a Time read out of a container): the
     codegen dispatch runs sp_time_* on the TIME tag and raises otherwise,
     so the non-raising result is an int (#3311). Declined when any class
     exposes the name as a method OR a reader (a Data/Struct member like
     `day` dispatches to the member, #3239). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "year") || sp_streq(name, "mon") || sp_streq(name, "month") ||
       sp_streq(name, "mday") || sp_streq(name, "day") || sp_streq(name, "hour") ||
       sp_streq(name, "sec") || sp_streq(name, "wday") || sp_streq(name, "yday"))) {
    int has_reader9 = 0;
    for (int k9 = 0; k9 < c->nclasses && !has_reader9; k9++)
      if (comp_reader_in_chain(c, k9, name, NULL)) has_reader9 = 1;
    if (!has_reader9) { *out = TY_INT; return 1; }
  }
  /* to_sym on a poly value: the codegen arm interns a STR tag / passes a SYM
     through and raises otherwise, so the non-raising result is a Symbol
     (typing it poly made the raw sp_sym land in an sp_RbVal slot, #3331). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "to_sym") || sp_streq(name, "intern")))
    { *out = TY_SYMBOL; return 1; }
  /* reduce/inject on a poly value (an array read out of a container): the
     fold runs over boxed elements, so the result is boxed. */
  if (recv >= 0 && rt == TY_POLY && argc <= 1 && nt_ref(nt, id, "block") >= 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "reduce") || sp_streq(name, "inject")))
    { *out = TY_POLY; return 1; }
  /* String#start_with? / #end_with? on a poly value: a bool. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) &&
      (sp_streq(name, "start_with?") || sp_streq(name, "end_with?")))
    { *out = TY_BOOL; return 1; }
  /* sort over a poly value that is an array at runtime (a group_by bucket / an
     inner array): a fresh sorted poly array (#2928). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && !an_user_defines_method(c, name) &&
      sp_streq(name, "sort") && nt_ref(nt, id, "block") < 0)
    { *out = TY_POLY_ARRAY; return 1; }
  /* ...and with a COMPARATOR block, which the emitter re-dispatches through
     the array path. Without the type the method emitted as void and answered
     nil, having sorted correctly on the way (#4290). min / max with one pick
     an element, so they answer boxed. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && !an_user_defines_method(c, name) &&
      nt_ref(nt, id, "block") >= 0) {
    if (sp_streq(name, "sort")) { *out = TY_POLY_ARRAY; return 1; }
    if (sp_streq(name, "min") || sp_streq(name, "max")) { *out = TY_POLY; return 1; }
  }
  /* Data#with on a poly value (a Data read out of a container) returns a new
     Data instance, boxed poly (#2890). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "with") &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode") &&
      !an_user_defines_method(c, name))
    { *out = TY_POLY; return 1; }
  /* poly.new(args): instantiating a Class value read out of a container yields
     a fresh object, boxed poly (#2888). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "new") &&
      !an_user_defines_method(c, name)) {
    /* a block goes to each class's `&blk`; a yielding initialize has none,
       and is inlined at static sites only (codegen's ctor_block_dispatchable) */
    int blk_ok = nt_ref(nt, id, "block") < 0;
    if (!blk_ok) {
      blk_ok = 1;
      for (int k = 0; k < c->nclasses && blk_ok; k++) {
        int im = comp_method_in_chain(c, k, "initialize", NULL);
        if (im >= 0 && c->scopes[im].yields) blk_ok = 0;
      }
    }
    if (blk_ok) { *out = TY_POLY; return 1; }
  }
  return 0;
}
