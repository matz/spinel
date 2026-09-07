/* codegen_iter.c -- block invocation, inline-call, and iteration/loop
   lowering, split out of codegen_call.c. Pure code movement, no logic change. */

#include "codegen_internal.h"

/* A fused loop names the receiver expression twice: once in the bound check
   (re-run on every iteration) and once in each element read. That is only
   sound while re-evaluating it is free and yields the same container. A call
   runs again per step, so `Dir.children(d).each { ... }` re-reads the
   directory mid-loop and a block that deletes entries skips half of them.
   Evaluate once into a rooted temp and rewrite the buffer to name it; a bare
   lvalue (`lv_x`, `sp_self->iv_a`) is left alone so the common loop keeps its
   current shape. */
static void hoist_loop_recv(Compiler *c, TyKind rt, Buf *rb, Buf *b, int indent) {
  if (!rb->p || !strchr(rb->p, '(')) return;
  int t = ++g_tmp;
  Buf ct; memset(&ct, 0, sizeof ct); emit_ctype(c, rt, &ct);
  emit_indent(b, indent);
  buf_printf(b, "%s _t%d = %s;", ct.p ? ct.p : "sp_RbVal", t, rb->p);
  free(ct.p);
  if (needs_root(rt)) buf_printf(b, rt == TY_POLY ? " SP_GC_ROOT_RBVAL(_t%d);" : " SP_GC_ROOT(_t%d);", t);
  buf_puts(b, "\n");
  free(rb->p); memset(rb, 0, sizeof *rb);
  buf_printf(rb, "_t%d", t);
}

/* Follow a chain of pure `...` forwarders (a method whose whole body is a
   single `target(...)` call, no receiver) from `mi` to the method that
   actually yields or owns the &block; return its index, else -1. A real-
   function forwarder can't pass a literal block down to a yielding target,
   so a block-bearing call is redirected straight to that final target. */
static int pure_forwarding_target(Compiler *c, int mi, int depth) {
  if (mi < 0 || depth > 16) return -1;
  Scope *m = &c->scopes[mi];
  if (m->yields || (m->blk_param && m->blk_param[0])) return mi;
  int body = m->body;
  if (body < 0 || !nt_type(c->nt, body) || !sp_streq(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
  if (n != 1) return -1;
  int call = st[0];
  const char *cty = nt_type(c->nt, call);
  if (!cty || !sp_streq(cty, "CallNode") || nt_ref(c->nt, call, "receiver") >= 0) return -1;
  int args = nt_ref(c->nt, call, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !nt_type(c->nt, av[0]) || !sp_streq(nt_type(c->nt, av[0]), "ForwardingArgumentsNode")) return -1;
  const char *tn = nt_str(c->nt, call, "name");
  if (!tn) return -1;
  int t = comp_method_index(c, tn);
  if (t < 0 && m->class_id >= 0) t = comp_method_in_chain(c, m->class_id, tn, NULL);
  return pure_forwarding_target(c, t, depth + 1);
}

/* Inline a call to a free-function yielding method `foo(args) { |bp| ... }`:
   declare the method's locals (renamed to avoid clashing with the call
   site), bind params to args, then emit the method body with yield
   expanding to the block. Returns 1 if handled. */
/* Depth of yielding-method inlining in flight. Legitimate re-entry (a yielded
   block that calls the same method again, `c.with_state { c.with_state { ... } }`)
   is bounded by the source's finite nesting, but a method whose OWN body calls
   itself (`rec` calling `rec`, base case a runtime `n`) inlines without bound
   and hangs the compiler. Cap the depth: real nesting is shallow, so exceeding
   the cap means unbounded self-recursion -- report it instead of looping (#2908). */
#define SP_INLINE_DEPTH_MAX 64
static int g_inline_depth = 0;

int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  int mi, recv_class = -1;
  int implicit_self = 0;
  if (recv < 0) {
    mi = comp_method_index(c, name);     /* free function */
    if (mi < 0) {                        /* implicit-self instance method */
      Scope *encl = comp_scope_of(c, id);
      if (encl->class_id >= 0) {
        mi = comp_method_in_chain(c, encl->class_id, name, NULL);
        implicit_self = 1;
        /* inside a class method, a bare call also reaches sibling class
           methods (self is the class there, no instance to bind) */
        if (mi < 0 && encl->is_cmethod) {
          mi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
          implicit_self = 0;
        }
      }
      else return 0;
    }
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    const char *rty = nt_type(nt, recv);
    /* a scoped receiver (NS::Base.transaction { }) resolves by its leaf name,
       the key classes are indexed under */
    const char *cname = (rty && (sp_streq(rty, "ConstantReadNode") ||
                                 sp_streq(rty, "ConstantPathNode")))
                        ? nt_str(nt, recv, "name") : NULL;
    int ci = cname ? comp_class_index(c, cname) : -1;
    if (ci >= 0) {
      /* Cls.method with a yield block: look up as a class method */
      mi = comp_cmethod_in_chain(c, ci, name, NULL);
    }
    else if (ty_is_object(rt)) {
      /* An instance receiver -- including a constant that holds an instance
         (e.g. `S = Set.new(...); S.each { }`), which is not a class name so
         falls through here rather than the class-method lookup above. */
      recv_class = ty_object_class(rt);
      mi = comp_method_in_chain(c, recv_class, name, NULL);
    }
    else if (g_inline_recv_expr && g_inline_recv_class >= 0) {
      /* poly-receiver dispatch arm (#2448): self is pre-bound to a cast of the
         boxed receiver, and the concrete class is supplied out of band */
      recv_class = g_inline_recv_class;
      mi = comp_method_in_chain(c, recv_class, name, NULL);
    }
    else return 0;
  }
  (void)implicit_self;
  if (mi < 0) return 0;
  /* `fwd(args) { block }` where fwd just forwards `target(...)`: a literal
     block can't reach a real-function forwarder, so retarget to `target`
     with this call's args + block (target then splices the block normally). */
  {
    int blk0 = nt_ref(nt, id, "block");
    if (blk0 >= 0 && nt_type(nt, blk0) && sp_streq(nt_type(nt, blk0), "BlockNode")) {
      int t = pure_forwarding_target(c, mi, 0);
      if (t >= 0) mi = t;
    }
  }
  Scope *m = &c->scopes[mi];
  if (!m->yields) return 0;
  /* A `return` inside the yielding method used to bail here -- but a bailed
     block call falls back to a plain function call against a symbol that is
     never emitted (yielding methods have no standalone function), an
     undefined-symbol link error (doom's PlayerPhysics#each_nearby_linedef:
     an early `@map.linedefs.each { |ld| yield ld }; return` branch). Inline
     anyway, funneling the method's own returns to a per-inline exit label;
     the caller block spliced at yield sites is exempted by
     emit_block_invoke, which restores the real function's funnel. */
  int m_has_ret = scope_has_return(c, mi);
  int block = nt_ref(nt, id, "block");   /* may be -1: no block passed */
  char yprocbuf[128];  /* holds "lv_" + a rename_local result (g_ren_to width 112) */
  const char *fwd_yield_proc = NULL;
  int fwd_proc_expr = -1, fwd_proc_tmp = 0;
  /* `inner(&)` / `inner(&block)`: a BlockArgumentNode forwards the block
     active at this (already-inlined) site, not a fresh literal. */
  if (block >= 0 && nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode")) {
    /* A forwarded `&blk` whose blk is a real (materialized) proc local -- e.g.
       the enclosing method nil-checks its &block, so it can't be an inlined
       literal block -- has no block body to splice; the inlined callee's
       `yield` must call the proc instead. */
    int fexpr = nt_ref(nt, block, "expression");
    const char *pn = (fexpr >= 0 && nt_type(nt, fexpr) &&
                      sp_streq(nt_type(nt, fexpr), "LocalVariableReadNode"))
                     ? nt_str(nt, fexpr, "name") : NULL;
    Scope *encl = pn ? comp_scope_of(c, id) : NULL;
    LocalVar *plv = encl ? scope_local(encl, pn) : NULL;
    /* `def outer(&b); inner(&b); end`: the name is the ENCLOSING inlined
       method's own block parameter, which has no local of its own -- the
       inliner skips it as a virtual slot. It is the proc reference this
       inline is already running under, and reading it as a local named an
       undeclared `lv_b`. */
    if (g_block_id < 0 && pn && g_yield_proc_ref &&
        g_block_param_name && sp_streq(pn, g_block_param_name)) {
      snprintf(yprocbuf, sizeof yprocbuf, "%s", g_yield_proc_ref);
      fwd_yield_proc = yprocbuf;
    }
    else if (g_block_id < 0 && plv && plv->type == TY_PROC) {
      snprintf(yprocbuf, sizeof yprocbuf, "lv_%s", rename_local(pn));
      fwd_yield_proc = yprocbuf;
    }
    /* Any other first-class callable passed with `&` -- a lambda literal, a
       `method(:m)`, a Proc read out of a container -- is hoisted into a temp
       and driven the same way; without this the splice found no block at all
       and the call answered nil (#3688). */
    else if (g_block_id < 0 && fexpr >= 0 && !fwd_yield_proc) {
      TyKind fkt = comp_ntype(c, fexpr);
      if (fkt == TY_PROC || fkt == TY_METHOD || fkt == TY_POLY) {
        fwd_proc_expr = fexpr;
        fwd_proc_tmp = ++g_tmp;
        snprintf(yprocbuf, sizeof yprocbuf, "_t%d", fwd_proc_tmp);
        fwd_yield_proc = yprocbuf;
      }
    }
    block = g_block_id;
  }
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  /* Pre-check: every body local must have an emittable type. Bail BEFORE
     writing anything (a mid-emit bail would leave an unbalanced `{`). */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  if (g_inline_depth >= SP_INLINE_DEPTH_MAX)
    unsupported_feature(c, id, "a method that uses its block (yield or block.call) and calls itself recursively (inlining cannot terminate; no standalone function to fall back to)");
  g_inline_depth++;
  int saved_nren = g_nren, saved_block = g_block_id;
  int saved_bnren = g_block_nren, saved_yfbn = g_yield_block_fallback_nren;
  int saved_emcls = g_emitting_class_id;
  const char *saved_self = g_self;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  const char *saved_bbv = g_block_brk_var, *saved_yfbv = g_yield_blk_brk_fallback;
  const char *saved_ser = g_brk_ser_var;
  int saved_bbe = g_block_brk_ebase, saved_yfbe = g_yield_blk_brk_efallback;
  int saved_bbexc = g_block_brk_exc_base, saved_bexc = g_brk_exc_base;
  int saved_ebase = g_brk_ensure_base;
  /* Stack-local, not static: emit_inline_call_x recurses (a yielded block can
     call the same yielding method), and g_self points into this buffer. A
     shared static would be clobbered by the nested inline, so the outer frame's
     ensure/trailing-self would emit the inner receiver temp (undeclared here). */
  char selfbuf[64];
  /* Nested `yield` inside the block body should chain to the block that was
     active before this inline, not to the inner block. */
  g_yield_block_fallback = saved_block;
  g_yield_block_fallback_nren = saved_bnren;
  g_yield_blk_brk_fallback = saved_bbv;
  g_yield_blk_brk_efallback = saved_bbe;
  /* the block being captured is caller code: record the caller's self so
     emit_block_invoke can restore it around the spliced block body. Aliasing
     g_self by pointer is safe now that selfbuf is stack-local: it names an
     ancestor frame's selfbuf, which stays live and unmodified for the whole
     nested emission (a frame only ever writes its own selfbuf). */
  const char *saved_self_fb = g_yield_self_fallback;
  const char *saved_deref_fb = g_yield_self_deref_fallback;
  int saved_emcls_fb = g_yield_emitting_class_fallback;
  /* The inlined callee's own yields splice this call site's block; only a
     yield in spliced CALLER code belongs to an enclosing lowered method.
     Park the lowered context for emit_block_invoke and clear it for the
     callee body -- the same discipline as the self/emitting-class pair. */
  int saved_low_fb = g_yield_lowered_fallback;
  const char *saved_lbnf = g_yield_lowered_blk_fallback;
  int saved_low = g_current_scope_is_lowered;
  const char *saved_lbn = g_lowered_blk_name;
  g_yield_lowered_fallback = g_current_scope_is_lowered;
  g_yield_lowered_blk_fallback = g_lowered_blk_name;
  g_current_scope_is_lowered = 0;
  g_lowered_blk_name = NULL;
  g_yield_self_fallback = g_self;
  g_yield_self_deref_fallback = g_self_deref;
  /* captured here, BEFORE the receiver-context switch below, so it holds the
     caller's class for the spliced (caller-code) block body */
  g_yield_emitting_class_fallback = g_emitting_class_id;
  g_block_id = block;
  /* a forwarded outer block keeps ITS definition depth; a literal block is
     call-site code at the depth BEFORE this inline's renames */
  g_block_nren = (block == saved_block) ? saved_bnren : saved_nren;
  const char *saved_ypr = g_yield_proc_ref;
  TyKind saved_yslot = g_yield_slot_ty;
  g_yield_proc_ref = fwd_yield_proc;   /* NULL clears it for a normal inline */
  g_yield_slot_ty = TY_UNKNOWN;        /* set to the inline's return type below */
  /* the literal block binds to THIS call site's break scope; a forwarded
     BlockArgumentNode block keeps its original definition-site scope */
  g_block_brk_var = (block == saved_block) ? saved_bbv : saved_ser;
  g_block_brk_ebase = (block == saved_block) ? saved_bbe : saved_ebase;
  g_block_brk_exc_base = (block == saved_block) ? saved_bbexc : saved_bexc;
  /* the METHOD BODY's own breaks (a while inside m) never target the caller */
  g_brk_ser_var = NULL;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  /* the `&callable` argument, evaluated once for the whole inlined body */
  if (fwd_proc_expr >= 0) {
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_Proc *_t%d = ", fwd_proc_tmp);
    if (comp_ntype(c, fwd_proc_expr) == TY_PROC) emit_expr(c, fwd_proc_expr, b);
    else { buf_puts(b, "sp_poly_to_proc("); emit_boxed(c, fwd_proc_expr, b); buf_puts(b, ")"); }
    buf_printf(b, "; SP_GC_ROOT(_t%d);\n", fwd_proc_tmp);
  }
  /* instance method: bind self to the receiver. A heap object is a pointer; a
     value-type receiver is a by-value struct, so copy it and dereference its
     ivars with `.` (value types are immutable, so the copy is transparent). */
  const char *saved_deref = g_self_deref;
  /* The receiver-context switch (g_self / g_self_deref / g_emitting_class_id)
     is DEFERRED until after the argument binding below: those arg
     expressions are call-site code and must resolve against the caller's
     self and class (e.g. a caller's attr_reader interpolated into a
     `fetch(key) { ... }` cache key). Only the receiver *temp* is declared
     here — and the receiver expression itself is still emitted in the
     caller's context (g_self unchanged at this point). */
  const char *recv_self_deref = NULL;
  if (recv >= 0 && recv_class >= 0) {
    int self_is_val = c->classes[recv_class].is_value_type;
    int st = ++g_tmp;
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_%s %s_t%d = ", c->classes[recv_class].c_name, self_is_val ? "" : "*", st);
    if (g_inline_recv_expr) buf_puts(b, g_inline_recv_expr);  /* pre-hoisted cast (#2448) */
    else emit_expr(c, recv, b);
    buf_puts(b, ";");
    /* Root it: for the whole inlined body this temp is the only handle on the
       receiver, and every ivar read in the body goes through it. The body
       allocates and so does the caller block spliced at each yield, so a
       receiver nothing else holds -- `make_ledger.each { churn }`, `Set#each`
       on a set the call itself built, or a local the block clears partway
       through -- was collected while the body still ran, and the walk stopped
       early: 25 of 200 turns on a plain release build, 1 of 200 under
       SPINEL_GC_STRESS=1, with no error either way.
       A value-type receiver is a struct copy that lives in the temp itself
       rather than behind it, so it must not be rooted; emit_gc_root_tmp
       declines it on its own account, and the test here is only so that the
       separating space is not emitted when it does. */
    if (!self_is_val) { buf_puts(b, " "); emit_gc_root_tmp(c, ty_object(recv_class), st, b); }
    buf_puts(b, "\n");
    snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
    recv_self_deref = self_is_val ? "." : "->";
  }
  int din = indent + 1;

  /* declare method locals under renamed names */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;  /* virtual &block slot */
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_inlined_local_decl(c, lv, rn, b, din);
  }

  /* bind params to call args (args are in the call-site scope: renames off) */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  /* `bar(...)` inside a `def foo(...)` forwarder: bind this (inlined) target's
     params from the enclosing forwarder's synth __fwd_* params, not from a
     literal ForwardingArgumentsNode (which has no value of its own). */
  Scope *fwd_encl = NULL;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode"))
    fwd_encl = comp_scope_of(c, argv[0]);
  /* A trailing keyword-hash arg binds by param name, not positionally. */
  int kwh = -1, pos_argc = argc;
  if (argc > 0 && argv && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1]; pos_argc = argc - 1;
  }
  /* A `**hash` inside the keyword-hash arg (`m(**h)`) carries no literal keys,
     so keyword params bind from a runtime lookup on the materialized hash, the
     same way emit_dispatch/emit_args_filled do -- without this each keyword
     param fell through to a fabricated default. */
  TyKind ds_type = TY_UNKNOWN;
  int ds_tmp = emit_ds_hash_materialize(c, kwh, &ds_type);
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    { char rn[128]; snprintf(rn, sizeof rn, "_y%d_%s", tag, m->pnames[i]);
      emit_inlined_param_target(c, m, m->pnames[i], rn, b); }
    /* hide THIS inline's renames only: args are call-site expressions,
       and the call site may itself be an outer inlined body whose locals
       are renamed (nested yield-method inlines) -- zeroing the whole
       table emitted the unrenamed lv_<name> (undeclared identifier, or a
       silent capture of a same-named caller local). */
    int sv = g_nren;
    /* The argument expression is call-site code, so the callee's renames are
       switched off for it. A nested inline INSIDE that expression pushes its
       own entries at this very depth and overwrites the callee's, so restoring
       the count alone brought back another method's names -- this body then
       emitted the unrenamed `lv_<name>` for whatever had been clobbered, which
       nothing declares (#3943). Park the entries across the argument, not just
       the count. */
    int park_n = sv - saved_nren;
    char (*park_f)[96] = NULL; char (*park_t)[112] = NULL;
    if (park_n > 0) {
      park_f = (char (*)[96])malloc(sizeof(char[96]) * (size_t)park_n);
      park_t = (char (*)[112])malloc(sizeof(char[112]) * (size_t)park_n);
      if (park_f && park_t) {
        memcpy(park_f, g_ren_from + saved_nren, sizeof(char[96]) * (size_t)park_n);
        memcpy(park_t, g_ren_to + saved_nren, sizeof(char[112]) * (size_t)park_n);
      }
      else { free(park_f); free(park_t); park_f = NULL; park_t = NULL; }
    }
    g_nren = saved_nren;
    if (fwd_encl && i < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[i]);
      LocalVar *mp = scope_local(m, m->pnames[i]);
      TyKind et = ep ? ep->type : TY_POLY;
      TyKind mt = mp ? mp->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[i]);
      if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, b);
      else buf_puts(b, txt);
    }
    /* A rest param collects the middle arguments into an Array. Without this
       the first argument was assigned straight into the rest slot -- a
       pointer of the wrong type, so the rest read back empty (or crashed). */
    else if (m->rest_idx >= 0 && i == m->rest_idx)
      emit_rest_pack_kwh(c, i, pos_argc - m->npost_rest, argv, -1, b);
    else if (m->rest_idx >= 0 && i > m->rest_idx && i <= m->rest_idx + m->npost_rest) {
      int post_j = i - m->rest_idx - 1;   /* 0-based index among the posts */
      int argv_idx = pos_argc - m->npost_rest + post_j;
      emit_arg_or_default(c, m, i,
                          (argv && argv_idx >= 0 && argv_idx < pos_argc) ? argv[argv_idx] : -1, b);
    }
    /* Anything past the rest that is not one of its posts is a keyword (or
       **kwrest) param: it binds by name, never positionally. */
    else if (i < pos_argc && !(m->rest_idx >= 0 && i > m->rest_idx))
      emit_arg_or_default(c, m, i, argv[i], b);
    else {
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      /* No literal key for this keyword param, but a `**hash` was splatted:
         extract it by name from the materialized hash (falls back to the
         param default when the key is absent). */
      if (kv < 0 && ds_tmp >= 0 && callee_has_kwarg(c, m, m->pnames[i]))
        emit_ds_param_extract(c, m, i, ds_tmp, ds_type, b);
      else
        emit_arg_or_default(c, m, i, kv, b);
    }
    g_nren = sv;
    if (park_f && park_t) {
      memcpy(g_ren_from + saved_nren, park_f, sizeof(char[96]) * (size_t)park_n);
      memcpy(g_ren_to + saved_nren, park_t, sizeof(char[112]) * (size_t)park_n);
    }
    free(park_f); free(park_t);
    buf_puts(b, ";\n");
  }

  /* Now switch into the RECEIVER's context for the method BODY. Both the
     self binding and the emitting-class must move together, and only here
     — AFTER argument binding — so that implicit-self references inside the
     body (`to_a` in `def map; to_a.map { |x| yield x }; end`) resolve
     against the receiver, while call-site arg expressions above stayed in
     the caller's context (a caller's attr_reader interpolated into a
     `fetch(key) { ... }` cache key must call the caller's method on the
     caller's self, not the receiver temp). Mirrors how a normal method-
     body emission sets g_self + g_emitting_class_id to its own object and
     class (codegen.c). */
  if (recv_self_deref) {
    g_self = selfbuf;
    g_self_deref = recv_self_deref;
  }
  if (recv_class >= 0) g_emitting_class_id = recv_class;

  /* per-inline return funnel (stack storage: the inliner recurses, and the
     saved outer label pointer must stay valid across a nested inline). */
  char inl_lbl[32];
  const char *sv_prl = g_method_pr_label, *sv_prv = g_method_pr_var;
  TyKind sv_prt = g_ret_type;
  int sv_prexc = g_method_pr_exc_depth;
  if (as_expr) {
    /* Use a result var so the tail uses assignment, not `return`, in the
       GCC statement-expression ({ ... result_var; }) context. */
    TyKind rt = comp_ntype(c, id);
    if (fwd_yield_proc) g_yield_slot_ty = rt;  /* value-position yield unboxes to this */
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value(rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = rvbuf; g_ret_type = rt;
      g_method_pr_exc_depth = g_exc_frame_depth;
      /* body in its own scope: the funnel goto then EXITS the scopes of any
         cleanup-attributed GC roots the body declares (legal, cleanups run)
         instead of jumping over them in the same scope (a C error). */
      emit_indent(b, din); buf_puts(b, "{\n");
    }
    emit_stmts_tail(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv; g_ret_type = sv_prt;
      g_method_pr_exc_depth = sv_prexc;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else {
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = NULL;
      g_method_pr_exc_depth = g_exc_frame_depth;
      emit_indent(b, din); buf_puts(b, "{\n");   /* see expr-path comment */
    }
    emit_stmts(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv;
      g_method_pr_exc_depth = sv_prexc;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
  }
  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_yield_proc_ref = saved_ypr;
  g_yield_slot_ty = saved_yslot;
  g_block_brk_var = saved_bbv; g_yield_blk_brk_fallback = saved_yfbv;
  g_block_brk_ebase = saved_bbe; g_yield_blk_brk_efallback = saved_yfbe;
  g_block_brk_exc_base = saved_bbexc; g_brk_exc_base = saved_bexc;
  g_brk_ser_var = saved_ser; g_brk_ensure_base = saved_ebase;
  g_self = saved_self;
  g_self_deref = saved_deref;
  g_emitting_class_id = saved_emcls;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  g_block_nren = saved_bnren;
  g_yield_block_fallback_nren = saved_yfbn;
  g_yield_self_fallback = saved_self_fb;
  g_yield_self_deref_fallback = saved_deref_fb;
  g_yield_emitting_class_fallback = saved_emcls_fb;
  g_yield_lowered_fallback = saved_low_fb;
  g_yield_lowered_blk_fallback = saved_lbnf;
  g_current_scope_is_lowered = saved_low;
  g_lowered_blk_name = saved_lbn;
  if (g_inline_depth > 0) g_inline_depth--;
  return 1;
}

int emit_inline_call(Compiler *c, int id, Buf *b, int indent) {
  return emit_inline_call_x(c, id, b, indent, 0);
}

/* Is `id` a `<&block-param>.call(...)` invocation of the active block? */
int is_block_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]") && !sp_streq(nm, "yield"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  /* Inside an Enumerator.new generator the block's first param is the YIELDER
     (g_block_param_name == g_yielder_name); `y.yield(v)` lowers to Fiber.yield
     via the g_yielder path, so it is not a block-param call to inline here. */
  if (g_yielder_name && rn && sp_streq(rn, g_yielder_name)) return 0;
  return rn && sp_streq(rn, g_block_param_name);
}

/* A `<&block-param>.call(...)` on the inlined method's block param while NO
   block is supplied at this site (g_block_id<0). This arises when a real-
   function forwarder with no block of its own inlines a target that calls its
   &block: the path is dead for any real caller (a block-requiring method
   invoked without one raises), but must still compile. Caller emits nil. */
int is_blockless_block_param_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id >= 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && sp_streq(rn, g_block_param_name);
}

/* Emit a call to the forwarded real-proc block (g_yield_proc_ref) with the
   given args -- shared by `yield` and `<blk>.call` inside a method inlined with
   a forwarded materialized proc. as_expr=0 emits a statement (value discarded);
   as_expr=1 emits a value expression, unboxed to result_ty when concrete.
   sp_proc_call returns a raw carrier; the poly result rides _sp_proc_poly_ret. */
void emit_yield_proc_call(Compiler *c, int args_node, TyKind result_ty, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  int yargc = 0;
  const int *yargv = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &yargc) : NULL;
  if (!as_expr) {
    emit_indent(b, indent);
    buf_printf(b, "sp_proc_call(%s, ", g_yield_proc_ref);
    emit_proc_call_args(c, yargc, yargv, b, 1);
    buf_puts(b, ";\n");
    return;
  }
  Buf cb; memset(&cb, 0, sizeof cb);
  buf_printf(&cb, "((void)sp_proc_call(%s, ", g_yield_proc_ref);
  emit_proc_call_args(c, yargc, yargv, &cb, 1);
  buf_puts(&cb, ", _sp_proc_poly_ret)");
  /* The result rides a single global, so two yields in one expression race:
     C does not sequence a call's arguments, and `yield(1) + yield(2)` could
     run both calls before either read _sp_proc_poly_ret. Capture each into
     its own temp in the statement prelude, where the call and the read stay
     adjacent. (#3399) */
  if (g_pre) {
    int yt = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
               yt, cb.p ? cb.p : "sp_box_nil()", yt);
    char tref[24]; snprintf(tref, sizeof tref, "_t%d", yt);
    if (result_ty == TY_POLY || result_ty == TY_UNKNOWN) buf_puts(b, tref);
    else emit_unbox_text(c, result_ty, tref, b);
    free(cb.p);
    return;
  }
  if (result_ty == TY_POLY || result_ty == TY_UNKNOWN) buf_puts(b, cb.p ? cb.p : "");
  else emit_unbox_text(c, result_ty, cb.p ? cb.p : "", b);
  free(cb.p);
}

/* Expand the active block's body, binding its params to the given call
   args. Shared by YieldNode and `block.call`. `as_expr` wraps in ({...}). */
/* Emit a block-arg source node coerced to the block param's slot type,
   mirroring the box/unbox handling of the requireds binding arm. */
static void emit_block_arg_coerced(Compiler *c, int node, TyKind ot, Buf *b) {
  TyKind at = comp_ntype(c, node);
  if (ot == TY_POLY && at != TY_POLY && at != TY_UNKNOWN) emit_boxed(c, node, b);
  else if (at == TY_POLY && ot != TY_POLY && ot != TY_UNKNOWN) {
    Buf t; memset(&t, 0, sizeof t); emit_expr(c, node, &t);
    emit_unbox_text(c, ot, t.p ? t.p : "", b); free(t.p);
  }
  else emit_expr(c, node, b);
}

/* A tail CALL whose STATEMENT form drops the value the splice is read for:
   tap/then/yield_self answer the receiver or the block's value, and an
   iterator that answers its receiver cannot re-read a computed one. The
   method-tail path already routes both to the value path (codegen_stmt.c);
   a spliced block's tail needed the same, or the statement expression took
   whatever the last emitted statement happened to leave (#4155). */
static int block_tail_needs_value_form(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") < 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  if (sp_streq(nm, "tap") || sp_streq(nm, "then") || sp_streq(nm, "yield_self"))
    return nt_ref(nt, id, "receiver") >= 0;
  return iter_value_answers_recv(c, id) && tail_iter_receiver(c, id) < 0;
}

void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr,
                       TyKind want_ty) {
  /* want_ty: the consumer's slot type for the block's value (the YieldNode's
     unified type). A poly slot must receive sp_RbVal even when THIS block's
     tail is concrete (a yield-result union of an rbs-seeded Hash and a class
     instance reached the boxed slot without a box, #3278). */
  int want_poly = as_expr && want_ty == TY_POLY;
  const NodeTable *nt = c->nt;
  int blk = g_block_id;
  int bbody = nt_ref(nt, blk, "body");
  int yc = 0;
  const int *yargs = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &yc) : NULL;
  Scope *bsc = comp_scope_of(c, blk);
  /* The spliced body and the block's own parameter NAMES resolve at the
     block's DEFINITION-site rename depth (g_block_nren): the entries the
     enclosing method-inline pushed above that mark must not capture
     same-named block locals (a numbered `_1` used by both the callee's own
     block and the caller's, #3281). Block-side text emits with the callee's
     entries PARKED (copied out, count truncated) so a nested inline inside
     the body cannot clobber them; method-side (yield-arg) text restores
     them. */
  int cs_nren = g_nren;
  int bi_nren = g_block_nren < cs_nren ? g_block_nren : cs_nren;
  int bi_cnt = cs_nren - bi_nren;
  int bi_parked = 0;
  char (*bi_pf)[96] = NULL;
  char (*bi_pt)[112] = NULL;
  if (bi_cnt > 0) {
    bi_pf = malloc(sizeof(char[96]) * (size_t)bi_cnt);
    bi_pt = malloc(sizeof(char[112]) * (size_t)bi_cnt);
  }
  #define BI_BLOCK_SIDE() do { \
    if (bi_pf && !bi_parked) { \
      memcpy(bi_pf, g_ren_from + bi_nren, sizeof(char[96]) * (size_t)bi_cnt); \
      memcpy(bi_pt, g_ren_to + bi_nren, sizeof(char[112]) * (size_t)bi_cnt); \
      bi_parked = 1; \
    } \
    g_nren = bi_nren; \
  } while (0)
  #define BI_METHOD_SIDE() do { \
    if (bi_pf && bi_parked) { \
      memcpy(g_ren_from + bi_nren, bi_pf, sizeof(char[96]) * (size_t)bi_cnt); \
      memcpy(g_ren_to + bi_nren, bi_pt, sizeof(char[112]) * (size_t)bi_cnt); \
      bi_parked = 0; \
    } \
    g_nren = cs_nren; \
  } while (0)
  /* CRuby's argument distribution needs the parameter shape up front:
     P pre-required, O optionals, Q post-required, R any rest marker
     (`*name`, bare `*`, or the implicit rest of a trailing comma `|a, |`).
     Requireds (pre and post) bind first, optionals take what remains
     left-to-right, a rest collects the leftover middle, extras drop. */
  int P = 0; while (block_param_name(c, blk, P)) P++;
  int O = 0; while (block_opt_name(c, blk, O)) O++;
  int Q = 0; while (block_post_name(c, blk, Q)) Q++;
  int R = block_rest_marker(c, blk);
  /* `yield(*arr)`: a single splat spreads the array across the block params
     (auto-splat). Evaluate it once into a rooted temp and bind each param (and
     any rest param) from its elements rather than from the splat AST node. */
  int splat_tmp = -1; TyKind splat_at = TY_UNKNOWN;
  int poly_splat_tmp = -1;   /* a boxed yielded value splatted at run time */
  if (yc == 1 && yargs) {
    int inner = -1;
    if (nt_type(nt, yargs[0]) && sp_streq(nt_type(nt, yargs[0]), "SplatNode"))
      inner = nt_ref(nt, yargs[0], "expression");
    /* CRuby auto-splat: a single (non-splat) Array yielded to a block taking
       more than one binding slot -- or at least one slot plus a rest marker --
       destructures across the params. Only-rest blocks (`|*a|`) keep the
       array whole, as does a single plain param. */
    else if (P + O + Q > 1 || (P + O + Q >= 1 && R))
      inner = yargs[0];
    TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
    /* A BOXED yielded value can be an array too, and CRuby splats it just the
       same. Only a statically typed array was splatted, so a method yielding
       what it read out of a boxed container -- Set#each over its element array
       -- bound the whole element to the first parameter and nil to the rest
       (#3944). Decide it at run time. */
    if (at == TY_POLY && inner >= 0) {
      poly_splat_tmp = ++g_tmp;
      Buf pb2; memset(&pb2, 0, sizeof pb2); emit_expr(c, inner, &pb2);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 poly_splat_tmp, pb2.p ? pb2.p : "sp_box_nil()", poly_splat_tmp);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "int _fs%d = (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id));\n",
                 poly_splat_tmp, poly_splat_tmp, poly_splat_tmp);
      free(pb2.p);
    }
    else if (ty_is_array(at) || at == TY_POLY_ARRAY) {
      splat_at = at;
      splat_tmp = ++g_tmp;
      Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, inner, &sb);
      emit_indent(g_pre, g_indent);
      emit_ctype(c, at, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", splat_tmp, sb.p ? sb.p : "");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", splat_tmp);
      free(sb.p);
    }
  }
  if (as_expr) buf_puts(b, "({ ");
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    /* The block's own param name resolves at the block's definition depth:
       renames pushed by the enclosing method-inline must not capture it. */
    char bprbuf[160];
    BI_BLOCK_SIDE();
    snprintf(bprbuf, sizeof bprbuf, "%s", rename_local(bp));
    BI_METHOD_SIDE();
    const char *bpr = bprbuf;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", bpr);
    if (poly_splat_tmp >= 0) {
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      char psrc[160];
      if (k == 0)
        snprintf(psrc, sizeof psrc, "(_fs%d ? sp_poly_index_poly(_t%d, sp_box_int(0)) : _t%d)",
                 poly_splat_tmp, poly_splat_tmp, poly_splat_tmp);
      else
        snprintf(psrc, sizeof psrc, "(_fs%d ? sp_poly_index_poly(_t%d, sp_box_int(%d)) : sp_box_nil())",
                 poly_splat_tmp, poly_splat_tmp, k);
      if (bt == TY_POLY || bt == TY_UNKNOWN) buf_puts(b, psrc);
      else emit_unbox_text(c, bt, psrc, b);
    }
    else if (splat_tmp >= 0) {
      /* element k of the splatted array, guarded: when the array is shorter
         than the param list the surplus params bind nil (CRuby auto-splat),
         using the same per-slot default the non-splat under-fill path does. */
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      TyKind et = ty_array_elem(splat_at);
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_array_elem_at(splat_at, splat_tmp, k, &eb);
      buf_printf(b, "(%d < (_t%d ? _t%d->len : 0) ? ", k, splat_tmp, splat_tmp);
      if (bt == TY_POLY && et != TY_POLY && et != TY_UNKNOWN)
        emit_boxed_text(c, et, eb.p ? eb.p : "0", b);
      else if (et == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN)
        emit_unbox_text(c, bt, eb.p ? eb.p : "", b);
      else
        buf_puts(b, eb.p ? eb.p : "");
      buf_printf(b, " : %s)", bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
      free(eb.p);
    }
    else if (k < yc) {
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      TyKind at = comp_ntype(c, yargs[k]);
      if (bt == TY_POLY && at != TY_POLY && at != TY_UNKNOWN)
        emit_boxed(c, yargs[k], b);
      else if (at == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN) {
        /* a poly yield value into a scalar (e.g. int, non-widened) block param:
           unbox down to the slot type (the reverse of the box arm above). */
        Buf yb; memset(&yb, 0, sizeof yb); emit_expr(c, yargs[k], &yb);
        emit_unbox_text(c, bt, yb.p ? yb.p : "", b); free(yb.p);
      }
      else
        emit_expr(c, yargs[k], b);
    }
    else {
      LocalVar *bl = scope_local(bsc, bp);
      TyKind bt = bl ? bl->type : TY_INT;
      buf_puts(b, bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Distribution counts. Direct (non-splat) yields resolve statically from
     yc; a splatted array's length is runtime, so the optional-take count and
     the post start index become runtime temps. */
  int ot_static = yc - P - Q;
  if (ot_static < 0) ot_static = 0;
  if (ot_static > O) ot_static = O;
  int rl_static = R ? yc - P - ot_static - Q : 0;
  if (rl_static < 0) rl_static = 0;
  int t_ot = -1;
  if (splat_tmp >= 0 && (O > 0 || Q > 0)) {
    t_ot = ++g_tmp;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "sp_int _t%d = (_t%d ? _t%d->len : 0) - %d - %d;"
                  " if (_t%d < 0) _t%d = 0; if (_t%d > %d) _t%d = %d;%s",
               t_ot, splat_tmp, splat_tmp, P, Q,
               t_ot, t_ot, t_ot, O, t_ot, O, as_expr ? " " : "\n");
  }
  /* Optional block params (`|a, b=10|`): bind from the args left over after
     the requireds (pre AND post) are satisfied, else the declared default. */
  for (int oi = 0; ; oi++) {
    const char *op = block_opt_name(c, blk, oi);
    if (!op) break;
    char oprbuf[160];
    BI_BLOCK_SIDE();
    snprintf(oprbuf, sizeof oprbuf, "%s", rename_local(op));
    BI_METHOD_SIDE();
    const char *opr = oprbuf;
    LocalVar *ol = bsc ? scope_local(bsc, op) : NULL;
    TyKind ot = ol ? ol->type : TY_UNKNOWN;
    int dv = block_opt_default(c, blk, oi);
    int yi = P + oi;
    const char *odflt = ot == TY_RANGE ? "(sp_Range){0}" : default_value(ot);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", opr);
    if (splat_tmp >= 0) {
      TyKind et = ty_array_elem(splat_at);
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_array_elem_at(splat_at, splat_tmp, yi, &eb);
      buf_printf(b, "(%d < _t%d ? ", oi, t_ot);
      if (ot == TY_POLY && et != TY_POLY && et != TY_UNKNOWN) emit_boxed_text(c, et, eb.p ? eb.p : "0", b);
      else if (et == TY_POLY && ot != TY_POLY && ot != TY_UNKNOWN) emit_unbox_text(c, ot, eb.p ? eb.p : "", b);
      else buf_puts(b, eb.p ? eb.p : "");
      buf_puts(b, " : ");
      if (dv >= 0) { BI_BLOCK_SIDE(); emit_block_arg_coerced(c, dv, ot, b); BI_METHOD_SIDE(); }
      else buf_puts(b, odflt);
      buf_puts(b, ")");
      free(eb.p);
    }
    else if (oi < ot_static) {
      emit_block_arg_coerced(c, yargs[yi], ot, b);
    }
    else if (dv >= 0) {
      BI_BLOCK_SIDE(); emit_block_arg_coerced(c, dv, ot, b); BI_METHOD_SIDE();
    }
    else {
      buf_puts(b, odflt);
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Keyword block params (`|a:, b: 5|`): match the trailing yielded kwargs hash
     by name; an omitted optional keyword takes its declared default. */
  int ykw = (yc > 0 && yargs && nt_type(nt, yargs[yc - 1]) &&
             sp_streq(nt_type(nt, yargs[yc - 1]), "KeywordHashNode")) ? yargs[yc - 1] : -1;
  for (int ki = 0; ; ki++) {
    const char *kp = block_keyword_name(c, blk, ki);
    if (!kp) break;
    char kprbuf[160];
    BI_BLOCK_SIDE();
    snprintf(kprbuf, sizeof kprbuf, "%s", rename_local(kp));
    BI_METHOD_SIDE();
    const char *kpr = kprbuf;
    LocalVar *kl = bsc ? scope_local(bsc, kp) : NULL;
    TyKind kt = kl ? kl->type : TY_UNKNOWN;
    int vn = ykw >= 0 ? ie_kwhash_value(c, ykw, kp) : -1;
    int dv = block_keyword_default(c, blk, ki);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", kpr);
    if (vn >= 0) emit_block_arg_coerced(c, vn, kt, b);
    else if (dv >= 0) { BI_BLOCK_SIDE(); emit_block_arg_coerced(c, dv, kt, b); BI_METHOD_SIDE(); }
    else buf_puts(b, kt == TY_RANGE ? "(sp_Range){0}" : default_value(kt));
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* `**kw` keyword-rest: the remaining pairs of the trailing yielded kwargs
     hash (those no named keyword param consumed), or a fresh empty hash --
     CRuby binds {}, never nil. Built into a temp and assigned last, like the
     positional rest below. */
  {
    const char *kwr = block_kwrest_name(c, blk);
    if (kwr) {
      char kwrrbuf[160];
      BI_BLOCK_SIDE();
      snprintf(kwrrbuf, sizeof kwrrbuf, "%s", rename_local(kwr));
      BI_METHOD_SIDE();
      const char *kwrr = kwrrbuf;
      int tkw = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new();%s", tkw, as_expr ? " " : "\n");
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "SP_GC_ROOT(_t%d);%s", tkw, as_expr ? " " : "\n");
      if (ykw >= 0) {
        int en2 = 0; const int *els2 = nt_arr(nt, ykw, "elements", &en2);
        for (int e2 = 0; e2 < en2; e2++) {
          if (!nt_type(nt, els2[e2]) || !sp_streq(nt_type(nt, els2[e2]), "AssocNode")) continue;
          int kn2 = nt_ref(nt, els2[e2], "key");
          int vn2 = nt_ref(nt, els2[e2], "value");
          if (kn2 < 0 || vn2 < 0) continue;
          /* a pair consumed by a named keyword param stays out of the rest */
          const char *ksym = nt_type(nt, kn2) && sp_streq(nt_type(nt, kn2), "SymbolNode")
                               ? nt_str(nt, kn2, "value") : NULL;
          int consumed = 0;
          for (int ki2 = 0; ksym; ki2++) {
            const char *kp2 = block_keyword_name(c, blk, ki2);
            if (!kp2) break;
            if (sp_streq(kp2, ksym) ||
                (strstr(kp2, "__bp") && !strncmp(kp2, ksym, strlen(ksym)) &&
                 !strncmp(kp2 + strlen(ksym), "__bp", 4))) { consumed = 1; break; }
          }
          if (consumed) continue;
          if (!as_expr) emit_indent(b, indent);
          buf_printf(b, "sp_PolyPolyHash_set(_t%d, ", tkw);
          emit_boxed(c, kn2, b);
          buf_puts(b, ", ");
          emit_boxed(c, vn2, b);
          buf_puts(b, as_expr ? "); " : ");\n");
        }
      }
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "lv_%s = _t%d;%s", kwrr, tkw, as_expr ? " " : "\n");
    }
  }
  /* A trailing rest parameter (`|*a|`) collects the yielded arguments past the
     requireds into a fresh array. */
  const char *brest = block_rest_name(c, blk);
  if (brest) {
    char brestrbuf[160];
    BI_BLOCK_SIDE();
    snprintf(brestrbuf, sizeof brestrbuf, "%s", rename_local(brest));
    BI_METHOD_SIDE();
    const char *brestr = brestrbuf;
    /* Build into a fresh temp, assign the rest param LAST: a yielded arg can
       reference the same (renamed) C slot the rest param occupies -- e.g. a
       sole-rest block whose name collides with the inlined method's own
       param (`def m(a) yield a end; m(x) { |*a| a }`). Assigning the slot
       first made the push read the fresh empty array as its own element. */
    int trest = ++g_tmp;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new();%s", trest, as_expr ? " " : "\n");
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "SP_GC_ROOT(_t%d);%s", trest, as_expr ? " " : "\n");
    if (splat_tmp >= 0) {
      /* collect the leftover middle: past the pre-requireds and the taken
         optionals, stopping short of the Q post-requireds */
      TyKind et = ty_array_elem(splat_at);
      int jj = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      if (t_ot >= 0)
        buf_printf(b, "for (sp_int _t%d = %d + _t%d; _t%d && _t%d < (_t%d->len - %d); _t%d++) sp_PolyArray_push(_t%d, ",
                   jj, P, t_ot, splat_tmp, jj, splat_tmp, Q, jj, trest);
      else
        buf_printf(b, "for (sp_int _t%d = %d; _t%d && _t%d < (_t%d->len - %d); _t%d++) sp_PolyArray_push(_t%d, ",
                   jj, P, splat_tmp, jj, splat_tmp, Q, jj, trest);
      char acc[96];
      if (splat_at == TY_POLY_ARRAY) snprintf(acc, sizeof acc, "sp_PolyArray_get(_t%d, _t%d)", splat_tmp, jj);
      else snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d)", array_kind(splat_at) ? array_kind(splat_at) : "Int", splat_tmp, jj);
      if (splat_at == TY_POLY_ARRAY) buf_puts(b, acc);
      else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, acc, &bx); buf_puts(b, bx.p ? bx.p : acc); free(bx.p); }
      buf_puts(b, as_expr ? "); " : ");\n");
    }
    else for (int j = P + ot_static; j < yc - Q; j++) {
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "sp_PolyArray_push(_t%d, ", trest);
      emit_boxed(c, yargs[j], b);
      buf_puts(b, as_expr ? "); " : ");\n");
    }
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = _t%d;%s", brestr, trest, as_expr ? " " : "\n");
  }
  /* Post-required params (`|a, *b, c, d|`): bind after the pre/optional/rest
     consumption point, left-to-right; missing positions bind the slot nil. */
  if (Q > 0) {
    int t_ps = -1;
    if (splat_tmp >= 0) {
      t_ps = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      if (R) {
        /* a rest absorbs the middle: posts sit at len-Q, clamped down to the
           pre/optional consumption point when the array is short */
        buf_printf(b, "sp_int _t%d = (_t%d ? _t%d->len : 0) - %d;"
                      " { sp_int _lo = %d%s%s%d; if (_t%d < _lo) _t%d = _lo; }%s",
                   t_ps, splat_tmp, splat_tmp, Q,
                   P, t_ot >= 0 ? " + _t" : " + ", t_ot >= 0 ? "" : "0",
                   t_ot >= 0 ? t_ot : 0, t_ps, t_ps, as_expr ? " " : "\n");
      }
      else {
        buf_printf(b, "sp_int _t%d = %d%s%d;%s",
                   t_ps, P, t_ot >= 0 ? " + _t" : " + ", t_ot >= 0 ? t_ot : 0,
                   as_expr ? " " : "\n");
      }
    }
    int ps_static = P + ot_static + rl_static;
    for (int qi = 0; qi < Q; qi++) {
      const char *qp = block_post_name(c, blk, qi);
      if (!qp) continue;   /* anonymous post: consumes a slot, binds nothing */
      char qprbuf[160];
      BI_BLOCK_SIDE();
      snprintf(qprbuf, sizeof qprbuf, "%s", rename_local(qp));
      BI_METHOD_SIDE();
      const char *qpr = qprbuf;
      LocalVar *ql = bsc ? scope_local(bsc, qp) : NULL;
      TyKind qt = ql ? ql->type : TY_UNKNOWN;
      const char *qdflt = qt == TY_RANGE ? "(sp_Range){0}" : default_value(qt);
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "lv_%s = ", qpr);
      if (splat_tmp >= 0) {
        TyKind et = ty_array_elem(splat_at);
        int te = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = _t%d + %d; (_t%d < (_t%d ? _t%d->len : 0) ? ",
                   te, t_ps, qi, te, splat_tmp, splat_tmp);
        char acc[96];
        if (splat_at == TY_POLY_ARRAY) snprintf(acc, sizeof acc, "sp_PolyArray_get(_t%d, _t%d)", splat_tmp, te);
        else snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d)", array_kind(splat_at) ? array_kind(splat_at) : "Int", splat_tmp, te);
        if (qt == TY_POLY && et != TY_POLY && et != TY_UNKNOWN) {
          Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, acc, &bx);
          buf_puts(b, bx.p ? bx.p : acc); free(bx.p);
        }
        else if (et == TY_POLY && qt != TY_POLY && qt != TY_UNKNOWN)
          emit_unbox_text(c, qt, acc, b);
        else buf_puts(b, acc);
        buf_printf(b, " : %s); })", qdflt);
      }
      else {
        int idx = ps_static + qi;
        if (idx < yc) {
          LocalVar *bl2 = ql;
          TyKind at = comp_ntype(c, yargs[idx]);
          TyKind bt2 = bl2 ? bl2->type : TY_UNKNOWN;
          if (bt2 == TY_POLY && at != TY_POLY && at != TY_UNKNOWN) emit_boxed(c, yargs[idx], b);
          else if (at == TY_POLY && bt2 != TY_POLY && bt2 != TY_UNKNOWN) {
            Buf yb; memset(&yb, 0, sizeof yb); emit_expr(c, yargs[idx], &yb);
            emit_unbox_text(c, bt2, yb.p ? yb.p : "", b); free(yb.p);
          }
          else emit_expr(c, yargs[idx], b);
        }
        else buf_puts(b, qdflt);
      }
      buf_puts(b, as_expr ? "; " : ";\n");
    }
  }
  /* Ruby evaluates every yielded argument for its side effects, even ones no
     block param binds -- an empty or under-arity block still runs the arg
     expression (`yield(@f = Foo.new)` must set @f). The loops above emitted only
     the bound args; a rest param collects and thereby evaluates the middle, but
     with no rest those dropped middle args would be lost. Evaluate them here for
     effect (#3209). */
  if (splat_tmp < 0 && !brest) {
    for (int j = P + ot_static; j < yc - Q; j++) {
      /* a trailing kwargs hash consumed by keyword params / **kwrest is not a
         dropped positional -- it was already read above */
      if (j == ykw && (block_keyword_name(c, blk, 0) || block_kwrest_name(c, blk))) continue;
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, yargs[j], &vb);
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "(void)(%s)%s", vb.p ? vb.p : "0", as_expr ? "; " : ";\n");
      free(vb.p);
    }
  }
  /* Keep the rename table active for the block body: the block's variable
     references are in the same lexical scope as the surrounding inlined
     method, so renames like x → _y3_x must stay visible. Nested inlines
     inside the block body append at the current g_nren and self-restore.
     Set g_block_id to the fallback (the block active before the enclosing
     inline started) so that a nested `yield` inside the block chains to
     the outermost caller's block rather than going dead. */
  int svb = g_block_id; g_block_id = g_yield_block_fallback;
  /* The fallback has to move out one level with it. Leaving it pointing at
     the block now being spliced makes that block its OWN fallback, so a yield
     inside its body re-splices the same body -- forever, until the compiler
     runs out of C stack. Only one fallback level is ever recorded, so once it
     is consumed there is no outer block left to name: -1, not itself.
     Reached by a method that both yields and recurses through a block that
     forwards the yield (`def walk; yield self; @kids.each { |k| k.walk { |x|
     yield x } }; end`) -- valid Ruby that segfaulted the compiler. */
  int svfb = g_yield_block_fallback; g_yield_block_fallback = -1;
  /* the body is block-definition-site code: emit it below the callee's
     rename entries, and pair the fallback block with ITS depth so a nested
     yield inside the body splices at the right level */
  BI_BLOCK_SIDE();
  int sv_bnren = g_block_nren; g_block_nren = g_yield_block_fallback_nren;
  const char *svbpn = g_block_param_name; g_block_param_name = NULL;
  /* the block body executes in its DEFINITION site's break scope: a
     top-level break targets the call that received the block, not whatever
     loop/iterator surrounds this yield inside the method body */
  const char *svser = g_brk_ser_var; g_brk_ser_var = g_block_brk_var;
  int svebase = g_brk_ensure_base; g_brk_ensure_base = g_block_brk_ebase;
  int svbexc = g_brk_exc_base; g_brk_exc_base = g_block_brk_exc_base;
  const char *svbbv = g_block_brk_var; g_block_brk_var = g_yield_blk_brk_fallback;
  int svbbe = g_block_brk_ebase; g_block_brk_ebase = g_yield_blk_brk_efallback;
  /* The block body lexically belongs to the REAL enclosing function: a
     `return` inside it exits that method, not the inlined region -- so the
     inline funnel (if one is active) is suspended in favor of the real
     function's own return funnel. */
  const char *sv_bl = g_method_pr_label, *sv_bv = g_method_pr_var;
  TyKind sv_bt = g_ret_type;
  int sv_bexc = g_method_pr_exc_depth;
  g_method_pr_label = g_fn_pr_label; g_method_pr_var = g_fn_pr_var;
  g_ret_type = g_fn_ret_type;
  g_method_pr_exc_depth = 0;   /* the real function's funnel sits at depth 0 */
  /* likewise, the block body's `self` is the CALLER's (an ivar read inside
     the block must not resolve against the inlined method's receiver) — and
     so is the block body's emitting-class, so an implicit-self *call* in the
     block resolves against the caller's class, not the receiver's. */
  const char *sv_bself = g_self, *sv_bderef = g_self_deref;
  int sv_bemcls = g_emitting_class_id;
  if (g_yield_self_fallback) {
    g_self = g_yield_self_fallback;
    g_self_deref = g_yield_self_deref_fallback;
    g_emitting_class_id = g_yield_emitting_class_fallback;
  }
  /* ... and the caller's lowered context: a `yield` in this spliced caller
     code binds the enclosing lowered method's proc, not this inline. */
  int sv_blow = g_current_scope_is_lowered;
  const char *sv_blbn = g_lowered_blk_name;
  g_current_scope_is_lowered = g_yield_lowered_fallback;
  g_lowered_blk_name = g_yield_lowered_blk_fallback;
  /* A `next` in a yielded block leaves the BLOCK with its value -- but this
     body is spliced inline (no _proc_ function, no loop), so a bare
     `continue` is invalid C. Only when the body owns a `next`, wrap the
     splice in do{}while(0) and route the value through a temp via the
     inline-each next-var machinery; blocks without `next` keep their exact
     previous emission. */
  int nx_own = subtree_has_own_next(nt, bbody);
  const char *sv_nx2 = g_ie_next_var; int sv_poly2 = g_ie_res_poly;
  int sv_lexc2 = g_loop_exc_base;
  int sv_lens2 = g_loop_ensure_base;
  char nxbuf[32]; int nx_tmp = 0;
  int bn3 = 0; const int *bd3 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn3) : NULL;
  TyKind nx_bt = TY_NIL; int nx_tail_stmt = 0;
  if (nx_own) {
    g_loop_exc_base = g_exc_frame_depth;
    g_loop_ensure_base = g_ensure_depth;
    g_c_loop_depth++;
    if (as_expr) {
      nx_bt = bn3 > 0 ? comp_ntype(c, bd3[bn3 - 1]) : TY_NIL;
      if (bn3 > 0) {
        const char *tty3 = nt_type(nt, bd3[bn3 - 1]);
        nx_tail_stmt = tty3 && (sp_streq(tty3, "IfNode") || sp_streq(tty3, "CaseNode") ||
                                sp_streq(tty3, "WhileNode") || sp_streq(tty3, "UntilNode") ||
                                sp_streq(tty3, "BeginNode") || sp_streq(tty3, "NextNode") ||
                                sp_streq(tty3, "ReturnNode"));
      }
      nx_tmp = ++g_tmp;
      snprintf(nxbuf, sizeof nxbuf, "_t%d", nx_tmp);
      g_ie_next_var = nxbuf;
      g_ie_res_poly = (nx_bt == TY_POLY || (want_poly && ty_is_object(nx_bt)));
      if (g_ie_res_poly) buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); ", nx_tmp);
      else if (nx_bt == TY_INT || nx_bt == TY_BOOL || nx_bt == TY_SYMBOL)
        buf_printf(b, "sp_int _t%d = SP_INT_NIL; ", nx_tmp);
      else if (proc_slot_is_ptr(nx_bt)) {
        emit_ctype(c, nx_bt, b); buf_printf(b, " _t%d = NULL; ", nx_tmp);
      }
      else {
        /* type-opaque tail (e.g. the body IS the `next`): ride the sp_int
           carrier with the nil sentinel; the next-var stays active so a
           valued `next` still delivers. */
        buf_printf(b, "sp_int _t%d = SP_INT_NIL; ", nx_tmp);
      }
      buf_puts(b, "do { ");
    }
    else {
      g_ie_next_var = NULL; g_ie_res_poly = 0;
      emit_indent(b, indent); buf_puts(b, "do {\n");
    }
  }
  if (nx_own && as_expr && g_ie_next_var && !nx_tail_stmt && bn3 > 0) {
    for (int k3 = 0; k3 < bn3 - 1; k3++) emit_stmt(c, bd3[k3], b, 0);
    buf_printf(b, "%s = ", nxbuf);
    if (g_ie_res_poly) emit_boxed(c, bd3[bn3 - 1], b);
    else emit_expr(c, bd3[bn3 - 1], b);
    buf_puts(b, "; ");
  }
  else if (as_expr && !nx_own && bn3 > 0 &&
           nt_type(nt, bd3[bn3 - 1]) &&
           (sp_streq(nt_type(nt, bd3[bn3 - 1]), "IfNode") ||
            sp_streq(nt_type(nt, bd3[bn3 - 1]), "UnlessNode") ||
            sp_streq(nt_type(nt, bd3[bn3 - 1]), "CaseNode") ||
            sp_streq(nt_type(nt, bd3[bn3 - 1]), "CaseMatchNode") ||
            /* `wrap { risky rescue fallback }`: the statement form of a
               rescue modifier is an if/else over setjmp, whose two arms
               compute the value and drop it. Same void tail as the rest. */
            sp_streq(nt_type(nt, bd3[bn3 - 1]), "RescueModifierNode") ||
            sp_streq(nt_type(nt, bd3[bn3 - 1]), "BeginNode") ||
            block_tail_needs_value_form(c, bd3[bn3 - 1]))) {
    /* A GNU statement-expression's value is its last statement only when that
       statement is an EXPRESSION; a trailing if/case/begin STATEMENT yields
       void, so a block whose value is such a construct (`wrap { if c then a
       else b end }`) produced a void ({...}). Emit the tail value-compound as
       an expression (a bare-expression tail already carries its value). */
    if (c->blk_body_map && bbody >= 0 && bbody < c->nt->count &&
        c->blk_body_map[bbody] >= 0)
      emit_block_locals_reset(c, c->blk_body_map[bbody], b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) emit_stmt(c, bd3[k3], b, 0);
    if (want_poly && ty_is_object(comp_ntype(c, bd3[bn3 - 1]))) emit_boxed(c, bd3[bn3 - 1], b);
    else emit_expr(c, bd3[bn3 - 1], b);
    buf_puts(b, "; ");
  }
  else if (as_expr && !nx_own && want_poly && bn3 > 0 &&
           (ty_is_object(comp_ntype(c, bd3[bn3 - 1])) ||
            /* A CALL whose value the statement form does not carry: `p x`
               emits as fputs + putchar, so the statement expression's value is
               putchar's int rather than the call's own. Emitting the tail as an
               expression is what makes the splice's value the block's value
               (#3781). */
            (nt_type(nt, bd3[bn3 - 1]) && sp_streq(nt_type(nt, bd3[bn3 - 1]), "CallNode") &&
             comp_ntype(c, bd3[bn3 - 1]) == TY_POLY)) &&
           nt_type(nt, bd3[bn3 - 1]) &&
           !sp_streq(nt_type(nt, bd3[bn3 - 1]), "ReturnNode")) {
    /* concrete-typed bare tail into a poly slot: box it (#3278) */
    if (c->blk_body_map && bbody >= 0 && bbody < c->nt->count &&
        c->blk_body_map[bbody] >= 0)
      emit_block_locals_reset(c, c->blk_body_map[bbody], b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) emit_stmt(c, bd3[k3], b, 0);
    emit_boxed(c, bd3[bn3 - 1], b);
    buf_puts(b, "; ");
  }
  else {
    emit_stmts(c, bbody, b, as_expr ? 0 : (nx_own ? indent + 1 : indent));
    /* The block's value is its last statement, and this splice is read as the
       value of a statement expression. A receiver-returning iterator there
       emits as a loop with no value, so the slot it feeds gets void (or, for
       an Array receiver, the loop counter). Put the receiver back. */
    if (as_expr && bbody >= 0) {
      int bn4 = 0; const int *bd4 = nt_arr(c->nt, bbody, "body", &bn4);
      int rr4 = (bd4 && bn4 > 0) ? tail_iter_receiver(c, bd4[bn4 - 1]) : -1;
      if (rr4 >= 0) { emit_expr(c, rr4, b); buf_puts(b, "; "); }
    }
  }
  if (nx_own) {
    g_c_loop_depth--;
    g_loop_exc_base = sv_lexc2;
    g_loop_ensure_base = sv_lens2;
    if (as_expr) buf_printf(b, "} while(0); %s; ", g_ie_next_var ? nxbuf : "(void)0");
    else { emit_indent(b, indent); buf_puts(b, "} while(0);\n"); }
    g_ie_next_var = sv_nx2; g_ie_res_poly = sv_poly2;
  }
  g_self = sv_bself; g_self_deref = sv_bderef;
  g_emitting_class_id = sv_bemcls;
  g_current_scope_is_lowered = sv_blow;
  g_lowered_blk_name = sv_blbn;
  g_method_pr_label = sv_bl; g_method_pr_var = sv_bv; g_ret_type = sv_bt;
  g_method_pr_exc_depth = sv_bexc;
  g_brk_ser_var = svser; g_brk_ensure_base = svebase; g_brk_exc_base = svbexc;
  g_block_brk_var = svbbv; g_block_brk_ebase = svbbe;
  g_block_nren = sv_bnren;
  BI_METHOD_SIDE();
  g_block_id = svb; g_yield_block_fallback = svfb; g_block_param_name = svbpn;
  if (as_expr) {
    /* `{ return e }`: the block exits the enclosing function, so the
       statement-expr's tail is unreachable — but C still needs a value
       expression there (a trailing `return;` makes the ({...}) void). */
    int bn2 = 0; const int *bd2 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn2) : NULL;
    if (bn2 > 0 && nt_type(nt, bd2[bn2 - 1]) &&
        sp_streq(nt_type(nt, bd2[bn2 - 1]), "ReturnNode")) {
      int ra = nt_ref(nt, bd2[bn2 - 1], "arguments");
      int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
      TyKind rt2 = rn > 0 ? comp_ntype(c, rv[0]) : TY_INT;
      buf_printf(b, " %s;", default_value(is_scalar_ret(rt2) ? rt2 : TY_INT));
    }
    buf_puts(b, "})");
  }
  free(bi_pf); free(bi_pt);
  #undef BI_BLOCK_SIDE
  #undef BI_METHOD_SIDE
}

/* Inline a yielding method call in expression position: ({ ...; value; }).
   The method must return a usable value (its body's last statement). */
/* poly-receiver block dispatch (#2448): `x.m { block }` where x is a poly
   value (a heterogeneous-array element, an un-narrowed hash value) and m is a
   block-forwarding/yielding user method. The per-arm inline needs a concrete
   self, so hoist the boxed receiver once, then emit a cls_id switch inlining m
   per instantiated user class that defines it -- self bound to the cast
   pointer via g_inline_recv_expr. Returns 1 if handled. */
int emit_poly_recv_block_dispatch(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int block = nt_ref(nt, id, "block");
  if (!name || recv < 0 || block < 0) return 0;
  if (!nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  if (comp_ntype(c, recv) != TY_POLY) return 0;
  /* Only receivers whose poly value comes out of a BUILTIN container -- an
     index read (`arr[i]`) or an element accessor (first/last/fetch/...) -- or
     a plain local/ivar holding such. A constant (its own const-inline path),
     or a USER method-call receiver (an accessor returning a class/object, with
     its own direct / Stage-2 dispatch), must not be preempted by this runtime
     cls_id switch (#2448). */
  {
    const char *rvty = nt_type(nt, recv);
    if (!rvty) return 0;
    if (sp_streq(rvty, "LocalVariableReadNode") || sp_streq(rvty, "InstanceVariableReadNode")) {
      /* ok: a plain variable holding a poly value */
    }
    else if (sp_streq(rvty, "CallNode")) {
      const char *rmn = nt_str(nt, recv, "name");
      static const char *const CONT[] = {"[]", "first", "last", "fetch", "sample",
        "dig", "shift", "pop", "min", "max", "at", NULL};
      int ok = 0;
      for (int i = 0; rmn && CONT[i]; i++) if (sp_streq(rmn, CONT[i])) { ok = 1; break; }
      /* the receiver of that accessor must itself be a container, not a class */
      if (ok) {
        int rr = nt_ref(nt, recv, "receiver");
        TyKind rrt = rr >= 0 ? comp_ntype(c, rr) : TY_UNKNOWN;
        if (!ty_is_array(rrt) && !ty_is_hash(rrt) && rrt != TY_POLY_ARRAY) ok = 0;
      }
      if (!ok) return 0;
    }
    else return 0;
  }
  /* single-param block only: a multi-param block forwarded to a builtin
     hash `each` (a [k,v] pair) needs its params registered on the poly path,
     which they are not yet -- gate to |x| so the un-handled multi-param case
     falls through to the loud unsupported error, not a silent empty body. */
  {
    int np = 0; while (block_param_name(c, block, np)) np++;
    if (np != 1 || block_rest_marker(c, block) || block_opt_name(c, block, 0) ||
        block_post_name(c, block, 0)) return 0;
  }
  /* candidate user classes: instantiated, define m, and m yields or forwards a
     block (a plain method wouldn't consume the block anyway) */
  int cand[64], nc = 0;
  for (int k = 0; k < c->nclasses && nc < 64; k++) {
    if (!c->classes[k].instantiated || is_builtin_reopen(c->classes[k].name)) continue;
    int km = comp_method_in_chain(c, k, name, NULL);
    if (km < 0) continue;
    Scope *ks = &c->scopes[km];
    int consumes = ks->yields || (ks->blk_param && ks->blk_param[0]) ||
                   pure_forwarding_target(c, km, 0) >= 0;
    if (!consumes) return 0;  /* a non-block method in the mix: not our shape */
    cand[nc++] = k;
  }
  if (nc == 0) return 0;
  /* names that a BUILTIN container also answers (each/map/...) are unsafe: a
     poly value here can be an Array/Hash at run time, not one of our user
     classes, and the cls_id switch would miss it silently. Only dispatch names
     that are exclusively user methods. */
  if (sp_streq(name, "each") || sp_streq(name, "each_pair") ||
      sp_streq(name, "each_with_index") || sp_streq(name, "map") ||
      sp_streq(name, "select") || sp_streq(name, "reject") ||
      sp_streq(name, "each_value") || sp_streq(name, "each_key") ||
      sp_streq(name, "reduce") || sp_streq(name, "inject") ||
      sp_streq(name, "find") || sp_streq(name, "detect"))
    return 0;
  int trecv = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "sp_RbVal _t%d = ", trecv); emit_boxed(c, recv, b); buf_puts(b, ";\n");
  emit_indent(b, indent);
  buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv);
  emit_indent(b, indent);
  buf_printf(b, "switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {\n", trecv, trecv);
  const char *sv_expr = g_inline_recv_expr;
  int sv_class = g_inline_recv_class;
  TyKind sv_cache = c->ntype[recv];
  for (int i = 0; i < nc; i++) {
    int k = cand[i];
    emit_indent(b, indent);
    buf_printf(b, "case %d: {\n", k);
    char castbuf[96];
    snprintf(castbuf, sizeof castbuf, "(sp_%s *)_t%d.v.p", c->classes[k].c_name, trecv);
    g_inline_recv_expr = castbuf;
    g_inline_recv_class = k;
    c->ntype[recv] = ty_object(k);  /* so the inline entry classifies the receiver */
    emit_inline_call(c, id, b, indent + 1);
    g_inline_recv_expr = sv_expr;
    g_inline_recv_class = sv_class;
    c->ntype[recv] = sv_cache;
    emit_indent(b, indent + 1); buf_puts(b, "break;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
  }
  /* map!/collect!: the poly value can also be a BUILTIN array at run time
     (a nested-array element) -- without this default arm the switch missed
     it silently and the mutation vanished (#3234). Rewrite in place over the
     normalized working array, then write back into the typed original. */
  if (sp_streq(name, "map!") || sp_streq(name, "collect!")) {
    const char *dp0 = block_param_name(c, block, 0);
    const char *dp0r = dp0 ? rename_local(dp0) : NULL;
    int dbody = nt_ref(nt, block, "body");
    int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
    if (dbn >= 1 && dp0r) {
      int tw = ++g_tmp, ti2 = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "default: {\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"map!\"); SP_GC_ROOT(_t%d);\n",
                 tw, trecv, tw);
      /* The loop below stores into the array's elements directly rather than
         through a runtime mutator, so it carries its own write barrier: the
         receiver may be an old array taking references to values this loop
         has just made. Once before the loop is enough -- the remembered set
         dedupes on the object, not the store. */
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_gc_wb((void *)_t%d);\n", tw);
      emit_indent(b, indent + 1);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, tw, ti2);
      emit_indent(b, indent + 2);
      buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", dp0r, tw, ti2);
      for (int j2 = 0; j2 + 1 < dbn; j2++) emit_stmt(c, dbb[j2], b, indent + 2);
      { int svi = g_indent; g_indent = indent + 2;
        Buf vb2; memset(&vb2, 0, sizeof vb2); emit_boxed(c, dbb[dbn - 1], &vb2);
        g_indent = svi;
        emit_indent(b, indent + 2);
        buf_printf(b, "_t%d->data[_t%d] = %s;\n", tw, ti2, vb2.p ? vb2.p : "sp_box_nil()");
        free(vb2.p); }
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_poly_arr_writeback(_t%d, _t%d);\n", trecv, tw);
      emit_indent(b, indent + 1); buf_puts(b, "break;\n");
      emit_indent(b, indent); buf_puts(b, "}\n");
    }
  }
  emit_indent(b, indent); buf_puts(b, "}\n");
  return 1;
}

/* Does this call target a user method that yields (so it has no standalone C
   function -- it is only ever inlined at its call sites)? A compact echo of
   emit_inline_call_x's own method resolution. (#2948) */
static int call_targets_yielding_method(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int mi = -1;
  if (recv < 0) {
    mi = comp_method_index(c, name);
    if (mi < 0) {
      Scope *encl = comp_scope_of(c, id);
      if (encl && encl->class_id >= 0) {
        mi = comp_method_in_chain(c, encl->class_id, name, NULL);
        if (mi < 0 && encl->is_cmethod) mi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      }
    }
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    const char *rty = nt_type(nt, recv);
    const char *cn = (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
                     ? nt_str(nt, recv, "name") : NULL;
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) mi = comp_cmethod_in_chain(c, ci, name, NULL);
    else if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  }
  return mi >= 0 && c->scopes[mi].yields;
}

int emit_inline_expr(Compiler *c, int id, Buf *b) {
  /* only when a value is actually produced (scalar return) */
  TyKind rt = comp_ntype(c, id);
  if (!is_scalar_ret(rt)) {
    /* A block that always raises leaves the call with no value type at all,
       but the call itself still inlines: hold the (dead) result boxed so the
       yielding method needs no standalone function (#3716). */
    if ((rt == TY_VOID || rt == TY_UNKNOWN || rt == TY_NIL) &&
        nt_ref(c->nt, id, "block") >= 0 && call_targets_yielding_method(c, id)) {
      TyKind sv = c->ntype[id];
      c->ntype[id] = TY_POLY;
      int ok = emit_inline_call_x(c, id, b, g_indent + 1, 1);
      c->ntype[id] = sv;
      if (ok) return 1;
    }
    /* a block-driving call to a yielding method that can't be inlined here (a
       non-scalar result) has no standalone function to fall back to: the plain
       call would emit an undefined symbol (invalid C). Fail loud (#2948). */
    if (nt_ref(c->nt, id, "block") >= 0 && call_targets_yielding_method(c, id))
      unsupported_feature(c, id,
        "a block-driving call to a method that yields could not be inlined "
        "(a yielding method has no standalone function to call)");
    return 0;
  }
  return emit_inline_call_x(c, id, b, g_indent + 1, 1);
}

/* Block iteration lowered to an inline C for-loop. Handles n.times,
   array.each, range.each, n.upto/downto. Returns 1 if handled. */
/* Emit `lv_<p0> = <expr_src>` boxing if p0 is poly and src is concrete. */
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig,
                                   const char *p0_ren, TyKind src_type,
                                   const char *src_expr, Buf *b, int indent) {
  Scope *sc = comp_scope_of(c, block);
  LocalVar *lv = sc ? scope_local(sc, p0_orig) : NULL;
  /* A parameter the analyzer never registered has no C declaration -- an
     unused one over an empty literal, where there is no element type to infer
     from. Binding it referenced an undeclared variable (#3853); skipping the
     binding is what the instance_eval path already does with its unused
     parameter, and an unbound name cannot be read. */
  if (!lv || lv->type == TY_UNKNOWN) return;
  TyKind pt = lv->type;
  emit_indent(b, indent);
  if (pt == TY_POLY && src_type != TY_POLY) {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, src_type, src_expr, &bx);
    buf_printf(b, "lv_%s = %s;\n", p0_ren, bx.p ? bx.p : src_expr);
    free(bx.p);
  }
  else {
    buf_printf(b, "lv_%s = %s;\n", p0_ren, src_expr);
  }
}

/* Bind a block's `*rest` param for one iteration. A splat-only block wraps
   the yielded element whole (lv_rest = [elem] -- CRuby does not auto-splat a
   splat-only block); with leading required params the rest is empty for a
   statically non-array element (only array elements distribute across
   |a, *r|, which the poly autosplat paths own). Declared in the loop body so
   the form is self-contained (shadowing any method-scope slot is harmless).
   Returns 1 when a rest param was bound, 0 when the block has none, and -1
   for the unsupported poly-element distribute shape. */
int emit_iter_bind_rest(Compiler *c, int block, int np, TyKind elem_t,
                        const char *elem_src, Buf *b, int indent) {
  const char *rn = block_rest_name(c, block);
  if (!rn || !*rn) return 0;
  if (np >= 1 && elem_t == TY_POLY) return -1;  /* would need runtime distribution */
  const char *rren = rename_local(rn);
  emit_indent(b, indent);
  /* Assign the prologue-declared slot (type_block_rest_params registers the
     rest param as a TY_POLY_ARRAY method local, so `lv_<rest>` is declared and
     GC-rooted once in the prologue). SP_GC_ROOT roots the slot address `&lv_x`,
     so this per-iteration reassignment is covered without re-rooting -- no
     shadow declaration, no per-iteration root accumulation. */
  buf_printf(b, "lv_%s = sp_PolyArray_new();\n", rren);
  if (np == 0) {
    emit_indent(b, indent);
    Buf bx; memset(&bx, 0, sizeof bx);
    if (elem_t == TY_POLY) buf_printf(&bx, "%s", elem_src);
    else emit_boxed_text(c, elem_t, elem_src, &bx);
    buf_printf(b, "sp_PolyArray_push(lv_%s, %s);\n", rren, bx.p ? bx.p : elem_src);
    free(bx.p);
  }
  return 1;
}

/* Does the subtree contain a `redo` that belongs to THIS loop, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? */
int subtree_has_own_redo(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "RedoNode")) return 1;
  /* nested scope/loop boundaries: a redo inside binds to that inner loop */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;  /* nested iteration */
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_redo(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_redo(nt, ids[k])) return 1;
  }
  return 0;
}

/* Does the subtree contain a `next` that belongs to THIS block, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? Same
   ownership rule as subtree_has_own_redo. */
int subtree_has_own_next(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "NextNode")) return 1;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_next(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_next(nt, ids[k])) return 1;
  }
  return 0;
}

/* Emit a loop body, prefixing a `_redo_N:` label (and pushing it on the redo
   stack) when the body contains a `redo` that targets this loop. The label
   sits at the body top so `redo` re-runs the body without advancing. */
void emit_loop_body(Compiler *c, int body, Buf *b, int indent) {
  /* break/next inside this body exit THIS C loop: record the live
     begin/rescue frame depth at loop entry so their emission can pop the
     frames opened inside the body (mirrors emit_return's accounting). */
  int sv_lexc = g_loop_exc_base;
  g_loop_exc_base = g_exc_frame_depth;
  int sv_lens = g_loop_ensure_base;
  g_loop_ensure_base = g_ensure_depth;
  g_c_loop_depth++;
  int has_redo = subtree_has_own_redo(c->nt, body);
  int lbl = 0;
  if (has_redo) {
    lbl = ++g_tmp;
    if (g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0]))
      g_redo_stack[g_redo_depth++] = lbl;
    else has_redo = 0;
  }
  if (has_redo) { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", lbl); }
  /* Safepoint poll at the loop back-edge: a threaded program's worker checks
     here whether a GC stop-the-world wants it to park, so a long-running loop
     cannot starve the collector. SP_SAFEPOINT_POLL() (sp_sched.h) is a relaxed
     atomic load of sp_safepoint_flag under SP_THREADS -- the collector writes
     the flag from another thread -- and a plain load otherwise. Emitted only
     when the program uses threads; a non-threaded program is byte-identical.
     At N=1 the flag is never set -- a predicted-not-taken load. */
  if (g_uses_threads) { emit_indent(b, indent); buf_puts(b, "if (SP_UNLIKELY(SP_SAFEPOINT_POLL())) sp_safepoint();\n"); }
  emit_stmts(c, body, b, indent);
  if (has_redo) g_redo_depth--;
  g_c_loop_depth--;
  g_loop_exc_base = sv_lexc;
  g_loop_ensure_base = sv_lens;
}

/* `recv.tap { |x| body }` / `recv.then { |x| body }` (alias yield_self) in
   expression position. tap runs the block for its side effect and yields the
   (unchanged) receiver; then yields the block's value. The loop body emits into
   the statement prelude (g_pre); the result temp is the expression value. */
int emit_tap_then_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_tap = sp_streq(name, "tap");
  int is_then = sp_streq(name, "then") || sp_streq(name, "yield_self");
  if (!is_tap && !is_then) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind et = comp_ntype(c, recv);
  /* An empty array literal receiver (`[].tap { |a| a << x }.join`) has no
     element type of its own, so comp_ntype leaves it unknown. Adopt the block
     param's container type (it was typed from the pushes) and materialize a
     FRESH container of it below -- matching the analyze-side tap result type so
     a downstream `.join` / `p` dispatches correctly (#3200, #3208). */
  int empty_arr_adopt = 0;
  if (is_tap) {
    const char *rty = nt_type(nt, recv);
    int rel = 0;
    if (rty && sp_streq(rty, "ArrayNode")) nt_arr(nt, recv, "elements", &rel);
    if (rty && sp_streq(rty, "ArrayNode") && rel == 0) {
      const char *bp = block_param_name(c, block, 0);
      Scope *bsc2 = bp ? comp_scope_of(c, block) : NULL;
      LocalVar *blv = bsc2 ? scope_local(bsc2, rename_local(bp)) : NULL;
      if (blv && ty_is_array(blv->type)) { et = blv->type; empty_arr_adopt = 1; }
    }
  }
  if (et == TY_UNKNOWN) return 0;
  /* a nil receiver (`nil.tap { }`) has no scalar C type: carry it boxed */
  int et_nil = (et == TY_NIL);
  if (et_nil) et = TY_POLY;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (is_then && bn < 1) return 0;  /* then must yield a value */
  const char *p0 = block_param_name(c, block, 0);
  if (p0) p0 = rename_local(p0);

  int tr = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  /* the adopted empty-literal receiver materializes as a FRESH mutable container
     of the block-param type -- emit_expr would render `[]` as its own untyped
     default (sp_IntArray_new()), mismatching et (#3200). */
  if (empty_arr_adopt) {
    if (et == TY_POLY_ARRAY) buf_puts(&rb, "sp_PolyArray_new()");
    else buf_printf(&rb, "sp_%sArray_new()", array_kind(et) ? array_kind(et) : "Int");
  }
  else if (et_nil) emit_boxed(c, recv, &rb); else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
  if (needs_root(et)) { emit_indent(g_pre, g_indent); emit_gc_root_tmp(c, et, tr, g_pre); buf_puts(g_pre, "\n"); }

  /* a then result temp is declared outside the (optional) shadow block so the
     block value escapes it. */
  int tres = 0; TyKind rett = TY_VOID;
  if (is_then) {
    rett = comp_ntype(c, id);
    /* A body that always `break`s completes normally nowhere, so it publishes
       no result type and `void` cannot declare the slot the substrate writes
       (#3986). The break itself delivers its value through sp_brk_val, and the
       slot is dead on that path, so a boxed one keeps the C valid. */
    /* TY_NIL joins them: `then { }` desugars to `then { nil }`, whose result
       type has no C slot either -- emit_ctype spells it `void` (#4028). */
    if (rett == TY_VOID || rett == TY_UNKNOWN || rett == TY_NIL) rett = TY_POLY;
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); emit_ctype(c, rett, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tres, default_value(rett));
    if (needs_root(rett)) { emit_indent(g_pre, g_indent); emit_gc_root_tmp(c, rett, tres, g_pre); buf_puts(g_pre, "\n"); }
  }

  /* pin the block param to the receiver type if inference widened it */
  Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
  TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
  int use_shadow = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
  int din = g_indent;
  if (use_shadow) {
    tlv0->type = et;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    din = g_indent + 1;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = _t%d;\n", p0, tr);
  }
  else if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tr); }

  int sv = g_indent; g_indent = din;
  if (is_then) {
    /* The body goes through the next-aware substrate: `next <v>` inside a
       `then` block leaves the block WITH that value, and this splice has no
       loop of its own, so a bare `continue` was both value-dropping and
       invalid C (#3978). The do{}while(0) wrapper it emits makes the
       continue exit exactly this block. */
    char destbuf[24]; snprintf(destbuf, sizeof destbuf, "_t%d", tres);
    emit_block_value_into(c, block, destbuf, rett == TY_POLY, din);
  }
  else {
    /* tap discards the block's value, but a `next` still leaves the block --
       same wrapper, no destination. */
    const char *sv_nxv = g_ie_next_var;
    g_ie_next_var = NULL;
    g_c_loop_depth++;
    emit_indent(g_pre, din); buf_puts(g_pre, "do {\n");
    int bi = din + 1; g_indent = bi;
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, bi);
    g_indent = din;
    emit_indent(g_pre, din); buf_puts(g_pre, "} while (0);\n");
    g_c_loop_depth--;
    g_ie_next_var = sv_nxv;
  }
  g_indent = sv;
  if (use_shadow) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
  if (tlv0) tlv0->type = tsaved0;

  buf_printf(b, "_t%d", is_tap ? tr : tres);
  return 1;
}

/* The nil sentinel a block param of type `pt` receives when an auto-splat
   source array has no item at the param's index. Mirrors the proc-literal
   convention in codegen.c (a missing arg binds nil, not a typed zero).
   `pt` is only ever TY_POLY or a scalar slot type (int/bool/float/symbol/
   string) here: these params are bound from poly-container elements, and a
   value-type (Range/Time/Complex/Rational/object value-type) boxes to poly
   inside a container, so it never arrives as a typed struct slot. */
static void emit_block_param_nil(Compiler *c, TyKind pt, Buf *b) {
  (void)c;
  if (pt == TY_POLY)                      buf_puts(b, "sp_box_nil()");
  else if (pt == TY_INT || pt == TY_BOOL) buf_puts(b, "SP_INT_NIL");
  else if (pt == TY_FLOAT)                buf_puts(b, "sp_float_nil()");
  else if (pt == TY_SYMBOL)               buf_puts(b, "((sp_sym)-1)");
  else                                    buf_puts(b, "NULL");  /* string / heap ptr */
}

/* Bind block param `pname` (already renamed) of type `pt` from a boxed
   sp_RbVal source `src`: a poly param takes the box directly; a scalar param
   unboxes down to its slot type. As in emit_block_param_nil, `pt` is TY_POLY
   or a scalar slot type only -- a value-type element is boxed to poly in its
   container, so emit_unbox_text is never asked for a struct-by-value slot. */
void emit_block_param_from_boxed(Compiler *c, const char *pname, TyKind pt,
                                 const char *src, Buf *b) {
  buf_printf(b, "lv_%s = ", pname);
  if (pt == TY_POLY) buf_puts(b, src);
  else emit_unbox_text(c, pt, src, b);
  buf_puts(b, ";\n");
}

/* A block iterator in VALUE position returns its receiver (each,
   each_value/each_key/each_pair, each_with_index, reverse_each): evaluate the
   receiver into a temp, run the statement emitter with the receiver's
   emission overridden to the temp, and yield the temp. Statement position
   never reaches this (emit_stmt claims iterators first). */
/* `<array>.take_while.with_index { |v, i| pred }` (and drop_while): a blockless
   take_while/drop_while returns an Enumerator whose with_index feeds the index
   to the predicate. Emit the take/drop-while loop with a running index over the
   typed array source, collecting into a fresh array of the same kind (#3182). */
int emit_takewhile_with_index(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "with_index")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *rnm = nt_str(nt, recv, "name");
  int is_take = rnm && sp_streq(rnm, "take_while");
  int is_drop = rnm && sp_streq(rnm, "drop_while");
  if ((!is_take && !is_drop) || nt_ref(nt, recv, "block") >= 0 || nt_ref(nt, recv, "arguments") >= 0)
    return 0;
  int src = nt_ref(nt, recv, "receiver");
  if (src < 0) return 0;
  TyKind srt = comp_ntype(c, src);
  if (!ty_is_array(srt)) return 0;
  const char *k = (srt == TY_POLY_ARRAY) ? "Poly" : array_kind(srt);
  if (!k) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int wargc; const int *wargv = call_args(nt, id, &wargc);
  Scope *bs = comp_scope_of(c, block);
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  LocalVar *p0lv = (p0 && bs) ? scope_local(bs, p0) : NULL;
  LocalVar *p1lv = (p1 && bs) ? scope_local(bs, p1) : NULL;

  int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, toff = ++g_tmp;
  int tdrop = is_drop ? ++g_tmp : -1;
  Buf sb = expr_buf(c, src);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k, ta, sb.p ? sb.p : "NULL", ta);
  free(sb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = ", toff);
  if (wargc == 1 && wargv) emit_int_expr(c, wargv[0], g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");
  if (is_drop) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = 1;\n", tdrop); }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, ta, ti);
  char es[64]; snprintf(es, sizeof es, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
  TyKind et = ty_array_elem(srt);
  if (p0lv) {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = ", rename_local(p0));
    /* coerce the typed element to the param's declared type: box when the
       param widened to poly, else assign the typed value directly. */
    if (p0lv->type == TY_POLY && et != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, es, &bx); buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    else buf_puts(g_pre, es);
    buf_puts(g_pre, ";\n");
  }
  if (p1lv) {
    emit_indent(g_pre, g_indent + 1);
    if (p1lv->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d + _t%d);\n", rename_local(p1), ti, toff);
    else buf_printf(g_pre, "lv_%s = _t%d + _t%d;\n", rename_local(p1), ti, toff);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int sv = g_indent; g_indent++;
  Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
  emit_indent(g_pre, g_indent + 1);
  if (is_take) {
    buf_printf(g_pre, "if (!(%s)) break;\n", cb.p ? cb.p : "0");
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, tr, es);
  }
  else {
    buf_printf(g_pre, "if (_t%d && (%s)) continue;\n", tdrop, cb.p ? cb.p : "0");
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "_t%d = 0;\n", tdrop);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, tr, es);
  }
  free(cb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* The iterator names whose value IS the receiver and which emit_iter_value_expr
   lowers by hoisting that receiver into a temp. Shared with the tail-statement
   emitter, which has to know whether this path can carry the value before it
   commits to the statement form. */
int iter_value_answers_recv(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  return sp_streq(name, "each") || sp_streq(name, "each_value") ||
        sp_streq(name, "each_key") || sp_streq(name, "each_pair") ||
        sp_streq(name, "each_with_index") || sp_streq(name, "reverse_each") ||
        sp_streq(name, "each_entry") ||
        /* each_slice / each_cons answer the receiver too; over a Hash or a
           Range that receiver is the marked `to_a` hop's own receiver */
        ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
         nt_ref(nt, id, "receiver") >= 0 &&
         ((nt_kind(nt, nt_ref(nt, id, "receiver")) == NK_CallNode &&
           nt_str(nt, nt_ref(nt, id, "receiver"), "enum_recv")) ||
          /* a Range receiver is materialized in place rather than through a
             marked hop, and answered the int array it walked (#3920) */
          comp_ntype(c, nt_ref(nt, id, "receiver")) == TY_RANGE)) ||
        /* `str.split(sep) { |piece| }` answers the receiver too; in value
           position the block was dropped and the split array returned */
        sp_streq(name, "split");
}

int emit_iter_value_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  /* A `&.` call has to reach the safe-nav guard first: this lowering answers
     the receiver and never looks at the operator, so `v&.each { }` walked a
     nil receiver and raised where CRuby answers nil. Stand down only BEFORE
     the guard runs -- it re-enters this emission on the guarded temp with
     g_sn_skip set, and that pass has to lower normally. */
  { const char *sop = nt_str(nt, id, "call_operator");
    int sn_recv = nt_ref(nt, id, "receiver");
    if (sop && sp_streq(sop, "&.") && g_sn_skip != id &&
        sn_recv >= 0 && comp_ntype(c, sn_recv) == TY_POLY) return 0; }
  if (!iter_value_answers_recv(c, id)) return 0;
  int block = nt_ref(nt, id, "block");
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || recv < 0) return 0;
  if (!nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* A poly receiver is allowed: `each` answers the receiver whatever kind it
     turns out to hold, and the loop below walks it through the poly surface.
     Bails on its own below when the iteration cannot be emitted. */
  if (rt == TY_UNKNOWN) return 0;
  if (g_n_argov >= MAX_ARG_OVERRIDE) return 0;
  /* When the receiver was rewritten to `obj.__enum_to_a` (a user Enumerable or
     Struct routed through its synthesized member array, #2546/#2547), the block
     iterator must still yield the ORIGINAL receiver `obj`, not the intermediate
     array: Enumerable#reverse_each / #each_with_index return the enumerable.
     Bind obj once, materialize the array from that binding, iterate, yield obj. */
  int objn = -1;
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    const char *rnm = nt_str(nt, recv, "name");
    if (rnm && sp_streq(rnm, "__enum_to_a")) objn = nt_ref(nt, recv, "receiver");
    else if (nt_str(nt, recv, "enum_recv")) objn = nt_ref(nt, recv, "receiver");
  }
  /* run the statement emitter against the temp into a scratch buffer first:
     splice only when it handles the shape, else leave the node to the
     later handlers untouched */
  int ta = ++g_tmp;
  g_argov_node[g_n_argov] = recv;
  snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
  g_n_argov++;
  Buf body; memset(&body, 0, sizeof body);
  int ok = emit_iteration_stmt(c, id, &body, 0);
  g_n_argov--;
  if (!ok) { free(body.p); return 0; }
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b);
  buf_printf(b, " _t%d = ", ta);
  emit_expr(c, recv, b);
  buf_puts(b, "; ");
  /* Root the hoisted receiver on the same test hoist_loop_recv uses: it lives
     across the whole loop body, which allocates. is_scalar_ret() answers TRUE
     for arrays, hashes and objects (it asks how a value is RETURNED, not
     whether it is collectable), so this rooted almost nothing. */
  if (needs_root(rt)) buf_printf(b, rt == TY_POLY ? "SP_GC_ROOT_RBVAL(_t%d); " : "SP_GC_ROOT(_t%d); ", ta);
  buf_puts(b, body.p ? body.p : "");
  free(body.p);
  /* yield the original Enumerable receiver, not the intermediate member array:
     `obj` is already materialized inside the `obj.__enum_to_a` emission above
     (hoisted into a temp for GC when non-trivial, else a plain lvalue), so
     re-emitting it here references that same value rather than re-evaluating. */
  if (objn >= 0) { buf_puts(b, " "); emit_expr(c, objn, b); buf_puts(b, "; })"); }
  else buf_printf(b, " _t%d; })", ta);
  return 1;
}

/* The in-place filter loop of select! / filter! / reject! / keep_if /
   delete_if on a hash of any variant, into `b` at `indent`, over the receiver
   text `rs`: the hash in `_t<tr>`, its pair count before the loop in
   `_t<torig>` and after it in `_t<twp>`, for the caller to answer from.
   The block's parameters take the variant's key and value types, so a
   general hash's block sees sp_RbVals; a one-parameter block takes the key
   alone. The predicate is read by Ruby truthiness.

   The loop's state lives outside it, and the advance or the delete is the
   for's own third clause, not a statement at the end of the body: `next` in
   the block is a C `continue`, which runs the third clause and skips whatever
   the body ends with -- as a trailing statement it was skipped and the same
   pair ran forever (the each loop's #3782). The verdict is the predicate's,
   or the value a `next` left, or nil for a bare one. CRuby refuses a new key
   during the iteration, and permits deleting one: a pair the block deleted
   slides the next pair into its slot, so the index advances only while the
   slot still holds the key the block was given (#3569), and a drop deletes
   by that key, which a block that deleted it already made a no-op.
   Answers 0 for a block with no body to read. */
int emit_hash_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
                          const char *rs, Buf *b, int indent, int *tr, int *torig, int *twp) {
  const NodeTable *nt = c->nt;
  const char *hn = ty_hash_cname(rt);
  int is_rej = sp_streq(name, "delete_if") || sp_streq(name, "reject!");
  const char *p0_raw = block_param_name(c, block, 0);
  const char *p1_raw = block_param_name(c, block, 1);
  const char *kp = p0_raw ? rename_local(p0_raw) : NULL;
  const char *vp = p1_raw ? rename_local(p1_raw) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!hn || bn < 1) return 0;
  (void)recv;
  Scope *hs = comp_scope_of(c, block);
  TyKind hkt = ty_hash_key(rt), hvt = ty_hash_val(rt);
  LocalVar *klv = (kp && hs) ? scope_local(hs, p0_raw) : NULL;
  LocalVar *vlv = (vp && hs) ? scope_local(hs, p1_raw) : NULL;
  TyKind ksaved = klv ? klv->type : TY_UNKNOWN;
  TyKind vsaved = vlv ? vlv->type : TY_UNKNOWN;
  if (klv) klv->type = hkt;
  if (vlv) vlv->type = hvt;
  for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
  int t = ++g_tmp, ti = ++g_tmp, to = ++g_tmp, tw = ++g_tmp;
  int tn = ++g_tmp, tk = ++g_tmp, tkey = ++g_tmp, tnv = ++g_tmp;
  /* rooted: a receiver that is a temporary has no other holder once its own
     expression is done, and the block body, or the general hash's delete,
     may collect before the loop is through */
  emit_indent(b, indent); emit_ctype(c, rt, b);
  buf_printf(b, " _t%d = %s; ", t, rs); emit_gc_root_tmp(c, rt, t, b); buf_puts(b, "\n");
  emit_indent(b, indent);
  buf_printf(b, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);\n", t, t, hash_box_cls(rt));
  emit_indent(b, indent);
  buf_printf(b, "sp_int _t%d = _t%d ? _t%d->len : 0;\n", to, t, t);
  emit_indent(b, indent);
  buf_printf(b, "sp_int _t%d = 0, _t%d = 0; sp_RbVal _t%d = sp_box_nil();\n", tn, tk, tnv);
  emit_indent(b, indent); emit_ctype(c, hkt, b);
  if (hkt == TY_POLY) buf_printf(b, " _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tkey, tkey);
  else if (hkt == TY_STRING) buf_printf(b, " _t%d = NULL; SP_GC_ROOT_STR(_t%d);\n", tkey, tkey);
  else buf_printf(b, " _t%d = 0;\n", tkey);
  emit_indent(b, indent);
  buf_printf(b, "for (sp_int _t%d = 0; _t%d && _t%d < _t%d->len; ({", ti, t, ti, t);
  buf_printf(b, " if (_t%d->len > _t%d) sp_raise_cls(\"RuntimeError\","
                " \"can't add a new key into hash during iteration\");", t, tn);
  buf_printf(b, " if (_t%d < 0) _t%d = %ssp_poly_truthy(_t%d);", tk, tk, is_rej ? "!" : "", tnv);
  buf_printf(b, " if (!_t%d) sp_%sHash_delete(_t%d, _t%d);", tk, hn, t, tkey);
  buf_printf(b, " else if (_t%d < _t%d->len && ", ti, t);
  if (hkt == TY_POLY) buf_printf(b, "sp_rbval_eql_key(_t%d->keys[_t%d->order[_t%d]], _t%d)", t, t, ti, tkey);
  else if (hkt == TY_STRING) buf_printf(b, "sp_str_eq(_t%d->order[_t%d], _t%d)", t, ti, tkey);
  else buf_printf(b, "_t%d->order[_t%d] == _t%d", t, ti, tkey);
  buf_printf(b, ") _t%d++; })) {\n", ti);
  emit_indent(b, indent + 1);
  buf_printf(b, "_t%d = _t%d->len; _t%d = -1; _t%d = sp_box_nil(); _t%d = %s;\n",
             tn, t, tk, tnv, tkey, hash_order_key(rt, t, ti));
  /* a key or a value the block holds outlives its pair when the block drops
     the pair itself, or is reassigned, and then allocates, so a collectable
     one is rooted, as the each loop's are */
  if (kp) {
    emit_indent(b, indent + 1); emit_ctype(c, hkt, b);
    buf_printf(b, " lv_%s = _t%d;", kp, tkey);
    if (hkt == TY_POLY) buf_printf(b, " SP_GC_ROOT_RBVAL(lv_%s);", kp);
    else if (hkt == TY_STRING) buf_printf(b, " SP_GC_ROOT_STR(lv_%s);", kp);
    buf_puts(b, "\n");
  }
  if (vp) {
    emit_indent(b, indent + 1); emit_ctype(c, hvt, b);
    buf_printf(b, " lv_%s = %s;", vp, hash_order_val(rt, t, ti));
    if (hvt == TY_POLY) buf_printf(b, " SP_GC_ROOT_RBVAL(lv_%s);", vp);
    else if (hvt == TY_STRING) buf_printf(b, " SP_GC_ROOT_STR(lv_%s);", vp);
    buf_puts(b, "\n");
  }
  const char *sv_nx = g_ie_next_var; int sv_poly = g_ie_res_poly; TyKind sv_nty = g_ie_next_ty;
  int sv_lexc = g_loop_exc_base, sv_lens = g_loop_ensure_base;
  char nxbuf[32]; snprintf(nxbuf, sizeof nxbuf, "_t%d", tnv);
  g_ie_next_var = nxbuf; g_ie_res_poly = 1; g_ie_next_ty = TY_UNKNOWN;
  g_loop_exc_base = g_exc_frame_depth; g_loop_ensure_base = g_ensure_depth;
  g_c_loop_depth++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], b, indent + 1);
  if (sp_streq(nt_type(nt, bb[bn - 1]), "NextNode")) emit_stmt(c, bb[bn - 1], b, indent + 1);
  else {
    /* the predicate in its own buffer: a multi-statement terminal (a block
       ending in an if/else expression) lowers its statements through g_pre,
       and they belong inside the loop body, before the verdict */
    Buf *sp_save = g_pre; int gi_save = g_indent;
    Buf cpre; memset(&cpre, 0, sizeof cpre); g_pre = &cpre; g_indent = indent + 1;
    Buf cexpr; memset(&cexpr, 0, sizeof cexpr);
    emit_cond(c, bb[bn - 1], &cexpr);
    g_pre = sp_save; g_indent = gi_save;
    if (cpre.p) { buf_puts(b, cpre.p); free(cpre.p); }
    emit_indent(b, indent + 1);
    buf_printf(b, "_t%d = %s(%s);\n", tk, is_rej ? "!" : "", cexpr.p ? cexpr.p : "0");
    free(cexpr.p);
  }
  g_c_loop_depth--;
  g_loop_exc_base = sv_lexc; g_loop_ensure_base = sv_lens;
  g_ie_next_var = sv_nx; g_ie_res_poly = sv_poly; g_ie_next_ty = sv_nty;
  emit_indent(b, indent); buf_puts(b, "}\n");
  emit_indent(b, indent);
  buf_printf(b, "sp_int _t%d = _t%d ? _t%d->len : 0;\n", tw, t, t);
  if (klv) klv->type = ksaved;
  if (vlv) vlv->type = vsaved;
  *tr = t; *torig = to; *twp = tw;
  return 1;
}

/* The in-place filter loop of select! / filter! / reject! / keep_if /
   delete_if on an array of any kind, into `b` at `indent`: the array in
   `_t<tr>`, its length before the loop in `_t<torig>` and after it in
   `_t<twp>`, for the caller to answer from. The block's parameter takes the
   element type, and the predicate is read by Ruby truthiness.

   The kept elements are written down over the dropped ones as the loop
   goes, and the length is cut once at the end. The advance and the keep are
   the for's own third clause, not the end of the body: `next` in the block
   is a C `continue`, which runs the third clause and skips whatever the
   body ends with -- as a trailing statement the keep was skipped, and a
   `next` dropped the element whatever value it carried. The verdict is the
   predicate's, or the value a `next` left, or nil for a bare one.

   The loop runs inside an ensure region, the protocol of emit_begin's ensure
   clause with an ensure body that is C alone: whatever leaves the loop
   early -- a `break`, a `raise`, a `return`, a `throw` -- lands here, the
   elements the block never saw are moved down behind the kept ones, the
   one it was given among them, and the length is cut, as CRuby's
   select_bang_ensure does; then the exit goes on its way. Without it the
   array was left as the compaction had it, the kept elements over the
   first slots and the old length in force. At the ensure stack's limit the
   loop goes without the region, as emit_begin's clause does, and an early
   exit leaves the array as before. The ensure body can raise only for an
   array the block froze under it, and then the FrozenError goes out in
   place of whatever was on its way, with no cause. The verdict slot, -1
   until the predicate or a `next` sets it, is what lets the keep sit in
   the third clause; emit_block_value_into's do-while cannot give that.
   Answers 0 for a block with no body to read, or an array of no typed
   kind, having emitted nothing. */
int emit_array_filter_loop(Compiler *c, int recv, int block, TyKind rt, const char *name,
                           Buf *b, int indent, int *tr, int *torig, int *twp) {
  const NodeTable *nt = c->nt;
  const char *kk = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  int is_rej = sp_streq(name, "reject!") || sp_streq(name, "delete_if");
  const char *bp0 = block_param_name(c, block, 0);
  const char *bp = bp0 ? rename_local(bp0) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!kk || bn < 1) return 0;
  int region = g_ensure_depth < MAX_ENSURE_DEPTH;
  TyKind et = ty_array_elem(rt);
  Scope *fs = comp_scope_of(c, block);
  LocalVar *flv = (fs && bp0) ? scope_local(fs, bp0) : NULL;
  TyKind fsaved = flv ? flv->type : TY_UNKNOWN;
  if (flv) { flv->type = et; for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]); }
  int t = ++g_tmp, ti = ++g_tmp, to = ++g_tmp, tw = ++g_tmp, tk = ++g_tmp, tnv = ++g_tmp, te = ++g_tmp;
  int eid = ++g_tmp, tj = ++g_tmp;
  int has_retval = (g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN);
  Buf rb = expr_buf(c, recv);
  /* rooted: a receiver that is a temporary has no other holder once its own
     expression is done, and the block body may collect before the loop is
     through; so are the element, which the block can drop from the array,
     and the value a `next` leaves. The indexes are volatile: an early exit
     lands past the setjmp and reads them. */
  emit_indent(b, indent); emit_ctype(c, rt, b);
  buf_printf(b, " _t%d = %s; ", t, rb.p ? rb.p : ""); emit_gc_root_tmp(c, rt, t, b); buf_puts(b, "\n");
  free(rb.p);
  /* a frozen receiver is refused before the block runs, as CRuby's
     modify check refuses it; the first kept element's write did, after */
  emit_indent(b, indent);
  buf_printf(b, "if (_t%d && _t%d->frozen) sp_raise_frozen_array_at(_t%d, %s);\n", t, t, t,
             sp_streq(kk, "Poly") ? "SP_BUILTIN_POLY_ARRAY" : sp_streq(kk, "Str") ? "SP_BUILTIN_STR_ARRAY"
             : sp_streq(kk, "Float") ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY");
  emit_indent(b, indent);
  buf_printf(b, "sp_int _t%d = sp_%sArray_length(_t%d); volatile sp_int _t%d = 0, _t%d = 0; sp_int _t%d = 0;", to, kk, t, ti, tw, tk);
  buf_printf(b, " sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tnv, tnv);
  emit_indent(b, indent); emit_ctype(c, et, b);
  if (et == TY_POLY) buf_printf(b, " _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", te, te);
  else if (et == TY_STRING) buf_printf(b, " _t%d = NULL; SP_GC_ROOT_STR(_t%d);\n", te, te);
  else buf_printf(b, " _t%d = 0;\n", te);
  /* the region: emit_begin's ensure protocol, less the deferred-next flag
     no inner region chains to (a `next` in the block targets this loop,
     which the region encloses) */
  if (region) {
    emit_indent(b, indent); buf_printf(b, "int _retf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "int _excf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_excmsg%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_exccls%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "void *_excobj%d = NULL;\n", eid);
    if (has_retval) {
      emit_indent(b, indent); emit_ctype(c, g_ret_type, b);
      buf_printf(b, " _retv%d = %s;\n", eid, default_value(g_ret_type));
    }
    g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth };
    emit_indent(b, indent); buf_puts(b, "sp_exc_check_depth();\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    g_exc_frame_depth++;
  }
  int li = indent + region;   /* the loop's own indent */
  emit_indent(b, li);
  buf_printf(b, "for (; _t%d < sp_%sArray_length(_t%d); ({", ti, kk, t);
  buf_printf(b, " if (_t%d < 0) _t%d = %ssp_poly_truthy(_t%d);", tk, tk, is_rej ? "!" : "", tnv);
  /* a kept element is written down only when it moves, as CRuby's is: a
     block that wrote to its receiver at this index keeps what it wrote */
  buf_printf(b, " if (_t%d) { if (_t%d != _t%d) sp_%sArray_set(_t%d, _t%d, _t%d); _t%d++; } _t%d++; })) {\n",
             tk, tw, ti, kk, t, tw, te, tw, ti);
  emit_indent(b, li + 1);
  buf_printf(b, "_t%d = -1; _t%d = sp_box_nil(); _t%d = sp_%sArray_get(_t%d, _t%d);\n", tk, tnv, te, kk, t, ti);
  if (bp) {
    /* a poly parameter is the hoisted local, rooted where it is declared; a
       typed one shadows it at the element type, and a String is rooted */
    emit_indent(b, li + 1);
    if (et != TY_POLY) { emit_ctype(c, et, b); buf_puts(b, " "); }
    buf_printf(b, "lv_%s = _t%d;", bp, te);
    if (et == TY_STRING) buf_printf(b, " SP_GC_ROOT_STR(lv_%s);", bp);
    buf_puts(b, "\n");
  }
  const char *sv_nx = g_ie_next_var; int sv_poly = g_ie_res_poly; TyKind sv_nty = g_ie_next_ty;
  int sv_lexc = g_loop_exc_base, sv_lens = g_loop_ensure_base;
  char nxbuf[32]; snprintf(nxbuf, sizeof nxbuf, "_t%d", tnv);
  g_ie_next_var = nxbuf; g_ie_res_poly = 1; g_ie_next_ty = TY_UNKNOWN;
  g_loop_exc_base = g_exc_frame_depth; g_loop_ensure_base = g_ensure_depth;
  g_c_loop_depth++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], b, li + 1);
  if (sp_streq(nt_type(nt, bb[bn - 1]), "NextNode")) emit_stmt(c, bb[bn - 1], b, li + 1);
  else {
    /* the predicate in its own buffer: a multi-statement terminal (a block
       ending in an if/else expression) lowers its statements through g_pre,
       and they belong inside the loop body, before the verdict */
    Buf *sp_save = g_pre; int gi_save = g_indent;
    Buf cpre; memset(&cpre, 0, sizeof cpre); g_pre = &cpre; g_indent = li + 1;
    Buf cexpr; memset(&cexpr, 0, sizeof cexpr);
    emit_cond(c, bb[bn - 1], &cexpr);
    g_pre = sp_save; g_indent = gi_save;
    if (cpre.p) { buf_puts(b, cpre.p); free(cpre.p); }
    emit_indent(b, li + 1);
    buf_printf(b, "_t%d = %s(%s);\n", tk, is_rej ? "!" : "", cexpr.p ? cexpr.p : "0");
    free(cexpr.p);
  }
  g_c_loop_depth--;
  g_loop_exc_base = sv_lexc; g_loop_ensure_base = sv_lens;
  g_ie_next_var = sv_nx; g_ie_res_poly = sv_poly; g_ie_next_ty = sv_nty;
  emit_indent(b, li); buf_puts(b, "}\n");
  if (!region) {
    emit_indent(b, indent); buf_printf(b, "if (_t%d) _t%d->len = _t%d;\n", t, t, tw);
    if (flv) flv->type = fsaved;
    *tr = t; *torig = to; *twp = tw;
    return 1;
  }
  g_exc_frame_depth--;
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  emit_indent(b, indent); buf_puts(b, "}\n");
  emit_indent(b, indent); buf_puts(b, "else {\n");
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n");
  emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) {\n");
  emit_indent(b, indent + 2);
  buf_printf(b, "_excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; _excobj%d = sp_exc_obj[sp_exc_top];\n",
             eid, eid, eid, eid);
  emit_indent(b, indent + 1); buf_puts(b, "}\n");
  emit_indent(b, indent); buf_puts(b, "}\n");
  g_ensure_depth--;
  buf_printf(b, "_ensure%d: ;\n", eid);
  /* the ensure body: the elements from the one the block was given on are
     moved down behind the kept ones, and the length is cut */
  emit_indent(b, indent);
  buf_printf(b, "if (_t%d) { for (sp_int _t%d = _t%d; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++)", t, tj, ti, tj, kk, t, tj, tw);
  buf_printf(b, " if (_t%d != _t%d) sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d)); _t%d->len = _t%d; }\n",
             tw, tj, kk, t, tw, kk, t, tj, t, tw);
  emit_indent(b, indent); buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
  emit_indent(b, indent);
  if (g_ensure_depth > 0) {
    EnsureCtx *outer = &g_ensure_stack[g_ensure_depth - 1];
    if (has_retval && outer->has_retval)
      buf_printf(b, "if (_retf%d) { _retv%d = _retv%d; _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                 eid, outer->lid, eid, outer->lid, outer->lid);
    else
      buf_printf(b, "if (_retf%d) { _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n", eid, outer->lid, outer->lid);
    emit_indent(b, indent);
    buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; _excobj%d = _excobj%d; sp_exc_top--; goto _ensure%d; }\n",
               eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid, eid, outer->lid);
  }
  else {
    {
      char g[24]; snprintf(g, sizeof g, "_retf%d", eid);
      if (emit_frame_unwind(b, 0, g)) { buf_puts(b, "\n"); emit_indent(b, indent); }
    }
    if (has_retval && g_in_proc_body && g_result_var && g_result_poly)
      buf_printf(b, "if (_retf%d) { %s = _retv%d; return 0; }\n", eid, g_result_var, eid);
    /* A fiber body is `static void`: returning the value there is a C
       constraint violation (GCC 14 rejects it), and the value had nowhere to
       go anyway -- a void function's caller cannot read it. Drop it and
       return, which is what the generated code already did in practice. */
    else if (has_retval && g_c_ret_void) buf_printf(b, "if (_retf%d) return;\n", eid);
    else if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d;\n", eid, eid);
    else if (g_in_proc_body && g_result_var && g_result_poly)
      buf_printf(b, "if (_retf%d) { %s = sp_box_nil(); return 0; }\n", eid, g_result_var);
    else if (g_c_ret_void) buf_printf(b, "if (_retf%d) return;\n", eid);
    else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil();\n", eid);
    else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0;\n", eid);
    /* a proc body's C function returns sp_int (the value rides
       _sp_proc_poly_ret), so a bare `return;` there is a mismatch the other
       way -- and leaves the returned int indeterminate. Its tail returns 0. */
    else if (g_in_proc_body) buf_printf(b, "if (_retf%d) return 0;\n", eid);
    else buf_printf(b, "if (_retf%d) return;\n", eid);
    emit_indent(b, indent);
    buf_printf(b, "if (_excf%d) { sp_pending_exc_obj = _excobj%d; sp_raise_cls(_exccls%d, _excmsg%d); }\n", eid, eid, eid, eid);
  }
  if (flv) flv->type = fsaved;
  *tr = t; *torig = to; *twp = tw;
  return 1;
}

int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  /* A `&.` call has to reach the safe-nav guard first: this lowering walks the
     receiver and never looks at the operator, so `v&.each { }` handed a nil to
     sp_poly_iter_check and raised where CRuby answers nil. Stand down only
     BEFORE the guard runs -- it re-enters this emission on the guarded temp
     with g_sn_skip set, and that pass has to lower normally. */
  { const char *sop = nt_str(nt, id, "call_operator");
    if (sop && sp_streq(sop, "&.") && g_sn_skip != id &&
        recv >= 0 && comp_ntype(c, recv) == TY_POLY) return 0; }
  /* CRuby checks arity and argument classes at dispatch, before the
     iteration starts: a loop emitted here never reaches emit_call, so both
     guards run here too (3.step(4, 1, 2) { } ran the loop with the extra
     argument dropped; 1.upto("a") { } emitted a pointer/int comparison). */
  { Buf gb; memset(&gb, 0, sizeof gb);
    if (emit_builtin_arity_guard(c, id, &gb) || emit_arg_type_guards(c, id, &gb)) {
      emit_indent(b, indent);
      buf_printf(b, "%s;\n", gb.p ? gb.p : "");
      free(gb.p);
      return 1;
    }
    free(gb.p); }
  /* A poly receiver whose name a user class also owns as a block-taking
     method: the loops below walk the receiver as a builtin container, which
     answers empty when the value is the user object. The cls_id dispatch is
     the only emitter with arms for both (#3409). */
  if (poly_block_call_needs_dispatch(c, id)) return 0;

  /* `xs.each(&h)` forwards a real callable: there is no block body to splice,
     and the loops below would run with an empty one -- silently doing nothing.
     On a receiver only known at run time, hand the proc to the enumerable
     driver; anything else declines to a path that can drive it. (The poly
     dispatch above takes precedence: a user class owning the name has to see
     the call, and its own `each` is not a container walk.) */
  { int rfb = resolve_forwarded_block(c, block);
    if (rfb < 0 || (nt_type(nt, rfb) && sp_streq(nt_type(nt, rfb), "BlockArgumentNode"))) {
      const char *inm = nt_str(nt, id, "name");
      int irecv = nt_ref(nt, id, "receiver");
      const char *pen = inm ? poly_enum_op_for(inm) : NULL;
      if (rfb >= 0 && pen && irecv >= 0 && comp_ntype(c, irecv) == TY_POLY) {
        Buf pb0; memset(&pb0, 0, sizeof pb0);
        if (!emit_forwarded_proc_arg(c, rfb, &pb0)) { free(pb0.p); return 0; }
        int tp0 = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d);\n", tp0, pb0.p ? pb0.p : "NULL", tp0);
        free(pb0.p);
        emit_indent(b, indent);
        buf_puts(b, "(void)sp_poly_enum_proc("); emit_boxed(c, irecv, b);
        buf_printf(b, ", %s, _t%d);\n", pen, tp0);
        return 1;
      }
      return 0;
    } }


  /* loop { ... } -- infinite loop, exited by break */
  if (recv < 0 && sp_streq(name, "loop")) {
    int lbody = nt_ref(nt, block, "body");
    /* Kernel#loop rescues StopIteration to terminate normally (e.g. an external
       Enumerator's #next at the end). Wrap the loop in a setjmp handler; a
       StopIteration falls through, any other exception re-raises. */
    int gcl = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "int _gcb%d = sp_gc_nroots; (void)_gcb%d;\n", gcl, gcl);
    emit_indent(b, indent); buf_puts(b, "sp_exc_check_depth();\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    emit_indent(b, indent + 1); buf_puts(b, "for (;;) {\n");
    /* The frame this loop opened is live for its body: a `return` from inside
       has to pop it on the way out, like any begin/rescue frame. Without the
       accounting the handler stack grew by one per call and eventually wrote
       past its end (#3781). */
    g_exc_frame_depth++;
    emit_loop_body(c, lbody, b, indent + 2);
    g_exc_frame_depth--;
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent + 1); buf_printf(b, "sp_gc_nroots = _gcb%d;\n", gcl);
    /* a non-local unwind (throw / valued break) lands here only because this
       frame sits between the thrower and its target -- pass it through, like
       every begin/rescue handler does. */
    emit_indent(b, indent + 1);
    buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
    emit_indent(b, indent + 1);
    buf_puts(b, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  if (recv < 0) return 0;
  int body = nt_ref(nt, block, "body");
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  TyKind rt = comp_ntype(c, recv);

  /* A Range Enumerable the array emitters below serve (each_slice/each_cons
     block forms, ...): materialize once into an int array and re-enter with
     the receiver's emission and type overridden -- the block-form mirror of
     the call_recv redispatch, keyed on the same predicate. */
  if (rt == TY_RANGE && range_enum_redispatch(c, id) && g_n_argov < MAX_ARG_OVERRIDE) {
    if (range_float_begin(c, recv)) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"can't iterate from Float\");\n");
      return 1;
    }
    int ta = ++g_tmp, tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); }); SP_GC_ROOT(_t%d);\n",
               ta, tr, rb.p ? rb.p : "", tr, ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_INT_ARRAY;
    int done = emit_iteration_stmt(c, id, b, indent);
    c->ntype[recv] = sv;
    g_n_argov--;
    return done;
  }

  /* (range).step(k) { |x| ... } -- materialise the stepped values (shared with
     the no-block path so they match exactly) and walk them; the element type
     follows the array, int or float. */
  if (sp_streq(name, "step") && (rt == TY_RANGE || rt == TY_FLOAT_RANGE)) {
    int args = nt_ref(nt, id, "arguments"); int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    /* An INTEGER-stepped Range walks its span directly instead of
       materializing it: an endless range would never finish building the
       array, so the block (and its break) was never reached (#3673). */
    if (rt == TY_RANGE && comp_ntype(c, sargv[0]) != TY_FLOAT && sargc == 1) {
      int tr = ++g_tmp, ts2 = ++g_tmp, tl2 = ++g_tmp, tv2 = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_Range _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = ", ts2); emit_int_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      /* a zero step never advances; a negative one simply enumerates nothing */
      buf_printf(b, "if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts2);
      emit_indent(b, indent);
      buf_printf(b, "if (_t%d.first == INTPTR_MIN) sp_raise_cls(\"ArgumentError\","
                    " \"#step for non-numeric beginless ranges is meaningless\");\n", tr);
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = _t%d.last - (_t%d.excl ? 1 : 0);\n", tl2, tr, tr);
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = _t%d.first; _t%d > 0 && _t%d <= _t%d; _t%d += _t%d) {\n",
                 tv2, tr, ts2, tv2, tl2, tv2, ts2);
      if (p0) {
        char elem[32]; snprintf(elem, sizeof elem, "_t%d", tv2);
        emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, elem, b, indent + 1);
      }
      { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", tv2);
        int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
        emit_iter_bind_rest(c, block, rs_np, TY_INT, rs_es, b, indent + 1); }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    int t = ++g_tmp, ti = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    TyKind at, et; const char *aty;
    if (rt == TY_FLOAT_RANGE) {
      /* (1.0..2.0).step(0.5) { } -> the float step array, then iterate it */
      int trf = ++g_tmp;
      buf_printf(&ab, "({ sp_FloatRange _t%d = ", trf); emit_expr(c, recv, &ab);
      buf_printf(&ab, "; sp_frange_step(_t%d, ", trf); emit_float_expr(c, sargv[0], &ab); buf_puts(&ab, "); })");
      at = TY_FLOAT_ARRAY; et = TY_FLOAT; aty = "sp_FloatArray";
    }
    else {
      at = emit_range_step_array(c, id, &ab);
      aty = at == TY_FLOAT_ARRAY ? "sp_FloatArray" : "sp_IntArray";
      et = at == TY_FLOAT_ARRAY ? TY_FLOAT : TY_INT;
    }
    emit_indent(b, indent);
    buf_printf(b, "%s *_t%d = %s; SP_GC_ROOT(_t%d);\n", aty, t, ab.p ? ab.p : "", t);
    free(ab.p);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, t, ti);
    if (p0) {
      char elem[64]; snprintf(elem, sizeof elem, "_t%d->data[_t%d]", t, ti);
      emit_iter_param_assign(c, block, p0_orig, p0, et, elem, b, indent + 1);
    }
    { char rs_es[64]; snprintf(rs_es, sizeof rs_es, "_t%d->data[_t%d]", t, ti);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, et, rs_es, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.times { |i| ... } */
  if (sp_streq(name, "times") && (rt == TY_INT || rt == TY_BIGINT)) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_int_expr(c, recv, &rb);
    /* The count is evaluated ONCE, so a receiver that can change (or change
       something) between rounds has to be hoisted: spliced into the loop
       condition, `rng.next_int(n).times` re-rolled the die every round. */
    if (subtree_has_side_effect(c, recv)) {
      int tn = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = %s;\n", tn, rb.p ? rb.p : "0");
      free(rb.p); memset(&rb, 0, sizeof rb);
      buf_printf(&rb, "_t%d", tn);
    }
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "; _t%d++) {\n", t);
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", t);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, TY_INT, rs_es, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* rational.step(limit[, step]) { |x| ... } -- walk the exact Rational
     sequence, yielding boxed Rational/Integer values. The bounds compare and
     the accumulator advances through the poly numeric tower (sp_poly_add keeps
     a Rational operand rational), so the values stay exact (#2566). */
  if (sp_streq(name, "step") && rt == TY_RATIONAL) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    int tc = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, td = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = sp_box_rational(", tc); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT_RBVAL(_t%d);\n", tc);
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = ", tl); emit_boxed(c, sargv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);\n", tl);
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = ", ts);
    if (sargc >= 2) emit_boxed(c, sargv[1], b); else buf_puts(b, "sp_box_int(1)");
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);\n", ts);
    emit_indent(b, indent);
    buf_printf(b, "if (sp_poly_cmp_ck(_t%d, sp_box_int(0)) == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts);
    emit_indent(b, indent);
    buf_printf(b, "sp_bool _t%d = sp_poly_cmp_ck(_t%d, sp_box_int(0)) > 0;\n", td, ts);
    emit_indent(b, indent);
    buf_printf(b, "for (; _t%d ? sp_poly_le(_t%d, _t%d) : sp_poly_ge(_t%d, _t%d); _t%d = sp_poly_add(_t%d, _t%d)) {\n",
               td, tc, tl, tc, tl, tc, tc, ts);
    if (p0) { char cs[32]; snprintf(cs, sizeof cs, "_t%d", tc); emit_iter_param_assign(c, block, p0_orig, p0, TY_POLY, cs, b, indent + 1); }
    { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", tc);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, TY_POLY, rs_es, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* num.step(limit[, step]) { [|i|] ... } -- stepping loop. A float receiver
     or a float limit/step makes it a float walk (yielding floats), computed
     by iteration count to avoid floating-point drift (CRuby semantics). */
  if (sp_streq(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    /* no limit (or an explicit nil limit): Integer#step iterates unboundedly
       until the block breaks (#2582). Integer receiver + integer step only. */
    int no_limit = sargc == 0 ||
                   (nt_type(nt, sargv[0]) && sp_streq(nt_type(nt, sargv[0]), "NilNode"));
    if (no_limit && rt == TY_INT &&
        (sargc < 2 || comp_ntype(c, sargv[1]) != TY_FLOAT)) {
      int t = ++g_tmp, ts = ++g_tmp;
      emit_indent(b, indent); buf_printf(b, "sp_int _t%d = ", ts);
      if (sargc >= 2) emit_int_expr(c, sargv[1], b); else buf_puts(b, "1");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts);
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = ", t); emit_int_expr(c, recv, b);
      buf_printf(b, "; ; _t%d += _t%d) {\n", t, ts);
      if (p0) { char ts2[32]; snprintf(ts2, sizeof ts2, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts2, b, indent + 1); }
      { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", t);
        int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
        emit_iter_bind_rest(c, block, rs_np, TY_INT, rs_es, b, indent + 1); }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    if (sargc < 1) return 0;
    int is_float = (rt == TY_FLOAT) || comp_ntype(c, sargv[0]) == TY_FLOAT ||
                   (sargc >= 2 && comp_ntype(c, sargv[1]) == TY_FLOAT);
    if (!is_float) {
      int t = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp;
      emit_indent(b, indent); buf_printf(b, "sp_int _t%d = ", tl); emit_int_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent); buf_printf(b, "sp_int _t%d = ", ts);
      if (sargc >= 2) emit_int_expr(c, sargv[1], b); else buf_puts(b, "1");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts);
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 ts, t, tl, t, tl, t, ts);
      if (p0) { char ts2[32]; snprintf(ts2, sizeof ts2, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts2, b, indent + 1); }
    { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", t);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, TY_INT, rs_es, b, indent + 1); }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    int tb = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", tb); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", tl); emit_expr(c, sargv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", ts);
    if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1.0");
    buf_puts(b, ";\n");
    /* a zero step never advances, so CRuby rejects it outright (#3648) */
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts);
    /* n = floor((limit-begin)/step + err); err bounds fp drift (CRuby) */
    emit_indent(b, indent);
    buf_printf(b, "sp_float _t%d_e = (fabs(_t%d)+fabs(_t%d)+fabs(_t%d-_t%d))/fabs(_t%d)*DBL_EPSILON;\n",
               tn, tb, tl, tl, tb, ts);
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d_e > 0.5) _t%d_e = 0.5;\n", tn, tn);
    emit_indent(b, indent);
    /* keep the bound as a float: NaN begin/limit/step makes `i <= NaN` false
       (0 iterations, matching CRuby) and an out-of-range begin gives a
       negative/+inf bound naturally, instead of UB from casting NaN to int
       (#3010) */
    buf_printf(b, "sp_float _t%d = floor((_t%d-_t%d)/_t%d + _t%d_e);\n", tn, tl, tb, ts, tn);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; (sp_float)_t%d <= _t%d; _t%d++) {\n", ti, ti, tn, ti);
    if (p0) { char fp_expr[64]; snprintf(fp_expr, sizeof fp_expr, "_t%d + _t%d * _t%d", tb, ti, ts); emit_iter_param_assign(c, block, p0_orig, p0, TY_FLOAT, fp_expr, b, indent + 1); }
    { char rs_es[64]; snprintf(rs_es, sizeof rs_es, "_t%d + _t%d * _t%d", tb, ti, ts);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, TY_FLOAT, rs_es, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* hash.each / each_pair { |k, v| ... } */
  if ((sp_streq(name, "each") || sp_streq(name, "each_pair")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    /* Hoist the receiver into one temp instead of splicing its text into the
       loop bound and every key/val access. When the receiver is an inlined
       block-method statement-expression (e.g. a Ruby-defined `group_by` that
       lowers to `({ ...produce hash... })`), re-emitting it per access both
       truncates into invalid C and re-runs its side effects. The temp is a
       root, as each_key's and each_value's hoist below already is: the loop
       reads its `len` as the bound on every turn and its `order` on every
       yield, and a receiver that is itself a temporary -- the hash a method
       call returned -- has no other holder while the block allocates. */
    int th = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "sp_%sHash *_t%d = ", hn, th);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d);\n", th);
    Buf rb; memset(&rb, 0, sizeof rb);
    buf_printf(&rb, "_t%d", th);
    /* Mutating the key set during #each: CRuby refuses a new key outright, and
       supports deleting the current one -- which slides the next entry into
       this slot, so the index must not advance past it (#3569). */
    int tn0 = ++g_tmp, tk0 = ++g_tmp;
    int key_is_int = (ty_hash_key(rt) == TY_SYMBOL || ty_hash_key(rt) == TY_INT);
    emit_indent(b, indent);
    buf_printf(b, "sp_int _t%d = %s->len;\n", tn0, rb.p);
    if (key_is_int) {
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = 0;\n", tk0);
    }
    emit_indent(b, indent);
    /* The advance is the loop's own third clause, not a statement at the end of
       the body: `next` in the block is a C `continue`, which runs the third
       clause and skips whatever the body ends with -- as a trailing statement
       it was skipped and the same entry ran forever (#3782). */
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < %s->len; ", t, t, rb.p);
    buf_printf(b, "({ if (%s->len > _t%d) sp_raise_cls(\"RuntimeError\","
                  " \"can't add a new key into hash during iteration\"); ", rb.p, tn0);
    if (key_is_int)
      buf_printf(b, "if (_t%d < %s->len && (sp_int)%s->order[_t%d] == _t%d) _t%d++;"
                    " else _t%d = %s->len; })) {\n",
                 t, rb.p, rb.p, t, tk0, t, tn0, rb.p);
    else buf_printf(b, "_t%d++; })) {\n", t);
    if (key_is_int) {
      emit_indent(b, indent + 1);
      buf_printf(b, "_t%d = (sp_int)%s->order[_t%d];\n", tk0, rb.p, t);
    }
    if (p0 && !p1) {
      /* a SOLO block param receives the boxed [k, v] PAIR (CRuby yields the
         pair as one argument; two params auto-splat it below) */
      int tpp = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", p0, tpp, tpp);
      if (rt == TY_POLY_POLY_HASH) {
        buf_printf(b, "sp_PolyArray_push(_t%d, %s->keys[%s->order[_t%d]]); ", tpp, rb.p, rb.p, t);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s->vals[%s->order[_t%d]]); ", tpp, rb.p, rb.p, t);
      }
      else {
        char kx[256], vx[288];
        snprintf(kx, sizeof kx, "%s->order[_t%d]", rb.p, t);
        snprintf(vx, sizeof vx, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
        Buf bx; memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, ty_hash_key(rt), kx, &bx);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", tpp, bx.p ? bx.p : ""); free(bx.p);
        memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, ty_hash_val(rt), vx, &bx);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", tpp, bx.p ? bx.p : ""); free(bx.p);
      }
      buf_printf(b, "sp_box_poly_array(_t%d); })", tpp);
      buf_puts(b, ";\n");
    }
    else if (p0) {
      /* The param may be poly (a name shared across hashes of differing element
         types); box a concrete key into the poly slot. */
      const char *raw0 = block_param_name(c, block, 0);
      LocalVar *pv0 = raw0 ? scope_local(comp_scope_of(c, block), raw0) : NULL;
      TyKind want0 = ty_hash_key(rt);
      int box0 = pv0 && pv0->type == TY_POLY && want0 != TY_POLY;
      char src0[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src0, sizeof src0, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src0, sizeof src0, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box0) emit_boxed_text(c, want0, src0, b); else buf_puts(b, src0);
      buf_puts(b, ";\n");
    }
    if (p1) {
      const char *raw1 = block_param_name(c, block, 1);
      LocalVar *pv1 = raw1 ? scope_local(comp_scope_of(c, block), raw1) : NULL;
      TyKind want1 = ty_hash_val(rt);
      int box1 = pv1 && pv1->type == TY_POLY && want1 != TY_POLY;
      char src1[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src1, sizeof src1, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src1, sizeof src1, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p1);
      if (box1) emit_boxed_text(c, want1, src1, b); else buf_puts(b, src1);
      buf_puts(b, ";\n");
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.each_value { |v| ... } / each_key { |k| ... } -- single param */
  if ((sp_streq(name, "each_value") || sp_streq(name, "each_key")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int is_val = sp_streq(name, "each_value");
    int t = ++g_tmp, th2 = ++g_tmp;
    /* Evaluate the receiver ONCE into a rooted temp: a call receiver (the ENV
       snapshot) re-evaluated per access built a fresh unrooted hash each time
       and the GC swept the earlier ones mid-loop (#2842). */
    {
      Buf hb0; memset(&hb0, 0, sizeof hb0);
      emit_expr(c, recv, &hb0);
      emit_indent(b, indent);
      buf_printf(b, "%s _t%d = %s; SP_GC_ROOT(_t%d);\n",
                 c_type_name(rt), th2, hb0.p ? hb0.p : "NULL", th2);
      free(hb0.p);
    }
    Buf rb; memset(&rb, 0, sizeof rb);
    buf_printf(&rb, "_t%d", th2);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "->len; _t%d++) {\n", t);
    if (p0) {
      /* The param may be poly (shared name across hashes of differing
         element types); box a concrete element into the poly slot. */
      const char *raw = block_param_name(c, block, 0);
      LocalVar *pv = raw ? scope_local(comp_scope_of(c, block), raw) : NULL;
      TyKind want = is_val ? ty_hash_val(rt) : ty_hash_key(rt);
      int box = pv && pv->type == TY_POLY && want != TY_POLY;
      char src[256];
      if (rt == TY_POLY_POLY_HASH) {
        /* PolyPolyHash: ->order[i] is an index; keys/vals hold sp_RbVal */
        if (is_val)
          snprintf(src, sizeof src, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
        else
          snprintf(src, sizeof src, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      }
      else if (is_val)
        snprintf(src, sizeof src, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      else
        snprintf(src, sizeof src, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box) emit_boxed_text(c, want, src, b);
      else buf_puts(b, src);
      buf_puts(b, ";\n");
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.delete_if / reject! / select! / filter! / keep_if { |k, v| cond }
     as a statement: the loop alone, its value unread (the expression form
     lives in emit_hash_call) */
  if ((sp_streq(name, "delete_if") || sp_streq(name, "reject!") || sp_streq(name, "select!") ||
       sp_streq(name, "filter!") || sp_streq(name, "keep_if")) && ty_is_hash(rt) && block >= 0) {
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    int tr2, to2, tw2;
    int ok = emit_hash_filter_loop(c, recv, block, rt, name, rb2.p ? rb2.p : "NULL", b, indent, &tr2, &to2, &tw2);
    free(rb2.p);
    if (ok) return 1;
  }

  /* array.each_with_index { |x, i| ... } */
  if (sp_streq(name, "each_with_index") && ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    hoist_loop_recv(c, rt, &rb, b, indent);
    Scope *cs_ewi = comp_scope_of(c, id);
    LocalVar *clv_ewi_p1 = (p1 && cs_ewi) ? scope_local(cs_ewi, p1) : NULL;
    LocalVar *clv_ewi_p0 = (p0 && cs_ewi) ? scope_local(cs_ewi, p0) : NULL;
    TyKind ewi_et = ty_array_elem(rt);
    int p0_box_poly = clv_ewi_p0 && clv_ewi_p0->type == TY_POLY && ewi_et != TY_POLY;
    int p1_box_poly = clv_ewi_p1 && clv_ewi_p1->type == TY_POLY;
    /* Save outer variables before loop */
    int ts_p0 = 0, ts_p1 = 0;
    if (p0 && clv_ewi_p0) {
      ts_p0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p0->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p0, p0); free(ot.p);
    }
    if (p1 && clv_ewi_p1) {
      ts_p1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p1->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p1, p1); free(ot.p);
    }
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
    buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    /* an unused parameter is pruned by liveness and has no declaration, so
       binding it would name an undeclared C variable (#3853) */
    { Scope *ewsc = comp_scope_of(c, block);
      const char *p0o = block_param_name(c, block, 0);
      if (p0 && (!ewsc || !p0o || !scope_local(ewsc, p0o))) p0 = NULL;
      const char *p1o = block_param_name(c, block, 1);
      if (p1 && (!ewsc || !p1o || !scope_local(ewsc, p1o))) p1 = NULL; }
    if (p0) {
      emit_indent(b, indent + 1);
      if (p0_box_poly) {
        char src[512]; snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        buf_printf(b, "lv_%s = ", p0); emit_boxed_text(c, ewi_et, src, b); buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    if (p1) {
      emit_indent(b, indent + 1);
      if (p1_box_poly) buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p1, t);
      else buf_printf(b, "lv_%s = _t%d;\n", p1, t);
    }
    /* splat-only block packs BOTH yielded values: [element, index] */
    if (!p0 && !p1 && block_rest_name(c, block) && *block_rest_name(c, block)) {
      const char *rr = rename_local(block_rest_name(c, block));
      emit_indent(b, indent + 1);
      /* assign the prologue-declared, slot-rooted rest local (see
         emit_iter_bind_rest) rather than shadow-declaring a fresh one */
      buf_printf(b, "lv_%s = sp_PolyArray_new();\n", rr);
      char rsrc[512]; snprintf(rsrc, sizeof rsrc, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_PolyArray_push(lv_%s, ", rr);
      emit_boxed_text(c, ewi_et, rsrc, b);
      buf_puts(b, ");\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_PolyArray_push(lv_%s, sp_box_int(_t%d));\n", rr, t);
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    /* Restore outer variables */
    if (p0 && ts_p0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_p0); }
    if (p1 && ts_p1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1, ts_p1); }
    free(rb.p);
    return 1;
  }

  /* array.zip(other) { |a, b| ... } — block form, returns nil */
  if (sp_streq(name, "zip") && (ty_is_array(rt) || rt == TY_POLY) && block >= 0) {
    int zargs_n = nt_ref(nt, id, "arguments");
    int zargc = 0; const int *zargv = zargs_n >= 0 ? nt_arr(nt, zargs_n, "arguments", &zargc) : NULL;
    /* The receiver, too, can be an array only at run time (a row read out of a
       poly table): walk it through the boxed accessors. Without this the call
       fell to the runtime dispatch, which has no zip arm at all. */
    int recv_poly = !ty_is_array(rt);
    const char *k = recv_poly ? "Poly" : ((rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt));
    if (k && zargc == 1 && zargv) {
      TyKind a0t = comp_ntype(c, zargv[0]);
      const char *k2 = ty_is_array(a0t) ? ((a0t == TY_POLY_ARRAY) ? "Poly" : array_kind(a0t)) : NULL;
      /* The other operand may be an array only at run time (a poly element of
         a table of rows). Read it through the boxed accessor rather than
         handing an sp_RbVal to the typed one. */
      int arg_poly = (k2 == NULL);
      if (!k2) k2 = k;
      TyKind et = recv_poly ? TY_POLY : ty_array_elem(rt);
      TyKind et2 = ty_is_array(a0t) ? ty_array_elem(a0t) : (arg_poly ? TY_POLY : et);
      const char *p1n = block_param_name(c, block, 1); if (p1n) p1n = rename_local(p1n);
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb);
      if (recv_poly) emit_boxed(c, recv, &rb); else emit_expr(c, recv, &rb);
      Buf ob; memset(&ob, 0, sizeof ob);
      if (arg_poly) emit_boxed(c, zargv[0], &ob); else emit_expr(c, zargv[0], &ob);
      if (recv_poly) {
        int trz = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", trz, rb.p ? rb.p : "sp_box_nil()", trz);
        free(rb.p); memset(&rb, 0, sizeof rb);
        buf_printf(&rb, "_t%d", trz);
      }
      else hoist_loop_recv(c, rt, &rb, b, indent);
      if (ty_is_array(a0t)) hoist_loop_recv(c, a0t, &ob, b, indent);
      Scope *zs = comp_scope_of(c, id);
      LocalVar *zlv0 = (p0 && zs) ? scope_local(zs, p0) : NULL;
      LocalVar *zlv1 = (p1n && zs) ? scope_local(zs, p1n) : NULL;
      int zs0 = 0, zs1 = 0;
      if (p0 && zlv0) {
        zs0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv0->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs0, p0); free(ot.p);
      }
      if (p1n && zlv1) {
        zs1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv1->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs1, p1n); free(ot.p);
      }
      emit_indent(b, indent);
      if (recv_poly)
        buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_poly_arr_len(%s); _t%d++) {\n",
                   t, t, rb.p ? rb.p : "sp_box_nil()", t);
      else
        buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) {\n",
                   t, t, k, rb.p ? rb.p : "NULL", t);
      if (p0 && zlv0 && !p1n) {
        /* SOLO param: the boxed [e1, e2] tuple (two params auto-splat it) */
        int tpz = ++g_tmp;
        char s1[512], s2[512];
        if (recv_poly) snprintf(s1, sizeof s1, "sp_poly_arr_get(%s, _t%d)", rb.p ? rb.p : "sp_box_nil()", t);
        else snprintf(s1, sizeof s1, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        if (arg_poly) snprintf(s2, sizeof s2, "sp_poly_arr_get(%s, _t%d)", ob.p ? ob.p : "sp_box_nil()", t);
        else snprintf(s2, sizeof s2, "sp_%sArray_get(%s, _t%d)", k2, ob.p ? ob.p : "NULL", t);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = ({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", p0, tpz, tpz);
        Buf bx; memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, et, s1, &bx);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", tpz, bx.p ? bx.p : ""); free(bx.p);
        memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, et2, s2, &bx);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", tpz, bx.p ? bx.p : ""); free(bx.p);
        buf_printf(b, "sp_box_poly_array(_t%d); });\n", tpz);
      }
      else if (p0 && zlv0) {
        char src[512];
        if (recv_poly) snprintf(src, sizeof src, "sp_poly_arr_get(%s, _t%d)", rb.p ? rb.p : "sp_box_nil()", t);
        else snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        int box0 = zlv0->type == TY_POLY && et != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p0);
        if (box0) emit_boxed_text(c, et, src, b);
        else buf_puts(b, src);
        buf_puts(b, ";\n");
      }
      if (p1n && zlv1 && ob.p) {
        char src2[512];
        if (arg_poly) snprintf(src2, sizeof src2, "sp_poly_arr_get(%s, _t%d)", ob.p, t);
        else snprintf(src2, sizeof src2, "sp_%sArray_get(%s, _t%d)", k2, ob.p, t);
        int box1 = zlv1->type == TY_POLY && et2 != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p1n);
        if (box1) emit_boxed_text(c, et2, src2, b);
        else buf_puts(b, src2);
        buf_puts(b, ";\n");
      }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      if (p0 && zs0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, zs0); }
      if (p1n && zs1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1n, zs1); }
      free(rb.p); free(ob.p);
      return 1;
    }
  }

  /* poly_val.each { |v| ... }: runtime-dispatch over a boxed array or hash */
  if ((sp_streq(name, "each") || sp_streq(name, "each_pair") ||
       sp_streq(name, "each_value") || sp_streq(name, "each_key") ||
       sp_streq(name, "each_with_index") ||
       /* each_entry yields what each yields for every builtin enumerable, so
          the boxed receiver iterates the same way (#3395, #3987), and
          reverse_each walks the same elements from the other end */
       sp_streq(name, "each_entry") || sp_streq(name, "reverse_each")) &&
      rt == TY_POLY && block >= 0) {
    /* each/each_pair walk the elements (sp_poly_each_elem renders a hash
       entry as a boxed [k, v] pair); each_value/each_key bind one half of
       that pair (the receiver is a hash when these names dispatch);
       each_with_index binds the whole element plus the loop index (no splat). */
    int pv_half = sp_streq(name, "each_value") ? 1 :
                  sp_streq(name, "each_key") ? 0 : -1;
    int is_ewi = sp_streq(name, "each_with_index");
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    /* The gate read the cached node type (poly); a cloned-body local can have
       settled to a TYPED container since (a per-includer module-method clone
       whose param pinned later, #2008). Re-infer and box the concrete kind --
       a raw typed pointer must never initialize the sp_RbVal receiver. */
    TyKind fresh_rt = infer_type(c, recv);
    if (fresh_rt != TY_POLY && (ty_is_hash(fresh_rt) || ty_is_array(fresh_rt))) {
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, fresh_rt, rb.p ? rb.p : "", &bx);
      free(rb.p); rb = bx;
    }
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = %s;\n", ta, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
    /* Root the boxed receiver so a GC fired by the loop body doesn't free a
       freshly-built collection held only by this temp. */
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", ta);
    emit_indent(b, indent); emit_poly_iter_obj_normalize(c, ta, b);
    emit_indent(b, indent); buf_printf(b, "sp_poly_iter_check(_t%d, \"%s\");\n", ta, name);
    emit_indent(b, indent); buf_printf(b, "sp_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tn, ta);
    emit_indent(b, indent);
    if (sp_streq(name, "reverse_each"))
      buf_printf(b, "for (sp_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", ti, tn, ti, ti);
    else
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    /* multi-param: auto-splat each poly element into params. Ruby splats only
       when the element is itself an Array (sp_poly_each_elem already renders a
       hash pair as a 2-element array, so |k, v| over a hash still splats); a
       non-array element binds param 0 with the rest nil. */
    int npp_poly = 0; while (block_param_name(c, block, npp_poly)) npp_poly++;
    if (is_ewi) {
      /* each_with_index { |v, i| }: bind param 0 to the WHOLE element (never
         splatting a nested array, unlike `each`) and param 1 to the loop index;
         any further params bind to nil each iteration. Every binding is gated on
         the param actually being a declared local -- an UNUSED block param is
         pruned by liveness (scope_local returns NULL and no `lv_<name>` is
         declared), so emitting an assignment to it would reference an undeclared
         C identifier. This mirrors the `each` sibling, which binds only live
         params. */
      Scope *ews = comp_scope_of(c, block);
      const char *e0_orig = block_param_name(c, block, 0);
      LocalVar *e0lv = (e0_orig && ews) ? scope_local(ews, e0_orig) : NULL;
      if (e0lv) {
        TyKind e0t = e0lv->type != TY_UNKNOWN ? e0lv->type : TY_POLY;
        char esrc[64]; snprintf(esrc, sizeof esrc, "sp_poly_each_elem(_t%d, _t%d)", ta, ti);
        emit_indent(b, indent + 1);
        emit_block_param_from_boxed(c, rename_local(e0_orig), e0t, esrc, b);
      }
      const char *i1_orig = block_param_name(c, block, 1);
      LocalVar *i1lv = (i1_orig && ews) ? scope_local(ews, i1_orig) : NULL;
      if (i1lv) {
        TyKind i1t = i1lv->type != TY_UNKNOWN ? i1lv->type : TY_POLY;
        emit_indent(b, indent + 1);
        if (i1t == TY_POLY) buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", rename_local(i1_orig), ti);
        else buf_printf(b, "lv_%s = _t%d;\n", rename_local(i1_orig), ti);
      }
      for (int pj = 2; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        LocalVar *pjlv = (pnj && ews) ? scope_local(ews, pnj) : NULL;
        if (!pjlv) continue;
        TyKind pjt = pjlv->type != TY_UNKNOWN ? pjlv->type : TY_POLY;
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = ", rename_local(pnj));
        emit_block_param_nil(c, pjt, b);
        buf_puts(b, ";\n");
      }
    }
    else if (npp_poly >= 2) {
      Scope *blk_pv = comp_scope_of(c, block);
      int telem = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);\n", telem, ta, ti);
      emit_indent(b, indent + 1);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) {\n", telem, telem);
      for (int pj = 0; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        if (!pnj) break;
        LocalVar *plv = blk_pv ? scope_local(blk_pv, pnj) : NULL;
        TyKind pt = plv ? plv->type : TY_POLY;
        char src[64]; snprintf(src, sizeof src, "sp_poly_arr_get(_t%d, %d)", telem, pj);
        emit_indent(b, indent + 2);
        emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
      }
      emit_indent(b, indent + 1); buf_puts(b, "}\nelse {\n");
      for (int pj = 0; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        if (!pnj) break;
        LocalVar *plv = blk_pv ? scope_local(blk_pv, pnj) : NULL;
        TyKind pt = plv ? plv->type : TY_POLY;
        emit_indent(b, indent + 2);
        if (pj == 0) {
          char src[32]; snprintf(src, sizeof src, "_t%d", telem);
          emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
        }
        else {
          buf_printf(b, "lv_%s = ", rename_local(pnj));
          emit_block_param_nil(c, pt, b);
          buf_puts(b, ";\n");
        }
      }
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
    }
    else if (p0 && pv_half >= 0) {
      /* each_value / each_key: the element is a [k, v] pair; bind one half
         (a non-pair element binds itself, mirroring the splat fallback) */
      Scope *pvs = comp_scope_of(c, block);
      LocalVar *pvl = pvs ? scope_local(pvs, block_param_name(c, block, 0)) : NULL;
      TyKind pvt = (pvl && pvl->type != TY_UNKNOWN) ? pvl->type : TY_POLY;
      int tel = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);\n", tel, ta, ti);
      emit_indent(b, indent + 1);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) _t%d = sp_poly_arr_get(_t%d, %d);\n",
                 tel, tel, tel, tel, pv_half);
      emit_indent(b, indent + 1);
      {
        char src[32]; snprintf(src, sizeof src, "_t%d", tel);
        emit_block_param_from_boxed(c, p0, pvt, src, b);
      }
    }
    else if (p0) {
      /* Coerce the boxed element to the block param's declared type: a param
         inferred as a concrete scalar (String from a would-be str_array whose
         producer actually diverged, #3147) must not take a raw sp_RbVal into a
         const char* slot. emit_block_param_from_boxed inserts the conversion. */
      Scope *e0s = comp_scope_of(c, block);
      LocalVar *e0lv = e0s ? scope_local(e0s, block_param_name(c, block, 0)) : NULL;
      TyKind e0t = (e0lv && e0lv->type != TY_UNKNOWN) ? e0lv->type : TY_POLY;
      char src[64]; snprintf(src, sizeof src, "sp_poly_each_elem(_t%d, _t%d)", ta, ti);
      emit_indent(b, indent + 1);
      emit_block_param_from_boxed(c, p0, e0t, src, b);
    }
    /* a paramless block (`each { ... }`) binds nothing; the loop still runs the
       body once per element for its side effect. */
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* <stored enumerator>.with_index(off) { |x, i| }: drain the enumerator once
     and drive the block with the offset index alongside each element. (The
     immediate chain forms -- arr.each.with_index { } -- are matched earlier by
     the chain emitters; this is the stored-value case. with_object desugars to
     to_a.each_with_object in analyze.) */
  if (rt == TY_ENUMERATOR && sp_streq(name, "with_index")) {
    int wargs = nt_ref(nt, id, "arguments");
    int wargc = 0;
    const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
    if (wargc <= 1) {
      const char *p1_orig = block_param_name(c, block, 1);
      const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
      int ta = ++g_tmp, ti = ++g_tmp, toff = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(b, indent);
      buf_printf(b, "sp_PolyArray *_t%d = sp_Enumerator_to_a(%s); SP_GC_ROOT(_t%d);\n",
                 ta, rb.p ? rb.p : "", ta);
      free(rb.p);
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = ", toff);
      if (wargc == 1 && wargv) emit_int_expr(c, wargv[0], b);
      else buf_puts(b, "0");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
      if (p0) {
        Scope *bs0 = comp_scope_of(c, block);
        LocalVar *b0 = p0_orig ? scope_local(bs0, p0_orig) : NULL;
        TyKind p0t = (b0 && b0->type != TY_UNKNOWN) ? b0->type : TY_POLY;
        char vb0[48];
        snprintf(vb0, sizeof vb0, "sp_PolyArray_get(_t%d, _t%d)", ta, ti);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = ", p0);
        if (p0t == TY_POLY) buf_puts(b, vb0);
        else emit_unbox_text(c, p0t, vb0, b);
        buf_puts(b, ";\n");
      }
      if (p1) {
        Scope *bs1 = comp_scope_of(c, block);
        LocalVar *b1 = p1_orig ? scope_local(bs1, p1_orig) : NULL;
        TyKind p1t = (b1 && b1->type != TY_UNKNOWN) ? b1->type : TY_POLY;
        emit_indent(b, indent + 1);
        if (p1t == TY_POLY)
          buf_printf(b, "lv_%s = sp_box_int(_t%d + _t%d);\n", p1, ti, toff);
        else
          buf_printf(b, "lv_%s = _t%d + _t%d;\n", p1, ti, toff);
      }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
  }

  /* array.each { |x| ... } */
  /* Also drives a materialized or generator Enumerator: `enum.each { }` drains
     it to a poly array once (a generator runs its fiber to completion -- an
     infinite generator loops here, matching Ruby's eager Enumerator#each) and
     reuses the poly-array param binding below. */
  if (sp_streq(name, "each") && (rt == TY_POLY_ARRAY || rt == TY_ENUMERATOR)) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    int ta = ++g_tmp;
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    Scope *cs_pa = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer_pa = (p0 && cs_pa) ? scope_local(cs_pa, p0) : NULL;
    int ts_pa = 0;
    if (outer_pa) {
      ts_pa = ++g_tmp; Buf ot_pa; memset(&ot_pa, 0, sizeof ot_pa); emit_ctype(c, outer_pa->type, &ot_pa);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot_pa.p ? ot_pa.p : "sp_RbVal", ts_pa, p0); free(ot_pa.p);
    }
    emit_indent(b, indent);
    if (rt == TY_ENUMERATOR)
      buf_printf(b, "sp_PolyArray *_t%d = sp_Enumerator_to_a(%s);\n", ta, rb.p ? rb.p : "");
    else
      buf_printf(b, "sp_PolyArray *_t%d = %s;\n", ta, rb.p ? rb.p : "");
    free(rb.p);
    /* Root the receiver: a freshly-built array referenced only by this temp
       is otherwise freed if the loop body triggers GC mid-iteration, leaving
       the next element fetch dangling. */
    emit_indent(b, indent);
    buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", t, t, ta, t);
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed */
      const char *orig_p0n = block_param_name(c, block, 0);
      Scope *blk_sp = comp_scope_of(c, block);
      LocalVar *bp0p = orig_p0n ? scope_local(blk_sp, orig_p0n) : NULL;
      TyKind bp0_tp = bp0p ? bp0p->type : TY_UNKNOWN;
      int npp = 0; while (block_param_name(c, block, npp)) npp++;
      int did_destruct = 0;
      /* An Enumerator's items are boxed PolyArray pairs (each_with_index etc.),
         never a typed inner array, so reading them as an sp_<K>Array would
         misinterpret the memory (#2622). Route those through the poly auto-splat
         below, which unboxes each sub-element. */
      if (npp >= 2 && bp0_tp != TY_POLY && bp0_tp != TY_UNKNOWN && rt != TY_ENUMERATOR) {
        const char *inner_kk = array_kind(ty_array_of(bp0_tp));
        if (inner_kk) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(_t%d, _t%d).v.p;\n",
                     inner_kk, tsub, inner_kk, ta, t);
          for (int pj = 0; pj < npp; pj++) {
            const char *pnj = block_param_name(c, block, pj);
            if (!pnj) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pnj), inner_kk, tsub, pj);
          }
          did_destruct = 1;
        }
      }
      /* Poly-param auto-splat: a 2+ param block whose params weren't proven to
         be a typed inner array (so they're poly/unknown). Ruby auto-splats each
         element ONLY when it is itself an Array -- destructure item k into param
         k (missing item -> nil); a non-array element binds param 0, rest nil. */
      if (!did_destruct && npp >= 2) {
        Scope *blk_sp2 = comp_scope_of(c, block);
        int telem = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, ta, t);
        emit_indent(b, indent + 1);
        buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) {\n", telem, telem);
        for (int pj = 0; pj < npp; pj++) {
          const char *pnj = block_param_name(c, block, pj);
          if (!pnj) break;
          LocalVar *plv = blk_sp2 ? scope_local(blk_sp2, pnj) : NULL;
          TyKind pt = plv ? plv->type : TY_POLY;
          char src[64]; snprintf(src, sizeof src, "sp_poly_arr_get(_t%d, %d)", telem, pj);
          emit_indent(b, indent + 2);
          emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
        }
        emit_indent(b, indent + 1); buf_puts(b, "}\nelse {\n");
        for (int pj = 0; pj < npp; pj++) {
          const char *pnj = block_param_name(c, block, pj);
          if (!pnj) break;
          LocalVar *plv = blk_sp2 ? scope_local(blk_sp2, pnj) : NULL;
          TyKind pt = plv ? plv->type : TY_POLY;
          emit_indent(b, indent + 2);
          if (pj == 0) {
            char src[32]; snprintf(src, sizeof src, "_t%d", telem);
            emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
          }
          else {
            buf_printf(b, "lv_%s = ", rename_local(pnj));
            emit_block_param_nil(c, pt, b);
            buf_puts(b, ";\n");
          }
        }
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        did_destruct = 1;
      }
      if (!did_destruct) {
        /* an unregistered parameter has no C declaration (see
           emit_iter_param_assign): binding it would name a variable that does
           not exist (#3853) */
        Scope *bsc = comp_scope_of(c, block);
        LocalVar *plv = bsc ? scope_local(bsc, block_param_name(c, block, 0)) : NULL;
        if (plv && plv->type != TY_UNKNOWN) {
          emit_indent(b, indent + 1);
          buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, ta, t);
        }
      }
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer_pa) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_pa); }
    return 1;
  }
  if ((sp_streq(name, "each") || sp_streq(name, "each_entry") || sp_streq(name, "reverse_each")) &&
      ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int rev = sp_streq(name, "reverse_each");
    int t = ++g_tmp, tn = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    hoist_loop_recv(c, rt, &rb, b, indent);
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    TyKind et = p0 ? ty_array_elem(rt) : TY_UNKNOWN;
    Scope *cs = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer = (p0 && cs) ? scope_local(cs, p0) : NULL;
    int box_to_poly = outer && outer->type == TY_POLY && et != TY_POLY;
    int ts = 0;
    if (outer) {
      /* Block params shadow outer variables in Ruby; save and restore */
      ts = ++g_tmp;
      Buf ot_ea; memset(&ot_ea, 0, sizeof ot_ea); emit_ctype(c, outer->type, &ot_ea);
      emit_indent(b, indent);
      buf_printf(b, "%s _t%d = lv_%s;\n", ot_ea.p ? ot_ea.p : "sp_RbVal", ts, p0); free(ot_ea.p);
    }
    if (rev) { emit_indent(b, indent); buf_printf(b, "sp_int _t%d = sp_%sArray_length(%s);\n", tn, k, rb.p); }
    emit_indent(b, indent);
    if (rev) buf_printf(b, "for (sp_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", t, tn, t, t);
    else {
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
      buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    }
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed
         (e.g. `[[1,2],[3,4]].each { |a,b| }` or numbered `{ _1; _2 }`).
         The poly element is an inner typed array; unbox and destructure. */
      Scope *blk_s = comp_scope_of(c, block);
      /* Use original (unrenameD) name for scope lookup; p0 is already renamed */
      const char *orig_p0_name = block_param_name(c, block, 0);
      LocalVar *bp0 = orig_p0_name ? scope_local(blk_s, orig_p0_name) : NULL;
      TyKind bp0_type = bp0 ? bp0->type : TY_UNKNOWN;
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np >= 2 && sp_streq(k, "Poly") && bp0_type != TY_POLY && bp0_type != TY_UNKNOWN) {
        /* Get the inner array kind from the first param's element type */
        const char *inner_k = array_kind(ty_array_of(bp0_type));
        if (inner_k) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(", inner_k, tsub, inner_k);
          buf_puts(b, rb.p); buf_printf(b, ", _t%d).v.p;\n", t);
          for (int pj = 0; pj < np; pj++) {
            const char *pname2 = block_param_name(c, block, pj);
            if (!pname2) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pname2), inner_k, tsub, pj);
          }
          goto each_body;
        }
      }
      /* Poly-typed params over poly elements auto-splat, which is what `each`
         already does through its own lowering: without it
         `each_with_index.to_a.reverse_each { |x, i| }` bound the whole
         [value, index] pair to x and left i nil (#4326). The helper writes
         through g_pre, so point it at this statement buffer. */
      if (np >= 2 && sp_streq(k, "Poly")) {
        char es_as[600];
        snprintf(es_as, sizeof es_as, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        Buf *sv_pre = g_pre; g_pre = b;
        int did = emit_iter_autosplat(c, block, rt, es_as, indent + 1);
        g_pre = sv_pre;
        if (did) goto each_body;
      }
      emit_indent(b, indent + 1);
      if (box_to_poly) {
        if (et == TY_INT) buf_printf(b, "lv_%s = sp_box_int(sp_%sArray_get(", p0, k);
        else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(sp_%sArray_get(", p0, k);
        else if (et == TY_FLOAT) buf_printf(b, "lv_%s = sp_box_float(sp_%sArray_get(", p0, k);
        else if (et == TY_BOOL) buf_printf(b, "lv_%s = sp_box_bool(sp_%sArray_get(", p0, k);
        else buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d)", t);
        if (et == TY_INT || et == TY_STRING || et == TY_FLOAT || et == TY_BOOL) buf_puts(b, ")");
        buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    /* a `*rest` param (splat-only wraps the element; alongside requireds it
       binds empty for scalar elements) */
    { char rs_es[560]; snprintf(rs_es, sizeof rs_es, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      TyKind rs_et = ty_array_elem(rt);
      if (emit_iter_bind_rest(c, block, rs_np, rs_et, rs_es, b, indent + 1) < 0) {
        unsupported(c, id, "block splat parameter alongside required params over a poly element");
        return 1;
      } }
    each_body:
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts); }
    free(rb.p);
    return 1;
  }

  /* int_array.combination(k)/permutation(k) { |c| ... } -- yield each k-element
     sub-array as a fresh int_array. permutation also accepts the argless
     (full-length) form. */
  if ((sp_streq(name, "combination") || sp_streq(name, "permutation") ||
       sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation")) &&
      rt == TY_INT_ARRAY) {
    int is_perm = sp_streq(name, "permutation");
    const char *genfn = sp_streq(name, "permutation") ? "sp_IntArray_permutation"
                      : sp_streq(name, "combination") ? "sp_IntArray_combination"
                      : sp_streq(name, "repeated_permutation") ? "sp_IntArray_repeated_permutation"
                      : "sp_IntArray_repeated_combination";
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 && !(is_perm && ac == 0)) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "{ sp_IntArray *_t%d = ", ta); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n"); free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PtrArray *_t%d = %s(_t%d, ", tc, genfn, ta);
    if (ac == 1) emit_int_expr(c, av[0], b); else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
    buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) {
      Scope *cbsc = comp_scope_of(c, block);
      LocalVar *clv = cbsc ? scope_local(cbsc, p0) : NULL;
      TyKind cpt = clv ? clv->type : TY_UNKNOWN;
      emit_indent(b, indent + 2);
      if (cpt == TY_POLY || cpt == TY_UNKNOWN)
        buf_printf(b, "lv_%s = sp_box_obj((sp_IntArray *)_t%d->data[_t%d], SP_BUILTIN_INT_ARRAY);\n", p0, tc, ti);
      else
        buf_printf(b, "lv_%s = (sp_IntArray *)_t%d->data[_t%d];\n", p0, tc, ti);
    }
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_cons(n) { |a, b, ...| } -- sliding window of n consecutive
     elements; a single param binds the n-element sub-array, multiple params
     destructure the window. The hoisted receiver is rooted for the same
     reason Hash#each's above is: its length is the loop bound, re-read every
     turn, and a receiver the program does not name has no other holder. */
  if (sp_streq(name, "each_cons") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int ec = 0; const int *eav = args >= 0 ? nt_arr(nt, args, "arguments", &ec) : NULL;
    if (ec != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np = 0; while (block_param_name(c, block, np)) np++;
    int ta = ++g_tmp, tnn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s; ", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_gc_root_tmp(c, rt, ta, b); buf_puts(b, "\n");
    emit_indent(b, indent); buf_printf(b, "sp_int _t%d = ", tnn); emit_int_expr(c, eav[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, tnn, k, ta, ti);
    if (np == 1) {
      const char *pn = block_param_name(c, block, 0);
      const char *rpn = rename_local(pn);
      Scope *csc_ec = comp_scope_of(c, block);
      LocalVar *clv_ec = csc_ec ? scope_local(csc_ec, pn) : NULL;
      TyKind csaved_ec = clv_ec ? clv_ec->type : TY_UNKNOWN;
      int use_shadow_ec = clv_ec && clv_ec->type != rt && rt != TY_UNKNOWN;
      if (use_shadow_ec) {
        int bodyBn = 0; const int *bodyBb = body >= 0 ? nt_arr(nt, body, "body", &bodyBn) : NULL;
        clv_ec->type = rt;
        for (int j = 0; j < bodyBn; j++) infer_type(c, bodyBb[j]);
        emit_indent(b, indent + 1); buf_puts(b, "{\n");
        emit_indent(b, indent + 2); emit_ctype(c, rt, b);
        buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_loop_body(c, body, b, indent + 2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        clv_ec->type = csaved_ec;
      }
      else {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_loop_body(c, body, b, indent + 1);
      }
    }
    else {
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, block, pj);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* ("a".."e").each { |s| ... } -- a string-endpoint range has no int sp_Range
     representation, so materialize the succ-sequence as a StrArray and loop over
     it. The block param is shadow-typed String for the body. */
  /* `(1..2).each { body }` with no block parameter iterates just the same: the
     emitter required one, so a paramless block fell through to NoMethodError
     (the `throw` idiom under catch is written that way) (#3858). */
  if (sp_streq(name, "each") && rt == TY_RANGE && !p0 && block >= 0 &&
      nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockNode") &&
      !range_float_begin(c, recv)) {
    int t0 = ++g_tmp, ts0 = ++g_tmp, te0 = ++g_tmp, ti0 = ++g_tmp;
    Buf rb0; memset(&rb0, 0, sizeof rb0); emit_expr(c, recv, &rb0);
    emit_indent(b, indent);
    buf_printf(b, "sp_Range _t%d = %s;\n", t0, rb0.p ? rb0.p : "");
    free(rb0.p);
    emit_indent(b, indent);
    buf_printf(b, "sp_int _t%d = sp_range_step(_t%d); sp_int _t%d = _t%d.last - (_t%d.excl ? (_t%d > 0 ? 1 : -1) : 0);\n",
               ts0, t0, te0, t0, t0, ts0);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = _t%d.first; _t%d > 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
               ti0, t0, ts0, ti0, te0, ti0, te0, ti0, ts0);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }
  if (sp_streq(name, "each") && rt == TY_RANGE && p0) {
    if (range_float_begin(c, recv)) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"can't iterate from Float\");\n");
      return 1;
    }
    {
      /* a beginless range cannot be enumerated from nil (#3066) */
      int rn0 = unwrap_parens(c, recv);
      if (rn0 >= 0 && nt_type(nt, rn0) && sp_streq(nt_type(nt, rn0), "RangeNode") &&
          nt_ref(nt, rn0, "left") < 0) {
        emit_indent(b, indent);
        buf_puts(b, "sp_raise_cls(\"TypeError\", \"can't iterate from NilClass\");\n");
        return 1;
      }
    }
    int rnode = unwrap_parens(c, recv);
    if (rnode >= 0 && nt_type(nt, rnode) && sp_streq(nt_type(nt, rnode), "RangeNode")) {
      int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
      if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
        int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
        int ta = ++g_tmp, ti = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta);
        emit_expr(c, lo, b); buf_puts(b, ", "); emit_expr(c, hi, b); buf_printf(b, ", %d);\n", excl);
        /* Root the materialized array: the loop body can allocate (and trigger
           GC), which would otherwise sweep it out from under sp_StrArray_get. */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
        Scope *ssc = comp_scope_of(c, block);
        LocalVar *slv = (ssc && p0_orig) ? scope_local(ssc, p0_orig) : NULL;
        TyKind saved = slv ? slv->type : TY_UNKNOWN;
        int use_shadow = slv && slv->type != TY_STRING;
        emit_indent(b, indent);
        buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ta, ti);
        if (use_shadow) {
          int sbn = 0; const int *sbb = body >= 0 ? nt_arr(nt, body, "body", &sbn) : NULL;
          slv->type = TY_STRING;
          for (int j = 0; j < sbn; j++) infer_type(c, sbb[j]);
          emit_indent(b, indent + 1);
          buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, ta, ti);
          emit_loop_body(c, body, b, indent + 1);
          slv->type = saved;
        }
        else {
          emit_indent(b, indent + 1);
          buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, ta, ti);
          emit_loop_body(c, body, b, indent + 1);
        }
        emit_indent(b, indent); buf_puts(b, "}\n");
        return 1;
      }
    }
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_Range _t%d = ", t); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n");
    free(rb.p);
    /* Under --int-overflow=promote the loop var is widened to poly; drive the
       loop with a fresh sp_int temp and re-box the counter each iteration
       (mirrors emit_for's poly-counter arm). */
    LocalVar *clv = p0_orig ? scope_local(comp_scope_of(c, block), p0_orig) : NULL;
    /* Direction-aware bounds: a descending range (n.downto(m)) walks by its
       negative step, which the plain ascending loop would skip entirely. */
    int ts = ++g_tmp, te = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "sp_int _t%d = sp_range_step(_t%d); sp_int _t%d = _t%d.last - (_t%d.excl ? (_t%d > 0 ? 1 : -1) : 0);\n",
               ts, t, te, t, t, ts);
    if (clv && clv->type == TY_POLY) {
      int tc = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = _t%d.first; _t%d > 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 tc, t, ts, tc, te, tc, te, tc, ts);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p0, tc);
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = _t%d.first; _t%d > 0 ? lv_%s <= _t%d : lv_%s >= _t%d; lv_%s += _t%d) {\n",
               p0, t, ts, p0, te, p0, te, p0, ts);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.upto(m) / n.downto(m) { [|i|] ... } -- a fresh temp drives the loop and
     the block param (if any) is rebound from it each iteration, like n.times.
     A blockless-param form (`1.upto(5) { body }`) must still run the body. */
  if ((sp_streq(name, "upto") || sp_streq(name, "downto")) && rt == TY_INT) {
    int up = sp_streq(name, "upto");
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = NULL;
    if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
    if (argc != 1) return 0;
    Buf lo; memset(&lo, 0, sizeof lo); emit_expr(c, recv, &lo);
    Buf hi; memset(&hi, 0, sizeof hi); emit_expr(c, argv[0], &hi);
    int ti = ++g_tmp;
    /* the limit sits in the loop condition, so a side-effecting one would be
       re-evaluated every round: it is computed once in Ruby */
    if (subtree_has_side_effect(c, argv[0])) {
      int th = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = %s;\n", th, hi.p ? hi.p : "0");
      free(hi.p); memset(&hi, 0, sizeof hi);
      buf_printf(&hi, "_t%d", th);
    }
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = ", ti); buf_puts(b, lo.p);
    buf_printf(b, "; _t%d %s ", ti, up ? "<=" : ">="); buf_puts(b, hi.p);
    buf_printf(b, "; _t%d%s) {\n", ti, up ? "++" : "--");
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", ti); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    { char rs_es[32]; snprintf(rs_es, sizeof rs_es, "_t%d", ti);
      int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
      emit_iter_bind_rest(c, block, rs_np, TY_INT, rs_es, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(lo.p); free(hi.p);
    return 1;
  }

  /* "a".upto("e") { |c| ... } -- string succ-sequence loop, mirrors
     sp_StrArray_from_string_range semantics (inclusive, 4096-cap) */
  if (sp_streq(name, "upto") && rt == TY_STRING && p0) {
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc != 1) return 0;
    int te = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp, tcmp = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", te); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", tc); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "for (int _t%d = 0; _t%d < 4096; _t%d++) {\n", ti, ti, ti);
    emit_indent(b, indent + 1); buf_printf(b, "int _t%d = sp_str_cmp_bytes(_t%d, _t%d);\n", tcmp, tc, te);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d > 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "lv_%s = _t%d;\n", p0, tc);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d == 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "_t%d = sp_str_succ(_t%d);\n", tc, tc);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* recv.tap { |p| body } -- run block for side effects, preserve outer var */
  if (sp_streq(name, "tap") && recv >= 0) {
    TyKind et = infer_type(c, recv);
    Scope *tsc = p0_orig ? comp_scope_of(c, block) : NULL;
    LocalVar *tlv0 = (tsc && p0_orig) ? scope_local(tsc, p0_orig) : NULL;
    TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
    int use_shadow_t = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
    int tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, et, b);
    buf_printf(b, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
    /* An OBJECT-receiver tap whose param widened to poly (it escaped through
       yield(_1) / a store) cannot be shadowed with a concrete object slot;
       box the receiver into the poly param instead (#3140). An array/hash
       receiver keeps its shadow path -- boxing would strip its methods and
       break `nums.tap { |a| a.sort! }`. */
    int tap_escapes = tlv0 && tsaved0 == TY_POLY && ty_is_object(et);
    /* tap runs the block once, not in a loop, so a `next` in it has no C loop
       to continue out of: give it one (#3978). */
    int tap_next = subtree_has_own_next(nt, body);
    const char *sv_tap_nx = g_ie_next_var;
    if (tap_next) g_ie_next_var = NULL;
    if (use_shadow_t && !tap_escapes) {
      int tbody_bn = 0; const int *tbody_bb = body >= 0 ? nt_arr(nt, body, "body", &tbody_bn) : NULL;
      tlv0->type = et;
      for (int j = 0; j < tbody_bn; j++) infer_type(c, tbody_bb[j]);
      emit_indent(b, indent); buf_puts(b, tap_next ? "do {\n" : "{\n");
      emit_indent(b, indent + 1); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = _t%d;\n", p0, tr);
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, tap_next ? "} while (0);\n" : "}\n");
      tlv0->type = tsaved0;
    }
    else {
      if (p0) {
        emit_indent(b, indent);
        /* the param's declared slot may be wider than the receiver -- a `_1`
           that escapes through `yield(_1)` widens to poly, so box the object
           into it rather than assigning the raw pointer (#3140) */
        TyKind ptt = tlv0 ? tlv0->type : et;
        if (ptt == TY_POLY && ty_is_object(et)) {
          char src[32]; snprintf(src, sizeof src, "_t%d", tr);
          buf_printf(b, "lv_%s = ", p0);
          emit_boxed_text(c, et, src, b);
          buf_puts(b, ";\n");
        }
        else {
          buf_printf(b, "lv_%s = _t%d;\n", p0, tr);
        }
      }
      if (tap_next) { emit_indent(b, indent); buf_puts(b, "do {\n"); }
      emit_loop_body(c, body, b, indent + (tap_next ? 1 : 0));
      if (tap_next) { emit_indent(b, indent); buf_puts(b, "} while (0);\n"); }
    }
    g_ie_next_var = sv_tap_nx;
    return 1;
  }

  /* array.cycle(n) { |p| body } -- repeat n times over the array; the
     argless form cycles forever (a block `break` is the only exit). Rooted
     hoist, as each_cons above. */
  if (sp_streq(name, "cycle") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int cyc_argc = 0; const int *cyc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &cyc_argc) : NULL;
    if (cyc_argc > 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    TyKind et = ty_array_elem(rt);
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_cy = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s; ", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_gc_root_tmp(c, rt, ta, b); buf_puts(b, "\n");
    emit_indent(b, indent);
    if (cyc_argc == 1) {
      buf_printf(b, "sp_int _t%d = ", tn);
      emit_int_expr_nilable(c, cyc_argv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    }
    else {
      /* An empty receiver cycles zero times, not forever: the countless form
         answers nil straight away in CRuby (#3852). */
      buf_printf(b, "if (sp_%sArray_length(_t%d) > 0) for (;;) {\n", k, ta);
    }
    emit_indent(b, indent + 1);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", tj, tj, k, ta, tj);
    int innerIndent = indent + 2;
    if (use_shadow_cy) {
      int cyb_bn = 0; const int *cyb_bb = body >= 0 ? nt_arr(nt, body, "body", &cyb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < cyb_bn; j++) infer_type(c, cyb_bb[j]);
      emit_indent(b, innerIndent); buf_puts(b, "{\n"); innerIndent++;
      emit_indent(b, innerIndent); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj);
      emit_loop_body(c, body, b, innerIndent);
      innerIndent--;
      emit_indent(b, innerIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, innerIndent); buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj); }
      emit_loop_body(c, body, b, innerIndent);
    }
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_slice(n) { |p| body } -- yield subarrays of size n. Rooted
     hoist, as each_cons above. */
  if (sp_streq(name, "each_slice") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int es_argc = 0; const int *es_argv = args >= 0 ? nt_arr(nt, args, "arguments", &es_argc) : NULL;
    if (es_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np_es = 0; while (block_param_name(c, block, np_es)) np_es++;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_es = np_es == 1 && clv0 && clv0->type != rt && rt != TY_UNKNOWN;
    int ta = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s; ", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_gc_root_tmp(c, rt, ta, b); buf_puts(b, "\n");
    emit_indent(b, indent); buf_printf(b, "sp_int _t%d = ", ts);
    emit_int_expr(c, es_argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
               ti, ti, k, ta, ti, ts);
    int bodyIndent = indent + 1;
    if (np_es > 1) {
      /* multi-param: destructure slice elements into individual params */
      for (int pj = 0; pj < np_es; pj++) {
        const char *pn = block_param_name(c, block, pj);
        if (!pn) break;
        emit_indent(b, bodyIndent);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, bodyIndent);
    }
    else if (use_shadow_es) {
      int esb_bn = 0; const int *esb_bb = body >= 0 ? nt_arr(nt, body, "body", &esb_bn) : NULL;
      clv0->type = rt;
      for (int j = 0; j < esb_bn; j++) infer_type(c, esb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); emit_ctype(c, rt, b);
      buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts);
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* str.split(sep) { |piece| body } -- iterate over the substrings; the call
     yields each piece and evaluates to the receiver (handled in statement
     position, where the value is discarded). */
  if (sp_streq(name, "split") && rt == TY_STRING) {
    int args = nt_ref(nt, id, "arguments");
    int sp_argc = 0; const int *sp_argv = args >= 0 ? nt_arr(nt, args, "arguments", &sp_argc) : NULL;
    if (sp_argc > 2) return 0;
    TyKind et = TY_STRING;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_sp = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int tm = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_StrArray *_t%d = ", tm);
    if (sp_argc == 0) buf_printf(b, "sp_str_split_ws(%s);\n", rb.p ? rb.p : "");
    else if (sp_argc == 1) {
      const char *aty = nt_type(nt, sp_argv[0]);
      int ws = (aty && sp_streq(aty, "NilNode")) ||
               (aty && sp_streq(aty, "StringNode") && nt_str(nt, sp_argv[0], "content") &&
                sp_streq(nt_str(nt, sp_argv[0], "content"), " "));
      int reli = re_lit_index(c, sp_argv[0]);
      if (ws) buf_printf(b, "sp_str_split_ws(%s);\n", rb.p ? rb.p : "");
      /* a Regexp separator splits with the engine, the way the expression
         arm does; it is the one class the pattern slot must not word as a
         wrong argument */
      else if (reli >= 0) buf_printf(b, "sp_re_split(sp_re_pat_%d, %s);\n", reli, rb.p ? rb.p : "");
      else if (comp_ntype(c, sp_argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split("); emit_expr(c, sp_argv[0], b);
        buf_printf(b, ", %s);\n", rb.p ? rb.p : "");
      }
      else {
        buf_printf(b, "sp_str_split_drop_trailing(%s, ", rb.p ? rb.p : ""); emit_str_pattern_expr(c, sp_argv[0], b); buf_puts(b, ");\n");
      }
    }
    else {
      int reli = re_lit_index(c, sp_argv[0]);
      if (reli >= 0) buf_printf(b, "sp_re_split_limit(sp_re_pat_%d, %s, ", reli, rb.p ? rb.p : "");
      else if (comp_ntype(c, sp_argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split_limit("); emit_expr(c, sp_argv[0], b);
        buf_printf(b, ", %s, ", rb.p ? rb.p : "");
      }
      else {
        buf_printf(b, "sp_str_split_limit(%s, ", rb.p ? rb.p : ""); emit_str_pattern_expr(c, sp_argv[0], b);
        buf_puts(b, ", ");
      }
      emit_int_expr(c, sp_argv[1], b); buf_puts(b, ");\n");
    }
    free(rb.p);
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tm);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {\n", ti, ti, tm, ti);
    int spIndent = indent + 1;
    if (use_shadow_sp) {
      int sb_bn = 0; const int *sb_bb = body >= 0 ? nt_arr(nt, body, "body", &sb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < sb_bn; j++) infer_type(c, sb_bb[j]);
      emit_indent(b, spIndent); buf_puts(b, "{\n"); spIndent++;
      emit_indent(b, spIndent); buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti);
      emit_loop_body(c, body, b, spIndent);
      spIndent--;
      emit_indent(b, spIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, spIndent); buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti); }
      emit_loop_body(c, body, b, spIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* str.scan(pattern) { |m| body } -- iterate over matches. A regexp pattern
     with capture groups yields group rows, not whole matches: that shape
     is handled by the value-form emitter (which binds/destructures the
     rows), so defer to it. */
  if (sp_streq(name, "scan") && rt == TY_STRING) {
    int args = nt_ref(nt, id, "arguments");
    int sc_argc = 0; const int *sc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &sc_argc) : NULL;
    if (sc_argc != 1) return 0;
    int sc_re = re_lit_index(c, sc_argv[0]);
    if (sc_re < 0 && comp_ntype(c, sc_argv[0]) != TY_STRING) return 0;
    if (sc_re >= 0 && re_has_captures(re_lit_src(c, sc_argv[0]))) return 0;
    TyKind et = TY_STRING;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_sc = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int tm = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    if (sc_re >= 0)
      buf_printf(b, "sp_StrArray *_t%d = sp_re_scan(sp_re_pat_%d, %s);\n",
                 tm, sc_re, rb.p ? rb.p : "");
    else {
      buf_printf(b, "sp_StrArray *_t%d = sp_str_scan(%s, ", tm, rb.p ? rb.p : "");
      emit_expr(c, sc_argv[0], b); buf_puts(b, ");\n");
    }
    free(rb.p);
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tm);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {\n",
               ti, ti, tm, ti);
    int bodyIndent = indent + 1;
    if (use_shadow_sc) {
      int scb_bn = 0; const int *scb_bb = body >= 0 ? nt_arr(nt, body, "body", &scb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < scb_bn; j++) infer_type(c, scb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti);
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  return 0;
}

/* ---- interpolation ---- */

