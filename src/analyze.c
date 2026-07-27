#include "analyze_internal.h"

/* --int-overflow=promote flag; see analyze.h. Default off. */
int g_promote_mode = 0;

/* Post-convergence bind pass: lets an empty array-literal argument fill a
   parameter that stayed UNKNOWN through the fixpoint (see bind_call_params);
   applied only after convergence so any concrete kind wins first. */
int g_final_bind_pass = 0;
/* Set while the type fixpoint iterates: a pass that would otherwise GUESS a
   type from ambiguous or not-yet-arrived evidence must instead leave the slot
   at UNKNOWN (bottom) and wait. "Not yet known" is not "could be anything",
   but the monotonic passes cannot tell the difference after the fact --
   parameter and return types only ever widen, so a guess made on the first
   iteration, when nothing has a type yet, is permanent. It then cascades: the
   caller's slot unifies int-array with the guess and lands on the scalar poly,
   the callee's element reads go poly, and every arithmetic helper downstream
   boxes. Cleared once the fixpoint converges and iteration continues, so a
   slot whose evidence really never arrives still takes the pessimistic type. */
int g_infer_optimistic = 0;

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

/* Collect the single user-exception class raised in a subtree (not crossing
   DefNode). *cid accumulates; *bad is set when a raise doesn't name one
   ivar-carrying user exception class, or two raises disagree. */
static void scan_raise_classes(Compiler *c, int id, int *cid, int *bad) {
  if (id < 0 || *bad) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "DefNode")) return;
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, "raise") && nt_ref(nt, id, "receiver") < 0) {
      int a = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      const char *cn = NULL;
      if (an >= 1) {
        const char *aty = nt_type(nt, av[0]);
        if (aty && sp_streq(aty, "ConstantReadNode")) cn = nt_str(nt, av[0], "name");
        else if (aty && sp_streq(aty, "CallNode") && nt_str(nt, av[0], "name") &&
                 sp_streq(nt_str(nt, av[0], "name"), "new")) {
          int rr = nt_ref(nt, av[0], "receiver");
          if (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "ConstantReadNode"))
            cn = nt_str(nt, rr, "name");
        }
      }
      int rcid = cn ? comp_class_index(c, cn) : -1;
      if (rcid < 0 || !class_is_exc_subclass(c, rcid) || c->classes[rcid].nivars <= 0) { *bad = 1; return; }
      if (*cid >= 0 && *cid != rcid) { *bad = 1; return; }
      *cid = rcid;
      return;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) scan_raise_classes(c, nt_ref_at(nt, id, i), cid, bad);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) scan_raise_classes(c, ids[k], cid, bad);
  }
}

/* Bare `rescue => e`: when every raise in the guarded begin body constructs
   the same ivar-carrying user exception class, specialize the binding to that
   class like a typed arm would. The emitted binder guards the carried-object
   cast with a class match, so a foreign StandardError arriving at the bare
   arm still binds a safe zero-ivar struct. */
static int bare_rescue_spec_cid(Compiler *c, int rescue_id) {
  const NodeTable *nt = c->nt;
  int nexc = 0;
  nt_arr(nt, rescue_id, "exceptions", &nexc);
  if (nexc != 0) return -1;
  NT_FOREACH_KIND(nt, NK_BeginNode, bid) {
    int arm = nt_ref(nt, bid, "rescue_clause");
    int hit = 0;
    for (int r = arm; r >= 0; r = nt_ref(nt, r, "subsequent"))
      if (r == rescue_id) { hit = 1; break; }
    if (!hit) continue;
    int cid = -1, bad = 0;
    scan_raise_classes(c, nt_ref(nt, bid, "statements"), &cid, &bad);
    return (!bad && cid >= 0) ? cid : -1;
  }
  return -1;
}

void compute_reachable(Compiler *c) {
  /* Build per-scope call sets (CallNode names, not entering nested DefNodes). */
  char ***scope_calls = calloc((size_t)c->nscopes, sizeof(char **));
  int   *sc_n        = calloc((size_t)c->nscopes, sizeof(int));
  int   *sc_cap      = calloc((size_t)c->nscopes, sizeof(int));
  for (int s = 0; s < c->nscopes; s++) {
    if (c->scopes[s].body >= 0)
      cr_collect_calls(c, c->nt, c->scopes[s].body, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    /* Also scan parameter defaults (e.g. def foo(opt = bar)) — these emit calls
       within the method scope but live in the DefNode parameters subtree. */
    if (c->scopes[s].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[s].def_node, "parameters");
      if (pn >= 0)
        cr_collect_calls(c, c->nt, pn, &scope_calls[s], &sc_n[s], &sc_cap[s]);
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

  /* case/in array and hash patterns may call a user object's #deconstruct /
     #deconstruct_keys, which have no explicit call site in the AST. If any such
     pattern exists, mark those methods reachable (dead ones are stripped). */
  {
    int has_arr_pat = 0, has_hash_pat = 0;
    for (int id = 0; id < c->nt->count; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty) continue;
      if (sp_streq(ty, "ArrayPatternNode") || sp_streq(ty, "FindPatternNode")) has_arr_pat = 1;
      else if (sp_streq(ty, "HashPatternNode")) has_hash_pat = 1;
    }
    if (has_arr_pat) MARK_NAME("deconstruct");
    if (has_hash_pat) MARK_NAME("deconstruct_keys");
    if (has_arr_pat || has_hash_pat)
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
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
      else if (comp_ntype(c, recv) == TY_CLASS || comp_ntype(c, recv) == TY_POLY) {
        /* `klass.new` on a dynamic Class value -- typed TY_CLASS, or a poly
           value that is a Class at run time (`REG["c"].new`, #2888): the class
           is unresolvable at compile time, so keep every class instantiated
           (its constructor and methods must exist for the runtime dispatch). */
        disable = 1; break;
      }
    }
    /* bare `new(...)` (no receiver) inside a class/singleton method (`def
       self.foo; ...; new(...); end`) is implicit `self.new(...)`, i.e. the
       enclosing class -- e.g. doom's `Texture.parse_texture` builds each
       Texture via a bare `new(name, ...)`. Missing this meant such a
       class's `instantiated` flag never got set unless some other call
       site also did `Texture.new` explicitly -- so a poly-array element
       member-read dispatch silently dropped its case arm, reading back
       nil/0 for every field of an otherwise fully-constructed object.
       Class methods only: inside an *instance* method bare `new` is not
       Class#new (Ruby raises NameError unless a method `new` is in
       scope), so it must not mark the class instantiated. */
    if (recv < 0 && sp_streq(name, "new")) {
      Scope *encl = comp_scope_of(c, id);
      if (encl && encl->class_id >= 0 && encl->is_cmethod)
        c->classes[encl->class_id].instantiated = 1;
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
  /* A yield in a lowered method reads the forwarded block, but through a
     YieldNode rather than a variable read, so the capture scan walked past it
     and a lifted body calling the block had nothing to capture (#3355). */
  if (sp_streq(ty, "YieldNode")) {
    Scope *ys = comp_scope_of(c, id);
    if (ys && ys->is_lowered_yield && ys->blk_param && ys->blk_param[0])
      aname_add(out, ys->blk_param);
  }
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
  /* a literal block on a first-class proc's .call is lifted onto the
     _sp_proc_blk side-channel as a real proc (#2648), so its captures need
     cells exactly like any other escaping block */
  if ((sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]")) &&
      recv >= 0 && infer_type(c, recv) == TY_PROC) return 1;
  int mi = -1;
  if (recv < 0) {
    mi = comp_method_index(c, name);
    if (mi < 0) {
      Scope *self = comp_scope_of(c, id);
      if (self && self->class_id >= 0) {
        mi = comp_method_in_chain(c, self->class_id, name, NULL);
        /* an implicit-self call inside a class method resolves to a CLASS
           method; its literal block is lifted all the same (#2444) */
        if (mi < 0) mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      }
    }
  }
else {
    const char *rty = nt_type(nt, recv);
    /* `Klass.cmeth { }` / `Mod::Sub.cmeth { }`: a class/module method keeps a
       real &block the same way an instance method does, so its block is lifted
       and captures enclosing locals. (Was omitted -- only ty_is_object was
       handled -- so a block passed to a module method never celled its
       captures, silently dropping writes to them.) */
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) mi = comp_cmethod_in_chain(c, ci, name, NULL);
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
    }
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
  return is_proc_create(c, id) || a_block_is_lifted(c, id) ||
         a_is_fiber_or_gen_create(c, id) || is_handler_proc_block(c, id);
}

/* Does the subtree rooted at `root` contain node `id`? Bounded recursion over
   every ref/arr field. */
static int a_subtree_contains(const NodeTable *nt, int root, int id, int depth) {
  if (root < 0 || root >= nt->count || depth > 300) return 0;
  if (root == id) return 1;
  const SpNode *nd = &nt->nodes[root];
  for (int i = 0; i < nd->nr; i++)
    if (a_subtree_contains(nt, nd->r[i].ref, id, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (a_subtree_contains(nt, nd->a[i].ids[j], id, depth + 1)) return 1;
  return 0;
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
    /* keyword params bind BOXED (extracted from the call-site kwargs hash),
       so their static type is poly -- an untyped one lets the body's tail
       mis-derive (a `[a, k]` settled INT_ARRAY and misread the PolyArray,
       #2728). Seeded: the write pass's reset must not wipe it. */
    {
      int kn2 = 0; const int *kws2 = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn2) : NULL;
      Scope *pbs2 = kn2 > 0 ? comp_scope_of(c, id) : NULL;
      for (int k2 = 0; k2 < kn2 && pbs2; k2++) {
        const char *kname2 = nt_str(nt, kws2[k2], "name");
        LocalVar *klv2 = kname2 ? scope_local_intern(pbs2, kname2) : NULL;
        if (klv2 && klv2->type == TY_UNKNOWN) { klv2->type = TY_POLY; klv2->rbs_seeded = 1; }
      }
      /* `**kw` binds the whole boxed kwargs hash: poly likewise */
      int kwrest2 = pn >= 0 ? nt_ref(nt, pn, "keyword_rest") : -1;
      const char *kwrty2 = kwrest2 >= 0 ? nt_type(nt, kwrest2) : NULL;
      if (kwrty2 && sp_streq(kwrty2, "KeywordRestParameterNode")) {
        const char *krn2 = nt_str(nt, kwrest2, "name");
        Scope *pbs3 = comp_scope_of(c, id);
        LocalVar *krv2 = (krn2 && pbs3) ? scope_local_intern(pbs3, krn2) : NULL;
        if (krv2 && krv2->type == TY_UNKNOWN) { krv2->type = TY_POLY; krv2->rbs_seeded = 1; }
      }
    }
    /* ENV's block mutators (delete_if/keep_if/...) pass (key, value) string
       pairs; seed the params so the proc binds them as strings (#2832) */
    {
      const char *cnm2 = nt_str(nt, id, "name");
      int crecv2 = nt_ref(nt, id, "receiver");
      if (cnm2 && crecv2 >= 0 && nt_kind(nt, crecv2) == NK_ConstantReadNode &&
          nt_str(nt, crecv2, "name") && sp_streq(nt_str(nt, crecv2, "name"), "ENV")) {
        int ern = 0; const int *ereqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &ern) : NULL;
        Scope *epbs = comp_scope_of(c, id);
        for (int ek = 0; ek < ern && ek < 2 && epbs; ek++) {
          const char *ernm = nt_str(nt, ereqs[ek], "name");
          LocalVar *erlv = ernm ? scope_local_intern(epbs, ernm) : NULL;
          if (erlv && (erlv->type == TY_UNKNOWN || erlv->type == TY_INT)) {
            erlv->type = TY_STRING;
            erlv->rbs_seeded = 1;
          }
        }
      }
    }
    /* a named &block param binds an sp_Proc* from the block side-channel:
       type it so `b.call(...)` rides the TY_PROC dispatch (#2648) */
    {
      int bpar = pn >= 0 ? nt_ref(nt, pn, "block") : -1;
      const char *bpty = bpar >= 0 ? nt_type(nt, bpar) : NULL;
      if (bpty && sp_streq(bpty, "BlockParameterNode")) {
        const char *bpn = nt_str(nt, bpar, "name");
        Scope *pbs = comp_scope_of(c, id);
        LocalVar *blv = (bpn && pbs) ? scope_local_intern(pbs, bpn) : NULL;
        if (blv && blv->type == TY_UNKNOWN) { blv->type = TY_PROC; blv->is_block_param = 1; }
        if (bpn) aname_add(&params, bpn);
      }
    }
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
      /* a param of an ENCLOSING proc: it lives in that proc's frame (a "later
         slice"), and this proc closes over it -- the enclosing proc's prologue
         materializes the cell (#2648). Only proc-fn enclosers: an inlined
         iterator block's params bind in the loop, where no cell exists yet. */
      if (!owned) {
        for (int q = 0; q < nt->count && !owned; q++) {
          if (q == id || !is_proc_create(c, q)) continue;
          int qpn = a_proc_params_node(c, q);
          int qrn = 0; const int *qreqs = qpn >= 0 ? nt_arr(nt, qpn, "requireds", &qrn) : NULL;
          int has = 0;
          for (int k = 0; k < qrn && !has; k++) {
            const char *qn = nt_str(nt, qreqs[k], "name");
            if (qn && sp_streq(qn, nm)) has = 1;
          }
          if (!has) continue;
          int qb = a_proc_body(c, q);
          if (qb >= 0 && a_subtree_contains(nt, qb, id, 0)) owned = 1;
        }
      }
      if (owned) {
        /* A fiber/generator now cells a captured heap object too (string /
           array / hash / object), via a typed-pointer cell, so a reassignment
           in the body reaches the enclosing scope. In-place mutation of a
           non-reassigned capture stays by pointer (the var isn't `owned`, so it
           never reaches here). Value-type objects have no stable pointer. */
        /* an sp_Proc* is a heap pointer with a stable identity, so a fiber or
           thread body can hold it in a typed cell -- which is what a lowered
           method's forwarded block needs to reach a yield inside such a body */
        int heap_ptr = (lv->type == TY_STRING || lv->type == TY_STRBUF ||
                        lv->type == TY_PROC ||
                        ty_is_array(lv->type) ||
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

/* `ENV` is compile-time modeled only as a call RECEIVER (`ENV[...]`,
   `ENV.fetch`, ...). Flowing it as a VALUE -- an argument, a parameter
   default, an assignment -- would smuggle a class-typed value into
   hash-typed uses and mis-emit C. Reject loudly with the read-at-the-
   use-site convention instead. defined?(ENV) evaluates nothing and is
   exempt. */
static void reject_env_value_uses(Compiler *c) {
  const NodeTable *nt = c->nt;
  char *ok = calloc((size_t)nt->count, 1);
  if (!ok) { perror("calloc"); exit(1); }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int r = nt_ref(nt, id, "receiver");
    if (r >= 0) ok[r] = 1;
  }
  NT_FOREACH_KIND(nt, NK_DefinedNode, id) {
    int v = nt_ref(nt, id, "value");
    if (v >= 0) ok[v] = 1;
  }
  NT_FOREACH_KIND(nt, NK_ConstantReadNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "ENV") || ok[id]) continue;
    int ln  = (int)nt_int(nt, id, "node_line", 0);
    int fid = (int)nt_int(nt, id, "node_file", 0);
    const char *file = nt_file_path(nt, fid);
    if (!file || !*file) file = nt->source_file;
    fprintf(stderr, "spinel: %s:%d: `ENV` cannot flow as a value (it is "
                    "compile-time modeled as a call receiver only); read "
                    "ENV[\"KEY\"] at the use site and pass the string\n",
            file ? file : "source.rb", ln);
    exit(1);
  }
  free(ok);
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
    if (recv < 0 || !nt_type(nt, recv)) continue;
    const char *rvty = nt_type(nt, recv);
    /* A ConstantPathNode whose LEAF is the alias (`Outer::OC.five` where
       `OC = Util`) resolves the same way: constants register by leaf name,
       and a path receiver with a real class leaf already dispatches. */
    if (!sp_streq(rvty, "ConstantReadNode") && !sp_streq(rvty, "ConstantPathNode")) continue;
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
    if (v >= 0 && !class_ivar_pinned(ci, nm)) {  /* --rbs seed pins are authoritative */
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

/* True if `name` is used as a local write or block parameter OUTSIDE the given
   block body (stamp `gen`) -- i.e. a block-local of that name would shadow a
   real enclosing use, so it must be split into its own slot. */
/* Name index over the write/param node set. The collision checks below ask
   "is this name used outside the block body?", and scanning every write and
   parameter node per block parameter was O(blocks * params * n) -- 12.7% of a
   37k-line compile. Bucketing by name makes each check touch only the nodes
   that actually carry it. A rename during the pass changes node names, so the
   index is rebuilt lazily when that happens (renames are the exception). */
typedef struct { const int *wp; int wpn; int *head; int *next; unsigned mask; } BlkpIdx;

static unsigned blkp_name_hash(const char *s) {
  unsigned h = 2166136261u;
  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}
static void blkp_idx_build(BlkpIdx *ix, const NodeTable *nt) {
  for (unsigned b = 0; b <= ix->mask; b++) ix->head[b] = -1;
  for (int i = 0; i < ix->wpn; i++) {
    const char *nm = nt_str(nt, ix->wp[i], "name");
    if (!nm) { ix->next[i] = -1; continue; }
    unsigned b = blkp_name_hash(nm) & ix->mask;
    ix->next[i] = ix->head[b];
    ix->head[b] = i;
  }
}
static int blkp_idx_init(BlkpIdx *ix, const NodeTable *nt, const int *wp, int wpn) {
  unsigned nb = 16;
  while (nb < (unsigned)wpn * 2u && nb < (1u << 22)) nb <<= 1;
  ix->wp = wp; ix->wpn = wpn; ix->mask = nb - 1;
  ix->head = malloc((size_t)nb * sizeof(int));
  ix->next = malloc((size_t)(wpn > 0 ? wpn : 1) * sizeof(int));
  if (!ix->head || !ix->next) { free(ix->head); free(ix->next); ix->head = ix->next = NULL; return 0; }
  blkp_idx_build(ix, nt);
  return 1;
}
static void blkp_idx_free(BlkpIdx *ix) { free(ix->head); free(ix->next); ix->head = ix->next = NULL; }
/* Scan the wp entries bucketed under `name`. The index is built ONCE and never
   rebuilt: a rename only ever moves a node from its source name to an invented
   `x__bpN`, and callers re-read and compare the name, so an entry left in a
   stale bucket is simply rejected. The one shape that could MISS an entry is a
   query for an invented name, which falls back to scanning every entry. */
typedef struct { const BlkpIdx *ix; int all; int i; } BlkpScan;

static BlkpScan blkp_scan(const BlkpIdx *ix, const char *name) {
  BlkpScan sc;
  sc.ix = ix;
  sc.all = strstr(name, "__bp") != NULL;
  sc.i = sc.all ? 0 : ix->head[blkp_name_hash(name) & ix->mask];
  return sc;
}
static int blkp_scan_next(BlkpScan *sc, int *out) {
  if (sc->all) {
    if (sc->i >= sc->ix->wpn) return 0;
    *out = sc->ix->wp[sc->i++];
    return 1;
  }
  if (sc->i < 0) return 0;
  *out = sc->ix->wp[sc->i];
  sc->i = sc->ix->next[sc->i];
  return 1;
}

static int name_written_outside(const NodeTable *nt, const char *name,
                                BlkpIdx *ix, const int *inbody, int gen) {
  BlkpScan sc = blkp_scan(ix, name);
  int w;
  while (blkp_scan_next(&sc, &w)) {
    if (inbody[w] == gen) continue;
    const char *wn = nt_str(nt, w, "name");
    if (wn && sp_streq(wn, name)) return 1;
  }
  return 0;
}

/* True if `name` is bound as a parameter anywhere in the params subtree
   (leading / optional / rest / keyword / post / destructured), as opposed to a
   block-local (`; x`) shadow declaration. Block-locals share the BlockNode's
   comma-joined `locals` string with the real params, so the two must be told
   apart before a locals entry is treated as a shadow. */
static int params_bind_name(const NodeTable *nt, int id, const char *name) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    if (pn && sp_streq(pn, name)) return 1;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (params_bind_name(nt, nt_ref_at(nt, id, i), name)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) if (params_bind_name(nt, ids[j], name)) return 1;
  }
  return 0;
}

/* Rename any block-local (`; x`) whose name collides with a local used outside
   the block body so the shadow becomes a distinct slot, mirroring the block
   parameter rename below. The declaration lives only in the BlockNode's
   comma-joined `locals` string (spinel's parser carries no BlockLocalVariableNode
   array), which is rewritten in lock-step so emit_block_locals_reset nils the
   renamed slot rather than the enclosing one. */
static void rename_shadowing_block_locals(Compiler *c, int L, int pn, int body,
                                          const int *inbody, int gen,
                                          BlkpIdx *ix) {
  const NodeTable *nt = c->nt;
  const char *locs0 = nt_str(nt, L, "locals");
  if (!locs0 || !*locs0) return;
  /* copy: nt_set_str below frees locs0's storage while we still read it */
  size_t llen = strlen(locs0);
  char *locs = malloc(llen + 1);
  if (!locs) return;
  memcpy(locs, locs0, llen + 1);
  size_t ocap = llen + 32;
  char *out = malloc(ocap);
  if (!out) { free(locs); return; }
  out[0] = 0;
  size_t out_len = 0;
  int changed = 0;
  char *save = NULL;
  for (char *tok = strtok_r(locs, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char newn[176];
    const char *emit = tok;
    if (!params_bind_name(nt, pn, tok) &&
        name_written_outside(nt, tok, ix, inbody, gen)) {
      snprintf(newn, sizeof newn, "%s__bp%d", tok, L);
      blkp_rewrite_refs(c, body, tok, newn);
      emit = newn;
      changed = 1;
    }
    /* track out_len + memcpy so the append is O(1), keeping the whole rebuild
       O(N) rather than rescanning `out` with strlen/strcat each iteration. */
    size_t emit_len = strlen(emit);
    size_t need = out_len + emit_len + 2;
    if (need > ocap) {
      ocap = need * 2;
      char *t = realloc(out, ocap);
      if (!t) { free(out); free(locs); return; }
      out = t;
    }
    if (out_len > 0) out[out_len++] = ',';
    memcpy(out + out_len, emit, emit_len);
    out_len += emit_len;
    out[out_len] = '\0';
  }
  if (changed) nt_set_str((NodeTable *)nt, L, "locals", out);
  free(out);
  free(locs);
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
  BlkpIdx ix;
  if (!blkp_idx_init(&ix, nt, wp, wpn)) { free(wp); free(inbody); free(owner); return; }
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
    /* rest / post parameters shadow-rename the same way as the requireds: a
       rest param sharing an outer local's name shares its C slot, so the
       rest-packing assignment aliases the outer variable (a sole-rest block
       whose name matches the inlined method's own param read garbage). */
    int extras[129]; int ne = 0;
    {
      int rref = nt_ref(nt, pn, "rest");
      if (rref >= 0 && nt_type(nt, rref) && sp_streq(nt_type(nt, rref), "RestParameterNode") &&
          nt_str(nt, rref, "name")) extras[ne++] = rref;
      int pon = 0; const int *posts = nt_arr(nt, pn, "posts", &pon);
      for (int q = 0; q < pon && ne < 128; q++)
        if (posts[q] >= 0 && nt_type(nt, posts[q]) &&
            sp_streq(nt_type(nt, posts[q]), "RequiredParameterNode") &&
            nt_str(nt, posts[q], "name")) extras[ne++] = posts[q];
      int oon = 0; const int *opts = nt_arr(nt, pn, "optionals", &oon);
      for (int q = 0; q < oon && ne < 128; q++)
        if (opts[q] >= 0 && nt_type(nt, opts[q]) &&
            sp_streq(nt_type(nt, opts[q]), "OptionalParameterNode") &&
            nt_str(nt, opts[q], "name")) extras[ne++] = opts[q];
      int kwr = nt_ref(nt, pn, "keyword_rest");
      if (kwr >= 0 && ne < 128 && nt_type(nt, kwr) &&
          sp_streq(nt_type(nt, kwr), "KeywordRestParameterNode") &&
          nt_str(nt, kwr, "name")) extras[ne++] = kwr;
    }
    /* block-locals (`; a, b`) live only in the BlockNode's comma-joined `locals`
       string; a block may carry them with no required params (`{ |; x| ... }`). */
    const char *locs = nt_str(nt, L, "locals");
    int have_locals = locs && *locs;
    if (rn == 0 && ne == 0 && !have_locals) continue;
    int body = nt_ref(nt, L, "body");
    if (body < 0) continue;
    gen++;
    blkp_stamp_subtree(nt, body, inbody, gen);
    for (int ii = 0; ii < rn + ne; ii++) {
      int pnode = ii < rn ? reqs[ii] : extras[ii - rn];
      const char *p = nt_str(nt, pnode, "name");
      if (!p) continue;
      /* collision: the name is used outside this block's body -- as a local
         write/read or as another block's parameter (param-vs-param, e.g. two
         inject folds sharing `|a, b|` with different element types). Both pollute
         the shared LocalVar's type. */
      int collide = 0;
      BlkpScan sc = blkp_scan(&ix, p);
      int w;
      while (!collide && blkp_scan_next(&sc, &w)) {
        if (inbody[w] == gen) continue;
        const char *wty = nt_type(nt, w);
        int is_param_node = wty && sp_streq(wty, "RequiredParameterNode");
        /* don't let this block's own parameter nodes count as a collision */
        if (is_param_node) {
          int own = 0;
          for (int q = 0; q < rn; q++) if (reqs[q] == w) { own = 1; break; }
          for (int q = 0; q < ne && !own; q++) if (extras[q] == w) own = 1;
          if (own) continue;
        }
        const char *wn = nt_str(nt, w, "name");
        if (wn && sp_streq(wn, p)) collide = 1;
      }
      if (!collide) continue;
      char oldn[160], newn[176];
      snprintf(oldn, sizeof oldn, "%s", p);   /* copy: nt_set_str frees p's storage */
      snprintf(newn, sizeof newn, "%s__bp%d", oldn, L);
      nt_set_str((NodeTable *)nt, pnode, "name", newn);
      blkp_rewrite_refs(c, body, oldn, newn);
      /* an optional's default expression can reference a renamed sibling
         param (`|a, b=a|`); those reads live under the ParametersNode, not
         the body, so rewrite there too */
      blkp_rewrite_refs(c, pn, oldn, newn);
    }
    if (have_locals)
      rename_shadowing_block_locals(c, L, pn, body, inbody, gen, &ix);
  }
  blkp_idx_free(&ix);
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

static int seed_class_index(Compiler *c, const char *name);

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
  /* heterogeneous unions map to the bare poly tag (#1255); accepting the
     token pins the slot to sp_RbVal instead of silently dropping the seed */
  if (sp_streq(buf, "poly"))   return TY_POLY;
  if (sp_streq(buf, "int_array"))    return TY_INT_ARRAY;
  if (sp_streq(buf, "float_array"))  return TY_FLOAT_ARRAY;
  if (sp_streq(buf, "str_array"))    return TY_STR_ARRAY;
  /* the extractor emits poly_array for Array[T-outside-subset] (its header
     documents it), but this parser never accepted it -- those seeds were
     silently dropped */
  if (sp_streq(buf, "poly_array"))   return TY_POLY_ARRAY;
  if (sp_streq(buf, "str_int_hash"))   return TY_STR_INT_HASH;
  if (sp_streq(buf, "str_str_hash"))   return TY_STR_STR_HASH;
  if (sp_streq(buf, "int_int_hash"))   return TY_INT_INT_HASH;
  if (sp_streq(buf, "int_str_hash"))   return TY_INT_STR_HASH;
  if (sp_streq(buf, "sym_poly_hash"))  return TY_SYM_POLY_HASH;
  if (sp_streq(buf, "str_poly_hash"))  return TY_STR_POLY_HASH;
  if (sp_streq(buf, "poly_poly_hash")) return TY_POLY_POLY_HASH;
  if (!strncmp(buf, "obj_", 4)) {
    /* the full seed matcher, not a bare table lookup: an obj_ token names a
       class the same way a `class` seed line does (module-nested leaf,
       collision-renamed form), and must match the same set */
    int ci = seed_class_index(c, buf + 4);
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
  /* A qualify_colliding_classes-renamed link (`Mod__Leaf`) already embeds its
     full enclosing path in its stored name; prepending its enclosers again
     would duplicate the module prefix (`Red::Base::Inner` with a renamed
     `Red__Base` link would reconstruct as `Red_Red__Base_Inner`, which no
     extractor-emitted seed name can match). Start at the outermost renamed
     link instead. */
  int start = n - 1;
  for (int i = n - 1; i >= 0; i--) {
    const char *nm = c->classes[chain[i]].name;
    if (nm && strstr(nm, "__")) { start = i; break; }
  }
  size_t j = 0;
  for (int i = start; i >= 0; i--) {
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
/* Equal with every run of consecutive underscores treated as one (see the
   fallback in seed_class_index). */
static int seed_name_und_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a == '_' && *b == '_') {
      while (*a == '_') a++;
      while (*b == '_') b++;
      continue;
    }
    if (*a != *b) return 0;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

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
  /* Last resort: compare with underscore runs collapsed on both sides.
     qualify_colliding_classes renames a colliding leaf to the `Mod__Leaf`
     (double-underscore) form -- stored as the class's own name, with the
     enclosing-module link intact -- while the extractor's path join is a
     single underscore (`Mod_Leaf`), so no exact form above can match and
     the seeds for exactly the collision-prone classes were silently
     dropped. Compare both the stored name (renamed classes) and the
     reconstructed qualified name (renamed classes nested deeper) with
     underscore runs collapsed. A fallback only: it can never shadow an
     exact match. */
  for (int i = 0; i < c->nclasses; i++) {
    if (c->classes[i].name && seed_name_und_eq(c->classes[i].name, buf)) return i;
    char qn[256];
    class_qualified_name(c, i, qn, sizeof qn);
    if (seed_name_und_eq(qn, buf)) return i;
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

/* Drop an --rbs ivar pin (a layout conflict with an ancestor demoted it;
   see the cast-compatibility note in inherit_members). */
void class_unpin_ivar(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->n_rbs_pin_ivars; i++) {
    if (sp_streq(ci->rbs_pin_ivars[i], name)) {
      free(ci->rbs_pin_ivars[i]);
      ci->rbs_pin_ivars[i] = ci->rbs_pin_ivars[--ci->n_rbs_pin_ivars];
      return;
    }
  }
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

/* Does `name` belong to an override family -- defined on two related
   classes (one an ancestor of the other)? A return seed on any member
   would pin that member's C decl repr while poly-dispatch call sites
   keep the family (boxed) view: `sp_Base_s_instantiate` declared
   `sp_Base *` but unboxed `.v.p` at the call (#3203). Overridden
   methods keep their inferred return -- the same never-makes-it-worse
   rule the seed path applies to layout conflicts. Params still seed:
   they are per-scope and carry no cross-family repr. */
static int method_in_override_family(Compiler *c, int class_id,
                                     const char *name, int is_cmethod) {
  if (class_id < 0 || !name) return 0;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!!s->is_cmethod != !!is_cmethod) continue;
    if (!s->name || !sp_streq(s->name, name)) continue;
    if (s->class_id == class_id || s->class_id < 0) continue;
    for (int ci = s->class_id; ci >= 0; ci = c->classes[ci].parent)
      if (ci == class_id) return 1;
    for (int ci = class_id; ci >= 0; ci = c->classes[ci].parent)
      if (ci == s->class_id) return 1;
  }
  return 0;
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
        /* The extractor emits the name WITHOUT the sigil (`ivar w1 obj_Mat`),
           but ClassInfo interns parse-time ivars as `@w1`. Interning the bare
           token created a PHANTOM parallel ivar: the seed typed and pinned
           "w1" while every lookup asked about "@w1" -- so seeds never pinned
           the real ivar, and the phantom's static emission strips the first
           character (`ivars[j] + 1` assumes the sigil), colliding `w1`/`b1`
           into two `civ_..._1` statics of conflicting C types (the toy FFN
           double-emission). Normalize to the sigil form. */
        char ivn[300];
        snprintf(ivn, sizeof ivn, "%s%s", a1[0] == '@' ? "" : "@", a1);
        int idx = comp_ivar_intern(ci, ivn);
        sp_ivwatch(a1, "rbs_seed_pin", ci->ivar_types[idx], t);
        ci->ivar_types[idx] = t;
        class_pin_ivar(ci, ivn);
      }
    }
    else if (sp_streq(kw, "meth") && a1 && a2) {
      int class_id = (cur_ci == -2) ? -1 : cur_ci;
      if (cur_ci == -2 || cur_ci >= 0)
        seed_method(c, find_method_scope(c, class_id, a1, 0),
                    method_in_override_family(c, class_id, a1, 0) ? NULL : a2, a3);
    }
    else if (sp_streq(kw, "cmeth") && a1 && a2 && cur_ci >= 0) {
      seed_method(c, find_method_scope(c, cur_ci, a1, 1),
                  method_in_override_family(c, cur_ci, a1, 1) ? NULL : a2, a3);
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

/* If `id` is a `to_enum`/`enum_for` call, resolve the target method name into
   `buf` (the literal first symbol, `:each` by default, or the enclosing method
   name for `__method__`) and report the count of forwarded args after the
   symbol plus whether a size block is present. Returns 1 for a handled call.
   `recv.to_enum(:m, *a)` defers `recv.m(*a)`; the internal iterator is lowered
   per receiver kind by desugar_to_enum. */
static int to_enum_target(Compiler *c, int id, char *buf, int buflen,
                          int *out_extra, int *out_has_block) {
  const NodeTable *nt = c->nt;
  if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "enum_for") && !sp_streq(nm, "to_enum"))) return 0;
  if (out_has_block) *out_has_block = nt_ref(nt, id, "block") >= 0;
  int args = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  const char *mname = NULL; int extra = 0;
  if (ac == 0) { mname = "each"; }             /* to_enum/enum_for default to :each */
  else {
    const char *aty = nt_type(nt, av[0]);
    if (aty && sp_streq(aty, "SymbolNode")) mname = nt_str(nt, av[0], "value");
    else if (aty && sp_streq(aty, "CallNode") && nt_ref(nt, av[0], "receiver") < 0) {
      const char *an = nt_str(nt, av[0], "name");
      if (an && sp_streq(an, "__method__")) {  /* enum_for(__method__) -> this method */
        Scope *es = comp_scope_of(c, id);
        mname = es ? es->name : NULL;
      }
    }
    if (!mname) return 0;                       /* a dynamic method symbol: not handled */
    extra = ac - 1;
  }
  if (!mname) return 0;
  snprintf(buf, buflen, "%s", mname);
  if (out_extra) *out_extra = extra;
  return 1;
}

/* Small synthetic-AST constructors for the to_enum generator helper. */
static int te_lvread(NodeTable *nt, const char *name) {
  int n = nt_new_node(nt, "LocalVariableReadNode"); nt_node_set_str(nt, n, "name", name); return n;
}
static int te_int(NodeTable *nt, long long v) {
  int n = nt_new_node(nt, "IntegerNode"); nt_node_set_int(nt, n, "value", v); return n;
}
static int te_const(NodeTable *nt, const char *name) {
  int n = nt_new_node(nt, "ConstantReadNode"); nt_node_set_str(nt, n, "name", name); return n;
}
static int te_args1(NodeTable *nt, int a) {
  int n = nt_new_node(nt, "ArgumentsNode"); nt_node_set_arr(nt, n, "arguments", &a, 1); return n;
}
static int te_call(NodeTable *nt, int recv, const char *name, int argsnode, int block) {
  int n = nt_new_node(nt, "CallNode"); nt_node_set_str(nt, n, "name", name);
  if (recv >= 0) nt_node_set_ref(nt, n, "receiver", recv);
  if (argsnode >= 0) nt_node_set_ref(nt, n, "arguments", argsnode);
  if (block >= 0) nt_node_set_ref(nt, n, "block", block);
  return n;
}
static int te_stmts1(NodeTable *nt, int s) {
  int n = nt_new_node(nt, "StatementsNode"); nt_node_set_arr(nt, n, "body", &s, 1); return n;
}
static int te_lvwrite(NodeTable *nt, const char *name, int value) {
  int n = nt_new_node(nt, "LocalVariableWriteNode");
  nt_node_set_str(nt, n, "name", name);
  nt_node_set_ref(nt, n, "value", value);
  return n;
}

/* `Enumerator.produce(init) { |prev| nxt }` -> a fiber-backed generator that
   yields init, then repeatedly applies the block. Rewritten (before the inference
   fixpoint, so the generator's block body is typed) to:
     Enumerator.new { |__py|
       __pv = init
       loop { __py << __pv; prev = __pv; __pv = begin nxt end }
     }
   The block's param name (`prev`) is bound to the current value each round and its
   body inlined via a begin-expression. `<<` lowers to Fiber.yield. (#2483) */
static void desugar_enumerator_produce(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "produce")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "ConstantReadNode")) continue;
    if (!nt_str(nt, recv, "name") || !sp_streq(nt_str(nt, recv, "name"), "Enumerator")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1 || !av) continue;   /* only produce(init) { } for now */
    int init = av[0];
    int bbody = nt_ref(nt, blk, "body");
    const char *pname = block_param_name(c, blk, 0);
    if (!pname || bbody < 0) continue;
    int pv_w = te_lvwrite(nt, "__pv", init);
    /* loop body: __py << __pv ; <pname> = __pv ; __pv = begin <body> end */
    int yshl = te_call(nt, te_lvread(nt, "__py"), "<<", te_args1(nt, te_lvread(nt, "__pv")), -1);
    int bind_p = te_lvwrite(nt, pname, te_lvread(nt, "__pv"));
    int beginn = nt_new_node(nt, "BeginNode"); nt_node_set_ref(nt, beginn, "statements", bbody);
    int pv_w2 = te_lvwrite(nt, "__pv", beginn);
    int loopbodyarr[3] = { yshl, bind_p, pv_w2 };
    int loopbody = nt_new_node(nt, "StatementsNode"); nt_node_set_arr(nt, loopbody, "body", loopbodyarr, 3);
    int loopblk = nt_new_node(nt, "BlockNode"); nt_node_set_ref(nt, loopblk, "body", loopbody);
    int loopcall = te_call(nt, -1, "loop", -1, loopblk);
    /* generator block: |__py| __pv=...; loop {...} */
    int genarr[2] = { pv_w, loopcall };
    int genbody = nt_new_node(nt, "StatementsNode"); nt_node_set_arr(nt, genbody, "body", genarr, 2);
    int pyreq = nt_new_node(nt, "RequiredParameterNode"); nt_node_set_str(nt, pyreq, "name", "__py");
    int genparams = nt_new_node(nt, "ParametersNode"); nt_node_set_arr(nt, genparams, "requireds", &pyreq, 1);
    int genbp = nt_new_node(nt, "BlockParametersNode"); nt_node_set_ref(nt, genbp, "parameters", genparams);
    int genblk = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, genblk, "parameters", genbp);
    nt_node_set_ref(nt, genblk, "body", genbody);
    /* rewrite THIS node into Enumerator.new { |__py| ... } */
    nt_node_set_str(nt, id, "name", "new");
    nt_node_set_ref(nt, id, "block", genblk);
    nt_node_set_ref(nt, id, "arguments", -1);
    comp_grow_node_arrays(c);
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
/* Synthesize, on each Struct/Data class with no user #each, the standard
   member iterator

       def each; yield @m1; ...; yield @mN; self; end

   so every inherited Enumerable method (map/select/sum/min/include?/...)
   rides the __enum_to_a machinery synthesized right below. Unused copies
   prune by reachability. */
/* `StructConst[a, b]` is Struct::[], an alias for StructConst.new(a, b). Rewrite
   the `[]` call to `new` once the struct classes are registered so the whole
   .new machinery (positional/keyword member fill, typing) serves it. */
static void desugar_struct_index_ctor(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "[]")) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    int ci = -1;
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
      ci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (rty && sp_streq(rty, "LocalVariableReadNode"))
      ci = class_var_static_ci(c, recv);
    if (ci < 0 || !c->classes[ci].is_struct) continue;
    nt_node_set_str(nt, id, "name", "new");
  }
}

static void synth_struct_each(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int ncls0 = c->nclasses;
  for (int ci = 0; ci < ncls0; ci++) {
    ClassInfo *cls = &c->classes[ci];
    if (!cls->is_struct || cls->nivars == 0) continue;
    if (comp_method_in_class(c, ci, "each") >= 0) continue;
    int stmts[65]; int nst = 0;
    for (int j = 0; j < cls->nivars && nst < 64; j++) {
      int ivr = nt_new_node(nt, "InstanceVariableReadNode");
      nt_node_set_str(nt, ivr, "name", cls->ivars[j]);
      int yargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, yargs, "arguments", &ivr, 1);
      int yn = nt_new_node(nt, "YieldNode");
      nt_node_set_ref(nt, yn, "arguments", yargs);
      stmts[nst++] = yn;
    }
    stmts[nst++] = nt_new_node(nt, "SelfNode");
    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", stmts, nst);
    int def = nt_new_node(nt, "DefNode");
    nt_node_set_str(nt, def, "name", "each");
    nt_node_set_ref(nt, def, "body", body);
    Scope *ms = comp_scope_new(c, "each", def);
    ms->class_id = ci;
    ms->body = body;
    ms->yields = 1;
    comp_grow_node_arrays(c);
    walk_scope(c, body, c->nscopes - 1, ci);
    /* def each_pair; yield :m1, @m1; ...; self; end */
    if (comp_method_in_class(c, ci, "each_pair") < 0) {
      int pst[65]; int pn = 0;
      for (int j = 0; j < cls->nivars && pn < 64; j++) {
        int sy = nt_new_node(nt, "SymbolNode");
        nt_node_set_str(nt, sy, "value", cls->ivars[j] + 1);
        int ivr = nt_new_node(nt, "InstanceVariableReadNode");
        nt_node_set_str(nt, ivr, "name", cls->ivars[j]);
        int ya[2] = { sy, ivr };
        int yargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, yargs, "arguments", ya, 2);
        int yn = nt_new_node(nt, "YieldNode");
        nt_node_set_ref(nt, yn, "arguments", yargs);
        pst[pn++] = yn;
      }
      pst[pn++] = nt_new_node(nt, "SelfNode");
      int pbody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, pbody, "body", pst, pn);
      int pdef = nt_new_node(nt, "DefNode");
      nt_node_set_str(nt, pdef, "name", "each_pair");
      nt_node_set_ref(nt, pdef, "body", pbody);
      Scope *ps = comp_scope_new(c, "each_pair", pdef);
      ps->class_id = ci;
      ps->body = pbody;
      ps->yields = 1;
      comp_grow_node_arrays(c);
      walk_scope(c, pbody, c->nscopes - 1, ci);
    }
    /* def each_with_index; yield @m1, 0; yield @m2, 1; ...; self; end
       (returns the receiver, unlike the __enum_to_a redirect which would
       return the flat member array). The index is a literal per member. */
    if (comp_method_in_class(c, ci, "each_with_index") < 0) {
      int wst[65]; int wn = 0;
      for (int j = 0; j < cls->nivars && wn < 64; j++) {
        int ivr = nt_new_node(nt, "InstanceVariableReadNode");
        nt_node_set_str(nt, ivr, "name", cls->ivars[j]);
        int ixn = nt_new_node(nt, "IntegerNode");
        nt_node_set_int(nt, ixn, "value", j);
        int ya[2] = { ivr, ixn };
        int yargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, yargs, "arguments", ya, 2);
        int yn = nt_new_node(nt, "YieldNode");
        nt_node_set_ref(nt, yn, "arguments", yargs);
        wst[wn++] = yn;
      }
      wst[wn++] = nt_new_node(nt, "SelfNode");
      int wbody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, wbody, "body", wst, wn);
      int wdef = nt_new_node(nt, "DefNode");
      nt_node_set_str(nt, wdef, "name", "each_with_index");
      nt_node_set_ref(nt, wdef, "body", wbody);
      Scope *ws = comp_scope_new(c, "each_with_index", wdef);
      ws->class_id = ci;
      ws->body = wbody;
      ws->yields = 1;
      comp_grow_node_arrays(c);
      walk_scope(c, wbody, c->nscopes - 1, ci);
    }
  }
}

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
    /* an each that drives the block via `yield`, or one that forwards an
       explicit `&blk` to an inner iterator (`def each(&b) = @xs.each(&b)`):
       both invoke the synthesized `each { |e| acc << e }` block per element.
       An anonymous `&` forward is excluded (blk_param is "") -- forwarding an
       anonymous block through the synthesized materializer is not yet wired and
       would yield an empty result, so it stays a loud reject. */
    if (!sp_streq(m->name, "each") ||
        !(m->yields || (m->blk_param && m->blk_param[0]))) continue;
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

/* Blockless builtin iterators that already lower to a materialized Enumerator;
   a builtin-receiver `to_enum(:m)` retargets to the plain blockless `recv.m`. */
static int to_enum_builtin_method(const char *m) {
  static const char *const ok[] = {
    "each", "reverse_each", "each_with_index", "each_index",
    "each_char", "each_line", "each_slice", "each_cons", "each_pair",
    "times", "each_byte", NULL };
  for (int k = 0; ok[k]; k++) if (sp_streq(m, ok[k])) return 1;
  return 0;
}

/* Synthesize, on each user class whose yielding instance method `m` is the
   target of a `to_enum(:m)`/`enum_for(:m)` call, a lazy generator helper

       def __to_enum_m
         Enumerator.new do |__ey|
           m do |*__ev|
             Fiber.yield(__ev.length <= 1 ? __ev[0] : __ev)
           end
         end
       end

   so `recv.to_enum(:m)` (rewritten to `recv.__to_enum_m` by desugar_to_enum)
   yields a real fiber-backed Enumerator. `m`'s yields flow through the rest
   param `__ev`; the `length<=1 ? [0] : self` collapse is CRuby's ary2sv
   (`[][0]` is nil), and `y << v` is emitted as a direct `Fiber.yield` so the
   inner block captures nothing (no proc-in-proc capture). The method call is
   the construction-time snapshot of the receiver (recv -> self via cap_self).
   Runs pre-fixpoint like synth_enum_to_a; unused helpers prune by reachability. */
static void synth_to_enum_generators(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (c->nscopes == 0) return;
  /* Collect the set of target method names across all to_enum/enum_for calls. */
  int nnames = 0, cnames = 8;
  char **names = malloc(sizeof(char *) * (size_t)cnames);
  if (!names) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    char buf[128];
    if (!to_enum_target(c, id, buf, sizeof buf, NULL, NULL)) continue;
    int dup = 0;
    for (int k = 0; k < nnames; k++) if (sp_streq(names[k], buf)) { dup = 1; break; }
    if (dup) continue;
    if (nnames >= cnames) {
      cnames *= 2; char **tmp = realloc(names, sizeof(char *) * (size_t)cnames);
      if (!tmp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      names = tmp;
    }
    names[nnames++] = strdup(buf);
  }
  if (nnames == 0) { free(names); return; }

  /* Collect (class, method-name, def_node) triples: a yielding instance method
     whose name is a to_enum target. Snapshot before synthesis grows c->scopes. */
  int ntr = 0, ctr = c->nscopes;
  int *tc = malloc(sizeof(int) * (size_t)ctr);
  int *tdef = malloc(sizeof(int) * (size_t)ctr);
  char **tm = malloc(sizeof(char *) * (size_t)ctr);
  if (!tc || !tdef || !tm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *m = &c->scopes[s];
    if (!m->name || m->is_cmethod || m->class_id < 0) continue;
    if (!(m->yields || (m->blk_param && m->blk_param[0]))) continue;
    int hit = 0;
    for (int k = 0; k < nnames; k++) if (sp_streq(names[k], m->name)) { hit = 1; break; }
    if (!hit) continue;
    char hname[160]; snprintf(hname, sizeof hname, "__to_enum_%s", m->name);
    if (comp_method_in_class(c, m->class_id, hname) >= 0) continue;   /* already synthesized */
    int dup = 0;
    for (int k = 0; k < ntr; k++)
      if (tc[k] == m->class_id && sp_streq(tm[k], m->name)) { dup = 1; break; }
    if (dup) continue;
    tc[ntr] = m->class_id; tdef[ntr] = m->def_node; tm[ntr] = strdup(m->name); ntr++;
  }

  for (int k = 0; k < ntr; k++) {
    int ci = tc[k];
    const char *m = tm[k];   /* stable strdup: survives nt_new_node reallocs */
    char hname[160]; snprintf(hname, sizeof hname, "__to_enum_%s", m);

    /* __ev.length <= 1 ? __ev[0] : __ev */
    int lencall = te_call(nt, te_lvread(nt, "__ev"), "length", -1, -1);
    int cmp = te_call(nt, lencall, "<=", te_args1(nt, te_int(nt, 1)), -1);
    int idx = te_call(nt, te_lvread(nt, "__ev"), "[]", te_args1(nt, te_int(nt, 0)), -1);
    int elsenode = nt_new_node(nt, "ElseNode");
    nt_node_set_ref(nt, elsenode, "statements", te_stmts1(nt, te_lvread(nt, "__ev")));
    int ifn = nt_new_node(nt, "IfNode");
    nt_node_set_ref(nt, ifn, "predicate", cmp);
    nt_node_set_ref(nt, ifn, "statements", te_stmts1(nt, idx));
    nt_node_set_ref(nt, ifn, "subsequent", elsenode);
    /* Fiber.yield(<collapse>) */
    int fyield = te_call(nt, te_const(nt, "Fiber"), "yield", te_args1(nt, ifn), -1);
    /* inner block: m { |*__ev| Fiber.yield(...) } */
    int evrest = nt_new_node(nt, "RestParameterNode"); nt_node_set_str(nt, evrest, "name", "__ev");
    int inner_params = nt_new_node(nt, "ParametersNode"); nt_node_set_ref(nt, inner_params, "rest", evrest);
    int inner_bp = nt_new_node(nt, "BlockParametersNode"); nt_node_set_ref(nt, inner_bp, "parameters", inner_params);
    int inner_blk = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, inner_blk, "parameters", inner_bp);
    nt_node_set_ref(nt, inner_blk, "body", te_stmts1(nt, fyield));
    int mcall = te_call(nt, -1, m, -1, inner_blk);   /* implicit self.m -> cap_self */
    /* outer block: Enumerator.new { |__ey| <mcall> } */
    int eyreq = nt_new_node(nt, "RequiredParameterNode"); nt_node_set_str(nt, eyreq, "name", "__ey");
    int outer_params = nt_new_node(nt, "ParametersNode"); nt_node_set_arr(nt, outer_params, "requireds", &eyreq, 1);
    int outer_bp = nt_new_node(nt, "BlockParametersNode"); nt_node_set_ref(nt, outer_bp, "parameters", outer_params);
    int outer_blk = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, outer_blk, "parameters", outer_bp);
    nt_node_set_ref(nt, outer_blk, "body", te_stmts1(nt, mcall));
    int enumnew = te_call(nt, te_const(nt, "Enumerator"), "new", -1, outer_blk);
    int mbody = te_stmts1(nt, enumnew);

    Scope *ms = comp_scope_new(c, hname, tdef[k]);
    ms->class_id = ci;
    ms->body = mbody;
    int ms_idx = c->nscopes - 1;
    LocalVar *ey = scope_local_intern(ms, "__ey"); if (ey) ey->is_block_param = 1;
    LocalVar *ev = scope_local_intern(ms, "__ev"); if (ev) ev->is_block_param = 1;
    comp_grow_node_arrays(c);
    walk_scope(c, mbody, ms_idx, ci);
  }

  for (int k = 0; k < nnames; k++) free(names[k]);
  for (int k = 0; k < ntr; k++) free(tm[k]);
  free(names); free(tc); free(tdef); free(tm);
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
    "find_all", "zip", "grep", "grep_v", "to_h", "uniq", "reverse",
    "member?", "minmax", "join", "index", "each",
    "each_cons", "each_slice", "chunk", "chunk_while", "slice_when",
    "minmax_by", "cycle", "lazy", "each_entry", "reverse_each", "compact",
    "chain", "slice_before", "slice_after", NULL };
  for (int k = 0; names[k]; k++) if (sp_streq(nm, names[k])) return 1;
  return 0;
}

/* Redirect a bare Enumerable call -- `obj.map { }`, `obj.to_a`, ... -- on a
   user class that defines `#each` but not that method, through the synthesized
   `__enum_to_a` helper: `obj.<m>{blk}` becomes `obj.__enum_to_a.<m>{blk}`, so
   the array path handles it without an explicit `enum_for(:each)`. Runs in the
   inference fixpoint (needs the receiver's type, and the inserted node is typed
   on a later iteration). Returns 1 if anything changed. */
/* One-shot chain normalizations before scope registration:
   - str.each_char.with_index { }  ->  str.chars.each.with_index { }
     (the each.with_index chain machinery keys on the `each` shape)
   - enum_recv.each.with_object(x) { }  ->  enum_recv.each_with_object(x) { }
   Both rewrites produce constructs the existing emitters already serve. */
static void desugar_enum_chain_shapes(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  /* A string-bounded range literal bound to a variable (`cr = ('a'..'e')`)
     has no sp_Range representation (int bounds only): materialize it into a
     string array at the assignment, like the literal-receiver interpose
     below (#1934 shape). Range-only methods on the variable then reject
     loudly instead of miscompiling. */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "LocalVariableWriteNode")) continue;
    int val = nt_ref(nt, id, "value");
    int rn = val;
    while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
      int pb = nt_ref(nt, rn, "body"); int pbn = 0;
      const int *pbb = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
      rn = pbn == 1 ? pbb[0] : -1;
    }
    if (rn < 0 || !nt_type(nt, rn) || !sp_streq(nt_type(nt, rn), "RangeNode")) continue;
    int rlo = nt_ref(nt, rn, "left");
    const char *rloty = rlo >= 0 ? nt_type(nt, rlo) : NULL;
    if (!rloty || !sp_streq(rloty, "StringNode")) continue;
    (void)rn;   /* a string range is its own value type now (#3064) */
  }
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int recv = nt_ref(nt, id, "receiver");
    /* `<finite>.lazy.each_cons(n)` / `.each_slice(n)`: each_cons/each_slice are
       not element-wise, so the lazy pipeline cannot fuse them and the chain
       falls to a NoMethodError. For a finite source (a bounded range literal or
       an array) the lazy is a no-op -- drop it so the eager each_cons/each_slice
       Enumerator, and a following .first(m), materialize (#3171). */
    if ((sp_streq(nm, "each_cons") || sp_streq(nm, "each_slice")) && recv >= 0 &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "lazy") &&
        nt_ref(nt, recv, "block") < 0 && nt_ref(nt, recv, "arguments") < 0) {
      int lz_recv = nt_ref(nt, recv, "receiver");
      /* `p (2..10).lazy...` parses the source as a ParenthesesNode; look through
         it (and nested parens) to the real range/array. */
      while (lz_recv >= 0 && nt_type(nt, lz_recv) && sp_streq(nt_type(nt, lz_recv), "ParenthesesNode")) {
        int pb = nt_ref(nt, lz_recv, "body"); int pbn = 0;
        const int *pbb = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
        lz_recv = (pbn == 1 && pbb) ? pbb[0] : -1;
      }
      const char *lty = lz_recv >= 0 ? nt_type(nt, lz_recv) : NULL;
      int finite = 0;
      if (lty && sp_streq(lty, "ArrayNode")) finite = 1;
      else if (lty && sp_streq(lty, "RangeNode")) {
        int rgt = nt_ref(nt, lz_recv, "right");   /* endless (nil right) stays lazy */
        finite = rgt >= 0 && nt_type(nt, rgt) && sp_streq(nt_type(nt, rgt), "IntegerNode");
      }
      if (finite) { nt_node_set_ref(nt, id, "receiver", lz_recv); continue; }
    }
    if ((sp_streq(nm, "merge") || sp_streq(nm, "merge!")) && recv >= 0) {
      /* h.merge(a, b, ...) folds left into h.merge(a).merge(b)...; merge!
         chains the same way because it returns self. */
      int argsn = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av0 = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
      if (an >= 2 && an <= 64) {
        const char *mname = sp_streq(nm, "merge!") ? "merge!" : "merge";
        int av[64];
        memcpy(av, av0, (size_t)an * sizeof(int));
        int mblk = nt_ref(nt, id, "block");
        int cur = recv;
        for (int j = 0; j < an - 1; j++) {
          int call = nt_new_node(nt, "CallNode");
          int one = nt_new_node(nt, "ArgumentsNode");
          if (call < 0 || one < 0) { cur = -1; break; }
          nt_node_set_arr(nt, one, "arguments", &av[j], 1);
          nt_node_set_str(nt, call, "name", mname);
          nt_node_set_ref(nt, call, "receiver", cur);
          nt_node_set_ref(nt, call, "arguments", one);
          /* a conflict block applies at every merge step */
          if (mblk >= 0) nt_node_set_ref(nt, call, "block", mblk);
          cur = call;
        }
        if (cur >= 0) {
          int last = nt_new_node(nt, "ArgumentsNode");
          if (last >= 0) {
            nt_node_set_arr(nt, last, "arguments", &av[an - 1], 1);
            nt_node_set_ref(nt, id, "receiver", cur);
            nt_node_set_ref(nt, id, "arguments", last);
            comp_grow_node_arrays(c);
          }
        }
      }
      continue;
    }
    /* A STRING-bounded range receiver ("a".."e") supports only the
       range-native methods; any other Enumerable method rides the
       materialized string array (the int-range redispatch has no char
       equivalent, #1934). */
    int rngn = recv;
    while (rngn >= 0 && nt_type(nt, rngn) && sp_streq(nt_type(nt, rngn), "ParenthesesNode")) {
      int pb = nt_ref(nt, rngn, "body");
      int pbn = 0;
      const int *pbb = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
      rngn = pbn == 1 ? pbb[0] : -1;
    }
    /* `m(&lambda_literal)`: attach the lambda's block directly as m's block
       (the :sym.to_proc desugar below produces exactly this shape when the
       proc is passed inline as a block argument). */
    {
      int blkref = nt_ref(nt, id, "block");
      if (blkref >= 0 && nt_type(nt, blkref) &&
          sp_streq(nt_type(nt, blkref), "BlockArgumentNode")) {
        int bex = nt_ref(nt, blkref, "expression");
        if (bex >= 0 && nt_type(nt, bex) && sp_streq(nt_type(nt, bex), "CallNode")) {
          const char *lname = nt_str(nt, bex, "name");
          int lblk = nt_ref(nt, bex, "block");
          if (lname && (sp_streq(lname, "lambda") || sp_streq(lname, "proc")) &&
              nt_ref(nt, bex, "receiver") < 0 && lblk >= 0 &&
              nt_type(nt, lblk) && sp_streq(nt_type(nt, lblk), "BlockNode"))
            nt_node_set_ref(nt, id, "block", lblk);
        }
      }
    }
    /* each_entry (block form) yields each element like #each but must return the
       RECEIVER (Enumerable#each_entry), unlike a user #each whose own return
       value leaks through. Leave the name intact so it rides the same
       receiver-returning redirect as reverse_each (#2621); blockless
       #each_entry stays for the Enumerable / struct enumerator path to
       materialize an Enumerator. */

    /* max(n) { cmp } / min(n) { cmp }: the n extremes by the comparator --
       sort { cmp } then take from the appropriate end (max descends). */
    if ((sp_streq(nm, "max") || sp_streq(nm, "min")) && recv >= 0 &&
        nt_ref(nt, id, "block") >= 0) {
      int margs = nt_ref(nt, id, "arguments");
      int man = 0;
      if (margs >= 0) nt_arr(nt, margs, "arguments", &man);
      int mblk = nt_ref(nt, id, "block");
      const char *mbty = mblk >= 0 ? nt_type(nt, mblk) : NULL;
      if (man == 1 && mbty && sp_streq(mbty, "BlockNode")) {
        int sortc = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, sortc, "name", "sort");
        nt_node_set_ref(nt, sortc, "receiver", recv);
        nt_node_set_ref(nt, sortc, "block", mblk);
        if (sp_streq(nm, "min")) {
          nt_node_set_str(nt, id, "name", "first");
          nt_node_set_ref(nt, id, "receiver", sortc);
          nt_node_set_ref(nt, id, "block", -1);
        }
        else {
          int lastc = nt_new_node(nt, "CallNode");
          nt_node_set_str(nt, lastc, "name", "last");
          nt_node_set_ref(nt, lastc, "receiver", sortc);
          nt_node_set_ref(nt, lastc, "arguments", margs);
          nt_node_set_str(nt, id, "name", "reverse");
          nt_node_set_ref(nt, id, "receiver", lastc);
          nt_node_set_ref(nt, id, "arguments", -1);
          nt_node_set_ref(nt, id, "block", -1);
        }
        comp_grow_node_arrays(c);
        continue;
      }
    }
    /* :sym.to_proc (explicit): rewrite to the equivalent lambda -- one
       parameter calling the method, or two for the binary operators
       (:+.to_proc adds its two arguments). The &:sym shorthand stays on its
       own (textual) path. */
    if (sp_streq(nm, "to_proc") && recv >= 0 && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "SymbolNode") && nt_ref(nt, id, "block") < 0) {
      const char *sym = nt_str(nt, recv, "value");
      if (sym && *sym) {
        static const char *const binops[] = {
          "+", "-", "*", "/", "%", "**", "==", "!=", "<", "<=", ">", ">=",
          "<=>", "<<", ">>", "&", "|", "^", NULL };
        int is_binop = 0;
        for (int j = 0; binops[j]; j++) if (sp_streq(sym, binops[j])) { is_binop = 1; break; }
        char pa[32], pb[32];
        snprintf(pa, sizeof pa, "__stp_a_%d", id);
        snprintf(pb, sizeof pb, "__stp_b_%d", id);
        int kpa = nt_new_node(nt, "RequiredParameterNode");
        nt_node_set_str(nt, kpa, "name", pa);
        int preq[2] = { kpa, -1 };
        int npar = 1;
        if (is_binop) {
          int kpb = nt_new_node(nt, "RequiredParameterNode");
          nt_node_set_str(nt, kpb, "name", pb);
          preq[1] = kpb; npar = 2;
        }
        int params = nt_new_node(nt, "ParametersNode");
        nt_node_set_arr(nt, params, "requireds", preq, npar);
        int bparams = nt_new_node(nt, "BlockParametersNode");
        nt_node_set_ref(nt, bparams, "parameters", params);
        int ra = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, ra, "name", pa);
        int call = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, call, "name", sym);
        nt_node_set_ref(nt, call, "receiver", ra);
        if (is_binop) {
          int rb2 = nt_new_node(nt, "LocalVariableReadNode");
          nt_node_set_str(nt, rb2, "name", pb);
          int cargs = nt_new_node(nt, "ArgumentsNode");
          nt_node_set_arr(nt, cargs, "arguments", &rb2, 1);
          nt_node_set_ref(nt, call, "arguments", cargs);
        }
        int blkbody = nt_new_node(nt, "StatementsNode");
        nt_node_set_arr(nt, blkbody, "body", &call, 1);
        int blk = nt_new_node(nt, "BlockNode");
        nt_node_set_ref(nt, blk, "parameters", bparams);
        nt_node_set_ref(nt, blk, "body", blkbody);
        nt_node_set_str(nt, id, "name", "lambda");
        nt_node_set_ref(nt, id, "receiver", -1);
        nt_node_set_ref(nt, id, "arguments", -1);
        nt_node_set_ref(nt, id, "block", blk);
        /* Symbol#to_proc always reports arity -2 (a required receiver plus
           optional arguments), whatever the underlying method takes; the
           synthesized lambda's own parameter count would say 1 or 2 (#3053) */
        nt_node_set_int(nt, id, "stp_arity", 1);
        comp_grow_node_arrays(c);
        continue;
      }
    }
    /* transform_keys(mapping) [no block]: key k maps to mapping[k] when
       present, else stays -- exactly `{ |k| mapping.fetch(k, k) }`. The
       mapping expression is re-evaluated per key, which is correct (and only
       wasteful) for the literal-hash common case. */
    if ((sp_streq(nm, "transform_keys") || sp_streq(nm, "transform_keys!")) &&
        recv >= 0) {
      int argsn = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av0 = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
      int origblk = nt_ref(nt, id, "block");
      if (an == 1) {
        int mnode = av0[0];
        char pname[32];
        snprintf(pname, sizeof pname, "__tk_k_%d", id);
        int kp = nt_new_node(nt, "RequiredParameterNode");
        nt_node_set_str(nt, kp, "name", pname);
        int params = nt_new_node(nt, "ParametersNode");
        nt_node_set_arr(nt, params, "requireds", &kp, 1);
        int bparams = nt_new_node(nt, "BlockParametersNode");
        nt_node_set_ref(nt, bparams, "parameters", params);
        int kr1 = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, kr1, "name", pname);
        int kr2 = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, kr2, "name", pname);
        int fargs = nt_new_node(nt, "ArgumentsNode");
        if (origblk >= 0) {
          /* a block alongside the mapping handles the unmapped keys:
             mapping.fetch(k) { original_block } */
          nt_node_set_arr(nt, fargs, "arguments", &kr1, 1);
          (void)kr2;
        }
        else {
          int fa[2] = { kr1, kr2 };
          nt_node_set_arr(nt, fargs, "arguments", fa, 2);
        }
        int fetch = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, fetch, "name", "fetch");
        nt_node_set_ref(nt, fetch, "receiver", mnode);
        nt_node_set_ref(nt, fetch, "arguments", fargs);
        if (origblk >= 0) nt_node_set_ref(nt, fetch, "block", origblk);
        int blkbody = nt_new_node(nt, "StatementsNode");
        nt_node_set_arr(nt, blkbody, "body", &fetch, 1);
        int blk = nt_new_node(nt, "BlockNode");
        nt_node_set_ref(nt, blk, "parameters", bparams);
        nt_node_set_ref(nt, blk, "body", blkbody);
        nt_node_set_ref(nt, id, "block", blk);
        nt_node_set_ref(nt, id, "arguments", -1);
        comp_grow_node_arrays(c);
      }
      /* fall through: the bang form wraps in replace below */
    }
    /* transform_values! / transform_keys! { }: build the transformed hash,
       then splice it back into the receiver (aliases observe the mutation). */
    /* transform_keys!/transform_values! on a direct hash literal has no alias to
       observe the in-place mutation, so it is just the non-bang transform. This
       also serves the key-type-changing case (sym -> str), where splicing a
       different hash variant back via #replace cannot work (#2355). */
    if ((sp_streq(nm, "transform_values!") || sp_streq(nm, "transform_keys!")) &&
        recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
        nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "HashNode") ||
                              sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
      nt_node_set_str(nt, id, "name",
                      sp_streq(nm, "transform_keys!") ? "transform_keys" : "transform_values");
      continue;
    }
    if ((sp_streq(nm, "transform_values!") || sp_streq(nm, "transform_keys!")) &&
        recv >= 0 && nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      int inner = nt_new_node(nt, "CallNode");
      int rargs = nt_new_node(nt, "ArgumentsNode");
      if (inner >= 0 && rargs >= 0) {
        nt_node_set_str(nt, inner, "name",
                        sp_streq(nm, "transform_keys!") ? "transform_keys" : "transform_values");
        nt_node_set_ref(nt, inner, "receiver", recv);
        nt_node_set_ref(nt, inner, "block", blk);
        nt_node_set_arr(nt, rargs, "arguments", &inner, 1);
        nt_node_set_str(nt, id, "name", "replace");
        nt_node_set_ref(nt, id, "block", -1);
        nt_node_set_ref(nt, id, "arguments", rargs);
        comp_grow_node_arrays(c);
      }
      continue;
    }
    if (sp_streq(nm, "[]") && recv >= 0 && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
        nt_ref(nt, id, "block") < 0) {
      const char *cn = nt_str(nt, recv, "name");
      int argsn = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av0 = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
      if (cn && sp_streq(cn, "Hash") && an >= 1) {
        if (an == 1 && nt_type(nt, av0[0]) &&
            sp_streq(nt_type(nt, av0[0]), "ArrayNode")) {
          /* Hash[[[k, v], ...]] == pairs.to_h */
          nt_node_set_str(nt, id, "name", "to_h");
          nt_node_set_ref(nt, id, "receiver", av0[0]);
          nt_node_set_ref(nt, id, "arguments", -1);
          continue;
        }
        /* Hash[arg] with any single non-literal argument: the same pairs.to_h
           rewrite (this desugar runs pre-type, so no array check is possible;
           to_h itself raises on a non-convertible receiver, as Hash[] does). */
        if (an == 1) {
          nt_node_set_str(nt, id, "name", "to_h");
          nt_node_set_ref(nt, id, "receiver", av0[0]);
          nt_node_set_ref(nt, id, "arguments", -1);
          continue;
        }
        if (an >= 2 && an % 2 == 0 && an <= 64) {
          /* Hash[k1, v1, k2, v2, ...] == [[k1, v1], [k2, v2], ...].to_h */
          int av[64];
          memcpy(av, av0, (size_t)an * sizeof(int));
          int pairs[32];
          int ok = 1;
          for (int j = 0; j < an / 2; j++) {
            int pair = nt_new_node(nt, "ArrayNode");
            if (pair < 0) { ok = 0; break; }
            nt_node_set_arr(nt, pair, "elements", &av[j * 2], 2);
            pairs[j] = pair;
          }
          if (ok) {
            int outer = nt_new_node(nt, "ArrayNode");
            if (outer >= 0) {
              nt_node_set_arr(nt, outer, "elements", pairs, an / 2);
              nt_node_set_str(nt, id, "name", "to_h");
              nt_node_set_ref(nt, id, "receiver", outer);
              nt_node_set_ref(nt, id, "arguments", -1);
              comp_grow_node_arrays(c);
            }
          }
          continue;
        }
      }
    }
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || nt_ref(nt, recv, "block") >= 0) continue;
    if (sp_streq(nm, "with_index") && sp_streq(rn, "each_char")) {
      nt_node_set_str(nt, recv, "name", "chars");
      int eachn = nt_new_node(nt, "CallNode");
      if (eachn < 0) continue;
      nt_node_set_str(nt, eachn, "name", "each");
      nt_node_set_ref(nt, eachn, "receiver", recv);
      nt_node_set_ref(nt, id, "receiver", eachn);
      comp_grow_node_arrays(c);
    }
    /* str.each_char/each_line (blockless, no args) followed by an eager
       block-consuming Enumerable method -> chars/lines, so the block param
       types as String (a StrArray element) instead of an opaque enumerator
       element (#2901). Not for the lazy/enumerator-shaped methods, which keep
       their own machinery. */
    else if (nt_ref(nt, id, "block") >= 0 && nt_ref(nt, recv, "arguments") < 0 &&
             (sp_streq(rn, "each_char") || sp_streq(rn, "each_line")) &&
             (sp_streq(nm, "map") || sp_streq(nm, "collect") || sp_streq(nm, "select") ||
              sp_streq(nm, "filter") || sp_streq(nm, "reject") || sp_streq(nm, "flat_map") ||
              sp_streq(nm, "collect_concat") || sp_streq(nm, "filter_map") ||
              sp_streq(nm, "min_by") || sp_streq(nm, "max_by") || sp_streq(nm, "sort_by") ||
              sp_streq(nm, "find") || sp_streq(nm, "detect") || sp_streq(nm, "count") ||
              sp_streq(nm, "sum") || sp_streq(nm, "each_with_index") ||
              sp_streq(nm, "partition") || sp_streq(nm, "group_by"))) {
      nt_node_set_str(nt, recv, "name", sp_streq(rn, "each_char") ? "chars" : "lines");
    }
    else if ((sp_streq(nm, "to_a") || sp_streq(nm, "force")) && sp_streq(rn, "take") &&
             nt_ref(nt, id, "block") < 0) {
      /* X.take(n).to_a == X.first(n) -- and first(n) is what the lazy
         pipeline terminates on, so a lazy .take(n).to_a materializes. */
      int inner = nt_ref(nt, recv, "receiver");
      int targs = nt_ref(nt, recv, "arguments");
      int tan = 0; if (targs >= 0) nt_arr(nt, targs, "arguments", &tan);
      if (inner >= 0 && tan == 1) {
        nt_node_set_str(nt, recv, "name", "first");
        nt_node_set_ref(nt, id, "receiver", inner);
        nt_node_set_str(nt, id, "name", "first");
        nt_node_set_ref(nt, id, "arguments", targs);
        /* the outer node IS now the first(n) call; drop the duplicated inner */
        nt_node_set_ref(nt, id, "receiver", inner);
      }
    }
    else if ((sp_streq(nm, "with_object") || sp_streq(nm, "each_with_object")) &&
             sp_streq(rn, "each")) {
      int inner = nt_ref(nt, recv, "receiver");
      if (inner < 0) continue;
      nt_node_set_str(nt, id, "name", "each_with_object");
      nt_node_set_ref(nt, id, "receiver", inner);
    }
  }
}

/* `recv.method(:sym)` over a BUILTIN receiver (string/int/array/...) has no
   compiled function to bind, so calling the Method crashed. Synthesize a
   top-level wrapper `def __bam_<id>(__bam_r) = __bam_r.sym` -- the wrapper's
   param is pinned to the receiver's type, the Method binds the wrapper (self
   is passed as the first argument by the bound-call ABI), and the existing
   resolved-target machinery types the .call. The symbol argument is rewritten
   to the wrapper's name. */
static int desugar_builtin_method_obj(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "method")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) {
      /* receiverless method(:Integer) etc.: a Kernel builtin has no C target
         for the bound-method fn slot, so synthesize a forwarding top-level
         def and retarget the symbol at it (`def __bam_N(__bam_r) = Integer(__bam_r)`).
         The wrapper's param types from the Method's call sites like any def. */
      static const char *const KFN0[] = { "Integer", "Float", "String", "Array",
                                          "Rational", "Complex", "puts", "print",
                                          "p", "pp", NULL };
      const char *ksym = method_sym_arg(c, id);
      if (!ksym || !ksym[0] || ksym[0] == '_') continue;
      int kknown = 0;
      for (int k = 0; KFN0[k]; k++) if (sp_streq(ksym, KFN0[k])) { kknown = 1; break; }
      if (!kknown) continue;
      if (comp_method_index(c, ksym) >= 0) continue;   /* a user def wins */
      char kwname[48];
      snprintf(kwname, sizeof kwname, "__bam_%d", id);
      if (comp_method_index(c, kwname) >= 0) continue;
      int krp = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, krp, "name", "__bam_r");
      int kparams = nt_new_node(nt, "ParametersNode");
      nt_node_set_arr(nt, kparams, "requireds", &krp, 1);
      int krread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, krread, "name", "__bam_r");
      int kargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, kargs, "arguments", &krread, 1);
      int kcall = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, kcall, "name", ksym);
      nt_node_set_ref(nt, kcall, "arguments", kargs);
      int kbody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, kbody, "body", &kcall, 1);
      int kdef = nt_new_node(nt, "DefNode");
      nt_node_set_str(nt, kdef, "name", kwname);
      nt_node_set_ref(nt, kdef, "parameters", kparams);
      nt_node_set_ref(nt, kdef, "body", kbody);
      Scope *kws = comp_scope_new(c, kwname, kdef);
      kws->class_id = -1;
      kws->body = kbody;
      int kws_idx = c->nscopes - 1;
      scope_add_param(kws, "__bam_r", -1);
      comp_grow_node_arrays(c);
      walk_scope(c, kbody, kws_idx, -1);
      int kargsn = nt_ref(nt, id, "arguments");
      int kan = 0;
      const int *kav = kargsn >= 0 ? nt_arr(nt, kargsn, "arguments", &kan) : NULL;
      if (kan == 1 && kav) nt_node_set_str(nt, kav[0], "value", kwname);
      changed = 1;
      continue;
    }
    const char *sym = method_sym_arg(c, id);
    if (!sym || !sym[0] || sym[0] == '_') continue;   /* already rewritten / internal */
    /* a binary operator method (5.method(:+)) gets a 2-param wrapper below;
       other non-letter syms have no wrapper shape */
    static const char *const BAM_BINOPS[] = { "+", "-", "*", "/", "%", "**",
                                              "&", "|", "^", "<<", ">>", "<=>",
                                              "==", "!=", "<", "<=", ">", ">=",
                                              "=~", "[]", NULL };
    int binop = 0;
    if (!(sym[0] >= 'a' && sym[0] <= 'z')) {
      for (int k = 0; BAM_BINOPS[k]; k++) if (sp_streq(sym, BAM_BINOPS[k])) { binop = 1; break; }
      if (!binop) continue;
    }
    TyKind rt = infer_type(c, recv);
    /* A user-object receiver whose sym is an attr/struct accessor with NO real
       def has no callable target (accessors inline at the call site), so
       method(:attr) / method(:attr=) would fail. Synthesize the same __bam_
       wrapper whose body `__bam_r.attr` (or `__bam_r.attr = __bam_a`) inlines
       the accessor -- a writer takes the binop-style 2-param shape. A real def
       is already handled via the resolved-target path (#3110 follow-up). */
    if (ty_is_object(rt)) {
      int ocid = ty_object_class(rt);
      int obj_accessor = 0;
      if (ocid >= 0 && comp_method_in_chain(c, ocid, sym, NULL) < 0) {
        size_t sl = strlen(sym);
        if (sl > 1 && sym[sl - 1] == '=') {
          char basew[256];
          if (sl - 1 < sizeof basew) {
            memcpy(basew, sym, sl - 1); basew[sl - 1] = '\0';
            if (comp_writer_in_chain(c, ocid, basew, NULL)) { obj_accessor = 1; binop = 1; }
          }
        }
        else if (comp_reader_in_chain(c, ocid, sym, NULL)) {
          obj_accessor = 1;
        }
      }
      if (!obj_accessor) continue;                    /* real def / non-accessor: handled elsewhere */
    }
    else if (rt == TY_UNKNOWN || rt == TY_POLY || rt == TY_VOID || rt == TY_NIL ||
             rt == TY_CLASS || rt == TY_METHOD || rt == TY_PROC)
      continue;
    /* the typed-array (kind, op) trampoline path owns these */
    if (ty_is_array(rt) && sp_streq(sym, "push")) continue;
    if (comp_method_index(c, sym) >= 0) continue;     /* a same-named top-level def wins */
    /* an undefined name must reach codegen's immediate NameError, not become
       a wrapper whose body call aborts the build (#2752) */
    {
      const char *bcls = rt == TY_STRING ? "String" : rt == TY_INT ? "Integer"
                       : rt == TY_FLOAT ? "Float" : rt == TY_SYMBOL ? "Symbol"
                       : ty_is_array(rt) ? "Array" : ty_is_hash(rt) ? "Hash"
                       : rt == TY_RANGE ? "Range" : rt == TY_TIME ? "Time"
                       : rt == TY_BOOL ? "Object" : NULL;
      /* TrueClass/FalseClass define the logical operators (#2835) */
      int bool_op = rt == TY_BOOL &&
                    (sp_streq(sym, "&") || sp_streq(sym, "|") || sp_streq(sym, "^"));
      if (bcls && !bool_op &&
          !builtin_method_known(bcls, sym) && !builtin_object_method_known(sym))
        continue;
    }
    char wname[48];
    snprintf(wname, sizeof wname, "__bam_%d", id);
    if (comp_method_index(c, wname) >= 0) continue;   /* synthesized on a prior pass */
    /* def __bam_<id>(__bam_r) = __bam_r.<sym>, or for a binary operator
       def __bam_<id>(__bam_r, __bam_a) = __bam_r <op> __bam_a */
    int preqs[2];
    int rp = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, rp, "name", "__bam_r");
    preqs[0] = rp;
    int params = nt_new_node(nt, "ParametersNode");
    if (binop) {
      int ap = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, ap, "name", "__bam_a");
      preqs[1] = ap;
      nt_node_set_arr(nt, params, "requireds", preqs, 2);
    }
    else nt_node_set_arr(nt, params, "requireds", preqs, 1);
    int rread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, rread, "name", "__bam_r");
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", sym);
    nt_node_set_ref(nt, call, "receiver", rread);
    if (binop) {
      int aread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, aread, "name", "__bam_a");
      int cargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, cargs, "arguments", &aread, 1);
      nt_node_set_ref(nt, call, "arguments", cargs);
    }
    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &call, 1);
    int def = nt_new_node(nt, "DefNode");
    nt_node_set_str(nt, def, "name", wname);
    nt_node_set_ref(nt, def, "parameters", params);
    nt_node_set_ref(nt, def, "body", body);
    Scope *ws = comp_scope_new(c, wname, def);
    ws->class_id = -1;
    ws->body = body;
    int ws_idx = c->nscopes - 1;
    scope_add_param(ws, "__bam_r", -1);
    if (binop) scope_add_param(ws, "__bam_a", -1);
    LocalVar *plv = scope_local(ws, "__bam_r");
    if (plv) { plv->type = rt; plv->rbs_seeded = 1; }  /* pin: no call sites exist */
    comp_grow_node_arrays(c);
    walk_scope(c, body, ws_idx, -1);
    /* retarget the Method at the wrapper */
    int argsn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
    if (an == 1 && av) nt_node_set_str(nt, av[0], "value", wname);
    changed = 1;
  }
  return changed;
}

/* An under-supplied call (`m(1)` against `def m(a, b)`) raises ArgumentError
   at runtime (args_raise), so the missing params never carry a real value --
   but with NO other call site they also never receive a type, the body can't
   infer, and the method's return stays TY_UNKNOWN, which rejects any VALUE
   use of the call (`puts m(1)`). Give such never-supplied required params a
   concrete placeholder type: every call missing them raises before the body
   runs, so the choice is unobservable; a later legitimate call site simply
   unifies over it. */
static int pad_unsupplied_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int mi = comp_method_index(c, name);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    if (m->rest_idx >= 0 || m->kwrest_idx >= 0 || m->npost_rest > 0) continue;
    int argsn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
    int fuzzy = 0, kwh = -1;
    for (int j = 0; j < an; j++) {
      const char *aty = av[j] >= 0 ? nt_type(nt, av[j]) : NULL;
      if (aty && (sp_streq(aty, "SplatNode") || sp_streq(aty, "ForwardingArgumentsNode")))
        fuzzy = 1;
      else if (aty && sp_streq(aty, "KeywordHashNode") && j == an - 1)
        kwh = av[j];
    }
    if (fuzzy) continue;
    int pos_an = kwh >= 0 ? an - 1 : an;
    for (int i = 0; i < m->nparams; i++) {
      if (i < pos_an) continue;                       /* supplied positionally */
      if (!m->pnames[i]) continue;
      if (m->pdefault[i] >= 0) continue;              /* optional: has a default */
      if (kwh >= 0 && kwh_lookup(nt, kwh, m->pnames[i]) >= 0) continue;  /* supplied by keyword */
      if (m->pnames[i][0] == '_' && m->pnames[i][1] == '_') continue;    /* synthesized */
      LocalVar *lv = scope_local(m, m->pnames[i]);
      if (lv && lv->type == TY_UNKNOWN) { lv->type = TY_INT; changed = 1; }
    }
  }
  return changed;
}

/* An inline `Hash.new` / `Hash.new(default)` in ARGUMENT position has no
   variable whose key/value usage could pick its hash variant, so it infers
   TY_UNKNOWN forever and codegen rejects the call -- which breaks the common
   counting idiom each_with_object(Hash.new(0)). Rename the call to the
   internal `__hash_new_default`, which infers (stably) as the
   universally-boxed PolyPoly variant. */
/* `m(*[a, b])`: a splat of an ARRAY LITERAL expands to its elements at
   compile time so builtin methods and proc calls (which have no runtime
   splat path) take plain positionals -- min(*[2]), add.(*[3, 4]). USER
   methods keep the runtime splat shape: emit_args_filled handles it and
   carries the correct ArgumentError timing for arity mismatches. Type-aware
   (user-object receivers excluded), hence in the fixpoint. */
static int expand_literal_splat_args(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) {
      if (comp_method_index(c, nm) >= 0 || comp_included_method_index(c, nm) >= 0)
        continue;   /* a user method: native splat path */
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (rt == TY_UNKNOWN || ty_is_object(rt) || rt == TY_CLASS) continue;
    }
    int argsn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
    int sp_at = -1;
    for (int j = 0; j < an; j++) {
      const char *aty = av[j] >= 0 ? nt_type(nt, av[j]) : NULL;
      if (aty && sp_streq(aty, "SplatNode")) { sp_at = j; break; }
    }
    if (sp_at < 0 || an > 32) continue;
    int inner = nt_ref(nt, av[sp_at], "expression");
    const char *ity = inner >= 0 ? nt_type(nt, inner) : NULL;
    if (!ity || !sp_streq(ity, "ArrayNode")) continue;
    int en = 0;
    const int *ev = nt_arr(nt, inner, "elements", &en);
    int nested = 0;
    for (int j = 0; j < en; j++) {
      const char *ety = ev[j] >= 0 ? nt_type(nt, ev[j]) : NULL;
      if (ety && sp_streq(ety, "SplatNode")) nested = 1;
    }
    if (nested || en > 32 || an - 1 + en > 64) continue;
    int na[96];
    int m0 = 0;
    for (int j = 0; j < sp_at; j++) na[m0++] = av[j];
    for (int j = 0; j < en; j++) na[m0++] = ev[j];
    for (int j = sp_at + 1; j < an; j++) na[m0++] = av[j];
    nt_node_set_arr((NodeTable *)nt, (int)argsn, "arguments", na, m0);
    changed = 1;
  }
  return changed;
}

/* A Symbol receiver delegates its STRING-flavored methods (regex match,
   pattern slice, comparisons) through the name text: interpose .to_s so the
   string machinery serves them. Symbol-native fast paths (==, to_s, case
   conversions, succ) keep their own arms. */
/* `arr.lazy.<stage>...`: uniq / chunk_while / slice_when / with_index /
   each_with_index carry state across elements, so they do not fuse into the
   lazy loop and the whole chain was rejected as opaque. Over a FINITE source
   the lazy and eager forms agree (CRuby's lazy uniq.to_a over an endless
   source never terminates either), so drop the `.lazy` and let the eager
   array path serve it. An endless range keeps its lazy chain and its
   (still unsupported) reject. (#2993) */
/* A string range serves only its endpoint/membership face natively; every
   other method rides the materialized element array. Type-driven, so it also
   catches a range held in a variable -- which is why it lives in the
   inference fixpoint rather than the one-shot shape pass (#3064). */
static int desugar_str_range_methods(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  static const char *const range_native[] = {
    "begin", "end", "min", "max", "include?", "member?", "cover?", "===",
    "exclude_end?", "==", "!=", "eql?", "inspect", "to_s", "class",
    "frozen?", "freeze", "itself", "dup", "clone", "hash",
    /* to_a IS the materializer: interposing it would recurse */
    "to_a", "entries",
    /* Range#size counts integer elements: nil for a string range */
    "size", NULL };
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0) continue;
    if (infer_type(c, recv) != TY_STR_RANGE) continue;
    int argn = nt_ref(nt, id, "arguments");
    int an = 0;
    if (argn >= 0) nt_arr(nt, argn, "arguments", &an);
    int native = 0;
    for (int j = 0; range_native[j]; j++)
      if (sp_streq(nm, range_native[j])) { native = 1; break; }
    /* first/last are the endpoints bare, a prefix/suffix ARRAY with a count */
    if (!native && an == 0 && (sp_streq(nm, "first") || sp_streq(nm, "last"))) native = 1;
    if (native) continue;
    int toa = nt_new_node(nt, "CallNode");
    if (toa < 0) continue;
    nt_node_set_str(nt, toa, "name", "to_a");
    nt_node_set_ref(nt, toa, "receiver", recv);
    nt_node_set_ref(nt, id, "receiver", toa);
    comp_grow_node_arrays(c);
    c->nscope[toa] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `SYM.to_proc.call(recv, *args)` is just `recv.SYM(*args)` -- Ruby's
   Symbol#to_proc proc takes the receiver first and forwards the rest. The
   synthesized lambda has a fixed parameter count (1, or 2 for a binary
   operator), so any other shape raised ArgumentError. Rewriting the direct
   chain sidesteps the proc entirely, and needs no dynamic dispatch. (#3097) */
static int desugar_sym_to_proc_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") &&
                !sp_streq(nm, "yield") && !sp_streq(nm, "[]"))) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    int tp = nt_ref(nt, id, "receiver");
    if (tp < 0 || !nt_type(nt, tp) || !sp_streq(nt_type(nt, tp), "CallNode")) continue;
    const char *tpn = nt_str(nt, tp, "name");
    if (!tpn || !sp_streq(tpn, "to_proc")) continue;
    int symn = nt_ref(nt, tp, "receiver");
    if (symn < 0) continue;
    const char *sym = sym_static_value(c, symn);
    if (!sym || !*sym) continue;
    int argn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argn >= 0 ? nt_arr(nt, argn, "arguments", &an) : NULL;
    if (!av || an < 1) continue;                /* needs at least the receiver */
    for (int j = 0; j < an; j++)                /* a splat keeps the proc path */
      if (nt_type(nt, av[j]) && sp_streq(nt_type(nt, av[j]), "SplatNode")) { an = 0; break; }
    if (an < 1) continue;
    int rest = nt_new_node(nt, "ArgumentsNode");
    if (rest < 0) continue;
    nt_node_set_arr(nt, rest, "arguments", av + 1, an - 1);
    nt_node_set_str(nt, id, "name", sym);
    nt_node_set_ref(nt, id, "receiver", av[0]);
    nt_node_set_ref(nt, id, "arguments", an > 1 ? rest : -1);
    comp_grow_node_arrays(c);
    c->nscope[rest] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `arr.reduce(:gcd)` / `inject(init, :lcm)`: a Symbol naming an ordinary
   method (not an operator) folds pairwise through that method. The operator
   symbols ride the typed/boxed fold emitters; rewrite the method-naming form
   into the equivalent block, which the block fold already serves. (#3125) */
static int desugar_reduce_method_symbol(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject"))) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    int argn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argn >= 0 ? nt_arr(nt, argn, "arguments", &an) : NULL;
    if (!av || an < 1 || an > 2) continue;
    const char *sym = sym_static_value(c, av[an - 1]);
    if (!sym || !*sym) continue;
    /* only an identifier-named method: operators keep their fold emitters */
    if (!((sym[0] >= 'a' && sym[0] <= 'z') || (sym[0] >= 'A' && sym[0] <= 'Z') || sym[0] == '_'))
      continue;
    char pa[32], pb[32];
    snprintf(pa, sizeof pa, "__rms_a_%d", id);
    snprintf(pb, sizeof pb, "__rms_b_%d", id);
    int base = nt->count;
    int kpa = nt_new_node(nt, "RequiredParameterNode");
    int kpb = nt_new_node(nt, "RequiredParameterNode");
    if (kpa < 0 || kpb < 0) continue;
    nt_node_set_str(nt, kpa, "name", pa);
    nt_node_set_str(nt, kpb, "name", pb);
    int preq[2] = { kpa, kpb };
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", preq, 2);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int ra = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, ra, "name", pa);
    int rb2 = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, rb2, "name", pb);
    int cargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, cargs, "arguments", &rb2, 1);
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", sym);
    nt_node_set_ref(nt, call, "receiver", ra);
    nt_node_set_ref(nt, call, "arguments", cargs);
    int blkbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, blkbody, "body", &call, 1);
    int blk = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blk, "parameters", bparams);
    nt_node_set_ref(nt, blk, "body", blkbody);
    nt_node_set_ref(nt, id, "block", blk);
    /* drop the symbol argument, keeping any leading init */
    if (an == 2) nt_node_set_arr(nt, argn, "arguments", av, 1);
    else nt_node_set_ref(nt, id, "arguments", -1);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

static int desugar_lazy_stateful_stage(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  static const char *const stateful[] = {
    "uniq", "chunk_while", "slice_when", "with_index", "each_with_index",
    /* not stateful, but unfused: over a finite source the eager forms agree
       (#3127) */
    "compact", "zip", NULL };
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "lazy") || nt_ref(nt, id, "block") >= 0) continue;
    int src = nt_ref(nt, id, "receiver");
    if (src < 0) continue;
    /* only a finite source: a typed array, or an array literal */
    TyKind st = infer_type(c, src);
    int finite = ty_is_array(st) ||
                 (nt_type(nt, src) && sp_streq(nt_type(nt, src), "ArrayNode"));
    if (!finite) continue;
    /* does any consumer up the chain need cross-element state? */
    int node = id, hit = 0;
    for (int depth = 0; depth < 16 && !hit; depth++) {
      int call = -1;
      for (int k = 0; k < nt->count; k++)
        if (nt_type(nt, k) && sp_streq(nt_type(nt, k), "CallNode") &&
            nt_ref(nt, k, "receiver") == node) { call = k; break; }
      if (call < 0) break;
      const char *cn = nt_str(nt, call, "name");
      if (!cn) break;
      for (int i = 0; stateful[i]; i++) if (sp_streq(cn, stateful[i])) { hit = 1; break; }
      /* with_index attaches to an enumerator, not to the bare array */
      if (hit && sp_streq(cn, "with_index")) hit = 2;
      node = call;
    }
    if (!hit) continue;
    /* Rewrite the `lazy` call in place: `each` for the stages that want an
       enumerator receiver, otherwise splice it out so the consumer reads the
       source array directly. */
    if (hit == 2) { nt_node_set_str(nt, id, "name", "each"); }
    else {
      for (int k = 0; k < nt->count; k++)
        if (nt_type(nt, k) && sp_streq(nt_type(nt, k), "CallNode") &&
            nt_ref(nt, k, "receiver") == id)
          nt_node_set_ref(nt, k, "receiver", src);
    }
    changed = 1;
  }
  return changed;
}

static int desugar_symbol_string_methods(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  static const char *const SYMSTR[] = {
    "match", "match?", "=~", "[]", "slice", "start_with?", "end_with?",
    "between?", "<", "<=", ">", ">=", "<=>", "count", "index", "tr",
    "sub", "gsub", NULL };
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int hit = 0;
    for (int j = 0; SYMSTR[j]; j++) if (sp_streq(nm, SYMSTR[j])) { hit = 1; break; }
    if (!hit) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || infer_type(c, recv) != TY_SYMBOL) continue;
    /* Symbol#<=> is defined only between Symbols: reading both sides as text
       would make :a <=> "a" answer 0 where Ruby answers nil, so leave a
       non-Symbol operand alone and let codegen emit the nil (#3081). */
    if (sp_streq(nm, "<=>")) {
      int cargs = nt_ref(nt, id, "arguments");
      int cn = 0;
      const int *cv = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cn) : NULL;
      if (cn == 1 && cv && cv[0] >= 0) {
        TyKind cat = infer_type(c, cv[0]);
        if (cat != TY_SYMBOL && cat != TY_POLY && cat != TY_UNKNOWN) continue;
      }
    }
    /* symbol-to-symbol comparisons need BOTH sides as text */
    int toa = nt_new_node(nt, "CallNode");
    if (toa < 0) continue;
    nt_node_set_str(nt, toa, "name", "to_s");
    nt_node_set_ref(nt, toa, "receiver", recv);
    nt_node_set_ref(nt, id, "receiver", toa);
    comp_grow_node_arrays(c);
    c->nscope[toa] = c->nscope[id];
    /* symbol ARGUMENTS to the comparisons also read as their names */
    int argsn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
    for (int j = 0; j < an && j < 8; j++) {
      if (av[j] >= 0 && infer_type(c, av[j]) == TY_SYMBOL) {
        int t2 = nt_new_node(nt, "CallNode");
        if (t2 < 0) continue;
        nt_node_set_str(nt, t2, "name", "to_s");
        nt_node_set_ref(nt, t2, "receiver", av[j]);
        comp_grow_node_arrays(c);
        c->nscope[t2] = c->nscope[id];
        int na2[8];
        const int *av2 = nt_arr(nt, argsn, "arguments", &an);
        for (int k2 = 0; k2 < an && k2 < 8; k2++) na2[k2] = (k2 == j) ? t2 : av2[k2];
        nt_node_set_arr(nt, argsn, "arguments", na2, an);
        av = nt_arr(nt, argsn, "arguments", &an);
      }
    }
    changed = 1;
  }
  return changed;
}

/* `m(&op)` where op is a SYMBOL variable: rewrite the block argument into a
   real block calling `send(op)` -- the dynamic-send desugar then expands the
   send over op's statically-known candidate set. Comparator consumers
   (reduce/inject/sort/min/max/minmax) get the two-parameter form so operator
   symbols (`op = :+`) apply both arguments. A `sym_var.to_proc` call gets the
   same treatment as an explicit lambda. */
/* `m(&method(:Integer))` where :Integer names a Kernel builtin (no user def):
   the Method-object route has no C target for a builtin, so rewrite the block
   argument into an explicit one-parameter block calling the builtin:
   m { |__bmk_N| Integer(__bmk_N) }. User-defined names keep the Method path. */
static int desugar_kernel_method_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  static const char *const KFN[] = { "Integer", "Float", "String", "Array",
                                     "Rational", "Complex", "puts", "print",
                                     "p", "pp", NULL };
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0 || !nt_type(nt, ex) || !sp_streq(nt_type(nt, ex), "CallNode")) continue;
    const char *exn = nt_str(nt, ex, "name");
    if (!exn || !sp_streq(exn, "method")) continue;
    if (nt_ref(nt, ex, "receiver") >= 0) continue;      /* recv.method(:x): wrapper path */
    const char *sym = method_sym_arg(c, ex);
    if (!sym) continue;
    int known = 0;
    for (int k = 0; KFN[k]; k++) if (sp_streq(sym, KFN[k])) { known = 1; break; }
    if (!known) continue;
    if (comp_method_index(c, sym) >= 0) continue;       /* a user def wins */
    char pn[32];
    snprintf(pn, sizeof pn, "__bmk_%d", id);
    int kpa = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, kpa, "name", pn);
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &kpa, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int ra = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, ra, "name", pn);
    int sargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, sargs, "arguments", &ra, 1);
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", sym);
    nt_node_set_ref(nt, call, "arguments", sargs);
    int blkbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, blkbody, "body", &call, 1);
    int blk2 = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blk2, "parameters", bparams);
    nt_node_set_ref(nt, blk2, "body", blkbody);
    comp_grow_node_arrays(c);
    int ns2[8] = { kpa, params, bparams, ra, sargs, call, blkbody, blk2 };
    for (int j = 0; j < 8; j++) c->nscope[ns2[j]] = c->nscope[id];
    Scope *sc2 = comp_scope_of(c, id);
    if (sc2) {
      LocalVar *lva = scope_local_intern(sc2, pn);
      if (lva) lva->is_block_param = 1;
    }
    nt_node_set_ref(nt, id, "block", blk2);
    changed = 1;
  }
  return changed;
}

/* `m(&h)` where h is a Hash: Hash#to_proc -- rewrite the block argument into
   an explicit one-parameter block indexing the hash (`m { |x| h[x] }`). */
static int desugar_hash_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0 || !nt_type(nt, ex)) continue;
    const char *exty = nt_type(nt, ex);
    int ex_is_lit = sp_streq(exty, "HashNode") || sp_streq(exty, "KeywordHashNode");
    if (!ex_is_lit &&
        !(sp_streq(exty, "LocalVariableReadNode") && ty_is_hash(infer_type(c, ex)))) continue;
    char pn[32];
    snprintf(pn, sizeof pn, "__bhp_%d", id);
    int kpa = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, kpa, "name", pn);
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &kpa, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int ra = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, ra, "name", pn);
    int sargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, sargs, "arguments", &ra, 1);
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", "[]");
    nt_node_set_ref(nt, call, "receiver", ex);
    nt_node_set_ref(nt, call, "arguments", sargs);
    int blkbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, blkbody, "body", &call, 1);
    int blk2 = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blk2, "parameters", bparams);
    nt_node_set_ref(nt, blk2, "body", blkbody);
    comp_grow_node_arrays(c);
    int ns2[7] = { kpa, params, bparams, ra, sargs, call, blkbody };
    for (int j = 0; j < 7; j++) c->nscope[ns2[j]] = c->nscope[id];
    c->nscope[blk2] = c->nscope[id];
    Scope *sc2 = comp_scope_of(c, id);
    if (sc2) {
      LocalVar *lva = scope_local_intern(sc2, pn);
      if (lva) lva->is_block_param = 1;
    }
    nt_node_set_ref(nt, id, "block", blk2);
    changed = 1;
  }
  return changed;
}

static int desugar_symbol_var_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int is_toproc = nm && sp_streq(nm, "to_proc");
    int ex = -1;
    if (is_toproc) {
      ex = nt_ref(nt, id, "receiver");
      if (nt_ref(nt, id, "block") >= 0 || nt_ref(nt, id, "arguments") >= 0) continue;
    }
    else {
      int blk = nt_ref(nt, id, "block");
      if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
      ex = nt_ref(nt, blk, "expression");
    }
    if (ex < 0 || !nt_type(nt, ex) || !sp_streq(nt_type(nt, ex), "LocalVariableReadNode")) continue;
    if (infer_type(c, ex) != TY_SYMBOL) continue;
    int two = 0;
    if (!is_toproc && nm)
      two = sp_streq(nm, "reduce") || sp_streq(nm, "inject") || sp_streq(nm, "sort") ||
            sp_streq(nm, "min") || sp_streq(nm, "max") || sp_streq(nm, "minmax");
    char pa[32], pb[32];
    snprintf(pa, sizeof pa, "__svp_a_%d", id);
    snprintf(pb, sizeof pb, "__svp_b_%d", id);
    int kpa = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, kpa, "name", pa);
    int preq[2] = { kpa, -1 };
    int npar = 1;
    if (two) {
      int kpb = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, kpb, "name", pb);
      preq[1] = kpb; npar = 2;
    }
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", preq, npar);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int ra = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, ra, "name", pa);
    int sargs = nt_new_node(nt, "ArgumentsNode");
    int sa[2] = { ex, -1 };
    int nsa = 1;
    if (two) {
      int rb2 = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, rb2, "name", pb);
      sa[1] = rb2; nsa = 2;
    }
    nt_node_set_arr(nt, sargs, "arguments", sa, nsa);
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", "send");
    nt_node_set_ref(nt, call, "receiver", ra);
    nt_node_set_ref(nt, call, "arguments", sargs);
    int blkbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, blkbody, "body", &call, 1);
    int blk2 = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blk2, "parameters", bparams);
    nt_node_set_ref(nt, blk2, "body", blkbody);
    comp_grow_node_arrays(c);
    /* new nodes belong to the call's scope */
    int ns2[16]; int nn2 = 0;
    ns2[nn2++] = kpa; if (two) ns2[nn2++] = preq[1];
    ns2[nn2++] = params; ns2[nn2++] = bparams; ns2[nn2++] = ra;
    if (two) ns2[nn2++] = sa[1];
    ns2[nn2++] = sargs; ns2[nn2++] = call; ns2[nn2++] = blkbody; ns2[nn2++] = blk2;
    for (int j = 0; j < nn2; j++) c->nscope[ns2[j]] = c->nscope[id];
    Scope *sc2 = comp_scope_of(c, id);
    if (sc2) {
      LocalVar *lva = scope_local_intern(sc2, pa);
      if (lva) lva->is_block_param = 1;
      if (two) {
        LocalVar *lvb = scope_local_intern(sc2, pb);
        if (lvb) lvb->is_block_param = 1;
      }
    }
    if (is_toproc) {
      /* sym_var.to_proc -> lambda { |x| x.send(sym_var) } */
      nt_node_set_str(nt, id, "name", "lambda");
      nt_node_set_ref(nt, id, "receiver", -1);
      nt_node_set_ref(nt, id, "block", blk2);
      nt_node_set_int(nt, id, "stp_arity", 1);   /* Symbol#to_proc is -2 (#3053) */
    }
    else
      nt_node_set_ref(nt, id, "block", blk2);
    changed = 1;
  }
  return changed;
}

static int pin_arg_position_hash_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int argsn = nt_ref(nt, id, "arguments");
    if (argsn < 0) continue;
    int an = 0;
    const int *av = nt_arr(nt, argsn, "arguments", &an);
    for (int j = 0; j < an; j++) {
      int a = av[j];
      if (a < 0) continue;
      const char *aty = nt_type(nt, a);
      if (!aty || !sp_streq(aty, "CallNode")) continue;
      const char *anm = nt_str(nt, a, "name");
      if (!anm || !sp_streq(anm, "new") || nt_ref(nt, a, "block") >= 0) continue;
      int arecv = nt_ref(nt, a, "receiver");
      if (arecv < 0) continue;
      const char *rty = nt_type(nt, arecv);
      if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) continue;
      const char *cn = nt_str(nt, arecv, "name");
      if (cn && sp_streq(cn, "Hash")) {
        nt_node_set_str(nt, a, "name", "__hash_new_default");
        changed = 1;
      }
    }
  }
  return changed;
}

static int name_is_math_fn(const char *nm) {
  static const char *const fns[] = {
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh",
    "tanh", "asinh", "acosh", "atanh", "exp", "log", "log2", "log10", "sqrt",
    "cbrt", "hypot", "ldexp", "erf", "erfc", "gamma", "lgamma", "frexp",
    "expm1", "log1p", NULL };
  for (int i = 0; fns[i]; i++) if (sp_streq(nm, fns[i])) return 1;
  return 0;
}

/* `include Math` exposes the module's functions as bare calls: rewrite
   `sqrt(x)` to `Math.sqrt(x)` so the existing Math-receiver machinery serves
   them (#2600). Only fires when the program includes Math and no user method of
   the same name shadows it; a bare call needs at least one argument (the Math
   functions are all n-ary), so a receiverless niladic call is never touched. */
/* Kernel's module functions, by name. A whitelist rather than "anything with
   the Kernel receiver": Kernel is also a VALUE, so `Kernel === 5` asks whether
   5 is in the Object hierarchy, and `Kernel.name` / `.to_s` / `.freeze` /
   `.instance_methods` are Module's own methods on it. Dropping the receiver for
   those would change what they mean. Only names that Module does not also
   answer belong here. */
static int kernel_module_function(const char *m) {
  static const char *const K[] = {
    "puts", "print", "p", "pp", "printf", "sprintf", "format",
    "raise", "fail", "exit", "exit!", "abort", "at_exit",
    "rand", "srand", "sleep", "gets", "loop", "lambda", "proc",
    "block_given?", "catch", "throw", "caller", "binding", "__method__",
    "require", "require_relative", "load", "warn", "system",
    "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex",
    NULL
  };
  for (int i = 0; K[i]; i++) if (sp_streq(m, K[i])) return 1;
  return 0;
}

/* `Kernel.puts x` / `Kernel.exit(1)` / `Kernel.format(...)`: Kernel's module
   functions are callable with the module as an explicit receiver, and mean
   exactly what the bare call means. Nothing served that receiver, so every one
   of them raised "undefined method 'exit' for class Kernel" -- including the
   `lambda { |status| Kernel.exit(status) }` idiom for making the exit path
   injectable in a CLI library.

   Drop the receiver and let the bare-call machinery handle it. Skipped when the
   program defines its own Kernel, where the name means whatever it says. */
int desugar_kernel_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (comp_class_index(c, "Kernel") >= 0) return 0;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "ConstantReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "Kernel")) continue;
    const char *mn = nt_str(nt, id, "name");
    if (!mn || !kernel_module_function(mn)) continue;
    nt_node_set_ref(nt, id, "receiver", -1);
    changed = 1;
  }
  return changed;
}

int desugar_include_math(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int has_include = 0;
  for (int id = 0; id < n0 && !has_include; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "include")) continue;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *aa = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      const char *at = nt_type(nt, aa[j]);
      if (at && sp_streq(at, "ConstantReadNode") && nt_str(nt, aa[j], "name") &&
          sp_streq(nt_str(nt, aa[j], "name"), "Math")) { has_include = 1; break; }
    }
  }
  if (!has_include) return 0;
  c->has_include_math = 1;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !name_is_math_fn(nm)) continue;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0;
    if (anode >= 0) nt_arr(nt, anode, "arguments", &an);
    if (an < 1) continue;
    /* a user def of this name (any scope) shadows the Math function */
    int shadowed = 0;
    for (int s = 0; s < c->nscopes && !shadowed; s++)
      if (c->scopes[s].name && sp_streq(c->scopes[s].name, nm)) shadowed = 1;
    if (shadowed) continue;
    int mc = nt_new_node(nt, "ConstantReadNode");
    if (mc < 0) continue;
    nt_node_set_str(nt, mc, "name", "Math");
    nt_node_set_ref(nt, id, "receiver", mc);
    comp_grow_node_arrays(c);
    c->nscope[mc] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_enum_method_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    /* hash.map.with_index { } / hash.each.with_index { }: interpose to_a so
       the pair-array enumerator chain (which the array machinery serves)
       carries it -- h.to_a.map.with_index. Type-aware, hence here and not in
       the one-shot pre-pass. */
    if (nm && (sp_streq(nm, "with_index") || sp_streq(nm, "with_object"))) {
      int wrecv = nt_ref(nt, id, "receiver");
      if (wrecv >= 0 && nt_type(nt, wrecv) && sp_streq(nt_type(nt, wrecv), "CallNode") &&
          nt_ref(nt, wrecv, "block") < 0) {
        const char *wrn = nt_str(nt, wrecv, "name");
        if (wrn && (sp_streq(wrn, "map") || sp_streq(wrn, "each") ||
                    sp_streq(wrn, "collect"))) {
          int hrecv = nt_ref(nt, wrecv, "receiver");
          if (hrecv >= 0 && ty_is_hash(infer_type(c, hrecv)) &&
              !(nt_type(nt, hrecv) && sp_streq(nt_type(nt, hrecv), "CallNode") &&
                nt_str(nt, hrecv, "name") && sp_streq(nt_str(nt, hrecv, "name"), "to_a"))) {
            int toa = nt_new_node(nt, "CallNode");
            if (toa >= 0) {
              nt_node_set_str(nt, toa, "name", "to_a");
              nt_node_set_ref(nt, toa, "receiver", hrecv);
              nt_node_set_ref(nt, wrecv, "receiver", toa);
              comp_grow_node_arrays(c);
              c->nscope[toa] = c->nscope[wrecv];
              changed = 1;
            }
          }
        }
      }
    }
    /* Enumerable#each_entry on a receiver whose #each yields ONE value per
       element is #each: same elements, same receiver as the value. That is
       every builtin enumerable -- an Array/Range/Enumerator element, a Hash
       pair (already packed), a Dir entry -- so rename and let the #each
       emitters serve it. Without this the call had no emitter at all for a
       Hash or a Range, and answered nil for an Array or a Dir (#3395).

       A user Enumerable is deliberately NOT rewritten: its #each may `yield a,
       b`, and there each_entry packs the pair where each spreads it. That path
       keeps its own machinery.

       Type-aware, hence here and not in the one-shot pre-pass; while the
       receiver type is still unsettled g_infer_optimistic holds the rewrite
       back rather than guessing. */
    if (nm && sp_streq(nm, "each_entry") && nt_ref(nt, id, "block") >= 0) {
      int erecv = nt_ref(nt, id, "receiver");
      if (erecv >= 0) {
        TyKind ert = infer_type(c, erecv);
        if (ert == TY_UNKNOWN) {
          if (!g_infer_optimistic) { /* settled and still unknown: leave it */ }
        }
        else if (ty_is_array(ert) || ty_is_hash(ert) || ert == TY_RANGE ||
                 ert == TY_ENUMERATOR || ert == TY_DIR) {
          nt_node_set_str(nt, id, "name", "each");
          changed = 1;
        }
      }
    }
    /* enum.to_set == Set.new(enum.to_a) whenever the set package's Set class
       is in the program (an in-place rewrite; Set's initialize adds each
       element, deduplicating).

       Type-aware, hence here and not in the one-shot pre-pass: a class that
       defines its OWN to_set has to keep it. The pre-pass could not tell --
       it runs before class collection, so it rewrote every to_set in the
       program and a `CookieJar#to_set` returning a Hash became a Set, with
       the error landing on whatever the real return type supported several
       lines later (#3378). While the receiver's type is still unresolved,
       wait rather than guess: g_infer_optimistic says the fixpoint has more
       to say. */
    if (nm && sp_streq(nm, "to_set") && nt_ref(nt, id, "block") < 0 &&
        nt_ref(nt, id, "arguments") < 0 && comp_class_index(c, "Set") >= 0) {
      int recv = nt_ref(nt, id, "receiver");
      if (recv >= 0) {
        TyKind rt = infer_type(c, recv);
        int decline = 0;
        if (rt == TY_UNKNOWN && g_infer_optimistic) decline = 1;   /* not yet known */
        else if (ty_is_object(rt) &&
                 comp_method_in_chain(c, ty_object_class(rt), "to_set", NULL) >= 0)
          decline = 1;                                             /* the class owns the name */
        if (!decline) {
          int cst = nt_new_node(nt, "ConstantReadNode");
          int one = nt_new_node(nt, "ArgumentsNode");
          /* Materialize the receiver to a flat array first: Set#initialize
             iterates its arg via `enum.each`, and a struct/user object passed
             as a poly value does not dispatch #each through the poly path.
             to_a rides the struct-native / __enum_to_a machinery, and is a
             no-op copy for array/range/hash receivers. */
          int toa = nt_new_node(nt, "CallNode");
          if (cst >= 0 && one >= 0 && toa >= 0) {
            nt_node_set_str(nt, toa, "name", "to_a");
            nt_node_set_ref(nt, toa, "receiver", recv);
            nt_node_set_str(nt, cst, "name", "Set");
            nt_node_set_arr(nt, one, "arguments", &toa, 1);
            nt_node_set_str(nt, id, "name", "new");
            nt_node_set_ref(nt, id, "receiver", cst);
            nt_node_set_ref(nt, id, "arguments", one);
            comp_grow_node_arrays(c);
            c->nscope[toa] = c->nscope[id];
            c->nscope[cst] = c->nscope[id];
            changed = 1;
            continue;
          }
        }
      }
    }
    /* Explicit `self.` receiver inside a class-method body: self IS the
       class there, so a call that resolves to a sibling/inherited class
       method drops the receiver and rides the bare-call cmethod dispatch
       (`def self.go; self.maker; end`). Writers (self.x = v) and names with
       no matching class method (self.class, self.name, ...) keep their
       receiver. */
    {
      int srecv = nt_ref(nt, id, "receiver");
      if (nm && srecv >= 0 && nt_type(nt, srecv) &&
          sp_streq(nt_type(nt, srecv), "SelfNode") &&
          nm[0] && nm[strlen(nm) - 1] != '=') {
        Scope *ssc = comp_scope_of(c, id);
        if (ssc && ssc->is_cmethod && ssc->class_id >= 0 &&
            comp_cmethod_in_chain(c, ssc->class_id, nm, NULL) >= 0) {
          nt_node_set_ref(nt, id, "receiver", -1);
          changed = 1;
          continue;
        }
      }
    }
    /* Array#entries is #to_a; the array emitters only know to_a. */
    if (nm && sp_streq(nm, "entries") && nt_ref(nt, id, "block") < 0) {
      int erecv2 = nt_ref(nt, id, "receiver");
      int ea2 = nt_ref(nt, id, "arguments");
      int eac2 = 0;
      if (ea2 >= 0) nt_arr(nt, ea2, "arguments", &eac2);
      if (erecv2 >= 0 && eac2 == 0 && ty_is_array(infer_type(c, erecv2))) {
        nt_node_set_str(nt, id, "name", "to_a");
        changed = 1;
        continue;
      }
    }
    /* poly-array sum { blk } == map { blk }.sum (the typed-array redispatch
       serves int/float receivers natively) */
    if (nm && sp_streq(nm, "sum") && nt_ref(nt, id, "block") >= 0) {
      int srecv = nt_ref(nt, id, "receiver");
      int sa = nt_ref(nt, id, "arguments");
      int sac = 0;
      if (sa >= 0) nt_arr(nt, sa, "arguments", &sac);
      if (srecv >= 0 && sac == 0 && infer_type(c, srecv) == TY_POLY_ARRAY) {
        int mapc = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, mapc, "name", "map");
        nt_node_set_ref(nt, mapc, "receiver", srecv);
        nt_node_set_ref(nt, mapc, "block", nt_ref(nt, id, "block"));
        nt_node_set_str(nt, id, "name", "sum");
        nt_node_set_ref(nt, id, "receiver", mapc);
        nt_node_set_ref(nt, id, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[mapc] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    /* Range no-block enumerator chains: interpose to_a so the array machinery
       serves them ((1..5).each_with_index.to_a, (1..3).map.with_index { },
       (1..3).cycle.first(7)). Blockless `each` (and each_slice/each_cons,
       whose runtime ctors take any boxed source) keep their first-class
       Enumerator arms. A beginless/endless literal has no array to build and
       falls through to the loud reject. */
    if (nm && nt_ref(nt, id, "block") < 0) {
      static const char *const RENUM0[] = { "each_with_index", "each_index",
                                            "map", "collect", "cycle", NULL };
      int rargs = nt_ref(nt, id, "arguments");
      int rac = 0;
      if (rargs >= 0) nt_arr(nt, rargs, "arguments", &rac);
      int hit = 0;
      if (rac == 0) { for (int k = 0; RENUM0[k]; k++) if (sp_streq(nm, RENUM0[k])) { hit = 1; break; } }
      if (hit) {
        int rrecv = nt_ref(nt, id, "receiver");
        if (rrecv >= 0 && infer_type(c, rrecv) == TY_RANGE) {
          int rlit = rrecv;
          while (rlit >= 0 && nt_type(nt, rlit) && sp_streq(nt_type(nt, rlit), "ParenthesesNode")) {
            int pb = nt_ref(nt, rlit, "body");
            int pn = 0;
            const int *pv = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
            rlit = (pn == 1 && pv) ? pv[0] : -1;
          }
          int open_ended = rlit >= 0 && nt_type(nt, rlit) && sp_streq(nt_type(nt, rlit), "RangeNode") &&
                           (nt_ref(nt, rlit, "left") < 0 || nt_ref(nt, rlit, "right") < 0);
          if (!open_ended) {
            int toa = nt_new_node(nt, "CallNode");
            nt_node_set_str(nt, toa, "name", "to_a");
            nt_node_set_ref(nt, toa, "receiver", rrecv);
            nt_node_set_ref(nt, id, "receiver", toa);
            comp_grow_node_arrays(c);
            c->nscope[toa] = c->nscope[id];
            changed = 1;
            continue;
          }
        }
      }
    }
    /* <stored enumerator>.with_object(memo) { }: drain to an array and ride
       the array each_with_object machinery (covers both statement and value
       forms, with the memo's own typing). Type-aware, hence fixpoint. */
    if (nm && sp_streq(nm, "with_object")) {
      int erecv = nt_ref(nt, id, "receiver");
      int eblk = nt_ref(nt, id, "block");
      int ea = nt_ref(nt, id, "arguments");
      int eac = 0;
      if (ea >= 0) nt_arr(nt, ea, "arguments", &eac);
      if (erecv >= 0 && eblk >= 0 && eac == 1 && infer_type(c, erecv) == TY_ENUMERATOR) {
        int toa = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, toa, "name", "to_a");
        nt_node_set_ref(nt, toa, "receiver", erecv);
        nt_node_set_str(nt, id, "name", "each_with_object");
        nt_node_set_ref(nt, id, "receiver", toa);
        comp_grow_node_arrays(c);
        c->nscope[toa] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    /* min_by(n)/max_by(n) { b }: sort_by { b } then take the n smallest /
       largest (largest reversed, matching CRuby's descending max_by order) */
    if (nm && (sp_streq(nm, "min_by") || sp_streq(nm, "max_by"))) {
      int mrc = nt_ref(nt, id, "receiver");
      int mbk = nt_ref(nt, id, "block");
      int ma = nt_ref(nt, id, "arguments");
      int mac = 0;
      const int *mav = ma >= 0 ? nt_arr(nt, ma, "arguments", &mac) : NULL;
      if (mrc >= 0 && mbk >= 0 && mac == 1 && ty_is_hash(infer_type(c, mrc))) {
        /* hash receivers only: arrays already have a native count form */
        int is_max = nm[1] == 'a';
        int sortc = nt_new_node(nt, "CallNode");
        nt_node_set_str(nt, sortc, "name", "sort_by");
        nt_node_set_ref(nt, sortc, "receiver", mrc);
        nt_node_set_ref(nt, sortc, "block", mbk);
        if (is_max) {
          int lastc = nt_new_node(nt, "CallNode");
          int largs = nt_new_node(nt, "ArgumentsNode");
          int mn = mav[0];
          nt_node_set_arr(nt, largs, "arguments", &mn, 1);
          nt_node_set_str(nt, lastc, "name", "last");
          nt_node_set_ref(nt, lastc, "receiver", sortc);
          nt_node_set_ref(nt, lastc, "arguments", largs);
          nt_node_set_str(nt, id, "name", "reverse");
          nt_node_set_ref(nt, id, "receiver", lastc);
          nt_node_set_ref(nt, id, "arguments", -1);
          nt_node_set_ref(nt, id, "block", -1);
          comp_grow_node_arrays(c);
          c->nscope[sortc] = c->nscope[id];
          c->nscope[lastc] = c->nscope[id];
          c->nscope[largs] = c->nscope[id];
        }
        else {
          int fargs = nt_new_node(nt, "ArgumentsNode");
          int mn = mav[0];
          nt_node_set_arr(nt, fargs, "arguments", &mn, 1);
          nt_node_set_str(nt, id, "name", "first");
          nt_node_set_ref(nt, id, "receiver", sortc);
          nt_node_set_ref(nt, id, "arguments", fargs);
          nt_node_set_ref(nt, id, "block", -1);
          comp_grow_node_arrays(c);
          c->nscope[sortc] = c->nscope[id];
          c->nscope[fargs] = c->nscope[id];
        }
        changed = 1;
        continue;
      }
    }
    /* Hash[k: v, ...] with a keyword-hash argument IS the hash literal */
    if (nm && sp_streq(nm, "[]")) {
      int krc = nt_ref(nt, id, "receiver");
      const char *krt2 = krc >= 0 ? nt_type(nt, krc) : NULL;
      if (krt2 && sp_streq(krt2, "ConstantReadNode") &&
          nt_str(nt, krc, "name") && sp_streq(nt_str(nt, krc, "name"), "Hash")) {
        int ka = nt_ref(nt, id, "arguments");
        int kac = 0;
        const int *kav = ka >= 0 ? nt_arr(nt, ka, "arguments", &kac) : NULL;
        if (kac == 1 && kav && nt_type(nt, kav[0]) &&
            sp_streq(nt_type(nt, kav[0]), "KeywordHashNode")) {
          /* rewrite this call node into a plain HashNode with the same
             element list */
          int en2 = 0;
          const int *els2 = nt_arr(nt, kav[0], "elements", &en2);
          int hn2 = nt_new_node(nt, "HashNode");
          nt_node_set_arr(nt, hn2, "elements", els2, en2);
          comp_grow_node_arrays(c);
          c->nscope[hn2] = c->nscope[id];
          /* graft: turn the CallNode into itself... easiest is retarget via
             receiver replacement not possible; instead rewrite in place by
             changing this node's type is unsupported -- wrap: make the call
             `(hash_literal).itself`-free by pointing the parent at hn2 is
             also unavailable here, so emit-side handles it; mark via rename */
          nt_node_set_str(nt, id, "name", "__hash_brackets_kw");
          nt_node_set_ref(nt, id, "receiver", hn2);
          nt_node_set_ref(nt, id, "arguments", -1);
          changed = 1;
        }
      }
      continue;
    }
    /* Hash#store(k, v) is exactly []= (whose value form already works) */
    if (nm && sp_streq(nm, "store")) {
      int hrc = nt_ref(nt, id, "receiver");
      int ha = nt_ref(nt, id, "arguments");
      int hac = 0;
      if (ha >= 0) nt_arr(nt, ha, "arguments", &hac);
      if (hrc >= 0 && hac == 2 && nt_ref(nt, id, "block") < 0 &&
          ty_is_hash(infer_type(c, hrc))) {
        nt_node_set_str(nt, id, "name", "[]=");
        changed = 1;
      }
      continue;
    }
    /* struct[member_literal] = v rewrites to the generated writer, so the
       member's type unifies with the value like any accessor write */
    if (nm && sp_streq(nm, "[]=")) {
      int wrc = nt_ref(nt, id, "receiver");
      int wa = nt_ref(nt, id, "arguments");
      int wac = 0;
      const int *wav = wa >= 0 ? nt_arr(nt, wa, "arguments", &wac) : NULL;
      TyKind wrt = wrc >= 0 ? infer_type(c, wrc) : TY_UNKNOWN;
      if (wac == 2 && ty_is_object(wrt) && c->classes[ty_object_class(wrt)].is_struct) {
        ClassInfo *wsc = &c->classes[ty_object_class(wrt)];
        int wmi = struct_member_idx(c, wsc, wav[0]);
        if (wmi >= 0) {
          char wn[300]; snprintf(wn, sizeof wn, "%s=", wsc->ivars[wmi] + 1);
          int one = nt_new_node(nt, "ArgumentsNode");
          if (one >= 0) {
            int varg = wav[1];
            nt_node_set_arr(nt, one, "arguments", &varg, 1);
            nt_node_set_str(nt, id, "name", wn);
            nt_node_set_ref(nt, id, "arguments", one);
            comp_grow_node_arrays(c);
            c->nscope[one] = c->nscope[id];
            changed = 1;
          }
        }
      }
      continue;
    }
    if (nm && (sp_streq(nm, "default=") || sp_streq(nm, "default"))) {
      /* default access on an un-narrowed empty-hash local: give it the
         symbol-keyed poly variant so the setter has a slot to store into
         (`a = {}; a.default = 9; a.default`) */
      int drc = nt_ref(nt, id, "receiver");
      if (drc >= 0 && nt_type(nt, drc) &&
          sp_streq(nt_type(nt, drc), "LocalVariableReadNode") &&
          infer_type(c, drc) == TY_UNKNOWN) {
        Scope *dsc = comp_scope_of(c, drc);
        const char *dvn = nt_str(nt, drc, "name");
        LocalVar *dlv = (dsc && dvn) ? scope_local(dsc, dvn) : NULL;
        if (dlv && dlv->type == TY_UNKNOWN) {
          dlv->type = TY_SYM_POLY_HASH;
          changed = 1;
        }
      }
      continue;
    }
    if (nm && (sp_streq(nm, "any?") || sp_streq(nm, "none?") ||
               sp_streq(nm, "all?") || sp_streq(nm, "one?") ||
               sp_streq(nm, "empty?"))) {
      /* blockless predicate on an un-narrowed empty-hash local: adopt the
         symbol-keyed poly variant so the hash fold arm serves it
         (`b = {}; b.none?`). Every write must be an empty {} literal, so an
         empty-ARRAY local can never be pulled into a hash type here. */
      int qa = nt_ref(nt, id, "arguments");
      int qac = 0;
      if (qa >= 0) nt_arr(nt, qa, "arguments", &qac);
      int qrc = nt_ref(nt, id, "receiver");
      if (qac == 0 && nt_ref(nt, id, "block") < 0 && qrc >= 0 &&
          nt_type(nt, qrc) && sp_streq(nt_type(nt, qrc), "LocalVariableReadNode") &&
          infer_type(c, qrc) == TY_UNKNOWN) {
        Scope *qsc = comp_scope_of(c, qrc);
        const char *qvn = nt_str(nt, qrc, "name");
        LocalVar *qlv = (qsc && qvn) ? scope_local(qsc, qvn) : NULL;
        if (qlv && qlv->type == TY_UNKNOWN && local_all_writes_empty_hash(c, qsc, qvn)) {
          qlv->type = TY_SYM_POLY_HASH;
          changed = 1;
        }
      }
      /* no rewrite of this node: fall through to the remaining arms */
    }
    if (nm && sp_streq(nm, "each_with_object")) {
      /* an empty-hash local passed as the memo becomes a general boxed
         key/value hash so any key type the block writes fits, matching the
         inline each_with_object({}) memo (#2969) */
      int ea = nt_ref(nt, id, "arguments");
      int eac = 0; const int *eav = ea >= 0 ? nt_arr(nt, ea, "arguments", &eac) : NULL;
      if (eac >= 1 && eav && nt_type(nt, eav[0]) &&
          sp_streq(nt_type(nt, eav[0]), "LocalVariableReadNode") &&
          infer_type(c, eav[0]) == TY_UNKNOWN) {
        Scope *esc = comp_scope_of(c, eav[0]);
        const char *evn = nt_str(nt, eav[0], "name");
        LocalVar *elv = (esc && evn) ? scope_local(esc, evn) : NULL;
        if (elv && elv->type == TY_UNKNOWN && local_all_writes_empty_hash(c, esc, evn)) {
          elv->type = TY_POLY_POLY_HASH;
          changed = 1;
        }
      }
      /* fall through */
    }
    if (nm && sp_streq(nm, "yield")) {
      /* Proc#yield is exactly #call */
      int yrc = nt_ref(nt, id, "receiver");
      if (yrc >= 0 && infer_type(c, yrc) == TY_PROC) {
        nt_node_set_str(nt, id, "name", "call");
        changed = 1;
      }
      continue;
    }
    if (nm && (sp_streq(nm, "grapheme_clusters") || sp_streq(nm, "each_grapheme_cluster"))) {
      /* grapheme clusters == characters over the supported text domain (no
         combining sequences): alias to the chars/each_char machinery */
      int grc = nt_ref(nt, id, "receiver");
      if (grc >= 0 && infer_type(c, grc) == TY_STRING) {
        nt_node_set_str(nt, id, "name",
                        sp_streq(nm, "grapheme_clusters") ? "chars" : "each_char");
        changed = 1;
        continue;
      }
    }
    /* Hash#reverse_each { |k, v| }: desugar to to_a.reverse.each so the
       existing pair-array destructure serves the two-param block (#2372). */
    if (nm && sp_streq(nm, "reverse_each") && nt_ref(nt, id, "block") >= 0) {
      int hrc = nt_ref(nt, id, "receiver");
      if (hrc >= 0 && ty_is_hash(infer_type(c, hrc))) {
        int toa = nt_new_node(nt, "CallNode");
        int rev = nt_new_node(nt, "CallNode");
        if (toa >= 0 && rev >= 0) {
          nt_node_set_str(nt, toa, "name", "to_a");
          nt_node_set_ref(nt, toa, "receiver", hrc);
          nt_node_set_str(nt, rev, "name", "reverse");
          nt_node_set_ref(nt, rev, "receiver", toa);
          nt_node_set_str(nt, id, "name", "each");
          nt_node_set_ref(nt, id, "receiver", rev);
          comp_grow_node_arrays(c);
          c->nscope[toa] = c->nscope[id];
          c->nscope[rev] = c->nscope[id];
          changed = 1;
        }
        continue;
      }
    }
    /* member? on a builtin container is Array/Hash/Range#include? (#2388);
       entries on an array is to_a (#2390). User classes keep their own. */
    if (nm && ((sp_streq(nm, "member?") || sp_streq(nm, "entries")) &&
               nt_ref(nt, id, "block") < 0)) {
      int mrc = nt_ref(nt, id, "receiver");
      int man = 0; { int _a = nt_ref(nt, id, "arguments");
                     if (_a >= 0) nt_arr(nt, _a, "arguments", &man); }
      TyKind mrt = mrc >= 0 ? infer_type(c, mrc) : TY_UNKNOWN;
      /* a not-yet-narrowed local holding an empty [] literal is still an
         array; restrict the UNKNOWN case to plain variable/literal receivers
         so a not-yet-typed method-call chain keeps its own entries path */
      int mrt_open = 0;
      if (mrt == TY_UNKNOWN && mrc >= 0 && nt_type(nt, mrc) &&
          sp_streq(nt_type(nt, mrc), "ArrayNode")) {
        int user_defines = 0;
        for (int uk = 0; uk < c->nclasses; uk++)
          if (comp_method_in_chain(c, uk, nm, NULL) >= 0) { user_defines = 1; break; }
        mrt_open = !user_defines;
      }
      if (sp_streq(nm, "member?") && man == 1 &&
          (ty_is_array(mrt) || ty_is_hash(mrt) || mrt == TY_RANGE || mrt_open)) {
        nt_node_set_str(nt, id, "name", "include?");
        changed = 1;
        continue;
      }
      if (sp_streq(nm, "entries") && man == 0 && (ty_is_array(mrt) || mrt_open)) {
        nt_node_set_str(nt, id, "name", "to_a");
        changed = 1;
        continue;
      }

    }
    /* `a.chain(b, c)` is desugared wholesale by desugar_enumerable_chain into
       `__enum_chain(a.to_a + b.to_a + c.to_a)` -- a real Enumerator::Chain that
       serves every terminal, not just a `.to_a` directly on the chain call. */
    /* find_all is a full alias of select on the builtin containers; rename
       so the select/with_index machinery serves both (#2389) */
    if (nm && sp_streq(nm, "find_all")) {
      int frc = nt_ref(nt, id, "receiver");
      TyKind frt = frc >= 0 ? infer_type(c, frc) : TY_UNKNOWN;
      int fa_user = 0;
      for (int uk = 0; uk < c->nclasses; uk++)
        if (comp_method_in_chain(c, uk, "find_all", NULL) >= 0) { fa_user = 1; break; }
      /* NOT hashes: Hash#find_all answers an array of pairs (the Enumerable
         contract), while Hash#select answers a hash -- the existing hash
         find_all machinery keeps that shape */
      int frt_open = frt == TY_UNKNOWN && !fa_user && frc >= 0 && nt_type(nt, frc) &&
                     sp_streq(nt_type(nt, frc), "ArrayNode");
      if (ty_is_array(frt) || frt == TY_RANGE || frt_open) {
        nt_node_set_str(nt, id, "name", "select");
        changed = 1;
        continue;
      }
    }
    /* String-endpoint ranges materialize to a StrArray, which has no
       begin/end of its own: alias them to first/last (#2411). */
    if (nm && (sp_streq(nm, "begin") || sp_streq(nm, "end")) &&
        nt_ref(nt, id, "block") < 0) {
      int brc = nt_ref(nt, id, "receiver");
      int ban = 0; { int _a = nt_ref(nt, id, "arguments");
                     if (_a >= 0) nt_arr(nt, _a, "arguments", &ban); }
      if (brc >= 0 && ban == 0 && infer_type(c, brc) == TY_STR_ARRAY) {
        nt_node_set_str(nt, id, "name", sp_streq(nm, "begin") ? "first" : "last");
        changed = 1;
        continue;
      }
    }
    if (nm && sp_streq(nm, "rfind")) {
      /* Array#rfind { block } == reverse.find { block }: interpose a reverse
         call so the existing find machinery serves it (#2320) */
      int rrc = nt_ref(nt, id, "receiver");
      /* an empty `[]` literal receiver infers TY_UNKNOWN but is an array all
         the same -- rfind on it must still desugar (to yield nil) (#2367) */
      int rrc_empty_lit = rrc >= 0 && nt_type(nt, rrc) &&
                          sp_streq(nt_type(nt, rrc), "ArrayNode") &&
                          ({ int _n = 0; nt_arr(nt, rrc, "elements", &_n); _n == 0; });
      if (rrc >= 0 && (ty_is_array(infer_type(c, rrc)) || rrc_empty_lit)) {
        int rev = nt_new_node(nt, "CallNode");
        if (rev >= 0) {
          nt_node_set_str(nt, rev, "name", "reverse");
          nt_node_set_ref(nt, rev, "receiver", rrc);
          nt_node_set_ref(nt, id, "receiver", rev);
          nt_node_set_str(nt, id, "name", "find");
          comp_grow_node_arrays(c);
          c->nscope[rev] = c->nscope[id];
          changed = 1;
          continue;
        }
      }
    }
    if (nm && sp_streq(nm, "step")) {
      /* Numeric#step keyword forms lower to the positional (limit, step):
         step(to: T, by: B) / step(by: B, to: T) / step(T, by: B). An
         endless step (by: only, no to:) stays a loud reject. */
      int sargs = nt_ref(nt, id, "arguments");
      int sac = 0;
      const int *sav = sargs >= 0 ? nt_arr(nt, sargs, "arguments", &sac) : NULL;
      if (sac >= 1 && sac <= 2 && nt_type(nt, sav[sac - 1]) &&
          sp_streq(nt_type(nt, sav[sac - 1]), "KeywordHashNode")) {
        int kwh = sav[sac - 1];
        int en = 0;
        const int *els = nt_arr(nt, kwh, "elements", &en);
        int to_v = -1, by_v = -1, other = 0;
        for (int e = 0; e < en; e++) {
          int kk = nt_ref(nt, els[e], "key");
          const char *kn = (kk >= 0 && nt_type(nt, kk) && sp_streq(nt_type(nt, kk), "SymbolNode"))
                           ? nt_str(nt, kk, "value") : NULL;
          if (kn && sp_streq(kn, "to")) to_v = nt_ref(nt, els[e], "value");
          else if (kn && sp_streq(kn, "by")) by_v = nt_ref(nt, els[e], "value");
          else other = 1;
        }
        if (sac == 2 && to_v < 0) to_v = sav[0];   /* step(limit, by: B) */
        if (!other && to_v >= 0 && by_v >= 0) {
          int na[2]; na[0] = to_v; na[1] = by_v;
          nt_node_set_arr(nt, sargs, "arguments", na, 2);
          changed = 1;
          continue;
        }
      }
    }
    if (nm && sp_streq(nm, "%")) {
      /* (range) % n is Range#step(n) (the arithmetic-sequence operator) */
      int prc = nt_ref(nt, id, "receiver");
      int pa = nt_ref(nt, id, "arguments");
      int pac = 0;
      if (pa >= 0) nt_arr(nt, pa, "arguments", &pac);
      if (prc >= 0 && pac == 1 && nt_ref(nt, id, "block") < 0 &&
          infer_type(c, prc) == TY_RANGE) {
        nt_node_set_str(nt, id, "name", "step");
        changed = 1;
      }
      continue;
    }
    if (!nm || !is_array_enum_method(nm)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    /* find_all == select once the receiver is ARRAY-shaped (a hash receiver
       reaches here as the redispatched pair array, so the result is the
       Enumerable pair list, not a hash) */
    if (sp_streq(nm, "find_all") && nt_ref(nt, id, "block") >= 0 &&
        (ty_is_array(comp_ntype(c, recv)) || ty_is_array(infer_type(c, recv)))) {
      nt_node_set_str(nt, id, "name", "select");
      changed = 1;
      continue;
    }
    /* flat_map whose block value is statically SCALAR is exactly map: CRuby
       appends a non-array value as-is, so with no arrays to splice the whole
       call is a map (an array/poly block value keeps the flat_map emitters) */
    if ((sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) &&
        nt_ref(nt, id, "block") >= 0 &&
        (ty_is_array(comp_ntype(c, recv)) || ty_is_array(infer_type(c, recv)))) {
      int fmblk = nt_ref(nt, id, "block");
      int fmbody = nt_ref(nt, fmblk, "body");
      int fmn = 0;
      const int *fmb = fmbody >= 0 ? nt_arr(nt, fmbody, "body", &fmn) : NULL;
      if (fmn >= 1) {
        TyKind fbt = infer_type(c, fmb[fmn - 1]);
        if (fbt != TY_UNKNOWN && fbt != TY_POLY && !ty_is_array(fbt) &&
            !ty_is_hash(fbt)) {
          nt_node_set_str(nt, id, "name", "map");
          changed = 1;
          continue;
        }
      }
    }
    /* already redirected through __enum_to_a -> leave it (idempotent) */
    if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "__enum_to_a")) continue;
    }
    TyKind rt = infer_type(c, recv);
    /* A materialized Enumerator delegates its block-driven Enumerable methods to
       its element array: `enum.map { }` -> `enum.to_a.map { }`. Only block forms
       need this; the blockless terminals (to_a, next, size, first) are emitted
       against the enumerator directly. to_a/entries ARE the materializer, so
       never wrap them (would recurse). An index-producing receiver
       (each_with_index / each_index / with_index) has its own dedicated chain
       codegen -- leave its shape intact rather than materializing early. The
       same holds for each_slice/each_cons: their .map/.collect chains are
       fold-unrolled with the receiver's concrete element type (a float slice's
       .sum came out 0 through the materialized poly detour). */
    int recv_is_index_enum = 0;
    if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      recv_is_index_enum = rn && (sp_streq(rn, "each_with_index") ||
                                  sp_streq(rn, "each_index") || sp_streq(rn, "with_index") ||
                                  sp_streq(rn, "each_slice") || sp_streq(rn, "each_cons"));
    }
    /* Blockless Enumerable terminals a materialized enumerator does not lower
       natively (unlike to_a/next/peek/size/first/take/with_index) also delegate
       through the element array: `enum.count` -> `enum.to_a.count`. */
    int enum_terminal = 0;
    if (nt_ref(nt, id, "block") < 0) {
      static const char *const term[] = {
        "count", "include?", "member?", "sum", "min", "max", "minmax",
        "sort", "reduce", "inject", "tally", "to_h", "uniq", "reverse",
        "find_index", "index", "join", "compact", "zip", NULL };
      for (int t = 0; term[t]; t++) if (sp_streq(nm, term[t])) { enum_terminal = 1; break; }
      /* bare `first` returns the single first element, which the materialized
         enumerator does not lower natively (only the counted first(n) does),
         so it delegates through the element array too (#2994) */
      if (sp_streq(nm, "first")) {
        int fa = nt_ref(nt, id, "arguments"); int fac = 0;
        if (fa >= 0) nt_arr(nt, fa, "arguments", &fac);
        if (fac == 0) enum_terminal = 1;
      }
    }
    /* A blockless TERMINAL on an index/slice enumerator delegates through
       to_a too (each_slice(2).count / .to_h): only the block-driving chains
       need the raw shape for their fold unrolls. */
    /* A BLOCK-form Enumerable on a SLICE/CONS enumerator delegates through
       to_a -- except map/collect, whose dedicated fold arms need the raw
       shape. The index enums (each_with_index / with_index) keep ALL their
       block chains on the dedicated two-param codegen. */
    int recv_is_slice_enum = 0;
    if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rn2 = nt_str(nt, recv, "name");
      recv_is_slice_enum = rn2 && (sp_streq(rn2, "each_slice") || sp_streq(rn2, "each_cons"));
    }
    if (rt == TY_ENUMERATOR && recv_is_slice_enum && nt_ref(nt, id, "block") >= 0 &&
        !sp_streq(nm, "map") && !sp_streq(nm, "collect") &&
        !sp_streq(nm, "with_index") && !sp_streq(nm, "each_with_index") &&
        !sp_streq(nm, "to_a") && !sp_streq(nm, "entries")) {
      int wrap3 = nt_new_node(nt, "CallNode");
      if (wrap3 >= 0) {
        nt_node_set_str(nt, wrap3, "name", "to_a");
        nt_node_set_ref(nt, wrap3, "receiver", recv);
        nt_node_set_ref(nt, id, "receiver", wrap3);
        comp_grow_node_arrays(c);
        c->nscope[wrap3] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    if (rt == TY_ENUMERATOR && recv_is_index_enum && enum_terminal &&
        !sp_streq(nm, "size")) {
      int wrap2 = nt_new_node(nt, "CallNode");
      if (wrap2 >= 0) {
        nt_node_set_str(nt, wrap2, "name", "to_a");
        nt_node_set_ref(nt, wrap2, "receiver", recv);
        nt_node_set_ref(nt, id, "receiver", wrap2);
        comp_grow_node_arrays(c);
        c->nscope[wrap2] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    if (rt == TY_ENUMERATOR && !recv_is_index_enum &&
        !sp_streq(nm, "to_a") && !sp_streq(nm, "entries") &&
        (nt_ref(nt, id, "block") >= 0 || enum_terminal)) {
      int wrap = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, wrap, "name", "to_a");
      nt_node_set_ref(nt, wrap, "receiver", recv);
      nt_node_set_ref(nt, id, "receiver", wrap);
      comp_grow_node_arrays(c);
      c->nscope[wrap] = c->nscope[id];
      changed = 1;
      continue;
    }
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (comp_method_in_chain(c, cid, "__enum_to_a", NULL) < 0) continue;  /* not an #each class */
    /* A Struct's blockless #each returns an Enumerator over its members (CRuby),
       not the LocalJumpError the synthesized yielding #each would raise. Route
       it through the member array's blockless #each, which materializes the
       enumerator. (each_entry was already renamed to each upstream.) */
    if (c->classes[cid].is_struct && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(nm, "each") || sp_streq(nm, "each_entry"))) {
      int wrap = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, wrap, "name", "__enum_to_a");
      nt_node_set_ref(nt, wrap, "receiver", recv);
      nt_node_set_ref(nt, id, "receiver", wrap);
      /* the array's blockless #each yields the Enumerator; each_entry keeps its
         own name (array #each_entry with no block also materializes one) */
      if (sp_streq(nm, "each_entry")) nt_node_set_str(nt, id, "name", "each");
      comp_grow_node_arrays(c);
      c->nscope[wrap] = c->nscope[id];
      changed = 1;
      continue;
    }
    if (comp_method_in_chain(c, cid, nm, NULL) >= 0) continue;            /* class defines it */
    /* a Struct/Data class serves these natively in the struct emit section
       (member-pair to_h, ordered to_a/values, size, dig, ...); the flat
       element array would change their semantics */
    if (c->classes[cid].is_struct) {
      /* A member accessor always beats the inherited Enumerable surface:
         CRuby defines accessors directly on the Struct/Data subclass, so
         Data.define(:count).new(count: 7).count reads the member (7), never
         Enumerable#count (the member total). Members are not methods in the
         chain (codegen serves them natively), so check them explicitly. */
      char mn[272]; snprintf(mn, sizeof mn, "@%s", nm);
      if (comp_ivar_index(&c->classes[cid], mn) >= 0) continue;
      static const char *const snative[] = {
        "to_a", "values", "to_h", "members", "size", "length",
        "dig", "deconstruct", "deconstruct_keys", "with", "inspect", "to_s",
        NULL };
      int nat = 0;
      for (int t = 0; snative[t]; t++) if (sp_streq(nm, snative[t])) { nat = 1; break; }
      if (nat) continue;
    }
    /* The block forms of each_slice/each_cons return the RECEIVER in Ruby,
       but the redirect makes the call read the flat element array, whose
       each_slice returns that array. Record the original receiver so the
       emit yields it instead (#2981). */
    if (nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(nm, "each_slice") || sp_streq(nm, "each_cons")))
      nt_node_set_int(nt, id, "enum_self_result", recv);
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

/* `for x in obj` over a user Enumerable (a class with an #each, hence a
   synthesized __enum_to_a): materialize the receiver to a flat element array so
   the for-loop iterates it via the existing array path. Without this the loop
   collection is unrecognized and silently iterates nothing. Idempotent; struct
   classes serve to_a natively and are left alone. */
static int desugar_for_enumerable(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "ForNode")) continue;
    int coll = nt_ref(nt, id, "collection");
    if (coll < 0) continue;
    const char *cty = nt_type(nt, coll);
    if (cty && sp_streq(cty, "CallNode") && nt_str(nt, coll, "name") &&
        sp_streq(nt_str(nt, coll, "name"), "__enum_to_a")) continue;  /* idempotent */
    TyKind rt = infer_type(c, coll);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (cid < 0 || comp_method_in_chain(c, cid, "__enum_to_a", NULL) < 0) continue;
    if (c->classes[cid].is_struct) continue;  /* struct to_a is native */
    int wrap = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, wrap, "name", "__enum_to_a");
    nt_node_set_ref(nt, wrap, "receiver", coll);
    nt_node_set_ref(nt, id, "collection", wrap);
    comp_grow_node_arrays(c);
    c->nscope[wrap] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* Rewrite `recv.to_enum(:m, *a)` / `enum_for(:m, *a)` into a real Enumerator,
   dispatched on the receiver kind (needs the receiver type, so it runs in the
   fixpoint). A user-class receiver whose class defines a yielding `m` becomes
   `recv.__to_enum_m` (the synthesized generator helper); a builtin receiver
   becomes the plain blockless `recv.m(*a)`, which already lowers to a
   materialized Enumerator. Un-resolved or unsupported receivers are left
   untouched -- a leftover to_enum/enum_for is loudly rejected downstream (no
   silent-wrong). Returns 1 if anything changed. */
static int desugar_to_enum(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    char m[128]; int extra = 0, has_block = 0;
    if (!to_enum_target(c, id, m, sizeof m, &extra, &has_block)) continue;
    if (has_block) continue;                 /* size-callable block: PR follow-up */
    int recv = nt_ref(nt, id, "receiver");
    /* receiver type: an explicit receiver's inferred type, else the enclosing
       self (implicit-self `enum_for(:m)` inside an instance method). */
    TyKind rt;
    if (recv >= 0) rt = infer_type(c, recv);
    else {
      Scope *es = comp_scope_of(c, id);
      rt = (es && es->class_id >= 0 && !es->is_cmethod) ? ty_object(es->class_id) : TY_UNKNOWN;
    }

    if (ty_is_object(rt)) {
      int cid = ty_object_class(rt);
      char hname[160]; snprintf(hname, sizeof hname, "__to_enum_%s", m);
      if (comp_method_in_chain(c, cid, hname, NULL) < 0) continue;  /* no yielding m: leave */
      if (extra > 0) continue;   /* user-class to_enum with args: PR follow-up (loud downstream) */
      /* `return enum_for(:m) unless block_given?` inside method m: a blockless
         call to m returns the Enumerator (with a block m runs the body and its
         return is ignored). Pin m's return type so the blockless call site does
         not inherit the polluted union of the enumerator and the yield path. */
      Scope *es = comp_scope_of(c, id);
      int self_recv = recv < 0 ||
                      (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode"));
      if (self_recv && es && es->name && sp_streq(es->name, m) &&
          es->ret != TY_ENUMERATOR) {
        es->ret = TY_ENUMERATOR;
        es->ret_specialized = 1;
        changed = 1;
      }
      nt_node_set_str(nt, id, "name", hname);
      int empty = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, empty, "arguments", NULL, 0);
      nt_node_set_ref(nt, id, "arguments", empty);
      comp_grow_node_arrays(c);
      c->nscope[empty] = c->nscope[id];
      changed = 1;
      continue;
    }

    /* Builtin receiver: retarget to the blockless iterator (drops the leading
       method symbol, keeps any trailing args). */
    int is_builtin = ty_is_array(rt) || ty_is_hash(rt) || rt == TY_STRING ||
                     rt == TY_RANGE || rt == TY_INT;
    if (is_builtin && recv >= 0 && to_enum_builtin_method(m)) {
      int args = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      int *rest = NULL;
      if (ac > 1) {
        rest = malloc(sizeof(int) * (size_t)(ac - 1));
        if (!rest) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        for (int j = 1; j < ac; j++) rest[j - 1] = av[j];   /* snapshot before realloc */
      }
      nt_node_set_str(nt, id, "name", m);
      int newargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, newargs, "arguments", rest, ac > 1 ? ac - 1 : 0);
      nt_node_set_ref(nt, id, "arguments", newargs);
      comp_grow_node_arrays(c);
      c->nscope[newargs] = c->nscope[id];
      free(rest);
      changed = 1;
      continue;
    }
    /* else: unresolved/unsupported receiver -- leave for a later iteration or a
       downstream loud reject. */
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
/* Element-class sentinel: the array's elements are themselves int-arrays
   (ExtField-style int[4]), narrowing the container to TY_INT_ARRAY_ARRAY
   rather than an object-pointer array. Distinct from -1 (none) / -2 (conflict)
   and from any user class id (>= 0), so oa_cls_join composes it unchanged. */
#define OA_CLS_IA (-1000)
/* the single user-object class of a node's value, an int-array sentinel, or -1
   if neither a lone object nor a lone int-array. */
static int oa_obj_class_of(Compiler *c, int node) {
  TyKind t = infer_type(c, node);
  if (ty_is_object(t)) return ty_object_class(t);
  if (t == TY_INT_ARRAY) return OA_CLS_IA;
  return -1;
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

/* An @ivar (or a bare attr_reader that returns one) passed to a helper whose
   parameter was push-widened (a `<<` inside pushed an element the bound type
   could not hold, e.g. an object into an empty-`[]` int array) shares its
   backing storage with that parameter, so the ivar must widen to the poly
   array too -- otherwise the call copy-converts the narrow int array and the
   helper's push lands on the throwaway copy. Runs post-fixpoint, when
   push_widened is final (during the fixpoint it is set only after
   bind_call_params has already observed the param). Monotone: typed array ->
   poly array only. (#3154) */
static void widen_ivars_from_pushed_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int ca = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
    if (!av || an == 0) continue;
    /* resolve the callee scope */
    int recv = nt_ref(nt, id, "receiver");
    int mi = -1;
    if (recv < 0) {
      mi = comp_method_index(c, name);
      if (mi < 0) {
        Scope *self = comp_scope_of(c, id);
        if (self && self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL);
      }
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
    }
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    Scope *asc = comp_scope_of(c, id);
    if (!asc || asc->class_id < 0) continue;
    ClassInfo *acls = &c->classes[asc->class_id];
    for (int k = 0; k < an && k < m->nparams; k++) {
      LocalVar *p = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
      if (!p || !p->push_widened) continue;
      const char *aty = nt_type(nt, av[k]);
      if (!aty) continue;
      char ivbuf[128];
      const char *ivn = NULL;
      if (sp_streq(aty, "InstanceVariableReadNode")) {
        ivn = nt_str(nt, av[k], "name");             /* already "@name" */
      }
      else if (sp_streq(aty, "CallNode") && nt_ref(nt, av[k], "receiver") < 0) {
        const char *cn = nt_str(nt, av[k], "name");  /* a bare attr_reader call */
        if (cn && cn[0] != '@' && strlen(cn) < sizeof ivbuf - 1) {
          ivbuf[0] = '@'; strcpy(ivbuf + 1, cn); ivn = ivbuf;
        }
      }
      if (!ivn) continue;
      int ivi = comp_ivar_index(acls, ivn);
      if (ivi < 0) continue;
      if (ty_is_array(acls->ivar_types[ivi]) && acls->ivar_types[ivi] != TY_POLY_ARRAY)
        acls->ivar_types[ivi] = TY_POLY_ARRAY;
    }
  }
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
    /* Resolve the call target the way emission will: a free function first,
       then an implicit-self instance method of the enclosing class, then an
       explicit receiver's class chain. -1 = unattributable here. */
    int oa_tmi = -1;
    if (name) {
      if (recv < 0) {
        oa_tmi = comp_method_index(c, name);
        if (oa_tmi < 0) {
          Scope *csc = comp_scope_of(c, id);
          if (csc && csc->class_id >= 0)
            oa_tmi = comp_method_in_chain(c, csc->class_id, name, NULL);
        }
      }
      else {
        TyKind ort = infer_type(c, recv);
        if (ty_is_object(ort))
          oa_tmi = comp_method_in_chain(c, ty_object_class(ort), name, NULL);
        else if (ort == TY_POLY || ort == TY_UNKNOWN || ort == TY_NIL) {
          /* A dynamic receiver may dispatch to ANY same-named instance
             method at runtime (the poly dispatch arms), passing boxed
             values; a param slot reachable that way must not narrow. */
          for (int pi = 0; pi < n; pi++) {
            if (!sl[pi].alive) continue;
            Scope *PS = &c->scopes[sl[pi].sidx];
            for (int pk = 0; pk < PS->nparams; pk++)
              if (PS->pnames[pk] && PS->name && sp_streq(PS->name, name) &&
                  scope_local(PS, PS->pnames[pk]) == sl[pi].lv)
                sl[pi].alive = 0;
          }
        }
      }
    }
    /* Interprocedural edges. A PARAM slot may only narrow when EVERY
       argument reaching it is a same-component candidate slot: the callee's
       C signature and the caller's C locals must agree on one container
       type. A call that resolves to the method but passes a non-slot value
       for that position, or a call to the method we cannot attribute at
       all, kills the param slot (the caller would keep passing a boxed
       poly array into an unboxed sp_PtrArray* -- the #1867 mis-box).
       Instance methods resolve through the class chain now; previously only
       free functions made edges, so an instance method's param narrowed
       with its callers' locals left poly. */
    if (oa_tmi >= 0) {
      Scope *M = &c->scopes[oa_tmi];
      for (int k = 0; k < M->nparams; k++) {
        LocalVar *plv = M->pnames[k] ? scope_local(M, M->pnames[k]) : NULL;
        int T = plv ? oa_find(sl, n, oa_tmi, plv) : -1;
        if (T < 0) continue;
        int a = (argv && k < argc) ? argv[k] : -1;
        if (a >= 0 && read_slot[a] >= 0) {
          claimed[a] = 1;
          oa_uf_union(sl, read_slot[a], T);
        }
        else {
          sl[T].alive = 0;   /* unmodeled incoming value for this position */
        }
      }
    }
    for (int k = 0; argv && k < argc; k++) {
      int a = argv[k];
      if (read_slot[a] < 0) continue;
      int S = read_slot[a];
      if (oa_tmi < 0) { sl[S].alive = 0; continue; }
      Scope *M = &c->scopes[oa_tmi];
      if (k >= M->nparams || (M->rest_idx >= 0 && k >= M->rest_idx)) { sl[S].alive = 0; continue; }
      LocalVar *plv = M->pnames[k] ? scope_local(M, M->pnames[k]) : NULL;
      int T = plv ? oa_find(sl, n, oa_tmi, plv) : -1;
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
        /* OA_CLS_IA (int-array element) is a negative SENTINEL, not an
           "invalid" -1/-2: it is real evidence, so it must not kill the slot
           (an `[[a,b],[c,d]]` array-of-int-array literal narrows like a pushed
           one already does -- the push arm never had this < 0 guard). */
        if (ec < 0 && ec != OA_CLS_IA) { sl[S].alive = 0; break; }
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
    if (!sl[r].alive) continue;
    if (sl[r].cls == -1 || sl[r].cls == -2) continue;  /* no evidence / conflict */
    if (sl[r].cls == OA_CLS_IA) {
      /* array-of-int-array: the codegen supports index/push/[]=/length/first/
         last but not the boxed sort/min/max comparators yet, so a component
         that used those (needs_cmp) stays on the poly path for now. */
      if (sl[r].needs_cmp) continue;
      sl[i].lv->type = TY_INT_ARRAY_ARRAY;
      continue;
    }
    /* a component using no-block sort/min/max narrows only when the element
       class can actually compare (has `<=>` in its chain); otherwise it stays
       poly, where the boxed comparator raises the CRuby ArgumentError. */
    if (sl[r].needs_cmp && comp_method_in_chain(c, sl[r].cls, "<=>", NULL) < 0) continue;
    sl[i].lv->type = ty_obj_array(sl[r].cls);
  }

  /* 8. dependent locals: `b = arr[i]` (arr now a narrowed obj array) makes `b`
        the element object type, so its field accesses unbox. Every write of the
        local must be such an index read of one class (nil writes excepted).
        Iterated, because the narrowing chains: `a = rows[0]` narrows a to an
        int array only in this step, and only then can `m = a[1]` see an int
        (#3354). */
  for (int prop = 0; prop < 8; prop++) {
  int prop_ch = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
      TyKind elem = TY_UNKNOWN; int ok = 1, saw = 0;
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
        /* element type of a narrowed obj-array OR the new int-array-array */
        /* a scalar-element array yields its element type; an out-of-range
           read is that type's nil (SP_INT_NIL / NULL), which it models */
        TyKind ec = ty_is_obj_array(rt) ? ty_object(ty_obj_array_class(rt))
                  : (rt == TY_INT_ARRAY_ARRAY) ? TY_INT_ARRAY
                  : (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)
                    ? ty_array_elem(rt) : TY_UNKNOWN;
        if (ec == TY_UNKNOWN) { ok = 0; break; }
        if (elem == TY_UNKNOWN) elem = ec; else if (elem != ec) { ok = 0; break; }
        saw = 1;
      }
      if (ok && saw && elem != TY_UNKNOWN) { lv->type = elem; prop_ch = 1; }
    }
  }
  if (!prop_ch) break;
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
  /* Every check below only cares about nodes in the candidate local's own
     scope (a read's consumer in receiver/argument position shares the read's
     scope -- only a block opens a new one, and blocks are separate refs).
     Bucket node ids by scope once, so each candidate walks its scope's nodes
     instead of the whole table: the old shape was O(poly-locals * nodes) and
     a dominant post-fixpoint cost on large trees. */
  int *sc_cnt = (int *)calloc((size_t)(c->nscopes + 1), sizeof(int));
  int *sc_off = (int *)malloc(sizeof(int) * (size_t)(c->nscopes + 1));
  int *sc_nodes = (int *)malloc(sizeof(int) * (size_t)nc);
  if (!is_read || !claimed || !sc_cnt || !sc_off || !sc_nodes) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(is_read, 0, (size_t)nc);
  memset(claimed, 0, (size_t)nc);
  for (int id = 0; id < nt->count; id++) {
    int s2 = c->nscope[id];
    if (s2 >= 0 && s2 < c->nscopes) sc_cnt[s2]++;
  }
  sc_off[0] = 0;
  for (int s = 0; s < c->nscopes; s++) sc_off[s + 1] = sc_off[s] + sc_cnt[s];
  for (int s = 0; s <= c->nscopes; s++) sc_cnt[s] = 0;
  for (int id = 0; id < nt->count; id++) {
    int s2 = c->nscope[id];
    if (s2 >= 0 && s2 < c->nscopes) sc_nodes[sc_off[s2] + sc_cnt[s2]++] = id;
  }
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
      for (int k = sc_off[s]; k < sc_off[s + 1] && ok; k++) {
        int id = sc_nodes[k];
        const char *ty = nt_type(nt, id);
        if (!ty) continue;
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

      /* 2. map this local's reads, then claim those in an int context.
         (is_read/claimed are re-cleared per candidate over this scope's nodes
         only -- nothing outside the scope is ever set.) */
      for (int k = sc_off[s]; k < sc_off[s + 1]; k++) {
        int id = sc_nodes[k];
        is_read[id] = 0; claimed[id] = 0;
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
        const char *rn = nt_str(nt, id, "name");
        if (rn && sp_streq(rn, nm)) is_read[id] = 1;
      }
      int has_index = 0;
      for (int k = sc_off[s]; k < sc_off[s + 1]; k++) {
        int id = sc_nodes[k];
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
            for (int k2 = 0; k2 < an; k2++) if (is_read[av[k2]]) claimed[av[k2]] = 1;
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
            for (int k2 = 0; k2 < an; k2++) if (is_read[av[k2]]) { claimed[av[k2]] = 1; has_index = 1; }
          }
        }
      }
      /* every read must be claimed, and at least one must be an index (which
         proves the value is an integer, making a poly-source coercion sound). */
      for (int k = sc_off[s]; k < sc_off[s + 1] && ok; k++) {
        int id = sc_nodes[k];
        if (is_read[id] && !claimed[id]) ok = 0;
      }
      if (ok && has_index) lv->type = TY_INT;
    }
  }
  free(sc_cnt); free(sc_off); free(sc_nodes);
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

/* ---- `if v.is_a?(K)` occurrence typing ------------------------------------
   A poly local/param unioned across several concrete types cannot dispatch a
   method that only SOME members define -- `v.to_a` where v is Array | String
   aborts at runtime "undefined method 'to_a' for poly". An `is_a?(K)` guard
   proves the runtime type inside its then-branch, so reads of v there can be
   narrowed to K's concrete type (unboxed at the read site, the same machinery
   `return .. if p.nil?` uses). The runtime is_a? check makes the unbox sound. */

/* Map a guard class name to the concrete narrowed type, or TY_UNKNOWN. */
static TyKind isa_narrow_type(const char *cn) {
  if (!cn) return TY_UNKNOWN;
  if (sp_streq(cn, "Array")) return TY_POLY_ARRAY;
  if (sp_streq(cn, "String")) return TY_STRING;
  if (sp_streq(cn, "Integer") || sp_streq(cn, "Fixnum")) return TY_INT;
  if (sp_streq(cn, "Float")) return TY_FLOAT;
  if (sp_streq(cn, "Symbol")) return TY_SYMBOL;
  return TY_UNKNOWN;
}

/* predicate `v.is_a?(K)` / `v.kind_of?(K)` on a poly local v of scope s:
   returns v's name and sets *out_t to the narrowed type, else NULL. */
static const char *isa_guard_local(Compiler *c, int pred, Scope *s, TyKind *out_t) {
  const NodeTable *nt = c->nt;
  if (pred < 0 || !nt_type(nt, pred) || !sp_streq(nt_type(nt, pred), "CallNode")) return NULL;
  const char *cn = nt_str(nt, pred, "name");
  if (!cn || (!sp_streq(cn, "is_a?") && !sp_streq(cn, "kind_of?"))) return NULL;
  int recv = nt_ref(nt, pred, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return NULL;
  const char *pn = nt_str(nt, recv, "name");
  if (!pn || comp_scope_of(c, recv) != s) return NULL;
  LocalVar *lv = scope_local(s, pn);
  if (!lv || lv->type != TY_POLY) return NULL;   /* only a poly local narrows */
  int args = nt_ref(nt, pred, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "ConstantReadNode")) return NULL;
  TyKind t = isa_narrow_type(nt_str(nt, av[0], "name"));
  if (t == TY_UNKNOWN) return NULL;
  *out_t = t;
  return pn;
}

/* Like nng_mark_reads, but a bare read that is a direct ELEMENT of an array
   or hash literal stays unnarrowed: narrowing it retypes the container literal
   (`[v]` becomes a typed array), which cascades into the container's consumers
   and is layout-hostile on hot programs (optcarrot's add_mappings ternary cost
   ~2-3%% fps) -- the array-typing lesson. Dispatch on v itself still narrows. */
static void isa_mark_reads(Compiler *c, int root, Scope *s, const char *pn, TyKind t) {
  if (root < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, root, "name");
    if (nm && sp_streq(nm, pn) && comp_scope_of(c, root) == s)
      c->nilnarrow[root] = t;
    return;
  }
  int is_container = ty && (sp_streq(ty, "ArrayNode") || sp_streq(ty, "HashNode"));
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) isa_mark_reads(c, nt_ref_at(nt, root, i), s, pn, t);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *a = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) {
      if (is_container && a[j] >= 0 && nt_type(nt, a[j]) &&
          sp_streq(nt_type(nt, a[j]), "LocalVariableReadNode")) {
        const char *enm = nt_str(nt, a[j], "name");
        if (enm && sp_streq(enm, pn)) continue;  /* bare element read: keep poly */
      }
      isa_mark_reads(c, a[j], s, pn, t);
    }
  }
}

/* A node that always exits the current control flow (block iteration / method):
   reaching a statement AFTER `<exit> unless cond` proves cond held. */
static int isa_node_diverges(Compiler *c, int node) {
  if (node < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "NextNode") || sp_streq(ty, "BreakNode") ||
      sp_streq(ty, "ReturnNode") || sp_streq(ty, "RedoNode")) return 1;
  /* a bare `raise` / `raise E` (or exit/abort/throw) also diverges, so a
     `raise unless v.is_a?(K)` guard proves K for the fall-through (#3150). */
  if (sp_streq(ty, "CallNode")) {
    const char *cn = nt_str(nt, node, "name");
    if (cn && nt_ref(nt, node, "receiver") < 0 &&
        (sp_streq(cn, "raise") || sp_streq(cn, "fail") || sp_streq(cn, "throw") ||
         sp_streq(cn, "exit") || sp_streq(cn, "exit!") || sp_streq(cn, "abort")))
      return 1;
  }
  if (sp_streq(ty, "StatementsNode")) {
    int n = 0; const int *bb = nt_arr(nt, node, "body", &n);
    return n > 0 && isa_node_diverges(c, bb[n - 1]);
  }
  return 0;
}

/* Walk statements; for each `if v.is_a?(K)` narrow reads of v in the then-arm
   (v provably IS K there) unless the arm rewrites v. Recurses so nested and
   `elsif v.is_a?(K2)` chains each narrow their own arm. */
static void isa_mark_guards(Compiler *c, int root, Scope *s) {
  if (root < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "IfNode")) {
    TyKind t = TY_UNKNOWN;
    const char *pn = isa_guard_local(c, nt_ref(nt, root, "predicate"), s, &t);
    if (pn) {
      int branch = nt_ref(nt, root, "statements");  /* the then-arm: v IS K */
      if (branch >= 0 && !nng_has_write(c, branch, pn))
        isa_mark_reads(c, branch, s, pn, t);
    }
  }
  /* guard-continue: `<exit> unless v.is_a?(K)` gates the fall-through, so every
     SUBSEQUENT sibling statement provably sees v as K (#3142). Scan a
     StatementsNode for such a guard and narrow the reads that follow it, up to
     a write of v. */
  if (ty && sp_streq(ty, "StatementsNode")) {
    int n = 0; const int *bb = nt_arr(nt, root, "body", &n);
    for (int i = 0; i < n; i++) {
      const char *cty = nt_type(nt, bb[i]);
      if (!cty || !sp_streq(cty, "UnlessNode")) continue;
      if (nt_ref(nt, bb[i], "else_clause") >= 0) continue;   /* a plain guard only */
      TyKind t = TY_UNKNOWN;
      const char *pn = isa_guard_local(c, nt_ref(nt, bb[i], "predicate"), s, &t);
      if (!pn || !isa_node_diverges(c, nt_ref(nt, bb[i], "statements"))) continue;
      for (int j = i + 1; j < n; j++) {
        if (nng_has_write(c, bb[j], pn)) break;
        isa_mark_reads(c, bb[j], s, pn, t);
      }
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) isa_mark_guards(c, nt_ref_at(nt, root, i), s);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0;
    const int *a = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++) isa_mark_guards(c, a[j], s);
  }
}

static void narrow_isa_guards(Compiler *c) {
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *s = &c->scopes[mi];
    if (s->body < 0 || s->cs_synth) continue;
    isa_mark_guards(c, s->body, s);
  }
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

/* Mark every empty `[]` literal used as the receiver of a whitelisted iterator
   (`[].each { }`, `[].map { }`, ...), so infer_uncached types it TY_POLY_ARRAY
   and the iterator dispatches over an empty poly array instead of aborting on an
   unresolved receiver. Restricted to iterators whose empty-poly-array codegen is
   known-good; the non-block empty-literal folds (`[].sum(n)`, `[].reduce(i,:s)`)
   and the `x = []` element back-fill are on other nodes, so both are untouched. */
static int empty_arr_iter_ok(const char *nm) {
  static const char *const ok[] = {
    "each", "map", "collect", "select", "filter", "reject", "filter_map",
    "find", "detect", "each_with_object",
    "group_by", "sort_by", "min_by", "max_by", "flat_map",
    "reduce", "inject", NULL };
  for (int i = 0; ok[i]; i++) if (sp_streq(nm, ok[i])) return 1;
  return 0;
}
static void mark_empty_array_receivers(Compiler *c) {
  if (!c->empty_arr_recv) return;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    /* product takes no block; max/min/sample WITH a count arg take no block
       but return an array (via the PolyArray sort/shuffle path) -- an empty
       int-array literal there would be passed to sp_PolyArray_* as an
       sp_IntArray* (#2864). Everything else on the list is block-gated so the
       non-block empty-literal folds (sum, inject(:sym), first) keep their arms. */
    int has_block = nt_ref(nt, id, "block") >= 0;
    int count_form = 0;
    if (nm && (sp_streq(nm, "max") || sp_streq(nm, "min") || sp_streq(nm, "sample"))) {
      int an = nt_ref(nt, id, "arguments"); int ac = 0;
      if (an >= 0) nt_arr(nt, an, "arguments", &ac);
      count_form = (ac == 1);
    }
    /* blockless zip pairs elementwise into a PolyArray of PolyArrays: an
       untyped empty literal receiver had no array kind, so the call fell
       through to the set-op arm and emitted sp_<T>Array_difference (#3332) */
    int zip_form = nm && sp_streq(nm, "zip") && !has_block;
    if (!has_block && !(nm && sp_streq(nm, "product")) && !count_form && !zip_form) continue;
    if (!nm || (!empty_arr_iter_ok(nm) && !sp_streq(nm, "product") && !count_form && !zip_form)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || recv >= c->node_cap) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !sp_streq(rty, "ArrayNode")) continue;
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) c->empty_arr_recv[recv] = 1;
  }
}
/* A direct empty `{}` literal receiver of a block method (`{}.select { }`)
   infers TY_UNKNOWN and can't dispatch; mark it so infer types it as the
   bare-{} hash (STR_POLY), the same fallback a never-narrowed `{}` local uses. */
/* A method whose body TAIL is a bare empty `[]` / `{}` literal returns that
   container, but an unmarked empty literal infers TY_UNKNOWN (its element
   type normally back-fills from usage), so the method collapsed to a void C
   function that silently discarded the value. Mark the tail literal so it
   types as the bare-container default (poly array / STR_POLY hash), the same
   thing `x = []; x` returns. Skipped when the method has explicit returns:
   their unification (`return [1]` elsewhere) should keep narrowing. */
static void mark_empty_literal_tails(Compiler *c) {
  if (!c->empty_arr_recv || !c->empty_hash_recv) return;
  const NodeTable *nt = c->nt;
  char *has_ret = (char *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
  if (has_ret) {
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_ReturnNode) continue;
      Scope *rs = comp_scope_of(c, id);
      if (rs) has_ret[(int)(rs - c->scopes)] = 1;
    }
  }
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->body < 0 || (has_ret && has_ret[s])) continue;
    if (nt_kind(nt, sc->body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn);
    if (bn <= 0) continue;
    int last = bb[bn - 1];
    if (last < 0 || last >= c->node_cap) continue;
    const char *lty = nt_type(nt, last);
    if (!lty) continue;
    int en = 0;
    if (sp_streq(lty, "ArrayNode")) {
      nt_arr(nt, last, "elements", &en);
      if (en == 0) c->empty_arr_recv[last] = 1;
    }
    else if (sp_streq(lty, "HashNode") || sp_streq(lty, "KeywordHashNode")) {
      nt_arr(nt, last, "elements", &en);
      if (en == 0) c->empty_hash_recv[last] = 1;
    }
  }
  free(has_ret);
}
/* An empty `{}` passed as an argument to a USER method (`m({})`) is an
   accumulator seed: the callee builds it up (a yield-wrapper's inject /
   each_with_object), so the param and the method's return settle from it. An
   unmarked empty {} stays UNKNOWN, and if the callee returns it (`acc`) the
   whole call is UNKNOWN -> a void method (#2860). Type it as the widest hash
   (POLY_POLY, so any key/value the callee writes is sound). Only user methods
   (a builtin has its own arg handling). Empty `[]` args are deliberately NOT
   marked: POLY_ARRAY would pessimize the common typed-array-seed case, and the
   observed void-return bug is hash-specific. */
/* True if method scope `mi`'s param `p` is index-assigned (`pname[k] = v`) with
   a non-string key -- an int or symbol key. The precise body-write widening
   (#397) only reads string/symbol keys and skips int keys (array-ambiguous), so
   an int-keyed mutation leaves the param poly, and mutating a poly hash through
   `[]=` widens it -- dropping the change on the caller's empty `{}` (#2871). */
/* Collected once per mark_empty_literal_args run: every `local[k] = v` write
   whose key type cannot be assumed String (#2894), as (scope, local-name)
   pairs. The old form rescanned the whole node table per (call, param) --
   O(calls * nodes) once call names resolve. */
typedef struct { const Scope *sc; const char *nm; } NonstrIdxWrite;
static NonstrIdxWrite *nonstr_idx_writes(Compiler *c, int *out_n) {
  const NodeTable *nt = c->nt;
  NonstrIdxWrite *v = NULL; int n = 0, cap = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "[]=")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn) continue;
    int args = nt_ref(nt, id, "arguments"); int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 2) continue;
    TyKind kt = infer_type(c, av[0]);
    /* An unresolved key (typically a parameter whose type arrives from the call
       sites) cannot be assumed String: defaulting the caller's `{}` to the
       string-keyed variant would drop the callee's write (#2894). */
    if (kt != TY_INT && kt != TY_SYMBOL && kt != TY_UNKNOWN && kt != TY_POLY) continue;
    if (n >= cap) {
      cap = cap ? cap * 2 : 16;
      v = realloc(v, sizeof(*v) * (size_t)cap);
      if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    v[n].sc = comp_scope_of(c, recv);
    v[n].nm = rn;
    n++;
  }
  *out_n = n;
  return v;
}
static int param_nonstr_index_written(Compiler *c, int mi, int p,
                                      const NonstrIdxWrite *ix, int ixn) {
  if (mi < 0 || p < 0 || p >= c->scopes[mi].nparams) return 0;
  const char *pn = c->scopes[mi].pnames[p];
  if (!pn) return 0;
  for (int i = 0; i < ixn; i++)
    if (ix[i].sc == &c->scopes[mi] && sp_streq(ix[i].nm, pn)) return 1;
  return 0;
}
static void mark_empty_literal_args(Compiler *c) {
  if (!c->empty_hash_arg) return;
  const NodeTable *nt = c->nt;
  /* This pass does not create or rename scopes, so the scope-shape index is
     stable across it: freeze so the per-class method lookups below (called for
     every CallNode * every class) hit the O(1) hash index instead of the
     unfrozen O(nscopes) linear reverse scan -- the dominant analyze cost on
     large apps (lobsters: ~72% of compile time was this triple loop). */
  comp_scope_index_set_frozen(1);
  /* The per-call fallback below resolved an unmatched name by probing every
     class (comp_method_in_class + comp_cmethod_in_class for k in 0..nclasses).
     Builtin call names (each/map/puts/...) match no class, so most CallNodes
     paid the full O(nclasses) scan: O(calls * classes) overall. Build a
     name -> first-defining-scope map once instead -- inserting classes in
     ascending order keeps the exact first-match semantics of the old loop. */
  int nix_n = 0;
  NonstrIdxWrite *nix = nonstr_idx_writes(c, &nix_n);
  int mm_cap = 64;
  while (mm_cap < c->nscopes * 2) mm_cap <<= 1;
  struct { const char *nm; int mi; } *mm = calloc((size_t)mm_cap, sizeof *mm);
  if (!mm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id < 0 || !s->name) continue;
    unsigned long h = 5381;
    for (const char *p = s->name; *p; p++) h = h * 33 + (unsigned char)*p;
    for (int probe = (int)(h & (mm_cap - 1));; probe = (probe + 1) & (mm_cap - 1)) {
      if (!mm[probe].nm) { mm[probe].nm = s->name; mm[probe].mi = si; break; }
      if (sp_streq(mm[probe].nm, s->name)) {
        /* keep the entry the old class-ascending, methods-before-cmethods
           probe order would have found first */
        Scope *o = &c->scopes[mm[probe].mi];
        if (s->class_id < o->class_id ||
            (s->class_id == o->class_id && !s->is_cmethod && o->is_cmethod))
          mm[probe].mi = si;
        break;
      }
    }
  }
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    /* Resolve to a user method. Two shapes need the widest hash for an empty
       `{}` arg, because the precise body-write widening (#397) can't reach the
       key/value types: (a) a YIELD-wrapper builds the seed through its block
       (`acc = yield(acc, x)`); (b) the matching param is index-written with a
       non-string key (`p[i]=v`), which #397 skips as array-ambiguous, leaving
       the param poly so a `[]=` widen drops the mutation on the caller's `{}`.
       A string-keyed direct-index wrapper stays with the precise mechanism. */
    int mi = comp_method_index(c, nm);
    if (mi < 0) {
      unsigned long h = 5381;
      for (const char *p = nm; *p; p++) h = h * 33 + (unsigned char)*p;
      for (int probe = (int)(h & (mm_cap - 1));; probe = (probe + 1) & (mm_cap - 1)) {
        if (!mm[probe].nm) break;
        if (sp_streq(mm[probe].nm, nm)) { mi = mm[probe].mi; break; }
      }
    }
    if (mi < 0) continue;
    int yields = c->scopes[mi].yields;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      int a = args[j];
      if (a < 0 || a >= c->node_cap) continue;
      int want = yields || param_nonstr_index_written(c, mi, j, nix, nix_n);
      if (!want) continue;
      NodeKind ak = nt_kind(nt, a);
      if (ak == NK_HashNode || ak == NK_KeywordHashNode) {
        int en = 0; nt_arr(nt, a, "elements", &en);
        if (en == 0) c->empty_hash_arg[a] = 1;
      }
      /* `h = {}; m(h)`: the arg is a local whose value is the empty literal.
         Mark that write's `{}` so the local (and thus the param) is the widest
         hash -- the mutation then lands in place on the caller's hash. */
      else if (ak == NK_LocalVariableReadNode) {
        const char *ln = nt_str(nt, a, "name");
        Scope *asc = comp_scope_of(c, a);
        if (ln && asc && local_all_writes_empty_hash(c, asc, ln)) {
          for (int w = 0; w < nt->count; w++) {
            if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
            const char *wn = nt_str(nt, w, "name");
            if (!wn || !sp_streq(wn, ln) || comp_scope_of(c, w) != asc) continue;
            int wv = nt_ref(nt, w, "value");
            if (wv >= 0 && wv < c->node_cap && nt_kind(nt, wv) == NK_HashNode)
              c->empty_hash_arg[wv] = 1;
          }
        }
      }
    }
  }
  free(mm);
  free(nix);
  comp_scope_index_set_frozen(0);
}
/* An empty `{}` compared against a hash-typed peer takes that peer's variant,
   so the two sides share a representation and `==` can compare contents
   instead of collapsing to a constant false (#3040). The literal carries no
   keys of its own, so adopting the peer's variant loses nothing. */
static void mark_empty_hash_cmp_peers(Compiler *c) {
  if (!c->empty_hash_want) return;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "==") && !sp_streq(nm, "!=") && !sp_streq(nm, "eql?"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    if (recv < 0 || an != 1) continue;
    int arg = av[0];
    for (int side = 0; side < 2; side++) {
      int lit = side ? arg : recv, peer = side ? recv : arg;
      if (lit < 0 || lit >= c->node_cap || peer < 0) continue;
      NodeKind lk = nt_kind(nt, lit);
      if (lk != NK_HashNode && lk != NK_KeywordHashNode) continue;
      int en = 0; nt_arr(nt, lit, "elements", &en);
      if (en != 0) continue;
      TyKind pt = infer_type(c, peer);
      if (ty_is_hash(pt)) c->empty_hash_want[lit] = pt;
    }
  }
}
/* A bare `{}` indexed or fetched with a statically-typed key takes the variant
   that key selects, instead of the StrPolyHash fallback that would coerce a
   Symbol key to a String (#3028) or emit ill-typed C for an Integer one
   (#3029). Values stay poly: the empty literal says nothing about them. */
static int mark_empty_hash_key_ctx(Compiler *c) {
  int changed = 0;
  if (!c->empty_hash_want) return 0;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    NodeKind idk = nt_kind(nt, id);
    /* `h[k] ||= v` and friends select a variant by their key exactly like a
       read does, but they are not CallNodes, so the name gate below never saw
       them and an Integer key fell back to StrPolyHash (#3353) */
    int is_idx_write = (idk == NK_IndexOrWriteNode || idk == NK_IndexAndWriteNode ||
                        idk == NK_IndexOperatorWriteNode);
    if (idk != NK_CallNode && !is_idx_write) continue;
    const char *nm = is_idx_write ? "[]" : nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "fetch") && !sp_streq(nm, "[]") &&
                !sp_streq(nm, "key?") && !sp_streq(nm, "has_key?") &&
                !sp_streq(nm, "include?") && !sp_streq(nm, "dig"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || recv >= c->node_cap) continue;
    NodeKind rk = nt_kind(nt, recv);
    int direct = (rk == NK_HashNode || rk == NK_KeywordHashNode);
    if (direct) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en != 0) continue;
      if (ty_is_hash(c->empty_hash_want[recv])) continue;   /* an earlier context won */
    }
    else if (rk != NK_LocalVariableReadNode && rk != NK_InstanceVariableReadNode) continue;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    if (an < 1) continue;
    TyKind kt = infer_type(c, av[0]);
    TyKind want = TY_UNKNOWN;
    if (kt == TY_SYMBOL) want = TY_SYM_POLY_HASH;
    else if (kt == TY_STRING) {
      /* The key operations this pass keys off (`key?`, `[]`, `fetch`, `dig`)
         say what the KEY is and nothing about the value, so the poly-valued
         variant was the only answer available here. But the slot's own
         `h[k] = v` writes do say, and this mark is permanent -- the variant
         only ever widens afterwards, so pinning poly loses a concrete value
         type that is right there. Read the writes; if they exist but have no
         type yet, wait rather than guess (see g_infer_optimistic). */
      int nw = 0;
      TyKind wv = aset_value_type_ex(c, recv, &nw);
      if (nw > 0 && wv == TY_UNKNOWN && g_infer_optimistic) continue;
      want = (wv == TY_INT || wv == TY_STRING) ? ty_hash_of(TY_STRING, wv)
                                               : TY_STR_POLY_HASH;
    }
    else if (kt == TY_INT) want = TY_POLY_POLY_HASH;  /* no Int->poly variant */
    /* a boxed key (one call site passes a String, another an Integer) is only
       representable by the poly-keyed variant; the StrPolyHash default would
       hand the boxed value to a const char * slot and segfault */
    else if (kt == TY_POLY) want = TY_POLY_POLY_HASH;
    if (!ty_is_hash(want)) continue;
    if (direct) { c->empty_hash_want[recv] = want; changed = 1; continue; }
    /* `@h = {}` in initialize, indexed with a typed key elsewhere in the same
       class: carry the context to the ivar's own empty literal, or the slot
       stays typeless and the index-write has no hash to emit against */
    if (rk == NK_InstanceVariableReadNode) {
      const char *ivn = nt_str(nt, recv, "name");
      Scope *rs = comp_scope_of(c, recv);
      int rcid = rs ? rs->class_id : -1;
      if (!ivn || rcid < 0) continue;
      for (int w = 0; w < nt->count; w++) {
        if (nt_kind(nt, w) != NK_InstanceVariableWriteNode) continue;
        const char *wn = nt_str(nt, w, "name");
        Scope *ws = comp_scope_of(c, w);
        if (!wn || !sp_streq(wn, ivn) || !ws || ws->class_id != rcid) continue;
        int wv = nt_ref(nt, w, "value");
        if (wv < 0 || wv >= c->node_cap || nt_kind(nt, wv) != NK_HashNode) continue;
        int en2 = 0; nt_arr(nt, wv, "elements", &en2);
        if (en2 != 0 || ty_is_hash(c->empty_hash_want[wv])) continue;
        c->empty_hash_want[wv] = want;
        changed = 1;
      }
      continue;
    }
    /* `h = {}; h.fetch(k)`: carry the key context back to the write's literal,
       so the local takes that variant instead of the StrPolyHash default */
    const char *ln = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    if (!ln || !sc || !local_all_writes_empty_hash(c, sc, ln)) continue;
    for (int w = 0; w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, ln) || comp_scope_of(c, w) != sc) continue;
      int wv = nt_ref(nt, w, "value");
      if (wv >= 0 && wv < c->node_cap && nt_kind(nt, wv) == NK_HashNode &&
          !ty_is_hash(c->empty_hash_want[wv])) {
        c->empty_hash_want[wv] = want;
        changed = 1;
      }
    }
  }
  return changed;
}
/* `TBL = {}` followed by `TBL[k] = v` elsewhere: an empty literal bound to a
   constant has no type of its own, so the constant got no runtime slot at all
   and every read raised "uninitialized constant". Derive the variant from the
   constant's index-writes (#2879). */
static void mark_empty_hash_const_writes(Compiler *c) {
  if (!c->empty_hash_want) return;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_ConstantWriteNode) continue;
    const char *cn = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (!cn || v < 0 || v >= c->node_cap) continue;
    if (nt_kind(nt, v) != NK_HashNode && nt_kind(nt, v) != NK_KeywordHashNode) continue;
    int en = 0; nt_arr(nt, v, "elements", &en);
    if (en != 0 || ty_is_hash(c->empty_hash_want[v])) continue;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int w = 0; w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_CallNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, "[]=")) continue;
      int wr = nt_ref(nt, w, "receiver");
      if (wr < 0 || nt_kind(nt, wr) != NK_ConstantReadNode) continue;
      const char *rn = nt_str(nt, wr, "name");
      if (!rn || !sp_streq(rn, cn)) continue;
      int wa = nt_ref(nt, w, "arguments");
      int wan = 0; const int *wav = wa >= 0 ? nt_arr(nt, wa, "arguments", &wan) : NULL;
      if (wan < 2) continue;
      kt = ty_unify(kt, infer_type(c, wav[0]));
      vt = ty_unify(vt, infer_type(c, wav[1]));
    }
    /* No resolved index-write: the constant still needs a slot, so give it the
       widest variant rather than leaving it typeless (and unreachable). */
    TyKind want = (kt == TY_SYMBOL) ? TY_SYM_POLY_HASH
                : (kt == TY_UNKNOWN) ? TY_POLY_POLY_HASH : ty_hash_of(kt, vt);
    if (!ty_is_hash(want)) want = (kt == TY_STRING) ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
    c->empty_hash_want[v] = want;
  }
}
static void mark_empty_hash_receivers(Compiler *c) {
  if (!c->empty_hash_recv) return;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    if (nt_ref(nt, id, "block") < 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || recv >= c->node_cap) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || (!sp_streq(rty, "HashNode") && !sp_streq(rty, "KeywordHashNode"))) continue;
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) c->empty_hash_recv[recv] = 1;
  }
}

/* --- byref string out-params (see LocalVar.byref_out) ---------------------
   CRuby strings are shared heap objects: a callee's `s << "x"` mutates the
   very object the caller passed. Spinel strings are immutable const char*
   values and `<<` is a local reassignment, so the mutation silently stops at
   the callee -- the classic pass-an-output-buffer pattern loses every append.
   Mark the mutated string params of statically-bound methods byref: codegen
   passes the caller's slot (const char **), the callee reads and reassigns
   through it (the existing is_cell deref forms), and the mutation lands in
   the caller's variable like CRuby.

   Eligibility is deliberately narrow -- byref changes the C ABI, so every
   call site must know the callee statically and no plain rebind may leak:
   - toplevel / class (singleton) methods only: an instance method can be
     reached through poly dispatch stubs and same-name subtree dispatch,
     which keep the plain ABI;
   - the name is unique program-wide, never aliased, and never appears as a
     symbol/string literal (send/method(:x)/define_method material);
   - all params required positional (no rest/kw/defaults: those fill slots
     through temps and per-name extraction, where an address is meaningless);
   - the param is mutated via a receiver-reassigning string mutator (`<<`,
     replace, prepend, insert, clear, concat, bang forms) or passed on into
     another method's byref slot (fixpoint), and is never PLAIN-reassigned:
     CRuby `s = ...` rebinds the local invisibly to the caller, which a
     write-through cell would wrongly propagate. */
static int an_str_mutator_name(const char *nm) {
  size_t l = nm ? strlen(nm) : 0;
  if (!l) return 0;
  return sp_streq(nm, "<<") || sp_streq(nm, "concat") || sp_streq(nm, "replace") ||
         sp_streq(nm, "prepend") || sp_streq(nm, "insert") || sp_streq(nm, "clear") ||
         sp_streq(nm, "[]=") || (l > 1 && nm[l - 1] == '!');
}

/* Unique method scope index by name across the program, or -1 (absent or
   defined more than once). Scope 0 is the top level (name NULL). */
static int an_unique_scope_by_name(Compiler *c, const char *nm) {
  int found = -1;
  if (!nm) return -1;
  for (int i = 1; i < c->nscopes; i++) {
    Scope *s = &c->scopes[i];
    if (!s->name || !sp_streq(s->name, nm)) continue;
    if (found >= 0) return -1;
    found = i;
  }
  return found;
}

/* Param index of `vn` in s->pnames, or -1. */
static int an_param_idx(Scope *s, const char *vn) {
  if (!vn) return -1;
  for (int i = 0; i < s->nparams; i++)
    if (s->pnames[i] && sp_streq(s->pnames[i], vn)) return i;
  return -1;
}

int comp_byref_param(Compiler *c, Scope *m, int idx) {
  if (!m || idx < 0 || idx >= m->nparams || !m->pnames[idx]) return 0;
  LocalVar *p = scope_local(m, m->pnames[idx]);
  return p && p->byref_out;
}

static void compute_byref_out_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = c->nscopes;
  char *elig = calloc((size_t)n, 1);
  unsigned *blocked = calloc((size_t)n, sizeof(unsigned));  /* per-scope param bitmask */
  if (!elig || !blocked) { free(elig); free(blocked); return; }

  for (int si = 1; si < n; si++) {
    Scope *s = &c->scopes[si];
    if (!s->name || s->def_node < 0 || s->body < 0) continue;
    if (s->yields || s->is_lowered_yield || s->dm_subst_name || s->cs_synth) continue;
    if (s->is_transplanted_source) continue;
    if (s->class_id >= 0 && !s->is_cmethod) continue;
    if (s->rest_idx >= 0 || s->kwrest_idx >= 0 || s->npost_rest > 0) continue;
    if (s->nparams <= 0 || s->nparams > 32 || s->nrequired != s->nparams) continue;
    int pn = nt_ref(nt, s->def_node, "parameters");
    if (pn >= 0) {
      int kn = 0; nt_arr(nt, pn, "keywords", &kn);
      if (kn > 0) continue;
    }
    if (an_unique_scope_by_name(c, s->name) != si) continue;
    elig[si] = 1;
  }
  /* an aliased name reaches the method under another spelling; the alias call
     sites keep the plain ABI, so the method must too */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int a = 0; a < cls->naliases; a++)
      for (int si = 1; si < n; si++)
        if (elig[si] && ((cls->alias_old[a] && sp_streq(cls->alias_old[a], c->scopes[si].name)) ||
                         (cls->alias_new[a] && sp_streq(cls->alias_new[a], c->scopes[si].name))))
          elig[si] = 0;
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* a symbol/string literal spelling the name: send / method(:x) /
       define_method / respond_to? -- all reach the plain ABI */
    const char *v = NULL;
    if (sp_streq(ty, "SymbolNode")) {
      v = nt_str(nt, id, "value");
      if (!v) v = nt_str(nt, id, "unescaped");
    }
    else if (sp_streq(ty, "StringNode")) {
      v = nt_str(nt, id, "content");
      if (!v) v = nt_str(nt, id, "unescaped");
    }
    if (v)
      for (int si = 1; si < n; si++)
        if (elig[si] && sp_streq(c->scopes[si].name, v)) elig[si] = 0;
    /* a plain rebind of a param blocks it (see the header comment) */
    if (sp_streq(ty, "LocalVariableWriteNode") ||
        sp_streq(ty, "LocalVariableOperatorWriteNode") ||
        sp_streq(ty, "LocalVariableOrWriteNode") ||
        sp_streq(ty, "LocalVariableAndWriteNode") ||
        sp_streq(ty, "LocalVariableTargetNode")) {
      Scope *s = comp_scope_of(c, id);
      int si = s ? (int)(s - c->scopes) : -1;
      if (si > 0 && si < n && elig[si]) {
        int pi = an_param_idx(s, nt_str(nt, id, "name"));
        if (pi >= 0 && pi < 32) blocked[si] |= 1u << pi;
      }
    }
  }

  int changed = 1;
  while (changed) {
    changed = 0;
    for (int id = 0; id < nt->count; id++) {
      const char *ty = nt_type(nt, id);
      if (!ty || !sp_streq(ty, "CallNode")) continue;
      Scope *s = comp_scope_of(c, id);
      int si = s ? (int)(s - c->scopes) : -1;
      if (si <= 0 || si >= n || !elig[si]) continue;
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
      /* direct mutator: param << ... / param.gsub!(...) */
      if (rty && sp_streq(rty, "LocalVariableReadNode") && an_str_mutator_name(nm)) {
        const char *vn = nt_str(nt, recv, "name");
        int pi = an_param_idx(s, vn);
        if (pi >= 0 && pi < 32 && !(blocked[si] & (1u << pi))) {
          LocalVar *p = scope_local(s, vn);
          if (p && p->is_param && p->type == TY_STRING && !p->is_cell &&
              !p->rbs_seeded && !p->is_block_param && !p->byref_out) {
            p->byref_out = 1;
            p->is_cell = 1;   /* body reads/writes ride the cell deref forms */
            changed = 1;
          }
        }
      }
      /* transitive: the param passed on into another method's byref slot */
      if (recv < 0 || (rty && (sp_streq(rty, "ConstantReadNode") ||
                               sp_streq(rty, "ConstantPathNode") ||
                               sp_streq(rty, "SelfNode")))) {
        int mi = an_unique_scope_by_name(c, nm);
        if (mi < 0) continue;
        Scope *m = &c->scopes[mi];
        int argsN = nt_ref(nt, id, "arguments");
        int argc2 = 0;
        const int *argv2 = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &argc2) : NULL;
        for (int j = 0; j < argc2 && j < m->nparams; j++) {
          if (!comp_byref_param(c, m, j)) continue;
          const char *aty = nt_type(nt, argv2[j]);
          if (!aty || !sp_streq(aty, "LocalVariableReadNode")) continue;
          const char *vn = nt_str(nt, argv2[j], "name");
          int pi = an_param_idx(s, vn);
          if (pi < 0 || pi >= 32 || (blocked[si] & (1u << pi))) continue;
          LocalVar *p = scope_local(s, vn);
          if (p && p->is_param && p->type == TY_STRING && !p->is_cell &&
              !p->rbs_seeded && !p->is_block_param && !p->byref_out) {
            p->byref_out = 1;
            p->is_cell = 1;
            changed = 1;
          }
        }
      }
    }
  }
  free(elig);
  free(blocked);
}

/* Does scope mi's body contain a call to its own method name on implicit or
   explicit self? The self-recursion test shared by the yield-lowering
   decisions: such a method must not take the inline path (expansion cannot
   terminate) -- it stays a real function with a proc-materialized block. */
static int scope_calls_itself(Compiler *c, int mi) {
  Scope *m = &c->scopes[mi];
  if (!m->name || m->body < 0) return 0;
  for (int id = 0; id < c->nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, m->name)) continue;
    int recv = nt_ref(c->nt, id, "receiver");
    const char *rty = recv >= 0 ? nt_type(c->nt, recv) : NULL;
    if (recv < 0 || (rty && sp_streq(rty, "SelfNode"))) return 1;
  }
  return 0;
}

/* Does any `initialize` in ci's chain write @<name>? A ctor that exists but
   never assigns a given ivar leaves it nil on a fresh instance, same as no
   ctor at all -- so the nil-backstop below must key on the actual write, not
   on the mere presence of an initialize (#3136). */
static int ctor_writes_ivar(Compiler *c, int ci, const char *ivname) {
  NodeTable *nt = (NodeTable *)c->nt;
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int mi = comp_method_in_class(c, k, "initialize");
    if (mi < 0) continue;
    int def = c->scopes[mi].def_node;
    if (def < 0) continue;
    /* scan the whole ctor subtree for a write to this ivar */
    for (int id = 0; id < nt->count; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(nt, id);
      if (!ty) continue;
      if (sp_streq(ty, "InstanceVariableWriteNode") ||
          sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
          sp_streq(ty, "InstanceVariableOrWriteNode") ||
          sp_streq(ty, "InstanceVariableAndWriteNode") ||
          sp_streq(ty, "InstanceVariableTargetNode")) {
        const char *wn = nt_str(nt, id, "name");
        if (wn && sp_streq(wn, ivname)) {
          /* A ctor write of a plain `nil` literal does not SEED the slot with a
             concrete value -- it explicitly leaves it nil. If every other write
             (elsewhere) is a scalar, the slot is nullable and must widen to poly,
             so a nil-only ctor write must not block the poly backstop (#3143).
             A non-nil write, or any operator/or/and/target write, does seed. */
          if (sp_streq(ty, "InstanceVariableWriteNode")) {
            int wv = nt_ref(nt, id, "value");
            const char *vty = wv >= 0 ? nt_type(nt, wv) : NULL;
            if (vty && sp_streq(vty, "NilNode")) continue;
          }
          return 1;
        }
      }
    }
  }
  return 0;
}



/* Shared-mutable strings, phase 3 (#3227): a container STORE (array/hash
   literal element, `arr << s`, `arr[i] = s`) or an `equal?` argument of an
   in-place-mutated string local promotes it to a shared TY_STRBUF handle
   DURING the fixpoint, and marks that read in c->strbuf_box so infer types
   it TY_STRBUF (the container then unifies to a poly variant and emit_boxed
   wraps the sp_String* as SP_BUILTIN_STRBUF). Unmarked reads of the same
   local keep demoting to TY_STRING, so ordinary consumers are unchanged. */
/* In-place mutation status of string local `vn` in scope `vs`:
   1 = mutated via a handle-capable mutator, -1 = disqualified (a mutator
   codegen cannot yet run in place on the handle), 0 = not mutated. */
static int strbuf_mut_kind(Compiler *c, const char *vn, Scope *vs) {
  const NodeTable *nt = c->nt;
  int mutated = 0;
  for (int u = 0; u < nt->count; u++) {
    const char *uty = nt_type(nt, u);
    if (!uty || !sp_streq(uty, "CallNode")) continue;
    int ur = nt_ref(nt, u, "receiver");
    if (ur < 0 || !nt_type(nt, ur) ||
        !sp_streq(nt_type(nt, ur), "LocalVariableReadNode")) continue;
    const char *urn = nt_str(nt, ur, "name");
    if (!urn || !sp_streq(urn, vn) || comp_scope_of(c, ur) != vs) continue;
    const char *un = nt_str(nt, u, "name");
    if (!un) continue;
    size_t ul = strlen(un);
    if (sp_str_mutator(un, SP_MUT_LOCAL)) mutated = 1;
    else if (ul > 0 && un[ul - 1] == '!')
      return -1;
  }
  return mutated;
}
/* In-place mutation status of ivar `nm` of class `cid` (same contract as
   strbuf_mut_kind). The supported set is narrower: the shadow-copy shim
   cannot rename an ivar, so []=/insert/slice!/setbyte disqualify. */
static int strbuf_ivar_mut_kind(Compiler *c, int cid, const char *nm) {
  const NodeTable *nt = c->nt;
  int mutated = 0;
  for (int u = 0; u < nt->count; u++) {
    if (nt_kind(nt, u) != NK_CallNode) continue;
    int ur = nt_ref(nt, u, "receiver");
    if (ur < 0 || nt_kind(nt, ur) != NK_InstanceVariableReadNode) continue;
    const char *urn = nt_str(nt, ur, "name");
    if (!urn || !sp_streq(urn, nm)) continue;
    Scope *us = comp_scope_of(c, ur);
    int ucid = -1;
    if (us && !us->is_cmethod && us->class_id >= 0) ucid = us->class_id;
    else if (us && !us->is_cmethod && us->class_id < 0) ucid = comp_class_index(c, "Toplevel");
    if (ucid != cid) continue;
    const char *un = nt_str(nt, u, "name");
    if (!un) continue;
    size_t ul = strlen(un);
    if (sp_str_mutator(un, SP_MUT_IVAR)) mutated = 1;
    else if (sp_streq(un, "insert") || sp_streq(un, "slice!") ||
             sp_streq(un, "[]=") || sp_streq(un, "setbyte") ||
             (ul > 0 && un[ul - 1] == '!'))
      return -1;
  }
  return mutated;
}
/* The owning class of an ivar READ/WRITE node under the same storage rules
   the emitters use (instance method -> class, top-level -> Toplevel; class
   methods / instance_eval contexts return -1). */
static int an_ivar_owner(Compiler *c, int node) {
  Scope *cs = comp_scope_of(c, node);
  if (!cs || cs->is_cmethod) return -1;
  if (cs->class_id >= 0) return cs->class_id;
  return comp_class_index(c, "Toplevel");
}
/* Promote ivar slot (cid, iv) to the shared handle if eligible; returns 1
   on a state change. */
static int strbuf_promote_ivar(Compiler *c, int cid, const char *nm) {
  if (cid < 0) return 0;
  ClassInfo *ci = &c->classes[cid];
  int iv = comp_ivar_index(ci, nm);
  if (iv < 0) return 0;
  if (ci->ivar_types[iv] != TY_STRING && ci->ivar_types[iv] != TY_STRBUF) return 0;
  if (ci->ivar_types[iv] == TY_STRBUF && ci->ivar_str_shared[iv]) return 0;
  ci->ivar_types[iv] = TY_STRBUF;
  ci->ivar_str_shared[iv] = 1;
  return 1;
}

/* When `node` is an argument-less reader call (attr or a simple
   `def m = @iv` method) over an object receiver, resolve the backing ivar:
   returns the ivar name (into buf) and sets *defc, else NULL. */
static const char *an_reader_ivar_of(Compiler *c, int node, int *defc,
                                     char *buf, size_t cap) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return NULL;
  if (nt_ref(nt, node, "block") >= 0) return NULL;
  { int a2 = nt_ref(nt, node, "arguments"); int an2 = 0;
    if (a2 >= 0) nt_arr(nt, a2, "arguments", &an2);
    if (an2 != 0) return NULL; }
  int rrecv = nt_ref(nt, node, "receiver");
  if (rrecv < 0) return NULL;
  TyKind rrt = infer_type(c, rrecv);
  if (!ty_is_object(rrt)) return NULL;
  int rcid = ty_object_class(rrt);
  const char *mn = nt_str(nt, node, "name");
  if (!mn) return NULL;
  *defc = rcid;
  if (comp_reader_in_chain(c, rcid, mn, defc)) {
    snprintf(buf, cap, "@%s", comp_resolve_alias(c, rcid, mn));
    return buf;
  }
  int rmi = comp_method_in_chain(c, rcid, mn, defc);
  if (rmi < 0) return NULL;
  int last2 = scope_body_last(c, rmi);
  if (last2 < 0 || nt_kind(nt, last2) != NK_InstanceVariableReadNode) return NULL;
  const char *ivn = nt_str(nt, last2, "name");
  if (!ivn) return NULL;
  snprintf(buf, cap, "%s", ivn);
  if (*defc < 0) *defc = c->scopes[rmi].class_id;
  return buf;
}
/* Does local `vn` participate in a pure alias (`x = vn` or `vn = x`)? */
/* The LocalVariableReadNode an aliasing string write bottoms out at: a bare
   local read, or a value-position `<<`/`concat` chain over one (the chain's
   value IS its base object). Parens unwrap. -1 when the shape is neither. */
static int an_strbuf_alias_source(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  for (int depth = 0; v >= 0 && depth < 64; depth++) {
    const char *vt = nt_type(nt, v);
    if (!vt) return -1;
    if (sp_streq(vt, "ParenthesesNode")) {
      int pb = nt_ref(nt, v, "body");
      int bn = 0; const int *bb = pb >= 0 ? nt_arr(nt, pb, "body", &bn) : NULL;
      if (bn != 1) return -1;
      v = bb[0]; continue;
    }
    if (sp_streq(vt, "LocalVariableReadNode")) return v;
    if (sp_streq(vt, "CallNode")) {
      const char *cn = nt_str(nt, v, "name");
      int cr = nt_ref(nt, v, "receiver");
      int ca = nt_ref(nt, v, "arguments"); int cac = 0;
      if (ca >= 0) nt_arr(nt, ca, "arguments", &cac);
      if (cn && (sp_streq(cn, "<<") || sp_streq(cn, "concat")) &&
          cr >= 0 && cac == 1) { v = cr; continue; }
      return -1;
    }
    return -1;
  }
  return -1;
}
static int an_local_has_alias(Compiler *c, const char *vn, Scope *vs) {
  const NodeTable *nt = c->nt;
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    if (comp_scope_of(c, w) != vs) continue;
    const char *wn = nt_str(nt, w, "name");
    int wv = an_strbuf_alias_source(c, nt_ref(nt, w, "value"));
    if (wv < 0) continue;
    const char *rn = nt_str(nt, wv, "name");
    if (!wn || !rn || sp_streq(wn, rn)) continue;
    if (sp_streq(wn, vn) || sp_streq(rn, vn)) return 1;
  }
  return 0;
}
/* Is this argument node a shared-handle slot read (a str_shared local or a
   shared ivar)? */
static int an_arg_is_shared_handle(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  if (nt_kind(nt, node) == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, node, "name");
    Scope *vs = vn ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = vs ? scope_local(vs, vn) : NULL;
    /* str_shared is the durable flag; the slot type may transiently read
       TY_STRING until the post-fixpoint re-assert runs */
    return lv && lv->str_shared &&
           (lv->type == TY_STRBUF || lv->type == TY_STRING);
  }
  if (nt_kind(nt, node) == NK_InstanceVariableReadNode) {
    const char *vn = nt_str(nt, node, "name");
    int cid = vn ? an_ivar_owner(c, node) : -1;
    if (cid < 0) return 0;
    int iv = comp_ivar_index(&c->classes[cid], vn);
    return iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF &&
           c->classes[cid].ivar_str_shared[iv];
  }
  return 0;
}

static int strbuf_slot_eligible(Compiler *c, const char *vn, Scope *vs, LocalVar *lv);
static int strbuf_mut_kind(Compiler *c, const char *vn, Scope *vs);
/* Demand every string stored into container local (contn, conts) to be a
   shared handle: stored eligible locals promote, everything else marks for
   a fresh-handle wrap at the store site. Returns changed. */
static int strbuf_demand_container_stores(Compiler *c, const char *contn, Scope *conts) {
  const NodeTable *nt = c->nt;
  int changed = 0;
    /* every store into this container local (same scope): array/hash
       literal writes, push/<<, []= */
    for (int w2 = 0; w2 < nt->count; w2++) {
      int nst = 0; int stores[64];
      NodeKind wk = nt_kind(nt, w2);
      if (wk == NK_LocalVariableWriteNode) {
        const char *wn2 = nt_str(nt, w2, "name");
        if (!wn2 || !sp_streq(wn2, contn) || comp_scope_of(c, w2) != conts) continue;
        int val2 = nt_ref(nt, w2, "value");
        if (val2 < 0) continue;
        NodeKind vk = nt_kind(nt, val2);
        if (vk == NK_ArrayNode) {
          int en2 = 0; const int *el2 = nt_arr(nt, val2, "elements", &en2);
          for (int e2 = 0; e2 < en2 && nst < 64; e2++) stores[nst++] = el2[e2];
        }
        else if (vk == NK_HashNode || vk == NK_KeywordHashNode) {
          int en2 = 0; const int *el2 = nt_arr(nt, val2, "elements", &en2);
          for (int e2 = 0; e2 < en2 && nst < 64; e2++)
            if (nt_kind(nt, el2[e2]) == NK_AssocNode)
              stores[nst++] = nt_ref(nt, el2[e2], "value");
        }
      }
      else if (wk == NK_CallNode) {
        int wr2 = nt_ref(nt, w2, "receiver");
        if (wr2 < 0 || nt_kind(nt, wr2) != NK_LocalVariableReadNode) continue;
        const char *wrn2 = nt_str(nt, wr2, "name");
        if (!wrn2 || !sp_streq(wrn2, contn) || comp_scope_of(c, wr2) != conts) continue;
        const char *wcn2 = nt_str(nt, w2, "name");
        if (!wcn2) continue;
        int a2 = nt_ref(nt, w2, "arguments");
        int an2 = 0; const int *av2 = a2 >= 0 ? nt_arr(nt, a2, "arguments", &an2) : NULL;
        if (sp_streq(wcn2, "<<") || sp_streq(wcn2, "push") ||
            sp_streq(wcn2, "append") || sp_streq(wcn2, "unshift")) {
          for (int e2 = 0; e2 < an2 && nst < 64; e2++) stores[nst++] = av2[e2];
        }
        else if (sp_streq(wcn2, "[]=") && an2 >= 2) stores[nst++] = av2[an2 - 1];
        else continue;
      }
      else continue;
      for (int e3 = 0; e3 < nst; e3++) {
        int sn = stores[e3];
        if (sn < 0 || c->strbuf_box[sn]) continue;
        NodeKind sk = nt_kind(nt, sn);
        if (sk == NK_LocalVariableReadNode) {
          const char *snm = nt_str(nt, sn, "name");
          Scope *sns = comp_scope_of(c, sn);
          LocalVar *snv = (snm && sns) ? scope_local(sns, snm) : NULL;
          if (!snv || !strbuf_slot_eligible(c, snm, sns, snv)) continue;
          if (strbuf_mut_kind(c, snm, sns) < 0) continue;
          snv->type = TY_STRBUF; snv->str_shared = 1;
          c->strbuf_box[sn] = 1; changed = 1;
        }
        else {
          TyKind st2 = infer_type(c, sn);
          if (st2 != TY_STRING && st2 != TY_STRBUF) continue;
          c->strbuf_box[sn] = 1; changed = 1;
        }
      }
    }
  return changed;
}

/* Slot-shape eligibility for the shared handle: parameters, cells, byref
   out-params and rbs-pinned slots wait for the cross-method phase. */
static int strbuf_slot_eligible_shape(Compiler *c, const char *vn, Scope *vs, LocalVar *lv);
static int strbuf_slot_eligible(Compiler *c, const char *vn, Scope *vs, LocalVar *lv) {
  if (!lv) return 0;
  if (lv->type != TY_STRING && lv->type != TY_STRBUF) return 0;
  return strbuf_slot_eligible_shape(c, vn, vs, lv);
}
/* shape-only eligibility (no type gate): the ALIAS arm may see the local
   while its type is still a transient UNKNOWN/POLY mid-fixpoint */
static int strbuf_slot_eligible_shape(Compiler *c, const char *vn, Scope *vs, LocalVar *lv) {
  const NodeTable *nt = c->nt;
  /* an rbs-seeded String slot may still take the handle REPRESENTATION:
     the pin constrains the Ruby-level type, not the C storage (#3227) */
  if (!lv || lv->is_param) return 0;
  for (int u = 0; u < nt->count; u++) {
    const char *uty = nt_type(nt, u);
    if (!uty || !sp_streq(uty, "CallNode")) continue;
    if (comp_scope_of(c, u) != vs) continue;
    int mi = an_unique_scope_by_name(c, nt_str(nt, u, "name"));
    if (mi < 0) continue;
    int argsN = nt_ref(nt, u, "arguments");
    int uargc = 0;
    const int *uargv = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &uargc) : NULL;
    for (int j = 0; j < uargc && j < c->scopes[mi].nparams; j++) {
      if (!comp_byref_param(c, &c->scopes[mi], j)) continue;
      const char *aty = nt_type(nt, uargv[j]);
      const char *an2 = aty && sp_streq(aty, "LocalVariableReadNode")
                          ? nt_str(nt, uargv[j], "name") : NULL;
      if (an2 && sp_streq(an2, vn)) return 0;
    }
  }
  return 1;
}

static int promote_shared_stored_strings(Compiler *c) {
  int changed = 0;
  const NodeTable *nt = c->nt;
  /* mutated string locals: receivers of an in-place mutator */
  for (int w = 0; w < nt->count; w++) {
    const char *wty = nt_type(nt, w);
    if (!wty) continue;
    int cand3[64]; int nc3 = 0;
    if (sp_streq(wty, "ArrayNode")) {
      int en3 = 0; const int *el3 = nt_arr(nt, w, "elements", &en3);
      for (int e3 = 0; e3 < en3 && nc3 < 64; e3++) cand3[nc3++] = el3[e3];
    }
    else if (sp_streq(wty, "HashNode") || sp_streq(wty, "KeywordHashNode")) {
      int en3 = 0; const int *el3 = nt_arr(nt, w, "elements", &en3);
      for (int e3 = 0; e3 < en3 && nc3 < 64; e3++) {
        if (nt_type(nt, el3[e3]) && sp_streq(nt_type(nt, el3[e3]), "AssocNode"))
          cand3[nc3++] = nt_ref(nt, el3[e3], "value");
      }
    }
    else if (sp_streq(wty, "CallNode")) {
      const char *cn3 = nt_str(nt, w, "name");
      int recv3 = nt_ref(nt, w, "receiver");
      TyKind rt3 = recv3 >= 0 ? c->ntype[recv3] : TY_UNKNOWN;
      if (cn3 && recv3 >= 0 &&
          (sp_streq(cn3, "<<") || sp_streq(cn3, "push") ||
           sp_streq(cn3, "append") || sp_streq(cn3, "unshift"))) {
        /* only when the receiver is a container -- `s1 << s2` is a string
           append, whose ARG must stay a plain read */
        if (ty_is_array(rt3)) {
          int a3 = nt_ref(nt, w, "arguments");
          int an3 = 0; const int *av3 = a3 >= 0 ? nt_arr(nt, a3, "arguments", &an3) : NULL;
          for (int e3 = 0; e3 < an3 && nc3 < 64; e3++) cand3[nc3++] = av3[e3];
        }
      }
      else if (cn3 && recv3 >= 0 && sp_streq(cn3, "[]=") &&
               (ty_is_array(rt3) || ty_is_hash(rt3))) {
        int a3 = nt_ref(nt, w, "arguments");
        int an3 = 0; const int *av3 = a3 >= 0 ? nt_arr(nt, a3, "arguments", &an3) : NULL;
        if (an3 >= 2) cand3[nc3++] = av3[an3 - 1];
      }
      else if (cn3 && recv3 >= 0 &&
               (sp_streq(cn3, "equal?") || sp_streq(cn3, "eql?"))) {
        /* identity test against the shared handle */
        int a3 = nt_ref(nt, w, "arguments");
        int an3 = 0; const int *av3 = a3 >= 0 ? nt_arr(nt, a3, "arguments", &an3) : NULL;
        if (an3 == 1) {
          cand3[nc3++] = av3[0];
          /* a reader call over a shared ivar as the identity operand hands
             out the handle too */
          char ivb3[300]; int defc3 = -1;
          const char *ivn3b = an_reader_ivar_of(c, av3[0], &defc3, ivb3, sizeof ivb3);
          if (ivn3b && defc3 >= 0) {
            int iv3b = comp_ivar_index(&c->classes[defc3], ivn3b);
            if (iv3b >= 0 && c->classes[defc3].ivar_str_shared[iv3b] &&
                !c->strbuf_box[av3[0]]) {
              c->strbuf_box[av3[0]] = 1; changed = 1;
            }
          }
        }
      }
    }
    for (int e3 = 0; e3 < nc3; e3++) {
      int vnode = cand3[e3];
      if (vnode < 0 || c->strbuf_box[vnode]) continue;
      /* a container-stored IVAR read: the slot itself promotes (#3227 P4) */
      if (nt_kind(nt, vnode) == NK_InstanceVariableReadNode) {
        const char *ivn3 = nt_str(nt, vnode, "name");
        int icid3 = ivn3 ? an_ivar_owner(c, vnode) : -1;
        if (icid3 >= 0 && strbuf_ivar_mut_kind(c, icid3, ivn3) == 1) {
          if (strbuf_promote_ivar(c, icid3, ivn3)) changed = 1;
          if (c->classes[icid3].ivar_str_shared[comp_ivar_index(&c->classes[icid3], ivn3)]) {
            c->strbuf_box[vnode] = 1; changed = 1;
          }
        }
        continue;
      }
      if (!nt_type(nt, vnode) ||
          !sp_streq(nt_type(nt, vnode), "LocalVariableReadNode")) continue;
      const char *vn3 = nt_str(nt, vnode, "name");
      Scope *vs3 = comp_scope_of(c, vnode);
      LocalVar *vlv = (vn3 && vs3) ? scope_local(vs3, vn3) : NULL;
      if (!strbuf_slot_eligible(c, vn3, vs3, vlv)) continue;
      /* already-shared (e.g. demanded through a return edge) qualifies even
         without a direct mutator of its own */
      if (strbuf_mut_kind(c, vn3, vs3) != 1 && !vlv->str_shared) continue;
      /* equal?/eql? args only join a set that is ALREADY shared (a store
         made it a handle); they must not themselves force the promotion */
      { const char *wn2 = nt_str(nt, w, "name");
        int is_eq = wty && sp_streq(wty, "CallNode") && wn2 &&
                    (sp_streq(wn2, "equal?") || sp_streq(wn2, "eql?"));
        if (is_eq && !vlv->str_shared) continue; }
      vlv->type = TY_STRBUF;
      vlv->str_shared = 1;
      c->strbuf_box[vnode] = 1;
      changed = 1;
    }
  }
  /* Container-read mutation (`arr[0].upcase!`, `h[:k] << x`): the mutator
     reaches the element THROUGH the container, so every string stored into
     that container must be the shared handle -- stored locals promote
     (regardless of their own direct mutation), and stored string literals /
     expression results are marked to wrap in a fresh sp_String at the store
     site. The container itself then unifies to the poly variant. */
  for (int mu = 0; mu < nt->count; mu++) {
    if (nt_kind(nt, mu) != NK_CallNode) continue;
    const char *mun = nt_str(nt, mu, "name");
    if (!mun) continue;
    {
      /* mutator name check (same handle-capable set) */
      if (!sp_str_mutator(mun, SP_MUT_CONTAINER)) continue;
    }
    int mrecv = nt_ref(nt, mu, "receiver");
    if (mrecv < 0 || nt_kind(nt, mrecv) != NK_CallNode) continue;
    const char *idxn = nt_str(nt, mrecv, "name");
    if (!idxn || !sp_streq(idxn, "[]")) continue;
    int cont = nt_ref(nt, mrecv, "receiver");
    if (cont < 0 || nt_kind(nt, cont) != NK_LocalVariableReadNode) continue;
    const char *contn = nt_str(nt, cont, "name");
    Scope *conts = contn ? comp_scope_of(c, cont) : NULL;
    LocalVar *contv = (contn && conts) ? scope_local(conts, contn) : NULL;
    if (!contv || (!ty_is_array(contv->type) && !ty_is_hash(contv->type) &&
                   contv->type != TY_UNKNOWN)) continue;
    changed |= strbuf_demand_container_stores(c, contn, conts);
  }

  /* External reader mutation (`expr.reader << x`): the mutator reaches the
     ivar THROUGH its reader, with the receiver an arbitrary object-typed
     expression (a container read, a call result). Promote the ivar to the
     shared handle and mark the reader read so its emission hands out the
     handle instead of the safe copy -- otherwise the mutation lands in a
     read-out copy and is silently lost. */
  for (int mu = 0; mu < nt->count; mu++) {
    if (nt_kind(nt, mu) != NK_CallNode) continue;
    const char *mun = nt_str(nt, mu, "name");
    if (!mun) continue;
    {
      if (!sp_str_mutator(mun, SP_MUT_IVAR)) continue;
    }
    int mrecv = nt_ref(nt, mu, "receiver");
    if (mrecv < 0 || nt_kind(nt, mrecv) != NK_CallNode) continue;
    if (nt_ref(nt, mrecv, "block") >= 0) continue;
    { int ma = nt_ref(nt, mrecv, "arguments"); int mac = 0;
      if (ma >= 0) nt_arr(nt, ma, "arguments", &mac);
      if (mac != 0) continue; }
    int rrecv = nt_ref(nt, mrecv, "receiver");
    if (rrecv < 0) continue;
    TyKind rrt = infer_type(c, rrecv);
    if (ty_is_object(rrt)) {
      char ivbuf4[300]; int defc4 = ty_object_class(rrt);
      const char *ivn4 = an_reader_ivar_of(c, mrecv, &defc4, ivbuf4, sizeof ivbuf4);
      if (!ivn4 || defc4 < 0) continue;
      if (strbuf_ivar_mut_kind(c, defc4, ivn4) < 0) continue;
      if (strbuf_promote_ivar(c, defc4, ivn4)) changed = 1;
      { int iv4 = comp_ivar_index(&c->classes[defc4], ivn4);
        if (iv4 < 0 || !c->classes[defc4].ivar_str_shared[iv4]) continue; }
      if (!c->strbuf_box[mrecv]) { c->strbuf_box[mrecv] = 1; changed = 1; }
    }
    else if (rrt == TY_POLY || rrt == TY_UNKNOWN) {
      /* boxed receiver (a container-read element): the runtime class is any
         instantiated class exposing the reader -- promote each candidate's
         backing ivar so the poly reader dispatch hands out handles */
      const char *rdn4 = nt_str(nt, mrecv, "name");
      int any4 = 0;
      for (int k4 = 0; rdn4 && k4 < c->nclasses; k4++) {
        int rdc4 = -1;
        if (!comp_reader_in_chain(c, k4, rdn4, &rdc4) || rdc4 < 0) continue;
        char ivb4[300];
        snprintf(ivb4, sizeof ivb4, "@%s", comp_resolve_alias(c, k4, rdn4));
        if (strbuf_ivar_mut_kind(c, rdc4, ivb4) < 0) continue;
        if (strbuf_promote_ivar(c, rdc4, ivb4)) changed = 1;
        { int iv4 = comp_ivar_index(&c->classes[rdc4], ivb4);
          if (iv4 < 0 || !c->classes[rdc4].ivar_str_shared[iv4]) continue; }
        any4 = 1;
      }
      if (any4 && !c->strbuf_box[mrecv]) { c->strbuf_box[mrecv] = 1; changed = 1; }
    }
  }

  /* Pure-alias pairs (`s2 = s1`): when either endpoint of the alias is
     in-place mutated, both share the one handle -- CRuby's mutable String
     objects -- regardless of which mutator (a bang-only alias set shares
     too). Non-literal writes are fine: the write emitter wraps them in
     sp_String_new, which inherits the source's frozen state. */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    /* the aliasing shapes: `s2 = s1` and the value-position append chain
       `s2 = (s1 << x)`, whose value IS the base object */
    int wv = an_strbuf_alias_source(c, nt_ref(nt, w, "value"));
    if (wv < 0) continue;
    const char *srcn = nt_str(nt, wv, "name");
    const char *tgtn = nt_str(nt, w, "name");
    Scope *ws = comp_scope_of(c, w);
    if (!srcn || !tgtn || !ws || sp_streq(srcn, tgtn)) continue;
    LocalVar *srcv = scope_local(ws, srcn);
    LocalVar *tgtv = scope_local(ws, tgtn);
    if (!srcv || !tgtv) continue;
    if (srcv->str_shared && tgtv->str_shared) continue;   /* settled */
    if (!strbuf_slot_eligible(c, srcn, ws, srcv) ||
        !strbuf_slot_eligible(c, tgtn, ws, tgtv)) continue;
    int ms = strbuf_mut_kind(c, srcn, ws);
    int mt = strbuf_mut_kind(c, tgtn, ws);
    if (ms < 0 || mt < 0) continue;         /* a disqualifying mutator */
    if (ms != 1 && mt != 1) continue;       /* alias set never mutated */
    if (srcv->type != TY_STRBUF || !srcv->str_shared)
      { srcv->type = TY_STRBUF; srcv->str_shared = 1; changed = 1; }
    if (tgtv->type != TY_STRBUF || !tgtv->str_shared)
      { tgtv->type = TY_STRBUF; tgtv->str_shared = 1; changed = 1; }
  }
  /* local <-> ivar alias pairs: `l = @s` / `@s = l`. When either side is
     in-place mutated, the ivar slot and the local share the handle. */
  for (int w = 0; w < nt->count; w++) {
    NodeKind wk2 = nt_kind(nt, w);
    int lval = -1, ivnode = -1;
    const char *lname = NULL, *ivname = NULL;
    if (wk2 == NK_LocalVariableWriteNode) {
      int wv2 = nt_ref(nt, w, "value");
      if (wv2 < 0 || nt_kind(nt, wv2) != NK_InstanceVariableReadNode) continue;
      lname = nt_str(nt, w, "name"); ivnode = wv2; ivname = nt_str(nt, wv2, "name");
      lval = w;
    }
    else if (wk2 == NK_InstanceVariableWriteNode) {
      int wv2 = nt_ref(nt, w, "value");
      if (wv2 < 0 || nt_kind(nt, wv2) != NK_LocalVariableReadNode) continue;
      ivname = nt_str(nt, w, "name"); ivnode = w;
      lname = nt_str(nt, wv2, "name"); lval = wv2;
    }
    else continue;
    if (!lname || !ivname) continue;
    Scope *ls = comp_scope_of(c, lval);
    LocalVar *llv = ls ? scope_local(ls, lname) : NULL;
    int icid = an_ivar_owner(c, ivnode);
    if (icid < 0 || !llv) continue;
    if (!strbuf_slot_eligible_shape(c, lname, ls, llv)) continue;
    if (llv->type != TY_UNKNOWN && llv->type != TY_STRING &&
        llv->type != TY_STRBUF && llv->type != TY_POLY) continue;
    int lmk = strbuf_mut_kind(c, lname, ls);
    int imk = strbuf_ivar_mut_kind(c, icid, ivname);
    if (lmk < 0 || imk < 0) continue;
    if (lmk != 1 && imk != 1) continue;
    int ch2 = strbuf_promote_ivar(c, icid, ivname);
    { ClassInfo *ci2 = &c->classes[icid];
      int iv2 = comp_ivar_index(ci2, ivname);
      if (iv2 < 0 || !ci2->ivar_str_shared[iv2]) continue; }
    if (ch2) changed = 1;
    if (llv->type != TY_STRBUF || !llv->str_shared)
      { llv->type = TY_STRBUF; llv->str_shared = 1; changed = 1; }
  }
  /* external reader alias: `x = obj.reader` where the reader exposes an
     in-place-mutated ivar -- x must hold the handle, so a later obj.bump
     stays visible through it (#3227 P5). The read node is marked so the
     reader emit hands out the handle instead of the safe copy. */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || nt_kind(nt, wv) != NK_CallNode) continue;
    if (nt_ref(nt, wv, "block") >= 0) continue;
    { int a2 = nt_ref(nt, wv, "arguments"); int an2 = 0;
      if (a2 >= 0) nt_arr(nt, a2, "arguments", &an2);
      if (an2 != 0) continue; }
    int rrecv = nt_ref(nt, wv, "receiver");
    if (rrecv < 0) continue;
    TyKind rrt = infer_type(c, rrecv);
    if (!ty_is_object(rrt)) continue;
    int rcid = ty_object_class(rrt);
    const char *mn = nt_str(nt, wv, "name");
    if (!mn) continue;
    char ivbuf[300]; int defc = rcid;
    const char *ivn = an_reader_ivar_of(c, wv, &defc, ivbuf, sizeof ivbuf);
    if (!ivn || defc < 0) continue;
    if (strbuf_ivar_mut_kind(c, defc, ivn) != 1) continue;
    const char *lname2 = nt_str(nt, w, "name");
    Scope *ls2 = comp_scope_of(c, w);
    LocalVar *llv2 = (lname2 && ls2) ? scope_local(ls2, lname2) : NULL;
    if (!llv2 || !strbuf_slot_eligible_shape(c, lname2, ls2, llv2)) continue;
    if (llv2->type != TY_UNKNOWN && llv2->type != TY_STRING &&
        llv2->type != TY_STRBUF && llv2->type != TY_POLY) continue;
    if (strbuf_promote_ivar(c, defc, ivn)) changed = 1;
    { int iv2 = comp_ivar_index(&c->classes[defc], ivn);
      if (iv2 < 0 || !c->classes[defc].ivar_str_shared[iv2]) continue; }
    if (!c->strbuf_box[wv]) { c->strbuf_box[wv] = 1; changed = 1; }
    if (llv2->type != TY_STRBUF || !llv2->str_shared)
      { llv2->type = TY_STRBUF; llv2->str_shared = 1; changed = 1; }
  }
  /* iteration-variable mutation: `arr.each { |x| x << "!" }` mutates the
     ELEMENT through the block binding, so the container's stored strings
     become handles and the block param binds the handle (#3227 P6). */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_CallNode) continue;
    const char *itn = nt_str(nt, w, "name");
    if (!itn || (!sp_streq(itn, "each") && !sp_streq(itn, "each_with_index")))
      continue;
    int blk4 = nt_ref(nt, w, "block");
    if (blk4 < 0) continue;
    int recv4 = nt_ref(nt, w, "receiver");
    if (recv4 < 0 || nt_kind(nt, recv4) != NK_LocalVariableReadNode) continue;
    const char *contn4 = nt_str(nt, recv4, "name");
    Scope *conts4 = contn4 ? comp_scope_of(c, recv4) : NULL;
    LocalVar *contv4 = (contn4 && conts4) ? scope_local(conts4, contn4) : NULL;
    if (!contv4 || (!ty_is_array(contv4->type) && contv4->type != TY_UNKNOWN))
      continue;
    const char *bp4 = block_param_name(c, blk4, 0);
    if (!bp4) continue;
    Scope *bs4 = comp_scope_of(c, blk4);
    LocalVar *bpv4 = bs4 ? scope_local(bs4, bp4) : NULL;
    if (!bpv4) continue;
    if (strbuf_mut_kind(c, bp4, bs4) != 1) continue;
    /* the container's elements must be strings: gate on the receiver's
       (settled or literal) element type so an array-of-arrays each+<<
       never promotes its param */
    if (contv4->type != TY_STR_ARRAY && contv4->type != TY_POLY_ARRAY) continue;
    if (contv4->type == TY_POLY_ARRAY) {
      /* mixed / not-provably-string elements: demand the string stores into
         handles and bind the param POLY -- the runtime mutator arms resolve
         `<<` per element kind (string append vs array push) (#3227) */
      changed |= strbuf_demand_container_stores(c, contn4, conts4);
      if (bpv4->type != TY_POLY) { bpv4->type = TY_POLY; changed = 1; }
      continue;
    }
    if (0) {
      /* only proceed when every literal store into the container is a
         string (a conservative element-type witness) */
      int all_str = 1, saw_lit = 0;
      for (int u2 = 0; u2 < nt->count && all_str; u2++) {
        if (nt_kind(nt, u2) != NK_LocalVariableWriteNode) continue;
        const char *un2 = nt_str(nt, u2, "name");
        if (!un2 || !sp_streq(un2, contn4) || comp_scope_of(c, u2) != conts4) continue;
        int uv2 = nt_ref(nt, u2, "value");
        if (uv2 < 0 || nt_kind(nt, uv2) != NK_ArrayNode) continue;
        int en2 = 0; const int *el2 = nt_arr(nt, uv2, "elements", &en2);
        for (int e2 = 0; e2 < en2; e2++) {
          saw_lit = 1;
          TyKind et2 = infer_type(c, el2[e2]);
          if (et2 != TY_STRING && et2 != TY_STRBUF) all_str = 0;
        }
      }
      if (!saw_lit || !all_str) continue;
    }
    /* accept (and correct) a str_array mis-guess of the param: a binding
       over string elements cannot itself be an array */
    if (bpv4->type != TY_UNKNOWN && bpv4->type != TY_STRING &&
        bpv4->type != TY_STRBUF && bpv4->type != TY_POLY &&
        bpv4->type != TY_STR_ARRAY) continue;
    changed |= strbuf_demand_container_stores(c, contn4, conts4);
    if (bpv4->type != TY_STRBUF || !bpv4->str_shared)
      { bpv4->type = TY_STRBUF; bpv4->str_shared = 1; changed = 1; }
  }
  /* deep-return alias: `r = make_held` where EVERY return path of the
     (receiverless, uniquely-named) callee yields a shared handle -- r joins
     the set and the call is marked so the emitter picks the handle off the
     side channel (#3227 P6). */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || nt_kind(nt, wv) != NK_CallNode) continue;
    if (c->strbuf_box[wv]) continue;
    if (nt_ref(nt, wv, "receiver") >= 0) continue;
    if (nt_ref(nt, wv, "block") >= 0) continue;
    const char *mn = nt_str(nt, wv, "name");
    int mi3 = mn ? an_unique_scope_by_name(c, mn) : -1;
    if (mi3 <= 0) continue;
    Scope *m3 = &c->scopes[mi3];
    /* every return tail (implicit + explicit) must be a shared slot read */
    int shared_ok = 1, saw_tail = 0;
    { int lastT = scope_body_last(c, mi3);
      if (lastT >= 0) {
        saw_tail = 1;
        if (!an_arg_is_shared_handle(c, lastT)) shared_ok = 0;
      } }
    for (int u = 0; shared_ok && u < nt->count; u++) {
      if (nt_kind(nt, u) != NK_ReturnNode) continue;
      if (comp_scope_of(c, u) != m3) continue;
      int ra = nt_ref(nt, u, "arguments");
      int rn2 = 0; const int *rv2 = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn2) : NULL;
      saw_tail = 1;
      if (rn2 != 1 || !an_arg_is_shared_handle(c, rv2[0])) shared_ok = 0;
    }
    const char *lname3 = nt_str(nt, w, "name");
    Scope *ls3 = comp_scope_of(c, w);
    if (saw_tail && !shared_ok && lname3 && ls3 &&
        strbuf_mut_kind(c, lname3, ls3) == 1) {
      /* the CALLER mutates the result (`rr = mk; rr << x`): demand the
         callee's returned locals into the shared set; the uniform gate then
         admits the site on a later iteration */
      int lastD = scope_body_last(c, mi3);
      int tails[33]; int ntails = 0;
      if (lastD >= 0 && ntails < 32) tails[ntails++] = lastD;
      for (int u = 0; u < nt->count && ntails < 32; u++) {
        if (nt_kind(nt, u) != NK_ReturnNode) continue;
        if (comp_scope_of(c, u) != m3) continue;
        int ra = nt_ref(nt, u, "arguments");
        int rn2 = 0; const int *rv2 = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn2) : NULL;
        if (rn2 == 1) tails[ntails++] = rv2[0];
      }
      for (int t2 = 0; t2 < ntails; t2++) {
        int tn2 = tails[t2];
        if (tn2 < 0 || nt_kind(nt, tn2) != NK_LocalVariableReadNode) continue;
        const char *tvn = nt_str(nt, tn2, "name");
        Scope *tvs = tvn ? comp_scope_of(c, tn2) : NULL;
        LocalVar *tlv = tvs ? scope_local(tvs, tvn) : NULL;
        if (!tlv || !strbuf_slot_eligible_shape(c, tvn, tvs, tlv)) continue;
        if (tlv->type != TY_UNKNOWN && tlv->type != TY_STRING &&
            tlv->type != TY_STRBUF && tlv->type != TY_POLY) continue;
        if (tlv->type != TY_STRBUF || !tlv->str_shared)
          { tlv->type = TY_STRBUF; tlv->str_shared = 1; changed = 1; }
      }
      continue;
    }
    if (!shared_ok || !saw_tail) continue;
    LocalVar *llv3 = (lname3 && ls3) ? scope_local(ls3, lname3) : NULL;
    if (!llv3 || !strbuf_slot_eligible_shape(c, lname3, ls3, llv3)) continue;
    if (llv3->type != TY_UNKNOWN && llv3->type != TY_STRING &&
        llv3->type != TY_STRBUF && llv3->type != TY_POLY) continue;
    c->strbuf_box[wv] = 1; changed = 1;
    if (llv3->type != TY_STRBUF || !llv3->str_shared)
      { llv3->type = TY_STRBUF; llv3->str_shared = 1; changed = 1; }
  }
  return changed;
}

static int convert_byref_handle_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* byref -> handle parameter conversion: a shared handle passed into a
     string-mutating (byref) parameter converts that parameter to the handle
     representation -- byref's const char** slot cannot carry the handle, so
     the mutation silently landed in a discarded copy. Once converted, every
     other call site's plain-local argument joins the shared set (so its own
     caller still observes the mutation, as with byref). */
  for (int mi2 = 1; mi2 < c->nscopes; mi2++) {
    Scope *m2 = &c->scopes[mi2];
    if (!m2->name || m2->nparams <= 0) continue;
    for (int pj = 0; pj < m2->nparams; pj++) {
      if (!m2->pnames[pj]) continue;
      LocalVar *pp = scope_local(m2, m2->pnames[pj]);
      if (!pp) continue;
      int is_handle = (pp->is_param && pp->type == TY_STRBUF && pp->str_shared);
      if (!pp->byref_out && !is_handle) continue;
      /* one pass over this method's call sites: detect a handle arg, and
         (once converted) pull plain-local args into the shared set */
      int saw_handle = is_handle;
      for (int u = 0; u < nt->count; u++) {
        if (nt_kind(nt, u) != NK_CallNode) continue;
        const char *un = nt_str(nt, u, "name");
        if (!un || !sp_streq(un, m2->name)) continue;
        if (an_unique_scope_by_name(c, un) != mi2) continue;
        int argsN = nt_ref(nt, u, "arguments");
        int uargc = 0;
        const int *uargv = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &uargc) : NULL;
        if (pj >= uargc) continue;
        if (an_arg_is_shared_handle(c, uargv[pj])) saw_handle = 1;
        /* an ALIASED plain-local argument also demands the handle: the
           callee's mutation must stay visible through the caller's alias
           (byref reassigns only the one slot) */
        else if (nt_kind(nt, uargv[pj]) == NK_LocalVariableReadNode) {
          const char *avn = nt_str(nt, uargv[pj], "name");
          Scope *avs = avn ? comp_scope_of(c, uargv[pj]) : NULL;
          LocalVar *alv0 = avs ? scope_local(avs, avn) : NULL;
          /* shape check WITHOUT the byref-arg clause: being this byref
             param's argument is exactly the situation we are converting */
          if (alv0 && !alv0->is_param && !alv0->is_cell &&
              (alv0->type == TY_STRING || alv0->type == TY_UNKNOWN ||
               alv0->type == TY_STRBUF) &&
              an_local_has_alias(c, avn, avs))
            saw_handle = 1;
        }
      }
      if (!saw_handle) continue;
      if (pp->byref_out) {
        pp->byref_out = 0;
        pp->is_cell = 0;
        pp->type = TY_STRBUF;
        pp->str_shared = 1;
        changed = 1;
      }
      /* pull the remaining plain-local args into the shared set */
      for (int u = 0; u < nt->count; u++) {
        if (nt_kind(nt, u) != NK_CallNode) continue;
        const char *un = nt_str(nt, u, "name");
        if (!un || !sp_streq(un, m2->name)) continue;
        if (an_unique_scope_by_name(c, un) != mi2) continue;
        int argsN = nt_ref(nt, u, "arguments");
        int uargc = 0;
        const int *uargv = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &uargc) : NULL;
        if (pj >= uargc) continue;
        int an2 = uargv[pj];
        if (nt_kind(nt, an2) == NK_LocalVariableReadNode) {
          const char *vn2 = nt_str(nt, an2, "name");
          Scope *vs2 = vn2 ? comp_scope_of(c, an2) : NULL;
          LocalVar *alv = vs2 ? scope_local(vs2, vn2) : NULL;
          if (!alv || !strbuf_slot_eligible_shape(c, vn2, vs2, alv)) continue;
          if (alv->type != TY_UNKNOWN && alv->type != TY_STRING &&
              alv->type != TY_STRBUF && alv->type != TY_POLY) continue;
          if (alv->type != TY_STRBUF || !alv->str_shared)
            { alv->type = TY_STRBUF; alv->str_shared = 1; changed = 1; }
        }
        else if (nt_kind(nt, an2) == NK_InstanceVariableReadNode) {
          const char *vn2 = nt_str(nt, an2, "name");
          int cid2 = vn2 ? an_ivar_owner(c, an2) : -1;
          if (cid2 >= 0 && strbuf_ivar_mut_kind(c, cid2, vn2) >= 0)
            if (strbuf_promote_ivar(c, cid2, vn2)) changed = 1;
        }
      }
    }
  }
  return changed;
}

/* True if `blk`'s subtree contains a YieldNode. */
static int subtree_has_yield_node(Compiler *c, int node, int depth) {
  const NodeTable *nt = c->nt;
  if (node < 0 || depth > 64) return 0;
  if (nt_kind(nt, node) == NK_YieldNode) return 1;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++)
    if (subtree_has_yield_node(c, nt_ref_at(nt, node, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_yield_node(c, ids[j], depth + 1)) return 1;
  }
  return 0;
}
/* A Thread.new / Fiber.new block in this method whose body yields. Such a body
   is emitted as its own function, so the yield-splice has no block to inline. */
static int scope_yields_inside_lifted_body(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi || nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "new")) continue;
    int r = nt_ref(nt, id, "receiver");
    const char *rn = (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode) ? nt_str(nt, r, "name") : NULL;
    if (!rn || (!sp_streq(rn, "Thread") && !sp_streq(rn, "Fiber"))) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && subtree_has_yield_node(c, blk, 0)) return 1;
  }
  return 0;
}

void analyze_program(Compiler *c) {
  comp_scope_index_set_frozen(0);  /* scope shape changes during the passes below */
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  /* `&(expr)`: the parentheses carry no meaning for a block argument, but they
     hide the expression from every shape check downstream (`&blk` as a proc
     local, a symbol, a literal). Strip them once here (#3054). */
  {
    NodeTable *ntm = (NodeTable *)c->nt;
    for (int id = 0; id < ntm->count; id++) {
      if (nt_kind(ntm, id) != NK_BlockArgumentNode) continue;
      int ex = nt_ref(ntm, id, "expression");
      if (ex < 0) continue;
      int un = unwrap_parens(c, ex);
      if (un >= 0 && un != ex) nt_node_set_ref(ntm, id, "expression", un);
    }
  }
  rename_shadowing_block_params(c);
  /* `:m.to_proc.call(r, a)` -> `r.m(a)`, before the to_proc rewrite below
     turns the receiver into a fixed-arity lambda (#3097). */
  desugar_sym_to_proc_call(c);
  desugar_enum_chain_shapes(c);          /* each_char.with_index / each.with_object */
  desugar_enum_chain_shapes(c);          /* 2nd pass: shapes the 1st created (e.g.
                                            map(&:sym.to_proc): to_proc->lambda first,
                                            then the lambda block attaches) */
  desugar_block_destructure_params(c);   /* |a,(b,c),d| -> flat param + `b,c = __destr` */
  desugar_enumerator_produce(c);         /* Enumerator.produce -> fiber generator */
  qualify_colliding_consts(c);
  qualify_colliding_classes(c);
  walk_scope(c, c->nt->root_id, 0, -1);
  register_singleton_defs(c);   /* def CONST.m / def x.m -> synthesized subclass */
  register_structs(c);
  desugar_struct_index_ctor(c);
  fix_struct_block_scopes(c);
  register_module_functions(c);
  register_locals(c);
  register_attrs(c);
  register_method_visibility(c);
  register_aliases(c);
  register_undefs(c);
  register_globals_consts(c);
  rewrite_const_alias_receivers(c);
  reject_env_value_uses(c);
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
      if (arms[rn].spec < 0) arms[rn].spec = bare_rescue_spec_cid(c, id);
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

  /* `iterator?` is the deprecated alias of `block_given?`; rename it up front
     (implicit/self receiver) so the block-aware marking and codegen below serve
     it identically (#3251). */
  for (int id = 0; id < c->nt->count; id++) {
    if (nt_kind(c->nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "iterator?")) continue;
    int r = nt_ref(c->nt, id, "receiver");
    const char *rty = r >= 0 ? nt_type(c->nt, r) : NULL;
    if (r < 0 || (rty && sp_streq(rty, "SelfNode")))
      nt_node_set_str((NodeTable *)c->nt, id, "name", "block_given?");
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

  /* Bare-Enumerable-via-#each: synthesize a per-class `__enum_to_a` helper that
     materializes #each into an array, used by desugar_enum_method_recv for bare
     Enumerable calls (`obj.map{}`). Done before the fixpoint so the helper body
     is typed, and before reachability so it is kept only when actually called.
     Explicit `to_enum`/`enum_for` no longer route here -- synth_to_enum_generators
     builds a real lazy Enumerator helper, retargeted by desugar_to_enum. */
  /* Reject the legacy `Struct::Name` constant path (the string-named Struct
     form), deliberately dropped -- see limitations.md and the matching check in
     register_struct_members (#3080). A bare `Struct.new("Foo", ...)` definition
     is compile-time only, so the `Struct::Foo` access is where the dropped form
     surfaces; reject it with a pointer to the modern `Name = Struct.new(...)`. */
  {
    const NodeTable *nt = c->nt;
    for (int id = 0; id < nt->count; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "ConstantPathNode")) continue;
      int par = nt_ref(nt, id, "parent");
      const char *pn = (par >= 0 && nt_type(nt, par) &&
                        sp_streq(nt_type(nt, par), "ConstantReadNode"))
                       ? nt_str(nt, par, "name") : NULL;
      if (pn && sp_streq(pn, "Struct")) {
        int ln = (int)nt_int(nt, id, "node_line", 0);
        const char *file = nt->source_file ? nt->source_file : "source.rb";
        fprintf(stderr, "spinel: %s:%d: the Struct::Name constant path is not supported "
                        "(legacy string-named Struct); use `Name = Struct.new(...)`\n", file, ln);
        exit(1);
      }
    }
  }

  synth_struct_each(c);
  synth_enum_to_a(c);
  synth_to_enum_generators(c);
  /* pre-fixpoint: an empty `{}` block-method receiver types as the STR_POLY
     hash so infer_block_params declares its |k, v| params (#2336). */
  mark_empty_hash_receivers(c);
  mark_empty_literal_tails(c);
  mark_empty_literal_args(c);
  /* after the arg/receiver marks: a bare `{}` with no other context takes its
     variant from the peer it is compared against (#3040) or from the key it is
     indexed with (#3028, #3029) */
  mark_empty_hash_cmp_peers(c);
  (void)mark_empty_hash_key_ctx(c);
  mark_empty_hash_const_writes(c);

  /* A bare identifier inside a class method that names a `class << self`
     attr reader is an implicit-self read of that singleton attribute; give it
     an explicit self receiver so it resolves like `self.reader` instead of
     failing as an undefined local/method (#3108). */
  {
    NodeTable *ntm = (NodeTable *)c->nt;
    int nend = ntm->count;
    for (int id = 0; id < nend; id++) {
      if (nt_kind(ntm, id) != NK_CallNode) continue;
      if (nt_ref(ntm, id, "receiver") >= 0) continue;
      int args = nt_ref(ntm, id, "arguments"); int ac = 0;
      if (args >= 0) nt_arr(ntm, args, "arguments", &ac);
      if (ac != 0 || nt_ref(ntm, id, "block") >= 0) continue;
      const char *nm = nt_str(ntm, id, "name");
      if (!nm) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || !s->is_cmethod || s->class_id < 0 || s->class_id >= c->nclasses) continue;
      if (!comp_is_sg_reader(&c->classes[s->class_id], nm)) continue;
      nt_node_set_ref(ntm, id, "receiver", nt_new_node(ntm, "SelfNode"));
    }
    if (ntm->count != nend) comp_grow_node_arrays(c);
  }

  /* A receiverless `instance_variable_get`/`_set`/`_defined?` inside an
     instance method operates on self; give it an explicit self receiver so it
     lowers through the object-receiver handler instead of failing as an
     unsupported bare call (#3059). The synthesized SelfNode must inherit the
     call's scope so infer_type resolves self to the instance (not top-level
     main), which the return-type fixpoint depends on. */
  {
    NodeTable *ntm = (NodeTable *)c->nt;
    int nend = ntm->count;
    int *self_ids = NULL, *self_scopes = NULL, npend = 0, cap = 0;
    for (int id = 0; id < nend; id++) {
      if (nt_kind(ntm, id) != NK_CallNode) continue;
      if (nt_ref(ntm, id, "receiver") >= 0) continue;
      const char *nm = nt_str(ntm, id, "name");
      if (!nm || (!sp_streq(nm, "instance_variable_get") &&
                  !sp_streq(nm, "instance_variable_set") &&
                  !sp_streq(nm, "instance_variable_defined?") &&
                  !sp_streq(nm, "remove_instance_variable"))) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->is_cmethod || s->class_id < 0 || s->class_id >= c->nclasses) continue;
      int sid = nt_new_node(ntm, "SelfNode");
      nt_node_set_ref(ntm, id, "receiver", sid);
      if (npend == cap) { cap = cap ? cap * 2 : 8;
        self_ids = realloc(self_ids, (size_t)cap * sizeof(int));
        self_scopes = realloc(self_scopes, (size_t)cap * sizeof(int)); }
      self_ids[npend] = sid; self_scopes[npend] = c->nscope[id]; npend++;
    }
    if (ntm->count != nend) comp_grow_node_arrays(c);
    for (int k = 0; k < npend; k++) c->nscope[self_ids[k]] = self_scopes[k];
    free(self_ids); free(self_scopes);
  }

  /* A receiverless call to a Struct/Data-inherited builtin (to_h, with, to_a,
     members, deconstruct, ...) inside an instance method of that Struct/Data
     class operates on self; give it an explicit self receiver so it lowers
     through the object-receiver struct emit instead of failing as an undefined
     method (to_h) or an unsupported bare call (with) (#3226). Skip names the
     class shadows with a member accessor or a user method -- those already
     resolve. The SelfNode inherits the call's scope so self infers as the
     instance (mirrors the instance_variable_get rewrite above). */
  {
    /* Data and Struct expose different inherited surfaces: `with` is Data-only,
       while to_a/values/size/length/dig/each_pair are Struct-only. Rewriting a
       name the class does not actually inherit would turn a correct NameError
       into a bad self-dispatch, so gate each list by the class kind. */
    static const char *const data_native[] = {
      "to_h", "with", "members", "deconstruct", "deconstruct_keys", NULL };
    static const char *const struct_native[] = {
      "to_h", "to_a", "values", "members", "size", "length",
      "dig", "deconstruct", "deconstruct_keys", "each_pair", NULL };
    NodeTable *ntm = (NodeTable *)c->nt;
    int nend = ntm->count;
    int *self_ids = NULL, *self_scopes = NULL, npend = 0, cap = 0;
    for (int id = 0; id < nend; id++) {
      if (nt_kind(ntm, id) != NK_CallNode) continue;
      if (nt_ref(ntm, id, "receiver") >= 0) continue;
      const char *nm = nt_str(ntm, id, "name");
      if (!nm) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->is_cmethod || s->class_id < 0 || s->class_id >= c->nclasses) continue;
      if (!c->classes[s->class_id].is_struct) continue;
      const char *const *native = c->classes[s->class_id].is_data ? data_native : struct_native;
      int is_native = 0;
      for (int t = 0; native[t]; t++) if (sp_streq(nm, native[t])) { is_native = 1; break; }
      if (!is_native) continue;
      /* a member accessor of that name beats the inherited builtin */
      char mn[272]; snprintf(mn, sizeof mn, "@%s", nm);
      if (comp_ivar_index(&c->classes[s->class_id], mn) >= 0) continue;
      /* a user method of that name is dispatched directly, not as self.builtin */
      if (comp_method_in_chain(c, s->class_id, nm, NULL) >= 0) continue;
      int sid = nt_new_node(ntm, "SelfNode");
      nt_node_set_ref(ntm, id, "receiver", sid);
      if (npend == cap) { cap = cap ? cap * 2 : 8;
        self_ids = realloc(self_ids, (size_t)cap * sizeof(int));
        self_scopes = realloc(self_scopes, (size_t)cap * sizeof(int)); }
      self_ids[npend] = sid; self_scopes[npend] = c->nscope[id]; npend++;
    }
    if (ntm->count != nend) comp_grow_node_arrays(c);
    for (int k = 0; k < npend; k++) c->nscope[self_ids[k]] = self_scopes[k];
    free(self_ids); free(self_scopes);
  }

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
        /* A Fiber/Enumerator/Thread `.new { }` block runs as an independent
           closure on its own fiber stack, and a `{ }` block lifted to a
           standalone proc (passed to a method that keeps a real &block)
           captures like a proc literal: a blk_param read inside either is a
           real capture-escape, so the method must keep a heap-materialized
           &blk (not be yield-inlined). */
        if (!a_proc_create_or_lifted(c, id)) continue;
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
      /* Nor if the method calls itself: inlining a self-recursive block-driving
         method cannot terminate (the depth-cap diagnostic), while the real
         (yields=0) function form -- &blk as a materialized sp_Proc * param, the
         self-call forwarding it -- compiles to plain C recursion and already
         works for the explicit-return shape above. */
      if (!has_ret && !scope_calls_itself(c, mi)) m->yields = 1;
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
    /* a def in value position evaluates to :name */
    else if (ty && sp_streq(ty, "DefNode")) {
      const char *dn = nt_str(c->nt, id, "name");
      if (dn) comp_sym_intern(c, dn);
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
  /* Proc#parameters reports param kinds (:req/:opt/:rest/:keyreq/:key/
     :keyrest/:block) and names as symbols; intern them now so they land in
     the table before the codegen prologue. Anonymous rest/kwrest/block use
     the CRuby placeholder names (:*, :**, :&); numbered params report
     :_1.._9. */
  for (int id = 0; id < c->nt->count; id++) {
    if (!is_proc_create(c, id)) continue;
    comp_sym_intern(c, "req");
    comp_sym_intern(c, "opt");
    int pn = a_proc_params_node(c, id);
    if (pn < 0) {
      for (int k = 1; k <= 9; k++) {
        char nbuf[4];
        snprintf(nbuf, sizeof nbuf, "_%d", k);
        comp_sym_intern(c, nbuf);
      }
      continue;
    }
    int rn = 0; const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
    for (int k = 0; k < rn; k++) { const char *nm = nt_str(c->nt, reqs[k], "name"); if (nm) comp_sym_intern(c, nm); }
    int on = 0; const int *opts = nt_arr(c->nt, pn, "optionals", &on);
    for (int k = 0; k < on; k++) { const char *nm = nt_str(c->nt, opts[k], "name"); if (nm) comp_sym_intern(c, nm); }
    int psn = 0; const int *posts = nt_arr(c->nt, pn, "posts", &psn);
    for (int k = 0; k < psn; k++) { const char *nm = nt_str(c->nt, posts[k], "name"); if (nm) comp_sym_intern(c, nm); }
    int rest = nt_ref(c->nt, pn, "rest");
    if (rest >= 0) {
      comp_sym_intern(c, "rest");
      const char *nm = nt_str(c->nt, rest, "name");
      comp_sym_intern(c, nm ? nm : "*");
    }
    int kn = 0; const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
    for (int k = 0; k < kn; k++) {
      const char *kt = nt_type(c->nt, kws[k]);
      comp_sym_intern(c, (kt && sp_streq(kt, "OptionalKeywordParameterNode")) ? "key" : "keyreq");
      const char *nm = nt_str(c->nt, kws[k], "name");
      if (nm) comp_sym_intern(c, nm);
    }
    int kwrest = nt_ref(c->nt, pn, "keyword_rest");
    if (kwrest >= 0) {
      comp_sym_intern(c, "keyrest");
      const char *nm = nt_str(c->nt, kwrest, "name");
      comp_sym_intern(c, nm ? nm : "**");
    }
    int bpar = nt_ref(c->nt, pn, "block");
    if (bpar >= 0) {
      comp_sym_intern(c, "block");
      const char *nm = nt_str(c->nt, bpar, "name");
      comp_sym_intern(c, nm ? nm : "&");
    }
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

  g_infer_optimistic = 1;
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
    ch |= expand_literal_splat_args(c);        /* builtin/proc m(*[a,b]) -> m(a, b) */
    ch |= pin_arg_position_hash_new(c);        /* f(Hash.new(d)) -> PolyPoly variant */
    ch |= pad_unsupplied_params(c);            /* under-supplied call: placeholder param type */
    ch |= desugar_builtin_method_obj(c);       /* builtin recv.method(:sym) -> wrapper def */
    ch |= desugar_include_math(c);             /* include Math: sqrt(x) -> Math.sqrt(x) */
    ch |= desugar_kernel_recv(c);              /* Kernel.puts x -> puts x */
    ch |= desugar_enum_method_recv(c);         /* obj.map{} -> obj.__enum_to_a.map{} */
    ch |= desugar_for_enumerable(c);           /* for x in obj -> for x in obj.__enum_to_a */
    ch |= desugar_to_enum(c);                  /* recv.to_enum(:m) -> generator/blockless */
    ch |= type_block_rest_params(c);           /* |*rest| locals are poly arrays */
    ch |= desugar_public_method(c);            /* recv.public_method(:m) -> recv.method(:m) */
    ch |= desugar_class_eval_value(c);
    ch |= desugar_instance_eval_builtin(c);    /* "s".instance_eval { m } -> splice on a temp */
    ch |= desugar_builtin_class_var_recv(c);   /* k = Array; k.new(..) -> Array.new(..) */
    ch |= desugar_compose_method_operand(c);   /* proc >> meth -> proc >> meth.to_proc */
    ch |= desugar_method_curry(c);             /* meth.curry -> meth.to_proc.curry */
    ch |= desugar_int_enum_with_index(c);      /* n.times.with_index -> n.times.each.with_index */
    ch |= widen_shared_cmp_params(c);          /* multi-class <=> takes its operand boxed */
    ch |= desugar_reduce_proc_arg(c);          /* reduce(&pr) -> reduce { |a,b| pr.call(a,b) } */
    ch |= desugar_block_capture_wrap(c);       /* { |i| ->{i} } -> { |i| (->(i){ ->{i} }).call(i) } */
    ch |= desugar_user_not_match(c);            /* a !~ b -> !(a =~ b) for a user =~ (#3019) */
    ch |= desugar_env_enum(c);                 /* ENV.keys/... -> __env_to_h.keys/... */
    ch |= desugar_dir_surface(c);              /* Dir.foreach{} -> entries.each{}, chdir{} splice, ... */           /* C.class_eval { v } -> (->(){v}).call */
    ch |= desugar_enumerable_chain(c);               /* x.chain(y) / enum+enum -> __enum_chain(x.to_a + y.to_a) */
    ch |= desugar_implicit_send(c);            /* send(:m, a) -> m(a) on self */
    ch |= desugar_public_send_recv(c);         /* r.public_send(:m, a) -> r.m(a), visibility-stamped */
    ch |= desugar_symbol_string_methods(c);    /* :sym.match(re) -> :sym.to_s.match(re) */
    /* re-run inside the fixpoint: a key whose type comes from a PARAMETER is
       still UNKNOWN on the pre-fixpoint pass, so `h[k] ||= []` fell back to
       the StrPolyHash default and handed an Integer key to a const char *
       (#3353). The mark is monotone, so repeating it only ever fills in. */
    ch |= mark_empty_hash_key_ctx(c);
    ch |= desugar_lazy_stateful_stage(c);      /* arr.lazy.uniq -> arr.uniq (finite source) */
    ch |= desugar_lazy_method_call(c);         /* lz.first where `def lz; ...lazy...; end` */
    ch |= desugar_str_range_methods(c);        /* ("a".."e").map -> .to_a.map */
    ch |= desugar_sym_to_proc_call(c);         /* :m.to_proc.call(r, a) -> r.m(a) */
    ch |= desugar_reduce_method_symbol(c);     /* reduce(:gcd) -> reduce { |a,x| a.gcd(x) } */
    ch |= desugar_symbol_var_block_arg(c);     /* m(&sym_var) -> m { |x| x.send(sym_var) } */
    ch |= desugar_kernel_method_block_arg(c);  /* m(&method(:Integer)) -> m { |x| Integer(x) } */
    ch |= desugar_hash_block_arg(c);           /* m(&hash) -> m { |x| hash[x] } */
    ch |= desugar_dynamic_send(c);             /* recv.send(var, a) -> static name dispatch */
    ch |= desugar_toplevel_instance_exec(c);   /* top-level instance_exec(&b) -> b.call */
    ch |= desugar_binding_lvget(c);            /* binding.local_variable_get(:x) -> x.itself */
    ch |= desugar_step_kwargs(c);              /* n.step(to: X, by: Y) -> n.step(X, Y) */
    ch |= desugar_respond_to_probe(c);         /* recv.respond_to?(:m) -> probe recv.m type */
    ch |= desugar_symbol_to_proc_call(c);      /* :sym.to_proc.call(x) -> x.sym */
    ch |= desugar_to_h_block(c);               /* recv.to_h{|e|[k,v]} -> recv.map{...}.to_h */
    ch |= desugar_to_proc_block_arg(c);        /* &obj (user to_proc) -> &(obj.to_proc) hoisted once */
    ch |= desugar_proc_expr_block_arg(c);      /* &(a >> b) -> hoisted temp */
    ch |= desugar_to_hash_splat(c);            /* f(**obj) -> f(**obj.to_hash) */
    ch |= desugar_value_callable_forwards(c);  /* &proc -> { |x| proc.call(x) } */
    ch |= infer_block_params(c);
    ch |= infer_for_index(c);
    ch |= infer_catch_block_params(c);
    /* Resolve constant types before ivar inference: a destructured constant
       (`CLK_1,.. = (1..8).map{...}`) is transiently poly in infer_write_types
       before its array element type settles, and a monotonic ivar that reads
       it (`@clk += CLK_1`) would lock onto that poly. Resolving constants
       first feeds the settled type into ivar inference. */
    ch |= infer_global_const_types(c);
    ch |= infer_multiwrite_const_types(c);
    ch |= promote_shared_stored_strings(c);
    ch |= infer_ivar_types(c);
    ch |= infer_cvar_types(c);
    ch |= infer_inherited_ivars(c);
    ch |= infer_return_types(c);
    ch |= backprop_hash_return_types(c);
    if (!ch) {
      /* Converged with the ambiguous guesses held at bottom. Clear the flag
         and keep going: a slot whose evidence never arrived is genuinely
         untypable, so it now takes the pessimistic type and the slots it
         feeds settle on that. Everything resolvable has already settled
         concretely, so this second stage widens only what really is open. */
      if (g_infer_optimistic) { g_infer_optimistic = 0; continue; }
      /* Converged: one backstop bind pass lets an empty array-literal arg
         fill a still-UNKNOWN parameter as an (empty) poly array. If it fills
         anything, keep iterating so dependent return types resolve. */
      static int backstop_ran;
      if (!backstop_ran) {
        backstop_ran = 1;
        g_final_bind_pass = 1;
        ch = infer_param_types(c);
        g_final_bind_pass = 0;
      }
      if (!ch) break;
    }
  }
  g_infer_optimistic = 0;

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
        if (c->classes[ci].ivar_types[iv] == TY_POLY &&
            (!c->classes[ci].ivars[iv] ||
             !class_ivar_pinned(&c->classes[ci], c->classes[ci].ivars[iv]))) {
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
        ch |= infer_catch_block_params(c);
        ch |= infer_global_const_types(c);
        ch |= infer_multiwrite_const_types(c);
        ch |= infer_ivar_types(c);
        /* AFTER the ivar write-merge: the re-clear zeroes recorded ivars at
           each iteration's top, so a promote gated on STRING must see the
           freshly re-derived type, not the cleared UNKNOWN (#3227 P4) */
        ch |= promote_shared_stored_strings(c);
        ch |= widen_shared_cmp_params(c);
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
  /* Backstop: a local assigned only empty hash literals `{}` that no key-write
     ever narrowed stays TY_UNKNOWN, so its hash block methods (select/reject/
     each/...) can't dispatch. Default it to the bare-{} hash type. Runs
     post-fixpoint, so a key-write that narrowed it to a concrete variant
     (SYM_POLY / STR_INT / ...) already set lv->type and is kept (#2336). */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "LocalVariableWriteNode")) continue;
    int v = nt_ref(c->nt, id, "value");
    const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (!vty || !sp_streq(vty, "HashNode")) continue;
    int en = 0; nt_arr(c->nt, v, "elements", &en);
    if (en != 0) continue;
    const char *nm = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    if (lv && !lv->rbs_seeded && lv->type == TY_UNKNOWN &&
        local_all_writes_empty_hash(c, s, nm)) {
      /* a use context on this write's literal (an indexing key, a compared
         peer) picks the variant; otherwise the local's own `[]=` keys do.
         The StrPolyHash default is only safe for a String (or absent) key:
         codegen reads a StrPolyHash key out of the boxed value as `.v.s`, so
         defaulting an Integer- or poly-keyed hash to it hands a boxed int to
         a `const char *` slot and segfaults. The key context pass could not
         see this key -- it runs during the fixpoint, and a key that is a
         block's return value has no type yet -- but here, post-fixpoint, it
         does (#3397). */
      TyKind want = (c->empty_hash_want && v < c->node_cap) ? c->empty_hash_want[v] : TY_UNKNOWN;
      if (!ty_is_hash(want)) {
        int nkw = 0;
        TyKind kt = local_aset_key_type(c, s, nm, &nkw);
        want = (kt == TY_SYMBOL) ? TY_SYM_POLY_HASH
             : (kt == TY_STRING || nkw == 0) ? TY_STR_POLY_HASH
             : TY_POLY_POLY_HASH;
      }
      lv->type = want;
    }
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
    for (int ri = 0; ri < cl->nreaders; ri++) {
      const char *rname = cl->readers[ri];
      if (!rname) continue;
      char ivname[300]; snprintf(ivname, sizeof ivname, "@%s", rname);
      /* skip only when THIS ivar is assigned in a ctor; an existing ctor that
         does not touch it still leaves it nil (#3136) */
      if (ctor_writes_ivar(c, ci, ivname)) continue;
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
  /* `if v.is_a?(K)` occurrence typing: reads of a poly v inside the guard's
     then-arm narrow to K's concrete type (post-fixpoint, same read-site unbox
     as the nil guards above). */
  narrow_isa_guards(c);
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
            if (ix >= 0 && !class_ivar_pinned(&c->classes[ci2], ivn) &&
                (c->classes[ci2].ivar_types[ix] == TY_STR_POLY_HASH ||
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
    /* A Struct's synthesized each returns self (Struct#each semantics), so keep
       its object return. A hand-written each's value is whatever its body tail
       evaluates to -- `self` for the `...; self` idiom, but a bare-yield each
       (`yield 1; yield 2`) ends in the block's return value (poly), and
       `def each(&b) = xs.each(&b)` ends in the inner array. Pinning those to the
       class type truncated the poly tail to an object pointer; type the return
       poly so any tail coerces (the __enum_to_a driver discards it either way). */
    int is_struct = m->class_id >= 0 && c->classes[m->class_id].is_struct;
    m->ret = is_struct ? ty_object(m->class_id) : TY_POLY;
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
     themselves recursively. Their implicit block is forwarded as a proc
     parameter -- the declared &block name when the def has one, else a
     synthetic __yblk__ -- so the method is emitted (yields=0) and each
     `yield` in its body calls sp_proc_call(<blk>, ...). A declared &block
     that only does blk.call/&blk-forward never reaches here (the
     blk_param pass above already kept it yields=0 when self-recursive);
     this loop covers bodies with literal `yield`s. */
  int lowered_recursive_yield = 0;
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->name || !m->reachable) continue;
    /* An anonymous `&` (blk_param == "") has no name to reference from the
       lowered body's yield sites; it keeps the inline path's diagnostic. */
    if (m->blk_param && !m->blk_param[0]) continue;
    if (!m->yields) continue;
    if (m->body < 0) continue;
    int has_yld = 0;
    for (int id = 0; id < c->nt->count && !has_yld; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(c->nt, id);
      if (ty && sp_streq(ty, "YieldNode")) has_yld = 1;
    }
    if (!has_yld) continue;
    /* A yield inside a Thread/Fiber body cannot be spliced: that body is
       lifted to its own C function, where the caller's block is not in scope,
       so the splice emitted a LocalJumpError raise and the surrounding
       expression was then ill-typed. Lowering forwards the block as a proc the
       lifted body can call (#3355). */
    int thread_yld = scope_yields_inside_lifted_body(c, mi);
    if (!scope_calls_itself(c, mi) && !thread_yld) continue;
    m->is_lowered_yield = 1;
    m->lowered_lifted_yield = thread_yld;
    m->yields = 0;
    if (!thread_yld) m->ret = TY_INT;
    if (!m->blk_param) m->blk_param = strdup("__yblk__");
    LocalVar *yblk = scope_local_intern(m, m->blk_param);
    if (yblk) {
      yblk->type = TY_PROC;
      yblk->is_param = 1;
      yblk->is_cell = 1;
    }
    lowered_recursive_yield = 1;
  }

  /* A method lowered just above (yields=1 -> a real &block-forwarding function)
     turns any literal block passed to it into a lifted first-class proc. That
     classification (a_block_is_lifted) reads m->yields, so those blocks were
     still "unlifted" when mark_proc_captures ran earlier -- their captures of
     enclosing locals never got heap cells (#3166). Re-run the capture pass now
     that the yields flags are final; it only sets is_cell and is idempotent. */
  if (lowered_recursive_yield) mark_proc_captures(c);

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
  /* Collect the names referenced by `method(:sym)` nodes once (they are
     rare), instead of rescanning every node per scope: the old shape was
     O(scopes * nodes) and dominated the post-fixpoint tail on large trees. */
  const char **msym_names = NULL;
  int msym_n = 0, msym_cap = 0;
  for (int id = 0; id < c->nt->count; id++) {
    const char *nty = nt_type(c->nt, id);
    if (!nty || !sp_streq(nty, "CallNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || !sp_streq(nm, "method")) continue;
    const char *msym = method_sym_arg(c, id);
    if (!msym) continue;
    if (msym_n >= msym_cap) {
      msym_cap = msym_cap ? msym_cap * 2 : 16;
      msym_names = realloc(msym_names, sizeof(*msym_names) * (size_t)msym_cap);
      if (!msym_names) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    msym_names[msym_n++] = msym;
  }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || !sc->name) continue;
    int taken = 0;
    for (int i = 0; i < msym_n && !taken; i++)
      if (sp_streq(msym_names[i], sc->name)) taken = 1;
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
  free(msym_names);


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

  /* Block splat params reach only the lowerings that bind them; the rest
     reject loudly here rather than emitting nil-bound or misdeclared bodies. */
  check_block_rest_support(c);

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

  /* Widen an ivar shared with a push-widened helper param -- after the write
     re-run above (which would re-narrow it from its empty-`[]` write) so the
     poly widening sticks, then re-derive return types only so a manual reader
     (`def flags; @flags; end`) reports the now-poly array (#3154). */
  widen_ivars_from_pushed_params(c);
  g_ret_no_new_poly = 1;
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;
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

  /* A method whose body tail is one of the locals just declared poly still
     carries an UNKNOWN return, which emits a `void` C function -- the value
     is then dropped at every call site (#3337: `def txn; r = yield; r; end`
     called with a block whose own tail is a local read; `r` only settles in
     the backstop above, after the last return pass). Lift ONLY returns that
     are still UNKNOWN: widening an established return here is what
     g_ret_no_new_poly deliberately prevents. Iterated so a caller whose tail
     is such a call picks the type up too. */
  for (int iter = 0; iter < 8; iter++) {
    int lifted = 0;
    for (int s = 0; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      if (sc->ret != TY_UNKNOWN || sc->body < 0) continue;
      TyKind rl = infer_type(c, sc->body);
      if (rl != TY_UNKNOWN && rl != TY_VOID) { sc->ret = rl; lifted = 1; }
    }
    if (!lifted) break;
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
    /* An ivar that widens here (e.g. `@query_log`, whose heterogeneous `= []` /
       `.push(str)` / `= prev` writes merge to poly) must carry its new type into
       any local that merely READS it (`prev = @query_log`). Widen such a local
       to the ivar's type -- monotonically, without the full local re-derivation
       infer_write_types does (which would reset pattern/massign/block-bound
       locals this late and mistype them). Otherwise the local is stranded at its
       pre-widen scalar type: an unsound `sp_StrArray *` <- `sp_RbVal` at
       `local = @ivar` (#1793). */
    ch |= reconcile_locals_reading_ivars(c);
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

  mark_empty_array_receivers(c);
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

  /* Byref string out-params: must run before the STRBUF promotion below so
     the promotion can exclude locals whose address is passed to a byref slot
     (a STRBUF local is an sp_String*, not the const char* slot byref needs). */
  compute_byref_out_params(c);
  /* handle args cannot ride byref's const char** slot: convert such params
     to the handle representation, cascading through transitive passes */
  while (convert_byref_handle_params(c)) {}

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
    /* Exclude vars passed into a byref out-param slot: the call site takes
       the local's address as const char**, which a promoted sp_String* slot
       cannot provide. */
    int passed_byref = 0;
    for (int u = 0; u < c->nt->count && !passed_byref; u++) {
      const char *uty = nt_type(c->nt, u);
      if (!uty || !sp_streq(uty, "CallNode")) continue;
      if (comp_scope_of(c, u) != s) continue;
      int mi = an_unique_scope_by_name(c, nt_str(c->nt, u, "name"));
      if (mi < 0) continue;
      int argsN = nt_ref(c->nt, u, "arguments");
      int uargc = 0;
      const int *uargv = argsN >= 0 ? nt_arr(c->nt, argsN, "arguments", &uargc) : NULL;
      for (int j = 0; j < uargc && j < c->scopes[mi].nparams; j++) {
        if (!comp_byref_param(c, &c->scopes[mi], j)) continue;
        const char *aty = nt_type(c->nt, uargv[j]);
        const char *an2 = aty && sp_streq(aty, "LocalVariableReadNode") ? nt_str(c->nt, uargv[j], "name") : NULL;
        if (an2 && sp_streq(an2, vn)) { passed_byref = 1; break; }
      }
    }
    if (passed_byref) continue;
    lv->type = TY_STRBUF;
  }

  /* Shared-mutable strings (#3227): re-assert the phase-3 promotion. The
     fixpoint pass (promote_shared_stored_strings) sets str_shared and the
     strbuf_box read marks, but a later assign-based inference pass can
     overwrite the LOCAL's type back to TY_STRING from its literal writes.
     The marks (and so the container typing) are durable; restore the slot
     type here so the local declares as the sp_String handle. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nlocals; i++) {
      LocalVar *lv = &sc->locals[i];
      if (lv->str_shared &&
          (lv->type == TY_STRING || lv->type == TY_STR_ARRAY)) lv->type = TY_STRBUF;
    }
  }
  for (int ci2 = 0; ci2 < c->nclasses; ci2++) {
    ClassInfo *ci = &c->classes[ci2];
    for (int iv = 0; iv < ci->nivars; iv++)
      if (ci->ivar_str_shared[iv] && ci->ivar_types[iv] == TY_STRING)
        ci->ivar_types[iv] = TY_STRBUF;
  }


  /* The local read a string-aliasing write bottoms out at: a bare local
     read, or a value-position `<<`/`concat` chain over one -- the chain's
     value IS its base object (`q = (v << x)` makes q an alias of v, and a
     later `v << y` is visible through q). Parens unwrap. -1 if neither. */
  /* (helper defined just below as a nested loop-free walk) */
  /* Shared-mutable strings (#3227): a string local that is a pure ALIAS of an
     in-place-mutated string (`s2 = s1` where s1 is now TY_STRBUF) must share
     s1's one sp_String* handle, so a later `s1 << x` is visible through s2 and
     `s1.equal?(s2)` is true (CRuby's mutable-String-object semantics). Promote
     the alias to TY_STRBUF and flag the whole set str_shared, which flips reads
     from copy-on-read to the live buffer and assignment to a handle copy.
     Conservative (Phase 1): the alias's every write must be `= <a TY_STRBUF
     local>`; other shapes keep today's value-copy behavior. Runs after the
     TY_STRBUF promotion above so the source's type is settled. */
  for (int w = 0; w < c->nt->count; w++) {
    const char *wty = nt_type(c->nt, w);
    if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
    int wv = an_strbuf_alias_source(c, nt_ref(c->nt, w, "value"));
    if (wv < 0) continue;
    const char *srcn = nt_str(c->nt, wv, "name");
    Scope *ws = comp_scope_of(c, w);
    LocalVar *src = (srcn && ws) ? scope_local(ws, srcn) : NULL;
    if (!src || src->type != TY_STRBUF) continue;   /* aliases an in-place-mutated string */
    const char *tgtn = nt_str(c->nt, w, "name");
    LocalVar *tgt = tgtn ? scope_local(ws, tgtn) : NULL;
    if (!tgt || tgt == src || tgt->is_param || tgt->is_cell || tgt->rbs_seeded) continue;
    if (tgt->type != TY_STRING && tgt->type != TY_STRBUF) continue;
    /* every write to the alias must itself be an aliasing shape */
    int pure_alias = 1;
    for (int u = 0; u < c->nt->count; u++) {
      const char *uty = nt_type(c->nt, u);
      if (!uty || !sp_streq(uty, "LocalVariableWriteNode")) continue;
      const char *un = nt_str(c->nt, u, "name");
      if (!un || !sp_streq(un, tgtn) || comp_scope_of(c, u) != ws) continue;
      if (an_strbuf_alias_source(c, nt_ref(c->nt, u, "value")) < 0) { pure_alias = 0; break; }
    }
    if (!pure_alias) continue;
    src->str_shared = 1;
    tgt->type = TY_STRBUF;
    tgt->str_shared = 1;
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
      /* an in-place-mutated string ivar (`@s << x` in a method) needs the
         mutation visible through every reference; a by-value struct copy
         would swallow it (#3227 P4) */
      if (t == TY_STRING && strbuf_ivar_mut_kind(c, i, ci->ivars[j]) != 0) { scalar = 0; break; }
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
    /* freeze/frozen? reaching a class's instances needs the GC-header frozen
       bit, which a by-value struct doesn't have: force heap representation
       and mark the class so codegen guards its ivar stores. A poly-receiver
       freeze can hit any boxable instance, so it marks every class. */
    if (sp_streq(ty, "CallNode")) {
      const char *fzn = nt_str(c->nt, id, "name");
      if (fzn && (sp_streq(fzn, "freeze") || sp_streq(fzn, "frozen?"))) {
        int frecv = nt_ref(c->nt, id, "receiver");
        int q = -1;
        if (frecv >= 0) {
          TyKind frt = comp_ntype(c, frecv);
          if (ty_is_object(frt)) q = ty_object_class(frt);
          else if (frt == TY_POLY && sp_streq(fzn, "freeze"))
            /* a poly-receiver freeze can hit any class's instance, so every
               class must carry the GC-header frozen bit -- disqualify them all
               from the by-value layout too, matching the single-class path */
            for (int q2 = 0; q2 < c->nclasses; q2++) {
              c->classes[q2].is_value_type = 0;
              c->classes[q2].freeze_observed = 1;
            }
        }
        else {
          Scope *fs = comp_scope_of(c, id);
          if (fs && fs->class_id >= 0 && !fs->is_cmethod) q = fs->class_id;
        }
        if (q >= 0 && q < c->nclasses) {
          c->classes[q].is_value_type = 0;
          c->classes[q].freeze_observed = 1;
        }
      }
    }
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
    /* `instance_variable_set` on self mutates the receiver just like `@x = v`;
       a by-value class would take the write on a copy and silently drop it. */
    if (sp_streq(ty, "CallNode")) {
      const char *ivsn = nt_str(c->nt, id, "name");
      if (ivsn && sp_streq(ivsn, "instance_variable_set")) {
        int ivr = nt_ref(c->nt, id, "receiver");
        const char *ivrt = ivr >= 0 ? nt_type(c->nt, ivr) : NULL;
        if (ivr < 0 || (ivrt && sp_streq(ivrt, "SelfNode"))) {
          Scope *s = comp_scope_of(c, id);
          if (s && s->class_id >= 0 && s->class_id < c->nclasses &&
              (!s->name || !sp_streq(s->name, "initialize")))
            c->classes[s->class_id].is_value_type = 0;
        }
        else {
          TyKind ivrty = comp_ntype(c, ivr);
          if (ty_is_object(ivrty)) {
            int q = ty_object_class(ivrty);
            if (q >= 0 && q < c->nclasses) c->classes[q].is_value_type = 0;
          }
        }
      }
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

