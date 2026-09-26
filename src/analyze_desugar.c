/* analyze_desugar.c -- the AST rewrites analyze_program drives, split out of
   analyze_pass.c. Pure code movement, no logic change: the functions keep
   their order, and analyze_program's call sequence is untouched.

   Five desugars stay in analyze_pass.c because they use file-static helpers
   that inference also uses (subtree_has_kind, subtree_rename_local, the
   bdp_/ie_ pairs); moving those would widen their linkage for no reason but
   this file split. */
#include "analyze_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Required-param count of a forwarded callable expression `ex`, or -1 if it
   cannot be determined statically. Chooses the hash-pair calling convention: a
   1-param callable receives the [k,v] pair as one array, a 2-param one is called
   positionally (matching CRuby's proc auto-splat of the yielded pair). */
static int fwd_callable_arity(Compiler *c, int ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *exty = nt_type(nt, ex);
  if (!exty) return -1;
  int create = -1;
  if (sp_streq(exty, "LambdaNode") || is_proc_create(c, ex)) create = ex;
  else if (sp_streq(exty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ex, "name");
    Scope *sc = vn ? comp_scope_of(c, ex) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
    }
  }
  if (create < 0) return -1;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return -1;
  int rn = 0; nt_arr(nt, pn, "requireds", &rn);
  return rn;
}

/* A method call on a local statically holding one BUILTIN class constant
   dispatches like the constant itself: retarget the receiver at the AST so
   `k = Array; k.new(3, 0)` rides every Array.new arm (#2715). User classes
   already resolve through class_var_static_ci at the dispatch sites. */
static unsigned bcv_key_hash(const char *name, const Scope *sc) {
  unsigned h = 5381;
  for (const char *p = name; *p; p++) h = h * 33u + (unsigned char)*p;
  size_t s = (size_t)(const void *)sc;
  for (unsigned b = 0; b < sizeof s; b++) h = h * 33u + (unsigned char)(s >> (b * 8));
  return h;
}

int desugar_builtin_class_var_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;

  /* Index every LocalVariableWriteNode by (variable name, scope) once, so
     resolving a receiver's static class scans only the writes that could
     actually bind it instead of the whole node table per receiver. Turns the
     pass from O(receivers * N) into O(N) -- the quadratic that stalled the
     lobsters tree (#3115). Scope belongs in the key because a hot name reused
     across many scopes otherwise leaves one long chain whose every entry needs
     its scope resolved. Inlines the old builtin_class_var_static_name
     resolution over the index. */
  int nbuckets = 16;
  while (nbuckets < n0) nbuckets <<= 1;
  int *head = malloc((size_t)nbuckets * sizeof(int));
  int *wnext = malloc((size_t)(n0 > 0 ? n0 : 1) * sizeof(int));
  if (!head || !wnext) { free(head); free(wnext); return 0; }
  for (int i = 0; i < nbuckets; i++) head[i] = -1;
  unsigned mask = (unsigned)nbuckets - 1;
  for (int w = 0; w < n0; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn) continue;
    unsigned h = bcv_key_hash(wn, comp_scope_of(c, w)) & mask;
    wnext[w] = head[h];
    head[h] = w;
  }

  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *vn = nt_str(nt, recv, "name");
    if (!vn) continue;
    Scope *sc = comp_scope_of(c, recv);
    unsigned h = bcv_key_hash(vn, sc) & mask;
    /* every write of this local in this scope must assign the SAME builtin (or
       user-class) constant, else the local is dynamic and does not retarget */
    const char *cn = NULL;
    int bail = 0;
    for (int w = head[h]; w >= 0; w = wnext[w]) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      const char *vcn = (val >= 0 && nt_kind(nt, val) == NK_ConstantReadNode)
                        ? nt_str(nt, val, "name") : NULL;
      if (!vcn || !(is_builtin_class_name(vcn) || comp_class_index(c, vcn) >= 0)) { bail = 1; break; }
      if (cn && !sp_streq(cn, vcn)) { bail = 1; break; }
      cn = vcn;
    }
    if (bail || !cn) continue;
    int cr = nt_new_node(nt, "ConstantReadNode");
    if (cr < 0) continue;
    nt_node_set_str(nt, cr, "name", cn);
    comp_grow_node_arrays(c);
    c->nscope[cr] = c->nscope[id];
    nt_node_set_ref(nt, id, "receiver", cr);
    changed = 1;
  }
  free(head); free(wnext);
  return changed;
}

/* A bare `new(...)` in a class body (`MAP = { 0 => new(0) }`, `ONE = new(1)`)
   is a call on the class itself, which is the implicit self there. Nothing
   resolved it: the constant it initialised typed unknown and was dropped,
   with a warning that said the constant was defined nowhere (#4515). Give
   it the class as its receiver, the way the body's own methods reach it
   (`K.new(0)`). A method body is left alone: bare `new` inside a class
   method already constructs the emitting class (codegen), and inside an
   instance method it is CRuby's NameError. */
int desugar_class_body_bare_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "new")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    if (id >= c->node_cap) continue;
    int cid = c->node_cbody[id];
    if (cid < 0 || cid >= c->nclasses) continue;
    Scope *sc = comp_scope_of(c, id);
    if (sc && sc->name) continue;   /* inside a def: not the body */
    const char *cn = c->classes[cid].name;
    if (!cn || !*cn) continue;
    int cr = nt_new_node(nt, "ConstantReadNode");
    if (cr < 0) continue;
    nt_node_set_str(nt, cr, "name", cn);
    comp_grow_node_arrays(c);
    c->nscope[cr] = c->nscope[id];
    c->node_cbody[cr] = cid;
    nt_node_set_ref(nt, id, "receiver", cr);
    changed = 1;
  }
  return changed;
}

/* A receiverless `const_get(:K)` in a class method, or in the class body
   itself, is sent to the class -- the implicit self there. It was left without
   a receiver, typed nothing, and the call on its value raised NoMethodError
   for "unknown" at run time (#4843). Give it self, as `self.const_get(:K)`,
   which already resolves. In an instance method self is an instance, which
   has no const_get, so that is left for the ordinary NoMethodError. */
int desugar_bare_const_get(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "const_get")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    if (id >= c->node_cap) continue;
    Scope *sc = comp_scope_of(c, id);
    int in_cmethod = sc && sc->name && sc->is_cmethod && sc->class_id >= 0;
    int in_body = (!sc || !sc->name) && c->node_cbody[id] >= 0;
    if (!in_cmethod && !in_body) continue;
    int sn = nt_new_node(nt, "SelfNode");
    if (sn < 0) continue;
    comp_grow_node_arrays(c);
    c->nscope[sn] = c->nscope[id];
    c->node_cbody[sn] = c->node_cbody[id];
    nt_node_set_ref(nt, id, "receiver", sn);
    changed = 1;
  }
  return changed;
}

/* Inside an instance_eval / instance_exec block self is the receiver, so a
   receiverless `is_a?(Box)` or `respond_to?(:v)` there asks the receiver.
   A user method of the name already resolves through the block's receiver
   class; one of Object's own had nothing to ask and was refused. Give it
   self, as `self.is_a?(Box)`, which the SelfNode path already rebinds to
   the receiver. */
int desugar_ie_bare_object_calls(Compiler *c) {
  static const char *const names[] = {
    "is_a?", "kind_of?", "instance_of?", "respond_to?", "frozen?", "nil?",
    "object_id", "hash", "inspect", "to_s", "freeze", "dup", "clone",
    "itself", "equal?", "eql?", "instance_variable_get",
    "instance_variable_set", "instance_variable_defined?",
    "instance_variables", "public_send", "__send__", "send", NULL };
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    int cls = ie_class_of(c, id);
    if (cls < 0 || id >= c->node_cap) continue;
    const char *nm = nt_str(nt, id, "name");
    int hit = 0;
    for (int k = 0; nm && names[k] && !hit; k++) hit = sp_streq(nm, names[k]);
    if (!hit || comp_method_in_chain(c, cls, nm, NULL) >= 0) continue;
    int sn = nt_new_node(nt, "SelfNode");
    if (sn < 0) continue;
    comp_grow_node_arrays(c);
    c->nscope[sn] = c->nscope[id];
    c->node_cbody[sn] = c->node_cbody[id];
    nt_node_set_ref(nt, id, "receiver", sn);
    changed = 1;
  }
  return changed;
}

/* `b["href"], title = rhs` where b is a user object: the index target is
   b's own []=, which the multiple-assignment emitter has no arm for -- a
   literal right side was refused and a computed one dropped the write. As
   a statement, it is `__mwi, title = rhs; b["href"] = __mwi`. Only a
   receiver and index that evaluating later cannot change (a variable, self,
   a constant, a literal), so moving the store past the right side keeps
   the order observable. */
static int masgn_stable_operand(const NodeTable *nt, int n) {
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode: case NK_SelfNode:
  case NK_ConstantReadNode: case NK_IntegerNode: case NK_StringNode: case NK_SymbolNode:
  case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    return 1;
  default:
    return 0;
  }
}
int desugar_masgn_object_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_MultiWriteNode || id >= c->node_cap) continue;
    int ln = 0; const int *ls = nt_arr(nt, id, "lefts", &ln);
    for (int j = 0; j < ln; j++) {
      int tgt = ls[j];
      if (nt_kind(nt, tgt) != NK_IndexTargetNode) continue;
      int recv = nt_ref(nt, tgt, "receiver");
      int anode = nt_ref(nt, tgt, "arguments");
      if (recv < 0 || !ty_is_object(infer_type(c, recv)) || !masgn_stable_operand(nt, recv)) continue;
      int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      int stable = an > 0;
      for (int k = 0; k < an && stable; k++) stable = masgn_stable_operand(nt, av[k]);
      if (!stable) continue;
      /* the statement list holding the multiple assignment */
      int stmts = -1, pos = -1;
      NT_FOREACH_KIND(nt, NK_StatementsNode, sn) {
        int bn = 0; const int *bb = nt_arr(nt, sn, "body", &bn);
        for (int k = 0; k < bn; k++) if (bb[k] == id) { stmts = sn; pos = k; }
        if (stmts >= 0) break;
      }
      if (stmts < 0) continue;
      char tmp[48]; snprintf(tmp, sizeof tmp, "__mwi_%d_%d", id, j);
      int lt = nt_new_node(nt, "LocalVariableTargetNode");
      int lr = nt_new_node(nt, "LocalVariableReadNode");
      int nargs = nt_new_node(nt, "ArgumentsNode");
      int call = nt_new_node(nt, "CallNode");
      if (lt < 0 || lr < 0 || nargs < 0 || call < 0) continue;
      nt_node_set_str(nt, lt, "name", tmp);
      nt_node_set_str(nt, lr, "name", tmp);
      int *na = malloc(sizeof(int) * (size_t)(an + 1));
      if (!na) continue;
      for (int k = 0; k < an; k++) na[k] = av[k];
      na[an] = lr;
      nt_node_set_arr(nt, nargs, "arguments", na, an + 1);
      free(na);
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", "[]=");
      nt_node_set_ref(nt, call, "arguments", nargs);
      nt_node_set_ref(nt, call, "block", -1);
      int line = (int)nt_int(nt, id, "node_line", 0);
      if (line) nt_node_set_int(nt, call, "node_line", line);
      /* the target becomes the temp; the store follows the statement */
      int *nl = malloc(sizeof(int) * (size_t)ln);
      if (!nl) continue;
      for (int k = 0; k < ln; k++) nl[k] = ls[k];
      nl[j] = lt;
      nt_node_set_arr(nt, id, "lefts", nl, ln);
      free(nl);
      int bn = 0; const int *bb = nt_arr(nt, stmts, "body", &bn);
      int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
      if (!nb) continue;
      for (int k = 0, o = 0; k < bn; k++) { nb[o++] = bb[k]; if (k == pos) nb[o++] = call; }
      nt_node_set_arr(nt, stmts, "body", nb, bn + 1);
      free(nb);
      comp_grow_node_arrays(c);
      int made[] = { lt, lr, nargs, call };
      for (int k = 0; k < 4; k++) { c->nscope[made[k]] = c->nscope[id]; c->node_cbody[made[k]] = c->node_cbody[id]; }
      Scope *sc = comp_scope_of(c, id);
      if (sc) scope_local_intern(sc, tmp);
      changed = 1;
      /* the lefts array was replaced; reread it before the next target */
      ls = nt_arr(nt, id, "lefts", &ln);
    }
  }
  return changed;
}

/* Proc#>> / #<< with a Method operand: wrap the Method side in #to_proc at the
   AST, so composition always runs proc-to-proc. The to_proc emission builds a
   real trampoline proc that publishes its boxed result through the return
   slot; the raw sp_method_to_proc tramp does not, which is why composing the
   Method directly mis-typed the intermediate (#2692). */
int desugar_compose_method_operand(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, ">>") && !sp_streq(nm, "<<"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || an != 1 || !av) continue;
    TyKind rt = infer_type(c, recv), at = infer_type(c, av[0]);
    int r_m = rt == TY_METHOD, a_m = at == TY_METHOD;
    if (!r_m && !a_m) continue;
    /* The other operand may be a Proc read out of a container, which arrives
       boxed: the composition arm unwraps it. Refusing the shape left a Method
       composed with a container-read Proc unemittable (#3884). A poly `<<` is
       unambiguous here -- the Method side says this is a composition. */
    if (!(rt == TY_METHOD || rt == TY_PROC || rt == TY_POLY) ||
        !(at == TY_METHOD || at == TY_PROC || at == TY_POLY)) continue;
    int a0 = av[0];
    int base = nt->count;
    if (r_m) {
      int tp = nt_new_node(nt, "CallNode");
      if (tp < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", recv);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_ref(nt, id, "receiver", tp);
    }
    if (a_m) {
      int tp = nt_new_node(nt, "CallNode");
      int na = nt_new_node(nt, "ArgumentsNode");
      if (tp < 0 || na < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", a0);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_arr(nt, na, "arguments", &tp, 1);
      nt_node_set_ref(nt, id, "arguments", na);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* proc.curry(obj) -> proc.curry(obj.to_int): CRuby converts a non-Integer
   count through to_int (a to_int-less count is its TypeError). The rewrite
   fires once per argument -- an arg already spelled to_int, an Integer, or
   a literal nil (curry's no-count spelling) is left alone. */
int desugar_curry_arity_to_int(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "curry")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || infer_type(c, recv) != TY_PROC) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1 || !av) continue;
    int a0 = av[0];
    /* Integer, nil-typed and BOXED counts are read at run time
       (sp_curry_new_v), so only a count of some other settled type -- a
       to_int object, a Float -- converts here. TY_UNKNOWN waits: the rewrite
       is irreversible, and firing before the count's type settles wrapped a
       later-nil method in to_int. */
    TyKind a0t = infer_type(c, a0);
    if (nt_kind(nt, a0) == NK_NilNode || a0t == TY_INT || a0t == TY_NIL ||
        a0t == TY_POLY || a0t == TY_UNKNOWN) continue;
    const char *anm = nt_kind(nt, a0) == NK_CallNode ? nt_str(nt, a0, "name") : NULL;
    if (anm && sp_streq(anm, "to_int")) continue;
    int base = nt->count;
    int ti = nt_new_node(nt, "CallNode");
    int na = nt_new_node(nt, "ArgumentsNode");
    if (ti < 0 || na < 0) continue;
    nt_node_set_ref(nt, ti, "receiver", a0);
    nt_node_set_str(nt, ti, "name", "to_int");
    nt_node_set_ref(nt, ti, "arguments", -1);
    nt_node_set_ref(nt, ti, "block", -1);
    nt_node_set_arr(nt, na, "arguments", &ti, 1);
    nt_node_set_ref(nt, id, "arguments", na);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* method.curry -> method.to_proc.curry: the Proc curry machinery (arity
   typing, boxed accumulation, param widening) then applies unchanged. The
   rewrite fires once: afterwards curry's receiver is the synthesized
   to_proc call, which infers TY_PROC. */
int desugar_method_curry(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "curry")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (infer_type(c, recv) != TY_METHOD) continue;
    int base = nt->count;
    int tp = nt_new_node(nt, "CallNode");
    if (tp < 0) continue;
    nt_node_set_ref(nt, tp, "receiver", recv);
    nt_node_set_str(nt, tp, "name", "to_proc");
    nt_node_set_ref(nt, tp, "arguments", -1);
    nt_node_set_ref(nt, tp, "block", -1);
    nt_node_set_ref(nt, id, "receiver", tp);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.times.with_index / upto / downto (and with_object/each_with_index):
   a blockless Integer enumerator types as a range, which has no with_index
   arm. Interpose `.each` -- range.each stays an external Enumerator ahead
   of these chains, whose machinery already serves both the blockless and
   the block forms. Fires once: afterwards the receiver is the each call. */
int desugar_int_enum_with_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "with_index") && !sp_streq(nm, "with_object") &&
                !sp_streq(nm, "each_with_index"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    if (nt_ref(nt, recv, "block") >= 0) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || (!sp_streq(rnm, "times") && !sp_streq(rnm, "upto") &&
                 !sp_streq(rnm, "downto"))) continue;
    if (infer_type(c, recv) != TY_RANGE) continue;
    int base = nt->count;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && (sp_streq(nm, "with_index") || sp_streq(nm, "each_with_index"))) {
      /* Block form returns the Integer RECEIVER (CRuby: the enumerator's
         underlying each return), not the range: hoist the receiver into a
         temp, run the chain for effect, and make the original call a
         transparent `.itself` on `(t = n; t.times.each.with_index {..}; t)`
         so the value is the int evaluated once. */
      int ircv = nt_ref(nt, recv, "receiver");
      if (ircv < 0) continue;
      char tmpn[32]; snprintf(tmpn, sizeof tmpn, "_spwi%d", id);
      /* register_locals already ran: intern the temp into the enclosing
         scope now; its type comes from the following inference passes. */
      { int encl0 = c->nscope[id];
        if (encl0 >= 0 && encl0 < c->nscopes)
          scope_local_intern(&c->scopes[encl0], tmpn); }
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      int rd1 = nt_new_node(nt, "LocalVariableReadNode");
      int rd2 = nt_new_node(nt, "LocalVariableReadNode");
      int ec = nt_new_node(nt, "CallNode");
      int inner = nt_new_node(nt, "CallNode");
      int stmts = nt_new_node(nt, "StatementsNode");
      int paren = nt_new_node(nt, "ParenthesesNode");
      if (w < 0 || rd1 < 0 || rd2 < 0 || ec < 0 || inner < 0 ||
          stmts < 0 || paren < 0) continue;
      nt_node_set_str(nt, w, "name", tmpn);
      nt_node_set_ref(nt, w, "value", ircv);
      nt_node_set_str(nt, rd1, "name", tmpn);
      nt_node_set_str(nt, rd2, "name", tmpn);
      nt_node_set_ref(nt, recv, "receiver", rd1);
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_str(nt, inner, "name", nm);
      nt_node_set_ref(nt, inner, "receiver", ec);
      nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
      nt_node_set_ref(nt, inner, "block", blk);
      { int items[3] = { w, inner, rd2 };
        nt_node_set_arr(nt, stmts, "body", items, 3); }
      nt_node_set_ref(nt, paren, "body", stmts);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "receiver", paren);
      nt_node_set_ref(nt, id, "block", -1);
      nt_node_set_ref(nt, id, "arguments", -1);
    }
    else {
      int ec = nt_new_node(nt, "CallNode");
      if (ec < 0) continue;
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_ref(nt, id, "receiver", ec);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_reduce_proc_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject"))) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    /* This rewrite serves the C fold emitters, which read the block's body.
       A receiver whose inject/reduce is the PROGRAM's own method -- a user
       class that defines the name -- keeps its `&b`: the block would otherwise be
       spliced into that method's body naming the caller's `b` from a frame
       that no longer has it (`'lv_b' undeclared`). */
    { TyKind rt0 = infer_type(c, nt_ref(nt, id, "receiver"));
      if (ty_is_object(rt0)) continue; }
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    int simple = exty && (sp_streq(exty, "LocalVariableReadNode") ||
                          sp_streq(exty, "InstanceVariableReadNode") ||
                          sp_streq(exty, "LambdaNode"));
    if (!simple || infer_type(c, ex) != TY_PROC) continue;

    int base = nt->count;
    char pn[2][40]; int reqs[2], reads[2];
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
      snprintf(pn[k], sizeof pn[k], "__fold_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      if (reqs[k] < 0 || reads[k] < 0) { ok = 0; break; }
      nt_node_set_str(nt, reqs[k], "name", pn[k]);
      nt_node_set_str(nt, reads[k], "name", pn[k]);
    }
    if (!ok) continue;
    int params = nt_new_node(nt, "ParametersNode");
    int bparams = nt_new_node(nt, "BlockParametersNode");
    int callargs = nt_new_node(nt, "ArgumentsNode");
    int callnode = nt_new_node(nt, "CallNode");
    int body = nt_new_node(nt, "StatementsNode");
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 || blocknode < 0) continue;
    nt_node_set_arr(nt, params, "requireds", reqs, 2);
    nt_node_set_ref(nt, bparams, "parameters", params);
    nt_node_set_arr(nt, callargs, "arguments", reads, 2);
    nt_node_set_ref(nt, callnode, "receiver", ex);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);
    nt_node_set_ref(nt, id, "block", blocknode);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < 2; k++) {
      LocalVar *lv = scope_local_intern(bs, pn[k]);
      if (lv) lv->is_block_param = 1;
    }
    changed = 1;
  }
  return changed;
}

/* ENV's enumeration/read-only surface rides a StrStr-hash snapshot: retarget
   the receiver at a receiverless __env_to_h call, and the whole Hash machinery
   serves keys/each/select/count{...}/inspect/... (#2742). Mutators and the
   direct read/write arms (\[\], \[\]=, fetch sans block, delete, store) stay on
   the real environment. */
static int env_enum_method(const char *n) {
  static const char *const M[] = {
    "keys", "values", "each", "each_pair", "each_key", "each_value",
    "each_entry", "to_h", "to_a", "select", "filter", "reject", "any?",
    "all?", "none?", "one?", "find", "detect", "find_all", "min_by", "max_by",
    "sort", "sort_by", "map", "collect", "flat_map", "filter_map", "group_by",
    "partition", "sum", "reduce", "inject", "invert", "key", "rassoc",
    "assoc", "slice", "except", "values_at", "count", "inspect", "hash",
    "empty?",
    /* the wider Enumerable/query surface (#2832) */
    "to_hash", "entries", "first", "min", "max", "minmax", "tally", "uniq",
    "zip", "take", "take_while", "drop", "drop_while", "each_slice",
    "each_cons", "each_with_index", "each_with_object", "find_index", "grep",
    "chunk", "chunk_while", "slice_when", "collect_concat",
    "reverse_each", "value?", "has_value?", "lazy", NULL };
  for (int i = 0; M[i]; i++) if (sp_streq(n, M[i])) return 1;
  return 0;
}

/* `a !~ b` where a's class defines `=~` (and no `!~` of its own): Object#!~ is
   !(a =~ b). Rewrite the CallNode into `!` over a FRESH `=~` node -- renaming
   in codegen poisoned the node-type cache (`!~` bool vs `=~` any, see #3018's
   note), a fresh node types independently (#3019). Receivers without a user
   `=~` (regex operands, bool/nil raises) keep their dedicated paths. */
int desugar_user_not_match(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "!~")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (cid < 0 || comp_method_in_chain(c, cid, "=~", NULL) < 0) continue;
    if (comp_method_in_chain(c, cid, "!~", NULL) >= 0) continue;  /* user !~ wins */
    int inner = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, inner, "name", "=~");
    nt_node_set_ref(nt, inner, "receiver", recv);
    nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
    nt_node_set_ref(nt, inner, "block", -1);
    nt_node_set_str(nt, id, "name", "!");
    nt_node_set_ref(nt, id, "receiver", inner);
    nt_node_set_ref(nt, id, "arguments", -1);
    comp_grow_node_arrays(c);
    c->nscope[inner] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_env_enum(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "ENV")) continue;
    int is_enum = env_enum_method(nm);
    /* fetch WITH a block rides the snapshot's block-aware Hash#fetch (#2745) */
    if (!is_enum && sp_streq(nm, "fetch") && nt_ref(nt, id, "block") >= 0) is_enum = 1;
    if (!is_enum) continue;
    /* ENV keys are Strings at the C level: a statically non-String argument to
       the string-keyed queries is CRuby's TypeError, not a silent miss (#3000).
       Rewrite the call into the raise before the snapshot desugar. */
    if (sp_streq(nm, "assoc") || sp_streq(nm, "key") || sp_streq(nm, "slice") ||
        sp_streq(nm, "values_at")) {
      int qargs = nt_ref(nt, id, "arguments");
      int qn = 0; const int *qav = qargs >= 0 ? nt_arr(nt, qargs, "arguments", &qn) : NULL;
      const char *badc = NULL;
      for (int k = 0; k < qn && !badc; k++) {
        TyKind at = infer_type(c, qav[k]);
        badc = at == TY_SYMBOL ? "Symbol" : at == TY_INT ? "Integer"
             : at == TY_FLOAT ? "Float" : at == TY_NIL ? "nil"
             : at == TY_BOOL ? "Boolean" : NULL;
      }
      if (badc) {
        char msg[128];
        snprintf(msg, sizeof msg, "no implicit conversion of %s into String", badc);
        int ecn = nt_new_node(nt, "ConstantReadNode");
        nt_node_set_str(nt, ecn, "name", "TypeError");
        int emn = nt_new_node(nt, "StringNode");
        nt_node_set_str(nt, emn, "content", msg);
        int ea[2] = { ecn, emn };
        int eargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, eargs, "arguments", ea, 2);
        nt_node_set_str(nt, id, "name", "raise");
        nt_node_set_ref(nt, id, "receiver", -1);
        nt_node_set_ref(nt, id, "arguments", eargs);
        nt_node_set_ref(nt, id, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[ecn] = c->nscope[emn] = c->nscope[eargs] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    int snap = nt_new_node(nt, "CallNode");
    if (snap < 0) continue;
    nt_node_set_str(nt, snap, "name", "__env_to_h");
    nt_node_set_ref(nt, snap, "receiver", -1);
    nt_node_set_ref(nt, snap, "arguments", -1);
    nt_node_set_ref(nt, snap, "block", -1);
    comp_grow_node_arrays(c);
    c->nscope[snap] = c->nscope[id];
    /* the plain-Enumerable names ride the pair ARRAY (the typed-hash surface
       does not carry them); hash-native names stay on the snapshot (#2832) */
    {
      static const char *const VIA_A[] = {
        "first", "min", "max", "minmax", "tally", "uniq", "zip", "take",
        "take_while", "drop", "drop_while", "each_slice", "each_cons",
        "each_with_index", "each_with_object", "find_index", "grep", "chunk",
        "chunk_while", "slice_when", "collect_concat", "reverse_each",
        "lazy", NULL };
      int via_a = 0;
      for (int q = 0; VIA_A[q]; q++) if (sp_streq(nm, VIA_A[q])) { via_a = 1; break; }
      if (via_a) {
        int toa = nt_new_node(nt, "CallNode");
        if (toa < 0) continue;
        nt_node_set_str(nt, toa, "name", "to_a");
        nt_node_set_ref(nt, toa, "receiver", snap);
        nt_node_set_ref(nt, toa, "arguments", -1);
        nt_node_set_ref(nt, toa, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[toa] = c->nscope[id];
        nt_node_set_ref(nt, id, "receiver", toa);
      }
      else nt_node_set_ref(nt, id, "receiver", snap);
    }
    /* aliases the hash surface spells differently */
    if (sp_streq(nm, "to_hash")) nt_node_set_str(nt, id, "name", "to_h");
    else if (sp_streq(nm, "entries")) nt_node_set_str(nt, id, "name", "to_a");
    else if (sp_streq(nm, "value?")) nt_node_set_str(nt, id, "name", "has_value?");
    else if (sp_streq(nm, "collect_concat")) nt_node_set_str(nt, id, "name", "flat_map");
    changed = 1;
  }
  return changed;
}

int desugar_public_method(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "public_method")) continue;
    if (!method_sym_arg(c, id)) continue;   /* literal symbol/string arg only */
    nt_node_set_str(nt, id, "name", "method");
    changed = 1;
  }
  return changed;
}

/* Is `n` a value spinel can materialize with #to_a -- i.e. an operand a chain
   may concatenate? Arrays/ranges/hashes/enumerators answer directly; a user
   object qualifies when its class defines #each (the Enumerable contract). */
static int chain_operand_ok(Compiler *c, int n) {
  if (n < 0) return 0;
  TyKind t = infer_type(c, n);
  if (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE || t == TY_ENUMERATOR) return 1;
  /* An empty array literal never narrows, so `a = []; a.chain.to_a` leaves the
     receiver UNKNOWN (#2474 / #2468). #chain is Enumerable-specific and a
     user-defined #chain is excluded by the caller, so an untyped operand is
     taken at its word; if it turns out to have no #to_a, that call reports it. */
  if (t == TY_UNKNOWN) return 1;
  if (ty_is_object(t)) {
    int ci = ty_object_class(t);
    return ci >= 0 && comp_method_in_chain(c, ci, "each", NULL) >= 0;
  }
  return 0;
}

/* Synthesize `<n>.to_a` (a fresh CallNode), or -1 on node-table OOM. */
static int chain_mk_to_a(Compiler *c, int n) {
  NodeTable *nt = (NodeTable *)c->nt;
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", n);
  nt_node_set_str(nt, call, "name", "to_a");
  nt_node_set_ref(nt, call, "arguments", -1);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* Synthesize `<a> + <b>`, or -1 on node-table OOM. */
static int chain_mk_concat(Compiler *c, int a, int b) {
  NodeTable *nt = (NodeTable *)c->nt;
  int args = nt_new_node(nt, "ArgumentsNode");
  if (args < 0) return -1;
  nt_node_set_arr(nt, args, "arguments", &b, 1);
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", a);
  nt_node_set_str(nt, call, "name", "+");
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* `recv.chain(a, b)` and `enum + enum` -> `__enum_chain(recv.to_a + a.to_a + b.to_a)`.
   Ruby's chain is lazy over its sources; spinel materializes them at build time
   and hands the concatenation to a snapshot enumerator, which serves every
   terminal the sources support (#to_a, #each, #map, #next, ...). Reusing #to_a
   is what lets a Struct, a user Enumerable, or another enumerator be an operand:
   each already knows how to materialize itself. #2545 / #2548 / #2551 */
int desugar_enumerable_chain(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;   /* chain{} is not a thing; leave it */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;

    int is_chain = sp_streq(nm, "chain");
    /* Enumerator#+ only: `+` is overwhelmingly numeric/array/string, so require
       BOTH operands to be enumerators before touching it. */
    int is_plus = sp_streq(nm, "+") && argc == 1 && argv &&
                  infer_type(c, recv) == TY_ENUMERATOR &&
                  infer_type(c, argv[0]) == TY_ENUMERATOR;
    if (!is_chain && !is_plus) continue;
    /* `arr.chain` with no argument is just the receiver's own elements (#2468),
       so argc == 0 is valid for chain (but `+` always has its operand). */
    if (argc > 32 || (argc > 0 && !argv) || (is_plus && argc != 1)) continue;

    if (is_chain) {
      /* a user-defined #chain wins over Enumerable's */
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) {
        int ci = ty_object_class(rt);
        if (ci >= 0 && comp_method_in_chain(c, ci, "chain", NULL) >= 0) continue;
      }
      /* A boxed receiver (an Array read out of a container) materializes
         through the run-time #to_a dispatch over whatever it holds, so it
         qualifies as an untyped one does; it stands down when a user class
         defines #chain, since it may hold an instance of that class. A boxed
         ARGUMENT qualifies only behind a boxed receiver, whose `+` is over
         two poly arrays; a typed receiver's `+` over the boxed array binds
         its operands without roots, so that call is left as it was. */
      if (rt == TY_POLY && an_user_defines_or_reads(c, "chain")) continue;
      if (rt != TY_POLY && !chain_operand_ok(c, recv)) continue;
    }
    int ok = 1;
    int recv_boxed = is_chain && infer_type(c, recv) == TY_POLY;
    for (int k = 0; k < argc && ok; k++)
      if (!chain_operand_ok(c, argv[k]) && !(recv_boxed && infer_type(c, argv[k]) == TY_POLY)) ok = 0;
    if (!ok) continue;

    int saved[32];
    for (int k = 0; k < argc; k++) saved[k] = argv[k];  /* copy before realloc */
    int base = nt->count;
    int acc = chain_mk_to_a(c, recv);
    for (int k = 0; k < argc && acc >= 0; k++) {
      int t = chain_mk_to_a(c, saved[k]);
      acc = (t >= 0) ? chain_mk_concat(c, acc, t) : -1;
    }
    if (acc < 0) continue;   /* node-table OOM: leave the call alone */
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", &acc, 1);
    nt_node_set_str(nt, id, "name", "__enum_chain");
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "arguments", newargs);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_implicit_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;        /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "send") && !sp_streq(nm, "__send__") &&
                !sp_streq(nm, "public_send"))) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                      /* non-literal name: leave it */
    if (sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
        sp_streq(mname, "public_send")) continue;          /* don't re-trigger next pass */
    int nrest = argc - 1;
    if (nrest > 64) continue;                             /* absurd arity: leave it */
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);        /* copy before realloc */
    /* public_send dispatches only public methods: stamp the retargeted call
       so codegen raises NoMethodError for a private/protected target */
    int vis_enf = sp_streq(nm, "public_send");
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);              /* retarget the call */
    nt_node_set_ref(nt, id, "arguments", newargs);         /* drop the name arg */
    if (vis_enf) nt_node_set_str(nt, id, "vis_enforce", "1");
    else nt_node_set_str(nt, id, "send_blind", "1");   /* see desugar_public_send_recv */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `recv.public_send(:m, args)` with a literal name -> a direct `recv.m(args)`
   call stamped `vis_enforce`, so codegen raises NoMethodError for a
   private/protected target. send/__send__ keep the visibility-blind textual
   rewrite in spinel_parse.c (CRuby's send ignores visibility). Mirrors
   desugar_implicit_send's node-retarget model. */
int desugar_public_send_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;          /* explicit receiver only */
    const char *nm = nt_str(nt, id, "name");
    /* Also handle send/__send__ here, but ONLY when they target another
       send-family method (the nested `d.send(:send, :greet)` case, #2688):
       simple `d.send(:m)` is already lowered textually in spinel_parse.c, and
       the send-of-send it leaves behind unwinds one layer per pass here. */
    int is_pub = nm && sp_streq(nm, "public_send");
    int is_snd = nm && (sp_streq(nm, "send") || sp_streq(nm, "__send__"));
    if (!is_pub && !is_snd) continue;
    /* A blank-slate receiver has no #send / #public_send (only __send__ is
       BasicObject's): leave the call unretargeted, and the blank-slate gate
       raises CRuby's NoMethodError for the send itself (#2725). */
    if (!sp_streq(nm, "__send__")) {
      int bsrecv = nt_ref(nt, id, "receiver");
      TyKind bsrt = bsrecv >= 0 ? infer_type(c, bsrecv) : TY_UNKNOWN;
      if (ty_is_object(bsrt) && class_is_blank_slate(c, ty_object_class(bsrt))) continue;
      /* A socket's #send is the datagram write, not Object#send: CRuby picks
         by the receiver's class, and `u.send("ping", 0, host, port)` would
         otherwise retarget to a method named "ping" (#2922). */
      if (sp_streq(nm, "send") && bsrt == TY_IO && sp_feature_required("socket")) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                       /* runtime name: dyn_send_arms */
    int m_is_send = sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
                    sp_streq(mname, "public_send");
    (void)m_is_send;
    /* send/__send__ that spinel_parse.c already lowered never reach here;
       the ones it leaves are the send-of-send residue (`d.send(:greet)` after
       stripping the outer :send), which we finish retargeting. */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);
    nt_node_set_ref(nt, id, "arguments", newargs);
    if (is_pub && !m_is_send) nt_node_set_str(nt, id, "vis_enforce", "1");
    /* `x.send(:m)` ignores visibility, which is the whole point of it: a
       top-level `def` is Object's PRIVATE instance method, so a plain
       `x.m` cannot reach it but a send can. The retargeted call carries
       that permission, since nothing else distinguishes it from the
       ordinary call it now looks like (#4070 follow-up). */
    if (is_snd) nt_node_set_str(nt, id, "send_blind", "1");
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.step(to: X[, by: Y]) is the keyword form of n.step(X, Y) (by defaults to 1).
   The step passes read positional arguments, so a lone KeywordHashNode argument
   is otherwise mis-read as an integer limit (an int-from-pointer miscompile).
   Rewrite the to:/by: form into the positional list before those passes run. */
/* `expr => pattern` is defined to mean the one-arm `case expr; in pattern;
   end`, and is rewritten to it. The rightward form had a destructuring
   emitter of its own that bound direct local targets and checked an
   array's length, and nothing else: a class or value pattern (`v => Shape`,
   `5 => String`) raised nothing, a nested one bound nothing (#4047), a nil
   in an object slot was deconstructed, and a typed hash value refused the
   build. The case form's emitter checks every pattern kind and answers
   NoMatchingPatternError on a miss. */
int desugar_rightward_pattern(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_MatchRequiredNode) continue;
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) continue;
    int inn = nt_new_node(nt, "InNode");
    int st = nt_new_node(nt, "StatementsNode");
    if (inn < 0 || st < 0) continue;
    nt_node_set_ref(nt, inn, "pattern", pattern);
    nt_node_set_arr(nt, st, "body", NULL, 0);
    nt_node_set_ref(nt, inn, "statements", st);
    nt_node_set_type(nt, id, "CaseMatchNode");
    nt_node_set_ref(nt, id, "predicate", value);
    nt_node_set_arr(nt, id, "conditions", &inn, 1);
    nt_node_set_ref(nt, id, "else_clause", -1);
    comp_grow_node_arrays(c);
    c->nscope[inn] = c->nscope[id];
    c->nscope[st] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `expr in pattern` is the one-arm `case expr; in pattern then true; else
   false; end`, and is rewritten to it for the same reason the rightward form
   is: the predicate had a condition emitter of its own that read a subset
   of the patterns (a qualified array or hash pattern, `v in Pt[1, _]`, was
   refused), and it bound nothing, where CRuby binds the pattern's names. */
int desugar_match_predicate(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_MatchPredicateNode) continue;
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) continue;
    int inn = nt_new_node(nt, "InNode");
    int st = nt_new_node(nt, "StatementsNode");
    int tn = nt_new_node(nt, "TrueNode");
    int els = nt_new_node(nt, "ElseNode");
    int est = nt_new_node(nt, "StatementsNode");
    int fn = nt_new_node(nt, "FalseNode");
    if (inn < 0 || st < 0 || tn < 0 || els < 0 || est < 0 || fn < 0) continue;
    nt_node_set_ref(nt, inn, "pattern", pattern);
    nt_node_set_arr(nt, st, "body", &tn, 1);
    nt_node_set_ref(nt, inn, "statements", st);
    nt_node_set_arr(nt, est, "body", &fn, 1);
    nt_node_set_ref(nt, els, "statements", est);
    nt_node_set_type(nt, id, "CaseMatchNode");
    nt_node_set_ref(nt, id, "predicate", value);
    nt_node_set_arr(nt, id, "conditions", &inn, 1);
    nt_node_set_ref(nt, id, "else_clause", els);
    comp_grow_node_arrays(c);
    int made[] = { inn, st, tn, els, est, fn };
    for (int k = 0; k < 6; k++) c->nscope[made[k]] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_step_kwargs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "step")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;         /* Numeric#step has a receiver */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
    if (ac != 1 || !av) continue;
    int kh = av[0];
    if (!nt_type(nt, kh) || !sp_streq(nt_type(nt, kh), "KeywordHashNode")) continue;
    int to_v = -1, by_v = -1, other = 0;
    int en = 0; const int *els = nt_arr(nt, kh, "elements", &en);
    for (int i = 0; i < en; i++) {
      if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) { other = 1; break; }
      int key = nt_ref(nt, els[i], "key");
      const char *kn = (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode"))
                       ? nt_str(nt, key, "value") : NULL;
      if (kn && sp_streq(kn, "to")) to_v = nt_ref(nt, els[i], "value");
      else if (kn && sp_streq(kn, "by")) by_v = nt_ref(nt, els[i], "value");
      else { other = 1; break; }
    }
    if (other || to_v < 0) continue;   /* only the to:[/by:] form; `to` is required */
    int sc = c->nscope[id];
    int base = nt->count;
    if (by_v < 0) {
      by_v = nt_new_node(nt, "IntegerNode");
      if (by_v < 0) continue;
      nt_node_set_int(nt, by_v, "value", 1);
    }
    int pos[2] = { to_v, by_v };
    nt_node_set_arr(nt, args, "arguments", pos, 2);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* A receiverless `instance_exec(&b)` at top level (or in a free function) has an
   implicit self, so instance_exec rebinds self to the current self -- i.e. it
   does not change self at all, and is exactly `<block>.call(<args>)`. Rewrite it
   so the value form (`x = run { }`) lowers like any block-call forward instead of
   stranding an un-emittable top-level instance_exec (which links to an undefined
   function). Class-level instance_exec forwarders DO rebind self to the instance
   and are handled by their own trampoline splice, so they are left untouched. */
int desugar_toplevel_instance_exec(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;         /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "instance_exec")) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || c->scopes[sc].class_id >= 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int bexpr = nt_ref(nt, blk, "expression");
    if (bexpr < 0) continue;                               /* anonymous `&`: no name to call */
    nt_node_set_ref(nt, id, "receiver", bexpr);            /* receiver = forwarded block */
    nt_node_set_str(nt, id, "name", "call");
    nt_node_set_ref(nt, id, "block", -1);                  /* the block is now the receiver */
    changed = 1;
  }
  return changed;
}

/* `binding.local_variable_get(:name)` with a literal symbol naming an in-scope
   local is the idiom for reading a reserved-word parameter (`def f(then:);
   binding.local_variable_get(:then); end`), the only way to reference such a
   name -- a bare `then` is a keyword. An AOT compiler has no reified Binding, but
   this statically-decidable form is exactly the value of that local, so rewrite
   it to `<local>.itself` (an identity that yields the local's value). Other
   binding uses have no static answer and are rejected in codegen. */
int desugar_binding_lvget(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "local_variable_get")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    if (nt_ref(nt, recv, "receiver") >= 0) continue;      /* binding must be receiverless */
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "binding")) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "SymbolNode")) continue;
    const char *vn = nt_str(nt, av[0], "value");
    if (!vn) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !scope_local(&c->scopes[sc], vn)) continue;
    char *vnbuf = malloc(strlen(vn) + 1);   /* copy before nt_new_node may realloc vn's storage */
    if (!vnbuf) continue;
    strcpy(vnbuf, vn);
    int base = nt->count;
    int lread = nt_new_node(nt, "LocalVariableReadNode");
    if (lread < 0) { free(vnbuf); continue; }
    nt_node_set_str(nt, lread, "name", vnbuf);
    free(vnbuf);
    nt_node_set_ref(nt, id, "receiver", lread);            /* <local>.itself */
    nt_node_set_str(nt, id, "name", "itself");
    nt_node_set_ref(nt, id, "arguments", -1);
    /* The `binding` receiver is now orphaned; rename it to a sentinel no pass
       matches so the binding reject (which scans all nodes, including
       unreferenced ones) does not fire on it. */
    nt_node_set_str(nt, recv, "name", "__orphaned__");
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* `recv.send(name_expr, args)` with a NON-literal name and an explicit receiver:
   lower it to a static dispatch over the method names that appear as symbol
   literals in the program. For each candidate name `m` we synthesize an ordinary
   `recv.m(args)` call; analyze types each (honoring arity), and codegen keeps the
   ones that resolve on the receiver's type and emits `name == :m1 ? recv.m1(args)
   : ... : NoMethodError` (result poly). A runtime name that is not one of those
   literals -- or whose call does not resolve on the receiver -- is not
   dispatchable and raises NoMethodError. The literal-name forms are rewritten
   earlier (spinel_parse.c / desugar_implicit_send); this covers a name known only
   at runtime but drawn from the program's closed set of symbol literals. The arm
   node ids are stashed on the send under "dyn_send_arms" for codegen. */
/* A literal that could be a method name: an identifier with an optional
   `?`/`!`/`=` tail, or one of the operator methods. */
static int dsend_method_name_shaped(const char *v) {
  static const char *const ops[] = { "+", "-", "*", "/", "%", "**", "==", "!=", "<", "<=", ">", ">=",
    "<=>", "===", "=~", "!~", "<<", ">>", "&", "|", "^", "~", "!", "[]", "[]=", "+@", "-@", "call", NULL };
  for (int k = 0; ops[k]; k++) if (sp_streq(v, ops[k])) return 1;
  unsigned char ch = (unsigned char)v[0];
  if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch >= 0x80)) return 0;
  size_t i = 1;
  for (; v[i]; i++) {
    ch = (unsigned char)v[i];
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch >= 0x80) continue;
    break;
  }
  /* the tail: `?` or `!`, then `=` (a Struct member `verbose?` has the
     writer `verbose?=`), each optional */
  if (v[i] == '?' || v[i] == '!') i++;
  if (v[i] == '=') i++;
  return v[i] == 0;
}

int desugar_dynamic_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  static const char *const sends[] = { "send", "__send__", "public_send", NULL };
  /* a user-defined method named send/etc. resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn) for (int k = 0; sends[k]; k++) if (sp_streq(sn, sends[k])) return 0; }
  /* quick out: nothing to do unless some not-yet-lowered explicit-receiver send
     with a runtime name exists (the common case has none, so skip the scans). */
  { int any = 0;
    for (int id = 0; id < n0 && !any; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      const char *nm = nt_str(nt, id, "name"); if (!nm) continue;
      int is = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is = 1; break; }
      if (!is) continue;
      int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue;
      int a = nt_ref(nt, id, "arguments"); if (a < 0) continue;
      int ac = 0; const int *av = nt_arr(nt, a, "arguments", &ac);
      if (ac < 1 || !av) continue;
      const char *a0 = nt_type(nt, av[0]);
      if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;
      any = 1;
    }
    if (!any) return 0;
  }
  /* collect distinct symbol/string-literal names = candidate method names (send
     accepts either; a string name interns to the same symbol at the call).
     Only names shaped like a method name: a log message or a label with a
     space in it is not one. */
  /* The name lookups below go through hashed sets: every literal in the
     program is a candidate, and comparing each against the candidates so far,
     every scope and class, and every call name was (literals x names) per
     round (rubys/roundhouse#72). */
  char **cand = NULL; int ncand = 0, candcap = 0;
  ANameHash cand_set; memset(&cand_set, 0, sizeof cand_set);
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    const char *v = NULL;
    if (ty && sp_streq(ty, "SymbolNode")) v = nt_str(nt, id, "value");
    else if (ty && sp_streq(ty, "StringNode")) v = nt_str(nt, id, "content");
    if (!v || !*v || !dsend_method_name_shaped(v)) continue;
    int skip = 0;
    for (int k = 0; sends[k]; k++) if (sp_streq(v, sends[k])) { skip = 1; break; }  /* avoid send-of-send recursion */
    if (!skip && anh_has(&cand_set, v)) skip = 1;
    if (skip) continue;
    if (ncand == candcap) { candcap = candcap ? candcap * 2 : 16; cand = (char **)realloc(cand, sizeof(char *) * candcap); }
    cand[ncand++] = strdup(v);
    anh_add(&cand_set, cand[ncand - 1]);
  }
  anh_free(&cand_set);
  if (ncand == 0) { free(cand); return 0; }
  /* The arms are one synthesized call per candidate per send, each typed by
     the fixpoint, so the set is capped. The cap used to be a hard 128 over
     EVERY literal in the program, and a program with 129 unrelated strings
     lost the lowering entirely, with the refusal blaming a runtime name
     (#4649). Rank the candidates instead -- a name the program defines, then
     one it calls somewhere, then the rest -- and cut the tail, so the names
     that can be meant survive whatever else the program spells. */
  if (ncand > 1) {
    int *score = (int *)calloc((size_t)ncand, sizeof(int));
    /* the names the program defines: its methods, and its classes' readers
       and writers */
    ANameHash defined; memset(&defined, 0, sizeof defined);
    for (int s = 0; s < c->nscopes; s++)
      if (c->scopes[s].name && !anh_has(&defined, c->scopes[s].name)) anh_add(&defined, c->scopes[s].name);
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *cl = &c->classes[ci];
      for (int r = 0; r < cl->nreaders; r++) if (cl->readers[r] && !anh_has(&defined, cl->readers[r])) anh_add(&defined, cl->readers[r]);
      for (int w = 0; w < cl->nwriters; w++) if (cl->writers[w] && !anh_has(&defined, cl->writers[w])) anh_add(&defined, cl->writers[w]);
    }
    ANameHash called; memset(&called, 0, sizeof called);
    for (int id = 0; id < n0; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      const char *nm = nt_str(nt, id, "name");
      if (nm && !anh_has(&called, nm)) anh_add(&called, nm);
    }
    for (int k = 0; k < ncand; k++) {
      if (anh_has(&defined, cand[k])) score[k] = 2;
      if (score[k] < 2 && !((cand[k][0] >= 'a' && cand[k][0] <= 'z') || cand[k][0] == '_')) score[k] = 1;   /* an operator */
      if (score[k] < 1 && anh_has(&called, cand[k])) score[k] = 1;
    }
    anh_free(&defined); anh_free(&called);
    /* stable sort by score, descending */
    for (int i = 1; i < ncand; i++) {
      char *cv = cand[i]; int cs = score[i]; int j = i - 1;
      while (j >= 0 && score[j] < cs) { cand[j + 1] = cand[j]; score[j + 1] = score[j]; j--; }
      cand[j + 1] = cv; score[j + 1] = cs;
    }
    free(score);
  }
  if (ncand > 256) { for (int k = 256; k < ncand; k++) free(cand[k]); ncand = 256; }
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int is_send = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is_send = 1; break; }
    if (!is_send) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) {
      /* A receiverless `send(name, ...)` in a method is `self.send(name, ...)`:
         send ignores visibility, so the two reach the same (private) methods,
         and the explicit form already lowers (#4851). public_send differs --
         it refuses a private target either way -- and is left alone. Only a
         runtime name: a literal one is rewritten earlier. */
      if (sp_streq(nm, "public_send")) continue;
      Scope *ss = comp_scope_of(c, id);
      if (!ss || !ss->name) continue;
      int sa = nt_ref(nt, id, "arguments");
      int sac = 0; const int *sav = sa >= 0 ? nt_arr(nt, sa, "arguments", &sac) : NULL;
      if (sac < 1 || !sav) continue;
      NodeKind s0 = nt_kind(nt, sav[0]);
      if (s0 == NK_SymbolNode || s0 == NK_StringNode) continue;
      int sn = nt_new_node(nt, "SelfNode");
      if (sn < 0) continue;
      comp_grow_node_arrays(c);
      c->nscope[sn] = c->nscope[id];
      nt_node_set_ref(nt, id, "receiver", sn);
      recv = sn;
      changed = 1;
    }
    { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue; }  /* already lowered */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0 = nt_type(nt, argv[0]);
    if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;  /* literal: handled earlier */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64]; for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    int base = nt->count;
    int arms[256]; int narm = 0;
    for (int k = 0; k < ncand; k++) {
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      if (nrest) nt_node_set_arr(nt, na, "arguments", rest, nrest);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", cand[k]);
      /* The dispatch keys each arm on the NAME it was built for, so a later
         desugar that rewrites the name (`first` -> `[]`) leaves the arm
         unreachable and the send raises. Mark them as owned. */
      nt_node_set_int(nt, call, "dyn_arm", 1);
      nt_node_set_ref(nt, call, "arguments", na);
      /* public_send arms enforce visibility at the dispatch site */
      if (sp_streq(nm, "public_send")) nt_node_set_str(nt, call, "vis_enforce", "1");
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_send_arms", arms, narm);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  for (int k = 0; k < ncand; k++) free(cand[k]);
  free(cand);
  return changed;
}

/* `recv.respond_to?(:m)` with an explicit receiver and a literal method name:
   synthesize a probe `recv.m` call. The analyze fixpoint types the probe with
   the ordinary resolver, so its inferred type tells codegen whether spinel can
   actually dispatch `m` on that receiver (UNKNOWN = it cannot). The probe id is
   stashed on the respond_to? node under "rt_probes" and is analysis-only -- it is
   never emitted. The codegen fold reads it for primitive/builtin receivers,
   deriving the answer from the real dispatch instead of a hand-maintained method
   list; user-object receivers keep their visibility-aware chain resolution. The
   probe carries no arguments: builtin method inference keys on the receiver type
   and name (not arity), so an arg-taking method like `+`/`[]` still types. */
int desugar_respond_to_probe(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* a user-defined respond_to? resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "respond_to?")) return 0; }
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                         /* implicit self handled in the fold */
    { int pn = 0; nt_arr(nt, id, "rt_probes", &pn); if (pn > 0) continue; }  /* already probed */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) {
      qm = nt_str(nt, argv[0], "content");
      if (!qm) qm = nt_str(nt, argv[0], "unescaped");
    }
    if (!qm || !*qm) continue;                      /* non-literal name: not foldable */
    int base = nt->count;
    /* Probe two call shapes and let codegen answer true if EITHER types, since a
       single shape cannot satisfy every method: a block method (`each`, `map`)
       rejects a positional argument but needs a block, while an operator (`+`,
       `[]`) needs an argument. One probe carries an empty block (no arg), the
       other one dummy argument (the receiver, always type-available). A plain
       no-arg method (`upcase`) types under either. Builtin method inference keys
       on the receiver type and name, so a recognized method types by name; an
       unrecognized one is UNKNOWN under both. */
    int probes[3]; int np = 0;
    /* shape 0: recv.m  -- no argument, no block. Resolves the blockless
       enumerator forms (`each`, `reverse_each` infer TY_ENUMERATOR) and plain
       no-arg methods, using only codegen-safe inference. Every probe carries
       the rt_probe flag: it is analysis-only, and the param-binding passes
       must not let its dummy shapes type real lambda/method parameters. */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 1: recv.m { }  -- block, no argument */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int blkbody = nt_new_node(nt, "StatementsNode");
      int blk = nt_new_node(nt, "BlockNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && blkbody >= 0 && blk >= 0 && probe >= 0) {
        nt_node_set_ref(nt, blk, "body", blkbody);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_ref(nt, probe, "block", blk);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 2: recv.m(recv)  -- one dummy argument, no block */
    {
      int dummy_args[1] = { recv };
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_arr(nt, na, "arguments", dummy_args, 1);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* Sync the parallel arrays for every node allocated in this iteration --
       even a partial shape (an allocation failed mid-shape, so no probe was
       added) leaves nodes past `base` whose c->nscope would otherwise stay
       uninitialized, desyncing the arrays from nt->count for later passes. */
    if (nt->count > base) {
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    }
    if (np == 0) continue;
    nt_node_set_arr(nt, id, "rt_probes", probes, np);
    changed = 1;
  }
  return changed;
}

/* `recv.at(i)` is `recv[i]` for a single argument -- Array#at takes exactly
   one integer and answers what #[] does. Written as its own name it reached
   neither the typed array arms nor the boxed dispatch, so an Array read out
   of a container answered NoMethodError (#3821). Rewritten here, every path
   that knows #[] knows it. */
/* `arr.first` / `arr.last` on a statically ARRAY receiver are `arr[0]` and
   `arr[-1]`, exactly -- both answer nil on an empty array. Rewriting them onto
   the index route is not a shortcut: the shared-mutable-string machinery keys
   its alias analysis off the element read, and only `[]` carried a local
   binding through it, so `a = b.first; a << "Z"` bound a COPY and the
   container never saw the append (#4013). One route, one behaviour.
   The count forms (`first(2)`) answer a new Array and are left alone, as are
   Hash / Range / Enumerator / poly receivers, whose #first is a different
   method. */
int desugar_array_first_last(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int user_fl = 0;
  for (int k = 0; k < c->nclasses && !user_fl; k++)
    if (comp_method_in_chain(c, k, "first", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "first", NULL) ||
        comp_method_in_chain(c, k, "last", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "last", NULL)) user_fl = 1;
  if (user_fl) return 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "first") && !sp_streq(nm, "last"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_ref(nt, id, "block") >= 0) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;   /* a dynamic-send arm keeps its name */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
    if (argc != 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) || ty_is_obj_array(rt)) continue;
    int idx = nt_new_node(nt, "IntegerNode");
    if (idx < 0) continue;
    nt_node_set_int(nt, idx, "value", sp_streq(nm, "first") ? 0 : -1);
    int ia = nt_new_node(nt, "ArgumentsNode");
    if (ia < 0) continue;
    nt_node_set_arr(nt, ia, "arguments", &idx, 1);
    comp_grow_node_arrays(c);
    c->nscope[idx] = c->nscope[id];
    c->nscope[ia] = c->nscope[id];
    nt_node_set_ref(nt, id, "arguments", ia);
    nt_node_set_str(nt, id, "name", "[]");
    changed = 1;
  }
  return changed;
}

int desugar_array_at(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  /* a user class owning the name keeps its own dispatch */
  int user_at = 0;
  for (int k = 0; k < c->nclasses && !user_at; k++)
    if (comp_method_in_chain(c, k, "at", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "at", NULL)) user_at = 1;
  if (!user_at)
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "at")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_ref(nt, id, "block") >= 0) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;   /* same reason as first/last */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc != 1 || !argv) continue;
    const char *aty = nt_type(nt, argv[0]);
    if (aty && sp_streq(aty, "SplatNode")) continue;
    TyKind rt = infer_type(c, recv);
    /* Time#at and a Struct's own member reader are different methods */
    if (rt == TY_TIME || rt == TY_CLASS || ty_is_object(rt)) continue;
    /* Array#at takes an index, never a Range: rewriting it to #[] handed the
       slice form a call CRuby answers with a TypeError (#3924). */
    { TyKind aat = infer_type(c, argv[0]);
      if (aat == TY_RANGE || aat == TY_FLOAT_RANGE || aat == TY_STR_RANGE) continue; }
    nt_node_set_str(nt, id, "name", "[]");
    changed = 1;
  }
  return changed;
}

/* `recv.attr op= value` where the writer is a hand-written `def attr=`.
   Ruby desugars this into a reader call and a writer call; the emitter's own
   lowering goes straight to the backing ivar, which is right for an
   attr_accessor and wrong for a writer with a body, so it refused the shape
   outright (#3809). Rewrite it into the two calls Ruby means and let the
   ordinary call machinery handle them; the accessor case is left alone, where
   the direct ivar store is worth keeping.

   The receiver is evaluated twice, so a form with no work behind it and no
   side effect -- a local, self, an ivar or a constant -- is simply cloned.
   Any other receiver (`reg.value |= bit` through a reader, `self.reg.x`,
   `regs[0].x`) is evaluated once into a fresh local first, as CRuby does,
   and both calls read that local; those were refused outright (#4826). */
int desugar_call_op_write(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallOperatorWriteNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int val = nt_ref(nt, id, "value");
    if (recv < 0 || !attr || !op || val < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty) continue;
    int simple = sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "SelfNode") ||
                 sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "ConstantReadNode");
    char wname[300];
    snprintf(wname, sizeof wname, "%s=", attr);
    int has_def_writer = 0;
    for (int k = 0; k < c->nclasses && !has_def_writer; k++)
      if (comp_method_in_chain(c, k, wname, NULL) >= 0) has_def_writer = 1;
    if (!has_def_writer) continue;                 /* attr_writer: keep the store */
    char aname[300]; snprintf(aname, sizeof aname, "%s", attr);
    char opname[64]; snprintf(opname, sizeof opname, "%s", op);
    if (!simple) {
      /* (__cow_N = recv; __cow_N.attr = __cow_N.attr op value) */
      char tname[48]; snprintf(tname, sizeof tname, "__cow_%d", id);
      int first = nt->count;
      int tw = nt_new_node(nt, "LocalVariableWriteNode");
      int tr1 = nt_new_node(nt, "LocalVariableReadNode");
      int tr2 = nt_new_node(nt, "LocalVariableReadNode");
      int rd = nt_new_node(nt, "CallNode");
      int binargs = nt_new_node(nt, "ArgumentsNode");
      int bin = nt_new_node(nt, "CallNode");
      int wargs = nt_new_node(nt, "ArgumentsNode");
      int wc = nt_new_node(nt, "CallNode");
      int stmts = nt_new_node(nt, "StatementsNode");
      if (tw < 0 || tr1 < 0 || tr2 < 0 || rd < 0 || binargs < 0 || bin < 0 ||
          wargs < 0 || wc < 0 || stmts < 0) continue;
      nt_node_set_str(nt, tw, "name", tname);
      nt_node_set_ref(nt, tw, "value", recv);
      nt_node_set_str(nt, tr1, "name", tname);
      nt_node_set_str(nt, tr2, "name", tname);
      nt_node_set_ref(nt, rd, "receiver", tr1);
      nt_node_set_str(nt, rd, "name", aname);
      { int one[1]; one[0] = val; nt_node_set_arr(nt, binargs, "arguments", one, 1); }
      nt_node_set_ref(nt, bin, "receiver", rd);
      nt_node_set_str(nt, bin, "name", opname);
      nt_node_set_ref(nt, bin, "arguments", binargs);
      { int one[1]; one[0] = bin; nt_node_set_arr(nt, wargs, "arguments", one, 1); }
      nt_node_set_ref(nt, wc, "receiver", tr2);
      nt_node_set_str(nt, wc, "name", wname);
      nt_node_set_ref(nt, wc, "arguments", wargs);
      { int two[2]; two[0] = tw; two[1] = wc; nt_node_set_arr(nt, stmts, "body", two, 2); }
      nt_node_set_type(nt, id, "ParenthesesNode");
      nt_node_set_ref(nt, id, "body", stmts);
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_ref(nt, id, "value", -1);
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = first; j < nt->count; j++) c->nscope[j] = encl;
      /* locals were collected before the fixpoint; this one is new */
      scope_local_intern(comp_scope_of(c, tw), tname);
      changed = 1;
      continue;
    }
    int recv2 = nt_clone_subtree(nt, recv);
    if (recv2 < 0) continue;
    int base = nt->count;
    int rd = nt_new_node(nt, "CallNode");
    int binargs = nt_new_node(nt, "ArgumentsNode");
    int bin = nt_new_node(nt, "CallNode");
    int wargs = nt_new_node(nt, "ArgumentsNode");
    if (rd < 0 || binargs < 0 || bin < 0 || wargs < 0) continue;
    nt_node_set_ref(nt, rd, "receiver", recv);
    nt_node_set_str(nt, rd, "name", aname);
    { int one[1]; one[0] = val; nt_node_set_arr(nt, binargs, "arguments", one, 1); }
    nt_node_set_ref(nt, bin, "receiver", rd);
    nt_node_set_str(nt, bin, "name", opname);
    nt_node_set_ref(nt, bin, "arguments", binargs);
    { int one[1]; one[0] = bin; nt_node_set_arr(nt, wargs, "arguments", one, 1); }
    nt_node_set_type(nt, id, "CallNode");
    nt_node_set_ref(nt, id, "receiver", recv2);
    nt_node_set_str(nt, id, "name", wname);
    nt_node_set_ref(nt, id, "arguments", wargs);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = recv2; j < nt->count; j++) c->nscope[j] = encl;
    (void)base;
    changed = 1;
  }
  return changed;
}

/* `self.m` where self is main and `m` is a top-level def: the def is a
   private method of Object, and a literal `self.` receiver may call a
   private method (Feature #11297), so this is the receiverless call the
   top-level function already serves. It went through main's dispatch,
   where the def is not, and raised NoMethodError (#5061). The rule is
   syntactic, as CRuby's is: only a bare `self` node, not `(self)` and not
   a local holding it. */
int desugar_main_self_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    /* ...and the other way: a receiverless `instance_eval { }` or
       `instance_exec { }` on main is the `self.instance_eval` spelling,
       which the rebinding machinery serves; bare, it was refused */
    if (recv < 0 && nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec")) &&
        comp_method_index(c, name) < 0 && self_is_main(c, id)) {
      int sn = nt_new_node(nt, "SelfNode");
      if (sn < 0) continue;
      nt_node_set_ref(nt, id, "receiver", sn);
      comp_grow_node_arrays(c);
      c->nscope[sn] = c->nscope[id];
      changed = 1;
      continue;
    }
    if (recv < 0 || nt_kind(nt, recv) != NK_SelfNode) continue;
    const char *cop = nt_str(nt, id, "call_operator");
    if (cop && sp_streq(cop, "&.")) continue;
    if (!self_is_main(c, recv)) continue;
    if (comp_method_index(c, name) < 0) continue;   /* no top-level def */
    nt_node_set_ref(nt, id, "receiver", -1);
    changed = 1;
  }
  return changed;
}

/* `recv[k] ||= v`, `recv[k] &&= v` and `recv[k] op= v` on an instance of a
   user class with its own `[]` and `[]=`: the index-write emitters know the
   builtin containers only, and refused the shape (#5054). Rewritten into the
   calls Ruby means, the receiver and the key each evaluated once into a
   fresh local first:
     (__ixr_N = recv; __ixk_N = k; __ixr_N[__ixk_N] || (__ixr_N[__ixk_N] = v))
   with `&&` for `&&=`, and `__ixr_N[__ixk_N] = __ixr_N[__ixk_N] op v` for an
   operator. A key list other than one plain argument is left alone. */
static int ixw_call(NodeTable *nt, int recv_tmp_name_node_src, const char *rname, const char *name,
                    const int *args, int nargs) {
  (void)recv_tmp_name_node_src;
  int rr = nt_new_node(nt, "LocalVariableReadNode");
  int call = nt_new_node(nt, "CallNode");
  int an = nargs > 0 ? nt_new_node(nt, "ArgumentsNode") : -1;
  if (rr < 0 || call < 0 || (nargs > 0 && an < 0)) return -1;
  nt_node_set_str(nt, rr, "name", rname);
  nt_node_set_int(nt, rr, "depth", 0);
  nt_node_set_ref(nt, call, "receiver", rr);
  nt_node_set_str(nt, call, "name", name);
  if (an >= 0) {
    nt_node_set_arr(nt, an, "arguments", args, nargs);
    nt_node_set_ref(nt, call, "arguments", an);
  }
  return call;
}

static int ixw_read(NodeTable *nt, const char *name) {
  int r = nt_new_node(nt, "LocalVariableReadNode");
  if (r < 0) return -1;
  nt_node_set_str(nt, r, "name", name);
  nt_node_set_int(nt, r, "depth", 0);
  return r;
}

int desugar_index_op_write_user(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_IndexOrWriteNode && k != NK_IndexAndWriteNode && k != NK_IndexOperatorWriteNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    int val = nt_ref(nt, id, "value");
    int args = nt_ref(nt, id, "arguments");
    if (recv < 0 || val < 0 || args < 0 || nt_ref(nt, id, "block") >= 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc != 1 || !argv) continue;
    NodeKind ak = nt_kind(nt, argv[0]);
    if (ak == NK_SplatNode || ak == NK_BlockArgumentNode || ak == NK_KeywordHashNode) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int ci = ty_object_class(rt);
    if (comp_method_in_chain(c, ci, "[]", NULL) < 0 || comp_method_in_chain(c, ci, "[]=", NULL) < 0) continue;
    const char *op = k == NK_IndexOperatorWriteNode ? nt_str(nt, id, "binary_operator") : NULL;
    if (k == NK_IndexOperatorWriteNode && !op) continue;
    char opname[64]; if (op) snprintf(opname, sizeof opname, "%s", op);
    int key = argv[0];
    char rname[48], kname[48];
    snprintf(rname, sizeof rname, "__ixr_%d", id);
    snprintf(kname, sizeof kname, "__ixk_%d", id);
    int first = nt->count;
    int rw = nt_new_node(nt, "LocalVariableWriteNode");
    int kw = nt_new_node(nt, "LocalVariableWriteNode");
    if (rw < 0 || kw < 0) continue;
    nt_node_set_str(nt, rw, "name", rname); nt_node_set_int(nt, rw, "depth", 0);
    nt_node_set_ref(nt, rw, "value", recv);
    nt_node_set_str(nt, kw, "name", kname); nt_node_set_int(nt, kw, "depth", 0);
    nt_node_set_ref(nt, kw, "value", key);
    int k1 = ixw_read(nt, kname);
    int get = k1 >= 0 ? ixw_call(nt, -1, rname, "[]", &k1, 1) : -1;
    if (get < 0) continue;
    int last = -1;
    if (k == NK_IndexOperatorWriteNode) {
      int bin = nt_new_node(nt, "CallNode");
      int ba = nt_new_node(nt, "ArgumentsNode");
      int k2 = ixw_read(nt, kname);
      if (bin < 0 || ba < 0 || k2 < 0) continue;
      nt_node_set_arr(nt, ba, "arguments", &val, 1);
      nt_node_set_ref(nt, bin, "receiver", get);
      nt_node_set_str(nt, bin, "name", opname);
      nt_node_set_ref(nt, bin, "arguments", ba);
      int wa[2] = { k2, bin };
      last = ixw_call(nt, -1, rname, "[]=", wa, 2);
    }
    else {
      int k2 = ixw_read(nt, kname);
      if (k2 < 0) continue;
      int wa[2] = { k2, val };
      int set = ixw_call(nt, -1, rname, "[]=", wa, 2);
      int logic = nt_new_node(nt, k == NK_IndexOrWriteNode ? "OrNode" : "AndNode");
      if (set < 0 || logic < 0) continue;
      nt_node_set_ref(nt, logic, "left", get);
      nt_node_set_ref(nt, logic, "right", set);
      last = logic;
    }
    int stmts = nt_new_node(nt, "StatementsNode");
    if (last < 0 || stmts < 0) continue;
    int body[3] = { rw, kw, last };
    nt_node_set_arr(nt, stmts, "body", body, 3);
    nt_node_set_type(nt, id, "ParenthesesNode");
    nt_node_set_ref(nt, id, "body", stmts);
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "arguments", -1);
    nt_node_set_ref(nt, id, "value", -1);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = first; j < nt->count; j++) c->nscope[j] = encl;
    /* locals were collected before the fixpoint; these are new */
    Scope *sc = comp_scope_of(c, rw);
    scope_local_intern(sc, rname);
    scope_local_intern(sc, kname);
    changed = 1;
  }
  return changed;
}

/* `:sym.to_proc.call(recv, *args)` -> `recv.sym(*args)`. An explicit Symbol#to_proc
   followed by a call applies the named method to the first argument; with both the
   symbol and the call site statically known, it rewrites to an ordinary method call
   and the normal dispatch handles it. (The `&:sym` block form lowers separately; a
   to_proc whose receiver isn't a literal symbol, or that isn't immediately called,
   is left alone.) Mirrors desugar_implicit_send's node-retarget model. */
int desugar_symbol_to_proc_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "call")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "to_proc")) continue;
    int rargs = nt_ref(nt, recv, "arguments");
    if (rargs >= 0) { int rc = 0; nt_arr(nt, rargs, "arguments", &rc); if (rc != 0) continue; }
    int sym = nt_ref(nt, recv, "receiver");
    if (sym < 0 || !nt_type(nt, sym) || !sp_streq(nt_type(nt, sym), "SymbolNode")) continue;
    const char *mname = nt_str(nt, sym, "value");
    if (!mname || !*mname) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;            /* needs the receiver argument */
    int newrecv = argv[0];
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);         /* copy before realloc */
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_ref(nt, id, "receiver", newrecv);           /* receiver = first arg */
    nt_node_set_str(nt, id, "name", namebuf);               /* call the named method */
    nt_node_set_ref(nt, id, "arguments", newargs);          /* drop the receiver arg */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `recv.to_h { |e| [k, v] }` -> `recv.map { |e| [k, v] }.to_h`. The block-taking
   to_h maps each element to a [key, value] pair and collects the pairs into a
   hash; map already lowers the block for any iterable and the blockless to_h
   already builds a typed hash from an array of pairs, so rewriting onto that
   pair reuses both instead of adding a bespoke hash-building iterator. */
int desugar_to_h_block(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "to_h")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0) continue;                 /* need a receiver and a block */
    if (!nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    /* Only builtin iterables lower onto map{}.to_h. A Struct/Data or other user
       object with a block-taking to_h has its own member-pair path; rewriting it
       onto map would change the element protocol and mistype the result. */
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) && !ty_is_hash(rt) && rt != TY_RANGE && rt != TY_ENUMERATOR) continue;
    int base = nt->count;
    int mapargs = nt_new_node(nt, "ArgumentsNode");
    int mapcall = nt_new_node(nt, "CallNode");
    if (mapargs < 0 || mapcall < 0) continue;          /* node-table OOM: leave as-is */
    nt_node_set_arr(nt, mapargs, "arguments", NULL, 0); /* map takes no positional args */
    nt_node_set_ref(nt, mapcall, "receiver", recv);
    nt_node_set_str(nt, mapcall, "name", "map");
    nt_node_set_ref(nt, mapcall, "arguments", mapargs);
    nt_node_set_ref(nt, mapcall, "block", blk);
    nt_node_set_ref(nt, id, "receiver", mapcall);      /* to_h now consumes the mapped pairs */
    nt_node_set_ref(nt, id, "block", -1);              /* and no longer carries the block */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl; /* new nodes share the scope */
    changed = 1;
  }
  return changed;
}

/* Descend `root`'s subtree (bounded to scope `sc`) tracking the nearest enclosing
   StatementsNode entry (curr_st/curr_idx). On reaching `target`, report that entry
   -- the innermost same-scope top-level statement whose subtree contains target.
   A single O(N) pass that prunes at nested scope boundaries. */
static int tp_find_stmt(Compiler *c, int root, int target, int sc,
                        int curr_st, int curr_idx, int *out_st, int *out_idx) {
  if (root < 0 || c->nscope[root] != sc) return 0;   /* out of scope -> prune */
  if (root == target) {
    if (curr_st < 0) return 0;                        /* no enclosing statement */
    *out_st = curr_st; *out_idx = curr_idx; return 1;
  }
  NodeTable *nt = (NodeTable *)c->nt;
  int is_stmt = nt_type(nt, root) && sp_streq(nt_type(nt, root), "StatementsNode");
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, root, i);
    if (ch >= 0 && tp_find_stmt(c, ch, target, sc, curr_st, curr_idx, out_st, out_idx)) return 1;
  }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *a = nt_arr_at(nt, root, i, &n);
    for (int k = 0; k < n; k++) {
      if (a[k] < 0) continue;
      int nst = is_stmt ? root : curr_st, nidx = is_stmt ? k : curr_idx;
      if (tp_find_stmt(c, a[k], target, sc, nst, nidx, out_st, out_idx)) return 1;
    }
  }
  return 0;
}

/*out_idx set. */
static int tp_enclosing_stmt(Compiler *c, int id, int *out_st, int *out_idx) {
  int sc = c->nscope[id];
  if (sc < 0 || sc >= c->nscopes) return 0;
  return tp_find_stmt(c, c->scopes[sc].body, id, sc, -1, -1, out_st, out_idx);
}

/* `recv.iter(&obj)` where obj is a user object defining `to_proc`: Ruby calls
   obj.to_proc exactly ONCE to obtain the block. Hoist `__tproc_N = obj.to_proc`
   to the statement enclosing the call and rewrite the block argument to the
   hoisted local, so the value-callable desugar below forwards the once-computed
   proc (mirroring Ruby's `&obj` => `obj.to_proc` model, evaluated once). Declines
   -- leaving a loud reject -- when the enclosing statement can't be located. */
int desugar_to_proc_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || sp_streq(exty, "SymbolNode")) continue;  /* &:sym lowers separately */
    /* Only a user object defining #to_proc (not a Proc/Method value or symbol). */
    TyKind ct = infer_type(c, ex);
    if (!ty_is_object(ct)) continue;
    int cid = ty_object_class(ct);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_proc", NULL) < 0) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;  /* can't hoist -> leave (reject) */
    int encl = c->nscope[id];
    int base = nt->count;
    int exclone = nt_clone_subtree(nt, ex);
    int tpcall = nt_new_node(nt, "CallNode");
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (exclone < 0 || tpcall < 0 || wnode < 0 || rd < 0) continue;
    char tpname[48];
    snprintf(tpname, sizeof tpname, "__tproc_%d", id);
    nt_node_set_ref(nt, tpcall, "receiver", exclone);
    nt_node_set_str(nt, tpcall, "name", "to_proc");
    nt_node_set_ref(nt, tpcall, "arguments", -1);
    nt_node_set_ref(nt, tpcall, "block", -1);
    nt_node_set_str(nt, wnode, "name", tpname);
    nt_node_set_ref(nt, wnode, "value", tpcall);
    nt_node_set_str(nt, rd, "name", tpname);
    nt_node_set_ref(nt, blk, "expression", rd);  /* block arg now &__tproc_N */
    /* insert `wnode` before body[idx] in the enclosing StatementsNode */
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), tpname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `m(&(a >> b))`: an inline proc-composition (or any Proc-valued expression
   that is not already a simple read) as a block argument. The block-argument
   lowering wants a value it can name, so hoist the expression into a temp on
   the enclosing statement and pass that. The same composition assigned to a
   local first already compiled; this makes the inline form agree. (#3117) */
int desugar_proc_expr_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || !sp_streq(exty, "CallNode")) continue;
    /* only the Proc combinators: everything else keeps its own lowering
       (&:sym, &obj.to_proc, &method(:m), a lambda literal, ...) */
    const char *exn = nt_str(nt, ex, "name");
    if (!exn || (!sp_streq(exn, ">>") && !sp_streq(exn, "<<"))) continue;
    if (infer_type(c, ex) != TY_PROC) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;
    int encl = c->nscope[id];
    int base = nt->count;
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (wnode < 0 || rd < 0) continue;
    char bname[48];
    snprintf(bname, sizeof bname, "__blkexpr_%d", id);
    nt_node_set_str(nt, wnode, "name", bname);
    nt_node_set_ref(nt, wnode, "value", ex);
    nt_node_set_str(nt, rd, "name", bname);
    nt_node_set_ref(nt, blk, "expression", rd);
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), bname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `f(**obj)` where obj is a user object defining `#to_hash`: Ruby converts it
   through to_hash. Rewrite the splat's value from `obj` to `obj.to_hash` so the
   existing double-splat machinery (which pre-evaluates the source hash into a
   temp -- once) forwards the converted hash. Mirrors the `to_ary` splice
   coercion; once rewritten the value is a to_hash call and is not revisited. */
int desugar_to_hash_splat(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "AssocSplatNode")) continue;
    int val = nt_ref(nt, id, "value");
    if (val < 0) continue;
    if (nt_type(nt, val) && sp_streq(nt_type(nt, val), "CallNode") &&
        nt_str(nt, val, "name") && sp_streq(nt_str(nt, val, "name"), "to_hash")) continue;
    TyKind t = infer_type(c, val);
    if (!ty_is_object(t)) continue;
    int cid = ty_object_class(t);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_hash", NULL) < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, val);
    int call = nt_new_node(nt, "CallNode");
    if (clone < 0 || call < 0) {
      /* A partial allocation (clone appended nodes but the call node failed)
         leaves nodes past `base` whose c->nscope would otherwise stay
         uninitialized, desyncing the arrays from nt->count for later passes. */
      if (nt->count > base) {
        comp_grow_node_arrays(c);
        int encl = c->nscope[id];
        for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      }
      continue;
    }
    nt_node_set_ref(nt, call, "receiver", clone);
    nt_node_set_str(nt, call, "name", "to_hash");
    nt_node_set_ref(nt, call, "arguments", -1);
    nt_node_set_ref(nt, call, "block", -1);
    nt_node_set_ref(nt, id, "value", call);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `def lz; [1,2,3].lazy.map { }; end; lz.first` -- a lazy chain returned from a
   parameterless method. A lazy value has no runtime representation to return,
   so the method body is not emittable at all; splice a clone of the chain into
   the call site instead, which is exactly what a local alias already gets. The
   method then has no callers left and drops out as dead code. Restricted by
   lazy_method_chain to a self-free body, so the clone means the same thing
   where it lands. */
int desugar_lazy_method_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    int chain = lazy_method_chain(c, recv);
    if (chain < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, chain);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    if (clone < 0) continue;
    nt_node_set_ref(nt, id, "receiver", clone);
    /* the cloned stages carry block parameters that were interned in the
       CALLEE's scope; re-intern them here or their locals are never declared.
       Locals ASSIGNED inside a cloned block body need it too -- a block that
       writes a temp before using it emitted an undeclared identifier (#3367). */
    for (int j = base; j < nt->count; j++) {
      NodeKind jk = nt_kind(nt, j);
      if (jk == NK_BlockNode) {
        for (int k = 0; k < 4; k++) {
          const char *bp = block_param_name(c, j, k);
          if (!bp) break;
          scope_local_intern(&c->scopes[encl], bp);
        }
        continue;
      }
      if (jk == NK_LocalVariableWriteNode || jk == NK_LocalVariableTargetNode ||
          jk == NK_LocalVariableOperatorWriteNode || jk == NK_LocalVariableOrWriteNode ||
          jk == NK_LocalVariableAndWriteNode) {
        const char *wn = nt_str(nt, j, "name");
        if (wn) scope_local_intern(&c->scopes[encl], wn);
      }
    }
    changed = 1;
  }
  return changed;
}

/* The iterators whose poly arm yields exactly one boxed value per element,
   so an anonymous `&` forward on a poly receiver can become a one-parameter
   yielding block (#4625). */
static int fwd_poly_recv_one_param_iter(const char *name) {
  static const char *const names[] = {
    "each", "each_value", "each_key", "each_entry", "map", "collect", "flat_map",
    "filter_map", "select", "filter", "reject", "find", "detect", "find_index",
    "any?", "all?", "none?", "sort_by", "min_by", "max_by", "group_by",
    "partition", "count", "sum", "take_while", "drop_while", NULL };
  for (int k = 0; names[k]; k++) if (sp_streq(name, names[k])) return 1;
  return 0;
}

/* The anonymous `&` of the method around call `id`, once every forward
   through it has become a yielding block, names nothing any more: drop it,
   so the method is the plain yielding method the same body spells by hand
   (`def map = xs.map { |x| yield x }`). Kept, the nameless parameter put the
   method on the block-parameter path, where a yield inside a block that a
   poly receiver runs as a materialized proc found no block (#4625). */
static void fwd_drop_spent_anon_block_param(Compiler *c, int id, int n0) {
  NodeTable *nt = (NodeTable *)c->nt;
  Scope *ms = comp_scope_of(c, id);
  if (!ms || !ms->blk_param || ms->def_node < 0) return;
  int named = ms->blk_param[0] != 0;
  for (int j = 0; j < n0; j++) {
    if (comp_scope_of(c, j) != ms) continue;
    NodeKind k = nt_kind(nt, j);
    if (k == NK_CallNode) {
      int b = nt_ref(nt, j, "block");
      if (b < 0 || nt_kind(nt, b) != NK_BlockArgumentNode) continue;
      int ex = nt_ref(nt, b, "expression");
      if (ex < 0 && !named) return;   /* another anonymous forward still needs it */
      if (ex >= 0 && named && nt_kind(nt, ex) == NK_LocalVariableReadNode &&
          nt_str(nt, ex, "name") && sp_streq(nt_str(nt, ex, "name"), ms->blk_param)) return;
    }
    /* a named parameter read as a value anywhere keeps it */
    else if (named && (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode) &&
             nt_str(nt, j, "name") && sp_streq(nt_str(nt, j, "name"), ms->blk_param)) return;
  }
  int pn = nt_ref(nt, ms->def_node, "parameters");
  if (pn < 0) return;
  int bp = nt_ref(nt, pn, "block");
  if (bp < 0 || !nt_type(nt, bp) || !sp_streq(nt_type(nt, bp), "BlockParameterNode")) return;
  if (named ? !nt_str(nt, bp, "name") : nt_str(nt, bp, "name") != NULL) return;
  nt_node_set_ref(nt, pn, "block", -1);
  /* the named parameter's slot was registered as a Proc parameter: it is
     neither now, or the function prologue rooted a parameter it no longer
     declares */
  if (named) {
    LocalVar *plv = scope_local(ms, ms->blk_param);
    if (plv) { plv->is_param = 0; plv->is_block_param = 0; plv->type = TY_UNKNOWN; plv->is_cell = 0; }
  }
  free(ms->blk_param);
  ms->blk_param = NULL;
}

/* Is the method's `&blk` name read anywhere in scope `ms` other than as the
   BlockArgumentNode expression `only`? A `blk.call`, a `blk` handed on as a
   value, or a second forward keeps the parameter a value. */
static int fwd_blk_param_read_elsewhere(Compiler *c, Scope *ms, const char *name, int only) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (id == only || comp_scope_of(c, id) != ms) continue;
    NodeKind k = nt_kind(nt, id);
    if (k != NK_LocalVariableReadNode && k != NK_LocalVariableWriteNode) continue;
    const char *n = nt_str(nt, id, "name");
    if (n && sp_streq(n, name)) return 1;
  }
  return 0;
}

int desugar_value_callable_forwards(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    /* An anonymous `&` (`def each(&) = @items.each(&)`) forwards the method's
       own block, which the inline path splices at every caller: the method
       is marked yielding, and the builtin loop that consumes the forward
       reads the caller's literal block. But nothing typed that block's
       parameters from the container, and nothing connected the caller's
       block to the loop the way a `yield` does: the loop ran with an empty
       body, silently doing nothing (#4618). The forward becomes the block
       `{ |__fwd..| yield __fwd.. }` here, exactly as a named `&blk` becomes
       `{ |__fwd..| blk.call(__fwd..) }` below, and the yield does the rest. */
    int anon = ex < 0;
    TyKind ct = TY_UNKNOWN;
    /* A named `&blk` forwarded from the method that declared it is the
       method's own block just as an anonymous `&` is: `blk.call(x)` inside a
       block the callee splices at its yield had no `lv_blk` to read (the
       block parameter of an inlined method is not a value), where a
       `yield x` there is connected the way every nested yield is. Only when
       the name is used for nothing else, so the parameter can be dropped
       with the forward (fwd_drop_spent_anon_block_param). */
    if (!anon && nt_kind(nt, ex) == NK_LocalVariableReadNode) {
      Scope *ms = comp_scope_of(c, id);
      const char *xn = nt_str(nt, ex, "name");
      if (ms && ms->name && ms->blk_param && ms->blk_param[0] && xn && sp_streq(xn, ms->blk_param) &&
          !ms->blk_param_value_use && !fwd_blk_param_read_elsewhere(c, ms, xn, ex))
        anon = 1;
    }
    if (anon) {
      Scope *ms = comp_scope_of(c, id);
      if (!ms || !ms->name || !ms->blk_param) continue;
      if (ms->blk_param[0] && ex < 0) continue;
    }
    else {
    const char *exty = nt_type(nt, ex);
    if (!exty) continue;
    /* a constant read is as deterministic and side-effect-free as a local one,
       so `&SOME_LAMBDA` forwards the same way (#3689) */
    int simple_ref = sp_streq(exty, "LocalVariableReadNode") ||
                     sp_streq(exty, "InstanceVariableReadNode") ||
                     sp_streq(exty, "ConstantReadNode") ||
                     sp_streq(exty, "ConstantPathNode");
    /* `&method(:m)`: a deterministic method-object lookup, safe to re-evaluate */
    int method_obj = sp_streq(exty, "CallNode") && nt_str(nt, ex, "name") &&
                     sp_streq(nt_str(nt, ex, "name"), "method");
    /* `&->(x){...}`: an inline lambda literal, equivalent to the block itself;
       building it per element has no observable side effect */
    int inline_lambda = sp_streq(exty, "LambdaNode");
    if (!simple_ref && !method_obj && !inline_lambda) continue;
    ct = infer_type(c, ex);
    /* A poly local can hold a callable produced by an operation whose static
       type stays poly -- e.g. `procs.reduce(:>>)`, a composed Proc. Forward it
       as a value callable too (its `.call` dispatches at runtime); restricted
       to a bare local/ivar read so re-evaluation is side-effect-free (#3167). */
    if (ct != TY_PROC && ct != TY_METHOD && !(ct == TY_POLY && simple_ref)) continue;
    }
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int encl = c->nscope[id];
    TyKind rt = infer_type(c, recv);
    /* Hash `each`/`each_pair` yields the [k,v] pair, forwarded as a single array
       argument `c.call([k, v])` (built below) -- correct for every arity: a
       1-param callable gets the pair, a 2-param proc auto-splats it, a 2-param
       lambda raises exactly as CRuby's `Hash#each(&lambda)` does. A Method object
       takes the pair via its array ABI; a proc/lambda value's param is typed as
       the pair array by the call-site argument inference (a container arg
       overrides the bare-int default). */
    TyKind pty[4];
    int arity;
    if (sp_streq(name, "each_with_object")) {
      /* each_with_object(init) { |elem, memo| }: two params, the element and the
         accumulator. Array receivers only (a `{}` hash memo is unsupported even
         for a literal block). The memo type is recovered from how the callable
         fills it (ewo_memo_elem_type) by infer_block_params, so seed it UNKNOWN;
         the wrap_pair / hash logic below does not apply. */
      if (!ty_is_array(rt)) continue;
      arity = 2;
      pty[0] = ty_array_elem(rt);
      pty[1] = TY_UNKNOWN;
      /* an empty `{}` memo makes the accumulator a general boxed hash, so the
         memo param is typed accordingly (an empty `[]` stays UNKNOWN and is
         recovered from its fills by ewo_memo_elem_type). */
      int ewo_a = nt_ref(nt, id, "arguments");
      int ewo_ac = 0; const int *ewo_av = ewo_a >= 0 ? nt_arr(nt, ewo_a, "arguments", &ewo_ac) : NULL;
      if (ewo_ac >= 1 && ewo_av) {
        const char *seedty = nt_type(nt, ewo_av[0]);
        int seed_n = 0;
        if (seedty && sp_streq(seedty, "HashNode") &&
            (nt_arr(nt, ewo_av[0], "elements", &seed_n), seed_n == 0))
          pty[1] = TY_POLY_POLY_HASH;
        /* An empty `[]` memo the block never fills directly -- it hands it to a
           callable instead -- has no element evidence to recover, so type it as
           the general boxed array rather than leaving it unresolved (#3657). */
        if (seedty && sp_streq(seedty, "ArrayNode") &&
            (nt_arr(nt, ewo_av[0], "elements", &seed_n), seed_n == 0) &&
            ewo_memo_elem_type(c, id) == TY_UNKNOWN)
          pty[1] = TY_POLY_ARRAY;
      }
    }
    else if (anon && rt == TY_POLY && fwd_poly_recv_one_param_iter(name)) {
      /* A poly receiver (`@mutex.synchronize { @items.dup }.each(&)`, a
         snapshot handed out from under a lock) has no static element type
         to read a yield shape from, and the decline below left the forward
         on the inline path, which splices the caller's block into this
         method and runs it with the wrong self (#4625). The poly iterator
         arms yield one boxed value per element (a Hash's pair as one
         array, which the caller's block auto-splats), so the forward is
         `{ |__fwd| yield __fwd }` with a poly parameter. */
      arity = 1;
      pty[0] = TY_POLY;
    }
    else {
      arity = ty_block_yield(rt, name, pty, 4);
      if (arity < 1) continue;  /* not a context-free iterator (or recv unresolved) */
    }

    /* Hash forwarding. A Method object takes any hash iterator's yield directly
       through its array ABI (the [k,v] pair as one array for `each`, the bare
       key/value for `each_key`/`each_value`). A proc/lambda VALUE is reliable
       only for the `each` pair forwarded to a single param, whose pair-array
       type the call-site inference recovers (a container overriding the bare-int
       default). Every other hash + proc/lambda combination -- inline lambdas
       (cloned, no write to type from), multi-param procs (CRuby auto-splat), and
       the scalar `each_key`/`each_value` yields -- needs cross-procedural param
       typing not yet modeled, so decline to the pre-existing path. */
    int wrap_pair = 0;
    if (ty_is_hash(rt) && anon) {
      /* a yield hands the pair as one array, which the caller's block
         auto-splats into |k, v| or takes whole as |pair|, as Hash#each does */
      wrap_pair = (arity == 2);
    }
    else if (ty_is_hash(rt)) {
      if (ct == TY_METHOD) {
        wrap_pair = (arity == 2);  /* each: pair as array; each_key/value: bare value */
      }
      else {
        /* a proc/lambda (value or inline): the call-site inference types its
           params from the forwarded call. `each` to a 1-param callable gets the
           [k,v] pair as one array; to a 2-param one, k and v positionally
           (auto-splat by arity); each_key/each_value pass the bare key/value. */
        int cpc = fwd_callable_arity(c, ex);
        if (arity == 2 && cpc == 1) wrap_pair = 1;
        else if (arity == 2 && cpc == 2) wrap_pair = 0;
        else if (arity == 1) wrap_pair = 0;
        else continue;  /* arity-2 with unresolved callable arity */
      }
    }

    int base = nt->count;
    int proc_clone = anon ? -1 : nt_clone_subtree(nt, ex);  /* re-read the proc per element */
    if (!anon && proc_clone < 0) continue;

    int reqs[4], reads[4];
    char pn[48];
    int alloc_ok = 1;
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, reqs[k], "name", pn);
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, reads[k], "name", pn);
      if (reqs[k] < 0 || reads[k] < 0) { alloc_ok = 0; break; }
    }
    if (!alloc_ok) continue;  /* node-table OOM: leave the call in its &-form */
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", reqs, arity);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);

    int callargs = nt_new_node(nt, "ArgumentsNode");
    if (wrap_pair) {
      int pairarr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, pairarr, "elements", reads, 2);
      nt_node_set_arr(nt, callargs, "arguments", &pairarr, 1);
    }
    else nt_node_set_arr(nt, callargs, "arguments", reads, arity);
    int callnode;
    if (anon) {
      callnode = nt_new_node(nt, "YieldNode");
      nt_node_set_ref(nt, callnode, "arguments", callargs);
    }
    else {
      callnode = nt_new_node(nt, "CallNode");
      nt_node_set_ref(nt, callnode, "receiver", proc_clone);
      nt_node_set_str(nt, callnode, "name", "call");
      nt_node_set_ref(nt, callnode, "arguments", callargs);
      nt_node_set_ref(nt, callnode, "block", -1);
    }

    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 ||
        blocknode < 0)
      continue;  /* node-table OOM: a -1 id is an out-of-bounds node index below */
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);
    /* the block stands for the enclosing method's own block, which a call
       of that method may not have given: codegen's block_given? in the
       callee answers from the outer block, not from this literal */
    if (anon) nt_node_set_int(nt, blocknode, "fwd_yield", 1);

    nt_node_set_ref(nt, id, "block", blocknode);  /* call now takes a literal block */
    /* a named `&blk` forward that became a yield: its read is orphaned, and
       must not count as a use of the parameter */
    if (anon && ex >= 0) nt_node_set_str(nt, ex, "name", "__orphaned__");

    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;

    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      LocalVar *lv = scope_local_intern(bs, pn);
      lv->is_block_param = 1;
      lv->type = pty[k];
    }
    if (anon) fwd_drop_spent_anon_block_param(c, id, n0);
    changed = 1;
  }
  return changed;
}

/* `|x,|` -- a trailing comma in a BLOCK's parameter list -- is Ruby's way of
   saying "destructure the element and take the leading names, drop the rest".
   Prism spells the comma as an ImplicitRestNode in the rest slot, and nothing
   downstream read it, so `|x,|` behaved as `|x|` and bound the whole element:
   `[[1, 2]].map { |x,| x }` answered `[[1, 2]]` where Ruby answers `[1]`.
   Give the list a trailing parameter nobody names, which is what the rest is,
   and every multi-parameter path destructures it from there. A method
   definition's trailing comma means nothing and is left alone. */
int desugar_block_implicit_rest(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  /* A lambda is strict about its parameter list, and there the trailing comma
     changes nothing: `lambda { |a,| }` takes exactly one argument and raises
     on two. Only a block (or a proc, which is lenient) destructures. */
  char *is_lambda_params = (char *)calloc(n0 > 0 ? (size_t)n0 : 1, 1);
  if (!is_lambda_params) return 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    int bp = -1;
    if (ty && sp_streq(ty, "LambdaNode")) bp = nt_ref(nt, id, "parameters");
    else if (ty && sp_streq(ty, "CallNode")) {
      const char *cn = nt_str(nt, id, "name");
      if (!cn || !sp_streq(cn, "lambda")) continue;
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) bp = nt_ref(nt, blk, "parameters");
    }
    if (bp >= 0 && bp < n0) is_lambda_params[bp] = 1;
  }
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "BlockParametersNode")) continue;
    if (is_lambda_params[id]) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int rest = nt_ref(nt, pn, "rest");
    const char *rty = rest >= 0 ? nt_type(nt, rest) : NULL;
    if (!rty || !sp_streq(rty, "ImplicitRestNode")) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    if (!reqs || rn < 1 || rn > 30) continue;
    int copy[32];
    for (int k = 0; k < rn; k++) copy[k] = reqs[k];
    int p = nt_new_node(nt, "RequiredParameterNode");
    if (p < 0) continue;
    char nm[32]; snprintf(nm, sizeof nm, "__implicit_rest_%d", id);
    nt_node_set_str(nt, p, "name", nm);
    copy[rn] = p;
    nt_node_set_arr(nt, pn, "requireds", copy, rn + 1);
    nt_node_set_ref(nt, pn, "rest", -1);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  free(is_lambda_params);
  return changed;
}

/* `::Name` (a ConstantPathNode with no parent) is the top-level constant
   `Name`. The analyzer resolves a bare ConstantReadNode everywhere -- the
   class census, the builtin receivers (ENV, File, Math), the exception
   names -- while the rooted spelling was recognised only at the handful of
   sites that looked for it, so `::ENV.fetch(k)` inside a method typed the
   receiver unknown and raised NoMethodError at run time, and `::File.x` /
   `::Math.sqrt` were refused outright (#4801).

   Retype the node in place, which makes every one of those sites answer;
   the id stays, so the parent's ref still names it. Only for a name NOTHING
   defines inside a class or module body: where a nested definition of the
   same name exists, the two spellings mean different things and the rooted
   one is the only way to say "the top-level one" (`::RootNS::Mid::LEAF`
   beside a `Lex::RootNS`, `include ::Helper` inside an `Outer::Helper`,
   `defined?(::Rails)` inside a `Underscore::Rails`). A write target
   (`::X = 1`, `::X ||= v`) keeps its own node type: the writers read the
   path. */
static void rsc_mark_nested_defs(const NodeTable *nt, int id, unsigned char *seen,
                                 char **names, int *nn, int depth) {
  if (id < 0 || id >= nt->count || seen[id] || depth > 64) return;
  seen[id] = 1;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_ConstantWriteNode) {
    const char *nm = NULL;
    if (k == NK_ConstantWriteNode) nm = nt_str(nt, id, "name");
    else {
      int cp = nt_ref(nt, id, "constant_path");
      if (cp >= 0) nm = nt_str(nt, cp, "name");
    }
    if (nm && *nn < 4096) names[(*nn)++] = (char *)nm;
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++)
    rsc_mark_nested_defs(nt, nd->r[j].ref, seen, names, nn, depth + 1);
  for (int j = 0; j < nd->na; j++)
    for (int k2 = 0; k2 < nd->a[j].n; k2++)
      rsc_mark_nested_defs(nt, nd->a[j].ids[k2], seen, names, nn, depth + 1);
}

int desugar_root_scoped_constants(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  if (n0 <= 0) return 0;
  unsigned char *skip = (unsigned char *)calloc((size_t)n0, 1);
  if (!skip) return 0;
  /* a write target keeps its node type */
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strncmp(ty, "ConstantPath", 12) != 0) continue;
    if (sp_streq(ty, "ConstantPathNode") || sp_streq(ty, "ConstantPathTargetNode")) continue;
    int t = nt_ref(nt, id, "target");
    if (t >= 0 && t < n0) skip[t] = 1;
  }
  /* the names some class or module body defines: there the bare spelling
     resolves lexically and the two spellings differ */
  char **names = (char **)malloc(sizeof(char *) * 4096);
  int nn = 0;
  if (names) {
    unsigned char *seen = (unsigned char *)calloc((size_t)n0, 1);
    if (seen) {
      for (int id = 0; id < n0; id++) {
        NodeKind k = nt_kind(nt, id);
        if (k != NK_ClassNode && k != NK_ModuleNode) continue;
        int body = nt_ref(nt, id, "body");
        if (body >= 0) rsc_mark_nested_defs(nt, body, seen, names, &nn, 0);
      }
      free(seen);
    }
  }
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (skip[id] || nt_kind(nt, id) != NK_ConstantPathNode) continue;
    if (nt_ref(nt, id, "parent") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int shadowed = 0;
    for (int k = 0; k < nn; k++) if (sp_streq(nm, names[k])) { shadowed = 1; break; }
    if (shadowed) continue;
    nt_node_set_type(nt, id, "ConstantReadNode");
    changed = 1;
  }
  free(names);
  free(skip);
  return changed;
}

int desugar_sort_by_with_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "with_index")) continue;
    /* with_index(offset) shifts every index; the pair map below applies it */
    int wi_off = -1;
    {
      int wa = nt_ref(nt, id, "arguments");
      if (wa >= 0) {
        int wn = 0; const int *wv = nt_arr(nt, wa, "arguments", &wn);
        if (wn != 1 || !wv) continue;
        wi_off = wv[0];
      }
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "sort_by")) continue;
    if (nt_ref(nt, recv, "block") >= 0 || nt_ref(nt, recv, "arguments") >= 0) continue;
    int src = nt_ref(nt, recv, "receiver");
    if (src < 0) continue;

    int base = nt->count;
    /* src.each_with_index */
    int ewi = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, ewi, "receiver", src);
    nt_node_set_str(nt, ewi, "name", "each_with_index");
    nt_node_set_ref(nt, ewi, "arguments", -1);
    nt_node_set_ref(nt, ewi, "block", -1);
    /* with_index(off): shift the pair indexes before the key block reads them,
       `ewi.map { |v, i| [v, i + off] }` (#3763) */
    int pairs = ewi;
    if (wi_off >= 0) {
      char vn[48], inm[48];
      snprintf(vn, sizeof vn, "__wi_v_%d", id);
      snprintf(inm, sizeof inm, "__wi_i_%d", id);
      int vreq = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, vreq, "name", vn);
      int ireq = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, ireq, "name", inm);
      int oreqs[2] = { vreq, ireq };
      int oparams = nt_new_node(nt, "ParametersNode");
      nt_node_set_arr(nt, oparams, "requireds", oreqs, 2);
      int obp = nt_new_node(nt, "BlockParametersNode");
      nt_node_set_ref(nt, obp, "parameters", oparams);
      int vread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, vread, "name", vn);
      int iread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, iread, "name", inm);
      int offargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, offargs, "arguments", &wi_off, 1);
      int shifted = nt_new_node(nt, "CallNode");
      nt_node_set_ref(nt, shifted, "receiver", iread);
      nt_node_set_str(nt, shifted, "name", "+");
      nt_node_set_ref(nt, shifted, "arguments", offargs);
      nt_node_set_ref(nt, shifted, "block", -1);
      int pair[2] = { vread, shifted };
      int parr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, parr, "elements", pair, 2);
      int obody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, obody, "body", &parr, 1);
      int oblk = nt_new_node(nt, "BlockNode");
      nt_node_set_ref(nt, oblk, "parameters", obp);
      nt_node_set_ref(nt, oblk, "body", obody);
      pairs = nt_new_node(nt, "CallNode");
      nt_node_set_ref(nt, pairs, "receiver", ewi);
      nt_node_set_str(nt, pairs, "name", "map");
      nt_node_set_ref(nt, pairs, "arguments", -1);
      nt_node_set_ref(nt, pairs, "block", oblk);
      if (pairs < 0) continue;
    }
    /* .sort_by { |v, i| key } -- the with_index block, moved across */
    int sb = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, sb, "receiver", pairs);
    nt_node_set_str(nt, sb, "name", "sort_by");
    nt_node_set_ref(nt, sb, "arguments", -1);
    nt_node_set_ref(nt, sb, "block", blk);
    /* .map { |p| p[0] } */
    char pn[48]; snprintf(pn, sizeof pn, "__wi_pair_%d", id);
    int preq = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, preq, "name", pn);
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &preq, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int pread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, pread, "name", pn);
    int zero = nt_new_node(nt, "IntegerNode");
    nt_node_set_int(nt, zero, "value", 0);
    int idxargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, idxargs, "arguments", &zero, 1);
    int idx = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, idx, "receiver", pread);
    nt_node_set_str(nt, idx, "name", "[]");
    nt_node_set_ref(nt, idx, "arguments", idxargs);
    nt_node_set_ref(nt, idx, "block", -1);
    int mbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, mbody, "body", &idx, 1);
    int mblk = nt_new_node(nt, "BlockNode");
    if (ewi < 0 || sb < 0 || preq < 0 || params < 0 || bparams < 0 || pread < 0 ||
        zero < 0 || idxargs < 0 || idx < 0 || mbody < 0 || mblk < 0) continue;
    nt_node_set_ref(nt, mblk, "parameters", bparams);
    nt_node_set_ref(nt, mblk, "body", mbody);

    /* the with_index call BECOMES the map, so the parent link stays put */
    int line = (int)nt_int(nt, id, "node_line", 0);
    int file = (int)nt_int(nt, id, "node_file", 0);
    nt_node_reset(nt, id, "CallNode");
    nt_node_set_ref(nt, id, "receiver", sb);
    nt_node_set_str(nt, id, "name", "map");
    nt_node_set_ref(nt, id, "arguments", -1);
    nt_node_set_ref(nt, id, "block", mblk);
    if (line) nt_node_set_int(nt, id, "node_line", line);
    if (file) nt_node_set_int(nt, id, "node_file", file);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* 1 = route whatever the call's shape; 2 = only with a block, because the
   blockless form answers an Enumerator that keeps its source (`(1..6)
   .each_slice(2)` inspects as `1..6:each_slice(2)`, not as its elements). */
static int enum_via_to_a_name(const char *n) {
  /* take_while, drop_while, flat_map, collect_concat, minmax_by, grep and
     grep_v were here: they are Ruby definitions now (builtins/enumerable.rb)
     whose `each` walks a Hash or a Range as it is, an endless Range
     included, and neither arm of grep/grep_v is ever an Enumerator (both
     compute immediately), so unlike find_index/minmax below they have no
     remaining form that still wants this hop. */
  static const char *always[] = {
    "chunk_while", "slice_when",
    "slice_before", "slice_after", "sort", "minmax", "zip",
    "find_index", "uniq", NULL
  };
  static const char *with_block[] = {
    "each_slice", "each_cons", "each_entry", "cycle", NULL
  };
  for (int i = 0; always[i]; i++) if (sp_streq(n, always[i])) return 1;
  for (int i = 0; with_block[i]; i++) if (sp_streq(n, with_block[i])) return 2;
  return 0;
}

int desugar_enumerable_via_to_a(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int how = nm ? enum_via_to_a_name(nm) : 0;
    if (!how) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    /* skip one we already rewrote (its receiver IS the to_a) */
    const char *rnm = nt_kind(nt, recv) == NK_CallNode ? nt_str(nt, recv, "name") : NULL;
    if (rnm && sp_streq(rnm, "to_a")) continue;
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_hash(rt) && rt != TY_RANGE) continue;
    /* A blockless `.each` receiver is an Enumerator, whatever the chain rules
       type it as; the with-block group answers that Enumerator, and routing it
       through to_a answered the elements instead (#3857). */
    { int er = recv;
      if (er >= 0 && nt_kind(nt, er) == NK_CallNode && nt_ref(nt, er, "block") < 0) {
        const char *ernm = nt_str(nt, er, "name");
        if (ernm && (sp_streq(ernm, "each") || sp_streq(ernm, "each_with_index") ||
                     sp_streq(ernm, "reverse_each"))) continue;
      } }
    /* find_index WITH A BLOCK is a Ruby definition now (builtins/enumerable.rb)
       whose `each` walks a Hash or a Range as it is, an endless Range
       included, the same carve-out find/detect already have below; only the
       value-argument form (`find_index(v)`, no walk of its own -- kept on
       its own emitter) still wants the faithfully-raising to_a hop on an
       endless Range. */
    if (sp_streq(nm, "find_index") && nt_ref(nt, id, "block") >= 0) continue;
    /* A one-sided Range cannot become an array at all, and find / detect
       have their own walk from the bounded end: routing them through to_a
       turned a working search into a RangeError (#3863). The other names
       have no such walk and keep the (faithfully raising) hop. */
    if (rt == TY_RANGE && (sp_streq(nm, "find") || sp_streq(nm, "detect"))) {
      int rn7 = recv;
      while (rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "ParenthesesNode")) {
        int pb7 = nt_ref(nt, rn7, "body"); int pn7 = 0;
        const int *pp7 = pb7 >= 0 ? nt_arr(nt, pb7, "body", &pn7) : NULL;
        rn7 = pn7 == 1 ? pp7[0] : -1;
      }
      if (rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "RangeNode") &&
          nt_ref(nt, rn7, "right") < 0) continue;
    }
    /* Only where the call found no arm at all. A form that IS wired -- a
       blockless each_slice answering an Enumerator, say -- has a type, and
       rerouting it through to_a would answer an Array instead. */
    if (comp_ntype(c, id) != TY_UNKNOWN) continue;
    /* A blockless each_* over a Range has an arm already, and its Enumerator
       keeps the Range as its source (`(1..6).each_slice(2)` inspects as
       `1..6:each_slice(2)`); routing it would answer the elements instead. A
       Hash has no such arm, so it routes either way. */
    if (how == 2 && rt == TY_RANGE && nt_ref(nt, id, "block") < 0) continue;
    int base = nt->count;
    int toa = nt_new_node(nt, "CallNode");
    if (toa < 0) continue;
    nt_node_set_ref(nt, toa, "receiver", recv);
    nt_node_set_str(nt, toa, "name", "to_a");
    nt_node_set_ref(nt, toa, "arguments", -1);
    nt_node_set_ref(nt, toa, "block", -1);
    /* The with-block group answers the RECEIVER, not the pairs it walked, so
       mark the synthesized hop: inference and the value emitter read it to
       yield the original Hash / Range (#3842). A `to_a` the program wrote
       itself carries no mark and keeps answering its array. */
    if (how == 2) nt_node_set_str(nt, toa, "enum_recv", "1");
    nt_node_set_ref(nt, id, "receiver", toa);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `def m(a, ...) = callee(a, ...)` with a rest/kwrest callee becomes
   `def m(a, *, **) = callee(a, *, **)`. The __fwd_N model (#1288) binds the
   callee's params positionally, which is exact for fixed params and flattens
   a rest/kwrest. `*` / `**` follow the callee's params; blocks are not
   forwarded by this form, so any block involvement keeps the __fwd_N model. */
static int fwd_node_is(const NodeTable *nt, int id, const char *ty) {
  return id >= 0 && nt_type(nt, id) && sp_streq(nt_type(nt, id), ty);
}
/* highest node id under `id`; ids are pre-order, so [id, max] is the subtree */
static int fwd_subtree_max(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return -1;
  const SpNode *nd = &nt->nodes[id];
  int mx = id;
  for (int j = 0; j < nd->nr; j++) { int m = fwd_subtree_max(nt, nd->r[j].ref); if (m > mx) mx = m; }
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) { int m = fwd_subtree_max(nt, nd->a[j].ids[k]); if (m > mx) mx = m; }
  return mx;
}
static int fwd_subtree_uses_yield_or_block(const NodeTable *nt, int def) {
  int pn = nt_ref(nt, def, "parameters");
  if (pn >= 0 && nt_ref(nt, pn, "block") >= 0) return 1;
  int hi = fwd_subtree_max(nt, def);
  for (int id = def; id <= hi; id++) {
    if (fwd_node_is(nt, id, "YieldNode")) return 1;
    if (fwd_node_is(nt, id, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && sp_streq(nm, "block_given?")) return 1;
    }
  }
  return 0;
}
/* bit 1 = positional forwarding, bit 2 = keyword forwarding, bit 4 =
   block/yield; -1 no def, -2 defs disagree. Fixed parameters count too:
   forwarding to `def f(*a, k: 0)` needs both channels, as does forwarding to
   `def f(x, **k)`. */
static int def_shape_by_name(const NodeTable *nt, const char *name) {
  int shape = -1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int pn = nt_ref(nt, id, "parameters");
    int sh = 0;
    if (pn >= 0) {
      int rn = 0; nt_arr(nt, pn, "requireds", &rn);
      int on = 0; nt_arr(nt, pn, "optionals", &on);
      int postn = 0; nt_arr(nt, pn, "posts", &postn);
      int kn = 0; nt_arr(nt, pn, "keywords", &kn);
      if (rn > 0 || on > 0 || postn > 0) sh |= 1;
      if (kn > 0) sh |= 2;
      if (fwd_node_is(nt, nt_ref(nt, pn, "rest"), "RestParameterNode")) sh |= 1;
      if (fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "KeywordRestParameterNode")) sh |= 2;
    }
    if (fwd_subtree_uses_yield_or_block(nt, id)) sh |= 4;
    if (shape >= 0 && shape != sh) return -2;
    shape = sh;
  }
  return shape;
}
static int fwd_new_node_like(NodeTable *nt, int like, const char *ty) {
  int id = nt_new_node(nt, ty);
  if (id < 0) return -1;
  nt_node_set_int(nt, id, "node_line", nt_int(nt, like, "node_line", 0));
  nt_node_set_int(nt, id, "node_file", nt_int(nt, like, "node_file", 0));
  nt_node_set_int(nt, id, "node_col", nt_int(nt, like, "node_col", 0));
  return id;
}
static int any_call_passes_block(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, name) && nt_ref(nt, id, "block") >= 0) return 1;
  }
  return 0;
}
/* Rewrite the anonymous `&` forwards in one method body into reads of the
   method's synthetic block param. A nested def/class/module is a scope of its
   own (its `&` is its own method's); a block or lambda inside the body shares
   the method's block param. */
static int anon_block_fwd_rewrite(Compiler *c, int node) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return 0;
  int changed = 0;
  if (k == NK_BlockArgumentNode && nt_ref(nt, node, "expression") < 0) {
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (rd >= 0) {
      nt_node_set_str(nt, rd, "name", "__anon_block");
      nt_node_set_int(nt, rd, "depth", 0);
      nt_node_set_ref(nt, node, "expression", rd);
      comp_grow_node_arrays(c);
      changed = 1;
    }
    return changed;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) changed |= anon_block_fwd_rewrite(c, nt_ref_at(nt, node, i));
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, node, i, &n);
    /* the array may move when a new node grows the table: walk a copy */
    int *cp = n > 0 ? (int *)malloc(sizeof(int) * (size_t)n) : NULL;
    if (n > 0 && !cp) continue;
    if (n > 0) memcpy(cp, ids, sizeof(int) * (size_t)n);
    for (int j = 0; j < n; j++) changed |= anon_block_fwd_rewrite(c, cp[j]);
    free(cp);
  }
  return changed;
}

/* `def m(&) = keep(&)` -> `def m(&__anon_block) = keep(&__anon_block)`.
   An anonymous `&` had no name, so it was always yield-inlined: the analysis
   that decides whether a named &blk escapes (and must stay a real sp_Proc *
   param) reads the param's local reads, and there were none to read. A block
   handed through it to a method that keeps it was then spliced into the
   forwarder and materialized there, and its captures of the caller's locals
   were copied by value -- the writes were lost. Named, the param takes the
   same path a `&blk` does (mirrors __anon_kwrest for `**`). */
int desugar_anon_block_param(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int pn = nt_ref(nt, id, "parameters");
    int bp = pn >= 0 ? nt_ref(nt, pn, "block") : -1;
    if (bp < 0 || nt_kind(nt, bp) != NK_BlockParameterNode) continue;
    const char *bn = nt_str(nt, bp, "name");
    if (bn && bn[0]) continue;
    nt_node_set_str(nt, bp, "name", "__anon_block");
    anon_block_fwd_rewrite(c, nt_ref(nt, id, "body"));
    changed = 1;
  }
  return changed;
}

int desugar_forwarding_to_rest_callee(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int def = 0; def < n0; def++) {
    if (!fwd_node_is(nt, def, "DefNode")) continue;
    int pn = nt_ref(nt, def, "parameters");
    if (pn < 0 || !fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "ForwardingParameterNode")) continue;
    const char *dname = nt_str(nt, def, "name");
    if (!dname) continue;
    int hi = fwd_subtree_max(nt, def) + 1;
    int calls[n0]; int ncalls = 0; int shape = 0; int ok = 1;
    for (int id = def + 1; id < hi && id < n0; id++) {
      if (!fwd_node_is(nt, id, "CallNode")) continue;
      int args = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      if (ac < 1 || !av || !fwd_node_is(nt, av[ac - 1], "ForwardingArgumentsNode")) continue;
      const char *cn = nt_str(nt, id, "name");
      int sh = (nt_ref(nt, id, "receiver") < 0 && cn) ? def_shape_by_name(nt, cn) : -1;
      if (sh < 0 || nt_ref(nt, id, "block") >= 0) { ok = 0; break; }
      if (ncalls && sh != shape) { ok = 0; break; }
      shape = sh;
      calls[ncalls++] = id;
    }
    if (!ok || !ncalls || !(shape & 3) || (shape & 4)) continue;
    if (any_call_passes_block(nt, dname)) continue;
    int base = nt->count;
    /* def m(a, ...) -> def m(a, *, **) */
    if (shape & 1) {
      int rp = fwd_new_node_like(nt, pn, "RestParameterNode");
      if (rp < 0) continue;
      nt_node_set_ref(nt, pn, "rest", rp);
    }
    if (shape & 2) {
      int kp = fwd_new_node_like(nt, pn, "KeywordRestParameterNode");
      if (kp < 0) continue;
      nt_node_set_ref(nt, pn, "keyword_rest", kp);
    } else nt_node_set_ref(nt, pn, "keyword_rest", -1);
    /* callee(x, ...) -> callee(x, *, **) */
    for (int k = 0; k < ncalls; k++) {
      int call = calls[k];
      int args = nt_ref(nt, call, "arguments");
      int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
      int nargs[ac + 1]; int nn = 0;
      for (int i = 0; i < ac - 1; i++) nargs[nn++] = av[i];
      if (shape & 1) {
        int sp = fwd_new_node_like(nt, call, "SplatNode");
        if (sp < 0) continue;
        nt_node_set_ref(nt, sp, "expression", -1);
        nargs[nn++] = sp;
      }
      if (shape & 2) {
        int kh = fwd_new_node_like(nt, call, "KeywordHashNode");
        int as = fwd_new_node_like(nt, call, "AssocSplatNode");
        if (kh < 0 || as < 0) continue;
        nt_node_set_ref(nt, as, "value", -1);
        nt_node_set_arr(nt, kh, "elements", &as, 1);
        nargs[nn++] = kh;
      }
      nt_node_set_arr(nt, args, "arguments", nargs, nn);
    }
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[def];
    changed = 1;
  }
  return changed;
}

/* ---- builtins/: Enumerable written in Ruby (builtins/enumerable.rb) ----
   The file is spliced ahead of a program that mentions one of its names
   (spinel_parse.c, sp_splice_builtins). Its `module Enumerable` reopen would
   register a class of that name, which no builtin receiver dispatches
   through, and would bring the class machinery along for a program that
   never asks for it. So, before the scopes are built, each definition
   becomes a top-level function that takes its receiver as the first
   parameter:

     module Enumerable                  def __enum_each_with_object(__self, memo)
       def each_with_object(memo)   ->    __self.each { |x| yield x, memo }
         each { |x| yield x, memo }       memo
         memo                           end
       end
     end

   `self` reads as `__self`, a receiverless call that is not a Kernel
   function goes to `__self`, and the module node is dropped. The inliner
   then specializes the function for every call site's receiver type, as it
   does for any yielding method the program wrote. The calls are rewritten
   onto these functions inside the fixpoint (desugar_builtin_enum_calls),
   once the receiver's type is known. */
extern char **sp_builtin_enum_names;
extern int sp_builtin_enum_names_n;

int builtin_enum_name_index(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < sp_builtin_enum_names_n; i++)
    if (sp_streq(sp_builtin_enum_names[i], name)) return i;
  return -1;
}

/* the receiverless calls a builtin body may make that are NOT methods of
   the receiver: Kernel's functions */
static int bi_kernel_call_name(const char *nm) {
  static const char *const ks[] = {
    "raise", "puts", "p", "print", "printf", "format", "sprintf", "block_given?", "loop",
    "lambda", "proc", "rand", "srand", "sleep", "require", "require_relative", "catch",
    "throw", "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex", "gets",
    "exit", "abort", "at_exit", "binding", "warn", "fail", "freeze", "frozen?", "nil?",
    "respond_to?", "is_a?", "kind_of?", "instance_of?", "equal?", "eql?", "hash",
    "object_id", "dup", "clone", "itself", "then", "tap", "inspect", "to_s", "class", NULL };
  for (int k = 0; ks[k]; k++) if (sp_streq(nm, ks[k])) return 1;
  return 0;
}

static int bi_subtree_max(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return -1;
  const SpNode *nd = &nt->nodes[id];
  int mx = id;
  for (int j = 0; j < nd->nr; j++) { int m = bi_subtree_max(nt, nd->r[j].ref); if (m > mx) mx = m; }
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) { int m = bi_subtree_max(nt, nd->a[j].ids[k]); if (m > mx) mx = m; }
  return mx;
}

/* Retype every node of the subtree at `id` to a NilNode. The generic
   definition is cloned per call site and then left out of the program, but
   the passes that walk the node table by id rather than by tree still saw
   its DefNode, its block parameters and its locals, and declared each of
   them (rooted) in main. A NilNode is what those passes skip. */
static void bi_subtree_blank(NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return;
  SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) bi_subtree_blank(nt, nd->r[j].ref);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) bi_subtree_blank(nt, nd->a[j].ids[k]);
  nt_node_set_type(nt, id, "NilNode");
}

/* does any DefNode among the program's own nodes carry this name? (the
   scopes are not built when desugar_builtins runs) */
static int program_defines_name(const NodeTable *nt, int n0, const char *name) {
  for (int id = 0; id < n0; id++)
    if (nt_kind(nt, id) == NK_DefNode && nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), name)) return 1;
  return 0;
}

/* `self` in nodes [lo, hi] reads the `__self` parameter, and the
   receiverless calls there (Kernel's aside) take it as their receiver */
static void bi_self_to_local(NodeTable *nt, int lo, int hi) {
  for (int id = lo; id <= hi; id++) {
    NodeKind kind = nt_kind(nt, id);
    if (kind == NK_SelfNode) {
      nt_node_set_type(nt, id, "LocalVariableReadNode");
      nt_node_set_str(nt, id, "name", "__self");
      nt_node_set_int(nt, id, "depth", 0);
    }
    else if (kind == NK_CallNode && nt_ref(nt, id, "receiver") < 0) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || bi_kernel_call_name(nm)) continue;
      int rd = nt_new_node(nt, "LocalVariableReadNode"); if (rd < 0) return;
      nt_node_set_str(nt, rd, "name", "__self");
      nt_node_set_int(nt, rd, "depth", 0);
      nt_node_set_ref(nt, id, "receiver", rd);
    }
  }
}

int desugar_builtins(Compiler *c) {
  if (sp_builtin_enum_names_n == 0) return 0;
  NodeTable *nt = (NodeTable *)c->nt;
  int root = nt->root_id;
  int top = root >= 0 ? nt_ref(nt, root, "statements") : -1;
  if (top < 0) return 0;
  int changed = 0;
  int tn = 0; const int *tb = nt_arr(nt, top, "body", &tn);
  if (!tb || tn == 0) return 0;
  int *nb = (int *)malloc(sizeof(int) * (size_t)(tn + 64));
  if (!nb) return 0;
  int nbn = 0, cap = tn + 64;
  /* the generic definitions, one per builtin name, taken out of the module */
  int *gdef = (int *)malloc(sizeof(int) * (size_t)sp_builtin_enum_names_n);
  if (!gdef) { free(nb); return 0; }
  for (int i = 0; i < sp_builtin_enum_names_n; i++) gdef[i] = -1;
  int n0 = nt->count;   /* the program's own nodes: the call sites to clone for */
  for (int i = 0; i < tn; i++) {
    int st = tb[i];
    int cp = nt_kind(nt, st) == NK_ModuleNode ? nt_ref(nt, st, "constant_path") : -1;
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!mn || !sp_streq(mn, "Enumerable")) { nb[nbn++] = st; continue; }
    int body = nt_ref(nt, st, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    int all_builtin = bn > 0;
    for (int k = 0; k < bn; k++)
      if (nt_kind(nt, bb[k]) != NK_DefNode || builtin_enum_name_index(nt_str(nt, bb[k], "name")) < 0) all_builtin = 0;
    if (!all_builtin) { nb[nbn++] = st; continue; }   /* a program's own reopen: left as it was */
    for (int k = 0; k < bn; k++) {
      int def = bb[k];
      const char *name = nt_str(nt, def, "name");
      int bi = builtin_enum_name_index(name);
      /* the receiver becomes the first required parameter */
      int hi = bi_subtree_max(nt, def);
      int pn = nt_ref(nt, def, "parameters");
      if (pn < 0) { pn = nt_new_node(nt, "ParametersNode"); if (pn < 0) break; nt_node_set_ref(nt, def, "parameters", pn); }
      int sp = nt_new_node(nt, "RequiredParameterNode"); if (sp < 0) break;
      nt_node_set_str(nt, sp, "name", "__self");
      { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
        int *nr = (int *)malloc(sizeof(int) * (size_t)(rn + 1));
        if (!nr) break;
        nr[0] = sp; for (int j = 0; j < rn; j++) nr[j + 1] = reqs[j];
        nt_node_set_arr(nt, pn, "requireds", nr, rn + 1); free(nr); }
      /* `self` and the receiverless calls in the body */
      int dbody = nt_ref(nt, def, "body");
      int lo = dbody >= 0 ? dbody : def;
      bi_self_to_local(nt, lo, hi);
      /* the generic definition itself stays out of the program: the copies
         below are what the call sites use. It keeps a name of its own so
         that, orphaned in the node table, it cannot be mistaken for a
         method of the builtin's name. */
      { char gn[256]; snprintf(gn, sizeof gn, "__enum_%s", name); nt_node_set_str(nt, def, "name", gn); }
      if (bi >= 0) gdef[bi] = def;
    }
    /* the module node and its `Enumerable` constant leave the program too:
       orphaned but still a ConstantReadNode, the constant made every
       program that mentioned a builtin carry the class machinery (the
       prologue scan walks the table by id) */
    bi_subtree_blank(nt, cp);
    nt_node_set_type(nt, st, "NilNode");
    changed = 1;
  }
  if (!changed) { free(gdef); free(nb); return 0; }
  /* One copy per call site. A method's parameters are typed by the union of
     its call sites, so one shared definition called on an IntArray here and
     a Hash there would carry a poly receiver and a poly memo everywhere;
     with its own copy each site's parameters take that site's types, and
     the inliner specializes the copy for the receiver it sees, as it does
     for a yielding method the program wrote for one purpose. The copy is
     named `__enum_<m>__<site>`, recorded on the call, and the call is
     rewritten onto it in the fixpoint once the receiver's type says the
     builtin serves it (desugar_builtin_enum_calls). A copy no site ends up
     calling is unreachable and never reaches the generated C. */
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *cn0 = nt_str(nt, id, "name");
    /* `enum.with_object(memo)` is renamed to each_with_object by the
       Enumerator desugar inside the fixpoint, after this pass: give it its
       copy under the name it will have. `recv.m(args).each { blk }` becomes
       `recv.m(args) { blk }` there too, on the OUTER node (#4332), so that
       node takes a copy of the inner's name. */
    if (cn0 && sp_streq(cn0, "each") && nt_ref(nt, id, "block") >= 0) {
      int er = nt_ref(nt, id, "receiver");
      if (er >= 0 && nt_kind(nt, er) == NK_CallNode && nt_ref(nt, er, "block") < 0) cn0 = nt_str(nt, er, "name");
    }
    if (cn0 && sp_streq(cn0, "with_object")) cn0 = "each_with_object";
    /* collect_concat is flat_map under another name: the call takes the
       name the definition has (a program that defines collect_concat
       itself keeps its call) */
    if (cn0 && sp_streq(cn0, "collect_concat") && builtin_enum_name_index("flat_map") >= 0 &&
        !program_defines_name(nt, n0, "collect_concat")) {
      cn0 = "flat_map";
      nt_node_set_str(nt, id, "name", cn0);
    }
    /* detect is find under another name, the same way */
    if (cn0 && sp_streq(cn0, "detect") && builtin_enum_name_index("find") >= 0 &&
        !program_defines_name(nt, n0, "detect")) {
      cn0 = "find";
      nt_node_set_str(nt, id, "name", cn0);
    }
    int bi = builtin_enum_name_index(cn0);
    if (bi < 0 || gdef[bi] < 0) continue;
    int copy = nt_clone_subtree(nt, gdef[bi]);
    if (copy < 0) break;
    char cn[256]; snprintf(cn, sizeof cn, "__enum_%s__%d", sp_builtin_enum_names[bi], id);
    nt_node_set_str(nt, copy, "name", cn);
    nt_node_set_int(nt, id, "enum_copy", copy);
    if (nbn >= cap) { cap *= 2; int *g = (int *)realloc(nb, sizeof(int) * (size_t)cap); if (!g) break; nb = g; }
    nb[nbn++] = copy;
  }
  nt_node_set_arr(nt, top, "body", nb, nbn);
  for (int i = 0; i < sp_builtin_enum_names_n; i++) if (gdef[i] >= 0) bi_subtree_blank(nt, gdef[i]);
  comp_grow_node_arrays(c);
  free(gdef); free(nb);
  return 1;
}

/* Whether any assignment in scope `s` writes the local `vn`. */
static int scope_writes_local(Compiler *c, Scope *s, const char *vn) {
  const NodeTable *nt = c->nt;
  static const NodeKind kinds[] = {
    NK_LocalVariableWriteNode, NK_LocalVariableOperatorWriteNode,
    NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode, NK_LocalVariableTargetNode,
  };
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    NT_FOREACH_KIND(nt, kinds[k], id) {
      const char *wn = nt_str(nt, id, "name");
      if (wn && sp_streq(wn, vn) && comp_scope_of(c, id) == s) return 1;
    }
  }
  return 0;
}

/* Keep the arm `ans` of the IfNode/UnlessNode `id` whose predicate is
   `pred`, blank the other, and forget the scope's local types (see below).
   Answers 0 when a node could not be made. */
static int fold_if_arm(Compiler *c, int id, int pred, int ans, Scope *s, const unsigned char *is_elsif) {
  NodeTable *nt = (NodeTable *)c->nt;
  NodeKind k = nt_kind(nt, id);
  int then_s = nt_ref(nt, id, "statements");
  int els = nt_ref(nt, id, k == NK_IfNode ? "subsequent" : "else_clause");
  int keep = ans ? then_s : els;
  int drop = ans ? els : then_s;
  if (keep >= 0 && nt_kind(nt, keep) == NK_ElseNode) keep = nt_ref(nt, keep, "statements");
  if (keep >= 0 && nt_kind(nt, keep) != NK_StatementsNode) {
    /* an `elsif` chain: the surviving arm is the next IfNode itself */
    int st = nt_new_node(nt, "StatementsNode");
    if (st < 0) return 0;
    nt_node_set_arr(nt, st, "body", &keep, 1);
    keep = st;
  }
  bi_subtree_blank(nt, pred);
  if (drop >= 0) bi_subtree_blank(nt, drop);
  nt_node_set_ref(nt, id, "predicate", -1);
  nt_node_set_ref(nt, id, "statements", -1);
  nt_node_set_ref(nt, id, k == NK_IfNode ? "subsequent" : "else_clause", -1);
  if (is_elsif && is_elsif[id]) {
    if (keep < 0) { keep = nt_new_node(nt, "StatementsNode"); if (keep < 0) return 0; }
    nt_node_set_type(nt, id, "ElseNode");
    nt_node_set_ref(nt, id, "statements", keep);
  }
  else if (keep >= 0) {
    nt_node_set_type(nt, id, "BeginNode");
    nt_node_set_ref(nt, id, "statements", keep);
  }
  else nt_node_set_type(nt, id, "NilNode");
  /* The locals of this scope were typed with the dropped arm's evidence
     in, and an empty-literal write carries a local's previous type from
     round to round (infer_write_types), so the type would never move:
     `out = []` beside `out << v` stayed a boxed array after `out.concat(v)`
     became its only fill. Forget them; the next round re-derives each from
     the evidence that is left. */
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *l = &s->locals[i];
    if (l->is_param || l->is_block_param || l->rbs_seeded) continue;
    l->type = TY_UNKNOWN; l->gc_root = (int)TY_UNKNOWN;
  }
  return 1;
}

/* `if v.is_a?(Array)` / `kind_of?` on a local whose type has settled is
   decided here: a typed array is one, a scalar, a hash, an object or a
   range is not, and the arm not taken leaves the program (blanked, so the
   passes that walk the table by id stop typing what it wrote). A boxed
   value, a boxed array (which may be nil, #4567) and an unresolved local
   keep the run-time test. The fixpoint's optimistic rounds are left alone:
   a type that is still moving must not decide an arm away.
   builtins/enumerable.rb's flat_map is the case this exists for: with both
   arms typed, `out << v` beside `out.concat(v)` made every result a boxed
   array where the emitter it replaced answered the element's own kind. */
int fold_static_is_a(Compiler *c) {
  if (g_infer_optimistic) return 0;
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  /* an `elsif` is the IfNode its parent's `subsequent` names: its
     replacement has to stay an ElseNode there, which is what the parent's
     emitter reads after its own arm */
  unsigned char *is_elsif = (unsigned char *)calloc((size_t)(n0 ? n0 : 1), 1);
  for (int id = 0; is_elsif && id < n0; id++) {
    if (nt_kind(nt, id) != NK_IfNode) continue;
    int sub = nt_ref(nt, id, "subsequent");
    if (sub >= 0 && sub < n0 && nt_kind(nt, sub) == NK_IfNode) is_elsif[sub] = 1;
  }
  /* the call site of each builtin clone (`enum_copy`), for the omitted-
     parameter test below */
  int *copy_site = (int *)malloc(sizeof(int) * (size_t)(n0 ? n0 : 1));
  for (int id = 0; copy_site && id < n0; id++) copy_site[id] = -1;
  NT_FOREACH_KIND(nt, NK_CallNode, cid) {
    int cp = (int)nt_int(nt, cid, "enum_copy", -1);
    if (copy_site && cp >= 0 && cp < n0) copy_site[cp] = cid;
  }
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_IfNode && k != NK_UnlessNode) continue;
    int pred = nt_ref(nt, id, "predicate");
    /* `if n` on an optional parameter of a builtin clone whose one call site
       leaves it out: the parameter is its nil default, so the arm is
       decided. `min_by(n = nil)` and `tally(hash = nil)` otherwise kept both
       arms, and the answer came back boxed. */
    if (pred >= 0 && nt_kind(nt, pred) == NK_LocalVariableReadNode && copy_site) {
      int ans = -1;
      const char *vn = nt_str(nt, pred, "name");
      Scope *ps = vn ? comp_scope_of(c, pred) : NULL;
      if (ps && ps->def_node >= 0 && ps->def_node < n0 && copy_site[ps->def_node] >= 0 && ps->pdefault) {
        int call = copy_site[ps->def_node];
        int ca = nt_ref(nt, call, "arguments");
        int cn = 0; const int *cv = ca >= 0 ? nt_arr(nt, ca, "arguments", &cn) : NULL;
        int plain = 1;
        for (int j = 0; j < cn; j++) {
          NodeKind ak = nt_kind(nt, cv[j]);
          if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode) plain = 0;
        }
        for (int pi = 0; plain && pi < ps->nparams; pi++) {
          if (!ps->pnames || !ps->pnames[pi] || !sp_streq(ps->pnames[pi], vn)) continue;
          int dv = ps->pdefault[pi];
          if (pi >= cn && dv >= 0 && nt_kind(nt, dv) == NK_NilNode && !scope_writes_local(c, ps, vn)) ans = 0;
          break;
        }
      }
      if (ans < 0) continue;
      if (k == NK_UnlessNode) ans = !ans;
      changed |= fold_if_arm(c, id, pred, ans, ps, is_elsif);
      continue;
    }
    if (pred < 0 || nt_kind(nt, pred) != NK_CallNode || nt_ref(nt, pred, "block") >= 0) continue;
    const char *nm = nt_str(nt, pred, "name");
    if (!nm || (!sp_streq(nm, "is_a?") && !sp_streq(nm, "kind_of?"))) continue;
    int recv = nt_ref(nt, pred, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    int args = nt_ref(nt, pred, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1 || !av || nt_kind(nt, av[0]) != NK_ConstantReadNode) continue;
    const char *kn = nt_str(nt, av[0], "name");
    if (!kn || !sp_streq(kn, "Array")) continue;
    const char *vn = nt_str(nt, recv, "name");
    Scope *s = vn ? comp_scope_of(c, recv) : NULL;
    LocalVar *lv = s ? scope_local(s, vn) : NULL;
    if (!lv) continue;
    TyKind t = lv->type;
    if (t == TY_UNKNOWN || t == TY_POLY || t == TY_POLY_ARRAY || t == TY_NIL || t == TY_VOID) continue;
    int ans = ty_is_array(t) || ty_is_obj_array(t);
    if (k == NK_UnlessNode) ans = !ans;
    if (!fold_if_arm(c, id, pred, ans, s, is_elsif)) break;
    changed = 1;
  }
  free(is_elsif);
  free(copy_site);
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* `recv.m(args) { }` with `m` a builtins name, on a receiver the builtin
   serves: an Array, a Hash, a Range, an Enumerator, a class that includes
   Enumerable without defining `m` itself, or a value known only at run time
   when no class in the program defines `m`. Rewritten into
   `__enum_m(recv, args) { }`; runs in the fixpoint so the receiver's type has
   settled. A receiver whose class defines `m` keeps its call. */
static void mark_subtree_ids(const NodeTable *nt, int id, unsigned char *mark) {
  if (id < 0 || id >= nt->count || mark[id]) return;
  mark[id] = 1;
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) mark_subtree_ids(nt, nd->r[j].ref, mark);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) mark_subtree_ids(nt, nd->a[j].ids[k], mark);
}

/* An optional/keyword parameter's default is hoisted to the CALL site (any
   one of them, however many there are) rather than evaluated inside the
   method's own body: the top-of-function local declaration a yielding call's
   spliced block param needs is emitted for the method that lexically OWNS
   the default (where it is dead) rather than for whichever caller actually
   evaluates it, so a caller's `lv_<param>` comes out undeclared there
   (independent of this migration -- a hand-written yielding method used the
   same way hits it too). find/detect's own typed/poly-array emitters below
   are self-contained (they declare the block param inside their own loop,
   the way the deleted C emitters for the other migrated names used to), so a
   find/detect call reachable from a default value keeps its emitter instead
   of taking the rewrite. */
static unsigned char *find_calls_in_param_defaults(const NodeTable *nt, int n0) {
  unsigned char *mark = (unsigned char *)calloc((size_t)(n0 ? n0 : 1), 1);
  if (!mark) return NULL;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "OptionalParameterNode") && !sp_streq(ty, "OptionalKeywordParameterNode")))
      continue;
    int v = nt_ref(nt, id, "value");
    if (v >= 0) mark_subtree_ids(nt, v, mark);
  }
  return mark;
}

/* each / each_with_index / zip / map / reduce whose block receives the row
   (and, for each_with_index, the index). A destructure of the row, a splat,
   or a block argument stays on the poly path: the pointer-array emitters
   bind one row pointer, not the row's elements. */
int nested_row_iter_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || nt_kind(nt, block) != NK_BlockNode) return 0;
  if (block_rest_name(c, block) || block_param_is_multi(c, block, 0)) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  int np = 0;
  while (block_param_name(c, block, np)) np++;
  if ((sp_streq(nm, "each") || sp_streq(nm, "reverse_each") || sp_streq(nm, "each_entry")) &&
      argc == 0 && np <= 1) return 1;
  if (sp_streq(nm, "each_with_index") && argc == 0 && np <= 2) return 1;
  if ((sp_streq(nm, "map") || sp_streq(nm, "collect")) && argc == 0 && np <= 1) return 1;
  if ((sp_streq(nm, "reduce") || sp_streq(nm, "inject")) && argc <= 1 && np == 2) return 1;
  if (sp_streq(nm, "zip") && argc == 1 && (np == 1 || np == 2)) return 1;
  return 0;
}

/* Does `node` hold a `break` that leaves the block it sits in, rather than a
   loop or a block nested inside it? */
static int block_body_breaks(const NodeTable *nt, int node) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_BreakNode) return 1;
  if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (block_body_breaks(nt, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) if (block_body_breaks(nt, ids[j])) return 1;
  }
  return 0;
}

/* `enum.m(args) { ... break ... }` on an Enumerator -> `__enumw_m(enum, args)
   { ... }` (builtins/enumerator.rb). The typed emitters and enumerable.rb's
   copies of these names take an Enumerator receiver through to_a first,
   which never returns for an endless one, so a block written to `break` out
   of it never ran; each_entry and each_slice/each_cons answered the
   Enumerator itself even when the block broke. The helpers walk the
   receiver with `each`, which drives it one element at a time, and a
   `break` leaves the helper with its value, as it leaves the method in Ruby.
   Only a block that can break is moved: without one the walk runs to the
   end either way, and the typed emitters are the faster path. */
int desugar_enum_walk_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (comp_method_index(c, "__enumw_map") < 0) return 0;   /* not spliced */
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    const char *hn = NULL;
    if ((sp_streq(name, "map") || sp_streq(name, "collect")) && an == 0) hn = "__enumw_map";
    else if ((sp_streq(name, "select") || sp_streq(name, "filter")) && an == 0) hn = "__enumw_select";
    else if (sp_streq(name, "reject") && an == 0) hn = "__enumw_reject";
    else if (sp_streq(name, "filter_map") && an == 0) hn = "__enumw_filter_map";
    else if ((sp_streq(name, "each_with_object") || sp_streq(name, "with_object")) && an == 1) hn = "__enumw_each_with_object";
    else if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) && an <= 1) hn = an ? "__enumw_inject1" : "__enumw_inject0";
    else if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && an == 1)
      hn = name[5] == 's' ? "__enumw_each_slice" : "__enumw_each_cons";
    else if (sp_streq(name, "each_entry") && an == 0) hn = "__enumw_each_entry";
    else if (sp_streq(name, "with_index") && an <= 1) {
      /* `arr.map.with_index { }` is map's, answering the mapped array; only
         an Enumerator that just walks its source (each, cycle, a generator)
         is a plain walk with a counter */
      if (nt_kind(nt, recv) == NK_CallNode && nt_ref(nt, recv, "block") < 0) {
        const char *rn = nt_str(nt, recv, "name");
        if (!rn || (!sp_streq(rn, "each") && !sp_streq(rn, "cycle") && !sp_streq(rn, "new"))) continue;
      }
      hn = "__enumw_with_index";
    }
    if (!hn) continue;
    const char *cop = nt_str(nt, id, "call_operator");
    if (cop && sp_streq(cop, "&.")) continue;
    if (infer_type(c, recv) != TY_ENUMERATOR) continue;
    if (!block_body_breaks(nt, nt_ref(nt, blk, "body"))) continue;
    int *na = (int *)malloc(sizeof(int) * (size_t)(an + 1));
    if (!na) return changed;
    na[0] = recv; for (int j = 0; j < an; j++) na[j + 1] = av[j];
    int nargs = nt_new_node(nt, "ArgumentsNode");
    if (nargs < 0) { free(na); return changed; }
    nt_node_set_arr(nt, nargs, "arguments", na, an + 1);
    free(na);
    nt_node_set_ref(nt, id, "arguments", nargs);
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_str(nt, id, "name", hn);
    comp_grow_node_arrays(c);
    c->nscope[nargs] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_builtin_enum_calls(Compiler *c) {
  if (sp_builtin_enum_names_n == 0) return 0;
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  unsigned char *in_default = find_calls_in_param_defaults(nt, n0);
  /* `recv.m(args).each { blk }` is `recv.m(args) { blk }` (the Enumerator's
     each runs the method it came from, #4332), and that chain rule keys on
     the inner call's receiver: a blockless call that is the receiver of an
     `each { }` is left for it, and comes back here with the block. */
  unsigned char *chained = (unsigned char *)calloc((size_t)(n0 ? n0 : 1), 1);
  for (int id = 0; chained && id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") < 0) continue;
    const char *nm = nt_str(nt, id, "name");
    /* `recv.m.with_index { |x, i| }` is the Enumerator chain the typed
       emitters serve per name (as map.with_index is); rewritten, the inner
       call answers a plain Enumerator over the elements and the chain
       loses the method it came from */
    if (!nm || (!sp_streq(nm, "each") && !sp_streq(nm, "with_index") && !sp_streq(nm, "each_with_index"))) continue;
    int er = nt_ref(nt, id, "receiver");
    if (er >= 0 && er < n0 && nt_kind(nt, er) == NK_CallNode && nt_ref(nt, er, "block") < 0) chained[er] = 1;
  }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (builtin_enum_name_index(name) < 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (chained && chained[id]) continue;
    TyKind rt = infer_type(c, recv);
    int ok = 0;
    /* an Enumerator over a generator is driven lazily through #next by the
       typed emitter of these names, which is what lets a prefix be taken
       from an infinite one (or a search on one to terminate at all); the
       definition's `each` would materialize it first. A user class with no
       `each` of its own that never ends (an infinite `loop { yield }`) is
       routed through the same `__to_enum_each` synthesis (#3756) before this
       runs, so it reaches here as TY_ENUMERATOR too. */
    int lazy_driven = rt == TY_ENUMERATOR &&
                      (sp_streq(name, "take_while") || sp_streq(name, "find") || sp_streq(name, "detect"));
    /* Range overrides these in CRuby with an O(1) answer read off the
       endpoints, never calling each -- observable, not only faster: a Float
       range cannot iterate at all, and `(1.0..5.0).minmax` answers. Those
       keep their typed emitter on a Range receiver. */
    int range_own = (rt == TY_RANGE || rt == TY_FLOAT_RANGE || rt == TY_STR_RANGE) &&
                    (sp_streq(name, "min") || sp_streq(name, "max") || sp_streq(name, "minmax") ||
                     sp_streq(name, "sum") || sp_streq(name, "count") || sp_streq(name, "size") ||
                     sp_streq(name, "first") || sp_streq(name, "last") || sp_streq(name, "include?") ||
                     sp_streq(name, "member?"));
    if (range_own && nt_ref(nt, id, "block") < 0) continue;
    /* minmax's blockless form on an Array or a Hash keeps its dedicated
       C routine (sp_XArray_min/_max, called once each, no per-element
       nullable-int/GC-root bookkeeping): measured ~80% slower as a
       hand-written Ruby loop on a 1000-element Int array x 200000 rounds
       (0.15s -> 0.27s), well past the ~10% bound, while the block-
       comparator form (which has no such dedicated routine to lose, only
       ever a fused single-pass scan either way) measured at parity. Only a
       receiver with no such routine (an Enumerable includer with its own
       #each, or a value known only at run time) still needs the Ruby
       computation for its blockless form. */
    if (sp_streq(name, "minmax") && nt_ref(nt, id, "block") < 0 &&
        (ty_is_array(rt) || ty_is_hash(rt))) continue;
    /* `count` with neither a block nor an argument is a size query -- the
       Array/Hash/Range/Enumerator typed emitters answer it in O(1), and a
       plain Enumerable-includer with no `size` of its own still needs the
       O(n) walk CRuby's Enumerable#count itself does, which the definition
       below does not special-case; both stay on the existing emitter. */
    if (sp_streq(name, "count") && nt_ref(nt, id, "block") < 0) continue;
    /* cycle without a block answers an Enumerator (an endless one without a
       count) that the emitter builds; the definition covers the block form */
    if (sp_streq(name, "cycle") && nt_ref(nt, id, "block") < 0) continue;
    /* any?/all?/none?/one? without a block ask about each element's own
       truthiness (or, with one argument, a `===` pattern), never the
       block's; both stay on the existing emitter, the way a blockless,
       argumentless count does. */
    if ((sp_streq(name, "any?") || sp_streq(name, "all?") ||
         sp_streq(name, "none?") || sp_streq(name, "one?")) &&
        nt_ref(nt, id, "block") < 0) continue;
    /* find_index without a block is either the value-argument form
       (`find_index(v)`, its own arity/emitter arm) or the blockless
       Enumerator form (`find_index` alone); the generic def has no
       parameter for the value form and no non-inlined body for the
       Enumerator form (both stay unreached by design, like find/detect's
       `else: each`), so a blockless call here found no block to inline
       against and called an out-of-line clone that was never emitted
       (undefined reference at link time). Only the block form is a
       rewrite target. */
    if (sp_streq(name, "find_index") && nt_ref(nt, id, "block") < 0) continue;
    /* reduce/inject without a block is the symbol form (`reduce(:+)`), the
       seeded form (`reduce(seed)`, `reduce(seed, :+)`), or the bare argless
       call, which must raise CRuby's ArgumentError; the definition has no
       parameter for any of the three, so all of them stay on the existing
       arity-checked emitter, the way blockless count/find_index do. An
       OPERATOR symbol spelled `&:+` reaches here as a raw BlockArgumentNode
       wrapping a SymbolNode too, not a real block: spinel_parse.c's textual
       `&:sym` -> block lowering deliberately leaves operator symbols (empty
       name_len there) unconverted for "the arith reduce/inject lowering" --
       the fold emitter's own symbol-operator path, which this definition's
       plain `yield` cannot splice. Without this carve-out the call was
       claimed anyway and block_given? read false at the specialized clone
       (nothing there is an inlineable block), raising this definition's
       own ArgumentError for a call that plainly passed one
       (`[1, 2, 3].inject(&:+)`). */
    if (sp_streq(name, "reduce") || sp_streq(name, "inject")) {
      int blk9 = nt_ref(nt, id, "block");
      if (blk9 < 0) continue;
      if (nt_kind(nt, blk9) == NK_BlockArgumentNode) {
        int ex9 = nt_ref(nt, blk9, "expression");
        if (ex9 < 0 || nt_kind(nt, ex9) == NK_SymbolNode) continue;
      }
      /* `reduce(:sym)` / `inject(:sym)`: desugar_reduce_method_symbol already
         turned this into a literal 0-arg block calling `.sym` on each
         element, indistinguishable at this point from a program-written
         block -- marked there for exactly this carve-out. Stays on the fold
         emitter, which answers a symbol naming no real method with CRuby's
         NoMethodError; this definition's plain `yield` has no such fallback
         and failed the C build outright (found testing inject/reduce). */
      if ((int)nt_int(nt, id, "sym_fold", 0)) continue;
    }
    /* each_with_index without a block, on an Array/Hash/Range/Enumerator, is
       the existing typed emitter's Enumerator-of-pairs (a real receiver+size,
       #next-replayable, matches each_with_index_enumerator.rb and
       each_with_index_struct_present.rb exactly, including `#size`, which the
       definition's own generator block cannot answer). An OBJECT receiver has
       no such emitter arm at all (the __enum_to_a bridge is declined for this
       name, see is_array_enum_method), so it falls through to the definition's
       `Enumerator.new` else-arm instead. */
    if (sp_streq(name, "each_with_index") && nt_ref(nt, id, "block") < 0 &&
        !ty_is_object(rt)) continue;
    /* each_with_index on an Enumerator receiver (`arr.each.each_with_index
       { }`, `5.downto(3).each_with_index { }`) is CRuby's native
       Enumerator#each_with_index, not Enumerable#each_with_index: it answers
       the enumerator's UNDERLYING object (`[1,2,3].each.each_with_index{}`
       answers the array itself, not the enumerator, verified against CRuby),
       which this definition's plain `self` cannot reproduce (self here is
       the enumerator __enum_each_with_index__N was called with). Stays on
       the existing typed emitter, which already gets this right
       (enumerator_block_returns_self.rb, issue_3315_int_enum_with_index_block.rb). */
    if (sp_streq(name, "each_with_index") && rt == TY_ENUMERATOR) continue;
    /* ...and one the analysis already routed through a marked `to_a` hop
       (enum_each_wrap): codegen walks the Enumerator itself */
    if (nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "enum_each_wrap")) continue;
    /* find/detect reachable from an optional/keyword parameter's default
       value: see find_calls_in_param_defaults. */
    if (in_default && in_default[id] &&
        (sp_streq(name, "find") || sp_streq(name, "detect") ||
         sp_streq(name, "any?") || sp_streq(name, "all?") ||
         sp_streq(name, "none?") || sp_streq(name, "one?"))) continue;
    if (ty_is_array(rt) || ty_is_hash(rt) || rt == TY_RANGE || rt == TY_FLOAT_RANGE ||
        rt == TY_STR_RANGE || (rt == TY_ENUMERATOR && !lazy_driven)) ok = 1;
    /* an empty `[]` / `{}` receiver has no type until its use decides one,
       and this is that use */
    else if (rt == TY_UNKNOWN && (nt_kind(nt, recv) == NK_ArrayNode || nt_kind(nt, recv) == NK_HashNode)) ok = 1;
    else if (ty_is_object(rt)) {
      int ci = ty_object_class(rt);
      ok = an_class_includes_enumerable(c, ci) && comp_method_in_chain(c, ci, name, NULL) < 0;
    }
    else if (rt == TY_POLY) ok = 1;   /* a class of its own definition is dispatched below */
    if (!ok) continue;
    int copy = (int)nt_int(nt, id, "enum_copy", -1);
    if (copy < 0 || copy >= nt->count) continue;   /* no copy was made for this site */
    const char *gn = nt_str(nt, copy, "name");
    if (!gn || comp_method_index(c, gn) < 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    /* the builtin's own arity: `str.partition(sep)` on a value that is a
       String at run time is String's method, not Enumerable's */
    { int cpn = nt_ref(nt, copy, "parameters");
      int crn = 0; if (cpn >= 0) nt_arr(nt, cpn, "requireds", &crn);
      int con = 0; if (cpn >= 0) nt_arr(nt, cpn, "optionals", &con);
      if (an + 1 < crn || an + 1 > crn + con) continue; }
    int base = nt->count;
    int encl = c->nscope[id];
    /* A receiver known only at run time may be an instance of a class that
       defines the name itself, which keeps its own method: the call becomes
         (__r = recv; __r.is_a?(K) ? __r.m(args) { } : __enum_m(__r, args) { })
       over the classes that define it, the block copied for the second arm.
       Without such a class the call is rewritten in place. */
    int ndef = 0, defcls[64];
    if (rt == TY_POLY) {
      for (int k = 0; k < c->nclasses && ndef < 64; k++)
        if (!c->classes[k].is_native_class && comp_poly_arm_defines_n(c, k, name, an)) defcls[ndef++] = k;
    }
    /* `v&.m { }`: the receiver is bound once and a nil answers nil, the
       same dispatch shape with a nil test for the class test */
    const char *cop = nt_str(nt, id, "call_operator");
    int safe_nav = cop && sp_streq(cop, "&.");
    int blk = nt_ref(nt, id, "block");
    int recv_read = recv;
    int generic = id;
    if (ndef > 0 || safe_nav) {
      char rn[64]; snprintf(rn, sizeof rn, "__enumrecv_%d", id);
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      int own = nt_new_node(nt, "CallNode");
      int ownr = nt_new_node(nt, "LocalVariableReadNode");
      int genr = nt_new_node(nt, "LocalVariableReadNode");
      int gen = nt_new_node(nt, "CallNode");
      int ifn = nt_new_node(nt, "IfNode");
      int ts = nt_new_node(nt, "StatementsNode");
      int es = nt_new_node(nt, "StatementsNode");
      int eln = nt_new_node(nt, "ElseNode");
      int body = nt_new_node(nt, "StatementsNode");
      if (w < 0 || own < 0 || ownr < 0 || genr < 0 || gen < 0 || ifn < 0 || ts < 0 || es < 0 || eln < 0 || body < 0) { free(chained); free(in_default); return changed; }
      nt_node_set_str(nt, w, "name", rn); nt_node_set_int(nt, w, "depth", 0);
      nt_node_set_ref(nt, w, "value", recv);
      nt_node_set_str(nt, ownr, "name", rn); nt_node_set_int(nt, ownr, "depth", 0);
      nt_node_set_str(nt, genr, "name", rn); nt_node_set_int(nt, genr, "depth", 0);
      /* the class test, one is_a? per defining class, or-ed; a safe
         navigation tests nil instead (and dispatches its classes after) */
      int pred = -1;
      if (safe_nav) {
        int nr = nt_new_node(nt, "LocalVariableReadNode");
        int nq = nt_new_node(nt, "CallNode");
        if (nr < 0 || nq < 0) { free(chained); free(in_default); return changed; }
        nt_node_set_str(nt, nr, "name", rn); nt_node_set_int(nt, nr, "depth", 0);
        nt_node_set_str(nt, nq, "name", "nil?");
        nt_node_set_ref(nt, nq, "receiver", nr);
        pred = nq;
      }
      for (int k = 0; !safe_nav && k < ndef; k++) {
        int pr = nt_new_node(nt, "LocalVariableReadNode");
        int cr = nt_new_node(nt, "ConstantReadNode");
        int ia = nt_new_node(nt, "CallNode");
        int iaa = nt_new_node(nt, "ArgumentsNode");
        if (pr < 0 || cr < 0 || ia < 0 || iaa < 0) { free(chained); free(in_default); return changed; }
        nt_node_set_str(nt, pr, "name", rn); nt_node_set_int(nt, pr, "depth", 0);
        nt_node_set_str(nt, cr, "name", c->classes[defcls[k]].name);
        nt_node_set_arr(nt, iaa, "arguments", &cr, 1);
        nt_node_set_str(nt, ia, "name", "is_a?");
        nt_node_set_ref(nt, ia, "receiver", pr);
        nt_node_set_ref(nt, ia, "arguments", iaa);
        if (pred < 0) pred = ia;
        else {
          int orn = nt_new_node(nt, "OrNode");
          if (orn < 0) { free(chained); free(in_default); return changed; }
          nt_node_set_ref(nt, orn, "left", pred);
          nt_node_set_ref(nt, orn, "right", ia);
          pred = orn;
        }
      }
      /* the class's own method, on the same receiver, with the block; under
         a safe navigation the nil arm answers nil and the class arm, when
         there is one, sits inside it */
      if (safe_nav && ndef == 0) {
        nt_node_set_type(nt, own, "NilNode");
        nt_node_set_arr(nt, ts, "body", &own, 1);
      }
      else if (safe_nav) {
        int nil_n = nt_new_node(nt, "NilNode");
        int ifc = nt_new_node(nt, "IfNode");
        int cts = nt_new_node(nt, "StatementsNode");
        int ces = nt_new_node(nt, "StatementsNode");
        int celse = nt_new_node(nt, "ElseNode");
        if (nil_n < 0 || ifc < 0 || cts < 0 || ces < 0 || celse < 0) { free(chained); free(in_default); return changed; }
        /* pred so far is `__r.nil?`; the class test becomes the inner if */
        int cpred = -1;
        for (int k = 0; k < ndef; k++) {
          int pr2 = nt_new_node(nt, "LocalVariableReadNode");
          int cr2 = nt_new_node(nt, "ConstantReadNode");
          int ia2 = nt_new_node(nt, "CallNode");
          int iaa2 = nt_new_node(nt, "ArgumentsNode");
          if (pr2 < 0 || cr2 < 0 || ia2 < 0 || iaa2 < 0) { free(chained); free(in_default); return changed; }
          nt_node_set_str(nt, pr2, "name", rn); nt_node_set_int(nt, pr2, "depth", 0);
          nt_node_set_str(nt, cr2, "name", c->classes[defcls[k]].name);
          nt_node_set_arr(nt, iaa2, "arguments", &cr2, 1);
          nt_node_set_str(nt, ia2, "name", "is_a?");
          nt_node_set_ref(nt, ia2, "receiver", pr2);
          nt_node_set_ref(nt, ia2, "arguments", iaa2);
          if (cpred < 0) cpred = ia2;
          else { int o2 = nt_new_node(nt, "OrNode"); if (o2 < 0) { free(chained); free(in_default); return changed; }
                 nt_node_set_ref(nt, o2, "left", cpred); nt_node_set_ref(nt, o2, "right", ia2); cpred = o2; }
        }
        nt_node_set_str(nt, own, "name", name);
        nt_node_set_ref(nt, own, "receiver", ownr);
        nt_node_set_int(nt, own, "enum_own", 1);   /* the class's method: a user arm */
        if (args >= 0) nt_node_set_ref(nt, own, "arguments", args);
        if (blk >= 0) nt_node_set_ref(nt, own, "block", blk);
        nt_node_set_arr(nt, cts, "body", &own, 1);
        nt_node_set_arr(nt, ces, "body", &gen, 1);
        nt_node_set_ref(nt, celse, "statements", ces);
        nt_node_set_ref(nt, ifc, "predicate", cpred);
        nt_node_set_ref(nt, ifc, "statements", cts);
        nt_node_set_ref(nt, ifc, "subsequent", celse);
        /* outer: nil? ? nil : (class ? own : generic) */
        nt_node_set_arr(nt, ts, "body", &nil_n, 1);
        nt_node_set_arr(nt, es, "body", &ifc, 1);
      }
      else {
        nt_node_set_str(nt, own, "name", name);
        nt_node_set_ref(nt, own, "receiver", ownr);
        nt_node_set_int(nt, own, "enum_own", 1);   /* the class's method: a user arm */
        if (args >= 0) nt_node_set_ref(nt, own, "arguments", args);
        if (blk >= 0) nt_node_set_ref(nt, own, "block", blk);
        nt_node_set_arr(nt, ts, "body", &own, 1);
      }
      /* the builtin's copy, with a copy of the block */
      if (!(safe_nav && ndef > 0)) nt_node_set_arr(nt, es, "body", &gen, 1);
      nt_node_set_ref(nt, eln, "statements", es);
      nt_node_set_ref(nt, ifn, "predicate", pred);
      nt_node_set_ref(nt, ifn, "statements", ts);
      nt_node_set_ref(nt, ifn, "subsequent", eln);
      int stmts[2] = { w, ifn };
      nt_node_set_arr(nt, body, "body", stmts, 2);
      nt_node_set_type(nt, id, "ParenthesesNode");
      nt_node_set_ref(nt, id, "body", body);
      if (safe_nav) nt_node_set_str(nt, id, "call_operator", ".");
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_ref(nt, id, "arguments", -1);
      nt_node_set_ref(nt, id, "block", -1);
      if (blk >= 0) { int bc = nt_clone_subtree(nt, blk); if (bc >= 0) nt_node_set_ref(nt, gen, "block", bc); }
      recv_read = genr;
      generic = gen;
      Scope *es2 = comp_scope_of(c, id);
      if (es2) scope_local_intern(es2, rn);
    }
    int *na = (int *)malloc(sizeof(int) * (size_t)(an + 1));
    if (!na) { free(chained); free(in_default); return changed; }
    na[0] = recv_read; for (int j = 0; j < an; j++) na[j + 1] = av[j];
    /* a fresh arguments node: the old one may be shared with a call the
       Enumerator each rule rewrote onto it (an orphan keeps a reference) */
    int nargs = nt_new_node(nt, "ArgumentsNode");
    if (nargs < 0) { free(na); free(chained); free(in_default); return changed; }
    nt_node_set_arr(nt, nargs, "arguments", na, an + 1);
    nt_node_set_ref(nt, generic, "arguments", nargs);
    free(na);
    nt_node_set_ref(nt, generic, "receiver", -1);
    { char gnb[256]; snprintf(gnb, sizeof gnb, "%s", gn); nt_node_set_str(nt, generic, "name", gnb); }
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  free(chained); free(in_default);
  return changed;
}

/* ---- builtins/: Integer, Float, Comparable (builtins/integer.rb etc,
   spliced by sp_splice_builtin_extras, spinel_parse.c). Unlike
   Enumerable, these methods take no block and never answer an
   Enumerator, so the generic def is a plain method: no block_given?
   split, no lazy-Enumerator carve-outs, no `each`-chain rule. The
   receiver rule is correspondingly simpler than desugar_builtin_enum_calls
   above: a call is rewritten only for a CONCRETE Integer/Bignum (the
   Integer container), a concrete Float (the Float container), or a
   receiver Comparable's methods can already answer without going through
   an open-ended is_a? split (the Comparable container, added with its
   own methods). A run-time-typed (poly) receiver is deliberately left on
   the existing runtime dispatch (sp_poly_int_*, sp_poly_float_* and
   friends in lib/spinel_rt.h): those already switch on the boxed tag
   correctly for every name this mechanism's first callers migrate
   (verified against CRuby per method, in each method's own probe and
   commit), so reproducing Enumerable's is_a? split here -- built for an
   open-ended set of user classes, which Integer/Float/Comparable are not
   -- would only add AST-rewrite surface for a receiver shape whose
   answer does not change. A program's own reopen (`class Integer; def
   digits`) wins the same way an Enumerable includer's own method does:
   checked per call site against the container's real class index
   (comp_class_index), not by skipping the splice outright the way
   enumerable.rb's `module Enumerable` guard does. */
enum { SP_BX_INTEGER = 0, SP_BX_FLOAT = 1, SP_BX_COMPARABLE = 2, SP_BX_N = 3 };
static const char *const sp_bx_class_name[SP_BX_N] = { "Integer", "Float", "Comparable" };
static const char *const sp_bx_prefix[SP_BX_N]     = { "__int_", "__flt_", "__cmp_" };

extern int sp_builtin_extra_names_n(int idx);
extern const char *sp_builtin_extra_name(int idx, int i);
extern int sp_builtin_extra_name_index(int idx, const char *name);

int desugar_builtin_scalar_defs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int root = nt->root_id;
  int top = root >= 0 ? nt_ref(nt, root, "statements") : -1;
  if (top < 0) return 0;
  int any_names = 0;
  for (int bx = 0; bx < SP_BX_N; bx++) if (sp_builtin_extra_names_n(bx) > 0) any_names = 1;
  if (!any_names) return 0;
  int tn = 0; const int *tb = nt_arr(nt, top, "body", &tn);
  if (!tb || tn == 0) return 0;
  int *nb = (int *)malloc(sizeof(int) * (size_t)(tn + 64));
  if (!nb) return 0;
  int nbn = 0, cap = tn + 64;
  int *gdef[SP_BX_N];
  for (int bx = 0; bx < SP_BX_N; bx++) {
    int n = sp_builtin_extra_names_n(bx);
    gdef[bx] = n > 0 ? (int *)malloc(sizeof(int) * (size_t)n) : NULL;
    for (int i = 0; i < n; i++) gdef[bx][i] = -1;
  }
  int n0 = nt->count;
  int changed = 0;
  /* A program's own reopen of the container -- for ANY name, not only one
     builtins/integer.rb migrated -- is registered by name the same way the
     spliced generic is, in odef_name/odef_def below, so a concrete call
     can rewrite onto a clone typed for that one call site (below, in the
     per-call-site loop) instead of the single native-int
     `sp_Integer_abs(sp_int self)` the class's ordinary compilation emits,
     which cannot accept a Bignum receiver at all. That ordinary
     compilation is left untouched -- the class stays in `nb` -- because
     the poly dispatch's prim-reopen arm (class_is_prim_reopen,
     codegen_call.c) still calls it for a run-time-typed receiver, and a
     later def of the same name overwrites the table entry (blanking the
     one it replaces), so the LAST reopen -- the program's own, when both a
     spliced generic and a program's own def claim a name -- wins, matching
     comp_method_in_chain's ordinary Ruby redefinition semantics. */
  char **odef_name[SP_BX_N]; int *odef_def[SP_BX_N]; int odef_n[SP_BX_N], odef_cap[SP_BX_N];
  for (int bx = 0; bx < SP_BX_N; bx++) { odef_name[bx] = NULL; odef_def[bx] = NULL; odef_n[bx] = 0; odef_cap[bx] = 0; }
  /* Splicing prepends the required file's content ahead of the program's
     own source (resolve_plain_requires), so the FIRST top-level
     ClassNode/ModuleNode for a given container is always the spliced
     generic one, if there is one at all. A program that reopens the same
     container itself (`class Integer; def digits; ...different...; end;
     end`, likely to override just that one name) is textually
     indistinguishable from "all-builtin-named defs" by shape alone --
     digits.rb probing found this the hard way, an own reopen consisting
     of exactly one builtin-named method converted along with the real
     one and shadowed EVERY call in the file, not just those after it.
     Consuming only the first occurrence per container as the spliced
     generic and running every later one through the clone-registration
     below (rather than the in-place transform reserved for the spliced
     occurrence) keeps that distinction. */
  int bx_done[SP_BX_N] = { 0, 0, 0 };
  for (int i = 0; i < tn; i++) {
    int st = tb[i];
    NodeKind sk = nt_kind(nt, st);
    if (sk != NK_ClassNode && sk != NK_ModuleNode) { nb[nbn++] = st; continue; }
    int cp = nt_ref(nt, st, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, st, "name");
    int bx = -1;
    for (int k = 0; k < SP_BX_N; k++) if (mn && sp_streq(mn, sp_bx_class_name[k])) { bx = k; break; }
    if (bx < 0 || sp_builtin_extra_names_n(bx) == 0) { nb[nbn++] = st; continue; }
    int body = nt_ref(nt, st, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    int all_builtin = 0;
    if (!bx_done[bx] && bn > 0) {
      all_builtin = 1;
      for (int k = 0; k < bn; k++)
        if (nt_kind(nt, bb[k]) != NK_DefNode || sp_builtin_extra_name_index(bx, nt_str(nt, bb[k], "name")) < 0) { all_builtin = 0; break; }
    }
    if (all_builtin) {
      bx_done[bx] = 1;
      for (int k = 0; k < bn; k++) {
        int def = bb[k];
        const char *name = nt_str(nt, def, "name");
        int bi = sp_builtin_extra_name_index(bx, name);
        int hi = bi_subtree_max(nt, def);
        int pn = nt_ref(nt, def, "parameters");
        if (pn < 0) { pn = nt_new_node(nt, "ParametersNode"); if (pn < 0) break; nt_node_set_ref(nt, def, "parameters", pn); }
        int spself = nt_new_node(nt, "RequiredParameterNode"); if (spself < 0) break;
        nt_node_set_str(nt, spself, "name", "__self");
        { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
          int *nr = (int *)malloc(sizeof(int) * (size_t)(rn + 1));
          if (!nr) break;
          nr[0] = spself; for (int j = 0; j < rn; j++) nr[j + 1] = reqs[j];
          nt_node_set_arr(nt, pn, "requireds", nr, rn + 1); free(nr); }
        int dbody = nt_ref(nt, def, "body");
        int lo = dbody >= 0 ? dbody : def;
        bi_self_to_local(nt, lo, hi);
        { char gn[256]; snprintf(gn, sizeof gn, "%s%s", sp_bx_prefix[bx], name); nt_node_set_str(nt, def, "name", gn); }
        if (bi >= 0) gdef[bx][bi] = def;
      }
      bi_subtree_blank(nt, cp);
      nt_node_set_type(nt, st, "NilNode");
      changed = 1;
      continue;
    }
    /* A program's own reopen: left fully intact (still in `nb`) for
       ordinary class compilation and the poly dispatch's prim-reopen arm.
       Each of its own simple-signature defs (no block, splat, or keyword
       params -- the same restricted shape desugar_builtin_scalar_calls'
       arity check already assumes) additionally gets a clone-based
       generic registered by name, built from a CLONE of the def so the
       original is never touched. */
    nb[nbn++] = st;
    for (int k = 0; k < bn; k++) {
      int def = bb[k];
      if (nt_kind(nt, def) != NK_DefNode) continue;
      const char *name = nt_str(nt, def, "name");
      if (!name) continue;
      int pn0 = nt_ref(nt, def, "parameters");
      int shape_ok = 1;
      if (pn0 >= 0 && (nt_ref(nt, pn0, "block") >= 0 || nt_ref(nt, pn0, "rest") >= 0 ||
                       nt_ref(nt, pn0, "keyword_rest") >= 0)) shape_ok = 0;
      if (shape_ok && pn0 >= 0) { int kwn = 0; nt_arr(nt, pn0, "keywords", &kwn); if (kwn > 0) shape_ok = 0; }
      if (!shape_ok) {
        /* A reopen this registration cannot clone (a splat, a block or
           keyword parameters) still REPLACES the name for the program.
           Skipping it silently was only safe while the name was the
           program's alone; when it is also one a builtins/ file defines,
           the spliced generic stays in the table and every concrete call
           site rewrites onto THAT, so the reopen was ignored outright:
           `class Integer; def gcd(*a) = "mine"; end; 12.gcd(8)` answered
           4. Stand the generic down instead -- with no table entry no
           call site rewrites, and the calls take the ordinary
           open-class path, which answers the reopen correctly (the same
           answer SPINEL_NO_BUILTINS=1 gives). */
        int bi0 = sp_builtin_extra_name_index(bx, name);
        if (bi0 >= 0 && gdef[bx] && gdef[bx][bi0] >= 0) {
          bi_subtree_blank(nt, gdef[bx][bi0]);
          gdef[bx][bi0] = -1;
        }
        continue;
      }
      int clone = nt_clone_subtree(nt, def);
      if (clone < 0) continue;
      int hi = bi_subtree_max(nt, clone);
      int pn = nt_ref(nt, clone, "parameters");
      if (pn < 0) { pn = nt_new_node(nt, "ParametersNode"); if (pn < 0) continue; nt_node_set_ref(nt, clone, "parameters", pn); }
      int spself = nt_new_node(nt, "RequiredParameterNode"); if (spself < 0) continue;
      nt_node_set_str(nt, spself, "name", "__self");
      { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
        int *nr = (int *)malloc(sizeof(int) * (size_t)(rn + 1));
        if (!nr) continue;
        nr[0] = spself; for (int j = 0; j < rn; j++) nr[j + 1] = reqs[j];
        nt_node_set_arr(nt, pn, "requireds", nr, rn + 1); free(nr); }
      int dbody = nt_ref(nt, clone, "body");
      int lo = dbody >= 0 ? dbody : clone;
      bi_self_to_local(nt, lo, hi);
      { char gn[256]; snprintf(gn, sizeof gn, "%s%s", sp_bx_prefix[bx], name); nt_node_set_str(nt, clone, "name", gn); }
      int bi = sp_builtin_extra_name_index(bx, name);
      if (bi >= 0) {
        if (gdef[bx][bi] >= 0) bi_subtree_blank(nt, gdef[bx][bi]);
        gdef[bx][bi] = clone;
      } else {
        int j = -1;
        for (int m = 0; m < odef_n[bx]; m++) if (sp_streq(odef_name[bx][m], name)) { j = m; break; }
        if (j >= 0) {
          if (odef_def[bx][j] >= 0) bi_subtree_blank(nt, odef_def[bx][j]);
          odef_def[bx][j] = clone;
        } else {
          if (odef_n[bx] >= odef_cap[bx]) {
            int newcap = odef_cap[bx] > 0 ? odef_cap[bx] * 2 : 8;
            char **ng = (char **)realloc(odef_name[bx], sizeof(char *) * (size_t)newcap);
            int *nd = (int *)realloc(odef_def[bx], sizeof(int) * (size_t)newcap);
            if (ng) odef_name[bx] = ng;
            if (nd) odef_def[bx] = nd;
            if (ng && nd) odef_cap[bx] = newcap;
          }
          if (odef_n[bx] < odef_cap[bx]) {
            odef_name[bx][odef_n[bx]] = strdup(name);
            odef_def[bx][odef_n[bx]] = clone;
            odef_n[bx]++;
          }
        }
      }
      changed = 1;
    }
  }
  if (!changed) {
    for (int bx = 0; bx < SP_BX_N; bx++) {
      free(gdef[bx]);
      for (int i = 0; i < odef_n[bx]; i++) free(odef_name[bx][i]);
      free(odef_name[bx]); free(odef_def[bx]);
    }
    free(nb); return 0;
  }
  /* one copy per call site, exactly as desugar_builtins does for
     enumerable.rb (a shared definition would carry the union of every
     call site's argument types onto every site) */
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *cn0 = nt_str(nt, id, "name");
    int bx = -1, gd = -1;
    for (int k = 0; k < SP_BX_N && gd < 0; k++) {
      int idx = sp_builtin_extra_name_index(k, cn0);
      if (idx >= 0 && gdef[k] && gdef[k][idx] >= 0) { bx = k; gd = gdef[k][idx]; break; }
      if (!cn0) continue;
      for (int m = 0; m < odef_n[k]; m++)
        if (sp_streq(odef_name[k][m], cn0) && odef_def[k][m] >= 0) { bx = k; gd = odef_def[k][m]; break; }
    }
    if (gd < 0) continue;
    int copy = nt_clone_subtree(nt, gd);
    if (copy < 0) break;
    char cn[256]; snprintf(cn, sizeof cn, "%s%s__%d", sp_bx_prefix[bx], cn0, id);
    nt_node_set_str(nt, copy, "name", cn);
    nt_node_set_int(nt, id, "bx_copy", copy);
    nt_node_set_int(nt, id, "bx_container", bx);
    if (nbn >= cap) { cap *= 2; int *g = (int *)realloc(nb, sizeof(int) * (size_t)cap); if (!g) break; nb = g; }
    nb[nbn++] = copy;
  }
  nt_node_set_arr(nt, top, "body", nb, nbn);
  for (int bx = 0; bx < SP_BX_N; bx++) {
    int n = sp_builtin_extra_names_n(bx);
    for (int i = 0; i < n; i++) if (gdef[bx] && gdef[bx][i] >= 0) bi_subtree_blank(nt, gdef[bx][i]);
    free(gdef[bx]);
    for (int i = 0; i < odef_n[bx]; i++) {
      if (odef_def[bx][i] >= 0) bi_subtree_blank(nt, odef_def[bx][i]);
      free(odef_name[bx][i]);
    }
    free(odef_name[bx]); free(odef_def[bx]);
  }
  comp_grow_node_arrays(c);
  free(nb);
  return 1;
}

/* `recv.m(args)` (no block, ever, for these three containers) on a receiver
   the container's method serves: rewritten into the per-call-site copy,
   `<prefix>m__N(recv, args)`, once the receiver's type has settled. Runs
   in the fixpoint alongside desugar_builtin_enum_calls. */
int desugar_builtin_scalar_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int copy = (int)nt_int(nt, id, "bx_copy", -1);
    if (copy < 0 || copy >= nt->count) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;   /* already rewritten in an earlier round */
    int bx = (int)nt_int(nt, id, "bx_container", -1);
    const char *name = nt_str(nt, id, "name");
    TyKind rt = infer_type(c, recv);
    /* Only a CONCRETE receiver is rewritten (Integer: TY_INT/TY_BIGINT,
       Float: TY_FLOAT, Comparable: as below). A run-time-typed (poly)
       receiver is deliberately left on the existing dispatch (the
       sp_poly_int_ and sp_poly_float_ runtime helpers in lib/spinel_rt.h,
       or the face table fallback in codegen_call_recv.c/codegen_call.c
       that a program-wide name collision with an unrelated class routes
       a poly value through): an is_a?-split rewrite for a poly receiver
       was tried and measured (a 1,000,000-call loop) at 1.7 to 3.5 times
       the cost of that existing dispatch, past the ~10% bound this
       migration is held to -- the split's own overhead (a write, a
       runtime is_a? check, and a box/unbox round trip for the argument)
       is comparable to or larger than a method like digits' own O(digit
       count) work, unlike Enumerable's poly split where that overhead is
       negligible next to a whole iteration. The concrete-type C emitter
       arms this migration removes for the COMMON case stay present in a
       narrower form specifically for that face-table fallback to call:
       see the comment where they are re-added. */
    int ok = 0;
    if (bx == SP_BX_INTEGER) ok = (rt == TY_INT || rt == TY_BIGINT);
    else if (bx == SP_BX_FLOAT) ok = (rt == TY_FLOAT);
    else if (bx == SP_BX_COMPARABLE) {
      /* Integer/Bignum/Float only, narrower than builtins/comparable.rb's
         own surface suggests. Two SEPARATE pre-existing gaps rule the
         other two receiver shapes out, both found writing that file and
         both reproducing with no Comparable migration involved at all:

         A String receiver: `self <=> min` inside the generic method,
         with `min` some other concrete non-String type at a given call
         site's clone (an object with no `<=>`, say), reaches the
         compiler's generic "no dispatch arm for this receiver/argument
         pair" fallback and hard-compiles an unconditional NoMethodError
         -- where the same `"str" <=> obj` written directly, outside any
         generic/cloned method body, correctly compiles a run-time nil
         check (test/numeric_coerce_protocol.rb's `"abc".between?(money,
         "b")`, for a `money` with no `<=>`, answered "undefined method
         '<=>' for an instance of String" instead of CRuby's "comparison
         of String with Money failed").

         A user class with its own `<=>`: writing `lo <=> hi` with the
         operands concretely that class is a genuinely NEW kind of call
         site for the class's own `<=>` -- every existing route to it
         (the `<`/`>`/`between?` operators, `sort`/`min`/`max`, the
         object-clamp emitter) calls it through the boxed runtime hook
         (sp_obj_cmp_hook), never as a plain statically typed Ruby
         expression. That one concretely-typed call site settles the
         method's OWN parameter type to the class, and a program that
         also uses the same `<=>` with a different argument type
         elsewhere (any `x.clamp(range)`, whose emitter calls `<=>` with
         an Integer endpoint through that hook) then miscompiles: the
         parameter stays typed as the class while a real Integer flows
         into it, read back through a pointer that was never one. A bare
         `a <=> b` beside an unrelated `x.clamp(1..5)` already breaks the
         same way on a compiler with no builtins/comparable.rb at all.

         Both are general method-typing gaps (a parameter's type has to
         account for every REACHABLE caller, hook-based ones included),
         not something one migration should paper over. */
      ok = (rt == TY_INT || rt == TY_BIGINT || rt == TY_FLOAT);
    }
    if (!ok) continue;
    /* Comparable's names are the one CROSS-container case: they are
       reopened on Integer/Float (`class Integer; def clamp`), never on
       `module Comparable`, so the registration below -- which keys a
       reopen to the container whose CLASS NAME the reopen spells -- files
       such a def under Integer, where the name is not a builtins name at
       all, and the Comparable generic stays live for every call site.
       The reopen was then ignored outright. Ask the receiver's own
       concrete class instead, and leave the call on the ordinary
       open-class path when it answers. */
    if (bx == SP_BX_COMPARABLE) {
      const char *concrete = rt == TY_FLOAT ? "Float" : "Integer";
      int cci = comp_class_index(c, concrete);
      if (cci >= 0 && comp_method_in_chain(c, cci, name, NULL) >= 0) continue;
    }
    /* Which def `copy` clones -- the spliced generic, or a program's own
       reopen -- was already decided in desugar_builtin_scalar_defs' name
       table (the program's own reopen overwrites the spliced generic's
       entry there), so bx_copy alone says which one this call rewrites
       onto; no separate "does the program's own class chain define this
       name" check is needed here. */
    const char *gn = nt_str(nt, copy, "name");
    if (!gn || comp_method_index(c, gn) < 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    { int cpn = nt_ref(nt, copy, "parameters");
      int crn = 0; if (cpn >= 0) nt_arr(nt, cpn, "requireds", &crn);
      int con = 0; if (cpn >= 0) nt_arr(nt, cpn, "optionals", &con);
      if (an + 1 < crn || an + 1 > crn + con) continue; }
    int encl = c->nscope[id];
    int base = nt->count;
    int *na = (int *)malloc(sizeof(int) * (size_t)(an + 1));
    if (!na) continue;
    na[0] = recv; for (int j = 0; j < an; j++) na[j + 1] = av[j];
    int nargs = nt_new_node(nt, "ArgumentsNode");
    if (nargs < 0) { free(na); continue; }
    nt_node_set_arr(nt, nargs, "arguments", na, an + 1);
    free(na);
    nt_node_set_ref(nt, id, "arguments", nargs);
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_str(nt, id, "name", gn);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* ---- a parameter default that calls back into its own method ----
   A default is filled at the call site: the omitted argument's expression is
   emitted in place of the argument. A default that calls its own method with
   that argument omitted again (`def m(x, y = (x > 0 ? m(x - 1)[0] : 0))`), or
   calls another method whose default comes back around, has no finite
   inlining, and codegen recursed until the compiler's stack ran out.

   Ruby evaluates the default in the callee, once per call. The same happens
   when the default becomes a method of its own, defined where the original
   is and on the same receiver, taking the earlier parameters it reads:

     def m(x, y = D)        ->  def __sp_default_m_y_N(x) = D
                                def m(x, y = __sp_default_m_y_N(x))

   The call site then inlines only the call to the helper, and the recursion
   happens at run time inside the helper's body, as it does in CRuby. Only
   defaults on such a cycle are rewritten. */
typedef struct {
  int def, param, val, bad;
  const char *cls;   /* the enclosing class or module's name, NULL at top level */
  int singleton;     /* `def self.m`, or a def inside `class << self` */
} RdDefault;

/* `alias` / `alias_method` pairs, new name then old, collected once per run */
static const char **rd_alias = NULL;
static int rd_nalias = 0;

static void rd_collect_aliases(const NodeTable *nt) {
  rd_nalias = 0;
  int cap = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *nn = NULL, *on = NULL;
    if (fwd_node_is(nt, id, "AliasMethodNode")) {
      nn = nt_str(nt, nt_ref(nt, id, "new_name"), "value");
      on = nt_str(nt, nt_ref(nt, id, "old_name"), "value");
    } else if (fwd_node_is(nt, id, "CallNode") && nt_str(nt, id, "name") &&
               sp_streq(nt_str(nt, id, "name"), "alias_method")) {
      int an = 0; const int *av = nt_arr(nt, nt_ref(nt, id, "arguments"), "arguments", &an);
      if (an == 2) { nn = nt_str(nt, av[0], "value"); on = nt_str(nt, av[1], "value"); }
    }
    if (!nn || !on) continue;
    if (2 * rd_nalias + 2 > cap) {
      cap = cap ? cap * 2 : 16;
      const char **g = realloc(rd_alias, sizeof *rd_alias * (size_t)cap);
      if (!g) return;
      rd_alias = g;
    }
    rd_alias[2 * rd_nalias] = nn;
    rd_alias[2 * rd_nalias + 1] = on;
    rd_nalias++;
  }
}

static int rd_call_name_is(const char *nm, const char *want) {
  if (!nm || !want) return 0;
  for (int hop = 0; nm && hop < 8; hop++) {
    if (sp_streq(nm, want)) return 1;
    const char *old = NULL;
    for (int k = 0; k < rd_nalias && !old; k++)
      if (sp_streq(rd_alias[2 * k], nm)) old = rd_alias[2 * k + 1];
    nm = old;
  }
  return 0;
}

/* Does `X.new` run the initialize of class `cls`: X is cls, or a subclass
   that inherits cls's initialize without defining its own? Classes are
   matched by their last name segment, before any scope exists. */
static int rd_new_reaches(const NodeTable *nt, const char *x, const char *cls, int depth) {
  if (!x || !cls || depth > 16) return 0;
  if (sp_streq(x, cls)) return 1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "ClassNode")) continue;
    const char *cn = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
    if (!cn || !sp_streq(cn, x)) continue;
    int bn = 0; const int *bv = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
    for (int k = 0; k < bn; k++)
      if (fwd_node_is(nt, bv[k], "DefNode") && nt_ref(nt, bv[k], "receiver") < 0 &&
          nt_str(nt, bv[k], "name") && sp_streq(nt_str(nt, bv[k], "name"), "initialize"))
        return 0;
    const char *sup = nt_str(nt, nt_ref(nt, id, "superclass"), "name");
    if (sup && rd_new_reaches(nt, sup, cls, depth + 1)) return 1;
  }
  return 0;
}

/* Can `call`, in the default `from`, reach the method default `want`
   belongs to? Only calls that name their target without a value to type
   count: receiverless or on self by name, `Const.new` for the initialize
   Const runs, `Const.m` for a singleton method of the class Const, and a
   bare `new` in a singleton method for its own class's initialize. Another
   receiver's method of the same name (`@cpu.update` in APU#update's
   default) is not this one, and neither is `Array.new` for a user class's
   initialize: rewriting those widened types, or made a helper whose call
   could not be emitted. A cycle through a call not followed here is
   refused at emit time instead. */
static int rd_call_reaches(const NodeTable *nt, int call, const RdDefault *from,
                           const RdDefault *want) {
  const char *nm = nt_str(nt, call, "name");
  const char *wn = nt_str(nt, want->def, "name");
  if (!nm || !wn) return 0;
  int init = sp_streq(wn, "initialize") && !want->singleton;
  int r = nt_ref(nt, call, "receiver");
  if (r < 0 || fwd_node_is(nt, r, "SelfNode")) {
    if (sp_streq(nm, "new"))
      return init && from->singleton && from->cls && want->cls && sp_streq(from->cls, want->cls);
    return rd_call_name_is(nm, wn);
  }
  if (!fwd_node_is(nt, r, "ConstantReadNode") && !fwd_node_is(nt, r, "ConstantPathNode")) return 0;
  const char *cn = nt_str(nt, r, "name");
  if (sp_streq(nm, "new")) return init && rd_new_reaches(nt, cn, want->cls, 0);
  return want->singleton && cn && want->cls && sp_streq(cn, want->cls) && rd_call_name_is(nm, wn);
}

/* Does the subtree at `id` call the method default `want` belongs to? `*bad`
   is set when it holds something that would mean another thing inside a
   method of its own: the caller's block, `super`, the method's name. */
static int rd_subtree_calls(const NodeTable *nt, int id, const RdDefault *from,
                            const RdDefault *want, int *bad) {
  if (id < 0 || id >= nt->count) return 0;
  const char *ty = nt_type(nt, id);
  int hit = 0;
  if (ty) {
    if (sp_streq(ty, "YieldNode") || sp_streq(ty, "SuperNode") ||
        sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "DefNode") ||
        sp_streq(ty, "ForwardingArgumentsNode")) *bad = 1;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && (sp_streq(nm, "block_given?") || sp_streq(nm, "__method__") ||
                 sp_streq(nm, "binding"))) *bad = 1;
      if (want && rd_call_reaches(nt, id, from, want)) hit = 1;
    }
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) hit |= rd_subtree_calls(nt, nd->r[j].ref, from, want, bad);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++)
      hit |= rd_subtree_calls(nt, nd->a[j].ids[k], from, want, bad);
  return hit;
}

static int rd_subtree_reads(const NodeTable *nt, int id, const char *name) {
  if (id < 0 || id >= nt->count) return 0;
  if (fwd_node_is(nt, id, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, name)) return 1;
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) if (rd_subtree_reads(nt, nd->r[j].ref, name)) return 1;
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) if (rd_subtree_reads(nt, nd->a[j].ids[k], name)) return 1;
  return 0;
}

/* The parameter nodes of `def`, in the order Ruby binds them. */
static int rd_params(const NodeTable *nt, int def, int *out, int cap) {
  int pn = nt_ref(nt, def, "parameters");
  if (pn < 0) return 0;
  int n = 0;
  static const char *const arrs[] = { "requireds", "optionals" };
  for (int a = 0; a < 2; a++) {
    int k = 0; const int *ids = nt_arr(nt, pn, arrs[a], &k);
    for (int i = 0; i < k && n < cap; i++) out[n++] = ids[i];
  }
  int r = nt_ref(nt, pn, "rest");
  if (r >= 0 && n < cap) out[n++] = r;
  { int k = 0; const int *ids = nt_arr(nt, pn, "posts", &k);
    for (int i = 0; i < k && n < cap; i++) out[n++] = ids[i]; }
  { int k = 0; const int *ids = nt_arr(nt, pn, "keywords", &k);
    for (int i = 0; i < k && n < cap; i++) out[n++] = ids[i]; }
  int kr = nt_ref(nt, pn, "keyword_rest");
  if (kr >= 0 && n < cap) out[n++] = kr;
  int b = nt_ref(nt, pn, "block");
  if (b >= 0 && n < cap) out[n++] = b;
  return n;
}

static void rd_mark_parents(const NodeTable *nt, int *parent, int n0) {
  for (int id = 0; id < n0; id++) {
    const SpNode *nd = &nt->nodes[id];
    for (int j = 0; j < nd->nr; j++) {
      int ch = nd->r[j].ref;
      if (ch >= 0 && ch < n0) parent[ch] = id;
    }
    for (int j = 0; j < nd->na; j++)
      for (int k = 0; k < nd->a[j].n; k++) {
        int ch = nd->a[j].ids[k];
        if (ch >= 0 && ch < n0) parent[ch] = id;
      }
  }
}

int desugar_recursive_param_defaults(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int nd = 0, cap = 0;
  RdDefault *ds = NULL;
  for (int def = 0; def < n0; def++) {
    if (!fwd_node_is(nt, def, "DefNode") || !nt_str(nt, def, "name")) continue;
    int ps[256]; int np = rd_params(nt, def, ps, 256);
    for (int i = 0; i < np; i++) {
      if (!fwd_node_is(nt, ps[i], "OptionalParameterNode") &&
          !fwd_node_is(nt, ps[i], "OptionalKeywordParameterNode")) continue;
      int v = nt_ref(nt, ps[i], "value");
      if (v < 0) continue;
      if (nd >= cap) {
        cap = cap ? cap * 2 : 16;
        RdDefault *g = realloc(ds, sizeof *ds * (size_t)cap);
        if (!g) { free(ds); return 0; }
        ds = g;
      }
      ds[nd].def = def; ds[nd].param = ps[i]; ds[nd].val = v; ds[nd].bad = 0;
      rd_subtree_calls(nt, v, NULL, NULL, &ds[nd].bad);
      nd++;
    }
  }
  if (nd == 0) { free(ds); return 0; }
  int *parent = malloc(sizeof(int) * (size_t)n0);
  if (!parent) { free(ds); return 0; }
  for (int k = 0; k < n0; k++) parent[k] = -1;
  rd_mark_parents(nt, parent, n0);
  for (int i = 0; i < nd; i++) {
    ds[i].cls = NULL;
    ds[i].singleton = fwd_node_is(nt, nt_ref(nt, ds[i].def, "receiver"), "SelfNode");
    for (int p = parent[ds[i].def]; p >= 0; p = parent[p]) {
      if (fwd_node_is(nt, p, "SingletonClassNode")) ds[i].singleton = 1;
      if (fwd_node_is(nt, p, "ClassNode") || fwd_node_is(nt, p, "ModuleNode")) {
        ds[i].cls = nt_str(nt, nt_ref(nt, p, "constant_path"), "name");
        break;
      }
    }
  }
  rd_collect_aliases(nt);
  /* edge i -> j: default i calls the method default j belongs to */
  unsigned char *edge = calloc((size_t)nd * (size_t)nd, 1);
  int *stack = malloc(sizeof(int) * (size_t)nd);
  unsigned char *seen = malloc((size_t)nd);
  if (!edge || !stack || !seen) {
    free(edge); free(stack); free(seen); free(ds); free(parent);
    return 0;
  }
  for (int i = 0; i < nd; i++)
    for (int j = 0; j < nd; j++) {
      int bad = 0;
      edge[(size_t)i * nd + j] =
        (unsigned char)rd_subtree_calls(nt, ds[i].val, &ds[i], &ds[j], &bad);
    }
  int changed = 0;
  for (int i = 0; i < nd; i++) {
    /* is default i reachable from itself? */
    memset(seen, 0, (size_t)nd);
    int sp = 0, cyc = 0;
    for (int j = 0; j < nd; j++)
      if (edge[(size_t)i * nd + j] && !seen[j]) { seen[j] = 1; stack[sp++] = j; }
    while (sp > 0 && !cyc) {
      int k = stack[--sp];
      if (k == i) { cyc = 1; break; }
      for (int j = 0; j < nd; j++)
        if (edge[(size_t)k * nd + j] && !seen[j]) { seen[j] = 1; stack[sp++] = j; }
    }
    if (!cyc || ds[i].bad) continue;
    int def = ds[i].def;
    const char *pname = nt_str(nt, ds[i].param, "name");
    if (!pname) continue;
    /* the earlier parameters the default reads become the helper's */
    int ps[256]; int np = rd_params(nt, def, ps, 256);
    const char *args[256]; int na = 0, later_read = 0, before = 1;
    for (int k = 0; k < np; k++) {
      if (ps[k] == ds[i].param) { before = 0; continue; }
      const char *an = nt_str(nt, ps[k], "name");
      if (!an || !rd_subtree_reads(nt, ds[i].val, an)) continue;
      if (before) args[na++] = an; else later_read = 1;
    }
    if (later_read) continue;
    int stmt = def, stmts = parent[def];
    while (stmts >= 0 && !fwd_node_is(nt, stmts, "StatementsNode")) { stmt = stmts; stmts = parent[stmts]; }
    if (stmts < 0) continue;

    char hname[256];
    { const char *dn = nt_str(nt, def, "name");
      int o = snprintf(hname, sizeof hname, "__sp_default_");
      for (const char *q = dn; *q && o < 120; q++)
        hname[o++] = ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                      (*q >= '0' && *q <= '9') || *q == '_') ? *q : '_';
      /* a name the program does not define itself, so no user method is
         shadowed or taken for the helper */
      for (int n = i; ; n++) {
        snprintf(hname + o, sizeof hname - (size_t)o, "_%s_%d", pname, n);
        int taken = 0;
        for (int id = 0; id < nt->count && !taken; id++)
          taken = fwd_node_is(nt, id, "DefNode") && nt_str(nt, id, "name") &&
                  sp_streq(nt_str(nt, id, "name"), hname);
        if (!taken) break;
      } }

    /* built in pre-order, so the helper's subtree is one id range */
    int hd = fwd_new_node_like(nt, def, "DefNode");
    if (hd < 0) break;
    nt_node_set_str(nt, hd, "name", hname);
    nt_node_set_int(nt, hd, "default_helper", 1);
    int hp = fwd_new_node_like(nt, def, "ParametersNode");
    int hreq[256];
    for (int k = 0; k < na; k++) {
      hreq[k] = fwd_new_node_like(nt, def, "RequiredParameterNode");
      nt_node_set_str(nt, hreq[k], "name", args[k]);
    }
    nt_node_set_arr(nt, hp, "requireds", hreq, na);
    nt_node_set_arr(nt, hp, "optionals", NULL, 0);
    nt_node_set_arr(nt, hp, "posts", NULL, 0);
    nt_node_set_arr(nt, hp, "keywords", NULL, 0);
    nt_node_set_ref(nt, hp, "rest", -1);
    nt_node_set_ref(nt, hp, "keyword_rest", -1);
    nt_node_set_ref(nt, hp, "block", -1);
    int orecv = nt_ref(nt, def, "receiver");
    int hrecv = orecv >= 0 ? nt_clone_subtree(nt, orecv) : -1;
    int hs = fwd_new_node_like(nt, ds[i].val, "StatementsNode");
    int body = nt_clone_subtree(nt, ds[i].val);
    nt_node_set_arr(nt, hs, "body", &body, 1);
    nt_node_set_ref(nt, hd, "parameters", hp);
    nt_node_set_ref(nt, hd, "body", hs);
    nt_node_set_ref(nt, hd, "receiver", hrecv);

    /* the default's own node becomes the call to the helper */
    int v = ds[i].val;
    long long vl = nt_int(nt, v, "node_line", 0), vf = nt_int(nt, v, "node_file", 0),
              vc = nt_int(nt, v, "node_col", 0);
    bi_subtree_blank(nt, v);
    nt_node_reset(nt, v, "CallNode");
    nt_node_set_int(nt, v, "node_line", vl);
    nt_node_set_int(nt, v, "node_file", vf);
    nt_node_set_int(nt, v, "node_col", vc);
    nt_node_set_str(nt, v, "name", hname);
    nt_node_set_ref(nt, v, "receiver", -1);
    nt_node_set_ref(nt, v, "block", -1);
    nt_node_set_str(nt, v, "call_operator", ".");
    if (na > 0) {
      int an = fwd_new_node_like(nt, v, "ArgumentsNode");
      int av[256];
      for (int k = 0; k < na; k++) {
        av[k] = fwd_new_node_like(nt, v, "LocalVariableReadNode");
        nt_node_set_str(nt, av[k], "name", args[k]);
      }
      nt_node_set_arr(nt, an, "arguments", av, na);
      nt_node_set_ref(nt, v, "arguments", an);
    } else nt_node_set_ref(nt, v, "arguments", -1);

    /* the helper is defined just ahead of the method */
    int bn = 0; const int *bv = nt_arr(nt, stmts, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1)); int nn = 0;
    if (!nb) break;
    for (int k = 0; k < bn; k++) {
      if (bv[k] == stmt) nb[nn++] = hd;
      nb[nn++] = bv[k];
    }
    nt_node_set_arr(nt, stmts, "body", nb, nn);
    free(nb);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  free(parent); free(stack); free(seen); free(edge); free(ds);
  free(rd_alias); rd_alias = NULL; rd_nalias = 0;
  return changed;
}
