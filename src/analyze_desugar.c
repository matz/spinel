/* analyze_desugar.c -- the AST rewrites analyze_program drives, split out of
   analyze_pass.c. Pure code movement, no logic change: the functions keep
   their order, and analyze_program's call sequence is untouched.

   Five desugars stay in analyze_pass.c because they use file-static helpers
   that inference also uses (subtree_has_kind, subtree_rename_local, the
   bdp_/ie_ pairs); moving those would widen their linkage for no reason but
   this file split. */
#include "analyze_internal.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

/* Required-param count of a forwarded callable expression `ex`, or -1 if it
   cannot be determined statically. Chooses the hash-pair calling convention: a
   1-param callable receives the [k,v] pair as one array, a 2-param one is called
   positionally (matching CRuby's proc auto-splat of the yielded pair). With
   `shape`, also whether the callable is a lambda or a Method, which take the
   pair as strictly as a method does, and its Proc#arity (the required count,
   or its complement when an optional or a rest follows). */
typedef struct { int strict; int arity; } FwdShape;
static int params_arity(const NodeTable *nt, int pn) {
  int rn = 0, on = 0, qn = 0;
  nt_arr(nt, pn, "requireds", &rn);
  nt_arr(nt, pn, "optionals", &on);
  nt_arr(nt, pn, "posts", &qn);
  return (on > 0 || nt_ref(nt, pn, "rest") >= 0) ? -(rn + qn) - 1 : rn + qn;
}
static int fwd_callable_arity(Compiler *c, int ex, FwdShape *shape) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *exty = nt_type(nt, ex);
  if (!exty) return -1;
  /* `method(:m)`: a Method is as strict as the def it names */
  if (shape && sp_streq(exty, "CallNode") && nt_str(nt, ex, "name") &&
      sp_streq(nt_str(nt, ex, "name"), "method")) {
    int mi = method_obj_target_mi(c, ex);
    int dn = mi >= 0 ? c->scopes[mi].def_node : -1;
    int pn = dn >= 0 ? nt_ref(nt, dn, "parameters") : -1;
    if (dn < 0) return -1;
    shape->strict = 1;
    shape->arity = pn >= 0 ? params_arity(nt, pn) : 0;
    int rn = 0; if (pn >= 0) nt_arr(nt, pn, "requireds", &rn);
    return rn;
  }
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
  if (shape) {
    const char *cty = nt_type(nt, create);
    const char *cn = nt_str(nt, create, "name");
    shape->strict = sp_streq(cty, "LambdaNode") || (cn && sp_streq(cn, "lambda"));
    shape->arity = params_arity(nt, pn);
  }
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

  /* Index every local write by (variable name, scope) once, so
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
    if (!comp_is_local_write(nt_kind(nt, w))) continue;
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
      if (!local_write_binds_value(nt_kind(nt, w))) { bail = 1; break; }
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
    nt_node_set_int(nt, id, "self_call", 1);
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
int desugar_bare_class_self_calls(Compiler *c) {
  static const struct { const char *name; int argc; } surf[] = {
    { "const_get", -1 }, { "superclass", 0 }, { "ancestors", 0 },
    { "include?", 1 }, { "to_s", 0 }, { "inspect", 0 }, { "frozen?", 0 },
  };
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int want = -2;
    for (size_t k = 0; k < sizeof surf / sizeof surf[0]; k++)
      if (sp_streq(nm, surf[k].name)) { want = surf[k].argc; break; }
    if (want == -2) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    if (id >= c->node_cap) continue;
    Scope *sc = comp_scope_of(c, id);
    int in_cmethod = sc && sc->name && sc->is_cmethod && sc->class_id >= 0;
    int in_body = (!sc || !sc->name) && c->node_cbody[id] >= 0;
    if (!in_cmethod && !in_body) continue;
    if (want >= 0) {
      int argc = 0, an = nt_ref(nt, id, "arguments");
      if (an >= 0) nt_arr(nt, an, "arguments", &argc);
      if (!in_cmethod || argc != want || nt_ref(nt, id, "block") >= 0) continue;
      if (comp_cmethod_in_chain(c, sc->class_id, nm, NULL) >= 0 || comp_method_index(c, nm) >= 0) continue;
    }
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

/* A method the program adds to Object (`class Object; def helper ...`) is
   every object's, so a receiverless `helper` in another class's instance
   method is `self.helper` (#5779). A class's chain stops short of Object, so
   the bare call found nothing there: a plain one raised NameError at run
   time and a yielding one was refused, where `self.helper` already reaches
   Object's method. Give it self, at the top level too, where self is main.
   A module's method runs on its includer and an instance_eval block on its
   receiver; both are left as they were. */
int desugar_bare_object_reopen_calls(Compiler *c) {
  int obj = comp_class_index(c, "Object");
  if (obj < 0) return 0;
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (nt_ref(nt, id, "receiver") >= 0 || id >= c->node_cap) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || comp_method_in_class(c, obj, nm) < 0) continue;
    /* in a class body self is the class, and in an instance_eval block
       the receiver */
    Scope *sc = comp_scope_of(c, id);
    if (((!sc || !sc->name) && c->node_cbody[id] >= 0) || ie_class_of(c, id) >= 0) continue;
    int cls = sc ? sc->class_id : -1;
    if (sc && sc->is_cmethod) continue;
    if (cls >= 0) {
      int dn = c->classes[cls].def_node;
      if (cls == obj || (dn >= 0 && dn < c->nt->count && nt_kind(c->nt, dn) == NK_ModuleNode)) continue;
      if (comp_method_in_chain(c, cls, nm, NULL) >= 0) continue;
    }
    /* at the top level self is main, an Object; a top-level `def` of the
       name is the one the bare call means */
    else if (comp_method_index(c, nm) >= 0) continue;
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

int desugar_descendant_reader_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (nt_ref(nt, id, "receiver") >= 0 || nt_ref(nt, id, "arguments") >= 0 ||
        nt_ref(nt, id, "block") >= 0 || id >= c->node_cap || ie_class_of(c, id) >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    Scope *sc = comp_scope_of(c, id);
    if (!nm || !sc || !sc->name || sc->is_cmethod || sc->class_id < 0) continue;
    if (comp_method_in_chain(c, sc->class_id, nm, NULL) >= 0 || comp_reader_in_chain(c, sc->class_id, nm, NULL)) continue;
    int nd = 0, hit = 0;
    const int *ds = comp_descendants(c, sc->class_id, &nd);
    for (int i = 0; i < nd && !hit; i++) hit = comp_reader_in_chain(c, ds[i], nm, NULL);
    if (!hit) continue;
    int sn = nt_new_node(nt, "SelfNode");
    if (sn < 0) continue;
    nt_node_set_int(nt, id, "vcall", 0);
    comp_grow_node_arrays(c);
    c->nscope[sn] = c->nscope[id];
    c->node_cbody[sn] = c->node_cbody[id];
    nt_node_set_ref(nt, id, "receiver", sn);
    changed = 1;
  }
  return changed;
}

/* `h[k], o.x = v, w` stores through `[]=` and `x=` just as `h[k] = v` and
   `o.x = w` do, but the passes that widen a container's key and element
   types, or an attribute's slot, from those stores only look at CallNodes:
   an index or attribute target of a multiple assignment was no evidence at
   all, so `h[cnt[0]], h[:k] = 1, "x"` built a symbol-keyed hash that dropped
   the integer key, and `a[0], a[2] = 1, "x"` stored a string into an int
   array. Each such target gets a detached `recv[k] = v` / `recv.x = v`
   CallNode, never in a statement list and so never emitted, whose value is
   the element the target receives: the tuple's element, nil past its end, or
   `rhs[i]` for a run-time array. The multiple assignment itself still does
   the store. A run-time right side waits until its type is known. */
static int masgn_ev_value(NodeTable *nt, int value, int tuple, int en, const int *els,
                          int scalar, long long pos) {
  if (tuple) {
    if (pos >= 0 && pos < en) return els[pos];
    return nt_new_node(nt, "NilNode");
  }
  if (scalar) return pos == 0 ? value : nt_new_node(nt, "NilNode");
  int ix = nt_new_node(nt, "IntegerNode");
  int ia = nt_new_node(nt, "ArgumentsNode");
  int rd = nt_new_node(nt, "CallNode");
  if (ix < 0 || ia < 0 || rd < 0) return -1;
  nt_node_set_int(nt, ix, "value", pos);
  nt_node_set_arr(nt, ia, "arguments", &ix, 1);
  nt_node_set_ref(nt, rd, "receiver", value);
  nt_node_set_str(nt, rd, "name", "[]");
  nt_node_set_ref(nt, rd, "arguments", ia);
  nt_node_set_ref(nt, rd, "block", -1);
  nt_node_set_int(nt, rd, "masgn_elem", pos);
  return rd;
}
static int masgn_ev_store(Compiler *c, int id, int tgt, int val) {
  NodeTable *nt = (NodeTable *)c->nt;
  NodeKind k = nt_kind(nt, tgt);
  if ((k != NK_IndexTargetNode && k != NK_CallTargetNode) || val < 0) return 0;
  int recv = nt_ref(nt, tgt, "receiver");
  if (recv < 0) return 0;
  int an = 0;
  int anode = k == NK_IndexTargetNode ? nt_ref(nt, tgt, "arguments") : -1;
  const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
  if (k == NK_IndexTargetNode && an < 1) return 0;
  const char *nm = k == NK_IndexTargetNode ? "[]=" : nt_str(nt, tgt, "name");
  if (!nm) return 0;
  int nargs = nt_new_node(nt, "ArgumentsNode");
  int call = nt_new_node(nt, "CallNode");
  int *na = malloc(sizeof(int) * (size_t)(an + 1));
  if (nargs < 0 || call < 0 || !na) { free(na); return 0; }
  for (int j = 0; j < an; j++) na[j] = av[j];
  na[an] = val;
  nt_node_set_arr(nt, nargs, "arguments", na, an + 1);
  free(na);
  nt_node_set_ref(nt, call, "receiver", recv);
  nt_node_set_str(nt, call, "name", nm);
  nt_node_set_ref(nt, call, "arguments", nargs);
  nt_node_set_ref(nt, call, "block", -1);
  int line = (int)nt_int(nt, id, "node_line", 0);
  if (line) nt_node_set_int(nt, call, "node_line", line);
  return 1;
}
int desugar_masgn_store_evidence(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_MultiWriteNode || id >= c->node_cap) continue;
    if (nt_int(nt, id, "masgn_ev", 0)) continue;
    int ln = 0; const int *ls = nt_arr(nt, id, "lefts", &ln);
    int rn = 0; const int *rs = nt_arr(nt, id, "rights", &rn);
    int want = 0;
    for (int j = 0; j < ln; j++)
      want |= nt_kind(nt, ls[j]) == NK_IndexTargetNode || nt_kind(nt, ls[j]) == NK_CallTargetNode;
    for (int j = 0; j < rn; j++)
      want |= nt_kind(nt, rs[j]) == NK_IndexTargetNode || nt_kind(nt, rs[j]) == NK_CallTargetNode;
    int rest = nt_ref(nt, id, "rest");
    int rtgt = rest >= 0 && nt_kind(nt, rest) == NK_SplatNode ? nt_ref(nt, rest, "expression") : -1;
    if (rtgt >= 0 && nt_kind(nt, rtgt) != NK_IndexTargetNode && nt_kind(nt, rtgt) != NK_CallTargetNode) rtgt = -1;
    want |= rtgt >= 0;
    if (!want) continue;
    int value = nt_ref(nt, id, "value");
    if (value < 0) continue;
    int tuple = masgn_tuple_rhs(nt, value), en = 0, scalar = 0;
    if (!tuple) {
      TyKind st = infer_type(c, value);
      if (st == TY_UNKNOWN) continue;
      if (ty_is_object(st)) { nt_node_set_int(nt, id, "masgn_ev", 1); continue; }
      scalar = st != TY_POLY && !ty_is_array(st);
    }
    nt_node_set_int(nt, id, "masgn_ev", 1);
    int n_before = nt->count;
    /* the arrays are reread: every new node may move the table */
    for (int j = 0; j < ln; j++) {
      ls = nt_arr(nt, id, "lefts", &ln);
      const int *els = tuple ? nt_arr(nt, value, "elements", &en) : NULL;
      int tgt = ls[j];
      NodeKind tk = nt_kind(nt, tgt);
      if (tk != NK_IndexTargetNode && tk != NK_CallTargetNode) continue;
      changed |= masgn_ev_store(c, id, tgt, masgn_ev_value(nt, value, tuple, en, els, scalar, j));
    }
    for (int j = 0; j < rn; j++) {
      rs = nt_arr(nt, id, "rights", &rn);
      const int *els = tuple ? nt_arr(nt, value, "elements", &en) : NULL;
      int tgt = rs[j];
      NodeKind tk = nt_kind(nt, tgt);
      if (tk != NK_IndexTargetNode && tk != NK_CallTargetNode) continue;
      /* a right target takes the element counted from the end, never one a
         left target already took */
      long long pos = tuple ? (en - rn + j < ln ? -1 : en - rn + j)
                    : scalar ? (j == 0 && ln == 0 ? 0 : -1) : j - rn;
      changed |= masgn_ev_store(c, id, tgt, masgn_ev_value(nt, value, tuple, en, els, scalar, pos));
    }
    /* a splat target takes the array of what the fixed targets leave: the
       tuple's middle, the scalar alone, or a run-time array's slice, typed
       as the array itself */
    if (rtgt >= 0) {
      int rv = -1;
      if (tuple || scalar) {
        if (tuple) nt_arr(nt, value, "elements", &en);
        int from = tuple ? ln : 0;
        int to = tuple ? en - rn : ln == 0 && rn == 0 ? 1 : 0;
        if (to > from) {
          int *ra = malloc(sizeof(int) * (size_t)(to - from));
          const int *els = tuple ? nt_arr(nt, value, "elements", &en) : NULL;
          for (int k = from; ra && k < to; k++) ra[k - from] = tuple ? els[k] : value;
          rv = ra ? nt_new_node(nt, "ArrayNode") : -1;
          if (rv >= 0) nt_node_set_arr(nt, rv, "elements", ra, to - from);
          free(ra);
        }
      }
      else rv = value;
      changed |= masgn_ev_store(c, id, rtgt, rv);
    }
    comp_grow_node_arrays(c);
    for (int k = n_before; k < nt->count; k++) {
      c->nscope[k] = c->nscope[id];
      c->node_cbody[k] = c->node_cbody[id];
    }
  }
  return changed;
}


/* ---- a def's &block parameter, reassigned ------------------------------
   `block = proc { ... }` inside `def dispatch(..., &block)`, or the
   `blk ||= proc { }` default: CRuby rebinds the local while `yield` and
   `block_given?` keep seeing the block the caller passed. Spinel refused the
   write where it was written (a yielding method is inlined at each call
   site, where its block is not a value at all -- the parameter has nowhere to
   take a write) and asked for the rewrite against a fresh local. This is
   that rewrite, done by the compiler: the parameter's VALUE moves to a fresh
   local `__bpv_<name>` assigned at the top of the body, every read and write
   of the name in the body uses the local, and yield / block_given? stay on
   the caller's block, which is exactly CRuby's split. A nested def is its own
   scope; a block declaring its own `<name>` shadows the parameter inside it.
   activesupport's BroadcastLogger#dispatch is the shape, and `require
   "active_support"` itself runs through it. */
static int bpw_kind_is_lvar(NodeKind k) {
  return k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode ||
         k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
         k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableTargetNode;
}
static int bpw_kind_is_write(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
         k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode ||
         k == NK_LocalVariableTargetNode;
}
/* rename == NULL: count the writes; else rename every read and write */
static int bpw_walk(NodeTable *nt, int node, const char *bp, const char *rename) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode) return 0;
  if ((k == NK_BlockNode || k == NK_LambdaNode) && blk_locals_have(nt_str(nt, node, "locals"), bp)) return 0;
  int hits = 0;
  if (bpw_kind_is_lvar(k)) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && sp_streq(nm, bp)) {
      if (rename) nt_node_set_str(nt, node, "name", rename);
      if (bpw_kind_is_write(k)) hits++;
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) hits += bpw_walk(nt, nt_ref_at(nt, node, i), bp, rename);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) hits += bpw_walk(nt, ids[j], bp, rename);
  }
  return hits;
}
int desugar_blk_param_writes(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int bpn = nt_ref(nt, pn, "block");
    if (bpn < 0 || nt_kind(nt, bpn) != NK_BlockParameterNode) continue;
    const char *bp = nt_str(nt, bpn, "name");
    if (!bp || !bp[0]) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    if (bpw_walk(nt, body, bp, NULL) == 0) continue;
    char nn[300]; snprintf(nn, sizeof nn, "__bpv_%s", bp);
    bpw_walk(nt, body, bp, nn);
    int rd = nt_new_node(nt, "LocalVariableReadNode"); if (rd < 0) continue;
    nt_node_set_str(nt, rd, "name", bp);
    int wr = nt_new_node(nt, "LocalVariableWriteNode"); if (wr < 0) continue;
    nt_node_set_str(nt, wr, "name", nn);
    nt_node_set_ref(nt, wr, "value", rd);
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    int *nb = (int *)malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) continue;
    nb[0] = wr; for (int j = 0; j < bn; j++) nb[j + 1] = bb[j];
    nt_node_set_arr(nt, body, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  return changed;
}

/* A `*rest` parameter the body assigns (`args = args.first`, the usual
   background-job `perform(*args)`) gave the parameter the union of the
   packed Array and what the body stores, so the method took an sp_RbVal
   while every call site still passed the packed sp_PolyArray *, and the C
   did not build (#6227). As a &blk parameter's writes do above, the body's
   reads and writes move to a fresh local copied from the parameter first,
   and the parameter stays the Array the callers pack. */
int desugar_rest_param_writes(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int rpn = nt_ref(nt, pn, "rest");
    if (rpn < 0 || nt_kind(nt, rpn) != NK_RestParameterNode) continue;
    const char *rp = nt_str(nt, rpn, "name");
    if (!rp || !rp[0]) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    if (bpw_walk(nt, body, rp, NULL) == 0) continue;
    char nn[300]; snprintf(nn, sizeof nn, "__rpv_%s", rp);
    /* The usual shape writes it once, as a statement of the body itself
       (`args = args.first`), with no write before it: the statements before
       read the parameter, the write's own value reads it too, and from the
       write on the name is the copy. Renaming in place adds no statement:
       a prepended copy is a node numbered after the whole body, and the
       write-type fold, which visits writes in node order, then took the
       copy's write after the reads it feeds and alternated between two
       answers until the round cap (#6491). */
    { int bn0 = 0; const int *bb0 = nt_arr(nt, body, "body", &bn0);
      int k = -1;
      for (int j = 0; j < bn0 && k < 0; j++) {
        if (nt_kind(nt, bb0[j]) == NK_LocalVariableWriteNode && nt_str(nt, bb0[j], "name") &&
            sp_streq(nt_str(nt, bb0[j], "name"), rp) &&
            bpw_walk(nt, nt_ref(nt, bb0[j], "value"), rp, NULL) == 0)
          k = j;
        else if (bpw_walk(nt, bb0[j], rp, NULL) > 0) break;   /* a write elsewhere first */
      }
      if (k >= 0) {
        nt_node_set_str(nt, bb0[k], "name", nn);
        for (int j = k + 1; j < bn0; j++) bpw_walk(nt, bb0[j], rp, nn);
        changed = 1;
        continue;
      } }
    bpw_walk(nt, body, rp, nn);
    int rd = nt_new_node(nt, "LocalVariableReadNode"); if (rd < 0) continue;
    nt_node_set_str(nt, rd, "name", rp);
    int wr = nt_new_node(nt, "LocalVariableWriteNode"); if (wr < 0) continue;
    nt_node_set_str(nt, wr, "name", nn);
    nt_node_set_ref(nt, wr, "value", rd);
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    int *nb = (int *)malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) continue;
    nb[0] = wr; for (int j = 0; j < bn; j++) nb[j + 1] = bb[j];
    /* The copy goes ahead of the body in node order too: numbered after it,
       the write-type fold (ascending ids) took the body's `args = args.first`
       read of the copy before the copy's own write re-typed it each round,
       and alternated between two answers until the round cap (#6491). The
       copy takes the first statement's number, and that statement the
       copy's. */
    if (bn > 0 && bb[0] < wr) {
      int first = bb[0];
      nt_swap_nodes(nt, first, wr);
      nb[0] = first; nb[1] = wr;
    }
    nt_node_set_arr(nt, body, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  return changed;
}

/* ---- `yield` inside a proc / lambda literal ----------------------------
   A proc literal is a real closure -- its own C function -- and a `yield` in
   it has no inlined caller's block to reach, so it raised LocalJumpError at
   run time however the method was called. The block IS reachable as a
   value: the method's &block parameter, captured by the closure like any
   local (`b.call(x)` inside the proc always worked). So a yield in a
   closure becomes `<bp>.call(args)` on the def's block parameter, and a def
   with no named one gets `&__blk`. A block attached to any other call
   (`each { yield }`) is inlined with its method and keeps its yield. */
static int yic_is_closure_call(const NodeTable *nt, int node) {
  if (nt_kind(nt, node) != NK_CallNode || nt_ref(nt, node, "block") < 0) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0) return sp_streq(nm, "proc") || sp_streq(nm, "lambda");
  const char *rty = nt_type(nt, recv);
  return sp_streq(nm, "new") && rty && sp_streq(rty, "ConstantReadNode") &&
         nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Proc");
}
/* bp == NULL: count the yields inside closures; else rewrite them */
static int yic_walk(NodeTable *nt, int node, int in_closure, const char *bp) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode) return 0;
  int hits = 0;
  if (k == NK_YieldNode && in_closure) {
    hits++;
    if (bp) {
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      if (rd >= 0) {
        nt_node_set_str(nt, rd, "name", bp);
        nt_node_set_type(nt, node, "CallNode");
        nt_node_set_ref(nt, node, "receiver", rd);
        nt_node_set_str(nt, node, "name", "call");
        nt_node_set_ref(nt, node, "block", -1);
        if (nt_ref(nt, node, "arguments") < 0) nt_node_set_ref(nt, node, "arguments", -1);
      }
    }
    return hits;
  }
  int enter = in_closure || k == NK_LambdaNode;
  if (yic_is_closure_call(nt, node)) {
    /* the closure's body is inside; its arguments and receiver are not */
    int nr = nt_num_refs(nt, node);
    for (int i = 0; i < nr; i++) {
      int ch = nt_ref_at(nt, node, i);
      hits += yic_walk(nt, ch, ch == nt_ref(nt, node, "block") ? 1 : in_closure, bp);
    }
    return hits;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) hits += yic_walk(nt, nt_ref_at(nt, node, i), enter, bp);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) hits += yic_walk(nt, ids[j], enter, bp);
  }
  return hits;
}
int desugar_yield_in_closure(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0) continue;
    if (yic_walk(nt, body, 0, NULL) == 0) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) {
      pn = nt_new_node(nt, "ParametersNode"); if (pn < 0) continue;
      nt_node_set_ref(nt, id, "parameters", pn);
    }
    int bpn = nt_ref(nt, pn, "block");
    const char *bp = (bpn >= 0 && nt_kind(nt, bpn) == NK_BlockParameterNode) ? nt_str(nt, bpn, "name") : NULL;
    if (!bp || !bp[0]) {
      if (bpn < 0) { bpn = nt_new_node(nt, "BlockParameterNode"); if (bpn < 0) continue; nt_node_set_ref(nt, pn, "block", bpn); }
      nt_node_set_str(nt, bpn, "name", "__blk");
      bp = "__blk";
    }
    char bpc[300]; snprintf(bpc, sizeof bpc, "%s", bp);
    yic_walk(nt, body, 0, bpc);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  return changed;
}

/* ---- `X.module_eval do ... end` on the body's own class ---------------------
   As a statement of a class or module body, a class_eval / module_eval /
   class_exec / module_exec block on that body's own class reopens it in
   place: the block's aliases and defs are the body's. X is `self`, the
   class's own constant, or a local holding one of those -- cgi/escape.rb:
     target = defined?(CGI::EscapeExt) && CGI::EscapeExt.method_defined?(:escapeHTML) ? CGI::EscapeExt : self
     target.module_eval do alias escape_html escapeHTML ... end
   where the condition is decided from the program's text: EscapeExt is a
   module the program defines (so `defined?` holds) whose bodies define no
   escapeHTML (so method_defined? does not), and the local is `self`. A
   receiver this cannot resolve is left alone (refused downstream as before).
   Runs before the scope pass, so everything here is syntactic. */
static void xc_push(int **arr, int *n, int v);
static void xc_push(int **arr, int *n, int v) {
  int *g = (int *)realloc(*arr, sizeof(int) * (size_t)(*n + 1));
  if (!g) return;
  *arr = g; (*arr)[(*n)++] = v;
}
static int subtree_has(const NodeTable *nt, int root, int id) {
  if (root == id) return 1;
  if (root < 0 || root >= nt->count) return 0;
  for (int j = 0; j < nt_num_refs(nt, root); j++) if (subtree_has(nt, nt_ref_at(nt, root, j), id)) return 1;
  for (int j = 0; j < nt_num_arrs(nt, root); j++) {
    int an = 0; const int *ids = nt_arr_at(nt, root, j, &an);
    for (int k = 0; k < an; k++) if (subtree_has(nt, ids[k], id)) return 1;
  }
  return 0;
}
static int me_leaf_defined(const NodeTable *nt, const char *leaf) {
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (nm && sp_streq(nm, leaf)) return 1;
  }
  return 0;
}
static int me_stmt_defines(const NodeTable *nt, int s, const char *m) {
  NodeKind sk = nt_kind(nt, s);
  if (sk == NK_DefNode) return nt_ref(nt, s, "receiver") < 0 && nt_str(nt, s, "name") && sp_streq(nt_str(nt, s, "name"), m);
  int nn = sk == NK_AliasMethodNode ? nt_ref(nt, s, "new_name") : -1;
  if (nn >= 0) return nt_kind(nt, nn) == NK_SymbolNode && nt_str(nt, nn, "value") && sp_streq(nt_str(nt, nn, "value"), m);
  const char *cn = sk == NK_CallNode ? nt_str(nt, s, "name") : NULL;
  int w = cn && (sp_streq(cn, "attr_writer") || sp_streq(cn, "attr_accessor")), r = cn && !sp_streq(cn, "attr_writer");
  if (!cn || (!w && !sp_streq(cn, "attr") && !sp_streq(cn, "attr_reader") && !sp_streq(cn, "define_method") && !sp_streq(cn, "alias_method"))) return 0;
  int an = 0; const int *av = nt_arr(nt, nt_ref(nt, s, "arguments"), "arguments", &an);
  for (int a = 0; a < an; a++) {
    NodeKind ak = nt_kind(nt, av[a]);
    const char *v = ak == NK_SymbolNode ? nt_str(nt, av[a], "value") : ak == NK_StringNode ? nt_str(nt, av[a], "content") : NULL;
    size_t vl = v ? strlen(v) : 0;
    if (v && ((r && sp_streq(v, m)) || (w && !strncmp(v, m, vl) && sp_streq(m + vl, "=")))) return 1;
  }
  return 0;
}
/* does any body of the class/module named `leaf` define instance method `m`? */
static int me_body_defines(const NodeTable *nt, const char *leaf, const char *m, int skip) {
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!nm || !sp_streq(nm, leaf) || subtree_has(nt, skip, id)) continue;
    int body = nt_ref(nt, id, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int i = 0; i < bn; i++) {
      int rv = nt_ref(nt, bb[i], "receiver");
      if ((rv < 0 || nt_kind(nt, rv) == NK_SelfNode) && me_stmt_defines(nt, bb[i], m)) return 1;
    }
  }
  return 0;
}
static const char *me_const_leaf(const NodeTable *nt, int n) {
  if (n < 0) return NULL;
  NodeKind k = nt_kind(nt, n);
  return k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(nt, n, "name") : NULL;
}
/* every segment of a constant path names a class or module the program defines */
static int me_path_defined(const NodeTable *nt, int n) {
  for (int seg = n; seg >= 0; ) {
    const char *leaf = me_const_leaf(nt, seg);
    if (!leaf || !me_leaf_defined(nt, leaf)) return 0;
    if (nt_kind(nt, seg) != NK_ConstantPathNode) return 1;
    seg = nt_ref(nt, seg, "parent");
    if (seg < 0) return 1;
  }
  return 0;
}
/* 1 / 0 when the predicate is decided by the program's text, else -1 */
static int me_static_pred(const NodeTable *nt, int pred) {
  if (pred < 0) return -1;
  NodeKind k = nt_kind(nt, pred);
  if (k == NK_TrueNode) return 1;
  if (k == NK_FalseNode || k == NK_NilNode) return 0;
  if (k == NK_ParenthesesNode) {
    int body = nt_ref(nt, pred, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn == 1 ? me_static_pred(nt, bb[0]) : -1;
  }
  if (k == NK_DefinedNode) {
    int v = nt_ref(nt, pred, "value");
    if (v < 0 || !me_const_leaf(nt, v)) return -1;
    return me_path_defined(nt, v) ? 1 : 0;
  }
  if (k == NK_AndNode) {
    int l = me_static_pred(nt, nt_ref(nt, pred, "left"));
    if (l == 0) return 0;
    if (l == 1) return me_static_pred(nt, nt_ref(nt, pred, "right"));
    return -1;
  }
  if (k == NK_OrNode) {
    int l = me_static_pred(nt, nt_ref(nt, pred, "left"));
    if (l == 1) return 1;
    if (l == 0) return me_static_pred(nt, nt_ref(nt, pred, "right"));
    return -1;
  }
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, pred, "name");
    if (!nm || (!sp_streq(nm, "method_defined?") && !sp_streq(nm, "public_method_defined?")) ||
        nt_ref(nt, pred, "block") >= 0) return -1;
    const char *leaf = me_const_leaf(nt, nt_ref(nt, pred, "receiver"));
    if (!leaf || !me_leaf_defined(nt, leaf)) return -1;
    int args = nt_ref(nt, pred, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1 || !av) return -1;
    NodeKind ak = nt_kind(nt, av[0]);
    const char *m = ak == NK_SymbolNode ? nt_str(nt, av[0], "value") : ak == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
    if (!m) return -1;
    return me_body_defines(nt, leaf, m, -1) ? 1 : 0;
  }
  return -1;
}
/* does the expression name the body's own class, by the program's text? */
static int me_is_own(const NodeTable *nt, int e, const char *own, int depth) {
  if (e < 0 || depth > 8) return 0;
  NodeKind k = nt_kind(nt, e);
  if (k == NK_SelfNode) return 1;
  const char *leaf = me_const_leaf(nt, e);
  if (leaf) return sp_streq(leaf, own);
  if (k == NK_ParenthesesNode) {
    int body = nt_ref(nt, e, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn == 1 && me_is_own(nt, bb[0], own, depth + 1);
  }
  if (k == NK_IfNode || k == NK_UnlessNode) {
    int p = me_static_pred(nt, nt_ref(nt, e, "predicate"));
    if (p < 0) return 0;
    int take_then = k == NK_IfNode ? p : !p;
    int arm;
    if (take_then) arm = nt_ref(nt, e, "statements");
    else {
      int sub = nt_ref(nt, e, k == NK_IfNode ? "subsequent" : "else_clause");
      if (sub >= 0 && nt_type(nt, sub) && sp_streq(nt_type(nt, sub), "ElseNode")) arm = nt_ref(nt, sub, "statements");
      else arm = sub;
    }
    if (arm < 0) return 0;
    if (nt_kind(nt, arm) == NK_StatementsNode) {
      int bn = 0; const int *bb = nt_arr(nt, arm, "body", &bn);
      return bn == 1 && me_is_own(nt, bb[0], own, depth + 1);
    }
    return me_is_own(nt, arm, own, depth + 1);
  }
  return 0;
}
static int me_count_writes(const NodeTable *nt, int n, const char *v) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return 0;
  int cnt = 0;
  if ((k == NK_LocalVariableWriteNode || k == NK_LocalVariableTargetNode || k == NK_LocalVariableOrWriteNode ||
       k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode) &&
      nt_str(nt, n, "name") && sp_streq(nt_str(nt, n, "name"), v)) cnt++;
  const SpNode *nd = &nt->nodes[n];
  for (int j = 0; j < nd->nr; j++) cnt += me_count_writes(nt, nd->r[j].ref, v);
  for (int j = 0; j < nd->na; j++)
    for (int k2 = 0; k2 < nd->a[j].n; k2++) cnt += me_count_writes(nt, nd->a[j].ids[k2], v);
  return cnt;
}
int desugar_body_module_eval(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ModuleNode && k != NK_ClassNode) continue;
    const char *own = me_const_leaf(nt, nt_ref(nt, id, "constant_path"));
    int body = nt_ref(nt, id, "body");
    if (!own || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb0 = nt_arr(nt, body, "body", &bn);
    int *bb = (int *)malloc(sizeof(int) * (size_t)(bn > 0 ? bn : 1));
    if (!bb) continue;
    memcpy(bb, bb0, sizeof(int) * (size_t)bn);
    int *nb = NULL, nbn = 0, any = 0;
    for (int i = 0; i < bn; i++) {
      int st = bb[i];
      int blk = nt_kind(nt, st) == NK_CallNode ? nt_ref(nt, st, "block") : -1;
      const char *nm = blk >= 0 ? nt_str(nt, st, "name") : NULL;
      int is_eval = nm && (sp_streq(nm, "class_eval") || sp_streq(nm, "module_eval") ||
                           sp_streq(nm, "class_exec") || sp_streq(nm, "module_exec")) &&
                    nt_kind(nt, blk) == NK_BlockNode && nt_ref(nt, blk, "parameters") < 0 &&
                    nt_ref(nt, st, "arguments") < 0;
      int recv = is_eval ? nt_ref(nt, st, "receiver") : -1;
      int is_own = 0;
      if (is_eval && recv >= 0) {
        if (nt_kind(nt, recv) == NK_LocalVariableReadNode) {
          const char *v = nt_str(nt, recv, "name");
          /* the local's one write, a statement of this body ahead of the call */
          if (v && me_count_writes(nt, body, v) == 1)
            for (int j = 0; j < i; j++)
              if (nt_kind(nt, bb[j]) == NK_LocalVariableWriteNode && nt_str(nt, bb[j], "name") &&
                  sp_streq(nt_str(nt, bb[j], "name"), v))
                is_own = me_is_own(nt, nt_ref(nt, bb[j], "value"), own, 0);
        }
        else is_own = me_is_own(nt, recv, own, 0);
      }
      int bbody = is_own ? nt_ref(nt, blk, "body") : -1;
      if (bbody >= 0 && nt_kind(nt, bbody) == NK_StatementsNode) {
        int en = 0; const int *es = nt_arr(nt, bbody, "body", &en);
        for (int j = 0; j < en; j++) xc_push(&nb, &nbn, es[j]);
        any = 1;
        continue;
      }
      if (bbody >= 0) { xc_push(&nb, &nbn, bbody); any = 1; continue; }
      xc_push(&nb, &nbn, st);
    }
    if (any) { nt_node_set_arr(nt, body, "body", nb, nbn); changed = 1; }
    free(nb); free(bb);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- `self.m(...)` in a class or module body --------------------------------
   A statement of a class or module body runs with `self` the class object,
   so `self.m(...)` there is the class's own method: activesupport's
   IsolatedExecutionState sets `self.isolation_level = :thread` in its body
   after defining the writer in `class << self`. The call's receiver becomes
   the class's constant, which the class-method dispatch already serves
   (`Mod.m`); the bare `m` form was served all along. Runs in the fixpoint,
   after the scope pass has recorded each node's enclosing body. */
int desugar_body_self_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_SelfNode) continue;
    int cb = self_class_body(c, recv);
    if (cb < 0 || !c->classes[cb].name) continue;
    long long line = nt_int(nt, recv, "node_line", 0), file = nt_int(nt, recv, "node_file", 0),
              col = nt_int(nt, recv, "node_col", 0);
    nt_node_reset(nt, recv, "ConstantReadNode");
    nt_node_set_str(nt, recv, "name", c->classes[cb].name);
    nt_node_set_int(nt, recv, "node_line", line);
    nt_node_set_int(nt, recv, "node_file", file);
    nt_node_set_int(nt, recv, "node_col", col);
    nt_node_set_int(nt, id, "self_call", 1);   /* still a call on self: a private class method answers it */
    changed = 1;
  }
  return changed;
}


/* ---- `extend self` in a module body --------------------------------------
   Every instance method the module defines is also the module's own:
   `Inflector.underscore(name)` beside `include Inflector; underscore(name)`.
   module_function is the same idea with a private instance copy, and Spinel
   models it by one class-level function that bare calls in includers are
   redirected to -- which leaves an explicit `obj.m` on an includer with no
   target. `extend self` keeps the instance defs exactly as written and adds
   a `def self.<name>` clone of each beside them, before any scope exists;
   the `extend self` statement itself is then dropped. The statement may sit
   before or after the defs, and in any reopening of the module: it extends
   the module object, so every method of the module counts, wherever its body
   was reopened (Inflector's transliterate.rb, loaded before the methods.rb
   that carries the `extend self`, defines parameterize). activesupport's
   Inflector is the shape, reached from its autoloader as
   `Inflector.underscore(full)`. */
static void fwd_ns_key(const NodeTable *nt, int id, char *key, size_t cap);
static void fwd_key_add(char *key, size_t cap, const char *seg);
static int xs_is_extend_self(const NodeTable *nt, int st) {
  if (nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, st, "name");
  if (!nm || !sp_streq(nm, "extend") || nt_ref(nt, st, "block") >= 0) return 0;
  int an = 0; int args = nt_ref(nt, st, "arguments");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  return an == 1 && av && nt_kind(nt, av[0]) == NK_SelfNode;
}
/* the module's own key: the namespace it is opened in and its name */
static void xs_module_key(const NodeTable *nt, int mod, char *key, size_t cap) {
  key[0] = 0;
  fwd_ns_key(nt, mod, key, cap);
  fwd_key_add(key, cap, nt_str(nt, nt_ref(nt, mod, "constant_path"), "name"));
}
static int xs_body_extends_self(const NodeTable *nt, int mod) {
  int body = nt_ref(nt, mod, "body");
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  for (int i = 0; i < bn; i++) if (xs_is_extend_self(nt, bb[i])) return 1;
  return 0;
}
int desugar_extend_self(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  /* The namespace keys of the modules some body of which carries the
     statement, taken before any body is rewritten (the statement is dropped
     as its body is), so a reopening after that body still finds it; a
     same-named module elsewhere has another key. */
  enum { XS_MAX = 256 };
  char keys[XS_MAX][512]; int nkeys = 0;
  for (int id = 0; id < n0 && nkeys < XS_MAX; id++) {
    if (nt_kind(nt, id) != NK_ModuleNode || !xs_body_extends_self(nt, id)) continue;
    xs_module_key(nt, id, keys[nkeys], sizeof keys[nkeys]);
    nkeys++;
  }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_ModuleNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    char key[512];
    xs_module_key(nt, id, key, sizeof key);
    int has = 0;
    for (int k = 0; k < nkeys && !has; k++) if (sp_streq(keys[k], key)) has = 1;
    if (!has) continue;
    int *nb = (int *)malloc(sizeof(int) * (size_t)(bn * 2 + 1)); int nbn = 0;
    int *old = (int *)malloc(sizeof(int) * (size_t)bn);
    if (!nb || !old) { free(nb); free(old); continue; }
    memcpy(old, bb, sizeof(int) * (size_t)bn);
    for (int i = 0; i < bn; i++) {
      int st = old[i];
      if (xs_is_extend_self(nt, st)) continue;   /* dropped: the clones are what it meant */
      nb[nbn++] = st;
      if (nt_kind(nt, st) != NK_DefNode || nt_ref(nt, st, "receiver") >= 0) continue;
      int clone = nt_clone_subtree(nt, st);
      if (clone < 0) continue;
      int sf = nt_new_node(nt, "SelfNode");
      if (sf < 0) continue;
      nt_node_set_ref(nt, clone, "receiver", sf);
      nb[nbn++] = clone;
    }
    nt_node_set_arr(nt, body, "body", nb, nbn);
    free(nb); free(old);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  return changed;
}

/* ---- `module Kernel` reopened at the top level ----------------------------
   Kernel is mixed into Object, so a method the reopening defines is a bare
   call from every scope -- activesupport's core_ext/kernel/reporting.rb adds
   silence_warnings, called from a module method as `silence_warnings { ... }`.
   A top-level def is exactly that in Spinel's model, so the reopening's bare
   defs move to the program root, in place of the module (which is dropped
   once only its defs and visibility markers are left; anything else keeps a
   module of the rest). reporting.rb is `module_function`, which makes the
   methods callable on the module too: a `Kernel.m(...)` naming a hoisted
   method drops its receiver, as the builtin Kernel functions do. */
static int kr_is_visibility_marker(const NodeTable *nt, int st) {
  if (nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "receiver") >= 0 ||
      nt_ref(nt, st, "arguments") >= 0 || nt_ref(nt, st, "block") >= 0) return 0;
  const char *nm = nt_str(nt, st, "name");
  return nm && (sp_streq(nm, "module_function") || sp_streq(nm, "private") ||
                sp_streq(nm, "public") || sp_streq(nm, "protected"));
}
int desugar_kernel_reopen(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int root = nt->root_id;
  int top = root >= 0 ? nt_ref(nt, root, "statements") : -1;
  if (top < 0) return 0;
  int tn = 0; const int *tb0 = nt_arr(nt, top, "body", &tn);
  if (!tb0 || tn == 0) return 0;
  int n0 = nt->count;
  int hoisted = 0;
  for (int i = 0; i < tn; i++) {
    int st = tb0[i];
    int cp = nt_kind(nt, st) == NK_ModuleNode ? nt_ref(nt, st, "constant_path") : -1;
    if (cp < 0 || nt_kind(nt, cp) != NK_ConstantReadNode) continue;
    const char *mn = nt_str(nt, cp, "name");
    if (!mn || !sp_streq(mn, "Kernel")) continue;
    int body = nt_ref(nt, st, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++)
      if (nt_kind(nt, bb[k]) == NK_DefNode && nt_ref(nt, bb[k], "receiver") < 0) hoisted++;
  }
  if (!hoisted) return 0;
  enum { KR_MAX = 256 };
  const char *names[KR_MAX]; int nn = 0;
  int *tb = (int *)malloc(sizeof(int) * (size_t)tn);
  int *nb = (int *)malloc(sizeof(int) * (size_t)(tn + hoisted));
  if (!tb || !nb) { free(tb); free(nb); return 0; }
  memcpy(tb, tb0, sizeof(int) * (size_t)tn);
  int nbn = 0;
  for (int i = 0; i < tn; i++) {
    int st = tb[i];
    int cp = nt_kind(nt, st) == NK_ModuleNode ? nt_ref(nt, st, "constant_path") : -1;
    const char *mn = cp >= 0 && nt_kind(nt, cp) == NK_ConstantReadNode ? nt_str(nt, cp, "name") : NULL;
    int body = mn && sp_streq(mn, "Kernel") ? nt_ref(nt, st, "body") : -1;
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) { nb[nbn++] = st; continue; }
    int bn = 0; const int *bb0 = nt_arr(nt, body, "body", &bn);
    int *rest = (int *)malloc(sizeof(int) * (size_t)(bn + 1));
    if (!rest) { nb[nbn++] = st; continue; }
    int nrest = 0;
    for (int k = 0; k < bn; k++) {
      int d = bb0[k];
      if (nt_kind(nt, d) == NK_DefNode && nt_ref(nt, d, "receiver") < 0) {
        nb[nbn++] = d;
        if (nn < KR_MAX && nt_str(nt, d, "name")) names[nn++] = nt_str(nt, d, "name");
      }
      else if (!kr_is_visibility_marker(nt, d)) rest[nrest++] = d;
    }
    if (nrest) { nt_node_set_arr(nt, body, "body", rest, nrest); nb[nbn++] = st; }
    free(rest);
  }
  nt_node_set_arr(nt, top, "body", nb, nbn);
  free(tb); free(nb);
  /* Kernel.silence_warnings { } -> silence_warnings { } */
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name"), *cn = nt_str(nt, id, "name");
    if (!rn || !cn || !sp_streq(rn, "Kernel")) continue;
    for (int k = 0; k < nn; k++)
      if (sp_streq(names[k], cn)) { nt_node_set_ref(nt, id, "receiver", -1); break; }
  }
  comp_grow_node_arrays(c);
  return 1;
}

/* ---- `singleton_class.prepend(Mod)` in a class or module body ------------
   The statement adds Mod's methods to the class object -- what `extend Mod`
   does, the precedence between Mod and the class's own singleton methods
   aside (prepend puts Mod first; a static program has no such override to
   arbitrate). activesupport's core_ext/enumerable.rb prepends a
   const_missing hook onto Enumerable's singleton this way, and the whole
   statement was refused as Object#singleton_class. `singleton_class.include`
   is the same shape. */
int desugar_singleton_class_mixin(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ModuleNode && k != NK_ClassNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    for (int i = 0; i < bn; i++) {
      int st = bb[i];
      if (nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "block") >= 0) continue;
      const char *nm = nt_str(nt, st, "name");
      if (!nm || (!sp_streq(nm, "prepend") && !sp_streq(nm, "include"))) continue;
      int recv = nt_ref(nt, st, "receiver");
      if (recv < 0 || nt_kind(nt, recv) != NK_CallNode || nt_ref(nt, recv, "receiver") >= 0 ||
          nt_ref(nt, recv, "arguments") >= 0 || nt_ref(nt, recv, "block") >= 0) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, "singleton_class")) continue;
      int args = nt_ref(nt, st, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1 || !av) continue;
      int ok = 1;
      for (int a = 0; a < an; a++)
        if (nt_kind(nt, av[a]) != NK_ConstantReadNode && nt_kind(nt, av[a]) != NK_ConstantPathNode) ok = 0;
      if (!ok) continue;
      nt_node_set_str(nt, st, "name", "extend");
      nt_node_set_ref(nt, st, "receiver", -1);
      nt_node_reset(nt, recv, "NilNode");
      changed = 1;
    }
  }
  return changed;
}

/* ---- `Other::Path::C = remove_const(:C)` -----------------------------------
   activesupport's core_ext/enumerable.rb moves a class it defined out of
   Enumerable:
     ActiveSupport::EnumerableCoreExt::SoleItemExpectedError = remove_const(:SoleItemExpectedError)
   A constant path is modelled flat, by its leaf name, so the path names the
   very class the value is: the write is a self-alias, and its target -- a
   class name -- has no constant slot to write, which refused the program.
   The statement goes; `Other::Path::C` and a lexical `C` both keep reaching
   the class. Only the same-name shape, with the value a bare `C` or
   `remove_const(:C)`, and only where the program defines a class or module
   named C. */
int desugar_constant_path_self_alias(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_ConstantPathWriteNode) continue;
    int tgt = nt_ref(nt, id, "target");
    const char *leaf = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    int v = nt_ref(nt, id, "value");
    if (!leaf || v < 0) continue;
    int same = 0;
    if (nt_kind(nt, v) == NK_ConstantReadNode) {
      const char *vn = nt_str(nt, v, "name");
      same = vn && sp_streq(vn, leaf);
    }
    else if (nt_kind(nt, v) == NK_CallNode && nt_ref(nt, v, "receiver") < 0 && nt_ref(nt, v, "block") < 0 &&
             nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "remove_const")) {
      int args = nt_ref(nt, v, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an == 1 && av) {
        NodeKind ak = nt_kind(nt, av[0]);
        const char *an0 = ak == NK_SymbolNode ? nt_str(nt, av[0], "value") :
                          ak == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
        same = an0 && sp_streq(an0, leaf);
      }
    }
    if (!same || !me_leaf_defined(nt, leaf)) continue;
    nt_node_reset(nt, id, "NilNode");
    changed = 1;
  }
  return changed;
}

/* `recv.const_get(name)` with a NAME known only at run time (a variable, a
   method result, an interpolated string -- activesupport's constantize is
   `Object.const_get(camel_cased_word)`). Every constant the program defines is
   known here, so the call lowers to a static dispatch over those names, the
   way a runtime `send` does over the program's method names: one synthesized
   `recv.const_get(:Name)` arm per candidate, each typed by the fixpoint
   through the literal rule that already resolves a class or a value, and
   codegen emits `name == :N1 ? arm1 : ... : NameError`. The candidate set is
   whole-program, so a runtime name matching none is genuinely undefined and
   the NameError is CRuby's, not a lowering gap. The arm ids are stashed on the
   call under "dyn_cget_arms". A receiverless call in a class method or class
   body was given `self` by desugar_bare_const_get first. */
int desugar_dynamic_const_get_arms(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  /* a user-defined const_get resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "const_get")) return 0; }
  int any = 0;
  for (int id = 0; id < n0 && !any; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "const_get") || nt_ref(nt, id, "receiver") < 0) continue;
    { int dn = 0; nt_arr(nt, id, "dyn_cget_arms", &dn); if (dn > 0) continue; }
    int a = nt_ref(nt, id, "arguments"); if (a < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, a, "arguments", &ac);
    if (ac < 1 || !av) continue;
    NodeKind k0 = nt_kind(nt, av[0]);
    if (k0 == NK_SymbolNode || k0 == NK_StringNode) continue;
    any = 1;
  }
  if (!any) return 0;
  /* the candidates: every constant the program writes or opens as a class or
     module, by its own (last) name -- the flat namespace the literal rule
     resolves in */
  char **cand = NULL; int ncand = 0, candcap = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    const char *v = NULL;
    if (k == NK_ConstantWriteNode) v = nt_str(nt, id, "name");
    else if (k == NK_ClassNode || k == NK_ModuleNode) {
      int cp = nt_ref(nt, id, "constant_path");
      v = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, id, "name");
    }
    if (!v || !*v) continue;
    int dup = 0;
    for (int j = 0; j < ncand && !dup; j++) if (sp_streq(cand[j], v)) dup = 1;
    if (dup) continue;
    if (ncand == candcap) { candcap = candcap ? candcap * 2 : 32; cand = (char **)realloc(cand, sizeof(char *) * (size_t)candcap); }
    cand[ncand++] = strdup(v);
  }
  if (ncand > 512) { for (int k = 512; k < ncand; k++) free(cand[k]); ncand = 512; }
  for (int id = 0; id < n0 && ncand > 0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "const_get")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    { int dn = 0; nt_arr(nt, id, "dyn_cget_arms", &dn); if (dn > 0) continue; }
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    NodeKind k0 = nt_kind(nt, argv[0]);
    if (k0 == NK_SymbolNode || k0 == NK_StringNode) continue;
    int nrest = argc - 1;
    if (nrest > 8) continue;
    int rest[8]; for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];
    int base = nt->count;
    int *arms = (int *)malloc(sizeof(int) * (size_t)ncand);
    if (!arms) break;
    int narm = 0;
    for (int k = 0; k < ncand; k++) {
      int sym = nt_new_node(nt, "SymbolNode"); if (sym < 0) break;
      nt_node_set_str(nt, sym, "value", cand[k]);
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      int aa[9]; aa[0] = sym; for (int j = 0; j < nrest; j++) aa[j + 1] = rest[j];
      nt_node_set_arr(nt, na, "arguments", aa, nrest + 1);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", "const_get");
      nt_node_set_int(nt, call, "dyn_arm", 1);
      nt_node_set_ref(nt, call, "arguments", na);
      nt_node_set_ref(nt, call, "block", -1);
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_cget_arms", arms, narm);
    free(arms);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    int cb = c->node_cbody ? c->node_cbody[id] : -1;
    for (int j = base; j < nt->count; j++) { c->nscope[j] = encl; if (c->node_cbody) c->node_cbody[j] = cb; }
    changed = 1;
  }
  for (int k = 0; k < ncand; k++) free(cand[k]);
  free(cand);
  return changed;
}

/* A receiverless call inside a program's own instance method on Range / Time /
   File / Class (`class Range; def span = last - first`). Ruby resolves it on
   self first, and self here is the builtin value; the scalar reopens
   (String, Integer, ...) answer the same shape from a hand-kept table of
   names in the inference and the emitter. Rather than a fifth table, the call
   is given `self` as its receiver when the builtin answers the name: the
   receiver is set, the call is typed, and a type the inference cannot give
   (an unknown name, or one whose argument types have not settled yet) takes
   the receiver away again for this round. A name the reopen or the program's
   top level defines is left alone, as is a Kernel function -- `puts` inside
   a Range method is Kernel#puts, not a method the Range answers. */
static int reopen_kernel_name(const char *nm) {
  /* Kernel FUNCTIONS: a bare `puts` is Kernel#puts whatever self is. A
     method every object answers on itself (respond_to?, nil?, to_s, dup,
     ...) is not here: bare, it IS a call on self, and giving it the
     receiver lets the ordinary folds and dispatch answer it. */
  static const char *const set[] = {
    "puts", "print", "p", "pp", "printf", "format", "sprintf", "raise", "warn",
    "require", "require_relative", "loop", "lambda", "proc", "block_given?",
    "rand", "srand", "sleep", "exit", "exit!", "abort", "at_exit", "catch", "throw",
    "binding", "caller", "gets", "open", "system", "exec", "fork", "spawn",
    "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex", NULL };
  return str_in(nm, set);
}

int desugar_reopen_implicit_self(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || reopen_kernel_name(nm)) continue;
    if (id >= c->node_cap) continue;
    Scope *sc = comp_scope_of(c, id);
    if (!sc || !sc->name || sc->is_cmethod || sc->class_id < 0 || sc->class_id >= c->nclasses) continue;
    const char *cn = c->classes[sc->class_id].name;
    /* Object / Numeric: self is a boxed value of ANY class, so even the
       reopen's own name goes through self -- `present?` inside
       Object#presence reaches String#present? for a String, as Ruby's bare
       call does. The others hold one builtin kind and call their own
       directly. */
    int boxed_any = cn && (sp_streq(cn, "Object") || sp_streq(cn, "Numeric"));
    if (!cn || !(boxed_any || sp_streq(cn, "Range") || sp_streq(cn, "Time") ||
                 io_family_class(c, sc->class_id) || sp_streq(cn, "Class") ||
                 sp_streq(cn, "Array") || sp_streq(cn, "Hash"))) continue;
    if (!boxed_any && comp_method_in_chain(c, sc->class_id, nm, NULL) >= 0) continue;   /* the reopen's own */
    if (comp_method_index(c, nm) >= 0) continue;                            /* a top-level def */
    int sn = nt_new_node(nt, "SelfNode");
    if (sn < 0) continue;
    comp_grow_node_arrays(c);
    c->nscope[sn] = c->nscope[id];
    if (c->node_cbody) c->node_cbody[sn] = c->node_cbody[id];
    nt_node_set_ref(nt, id, "receiver", sn);
    TyKind t = infer_type(c, id);
    /* A boxed self (Object / Numeric / Array / Hash) keeps the receiver even
       when the call cannot be typed yet: bare, it IS a call on self, and as
       `self.m` an unresolved name is the same run-time NoMethodError the
       explicit spelling gets -- reached only if the method runs. Taking the
       receiver away left a bare unresolved call, which is refused at compile
       time for the whole program: a module method copied into the live Hash
       reopen (activesupport's DeepMergeable#deep_merge!, `merge!(other) do`)
       refused `require "active_support/core_ext/hash/deep_merge"` outright
       though nothing called it. A Range / Time / File / Class self holds one
       builtin kind and still gives the name back for the top level. */
    if (t == TY_UNKNOWN && !boxed_any && !sp_streq(cn, "Array") && !sp_streq(cn, "Hash")) {
      nt_node_set_ref(nt, id, "receiver", -1);
      continue;
    }
    changed = 1;
  }
  return changed;
}

/* ---- a bare call inside a `class Thread` / `class Fiber` reopening ----------
   `thread_variable_get(:tag)` in a method the reopening adds is the thread's
   own builtin method on self. The implicit-self resolution inlines a few
   String / Integer names for the scalar reopenings and knows nothing of a
   handle's surface; give the call its receiver, and the typed dispatch on
   the thread (self is a Thread there) serves it like any `t.m(...)`. Only a
   name no reopening defines (those resolve as the class's own). Runs in the
   fixpoint, after the scope pass. */
int desugar_handle_reopen_self_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || s->class_id < 0 || s->is_cmethod || s->class_id >= c->nclasses) continue;
    const char *cn = c->classes[s->class_id].name;
    if (!cn || (!sp_streq(cn, "Thread") && !sp_streq(cn, "Fiber") &&
                !is_builtin_exception_name(cn))) continue;
    if (comp_method_in_chain(c, s->class_id, nm, NULL) >= 0) continue;
    if (nt_int(nt, id, "vcall", 0) && nt_ref(nt, id, "arguments") < 0) {
      /* a bare identifier is a local read when the scope declares that name
         (`message` in a LoadError reopening is the exception's message) */
      int is_local = 0;
      for (int i = 0; i < s->nlocals; i++)
        if (s->locals[i].name && sp_streq(s->locals[i].name, nm)) { is_local = 1; break; }
      if (is_local) continue;
    }
    int sf = nt_new_node(nt, "SelfNode");
    if (sf < 0) continue;
    nt_node_set_int(nt, sf, "node_line", nt_int(nt, id, "node_line", 0));
    nt_node_set_int(nt, sf, "node_file", nt_int(nt, id, "node_file", 0));
    nt_node_set_ref(nt, id, "receiver", sf);
    comp_grow_node_arrays(c);
    c->nscope[sf] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* ---- `Thread.attr_accessor :x` / `Fiber.attr_accessor :x` -------------------
   An attribute on every thread and fiber object -- activesupport's
   IsolatedExecutionState gives both an active_support_execution_state. A
   thread and a fiber are runtime handles with no ivar slots, but each has a
   store of its own: the thread's thread-local table, the fiber's storage.
   The declaration becomes a reopening appended to the program:
     class Thread
       def x = thread_variable_get(:__attr_x)
       def x=(val) = thread_variable_set(:__attr_x, val)
     end
   (a fiber's through __storage_get / __storage_set on self), keyed apart
   from the program's own thread variables. attr_reader / attr_writer give
   one half. Symbol names only; anything else leaves the call alone. */
static int ma_def(NodeTable *nt, const char *name, int self_recv, int with_val, int body_stmt, long long line);
static int ha_call(NodeTable *nt, const char *m, const char *key, int with_val, long long line) {
  int cl = nt_new_node(nt, "CallNode"); if (cl < 0) return -1;
  nt_node_set_ref(nt, cl, "receiver", -1);
  nt_node_set_str(nt, cl, "name", m);
  nt_node_set_int(nt, cl, "node_line", line);
  nt_node_set_ref(nt, cl, "block", -1);
  int sym = nt_new_node(nt, "SymbolNode"); if (sym < 0) return -1;
  nt_node_set_str(nt, sym, "value", key);
  int args = nt_new_node(nt, "ArgumentsNode"); if (args < 0) return -1;
  int av[2] = { sym, -1 }; int an = 1;
  if (with_val) {
    int rd = nt_new_node(nt, "LocalVariableReadNode"); if (rd < 0) return -1;
    nt_node_set_str(nt, rd, "name", "val"); nt_node_set_int(nt, rd, "depth", 0);
    av[1] = rd; an = 2;
  }
  nt_node_set_arr(nt, args, "arguments", av, an);
  nt_node_set_ref(nt, cl, "arguments", args);
  return cl;
}
int desugar_handle_attr_accessor(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int root = nt->root_id;
  int top = root >= 0 ? nt_ref(nt, root, "statements") : -1;
  if (top < 0 || nt_kind(nt, top) != NK_StatementsNode) return 0;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    int reader = nm && (sp_streq(nm, "attr_accessor") || sp_streq(nm, "attr_reader"));
    int writer = nm && (sp_streq(nm, "attr_accessor") || sp_streq(nm, "attr_writer"));
    if (!reader && !writer) continue;
    int recv = nt_ref(nt, id, "receiver");
    const char *rn = recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL;
    if (!rn || (!sp_streq(rn, "Thread") && !sp_streq(rn, "Fiber"))) continue;
    int is_thread = sp_streq(rn, "Thread");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av0 = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 1 || !av0) continue;
    int ok = 1;
    for (int a = 0; a < an; a++) if (nt_kind(nt, av0[a]) != NK_SymbolNode || !nt_str(nt, av0[a], "value")) ok = 0;
    if (!ok) continue;
    int *av = (int *)malloc(sizeof(int) * (size_t)an);
    if (!av) continue;
    memcpy(av, av0, sizeof(int) * (size_t)an);
    long long line = nt_int(nt, id, "node_line", 0);
    int *defs = NULL, nd = 0;
    for (int a = 0; a < an && ok; a++) {
      const char *base = nt_str(nt, av[a], "value");
      char key[300], wn[300];
      snprintf(key, sizeof key, "__attr_%s", base);
      snprintf(wn, sizeof wn, "%s=", base);
      if (reader) {
        int body = ha_call(nt, is_thread ? "thread_variable_get" : "__storage_get", key, 0, line);
        int d = body >= 0 ? ma_def(nt, base, 0, 0, body, line) : -1;
        if (d < 0) { ok = 0; break; }
        xc_push(&defs, &nd, d);
      }
      if (writer) {
        int body = ha_call(nt, is_thread ? "thread_variable_set" : "__storage_set", key, 1, line);
        int d = body >= 0 ? ma_def(nt, wn, 0, 1, body, line) : -1;
        if (d < 0) { ok = 0; break; }
        xc_push(&defs, &nd, d);
      }
    }
    free(av);
    if (!ok || !nd) { free(defs); continue; }
    int cls = nt_new_node(nt, "ClassNode");
    int cp = nt_new_node(nt, "ConstantReadNode");
    int body = nt_new_node(nt, "StatementsNode");
    if (cls < 0 || cp < 0 || body < 0) { free(defs); continue; }
    nt_node_set_str(nt, cp, "name", rn);
    nt_node_set_ref(nt, cls, "constant_path", cp);
    nt_node_set_ref(nt, cls, "superclass", -1);
    nt_node_set_arr(nt, body, "body", defs, nd);
    nt_node_set_ref(nt, cls, "body", body);
    nt_node_set_int(nt, cls, "node_line", line);
    nt_node_set_int(nt, cls, "node_file", nt_int(nt, id, "node_file", 0));
    free(defs);
    /* the reopening joins the program's statements; the declaration goes */
    int tn = 0; const int *tb = nt_arr(nt, top, "body", &tn);
    int *nb = (int *)malloc(sizeof(int) * (size_t)(tn + 1));
    if (!nb) continue;
    memcpy(nb, tb, sizeof(int) * (size_t)tn);
    nb[tn] = cls;
    nt_node_set_arr(nt, top, "body", nb, tn + 1);
    free(nb);
    nt_node_reset(nt, id, "NilNode");
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
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

/* A String mutator whose receiver is an expression answering an existing
   String is sent to that String in CRuby: `(c ? s : t) << x`,
   `(@buf ||= +"") << x`, `(s).upcase!`, `s.to_s << x`. The mutator
   lowerings act in place only on a receiver they can name, so such a
   receiver was a copy and the mutation was lost. The call moves to where
   the receiver's value is decided:

     (c ? a : b).m(x)   ->  ((c ? a.m(x) : b.m(x)))
     (s1; s2).m(x)      ->  ((s1; s2.m(x)))
     (@v ||= e).m(x)    ->  ((@v ||= e; @v.m(x)))
     (a || b).m(x)      ->  ((a ? a.m(x) : b.m(x)))     a variable's read
     s.to_s.m(x)        ->  s.m(x)                      s a String

   The receiver still runs first and the arguments after it, once on each
   path. Only a String-typed receiver is rewritten, so other programs keep
   their tree; a conditional missing an arm (whose value is nil), a call
   with a block and an argument holding one are left alone. */
static int mrv_no_scope(const NodeTable *nt, int root, int depth) {
  if (root < 0 || root >= nt->count) return 1;
  if (depth > 200) return 0;
  NodeKind k = nt_kind(nt, root);
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode ||
      k == NK_ModuleNode || k == NK_SingletonClassNode) return 0;
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++) if (!mrv_no_scope(nt, nd->r[i].ref, depth + 1)) return 0;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++) if (!mrv_no_scope(nt, nd->a[i].ids[j], depth + 1)) return 0;
  return 1;
}
static int mrv_is_string(TyKind t) { return t == TY_STRING || t == TY_STRBUF; }
/* Does a value leave instead of answering one (`return`, `break`, `next`,
   `redo`, `retry`, also inside a paren)? The call moved onto it would be a
   call on no value, so such an arm keeps the call where it was. */
static int mrv_jumps(const NodeTable *nt, int v) {
  NodeKind k = nt_kind(nt, v);
  if (k == NK_ParenthesesNode) {
    int b = nt_ref(nt, v, "body"), bn = 0;
    const int *bb = b >= 0 && nt_kind(nt, b) == NK_StatementsNode ? nt_arr(nt, b, "body", &bn) : NULL;
    return bn > 0 && mrv_jumps(nt, bb[bn - 1]);
  }
  return k == NK_ReturnNode || k == NK_BreakNode || k == NK_NextNode || k == NK_RedoNode || k == NK_RetryNode;
}
/* Does the subtree under `root` write a variable -- any, or the one named
   `vn` when it is given? Past the depth it follows, it answers that it may. */
static int mrv_writes(const NodeTable *nt, int root, const char *vn, int depth) {
  if (root < 0 || root >= nt->count) return 0;
  if (depth > 200) return 1;
  const char *ty = nt_type(nt, root);
  size_t tl = ty ? strlen(ty) : 0;
  if (((tl > 9 && strcmp(ty + tl - 9, "WriteNode") == 0) || (tl > 10 && strcmp(ty + tl - 10, "TargetNode") == 0)) &&
      strstr(ty, "Variable") && (!vn || sp_streq(nt_str(nt, root, "name"), vn))) return 1;
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++) if (mrv_writes(nt, nd->r[i].ref, vn, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++) if (mrv_writes(nt, nd->a[i].ids[j], vn, depth + 1)) return 1;
  return 0;
}
/* The read of the variable a write node names (`x = v`, `@v ||= e`), or -1
   for another node. */
static int mrv_target_read(NodeTable *nt, int w) {
  const char *ty = nt_type(nt, w);
  static const char *const PFX[] = { "LocalVariable", "InstanceVariable", "GlobalVariable", "ClassVariable", NULL };
  if (!ty) return -1;
  size_t tl = strlen(ty);
  if (tl < 9 || strcmp(ty + tl - 9, "WriteNode") != 0) return -1;
  for (int i = 0; PFX[i]; i++) {
    size_t pl = strlen(PFX[i]);
    if (strncmp(ty, PFX[i], pl) != 0) continue;
    char rt[48]; snprintf(rt, sizeof rt, "%sReadNode", PFX[i]);
    int r = nt_new_node(nt, rt);
    if (r < 0) return -1;
    nt_node_set_str(nt, r, "name", nt_str(nt, w, "name"));
    if (i == 0) nt_node_set_int(nt, r, "depth", nt_int(nt, w, "depth", 0));
    nt_node_set_int(nt, r, "node_line", nt_int(nt, w, "node_line", 0));
    return r;
  }
  return -1;
}
/* The StatementsNodes whose last statement is a value `v` answers: the
   body of a paren, each arm of an if, unless or case. 0 when an arm is
   missing or empty, or `v` is none of these. */
static int mrv_arms(const NodeTable *nt, int v, int *out, int cap) {
  int n = 0;
  switch (nt_kind(nt, v)) {
    case NK_ParenthesesNode: {
      int b = nt_ref(nt, v, "body"), bn = 0;
      if (b < 0 || nt_kind(nt, b) != NK_StatementsNode) return 0;
      nt_arr(nt, b, "body", &bn);
      if (bn < 1 || cap < 1) return 0;
      out[n++] = b;
      return n;
    }
    case NK_IfNode: case NK_UnlessNode: {
      for (int cur = v; cur >= 0 && n < cap; ) {
        int st = nt_ref(nt, cur, "statements"), sn = 0;
        if (st < 0) return 0;
        nt_arr(nt, st, "body", &sn);
        if (sn < 1) return 0;
        out[n++] = st;
        int sub = nt_ref(nt, cur, nt_kind(nt, cur) == NK_IfNode ? "subsequent" : "else_clause");
        if (sub < 0) return 0;
        if (nt_kind(nt, sub) == NK_IfNode) { cur = sub; continue; }
        if (nt_kind(nt, sub) != NK_ElseNode) return 0;
        int es = nt_ref(nt, sub, "statements"), en = 0;
        if (es < 0) return 0;
        nt_arr(nt, es, "body", &en);
        if (en < 1 || n >= cap) return 0;
        out[n++] = es;
        return n;
      }
      return 0;
    }
    case NK_CaseNode: {
      int wn = 0; const int *w = nt_arr(nt, v, "conditions", &wn);
      for (int i = 0; i < wn; i++) {
        int st = nt_ref(nt, w[i], "statements"), sn = 0;
        if (st < 0 || n >= cap) return 0;
        nt_arr(nt, st, "body", &sn);
        if (sn < 1) return 0;
        out[n++] = st;
      }
      int e = nt_ref(nt, v, "else_clause");
      int es = e >= 0 ? nt_ref(nt, e, "statements") : -1, en = 0;
      if (es < 0 || n >= cap) return 0;
      nt_arr(nt, es, "body", &en);
      if (en < 1) return 0;
      out[n++] = es;
      return n;
    }
    default:
      return 0;
  }
}
/* Is a paren's value one the call cannot already reach: a variable's read
   or write, a conditional, another paren, or a String method answering its
   receiver? (`(s << "a") << "b"` already appends to s.) */
static int mrv_paren_value(const NodeTable *nt, int v) {
  NodeKind k = nt_kind(nt, v);
  if (k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || k == NK_GlobalVariableReadNode ||
      k == NK_ClassVariableReadNode || k == NK_IfNode || k == NK_UnlessNode || k == NK_CaseNode ||
      k == NK_ParenthesesNode || k == NK_OrNode) return 1;
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, v, "name");
    int a = nt_ref(nt, v, "arguments"), ac = 0;
    if (a >= 0) nt_arr(nt, a, "arguments", &ac);
    return nm && ac == 0 && nt_ref(nt, v, "block") < 0 &&
           (sp_streq(nm, "to_s") || sp_streq(nm, "to_str") || sp_streq(nm, "itself"));
  }
  const char *ty = nt_type(nt, v);
  size_t tl = ty ? strlen(ty) : 0;
  return tl > 9 && strstr(ty, "Variable") && strcmp(ty + tl - 9, "WriteNode") == 0;
}
/* Is `r` a `then`/`yield_self` whose block answers its only parameter,
   unchanged (`s.then { |z| p z; z }`)? */
static int mrv_then_self(Compiler *c, int r) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, r, "name");
  if (!nm || !(sp_streq(nm, "then") || sp_streq(nm, "yield_self"))) return 0;
  int b = nt_ref(nt, r, "block");
  int bp = nt_ref(nt, b, "parameters"), pn = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
  int rn = 0, on = 0; const int *rq = pn >= 0 ? nt_arr(nt, pn, "requireds", &rn) : NULL;
  if (pn >= 0) nt_arr(nt, pn, "optionals", &on);
  if (rn != 1 || on != 0 || nt_ref(nt, pn, "rest") >= 0 || nt_kind(nt, rq[0]) != NK_RequiredParameterNode) return 0;
  const char *zn = nt_str(nt, rq[0], "name");
  int body = nt_ref(nt, b, "body"), n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  if (n < 1 || nt_kind(nt, st[n - 1]) != NK_LocalVariableReadNode || !sp_streq(nt_str(nt, st[n - 1], "name"), zn))
    return 0;
  /* rebound in the block, it answers something else */
  Scope *bs = comp_scope_of(c, rq[0]);
  int si = bs ? (int)(bs - c->scopes) : -1;
  for (int w = si >= 0 ? comp_lvw_first_sc(c, si, zn) : -1; w >= 0; w = comp_lvw_next_sc(c, w))
    if (c->nscope[w] == si && sp_streq(nt_str(nt, w, "name"), zn)) return 0;
  return 1;
}
int desugar_mutator_receiver_value(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int r = nt_ref(nt, id, "receiver");
    if (!nm || r < 0 || !sp_str_mutator(nm, SP_MUT_LOCAL) || nt_ref(nt, id, "block") >= 0) continue;
    int args = nt_ref(nt, id, "arguments");
    /* an argument that writes a variable runs after the receiver: moved
       into an arm, or onto a re-read of the variable, it would replace the
       String the receiver answered (`s.to_s << (s = +"b")`) */
    if (mrv_writes(nt, args, NULL, 0)) continue;
    NodeKind rk = nt_kind(nt, r);
    /* `s.tap { }` answers s: the block runs, then the call on s; so does
       `s.then { |z| ...; z }`, whose block answers its parameter */
    if (rk == NK_CallNode && nt_kind(nt, nt_ref(nt, r, "block")) == NK_BlockNode &&
        (sp_streq(nt_str(nt, r, "name"), "tap") || mrv_then_self(c, r))) {
      int x = nt_ref(nt, r, "receiver");
      NodeKind xk = nt_kind(nt, x);
      if ((xk != NK_LocalVariableReadNode && xk != NK_InstanceVariableReadNode) ||
          !mrv_is_string(infer_type(c, x))) continue;
      /* a block that assigns the variable leaves it naming another String
         than the one the call answers */
      if (mrv_writes(nt, nt_ref(nt, r, "block"), nt_str(nt, x, "name"), 0)) continue;
      int base = nt->count;
      int x2 = nt_clone_subtree(nt, x), st = nt_new_node(nt, "StatementsNode"), pr = nt_new_node(nt, "ParenthesesNode");
      long long line = nt_int(nt, id, "node_line", 0), file = nt_int(nt, id, "node_file", 0);
      int seq[2] = { r, pr };
      nt_node_set_ref(nt, id, "receiver", x2);
      nt_swap_nodes(nt, id, pr);
      nt_node_reset(nt, id, "ParenthesesNode");
      nt_node_set_arr(nt, st, "body", seq, 2);
      nt_node_set_ref(nt, id, "body", st);
      if (line > 0) { nt_node_set_int(nt, id, "node_line", line); nt_node_set_int(nt, id, "node_file", file); }
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      changed = 1;
      continue;
    }
    /* `s.to_s`, `s.to_str`, `s.itself` on a String is s */
    if (rk == NK_CallNode) {
      const char *rn = nt_str(nt, r, "name");
      int ra = nt_ref(nt, r, "arguments"), rac = 0;
      if (ra >= 0) nt_arr(nt, ra, "arguments", &rac);
      int x = nt_ref(nt, r, "receiver");
      if (!rn || rac != 0 || x < 0 || nt_ref(nt, r, "block") >= 0 ||
          !(sp_streq(rn, "to_s") || sp_streq(rn, "to_str") || sp_streq(rn, "itself")) ||
          !mrv_is_string(infer_type(c, x))) continue;
      nt_node_set_ref(nt, id, "receiver", x);
      nt_node_reset(nt, r, "NilNode");
      changed = 1;
      continue;
    }
    /* `(@v ||= e) << x`: the write, then the call on the variable */
    if (mrv_paren_value(nt, r) && strstr(nt_type(nt, r), "WriteNode") && mrv_is_string(infer_type(c, r))) {
      int base = nt->count;
      int tr = mrv_target_read(nt, r), st = nt_new_node(nt, "StatementsNode"), pr = nt_new_node(nt, "ParenthesesNode");
      long long line = nt_int(nt, id, "node_line", 0), file = nt_int(nt, id, "node_file", 0);
      int seq[2] = { r, pr };
      /* the call moves into a new node; the call's own becomes the paren */
      nt_node_set_ref(nt, id, "receiver", tr);
      nt_swap_nodes(nt, id, pr);
      nt_node_reset(nt, id, "ParenthesesNode");
      nt_node_set_arr(nt, st, "body", seq, 2);
      nt_node_set_ref(nt, id, "body", st);
      if (line > 0) { nt_node_set_int(nt, id, "node_line", line); nt_node_set_int(nt, id, "node_file", file); }
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      changed = 1;
      continue;
    }
    if (rk != NK_ParenthesesNode && rk != NK_IfNode && rk != NK_UnlessNode && rk != NK_CaseNode &&
        rk != NK_OrNode) continue;
    if (!mrv_is_string(infer_type(c, r)) || !mrv_no_scope(nt, args, 0)) continue;
    int arms[64], na = 0;
    int orl = -1;
    if (rk == NK_OrNode) {
      orl = nt_ref(nt, r, "left");
      if (mrv_jumps(nt, nt_ref(nt, r, "right"))) continue;
    }
    else {
      na = mrv_arms(nt, r, arms, 64);
      if (na == 0) continue;
      /* an arm longer than the rewrite's buffer is left as it was */
      int big = 0;
      for (int k = 0; k < na && !big; k++) { int bn = 0; nt_arr(nt, arms[k], "body", &bn); big = bn > 255; }
      if (big) continue;
      int jumps = 0;
      for (int k = 0; k < na && !jumps; k++) {
        int bn = 0; const int *bb = nt_arr(nt, arms[k], "body", &bn);
        jumps = mrv_jumps(nt, bb[bn - 1]);
      }
      if (jumps) continue;
      if (rk == NK_ParenthesesNode) {
        int bn = 0; const int *bb = nt_arr(nt, arms[0], "body", &bn);
        if (!mrv_paren_value(nt, bb[bn - 1])) continue;
      }
    }
    int base = nt->count;
    long long line = nt_int(nt, id, "node_line", 0), file = nt_int(nt, id, "node_file", 0);
    /* the call without its receiver and arguments, copied once per path */
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "arguments", -1);
    int paths = rk == NK_OrNode ? 2 : na, shells[64];
    for (int k = 0; k < paths; k++) {
      shells[k] = nt_clone_subtree(nt, id);
      int ak = k == 0 ? args : (args >= 0 ? nt_clone_subtree(nt, args) : -1);
      nt_node_set_ref(nt, shells[k], "arguments", ak);
    }
    int top = r;
    if (rk == NK_OrNode) {
      /* `a || b` re-reads a variable's a in the arm that takes it; any
         other a is run once into a local (`(t = a) ? t.m(x) : b.m(x)`) */
      int iff = nt_new_node(nt, "IfNode"), s1 = nt_new_node(nt, "StatementsNode");
      int el = nt_new_node(nt, "ElseNode"), s2 = nt_new_node(nt, "StatementsNode");
      NodeKind lk = nt_kind(nt, orl);
      int pred = orl, re;
      if (lk == NK_LocalVariableReadNode || lk == NK_InstanceVariableReadNode) re = nt_clone_subtree(nt, orl);
      else {
        char tname[48]; snprintf(tname, sizeof tname, "__mrv_%d", id);
        pred = nt_new_node(nt, "LocalVariableWriteNode");
        re = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, pred, "name", tname); nt_node_set_int(nt, pred, "depth", 0);
        nt_node_set_ref(nt, pred, "value", orl);
        nt_node_set_str(nt, re, "name", tname); nt_node_set_int(nt, re, "depth", 0);
        scope_local_intern(comp_scope_of(c, id), tname);
      }
      nt_node_set_ref(nt, iff, "predicate", pred);
      nt_node_set_ref(nt, shells[0], "receiver", re);
      nt_node_set_arr(nt, s1, "body", &shells[0], 1);
      nt_node_set_ref(nt, iff, "statements", s1);
      nt_node_set_ref(nt, shells[1], "receiver", nt_ref(nt, r, "right"));
      nt_node_set_arr(nt, s2, "body", &shells[1], 1);
      nt_node_set_ref(nt, el, "statements", s2);
      nt_node_set_ref(nt, iff, "subsequent", el);
      nt_node_set_int(nt, iff, "node_line", line);
      nt_node_reset(nt, r, "NilNode");
      top = iff;
    }
    else
      for (int k = 0; k < na; k++) {
        int bn = 0; const int *bb = nt_arr(nt, arms[k], "body", &bn);
        int body[256];
        memcpy(body, bb, sizeof(int) * (size_t)bn);
        int last = body[bn - 1], tr = mrv_target_read(nt, last);
        if (tr >= 0) {
          nt_node_set_ref(nt, shells[k], "receiver", tr);
          body[bn++] = shells[k];
        }
        else {
          nt_node_set_ref(nt, shells[k], "receiver", last);
          body[bn - 1] = shells[k];
        }
        nt_node_set_arr(nt, arms[k], "body", body, bn);
      }
    /* the call's node is the value now: a paren around it */
    int st = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, st, "body", &top, 1);
    nt_node_reset(nt, id, "ParenthesesNode");
    nt_node_set_ref(nt, id, "body", st);
    if (line > 0) { nt_node_set_int(nt, id, "node_line", line); nt_node_set_int(nt, id, "node_file", file); }
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
      char tmpn[64]; snprintf(tmpn, sizeof tmpn, "_spwi%s", comp_node_tag(c, id));
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

/* reduce(&pr) -> reduce { |a, b| pr.call(a, b) }, and so for the comparators
   sort, sort!, min, max and minmax, whose emitters read a block's body too
   and ran a Proc block argument as if no block were given. */
int desugar_reduce_proc_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject") && !sp_streq(nm, "sort") &&
                !sp_streq(nm, "sort!") && !sp_streq(nm, "min") && !sp_streq(nm, "max") &&
                !sp_streq(nm, "minmax"))) continue;
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
    /* the method's own `&b` handed on is nil when its caller gave no block,
       and a comparator then compares by <=>: the forward keeps it */
    if (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject") && nt_kind(nt, ex) == NK_LocalVariableReadNode) {
      Scope *es = comp_scope_of(c, ex);
      const char *en = nt_str(nt, ex, "name");
      if (es && es->blk_param && en && sp_streq(es->blk_param, en)) continue;
    }

    int base = nt->count;
    char pn[2][64]; int reqs[2], reads[2];
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
      snprintf(pn[k], sizeof pn[k], "__fold_%s_%d", comp_node_tag(c, id), k);
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
  return str_in(n, M);
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
    expand_static_splat_args(c, id, id + 1);
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
    int get = nm && sp_streq(nm, "local_variable_get"), set = nm && sp_streq(nm, "local_variable_set");
    if (!get && !set && (!nm || !sp_streq(nm, "local_variable_defined?"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    if (nt_ref(nt, recv, "receiver") >= 0) continue;      /* binding must be receiverless */
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "binding")) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 + set || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "SymbolNode")) continue;
    const char *vn = nt_str(nt, av[0], "value");
    if (!vn) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes) continue;
    if (vn[0] == '_' && vn[1] >= '1' && vn[1] <= '9' && !vn[2]) {
      char msg[64];
      snprintf(msg, sizeof msg, "numbered parameter '%s' is not a local variable", vn);
      int base = nt->count, sym = av[0], val = set ? av[1] : -1;
      int ecn = nt_new_node(nt, "ConstantReadNode"), emn = nt_new_node(nt, "StringNode");
      int rargs = nt_new_node(nt, "ArgumentsNode");
      if (val >= 0) {
        int ps = nt_new_node(nt, "StatementsNode"), seq[2] = { val, sym };
        sym = nt_new_node(nt, "ParenthesesNode");
        nt_node_set_arr(nt, ps, "body", seq, 2);
        nt_node_set_ref(nt, sym, "body", ps);
      }
      int na[2] = { emn, sym };
      nt_node_set_str(nt, ecn, "name", "NameError");
      nt_node_set_str(nt, emn, "content", msg);
      nt_node_set_arr(nt, args, "arguments", na, 2);
      nt_node_set_ref(nt, recv, "receiver", ecn);
      nt_node_set_str(nt, recv, "name", "new");
      nt_node_set_ref(nt, recv, "arguments", args);
      nt_node_set_arr(nt, rargs, "arguments", &recv, 1);
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_str(nt, id, "name", "raise");
      nt_node_set_ref(nt, id, "arguments", rargs);
      comp_grow_node_arrays(c);
      for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
      changed = 1;
      continue;
    }
    if (!get || !scope_local(&c->scopes[sc], vn)) continue;
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


static void engine_blank(NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return;
  int nr = nt_num_refs(nt, id);
  for (int j = 0; j < nr; j++) engine_blank(nt, nt_ref_at(nt, id, j));
  int na = nt_num_arrs(nt, id);
  for (int j = 0; j < na; j++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, j, &n);
    int *copy = n > 0 ? (int *)malloc(sizeof(int) * (size_t)n) : NULL;
    if (copy) memcpy(copy, ids, sizeof(int) * (size_t)n);
    for (int k = 0; k < n; k++) engine_blank(nt, copy[k]);
    free(copy);
  }
  nt_node_reset(nt, id, "NilNode");
}

static int engine_const(NodeTable *nt, int id, const char *name) {
  const char *nm = nt_kind(nt, id) == NK_ConstantReadNode ? nt_str(nt, id, "name") : NULL;
  return nm && sp_streq(nm, name);
}

static const char *engine_operand(NodeTable *nt, int id, int eng, int ver, int *con, int *gem) {
  if (nt_kind(nt, id) == NK_StringNode) return nt_str(nt, id, "content");
  if (engine_const(nt, id, "RUBY_ENGINE")) { *con = 1; return eng ? "spinel" : NULL; }
  if (engine_const(nt, id, "RUBY_VERSION")) { *con = 1; return ver ? SP_RUBY_VERSION : NULL; }
  const char *nm = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int recv = nt_ref(nt, id, "receiver"), args = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (!nm || nt_ref(nt, id, "block") >= 0) return NULL;
  *gem = 1;
  const char *vn = nt_kind(nt, recv) == NK_ConstantPathNode ? nt_str(nt, recv, "name") : NULL;
  if (!sp_streq(nm, "new") || ac != 1 || !vn || !sp_streq(vn, "Version") || !engine_const(nt, nt_ref(nt, recv, "parent"), "Gem")) return NULL;
  int g = 0;
  const char *v = engine_operand(nt, av[0], 0, ver, con, &g);
  return g ? NULL : v;
}

static int engine_numeric(const char *s) {
  size_t n = strlen(s);
  for (const char *p = s; *p; p++) if (strspn(p, "0123456789") > 18) return 0;
  return n && strspn(s, "0123456789.") == n && s[0] != '.' && s[n - 1] != '.' && !strstr(s, "..");
}

static int engine_version_cmp(const char *a, const char *b) {
  while (*a || *b) {
    char *ea, *eb;
    long long x = strtoll(a, &ea, 10), y = strtoll(b, &eb, 10);
    if (x != y) return x < y ? -1 : 1;
    a = *ea ? ea + 1 : ea;
    b = *eb ? eb + 1 : eb;
  }
  return 0;
}

static const char *engine_written(NodeTable *nt, int id) {
  NodeKind k = nt_kind(nt, id);
  if (k == NK_ClassNode || k == NK_ModuleNode) return nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
  if (k != NK_ConstantWriteNode && k != NK_ConstantOrWriteNode && k != NK_ConstantAndWriteNode &&
      k != NK_ConstantOperatorWriteNode && k != NK_ConstantTargetNode && k != NK_ConstantPathWriteNode &&
      k != NK_ConstantPathOrWriteNode && k != NK_ConstantPathAndWriteNode &&
      k != NK_ConstantPathOperatorWriteNode && k != NK_ConstantPathTargetNode) return NULL;
  int t = nt_ref(nt, id, "target");
  return t >= 0 ? nt_str(nt, t, "name") : nt_str(nt, id, "name");
}

/* Is `id` inside the body of a class, module or singleton class (other than
   the one it is)? Such a node's constant lookups and writes start from that
   namespace, not Object. */
static int engine_lexically_nested(NodeTable *nt, int id) {
  for (int n = 0; n < nt->count; n++) {
    NodeKind k = nt_kind(nt, n);
    if (n == id || (k != NK_ClassNode && k != NK_ModuleNode && k != NK_SingletonClassNode)) continue;
    if (subtree_has(nt, nt_ref(nt, n, "body"), id)) return 1;
  }
  return 0;
}

/* The namespace a constant path's parent names is Object itself (`::X`,
   `Object::X`, `::Object::X`), or anything else. */
static int engine_parent_is_object(NodeTable *nt, int path) {
  int par = nt_ref(nt, path, "parent");
  if (par < 0) return 1;
  NodeKind pk = nt_kind(nt, par);
  if (pk == NK_ConstantPathNode && nt_ref(nt, par, "parent") >= 0) return 0;
  return (pk == NK_ConstantReadNode || pk == NK_ConstantPathNode) && nt_str(nt, par, "name") &&
         sp_streq(nt_str(nt, par, "name"), "Object");
}

/* The innermost class, module or singleton class body `id` is in, or -1. */
static int engine_inner_body(NodeTable *nt, int id) {
  int inner = -1;
  for (int n = 0; n < nt->count; n++) {
    NodeKind nk = nt_kind(nt, n);
    if (n == id || (nk != NK_ClassNode && nk != NK_ModuleNode && nk != NK_SingletonClassNode)) continue;
    if (!subtree_has(nt, nt_ref(nt, n, "body"), id)) continue;
    if (inner < 0 || subtree_has(nt, nt_ref(nt, inner, "body"), n)) inner = n;
  }
  return inner;
}

/* Is `name` Object, Kernel or BasicObject, whose constants Object's lookup
   reaches, or a name the program also assigns as a value (`K = Kernel`)? */
static int engine_maybe_object_chain(NodeTable *nt, const char *name) {
  if (!name || sp_streq(name, "Object") || sp_streq(name, "Kernel") || sp_streq(name, "BasicObject")) return 1;
  for (int w = 0; w < nt->count; w++) {
    NodeKind wk = nt_kind(nt, w);
    if (wk == NK_ClassNode || wk == NK_ModuleNode) continue;
    const char *wn = engine_written(nt, w);
    if (wn && sp_streq(wn, name)) return 1;
  }
  return 0;
}

/* Writer `id` (an engine_written node) provably defines a constant of some
   namespace other than Object: written inside a class/module body that is not
   Object/Kernel/BasicObject (whose constants Object's lookup reaches), or
   through a path qualified by another namespace (`module A::X`, `A::X = 1`). */
static int engine_writer_qualified(NodeTable *nt, int id) {
  NodeKind k = nt_kind(nt, id);
  int path = k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, id, "constant_path") :
             k == NK_ConstantPathTargetNode ? id : nt_ref(nt, id, "target");
  if (path >= 0 && (nt_kind(nt, path) == NK_ConstantPathNode || nt_kind(nt, path) == NK_ConstantPathTargetNode))
    return !engine_parent_is_object(nt, path);
  /* the innermost enclosing body owns the constant */
  int inner = engine_inner_body(nt, id);
  if (inner < 0 || nt_kind(nt, inner) == NK_SingletonClassNode) return 0;  /* `class << obj`: whose? */
  /* `K = Kernel; module K` reopens Kernel: a namespace name that is also
     assigned as a value could be Object's chain */
  return !engine_maybe_object_chain(nt, nt_str(nt, nt_ref(nt, inner, "constant_path"), "name"));
}

/* Does the program mix a module into Object (a top-level `include M`,
   `Object.include M`, ...)? Then a module's constants are Object's too, and
   the namespace test above is off. */
static int engine_object_mixes_in_scan(NodeTable *nt);
/* Asked once per desugar_engine_branches run, not once per guard: the scan
   walks every class body for each include, and a program with hundreds of
   guards and includes took seconds. The pass only blanks nodes, so an
   answer taken before a fold stays the conservative one. */
static int g_engine_mixes = -1;
static int engine_object_mixes_in(NodeTable *nt) {
  if (g_engine_mixes < 0) g_engine_mixes = engine_object_mixes_in_scan(nt);
  return g_engine_mixes;
}
static int engine_object_mixes_in_scan(NodeTable *nt) {
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int mix = sp_streq(nm, "include") || sp_streq(nm, "prepend");
    if (!mix && (sp_streq(nm, "send") || sp_streq(nm, "__send__") || sp_streq(nm, "public_send"))) {
      int ac = 0; const int *av = nt_arr(nt, nt_ref(nt, id, "arguments"), "arguments", &ac);
      const char *s = ac < 1 ? NULL : nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value") :
                      nt_kind(nt, av[0]) == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
      /* a computed method name could be either */
      mix = ac < 1 || !s || sp_streq(s, "include") || sp_streq(s, "prepend");
    }
    if (!mix) continue;
    int r = nt_ref(nt, id, "receiver");
    /* receiverless: top-level self is main, whose include is Object's, and
       in `class Object` / `module Kernel` the body's self is that one */
    if (r < 0 || nt_kind(nt, r) == NK_SelfNode) {
      int inner = engine_inner_body(nt, id);
      if (inner < 0) return 1;
      if (nt_kind(nt, inner) != NK_SingletonClassNode &&
          engine_maybe_object_chain(nt, nt_str(nt, nt_ref(nt, inner, "constant_path"), "name"))) return 1;
      continue;
    }
    /* a named class or module other than Object's chain keeps it; anything
       computed, or a constant also assigned a value (`O = Object`), could be
       Object */
    const char *rn = nt_kind(nt, r) == NK_ConstantReadNode ? nt_str(nt, r, "name") : NULL;
    if (engine_maybe_object_chain(nt, rn)) return 1;
  }
  return 0;
}

/* Does `id` sit where it can run more than once -- in a loop, a block, a
   method or a lambda, or a begin whose rescue may `retry`? */
static int engine_may_rerun(NodeTable *nt, int id) {
  for (int n = 0; n < nt->count; n++) {
    NodeKind k = nt_kind(nt, n);
    if (k == NK_RetryNode) {
      for (int b = 0; b < nt->count; b++)
        if (nt_kind(nt, b) == NK_BeginNode && subtree_has(nt, b, n) && subtree_has(nt, b, id)) return 1;
      continue;
    }
    if (n == id || (k != NK_WhileNode && k != NK_UntilNode && k != NK_ForNode && k != NK_BlockNode &&
                    k != NK_DefNode && k != NK_LambdaNode)) continue;
    if (subtree_has(nt, n, id)) return 1;
  }
  return 0;
}

/* Are `a` and `b` in different arms of one if/unless that runs at most once?
   Then at most one of them runs. A file a program requires under two
   conditions is inlined under each (ffi-yajl's `if FORCE == "ffi" ... else
   begin ... rescue LoadError ... end`), so the copy in the other arm is no
   writer the guard in this one can observe. */
static int engine_exclusive_arms(NodeTable *nt, int a, int b) {
  for (int n = 0; n < nt->count; n++) {
    NodeKind k = nt_kind(nt, n);
    if (k != NK_IfNode && k != NK_UnlessNode) continue;
    int th = nt_ref(nt, n, "statements");
    int el = nt_ref(nt, n, k == NK_UnlessNode ? "else_clause" : "subsequent");
    if (th < 0 || el < 0) continue;
    if (!((subtree_has(nt, th, a) && subtree_has(nt, el, b)) ||
          (subtree_has(nt, el, a) && subtree_has(nt, th, b)))) continue;
    return !engine_may_rerun(nt, n);
  }
  return 0;
}

static const char *engine_body_class(NodeTable *nt, int stmt) {
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_ClassNode && nt_kind(nt, id) != NK_ModuleNode) continue;
    int bn = 0; const int *b = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
    for (int i = 0; i < bn; i++) if (b[i] == stmt) return nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
  }
  return NULL;
}

static int engine_method_absent(NodeTable *nt, const char *cls, const char *m, int in, int depth) {
  if (builtin_method_known(cls, m) || me_body_defines(nt, cls, m, in)) return 0;
  if (is_builtin_class_name(cls) || is_builtin_module_name(cls)) return builtin_method_known(cls, NULL) ? 1 : -1;
  if (!me_leaf_defined(nt, cls) || depth > 16) return -1;
  int a = 1;
  for (int id = 0; id < nt->count && a > 0; id++) {
    const char *nm = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
    if ((nt_kind(nt, id) != NK_ClassNode && nt_kind(nt, id) != NK_ModuleNode) || !nm || !sp_streq(nm, cls)) continue;
    int sc = nt_ref(nt, id, "superclass"), bn = 0;
    if (sc >= 0) a = nt_kind(nt, sc) == NK_ConstantReadNode ? engine_method_absent(nt, nt_str(nt, sc, "name"), m, in, depth + 1) : -1;
    const int *b = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
    for (int i = 0; i < bn && a > 0; i++) {
      const char *n = nt_kind(nt, b[i]) == NK_CallNode && nt_ref(nt, b[i], "receiver") < 0 ? nt_str(nt, b[i], "name") : NULL;
      if (!n || (!sp_streq(n, "include") && !sp_streq(n, "prepend"))) continue;
      int ac = 0; const int *av = nt_arr(nt, nt_ref(nt, b[i], "arguments"), "arguments", &ac);
      for (int k = 0; k < ac && a > 0; k++)
        a = nt_kind(nt, av[k]) == NK_ConstantReadNode ? engine_method_absent(nt, nt_str(nt, av[k], "name"), m, in, depth + 1) : -1;
    }
  }
  return a;
}

static int engine_absent(NodeTable *nt, int e, int in) {
  const char *x = NULL;
  const char *en = nt_kind(nt, e) == NK_CallNode && nt_ref(nt, e, "block") < 0 ? nt_str(nt, e, "name") : NULL;
  if (en && (sp_streq(en, "method_defined?") || sp_streq(en, "public_method_defined?"))) {
    int r = nt_ref(nt, e, "receiver"), ac = 0;
    const int *av = nt_arr(nt, nt_ref(nt, e, "arguments"), "arguments", &ac);
    const char *cls = r < 0 || nt_kind(nt, r) == NK_SelfNode ? engine_body_class(nt, in) :
                      nt_kind(nt, r) == NK_ConstantReadNode ? nt_str(nt, r, "name") : NULL;
    const char *m = ac != 1 ? NULL : nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value") :
                    nt_kind(nt, av[0]) == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
    int a = !cls || !m ? -1 : builtin_object_method_known(m) ? 0 : engine_method_absent(nt, cls, m, in, 0);
    if (a > 0)
      for (int id = 0; id < nt->count; id++) if (me_stmt_defines(nt, id, m) && !subtree_has(nt, in, id)) return -1;
    return a;
  }
  if (nt_kind(nt, e) == NK_DefinedNode) {
    int v = nt_ref(nt, e, "value");
    while (nt_kind(nt, v) == NK_ConstantPathNode && nt_ref(nt, v, "parent") >= 0) v = nt_ref(nt, v, "parent");
    if (nt_kind(nt, v) == NK_ConstantReadNode || nt_kind(nt, v) == NK_ConstantPathNode) x = nt_str(nt, v, "name");
  }
  else if (en && sp_streq(en, "const_defined?") && (nt_kind(nt, nt_ref(nt, e, "receiver")) == NK_ConstantReadNode ||
                                                     nt_kind(nt, nt_ref(nt, e, "receiver")) == NK_ConstantPathNode)) {
    int ac = 0; const int *av = nt_arr(nt, nt_ref(nt, e, "arguments"), "arguments", &ac);
    if (ac >= 1 && nt_kind(nt, av[0]) == NK_SymbolNode) x = nt_str(nt, av[0], "value");
    if (ac >= 1 && nt_kind(nt, av[0]) == NK_StringNode) x = nt_str(nt, av[0], "content");
  }
  if (!x || comp_is_wellknown_const(x) || is_builtin_class_name(x) || is_builtin_module_name(x) ||
      is_builtin_exception_name(x)) return -1;
  /* A top-level `defined?(X)` looks X up in Object alone, so a same-named
     constant some module or class owns (ffi-yajl's FFI_Yajl::FFI beside the
     `require "ffi" unless defined?(FFI)` guard) is not the one it asks about. */
  int top = nt_kind(nt, e) == NK_DefinedNode && !engine_lexically_nested(nt, e) &&
            !engine_object_mixes_in(nt);
  for (int id = 0; id < nt->count; id++)
    if (engine_written(nt, id) && sp_streq(engine_written(nt, id), x) && !subtree_has(nt, in, id) &&
        !(top && engine_writer_qualified(nt, id)) && !engine_exclusive_arms(nt, in, id)) return -1;
  return 1;
}

static int engine_absent_fold(NodeTable *nt, int e, int in) {
  NodeKind k = nt_kind(nt, e);
  if (k == NK_AndNode || k == NK_OrNode)
    return engine_absent_fold(nt, nt_ref(nt, e, "left"), in) | engine_absent_fold(nt, nt_ref(nt, e, "right"), in);
  int neg = k == NK_CallNode && nt_str(nt, e, "name") && sp_streq(nt_str(nt, e, "name"), "!") &&
            nt_ref(nt, e, "arguments") < 0;
  int q = neg ? nt_ref(nt, e, "receiver") : e, a = engine_absent(nt, q, in);
  if (a < 0) return 0;
  engine_blank(nt, q);
  nt_node_reset(nt, e, neg == a ? "TrueNode" : "FalseNode");
  nt_node_set_int(nt, e, "engine_check", 1);
  return 1;
}

int desugar_engine_branches(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0, eng = 1, ver = 1;
  g_engine_mixes = -1;
  /* A program that defines a RUBY_ENGINE of its own (a shim module's
     `RUBY_ENGINE = "jruby"`) reads that one where it is in scope, so the
     fold, which knows only the global, would answer for the wrong constant:
     leave every check alone then. */
  for (int id = 0; id < n0; id++) {
    const char *wn = engine_written(nt, id);
    if (wn && sp_streq(wn, "RUBY_ENGINE")) eng = 0;
    if (wn && sp_streq(wn, "RUBY_VERSION")) ver = 0;
  }
  for (int id = 0; id < n0; id++)
    if (nt_kind(nt, id) == NK_IfNode || nt_kind(nt, id) == NK_UnlessNode)
      changed |= engine_absent_fold(nt, nt_ref(nt, id, "predicate"), id);
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *op = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (!op || recv < 0 || ac != 1 || !av) continue;
    int lc = 0, rc = 0, lg = 0, rg = 0;
    const char *l = engine_operand(nt, recv, eng, ver, &lc, &lg);
    const char *r = engine_operand(nt, av[0], eng, ver, &rc, &rg);
    if (!l || !r || lc == rc || rg > lg || (lg && (!engine_numeric(l) || !engine_numeric(r)))) continue;
    int cmp = lg ? engine_version_cmp(l, r) : strcmp(l, r);
    int truth = sp_streq(op, "==") ? cmp == 0 : sp_streq(op, "!=") ? cmp != 0 : sp_streq(op, "<") ? cmp < 0 :
                sp_streq(op, "<=") ? cmp <= 0 : sp_streq(op, ">") ? cmp > 0 : sp_streq(op, ">=") ? cmp >= 0 :
                sp_streq(op, "start_with?") && !lg ? strncmp(l, r, strlen(r)) == 0 : -1;
    if (truth < 0) continue;
    engine_blank(nt, recv);
    engine_blank(nt, args);
    nt_node_reset(nt, id, truth ? "TrueNode" : "FalseNode");
    nt_node_set_int(nt, id, "engine_check", 1);
    changed = 1;
  }
  for (int id = n0 - 1; id >= 0; id--) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_AndNode && k != NK_OrNode) continue;
    int l = nt_ref(nt, id, "left"), r = nt_ref(nt, id, "right");
    if (nt_int(nt, l, "engine_check", 0) <= 0) continue;
    int v = (nt_kind(nt, l) == NK_TrueNode) == (k == NK_OrNode) ? l : r;
    if (nt_int(nt, v, "engine_check", 0) <= 0) continue;
    int truth = nt_kind(nt, v) == NK_TrueNode;
    engine_blank(nt, l);
    engine_blank(nt, r);
    nt_node_reset(nt, id, truth ? "TrueNode" : "FalseNode");
    nt_node_set_int(nt, id, "engine_check", 1);
    changed = 1;
  }
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_IfNode && k != NK_UnlessNode) continue;
    int pred = nt_ref(nt, id, "predicate");
    if (pred < 0 || nt_int(nt, pred, "engine_check", 0) <= 0) continue;
    int runs_statements = (nt_kind(nt, pred) == NK_TrueNode) == (k == NK_IfNode);
    const char *dead = runs_statements ? (k == NK_IfNode ? "subsequent" : "else_clause") : "statements";
    int d = nt_ref(nt, id, dead);
    if (d < 0) continue;
    engine_blank(nt, d);
    nt_node_set_ref(nt, id, dead, -1);
    changed = 1;
  }
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_StatementsNode) continue;
    int n = 0; const int *st = nt_arr(nt, id, "body", &n);
    for (int k = 0; k < n - 1; k++) {
      NodeKind sk = nt_kind(nt, st[k]);
      int pred = sk == NK_IfNode || sk == NK_UnlessNode ? nt_ref(nt, st[k], "predicate") : -1;
      long long pop = nt_int(nt, st[k], "req_pop", 0);
      const char *other = sk == NK_IfNode ? "subsequent" : "else_clause";
      int folded = pred >= 0 && nt_int(nt, pred, "engine_check", 0) > 0;
      if (pred < 0 || (folded ? (nt_kind(nt, pred) == NK_TrueNode) != (sk == NK_IfNode) :
                                pop <= 0 || nt_ref(nt, st[k], other) >= 0)) continue;
      int body = nt_ref(nt, st[k], "statements");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn == 0 || nt_kind(nt, bb[bn - 1]) != NK_ReturnNode) continue;
      int *keep = malloc(sizeof(int) * (size_t)n);
      if (!keep) break;
      memcpy(keep, st, sizeof(int) * (size_t)n);
      int e = pop > 0 ? k + 1 : n;
      while (e < n && nt_int(nt, keep[e], "req_pop", 0) > 0 && nt_int(nt, keep[e], "req_pop", 0) <= pop) e++;
      if (pop > 0) engine_blank(nt, bb[bn - 1]);
      if (!folded && e > k + 1) {
        int rest = nt_new_node(nt, "StatementsNode"), els = nt_new_node(nt, "ElseNode");
        nt_node_set_arr(nt, rest, "body", keep + k + 1, e - k - 1);
        nt_node_set_ref(nt, els, "statements", rest);
        nt_node_set_ref(nt, keep[k], other, els);
        comp_grow_node_arrays(c);
      }
      for (int j = k + 1; folded && j < e; j++) engine_blank(nt, keep[j]);
      memmove(keep + k + 1, keep + e, sizeof(int) * (size_t)(n - e));
      nt_node_set_arr(nt, id, "body", keep, n - (e - k - 1));
      free(keep);
      changed = 1;
      st = nt_arr(nt, id, "body", &n);
    }
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

static void dsend_add_name(char ***names, int *n, int *cap, ANameHash *seen, const char *v) {
  if (!v || !*v || anh_has(seen, v)) return;
  if (sp_streq(v, "send") || sp_streq(v, "__send__") || sp_streq(v, "public_send")) return;
  if (*n == *cap) { *cap = *cap ? *cap * 2 : 32; *names = (char **)realloc(*names, sizeof(char *) * (size_t)*cap); }
  (*names)[(*n)++] = strdup(v);
  anh_add(seen, (*names)[*n - 1]);
}

/* The names an instance of `cls` answers. A receiver typed `cls` may hold
   an instance of any subclass (`self` in a base-class method that sends
   "on_#{ev}" to a hook only the subclass defines), so with `subclasses`
   set every descendant's methods, readers, writers and aliases count too. */
static const char *const object_methods[] = { "to_s", "inspect", "class", "hash", "frozen?", "nil?",
  "==", "!=", "equal?", "eql?", "respond_to?", "is_a?", "kind_of?", "instance_of?", "freeze", "dup",
  "itself", "object_id", NULL };
static int dsend_receiver_names(Compiler *c, int cls, int subclasses, char ***out) {
  char **names = NULL; int n = 0, cap = 0;
  ANameHash seen; memset(&seen, 0, sizeof seen);
  for (int d = 0; d < c->nclasses; d++) {
    if (cls < 0 ? comp_class_is_module(c, &c->classes[d]) : d != cls && !(subclasses && is_descendant(c, d, cls))) continue;
    for (int s = 0; s < c->nscopes; s++) {
      const Scope *sc = &c->scopes[s];
      if (!sc->name || sc->is_cmethod || sc->class_id < 0) continue;
      if (sp_streq(sc->name, "initialize") || sp_streq(sc->name, "initialize_copy")) continue;
      if (comp_method_in_chain(c, d, sc->name, NULL) >= 0) dsend_add_name(&names, &n, &cap, &seen, sc->name);
    }
    for (int k = d; k >= 0 && k < c->nclasses; k = c->classes[k].parent) {
      ClassInfo *cl = &c->classes[k];
      for (int r = 0; r < cl->nreaders; r++) dsend_add_name(&names, &n, &cap, &seen, cl->readers[r]);
      for (int w = 0; w < cl->nwriters; w++) {
        char wn[256];
        snprintf(wn, sizeof wn, "%s=", cl->writers[w]);
        dsend_add_name(&names, &n, &cap, &seen, wn);
      }
      for (int a = 0; a < cl->naliases; a++) dsend_add_name(&names, &n, &cap, &seen, cl->alias_new[a]);
    }
  }
  for (int k = 0; object_methods[k]; k++) dsend_add_name(&names, &n, &cap, &seen, object_methods[k]);
  anh_free(&seen);
  *out = names;
  return n;
}

/* Does every definition of `name` in the program refuse `argc` positional
   arguments? A name the program does not define (a builtin's) is never
   excluded; an attr reader takes none, an attr writer exactly one. */
static int dsend_defined_arity_excludes(Compiler *c, const char *name, int argc) {
  int defined = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->name || !sp_streq(sc->name, name)) continue;
    defined = 1;
    if (argc >= sc->nrequired && (sc->rest_idx >= 0 || argc <= sc->nparams)) return 0;
  }
  char wbase[256];
  int is_w = setter_base_name(name, wbase, sizeof wbase);
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    if (comp_is_reader(cl, name)) { defined = 1; if (argc == 0) return 0; }
    if (is_w && comp_is_writer(cl, wbase)) { defined = 1; if (argc == 1) return 0; }
  }
  return defined;
}

/* The candidates dsend_candidates took from the program's literals (keys
   point into its answer, valid until the caller frees that). */
static ANameHash g_dsend_lits;

/* The closed set of method names a runtime-name send (and respond_to?)
   dispatches over: the symbol and string literals the program spells, and
   the methods it defines, ranked so the names that can be meant survive the
   cap. Answers the names (strdup'd, the caller frees) and their count. */
char **dsend_candidates(Compiler *c, int *out_n) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  static const char *const sends[] = { "send", "__send__", "public_send", NULL };
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
  anh_free(&g_dsend_lits); memset(&g_dsend_lits, 0, sizeof g_dsend_lits);
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
    anh_add(&g_dsend_lits, cand[ncand - 1]);
  }
  /* ... and the names the program DEFINES: its defs, class methods, attr
     readers and writers (as `x=`). A writer reached as `public_send("#{name}=",
     value)` (activesupport's deprecators) or a method named by concatenation
     is spelled by no literal, yet is exactly what the receiver answers; a
     name outside both sets raises NoMethodError at the dispatch. The
     constructor is left out: it is emitted as a void C function whatever
     its body's value, and an arm typed from that body did not compile. */
  {
    for (int s = 0; s < c->nscopes; s++) {
      const char *sn = c->scopes[s].name;
      if (!sn || !*sn || strncmp(sn, "__", 2) == 0 || strchr(sn, '#') || !dsend_method_name_shaped(sn)) continue;
      if (sp_streq(sn, "initialize")) continue;
      int skip = 0;
      for (int k = 0; sends[k]; k++) if (sp_streq(sn, sends[k])) { skip = 1; break; }
      if (skip || anh_has(&cand_set, sn)) continue;
      if (ncand == candcap) { candcap = candcap ? candcap * 2 : 16; cand = (char **)realloc(cand, sizeof(char *) * candcap); }
      cand[ncand++] = strdup(sn);
      anh_add(&cand_set, cand[ncand - 1]);
    }
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *cl = &c->classes[ci];
      for (int pass = 0; pass < 2; pass++) {
        int n = pass ? cl->nwriters : cl->nreaders;
        char **names = pass ? cl->writers : cl->readers;
        for (int j = 0; j < n; j++) {
          char nm2[200]; snprintf(nm2, sizeof nm2, pass ? "%s=" : "%s", names[j]);
          if (anh_has(&cand_set, nm2) || !dsend_method_name_shaped(nm2)) continue;
          if (ncand == candcap) { candcap = candcap ? candcap * 2 : 16; cand = (char **)realloc(cand, sizeof(char *) * candcap); }
          cand[ncand++] = strdup(nm2);
          anh_add(&cand_set, cand[ncand - 1]);
        }
      }
    }
  }
  anh_free(&cand_set);
  int any_computed = 0;
  for (int id = 0; id < n0 && !any_computed; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int a = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    if (ac >= 1 && av && an_send_name_is_computed(c, av[0])) any_computed = 1;
  }
  if (ncand == 0 && !any_computed) { free(cand); return 0; }
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
      /* the writer table holds the attribute, so `x=` ranks through `x` */
      { char wbase[256];
        if (score[k] < 2 && setter_base_name(cand[k], wbase, sizeof wbase) && anh_has(&defined, wbase)) score[k] = 2; }
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
  *out_n = ncand;
  return cand;
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
  int ncand = 0;
  char **cand = dsend_candidates(c, &ncand);
  char **picked = (char **)malloc(sizeof(char *) * (size_t)(ncand > 0 ? ncand : 1));
  char **lits = (char **)malloc(sizeof(char *) * (size_t)(ncand > 0 ? ncand : 1));
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
         and the explicit form already lowers (#4851). A receiverless
         public_send is `self.public_send` the same way: it refuses a private
         target either way, which is what its arms' vis_enforce stamp does.
         Only a runtime name: a literal one is rewritten earlier. At the top
         level self is main, whose methods are the top-level defs (private
         methods of Object, which send reaches). */
      Scope *ss = comp_scope_of(c, id);
      if (!ss) continue;
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
    /* `recv.public_send(*args, &blk)` (activesupport's Object#try): the name
       is the array's first element and the rest are the arguments. The name
       reads as `args[0]` and every arm takes `*args.drop(1)`; the array is
       read more than once, so only a variable's -- a rest parameter, an
       ivar -- not an expression with effects. Interning the whole array as
       the name answered "undefined method '[:name]'". */
    int splat_src = -1;
    if (argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode) {
      int se = nt_ref(nt, argv[0], "expression");
      if (se < 0) {
        /* an anonymous `*` (a `...` forwarder's `__send__(*, **, &)`): the
           rest local the parameter registration named for it */
        se = nt_new_node(nt, "LocalVariableReadNode");
        if (se < 0) continue;
        nt_node_set_str(nt, se, "name", "__anon_rest");
        comp_grow_node_arrays(c);
        c->nscope[se] = c->nscope[id];
        args = nt_ref(nt, id, "arguments"); argv = nt_arr(nt, args, "arguments", &argc);   /* realloc-safe */
      }
      NodeKind sk = se >= 0 ? nt_kind(nt, se) : NK_NONE;
      if (sk != NK_LocalVariableReadNode && sk != NK_InstanceVariableReadNode) continue;
      splat_src = se;
    }
    int nrest = argc - 1;
    if (nrest > 64) continue;
    char **use = cand; int nuse = ncand;
    char **own = NULL; int nown = 0;
    int computed = an_send_name_is_computed(c, argv[0]);
    int builtin_recv = 0;   /* the arms are a builtin class's methods */
    /* A name that is not computed -- a variable, a parameter, a table read
       (`send(type, ...)`, `send(*DISPATCH[op])`) -- holds one of the names
       the program spells, so its arms are the literals alone. The methods it
       defines are for a computed name (an interpolation, a concatenation),
       which can spell a name no literal does; as arms of every send, their
       return types widened the slots the sends flow into (optcarrot's
       CPU#irq_flags became boxed, 42% more instructions). */
    int nlit = 0;
    if (!computed) {
      for (int k = 0; k < ncand; k++)
        if (anh_has(&g_dsend_lits, cand[k])) lits[nlit++] = cand[k];
      use = lits; nuse = nlit;
    }
    if (!computed && nlit > 256) {
      /* Past the cap, the literals the receiver answers go first and are all
         kept, so a program's other literals can't crowd its own names out. */
      int npick = 0;
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt) || rt == TY_POLY || rt == TY_UNKNOWN) {
        ANameHash answers; memset(&answers, 0, sizeof answers);
        for (int d = 0; d < c->nclasses; d++) {
          if (ty_is_object(rt) ? d != ty_object_class(rt) : comp_class_is_module(c, &c->classes[d])) continue;
          char **rn = NULL; int nrn = dsend_receiver_names(c, d, ty_is_object(rt), &rn);
          for (int k = 0; k < nrn; k++) { if (!anh_has(&answers, rn[k])) anh_add(&answers, rn[k]); else free(rn[k]); }
          free(rn);
        }
        for (int k = 0; k < nlit; k++)
          if (anh_has(&answers, lits[k])) picked[npick++] = lits[k];
        for (int k = 0; k < nlit && npick < 256; k++)
          if (!anh_has(&answers, lits[k])) picked[npick++] = lits[k];
        for (int k = 0; k < answers.n; k++) free((char *)answers.key[k]);
        anh_free(&answers);
      }
      else for (; npick < 256; npick++) picked[npick] = lits[npick];
      use = picked; nuse = npick;
    }
    if (computed) {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) nown = dsend_receiver_names(c, ty_object_class(rt), 1, &own);
      else if (rt == TY_POLY || rt == TY_UNKNOWN) {
        int cap = 0; ANameHash seen; memset(&seen, 0, sizeof seen);
        for (int k = 0; k < c->nclasses; k++) {
          if (comp_class_is_module(c, &c->classes[k])) continue;
          char **kn = NULL; int nk = dsend_receiver_names(c, k, 0, &kn);
          for (int j = 0; j < nk; j++) { dsend_add_name(&own, &nown, &cap, &seen, kn[j]); free(kn[j]); }
          free(kn);
        }
        for (int k = 0; k < ncand && k < 256; k++) dsend_add_name(&own, &nown, &cap, &seen, cand[k]);
        anh_free(&seen);
      }
      else if (rt != TY_CLASS && builtin_class_of_type(rt)) {
        /* a receiver known to be a builtin answers its class's own methods
           (`io.public_send("#{k}=", v)`); the shape filter below keeps the
           ones the name can spell. Not a Class value, whose own singleton
           methods -- what such a send in a class body means -- are not
           Class's. */
        static const char *const enumerable_methods[] = { "all?", "any?", "chain", "chunk",
          "chunk_while", "collect", "collect_concat", "compact", "count", "cycle", "detect", "drop",
          "drop_while", "each_cons", "each_entry", "each_slice", "each_with_index", "each_with_object",
          "entries", "filter", "filter_map", "find", "find_all", "find_index", "first", "flat_map",
          "grep", "grep_v", "group_by", "include?", "inject", "lazy", "map", "max", "max_by",
          "member?", "min", "min_by", "minmax", "minmax_by", "none?", "one?", "partition", "reduce",
          "reject", "reverse_each", "select", "slice_after", "slice_before", "slice_when", "sort",
          "sort_by", "sum", "take", "take_while", "tally", "to_a", "to_h", "to_set", "uniq", "zip", NULL };
        static const char *const comparable_methods[] = { "<", "<=", "==", ">", ">=", "between?",
          "clamp", NULL };
        static const char *const numeric_methods[] = { "+@", "abs2", "angle", "arg", "clone", "conj",
          "conjugate", "dup", "eql?", "finite?", "i", "imag", "imaginary", "infinite?", "negative?",
          "nonzero?", "phase", "polar", "positive?", "quo", "real", "real?", "rect", "rectangular",
          "step", "to_c", NULL };
        const char *bcls = builtin_class_of_type(rt);
        builtin_recv = 1;
        const char *bnames[512];
        int cap = 0; ANameHash seen; memset(&seen, 0, sizeof seen);
        /* the class's own rows, then what it inherits -- the methods every
           object answers and the modules it includes -- which a name the
           program computes reaches as well (`s.public_send("#{q}?")` with q
           "frozen", `a.public_send("#{q}_by")` with q "min") */
        int nb = builtin_method_names(bcls, bnames, 512);
        for (int k = 0; k < nb; k++) dsend_add_name(&own, &nown, &cap, &seen, bnames[k]);
        for (int k = 0; object_methods[k]; k++) dsend_add_name(&own, &nown, &cap, &seen, object_methods[k]);
        int is_enum = sp_streq(bcls, "Array") || sp_streq(bcls, "Hash") || sp_streq(bcls, "Range") ||
                      sp_streq(bcls, "File");
        int is_num = sp_streq(bcls, "Integer") || sp_streq(bcls, "Float");
        int is_cmp = is_num || sp_streq(bcls, "String") || sp_streq(bcls, "Symbol") || sp_streq(bcls, "Time");
        for (int k = 0; is_enum && enumerable_methods[k]; k++) dsend_add_name(&own, &nown, &cap, &seen, enumerable_methods[k]);
        for (int k = 0; is_cmp && comparable_methods[k]; k++) dsend_add_name(&own, &nown, &cap, &seen, comparable_methods[k]);
        for (int k = 0; is_num && numeric_methods[k]; k++) dsend_add_name(&own, &nown, &cap, &seen, numeric_methods[k]);
        /* and the program's own methods there: a reopen of the class
           (`class String; def shout? ...`), of Object, of Numeric */
        const char *pcls[] = { bcls, "Object", is_num ? "Numeric" : NULL };
        for (int r = 0; r < 3; r++) {
          int pci = pcls[r] ? comp_class_index(c, pcls[r]) : -1;
          if (pci < 0) continue;
          char **kn = NULL; int nk = dsend_receiver_names(c, pci, 0, &kn);
          for (int j = 0; j < nk; j++) { dsend_add_name(&own, &nown, &cap, &seen, kn[j]); free(kn[j]); }
          free(kn);
        }
        anh_free(&seen);
        if (nown == 0) continue;
      }
      else continue;
      /* An interpolated name fixes part of itself -- `"#{name}="` ends in
         "=" -- and only a name of that shape can be meant: on a receiver
         whose class is not known, the candidates are every class's methods,
         which in a program as large as activesupport passed the cap below
         and the send was refused (Deprecators#set_option). */
      { NodeKind ak = nt_kind(nt, argv[0]);
        int pn = 0; const int *ps = (ak == NK_InterpolatedStringNode || ak == NK_InterpolatedSymbolNode)
                                    ? nt_arr(nt, argv[0], "parts", &pn) : NULL;
        const char *pre = pn > 1 && nt_kind(nt, ps[0]) == NK_StringNode ? nt_str(nt, ps[0], "content") : NULL;
        const char *suf = pn > 1 && nt_kind(nt, ps[pn - 1]) == NK_StringNode ? nt_str(nt, ps[pn - 1], "content") : NULL;
        if ((pre && *pre) || (suf && *suf)) {
          size_t pl = pre ? strlen(pre) : 0, sl = suf ? strlen(suf) : 0;
          int kept = 0;
          for (int k = 0; k < nown; k++) {
            size_t ol = strlen(own[k]);
            if (ol >= pl + sl && (!pl || strncmp(own[k], pre, pl) == 0) &&
                (!sl || strcmp(own[k] + ol - sl, suf) == 0)) kept++;
          }
          /* none of that shape: the dispatch keeps its arms and raises
             NoMethodError for the name, as CRuby does */
          if (kept > 0) {
            int w = 0;
            for (int k = 0; k < nown; k++) {
              size_t ol = strlen(own[k]);
              if (ol >= pl + sl && (!pl || strncmp(own[k], pre, pl) == 0) &&
                  (!sl || strcmp(own[k] + ol - sl, suf) == 0)) own[w++] = own[k];
              else free(own[k]);
            }
            nown = w;
          }
        } }
      if (nown == 0 || nown > 1024) { for (int k = 0; k < nown; k++) free(own[k]); free(own); continue; }
      use = own; nuse = nown;
    }
    int rest[64]; for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    /* the send's block goes to whichever method the name selects: a
       forwarded `&blk` or the block written at the send (the arms are
       emitted one per branch, so one node serves them all) */
    int sblk = nt_ref(nt, id, "block");
    int base = nt->count;
    if (splat_src >= 0) {
      int r0 = nt_clone_subtree(nt, splat_src); if (r0 < 0) continue;
      int i0 = nt_new_node(nt, "IntegerNode"); if (i0 < 0) continue;
      nt_node_set_int(nt, i0, "value", 0);
      int a0n = nt_new_node(nt, "ArgumentsNode"); if (a0n < 0) continue;
      nt_node_set_arr(nt, a0n, "arguments", &i0, 1);
      int nmcall = nt_new_node(nt, "CallNode"); if (nmcall < 0) continue;
      nt_node_set_ref(nt, nmcall, "receiver", r0);
      nt_node_set_str(nt, nmcall, "name", "[]");
      nt_node_set_ref(nt, nmcall, "arguments", a0n);
      int sargs = nt_new_node(nt, "ArgumentsNode"); if (sargs < 0) continue;
      nt_node_set_arr(nt, sargs, "arguments", &nmcall, 1);
      nt_node_set_ref(nt, id, "arguments", sargs);           /* the name: args[0] */
      int r1 = nt_clone_subtree(nt, splat_src); if (r1 < 0) continue;
      int i1 = nt_new_node(nt, "IntegerNode"); if (i1 < 0) continue;
      nt_node_set_int(nt, i1, "value", 1);
      int a1n = nt_new_node(nt, "ArgumentsNode"); if (a1n < 0) continue;
      nt_node_set_arr(nt, a1n, "arguments", &i1, 1);
      int drop = nt_new_node(nt, "CallNode"); if (drop < 0) continue;
      nt_node_set_ref(nt, drop, "receiver", r1);
      nt_node_set_str(nt, drop, "name", "drop");
      nt_node_set_ref(nt, drop, "arguments", a1n);
      int sp = nt_new_node(nt, "SplatNode"); if (sp < 0) continue;
      nt_node_set_ref(nt, sp, "expression", drop);
      rest[0] = sp; nrest = 1;                                 /* the arms: *args.drop(1) */
    }
    /* a `*args` or `**kwargs` among the arguments makes the count a run-time
       matter (`send(method, *args, **kwargs, &block)`): the arity filter
       below would have read them as two positionals and dropped a
       one-parameter callee */
    int rest_variadic = 0;
    for (int k = 0; k < nrest; k++) {
      NodeKind rk = nt_kind(nt, rest[k]);
      if (rk == NK_SplatNode || rk == NK_KeywordHashNode) rest_variadic = 1;
    }
    int *arms = (int *)malloc(sizeof(int) * (size_t)(nuse > 0 ? nuse : 1)); int narm = 0;
    for (int k = 0; k < nuse; k++) {
      if (sp_streq(use[k], "initialize") || sp_streq(use[k], "initialize_copy")) continue;
      /* A method the program defines takes only the arities its definitions
         take: with a fixed argument list, a name no definition accepts at
         that count gets no arm (a reader takes none, a writer one). The
         fixpoint types such a call anyway, and its arm was emitted -- and
         one naming the enclosing method itself, which the block-carrying
         inline expands at the arm, expanded without end. */
      /* not for a builtin receiver's arms: another class's `empty?(x)` says
         nothing about String#empty? */
      if (splat_src < 0 && !rest_variadic && !builtin_recv && dsend_defined_arity_excludes(c, use[k], nrest)) continue;
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      if (nrest) nt_node_set_arr(nt, na, "arguments", rest, nrest);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      if (sblk >= 0) nt_node_set_ref(nt, call, "block", sblk);
      nt_node_set_str(nt, call, "name", use[k]);
      comp_sym_intern(c, use[k]);
      /* The dispatch keys each arm on the NAME it was built for, so a later
         desugar that rewrites the name (`first` -> `[]`) leaves the arm
         unreachable and the send raises. Mark them as owned. */
      nt_node_set_int(nt, call, "dyn_arm", 1);
      nt_node_set_str(nt, call, "dyn_name", use[k]);
      nt_node_set_ref(nt, call, "arguments", na);
      if (computed && nt_ref(nt, id, "block") >= 0) nt_node_set_ref(nt, call, "block", nt_ref(nt, id, "block"));
      /* public_send arms enforce visibility at the dispatch site */
      if (sp_streq(nm, "public_send")) nt_node_set_str(nt, call, "vis_enforce", "1");
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_send_arms", arms, narm);
    free(arms);
    if (computed) nt_node_set_int(nt, id, "dyn_send_complete", 1);
    for (int k = 0; k < nown; k++) free(own[k]);
    free(own);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    expand_static_splat_args(c, base, nt->count);
    changed = 1;
  }
  free(picked);
  free(lits);
  for (int k = 0; k < ncand; k++) free(cand[k]);
  free(cand);
  return changed;
}

/* `method(name).call(args)` with a NAME known only at run time is
   `send(name, args)`: both reach private methods, and the call is the only
   use of the Method. Rewritten so the runtime-name send lowering dispatches
   it (or refuses it where it cannot, a receiverless one at the top level);
   left alone, the call on an unresolved Method compiled to an unconditional
   NoMethodError raise (#6484). A program defining its own `method` keeps it. */
int desugar_method_call_runtime_name(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "method")) return 0;
  }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode || !sp_streq(nt_str(nt, id, "name"), "call")) continue;
    int m = nt_ref(nt, id, "receiver");
    if (m < 0 || nt_kind(nt, m) != NK_CallNode || !sp_streq(nt_str(nt, m, "name"), "method")) continue;
    if (nt_ref(nt, m, "block") >= 0) continue;
    int ma = nt_ref(nt, m, "arguments"), mac = 0;
    const int *mav = ma >= 0 ? nt_arr(nt, ma, "arguments", &mac) : NULL;
    if (mac != 1 || !mav) continue;
    NodeKind k0 = nt_kind(nt, mav[0]);
    if (k0 == NK_SymbolNode || k0 == NK_StringNode || k0 == NK_SplatNode) continue;
    int ca = nt_ref(nt, id, "arguments"), cac = 0;
    const int *cav = ca >= 0 ? nt_arr(nt, ca, "arguments", &cac) : NULL;
    int *nv = (int *)malloc(sizeof(int) * (size_t)(cac + 1));
    if (!nv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    nv[0] = mav[0];
    for (int i = 0; i < cac; i++) nv[i + 1] = cav[i];
    int na = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, na, "arguments", nv, cac + 1);
    free(nv);
    comp_grow_node_arrays(c);
    c->nscope[na] = c->nscope[id];
    nt_node_set_ref(nt, id, "receiver", nt_ref(nt, m, "receiver"));
    nt_node_set_str(nt, id, "name", "send");
    nt_node_set_ref(nt, id, "arguments", na);
    changed = 1;
  }
  return changed;
}

int desugar_dynamic_method(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode || !sp_streq(nt_str(nt, id, "name"), "method")) continue;
    int recv = nt_ref(nt, id, "receiver"), args = nt_ref(nt, id, "arguments"), argc = 0, dn = 0;
    if (args >= 0) nt_arr(nt, args, "arguments", &argc);
    nt_arr(nt, id, "dyn_send_arms", &dn);
    if (recv < 0 || argc != 1 || dn > 0 || method_sym_arg(c, id)) continue;
    TyKind rt = infer_type(c, recv);
    int cls = ty_is_object(rt) ? ty_object_class(rt) : -1, own_method = 0;
    if (cls >= 0) own_method = comp_method_in_chain(c, cls, "method", NULL) >= 0;
    if ((cls < 0 && rt != TY_POLY) || own_method) continue;
    char **own = NULL;
    int nown = dsend_receiver_names(c, cls, 1, &own), base = nt->count;
    int *arms = (int *)malloc(sizeof(int) * (size_t)(nown > 0 ? nown : 1));
    for (int k = 0; k < nown; k++) {
      int sym = nt_new_node(nt, "SymbolNode"), na = nt_new_node(nt, "ArgumentsNode");
      arms[k] = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, sym, "value", own[k]);
      nt_node_set_arr(nt, na, "arguments", &sym, 1);
      nt_node_set_ref(nt, arms[k], "receiver", recv);
      nt_node_set_str(nt, arms[k], "name", "method");
      nt_node_set_str(nt, arms[k], "dyn_name", own[k]);
      nt_node_set_int(nt, arms[k], "dyn_of", id);
      nt_node_set_ref(nt, arms[k], "arguments", na);
      comp_sym_intern(c, own[k]);
      free(own[k]);
    }
    nt_node_set_arr(nt, id, "dyn_send_arms", arms, nown);
    free(arms); free(own);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `recv.respond_to?(name)` with a NAME known only at run time (`respond_to?(
   args.first)` in activesupport's Object#try): the answer is decided at the
   dispatch, over the same closed set of names a runtime send resolves over,
   one synthesized `recv.respond_to?(:m)` arm per candidate for the literal
   fold to answer against the receiver's class (a boxed receiver's arms are
   its own runtime test). The arm ids are stashed under "dyn_rto_arms" and
   codegen emits `name == :m1 ? arm1 : ... : false`. A receiverless call stays
   receiverless: the fold resolves it against the enclosing class. A name in
   none of the arms answers false -- the set is the program's literals and
   definitions, so a builtin method named only at run time is the shape it
   does not cover. Before this a user object raised NoMethodError for
   respond_to? itself and the receiverless form was refused. */
int desugar_dynamic_respond_to(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  /* a user-defined respond_to? resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "respond_to?")) return 0; }
  int any = 0;
  for (int id = 0; id < n0 && !any; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;
    { int dn = 0; nt_arr(nt, id, "dyn_rto_arms", &dn); if (dn > 0) continue; }
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    int a = nt_ref(nt, id, "arguments"); if (a < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, a, "arguments", &ac);
    if (ac < 1 || !av) continue;
    NodeKind k0 = nt_kind(nt, av[0]);
    if (k0 == NK_SymbolNode || k0 == NK_StringNode) continue;
    any = 1;
  }
  if (!any) return 0;
  int ncand = 0;
  char **cand = dsend_candidates(c, &ncand);
  if (ncand == 0) { free(cand); return 0; }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;
    { int dn = 0; nt_arr(nt, id, "dyn_rto_arms", &dn); if (dn > 0) continue; }
    int recv = nt_ref(nt, id, "receiver");
    /* an explicit receiver takes the runtime name check at the call (the
       name is type-checked and matched against the receiver's methods);
       the receiverless form has no such emission, so it lowers here */
    if (recv >= 0) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || argc > 2 || !argv) continue;
    NodeKind k0 = nt_kind(nt, argv[0]);
    if (k0 == NK_SymbolNode || k0 == NK_StringNode || k0 == NK_SplatNode) continue;
    int extra = argc == 2 ? argv[1] : -1;                    /* include_all */
    int base = nt->count;
    int arms[256]; int narm = 0;
    for (int k = 0; k < ncand && narm < 256; k++) {
      int sym = nt_new_node(nt, "SymbolNode"); if (sym < 0) break;
      nt_node_set_str(nt, sym, "value", cand[k]);
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      int aa[2]; aa[0] = sym; int nn = 1;
      if (extra >= 0) aa[nn++] = extra;
      nt_node_set_arr(nt, na, "arguments", aa, nn);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", "respond_to?");
      nt_node_set_int(nt, call, "dyn_arm", 1);
      nt_node_set_ref(nt, call, "arguments", na);
      nt_node_set_ref(nt, call, "block", -1);
      comp_sym_intern(c, cand[k]);           /* the dispatch key, see desugar_dynamic_send */
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_rto_arms", arms, narm);
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
  /* a user-defined respond_to? resolves normally on its own class's
     instances; the other receivers keep their probes (one such definition --
     activesupport's TimeWithZone -- used to switch every probe in the program
     off, and a `respond_to?` on an exception or a String was then refused) */
  int user_rto = 0;
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "respond_to?")) { user_rto = 1; break; } }
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                         /* implicit self handled in the fold */
    if (user_rto) {
      TyKind rt = infer_type(c, recv);
      if (rt == TY_POLY || rt == TY_UNKNOWN) continue;
      if (ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), "respond_to?", NULL) >= 0) continue;
    }
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

/* A call `defined?` looks up on its receiver's value: an explicit receiver
   (`&.` and setters alike) and no literal block (an iterator answers
   "expression"). */
static int defined_method_call(const NodeTable *nt, int v) {
  return v >= 0 && nt_kind(nt, v) == NK_CallNode && nt_ref(nt, v, "receiver") >= 0 &&
         nt_kind(nt, nt_ref(nt, v, "block")) != NK_BlockNode;
}

/* `defined?(r.m(a))`: CRuby checks that r and a are defined, evaluates r,
   and answers "method" when its value has a public m. Each level of a chain
   `r.m1.m2` asks `respond_to?` of the value the level below produced, which a
   fresh local carries so every receiver is evaluated once, as CRuby does.
   The receiver and argument checks go on the node as "method_guards", the
   respond_to? conjunction as "method_cond"; codegen joins them. */
int desugar_defined_method_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefinedNode || nt_ref(nt, id, "method_cond") >= 0) continue;
    int v = nt_ref(nt, id, "value");
    if (!defined_method_call(nt, v)) continue;
    int lv[64], k = 0;
    for (int cur = v; defined_method_call(nt, cur) && k < 64; cur = nt_ref(nt, cur, "receiver")) lv[k++] = cur;
    int r0 = nt_ref(nt, lv[k - 1], "receiver");
    NodeKind rk = nt_kind(nt, r0);
    /* a receiver whose evaluation has no effect needs no local */
    int pure = rk == NK_LocalVariableReadNode || rk == NK_SelfNode || rk == NK_InstanceVariableReadNode ||
               rk == NK_ConstantReadNode || rk == NK_ConstantPathNode || rk == NK_NilNode ||
               rk == NK_TrueNode || rk == NK_FalseNode || rk == NK_IntegerNode || rk == NK_FloatNode ||
               rk == NK_SymbolNode || rk == NK_StringNode;
    long long line = nt_int(nt, id, "node_line", 0);
    int base = nt->count, gn = 0, guards[256], cond = -1, recv = r0;
    guards[gn++] = nt_new_node(nt, "DefinedNode");
    nt_node_set_ref(nt, guards[0], "value", r0);
    for (int i = k - 1; i >= 0; i--) {
      int args = nt_ref(nt, lv[i], "arguments"), ac = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      int blk = nt_ref(nt, lv[i], "block");
      for (int a = 0; a <= ac && gn < 256; a++) {
        int e = a < ac ? av[a] : blk >= 0 ? nt_ref(nt, blk, "expression") : -1;
        if (e >= 0 && nt_kind(nt, e) == NK_SplatNode) e = nt_ref(nt, e, "expression");
        if (e < 0) continue;
        guards[gn] = nt_new_node(nt, "DefinedNode");
        nt_node_set_ref(nt, guards[gn++], "value", e);
      }
      const char *m = nt_str(nt, lv[i], "name");
      int sym = nt_new_node(nt, "SymbolNode"), rargs = nt_new_node(nt, "ArgumentsNode");
      int rto = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, sym, "value", m);
      nt_node_set_arr(nt, rargs, "arguments", &sym, 1);
      nt_node_set_str(nt, rto, "name", "respond_to?");
      nt_node_set_ref(nt, rto, "arguments", rargs);
      nt_node_set_int(nt, rto, "node_line", line);
      comp_sym_intern(c, m);
      int ask = rto, next = -1;
      if (pure && i == k - 1) nt_node_set_ref(nt, rto, "receiver", recv);
      else {
        /* (__dfm_ID_i = recv; __dfm_ID_i.respond_to?(:m)) */
        char tname[64]; snprintf(tname, sizeof tname, "__dfm_%s_%d", comp_node_tag(c, id), i);
        int tw = nt_new_node(nt, "LocalVariableWriteNode"), tr = nt_new_node(nt, "LocalVariableReadNode");
        int st = nt_new_node(nt, "StatementsNode");
        ask = nt_new_node(nt, "ParenthesesNode");
        nt_node_set_str(nt, tw, "name", tname); nt_node_set_int(nt, tw, "depth", 0);
        nt_node_set_ref(nt, tw, "value", recv);
        nt_node_set_str(nt, tr, "name", tname); nt_node_set_int(nt, tr, "depth", 0);
        nt_node_set_ref(nt, rto, "receiver", tr);
        { int two[2] = { tw, rto }; nt_node_set_arr(nt, st, "body", two, 2); }
        nt_node_set_ref(nt, ask, "body", st);
        scope_local_intern(comp_scope_of(c, id), tname);
        if (i > 0) {
          int tr2 = nt_new_node(nt, "LocalVariableReadNode");
          nt_node_set_str(nt, tr2, "name", tname); nt_node_set_int(nt, tr2, "depth", 0);
          next = tr2;
        }
      }
      if (i > 0) {
        /* the next level's receiver: this level's call on the value just asked */
        int call = nt_new_node(nt, "CallNode");
        nt_node_set_ref(nt, call, "receiver", next >= 0 ? next : recv);
        nt_node_set_str(nt, call, "name", m);
        nt_node_set_str(nt, call, "call_operator", ".");
        nt_node_set_ref(nt, call, "arguments", args);
        nt_node_set_ref(nt, call, "block", blk);
        nt_node_set_int(nt, call, "node_line", line);
        recv = call;
      }
      if (cond < 0) cond = ask;
      else {
        int both = nt_new_node(nt, "AndNode");
        nt_node_set_ref(nt, both, "left", cond);
        nt_node_set_ref(nt, both, "right", ask);
        cond = both;
      }
    }
    if (!pure || k > 1) {
      /* an exception raised by an evaluated receiver answers nil */
      int rm = nt_new_node(nt, "RescueModifierNode"), no = nt_new_node(nt, "FalseNode");
      nt_node_set_ref(nt, rm, "expression", cond);
      nt_node_set_ref(nt, rm, "rescue_expression", no);
      cond = rm;
    }
    nt_node_set_arr(nt, id, "method_guards", guards, gn);
    nt_node_set_ref(nt, id, "method_cond", cond);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
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
    /* a receiver that turns out boxed may be a Hash or a String, which have #[] but
       no #at: the index read checks the receiver first */
    nt_node_set_int(nt, id, "was_at", 1);
    nt_node_set_str(nt, id, "name", "[]");
    changed = 1;
  }
  return changed;
}

/* Is `target` what `node` answers: the node itself, the last statement of a
   body, or the end of an if/unless branch or a parenthesized group? */
static int cow_in_tail(const NodeTable *nt, int node, int target) {
  if (node < 0) return 0;
  if (node == target) return 1;
  switch (nt_kind(nt, node)) {
  case NK_StatementsNode: {
    int n = 0;
    const int *st = nt_arr(nt, node, "body", &n);
    return st && n > 0 && cow_in_tail(nt, st[n - 1], target);
  }
  case NK_ParenthesesNode: return cow_in_tail(nt, nt_ref(nt, node, "body"), target);
  case NK_ElseNode: return cow_in_tail(nt, nt_ref(nt, node, "statements"), target);
  case NK_IfNode:
    return cow_in_tail(nt, nt_ref(nt, node, "statements"), target) ||
           cow_in_tail(nt, nt_ref(nt, node, "subsequent"), target);
  case NK_UnlessNode:
    return cow_in_tail(nt, nt_ref(nt, node, "statements"), target) ||
           cow_in_tail(nt, nt_ref(nt, node, "else_clause"), target);
  default: return 0;
  }
}

/* The op-assign is what its method returns: its value is used. */
static int cow_is_method_value(Compiler *c, int id) {
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].def_node >= 0 && cow_in_tail(c->nt, c->scopes[s].body, id)) return 1;
  return 0;
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
      if (comp_method_in_chain(c, k, wname, NULL) >= 0 || comp_method_in_chain(c, k, attr, NULL) >= 0) has_def_writer = 1;
    char aname[300]; snprintf(aname, sizeof aname, "%s", attr);
    /* attr_writer: keep the store, unless the method answers the op-assign's
       value, which the writer call carries and the store does not */
    if (!has_def_writer && !cow_is_method_value(c, id)) continue;
    char opname[64]; snprintf(opname, sizeof opname, "%s", op);
    if (!simple) {
      /* (__cow_N = recv; __cow_N.attr = __cow_N.attr op value) */
      char tname[48]; snprintf(tname, sizeof tname, "__cow_%s", comp_node_tag(c, id));
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

/* `recv.attr ||= value` / `&&=` where the receiver is a builtin a reopening
   gave the reader and the writer to -- a Thread with the accessor
   `Thread.attr_accessor` declared, activesupport's IsolatedExecutionState
   `@scope.current.active_support_execution_state ||= {}` -- has no ivar
   slot to store through and no user object for the reader/writer pairing,
   so it was refused. It is the two calls Ruby means, on the receiver
   evaluated once:
     (__cow_N = recv; __cow_N.attr || (__cow_N.attr = value))
   A user-object receiver keeps the emitter's own pairing; a receiver of no
   known kind keeps the poly path. */
int desugar_call_or_write_reopen(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_CallOrWriteNode && k != NK_CallAndWriteNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (recv < 0 || !attr || val < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (rt == TY_UNKNOWN || rt == TY_VOID || rt == TY_NIL || ty_is_object(rt)) continue;
    char wname[300];
    snprintf(wname, sizeof wname, "%s=", attr);
    /* both halves defined by a reopening of a builtin (a poly receiver
       counts only for those: a user object's pairing stays the emitter's) */
    int has_def_writer = 0, has_def_reader = 0;
    for (int q = 0; q < c->nclasses && !(has_def_writer && has_def_reader); q++) {
      if (!c->classes[q].name || !is_builtin_reopen(c->classes[q].name)) continue;
      if (comp_method_in_chain(c, q, wname, NULL) >= 0) has_def_writer = 1;
      if (comp_method_in_chain(c, q, attr, NULL) >= 0) has_def_reader = 1;
    }
    if (!has_def_writer || !has_def_reader) continue;
    char tname[48]; snprintf(tname, sizeof tname, "__cow_%s", comp_node_tag(c, id));
    int first = nt->count;
    int tw = nt_new_node(nt, "LocalVariableWriteNode");
    int tr1 = nt_new_node(nt, "LocalVariableReadNode");
    int tr2 = nt_new_node(nt, "LocalVariableReadNode");
    int rd = nt_new_node(nt, "CallNode");
    int wargs = nt_new_node(nt, "ArgumentsNode");
    int wc = nt_new_node(nt, "CallNode");
    int join = nt_new_node(nt, k == NK_CallOrWriteNode ? "OrNode" : "AndNode");
    int stmts = nt_new_node(nt, "StatementsNode");
    if (tw < 0 || tr1 < 0 || tr2 < 0 || rd < 0 || wargs < 0 || wc < 0 || join < 0 || stmts < 0) continue;
    long long line = nt_int(nt, id, "node_line", 0);
    nt_node_set_str(nt, tw, "name", tname); nt_node_set_int(nt, tw, "depth", 0);
    nt_node_set_ref(nt, tw, "value", recv);
    nt_node_set_str(nt, tr1, "name", tname); nt_node_set_int(nt, tr1, "depth", 0);
    nt_node_set_str(nt, tr2, "name", tname); nt_node_set_int(nt, tr2, "depth", 0);
    nt_node_set_ref(nt, rd, "receiver", tr1); nt_node_set_str(nt, rd, "name", attr);
    nt_node_set_ref(nt, rd, "arguments", -1); nt_node_set_ref(nt, rd, "block", -1);
    nt_node_set_int(nt, rd, "node_line", line);
    { int one[1]; one[0] = val; nt_node_set_arr(nt, wargs, "arguments", one, 1); }
    nt_node_set_ref(nt, wc, "receiver", tr2); nt_node_set_str(nt, wc, "name", wname);
    nt_node_set_ref(nt, wc, "arguments", wargs); nt_node_set_ref(nt, wc, "block", -1);
    nt_node_set_int(nt, wc, "node_line", line);
    nt_node_set_ref(nt, join, "left", rd); nt_node_set_ref(nt, join, "right", wc);
    { int two[2]; two[0] = tw; two[1] = join; nt_node_set_arr(nt, stmts, "body", two, 2); }
    nt_node_set_type(nt, id, "ParenthesesNode");
    nt_node_set_ref(nt, id, "body", stmts);
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "value", -1);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = first; j < nt->count; j++) c->nscope[j] = encl;
    scope_local_intern(comp_scope_of(c, tw), tname);
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
    if (!self_is_main(c, recv) || nt_int(nt, recv, "ie_self", 0)) continue;
    if (comp_method_index(c, name) < 0) continue;   /* no top-level def */
    nt_node_set_ref(nt, id, "receiver", -1);
    changed = 1;
  }
  return changed;
}

/* `class_variable_get(:@@x)`, `class_variable_set(:@@x, v)` and
   `class_variable_defined?(:@@x)`, bare or on `self`, in a class method of a
   class no other class inherits from: self is that class, so the call is the
   `Klass.class_variable_*` spelling the cvar reflection resolves. Bare, it
   was refused. */
int desugar_cmethod_cvar_reflection(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name || (!sp_streq(name, "class_variable_get") && !sp_streq(name, "class_variable_set") &&
                  !sp_streq(name, "class_variable_defined?"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv >= 0 && (nt_kind(nt, recv) != NK_SelfNode || nt_int(nt, recv, "ie_self", 0))) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 1 || nt_kind(nt, av[0]) != NK_SymbolNode) continue;
    const char *cvn = nt_str(nt, av[0], "value");
    if (!cvn || cvn[0] != '@' || cvn[1] != '@') continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || !s->is_cmethod || s->class_id < 0 || ie_class_of(c, id) != -1) continue;
    int cid = s->class_id;
    if (comp_cmethod_in_chain(c, cid, name, NULL) >= 0) continue;
    int inherited = 0;
    for (int k = 0; k < c->nclasses && !inherited; k++) inherited = c->classes[k].parent == cid;
    if (inherited) continue;
    int cr = nt_new_node(nt, "ConstantReadNode");
    if (cr < 0) continue;
    nt_node_set_str(nt, cr, "name", c->classes[cid].name);
    nt_node_set_ref(nt, id, "receiver", cr);
    comp_grow_node_arrays(c);
    c->nscope[cr] = c->nscope[id];
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
    int ci = ty_is_object(rt) ? ty_object_class(rt) : -1;
    if (rt != TY_THREAD && (ci < 0 || comp_method_in_chain(c, ci, "[]", NULL) < 0 || comp_method_in_chain(c, ci, "[]=", NULL) < 0)) continue;
    const char *op = k == NK_IndexOperatorWriteNode ? nt_str(nt, id, "binary_operator") : NULL;
    if (k == NK_IndexOperatorWriteNode && !op) continue;
    char opname[64]; if (op) snprintf(opname, sizeof opname, "%s", op);
    int key = argv[0];
    char rname[48], kname[48];
    snprintf(rname, sizeof rname, "__ixr_%s", comp_node_tag(c, id));
    snprintf(kname, sizeof kname, "__ixk_%s", comp_node_tag(c, id));
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
    snprintf(tpname, sizeof tpname, "__tproc_%s", comp_node_tag(c, id));
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
    snprintf(bname, sizeof bname, "__blkexpr_%s", comp_node_tag(c, id));
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

/* `[*h]`, `x = *h`, `f(*h)` with a Hash, a Struct, or an object defining #to_a:
   a splat converts its operand through #to_a, so a Hash spreads its [k, v]
   pairs rather than landing as one element. Rewrite the operand to
   `h.to_a` so the array splat paths see an array. */
int desugar_splat_to_a(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_SplatNode) continue;
    int val = nt_ref(nt, id, "expression");
    if (val < 0) continue;
    const char *vty = nt_type(nt, val);
    if (!vty || strstr(vty, "TargetNode")) continue;
    if (sp_streq(vty, "CallNode") && nt_str(nt, val, "name") &&
        sp_streq(nt_str(nt, val, "name"), "to_a")) continue;
    TyKind t = infer_type(c, val);
    if (!ty_is_hash(t) && !sp_streq(vty, "HashNode")) {
      if (!ty_is_object(t)) continue;
      int cid = ty_object_class(t);
      if (cid < 0) continue;
      int st = 0;
      for (int k = cid; k >= 0 && !st; k = c->classes[k].parent) st = c->classes[k].is_struct;
      if (!st && comp_method_in_chain(c, cid, "to_a", NULL) < 0) continue;
    }
    int base = nt->count;
    int call = nt_new_node(nt, "CallNode");
    if (call < 0) continue;
    nt_node_set_ref(nt, call, "receiver", val);
    nt_node_set_str(nt, call, "name", "to_a");
    nt_node_set_ref(nt, call, "arguments", -1);
    nt_node_set_ref(nt, call, "block", -1);
    nt_node_set_ref(nt, id, "expression", call);
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
  return str_in(name, names);
}

/* Hash's own select, filter, reject and to_h yield the key and the value as
   two values, where `each` and the Enumerable iterators yield the [k, v] pair
   as one: a proc taking |x| gets the key alone, and a lambda or Method of two
   parameters takes both. */
static int hash_two_value_iter(const char *name) {
  return sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "reject") || sp_streq(name, "to_h");
}

/* Over an Enumerator that yields two values, these pass both on to their
   block as two; the other block iterators pack them into one Array. */
static int enum_pair_spread_iter(const char *name) {
  static const char *const names[] = {
    "map", "collect", "flat_map", "collect_concat", "filter_map", "count", "take_while",
    "find_index", "any?", "all?", "none?", "one?", "each", "uniq", NULL };
  return str_in(name, names);
}

/* `e.with_index(off) { }` / `e.with_object(memo) { }`: the element and the
   index or memo, as two values. */
static int fwd_with_index_call(const NodeTable *nt, int id) {
  const char *nm = nt_str(nt, id, "name");
  int a = nt_ref(nt, id, "arguments");
  int n = 0; if (a >= 0) nt_arr(nt, a, "arguments", &n);
  return nm && ((sp_streq(nm, "with_index") && n <= 1) || (sp_streq(nm, "with_object") && n == 1));
}

int enum_pair_source_call(const NodeTable *nt, int recv) {
  if (recv < 0 || nt_kind(nt, recv) != NK_CallNode || nt_ref(nt, recv, "block") >= 0) return 0;
  const char *rn = nt_str(nt, recv, "name");
  /* the to_a an Enumerator's block call is routed through (enum_hop) */
  if (rn && sp_streq(rn, "to_a") && nt_str(nt, recv, "enum_hop"))
    return enum_pair_source_call(nt, nt_ref(nt, recv, "receiver"));
  int ra = nt_ref(nt, recv, "arguments");
  int rc = 0; if (ra >= 0) nt_arr(nt, ra, "arguments", &rc);
  if (!rn) return 0;
  return (sp_streq(rn, "each_with_index") && rc == 0) ||
         (sp_streq(rn, "with_index") && rc <= 1) ||
         ((sp_streq(rn, "each_with_object") || sp_streq(rn, "with_object")) && rc == 1);
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

/* `recv.each(&callable)` whose callable is not a plain read (`h.map(&a[0])`,
   `h.map(&mk)`): the forward re-reads the callable per element, which an
   expression that calls something cannot be, so the forward declined and the
   call raised NoMethodError at run time. The callable goes into a temp the
   forward reads, assigned in the receiver's place, `(t = callable;
   recv).each(&t)`, so it runs once. Ahead of the receiver is its source
   position whenever nothing before it can act: desugar_block_arg_order has
   already put every operand of a call where something can into temps of its
   own, in source order, the callable a plain read among them. *ex becomes a
   read of the temp. */
static void fwd_hoist_callable(Compiler *c, int id, int blk, int *ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  char tn[48];
  snprintf(tn, sizeof tn, "__fwdc_%s", comp_node_tag(c, id));
  int w = nt_new_node(nt, "LocalVariableWriteNode");
  nt_node_set_str(nt, w, "name", tn);
  nt_node_set_ref(nt, w, "value", *ex);
  scope_local_intern(comp_scope_of(c, id), tn);
  int r = nt_new_node(nt, "LocalVariableReadNode");
  nt_node_set_str(nt, r, "name", tn);
  nt_node_set_ref(nt, blk, "expression", r);
  *ex = r;
  /* a `to_a` the call is routed through (enum_hop, enum_recv) stays the
     call's receiver, and a call answering its receiving Enumerator answers
     the parentheses now (enum_self_result) */
  int owner = id, recv = nt_ref(nt, id, "receiver");
  while (nt_kind(nt, recv) == NK_CallNode &&
         (nt_str(nt, recv, "enum_hop") || nt_str(nt, recv, "enum_recv"))) {
    owner = recv;
    recv = nt_ref(nt, recv, "receiver");
  }
  int stmts[2] = { w, recv };
  int body = nt_new_node(nt, "StatementsNode");
  nt_node_set_arr(nt, body, "body", stmts, 2);
  int paren = nt_new_node(nt, "ParenthesesNode");
  nt_node_set_ref(nt, paren, "body", body);
  nt_node_set_ref(nt, owner, "receiver", paren);
  if (nt_int(nt, id, "enum_self_result", -1) == recv) nt_node_set_int(nt, id, "enum_self_result", paren);
}

/* `recv.name(arg)` (no argument for -1), for the forwards built below */
static int fwd_new_call(NodeTable *nt, int recv, const char *name, int arg) {
  int call = nt_new_node(nt, "CallNode");
  nt_node_set_ref(nt, call, "receiver", recv);
  nt_node_set_str(nt, call, "name", name);
  int args = -1;
  if (arg >= 0) {
    args = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, args, "arguments", &arg, 1);
  }
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}
static int fwd_new_int(NodeTable *nt, int v) {
  int n = nt_new_node(nt, "IntegerNode");
  nt_node_set_int(nt, n, "value", v);
  return n;
}

/* The call a Hash iterator's forward makes to a callable whose parameters it
   cannot see, `pair` handing it the [k, v] pair: map spreads the pair for more
   than one required parameter, `q.arity >= 2 || q.arity < -2 ? q.call(k, v) :
   pair`, and find for a Proc (a lambda included) that would auto-splat it,
   `q.is_a?(Proc) && (q.arity >= 2 || q.arity < -1) ? ...`. The Proc test is
   left out where the callable is known to be one. */
static int fwd_arity_pick(Compiler *c, NodeTable *nt, int ex, int id, int pair, int find, int proc_test) {
  int ge = fwd_new_call(nt, fwd_new_call(nt, nt_clone_subtree(nt, ex), "arity", -1), ">=", fwd_new_int(nt, 2));
  int lt = fwd_new_call(nt, fwd_new_call(nt, nt_clone_subtree(nt, ex), "arity", -1), "<",
                        fwd_new_int(nt, find ? -1 : -2));
  int cond = nt_new_node(nt, "OrNode");
  nt_node_set_ref(nt, cond, "left", ge);
  nt_node_set_ref(nt, cond, "right", lt);
  if (proc_test) {
    int pc = nt_new_node(nt, "ConstantReadNode");
    nt_node_set_str(nt, pc, "name", "Proc");
    int both = nt_new_node(nt, "AndNode");
    nt_node_set_ref(nt, both, "left", fwd_new_call(nt, nt_clone_subtree(nt, ex), "is_a?", pc));
    nt_node_set_ref(nt, both, "right", cond);
    cond = both;
  }
  int kv[2];
  char pn[64];
  for (int k = 0; k < 2; k++) {
    snprintf(pn, sizeof pn, "__fwd_%s_%d", comp_node_tag(c, id), k);
    kv[k] = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, kv[k], "name", pn);
  }
  int twoargs = nt_new_node(nt, "ArgumentsNode");
  nt_node_set_arr(nt, twoargs, "arguments", kv, 2);
  int two = fwd_new_call(nt, nt_clone_subtree(nt, ex), "call", -1);
  nt_node_set_ref(nt, two, "arguments", twoargs);
  int then = nt_new_node(nt, "StatementsNode");
  nt_node_set_arr(nt, then, "body", &two, 1);
  int other = nt_new_node(nt, "StatementsNode");
  nt_node_set_arr(nt, other, "body", &pair, 1);
  int els = nt_new_node(nt, "ElseNode");
  nt_node_set_ref(nt, els, "statements", other);
  int pick = nt_new_node(nt, "IfNode");
  nt_node_set_ref(nt, pick, "predicate", cond);
  nt_node_set_ref(nt, pick, "statements", then);
  nt_node_set_ref(nt, pick, "subsequent", els);
  return pick;
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
    int hoist = 0;
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
    /* any other expression is evaluated once, into a temp the forward
       re-reads (fwd_hoist_callable) */
    hoist = !simple_ref && !method_obj && !inline_lambda;
    ct = infer_type(c, ex);
    /* A poly local can hold a callable produced by an operation whose static
       type stays poly -- e.g. `procs.reduce(:>>)`, a composed Proc. Forward it
       as a value callable too (its `.call` dispatches at runtime) (#3167). */
    if (ct != TY_PROC && ct != TY_METHOD && ct != TY_POLY) continue;
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
    else if ((enum_pair_source_call(nt, recv) && enum_pair_spread_iter(name)) ||
             fwd_with_index_call(nt, id)) {
      /* `a.each_with_index.map(&)`, `a.map.with_index(&)`: two values are
         yielded, and passed on as two (a lone `*r` takes both spread) */
      arity = 2;
      pty[0] = pty[1] = TY_UNKNOWN;
    }
    else if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode && sp_streq(name, "new") &&
             nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Array")) {
      /* Array.new(n, &blk): the block takes each index. Left in its &-form,
         the builder's loop has no block body to run and the array came out
         empty. */
      arity = 1;
      pty[0] = TY_INT;
    }
    else {
      arity = ty_block_yield(rt, name, pty, 4);
      if (arity < 1) continue;  /* not a context-free iterator (or recv unresolved) */
    }

    /* Hash forwarding. Hash's own select, filter, reject and to_h yield the
       key and the value as two, and they go on as two to whatever takes them.
       The other iterators yield the [k, v] pair. A Method object takes it
       through its array ABI (the pair as one array for `each`, the bare
       key/value for `each_key`/`each_value`). */
    int wrap_pair = 0;
    int spread = 0, find_proc_test = 0;   /* spread -1: chosen at run time */
    if (ty_is_hash(rt) && hash_two_value_iter(name)) wrap_pair = 0;
    else if (ty_is_hash(rt) && anon) {
      /* a yield hands the pair as one array, which the caller's block
         auto-splats into |k, v| or takes whole as |pair|, as Hash#each does */
      wrap_pair = (arity == 2);
    }
    else if (ty_is_hash(rt) && arity == 2) {
      /* The pair as one array, or the key and the value as two, as CRuby
         hands them. A Proc auto-splats the pair where it runs, so for one
         taking |k, v| the two are the same; the call-site inference types a
         visible Proc's params better from two. A lambda or a Method takes
         the pair strictly, and raises for a second required parameter,
         except through map, whose block of more than one required parameter
         takes the key and the value (a Method's too), and find, which
         treats a lambda as a Proc and a Method strictly. A callable whose
         parameters are not visible here -- one boxed in a poly slot, or a
         Proc a method returned -- gets the pair, or has map and find ask
         its arity at run time. Declined for want of a static arity, `h.map
         (&q)` stayed in its &-form, and the call raised NoMethodError at
         run time. */
      int is_map = sp_streq(name, "map") || sp_streq(name, "collect");
      int is_find = sp_streq(name, "find") || sp_streq(name, "detect");
      FwdShape sh = { 0, 0 };
      int cpc = fwd_callable_arity(c, ex, &sh);
      if (cpc < 0) {
        spread = (is_map || (is_find && ct != TY_METHOD)) ? -1 : 0;
        find_proc_test = is_find && ct == TY_POLY;
      }
      else if (!sh.strict) spread = cpc == 2;
      else if (is_map) spread = sh.arity >= 2 || sh.arity < -2;
      else if (is_find) spread = ct != TY_METHOD && (sh.arity >= 2 || sh.arity < -1);
      else spread = 0;
      wrap_pair = spread != 1;
    }
    else if (ty_is_hash(rt)) wrap_pair = 0;   /* each_key/each_value: the bare key/value */

    int base = nt->count;
    if (hoist) fwd_hoist_callable(c, id, blk, &ex);
    int proc_clone = anon ? -1 : nt_clone_subtree(nt, ex);  /* re-read the proc per element */
    if (!anon && proc_clone < 0) continue;

    int reqs[4], reads[4];
    char pn[64];
    int alloc_ok = 1;
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%s_%d", comp_node_tag(c, id), k);
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
      callnode = fwd_new_call(nt, proc_clone, "call", -1);
      nt_node_set_ref(nt, callnode, "arguments", callargs);
    }
    if (spread < 0 && callnode >= 0)
      callnode = fwd_arity_pick(c, nt, ex, id, callnode, sp_streq(name, "find") || sp_streq(name, "detect"),
                                find_proc_test);

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
      snprintf(pn, sizeof pn, "__fwd_%s_%d", comp_node_tag(c, id), k);
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
    char nm[64]; snprintf(nm, sizeof nm, "__implicit_rest_%s", comp_node_tag(c, id));
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

/* `return a, *b, c` / `break a, *b` / `next a, b` hand back one array:
   CRuby reads them as `return [a, *b, c]`. Wrap the arguments in that
   ArrayNode so the array-literal builders splice the splat and every
   value consumer sees one argument. The per-jump builders pushed each
   argument boxed, a splat as one nested array, and `next` kept only the
   first argument. A splat-free `return` / `break` keeps its own path.
   `yield a, *b` and `blk.call(a, *b)` become `yield(*[a, *b])`, which the
   block binder already spreads; it bound each argument to one parameter,
   the splat's whole array included. */
int desugar_multi_value_jump(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    int is_call = k == NK_CallNode && nt_ref(nt, id, "receiver") >= 0 &&
                  nt_ref(nt, id, "block") < 0 && sp_streq(nt_str(nt, id, "name"), "call");
    if (k != NK_ReturnNode && k != NK_BreakNode && k != NK_NextNode && k != NK_YieldNode && !is_call) continue;
    int args = nt_ref(nt, id, "arguments");
    int n = 0; const int *a = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
    if (!a || n < 2) continue;
    int splat = 0, other = 0;
    for (int j = 0; j < n; j++) {
      NodeKind ak = nt_kind(nt, a[j]);
      if (ak == NK_SplatNode && nt_ref(nt, a[j], "expression") < 0) other = 1;
      else if (ak == NK_SplatNode) splat = 1;
      else if (ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode) other = 1;
    }
    if (other || (!splat && k != NK_NextNode)) continue;
    int arr = nt_new_node(nt, "ArrayNode");
    int wrap = (k == NK_YieldNode || is_call) ? nt_new_node(nt, "SplatNode") : arr;
    if (arr < 0 || wrap < 0) continue;
    int lk[2] = { arr, wrap };
    for (int j = 0; j < 2; j++) {
      nt_node_set_int(nt, lk[j], "node_line", nt_int(nt, id, "node_line", 0));
      nt_node_set_int(nt, lk[j], "node_file", nt_int(nt, id, "node_file", 0));
      nt_node_set_int(nt, lk[j], "node_col", nt_int(nt, id, "node_col", 0));
    }
    nt_node_set_arr(nt, arr, "elements", a, n);
    if (wrap != arr) nt_node_set_ref(nt, wrap, "expression", arr);
    nt_node_set_arr(nt, args, "arguments", &wrap, 1);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  return changed;
}

static const char *sym_or_str_literal(const NodeTable *nt, int node);

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

static int rsc_shadowed(char **names, int nn, const char *nm) {
  for (int k = 0; k < nn; k++) if (sp_streq(nm, names[k])) return 1;
  return 0;
}

/* Errno::EWOULDBLOCK -> Errno::EAGAIN where the two share a number (and
   EOPNOTSUPP -> ENOTSUP, EDEADLOCK -> EDEADLK): CRuby makes the later name a
   constant for the earlier class, so they are one class -- equal, rescued by
   either name, named the first way (errno_canonical_name). Renaming the path
   once, before anything reads it, gives every later stage one spelling. */
int desugar_errno_aliases(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  NT_FOREACH_KIND(nt, NK_ConstantPathNode, id) {
    int par = nt_ref(nt, id, "parent");
    const char *leaf = nt_str(nt, id, "name");
    if (par < 0 || !leaf || nt_kind(nt, par) != NK_ConstantReadNode) continue;
    const char *pn = nt_str(nt, par, "name");
    if (!pn || !sp_streq(pn, "Errno")) continue;
    char q[96];
    snprintf(q, sizeof q, "Errno::%s", leaf);
    const char *cn = errno_canonical_name(q);
    if (cn == q) continue;
    nt_node_set_str(nt, id, "name", cn + 7);
    changed = 1;
  }
  return changed;
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
  for (int id = n0 - 1; id >= 0; id--) {
    if (skip[id] || nt_kind(nt, id) != NK_ConstantPathNode) continue;
    int par = nt_ref(nt, id, "parent");
    if (par >= 0 && !engine_const(nt, par, "Object")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    if (rsc_shadowed(names, nn, nm)) continue;
    nt_node_set_type(nt, id, "ConstantReadNode");
    nt_node_set_ref(nt, id, "parent", -1);
    changed = 1;
  }
  for (int id = 0; id < n0; id++) {
    const char *cm = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
    int args = cm && sp_streq(cm, "const_get") ? nt_ref(nt, id, "arguments") : -1;
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    const char *cn = an == 1 ? sym_or_str_literal(nt, av[0]) : NULL;
    int rv = nt_ref(nt, id, "receiver");
    const char *rn = nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
    if (!cn || !comp_is_wellknown_const(cn) || !builtin_class_id(rn) || sp_streq(rn, "BasicObject")) continue;
    nt_node_set_type(nt, id, rsc_shadowed(names, nn, cn) ? "ConstantPathNode" : "ConstantReadNode");
    nt_node_set_str(nt, id, "name", cn);
    nt_node_set_ref(nt, id, "receiver", -1); nt_node_set_ref(nt, id, "arguments", -1);
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
      snprintf(vn, sizeof vn, "__wi_v_%s", comp_node_tag(c, id));
      snprintf(inm, sizeof inm, "__wi_i_%s", comp_node_tag(c, id));
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
    char pn[48]; snprintf(pn, sizeof pn, "__wi_pair_%s", comp_node_tag(c, id));
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
      int rn7 = an_unparen(nt, recv);
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

/* `def m(a, ...) = callee(a, ...)` becomes `def m(a, *, **, &) =
   callee(a, *, **, &)`, and so do `super(...)`, a bare `super` and
   `new(...)`. The __fwd_N model (#1288) binds the callee's params
   positionally: it flattens a rest/kwrest, fills an argument the caller
   left out with nil where the callee has a default, and carries no block.
   `*` / `**` follow the callee's params, and `&` is added when the callee
   takes a block or a caller passes one. */
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
/* Walked through the references, not swept as an id range: a forwarder this
   pass has rewritten holds its new `*` / `**` parameters at the end of the
   table, and the ids in between belong to other methods (whose `yield`
   would make it look block-taking). */
static int fwd_subtree_yields(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return 0;
  if (fwd_node_is(nt, id, "YieldNode")) return 1;
  if (fwd_node_is(nt, id, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, "block_given?")) return 1;
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) if (fwd_subtree_yields(nt, nd->r[j].ref)) return 1;
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) if (fwd_subtree_yields(nt, nd->a[j].ids[k])) return 1;
  return 0;
}
static int fwd_subtree_uses_yield_or_block(const NodeTable *nt, int def) {
  int pn = nt_ref(nt, def, "parameters");
  if (pn >= 0 && nt_ref(nt, pn, "block") >= 0) return 1;
  return fwd_subtree_yields(nt, def);
}
static int fwd_any_def_yields(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++)
    if (fwd_node_is(nt, id, "DefNode") && nt_str(nt, id, "name") &&
        sp_streq(nt_str(nt, id, "name"), name) && fwd_subtree_yields(nt, id)) return 1;
  return 0;
}
/* bit 1 = positional forwarding, bit 2 = keyword forwarding, bit 4 =
   block/yield; -1 no def, -2 defs disagree. Fixed parameters count too:
   forwarding to `def f(*a, k: 0)` needs both channels, as does forwarding to
   `def f(x, **k)`. A `def m(...)` forwarder is -1: it takes whatever shape
   its own target has. */
static int def_shape(const NodeTable *nt, int id) {
  int pn = nt_ref(nt, id, "parameters");
  if (pn >= 0 && fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "ForwardingParameterNode")) return -1;
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
    /* A `**nil` method takes no keyword, but it has to see the keywords a
       forward passes to refuse them: without the `**` channel `...` carried
       them into the rest as a positional Hash, so `def w(...) = m(...)`
       into `def m(a, **nil)` raised a wrong count for `w(1, z: 3)`, and
       bound the Hash into an optional where m had one, instead of CRuby's
       "no keywords accepted". An empty `**` passes nothing to refuse. */
    if (fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "NoKeywordsParameterNode")) sh |= 2;
  }
  if (fwd_subtree_uses_yield_or_block(nt, id)) sh |= 4;
  return sh;
}
static int def_shape_by_name(const NodeTable *nt, const char *name) {
  int shape = -1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int sh = def_shape(nt, id);
    if (sh < 0) continue;
    if (shape >= 0 && shape != sh) return -2;
    shape = sh;
  }
  return shape;
}
/* The channels any def of `name` takes, together; -1 when none says. A
   receiver whose class is not known here may be any of them, and the
   anonymous `*, **, &` passes each exactly what the call had: an empty
   splat and an empty `**` pass nothing to a def that takes neither. */
static int def_shape_union_by_name(const NodeTable *nt, const char *name) {
  int shape = -1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int sh = def_shape(nt, id);
    if (sh < 0) continue;
    shape = shape < 0 ? sh : (shape | sh);
  }
  return shape;
}
static int fwd_any_def_named(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++)
    if (fwd_node_is(nt, id, "DefNode") && nt_str(nt, id, "name") &&
        sp_streq(nt_str(nt, id, "name"), name)) return 1;
  return 0;
}
static void fwd_key_add(char *key, size_t cap, const char *seg) {
  size_t n = strlen(key);
  if (n < cap) snprintf(key + n, cap - n, "%s::", seg ? seg : "");
}
static void fwd_path_key(const NodeTable *nt, int path, char *key, size_t cap) {
  if (!fwd_node_is(nt, path, "ConstantPathNode")) return;
  int par = nt_ref(nt, path, "parent");
  fwd_path_key(nt, par, key, cap);
  fwd_key_add(key, cap, par >= 0 ? nt_str(nt, par, "name") : NULL);
}
/* A body statement as the def it declares: the statement itself, or the
   one argument of a receiverless `private def m ...` (also protected, public
   and module_function), which declares the same method. -1 otherwise. */
static int fwd_body_def(const NodeTable *nt, int st) {
  if (fwd_node_is(nt, st, "DefNode")) return st;
  if (!fwd_node_is(nt, st, "CallNode") || nt_ref(nt, st, "receiver") >= 0) return -1;
  const char *nm = nt_str(nt, st, "name");
  if (!nm || !(sp_streq(nm, "private") || sp_streq(nm, "protected") ||
               sp_streq(nm, "public") || sp_streq(nm, "module_function"))) return -1;
  int an = 0; const int *av = nt_arr(nt, nt_ref(nt, st, "arguments"), "arguments", &an);
  return an == 1 && fwd_node_is(nt, av[0], "DefNode") ? av[0] : -1;
}
/* Does the body of class or module `ct` hold `id` as one of its statements,
   or as a def a visibility call wraps? */
static int fwd_body_holds(const NodeTable *nt, int ct, int id) {
  int bn = 0; const int *bv = nt_arr(nt, nt_ref(nt, ct, "body"), "body", &bn);
  for (int k = 0; k < bn; k++)
    if (bv[k] == id || fwd_body_def(nt, bv[k]) == id) return 1;
  return 0;
}
static void fwd_ns_key(const NodeTable *nt, int id, char *key, size_t cap);
/* The lexical namespace a node sits in: its enclosing classes and modules. */
static void fwd_lex_ctx(const NodeTable *nt, int id, char *key, size_t cap) {
  for (int ct = 0; ct < id; ct++) {
    if (!fwd_node_is(nt, ct, "ClassNode") && !fwd_node_is(nt, ct, "ModuleNode")) continue;
    if (!fwd_body_holds(nt, ct, id)) continue;
    fwd_ns_key(nt, ct, key, cap);
    fwd_key_add(key, cap, nt_str(nt, nt_ref(nt, ct, "constant_path"), "name"));
    break;
  }
}
/* The namespace a class or module node is opened in: its lexical namespace
   and the qualifier of its own path. */
static void fwd_ns_key(const NodeTable *nt, int id, char *key, size_t cap) {
  fwd_lex_ctx(nt, id, key, cap);
  fwd_path_key(nt, nt_ref(nt, id, "constant_path"), key, cap);
}
/* The namespace key of the class `ref` (a constant node, or `refname` when
   ref < 0) names when read in namespace `ctx`: looked up, with any
   qualifier of its path, in ctx and then each enclosing namespace out to the
   top. A name the lexical walk does not reach (a class an enclosing module
   includes, `module N; include Lib; class C < Base`) is the one class of that
   name when there is exactly one. 0 when no class is found. */
static int fwd_resolve_class(const NodeTable *nt, const char *ctx, int ref, const char *refname,
                             char *out, size_t cap) {
  const char *cls = ref >= 0 ? nt_str(nt, ref, "name") : refname;
  if (!cls) return 0;
  if (ref >= 0 && !fwd_node_is(nt, ref, "ConstantReadNode") &&
      !fwd_node_is(nt, ref, "ConstantPathNode")) return 0;
  char qual[512] = "", prefix[512], cand[1024], key[512];
  fwd_path_key(nt, ref, qual, sizeof qual);
  snprintf(prefix, sizeof prefix, "%s", ctx);
  for (;;) {
    snprintf(cand, sizeof cand, "%s%s", prefix, qual);
    for (int id = 0; id < nt->count; id++) {
      if (!fwd_node_is(nt, id, "ClassNode")) continue;
      const char *cn = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
      if (!cn || !sp_streq(cn, cls)) continue;
      key[0] = '\0';
      fwd_ns_key(nt, id, key, sizeof key);
      if (sp_streq(key, cand)) { snprintf(out, cap, "%s", key); return 1; }
    }
    if (!prefix[0]) break;
    size_t n = strlen(prefix) - 2;
    while (n >= 2 && !(prefix[n - 1] == ':' && prefix[n - 2] == ':')) n--;
    prefix[n >= 2 ? n : 0] = '\0';
  }
  int only = -1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "ClassNode")) continue;
    const char *cn = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
    if (!cn || !sp_streq(cn, cls)) continue;
    key[0] = '\0';
    fwd_ns_key(nt, id, key, sizeof key);
    if (only >= 0 && !sp_streq(out, key)) return 0;   /* two classes of that name */
    only = id;
    snprintf(out, cap, "%s", key);
  }
  return only >= 0;
}
/* The shape of instance method `name` as the class `ref` names from
   namespace `ctx` (or its nearest superclass defining it) has it. Classes
   are matched by namespace and name, before any scope exists. */
static int fwd_class_method_shape(const NodeTable *nt, const char *ctx, int ref,
                                  const char *refname, const char *name) {
  char at[512], key[512];
  snprintf(at, sizeof at, "%s", ctx);
  for (int depth = 0; depth < 16; depth++) {
    char found[512];
    if (!fwd_resolve_class(nt, at, ref, refname, found, sizeof found)) return -1;
    const char *cls = ref >= 0 ? nt_str(nt, ref, "name") : refname;
    int shape = -1, next = -1, next_cls = -1;
    for (int id = 0; id < nt->count; id++) {
      if (!fwd_node_is(nt, id, "ClassNode")) continue;
      const char *cn = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
      if (!cn || !sp_streq(cn, cls)) continue;
      key[0] = '\0';
      fwd_ns_key(nt, id, key, sizeof key);
      if (!sp_streq(key, found)) continue;
      if (next < 0 && nt_ref(nt, id, "superclass") >= 0) { next = nt_ref(nt, id, "superclass"); next_cls = id; }
      int bn = 0; const int *bv = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
      for (int k = 0; k < bn; k++) {
        int dk = fwd_body_def(nt, bv[k]);
        if (dk < 0 || nt_ref(nt, dk, "receiver") >= 0) continue;
        const char *dn = nt_str(nt, dk, "name");
        if (!dn || !sp_streq(dn, name)) continue;
        int sh = def_shape(nt, dk);
        if (sh < 0 || (shape >= 0 && sh != shape)) return -2;
        shape = sh;
      }
    }
    if (shape >= 0) return shape;
    if (next < 0) return -1;
    at[0] = '\0';
    fwd_lex_ctx(nt, next_cls, at, sizeof at);
    ref = next; refname = NULL;
  }
  return -1;
}
/* The ClassNode whose body holds `def` directly or under a visibility call,
   or -1. */
static int fwd_enclosing_class(const NodeTable *nt, int def) {
  for (int id = 0; id < def; id++)
    if (fwd_node_is(nt, id, "ClassNode") && fwd_body_holds(nt, id, def)) return id;
  return -1;
}
/* The shape `k.new(...)` reaches when `k` is a class held in a value: the
   one every initialize has, or else every channel some initialize takes,
   and a positional rest when no class defines one. A class-value `new`
   passing `**` has no arm for a Struct or Data class, so the keyword
   channel is refused where one could be the receiver. */
static int fwd_class_value_new_shape(const NodeTable *nt) {
  int sh = def_shape_by_name(nt, "initialize");
  if (sh == -1 && !fwd_any_def_named(nt, "initialize")) return 1;
  if (sh != -2) return sh;
  int shape = 1;
  for (int id = 0; id < nt->count; id++) {
    const char *nm = nt_str(nt, id, "name");
    int dsh = fwd_node_is(nt, id, "DefNode") && nm && sp_streq(nm, "initialize") ? def_shape(nt, id) : -1;
    if (dsh >= 0) shape |= dsh;
  }
  if (!(shape & 2)) return shape;
  for (int id = 0; id < nt->count; id++) {
    const char *nm = nt_str(nt, id, "name");
    if (!fwd_node_is(nt, id, "CallNode") || !nm || !(sp_streq(nm, "define") || sp_streq(nm, "new")))
      continue;
    const char *rn = nt_str(nt, nt_ref(nt, id, "receiver"), "name");
    if (rn && (sp_streq(rn, "Struct") || sp_streq(rn, "Data"))) return -2;
  }
  return shape;
}
/* A name no def declares is a builtin's. Its arity is the C function's, so
   a rest spread into it has no count to bind by: the forwarder takes the
   arguments its callers pass as fixed parameters instead. */
#define FWD_BUILTIN 8
/* The number of arguments every call of `name` passes: -1 when they differ
   or one passes a splat, keywords or a `...` of its own, -2 when there is no
   call. */
static int fwd_fixed_call_arity(const NodeTable *nt, const char *name) {
  int n = -2;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int ac = 0; const int *av = nt_arr(nt, nt_ref(nt, id, "arguments"), "arguments", &ac);
    for (int k = 0; k < ac; k++)
      if (fwd_node_is(nt, av[k], "SplatNode") || fwd_node_is(nt, av[k], "KeywordHashNode") ||
          fwd_node_is(nt, av[k], "ForwardingArgumentsNode")) return -1;
    if (n != -2 && ac != n) return -1;
    n = ac;
  }
  return n;
}
/* The shape a forwarding `call` in `def` reaches: `super` the parent's
   method, `new` the constructed class's initialize. */
/* The shape of the class-level `new` class `cname` or an ancestor defines
   (`def self.new`, or `def new` in its `class << self`): a `new(...)` on that
   class reaches it rather than initialize. -1 when none does, -2 when two
   definitions in one class disagree. Classes are matched by their last name
   segment, as the superclass links are followed. */
/* The ClassNode whose `class << self` body holds `def`, or -1. */
static int fwd_sclass_owner(const NodeTable *nt, int def) {
  for (int id = 0; id < def; id++) {
    if (!fwd_node_is(nt, id, "ClassNode")) continue;
    int bn = 0; const int *bv = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
    for (int k = 0; k < bn; k++) {
      if (!fwd_node_is(nt, bv[k], "SingletonClassNode") ||
          !fwd_node_is(nt, nt_ref(nt, bv[k], "expression"), "SelfNode")) continue;
      int sn = 0; const int *sv = nt_arr(nt, nt_ref(nt, bv[k], "body"), "body", &sn);
      for (int j = 0; j < sn; j++) if (fwd_body_def(nt, sv[j]) == def) return id;
    }
  }
  return -1;
}
static int fwd_class_new_shape_of(const NodeTable *nt, const char *cname) {
  for (int depth = 0; cname && depth < 32; depth++) {
    int shape = -1; const char *super_name = NULL;
    for (int id = 0; id < nt->count; id++) {
      if (!fwd_node_is(nt, id, "ClassNode")) continue;
      const char *cn = nt_str(nt, nt_ref(nt, id, "constant_path"), "name");
      if (!cn || !sp_streq(cn, cname)) continue;
      int sc = nt_ref(nt, id, "superclass");
      if (!super_name && sc >= 0) super_name = nt_str(nt, sc, "name");
      int bn = 0; const int *bv = nt_arr(nt, nt_ref(nt, id, "body"), "body", &bn);
      for (int k = 0; k < bn; k++) {
        int cand[64]; int nc = 0;
        int dk = fwd_body_def(nt, bv[k]);
        if (dk >= 0 && fwd_node_is(nt, nt_ref(nt, dk, "receiver"), "SelfNode")) cand[nc++] = dk;
        else if (fwd_node_is(nt, bv[k], "SingletonClassNode")) {
          int sn = 0; const int *sv = nt_arr(nt, nt_ref(nt, bv[k], "body"), "body", &sn);
          for (int j = 0; j < sn && nc < 64; j++) {
            int sd = fwd_body_def(nt, sv[j]);
            if (sd >= 0 && nt_ref(nt, sd, "receiver") < 0) cand[nc++] = sd;
          }
        }
        for (int j = 0; j < nc; j++) {
          const char *nm = nt_str(nt, cand[j], "name");
          if (!nm || !sp_streq(nm, "new")) continue;
          int sh = def_shape(nt, cand[j]);
          if (sh < 0 || (shape >= 0 && sh != shape)) return -2;
          shape = sh;
        }
      }
    }
    if (shape != -1) return shape;
    cname = super_name;
  }
  return -1;
}
static int fwd_target_shape(const NodeTable *nt, int def, int call, int is_super) {
  const char *name = is_super ? nt_str(nt, def, "name") : nt_str(nt, call, "name");
  if (!name) return -1;
  if (!is_super && !sp_streq(name, "new"))
    return fwd_any_def_named(nt, name) ? def_shape_union_by_name(nt, name) : FWD_BUILTIN;
  int recv = is_super ? -1 : nt_ref(nt, call, "receiver");
  int cls = fwd_enclosing_class(nt, def);
  char ctx[512] = "";
  /* a `new` the class defines itself takes the arguments (#5405, #5410);
     a forwarder in `class << self` belongs to the class around that body */
  if (!is_super && (recv < 0 || fwd_node_is(nt, recv, "SelfNode"))) {
    int owner = nt_ref(nt, def, "receiver") >= 0 ? cls : fwd_sclass_owner(nt, def);
    if (owner >= 0) {
      int us = fwd_class_new_shape_of(nt, nt_str(nt, nt_ref(nt, owner, "constant_path"), "name"));
      if (us != -1) return us;
    }
  }
  if (!is_super && recv >= 0 &&
      (fwd_node_is(nt, recv, "ConstantReadNode") || fwd_node_is(nt, recv, "ConstantPathNode"))) {
    int us = fwd_class_new_shape_of(nt, nt_str(nt, recv, "name"));
    if (us != -1) return us;
  }
  if (is_super) {
    if (nt_ref(nt, def, "receiver") >= 0 || cls < 0) return -1;
    fwd_lex_ctx(nt, cls, ctx, sizeof ctx);
    return fwd_class_method_shape(nt, ctx, nt_ref(nt, cls, "superclass"), NULL, name);
  }
  if (recv < 0 || fwd_node_is(nt, recv, "SelfNode")) {
    if (nt_ref(nt, def, "receiver") < 0 || cls < 0) return -1;
    fwd_ns_key(nt, cls, ctx, sizeof ctx);
    return fwd_class_method_shape(nt, ctx, -1, nt_str(nt, nt_ref(nt, cls, "constant_path"), "name"),
                                  "initialize");
  }
  if (fwd_node_is(nt, recv, "ConstantReadNode") || fwd_node_is(nt, recv, "ConstantPathNode")) {
    fwd_lex_ctx(nt, def, ctx, sizeof ctx);
    return fwd_class_method_shape(nt, ctx, recv, NULL, "initialize");
  }
  return fwd_class_value_new_shape(nt);
}
/* whether a def of `name` takes a *rest or **rest: a forward to it wants the
   splat channels, where the direct model (one synthesized parameter per
   forwarded argument) binds the first argument to the rest slot */
static int def_has_rest_by_name(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    if (fwd_node_is(nt, nt_ref(nt, pn, "rest"), "RestParameterNode") ||
        fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "KeywordRestParameterNode")) return 1;
  }
  return 0;
}
static int fwd_new_node_like(NodeTable *nt, int like, const char *ty) {
  int id = nt_new_node(nt, ty);
  if (id < 0) return -1;
  nt_node_set_int(nt, id, "node_line", nt_int(nt, like, "node_line", 0));
  nt_node_set_int(nt, id, "node_file", nt_int(nt, like, "node_file", 0));
  nt_node_set_int(nt, id, "node_col", nt_int(nt, like, "node_col", 0));
  return id;
}
/* is there a def of `name` at all? def_shape_by_name answers -1 both for
   none and for a `...` forwarder's own def (which takes its target's
   shape), and only the first is a callee no def describes */
static int def_exists_by_name(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, name)) return 1;
  }
  return 0;
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
static int anon_fwd_rewrite(Compiler *c, int node, int parent, unsigned anon) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return 0;
  const char *pty = parent >= 0 ? nt_type(nt, parent) : NULL;
  const char *nm = NULL, *field = NULL;
  if ((anon & 1) && k == NK_BlockArgumentNode) nm = "__anon_block", field = "expression";
  else if ((anon & 2) && k == NK_SplatNode && pty &&
           (sp_streq(pty, "ArgumentsNode") || sp_streq(pty, "ArrayNode")))
    nm = "__anon_rest", field = "expression";
  else if ((anon & 4) && k == NK_AssocSplatNode) nm = "__anon_kwrest", field = "value";
  if (nm && nt_ref(nt, node, field) < 0) {
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (rd < 0) return 0;
    nt_node_set_str(nt, rd, "name", nm);
    nt_node_set_int(nt, rd, "depth", 0);
    nt_node_set_ref(nt, node, field, rd);
    comp_grow_node_arrays(c);
    return 1;
  }
  int changed = 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) changed |= anon_fwd_rewrite(c, nt_ref_at(nt, node, i), node, anon);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, node, i, &n);
    /* the array may move when a new node grows the table: walk a copy */
    int *cp = n > 0 ? (int *)malloc(sizeof(int) * (size_t)n) : NULL;
    if (n > 0 && !cp) continue;
    if (n > 0) memcpy(cp, ids, sizeof(int) * (size_t)n);
    for (int j = 0; j < n; j++) changed |= anon_fwd_rewrite(c, cp[j], node, anon);
    free(cp);
  }
  return changed;
}

static int module_is_extended(const NodeTable *nt, const char *mn) {
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "extend") || nt_ref(nt, id, "receiver") >= 0) continue;
    int an = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    for (int k = 0; k < ac; k++) {
      NodeKind ak = nt_kind(nt, av[k]);
      const char *anm = nt_str(nt, av[k], "name");
      if ((ak == NK_ConstantReadNode || ak == NK_ConstantPathNode) && anm && sp_streq(anm, mn))
        return 1;
    }
  }
  return 0;
}

static int attr_as_def(NodeTable *nt, int call, const char *base, int writer) {
  char mn[256], ivn[256];
  snprintf(mn, sizeof mn, "%s%s", base, writer ? "=" : "");
  snprintf(ivn, sizeof ivn, "@%s", base);
  int iv = nt_new_node(nt, writer ? "InstanceVariableWriteNode" : "InstanceVariableReadNode");
  int def = nt_new_node(nt, "DefNode");
  int body = nt_new_node(nt, "StatementsNode");
  if (iv < 0 || def < 0 || body < 0) return -1;
  nt_node_set_str(nt, iv, "name", ivn);
  if (writer) {
    int pr = nt_new_node(nt, "RequiredParameterNode");
    int ps = nt_new_node(nt, "ParametersNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (pr < 0 || ps < 0 || rd < 0) return -1;
    nt_node_set_str(nt, pr, "name", "value");
    nt_node_set_arr(nt, ps, "requireds", &pr, 1);
    nt_node_set_str(nt, rd, "name", "value");
    nt_node_set_ref(nt, iv, "value", rd);
    nt_node_set_ref(nt, def, "parameters", ps);
  }
  nt_node_set_arr(nt, body, "body", &iv, 1);
  nt_node_set_str(nt, def, "name", mn);
  nt_node_set_ref(nt, def, "body", body);
  static const char *const pos[] = { "node_line", "node_file", "node_col" };
  for (int k = 0; k < 3; k++) {
    long long v = nt_int(nt, call, pos[k], 0);
    nt_node_set_int(nt, def, pos[k], v);
    nt_node_set_int(nt, iv, pos[k], v);
  }
  return def;
}

void desugar_extended_module_attrs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ModuleNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    int body = nt_ref(nt, m, "body");
    int n = 0;
    const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (!mn || !st || !module_is_extended(nt, mn)) continue;
    int *out = malloc(sizeof(int) * (size_t)(n * 2 + 64));
    if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    int no = 0, cap = n * 2 + 64, rewrote = 0;
    for (int k = 0; k < n; k++) {
      int s = st[k];
      const char *an = nt_kind(nt, s) == NK_CallNode && nt_ref(nt, s, "receiver") < 0 &&
                       nt_ref(nt, s, "block") < 0 ? nt_str(nt, s, "name") : NULL;
      int rd = an && (sp_streq(an, "attr_accessor") || sp_streq(an, "attr_reader"));
      int wr = an && (sp_streq(an, "attr_accessor") || sp_streq(an, "attr_writer"));
      int ar = nt_ref(nt, s, "arguments");
      int ac = 0; const int *av = (rd || wr) && ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
      int ok = ac > 0;
      for (int j = 0; j < ac && ok; j++) ok = nt_kind(nt, av[j]) == NK_SymbolNode && nt_str(nt, av[j], "value");
      if (!ok) {
        if (no == cap) { cap *= 2; out = realloc(out, sizeof(int) * (size_t)cap); if (!out) exit(1); }
        out[no++] = s;
        continue;
      }
      for (int j = 0; j < ac; j++) {
        char base[256];
        snprintf(base, sizeof base, "%s", nt_str(nt, av[j], "value"));
        for (int w = 0; w < 2; w++) {
          if (!(w ? wr : rd)) continue;
          int d = attr_as_def(nt, s, base, w);
          if (d < 0) continue;
          if (no == cap) { cap *= 2; out = realloc(out, sizeof(int) * (size_t)cap); if (!out) exit(1); }
          out[no++] = d;
        }
      }
      rewrote = 1;
    }
    if (rewrote) {
      nt_node_set_arr(nt, body, "body", out, no);
      comp_grow_node_arrays(c);
    }
    free(out);
  }
}

static const struct { NodeKind local; const char *global; } dmc_kinds[] = {
  { NK_LocalVariableReadNode, "GlobalVariableReadNode" },
  { NK_LocalVariableWriteNode, "GlobalVariableWriteNode" },
  { NK_LocalVariableTargetNode, "GlobalVariableTargetNode" },
  { NK_LocalVariableOperatorWriteNode, "GlobalVariableOperatorWriteNode" },
  { NK_LocalVariableOrWriteNode, "GlobalVariableOrWriteNode" },
  { NK_LocalVariableAndWriteNode, "GlobalVariableAndWriteNode" },
};

static int dmc_local_kind(NodeKind k) {
  for (size_t i = 0; i < sizeof dmc_kinds / sizeof dmc_kinds[0]; i++)
    if (dmc_kinds[i].local == k) return (int)i;
  return -1;
}

typedef struct { char **names; int n, cap; } DmcNames;

static int dmc_has(const DmcNames *s, const char *nm) {
  for (int i = 0; i < s->n; i++) if (sp_streq(s->names[i], nm)) return 1;
  return 0;
}

/* Walk a body's lexical scope, not into a def or a nested class. `lvl`
   counts the blocks entered, so a local whose depth equals it is the body's
   own. Unset `rewrite` collects those referenced inside a define_method
   block; set, it retypes every reference to a collected name into the
   body's global. */
static void dmc_walk(NodeTable *nt, int id, int lvl, int in_dm, const char *cls,
                     DmcNames *s, int rewrite) {
  if (id < 0) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return;
  int lk = dmc_local_kind(k);
  const char *nm = lk >= 0 ? nt_str(nt, id, "name") : NULL;
  if (nm && nt_int(nt, id, "depth", 0) == lvl) {
    if (!rewrite && in_dm && !dmc_has(s, nm)) {
      if (s->n >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->names = realloc(s->names, sizeof(char *) * (size_t)s->cap);
      }
      s->names[s->n++] = strdup(nm);
    }
    else if (rewrite && dmc_has(s, nm)) {
      char gname[256];
      snprintf(gname, sizeof gname, "$__dmcap%s_%s", cls, nm);
      nt_node_set_type(nt, id, dmc_kinds[lk].global);
      nt_node_set_str(nt, id, "name", gname);
    }
  }
  if (k == NK_BlockNode || k == NK_LambdaNode) lvl++;
  int dm_blk = -1;
  if (k == NK_CallNode) {
    const char *cn = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (cn && (sp_streq(cn, "define_method") || sp_streq(cn, "define_singleton_method")) &&
        (recv < 0 || nt_kind(nt, recv) == NK_SelfNode))
      dm_blk = nt_ref(nt, id, "block");
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) {
    int r = nt_ref_at(nt, id, i);
    dmc_walk(nt, r, lvl, in_dm || (r >= 0 && r == dm_blk), cls, s, rewrite);
  }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *v = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) dmc_walk(nt, v[j], lvl, in_dm, cls, s, rewrite);
  }
}

/* `singleton_class.define_method(:m) { }` (or `self.singleton_class.`) in a
   class or module body -> `define_singleton_method(:m) { }`: self is the
   class there, so its singleton class is the class's own. */
int desugar_singleton_class_define_method(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int cls = 0; cls < n0; cls++) {
    NodeKind ck = nt_kind(nt, cls);
    if (ck != NK_ClassNode && ck != NK_ModuleNode) continue;
    int body = nt_ref(nt, cls, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bv = nt_arr(nt, body, "body", &bn);
    for (int i = 0; i < bn; i++) {
      int id = bv[i];
      const char *cn = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
      int recv = nt_ref(nt, id, "receiver");
      if (!cn || !sp_streq(cn, "define_method") || recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (!rn || !sp_streq(rn, "singleton_class") || nt_ref(nt, recv, "arguments") >= 0 ||
          nt_ref(nt, recv, "block") >= 0 || (rr >= 0 && nt_kind(nt, rr) != NK_SelfNode)) continue;
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_str(nt, id, "name", "define_singleton_method");
      nt_node_reset(nt, recv, "NilNode");
      changed = 1;
    }
  }
  return changed;
}

/* The BlockNode a Proc literal (`-> { }`, `lambda { }`, `proc { }`,
   `Proc.new { }`) runs, else -1. */
static int dmp_literal_block(NodeTable *nt, int v) {
  if (v < 0) return -1;
  if (nt_kind(nt, v) == NK_LambdaNode) return v;
  if (nt_kind(nt, v) != NK_CallNode || nt_ref(nt, v, "arguments") >= 0) return -1;
  const char *nm = nt_str(nt, v, "name");
  int recv = nt_ref(nt, v, "receiver");
  int blk = nt_ref(nt, v, "block");
  if (!nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return -1;
  if (recv < 0 && (sp_streq(nm, "lambda") || sp_streq(nm, "proc"))) return blk;
  if (recv >= 0 && sp_streq(nm, "new") && nt_kind(nt, recv) == NK_ConstantReadNode &&
      sp_streq(nt_str(nt, recv, "name"), "Proc")) return blk;
  return -1;
}

/* The call a body statement makes: the statement itself, or the one
   argument of `private`/`protected`/`public` (`private define_method ...`). */
static int dm_stmt_call(NodeTable *nt, int s) {
  if (nt_kind(nt, s) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, s, "name");
  int args = nt_ref(nt, s, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (nm && nt_ref(nt, s, "receiver") < 0 && an == 1 && nt_kind(nt, av[0]) == NK_CallNode &&
      (sp_streq(nm, "private") || sp_streq(nm, "protected") || sp_streq(nm, "public")))
    return av[0];
  return s;
}

/* Count the writes of body-level local `nm` in a body's lexical scope. */
static int dmp_local_writes(NodeTable *nt, int id, int lvl, const char *nm) {
  if (id < 0) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return 0;
  int lk = dmc_local_kind(k);
  const char *vn = lk > 0 ? nt_str(nt, id, "name") : NULL;
  int n = vn && sp_streq(vn, nm) && nt_int(nt, id, "depth", 0) == lvl;
  if (k == NK_BlockNode || k == NK_LambdaNode) lvl++;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) n += dmp_local_writes(nt, nt_ref_at(nt, id, i), lvl, nm);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *v = nt_arr_at(nt, id, i, &m);
    for (int j = 0; j < m; j++) n += dmp_local_writes(nt, v[j], lvl, nm);
  }
  return n;
}

/* `define_method(:m, instance_method(:x))` -> `alias_method(:m, :x)`: both
   copy x's current body under the new name. */
static int dmp_instance_method_alias(NodeTable *nt, int call, const char *cn, int src, int blk) {
  if (!sp_streq(cn, "define_method") || blk >= 0 || nt_kind(nt, src) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, src, "name");
  int recv = nt_ref(nt, src, "receiver");
  if (!nm || !sp_streq(nm, "instance_method") || nt_ref(nt, src, "block") >= 0 ||
      (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode)) return 0;
  int sargs = nt_ref(nt, src, "arguments");
  int sn = 0; const int *sv = sargs >= 0 ? nt_arr(nt, sargs, "arguments", &sn) : NULL;
  if (sn != 1 || nt_kind(nt, sv[0]) != NK_SymbolNode) return 0;
  int args = nt_ref(nt, call, "arguments");
  int an = 0; const int *av = nt_arr(nt, args, "arguments", &an);
  if (nt_kind(nt, av[0]) == NK_StringNode) {
    char mname[256];
    snprintf(mname, sizeof mname, "%s", nt_str(nt, av[0], "content"));
    nt_node_set_type(nt, av[0], "SymbolNode");
    nt_node_set_str(nt, av[0], "value", mname);
  }
  int na[2] = { av[0], sv[0] };
  nt_node_set_arr(nt, args, "arguments", na, 2);
  nt_node_set_str(nt, call, "name", "alias_method");
  nt_node_reset(nt, src, "NilNode");
  return 1;
}

/* `define_method(:m, <proc>)` / `define_method(:m, &<proc>)` in a class,
   module or `class << self` body, where <proc> is a Proc literal or a body
   local assigned one once, earlier in the body -> `define_method(:m) { }`
   with that literal's block. A local keeps its own literal; the method gets
   a copy. */
int desugar_define_method_proc_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int cls = 0; cls < n0; cls++) {
    NodeKind ck = nt_kind(nt, cls);
    if (ck != NK_ClassNode && ck != NK_ModuleNode && ck != NK_SingletonClassNode) continue;
    int body = nt_ref(nt, cls, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bv = nt_arr(nt, body, "body", &bn);
    for (int i = 0; i < bn; i++) {
      int id = dm_stmt_call(nt, bv[i]);
      if (id < 0) continue;
      const char *cn = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      if (!cn || (!sp_streq(cn, "define_method") && !sp_streq(cn, "define_singleton_method")) ||
          (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      int blk = nt_ref(nt, id, "block");
      int src = -1;
      if (an == 2 && blk < 0) src = av[1];
      else if (an == 1 && blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode)
        src = nt_ref(nt, blk, "expression");
      if (src < 0) continue;
      if (dmp_instance_method_alias(nt, id, cn, src, blk)) {
        changed = 1;
        if (bv[i] == id) continue;
        /* `private define_method(:m, ...)` -> `alias_method(:m, :x); private :m` */
        int vis = bv[i];
        int aa = nt_ref(nt, id, "arguments");
        int an2 = 0; const int *av2 = nt_arr(nt, aa, "arguments", &an2);
        int name = nt_clone_subtree(nt, av2[0]);
        int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
        if (!nb || name < 0) { free(nb); continue; }
        memcpy(nb, bv, sizeof(int) * (size_t)i);
        nb[i] = id;
        nb[i + 1] = vis;
        memcpy(nb + i + 2, bv + i + 1, sizeof(int) * (size_t)(bn - i - 1));
        nt_node_set_arr(nt, body, "body", nb, bn + 1);
        free(nb);
        nt_node_set_arr(nt, nt_ref(nt, vis, "arguments"), "arguments", &name, 1);
        bv = nt_arr(nt, body, "body", &bn);
        i++;
        continue;
      }
      int lit = dmp_literal_block(nt, src), copy = 0;
      if (lit < 0 && nt_kind(nt, src) == NK_LocalVariableReadNode &&
          nt_int(nt, src, "depth", 0) == 0) {
        const char *ln = nt_str(nt, src, "name");
        for (int j = 0; j < i && ln; j++) {
          int w = bv[j];
          if (nt_kind(nt, w) != NK_LocalVariableWriteNode || !sp_streq(nt_str(nt, w, "name"), ln) ||
              dmp_literal_block(nt, nt_ref(nt, w, "value")) < 0) continue;
          if (dmp_local_writes(nt, body, 0, ln) != 1) break;
          int cv = nt_clone_subtree(nt, nt_ref(nt, w, "value"));
          lit = dmp_literal_block(nt, cv);
          if (lit >= 0 && lit != cv) nt_node_reset(nt, cv, "NilNode");
          copy = 1;
          break;
        }
      }
      if (lit < 0) continue;
      if (!copy && lit != src) nt_node_reset(nt, src, "NilNode");
      if (blk >= 0) nt_node_reset(nt, blk, "NilNode");
      if (nt_kind(nt, lit) == NK_LambdaNode) {
        /* a lambda holds its ParametersNode directly, a block through a
           BlockParametersNode */
        int lp = nt_ref(nt, lit, "parameters");
        if (lp >= 0 && nt_kind(nt, lp) == NK_ParametersNode) {
          int bp = fwd_new_node_like(nt, lp, "BlockParametersNode");
          if (bp >= 0) {
            nt_node_set_ref(nt, bp, "parameters", lp);
            nt_node_set_ref(nt, lit, "parameters", bp);
          }
        }
        nt_node_set_type(nt, lit, "BlockNode");
      }
      nt_node_set_arr(nt, args, "arguments", av, 1);
      nt_node_set_ref(nt, id, "block", lit);
      bv = nt_arr(nt, body, "body", &bn);
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* `class D; x = 5; define_method(:f) { x } end`: a local of a class, module
   or top-level body that a define_method block reads or writes becomes a
   global private to that body, in the body and in every block of it. The
   body runs once, so the global is the local's one binding. */
int desugar_define_method_captures(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int cls = 0; cls < n0; cls++) {
    NodeKind ck = nt_kind(nt, cls);
    int body = cls == nt->root_id ? nt_ref(nt, cls, "statements")
             : ck == NK_ClassNode || ck == NK_ModuleNode || ck == NK_SingletonClassNode
             ? nt_ref(nt, cls, "body") : -1;
    if (body < 0) continue;
    DmcNames s = { 0 };
    dmc_walk(nt, body, 0, 0, NULL, &s, 0);
    if (s.n) {
      /* the global carries the class's stable number, not its node id */
      char ctag[64]; snprintf(ctag, sizeof ctag, "%s", comp_node_tag(c, cls));
      dmc_walk(nt, body, 0, 0, ctag, &s, 1);
      changed = 1;
    }
    for (int i = 0; i < s.n; i++) free(s.names[i]);
    free(s.names);
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
/* `define_method(:m) { |a, k: 1, **kw| ... }` in a class body ->
   `def m(a, k: 1, **kw) ... end`, and `define_singleton_method` -> `def
   self.m`. A defined method takes its keywords, rest, post and block
   parameters as a method does, and the call sites and the method's own
   binding of them read a DefNode's parameters; the define_method scope
   registers only its required and optional positionals. Blocks with only
   those keep the define_method form. */
int desugar_define_method_keywords(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int cls = 0; cls < n0; cls++) {
    NodeKind ck = nt_kind(nt, cls);
    if (ck != NK_ClassNode && ck != NK_ModuleNode && ck != NK_SingletonClassNode) continue;
    int body = nt_ref(nt, cls, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bv = nt_arr(nt, body, "body", &bn);
    for (int i = 0; i < bn; i++) {
      int id = dm_stmt_call(nt, bv[i]);
      if (id < 0) continue;
      const char *cn = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      int single = cn && sp_streq(cn, "define_singleton_method") && ck != NK_SingletonClassNode &&
                   (recv < 0 || nt_kind(nt, recv) == NK_SelfNode);
      if (!cn || (!single && (!sp_streq(cn, "define_method") || recv >= 0))) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an != 1) continue;
      const char *mname = nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value")
                        : nt_kind(nt, av[0]) == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
      int blk = nt_ref(nt, id, "block");
      if (!mname || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
      int bp = nt_ref(nt, blk, "parameters");
      if (bp < 0 || nt_kind(nt, bp) != NK_BlockParametersNode) continue;
      int pn = nt_ref(nt, bp, "parameters");
      if (pn < 0) continue;
      int kn = 0, pon = 0;
      nt_arr(nt, pn, "keywords", &kn);
      nt_arr(nt, pn, "posts", &pon);
      int rest = nt_ref(nt, pn, "rest");
      if (kn == 0 && pon == 0 && nt_ref(nt, pn, "keyword_rest") < 0 && nt_ref(nt, pn, "block") < 0 &&
          (rest < 0 || nt_kind(nt, rest) != NK_RestParameterNode)) continue;
      if (single && id != bv[i]) continue;
      int def = fwd_new_node_like(nt, id, "DefNode");
      int dself = single ? fwd_new_node_like(nt, id, "SelfNode") : -1;
      if (def < 0 || (single && dself < 0)) continue;
      nt_node_set_str(nt, def, "name", mname);
      nt_node_set_ref(nt, def, "parameters", pn);
      nt_node_set_ref(nt, def, "body", nt_ref(nt, blk, "body"));
      nt_node_set_ref(nt, def, "receiver", dself);
      if (id != bv[i]) {
        /* `private define_method(...)` -> `private def ...` */
        nt_node_set_arr(nt, nt_ref(nt, bv[i], "arguments"), "arguments", &def, 1);
      }
      else {
        int *nb = malloc(sizeof(int) * (size_t)bn);
        if (!nb) continue;
        memcpy(nb, bv, sizeof(int) * (size_t)bn);
        nb[i] = def;
        nt_node_set_arr(nt, body, "body", nb, bn);
        free(nb);
        bv = nt_arr(nt, body, "body", &bn);
      }
      /* the parameters and body are the def's alone now: passes that scan
         every block or call must not reach them through the old nodes */
      nt_node_reset(nt, blk, "NilNode");
      nt_node_reset(nt, id, "NilNode");
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

int desugar_anon_block_param(Compiler *c) {
  static const struct { const char *field, *kind, *name; } anon_params[] = {
    { "block", "BlockParameterNode", "__anon_block" },
    { "rest", "RestParameterNode", "__anon_rest" },
    { "keyword_rest", "KeywordRestParameterNode", "__anon_kwrest" },
  };
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_DefNode) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    unsigned anon = 0;
    for (unsigned a = 0; a < 3; a++) {
      int p = nt_ref(nt, pn, anon_params[a].field);
      const char *pty = p >= 0 ? nt_type(nt, p) : NULL;
      const char *bn = p >= 0 ? nt_str(nt, p, "name") : NULL;
      if (!pty || !sp_streq(pty, anon_params[a].kind) || (bn && bn[0])) continue;
      nt_node_set_str(nt, p, "name", anon_params[a].name);
      anon |= 1u << a;
    }
    if (!anon) continue;
    anon_fwd_rewrite(c, nt_ref(nt, id, "body"), id, anon);
    changed = 1;
  }
  return changed;
}

/* does any call of `name` pass keywords (a trailing `k: v` / `**h`)? */
static int any_call_passes_keywords(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac >= 1 && av && fwd_node_is(nt, av[ac - 1], "KeywordHashNode")) return 1;
  }
  return 0;
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
    int nfwd_args = 0;
    int nreq = 0; const int *reqs = nt_arr(nt, pn, "requireds", &nreq);
    int nopt = 0; const int *opts = nt_arr(nt, pn, "optionals", &nopt);
    int nlead = nreq + nopt;
    for (int i = 0; i < nreq; i++)
      if (!fwd_node_is(nt, reqs[i], "RequiredParameterNode")) nlead = -1;
    int leads[nlead + 1 > 0 ? nlead + 1 : 1];
    for (int i = 0; i < nlead; i++) leads[i] = i < nreq ? reqs[i] : opts[i - nreq];
    for (int id = def + 1; id < hi && id < n0; id++) {
      /* a bare `super` forwards everything, as `super(...)` does */
      int is_zsuper = fwd_node_is(nt, id, "ForwardingSuperNode");
      if (is_zsuper || fwd_node_is(nt, id, "ForwardingArgumentsNode")) nfwd_args++;
      int is_super = is_zsuper || fwd_node_is(nt, id, "SuperNode");
      if (!is_super && !fwd_node_is(nt, id, "CallNode")) continue;
      int args = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      if (!is_zsuper && (ac < 1 || !av || !fwd_node_is(nt, av[ac - 1], "ForwardingArgumentsNode"))) continue;
      /* `super(...)` reaches the parent's method of this name, and `new(...)`
         the constructed class's initialize */
      const char *cn = is_super ? dname : nt_str(nt, id, "name");
      int is_new = cn && sp_streq(cn, "new");
      if (is_new) cn = "initialize";
      int sh = fwd_target_shape(nt, def, id, is_super);
      if (sh == FWD_BUILTIN) {
        int arity = fwd_fixed_call_arity(nt, dname);
        int via = cn && (sp_streq(cn, "__send__") || sp_streq(cn, "send") ||
                         sp_streq(cn, "public_send") || sp_streq(cn, "call"));
        if (arity < 0 && via) {
          /* A callee that takes anything and sorts it out itself -- a proc's
             `.call`, a `__send__` (the deprecation proxy's method_missing) --
             whose callers disagree on their count or do not exist: it has no
             parameter list to read the channels from, so they are the ones
             this method's callers use, forwarded anonymously (the positional
             channel always, keywords when a caller passes them, the block
             when one passes a block). A fixed-arity builtin keeps the
             refusal below. */
          sh = 1 | (any_call_passes_keywords(nt, dname) ? 2 : 0) | (any_call_passes_block(nt, dname) ? 4 : 0);
        }
        else {
          if (arity == -2) { ok = 0; break; }
          if (arity < 0)
            unsupported_feature(c, id, "`...` forwarded into a builtin method, from calls that do not all "
                                       "pass the same number of positional arguments");
        }
      }
      int recv = is_super ? -1 : nt_ref(nt, id, "receiver");
      if (is_new && sh == -2 && recv >= 0 && !fwd_node_is(nt, recv, "SelfNode") &&
          !fwd_node_is(nt, recv, "ConstantReadNode") && !fwd_node_is(nt, recv, "ConstantPathNode"))
        unsupported_feature(c, id, "`...` forwarded to `new` on a class value, where an initialize "
                                   "takes keywords and a Struct or Data class could be constructed");
      /* A yielding initialize or parent keeps the __fwd_N model, which
         forwards only a `super(...)` that passes nothing before the `...` */
      if (is_zsuper && nlead < 0) { ok = 0; break; }
      int lead = is_zsuper ? nlead : ac - 1;
      /* A Class value's `new` builds a yielding initialize through its proc
         form, which takes the block as the `&` this rewrite passes. */
      int dyn_new = is_new && recv >= 0 && !fwd_node_is(nt, recv, "SelfNode") &&
                    !fwd_node_is(nt, recv, "ConstantReadNode") && !fwd_node_is(nt, recv, "ConstantPathNode");
      if ((is_new || (is_super && !lead)) && !dyn_new && sh >= 0 && (sh & 4) && fwd_any_def_yields(nt, cn)) {
        ok = 0; break;
      }
      if (sh < 0 || nt_ref(nt, id, "block") >= 0) { ok = 0; break; }
      /* Targets of different shapes (`c ? m(...) : n(...)` into `m(a)` and
         `n(**kw)`) each take both channels: `...` forwards everything, and
         each target sorts out what it was handed. Left to the __fwd_N model,
         which binds one slot per argument a caller passes, n was handed the
         leading parameter c in place of the keywords. */
      if (ncalls && sh != shape) {
        if ((sh | shape) & FWD_BUILTIN) { ok = 0; break; }
        sh = 3 | ((sh | shape) & 4);
      }
      shape = sh;
      calls[ncalls++] = id;
    }
    /* A target taking no argument at all forwards the two channels too:
       its own count check has to see what the call passes, which the
       __fwd_N model binds into slots it hands nowhere, so `w(1)` and
       `w(*[], **{"s" => 1})` into `def m()` answered where CRuby raises
       a wrong count */
    if (ok && shape == 0) shape = 3;
    if (!ok || !ncalls || nfwd_args != ncalls || !(shape & (3 | FWD_BUILTIN))) continue;
    /* the block rides along as an anonymous `&` */
    int fwd_block = (shape & 4) || any_call_passes_block(nt, dname);
    int base = nt->count;
    /* def m(a, ...) -> def m(a, __fwdb_0, __fwdb_1) for the two arguments
       its callers pass on, after any optionals, which they then fill */
    int nfixed = 0;
    if (shape == FWD_BUILTIN) {
      nfixed = fwd_fixed_call_arity(nt, dname) - nlead;
      if (nfixed < 0) nfixed = 0;
      int np[nreq + nfixed + 1], nn = 0;
      if (!nopt) for (int i = 0; i < nreq; i++) np[nn++] = leads[i];
      for (int i = 0; i < nfixed; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "__fwdb_%d", i);
        np[nn] = fwd_new_node_like(nt, pn, "RequiredParameterNode");
        nt_node_set_str(nt, np[nn++], "name", nm);
      }
      if (nfixed) nt_node_set_arr(nt, pn, nopt ? "posts" : "requireds", np, nn);
    }
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
    }
    else nt_node_set_ref(nt, pn, "keyword_rest", -1);
    if (fwd_block) {
      int bp = fwd_new_node_like(nt, pn, "BlockParameterNode");
      if (bp < 0) continue;
      nt_node_set_ref(nt, pn, "block", bp);
    }
    /* callee(x, ...) -> callee(x, *, **, &) */
    for (int k = 0; k < ncalls; k++) {
      int call = calls[k];
      if (fwd_node_is(nt, call, "ForwardingSuperNode")) {
        int line = (int)nt_int(nt, call, "node_line", 0);
        int file = (int)nt_int(nt, call, "node_file", 0);
        int col = (int)nt_int(nt, call, "node_col", 0);
        int fargs = fwd_new_node_like(nt, call, "ArgumentsNode");
        if (fargs < 0) continue;
        nt_node_reset(nt, call, "SuperNode");
        nt_node_set_int(nt, call, "node_line", line);
        nt_node_set_int(nt, call, "node_file", file);
        nt_node_set_int(nt, call, "node_col", col);
        nt_node_set_ref(nt, call, "arguments", fargs);
        nt_node_set_ref(nt, call, "block", -1);
        /* a bare super passes the leading parameters too, as their current values */
        int la[nlead + 1];
        for (int i = 0; i < nlead; i++) {
          la[i] = fwd_new_node_like(nt, call, "LocalVariableReadNode");
          if (la[i] < 0) break;
          nt_node_set_str(nt, la[i], "name", nt_str(nt, leads[i], "name"));
          nt_node_set_int(nt, la[i], "depth", 0);
        }
        /* a placeholder for the `...` the loop below drops */
        la[nlead] = fwd_new_node_like(nt, call, "ForwardingArgumentsNode");
        nt_node_set_arr(nt, fargs, "arguments", la, nlead + 1);
      }
      int args = nt_ref(nt, call, "arguments");
      int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
      int nargs[ac + nfixed + 2]; int nn = 0;
      for (int i = 0; i < ac - 1; i++) nargs[nn++] = av[i];
      for (int i = 0; i < nfixed; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "__fwdb_%d", i);
        int rd = fwd_new_node_like(nt, call, "LocalVariableReadNode");
        nt_node_set_str(nt, rd, "name", nm);
        nt_node_set_int(nt, rd, "depth", 0);
        nargs[nn++] = rd;
      }
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
      if (fwd_block) {
        int ba = fwd_new_node_like(nt, call, "BlockArgumentNode");
        if (ba < 0) continue;
        nt_node_set_ref(nt, ba, "expression", -1);
        nt_node_set_ref(nt, call, "block", ba);
      }
    }
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[def];
    changed = 1;
  }
  return changed;
}

typedef struct { int def, st, top, tst, cond, cm, cls; const char *path, *vis; } CondDef;
static int cdef_vis_def(const NodeTable *nt, int id) {
  const char *nm = nt_kind(nt, id) == NK_CallNode && nt_ref(nt, id, "receiver") < 0 ? nt_str(nt, id, "name") : NULL;
  int an = 0;
  const int *av = nm && (sp_streq(nm, "private") || sp_streq(nm, "protected") || sp_streq(nm, "public"))
                  ? nt_arr(nt, nt_ref(nt, id, "arguments"), "arguments", &an) : NULL;
  return an == 1 && nt_kind(nt, av[0]) == NK_DefNode ? av[0] : -1;
}
static void cdef_path(const NodeTable *nt, int cp, char *p, size_t cap) {
  if (nt_kind(nt, cp) == NK_ConstantPathNode) cdef_path(nt, nt_ref(nt, cp, "parent"), p, cap);
  size_t l = strlen(p);
  snprintf(p + l, cap - l, "/%s", nt_str(nt, cp, "name") ? nt_str(nt, cp, "name") : "");
}
static void cdef_walk(NodeTable *nt, int id, CondDef x, CondDef **v, int *n) {
  NodeKind k = nt_kind(nt, id);
  switch (k) {
  case NK_StatementsNode: {
    int bn = 0; const int *b = nt_arr(nt, id, "body", &bn);
    CondDef y = x;
    y.st = id;
    if (x.top < 0) y.tst = id;
    for (int i = 0; i < bn; i++) {
      if (x.top < 0) y.top = b[i];
      cdef_walk(nt, b[i], y, v, n);
    }
    break;
  }
  case NK_IfNode: case NK_UnlessNode: case NK_ElseNode:
    x.cond = 1;
    cdef_walk(nt, nt_ref(nt, id, "statements"), x, v, n);
    cdef_walk(nt, nt_ref(nt, id, "subsequent"), x, v, n);
    cdef_walk(nt, nt_ref(nt, id, "else_clause"), x, v, n);
    break;
  case NK_ClassNode: case NK_ModuleNode: {
    char p[1024];
    snprintf(p, sizeof p, "%s", x.path);
    cdef_path(nt, nt_ref(nt, id, "constant_path"), p, sizeof p);
    x.path = p; x.cls = id; x.cm = 0; x.top = -1;
    cdef_walk(nt, nt_ref(nt, id, "body"), x, v, n);
    break;
  }
  default: break;
  }
  if (k == NK_SingletonClassNode && nt_kind(nt, nt_ref(nt, id, "expression")) == NK_SelfNode) {
    x.cm = 1; x.top = -1;
    cdef_walk(nt, nt_ref(nt, id, "body"), x, v, n);
  }
  else if (cdef_vis_def(nt, id) >= 0) {
    x.vis = nt_str(nt, id, "name");
    cdef_walk(nt, cdef_vis_def(nt, id), x, v, n);
  }
  else if (k == NK_DefNode && x.st >= 0 && nt_str(nt, id, "name") &&
           (nt_ref(nt, id, "receiver") < 0 || nt_kind(nt, nt_ref(nt, id, "receiver")) == NK_SelfNode)) {
    CondDef *g = realloc(*v, sizeof(CondDef) * (size_t)(*n + 1));
    if (!g) return;
    *v = g;
    x.def = id;
    x.cm |= nt_ref(nt, id, "receiver") >= 0;
    x.path = strdup(x.path);
    g[(*n)++] = x;
  }
}
static int cdef_arity(const NodeTable *nt, int def) {
  int pn = nt_ref(nt, def, "parameters"), rn = 0, on = 0, sn = 0, kn = 0;
  const int *r = nt_arr(nt, pn, "requireds", &rn);
  nt_arr(nt, pn, "optionals", &on); nt_arr(nt, pn, "posts", &sn); nt_arr(nt, pn, "keywords", &kn);
  if (on || sn || kn || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0) return -1;
  for (int i = 0; i < rn; i++) if (nt_kind(nt, r[i]) != NK_RequiredParameterNode) return -1;
  return rn;
}
static int cdef_same(const NodeTable *nt, int a, int b) {
  if (a < 0 || b < 0) return a == b;
  const SpNode *x = &nt->nodes[a], *y = &nt->nodes[b];
  if (nt_kind(nt, a) != nt_kind(nt, b) || x->ns != y->ns || x->ni != y->ni || x->nr != y->nr || x->na != y->na ||
      (x->content ? !y->content || strcmp(x->content, y->content) : y->content != NULL)) return 0;
  for (int j = 0; j < x->ns; j++)
    if (strcmp(x->s[j].key, y->s[j].key) || x->s[j].val_len != y->s[j].val_len ||
        memcmp(x->s[j].val, y->s[j].val, x->s[j].val_len)) return 0;
  for (int j = 0; j < x->ni; j++)
    if (strcmp(x->i[j].key, y->i[j].key) || (strncmp(x->i[j].key, "node_", 5) && x->i[j].val != y->i[j].val)) return 0;
  for (int j = 0; j < x->nr; j++) if (!cdef_same(nt, x->r[j].ref, y->r[j].ref)) return 0;
  for (int j = 0; j < x->na; j++) {
    if (x->a[j].n != y->a[j].n) return 0;
    for (int k = 0; k < x->a[j].n; k++) if (!cdef_same(nt, x->a[j].ids[k], y->a[j].ids[k])) return 0;
  }
  return 1;
}
static void cdef_insert_after(NodeTable *nt, int st, int after, int node) {
  int bn = 0; const int *b = nt_arr(nt, st, "body", &bn);
  int *nb = malloc(sizeof(int) * (size_t)(bn + 1)), m = 0;
  if (!nb) return;
  for (int i = 0; i < bn; i++) { nb[m++] = b[i]; if (b[i] == after || cdef_vis_def(nt, b[i]) == after) nb[m++] = node; }
  nt_node_set_arr(nt, st, "body", nb, m);
  free(nb);
}
static int cdef_local(NodeTable *nt, const char *ty, const char *nm) {
  int id = nt_new_node(nt, ty);
  nt_node_set_str(nt, id, "name", nm);
  nt_node_set_int(nt, id, "depth", 0);
  return id;
}
static int cdef_stmts(NodeTable *nt, int st) {
  int b = nt_new_node(nt, "StatementsNode");
  nt_node_set_arr(nt, b, "body", &st, 1);
  return b;
}
int desugar_conditional_defs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  CondDef *v = NULL;
  int n = 0, serial = 0, groups = 0;
  cdef_walk(nt, nt_ref(nt, nt->root_id, "statements"), (CondDef){ -1, -1, -1, -1, 0, 0, -1, "", "" }, &v, &n);
  for (int i = 0; i < n; i++) {
    if (v[i].def < 0 || !v[i].cond) continue;
    char *base = strdup(nt_str(nt, v[i].def, "name"));
    CondDef g[n];
    int ng = 0, bad = 0, blk = 0, same = 1, d0 = v[i].def, ar = cdef_arity(nt, d0);
    for (int j = 0; j < n; j++) {
      if (v[j].def < 0 || v[j].cm != v[i].cm || strcmp(v[j].path, v[i].path) ||
          !sp_streq(nt_str(nt, v[j].def, "name"), base)) continue;
      int hi = fwd_subtree_max(nt, v[j].def);
      for (int s = v[j].def; s <= hi; s++)
        bad |= nt_kind(nt, s) == NK_SuperNode || nt_kind(nt, s) == NK_ForwardingSuperNode;
      blk |= fwd_subtree_uses_yield_or_block(nt, v[j].def);
      bad |= !sp_streq(v[j].vis, v[i].vis);
      if (cdef_arity(nt, v[j].def) != ar) ar = -1;
      same &= cdef_same(nt, v[j].def, d0);
      g[ng++] = v[j];
      v[j].def = -1;
    }
    if (ng < 2 || bad || same) { free(base); continue; }
    char msg[512], nm[512], sel[64], pn[32];
    const char *cn = g[0].cls >= 0 ? nt_str(nt, nt_ref(nt, g[0].cls, "constant_path"), "name") : NULL;
    if (!cn) snprintf(msg, sizeof msg, "undefined method '%s' for main", base);
    else if (g[0].cm) snprintf(msg, sizeof msg, "undefined method '%s' for %s %s", base,
                               nt_kind(nt, g[0].cls) == NK_ModuleNode ? "module" : "class", cn);
    else snprintf(msg, sizeof msg, "undefined method '%s' for an instance of %s", base, cn);
    snprintf(sel, sizeof sel, "$__cond_def%d", ++groups);
    int def = fwd_new_node_like(nt, g[ng - 1].def, "DefNode");
    int ec = nt_new_node(nt, "ConstantReadNode"), em = nt_new_node(nt, "StringNode");
    nt_node_set_str(nt, ec, "name", "NoMethodError");
    nt_node_set_str(nt, em, "content", msg);
    int ea[2] = { ec, em }, eargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, eargs, "arguments", ea, 2);
    int raise = fwd_new_call(nt, -1, "raise", -1);
    nt_node_set_ref(nt, raise, "arguments", eargs);
    int chain = nt_new_node(nt, "ElseNode");
    nt_node_set_ref(nt, chain, "statements", cdef_stmts(nt, raise));
    for (int k = ng - 1; k >= 0; k--) {
      snprintf(nm, sizeof nm, "%s#__cond%d", base, serial + k + 1);
      nt_set_str(nt, g[k].def, "name", nm);
      int wk = fwd_new_node_like(nt, g[k].def, "GlobalVariableWriteNode");
      nt_node_set_str(nt, wk, "name", sel);
      nt_node_set_ref(nt, wk, "value", fwd_new_int(nt, k + 1));
      cdef_insert_after(nt, g[k].st, g[k].def, wk);
      int na = ar < 0 ? 2 : ar, av[na > 0 ? na : 1];
      for (int a = 0; a < ar; a++) { snprintf(pn, sizeof pn, "__cond%d", a); av[a] = cdef_local(nt, "LocalVariableReadNode", pn); }
      if (ar < 0) {
        av[0] = nt_new_node(nt, "SplatNode");
        nt_node_set_ref(nt, av[0], "expression", cdef_local(nt, "LocalVariableReadNode", "__condr"));
        int as = nt_new_node(nt, "AssocSplatNode");
        nt_node_set_ref(nt, as, "value", cdef_local(nt, "LocalVariableReadNode", "__condk"));
        av[1] = nt_new_node(nt, "KeywordHashNode");
        nt_node_set_arr(nt, av[1], "elements", &as, 1);
      }
      int call = fwd_new_call(nt, -1, nm, -1);
      if (na) {
        int args = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, args, "arguments", av, na);
        nt_node_set_ref(nt, call, "arguments", args);
      }
      if (blk) {
        int ba = nt_new_node(nt, "BlockArgumentNode");
        nt_node_set_ref(nt, ba, "expression", cdef_local(nt, "LocalVariableReadNode", "__condb"));
        nt_node_set_ref(nt, call, "block", ba);
      }
      int ifn = nt_new_node(nt, "IfNode");
      nt_node_set_ref(nt, ifn, "predicate", fwd_new_call(nt, cdef_local(nt, "GlobalVariableReadNode", sel), "==",
                                                         fwd_new_int(nt, k + 1)));
      nt_node_set_ref(nt, ifn, "statements", cdef_stmts(nt, call));
      nt_node_set_ref(nt, ifn, "subsequent", chain);
      chain = ifn;
    }
    int params = ar == 0 && !blk ? -1 : nt_new_node(nt, "ParametersNode"), pv[ar > 0 ? ar : 1];
    for (int a = 0; a < ar; a++) { snprintf(pn, sizeof pn, "__cond%d", a); pv[a] = cdef_local(nt, "RequiredParameterNode", pn); }
    if (ar > 0) nt_node_set_arr(nt, params, "requireds", pv, ar);
    if (ar < 0) {
      nt_node_set_ref(nt, params, "rest", cdef_local(nt, "RestParameterNode", "__condr"));
      nt_node_set_ref(nt, params, "keyword_rest", cdef_local(nt, "KeywordRestParameterNode", "__condk"));
    }
    if (blk) nt_node_set_ref(nt, params, "block", cdef_local(nt, "BlockParameterNode", "__condb"));
    nt_node_set_str(nt, def, "name", base);
    nt_node_set_ref(nt, def, "receiver", nt_ref(nt, g[ng - 1].def, "receiver") >= 0 ? nt_new_node(nt, "SelfNode") : -1);
    nt_node_set_ref(nt, def, "parameters", params);
    nt_node_set_ref(nt, def, "body", cdef_stmts(nt, chain));
    cdef_insert_after(nt, g[ng - 1].tst, g[ng - 1].top, *g[0].vis ? fwd_new_call(nt, -1, g[0].vis, def) : def);
    serial += ng;
    free(base);
  }
  for (int i = 0; i < n; i++) free((char *)v[i].path);
  free(v);
  comp_grow_node_arrays(c);
  return groups > 0;
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
    "object_id", "dup", "clone", "itself", "then", "tap", "inspect", "to_s", "class",
    "__enum_pairs", NULL };
  return str_in(nm, ks);
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
      int hi = fwd_subtree_max(nt, def);
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
  /* the numbers carry on across calls: a second pass's copies must not
     take names the first pass's already have */
  static int *site_seq = NULL;
  if (!site_seq) site_seq = calloc((size_t)sp_builtin_enum_names_n + 1, sizeof(int));
  if (!site_seq) { free(gdef); free(nb); return 0; }
  /* One copy per call site. A method's parameters are typed by the union of
     its call sites, so one shared definition called on an IntArray here and
     a Hash there would carry a poly receiver and a poly memo everywhere;
     with its own copy each site's parameters take that site's types, and
     the inliner specializes the copy for the receiver it sees, as it does
     for a yielding method the program wrote for one purpose. The copy is
     named `__enum_<m>__<k>`, k counting the sites of `m` in source order
     (a node id would move whenever a builtin grew or shrank), recorded on
     the call, and the call is
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
    char cn[256]; snprintf(cn, sizeof cn, "__enum_%s__%d", sp_builtin_enum_names[bi], site_seq[bi]++);
    nt_node_set_str(nt, copy, "name", cn);
    nt_node_set_int(nt, copy, "enum_site", id);   /* the call it serves (enum_copy_site) */
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

static int stored_enum_write(Compiler *c, int id, int n0) {
  static const char *const meths[] = {
    "map", "collect", "select", "filter", "find_all", "reject", "sort_by", "group_by",
    "min_by", "max_by", "find", "detect", "flat_map", "collect_concat", "filter_map",
    "partition", "take_while", "drop_while", "find_index", "minmax_by", "reverse_each", "each_entry", NULL };
  NodeTable *nt = (NodeTable *)c->nt;
  int x = nt_ref(nt, id, "receiver");
  const char *nm = nt_str(nt, id, "name"), *vn = nt_str(nt, x, "name");
  if (!nm || !sp_streq(nm, "each") || nt_ref(nt, id, "block") < 0 || nt_ref(nt, id, "arguments") >= 0 ||
      nt_kind(nt, x) != NK_LocalVariableReadNode || !vn) return -1;
  int w = -1, nw = 0;
  for (int k = comp_lvw_first(c, vn); k >= 0; k = comp_lvw_next(c, k))
    if (nt_str(nt, k, "name") && sp_streq(nt_str(nt, k, "name"), vn)) { w = k; nw++; }
  if (nw != 1 || nt_kind(nt, w) != NK_LocalVariableWriteNode) return -1;
  int v = (int)nt_int(nt, w, "enum_src", nt_ref(nt, w, "value"));
  const char *m = nt_str(nt, v, "name");
  int an = 0, k = 0; const int *av = nt_arr(nt, nt_ref(nt, v, "arguments"), "arguments", &an);
  if (nt_kind(nt, v) != NK_CallNode || nt_ref(nt, v, "block") >= 0 || !m || an > sp_streq(m, "find") ||
      (an && (nt_kind(nt, av[0]) == NK_SplatNode || nt_kind(nt, av[0]) == NK_KeywordHashNode)) ||
      nt_ref(nt, v, "receiver") < 0 ||
      (nt_str(nt, v, "call_operator") && sp_streq(nt_str(nt, v, "call_operator"), "&."))) return -1;
  while (meths[k] && !sp_streq(m, meths[k])) k++;
  if (!meths[k] || program_defines_name(nt, n0, m)) return -1;
  for (int p = 0; p < n0; p++) {
    const char *pt = nt_type(nt, p), *pn = nt_str(nt, p, "name");
    if (pt && pn && sp_streq(pn, vn) && (strstr(pt, "ParameterNode") || sp_streq(pt, "BlockLocalVariableNode"))) return -1;
  }
  return w;
}

void desugar_stored_enum_each(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    int w = nt_kind(nt, id) == NK_CallNode ? stored_enum_write(c, id, n0) : -1;
    if (w < 0) continue;
    char hs[48], hp[48]; snprintf(hs, sizeof hs, "__enum_src_%s", comp_node_tag(c, w)); snprintf(hp, sizeof hp, "__enum_ifn_%s", comp_node_tag(c, w));
    int v = (int)nt_int(nt, w, "enum_src", nt_ref(nt, w, "value"));
    int an = 0; const int *av = nt_arr(nt, nt_ref(nt, v, "arguments"), "arguments", &an);
    if (nt_int(nt, w, "enum_src", -1) < 0) {
      int p0 = an ? av[0] : -1;
      int hw = nt_new_node(nt, "LocalVariableWriteNode"), hr = nt_new_node(nt, "LocalVariableReadNode");
      int st = nt_new_node(nt, "StatementsNode"), par = nt_new_node(nt, "ParenthesesNode");
      int pw = an ? nt_new_node(nt, "LocalVariableWriteNode") : -1, pr = an ? nt_new_node(nt, "LocalVariableReadNode") : -1;
      if (hw < 0 || hr < 0 || st < 0 || par < 0 || (an && (pw < 0 || pr < 0))) break;
      nt_node_set_str(nt, hw, "name", hs);
      nt_node_set_ref(nt, hw, "value", nt_ref(nt, v, "receiver"));
      nt_node_set_str(nt, hr, "name", hs);
      nt_node_set_ref(nt, v, "receiver", hr);
      if (an) {
        nt_node_set_str(nt, pw, "name", hp);
        nt_node_set_ref(nt, pw, "value", p0);
        nt_node_set_str(nt, pr, "name", hp);
        nt_node_set_arr(nt, nt_ref(nt, v, "arguments"), "arguments", &pr, 1);
      }
      int sb[3] = { hw, an ? pw : v, v };
      nt_node_set_arr(nt, st, "body", sb, 2 + an);
      nt_node_set_ref(nt, par, "body", st);
      nt_node_set_ref(nt, w, "value", par);
      nt_node_set_int(nt, w, "enum_src", v);
    }
    int sr = nt_new_node(nt, "LocalVariableReadNode");
    if (sr < 0) break;
    nt_node_set_str(nt, sr, "name", hs);
    nt_node_set_int(nt, sr, "depth", nt_int(nt, nt_ref(nt, id, "receiver"), "depth", 0));
    nt_node_set_ref(nt, id, "receiver", sr);
    nt_node_set_str(nt, id, "name", nt_str(nt, v, "name"));
    nt_node_set_int(nt, id, "enum_copy", nt_int(nt, v, "enum_copy", -1));
    if (an) {
      int ar = nt_new_node(nt, "ArgumentsNode"), pr = nt_new_node(nt, "LocalVariableReadNode");
      if (ar < 0 || pr < 0) break;
      nt_node_set_str(nt, pr, "name", hp);
      nt_node_set_int(nt, pr, "depth", nt_int(nt, sr, "depth", 0));
      nt_node_set_arr(nt, ar, "arguments", &pr, 1);
      nt_node_set_ref(nt, id, "arguments", ar);
    }
  }
  comp_grow_node_arrays(c);
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

static int enum_is_a_chain(Compiler *c, const char *rn, const int *defcls, int ndef) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pred = -1;
  for (int k = 0; k < ndef; k++) {
    int pr = nt_new_node(nt, "LocalVariableReadNode");
    int cr = nt_new_node(nt, "ConstantReadNode");
    int ia = nt_new_node(nt, "CallNode");
    int iaa = nt_new_node(nt, "ArgumentsNode");
    if (pr < 0 || cr < 0 || ia < 0 || iaa < 0) return -1;
    nt_node_set_str(nt, pr, "name", rn); nt_node_set_int(nt, pr, "depth", 0);
    nt_node_set_str(nt, cr, "name", c->classes[defcls[k]].name);
    nt_node_set_arr(nt, iaa, "arguments", &cr, 1);
    nt_node_set_str(nt, ia, "name", "is_a?");
    nt_node_set_ref(nt, ia, "receiver", pr);
    nt_node_set_ref(nt, ia, "arguments", iaa);
    if (pred < 0) pred = ia;
    else {
      int orn = nt_new_node(nt, "OrNode");
      if (orn < 0) return -1;
      nt_node_set_ref(nt, orn, "left", pred);
      nt_node_set_ref(nt, orn, "right", ia);
      pred = orn;
    }
  }
  return pred;
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
    /* grep/grep_v answer an Array blockless, never an Enumerator, and their
       block maps rather than iterates: `a.grep(p).each { }` is an each over
       that Array, and left for the chain rule it was left unrewritten, so
       the call named a method Array does not have (Benchmark.benchmark) */
    const char *ern = (er >= 0 && er < n0) ? nt_str(nt, er, "name") : NULL;
    if (ern && (sp_streq(ern, "grep") || sp_streq(ern, "grep_v"))) continue;
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
      char rn[64]; snprintf(rn, sizeof rn, "__enumrecv_%s", comp_node_tag(c, id));
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
      if (!safe_nav) pred = enum_is_a_chain(c, rn, defcls, ndef);
      if (pred < 0) { free(chained); free(in_default); return changed; }
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
        int cpred = enum_is_a_chain(c, rn, defcls, ndef);
        if (cpred < 0) { free(chained); free(in_default); return changed; }
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
        int hi = fwd_subtree_max(nt, def);
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
      int hi = fwd_subtree_max(nt, clone);
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
      }
      else {
        int j = -1;
        for (int m = 0; m < odef_n[bx]; m++) if (sp_streq(odef_name[bx][m], name)) { j = m; break; }
        if (j >= 0) {
          if (odef_def[bx][j] >= 0) bi_subtree_blank(nt, odef_def[bx][j]);
          odef_def[bx][j] = clone;
        }
        else {
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
  static char *seq_name[256]; static int seq_n[256]; static int nseq = 0;   /* across calls, as above */
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
    /* numbered per method in source order, as desugar_builtins numbers its
       copies, not by the call's node id */
    char base[224]; snprintf(base, sizeof base, "%s%s", sp_bx_prefix[bx], cn0);
    int seq = -1, si = 0;
    for (; si < nseq; si++) if (sp_streq(seq_name[si], base)) { seq = seq_n[si]++; break; }
    if (seq < 0 && nseq < (int)(sizeof seq_n / sizeof seq_n[0]) && (seq_name[nseq] = strdup(base))) {
      seq = 0; seq_n[nseq++] = 1;
    }
    char cn[256];
    if (seq >= 0) snprintf(cn, sizeof cn, "%s__%d", base, seq);
    else snprintf(cn, sizeof cn, "%s__n%s", base, comp_node_tag(c, id));   /* past the table: the node id, still unique */
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
    }
    else if (fwd_node_is(nt, id, "CallNode") && nt_str(nt, id, "name") &&
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
    }
    else nt_node_set_ref(nt, v, "arguments", -1);

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

/* How many values the builtin iterator `nm` yields to its block (0: not one
   handled here), and the element type a single yielded value has, so the
   caller can tell whether it is an Array. `hash_pair`: the value is a Hash's
   [key, value] pair. */
static int bs_yield_count(TyKind rt, const char *nm, int argc, TyKind *elem, int *hash_pair) {
  static const char *const one[] = {
    "each", "map", "collect", "flat_map", "collect_concat", "filter_map", "any?", "all?",
    "none?", "one?", "count", "find", "detect", "find_index", "min_by", "max_by", "sort_by",
    "group_by", "partition", "sum", "each_entry", "reverse_each", "take_while", "drop_while",
    "uniq", "select", "filter", "reject", "delete_if", "keep_if", "select!", "filter!",
    "reject!", "map!", "collect!", NULL };
  static const char *const hash_kv[] = {
    "select", "filter", "reject", "delete_if", "keep_if", "select!", "filter!", "reject!", NULL };
  *hash_pair = 0;
  *elem = TY_UNKNOWN;
  /* tap, then and yield_self yield the receiver, whatever it is */
  if ((sp_streq(nm, "tap") || sp_streq(nm, "then") || sp_streq(nm, "yield_self")) && argc == 0) {
    *elem = rt;
    return rt == TY_UNKNOWN ? 0 : 1;
  }
  if (sp_streq(nm, "each_with_index") && argc == 0) return 2;
  if (sp_streq(nm, "each_with_object") && argc == 1) return 2;
  if (ty_is_hash(rt)) {
    if (argc != 0) return 0;
    for (int k = 0; hash_kv[k]; k++) if (sp_streq(nm, hash_kv[k])) return 2;
    if (sp_streq(nm, "each_pair")) { *hash_pair = 1; return 1; }
    if (sp_streq(nm, "each_key")) { *elem = ty_hash_key(rt); return 1; }
    if (sp_streq(nm, "each_value")) { *elem = ty_hash_val(rt); return 1; }
    if (sp_streq(nm, "reverse_each") || sp_streq(nm, "uniq") || sp_streq(nm, "map!") ||
        sp_streq(nm, "collect!")) return 0;
    for (int k = 0; one[k]; k++) if (sp_streq(nm, one[k])) { *hash_pair = 1; return 1; }
    return 0;
  }
  /* A boxed receiver is known only at run time; the names only an Array
     (or only a Hash) answers yield what the Array's (the Hash's) do. The
     element walks are left alone: over an Enumerator one step may yield
     several values. */
  if (rt == TY_POLY) {
    if ((sp_streq(nm, "combination") || sp_streq(nm, "repeated_combination") ||
         sp_streq(nm, "repeated_permutation") || sp_streq(nm, "zip")) && argc == 1) {
      *elem = TY_POLY_ARRAY; return 1;
    }
    if ((sp_streq(nm, "permutation") && argc <= 1) || (sp_streq(nm, "product") && argc >= 1)) {
      *elem = TY_POLY_ARRAY; return 1;
    }
    if ((sp_streq(nm, "each_key") || sp_streq(nm, "each_value")) && argc == 0) {
      *elem = TY_POLY; return 1;
    }
    return 0;
  }
  if (rt == TY_INT) {
    if ((sp_streq(nm, "times") && argc == 0) ||
        ((sp_streq(nm, "upto") || sp_streq(nm, "downto")) && argc == 1)) { *elem = TY_INT; return 1; }
    /* the numbers step yields are Integers or Floats; neither spreads */
    if (sp_streq(nm, "step") && argc >= 1 && argc <= 2) { *elem = TY_INT; return 1; }
    return 0;
  }
  if (rt == TY_FLOAT) {
    if (sp_streq(nm, "step") && argc >= 1 && argc <= 2) { *elem = TY_FLOAT; return 1; }
    return 0;
  }
  if (rt == TY_STRING) {
    if ((sp_streq(nm, "each_char") || sp_streq(nm, "each_line")) && argc == 0) { *elem = TY_STRING; return 1; }
    if (sp_streq(nm, "each_byte") && argc == 0) { *elem = TY_INT; return 1; }
    return 0;
  }
  if ((rt == TY_RANGE || rt == TY_FLOAT_RANGE) && sp_streq(nm, "step") && argc == 1) {
    *elem = rt == TY_RANGE ? TY_INT : TY_FLOAT;
    return 1;
  }
  TyKind et;
  if (ty_is_array(rt) || ty_is_obj_array(rt)) et = ty_array_elem(rt);
  else if (rt == TY_RANGE) et = TY_INT;
  else return 0;
  if ((sp_streq(nm, "each_slice") || sp_streq(nm, "each_cons")) && argc == 1) {
    *elem = TY_POLY_ARRAY;   /* an Array of whatever the elements are */
    return 1;
  }
  if (sp_streq(nm, "sum") && argc <= 1) { *elem = et; return 1; }
  /* inject with a seed yields the accumulator and the element (the seedless
     form is builtins/enumerable.rb's, which yields them itself) */
  if ((sp_streq(nm, "inject") || sp_streq(nm, "reduce")) && argc == 1) return 2;
  if (ty_is_array(rt) || ty_is_obj_array(rt)) {
    /* each_index and the block form of fill yield the index */
    if ((sp_streq(nm, "each_index") && argc == 0) || (sp_streq(nm, "fill") && argc <= 2)) {
      *elem = TY_INT;
      return 1;
    }
    if (sp_streq(nm, "sort_by!") && argc == 0) { *elem = et; return 1; }
    /* one Array per step: a tuple of the receiver's own kind, and product's
       and zip's of boxed values */
    if ((sp_streq(nm, "combination") || sp_streq(nm, "repeated_combination") ||
         sp_streq(nm, "repeated_permutation")) && argc == 1) { *elem = rt; return 1; }
    if (sp_streq(nm, "permutation") && argc <= 1) { *elem = rt; return 1; }
    if ((sp_streq(nm, "product") && argc >= 1) || (sp_streq(nm, "zip") && argc == 1)) {
      *elem = TY_POLY_ARRAY;
      return 1;
    }
  }
  if (argc != 0) return 0;
  if (rt == TY_RANGE && (sp_streq(nm, "reverse_each") || sp_streq(nm, "uniq") ||
                         sp_streq(nm, "map!") || sp_streq(nm, "collect!") ||
                         sp_streq(nm, "delete_if") || sp_streq(nm, "keep_if") ||
                         sp_streq(nm, "select!") || sp_streq(nm, "filter!") ||
                         sp_streq(nm, "reject!"))) return 0;
  for (int k = 0; one[k]; k++) if (sp_streq(nm, one[k])) { *elem = et; return 1; }
  return 0;
}

/* Does the emitter of builtin iterator `nm` on `rt` bind a rest beside the
   leading requireds itself (`|x, *r|`, the rest empty for a value that does
   not spread)? The Array element walks and the Integer counters do
   (emit_iter_bind_rest), and so do the builtins/enumerable.rb methods, whose
   yield binds by the block distribution (emit_block_binds). Any other keeps
   such a block for the prologue below: left to it, the rest read nil, or the
   call was refused (check_block_rest_support). */
static int bs_binds_rest(TyKind rt, const char *nm, int argc) {
  if ((sp_streq(nm, "inject") || sp_streq(nm, "reduce")) && argc > 0) return 0;
  if (builtin_enum_name_index(nm) >= 0) return 1;
  if (rt == TY_INT) return 1;   /* times, upto, downto, step */
  if (sp_streq(nm, "step")) return rt == TY_FLOAT || rt == TY_RANGE || rt == TY_FLOAT_RANGE;
  int arr = ty_is_array(rt) || ty_is_obj_array(rt);
  if (!arr && rt != TY_RANGE) return 0;
  if (sp_streq(nm, "map") || sp_streq(nm, "collect") || sp_streq(nm, "select") ||
      sp_streq(nm, "filter") || sp_streq(nm, "reject")) return 1;
  return arr && (sp_streq(nm, "each") || sp_streq(nm, "each_entry") || sp_streq(nm, "reverse_each"));
}

/* Does the emitter of builtin iterator `nm` spread the one Array a step
   yields across `np` plain requireds itself, as CRuby's block does (`|a, b|`
   over [1, 2] binds 1 and 2)? The element walks do (emit_poly_auto_splat,
   the typed destructure of a row, emit_row_param_bind), and so do the
   builtins/enumerable.rb methods, and product's tuple (emit_tuple_block_params).
   These bind the whole value to the first and leave the others nil (a boxed
   Hash's each_key and each_value spread the pair instead), and zip its two
   values only to two. */
static int bs_spreads(const char *nm, int np) {
  static const char *const whole[] = {
    "combination", "permutation", "repeated_combination", "repeated_permutation",
    "tap", "then", "yield_self", "select!", "filter!", "keep_if", "delete_if", "reject!",
    "map!", "collect!", "each_key", "each_value", NULL };
  if (sp_streq(nm, "zip")) return np == 2;
  for (int k = 0; whole[k]; k++) if (sp_streq(nm, whole[k])) return 0;
  return 1;
}

/* bs_yield_count for a call on an Enumerator. with_index / with_object yield
   the element and the index or memo. Over one that yields two values, the
   methods that pass what `each` yields straight to the block yield both,
   and the rest yield them packed as one [element, index] Array. */
static int bs_enum_yield_count(Compiler *c, int recv, const char *nm, int argc, TyKind *elem) {
  static const char *const packed[] = {
    "select", "filter", "find_all", "reject", "sort_by", "find", "detect", "group_by",
    "min_by", "max_by", "minmax_by", "each_entry", "partition", "drop_while", "sum", NULL };
  const NodeTable *nt = c->nt;
  *elem = TY_UNKNOWN;
  if ((sp_streq(nm, "with_index") && argc <= 1) || (sp_streq(nm, "with_object") && argc == 1)) {
    if (infer_type(c, recv) == TY_ENUMERATOR) return 2;
    /* `arr.map.with_index { }`: the blockless map is typed as the chain it
       heads rather than as an Enumerator */
    int src = nt_kind(nt, recv) == NK_CallNode && nt_ref(nt, recv, "block") < 0 ? nt_ref(nt, recv, "receiver") : -1;
    TyKind st = src >= 0 ? infer_type(c, src) : TY_UNKNOWN;
    return (ty_is_array(st) || ty_is_obj_array(st) || ty_is_hash(st) || st == TY_RANGE) ? 2 : 0;
  }
  if (argc != 0) return 0;
  /* each_slice(n) / each_cons(n) yield one Array per step, which their
     chain emitters bind to the leading parameter and nothing else */
  if (nt_kind(nt, recv) == NK_CallNode && nt_ref(nt, recv, "block") < 0 &&
      infer_type(c, recv) == TY_ENUMERATOR) {
    const char *rn = nt_str(nt, recv, "name");
    int ra = nt_ref(nt, recv, "arguments");
    int rc = 0; if (ra >= 0) nt_arr(nt, ra, "arguments", &rc);
    if (rn && rc == 1 && (sp_streq(rn, "each_slice") || sp_streq(rn, "each_cons"))) {
      int known = enum_pair_spread_iter(nm);
      for (int k = 0; packed[k]; k++) if (sp_streq(nm, packed[k])) known = 1;
      if (!known) return 0;
      *elem = TY_POLY_ARRAY;
      return 1;
    }
  }
  if (!enum_pair_source_call(nt, recv)) return 0;
  if (infer_type(c, recv) != TY_ENUMERATOR && !nt_str(nt, recv, "enum_hop")) return 0;
  if (enum_pair_spread_iter(nm)) return 2;
  for (int k = 0; packed[k]; k++) if (sp_streq(nm, packed[k])) { *elem = TY_POLY_ARRAY; return 1; }
  return 0;
}

typedef struct {
  NodeTable *nt;
  int ok;
} BsB;

static int bs_new(BsB *b, const char *type) {
  int id = nt_new_node(b->nt, type);
  if (id < 0) b->ok = 0;
  return id;
}
static int bs_read(BsB *b, const char *name) {
  int id = bs_new(b, "LocalVariableReadNode");
  if (id < 0) return id;
  nt_node_set_str(b->nt, id, "name", name);
  nt_node_set_int(b->nt, id, "depth", 0);
  return id;
}
static int bs_int(BsB *b, long v) {
  int id = bs_new(b, "IntegerNode");
  if (id >= 0) nt_node_set_int(b->nt, id, "value", v);
  return id;
}
static int bs_call(BsB *b, int recv, const char *name, const int *args, int n) {
  int id = bs_new(b, "CallNode");
  if (id < 0) return id;
  nt_node_set_str(b->nt, id, "name", name);
  nt_node_set_ref(b->nt, id, "receiver", recv);
  nt_node_set_ref(b->nt, id, "block", -1);
  int an = -1;
  if (n > 0) {
    an = bs_new(b, "ArgumentsNode");
    if (an < 0) return -1;
    nt_node_set_arr(b->nt, an, "arguments", args, n);
  }
  nt_node_set_ref(b->nt, id, "arguments", an);
  return id;
}
static int bs_stmts(BsB *b, const int *ids, int n) {
  int id = bs_new(b, "StatementsNode");
  if (id >= 0) nt_node_set_arr(b->nt, id, "body", ids, n);
  return id;
}
static int bs_if(BsB *b, int pred, const int *then_ids, int nthen, const int *else_ids, int nelse) {
  int id = bs_new(b, "IfNode");
  int ts = bs_stmts(b, then_ids, nthen);
  int es = bs_stmts(b, else_ids, nelse);
  int el = bs_new(b, "ElseNode");
  if (id < 0 || ts < 0 || es < 0 || el < 0) return -1;
  nt_node_set_ref(b->nt, el, "statements", es);
  nt_node_set_ref(b->nt, id, "predicate", pred);
  nt_node_set_ref(b->nt, id, "statements", ts);
  nt_node_set_ref(b->nt, id, "subsequent", el);
  return id;
}
static int bs_write(BsB *b, const char *name, int value) {
  int id = bs_new(b, "LocalVariableWriteNode");
  if (id < 0 || value < 0) { b->ok = 0; return -1; }
  nt_node_set_str(b->nt, id, "name", name);
  nt_node_set_int(b->nt, id, "depth", 0);
  nt_node_set_ref(b->nt, id, "value", value);
  return id;
}
static int bs_index(BsB *b, const char *ary, long i) {
  int ix = bs_int(b, i);
  return bs_call(b, bs_read(b, ary), "[]", &ix, 1);
}
static int bs_len_gt(BsB *b, const char *ary, long n) {
  int len = bs_call(b, bs_read(b, ary), "length", NULL, 0);
  int lit = bs_int(b, n);
  return bs_call(b, len, ">", &lit, 1);
}

typedef struct {
  const int *pre; int P;
  const int *opt; int O;
  const int *post; int Q;
  int rest;            /* the RestParameterNode, or -1 */
} BsShape;

/* An optional's default: the node itself on its one use, a copy otherwise */
static int bs_default(BsB *b, const BsShape *s, int j, int copy) {
  int v = nt_ref(b->nt, s->opt[j], "value");
  if (copy) v = nt_clone_subtree(b->nt, v);
  if (v < 0) b->ok = 0;
  return v;
}

/* Bind the parameters from the m values args[] names, a count known at
   compile time. The writes go to out[]; answers how many. */
static int bs_bind_static(BsB *b, const BsShape *s, const char *const *args, int m,
                          int copy_defaults, int *out) {
  NodeTable *nt = b->nt;
  int n = 0;
  for (int i = 0; i < s->P; i++)
    out[n++] = bs_write(b, nt_str(nt, s->pre[i], "name"),
                        i < m ? bs_read(b, args[i]) : bs_new(b, "NilNode"));
  int avail = m - s->P - s->Q;
  for (int j = 0; j < s->O; j++)
    out[n++] = bs_write(b, nt_str(nt, s->opt[j], "name"),
                        j < avail ? bs_read(b, args[s->P + j]) : bs_default(b, s, j, copy_defaults));
  const char *rn = s->rest >= 0 ? nt_str(nt, s->rest, "name") : NULL;
  if (rn && *rn) {
    int els[2]; int ne = 0;
    for (int k = s->P + s->O; k < m - s->Q; k++) els[ne++] = bs_read(b, args[k]);
    int arr = bs_new(b, "ArrayNode");
    if (arr >= 0) nt_node_set_arr(nt, arr, "elements", els, ne);
    out[n++] = bs_write(b, rn, arr);
  }
  int pstart = m - s->Q > s->P ? m - s->Q : s->P;
  for (int k = 0; k < s->Q; k++)
    out[n++] = bs_write(b, nt_str(nt, s->post[k], "name"),
                        pstart + k < m ? bs_read(b, args[pstart + k]) : bs_new(b, "NilNode"));
  return n;
}

/* The same distribution over the elements of the Array `ary`, whose length
   is known only at run time. */
static int bs_bind_dynamic(BsB *b, const BsShape *s, const char *ary, int copy_defaults, int *out) {
  NodeTable *nt = b->nt;
  int n = 0;
  for (int i = 0; i < s->P; i++)
    out[n++] = bs_write(b, nt_str(nt, s->pre[i], "name"), bs_index(b, ary, i));
  for (int j = 0; j < s->O; j++) {
    const char *on = nt_str(nt, s->opt[j], "name");
    int t = bs_write(b, on, bs_index(b, ary, s->P + j));
    int e = bs_write(b, on, bs_default(b, s, j, copy_defaults));
    out[n++] = bs_if(b, bs_len_gt(b, ary, s->P + s->Q + j), &t, 1, &e, 1);
  }
  const char *rn = s->rest >= 0 ? nt_str(nt, s->rest, "name") : NULL;
  if (rn && *rn) {
    int fixed = s->P + s->O + s->Q;
    int len = bs_call(b, bs_read(b, ary), "length", NULL, 0);
    int fx = bs_int(b, fixed);
    int sargs[2] = { bs_int(b, s->P + s->O), bs_call(b, len, "-", &fx, 1) };
    int t = bs_write(b, rn, bs_call(b, bs_read(b, ary), "[]", sargs, 2));
    int z[2] = { bs_int(b, 0), bs_int(b, 0) };
    int e = bs_write(b, rn, bs_call(b, bs_read(b, ary), "[]", z, 2));
    out[n++] = bs_if(b, bs_len_gt(b, ary, fixed), &t, 1, &e, 1);
  }
  /* the posts take the last values once the pre-requireds are covered, and
     the ones right after them otherwise */
  for (int k = 0; k < s->Q; k++) {
    const char *qn = nt_str(nt, s->post[k], "name");
    int t = bs_write(b, qn, bs_index(b, ary, k - s->Q));
    int e = bs_write(b, qn, bs_index(b, ary, s->P + k));
    out[n++] = bs_if(b, bs_len_gt(b, ary, s->P + s->Q), &t, 1, &e, 1);
  }
  return n;
}

/* Prepend the statements pro[0..np) to the block's body. */
static int bs_prepend(BsB *b, int blk, const int *pro, int np) {
  NodeTable *nt = b->nt;
  int body = nt_ref(nt, blk, "body");
  int on = 0;
  const int *old = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &on) : NULL;
  int *all = (int *)malloc(sizeof(int) * (size_t)(np + on + 1));
  if (!all) return -1;
  memcpy(all, pro, sizeof(int) * (size_t)np);
  int na = np;
  if (old) { memcpy(all + na, old, sizeof(int) * (size_t)on); na += on; }
  else all[na++] = body >= 0 ? body : bs_new(b, "NilNode");
  int nbody = bs_stmts(b, all, na);
  free(all);
  return nbody;
}

/* A builtin yields no keywords and no block, so a block's keyword
   parameters take their defaults (a required one raises), `**kw` is {} and
   `&b` nil. They do not count toward spreading a yielded Array either, so
   they leave the parameter list and a prologue binds them. */
static int bs_strip_keywords(Compiler *c, int blk, int bp, int pn) {
  NodeTable *nt = (NodeTable *)c->nt;
  int kn = 0;
  const int *kwa = nt_arr(nt, pn, "keywords", &kn);
  int kr = nt_ref(nt, pn, "keyword_rest");
  int bl = nt_ref(nt, pn, "block");
  if (kn > 16) return 0;
  int kws[16];
  for (int i = 0; i < kn; i++) kws[i] = kwa[i];
  BsB b = { nt, 1 };
  int base = nt->count;
  int pro[20]; int np = 0;
  char names[512]; int missing = 0, mo = 0;
  for (int i = 0; i < kn; i++) {
    if (!fwd_node_is(nt, kws[i], "RequiredKeywordParameterNode")) continue;
    const char *kname = nt_str(nt, kws[i], "name");
    if (!kname) return 0;
    mo += snprintf(names + mo, sizeof names - (size_t)mo, "%s:%.*s", missing ? ", " : "",
                   (int)block_param_written_len(kname), kname);
    if (mo >= (int)sizeof names) return 0;
    missing++;
  }
  if (missing) {
    char msg[600];
    snprintf(msg, sizeof msg, "missing keyword%s: %s", missing > 1 ? "s" : "", names);
    int ea[2] = { bs_new(&b, "ConstantReadNode"), bs_new(&b, "StringNode") };
    if (ea[0] >= 0) nt_node_set_str(nt, ea[0], "name", "ArgumentError");
    if (ea[1] >= 0) nt_node_set_str(nt, ea[1], "content", msg);
    pro[np++] = bs_call(&b, -1, "raise", ea, 2);
  }
  for (int i = 0; i < kn; i++)
    if (fwd_node_is(nt, kws[i], "OptionalKeywordParameterNode"))
      pro[np++] = bs_write(&b, nt_str(nt, kws[i], "name"), nt_ref(nt, kws[i], "value"));
  const char *krn = kr >= 0 && nt_kind(nt, kr) == NK_KeywordRestParameterNode ? nt_str(nt, kr, "name") : NULL;
  if (krn && *krn) {
    int h = bs_new(&b, "HashNode");
    if (h >= 0) nt_node_set_arr(nt, h, "elements", NULL, 0);
    pro[np++] = bs_write(&b, krn, h);
  }
  const char *bln = bl >= 0 ? nt_str(nt, bl, "name") : NULL;
  if (bln && *bln) pro[np++] = bs_write(&b, bln, bs_new(&b, "NilNode"));
  for (int i = 0; i < np; i++) if (pro[i] < 0) b.ok = 0;
  int nbody = np ? bs_prepend(&b, blk, pro, np) : nt_ref(nt, blk, "body");
  if (!b.ok || (np && nbody < 0)) return 0;
  nt_node_set_arr(nt, pn, "keywords", NULL, 0);
  nt_node_set_ref(nt, pn, "keyword_rest", -1);
  nt_node_set_ref(nt, pn, "block", -1);
  int left = 0;
  const char *arrs[3] = { "requireds", "optionals", "posts" };
  for (int k = 0; k < 3; k++) { int n = 0; nt_arr(nt, pn, arrs[k], &n); left += n; }
  if (left == 0 && nt_ref(nt, pn, "rest") < 0) nt_node_set_ref(nt, bp, "parameters", -1);
  nt_node_set_ref(nt, blk, "body", nbody);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
  Scope *bs = comp_scope_of(c, blk);
  /* the keywords are plain locals of this block now, kept apart from any
     other scope's local of the same name */
  for (int i = 0; i < kn; i++) {
    const char *wn = nt_str(nt, kws[i], "name");
    if (block_param_is_renamed(wn)) { scope_local_intern(bs, wn); continue; }
    char kname[160];
    block_param_invent_name(c, kname, sizeof kname, wn, blk);
    blkp_rewrite_refs(c, nbody, nt_str(nt, kws[i], "name"), kname);
    numbered_rename_locals_str(nt, blk, nt_str(nt, kws[i], "name"), kname);
    scope_local_intern(bs, kname);
  }
  if (krn && *krn) scope_local_intern(bs, krn);
  if (bln && *bln) scope_local_intern(bs, bln);
  return 1;
}

/* `enum.map(*a, &b)`: an Enumerator's block iterators take no arguments, so
   a splat forwarded into one (`def m(*, &) = e.map(*, &)`) can only be
   empty. The Array forms already ignore it; over an Enumerator the arms that
   type and emit these calls counted it as an argument, answering nil. */
int desugar_enum_iter_splat_args(Compiler *c) {
  static const char *const names[] = {
    "map", "collect", "flat_map", "collect_concat", "filter_map", "take_while",
    "find_index", "each", "uniq", "select", "filter", "find_all", "reject", "sort_by",
    "group_by", "min_by", "max_by", "minmax_by", "each_entry", "partition", "drop_while",
    "each_with_index", NULL };
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    if (recv < 0 || args < 0 || nt_ref(nt, id, "block") < 0) continue;
    const char *nm = nt_str(nt, id, "name");
    int known = 0;
    for (int k = 0; nm && names[k]; k++) if (sp_streq(nm, names[k])) known = 1;
    if (!known) continue;
    int argc = 0; const int *av = nt_arr(nt, args, "arguments", &argc);
    int only_splats = argc > 0;
    for (int k = 0; k < argc; k++) if (nt_kind(nt, av[k]) != NK_SplatNode) only_splats = 0;
    if (!only_splats || infer_type(c, recv) != TY_ENUMERATOR) continue;
    nt_node_set_ref(nt, id, "arguments", -1);
    changed = 1;
  }
  return changed;
}

static int pdl_body_reads(const NodeTable *nt, int id, const char **names, int n);

/* Does the block read one of its leading requireds past the `m` values a
   step yields (`|q, r|` over each_index reading r)? Those are nil in CRuby.
   The emitters bind the first m and leave the others as they were, which
   reads nil only while nothing else types the slot; the builtins/
   enumerable.rb methods bind them nil through their yield. A block that
   never reads them is left alone. */
static int bs_extras_read(Compiler *c, int blk, const char *const *names, int np, int m,
                          const char *nm, int argc) {
  if (np <= m || ((builtin_enum_name_index(nm) >= 0) &&
                  !((sp_streq(nm, "inject") || sp_streq(nm, "reduce")) && argc > 0))) return 0;
  return pdl_body_reads(c->nt, nt_ref(c->nt, blk, "body"), (const char **)names + m, np - m);
}

/* Can `recv`'s Hash type still be a guess the fixpoint revises? In the
   optimistic rounds a local or an instance variable read can be typed from
   partial evidence: `c[5] = 9` on a local read out of an ivar not yet typed
   reads as a store into a Hash. Where a Hash and an Array yield a different
   count (the filters: the key and the value, or the element), the rewrite,
   which is for good, waits until those rounds are over. */
static int bs_hash_guess(Compiler *c, int recv, TyKind rt, const char *nm) {
  static const char *const two[] = {
    "select", "filter", "reject", "delete_if", "keep_if", "select!", "filter!", "reject!", NULL };
  if (!g_infer_optimistic || !ty_is_hash(rt)) return 0;
  NodeKind k = nt_kind(c->nt, recv);
  if (k != NK_LocalVariableReadNode && k != NK_InstanceVariableReadNode) return 0;
  for (int i = 0; two[i]; i++) if (sp_streq(nm, two[i])) return 1;
  return 0;
}

/* A block given to a builtin iterator with a parameter list the typed
   emitters do not distribute: optionals (`|c, a = 10|`), posts (`|*r, c|`),
   or a rest a yielded pair or Array is spread across. The emitters bind the
   leading requireds by position and nothing else, so the others read nil.
   The block is rewritten to take exactly the values the builtin yields as
   plain requireds, and a prologue assigns the original parameters from them
   by CRuby's rules: requireds (pre and post) first, optionals left to right
   from what remains, the rest the middle. A single yielded Array is spread
   when the list has more than one slot (or a slot and a rest); its length is
   known only at run time, so that prologue indexes it, behind an
   is_a?(Array) test when the element type does not settle it. */
int desugar_builtin_iter_block_shapes(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int bp = nt_ref(nt, blk, "parameters");
    if (bp < 0) continue;
    /* `_1, _2` take the values as `|_1, _2|` would; a lone `_1` or `it`
       never spreads one */
    int numbered = nt_kind(nt, bp) == NK_NumberedParametersNode;
    if (numbered && nt_int(nt, bp, "maximum", 0) < 2) continue;
    if (!numbered && nt_kind(nt, bp) != NK_BlockParametersNode) continue;
    int pn = numbered ? -1 : nt_ref(nt, bp, "parameters");
    if (!numbered && pn < 0) continue;
    BsShape s;
    int npre[9];
    memset(&s, 0, sizeof s);
    s.rest = -1;
    if (numbered) {
      s.P = (int)nt_int(nt, bp, "maximum", 0);
      if (s.P > 9) continue;
      s.pre = npre;
    }
    else {
      s.pre = nt_arr(nt, pn, "requireds", &s.P);
      s.opt = nt_arr(nt, pn, "optionals", &s.O);
      s.post = nt_arr(nt, pn, "posts", &s.Q);
      s.rest = nt_ref(nt, pn, "rest");
      if (s.rest >= 0 && nt_kind(nt, s.rest) != NK_RestParameterNode) s.rest = -1;
    }
    int kn = 0; if (pn >= 0) nt_arr(nt, pn, "keywords", &kn);
    int has_kw = pn >= 0 && (kn || nt_ref(nt, pn, "keyword_rest") >= 0 || nt_ref(nt, pn, "block") >= 0);
    /* leading requireds alone are bound by every emitter, but for the
       spreading of one Array across them, settled once the yield is known
       (bs_spreads) */
    if (s.O == 0 && s.Q == 0 && s.rest < 0 && !has_kw && s.P < 2) continue;
    int bad = 0;
    const char *cop = nt_str(nt, id, "call_operator");
    if (cop && sp_streq(cop, "&.")) bad = 1;
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    for (int k = 0; k < argc; k++) {
      NodeKind ak = nt_kind(nt, av[k]);
      if (ak == NK_SplatNode || ak == NK_BlockArgumentNode || ak == NK_KeywordHashNode) bad = 1;
    }
    if (bad) continue;
    TyKind rt = infer_type(c, recv);
    /* a program's own tap or then, on any receiver, yields what it likes,
       and so does its own method of the name on a boxed receiver */
    if ((rt == TY_POLY || sp_streq(nm, "tap") || sp_streq(nm, "then") || sp_streq(nm, "yield_self")) &&
        def_exists_by_name(nt, nm)) continue;
    if (bs_hash_guess(c, recv, rt, nm)) continue;
    TyKind elem; int hash_pair;
    int m = bs_enum_yield_count(c, recv, nm, argc, &elem);
    int via_enum = m != 0;
    hash_pair = 0;
    if (m == 0) m = bs_yield_count(rt, nm, argc, &elem, &hash_pair);
    /* Array.new(n) { } yields the index */
    if (m == 0 && sp_streq(nm, "new") && argc == 1 && nt_kind(nt, recv) == NK_ConstantReadNode &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Array")) {
      m = 1;
      elem = TY_INT;
    }
    if (m == 0) {
      /* no Enumerator passes its block keywords, whatever it yields */
      if (has_kw && rt == TY_ENUMERATOR && bs_strip_keywords(c, blk, bp, pn)) changed = 1;
      continue;
    }
    if (has_kw) {
      if (!bs_strip_keywords(c, blk, bp, pn)) continue;
      changed = 1;
      s.pre = nt_arr(nt, pn, "requireds", &s.P);
      s.opt = nt_arr(nt, pn, "optionals", &s.O);
      s.post = nt_arr(nt, pn, "posts", &s.Q);
    }
    int plain = s.O == 0 && s.Q == 0 && s.rest < 0;
    /* past the values that do not spread, a required the body reads is
       bound nil (bs_extras_read) */
    int nospread = m >= 2 || hash_pair || (elem != TY_UNKNOWN && elem != TY_POLY && !ty_is_array(elem));
    const char *pnames[12];
    for (int i = 0; i < s.P && i < 12; i++)
      pnames[i] = numbered ? numbered_param_name(c, bp, i) : nt_str(nt, s.pre[i], "name");
    int extras = plain && !via_enum && nospread && s.P <= 12 &&
                 bs_extras_read(c, blk, pnames, s.P, hash_pair ? 2 : m, nm, argc);
    if (plain && !extras && (m != 1 || s.P < 2 || hash_pair || via_enum || bs_spreads(nm, s.P))) continue;
    bad = s.P + s.O + s.Q > 12;
    for (int i = 0; i < s.P && !numbered; i++) if (nt_kind(nt, s.pre[i]) != NK_RequiredParameterNode) bad = 1;
    for (int i = 0; i < s.Q; i++) if (nt_kind(nt, s.post[i]) != NK_RequiredParameterNode) bad = 1;
    if (bad) continue;
    int slots = s.P + s.O + s.Q;
    int splat = m == 1 && (slots > 1 || (slots >= 1 && s.rest >= 0));
    int dyn = 0;   /* 1: the value is an Array; 2: tested at run time */
    if (splat && hash_pair) m = 2;
    else if (splat) {
      if (elem == TY_UNKNOWN) continue;
      if (ty_is_array(elem)) dyn = 1;
      else if (elem == TY_POLY) dyn = 2;
    }
    /* a rest beside the leading requireds is left to an emitter that binds
       it (bs_binds_rest); a lone rest over ONE yielded value never spreads
       it (`|*r|` gets `[x]` even when x is an Array), which the emitters got
       wrong for an Array element, so that shape is always lowered here; the
       chain emitters over an Enumerator bind no rest right, so there it is
       too */
    { const char *rn = s.rest >= 0 ? nt_str(nt, s.rest, "name") : NULL;
      /* an anonymous `*` binds nothing: beside requireds it only drops the
         values past them, which every emitter does */
      int rest_bound = !(rn && *rn) || bs_binds_rest(rt, nm, argc);
      if (s.O == 0 && s.Q == 0 && !splat && m != 1 && !ty_is_hash(rt) && !via_enum && rest_bound && !extras) continue;
      if (s.O == 0 && s.Q == 0 && splat && !hash_pair && !dyn && rest_bound && !extras) continue; }
    if (plain && !dyn && !extras) continue;

    BsB b = { nt, 1 };
    int base = nt->count;
    if (numbered)
      for (int i = 0; i < s.P; i++) {
        npre[i] = bs_new(&b, "RequiredParameterNode");
        if (npre[i] >= 0) nt_node_set_str(nt, npre[i], "name", numbered_param_name(c, bp, i));
      }
    char names[2][64];
    const char *argn[2];
    int reqs[2];
    for (int k = 0; k < m; k++) {
      snprintf(names[k], sizeof names[k], "__bs%d_%s", k, comp_node_tag(c, blk));
      argn[k] = names[k];
      reqs[k] = bs_new(&b, "RequiredParameterNode");
      if (reqs[k] >= 0) nt_node_set_str(nt, reqs[k], "name", argn[k]);
    }
    int pro[32]; int np = 0;
    if (dyn == 0) np = bs_bind_static(&b, &s, argn, m, 0, pro);
    else if (dyn == 1) np = bs_bind_dynamic(&b, &s, argn[0], 0, pro);
    else {
      int dw[16], sw[16];
      int nd = bs_bind_dynamic(&b, &s, argn[0], 1, dw);
      int ns = bs_bind_static(&b, &s, argn, 1, 0, sw);
      int cr = bs_new(&b, "ConstantReadNode");
      if (cr >= 0) nt_node_set_str(nt, cr, "name", "Array");
      int pred = bs_call(&b, bs_read(&b, argn[0]), "is_a?", &cr, 1);
      pro[np++] = bs_if(&b, pred, dw, nd, sw, ns);
    }
    for (int i = 0; i < np; i++) if (pro[i] < 0) b.ok = 0;
    int nbody = bs_prepend(&b, blk, pro, np);
    int npn = bs_new(&b, "ParametersNode");
    if (!b.ok || npn < 0 || nbody < 0) return changed;
    nt_node_set_arr(nt, npn, "requireds", reqs, m);
    if (numbered) {
      bp = bs_new(&b, "BlockParametersNode");
      if (bp < 0) return changed;
      nt_node_set_ref(nt, blk, "parameters", bp);
    }
    nt_node_set_ref(nt, bp, "parameters", npn);
    nt_node_set_ref(nt, blk, "body", nbody);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < m; k++) {
      LocalVar *lv = scope_local_intern(bs, argn[k]);
      if (lv) lv->is_block_param = 1;
    }
    /* the original names are plain locals now, and one nothing reads still
       needs its slot for the prologue's write */
    for (int i = 0; i < s.P; i++) scope_local_intern(bs, nt_str(nt, s.pre[i], "name"));
    for (int i = 0; i < s.O; i++) scope_local_intern(bs, nt_str(nt, s.opt[i], "name"));
    for (int i = 0; i < s.Q; i++) scope_local_intern(bs, nt_str(nt, s.post[i], "name"));
    { const char *rn = s.rest >= 0 ? nt_str(nt, s.rest, "name") : NULL;
      if (rn && *rn) scope_local_intern(bs, rn); }
    changed = 1;
  }
  return changed;
}

/* `&:m` reaches here as `{ |_spx| _spx.m }`, a block spinel_parse.c marks
   sym_proc_block (a user's own block spelled the same is not one). The
   block is marked with m before a desugar rewrites the call (`_spx.first`
   -> `_spx[0]`), for the shapes below that pass it a second value. */
void mark_sym_proc_blocks(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  NT_FOREACH_KIND(nt, NK_BlockNode, blk) {
    if (!nt_int(nt, blk, "sym_proc_block", 0)) continue;
    int bp = nt_ref(nt, blk, "parameters");
    int pn = bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
    if (pn < 0) continue;
    int P = 0, O = 0, Q = 0, kn = 0;
    const int *pre = nt_arr(nt, pn, "requireds", &P);
    nt_arr(nt, pn, "optionals", &O); nt_arr(nt, pn, "posts", &Q); nt_arr(nt, pn, "keywords", &kn);
    if (P != 1 || O || Q || kn || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0 ||
        nt_ref(nt, pn, "block") >= 0) continue;
    const char *pnm = nt_str(nt, pre[0], "name");
    if (nt_kind(nt, pre[0]) != NK_RequiredParameterNode || !pnm) continue;
    int body = nt_ref(nt, blk, "body");
    int bn = 0;
    const int *bv = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn != 1 || nt_kind(nt, bv[0]) != NK_CallNode || nt_ref(nt, bv[0], "block") >= 0) continue;
    int r = nt_ref(nt, bv[0], "receiver");
    const char *rn = r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode ? nt_str(nt, r, "name") : NULL;
    const char *mn = nt_str(nt, bv[0], "name");
    int an = 0; int a = nt_ref(nt, bv[0], "arguments");
    if (a >= 0) nt_arr(nt, a, "arguments", &an);
    if (!rn || !sp_streq(rn, pnm) || an || !mn || !(mn[0] == '_' || isalpha((unsigned char)mn[0])))
      continue;
    nt_node_set_str(nt, blk, "sym_proc", mn);
  }
}

/* The method a still one-parameter `&:m` block calls, or NULL. */
static const char *sym_proc_block_name(const NodeTable *nt, int blk) {
  const char *mn = nt_str(nt, blk, "sym_proc");
  int bp = mn ? nt_ref(nt, blk, "parameters") : -1;
  int pn = bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
  int P = 0;
  if (pn >= 0) nt_arr(nt, pn, "requireds", &P);
  return P == 1 ? mn : NULL;
}

/* `<recv>.m(<second>)` */
static int sym_proc_call2(BsB *b, const char *recv, const char *mn, int second) {
  int call = bs_call(b, bs_read(b, recv), mn, &second, 1);
  return second < 0 ? -1 : call;
}

/* The values the body under `node` passes its method's block, as every
   `yield` and every call of the `&b` named `bpn` agree (-1 when none is
   there yet); -2 when two disagree, one spreads a splat, or `b` is read for
   anything but a call. `*calls` counts those calls, `*reads` every read of b. */
static int block_values_in(const NodeTable *nt, int node, const char *bpn, int n, int *calls, int *reads) {
  if (node < 0 || node >= nt->count || n == -2) return n;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return n;
  int args = -2;
  if (k == NK_YieldNode) args = nt_ref(nt, node, "arguments");
  else if (bpn && k == NK_LocalVariableReadNode && sp_streq(nt_str(nt, node, "name"), bpn)) (*reads)++;
  else if (bpn && k == NK_CallNode) {
    int r = nt_ref(nt, node, "receiver");
    const char *cn = nt_str(nt, node, "name");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode && sp_streq(nt_str(nt, r, "name"), bpn) &&
        cn && (sp_streq(cn, "call") || sp_streq(cn, "yield") || sp_streq(cn, "()") || sp_streq(cn, "[]"))) {
      (*calls)++;
      args = nt_ref(nt, node, "arguments");
    }
  }
  if (args != -2) {
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      NodeKind ak = nt_kind(nt, av[j]);
      const char *aty = nt_type(nt, av[j]);
      if (ak == NK_SplatNode || ak == NK_BlockArgumentNode ||
          (aty && sp_streq(aty, "ForwardingArgumentsNode"))) return -2;
    }
    if (n >= 0 && n != an) return -2;
    n = an;
  }
  const SpNode *nd = &nt->nodes[node];
  for (int j = 0; j < nd->nr; j++) n = block_values_in(nt, nd->r[j].ref, bpn, n, calls, reads);
  for (int j = 0; j < nd->na; j++)
    for (int q = 0; q < nd->a[j].n; q++) n = block_values_in(nt, nd->a[j].ids[q], bpn, n, calls, reads);
  return n;
}

/* For a call `id` of a method of the program, how many values it passes its
   block when that is more than one and its body agrees on it, else 0; -1
   when the call reaches no method of the program. */
static int user_block_values(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (nt_kind(nt, id) != NK_CallNode || (nm && strncmp(nm, "__enum_", 7) == 0)) return -1;
  int mi = backprop_call_target(c, id);
  if (mi < 0 || mi >= c->nscopes) return -1;
  const Scope *ms = &c->scopes[mi];
  const char *bpn = ms->blk_param;
  if (bpn && !*bpn) return 0;   /* an anonymous `&` is only handed on */
  int calls = 0, reads = 0;
  int n = block_values_in(nt, ms->def_node >= 0 ? nt_ref(nt, ms->def_node, "body") : -1,
                          bpn, -1, &calls, &reads);
  return n >= 2 && n <= 8 && reads == calls ? n : 0;
}

/* How many values the call `id` passes a Symbol's block, when it is more
   than one, so that the block sends the rest to the first, as Symbol#to_proc
   does; 0 otherwise. A method of the program passes what its body yields, or
   hands its `&b`, when every such site agrees. A builtin iterator passes two
   when it calls its block with an accumulator and the element, the two it
   compares, an element and its index or memo, or a Hash's key and value. A
   call desugar_builtin_enum_calls moved onto its builtin's copy
   (`__enum_inject__N(recv, ...)`) answers by the name it had. */
int sym_block_values(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  char nb[64];
  if (nm && recv < 0 && argc >= 1 && strncmp(nm, "__enum_", 7) == 0) {
    const char *sep = strstr(nm + 7, "__");
    if (!sep || (size_t)(sep - nm - 7) >= sizeof nb) return 0;
    memcpy(nb, nm + 7, (size_t)(sep - nm - 7)); nb[sep - nm - 7] = 0;
    nm = nb; recv = av[0]; argc--;
  }
  else {
    int n = user_block_values(c, id);
    if (n >= 0) return n;
  }
  if (!nm) return 0;
  if (sp_streq(nm, "reduce") || sp_streq(nm, "inject") || sp_streq(nm, "sort") ||
      sp_streq(nm, "sort!") || sp_streq(nm, "min") || sp_streq(nm, "max") || sp_streq(nm, "minmax") ||
      sp_streq(nm, "chunk_while") || sp_streq(nm, "slice_when"))
    return 2;
  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  if (ty_is_hash(rt) && sp_streq(nm, "to_h") && argc == 0) return 2;
  TyKind elem; int hash_pair;
  return recv >= 0 && bs_yield_count(rt, nm, argc, &elem, &hash_pair) == 2 ? 2 : 0;
}

/* Does comparator `nm` (sort, min, max, minmax) over `recv` compare
   elements no typed emitter compares itself -- Arrays, a Hash's pairs,
   objects, a receiver known only at run time -- so that an operator symbol
   has to become its block? Over Integers, Floats and Strings the emitters
   compare the elements directly. */
static int op_sym_comparator(Compiler *c, int recv, const char *nm) {
  if (!sp_streq(nm, "sort") && !sp_streq(nm, "sort!") && !sp_streq(nm, "min") &&
      !sp_streq(nm, "max") && !sp_streq(nm, "minmax")) return 0;
  TyKind rt = infer_type(c, recv);
  if (ty_is_hash(rt) || rt == TY_POLY) return 1;
  if (!ty_is_array(rt) && !ty_is_obj_array(rt)) return 0;
  TyKind et = ty_array_elem(rt);
  return et != TY_INT && et != TY_FLOAT && et != TY_STRING;
}

/* An operator symbol (`&:+`) stays a BlockArgumentNode, which spinel_parse.c
   does not spell as a block. Over a chain yielding two values, a method of
   the program or a builtin iterator that does (each_with_object, a Hash's
   select), or a comparator over elements its emitter does not compare
   (op_sym_comparator), it calls the operator on the first with the second:
   { |__spa_N, __spb_N| __spa_N + __spb_N }. reduce and inject keep theirs
   for the fold emitters. */
static int desugar_enum_pair_op_sym(Compiler *c, int id, int recv, int blk, const char *nm, int argc) {
  NodeTable *nt = (NodeTable *)c->nt;
  int ex = nt_ref(nt, blk, "expression");
  const char *mn = ex >= 0 && nt_kind(nt, ex) == NK_SymbolNode ? nt_str(nt, ex, "value") : NULL;
  TyKind elem; int hash_pair;
  if (!mn || !*mn) return 0;
  if (sp_streq(nm, "reduce") || sp_streq(nm, "inject")) return 0;
  if (user_block_values(c, id) != 2 &&
      (recv < 0 || (bs_enum_yield_count(c, recv, nm, argc, &elem) != 2 &&
                    bs_yield_count(infer_type(c, recv), nm, argc, &elem, &hash_pair) != 2 &&
                    !op_sym_comparator(c, recv, nm)))) return 0;
  char pa[48], pb[48];
  snprintf(pa, sizeof pa, "__spa_%s", comp_node_tag(c, blk));
  snprintf(pb, sizeof pb, "__spb_%s", comp_node_tag(c, blk));
  BsB b = { nt, 1 };
  int base = nt->count;
  int reqs[2] = { bs_new(&b, "RequiredParameterNode"), bs_new(&b, "RequiredParameterNode") };
  if (!b.ok) return 0;
  nt_node_set_str(nt, reqs[0], "name", pa);
  nt_node_set_str(nt, reqs[1], "name", pb);
  int second = bs_read(&b, pb);
  int call = bs_call(&b, bs_read(&b, pa), mn, &second, 1);
  int body = bs_stmts(&b, &call, 1);
  int params = bs_new(&b, "ParametersNode");
  int bparams = bs_new(&b, "BlockParametersNode");
  int blocknode = bs_new(&b, "BlockNode");
  if (!b.ok) return 0;
  nt_node_set_arr(nt, params, "requireds", reqs, 2);
  nt_node_set_ref(nt, bparams, "parameters", params);
  nt_node_set_ref(nt, blocknode, "parameters", bparams);
  nt_node_set_ref(nt, blocknode, "body", body);
  nt_node_set_ref(nt, id, "block", blocknode);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
  Scope *bs = comp_scope_of(c, blocknode);
  for (int k = 0; k < 2; k++) {
    LocalVar *lv = scope_local_intern(bs, k ? pb : pa);
    if (lv) lv->is_block_param = 1;
  }
  return 1;
}

/* An operator symbol block argument (`&:+`, `&:[]`) of call `id` as the
   block spinel_parse.c spells a named one: { |_spx| _spx.+() }, marked
   sym_proc. Only for a receiver whose shape is settled at run time, where
   the block is reshaped to call the operator with the second value too. */
static int sym_proc_blockify_op(Compiler *c, int id) {
  NodeTable *nt = (NodeTable *)c->nt;
  int blk = nt_ref(nt, id, "block");
  int ex = blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode ? nt_ref(nt, blk, "expression") : -1;
  const char *mn = ex >= 0 && nt_kind(nt, ex) == NK_SymbolNode ? nt_str(nt, ex, "value") : NULL;
  if (!mn || !*mn || mn[0] == '_' || isalpha((unsigned char)mn[0])) return 0;
  static const char *const ops[] = {
    "+", "-", "*", "/", "%", "**", "==", "!=", "<", ">", "<=", ">=", "<=>", "===", "=~",
    "<<", ">>", "&", "|", "^", "[]", NULL };
  int known = 0;
  for (int k = 0; ops[k]; k++) if (sp_streq(mn, ops[k])) known = 1;
  if (!known) return 0;
  BsB b = { nt, 1 };
  int base = nt->count;
  int req = bs_new(&b, "RequiredParameterNode");
  if (req < 0) return 0;
  nt_node_set_str(nt, req, "name", "_spx");
  int call = bs_call(&b, bs_read(&b, "_spx"), mn, NULL, 0);
  int body = bs_stmts(&b, &call, 1);
  int params = bs_new(&b, "ParametersNode");
  int bparams = bs_new(&b, "BlockParametersNode");
  int blocknode = bs_new(&b, "BlockNode");
  if (!b.ok) return 0;
  nt_node_set_arr(nt, params, "requireds", &req, 1);
  nt_node_set_ref(nt, bparams, "parameters", params);
  nt_node_set_ref(nt, blocknode, "parameters", bparams);
  nt_node_set_ref(nt, blocknode, "body", body);
  nt_node_set_str(nt, blocknode, "sym_proc", mn);
  nt_node_set_ref(nt, id, "block", blocknode);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
  LocalVar *lv = scope_local_intern(comp_scope_of(c, blocknode), "_spx");
  if (lv) lv->is_block_param = 1;
  return 1;
}

/* Whether some user class has a block-taking method `nm`, which a poly
   receiver reaches through the class-id dispatch
   (poly_block_call_needs_dispatch) rather than the poly map loop. */
static int user_block_method(Compiler *c, const char *nm) {
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_method_in_chain(c, k, nm, NULL);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    if (m->yields || (m->blk_param && m->blk_param[0])) return 1;
  }
  return 0;
}

/* `&:m` block `blk` as one taking every yielded value, for a call the
   class-id dispatch may serve: it lifts the block, so a user method
   yielding two values reaches it without sym_proc_pair.
   { |*__spr_N| __spr_N.length > 1 ? __spr_N[0].m(__spr_N[1]) : __spr_N[0].m } */
static int sym_proc_rest_view(Compiler *c, int blk, const char *mn) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pn = nt_ref(nt, nt_ref(nt, blk, "parameters"), "parameters");
  char rn[48]; snprintf(rn, sizeof rn, "__spr_%s", comp_node_tag(c, blk));
  BsB b = { nt, 1 };
  int base = nt->count;
  int rest = bs_new(&b, "RestParameterNode");
  if (rest < 0) return 0;
  nt_node_set_str(nt, rest, "name", rn);
  int second = bs_index(&b, rn, 1);
  int two = bs_call(&b, bs_index(&b, rn, 0), mn, &second, 1);
  int one = bs_call(&b, bs_index(&b, rn, 0), mn, NULL, 0);
  int body = bs_if(&b, bs_len_gt(&b, rn, 1), &two, 1, &one, 1);
  int stmts = bs_stmts(&b, &body, 1);
  if (!b.ok || stmts < 0) return 0;
  nt_node_set_arr(nt, pn, "requireds", NULL, 0);
  nt_node_set_ref(nt, pn, "rest", rest);
  nt_node_set_ref(nt, blk, "body", stmts);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
  LocalVar *lv = scope_local_intern(comp_scope_of(c, blk), rn);
  if (lv) lv->is_block_param = 1;
  return 1;
}

/* `&:m` of a map over a receiver known only at run time: an Enumerator
   yielding two values calls m on the first with the second, which the
   poly map loop decides per call by the flag it carries
   (sp_poly_yields_pair). The block keeps its one-value body and gains
   that call as `sym_proc_pair`, over a parameter `sym_proc_arg` of its
   own: { |_spx| _spx.m } + _spx.m(__spy_N). */
int sym_proc_poly_pair_view(Compiler *c, int id) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *nm = nt_str(nt, id, "name");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (!nm || argc || ty_iter_shape(nm) != TY_ITER_MAP || nt_ref(nt, id, "block") < 0) return 0;
  int changed = sym_proc_blockify_op(c, id);
  int blk = nt_ref(nt, id, "block");
  const char *mn = nt_kind(nt, blk) == NK_BlockNode ? sym_proc_block_name(nt, blk) : NULL;
  if (!mn || nt_ref(nt, blk, "sym_proc_pair") >= 0) return changed;
  if (user_block_method(c, nm)) return sym_proc_rest_view(c, blk, mn) | changed;
  int pn = nt_ref(nt, nt_ref(nt, blk, "parameters"), "parameters");
  int P = 0; const int *pre = nt_arr(nt, pn, "requireds", &P);
  const char *xn = nt_str(nt, pre[0], "name");
  if (!xn) return changed;
  char an[48]; snprintf(an, sizeof an, "__spy_%s", comp_node_tag(c, blk));
  BsB b = { nt, 1 };
  int base = nt->count;
  int call = sym_proc_call2(&b, xn, mn, bs_read(&b, an));
  if (!b.ok || call < 0) return changed;
  nt_node_set_ref(nt, blk, "sym_proc_pair", call);
  nt_node_set_str(nt, blk, "sym_proc_arg", an);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
  LocalVar *lv = scope_local_intern(comp_scope_of(c, blk), an);
  if (lv) { lv->is_block_param = 1; lv->type = TY_POLY; }
  return 1;
}

/* Over a chain whose source the analysis sees yielding two values
   (bs_enum_yield_count), a block of one parameter takes the first value,
   as CRuby binds two yielded values to `|a|`, `_1` or `it`; the chain
   emitters bind a lone parameter to the packed pair. The block takes the
   second value as a parameter of its own: { |a, __spx1| }. `&:m` also
   calls m on the first with the second as its argument:
   { |_spx, __spx1| _spx.m(__spx1) }. So does `&:m` over a call that passes
   its block more than one value itself (sym_block_values), with or without
   its arguments, taking as many: `inject(&:concat)` called acc.concat with
   nothing. */
int desugar_enum_pair_lone_param(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    const char *nm = nt_str(nt, id, "name");
    if (blk < 0 || !nm) continue;
    int sym_n = nt_kind(nt, blk) == NK_BlockNode && sym_proc_block_name(nt, blk) ?
                sym_block_values(c, id) : 0;
    if (recv < 0 && !sym_n && nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
    if (nt_kind(nt, blk) == NK_BlockArgumentNode) {
      changed |= desugar_enum_pair_op_sym(c, id, recv, blk, nm, argc);
      continue;
    }
    if (nt_kind(nt, blk) != NK_BlockNode) continue;
    int bp = nt_ref(nt, blk, "parameters");
    if ((argc && !sym_n) || bp < 0) continue;
    int numbered = nt_kind(nt, bp) == NK_NumberedParametersNode;
    int pn = -1;
    if (numbered) {
      if (nt_int(nt, bp, "maximum", 0) != 1) continue;
    }
    else {
      pn = nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
      if (pn < 0) continue;
      int P = 0, O = 0, Q = 0, kn = 0;
      const int *pre = nt_arr(nt, pn, "requireds", &P);
      nt_arr(nt, pn, "optionals", &O); nt_arr(nt, pn, "posts", &Q); nt_arr(nt, pn, "keywords", &kn);
      if (P != 1 || O || Q || kn || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0 ||
          nt_ref(nt, pn, "block") >= 0 || nt_kind(nt, pre[0]) != NK_RequiredParameterNode) continue;
    }
    TyKind elem;
    if (!sym_n && bs_enum_yield_count(c, recv, nm, 0, &elem) != 2) continue;
    char an[48]; snprintf(an, sizeof an, "__spx1_%s", comp_node_tag(c, blk));
    if (numbered) {
      nt_node_set_int(nt, bp, "maximum", 2);
      nt_node_set_str(nt, bp, "n2", an);
      LocalVar *lv = scope_local_intern(comp_scope_of(c, blk), an);
      if (lv) lv->is_block_param = 1;
      changed = 1;
      continue;
    }
    const char *mn = sym_proc_block_name(nt, blk);
    BsB b = { nt, 1 };
    int base = nt->count;
    int m = sym_n ? sym_n : 2;
    char ans[8][64];
    int P = 0; const int *pre = nt_arr(nt, pn, "requireds", &P);
    int reqs[8] = { pre[0] }, reads[8];
    for (int k = 1; k < m; k++) {
      if (k == 1) snprintf(ans[k], sizeof ans[k], "%s", an);
      else snprintf(ans[k], sizeof ans[k], "__spx%d_%s", k, comp_node_tag(c, blk));
      reqs[k] = bs_new(&b, "RequiredParameterNode");
      if (reqs[k] < 0) return changed;
      nt_node_set_str(nt, reqs[k], "name", ans[k]);
      reads[k] = bs_read(&b, ans[k]);
    }
    if (mn) {
      int call = bs_call(&b, bs_read(&b, nt_str(nt, pre[0], "name")), mn, reads + 1, m - 1);
      int nbody = bs_stmts(&b, &call, 1);
      if (!b.ok || nbody < 0) return changed;
      nt_node_set_ref(nt, blk, "body", nbody);
    }
    nt_node_set_arr(nt, pn, "requireds", reqs, m);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
    for (int k = 1; k < m; k++) {
      LocalVar *lv = scope_local_intern(comp_scope_of(c, blk), ans[k]);
      if (lv) lv->is_block_param = 1;
    }
    changed = 1;
  }
  return changed;
}

/* A block of map and its kin over an Enumerator that yields two values
   binds both: a lone `|x|` takes the first, a lone `|*r|` both, and `&:m`
   calls m on the first with the second as its argument. Which Enumerator a
   local or a parameter holds is known only at run time, so the to_a `hop`
   desugar_enum_method_recv puts in front of call `id` answers what the
   block binds, by the flag the Enumerator carries
   (sp_Enumerator_to_a_yielded), and the block is shaped to take that. */
void enum_hop_yield_view(Compiler *c, int id, int hop) {
  NodeTable *nt = (NodeTable *)c->nt;
  int blk = nt_ref(nt, id, "block");
  const char *nm = nt_str(nt, id, "name");
  if (blk < 0 || !nm || !enum_pair_spread_iter(nm) || enum_pair_source_call(nt, hop)) return;
  /* the builtins' own walks (builtins/, `each { |x| yield x }`) hand the
     packed item on as the one value their block takes */
  const char *sn = comp_scope_of(c, id)->name;
  if (sn && strncmp(sn, "__enum", 6) == 0) return;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc) return;
  if (sym_proc_blockify_op(c, id)) blk = nt_ref(nt, id, "block");
  if (nt_kind(nt, blk) != NK_BlockNode) return;
  const char *mn = sym_proc_block_name(nt, blk);
  BsB b = { nt, 1 };
  int base = nt->count;
  int bp = nt_ref(nt, blk, "parameters");
  int pn = -1;
  int lone_req = 0;   /* |x| */
  int lone_rest = -1; /* |*r| */
  const char *xn = NULL;
  if (bp >= 0 && nt_kind(nt, bp) == NK_NumberedParametersNode)
    lone_req = nt_int(nt, bp, "maximum", 0) == 1;
  else if (bp >= 0 && nt_type(nt, bp) && sp_streq(nt_type(nt, bp), "ItParametersNode"))
    lone_req = 1;
  else if (bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode &&
           (pn = nt_ref(nt, bp, "parameters")) >= 0) {
    int P = 0, O = 0, Q = 0, kn = 0;
    const int *pre = nt_arr(nt, pn, "requireds", &P);
    nt_arr(nt, pn, "optionals", &O); nt_arr(nt, pn, "posts", &Q); nt_arr(nt, pn, "keywords", &kn);
    int rest = nt_ref(nt, pn, "rest");
    if (O || Q || kn || nt_ref(nt, pn, "keyword_rest") >= 0 || nt_ref(nt, pn, "block") >= 0) return;
    if (P == 1 && rest < 0 && nt_kind(nt, pre[0]) == NK_RequiredParameterNode) {
      lone_req = 1;
      xn = nt_str(nt, pre[0], "name");
    }
    else if (P == 0 && rest >= 0 && nt_kind(nt, rest) == NK_RestParameterNode) {
      const char *rn = nt_str(nt, rest, "name");
      if (rn && *rn) lone_rest = rest;
    }
  }
  int req = -1;
  if (mn && xn) {
    /* { |__spa| x = __spa[0]; __spa.length > 1 ? x.m(__spa[1]) : x.m } */
    char an[48]; snprintf(an, sizeof an, "__spa_%s", comp_node_tag(c, blk));
    req = bs_new(&b, "RequiredParameterNode");
    if (req < 0) return;
    nt_node_set_str(nt, req, "name", an);
    int two = sym_proc_call2(&b, xn, mn, bs_index(&b, an, 1));
    int one = bs_call(&b, bs_read(&b, xn), mn, NULL, 0);
    int pro[2] = { bs_write(&b, xn, bs_index(&b, an, 0)),
                   bs_if(&b, bs_len_gt(&b, an, 1), &two, 1, &one, 1) };
    int nbody = bs_stmts(&b, pro, 2);
    if (!b.ok || nbody < 0) return;
    nt_node_set_ref(nt, blk, "body", nbody);
  }
  else if (lone_rest >= 0) {
    /* |*r| -> |r| over the yielded values as one Array */
    req = bs_new(&b, "RequiredParameterNode");
    if (req < 0) return;
    nt_node_set_str(nt, req, "name", nt_str(nt, lone_rest, "name"));
    nt_node_set_ref(nt, pn, "rest", -1);
  }
  else if (lone_req) {
    nt_node_set_str(nt, hop, "enum_yield_view", "first");
    return;
  }
  else return;
  nt_node_set_arr(nt, pn, "requireds", &req, 1);
  nt_node_set_str(nt, hop, "enum_yield_view", "args");
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[blk];
  Scope *bs = comp_scope_of(c, blk);
  LocalVar *lv = scope_local_intern(bs, nt_str(nt, req, "name"));
  if (lv) lv->is_block_param = 1;
  if (mn && xn) scope_local_intern(bs, xn);
}

/* `case x when 0..0.05`, `(..2.5) === x`: a range literal that only matches
   -- a `when` condition, or the receiver of === / include? / member? /
   cover? of a non-Range (the Float representation has no cover?(Range)) --
   is marked, and one with a Float bound then takes the Float representation.
   The integer one truncates that bound: right for iterating `1..5.5`, wrong
   for matching. */
void mark_match_ranges(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  for (int w = 0; w < nt->count; w++) {
    const char *wty = nt_type(nt, w);
    int n = 0, rcv = -1; const int *conds = NULL;
    if (wty && sp_streq(wty, "WhenNode")) conds = nt_arr(nt, w, "conditions", &n);
    else if (nt_kind(nt, w) == NK_CallNode && nt_str(nt, w, "name")) {
      const char *mn = nt_str(nt, w, "name");
      int args = nt_ref(nt, w, "arguments"), an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an != 1) continue;
      if (!(sp_streq(mn, "===") || sp_streq(mn, "include?") || sp_streq(mn, "member?") ||
            (sp_streq(mn, "cover?") && nt_kind(nt, unwrap_parens(c, av[0])) != NK_RangeNode))) continue;
      rcv = unwrap_parens(c, nt_ref(nt, w, "receiver"));
      conds = &rcv; n = 1;
    }
    for (int i = 0; i < n; i++)
      if (nt_kind(nt, conds[i]) == NK_RangeNode) nt_node_set_int(nt, conds[i], "match_only", 1);
  }
}

/* `|_, _, offset|` / `def m(_, _)`: Ruby lets an underscore-prefixed name
   repeat in one parameter list (the body reads the first), and each
   repetition still binds a slot. Give the repeats names of their own, or they
   became two declarations of one C local. */
static int dup_param_taken(char **names, int n, const char *nm) {
  for (int q = 0; q < n; q++) if (sp_streq(names[q], nm)) return 1;
  return 0;
}

int desugar_duplicate_underscore_params(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0, serial = 0;
  NT_FOREACH_KIND(nt, NK_ParametersNode, pn) {
    static const char *const lists[] = { "requireds", "optionals", "posts", "keywords" };
    static const char *const refs[] = { "rest", "keyword_rest", "block" };
    /* every name the list binds, so a generated one is fresh */
    int cap = 8, na = 0;
    char **names = malloc(sizeof(char *) * (size_t)cap);
    for (int li = 0; li < 4; li++) {
      int n = 0; const int *ids = nt_arr(nt, pn, lists[li], &n);
      for (int k = 0; k < n; k++) {
        const char *nm = nt_str(nt, ids[k], "name");
        if (!nm) continue;
        if (na == cap) { cap *= 2; names = realloc(names, sizeof(char *) * (size_t)cap); }
        names[na++] = strdup(nm);
      }
    }
    for (int r = 0; r < 3; r++) {
      int x = nt_ref(nt, pn, refs[r]);
      const char *nm = x >= 0 ? nt_str(nt, x, "name") : NULL;
      if (!nm) continue;
      if (na == cap) { cap *= 2; names = realloc(names, sizeof(char *) * (size_t)cap); }
      names[na++] = strdup(nm);
    }
    int nseen = 0;   /* the underscore names met so far */
    char **seen = malloc(sizeof(char *) * (size_t)(na ? na : 1));
    for (int li = 0; li < 3; li++) {
      int n = 0; const int *ids = nt_arr(nt, pn, lists[li], &n);
      for (int k = 0; k < n; k++) {
        const char *nm = nt_str(nt, ids[k], "name");
        if (!nm || nm[0] != '_') continue;
        if (!dup_param_taken(seen, nseen, nm)) { seen[nseen++] = strdup(nm); continue; }
        char nn[160];
        do snprintf(nn, sizeof nn, "%s__dup%d", nm, ++serial);
        while (dup_param_taken(names, na, nn));
        if (na == cap) { cap *= 2; names = realloc(names, sizeof(char *) * (size_t)cap); }
        names[na++] = strdup(nn);
        nt_set_str(nt, ids[k], "name", nn);
        changed = 1;
      }
    }
    for (int q = 0; q < nseen; q++) free(seen[q]);
    free(seen);
    for (int q = 0; q < na; q++) free(names[q]);
    free(names);
  }
  return changed;
}

/* ---- Encoding's class-level queries ----
 *
 * Spinel strings are UTF-8 (or binary), so the answers are fixed:
 * Encoding.default_internal is nil, default_external is UTF-8, and
 * Encoding.find with a literal name of one of the encodings a string here can
 * carry is that constant. The setters take their value and change nothing. */
/* the text of a literal String or Symbol argument, else NULL */
static const char *sym_or_str_literal(const NodeTable *nt, int node) {
  if (node < 0) return NULL;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_StringNode) {
    const char *u = nt_str(nt, node, "unescaped");
    return u ? u : nt_str(nt, node, "content");
  }
  if (k == NK_SymbolNode) return nt_str(nt, node, "value");
  return NULL;
}

static int enc_const(NodeTable *nt, int like, const char *cname) {
  int par = fwd_new_node_like(nt, like, "ConstantReadNode");
  int cp = fwd_new_node_like(nt, like, "ConstantPathNode");
  if (par < 0 || cp < 0) return -1;
  nt_node_set_str(nt, par, "name", "Encoding");
  nt_node_set_ref(nt, cp, "parent", par);
  nt_node_set_str(nt, cp, "name", cname);
  return cp;
}

int desugar_encoding_queries(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    NodeKind rk = nt_kind(nt, recv);
    if (rk != NK_ConstantReadNode && !(rk == NK_ConstantPathNode && nt_ref(nt, recv, "parent") < 0)) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "Encoding")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int an = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    if (sp_streq(nm, "default_internal") && ac == 0) {
      nt_node_reset(nt, id, "NilNode");
      changed = 1;
    }
    else if (sp_streq(nm, "default_external") && ac == 0) {
      int k = enc_const(nt, id, "UTF_8");
      if (k < 0) continue;
      /* retype the call node in place as that constant path */
      int par = nt_ref(nt, k, "parent");
      nt_node_reset(nt, id, "ConstantPathNode");
      nt_node_set_ref(nt, id, "parent", par);
      nt_node_set_str(nt, id, "name", "UTF_8");
      changed = 1;
    }
    else if ((sp_streq(nm, "default_internal=") || sp_streq(nm, "default_external=")) && ac == 1) {
      /* the assigned value is the expression's value */
      int v = av[0];
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "arguments", -1);
      nt_node_set_ref(nt, id, "receiver", v);
      changed = 1;
    }
    else if (sp_streq(nm, "find") && ac == 1) {
      /* a literal name only: the constant it names */
      const char *lit = sym_or_str_literal(nt, av[0]);
      if (!lit) continue;
      const char *cn = NULL;
      if (!strcasecmp(lit, "utf-8") || !strcasecmp(lit, "utf8")) cn = "UTF_8";
      else if (!strcasecmp(lit, "binary") || !strcasecmp(lit, "ascii-8bit")) cn = "ASCII_8BIT";
      else if (!strcasecmp(lit, "us-ascii") || !strcasecmp(lit, "ascii")) cn = "US_ASCII";
      if (!cn) continue;
      int k = enc_const(nt, id, cn);
      if (k < 0) continue;
      int par = nt_ref(nt, k, "parent");
      nt_node_reset(nt, id, "ConstantPathNode");
      nt_node_set_ref(nt, id, "parent", par);
      nt_node_set_str(nt, id, "name", cn);
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- an alias of an inherited method ----
 *
 * `class HashResultSet < ResultSet; alias_method :next, :next_hash; end`: the
 * subclass's `next` is the inherited `next_hash`. An alias is a name mapping,
 * which the dispatch of `self.next` inside an inherited method (ResultSet#each)
 * never consults -- it saw no override and called ResultSet#next. When the
 * aliased method is not defined in the class itself, the alias becomes a real
 * method that forwards to it:
 *
 *   def next(*a, &b) = next_hash(*a, &b)
 *
 * (An alias of the class's own method keeps the mapping, which also captures
 * the definition in effect at the alias.) */

/* `alias_method "k", "y"`: a String name is the Symbol of its text. Every
   pass that reads alias_method reads Symbol arguments, so the literal
   becomes one. */
int desugar_alias_method_string_names(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "alias_method")) continue;
    int an = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    for (int i = 0; i < ac && i < 2; i++) {
      if (nt_kind(nt, av[i]) != NK_StringNode) continue;
      const char *s = sym_or_str_literal(nt, av[i]);
      if (!s) continue;
      char *name = strdup(s);
      nt_node_reset(nt, av[i], "SymbolNode");
      nt_node_set_str(nt, av[i], "value", name);
      comp_sym_intern(c, name);
      free(name);
      changed = 1;
    }
  }
  return changed;
}

/* A receiverless `alias_method :new, :old` whose value is used */
static int alias_value_call(const NodeTable *nt, int id) {
  if (id < 0 || nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "receiver") >= 0 ||
      nt_ref(nt, id, "block") >= 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "alias_method")) return 0;
  int an = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  return ac == 2 && nt_kind(nt, av[0]) == NK_SymbolNode && nt_kind(nt, av[1]) == NK_SymbolNode &&
         nt_str(nt, av[0], "value") != NULL;
}

/* `r = alias_method :a, :b` / `p(alias_method :a, :b)` in a class body: the
   call returns the new name as a Symbol. Every pass that registers an alias
   reads body statements, so the call moves in front of its statement as one
   and its value becomes the Symbol literal. */
int desugar_alias_method_values(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int m = 0; m < n0; m++) {
    int k = nt_kind(nt, m);
    if (k != NK_ClassNode && k != NK_ModuleNode && k != NK_SingletonClassNode) continue;
    int body = nt_ref(nt, m, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int n = 0; const int *st0 = nt_arr(nt, body, "body", &n);
    if (n == 0) continue;
    int *out = malloc(sizeof(int) * (size_t)(2 * n)), no = 0, moved = 0;
    int *st = malloc(sizeof(int) * (size_t)n);
    memcpy(st, st0, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
      int s = st[i], sk = nt_kind(nt, s), call = -1;
      if (sk == NK_LocalVariableWriteNode || sk == NK_InstanceVariableWriteNode ||
          sk == NK_ClassVariableWriteNode || sk == NK_ConstantWriteNode ||
          sk == NK_GlobalVariableWriteNode) {
        int v = nt_ref(nt, s, "value");
        if (alias_value_call(nt, v)) {
          call = v;
          int sym = fwd_new_node_like(nt, v, "SymbolNode");
          int an = 0; const int *av = nt_arr(nt, nt_ref(nt, v, "arguments"), "arguments", &an);
          nt_node_set_str(nt, sym, "value", nt_str(nt, av[0], "value"));
          nt_node_set_ref(nt, s, "value", sym);
        }
      }
      else if (sk == NK_CallNode && !alias_value_call(nt, s) && nt_ref(nt, s, "arguments") >= 0) {
        int args = nt_ref(nt, s, "arguments");
        int ac = 0; const int *av0 = nt_arr(nt, args, "arguments", &ac);
        int *av = malloc(sizeof(int) * (size_t)(ac > 0 ? ac : 1));
        if (ac > 0) memcpy(av, av0, sizeof(int) * (size_t)ac);
        for (int j = 0; j < ac && call < 0; j++) {
          if (!alias_value_call(nt, av[j])) continue;
          call = av[j];
          int sym = fwd_new_node_like(nt, call, "SymbolNode");
          int cn = 0; const int *cv = nt_arr(nt, nt_ref(nt, call, "arguments"), "arguments", &cn);
          nt_node_set_str(nt, sym, "value", nt_str(nt, cv[0], "value"));
          av[j] = sym;
          nt_node_set_arr(nt, args, "arguments", av, ac);
        }
        free(av);
      }
      if (call >= 0) { out[no++] = call; moved = 1; }
      out[no++] = s;
    }
    if (moved) { nt_node_set_arr(nt, body, "body", out, no); changed = 1; }
    free(out); free(st);
  }
  return changed;
}

/* a read of local `name` shaped like node `like` */
static int local_read_like(NodeTable *nt, int like, const char *name) {
  int r = fwd_new_node_like(nt, like, "LocalVariableReadNode");
  if (r < 0) return -1;
  nt_node_set_str(nt, r, "name", name);
  nt_node_set_int(nt, r, "depth", 0);
  return r;
}

static void alias_pair_names(const NodeTable *nt, int s, const char **nw, const char **od) {
  if (nt_kind(nt, s) == NK_AliasMethodNode) {
    int nn = nt_ref(nt, s, "new_name"), on = nt_ref(nt, s, "old_name");
    *nw = nn >= 0 ? nt_str(nt, nn, "value") : NULL;
    *od = on >= 0 ? nt_str(nt, on, "value") : NULL;
  }
  else if (nt_kind(nt, s) == NK_CallNode && nt_ref(nt, s, "receiver") < 0 &&
           nt_str(nt, s, "name") && sp_streq(nt_str(nt, s, "name"), "alias_method")) {
    int an = nt_ref(nt, s, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    if (ac == 2) { *nw = sym_or_str_literal(nt, av[0]); *od = sym_or_str_literal(nt, av[1]); }
  }
}

static int alias_class_defines(const NodeTable *nt, const char *cls, const char *meth, int n0) {
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || !sp_streq(cn, cls)) continue;
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      if (nt_kind(nt, st[k]) == NK_DefNode && nt_ref(nt, st[k], "receiver") < 0 &&
          nt_str(nt, st[k], "name") && sp_streq(nt_str(nt, st[k], "name"), meth)) return 1;
      /* an alias that binds the name here is a definition of it too: the
         forwarder would call that one, not the inherited body */
      const char *anw = NULL, *aod = NULL;
      alias_pair_names(nt, st[k], &anw, &aod);
      if (anw && sp_streq(anw, meth)) return 1;
    }
  }
  return 0;
}

int desugar_inherited_aliases(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *cls = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cls || nt_ref(nt, m, "superclass") < 0) continue;   /* only a subclass inherits */
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = st[k];
      const char *nw = NULL, *od = NULL;
      alias_pair_names(nt, s, &nw, &od);
      if (!nw || !od || alias_class_defines(nt, cls, od, n0)) continue;
      char nwc[256], odc[256];
      snprintf(nwc, sizeof nwc, "%s", nw); snprintf(odc, sizeof odc, "%s", od);
      /* def <nw>(*spinel_alias_a__, &spinel_alias_b__) = <od>(*..., &...) */
      nt_node_reset(nt, s, "DefNode");
      int ps = fwd_new_node_like(nt, s, "ParametersNode");
      int rp = fwd_new_node_like(nt, s, "RestParameterNode");
      int bp = fwd_new_node_like(nt, s, "BlockParameterNode");
      nt_node_set_str(nt, rp, "name", "spinel_alias_a__");
      nt_node_set_str(nt, bp, "name", "spinel_alias_b__");
      nt_node_set_ref(nt, ps, "rest", rp);
      nt_node_set_ref(nt, ps, "block", bp);
      int call = fwd_new_node_like(nt, s, "CallNode");
      int args = fwd_new_node_like(nt, s, "ArgumentsNode");
      int sp = fwd_new_node_like(nt, s, "SplatNode");
      nt_node_set_ref(nt, sp, "expression", local_read_like(nt, s, "spinel_alias_a__"));
      nt_node_set_arr(nt, args, "arguments", &sp, 1);
      int ba = fwd_new_node_like(nt, s, "BlockArgumentNode");
      nt_node_set_ref(nt, ba, "expression", local_read_like(nt, s, "spinel_alias_b__"));
      nt_node_set_str(nt, call, "name", odc);
      nt_node_set_ref(nt, call, "arguments", args);
      nt_node_set_ref(nt, call, "block", ba);
      int bd = fwd_new_node_like(nt, s, "StatementsNode");
      nt_node_set_arr(nt, bd, "body", &call, 1);
      nt_node_set_str(nt, s, "name", nwc);
      nt_node_set_ref(nt, s, "parameters", ps);
      nt_node_set_ref(nt, s, "body", bd);
      nt_node_set_ref(nt, s, "receiver", -1);
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- an alias of an attribute reader the body later redefines ----
 *
 * `attr_reader :v; alias v1 v; def v = 40`: v1 is the reader, but a name
 * mapping resolves to the later `def`. The alias becomes `def v1 = @v`. */
static int ra_body_declares_reader(const NodeTable *nt, const int *st, int upto, const char *meth) {
  int in_effect = 0;
  for (int k = 0; k < upto; k++) {
    if (nt_kind(nt, st[k]) == NK_DefNode && nt_ref(nt, st[k], "receiver") < 0 &&
        nt_str(nt, st[k], "name") && sp_streq(nt_str(nt, st[k], "name"), meth)) {
      in_effect = 0;
      continue;
    }
    if (nt_kind(nt, st[k]) != NK_CallNode || nt_ref(nt, st[k], "receiver") >= 0) continue;
    const char *cn = nt_str(nt, st[k], "name");
    if (!cn || !(sp_streq(cn, "attr_reader") || sp_streq(cn, "attr_accessor"))) continue;
    int an = nt_ref(nt, st[k], "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    for (int i = 0; i < ac; i++) {
      const char *a = sym_or_str_literal(nt, av[i]);
      if (a && sp_streq(a, meth)) in_effect = 1;
    }
  }
  return in_effect;
}

int desugar_reader_aliases_before_redef(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode && nt_kind(nt, m) != NK_ModuleNode) continue;
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = st[k];
      const char *nw = NULL, *od = NULL;
      alias_pair_names(nt, s, &nw, &od);
      if (!nw || !od || !ra_body_declares_reader(nt, st, k, od)) continue;
      int later = 0;
      for (int j = k + 1; j < n && !later; j++)
        if (nt_kind(nt, st[j]) == NK_DefNode && nt_ref(nt, st[j], "receiver") < 0 &&
            nt_str(nt, st[j], "name") && sp_streq(nt_str(nt, st[j], "name"), od)) later = 1;
      if (!later) continue;
      char *nwc = strdup(nw), *ivc = malloc(strlen(od) + 2);
      ivc[0] = '@'; strcpy(ivc + 1, od);
      nt_node_reset(nt, s, "DefNode");
      int rd = fwd_new_node_like(nt, s, "InstanceVariableReadNode");
      nt_node_set_str(nt, rd, "name", ivc);
      int bd = fwd_new_node_like(nt, s, "StatementsNode");
      nt_node_set_arr(nt, bd, "body", &rd, 1);
      nt_node_set_str(nt, s, "name", nwc);
      nt_node_set_ref(nt, s, "parameters", -1);
      nt_node_set_ref(nt, s, "body", bd);
      nt_node_set_ref(nt, s, "receiver", -1);
      free(nwc); free(ivc);
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- a block parameter the block body assigns ----
 *
 * `prepare(sql) do |stmt| stmt = build_result_set(stmt); ... end`: the body
 * rebinds its own parameter to a value of another class. A block parameter's
 * type is its yield's, so the write was forced into the yielded type and the
 * conversion raised TypeError. Such a parameter becomes an ordinary local fed
 * from a renamed parameter (`|stmt__bpin| stmt = stmt__bpin; ...`), whose
 * writes widen it like any local's. */
static int rbp_writes(const NodeTable *nt, int node, const char *name, int level) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return 0;
  if ((k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
       k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode ||
       k == NK_LocalVariableTargetNode) &&
      nt_str(nt, node, "name") && sp_streq(nt_str(nt, node, "name"), name) &&
      nt_int(nt, node, "depth", 0) == level) {
    /* `r = nil` only makes the parameter nullable, which it already widens to */
    int v = k == NK_LocalVariableWriteNode ? nt_ref(nt, node, "value") : -1;
    if (!(v >= 0 && nt_kind(nt, v) == NK_NilNode)) return 1;
  }
  int inner = (k == NK_BlockNode || k == NK_LambdaNode) ? level + 1 : level;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (rbp_writes(nt, nt_ref_at(nt, node, i), name, inner)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) if (rbp_writes(nt, ids[j], name, inner)) return 1;
  }
  return 0;
}

static int rbp_captured(const NodeTable *nt, int node, const char *name, int level) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return 0;
  if (level > 0 && (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode ||
                    k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableOrWriteNode ||
                    k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableTargetNode) &&
      nt_str(nt, node, "name") && sp_streq(nt_str(nt, node, "name"), name) &&
      nt_int(nt, node, "depth", 0) == level)
    return 1;
  int inner = (k == NK_BlockNode || k == NK_LambdaNode) ? level + 1 : level;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (rbp_captured(nt, nt_ref_at(nt, node, i), name, inner)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) if (rbp_captured(nt, ids[j], name, inner)) return 1;
  }
  return 0;
}

int desugar_reassigned_block_params(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int b = 0; b < n0; b++) {
    if (nt_kind(nt, b) != NK_BlockNode) continue;
    int bps = nt_ref(nt, b, "parameters");
    if (bps < 0 || nt_kind(nt, bps) != NK_BlockParametersNode) continue;
    int ps = nt_ref(nt, bps, "parameters");
    if (ps < 0) continue;
    int body = nt_ref(nt, b, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int rn = 0; const int *rq = nt_arr(nt, ps, "requireds", &rn);
    int *pre = NULL; int npre = 0;
    for (int i = 0; i < rn; i++) {
      if (nt_kind(nt, rq[i]) != NK_RequiredParameterNode) continue;
      const char *pn = nt_str(nt, rq[i], "name");
      if (!pn || pn[0] == '_' || !rbp_writes(nt, body, pn, 0)) continue;
      /* a parameter a nested block or lambda captures keeps its one cell per
         iteration, which only a block parameter has */
      if (rbp_captured(nt, body, pn, 0)) continue;
      char orig[160], renamed[176];
      snprintf(orig, sizeof orig, "%s", pn);
      snprintf(renamed, sizeof renamed, "%s__bpin", orig);
      nt_set_str(nt, rq[i], "name", renamed);
      int w = fwd_new_node_like(nt, rq[i], "LocalVariableWriteNode");
      nt_node_set_str(nt, w, "name", orig);
      nt_node_set_int(nt, w, "depth", 0);
      int rd = fwd_new_node_like(nt, rq[i], "LocalVariableReadNode");
      nt_node_set_str(nt, rd, "name", renamed);
      nt_node_set_int(nt, rd, "depth", 0);
      nt_node_set_ref(nt, w, "value", rd);
      /* the iteration's setup, which a redo does not re-run
         (block_param_rebind_len) */
      nt_node_set_int(nt, w, "bp_rebind", 1);
      pre = realloc(pre, sizeof(int) * (size_t)(npre + 1));
      pre[npre++] = w;
    }
    if (npre > 0) {
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      int *out = malloc(sizeof(int) * (size_t)(bn + npre));
      memcpy(out, pre, sizeof(int) * (size_t)npre);
      memcpy(out + npre, bs, sizeof(int) * (size_t)bn);
      nt_node_set_arr(nt, body, "body", out, bn + npre);
      free(out);
      changed = 1;
    }
    free(pre);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- Class.new / Module.new with a block ----
 *
 * `Point = Class.new(Base) do ... end` is a class definition spelled as a
 * call; it becomes `class Point < Base; ...; end`. An anonymous one used as a
 * value (`k = Class.new do ... end`) becomes a named class defined just
 * before the top-level statement that builds it, when its body reads none of
 * the surrounding method's locals -- the class is then the same every time
 * the code runs. One whose body does read them is a class built at run time:
 * the call raises NotImplementedError when reached, and its methods no longer
 * leak into the enclosing class (they used to replace that class's own,
 * `initialize` included). */
/* a String literal node shaped like `like` */
static int str_node_like(NodeTable *nt, int like, const char *s) {
  int n = fwd_new_node_like(nt, like, "StringNode");
  if (n < 0) return -1;
  nt_node_set_str(nt, n, "unescaped", s);
  nt_node_set_str(nt, n, "content", s);
  return n;
}

static int cn_reads_outer_local(const NodeTable *nt, int node, int level) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode) return 0;                 /* a def sees no outer local */
  if (k == NK_LocalVariableReadNode || k == NK_LocalVariableWriteNode ||
      k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
      k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableTargetNode) {
    if (nt_int(nt, node, "depth", 0) > level) return 1;
  }
  int inner = (k == NK_BlockNode || k == NK_LambdaNode) ? level + 1 : level;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (cn_reads_outer_local(nt, nt_ref_at(nt, node, i), inner)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) if (cn_reads_outer_local(nt, ids[j], inner)) return 1;
  }
  return 0;
}

static int cn_contains(const NodeTable *nt, int root, int target) {
  if (root < 0) return 0;
  if (root == target) return 1;
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (cn_contains(nt, nt_ref_at(nt, root, i), target)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, root, i, &cnt);
    for (int j = 0; j < cnt; j++) if (cn_contains(nt, ids[j], target)) return 1;
  }
  return 0;
}

/* the class body a Class.new block becomes: its statements, as a StatementsNode */
static int cn_body(NodeTable *nt, int blk) {
  int bb = nt_ref(nt, blk, "body");
  if (bb >= 0 && nt_kind(nt, bb) == NK_StatementsNode) return bb;
  int st = fwd_new_node_like(nt, blk, "StatementsNode");
  if (st < 0) return -1;
  if (bb >= 0) nt_node_set_arr(nt, st, "body", &bb, 1);
  else nt_node_set_arr(nt, st, "body", NULL, 0);
  return st;
}

static int cn_make_class(NodeTable *nt, int like, int is_module, const char *name, int super_node, int body) {
  int cls = fwd_new_node_like(nt, like, is_module ? "ModuleNode" : "ClassNode");
  int cp = fwd_new_node_like(nt, like, "ConstantReadNode");
  if (cls < 0 || cp < 0) return -1;
  nt_node_set_str(nt, cp, "name", name);
  nt_node_set_ref(nt, cls, "constant_path", cp);
  if (!is_module) nt_node_set_ref(nt, cls, "superclass", super_node);
  nt_node_set_ref(nt, cls, "body", body);
  return cls;
}

static int cn_has_def(const NodeTable *nt, int node) {
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode) return 1;
  if (k == NK_ClassNode || k == NK_ModuleNode) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (cn_has_def(nt, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) if (cn_has_def(nt, ids[j])) return 1;
  }
  return 0;
}

static void cn_neutralize(NodeTable *nt, int node) {
  if (node < 0) return;
  int nr = nt_num_refs(nt, node);
  int *kids = NULL; int nk = 0, cap = 0;
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    if (ch < 0) continue;
    if (nk == cap) { cap = cap ? cap * 2 : 8; kids = realloc(kids, sizeof(int) * (size_t)cap); }
    kids[nk++] = ch;
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) {
      if (nk == cap) { cap = cap ? cap * 2 : 8; kids = realloc(kids, sizeof(int) * (size_t)cap); }
      kids[nk++] = ids[j];
    }
  }
  for (int i = 0; i < nk; i++) cn_neutralize(nt, kids[i]);
  free(kids);
  nt_node_reset(nt, node, "NilNode");
}

int desugar_class_new_blocks(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0, serial = 0;
  int *parent = malloc(sizeof(int) * (size_t)(n0 > 0 ? n0 : 1));
  if (!parent) return 0;
  for (int i = 0; i < n0; i++) parent[i] = -1;
  for (int p = 0; p < n0; p++) {
    int nr = nt_num_refs(nt, p);
    for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, p, i); if (ch >= 0 && ch < n0) parent[ch] = p; }
    int na = nt_num_arrs(nt, p);
    for (int i = 0; i < na; i++) {
      int cnt = 0; const int *ids = nt_arr_at(nt, p, i, &cnt);
      for (int j = 0; j < cnt; j++) if (ids[j] >= 0 && ids[j] < n0) parent[ids[j]] = p;
    }
  }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    /* `k.class_eval do def m; end end` on a class held in a variable: the
       defs shape a class that exists only at run time, and left in place
       they would land in the enclosing class */
    if (nm && recv >= 0 && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode &&
        (sp_streq(nm, "class_eval") || sp_streq(nm, "module_eval") || sp_streq(nm, "class_exec") ||
         sp_streq(nm, "module_exec") || sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec"))) {
      NodeKind rk0 = nt_kind(nt, recv);
      /* `self.class.class_eval` names the enclosing class, as a constant does */
      int self_class = 0;
      if (rk0 == NK_CallNode) {
        const char *rnm = nt_str(nt, recv, "name");
        int rr = nt_ref(nt, recv, "receiver");
        self_class = rnm && sp_streq(rnm, "class") && nt_ref(nt, recv, "arguments") < 0 &&
                     (rr < 0 || nt_kind(nt, rr) == NK_SelfNode);
      }
      /* `base.class_eval do def m; end end` in a module's `self.included(base)`
         or `self.extended(base)` hook: base is the including class, which the
         hook inliner (desugar_included_hooks) substitutes at the include site,
         so the defs land on that class; leave the call for it */
      int hook_base = 0;
      if (rk0 == NK_LocalVariableReadNode) {
        const char *bn = nt_str(nt, recv, "name");
        int d = parent[id];
        while (d >= 0 && nt_kind(nt, d) != NK_DefNode) d = parent[d];
        if (d >= 0 && bn) {
          const char *dn = nt_str(nt, d, "name");
          int dr = nt_ref(nt, d, "receiver");
          int ps = nt_ref(nt, d, "parameters");
          int rn = 0; const int *rq = ps >= 0 ? nt_arr(nt, ps, "requireds", &rn) : NULL;
          hook_base = dn && (sp_streq(dn, "included") || sp_streq(dn, "extended")) &&
                      dr >= 0 && nt_kind(nt, dr) == NK_SelfNode && rn == 1 &&
                      nt_str(nt, rq[0], "name") && sp_streq(nt_str(nt, rq[0], "name"), bn);
        }
      }
      if (rk0 != NK_ConstantReadNode && rk0 != NK_ConstantPathNode && rk0 != NK_SelfNode &&
          !self_class && !hook_base && cn_has_def(nt, nt_ref(nt, blk, "body"))) {
        cn_neutralize(nt, blk);
        nt_node_reset(nt, id, "CallNode");
        nt_node_set_str(nt, id, "name", "raise");
        int args = fwd_new_node_like(nt, id, "ArgumentsNode");
        int ex = fwd_new_node_like(nt, id, "ConstantReadNode");
        nt_node_set_str(nt, ex, "name", "NotImplementedError");
        int msg = str_node_like(nt, id, "spinel: defining methods on a class held in a variable "
                                 "(class_eval with a def) is not supported");
        int av2[2] = { ex, msg };
        nt_node_set_arr(nt, args, "arguments", av2, 2);
        nt_node_set_ref(nt, id, "arguments", args);
        changed = 1;
        continue;
      }
    }
    if (!nm || !sp_streq(nm, "new") || recv < 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    if (nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    int is_module = rn && sp_streq(rn, "Module");
    if (!rn || (!is_module && !sp_streq(rn, "Class"))) continue;
    int an = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    int super_node = (!is_module && ac >= 1) ? av[0] : -1;
    /* a superclass the program names is static; one held in a variable is
       a class built at run time */
    int static_super = super_node < 0 || nt_kind(nt, super_node) == NK_ConstantReadNode ||
                       nt_kind(nt, super_node) == NK_ConstantPathNode;
    int body = cn_body(nt, blk);
    if (body < 0) continue;
    int par = parent[id];
    /* a class body opens a scope of its own, a block does not: a body reading
       the surrounding locals stays a block */
    int reads_outer = cn_reads_outer_local(nt, body, 0);
    if (static_super && !reads_outer && par >= 0 && nt_kind(nt, par) == NK_ConstantWriteNode &&
        nt_ref(nt, par, "value") == id) {
      /* Name = Class.new(...) do ... end  ->  class Name < ...; ...; end */
      const char *cn = nt_str(nt, par, "name");
      char name[256]; snprintf(name, sizeof name, "%s", cn ? cn : "SpinelAnon");
      nt_node_reset(nt, par, is_module ? "ModuleNode" : "ClassNode");
      int cp = fwd_new_node_like(nt, par, "ConstantReadNode");
      nt_node_set_str(nt, cp, "name", name);
      nt_node_set_ref(nt, par, "constant_path", cp);
      if (!is_module) nt_node_set_ref(nt, par, "superclass", super_node);
      nt_node_set_ref(nt, par, "body", body);
      changed = 1;
      continue;
    }
    /* the anonymous class is defined in the nearest enclosing class or
       module body (the top level when there is none), where its superclass
       and body constants resolve as they would around the call */
    int host_st = nt_ref(nt, nt->root_id, "statements");
    for (int a = par; a >= 0; a = parent[a]) {
      NodeKind ak = nt_kind(nt, a);
      if (ak == NK_ClassNode || ak == NK_ModuleNode) { host_st = nt_ref(nt, a, "body"); break; }
    }
    if (static_super && !reads_outer && host_st >= 0 && nt_kind(nt, host_st) == NK_StatementsNode) {
      /* an anonymous class that is the same every time: name it */
      int rn2 = 0; const int *rs = nt_arr(nt, host_st, "body", &rn2);
      int at = -1;
      for (int k = 0; k < rn2 && at < 0; k++) if (cn_contains(nt, rs[k], id)) at = k;
      if (at < 0) continue;
      char name[64]; snprintf(name, sizeof name, "SpinelAnonClass%d", ++serial);
      int cls = cn_make_class(nt, id, is_module, name, super_node, body);
      if (cls < 0) continue;
      rs = nt_arr(nt, host_st, "body", &rn2);
      int *out = malloc(sizeof(int) * (size_t)(rn2 + 1));
      memcpy(out, rs, sizeof(int) * (size_t)at);
      out[at] = cls;
      memcpy(out + at + 1, rs + at, sizeof(int) * (size_t)(rn2 - at));
      nt_node_set_arr(nt, host_st, "body", out, rn2 + 1);
      free(out);
      nt_node_reset(nt, id, "ConstantReadNode");
      nt_node_set_str(nt, id, "name", name);
      changed = 1;
      continue;
    }
    /* built from the running method's values: not a class the program has.
       Every node of the dropped body goes inert too -- passes that walk the
       whole table by node kind would otherwise still find its defs and
       ivar writes and give them to the top level. */
    cn_neutralize(nt, blk);
    nt_node_reset(nt, id, "CallNode");
    nt_node_set_str(nt, id, "name", "raise");
    int args = fwd_new_node_like(nt, id, "ArgumentsNode");
    int ex = fwd_new_node_like(nt, id, "ConstantReadNode");
    nt_node_set_str(nt, ex, "name", "NotImplementedError");
    int msg = str_node_like(nt, id, "spinel: a class built at run time (Class.new with a block that reads the "
                             "surrounding method's locals) is not supported");
    int av2[2] = { ex, msg };
    nt_node_set_arr(nt, args, "arguments", av2, 2);
    nt_node_set_ref(nt, id, "arguments", args);
    changed = 1;
  }
  free(parent);
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- Module.included hooks ----
 *
 * `include M` calls M.included(base) as the class body runs, and the hook's
 * usual work is to shape the includer: `base.class_eval { layout ... }`,
 * `base.extend(ClassMethods)`, `base.attr_accessor :x`. The includer is known
 * at each include site, so the hook's body is spliced in right after the
 * include, with `base` as the class the body belongs to:
 *
 *   base.class_eval do BODY end   ->  BODY           (self is the class)
 *   base.m(args)                  ->  m(args)        (a class-body call)
 *   any other read of base        ->  the class's constant
 *
 * The hook method itself stays on M for anything that calls it by name. */
static int incl_find_hook_named(const NodeTable *nt, const char *mn, int n0, const char **param,
                                const char *hook_name);
static int incl_find_hook(const NodeTable *nt, const char *mn, int n0, const char **param) {
  return incl_find_hook_named(nt, mn, n0, param, "included");
}
static int incl_find_hook_named(const NodeTable *nt, const char *mn, int n0, const char **param,
                                const char *hook_name) {
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ModuleNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!nm || !sp_streq(nm, mn)) continue;
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int d = st[k];
      if (nt_kind(nt, d) != NK_DefNode) continue;
      const char *dn = nt_str(nt, d, "name");
      int r = nt_ref(nt, d, "receiver");
      if (!dn || !sp_streq(dn, hook_name) || r < 0 || nt_kind(nt, r) != NK_SelfNode) continue;
      int ps = nt_ref(nt, d, "parameters");
      int rn = 0; const int *rq = ps >= 0 ? nt_arr(nt, ps, "requireds", &rn) : NULL;
      if (rn != 1) continue;
      *param = nt_str(nt, rq[0], "name");
      return *param ? d : -1;
    }
  }
  return -1;
}

/* Rewrite the cloned hook body in place: see the header comment. */
static void incl_subst(NodeTable *nt, int node, const char *param, const char *cls_name) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return;
  if (k == NK_CallNode) {
    int r = nt_ref(nt, node, "receiver");
    const char *cm = nt_str(nt, node, "name");
    /* the module's own introspection reads the class: `base.name` */
    int introspect = cm && (sp_streq(cm, "name") || sp_streq(cm, "to_s") || sp_streq(cm, "inspect") ||
                            sp_streq(cm, "ancestors") || sp_streq(cm, "superclass") ||
                            sp_streq(cm, "instance_methods") || sp_streq(cm, "const_get"));
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode && !introspect &&
        nt_str(nt, r, "name") && sp_streq(nt_str(nt, r, "name"), param))
      nt_node_set_ref(nt, node, "receiver", -1);
  }
  if (k == NK_LocalVariableReadNode && nt_str(nt, node, "name") &&
      sp_streq(nt_str(nt, node, "name"), param)) {
    if (!cls_name) { nt_node_reset(nt, node, "SelfNode"); return; }
    nt_node_reset(nt, node, "ConstantReadNode");
    nt_node_set_str(nt, node, "name", cls_name);
    return;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) incl_subst(nt, nt_ref_at(nt, node, i), param, cls_name);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0;
    const int *ids = nt_arr_at(nt, node, i, &cnt);
    int *cp = cnt > 0 ? malloc(sizeof(int) * (size_t)cnt) : NULL;
    if (cnt > 0 && !cp) continue;
    if (cnt > 0) memcpy(cp, ids, sizeof(int) * (size_t)cnt);
    for (int j = 0; j < cnt; j++) incl_subst(nt, cp[j], param, cls_name);
    free(cp);
  }
}

/* The statements a hook statement becomes: a bare class_eval-family call
   with a block is its block's body; anything else is itself. */
static void incl_emit_stmt(const NodeTable *nt, int s, int **out, int *no, int *cap) {
  if (nt_kind(nt, s) == NK_CallNode && nt_ref(nt, s, "receiver") < 0) {
    const char *nm = nt_str(nt, s, "name");
    int blk = nt_ref(nt, s, "block");
    if (nm && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode &&
        (sp_streq(nm, "class_eval") || sp_streq(nm, "class_exec") || sp_streq(nm, "module_eval") ||
         sp_streq(nm, "module_exec") || sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec"))) {
      int bb = nt_ref(nt, blk, "body");
      int bn = 0; const int *bs = bb >= 0 && nt_kind(nt, bb) == NK_StatementsNode ? nt_arr(nt, bb, "body", &bn) : NULL;
      for (int j = 0; j < bn; j++) incl_emit_stmt(nt, bs[j], out, no, cap);
      return;
    }
  }
  if (*no == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *out = realloc(*out, sizeof(int) * (size_t)*cap);
    if (!*out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*out)[(*no)++] = s;
}

int desugar_included_hooks(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int cn = 0; cn < n0; cn++) {
    NodeKind ck = nt_kind(nt, cn);
    int is_mod = ck == NK_ModuleNode;
    const char *cls = NULL;
    int body = -1;
    if (ck == NK_ClassNode || is_mod) {
      int cp = nt_ref(nt, cn, "constant_path");
      cls = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      body = nt_ref(nt, cn, "body");
    }
    else if (ck == NK_ConstantWriteNode) {   /* Name = Struct.new(...) do ... end */
      cls = nt_str(nt, cn, "name");
      body = class_def_body(c, cn);
    }
    else if (ck == NK_LocalVariableWriteNode) {   /* k = Struct.new(...) do ... end: base is self */
      body = class_def_body(c, cn);
    }
    else continue;
    if ((!cls && ck != NK_LocalVariableWriteNode) || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int n = 0; const int *st = nt_arr(nt, body, "body", &n);
    int *out = NULL; int no = 0, cap = 0, spliced = 0;
    for (int k = 0; k < n; k++) {
      int s = st[k];
      incl_emit_stmt(nt, s, &out, &no, &cap);   /* the statement itself */
      /* incl_emit_stmt only unwraps receiverless class_eval; a plain
         statement passes through, so this is the statement as written */
      if (nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) continue;
      const char *nm = nt_str(nt, s, "name");
      /* `extend M` runs M.extended(base) the same way */
      int is_ext = nm && sp_streq(nm, "extend");
      if (!nm || (!sp_streq(nm, "include") && !is_ext)) continue;
      if (is_mod && !is_ext) continue;
      int an = nt_ref(nt, s, "arguments");
      int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      for (int j = 0; j < ac; j++) {
        NodeKind ak = nt_kind(nt, av[j]);
        if (ak != NK_ConstantReadNode && ak != NK_ConstantPathNode) continue;
        const char *param = NULL;
        int hook = incl_find_hook_named(nt, nt_str(nt, av[j], "name"), n0, &param,
                                        is_ext ? "extended" : "included");
        if (hook < 0) continue;
        int hb = nt_ref(nt, hook, "body");
        if (hb < 0) continue;
        int clone = nt_clone_subtree(nt, hb);
        if (clone < 0) continue;
        incl_subst(nt, clone, param, cls);
        int hn = 0; const int *hs = nt_kind(nt, clone) == NK_StatementsNode ? nt_arr(nt, clone, "body", &hn) : NULL;
        if (!hs) { incl_emit_stmt(nt, clone, &out, &no, &cap); spliced = 1; continue; }
        int *hcp = hn > 0 ? malloc(sizeof(int) * (size_t)hn) : NULL;
        if (hn > 0 && !hcp) continue;
        if (hn > 0) memcpy(hcp, hs, sizeof(int) * (size_t)hn);
        for (int q = 0; q < hn; q++) incl_emit_stmt(nt, hcp[q], &out, &no, &cap);
        free(hcp);
        spliced = 1;
      }
    }
    if (spliced) {
      nt_node_set_arr(nt, body, "body", out, no);
      changed = 1;
    }
    free(out);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- `const_get :Name` on self in a class method ----------------------------
 *
 *   class GObject
 *     class << self
 *       def ffi_managed_struct = const_get(:ManagedStruct)
 *     end
 *   end
 *   class Image < GObject
 *     class ManagedStruct < GObject::ManagedStruct; end
 *   end
 *
 * The literal name resolves against whichever class self is at run time, so
 * each class that defines `Name` in its body (and the method's own class) gets
 *
 *   def self.__spinel_cg_Name = Name
 *
 * and the call becomes `__spinel_cg_Name` on self: class-method dispatch then
 * picks the nearest definition, as the ancestor lookup would. */

static void scg_add_def_body(NodeTable *nt, int cls, const char *cname, int like, int raising,
                             const char *mname);
static void scg_add_def(NodeTable *nt, int cls, const char *cname, int like, const char *mname) {
  scg_add_def_body(nt, cls, cname, like, 0, mname);
}
/* `raising`: 0 reads the constant, 1 raises NameError; 2 and 3 are the
   `defined?` twin, `__spinel_cd_Name`, answering "constant" or nil */
static void scg_add_def_body(NodeTable *nt, int cls, const char *cname, int like, int raising,
                             const char *mname) {
  int body = nt_ref(nt, cls, "body");
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) {
    int nb = fwd_new_node_like(nt, like, "StatementsNode");
    nt_node_set_arr(nt, nb, "body", NULL, 0);
    nt_node_set_ref(nt, cls, "body", nb);
    body = nb;
  }
  int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
  for (int k = 0; k < bn; k++)
    if (nt_kind(nt, bs[k]) == NK_DefNode && nt_str(nt, bs[k], "name") &&
        sp_streq(nt_str(nt, bs[k], "name"), mname)) return;
  int d = fwd_new_node_like(nt, like, "DefNode");
  int db = fwd_new_node_like(nt, like, "StatementsNode");
  if (raising == 2) {
    int sn = str_node_like(nt, like, "constant");
    nt_node_set_int(nt, sn, "fzl", 1);   /* defined?'s answers are frozen */
    nt_node_set_arr(nt, db, "body", &sn, 1);
  }
  else if (raising == 3) {
    /* nil, typed as defined?'s answer is (a bare nil would make the twin
       return nothing while its siblings return a String) */
    int dn = fwd_new_node_like(nt, like, "DefinedNode");
    int cr = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, cr, "name", "SpinelNoSuchConstant__");
    nt_node_set_ref(nt, dn, "value", cr);
    nt_node_set_arr(nt, db, "body", &dn, 1);
  }
  else if (raising) {
    /* raise NameError, "uninitialized constant X"; nil */
    int rc = fwd_new_node_like(nt, like, "CallNode");
    int ra = fwd_new_node_like(nt, like, "ArgumentsNode");
    int ne = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, ne, "name", "NameError");
    char msg[300]; snprintf(msg, sizeof msg, "uninitialized constant %s", cname);
    int av[2] = { ne, str_node_like(nt, like, msg) };
    nt_node_set_arr(nt, ra, "arguments", av, 2);
    nt_node_set_str(nt, rc, "name", "raise");
    nt_node_set_ref(nt, rc, "arguments", ra);
    int nl = fwd_new_node_like(nt, like, "NilNode");
    int bb[2] = { rc, nl };
    nt_node_set_arr(nt, db, "body", bb, 2);
  }
  else {
    int cr = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, cr, "name", cname);
    nt_node_set_arr(nt, db, "body", &cr, 1);
  }
  nt_node_set_str(nt, d, "name", mname);
  nt_node_set_ref(nt, d, "receiver", fwd_new_node_like(nt, like, "SelfNode"));
  nt_node_set_ref(nt, d, "body", db);
  int *out = malloc(sizeof(int) * (size_t)(bn + 1));
  out[0] = d;
  if (bn) memcpy(out + 1, bs, sizeof(int) * (size_t)bn);
  nt_node_set_arr(nt, body, "body", out, bn + 1);
  free(out);
}

static int scg_stmts_define(const NodeTable *nt, int body, const char *cname, int depth);
static int scg_body_defines(const NodeTable *nt, int cls, const char *cname) {
  return scg_stmts_define(nt, nt_ref(nt, cls, "body"), cname, 0);
}
static int scg_stmts_define(const NodeTable *nt, int body, const char *cname, int depth) {
  if (depth > 8) return 0;
  int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    NodeKind sk = nt_kind(nt, st[k]);
    if (sk == NK_IfNode || sk == NK_UnlessNode) {
      if (scg_stmts_define(nt, nt_ref(nt, st[k], "statements"), cname, depth + 1)) return 1;
      int e = nt_ref(nt, st[k], sk == NK_IfNode ? "subsequent" : "else_clause");
      if (e >= 0 && nt_kind(nt, e) == NK_ElseNode && scg_stmts_define(nt, nt_ref(nt, e, "statements"), cname, depth + 1)) return 1;
      continue;
    }
    const char *cn = NULL;
    if (sk == NK_ConstantWriteNode || sk == NK_ConstantOrWriteNode) cn = nt_str(nt, st[k], "name");
    else if (sk == NK_ClassNode || sk == NK_ModuleNode) {
      int ccp = nt_ref(nt, st[k], "constant_path");
      cn = ccp >= 0 ? nt_str(nt, ccp, "name") : NULL;
    }
    if (cn && sp_streq(cn, cname)) return 1;
  }
  return 0;
}

/* Does the body make `cname` a private constant (`private_constant :cname`)?
   Read through a scope, `self::cname`, such a constant raises NameError. */
static int scg_body_privatizes(const NodeTable *nt, int cls, const char *cname) {
  int body = nt_ref(nt, cls, "body");
  int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    if (nt_kind(nt, st[k]) != NK_CallNode || nt_ref(nt, st[k], "receiver") >= 0) continue;
    const char *cn = nt_str(nt, st[k], "name");
    if (!cn || !sp_streq(cn, "private_constant")) continue;
    int args = nt_ref(nt, st[k], "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    for (int a = 0; a < an; a++) {
      const char *v = sym_or_str_literal(nt, av[a]);
      if (v && sp_streq(v, cname)) return 1;
    }
  }
  return 0;
}

/* The name a superclass or an included module is written with: the last
   segment of a qualified one (`Outer::Parent`), as class bodies are matched
   by name here. */
static const char *scg_const_name(const NodeTable *nt, int n) {
  if (n < 0) return NULL;
  NodeKind k = nt_kind(nt, n);
  return k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(nt, n, "name") : NULL;
}

/* Where the lookup of `cname` from a class or module named `nm` ends: 1 when
   it or a module it includes defines it, -1 when the nearest definition is
   private and `honor_private` (a scoped read, `self::cname`, raises there),
   0 when none does. A private_constant in any reopening counts, and the
   module included last is searched first, as Ruby's ancestors run. Names
   match by their last segment, so `Ns::X` including `Y` that includes `X`
   reads as a cycle: a name already walked (`seen`) is not walked again. */
typedef struct { const char *nm[64]; int n; } ScgSeen;
static int scg_named_lookup(const NodeTable *nt, const char *nm, const char *cname, int n0,
                            int honor_private, ScgSeen *seen) {
  for (int q = 0; q < seen->n; q++) if (sp_streq(seen->nm[q], nm)) return 0;
  if (seen->n >= 64) return 0;
  seen->nm[seen->n++] = nm;
  int defined = 0, priv = 0;
  for (int m = 0; m < n0; m++) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ClassNode && mk != NK_ModuleNode) continue;
    const char *mn = scg_const_name(nt, nt_ref(nt, m, "constant_path"));
    if (!mn || !sp_streq(mn, nm)) continue;
    if (scg_body_defines(nt, m, cname)) defined = 1;
    if (scg_body_privatizes(nt, m, cname)) priv = 1;
  }
  if (defined) return honor_private && priv ? -1 : 1;
  for (int m = n0 - 1; m >= 0; m--) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ClassNode && mk != NK_ModuleNode) continue;
    const char *mn = scg_const_name(nt, nt_ref(nt, m, "constant_path"));
    if (!mn || !sp_streq(mn, nm)) continue;
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = n - 1; k >= 0; k--) {
      if (nt_kind(nt, st[k]) != NK_CallNode || nt_ref(nt, st[k], "receiver") >= 0) continue;
      const char *cn = nt_str(nt, st[k], "name");
      if (!cn || !sp_streq(cn, "include")) continue;
      int args = nt_ref(nt, st[k], "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      for (int a = 0; a < an; a++) {
        const char *in = scg_const_name(nt, av[a]);
        if (!in || sp_streq(in, nm)) continue;
        int r = scg_named_lookup(nt, in, cname, n0, honor_private, seen);
        if (r) return r;
      }
    }
  }
  return 0;
}

/* Does any class or module body make `cname` a private constant? */
static int scg_any_privatizes(const NodeTable *nt, const char *cname, int n0) {
  for (int m = 0; m < n0; m++) {
    NodeKind mk = nt_kind(nt, m);
    if ((mk == NK_ClassNode || mk == NK_ModuleNode) && scg_body_privatizes(nt, m, cname)) return 1;
  }
  return 0;
}

/* The same from class `cls` up its superclasses: 1 when the nearest
   definition is one the lookup reads, -1 when it is private to a scoped
   read, 0 when no ancestor defines the name. */
static int scg_ancestor_lookup(const NodeTable *nt, int cls, const char *cname, int n0, int honor_private) {
  const char *nm = scg_const_name(nt, nt_ref(nt, cls, "constant_path"));
  ScgSeen seen = {{0}, 0};
  for (int depth = 0; nm && depth < 32; depth++) {
    int r = scg_named_lookup(nt, nm, cname, n0, honor_private, &seen);
    if (r) return r;
    const char *sup = NULL;
    for (int m = 0; m < n0 && !sup; m++) {
      if (nt_kind(nt, m) != NK_ClassNode) continue;
      const char *mn = scg_const_name(nt, nt_ref(nt, m, "constant_path"));
      if (mn && sp_streq(mn, nm)) sup = scg_const_name(nt, nt_ref(nt, m, "superclass"));
    }
    nm = sup;
  }
  return 0;
}

int desugar_self_const_get(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  int *parent = NULL;
  for (int id = 0; id < n0; id++) {
    /* `self::NAME` in a class method, `self.class::NAME` in an instance
       method: the same lookup */
    int cpath = 0, cls_recv = -1;
    const char *cname = NULL;
    if (nt_kind(nt, id) == NK_ConstantPathNode) {
      int par = nt_ref(nt, id, "parent");
      if (par < 0) continue;
      if (nt_kind(nt, par) == NK_SelfNode) cpath = 1;
      else if (nt_kind(nt, par) == NK_CallNode && nt_str(nt, par, "name") &&
               sp_streq(nt_str(nt, par, "name"), "class") && nt_ref(nt, par, "arguments") < 0 &&
               nt_ref(nt, par, "receiver") >= 0 && nt_kind(nt, nt_ref(nt, par, "receiver")) == NK_SelfNode) {
        cpath = 2; cls_recv = par;
      }
      else continue;
      cname = nt_str(nt, id, "name");
    }
    else {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "const_get")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 1 || an > 2) continue;
    cname = sym_or_str_literal(nt, av[0]);
    }
    if (!cname || !cname[0] || cname[0] < 'A' || cname[0] > 'Z' || strstr(cname, "::")) continue;
    if (!parent) parent = an_parent_map(nt);
    if (!parent) break;
    /* `defined?(self::NAME)`: the same dispatch, to a twin that answers
       "constant" or nil, and the defined? itself becomes the call */
    int dnode = cpath && parent[id] >= 0 && nt_kind(nt, parent[id]) == NK_DefinedNode ? parent[id] : -1;
    /* the enclosing def must be a class method: `def self.m` or a def in
       `class << self`; then the class it belongs to */
    int p = parent[id], def = -1;
    while (p >= 0 && nt_kind(nt, p) != NK_DefNode) {
      NodeKind pk = nt_kind(nt, p);
      if (pk == NK_ClassNode || pk == NK_ModuleNode || pk == NK_SingletonClassNode) break;
      p = parent[p];
    }
    if (p < 0 || nt_kind(nt, p) != NK_DefNode) continue;
    def = p;
    int dr = nt_ref(nt, def, "receiver");
    int is_cm = dr >= 0 && nt_kind(nt, dr) == NK_SelfNode;
    p = parent[def];
    while (p >= 0 && nt_kind(nt, p) != NK_ClassNode && nt_kind(nt, p) != NK_ModuleNode &&
           nt_kind(nt, p) != NK_SingletonClassNode) p = parent[p];
    if (p >= 0 && nt_kind(nt, p) == NK_SingletonClassNode) {
      int ex = nt_ref(nt, p, "expression");
      if (dr >= 0 || ex < 0 || nt_kind(nt, ex) != NK_SelfNode) continue;
      is_cm = 1;
      p = parent[p];
      while (p >= 0 && nt_kind(nt, p) != NK_ClassNode && nt_kind(nt, p) != NK_ModuleNode) p = parent[p];
    }
    if (p < 0 || (cpath == 2 ? is_cm : !is_cm)) continue;
    int owner = p;
    /* `self::NAME` raises for a private constant, const_get reads it: where
       the program makes the name private anywhere, const_get takes getters of
       its own */
    int honor = cpath != 0;
    char mname[256];
    snprintf(mname, sizeof mname, !honor && scg_any_privatizes(nt, cname, n0) ? "__spinel_cgp_%s" : "__spinel_cg_%s",
             cname);
    char dname[256]; snprintf(dname, sizeof dname, "__spinel_cd_%s", cname);
    /* every class body that defines the name */
    int others = 0;
    for (int m = 0; m < n0; m++) {
      NodeKind mk = nt_kind(nt, m);
      if ((mk != NK_ClassNode && mk != NK_ModuleNode) || m == owner) continue;
      if (!scg_body_defines(nt, m, cname)) continue;
      const char *mn = scg_const_name(nt, nt_ref(nt, m, "constant_path"));
      ScgSeen mseen = {{0}, 0};
      int priv = honor && mn && scg_named_lookup(nt, mn, cname, n0, 1, &mseen) < 0;
      if (dnode >= 0) scg_add_def_body(nt, m, cname, id, priv ? 3 : 2, dname);
      else scg_add_def_body(nt, m, cname, id, priv, mname);
      others = 1;
    }
    if (dnode >= 0) {
      scg_add_def_body(nt, owner, cname, id, scg_ancestor_lookup(nt, owner, cname, n0, 1) > 0 ? 2 : 3, dname);
      int line = (int)nt_int(nt, dnode, "node_line", 0);
      int file = (int)nt_int(nt, dnode, "node_file", 0);
      nt_node_reset(nt, dnode, "CallNode");
      if (line) nt_node_set_int(nt, dnode, "node_line", line);
      if (file) nt_node_set_int(nt, dnode, "node_file", file);
      if (cls_recv >= 0) nt_node_set_ref(nt, dnode, "receiver", cls_recv);
      nt_node_set_str(nt, dnode, "name", dname);
      nt_node_set_ref(nt, dnode, "arguments", -1);
      changed = 1;
      continue;
    }
    /* the method's own class answers through its lexical scope -- unless it
       leaves the name to its subclasses (an abstract `self::KEYBYTES`), where
       a reader of a constant defined nowhere would only raise. A name it
       inherits, from a superclass or an included module, is not one it
       leaves: the lookup finds it there. One whose nearest definition is
       private raises for `self::NAME`, while const_get reads it. */
    int found = scg_ancestor_lookup(nt, owner, cname, n0, honor);
    if (found > 0 || (!others && found == 0)) scg_add_def(nt, owner, cname, id, mname);
    else scg_add_def_body(nt, owner, cname, id, 1, mname);
    if (cpath) {
      int line = (int)nt_int(nt, id, "node_line", 0);
      int file = (int)nt_int(nt, id, "node_file", 0);
      nt_node_reset(nt, id, "CallNode");
      if (line) nt_node_set_int(nt, id, "node_line", line);
      if (file) nt_node_set_int(nt, id, "node_file", file);
      if (cls_recv >= 0) nt_node_set_ref(nt, id, "receiver", cls_recv);
    }
    nt_node_set_str(nt, id, "name", mname);
    nt_node_set_ref(nt, id, "arguments", -1);
    changed = 1;
  }
  free(parent);
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- const_get / const_defined? with a name known only at run time ----
 *
 * A module's constants are all known when the program is compiled, so a name
 * computed at run time (`Archive.const_get("COMPRESSION_#{c.upcase}")`) is a
 * lookup into a table of them. Each module whose const_get is called that way
 * gets
 *
 *   def self.__const_get__(n)     = {"A" => A, "B" => B, ...}[n.to_s]
 *   def self.__const_defined__(n) = {"A" => A, ...}.key?(n.to_s)
 *
 * built from the constants its bodies assign and the classes and modules
 * they define, and the call is renamed to it. A name outside the table
 * raises NameError (const_get) / answers false (const_defined?). */
static void cg_collect(const NodeTable *nt, const char *mn, int n0, char ***names, int *nn, int *cap) {
  for (int m = 0; m < n0; m++) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ModuleNode && mk != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!nm || !sp_streq(nm, mn)) continue;
    int body = nt_ref(nt, m, "body");
    int n = 0; const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      NodeKind sk = nt_kind(nt, st[k]);
      const char *cn = NULL;
      if (sk == NK_ConstantWriteNode || sk == NK_ConstantOrWriteNode) cn = nt_str(nt, st[k], "name");
      else if (sk == NK_ClassNode || sk == NK_ModuleNode) {
        int ccp = nt_ref(nt, st[k], "constant_path");
        cn = ccp >= 0 ? nt_str(nt, ccp, "name") : NULL;
      }
      if (!cn) continue;
      int dup = 0;
      for (int q = 0; q < *nn; q++) if (sp_streq((*names)[q], cn)) dup = 1;
      if (dup) continue;
      if (*nn == *cap) { *cap = *cap ? *cap * 2 : 32; *names = realloc(*names, sizeof(char *) * (size_t)*cap); }
      (*names)[(*nn)++] = strdup(cn);
    }
  }
}

/* def self.<mname>(__cg_n) = {<table>}.<op>(__cg_n.to_s) */
static int cg_def(NodeTable *nt, int like, const char *mname, const char *op,
                  char **names, int nn, const char *mod) {
  int def = fwd_new_node_like(nt, like, "DefNode");
  int self = fwd_new_node_like(nt, like, "SelfNode");
  int ps = fwd_new_node_like(nt, like, "ParametersNode");
  int rq = fwd_new_node_like(nt, like, "RequiredParameterNode");
  int body = fwd_new_node_like(nt, like, "StatementsNode");
  int hash = fwd_new_node_like(nt, like, "HashNode");
  int call = fwd_new_node_like(nt, like, "CallNode");
  int args = fwd_new_node_like(nt, like, "ArgumentsNode");
  int ts = fwd_new_node_like(nt, like, "CallNode");
  if (def < 0 || self < 0 || ps < 0 || rq < 0 || body < 0 || hash < 0 || call < 0 || args < 0 || ts < 0) return -1;
  int *els = malloc(sizeof(int) * (size_t)(nn > 0 ? nn : 1));
  for (int i = 0; i < nn; i++) {
    int as = fwd_new_node_like(nt, like, "AssocNode");
    int cr = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, cr, "name", names[i]);
    nt_node_set_ref(nt, as, "key", str_node_like(nt, like, names[i]));
    nt_node_set_ref(nt, as, "value", cr);
    els[i] = as;
  }
  nt_node_set_arr(nt, hash, "elements", els, nn);
  free(els);
  nt_node_set_str(nt, rq, "name", "__cg_n");
  nt_node_set_arr(nt, ps, "requireds", &rq, 1);
  nt_node_set_str(nt, ts, "name", "to_s");
  nt_node_set_ref(nt, ts, "receiver", local_read_like(nt, like, "__cg_n"));
  nt_node_set_arr(nt, args, "arguments", &ts, 1);
  nt_node_set_str(nt, call, "name", op);
  nt_node_set_ref(nt, call, "receiver", hash);
  nt_node_set_ref(nt, call, "arguments", args);
  if (!sp_streq(op, "[]") || !mod) nt_node_set_arr(nt, body, "body", &call, 1);
  else {
    /* const_get of a name outside the table is CRuby's NameError:
         __cg_h = {...}; __cg_k = __cg_n.to_s
         raise NameError, "uninitialized constant Mod::" + __cg_k unless __cg_h.key?(__cg_k)
         __cg_h[__cg_k] */
    int wh = fwd_new_node_like(nt, like, "LocalVariableWriteNode");
    nt_node_set_str(nt, wh, "name", "__cg_h");
    nt_node_set_int(nt, wh, "depth", 0);
    nt_node_set_ref(nt, wh, "value", hash);
    int wk = fwd_new_node_like(nt, like, "LocalVariableWriteNode");
    nt_node_set_str(nt, wk, "name", "__cg_k");
    nt_node_set_int(nt, wk, "depth", 0);
    nt_node_set_ref(nt, wk, "value", ts);
    int kq = fwd_new_node_like(nt, like, "CallNode");
    int kqa = fwd_new_node_like(nt, like, "ArgumentsNode");
    int kqk = local_read_like(nt, like, "__cg_k");
    nt_node_set_arr(nt, kqa, "arguments", &kqk, 1);
    nt_node_set_str(nt, kq, "name", "key?");
    nt_node_set_ref(nt, kq, "receiver", local_read_like(nt, like, "__cg_h"));
    nt_node_set_ref(nt, kq, "arguments", kqa);
    char msg[300]; snprintf(msg, sizeof msg, "uninitialized constant %s::", mod);
    int cat = fwd_new_node_like(nt, like, "CallNode");
    int cata = fwd_new_node_like(nt, like, "ArgumentsNode");
    int catk = local_read_like(nt, like, "__cg_k");
    nt_node_set_arr(nt, cata, "arguments", &catk, 1);
    nt_node_set_str(nt, cat, "name", "+");
    nt_node_set_ref(nt, cat, "receiver", str_node_like(nt, like, msg));
    nt_node_set_ref(nt, cat, "arguments", cata);
    int rc = fwd_new_node_like(nt, like, "CallNode");
    int rca = fwd_new_node_like(nt, like, "ArgumentsNode");
    int ne = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, ne, "name", "NameError");
    int rav[2] = { ne, cat };
    nt_node_set_arr(nt, rca, "arguments", rav, 2);
    nt_node_set_str(nt, rc, "name", "raise");
    nt_node_set_ref(nt, rc, "arguments", rca);
    int unl = fwd_new_node_like(nt, like, "UnlessNode");
    int ust = fwd_new_node_like(nt, like, "StatementsNode");
    nt_node_set_arr(nt, ust, "body", &rc, 1);
    nt_node_set_ref(nt, unl, "predicate", kq);
    nt_node_set_ref(nt, unl, "statements", ust);
    /* a name that is no constant name (`const_get("lower")`) is CRuby's
       other NameError, checked first:
         raise NameError, "wrong constant name " + __cg_k unless __cg_k[0].between?("A", "Z") */
    int c0 = fwd_new_node_like(nt, like, "CallNode");
    int c0a = fwd_new_node_like(nt, like, "ArgumentsNode");
    int i0 = fwd_new_node_like(nt, like, "IntegerNode");
    nt_node_set_int(nt, i0, "value", 0);
    nt_node_set_arr(nt, c0a, "arguments", &i0, 1);
    nt_node_set_str(nt, c0, "name", "[]");
    nt_node_set_ref(nt, c0, "receiver", local_read_like(nt, like, "__cg_k"));
    nt_node_set_ref(nt, c0, "arguments", c0a);
    int bw = fwd_new_node_like(nt, like, "CallNode");
    int bwa = fwd_new_node_like(nt, like, "ArgumentsNode");
    int bwv[2] = { str_node_like(nt, like, "A"), str_node_like(nt, like, "Z") };
    nt_node_set_arr(nt, bwa, "arguments", bwv, 2);
    nt_node_set_str(nt, bw, "name", "between?");
    nt_node_set_ref(nt, bw, "receiver", c0);
    nt_node_set_ref(nt, bw, "arguments", bwa);
    int cat2 = fwd_new_node_like(nt, like, "CallNode");
    int cat2a = fwd_new_node_like(nt, like, "ArgumentsNode");
    int cat2k = local_read_like(nt, like, "__cg_k");
    nt_node_set_arr(nt, cat2a, "arguments", &cat2k, 1);
    nt_node_set_str(nt, cat2, "name", "+");
    nt_node_set_ref(nt, cat2, "receiver", str_node_like(nt, like, "wrong constant name "));
    nt_node_set_ref(nt, cat2, "arguments", cat2a);
    int rc2 = fwd_new_node_like(nt, like, "CallNode");
    int rc2a = fwd_new_node_like(nt, like, "ArgumentsNode");
    int ne2 = fwd_new_node_like(nt, like, "ConstantReadNode");
    nt_node_set_str(nt, ne2, "name", "NameError");
    int rav2[2] = { ne2, cat2 };
    nt_node_set_arr(nt, rc2a, "arguments", rav2, 2);
    nt_node_set_str(nt, rc2, "name", "raise");
    nt_node_set_ref(nt, rc2, "arguments", rc2a);
    int unl2 = fwd_new_node_like(nt, like, "UnlessNode");
    int ust2 = fwd_new_node_like(nt, like, "StatementsNode");
    nt_node_set_arr(nt, ust2, "body", &rc2, 1);
    nt_node_set_ref(nt, unl2, "predicate", bw);
    nt_node_set_ref(nt, unl2, "statements", ust2);
    int kr = local_read_like(nt, like, "__cg_k");
    nt_node_set_arr(nt, args, "arguments", &kr, 1);
    nt_node_set_ref(nt, call, "receiver", local_read_like(nt, like, "__cg_h"));
    int stmts[5] = { wh, wk, unl2, unl, call };
    nt_node_set_arr(nt, body, "body", stmts, 5);
  }
  nt_node_set_str(nt, def, "name", mname);
  nt_node_set_ref(nt, def, "receiver", self);
  nt_node_set_ref(nt, def, "parameters", ps);
  nt_node_set_ref(nt, def, "body", body);
  return def;
}

int desugar_dynamic_const_get(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  char **done = NULL; int ndone = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int is_get = nm && sp_streq(nm, "const_get");
    int is_def = nm && sp_streq(nm, "const_defined?");
    if (!is_get && !is_def) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 1 || !av) continue;
    NodeKind ak = nt_kind(nt, av[0]);
    if (ak == NK_SymbolNode || ak == NK_StringNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || (nt_kind(nt, recv) != NK_ConstantReadNode && nt_kind(nt, recv) != NK_ConstantPathNode)) continue;
    const char *mn = nt_str(nt, recv, "name");
    if (!mn) continue;
    /* the module's first body carries the generated methods */
    int first = -1;
    for (int m = 0; m < n0 && first < 0; m++) {
      NodeKind mk = nt_kind(nt, m);
      if (mk != NK_ModuleNode && mk != NK_ClassNode) continue;
      int cp = nt_ref(nt, m, "constant_path");
      const char *bn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      if (bn && sp_streq(bn, mn) && nt_ref(nt, m, "body") >= 0) first = m;
    }
    if (first < 0) continue;
    int seen = 0;
    for (int q = 0; q < ndone; q++) if (sp_streq(done[q], mn)) seen = 1;
    if (!seen) {
      char **names = NULL; int nn = 0, cap = 0;
      cg_collect(nt, mn, n0, &names, &nn, &cap);
      int body = nt_ref(nt, first, "body");
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      int *out = malloc(sizeof(int) * (size_t)(bn + 2));
      memcpy(out, bs, sizeof(int) * (size_t)bn);
      int d1 = cg_def(nt, id, "__const_get__", "[]", names, nn, mn);
      int d2 = cg_def(nt, id, "__const_defined__", "key?", names, nn, NULL);
      int no = bn;
      if (d1 >= 0) out[no++] = d1;
      if (d2 >= 0) out[no++] = d2;
      nt_node_set_arr(nt, body, "body", out, no);
      free(out);
      for (int q = 0; q < nn; q++) free(names[q]);
      free(names);
      done = realloc(done, sizeof(char *) * (size_t)(ndone + 1));
      done[ndone++] = strdup(mn);
    }
    nt_node_set_str(nt, id, "name", is_get ? "__const_get__" : "__const_defined__");
    /* a second argument (inherit) has no meaning for the table */
    if (an > 1) nt_node_set_arr(nt, args, "arguments", av, 1);
    changed = 1;
  }
  for (int q = 0; q < ndone; q++) free(done[q]);
  free(done);
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- implicit self in methods added to a builtin class ------------------
 *
 *   class Hash
 *     def ffi_yajl(gen, state)
 *       each do |k, v| ... end        # self.each
 *     end
 *   end
 *   class Time
 *     def stamp = strftime("%Y")      # self.strftime
 *   end
 *
 * A receiverless call in such a method is a call on the builtin value, but
 * the builtin surface is reached through a receiver: the bare name has no
 * method to resolve to. Each core method name the class body itself does not
 * define is given `self` as its receiver. Kernel's names (puts, raise,
 * format, ...) stay as they are -- they are self's private methods, which a
 * receiver would not reach. In a method added to Array, self as the receiver
 * of an Array method (explicit or not) is marked for codegen, which holds
 * self boxed. */
static const char *const CORE_METHOD_NAMES[] = {
#include "core_method_names.inc"
  NULL };
static const char *const OBJECT_METHOD_NAMES[] = {
#include "object_method_names.inc"
  NULL };
static const char *const RB_OBJECT_PUBLIC[] = {
#include "object_public_method_names.inc"
  NULL };

int core_method_name(const char *n) {
  /* the table is sorted */
  int lo = 0, hi = (int)(sizeof CORE_METHOD_NAMES / sizeof CORE_METHOD_NAMES[0]) - 2;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int r = strcmp(n, CORE_METHOD_NAMES[mid]);
    if (r == 0) return 1;
    if (r < 0) hi = mid - 1; else lo = mid + 1;
  }
  return 0;
}

static int name_in_list(const char *const *list, const char *n) {
  return str_in(n, list);
}

static int rbself_builtin(const char *cn) {
  static const char *const B[] = { "String", "Integer", "Float", "Symbol", "TrueClass",
    "FalseClass", "NilClass", "Array", "Hash", "Time", "Numeric", "Range", "Regexp", NULL };
  return str_in(cn, B);
}

static int rbself_array_method(const char *nm) {
  return core_method_name(nm) && !name_in_list(OBJECT_METHOD_NAMES, nm);
}

static void rbself_walk(NodeTable *nt, int node, char **defs, int nd, int *changed, int is_array) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return;
  /* in a method added to Array, self is held boxed while typed as the poly
     array: as the receiver of an Array method it reads through the
     conversion (Object's methods, `self.class`, take the boxed value) */
  if (k == NK_CallNode && is_array) {
    int r = nt_ref(nt, node, "receiver");
    const char *nm = nt_str(nt, node, "name");
    if (r >= 0 && nt_kind(nt, r) == NK_SelfNode && nm && rbself_array_method(nm))
      nt_node_set_int(nt, r, "ary_self", 1);
  }
  if (k == NK_CallNode && nt_ref(nt, node, "receiver") < 0) {
    const char *nm = nt_str(nt, node, "name");
    int user = 0;
    for (int i = 0; nm && i < nd; i++) if (sp_streq(defs[i], nm)) user = 1;
    if (nm && !user && ((core_method_name(nm) && !name_in_list(OBJECT_METHOD_NAMES, nm)) ||
                        name_in_list(RB_OBJECT_PUBLIC, nm)) &&
        !sp_streq(nm, "lambda") && !sp_streq(nm, "proc") && !sp_streq(nm, "loop") &&
        !sp_streq(nm, "catch") && !sp_streq(nm, "throw") && !sp_streq(nm, "attr_reader") &&
        !sp_streq(nm, "attr_accessor") && !sp_streq(nm, "attr_writer") && !sp_streq(nm, "binding")) {
      int rself = fwd_new_node_like(nt, node, "SelfNode");
      if (is_array && rbself_array_method(nm)) nt_node_set_int(nt, rself, "ary_self", 1);
      nt_node_set_ref(nt, node, "receiver", rself);
      *changed = 1;
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) rbself_walk(nt, nt_ref_at(nt, node, i), defs, nd, changed, is_array);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    int *cp = cnt > 0 ? malloc(sizeof(int) * (size_t)cnt) : NULL;
    if (cnt > 0) memcpy(cp, ids, sizeof(int) * (size_t)cnt);
    for (int j = 0; j < cnt; j++) rbself_walk(nt, cp[j], defs, nd, changed, is_array);
    free(cp);
  }
}

int desugar_builtin_reopen_self_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || !rbself_builtin(cn)) continue;
    /* the methods every body of the class defines */
    char *defs[512]; int nd = 0;
    for (int m2 = 0; m2 < n0; m2++) {
      if (nt_kind(nt, m2) != NK_ClassNode) continue;
      int cp2 = nt_ref(nt, m2, "constant_path");
      const char *cn2 = cp2 >= 0 ? nt_str(nt, cp2, "name") : NULL;
      if (!cn2 || !sp_streq(cn2, cn)) continue;
      int b2 = nt_ref(nt, m2, "body");
      int bn2 = 0; const int *bs2 = b2 >= 0 ? nt_arr(nt, b2, "body", &bn2) : NULL;
      for (int k = 0; k < bn2 && nd < 512; k++)
        if (nt_kind(nt, bs2[k]) == NK_DefNode && nt_str(nt, bs2[k], "name"))
          defs[nd++] = (char *)nt_str(nt, bs2[k], "name");
    }
    int body = nt_ref(nt, m, "body");
    int bn = 0; const int *bs = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++) {
      if (nt_kind(nt, bs[k]) != NK_DefNode || nt_ref(nt, bs[k], "receiver") >= 0) continue;
      rbself_walk(nt, nt_ref(nt, bs[k], "body"), defs, nd, &changed, sp_streq(cn, "Array"));
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- methods added to a builtin class with no native self ----------------
 *
 *   class Hash
 *     def two = size * 2
 *   end
 *   {a: 1}.two
 *
 * A method added to Hash, Time, Range, ... has no arm on the builtin
 * receiver: dispatch on those values reaches Object's methods (whose self is
 * the value, boxed) and the reopened scalars (String, Integer, ...), which
 * take their unboxed self. Each such method becomes Object's, guarded by the
 * class it was added to:
 *
 *   class Object
 *     def two = if is_a?(Hash) then size * 2
 *               else raise NoMethodError, "undefined method 'two'" end
 *   end
 */
/* Spinel has no Date or DateTime of its own: a program that defines one --
   a body of the name with a superclass, an initialize, or class methods (a
   pure-Ruby Date) -- owns the class, and its bodies are not a builtin's
   reopening. A body that only adds instance methods (a gem reopening the
   Date it expects the date library to provide) still is. */
static int mo_program_owns(const NodeTable *nt, int n0, const char *cn) {
  if (!sp_streq(cn, "Date") && !sp_streq(cn, "DateTime")) return 0;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *mn = cp >= 0 && nt_kind(nt, cp) == NK_ConstantReadNode ? nt_str(nt, cp, "name") : NULL;
    if (!mn || !sp_streq(mn, cn)) continue;
    if (nt_ref(nt, m, "superclass") >= 0) return 1;
    int b = nt_ref(nt, m, "body");
    int bn = 0; const int *bs = b >= 0 ? nt_arr(nt, b, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++) {
      if (nt_kind(nt, bs[k]) == NK_SingletonClassNode) return 1;
      if (nt_kind(nt, bs[k]) != NK_DefNode) continue;
      if (nt_ref(nt, bs[k], "receiver") >= 0) return 1;
      const char *dn = nt_str(nt, bs[k], "name");
      if (dn && sp_streq(dn, "initialize")) return 1;
    }
  }
  return 0;
}

static const char *mo_guard_class(const char *cn) {
  /* Hash, Time and Range reopenings are modelled directly (a typed receiver
     dispatches to the reopening's own method, a boxed one through the
     dispatch key, and respond_to? sees them): the guard form is for the
     builtins that model does not cover */
  static const char *const B[] = { "Regexp", "Proc", "Date",
    "DateTime", "Rational", "Complex", NULL };
  for (int i = 0; B[i]; i++) if (sp_streq(B[i], cn)) return B[i];
  return NULL;
}

static int mo_guard_pred(NodeTable *nt, int like, const char *cn) {
  int call = fwd_new_node_like(nt, like, "CallNode");
  int args = fwd_new_node_like(nt, like, "ArgumentsNode");
  nt_node_set_ref(nt, call, "receiver", fwd_new_node_like(nt, like, "SelfNode"));
  nt_node_set_str(nt, call, "name", "is_a?");
  int arg = fwd_new_node_like(nt, like, "ConstantReadNode");
  nt_node_set_str(nt, arg, "name", cn);
  nt_node_set_arr(nt, args, "arguments", &arg, 1);
  nt_node_set_ref(nt, call, "arguments", args);
  return call;
}

static int mo_object_defines(const NodeTable *nt, int n0, const char *mname) {
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || !sp_streq(cn, "Object")) continue;
    int b = nt_ref(nt, m, "body");
    int bn = 0; const int *bs = b >= 0 ? nt_arr(nt, b, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++) {
      const char *dn = nt_kind(nt, bs[k]) == NK_DefNode && nt_ref(nt, bs[k], "receiver") < 0
                       ? nt_str(nt, bs[k], "name") : NULL;
      if (dn && sp_streq(dn, mname)) return 1;
    }
  }
  return 0;
}

/* A builtin class body left with nothing in it disappears, so no user class
   of a builtin's name is defined (`class Time` would clash with the runtime's
   Time). */
int desugar_builtin_reopen_methods(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  int root = nt->root_id;
  int rst = root >= 0 ? nt_ref(nt, root, "statements") : -1;
  if (rst < 0) return 0;
  for (int m = 0; m < n0; m++) {
    if (nt_kind(nt, m) != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    if (cp < 0 || nt_kind(nt, cp) != NK_ConstantReadNode) continue;
    const char *cn = nt_str(nt, cp, "name");
    const char *g = cn ? mo_guard_class(cn) : NULL;
    if (!g) continue;
    /* a program's own class of the name (with a superclass) is its own */
    if (nt_ref(nt, m, "superclass") >= 0 || mo_program_owns(nt, n0, cn)) continue;
    int b = nt_ref(nt, m, "body");
    int bn = 0; const int *bs = b >= 0 ? nt_arr(nt, b, "body", &bn) : NULL;
    int *keep = malloc(sizeof(int) * (size_t)(bn ? bn : 1)); int nk = 0;
    for (int k = 0; k < bn; k++) {
      int d = bs[k];
      if (nt_kind(nt, d) != NK_DefNode || nt_ref(nt, d, "receiver") >= 0) { keep[nk++] = d; continue; }
      /* def m(...) = if is_a?(K) then body else raise NoMethodError, "..." end,
         in a fresh `class Object` at the top level */
      const char *mname = nt_str(nt, d, "name");
      /* Object's own method of the name would be replaced: left as it was */
      if (!mname || mo_object_defines(nt, n0, mname)) { keep[nk++] = d; continue; }
      int body = nt_ref(nt, d, "body");
      int st = body;
      if (st >= 0 && nt_kind(nt, st) != NK_StatementsNode) {
        st = fwd_new_node_like(nt, d, "StatementsNode");
        nt_node_set_arr(nt, st, "body", &body, 1);
      }
      if (st < 0) {
        st = fwd_new_node_like(nt, d, "StatementsNode");
        int nl = fwd_new_node_like(nt, d, "NilNode");
        nt_node_set_arr(nt, st, "body", &nl, 1);
      }
      int ifn = fwd_new_node_like(nt, d, "IfNode");
      nt_node_set_ref(nt, ifn, "predicate", mo_guard_pred(nt, d, g));
      nt_node_set_ref(nt, ifn, "statements", st);
      int els = fwd_new_node_like(nt, d, "ElseNode");
      int est = fwd_new_node_like(nt, d, "StatementsNode");
      int rc = fwd_new_node_like(nt, d, "CallNode");
      int ra = fwd_new_node_like(nt, d, "ArgumentsNode");
      int ne = fwd_new_node_like(nt, d, "ConstantReadNode");
      nt_node_set_str(nt, ne, "name", "NoMethodError");
      char msg[300]; snprintf(msg, sizeof msg, "undefined method '%s'", mname);
      int av[2] = { ne, str_node_like(nt, d, msg) };
      nt_node_set_arr(nt, ra, "arguments", av, 2);
      nt_node_set_str(nt, rc, "name", "raise");
      nt_node_set_ref(nt, rc, "arguments", ra);
      nt_node_set_arr(nt, est, "body", &rc, 1);
      nt_node_set_ref(nt, els, "statements", est);
      nt_node_set_ref(nt, ifn, "subsequent", els);
      int nb = fwd_new_node_like(nt, d, "StatementsNode");
      nt_node_set_arr(nt, nb, "body", &ifn, 1);
      nt_node_set_ref(nt, d, "body", nb);
      int oc = fwd_new_node_like(nt, d, "ClassNode");
      int ocp = fwd_new_node_like(nt, d, "ConstantReadNode");
      nt_node_set_str(nt, ocp, "name", "Object");
      nt_node_set_ref(nt, oc, "constant_path", ocp);
      int ob = fwd_new_node_like(nt, d, "StatementsNode");
      nt_node_set_arr(nt, ob, "body", &d, 1);
      nt_node_set_ref(nt, oc, "body", ob);
      /* before the program's own statements, as the class body would be */
      int rn = 0; const int *rs = nt_arr(nt, rst, "body", &rn);
      int *nr = malloc(sizeof(int) * (size_t)(rn + 1));
      nr[0] = oc;
      if (rn) memcpy(nr + 1, rs, sizeof(int) * (size_t)rn);
      nt_node_set_arr(nt, rst, "body", nr, rn + 1);
      free(nr);
      changed = 1;
    }
    if (nk != bn) nt_node_set_arr(nt, b, "body", keep, nk);
    free(keep);
    if (nk == 0 && b >= 0) {
      /* nothing left: the reopening itself goes */
      nt_node_reset(nt, m, "NilNode");
      changed = 1;
    }
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- module/class-body ivars read outside a method ------------------------
 *
 *   module GLib
 *     @logger = Logger.new($stdout)
 *     H = proc { |d, l, m| @logger.log(l, m, d) }
 *   end
 *
 * A body-level `@x` is the module object's own ivar -- the one its class
 * methods see. Writes directly in the body are attributed to it, but a read
 * (and anything inside a block, whose C function is emitted outside the body)
 * fell to the Toplevel pseudo-class or an instance slot. Every such access
 * is routed through a pair of class-method accessors
 *
 *   def self.__spinel_civget_x = @x
 *   def self.__spinel_civset_x(v) = @x = v
 *
 * called on the module constant, so they resolve to the module's civ. Blocks
 * whose self is something else (class_eval, instance_eval, define_method,
 * Class.new, ...) and nested class/def bodies are not entered. */
static int cbi_self_changing_block(const NodeTable *nt, int call) {
  const char *nm = nt_str(nt, call, "name");
  if (!nm) return 0;
  static const char *const SC[] = { "class_eval", "module_eval", "class_exec", "module_exec",
    "instance_eval", "instance_exec", "define_method", "define_singleton_method",
    "new", "define", "configure", NULL };
  return str_in(nm, SC);
}

/* `@x op= v` / `@x ||= v` / `@x &&= v` spelled as the plain read and write
   the accessor rewrite handles: `@x = @x op v`, `@x || @x = v`, `@x && @x = v` */
static void cbi_lower_op_writes(NodeTable *nt, int node) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode || k == NK_SingletonClassNode)
    return;
  if (k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableOrWriteNode ||
      k == NK_InstanceVariableAndWriteNode) {
    const char *iv = nt_str(nt, node, "name");
    const char *op = k == NK_InstanceVariableOperatorWriteNode ? nt_str(nt, node, "binary_operator") : NULL;
    int v = nt_ref(nt, node, "value");
    if (iv && v >= 0 && (op || k != NK_InstanceVariableOperatorWriteNode)) {
      char ivb[256], opb[16];
      snprintf(ivb, sizeof ivb, "%s", iv);
      snprintf(opb, sizeof opb, "%s", op ? op : "");
      int rd = fwd_new_node_like(nt, node, "InstanceVariableReadNode");
      nt_node_set_str(nt, rd, "name", ivb);
      if (k == NK_InstanceVariableOperatorWriteNode) {
        int call = fwd_new_node_like(nt, node, "CallNode");
        int args = fwd_new_node_like(nt, node, "ArgumentsNode");
        nt_node_set_arr(nt, args, "arguments", &v, 1);
        nt_node_set_str(nt, call, "name", opb);
        nt_node_set_ref(nt, call, "receiver", rd);
        nt_node_set_ref(nt, call, "arguments", args);
        nt_node_reset(nt, node, "InstanceVariableWriteNode");
        nt_node_set_str(nt, node, "name", ivb);
        nt_node_set_ref(nt, node, "value", call);
      }
      else {
        int wr = fwd_new_node_like(nt, node, "InstanceVariableWriteNode");
        nt_node_set_str(nt, wr, "name", ivb);
        nt_node_set_ref(nt, wr, "value", v);
        nt_node_reset(nt, node, k == NK_InstanceVariableOrWriteNode ? "OrNode" : "AndNode");
        nt_node_set_ref(nt, node, "left", rd);
        nt_node_set_ref(nt, node, "right", wr);
      }
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) cbi_lower_op_writes(nt, nt_ref_at(nt, node, i));
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    int *cp = cnt > 0 ? malloc(sizeof(int) * (size_t)cnt) : NULL;
    if (cnt > 0) memcpy(cp, ids, sizeof(int) * (size_t)cnt);
    for (int j = 0; j < cnt; j++) cbi_lower_op_writes(nt, cp[j]);
    free(cp);
  }
}

/* A proc handed to one of those calls as an argument -- `define_method(:k,
   -> { @v })`, or a local holding it, `define_method(:h, pr)` -- runs with
   the other self too: the locals so passed, to leave their procs alone. */
typedef struct { const char *names[64]; int n; } CbiProcLocals;

static void cbi_proc_locals(const NodeTable *nt, int node, CbiProcLocals *pl) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode || k == NK_SingletonClassNode)
    return;
  if (k == NK_CallNode && cbi_self_changing_block(nt, node)) {
    int an = nt_ref(nt, node, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    int ba = nt_ref(nt, node, "block");
    for (int i = 0; i <= ac; i++) {
      int x = i < ac ? av[i] : (ba >= 0 && nt_kind(nt, ba) == NK_BlockArgumentNode
                                ? nt_ref(nt, ba, "expression") : -1);
      if (x >= 0 && nt_kind(nt, x) == NK_BlockArgumentNode) x = nt_ref(nt, x, "expression");
      if (x >= 0 && nt_kind(nt, x) == NK_LocalVariableReadNode && nt_str(nt, x, "name") && pl->n < 64)
        pl->names[pl->n++] = nt_str(nt, x, "name");
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) cbi_proc_locals(nt, nt_ref_at(nt, node, i), pl);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) cbi_proc_locals(nt, ids[j], pl);
  }
}

/* `no_procs`: in the arguments of a self-changing call, whose procs are
   entered by the other self */
static void cbi_collect(const NodeTable *nt, int node, int in_block, int no_procs,
                        const CbiProcLocals *pl, int *hits, int *nhits, int cap, int *trigger) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_DefNode || k == NK_SingletonClassNode)
    return;
  if ((k == NK_BlockNode || k == NK_LambdaNode) && no_procs) return;
  if (k == NK_CallNode && cbi_self_changing_block(nt, node)) {
    /* the receiver and arguments are still body code, their procs are not */
    cbi_collect(nt, nt_ref(nt, node, "receiver"), in_block, no_procs, pl, hits, nhits, cap, trigger);
    cbi_collect(nt, nt_ref(nt, node, "arguments"), in_block, 1, pl, hits, nhits, cap, trigger);
    int ba = nt_ref(nt, node, "block");
    if (ba >= 0 && nt_kind(nt, ba) == NK_BlockArgumentNode)
      cbi_collect(nt, ba, in_block, 1, pl, hits, nhits, cap, trigger);
    return;
  }
  if (k == NK_LocalVariableWriteNode && nt_str(nt, node, "name")) {
    for (int q = 0; q < pl->n; q++)
      if (sp_streq(pl->names[q], nt_str(nt, node, "name"))) { no_procs = 1; break; }
  }
  if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode) {
    if (*nhits < cap) hits[(*nhits)++] = node;
    if (k == NK_InstanceVariableReadNode || in_block) *trigger = 1;
  }
  int blk = (k == NK_BlockNode || k == NK_LambdaNode) ? 1 : in_block;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++)
    cbi_collect(nt, nt_ref_at(nt, node, i), blk, no_procs, pl, hits, nhits, cap, trigger);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++)
      cbi_collect(nt, ids[j], blk, no_procs, pl, hits, nhits, cap, trigger);
  }
}

int desugar_body_ivars(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int m = 0; m < n0; m++) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ModuleNode && mk != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    if (cp < 0 || nt_kind(nt, cp) != NK_ConstantReadNode) continue;
    const char *cn = nt_str(nt, cp, "name");
    int body = nt_ref(nt, m, "body");
    if (!cn || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int cap = 4096, nhits = 0, trigger = 0;
    int *hits = malloc(sizeof(int) * (size_t)cap);
    int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
    CbiProcLocals pl; pl.n = 0;
    for (int k = 0; k < bn; k++) cbi_proc_locals(nt, bs[k], &pl);
    for (int k = 0; k < bn; k++) cbi_collect(nt, bs[k], 0, 0, &pl, hits, &nhits, cap, &trigger);
    if (trigger) {
      for (int k = 0; k < bn; k++) cbi_lower_op_writes(nt, bs[k]);
      bs = nt_arr(nt, body, "body", &bn);
      nhits = 0; pl.n = 0;
      for (int k = 0; k < bn; k++) cbi_proc_locals(nt, bs[k], &pl);
      for (int k = 0; k < bn; k++) cbi_collect(nt, bs[k], 0, 0, &pl, hits, &nhits, cap, &trigger);
    }
    if (!trigger || nhits == 0) { free(hits); continue; }
    /* names needing accessors */
    char *names[256]; int nn = 0;
    for (int h = 0; h < nhits; h++) {
      const char *iv = nt_str(nt, hits[h], "name");
      if (!iv || iv[0] != '@' || iv[1] == '@') continue;
      int seen = 0;
      for (int q = 0; q < nn; q++) if (sp_streq(names[q], iv)) seen = 1;
      if (!seen && nn < 256) names[nn++] = strdup(iv);
    }
    int *defs = malloc(sizeof(int) * (size_t)(2 * nn + bn));
    int nd = 0;
    for (int q = 0; q < nn; q++) {
      const char *iv = names[q];
      char gname[256], sname[256];
      snprintf(gname, sizeof gname, "__spinel_civget_%s", iv + 1);
      snprintf(sname, sizeof sname, "__spinel_civset_%s", iv + 1);
      int gd = fwd_new_node_like(nt, m, "DefNode");
      int gb = fwd_new_node_like(nt, m, "StatementsNode");
      int gr = fwd_new_node_like(nt, m, "InstanceVariableReadNode");
      nt_node_set_str(nt, gr, "name", iv);
      nt_node_set_arr(nt, gb, "body", &gr, 1);
      nt_node_set_str(nt, gd, "name", gname);
      nt_node_set_ref(nt, gd, "receiver", fwd_new_node_like(nt, m, "SelfNode"));
      nt_node_set_ref(nt, gd, "body", gb);
      int sd = fwd_new_node_like(nt, m, "DefNode");
      int sb = fwd_new_node_like(nt, m, "StatementsNode");
      int sw = fwd_new_node_like(nt, m, "InstanceVariableWriteNode");
      int ps = fwd_new_node_like(nt, m, "ParametersNode");
      int rq = fwd_new_node_like(nt, m, "RequiredParameterNode");
      nt_node_set_str(nt, rq, "name", "spinel_civ_v__");
      nt_node_set_arr(nt, ps, "requireds", &rq, 1);
      nt_node_set_str(nt, sw, "name", iv);
      nt_node_set_ref(nt, sw, "value", local_read_like(nt, m, "spinel_civ_v__"));
      nt_node_set_arr(nt, sb, "body", &sw, 1);
      nt_node_set_str(nt, sd, "name", sname);
      nt_node_set_ref(nt, sd, "receiver", fwd_new_node_like(nt, m, "SelfNode"));
      nt_node_set_ref(nt, sd, "parameters", ps);
      nt_node_set_ref(nt, sd, "body", sb);
      defs[nd++] = gd; defs[nd++] = sd;
    }
    /* rewrite the accesses in place */
    for (int h = 0; h < nhits; h++) {
      int id = hits[h];
      const char *iv0 = nt_str(nt, id, "name");
      if (!iv0 || iv0[1] == '@') continue;
      char iv[256]; snprintf(iv, sizeof iv, "%s", iv0);
      NodeKind k = nt_kind(nt, id);
      int line = (int)nt_int(nt, id, "node_line", 0);
      int file = (int)nt_int(nt, id, "node_file", 0);
      int recv = fwd_new_node_like(nt, id, "ConstantReadNode");
      nt_node_set_str(nt, recv, "name", cn);
      char mname[256];
      if (k == NK_InstanceVariableReadNode) {
        snprintf(mname, sizeof mname, "__spinel_civget_%s", iv + 1);
        nt_node_reset(nt, id, "CallNode");
        nt_node_set_str(nt, id, "name", mname);
        nt_node_set_ref(nt, id, "receiver", recv);
      }
      else {
        int v = nt_ref(nt, id, "value");
        snprintf(mname, sizeof mname, "__spinel_civset_%s", iv + 1);
        int args = fwd_new_node_like(nt, id, "ArgumentsNode");
        nt_node_set_arr(nt, args, "arguments", &v, 1);
        nt_node_reset(nt, id, "CallNode");
        nt_node_set_str(nt, id, "name", mname);
        nt_node_set_ref(nt, id, "receiver", recv);
        nt_node_set_ref(nt, id, "arguments", args);
      }
      if (line) nt_node_set_int(nt, id, "node_line", line);
      if (file) nt_node_set_int(nt, id, "node_file", file);
    }
    memcpy(defs + nd, bs, sizeof(int) * (size_t)bn);
    nt_node_set_arr(nt, body, "body", defs, nd + bn);
    free(defs);
    for (int q = 0; q < nn; q++) free(names[q]);
    free(hits);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- a method on Object, overridden in builtin classes --------------------
 *
 *   class Object;    def ffi_yajl(g, s) ... to_json ... end; end
 *   class Hash;      def ffi_yajl(g, s) ... each { } ... end; end
 *   class Array;     def ffi_yajl(g, s) ... end; end
 *   class TrueClass; def ffi_yajl(g, s) ... end; end
 *
 * Dispatch on a run-time value reaches Object's method (whose self is the
 * boxed value) and the reopened scalars (String, Integer, ...), which take
 * their unboxed self; a container, boolean or other builtin override has no
 * such arm. The overrides move into Object's method as branches on self's
 * class:
 *
 *   def ffi_yajl(g, s)
 *     if is_a?(Hash) then <Hash's body> elsif is_a?(Array) then <Array's>
 *     elsif self == true then <TrueClass's> else <Object's> end
 *   end
 *
 * Only when every override takes the same parameters as Object's. */
static const char *mo_override_class(const char *cn) {
  static const char *const B[] = { "Hash", "Array", "TrueClass", "FalseClass", "Time", "Range",
    "Regexp", "Proc", "Date", "DateTime", "Rational", "Complex", "Exception", NULL };
  for (int i = 0; B[i]; i++) if (sp_streq(B[i], cn)) return B[i];
  return NULL;
}

static int mo_params_sig(const NodeTable *nt, int def, char *out, size_t cap) {
  int ps = nt_ref(nt, def, "parameters");
  out[0] = 0;
  if (ps < 0) return 1;
  static const char *const L[] = { "requireds", "optionals", "posts", "keywords" };
  size_t o = 0;
  for (int li = 0; li < 4; li++) {
    int n = 0; const int *ids = nt_arr(nt, ps, L[li], &n);
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s:%d;", L[li], n);
    for (int j = 0; j < n; j++) {
      const char *pn = nt_str(nt, ids[j], "name");
      o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s,", pn ? pn : "?");
    }
  }
  const char *R[] = { "rest", "keyword_rest", "block" };
  for (int r = 0; r < 3; r++) {
    int x = nt_ref(nt, ps, R[r]);
    const char *pn = x >= 0 ? nt_str(nt, x, "name") : NULL;
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s=%s;", R[r], x >= 0 ? (pn ? pn : "_") : "-");
  }
  return o < cap;
}

/* is_a?(K), or `self == true` / `self == false` for the booleans */
static int mo_override_pred(NodeTable *nt, int like, const char *cn) {
  if (!sp_streq(cn, "TrueClass") && !sp_streq(cn, "FalseClass")) return mo_guard_pred(nt, like, cn);
  int call = fwd_new_node_like(nt, like, "CallNode");
  int args = fwd_new_node_like(nt, like, "ArgumentsNode");
  nt_node_set_ref(nt, call, "receiver", fwd_new_node_like(nt, like, "SelfNode"));
  nt_node_set_str(nt, call, "name", "==");
  int arg = fwd_new_node_like(nt, like, sp_streq(cn, "TrueClass") ? "TrueNode" : "FalseNode");
  nt_node_set_arr(nt, args, "arguments", &arg, 1);
  nt_node_set_ref(nt, call, "arguments", args);
  return call;
}

int desugar_object_method_builtin_overrides(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  static int rm_def[1024], rm_cls[1024];
  int nrm = 0;
  /* Object's own instance methods */
  for (int om = 0; om < n0; om++) {
    if (nt_kind(nt, om) != NK_ClassNode) continue;
    int ocp = nt_ref(nt, om, "constant_path");
    const char *ocn = ocp >= 0 ? nt_str(nt, ocp, "name") : NULL;
    if (!ocn || !sp_streq(ocn, "Object")) continue;
    int ob = nt_ref(nt, om, "body");
    int obn = 0; const int *obs = ob >= 0 ? nt_arr(nt, ob, "body", &obn) : NULL;
    for (int oi = 0; oi < obn; oi++) {
      int odef = obs[oi];
      if (nt_kind(nt, odef) != NK_DefNode || nt_ref(nt, odef, "receiver") >= 0) continue;
      const char *mname = nt_str(nt, odef, "name");
      if (!mname) continue;
      char osig[1024];
      if (!mo_params_sig(nt, odef, osig, sizeof osig)) continue;
      /* the overrides */
      int ovr[32], ovc[32]; const char *ocls[32]; int no = 0, bad = 0;
      for (int m = 0; m < n0 && !bad; m++) {
        if (nt_kind(nt, m) != NK_ClassNode) continue;
        int cp = nt_ref(nt, m, "constant_path");
        const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
        const char *g = cn ? mo_override_class(cn) : NULL;
        if (!g || mo_program_owns(nt, n0, cn)) continue;
        int b = nt_ref(nt, m, "body");
        int bn = 0; const int *bs = b >= 0 ? nt_arr(nt, b, "body", &bn) : NULL;
        for (int k = 0; k < bn; k++) {
          if (nt_kind(nt, bs[k]) != NK_DefNode || nt_ref(nt, bs[k], "receiver") >= 0) continue;
          const char *dn = nt_str(nt, bs[k], "name");
          if (!dn || !sp_streq(dn, mname)) continue;
          char sig[1024];
          if (!mo_params_sig(nt, bs[k], sig, sizeof sig) || strcmp(sig, osig) != 0) { bad = 1; break; }
          if (no < 32) { ovr[no] = bs[k]; ovc[no] = m; ocls[no] = g; no++; }
        }
      }
      if (bad || no == 0) continue;
      /* if is_a?(A) then A's body elsif ... else Object's body end */
      int obody = nt_ref(nt, odef, "body");
      int tail = -1;   /* the else part */
      if (obody >= 0) {
        tail = fwd_new_node_like(nt, odef, "ElseNode");
        int st = obody;
        if (nt_kind(nt, st) != NK_StatementsNode) {
          st = fwd_new_node_like(nt, odef, "StatementsNode");
          nt_node_set_arr(nt, st, "body", &obody, 1);
        }
        nt_node_set_ref(nt, tail, "statements", st);
      }
      for (int q = no - 1; q >= 0; q--) {
        int ifn = fwd_new_node_like(nt, odef, "IfNode");
        nt_node_set_ref(nt, ifn, "predicate", mo_guard_pred(nt, odef, ocls[q]));
        int body = nt_ref(nt, ovr[q], "body");
        /* a copy: the same override may serve several Object definitions
           (one file inlined under several conditions) */
        if (body >= 0) body = nt_clone_subtree(nt, body);
        int st = body;
        if (st >= 0 && nt_kind(nt, st) != NK_StatementsNode) {
          st = fwd_new_node_like(nt, odef, "StatementsNode");
          nt_node_set_arr(nt, st, "body", &body, 1);
        }
        if (st < 0) {
          st = fwd_new_node_like(nt, odef, "StatementsNode");
          int nl = fwd_new_node_like(nt, odef, "NilNode");
          nt_node_set_arr(nt, st, "body", &nl, 1);
        }
        nt_node_set_ref(nt, ifn, "statements", st);
        if (tail >= 0) nt_node_set_ref(nt, ifn, "subsequent", tail);
        tail = ifn;
      }
      int nb = fwd_new_node_like(nt, odef, "StatementsNode");
      nt_node_set_arr(nt, nb, "body", &tail, 1);
      nt_node_set_ref(nt, odef, "body", nb);
      /* the overrides leave their classes once every Object definition has
         taken them */
      for (int q = 0; q < no; q++) {
        int dup = 0;
        for (int r = 0; r < nrm; r++) if (rm_def[r] == ovr[q]) dup = 1;
        if (!dup && nrm < 1024) { rm_def[nrm] = ovr[q]; rm_cls[nrm] = ovc[q]; nrm++; }
      }
      changed = 1;
    }
  }
  for (int r = 0; r < nrm; r++) {
    int b = nt_ref(nt, rm_cls[r], "body");
    int bn = 0; const int *bs = nt_arr(nt, b, "body", &bn);
    int *keep = malloc(sizeof(int) * (size_t)(bn ? bn : 1)); int nk = 0;
    for (int k = 0; k < bn; k++) if (bs[k] != rm_def[r]) keep[nk++] = bs[k];
    nt_node_set_arr(nt, b, "body", keep, nk);
    free(keep);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- a parameter default that assigns a local the body reads ----
   `def index_with(default = (no_default = true))` (activesupport's
   Enumerable#index_with) tells an omitted argument from a passed one by the
   local its default leaves behind: set when the default ran, nil otherwise.
   A default is evaluated at the call site, where that local has no way into
   the callee; so the callee runs this default itself. The parameter takes a
   private symbol as its default, the locals are declared nil ahead of the
   body, and a guard binds the parameter from the original default when it
   sees the symbol:
     def m(p = :__sp_absent)
       no_default = nil
       p = (no_default = true) if p == :__sp_absent
       ...
   The parameter's type widens by the symbol; a default whose locals the
   body never reads keeps the call-site model. */
static void pdl_collect_writes(const NodeTable *nt, int id, const char **out, int cap, int *n) {
  if (id < 0 || *n >= cap) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode ||
      k == NK_BlockNode || k == NK_LambdaNode) return;
  if (k == NK_LocalVariableWriteNode || k == NK_LocalVariableTargetNode ||
      k == NK_LocalVariableOperatorWriteNode || k == NK_LocalVariableOrWriteNode ||
      k == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    int dup = 0;
    for (int i = 0; nm && i < *n; i++) if (sp_streq(out[i], nm)) { dup = 1; break; }
    if (nm && !dup) out[(*n)++] = nm;
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) pdl_collect_writes(nt, nd->r[j].ref, out, cap, n);
  for (int j = 0; j < nd->na; j++)
    for (int k2 = 0; k2 < nd->a[j].n; k2++) pdl_collect_writes(nt, nd->a[j].ids[k2], out, cap, n);
}
static int pdl_body_reads(const NodeTable *nt, int id, const char **names, int n) {
  if (id < 0) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return 0;
  if (k == NK_LocalVariableReadNode || k == NK_LocalVariableOperatorWriteNode ||
      k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    for (int i = 0; nm && i < n; i++) if (sp_streq(names[i], nm)) return 1;
  }
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) if (pdl_body_reads(nt, nd->r[j].ref, names, n)) return 1;
  for (int j = 0; j < nd->na; j++)
    for (int k2 = 0; k2 < nd->a[j].n; k2++) if (pdl_body_reads(nt, nd->a[j].ids[k2], names, n)) return 1;
  return 0;
}
int desugar_param_default_assigns_local(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int def = 0; def < n0; def++) {
    if (!fwd_node_is(nt, def, "DefNode")) continue;
    int ps[256]; int np = rd_params(nt, def, ps, 256);
    int pro[64]; int npro = 0;
    for (int i = 0; i < np && npro < 60; i++) {
      if (!fwd_node_is(nt, ps[i], "OptionalParameterNode") &&
          !fwd_node_is(nt, ps[i], "OptionalKeywordParameterNode")) continue;
      int v = nt_ref(nt, ps[i], "value");
      const char *pname = nt_str(nt, ps[i], "name");
      if (v < 0 || !pname) continue;
      const char *names[32]; int nn = 0;
      pdl_collect_writes(nt, v, names, 32, &nn);
      if (!nn || !pdl_body_reads(nt, nt_ref(nt, def, "body"), names, nn)) continue;
      BsB b = { nt, 1 };
      long long line = nt_int(nt, ps[i], "node_line", 0);
      for (int k = 0; k < nn && npro < 60; k++) {
        int w = bs_write(&b, names[k], bs_new(&b, "NilNode"));
        if (w >= 0) nt_node_set_int(nt, w, "node_line", line);
        pro[npro++] = w;
      }
      int sym = bs_new(&b, "SymbolNode");
      if (sym >= 0) nt_node_set_str(nt, sym, "value", "__sp_absent");
      int sym2 = bs_new(&b, "SymbolNode");
      if (sym2 >= 0) nt_node_set_str(nt, sym2, "value", "__sp_absent");
      int pred = bs_call(&b, bs_read(&b, pname), "==", &sym2, 1);
      int bind = bs_write(&b, pname, v);
      int guard = bs_if(&b, pred, &bind, 1, NULL, 0);
      if (!b.ok || sym < 0 || pred < 0 || bind < 0 || guard < 0) continue;
      nt_node_set_int(nt, pred, "node_line", line);
      nt_node_set_int(nt, bind, "node_line", line);
      nt_node_set_int(nt, guard, "node_line", line);
      pro[npro++] = guard;
      nt_node_set_ref(nt, ps[i], "value", sym);
    }
    if (!npro) continue;
    BsB b = { nt, 1 };
    int nbody = bs_prepend(&b, def, pro, npro);
    if (nbody < 0 || !b.ok) continue;
    nt_node_set_ref(nt, def, "body", nbody);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- `r[k] ||= v` / `r[k] &&= v` / `r[k] op= v` on a USER-class receiver ----
   The index and/or/op-assign emitters lower the builtin containers only; a
   receiver of a program class defining [] and []= (a registry wrapping a
   Hash, Concurrent::Map) was refused. Once the receiver's type is known
   (this runs in the typed fixpoint), such a node is rewritten to what Ruby
   defines it as:
     r[k] ||= v   ->  r[k] || (r[k] = v)          (an OrNode)
     r[k] &&= v   ->  r[k] && (r[k] = v)          (an AndNode)
     r[k] op= v   ->  r[k] = r[k] op v            (a []= call)
   with the receiver and key cloned for their second evaluation, so only a
   receiver and key without side effects (a variable, self, a constant, a
   literal) qualify; anything else stays refused as before. The []= call's
   value is that method's answer, where Ruby answers v -- the same for every
   []= that returns its value, which is the convention. */
static int ix_pure(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  switch (nt_kind(nt, n)) {
    case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
    case NK_ClassVariableReadNode: case NK_GlobalVariableReadNode:
    case NK_SelfNode: case NK_ConstantReadNode: case NK_ConstantPathNode:
    case NK_SymbolNode: case NK_IntegerNode: case NK_StringNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
      return 1;
    default: return 0;
  }
}
static int ix_index_call(NodeTable *nt, const char *name, int recv, int a0, int a1) {
  int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) return -1;
  int aa[2]; int n = 0; aa[n++] = a0; if (a1 >= 0) aa[n++] = a1;
  nt_node_set_arr(nt, na, "arguments", aa, n);
  int call = nt_new_node(nt, "CallNode"); if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", recv);
  nt_node_set_str(nt, call, "name", name);
  nt_node_set_ref(nt, call, "arguments", na);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}
int desugar_index_assign_user_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_IndexOrWriteNode && k != NK_IndexAndWriteNode && k != NK_IndexOperatorWriteNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int v = nt_ref(nt, id, "value");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || v < 0 || an != 1 || !av || nt_ref(nt, id, "block") >= 0) continue;
    if (!ix_pure(nt, recv) || !ix_pure(nt, av[0])) continue;
    NodeKind rk = nt_kind(nt, recv);
    if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
      /* `Mod[k] ||= v` on a module or class whose `[]` / `[]=` are its own
         class-level methods (activesupport's IsolatedExecutionState store):
         the same rewrite, calling the constant's methods */
      const char *cn = nt_str(nt, recv, "name");
      int cid = cn ? comp_class_index(c, cn) : -1;
      if (cid < 0 || cid >= c->nclasses) continue;
      if (comp_cmethod_in_chain(c, cid, "[]", NULL) < 0 || comp_cmethod_in_chain(c, cid, "[]=", NULL) < 0) continue;
    }
    else {
      TyKind rt = comp_ntype(c, recv);
      if (!ty_is_object(rt)) rt = infer_type(c, recv);   /* a local's type lives in its scope slot */
      if (!ty_is_object(rt)) continue;
      int cid = ty_object_class(rt);
      if (cid < 0 || cid >= c->nclasses) continue;
      if (comp_method_in_chain(c, cid, "[]", NULL) < 0 || comp_method_in_chain(c, cid, "[]=", NULL) < 0) continue;
    }
    const char *op = k == NK_IndexOperatorWriteNode ? nt_str(nt, id, "binary_operator") : NULL;
    if (k == NK_IndexOperatorWriteNode && !op) continue;
    int key = av[0];
    int base = nt->count;
    int recv2 = nt_clone_subtree(nt, recv), key2 = nt_clone_subtree(nt, key);
    if (recv2 < 0 || key2 < 0) continue;
    int read = ix_index_call(nt, "[]", recv, key, -1);
    if (read < 0) continue;
    if (op) {
      /* r[k] = (r[k] op v) */
      int opc = ix_index_call(nt, op, read, v, -1);
      int store = opc >= 0 ? ix_index_call(nt, "[]=", recv2, key2, opc) : -1;
      if (store < 0) continue;
      int na = nt_ref(nt, store, "arguments");
      nt_node_reset(nt, id, "CallNode");
      nt_node_set_ref(nt, id, "receiver", recv2);
      nt_node_set_str(nt, id, "name", "[]=");
      nt_node_set_ref(nt, id, "arguments", na);
      nt_node_set_ref(nt, id, "block", -1);
    }
    else {
      int store = ix_index_call(nt, "[]=", recv2, key2, v);
      if (store < 0) continue;
      nt_node_reset(nt, id, k == NK_IndexOrWriteNode ? "OrNode" : "AndNode");
      nt_node_set_ref(nt, id, "left", read);
      nt_node_set_ref(nt, id, "right", store);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    int cb = c->node_cbody ? c->node_cbody[id] : -1;
    for (int j = base; j < nt->count; j++) { c->nscope[j] = encl; if (c->node_cbody) c->node_cbody[j] = cb; }
    changed = 1;
  }
  return changed;
}

/* ---- `Klass.attr op= v` / `||=` / `&&=` on a class object ----
   The attribute op-assign emitters serve object receivers; a CONSTANT
   receiver (a class-level accessor, `Config.limit += 5`) was refused. The
   receiver evaluates without effect, so the node becomes what Ruby defines:
     Klass.a op= v   ->  Klass.a = Klass.a op v
     Klass.a ||= v   ->  Klass.a || (Klass.a = v)     (&&= likewise) */
static int ca_attr_call(NodeTable *nt, int recv, const char *name, int arg) {
  int cl = nt_new_node(nt, "CallNode"); if (cl < 0) return -1;
  nt_node_set_ref(nt, cl, "receiver", recv); nt_node_set_str(nt, cl, "name", name);
  if (arg >= 0) {
    int args = nt_new_node(nt, "ArgumentsNode"); if (args < 0) return -1;
    nt_node_set_arr(nt, args, "arguments", &arg, 1);
    nt_node_set_ref(nt, cl, "arguments", args);
  }
  else nt_node_set_ref(nt, cl, "arguments", -1);
  nt_node_set_ref(nt, cl, "block", -1);
  return cl;
}
/* `recv&.a op= v` (and `||=`, `&&=`) is nil when recv is: the read, the
   operator and the write all happen only for a receiver that is not nil.
   The op-write lowerings below take the receiver as present, so a nil one
   was called (a segfault through a writer with a body, a refusal for an
   accessor in value position). Before any of them runs it becomes

     (__snw_N = recv).nil? ? nil : __snw_N.a op= v

   with the receiver evaluated once, as CRuby does. The `if` stands where
   the op-write stood, so a statement stays a statement and its write is
   emitted as one. */
int desugar_safe_nav_attr_write(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !(sp_streq(ty, "CallOperatorWriteNode") || sp_streq(ty, "CallOrWriteNode") ||
                 sp_streq(ty, "CallAndWriteNode"))) continue;
    const char *cop = nt_str(nt, id, "call_operator");
    int recv = nt_ref(nt, id, "receiver");
    const char *rn = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (!cop || !sp_streq(cop, "&.") || recv < 0 || !rn || v < 0) continue;
    const char *bop = nt_str(nt, id, "binary_operator");
    char tyb[32], bopb[16];
    snprintf(tyb, sizeof tyb, "%s", ty);
    snprintf(bopb, sizeof bopb, "%s", bop ? bop : "");
    char rnb[256]; snprintf(rnb, sizeof rnb, "%s", rn);
    char tname[48]; snprintf(tname, sizeof tname, "__snw_%s", comp_node_tag(c, id));
    int w = nt_new_node(nt, "LocalVariableWriteNode");
    int pw = nt_new_node(nt, "ParenthesesNode");
    int pws = nt_new_node(nt, "StatementsNode");
    int r2 = nt_new_node(nt, "LocalVariableReadNode");
    int nq = nt_new_node(nt, "CallNode");
    int nil_n = nt_new_node(nt, "NilNode");
    int ts = nt_new_node(nt, "StatementsNode");
    int op = nt_new_node(nt, tyb);
    int es = nt_new_node(nt, "StatementsNode");
    int eln = nt_new_node(nt, "ElseNode");
    if (w < 0 || pw < 0 || pws < 0 || r2 < 0 || nq < 0 || nil_n < 0 || ts < 0 || op < 0 || es < 0 ||
        eln < 0) break;
    nt_node_set_str(nt, w, "name", tname); nt_node_set_int(nt, w, "depth", 0);
    nt_node_set_ref(nt, w, "value", recv);
    nt_node_set_arr(nt, pws, "body", &w, 1);
    nt_node_set_ref(nt, pw, "body", pws);
    nt_node_set_str(nt, r2, "name", tname); nt_node_set_int(nt, r2, "depth", 0);
    nt_node_set_str(nt, nq, "name", "nil?");
    nt_node_set_ref(nt, nq, "receiver", pw);
    nt_node_set_ref(nt, nq, "arguments", -1); nt_node_set_ref(nt, nq, "block", -1);
    nt_node_set_arr(nt, ts, "body", &nil_n, 1);
    /* the op-write itself, on the bound receiver, with a plain `.` */
    nt_node_set_ref(nt, op, "receiver", r2);
    nt_node_set_str(nt, op, "name", rnb);
    if (bopb[0]) nt_node_set_str(nt, op, "binary_operator", bopb);
    nt_node_set_ref(nt, op, "value", v);
    /* the source position of the op-write, which a refusal of the rewritten
       one (a receiver that is always nil, a value position) reports */
    long long ln = nt_int(nt, id, "node_line", 0), fl = nt_int(nt, id, "node_file", 0);
    {
      int pos[3] = { op, w, nq };
      for (int k = 0; k < 3 && ln > 0; k++) {
        nt_node_set_int(nt, pos[k], "node_line", ln);
        nt_node_set_int(nt, pos[k], "node_file", fl);
      }
    }
    nt_node_set_arr(nt, es, "body", &op, 1);
    nt_node_set_ref(nt, eln, "statements", es);
    nt_node_reset(nt, id, "IfNode");
    if (ln > 0) { nt_node_set_int(nt, id, "node_line", ln); nt_node_set_int(nt, id, "node_file", fl); }
    nt_node_set_ref(nt, id, "predicate", nq);
    nt_node_set_ref(nt, id, "statements", ts);
    nt_node_set_ref(nt, id, "subsequent", eln);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

int desugar_const_attr_op_assign(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int is_op = sp_streq(ty, "CallOperatorWriteNode"), is_or = sp_streq(ty, "CallOrWriteNode"), is_and = sp_streq(ty, "CallAndWriteNode");
    if (!is_op && !is_or && !is_and) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || (nt_kind(nt, recv) != NK_ConstantReadNode && nt_kind(nt, recv) != NK_ConstantPathNode)) continue;
    /* the dumper spells the attribute once, as `name`; the writer is name= */
    const char *rn = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (!rn || !rn[0] || v < 0 || nt_ref(nt, id, "arguments") >= 0) continue;
    char wn[200]; snprintf(wn, sizeof wn, "%s=", rn);
    const char *op = is_op ? nt_str(nt, id, "binary_operator") : NULL;
    if (is_op && !op) continue;
    int recv2 = nt_clone_subtree(nt, recv); if (recv2 < 0) continue;
    int read = ca_attr_call(nt, recv, rn, -1); if (read < 0) continue;
    if (op) {
      int opc = ca_attr_call(nt, read, op, v); if (opc < 0) continue;
      int store = ca_attr_call(nt, recv2, wn, opc); if (store < 0) continue;
      int na = nt_ref(nt, store, "arguments");
      nt_node_reset(nt, id, "CallNode");
      nt_node_set_ref(nt, id, "receiver", recv2); nt_node_set_str(nt, id, "name", wn);
      nt_node_set_ref(nt, id, "arguments", na); nt_node_set_ref(nt, id, "block", -1);
    }
    else {
      int store = ca_attr_call(nt, recv2, wn, v); if (store < 0) continue;
      nt_node_reset(nt, id, is_or ? "OrNode" : "AndNode");
      nt_node_set_ref(nt, id, "left", read); nt_node_set_ref(nt, id, "right", store);
    }
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- the ffi gem's attach_function ----
 *
 * `attach_function :name, [types], :ret` in a module that extends
 * FFI::Library defines `name` on the module when the body runs (the bundled
 * packages/ffi/ffi.rb). A method's name has to exist when the program is
 * compiled, so each literal-named attach also gets its definition here:
 *
 *   def self.name(*__ffi_a, &__ffi_b) = __ffi_call(:name, __ffi_a, __ffi_b)
 *
 * __ffi_call is FFI::Library's, reached through the module's `extend`; it
 * finds the Function the attach made and invokes it. The attach call itself
 * stays where it was written, so symbol lookup, NotFoundError and a `rescue`
 * around it behave as in CRuby. `attach_variable :name, ...` gets the reader
 * and writer the gem defines the same way. */

/* `extend FFI::Library` or `extend Fiddle::Importer` (or `extend ::FFI::Library`, which an earlier pass
   has already made relative) among a module or class body's statements. */
static int body_extends_ffi_library(const NodeTable *nt, int body) {
  int n = 0;
  const int *st = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = st[k];
    if (nt_kind(nt, s) != NK_CallNode || nt_ref(nt, s, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || !sp_streq(nm, "extend")) continue;
    int an = nt_ref(nt, s, "arguments");
    int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    for (int j = 0; j < ac; j++) {
      if (nt_kind(nt, av[j]) != NK_ConstantPathNode) continue;
      const char *leaf = nt_str(nt, av[j], "name");
      int par = nt_ref(nt, av[j], "parent");
      const char *pn = par >= 0 ? nt_str(nt, par, "name") : NULL;
      if (leaf && pn && sp_streq(leaf, "Library") && sp_streq(pn, "FFI")) return 1;
      if (leaf && pn && sp_streq(leaf, "Importer") && sp_streq(pn, "Fiddle")) return 1;
    }
  }
  return 0;
}

/* Is `mn` (a module/class leaf name) extended with FFI::Library by any of
   its bodies? Reopenings count: the attach may sit in a later file. */
/* The bundled package is in the program (it defines FFI__Registry): without
   it -- no libffi where spinel was built -- `extend FFI::Library` modules are
   the builtin FFI DSL's. */
static int ffi_package_loaded(const NodeTable *nt) {
  static const NodeTable *memo_nt; static int memo;
  if (memo_nt == nt) return memo;
  memo_nt = nt; memo = 0;
  NT_FOREACH_KIND(nt, NK_ModuleNode, m) {
    int cp = nt_ref(nt, m, "constant_path");
    const char *n = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (n && sp_streq(n, "FFI__Registry")) { memo = 1; break; }
  }
  return memo;
}

static int ffi_library_module(const NodeTable *nt, const char *mn, int n0) {
  if (!ffi_package_loaded(nt)) return 0;
  for (int m = 0; m < n0; m++) {
    NodeKind k = nt_kind(nt, m);
    if (k != NK_ModuleNode && k != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (nm && sp_streq(nm, mn) && body_extends_ffi_library(nt, nt_ref(nt, m, "body"))) return 1;
  }
  return 0;
}

static const char *ffi_literal_name(const NodeTable *nt, int node) {
  if (node < 0) return NULL;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_SymbolNode) return nt_str(nt, node, "value");
  if (k == NK_StringNode) {
    const char *s = nt_str(nt, node, "content");
    return s ? s : nt_str(nt, node, "unescaped");
  }
  return NULL;
}

/* Collect the literal names of attach_function / attach_variable calls made
   directly by the module body -- including inside begin/rescue, if/unless
   and similar statement wrappers, but not inside method bodies, blocks or
   nested modules. kind: 0 function, 1 variable. */
typedef struct { char *name; int kind; int at; } FfiAttach;
static void ffi_collect_attaches(const NodeTable *nt, int node, FfiAttach **out, int *n, int *cap) {
  if (node < 0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_SingletonClassNode)
    return;
  if (k == NK_CallNode) {
    const char *nm = nt_str(nt, node, "name");
    int r = nt_ref(nt, node, "receiver");
    if (nm && (r < 0 || nt_kind(nt, r) == NK_SelfNode) &&
        (sp_streq(nm, "attach_function") || sp_streq(nm, "attach_variable"))) {
      int an = nt_ref(nt, node, "arguments");
      int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      const char *fname = ac > 0 ? ffi_literal_name(nt, av[0]) : NULL;
      if (fname) {
        if (*n == *cap) {
          *cap = *cap ? *cap * 2 : 16;
          *out = realloc(*out, sizeof(FfiAttach) * (size_t)*cap);
          if (!*out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        (*out)[*n].name = strdup(fname);
        (*out)[*n].kind = sp_streq(nm, "attach_variable") ? 1 : 0;
        (*out)[*n].at = node;
        (*n)++;
      }
      return;
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) ffi_collect_attaches(nt, nt_ref_at(nt, node, i), out, n, cap);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0;
    const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) ffi_collect_attaches(nt, ids[j], out, n, cap);
  }
}

static int ffi_symbol(NodeTable *nt, int like, const char *name) {
  int sy = fwd_new_node_like(nt, like, "SymbolNode");
  if (sy < 0) return -1;
  nt_node_set_str(nt, sy, "value", name);
  nt_node_set_str(nt, sy, "unescaped", name);
  return sy;
}

/* def self.<mname>(<params>) = <helper>(:<key>, <args...>) */
static int ffi_forwarding_def(NodeTable *nt, int like, const char *mname, const char *helper,
                              const char *key, int with_rest, int with_block, int with_value) {
  int def = fwd_new_node_like(nt, like, "DefNode");
  int self = fwd_new_node_like(nt, like, "SelfNode");
  int body = fwd_new_node_like(nt, like, "StatementsNode");
  int call = fwd_new_node_like(nt, like, "CallNode");
  int args = fwd_new_node_like(nt, like, "ArgumentsNode");
  if (def < 0 || self < 0 || body < 0 || call < 0 || args < 0) return -1;
  int av[4]; int ac = 0;
  av[ac++] = ffi_symbol(nt, like, key);
  if (with_rest || with_block || with_value) {
    int ps = fwd_new_node_like(nt, like, "ParametersNode");
    if (ps < 0) return -1;
    if (with_rest) {
      int rp = fwd_new_node_like(nt, like, "RestParameterNode");
      nt_node_set_str(nt, rp, "name", "ffi_a__");
      nt_node_set_ref(nt, ps, "rest", rp);
      av[ac++] = local_read_like(nt, like, "ffi_a__");
    }
    if (with_value) {
      int rq = fwd_new_node_like(nt, like, "RequiredParameterNode");
      nt_node_set_str(nt, rq, "name", "ffi_v__");
      nt_node_set_arr(nt, ps, "requireds", &rq, 1);
      av[ac++] = local_read_like(nt, like, "ffi_v__");
    }
    if (with_block) {
      int bp = fwd_new_node_like(nt, like, "BlockParameterNode");
      nt_node_set_str(nt, bp, "name", "ffi_b__");
      nt_node_set_ref(nt, ps, "block", bp);
      av[ac++] = local_read_like(nt, like, "ffi_b__");
    }
    nt_node_set_ref(nt, def, "parameters", ps);
  }
  nt_node_set_arr(nt, args, "arguments", av, ac);
  nt_node_set_str(nt, call, "name", helper);
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_arr(nt, body, "body", &call, 1);
  nt_node_set_str(nt, def, "name", mname);
  nt_node_set_ref(nt, def, "receiver", self);
  nt_node_set_ref(nt, def, "body", body);
  return def;
}

/* The ffi gem's builtin type names: a parameter spelled with one of these
   is never a callback, so it never takes the call's block. */
static int ffi_builtin_type_name(const char *n) {
  static const char *const T[] = {
    "void", "bool", "char", "uchar", "short", "ushort", "int", "uint", "long", "ulong",
    "long_long", "ulong_long", "float", "double", "long_double", "pointer", "string", "strptr",
    "buffer_in", "buffer_out", "buffer_inout", "int8", "uint8", "int16", "uint16", "int32",
    "uint32", "int64", "uint64", "float32", "float64", "size_t", "ssize_t", "intptr_t",
    "uintptr_t", "ptrdiff_t", "off_t", "time_t", "pid_t", "uid_t", "gid_t", "mode_t",
    "socklen_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "int64_t", NULL };
  return str_in(n, T);
}

/* def self.<mname>(__ffi_a0, ..., [&__ffi_b]) = __ffi_call(:<key>, [__ffi_a0, ...], __ffi_b)
   -- the fixed-arity form, for an attach whose parameter list is a literal:
   an arity the compiler knows is one `method(:name)` and every other
   dispatch path can bind (a *rest method is not). */
static int ffi_fixed_def(NodeTable *nt, int like, const char *mname, const char *key, int nparams, int with_block) {
  int def = fwd_new_node_like(nt, like, "DefNode");
  int self = fwd_new_node_like(nt, like, "SelfNode");
  int body = fwd_new_node_like(nt, like, "StatementsNode");
  int call = fwd_new_node_like(nt, like, "CallNode");
  int args = fwd_new_node_like(nt, like, "ArgumentsNode");
  int arr = fwd_new_node_like(nt, like, "ArrayNode");
  int ps = fwd_new_node_like(nt, like, "ParametersNode");
  if (def < 0 || self < 0 || body < 0 || call < 0 || args < 0 || arr < 0 || ps < 0) return -1;
  int *rq = malloc(sizeof(int) * (size_t)(nparams > 0 ? nparams : 1));
  int *el = malloc(sizeof(int) * (size_t)(nparams > 0 ? nparams : 1));
  /* a trailing callback may come as the call's block instead: it is then
     optional, and __ffi_call puts the block in its place */
  int nreq = with_block && nparams > 0 ? nparams - 1 : nparams;
  for (int i = 0; i < nparams; i++) {
    char pn[32]; snprintf(pn, sizeof pn, "ffi_a%d__", i);
    if (i < nreq) {
      rq[i] = fwd_new_node_like(nt, like, "RequiredParameterNode");
      nt_node_set_str(nt, rq[i], "name", pn);
    }
    else {
      int op = fwd_new_node_like(nt, like, "OptionalParameterNode");
      int nilv = fwd_new_node_like(nt, like, "NilNode");
      nt_node_set_str(nt, op, "name", pn);
      nt_node_set_ref(nt, op, "value", nilv);
      nt_node_set_arr(nt, ps, "optionals", &op, 1);
    }
    el[i] = local_read_like(nt, like, pn);
  }
  nt_node_set_arr(nt, ps, "requireds", rq, nreq);
  nt_node_set_arr(nt, arr, "elements", el, nparams);
  free(rq); free(el);
  int av[3]; int ac = 0;
  av[ac++] = ffi_symbol(nt, like, key);
  av[ac++] = arr;
  if (with_block) {
    int bp = fwd_new_node_like(nt, like, "BlockParameterNode");
    nt_node_set_str(nt, bp, "name", "ffi_b__");
    nt_node_set_ref(nt, ps, "block", bp);
    av[ac++] = local_read_like(nt, like, "ffi_b__");
  }
  nt_node_set_ref(nt, def, "parameters", ps);
  nt_node_set_arr(nt, args, "arguments", av, ac);
  nt_node_set_str(nt, call, "name", "__ffi_call");
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_arr(nt, body, "body", &call, 1);
  nt_node_set_str(nt, def, "name", mname);
  nt_node_set_ref(nt, def, "receiver", self);
  nt_node_set_ref(nt, def, "body", body);
  return def;
}

/* The literal parameter list of an attach_function call, or -1: its element
   count in *n, and whether the last element could be a callback. */
static int ffi_attach_params(const NodeTable *nt, int call, int *n, int *maybe_cb) {
  int an = nt_ref(nt, call, "arguments");
  int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  if (ac < 3) return -1;
  int arr = nt_kind(nt, av[1]) == NK_ArrayNode ? av[1] : (ac >= 4 && nt_kind(nt, av[2]) == NK_ArrayNode ? av[2] : -1);
  if (arr < 0) return -1;
  int en = 0; const int *els = nt_arr(nt, arr, "elements", &en);
  for (int i = 0; i < en; i++) {
    if (nt_kind(nt, els[i]) == NK_SplatNode) return -1;
    const char *v = nt_kind(nt, els[i]) == NK_SymbolNode ? nt_str(nt, els[i], "value") : NULL;
    if (v && sp_streq(v, "varargs")) return -1;
  }
  *n = en;
  *maybe_cb = 0;
  if (en > 0) {
    int last = els[en - 1];
    const char *v = nt_kind(nt, last) == NK_SymbolNode ? nt_str(nt, last, "value") : NULL;
    *maybe_cb = !(v && ffi_builtin_type_name(v));
  }
  return arr;
}

/* does the body already define `def self.<name>`? */
static int body_defines_smethod(const NodeTable *nt, const int *st, int n, const char *name) {
  for (int k = 0; k < n; k++) {
    if (nt_kind(nt, st[k]) != NK_DefNode) continue;
    int r = nt_ref(nt, st[k], "receiver");
    const char *dn = nt_str(nt, st[k], "name");
    if (r >= 0 && nt_kind(nt, r) == NK_SelfNode && dn && sp_streq(dn, name)) return 1;
  }
  return 0;
}

int comp_ffi_library_module(const NodeTable *nt, const char *mn) {
  return mn && ffi_library_module(nt, mn, nt->count);
}

int desugar_ffi_library_functions(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  for (int m = 0; m < n0; m++) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ModuleNode && mk != NK_ClassNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    int body = nt_ref(nt, m, "body");
    if (!mn || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    if (!ffi_library_module(nt, mn, n0)) continue;
    FfiAttach *at = NULL; int na = 0, cap = 0;
    ffi_collect_attaches(nt, body, &at, &na, &cap);
    if (na == 0) { free(at); continue; }
    int n = 0;
    const int *st = nt_arr(nt, body, "body", &n);
    int *out = malloc(sizeof(int) * (size_t)(n + na * 2 + 1));
    if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(out, st, sizeof(int) * (size_t)n);
    int no = n;
    for (int i = 0; i < na; i++) {
      int dup = 0;
      for (int j = 0; j < i; j++) if (at[j].kind == at[i].kind && sp_streq(at[j].name, at[i].name)) dup = 1;
      if (dup || body_defines_smethod(nt, st, n, at[i].name)) continue;
      if (at[i].kind == 0) {
        int np = 0, mcb = 0;
        int d = ffi_attach_params(nt, at[i].at, &np, &mcb) >= 0
                  ? ffi_fixed_def(nt, at[i].at, at[i].name, at[i].name, np, mcb)
                  : ffi_forwarding_def(nt, at[i].at, at[i].name, "__ffi_call", at[i].name, 1, 1, 0);
        if (d >= 0) out[no++] = d;
      }
      else {
        char wn[512];
        snprintf(wn, sizeof wn, "%s=", at[i].name);
        int g = ffi_forwarding_def(nt, at[i].at, at[i].name, "__ffi_var_get", at[i].name, 0, 0, 0);
        int s = ffi_forwarding_def(nt, at[i].at, wn, "__ffi_var_set", at[i].name, 0, 0, 1);
        if (g >= 0) out[no++] = g;
        if (s >= 0) out[no++] = s;
      }
    }
    nt_node_set_arr(nt, body, "body", out, no);
    free(out);
    for (int i = 0; i < na; i++) free(at[i].name);
    free(at);
    changed = 1;
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- calls into an FFI library that no definition names ----
 *
 * Binding generators attach functions whose names come from data -- a table
 * of symbols walked in a method (sdl2-bindings, raylib-bindings, opengl), or
 * a name computed from another (raylib's snake_case mode) -- and programs
 * call them on the module (`SDL.Init(...)`) or, having included it, bare
 * (`InitWindow(...)`). No `def` of those names exists to compile, so a call
 * that resolves to nothing is sent through the attached-function table at
 * run time instead:
 *
 *   Mod.Name(args)  ->  Mod.__ffi_call(:Name, [args])        (block kept)
 *   Name(args)      ->  FFI__Registry.__ffi_dispatch([:Mod, ...], :Name, [args])
 *                                                           (block kept)
 *
 * The receiver form applies to a module that extends FFI::Library and has
 * no class method of the name; the bare form to a call no method in the
 * program defines, made where such a module is in reach -- inside its own
 * methods, or in a class (or the top level) that includes it. Both answer
 * NoMethodError at run time when nothing was attached under the name. */
static int ffi_kernel_name(const char *n) {
  static const char *const K[] = {
    "puts", "print", "p", "pp", "printf", "sprintf", "format", "gets", "require", "require_relative",
    "load", "raise", "fail", "loop", "lambda", "proc", "rand", "srand", "sleep", "exit", "exit!", "abort",
    "at_exit", "catch", "throw", "binding", "block_given?", "iterator?", "caller", "caller_locations",
    "freeze", "frozen?", "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex",
    "system", "exec", "spawn", "fork", "trap", "open", "select", "warn", "autoload", "`", "putc",
    "readline", "readlines", "__method__", "__dir__", "__callee__", "define_method", "attr_accessor",
    "attr_reader", "attr_writer", "attr", "include", "extend", "prepend", "private", "public",
    "protected", "module_function", "alias_method", "tap", "then", "yield_self", "instance_variable_get",
    "instance_variable_set", "instance_variables", "instance_variable_defined?", "respond_to?", "send",
    "public_send", "__send__", "method", "methods", "is_a?", "kind_of?", "instance_of?", "nil?",
    "class", "object_id", "hash", "inspect", "to_s", "dup", "clone", "itself", "display",
    "private_constant", "public_constant", "const_get", "const_set", "const_defined?", "constants",
    "instance_eval", "instance_exec", "class_eval", "module_eval", "class_exec", "define_singleton_method",
    "singleton_class", "extend_object", "new", "allocate", "superclass", "name", "ancestors",
    "private_class_method", "public_class_method", "ffi_lib", "ffi_lib_flags", "ffi_convention",
    "attach_function", "attach_variable", "callback", "typedef", "enum", "bitmask", "find_type",
    "enum_type", "enum_value", "ffi_libraries", "__ffi_call", "equal?", "eql?", "==", "!=", "!",
    "=~", "===", "<=>", "instance_variables", "local_variables", "global_variables", "sprintf",
    "gsub", "sub", "chomp", "chop", "test", "set_trace_func", "trace_var", "untrace_var",
    "ObjectSpace", "GC", "binding", "private_method_defined?", "method_defined?",
    "public_method_defined?", "instance_method", "instance_methods", "remove_method", "undef_method",
    "pack", "unpack", "unpack1", "exit_status", "Pathname", "BigDecimal", "URI",
    NULL };
  return str_in(n, K);
}

static int ffi_cls_is_lib(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses) return 0;
  const char *nm = c->classes[ci].name;
  if (!nm) return 0;
  const char *leaf = strrchr(nm, ':');
  leaf = leaf ? leaf + 1 : nm;
  return comp_ffi_library_module(c->nt, leaf);
}

/* The FFI library modules a bare call in scope `s` reaches, in the order
   Ruby's method lookup meets them: the module itself (its own class methods
   and body), those included into the scope's class chain (the latest include
   first, a class before its superclass), then those included at the top
   level. At most `max` are written to `out`; the count is returned. */
static void ffi_reach_add(Compiler *c, int k, int *out, int *n, int max) {
  if (!ffi_cls_is_lib(c, k)) return;
  for (int q = 0; q < *n; q++) if (out[q] == k) return;
  if (*n < max) out[(*n)++] = k;
}

static int ffi_bare_reach(Compiler *c, Scope *s, int *out, int max) {
  int n = 0;
  int ci = s ? s->class_id : -1;
  if (ci >= 0) ffi_reach_add(c, ci, out, &n, max);
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    ClassInfo *ki = &c->classes[k];
    for (int j = ki->nincluded_mods - 1; j >= 0; j--) ffi_reach_add(c, ki->included_mods[j], out, &n, max);
  }
  for (int j = c->ntoplevel_includes - 1; j >= 0; j--)
    ffi_reach_add(c, c->toplevel_includes[j], out, &n, max);
  return n;
}

/* The call's arguments (and nothing else) as one Array literal. */
static int ffi_args_array(NodeTable *nt, int call) {
  int arr = fwd_new_node_like(nt, call, "ArrayNode");
  if (arr < 0) return -1;
  int an = nt_ref(nt, call, "arguments");
  int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  int *cp = ac > 0 ? malloc(sizeof(int) * (size_t)ac) : NULL;
  if (ac > 0 && !cp) return -1;
  if (ac > 0) memcpy(cp, av, sizeof(int) * (size_t)ac);
  nt_node_set_arr(nt, arr, "elements", cp, ac);
  free(cp);
  return arr;
}

/* Stamp every node of a class body with the class, stopping at method
   bodies and nested classes (which have a self of their own). */
static void ffi_mark_body(const NodeTable *nt, int node, int ci, int *out, int n0) {
  if (node < 0 || node >= n0) return;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return;
  out[node] = ci;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) ffi_mark_body(nt, nt_ref_at(nt, node, i), ci, out, n0);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0;
    const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int j = 0; j < cnt; j++) ffi_mark_body(nt, ids[j], ci, out, n0);
  }
}

/* `class << self; attr_accessor :logger; def x; end; end` inside the body of
   class ci: its singleton methods, which are not (yet) cmethods here. */
static int ffi_sclass_defines(Compiler *c, int ci, const char *name) {
  const NodeTable *nt = c->nt;
  if (ci < 0 || !name) return 0;
  size_t nl = strlen(name);
  for (int m = 0; m < nt->count; m++) {
    NodeKind mk = nt_kind(nt, m);
    if (mk != NK_ClassNode && mk != NK_ModuleNode) continue;
    int cp = nt_ref(nt, m, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || comp_class_index(c, cn) != ci) continue;
    int body = nt_ref(nt, m, "body");
    int bn = 0; const int *bs = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++) {
      if (nt_kind(nt, bs[k]) != NK_SingletonClassNode) continue;
      int ex = nt_ref(nt, bs[k], "expression");
      if (ex < 0 || nt_kind(nt, ex) != NK_SelfNode) continue;
      int sb = nt_ref(nt, bs[k], "body");
      int sn = 0; const int *ss = sb >= 0 ? nt_arr(nt, sb, "body", &sn) : NULL;
      for (int q = 0; q < sn; q++) {
        NodeKind qk = nt_kind(nt, ss[q]);
        const char *qn = nt_str(nt, ss[q], "name");
        if (!qn) continue;
        if (qk == NK_DefNode && sp_streq(qn, name)) return 1;
        if (qk != NK_CallNode || nt_ref(nt, ss[q], "receiver") >= 0) continue;
        int rd = sp_streq(qn, "attr_reader") || sp_streq(qn, "attr_accessor") || sp_streq(qn, "attr");
        int wr = sp_streq(qn, "attr_writer") || sp_streq(qn, "attr_accessor");
        if (!rd && !wr) continue;
        int an = nt_ref(nt, ss[q], "arguments");
        int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
        for (int a = 0; a < ac; a++) {
          const char *sym = ffi_literal_name(nt, av[a]);
          if (!sym) continue;
          if (rd && sp_streq(sym, name)) return 1;
          if (wr && strlen(sym) + 1 == nl && strncmp(sym, name, nl - 1) == 0 && name[nl - 1] == '=') return 1;
        }
      }
    }
  }
  return 0;
}

int rewrite_ffi_dynamic_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int any_lib = 0;
  for (int k = 0; k < c->nclasses && !any_lib; k++) if (ffi_cls_is_lib(c, k)) any_lib = 1;
  if (!any_lib) return 0;
  int n0 = nt->count;
  int first_new = n0;
  int changed = 0;
  /* the class whose body (not one of its methods) each node sits in */
  int *body_cls = malloc(sizeof(int) * (size_t)(n0 > 0 ? n0 : 1));
  if (body_cls) {
    for (int i = 0; i < n0; i++) body_cls[i] = -1;
    for (int m = 0; m < n0; m++) {
      NodeKind mk = nt_kind(nt, m);
      if (mk != NK_ClassNode && mk != NK_ModuleNode) continue;
      int cp = nt_ref(nt, m, "constant_path");
      int ci = cp >= 0 ? comp_class_index(c, nt_str(nt, cp, "name")) : -1;
      if (ci < 0) continue;
      ffi_mark_body(nt, nt_ref(nt, m, "body"), ci, body_cls, n0);
    }
  }
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name || !*name || ffi_kernel_name(name)) continue;
    if (strncmp(name, "__ffi", 5) == 0) continue;
    /* the compile-time FFI and native-binding declarations are not calls */
    if (strncmp(name, "ffi_", 4) == 0 || strncmp(name, "native_", 7) == 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    int sc = id < c->node_cap ? c->nscope[id] : 0;
    Scope *s = (sc >= 0 && sc < c->nscopes) ? &c->scopes[sc] : NULL;
    if (recv >= 0) {
      NodeKind rk = nt_kind(nt, recv);
      if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) continue;
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (!ffi_cls_is_lib(c, ci)) continue;
      if (comp_cmethod_in_chain(c, ci, name, NULL) >= 0) continue;
      if (ffi_sclass_defines(c, ci, name)) continue;
      /* a nested class or constant written as a call (`Mod::Name`) is not one */
      if (name[strlen(name) - 1] == '=') continue;
      int arr = ffi_args_array(nt, id);
      int sym = ffi_symbol(nt, id, name);
      int args = fwd_new_node_like(nt, id, "ArgumentsNode");
      if (arr < 0 || sym < 0 || args < 0) continue;
      int av[2] = { sym, arr };
      nt_node_set_arr(nt, args, "arguments", av, 2);
      nt_node_set_ref(nt, id, "arguments", args);
      nt_node_set_str(nt, id, "name", "__ffi_call");
      changed = 1;
    }
    else {
      if (comp_method_index(c, name) >= 0) continue;
      int bk = body_cls ? body_cls[id] : -1;
      int obj = comp_class_index(c, "Object");
      if (bk >= 0) {
        /* a class-body statement: self is the class */
        if (comp_cmethod_in_chain(c, bk, name, NULL) >= 0) continue;
        if (ffi_sclass_defines(c, bk, name)) continue;
      }
      else if (s && s->is_cmethod && s->class_id >= 0) {
        if (comp_cmethod_in_chain(c, s->class_id, name, NULL) >= 0) continue;
        if (ffi_sclass_defines(c, s->class_id, name)) continue;
      }
      else if (s && s->class_id >= 0) {
        if (comp_method_in_chain(c, s->class_id, name, NULL) >= 0 ||
            comp_reader_in_chain(c, s->class_id, name, NULL)) continue;
      }
      if (obj >= 0 && comp_method_in_chain(c, obj, name, NULL) >= 0) continue;
      Scope *reach_s = s;
      Scope body_s;
      if (bk >= 0) { memset(&body_s, 0, sizeof body_s); body_s.class_id = bk; reach_s = &body_s; }
      int libs[32];
      int nlibs = ffi_bare_reach(c, reach_s, libs, 32);
      if (nlibs == 0) continue;
      /* a bare word could be a local only the parser knew about; those are
         LocalVariableReadNodes already, so a CallNode here is a call */
      int owners = fwd_new_node_like(nt, id, "ArrayNode");
      int osyms[32], ok = owners >= 0;
      for (int j = 0; j < nlibs && ok; j++) {
        const char *rn = class_ruby_name(c, libs[j]);
        osyms[j] = rn ? ffi_symbol(nt, id, rn) : -1;
        if (osyms[j] < 0) ok = 0;
      }
      if (!ok) continue;
      nt_node_set_arr(nt, owners, "elements", osyms, nlibs);
      int arr = ffi_args_array(nt, id);
      int sym = ffi_symbol(nt, id, name);
      int args = fwd_new_node_like(nt, id, "ArgumentsNode");
      int reg = fwd_new_node_like(nt, id, "ConstantReadNode");
      if (arr < 0 || sym < 0 || args < 0 || reg < 0) continue;
      nt_node_set_str(nt, reg, "name", "FFI__Registry");
      int av[3] = { owners, sym, arr };
      nt_node_set_arr(nt, args, "arguments", av, 3);
      nt_node_set_ref(nt, id, "arguments", args);
      nt_node_set_ref(nt, id, "receiver", reg);
      nt_node_set_str(nt, id, "name", "__ffi_dispatch");
      changed = 1;
    }
  }
  free(body_cls);
  if (changed) {
    comp_grow_node_arrays(c);
    /* the new nodes belong to the scope of the call they were made for:
       each was created right after the node it annotates, so walk the
       parents' refs to stamp them */
    for (int id = 0; id < n0; id++) {
      if (nt_kind(nt, id) != NK_CallNode) continue;
      int sc = c->nscope[id];
      int an = nt_ref(nt, id, "arguments");
      if (an >= first_new) {
        c->nscope[an] = sc;
        int ac = 0; const int *av = nt_arr(nt, an, "arguments", &ac);
        for (int j = 0; j < ac; j++) {
          if (av[j] < first_new) continue;
          c->nscope[av[j]] = sc;
          /* the owners Array's Symbols are new too */
          int ec = 0; const int *ev = nt_kind(nt, av[j]) == NK_ArrayNode ? nt_arr(nt, av[j], "elements", &ec) : NULL;
          for (int e = 0; e < ec; e++) if (ev[e] >= first_new) c->nscope[ev[e]] = sc;
        }
      }
      int r = nt_ref(nt, id, "receiver");
      if (r >= first_new) c->nscope[r] = sc;
    }
  }
  return changed;
}
static int ma_stmts1(NodeTable *nt, int st) {
  int b = nt_new_node(nt, "StatementsNode"); if (b < 0) return -1;
  nt_node_set_arr(nt, b, "body", &st, 1);
  return b;
}
/* `def name(val)` / `def self.name` with a one-statement body */
static int ma_def(NodeTable *nt, const char *name, int self_recv, int with_val, int body_stmt, long long line) {
  int def = nt_new_node(nt, "DefNode"); if (def < 0) return -1;
  nt_node_set_str(nt, def, "name", name);
  nt_node_set_int(nt, def, "node_line", line);
  if (self_recv) { int sf = nt_new_node(nt, "SelfNode"); if (sf < 0) return -1; nt_node_set_ref(nt, def, "receiver", sf); }
  else nt_node_set_ref(nt, def, "receiver", -1);
  if (with_val) {
    int params = nt_new_node(nt, "ParametersNode"); if (params < 0) return -1;
    int rp = nt_new_node(nt, "RequiredParameterNode"); if (rp < 0) return -1;
    nt_node_set_str(nt, rp, "name", "val");
    nt_node_set_arr(nt, params, "requireds", &rp, 1);
    nt_node_set_ref(nt, def, "parameters", params);
  }
  else nt_node_set_ref(nt, def, "parameters", -1);
  int body = ma_stmts1(nt, body_stmt); if (body < 0) return -1;
  nt_node_set_ref(nt, def, "body", body);
  return def;
}
/* ---- `singleton_class.attr_accessor :x` in a class or module body ----
   Accessors on the class object over its class-level ivar: what
   `def self.x; @x; end` / `def self.x=(val); @x = val; end` spell, and the
   one singleton_class idiom that never needs the singleton class as a value
   (which stays unsupported: docs/limitations.md). attr_reader / attr_writer
   likewise; symbol or string names, or the call is left alone. */
int desugar_singleton_attr(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ModuleNode && k != NK_ClassNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb0 = nt_arr(nt, body, "body", &bn);
    int *bb = (int *)malloc(sizeof(int) * (size_t)(bn > 0 ? bn : 1));
    if (!bb) continue;
    memcpy(bb, bb0, sizeof(int) * (size_t)bn);
    int *nb = NULL, nbn = 0, any = 0;
    for (int i = 0; i < bn; i++) {
      int st = bb[i];
      const char *nm = nt_kind(nt, st) == NK_CallNode ? nt_str(nt, st, "name") : NULL;
      int reader = 0, writer = 0;
      if (nm && sp_streq(nm, "attr_accessor")) reader = writer = 1;
      else if (nm && sp_streq(nm, "attr_reader")) reader = 1;
      else if (nm && sp_streq(nm, "attr_writer")) writer = 1;
      int rcv = nm ? nt_ref(nt, st, "receiver") : -1;
      int is_sc = rcv >= 0 && nt_kind(nt, rcv) == NK_CallNode && nt_str(nt, rcv, "name") &&
                  sp_streq(nt_str(nt, rcv, "name"), "singleton_class") &&
                  nt_ref(nt, rcv, "arguments") < 0 && nt_ref(nt, rcv, "block") < 0 &&
                  (nt_ref(nt, rcv, "receiver") < 0 || nt_kind(nt, nt_ref(nt, rcv, "receiver")) == NK_SelfNode);
      if (!(reader || writer) || !is_sc || nt_ref(nt, st, "block") >= 0) { xc_push(&nb, &nbn, st); continue; }
      int an = 0; int args = nt_ref(nt, st, "arguments");
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      const char *syms[32]; int nsyms = 0, ok = an > 0;
      for (int a = 0; a < an && ok; a++) {
        NodeKind ak = nt_kind(nt, av[a]);
        const char *t = ak == NK_SymbolNode ? nt_str(nt, av[a], "value") : ak == NK_StringNode ? nt_str(nt, av[a], "content") : NULL;
        if (!t || !t[0] || nsyms >= 32) ok = 0; else syms[nsyms++] = t;
      }
      if (!ok) { xc_push(&nb, &nbn, st); continue; }
      long long line = nt_int(nt, st, "node_line", 0);
      for (int j = 0; j < nsyms && ok; j++) {
        char iv[300], wn[300]; snprintf(iv, sizeof iv, "@%s", syms[j]); snprintf(wn, sizeof wn, "%s=", syms[j]);
        if (reader) {
          int rd = nt_new_node(nt, "InstanceVariableReadNode"); if (rd < 0) { ok = 0; break; }
          nt_node_set_str(nt, rd, "name", iv);
          int d = ma_def(nt, syms[j], 1, 0, rd, line); if (d < 0) { ok = 0; break; }
          xc_push(&nb, &nbn, d);
        }
        if (writer) {
          int w = nt_new_node(nt, "InstanceVariableWriteNode"); if (w < 0) { ok = 0; break; }
          nt_node_set_str(nt, w, "name", iv);
          int vr = nt_new_node(nt, "LocalVariableReadNode"); if (vr < 0) { ok = 0; break; }
          nt_node_set_str(nt, vr, "name", "val"); nt_node_set_int(nt, vr, "depth", 0);
          nt_node_set_ref(nt, w, "value", vr);
          int d = ma_def(nt, wn, 1, 1, w, line); if (d < 0) { ok = 0; break; }
          xc_push(&nb, &nbn, d);
        }
      }
      if (!ok) { free(nb); nb = NULL; nbn = 0; any = 0; break; }
      any = 1;
    }
    if (any && nb) { nt_node_set_arr(nt, body, "body", nb, nbn); changed = 1; }
    free(nb); free(bb);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}



/* ---- `def m ... end unless method_defined?(:m)` in a class body ----
   The guard runs where it stands: the def happens only when nothing has
   defined the method by then -- an earlier def in this or another body of
   the class (or module), or in a superclass, by the program's text. Each
   guard is answered in program order and replaced by its statements or
   dropped, so a def an earlier guard dropped does not count. A private
   method is not defined to method_defined?. Only a bare or
   `self.` method_defined? / public_method_defined? with a literal name is
   answered; anything else keeps its runtime refusal. */
typedef struct { int priv, found, seen; } DumState;
/* a receiverless visibility call: 1 private, 2 public or protected */
static int dum_vis(const NodeTable *nt, int n) {
  if (nt_kind(nt, n) != NK_CallNode || nt_ref(nt, n, "receiver") >= 0 || nt_ref(nt, n, "block") >= 0) return 0;
  const char *cn = nt_str(nt, n, "name");
  if (!cn) return 0;
  if (sp_streq(cn, "private")) return 1;
  return sp_streq(cn, "public") || sp_streq(cn, "protected") ? 2 : 0;
}
static const char *dum_lit_name(const NodeTable *nt, int a) {
  NodeKind k = nt_kind(nt, a);
  return k == NK_SymbolNode ? nt_str(nt, a, "value") : k == NK_StringNode ? nt_str(nt, a, "content") : NULL;
}
static const int *dum_args(const NodeTable *nt, int call, int *n) {
  int ar = nt_ref(nt, call, "arguments");
  *n = 0;
  return ar >= 0 ? nt_arr(nt, ar, "arguments", n) : NULL;
}
/* body subtree `n` in program order, ahead of `before` and outside it: a def
   of `m` (no receiver) defines it unless it is private there, and a later
   `private :m` hides it again; nested classes, defs and singleton classes
   are not entered */
static void dum_walk(const NodeTable *nt, int n, const char *m, int before, DumState *st) {
  if (n < 0 || n == before) return;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_DefNode) {
    const char *dn = nt_str(nt, n, "name");
    if (n < before && nt_ref(nt, n, "receiver") < 0 && dn && sp_streq(dn, m)) { st->seen = 1; st->found = !st->priv; }
    return;
  }
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return;
  int vis = dum_vis(nt, n);
  if (vis) {
    int an = 0; const int *av = dum_args(nt, n, &an);
    if (an == 0) { if (n < before) st->priv = vis == 1; return; }
    for (int q = 0; q < an; q++) {
      if (nt_kind(nt, av[q]) == NK_DefNode) {
        const char *dn = nt_str(nt, av[q], "name");
        if (av[q] < before && nt_ref(nt, av[q], "receiver") < 0 && dn && sp_streq(dn, m)) { st->seen = 1; st->found = vis == 2; }
        continue;
      }
      const char *ln = dum_lit_name(nt, av[q]);
      if (ln && sp_streq(ln, m) && n < before && st->seen) st->found = vis == 2;
    }
    return;
  }
  const SpNode *nd = &nt->nodes[n];
  for (int j = 0; j < nd->nr; j++) dum_walk(nt, nd->r[j].ref, m, before, st);
  for (int j = 0; j < nd->na; j++)
    for (int q = 0; q < nd->a[j].n; q++) dum_walk(nt, nd->a[j].ids[q], m, before, st);
}
static const char *dum_leaf(const NodeTable *nt, int path) {
  if (path < 0) return NULL;
  NodeKind k = nt_kind(nt, path);
  return k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(nt, path, "name") : NULL;
}
static int dum_defined_before(const NodeTable *nt, const char *cls, const char *m, int before, int depth) {
  if (!cls || depth > 16) return 0;
  const char *super = NULL;
  DumState st = {0, 0, 0};
  for (int n = 0; n < before && n < nt->count; n++) {
    NodeKind k = nt_kind(nt, n);
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    const char *ln = dum_leaf(nt, nt_ref(nt, n, "constant_path"));
    if (!ln || !sp_streq(ln, cls)) continue;
    st.priv = 0;   /* each body starts public */
    dum_walk(nt, nt_ref(nt, n, "body"), m, before, &st);
    if (k == NK_ClassNode && !super) super = dum_leaf(nt, nt_ref(nt, n, "superclass"));
  }
  /* the class's own def decides, a private one too (it hides an inherited one) */
  if (st.seen) return st.found;
  return super ? dum_defined_before(nt, super, m, before, depth + 1) : 0;
}
int desugar_def_unless_method_defined(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0, n0 = nt->count;
  for (int cls = 0; cls < n0; cls++) {
    NodeKind ck = nt_kind(nt, cls);
    if (ck != NK_ClassNode && ck != NK_ModuleNode) continue;
    const char *cn = dum_leaf(nt, nt_ref(nt, cls, "constant_path"));
    int body = nt_ref(nt, cls, "body");
    if (!cn || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
    int *out = malloc(sizeof(int) * (size_t)(bn + 1)); int no = 0, touched = 0;
    for (int i = 0; i < bn; i++) {
      int st = bs[i];
      int pred = nt_kind(nt, st) == NK_UnlessNode ? nt_ref(nt, st, "predicate") : -1;
      const char *pn = pred >= 0 && nt_kind(nt, pred) == NK_CallNode ? nt_str(nt, pred, "name") : NULL;
      int pr = pred >= 0 ? nt_ref(nt, pred, "receiver") : -1;
      int an = 0; const int *av = pn ? nt_arr(nt, nt_ref(nt, pred, "arguments"), "arguments", &an) : NULL;
      const char *m = an == 1 && nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "value")
                    : an == 1 && nt_kind(nt, av[0]) == NK_StringNode ? nt_str(nt, av[0], "content") : NULL;
      int arm = pn ? nt_ref(nt, st, "statements") : -1;
      if (!pn || (!sp_streq(pn, "method_defined?") && !sp_streq(pn, "public_method_defined?")) ||
          (pr >= 0 && nt_kind(nt, pr) != NK_SelfNode) || !m || nt_ref(nt, pred, "block") >= 0 ||
          nt_ref(nt, st, "else_clause") >= 0 || arm < 0 || nt_kind(nt, arm) != NK_StatementsNode) {
        out[no++] = st; continue;
      }
      touched = 1; changed = 1;
      if (dum_defined_before(nt, cn, m, st, 0)) continue;   /* defined already: the guarded body does not run */
      int sn = 0; const int *ss = nt_arr(nt, arm, "body", &sn);
      out = realloc(out, sizeof(int) * (size_t)(no + sn + (bn - i) + 1));
      for (int q = 0; q < sn; q++) out[no++] = ss[q];
    }
    if (touched) nt_node_set_arr(nt, body, "body", out, no);
    free(out);
  }
  return changed;
}

/* ---- a string class_eval whose text is known at compile time ----
   `class_eval <<-RUBY ... RUBY` in a class or module body, either plain or
   stamped out per element of a literal array:
     METHODS = %w[a b]
     METHODS.each do |m|
       class_eval <<-RUBY, __FILE__, __LINE__ + 1
         def #{m}(...) = dispatch(:#{m}, ...)
       RUBY
     end
   is the code it spells, so the text is built per element (an
   interpolation must be the loop variable, or its to_s / upcase /
   downcase / capitalize), parsed as a snippet and grafted into the body in
   place of the statement. The __FILE__ / __LINE__ arguments are ignored;
   the grafted nodes carry the site's line and file. Anything else stays a
   runtime eval, which is refused as before. */
char *sp_parse_snippet_to_text(const char *src);
static int sce_is_eval_name(const char *nm) {
  return nm && (sp_streq(nm, "class_eval") || sp_streq(nm, "module_eval"));
}
/* the class_eval call's string argument node, or -1 */
static int sce_eval_string(const NodeTable *nt, int st) {
  if (nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "block") >= 0) return -1;
  int rcv = nt_ref(nt, st, "receiver");
  if (rcv >= 0 && nt_kind(nt, rcv) != NK_SelfNode) return -1;
  if (!sce_is_eval_name(nt_str(nt, st, "name"))) return -1;
  int an = 0; int args = nt_ref(nt, st, "arguments");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 1 || !av) return -1;
  NodeKind k = nt_kind(nt, av[0]);
  if (k != NK_StringNode && k != NK_InterpolatedStringNode) return -1;
  /* the file and line arguments are not evaluated: only ones with nothing
     to run (`__FILE__`, a string, `__LINE__`, `__LINE__ + 1`, an integer) */
  if (an > 3) return -1;
  if (an >= 2 && nt_kind(nt, av[1]) != NK_SourceFileNode && nt_kind(nt, av[1]) != NK_StringNode) return -1;
  if (an >= 3) {
    int l = av[2];
    if (nt_kind(nt, l) == NK_CallNode) {
      const char *op = nt_str(nt, l, "name");
      int la = nt_ref(nt, l, "arguments"), ln = 0;
      const int *lv = la >= 0 ? nt_arr(nt, la, "arguments", &ln) : NULL;
      int lr = nt_ref(nt, l, "receiver");
      if (!op || !(sp_streq(op, "+") || sp_streq(op, "-")) || ln != 1 || !lv || lr < 0 ||
          nt_kind(nt, lr) != NK_SourceLineNode || nt_kind(nt, lv[0]) != NK_IntegerNode)
        return -1;
    }
    else if (nt_kind(nt, l) != NK_SourceLineNode && nt_kind(nt, l) != NK_IntegerNode) return -1;
  }
  return av[0];
}
static void sce_append(char **buf, size_t *len, size_t *cap, const char *t) {
  size_t tl = strlen(t);
  if (*len + tl + 1 > *cap) { *cap = (*len + tl + 1) * 2 + 64; *buf = (char *)realloc(*buf, *cap); }
  memcpy(*buf + *len, t, tl); *len += tl; (*buf)[*len] = 0;
}
/* the string's text with `var` (a block parameter) read as `elem`; NULL
   when an interpolation is anything else */
static char *sce_text(const NodeTable *nt, int str, const char *var, const char *elem) {
  char *buf = NULL; size_t len = 0, cap = 0;
  sce_append(&buf, &len, &cap, "");
  if (nt_kind(nt, str) == NK_StringNode) {
    const char *t = nt_str(nt, str, "content");
    if (!t) { free(buf); return NULL; }
    sce_append(&buf, &len, &cap, t);
    return buf;
  }
  int pn = 0; const int *parts = nt_arr(nt, str, "parts", &pn);
  for (int i = 0; i < pn; i++) {
    NodeKind k = nt_kind(nt, parts[i]);
    if (k == NK_StringNode) {
      const char *t = nt_str(nt, parts[i], "content");
      if (!t) { free(buf); return NULL; }
      sce_append(&buf, &len, &cap, t);
      continue;
    }
    if (k != NK_EmbeddedStatementsNode || !var) { free(buf); return NULL; }
    int stmts = nt_ref(nt, parts[i], "statements");
    int sn = 0; const int *ss = stmts >= 0 ? nt_arr(nt, stmts, "body", &sn) : NULL;
    if (sn != 1) { free(buf); return NULL; }
    int e = ss[0];
    const char *conv = NULL;
    if (nt_kind(nt, e) == NK_CallNode && nt_ref(nt, e, "arguments") < 0 && nt_ref(nt, e, "block") < 0) {
      conv = nt_str(nt, e, "name"); e = nt_ref(nt, e, "receiver");
      if (!conv || (!sp_streq(conv, "to_s") && !sp_streq(conv, "upcase") && !sp_streq(conv, "downcase") &&
                    !sp_streq(conv, "capitalize"))) { free(buf); return NULL; }
    }
    if (e < 0 || nt_kind(nt, e) != NK_LocalVariableReadNode) { free(buf); return NULL; }
    const char *nm = nt_str(nt, e, "name");
    if (!nm || !sp_streq(nm, var)) { free(buf); return NULL; }
    char *v = strdup(elem);
    if (conv && !sp_streq(conv, "to_s")) {
      /* by character in CRuby: only ASCII maps the same byte by byte */
      for (size_t q = 0; v[q]; q++) if ((unsigned char)v[q] >= 0x80) { free(v); free(buf); return NULL; }
      for (size_t q = 0; v[q]; q++) {
        if (sp_streq(conv, "upcase")) v[q] = (char)toupper((unsigned char)v[q]);
        else if (sp_streq(conv, "downcase")) v[q] = (char)tolower((unsigned char)v[q]);
        else if (sp_streq(conv, "capitalize")) v[q] = q == 0 ? (char)toupper((unsigned char)v[q]) : (char)tolower((unsigned char)v[q]);
      }
    }
    sce_append(&buf, &len, &cap, v);
    free(v);
  }
  return buf;
}
/* The line the eval's text starts on: the `__LINE__` / `__LINE__ + k` it is
   given, else the call's own line; -1 when the program has no lines. */
static long long sce_base_line(const NodeTable *nt, int site) {
  long long line = nt_int(nt, site, "node_line", -1);
  if (line < 0) return -1;
  int an = 0; int args = nt_ref(nt, site, "arguments");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 3 || !av) return line;
  int a = av[2];
  long long k = 0;
  if (nt_kind(nt, a) == NK_CallNode && nt_str(nt, a, "name") &&
      (sp_streq(nt_str(nt, a, "name"), "+") || sp_streq(nt_str(nt, a, "name"), "-"))) {
    int aa = nt_ref(nt, a, "arguments"); int kn = 0;
    const int *kv = aa >= 0 ? nt_arr(nt, aa, "arguments", &kn) : NULL;
    if (kn != 1 || !kv || nt_kind(nt, kv[0]) != NK_IntegerNode) return line;
    k = nt_int(nt, kv[0], "value", 0);
    if (sp_streq(nt_str(nt, a, "name"), "-")) k = -k;
    a = nt_ref(nt, a, "receiver");
  }
  if (a < 0 || nt_kind(nt, a) != NK_SourceLineNode) return line;
  return nt_int(nt, a, "start_line", line) + k;
}
/* parse `text` and graft its statements into nt: each node on its line in
   the text counted from the eval's base line, in the site's file. The count,
   or -1 when it does not parse (or reads differently spliced, see
   sp_snippet_graftable) */
static int sce_graft(Compiler *c, const char *text, int site, int **out, int *n) {
  NodeTable *nt = (NodeTable *)c->nt;
  char *ast = sp_parse_snippet_to_text(text);
  if (!ast) return -1;
  NodeTable *tmp = nt_load_text(ast);
  free(ast);
  if (!tmp) return -1;
  int prog = tmp->root_id;
  int stmts = prog >= 0 ? nt_ref(tmp, prog, "statements") : -1;
  int sn = 0; const int *ss = stmts >= 0 ? nt_arr(tmp, stmts, "body", &sn) : NULL;
  long long line = sce_base_line(nt, site), file = nt_int(nt, site, "node_file", -1);
  int base = nt->count;
  for (int i = 0; i < sn; i++) {
    int id = nt_import_subtree(nt, tmp, ss[i]);
    if (id < 0) { nt_free(tmp); return -1; }
    xc_push(out, n, id);
  }
  if (line >= 0)
    for (int id = base; id < nt->count; id++) {
      long long rel = nt_int(nt, id, "node_line", 1);
      nt_node_set_int(nt, id, "node_line", line + (rel > 0 ? rel - 1 : 0));
      if (file >= 0) nt_node_set_int(nt, id, "node_file", file);
    }
  nt_free(tmp);
  return sn;
}
/* Is every read of constant `name` in the program the receiver of a call
   that leaves the Array as it is? Then its literal is what an each sees. */
static int sce_const_unmutated(const NodeTable *nt, const char *name) {
  static const char *const RO[] = { "each", "each_with_index", "each_with_object", "map", "flat_map",
    "filter_map", "select", "reject", "find", "include?", "first", "last", "size", "length", "count",
    "empty?", "any?", "all?", "none?", "join", "to_a", "freeze", "frozen?", "dup", "[]", "index",
    "sort", "reverse", "uniq", "zip", "min", "max", "inspect", "to_s", "==", "+", "-", "&", "|", NULL };
  int reads = 0, ro = 0;
  NtKindIter it = nt_kind_iter_begin(nt, NK_ConstantReadNode);
  while (nt_kind_iter_next(&it)) {
    const char *cn = nt_str(nt, it.id, "name");
    if (cn && sp_streq(cn, name)) reads++;
  }
  nt_kind_iter_close(&it);
  it = nt_kind_iter_begin(nt, NK_ConstantPathNode);
  while (nt_kind_iter_next(&it)) {
    const char *cn = nt_str(nt, it.id, "name");
    if (cn && sp_streq(cn, name)) { nt_kind_iter_close(&it); return 0; }
  }
  nt_kind_iter_close(&it);
  it = nt_kind_iter_begin(nt, NK_CallNode);
  while (nt_kind_iter_next(&it)) {
    int r = nt_ref(nt, it.id, "receiver");
    if (r < 0 || nt_kind(nt, r) != NK_ConstantReadNode) continue;
    const char *cn = nt_str(nt, r, "name"), *m = nt_str(nt, it.id, "name");
    if (!cn || !m || !sp_streq(cn, name)) continue;
    for (int q = 0; RO[q]; q++) if (sp_streq(m, RO[q])) { ro++; break; }
  }
  nt_kind_iter_close(&it);
  return reads == ro;
}
/* the element texts (strings or symbols) of the literal array constant
   `name` is when statement `before` of `body` runs: written exactly once in
   this body, ahead of it, and never changed after (sce_const_unmutated).
   One written anywhere else could be shadowed by a superclass's or an
   enclosing scope's on the way, so only the body's own is read. -1 if not. */
static int sce_const_elems(const NodeTable *nt, int body, int before, const char *name, const char ***out) {
  int w = -1;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  for (int i = 0; i < bn; i++)
    if (nt_kind(nt, bb[i]) == NK_ConstantWriteNode && nt_str(nt, bb[i], "name") && sp_streq(nt_str(nt, bb[i], "name"), name)) {
      if (w >= 0 || i >= before) return -1;
      w = bb[i];
    }
  if (w < 0 || !sce_const_unmutated(nt, name)) return -1;
  int v = nt_ref(nt, w, "value");
  if (v < 0 || nt_kind(nt, v) != NK_ArrayNode) return -1;
  int en = 0; const int *els = nt_arr(nt, v, "elements", &en);
  const char **res = (const char **)malloc(sizeof(char *) * (size_t)(en > 0 ? en : 1));
  if (!res) return -1;
  for (int i = 0; i < en; i++) {
    NodeKind k = nt_kind(nt, els[i]);
    const char *t = k == NK_StringNode ? nt_str(nt, els[i], "content") : k == NK_SymbolNode ? nt_str(nt, els[i], "value") : NULL;
    if (!t) { free(res); return -1; }
    res[i] = t;
  }
  *out = res;
  return en;
}
/* A program that gives class_eval / module_eval a method of its own, or
   hooks what a graft does (method_added, singleton_method_added,
   const_added), is grafted nowhere: the splice would bypass the override or
   the hook (the rule sp_macro.c's expansion follows). */
static int sce_name_reflective(const char *nm) {
  static const char *const NAMES[] = { "class_eval", "module_eval", "method_added",
    "singleton_method_added", "const_added", NULL };
  if (!nm) return 0;
  if (*nm == ':') nm++;
  return str_in(nm, NAMES);
}
static int sce_program_reflects(const NodeTable *nt) {
  int hit = 0;
  NtKindIter it = nt_kind_iter_begin(nt, NK_DefNode);
  while (!hit && nt_kind_iter_next(&it)) hit = sce_name_reflective(nt_str(nt, it.id, "name"));
  nt_kind_iter_close(&it);
  it = nt_kind_iter_begin(nt, NK_AliasMethodNode);
  while (!hit && nt_kind_iter_next(&it)) {
    int nn = nt_ref(nt, it.id, "new_name");
    hit = nn >= 0 && sce_name_reflective(nt_str(nt, nn, "value"));
  }
  nt_kind_iter_close(&it);
  it = nt_kind_iter_begin(nt, NK_UndefNode);
  while (!hit && nt_kind_iter_next(&it)) {
    int un = 0; const int *uv = nt_arr(nt, it.id, "names", &un);
    for (int q = 0; q < un && !hit; q++) hit = sce_name_reflective(nt_str(nt, uv[q], "value"));
  }
  nt_kind_iter_close(&it);
  it = nt_kind_iter_begin(nt, NK_CallNode);
  while (!hit && nt_kind_iter_next(&it)) {
    const char *m = nt_str(nt, it.id, "name");
    int dm = m && sp_streq(m, "define_method");
    if (!m || !(dm || sp_streq(m, "alias_method") || sp_streq(m, "define_singleton_method") ||
                sp_streq(m, "remove_method") || sp_streq(m, "undef_method")))
      continue;
    int args = nt_ref(nt, it.id, "arguments"); int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 1 || !av) continue;
    NodeKind k = nt_kind(nt, av[0]);
    if (k == NK_SymbolNode) hit = sce_name_reflective(nt_str(nt, av[0], "value"));
    else if (k == NK_StringNode) hit = sce_name_reflective(nt_str(nt, av[0], "content"));
    else hit = !dm;   /* a computed name could be any of them (as sp_macro.c reads it) */
  }
  nt_kind_iter_close(&it);
  return hit;
}
/* a bare `private` / `protected` / `public` / `module_function`: a def
   spliced after it takes that visibility, a class_eval'd one does not */
static int sce_bare_visibility(const NodeTable *nt, int st) {
  if (nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "receiver") >= 0 || nt_ref(nt, st, "arguments") >= 0) return 0;
  const char *m = nt_str(nt, st, "name");
  return m && (sp_streq(m, "private") || sp_streq(m, "protected") || sp_streq(m, "public") ||
               sp_streq(m, "module_function"));
}
int desugar_static_class_eval(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  int gate = -1;   /* sce_program_reflects, asked once a candidate is seen */
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k != NK_ModuleNode && k != NK_ClassNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb0 = nt_arr(nt, body, "body", &bn);
    int *bb = (int *)malloc(sizeof(int) * (size_t)(bn > 0 ? bn : 1));
    if (!bb) continue;
    memcpy(bb, bb0, sizeof(int) * (size_t)bn);
    int *nb = NULL, nbn = 0, any = 0, vis = 0;
    for (int i = 0; i < bn; i++) {
      int st = bb[i];
      int *ins = NULL, nins = 0, ok = 0;
      vis |= sce_bare_visibility(nt, st);
      int str = vis ? -1 : sce_eval_string(nt, st);
      int each = !vis && str < 0 && nt_kind(nt, st) == NK_CallNode && nt_str(nt, st, "name") &&
                 sp_streq(nt_str(nt, st, "name"), "each") && nt_ref(nt, st, "arguments") < 0;
      if ((str >= 0 || each) && gate < 0) gate = sce_program_reflects(nt);
      if (gate > 0) { str = -1; each = 0; }
      if (str >= 0) {
        char *text = sce_text(nt, str, NULL, NULL);
        if (text) { ok = sce_graft(c, text, st, &ins, &nins) >= 0; free(text); }
      }
      else if (each) {
        /* CONST.each do |v| class_eval "..." end */
        int recv = nt_ref(nt, st, "receiver"), blk = nt_ref(nt, st, "block");
        const char *cname = recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL;
        int bbody = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode ? nt_ref(nt, blk, "body") : -1;
        int pn = blk >= 0 ? nt_ref(nt, blk, "parameters") : -1;
        int inner = pn >= 0 ? nt_ref(nt, pn, "parameters") : -1;
        int rn = 0; const int *reqs = inner >= 0 ? nt_arr(nt, inner, "requireds", &rn) : NULL;
        const char *var = rn == 1 && reqs ? nt_str(nt, reqs[0], "name") : NULL;
        int sn = 0; const int *ss = bbody >= 0 && nt_kind(nt, bbody) == NK_StatementsNode ? nt_arr(nt, bbody, "body", &sn) : NULL;
        int estr = sn == 1 ? sce_eval_string(nt, ss[0]) : -1;
        const char **elems = NULL; int ne = cname ? sce_const_elems(nt, body, i, cname, &elems) : -1;
        if (var && estr >= 0 && ne >= 0) {
          ok = 1;
          for (int e = 0; e < ne && ok; e++) {
            char *text = sce_text(nt, estr, var, elems[e]);
            if (!text || sce_graft(c, text, ss[0], &ins, &nins) < 0) ok = 0;
            free(text);
          }
        }
        free(elems);
      }
      if (ok) { for (int j = 0; j < nins; j++) xc_push(&nb, &nbn, ins[j]); any = 1; }
      else xc_push(&nb, &nbn, st);
      free(ins);
    }
    if (any && nb) { nt_node_set_arr(nt, body, "body", nb, nbn); changed = 1; }
    free(nb); free(bb);
  }
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- a class body's calls of its own class methods ----
   In `class Sub < Base; self.v = 1; p v; end` self is Sub, and an inherited
   class method it calls runs for Sub: a class-level @ivar it touches is
   Sub's own. The body's code is emitted outside any method, where a
   `self.` receiver -- or none -- reached the defining class's generic copy,
   so Sub's `self.v = 1` wrote Base's @v. Such a call takes the class's
   constant as its receiver, the form `Sub.v = 1` from outside already
   compiles right: an explicit `self.` one or a bare one, whose name a
   class method of the class or a superclass defines. Nested defs, classes
   and singleton classes are not entered, nor blocks whose self is another
   object (class_eval, instance_eval, define_method, ...). */
/* A class's name as the program spells it, qualified by the modules and
   classes it is nested in lexically (`module M; class A::B` is "M::A::B",
   `class ::C` is "C"): two classes of one leaf name in different namespaces
   are different classes. A constant path it can't spell is NULL. */
typedef struct { char **qual; char **outer; int *cls; int ncls, n; } CbsNames;
static int cbs_path_text(const NodeTable *nt, int p, char *buf, size_t cap, int *rooted) {
  if (p < 0) return 0;
  NodeKind k = nt_kind(nt, p);
  const char *nm = nt_str(nt, p, "name");
  if (!nm || (k != NK_ConstantReadNode && k != NK_ConstantPathNode)) return 0;
  if (k == NK_ConstantPathNode) {
    int par = nt_ref(nt, p, "parent");
    if (par < 0) { *rooted = 1; buf[0] = '\0'; }
    else if (!cbs_path_text(nt, par, buf, cap, rooted)) return 0;
    if (par >= 0 && strlen(buf) + 2 < cap) strcat(buf, "::");
  }
  else buf[0] = '\0';
  if (strlen(buf) + strlen(nm) + 1 > cap) return 0;
  strcat(buf, nm);
  return 1;
}
static char *cbs_qualify(const NodeTable *nt, int path, const char *outer) {
  char t[512]; int rooted = 0;
  if (!cbs_path_text(nt, path, t, sizeof t, &rooted)) return NULL;
  if (rooted || !outer || !*outer) return strdup(t);
  size_t l = strlen(outer) + strlen(t) + 3;
  char *q = malloc(l);
  snprintf(q, l, "%s::%s", outer, t);
  return q;
}
static void cbs_name_walk(const NodeTable *nt, CbsNames *q, int n, const char *outer, int depth) {
  if (n < 0 || n >= q->n || depth > 4096) return;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_ClassNode || k == NK_ModuleNode) {
    char *qn = cbs_qualify(nt, nt_ref(nt, n, "constant_path"), outer);
    if (!qn) return;   /* a namespace it can't spell: its classes are left alone */
    if (k == NK_ClassNode) {
      q->qual[n] = qn; q->outer[n] = outer ? strdup(outer) : NULL;
      q->cls[q->ncls++] = n;
    }
    cbs_name_walk(nt, q, nt_ref(nt, n, "body"), qn, depth + 1);
    if (k != NK_ClassNode) free(qn);
    return;
  }
  const SpNode *nd = &nt->nodes[n];
  for (int j = 0; j < nd->nr; j++) cbs_name_walk(nt, q, nd->r[j].ref, outer, depth + 1);
  for (int j = 0; j < nd->na; j++)
    for (int i = 0; i < nd->a[j].n; i++) cbs_name_walk(nt, q, nd->a[j].ids[i], outer, depth + 1);
}
/* the qualified name of the program class the superclass expression `sc`,
   written inside `outer`, names: looked up from the innermost namespace out,
   as a constant is; NULL for a class the program does not define */
static const char *cbs_resolve_super(const NodeTable *nt, const CbsNames *q, int sc, const char *outer) {
  char t[512], cand[1024]; int rooted = 0;
  if (!cbs_path_text(nt, sc, t, sizeof t, &rooted)) return NULL;
  char pre[512]; snprintf(pre, sizeof pre, "%s", rooted || !outer ? "" : outer);
  for (;;) {
    if (*pre) snprintf(cand, sizeof cand, "%s::%s", pre, t);
    else snprintf(cand, sizeof cand, "%s", t);
    for (int i = 0; i < q->ncls; i++)
      if (sp_streq(q->qual[q->cls[i]], cand)) return q->qual[q->cls[i]];
    if (!*pre) return NULL;
    char *cut = strrchr(pre, ':');
    if (cut && cut > pre) cut[-1] = '\0';
    else pre[0] = '\0';
  }
}
/* does a body of class `cls` (qualified), or of a superclass, define a
   class method `m` (`def self.m` or in `class << self`)? */
static int cbs_cmethod_defined(const NodeTable *nt, const CbsNames *q, const char *cls, const char *m, int depth) {
  if (!cls || depth > 16) return 0;
  const char *super = NULL;
  for (int i = 0; i < q->ncls; i++) {
    int n = q->cls[i];
    if (!sp_streq(q->qual[n], cls)) continue;
    if (!super && nt_ref(nt, n, "superclass") >= 0)
      super = cbs_resolve_super(nt, q, nt_ref(nt, n, "superclass"), q->outer[n]);
    int b = nt_ref(nt, n, "body");
    int bn = 0; const int *bs = b >= 0 && nt_kind(nt, b) == NK_StatementsNode ? nt_arr(nt, b, "body", &bn) : NULL;
    for (int k = 0; k < bn; k++) {
      int st = bs[k];
      if (nt_kind(nt, st) == NK_DefNode && nt_ref(nt, st, "receiver") >= 0 &&
          nt_kind(nt, nt_ref(nt, st, "receiver")) == NK_SelfNode && nt_str(nt, st, "name") &&
          sp_streq(nt_str(nt, st, "name"), m)) return 1;
      if (nt_kind(nt, st) == NK_SingletonClassNode) {
        int sb = nt_ref(nt, st, "body");
        int sn = 0; const int *ss = sb >= 0 && nt_kind(nt, sb) == NK_StatementsNode ? nt_arr(nt, sb, "body", &sn) : NULL;
        for (int r = 0; r < sn; r++)
          if (nt_kind(nt, ss[r]) == NK_DefNode && nt_str(nt, ss[r], "name") && sp_streq(nt_str(nt, ss[r], "name"), m)) return 1;
      }
    }
  }
  return super ? cbs_cmethod_defined(nt, q, super, m, depth + 1) : 0;
}
static int cbs_self_changing_block_call(const NodeTable *nt, int call) {
  static const char *const names[] = { "class_eval", "module_eval", "class_exec", "module_exec",
    "instance_eval", "instance_exec", "define_method", "define_singleton_method", "new", NULL };
  const char *nm = nt_str(nt, call, "name");
  for (int i = 0; nm && names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
static int cbs_walk(Compiler *c, const CbsNames *q, int n, int cls_node, const char *cls) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return 0;
  int changed = 0;
  if (k == NK_CallNode) {
    int recv = nt_ref(nt, n, "receiver");
    const char *nm = nt_str(nt, n, "name");
    /* only a class method the program defines: a builtin one (singleton_class,
       instance_method, attr_accessor, ...) keeps the self or bare form its
       own desugars look for */
    int retarget = nm && (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) && cbs_cmethod_defined(nt, q, cls, nm, 0);
    if (retarget) {
      int cp = nt_clone_subtree(nt, nt_ref(nt, cls_node, "constant_path"));
      if (cp >= 0) {
        comp_grow_node_arrays(c); c->nscope[cp] = c->nscope[n]; nt_node_set_ref(nt, n, "receiver", cp);
        /* still a call on self: a private class method answers it */
        nt_node_set_int(nt, n, "self_call", 1);
        changed = 1;
      }
    }
    if (nt_ref(nt, n, "block") >= 0 && cbs_self_changing_block_call(nt, n)) {
      /* the block's self is another object: only the receiver and arguments are ours */
      changed |= cbs_walk(c, q, nt_ref(nt, n, "receiver"), cls_node, cls);
      changed |= cbs_walk(c, q, nt_ref(nt, n, "arguments"), cls_node, cls);
      return changed;
    }
  }
  const SpNode *nd = &nt->nodes[n];
  int nr = nd->nr;
  int refs[64]; if (nr > 64) nr = 64;
  for (int j = 0; j < nr; j++) refs[j] = nd->r[j].ref;
  for (int j = 0; j < nr; j++) changed |= cbs_walk(c, q, refs[j], cls_node, cls);
  for (int j = 0; j < nt->nodes[n].na; j++) {
    int an = nt->nodes[n].a[j].n;
    if (an <= 0) continue;  /* an empty array's ids may be NULL: memcpy from NULL is UB */
    int *ids = malloc(sizeof(int) * (size_t)(an + 1));
    memcpy(ids, nt->nodes[n].a[j].ids, sizeof(int) * (size_t)an);
    for (int r = 0; r < an; r++) changed |= cbs_walk(c, q, ids[r], cls_node, cls);
    free(ids);
  }
  return changed;
}
int desugar_class_body_self_calls(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0, n0 = nt->count;
  CbsNames q;
  q.n = n0; q.ncls = 0;
  q.qual = calloc((size_t)n0 + 1, sizeof(char *));
  q.outer = calloc((size_t)n0 + 1, sizeof(char *));
  q.cls = calloc((size_t)n0 + 1, sizeof(int));
  cbs_name_walk(nt, &q, nt->root_id, NULL, 0);
  int ncls = q.ncls;
  for (int i = 0; i < ncls; i++) {
    int cl = q.cls[i];
    changed |= cbs_walk(c, &q, nt_ref(nt, cl, "body"), cl, q.qual[cl]);
  }
  for (int i = 0; i < n0; i++) { free(q.qual[i]); free(q.outer[i]); }
  free(q.qual); free(q.outer); free(q.cls);
  return changed;
}

/* `class Rational < Numeric` -- a builtin reopened with its own superclass
   named, as the bigdecimal gem's util.rb writes it -- is the reopening
   `class Rational` is: Ruby accepts the superclass because it is the one the
   class already has. Left in, it made a user class of the builtin's name,
   whose struct the runtime's own type already took. The superclass is
   dropped when it is the builtin's own (not for the exception classes,
   whose chain the runtime answers). */
int desugar_builtin_reopen_named_superclass(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int n = 0; n < nt->count; n++) {
    if (nt_kind(nt, n) != NK_ClassNode) continue;
    int cp = nt_ref(nt, n, "constant_path"), sc = nt_ref(nt, n, "superclass");
    if (cp < 0 || sc < 0 || nt_kind(nt, cp) != NK_ConstantReadNode || nt_kind(nt, sc) != NK_ConstantReadNode) continue;
    const char *cn = nt_str(nt, cp, "name"), *sn = nt_str(nt, sc, "name");
    if (!cn || !sn || is_builtin_exception_name(cn)) continue;
    int cid = builtin_class_id(cn), sid = builtin_class_id(sn);
    if (cid == 0 || sid == 0 || builtin_class_parent_id(cid) != sid) continue;
    nt_node_set_ref(nt, n, "superclass", -1);
    changed = 1;
  }
  return changed;
}
