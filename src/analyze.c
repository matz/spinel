#include "analyze_internal.h"

/* --int-overflow=promote flag; see analyze.h. Default off. */
int g_promote_mode = 0;

/* Defined in codegen.c (linked into the same binary). Used to specialize a
   `rescue <UserExc> => e` binding to the exception subclass's object type. */
int class_is_exc_subclass(Compiler *c, int ci);

/* The class index a rescue arm specializes its bound variable to: exactly one
   named user exception subclass that carries ivars, else -1 (#1415). */
static int rescue_arm_spec_cid(Compiler *c, int rescue_id) {
  int nexc = 0;
  const int *exc = nt_arr(c->nt, rescue_id, "exceptions", &nexc);
  if (nexc != 1) return -1;
  const char *en = nt_type(c->nt, exc[0]);
  if (!en || (!sp_streq(en, "ConstantReadNode") && !sp_streq(en, "ConstantPathNode"))) return -1;
  const char *enm = nt_str(c->nt, exc[0], "name");
  int xc = enm ? comp_class_index(c, enm) : -1;
  if (xc >= 0 && class_is_exc_subclass(c, xc) && c->classes[xc].nivars > 0) return xc;
  return -1;
}

void compute_reachable(Compiler *c) {
  /* Build per-scope call sets (CallNode names, not entering nested DefNodes). */
  char ***scope_calls = calloc((size_t)c->nscopes, sizeof(char **));
  int   *sc_n        = calloc((size_t)c->nscopes, sizeof(int));
  int   *sc_cap      = calloc((size_t)c->nscopes, sizeof(int));
  for (int s = 0; s < c->nscopes; s++) {
    if (c->scopes[s].body >= 0)
      cr_collect_calls(c->nt, c->scopes[s].body, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    /* Also scan parameter defaults (e.g. def foo(opt = bar)) — these emit calls
       within the method scope but live in the DefNode parameters subtree. */
    if (c->scopes[s].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[s].def_node, "parameters");
      if (pn >= 0)
        cr_collect_calls(c->nt, pn, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    }
  }

  /* Names that may be invoked implicitly (no explicit CallNode): keep live. */
  static const char *const implicit[] = {
    "to_s", "inspect", "==", "<=>", "eql?", "hash", "each", "coerce",
    "to_str", "to_ary", "to_a", "to_i", "to_int", "to_h", "to_proc", "call",
    "initialize_copy", NULL };

  /* BFS queue (scope indices). */
  int *queue = malloc((size_t)c->nscopes * sizeof(int));
  int qhead = 0, qtail = 0;

  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    sc->reachable = 0;
    int is_root = (s == 0 || !sc->name || sp_streq(sc->name, "initialize"));
    if (!is_root)
      for (int i = 0; implicit[i]; i++) if (sp_streq(implicit[i], sc->name)) { is_root = 1; break; }
    if (is_root) { sc->reachable = 1; queue[qtail++] = s; }
  }

  /* "called_names" tracks every method name reached from any reachable scope.
     Used by alias and prep_to propagation (aliases have no scope of their own). */
  char **called_names = NULL; int cn_n = 0, cn_cap = 0;
  #define CN_ADD(NM) do { const char *_n=(NM); if(_n){ int _f=0; \
    for(int _i=0;_i<cn_n;_i++) if(sp_streq(called_names[_i],_n)){_f=1;break;} \
    if(!_f){if(cn_n>=cn_cap){cn_cap=cn_cap?cn_cap*2:32;called_names=realloc(called_names,sizeof(char*)*cn_cap);} \
    called_names[cn_n++]=strdup(_n);}} } while(0)

  /* Helper: mark a name reachable — all scopes with that name join the BFS. */
  #define MARK_NAME(NM) do { const char *_mn=(NM); if(_mn){ CN_ADD(_mn); \
    for(int _t=0;_t<c->nscopes;_t++) \
      if(!c->scopes[_t].reachable&&c->scopes[_t].name&&sp_streq(c->scopes[_t].name,_mn)) \
        { c->scopes[_t].reachable=1; queue[qtail++]=_t; } } } while(0)

  while (qhead < qtail) {
    int s = queue[qhead++];
    for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
  }

  /* The synthesized compiler_state dump method calls ir_emit_int/str/sa/ia,
     but it has no AST so the BFS above can't see those calls. Mark them
     reachable when any class declares compiler_state fields. */
  {
    int any_cs = 0;
    for (int ci = 0; ci < c->nclasses; ci++) if (c->classes[ci].ncs > 0) { any_cs = 1; break; }
    if (any_cs) {
      MARK_NAME("ir_emit_int"); MARK_NAME("ir_emit_str");
      MARK_NAME("ir_emit_sa");  MARK_NAME("ir_emit_ia");
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
    }
  }

  /* Alias/prep_to propagation: when alias_new (or alias_old) is in called_names,
     make the counterpart reachable too (aliases have no scope of their own). */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *cls = &c->classes[ci];
      for (int i = 0; i < cls->naliases; i++) {
        const char *an = cls->alias_new[i], *ao = cls->alias_old[i];
        int an_live = 0, ao_live = 0;
        for (int j = 0; j < cn_n; j++) {
          if (an && sp_streq(called_names[j], an)) an_live = 1;
          if (ao && sp_streq(called_names[j], ao)) ao_live = 1;
        }
        /* also check reachable scope names (covers scope-backed aliases) */
        for (int s = 0; s < c->nscopes; s++) {
          if (c->scopes[s].reachable && c->scopes[s].name) {
            if (an && sp_streq(c->scopes[s].name, an)) an_live = 1;
            if (ao && sp_streq(c->scopes[s].name, ao)) ao_live = 1;
          }
        }
        if (an_live && !ao_live) {
          int prev_qtail = qtail;
          MARK_NAME(ao);
          if (qtail > prev_qtail) changed = 1;
          /* drain newly enqueued scopes */
          while (qhead < qtail) {
            int s = queue[qhead++];
            for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
          }
        }
        if (ao_live && !an_live) {
          int prev_qtail = qtail;
          MARK_NAME(an);
          if (qtail > prev_qtail) changed = 1;
          while (qhead < qtail) {
            int s = queue[qhead++];
            for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
          }
        }
      }
      for (int i = 0; i < cls->nprep_chain; i++) {
        const char *pf = cls->prep_from[i]; /* user-facing name, e.g. "hi" */
        const char *pt = cls->prep_to[i];   /* shadow name, e.g. "__prep_0_hi" */
        if (!pf || !pt) continue;
        /* When the user-facing name is called, the codegen wrapper calls the shadow
           implementation directly — so mark the shadow reachable too. */
        int pf_in_called = 0;
        for (int j = 0; j < cn_n; j++) if (sp_streq(called_names[j], pf)) { pf_in_called = 1; break; }
        if (!pf_in_called) {
          for (int s = 0; s < c->nscopes; s++)
            if (c->scopes[s].reachable && c->scopes[s].name && sp_streq(c->scopes[s].name, pf)) { pf_in_called = 1; break; }
        }
        if (pf_in_called) {
          int prev_qtail = qtail;
          MARK_NAME(pt);
          if (qtail > prev_qtail) { changed = 1;
            while (qhead < qtail) { int s=queue[qhead++]; for(int ni=0;ni<sc_n[s];ni++) MARK_NAME(scope_calls[s][ni]); }
          }
        }
      }
    }
  }

  for (int i = 0; i < cn_n; i++) free(called_names[i]);
  free(called_names);
  #undef CN_ADD
  #undef MARK_NAME

  /* Cleanup. */
  for (int s = 0; s < c->nscopes; s++) {
    for (int i = 0; i < sc_n[s]; i++) free(scope_calls[s][i]);
    free(scope_calls[s]);
  }
  free(scope_calls); free(sc_n); free(sc_cap); free(queue);
}

/* Mark each user class whose exact cls_id can appear at runtime. A poly value
   carries class C's cls_id only if a C instance was minted: `C.new`,
   `C.allocate`, `raise C` (exception construct), or a `Struct.new`-defined C.
   `dup`/`clone` copy an existing C value (propagate, never originate) and a
   subclass D mints cls_id D (not C), so neither marks C. `Marshal.load` /
   `Marshal.restore` can mint *any* user class, as can `.new` on a dynamic Class
   value -- either disables the gating (every class kept) so we never drop a
   live arm. Conservative by construction: this is a whole-program scan (it does
   not require the origination site to be reachable), so it only over-marks,
   never under-marks. The poly-dispatch switch reads `instantiated` to skip the
   `case` arm of a class no value can be (codegen_call.c). */
void compute_instantiated(Compiler *c) {
  const NodeTable *nt = c->nt;
  int disable = 0;
  /* Struct classes: conservatively live (their instances flow as poly). */
  for (int k = 0; k < c->nclasses; k++)
    if (c->classes[k].is_struct) c->classes[k].instantiated = 1;
  for (int id = 0; id < nt->count && !disable; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int recv = nt_ref(nt, id, "receiver");
    /* Marshal.load / Marshal.restore -> any class can be minted. */
    if (recv >= 0 && (sp_streq(name, "load") || sp_streq(name, "restore"))) {
      const char *rty = nt_type(nt, recv);
      const char *rn = nt_str(nt, recv, "name");
      if (rty && sp_streq(rty, "ConstantReadNode") && rn && sp_streq(rn, "Marshal")) {
        disable = 1; break;
      }
    }
    /* C.new / C.allocate */
    if (recv >= 0 && (sp_streq(name, "new") || sp_streq(name, "allocate"))) {
      const char *rty = nt_type(nt, recv);
      if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
        const char *cn = nt_str(nt, recv, "name");
        int ci = cn ? comp_class_index(c, cn) : -1;
        if (ci >= 0) c->classes[ci].instantiated = 1;
        /* an unknown constant is a builtin (Array.new, ...) -- not a user arm */
      }
      else if (comp_ntype(c, recv) == TY_CLASS) {
        /* `klass.new` on a dynamic Class value: unresolvable -> keep all. */
        disable = 1; break;
      }
    }
    /* raise Cls / raise Cls, msg : constructs an instance of Cls */
    if (recv < 0 && sp_streq(name, "raise")) {
      int rargs = nt_ref(nt, id, "arguments");
      int ran = 0; const int *rav = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &ran) : NULL;
      if (ran >= 1 && rav) {
        const char *aty = nt_type(nt, rav[0]);
        if (aty && (sp_streq(aty, "ConstantReadNode") || sp_streq(aty, "ConstantPathNode"))) {
          const char *cn = nt_str(nt, rav[0], "name");
          int ci = cn ? comp_class_index(c, cn) : -1;
          if (ci >= 0) c->classes[ci].instantiated = 1;
        }
      }
    }
  }
  if (disable)
    for (int k = 0; k < c->nclasses; k++) c->classes[k].instantiated = 1;
}

/* ---- proc capture detection (closures) ----
   A local read inside a proc body that isn't bound by the proc (param or a
   local the body itself writes) is a captured/free variable; its enclosing
   local must live in a heap cell so the closure and the enclosing scope share
   mutable storage. Mark those enclosing locals is_cell. */
/* ANameSet: moved to analyze_internal.h */
int aname_has(ANameSet *s, const char *nm) {
  if (!nm) return 1;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->v[i], nm)) return 1;
  return 0;
}
void aname_add(ANameSet *s, const char *nm) {
  if (aname_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
int a_nested_block(const char *ty) { return ty && (sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode")); }
int a_is_local_node(const char *ty) {
  return ty && (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
                sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
                sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode"));
}
int a_is_write_node(const char *ty) {
  return ty && (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableTargetNode") ||
                sp_streq(ty, "LocalVariableOperatorWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
                sp_streq(ty, "LocalVariableAndWriteNode"));
}
/* Mark every node id in the subtree (crossing nested blocks: a node inside an
   inner block is still "inside a proc"). */
void a_mark_subtree(Compiler *c, int id, char *inproc) {
  if (id < 0) return;
  inproc[id] = 1;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_mark_subtree(c, ch, inproc); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_mark_subtree(c, ids[k], inproc); }
}
/* Names used (read or written) anywhere in the proc/fiber body, INCLUDING
   nested blocks. A nested block (`Fiber.new { 3.times { |i| acc += i } }`) is
   inlined into the same flat C function as the body, so a use of an enclosing
   local there must still be seen as a capture. A nested block's own params /
   locals are collected too, but the caller's `owned` test (which requires a
   non-proc write in the enclosing scope) classifies them as block-local, not
   captures, so they are harmless. */
void a_collect_used(Compiler *c, int id, ANameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (a_is_local_node(ty)) aname_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_collect_used(c, ids[k], out); }
}
int a_proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && sp_streq(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  int block = nt_ref(c->nt, create, "block");
  if (block < 0) return -1;
  int bp = nt_ref(c->nt, block, "parameters");
  return bp < 0 ? -1 : nt_ref(c->nt, bp, "parameters");
}
int a_proc_body(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && sp_streq(ty, "LambdaNode")) return nt_ref(c->nt, create, "body");
  int block = nt_ref(c->nt, create, "block");
  return block >= 0 ? nt_ref(c->nt, block, "body") : -1;
}
/* A name used inside a proc is captured iff it belongs to the enclosing scope:
   it is an enclosing parameter, or it is assigned somewhere in the enclosing
   scope OUTSIDE any proc body. (A name assigned only inside the proc is a
   proc-local, not a capture -- Ruby's block-local rule.) Captured enclosing
   locals get a heap cell. */
/* A plain block `m(args) { ... }` passed to a method that keeps a real &block
   parameter (not yield-inlined) is lifted to a standalone proc function, so it
   captures enclosing variables exactly like a proc literal. */
int a_block_is_lifted(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int mi = -1;
  if (recv < 0) {
    mi = comp_method_index(c, name);
    if (mi < 0) { Scope *self = comp_scope_of(c, id); if (self && self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL); }
  }
else {
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  }
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  /* A lowered yielding method also receives its block as a real proc, so a
     block passed to it is lifted and captures enclosing locals like any other. */
  if (!m->blk_param || !m->blk_param[0] || m->yields) return 0;
  /* instance_eval/exec trampolines splice their block at the call site rather
     than lifting it to a proc, so they are not lifted-block captures. */
  if (m->class_id >= 0 && !m->is_cmethod && m->name &&
      comp_trampoline_kind(c, m->class_id, m->name, NULL)) return 0;
  return 1;
}

/* `Fiber.new { }` / `Enumerator.new { }` / `Thread.new { }` run their block on a
   fiber stack, so -- like an escaping proc -- an enclosing local they mutate must
   live in a shared heap cell rather than be captured by value. */
int a_is_fiber_or_gen_create(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "new") || nt_ref(nt, id, "block") < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && (sp_streq(rn, "Fiber") || sp_streq(rn, "Enumerator") || sp_streq(rn, "Thread"));
}

int a_proc_create_or_lifted(Compiler *c, int id) {
  return is_proc_create(c, id) || a_block_is_lifted(c, id) || a_is_fiber_or_gen_create(c, id);
}

void mark_proc_captures(Compiler *c) {
  const NodeTable *nt = c->nt;
  char *inproc = (char *)calloc((size_t)nt->count, 1);
  if (!inproc) return;
  for (int id = 0; id < nt->count; id++)
    if (a_proc_create_or_lifted(c, id)) { int body = a_proc_body(c, id); if (body >= 0) a_mark_subtree(c, body, inproc); }

  for (int id = 0; id < nt->count; id++) {
    if (!a_proc_create_or_lifted(c, id)) continue;
    /* A fiber/generator only needs a cell for a *value-type* capture, where a
       by-value copy would drop the write. A captured heap object (string, array,
       hash, ...) is already shared by pointer -- in-place mutation reaches the
       enclosing scope -- and the cell machinery does not handle some of those
       types (e.g. a mutable-string buffer), so leave them by value. */
    int fib_create = a_is_fiber_or_gen_create(c, id);
    int body = a_proc_body(c, id);
    if (body < 0) continue;
    int encl = c->nscope[id];
    ANameSet params = {0}, used = {0};
    int pn = a_proc_params_node(c, id);
    if (pn >= 0) { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn); for (int k = 0; k < rn; k++) aname_add(&params, nt_str(nt, reqs[k], "name")); }
    a_collect_used(c, body, &used);
    Scope *es = &c->scopes[encl];
    for (int u = 0; u < used.n; u++) {
      const char *nm = used.v[u];
      if (aname_has(&params, nm)) continue;          /* the proc's own param */
      LocalVar *lv = scope_local(es, nm);
      if (!lv) continue;                              /* not an enclosing local */
      int owned = lv->is_param;
      for (int w = 0; w < nt->count && !owned; w++) {
        if (c->nscope[w] != encl || inproc[w]) continue;
        if (!a_is_write_node(nt_type(nt, w))) continue;
        const char *wn = nt_str(nt, w, "name");
        if (wn && sp_streq(wn, nm)) owned = 1;
      }
      if (owned) {
        /* A fiber/generator now cells a captured heap object too (string /
           array / hash / object), via a typed-pointer cell, so a reassignment
           in the body reaches the enclosing scope. In-place mutation of a
           non-reassigned capture stays by pointer (the var isn't `owned`, so it
           never reaches here). Value-type objects have no stable pointer. */
        int heap_ptr = (lv->type == TY_STRING || ty_is_array(lv->type) ||
                        ty_is_hash(lv->type) || ty_is_object(lv->type)) &&
                       !comp_ty_value_obj(c, lv->type);
        if (fib_create && lv->type != TY_INT && lv->type != TY_BOOL &&
            lv->type != TY_FLOAT && lv->type != TY_POLY && !heap_ptr)
          continue;   /* capture type without a usable cell: leave by value */
        lv->is_cell = 1;
      }
    }
    free(params.v); free(used.v);
  }
  free(inproc);
}

/* ---- bigint loop-variable detection ---- */
/* Scan a while-loop body for `x = x * y` or `x *= y` patterns and collect
   the variable names in a heap-allocated array. Returns the count; caller
   must free the returned array. */
void bigint_scan_body(const NodeTable *nt, int id, char ***names, int *n, int *cap) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  /* x *= y  (LocalVariableOperatorWriteNode with * or **) */
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
    const char *op = nt_str(nt, id, "binary_operator");
    if (op && (sp_streq(op, "*") || sp_streq(op, "**"))) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) {
        for (int k = 0; k < *n; k++) if (sp_streq((*names)[k], nm)) goto skip_mul;
        if (*n >= *cap) { *cap = (*cap * 2) + 4; *names = (char **)realloc(*names, (size_t)*cap * sizeof(char *)); }
        (*names)[(*n)++] = (char *)nm;
        skip_mul:;
      }
    }
  }
  /* x = x * y  (LocalVariableWriteNode where value is CallNode * with recv = x) */
  if (sp_streq(ty, "LocalVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (nm && val >= 0 && sp_streq(nt_type(nt, val) ? nt_type(nt, val) : "", "CallNode")) {
      const char *op2 = nt_str(nt, val, "name");
      int recv2 = nt_ref(nt, val, "receiver");
      if (op2 && (sp_streq(op2, "*") || sp_streq(op2, "**")) && recv2 >= 0 &&
          sp_streq(nt_type(nt, recv2) ? nt_type(nt, recv2) : "", "LocalVariableReadNode") &&
          sp_streq(nt_str(nt, recv2, "name") ? nt_str(nt, recv2, "name") : "", nm)) {
        for (int k = 0; k < *n; k++) if (sp_streq((*names)[k], nm)) goto skip_lv;
        if (*n >= *cap) { *cap = (*cap * 2) + 4; *names = (char **)realloc(*names, (size_t)*cap * sizeof(char *)); }
        (*names)[(*n)++] = (char *)nm;
        skip_lv:;
      }
    }
  }
  /* Recurse into body / stmts / subsequent */
  bigint_scan_body(nt, nt_ref(nt, id, "body"), names, n, cap);
  int sn = 0; const int *stmts2 = nt_arr(nt, id, "body", &sn);
  for (int k = 0; k < sn; k++) bigint_scan_body(nt, stmts2[k], names, n, cap);
  bigint_scan_body(nt, nt_ref(nt, id, "subsequent"), names, n, cap);
}

void detect_bigint_loop_vars(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "WhileNode")) continue;
    int body = nt_ref(nt, id, "statements");
    if (body < 0) continue;
    char **cands = NULL; int ncands = 0, cap = 0;
    bigint_scan_body(nt, body, &cands, &ncands, &cap);
    /* Promote matching TY_INT locals to TY_BIGINT */
    for (int k = 0; k < ncands; k++) {
      Scope *s = comp_scope_of(c, id);
      LocalVar *lv = s ? scope_local(s, cands[k]) : NULL;
      if (lv && lv->type == TY_INT && !lv->rbs_seeded) lv->type = TY_BIGINT;
    }
    free(cands);
  }
}

/* After detect_bigint_loop_vars promotes some locals to TY_BIGINT, cascade
   the promotion to variables assigned from bigint-typed expressions. */
void propagate_bigint_cascade(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int id = 0; id < nt->count; id++) {
      const char *ty = nt_type(nt, id);
      if (!ty) continue;
      if (sp_streq(ty, "LocalVariableWriteNode")) {
        const char *nm = nt_str(nt, id, "name");
        Scope *s = comp_scope_of(c, id);
        LocalVar *lv = nm ? scope_local(s, nm) : NULL;
        if (!lv || lv->type != TY_INT || lv->rbs_seeded) continue;
        TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
        if (vt == TY_BIGINT) { lv->type = TY_BIGINT; changed = 1; }
      }
      else if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
        const char *nm = nt_str(nt, id, "name");
        Scope *s = comp_scope_of(c, id);
        LocalVar *lv = nm ? scope_local(s, nm) : NULL;
        if (!lv || lv->type != TY_INT || lv->rbs_seeded) continue;
        TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
        if (vt == TY_BIGINT) { lv->type = TY_BIGINT; changed = 1; }
      }
    }
  }
}

/* For nodes inside an instance_eval/exec block, the receiver's class id; -1
   elsewhere. Lets bare calls/ivar refs in the block resolve against the
   receiver's class during inference (codegen mirrors this via an_ie_class_id). */
int *g_ie_node_class = NULL;
static int g_ie_node_class_cap = 0;

void mark_ie_subtree(Compiler *c, int node, int cls) {
  if (node < 0) return;
  const char *ty = nt_type(c->nt, node);
  if (!ty) return;
  /* a nested def/class starts a fresh self; don't bleed the rebind into it */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  g_ie_node_class[node] = cls;
  int nr = nt_num_refs(c->nt, node);
  for (int i = 0; i < nr; i++) mark_ie_subtree(c, nt_ref_at(c->nt, node, i), cls);
  int na = nt_num_arrs(c->nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, node, i, &n); for (int k = 0; k < n; k++) mark_ie_subtree(c, ids[k], cls); }
}

/* `A = SomeClass` (a constant aliasing a class) then `A.foo`: rewrite the
   ConstantRead receiver's name to the underlying class so class-method dispatch
   resolves it exactly like the direct `SomeClass.foo`. Mirrors the `class CONST`
   reopening rewrite in walk_scope. Runs once after classes are registered. */
void rewrite_const_alias_receivers(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "ConstantReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || comp_class_index(c, rn) >= 0) continue;  /* already a class name */
    const char *real = resolve_class_alias(c, rn);
    if (real && !sp_streq(real, rn)) {
      char buf[256]; snprintf(buf, sizeof buf, "%s", real);  /* copy: set frees rn */
      nt_set_str((NodeTable *)nt, recv, "name", buf);
    }
  }
}

/* For a receiverless instance_eval/exec CallNode with a literal block inside
   an instance method, the receiver is self (CRuby resolves it to
   self.instance_exec). Return that class index, else -1. The literal-block
   requirement (a BlockNode, not a `&b` BlockArgumentNode) keeps this distinct
   from a trampoline body's `instance_exec(args, &b)`, which codegen lowers via
   its own trampoline detector. */
int ie_implicit_self_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "receiver") >= 0) return -1;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "instance_eval") && !sp_streq(nm, "instance_exec"))) return -1;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return -1;
  const char *bty = nt_type(nt, blk);
  /* A literal block, or a `&b` forward of the enclosing method's block (which
     resolves to the literal active where the method inlines). */
  if (!bty || (!sp_streq(bty, "BlockNode") && !sp_streq(bty, "BlockArgumentNode"))) return -1;
  Scope *s = comp_scope_of(c, id);
  if (!s || s->class_id < 0 || s->is_cmethod) return -1;
  return s->class_id;
}

/* In a call-site KeywordHashNode (`k: 9, j: 2`), the value node bound to the
   keyword `name`, or -1. Used to match instance_exec keyword block params. */
int ie_kwhash_value(Compiler *c, int kwhash, const char *name) {
  const NodeTable *nt = c->nt;
  if (kwhash < 0 || !name) return -1;
  int en = 0; const int *els = nt_arr(nt, kwhash, "elements", &en);
  for (int i = 0; i < en; i++) {
    const char *ety = nt_type(nt, els[i]);
    if (!ety || !sp_streq(ety, "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
    if (!kty || !sp_streq(kty, "SymbolNode")) continue;
    const char *kn = nt_str(nt, key, "value");
    if (kn && sp_streq(kn, name)) return nt_ref(nt, els[i], "value");
  }
  return -1;
}

/* The trailing KeywordHashNode of a call's arguments (`k: 1`), or -1. */
int ie_call_kwhash(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac <= 0) return -1;
  const char *lty = nt_type(nt, av[ac - 1]);
  return (lty && sp_streq(lty, "KeywordHashNode")) ? av[ac - 1] : -1;
}

/* For a call `recv.m(cargs) { ... }` to an instance_exec trampoline
   `def m(p..., &b); instance_exec(tbody..., &b); end`, the node to bind/emit
   for the block's p-th parameter: the p-th trampoline-body arg, with a read of
   one of the trampoline's own positional params rewritten to the matching
   caller argument. Returns -1 when out of range, when not such a trampoline, or
   when tbody uses a splat (the existing 1:1 forwarding path handles that).
   ie_tramp_effective_argc returns the tbody arg count (or -1 to bail). */
static int ie_tramp_body_args(Compiler *c, int caller_id, const int **tav_out, Scope **ms_out) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, caller_id, "receiver");
  if (recv < 0) return -1;
  TyKind rt = infer_type(c, recv);
  if (!ty_is_object(rt)) return -1;
  const char *nm = nt_str(nt, caller_id, "name");
  int mi = nm ? comp_method_in_chain(c, ty_object_class(rt), nm, NULL) : -1;
  if (mi < 0) return -1;
  Scope *ms = &c->scopes[mi];
  if (ms->body < 0) return -1;
  int bn = 0; const int *bb = nt_arr(nt, ms->body, "body", &bn);
  if (bn != 1 || !bb) return -1;
  int targs = nt_ref(nt, bb[0], "arguments");
  int tac = 0; const int *tav = targs >= 0 ? nt_arr(nt, targs, "arguments", &tac) : NULL;
  for (int i = 0; i < tac; i++) {
    const char *aty = nt_type(nt, tav[i]);
    if (aty && sp_streq(aty, "SplatNode")) return -1;  /* forwarding path handles splat */
  }
  if (tav_out) *tav_out = tav;
  if (ms_out) *ms_out = ms;
  return tac;
}

int ie_tramp_effective_argc(Compiler *c, int caller_id) {
  return ie_tramp_body_args(c, caller_id, NULL, NULL);
}

int ie_tramp_effective_arg(Compiler *c, int caller_id, int p) {
  const NodeTable *nt = c->nt;
  const int *tav = NULL; Scope *ms = NULL;
  int tac = ie_tramp_body_args(c, caller_id, &tav, &ms);
  if (tac < 0 || p < 0 || p >= tac) return -1;
  int arg = tav[p];
  const char *aty = nt_type(nt, arg);
  if (aty && sp_streq(aty, "LocalVariableReadNode")) {
    const char *an = nt_str(nt, arg, "name");
    for (int j = 0; j < ms->nparams; j++) {
      if (ms->pnames[j] && an && sp_streq(ms->pnames[j], an)) {
        int cargs = nt_ref(nt, caller_id, "arguments");
        int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
        return j < cac ? cav[j] : -1;
      }
    }
  }
  return arg;  /* ivar / literal / other: evaluated in the rebound-self context */
}

/* (Re)build the instance_eval/exec node→class map from current receiver types. */
void build_ie_map(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* sized to nt->count, which can grow mid-analysis when forwarded callables
     are desugared into synthetic blocks; resize so per-node writes stay bounded */
  if (g_ie_node_class_cap < nt->count) {
    int *grown = realloc(g_ie_node_class, sizeof(int) * (size_t)nt->count);
    if (!grown) return;  /* OOM: keep the old map rather than leak/deref NULL */
    g_ie_node_class = grown;
    g_ie_node_class_cap = nt->count;
  }
  for (int i = 0; i < nt->count; i++) g_ie_node_class[i] = -1;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int cls;
    if (recv < 0) {
      /* receiverless instance_eval/exec inside an instance method: self. */
      cls = ie_implicit_self_class(c, id);
      if (cls < 0) continue;
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (!ty_is_object(rt)) continue;
      cls = ty_object_class(rt);
      if (!sp_streq(nm, "instance_eval") && !sp_streq(nm, "instance_exec")) {
        /* not a direct instance_eval/exec: maybe a trampoline method on `cls`? */
        if (!comp_trampoline_kind(c, cls, nm, NULL)) continue;
      }
    }
    int body = nt_ref(nt, blk, "body");
    if (body >= 0) mark_ie_subtree(c, body, cls);
  }
}

/* The receiver class for a node inside an instance_eval/exec block, or -1. */
int ie_class_of(Compiler *c, int node) {
  (void)c;
  /* g_ie_node_class is sized to nt->count per fixpoint iteration; a node
     synthesized mid-iteration (id >= cap) has no instance_eval receiver yet. */
  return (g_ie_node_class && node >= 0 && node < g_ie_node_class_cap)
           ? g_ie_node_class[node] : -1;
}

/* Register an ivar first assigned inside an instance_exec/instance_eval block on
   the block's receiver class. register_locals only interns ivar writes whose
   enclosing scope is a class body or method; an ivar written solely inside a
   lifted iexec block has no such scope, so without this it gets no struct slot
   (and any read of it fails to resolve). Runs in the fixpoint right after
   build_ie_map; returns 1 when it adds a new slot so the fixpoint re-runs and
   infers the new ivar's type from its assignment. */
int register_ie_block_ivars(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!g_ie_node_class) return 0;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    int cls = g_ie_node_class[id];
    if (cls < 0) continue;
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!sp_streq(ty, "InstanceVariableWriteNode") &&
        !sp_streq(ty, "InstanceVariableOperatorWriteNode") &&
        !sp_streq(ty, "InstanceVariableOrWriteNode") &&
        !sp_streq(ty, "InstanceVariableAndWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    ClassInfo *ci = &c->classes[cls];
    int iv = comp_ivar_index(ci, nm);
    if (iv < 0) { iv = comp_ivar_intern(ci, nm); changed = 1; }
    /* Type the slot from the assignment value. The general ivar-write inference
       keys on the write's enclosing scope class_id, which for a lifted iexec
       block is the toplevel/method, not the receiver -- so it never types this
       slot. An ivar written only inside iexec blocks has no competing writer, so
       widening it here from the value type is safe. */
    int v = nt_ref(nt, id, "value");
    if (v >= 0) {
      TyKind vt = infer_type(c, v);
      if (vt != TY_UNKNOWN && vt != TY_NIL) {
        TyKind cur = ci->ivar_types[iv];
        TyKind nu = (cur == TY_UNKNOWN) ? vt : ty_unify(cur, vt);
        if (nu != cur) { ci->ivar_types[iv] = nu; changed = 1; }
      }
    }
  }
  return changed;
}

/* ---- Block/lambda parameter alpha-renaming ----------------------------
 * Block and lambda parameters are interned into the *enclosing* scope, so a
 * parameter sharing a name with an enclosing local collapses onto a single
 * LocalVar (hence one type), corrupting both. Ruby semantics say the two are
 * distinct (the parameter shadows). When the name is also assigned outside the
 * block body -- the case that pollutes the shared type -- rename the parameter
 * and its in-body references to a fresh, collision-free name so they become
 * separate variables. Runs before walk_scope so all downstream interning and
 * codegen see the disambiguated names. */

/* The ParametersNode for a block (BlockParametersNode -> ParametersNode) or a
   lambda (ParametersNode directly). -1 if none / not a plain ParametersNode. */
int blkp_params_node(Compiler *c, int create) {
  const NodeTable *nt = c->nt;
  int pn = nt_ref(nt, create, "parameters");
  if (pn < 0) return -1;
  const char *pty = nt_type(nt, pn);
  if (pty && sp_streq(pty, "BlockParametersNode")) pn = nt_ref(nt, pn, "parameters");
  return pn;
}

int blkp_binds_param(Compiler *c, int create, const char *name) {
  int pn = blkp_params_node(c, create);
  if (pn < 0) return 0;
  const char *pty = nt_type(c->nt, pn);
  if (!pty || !sp_streq(pty, "ParametersNode")) return 0;
  int rn = 0; const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
  for (int i = 0; i < rn; i++) {
    const char *p = nt_str(c->nt, reqs[i], "name");
    if (p && sp_streq(p, name)) return 1;
  }
  return 0;
}

int lv_node_is_named_ref(const char *ty) {
  return ty && (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
                sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
                sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode"));
}
int lv_node_is_write(const char *ty) {
  return ty && (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableTargetNode") ||
                sp_streq(ty, "LocalVariableOperatorWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
                sp_streq(ty, "LocalVariableAndWriteNode"));
}

/* Rewrite references to `oldn` -> `newn`, stopping at nested defs/classes and
   at nested blocks/lambdas that re-bind `oldn`. */
void blkp_rewrite_refs(Compiler *c, int node, const char *oldn, const char *newn) {
  if (node < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  if ((sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode")) && blkp_binds_param(c, node, oldn)) return;
  if (lv_node_is_named_ref(ty)) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && sp_streq(nm, oldn)) nt_set_str((NodeTable *)nt, node, "name", newn);
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) blkp_rewrite_refs(c, nt_ref_at(nt, node, i), oldn, newn);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) blkp_rewrite_refs(c, ids[k], oldn, newn); }
}

void blkp_mark_subtree(const NodeTable *nt, int node, char *marks) {
  if (node < 0) return;
  marks[node] = 1;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) blkp_mark_subtree(nt, nt_ref_at(nt, node, i), marks);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) blkp_mark_subtree(nt, ids[k], marks); }
}
/* Generation-stamping variant: writes `gen` instead of 1, so the membership
   array can be reused across blocks without an O(n) memset per block (a node is
   "in body" iff stamp[node] == gen). */
static void blkp_stamp_subtree(const NodeTable *nt, int node, int *stamp, int gen) {
  if (node < 0) return;
  stamp[node] = gen;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) blkp_stamp_subtree(nt, nt_ref_at(nt, node, i), stamp, gen);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) blkp_stamp_subtree(nt, ids[k], stamp, gen); }
}

/* A block/lambda parameter is interned into the enclosing (flat) scope, so two
   blocks reusing a name -- or a block param sharing a name with an enclosing
   local -- collapse onto one LocalVar and one type. The rename pass below splits
   them, but only for nodes this predicate accepts. We accept any block owned by
   a call: the collision check in rename_shadowing_block_params is the real
   filter (it fires only when the name is actually shared), and codegen reads
   every param name through block_param_name + rename_local, so a renamed slot
   stays consistent in the inliner, the standalone-proc lowering, and the
   instance_eval/exec splice path alike. Returns 1 if `L` is such a node. */
int blkp_needs_rename(Compiler *c, int L) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, L);
  if (ty && sp_streq(ty, "LambdaNode")) return 1;
  if (!ty || !sp_streq(ty, "BlockNode")) return 0;
  /* A block owned by a call is renameable. Ordinary iteration blocks
     (each/map/select/...) were once excluded on the assumption the inliner's
     save/restore made them shadow-safe; that holds for the element-typed shadow
     path but not when sibling blocks of divergent element types share a name
     (e.g. `arr.map{|x| x+0.5}.map{|x| x.floor}` -- the poly-array map leg writes
     the shared poly slot), so they go through the collision gate too. */
  for (int id = 0; id < nt->count; id++) {
    if (nt_ref(nt, id, "block") != L) continue;
    return nt_str(nt, id, "name") != NULL;
  }
  return 0;
}

/* ---- Colliding nested-constant qualification --------------------------
 * Constants live in a flat cst_<NAME> namespace, so `RootNS::Mid::LEAF` and
 * `Lex::RootNS::Mid::LEAF` collide. When the same constant name is written
 * under 2+ distinct module paths, rename each nested write to a qualified
 * `<Mod>__..__<NAME>` and rewrite every path read to whichever qualified
 * constant it denotes (relative reads prefer the lexically enclosing module
 * chain; `::`-anchored reads resolve from the root). Collision-gated: programs
 * with unique constant names are untouched. */


/* QCWrite: moved to analyze_internal.h */

void qc_collect_writes(Compiler *c, int node, char (*path)[64], int depth,
                              QCWrite **ws, int *n, int *cap) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  if ((sp_streq(ty, "ModuleNode") || sp_streq(ty, "ClassNode")) && depth < QC_MAXDEPTH) {
    int cp = nt_ref(nt, node, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (mn) {
      snprintf(path[depth], 64, "%s", mn);
      depth++;
    }
  }
  else if (sp_streq(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, node, "name");
    if (nm) {
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 16; *ws = realloc(*ws, sizeof(QCWrite) * (size_t)*cap); }
      QCWrite *w = &(*ws)[(*n)++];
      w->node = node; w->depth = depth;
      for (int i = 0; i < depth; i++) snprintf(w->path[i], 64, "%s", path[i]);
      snprintf(w->name, sizeof w->name, "%s", nm);
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) qc_collect_writes(c, nt_ref_at(nt, node, i), path, depth, ws, n, cap);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) qc_collect_writes(c, ids[k], path, depth, ws, n, cap); }
}

/* Reconstruct a path read's chain (["RootNS","Mid","LEAF"]) and whether it is
   root-anchored. Returns the chain length, or 0 if unsupported. */
int qc_read_chain(const NodeTable *nt, int node, char (*chain)[64], int *abs_anchor) {
  char rev[QC_MAXDEPTH + 1][64];
  int n = 0;
  int cur = node;
  *abs_anchor = 0;
  while (cur >= 0 && n <= QC_MAXDEPTH) {
    const char *ty = nt_type(nt, cur);
    const char *nm = nt_str(nt, cur, "name");
    if (!ty || !nm) return 0;
    snprintf(rev[n++], 64, "%s", nm);
    if (sp_streq(ty, "ConstantReadNode")) break;
    if (!sp_streq(ty, "ConstantPathNode")) return 0;
    int par = nt_ref(nt, cur, "parent");
    if (par < 0) { *abs_anchor = 1; break; }
    cur = par;
  }
  for (int i = 0; i < n; i++) snprintf(chain[i], 64, "%s", rev[n - 1 - i]);
  return n;
}

void qc_qualified_name(char *out, size_t cap, const QCWrite *w) {
  out[0] = 0;
  for (int i = 0; i < w->depth; i++) { strncat(out, w->path[i], cap - strlen(out) - 1); strncat(out, "__", cap - strlen(out) - 1); }
  strncat(out, w->name, cap - strlen(out) - 1);
}

/* Reverse-reference flags for qc_rewrite_reads, built once per top-level call
   (not rescanned per constant node, which made the pass O(constants * nodes) on
   a flattened runtime). qc_cpath_parent[id]: some ConstantPathNode has parent
   == id. qc_def_cpath[id]: some Class/ModuleNode has constant_path == id. */
static unsigned char *qc_cpath_parent = NULL;
static unsigned char *qc_def_cpath = NULL;
static void qc_build_reverse_flags(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  qc_cpath_parent = calloc((size_t)n, 1);
  qc_def_cpath = calloc((size_t)n, 1);
  if (!qc_cpath_parent || !qc_def_cpath) return;
  for (int q = 0; q < n; q++) {
    const char *qt = nt_type(nt, q);
    if (!qt) continue;
    if (sp_streq(qt, "ConstantPathNode")) {
      int p = nt_ref(nt, q, "parent");
      if (p >= 0 && p < n) qc_cpath_parent[p] = 1;
    }
    else if (sp_streq(qt, "ClassNode") || sp_streq(qt, "ModuleNode")) {
      int cp = nt_ref(nt, q, "constant_path");
      if (cp >= 0 && cp < n) qc_def_cpath[cp] = 1;
    }
  }
}
static void qc_free_reverse_flags(void) {
  free(qc_cpath_parent); qc_cpath_parent = NULL;
  free(qc_def_cpath); qc_def_cpath = NULL;
}
void qc_rewrite_reads(Compiler *c, int node, char (*mods)[64], int mdepth,
                             QCWrite *ws, int wn) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  int depth = mdepth;
  char (*path)[64] = mods;
  if ((sp_streq(ty, "ModuleNode") || sp_streq(ty, "ClassNode")) && depth < QC_MAXDEPTH) {
    int cp = nt_ref(nt, node, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (mn) { snprintf(path[depth], 64, "%s", mn); depth++; }
  }
  else if (sp_streq(ty, "ConstantReadNode")) {
    /* bare constant read inside a module body: resolve lexically innermost-
       first against the colliding writes. Skip reads that are a path's parent
       (handled via the chain) or a class/module definition name. */
    const char *nm = nt_str(nt, node, "name");
    int part_of_other = (qc_cpath_parent && qc_cpath_parent[node]) ||
                        (qc_def_cpath && qc_def_cpath[node]);
    if (nm && !part_of_other) {
      int involved = 0;
      for (int i = 0; i < wn; i++) if (sp_streq(ws[i].name, nm)) { involved = 1; break; }
      if (involved) {
        for (int pref = depth; pref >= 0; pref--) {
          int matched = -1;
          for (int i = 0; i < wn && matched < 0; i++) {
            if (!sp_streq(ws[i].name, nm) || ws[i].depth != pref) continue;
            int ok = 1;
            for (int j = 0; j < pref && ok; j++) if (!sp_streq(ws[i].path[j], path[j])) ok = 0;
            if (ok) matched = i;
          }
          if (matched >= 0) {
            if (ws[matched].depth > 0) {
              char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[matched]);
              nt_set_str((NodeTable *)nt, node, "name", qn);
            }
            break;
          }
        }
      }
    }
  }
  else if (sp_streq(ty, "ConstantPathNode")) {
    /* only process path heads: skip if this node is some other path's parent */
    int is_parent = qc_cpath_parent && qc_cpath_parent[node];
    if (!is_parent) {
      char chain[QC_MAXDEPTH + 1][64];
      int abs_anchor = 0;
      int cl = qc_read_chain(nt, node, chain, &abs_anchor);
      if (cl >= 2) {
        const char *cname = chain[cl - 1];
        /* does this name participate in a collision? */
        int involved = 0;
        for (int i = 0; i < wn; i++) if (sp_streq(ws[i].name, cname)) { involved = 1; break; }
        if (involved) {
          /* try lexical prefixes innermost-first (relative), or only the root (::) */
          int max_pref = abs_anchor ? 0 : depth;
          for (int pref = max_pref; pref >= 0; pref--) {
            int matched = -1;
            for (int i = 0; i < wn && matched < 0; i++) {
              if (!sp_streq(ws[i].name, cname)) continue;
              if (ws[i].depth != pref + (cl - 1)) continue;
              int ok = 1;
              for (int j = 0; j < pref && ok; j++) if (!sp_streq(ws[i].path[j], path[j])) ok = 0;
              for (int j = 0; j < cl - 1 && ok; j++) if (!sp_streq(ws[i].path[pref + j], chain[j])) ok = 0;
              if (ok) matched = i;
            }
            if (matched >= 0) {
              if (ws[matched].depth > 0) {
                char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[matched]);
                nt_set_str((NodeTable *)nt, node, "name", qn);
              }
              break;
            }
          }
        }
      }
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) qc_rewrite_reads(c, nt_ref_at(nt, node, i), path, depth, ws, wn);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) qc_rewrite_reads(c, ids[k], path, depth, ws, wn); }
}

void qualify_colliding_consts(Compiler *c) {
  const NodeTable *nt = c->nt;
  QCWrite *ws = NULL; int wn = 0, wcap = 0;
  char path[QC_MAXDEPTH][64];
  qc_collect_writes(c, nt->root_id, path, 0, &ws, &wn, &wcap);
  /* keep only names written under 2+ distinct module paths */
  int any = 0;
  for (int i = 0; i < wn; i++) {
    int collide = 0;
    for (int j = 0; j < wn && !collide; j++) {
      if (i == j || !sp_streq(ws[i].name, ws[j].name)) continue;
      if (ws[i].depth != ws[j].depth) { collide = 1; break; }
      for (int k = 0; k < ws[i].depth; k++) if (!sp_streq(ws[i].path[k], ws[j].path[k])) { collide = 1; break; }
    }
    if (!collide) { ws[i] = ws[--wn]; i--; continue; }
    any = 1;
  }
  if (any) {
    /* rewrite reads first (they match against the original write names) */
    char mods[QC_MAXDEPTH][64];
    qc_build_reverse_flags(c);
    qc_rewrite_reads(c, nt->root_id, mods, 0, ws, wn);
    qc_free_reverse_flags();
    /* then qualify the nested writes themselves */
    for (int i = 0; i < wn; i++) {
      if (ws[i].depth == 0) continue;
      char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[i]);
      nt_set_str((NodeTable *)nt, ws[i].node, "name", qn);
    }
  }
  free(ws);
}

/* ---- Colliding class/module-name qualification ------------------------
 * Classes/modules live in a flat sp_<Name> C namespace keyed by the leaf
 * name, so `Web::Response` and `Chat::Response` collapse onto one ClassInfo
 * (struct + methods merge, the last `initialize` wins -> uninitialized ivars
 * -> SIGSEGV). This is the class-definition analogue of the nested-constant
 * pass above: when the same class/module leaf name is defined under 2+
 * distinct module paths, rename each nested definition to a qualified
 * `<Mod>__..__<NAME>` and rewrite every reference to whichever qualified
 * class it denotes (relative reads prefer the lexically enclosing module
 * chain; `::`-anchored reads resolve from the root). Collision-gated: a
 * program whose class names are all unique is untouched (so the common case,
 * optcarrot, and the self-host build see zero change). The rewrite happens
 * before walk_scope, so registration (comp_class_new) and every reference
 * lookup (comp_class_index) naturally key on the now-distinct names, and the
 * emitted C identifier (sp_<name>) is distinct too -- no change to the many
 * leaf-name read sites. */
void qc_collect_class_writes(Compiler *c, int node, char (*path)[64], int depth,
                             QCWrite **ws, int *n, int *cap) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  if ((sp_streq(ty, "ModuleNode") || sp_streq(ty, "ClassNode")) && depth < QC_MAXDEPTH) {
    int cp = nt_ref(nt, node, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (mn) {
      /* record this class/module definition as a "write" at the current
         (pre-push) depth -- ws[i].node is the constant_path node whose name
         the write-rewrite pass will qualify. */
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 16; *ws = realloc(*ws, sizeof(QCWrite) * (size_t)*cap); }
      QCWrite *w = &(*ws)[(*n)++];
      w->node = cp; w->depth = depth;
      for (int i = 0; i < depth; i++) snprintf(w->path[i], 64, "%s", path[i]);
      snprintf(w->name, sizeof w->name, "%s", mn);
      snprintf(path[depth], 64, "%s", mn);
      depth++;
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) qc_collect_class_writes(c, nt_ref_at(nt, node, i), path, depth, ws, n, cap);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) qc_collect_class_writes(c, ids[k], path, depth, ws, n, cap); }
}

void qualify_colliding_classes(Compiler *c) {
  const NodeTable *nt = c->nt;
  QCWrite *ws = NULL; int wn = 0, wcap = 0;
  char path[QC_MAXDEPTH][64];
  qc_collect_class_writes(c, nt->root_id, path, 0, &ws, &wn, &wcap);
  /* keep only leaf names defined under 2+ distinct module paths */
  int any = 0;
  for (int i = 0; i < wn; i++) {
    int collide = 0;
    for (int j = 0; j < wn && !collide; j++) {
      if (i == j || !sp_streq(ws[i].name, ws[j].name)) continue;
      if (ws[i].depth != ws[j].depth) { collide = 1; break; }
      for (int k = 0; k < ws[i].depth; k++) if (!sp_streq(ws[i].path[k], ws[j].path[k])) { collide = 1; break; }
    }
    if (!collide) { ws[i] = ws[--wn]; i--; continue; }
    any = 1;
  }
  if (any) {
    /* rewrite references first (they match against the original leaf names),
       then qualify the nested definitions themselves */
    char mods[QC_MAXDEPTH][64];
    qc_build_reverse_flags(c);
    qc_rewrite_reads(c, nt->root_id, mods, 0, ws, wn);
    qc_free_reverse_flags();
    for (int i = 0; i < wn; i++) {
      if (ws[i].depth == 0) continue;
      char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[i]);
      nt_set_str((NodeTable *)nt, ws[i].node, "name", qn);
    }
  }
  free(ws);
}

void rename_shadowing_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  /* Reverse map block-node -> owning node (the node whose "block" ref is it),
     built in one O(n) pass. blkp_needs_rename otherwise rescans all n nodes per
     block, making this whole pass O(blocks*n) on large inputs (a flattened
     runtime is ~500k nodes). */
  int *owner = malloc((size_t)n * sizeof(int));
  if (!owner) return;
  for (int i = 0; i < n; i++) owner[i] = -1;
  for (int id = 0; id < n; id++) {
    int b = nt_ref(nt, id, "block");
    if (b >= 0 && b < n) owner[b] = id;
  }
  /* inbody membership via generation stamp (avoids an O(n) memset per block). */
  int *inbody = calloc((size_t)n, sizeof(int));
  if (!inbody) { free(owner); return; }
  /* All local-variable-write and required-block-param nodes, collected once;
     the per-param collision scan iterates this set rather than all n nodes (it
     re-reads each name fresh, so a rename made earlier in this pass is still
     reflected). */
  int *wp = malloc((size_t)n * sizeof(int));
  int wpn = 0;
  if (!wp) { free(owner); free(inbody); return; }
  for (int w = 0; w < n; w++) {
    const char *wty = nt_type(nt, w);
    if (lv_node_is_write(wty) || (wty && sp_streq(wty, "RequiredParameterNode"))) wp[wpn++] = w;
  }
  int gen = 0;
  for (int L = 0; L < n; L++) {
    const char *ty = nt_type(nt, L);
    if (!ty) continue;
    int is_lambda = sp_streq(ty, "LambdaNode");
    if (!is_lambda && !sp_streq(ty, "BlockNode")) continue;
    /* renameable: a lambda, or a block owned by a named call (see
       blkp_needs_rename) -- resolved in O(1) through the owner index. */
    if (!is_lambda) {
      int o = owner[L];
      if (o < 0 || nt_str(nt, o, "name") == NULL) continue;
    }
    int pn = blkp_params_node(c, L);
    if (pn < 0) continue;
    const char *pty = nt_type(nt, pn);
    if (!pty || !sp_streq(pty, "ParametersNode")) continue;  /* numbered params handled elsewhere */
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    if (rn == 0) continue;
    int body = nt_ref(nt, L, "body");
    if (body < 0) continue;
    gen++;
    blkp_stamp_subtree(nt, body, inbody, gen);
    for (int i = 0; i < rn; i++) {
      const char *p = nt_str(nt, reqs[i], "name");
      if (!p) continue;
      /* collision: the name is used outside this block's body -- as a local
         write/read or as another block's parameter (param-vs-param, e.g. two
         inject folds sharing `|a, b|` with different element types). Both pollute
         the shared LocalVar's type. */
      int collide = 0;
      for (int wi = 0; wi < wpn && !collide; wi++) {
        int w = wp[wi];
        if (inbody[w] == gen) continue;
        const char *wty = nt_type(nt, w);
        int is_param_node = wty && sp_streq(wty, "RequiredParameterNode");
        /* don't let this block's own parameter nodes count as a collision */
        if (is_param_node) {
          int own = 0;
          for (int q = 0; q < rn; q++) if (reqs[q] == w) { own = 1; break; }
          if (own) continue;
        }
        const char *wn = nt_str(nt, w, "name");
        if (wn && sp_streq(wn, p)) collide = 1;
      }
      if (!collide) continue;
      char oldn[160], newn[176];
      snprintf(oldn, sizeof oldn, "%s", p);   /* copy: nt_set_str frees p's storage */
      snprintf(newn, sizeof newn, "%s__bp%d", oldn, L);
      nt_set_str((NodeTable *)nt, reqs[i], "name", newn);
      blkp_rewrite_refs(c, body, oldn, newn);
    }
  }
  free(wp);
  free(inbody);
  free(owner);
}

/* ---- --rbs advisory type seeds ----
   spinel_rbs_extract emits line-oriented seeds, read from the file named by
   SPINEL_RBS_SEED. Before the fixpoint we pin the named params / returns /
   ivars to the seeded type; guards at the inference write sites then keep the
   fixpoint from widening a pinned slot (legacy "RBS wins" semantics). Type
   tokens with no precise C kind (poly*, sym_array, obj_X_ptr_array, unknown
   classes) are skipped, so a seed never makes inference worse. Entirely inert
   when SPINEL_RBS_SEED is unset -- the normal compile path is unchanged. */

static TyKind parse_seed_type(Compiler *c, const char *tok) {
  if (!tok || !*tok) return TY_UNKNOWN;
  size_t n = strlen(tok);
  char buf[128];
  if (n >= sizeof buf) return TY_UNKNOWN;
  memcpy(buf, tok, n + 1);
  /* A trailing nullable '?' has no C scalar kind (objects are NULL-nullable
     already); pin to the base type. */
  if (n > 0 && buf[n - 1] == '?') buf[--n] = '\0';
  if (sp_streq(buf, "int"))    return TY_INT;
  if (sp_streq(buf, "float"))  return TY_FLOAT;
  if (sp_streq(buf, "string") || sp_streq(buf, "str")) return TY_STRING;
  if (sp_streq(buf, "symbol") || sp_streq(buf, "sym")) return TY_SYMBOL;
  if (sp_streq(buf, "bool"))   return TY_BOOL;
  if (sp_streq(buf, "nil"))    return TY_NIL;
  if (sp_streq(buf, "void"))   return TY_VOID;
  if (sp_streq(buf, "int_array"))    return TY_INT_ARRAY;
  if (sp_streq(buf, "float_array"))  return TY_FLOAT_ARRAY;
  if (sp_streq(buf, "str_array"))    return TY_STR_ARRAY;
  if (sp_streq(buf, "str_int_hash"))   return TY_STR_INT_HASH;
  if (sp_streq(buf, "str_str_hash"))   return TY_STR_STR_HASH;
  if (sp_streq(buf, "int_int_hash"))   return TY_INT_INT_HASH;
  if (sp_streq(buf, "int_str_hash"))   return TY_INT_STR_HASH;
  if (sp_streq(buf, "sym_poly_hash"))  return TY_SYM_POLY_HASH;
  if (sp_streq(buf, "str_poly_hash"))  return TY_STR_POLY_HASH;
  if (sp_streq(buf, "poly_poly_hash")) return TY_POLY_POLY_HASH;
  if (!strncmp(buf, "obj_", 4)) {
    int ci = comp_class_index(c, buf + 4);
    return ci >= 0 ? ty_object(ci) : TY_UNKNOWN;
  }
  return TY_UNKNOWN;
}

/* Build ci's fully-qualified name as `Outer_Inner_Leaf` -- the module path
   joined with `_`, matching the form spinel_rbs_extract emits for a seed
   `class` line. The compiler's class table stores only the leaf name (e.g.
   `Flash`) plus an enclosing_class link, so a qualified seed name needs this
   to match. */
static void class_qualified_name(Compiler *c, int ci, char *out, size_t cap) {
  int chain[64];
  int n = 0;
  for (int x = ci; x >= 0 && n < (int)(sizeof chain / sizeof chain[0]);
       x = c->classes[x].enclosing_class)
    chain[n++] = x;
  size_t j = 0;
  for (int i = n - 1; i >= 0; i--) {
    const char *nm = c->classes[chain[i]].name;
    if (!nm) continue;
    if (j && j + 1 < cap) out[j++] = '_';
    for (const char *p = nm; *p && j + 1 < cap; p++) out[j++] = *p;
  }
  /* The loop only advances j while j + 1 < cap, so j < cap here. */
  if (cap) out[j] = '\0';
}

/* Class index for a seed `class` line, normalizing `::` to the `_` form used
   in the compiler's class table. -1 if no such user class. */
static int seed_class_index(Compiler *c, const char *name) {
  char buf[256];
  size_t j = 0;
  for (const char *p = name; *p && j < sizeof buf - 1; ) {
    if (p[0] == ':' && p[1] == ':') { buf[j++] = '_'; p += 2; }
    else buf[j++] = *p++;
  }
  buf[j] = '\0';
  int direct = comp_class_index(c, buf);
  if (direct >= 0) return direct;
  /* A class whose leaf name collided across modules was renamed to the
     `<Mod>__..__<Leaf>` form by qualify_colliding_classes; match that too. */
  char buf2[256];
  size_t j2 = 0;
  for (const char *p = name; *p && j2 < sizeof buf2 - 1; ) {
    if (p[0] == ':' && p[1] == ':') { if (j2 + 2 < sizeof buf2) { buf2[j2++] = '_'; buf2[j2++] = '_'; } p += 2; }
    else buf2[j2++] = *p++;
  }
  buf2[j2] = '\0';
  if (!sp_streq(buf, buf2)) { int q2 = comp_class_index(c, buf2); if (q2 >= 0) return q2; }
  /* A module-nested class (`module M; class C`) is stored under its leaf name
     `C` with enclosing_class = M, but the extractor emits the qualified
     `M_C`. Match against each class's reconstructed qualified name so the
     seed's ivar/method types are not silently dropped. */
  for (int i = 0; i < c->nclasses; i++) {
    char qn[256];
    class_qualified_name(c, i, qn, sizeof qn);
    if (sp_streq(qn, buf)) return i;
  }
  return -1;
}

static Scope *find_method_scope(Compiler *c, int class_id, const char *name, int is_cmethod) {
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id != class_id) continue;
    if (!!s->is_cmethod != !!is_cmethod) continue;
    if (s->name && sp_streq(s->name, name)) return s;
  }
  return NULL;
}

int class_ivar_pinned(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->n_rbs_pin_ivars; i++)
    if (sp_streq(ci->rbs_pin_ivars[i], name)) return 1;
  return 0;
}

static void class_pin_ivar(ClassInfo *ci, const char *name) {
  if (class_ivar_pinned(ci, name)) return;
  if (ci->n_rbs_pin_ivars >= ci->c_rbs_pin_ivars) {
    int nc = ci->c_rbs_pin_ivars ? ci->c_rbs_pin_ivars * 2 : 4;
    ci->rbs_pin_ivars = realloc(ci->rbs_pin_ivars, sizeof(char *) * (size_t)nc);
    ci->c_rbs_pin_ivars = nc;
  }
  ci->rbs_pin_ivars[ci->n_rbs_pin_ivars++] = strdup(name);
}

/* Pin scope `s`'s return and each named parameter to its seeded type. ptypes
   is a comma-separated, param-index-aligned list (empty fields preserved so a
   skipped middle param doesn't shift the rest). */
static void seed_method(Compiler *c, Scope *s, const char *ret_tok, char *ptypes) {
  if (!s) return;
  TyKind rt = parse_seed_type(c, ret_tok);
  if (rt != TY_UNKNOWN) { s->ret = rt; s->ret_rbs_seeded = 1; }
  if (!ptypes) return;
  char *p = ptypes;
  int pi = 0;
  while (p && pi < s->nparams) {
    char *comma = strchr(p, ',');
    if (comma) *comma = '\0';
    TyKind pt = parse_seed_type(c, p);
    if (pt != TY_UNKNOWN) {
      LocalVar *lv = scope_local(s, s->pnames[pi]);
      if (lv) { lv->type = pt; lv->rbs_seeded = 1; }
    }
    pi++;
    if (!comma) break;
    p = comma + 1;
  }
}

static void apply_rbs_seeds(Compiler *c, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return;
  int cur_ci = -1;       /* current class index; -2 = top level (Object) */
  char line[2048];
  while (fgets(line, sizeof line, f)) {
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
    if (L == 0) continue;
    /* split into keyword + up to 3 fields (the 3rd holds the rest of line) */
    char *kw = line, *a1 = NULL, *a2 = NULL, *a3 = NULL;
    char *s1 = strchr(line, ' ');
    if (s1) {
      *s1 = '\0'; a1 = s1 + 1;
      char *s2 = strchr(a1, ' ');
      if (s2) {
        *s2 = '\0'; a2 = s2 + 1;
        char *s3 = strchr(a2, ' ');
        if (s3) { *s3 = '\0'; a3 = s3 + 1; }
      }
    }
    if (sp_streq(kw, "class")) {
      if (a1 && sp_streq(a1, "Object")) cur_ci = -2;
      else cur_ci = a1 ? seed_class_index(c, a1) : -1;
    }
    else if (sp_streq(kw, "ivar") && a1 && a2 && cur_ci >= 0) {
      TyKind t = parse_seed_type(c, a2);
      if (t != TY_UNKNOWN) {
        ClassInfo *ci = &c->classes[cur_ci];
        int idx = comp_ivar_intern(ci, a1);
        ci->ivar_types[idx] = t;
        class_pin_ivar(ci, a1);
      }
    }
    else if (sp_streq(kw, "meth") && a1 && a2) {
      int class_id = (cur_ci == -2) ? -1 : cur_ci;
      if (cur_ci == -2 || cur_ci >= 0)
        seed_method(c, find_method_scope(c, class_id, a1, 0), a2, a3);
    }
    else if (sp_streq(kw, "cmeth") && a1 && a2 && cur_ci >= 0) {
      seed_method(c, find_method_scope(c, cur_ci, a1, 1), a2, a3);
    }
  }
  fclose(f);
}

/* Iteration methods whose block binds a parameter to the receiver array's
   element type (the forms infer_block_params re-derives from the receiver).
   A param of such a block can lock to poly when the element type settles only
   late in the fixpoint (e.g. `arr.map{...}.map{...}`: the inner map's receiver
   becomes a typed array only after the outer map narrows). */
static int iter_elem_block_method(const char *n) {
  static const char *names[] = {
    "each","map","collect","select","reject","filter","find","detect",
    "find_all","flat_map","filter_map","reverse_each","each_entry",
    "take_while","drop_while","sort_by","sort_by!","min_by","max_by","group_by",
    "partition","count","sum","any?","all?","none?","one?","keep_if",
    "delete_if","uniq","find_index","each_with_index","reduce","inject", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}

/* Is local `nm` assigned anywhere in `node`'s subtree (a block body)? Stops at
   nested defs/classes and at nested blocks/lambdas that re-bind `nm` (its
   writes there are a different variable). Used to leave a reassigned block
   param locked: its widening contribution is not recoverable by re-derivation
   from the element type alone. */
static int blkp_name_written(Compiler *c, int node, const char *nm) {
  if (node < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  if ((sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode")) && blkp_binds_param(c, node, nm)) return 0;
  if (lv_node_is_write(ty)) {
    const char *n = nt_str(nt, node, "name");
    if (n && sp_streq(n, nm)) return 1;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (blkp_name_written(c, nt_ref_at(nt, node, i), nm)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) if (blkp_name_written(c, ids[k], nm)) return 1; }
  return 0;
}

/* Clear a transient poly lock on iteration-block params over a now-typed
   (non-poly) array, so the optimistic re-narrow can re-derive the element
   type. Only read-only params are reset: a param reassigned in the body has a
   widening contribution the element-type re-derivation cannot recover (the
   re-run would re-narrow it and silently miscompile), so it stays locked.
   Reset to UNKNOWN -- never the element type directly. Returns the count. */
static int reset_locked_iter_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = 0;
  for (int id = 0; id < nt->count; id++) {
    int block = nt_ref(nt, id, "block");
    if (block < 0) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn || !iter_elem_block_method(cn)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) || rt == TY_POLY_ARRAY) continue;
    Scope *bs = comp_scope_of(c, block);
    if (!bs) continue;
    int body = nt_ref(nt, block, "body");
    for (int k = 0; ; k++) {
      const char *pn = block_param_name(c, block, k);
      if (!pn) break;
      LocalVar *lp = scope_local(bs, pn);
      if (lp && lp->type == TY_POLY && !blkp_name_written(c, body, pn)) { lp->type = TY_UNKNOWN; n++; }
    }
  }
  return n;
}

/* Is `id` a `recv.enum_for` / `recv.to_enum` call that materializes the
   receiver's `#each` (no args, or a single literal `:each`), with no block?
   These are the forms we lower to an eager `#each`-into-array helper. */
static int is_enum_for_each(const Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "enum_for") && !sp_streq(nm, "to_enum"))) return 0;
  if (nt_ref(nt, id, "receiver") < 0) return 0;       /* need a receiver to drive */
  if (nt_ref(nt, id, "block") >= 0) return 0;         /* a size block is unsupported */
  int args = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac == 0) return 1;                               /* enum_for defaults to :each */
  if (ac != 1 || !av) return 0;
  if (!nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "SymbolNode")) return 0;
  const char *s = nt_str(nt, av[0], "value");
  return s && sp_streq(s, "each");
}

/* Rewrite every `recv.enum_for(:each)` / `recv.to_enum` into a call to the
   synthesized `recv.__enum_to_a`, which materializes `#each` into an array.
   Purely syntactic (no receiver type needed); the per-class helper is
   synthesized separately and pruned by reachability when unused. */
static void rewrite_enum_for_each(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* snapshot the count: synthesis below appends nodes, which must not be rescanned */
  int count = nt->count;
  for (int id = 0; id < count; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (!is_enum_for_each(c, id)) continue;
    int empty = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, empty, "arguments", NULL, 0);
    nt_node_set_str(nt, id, "name", "__enum_to_a");
    nt_node_set_ref(nt, id, "arguments", empty);
    comp_grow_node_arrays(c);
    c->nscope[empty] = c->nscope[id];
  }
}

/* Synthesize, on every class that defines an instance `#each` that yields, a
   helper method

       def __enum_to_a
         __enum_acc = []
         each { |__enum_e| __enum_acc << __enum_e }
         __enum_acc
       end

   so an `enum_for(:each)` rewritten to `__enum_to_a` materializes the receiver
   eagerly into an array (then map/select/to_a/... use the array path). The
   helper is a normal method whose body is inferred and emitted by the usual
   machinery; when `#each` is force-lowered the inlined `each { }` becomes a
   real call passing the block as a proc. Unused helpers are pruned by
   reachability, so this is inert for programs that never call enum_for/to_enum
   (the self-host compiler included). */
static void synth_enum_to_a(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (c->nscopes == 0) return;
  /* collect target classes first (synthesis below grows c->scopes). At most one
     entry per scope, so pre-size both arrays to nscopes and avoid per-item growth. */
  int *cls = malloc(sizeof(int) * (size_t)c->nscopes);
  int *eachdef = malloc(sizeof(int) * (size_t)c->nscopes);
  if (!cls || !eachdef) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int ncls = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *m = &c->scopes[s];
    if (!m->name || m->is_cmethod || m->class_id < 0) continue;
    if (!sp_streq(m->name, "each") || !m->yields) continue;
    if (comp_method_in_class(c, m->class_id, "__enum_to_a") >= 0) continue;
    int dup = 0;
    for (int k = 0; k < ncls; k++) if (cls[k] == m->class_id) { dup = 1; break; }
    if (dup) continue;
    cls[ncls] = m->class_id; eachdef[ncls] = m->def_node; ncls++;
  }
  for (int k = 0; k < ncls; k++) {
    int arr = nt_new_node(nt, "ArrayNode");
    nt_node_set_arr(nt, arr, "elements", NULL, 0);
    int accw = nt_new_node(nt, "LocalVariableWriteNode");
    nt_node_set_str(nt, accw, "name", "__enum_acc");
    nt_node_set_ref(nt, accw, "value", arr);
    int ep = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, ep, "name", "__enum_e");
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &ep, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int accr1 = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, accr1, "name", "__enum_acc");
    int eread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, eread, "name", "__enum_e");
    int pushargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, pushargs, "arguments", &eread, 1);
    int push = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, push, "name", "<<");
    nt_node_set_ref(nt, push, "receiver", accr1);
    nt_node_set_ref(nt, push, "arguments", pushargs);
    int blkbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, blkbody, "body", &push, 1);
    int blk = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blk, "parameters", bparams);
    nt_node_set_ref(nt, blk, "body", blkbody);
    int eachcall = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, eachcall, "name", "each");
    nt_node_set_ref(nt, eachcall, "block", blk);
    int accr2 = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, accr2, "name", "__enum_acc");
    int body = nt_new_node(nt, "StatementsNode");
    int stmts[3] = { accw, eachcall, accr2 };
    nt_node_set_arr(nt, body, "body", stmts, 3);

    Scope *ms = comp_scope_new(c, "__enum_to_a", eachdef[k]);
    ms->class_id = cls[k];
    ms->body = body;
    int ms_idx = c->nscopes - 1;
    /* register_locals already ran (before synthesis), so intern this scope's
       locals here; types are filled in by the inference fixpoint that follows. */
    scope_local_intern(ms, "__enum_acc");
    LocalVar *ev = scope_local_intern(ms, "__enum_e");
    if (ev) ev->is_block_param = 1;
    comp_grow_node_arrays(c);
    walk_scope(c, body, ms_idx, cls[k]);
  }
  free(cls); free(eachdef);
}

/* Enumerable methods that work on an array receiver in Spinel; a bare call to
   one of these on a user `#each` class (that does not define it) is redirected
   through the materialized array. Kept to methods the array path supports. */
static int is_array_enum_method(const char *nm) {
  static const char *const names[] = {
    "map", "collect", "select", "filter", "reject", "to_a", "entries",
    "find", "detect", "find_index", "count", "sum", "min", "max",
    "include?", "first", "sort", "sort_by", "min_by", "max_by",
    "reduce", "inject", "each_with_index", "flat_map", "collect_concat",
    "any?", "all?", "none?", "one?", "take", "drop", "take_while", "drop_while",
    "filter_map", "partition", "group_by", "each_with_object", "tally",
    "find_all", "zip", "grep", "grep_v", NULL };
  for (int k = 0; names[k]; k++) if (sp_streq(nm, names[k])) return 1;
  return 0;
}

/* Redirect a bare Enumerable call -- `obj.map { }`, `obj.to_a`, ... -- on a
   user class that defines `#each` but not that method, through the synthesized
   `__enum_to_a` helper: `obj.<m>{blk}` becomes `obj.__enum_to_a.<m>{blk}`, so
   the array path handles it without an explicit `enum_for(:each)`. Runs in the
   inference fixpoint (needs the receiver's type, and the inserted node is typed
   on a later iteration). Returns 1 if anything changed. */
int desugar_enum_method_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !is_array_enum_method(nm)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    /* already redirected through __enum_to_a -> leave it (idempotent) */
    if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "__enum_to_a")) continue;
    }
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (comp_method_in_chain(c, cid, "__enum_to_a", NULL) < 0) continue;  /* not an #each class */
    if (comp_method_in_chain(c, cid, nm, NULL) >= 0) continue;            /* class defines it */
    int wrap = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, wrap, "name", "__enum_to_a");
    nt_node_set_ref(nt, wrap, "receiver", recv);
    nt_node_set_ref(nt, id, "receiver", wrap);
    comp_grow_node_arrays(c);
    c->nscope[wrap] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* ===== Post-fixpoint: narrow monomorphic object arrays to TY_OBJ_ARRAY =====
   A POLY_ARRAY local/param whose every element is an instance of one user
   class X, and whose every use is in a small supported op set (index, push,
   length, ...), is narrowed to ty_obj_array(X) -- the runtime sp_PtrArray of
   unboxed sp_X*, dropping the per-element boxing and cls-id dispatch that a
   poly array pays on every `arr[i]` / `arr[i].field`. Interprocedural soundness
   is by an optimistic-then-revoke union-find: a slot flowing (as a positional
   arg, or an alias `b = a`) into another slot shares one C container type, so
   the two are unioned and the whole component narrows together or not at all.
   Strictly conservative: any unmodeled use, class conflict, unresolved flow, or
   absent object evidence kills the component, leaving it TY_POLY_ARRAY. Runs
   ONCE, after the fixpoint, so the new type never feeds forward inference. */
typedef struct { int sidx; LocalVar *lv; int cls; int alive; int uf; int needs_cmp; } OAS;

static int oa_find(OAS *sl, int n, int sidx, LocalVar *lv) {
  for (int i = 0; i < n; i++) if (sl[i].sidx == sidx && sl[i].lv == lv) return i;
  return -1;
}
static int oa_uf_find(OAS *sl, int i) {
  while (sl[i].uf != i) { sl[i].uf = sl[sl[i].uf].uf; i = sl[i].uf; }
  return i;
}
static void oa_uf_union(OAS *sl, int a, int b) {
  int ra = oa_uf_find(sl, a), rb = oa_uf_find(sl, b);
  if (ra != rb) sl[ra].uf = rb;
}
/* the single user-object class of a node's value, or -1 if not a lone object. */
static int oa_obj_class_of(Compiler *c, int node) {
  TyKind t = infer_type(c, node);
  return ty_is_object(t) ? ty_object_class(t) : -1;
}
/* class evidence join: -1 = none seen, -2 = conflicting classes. */
static int oa_cls_join(int a, int b) {
  if (b == -1) return a;
  if (a == -1) return b;
  if (a == b) return a;
  return -2;
}
static int oa_recv_op_ok(const char *nm, int argc, int has_block) {
  if (!nm || has_block) return 0;
  if ((sp_streq(nm, "[]") || sp_streq(nm, "at")) && argc == 1) return 1;
  if (sp_streq(nm, "[]=") && argc == 2) return 1;
  if ((sp_streq(nm, "push") || sp_streq(nm, "<<") || sp_streq(nm, "append")) && argc >= 1) return 1;
  if ((sp_streq(nm, "length") || sp_streq(nm, "size")) && argc == 0) return 1;
  if (sp_streq(nm, "empty?") && argc == 0) return 1;
  if ((sp_streq(nm, "first") || sp_streq(nm, "last")) && argc == 0) return 1;
  /* no-block comparisons: usable when the element class has `<=>` (the
     needs_cmp bit, checked at component resolution). sort's RESULT must
     also land in a modeled place (slot alias or statement position) --
     step 4 kills the component otherwise, so an escaping `arr.sort.map`
     keeps today's boxed poly path instead of becoming a reject. */
  if ((sp_streq(nm, "min") || sp_streq(nm, "max")) && argc == 0) return 1;
  if ((sp_streq(nm, "sort") || sp_streq(nm, "sort!")) && argc == 0) return 1;
  return 0;
}

static void narrow_object_arrays(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* 1. candidate slots: POLY_ARRAY locals/params (skip block params + rbs). */
  int cap = 16, n = 0;
  OAS *sl = (OAS *)malloc(sizeof(OAS) * cap);
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY_ARRAY || lv->is_block_param || lv->rbs_seeded) continue;
      if (n >= cap) { cap *= 2; sl = (OAS *)realloc(sl, sizeof(OAS) * cap); if (!sl) { fprintf(stderr, "oom\n"); exit(1); } }
      sl[n].sidx = s; sl[n].lv = lv; sl[n].cls = -1; sl[n].alive = 1; sl[n].uf = n; sl[n].needs_cmp = 0; n++;
    }
  }
  if (n == 0) { free(sl); return; }
  int nc = nt->count ? nt->count : 1;
  int *read_slot = (int *)malloc(sizeof(int) * nc);
  char *claimed = (char *)calloc(nc, 1);
  /* value_ok[id]: node id sits in statement position (a StatementsNode body
     entry) -- one modeled consumer for a `sort`/`sort!` result. The other
     (slot-alias RHS) is recognized in step 5. */
  char *value_ok = (char *)calloc(nc, 1);
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "StatementsNode")) continue;
    int bn = 0; const int *bl = nt_arr(nt, id, "body", &bn);
    for (int i = 0; i < bn; i++) if (bl[i] >= 0 && bl[i] < nc) value_ok[bl[i]] = 1;
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int sidx = c->nscope[id];
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
    if (!lv || oa_find(sl, n, sidx, lv) < 0) continue;   /* target must be a slot */
    int v = nt_ref(nt, id, "value");
    if (v >= 0 && v < nc) value_ok[v] = 1;   /* alias edge made in step 5 */
  }

  /* 2. map each LocalVariableReadNode of a slot to its slot index. */
  for (int id = 0; id < nt->count; id++) {
    read_slot[id] = -1;
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
    int sidx = c->nscope[id];
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
    if (lv) read_slot[id] = oa_find(sl, n, sidx, lv);
  }

  /* 3. op-assign / multi-target write forms on a slot are unmodeled -> kill. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!sp_streq(ty, "LocalVariableOperatorWriteNode") &&
        !sp_streq(ty, "LocalVariableTargetNode") &&
        !sp_streq(ty, "LocalVariableAndWriteNode") &&
        !sp_streq(ty, "LocalVariableOrWriteNode")) continue;
    int sidx = c->nscope[id];
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
    int si = lv ? oa_find(sl, n, sidx, lv) : -1;
    if (si >= 0) sl[si].alive = 0;
  }

  /* 4. CallNodes: classify slot-as-receiver (supported op + element evidence)
        and slot-as-arg (positional into a resolvable free method -> edge). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    int has_block = nt_ref(nt, id, "block") >= 0;
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (recv >= 0 && read_slot[recv] >= 0) {
      int S = read_slot[recv];
      if (oa_recv_op_ok(name, argc, has_block)) {
        claimed[recv] = 1;
        if (name && (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")))
          for (int a = 0; a < argc; a++) sl[S].cls = oa_cls_join(sl[S].cls, oa_obj_class_of(c, argv[a]));
        else if (name && sp_streq(name, "[]=") && argc == 2) {
          /* a range-keyed []= is a splice, which the obj-array representation
             has no emitter for: keep the slot on the poly path */
          if (infer_type(c, argv[0]) == TY_RANGE) sl[S].alive = 0;
          else sl[S].cls = oa_cls_join(sl[S].cls, oa_obj_class_of(c, argv[1]));
        }
        else if (name && (sp_streq(name, "min") || sp_streq(name, "max") ||
                          sp_streq(name, "sort") || sp_streq(name, "sort!"))) {
          sl[S].needs_cmp = 1;
          /* a sort/sort! RESULT must land in a modeled consumer (statement
             position, or a slot-alias write handled in step 5); an escaping
             result keeps the component on the boxed poly path. */
          if ((sp_streq(name, "sort") || sp_streq(name, "sort!")) &&
              !(id < nc && value_ok[id]))
            sl[S].alive = 0;
        }
      }
      else sl[S].alive = 0;
    }
    for (int k = 0; argv && k < argc; k++) {
      int a = argv[k];
      if (read_slot[a] < 0) continue;
      int S = read_slot[a];
      int mi = (recv < 0 && name) ? comp_method_index(c, name) : -1;
      if (mi < 0) { sl[S].alive = 0; continue; }
      Scope *M = &c->scopes[mi];
      if (k >= M->nparams || (M->rest_idx >= 0 && k >= M->rest_idx)) { sl[S].alive = 0; continue; }
      LocalVar *plv = M->pnames[k] ? scope_local(M, M->pnames[k]) : NULL;
      int T = plv ? oa_find(sl, n, mi, plv) : -1;
      if (T < 0) { sl[S].alive = 0; continue; }
      claimed[a] = 1;
      oa_uf_union(sl, S, T);
    }
  }

  /* 5. LocalVariableWriteNode sources: object literal -> evidence; alias to
        another slot -> edge; empty/nil -> neutral; anything else -> kill. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int sidx = c->nscope[id];
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
    int S = lv ? oa_find(sl, n, sidx, lv) : -1;
    if (S < 0) continue;
    int v = nt_ref(nt, id, "value");
    const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
    if (!vty) { sl[S].alive = 0; continue; }
    if (sp_streq(vty, "ArrayNode")) {
      int en = 0; const int *el = nt_arr(nt, v, "elements", &en);
      for (int e = 0; e < en; e++) {
        const char *ety = nt_type(nt, el[e]);
        int ec = (ety && sp_streq(ety, "SplatNode")) ? -2 : oa_obj_class_of(c, el[e]);
        if (ec < 0) { sl[S].alive = 0; break; }
        sl[S].cls = oa_cls_join(sl[S].cls, ec);
      }
    }
    else if (sp_streq(vty, "LocalVariableReadNode") && read_slot[v] >= 0) {
      claimed[v] = 1; oa_uf_union(sl, S, read_slot[v]);
    }
    else if (sp_streq(vty, "NilNode")) { /* nullable, neutral */ }
    else if (sp_streq(vty, "CallNode")) {
      /* `b = arr.sort` / `b = arr.sort!`: the sorted array shares arr's
         element class -- an alias edge, like a plain slot-to-slot copy. */
      const char *cn = nt_str(nt, v, "name");
      int crecv = nt_ref(nt, v, "receiver");
      int cargs = nt_ref(nt, v, "arguments");
      int can = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &can);
      if (cn && (sp_streq(cn, "sort") || sp_streq(cn, "sort!")) && can == 0 &&
          nt_ref(nt, v, "block") < 0 && crecv >= 0 && read_slot[crecv] >= 0) {
        claimed[crecv] = 1;
        oa_uf_union(sl, S, read_slot[crecv]);
      }
      else sl[S].alive = 0;
    }
    else sl[S].alive = 0;
  }

  /* 6. a slot read left unclaimed escaped into an unmodeled context -> kill. */
  for (int id = 0; id < nt->count; id++)
    if (read_slot[id] >= 0 && !claimed[id]) sl[read_slot[id]].alive = 0;

  /* 7. resolve components: roll member alive/cls up to the root, then a
        component narrows only if every member is alive and one class is known. */
  for (int i = 0; i < n; i++) {
    int r = oa_uf_find(sl, i);
    if (r == i) continue;
    sl[r].cls = oa_cls_join(sl[r].cls, sl[i].cls);
    if (sl[i].needs_cmp) sl[r].needs_cmp = 1;
    if (!sl[i].alive) sl[r].alive = 0;
  }
  for (int i = 0; i < n; i++) {
    int r = oa_uf_find(sl, i);
    if (!sl[r].alive || sl[r].cls < 0) continue;
    /* a component using no-block sort/min/max narrows only when the element
       class can actually compare (has `<=>` in its chain); otherwise it stays
       poly, where the boxed comparator raises the CRuby ArgumentError. */
    if (sl[r].needs_cmp && comp_method_in_chain(c, sl[r].cls, "<=>", NULL) < 0) continue;
    sl[i].lv->type = ty_obj_array(sl[r].cls);
  }

  /* 8. dependent locals: `b = arr[i]` (arr now a narrowed obj array) makes `b`
        the element object type, so its field accesses unbox. Every write of the
        local must be such an index read of one class (nil writes excepted). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
      int cls = -1, ok = 1, saw = 0;
      for (int id = 0; id < nt->count && ok; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "LocalVariableWriteNode") || c->nscope[id] != s) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm || !sp_streq(nm, lv->name)) continue;
        int v = nt_ref(nt, id, "value");
        const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
        if (vty && sp_streq(vty, "NilNode")) continue;
        if (!vty || !sp_streq(vty, "CallNode")) { ok = 0; break; }
        const char *cn = nt_str(nt, v, "name");
        int crecv = nt_ref(nt, v, "receiver");
        int can = 0; { int ca = nt_ref(nt, v, "arguments"); if (ca >= 0) nt_arr(nt, ca, "arguments", &can); }
        int idx_op = cn && (sp_streq(cn, "[]") || sp_streq(cn, "at")) && can == 1;
        int end_op = cn && (sp_streq(cn, "first") || sp_streq(cn, "last")) && can == 0;
        if ((!idx_op && !end_op) || crecv < 0) { ok = 0; break; }
        TyKind rt = infer_type(c, crecv);
        if (!ty_is_obj_array(rt)) { ok = 0; break; }
        int ec = ty_obj_array_class(rt);
        if (cls < 0) cls = ec; else if (cls != ec) { ok = 0; break; }
        saw = 1;
      }
      if (ok && saw && cls >= 0) lv->type = ty_object(cls);
    }
  }

  free(sl); free(read_slot); free(claimed); free(value_ok);
}

/* ===== Post-fixpoint: narrow a poly local that is only ever used as an int =====
   A method-local typed TY_POLY whose every read feeds an int context (an
   arithmetic/comparison operator, or an array/hash index) and whose every write
   is an int or a poly value is narrowed to TY_INT. The poly writes are then
   coerced by emit_assign's existing int-slot-poly-rhs path (sp_poly_to_i), and
   the arithmetic emits natively instead of through the boxed sp_poly_* helpers.
   This removes per-iteration boxing from hot loops where a value picks up the
   poly type from a single poly source (e.g. optcarrot's render_pixel `pixel`,
   which is `sprite[2]` -- a poly-array element -- on one branch but is only ever
   used `% 4`, `==`, and as `@output_color[pixel]`). Requiring at least one index
   use proves the value is genuinely an integer, so coercing a poly source is
   sound. Strictly conservative: any non-int use, op-assign, or non-int/poly
   write source leaves it TY_POLY. */
/* Arithmetic/bitwise operators only -- NOT comparisons (`==`/`<`/`<=>`...),
   which are polymorphic (a string compares with `==` too), so a comparison
   operand is not evidence the value is an int. These arithmetic ops are also
   overloaded on strings/arrays, but the required array-index use proves the
   value is genuinely an integer, so an int reading of these is then sound. */
static int npi_is_int_op(const char *nm) {
  if (!nm) return 0;
  static const char *const ops[] = {
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "**", NULL };
  for (int i = 0; ops[i]; i++) if (sp_streq(nm, ops[i])) return 1;
  return 0;
}
static void narrow_poly_int_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  int nc = nt->count ? nt->count : 1;
  char *is_read = (char *)malloc(nc);
  char *claimed = (char *)malloc(nc);
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY) continue;
      if (lv->is_param || lv->is_block_param || lv->is_cell || lv->rbs_seeded) continue;
      const char *nm = lv->name;
      int ok = 1, saw_write = 0;

      /* 1. writes: plain LocalVariableWriteNode with an int/poly value; any
            op/and/or-write or multi-target leaves it poly. */
      for (int id = 0; id < nt->count && ok; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || c->nscope[id] != s) continue;
        if (sp_streq(ty, "LocalVariableOperatorWriteNode") ||
            sp_streq(ty, "LocalVariableAndWriteNode") ||
            sp_streq(ty, "LocalVariableOrWriteNode") ||
            sp_streq(ty, "LocalVariableTargetNode")) {
          const char *wn = nt_str(nt, id, "name");
          if (wn && sp_streq(wn, nm)) ok = 0;
        }
        else if (sp_streq(ty, "LocalVariableWriteNode")) {
          const char *wn = nt_str(nt, id, "name");
          if (!wn || !sp_streq(wn, nm)) continue;
          saw_write = 1;
          int v = nt_ref(nt, id, "value");
          TyKind vt = v >= 0 ? infer_type(c, v) : TY_UNKNOWN;
          if (vt != TY_INT && vt != TY_POLY) ok = 0;
        }
      }
      if (!ok || !saw_write) continue;

      /* 2. map this local's reads, then claim those in an int context. */
      for (int id = 0; id < nt->count; id++) {
        is_read[id] = 0; claimed[id] = 0;
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "LocalVariableReadNode") || c->nscope[id] != s) continue;
        const char *rn = nt_str(nt, id, "name");
        if (rn && sp_streq(rn, nm)) is_read[id] = 1;
      }
      int has_index = 0;
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty) continue;
        if (sp_streq(ty, "CallNode")) {
          const char *cn = nt_str(nt, id, "name");
          int recv = nt_ref(nt, id, "receiver");
          int args = nt_ref(nt, id, "arguments"); int an = 0;
          const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
          int is_idx = cn && (sp_streq(cn, "[]") || sp_streq(cn, "at") || sp_streq(cn, "[]="));
          if (npi_is_int_op(cn)) {
            if (recv >= 0 && is_read[recv]) claimed[recv] = 1;
            for (int k = 0; k < an; k++) if (is_read[av[k]]) claimed[av[k]] = 1;
          }
          else if (is_idx && an >= 1 && is_read[av[0]]) {
            /* the index is arg 0, and only an ARRAY index proves an int (a hash
               key can be any type). A `[]=` value (arg 1) is not an index. */
            TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
            if (ty_is_array(rt) || ty_is_obj_array(rt)) { claimed[av[0]] = 1; has_index = 1; }
          }
        }
        else if (sp_streq(ty, "IndexNode")) {
          int recv = nt_ref(nt, id, "receiver");
          TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
          if (ty_is_array(rt) || ty_is_obj_array(rt)) {
            int args = nt_ref(nt, id, "arguments"); int an = 0;
            const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            for (int k = 0; k < an; k++) if (is_read[av[k]]) { claimed[av[k]] = 1; has_index = 1; }
          }
        }
      }
      /* every read must be claimed, and at least one must be an index (which
         proves the value is an integer, making a poly-source coercion sound). */
      for (int id = 0; id < nt->count && ok; id++)
        if (is_read[id] && !claimed[id]) ok = 0;
      if (ok && has_index) lv->type = TY_INT;
    }
  }
  free(is_read); free(claimed);
}

/* ---- `return <expr> if p.nil?` guard narrowing (#1661) --------------------
   A method whose call sites pass `T | nil` types its parameter poly (T has no
   first-class nullable slot: String, Time, ...). When the body OPENS with
   early-return nil guards on that parameter and never reassigns it, every
   read after the guard is provably non-nil: mark those read nodes with the
   non-nil type in c->nilnarrow. infer_type returns the narrowed type for the
   marked reads and codegen unboxes the poly slot at each read site, so the
   rest of the body (and the method's return) type as T. */

static void nng_mark_reads(Compiler *c, int root, Scope *s, const char *pn, TyKind t) {
  if (root < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, root, "name");
    /* only reads owned by the method scope itself: a block-interior read goes
       through capture plumbing and keeps the poly slot (sound, just unnarrowed) */
    if (nm && sp_streq(nm, pn) && comp_scope_of(c, root) == s)
      c->nilnarrow[root] = t;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) nng_mark_reads(c, nt_ref_at(nt, root, i), s, pn, t);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *a = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) nng_mark_reads(c, a[j], s, pn, t);
  }
}

/* Any write/target of `pn` anywhere in the method disables narrowing (it
   could re-store nil, or another type, after the guard). */
static int nng_has_write(Compiler *c, int root, const char *pn) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableTargetNode") ||
             sp_streq(ty, "LocalVariableOperatorWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
             sp_streq(ty, "LocalVariableAndWriteNode"))) {
    const char *nm = nt_str(nt, root, "name");
    if (nm && sp_streq(nm, pn)) return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (nng_has_write(c, nt_ref_at(nt, root, i), pn)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *a = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) if (nng_has_write(c, a[j], pn)) return 1;
  }
  return 0;
}

/* `return <expr> if p.nil?` (modifier or block form, no else) on a poly
   param of scope `s`: returns the param name, else NULL. */
static const char *nng_guard_param(Compiler *c, Scope *s, int st) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, st);
  if (!ty || !sp_streq(ty, "IfNode")) return NULL;
  if (nt_ref(nt, st, "subsequent") >= 0) return NULL;
  int pred = nt_ref(nt, st, "predicate");
  if (pred < 0 || !nt_type(nt, pred) || !sp_streq(nt_type(nt, pred), "CallNode")) return NULL;
  const char *cn = nt_str(nt, pred, "name");
  if (!cn || !sp_streq(cn, "nil?")) return NULL;
  if (nt_ref(nt, pred, "arguments") >= 0) return NULL;
  int recv = nt_ref(nt, pred, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return NULL;
  const char *pn = nt_str(nt, recv, "name");
  if (!pn) return NULL;
  LocalVar *lv = scope_local(s, pn);
  if (!lv || !lv->is_param || lv->is_block_param || lv->type != TY_POLY) return NULL;
  int thb = nt_ref(nt, st, "statements");
  int n = 0;
  const int *a = thb >= 0 ? nt_arr(nt, thb, "body", &n) : NULL;
  if (!a || n != 1) return NULL;
  const char *rt2 = nt_type(nt, a[0]);
  if (!rt2 || !sp_streq(rt2, "ReturnNode")) return NULL;
  return pn;
}

/* ---- caller-side narrowing of a nil-guarded poly LOCAL (#1675) ------------
   `a = m()` where m's value is `nil | Time` types `a` poly (Time has no
   first-class nullable slot). When a branch is guarded by `a.nil?` or `a`
   truthiness and the local is never reassigned, reads inside the non-nil arm
   are provably Time: mark them in c->nilnarrow, the same read-site unbox
   #1661 uses for parameters -- no global type mutates, so nothing cascades. */

/* returns-walk: every explicit return owned by scope mi is either nil or the
   single base type; blocks' returns belong to their block scope and are NOT
   collected, which can only lose a nil witness and disable the narrowing. */
static void spnb_ret_walk(Compiler *c, int root, Scope *s, TyKind *base, int *saw_nil, int *ok) {
  if (root < 0 || !*ok) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "ReturnNode") && comp_scope_of(c, root) == s) {
    int a = nt_ref(nt, root, "arguments");
    int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0 || (nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "NilNode"))) {
      *saw_nil = 1;
    }
    else {
      TyKind t = infer_type(c, av[0]);
      if (t == TY_TIME && (*base == TY_UNKNOWN || *base == TY_TIME)) *base = TY_TIME;
      else { *ok = 0; return; }
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) spnb_ret_walk(c, nt_ref_at(nt, root, i), s, base, saw_nil, ok);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *arr = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) spnb_ret_walk(c, arr[j], s, base, saw_nil, ok);
  }
}

/* tail-value contribution: the body's falling-off value */
static void spnb_tail(Compiler *c, int node, TyKind *base, int *saw_nil, int *ok) {
  if (node < 0 || !*ok) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) { *ok = 0; return; }
  if (sp_streq(ty, "StatementsNode")) {
    int n = 0;
    const int *bb = nt_arr(nt, node, "body", &n);
    if (n == 0) { *saw_nil = 1; return; }
    spnb_tail(c, bb[n - 1], base, saw_nil, ok);
    return;
  }
  if (sp_streq(ty, "ReturnNode")) return;   /* counted by the returns walk */
  if (sp_streq(ty, "NilNode")) { *saw_nil = 1; return; }
  if (sp_streq(ty, "IfNode")) {
    spnb_tail(c, nt_ref(nt, node, "statements"), base, saw_nil, ok);
    int sub = nt_ref(nt, node, "subsequent");
    if (sub >= 0) spnb_tail(c, sub, base, saw_nil, ok);
    else *saw_nil = 1;                       /* if-without-else can fall nil */
    return;
  }
  if (sp_streq(ty, "ElseNode")) {
    spnb_tail(c, nt_ref(nt, node, "statements"), base, saw_nil, ok);
    return;
  }
  if (sp_streq(ty, "UnlessNode")) {
    spnb_tail(c, nt_ref(nt, node, "statements"), base, saw_nil, ok);
    int sub = nt_ref(nt, node, "else_clause");
    if (sub >= 0) spnb_tail(c, sub, base, saw_nil, ok);
    else *saw_nil = 1;
    return;
  }
  {
    TyKind t = infer_type(c, node);
    if (t == TY_TIME && (*base == TY_UNKNOWN || *base == TY_TIME)) { *base = TY_TIME; return; }
  }
  *ok = 0;
}

/* the single non-nil type scope mi's poly return decomposes to, or UNKNOWN */
static TyKind scope_poly_nil_base(Compiler *c, int mi) {
  Scope *s = &c->scopes[mi];
  if (s->ret != TY_POLY || s->body < 0 || s->cs_synth || s->is_lowered_yield) return TY_UNKNOWN;
  TyKind base = TY_UNKNOWN;
  int saw_nil = 0, ok = 1;
  spnb_ret_walk(c, s->body, s, &base, &saw_nil, &ok);
  spnb_tail(c, s->body, &base, &saw_nil, &ok);
  return (ok && saw_nil && base == TY_TIME) ? base : TY_UNKNOWN;
}

/* predicate shape: `A.nil?` (returns 1) or bare `A` truthiness (returns 2) */
static int spnb_pred_shape(Compiler *c, int pred, Scope *s, const char *nm) {
  const NodeTable *nt = c->nt;
  const char *ty = pred >= 0 ? nt_type(nt, pred) : NULL;
  if (!ty) return 0;
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *pn = nt_str(nt, pred, "name");
    return pn && sp_streq(pn, nm) && comp_scope_of(c, pred) == s ? 2 : 0;
  }
  if (sp_streq(ty, "CallNode")) {
    const char *cn = nt_str(nt, pred, "name");
    if (!cn || !sp_streq(cn, "nil?")) return 0;
    int recv = nt_ref(nt, pred, "receiver");
    const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
    if (!rty || !sp_streq(rty, "LocalVariableReadNode")) return 0;
    const char *pn = nt_str(nt, recv, "name");
    return pn && sp_streq(pn, nm) && comp_scope_of(c, recv) == s ? 1 : 0;
  }
  return 0;
}

static void spnb_mark_guards(Compiler *c, int root, Scope *s, const char *nm, TyKind base) {
  if (root < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && (sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode"))) {
    int is_unless = sp_streq(ty, "UnlessNode");
    int shape = spnb_pred_shape(c, nt_ref(nt, root, "predicate"), s, nm);
    int branch = -1;
    if (shape == 1)       /* A.nil?  -> the falsy arm is non-nil */
      branch = nt_ref(nt, root, is_unless ? "statements" : "subsequent");
    else if (shape == 2)  /* bare A  -> the truthy arm is non-nil */
      branch = nt_ref(nt, root, is_unless ? "else_clause" : "statements");
    if (branch >= 0 && !nng_has_write(c, branch, nm))
      nng_mark_reads(c, branch, s, nm, base);
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) spnb_mark_guards(c, nt_ref_at(nt, root, i), s, nm, base);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *arr = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) spnb_mark_guards(c, arr[j], s, nm, base);
  }
}

static void narrow_nil_guard_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    Scope *s = comp_scope_of(c, id);
    if (!s) continue;
    const char *nm = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (!nm || val < 0 || nt_kind(nt, val) != NK_CallNode) continue;
    LocalVar *lv = scope_local(s, nm);
    if (!lv || lv->type != TY_POLY || lv->is_param || lv->is_block_param || lv->is_cell) continue;
    int mi = backprop_call_target(c, val);
    if (mi < 0) continue;
    TyKind base = scope_poly_nil_base(c, mi);
    if (base == TY_UNKNOWN) continue;
    /* the assignment above must be the local's ONLY write in its scope:
       nng_has_write counts every write form, so probe the scope body with
       this node's own write discounted by name-match count */
    int writes = 0;
    {
      NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w2) {
        const char *n2 = nt_str(nt, w2, "name");
        if (n2 && sp_streq(n2, nm) && comp_scope_of(c, w2) == s) writes++;
      }
      NT_FOREACH_KIND(nt, NK_LocalVariableTargetNode, w3) {
        const char *n3 = nt_str(nt, w3, "name");
        if (n3 && sp_streq(n3, nm) && comp_scope_of(c, w3) == s) writes += 2;  /* massign etc: bail */
      }
      NT_FOREACH_KIND(nt, NK_LocalVariableOperatorWriteNode, w4) {
        const char *n4 = nt_str(nt, w4, "name");
        if (n4 && sp_streq(n4, nm) && comp_scope_of(c, w4) == s) writes += 2;
      }
      NT_FOREACH_KIND(nt, NK_LocalVariableOrWriteNode, w5) {
        const char *n5 = nt_str(nt, w5, "name");
        if (n5 && sp_streq(n5, nm) && comp_scope_of(c, w5) == s) writes += 2;
      }
      NT_FOREACH_KIND(nt, NK_LocalVariableAndWriteNode, w6) {
        const char *n6 = nt_str(nt, w6, "name");
        if (n6 && sp_streq(n6, nm) && comp_scope_of(c, w6) == s) writes += 2;
      }
    }
    if (writes != 1) continue;
    int body = s->body >= 0 ? s->body : (s->def_node >= 0 ? nt_ref(nt, s->def_node, "body") : -1);
    if (body < 0) continue;
    spnb_mark_guards(c, body, s, nm, base);
  }
}

static void narrow_nil_guard_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->def_node < 0 || s->nparams <= 0 || !s->name) continue;
    /* one definition of this name program-wide: the argument collection below
       is name-matched (like param inference itself), so a same-named sibling
       method would blend unrelated call sites */
    int dup = 0;
    for (int sj = 0; sj < c->nscopes && !dup; sj++)
      if (sj != si && c->scopes[sj].def_node >= 0 && c->scopes[sj].name &&
          sp_streq(c->scopes[sj].name, s->name)) dup = 1;
    if (dup) continue;
    int body = nt_ref(nt, s->def_node, "body");
    int bn = 0;
    const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (!bb || bn < 2) continue;
    for (int g = 0; g < bn - 1; g++) {
      const char *pn = nng_guard_param(c, s, bb[g]);
      if (!pn) break;               /* leading guards only */
      int k = -1;
      for (int i = 0; i < s->nparams; i++) if (sp_streq(s->pnames[i], pn)) { k = i; break; }
      if (k < 0 || (s->rest_idx >= 0 && k >= s->rest_idx)) continue;
      if (nng_has_write(c, body, pn)) continue;
      /* non-nil unify over every name-matched plain call's argument k; any
         caller shape this cannot see through (super, splat, kwargs, missing
         positional) bails */
      TyKind t = TY_UNKNOWN;
      int ok = 1;
      for (int id = 0; id < nt->count && ok; id++) {
        const char *ty2 = nt_type(nt, id);
        if (!ty2) continue;
        if (sp_streq(ty2, "SuperNode") || sp_streq(ty2, "ForwardingSuperNode")) {
          Scope *cs = comp_scope_of(c, id);
          if (cs && cs->name && sp_streq(cs->name, s->name)) ok = 0;
          continue;
        }
        if (!sp_streq(ty2, "CallNode")) continue;
        const char *cn2 = nt_str(nt, id, "name");
        if (!cn2 || !sp_streq(cn2, s->name)) continue;
        int args = nt_ref(nt, id, "arguments");
        int an = 0;
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        if (!av || an <= k) { ok = 0; break; }
        const char *aty = nt_type(nt, av[k]);
        if (aty && (sp_streq(aty, "SplatNode") || sp_streq(aty, "KeywordHashNode"))) { ok = 0; break; }
        if (aty && sp_streq(aty, "NilNode")) continue;
        TyKind at = infer_type(c, av[k]);
        if (at == TY_NIL) continue;
        if (at == TY_UNKNOWN || at == TY_POLY || at == TY_VOID) { ok = 0; break; }
        t = (t == TY_UNKNOWN) ? at : ty_unify(t, at);
      }
      if (!ok || t == TY_UNKNOWN || t == TY_POLY || t == TY_NIL) continue;
      /* the read-site unbox must be expressible for T */
      if (!(t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_SYMBOL ||
            t == TY_STRING || t == TY_TIME || ty_is_object(t) ||
            ty_is_array(t) || ty_is_hash(t))) continue;
      for (int j = g + 1; j < bn; j++) nng_mark_reads(c, bb[j], s, pn, t);
    }
  }
}

void analyze_program(Compiler *c) {
  comp_scope_index_set_frozen(0);  /* scope shape changes during the passes below */
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  rename_shadowing_block_params(c);
  qualify_colliding_consts(c);
  qualify_colliding_classes(c);
  walk_scope(c, c->nt->root_id, 0, -1);
  register_structs(c);
  fix_struct_block_scopes(c);
  register_module_functions(c);
  register_locals(c);
  register_attrs(c);
  register_method_visibility(c);
  register_aliases(c);
  register_undefs(c);
  register_globals_consts(c);
  rewrite_const_alias_receivers(c);
  register_ffi_decls(c);
  topup_forwarding_arity(c);

  /* rescue variables (`rescue => e`) are typed as exception objects. When the
     arm names exactly one user exception subclass that carries ivars, type the
     binding as that object instead so `e.<ivar>` reads resolve and the carried
     object's fields are reachable (#1415); otherwise plain TY_EXCEPTION.
     A name reused across rescue arms (`rescue A => e` ... `rescue B => e`)
     interns to one LocalVar, so it may only specialize when every arm binding
     it agrees on the same class -- otherwise the slot would collapse onto one
     of the types and mis-read the others. */
  /* Collect the rescue arms that bind a local (`rescue X => e`) once; the
     unanimity check then compares arms against this small list instead of
     rescanning the whole node table per arm (was O(rescues * nodes)). */
  {
    int cap = 0, rn = 0;
    struct { int id; const char *nm; Scope *vsc; int spec; } *arms = NULL;
    for (int id = 0; id < c->nt->count; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || !sp_streq(ty, "RescueNode")) continue;
      int ref = nt_ref(c->nt, id, "reference");
      if (ref < 0 || !nt_type(c->nt, ref) || !sp_streq(nt_type(c->nt, ref), "LocalVariableTargetNode")) continue;
      const char *nm = nt_str(c->nt, ref, "name");
      if (!nm) continue;
      Scope *vsc = comp_scope_of(c, ref);
      scope_local_intern(vsc, nm);   /* ensure the LocalVar exists for every arm first */
      if (rn >= cap) { cap = cap ? cap * 2 : 16; arms = realloc(arms, sizeof(*arms) * (size_t)cap); }
      arms[rn].id = id; arms[rn].nm = nm; arms[rn].vsc = vsc;
      arms[rn].spec = rescue_arm_spec_cid(c, id);
      rn++;
    }
    for (int i = 0; i < rn; i++) {
      /* unanimity across every same-name rescue arm in the same scope */
      int unanimous = arms[i].spec;
      for (int j = 0; j < rn && unanimous >= 0; j++) {
        if (j == i || arms[j].vsc != arms[i].vsc || !sp_streq(arms[j].nm, arms[i].nm)) continue;
        if (arms[j].spec != arms[i].spec) unanimous = -1;
      }
      LocalVar *lv = scope_local_intern(arms[i].vsc, arms[i].nm);
      lv->type = unanimous >= 0 ? ty_object(unanimous) : TY_EXCEPTION;
      lv->is_block_param = 1;  /* set externally; don't reset in the fixpoint */
    }
    free(arms);
  }

  resolve_parents(c);
  inherit_members(c);
  register_includes(c);
  register_extends(c);
  register_prepends(c);
  specialize_inherited_cls_new(c);

  /* collect top-level `include <Mod>` calls so bare method calls can
     resolve to module_function methods in those modules. */
  {
    const NodeTable *nt = c->nt;
    int root_stmts = nt_ref(nt, nt->root_id, "statements");
    int sn = 0;
    const int *stmts = root_stmts >= 0 ? nt_arr(nt, root_stmts, "body", &sn) : NULL;
    for (int i = 0; i < sn; i++) {
      if (!nt_type(nt, stmts[i]) || !sp_streq(nt_type(nt, stmts[i]), "CallNode")) continue;
      if (!nt_str(nt, stmts[i], "name") || !sp_streq(nt_str(nt, stmts[i], "name"), "include")) continue;
      if (nt_ref(nt, stmts[i], "receiver") >= 0) continue;
      int anode = nt_ref(nt, stmts[i], "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && sp_streq(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && sp_streq(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int ci = mname ? comp_class_index(c, mname) : -1;
        if (ci < 0) continue;
        c->toplevel_includes = realloc(c->toplevel_includes,
                                       sizeof(int) * (size_t)(c->ntoplevel_includes + 1));
        c->toplevel_includes[c->ntoplevel_includes++] = ci;
      }
    }
  }

  /* mark block-aware methods (contain yield or block_given?) -- these are
     inlined at every call site so block_given? reflects the actual site */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "YieldNode")) comp_scope_of(c, id)->yields = 1;
    else if (sp_streq(ty, "CallNode")) {
      int r = nt_ref(c->nt, id, "receiver");
      const char *rty = r >= 0 ? nt_type(c->nt, r) : NULL;
      int self_or_none = r < 0 || (rty && sp_streq(rty, "SelfNode"));
      const char *nm = nt_str(c->nt, id, "name");
      if (self_or_none && nm && sp_streq(nm, "block_given?")) comp_scope_of(c, id)->yields = 1;
    }
  }

  /* Eager Enumerable-via-#each: rewrite `enum_for(:each)`/`to_enum` to a
     synthesized per-class `__enum_to_a` helper that materializes #each into an
     array. Done before the inference fixpoint so the helper's body is typed,
     and before reachability so the helper is kept only when actually called. */
  rewrite_enum_for_each(c);
  synth_enum_to_a(c);

  /* `&block` + block.call: a method whose block parameter never escapes
     (every read is a `.call` receiver or a `&block` forward) is inlined at
     its call sites exactly like a yielding method. The block-param slot is
     then virtual -- the literal block flows in like an implicit yield. */
  /* Reverse flags built once: blk_call_recv[id] -- a `.call` CallNode has
     receiver == id; blk_arg_expr[id] -- a `&arg` forwards id. They answer the
     per-read "approved use?" test below in O(1) instead of an inner whole-table
     scan (which made the escape analysis O(methods * reads * nodes)). */
  char *blk_call_recv = (char *)calloc((size_t)c->nt->count, 1);
  char *blk_arg_expr = (char *)calloc((size_t)c->nt->count, 1);
  for (int p = 0; (blk_call_recv && blk_arg_expr) && p < c->nt->count; p++) {
    const char *pty = nt_type(c->nt, p);
    if (!pty) continue;
    if (sp_streq(pty, "CallNode")) {
      const char *cn = nt_str(c->nt, p, "name");
      if (cn && sp_streq(cn, "call")) {
        int r = nt_ref(c->nt, p, "receiver");
        if (r >= 0 && r < c->nt->count) blk_call_recv[r] = 1;
      }
    }
    else if (sp_streq(pty, "BlockArgumentNode")) {
      int e = nt_ref(c->nt, p, "expression");
      if (e >= 0 && e < c->nt->count) blk_arg_expr[e] = 1;
    }
  }
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->blk_param) continue;
    /* instance_eval/exec trampolines are inlined at call sites by their own
       dedicated splice; don't treat the &block forward as a yield here. */
    if (m->class_id >= 0 && !m->is_cmethod && m->name &&
        comp_trampoline_kind(c, m->class_id, m->name, NULL)) continue;
    /* Anonymous `&`: nameless, so it can only be forwarded -- always safe
       to inline (there is no escaping read to worry about). */
    if (!m->blk_param[0]) { m->yields = 1; continue; }
    /* Mark nodes inside proc/lambda bodies nested within this method.
       A blk_param read there is a real capture-escape: the proc runs
       independently and needs blk to live in a heap cell. */
    char *inproc_m = (char *)calloc((size_t)c->nt->count, 1);
    if (inproc_m) {
      for (int id = 0; id < c->nt->count; id++) {
        if (!is_proc_create(c, id)) continue;
        if (comp_scope_of(c, id) != m) continue;
        int body = a_proc_body(c, id);
        if (body >= 0) a_mark_subtree(c, body, inproc_m);
      }
    }
    int escapes = 0, uses = 0;
    for (int id = 0; id < c->nt->count && !escapes; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
      if (comp_scope_of(c, id) != m) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || !sp_streq(nm, m->blk_param)) continue;
      /* A read inside a nested proc body is a capture-escape: the proc
         holds a reference to blk independently of the call site. */
      if (inproc_m && inproc_m[id]) { escapes = 1; break; }
      uses++;
      /* approved: receiver of a `.call`, or expression of a `&block` arg */
      int ok = (blk_call_recv && blk_call_recv[id]) || (blk_arg_expr && blk_arg_expr[id]);
      if (!ok) escapes = 1;
    }
    free(inproc_m);
    if (!escapes && uses > 0) {
      /* Don't mark yields=1 if the method has an explicit return: emit_inlined_call
         would reject inlining anyway (scope_has_return), but the method would then
         be skipped in emission because yields=1 -- causing undefined references. */
      int has_ret = 0;
      for (int id2 = 0; id2 < c->nt->count && !has_ret; id2++) {
        const char *ty2 = nt_type(c->nt, id2);
        if (ty2 && sp_streq(ty2, "ReturnNode") && comp_scope_of(c, id2) == m) has_ret = 1;
      }
      if (!has_ret) m->yields = 1;
    }
  }
  free(blk_call_recv);
  free(blk_arg_expr);

  /* intern every symbol literal so codegen can emit the id table */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && sp_streq(ty, "SymbolNode")) {
      const char *v = nt_str(c->nt, id, "value");
      if (v) comp_sym_intern(c, v);
    }
    /* __method__ / __callee__ yield the enclosing method's name as a symbol;
       intern it now so the id table is sized before the codegen prologue */
    else if (ty && sp_streq(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) {
      const char *nm = nt_str(c->nt, id, "name");
      if (nm && (sp_streq(nm, "__method__") || sp_streq(nm, "__callee__"))) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->name && s->name[0]) comp_sym_intern(c, s->name);
      }
    }
  }
  /* Proc#parameters reports param kinds (:req/:opt) and names as symbols;
     intern them now so they land in the table before the codegen prologue. */
  for (int id = 0; id < c->nt->count; id++) {
    if (!is_proc_create(c, id)) continue;
    comp_sym_intern(c, "req");
    comp_sym_intern(c, "opt");
    int pn = a_proc_params_node(c, id);
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
    for (int k = 0; k < rn; k++) { const char *nm = nt_str(c->nt, reqs[k], "name"); if (nm) comp_sym_intern(c, nm); }
  }

  /* Apply --rbs advisory seeds (pin param/return/ivar types) before the
     fixpoint so the inference passes observe the pinned types from round one.
     No-op unless SPINEL_RBS_SEED names a seed file. */
  {
    const char *seed = getenv("SPINEL_RBS_SEED");
    if (seed && *seed) apply_rbs_seeds(c, seed);
  }

  /* Scope shape (count, class_id, name, is_cmethod) is fixed from here on, so
     the method-lookup index can be used; it is rebuilt if the scope count ever
     grows. This is where comp_method_in_class / comp_cmethod_in_class are
     hottest (called per node, every fixpoint iteration). */
  comp_scope_index_set_frozen(1);

  for (int iter = 0; iter < 128; iter++) {
    int ch = 0;
    sp_narrow_memo_bump();  /* invalidate per-iteration narrow-helper memo */
    build_ie_map(c);  /* refresh instance_exec receiver-class map each pass */
    ch |= register_ie_block_ivars(c);  /* slot ivars first assigned in iexec blocks */
    ch |= infer_write_types(c);
    ch |= infer_param_types(c);
    ch |= infer_param_hash_value(c);
    ch |= propagate_prep_params(c);
    ch |= infer_string_params(c);
    ch |= infer_default_param_types(c);
    ch |= desugar_enum_chain_to_a(c);          /* each_slice(n).to_a -> .map{|s|s} */
    ch |= desugar_enum_method_recv(c);         /* obj.map{} -> obj.__enum_to_a.map{} */
    ch |= desugar_implicit_send(c);            /* send(:m, a) -> m(a) on self */
    ch |= desugar_dynamic_send(c);             /* recv.send(var, a) -> static name dispatch */
    ch |= desugar_symbol_to_proc_call(c);      /* :sym.to_proc.call(x) -> x.sym */
    ch |= desugar_value_callable_forwards(c);  /* &proc -> { |x| proc.call(x) } */
    ch |= infer_block_params(c);
    ch |= infer_for_index(c);
    /* Resolve constant types before ivar inference: a destructured constant
       (`CLK_1,.. = (1..8).map{...}`) is transiently poly in infer_write_types
       before its array element type settles, and a monotonic ivar that reads
       it (`@clk += CLK_1`) would lock onto that poly. Resolving constants
       first feeds the settled type into ivar inference. */
    ch |= infer_global_const_types(c);
    ch |= infer_multiwrite_const_types(c);
    ch |= infer_ivar_types(c);
    ch |= infer_cvar_types(c);
    ch |= infer_inherited_ivars(c);
    ch |= infer_return_types(c);
    ch |= backprop_hash_return_types(c);
    if (!ch) break;
  }

  /* Optimistic re-narrow: the monotonic fixpoint locks a slot to poly the
     first time it sees a transient poly (a value read before its type settled,
     or an index/hash promotion driven by a then-poly key). Reset every poly
     ivar and poly (non-block) param to UNKNOWN once, then re-run the monotonic
     passes. With the now-stable concrete slots feeding them, a slot whose
     entire contribution closure is concrete re-narrows; one with a genuine
     heterogeneous contribution re-widens to poly. Sound (monotonic re-derive
     over fixed inputs); dissolves the cleared transient-poly cycles. */
  {
    int any = 0;
    /* Record the reset poly ivars so the re-run can re-clear them FRESH each
       iteration (a narrowing recompute), not just once. infer_ivar_types is
       monotonic (ty_unify only widens), so a one-shot reset still re-locks an
       ivar to poly the first time the re-run observes a transient poly from a
       not-yet-settled slot -- which is exactly what happens in a groundless
       self-referential poly cycle (clk_irq <- next_interrupt_clock(clk) <- clk
       <- clk_irq, whose real writes are all int). Re-clearing each iteration
       makes the ivar = unify-of-its-writes-this-iteration, so once the cycle's
       int anchors dominate it settles to int; a genuinely heterogeneous ivar
       re-widens and stays poly. Bounded narrowing over the lattice. */
    int rcap = 16, nrec = 0;
    int *recCi = (int *)malloc(sizeof(int) * rcap), *recIv = (int *)malloc(sizeof(int) * rcap);
    for (int ci = 0; ci < c->nclasses; ci++)
      for (int iv = 0; iv < c->classes[ci].nivars; iv++)
        if (c->classes[ci].ivar_types[iv] == TY_POLY) {
          const char *_n = c->classes[ci].ivars[iv]; sp_ivwatch(_n && _n[0]=='@' ? _n+1 : _n, "renarrow_reset", TY_POLY, TY_UNKNOWN);
          c->classes[ci].ivar_types[iv] = TY_UNKNOWN; any = 1;
          if (nrec >= rcap) { rcap *= 2; recCi = realloc(recCi, sizeof(int) * rcap); recIv = realloc(recIv, sizeof(int) * rcap); }
          recCi[nrec] = ci; recIv[nrec] = iv; nrec++;
        }
    for (int s = 0; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      for (int i = 0; i < sc->nparams; i++) {
        LocalVar *p = scope_local(sc, sc->pnames[i]);
        if (p && p->type == TY_POLY && !p->is_block_param) { p->type = TY_UNKNOWN; any = 1; }
      }
    }
    /* Plain locals ratchet through the same monotonic unify, so a local read
       before its feeding ivar settled (Pulse#sample's `sum` <- @timer) locks
       poly and re-poisons the ivars this loop just re-narrowed. Reset and
       re-clear them per iteration exactly like the ivars. */
    int lcap = 16, nlrec = 0;
    int *recLs = (int *)malloc(sizeof(int) * lcap), *recLi = (int *)malloc(sizeof(int) * lcap);
    for (int s = 0; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      for (int i = 0; i < sc->nlocals; i++) {
        LocalVar *lv = &sc->locals[i];
        if (lv->type != TY_POLY || lv->is_param || lv->is_block_param) continue;
        lv->type = TY_UNKNOWN; any = 1;
        if (nlrec >= lcap) { lcap *= 2; recLs = realloc(recLs, sizeof(int) * lcap); recLi = realloc(recLi, sizeof(int) * lcap); }
        recLs[nlrec] = s; recLi[nlrec] = i; nlrec++;
      }
    }
    if (reset_locked_iter_block_params(c)) any = 1;
    if (any) {
      TyKind *prev = (TyKind *)malloc(sizeof(TyKind) * (nrec > 0 ? nrec : 1));
      TyKind *lprev = (TyKind *)malloc(sizeof(TyKind) * (nlrec > 0 ? nlrec : 1));
      for (int iter = 0; iter < 128; iter++) {
        /* stash last-settled values, then re-clear the reset ivars so they
           recompute fresh (narrowing) this iteration. */
        for (int k = 0; k < nrec; k++) prev[k] = c->classes[recCi[k]].ivar_types[recIv[k]];
        for (int k = 0; k < nrec; k++) c->classes[recCi[k]].ivar_types[recIv[k]] = TY_UNKNOWN;
        for (int k = 0; k < nlrec; k++) lprev[k] = c->scopes[recLs[k]].locals[recLi[k]].type;
        for (int k = 0; k < nlrec; k++) c->scopes[recLs[k]].locals[recLi[k]].type = TY_UNKNOWN;
        sp_narrow_memo_bump();  /* invalidate per-iteration narrow-helper memo */
        int ch = 0;
        ch |= infer_write_types(c);
        ch |= infer_param_types(c);
        ch |= infer_param_hash_value(c);
        ch |= propagate_prep_params(c);
        ch |= infer_string_params(c);
        ch |= infer_default_param_types(c);
        ch |= infer_block_params(c);
        ch |= infer_for_index(c);
        ch |= infer_global_const_types(c);
        ch |= infer_multiwrite_const_types(c);
        ch |= infer_ivar_types(c);
        ch |= infer_cvar_types(c);
        ch |= infer_inherited_ivars(c);
        ch |= infer_return_types(c);
        /* With reset ivars, the re-clear makes infer_ivar_types report change
           every iteration, so converge on ivar value-stability instead. With
           none (only poly params/returns reset), fall back to the normal
           no-change fixpoint so the param/return passes fully settle. */
        if (nrec > 0 || nlrec > 0) {
          int stable = 1;
          for (int k = 0; k < nrec; k++)
            if (c->classes[recCi[k]].ivar_types[recIv[k]] != prev[k]) { stable = 0; break; }
          for (int k = 0; stable && k < nlrec; k++)
            if (c->scopes[recLs[k]].locals[recLi[k]].type != lprev[k]) stable = 0;
          if (stable) break;
        }
        else if (!ch) break;
      }
      free(prev); free(lprev);
    }
    free(recCi); free(recIv); free(recLs); free(recLi);
  }

  /* Backstop: a parameter still unknown but with a `= nil` default is a
     nullable param -- represent it as poly so it can hold nil or a value.
     Also widen TY_SYMBOL/TY_BOOL params: those types have no nil sentinel
     and must be boxed into poly when the nil default is reachable. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0) continue;
      const char *dty = nt_type(c->nt, sc->pdefault[i]);
      if (!dty || !sp_streq(dty, "NilNode")) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p || p->rbs_seeded) continue;
      if (p->type == TY_UNKNOWN || p->type == TY_SYMBOL || p->type == TY_BOOL)
        p->type = TY_POLY;
    }
  }

  /* Backstop: transplanted module scopes share the same def_node. If one
     copy has known param types (from call sites) but another copy lacks callers
     and has TY_UNKNOWN params, propagate the known types across. */
  for (int s1 = 0; s1 < c->nscopes; s1++) {
    Scope *sc1 = &c->scopes[s1];
    if (sc1->nparams == 0 || sc1->def_node < 0 || !sc1->name) continue;
    for (int pi = 0; pi < sc1->nparams; pi++) {
      if (!sc1->pnames[pi]) continue;
      LocalVar *p1 = scope_local(sc1, sc1->pnames[pi]);
      if (!p1 || p1->type != TY_UNKNOWN) continue;
      for (int s2 = 0; s2 < c->nscopes; s2++) {
        if (s2 == s1) continue;
        Scope *sc2 = &c->scopes[s2];
        if (sc2->def_node != sc1->def_node || sc2->nparams != sc1->nparams) continue;
        if (pi >= sc2->nparams || !sc2->pnames[pi]) continue;
        LocalVar *p2 = scope_local(sc2, sc2->pnames[pi]);
        if (!p2 || p2->type == TY_UNKNOWN) continue;
        p1->type = p2->type;
        break;
      }
    }
  }

  /* Backstop: an ivar assigned only an empty array literal (no element
     evidence from usage) is left UNKNOWN, which falls back to int and a
     scalar struct field. Default such a slot to an (empty) int array so the
     field is a pointer matching the emitted sp_IntArray_new(). */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "InstanceVariableWriteNode")) continue;
    int v = nt_ref(c->nt, id, "value");
    const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (!vty || !sp_streq(vty, "ArrayNode")) continue;
    int en = 0; nt_arr(c->nt, v, "elements", &en);
    if (en != 0) continue;
    Scope *s = comp_scope_of(c, id);
    int cls_id_bs = s->class_id;
    if (cls_id_bs < 0) cls_id_bs = comp_class_index(c, "Toplevel");
    if (cls_id_bs < 0) continue;
    ClassInfo *ci = &c->classes[cls_id_bs];
    int iv = comp_ivar_index(ci, nt_str(c->nt, id, "name"));
    if (iv >= 0 && ci->ivar_types[iv] == TY_UNKNOWN) ci->ivar_types[iv] = TY_INT_ARRAY;
  }
  /* Backstop: a local variable assigned only empty array literals with no
     push evidence stays TY_UNKNOWN. Default it to TY_POLY_ARRAY so array
     operations (map!, p, etc.) can dispatch. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int v = nt_ref(c->nt, id, "value");
    const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (!vty || !sp_streq(vty, "ArrayNode")) continue;
    int en = 0; nt_arr(c->nt, v, "elements", &en);
    if (en != 0) continue;
    const char *nm = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    /* Also reset any hash type that crept in via premature [] read
       promotion: a variable whose only write is an empty array literal
       is definitively an array, not a hash. */
    if (lv && !lv->rbs_seeded && (lv->type == TY_UNKNOWN || ty_is_hash(lv->type))) lv->type = TY_POLY_ARRAY;
  }
  /* Re-narrow a POLY_ARRAY ivar to IntArray when every element source is now
     (post-fixpoint) int. The monotonic usage pass locks the slot to POLY_ARRAY
     the first time it sees a push whose element type is still unknown -- e.g.
     `@output_pixels << @output_color[i]` evaluated before @output_color settled
     to IntArray. Once the element types settle, a slot whose only writes are int
     pushes / int `[]=` / empty-or-int-array assignments can drop the per-element
     boxing (optcarrot's per-frame pixel buffer is the motivating case). Strictly
     conservative: any non-int source, or a write shape we don't model, keeps it
     poly. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      if (cl->ivar_types[iv] != TY_POLY_ARRAY) continue;
      const char *ivn = cl->ivars[iv];
      int saw = 0, ok = 1;
      for (int id = 0; id < c->nt->count && ok; id++) {
        const char *ty = nt_type(c->nt, id);
        if (!ty) continue;
        if (sp_streq(ty, "CallNode")) {
          int recv = nt_ref(c->nt, id, "receiver");
          if (recv < 0) continue;
          const char *rty = nt_type(c->nt, recv);
          if (!rty || !sp_streq(rty, "InstanceVariableReadNode")) continue;
          const char *rivn = nt_str(c->nt, recv, "name");
          if (!rivn || !sp_streq(rivn, ivn)) continue;
          Scope *sc = comp_scope_of(c, id);
          if (!sc || sc->class_id != ci) { ok = 0; break; }
          const char *nm = nt_str(c->nt, id, "name");
          int args = nt_ref(c->nt, id, "arguments"); int an = 0;
          const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &an) : NULL;
          if (nm && (sp_streq(nm, "<<") || sp_streq(nm, "push") || sp_streq(nm, "append"))) {
            for (int a = 0; a < an; a++) { saw = 1; if (infer_type(c, argv[a]) != TY_INT) { ok = 0; break; } }
          }
          else if (nm && sp_streq(nm, "[]=") && an == 2) {
            saw = 1; if (infer_type(c, argv[1]) != TY_INT) ok = 0;
          }
        }
        else if (sp_streq(ty, "InstanceVariableWriteNode")) {
          const char *wivn = nt_str(c->nt, id, "name");
          if (!wivn || !sp_streq(wivn, ivn)) continue;
          Scope *sc = comp_scope_of(c, id);
          if (!sc || sc->class_id != ci) { ok = 0; break; }
          int v = nt_ref(c->nt, id, "value");
          TyKind vt = v >= 0 ? infer_type(c, v) : TY_UNKNOWN;
          if (vt == TY_INT_ARRAY) { /* int array source */ }
          else if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "ArrayNode")) {
            int en = 0; const int *el = nt_arr(c->nt, v, "elements", &en);
            for (int e = 0; e < en; e++) if (infer_type(c, el[e]) != TY_INT) { ok = 0; break; }
          }
          else { ok = 0; }
        }
      }
      if (saw && ok) {
        sp_ivwatch(ivn && ivn[0] == '@' ? ivn + 1 : ivn, "renarrow_int_array", TY_POLY_ARRAY, TY_INT_ARRAY);
        cl->ivar_types[iv] = TY_INT_ARRAY;
      }
    }
  }

  /* A read-only ivar (referenced but never assigned a typed value) stays
     TY_UNKNOWN -> it has no C type. Such a slot always reads nil at runtime;
     give it a boxed-nil poly field so `.nil?`/`.inspect` behave (#712).
     A never-typed BASE-class slot (only nil inits, e.g. an abstract
     Oscillator whose subclasses hold the real object) takes its subclasses'
     unified type first: going straight to poly would re-widen every typed
     subclass slot on the rerun below. Post-fixpoint, so subclass types are
     final. */
  int ivar_backstop_changed = 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      if (cl->ivar_types[iv] != TY_UNKNOWN) continue;
      /* Unify across the whole hierarchy this class belongs to (root's
         subtree), so an abstract base AND a leaf that never writes the ivar
         (e.g. Triangle's @envelope) both take the type their siblings hold,
         keeping one C type per field across the hierarchy. */
      int root = ci;
      while (c->classes[root].parent >= 0) root = c->classes[root].parent;
      TyKind fill = TY_UNKNOWN;
      for (int cj = 0; cj < c->nclasses; cj++) {
        int an = cj;
        while (an >= 0 && an != root) an = c->classes[an].parent;
        if (an != root) continue;
        int cidx = comp_ivar_index(&c->classes[cj], cl->ivars[iv]);
        if (cidx >= 0 && c->classes[cj].ivar_types[cidx] != TY_UNKNOWN)
          fill = ty_unify(fill, c->classes[cj].ivar_types[cidx]);
      }
      const char *_n = cl->ivars[iv];
      sp_ivwatch(_n && _n[0]=='@' ? _n+1 : _n, "unknown_backstop", TY_UNKNOWN,
                 fill != TY_UNKNOWN ? fill : TY_POLY);
      cl->ivar_types[iv] = fill != TY_UNKNOWN ? fill : TY_POLY;
      ivar_backstop_changed = 1;
    }
  }
  /* A void/nil-typed ivar slot -- only ever assigned a value-less expression
     (a writer call, a value-less `if`) or a bare nil that never unified with
     a concrete type -- has no C storage type: emit_class_struct would declare
     a `void` field. Widen to poly; such a slot always reads nil at runtime. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      TyKind ivt = cl->ivar_types[iv];
      if (ivt != TY_VOID && ivt != TY_NIL) continue;
      const char *_n = cl->ivars[iv];
      sp_ivwatch(_n && _n[0] == '@' ? _n + 1 : _n, "void_backstop", ivt, TY_POLY);
      cl->ivar_types[iv] = TY_POLY;
      ivar_backstop_changed = 1;
    }
  }
  /* An attr_reader/attr_accessor ivar typed via a writer call (scalar type),
     but whose class has no initialize that writes it, starts nil on fresh
     instances. Only widen when there is NO write inside ANY initialize in
     the inheritance chain (the read-only case is already TY_POLY via the
     TY_UNKNOWN pass above). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    if (cl->is_struct) continue; /* struct members are set by generated ctor */
    int init_mi = comp_method_in_chain(c, ci, "initialize", NULL);
    if (init_mi >= 0) continue;
    for (int ri = 0; ri < cl->nreaders; ri++) {
      const char *rname = cl->readers[ri];
      if (!rname) continue;
      char ivname[300]; snprintf(ivname, sizeof ivname, "@%s", rname);
      int iv = comp_ivar_index(cl, ivname);
      if (iv < 0) continue;
      if (class_ivar_pinned(cl, ivname)) continue;  /* --rbs seed pins the type */
      TyKind t = cl->ivar_types[iv];
      /* TY_INT is exempt: the generated constructor already seeds int ivars
         with SP_INT_NIL (emit_ivar_nil_inits), so a pre-write read is nil
         through the nullable-int machinery without widening to poly. */
      if (t != TY_FLOAT && t != TY_STRING &&
          t != TY_SYMBOL && t != TY_BOOL) continue;
      cl->ivar_types[iv] = TY_POLY;
      ivar_backstop_changed = 1;
      /* Also patch the node-type cache for all InstanceVariableReadNode and
         InstanceVariableWriteNode nodes that reference this ivar, so codegen
         sees TY_POLY for both the struct field and the node type. */
      for (int nid = 0; nid < c->nt->count; nid++) {
        const char *nty = nt_type(c->nt, nid);
        if (!nty) continue;
        if (!sp_streq(nty, "InstanceVariableReadNode") &&
            !sp_streq(nty, "InstanceVariableWriteNode") &&
            !sp_streq(nty, "InstanceVariableOperatorWriteNode") &&
            !sp_streq(nty, "InstanceVariableOrWriteNode") &&
            !sp_streq(nty, "InstanceVariableAndWriteNode")) continue;
        /* only within methods of this class */
        Scope *s = comp_scope_of(c, nid);
        if (!s || s->class_id != ci) continue;
        const char *nm = nt_str(c->nt, nid, "name");
        if (nm && sp_streq(nm, ivname)) c->ntype[nid] = TY_POLY;
      }
    }
  }
  /* `return .. if p.nil?` early-return guards: reads of the guarded poly
     param after the guard take its non-nil type (#1661). Post-fixpoint so
     param and argument types are final; before the return recompute below so
     narrowed tails type the enclosing method's return. */
  narrow_nil_guard_params(c);
  narrow_nil_guard_locals(c);
  /* Post-backstop: re-run write type inference so multi-write locals whose
     RHS chains through a now-typed ivar (e.g. @h[bank][idx] where @h was
     just promoted from UNKNOWN to POLY) get their types resolved. */
  infer_write_types(c);
  /* recompute returns: a method returning such a param is now poly */
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;

  /* Post-fixpoint body-usage inference: type any param still TY_UNKNOWN
     from how it is used inside the method body (hash subscript patterns,
     array-specific calls). Runs after the main fixpoint so caller-side
     types always win; the mini-loop below propagates the new types.
     Also re-runs after the ivar backstops above widened an UNKNOWN slot
     to poly: reads of such an ivar inferred UNKNOWN during the fixpoint,
     leaving params bound from them untyped (and the method dropped,
     turning its call sites into undefined references). */
  if (infer_hash_params(c) | infer_array_params(c) | infer_params_from_ivar_hash_ops(c) |
      ivar_backstop_changed) {
    for (int iter = 0; iter < 16; iter++) {
      int ch = 0;
      ch |= infer_param_types(c);
      ch |= infer_return_types(c);
      /* Re-run write-type inference so locals whose types derive from
         function return types (e.g. `x = f([])` after `f`'s param was
         promoted from UNKNOWN to POLY_ARRAY) get updated. */
      ch |= infer_write_types(c);
      if (!ch) break;
    }
  }

  /* Post-fixpoint: hash-variant back-propagation. A local that unified to
     TY_POLY_POLY_HASH (e.g. a subscript write with a poly key widened it)
     while the ivars feeding it stayed Str/Sym-keyed leaves the emitted C
     with a PolyPolyHash* local pointing at a StrPolyHash object -- every
     write through it then runs PolyPolyHash_set against the wrong struct
     layout and corrupts the heap (doom's Animations#update
     `translation = ... ? @texture_translation : @flat_translation`, which
     shredded neighboring framebuffer memory into nils). Widen such source
     ivars to the local's variant and re-run inference. */
  {
    int hb_changed = 0;
    for (int id = 0; id < c->nt->count; id++) {
      const char *nty = nt_type(c->nt, id);
      if (!nty || !sp_streq(nty, "LocalVariableWriteNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
      if (!lv || lv->type != TY_POLY_POLY_HASH) continue;
      int v = nt_ref(c->nt, id, "value");
      if (v < 0) continue;
      /* DFS the value subtree for ivar reads (covers ternary/if arms);
         stop at nested defs. Small fixed stack: value subtrees are tiny. */
      int stack[256]; int sp = 0; stack[sp++] = v;
      while (sp > 0) {
        int nid = stack[--sp];
        const char *t2 = nt_type(c->nt, nid);
        if (!t2 || sp_streq(t2, "DefNode")) continue;
        if (sp_streq(t2, "InstanceVariableReadNode")) {
          Scope *sc2 = comp_scope_of(c, nid);
          int ci2 = sc2 ? sc2->class_id : -1;
          const char *ivn = nt_str(c->nt, nid, "name");
          if (ci2 >= 0 && ivn) {
            int ix = comp_ivar_index(&c->classes[ci2], ivn);
            if (ix >= 0 && (c->classes[ci2].ivar_types[ix] == TY_STR_POLY_HASH ||
                            c->classes[ci2].ivar_types[ix] == TY_SYM_POLY_HASH)) {
              c->classes[ci2].ivar_types[ix] = TY_POLY_POLY_HASH;
              hb_changed = 1;
              /* patch the node-type cache for every read/write of this ivar
                 in this class so codegen agrees (mirrors the UNKNOWN
                 backstop above) */
              for (int nid2 = 0; nid2 < c->nt->count; nid2++) {
                const char *nty2 = nt_type(c->nt, nid2);
                if (!nty2) continue;
                if (!sp_streq(nty2, "InstanceVariableReadNode") &&
                    !sp_streq(nty2, "InstanceVariableWriteNode") &&
                    !sp_streq(nty2, "InstanceVariableOrWriteNode")) continue;
                Scope *s2 = comp_scope_of(c, nid2);
                if (!s2 || s2->class_id != ci2) continue;
                const char *nm2 = nt_str(c->nt, nid2, "name");
                if (nm2 && sp_streq(nm2, ivn)) c->ntype[nid2] = TY_POLY_POLY_HASH;
              }
            }
          }
        }
        int nr2 = nt_num_refs(c->nt, nid);
        for (int i2 = 0; i2 < nr2 && sp < 250; i2++) { int ch2 = nt_ref_at(c->nt, nid, i2); if (ch2 >= 0) stack[sp++] = ch2; }
        int na2 = nt_num_arrs(c->nt, nid);
        for (int i2 = 0; i2 < na2 && sp < 250; i2++) { int nn2 = 0; const int *ids2 = nt_arr_at(c->nt, nid, i2, &nn2); for (int k2 = 0; k2 < nn2 && sp < 250; k2++) if (ids2[k2] >= 0) stack[sp++] = ids2[k2]; }
      }
    }
    if (hb_changed) {
      for (int iter = 0; iter < 16; iter++) {
        int ch = 0;
        ch |= infer_param_types(c);
        ch |= infer_return_types(c);
        ch |= infer_write_types(c);
        if (!ch) break;
      }
    }
  }

  /* Post-fixpoint: unify param types across method override families.
     When an override widens a param to TY_POLY but the parent (or
     sibling) keeps it scalar, the generated C signatures disagree and
     virtual dispatch can't call both with the same arg temps. Walk all
     scope pairs that are overrides of the same instance method in a
     parent-child class pair and widen any differing slot to TY_POLY. */
  for (int s1 = 0; s1 < c->nscopes; s1++) {
    Scope *sc1 = &c->scopes[s1];
    if (sc1->class_id < 0 || !sc1->name || sc1->is_cmethod || sc1->nparams == 0) continue;
    /* initialize is never virtually dispatched (always via ClassName.new), so
       each override may have fully independent param types. */
    if (sp_streq(sc1->name, "initialize")) continue;
    for (int s2 = s1 + 1; s2 < c->nscopes; s2++) {
      Scope *sc2 = &c->scopes[s2];
      if (sc2->class_id < 0 || !sc2->name || sc2->is_cmethod || sc2->nparams == 0) continue;
      if (!sp_streq(sc1->name, sc2->name)) continue;
      /* check ancestor relationship: one class must be an ancestor of the other */
      int c1 = sc1->class_id, c2 = sc2->class_id;
      int related = 0;
      for (int k = c1; k >= 0; k = c->classes[k].parent) if (k == c2) { related = 1; break; }
      if (!related)
        for (int k = c2; k >= 0; k = c->classes[k].parent) if (k == c1) { related = 1; break; }
      if (!related) continue;
      int np = sc1->nparams < sc2->nparams ? sc1->nparams : sc2->nparams;
      for (int k = 0; k < np; k++) {
        LocalVar *p1 = scope_local(sc1, sc1->pnames[k]);
        LocalVar *p2 = scope_local(sc2, sc2->pnames[k]);
        if (!p1 || !p2) continue;
        if (p1->type != p2->type && (p1->type == TY_POLY || p2->type == TY_POLY)) {
          p1->type = TY_POLY;
          p2->type = TY_POLY;
        }
      }
      /* Also unify return types: if one member returns poly and another void/nil,
         make both return poly so the dispatch statement-expression can capture
         a scalar result from any arm. */
      if (sc1->ret != sc2->ret && (sc1->ret == TY_POLY || sc2->ret == TY_POLY)) {
        sc1->ret = TY_POLY;
        sc2->ret = TY_POLY;
      }
    }
  }

  /* Promote loop-multiplication variables to bigint */
  detect_bigint_loop_vars(c);
  propagate_bigint_cascade(c);

  /* Force-lower `#each` for any class whose synthesized `__enum_to_a` helper is
     actually called (an `enum_for`/`to_enum` survived the rewrite). The helper
     drives `#each` with a collector block; the real (lowered) function form
     passes that block as a proc. Done BEFORE mark_proc_captures so the
     collector block lifts and its `__enum_acc` accumulator gets a heap cell. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "__enum_to_a")) continue;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int ec = comp_method_in_chain(c, ty_object_class(rt), "each", NULL);
    if (ec < 0) continue;
    Scope *m = &c->scopes[ec];
    if (m->blk_param || !m->yields) continue;   /* already lowered, or no yielding each */
    m->is_lowered_yield = 1;
    m->yields = 0;
    m->ret = TY_INT;
    m->blk_param = strdup("__yblk__");
    LocalVar *yblk = scope_local_intern(m, "__yblk__");
    if (yblk) { yblk->type = TY_PROC; yblk->is_param = 1; yblk->is_cell = 1; }
  }

  /* mark locals captured by escaping procs (they need heap cells) */
  mark_proc_captures(c);

  /* Reachability: an instance/free method is live only if its name is
     referenced somewhere -- as a call name, an alias target, or a symbol
     literal (covering send/method/define_method). Names never mentioned
     are dead code; skipping them avoids type-checking uninvoked methods
     (e.g. a never-called method with an uninferrable param). */
  compute_reachable(c);
  /* Which exact cls_ids can appear at runtime -- lets the poly-dispatch switch
     drop `case` arms for classes that are never instantiated (the referenced
     method then DCEs as an unreferenced static). */
  compute_instantiated(c);

  /* Lower self-recursive yield methods: methods that use `yield` AND call
     themselves recursively. Their implicit block is forwarded as a synthetic
     __yblk__ sp_Proc * parameter, so the method is emitted (yields=0) and
     each `yield` in its body calls sp_proc_call(__yblk__, ...). */
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->name || !m->reachable || m->blk_param) continue;
    if (!m->yields) continue;
    if (m->body < 0) continue;
    int has_yld = 0;
    for (int id = 0; id < c->nt->count && !has_yld; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(c->nt, id);
      if (ty && sp_streq(ty, "YieldNode")) has_yld = 1;
    }
    if (!has_yld) continue;
    int has_self_call = 0;
    for (int id = 0; id < c->nt->count && !has_self_call; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(c->nt, id);
      if (!ty || !sp_streq(ty, "CallNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || !sp_streq(nm, m->name)) continue;
      int recv = nt_ref(c->nt, id, "receiver");
      const char *rty = recv >= 0 ? nt_type(c->nt, recv) : NULL;
      if (recv < 0 || (rty && sp_streq(rty, "SelfNode"))) has_self_call = 1;
    }
    if (!has_self_call) continue;
    m->is_lowered_yield = 1;
    m->yields = 0;
    m->ret = TY_INT;
    m->blk_param = strdup("__yblk__");
    LocalVar *yblk = scope_local_intern(m, "__yblk__");
    if (yblk) {
      yblk->type = TY_PROC;
      yblk->is_param = 1;
      yblk->is_cell = 1;
    }
  }

  /* Post-fixpoint: propagate include-copy param types back to the source
     scope so the final infer_type scan (which uses comp_scope_of, mapping
     body nodes to the ORIGINAL scope) sees the correctly-typed params.
     Without this, LocalVariableReadNodes inside the body get TY_UNKNOWN
     because the source scope's params were never updated (no direct calls
     go through it). */
  for (int ci = 0; ci < c->nscopes; ci++) {
    Scope *copy = &c->scopes[ci];
    if (!copy->name || !copy->is_transplanted_source || copy->nparams == 0) continue;
    /* This is a transplanted SOURCE: find copies (same body, different class_id,
       params registered and typed) and unify their param types back here. */
    for (int k = 0; k < c->nscopes; k++) {
      if (k == ci) continue;
      Scope *dst = &c->scopes[k];
      if (!dst->name || !sp_streq(dst->name, copy->name)) continue;
      if (dst->body != copy->body || dst->nparams != copy->nparams) continue;
      if (!dst->is_transplanted_source) {
        /* dst is a copy: unify its param types into the source */
        for (int p = 0; p < copy->nparams; p++) {
          if (!copy->pnames[p]) continue;
          LocalVar *slv = scope_local(copy, copy->pnames[p]);
          LocalVar *dlv = scope_local(dst,  dst->pnames[p]);
          if (!slv || !dlv || dlv->type == TY_UNKNOWN || slv->rbs_seeded) continue;
          TyKind mg = ty_unify(slv->type, dlv->type);
          if (mg != slv->type) slv->type = mg;
        }
        if (dst->ret != TY_UNKNOWN && copy->ret == TY_UNKNOWN)
          copy->ret = dst->ret;
      }
    }
  }

  /* Backstop step 1: a method reached only via method(:sym) is invoked through
     the bound Method ABI, which passes mrb_int args -- default its untyped
     params/ret to int rather than dropping it (which would leave it undeclared).
     Done before the drop decision below so the freshly-typed params can
     propagate through poly-dispatch param binding (e.g. a poke dispatch table
     entry typed here flows into `@pads[0].poke(data)` -> Pad#poke). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || !sc->name) continue;
    int taken = 0;
    for (int id = 0; id < c->nt->count && !taken; id++) {
      const char *nty = nt_type(c->nt, id);
      if (!nty || !sp_streq(nty, "CallNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || !sp_streq(nm, "method")) continue;
      const char *msym = method_sym_arg(c, id);
      if (msym && sp_streq(msym, sc->name)) taken = 1;
    }
    if (taken) {
      /* The bound-Method ABI is `mrb_int (*)(void *, mrb_int...)`: the dispatch
         site (sp_poly_arr_get_hash / sp_poly_slice) reads the return as mrb_int
         and passes int args. So a method(:sym) target MUST return int and take
         int params -- a poly return (e.g. PPU#peek_2002 returning the poly
         @io_latch) would be misread as a struct through the int cast and yield
         garbage. Pin unknown params to int and a poly/unknown return to int
         (codegen coerces the body's poly return via sp_poly_to_i). */
      for (int i = 0; i < sc->nparams; i++) {
        LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
        if (p && p->type == TY_UNKNOWN) p->type = TY_INT;
      }
      if (sc->ret == TY_UNKNOWN || sc->ret == TY_POLY) sc->ret = TY_INT;
    }
  }


  /* Propagate ivar types up the inheritance chain: a base-class method runs on
     subclass instances, so an ivar it reads must carry the union of every
     subclass's assignments. Without this, an abstract base whose @x is only set
     to nil/placeholder there sees the wrong type when it calls `@x.foo`, even
     though every concrete subclass assigns @x a real object. Monotonic (unify
     only widens), so iterate to a fixpoint. */
  for (int iter = 0; iter < 16; iter++) {
    int prop_changed = 0;
    for (int k = 0; k < c->nclasses; k++) {
      ClassInfo *kc = &c->classes[k];
      for (int iv = 0; iv < kc->nivars; iv++) {
        TyKind kt = kc->ivar_types[iv];
        if (kt == TY_UNKNOWN) continue;
        const char *ivn = kc->ivars[iv];
        for (int a = kc->parent; a >= 0; a = c->classes[a].parent) {
          int ai = comp_ivar_index(&c->classes[a], ivn);
          if (ai < 0) continue;
          if (class_ivar_pinned(&c->classes[a], ivn)) continue;  /* --rbs seed pins it */
          TyKind merged = ty_unify(c->classes[a].ivar_types[ai], kt);
          sp_ivwatch(ivn[0] == '@' ? ivn + 1 : ivn, "inherited_merge", c->classes[a].ivar_types[ai], merged);
          if (merged != c->classes[a].ivar_types[ai]) {
            c->classes[a].ivar_types[ai] = merged; prop_changed = 1;
          }
        }
      }
    }
    if (!prop_changed) break;
  }

  /* Re-run param binding now that method(:sym) targets are int-typed (step 1
     above) and ivars carry their inheritance-unioned types: a base method
     calling `@x.foo(arg)` on a poly @x (only widened to poly by the up-propagation
     just above) can finally bind foo's params. Without this the bound method
     would stay TY_UNKNOWN and be dropped below, leaving an undefined function at
     the poly-dispatch call site. */
  build_ie_map(c);
  /* Seed the synthesized compiler_state IR-emit helpers' signature types. They
     are called only by the synthesized dump_compiler_state_ir (which has no
     AST), so the param backstop below can't bind them from a call site;
     unseeded they stay TY_UNKNOWN, get pruned, and the dump then references an
     undefined function. Seeding BEFORE the backstop lets the binding propagate
     into the helpers they call (ir_join_ints / ir_escape). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    const char *snm = sc->name;
    if (!snm || sc->nparams < 3) continue;
    if (!sp_streq(snm, "ir_emit_int") && !sp_streq(snm, "ir_emit_str") &&
        !sp_streq(snm, "ir_emit_sa") && !sp_streq(snm, "ir_emit_ia")) continue;
    LocalVar *pb = scope_local(sc, sc->pnames[0]);
    LocalVar *pn = scope_local(sc, sc->pnames[1]);
    LocalVar *pv = scope_local(sc, sc->pnames[2]);
    if (pb) pb->type = TY_STRING;
    if (pn) pn->type = TY_STRING;
    if (pv) pv->type = sp_streq(snm, "ir_emit_int") ? TY_INT :
                       sp_streq(snm, "ir_emit_str") ? TY_STRING :
                       sp_streq(snm, "ir_emit_sa")  ? TY_STR_ARRAY : TY_INT_ARRAY;
    sc->ret = TY_STRING;  /* each returns the accumulated buf (a string) */
  }
  for (int it = 0; it < 8; it++) { if (!infer_param_types(c)) break; }

  /* method_missing is not honored: spinel resolves every call statically and
     does not route an undefined-method call to method_missing (that would need
     runtime dispatch foreign to the whole-program model). Warn once per
     definition so the author isn't misled into relying on it -- the method is
     still callable explicitly, it just never fires as a missing-method hook. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->class_id < 0 || !sc->name || !sp_streq(sc->name, "method_missing")) continue;
    int dn = sc->def_node;
    int ln = dn >= 0 ? (int)nt_int(c->nt, dn, "node_line", 0) : 0;
    const char *file = c->nt->source_file;
    if (ln > 0) {
      const char *f = nt_file_path(c->nt, (int)nt_int(c->nt, dn, "node_file", 0));
      if (f && *f) file = f;
    }
    if (!file || !*file) file = "source.rb";
    fprintf(stderr, "spinel: %s:%d: warning: method_missing is defined but "
            "spinel does not dispatch undefined-method calls to it; such calls "
            "raise NoMethodError (method_missing can still be called "
            "explicitly)\n", file, ln);
  }

  /* Backstop step 2: a reachable method whose parameter STILL has TY_UNKNOWN was
     never bound by a typed call site -- every call reached it with a poly/untyped
     argument (e.g. an FFI :pointer return). The method is still genuinely called,
     so it cannot just be dropped: codegen would still emit the call and dangle on
     the missing symbol (#1606 -- the direct-call sibling of #1583). Widen the
     unknown parameter to TY_POLY so the body emits with a poly parameter and the
     call links. A truly-dead method (no real call) gets the same harmless poly
     body and is dropped by --gc-sections at link. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || sc->nparams == 0) continue;
    for (int i = 0; i < sc->nparams; i++) {
      LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
      if (p && p->type == TY_UNKNOWN) p->type = TY_POLY;
    }
  }

  /* The method-reference backstop and ivar up-propagation above changed param
     and ivar types after the main fixpoint, so re-run local write-type inference:
     a local like `xfine = 8 - (data & 0x7)` only becomes int once its method's
     `data` param is pinned to int by the backstop. Return types alternate with
     the write re-runs: a method returning through a local (`r = expr; r`) whose
     expr only settles here re-derives its return, which in turn types callers'
     locals (`a = m()`) -- and the callers' own returns -- on the next round.
     A write-only re-run would strand such a caller's return at UNKNOWN, and
     the method would emit as void, silently dropping its value (#1670). */
  g_ret_no_new_poly = 1;
  for (int iter = 0; iter < 8; iter++) {
    int ch = infer_write_types(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }
  g_ret_no_new_poly = 0;
  /* The write-type re-run can re-derive a hash/array container type for an
     iteration-bound block param from its element-index usage (e.g. `a[1]=v`),
     clobbering the TY_POLY the block-param pass pinned for a poly-collection
     `.each`. Re-pin block-param types so poly elements stay poly. */
  for (int iter = 0; iter < 8; iter++) if (!infer_block_params(c)) break;
  /* infer_write_types resets non-param locals, undoing the earlier bigint
     loop-variable promotion, so re-apply it. */
  detect_bigint_loop_vars(c);
  propagate_bigint_cascade(c);

  /* A non-parameter local that inference never resolved holds a value of unknown
     static type -- a block param bound to an element of a poly receiver, or a
     local fed by a dynamically-dispatched call (e.g. optcarrot's memory-map
     procs). Declare it boxed (poly) rather than failing codegen; reads then go
     through the tag-dispatching poly paths. Method params are excluded: the
     backstop above already pins or drops those. Set before the node-type cache
     is rebuilt so reads of the local see poly. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *blv = &c->scopes[s].locals[i];
      if (!blv->is_param && blv->type == TY_UNKNOWN) blv->type = TY_POLY;
      /* A slot left at TY_NIL only ever saw nil (it never narrowed against an
         object). It has no object class, so represent it as a boxed-nil poly.
         Applies to params too (a purely-nil param). An --rbs-seeded slot keeps
         its pinned type. */
      if (blv->type == TY_NIL && !blv->rbs_seeded) blv->type = TY_POLY;
    }

  /* Re-merge inherited ivar NAMES into subclasses now that the fixpoint has
     registered every ivar -- including ones a parent only gained from an
     included module's transplanted methods (`module M; def m; @x = ...; end`
     included into a base, then subclassed). inherit_members ran once before
     the fixpoint, when the parent had no such ivar yet, so the subclass struct
     would otherwise lack the field (a default-arg read `def g(r = @x)` on the
     subclass then references a non-existent member). Rebuilds parent-first, so
     the [parent ivars..., own ivars...] cast-compatible layout is preserved. */
  inherit_members(c);

  /* Re-run ivar inference now that purely-nil params/locals became poly: an
     ivar fed by such a param (`@x = idx` where every `set` call passed nil)
     was skipped during the fixpoint (its value read as TY_NIL) and may have
     stayed a narrower scalar; with the param now poly the write contributes
     poly so the ivar widens to match. */
  for (int it = 0; it < 8; it++) {
    int ch = infer_ivar_types(c);
    ch |= infer_inherited_ivars(c);
    if (!ch) break;
  }

  /* --int-overflow=promote: widen every statically-int slot (param / return /
     local / ivar / cvar) to poly so an arithmetic result that overflows int64
     can be carried as a boxed bigint (sp_poly_add/mul promote at runtime). A
     poly slot still holds a small value inline (SP_TAG_INT, no heap), so this is
     far cheaper than the legacy "everything becomes sp_Bigint*" widen. Done
     after the fixpoint and before the final node-type cache so reads pick up the
     widened slot types. TY_BIGINT loop vars (detect_bigint_loop_vars) stay
     bigint. EXPERIMENTAL: gated by g_promote_mode; default/wrap untouched. */
  if (g_promote_mode) {
    for (int s = 0; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      /* `<=>` yields a bounded -1/0/1 and is consumed as `(a <=> b) <cmp> 0`;
         widening its return to poly would force every caller's comparison onto
         the poly path while the result never overflows. Keep it int. */
      int is_spaceship = sc->name && sp_streq(sc->name, "<=>");
      /* A lowered self-recursive yield method returns its block's value through
         a raw mrb_int carrier (a string is laundered through the slot, an int
         rides it directly), and every call site casts that carrier back to its
         own block's concrete type. Widening the return to poly would emit a
         `sp_box_str(mrb_int)` over that raw carrier and mismatch the per-call
         casts; keep it as the carrier, exactly as default/wrap mode does. */
      if (sc->ret == TY_INT && !is_spaceship && !sc->is_lowered_yield) sc->ret = TY_POLY;
      for (int i = 0; i < sc->nlocals; i++) {
        /* Skip block params: they are typed by the iterated collection's
           element type (an IntArray yields int elements), and the block
           emitters already retype the param to that element type for the body
           (use_shadow). Widening them to poly only creates an int/poly shadow
           and inconsistent reads (e.g. `x.even?` keeps the stale poly type
           while `x*2` sees the retyped int). Method params still widen. */
        if (sc->locals[i].is_block_param) continue;
        if (sc->locals[i].type == TY_INT) sc->locals[i].type = TY_POLY;
      }
    }
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *cl = &c->classes[ci];
      for (int i = 0; i < cl->nivars; i++)
        if (cl->ivar_types[i] == TY_INT) cl->ivar_types[i] = TY_POLY;
      for (int i = 0; i < cl->ncvars; i++)
        if (cl->cvar_types[i] == TY_INT) cl->cvar_types[i] = TY_POLY;
    }
  }

  /* narrow monomorphic object arrays (POLY_ARRAY -> obj-pointer array) before
     the node cache is finalized so the rebuild below propagates the new element
     types to every `arr[i]` / `arr[i].field` site. */
  narrow_object_arrays(c);

  /* narrow poly locals that are only ever used as ints (drop per-use boxing);
     the rebuild below re-infers their reads/ops at the narrowed int type. */
  narrow_poly_int_locals(c);

  /* finalize: gc-root needs + full node type cache */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++)
      c->scopes[s].locals[i].gc_root = (c->scopes[s].locals[i].type == TY_STRING);

  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);

  /* --int-overflow=promote: the widen above can change a proc body's return
     type (a captured int local widened to poly), so a proc's caller-side
     proc_ret / a factory method's ret_proc_ret must be re-derived from the
     now-widened body -- else a `.call` reads the wrong return channel (raw
     mrb_int slot vs the poly side-channel) and yields 0. Re-run JUST the
     proc_ret / ret_proc_ret derivations (mirroring infer_return_types and
     infer_write_types) as a focused fixpoint; this touches only proc-return
     metadata, never the widened slot types, so it cannot undo the widen.
     A factory's ret_proc_ret feeds a caller's proc_ret, hence the loop. */
  if (g_promote_mode) {
    const NodeTable *nt = c->nt;
    int changed = 1, iters = 0;
    while (changed && iters++ < 32) {
      changed = 0;
      /* (1) method scopes that return a proc: ret_proc_ret from the body. */
      for (int s = 0; s < c->nscopes; s++) {
        Scope *sc = &c->scopes[s];
        if (sc->ret != TY_PROC) continue;
        TyKind pr = TY_UNKNOWN;
        if (sc->body >= 0) {
          int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn);
          if (bn > 0) pr = proc_ret_of(c, bb[bn - 1]);
        }
        for (int id = 0; id < nt->count; id++) {
          const char *ty = nt_type(nt, id);
          if (ty && sp_streq(ty, "ReturnNode") && comp_scope_of(c, id) == sc) {
            int a = nt_ref(nt, id, "arguments"); int an = 0;
            const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
            if (an > 0) pr = proc_ret_of(c, av[0]);
          }
        }
        if (pr != TY_UNKNOWN && sc->ret_proc_ret != (int)pr) { sc->ret_proc_ret = (int)pr; changed = 1; }
      }
      /* (2) proc-typed locals: proc_ret from the assigned proc / factory call. */
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm) continue;
        LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
        if (!lv || lv->type != TY_PROC) continue;
        int vnode = nt_ref(nt, id, "value");
        TyKind pr = vnode >= 0 ? proc_ret_of(c, vnode) : TY_UNKNOWN;
        if (pr != TY_UNKNOWN && (TyKind)lv->proc_ret != pr) { lv->proc_ret = (int)pr; changed = 1; }
      }
      /* (3) a typed-array-returning method whose elements widened now yields a
         poly array in its body (e.g. `[a/b, a%b]` with poly a,b builds a
         PolyArray), so its return type must follow to TY_POLY_ARRAY. */
      for (int s = 0; s < c->nscopes; s++) {
        Scope *sc = &c->scopes[s];
        TyKind r = (TyKind)sc->ret;
        if (r != TY_INT_ARRAY && r != TY_STR_ARRAY && r != TY_FLOAT_ARRAY) continue;
        TyKind br = TY_UNKNOWN;
        if (sc->body >= 0) { int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn); if (bn > 0) br = comp_ntype(c, bb[bn - 1]); }
        for (int id = 0; id < nt->count && br != TY_POLY_ARRAY; id++) {
          const char *ty = nt_type(nt, id);
          if (ty && sp_streq(ty, "ReturnNode") && comp_scope_of(c, id) == sc) {
            int a = nt_ref(nt, id, "arguments"); int an = 0;
            const int *av = a >= 0 ? nt_arr(nt, id, "arguments", &an) : NULL;
            if (an == 1 && comp_ntype(c, av[0]) == TY_POLY_ARRAY) br = TY_POLY_ARRAY;
          }
        }
        if (br == TY_POLY_ARRAY) { sc->ret = TY_POLY_ARRAY; changed = 1; }
      }
      /* (4) a local whose assigned value widened to a poly array must follow:
         its declared IntArray/StrArray/FloatArray slot would otherwise mismatch
         the PolyArray now produced -- whether by a map method whose return
         widened (step 3), or by an array literal whose elements widened
         (`arr = [x, y, x + y]` with poly x,y builds a PolyArray). */
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm) continue;
        LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
        if (!lv) continue;
        if (lv->type != TY_INT_ARRAY && lv->type != TY_STR_ARRAY &&
            lv->type != TY_FLOAT_ARRAY) continue;
        int vnode = nt_ref(nt, id, "value");
        if (vnode < 0) continue;
        if (infer_type(c, vnode) == TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }
      }
      /* (5) a constant assigned from a value that widened to poly (a method
         return widened in step 3, an int constant assigned an arithmetic
         result, ...) must follow: a `COUNT = obj.m` whose method now returns
         poly otherwise mismatches the int constant slot. */
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "ConstantWriteNode")) continue;
        const char *nm = nt_str(nt, id, "name");
        LocalVar *cv = nm ? comp_const(c, nm) : NULL;
        if (!cv || cv->type != TY_INT) continue;
        int vnode = nt_ref(nt, id, "value");
        if (vnode < 0) continue;
        if (infer_type(c, vnode) == TY_POLY) { cv->type = TY_POLY; changed = 1; }
      }
    }
    /* refresh the node-type cache so a `proc.call` node picks up the updated
       proc_ret (codegen reads comp_ntype, not lv->proc_ret directly). Re-infer
       reads scope-local types only -- it does not mutate them, so the widen
       stands. */
    for (int id = 0; id < nt->count; id++)
      infer_type(c, id);
  }


  /* Re-infer nodes inside instance_eval block bodies with the receiver's class
     context, so ivar reads get correct types in the final c->ntype cache.
     Call infer_type on each body statement: it recursively re-infers all
     sub-expressions (including ivar reads) and updates c->ntype. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty2 = nt_type(c->nt, id);
    if (!ty2 || !sp_streq(ty2, "CallNode")) continue;
    const char *nm2 = nt_str(c->nt, id, "name");
    if (!nm2) continue;
    int blk2 = nt_ref(c->nt, id, "block");
    int recv2 = nt_ref(c->nt, id, "receiver");
    if (blk2 < 0 || recv2 < 0) continue;
    TyKind rt2 = c->ntype[recv2];
    if (!ty_is_object(rt2)) continue;
    int is_ie2 = sp_streq(nm2, "instance_eval") || sp_streq(nm2, "instance_exec");
    if (is_ie2) {
      if (comp_method_in_chain(c, ty_object_class(rt2), nm2, NULL) >= 0) continue;
    }
    else if (!comp_trampoline_kind(c, ty_object_class(rt2), nm2, NULL)) continue;
    int bdy2 = nt_ref(c->nt, blk2, "body");
    if (bdy2 < 0) continue;
    int bn2 = 0; const int *bb2 = nt_arr(c->nt, bdy2, "body", &bn2);
    if (bn2 <= 0 || !bb2) continue;
    int saved2 = an_ie_class_id;
    an_ie_class_id = ty_object_class(rt2);
    for (int k2 = 0; k2 < bn2; k2++) infer_type(c, bb2[k2]);
    /* refresh the splice call's own type from the now-rebound body so a
       consumer (e.g. truthiness) sees the poly result, not the stale type
       computed before an_ie_class_id was bound. */
    infer_type(c, id);
    an_ie_class_id = saved2;
  }

  /* Promote `<<`-appended string locals to mutable strings (TY_STRBUF) so the
     append is amortized O(1) instead of an O(n) copy-concat (which makes a
     build-in-a-loop O(n^2)). This is a storage-only refinement: comp_ntype
     demotes it to TY_STRING, and every read hands out an immutable copy, so a
     STRBUF never escapes its sp_String wrapper. Parameters are excluded (their
     type is part of the function signature). Runs last, after the node-type
     cache is finalized, so reads keep their TY_STRING cache entry. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "<<")) continue;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv < 0 || !nt_type(c->nt, recv) ||
        !sp_streq(nt_type(c->nt, recv), "LocalVariableReadNode")) continue;
    if (c->ntype[recv] != TY_STRING) continue;   /* string append only */
    const char *vn = nt_str(c->nt, recv, "name");
    Scope *s = vn ? comp_scope_of(c, recv) : NULL;
    LocalVar *lv = s ? scope_local(s, vn) : NULL;
    if (!lv || lv->is_param || lv->type != TY_STRING) continue;
    /* A captured-and-celled string stays TY_STRING so it rides a typed-pointer
       cell: `<<` then reassigns through the cell (*_cell = concat(...)), which
       propagates to the enclosing scope. A STRBUF's in-place buffer + read-time
       demotion don't fit the cell's stable-pointer model. */
    if (lv->is_cell) continue;
    /* Only promote when every write to this local is a bare string literal:
       such a value is a fresh mutable string. A `.freeze`/`.dup`/method-result
       write may carry runtime frozen state that sp_String_new would discard,
       so `<<` would fail to raise FrozenError. Conservative but sound.
       Under `# frozen_string_literal: true` (the literal's `fzl` node flag,
       per file) the premise inverts -- the literal is FROZEN, and copying it
       into a writable sp_String would let `<<` mutate where CRuby raises
       FrozenError -- so a frozen contributing literal blocks the promotion
       and the value path's sp_str_check_mutable raises faithfully. */
    int all_literal_writes = 1, saw_write = 0, frozen_literal_write = 0;
    for (int w = 0; w < c->nt->count; w++) {
      const char *wty = nt_type(c->nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(c->nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != s) continue;
      saw_write = 1;
      int wv = nt_ref(c->nt, w, "value");
      const char *wvty = wv >= 0 ? nt_type(c->nt, wv) : NULL;
      if (!wvty || !sp_streq(wvty, "StringNode")) { all_literal_writes = 0; break; }
      if (nt_int(c->nt, wv, "fzl", 0)) { frozen_literal_write = 1; break; }
    }
    if (!saw_write || !all_literal_writes || frozen_literal_write) continue;
    /* Exclude vars used with a reassigning string mutator (replace/prepend/
       insert/clear or a bang method): codegen emits those as `recv = ...`,
       which needs a plain lvalue, not the copy a STRBUF read produces. */
    int has_reassign_mutate = 0;
    for (int u = 0; u < c->nt->count && !has_reassign_mutate; u++) {
      const char *uty = nt_type(c->nt, u);
      if (!uty || !sp_streq(uty, "CallNode")) continue;
      int urecv = nt_ref(c->nt, u, "receiver");
      if (urecv < 0 || !nt_type(c->nt, urecv) ||
          !sp_streq(nt_type(c->nt, urecv), "LocalVariableReadNode")) continue;
      const char *urn = nt_str(c->nt, urecv, "name");
      if (!urn || !sp_streq(urn, vn) || comp_scope_of(c, urecv) != s) continue;
      const char *un = nt_str(c->nt, u, "name");
      if (!un) continue;
      size_t ul = strlen(un);
      if (sp_streq(un, "replace") || sp_streq(un, "prepend") || sp_streq(un, "insert") ||
          sp_streq(un, "clear") || (ul > 0 && un[ul - 1] == '!'))
        has_reassign_mutate = 1;
    }
    if (has_reassign_mutate) continue;
    if (lv->rbs_seeded) continue;
    lv->type = TY_STRBUF;
  }

  /* Value-type object detection (Stage 1, conservative). A user class is
     represented by value (sp_X, no heap/GC) when it is a small, immutable,
     scalar-only leaf whose instances never need a heap pointer (never boxed,
     stored, passed, or captured). See reference_legacy_value_type_logic. */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_struct) continue;
    if (ci->def_node < 0) continue;                /* synthetic / Toplevel */
    if (!ci->name || sp_streq(ci->name, "Toplevel")) continue;
    if (ci->nivars < 1 || ci->nivars > 8) continue;
    if (ci->nwriters > 0) continue;
    if (ci->parent >= 0) continue;                 /* explicit user superclass */
    /* A class with any superclass node -- including a builtin like
       StandardError -- is not a standalone leaf, so it can't be a value
       type. The parent check above only catches user superclasses (a
       builtin parent leaves ci->parent == -1); without this, an Exception
       subclass with a scalar ivar was wrongly returned by value and never
       got a struct (#1415). */
    if (ci->def_node >= 0 && nt_ref(c->nt, ci->def_node, "superclass") >= 0) continue;
    int scalar = 1;
    for (int j = 0; j < ci->nivars; j++) {
      TyKind t = ci->ivar_types[j];
      /* int/float/bool need no GC; string fields are heap pointers but get
         GC-rooted per value-type local (the field slot is a stable root). */
      if (t != TY_INT && t != TY_FLOAT && t != TY_BOOL && t != TY_STRING) { scalar = 0; break; }
    }
    if (!scalar) continue;
    int has_sub = 0;
    for (int j = 0; j < c->nclasses; j++)
      if (c->classes[j].parent == i) { has_sub = 1; break; }
    if (has_sub) continue;
    ci->is_value_type = 1;   /* tentative; disqualified below */
  }
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty) continue;
    /* nil-witness: a slot holding `nil | W` encodes nil as the heap
       pointer's NULL; a by-value struct has no nil representation, so any
       nil witness on a W-typed slot disqualifies the value layout (#1686).
       The pointer form's NULL-nil machinery (and the nil-guard narrowing)
       then applies unchanged. */
    if (sp_streq(ty, "ReturnNode")) {
      int a2 = nt_ref(c->nt, id, "arguments");
      int an2 = 0;
      const int *av2 = a2 >= 0 ? nt_arr(c->nt, a2, "arguments", &an2) : NULL;
      int is_nil = (an2 == 0) ||
                   (nt_type(c->nt, av2[0]) && sp_streq(nt_type(c->nt, av2[0]), "NilNode"));
      if (is_nil) {
        Scope *s2 = comp_scope_of(c, id);
        if (s2 && ty_is_object(s2->ret)) {
          int q = ty_object_class(s2->ret);
          if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
        }
      }
    }
    if (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
        sp_streq(ty, "LocalVariableAndWriteNode")) {
      int v2 = nt_ref(c->nt, id, "value");
      if (v2 >= 0 && nt_type(c->nt, v2) && sp_streq(nt_type(c->nt, v2), "NilNode")) {
        const char *nm2 = nt_str(c->nt, id, "name");
        Scope *s2 = comp_scope_of(c, id);
        LocalVar *lv2 = (nm2 && s2) ? scope_local(s2, nm2) : NULL;
        if (lv2 && ty_is_object(lv2->type)) {
          int q = ty_object_class(lv2->type);
          if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
        }
      }
    }
    if (sp_streq(ty, "InstanceVariableWriteNode") || sp_streq(ty, "InstanceVariableOrWriteNode")) {
      int v2 = nt_ref(c->nt, id, "value");
      if (v2 >= 0 && nt_type(c->nt, v2) && sp_streq(nt_type(c->nt, v2), "NilNode")) {
        Scope *s2 = comp_scope_of(c, id);
        const char *ivn = nt_str(c->nt, id, "name");
        if (s2 && s2->class_id >= 0 && ivn) {
          int ix = comp_ivar_index(&c->classes[s2->class_id], ivn);
          TyKind it2 = ix >= 0 ? c->classes[s2->class_id].ivar_types[ix] : TY_UNKNOWN;
          if (ty_is_object(it2)) {
            int q = ty_object_class(it2);
            if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
          }
        }
      }
    }
    if (sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode")) {
      TyKind t2 = comp_ntype(c, id);
      if (ty_is_object(t2)) {
        /* a W-valued conditional with a nil arm (x = cond ? W.new : nil):
           if either arm's tail is nil -- or the else arm is absent -- the
           expression carries nil */
        int nil_arm = 0;
        int arms[2];
        arms[0] = nt_ref(c->nt, id, "statements");
        arms[1] = nt_ref(c->nt, id, sp_streq(ty, "IfNode") ? "subsequent" : "else_clause");
        if (arms[1] < 0) nil_arm = 1;
        for (int ai = 0; ai < 2 && !nil_arm; ai++) {
          int an3 = arms[ai];
          if (an3 < 0) continue;
          const char *aty = nt_type(c->nt, an3);
          if (aty && sp_streq(aty, "ElseNode")) an3 = nt_ref(c->nt, an3, "statements");
          const char *aty2 = an3 >= 0 ? nt_type(c->nt, an3) : NULL;
          if (aty2 && sp_streq(aty2, "StatementsNode")) {
            int bn3 = 0;
            const int *bb3 = nt_arr(c->nt, an3, "body", &bn3);
            an3 = bn3 > 0 ? bb3[bn3 - 1] : -1;
          }
          if (an3 < 0) { nil_arm = 1; break; }
          const char *lty = nt_type(c->nt, an3);
          if (lty && sp_streq(lty, "NilNode")) nil_arm = 1;
        }
        if (nil_arm) {
          int q = ty_object_class(t2);
          if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
        }
      }
    }
    if (sp_streq(ty, "OptionalParameterNode") || sp_streq(ty, "OptionalKeywordParameterNode")) {
      int v2 = nt_ref(c->nt, id, "value");
      if (v2 >= 0 && nt_type(c->nt, v2) && sp_streq(nt_type(c->nt, v2), "NilNode")) {
        const char *nm2 = nt_str(c->nt, id, "name");
        Scope *s2 = comp_scope_of(c, id);
        LocalVar *lv2 = (nm2 && s2) ? scope_local(s2, nm2) : NULL;
        if (lv2 && ty_is_object(lv2->type)) {
          int q = ty_object_class(lv2->type);
          if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
        }
      }
    }
    /* immutable check: an ivar write outside `initialize` defeats value type */
    if (sp_streq(ty, "InstanceVariableWriteNode") ||
        sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
        sp_streq(ty, "InstanceVariableTargetNode")) {
      Scope *s = comp_scope_of(c, id);
      if (s && s->class_id >= 0 && s->class_id < c->nclasses &&
          c->classes[s->class_id].is_value_type &&
          (!s->name || !sp_streq(s->name, "initialize")))
        c->classes[s->class_id].is_value_type = 0;
    }
    /* unsafe uses that would need a heap pointer / boxing (void* slot) */
    if (sp_streq(ty, "ArrayNode") || sp_streq(ty, "HashNode") ||
        sp_streq(ty, "KeywordHashNode")) {
      int n = 0; const int *els = nt_arr(c->nt, id, "elements", &n);
      for (int k = 0; k < n; k++) {
        TyKind et = comp_ntype(c, els[k]);
        if (ty_is_object(et)) { int q = ty_object_class(et); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
      }
    }
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(c->nt, id, "name");
      int recv = nt_ref(c->nt, id, "receiver");
      if (nm && sp_streq(nm, "method") && recv >= 0) {
        TyKind rt = comp_ntype(c, recv);
        if (ty_is_object(rt)) { int q = ty_object_class(rt); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
      }
      /* `method(:foo)` with no receiver captures `self`; a bound Method needs a
         stable heap pointer, so the enclosing class can't be a value type. */
      if (nm && sp_streq(nm, "method") && recv < 0) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->class_id >= 0 && s->class_id < c->nclasses) c->classes[s->class_id].is_value_type = 0;
      }
      /* instance_eval/exec lifts the block body into a method on the receiver
         that needs a by-pointer self; exclude such receivers. */
      if (nm && (sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec")) && recv >= 0) {
        TyKind rt = comp_ntype(c, recv);
        if (ty_is_object(rt)) { int q = ty_object_class(rt); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
      }
      int args = nt_ref(c->nt, id, "arguments"); int an = 0;
      const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &an) : NULL;
      for (int k = 0; k < an; k++) {
        TyKind at = comp_ntype(c, av[k]);
        if (ty_is_object(at)) { int q = ty_object_class(at); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
      }
    }
    if (sp_streq(ty, "InstanceVariableWriteNode") || sp_streq(ty, "GlobalVariableWriteNode") ||
        sp_streq(ty, "ConstantWriteNode")) {
      int v = nt_ref(c->nt, id, "value");
      if (v >= 0) { TyKind vt = comp_ntype(c, v); if (ty_is_object(vt)) { int q = ty_object_class(vt); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; } }
    }
    /* a value-type instance written into a poly-typed local would be boxed */
    if (sp_streq(ty, "LocalVariableWriteNode")) {
      int v = nt_ref(c->nt, id, "value");
      if (v >= 0) {
        TyKind vt = comp_ntype(c, v);
        if (ty_is_object(vt)) {
          const char *vn = nt_str(c->nt, id, "name");
          Scope *s = comp_scope_of(c, id);
          LocalVar *lv = (vn && s) ? scope_local(s, vn) : NULL;
          if (lv && lv->type != vt) { int q = ty_object_class(vt); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
        }
      }
    }
    /* A nil assignment to a value-object-typed slot makes it nullable. Value
       types are stack values with no NULL encoding, so the class must be a
       heap object instead. (ty_unify keeps an object type when it also sees
       nil; this disqualifies the value-type representation for such a class.) */
    if (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "InstanceVariableWriteNode")) {
      int v = nt_ref(c->nt, id, "value");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "NilNode")) {
        TyKind st = TY_UNKNOWN;
        Scope *s = comp_scope_of(c, id);
        const char *nm = nt_str(c->nt, id, "name");
        if (sp_streq(ty, "LocalVariableWriteNode")) {
          LocalVar *lv = (nm && s) ? scope_local(s, nm) : NULL;
          if (lv) st = lv->type;
        }
        else {
          int cid2 = s ? s->class_id : -1;
          if (cid2 < 0 && c->node_cbody[id] >= 0) cid2 = c->node_cbody[id];
          if (cid2 >= 0 && cid2 < c->nclasses && nm) {
            int iv = comp_ivar_index(&c->classes[cid2], nm);
            if (iv >= 0) st = c->classes[cid2].ivar_types[iv];
          }
        }
        if (ty_is_object(st)) { int q = ty_object_class(st); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
      }
    }
  }
  /* An instance built inside a poly-returning method is liable to be boxed at
     the (poly) return -- sp_box_obj would carry a stack pointer. And an
     instance that is a block's result is collected (map/collect -> a boxed
     array). Both box a value, so exclude such classes. */
  for (int id = 0; id < c->nt->count; id++) {
    TyKind t = comp_ntype(c, id);
    if (!ty_is_object(t)) continue;
    int q = ty_object_class(t);
    if (q < 0 || q >= c->nclasses || !c->classes[q].is_value_type) continue;
    Scope *s = comp_scope_of(c, id);
    /* poly scalar return boxes; a tuple return (POLY_ARRAY, e.g. `return a, b, c`)
       boxes each element. Either way a value instance here would be boxed. */
    if (s && (s->ret == TY_POLY || s->ret == TY_POLY_ARRAY)) c->classes[q].is_value_type = 0;
  }
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "BlockNode")) continue;
    int body = nt_ref(c->nt, id, "body");
    if (body < 0) continue;
    int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
    if (n <= 0) continue;
    TyKind lt = comp_ntype(c, st[n - 1]);
    if (ty_is_object(lt)) { int q = ty_object_class(lt); if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0; }
  }

  /* Reconcile a hash literal's node type with the variable it initializes.
     A literal like `{ begin: ... }` infers a narrow variant (SYM_POLY_HASH)
     from its own keys, but the variable may later be promoted to a wider hash
     (e.g. POLY_POLY_HASH from a mixed-key `dic[sym_or_int] = ...`). Codegen
     emits the literal from its own node type, so without this the constructor
     (sp_SymPolyHash_new) disagrees with the variable's declared C type
     (sp_PolyPolyHash *) -- an incompatible-pointer assignment that corrupts
     the hash at runtime. Align the literal to the variable's type so the right
     constructor and key/value boxing are emitted. Widen only (the variable
     type is the union over all writes, so it is never narrower). */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty) continue;
    int is_lv = sp_streq(ty, "LocalVariableWriteNode") ||
                sp_streq(ty, "LocalVariableOrWriteNode") ||
                sp_streq(ty, "LocalVariableAndWriteNode") ||
                sp_streq(ty, "LocalVariableOperatorWriteNode");
    int is_iv = sp_streq(ty, "InstanceVariableWriteNode") ||
                sp_streq(ty, "InstanceVariableOrWriteNode") ||
                sp_streq(ty, "InstanceVariableAndWriteNode") ||
                sp_streq(ty, "InstanceVariableOperatorWriteNode");
    if (!is_lv && !is_iv) continue;
    int v = nt_ref(c->nt, id, "value");
    if (v < 0) continue;
    const char *vty = nt_type(c->nt, v);
    /* `x ||= expr || {}`: the literal hides one level down in an or/and
       arm; align that arm instead (doom's `@gfx_weapons ||= ...weapons
       || {}`, whose {} otherwise defaulted to StrPolyHash against a
       Sym-keyed slot). */
    if (vty && (sp_streq(vty, "OrNode") || sp_streq(vty, "AndNode"))) {
      int alt = nt_ref(c->nt, v, "right");
      const char *aty = alt >= 0 ? nt_type(c->nt, alt) : NULL;
      if (aty && (sp_streq(aty, "HashNode") || sp_streq(aty, "KeywordHashNode"))) { v = alt; vty = aty; }
    }
    if (!vty || (!sp_streq(vty, "HashNode") && !sp_streq(vty, "KeywordHashNode"))) continue;
    TyKind litt = c->ntype[v];
    /* an EMPTY literal often has no inferred type at all (TY_UNKNOWN) --
       `@near_linedefs ||= {}` -- and can adopt any variant, so let it
       through; the widen-only guard below still applies. */
    int lit_n = 0; nt_arr(c->nt, v, "elements", &lit_n);
    if (!ty_is_hash(litt) && !(lit_n == 0 && (litt == TY_UNKNOWN || litt == TY_POLY))) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm) continue;
    TyKind dstt = TY_UNKNOWN;
    if (is_lv) {
      Scope *s = comp_scope_of(c, id);
      LocalVar *lv = s ? scope_local(s, nm) : NULL;
      if (lv) dstt = lv->type;
    }
    else {
      Scope *s = comp_scope_of(c, id);
      if (s && s->class_id >= 0 && s->class_id < c->nclasses) {
        int iv = comp_ivar_index(&c->classes[s->class_id], nm);
        if (iv >= 0) dstt = c->classes[s->class_id].ivar_types[iv];
      }
    }
    /* Only widen toward POLY_POLY_HASH, the universally-boxed variant whose
       constructor accepts any keys/values -- a safe target for any narrower
       literal. Other cross-variant widenings are left alone. */
    if (dstt == TY_POLY_POLY_HASH && litt != TY_POLY_POLY_HASH)
      c->ntype[v] = dstt;
    /* an empty literal adopts ANY destination hash variant (nothing to
       re-box), fixing e.g. a Sym-keyed slot initialized with `... || {}` */
    else if (lit_n == 0 && ty_is_hash(dstt) && litt != dstt)
      c->ntype[v] = dstt;
  }

  /* Final safety pass: a hash-typed LOCAL assigned from a plain TY_POLY
     value, or from a reader whose backing ivar finished on a different
     type, cannot assume its variant -- codegen would cast the boxed
     pointer to the inferred variant and index a different struct layout
     (doom's `opts = @menu.options`: a PolyPolyHash_get over the actual
     SymPolyHash object segfaulted in respawn_player). Runs LAST so it
     sees final ivar types; widens the local to TY_POLY and repoints the
     node-type cache so subscripts go through the runtime dispatch. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *nty = nt_type(c->nt, id);
    if (!nty || !sp_streq(nty, "LocalVariableWriteNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    Scope *lsc = comp_scope_of(c, id);
    LocalVar *lv = nm && lsc ? scope_local(lsc, nm) : NULL;
    if (!lv || !ty_is_hash(lv->type)) continue;
    int v = nt_ref(c->nt, id, "value");
    if (v < 0) continue;
    const char *vty2 = nt_type(c->nt, v);
    if (vty2 && (sp_streq(vty2, "HashNode") || sp_streq(vty2, "KeywordHashNode"))) continue;
    int widen2 = (comp_ntype(c, v) == TY_POLY);
    if (!widen2 && vty2 && sp_streq(vty2, "CallNode")) {
      int vrecv = nt_ref(c->nt, v, "receiver");
      int vargs = nt_ref(c->nt, v, "arguments");
      int vac = 0; if (vargs >= 0) nt_arr(c->nt, vargs, "arguments", &vac);
      const char *vnm = nt_str(c->nt, v, "name");
      if (vrecv >= 0 && vac == 0 && vnm && nt_ref(c->nt, v, "block") < 0) {
        TyKind rt3 = comp_ntype(c, vrecv);
        for (int ci3 = 0; ci3 < c->nclasses && !widen2; ci3++) {
          if (ty_is_object(rt3) && ty_object_class(rt3) != ci3) continue;
          if (!ty_is_object(rt3) && rt3 != TY_POLY) break;
          if (!ty_is_object(rt3) && !c->classes[ci3].instantiated) continue;
          int pdc3 = -1;
          if (!comp_reader_in_chain(c, ci3, vnm, &pdc3)) continue;
          char ivn3[300]; snprintf(ivn3, sizeof ivn3, "@%s", comp_resolve_alias(c, pdc3, vnm));
          int ix3 = comp_ivar_index(&c->classes[pdc3], ivn3);
          if (ix3 >= 0 && c->classes[pdc3].ivar_types[ix3] != lv->type) widen2 = 1;
        }
      }
    }
    if (widen2) {
      lv->type = TY_POLY;
      /* repoint the cache for this local's read/write nodes in this scope */
      for (int nid2 = 0; nid2 < c->nt->count; nid2++) {
        const char *t4 = nt_type(c->nt, nid2);
        if (!t4) continue;
        if (!sp_streq(t4, "LocalVariableReadNode") && !sp_streq(t4, "LocalVariableWriteNode") &&
            !sp_streq(t4, "LocalVariableTargetNode") && !sp_streq(t4, "LocalVariableOrWriteNode")) continue;
        const char *n4 = nt_str(c->nt, nid2, "name");
        if (!n4 || !sp_streq(n4, nm)) continue;
        if (comp_scope_of(c, nid2) != lsc) continue;
        if (ty_is_hash(c->ntype[nid2])) c->ntype[nid2] = TY_POLY;
      }
    }
  }
}

