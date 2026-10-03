/* codegen_iter.c -- block invocation, inline-call, and iteration/loop
   lowering, split out of codegen_call.c. Pure code movement, no logic change. */

#include "codegen_internal.h"
#include "call_plan.h"

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
int inline_splice_depth(void) { return g_inline_depth; }

/* --- inline parameter aliasing ------------------------------------------
   An inlined (yielding) method's String parameter that the body APPENDS to
   is bound as an alias of the caller's variable, not a copy: the copy shares
   the buffer only until the first reallocation, after which the caller's
   variable still names the old bytes. `wrap_into(io, name) { io << "x" }`
   handed the caller back only what its own block appended (#4476), and a
   second nesting lost the outer expansion's post-yield append (#4479).
   The alias is a pointer to the caller's slot, declared under the callee's
   renamed cell name, and the parameter's is_cell is held at 1 while the
   expansion is emitted so the body reads and writes through it. */
/* Does scope mi's body rebind `name`? 0 = never; 1 = only by plain
   assignment (`io = String.new(...)`), which the alias survives: the store
   repoints the cell at a private local, so the caller's variable keeps what
   was appended before the rebind and the body's later reads and writes go to
   its own binding, which is what CRuby does; 2 = by an op-assign, `||=`, or
   a multiple-assignment target, forms the store emitters do not spell that
   way, so such a parameter is copied as before. */
static int inline_param_rebound(Compiler *c, int mi, const char *name) {
  const NodeTable *nt = c->nt;
  int kind = 0;
  for (int q = 0; q < nt->count; q++) {
    if (c->nscope[q] != mi) continue;
    NodeKind k = nt_kind(nt, q);
    if (k != NK_LocalVariableWriteNode && k != NK_LocalVariableOperatorWriteNode &&
        k != NK_LocalVariableOrWriteNode && k != NK_LocalVariableAndWriteNode &&
        k != NK_LocalVariableTargetNode) continue;
    const char *wn = nt_str(nt, q, "name");
    if (!wn || !sp_streq(wn, name)) continue;
    if (k != NK_LocalVariableWriteNode) return 2;
    kind = 1;
  }
  return kind;
}
/* Does scope mi's body mutate `name` in place, or hand it on as a call
   argument (where a callee may)? */
static int inline_param_mutated(Compiler *c, int mi, const char *name) {
  const NodeTable *nt = c->nt;
  for (int q = 0; q < nt->count; q++) {
    if (c->nscope[q] != mi || nt_kind(nt, q) != NK_CallNode) continue;
    int r = nt_ref(nt, q, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode) {
      const char *rn = nt_str(nt, r, "name");
      if (rn && sp_streq(rn, name) && an_str_mutator_name(nt_str(nt, q, "name"))) return 1;
    }
    int aa = nt_ref(nt, q, "arguments"); int an = 0;
    const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++) {
      if (nt_kind(nt, av[k]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[k], "name");
      if (vn && sp_streq(vn, name)) return 1;
    }
  }
  return 0;
}

/* Does the subtree under `id` mutate local `name` in place (a receiver-
   reassigning string mutator on a plain read of it)? The block body of a
   call site, asked about the block's own parameter. */
static int subtree_str_mutates_local(const NodeTable *nt, int id, const char *name) {
  if (id < 0) return 0;
  if (nt_kind(nt, id) == NK_CallNode) {
    int r = nt_ref(nt, id, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode) {
      const char *rn = nt_str(nt, r, "name");
      if (rn && sp_streq(rn, name) && an_str_mutator_name(nt_str(nt, id, "name"))) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_str_mutates_local(nt, nt_ref_at(nt, id, i), name)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *a = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (subtree_str_mutates_local(nt, a[k], name)) return 1;
  }
  return 0;
}
/* Does block `blk` append to its parameter `bp` through a method it hands
   it to, whose parameter there is lent, the handle or appended to (the
   dynamic-call analysis's answer, dyn_block_appends)? */
static int block_param_handed_to_appender(Compiler *c, int blk, const char *bp) {
  for (int j = 0; j < 16; j++) {
    const char *pn = proc_param_name(c, blk, j);
    if (!pn) break;
    if (sp_streq(pn, bp)) return dyn_block_appends(c, blk, j);
  }
  return 0;
}
/* Block `blk`'s keyword parameter that keyword `key` binds, or NULL. */
static const char *block_kw_key(const char *kp, char *buf, size_t n);
static const char *block_kw_param_named(Compiler *c, int blk, const char *key) {
  for (int ki = 0; key; ki++) {
    const char *kp = block_keyword_name(c, blk, ki);
    char knb[160];
    if (!kp) break;
    if (sp_streq(block_kw_key(kp, knb, sizeof knb), key)) return kp;
  }
  return NULL;
}
static int block_local_wants_alias_at(Compiler *c, int blk, const char *bp, int depth);
static int block_local_wants_alias(Compiler *c, int blk, const char *bp) {
  return block_local_wants_alias_at(c, blk, bp, 0);
}
/* The same for block `blk`'s keyword parameter that keyword `key` binds. */
int block_kw_wants_alias(Compiler *c, int blk, const char *key) {
  const char *kp = block_kw_param_named(c, blk, key);
  return kp && block_local_wants_alias(c, blk, kp);
}
/* The block a spliced block's own `yield` reaches, per enclosing inline: a
   literal block passed to an inlined method yields to the block that was
   current where it is written (g_yield_block_fallback while its body is
   spliced). Kept as a stack, since the fallback names one level only, and
   `def run2(x) = run(x) { |u| yield u }` asks it of the block a level out.
   Each entry also keeps what the block's splice runs under, the context the
   inliner hands g_block_id and its fallback: its rename depth, the &block
   name of the scope it is written in, its break scope, its self, the
   forwarded proc and the lowered context its own yields bind. `up` is the
   entry of its target, and g_ytgt_cur the entry of g_block_id while it
   names one. With them emit_block_invoke moves the whole context out a
   level, where the fallback globals record one level only: a block yielding
   on from three inlined methods deep (`run3(x) { |w| ... }` over
   `def run3(x) = run2(x) { |v| yield v }`) found no block two splices out
   and raised LocalJumpError. */
typedef struct {
  int blk, target, up;
  const char *owner;
  int nren;
  const char *brk; int brk_ebase;
  const char *self, *self_deref; int emcls;
  const char *ypr; TyKind yslot;
  int lowered; const char *lowered_blk;
} YieldTarget;
static YieldTarget g_ytgt[SP_INLINE_DEPTH_MAX + 1];
static int g_nytgt;
static int g_ytgt_cur = -1;
/* The block `k` levels out from g_block_id along the yield targets (0 is
   g_block_id, 1 its fallback), or -1 where none is recorded: what a chain of
   blocks whose tails are yields answers at this site. */
int yield_block_out(int k) {
  if (k <= 0) return g_block_id;
  if (k == 1) return g_yield_block_fallback;
  int e = (g_ytgt_cur >= 0 && g_ytgt[g_ytgt_cur].blk == g_block_id) ? g_ytgt[g_ytgt_cur].up : -1;
  if (e < 0 || g_ytgt[e].blk != g_yield_block_fallback) return -1;
  for (int i = 1; i < k && e >= 0; i++) e = g_ytgt[e].up;
  return e >= 0 ? g_ytgt[e].blk : -1;
}
/* The fallback-level globals a splice refills from an entry. */
typedef struct {
  int cur, nren, brk_ebase, emcls, lowered;
  const char *owner, *brk, *self, *self_deref, *ypr, *lowered_blk;
  TyKind yslot;
} YieldTargetSave;
static void yield_target_save(YieldTargetSave *s) {
  s->cur = g_ytgt_cur;
  s->nren = g_yield_block_fallback_nren; s->owner = g_yield_block_fallback_param_name;
  s->brk = g_yield_blk_brk_fallback; s->brk_ebase = g_yield_blk_brk_efallback;
  s->self = g_yield_self_fallback2; s->self_deref = g_yield_self_deref_fallback2;
  s->emcls = g_yield_emitting_class_fallback2;
  s->ypr = g_yield_proc_ref_fallback2; s->yslot = g_yield_slot_ty_fallback2;
  s->lowered = g_yield_lowered_fallback; s->lowered_blk = g_yield_lowered_blk_fallback;
}
static void yield_target_restore(const YieldTargetSave *s) {
  g_ytgt_cur = s->cur;
  g_yield_block_fallback_nren = s->nren; g_yield_block_fallback_param_name = s->owner;
  g_yield_blk_brk_fallback = s->brk; g_yield_blk_brk_efallback = s->brk_ebase;
  g_yield_self_fallback2 = s->self; g_yield_self_deref_fallback2 = s->self_deref;
  g_yield_emitting_class_fallback2 = s->emcls;
  g_yield_proc_ref_fallback2 = s->ypr; g_yield_slot_ty_fallback2 = s->yslot;
  g_yield_lowered_fallback = s->lowered; g_yield_lowered_blk_fallback = s->lowered_blk;
}
/* Entry `fe` is the block a splice makes current: its own yields bind its
   lowered context (the spliced block's was what the fallback held), and
   its target, when recorded, becomes the fallback with that target's own
   context, one level further out. */
static void yield_target_enter(int fe) {
  const YieldTarget *f = &g_ytgt[fe];
  g_yield_lowered_fallback = f->lowered; g_yield_lowered_blk_fallback = f->lowered_blk;
  if (f->up < 0) return;
  const YieldTarget *t = &g_ytgt[f->up];
  g_yield_block_fallback = t->blk;
  g_yield_block_fallback_nren = t->nren; g_yield_block_fallback_param_name = t->owner;
  g_yield_blk_brk_fallback = t->brk; g_yield_blk_brk_efallback = t->brk_ebase;
  g_yield_self_fallback2 = t->self; g_yield_self_deref_fallback2 = t->self_deref;
  g_yield_emitting_class_fallback2 = t->emcls;
  g_yield_proc_ref_fallback2 = t->ypr; g_yield_slot_ty_fallback2 = t->yslot;
}
static int yield_target_of(int blk, const char **owner) {
  for (int i = g_nytgt - 1; i >= 0; i--)
    if (g_ytgt[i].blk == blk) { *owner = g_ytgt[i].owner; return g_ytgt[i].target; }
  return -1;
}
/* Does the subtree under `id` yield a plain read of `name` at a position
   whose parameter of block `target` wants the alias? A `blk.call(...)` on
   the &block parameter of the method the block is written in (`owner`)
   splices as that yield does. */
static int block_param_wants_alias_at(Compiler *c, int blk, int k, int n, int depth);
static int subtree_yields_local_to_alias(Compiler *c, int id, const char *name, int target,
                                         const char *owner, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return 0;
  int yields = k == NK_YieldNode;
  if (!yields && k == NK_CallNode && owner) {
    int r = nt_ref(nt, id, "receiver");
    const char *cn = nt_str(nt, id, "name");
    yields = r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode && cn && sp_streq(cn, "call") &&
             nt_str(nt, r, "name") && sp_streq(nt_str(nt, r, "name"), owner);
  }
  if (yields) {
    int aa = nt_ref(nt, id, "arguments"); int an = 0;
    const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      /* yielded on by keyword (`yield(k: u)`) to the target's keyword */
      if (nt_kind(nt, av[j]) == NK_KeywordHashNode) {
        int en = 0; const int *el = nt_arr(nt, av[j], "elements", &en);
        for (int e = 0; e < en; e++) {
          int v;
          const char *key = dyn_kw_elem_key(c, el[e], &v);
          const char *kp = key ? block_kw_param_named(c, target, key) : NULL;
          if (kp && v >= 0 && nt_kind(nt, v) == NK_LocalVariableReadNode && nt_str(nt, v, "name") &&
              sp_streq(nt_str(nt, v, "name"), name) && block_local_wants_alias_at(c, target, kp, depth + 1))
            return 1;
        }
        continue;
      }
      if (nt_kind(nt, av[j]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[j], "name");
      if (vn && sp_streq(vn, name) && block_param_wants_alias_at(c, target, j, call_plain_argc(c, id), depth + 1)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_yields_local_to_alias(c, nt_ref_at(nt, id, i), name, target, owner, depth)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *a = nt_arr_at(nt, id, i, &n);
    for (int q = 0; q < n; q++)
      if (subtree_yields_local_to_alias(c, a[q], name, target, owner, depth)) return 1;
  }
  return 0;
}
/* A block parameter of `blk` that the block's body mutates in place, or
   hands to a method that appends to it, and that no write site in its scope
   rebinds: the yield it is bound from has to lend the yielded variable
   itself (see emit_block_invoke's alias binding), or the append lands in
   the parameter's copy and the yielded string never sees it (`fill(buf) {
   |s| s << "z" }` left buf empty, and so did `{ |s| grow(s) }`). */
int block_param_wants_alias(Compiler *c, int blk, int k, int n) {
  return block_param_wants_alias_at(c, blk, k, n, 0);
}
static int block_param_wants_alias_at(Compiler *c, int blk, int k, int n, int depth) {
  if (blk < 0 || depth > SP_INLINE_DEPTH_MAX) return 0;
  /* a post the position binds of a yield of n plain arguments too
     (emit_block_binds aliases it) */
  const char *bp = block_param_at(c, blk, k, n);
  return bp && block_local_wants_alias_at(c, blk, bp, depth);
}
/* The same for any of the block's own parameters by name, a keyword one
   (`|k:|`) included. */
static int block_local_wants_alias_at(Compiler *c, int blk, const char *bp, int depth) {
  const NodeTable *nt = c->nt;
  Scope *bs = comp_scope_of(c, blk);
  LocalVar *lv = bs ? scope_local(bs, bp) : NULL;
  if (!lv || lv->type != TY_STRING || (lv->is_cell && !lv->inline_alias)) return 0;
  /* Mutated in place, or yielded on to a block that is: `run(x) { |u|
     yield u }` inside a method whose own block appends hands that block
     the parameter, which is a copy unless it aliases the yielded variable
     in turn (each level's copy kept the append from the one before). */
  if (!subtree_str_mutates_local(nt, nt_ref(nt, blk, "body"), bp) &&
      !block_param_handed_to_appender(c, blk, bp)) {
    const char *owner = NULL;
    int tgt = yield_target_of(blk, &owner);
    if (tgt < 0 || !subtree_yields_local_to_alias(c, nt_ref(nt, blk, "body"), bp, tgt, owner, depth))
      return 0;
  }
  for (int w = 0; w < nt->count; w++) {
    NodeKind wk = nt_kind(nt, w);
    if (wk != NK_LocalVariableWriteNode && wk != NK_LocalVariableOperatorWriteNode &&
        wk != NK_LocalVariableOrWriteNode && wk != NK_LocalVariableAndWriteNode &&
        wk != NK_LocalVariableTargetNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (wn && sp_streq(wn, bp) && comp_scope_of(c, w) == bs) return 0;
  }
  return 1;
}
/* Does scope mi yield `name` to a block parameter the call site's block
   (`blk`) mutates in place? Then the parameter is mutated through the block,
   and has to be an alias of the caller's variable like a directly mutated
   one (inline_param_mutated). */
static int inline_param_yielded_mutated(Compiler *c, int mi, const char *name, int blk) {
  const NodeTable *nt = c->nt;
  if (blk < 0) return 0;
  const char *bpn = c->scopes[mi].blk_param;
  for (int q = 0; q < nt->count; q++) {
    if (c->nscope[q] != mi) continue;
    /* `blk.call(...)` on the method's own &block splices as a yield does */
    int r = nt_kind(nt, q) == NK_CallNode ? nt_ref(nt, q, "receiver") : -1;
    int blk_call = r >= 0 && bpn && nt_kind(nt, r) == NK_LocalVariableReadNode &&
                   nt_str(nt, r, "name") && sp_streq(nt_str(nt, r, "name"), bpn) &&
                   nt_str(nt, q, "name") && sp_streq(nt_str(nt, q, "name"), "call");
    if (nt_kind(nt, q) != NK_YieldNode && !blk_call) continue;
    int aa = nt_ref(nt, q, "arguments"); int an = 0;
    const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++) {
      /* a keyword (`yield(k: name)`) binds the block's keyword of that name */
      if (nt_kind(nt, av[k]) == NK_KeywordHashNode) {
        int en = 0; const int *el = nt_arr(nt, av[k], "elements", &en);
        for (int e = 0; e < en; e++) {
          int key = nt_kind(nt, el[e]) == NK_AssocNode ? nt_ref(nt, el[e], "key") : -1;
          int v = key >= 0 ? nt_ref(nt, el[e], "value") : -1;
          if (key < 0 || nt_kind(nt, key) != NK_SymbolNode || v < 0 ||
              nt_kind(nt, v) != NK_LocalVariableReadNode || !nt_str(nt, v, "name") ||
              !sp_streq(nt_str(nt, v, "name"), name)) continue;
          const char *kp = block_kw_param_named(c, blk, nt_str(nt, key, "value"));
          if (kp && block_local_wants_alias(c, blk, kp)) return 1;
        }
        continue;
      }
      if (nt_kind(nt, av[k]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[k], "name");
      if (vn && sp_streq(vn, name) && block_param_wants_alias(c, blk, k, call_plain_argc(c, q))) return 1;
    }
  }
  return 0;
}

/* Does a `name` call to yielding method `mi` on a `disp_cls` receiver go
   through the cls_id switch rather than being spliced? */
static int takes_class_dispatch(Compiler *c, int mi, int disp_cls, const char *name) {
  return disp_cls >= 0 && !c->scopes[mi].is_cmethod && scope_proc_form_of(c, mi) >= 0 &&
         dispatch_impl_count(c, disp_cls, name) > 1;
}

/* A block-driving call emit_inline_call_x leaves to the cls_id switch: the
   plain call it falls back to calls the proc-form clones, so it is no call to
   refuse. */
int block_call_takes_class_dispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int cls = -1;
  if (recv < 0) {
    Scope *encl = comp_scope_of(c, id);
    if (!encl || encl->class_id < 0 || encl->is_cmethod) return 0;
    cls = encl->class_id;
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_object(rt)) return 0;
    cls = ty_object_class(rt);
  }
  int mi = comp_method_in_chain(c, cls, name, NULL);
  return mi >= 0 && c->scopes[mi].yields && takes_class_dispatch(c, mi, cls, name);
}

/* Is `a` a read of a local that is the shared String handle? */
int local_is_handle(Compiler *c, int a) {
  if (a < 0 || nt_kind(c->nt, a) != NK_LocalVariableReadNode) return 0;
  const char *vn = nt_str(c->nt, a, "name");
  Scope *vs = vn ? comp_scope_of(c, a) : NULL;
  LocalVar *lv = vs ? scope_local(vs, vn) : NULL;
  return lv && lv->type == TY_STRBUF && lv->str_shared;
}
/* A local that is the shared handle, as a reference to the handle itself
   rather than the copy a plain read of it takes. 0 when it is none. */
int emit_handle_var_ref(Compiler *c, int a, Buf *b) {
  if (!local_is_handle(c, a)) return 0;
  /* a value that ran first, ahead of a later one that rebinds the local
     (`yield(s, (s = +"q"; 1))`), is the handle it read then: the slot holds
     the new String by now (ran_first_handle, as emit_boxed asks it) */
  int th = ran_first_handle(a);
  if (th >= 0) { buf_printf(b, "_t%d", th); return 1; }
  char ref[1024];
  int svm = view_push_repr(c, a, VR_STRBUF_BOX, 1);
  int ok = strbuf_slot_ref(c, a, ref, sizeof ref);
  view_pop(c, svm);
  if (!ok) return 0;
  buf_puts(b, ref);
  return 1;
}
/* Which parameters of inlined method `mi` bind as ALIASES of the caller's
   variables (see inline_param_mutated above): a String the body mutates,
   hands on, or yields to a block parameter `blk` mutates, passed as a plain
   local read (or an instance method's ivar), not rebound by the body, and
   not celled for a capture of its own. Each is marked an alias until
   inline_alias_release. Decided before the locals are declared, since an
   aliased parameter gets no local of its own. */
unsigned inline_alias_params(Compiler *c, int mi, const int *argv, int pargc, const ArgLayout *L, int blk) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  unsigned alias_mask = 0;
  for (int i = 0; i < m->nparams && i < 32 && !L->gather; i++) {
    if (L->from[i] != ARG_NODE || L->arg[i] >= pargc || (m->rest_idx >= 0 && i >= m->rest_idx)) continue;
    int an = argv[L->arg[i]];
    NodeKind ak = nt_kind(nt, an);
    /* the C global lent, when the argument is one (refuse_lent_global_rebound) */
    char gref[256];
    int gslot = 0;
    if (ak == NK_InstanceVariableReadNode) {
      /* an ivar buffer: the object's own slot, from an instance method of a
         heap class (a value type is a struct copy with no slot to lend), or
         the C global a top-level or class method's ivar lives in */
      Scope *as = comp_scope_of(c, an);
      if (comp_ntype(c, an) != TY_STRING) continue;
      gslot = ivar_global_slot(c, an, gref, sizeof gref);
      if (!gslot &&
          (!as || as->class_id < 0 || as->is_cmethod ||
           comp_ty_value_obj(c, ty_object(as->class_id)) || !g_self)) continue;
    }
    /* a global variable's C global (gv_), and a class variable's, as a call
       lends it */
    else if (ak == NK_GlobalVariableReadNode || ak == NK_ClassVariableReadNode) {
      if (comp_ntype(c, an) != TY_STRING ||
          !(ak == NK_GlobalVariableReadNode ? gvar_global_slot(c, an, gref, sizeof gref)
                                            : cvar_global_slot(c, an, gref, sizeof gref))) continue;
      gslot = 1;
    }
    /* a String that is the shared handle has no slot to lend: the
       parameter takes the handle (yield_splice_handles) or a copy */
    else if (ak != NK_LocalVariableReadNode || local_is_handle(c, an)) continue;
    /* a variable that can hold other values (`u = nil`) is an sp_RbVal,
       no `const char *` slot to lend either */
    else if (comp_ntype(c, an) != TY_STRING) continue;
    LocalVar *lv = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
    if (!lv || !lv->is_param || lv->is_block_param || lv->type != TY_STRING) continue;
    if (lv->is_cell && !lv->inline_alias) continue;
    if (inline_param_rebound(c, mi, m->pnames[i]) == 2 ||
        (!inline_param_mutated(c, mi, m->pnames[i]) &&
         !inline_param_yielded_mutated(c, mi, m->pnames[i], blk))) continue;
    if (gslot) refuse_lent_global_rebound(c, an, gref, m->name, m->pnames[i]);
    alias_mask |= 1u << i;
    lv->inline_alias++;
    lv->is_cell = 1;
  }
  return alias_mask;
}
void inline_alias_release(Scope *m, unsigned alias_mask) {
  for (int i = 0; i < m->nparams && i < 32; i++) {
    if (!(alias_mask & (1u << i))) continue;
    LocalVar *lv = scope_local(m, m->pnames[i]);
    if (lv && --lv->inline_alias == 0) lv->is_cell = 0;
  }
}
/* Declare inlined method `mi`'s locals under renamed names, all but the
   aliased parameters: the alias is declared at the binding, and a
   parameter the body also rebinds gets the private local the rebind
   repoints the cell at. */
void emit_inline_locals_aliased(Compiler *c, int mi, int tag, unsigned alias_mask, Buf *b, int din) {
  Scope *m = &c->scopes[mi];
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;  /* virtual &block slot */
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    if (lv->is_param && lv->inline_alias) {
      int pi = -1;
      for (int k = 0; k < m->nparams; k++) if (m->pnames[k] && sp_streq(m->pnames[k], lv->name)) { pi = k; break; }
      if (pi >= 0 && (alias_mask & (1u << pi))) {
        if (inline_param_rebound(c, mi, lv->name) == 1) {
          emit_indent(b, din);
          buf_printf(b, "const char *lv_%s = NULL; SP_GC_ROOT_STR(lv_%s);\n", rn, rn);
        }
        continue;
      }
    }
    emit_inlined_local_decl(c, lv, rn, b, din);
  }
}
/* The alias an aliased parameter binds: the caller's variable's own slot. */
void emit_inline_alias_arg(Compiler *c, int av, Buf *b) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, av) == NK_InstanceVariableReadNode) {
    /* the slot itself, and the owner pinned as a byref call pins it: the
       store lands inside this expansion, past any dirty bit (#4378) */
    const char *ivn = nt_str(nt, av, "name");
    /* a C global is a root every collection marks: no owner to pin */
    char gref[256];
    if (ivar_global_slot(c, av, gref, sizeof gref)) { buf_printf(b, "%s)", gref); return; }
    buf_printf(b, "%s%siv_%s); sp_gc_pin_remembered((void *)%s)", g_self, g_self_deref, iv_c(ivn + 1), g_self);
    return;
  }
  emit_expr(c, av, b); buf_puts(b, ")");
  /* The caller's variable may be a heap cell (captured by a proc): the
     body will store through it from inside this expansion, which is the
     placement a dirty bit cannot cover, so pin the cell as a byref call
     would (#4391); a stack slot, or a cell the caller itself was lent,
     is not ours to pin. */
  const char *avn = nt_str(nt, av, "name");
  LocalVar *alv = avn ? scope_local(comp_scope_of(c, av), avn) : NULL;
  if (alv && alv->is_cell && !alv->byref_out && !alv->inline_alias &&
      !(g_cap_struct && g_cap_names && nameset_has(g_cap_names, avn)))
    buf_printf(b, "; sp_gc_pin_remembered((void *)_cell_%s)", rename_local(avn));
}

/* Bind an inlined yielding method's parameters from a call's arguments:
   a splat spread at run time, a keyword hash by name, a rest and its posts
   packed, as the ordinary call paths bind them. The expansion's renames
   above saved_nren are hidden while argument code is emitted. */
void emit_inline_bind_params(Compiler *c, Scope *m, int args, const int *argv, int argc,
                             const ArgLayout *L, unsigned alias_mask, int tag, int saved_nren,
                             int din, Buf *b) {
  const NodeTable *nt = c->nt;
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
  /* The `**` operands, the positionals evaluated ahead of them and a splat
     spread below are call-site code, like each argument the loop binds:
     this inline's renames are off for them, and an inlined call inside any
     of them pushes its own at this depth, so the entries are parked across
     them all. A `**h` read under them named the callee's own `h` if it had
     one, a renamed local nothing declares here. */
  RenPark park0 = ren_park(saved_nren);
  /* A `**hash` inside the keyword-hash arg (`m(**h)`) carries no literal keys,
     so keyword params bind from a runtime lookup on the materialized hash, the
     same way emit_dispatch/emit_args_filled do -- without this each keyword
     param fell through to a fabricated default. */
  int kw_merged = kwh_merged(c, m, kwh);
  int argov_saved = g_n_argov;
  if (L->kw.args_first || kwh_runs_ahead(c, m, kwh)) emit_args_run(c, argv, argc);
  else emit_args_before_binding(c, m, argv, argc, g_pre);
  TyKind ds_type = TY_UNKNOWN;
  int ds_tmp = emit_ds_hash_materialize(c, m, kwh, &ds_type);
  /* The count and the keys, by the rule the ordinary call path follows. The
     loop below walks the PARAMETERS, so an argument none of them reads --
     a key naming no parameter (#4419), a positional past the last one --
     was simply dropped, and a missing one bound its zero value: `y1 { }` on
     `def y1(x)` ran with x padded, `y(1, 2) { }` on `def y(x, k: 1)`
     dropped the 2. A `...` forward carries the forwarder's own params. */
  if (fwd_encl) emit_unknown_kwarg_raise(c, m, argv, argc);
  else emit_call_arity_check(c, m, argc, argv);
  /* The options-hash idiom: a braceless keyword hash no keyword parameter
     claims packs into the first unfilled positional (`def check(sel, opts =
     nil)` called `check(".x", count: 0)`). The other two call paths have done
     this since #3191; this one looked the keys up by parameter NAME only, so
     `opts` kept its default and every `assert_select(sel, count: 0)` in a
     yielding helper asserted presence instead (#4436). */
  int gather_tmp = -1;
  if (L->gather && !fwd_encl) gather_tmp = emit_splat_gather(c, m, argv, L);
  /* the keywords a `**` brings, once the count has been judged, which
     CRuby judges first */
  emit_ds_kwarg_check(c, m, kwh, ds_tmp, ds_type);
  ren_unpark(&park0);
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    int aliased = i < 32 && (alias_mask & (1u << i));
    if (aliased) buf_printf(b, "const char **_cell__y%d_%s = &(", tag, m->pnames[i]);
    else { char rn[128]; snprintf(rn, sizeof rn, "_y%d_%s", tag, m->pnames[i]);
      emit_inlined_param_target(c, m, m->pnames[i], rn, b); }
    /* hide THIS inline's renames only: args are call-site expressions,
       and the call site may itself be an outer inlined body whose locals
       are renamed (nested yield-method inlines) -- zeroing the whole
       table emitted the unrenamed lv_<name> (undeclared identifier, or a
       silent capture of a same-named caller local). */
    /* The argument expression is call-site code, so the callee's renames are
       switched off for it. A nested inline INSIDE that expression pushes its
       own entries at this very depth and overwrites the callee's, so restoring
       the count alone brought back another method's names -- this body then
       emitted the unrenamed `lv_<name>` for whatever had been clobbered, which
       nothing declares (#3943). Park the entries across the argument, not just
       the count. */
    RenPark park = ren_park(saved_nren);
    if (fwd_encl && i < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[i]);
      LocalVar *mp = scope_local(m, m->pnames[i]);
      TyKind et = ep ? ep->type : TY_POLY;
      TyKind mt = mp ? mp->type : TY_POLY;
      char txt[128]; snprintf(txt, sizeof txt, "lv_%s", rename_local(fwd_encl->pnames[i]));
      if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, b);
      else buf_puts(b, txt);
    }
    else if (gather_tmp >= 0 && L->from[i] == ARG_GATHERED)
      emit_gathered_param(c, m, i, gather_tmp, b);
    /* A rest param collects the middle arguments into an Array. Without this
       the first argument was assigned straight into the rest slot -- a
       pointer of the wrong type, so the rest read back empty (or crashed).
       A keyword hash no parameter takes is its last element, as on the other
       call paths; it was dropped here, `rs(a: 1) { }` binding `[]`. */
    else if (L->from[i] == ARG_REST)
      emit_rest_pack_kwh(c, i, L->rest_argc - m->npost_rest, argv, L->rest_kwh, b);
    /* a post, from the end of the call's arguments */
    else if (m->rest_idx >= 0 && i > m->rest_idx && i <= m->rest_idx + m->npost_rest)
      emit_arg_or_default(c, m, i, L->from[i] == ARG_NODE ? argv[L->arg[i]] : -1, b);
    /* Anything past the rest that is not one of its posts is a keyword (or
       **kwrest) param: it binds by name, never positionally. */
    else if (aliased) emit_inline_alias_arg(c, argv[L->arg[i]], b);
    /* a **kwrest collects the keywords no declared keyword param takes, as
       on the other call paths; it bound its nil default here */
    else if (i == m->kwrest_idx) {
      int krhash = emit_kwrest_collect(c, m, kwh, ds_tmp, ds_type, args);
      LocalVar *krp = scope_local(m, m->pnames[i]);
      if (krp && krp->type == TY_POLY) buf_printf(b, "sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH)", krhash);
      else buf_printf(b, "_t%d", krhash);
    }
    /* a keyword or **kwrest param never takes a positional: a surplus one
       (refused above) bound `ykw(1, 2)`'s 2 into the kwrest's hash slot, a
       C type error */
    else if (L->from[i] == ARG_NODE)
      emit_arg_or_default(c, m, i, argv[L->arg[i]], b);
    else if (L->from[i] == ARG_KWH)
      emit_arg_or_default(c, m, i, kwh, b);
    else {
      int kv = kwh >= 0 && !kw_merged ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      /* No literal key for this keyword param, but a `**hash` was splatted:
         extract it by name from the materialized hash (falls back to the
         param default when the key is absent). */
      if (kv < 0 && ds_tmp >= 0 && callee_has_kwarg(c, m, m->pnames[i]))
        emit_ds_param_extract(c, m, i, ds_tmp, ds_type, b);
      else
        emit_arg_or_default(c, m, i, kv, b);
    }
    ren_unpark(&park);
    buf_puts(b, ";\n");
  }
  view_unbind(argov_saved);
}

/* The method a call with a block is spliced from, and the self it binds: the
   inline's own lookup, which the plan stands in for (inline_target_plan) and
   --plan-check keeps as the assertion. */
typedef struct {
  int mi;
  int recv_class;   /* the instance self is bound to */
  /* the class a CLASS METHOD is inlined for: no instance self to bind (which
     is what recv_class drives), but its body's bare `new` must still build
     this class rather than the host method's */
  int cm_class, cm_self_id;
  int implicit_self;
} InlineTarget;

static void inline_target_lookup(Compiler *c, int id, const char *name, int recv, InlineTarget *t) {
  const NodeTable *nt = c->nt;
  t->mi = -1; t->recv_class = -1; t->cm_class = -1; t->cm_self_id = 0; t->implicit_self = 0;
  if (recv < 0) {
    /* A bare call resolves to self first, as Ruby does and as the analyzer
       does (comp_self_call_mi): a top-level `def request` beside a class's
       own `request` took the free function here, the inline declined on its
       arity, and the fallback called a symbol the self method never had
       because every other site inlined it (#4500). */
    Scope *encl = comp_scope_of(c, id);
    /* inside an instance_eval/exec splice self is the rebound receiver */
    if (g_ie_class_id >= 0 && (t->mi = comp_method_in_chain(c, g_ie_class_id, name, NULL)) >= 0)
      t->recv_class = g_ie_class_id;
    else if (encl && encl->class_id >= 0) {
      if (encl->is_cmethod) t->mi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      if (t->mi < 0) {
        t->mi = comp_method_in_chain(c, encl->class_id, name, NULL);
        if (t->mi >= 0) t->implicit_self = 1;
      }
    }
    else if (g_class_body_id >= 0) {
      t->mi = comp_cmethod_in_chain(c, g_class_body_id, name, NULL);
      if (t->mi >= 0) t->cm_class = g_class_body_id;
    }
    if (t->mi < 0) t->mi = comp_method_index(c, name);   /* free function */
    /* A method of a module included at the top level is callable bare, like
       a free function. A yielding one exists only inlined, so a call left to
       the top-level-include arm in emit_call named a function that was never
       emitted and the link failed. A module_function method is a class
       method here, and it runs with the module as self, as it does when
       called through the module. One that touches an instance variable is
       left to that arm, which refuses it: main holds no module state. */
    if (t->mi < 0) {
      int imi = comp_included_method_index(c, name, id);
      if (imi >= 0 && !scope_uses_ivars(c, imi)) {
        t->mi = imi;
        if (c->scopes[imi].is_cmethod) t->cm_class = c->scopes[imi].class_id;
      }
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
    int ci = cname ? comp_class_index(c, cname) : self_class_static_ci(c, recv);
    if ((t->mi = class_reopen_cmethod(c, recv, name)) >= 0) {
      t->cm_class = c->scopes[t->mi].class_id;
      t->cm_self_id = builtin_class_id(cname);
    }
    else if (ci >= 0) {
      /* Cls.method with a yield block: look up as a class method */
      t->mi = comp_cmethod_in_chain(c, ci, name, NULL);
      t->cm_class = ci;
    }
    else if (ty_is_object(rt)) {
      /* An instance receiver -- including a constant that holds an instance
         (e.g. `S = Set.new(...); S.each { }`), which is not a class name so
         falls through here rather than the class-method lookup above. */
      t->recv_class = ty_object_class(rt);
      t->mi = comp_method_in_chain(c, t->recv_class, name, NULL);
    }
    else if (g_inline_recv_expr && g_inline_recv_class >= 0) {
      /* poly-receiver dispatch arm (#2448): self is pre-bound to a cast of the
         boxed receiver, and the concrete class is supplied out of band */
      t->recv_class = g_inline_recv_class;
      t->mi = comp_method_in_chain(c, t->recv_class, name, NULL);
    }
  }
}

/* The same from the call's plan (call_plan.c), for the plans whose form says
   the binding: a bare call on the enclosing class's chain, its class methods
   in a class method, a class body's class method, a top-level method or an
   included module's; a class constant's class method (a Class reopen's for a
   builtin class); an instance receiver's own lookup. An instance_exec self
   and a poly arm's receiver class are read into the plan (cplan_user_in).
   0 for a plan of another kind, which keeps the lookup. `narrow` is
   call_targets_yielding_method's shorter echo: a constant receiver only, and
   no Class reopen. */
static int inline_target_plan(Compiler *c, int id, const char *name, int recv, int narrow,
                              InlineTarget *t) {
  const NodeTable *nt = c->nt;
  t->mi = -1; t->recv_class = -1; t->cm_class = -1; t->cm_self_id = 0; t->implicit_self = 0;
  const CallPlan *p;
  if (recv < 0 && g_ie_class_id >= 0) {
    /* inside an instance_eval/exec splice self is the rebound receiver: its
       class's lookup first, then the call as its own */
    p = cplan_user_in(c, id, g_ie_class_id, CPX_IE);
    if (p->chain && p->via == UC_INST && p->owner_ci == g_ie_class_id) {
      t->mi = p->mi; t->recv_class = g_ie_class_id;
      return 1;
    }
  }
  else if (recv >= 0 && !narrow && g_inline_recv_expr && g_inline_recv_class >= 0 &&
           !ty_is_object(comp_ntype(c, recv)) && self_class_static_ci(c, recv) < 0 &&
           !(nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode)) {
    /* a poly arm (#2448): self is pre-bound to a cast of the boxed receiver,
       and the concrete class is supplied out of band */
    CallPlan q = *cplan_user_in(c, id, g_inline_recv_class, CPX_ARM);
    t->recv_class = g_inline_recv_class;
    if (q.chain && q.via == UC_INST && q.owner_ci == g_inline_recv_class) t->mi = q.mi;
    return 1;
  }
  else p = cplan_user(c, id);
  if (p->mi < 0) return 0;
  if (recv < 0) {
    Scope *encl = comp_scope_of(c, id);
    int ecls = encl ? encl->class_id : -1;
    /* a class body's own class methods come first (codegen's
       g_class_body_id); the plan asked them first too when the call is that
       body's (comp_cbody_call_mi) */
    int top_ok = ecls >= 0 || g_class_body_id < 0 ||
                 (c->node_cbody[id] == g_class_body_id && !(encl && encl->name));
    if (ecls >= 0 && p->via == UC_INST && p->chain && p->owner_ci == ecls) {
      t->mi = p->mi; t->implicit_self = 1;
      return 1;
    }
    if (ecls >= 0 && encl->is_cmethod && p->via == UC_CMETH && p->owner_ci == ecls &&
        c->scopes[p->mi].is_cmethod) {
      t->mi = p->mi;
      return 1;
    }
    if (ecls < 0 && g_class_body_id >= 0 && p->via == UC_CMETH && p->owner_ci == g_class_body_id &&
        c->node_cbody[id] == g_class_body_id) {
      t->mi = p->mi; t->cm_class = g_class_body_id;
      return 1;
    }
    if (top_ok && p->via == UC_TOP) { t->mi = p->mi; return 1; }
    if (top_ok && p->via == UC_INCLUDED) {
      if (!scope_uses_ivars(c, p->mi)) {
        t->mi = p->mi;
        if (c->scopes[p->mi].is_cmethod) t->cm_class = c->scopes[p->mi].class_id;
      }
      return 1;
    }
    return 0;
  }
  const char *rty = nt_type(nt, recv);
  const char *cname = (rty && (sp_streq(rty, "ConstantReadNode") ||
                               sp_streq(rty, "ConstantPathNode")))
                      ? nt_str(nt, recv, "name") : NULL;
  if (narrow && !cname) {
    TyKind rt = comp_ntype(c, recv);
    if (p->via == UC_INST && p->chain && ty_is_object(rt) && p->owner_ci == ty_object_class(rt)) {
      t->mi = p->mi; t->recv_class = p->owner_ci;
      return 1;
    }
    return 0;
  }
  int ci = cname ? comp_class_index(c, cname) : self_class_static_ci(c, recv);
  if (p->via == UC_CMETH && !p->by_name && (cname || ci >= 0)) {
    if (ci >= 0 && p->owner_ci == ci) { t->mi = p->mi; t->cm_class = ci; return 1; }
    if (!narrow && cname && builtin_class_id(cname) && p->owner_ci == c->scopes[p->mi].class_id &&
        p->owner_ci != ci) {
      t->mi = p->mi; t->cm_class = p->owner_ci; t->cm_self_id = builtin_class_id(cname);
      return 1;
    }
    return 0;
  }
  if (ci < 0 && p->via == UC_INST && p->chain) {
    TyKind rt = comp_ntype(c, recv);
    if (ty_is_object(rt) && p->owner_ci == ty_object_class(rt)) {
      t->mi = p->mi; t->recv_class = p->owner_ci;
      return 1;
    }
  }
  return 0;
}

static int inline_target_same(const InlineTarget *a, const InlineTarget *b) {
  return a->mi == b->mi && a->recv_class == b->recv_class && a->cm_class == b->cm_class &&
         a->cm_self_id == b->cm_self_id && a->implicit_self == b->implicit_self;
}

int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  if (g_plan_check) ucall_emitted(id);
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  /* the target is the call's plan where its form says the binding;
     otherwise, and under --plan-check as the assertion, the lookup */
  InlineTarget it;
  int served = inline_target_plan(c, id, name, recv, 0, &it);
  if (g_plan_check && served) cplan_served("inline-call");
  if (g_plan_check || !served) {
    InlineTarget ot;
    inline_target_lookup(c, id, name, recv, &ot);
    if (!served) {
      if (g_plan_check && ot.mi >= 0)
        fprintf(stderr, "plan-check: cplan-fallback: inline-call node %d %s%s\n", id, name,
                g_ie_class_id >= 0 ? " (instance_exec self)" : "");
      it = ot;
    }
    else if (!inline_target_same(&it, &ot))
      fprintf(stderr, "plan-check: cplan-conflict: inline-call node %d %s: plan %d/%d/%d/%d/%d, "
              "lookup %d/%d/%d/%d/%d\n", id, name, it.mi, it.recv_class, it.cm_class,
              it.cm_self_id, it.implicit_self, ot.mi, ot.recv_class, ot.cm_class, ot.cm_self_id,
              ot.implicit_self);
  }
  int mi = it.mi, recv_class = it.recv_class, cm_class = it.cm_class;
  int cm_self_id = it.cm_self_id, implicit_self = it.implicit_self;
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
  /* A subclass overriding the method takes its own arm of the cls_id switch;
     splicing this body answered for every class with the base's method. The
     switch reaches this one through its proc-form clone. */
  if (takes_class_dispatch(c, mi, implicit_self ? comp_scope_of(c, id)->class_id : recv_class,
                           name))
    return 0;
  /* a private or protected method refuses an outside receiver before its body
     is spliced in, as the plain call does */
  {
    Buf vb; memset(&vb, 0, sizeof vb);
    if (emit_vis_refusal(c, id, &vb)) {
      if (as_expr) buf_puts(b, vb.p);
      else { emit_indent(b, indent); buf_puts(b, vb.p); buf_puts(b, ";\n"); }
      free(vb.p);
      return 1;
    }
    free(vb.p);
  }
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
  int fwd_value = -1;   /* the block argument's value, for g_yield_proc_expr */
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
    /* Only an anonymous `&` or the enclosing method's own block parameter
       forwards the block this site runs under. Any other `&x` is a value --
       a proc read out of a list, a local -- even inside an inline that has a
       literal block: taken as a forward, `measure(&item)` inside Benchmark's
       bmbm spliced the user's block given to bmbm and ran it with no
       argument. */
    int fwd_encl = fexpr < 0 || (pn && g_block_param_name && sp_streq(pn, g_block_param_name));
    int no_encl = g_block_id < 0 || !fwd_encl;
    /* `def outer(&b); inner(&b); end`: the name is the ENCLOSING inlined
       method's own block parameter, which has no local of its own -- the
       inliner skips it as a virtual slot. It is the proc reference this
       inline is already running under, and reading it as a local named an
       undeclared `lv_b`. */
    if (g_block_id < 0 && pn && g_yield_proc_ref &&
        g_block_param_name && sp_streq(pn, g_block_param_name)) {
      snprintf(yprocbuf, sizeof yprocbuf, "%s", g_yield_proc_ref);
      fwd_yield_proc = yprocbuf;
      if (g_yield_proc_expr_ref == g_yield_proc_ref) fwd_value = g_yield_proc_expr;
    }
    /* The same forward at a site that has NO block and no proc either: the
       enclosing method was called blockless, so the callee's `yield` has
       nothing to run. Such a site is normally dead (`capture(&__blk) unless
       __blk.nil?`) but is emitted all the same, and reading the virtual
       parameter as a local named an undeclared lv_<blk> (#4477). */
    else if (g_block_id < 0 && pn && !g_yield_proc_ref &&
             g_block_param_name && sp_streq(pn, g_block_param_name)) {
      /* nothing to forward: fall through with block = -1 below */
    }
    else if (no_encl && plv && plv->type == TY_PROC && !plv->is_cell) {
      snprintf(yprocbuf, sizeof yprocbuf, "lv_%s", rename_local(pn));
      fwd_yield_proc = yprocbuf;
      fwd_value = fexpr;
    }
    /* Any other first-class callable passed with `&` -- a lambda literal, a
       `method(:m)`, a Proc read out of a container -- is hoisted into a temp
       and driven the same way; without this the splice found no block at all
       and the call answered nil (#3688). */
    else if (no_encl && fexpr >= 0 && !fwd_yield_proc) {
      TyKind fkt = comp_ntype(c, fexpr);
      if (fkt == TY_PROC || fkt == TY_METHOD || fkt == TY_POLY) {
        fwd_proc_expr = fexpr;
        fwd_proc_tmp = ++g_tmp;
        snprintf(yprocbuf, sizeof yprocbuf, "_t%d", fwd_proc_tmp);
        fwd_yield_proc = yprocbuf;
        fwd_value = fexpr;
      }
    }
    /* a proc value drives the yields; the site's own block is not this one */
    block = (fwd_yield_proc && !fwd_encl) ? -1 : g_block_id;
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
  /* --plan-check: the call is spliced from mi (observed at the call's own
     depth, before the body's) */
  if (g_plan_check) ucall_observe(c, id, mi, recv_class >= 0 ? recv_class : cm_class, 0);
  g_inline_depth++;
  int saved_nren = g_nren, saved_block = g_block_id;
  int saved_bnren = g_block_nren, saved_yfbn = g_yield_block_fallback_nren;
  const char *saved_bown = g_block_owner_param_name, *saved_yfbpn = g_yield_block_fallback_param_name;
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
  char cm_selfbuf[32];   /* the class token an inlined class method's self names */
  /* Nested `yield` inside the block body should chain to the block that was
     active before this inline, not to the inner block. A forwarded block
     (`def run2(x, &b) = run(x, &b)`) with a yield-target entry is the block
     current here, and the fallback already names its own target, with the
     proc and lowered context its yields bind: they stay. Set to the block
     itself, its body's `yield` found no block (LocalJumpError). */
  int fwd_kept = block >= 0 && block == saved_block && g_ytgt_cur >= 0 &&
                 g_ytgt[g_ytgt_cur].blk == block;
  if (!fwd_kept) {
    g_yield_block_fallback = saved_block;
    g_yield_block_fallback_nren = saved_bnren;
    g_yield_block_fallback_param_name = saved_bown;
  }
  /* a literal block yields to the block current here (a forwarded one keeps
     the target it was given where it was written) */
  int pushed_ytgt = 0, saved_ytgt_cur = g_ytgt_cur;
  if (block >= 0 && block != saved_block && g_nytgt <= SP_INLINE_DEPTH_MAX) {
    /* its context is this call site's, as the assignments below hand it to
       g_block_id: caller code at the depth before this inline's renames */
    YieldTarget *yt = &g_ytgt[g_nytgt];
    yt->blk = block; yt->target = saved_block;
    yt->up = (g_ytgt_cur >= 0 && g_ytgt[g_ytgt_cur].blk == saved_block) ? g_ytgt_cur : -1;
    yt->owner = saved_bpn;
    yt->nren = saved_nren;
    yt->brk = saved_ser; yt->brk_ebase = saved_ebase;
    yt->self = g_self; yt->self_deref = g_self_deref; yt->emcls = g_emitting_class_id;
    yt->ypr = g_yield_proc_ref; yt->yslot = g_yield_slot_ty;
    yt->lowered = g_current_scope_is_lowered; yt->lowered_blk = g_lowered_blk_name;
    g_ytgt_cur = g_nytgt++; pushed_ytgt = 1;
  }
  else if (block != saved_block) g_ytgt_cur = -1;
  if (!fwd_kept) {
    g_yield_blk_brk_fallback = saved_bbv;
    g_yield_blk_brk_efallback = saved_bbe;
  }
  /* the block being captured is caller code: record the caller's self so
     emit_block_invoke can restore it around the spliced block body. Aliasing
     g_self by pointer is safe now that selfbuf is stack-local: it names an
     ancestor frame's selfbuf, which stays live and unmodified for the whole
     nested emission (a frame only ever writes its own selfbuf). */
  const char *saved_self_fb = g_yield_self_fallback;
  const char *saved_deref_fb = g_yield_self_deref_fallback;
  int saved_emcls_fb = g_yield_emitting_class_fallback;
  const char *saved_self_fb2 = g_yield_self_fallback2;
  const char *saved_deref_fb2 = g_yield_self_deref_fallback2;
  int saved_emcls_fb2 = g_yield_emitting_class_fallback2;
  /* The inlined callee's own yields splice this call site's block; only a
     yield in spliced CALLER code belongs to an enclosing lowered method.
     Park the lowered context for emit_block_invoke and clear it for the
     callee body -- the same discipline as the self/emitting-class pair. */
  int saved_low_fb = g_yield_lowered_fallback;
  const char *saved_lbnf = g_yield_lowered_blk_fallback;
  int saved_low = g_current_scope_is_lowered;
  const char *saved_lbn = g_lowered_blk_name;
  if (!fwd_kept) {
    g_yield_lowered_fallback = g_current_scope_is_lowered;
    g_yield_lowered_blk_fallback = g_lowered_blk_name;
  }
  /* and the enclosing inline's forwarded-proc block: a `yield` in the
     spliced caller code binds that proc, not this inline's block. Without
     it a block `{ |x| yield x }` handed to an inlined callee from a method
     itself called with `&proc` found no block at all. */
  const char *saved_ypr_fb = g_yield_proc_ref_fallback;
  TyKind saved_yslot_fb = g_yield_slot_ty_fallback;
  const char *saved_ypr_fb2 = g_yield_proc_ref_fallback2;
  TyKind saved_yslot_fb2 = g_yield_slot_ty_fallback2;
  if (!fwd_kept) {
    g_yield_proc_ref_fallback = g_yield_proc_ref;
    g_yield_slot_ty_fallback = g_yield_slot_ty;
  }
  g_current_scope_is_lowered = 0;
  g_lowered_blk_name = NULL;
  /* the block that was current keeps its own self one level out; a literal
     block written here runs under the self of this call site */
  if (block != saved_block) {
    g_yield_self_fallback2 = saved_self_fb;
    g_yield_self_deref_fallback2 = saved_deref_fb;
    g_yield_emitting_class_fallback2 = saved_emcls_fb;
    g_yield_self_fallback = g_self;
    g_yield_self_deref_fallback = g_self_deref;
    /* captured here, BEFORE the receiver-context switch below, so it holds the
       caller's class for the spliced (caller-code) block body */
    g_yield_emitting_class_fallback = g_emitting_class_id;
    /* the forwarded proc one level out moves out with it, the same way: the
       block now becoming g_yield_block_fallback (`block`, one level out from
       the callee we are entering) keeps ITS OWN proc-ref (saved_ypr_fb, what
       g_yield_proc_ref_fallback was BEFORE this call overwrote it above) so a
       yield two splices deep still finds it once this block's own body is
       finally spliced (emit_block_invoke's one-level-out step reads it back
       out of the fallback2 pair). Without this, a literal block forwarding a
       method's own `&block` by name (`b.call(x)`) through a second inlined
       yielding callee lost the proc and emitted the caller's `b` unrenamed
       (`'lv_b' undeclared`) -- found migrating Enumerable#inject to Ruby,
       general and unrelated to the migration itself. */
    g_yield_proc_ref_fallback2 = saved_ypr_fb;
    g_yield_slot_ty_fallback2 = saved_yslot_fb;
  }
  g_block_id = block;
  /* a forwarded outer block keeps ITS definition depth; a literal block is
     call-site code at the depth BEFORE this inline's renames */
  g_block_nren = (block == saved_block) ? saved_bnren : saved_nren;
  /* ... and, the same way, the &block name of the scope that owns it: a
     forwarded outer block keeps the name ITS OWN definition scope answers
     to (saved_yfbpn); a literal block is call-site code, so it answers to
     THIS call site's own enclosing &block (saved_bpn, this frame's
     g_block_param_name before line below overwrites it for the callee). */
  g_block_owner_param_name = (block == saved_block) ? saved_yfbpn : saved_bpn;
  const char *saved_ypr = g_yield_proc_ref;
  TyKind saved_yslot = g_yield_slot_ty;
  g_yield_proc_ref = fwd_yield_proc;   /* NULL clears it for a normal inline */
  int saved_ype = g_yield_proc_expr; const char *saved_yper = g_yield_proc_expr_ref;
  g_yield_proc_expr = fwd_yield_proc ? fwd_value : -1;
  g_yield_proc_expr_ref = fwd_yield_proc;
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
    /* nil here is "no block", not a TypeError: see sp_poly_to_block */
    else { buf_puts(b, "sp_poly_to_block("); emit_boxed(c, fwd_proc_expr, b); buf_puts(b, ")"); }
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
     here -- and the receiver expression itself is still emitted in the
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

  /* Which parameters this expansion binds as ALIASES of the caller's
     variables (see inline_param_mutated above): a String the body mutates,
     passed as a plain local read, not rebound by the body, and not celled
     for a capture of its own. Decided before the locals are declared, since
     an aliased parameter gets no local of its own. */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  ArgLayout L;
  unsigned alias_mask;
  {
    int pargc = argc;
    if (argc > 0 && argv && nt_type(nt, argv[argc - 1]) &&
        sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) pargc = argc - 1;
    arg_layout(c, m, argv, pargc, pargc < argc ? argv[pargc] : -1, 1, &L);
    refuse_yield_handle_args(c, id);
    alias_mask = inline_alias_params(c, mi, argv, pargc, &L, nt_ref(nt, id, "block"));
  }

  /* declare method locals under renamed names */
  emit_inline_locals_aliased(c, mi, tag, alias_mask, b, din);

  char dflt_cm_self[32];
  const char *dflt_self = recv_self_deref ? selfbuf : NULL, *dflt_deref = recv_self_deref;
  if (!recv_self_deref && cm_class >= 0) {
    snprintf(dflt_cm_self, sizeof dflt_cm_self, "((sp_Class){%d})", cm_self_id ? cm_self_id : cm_class);
    dflt_self = dflt_cm_self;
  }
  InlDflt sv_dflt = inl_dflt_enter(m, g_nren, dflt_self, dflt_deref,
                                   recv_class >= 0 ? recv_class : cm_class);
  emit_inline_bind_params(c, m, args, argv, argc, &L, alias_mask, tag, saved_nren, din, b);
  arg_layout_free(&L);
  inl_dflt_leave(sv_dflt);

  /* Now switch into the RECEIVER's context for the method BODY. Both the
     self binding and the emitting-class must move together, and only here
     -- AFTER argument binding -- so that implicit-self references inside the
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
  /* A class method's body inlined into another class's method: its bare `new`
     asks g_emitting_class_id, which was still the HOST's. `Http.start { }`
     from `Fetch#go` built a Fetch -- clang refused on the pointer type, and two
     layout-compatible classes would have got the wrong object silently
     (#4430). The instance branch above has always set this; the class-method
     branch set only `mi`. */
  else if (cm_class >= 0) {
    g_emitting_class_id = cm_class;
    /* and `self` in that body is the CLASS, the way the method's own function
       spells it -- not the host's instance. `yield self.new` from the same
       shape was refused as `initializing 'sp_Class' with 'sp_Fetch *'`. */
    snprintf(cm_selfbuf, sizeof cm_selfbuf, "((sp_Class){%d})", cm_self_id ? cm_self_id : cm_class);
    g_self = cm_selfbuf;
    g_self_deref = NULL;
  }

  /* per-inline return funnel (stack storage: the inliner recurses, and the
     saved outer label pointer must stay valid across a nested inline). */
  char inl_lbl[32];
  const char *sv_prl = g_method_pr_label, *sv_prv = g_method_pr_var;
  TyKind sv_prt = g_ret_type;
  int sv_prexc = g_method_pr_exc_depth;
  int sv_prens = g_method_pr_ensure_depth;
  if (as_expr) {
    /* Use a result var so the tail uses assignment, not `return`, in the
       GCC statement-expression ({ ... result_var; }) context. */
    TyKind rt = comp_ntype(c, id);
    if (fwd_yield_proc) g_yield_slot_ty = rt;  /* value-position yield unboxes to this */
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    /* a value-type object is a struct, whose zero is `{0}`, not NULL */
    buf_printf(b, " _t%d = %s;\n", rtag, comp_ty_value_obj(c, rt) ? "{0}" : default_value_from_compiler(c, rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    /* g_result_ty is the slot type a tail statement reads to pick its own
       nil sentinel (a bare `nil` feeding a nullable Integer/Float slot,
       #4692-class): every OTHER place that sets g_result_var (begin/rescue)
       keeps it in sync, and this inliner did not, so a body whose only
       fall-through value was a literal `nil` (find_index's "not found" arm)
       read whatever g_result_ty happened to hold from an outer, unrelated
       context and picked that type's default instead of this call's. */
    TyKind sv_rty = g_result_ty; g_result_ty = rt;
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = rvbuf; g_ret_type = rt;
      g_method_pr_exc_depth = g_exc_frame_depth;
      g_method_pr_ensure_depth = g_ensure_depth;
      /* body in its own scope: the funnel goto then EXITS the scopes of any
         cleanup-attributed GC roots the body declares (legal, cleanups run)
         instead of jumping over them in the same scope (a C error). */
      emit_indent(b, din); buf_puts(b, "{\n");
    }
    emit_stmts_tail(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv; g_ret_type = sv_prt;
      g_method_pr_exc_depth = sv_prexc;
      g_method_pr_ensure_depth = sv_prens;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
    g_result_var = sv_rv; g_result_poly = sp; g_result_ty = sv_rty;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else {
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = NULL;
      g_method_pr_exc_depth = g_exc_frame_depth;
      g_method_pr_ensure_depth = g_ensure_depth;
      emit_indent(b, din); buf_puts(b, "{\n");   /* see expr-path comment */
    }
    emit_stmts(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv;
      g_method_pr_exc_depth = sv_prexc;
      g_method_pr_ensure_depth = sv_prens;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
  }
  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  inline_alias_release(m, alias_mask);
  g_nren = saved_nren;
  g_block_id = saved_block;
  g_yield_proc_ref = saved_ypr;
  g_yield_proc_expr = saved_ype; g_yield_proc_expr_ref = saved_yper;
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
  if (pushed_ytgt) g_nytgt--;
  g_ytgt_cur = saved_ytgt_cur;
  g_block_nren = saved_bnren;
  g_yield_block_fallback_nren = saved_yfbn;
  g_block_owner_param_name = saved_bown;
  g_yield_block_fallback_param_name = saved_yfbpn;
  g_yield_proc_ref_fallback = saved_ypr_fb;
  g_yield_slot_ty_fallback = saved_yslot_fb;
  g_yield_proc_ref_fallback2 = saved_ypr_fb2;
  g_yield_slot_ty_fallback2 = saved_yslot_fb2;
  g_yield_self_fallback = saved_self_fb;
  g_yield_self_fallback2 = saved_self_fb2; g_yield_self_deref_fallback2 = saved_deref_fb2;
  g_yield_emitting_class_fallback2 = saved_emcls_fb2;
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
  if (!nm || !is_call_or_yield(nm)) return 0;
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
   block is supplied at this site (g_block_id<0): a bare `m` of
   `def m(&b) = b.call(1)`, or a real-function forwarder with no block of its
   own inlining such a target. The parameter is nil there, so unless a
   forwarded real proc stands in for it (g_yield_proc_ref) the caller emits
   the NoMethodError CRuby raises for `nil.call`. */
int is_blockless_block_param_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id >= 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !is_call_alias(nm)) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && sp_streq(rn, g_block_param_name);
}

/* The method name CRuby's NoMethodError names for such a call on nil:
   `b.()` is sugar for `b.call`, `b[x]` is its own method, and a `b.yield`
   was renamed to `call` by inference but keeps what it was written as. */
const char *blockless_block_param_call_name(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  const char *wn = nt_str(c->nt, id, "written_name");
  return (nm && sp_streq(nm, "[]")) ? "[]" : wn ? wn : "call";
}

/* The call of a block proc `ref` with the yielded args. A splat, or a
   trailing `**h` that passes nothing when empty, makes the count dynamic:
   the args are collected into an array and spread at run time. */
void emit_proc_yield(Compiler *c, const char *ref, int yargc, const int *yargv, Buf *b) {
  /* the proc behind the yield, when the inline that drives it named one */
  { int sv = g_yield_proc_expr;
    if (!ref || g_yield_proc_expr_ref != g_yield_proc_ref || ref != g_yield_proc_ref) g_yield_proc_expr = -1;
    /* a proc form's own block: the blocks its source method's calls pass */
    Scope *ys = yargc > 0 ? comp_scope_of(c, yargv[0]) : NULL;
    g_yield_proc_method = -1;
    if (ref && ys && ys->is_proc_form && ys->blk_param && !strncmp(ref, "lv_", 3) && sp_streq(ref + 3, ys->blk_param))
      g_yield_proc_method = proc_form_source(c, (int)(ys - c->scopes));
    refuse_yield_string_copies(c, yargc, yargv);
    g_yield_proc_method = -1;
    g_yield_proc_expr = sv; }
  if (call_args_need_spread(c->nt, yargv, yargc)) {
    char kwp[24];
    int ta = emit_spread_args_kw(c, yargv, yargc, kwp, sizeof kwp);
    buf_printf(b, "sp_proc_yield_spread(%s, sp_box_poly_array(_t%d), %s)", ref, ta, kwp);
    return;
  }
  buf_printf(b, "sp_proc_yield(%s, ", ref);
  /* a String variable that is the shared handle goes over as the handle,
     its read marked for the call alone: the same yield spliced into a
     literal block binds the plain String (#6179) */
  unsigned char mk[16]; TyKind mt[16];
  int nm = yargc < 16 ? yargc : 16;
  /* the handle's live bytes may ride the plain slot where every target
     reads the box or only reads the String: the blocks a lowered method's
     call sites pass, or the proc forwarded into this splice */
  Scope *ys = nm > 0 ? comp_scope_of(c, yargv[0]) : NULL;
  int lmi = ys && ys->is_lowered_yield ? (int)(ys - c->scopes) : -1;
  unsigned live = 0;
  for (int k = 0; k < nm; k++) {
    mk[k] = c->strbuf_box[yargv[k]]; mt[k] = c->ntype[yargv[k]];
    if (!mk[k] && local_is_handle(c, yargv[k])) { c->strbuf_box[yargv[k]] = 1; c->ntype[yargv[k]] = TY_STRBUF; }
    if (lmi >= 0) { if (dyn_yield_live(c, lmi, k)) live |= 1u << k; }
    else if (g_yield_proc_expr >= 0 && ref && ref == g_yield_proc_ref && g_yield_proc_expr_ref == g_yield_proc_ref) {
      DynReach r;
      dyn_value_reach(c, g_yield_proc_expr, k, &r);
      if (!r.unknown && !r.keeps && !r.unlifted) live |= 1u << k;
    }
  }
  unsigned sv_live = g_yield_live_mask; g_yield_live_mask = live;
  emit_proc_call_args(c, -1, yargc, yargv, b, 1);
  g_yield_live_mask = sv_live;
  for (int k = 0; k < nm; k++) { c->strbuf_box[yargv[k]] = mk[k]; c->ntype[yargv[k]] = mt[k]; }
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
    emit_proc_yield(c, g_yield_proc_ref, yargc, yargv, b);
    buf_puts(b, ";\n");
    return;
  }
  Buf cb; memset(&cb, 0, sizeof cb);
  buf_puts(&cb, "((void)");
  emit_proc_yield(c, g_yield_proc_ref, yargc, yargv, &cb);
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
/* The keyword a block keyword param answers to: its name without the
   `__bp<N>` suffix a block-param rename recorded for it. */
static const char *block_kw_key(const char *kp, char *buf, size_t n) {
  if (!kp || !block_param_is_renamed(kp)) return kp;
  snprintf(buf, n, "%.*s", (int)block_param_written_len(kp), kp);
  return buf;
}

/* True when the literal keys of the yielded keyword hash `ykw` do not come in
   the order the keyword params of block `blk` bind them, as kwh_out_of_order
   judges a method's: one names a param ahead of an earlier key's, or the one
   before it again, or a param after a key the `**kw` rest takes. */
static int ykw_out_of_order(Compiler *c, int blk, int ykw) {
  const NodeTable *nt = c->nt;
  int nkw = 0; while (block_keyword_name(c, blk, nkw)) nkw++;
  int en = 0; const int *el = nt_arr(nt, ykw, "elements", &en);
  int last = -1;
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, el[e], "key");
    if (nt_kind(nt, el[e]) != NK_AssocNode || nt_kind(nt, key) != NK_SymbolNode) continue;
    const char *kn = nt_str(nt, key, "value");
    int at = nkw;
    for (int ki = 0; ki < nkw; ki++) {
      char knb[160];
      if (sp_streq(block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb), kn)) { at = ki; break; }
    }
    if (at < last || (at == last && at < nkw)) return 1;
    last = at;
  }
  return 0;
}

static void emit_block_arg_coerced(Compiler *c, int node, TyKind ot, Buf *b) {
  TyKind at = comp_ntype(c, node);
  /* an empty `{}` / `[]` stays untyped, and emit_boxed gives it the poly form */
  NodeKind nk = nt_kind(c->nt, node);
  int empty_lit = 0;
  if (at == TY_UNKNOWN && (nk == NK_HashNode || nk == NK_ArrayNode)) {
    nt_arr(c->nt, node, "elements", &empty_lit);
    empty_lit = empty_lit == 0;
  }
  if (ot == TY_POLY && ((at != TY_POLY && at != TY_UNKNOWN) || empty_lit)) emit_boxed(c, node, b);
  /* a literal nil into an Integer or a Float parameter (one
     block_settle_types kept nullable, bs_join_val) is the slot's own nil,
     not the 0 it emits as */
  else if (nk == NK_NilNode && ot == TY_INT) buf_puts(b, "SP_INT_NIL");
  else if (nk == NK_NilNode && ot == TY_FLOAT) buf_puts(b, "sp_float_nil()");
  /* and so is a boxed nil: under --int-overflow=promote an Integer slot
     that can hold nil (an ivar, a local, a method's parameter) is widened
     to the box, while a block parameter it feeds keeps its Integer type,
     and `.v.i` read the 0 under the nil tag, so `y(@u) { |a| a }` bound
     0 for a nil @u */
  else if (at == TY_POLY && ot != TY_POLY && ot != TY_UNKNOWN) {
    Buf t; memset(&t, 0, sizeof t); emit_expr(c, node, &t);
    emit_unbox_nilable_text(c, ot, t.p ? t.p : "", b); free(t.p);
  }
  /* a String into a parameter a mutating callee made a buffer, as a plain
     assignment of one takes it (#6039) */
  else if (ot == TY_STRBUF && at == TY_STRING) {
    buf_puts(b, "sp_String_new_shared("); emit_expr(c, node, b); buf_puts(b, ")");
  }
  /* the shared handle, its read marked for a proc a yield of it may call,
     into a plain String parameter: the copy a plain read takes */
  else if (at == TY_STRBUF && ot == TY_STRING && nk == NK_LocalVariableReadNode) {
    int svm = view_push_repr(c, node, VR_STRBUF_BOX, 0);
    int svt = view_push(c, node, TY_STRING);
    emit_expr(c, node, b);
    view_pop(c, svt);
    view_pop(c, svm);
  }
  else emit_expr(c, node, b);
}

/* A tail CALL whose STATEMENT form drops the value the splice is read for:
   tap/then/yield_self answer the receiver or the block's value, and an
   iterator that answers its receiver cannot re-read a computed one. The
   method-tail path already routes both to the value path (codegen_stmt.c);
   a spliced block's tail needed the same, or the statement expression took
   whatever the last emitted statement happened to leave (#4155). */
static int call_targets_yielding_method(Compiler *c, int id);
static int block_tail_needs_value_form(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  /* a tail `yield`: the splice is read for the block's value, and that is
     whatever the yield's own block answers. The statement form drops it,
     as a bare sp_proc_yield call when the block is a forwarded proc, and
     as a statement-form splice of the inner block when it is a literal one:
     an inner tail that is a value-less statement (`x if x != 2`, an
     if-modifier) left the outer splice void (`{ |__fwd| yield __fwd }`, the
     forward of a named &block, handed on to a builtin). */
  if (nt_kind(nt, id) == NK_YieldNode) return 1;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  /* `system(cmd)` as the tail: its statement form is a compound (the argv
     array is built inside braces), whose value is void, while the call has
     a value (the exit status as a boolean) that the block answers with.
     `with_retries { system(cmd) }` did not build (#4802). The expression
     form is the same compound as a statement expression. */
  if (nt_ref(nt, id, "receiver") < 0 && sp_streq(nm, "system")) return 1;
  /* `p x` / `pp x` as the tail: the statement form prints and yields
     nothing, while the call answers its argument (or the argument array),
     which a predicate iterator reads as the block's truthiness
     (`h.any? { |pa| p pa }` did not build). */
  if (nt_ref(nt, id, "receiver") < 0 && (sp_streq(nm, "p") || sp_streq(nm, "pp")) &&
      nt_ref(nt, id, "block") < 0 && !bare_call_class_owned(c, id)) {
    int pa = nt_ref(nt, id, "arguments"), pn = 0;
    if (pa >= 0) nt_arr(nt, pa, "arguments", &pn);
    return pn >= 1;
  }
  /* `a.concat(b)` / `a.push(b)` / `a << b` on an Array as the tail: the
     statement form appends in a compound or through a void C call, while the
     call answers the receiver (`def two = yield([1], [2]); two(&:concat)`
     did not build). */
  if ((sp_streq(nm, "concat") || sp_streq(nm, "push") || sp_streq(nm, "append") || sp_streq(nm, "<<")) &&
      nt_ref(nt, id, "receiver") >= 0 && nt_ref(nt, id, "block") < 0 &&
      nt_ref(nt, id, "arguments") >= 0 && ty_is_array(comp_ntype(c, nt_ref(nt, id, "receiver"))))
    return 1;
  /* and a String mutator on a String held as the sp_String buffer (a block
     parameter that is the shared handle): the statement form appends
     through a void C call, while the call answers the String */
  { int r = nt_ref(nt, id, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode && an_str_mutator_name(nm) &&
        nt_ref(nt, id, "block") < 0) {
      const char *rn = nt_str(nt, r, "name");
      Scope *rs = rn ? comp_scope_of(c, r) : NULL;
      LocalVar *rl = rs ? scope_local(rs, rn) : NULL;
      if (rl && rl->type == TY_STRBUF) return 1;
    } }
  if (nt_ref(nt, id, "block") < 0) return 0;
  if (is_tap_alias(nm))
    return nt_ref(nt, id, "receiver") >= 0;
  /* a block-driving call to a user method that yields is spliced inline;
     its STATEMENT form is a plain compound whose value is void, so a block
     whose value it is must take the expression form (`wrap { M.build(n) {
     ... } }` assigned a void ({...}) to wrap's slot). */
  if (call_targets_yielding_method(c, id)) return 1;
  return iter_value_answers_recv(c, id) && tail_iter_receiver(c, id) < 0;
}

/* The rename entries a spliced block's text and its yield's argument text
   each resolve against (see emit_block_invoke); NULL for a block bound in
   place, whose parameters need no switching. */
struct BiRen {
  int cs_nren, bi_nren, cnt, parked;
  char (*pf)[96];
  char (*pt)[112];
};

static void bi_block_side(BiRen *r) {
  if (!r) return;
  if (r->pf && !r->parked) {
    memcpy(r->pf, g_ren_from + r->bi_nren, sizeof(char[96]) * (size_t)r->cnt);
    memcpy(r->pt, g_ren_to + r->bi_nren, sizeof(char[112]) * (size_t)r->cnt);
    r->parked = 1;
  }
  g_nren = r->bi_nren;
}

static void bi_method_side(BiRen *r) {
  if (!r) return;
  if (r->pf && r->parked) {
    memcpy(g_ren_from + r->bi_nren, r->pf, sizeof(char[96]) * (size_t)r->cnt);
    memcpy(g_ren_to + r->bi_nren, r->pt, sizeof(char[112]) * (size_t)r->cnt);
    r->parked = 0;
  }
  g_nren = r->cs_nren;
}

/* Was yielded value `v` run ahead into a temp (emit_args_before), since a
   later value rebinds the variable it reads? An alias of it then names the
   temp, not the variable's slot, and the block's append never reaches the
   String the caller holds: refused rather than compiled without it. */
static void refuse_alias_of_snapshot(Compiler *c, int v, const char *pn) {
  for (int i = 0; i < g_n_argov; i++) {
    if (g_argov_node[i] != v) continue;
    char msg[512];
    snprintf(msg, sizeof msg,
             "a String is passed to a block's parameter `%s` through a yield, which the block appends to, "
             "and a later value of the same yield rebinds the variable it is read from: the block would "
             "append to a copy of the String the variable held, so the append would not reach the "
             "caller's String. Read the variable into another one ahead of the yield.", pn);
    unsupported_feature(c, v, msg);
    return;
  }
}
/* Bind block `blk`'s keyword parameters and **kwrest from a call's trailing
   keyword hash `ykw` (-1: the call passes none), for a yield or block.call
   and for instance_exec. `bsc` holds the parameters' slots. */
void emit_block_kw_binds(Compiler *c, int blk, int ykw, Scope *bsc, Buf *b, int indent,
                         int as_expr, BiRen *bi, BlockAliases *al) {
  const NodeTable *nt = c->nt;
  /* Keyword block params (`|a:, b: 5|`) and a `**kw` keyword-rest take the
     trailing yielded kwargs hash. Literal pairs match by name at compile
     time. A hash carrying a `**h` is known only at run time, so it is built
     whole -- its merge order decides whether a literal or a splatted key
     wins -- and each keyword is looked up in it. CRuby checks a block's
     keywords as it checks a method's: a required keyword the hash lacks
     raises `missing keyword`, and with no **kwrest a key naming no keyword
     raises `unknown keyword`. */
  int nkw = 0; while (block_keyword_name(c, blk, nkw)) nkw++;
  int ykw_splat = 0;
  if (ykw >= 0) {
    int en = 0; const int *els = nt_arr(nt, ykw, "elements", &en);
    for (int e = 0; e < en; e++) {
      if (nt_kind(nt, els[e]) == NK_AssocSplatNode) {
        ykw_splat = 1;
        int hash = nt_ref(nt, els[e], "value"), hn = 0;
        const int *he = hash >= 0 && nt_kind(nt, hash) == NK_HashNode ? nt_arr(nt, hash, "elements", &hn) : NULL;
        for (int h = 0; h < hn; h++) {
          int value;
          const char *key = dyn_kw_elem_key(c, he[h], &value);
          if (!key || value < 0) continue;
          int overridden = 0;
          for (int j = e + 1; j < en; j++) {
            int later_value;
            const char *later_key = dyn_kw_elem_key(c, els[j], &later_value);
            if (later_key && sp_streq(later_key, key)) { overridden = 1; break; }
          }
          if (overridden) continue;
          NodeKind vk = nt_kind(nt, value);
          TyKind vt = comp_ntype(c, value);
          DynReach r;
          dyn_value_kw_reach(c, blk, key, &r);
          if ((vt == TY_STRING || vt == TY_STRBUF) &&
              (vk == NK_LocalVariableReadNode || vk == NK_InstanceVariableReadNode ||
               vk == NK_GlobalVariableReadNode || vk == NK_ClassVariableReadNode) &&
              r.app)
            unsupported_feature(c, value, "a String variable is passed through a splatted Hash literal "
                                "(`**{ k: v }`) to a block's appending keyword parameter "
                                "(a String is not yet shared by reference through a splatted Hash literal "
                                "(`**{ k: v }`)). Return the String from the block and assign it, or append to it in the caller.");
        }
      }
      /* so is one whose key is an expression: what it names is the run
         time's (`yield(key(1) => v)`) */
      int key = nt_kind(nt, els[e]) == NK_AssocNode ? nt_ref(nt, els[e], "key") : -1;
      if (key >= 0 && nt_kind(nt, key) != NK_SymbolNode && nt_kind(nt, key) != NK_StringNode) ykw_splat = 1;
    }
  }
  int kwh_tmp = -1;
  if (ykw_splat) {
    kwh_tmp = ++g_tmp;
    Buf hb; memset(&hb, 0, sizeof hb); emit_boxed(c, ykw, &hb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
               kwh_tmp, hb.p ? hb.p : "sp_box_nil()", kwh_tmp);
    free(hb.p);
  }
  if (nkw > 0) {
    int kw_bad = ykw_splat;
    int lit_n = 0; const int *lit_els = ykw >= 0 && !ykw_splat ? nt_arr(nt, ykw, "elements", &lit_n) : NULL;
    for (int ki = 0; ki < nkw && !kw_bad; ki++) {
      char knb[160];
      const char *kn = block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb);
      if (block_keyword_default(c, blk, ki) < 0 && (ykw < 0 || ie_kwhash_value(c, ykw, kn) < 0)) kw_bad = 1;
    }
    for (int e = 0; e < lit_n && !kw_bad && !block_kwrest_name(c, blk); e++) {
      int key = nt_ref(nt, lit_els[e], "key");
      const char *ks = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
      int known = 0;
      for (int ki = 0; ks && ki < nkw; ki++) {
        char knb[160];
        if (sp_streq(block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb), ks)) known = 1;
      }
      if (!known) kw_bad = 1;
    }
    if (kw_bad) {
      int chk = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "static const char *const _kw%d[] = {", chk);
      for (int ki = 0; ki < nkw; ki++) {
        char knb[160];
        buf_printf(b, "\"%s\", ", block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb));
      }
      buf_printf(b, "0}, *const _kr%d[] = {", chk);
      for (int ki = 0; ki < nkw; ki++) {
        char knb[160];
        if (block_keyword_default(c, blk, ki) < 0)
          buf_printf(b, "\"%s\", ", block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb));
      }
      buf_printf(b, "0}, *const _kl%d[] = {", chk);
      for (int e = 0; e < lit_n; e++) {
        int key = nt_ref(nt, lit_els[e], "key");
        const char *ks = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
        if (ks) buf_printf(b, "\"%s\", ", ks);
      }
      buf_puts(b, "0}; ");
      if (kwh_tmp >= 0) buf_printf(b, "sp_kwargs_verify(_t%d, ", kwh_tmp);
      else buf_puts(b, "sp_kwargs_verify(sp_box_nil(), ");
      buf_printf(b, "_kw%d, _kr%d, _kl%d, %d)%s", chk, chk, chk,
                 block_kwrest_name(c, blk) ? 0 : 1, as_expr ? "; " : ";\n");
    }
  }
  for (int ki = 0; ; ki++) {
    const char *kp = block_keyword_name(c, blk, ki);
    if (!kp) break;
    char kprbuf[160];
    bi_block_side(bi);
    snprintf(kprbuf, sizeof kprbuf, "%s", rename_local(kp));
    bi_method_side(bi);
    const char *kpr = kprbuf;
    char knb[160];
    const char *kn = block_kw_key(kp, knb, sizeof knb);
    LocalVar *kl = bsc ? scope_local(bsc, kp) : NULL;
    TyKind kt = kl ? kl->type : TY_UNKNOWN;
    int vn = ykw >= 0 && !ykw_splat ? ie_kwhash_value(c, ykw, kn) : -1;
    int dv = block_keyword_default(c, blk, ki);
    /* A keyword the block appends to, bound from a yield of a plain String
       variable: alias the variable, as a positional parameter is aliased
       (emit_block_binds), or the append lands in the keyword's copy. */
    int kw_alias = al && vn >= 0 && kl && nt_kind(nt, vn) == NK_LocalVariableReadNode &&
                   comp_ntype(c, vn) == TY_STRING && block_local_wants_alias(c, blk, kp);
    /* a keyword that is the shared handle takes a handle yielded to it
       itself (yield_splice_kw_handles), as a positional one does */
    if (vn >= 0 && kl && kwh_tmp < 0 && kl->type == TY_STRBUF && kl->str_shared &&
        nt_kind(nt, vn) == NK_LocalVariableReadNode) {
      Buf hb; memset(&hb, 0, sizeof hb);
      if (emit_handle_var_ref(c, vn, &hb)) {
        if (!as_expr) emit_indent(b, indent);
        buf_printf(b, "lv_%s = %s%s", kpr, hb.p, as_expr ? "; " : ";\n");
        free(hb.p);
        continue;
      }
      free(hb.p);
    }
    /* A variable that is the shared handle has no `const char *` slot to
       lend: a keyword that did not take the handle above would append to a
       copy, so the program is refused rather than compiled without it. */
    if (kw_alias && strbuf_local_name(c, vn)) {
      char msg[512];
      snprintf(msg, sizeof msg,
               "a String is passed to a block's keyword `%s` through a yield, which the block appends to: "
               "this yield hands the block a copy, so the append would not reach the caller's String (a "
               "String is not yet shared by reference through a `yield` from a variable that is also "
               "shared with a proc or method). Return the String from the block and assign it, or append "
               "to it in the caller.", kn);
      unsupported_feature(c, vn, msg);
    }
    if (kw_alias) refuse_alias_of_snapshot(c, vn, kn);
    if (kw_alias && al->n < (int)(sizeof al->lv / sizeof al->lv[0])) {
      if (!as_expr) emit_indent(b, indent);
      if (!as_expr && !al->open) { buf_puts(b, "{\n"); emit_indent(b, indent); al->open = 1; }
      buf_printf(b, "const char **_cell_%s = &(", kpr);
      emit_expr(c, vn, b);
      buf_puts(b, ")");
      buf_puts(b, as_expr ? "; " : ";\n");
      al->lv[al->n++] = kl;
      kl->inline_alias++;
      kl->is_cell = 1;
      continue;
    }
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", kpr);
    if (kwh_tmp >= 0) {
      buf_printf(b, "({ sp_bool _f = 0; sp_RbVal _v = sp_poly_hash_get_pair_val(_t%d, "
                    "sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? ", kwh_tmp, kn);
      if (kt == TY_POLY || kt == TY_UNKNOWN) buf_puts(b, "_v");
      else emit_unbox_text(c, kt, "_v", b);
      buf_puts(b, " : ");
      if (dv >= 0) { bi_block_side(bi); emit_block_arg_coerced(c, dv, kt, b); bi_method_side(bi); }
      else buf_puts(b, kt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, kt));
      buf_puts(b, "; })");
    }
    else if (vn >= 0) emit_block_arg_coerced(c, vn, kt, b);
    else if (dv >= 0) { bi_block_side(bi); emit_block_arg_coerced(c, dv, kt, b); bi_method_side(bi); }
    else buf_puts(b, kt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, kt));
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
      bi_block_side(bi);
      snprintf(kwrrbuf, sizeof kwrrbuf, "%s", rename_local(kwr));
      bi_method_side(bi);
      const char *kwrr = kwrrbuf;
      int tkw = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      if (kwh_tmp >= 0)
        buf_printf(b, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_from_poly(_t%d);%s",
                   tkw, kwh_tmp, as_expr ? " " : "\n");
      else
        buf_printf(b, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new();%s", tkw, as_expr ? " " : "\n");
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "SP_GC_ROOT(_t%d);%s", tkw, as_expr ? " " : "\n");
      for (int ki = 0; kwh_tmp >= 0 && ki < nkw; ki++) {
        char knb[160];
        if (!as_expr) emit_indent(b, indent);
        buf_printf(b, "sp_PolyPolyHash_delete(_t%d, sp_box_sym(sp_sym_intern(\"%s\")));%s", tkw,
                   block_kw_key(block_keyword_name(c, blk, ki), knb, sizeof knb), as_expr ? " " : "\n");
      }
      if (ykw >= 0 && kwh_tmp < 0) {
        int en2 = 0; const int *els2 = nt_arr(nt, ykw, "elements", &en2);
        for (int e2 = 0; e2 < en2; e2++) {
          if (!nt_type(nt, els2[e2]) || !sp_streq(nt_type(nt, els2[e2]), "AssocNode")) continue;
          int kn2 = nt_ref(nt, els2[e2], "key");
          int vn2 = nt_ref(nt, els2[e2], "value");
          if (kn2 < 0 || vn2 < 0) continue;
          if (kwh_elem_dropped(nt, ykw, e2)) {
            Buf *sv_pre = g_pre; g_pre = b;
            emit_dropped_value(c, vn2, b);
            g_pre = sv_pre;
            continue;
          }
          /* a pair consumed by a named keyword param stays out of the rest */
          const char *ksym = nt_type(nt, kn2) && sp_streq(nt_type(nt, kn2), "SymbolNode")
                               ? nt_str(nt, kn2, "value") : NULL;
          int consumed = 0;
          for (int ki2 = 0; ksym; ki2++) {
            const char *kp2 = block_keyword_name(c, blk, ki2);
            if (!kp2) break;
            char knb2[128];
            if (sp_streq(block_kw_key(kp2, knb2, sizeof knb2), ksym)) { consumed = 1; break; }
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
      /* a boxed slot (the walks of a boxed receiver type the parameters
         they bind poly, emit_boxed_step_binds) takes the Hash boxed */
      LocalVar *kwl = bsc ? scope_local(bsc, kwr) : NULL;
      char kwt[32]; snprintf(kwt, sizeof kwt, "_t%d", tkw);
      buf_printf(b, "lv_%s = ", kwrr);
      if (kwl && kwl->type == TY_POLY) emit_boxed_text(c, TY_POLY_POLY_HASH, kwt, b);
      else buf_puts(b, kwt);
      buf_puts(b, as_expr ? "; " : ";\n");
    }
  }}

static int subtree_has_own_redo_ex(const NodeTable *nt, int id, int redo);
/* Is `arg` yielded by one of the builtins/ methods whose block CRuby hands
   every value a step of the receiver yields (enum_pair_spread_iter), over
   a receiver whose steps are known only at run time: a boxed one, or an
   Enumerator the method walks itself (find_index, one item at a time)?
   Answers the receiver's type, TY_UNKNOWN for none. */
static TyKind builtin_yield_self_pair(Compiler *c, int arg) {
  Scope *s = comp_scope_of(c, arg);
  if (!s || !s->name || strncmp(s->name, "__enum_", 7) != 0) return TY_UNKNOWN;
  static const char *const names[] = {
    "flat_map", "collect_concat", "filter_map", "count", "any?", "all?", "none?", "one?",
    "find_index", "take_while", NULL };
  int hit = 0;
  /* the method's name, or a clone's (`__enum_any?__12`) */
  for (int k = 0; names[k]; k++) {
    size_t n = strlen(names[k]);
    const char *tail = s->name + 7 + n;
    if (strncmp(s->name + 7, names[k], n) == 0 && (!*tail || strncmp(tail, "__", 2) == 0)) hit = 1;
  }
  if (!hit) return TY_UNKNOWN;
  LocalVar *self = scope_local(s, "__self");
  if (!self || (self->type != TY_POLY && self->type != TY_ENUMERATOR)) return TY_UNKNOWN;
  return self->type;
}

/* Bind block `blk`'s parameters from the values `yargs` (yc positionals,
   and `ykw`, the trailing keyword hash the block's keywords take, or -1),
   by CRuby's proc distribution: requireds (leading and post) first,
   optionals left to right from what remains, a rest the middle, extras
   dropped but still run, missing positions nil or their default, and a
   lone Array auto-splatted when the block takes more than one parameter
   (block_auto_splats) and the call passed no keywords. A count known only at run time -- a splat, a `**`
   that may be empty -- gathers every value into one array first. The yield
   of a spliced block (emit_block_invoke) and instance_exec's literal block
   bind through here; `bi` switches the rename tables for the former (NULL
   for a block bound in place), and `al` collects the parameters aliased to
   a yielded String variable (NULL: none are). */
/* A post block parameter `bp` (renamed `bpr`) the block appends to, bound
   from a yield of plain String variable `yarg`: alias the variable, as a
   required parameter is aliased, or the append lands in the post's copy
   (#6179). A variable that is the shared handle has no slot to lend, and a
   post that does not take the handle is refused there, as a keyword is.
   Answers 1 when it bound the alias. */
static int emit_block_post_alias(Compiler *c, int blk, const char *bp, const char *bpr, LocalVar *bl,
                                 int yarg, Buf *b, int indent, int as_expr, BlockAliases *al) {
  const NodeTable *nt = c->nt;
  if (!al || !bl || nt_kind(nt, yarg) != NK_LocalVariableReadNode || !block_local_wants_alias(c, blk, bp))
    return 0;
  if (local_is_handle(c, yarg) || comp_ntype(c, yarg) == TY_STRBUF) {
    char msg[512], bnb[160];
    snprintf(msg, sizeof msg,
             "a String is passed to a block's post parameter `%s` through a yield, which the block "
             "appends to: this yield hands the block a copy, so the append would not reach the caller's "
             "String (a String is not yet shared by reference through a `yield` into a post block "
             "parameter from a variable that is also shared with a proc or method). Return the String "
             "from the block and assign it, or append to it in the caller.", block_kw_key(bp, bnb, sizeof bnb));
    unsupported_feature(c, yarg, msg);
    return 0;
  }
  if (comp_ntype(c, yarg) != TY_STRING || al->n >= (int)(sizeof al->lv / sizeof al->lv[0])) return 0;
  refuse_alias_of_snapshot(c, yarg, bp);
  if (!as_expr) emit_indent(b, indent);
  if (!as_expr && !al->open) { buf_puts(b, "{\n"); emit_indent(b, indent); al->open = 1; }
  buf_printf(b, "const char **_cell_%s = &(", bpr);
  emit_expr(c, yarg, b);
  buf_puts(b, ")");
  buf_puts(b, as_expr ? "; " : ";\n");
  al->lv[al->n++] = bl;
  bl->inline_alias++;
  bl->is_cell = 1;
  return 1;
}

/* The binds that close every form of emit_block_binds: the keywords, from
   the yield's keyword hash `ykw` (-1: none), and then the rest parameter
   `rest_lv`, from the array built into temp `rest_tmp` (-1: none). */
static void emit_block_binds_close(Compiler *c, int blk, int ykw, Scope *bsc, int rest_tmp, const char *rest_lv,
                                   Buf *b, int indent, int as_expr, BiRen *bi, BlockAliases *al) {
  /* The keywords come last in the call, so they run after every positional
     (the dropped ones above among them): a missing keyword raised before
     `yield(lit(1))` into `|k:|` had evaluated its argument. */
  if (block_no_keywords(c, blk) && ykw >= 0) {
    /* `|**nil|` refuses keywords, an empty `**h` among them excepted */
    Buf hb; memset(&hb, 0, sizeof hb);
    Buf *sv_pre = g_pre; g_pre = b;
    emit_boxed(c, ykw, &hb);
    g_pre = sv_pre;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "if (sp_poly_length(%s) > 0) sp_raise_cls(\"ArgumentError\", \"no keywords accepted\");%s",
               hb.p ? hb.p : "sp_box_nil()", as_expr ? " " : "\n");
    free(hb.p);
  }
  else emit_block_kw_binds(c, blk, ykw, bsc, b, indent, as_expr, bi, al);
  if (rest_tmp >= 0) {
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = _t%d;%s", rest_lv, rest_tmp, as_expr ? " " : "\n");
  }
}

/* Bind block `blk`'s parameters from values whose count only the run time
   knows, gathered into one array in temp `_t<t>` of type `at` (its first
   `sure_n` elements surely there), by the proc distribution sp_proc_fill
   computes: each leading required takes its element or nil, the optionals
   what the requireds leave, a rest the middle, the posts the end. `ykw` is
   the yield's keyword hash for the block's keywords (-1: none), and `site`
   the node a refusal names. The array
   is bound as it stands: an auto-splat of a lone Array is the caller's,
   which knows whether the values allow one. emit_block_binds binds a yield
   with a splat through here, and the walks of a boxed receiver bind the
   values of each step (emit_boxed_step_binds). */
static void emit_block_binds_gathered(Compiler *c, int blk, int t, TyKind at, int sure_n, int ykw, int site,
                                      Buf *b, int indent, int as_expr, BiRen *bi, BlockAliases *al) {
  Scope *bsc = comp_scope_of(c, blk);
  int P = 0; while (block_param_name(c, blk, P)) P++;
  int O = 0; while (block_opt_name(c, blk, O)) O++;
  int Q = 0; while (block_post_name(c, blk, Q)) Q++;
  int R = block_rest_marker(c, blk);
  TyKind et = ty_array_elem(at);
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    char bprbuf[160];
    bi_block_side(bi);
    snprintf(bprbuf, sizeof bprbuf, "%s", rename_local(bp));
    bi_method_side(bi);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", bprbuf);
    /* element k of the gathered array, guarded: when the array is shorter
       than the param list the surplus params bind nil (CRuby auto-splat),
       using the same per-slot default the non-splat under-fill path does. */
    LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
    TyKind bt = bl ? bl->type : TY_UNKNOWN;
    /* a parameter that takes the handle (block_splat_pull_args), bound
       from a gathered Array of plain Strings: no value there was pulled
       into the handle (`yield(*[a[0]])`), so the block would append to
       a copy */
    if (bt == TY_STRBUF && et == TY_STRING)
      unsupported_feature(c, site,
                          "a String is passed through a splat into a yield to a block parameter the block "
                          "appends to, from a value that is not a String variable: the block would append "
                          "to a copy. Return the String from the block and assign it, or append to it in the caller");
    Buf eb; memset(&eb, 0, sizeof eb);
    int sure = k < sure_n;
    if (sure) emit_array_elem_sure(at, t, k, &eb);
    else {
      emit_array_elem_at(at, t, k, &eb);
      buf_printf(b, "(%d < (_t%d ? _t%d->len : 0) ? ", k, t, t);
    }
    if (bt == TY_POLY && et != TY_POLY && et != TY_UNKNOWN)
      emit_boxed_text(c, et, eb.p ? eb.p : "0", b);
    /* a nil element (`yield(*[1, nil])`) into a nullable Integer or
       Float is its sentinel, where `.v.i` / `.v.f` read the 0 under the
       nil tag. Such a parameter holds no other kind: an Integer and a
       Float element box it (ty_unify). */
    else if (et == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN && bl->nullable_int)
      emit_unbox_nilable_text(c, bt, eb.p ? eb.p : "", b);
    else if (et == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN)
      emit_unbox_text(c, bt, eb.p ? eb.p : "", b);
    else
      buf_puts(b, eb.p ? eb.p : "");
    if (!sure) buf_printf(b, " : %s)", bt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, bt));
    free(eb.p);
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Distribution counts: the array's length is the run time's, so the
     optional-take count and the post start index are runtime temps
     (sp_proc_fill, which a proc's prologue binds by too). */
  const char *brest = block_rest_name(c, blk);
  int t_ot = -1, t_ps = -1;
  if (O > 0 || Q > 0 || brest) {
    t_ot = ++g_tmp; t_ps = ++g_tmp;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "sp_int _t%d, _t%d; sp_proc_fill(%d, %d, %d, %d, _t%d ? _t%d->len : 0, &_t%d, &_t%d);%s",
               t_ot, t_ps, P, O, Q, R, t, t, t_ot, t_ps, as_expr ? " " : "\n");
  }
  /* Optional block params (`|a, b=10|`): bind from the args left over after
     the requireds (pre AND post) are satisfied, else the declared default. */
  for (int oi = 0; ; oi++) {
    const char *op = block_opt_name(c, blk, oi);
    if (!op) break;
    char oprbuf[160];
    bi_block_side(bi);
    snprintf(oprbuf, sizeof oprbuf, "%s", rename_local(op));
    bi_method_side(bi);
    LocalVar *ol = bsc ? scope_local(bsc, op) : NULL;
    TyKind ot = ol ? ol->type : TY_UNKNOWN;
    int dv = block_opt_default(c, blk, oi);
    const char *odflt = ot == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, ot);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", oprbuf);
    Buf eb; memset(&eb, 0, sizeof eb);
    emit_array_elem_at(at, t, P + oi, &eb);
    buf_printf(b, "(%d < _t%d ? ", oi, t_ot);
    if (ot == TY_POLY && et != TY_POLY && et != TY_UNKNOWN) emit_boxed_text(c, et, eb.p ? eb.p : "0", b);
    else if (et == TY_POLY && ot != TY_POLY && ot != TY_UNKNOWN && ol->nullable_int)
      emit_unbox_nilable_text(c, ot, eb.p ? eb.p : "", b);
    else if (et == TY_POLY && ot != TY_POLY && ot != TY_UNKNOWN) emit_unbox_text(c, ot, eb.p ? eb.p : "", b);
    else buf_puts(b, eb.p ? eb.p : "");
    buf_puts(b, " : ");
    if (dv >= 0) { bi_block_side(bi); emit_block_arg_coerced(c, dv, ot, b); bi_method_side(bi); }
    else buf_puts(b, odflt);
    buf_puts(b, ")");
    free(eb.p);
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* A trailing rest parameter (`|*a|`) collects the leftover middle, past
     the pre-requireds and the taken optionals, up to the first post's
     value, into a fresh array, assigned last (see emit_block_binds). */
  int rest_tmp = -1; char rest_lv[160] = "";
  if (brest) {
    bi_block_side(bi);
    snprintf(rest_lv, sizeof rest_lv, "%s", rename_local(brest));
    bi_method_side(bi);
    rest_tmp = ++g_tmp;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new();%s", rest_tmp, as_expr ? " " : "\n");
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "SP_GC_ROOT(_t%d);%s", rest_tmp, as_expr ? " " : "\n");
    int jj = ++g_tmp;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = %d + _t%d; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, ",
               jj, P, t_ot, jj, t_ps, jj, rest_tmp);
    char acc[96];
    if (at == TY_POLY_ARRAY) snprintf(acc, sizeof acc, "sp_PolyArray_get(_t%d, _t%d)", t, jj);
    else snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d)", array_kind(at) ? array_kind(at) : "Int", t, jj);
    if (at == TY_POLY_ARRAY) buf_puts(b, acc);
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, acc, &bx); buf_puts(b, bx.p ? bx.p : acc); free(bx.p); }
    buf_puts(b, as_expr ? "); " : ");\n");
  }
  /* Post-required params (`|a, *b, c, d|`): bind from the post start index
     on, left-to-right; missing positions bind the slot nil. */
  for (int qi = 0; qi < Q; qi++) {
    const char *qp = block_post_name(c, blk, qi);
    if (!qp) continue;   /* anonymous post: consumes a slot, binds nothing */
    char qprbuf[160];
    bi_block_side(bi);
    snprintf(qprbuf, sizeof qprbuf, "%s", rename_local(qp));
    bi_method_side(bi);
    LocalVar *ql = bsc ? scope_local(bsc, qp) : NULL;
    TyKind qt = ql ? ql->type : TY_UNKNOWN;
    const char *qdflt = qt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, qt);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", qprbuf);
    int te = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = _t%d + %d; (_t%d < (_t%d ? _t%d->len : 0) ? ",
               te, t_ps, qi, te, t, t);
    char acc[96];
    if (at == TY_POLY_ARRAY) snprintf(acc, sizeof acc, "sp_PolyArray_get(_t%d, _t%d)", t, te);
    else snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d)", array_kind(at) ? array_kind(at) : "Int", t, te);
    if (qt == TY_POLY && et != TY_POLY && et != TY_UNKNOWN) {
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, acc, &bx);
      buf_puts(b, bx.p ? bx.p : acc); free(bx.p);
    }
    else if (et == TY_POLY && qt != TY_POLY && qt != TY_UNKNOWN)
      emit_unbox_text(c, qt, acc, b);
    else buf_puts(b, acc);
    buf_printf(b, " : %s); })", qdflt);
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  emit_block_binds_close(c, blk, ykw, bsc, rest_tmp, rest_lv, b, indent, as_expr, bi, al);
}

/* Does block `blk` take more than plain leading requireds: an optional, a
   named rest, a post, a keyword or a `**`? A walk over a boxed receiver binds
   such a block through emit_boxed_step_binds, and a plain one (a trailing
   comma's implicit rest among them) by its own reads. */
int block_binds_gathered(Compiler *c, int blk) {
  if (block_opt_name(c, blk, 0) || block_post_name(c, blk, 0) || block_keyword_name(c, blk, 0))
    return 1;
  const char *rest = block_rest_name(c, blk);
  if (rest && *rest) return 1;
  int bp = nt_ref(c->nt, blk, "parameters");
  int pn = bp >= 0 ? nt_ref(c->nt, bp, "parameters") : -1;
  return pn >= 0 && nt_ref(c->nt, pn, "keyword_rest") >= 0;
}

/* Bind block `blk`'s parameters from one step of a walk over a boxed
   receiver. How many values a step yields is the run time's: one for an
   Array element or a Hash entry's pair, two for the pair Hash#select
   yields, as many as the step yielded for an Enumerator. `vals` is a C
   expression answering them as a fresh sp_PolyArray *; they bind by the
   proc distribution, a lone Array auto-splatted where the block's shape
   asks for it, as the yield of each value would bind them. Answers 0,
   binding nothing, for a plain block (block_binds_gathered), which the
   walks bind by their own reads. */
int emit_boxed_step_binds(Compiler *c, int blk, const char *vals, Buf *b, int indent, int as_expr) {
  if (!block_binds_gathered(c, blk)) return 0;
  int P = 0; while (block_param_name(c, blk, P)) P++;
  int O = 0; while (block_opt_name(c, blk, O)) O++;
  int Q = 0; while (block_post_name(c, blk, Q)) Q++;
  int t = ++g_tmp;
  if (!as_expr) emit_indent(b, indent);
  buf_printf(b, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);%s", t, vals, t, as_expr ? " " : "\n");
  if (block_auto_splats(P, O, Q, block_rest_marker(c, blk))) {
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "if (_t%d->len == 1) { sp_RbVal _e = sp_PolyArray_get(_t%d, 0); "
                  "if (_e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_e.cls_id)) "
                  "_t%d = sp_poly_to_poly_array(_e); }%s",
               t, t, t, as_expr ? " " : "\n");
  }
  emit_block_binds_gathered(c, blk, t, TY_POLY_ARRAY, 0, -1, blk, b, indent, as_expr, NULL, NULL);
  return 1;
}

void emit_block_binds(Compiler *c, int blk, const int *yargs, int yc,
                      Buf *b, int indent, int as_expr, BiRen *bi, BlockAliases *al) {
  const NodeTable *nt = c->nt;
  Scope *bsc = comp_scope_of(c, blk);
  /* A trailing kwargs hash (`k: 1`, `**h`) goes to a block that declares
     keyword params, a **kwrest or `**nil`, never to its positionals: CRuby
     binds `yield(k: 1)` into `|a, k:|` as a = nil. It stays positional only
     for a block that takes no keywords. */
  int ykw = -1, nokw = block_no_keywords(c, blk);
  if (yc > 0 && yargs && nt_kind(nt, yargs[yc - 1]) == NK_KeywordHashNode &&
      (block_keyword_name(c, blk, 0) || block_kwrest_name(c, blk) || nokw)) {
    ykw = yargs[yc - 1];
    yc--;
  }
  /* CRuby's argument distribution needs the parameter shape up front:
     P pre-required, O optionals, Q post-required, R any rest marker
     (`*name`, bare `*`, or the implicit rest of a trailing comma `|a, |`).
     Requireds (pre and post) bind first, optionals take what remains
     left-to-right, a rest collects the leftover middle, extras drop. */
  int P = 0; while (block_param_name(c, blk, P)) P++;
  int O = 0; while (block_opt_name(c, blk, O)) O++;
  int Q = 0; while (block_post_name(c, blk, Q)) Q++;
  int R = block_rest_marker(c, blk);
  /* CRuby runs every value before it binds any, and the binds below run in
     the parameters' order, the block's defaults among them: keywords out
     of that order, an optional's default that runs ahead of a later post's
     value (`yield(1, lit(2))` into `|a, b = lit(9), c|`), two values with
     an effect, one's setup draining ahead of the other's bind
     (`yield(lit(1), **h(x))`), and a read a later value or a default can
     change (`yield(@v, **h(x))`, `yield(k: @v)` into `|a = (@v = 9), k:|`)
     run the values first, in source order, into temps the binds read (the
     caller restores g_n_argov). A value no later one can change stays
     where it is. */
  int nk = 0; while (block_keyword_name(c, blk, nk)) nk++;
  int *dfl = malloc(sizeof(int) * (size_t)(O + nk + 1)), nd = 0;
  for (int oi = 0; oi < O; oi++) dfl[nd++] = block_opt_default(c, blk, oi);
  for (int ki = 0; ki < nk; ki++) dfl[nd++] = block_keyword_default(c, blk, ki);
  if (ykw >= 0 && !kwh_has_splat(nt, ykw) && ykw_out_of_order(c, blk, ykw))
    emit_args_before(c, yargs, yc + 1, dfl, nd, g_pre);
  else if (yargs && !call_args_need_spread(nt, yargs, yc)) {
    int ot = 0, ps = 0;
    block_fill(P, O, Q, R, yc, &ot, &ps);
    if ((ot < O && Q > 0 && yc > ps) || args_order_matters(c, yargs, yc + (ykw >= 0), dfl, nd))
      emit_args_before(c, yargs, yc + (ykw >= 0), dfl, nd, g_pre);
  }
  free(dfl);
  /* `yield(*arr)`: a single splat spreads the array across the block params
     (auto-splat). Evaluate it once into a rooted temp and bind each param (and
     any rest param) from its elements rather than from the splat AST node. */
  int splat_tmp = -1; TyKind splat_at = TY_UNKNOWN;
  int splat_sure = 0;   /* leading elements the splatted array surely holds */
  /* Keywords the block's keywords take turn the auto-splat off, empty ones
     too: CRuby spreads a lone Array only for a yield that passed none, bar
     the only-leading-requireds block, which takes no keywords (the `**h`
     arm below). `yield([1], **h)` into `|a, b, **kw|` binds a = [1]. A
     block bound in place (bi NULL) is instance_exec's, a method that drops
     an empty `**h` before it yields, so there a `**` alone keeps it on. */
  int autosplat = (ykw < 0 || (!bi && kwh_only_spreads(nt, ykw))) && block_auto_splats(P, O, Q, R);
  /* ...but only an EMPTY one: a `**h` that spreads keywords turns it off as
     any keywords do, and only the run time knows which. The values are
     gathered and the lone Array spread there when every `**` is empty; a
     non-empty `**h` bound [1, 2] across `|a, b, **kw|`. The `**` values run
     first, in source order, so the test and the keywords' bind read them
     once. */
  int ie_kw = autosplat && ykw >= 0;
  if (ie_kw) emit_args_before(c, yargs, yc + 1, NULL, 0, g_pre);
  int lone_splat_typed = 0;
  if (yc == 1 && yargs && nt_kind(nt, yargs[0]) == NK_SplatNode) {
    TyKind lt = comp_ntype(c, nt_ref(nt, yargs[0], "expression"));
    lone_splat_typed = ty_is_array(lt) || lt == TY_POLY_ARRAY;
  }
  /* A boxed value auto-splatted into a block with optionals, posts or a
     rest is gathered like a splat, so those bind by the distribution: bound
     by index, `yield(x)` with x = [1, 2, 3] into `|a, b = 5, c|` answered
     [1, 5, nil]. Leading requireds alone keep the lighter per-index read
     below. */
  int boxed_spread = autosplat && yc == 1 && yargs && nt_kind(nt, yargs[0]) != NK_SplatNode &&
                     comp_ntype(c, yargs[0]) == TY_POLY && (O > 0 || Q > 0 || block_rest_name(c, blk));
  int poly_splat_tmp = -1;   /* a boxed yielded value splatted at run time */
  TyKind self_ty = TY_UNKNOWN;
  /* One step of a boxed or Enumerator receiver, which a builtins/ method
     yields packed as one value where CRuby hands the block every value the
     step yielded (builtin_yield_self_pair): an optional, a post or a rest
     binds the values themselves, gathered (sp_yielded_args). Spread as
     the one value, `count { |*r| }` over each_with_index bound [[3, 0]]. */
  TyKind step_ty = (ykw < 0 && yc == 1 && yargs && nt_kind(nt, yargs[0]) != NK_SplatNode &&
                    (O > 0 || Q > 0 || block_rest_name(c, blk)))
                   ? builtin_yield_self_pair(c, yargs[0]) : TY_UNKNOWN;
  /* A trailing hash made only of `**h` splats, into a block taking no
     keywords, is a positional only when it is non-empty: `yield(1, **{})`
     yields just 1. The count is known at run time, so the arguments are
     collected into an array and bound as a splat. */
  if (ykw < 0 && yc > 0 && yargs && kwh_only_spreads(nt, yargs[yc - 1])) {
    splat_at = TY_POLY_ARRAY;
    splat_tmp = emit_spread_args(c, yargs, yc);
    /* instance_exec (bi NULL) drops an empty `**h` before it yields, so
       its auto-splat is the usual one */
    if (yc == 2 && (!bi ? autosplat : block_lead_only(c, blk))) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (_t%d->len == 1) { sp_RbVal _e = sp_PolyArray_get(_t%d, 0); "
                        "if (_e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_e.cls_id)) "
                        "_t%d = sp_poly_to_poly_array(_e); }\n",
                 splat_tmp, splat_tmp, splat_tmp);
    }
  }
  else if (step_ty != TY_UNKNOWN) {
    splat_at = TY_POLY_ARRAY;
    splat_tmp = ++g_tmp;
    Buf pb2; memset(&pb2, 0, sizeof pb2); emit_boxed(c, yargs[0], &pb2);
    emit_indent(g_pre, g_indent);
    if (step_ty == TY_POLY)
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_yielded_args(sp_poly_yields_pair(lv_%s), %s); SP_GC_ROOT(_t%d);\n",
                 splat_tmp, rename_local("__self"), pb2.p ? pb2.p : "sp_box_nil()", splat_tmp);
    else
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_yielded_args(lv_%s ? lv_%s->yields_pair : 0, %s); SP_GC_ROOT(_t%d);\n",
                 splat_tmp, rename_local("__self"), rename_local("__self"), pb2.p ? pb2.p : "sp_box_nil()", splat_tmp);
    free(pb2.p);
    if (autosplat) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (_t%d->len == 1) { sp_RbVal _e = sp_PolyArray_get(_t%d, 0); "
                        "if (_e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_e.cls_id)) "
                        "_t%d = sp_poly_to_poly_array(_e); }\n",
                 splat_tmp, splat_tmp, splat_tmp);
    }
  }
  /* A splat beside other yielded values spreads by a length only the run
     time knows, as a method call's gather does (arg_layout): the values go
     into one array, bound as a splat. Each parameter took the value at its
     own index, the splat's whole array among them, so `yield(*s, 2)` bound
     the array into the first parameter (a C type error for a typed one)
     and `yield(*[], 1)` bound nil. A lone value left is auto-splatted as a
     lone yielded Array would be. A lone splat of something that is neither
     a typed array nor boxed (`yield(*[])`, `*nil`, a Hash, a Range) is
     gathered too: the path below binds only those, and the splat node
     itself was bound as a positional (an sp_PolyArray * into a slot). A
     boxed one gathers as well (sp_enum_items_from), and its lone element
     auto-splats: bound by index, `yield(*x)` bound only the requireds, and
     a post took the splat node itself (a C type error). */
  else if (yargs && (ie_kw || (call_args_need_spread(nt, yargs, yc) && !lone_splat_typed) || boxed_spread)) {
    splat_at = TY_POLY_ARRAY;
    splat_tmp = emit_spread_args(c, yargs, yc);
    if (autosplat) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (_t%d->len == 1", splat_tmp);
      int kn = 0; const int *kv = ie_kw ? nt_arr(nt, ykw, "elements", &kn) : NULL;
      for (int e = 0; kv && e < kn; e++) {
        Buf hb; memset(&hb, 0, sizeof hb);
        emit_boxed(c, nt_ref(nt, kv[e], "value"), &hb);
        buf_printf(g_pre, " && sp_poly_length(%s) == 0", hb.p ? hb.p : "sp_box_nil()");
        free(hb.p);
      }
      buf_printf(g_pre, ") { sp_RbVal _e = sp_PolyArray_get(_t%d, 0); "
                        "if (_e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_e.cls_id)) "
                        "_t%d = sp_poly_to_poly_array(_e); }\n",
                 splat_tmp, splat_tmp);
    }
  }
  if (splat_tmp < 0 && yc == 1 && yargs) {
    int inner = -1;
    if (nt_type(nt, yargs[0]) && sp_streq(nt_type(nt, yargs[0]), "SplatNode"))
      inner = nt_ref(nt, yargs[0], "expression");
    /* CRuby auto-splat: a single (non-splat) Array yielded to a block taking
       more than one binding slot destructures across the params
       (block_auto_splats). */
    else if (autosplat)
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
    else if (inner < 0 && P == 1 && !O && !Q && !R &&
             (self_ty = builtin_yield_self_pair(c, yargs[0])) != TY_UNKNOWN) {
      /* A builtin whose block CRuby hands the values one step of an
         Enumerator yields, walking a receiver known only at run time: a
         lone parameter takes the first of a step that yielded several. */
      poly_splat_tmp = ++g_tmp;
      Buf pb2; memset(&pb2, 0, sizeof pb2); emit_boxed(c, yargs[0], &pb2);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 poly_splat_tmp, pb2.p ? pb2.p : "sp_box_nil()", poly_splat_tmp);
      emit_indent(g_pre, g_indent);
      if (self_ty == TY_POLY)
        buf_printf(g_pre, "int _fs%d = sp_yielded_packed(sp_poly_yields_pair(lv_%s), _t%d);\n",
                   poly_splat_tmp, rename_local("__self"), poly_splat_tmp);
      else
        buf_printf(g_pre, "int _fs%d = sp_yielded_packed(lv_%s ? lv_%s->yields_pair : 0, _t%d);\n",
                   poly_splat_tmp, rename_local("__self"), rename_local("__self"), poly_splat_tmp);
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
      /* a local whose length nothing can change holds every element of the
         literal it was assigned (splat_local_sure_lit): the parameters those
         reach bind them without a length test, as the analysis bound them
         without a nil */
      if (inner != yargs[0] && (at == TY_INT_ARRAY || at == TY_FLOAT_ARRAY || at == TY_STR_ARRAY)) {
        int lit = splat_local_sure_lit(c, inner);
        if (lit >= 0) nt_arr(nt, lit, "elements", &splat_sure);
      }
      /* the one value a splat leaves is auto-splatted, as a lone yielded
         Array is: `yield(*[[1, 2]])` into `|a, b|` binds 1 and 2 */
      if (inner != yargs[0] && at == TY_POLY_ARRAY && autosplat) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "if (_t%d && _t%d->len == 1) { sp_RbVal _e = sp_PolyArray_get(_t%d, 0); "
                          "if (_e.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_e.cls_id)) "
                          "_t%d = sp_poly_to_poly_array(_e); }\n",
                   splat_tmp, splat_tmp, splat_tmp, splat_tmp);
      }
    }
  }
  if (splat_tmp >= 0) {
    emit_block_binds_gathered(c, blk, splat_tmp, splat_at, splat_sure, ykw, yargs && yc > 0 ? yargs[0] : blk,
                              b, indent, as_expr, bi, al);
    return;
  }
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    /* The block's own param name resolves at the block's definition depth:
       renames pushed by the enclosing method-inline must not capture it. */
    char bprbuf[160];
    bi_block_side(bi);
    snprintf(bprbuf, sizeof bprbuf, "%s", rename_local(bp));
    bi_method_side(bi);
    const char *bpr = bprbuf;
    if (!as_expr) emit_indent(b, indent);
    /* A parameter the block mutates in place, bound from a yield of a plain
       variable: alias the variable itself rather than copy it, so the
       block's append reaches what was yielded. The parameter reads and
       writes through the cell for the rest of this splice. */
    /* A global's or a class variable's C global is aliased the same way,
       as a call lends it, unless the block or what it calls can assign the
       variable meanwhile (refuse_lent_global_rebound). */
    char gref[256];
    NodeKind yk = poly_splat_tmp < 0 && k < yc ? nt_kind(nt, yargs[k]) : NK__COUNT;
    int gslot = (yk == NK_GlobalVariableReadNode && gvar_global_slot(c, yargs[k], gref, sizeof gref)) ||
                (yk == NK_ClassVariableReadNode && cvar_global_slot(c, yargs[k], gref, sizeof gref));
    if (poly_splat_tmp < 0 && k < yc &&
        ((yk == NK_LocalVariableReadNode && !local_is_handle(c, yargs[k])) || gslot) &&
        comp_ntype(c, yargs[k]) == TY_STRING && block_param_wants_alias(c, blk, k, -1)) {
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      if (bl && al && !gslot) refuse_alias_of_snapshot(c, yargs[k], bp);
      if (bl && al && gslot) refuse_lent_global_rebound(c, yargs[k], gref, "a block", bp);
      if (bl && al && al->n < (int)(sizeof al->lv / sizeof al->lv[0])) {
        if (!as_expr && !al->open) { buf_puts(b, "{\n"); emit_indent(b, indent); al->open = 1; }
        buf_printf(b, "const char **_cell_%s = &(", bpr);
        emit_expr(c, yargs[k], b);
        buf_puts(b, ")");
        buf_puts(b, as_expr ? "; " : ";\n");
        al->lv[al->n++] = bl;
        bl->inline_alias++;
        bl->is_cell = 1;
        continue;
      }
    }
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
    else if (k < yc) {
      /* boxed into a poly slot, unboxed into a typed one, and an empty `[]`
         or `{}` (untyped) given its poly form, as every arm binds a value;
         a parameter that is the shared handle takes a handle yielded to it
         itself (yield_splice_handles) */
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      Buf hb; memset(&hb, 0, sizeof hb);
      if (bl && bl->type == TY_STRBUF && bl->str_shared && emit_handle_var_ref(c, yargs[k], b)) {}
      /* a boxed parameter takes the handle's own box, or nil when the
         variable holds none (the handle is NULL for a nil) */
      else if (bl && bl->type == TY_POLY && emit_handle_var_ref(c, yargs[k], &hb))
        buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", hb.p);
      else emit_block_arg_coerced(c, yargs[k], bl ? bl->type : TY_UNKNOWN, b);
      free(hb.p);
    }
    else {
      LocalVar *bl = scope_local(bsc, bp);
      TyKind bt = bl ? bl->type : TY_INT;
      buf_puts(b, bt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, bt));
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Distribution counts: the yields are direct, so they resolve statically
     from yc (a splat's gather binds by emit_block_binds_gathered). */
  int ot_static = 0, ps_static = 0;
  block_fill(P, O, Q, R, yc, &ot_static, &ps_static);
  const char *brest = block_rest_name(c, blk);
  /* Optional block params (`|a, b=10|`): bind from the args left over after
     the requireds (pre AND post) are satisfied, else the declared default. */
  for (int oi = 0; ; oi++) {
    const char *op = block_opt_name(c, blk, oi);
    if (!op) break;
    char oprbuf[160];
    bi_block_side(bi);
    snprintf(oprbuf, sizeof oprbuf, "%s", rename_local(op));
    bi_method_side(bi);
    const char *opr = oprbuf;
    LocalVar *ol = bsc ? scope_local(bsc, op) : NULL;
    TyKind ot = ol ? ol->type : TY_UNKNOWN;
    int dv = block_opt_default(c, blk, oi);
    int yi = P + oi;
    const char *odflt = ot == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, ot);
    /* An optional the block appends to, bound from a yield of a plain
       String variable: alias the variable, as a required parameter is
       aliased above, or the append lands in the optional's copy (#6179).
       A variable that is the shared handle has no slot to lend, and the
       optional does not take the handle: refused, as a keyword is. */
    int opt_alias = al && ol && poly_splat_tmp < 0 && oi < ot_static && yi < yc &&
                    nt_kind(nt, yargs[yi]) == NK_LocalVariableReadNode &&
                    block_local_wants_alias(c, blk, op);
    if (opt_alias && (local_is_handle(c, yargs[yi]) || comp_ntype(c, yargs[yi]) == TY_STRBUF)) {
      char msg[512], onb[160];
      snprintf(msg, sizeof msg,
               "a String is passed to a block's optional parameter `%s` through a yield, which the block "
               "appends to: this yield hands the block a copy, so the append would not reach the caller's "
               "String (a String is not yet shared by reference through a `yield` into an optional block "
               "parameter from a variable that is also shared with a proc or method). Return the String "
               "from the block and assign it, or append to it in the caller.", block_kw_key(op, onb, sizeof onb));
      unsupported_feature(c, yargs[yi], msg);
    }
    if (opt_alias && comp_ntype(c, yargs[yi]) == TY_STRING && !local_is_handle(c, yargs[yi]) &&
        al->n < (int)(sizeof al->lv / sizeof al->lv[0])) {
      refuse_alias_of_snapshot(c, yargs[yi], op);
      if (!as_expr) emit_indent(b, indent);
      if (!as_expr && !al->open) { buf_puts(b, "{\n"); emit_indent(b, indent); al->open = 1; }
      buf_printf(b, "const char **_cell_%s = &(", opr);
      emit_expr(c, yargs[yi], b);
      buf_puts(b, ")");
      buf_puts(b, as_expr ? "; " : ";\n");
      al->lv[al->n++] = ol;
      ol->inline_alias++;
      ol->is_cell = 1;
      continue;
    }
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", opr);
    if (oi < ot_static) {
      /* an optional that is the shared handle takes a handle yielded to it
         itself (yield_splice_handles), as a required one does */
      if (!(ol && ol->type == TY_STRBUF && ol->str_shared && emit_handle_var_ref(c, yargs[yi], b)))
        emit_block_arg_coerced(c, yargs[yi], ot, b);
    }
    else if (dv >= 0) {
      bi_block_side(bi); emit_block_arg_coerced(c, dv, ot, b); bi_method_side(bi);
    }
    else {
      buf_puts(b, odflt);
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* A trailing rest parameter (`|*a|`) collects the yielded arguments past the
     requireds into a fresh array. */
  int rest_tmp = -1; char rest_lv[160] = "";
  if (brest) {
    char brestrbuf[160];
    bi_block_side(bi);
    snprintf(brestrbuf, sizeof brestrbuf, "%s", rename_local(brest));
    bi_method_side(bi);
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
    for (int j = P + ot_static; j < yc - Q; j++) {
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "sp_PolyArray_push(_t%d, ", trest);
      emit_boxed(c, yargs[j], b);
      buf_puts(b, as_expr ? "); " : ");\n");
    }
    rest_tmp = trest;
    snprintf(rest_lv, sizeof rest_lv, "%s", brestr);
  }
  /* Post-required params (`|a, *b, c, d|`): bind after the pre/optional/rest
     consumption point, left-to-right; missing positions bind the slot nil. */
  if (Q > 0) {
    for (int qi = 0; qi < Q; qi++) {
      const char *qp = block_post_name(c, blk, qi);
      if (!qp) continue;   /* anonymous post: consumes a slot, binds nothing */
      char qprbuf[160];
      bi_block_side(bi);
      snprintf(qprbuf, sizeof qprbuf, "%s", rename_local(qp));
      bi_method_side(bi);
      const char *qpr = qprbuf;
      LocalVar *ql = bsc ? scope_local(bsc, qp) : NULL;
      TyKind qt = ql ? ql->type : TY_UNKNOWN;
      const char *qdflt = qt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, qt);
      /* a post the block appends to aliases the variable yielded to it */
      if (poly_splat_tmp < 0 && ps_static + qi < yc &&
          emit_block_post_alias(c, blk, qp, qpr, ql, yargs[ps_static + qi], b, indent, as_expr, al))
        continue;
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "lv_%s = ", qpr);
      int idx = ps_static + qi;
      /* a post that is the shared handle takes a handle yielded to it
         itself (yield_splice_handles), as a required one does */
      if (idx < yc && ql && ql->type == TY_STRBUF && ql->str_shared && emit_handle_var_ref(c, yargs[idx], b)) {}
      else if (idx < yc) emit_block_arg_coerced(c, yargs[idx], qt, b);
      else buf_puts(b, qdflt);
      buf_puts(b, as_expr ? "; " : ";\n");
    }
  }
  /* Ruby evaluates every yielded argument for its side effects, even ones no
     block param binds -- an empty or under-arity block still runs the arg
     expression (`yield(@f = Foo.new)` must set @f). The loops above emitted only
     the bound args; a rest param collects and thereby evaluates the middle, but
     with no rest those dropped middle args would be lost. Evaluate them here for
     effect (#3209). */
  if (!brest) {
    /* with no rest the posts take the values right after the optionals, so
       the dropped ones are those past the posts: counted back from the end,
       a post's own value ran twice and the last extra never did */
    for (int j = ps_static + Q; j < yc; j++) {
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, yargs[j], &vb);
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "(void)(%s)%s", vb.p ? vb.p : "0", as_expr ? "; " : ";\n");
      free(vb.p);
    }
  }
  emit_block_binds_close(c, blk, ykw, bsc, rest_tmp, rest_lv, b, indent, as_expr, bi, al);
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
  /* emit_block_binds may run the values first into temps the binds read */
  int argov_saved = g_n_argov;
  /* The spliced body and the block's own parameter NAMES resolve at the
     block's DEFINITION-site rename depth (g_block_nren): the entries the
     enclosing method-inline pushed above that mark must not capture
     same-named block locals (a numbered `_1` used by both the callee's own
     block and the caller's, #3281). Block-side text emits with the callee's
     entries PARKED (copied out, count truncated) so a nested inline inside
     the body cannot clobber them; method-side (yield-arg) text restores
     them. */
  BiRen bi = { .cs_nren = g_nren };
  bi.bi_nren = g_block_nren < bi.cs_nren ? g_block_nren : bi.cs_nren;
  bi.cnt = bi.cs_nren - bi.bi_nren;
  if (bi.cnt > 0) {
    bi.pf = malloc(sizeof(char[96]) * (size_t)bi.cnt);
    bi.pt = malloc(sizeof(char[112]) * (size_t)bi.cnt);
  }
  #define BI_BLOCK_SIDE() bi_block_side(&bi)
  #define BI_METHOD_SIDE() bi_method_side(&bi)
  BlockAliases al = { .n = 0 };
  refuse_yield_splat(c, blk, yc, yargs);
  refuse_yield_capwrap(c, blk, yc, yargs);
  if (as_expr) buf_puts(b, "({ ");
  emit_block_binds(c, blk, yargs, yc, b, indent, as_expr, &bi, &al);
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
     runs out of C stack. The globals record one fallback level, so once it
     is consumed they have no outer block left to name: -1, not itself (the
     yield-target entries refill it below when they record one).
     Reached by a method that both yields and recurses through a block that
     forwards the yield (`def walk; yield self; @kids.each { |k| k.walk { |x|
     yield x } }; end`) -- valid Ruby that segfaulted the compiler. */
  int svfb = g_yield_block_fallback; g_yield_block_fallback = -1;
  /* the body is block-definition-site code: emit it below the callee's
     rename entries, and pair the fallback block with ITS depth so a nested
     yield inside the body splices at the right level */
  BI_BLOCK_SIDE();
  int sv_bnren = g_block_nren; g_block_nren = g_yield_block_fallback_nren;
  /* The block body about to be emitted is g_block_owner_param_name's own
     code: a `<name>.call(...)` inside it, where <name> is THAT scope's own
     &block parameter, must still expand as a yield there, so this splice
     runs under that scope's own name -- not the current callee's
     (g_block_param_name, which g_block_owner_param_name already holds the
     right value FOR, having been tracked in lockstep with g_block_id/
     g_block_nren the whole way down). Was unconditionally NULL, which read
     as "no forwarded &block here" for a block that forwarded one two
     splices out -- the caller's own name then had no rename entry either
     (it is deliberately not a plain local, see codegen_util.c's
     "virtual &block slot"), and the generated C read it unrenamed
     (`'lv_b' undeclared`). General and pre-existing; found migrating
     Enumerable#inject to Ruby (a literal block forwarding a method's own
     `&block` by name through a second inlined yielding callee). */
  const char *svbpn = g_block_param_name; g_block_param_name = g_block_owner_param_name;
  const char *svbown = g_block_owner_param_name; g_block_owner_param_name = g_yield_block_fallback_param_name;
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
  int sv_bens = g_method_pr_ensure_depth;
  g_method_pr_label = g_fn_pr_label; g_method_pr_var = g_fn_pr_var;
  g_ret_type = g_fn_ret_type;
  g_method_pr_exc_depth = 0;   /* the real function's funnel sits at depth 0 */
  g_method_pr_ensure_depth = 0;
  /* likewise, the block body's `self` is the CALLER's (an ivar read inside
     the block must not resolve against the inlined method's receiver) -- and
     so is the block body's emitting-class, so an implicit-self *call* in the
     block resolves against the caller's class, not the receiver's. */
  const char *sv_bself = g_self, *sv_bderef = g_self_deref;
  int sv_bemcls = g_emitting_class_id;
  const char *sv_ysf = g_yield_self_fallback, *sv_ysdf = g_yield_self_deref_fallback;
  int sv_yecf = g_yield_emitting_class_fallback;
  if (g_yield_self_fallback) {
    g_self = g_yield_self_fallback;
    g_self_deref = g_yield_self_deref_fallback;
    g_emitting_class_id = g_yield_emitting_class_fallback;
  }
  /* the block one level out is now g_block_id: its self comes with it */
  g_yield_self_fallback = g_yield_self_fallback2;
  g_yield_self_deref_fallback = g_yield_self_deref_fallback2;
  g_yield_emitting_class_fallback = g_yield_emitting_class_fallback2;
  /* ... and the caller's lowered context: a `yield` in this spliced caller
     code binds the enclosing lowered method's proc, not this inline. */
  int sv_blow = g_current_scope_is_lowered;
  const char *sv_blbn = g_lowered_blk_name;
  g_current_scope_is_lowered = g_yield_lowered_fallback;
  g_lowered_blk_name = g_yield_lowered_blk_fallback;
  const char *sv_bypr = g_yield_proc_ref; TyKind sv_byslot = g_yield_slot_ty;
  const char *sv_yprf = g_yield_proc_ref_fallback; TyKind sv_yslotf = g_yield_slot_ty_fallback;
  g_yield_proc_ref = g_yield_proc_ref_fallback;
  g_yield_slot_ty = g_yield_slot_ty_fallback;
  /* the forwarded proc one level out moves out with it, the same way self's
     fallback2 does just above: without this, a THIRD nested splice (a block
     forwarding a method's own `&block` by name, itself handed through a
     second inlined yielding callee) found this slot already overwritten by
     the second callee's own (irrelevant) proc-ref and fell back to an
     unrenamed caller local (`'lv_b' undeclared`) -- general and pre-existing,
     found migrating Enumerable#inject to Ruby. */
  g_yield_proc_ref_fallback = g_yield_proc_ref_fallback2;
  g_yield_slot_ty_fallback = g_yield_slot_ty_fallback2;
  /* The fallback level just consumed, refilled from the yield-target entries
     when the block now current has one: its own lowered context, and its
     target with the whole context that target runs under. Without an entry
     it stays -1, as above. */
  YieldTargetSave yts;
  yield_target_save(&yts);
  {
    int fe = (g_ytgt_cur >= 0 && g_ytgt[g_ytgt_cur].blk == svb) ? g_ytgt[g_ytgt_cur].up : -1;
    if (fe >= 0 && g_ytgt[fe].blk != g_block_id) fe = -1;
    g_ytgt_cur = fe;
    if (fe >= 0) yield_target_enter(fe);
  }
  /* A `next` in a yielded block leaves the BLOCK with its value -- but this
     body is spliced inline (no _proc_ function, no loop), so a bare
     `continue` is invalid C. Only when the body owns a `next`, wrap the
     splice in do{}while(0) and route the value through a temp via the
     inline-each next-var machinery; blocks without `next` keep their exact
     previous emission. */
  int nx_own = subtree_has_own_next(nt, bbody);
  const char *sv_nx2 = g_ie_next_var; int sv_poly2 = g_ie_res_poly; TyKind sv_nty2 = g_ie_next_ty;
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
      /* A void tail (a call to a method that always raises) produces no
         value, and method_call_ret types it nil for the call. Joined raw,
         VOID and an Integer `next` made the carrier poly while the call was
         typed Integer, and the boxed carrier did not fit the call's sp_int.
         An untyped tail stays as it is: method_call_ret takes the `next`
         type alone for it, and so does the join below. */
      if (nx_bt == TY_VOID) nx_bt = TY_NIL;
      /* the block's value is its tail OR a `next v`: a nil tail with an
         Integer next is the nullable Integer, not nil */
      { TyKind nxv = block_next_value_ntype(c, bbody);
        if (nxv != TY_UNKNOWN && nxv != TY_VOID) nx_bt = ty_unify(nx_bt, nxv); }
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
      /* a `next nil` into an Integer or Float slot is the sentinel */
      g_ie_next_ty = (nx_bt == TY_INT || nx_bt == TY_FLOAT) ? nx_bt : TY_UNKNOWN;
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
      g_ie_next_var = NULL; g_ie_res_poly = 0; g_ie_next_ty = TY_UNKNOWN;
      emit_indent(b, indent); buf_puts(b, "do {\n");
    }
  }
  /* A `redo` in the block re-runs the block's body with the same arguments,
     not the loop around the yield: the label goes on the splice, after the
     parameters are bound. The loop's own label re-ran the callee's loop body
     (a builtin's `buf << x` pushed the element again), and a yield outside
     any loop had none at all (`continue` outside a loop). */
  int rd_lbl = 0;
  if (bbody >= 0 && subtree_has_own_redo_ex(nt, bbody, -1) &&
      g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0])) {
    rd_lbl = ++g_tmp;
    g_redo_owner[g_redo_depth] = bbody;
    g_redo_stack[g_redo_depth++] = rd_lbl;
  }
  /* ...and after the body's setup, its locals' reset and the parameter
     rebindings (block_param_rebind_len), which a redo does not re-run. The
     arms below that emit the statements one by one place it after the
     rebindings; emit_stmts places it for the last arm. */
  int rd_head = rd_lbl ? block_param_rebind_len(nt, bbody) : 0;
  if (nx_own && as_expr && g_ie_next_var && !nx_tail_stmt && bn3 > 0) {
    /* the block's locals are fresh on every call, as in the arms below
       (block_of_body builds the map on first use: read off the compiler
       before any block body had been emitted, it was not there yet, and a
       block spliced into an expression kept the previous call's locals) */
    if (block_of_body(c, bbody) >= 0) emit_block_locals_reset(c, block_of_body(c, bbody), b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) {
      if (rd_lbl && k3 == rd_head) buf_printf(b, "_redo_%d: ; ", rd_lbl);
      emit_stmt(c, bd3[k3], b, 0);
    }
    if (rd_lbl && rd_head >= bn3 - 1) buf_printf(b, "_redo_%d: ; ", rd_lbl);
    /* the tail's prelude stays inside the splice, after the parameter
       bindings: hoisted to the enclosing statement, an array literal tail
       (`{ |x| next [] if x == 2; [x] }`) was built from the parameter's
       slot before the element was bound into it */
    /* An untyped call tail (a method no class answers) is lowered to its
       NoMethodError raise, or to a nil placeholder on a dynamic receiver. It
       has no value to store: the slot the `next` values type already holds
       nil. Assigned, the raise's sp_RbVal went into an sp_int slot for
       `{ next 5 if c; obj.missing }`, and the C did not compile. */
    int tl3 = bd3[bn3 - 1];
    TyKind tlt3 = comp_ntype(c, tl3);
    int void_tail = (tlt3 == TY_UNKNOWN || tlt3 == TY_VOID) &&
                    nt_kind(nt, tl3) == NK_CallNode && nt_ref(nt, tl3, "receiver") >= 0;
    { Buf tb; memset(&tb, 0, sizeof tb);
      Buf *svp3 = g_pre; int svi3 = g_indent; g_pre = b; g_indent = 0;
      if (void_tail) emit_expr(c, tl3, &tb);
      else if (g_ie_res_poly) emit_boxed(c, tl3, &tb);
      else emit_expr_slot(c, tl3, nx_bt, &tb);
      g_pre = svp3; g_indent = svi3;
      if (void_tail) buf_puts(b, "(void)(");
      else buf_printf(b, "%s = ", nxbuf);
      if (tb.p) buf_puts(b, tb.p);
      if (void_tail) buf_puts(b, ")");
      free(tb.p); }
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
    if (block_of_body(c, bbody) >= 0) emit_block_locals_reset(c, block_of_body(c, bbody), b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) {
      if (rd_lbl && k3 == rd_head) buf_printf(b, "_redo_%d: ; ", rd_lbl);
      emit_stmt(c, bd3[k3], b, 0);
    }
    if (rd_lbl && rd_head >= bn3 - 1) buf_printf(b, "_redo_%d: ; ", rd_lbl);
    /* the tail's own prelude stays INSIDE the splice, after the parameter
       bindings above it: hoisted to the enclosing statement, a forwarded
       proc's yield read the block parameter before it was bound */
    /* the expression itself is rendered aside first: an emitter writes its
       prelude while it is part-way through the expression text, and with
       the prelude buffer being `b` that text would have landed in the
       middle of a call's argument list (a proc-forwarded yield's rooted
       argument temp, #4662) */
    { Buf tb; memset(&tb, 0, sizeof tb);
      Buf *svp3 = g_pre; int svi3 = g_indent; g_pre = b; g_indent = 0;
      if (want_poly && ty_is_object(comp_ntype(c, bd3[bn3 - 1]))) emit_boxed(c, bd3[bn3 - 1], &tb);
      else emit_expr(c, bd3[bn3 - 1], &tb);
      g_pre = svp3; g_indent = svi3;
      if (tb.p) buf_puts(b, tb.p);
      free(tb.p); }
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
             comp_ntype(c, bd3[bn3 - 1]) == TY_POLY) ||
            /* a scalar tail into the poly slot the yield was typed for (a
               block answering an Integer where another site's answers a
               String): boxed, or the splice handed the raw sp_int to an
               sp_RbVal (`groups[yield(x)] ||= []` under two block kinds) */
            comp_ntype(c, bd3[bn3 - 1]) == TY_INT || comp_ntype(c, bd3[bn3 - 1]) == TY_FLOAT ||
            comp_ntype(c, bd3[bn3 - 1]) == TY_STRING || comp_ntype(c, bd3[bn3 - 1]) == TY_BOOL ||
            comp_ntype(c, bd3[bn3 - 1]) == TY_SYMBOL) &&
           nt_type(nt, bd3[bn3 - 1]) &&
           !sp_streq(nt_type(nt, bd3[bn3 - 1]), "ReturnNode")) {
    /* concrete-typed bare tail into a poly slot: box it (#3278) */
    if (block_of_body(c, bbody) >= 0) emit_block_locals_reset(c, block_of_body(c, bbody), b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) {
      if (rd_lbl && k3 == rd_head) buf_printf(b, "_redo_%d: ; ", rd_lbl);
      emit_stmt(c, bd3[k3], b, 0);
    }
    if (rd_lbl && rd_head >= bn3 - 1) buf_printf(b, "_redo_%d: ; ", rd_lbl);
    /* the tail's prelude stays inside the splice here too: a forwarded
       proc's `f.call(__fwd)` read its parameter's slot ahead of the binding
       when the read was hoisted to the enclosing statement */
    { Buf tb; memset(&tb, 0, sizeof tb);
      Buf *svp3 = g_pre; int svi3 = g_indent; g_pre = b; g_indent = 0;
      emit_boxed(c, bd3[bn3 - 1], &tb);
      g_pre = svp3; g_indent = svi3;
      if (tb.p) buf_puts(b, tb.p);
      free(tb.p); }
    buf_puts(b, "; ");
  }
  else if (as_expr && !nx_own && bn3 > 0 &&
           want_ty != TY_POLY && want_ty != TY_UNKNOWN && want_ty != TY_VOID && want_ty != TY_NIL &&
           nt_kind(nt, unwrap_parens(c, bd3[bn3 - 1])) == NK_CallNode &&
           comp_ntype(c, unwrap_parens(c, bd3[bn3 - 1])) == TY_UNKNOWN) {
    /* An untyped call tail (a method no class answers) lowers to the gate's
       NoMethodError raise, an sp_RbVal; spliced bare, it became the value of
       a statement expression read into the yield's typed slot, and the C did
       not compile when another site's block typed that slot (`try { 1 }` then
       `try { obj.missing }`). The raise never returns, so coerce it to the
       slot as emit_unresolved_coerced does for any typed store; the `next`
       arm above drops the same tail for the same reason. */
    if (block_of_body(c, bbody) >= 0) emit_block_locals_reset(c, block_of_body(c, bbody), b, 0);
    for (int k3 = 0; k3 < bn3 - 1; k3++) {
      if (rd_lbl && k3 == rd_head) buf_printf(b, "_redo_%d: ; ", rd_lbl);
      emit_stmt(c, bd3[k3], b, 0);
    }
    if (rd_lbl && rd_head >= bn3 - 1) buf_printf(b, "_redo_%d: ; ", rd_lbl);
    { Buf tb; memset(&tb, 0, sizeof tb);
      Buf *svp3 = g_pre; int svi3 = g_indent; g_pre = b; g_indent = 0;
      emit_unresolved_coerced(c, bd3[bn3 - 1], want_ty, &tb);
      g_pre = svp3; g_indent = svi3;
      if (tb.p) buf_puts(b, tb.p);
      free(tb.p); }
    buf_puts(b, "; ");
  }
  else {
    if (rd_lbl && block_of_body(c, bbody) >= 0) g_redo_pending = rd_lbl;
    else if (rd_lbl && as_expr) buf_printf(b, "_redo_%d: ; ", rd_lbl);
    else if (rd_lbl) { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", rd_lbl); }
    emit_stmts(c, bbody, b, as_expr ? 0 : (nx_own ? indent + 1 : indent));
    /* The block's value is its last statement, and this splice is read as the
       value of a statement expression. A receiver-returning iterator there
       emits as a loop with no value, so the slot it feeds gets void (or, for
       an Array receiver, the loop counter). Put the receiver back. */
    if (as_expr && bbody >= 0) {
      int bn4 = 0; const int *bd4 = nt_arr(c->nt, bbody, "body", &bn4);
      int rr4 = (bd4 && bn4 > 0) ? tail_iter_receiver(c, bd4[bn4 - 1]) : -1;
      if (rr4 >= 0) { emit_tail_recv_value(c, bd4[bn4 - 1], rr4, b); buf_puts(b, "; "); }
    }
  }
  if (rd_lbl) g_redo_depth--;
  if (nx_own) {
    g_c_loop_depth--;
    g_loop_exc_base = sv_lexc2;
    g_loop_ensure_base = sv_lens2;
    if (as_expr) buf_printf(b, "} while(0); %s; ", g_ie_next_var ? nxbuf : "(void)0");
    else { emit_indent(b, indent); buf_puts(b, "} while(0);\n"); }
    g_ie_next_var = sv_nx2; g_ie_res_poly = sv_poly2; g_ie_next_ty = sv_nty2;
  }
  for (int ya = 0; ya < al.n; ya++) {
    if (--al.lv[ya]->inline_alias == 0) al.lv[ya]->is_cell = 0;
  }
  if (al.open) { emit_indent(b, indent); buf_puts(b, "}\n"); }
  g_self = sv_bself; g_self_deref = sv_bderef;
  g_yield_self_fallback = sv_ysf; g_yield_self_deref_fallback = sv_ysdf; g_yield_emitting_class_fallback = sv_yecf;
  g_emitting_class_id = sv_bemcls;
  g_current_scope_is_lowered = sv_blow;
  g_lowered_blk_name = sv_blbn;
  g_yield_proc_ref = sv_bypr; g_yield_slot_ty = sv_byslot;
  g_yield_proc_ref_fallback = sv_yprf; g_yield_slot_ty_fallback = sv_yslotf;
  g_method_pr_label = sv_bl; g_method_pr_var = sv_bv; g_ret_type = sv_bt;
  g_method_pr_exc_depth = sv_bexc;
  g_method_pr_ensure_depth = sv_bens;
  g_brk_ser_var = svser; g_brk_ensure_base = svebase; g_brk_exc_base = svbexc;
  g_block_brk_var = svbbv; g_block_brk_ebase = svbbe;
  g_block_nren = sv_bnren;
  BI_METHOD_SIDE();
  yield_target_restore(&yts);
  g_block_id = svb; g_yield_block_fallback = svfb; g_block_param_name = svbpn;
  g_block_owner_param_name = svbown;
  if (as_expr) {
    /* `{ return e }`: the block exits the enclosing function, so the
       statement-expr's tail is unreachable -- but C still needs a value
       expression there (a trailing `return;` makes the ({...}) void).
       `{ raise "x" }` leaves the same hole: the block never produces a value,
       and the slot it feeds is typed from the OTHER call sites' blocks, so
       the splice landed a void where an sp_int was wanted and the C did not
       build -- `@v = yield`, `t += yield i`, and a bare `yield` in a method
       carrying a rescue all stopped a program that CRuby runs. */
    int bn2 = 0; const int *bd2 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn2) : NULL;
    int tail_ret = bn2 > 0 && nt_type(nt, bd2[bn2 - 1]) &&
                   sp_streq(nt_type(nt, bd2[bn2 - 1]), "ReturnNode");
    if (tail_ret || stmts_diverge(c, bbody)) {
      /* The filler is read as the value of the statement expression, so the
         type it has to have is the CONSUMER's -- want_ty -- not the type of
         the value the unreachable return carries. Taking the return's own put
         a String literal in a poly yield slot and stopped the build: the two
         only agree when the block's value and the method's return happen to
         be the same type, which is the case that had been tried. */
      TyKind ft = want_ty;
      /* Only a `return` carries a value whose type can stand in; a raise's
         argument is its MESSAGE, and taking that put a String where an int
         slot was wanted. */
      if ((ft == TY_UNKNOWN || ft == TY_VOID || ft == TY_NIL) && tail_ret) {
        int ra = nt_ref(nt, bd2[bn2 - 1], "arguments");
        int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
        TyKind rt2 = rn > 0 ? comp_ntype(c, rv[0]) : TY_INT;
        ft = is_scalar_ret(rt2) ? rt2 : TY_INT;
      }
      if (ft == TY_UNKNOWN || ft == TY_VOID || ft == TY_NIL) ft = TY_INT;
      /* a by-value object is a bare struct: default_value's NULL is ill-typed */
      if (ty_is_object(ft) && comp_ty_value_obj(c, ft))
        buf_printf(b, " (sp_%s){0};", c->classes[ty_object_class(ft)].c_name);
      else
        buf_printf(b, " %s;", default_value_from_compiler(c, ft));
    }
    buf_puts(b, "})");
  }
  view_unbind(argov_saved);
  free(bi.pf); free(bi.pt);
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
/* the call whose builtin default arm is being emitted: it must not come
   back here and build the same switch again */
/* Does a statement-level block call on a poly receiver take the block
   dispatch (#2448), and through which user classes? Their count into cand
   (at most max), or 0 when the dispatch does not apply. Shared with the
   resolver (cplan_poly_block). */
int poly_block_dispatch_cands(Compiler *c, int id, int *cand, int max) {
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
  int nc = 0;
  for (int k = 0; k < c->nclasses && nc < max; k++) {
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
  /* a class method of the name: the value may be a Class, which only the
     ordinary poly dispatch's class-side arms answer */
  { int ncc = 0; comp_cmethod_candidates(c, name, &ncc); if (ncc > 0) return 0; }
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
  return nc;
}

int emit_poly_recv_block_dispatch(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int block = nt_ref(nt, id, "block");
  if (g_prbd_skip == id) return 0;
  int cand[64];
  int nc = poly_block_dispatch_cands(c, id, cand, 64);
  if (nc == 0) return 0;
  int trecv = ++g_tmp;
  /* Build the whole switch into a scratch buffer first. `emit_inline_call`
     can DECLINE an arm -- a candidate that consumes its block through a
     declared `&blk` rather than `yield` is not an inlining target: it has a
     standalone C function instead, which is the point of that spelling. Its
     return value was discarded here, so the arm was opened and nothing was
     put in it (`case 29: { break; }`) and the call silently disappeared: a
     poly-receiver `Net::HTTP#request` with a block did exactly this, and so
     does any mix where one candidate yields and another declares `&blk`.
     Nothing is written to `b` until every arm has emitted a body; if one
     declines, discard the lot and return 0 so the ordinary poly dispatch
     emits the call and threads the block, the way it already does for a
     typed receiver. Same policy as the |x| gate above: hand it to a path
     that emits something, never leave a silent empty body. */
  Buf sw; memset(&sw, 0, sizeof sw);
  emit_indent(&sw, indent);
  buf_printf(&sw, "sp_RbVal _t%d = ", trecv); emit_boxed(c, recv, &sw); buf_puts(&sw, ";\n");
  emit_indent(&sw, indent);
  buf_printf(&sw, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv);
  emit_indent(&sw, indent);
  buf_printf(&sw, "switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {\n", trecv, trecv);
  const char *sv_expr = g_inline_recv_expr;
  int sv_class = g_inline_recv_class;
  int pa_frame = g_plan_check ? pa_begin(id) : -1;
  for (int i = 0; i < nc; i++) {
    int k = cand[i];
    emit_indent(&sw, indent);
    buf_printf(&sw, "case %d: {\n", k);
    char castbuf[96];
    /* a by-value (value-type) class is stored inline: the inline entry binds
       self as `sp_X _tN = <expr>`, so hand it the struct, not the pointer */
    snprintf(castbuf, sizeof castbuf, "%s(sp_%s *)_t%d.v.p",
             c->classes[k].is_value_type ? "*" : "", c->classes[k].c_name, trecv);
    g_inline_recv_expr = castbuf;
    g_inline_recv_class = k;
    int v = view_push(c, recv, ty_object(k));  /* so the inline entry classifies the receiver */
    size_t before = sw.len;
    int armed = emit_inline_call(c, id, &sw, indent + 1);
    int empty = !armed || sw.len == before;
    g_inline_recv_expr = sv_expr;
    g_inline_recv_class = sv_class;
    view_pop(c, v);
    if (empty) { free(sw.p); if (g_plan_check) pa_drop(pa_frame); return 0; }
    emit_indent(&sw, indent + 1); buf_puts(&sw, "break;\n");
    emit_indent(&sw, indent); buf_puts(&sw, "}\n");
    if (g_plan_check) {
      pa_resume(pa_frame);
      pa_observe(PA_USER, k, comp_method_in_chain(c, k, name, NULL), TY_UNKNOWN, PC_VOID);
    }
  }
  int emitted_default = 0;
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
      emit_indent(&sw, indent); buf_puts(&sw, "default: {\n");
      emit_indent(&sw, indent + 1);
      buf_printf(&sw, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"map!\"); SP_GC_ROOT(_t%d);\n",
                 tw, trecv, tw);
      /* The loop below stores into the array's elements directly rather than
         through a runtime mutator, so it carries its own write barrier: the
         receiver may be an old array taking references to values this loop
         has just made. Once before the loop is enough -- the remembered set
         dedupes on the object, not the store. */
      emit_indent(&sw, indent + 1);
      buf_printf(&sw, "sp_gc_wb((void *)_t%d);\n", tw);
      emit_indent(&sw, indent + 1);
      buf_printf(&sw, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, tw, ti2);
      emit_indent(&sw, indent + 2);
      buf_printf(&sw, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", dp0r, tw, ti2);
      for (int j2 = 0; j2 + 1 < dbn; j2++) emit_stmt(c, dbb[j2], b, indent + 2);
      { int svi = g_indent; g_indent = indent + 2;
        Buf vb2; memset(&vb2, 0, sizeof vb2); emit_boxed(c, dbb[dbn - 1], &vb2);
        g_indent = svi;
        emit_indent(&sw, indent + 2);
        buf_printf(&sw, "_t%d->data[_t%d] = %s;\n", tw, ti2, vb2.p ? vb2.p : "sp_box_nil()");
        free(vb2.p); }
      emit_indent(&sw, indent + 1); buf_puts(&sw, "}\n");
      emit_indent(&sw, indent + 1);
      buf_printf(&sw, "sp_poly_arr_writeback(_t%d, _t%d);\n", trecv, tw);
      emit_indent(&sw, indent + 1); buf_puts(&sw, "break;\n");
      emit_indent(&sw, indent); buf_puts(&sw, "}\n");
      emitted_default = 1;
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_BD_MAP_BANG, -1, TY_UNKNOWN, PC_SAME);
    }
  }
  /* A String's own iterators: the switch exists because a user class owns
     the name (packages/stringio's StringIO#each_char), and its arms are that
     class's, so a String reaching it raised NoMethodError for a method String
     has (#5083, the block-taking sibling of #4816). The default arm serves a
     String with the ordinary String emission: the receiver is handed over as
     the string's bytes, typed String for the length of the emission, so the
     typed iterator runs the block exactly as it would with no user class of
     that name in the program. */
  /* A File's line, char and byte iterators are the same case: StringIO
     defines them in Ruby, and a File sharing the slot (a parameter taking
     either stream) raised NoMethodError. Its arm is the typed File
     emission, the receiver handed over as the unboxed handle. */
  int str_iter = sp_streq(name, "each_char") || sp_streq(name, "each_byte") ||
                 sp_streq(name, "each_line") || sp_streq(name, "each_grapheme_cluster") ||
                 sp_streq(name, "each_codepoint");
  int io_iter = sp_streq(name, "each_char") || sp_streq(name, "each_byte") ||
                sp_streq(name, "each_line");
  /* the default the chain below settles on (PT_BD_DEFAULT): 1 a String or
     IO iterator, 2 the builtin surface, 0 the raise */
  int pa_answer = 0, pa_offered = !emitted_default;
  if (!emitted_default && nt_ref(nt, id, "arguments") < 0 && (str_iter || io_iter) &&
      g_n_argov + 1 <= MAX_ARG_OVERRIDE) {
    int ts = ++g_tmp, tf = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    Buf fb; memset(&fb, 0, sizeof fb);
    int slot = view_bind(recv, "_t%d", ts);
    if (str_iter) {
      int v = view_push(c, recv, TY_STRING);
      /* analysis renames a String's each_grapheme_cluster to each_char (the
         two agree over the text spinel carries); this receiver was not a
         String then, so the arm takes the same spelling for its emission */
      int graph = sp_streq(name, "each_grapheme_cluster");
      if (graph) nt_node_set_str((NodeTable *)nt, id, "name", "each_char");
      emit_stmt(c, id, &ab, indent + 2);
      if (graph) {
        nt_node_set_str((NodeTable *)nt, id, "name", "each_grapheme_cluster");
        name = nt_str(nt, id, "name");   /* the set replaced the string name read */
      }
      view_pop(c, v);
    }
    if (io_iter) {
      view_unbind(slot);
      view_bind(recv, "_t%d", tf);
      int v = view_push(c, recv, TY_IO);
      emit_stmt(c, id, &fb, indent + 2);
      view_pop(c, v);
    }
    view_unbind(g_n_argov - 1);
    int str_arm = ab.p && !strstr(ab.p, "sp_raise_nomethod(");
    int io_arm = fb.p && !strstr(fb.p, "sp_raise_nomethod(");
    if (str_arm || io_arm) {
      emit_indent(&sw, indent); buf_puts(&sw, "default: {\n");
      if (str_arm) {
        emit_indent(&sw, indent + 1);
        buf_printf(&sw, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) {\n", trecv, trecv);
        emit_indent(&sw, indent + 2);
        buf_printf(&sw, "const char *_t%d = sp_poly_recv_s(_t%d, \"%s\"); SP_GC_ROOT_STR(_t%d);\n",
                   ts, trecv, name, ts);
        buf_puts(&sw, ab.p);
        emit_indent(&sw, indent + 2); buf_puts(&sw, "break;\n");
        emit_indent(&sw, indent + 1); buf_puts(&sw, "}\n");
      }
      if (io_arm) {
        emit_indent(&sw, indent + 1);
        buf_printf(&sw, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) {\n", trecv, trecv);
        emit_indent(&sw, indent + 2);
        buf_printf(&sw, "sp_File *_t%d = (sp_File *)_t%d.v.p;\n", tf, trecv);
        buf_puts(&sw, fb.p);
        emit_indent(&sw, indent + 2); buf_puts(&sw, "break;\n");
        emit_indent(&sw, indent + 1); buf_puts(&sw, "}\n");
      }
      emit_indent(&sw, indent + 1);
      buf_printf(&sw, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;\n", name, trecv);
      emit_indent(&sw, indent); buf_puts(&sw, "}\n");
      emitted_default = 1;
      pa_answer = 1;
    }
    free(ab.p);
    free(fb.p);
  }
  /* Every other poly dispatch closes with a raising default; this one closed
     with nothing, so a runtime class outside the candidate set fell through
     the switch and the call silently did nothing -- CRuby raises NoMethodError
     there. The candidate set is the INSTANTIATED user classes that define the
     name, so an instance of a class that does not is exactly the case: real,
     and previously silent. (#3234 is the same hole in this switch, found from
     the builtin-array side and patched only for map!/collect!.) */
  /* Any other builtin receiver: the switch exists because a user class owns
     the name, and a user class owning a name must not change what a builtin
     does. Re-enter the statement's ordinary emission with the receiver read
     from the hoisted temp and g_poly_builtin_arm set, so the builtin surface
     serves it -- Array#each_slice(2) { } beside a user each_slice(n, &blk)
     -- exactly as in a program with no user class of that name. The block is
     spliced into this arm as it is into the user arms; only one arm runs. */
  if (!emitted_default && g_prbd_skip != id && g_n_argov + 1 <= MAX_ARG_OVERRIDE) {
    /* on the heap: the probe may longjmp back after the emitter wrote to it */
    Buf *ab = calloc(1, sizeof *ab);
    int slot = view_bind(recv, "_t%d", trecv);
    int va = view_push_arm(g_pd_skip, id, 1);
    /* under the silent probe: a builtin emitter that refuses the call drops
       this arm, not the build */
    Buf *sv_gpre = g_pre;
    int sv_probe = g_unsup_probe, sv_open_defaults = g_open_defaults;
    int sv_nren = g_nren, sv_block = g_block_id;
    ConvHold *sv_hold = g_conv_hold;
    jmp_buf sv_jb; memcpy(sv_jb, g_unsup_recover, sizeof(jmp_buf));
    volatile int ok = 1;
    EmitUnitState *sv_state = emit_state_snapshot();
    g_unsup_probe = 1;
    if (setjmp(g_unsup_recover) == 0) emit_stmt(c, id, ab, indent + 1);
    else ok = 0;
    emit_state_release(sv_state, !ok);
    memcpy(g_unsup_recover, sv_jb, sizeof(jmp_buf));
    g_conv_hold = sv_hold; g_open_defaults = sv_open_defaults;
    g_nren = sv_nren; g_block_id = sv_block;
    g_unsup_probe = sv_probe; g_pre = sv_gpre;
    view_pop(c, va);
    view_unbind(slot);
    char rtok[300];
    snprintf(rtok, sizeof rtok, "sp_nomethod_msg(\"%s\"", name);
    if (ok && ab->p && !strstr(ab->p, rtok)) {
      emit_indent(&sw, indent); buf_puts(&sw, "default: {\n");
      buf_puts(&sw, ab->p);
      emit_indent(&sw, indent + 1); buf_puts(&sw, "break;\n");
      emit_indent(&sw, indent); buf_puts(&sw, "}\n");
      emitted_default = 1;
      pa_answer = 2;
    }
    free(ab->p); free(ab);
  }
  if (!emitted_default) {
    emit_indent(&sw, indent); buf_puts(&sw, "default: ");
    buf_printf(&sw, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;\n", name, trecv);
  }
  emit_indent(&sw, indent); buf_puts(&sw, "}\n");
  if (g_plan_check) {
    pa_resume(pa_frame);
    if (pa_offered) pa_observe(PA_TRIAL, PA_KEY_TRIAL + PT_BD_DEFAULT, -1, TY_UNKNOWN, pa_answer);
    pa_end(c, pa_frame, cplan_poly_block(c, id));
  }
  /* Committed only now: every arm emitted a body, so the switch is complete
     and the caller's buffer sees it for the first time here. */
  if (sw.p) buf_puts(b, sw.p);
  free(sw.p);
  return 1;
}

/* The same dispatch where the call's value is used (`def f(io) =
   io.each_line { }`), for a name a native class defines in Ruby
   (StringIO#each_line): its arms come from no binding, so the value-position
   poly dispatch had none. Those iterators answer their receiver, so the
   switch runs as a statement over the evaluated receiver, which is the
   value. */
int emit_poly_recv_block_value(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0 || nt_ref(nt, id, "block") < 0 || comp_ntype(c, id) != TY_POLY ||
      comp_ntype(c, recv) != TY_POLY || g_n_argov + 1 > MAX_ARG_OVERRIDE) return 0;
  if (!sp_streq(name, "each_line") && !sp_streq(name, "each_char") && !sp_streq(name, "each_byte"))
    return 0;
  int native = 0;
  for (int k = 0; k < c->nclasses && !native; k++)
    if (c->classes[k].is_native_class && c->classes[k].instantiated &&
        comp_method_in_chain(c, k, name, NULL) >= 0) native = 1;
  if (!native) return 0;
  int tr = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_boxed(c, recv, &rb);
  Buf sb; memset(&sb, 0, sizeof sb);
  int slot = view_bind(recv, "_t%d", tr);
  int ok = emit_poly_recv_block_dispatch(c, id, &sb, 1);
  view_unbind(g_n_argov - 1);
  if (ok && sb.p)
    buf_printf(b, "({ sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n%s _t%d; })",
               tr, rb.p ? rb.p : "sp_box_nil()", tr, sb.p, tr);
  free(rb.p); free(sb.p);
  return ok;
}

/* Does this call target a user method that yields (so it has no standalone C
   function -- it is only ever inlined at its call sites)? A compact echo of
   emit_inline_call_x's own method resolution. (#2948) */
static int call_targets_yielding_method(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  /* the call's plan where its form says the target (inline_target_plan);
     otherwise, and under --plan-check as the assertion, the echo below */
  InlineTarget it;
  int served = inline_target_plan(c, id, name, recv, 1, &it);
  if (g_plan_check && served) cplan_served("yielding-target");
  if (served && !g_plan_check) return it.mi >= 0 && c->scopes[it.mi].yields;
  int mi = -1;
  if (recv < 0) {
    /* emit_inline_call_x's order: the instance_exec class, then the
       enclosing class chain (its class methods first in a class method),
       then the class body, and only then the top level. Looking at the top
       level first took a top-level def that does not yield for a class's
       own yielding method of the same name, so the call was left to a plain
       call of a function a yielding method never has; and in a class method
       the instance chain was asked before the class methods. */
    Scope *encl = comp_scope_of(c, id);
    if (g_ie_class_id >= 0) mi = comp_method_in_chain(c, g_ie_class_id, name, NULL);
    if (mi < 0 && encl && encl->class_id >= 0) {
      if (encl->is_cmethod) mi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      if (mi < 0) mi = comp_method_in_chain(c, encl->class_id, name, NULL);
    }
    else if (mi < 0 && g_class_body_id >= 0)
      mi = comp_cmethod_in_chain(c, g_class_body_id, name, NULL);
    if (mi < 0) mi = comp_method_index(c, name);
    /* A bare call to a method of a module included at the top level, which
       emit_inline_call_x splices under the same condition. Missed here, a
       block whose tail is such a call took the statement form, and the
       splice read for the block's value was void: `check { raises? { raise
       E } }` under `include T` gave "invalid argument type 'void' to unary
       expression" at check's `unless yield`. */
    if (mi < 0) {
      int imi = comp_included_method_index(c, name, id);
      if (imi >= 0 && !scope_uses_ivars(c, imi)) mi = imi;
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
  if (served && it.mi != mi)
    fprintf(stderr, "plan-check: cplan-conflict: yielding-target node %d %s: plan %d, lookup %d\n",
            id, name, it.mi, mi);
  if (!served && g_plan_check && mi >= 0)
    fprintf(stderr, "plan-check: cplan-fallback: yielding-target node %d %s\n", id, name);
  if (served) mi = it.mi;
  return mi >= 0 && c->scopes[mi].yields;
}

int emit_inline_expr(Compiler *c, int id, Buf *b) {
  /* only when a value is actually produced (scalar return) */
  TyKind rt = comp_ntype(c, id);
  if (!is_scalar_ret(rt)) {
    /* A block that always raises leaves the call with no value type at all,
       but the call itself still inlines: hold the (dead) result boxed so the
       yielding method needs no standalone function (#3716). A call with no
       block has none to fall back to either: a method whose only value is
       its `return nil unless block_given?` called a symbol never emitted
       (#5097). */
    if ((rt == TY_VOID || rt == TY_UNKNOWN || rt == TY_NIL) &&
        call_targets_yielding_method(c, id)) {
      int v = view_push(c, id, TY_POLY);
      int ok = emit_inline_call_x(c, id, b, g_indent + 1, 1);
      view_pop(c, v);
      if (ok) return 1;
    }
    /* a block-driving call to a yielding method that can't be inlined here (a
       non-scalar result) has no standalone function to fall back to: the plain
       call would emit an undefined symbol (invalid C). Fail loud (#2948). */
    if (nt_ref(c->nt, id, "block") >= 0 && call_targets_yielding_method(c, id) &&
        !block_call_takes_class_dispatch(c, id))
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
   nested inside a deeper loop/block/def (which would own it instead)? With
   `redo` >= 0, that particular RedoNode. */
static int subtree_has_own_redo_ex(const NodeTable *nt, int id, int redo) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "RedoNode")) return redo < 0 || id == redo;
  /* a `redo` in a block a yield here splices belongs to that splice, which
     carries its own label (emit_block_invoke), not to this loop */
  /* nested scope/loop boundaries: a redo inside binds to that inner loop */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;  /* nested iteration */
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_redo_ex(nt, nt_ref_at(nt, id, i), redo)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_redo_ex(nt, ids[k], redo)) return 1;
  }
  return 0;
}
int subtree_has_own_redo(const NodeTable *nt, int id) { return subtree_has_own_redo_ex(nt, id, -1); }
int subtree_owns_redo(const NodeTable *nt, int body, int redo) {
  return redo >= 0 && subtree_has_own_redo_ex(nt, body, redo);
}

/* Does the subtree contain a `next` that belongs to THIS block, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? Same
   ownership rule as subtree_has_own_redo. With `next` >= 0 the answer is for
   that one node, and it is also looked for where a nested iteration is
   evaluated in this block: the receiver, the arguments and a `&blk` of a
   call with a block, and the collection of a `for`. The any-`next` form
   does not look there: its callers pick by the answer how a block spliced
   in place is written, and emit_fallback_block_value leaves the leading
   statements out of a block that has one. */
static int subtree_has_own_next_ex(const NodeTable *nt, int id, int next) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "NextNode")) return next < 0 || id == next;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "ForNode"))
    return next >= 0 && subtree_has_own_next_ex(nt, nt_ref(nt, id, "collection"), next);
  int blk = sp_streq(ty, "CallNode") ? nt_ref(nt, id, "block") : -1;
  if (blk >= 0) {
    if (next < 0) return 0;
    const char *bty = nt_type(nt, blk);
    return subtree_has_own_next_ex(nt, nt_ref(nt, id, "receiver"), next) ||
           subtree_has_own_next_ex(nt, nt_ref(nt, id, "arguments"), next) ||
           (bty && sp_streq(bty, "BlockArgumentNode") && subtree_has_own_next_ex(nt, blk, next));
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_next_ex(nt, nt_ref_at(nt, id, i), next)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_next_ex(nt, ids[k], next)) return 1;
  }
  return 0;
}
int subtree_has_own_next(const NodeTable *nt, int id) { return subtree_has_own_next_ex(nt, id, -1); }
int subtree_owns_next(const NodeTable *nt, int body, int next) {
  return next >= 0 && subtree_has_own_next_ex(nt, body, next);
}

/* Mark every `next` written where the value of `id` is: `id` itself, the
   last statement of a sequence, an arm of an `if`, `unless`, `case` or
   `begin`, the right of an `and` or `or`. An `ensure` clause, a condition
   and every other operand are not: their value is not the node's. */
static void mark_value_nexts(const NodeTable *nt, int id, char *mark) {
  if (id < 0 || id >= nt->count) return;
  int n = 0; const int *a;
  switch (nt_kind(nt, id)) {
  case NK_NextNode: mark[id] = 1; return;
  case NK_StatementsNode:
    a = nt_arr(nt, id, "body", &n);
    if (n > 0) mark_value_nexts(nt, a[n - 1], mark);
    return;
  case NK_ParenthesesNode: mark_value_nexts(nt, nt_ref(nt, id, "body"), mark); return;
  case NK_IfNode:
    mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "subsequent"), mark);
    return;
  case NK_UnlessNode:
    mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "else_clause"), mark);
    return;
  case NK_ElseNode: case NK_InNode:
    mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    return;
  case NK_CaseNode: case NK_CaseMatchNode:
    a = nt_arr(nt, id, "conditions", &n);
    for (int i = 0; i < n; i++) mark_value_nexts(nt, a[i], mark);
    mark_value_nexts(nt, nt_ref(nt, id, "else_clause"), mark);
    return;
  case NK_BeginNode:
    /* with an `else`, the body's last statement is not the begin's value */
    if (nt_ref(nt, id, "else_clause") < 0) mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "rescue_clause"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "else_clause"), mark);
    return;
  case NK_RescueNode:
    mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "subsequent"), mark);
    return;
  case NK_RescueModifierNode:
    mark_value_nexts(nt, nt_ref(nt, id, "expression"), mark);
    mark_value_nexts(nt, nt_ref(nt, id, "rescue_expression"), mark);
    return;
  case NK_AndNode: case NK_OrNode: mark_value_nexts(nt, nt_ref(nt, id, "right"), mark); return;
  default:
    /* a `when` has no kind of its own */
    if (nt_type(nt, id) && sp_streq(nt_type(nt, id), "WhenNode"))
      mark_value_nexts(nt, nt_ref(nt, id, "statements"), mark);
    return;
  }
}

/* Is this `next` the value of the block it leaves: written where the block's
   last expression is, so that leaving the block with v and answering v are
   the same thing? The expression emitter takes such a `next v` as v (#3026).
   Any other `next` an expression holds -- an operand, an argument, the value
   of an assignment, `c && (next)` ahead of more statements -- has to leave
   the block from where it is. The marks are built once per node table. */
int next_is_block_value(Compiler *c, int next) {
  static char *mark; static const NodeTable *mark_nt; static int mark_n = -1; static unsigned mark_ver;
  const NodeTable *nt = c->nt;
  if (!mark || mark_nt != nt || mark_n != nt->count || mark_ver != nt->version) {
    free(mark);
    mark = calloc((size_t)nt->count + 1, 1);
    if (!mark) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    NT_FOREACH_KIND(nt, NK_BlockNode, blk) mark_value_nexts(nt, nt_ref(nt, blk, "body"), mark);
    NT_FOREACH_KIND(nt, NK_LambdaNode, lam) mark_value_nexts(nt, nt_ref(nt, lam, "body"), mark);
    mark_nt = nt; mark_n = nt->count; mark_ver = nt->version;
  }
  return next >= 0 && next < nt->count && mark[next];
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
  /* A `next <v>` in this body leaves THIS loop's iteration, so the value slot
     an enclosing collecting block opened (g_ie_next_var, a `then` / `map` /
     inject body's destination) is not its target: left set, the inner next
     assigned the outer block's slot before its continue, which built only
     when the two kinds agreed and was then right by accident, the tail
     overwriting it (#4748). The slot's kind goes with it. g_ie_res_poly
     stays: the while-as-value emitter sets it for its own `break` value, which
     the break emitter reads inside this body. */
  const char *sv_nxv = g_ie_next_var; TyKind sv_nxt = g_ie_next_ty;
  g_ie_next_var = NULL; g_ie_next_ty = TY_UNKNOWN;
  int has_redo = subtree_has_own_redo(c->nt, body);
  int lbl = 0;
  if (has_redo) {
    lbl = ++g_tmp;
    if (g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0])) {
      g_redo_owner[g_redo_depth] = body;
      g_redo_stack[g_redo_depth++] = lbl;
    }
    else has_redo = 0;
  }
  /* a block body's label goes after its setup, where emit_stmts puts it */
  if (has_redo && block_of_body(c, body) >= 0) g_redo_pending = lbl;
  else if (has_redo) { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", lbl); }
  /* Safepoint poll at the loop back-edge: a threaded program's worker checks
     here whether a GC stop-the-world wants it to park, so a long-running loop
     cannot starve the collector. SP_SAFEPOINT_POLL() (sp_sched.h) is a relaxed
     atomic load of sp_safepoint_flag under SP_THREADS -- the collector writes
     the flag from another thread -- and a plain load otherwise. Emitted only
     when the program uses threads; a non-threaded program is byte-identical.
     At N=1 the flag is never set -- a predicted-not-taken load. */
  /* A loop whose condition already polls (hc_loop_polls_in_cond) takes them
     there instead: see emit_while. */
  int polls_here = !g_loop_polls_in_cond;
  g_loop_polls_in_cond = 0;
  if (polls_here && g_uses_threads) { emit_indent(b, indent); buf_printf(b, "if (SP_UNLIKELY(SP_SAFEPOINT_POLL())) sp_safepoint()%s;\n", hc_mark()); }
  /* and for pending finalizers (sp_gc.h), when the program defines any. A
     finalizer is Ruby code that may resize an array this loop has cached, so
     the cache reloads after one runs, as after a safepoint. */
  if (polls_here && g_uses_finalizers) { emit_indent(b, indent); buf_printf(b, "if (SP_UNLIKELY(SP_ATOMIC_LOAD(&sp_fin_pending_flag, __ATOMIC_RELAXED))) sp_fin_run_pending()%s;\n", hc_mark()); }
  emit_stmts(c, body, b, indent);
  if (has_redo) g_redo_depth--;
  g_c_loop_depth--;
  g_ie_next_var = sv_nxv; g_ie_next_ty = sv_nxt;
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
    buf_printf(g_pre, " _t%d = %s;\n", tres, default_value_from_compiler(c, rett));
    if (needs_root(rett)) { emit_indent(g_pre, g_indent); emit_gc_root_tmp(c, rett, tres, g_pre); buf_puts(g_pre, "\n"); }
  }

  /* pin the block param to the receiver type if inference widened it */
  Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
  TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
  int use_shadow = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
  /* A boxed receiver over a SCALAR block parameter keeps the parameter typed
     and unboxes the value into it, rather than shadowing the parameter as
     boxed: the body was inferred against the scalar and reads it as one
     (`v.negative?` on an sp_int), and the shadow's retype does not reach the
     cached node types, so the two disagreed and the C did not build. Under
     --int-overflow=promote every Integer local is boxed while a block
     parameter is not, which is where this shape lives (#4730). The unbox
     raises past the word, as the other typed sinks do. */
  /* A boxed nil is the parameter's nil sentinel, as any narrowing unbox
     keeps it: sp_poly_to_i read it as 0, so under promote `x = nil;
     x.then { |y| y.nil? }` answered false. */
  const char *unbox = NULL;
  if (use_shadow && et == TY_POLY && tsaved0 == TY_INT) unbox = "sp_poly_to_i_or_nil";
  else if (use_shadow && et == TY_POLY && tsaved0 == TY_FLOAT) unbox = "sp_poly_to_f_or_nil";
  if (unbox) use_shadow = 0;
  int din = g_indent;
  if (use_shadow) {
    tlv0->type = et;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    din = g_indent + 1;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = _t%d;\n", p0, tr);
    if (!(g_cap_struct && g_cap_names && nameset_has(g_cap_names, p0)))
      emit_cell_shadow_store(c, tsc, p0, g_pre, din);
  }
  else if (p0) {
    emit_indent(g_pre, g_indent);
    if (unbox) buf_printf(g_pre, "lv_%s = %s(_t%d);\n", p0, unbox, tr);
    else buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tr);
    /* The block's parameter may be CELLED -- something inside the body needs it
       as a proc's capture, which here means a dispatch arm that hands the inner
       block over as a proc rather than splicing it. The arms that DO splice read
       the cell, and nothing had written it: the binding above fills the plain
       slot only, so the cell kept its NULL and `body << chunk` dereferenced it
       (#4418, SIGSEGV; a String receiver made it a nil instead). The publish
       belongs at the binding, which is the one point every arm passes through --
       the capture fill, where it used to be, is only reached when a proc is
       actually built. */
    /* Inlined inside a real proc function the cell is reachable only through
       the capture struct and is not this frame's to name, which is the guard
       the loop emitters carry for the same publish. */
    if (!(g_cap_struct && g_cap_names && nameset_has(g_cap_names, p0)))
      emit_cell_shadow_store(c, tsc, p0, g_pre, g_indent);
  }

  int sv = g_indent; g_indent = din;
  if (is_then) {
    /* The body goes through the next-aware substrate: `next <v>` inside a
       `then` block leaves the block WITH that value, and this splice has no
       loop of its own, so a bare `continue` was both value-dropping and
       invalid C (#3978). The do{}while(0) wrapper it emits makes the
       continue exit exactly this block. */
    char destbuf[24]; snprintf(destbuf, sizeof destbuf, "_t%d", tres);
    /* The slot is the poly array a `next` arm of another kind widened the
       value to while the tail is still typed: say so, or the substrate keys
       the arms and the tail on the tail's kind (#4747). */
    TyKind tailt = comp_ntype(c, bb[bn - 1]);
    g_bv_dest_ty = (rett == TY_POLY_ARRAY && tailt != rett && array_to_poly_fn(tailt)) ? rett : TY_UNKNOWN;
    emit_block_value_into(c, block, destbuf, rett == TY_POLY, din);
  }
  else {
    /* tap discards the block's value, but a `next` still leaves the block --
       same wrapper, no destination. */
    const char *sv_nxv = g_ie_next_var;
    g_ie_next_var = NULL;
    int sv_lexcw = g_loop_exc_base, sv_lensw = g_loop_ensure_base;
    g_loop_exc_base = g_exc_frame_depth; g_loop_ensure_base = g_ensure_depth;
    g_c_loop_depth++;
    emit_indent(g_pre, din); buf_puts(g_pre, "do {\n");
    int bi = din + 1; g_indent = bi;
    emit_iter_step_body(c, block, g_pre, bi);
    g_indent = din;
    emit_indent(g_pre, din); buf_puts(g_pre, "} while (0);\n");
    g_c_loop_depth--;
    g_loop_exc_base = sv_lexcw; g_loop_ensure_base = sv_lensw;
    g_ie_next_var = sv_nxv;
  }
  g_indent = sv;
  if (use_shadow) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
  if (tlv0) tlv0->type = tsaved0;

  /* a receiver read as the shared handle (a block parameter that appends to
     it is the handle, promote_shared_stored_strings): tap's value is that
     String, read as one */
  if (is_tap && et == TY_STRBUF && comp_ntype(c, id) == TY_STRING)
    buf_printf(b, "(_t%d ? sp_String_cstr(_t%d) : NULL)", tr, tr);
  else buf_printf(b, "_t%d", is_tap ? tr : tres);
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
  switch (pt) {
  case TY_POLY:              buf_puts(b, "sp_box_nil()"); break;
  case TY_INT: case TY_BOOL: buf_puts(b, "SP_INT_NIL"); break;
  case TY_FLOAT:             buf_puts(b, "sp_float_nil()"); break;
  case TY_SYMBOL:            buf_puts(b, "((sp_sym)-1)"); break;
  default:                   buf_puts(b, "NULL"); break;  /* string / heap ptr */
  }
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
  /* the kept elements carry the receiver's nils */
  if (sp_streq(k, "Int") || sp_streq(k, "Float")) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray_nil_from(_t%d, _t%d);\n", k, tr, ta);
  }
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

/* A receiver the value of an iteration answers is read twice, once under the
   `to_a` hop the call walks and once as the answer, and one that acts
   (`(z = lit(1); 1..2).each_entry { }`) ran twice. Bind it once, ahead of the
   statement where the hop's own receiver temps go, and push an override so
   both reads take the binding; answers 1 when the caller must pop it. */
int iter_recv_bind_once(Compiler *c, int node) {
  if (!g_pre || g_n_argov >= MAX_ARG_OVERRIDE || !subtree_has_side_effect(c, node)) return 0;
  TyKind ot = comp_ntype(c, node);
  if (ot == TY_UNKNOWN) return 0;
  int t = ++g_tmp;
  Buf ob = expr_buf(c, node);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, ot, g_pre);
  buf_printf(g_pre, " _t%d = %s;", t, ob.p ? ob.p : "");
  free(ob.p);
  if (needs_root(ot)) buf_printf(g_pre, ot == TY_POLY ? " SP_GC_ROOT_RBVAL(_t%d);" : " SP_GC_ROOT(_t%d);", t);
  buf_puts(g_pre, "\n");
  view_bind(node, "_t%d", t);
  return 1;
}

/* Set by emit_iter_value_expr around the Enumerator walk in emit_iteration_stmt: the temp
   that receives what `e.each { }` answers. */
static int g_enum_walk_res = 0;
void set_enum_walk_result(int tmp) { g_enum_walk_res = tmp; }

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
  /* `e.each { }` over an Enumerator answers what its underlying each does:
     a generator's body value, a materialized one's collection. The walk
     runs once; the `to_a` hop in front of it is never evaluated. */
  if ((sp_streq(name, "each") || sp_streq(name, "each_with_index")) &&
      nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "enum_each_wrap") &&
      nt_ref(nt, recv, "receiver") >= 0 &&
      comp_ntype(c, nt_ref(nt, recv, "receiver")) == TY_ENUMERATOR &&
      comp_ntype(c, id) == TY_POLY) {
    int tres = ++g_tmp;
    Buf wb; memset(&wb, 0, sizeof wb);
    g_enum_walk_res = tres;
    int wok = emit_iteration_stmt(c, id, &wb, 0);
    g_enum_walk_res = 0;
    if (!wok) { free(wb.p); return 0; }
    buf_printf(b, "({ sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); %s _t%d; })",
               tres, tres, wb.p ? wb.p : "", tres);
    free(wb.p);
    return 1;
  }
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
  view_bind(recv, "_t%d", ta);
  Buf body; memset(&body, 0, sizeof body);
  int ok = emit_iteration_stmt(c, id, &body, 0);
  view_unbind(g_n_argov - 1);
  if (!ok) { free(body.p); return 0; }
  /* The original receiver is read twice, once under the hop and once as the
     answer */
  int to = objn >= 0 ? iter_recv_bind_once(c, objn) : 0;
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
  if (to) view_unbind(g_n_argov - 1);
  return 1;
}

static void emit_filter_body(Compiler *c, int body, int tnv, int tk, int is_rej, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  const char *sv_nx = g_ie_next_var; int sv_poly = g_ie_res_poly; TyKind sv_nty = g_ie_next_ty;
  int sv_lexc = g_loop_exc_base, sv_lens = g_loop_ensure_base;
  char nxbuf[32]; snprintf(nxbuf, sizeof nxbuf, "_t%d", tnv);
  g_ie_next_var = nxbuf; g_ie_res_poly = 1; g_ie_next_ty = TY_UNKNOWN;
  g_loop_exc_base = g_exc_frame_depth; g_loop_ensure_base = g_ensure_depth;
  g_c_loop_depth++;
  /* the step's setup: locals fresh, and a redo's label after them */
  if (block_of_body(c, body) >= 0) emit_block_locals_reset(c, block_of_body(c, body), b, indent + 1);
  int rd_lbl = emit_iter_step_stmts(c, body, b, indent + 1, NULL);
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
  if (rd_lbl) g_redo_depth--;
  g_c_loop_depth--;
  g_loop_exc_base = sv_lexc; g_loop_ensure_base = sv_lens;
  g_ie_next_var = sv_nx; g_ie_res_poly = sv_poly; g_ie_next_ty = sv_nty;
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
  emit_filter_body(c, body, tnv, tk, is_rej, b, indent);
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
      buf_printf(b, " _retv%d = %s;\n", eid, default_value_from_compiler(c, g_ret_type));
    }
    g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth, g_ret_type };
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
  emit_filter_body(c, body, tnv, tk, is_rej, b, li);
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
    if (has_retval && g_ret_type == TY_POLY && proc_ret_slot())
      buf_printf(b, "if (_retf%d) { %s = _retv%d; return 0; }\n", eid, proc_ret_slot(), eid);
    else emit_retf_return(eid, has_retval, b);
    emit_indent(b, indent);
    buf_printf(b, "if (_excf%d) { sp_pending_exc_obj = _excobj%d; sp_raise_cls(_exccls%d, _excmsg%d); }\n", eid, eid, eid, eid);
  }
  if (flv) flv->type = fsaved;
  *tr = t; *torig = to; *twp = tw;
  return 1;
}

/* Bind one zip block param. A poly slot takes a boxed source; a concrete
   slot takes a typed array read as-is. The remaining case is a concrete
   slot fed by a poly operand (`ai.zip(tj)` where `ai` is Array[Float] and
   `tj` is only an array at run time): the read is an sp_RbVal, and the
   param was still inferred as the receiver's element type. Assigning the
   box into that slot does not compile. Narrow it, keeping nil as the
   slot's own nil so a shorter operand still yields nil. */
static void emit_zip_block_param(Compiler *c, TyKind slot, TyKind src_ty,
                                 const char *src, Buf *b) {
  if (slot == TY_POLY && src_ty != TY_POLY) emit_boxed_text(c, src_ty, src, b);
  else if (src_ty == TY_POLY && slot != TY_POLY && slot != TY_UNKNOWN)
    emit_unbox_nilable_text(c, slot, src, b);
  else buf_puts(b, src);
}

static void emit_poly_auto_splat(Compiler *c, int block, int telem, Buf *b, int indent) {
  Scope *bs = comp_scope_of(c, block);
  int npp = 0; while (block_param_name(c, block, npp)) npp++;
  emit_indent(b, indent + 1);
  buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) {\n", telem, telem);
  for (int pj = 0; pj < npp; pj++) {
    const char *pnj = block_param_name(c, block, pj);
    if (!pnj) break;
    LocalVar *plv = bs ? scope_local(bs, pnj) : NULL;
    TyKind pt = plv ? plv->type : TY_POLY;
    char src[64]; snprintf(src, sizeof src, "sp_poly_arr_get(_t%d, %d)", telem, pj);
    emit_indent(b, indent + 2);
    emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
  }
  emit_indent(b, indent + 1); buf_puts(b, "}\nelse {\n");
  for (int pj = 0; pj < npp; pj++) {
    const char *pnj = block_param_name(c, block, pj);
    if (!pnj) break;
    LocalVar *plv = bs ? scope_local(bs, pnj) : NULL;
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

static int emit_shadow_save(Compiler *c, TyKind t, const char *name, Buf *b, int indent) {
  int ts = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, t, &ot);
  emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts, name); free(ot.p);
  return ts;
}

static int emit_iteration_stmt_body(Compiler *c, int id, Buf *b, int indent);
int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  return emit_ivar_nil_guarded(c, id, b, indent, emit_iteration_stmt_body);
}
/* Block parameter pj of an each_slice / each_cons row: element pj of the
   row that starts at `_t<ti>` of the array `_t<ta>` (of kind k and type rt)
   and is `_t<tn>` long (`lit` when the size is written out, else 0). A
   parameter past the row is nil, where it read the next row's element; the
   array's end is a missed read, nil already. An element of a boxed row
   into a scalar parameter keeps its nil as the sentinel: under
   --int-overflow=promote the row is boxed while the parameter is not, and
   the plain assignment did not build. */
static void emit_row_param_bind(Compiler *c, int block, int pj, const char *k, TyKind rt,
                                int ta, int ti, int tn, long long lit, int indent, Buf *b) {
  const char *pn = block_param_name(c, block, pj);
  if (!pn) return;
  const char *rpn = rename_local(pn);
  Scope *sc = comp_scope_of(c, block);
  LocalVar *lv = sc ? scope_local(sc, rpn) : NULL;
  TyKind pt = lv ? lv->type : TY_UNKNOWN;
  char get[200];
  if (rt == TY_POLY_ARRAY && (pt == TY_INT || pt == TY_FLOAT))
    snprintf(get, sizeof get, "%s(sp_PolyArray_get(_t%d, _t%d + %d))",
             pt == TY_INT ? "sp_poly_to_i_or_nil" : "sp_poly_to_f_or_nil", ta, ti, pj);
  else snprintf(get, sizeof get, "sp_%sArray_get(_t%d, _t%d + %d)", k, ta, ti, pj);
  const char *nil = pt == TY_UNKNOWN ? NULL : nil_value(pt) ? nil_value(pt) : default_value_from_compiler(c, pt);
  emit_indent(b, indent);
  if (pj == 0 || pj < lit || !nil) buf_printf(b, "lv_%s = %s;\n", rpn, get);
  else buf_printf(b, "lv_%s = %d < _t%d ? %s : %s;\n", rpn, pj, tn, get, nil);
}

static int emit_iteration_stmt_body(Compiler *c, int id, Buf *b, int indent) {
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

  /* `e.each { }` on an Enumerator, as a statement: the analysis routed it
     through a `to_a` it marked, which never ends for an endless or generator
     Enumerator ([1, 2].cycle, Enumerator.new { loop { } }). Pull the elements
     one at a time instead, from a walk of its own (the Enumerator's #next
     cursor is not touched), so a `break` in the block ends it. */
  int ewi_walk = sp_streq(name, "each_with_index");
  if ((sp_streq(name, "each") || ewi_walk) && recv >= 0 && nt_kind(nt, recv) == NK_CallNode &&
      nt_str(nt, recv, "enum_each_wrap") && nt_type(nt, block) &&
      sp_streq(nt_type(nt, block), "BlockNode")) {
    int er = nt_ref(nt, recv, "receiver");
    if (er >= 0 && comp_ntype(c, er) == TY_ENUMERATOR) {
      int tres = g_enum_walk_res; g_enum_walk_res = 0;
      const char *q0 = block_param_name(c, block, 0), *q1 = block_param_name(c, block, 1);
      Scope *bsc = comp_scope_of(c, block);
      int body = nt_ref(nt, block, "body");
      int te = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, ti2 = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, er, &rb);
      emit_indent(b, indent);
      buf_printf(b, "{ sp_Enumerator *_t%d = %s; SP_GC_ROOT(_t%d);\n", te, rb.p ? rb.p : "NULL", te);
      free(rb.p);
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_Fiber *_t%d = (_t%d && _t%d->gen) ? sp_Fiber_new(_t%d->gen) : NULL; SP_GC_ROOT(_t%d);\n",
                 tf, te, te, te, tf);
      emit_indent(b, indent + 1);
      buf_printf(b, "if (_t%d && _t%d->gen_cap) { sp_gc_wb((void *)_t%d); _t%d->user_data = _t%d->gen_cap; }\n",
                 tf, te, tf, tf, te);
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_int _t%d = 0, _t%d = 0; sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", ti, ti2, tv, tv);
      emit_indent(b, indent + 1);
      buf_puts(b, "for (;;) {\n");
      emit_indent(b, indent + 2);
      if (tres)
        buf_printf(b, "if (_t%d) { if (!sp_Fiber_alive(_t%d)) break; _t%d = sp_Fiber_resume(_t%d, sp_box_nil()); if (!sp_Fiber_alive(_t%d)) { _t%d = _t%d; break; } }\n",
                   tf, tf, tv, tf, tf, tres, tv);
      else
        buf_printf(b, "if (_t%d) { if (!sp_Fiber_alive(_t%d)) break; _t%d = sp_Fiber_resume(_t%d, sp_box_nil()); if (!sp_Fiber_alive(_t%d)) break; }\n",
                   tf, tf, tv, tf, tf);
      emit_indent(b, indent + 2);
      buf_printf(b, "else { if (_t%d && _t%d->endless && _t%d->items && _t%d->items->len > 0 && _t%d >= _t%d->items->len) _t%d = 0;"
                    " if (!_t%d || !_t%d->items || _t%d >= _t%d->items->len) break; _t%d = _t%d->items->data[_t%d++]; }\n",
                 te, te, te, te, ti, te, ti, te, te, ti, te, tv, te, ti);
      emit_indent(b, indent + 2);
      buf_printf(b, "_t%d++;\n", ti2);   /* the element's index, for each_with_index */
      /* one parameter binds the element; two autosplat an array element,
         or for each_with_index take the element and its index */
      const char *qs[2] = { q0, q1 };
      /* a lone `|x|` / `|*r|` over an Enumerator yielding two values
         (desugar's enum_hop_yield_view) */
      const char *view = ewi_walk ? NULL : nt_str(nt, recv, "enum_yield_view");
      for (int k = 0; k < 2; k++) {
        if (!qs[k]) continue;
        LocalVar *lvq = bsc ? scope_local(bsc, qs[k]) : NULL;
        if (!lvq) continue;
        char vx[96];
        if (view && sp_streq(view, "first"))
          snprintf(vx, sizeof vx, "sp_yielded_first(_t%d->yields_pair, _t%d)", te, tv);
        else if (view)
          snprintf(vx, sizeof vx, "sp_box_poly_array(sp_yielded_args(_t%d->yields_pair, _t%d))", te, tv);
        else if (ewi_walk && k == 1) snprintf(vx, sizeof vx, "sp_box_int(_t%d - 1)", ti2);
        else if (q1 && !ewi_walk) snprintf(vx, sizeof vx, "sp_poly_massign_get(_t%d, %d)", tv, k);
        else snprintf(vx, sizeof vx, "_t%d", tv);
        emit_indent(b, indent + 2);
        buf_printf(b, "lv_%s = ", rename_local(qs[k]));
        if (lvq->type == TY_POLY || lvq->type == TY_UNKNOWN) buf_puts(b, vx);
        else emit_unbox_text(c, lvq->type, vx, b);
        buf_puts(b, ";\n");
      }
      emit_loop_body(c, body, b, indent + 2);
      emit_indent(b, indent + 1);
      buf_puts(b, "}\n");
      if (tres) {
        emit_indent(b, indent + 1);
        buf_printf(b, "if (!_t%d) _t%d = sp_enum_walk_result(_t%d);\n", tf, tres, te);
      }
      emit_indent(b, indent);
      buf_puts(b, "}\n");
      return 1;
    }
  }

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
    }
    /* An anonymous `&` inside an inlined body resolved to the CALLER's
       literal block: the loops below read the block's params and body off
       `block`, so it has to be that node, not the BlockArgumentNode -- with
       the latter every loop ran with an empty body, silently doing nothing
       (`def each(&) = @items.each(&)`, #4618). A named `&blk` never reached
       this because desugar_value_callable_forwards rewrote it first. */
    if (rfb != block) block = rfb; }


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
    buf_puts(b, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) { sp_pending_exc_obj = sp_exc_obj[sp_exc_top]; sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]); }\n");
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
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_INT_ARRAY);
    int done = emit_iteration_stmt(c, id, b, indent);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
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
    /* A Float range walks its steps as CRuby's ruby_float_step yields them
       (sp_float_step_at, the count from sp_float_step_size), one at a time:
       an endless one (1.5..) never finishes, so materializing it first never
       reached the block (or its break). A beginless one has no first step. */
    if (rt == TY_FLOAT_RANGE && sargc == 1) {
      int tr = ++g_tmp, ts2 = ++g_tmp, tn2 = ++g_tmp, ti2 = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "sp_float _t%d = ", ts2); emit_float_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "sp_float _t%d = sp_frange_step_count(_t%d, _t%d);\n", tn2, tr, ts2);
      emit_indent(b, indent);
      buf_printf(b, "for (sp_int _t%d = 0; (sp_float)_t%d < _t%d; _t%d++) {\n", ti2, ti2, tn2, ti2);
      char elem[96]; snprintf(elem, sizeof elem, "sp_float_step_at(_t%d.first, _t%d.last, _t%d, _t%d)", tr, tr, ts2, ti2);
      if (p0) emit_iter_param_assign(c, block, p0_orig, p0, TY_FLOAT, elem, b, indent + 1);
      { int rs_np = 0; while (block_param_name(c, block, rs_np)) rs_np++;
        emit_iter_bind_rest(c, block, rs_np, TY_FLOAT, elem, b, indent + 1); }
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
      buf_printf(&ab, "; sp_FloatArray_from_step(_t%d.first, _t%d.last, ", trf, trf); emit_float_expr(c, sargv[0], &ab);
      buf_printf(&ab, ", _t%d.excl); })", trf);
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
  /* a Bignum receiver walks the same boxed sequence: it does not fit the
     sp_int loop below, and it had no arm at all (#4779) */
  if (sp_streq(name, "step") && (rt == TY_RATIONAL || rt == TY_BIGINT)) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    int tc = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, td = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = ", tc);
    if (rt == TY_RATIONAL) { buf_puts(b, "sp_box_rational("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else emit_boxed(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);\n", tc);
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
    /* the limit and the step through the Float slot's conversion: a BOXED
       one (an Integer local under --int-overflow=promote, the argument of a
       boxed receiver's Float arm) unboxes, a typed Integer widens, and a
       non-Numeric is CRuby's TypeError rather than a C build error (#4774) */
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", tb); emit_float_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", tl); emit_float_expr(c, sargv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "sp_float _t%d = ", ts);
    if (sargc >= 2) emit_float_expr(c, sargv[1], b); else buf_puts(b, "1.0");
    buf_puts(b, ";\n");
    /* a zero step never advances, so CRuby rejects it outright (#3648) */
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");\n", ts);
    emit_indent(b, indent);
    /* keep the count as a float: NaN begin/limit/step makes `i < NaN` false
       (0 iterations, matching CRuby) instead of UB from casting NaN to int
       (#3010) */
    buf_printf(b, "sp_float _t%d = sp_float_step_size(_t%d, _t%d, _t%d, 0);\n", tn, tb, tl, ts);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; (sp_float)_t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    if (p0) { char fp_expr[96]; snprintf(fp_expr, sizeof fp_expr, "sp_float_step_at(_t%d, _t%d, _t%d, _t%d)", tb, tl, ts, ti); emit_iter_param_assign(c, block, p0_orig, p0, TY_FLOAT, fp_expr, b, indent + 1); }
    { char rs_es[96]; snprintf(rs_es, sizeof rs_es, "sp_float_step_at(_t%d, _t%d, _t%d, _t%d)", tb, tl, ts, ti);
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
      /* the other way: a one-class hash binds its boxed values unboxed into
         the class-typed parameter (#4846) */
      int unbox = is_val && pv && ty_is_object(pv->type) && want == TY_POLY;
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
      else if (unbox) emit_unbox_text(c, pv->type, src, b);
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
  if (is_select_bang(name) && ty_is_hash(rt) && block >= 0) {
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    int tr2, to2, tw2;
    int ok = emit_hash_filter_loop(c, recv, block, rt, name, rb2.p ? rb2.p : "NULL", b, indent, &tr2, &to2, &tw2);
    free(rb2.p);
    if (ok) return 1;
  }

  /* array.each_with_index { |x, i| ... } */
  if (sp_streq(name, "each_with_index") && (ty_is_array(rt) || ty_is_obj_array(rt))) {
    const char *k = array_iter_kind(rt);
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
    if (p0 && clv_ewi_p0) ts_p0 = emit_shadow_save(c, clv_ewi_p0->type, p0, b, indent);
    if (p1 && clv_ewi_p1) ts_p1 = emit_shadow_save(c, clv_ewi_p1->type, p1, b, indent);
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

  /* array.zip(other) { |a, b| ... } -- block form, returns nil */
  if (sp_streq(name, "zip") && (ty_is_array(rt) || rt == TY_POLY) && block >= 0) {
    int zargs_n = nt_ref(nt, id, "arguments");
    int zargc = 0; const int *zargv = zargs_n >= 0 ? nt_arr(nt, zargs_n, "arguments", &zargc) : NULL;
    /* The receiver, too, can be an array only at run time (a row read out of a
       poly table): walk it through the boxed accessors. Without this the call
       fell to the runtime dispatch, which has no zip arm at all. */
    int recv_poly = !ty_is_array(rt);
    const char *k = recv_poly ? "Poly" : array_iter_kind(rt);
    if (k && zargc == 1 && zargv) {
      TyKind a0t = comp_ntype(c, zargv[0]);
      const char *k2 = ty_is_array(a0t) ? array_iter_kind(a0t) : NULL;
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
      if (p0 && zlv0) zs0 = emit_shadow_save(c, zlv0->type, p0, b, indent);
      if (p1n && zlv1) zs1 = emit_shadow_save(c, zlv1->type, p1n, b, indent);
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
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p0);
        emit_zip_block_param(c, zlv0->type, et, src, b);
        buf_puts(b, ";\n");
      }
      if (p1n && zlv1 && ob.p) {
        char src2[512];
        if (arg_poly) snprintf(src2, sizeof src2, "sp_poly_arr_get(%s, _t%d)", ob.p, t);
        else snprintf(src2, sizeof src2, "sp_%sArray_get(%s, _t%d)", k2, ob.p, t);
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p1n);
        emit_zip_block_param(c, zlv1->type, et2, src2, b);
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
    emit_indent(b, indent); emit_poly_iter_obj_reject(c, ta, name, b);
    emit_indent(b, indent); buf_printf(b, "sp_poly_iter_check(_t%d, \"%s\");\n", ta, name);
    /* `each { |x| }` over an Enumerator yielding several values in a step
       binds x the first of them; the builtins/ walks (`each { |x| yield x }`)
       hand the step on whole */
    int tpair = 0;
    int gather = block_binds_gathered(c, block);
    {
      Scope *ss = comp_scope_of(c, id);
      const char *sn = ss ? ss->name : NULL;
      const char *rest = block_rest_name(c, block);
      if (sp_streq(name, "each") &&
          ((p0 && !block_param_name(c, block, 1) && !(rest && *rest)) || block_lone_rest(c, block) || gather) &&
          !(sn && strncmp(sn, "__enum", 6) == 0)) {
        tpair = ++g_tmp;
        emit_indent(b, indent); buf_printf(b, "int _t%d = sp_poly_yields_pair(_t%d);\n", tpair, ta);
      }
    }
    /* an Enumerator (or a String Range) has no element read of its own: walk
       the items it yields, so a Ruby-defined Enumerable method (find, count,
       each_with_object, ...) over a boxed one saw no elements. A generator
       is walked one item ahead, so a block that breaks out stops an endless
       one; reverse_each needs them all first. */
    emit_indent(b, indent);
    buf_printf(b, "_t%d = %s(_t%d);\n", ta,
               sp_streq(name, "reverse_each") ? "sp_poly_iter_subject" : "sp_poly_iter_walk", ta);
    emit_indent(b, indent); buf_printf(b, "sp_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tn, ta);
    emit_indent(b, indent);
    if (sp_streq(name, "reverse_each"))
      buf_printf(b, "for (sp_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", ti, tn, ti, ti);
    else
      /* the length is read again every turn, as the typed loops read theirs:
         a block that shrinks the receiver (`a.clear`, `a.pop`) stops the
         walk where the array now ends, where the hoisted count walked on
         past it and yielded nils, and one that grows it is followed (every
         Ruby-defined Enumerable method iterates through this loop on a
         boxed receiver) */
      buf_printf(b, "for (sp_int _t%d = 0; (void)_t%d, _t%d < sp_poly_arr_len_ex(_t%d); _t%d++) {\n", ti, tn, ti, ta, ti);
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
    else if (npp_poly >= 2 && !gather) {
      int telem = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);\n", telem, ta, ti);
      emit_poly_auto_splat(c, block, telem, b, indent);
    }
    else if (pv_half < 0 && block_lone_rest(c, block)) {
      /* `each { |*r| }`: the element, a Hash entry's [k, v] pair included,
         is the one value the rest array holds; an Enumerator that yields
         several values per step gives them all */
      if (tpair) {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_yielded_args(_t%d, sp_poly_each_elem(_t%d, _t%d));\n",
                   rename_local(block_rest_name(c, block)), tpair, ta, ti);
      }
      else {
        char rsrc[64]; snprintf(rsrc, sizeof rsrc, "sp_poly_each_elem(_t%d, _t%d)", ta, ti);
        emit_iter_bind_rest(c, block, 0, TY_POLY, rsrc, b, indent + 1);
      }
    }
    else if (gather) {
      /* any other shape binds the step's values by the proc distribution:
         the element (a Hash entry's [k, v] pair), the half of the pair
         each_value / each_key yield, or every value an Enumerator's step
         yielded */
      char elem[96]; snprintf(elem, sizeof elem, "sp_poly_each_elem(_t%d, _t%d)", ta, ti);
      if (pv_half >= 0) {
        int tel = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_RbVal _t%d = %s;\n", tel, elem);
        emit_indent(b, indent + 1);
        buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) _t%d = sp_poly_arr_get(_t%d, %d);\n",
                   tel, tel, tel, tel, pv_half);
        snprintf(elem, sizeof elem, "_t%d", tel);
      }
      char vals[160];
      if (tpair) snprintf(vals, sizeof vals, "sp_yielded_args(_t%d, %s)", tpair, elem);
      else snprintf(vals, sizeof vals, "sp_yielded_args(0, %s)", elem);
      emit_boxed_step_binds(c, block, vals, b, indent + 1, 0);
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
      char src[96];
      if (tpair) snprintf(src, sizeof src, "sp_yielded_first(_t%d, sp_poly_each_elem(_t%d, _t%d))", tpair, ta, ti);
      else snprintf(src, sizeof src, "sp_poly_each_elem(_t%d, _t%d)", ta, ti);
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
        /* an element that is nil binds a nullable parameter's own nil, where
           `.v.i` read the 0 under the tag */
        else if (b0->nullable_int) emit_unbox_nilable_text(c, p0t, vb0, b);
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
    if (outer_pa) ts_pa = emit_shadow_save(c, outer_pa->type, p0, b, indent);
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
        int telem = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, ta, t);
        emit_poly_auto_splat(c, block, telem, b, indent);
        did_destruct = 1;
      }
      if (!did_destruct) {
        /* an unregistered parameter has no C declaration (see
           emit_iter_param_assign): binding it would name a variable that does
           not exist (#3853) */
        Scope *bsc = comp_scope_of(c, block);
        LocalVar *plv = bsc ? scope_local(bsc, block_param_name(c, block, 0)) : NULL;
        if (plv && plv->type != TY_UNKNOWN) {
          /* A parameter typed from an earlier, narrower answer of the
             receiver (a String array, before the method returning it widened
             to a boxed one) takes the element unboxed, as the destructuring
             binding above does (#5521). */
          char src[64]; snprintf(src, sizeof src, "sp_PolyArray_get(_t%d, _t%d)", ta, t);
          emit_indent(b, indent + 1);
          emit_block_param_from_boxed(c, p0, plv->type, src, b);
        }
      }
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer_pa) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_pa); }
    return 1;
  }
  if (is_each_walk(name) &&
      (ty_is_array(rt) || ty_is_obj_array(rt))) {   /* an object array walks as sp_PtrArray (#4846) */
    const char *k = array_iter_kind(rt);
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
    /* the parameter's own slot can be boxed while the receiver stays a typed
       array (a widened slot, #4188): the element binds boxed into it, the
       way the shadowed outer's would (the take_while of a Ruby definition
       whose block parameter widened assigned a const char * to it) */
    int to_strbuf = outer && outer->type == TY_STRBUF && et == TY_STRING;
    if (!box_to_poly && p0 && et != TY_POLY) {
      Scope *bsc = comp_scope_of(c, block);
      LocalVar *blv = bsc ? scope_local(bsc, p0_orig ? p0_orig : p0) : NULL;
      if (blv && blv->type == TY_POLY) box_to_poly = 1;
      /* a slot a mutating callee made a String buffer takes a String
         element as one, as a plain assignment to it does (#6038) */
      if (blv && blv->type == TY_STRBUF && et == TY_STRING) to_strbuf = 1;
    }
    int ts = 0;
    if (outer) {
      /* Block params shadow outer variables in Ruby; save and restore */
      ts = emit_shadow_save(c, outer->type, p0, b, indent);
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
        /* A nested row is a pointer, not one of the scalar boxes. The same
           helper each_with_index uses covers that and the scalars. */
        Buf src; memset(&src, 0, sizeof src);
        buf_printf(&src, "sp_%sArray_get(", k);
        buf_puts(&src, rb.p ? rb.p : "NULL");
        buf_printf(&src, ", _t%d)", t);
        buf_printf(b, "lv_%s = ", p0);
        emit_boxed_text(c, et, src.p ? src.p : "", b);
        free(src.p);
        buf_puts(b, ";\n");
      }
      else if (to_strbuf) {
        buf_printf(b, "lv_%s = sp_String_new_shared(sp_StrArray_get(", p0);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d));\n", t);
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

  /* The arms below bind the tuple to the first parameter only. CRuby
     spreads it across a block taking two or more parameters (or a rest, an
     optional or a post). desugar_builtin_iter_block_shapes lowers such a
     block to one parameter; a block it leaves as written (a `&.` call, a
     destructuring parameter it does not take) would bind m to the whole
     tuple and n to nil, so it is refused rather than answered differently. */
  if (is_combination_family(name) &&
      (rt == TY_INT_ARRAY || rt == TY_POLY_ARRAY || rt == TY_FLOAT_ARRAY) && block >= 0 &&
      (block_lead_only(c, block) || block_rest_marker(c, block) ||
       block_opt_name(c, block, 0) || block_post_name(c, block, 0)))
    unsupported_feature(c, id, "a block taking more than one parameter on combination, permutation or their repeated forms");

  /* int_array.combination(k)/permutation(k) { |c| ... } -- yield each k-element
     sub-array as a fresh int_array. permutation also accepts the argless
     (full-length) form. */
  if (is_combination_family(name) &&
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

  /* the same over a poly array: the runtime builds each sub-array boxed, and
     the block binds it boxed, or as the poly array a typed parameter holds
     (#4919) */
  if (is_combination_family(name) &&
      rt == TY_POLY_ARRAY) {
    int is_perm = sp_streq(name, "permutation");
    const char *genfn = sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                      : sp_streq(name, "combination") ? "sp_PolyArray_combination"
                      : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                      : "sp_PolyArray_repeated_combination";
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 && !(is_perm && ac == 0)) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "{ sp_PolyArray *_t%d = ", ta); buf_puts(b, rb.p ? rb.p : ""); buf_printf(b, "; SP_GC_ROOT(_t%d);\n", ta); free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PolyArray *_t%d = %s(_t%d, ", tc, genfn, ta);
    if (ac == 1) emit_int_expr(c, av[0], b); else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
    buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) {
      Scope *cbsc = comp_scope_of(c, block);
      LocalVar *clv = cbsc ? scope_local(cbsc, p0) : NULL;
      TyKind cpt = clv ? clv->type : TY_UNKNOWN;
      emit_indent(b, indent + 2);
      if (cpt == TY_POLY_ARRAY)
        buf_printf(b, "lv_%s = (sp_PolyArray *)sp_PolyArray_get(_t%d, _t%d).v.p;\n", p0, tc, ti);
      else
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, tc, ti);
    }
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* the same over a Float array: the boxed implementation builds the
     sub-arrays, and a parameter typed Float array takes each one unboxed
     (the block was not emitted at all, and the call fell to the unresolved
     NoMethodError) */
  if (is_combination_family(name) &&
      rt == TY_FLOAT_ARRAY) {
    int is_perm = sp_streq(name, "permutation");
    const char *genfn = sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                      : sp_streq(name, "combination") ? "sp_PolyArray_combination"
                      : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                      : "sp_PolyArray_repeated_combination";
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 && !(is_perm && ac == 0)) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "{ sp_PolyArray *_t%d = sp_poly_to_poly_array(%s); SP_GC_ROOT(_t%d);\n", ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PolyArray *_t%d = %s(_t%d, ", tc, genfn, ta);
    if (ac == 1) emit_int_expr(c, av[0], b); else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
    buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) {
      Scope *cbsc = comp_scope_of(c, block);
      LocalVar *clv = cbsc ? scope_local(cbsc, p0) : NULL;
      TyKind cpt = clv ? clv->type : TY_UNKNOWN;
      emit_indent(b, indent + 2);
      if (cpt == TY_FLOAT_ARRAY)
        buf_printf(b, "lv_%s = sp_FloatArray_from_poly_array((sp_PolyArray *)sp_PolyArray_get(_t%d, _t%d).v.p);\n", p0, tc, ti);
      else if (cpt == TY_POLY_ARRAY)
        buf_printf(b, "lv_%s = (sp_PolyArray *)sp_PolyArray_get(_t%d, _t%d).v.p;\n", p0, tc, ti);
      else
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, tc, ti);
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
    buf_printf(b, "if (_t%d <= 0) sp_raise_cls(\"ArgumentError\", \"invalid size\");\n", tnn);
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
      long long lit = nt_kind(nt, eav[0]) == NK_IntegerNode ? nt_int(nt, eav[0], "value", 0) : 0;
      for (int pj = 0; pj < np; pj++)
        emit_row_param_bind(c, block, pj, k, rt, ta, ti, tnn, lit, indent + 1, b);
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
    /* a beginless Range held in a variable or built at run time cannot be
       enumerated from nil, as the literal `(..3).each` above refuses; it
       walked up from INTPTR_MIN, whose first value read back as nil */
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d.first == INTPTR_MIN && _t%d > 0) sp_range_nil_begin_raise();\n", t, ts);
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
    /* a boxed limit (the receiver reached this arm through the poly face,
       and its limit is boxed too) is unboxed to the counter's sp_int; it did
       not compile against it (#4665). A typed limit stays as it is: a Float
       one compares as a Float (`-5.upto(-1.3)` stops at -2). */
    Buf hi; memset(&hi, 0, sizeof hi);
    if (comp_ntype(c, argv[0]) == TY_POLY) emit_int_expr(c, argv[0], &hi);
    else emit_expr(c, argv[0], &hi);
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
    /* a receiver of no type -- a call proven to raise NoMethodError -- is
       the raise's sp_RbVal; `void _t` did not compile (#6213) */
    if (et == TY_UNKNOWN || et == TY_VOID) et = TY_POLY;
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
    /* A boxed receiver over a SCALAR param: keep the param typed and unbox
       the value into it (raising past the word, as the other typed sinks
       do) rather than shadowing it as boxed -- the body was inferred against
       the scalar, and the shadow's retype does not reach the cached node
       types, so `v.negative?` read an sp_int out of an sp_RbVal and the C did
       not build. Under --int-overflow=promote every Integer local is boxed
       while a block param is not, which is where the shape lives (#4730).
       The expression-form emitter (emit_tap_then_expr) does the same. */
    const char *tap_unbox = NULL;
    if (use_shadow_t && et == TY_POLY && tsaved0 == TY_INT) tap_unbox = "sp_poly_to_i";
    else if (use_shadow_t && et == TY_POLY && tsaved0 == TY_FLOAT) tap_unbox = "sp_poly_to_f";
    if (tap_unbox) use_shadow_t = 0;
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
        else if (tap_unbox) {
          buf_printf(b, "lv_%s = %s(_t%d);\n", p0, tap_unbox, tr);
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
    /* a size of 0 stepped the loop by nothing, forever */
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d <= 0) sp_raise_cls(\"ArgumentError\", \"invalid slice size\");\n", ts);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
               ti, ti, k, ta, ti, ts);
    int bodyIndent = indent + 1;
    if (np_es > 1) {
      /* multi-param: destructure slice elements into individual params */
      long long lit = nt_kind(nt, es_argv[0]) == NK_IntegerNode ? nt_int(nt, es_argv[0], "value", 0) : 0;
      for (int pj = 0; pj < np_es; pj++)
        emit_row_param_bind(c, block, pj, k, rt, ta, ti, ts, lit, bodyIndent, b);
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
                sp_streq(nt_str(nt, sp_argv[0], "content"), " ") && nt_str_len(nt, sp_argv[0], "content") == 1);
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
    if (sc_re >= 0 && an_re_has_captures(re_lit_src(c, sc_argv[0]))) return 0;
    /* a body that reads `$~` or a capture global sees its own turn's match,
       which the value-form emitter walks the subject for (#3601) */
    if (subtree_reads_match_globals(c, body)) return 0;
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

