#include <stdint.h>
#include "analyze_internal.h"


static int narrow_int_table_ivars(Compiler *c);  /* declared early: the fixpoint calls it */

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
int g_infer_write_round = 0;
int g_fixpoint_rounds = 0;

/* Defined in codegen.c (linked into the same binary). Used to specialize a
   `rescue <UserExc> => e` binding to the exception subclass's object type. */
int class_is_exc_subclass(Compiler *c, int ci);

/* True when the class body defines an instance method of its own (an exception
   subclass with behaviour, not just a name). */
static int class_defines_own_method(Compiler *c, int ci) {
  for (int s = 1; s < c->nscopes; s++)
    if (c->scopes[s].class_id == ci && !c->scopes[s].is_cmethod && c->scopes[s].name)
      return 1;
  return 0;
}

/* The class index a rescue arm specializes its bound variable to: exactly one
   named user exception subclass that carries state or behaviour of its own --
   ivars (#1415) or methods (#3707) -- else -1. */
static int rescue_arm_spec_cid(Compiler *c, int rescue_id) {
  int nexc = 0;
  const int *exc = nt_arr(c->nt, rescue_id, "exceptions", &nexc);
  if (nexc != 1) return -1;
  const char *en = nt_type(c->nt, exc[0]);
  if (!en || (!sp_streq(en, "ConstantReadNode") && !sp_streq(en, "ConstantPathNode"))) return -1;
  const char *enm = nt_str(c->nt, exc[0], "name");
  int xc = enm ? comp_class_index(c, enm) : -1;
  if (xc >= 0 && class_is_exc_subclass(c, xc) &&
      (c->classes[xc].nivars > 0 || class_defines_own_method(c, xc))) return xc;
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

/* Names the emitted program can reach with no CallNode of its own: the runtime
   protocols. `5 + money` calls Money#coerce, `puts obj` calls to_s, a `for`
   loop calls each. Reachability keeps such a method alive; a pass reasoning
   about how a method is entered or what its C signature may be must likewise
   treat it as having a caller it cannot see. */
static int method_name_implicitly_invoked(const char *nm) {
  static const char *const implicit[] = {
    "to_s", "inspect", "==", "<=>", "eql?", "hash", "each", "coerce",
    "to_str", "to_ary", "to_a", "to_i", "to_int", "to_h", "to_hash", "to_proc", "call",
    "to_path",   /* File, Dir and IO's path slots ask it first (rb_get_path) */
    "to_io",     /* IO.select waits on whatever answers it; the only caller is
                    the generated dispatch behind sp_user_to_io_hook, so nothing
                    in the program names it (#4105-adjacent, the TLS socket
                    shape) */
    "initialize_copy",
    /* the materializer synthesized for a class that includes Enumerable: the
       generated sp_obj_to_a dispatch calls it for an instance the poly
       machinery meets, and nothing in the program names it (#3761) */
    "__enum_to_a", NULL };
  if (!nm) return 0;
  for (int i = 0; implicit[i]; i++) if (sp_streq(implicit[i], nm)) return 1;
  return 0;
}

/* Can this fold seed only ever be a BUILTIN? A literal, or an expression whose
   value a literal decides, can never reach a user class's `+`, so a seeded
   `sum` written with one must not keep every `+` in the program alive. Asked
   of the Prism node because compute_reachable runs before inference. */
static int an_seed_is_builtin(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  switch (nt_kind(nt, id)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_StringNode:
    case NK_InterpolatedStringNode: case NK_SymbolNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    case NK_ArrayNode: case NK_HashNode: case NK_RangeNode:
    case NK_RationalNode: case NK_ImaginaryNode:
      return 1;
    case NK_CallNode: {
      const char *nm = nt_str(nt, id, "name");
      int rcv = nt_ref(nt, id, "receiver");
      if (!nm) return 0;
      /* the Kernel constructors, which name a builtin class and answer one */
      if (rcv < 0 &&
          (sp_streq(nm, "Rational") || sp_streq(nm, "Complex") ||
           sp_streq(nm, "Float") || sp_streq(nm, "Integer") ||
           sp_streq(nm, "String") || sp_streq(nm, "Array")))
        return 1;
      /* arithmetic ON a literal is still a literal's class: `10**30`, `-1`.
         An operator name is the one that does not start like an identifier. */
      if (rcv >= 0 && nm[0] &&
          !((nm[0] >= 'a' && nm[0] <= 'z') || (nm[0] >= 'A' && nm[0] <= 'Z') || nm[0] == '_'))
        return an_seed_is_builtin(nt, rcv);
      return 0;
    }
    default: return 0;
  }
}

/* A hashed name set (ANameHash). Passes that asked "is this name among
   those" with a linear scan, once per node or per mark, paid (asks x names)
   every round (rubys/roundhouse#72); compute_reachable's called-name set is
   the first user. */
static unsigned cr_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}
int anh_find(const ANameHash *st, const char *nm) {
  if (!st->nb) return -1;
  for (int i = st->head[cr_hash(nm) % (unsigned)st->nb]; i >= 0; i = st->next[i])
    if (sp_streq(st->key[i], nm)) return i;   /* the most recent add of it */
  return -1;
}
int anh_has(const ANameHash *st, const char *nm) { return anh_find(st, nm) >= 0; }
void anh_add(ANameHash *st, const char *nm) {
  if (!st->nb) {
    st->nb = 4096;
    st->head = malloc(sizeof(int) * (size_t)st->nb);
    for (int b = 0; b < st->nb; b++) st->head[b] = -1;
  }
  if (st->n == st->cap) {
    st->cap = st->cap ? st->cap * 2 : 64;
    st->key = realloc(st->key, sizeof(char *) * (size_t)st->cap);
    st->next = realloc(st->next, sizeof(int) * (size_t)st->cap);
  }
  unsigned b = cr_hash(nm) % (unsigned)st->nb;
  st->key[st->n] = nm; st->next[st->n] = st->head[b]; st->head[b] = st->n; st->n++;
}
void anh_free(ANameHash *st) { free(st->key); free(st->next); free(st->head); }

void compute_reachable(Compiler *c) {
  /* Build per-scope call sets (CallNode names, not entering nested DefNodes). */
  char ***scope_calls = calloc((size_t)c->nscopes, sizeof(char **));
  int   *sc_n        = calloc((size_t)c->nscopes, sizeof(int));
  int   *sc_cap      = calloc((size_t)c->nscopes, sizeof(int));
  for (int s = 0; s < c->nscopes; s++) {
    if (c->scopes[s].body >= 0)
      cr_collect_calls(c, c->nt, c->scopes[s].body, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    /* Also scan parameter defaults (e.g. def foo(opt = bar)) -- these emit calls
       within the method scope but live in the DefNode parameters subtree. */
    if (c->scopes[s].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[s].def_node, "parameters");
      if (pn >= 0)
        cr_collect_calls(c, c->nt, pn, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    }
  }


  /* BFS queue (scope indices). */
  int *queue = malloc((size_t)c->nscopes * sizeof(int));
  int qhead = 0, qtail = 0;

  /* --ext-entry designations join the graph as roots: an entry has no
     compiled call site, so without this it would be unreachable and DCE'd
     out of the very library it is the point of (ext-design.md, Layer 1). */
  if (g_ext_entries && *g_ext_entries) {
    char list[2048];
    snprintf(list, sizeof list, "%s", g_ext_entries);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
      char *dot = strrchr(tok, '.');
      if (!dot) {
        fprintf(stderr, "spinel: --ext-entry `%s`: spell it Module.method "
                        "(module singleton methods export in v1)\n", tok);
        exit(1);
      }
      *dot = '\0';
      int ci2 = comp_class_index(c, tok);
      int mi2 = ci2 >= 0 ? comp_cmethod_in_chain(c, ci2, dot + 1, NULL) : -1;
      if (mi2 < 0) {
        fprintf(stderr, "spinel: --ext-entry `%s.%s` does not name a "
                        "`def self.%s` in module %s\n",
                tok, dot + 1, dot + 1, tok);
        exit(1);
      }
      c->scopes[mi2].is_ext_entry = 1;
    }
  }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    sc->reachable = 0;
    int is_root = (s == 0 || !sc->name || sp_streq(sc->name, "initialize") ||
                   method_name_implicitly_invoked(sc->name) || sc->is_ext_entry);
    if (is_root) { sc->reachable = 1; queue[qtail++] = s; }
  }

  /* "called_names" tracks every method name reached from any reachable scope.
     Used by alias and prep_to propagation (aliases have no scope of their own). */
  /* Both name lookups below were linear scans, one per mark: (calls x names)
     for the called set, (calls x scopes) for a name's scopes. */
  char **called_names = NULL; int cn_n = 0, cn_cap = 0;
  ANameHash cn_set; memset(&cn_set, 0, sizeof cn_set);
  /* name -> its scopes, ascending: scope names are fixed for this pass */
  ANameHash sn_set; memset(&sn_set, 0, sizeof sn_set);
  int *sn_first = NULL, *sn_link = malloc(sizeof(int) * (size_t)(c->nscopes + 1));
  for (int t = 0; t < c->nscopes; t++) {
    sn_link[t] = -1;
    const char *tn = c->scopes[t].name;
    if (!tn) continue;
    int k = -1;
    if (sn_set.nb)
      for (int i = sn_set.head[cr_hash(tn) % (unsigned)sn_set.nb]; i >= 0; i = sn_set.next[i])
        if (sp_streq(sn_set.key[i], tn)) { k = i; break; }
    if (k < 0) {
      anh_add(&sn_set, tn); k = sn_set.n - 1;
      sn_first = realloc(sn_first, sizeof(int) * (size_t)sn_set.cap);
      sn_first[k] = t;
    }
    else {
      int last = sn_first[k];
      while (sn_link[last] >= 0) last = sn_link[last];
      sn_link[last] = t;
    }
  }
  #define SN_FIRST(NM) ({ const char *_q = (NM); int _r = -1; \
    if (sn_set.nb) for (int _i = sn_set.head[cr_hash(_q) % (unsigned)sn_set.nb]; _i >= 0; _i = sn_set.next[_i]) \
      if (sp_streq(sn_set.key[_i], _q)) { _r = sn_first[_i]; break; } _r; })
  /* A name already called is fully marked: reachability never clears, so a
     second MARK_NAME of it found nothing to do. */
  #define CN_ADD(NM) do { const char *_n=(NM); if(_n && !anh_has(&cn_set,_n)){ \
    if(cn_n>=cn_cap){cn_cap=cn_cap?cn_cap*2:32;called_names=realloc(called_names,sizeof(char*)*cn_cap);} \
    called_names[cn_n++]=strdup(_n); anh_add(&cn_set,called_names[cn_n-1]);} } while(0)

  /* Helper: mark a name reachable -- all scopes with that name join the BFS. */
  #define MARK_NAME(NM) do { const char *_mn=(NM); if(_mn && !anh_has(&cn_set,_mn)){ CN_ADD(_mn); \
    for(int _t=SN_FIRST(_mn);_t>=0;_t=sn_link[_t]) \
      if(!c->scopes[_t].reachable) \
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

  /* `obj.attr ||= v` calls the reader and, conditionally, the writer -- and it
     is a CallOrWriteNode, not a CallNode, so the walk above never saw either
     name. It did not matter while the emitter reached past both to the ivar;
     it does now that a hand-written pair is called (#4148). */
  {
    int any_cw = 0;
    for (int id = 0; id < c->nt->count; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || (!sp_streq(ty, "CallOrWriteNode") && !sp_streq(ty, "CallAndWriteNode"))) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm) continue;
      char wnm[300]; snprintf(wnm, sizeof wnm, "%s=", nm);
      MARK_NAME(nm); MARK_NAME(wnm);
      any_cw = 1;
    }
    if (any_cw)
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
  }

  /* A `case obj when other` arm calls the pattern's #=== (or #==, which Ruby's
     default #=== is), and that call has no CallNode of its own -- the arm is a
     WhenNode. Mark them reachable when any case has a when arm (#3820). */
  {
    int has_when = 0;
    for (int id = 0; id < c->nt->count && !has_when; id++) {
      const char *ty = nt_type(c->nt, id);
      if (ty && sp_streq(ty, "WhenNode")) has_when = 1;
    }
    if (has_when) {
      MARK_NAME("==="); MARK_NAME("==");
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
    }
  }

  /* `reduce(:sym)` / `inject(seed, :sym)` names the method by Symbol, which is
     not a CallNode, so the BFS above never
     saw the name and the method was pruned -- the fold then fell through to
     the unresolved-call raise, which reads as "undefined method '^' for an
     instance of F" for a method the program plainly defines (#4069). Only a
     method with no OTHER call site was affected, which is what made it look
     like an operator problem rather than a reachability one. */
  {
    for (int id = 0; id < c->nt->count; id++) {
      if (nt_kind(c->nt, id) != NK_CallNode) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm) continue;
      if (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject")) continue;
      int args = nt_ref(c->nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &an) : NULL;
      for (int k = 0; k < an; k++)
        if (nt_kind(c->nt, av[k]) == NK_SymbolNode && nt_str(c->nt, av[k], "value"))
          MARK_NAME(nt_str(c->nt, av[k], "value"));
      /* `reduce(seed, &:sym)` spells the same operator in the block, where the
         argument walk above cannot see it. */
      { int blk = nt_ref(c->nt, id, "block");
        if (blk >= 0 && nt_type(c->nt, blk) && sp_streq(nt_type(c->nt, blk), "BlockArgumentNode")) {
          int ex = nt_ref(c->nt, blk, "expression");
          if (ex >= 0 && nt_kind(c->nt, ex) == NK_SymbolNode && nt_str(c->nt, ex, "value"))
            MARK_NAME(nt_str(c->nt, ex, "value"));
        } }
    }
    /* `sum(seed)` names no operator at all: the fold applies the SEED's `+`
       once per element. A user class used as the seed therefore needs its `+`
       kept, exactly as `reduce(seed, :+)` does -- without it the dispatch arm
       was emitted empty and the fold raised NoMethodError for a method the
       program defines. MARK_NAME is by name and global, so ask first whether
       the seed could BE a user object: a literal, or an expression that can
       only build a builtin, never reaches a user `+`, and marking for one
       resurrects every `+` in the program -- including bodies the emitter
       cannot compile, which turned a program that built into one that does
       not. A local, an ivar, a constant or a call such as `Money.new(0)`
       marks, which is the same reach `reduce(seed, :+)` already has. */
    for (int id = 0; id < c->nt->count; id++) {
      if (nt_kind(c->nt, id) != NK_CallNode) continue;
      { const char *nm = nt_str(c->nt, id, "name");
        if (!nm || !sp_streq(nm, "sum")) continue; }
      if (nt_ref(c->nt, id, "block") >= 0) continue;   /* a block decides what is summed */
      /* `"abc".sum(16)` is String's checksum width, not a fold seed */
      { int rcv = nt_ref(c->nt, id, "receiver");
        if (rcv >= 0 && nt_kind(c->nt, rcv) == NK_StringNode) continue; }
      { int args = nt_ref(c->nt, id, "arguments");
        int an = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &an) : NULL;
        if (an < 1 || !av) continue;
        if (an_seed_is_builtin(c->nt, av[0])) continue;
        MARK_NAME("+");
        break; }
    }
    while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
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

  /* Kernel#Integer converts its argument through its #to_int, then #to_str,
     then #to_i, and Kernel#Float through its #to_f -- calls the conversion
     site names nowhere in the AST. The receiver forms (obj.send(:Integer, x)
     desugared, Kernel.Float(x)) convert the same way, so any call by the name
     counts, and the program is marked as one that converts at all. */
  {
    int has_kint = 0, has_kflt = 0;
    for (int id = 0; id < c->nt->count; id++) {
      if (nt_kind(c->nt, id) != NK_CallNode) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm) continue;
      if (sp_streq(nm, "Integer")) has_kint = 1;
      else if (sp_streq(nm, "Float")) has_kflt = 1;
    }
    /* a Numeric of the program's own converts through its #to_f wherever a
       Float argument is taken (Math.sqrt(big_decimal)): the same bridge */
    int has_unum = 0;
    NT_FOREACH_KIND(c->nt, NK_ClassNode, cid) {
      int sup = nt_ref(c->nt, cid, "superclass");
      const char *sn = sup >= 0 ? nt_str(c->nt, sup, "name") : NULL;
      if (sn && sp_streq(sn, "Numeric")) has_unum = 1;
    }
    c->uses_kconv = has_kint || has_kflt || has_unum;
    if (has_kint) { MARK_NAME("to_int"); MARK_NAME("to_str"); MARK_NAME("to_i"); }
    if (has_kflt || has_unum) MARK_NAME("to_f");
    if (has_kint || has_kflt || has_unum)
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
  }

  /* JSON.generate serializes a nested user object through its own #to_json,
     which likewise has no call site of its own in the AST. */
  if (sp_feature_required("json")) {
    MARK_NAME("to_json");
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
        int an_live = (an && anh_has(&cn_set, an)), ao_live = (ao && anh_has(&cn_set, ao));
        /* also check reachable scope names (covers scope-backed aliases) */
        if (an) for (int t = SN_FIRST(an); t >= 0 && !an_live; t = sn_link[t]) if (c->scopes[t].reachable) an_live = 1;
        if (ao) for (int t = SN_FIRST(ao); t >= 0 && !ao_live; t = sn_link[t]) if (c->scopes[t].reachable) ao_live = 1;
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
           implementation directly -- so mark the shadow reachable too. */
        int pf_in_called = anh_has(&cn_set, pf);
        if (!pf_in_called)
          for (int t = SN_FIRST(pf); t >= 0; t = sn_link[t])
            if (c->scopes[t].reachable) { pf_in_called = 1; break; }
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
  anh_free(&cn_set); anh_free(&sn_set); free(sn_first); free(sn_link);
  #undef CN_ADD
  #undef MARK_NAME
  #undef SN_FIRST

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
/* Twice: `early`, before the type fixpoint, so that the return type a poly
   receiver's call unifies over its candidates (analyze_infer.c) leaves out a
   class no reachable code constructs -- a dead FFI wrapper's `Vector2.new`
   put Vector2's float `x` into every `x` read in the program, and a Struct
   field read beside it went poly (#4460); and again after it, when a `.new`
   on a receiver the types can name as a Class value can be told from one
   that cannot. The early pass has no types, so it takes every `.new` on a
   non-constant receiver as the dynamic case and keeps every class: it can
   only over-mark relative to the late pass, and the late pass can only
   un-mark, so an arm codegen drops was never in the inferred union. */
void compute_instantiated(Compiler *c, int early) {
  const NodeTable *nt = c->nt;
  int disable = 0;
  /* The early pass writes ctor_reachable and leaves `instantiated` as the
     fixpoint has always seen it (unset); the late pass writes both. */
  int *keep = NULL;
  if (early) { keep = (int *)malloc(sizeof(int) * (size_t)(c->nclasses ? c->nclasses : 1)); for (int k = 0; k < c->nclasses; k++) keep[k] = c->classes[k].instantiated; }
  for (int k = 0; k < c->nclasses; k++) c->classes[k].instantiated = 0;
  /* Struct classes: conservatively live (their instances flow as poly). A
     native (C-backed) class too: its instances can be minted on the C side
     and handed back boxed through a :any binding, with no `.new` in the
     program for this census to see, and a class it left uninstantiated had
     its arms dropped from every poly dispatch (#4504). */
  for (int k = 0; k < c->nclasses; k++)
    if (c->classes[k].is_struct || c->classes[k].is_native_class) c->classes[k].instantiated = 1;
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
        /* A construction inside a method nothing reaches mints nothing:
           compute_reachable has run, and its answer is conservative (every
           method whose name is mentioned anywhere is reachable), so a site
           it leaves dead is dead. A dead FFI wrapper's `Vector2.new` used to
           make Vector2 an arm of every `x` / `y` dispatch in the program,
           and the float it returns widened an unrelated hot loop's Struct
           field reads to poly (#4460). */
        Scope *encl = comp_scope_of(c, id);
        if (ci >= 0 && (!encl || encl->reachable)) c->classes[ci].instantiated = 1;
        /* an unknown constant is a builtin (Array.new, ...) -- not a user arm */
      }
      else if (early || comp_ntype(c, recv) == TY_CLASS || comp_ntype(c, recv) == TY_POLY) {
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
      if (encl && encl->class_id >= 0 && encl->is_cmethod && encl->reachable)
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
  for (int k = 0; k < c->nclasses; k++) c->classes[k].ctor_reachable = c->classes[k].instantiated;
  if (early) { for (int k = 0; k < c->nclasses; k++) c->classes[k].instantiated = keep[k]; free(keep); }
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
/* Count this subtree's nodes into `cnt` (one pass per proc body gives every
   node the number of proc bodies covering it, i.e. its nesting depth). */
static void a_count_subtree(Compiler *c, int id, int *cnt, int depth) {
  if (id < 0 || depth > 400) return;
  cnt[id]++;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_count_subtree(c, ch, cnt, depth + 1); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_count_subtree(c, ids[k], cnt, depth + 1); }
}

/* Stamp every node in the subtree with `owner` (a proc-create node id). */
static void a_stamp_subtree(Compiler *c, int id, int *owner_of, int owner, int depth) {
  if (id < 0 || depth > 400) return;
  owner_of[id] = owner;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_stamp_subtree(c, ch, owner_of, owner, depth + 1); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_stamp_subtree(c, ids[k], owner_of, owner, depth + 1); }
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
/* Does this method hand its own block on to a POLY receiver? The dispatch
   there materializes whatever block reaches it as a real proc, so even though
   the method itself is yield-inlined (its `&blk` calls are spliced at the call
   site), the block passed to it still escapes into that proc and its captures
   need cells. */
static int a_scope_forwards_block_to_poly(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (!m->blk_param) return 0;
  for (int nid = 0; nid < nt->count; nid++) {
    if (c->nscope[nid] != mi) continue;
    const char *ty = nt_type(nt, nid);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int blk = nt_ref(nt, nid, "block");
    const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (!bty || !sp_streq(bty, "BlockArgumentNode")) continue;
    int fwd = nt_ref(nt, blk, "expression");
    if (fwd >= 0) {   /* named `&blk`: only THIS method's block param counts */
      const char *fty = nt_type(nt, fwd);
      const char *fn = fty && sp_streq(fty, "LocalVariableReadNode") ? nt_str(nt, fwd, "name") : NULL;
      if (!fn || !m->blk_param[0] || !sp_streq(fn, m->blk_param)) continue;
    }
    int recv = nt_ref(nt, nid, "receiver");
    if (recv < 0) continue;
    if (infer_type(c, recv) == TY_POLY) return 1;
  }
  return 0;
}

/* Does scope `mi` hand its block param on to a method that takes a REAL &block
   -- one lowered out of yield-inlining because it recurses (or yields from
   inside a lifted body)? Such a target cannot have the block spliced into it,
   so the block has to be materialized as a proc, and a forwarder in front of
   it cannot splice either. The forwarder is still marked `yields` (forwarding
   is what earns that mark), which is exactly what made a_block_is_lifted call
   the literal block unlifted and leave its captures without cells (#4145). */
static int a_scope_forwards_block_to_lowered_1(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (!m->blk_param) return 0;
  for (int nid = 0; nid < nt->count; nid++) {
    if (c->nscope[nid] != mi) continue;
    const char *ty = nt_type(nt, nid);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int blk = nt_ref(nt, nid, "block");
    const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (!bty || !sp_streq(bty, "BlockArgumentNode")) continue;
    const char *tn = nt_str(nt, nid, "name");
    if (!tn || nt_ref(nt, nid, "receiver") >= 0) continue;
    int tmi = comp_self_call_mi(c, nid, tn);
    if (tmi >= 0 && tmi != mi && c->scopes[tmi].is_lowered_yield) return 1;
  }
  return 0;
}

static int a_scope_forwards_block_to_lowered(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (!m->blk_param) return 0;
  for (int nid = 0; nid < nt->count; nid++) {
    if (c->nscope[nid] != mi) continue;
    const char *ty = nt_type(nt, nid);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    int blk = nt_ref(nt, nid, "block");
    const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (!bty || !sp_streq(bty, "BlockArgumentNode")) continue;
    int fwd = nt_ref(nt, blk, "expression");
    if (fwd >= 0) {   /* named `&blk`: only THIS method's block param counts */
      const char *fty = nt_type(nt, fwd);
      const char *fn = fty && sp_streq(fty, "LocalVariableReadNode") ? nt_str(nt, fwd, "name") : NULL;
      if (!fn || !m->blk_param[0] || !sp_streq(fn, m->blk_param)) continue;
    }
    const char *tn = nt_str(nt, nid, "name");
    if (!tn) continue;
    int recv = nt_ref(nt, nid, "receiver");
    int tmi = -1;
    if (recv < 0) tmi = comp_self_call_mi(c, nid, tn);
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) tmi = comp_method_in_chain(c, ty_object_class(rt), tn, NULL);
    }
    if (tmi < 0 || tmi == mi) continue;
    if (c->scopes[tmi].is_lowered_yield) return 1;
    /* the target is itself a forwarder: follow one link, which is as deep as
       the shape goes in practice and keeps this terminating */
    if (c->scopes[tmi].blk_param && c->scopes[tmi].blk_param[0] &&
        a_scope_forwards_block_to_lowered_1(c, tmi)) return 1;
  }
  return 0;
}

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
    int const_recv = rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"));
    int const_is_class = 0;
    if (const_recv) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        const_is_class = 1; mi = comp_cmethod_in_chain(c, ci, name, NULL);
        /* `Klass.new { }` with no `def self.new` hands the block to
           initialize, which lifts it like any method keeping a real &block */
        if (mi < 0 && sp_streq(name, "new")) mi = comp_method_in_chain(c, ci, "initialize", NULL);
      }
    }
    /* A constant that names no class is an ordinary VALUE (`CONFIG.each { }`),
       so it is typed like any other receiver -- including poly, whose dispatch
       lifts the block. Reading it as a class name and stopping there left such
       a block unlifted and its captures without storage. */
    if (!const_recv || !const_is_class) {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
      /* A poly receiver dispatches on the runtime class, and the dispatch
         materializes the block as a real proc once, ahead of the switch, for
         any candidate that takes a real &block or is served by a proc-form
         clone. That proc is as escaping as any other, so its captures need
         cells -- which nothing here decided, because no single `mi` names the
         target. Codegen then met a capture with no storage and refused the
         whole call. Reachable only since a yielding method became
         poly-dispatchable at all (#3408). */
      else if (rt == TY_POLY) {
        /* not filtered by `instantiated`: that is decided after this runs, and
           a spurious cell is only a missed optimization */
        for (int k = 0; k < c->nclasses; k++) {
          int ci2 = comp_method_in_chain(c, k, name, NULL);
          if (ci2 < 0) continue;
          Scope *cm2 = &c->scopes[ci2];
          if (cm2->yields || (cm2->blk_param && cm2->blk_param[0])) return 1;
          /* The candidate ignores the block, but a builtin Array or Hash in
             the same union does not: the dispatch's builtin arm materializes
             the block as a proc and drives it over the elements. That proc
             escapes like any other, and without cells the block's writes to
             enclosing locals went to a copy -- `a.map { |x| n = n + 1 }` on a
             real Array ran the block and left n at 0 (#3459). */
          { int bargs = nt_ref(nt, id, "arguments");
            int bn2 = 0;
            if (bargs >= 0) nt_arr(nt, bargs, "arguments", &bn2);
            if (bn2 == 0 && poly_enum_op_for(name)) return 1; }
        }
      }
    }
  }
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  /* A lowered yielding method also receives its block as a real proc, so a
     block passed to it is lifted and captures enclosing locals like any other. */
  if (!m->blk_param || !m->blk_param[0]) return 0;
  /* A yielding method has its block SPLICED into it, so nothing escapes and
     the block needs no cells -- unless the block does not stop here. A
     forwarder is marked `yields` too, and if what it forwards to is a lowered
     yielder taking a real &block, the block is materialized as a proc after
     all. Its captures need storage like any other proc's (#4145). */
  if (m->yields && !a_scope_forwards_block_to_lowered(c, mi)) return 0;
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

/* Does yield-inlined scope `mi` hand its block on to a method that KEEPS it --
   one taking a real named &block that it is not spliced into? An anonymous `&`
   is always inlined (it has no name to escape through), so a literal block at
   its call site is spliced onto the forward and lands on the keeper's call,
   which materializes it as a proc that outlives the call. A named `&blk`
   forward straight into a keeper is never inlined in the first place (#3772);
   one into an anonymous forwarder is, so inlined forwarders are followed a few
   links deep. */
static int a_scope_forwards_block_to_keeper(Compiler *c, int mi, int depth) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (!m->blk_param || !m->yields || depth > 4) return 0;
  for (int nid = 0; nid < nt->count; nid++) {
    if (c->nscope[nid] != mi) continue;
    if (nt_kind(nt, nid) != NK_CallNode) continue;
    int blk = nt_ref(nt, nid, "block");
    const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (!bty || !sp_streq(bty, "BlockArgumentNode")) continue;
    int fwd = nt_ref(nt, blk, "expression");
    if (fwd >= 0) {   /* named `&blk`: only THIS method's block param counts */
      const char *fty = nt_type(nt, fwd);
      const char *fn = fty && sp_streq(fty, "LocalVariableReadNode") ? nt_str(nt, fwd, "name") : NULL;
      if (!fn || !m->blk_param[0] || !sp_streq(fn, m->blk_param)) continue;
    }
    const char *tn = nt_str(nt, nid, "name");
    if (!tn) continue;
    int recv = nt_ref(nt, nid, "receiver");
    int tmi = -1;
    if (recv < 0) tmi = comp_self_call_mi(c, nid, tn);
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) tmi = comp_method_in_chain(c, ty_object_class(rt), tn, NULL);
    }
    if (tmi < 0 || tmi == mi) continue;
    Scope *t = &c->scopes[tmi];
    if (!t->blk_param) continue;
    if (!t->yields && t->blk_param[0]) return 1;
    if (a_scope_forwards_block_to_keeper(c, tmi, depth + 1)) return 1;
  }
  return 0;
}

/* The block of a call whose target hands the block on to a poly receiver, or
   to a method that keeps it. The target is yield-inlined, so the block is
   spliced into its body -- and lands on the dispatch or the keeper's call
   there, which materializes it as a real proc. Only the capture marking needs
   this: the target itself keeps no &block, so the inlining decision (which
   reads a_proc_create_or_lifted) must not see it. */
static int a_block_forwarded_to_proc(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int blk = nt_ref(nt, id, "block");
  const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int mi = -1;
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL);
  }
  else {
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  }
  return mi >= 0 && (a_scope_forwards_block_to_poly(c, mi) ||
                     a_scope_forwards_block_to_keeper(c, mi, 0));
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

/* Does this method's RETURN VALUE come out of a `yield`? For a lowered
   recursive yielder that matters to the C type: the emitted tail reads the
   block's answer out of the poly side-channel as a raw slot
   (`_sp_proc_poly_ret.v.i`), so the function has to be typed to receive it.
   A yield whose value is discarded -- a bare `yield` statement -- says nothing
   about the return, and typing on it made `walk` answer Integer where it
   answers nil, and a body ending in a String emit `return char *` from a
   function typed sp_int (#4145). */
static int a_subtree_has_yield(const NodeTable *nt, int id, int depth) {
  if (id < 0 || id >= nt->count || depth > 200) return 0;
  const char *ty = nt_type(nt, id);
  if (ty && sp_streq(ty, "YieldNode")) return 1;
  /* a nested block or lambda has its own yield target */
  if (ty && (sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode"))) return 0;
  const SpNode *nd = &nt->nodes[id];
  for (int i = 0; i < nd->nr; i++)
    if (a_subtree_has_yield(nt, nd->r[i].ref, depth + 1)) return 1;
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      if (a_subtree_has_yield(nt, nd->a[i].ids[j], depth + 1)) return 1;
  return 0;
}

static int a_scope_returns_a_yield(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  /* an explicit `return yield ...` anywhere in the body */
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "ReturnNode")) continue;
    if (a_subtree_has_yield(nt, nt_ref(nt, id, "arguments"), 0)) return 1;
  }
  /* the body's tail, which is the value a method without `return` answers */
  if (m->body >= 0) {
    int bn = 0; const int *bb = nt_arr(nt, m->body, "body", &bn);
    if (bb && bn > 0 && a_subtree_has_yield(nt, bb[bn - 1], 0)) return 1;
  }
  return 0;
}

/* (key, scope) -> ascending node list, for mark_proc_captures' per-name
   lookups (rubys/roundhouse#72): the writes of a name in the enclosing scope,
   and the procs and block calls that bind a name as a parameter. Each was a
   walk of every write, or every proc and block call, per captured name per
   proc. */
typedef struct MpEnt { const char *key; int sc; int *ids; int n, cap; struct MpEnt *next; } MpEnt;
typedef struct { MpEnt **tab; int nb; } MpIx;
static unsigned mp_hash(const char *k, int sc) {
  unsigned h = 2166136261u ^ (unsigned)sc * 2654435761u;
  for (; *k; k++) { h ^= (unsigned char)*k; h *= 16777619u; }
  return h;
}
static void mp_add(MpIx *ix, const char *k, int sc, int id) {
  if (!ix->tab) { ix->nb = 4096; ix->tab = calloc((size_t)ix->nb, sizeof(MpEnt *)); }
  unsigned b = mp_hash(k, sc) % (unsigned)ix->nb;
  MpEnt *e = ix->tab[b];
  while (e && !(e->sc == sc && sp_streq(e->key, k))) e = e->next;
  if (!e) { e = calloc(1, sizeof *e); e->key = k; e->sc = sc; e->next = ix->tab[b]; ix->tab[b] = e; }
  if (e->n && e->ids[e->n - 1] == id) return;   /* a proc naming a param twice */
  if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 4; e->ids = realloc(e->ids, sizeof(int) * (size_t)e->cap); }
  e->ids[e->n++] = id;
}
static const int *mp_get(const MpIx *ix, const char *k, int sc, int *n) {
  *n = 0;
  if (!ix->tab) return NULL;
  for (MpEnt *e = ix->tab[mp_hash(k, sc) % (unsigned)ix->nb]; e; e = e->next)
    if (e->sc == sc && sp_streq(e->key, k)) { *n = e->n; return e->ids; }
  return NULL;
}
static void mp_free(MpIx *ix) {
  if (!ix->tab) return;
  for (int b = 0; b < ix->nb; b++)
    for (MpEnt *e = ix->tab[b]; e; ) { MpEnt *nx = e->next; free(e->ids); free(e); e = nx; }
  free(ix->tab);
}

void mark_proc_captures(Compiler *c) {
  const NodeTable *nt = c->nt;
  char *inproc = (char *)calloc((size_t)nt->count, 1);
  if (!inproc) return;
  for (int id = 0; id < nt->count; id++)
    if (a_proc_create_or_lifted(c, id) || a_block_forwarded_to_proc(c, id)) {
      int body = a_proc_body(c, id); if (body >= 0) a_mark_subtree(c, body, inproc); }

  /* Which proc frame each node belongs to: the INNERMOST proc whose body
     contains it, or -1 for the enclosing scope's own code. A local written in
     the body of the proc that encloses this one lives in that frame and is
     capturable from here, so asking only "is it inside some proc" refused a
     lambda nested in another proc -- including the wrapper that
     desugar_block_capture_wrap builds around an iteration block, which is why
     a lambda capturing a block local alongside the block parameter was
     rejected (#3912). Stamping outer procs first lets each inner one overwrite
     its own region. */
  int *procof = (int *)malloc(sizeof(int) * (size_t)nt->count);
  int *pdepth = (int *)calloc((size_t)nt->count, sizeof(int));
  if (procof && pdepth) {
    for (int id = 0; id < nt->count; id++) procof[id] = -1;
    /* nesting depth of each proc = how many proc bodies cover it, counted by
       stamping every proc body once (O(nodes) per proc, not O(nodes) per pair) */
    for (int id = 0; id < nt->count; id++) {
      if (!a_proc_create_or_lifted(c, id) && !a_block_forwarded_to_proc(c, id)) continue;
      int body = a_proc_body(c, id);
      if (body >= 0) a_count_subtree(c, body, pdepth, 0);
    }
    int maxd = 0;
    for (int id = 0; id < nt->count; id++) {
      if (!a_proc_create_or_lifted(c, id) && !a_block_forwarded_to_proc(c, id)) continue;
      pdepth[id] += 1;                /* 0 means "not a proc" */
      if (pdepth[id] > maxd) maxd = pdepth[id];
    }
    for (int d = 1; d <= maxd; d++)
      for (int id = 0; id < nt->count; id++) {
        if (pdepth[id] != d) continue;
        int body = a_proc_body(c, id);
        if (body >= 0) a_stamp_subtree(c, body, procof, id, 0);
      }
  }
  free(pdepth);

  /* The two walks per captured name below read only two kinds of node: the
     writes, and the procs and block calls that may bind the name. Listed
     once, in node order, they were whole-table walks per name per proc. */
  int *wl = malloc(sizeof(int) * (size_t)(nt->count + 1));
  int *ql = malloc(sizeof(int) * (size_t)(nt->count + 1));
  char *qproc = calloc((size_t)nt->count + 1, 1);
  int nwl = 0, nql = 0;
  if (wl && ql && qproc)
    for (int q = 0; q < nt->count; q++) {
      if (a_is_write_node(nt_type(nt, q))) wl[nwl++] = q;
      int qp = is_proc_create(c, q);
      int qb = 0;
      if (!qp && nt_kind(nt, q) == NK_CallNode) {
        int qblk = nt_ref(nt, q, "block");
        qb = qblk >= 0 && nt_kind(nt, qblk) == NK_BlockNode;
      }
      if (qp || qb) { qproc[q] = (char)qp; ql[nql++] = q; }
    }
  MpIx wix = {0}, qix = {0};
  for (int wi = 0; wi < nwl; wi++) {
    const char *wn = nt_str(nt, wl[wi], "name");
    if (wn) mp_add(&wix, wn, c->nscope[wl[wi]], wl[wi]);
  }
  for (int qi = 0; qi < nql; qi++) {
    int q = ql[qi];
    int qpn = a_proc_params_node(c, q);
    int qrn = 0; const int *qreqs = qpn >= 0 ? nt_arr(nt, qpn, "requireds", &qrn) : NULL;
    for (int k = 0; k < qrn; k++) {
      const char *qn = nt_str(nt, qreqs[k], "name");
      if (qn) mp_add(&qix, qn, -1, q);
    }
  }

  for (int id = 0; id < nt->count; id++) {
    if (!a_proc_create_or_lifted(c, id) && !a_block_forwarded_to_proc(c, id)) continue;
    /* A fiber/generator only needs a cell for a *value-type* capture, where a
       by-value copy would drop the write. A captured heap object (string, array,
       hash, ...) is already shared by pointer -- in-place mutation reaches the
       enclosing scope -- and the cell machinery does not handle some of those
       types (e.g. a mutable-string buffer), so leave them by value. */
    int fib_create = a_is_fiber_or_gen_create(c, id);
    /* a lifted iteration block is consumed while its call runs; everything
       else here may hold its cells past the call (see LocalVar.cell_outlives) */
    int outlives = !(a_block_is_lifted(c, id) && !is_proc_create(c, id) && !fib_create &&
                     !is_handler_proc_block(c, id) && !a_block_forwarded_to_proc(c, id));
    /* ...nor does the capture-wrap lambda (desugar_block_capture_wrap): it is
       called where it is made and returns inside the iteration, so its cells
       end with the call. Counted as outliving, a parameter it captured lost
       the by-reference ABI, and every append to lobsters' `io` in show_into
       landed in a copy the caller never saw (#5087). */
    if (nt_int(nt, id, "cap_iife", 0)) outlives = 0;
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
      int myframe = procof ? procof[id] : -1;
      int nwn = 0; const int *wns = mp_get(&wix, nm, encl, &nwn);
      for (int wi = 0; wi < nwn && !owned; wi++) {
        int w = wns[wi];
        if (c->nscope[w] != encl) continue;
        const char *wn = nt_str(nt, w, "name");
        if (!wn || !sp_streq(wn, nm)) continue;
        if (!procof) { if (!inproc[w]) owned = 1; continue; }
        /* The write's frame is this proc's own, or one that ENCLOSES it. An
           enclosing frame's write is just as capturable: a_collect_used is
           deep, so every proc between that write and this reader has the name
           in its own used set and cells it too, and the chain of cells is what
           this proc reads through. Accepting only myframe refused a block
           nested inside a Thread whose body never names the variable directly
           -- the write sat in the outermost frame, two levels up, and the
           compiler reported an uncaptured outer variable (#4349). */
        int wf = procof[w], f = myframe;
        for (;;) {
          if (wf == f) { owned = 1; break; }
          if (f < 0) break;
          f = procof[f];
        }
      }
      /* a param of an ENCLOSING proc: it lives in that proc's frame (a "later
         slice"), and this proc closes over it -- the enclosing proc's prologue
         materializes the cell (#2648). Only proc-fn enclosers: an inlined
         iterator block's params bind in the loop, where no cell exists yet. */
      /* The binding-site walk below runs whether or not a write already
         settled `owned`: a param of an INLINED iteration block that the body
         also REASSIGNS is owned by the write, but the loop still binds it by
         writing the plain C slot, so it needs the shadow slot all the same
         (the cell alone left `lv_i` undeclared, and the C build failed). */
      int shadow = 0;
      {
        int owned_q = 0;
        int nqn = 0; const int *qns = mp_get(&qix, nm, -1, &nqn);
        for (int qi = 0; qi < nqn && !owned_q; qi++) {
          int q = qns[qi];
          if (q == id) continue;
          /* An INLINED iteration block binds its params in the loop, where the
             emitters write the plain C slot -- so celling one needs the slot
             kept alongside the cell (LocalVar.cell_shadow) and copied in at the
             top of the body. Refusing instead left a proc lifted out of a
             nested block with nowhere to read the enclosing loop variable, and
             the emitter had to reject the whole program. */
          int q_is_proc = qproc[q];
          int q_is_block = !q_is_proc;   /* listed: a proc or a block call */
          /* One shared cell per loop, not one per iteration: only a proc the
             call CONSUMES while the iteration runs may read it. A `proc {}` /
             lambda / Fiber / Thread body outlives its iteration -- Ruby gives
             each of those its own binding -- so those keep the by-value
             capture they had. */
          if (!q_is_proc && q_is_block && (fib_create || is_proc_create(c, id))) continue;
          if (!q_is_proc && !q_is_block) continue;
          int qpn = a_proc_params_node(c, q);
          int qrn = 0; const int *qreqs = qpn >= 0 ? nt_arr(nt, qpn, "requireds", &qrn) : NULL;
          int has = 0;
          for (int k = 0; k < qrn && !has; k++) {
            const char *qn = nt_str(nt, qreqs[k], "name");
            if (qn && sp_streq(qn, nm)) has = 1;
          }
          if (!has) continue;
          int qb = a_proc_body(c, q);
          if (qb >= 0 && a_subtree_contains(nt, qb, id, 0)) { owned_q = 1; shadow = !q_is_proc; }
        }
        if (owned_q) owned = 1;
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
        if (outlives) lv->cell_outlives = 1;
        if (shadow) lv->cell_shadow = 1;
      }
    }
    free(params.v); free(used.v);
  }
  free(procof);
  free(inproc); free(wl); free(ql); free(qproc);
  mp_free(&wix); mp_free(&qix);
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

/* The value a write actually CARRIES, looking through a chain. infer_type of
   a write node answers the target's slot type, which is right for the
   expression's value (`a = (b = x)` evaluates to b's slot) but wrong as
   evidence about this assignment: `a = b = 0` puts 0 in both, and b widening
   to Bignum later says nothing about a. Reading the slot promoted a through
   the chain, and a return of `[a]` then declared a typed array while the body
   built a poly one -- a C function whose result type is not what it returns
   (#4686). */
static int bigint_cascade_value_node(const NodeTable *nt, int id) {
  int v = nt_ref(nt, id, "value");
  for (int guard = 0; v >= 0 && guard < 64; guard++) {
    NodeKind k = nt_kind(nt, v);
    if (k != NK_LocalVariableWriteNode) break;
    v = nt_ref(nt, v, "value");
  }
  return v;
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
        TyKind vt = infer_type(c, bigint_cascade_value_node(nt, id));
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

static int ie_self_call_names(Compiler *c, int node, const char **names, int n, int max) {
  const NodeTable *nt = c->nt;
  if (node < 0) return n;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return n;
  int skip = -1;
  if (k == NK_CallNode) {
    int r = nt_ref(nt, node, "receiver");
    const char *nm = nt_str(nt, node, "name");
    int self_call = r < 0 || nt_kind(nt, r) == NK_SelfNode;
    if (nm && self_call && !ie_kernel_global(nm)) {
      int seen = 0;
      for (int i = 0; i < n; i++) if (sp_streq(names[i], nm)) seen = 1;
      if (!seen && n < max) names[n++] = nm;
    }
    if (self_call || (nm && (sp_streq(nm, "instance_eval") || sp_streq(nm, "instance_exec"))))
      skip = nt_ref(nt, node, "block");
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    if (ch != skip) n = ie_self_call_names(c, ch, names, n, max);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) n = ie_self_call_names(c, ids[j], names, n, max);
  }
  return n;
}

static int ie_class_answers(Compiler *c, int k, const char *nm) {
  return comp_method_in_chain(c, k, nm, NULL) >= 0 || comp_reader_in_chain(c, k, nm, NULL);
}

static int ie_poly_class_ok(Compiler *c, int k) {
  return !c->classes[k].is_native_class && c->classes[k].ctor_reachable &&
         builtin_class_id(c->classes[k].name) == 0;
}

/* The classes a poly receiver can run an instance_eval/exec body as: those
   answering every self call that some class, and no top-level def, answers
   (*need: the first). 0 when the body makes no such call. */
int ie_poly_self_classes(Compiler *c, const char *name, int body, int *out, int max,
                         const char **need) {
  const char *names[64];
  int nn = ie_self_call_names(c, body, names, 0, 64);
  if (nn == 64) return 0;
  int ask[64], nask = 0;
  for (int i = 0; i < nn; i++) {
    if (comp_method_index(c, names[i]) >= 0) continue;
    for (int k = 0; k < c->nclasses; k++) {
      if (ie_poly_class_ok(c, k) && ie_class_answers(c, k, names[i])) { ask[nask++] = i; break; }
    }
  }
  if (nask == 0) return 0;
  if (need) *need = names[ask[0]];
  int n = 0;
  for (int k = 0; k < c->nclasses && n < max; k++) {
    if (!ie_poly_class_ok(c, k) || comp_method_in_chain(c, k, name, NULL) >= 0) continue;
    int all = 1;
    for (int i = 0; i < nask && all; i++) all = ie_class_answers(c, k, names[ask[i]]);
    if (all) out[n++] = k;
  }
  return n;
}

static int ie_poly_mark(Compiler *c, int id, TyKind rt) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int blk = nt_ref(nt, id, "block");
  if (rt != TY_POLY || !nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode ||
      (!sp_streq(nm, "instance_eval") && !sp_streq(nm, "instance_exec"))) return -1;
  int k[2];
  int n = ie_poly_self_classes(c, nm, nt_ref(nt, blk, "body"), k, 2, NULL);
  return n == 1 ? k[0] : n > 1 ? -2 - id : -1;
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
      cls = ty_is_object(rt) ? ty_object_class(rt) : ie_poly_mark(c, id, rt);
      if (cls == -1) continue;
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

int *ie_body_retype(Compiler *c, int body, int cls) {
  int n = c->nt->count;
  if (body < 0 || !g_ie_node_class || g_ie_node_class_cap < n || g_ie_node_class[body] == cls)
    return NULL;
  int *snap = malloc(sizeof(int) * (2 * (size_t)n + 1));
  if (!snap) return NULL;
  snap[0] = n;
  for (int i = 0; i < n; i++) { snap[1 + i] = (int)c->ntype[i]; snap[1 + n + i] = g_ie_node_class[i]; }
  mark_ie_subtree(c, body, cls);
  if (cls >= 0) infer_subtree(c, body);
  return snap;
}

void ie_body_restore(Compiler *c, int *snap) {
  if (!snap) return;
  int n = snap[0];
  for (int i = 0; i < n; i++) { c->ntype[i] = (TyKind)snap[1 + i]; g_ie_node_class[i] = snap[1 + n + i]; }
  free(snap);
}

int ie_poly_classes_at(Compiler *c, int node, int *out, int max) {
  int v = ie_class_of(c, node);
  if (v >= -1) return 0;
  int call = -2 - v;
  int blk = nt_ref(c->nt, call, "block");
  return ie_poly_self_classes(c, nt_str(c->nt, call, "name"), nt_ref(c->nt, blk, "body"), out, max, NULL);
}

/* Whether `self` at node is top-level self, the main object (#4926): no
   enclosing class and no instance_eval/exec receiver rebinding it. */
int self_is_main(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  return s && s->class_id < 0 && an_ie_class_id < 0 && ie_class_of(c, node) == -1;
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
  /* `Node = Struct.new(...)` / `Data.define(...)` inside a class or module
     defines a class just as `class Node` does, and its leaf name collides the
     same way -- two sibling namespaces each with their own `TreeNode` bound
     every reference to whichever was registered first. */
  if (sp_streq(ty, "ConstantWriteNode") && depth > 0) {
    int v = nt_ref(nt, node, "value");
    const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
    if (vty && sp_streq(vty, "CallNode")) {
      const char *vn = nt_str(nt, v, "name");
      int vr = nt_ref(nt, v, "receiver");
      const char *rn = vr >= 0 && nt_type(nt, vr) &&
                       sp_streq(nt_type(nt, vr), "ConstantReadNode") ? nt_str(nt, vr, "name") : NULL;
      const char *cn = nt_str(nt, node, "name");
      if (cn && rn && vn &&
          ((sp_streq(rn, "Struct") && sp_streq(vn, "new")) ||
           (sp_streq(rn, "Data") && sp_streq(vn, "define")))) {
        if (*n >= *cap) { *cap = *cap ? *cap * 2 : 16; *ws = realloc(*ws, sizeof(QCWrite) * (size_t)*cap); }
        QCWrite *w = &(*ws)[(*n)++];
        w->node = node; w->depth = depth;
        for (int i = 0; i < depth; i++) snprintf(w->path[i], 64, "%s", path[i]);
        snprintf(w->name, sizeof w->name, "%s", cn);
      }
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
    /* A class defined INSIDE a module whose leaf name is a builtin's --
       `module M; class Array < Bench` -- collides with the BUILTIN rather than
       with another user class, and every builtin-name test (Array.new, the
       reopen checks) would take it for the builtin. Qualify it too (#3781). */
    if (!collide && ws[i].depth > 0 && builtin_class_id(ws[i].name) != 0) collide = 1;
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

/* A native class's own methods, called on an implicit self from a method the
   program adds by reopening the class. The native surface is dispatched
   through the receiver, and an implicit self has no receiver node at all, so
   every one of them was a NameError:

     class StringIO
       def rest = gets        # undefined local variable or method 'gets'
     end

   Give it the receiver it means. Same shape as the Enumerable-on-self
   redirect, and for the same reason. Only names the class's native surface
   actually declares, so a genuine typo still reports as one, and only when no
   Ruby method of the class already answers -- that one binds first. */
static int give_native_self_calls_a_receiver(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    Scope *osc = comp_scope_of(c, id);
    int ocid = osc ? osc->class_id : -1;
    if (ocid < 0 || osc->is_cmethod) continue;
    if (ocid >= c->nclasses || !c->classes[ocid].is_native_class) continue;
    if (comp_method_in_chain(c, ocid, nm, NULL) >= 0) continue;   /* the class's own Ruby method */
    int argc = 0;
    { int an = nt_ref(nt, id, "arguments");
      if (an >= 0) nt_arr(nt, an, "arguments", &argc); }
    if (comp_native_method_find(c, ocid, nm, argc, 0) < 0) continue;
    int selfn = nt_new_node(nt, "SelfNode");
    if (selfn < 0) continue;
    nt_node_set_ref(nt, id, "receiver", selfn);
    comp_grow_node_arrays(c);
    c->nscope[selfn] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* The same missing receiver, for the universal Object predicates. `is_a?` and
   friends are dispatched through the receiver, so written on an implicit self
   they had no receiver node and were rejected outright:

     class Room < ApplicationRecord
       def open? = is_a?(Rooms::Open)   # unsupported call
     end

   which is how Rails spells single-table inheritance, and how anyone spells a
   type predicate on self. Give it the receiver it means (#4142).

   The list was deliberately short at first -- only predicates that are
   Object's and have no sensible meaning as a bare call -- and being short is
   what brought it back: `to_s`, `inspect`, `hash`, `dup`, `itself` and
   `object_id` on an implicit self were all still the NameError, in EVERY
   class, so `def label = "x: " + to_s` did not compile anywhere (#4387). A
   hand-kept list of what an object answers goes stale exactly this way; the
   note on POLY_RAW in analyze_infer.c says so about its own twin, and this is
   the second time here.

   So the names come from that table rather than from a list of their own. It
   is the compiler's statement of what a receiver answers universally, every
   consumer already reads it, and a name added there is answered here without
   anyone remembering to. What stays local is the part POLY_RAW cannot know:
   the two arg-taking type predicates, and the renderers whose answer is a
   String rather than a raw scalar.

   A method the class itself defines wins, as does a top-level def of the same
   name -- both bind ahead of Object's. */
static int give_self_predicates_a_receiver(Compiler *c) {
  /* What AN_POLY_RAW cannot carry: it states what a receiver answers as a RAW
     C scalar, so a name whose answer is a String (the renderers), an object
     (`dup`, `itself`), or self (`freeze`, `tap`) has no row there and is named
     here instead. Every one of these is verified against an explicit `self.`
     receiver by test/implicit_self_universal_surface.rb, so a name that stops
     being answered fails there rather than going quiet.

     Only names that were actually refused. `freeze` and `respond_to?` already
     answered an implicit self by another route, and naming them here moved
     them onto this one -- which widened an ivar from sp_int to sp_RbVal in
     test/respond_to_implicit_self.rb. A redirect that changes the route of a
     call that already worked is a cost with no benefit.

     Not everything receiver-less can join this: the bare Kernel surface is NOT
     receiver-transparent. `raise "x"` inside a method compiles to the raise;
     rewritten to `self.raise("x")` it becomes a NoMethodError, which is what a
     list-free version of this pass did to 226 test programs. The sweep is what
     caught it -- a probe that wrapped the call in `rescue` had reported it
     working. */
  static const struct { const char *name; int argc; } preds[] = {
    { "is_a?", 1 }, { "kind_of?", 1 }, { "instance_of?", 1 },
    { "to_s", 0 }, { "inspect", 0 },
    { "itself", 0 }, { "dup", 0 }, { "clone", 0 }, { "tap", 0 },
    { "display", 0 }, { "instance_variables", 0 },
    { "instance_variable_get", 1 }, { "instance_variable_set", 2 },
  };
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, changed = 0;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int want = -1;
    for (size_t k = 0; k < sizeof preds / sizeof preds[0]; k++)
      if (sp_streq(nm, preds[k].name)) { want = preds[k].argc; break; }
    if (want < 0) want = an_poly_raw_argc(nm);   /* the universal table decides the rest */
    if (want < 0) continue;
    Scope *osc = comp_scope_of(c, id);
    int ocid = osc ? osc->class_id : -1;
    if (ocid < 0 || osc->is_cmethod) continue;
    if (ocid >= c->nclasses) continue;
    if (comp_method_in_chain(c, ocid, nm, NULL) >= 0) continue;
    if (comp_method_index(c, nm) >= 0) continue;
    int argc = 0;
    { int an = nt_ref(nt, id, "arguments");
      if (an >= 0) nt_arr(nt, an, "arguments", &argc); }
    if (argc != want) continue;
    int selfn = nt_new_node(nt, "SelfNode");
    if (selfn < 0) continue;
    nt_node_set_ref(nt, id, "receiver", selfn);
    comp_grow_node_arrays(c);
    c->nscope[selfn] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* Give each block its own numbered parameters, where a scope holds more than
   one block that uses them.

   `_1` .. `_9` (and `it`, which the parser lowers to `_1`) are names spinel
   synthesizes rather than names the author wrote, and blocks share their
   enclosing scope's local table -- so two such blocks in one method interned
   the SAME slot and their types merged. A scope with one of them gets
   `sp_int lv__1`; add a second over strings and both become a boxed
   `sp_RbVal`. Values stayed right (each block writes the slot before reading
   it), so what this costs is the type. It also left two passes typing that one
   slot from different call shapes on every round, which is the last program in
   the suite whose inference fixpoint ran to its cap (#4116).

   Only where the names actually collide, which is what rename_shadowing_block_params
   does for a named parameter and for the same reason: a scope with a single
   numbered-param block is already correct, and renaming it would churn the
   emitted identifier for nothing.

   The rewrite is textual on the AST, like that pass: the body's reads are
   rewritten, the block's comma-joined `locals` string with them, and the
   generated name is recorded on the NumberedParametersNode where
   numbered_param_name reads it back. The node keeps its shape, so every pass
   that recognises a numbered-param block still does.

   A nested block with its own numbered parameters is a SyntaxError in CRuby
   ("numbered parameter is already used in outer block"), so the walk stops at
   one rather than guessing which block a name belongs to. */
static int numbered_params_node_of(Compiler *c, int L) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, L);
  if (!ty || (!sp_streq(ty, "BlockNode") && !sp_streq(ty, "LambdaNode"))) return -1;
  int bp = nt_ref(nt, L, "parameters");
  if (bp < 0) return -1;
  const char *bpt = nt_type(nt, bp);
  return (bpt && sp_streq(bpt, "NumberedParametersNode")) ? bp : -1;
}

static void numbered_rename_reads(NodeTable *nt, int id, const char *from,
                                  const char *to, int depth) {
  if (id < 0 || id >= nt->count || depth > 200) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode")) {
    int bp = nt_ref(nt, id, "parameters");
    const char *bpt = bp >= 0 ? nt_type(nt, bp) : NULL;
    if (bpt && sp_streq(bpt, "NumberedParametersNode")) return;   /* binds it itself */
  }
  if (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, from)) nt_set_str(nt, id, "name", to);
  }
  const SpNode *nd = &nt->nodes[id];
  for (int i = 0; i < nd->nr; i++) numbered_rename_reads(nt, nd->r[i].ref, from, to, depth + 1);
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      numbered_rename_reads(nt, nd->a[i].ids[j], from, to, depth + 1);
}

/* Replace `from` with `to` in a block's comma-joined `locals` string.
   emit_block_locals_reset nils every name in it that is not a parameter, so a
   stale entry there nils the renamed slot right after the bind, once per
   iteration, and the body reads nil. */
static void numbered_rename_locals_str(NodeTable *nt, int L, const char *from, const char *to) {
  const char *locs = nt_str(nt, L, "locals");
  if (!locs || !*locs) return;
  size_t flen = strlen(from), cap = strlen(locs) + strlen(to) + 8, w = 0;
  char *out = (char *)malloc(cap);
  if (!out) return;
  out[0] = 0;
  for (const char *p = locs; ; ) {
    const char *e = strchr(p, ',');
    size_t seg = e ? (size_t)(e - p) : strlen(p);
    const char *put = p; size_t putn = seg;
    if (seg == flen && !strncmp(p, from, seg)) { put = to; putn = strlen(to); }
    if (w) out[w++] = ',';
    memcpy(out + w, put, putn); w += putn; out[w] = 0;
    if (!e) break;
    p = e + 1;
  }
  nt_set_str(nt, L, "locals", out);
  free(out);
}

static void scope_numbered_block_params(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  /* how many numbered-param blocks each scope holds */
  int *per_scope = (int *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(int));
  if (!per_scope) return;
  for (int L = 0; L < n0; L++) {
    if (numbered_params_node_of(c, L) < 0) continue;
    int sc = c->nscope[L];
    if (sc >= 0 && sc < c->nscopes) per_scope[sc]++;
  }
  for (int L = 0; L < n0; L++) {
    int bp = numbered_params_node_of(c, L);
    if (bp < 0) continue;
    int sc = c->nscope[L];
    if (sc < 0 || sc >= c->nscopes || per_scope[sc] < 2) continue;   /* no collision */
    int maxn = (int)nt_int(nt, bp, "maximum", 0);
    if (maxn <= 0 || maxn > 9) continue;
    int body = nt_ref(nt, L, "body");
    for (int k = 1; k <= maxn; k++) {
      char from[8], to[32], key[8];
      snprintf(from, sizeof from, "_%d", k);
      snprintf(to, sizeof to, "_%d__b%d", k, L);
      snprintf(key, sizeof key, "n%d", k);
      if (body >= 0) numbered_rename_reads(nt, body, from, to, 0);
      numbered_rename_locals_str(nt, L, from, to);
      /* nt_node_set_str, not nt_set_str: the latter only UPDATES a key the node
         already carries and silently drops a new one. */
      nt_node_set_str(nt, bp, key, to);
    }
  }
  free(per_scope);
}

/* Every parameter kind that binds a name: a block parameter or block-local
   shadowing any of them needs its own slot, not just a shadowed required
   parameter. Counting only RequiredParameterNode left `def m(q = 7)` with a
   `|x; q|` block writing the method's q, and `*q` / `**q` / `&q` sharing the
   C slot with a different C type, which did not compile. */
static int is_name_binding_param(const char *ty) {
  return ty && (sp_streq(ty, "RequiredParameterNode") ||
                sp_streq(ty, "OptionalParameterNode") ||
                sp_streq(ty, "RestParameterNode") ||
                sp_streq(ty, "RequiredKeywordParameterNode") ||
                sp_streq(ty, "OptionalKeywordParameterNode") ||
                sp_streq(ty, "KeywordRestParameterNode") ||
                sp_streq(ty, "BlockParameterNode"));
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
  /* All local-variable-write and parameter nodes, collected once;
     the per-param collision scan iterates this set rather than all n nodes (it
     re-reads each name fresh, so a rename made earlier in this pass is still
     reflected). */
  int *wp = malloc((size_t)n * sizeof(int));
  int wpn = 0;
  if (!wp) { free(owner); free(inbody); return; }
  for (int w = 0; w < n; w++) {
    const char *wty = nt_type(nt, w);
    if (lv_node_is_write(wty) || is_name_binding_param(wty)) wp[wpn++] = w;
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
    const char *pty = pn >= 0 ? nt_type(nt, pn) : NULL;
    /* Numbered parameters are renamed by scope_numbered_block_params, which
       runs just before this and keys on the NumberedParametersNode; this pass
       only ever sees an ordinary ParametersNode -- or no parameters at all,
       for a block whose only names are its body's own locals. */
    if (pn >= 0 && (!pty || !sp_streq(pty, "ParametersNode"))) continue;
    int rn = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &rn) : NULL;
    /* rest / post parameters shadow-rename the same way as the requireds: a
       rest param sharing an outer local's name shares its C slot, so the
       rest-packing assignment aliases the outer variable (a sole-rest block
       whose name matches the inlined method's own param read garbage). */
    int extras[129]; int ne = 0;
    if (pn >= 0) {
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
       string; a block may carry them with no required params (`{ |; x| ... }`),
       and the parser lists a body's own locals there too: a name first
       assigned inside the block, which Ruby scopes to the block even when the
       enclosing scope assigns the same name further down. A paramless block
       (`Thread.new do ... end`) was skipped here, so its local shared the
       enclosing slot with that later assignment -- one cell for every thread
       (found under #4528). */
    const char *locs = nt_str(nt, L, "locals");
    int have_locals = locs && *locs;
    if (rn == 0 && ne == 0 && !have_locals) continue;
    /* An EMPTY body (`tap do |a| end`) still binds the parameter, so it still
       collides: skipping it left the two names sharing one C slot, and the
       slot took the inner block's type while the outer block assigned its own
       (#4027). There is simply nothing inside to rewrite -- both helpers below
       take a negative body as the empty subtree it is. */
    int body = nt_ref(nt, L, "body");
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
        int is_param_node = is_name_binding_param(wty);
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
      /* `Proc#parameters` reports the name the program wrote, and the emitter
         recovers it by stripping this suffix -- but a stripped name that
         appears nowhere else is not in the generated symbol table, so interning
         it at emit time came too late and it rendered as the empty symbol
         (#4045). Put it in the table here, while the table is still open. */
      comp_sym_intern(c, oldn);
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

/* Set by parse_seed_type when the token carried RBS's trailing `?`. An int or
   float pin keeps the unboxed kind and spells nil with its reserved sentinel,
   so every site that BOXES such a slot has to know -- otherwise the sentinel
   goes out as an ordinary number (a Hash key that misses a literal nil, #3493).
   Recorded here because the '?' is gone by the time the type is returned. */
static int g_seed_nilable;
static TyKind parse_seed_type(Compiler *c, const char *tok) {
  g_seed_nilable = 0;
  if (!tok || !*tok) return TY_UNKNOWN;
  size_t n = strlen(tok);
  char buf[128];
  if (n >= sizeof buf) return TY_UNKNOWN;
  memcpy(buf, tok, n + 1);
  /* A trailing '?' is RBS's nilable form (`Integer?`, `bool?`). Pinning it to
     the base type is right only where that type's C slot still has an
     inhabitant left to spell nil with: a pointer kind uses NULL, and int /
     float / string carry a reserved sentinel every nil? / to_s / boxing site
     already tests for. `sp_bool` and `sp_sym` have none -- 0 is `false`, and
     symbol 0 is a real symbol -- so a `bool?` / `Symbol?` pin has nowhere to
     put nil and collapses it onto false / :"" (#3412). Those pin to the tagged
     union, which is what `bool | nil` means anyway. */
  int nilable = 0;
  if (n > 0 && buf[n - 1] == '?') { buf[--n] = '\0'; nilable = 1; g_seed_nilable = 1; }
  if (sp_streq(buf, "int"))    return TY_INT;
  if (sp_streq(buf, "float"))  return TY_FLOAT;
  if (sp_streq(buf, "string") || sp_streq(buf, "str")) return TY_STRING;
  if (sp_streq(buf, "symbol") || sp_streq(buf, "sym")) return nilable ? TY_POLY : TY_SYMBOL;
  if (sp_streq(buf, "bool"))   return nilable ? TY_POLY : TY_BOOL;
  /* `singleton(X)` and unions of them: a Class value, which has no nil of
     its own, so the nilable form is the boxed one */
  if (sp_streq(buf, "class"))  return nilable ? TY_POLY : TY_CLASS;
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
    /* obj_X_ptr_array (`Array[X]`) is not a pin: see seed_obj_array_class */
    int ci = seed_class_index(c, buf + 4);
    return ci >= 0 ? ty_object(ci) : TY_UNKNOWN;
  }
  return TY_UNKNOWN;
}

/* `Array[Vec]` arrives as obj_Vec_ptr_array: the homogeneous pointer array
   narrow_object_arrays derives from the uses. It cannot be pinned the way a
   scalar seed is -- the unboxed form has emitters for only a few operations
   (index, push, length, ...), and a pin on a slot that is also iterated or
   handed out would leave codegen with no arm -- so the ivar seed arm records
   it as a request the pass answers, and every other arm drops it as before
   (#4444). Answers the element class, or -1 for any other token. */
/* `Array[Array[Integer]]` / `Array[Array[Float]]` arrive as int_array_array /
   float_array_array. Like obj_X_ptr_array they name a kind only
   narrow_object_arrays produces, so they are requests rather than pins -- and
   here the distinction is not a nicety. parse_seed_type would have to answer
   TY_POLY_ARRAY for them (the tags map to no scalar kind), which PINS the ivar,
   and a pinned ivar is skipped by the very pass that would have narrowed it: a
   correct declaration made the program slower, silently. Answers the request,
   or 0 for any other token. */
static int seed_nested_array_req(const char *tok) {
  if (!tok) return 0;
  if (sp_streq(tok, "int_array_array"))   return SEED_OA_INT_TABLE;
  if (sp_streq(tok, "float_array_array")) return SEED_OA_FLT_TABLE;
  return 0;
}

static int seed_obj_array_class(Compiler *c, const char *tok) {
  size_t n = tok ? strlen(tok) : 0;
  char buf[128];
  if (n < 15 || n >= sizeof buf || strncmp(tok, "obj_", 4) != 0) return -1;
  memcpy(buf, tok, n + 1);
  if (!sp_streq(buf + n - 10, "_ptr_array")) return -1;
  buf[n - 10] = '\0';
  return seed_class_index(c, buf + 4);
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

/* Does every value `id` can produce name a class (a constant that is one of
   the program's classes)? The class names met are appended to `names`. */
static int value_leaves_are_classes(Compiler *c, int id, char *names, size_t cap, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0 || depth > 32) return 0;
  switch (nt_kind(nt, id)) {
  case NK_StatementsNode: {
    int n = 0; const int *b = nt_arr(nt, id, "body", &n);
    return b && n > 0 && value_leaves_are_classes(c, b[n - 1], names, cap, depth + 1);
  }
  case NK_ParenthesesNode:
    return value_leaves_are_classes(c, nt_ref(nt, id, "body"), names, cap, depth + 1);
  case NK_IfNode: case NK_UnlessNode: {
    int els = nt_ref(nt, id, nt_kind(nt, id) == NK_IfNode ? "subsequent" : "else_clause");
    return value_leaves_are_classes(c, nt_ref(nt, id, "statements"), names, cap, depth + 1) &&
           value_leaves_are_classes(c, els, names, cap, depth + 1);
  }
  case NK_ElseNode:
    return value_leaves_are_classes(c, nt_ref(nt, id, "statements"), names, cap, depth + 1);
  case NK_ConstantReadNode: case NK_ConstantPathNode: {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || comp_class_index(c, nm) < 0) return 0;
    if (!strstr(names, nm) && strlen(names) + strlen(nm) + 3 < cap) {
      if (*names) strcat(names, ", ");
      strcat(names, nm);
    }
    return 1;
  }
  default:
    return 0;
  }
}

/* A return seed naming an instance type (`-> Story`, or a union of them)
   on a method whose every value is a class (`def searched_model = Story`):
   the RBS means `singleton(Story)`. Pinned as it stands, a single class
   declared the C return as `sp_Story *` and returned the sp_Class into it,
   which the C compiler rejected without a word about the signature; a union
   compiles (both are boxed) but types every call on the value from the wrong
   side. Answers 1 when the seed must be dropped. */
static int seed_ret_contradicts_class_body(Compiler *c, Scope *s, TyKind rt) {
  if (!ty_is_object(rt) && rt != TY_POLY) return 0;
  if (s->body < 0) return 0;
  char names[256] = "";
  if (!value_leaves_are_classes(c, s->body, names, sizeof names, 0)) return 0;
  const NodeTable *nt = c->nt;
  int si = (int)(s - c->scopes);
  NT_FOREACH_KIND(nt, NK_ReturnNode, rid) {
    if (c->nscope[rid] != si) continue;
    int an = nt_ref(nt, rid, "arguments"), n = 0;
    const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &n) : NULL;
    if (!av || n != 1 || !value_leaves_are_classes(c, av[0], names, sizeof names, 0)) return 0;
  }
  const char *cls = s->class_id >= 0 && s->class_id < c->nclasses ? c->classes[s->class_id].name : NULL;
  fprintf(stderr, "spinel: warning: --rbs: %s%s%s is declared to return an instance, but it returns "
                  "the class itself (%s); write singleton(...) in the signature. %s\n",
          cls ? cls : "", cls ? (s->is_cmethod ? "." : "#") : "", s->name ? s->name : "?", names,
          ty_is_object(rt) ? "The declaration is ignored."
                           : "Calls on its value are typed from the declaration.");
  return ty_is_object(rt);
}

/* Pin scope `s`'s return and each named parameter to its seeded type. ptypes
   is a comma-separated, param-index-aligned list (empty fields preserved so a
   skipped middle param doesn't shift the rest). */
static void seed_method(Compiler *c, Scope *s, const char *ret_tok, char *ptypes) {
  if (!s) return;
  TyKind rt = parse_seed_type(c, ret_tok);
  int nilable_seed = g_seed_nilable;
  if (rt != TY_UNKNOWN && seed_ret_contradicts_class_body(c, s, rt)) rt = TY_UNKNOWN;
  g_seed_nilable = nilable_seed;
  if (rt != TY_UNKNOWN) {
    s->ret = rt; s->ret_rbs_seeded = 1;
    /* `String?` is as nilable as `Integer?`: a bare `const char *` slot
       carries nil as NULL, so the seed has to record it or the implicit
       nil arm returns the empty string instead (#4250). */
    s->ret_rbs_nilable = g_seed_nilable &&
                         (rt == TY_INT || rt == TY_FLOAT || rt == TY_STRING);
    /* A memoized CONTAINER reader (`def self.table; @table ||= {}; end`) IS
       its ivar: pinning only the return left the slot poly, and the seeded
       narrowing then cast a PolyPolyHash payload to the declared hash -- a
       segfault on the first read (#3779). Pin the ivar the body answers too.
       Containers only: a scalar slot pinned this way loses the nil a boxed
       reader legitimately answers. */
    if ((ty_is_hash(rt) || ty_is_array(rt)) && !g_seed_nilable &&
        s->class_id >= 0 && s->class_id < c->nclasses && s->body >= 0) {
      const NodeTable *nt = c->nt;
      int bn = 0; const int *bb = nt_arr(nt, s->body, "body", &bn);
      if (bb && bn > 0) {
        int last = bb[bn - 1];
        NodeKind lk = nt_kind(nt, last);
        if (lk == NK_InstanceVariableOrWriteNode || lk == NK_InstanceVariableReadNode) {
          const char *ivn = nt_str(nt, last, "name");
          if (ivn && ivn[0] == '@') {
            ClassInfo *ci = &c->classes[s->class_id];
            int idx = comp_ivar_intern(ci, ivn);
            ci->ivar_types[idx] = rt;
            class_pin_ivar(ci, ivn);
          }
        }
      }
    }
  }
  if (!ptypes) return;
  char *p = ptypes;
  int pi = 0;
  while (p && pi < s->nparams) {
    char *comma = strchr(p, ',');
    if (comma) *comma = '\0';
    TyKind pt = parse_seed_type(c, p);
    if (pt != TY_UNKNOWN) {
      LocalVar *lv = scope_local(s, s->pnames[pi]);
      if (lv) {
        lv->type = pt; lv->rbs_seeded = 1; lv->rbs_type = pt;
        /* a nilable int parameter holds the sentinel like any other: mark it
           so boxing it answers nil rather than INTPTR_MIN */
        if (g_seed_nilable && (pt == TY_INT || pt == TY_FLOAT)) lv->nullable_int = 1;
      }
    }
    pi++;
    if (!comma) break;
    p = comma + 1;
  }
}

static int is_empty_array_literal(const NodeTable *nt, int id, int cap);
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
      int oac = seed_obj_array_class(c, a2);
      int nreq = oac >= 0 ? 0 : seed_nested_array_req(a2);
      TyKind t = (oac >= 0 || nreq) ? TY_UNKNOWN : parse_seed_type(c, a2);
      if (oac >= 0 || nreq) {   /* a request for narrow_object_arrays, not a pin */
        char ivn[300];
        snprintf(ivn, sizeof ivn, "%s%s", a1[0] == '@' ? "" : "@", a1);
        int idx = comp_ivar_intern(&c->classes[cur_ci], ivn);
        c->classes[cur_ci].ivar_oa_seed[idx] = nreq ? nreq : oac + 1;
        /* `@t = []` under a nested seed: the empty literal's default kind is
           the int array, which would type the ivar as one and keep it out of
           the narrowing pass altogether. The seed says the literal is the
           table, so it starts as the poly array the pass narrows (#4484). */
        if (nreq && c->arr_want) {
          const NodeTable *nt = c->nt;
          NT_FOREACH_KIND(nt, NK_InstanceVariableWriteNode, wid) {
            const char *wn = nt_str(nt, wid, "name");
            int wv = nt_ref(nt, wid, "value");
            if (!wn || !sp_streq(wn, ivn) || !is_empty_array_literal(nt, wv, c->node_cap)) continue;
            Scope *wsc = comp_scope_of(c, wid);
            if (!wsc || wsc->class_id != cur_ci) continue;
            if (c->arr_want[wv] == TY_UNKNOWN) c->arr_want[wv] = TY_POLY_ARRAY;
          }
        }
      }
      else if (t != TY_UNKNOWN) {
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
    /* Several block params autosplat the state: `produce([0, 1]) { |a, b| }`
       binds a = state[0], b = state[1] (#3582). */
    int bpn2 = nt_ref(nt, blk, "parameters");
    int preqn = 0;
    if (bpn2 >= 0) {
      int pp2 = nt_ref(nt, bpn2, "parameters");
      if (pp2 >= 0) nt_arr(nt, pp2, "requireds", &preqn);
    }
    int loopbodyarr[10];
    int nlb = 0;
    loopbodyarr[nlb++] = yshl;
    if (preqn > 1 && preqn <= 8) {
      for (int k = 0; k < preqn; k++) {
        const char *pk = block_param_name(c, blk, k);
        if (!pk) break;
        int idxc = te_call(nt, te_lvread(nt, "__pv"), "[]", te_args1(nt, te_int(nt, k)), -1);
        loopbodyarr[nlb++] = te_lvwrite(nt, pk, idxc);
      }
    }
    else loopbodyarr[nlb++] = te_lvwrite(nt, pname, te_lvread(nt, "__pv"));
    int beginn = nt_new_node(nt, "BeginNode"); nt_node_set_ref(nt, beginn, "statements", bbody);
    int pv_w2 = te_lvwrite(nt, "__pv", beginn);
    loopbodyarr[nlb++] = pv_w2;
    int loopbody = nt_new_node(nt, "StatementsNode"); nt_node_set_arr(nt, loopbody, "body", loopbodyarr, nlb);
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
    /* marked for the boxed-receiver dispatch, which answers a blockless call
       to the synthesized method with an Enumerator, as the typed receiver does */
    nt_node_set_str(nt, def, "synth", "struct");
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
        /* ONE argument, the [name, value] pair: CRuby's each_pair yields the
           pair, so a single-parameter block sees it whole and a two-parameter
           one destructures it. Yielding two values gave the 1-param form only
           the name. */
        int ya[2] = { sy, ivr };
        int pair = nt_new_node(nt, "ArrayNode");
        nt_node_set_arr(nt, pair, "elements", ya, 2);
        int yargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, yargs, "arguments", &pair, 1);
        int yn = nt_new_node(nt, "YieldNode");
        nt_node_set_ref(nt, yn, "arguments", yargs);
        pst[pn++] = yn;
      }
      pst[pn++] = nt_new_node(nt, "SelfNode");
      int pbody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, pbody, "body", pst, pn);
      int pdef = nt_new_node(nt, "DefNode");
      nt_node_set_str(nt, pdef, "name", "each_pair");
      nt_node_set_str(nt, pdef, "synth", "struct");
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

/* Does `class <ci>`'s body say `include Enumerable`? A builtin module has no
   class-table entry, so the AST is what records it (#3755). */
/* The names of the classes whose body includes Enumerable, from one walk of
   the ClassNodes; asked per enum call site (desugar_builtin_enum_calls,
   narrow_object_arrays), the walk of every node per ask was quadratic
   (rubys/roundhouse#72). Rebuilt whenever the node table changes. */
static ANameHash g_enum_cls;
static const NodeTable *g_enum_cls_nt;
static int g_enum_cls_cnt = -1;
static int class_body_includes_enumerable(const NodeTable *nt, int id);
int an_class_includes_enumerable(Compiler *c, int ci) {
  const NodeTable *nt = c->nt;
  if (ci < 0 || ci >= c->nclasses) return 0;
  const char *cn = c->classes[ci].name;
  if (!cn) return 0;
  /* Keyed on the class declarations, not the table's version: the pass
     that asks most (desugar_builtin_enum_calls) rewrites nodes between asks,
     and a version key rebuilt the set once per rewrite -- K rebuilds of K
     classes (rubys in #5035). An include comes only from a class body, and
     no pass adds one to an existing body; a new class grows the table. */
  int ncls_now = c->nclasses;   /* not the kind list: asking it rebuilds the
                                    kind index after every rewrite */
  if (g_enum_cls_nt != nt || g_enum_cls_cnt != ncls_now) {
    anh_free(&g_enum_cls); memset(&g_enum_cls, 0, sizeof g_enum_cls);
    NT_FOREACH_KIND(nt, NK_ClassNode, id) {
      if (!class_body_includes_enumerable(nt, id)) continue;
      int cp = nt_ref(nt, id, "constant_path");
      const char *n = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, id, "name");
      if (n && !anh_has(&g_enum_cls, n)) anh_add(&g_enum_cls, n);
    }
    g_enum_cls_nt = nt; g_enum_cls_cnt = ncls_now;
  }
  return anh_has(&g_enum_cls, cn);
}
static int class_body_includes_enumerable(const NodeTable *nt, int id) {
  int body = nt_ref(nt, id, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  for (int k = 0; k < bn; k++) {
    if (nt_kind(nt, bb[k]) != NK_CallNode) continue;
    const char *nm = nt_str(nt, bb[k], "name");
    if (!nm || !sp_streq(nm, "include") || nt_ref(nt, bb[k], "receiver") >= 0) continue;
    int an = nt_ref(nt, bb[k], "arguments");
    int n2 = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &n2) : NULL;
    for (int j = 0; j < n2; j++) {
      const char *mn = nt_str(nt, av[j], "name");
      if (mn && sp_streq(mn, "Enumerable")) return 1;
    }
  }
  return 0;
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
        !(m->yields || (m->blk_param && m->blk_param[0]) ||
          /* a class that mixes Enumerable in still gets the materializer even
             when its #each never yields: the mixin is simply empty (#3755) */
          an_class_includes_enumerable(c, m->class_id))) continue;
    if (comp_method_in_class(c, m->class_id, "__enum_to_a") >= 0) continue;
    int dup = 0;
    for (int k = 0; k < ncls; k++) if (cls[k] == m->class_id) { dup = 1; break; }
    if (dup) continue;
    cls[ncls] = m->class_id; eachdef[ncls] = m->def_node; ncls++;
  }
  for (int k = 0; k < ncls; k++) {
    /* `each` may yield more than one value per element (`yield k, v`), and
       Enumerable packs those into one array element. The collector block takes
       as many parameters as the widest yield and pushes them as an array; with
       a single parameter it saw only the first value (#3754). */
    int yarity = 1;
    { int esi = comp_method_in_class(c, cls[k], "each");
      if (esi >= 0)
        for (int nid = 0; nid < nt->count; nid++) {
          if (c->nscope[nid] != esi) continue;
          const char *ynt = nt_type(nt, nid);
          if (!ynt || !sp_streq(ynt, "YieldNode")) continue;
          int ya = nt_ref(nt, nid, "arguments");
          int yn2 = 0;
          if (ya >= 0) nt_arr(nt, ya, "arguments", &yn2);
          if (yn2 > yarity) yarity = yn2;
        }
      /* The same method written with an explicit block parameter drives it
         through `block.call(k, v)` rather than `yield`: count those sites too,
         or the collector took one parameter and every element arrived as the
         first value alone (#3792). */
      if (esi >= 0 && c->scopes[esi].blk_param && c->scopes[esi].blk_param[0]) {
        const char *bpn = c->scopes[esi].blk_param;
        for (int nid = 0; nid < nt->count; nid++) {
          if (c->nscope[nid] != esi) continue;
          if (nt_kind(nt, nid) != NK_CallNode) continue;
          const char *cnm2 = nt_str(nt, nid, "name");
          if (!cnm2 || (!sp_streq(cnm2, "call") && !sp_streq(cnm2, "yield") &&
                        !sp_streq(cnm2, "()") && !sp_streq(cnm2, "[]"))) continue;
          int crv = nt_ref(nt, nid, "receiver");
          if (crv < 0 || nt_kind(nt, crv) != NK_LocalVariableReadNode) continue;
          const char *crn = nt_str(nt, crv, "name");
          if (!crn || !sp_streq(crn, bpn)) continue;
          int ca2 = nt_ref(nt, nid, "arguments");
          int cn2 = 0;
          if (ca2 >= 0) nt_arr(nt, ca2, "arguments", &cn2);
          if (cn2 > yarity) yarity = cn2;
        }
      }
      if (yarity > 8) yarity = 8;
      c->classes[cls[k]].enum_yield_arity = yarity;
    }
    int arr = nt_new_node(nt, "ArrayNode");
    nt_node_set_arr(nt, arr, "elements", NULL, 0);
    int accw = nt_new_node(nt, "LocalVariableWriteNode");
    nt_node_set_str(nt, accw, "name", "__enum_acc");
    nt_node_set_ref(nt, accw, "value", arr);
    char enames[8][16];
    int eps[8];
    for (int q = 0; q < yarity; q++) {
      if (q == 0) snprintf(enames[q], sizeof enames[q], "__enum_e");
      else snprintf(enames[q], sizeof enames[q], "__enum_e%d", q);
      eps[q] = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, eps[q], "name", enames[q]);
    }
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", eps, yarity);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int accr1 = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, accr1, "name", "__enum_acc");
    int ereads[8];
    for (int q = 0; q < yarity; q++) {
      ereads[q] = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, ereads[q], "name", enames[q]);
    }
    int eread;
    if (yarity == 1) eread = ereads[0];
    else {
      eread = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, eread, "elements", ereads, yarity);
    }
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
    for (int q = 0; q < yarity; q++) {
      LocalVar *ev = scope_local_intern(ms, enames[q]);
      if (ev) ev->is_block_param = 1;
    }
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
  /* A short-circuiting Enumerable call is served by the same generator: the
     redirect below routes `obj.first`/`take`/`find`/`take_while`/`include?`/
     `lazy` through `__to_enum_each` so a class whose #each never ends still
     answers (#3756). Seed "each" whenever such a call is in the program; an
     unused helper prunes by reachability like every other. */
  for (int id = 0; id < n0 && nnames == 0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    if (sp_streq(cn, "first") || sp_streq(cn, "take") || sp_streq(cn, "find") ||
        sp_streq(cn, "detect") || sp_streq(cn, "take_while") || sp_streq(cn, "lazy") ||
        sp_streq(cn, "include?") || sp_streq(cn, "member?"))
      names[nnames++] = strdup("each");
  }
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
    "reduce", "inject", "flat_map", "collect_concat",
    "any?", "all?", "none?", "one?", "take", "drop", "take_while", "drop_while",
    "filter_map", "partition", "group_by", "each_with_object", "tally",
    "find_all", "zip", "grep", "grep_v", "to_h", "uniq", "reverse",
    "member?", "each_with_index", "join", "index", "each",
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
        /* both calls name the receiver node: the poly replace arm reads this
           to bind it once (codegen_call_recv.c) */
        nt_node_set_str(nt, id, "bang_splice", "1");
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
          /* Hash[] is laxer about pair shape than Array#to_h: a one-element
             sub-array gives a nil value rather than raising */
          nt_node_set_int(nt, id, "hash_brackets", 1);
          continue;
        }
        /* Hash[arg] with any single non-literal argument: the same pairs.to_h
           rewrite (this desugar runs pre-type, so no array check is possible;
           to_h itself raises on a non-convertible receiver, as Hash[] does). */
        if (an == 1) {
          nt_node_set_str(nt, id, "name", "to_h");
          nt_node_set_ref(nt, id, "receiver", av0[0]);
          nt_node_set_ref(nt, id, "arguments", -1);
          nt_node_set_int(nt, id, "hash_brackets", 1);
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
    /* str.each_char/each_line/each_byte(blockless, no args).with_index ->
       chars/lines/bytes.each.with_index, so the pair types as an array element
       plus an int index. Only each_char was rewritten, so the other two kept an
       opaque enumerator element and a BOXED index: passed to anything wanting
       an Integer -- a Struct field another site builds with a literal -- the
       boxed value met an sp_int parameter and the build failed (#3939). */
    if (sp_streq(nm, "with_index") && nt_ref(nt, recv, "arguments") < 0 &&
        (sp_streq(rn, "each_char") || sp_streq(rn, "each_line") || sp_streq(rn, "each_byte"))) {
      nt_node_set_str(nt, recv, "name",
                      sp_streq(rn, "each_char") ? "chars" :
                      sp_streq(rn, "each_line") ? "lines" : "bytes");
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

/* How many positional arguments the program passes a `method(:sym)` value
   through Method#call / #() / #[] / #===: the count when every such site
   agrees, -1 when there is none or they differ (a splat, a keyword hash or
   a block at a site is "differ" too). The receiver is resolved the way the
   Method machinery resolves it (method_recv_node), so `m = s.method(:x);
   m.call(1, 2)` counts with `s.method(:x).call(1, 2)`. A synthesized
   wrapper takes that many parameters and forwards them: with the one
   __bam_r parameter it had, `[3, 1, 2].method(:rotate).call(2)` answered
   rotate's default (the argument fell off the C cast on a native target,
   and is a signature-mismatch trap on wasm32). */
static int bam_call_argc(Compiler *c, int mnode) {
  const NodeTable *nt = c->nt;
  int n = -1;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !(sp_streq(nm, "call") || sp_streq(nm, "[]") || sp_streq(nm, "==="))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || method_recv_node(c, recv) != mnode) continue;
    if (nt_ref(nt, id, "block") >= 0) return -1;
    int argsn = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = argsn >= 0 ? nt_arr(nt, argsn, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++) {
      const char *aty = av[k] >= 0 ? nt_type(nt, av[k]) : NULL;
      if (!aty || sp_streq(aty, "SplatNode") || sp_streq(aty, "KeywordHashNode") ||
          sp_streq(aty, "BlockArgumentNode") || sp_streq(aty, "ForwardingArgumentsNode")) return -1;
    }
    if (n >= 0 && n != an) return -1;
    n = an;
  }
  return n;
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
      /* the wrapper takes what the call sites pass (bam_call_argc below),
         `def __bam_N(__bam_r, __bam_a, ...) = Integer(__bam_r, __bam_a, ...)`;
         one parameter when they disagree or there are none */
      int knf = bam_call_argc(c, id);
      if (knf < 1 || knf > 8) knf = 1;
      int kpn[8], kar[8];
      char kpnm[8][16];
      for (int k = 0; k < knf; k++) {
        if (k == 0) snprintf(kpnm[k], sizeof kpnm[k], "__bam_r");
        else if (k == 1) snprintf(kpnm[k], sizeof kpnm[k], "__bam_a");
        else snprintf(kpnm[k], sizeof kpnm[k], "__bam_a%d", k - 1);
        kpn[k] = nt_new_node(nt, "RequiredParameterNode");
        nt_node_set_str(nt, kpn[k], "name", kpnm[k]);
        kar[k] = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, kar[k], "name", kpnm[k]);
      }
      int kparams = nt_new_node(nt, "ParametersNode");
      nt_node_set_arr(nt, kparams, "requireds", kpn, knf);
      int kargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, kargs, "arguments", kar, knf);
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
      for (int k = 0; k < knf; k++) scope_add_param(kws, kpnm[k], -1);
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
    /* def __bam_<id>(__bam_r) = __bam_r.<sym>; for a binary operator
       def __bam_<id>(__bam_r, __bam_a) = __bam_r <op> __bam_a; and for a
       method whose call sites pass N arguments
       def __bam_<id>(__bam_r, __bam_a, __bam_a1, ...) = __bam_r.<sym>(__bam_a, ...) */
    int nfwd = binop ? 1 : bam_call_argc(c, id);
    if (nfwd < 0 || nfwd > 8) nfwd = 0;
    int preqs[9];
    int rp = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, rp, "name", "__bam_r");
    preqs[0] = rp;
    int params = nt_new_node(nt, "ParametersNode");
    char aname[8][16];
    for (int k = 0; k < nfwd; k++) {
      if (k == 0) snprintf(aname[k], sizeof aname[k], "__bam_a");
      else snprintf(aname[k], sizeof aname[k], "__bam_a%d", k);
      int ap = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, ap, "name", aname[k]);
      preqs[1 + k] = ap;
    }
    nt_node_set_arr(nt, params, "requireds", preqs, 1 + nfwd);
    int rread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, rread, "name", "__bam_r");
    int call = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, call, "name", sym);
    nt_node_set_ref(nt, call, "receiver", rread);
    if (nfwd > 0) {
      int areads[8];
      for (int k = 0; k < nfwd; k++) {
        areads[k] = nt_new_node(nt, "LocalVariableReadNode");
        nt_node_set_str(nt, areads[k], "name", aname[k]);
      }
      int cargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, cargs, "arguments", areads, nfwd);
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
    for (int k = 0; k < nfwd; k++) scope_add_param(ws, aname[k], -1);
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
  for (int id = comp_kind_first(c, NK_CallNode); id >= 0; id = comp_kind_next(c, id)) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    /* the shape first: only a call with a `*[...]` argument is rewritten, and
       the receiver's type (asked below) cost a lookup for every call, every
       round */
    { int argsn0 = nt_ref(nt, id, "arguments"), an0 = 0;
      const int *av0 = argsn0 >= 0 ? nt_arr(nt, argsn0, "arguments", &an0) : NULL;
      int lit = 0;
      for (int j = 0; j < an0 && !lit; j++)
        if (av0[j] >= 0 && nt_kind(nt, av0[j]) == NK_SplatNode) {
          int in0 = nt_ref(nt, av0[j], "expression");
          lit = in0 >= 0 && nt_kind(nt, in0) == NK_ArrayNode;
          break;
        }
      if (!lit) continue; }
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
    /* the identity predicates answer about the RANGE, not its members: routing
       them through to_a made `("a".."e").is_a?(Range)` false (#3619) */
    "is_a?", "kind_of?", "instance_of?", "nil?", "equal?", "respond_to?",
    /* to_a IS the materializer: interposing it would recurse */
    "to_a", "entries",
    /* Range#size counts integer elements: nil for a string range */
    "size",
    /* step / % walk the members by stride: their own arm answers. The block
       form has no arm, so it keeps riding the materialized array (#3671). */
    "step", "%", NULL };
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0) continue;
    if (infer_type(c, recv) != TY_STR_RANGE) continue;
    /* `range % n` is `range.step(n)`; the arithmetic emitter has no arm for a
       string range, so name it what it is (#3671) */
    if (sp_streq(nm, "%")) { nt_node_set_str(nt, id, "name", "step"); changed = 1; continue; }
    int argn = nt_ref(nt, id, "arguments");
    int an = 0;
    if (argn >= 0) nt_arr(nt, argn, "arguments", &an);
    int native = 0;
    for (int j = 0; range_native[j]; j++)
      if (sp_streq(nm, range_native[j])) { native = 1; break; }
    /* a block-driven step is `step(n).each { }`: the blockless arm answers the
       Enumerator, whose each the array path already drives (#3671) */
    if (native && sp_streq(nm, "step") && nt_ref(nt, id, "block") >= 0) {
      int inner = nt_new_node(nt, "CallNode");
      if (inner < 0) continue;
      nt_node_set_str(nt, inner, "name", "step");
      nt_node_set_ref(nt, inner, "receiver", recv);
      nt_node_set_ref(nt, inner, "arguments", argn);
      nt_node_set_ref(nt, inner, "block", -1);
      nt_node_set_ref(nt, id, "receiver", inner);
      nt_node_set_str(nt, id, "name", "each");
      nt_node_set_ref(nt, id, "arguments", -1);
      /* the Enumerator#each fold in desugar_enum_method_recv would turn this
         straight back into the block-driven step, every round (#4962) */
      nt_node_set_int(nt, id, "lowered_each", 1);
      comp_grow_node_arrays(c);
      c->nscope[inner] = c->nscope[id];
      changed = 1;
      continue;
    }
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
    /* Marks this block as SYNTHESIZED from a symbol argument, not written by
       the program: builtins/enumerable.rb's Ruby definition (0 extra args,
       a real block) would otherwise claim it too, once the seedless form
       drops its argument below and looks identical to a genuine 0-arg block
       call -- and its plain `yield` has no fold emitter's fallback for a
       symbol naming no real method (`[1, 2].inject(:nope)` must answer
       CRuby's NoMethodError, not fail the C build). See
       desugar_builtin_enum_calls's carve-out. */
    nt_node_set_int(nt, id, "sym_fold", 1);
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
  /* the rewrite cannot be taken back, so it waits for types that have
     settled: a local that looked like a Symbol in an optimistic round and
     widened after (`key = yield x` over nil and Symbols) was compared as a
     String against a Symbol and answered nil */
  if (g_infer_optimistic) return 0;
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
    /* `&p.to_proc` on a Proc is `&p`: to_proc is self there (#3687) */
    if (exn && sp_streq(exn, "to_proc")) {
      int tpr = nt_ref(nt, ex, "receiver");
      int targs = nt_ref(nt, ex, "arguments"); int tac = 0;
      if (targs >= 0) nt_arr(nt, targs, "arguments", &tac);
      if (tpr >= 0 && tac == 0 && infer_type(c, tpr) == TY_PROC) {
        nt_node_set_ref(nt, blk, "expression", tpr);
        changed = 1;
      }
      continue;
    }
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
/* An empty block body IS nil: `{ }` and `{ nil }` are the same block, and
   CRuby answers accordingly (`[1,2].filter_map {}` -> [], `.group_by {}` ->
   {nil=>[1,2]}). The iterator arms each read the body's tail to type their
   result, and most of them decline outright when there is none -- so the call
   fell through to the unresolved-call gate and reported the METHOD as
   undefined, which is the one thing it is not (#4006). Write the nil the block
   already means, once, rather than teaching a dozen arms to special-case it.
   The arms that DO have an empty-body path keep answering the same thing: a
   body of `nil` maps every element to nil, which is what they hard-coded. */
static int desugar_empty_block_body(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_BlockNode) continue;
    int body = nt_ref(nt, id, "body");
    if (body >= 0) {
      /* a `begin`/rescue body is not a StatementsNode and is never empty */
      if (nt_kind(nt, body) != NK_StatementsNode) continue;
      int bn = 0; nt_arr(nt, body, "body", &bn);
      if (bn > 0) continue;
    }
    int nilnode = nt_new_node(nt, "NilNode");
    if (nilnode < 0) continue;
    if (body < 0) {
      body = nt_new_node(nt, "StatementsNode");
      if (body < 0) continue;
      nt_node_set_ref(nt, id, "body", body);
    }
    nt_node_set_arr(nt, body, "body", &nilnode, 1);
    comp_grow_node_arrays(c);
    c->nscope[nilnode] = c->nscope[id];
    c->nscope[body] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

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
  /* An optional parameter's default is hoisted to the call site, which is
     argument position by another name: `def m(x, memo = Hash.new)` had no
     variable whose key usage could pick a variant either, and the call fell
     to the unresolved-call gate (#3878). */
  for (int id = 0; id < nt->count; id++) {
    { const char *pty = nt_type(nt, id);
      if (!pty || !sp_streq(pty, "OptionalParameterNode")) continue; }
    int a = nt_ref(nt, id, "value");
    if (a < 0 || nt_kind(nt, a) != NK_CallNode) continue;
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

/* `Array[a, b, c]` and `Range.new(lo, hi)` are the constructor spellings of the
   `[a, b, c]` and `(lo..hi)` literals, and both raised NoMethodError -- the
   literal worked and the documented constructor for the same value did not
   (#3485, #3486). Build the literal and carry it as the receiver of a marker
   call that inference and emission both see through, the way `Hash[k: v]`
   already reaches the hash literal. Everything downstream then types and emits
   these exactly as the literal form, with no second implementation to keep in
   step.

   Left alone when the program owns the constructor: reopening Array is fine,
   defining `Array.[]` means whatever it says. */
static int desugar_class_literal_ctors(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    const char *rn = nt_str(nt, recv, "name");
    const char *mn = nt_str(nt, id, "name");
    if (!rn || !mn) continue;
    int ca = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = ca >= 0 ? nt_arr(nt, ca, "arguments", &argc) : NULL;
    for (int k = 0; k < argc; k++)
      if (nt_kind(nt, argv[k]) == NK_SplatNode) { argc = -1; break; }
    if (argc < 0) continue;
    int ci = comp_class_index(c, rn);
    if (sp_streq(rn, "Array") && sp_streq(mn, "[]")) {
      if (ci >= 0 && comp_cmethod_in_chain(c, ci, "[]", NULL) >= 0) continue;
      int els[64];
      if (argc > (int)(sizeof els / sizeof els[0])) continue;
      for (int k = 0; k < argc; k++) els[k] = argv[k];
      nt_node_reset(nt, id, "ArrayNode");
      nt_node_set_arr(nt, id, "elements", els, argc);
    }
    else if (sp_streq(rn, "Range") && sp_streq(mn, "new") && (argc == 2 || argc == 3)) {
      if (ci >= 0 && comp_cmethod_in_chain(c, ci, "new", NULL) >= 0) continue;
      /* the exclusive flag is part of the literal's shape, so it has to be
         readable here; a computed third argument keeps today's behaviour */
      int excl = 0;
      if (argc == 3) {
        NodeKind ek = nt_kind(nt, argv[2]);
        if (ek == NK_TrueNode) excl = 1;
        else if (ek != NK_FalseNode && ek != NK_NilNode) continue;
      }
      int lo = argv[0], hi = argv[1];
      nt_node_reset(nt, id, "RangeNode");
      nt_node_set_ref(nt, id, "left", lo);
      nt_node_set_ref(nt, id, "right", hi);
      if (excl) nt_node_set_int(nt, id, "flags", 4);
    }
    else continue;
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
    /* `x.to_h { |..| pair }` on a value only known at run time is
       `x.map { |..| pair }.to_h`: Array#to_h, Hash#to_h and Enumerable#to_h
       with a block all build the hash from the block's pairs. The boxed
       dispatch has the blockless to_h and a boxed map, but no block form of
       to_h, so the call compiled to an unconditional NoMethodError naming the
       very class that defines it (#4838). A typed receiver keeps its own
       emitter; a program class with its own to_h keeps it too, since the
       boxed value may be one of those. */
    if (nm && sp_streq(nm, "to_h") && nt_kind(nt, nt_ref(nt, id, "block")) == NK_BlockNode &&
        nt_ref(nt, id, "arguments") < 0) {
      int trecv = nt_ref(nt, id, "receiver");
      int user_to_h = 0;
      for (int ci = 0; ci < c->nclasses && !user_to_h; ci++)
        if (comp_method_in_class(c, ci, "to_h") >= 0) user_to_h = 1;
      if (trecv >= 0 && !user_to_h && infer_type(c, trecv) == TY_POLY) {
        int mapc = nt_new_node(nt, "CallNode");
        if (mapc >= 0) {
          nt_node_set_str(nt, mapc, "name", "map");
          nt_node_set_ref(nt, mapc, "receiver", trecv);
          nt_node_set_ref(nt, mapc, "block", nt_ref(nt, id, "block"));
          nt_node_set_ref(nt, id, "receiver", mapc);
          nt_node_set_ref(nt, id, "block", -1);
          comp_grow_node_arrays(c);
          c->nscope[mapc] = c->nscope[id];
          changed = 1;
          continue;
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
          /* #each_entry answers the receiver, which #each does not do for an
             Enumerator; record it before the name is gone (#3591). A blockless
             `.each` receiver IS that Enumerator however its own type settled --
             over a Range it settles as the materialized element array, and the
             call then answered those elements (#3857). */
          int erecv_enum = ert == TY_ENUMERATOR;
          if (!erecv_enum && nt_kind(nt, erecv) == NK_CallNode &&
              nt_ref(nt, erecv, "block") < 0) {
            const char *ernm = nt_str(nt, erecv, "name");
            if (ernm && (sp_streq(ernm, "each") || sp_streq(ernm, "each_with_index") ||
                         sp_streq(ernm, "reverse_each")))
              erecv_enum = 1;
          }
          if (erecv_enum) nt_node_set_int(nt, id, "enum_self_result", erecv);
          nt_node_set_str(nt, id, "name", "each");
          changed = 1;
          continue;   /* `nm` was the name just replaced (freed): the arms below read it */
        }
      }
    }
    /* The block forms of each_slice / each_cons answer the receiver too, and
       over a blockless `.each` that receiver is the Enumerator (#3857). */
    if (nm && (sp_streq(nm, "each_slice") || sp_streq(nm, "each_cons")) &&
        nt_ref(nt, id, "block") >= 0 && nt_int(nt, id, "enum_self_result", -1) < 0) {
      int srecv = nt_ref(nt, id, "receiver");
      if (srecv >= 0 && nt_kind(nt, srecv) == NK_CallNode && nt_ref(nt, srecv, "block") < 0) {
        const char *srnm = nt_str(nt, srecv, "name");
        if (srnm && sp_streq(srnm, "each")) {
          nt_node_set_int(nt, id, "enum_self_result", srecv);
          changed = 1;
        }
      }
    }
    /* Enumerator.product(a, b) { blk } iterates the pairs and answers nil; the
       constructor arm builds the Enumerator, so drive it with #each (#3589) */
    if (nm && sp_streq(nm, "product") && nt_ref(nt, id, "block") >= 0) {
      int prv = nt_ref(nt, id, "receiver");
      if (prv >= 0 && nt_type(nt, prv) && sp_streq(nt_type(nt, prv), "ConstantReadNode") &&
          nt_str(nt, prv, "name") && sp_streq(nt_str(nt, prv, "name"), "Enumerator")) {
        int pargs = nt_ref(nt, id, "arguments");
        int pn2 = 0; if (pargs >= 0) nt_arr(nt, pargs, "arguments", &pn2);
        if (pn2 == 2 || pn2 == 3) {
          int inner = nt_new_node(nt, "CallNode");
          if (inner >= 0) {
            nt_node_set_str(nt, inner, "name", "product");
            nt_node_set_ref(nt, inner, "receiver", prv);
            nt_node_set_ref(nt, inner, "arguments", pargs);
            nt_node_set_ref(nt, inner, "block", -1);
            nt_node_set_ref(nt, id, "receiver", inner);
            nt_node_set_str(nt, id, "name", "each");
            nt_node_set_ref(nt, id, "arguments", -1);
            /* the block form answers nil, not the enumerator #each hands back */
            nt_node_set_int(nt, id, "nil_result", 1);
            comp_grow_node_arrays(c);
            c->nscope[inner] = c->nscope[id];
            changed = 1;
            continue;
          }
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
    if (nm && sp_streq(nm, "to_set") &&
        nt_ref(nt, id, "arguments") < 0 && comp_class_index(c, "Set") >= 0) {
      int recv = nt_ref(nt, id, "receiver");
      if (recv >= 0) {
        TyKind rt = infer_type(c, recv);
        int decline = 0;
        if (rt == TY_UNKNOWN && g_infer_optimistic) decline = 1;   /* not yet known */
        /* Set#to_set is the receiver itself, which is also what keeps a
           `to_set.to_set` chain from nesting one Set.new inside another (#3623) */
        else if (ty_is_object(rt) && ty_object_class(rt) == comp_class_index(c, "Set") &&
                 nt_ref(nt, id, "block") < 0) {
          nt_node_set_int(nt, id, "enum_self_result", recv);
          nt_node_set_str(nt, id, "name", "itself");
          nt_node_set_ref(nt, id, "arguments", -1);
          changed = 1;
          continue;
        }
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
            /* a block maps each element on the way in: Set.new's own block
               parameter does exactly that (#3623) */
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
      /* a block that breaks out of the sum answers the break value: the
         mapped copy would break out of the map and sum what it gave (#4918) */
      int sblk = nt_ref(nt, id, "block");
      int sbreaks = sblk >= 0 && nt_kind(nt, sblk) == NK_BlockNode &&
                    block_has_top_break(c, nt_ref(nt, sblk, "body"));
      if (srecv >= 0 && sac == 0 && !sbreaks && infer_type(c, srecv) == TY_POLY_ARRAY) {
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
    /* `arr.each_index.reverse_each { }` / `.each_with_index.reverse_each { }`:
       the blockless inner call answers an Enumerator, and reverse_each has no
       arm for one, so the whole chain was refused -- though `.each` on the
       same Enumerator works and `.to_a` on it answers the right elements.
       Interpose to_a, exactly as the Range chains below do, and the array
       machinery serves it (#4302). */
    if (nm && sp_streq(nm, "reverse_each")) {
      int rr = nt_ref(nt, id, "receiver");
      if (rr >= 0 && nt_kind(nt, rr) == NK_CallNode && nt_ref(nt, rr, "block") < 0 &&
          infer_type(c, rr) == TY_ENUMERATOR) {
        const char *rrn = nt_str(nt, rr, "name");
        int rra = nt_ref(nt, rr, "arguments"); int rrac = 0;
        if (rra >= 0) nt_arr(nt, rra, "arguments", &rrac);
        /* `each` belongs here with the index enumerators: it reaches the
           array machinery on its own, but then answers the array it walked
           rather than the Enumerator, and the marked hop below is what carries
           the receiver through (#4325). */
        if (rrn && rrac == 0 &&
            (sp_streq(rrn, "each_index") || sp_streq(rrn, "each_with_index") ||
             sp_streq(rrn, "each") || sp_streq(rrn, "each_entry"))) {
          int toa2 = nt_new_node(nt, "CallNode");
          nt_node_set_str(nt, toa2, "name", "to_a");
          nt_node_set_ref(nt, toa2, "receiver", rr);
          /* The block form of reverse_each answers the RECEIVER, and after this
             hop that is the interposed array rather than the Enumerator the
             program wrote (#4325). `enum_recv` is the marker the value emitter
             already reads for exactly this: it yields the marked hop's own
             receiver instead of the hop. */
          nt_node_set_str(nt, toa2, "enum_recv", "1");
          nt_node_set_ref(nt, id, "receiver", toa2);
          comp_grow_node_arrays(c);
          c->nscope[toa2] = c->nscope[id];
          changed = 1;
          continue;
        }
      }
    }
    /* `recv.<m>(args).each { blk }` IS `recv.<m>(args) { blk }`: Enumerator#each
       runs the method the Enumerator came from with that block, and answers
       what THAT method answers -- the receiver for each_with_index, the memo
       for each_with_object, the mapped array for map. Read instead as an
       iteration over the values the Enumerator yields, it answered those values
       (#4332), and over an each_with_index Enumerator it did not compile at all
       (#4331). `lazy` is left alone: its block form is not its each form. */
    if (nm && sp_streq(nm, "each") && nt_ref(nt, id, "block") >= 0 &&
        /* ...but never a synthesized one. `Enumerator.product(a, b) { blk }` is
           lowered to `Enumerator.product(a, b).each { blk }` above (#3589), and
           rewriting that back left the two rules undoing each other until the
           fixpoint gave up, with the block never run. The nil_result marker is
           what that lowering leaves behind, and `lowered_each` the string
           range's block-driven step (#4962). */
        !nt_int(nt, id, "nil_result", 0) && !nt_int(nt, id, "lowered_each", 0)) {
      int ea = nt_ref(nt, id, "arguments"); int eac = 0;
      if (ea >= 0) nt_arr(nt, ea, "arguments", &eac);
      int er = nt_ref(nt, id, "receiver");
      if (eac == 0 && er >= 0 && nt_kind(nt, er) == NK_CallNode &&
          nt_ref(nt, er, "block") < 0 && nt_ref(nt, er, "receiver") >= 0 &&
          infer_type(c, er) == TY_ENUMERATOR) {
        const char *ern = nt_str(nt, er, "name");
        if (ern && !sp_streq(ern, "lazy")) {
          nt_node_set_str(nt, id, "name", ern);
          nt_node_set_ref(nt, id, "receiver", nt_ref(nt, er, "receiver"));
          int ira = nt_ref(nt, er, "arguments");
          if (ira >= 0) nt_node_set_ref(nt, id, "arguments", ira);
          changed = 1;
          continue;
        }
      }
    }
    /* `arr.each_with_index.reduce { }` / `.inject { }`: the blockless inner
       call answers an Enumerator, and the fold read its elements as scalars --
       a seedless fold assigned a [value, index] pair into an sp_int and the C
       compiler refused it, while a fold answering the accumulator answered 0
       (#4321). Interpose to_a, exactly as the reverse_each sibling above does;
       the fold's answer is its own, so this hop needs no `enum_recv` marker. */
    if (nm && (sp_streq(nm, "reduce") || sp_streq(nm, "inject")) &&
        nt_ref(nt, id, "block") >= 0) {
      int fr = nt_ref(nt, id, "receiver");
      if (fr >= 0 && nt_kind(nt, fr) == NK_CallNode && nt_ref(nt, fr, "block") < 0 &&
          infer_type(c, fr) == TY_ENUMERATOR) {
        const char *frn = nt_str(nt, fr, "name");
        int fra = nt_ref(nt, fr, "arguments"); int frac = 0;
        if (fra >= 0) nt_arr(nt, fra, "arguments", &frac);
        if (frn && frac == 0 &&
            (sp_streq(frn, "each_index") || sp_streq(frn, "each_with_index"))) {
          int toa3 = nt_new_node(nt, "CallNode");
          nt_node_set_str(nt, toa3, "name", "to_a");
          nt_node_set_ref(nt, toa3, "receiver", fr);
          nt_node_set_ref(nt, id, "receiver", toa3);
          comp_grow_node_arrays(c);
          c->nscope[toa3] = c->nscope[id];
          changed = 1;
          continue;
        }
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
        /* the spelling the program wrote, for a NoMethodError on nil */
        nt_node_set_str(nt, id, "written_name", "yield");
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
    /* Blockless each_pair on a Struct answers an Enumerator over the
       [name, value] pairs, which is what the member hash's own blockless
       #each answers; the synthesized yielding each_pair raised LocalJumpError.
       Ahead of the Enumerable-name gate, which does not list each_pair (and
       must not: its block form yields pairs, not members). */
    if (nm && sp_streq(nm, "each_pair") && nt_ref(nt, id, "block") < 0) {
      int prv = nt_ref(nt, id, "receiver");
      TyKind prt = prv >= 0 ? infer_type(c, prv) : TY_UNKNOWN;
      int pcid = ty_is_object(prt) ? ty_object_class(prt) : -1;
      if (pcid >= 0 && pcid < c->nclasses && c->classes[pcid].is_struct) {
        int wrap = nt_new_node(nt, "CallNode");
        if (wrap >= 0) {
          nt_node_set_str(nt, wrap, "name", "to_h");
          nt_node_set_ref(nt, wrap, "receiver", prv);
          nt_node_set_ref(nt, wrap, "arguments", -1);
          nt_node_set_ref(nt, wrap, "block", -1);
          nt_node_set_ref(nt, id, "receiver", wrap);
          nt_node_set_str(nt, id, "name", "each");
          comp_grow_node_arrays(c);
          c->nscope[wrap] = c->nscope[id];
          changed = 1;
          continue;
        }
      }
    }
    if (!nm || !is_array_enum_method(nm)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) {
      /* `count` with no receiver inside another method of the same class: the
         redirect below keys on the receiver's type, and an implicit self has
         no receiver node at all, so an Enumerable call on self was left with
         nothing to resolve to. Give it one. */
      Scope *osc = comp_scope_of(c, id);
      int ocid = osc ? osc->class_id : -1;
      if (ocid < 0 || osc->is_cmethod) continue;
      /* a Data class includes no Enumerable in CRuby -- `to_a` inside one is a
         NameError, not its members */
      if (ocid < c->nclasses && c->classes[ocid].is_data) continue;
      if (comp_method_in_chain(c, ocid, "__enum_to_a", NULL) < 0) continue;
      if (comp_method_in_chain(c, ocid, nm, NULL) >= 0) continue;   /* the class's own */
      int selfn = nt_new_node(nt, "SelfNode");
      int wrap = nt_new_node(nt, "CallNode");
      if (selfn < 0 || wrap < 0) continue;
      nt_node_set_str(nt, wrap, "name", "__enum_to_a");
      nt_node_set_ref(nt, wrap, "receiver", selfn);
      nt_node_set_ref(nt, wrap, "arguments", -1);
      nt_node_set_ref(nt, wrap, "block", -1);
      nt_node_set_ref(nt, id, "receiver", wrap);
      comp_grow_node_arrays(c);
      c->nscope[selfn] = c->nscope[id];
      c->nscope[wrap] = c->nscope[id];
      changed = 1;
      continue;
    }
    /* find_all == select once the receiver is ARRAY-shaped (a hash receiver
       reaches here as the redispatched pair array, so the result is the
       Enumerable pair list, not a hash) */
    if (sp_streq(nm, "find_all") && nt_ref(nt, id, "block") >= 0 &&
        (ty_is_array(comp_ntype(c, recv)) || ty_is_array(infer_type(c, recv)))) {
      nt_node_set_str(nt, id, "name", "select");
      changed = 1;
      continue;
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
      /* bare `first` is `take(1)[0]`: the counted take lowers natively and
         stops after one element, so an infinite enumerator answers (#3756) */
      if (sp_streq(nm, "first")) {
        int fa = nt_ref(nt, id, "arguments"); int fac = 0;
        if (fa >= 0) nt_arr(nt, fa, "arguments", &fac);
        if (fac == 0 && rt == TY_ENUMERATOR) {
          int one = nt_new_node(nt, "IntegerNode");
          nt_node_set_int(nt, one, "value", 1);
          int targs = nt_new_node(nt, "ArgumentsNode");
          nt_node_set_arr(nt, targs, "arguments", &one, 1);
          int tk = nt_new_node(nt, "CallNode");
          nt_node_set_str(nt, tk, "name", "take");
          nt_node_set_ref(nt, tk, "receiver", recv);
          nt_node_set_ref(nt, tk, "arguments", targs);
          nt_node_set_ref(nt, tk, "block", -1);
          int zero = nt_new_node(nt, "IntegerNode");
          nt_node_set_int(nt, zero, "value", 0);
          int zargs = nt_new_node(nt, "ArgumentsNode");
          nt_node_set_arr(nt, zargs, "arguments", &zero, 1);
          nt_node_set_str(nt, id, "name", "[]");
          nt_node_set_ref(nt, id, "receiver", tk);
          nt_node_set_ref(nt, id, "arguments", zargs);
          comp_grow_node_arrays(c);
          c->nscope[one] = c->nscope[id]; c->nscope[targs] = c->nscope[id];
          c->nscope[tk] = c->nscope[id];
          c->nscope[zero] = c->nscope[id]; c->nscope[zargs] = c->nscope[id];
          changed = 1;
          continue;
        }
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
    /* the blockless grouping/cycling Enumerables answer an Enumerator of their
       own; delegating through to_a gives the Array forms, which are wired
       (#3604) */
    int enum_regroup = nt_ref(nt, id, "block") < 0 &&
        (sp_streq(nm, "each_slice") || sp_streq(nm, "each_cons") ||
         sp_streq(nm, "each_entry") || sp_streq(nm, "cycle") ||
         sp_streq(nm, "reverse_each"));
    /* find/detect/take_while are driven lazily through #next by their own
       emitter, so they terminate over an INFINITE enumerator; materializing
       the elements first would loop forever (#3590) */
    int enum_lazy_driven = nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(nm, "find") || sp_streq(nm, "detect") || sp_streq(nm, "take_while"));
    /* include?/member? stop at the first hit through the same driver (#3756) */
    if (!enum_lazy_driven && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(nm, "include?") || sp_streq(nm, "member?"))) {
      int ia = nt_ref(nt, id, "arguments"); int iac = 0;
      if (ia >= 0) nt_arr(nt, ia, "arguments", &iac);
      if (iac == 1) enum_lazy_driven = 1;
    }
    if (rt == TY_ENUMERATOR && !recv_is_index_enum && !enum_lazy_driven &&
        !sp_streq(nm, "to_a") && !sp_streq(nm, "entries") &&
        (nt_ref(nt, id, "block") >= 0 || enum_terminal || enum_regroup)) {
      /* the block forms of these answer the RECEIVING Enumerator, but the
         redirect makes the call read the materialized array, whose own
         each_cons/each_slice/each_entry answers that array (#3591) */
      if (nt_ref(nt, id, "block") >= 0 &&
          (sp_streq(nm, "each_slice") || sp_streq(nm, "each_cons") ||
           sp_streq(nm, "each_entry")))
        nt_node_set_int(nt, id, "enum_self_result", recv);
      int wrap = nt_new_node(nt, "CallNode");
      /* When the call answers its receiving Enumerator, that receiver has to
         STAY one: a blockless `.each` over a Range otherwise reads the to_a
         hop as its consumer and settles as the materialized element array,
         which the call then answered (#3857). */
      if (nt_int(nt, id, "enum_self_result", -1) >= 0)
        nt_node_set_str(nt, wrap, "enum_recv", "1");
      /* a statement `e.each { }` pulls the elements one at a time instead
         (codegen): an endless Enumerator has no array to read */
      if ((sp_streq(nm, "each") || sp_streq(nm, "each_with_index")) && nt_ref(nt, id, "block") >= 0)
        nt_node_set_str(nt, wrap, "enum_each_wrap", "1");
      nt_node_set_str(nt, wrap, "name", "to_a");
      nt_node_set_ref(nt, wrap, "receiver", recv);
      nt_node_set_ref(nt, id, "receiver", wrap);
      comp_grow_node_arrays(c);
      c->nscope[wrap] = c->nscope[id];
      changed = 1;
      continue;
    }
    /* An enumerable ARGUMENT gets the same treatment as the receiver: `a.zip(b)`
       where b is a user Enumerable handed the array path a raw instance
       pointer. Done before the receiver check so an Array receiver with such
       an argument is covered too. */
    if (sp_streq(nm, "zip") || sp_streq(nm, "product")) {
      int zargs = nt_ref(nt, id, "arguments");
      int zn = 0; const int *zav = zargs >= 0 ? nt_arr(nt, zargs, "arguments", &zn) : NULL;
      for (int zi = 0; zi < zn && zav; zi++) {
        int za = zav[zi];
        if (nt_kind(nt, za) == NK_CallNode) {
          const char *zn2 = nt_str(nt, za, "name");
          if (zn2 && sp_streq(zn2, "__enum_to_a")) continue;   /* idempotent */
        }
        TyKind zat = infer_type(c, za);
        if (!ty_is_object(zat)) continue;
        int zcid = ty_object_class(zat);
        if (zcid < 0 || comp_method_in_chain(c, zcid, "__enum_to_a", NULL) < 0) continue;
        int zwrap = nt_new_node(nt, "CallNode");
        if (zwrap < 0) continue;
        nt_node_set_str(nt, zwrap, "name", "__enum_to_a");
        nt_node_set_ref(nt, zwrap, "receiver", za);
        nt_node_set_ref(nt, zwrap, "arguments", -1);
        nt_node_set_ref(nt, zwrap, "block", -1);
        int newargs[8];
        if (zn > 8) break;
        for (int k = 0; k < zn; k++) newargs[k] = (k == zi) ? zwrap : zav[k];
        nt_node_set_arr(nt, zargs, "arguments", newargs, zn);
        comp_grow_node_arrays(c);
        c->nscope[zwrap] = c->nscope[id];
        changed = 1;
        zav = nt_arr(nt, zargs, "arguments", &zn);
      }
    }
    if (!ty_is_object(rt)) continue;
    /* each_with_index keeps its own object-receiver clone (builtins/
       enumerable.rb, via desugar_builtin_enum_calls's ty_is_object arm),
       which walks the TRUE receiver's own #each directly: this bridge's
       to_a materialization answered the MATERIALIZED ARRAY as the block
       form's `self` tail instead of the receiver itself (Nums.new(1,2,3)
       .each_with_index{}.class -> Array, not Nums). The TY_ENUMERATOR arm
       above this one still needs the name (`arr.each.each_with_index{}`,
       Enumerator's own native override), so it stays in
       is_array_enum_method; only THIS object-receiver wrap declines it. */
    if (sp_streq(nm, "each_with_index")) continue;
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
    /* The short-circuiting Enumerables stop before the source runs out, so
       they ride the fiber-backed generator rather than the eager element
       array: a class whose #each never ends materializes forever (#3756). */
    const char *matn = "__enum_to_a";
    if (comp_method_in_chain(c, cid, "__to_enum_each", NULL) >= 0 &&
        (sp_streq(nm, "first") || sp_streq(nm, "take") || sp_streq(nm, "lazy") ||
         ((sp_streq(nm, "find") || sp_streq(nm, "detect") || sp_streq(nm, "take_while")) &&
          nt_ref(nt, id, "block") >= 0) ||
         ((sp_streq(nm, "include?") || sp_streq(nm, "member?")) &&
          nt_ref(nt, id, "block") < 0)))
      matn = "__to_enum_each";
    int wrap = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, wrap, "name", matn);
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
/* A terminal that the lazy pipeline does not fuse -- sum, count, min, include?
   and friends -- runs on the materialized result instead: `lz.sum` becomes
   `lz.to_a.sum`. Without this the call had no lazy arm at all and answered nil
   or refused to compile (#3585). */
/* `a.upto(b)` over Strings is the String range `(a..b)`: the succ-based walk
   the range's own iteration already does. There was no arm for it at all
   (#3600). The Integer form has its own emitter and is left alone. */
/* `Enumerator.new { |y| src.each(&y) }`: the yielder passed as a block. It has
   no proc object -- `y << v` lowers to a Fiber.yield -- so drive the iterator
   with a literal block that pushes each element instead (#3587). */
static int desugar_yielder_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  /* The two walks below ran over every node once per generator, every round.
     Their candidates -- a call named to_proc, a call passed a block argument
     -- are collected once, ascending; each is still tested in full where it
     is used, since a rewrite for one generator (`&y.to_proc` -> `&y`) can
     make a node a candidate's match for the next. */
  int *tp_ids = NULL, ntp = 0, *ba_ids = NULL, nba = 0, has_gen = 0;
  for (int id = 0; id < n0 && !has_gen; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, "new") && nt_ref(nt, id, "block") >= 0) has_gen = 1;
  }
  if (!has_gen) return 0;
  tp_ids = malloc(sizeof(int) * (size_t)(n0 > 0 ? n0 : 1));
  ba_ids = malloc(sizeof(int) * (size_t)(n0 > 0 ? n0 : 1));
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, "to_proc")) tp_ids[ntp++] = id;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) ba_ids[nba++] = id;
  }
  for (int en = 0; en < n0; en++) {
    if (nt_kind(nt, en) != NK_CallNode) continue;
    const char *enm = nt_str(nt, en, "name");
    int erecv = nt_ref(nt, en, "receiver");
    if (!enm || !sp_streq(enm, "new") || erecv < 0) continue;
    if (nt_kind(nt, erecv) != NK_ConstantReadNode && nt_kind(nt, erecv) != NK_ConstantPathNode) continue;
    const char *ecn = nt_str(nt, erecv, "name");
    if (!ecn || !sp_streq(ecn, "Enumerator")) continue;
    int eblk = nt_ref(nt, en, "block");
    if (eblk < 0 || nt_kind(nt, eblk) != NK_BlockNode) continue;
    const char *yname = block_param_name(c, eblk, 0);
    int ebody = nt_ref(nt, eblk, "body");
    if (!yname || ebody < 0) continue;
    /* The yielder is a value of its own (the fiber under the Yielder id when
       a proc inside the body captures it), never a container: its `<<` is
       a Fiber.yield. Pin it boxed so the push evidence on `y << [x, i]` in
       such a proc cannot make it an array. */
    { Scope *ysc = comp_scope_of(c, eblk);
      LocalVar *ylv = ysc ? scope_local_intern(ysc, yname) : NULL;
      if (ylv && !ylv->rbs_seeded) { ylv->type = TY_POLY; ylv->rbs_seeded = 1; ylv->is_block_param = 1; } }
    /* `y.to_proc` names the same channel the yielder is: a call through it
       pushes, and `&y.to_proc` is `&y`. Neither had an arm, so the explicit
       spelling raised NoMethodError (#3844). Rewritten before the `&y` loop
       below so the block-argument form rides it. */
    for (int ti = 0; ti < ntp; ti++) {
      int id = tp_ids[ti];
      if (nt_kind(nt, id) != NK_CallNode) continue;
      const char *tnm = nt_str(nt, id, "name");
      if (!tnm || !sp_streq(tnm, "to_proc")) continue;
      int trecv = nt_ref(nt, id, "receiver");
      if (trecv < 0 || nt_kind(nt, trecv) != NK_LocalVariableReadNode) continue;
      const char *tvn = nt_str(nt, trecv, "name");
      if (!tvn || !sp_streq(tvn, yname)) continue;
      if (!a_subtree_contains(nt, ebody, id, 0)) continue;
      /* `&y.to_proc` -> `&y` */
      for (int ba = 0; ba < n0; ba++)
        if (nt_kind(nt, ba) == NK_BlockArgumentNode && nt_ref(nt, ba, "expression") == id) {
          nt_node_set_ref(nt, ba, "expression", trecv);
          changed = 1;
        }
      /* `y.to_proc.call(v)` / `.()` / `[v]` -> `y << v` */
      for (int cl = 0; cl < n0; cl++) {
        if (nt_kind(nt, cl) != NK_CallNode || nt_ref(nt, cl, "receiver") != id) continue;
        const char *cnm = nt_str(nt, cl, "name");
        if (!cnm || (!sp_streq(cnm, "call") && !sp_streq(cnm, "()") && !sp_streq(cnm, "[]") &&
                     !sp_streq(cnm, "yield"))) continue;
        nt_node_set_str(nt, cl, "name", "<<");
        nt_node_set_ref(nt, cl, "receiver", trecv);
        nt_node_set_int(nt, cl, "yielder_push", 1);
        changed = 1;
      }
    }
    /* every `&y` inside this generator body */
    for (int bi = 0; bi < nba; bi++) {
      int id = ba_ids[bi];
      if (nt_kind(nt, id) != NK_CallNode) continue;
      int blk = nt_ref(nt, id, "block");
      if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
      int e = nt_ref(nt, blk, "expression");
      if (e < 0 || nt_kind(nt, e) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, e, "name");
      if (!vn || !sp_streq(vn, yname)) continue;
      if (!a_subtree_contains(nt, ebody, id, 0)) continue;
      char pnm[48];
      snprintf(pnm, sizeof pnm, "__yb_e_%d", id);
      int preq = nt_new_node(nt, "RequiredParameterNode");
      if (preq < 0) continue;
      nt_node_set_str(nt, preq, "name", pnm);
      int params = nt_new_node(nt, "ParametersNode");
      nt_node_set_arr(nt, params, "requireds", &preq, 1);
      int bparams = nt_new_node(nt, "BlockParametersNode");
      nt_node_set_ref(nt, bparams, "parameters", params);
      int yread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, yread, "name", yname);
      int pread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, pread, "name", pnm);
      int pargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, pargs, "arguments", &pread, 1);
      int push = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, push, "name", "<<");
      nt_node_set_ref(nt, push, "receiver", yread);
      nt_node_set_ref(nt, push, "arguments", pargs);
      nt_node_set_ref(nt, push, "block", -1);
      /* the push is a Fiber.yield, not an array append: its value is whatever
         the consumer feeds back, so type it boxed rather than as a container */
      nt_node_set_int(nt, push, "yielder_push", 1);
      int pbody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, pbody, "body", &push, 1);
      int nb = nt_new_node(nt, "BlockNode");
      if (nb < 0) continue;
      nt_node_set_ref(nt, nb, "parameters", bparams);
      nt_node_set_ref(nt, nb, "body", pbody);
      nt_node_set_ref(nt, id, "block", nb);
      comp_grow_node_arrays(c);
      for (int j = preq; j <= nb && j < nt->count; j++) c->nscope[j] = c->nscope[id];
      Scope *sc = comp_scope_of(c, id);
      if (sc) { LocalVar *lv = scope_local_intern(sc, pnm); if (lv) lv->is_block_param = 1; }
      changed = 1;
    }
  }
  free(tp_ids); free(ba_ids);
  return changed;
}

/* `iter(&curried)`: a curried Proc has no proc object to hand a block slot,
   so drive it through a literal block that applies one element -- exactly what
   CRuby's Proc#curry answers to `&` (#3654). */
static int desugar_curry_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    int e = nt_ref(nt, blk, "expression");
    if (e < 0 || infer_type(c, e) != TY_CURRY) continue;
    char pnm[48];
    snprintf(pnm, sizeof pnm, "__curry_e_%d", id);
    int preq = nt_new_node(nt, "RequiredParameterNode");
    if (preq < 0) continue;
    nt_node_set_str(nt, preq, "name", pnm);
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &preq, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int pread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, pread, "name", pnm);
    int cargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, cargs, "arguments", &pread, 1);
    int capply = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, capply, "name", "[]");
    nt_node_set_ref(nt, capply, "receiver", e);
    nt_node_set_ref(nt, capply, "arguments", cargs);
    nt_node_set_ref(nt, capply, "block", -1);
    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &capply, 1);
    int bn = nt_new_node(nt, "BlockNode");
    if (bn < 0) continue;
    nt_node_set_ref(nt, bn, "parameters", bparams);
    nt_node_set_ref(nt, bn, "body", body);
    nt_node_set_ref(nt, id, "block", bn);
    comp_grow_node_arrays(c);
    for (int j = preq; j <= bn && j < nt->count; j++) c->nscope[j] = c->nscope[id];
    Scope *sc = comp_scope_of(c, id);
    if (sc) { LocalVar *lv = scope_local_intern(sc, pnm); if (lv) lv->is_block_param = 1; }
    changed = 1;
  }
  return changed;
}

/* `m(&method(:x))`: a Method passed as a block is its #to_proc, which spinel
   already synthesizes -- interposing it lets the callable reach the block slot
   instead of failing the conversion at run time (#3688). */
static int desugar_method_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int cid = 0; cid < n0; cid++) {
    if (nt_kind(nt, cid) != NK_CallNode) continue;
    int id = nt_ref(nt, cid, "block");
    if (id < 0 || nt_kind(nt, id) != NK_BlockArgumentNode) continue;
    /* Only where the callee is a USER method that keeps a real &block: the
       builtin iterators have their own arm for a Method operand, and a local
       that merely types TY_METHOD may be a forwarded block param. */
    {
      const char *cn = nt_str(nt, cid, "name");
      if (!cn || nt_ref(nt, cid, "receiver") >= 0) continue;
      int mi = comp_method_index(c, cn);
      if (mi < 0) {
        Scope *sc = comp_scope_of(c, cid);
        if (sc && sc->class_id >= 0) mi = comp_method_in_chain(c, sc->class_id, cn, NULL);
      }
      if (mi < 0 || !c->scopes[mi].blk_param || !c->scopes[mi].blk_param[0]) continue;
    }
    int e = nt_ref(nt, id, "expression");
    if (e < 0 || nt_kind(nt, e) != NK_CallNode) continue;
    { const char *en = nt_str(nt, e, "name");
      if (!en || !sp_streq(en, "method")) continue; }
    if (infer_type(c, e) != TY_METHOD) continue;
    int tp = nt_new_node(nt, "CallNode");
    if (tp < 0) continue;
    nt_node_set_str(nt, tp, "name", "to_proc");
    nt_node_set_ref(nt, tp, "receiver", e);
    nt_node_set_ref(nt, tp, "arguments", -1);
    nt_node_set_ref(nt, tp, "block", -1);
    nt_node_set_ref(nt, id, "expression", tp);
    comp_grow_node_arrays(c);
    c->nscope[tp] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `File::Stat.new(path)` is CRuby's direct constructor for what `File.stat`
   answers, and spinel carries a stat as the File handle itself: rewrite the
   constructor to the class method rather than resolving a Stat class that has
   no definition (#3766). */
/* `module_function` makes each method BOTH a module method and a private
   instance method of every includer. Spinel models it as the module method
   alone wherever it can, and rewrites a receiverless call inside an includer
   onto the module call the emitter already serves rather than cloning the
   method in (#3734).

   That holds only for a body which does not depend on its receiver. This
   comment used to claim no such body can -- "its self is the module in one
   spelling and the instance in the other" -- which is the reason the two
   spellings differ, not a reason they agree: through the includer CRuby runs
   the method on the INSTANCE, so `@x` is the instance's ivar, `self` is the
   instance, and a receiverless sibling call dispatches on the instance's
   class (#4603). A body that does any of those is cloned into the includer
   by the include transplant (module_function_self_dependent), which gives it
   a real entry in the includer's chain -- and the comp_method_in_chain test
   at the top of the loop below then declines to rewrite its call. The
   rewrite still serves every body that genuinely does not care. */
static int desugar_module_function_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    Scope *sc = comp_scope_of(c, id);
    if (!sc || sc->class_id < 0 || sc->class_id >= c->nclasses) continue;
    /* the enclosing class's own (or inherited) method wins over the mixin */
    if (comp_method_in_chain(c, sc->class_id, nm, NULL) >= 0) continue;
    if (comp_cmethod_in_chain(c, sc->class_id, nm, NULL) >= 0) continue;
    /* a local of this name is a variable read, not a call */
    if (scope_local(sc, nm)) continue;
    int mod = -1;
    for (int k = sc->class_id; k >= 0 && mod < 0; k = c->classes[k].parent) {
      ClassInfo *cl = &c->classes[k];
      for (int j = 0; j < cl->nincluded_mods && mod < 0; j++) {
        int mi2 = cl->included_mods[j];
        if (mi2 < 0 || mi2 >= c->nclasses) continue;
        for (int si = 1; si < c->nscopes; si++) {
          Scope *ms = &c->scopes[si];
          if (ms->class_id != mi2 || !ms->is_module_function) continue;
          if (ms->name && sp_streq(ms->name, nm)) { mod = mi2; break; }
        }
      }
    }
    if (mod < 0) continue;
    int mc = nt_new_node(nt, "ConstantReadNode");
    if (mc < 0) continue;
    nt_node_set_str(nt, mc, "name", c->classes[mod].name);
    nt_node_set_ref(nt, id, "receiver", mc);
    comp_grow_node_arrays(c);
    c->nscope[mc] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

static int desugar_file_stat_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_ConstantPathNode) continue;
    const char *cn = nt_str(nt, recv, "name");
    if (!cn || !sp_streq(cn, "Stat")) continue;
    int par = nt_ref(nt, recv, "parent");
    const char *pn = par >= 0 ? nt_str(nt, par, "name") : NULL;
    if (!pn || !sp_streq(pn, "File")) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    if (args >= 0) nt_arr(nt, args, "arguments", &an);
    if (an != 1) continue;
    int fc = nt_new_node(nt, "ConstantReadNode");
    if (fc < 0) continue;
    nt_node_set_str(nt, fc, "name", "File");
    nt_node_set_ref(nt, id, "receiver", fc);
    nt_node_set_str(nt, id, "name", "stat");
    comp_grow_node_arrays(c);
    c->nscope[fc] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

static int desugar_string_upto(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "upto")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (recv < 0 || (ac != 1 && ac != 2)) continue;
    if (infer_type(c, recv) != TY_STRING || infer_type(c, av[0]) != TY_STRING) continue;
    /* the second argument is the exclusive flag, which is the range's own */
    int excl = 0;
    if (ac == 2) {
      const char *ety = nt_type(nt, av[1]);
      if (ety && sp_streq(ety, "TrueNode")) excl = 1;
      else if (!(ety && sp_streq(ety, "FalseNode"))) continue;
    }
    int rng = nt_new_node(nt, "RangeNode");
    if (rng < 0) continue;
    nt_node_set_ref(nt, rng, "left", recv);
    nt_node_set_ref(nt, rng, "right", av[0]);
    nt_node_set_int(nt, rng, "flags", excl ? 4 : 0);
    nt_node_set_ref(nt, id, "receiver", rng);
    nt_node_set_str(nt, id, "name", "each");
    nt_node_set_ref(nt, id, "arguments", -1);
    /* upto answers the RECEIVER, not the range it walks */
    if (nt_ref(nt, id, "block") >= 0) nt_node_set_int(nt, id, "enum_self_result", recv);
    comp_grow_node_arrays(c);
    c->nscope[rng] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

static int desugar_lazy_terminal(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  static const char *const TERMS[] = {
    "sum", "count", "min", "max", "minmax", "min_by", "max_by", "tally",
    "include?", "member?", "reduce", "inject", "group_by", "partition",
    "sort", "sort_by", "each_with_object", "find_index", "each_entry", NULL };
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int hit = 0;
    for (int t = 0; TERMS[t] && !hit; t++) if (sp_streq(nm, TERMS[t])) hit = 1;
    if (!hit) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !chain_is_lazy_valued(c, recv)) continue;
    int wrap = nt_new_node(nt, "CallNode");
    if (wrap < 0) continue;
    nt_node_set_str(nt, wrap, "name", "to_a");
    nt_node_set_ref(nt, wrap, "receiver", recv);
    nt_node_set_ref(nt, wrap, "arguments", -1);
    nt_node_set_ref(nt, wrap, "block", -1);
    nt_node_set_ref(nt, id, "receiver", wrap);
    comp_grow_node_arrays(c);
    c->nscope[wrap] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

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
    /* `for` binds only as many values as it has index variables, while the
       collector packs a multi-value yield into one array element. One index
       variable therefore destructures that element and keeps its first value,
       which is what a single-left MultiTarget already means. */
    int idxn = nt_ref(nt, id, "index");
    const char *ixt = idxn >= 0 ? nt_type(nt, idxn) : NULL;
    if (c->classes[cid].enum_yield_arity > 1 && ixt && !sp_streq(ixt, "MultiTargetNode")) {
      int mt = nt_new_node(nt, "MultiTargetNode");
      nt_node_set_arr(nt, mt, "lefts", &idxn, 1);
      nt_node_set_ref(nt, id, "index", mt);
      comp_grow_node_arrays(c);
      c->nscope[mt] = c->nscope[id];
    }
    comp_grow_node_arrays(c);
    c->nscope[wrap] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `obj.map { |x| }` on a user Enumerable whose #each yields SEVERAL values:
   CRuby hands them to the block, so a one-parameter block binds the first
   value -- while #select / #find / #sort_by answer the packed element. The
   collector packs every yield into one array element, so the map block's sole
   parameter destructures it, exactly as a `for` index does (#3879). */
static int desugar_multi_yield_map_param(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "map") && !sp_streq(nm, "collect"))) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (cid < 0 || cid >= c->nclasses || c->classes[cid].enum_yield_arity <= 1) continue;
    int bp = nt_ref(nt, blk, "parameters");
    if (bp < 0) continue;
    const char *bpty = nt_type(nt, bp);
    int pn = (bpty && sp_streq(bpty, "BlockParametersNode")) ? nt_ref(nt, bp, "parameters") : bp;
    if (pn < 0 || !nt_type(nt, pn) || !sp_streq(nt_type(nt, pn), "ParametersNode")) continue;
    int nreq = 0; const int *reqs = nt_arr(nt, pn, "requireds", &nreq);
    if (nreq != 1 || !reqs) continue;
    int p0 = reqs[0];
    if (p0 < 0 || nt_kind(nt, p0) != NK_RequiredParameterNode) continue;
    const char *pnm = nt_str(nt, p0, "name");
    int body = nt_ref(nt, blk, "body");
    if (!pnm || body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    /* idempotent: the rewritten body starts with the very assignment below */
    if (bn > 0 && nt_kind(nt, bb[0]) == NK_LocalVariableWriteNode &&
        nt_str(nt, bb[0], "name") && sp_streq(nt_str(nt, bb[0], "name"), pnm)) {
      int v0 = nt_ref(nt, bb[0], "value");
      if (v0 >= 0 && nt_kind(nt, v0) == NK_CallNode && nt_str(nt, v0, "name") &&
          sp_streq(nt_str(nt, v0, "name"), "[]")) continue;
    }
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    int ix = nt_new_node(nt, "IntegerNode");
    int ar = nt_new_node(nt, "ArgumentsNode");
    int cl = nt_new_node(nt, "CallNode");
    int wr = nt_new_node(nt, "LocalVariableWriteNode");
    if (rd < 0 || ix < 0 || ar < 0 || cl < 0 || wr < 0) continue;
    nt_node_set_str(nt, rd, "name", pnm);
    nt_node_set_int(nt, ix, "value", 0);
    nt_node_set_arr(nt, ar, "arguments", &ix, 1);
    nt_node_set_str(nt, cl, "name", "[]");
    nt_node_set_ref(nt, cl, "receiver", rd);
    nt_node_set_ref(nt, cl, "arguments", ar);
    nt_node_set_ref(nt, cl, "block", -1);
    nt_node_set_str(nt, wr, "name", pnm);
    nt_node_set_ref(nt, wr, "value", cl);
    { int nb[64];
      if (bn + 1 > 64) continue;
      nb[0] = wr;
      for (int q = 0; q < bn; q++) nb[q + 1] = bb[q];
      nt_node_set_arr(nt, body, "body", nb, bn + 1); }
    comp_grow_node_arrays(c);
    c->nscope[rd] = c->nscope[ix] = c->nscope[ar] = c->nscope[cl] = c->nscope[wr] = c->nscope[blk];
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
/* A slot is a local (lv), a method's value (lv NULL, sidx the method), or an
   @ivar of one class (ici/iiv, sidx -1; #4444). */
/* Does block `blk` contain another block or a lambda anywhere in its body? */
static int oa_block_has_nested_block_in(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_BlockNode || k == NK_LambdaNode) return 1;
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) if (oa_block_has_nested_block_in(nt, nt_ref_at(nt, n, i))) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (oa_block_has_nested_block_in(nt, ids[j])) return 1;
  }
  return 0;
}
static int oa_block_has_nested_block(const NodeTable *nt, int blk) {
  return blk >= 0 && oa_block_has_nested_block_in(nt, nt_ref(nt, blk, "body"));
}

typedef struct { int sidx; LocalVar *lv; int cls; int alive; int uf; int needs_cmp; int row_iter; int saw_call; TyKind old_pin; int ici, iiv; } OAS;

/* The (sidx, lv) -> first slot index map, so a lookup is not a scan of every
   slot: the pass asks once per node, and the slot list grows with the
   program. Built on first use for a given (sl, n) and dropped by
   oa_index_reset() when the pass starts over; an index is the FIRST matching
   slot, which is what the scan answered. */
static struct { const OAS *sl; int n, cap; int *tab; } g_oa_ix;
static void oa_index_reset(void) { g_oa_ix.sl = NULL; g_oa_ix.n = -1; }
static unsigned oa_hash(int sidx, const LocalVar *lv) {
  uint64_t h = (uint64_t)(uintptr_t)lv * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)sidx * 0xC2B2AE3D27D4EB4Full;
  return (unsigned)(h ^ (h >> 29));
}
static int oa_find(OAS *sl, int n, int sidx, LocalVar *lv) {
  if (g_oa_ix.sl != sl || g_oa_ix.n != n) {
    int cap = 16; while (cap < 2 * n) cap <<= 1;
    if (cap > g_oa_ix.cap) { free(g_oa_ix.tab); g_oa_ix.tab = (int *)malloc(sizeof(int) * cap); g_oa_ix.cap = cap; }
    for (int i = 0; i < g_oa_ix.cap; i++) g_oa_ix.tab[i] = -1;
    unsigned mask = (unsigned)g_oa_ix.cap - 1;
    for (int i = 0; i < n; i++) {
      if (sl[i].ici >= 0) continue;
      unsigned h = oa_hash(sl[i].sidx, sl[i].lv) & mask;
      while (g_oa_ix.tab[h] >= 0 && !(sl[g_oa_ix.tab[h]].sidx == sl[i].sidx && sl[g_oa_ix.tab[h]].lv == sl[i].lv)) h = (h + 1) & mask;
      if (g_oa_ix.tab[h] < 0) g_oa_ix.tab[h] = i;   /* keep the first */
    }
    g_oa_ix.sl = sl; g_oa_ix.n = n;
  }
  unsigned mask = (unsigned)g_oa_ix.cap - 1;
  for (unsigned h = oa_hash(sidx, lv) & mask; g_oa_ix.tab[h] >= 0; h = (h + 1) & mask) {
    const OAS *e = &sl[g_oa_ix.tab[h]];
    if (e->sidx == sidx && e->lv == lv) return g_oa_ix.tab[h];
  }
  return -1;
}
static int oa_find_iv(OAS *sl, int n, int ici, int iiv) {
  for (int i = 0; i < n; i++) if (sl[i].ici == ici && sl[i].iiv == iiv) return i;
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
/* Element-class sentinels: the array's elements are themselves int-arrays
   (ExtField-style int[4]) or float-arrays (a numeric table), narrowing the
   container to TY_INT_ARRAY_ARRAY / TY_FLOAT_ARRAY_ARRAY rather than an
   object-pointer array. Distinct from -1 (none) / -2 (conflict) and from any
   user class id (>= 0), so oa_cls_join composes them unchanged -- and distinct
   from EACH OTHER, so a component mixing int rows and float rows joins to -2
   and stays boxed, as two user classes would. */
#define OA_CLS_IA (-1000)
#define OA_CLS_FA (-1001)
#define OA_CLS_IS_NESTED(x) ((x) == OA_CLS_IA || (x) == OA_CLS_FA)
/* the single user-object class of a node's value, a nested scalar-array
   sentinel, or -1 if neither a lone object nor a lone scalar array. */
static int oa_obj_class_of(Compiler *c, int node) {
  TyKind t = infer_type(c, node);
  if (ty_is_object(t)) return ty_object_class(t);
  if (t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY) {
    /* A literal whose elements are not all settled reads as a scalar array
       while the fixpoint runs -- an UNKNOWN element unifies away -- and
       `[obj, -1]` looked like one until `obj` acquired its class. Narrowing on
       that evidence pinned the container to a table of int arrays, and the pin
       survived the rounds that knew better (#3781). Take no evidence until
       every element has a type. */
    const NodeTable *nt = c->nt;
    const char *nty = nt_type(nt, node);
    if (nty && sp_streq(nty, "ArrayNode")) {
      int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
      for (int k = 0; k < en; k++)
        if (infer_type(c, els[k]) == TY_UNKNOWN) return -1;
    }
    return t == TY_INT_ARRAY ? OA_CLS_IA : OA_CLS_FA;
  }
  /* A value of another CONCRETE type is evidence against one class: a String
     or an Integer pushed or `[]=`-stored into the array puts something in it
     the pointer array cannot hold (`a[0] = "x"` was joined as "nothing seen"
     and the store then initialised an sp_Vec * from a string, #4444). A type
     not known yet, nil (a NULL element) and a boxed poly value stay neutral:
     the poly may well be the class (a widened return, #4293), and the push
     emitter unboxes it with the class check at run time. */
  if (t == TY_UNKNOWN || t == TY_NIL || t == TY_POLY) return -1;
  return -2;
}
/* class evidence join: -1 = none seen, -2 = conflicting classes. */
static int oa_cls_join(int a, int b) {
  if (b == -1) return a;
  if (a == -1) return b;
  if (a == b) return a;
  return -2;   /* two classes, or a foreign element (-2) from either side */
}
/* The empty `[]` literals that flowed into a slot as ELEMENTS this round: a
   row of `Array.new(n) { [] }`, an element of `[[], []]`, the value of a
   push or a `[]=`. An empty literal is no evidence about the element kind
   (it infers UNKNOWN until a use fixes it) and is neutral in the join; when
   the component does narrow to a table, from another row or from an
   `Array[Array[Float]]` seed, the literal has to be BUILT as a row of that
   kind, or the table would hold a poly array where its reader casts an
   sp_FloatArray *. arr_want is the durable pin an empty literal takes its
   kind from (mark_empty_array_operands uses the same one), so a stamp here
   is seen by the next round's inference and by codegen (#4484). */
static int *g_oa_empt_slot, *g_oa_empt_node, g_oa_empt_n, g_oa_empt_cap;
/* Nodes whose own emitted type has to follow the slot they feed: a
   `idx.map { |k| cols[k] }` builds the table in place, so narrowing the slot
   without retyping the map leaves the emitter building a poly array and
   assigning it to a narrowed one -- which does not compile. The empty-literal
   list beside this one does the same job for `[]`. */
static int *g_oa_src_slot, *g_oa_src_node, g_oa_src_n, g_oa_src_cap;
static void oa_note_src(int S, int node) {
  if (node < 0) return;
  if (g_oa_src_n >= g_oa_src_cap) {
    g_oa_src_cap = g_oa_src_cap ? g_oa_src_cap * 2 : 64;
    g_oa_src_slot = (int *)realloc(g_oa_src_slot, sizeof(int) * (size_t)g_oa_src_cap);
    g_oa_src_node = (int *)realloc(g_oa_src_node, sizeof(int) * (size_t)g_oa_src_cap);
    if (!g_oa_src_slot || !g_oa_src_node) { fprintf(stderr, "oom\n"); exit(1); }
  }
  g_oa_src_slot[g_oa_src_n] = S; g_oa_src_node[g_oa_src_n] = node; g_oa_src_n++;
}
static void oa_note_empty(Compiler *c, int S, int node) {
  if (!is_empty_array_literal(c->nt, node, c->node_cap)) return;
  if (g_oa_empt_n >= g_oa_empt_cap) {
    g_oa_empt_cap = g_oa_empt_cap ? g_oa_empt_cap * 2 : 64;
    g_oa_empt_slot = (int *)realloc(g_oa_empt_slot, sizeof(int) * (size_t)g_oa_empt_cap);
    g_oa_empt_node = (int *)realloc(g_oa_empt_node, sizeof(int) * (size_t)g_oa_empt_cap);
    if (!g_oa_empt_slot || !g_oa_empt_node) { fprintf(stderr, "oom\n"); exit(1); }
  }
  g_oa_empt_slot[g_oa_empt_n] = S; g_oa_empt_node[g_oa_empt_n] = node; g_oa_empt_n++;
}
/* element evidence from one value flowing into slot S, noting an empty
   literal for the stamp above */
static int oa_elem_evidence(Compiler *c, int S, int node) {
  oa_note_empty(c, S, node);
  return oa_obj_class_of(c, node);
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
/* The sibling of the pass below, for the other way an ivar's storage escapes:
   read out through a READER into a slot that is a poly array.

   `e = m.a` over `def initialize; @a = ["seed"]; end` binds a poly local,
   because `e << 7` puts an Integer in it. The ivar stayed a str array, so the
   emitter widened at the assignment -- and widening an array is a COPY, so
   the push landed on the copy and `equal?` was false where CRuby says true
   (#4412). Widening the ivar removes the conversion instead of copying
   through it, which is what the sibling pass already does for the argument
   shape (#3154).

   Monotone and narrow: only a typed array becomes a poly array, only when the
   slot it is read into is already one. A reader is a method whose body is a
   bare ivar read, which is what both `attr_reader :a` and `def a; @a; end`
   come to. */
static const char *an_reader_ivar_name(Compiler *c, const char *mname, int cls) {
  if (cls < 0 || !mname) return NULL;
  /* attr_reader is synthesized, so it has no scope to read a body out of:
     comp_resolve_member is what knows the name is an attribute, and its
     backing ivar is the name with an @ in front (see the attr_reader
     backing-ivar rule). */
  { int def_cls = -1, mix = -1;
    if (comp_resolve_member(c, cls, mname, 0, &def_cls, &mix) == SP_MEMBER_ATTR) {
      static char ivb[128];
      if (strlen(mname) < sizeof ivb - 2) {
        ivb[0] = '@'; strcpy(ivb + 1, mname);
        return ivb;
      }
      return NULL;
    } }
  int mi = comp_method_in_chain(c, cls, mname, NULL);
  if (mi < 0) return NULL;
  Scope *m = &c->scopes[mi];
  if (m->body < 0) return NULL;
  int n = 0; const int *st = nt_arr(c->nt, m->body, "body", &n);
  if (n != 1 || !st) return NULL;
  const char *bt = nt_type(c->nt, st[0]);
  if (!bt || !sp_streq(bt, "InstanceVariableReadNode")) return NULL;
  return nt_str(c->nt, st[0], "name");
}
static void widen_ivars_read_into_poly(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    int v = nt_ref(nt, id, "value");
    if (v < 0 || nt_kind(nt, v) != NK_CallNode) continue;
    { int ca = nt_ref(nt, v, "arguments");
      int an = 0; if (ca >= 0) nt_arr(nt, ca, "arguments", &an);
      if (an > 0 || nt_ref(nt, v, "block") >= 0) continue; }
    int recv = nt_ref(nt, v, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    const char *mname = nt_str(nt, v, "name");
    if (!mname) continue;
    int rcls = ty_object_class(rt);
    const char *ivn = an_reader_ivar_name(c, mname, rcls);
    if (!ivn) continue;
    /* the slot being written must already be a poly array: that is what says
       the program puts something in it the ivar's element type cannot hold */
    Scope *ws = comp_scope_of(c, id);
    const char *wn = nt_str(nt, id, "name");
    LocalVar *wl = (ws && wn) ? scope_local(ws, wn) : NULL;
    if (!wl || wl->type != TY_POLY_ARRAY) continue;
    int ivi = comp_ivar_index(&c->classes[rcls], ivn);
    if (ivi < 0) continue;
    TyKind ivt = c->classes[rcls].ivar_types[ivi];
    if (!ty_is_array(ivt) || ivt == TY_POLY_ARRAY) continue;
    c->classes[rcls].ivar_types[ivi] = TY_POLY_ARRAY;
  }
  /* And the shape with no local at all: a push straight through the reader,
     `self.errors << "blank"` over `@errors = []`. The empty literal defaults
     to an int array, so the String met sp_IntArray_push and the C compiler
     refused it -- the same lock seen from the side where no conversion
     exists to insert. Widen when the ivar's element type cannot hold what is
     pushed; an int array pushed an int stays an int array. */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "<<") && !sp_streq(nm, "push"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    { int ca = nt_ref(nt, recv, "arguments");
      int an = 0; if (ca >= 0) nt_arr(nt, ca, "arguments", &an);
      if (an > 0 || nt_ref(nt, recv, "block") >= 0) continue; }
    int rr = nt_ref(nt, recv, "receiver");
    if (rr < 0) continue;
    TyKind rt = infer_type(c, rr);
    if (!ty_is_object(rt)) continue;
    int rcls = ty_object_class(rt);
    const char *rn = nt_str(nt, recv, "name");
    const char *ivn = rn ? an_reader_ivar_name(c, rn, rcls) : NULL;
    if (!ivn) continue;
    int ivi = comp_ivar_index(&c->classes[rcls], ivn);
    if (ivi < 0) continue;
    TyKind ivt = c->classes[rcls].ivar_types[ivi];
    if (!ty_is_array(ivt) || ivt == TY_POLY_ARRAY) continue;
    int ca2 = nt_ref(nt, id, "arguments");
    int an2 = 0; const int *av2 = ca2 >= 0 ? nt_arr(nt, ca2, "arguments", &an2) : NULL;
    if (!av2 || an2 != 1) continue;
    TyKind at = infer_type(c, av2[0]);
    if (at == TY_UNKNOWN) continue;
    if (at == ty_array_elem(ivt)) continue;
    c->classes[rcls].ivar_types[ivi] = TY_POLY_ARRAY;
  }
}

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
    if (recv < 0) mi = comp_self_call_mi(c, id, name);
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
      /* `Held.add(@found)` / `W.new(@found)`: a singleton method (or the
         constructor) shares the ivar's backing storage with its parameter
         exactly like an instance method does, and was the one caller shape
         this resolution missed -- the pushed element was stored as the int
         array's own kind, or dropped (#4213; the same arm #3505 added to
         the sentinel-marking loop). */
      else if (nt_kind(nt, recv) == NK_ConstantReadNode) {
        int rci = comp_class_index(c, nt_str(nt, recv, "name"));
        if (rci >= 0)
          mi = sp_streq(name, "new") ? comp_method_in_chain(c, rci, "initialize", NULL)
                                     : comp_cmethod_in_chain(c, rci, name, NULL);
      }
    }
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    Scope *asc = comp_scope_of(c, id);
    if (!asc || asc->class_id < 0) continue;
    ClassInfo *acls = &c->classes[asc->class_id];
    for (int k = 0; k < an && k < m->nparams; k++) {
      LocalVar *p = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
      if (!p) continue;
      /* Two ways in. A typed parameter that was push-widened says so directly.
         A BOXED one hid its container from the call site, so it carries the
         element it pushes instead and the comparison happens here, where the
         ivar's own element type is in hand -- widen only when the ivar cannot
         hold what the callee pushes, so an int array stays an int array. */
      int boxed_hazard = 0;
      if (!p->push_widened) {
        if (p->type != TY_POLY || p->boxed_push_elem == TY_UNKNOWN) continue;
        boxed_hazard = 1;
      }
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
      TyKind ivt = acls->ivar_types[ivi];
      if (!ty_is_array(ivt) || ivt == TY_POLY_ARRAY) continue;
      if (boxed_hazard && ty_array_elem(ivt) == p->boxed_push_elem) continue;
      acls->ivar_types[ivi] = TY_POLY_ARRAY;
    }
  }
}

/* Narrow an @ivar holding a table of int arrays. An ivar
   is harder than a local in one way that matters: its references are
   spread over every method of the class and can escape through a reader,
   a bare return or an argument, and a missed one here is not a lost
   optimization but an sp_PtrArray * meeting sp_RbVal in the emitted C. So
   the vetting is deliberately absolute -- EVERY reference to the ivar,
   anywhere in the program, must be the receiver of an op this pass models,
   from a method of the owning class itself; anything else leaves the slot
   alone. An attr reader/writer is an escape by definition and disqualifies
   the ivar outright.

   Placement matters as much as the vetting: this runs inside
   narrow_object_arrays, after every pass that re-derives an ivar type by
   unifying it with its writes. Earlier, that re-derivation would unify
   TY_INT_ARRAY_ARRAY with the write's TY_POLY_ARRAY -- two array kinds,
   which unify to the plain poly scalar -- and the slot would come out
   WORSE than it went in. */
static int narrow_int_table_ivars(Compiler *c) {
  int narrowed = 0;
  /* Re-assert first. The slot's own write still reads TY_POLY_ARRAY and ~90
     sites derive an ivar type from one, so something re-derives this one on
     nearly every round -- and TY_INT_ARRAY_ARRAY unified with TY_POLY_ARRAY is
     the plain poly SCALAR, strictly worse than either. Guarding every writer
     is not practical; restoring the pinned type on each round is what
     ivar_str_shared already does for the same reason. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      if (!cl->ivar_int_table[iv]) continue;
      TyKind want = cl->ivar_oa_type[iv] != TY_UNKNOWN ? cl->ivar_oa_type[iv] : TY_INT_ARRAY_ARRAY;
      if (cl->ivar_types[iv] != want) cl->ivar_types[iv] = want;
    }
  }
  const NodeTable *nt = c->nt;
  /* The ivar reads and writes, in node order, listed once and grouped by name:
     each candidate slot below walked the whole table for them, and then every
     ivar node of every name. And for a read, the first call it is the
     receiver of -- the only one the check below looks at -- which it found by
     walking the whole table again, per read. */
  int *ivl = NULL, nivl = 0, *ivl_next = NULL, *first_call = NULL;
  const char **ivl_name = NULL;
  int ivl_buckets = 0, *ivl_head = NULL;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      if (cl->ivar_int_table[iv]) continue;   /* already narrowed and pinned */
      if (!ivl) {
        ivl = malloc(sizeof(int) * (size_t)(nt->count + 1));
        if (!ivl) return narrowed;
        for (int id = 0; id < nt->count; id++) {
          NodeKind k = nt_kind(nt, id);
          if (k == NK_InstanceVariableReadNode || k == NK_InstanceVariableWriteNode ||
              k == NK_InstanceVariableOperatorWriteNode || k == NK_InstanceVariableOrWriteNode ||
              k == NK_InstanceVariableAndWriteNode) ivl[nivl++] = id;
        }
        /* name -> its ivar nodes, ascending (chained in reverse, walked forward) */
        ivl_buckets = nivl > 0 ? nivl : 1;
        ivl_head = malloc(sizeof(int) * (size_t)ivl_buckets);
        ivl_next = malloc(sizeof(int) * (size_t)(nivl + 1));
        ivl_name = malloc(sizeof(char *) * (size_t)(nivl + 1));
        first_call = malloc(sizeof(int) * (size_t)(nt->count + 1));
        if (!ivl_head || !ivl_next || !ivl_name || !first_call) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        for (int b = 0; b < ivl_buckets; b++) ivl_head[b] = -1;
        for (int li = nivl - 1; li >= 0; li--) {
          const char *nm = nt_str(nt, ivl[li], "name");
          ivl_name[li] = nm;
          unsigned h = 2166136261u;
          for (const char *q = nm ? nm : ""; *q; q++) { h ^= (unsigned char)*q; h *= 16777619u; }
          int b = (int)(h % (unsigned)ivl_buckets);
          ivl_next[li] = ivl_head[b]; ivl_head[b] = li;
        }
        for (int u = 0; u < nt->count; u++) first_call[u] = -1;
        for (int u = nt->count - 1; u >= 0; u--) {   /* descending: the lowest call wins */
          if (nt_kind(nt, u) != NK_CallNode) continue;
          int r = nt_ref(nt, u, "receiver");
          if (r >= 0 && r < nt->count) first_call[r] = u;
        }
      }
      if (cl->ivar_types[iv] != TY_POLY_ARRAY) continue;
      const char *ivn = cl->ivars[iv];
      if (!ivn || !ivn[0]) continue;
      if (class_ivar_pinned(cl, ivn)) continue;         /* an --rbs seed owns it */
      /* this pass makes int tables only; a slot declared `Array[Array[Float]]`
         is narrow_object_arrays' to decide, where the seed is evidence and
         an int row against it is the contradiction reported after the
         fixpoint rather than a table of the other kind (#4484) */
      if (cl->ivar_oa_seed[iv] == SEED_OA_FLT_TABLE) continue;
      const char *bare = ivn[0] == '@' ? ivn + 1 : ivn;
      if (comp_is_reader(cl, bare) || comp_is_writer(cl, bare)) continue;
      if (comp_is_sg_reader(cl, bare) || comp_is_sg_writer(cl, bare)) continue;
      int ok = 1, saw_table = 0;
      unsigned hb = 2166136261u;
      for (const char *q = ivn; *q; q++) { hb ^= (unsigned char)*q; hb *= 16777619u; }
      for (int li = ivl_head[hb % (unsigned)ivl_buckets]; li >= 0 && ok; li = ivl_next[li]) {
        if (!ivl_name[li] || !sp_streq(ivl_name[li], ivn)) continue;
        int id = ivl[li];
        const char *ty = nt_type(nt, id);
        if (!ty) continue;
        int is_read  = sp_streq(ty, "InstanceVariableReadNode");
        int is_write = sp_streq(ty, "InstanceVariableWriteNode");
        int is_owrite = sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
                        sp_streq(ty, "InstanceVariableOrWriteNode") ||
                        sp_streq(ty, "InstanceVariableAndWriteNode");
        if (!is_read && !is_write && !is_owrite) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm || !sp_streq(nm, ivn)) continue;
        Scope *sc = comp_scope_of(c, id);
        if (!sc) { ok = 0; break; }
        if (sc->class_id != ci) {
          /* An unrelated class with an ivar of the same name is a different
             slot -- ignore it. A subclass (or a module transplanted in) reads
             THIS slot from code this pass does not vet, so that disqualifies. */
          int related = 0;
          for (int k = sc->class_id; k >= 0; k = c->classes[k].parent)
            if (k == ci) { related = 1; break; }
          if (!related) continue;
          ok = 0; break;
        }
        if (is_owrite) { ok = 0; break; }   /* `@t ||= ...`: unmodeled */
        if (is_write) {
          int v = nt_ref(nt, id, "value");
          const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
          if (!vty) { ok = 0; break; }
          if (sp_streq(vty, "NilNode")) continue;                  /* neutral */
          if (sp_streq(vty, "ArrayNode")) {
            int en = 0; const int *el = nt_arr(nt, v, "elements", &en);
            if (en == 0) continue;                                 /* `@t = []` */
            for (int e = 0; e < en && ok; e++)
              if (infer_type(c, el[e]) != TY_INT_ARRAY) ok = 0;
            if (ok) saw_table = 1;
            continue;
          }
          /* `@t = Array.new(n) { <int array> }` */
          if (!sp_streq(vty, "CallNode")) { ok = 0; break; }
          const char *cn = nt_str(nt, v, "name");
          int crecv = nt_ref(nt, v, "receiver");
          int gb = nt_ref(nt, v, "block");
          if (!cn || !sp_streq(cn, "new") || crecv < 0 || gb < 0 ||
              !nt_type(nt, crecv) || !sp_streq(nt_type(nt, crecv), "ConstantReadNode") ||
              !nt_str(nt, crecv, "name") ||
              !sp_streq(nt_str(nt, crecv, "name"), "Array")) { ok = 0; break; }
          int gbody = nt_ref(nt, gb, "body");
          int gn = 0; const int *gs = gbody >= 0 ? nt_arr(nt, gbody, "body", &gn) : NULL;
          if (!gs || gn <= 0 || infer_type(c, gs[gn - 1]) != TY_INT_ARRAY) { ok = 0; break; }
          saw_table = 1;
          continue;
        }
        /* a read: it must BE the receiver of a modeled op, nothing else */
        int used = 0;
        for (int u = first_call[id]; u >= 0; u = -1) {
          const char *un = nt_str(nt, u, "name");
          int ua = nt_ref(nt, u, "arguments"); int uan = 0;
          if (ua >= 0) nt_arr(nt, ua, "arguments", &uan);
          /* sort/min/max need the boxed comparator, which this element kind
             has no arm for: leave those tables alone */
          if (un && (sp_streq(un, "sort") || sp_streq(un, "sort!") ||
                     sp_streq(un, "min") || sp_streq(un, "max"))) { used = 0; break; }
          if (!oa_recv_op_ok(un, uan, nt_ref(nt, u, "block") >= 0)) { used = 0; break; }
          used = 1; break;
        }
        if (!used) { ok = 0; break; }
      }
      if (ok && saw_table) {
        cl->ivar_types[iv] = TY_INT_ARRAY_ARRAY;
        cl->ivar_int_table[iv] = 1;
        narrowed = 1;
      }
    }
  }
  free(ivl); free(ivl_head); free(ivl_next); free(ivl_name); free(first_call);
  return narrowed;
}

/* Locals read out of an already-narrowed array: `b = arr[i]` makes b the
   element type, so its own reads unbox. Split out of narrow_object_arrays
   so it still runs when that pass has no local candidate slot and returns
   early -- an ivar table narrowed by narrow_int_table_ivars has exactly
   that shape, and without this its rows stayed boxed and the narrowing
   bought nothing. */
static int narrow_locals_from_arrays(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* dependent locals: `b = arr[i]` (arr now a narrowed obj array) makes `b`
        the element object type, so its field accesses unbox. Every write of the
        local must be such an index read of one class (nil writes excepted).
        Iterated, because the narrowing chains: `a = rows[0]` narrows a to an
        int array only in this step, and only then can `m = a[1]` see an int
        (#3354). */
  int any = 0;
  for (int prop = 0; prop < 8; prop++) {
  int prop_ch = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
      TyKind elem = TY_UNKNOWN; int ok = 1, saw = 0;
      /* the (scope, name) write chain replaces a full node-table walk that
         ran once per candidate local per propagation round -- quadratic on
         machine-generated programs, where one 52k-line input spent 58% of
         its analyze in this loop. The chain carries hash collisions, so the
         kind/scope/name filters stay. */
      for (int id = comp_lvw_first_sc(c, s, lv->name); id >= 0 && ok; id = comp_lvw_next_sc(c, id)) {
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
                  : (rt == TY_FLOAT_ARRAY_ARRAY) ? TY_FLOAT_ARRAY
                  : (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)
                    ? ty_array_elem(rt) : TY_UNKNOWN;
        if (ec == TY_UNKNOWN) { ok = 0; break; }
        if (elem == TY_UNKNOWN) elem = ec; else if (elem != ec) { ok = 0; break; }
        saw = 1;
      }
      if (ok && saw && elem != TY_UNKNOWN) {
        /* Pin it: the next write pass resets the slot and re-derives the plain
           poly the container read hands back, and this pass would narrow it
           again -- an oscillation that never converged, so the fixpoint ran its
           full round budget and left whatever types the cap happened to catch
           (#3781). Every write of the local is an index read of the same
           element type (the loop above insists), so the pin cannot mask a
           genuine widening. */
        lv->type = elem;
        lv->oa_pin = elem;
        prop_ch = 1; any = 1;
      }
    }
  }
  if (!prop_ch) break;
  }
  return any;
}

/* Classify one value flowing into slot S: an object or int-array literal is
   element evidence, another slot (or a narrowable call's value) is an alias
   edge, nil is neutral, and anything the pass does not model kills the slot.
   Shared by a local's write sources and by a method's return expressions --
   `t = build_table(n)` names one storage location in two scopes, so the
   caller's local and the callee's value have to agree on one container type
   or the call would hand an unboxed sp_PtrArray* to a boxed reader. */
static void oa_classify_value(Compiler *c, OAS *sl, int n, const int *read_slot,
                              const int *call_ret, char *claimed, int S, int v) {
  const NodeTable *nt = c->nt;
  const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
  if (!vty) { sl[S].alive = 0; return; }
  if (sp_streq(vty, "ArrayNode")) {
    int en = 0; const int *el = nt_arr(nt, v, "elements", &en);
    for (int e = 0; e < en; e++) {
      const char *ety = nt_type(nt, el[e]);
      int ec = (ety && sp_streq(ety, "SplatNode")) ? -2 : oa_elem_evidence(c, S, el[e]);
      /* OA_CLS_IA / OA_CLS_FA (a nested scalar array) are negative SENTINELS,
         not an "invalid" -1/-2: they are real evidence, so they must not kill
         the slot (an `[[a,b],[c,d]]` array-of-int-array literal narrows like a
         pushed one already does -- the push arm never had this < 0 guard).
         An empty `[]` element is a row with no kind of its own yet: neutral,
         and built at the component's kind if it narrows (oa_note_empty). */
      if (is_empty_array_literal(nt, el[e], c->node_cap) && !OA_CLS_IS_NESTED(ec)) continue;
      if (ec < 0 && !OA_CLS_IS_NESTED(ec)) { sl[S].alive = 0; break; }
      sl[S].cls = oa_cls_join(sl[S].cls, ec);
    }
    return;
  }
  if (read_slot[v] >= 0) {   /* a local, an @ivar, or its attr_reader's value */
    claimed[v] = 1; oa_uf_union(sl, S, read_slot[v]);
    return;
  }
  if (sp_streq(vty, "NilNode")) return;   /* nullable, neutral */
  if (sp_streq(vty, "CallNode")) {
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
    /* `slot << x` / `slot.push(x)` answer the receiver itself: a method whose
       body is `collection << Flag.new` has that array as its value, and
       without this edge its return slot died while its parameter narrowed,
       leaving an sp_PtrArray body under an sp_PolyArray return. The push
       argument is element evidence exactly as in the receiver-op arm. */
    else if (cn && (sp_streq(cn, "<<") || sp_streq(cn, "push") || sp_streq(cn, "append")) &&
             can >= 1 && nt_ref(nt, v, "block") < 0 && crecv >= 0 && read_slot[crecv] >= 0) {
      int cargv_n = 0; const int *cargv = nt_arr(nt, cargs, "arguments", &cargv_n);
      for (int a = 0; a < cargv_n; a++) sl[S].cls = oa_cls_join(sl[S].cls, oa_elem_evidence(c, S, cargv[a]));
      claimed[crecv] = 1;
      oa_uf_union(sl, S, read_slot[crecv]);
    }
    /* `t = Array.new(n) { <int array> }`: the generator block's value is the
       element, so its type is the same evidence a pushed element gives. The
       nested int table this builds is the one shape the pass still left on
       the boxed poly path, so every read of it went through sp_poly_arr_get
       and its arithmetic boxed. */
    else if (cn && sp_streq(cn, "new") && crecv >= 0 &&
             nt_type(nt, crecv) && sp_streq(nt_type(nt, crecv), "ConstantReadNode") &&
             nt_str(nt, crecv, "name") && sp_streq(nt_str(nt, crecv, "name"), "Array") &&
             nt_ref(nt, v, "block") >= 0) {
      int gb = nt_ref(nt, v, "block");
      int gbody = gb >= 0 ? nt_ref(nt, gb, "body") : -1;
      int gn = 0;
      const int *gs = gbody >= 0 ? nt_arr(nt, gbody, "body", &gn) : NULL;
      int ec = (gs && gn > 0) ? oa_elem_evidence(c, S, gs[gn - 1]) : -1;
      /* `Array.new(n) { [] }`: the row is an empty literal, neutral here and
         built at the table's kind once something else (a pushed row, an
         `Array[Array[Float]]` seed) decides it (#4484) */
      if (gs && gn > 0 && is_empty_array_literal(nt, gs[gn - 1], c->node_cap) &&
          !ty_is_ptr_array(infer_type(c, gs[gn - 1])) &&
          infer_type(c, gs[gn - 1]) != TY_INT_ARRAY && infer_type(c, gs[gn - 1]) != TY_FLOAT_ARRAY) { }
      else if (ec < 0 && !OA_CLS_IS_NESTED(ec)) sl[S].alive = 0;
      else sl[S].cls = oa_cls_join(sl[S].cls, ec);
    }
    /* `raw = idx.map { |k| cols[k] }`: the mapped table's rows ARE the other
       table's rows -- the same objects, not copies -- so the two must agree on
       a row kind. That is an EDGE, not element evidence: the source table's
       kind is still being derived this round (every slot is reset at the top
       of the pass), so reading it as evidence answers "no kind yet" and the
       slot dies. The while-loop spelling never needed this because its rows
       come from its own `Array.new(0, 0.0)` generator.

       Deliberately only this shape. Keeping every map source alive also
       narrows `[...].map { Obj.new(s) }` to an object table, and the places
       that consume one -- `min`, `join`, interpolation -- still want the boxed
       element, so the C stopped compiling. The rows here are already unboxed
       pointers read out of a narrowed table, which is what makes this shape
       the one that pays.

       The receiver must be an ARRAY. `h.map { |k, v| cols[v] }` answers the
       same rows, but its emitter has no pointer-array container to collect
       them into: the hash-collect path bails on the kind and the fallback it
       drops through does not reach Hash#map at all, so the program stopped
       running. Narrowing a slot whose builder cannot build it at that kind is
       worse than leaving it boxed. */
    else if (cn && (sp_streq(cn, "map") || sp_streq(cn, "collect")) && can == 0 &&
             nt_ref(nt, v, "block") >= 0 && nt_type(nt, nt_ref(nt, v, "block")) &&
             sp_streq(nt_type(nt, nt_ref(nt, v, "block")), "BlockNode") &&
             crecv >= 0 && ty_is_array(infer_type(c, crecv))) {
      int mb = nt_ref(nt, v, "block");
      int mbody = nt_ref(nt, mb, "body");
      int mn = 0;
      const int *ms = mbody >= 0 ? nt_arr(nt, mbody, "body", &mn) : NULL;
      int mtail = (ms && mn > 0) ? ms[mn - 1] : -1;
      int joined = 0;
      if (mtail >= 0 && nt_kind(nt, mtail) == NK_CallNode) {
        const char *mtn = nt_str(nt, mtail, "name");
        int mtr = nt_ref(nt, mtail, "receiver");
        int mta = nt_ref(nt, mtail, "arguments"); int mtan = 0;
        if (mta >= 0) nt_arr(nt, mta, "arguments", &mtan);
        if (mtn && (sp_streq(mtn, "[]") || sp_streq(mtn, "at")) && mtan == 1 &&
            mtr >= 0 && read_slot[mtr] >= 0) {
          claimed[mtr] = 1;
          oa_uf_union(sl, S, read_slot[mtr]);
          /* the map BUILDS the table in place, so the emitter has to build it
             at the kind this component settles on -- recorded as a want, which
             later inference passes re-read (a c->ntype stamp would be
             recomputed away before emission) */
          oa_note_src(S, v);
          joined = 1;
        }
      }
      if (!joined) sl[S].alive = 0;   /* every other map source, as before */
    }
    /* a call whose own value is a tracked slot: join the two. */
    else if (call_ret[v] >= 0) {
      claimed[v] = 1; oa_uf_union(sl, S, call_ret[v]);
    }
    else sl[S].alive = 0;
    return;
  }
  sl[S].alive = 0;
}

/* A want narrow_object_arrays cleared at the top of a round and did NOT
   re-stamp is a decision that went away. Reporting it re-runs write inference
   without it; see the call at the end of the pass for why that matters. Only a
   genuinely dropped one: the ordinary round clears and re-stamps, and a node
   still carrying a stamp is not a drop. */
static int oa_want_dropped(Compiler *c, const int *cleared, int n_cleared) {
  if (!c->arr_want) return 0;
  for (int e = 0; e < n_cleared; e++) {
    int cid = cleared[e];
    if (cid >= 0 && cid < c->node_cap && c->arr_want[cid] == TY_UNKNOWN) return 1;
  }
  return 0;
}

/* `__enum_<name>__<callId>`: the per-site copy desugar_builtins made for one
   call. The id is the call this body was specialized for. */
static int enum_copy_site(const char *name) {
  if (!name || strncmp(name, "__enum_", 7) != 0) return -1;
  const char *p = strrchr(name, '_');
  if (!p || p == name || p[-1] != '_') return -1;
  char *end = NULL;
  long v = strtol(p + 1, &end, 10);
  if (!end || *end || v < 0 || v > 2000000000L) return -1;
  return (int)v;
}

static void oa_mark_subtree(const NodeTable *nt, int id, unsigned char *dead) {
  if (id < 0 || id >= nt->count || dead[id]) return;
  dead[id] = 1;
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) oa_mark_subtree(nt, nd->r[j].ref, dead);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) oa_mark_subtree(nt, nd->a[j].ids[k], dead);
}

static int narrow_object_arrays(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* Start every round from the same shape. A slot narrowed last round holds
     its pointer-array type, which would keep it out of the candidate list
     below and split its component; put it back on the poly array and let the
     evidence decide again. Doing it this way rather than pinning-and-skipping
     keeps the pass idempotent, so a slot that stops qualifying (a new escape
     appeared as the fixpoint desugared the program) un-narrows instead of
     keeping a type its uses no longer support. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      TyKind pn = sc->locals[li].oa_pin;
      /* only THIS pass's own narrowings go back on the poly array: the pin
         field also carries the element-narrowing's decision (a local bound
         from a container read), and resetting one of those to a poly ARRAY
         made the two passes trade the slot every round (#3781) */
      if (pn == TY_INT_ARRAY_ARRAY || pn == TY_FLOAT_ARRAY_ARRAY ||
          ty_is_obj_array(pn)) sc->locals[li].type = TY_POLY_ARRAY;
      /* Enumerable's per-site copy types `__self` from its argument, so a
         nested table arrives here already narrowed and would miss the
         candidate list. The argument loop then reads "param not a candidate"
         as a kill and the caller's table oscillates with the pin reassert.
         Put the copy's receiver back on the poly array and let this round's
         evidence decide it again, the same as a pin this pass wrote itself. */
      if (sc->name && strncmp(sc->name, "__enum_", 7) == 0 &&
          sc->locals[li].name && sp_streq(sc->locals[li].name, "__self") &&
          (sc->locals[li].type == TY_INT_ARRAY_ARRAY ||
           sc->locals[li].type == TY_FLOAT_ARRAY_ARRAY))
        sc->locals[li].type = TY_POLY_ARRAY;
    }
  }
  /* 1. candidate slots: POLY_ARRAY locals/params (skip block params + rbs). */
  int cap = 16, n = 0;
  OAS *sl = (OAS *)malloc(sizeof(OAS) * cap);
  oa_index_reset();
  g_oa_empt_n = 0;
  g_oa_src_n = 0;
  /* Drop every want this pass stamped on a map source in an earlier round,
     before the round re-derives them. OA_DROP_SRC_STAMP below only reaches the
     sources RECORDED this round, and a node stops being recorded for reasons
     that have nothing to do with it still carrying a stamp -- its slot is no
     longer a candidate, the receiver's type no longer reads as an array, the
     row it aliases is no longer a slot. Such a node kept a pointer-array want
     while its destination went back to the poly array, and infer_type hands
     that want back ahead of the poly fallback.
     A map/collect CallNode is the exact discriminator: this pass is the only
     producer that stamps one. The other producers stamp empty `[]` literals,
     which are ArrayNodes, so their wants are untouched. */
  int n_cleared = 0, cap_cleared = 0;
  int *cleared = NULL;
  if (c->arr_want) {
    for (int id = 0; id < c->nt->count && id < c->node_cap; id++) {
      if (!ty_is_ptr_array(c->arr_want[id])) continue;
      if (nt_kind(nt, id) != NK_CallNode) continue;
      const char *rn = nt_str(nt, id, "name");
      if (!rn || !(sp_streq(rn, "map") || sp_streq(rn, "collect"))) continue;
      c->arr_want[id] = TY_UNKNOWN;
      if (n_cleared == cap_cleared) {
        cap_cleared = cap_cleared ? cap_cleared * 2 : 16;
        int *nc = (int *)realloc(cleared, sizeof(int) * (size_t)cap_cleared);
        if (!nc) { free(cleared); cleared = NULL; n_cleared = 0; cap_cleared = 0; break; }
        cleared = nc;
      }
      cleared[n_cleared++] = id;
    }
  }
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int li = 0; li < sc->nlocals; li++) {
      LocalVar *lv = &sc->locals[li];
      if (lv->type != TY_POLY_ARRAY || lv->is_block_param || lv->rbs_seeded) continue;
      if (n >= cap) { cap *= 2; sl = (OAS *)realloc(sl, sizeof(OAS) * cap); if (!sl) { fprintf(stderr, "oom\n"); exit(1); } }
      sl[n].sidx = s; sl[n].lv = lv; sl[n].cls = -1; sl[n].alive = 1; sl[n].uf = n; sl[n].needs_cmp = 0; sl[n].row_iter = 0; sl[n].saw_call = 0; sl[n].ici = -1; sl[n].iiv = -1;
      sl[n].old_pin = lv->oa_pin; lv->oa_pin = TY_UNKNOWN; n++;
    }
  }
  /* 1b. one more slot per method whose VALUE is a poly array. Factoring a
        table out into `build_table(n)` puts the same storage in two scopes,
        and with no slot for the callee's value the caller's `t = build_table(n)`
        was an unmodeled write source that killed the caller's slot -- the
        narrowing stopped at the first method boundary. A return slot carries a
        NULL local; its scope index names the method. Bodies with no single C
        return to retype (inlined yielders, proc-form clones, synthesized and
        transplanted ones) and returns pinned by a seed stay out. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret_oa_pin == TY_INT_ARRAY_ARRAY || sc->ret_oa_pin == TY_FLOAT_ARRAY_ARRAY ||
        ty_is_obj_array(sc->ret_oa_pin))
      sc->ret = TY_POLY_ARRAY;   /* same reset */
    if (sc->ret != TY_POLY_ARRAY || !sc->name || sc->def_node < 0) continue;
    if (sc->ret_rbs_seeded || sc->ret_specialized) continue;
    /* `new` answers the object, never initialize's value: a constructor
       ending in `@items << x` has no return slot to keep alive, and one that
       joined the ivar's component through that push would only sink it */
    if (sp_streq(sc->name, "initialize")) continue;
    if (sc->yields || sc->is_proc_form || sc->is_lowered_yield ||
        sc->cs_synth || sc->is_transplanted_source) continue;
    /* a runtime protocol enters the method with no call node to vet, and the
       emitted protocol call reads the boxed array back (`5 + money` asks
       Money#coerce for a pair) */
    if (method_name_implicitly_invoked(sc->name)) continue;
    if (n >= cap) { cap *= 2; sl = (OAS *)realloc(sl, sizeof(OAS) * cap); if (!sl) { fprintf(stderr, "oom\n"); exit(1); } }
    sl[n].sidx = s; sl[n].lv = NULL; sl[n].cls = -1; sl[n].alive = 1; sl[n].uf = n; sl[n].needs_cmp = 0; sl[n].row_iter = 0; sl[n].saw_call = 0; sl[n].ici = -1; sl[n].iiv = -1;
    sl[n].old_pin = sc->ret_oa_pin; sc->ret_oa_pin = TY_UNKNOWN; n++;
  }
  /* 1c. one slot per @ivar holding a poly array (#4444). An ivar's references
        are spread over every method of its class and can leave through an
        attr_reader, so the slot is vetted the way narrow_int_table_ivars vets
        a table: every read is the receiver of a modeled op or an alias edge
        this pass follows, every write a modeled source, from methods of the
        owning class only. A reader is not an escape by itself -- each of its
        call sites on a receiver statically of this class is one more read of
        the slot -- but a dynamic receiver that could dispatch to it, a symbol
        naming it, a writer, a subclass, an ancestor that already declares the
        ivar, or a class-level (singleton) read all keep the slot boxed. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    if (cl->def_node < 0 || nt_kind(nt, cl->def_node) == NK_ModuleNode) continue;
    if (cl->is_struct || cl->is_data || cl->is_native_class || cl->is_value_type) continue;
    int has_sub = 0;
    for (int k = 0; k < c->nclasses && !has_sub; k++) if (k != ci && c->classes[k].parent == ci) has_sub = 1;
    if (has_sub) continue;
    for (int iv = 0; iv < cl->nivars; iv++) {
      const char *ivn = cl->ivars[iv];
      if (!ivn || ivn[0] != '@') continue;
      if (cl->ivar_oa_type[iv] != TY_UNKNOWN) {
        /* this pass's own narrowing from the last round: back on the poly
           array, the evidence decides again (the same reset the locals get) */
        cl->ivar_types[iv] = TY_POLY_ARRAY; cl->ivar_int_table[iv] = 0;
      }
      if (cl->ivar_types[iv] != TY_POLY_ARRAY || cl->ivar_int_table[iv]) continue;
      if (class_ivar_pinned(cl, ivn)) continue;
      const char *bare = ivn + 1;
      if (comp_is_sg_writer(cl, bare) || comp_is_sg_reader(cl, bare)) continue;
      int inherited = 0;
      for (int k = cl->parent; k >= 0; k = c->classes[k].parent)
        if (comp_ivar_index(&c->classes[k], ivn) >= 0) { inherited = 1; break; }
      if (inherited) continue;
      if (n >= cap) { cap *= 2; sl = (OAS *)realloc(sl, sizeof(OAS) * cap); if (!sl) { fprintf(stderr, "oom\n"); exit(1); } }
      sl[n].sidx = -1; sl[n].lv = NULL; sl[n].cls = -1; sl[n].alive = 1; sl[n].uf = n; sl[n].needs_cmp = 0; sl[n].row_iter = 0; sl[n].saw_call = 0; sl[n].ici = ci; sl[n].iiv = iv;
      /* An `Array[Array[Integer]]` / `Array[Array[Float]]` seed is element
         evidence, the same evidence a pushed row gives: it decides the kind
         when the program's own rows are silent (`Array.new(n) { [] }`), and
         a row of the other kind joins against it into a conflict the report
         after the fixpoint names (#4484). The seed is evidence and not a
         type pin because a pinned ivar is skipped by this very pass. */
      if (cl->ivar_oa_seed[iv] == SEED_OA_INT_TABLE) sl[n].cls = OA_CLS_IA;
      else if (cl->ivar_oa_seed[iv] == SEED_OA_FLT_TABLE) sl[n].cls = OA_CLS_FA;
      sl[n].old_pin = cl->ivar_oa_type[iv]; cl->ivar_oa_type[iv] = TY_UNKNOWN; n++;
    }
  }
  /* the drop still has to be reported (and `cleared` freed) on the way out:
     a round with no candidate slot at all still cleared this pass's wants. */
  if (n == 0) { int dropped = oa_want_dropped(c, cleared, n_cleared); free(cleared); free(sl); return dropped; }
  /* (class, ivar) -> slot + 1, so a read costs one ivar-name lookup in its
     own class rather than a scan of every slot: the scan was O(nodes x
     slots) per round and took a 100k-line program from seconds to minutes. */
  int **ivslot = (int **)calloc((size_t)(c->nclasses ? c->nclasses : 1), sizeof(int *));
  for (int i = 0; i < n; i++) {
    if (sl[i].ici < 0) continue;
    if (!ivslot[sl[i].ici]) ivslot[sl[i].ici] = (int *)calloc((size_t)c->classes[sl[i].ici].nivars, sizeof(int));
    ivslot[sl[i].ici][sl[i].iiv] = i + 1;
  }
  #define OA_IVSLOT(ci, ivn) ({ int _r = -1; if ((ci) >= 0 && (ci) < c->nclasses && ivslot[ci]) { int _iv = comp_ivar_index(&c->classes[ci], (ivn)); if (_iv >= 0) _r = ivslot[ci][_iv] - 1; } _r; })
  int nc = nt->count ? nt->count : 1;
  /* The copy of Enumerable#each_with_index (and the other yielding builtins)
     keeps both arms of `if block_given?` in the AST. Codegen folds the test,
     so exactly one arm runs for this site. The other still reads the table
     (`recv = self` in the Enumerator arm) and, unclaimed, killed the pin.
     Mark that arm dead for the rest of this pass. The cell the else arm
     allocates stays in the AST; this only stops it counting as a use. */
  unsigned char *dead = (unsigned char *)calloc((size_t)nc, 1);
  if (dead) {
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_IfNode) continue;
      int pred = nt_ref(nt, id, "predicate");
      if (pred < 0 || nt_kind(nt, pred) != NK_CallNode || nt_ref(nt, pred, "receiver") >= 0) continue;
      const char *pn = nt_str(nt, pred, "name");
      if (!pn || !sp_streq(pn, "block_given?")) continue;
      Scope *sc = comp_scope_of(c, id);
      int site = sc && sc->name ? enum_copy_site(sc->name) : -1;
      if (site < 0 || site >= nt->count || nt_kind(nt, site) != NK_CallNode) continue;
      int has_block = nt_ref(nt, site, "block") >= 0;
      int arm = -1;
      if (has_block) {
        int sub = nt_ref(nt, id, "subsequent");
        if (sub >= 0 && nt_kind(nt, sub) == NK_ElseNode) arm = sub;
      } else {
        arm = nt_ref(nt, id, "statements");
      }
      if (arm >= 0) oa_mark_subtree(nt, arm, dead);
    }
  }
  int *read_slot = (int *)malloc(sizeof(int) * nc);
  /* call_ret[id]: the return slot this CallNode's value comes from, or -1. */
  int *call_ret = (int *)malloc(sizeof(int) * nc);
  for (int id = 0; id < nc; id++) call_ret[id] = -1;
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
  /* The tail of a method body is the method's value, not a discarded
     statement. When the method has a return slot, step 5b classifies the
     tail; when it has none (its return was still unknown this round because
     the callee is defined further down the file, or is not an array at all)
     the value leaves through a return this pass does not model, so the
     callee's slot must die here rather than narrow under a caller whose own
     uses were never vetted (#4490). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->def_node < 0 || !sc->name || sc->body < 0) continue;
    if (oa_find(sl, n, s, NULL) >= 0) continue;
    int bn = 0; const int *bl = nt_arr(nt, sc->body, "body", &bn);
    if (bn > 0 && bl[bn - 1] >= 0 && bl[bn - 1] < nc) value_ok[bl[bn - 1]] = 0;
  }
  /* walk_disc[id]: a walk's value is thrown away -- a statement followed by
     another, or one of the program's top-level statements. An object array
     walk narrows only there: `y = a.each { }` or `a.each { break r }` hands
     the array (or the break value) to a consumer typed for the boxed array,
     which the narrowed sp_PtrArray is not (#4879). */
  char *walk_disc = (char *)calloc(nc, 1);
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "StatementsNode")) continue;
    int bn = 0; const int *bl = nt_arr(nt, id, "body", &bn);
    for (int i = 0; i + 1 < bn; i++) if (bl[i] >= 0 && bl[i] < nc) walk_disc[bl[i]] = 1;
  }
  { int root = nt->count > 0 ? nt->count - 1 : -1;
    for (int id = 0; id < nt->count; id++)
      if (nt_type(nt, id) && sp_streq(nt_type(nt, id), "ProgramNode")) { root = id; break; }
    int ps = root >= 0 ? nt_ref(nt, root, "statements") : -1;
    int bn = 0; const int *bl = ps >= 0 ? nt_arr(nt, ps, "body", &bn) : NULL;
    for (int i = 0; i < bn; i++) if (bl[i] >= 0 && bl[i] < nc) walk_disc[bl[i]] = 1; }
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

  /* 2. map each LocalVariableReadNode of a slot to its slot index. An ivar
        slot is read by `@x` inside an instance method of its class, and by a
        call of its attr_reader on a receiver statically of that class (or a
        bare reader call from inside the class); a read from any other class
        (a subclass, a module method, a singleton method) is a use this pass
        does not vet and kills the slot. */
  for (int id = 0; id < nt->count; id++) {
    read_slot[id] = -1;
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "LocalVariableReadNode")) {
      if (dead && dead[id]) continue;
      int sidx = c->nscope[id];
      const char *nm = nt_str(nt, id, "name");
      LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
      if (lv) read_slot[id] = oa_find(sl, n, sidx, lv);
      continue;
    }
    if (sp_streq(ty, "InstanceVariableReadNode")) {
      /* a candidate class has no subclass and no ancestor declaring the
         ivar, so the only other reader of this slot is a singleton method
         of the class itself (a class-level ivar of the same name) */
      const char *nm = nt_str(nt, id, "name");
      Scope *sc = comp_scope_of(c, id);
      if (!nm || !sc) continue;
      int i = OA_IVSLOT(sc->class_id, nm);
      if (i < 0) continue;
      if (sc->is_cmethod) sl[i].alive = 0;
      else read_slot[id] = i;
      continue;
    }
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      int args = nt_ref(nt, id, "arguments"); int an = 0;
      if (args >= 0) nt_arr(nt, args, "arguments", &an);
      if (!nm || an != 0 || nt_ref(nt, id, "block") >= 0) continue;
      int rci = -1;
      if (recv >= 0) { TyKind rt = infer_type(c, recv); if (ty_is_object(rt)) rci = ty_object_class(rt); }
      else { Scope *sc = comp_scope_of(c, id); if (sc && !sc->is_cmethod) rci = sc->class_id; }
      if (rci < 0 || !ivslot[rci] || !comp_is_reader(&c->classes[rci], nm)) continue;
      char ivn[300];
      if (strlen(nm) >= sizeof ivn - 1) continue;
      ivn[0] = '@'; strcpy(ivn + 1, nm);
      int i = OA_IVSLOT(rci, ivn);
      /* the synthesized attr_reader only: an explicit `def x; @x; end` is a
         method with a return slot, reached through call_ret in step 4 */
      if (i >= 0 && comp_method_in_chain(c, rci, nm, NULL) < 0) read_slot[id] = i;
    }
  }

  /* 3. op-assign / multi-target write forms on a slot are unmodeled -> kill. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
        sp_streq(ty, "InstanceVariableTargetNode") ||
        sp_streq(ty, "InstanceVariableAndWriteNode") ||
        sp_streq(ty, "InstanceVariableOrWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      for (int i = 0; nm && i < n; i++)
        if (sl[i].ici >= 0 && sp_streq(c->classes[sl[i].ici].ivars[sl[i].iiv], nm)) sl[i].alive = 0;
      continue;
    }
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

  /* 3a. a route that INVOKES an ivar's writer. A writer stores whatever it is
         handed, and a synthesized `attr_accessor`/`attr_writer` one does so
         with no node in the tree -- so unlike every other write source, an
         unvetted one leaves nothing behind for step 5a to classify and kill
         the slot with. Every route is named here instead: `o.t = v` and the
         multi-target form carry the setter name, the op-assign forms carry the
         BARE name (they read and write through both), and a `:t=` symbol
         anywhere is `send`/`method`/`define_method`/`alias`. Matching the name
         without resolving the receiver is the conservatism step 3 uses too.

         This replaces a blanket bail on any class DECLARING a writer (#4444),
         which cost the narrowing to writers that are never called -- ones
         spinel does not even emit. A method absent from the output should not
         change the output's types.

         Collected as DISTINCT names first, then matched against the slots
         once: a scan of every slot per setter call is the O(nodes x slots)
         shape that took a 100k-line program from seconds to minutes (the same
         trap ivslot above was introduced for). Distinct setter names are tens,
         not thousands. */
  int nwn = 0, cwn = 0; char **wn = NULL;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int trim;   /* the node names the SETTER (trim the `=`) or the attribute */
    if (sp_streq(ty, "CallNode") || sp_streq(ty, "CallTargetNode") ||
        sp_streq(ty, "SymbolNode")) trim = 1;
    else if (sp_streq(ty, "CallOperatorWriteNode") || sp_streq(ty, "CallAndWriteNode") ||
             sp_streq(ty, "CallOrWriteNode")) trim = 0;
    else continue;
    const char *nm = sp_streq(ty, "SymbolNode") ? nt_str(nt, id, "value") : nt_str(nt, id, "name");
    if (!nm) continue;
    char attr[300];
    if (trim) {
      if (!setter_base_name(nm, attr, sizeof attr)) continue;
    } else {
      if (strlen(nm) >= sizeof attr) continue;
      strcpy(attr, nm);
    }
    int seen = 0;
    for (int k = 0; k < nwn && !seen; k++) if (sp_streq(wn[k], attr)) seen = 1;
    if (seen) continue;
    if (nwn >= cwn) { cwn = cwn ? cwn * 2 : 16; wn = (char **)realloc(wn, sizeof(char *) * (size_t)cwn); if (!wn) { fprintf(stderr, "oom\n"); exit(1); } }
    wn[nwn++] = strdup(attr);
  }
  for (int i = 0; nwn && i < n; i++) {
    if (sl[i].ici < 0 || !sl[i].alive) continue;
    const char *bare = c->classes[sl[i].ici].ivars[sl[i].iiv] + 1;
    for (int k = 0; k < nwn; k++) if (sp_streq(wn[k], bare)) { sl[i].alive = 0; break; }
  }
  for (int k = 0; k < nwn; k++) free(wn[k]);
  free(wn);

  /* 4. CallNodes: classify slot-as-receiver (supported op + element evidence)
        and slot-as-arg (positional into a resolvable free method -> edge). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    if (dead && dead[id]) continue;
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
          for (int a = 0; a < argc; a++) sl[S].cls = oa_cls_join(sl[S].cls, oa_elem_evidence(c, S, argv[a]));
        else if (name && sp_streq(name, "[]=") && argc == 2) {
          /* a range-keyed []= is a splice, which the obj-array representation
             has no emitter for: keep the slot on the poly path */
          if (infer_type(c, argv[0]) == TY_RANGE) sl[S].alive = 0;
          else sl[S].cls = oa_cls_join(sl[S].cls, oa_elem_evidence(c, S, argv[1]));
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
      /* each / each_with_index / zip / map / reduce yield the row. Admitted
         here so the slot can still narrow. A nested numeric table takes all
         of them. An object array takes the walks -- each, reverse_each,
         each_entry, each_with_index, map, collect -- whose emitters bind an
         sp_X * element (#4846); a reduce, an inject or a zip (bit 2) keeps it
         boxed, those emitters walking only numeric rows. */
      else if (nested_row_iter_call(c, id)) {
        claimed[recv] = 1;
        int fold = name && (sp_streq(name, "reduce") || sp_streq(name, "inject") ||
                            sp_streq(name, "zip"));
        sl[S].row_iter |= fold ? 2 : 1;
        /* a block with a block of its own may be lifted into a proc, whose
           parameter passing does not carry a typed object element yet (bit 4) */
        if (oa_block_has_nested_block(nt, nt_ref(nt, id, "block"))) sl[S].row_iter |= 4;
        /* an each-family walk answers its receiver: one whose value is used
           hands the array on (bit 8). A map answers a new array. */
        int maps = name && (sp_streq(name, "map") || sp_streq(name, "collect"));
        if (!fold && !maps && !(id < nc && walk_disc[id])) sl[S].row_iter |= 8;
      }
      else sl[S].alive = 0;
    }
    /* Resolve the call target the way emission will: the enclosing self's
       ancestry first and a free function only as the fallback, then an
       explicit receiver -- a constant names a class, so the target is that
       class's chain of class methods; anything else goes by the receiver's
       type. -1 = unattributable here. A module of `self.` methods calling its
       own helpers is the whole shape of a Ruby module used as a namespace, and
       without the class-method arms every slot passed between two of them
       died here.

       This used to ask for the free functions FIRST, which is not what
       emission does: a same-named top-level def then took the call, so the
       real callee's parameter was never joined to the caller's slot and the
       two were narrowed apart -- an sp_PolyArray argument passed to an
       sp_PtrArray parameter, and the C build stopped (#4130). */
    int oa_tmi = -1;
    if (name) {
      if (recv < 0) {
        oa_tmi = comp_self_call_mi(c, id, name);
        /* Inside a class method, a bare `new(...)` constructs this class -- the
           `def self.load; ...; new(labels, columns, ...); end` factory shape,
           which is how the form below is actually written. There is no class
           method named `new` for the lookup to find, so the arguments' route
           into initialize's parameters was invisible here too. */
        if (oa_tmi < 0 && sp_streq(name, "new")) {
          Scope *ncs = comp_scope_of(c, id);
          if (ncs && ncs->is_cmethod && ncs->class_id >= 0)
            oa_tmi = comp_method_in_chain(c, ncs->class_id, "initialize", NULL);
        }
      }
      else if (nt_type(nt, recv) &&
               (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
                sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
        int rci = comp_class_index(c, nt_str(nt, recv, "name"));
        oa_tmi = rci >= 0 ? comp_cmethod_in_chain(c, rci, name, NULL) : -1;
        /* `T.new(cols)` names no class method -- `new` is implicit -- so the
           lookup above answered -1 and the argument loop below read that as
           "callee unattributable" and killed the caller's slot. The arguments
           really do reach T#initialize's parameters, and a table built as a
           local in a `self.load` factory and handed to the constructor is
           ordinary Ruby: it stayed boxed while the identical table assigned to
           the ivar inside `initialize` narrowed. A class that defines its own
           `self.new` is found above and keeps it. */
        if (oa_tmi < 0 && rci >= 0 && sp_streq(name, "new"))
          oa_tmi = comp_method_in_chain(c, rci, "initialize", NULL);
      }
      else {
        TyKind ort = infer_type(c, recv);
        if (ty_is_object(ort))
          oa_tmi = comp_method_in_chain(c, ty_object_class(ort), name, NULL);
        else if (ort == TY_POLY || ort == TY_UNKNOWN || ort == TY_NIL) {
          /* A dynamic receiver may dispatch to ANY same-named instance
             method at runtime (the poly dispatch arms), passing boxed
             values and expecting a boxed one back; neither a param slot nor
             a return slot reachable that way must narrow. */
          for (int pi = 0; pi < n; pi++) {
            if (!sl[pi].alive) continue;
            if (sl[pi].ici >= 0) {
              /* a dynamic receiver may be this class: its reader would hand
                 the unboxed array to a caller expecting the boxed one */
              if (sp_streq(c->classes[sl[pi].ici].ivars[sl[pi].iiv] + 1, name)) sl[pi].alive = 0;
              continue;
            }
            Scope *PS = &c->scopes[sl[pi].sidx];
            if (!PS->name || !sp_streq(PS->name, name)) continue;
            if (!sl[pi].lv) { sl[pi].alive = 0; continue; }
            for (int pk = 0; pk < PS->nparams; pk++)
              if (PS->pnames[pk] && scope_local(PS, PS->pnames[pk]) == sl[pi].lv)
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
      int R = oa_find(sl, n, oa_tmi, NULL);
      if (R >= 0 && id < nc) { call_ret[id] = R; sl[R].saw_call = 1; }
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
      /* zip's other operand is a peer array. A nested table passed there is
         still the table: claiming it lets the literal `[[10],[20]]` narrow
         instead of dying as an unmodeled builtin argument. */
      if (oa_tmi < 0) {
        if (k == 0 && name && sp_streq(name, "zip") && nested_row_iter_call(c, id)) {
          claimed[a] = 1;
          sl[S].row_iter |= 2;   /* a zip's argument: numeric rows only (#4846) */
          continue;
        }
        sl[S].alive = 0;
        continue;
      }
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
    if (dead && dead[id]) continue;
    int sidx = c->nscope[id];
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(&c->scopes[sidx], nm) : NULL;
    int S = lv ? oa_find(sl, n, sidx, lv) : -1;
    if (S < 0) continue;
    oa_classify_value(c, sl, n, read_slot, call_ret, claimed, S, nt_ref(nt, id, "value"));
  }

  /* 5a. InstanceVariableWriteNode sources, the same way; a write from a
         method of another class (or a singleton method) is unvetted. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "InstanceVariableWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    Scope *sc = comp_scope_of(c, id);
    if (!nm || !sc) continue;
    int i = OA_IVSLOT(sc->class_id, nm);
    if (i < 0) continue;
    if (sc->is_cmethod) sl[i].alive = 0;
    else oa_classify_value(c, sl, n, read_slot, call_ret, claimed, i, nt_ref(nt, id, "value"));
  }

  /* 5b. a return slot's own value: the method's tail expression and every
         explicit `return`, classified exactly like a local's write sources. */
  for (int i = 0; i < n; i++) {
    if (sl[i].lv || sl[i].ici >= 0 || !sl[i].alive) continue;
    Scope *M = &c->scopes[sl[i].sidx];
    int bn = 0; const int *bl = M->body >= 0 ? nt_arr(nt, M->body, "body", &bn) : NULL;
    if (!bl || bn == 0) { sl[i].alive = 0; continue; }
    oa_classify_value(c, sl, n, read_slot, call_ret, claimed, i, bl[bn - 1]);
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "ReturnNode")) continue;
    if (dead && dead[id]) continue;
    int R = oa_find(sl, n, c->nscope[id], NULL);
    if (R < 0) continue;
    int ra = nt_ref(nt, id, "arguments");
    int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
    if (!rv || rn != 1) { sl[R].alive = 0; continue; }
    oa_classify_value(c, sl, n, read_slot, call_ret, claimed, R, rv[0]);
  }

  /* 5c. a method reachable other than through a call site this pass resolved
         can be entered by a path that still expects the boxed value: a symbol
         anywhere names it for `send`/`method`/`define_method`/`&:m`, and
         `super` reaches it with no call node at all. Retyping its C return
         under either would hand back something the caller cannot read. */
  /* the `attr_reader :items` declaration itself names the reader with a
     symbol; that one is the reader's definition, not a dynamic route to it.
     `attr_accessor`/`attr_writer` name it the same way -- their WRITER is
     what has to be vetted, and step 3a does that by its own `:items=` name,
     so treating the declaration here as a dynamic route would kill every
     accessor slot for the bare symbol it is spelled with. */
  char *attr_sym = (char *)calloc(nc, 1);
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode") || nt_ref(nt, id, "receiver") >= 0) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn || !(sp_streq(cn, "attr_reader") || sp_streq(cn, "attr") ||
                 sp_streq(cn, "attr_accessor") || sp_streq(cn, "attr_writer"))) continue;
    int aa = nt_ref(nt, id, "arguments"); int aan = 0;
    const int *aav = aa >= 0 ? nt_arr(nt, aa, "arguments", &aan) : NULL;
    for (int k = 0; k < aan; k++) if (aav[k] >= 0 && aav[k] < nc) attr_sym[aav[k]] = 1;
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    const char *mn = NULL;
    if (sp_streq(ty, "SymbolNode")) { if (attr_sym[id]) continue; mn = nt_str(nt, id, "value"); }
    else if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode")) {
      Scope *ssc = comp_scope_of(c, id);
      mn = ssc ? ssc->name : NULL;
    }
    if (!mn) continue;
    for (int i = 0; i < n; i++) {
      if (sl[i].ici >= 0) {
        /* `send(:items)`, `instance_variable_get(:@items)`, `method(:items)`,
           a re-declared accessor: any symbol naming the ivar or its reader */
        const char *ivn = c->classes[sl[i].ici].ivars[sl[i].iiv];
        if (sp_streq(ivn, mn) || sp_streq(ivn + 1, mn)) sl[i].alive = 0;
        continue;
      }
      if (!sl[i].lv && c->scopes[sl[i].sidx].name &&
          sp_streq(c->scopes[sl[i].sidx].name, mn)) sl[i].alive = 0;
    }
  }

  /* Enumerable#each_with_index answers `self` from the block arm of
     `if block_given?`. That read is the method's value, the same table,
     not an escape. Unclaimed, it killed the table the arm walks, so a
     nested table rewritten onto the Ruby definition went back to a poly
     array. The dead arm is the same shape. */
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_IfNode) continue;
    int pred = nt_ref(nt, id, "predicate");
    if (pred < 0 || nt_kind(nt, pred) != NK_CallNode || nt_ref(nt, pred, "receiver") >= 0) continue;
    const char *pn = nt_str(nt, pred, "name");
    if (!pn || !sp_streq(pn, "block_given?")) continue;
    int arms[2];
    arms[0] = nt_ref(nt, id, "statements");
    int sub = nt_ref(nt, id, "subsequent");
    arms[1] = (sub >= 0 && nt_kind(nt, sub) == NK_ElseNode) ? nt_ref(nt, sub, "statements") : -1;
    for (int a = 0; a < 2; a++) {
      int bn = 0; const int *bb = arms[a] >= 0 ? nt_arr(nt, arms[a], "body", &bn) : NULL;
      if (!bb || bn == 0 || bb[bn - 1] < 0 || bb[bn - 1] >= nc) continue;
      if (read_slot[bb[bn - 1]] >= 0) claimed[bb[bn - 1]] = 1;
    }
  }

  /* 6. a slot read left unclaimed escaped into an unmodeled context -> kill.
        A call whose value is a return slot is the same story: unless a slot
        write claimed it above, the value went somewhere unmodeled. And a
        return slot no call site reached at all is a method entered by some
        route this pass never saw, so its C signature must not move. */
  for (int id = 0; id < nt->count; id++)
    if (call_ret[id] >= 0 && !claimed[id] && !value_ok[id]) sl[call_ret[id]].alive = 0;
  for (int i = 0; i < n; i++) if (!sl[i].lv && sl[i].ici < 0 && !sl[i].saw_call) sl[i].alive = 0;
  for (int id = 0; id < nt->count; id++)
    if (read_slot[id] >= 0 && !claimed[id]) sl[read_slot[id]].alive = 0;

  /* 7. resolve components: roll member alive/cls up to the root, then a
        component narrows only if every member is alive and one class is known. */
  for (int i = 0; i < n; i++) {
    int r = oa_uf_find(sl, i);
    if (r == i) continue;
    sl[r].cls = oa_cls_join(sl[r].cls, sl[i].cls);
    if (sl[i].needs_cmp) sl[r].needs_cmp = 1;
    sl[r].row_iter |= sl[i].row_iter;
    if (!sl[i].alive) sl[r].alive = 0;
  }
  for (int i = 0; i < n; i++) {
    int r = oa_uf_find(sl, i);
    /* A component that reaches no decision this round drops any stamp an
       earlier round left on its builder nodes. The pass is written to
       un-narrow a slot that stops qualifying (a new escape appearing as the
       fixpoint desugars), and the slot then goes back to the poly array while
       a stale want would still have the emitter build an sp_PtrArray into it.
       Used at each of the three bails that do so: no decision, and the two
       comparator fallbacks (the nested one and the object-array one).
       Not observed to fire -- across the suite, the packages and a large
       application the drop site is reached constantly but the want is always
       already UNKNOWN -- so this states the invariant rather than fixing a
       reproduced failure.
       Only THIS pass's own source nodes, which are `map` calls. The empty-row
       literal of #4484 is stamped here too and has the same exposure, but its
       node is an empty `[]`, and two other producers stamp those as well
       (mark_empty_array_operands and the ivar-write scan); clearing one here
       could drop a want this pass never set. */
    #define OA_DROP_SRC_STAMP() do { \
      for (int _e = 0; _e < g_oa_src_n; _e++) { \
        if (oa_uf_find(sl, g_oa_src_slot[_e]) != oa_uf_find(sl, i)) continue; \
        int _sn = g_oa_src_node[_e]; \
        if (c->arr_want && _sn >= 0 && _sn < c->node_cap) c->arr_want[_sn] = TY_UNKNOWN; \
      } \
    } while (0)
    /* This pass cleared every candidate's pin on the way in, but the pin field
       is also written by the element-narrowing below it. Put a pin back when
       this pass reaches no decision, or the two passes alternate forever and
       the fixpoint burns its whole round budget (#3781). */
    if (sl[i].ici >= 0 && c->classes[sl[i].ici].ivar_oa_seed[sl[i].iiv] < 0)
      c->classes[sl[i].ici].ivar_oa_conflict[sl[i].iiv] = (unsigned char)(sl[r].cls == -2);
    if (!sl[r].alive || sl[r].cls == -1 || sl[r].cls == -2) {
      OA_DROP_SRC_STAMP();
      if (sl[i].ici >= 0) continue;   /* an ivar with no decision stays the poly array it was reset to */
      /* This pass's OWN narrowing gets one round of grace: a slot can miss a
         decision for a round while the slots around it catch up (a callee's
         parameter that is not a candidate yet), and the pin carries it over.
         A second round in a row means the evidence is really gone: the slot
         was reset to the poly array at the top, and handing the withdrawn
         decision back as a pin had infer_write_types re-narrow it next round,
         only for this pass to reset it again, every round until the cap
         (#4962). */
      TyKind back = sl[i].old_pin;
      unsigned char *grace = sl[i].lv ? &sl[i].lv->oa_grace : &c->scopes[sl[i].sidx].ret_oa_grace;
      if (back == TY_INT_ARRAY_ARRAY || back == TY_FLOAT_ARRAY_ARRAY || ty_is_obj_array(back)) {
        if (*grace) { back = TY_UNKNOWN; *grace = 0; }
        else *grace = 1;
      }
      if (sl[i].lv) sl[i].lv->oa_pin = back;
      else c->scopes[sl[i].sidx].ret_oa_pin = back;
      continue;
    }
    /* A block iterator is kept for a numeric row, and for an object element
       only when it is one of the walks (bit 1): a reduce, an inject or a zip
       (bit 2) on an object array would narrow a table those emitters do not
       walk. */
    /* ... and only while nothing yet unsupported comes with it: a block that
       nests another (bit 4), a walk whose value is used (bit 8), or an
       element class that is itself Enumerable, whose Enumerable methods have
       no emitter on a typed receiver. */
    if (!OA_CLS_IS_NESTED(sl[r].cls) &&
        ((sl[r].row_iter & 14) ||
         (sl[r].row_iter && sl[r].cls >= 0 &&
          (an_class_includes_enumerable(c, sl[r].cls) ||
           comp_method_in_chain(c, sl[r].cls, "each", NULL) >= 0)))) {
      OA_DROP_SRC_STAMP();
      if (sl[i].ici >= 0) continue;
      if (sl[i].lv) sl[i].lv->oa_pin = sl[i].old_pin;
      else c->scopes[sl[i].sidx].ret_oa_pin = sl[i].old_pin;
      continue;
    }
    TyKind nty;
    if (OA_CLS_IS_NESTED(sl[r].cls)) {
      /* a nested scalar array: the codegen supports index/push/[]=/length/
         first/last but not the boxed sort/min/max comparators yet, so a
         component that used those (needs_cmp) stays on the poly path for now. */
      if (sl[r].needs_cmp) {
        OA_DROP_SRC_STAMP();
        if (sl[i].ici >= 0) continue;
        if (sl[i].lv) sl[i].lv->oa_pin = sl[i].old_pin;
        else c->scopes[sl[i].sidx].ret_oa_pin = sl[i].old_pin;
        continue;
      }
      nty = sl[r].cls == OA_CLS_IA ? TY_INT_ARRAY_ARRAY : TY_FLOAT_ARRAY_ARRAY;
      /* every empty `[]` that flowed into this component is a row: build it
         at the row kind (see oa_note_empty) */
      for (int e = 0; e < g_oa_empt_n; e++) {
        if (oa_uf_find(sl, g_oa_empt_slot[e]) != r) continue;
        int lit = g_oa_empt_node[e];
        if (c->arr_want && lit < c->node_cap && c->arr_want[lit] == TY_UNKNOWN)
          c->arr_want[lit] = sl[r].cls == OA_CLS_IA ? TY_INT_ARRAY : TY_FLOAT_ARRAY;
      }
    }
    else {
      /* a component using no-block sort/min/max narrows only when the element
         class can actually compare (has `<=>` in its chain); otherwise it stays
         poly, where the boxed comparator raises the CRuby ArgumentError. */
      if (sl[r].needs_cmp && comp_method_in_chain(c, sl[r].cls, "<=>", NULL) < 0) {
        OA_DROP_SRC_STAMP();
        if (sl[i].ici >= 0) continue;
        if (sl[i].lv) sl[i].lv->oa_pin = sl[i].old_pin;
        else c->scopes[sl[i].sidx].ret_oa_pin = sl[i].old_pin;
        continue;
      }
      nty = ty_obj_array(sl[r].cls);
    }
    if (sl[i].ici >= 0) {
      ClassInfo *cl = &c->classes[sl[i].ici];
      sp_ivwatch(cl->ivars[sl[i].iiv], "narrow_object_arrays", cl->ivar_types[sl[i].iiv], nty);
      cl->ivar_types[sl[i].iiv] = nty; cl->ivar_oa_type[sl[i].iiv] = nty; cl->ivar_int_table[sl[i].iiv] = 1;
    }
    else if (sl[i].lv) {
      sl[i].lv->type = nty; sl[i].lv->oa_pin = nty; sl[i].lv->oa_grace = 0;
    }
    else { c->scopes[sl[i].sidx].ret = nty; c->scopes[sl[i].sidx].ret_oa_pin = nty; c->scopes[sl[i].sidx].ret_oa_grace = 0; }
    /* and the sources that BUILD the table in place, so the emitter builds it
       at the narrowed kind rather than building a poly array and assigning it
       into a narrowed slot */
    for (int e = 0; e < g_oa_src_n; e++) {
      if (oa_uf_find(sl, g_oa_src_slot[e]) != oa_uf_find(sl, i)) continue;
      int src = g_oa_src_node[e];
      /* a WANT, not c->ntype: the type cache is recomputed by every later
         inference pass, so a stamp there is gone by emission. infer_type
         re-reads arr_want on each pass, which is exactly how the empty-row
         literal of an `Array.new(n) { [] }` table keeps the kind this pass
         gives it (#4484). */
      if (c->arr_want && src >= 0 && src < c->node_cap) c->arr_want[src] = nty;
    }
  }
  /* the round changed something exactly when some slot's decision differs from
     the one it carried in -- the fixpoint's convergence test depends on this
     being false once the evidence settles. */
  for (int i = 0; i < n; i++) {
    TyKind now = sl[i].ici >= 0 ? c->classes[sl[i].ici].ivar_oa_type[sl[i].iiv]
               : sl[i].lv ? sl[i].lv->oa_pin : c->scopes[sl[i].sidx].ret_oa_pin;
    if (now != sl[i].old_pin) { changed = 1; break; }
  }

  /* A want that was cleared above and NOT re-stamped is a decision that went
     away this round. infer_write_types runs before this pass in the fixpoint
     body, so it had already read that want into a destination local, and the
     `ch |= infer_write_types(c)` after this pass only runs when this pass
     reports a change -- which the pin comparison below does not notice, since
     the slot's pin is simply restored. Report it, and the write types are
     re-derived without the want. Only a genuinely dropped one: the normal
     round clears and re-stamps, and reporting that would never converge. */
  if (oa_want_dropped(c, cleared, n_cleared)) changed = 1;
  free(cleared);
  for (int k = 0; k < c->nclasses; k++) free(ivslot[k]);
  free(ivslot);
  #undef OA_IVSLOT
  free(sl); free(read_slot); free(call_ret); free(claimed); free(value_ok); free(walk_disc); free(attr_sym); free(dead);
  return changed;
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

/* ---- `rescue K => e`: an arm's reads take that arm's class (#4343) --------
   A name bound by two rescue arms interns to ONE LocalVar, so the slot cannot
   take either arm's class and lands on plain TY_EXCEPTION -- and a call naming
   a method only the user class owns then had no route at all and stopped the
   build. A read inside an arm still knows which class it is: the arm names it.
   Every user exception subclass's struct opens with the sp_Exception header,
   so reading the binding as that class there is a pointer cast, not a guess.

   Marked in c->nilnarrow, the same read-site table the nil and is_a? guards
   use, and only for the names the arm's class chain actually owns: everything
   the builtin exception surface answers (#message, #backtrace) keeps the arms
   it has today. A slot already specialized to one class needs nothing. */
static int nng_has_write(Compiler *c, int root, const char *pn);

static void nrar_mark_calls(Compiler *c, int root, Scope *s, const char *nm,
                            int cid, TyKind t) {
  if (root < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (!ty) return;
  /* a nested def has its own scope and its own `e`; the walk stops there */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode"))
    return;
  if (sp_streq(ty, "CallNode")) {
    int recv = nt_ref(nt, root, "receiver");
    const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
    const char *mn = nt_str(nt, root, "name");
    if (rty && sp_streq(rty, "LocalVariableReadNode") && mn) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, nm) && comp_scope_of(c, recv) == s &&
          (comp_method_in_chain(c, cid, mn, NULL) >= 0 ||
           comp_reader_in_chain(c, cid, mn, NULL) >= 0))
        c->nilnarrow[recv] = t;
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) nrar_mark_calls(c, nt_ref_at(nt, root, i), s, nm, cid, t);
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, root, i, &n);
    for (int k = 0; k < n; k++) nrar_mark_calls(c, ids[k], s, nm, cid, t);
  }
}

static void narrow_rescue_arm_reads(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "RescueNode")) continue;
    int ref = nt_ref(nt, id, "reference");
    if (ref < 0 || !nt_type(nt, ref) ||
        !sp_streq(nt_type(nt, ref), "LocalVariableTargetNode")) continue;
    const char *nm = nt_str(nt, ref, "name");
    if (!nm) continue;
    /* exactly one named class, and a user exception subclass */
    int nexc = 0;
    const int *exc = nt_arr(nt, id, "exceptions", &nexc);
    if (nexc != 1) continue;
    const char *en = nt_type(nt, exc[0]);
    if (!en || (!sp_streq(en, "ConstantReadNode") && !sp_streq(en, "ConstantPathNode")))
      continue;
    const char *enm = nt_str(nt, exc[0], "name");
    int cid = enm ? comp_class_index(c, enm) : -1;
    if (cid < 0 || !class_is_exc_subclass(c, cid)) continue;
    Scope *vsc = comp_scope_of(c, ref);
    LocalVar *lv = vsc ? scope_local(vsc, nm) : NULL;
    /* only the slot the arms could not agree on: a specialized one already
       reads as its class, and a builtin-typed one is what this repairs */
    if (!lv || lv->type != TY_EXCEPTION) continue;
    int body = nt_ref(nt, id, "statements");
    /* an arm that reassigns the name is no longer holding what it caught:
       the cast would be reading a class the value never was */
    if (nng_has_write(c, body, nm)) continue;
    nrar_mark_calls(c, body, vsc, nm, cid, ty_object(cid));
  }
}

/* `Cls.new(...)` / `Cls.exception(...)` of an exception class */
static int value_is_new_exception(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return 0;
  const char *m = nt_str(nt, v, "name");
  if (!m || (!sp_streq(m, "new") && !sp_streq(m, "exception"))) return 0;
  int r = nt_ref(nt, v, "receiver");
  if (r < 0 || (nt_kind(nt, r) != NK_ConstantReadNode && nt_kind(nt, r) != NK_ConstantPathNode)) return 0;
  const char *cn = nt_str(nt, r, "name");
  if (!cn) return 0;
  int cid = comp_class_index(c, cn);
  return cid >= 0 ? class_is_exc_subclass(c, cid) : is_builtin_exception_name(cn);
}

/* Whether a local that a `rescue => name` arm binds is also written by an
   ordinary assignment in the same scope; a target is ordinary unless it is a
   rescue arm's own reference. */
static int rescue_name_written_elsewhere(Compiler *c, Scope *vsc, const char *nm,
                                         const int *rescues, int nrescues) {
  const NodeTable *nt = c->nt;
  static const NodeKind kinds[] = {
    NK_LocalVariableWriteNode, NK_LocalVariableOperatorWriteNode,
    NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode,
    NK_LocalVariableTargetNode,
  };
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    NT_FOREACH_KIND(nt, kinds[k], id) {
      const char *wn = nt_str(nt, id, "name");
      if (!wn || !sp_streq(wn, nm) || comp_scope_of(c, id) != vsc) continue;
      if (kinds[k] == NK_LocalVariableTargetNode) {
        int own = 0;
        for (int r = 0; r < nrescues && !own; r++) own = nt_ref(nt, rescues[r], "reference") == id;
        if (own) continue;
      }
      /* an exception built in place (`e = MyErr.new(...)`) is what the
         exception slot holds anyway */
      if (kinds[k] == NK_LocalVariableWriteNode && value_is_new_exception(c, nt_ref(nt, id, "value")))
        continue;
      return 1;
    }
  }
  return 0;
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

/* An empty `[]` infers TY_UNKNOWN so `x = []; x << 1` can back-fill from the
   writes. A literal consumed on the spot has no writes to wait for, so it takes
   its kind from the use: a receiver or interpolation is a poly array; an
   argument to a typed-array method is that array's kind when the method
   combines arrays (`+`, `concat`, `==`, ...) and a poly array otherwise.
   Runs after the fixpoint, where receiver kinds are settled. */
static int empty_arr_same_kind_method(const char *nm) {
  static const char *const ok[] = {
    "+", "-", "&", "|", "==", "!=", "<=>", "eql?", "concat", "replace",
    "union", "intersection", "difference", "intersect?", "equal?", NULL };
  for (int i = 0; ok[i]; i++) if (sp_streq(nm, ok[i])) return 1;
  return 0;
}
static int is_empty_array_literal(const NodeTable *nt, int id, int cap) {
  if (id < 0 || id >= cap || nt_kind(nt, id) != NK_ArrayNode) return 0;
  int en = 0; nt_arr(nt, id, "elements", &en);
  return en == 0;
}
static void mark_empty_array_operands(Compiler *c) {
  if (!c->empty_arr_recv || !c->arr_want) return;
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_EmbeddedStatementsNode, es) {
    int st = nt_ref(nt, es, "statements");
    int bn = 0; const int *bb = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
    if (bn == 1 && is_empty_array_literal(nt, bb[0], c->node_cap)) c->empty_arr_recv[bb[0]] = 1;
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || recv >= c->node_cap) continue;
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    int recv_empty = is_empty_array_literal(nt, recv, c->node_cap);
    /* an inject / each_with_object seed is written through the block param
       and back-fills like a local: leave it */
    int seed = nt_ref(nt, id, "block") >= 0 && nm &&
               (sp_streq(nm, "each_with_object") || sp_streq(nm, "inject") || sp_streq(nm, "reduce"));
    /* `f.call([])` on a Proc: the parameter takes its type from the argument,
       and an empty literal has none of its own, so the parameter defaulted to
       int and the literal was still built as a container -- an sp_IntArray *
       into an sp_int slot, which did not compile (#4295). A plain method's
       parameter already reads it as a container; give the proc parameter the
       same poly layout. `.()` and `f[x]` parse to the same names. */
    if (an > 0 && nm && !recv_empty &&
        (sp_streq(nm, "call") || sp_streq(nm, "()") || sp_streq(nm, "yield") ||
         sp_streq(nm, "[]")) &&
        infer_type(c, recv) == TY_PROC) {
      for (int k = 0; k < an; k++) {
        if (is_empty_array_literal(nt, av[k], c->node_cap) && c->arr_want[av[k]] == TY_UNKNOWN)
          c->arr_want[av[k]] = TY_POLY_ARRAY;
        /* the same for `{}`: the proc-call argument emitter asks the node's
           type to decide whether the value is storable, and an untyped empty
           literal made it declare an sp_int temp and box nil, while the
           literal itself still built a hash */
        else if (c->hash_want && av[k] < c->node_cap && c->hash_want[av[k]] == TY_UNKNOWN) {
          NodeKind hk = nt_kind(nt, av[k]);
          int hen = 0;
          if (hk == NK_HashNode || hk == NK_KeywordHashNode) {
            nt_arr(nt, av[k], "elements", &hen);
            if (hen == 0) c->hash_want[av[k]] = TY_STR_POLY_HASH;
          }
        }
      }
    }
    if (an > 0 && nm && !recv_empty) {
      TyKind rt = infer_type(c, recv);
      if ((ty_is_array(rt) && !seed) || ty_is_hash(rt)) {
        TyKind want = (ty_is_array(rt) && rt != TY_POLY_ARRAY && empty_arr_same_kind_method(nm))
                      ? rt : TY_POLY_ARRAY;
        for (int k = 0; k < an; k++)
          if (is_empty_array_literal(nt, av[k], c->node_cap) && c->arr_want[av[k]] == TY_UNKNOWN)
            c->arr_want[av[k]] = want;
      }
    }
    if (!recv_empty) continue;
    /* `[] + a` takes a's kind ... */
    if (an == 1 && nm && empty_arr_same_kind_method(nm) && c->arr_want[recv] == TY_UNKNOWN) {
      TyKind at = infer_type(c, av[0]);
      if (ty_is_array(at) && at != TY_POLY_ARRAY) { c->arr_want[recv] = at; continue; }
    }
    /* When both operands are bare `[]`, the receiver becomes a poly array but
       the argument has no independently inferred kind. Give that argument the
       same poly layout before codegen; otherwise `[] == []` / `[] | []` pass
       the default sp_IntArray* into a path expecting sp_PolyArray*. Seeds keep
       their existing back-fill behavior through the block parameter. */
    if (!seed) {
      for (int k = 0; k < an; k++)
        if (is_empty_array_literal(nt, av[k], c->node_cap) && c->arr_want[av[k]] == TY_UNKNOWN)
          c->arr_want[av[k]] = TY_POLY_ARRAY;
    }
    /* ... any other bare `[]` receiver is an empty poly array */
    c->empty_arr_recv[recv] = 1;
    /* The marking gives the receiver an element type, but a block parameter is
       interned only where something reads it. With neither -- `[].map { |x| 1 }`
       -- no slot exists at all, while the element loop binds `lv_x` anyway and
       named an undeclared identifier (#3921). The element of a marked empty
       literal is boxed, so claim the slot here. */
    { int bp = nt_ref(nt, id, "block");
      int pn = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
      int pp = pn >= 0 ? nt_ref(nt, pn, "parameters") : -1;
      int rn = 0; const int *reqs = pp >= 0 ? nt_arr(nt, pp, "requireds", &rn) : NULL;
      Scope *bs = reqs ? comp_scope_of(c, bp) : NULL;
      for (int k = 0; bs && k < rn; k++) {
        const char *pnm = nt_str(nt, reqs[k], "name");
        if (!pnm || scope_local(bs, pnm)) continue;
        LocalVar *plv = scope_local_intern(bs, pnm);
        if (plv) { plv->type = TY_POLY; plv->is_block_param = 1; }
      }
    }
  }
}
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
  /* Mark the empty literal that ends `stmts`, the body of something whose
     value that literal is. */
  #define MARK_EMPTY_TAIL(stmts) do { \
    int bn_ = 0; const int *bb_ = nt_arr(nt, (stmts), "body", &bn_); \
    if (bn_ > 0) { \
      int last_ = bb_[bn_ - 1]; \
      const char *lty_ = (last_ >= 0 && last_ < c->node_cap) ? nt_type(nt, last_) : NULL; \
      int en_ = 0; \
      if (lty_ && sp_streq(lty_, "ArrayNode")) { \
        nt_arr(nt, last_, "elements", &en_); \
        if (en_ == 0) c->empty_arr_recv[last_] = 1; \
      } \
      else if (lty_ && (sp_streq(lty_, "HashNode") || sp_streq(lty_, "KeywordHashNode"))) { \
        nt_arr(nt, last_, "elements", &en_); \
        if (en_ == 0) c->empty_hash_recv[last_] = 1; \
      } \
    } \
  } while (0)
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->body < 0 || (has_ret && has_ret[s])) continue;
    if (nt_kind(nt, sc->body) != NK_StatementsNode) continue;
    MARK_EMPTY_TAIL(sc->body);
  }
  /* A block's value is its tail the same way a method's is, but only a DEF
     scope carries a body, so a block was never reached here: `x.then { [] }`
     takes the block's value as its own and had none to take, which declared
     the C temp `void` (#3930). A `return` inside leaves the method rather than
     the block, so those are skipped exactly as a method's are. */
  NT_FOREACH_KIND(nt, NK_BlockNode, blk) {
    int bs = nt_ref(nt, blk, "body");
    if (bs < 0 || nt_kind(nt, bs) != NK_StatementsNode) continue;
    Scope *sc = comp_scope_of(c, blk);
    if (has_ret && sc && has_ret[(int)(sc - c->scopes)]) continue;
    MARK_EMPTY_TAIL(bs);
  }
  #undef MARK_EMPTY_TAIL
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
     large apps (lobsters: ~72% of compile time was this triple loop). The
     state on entry is put back on exit: this also runs inside the fixpoint,
     after a builtin call is rewritten (b29ec9a7), where the index is frozen
     already, and leaving it unfrozen there made every lookup of the rest of
     the analysis a linear scan over the scopes (#4662). */
  int was_frozen = comp_scope_index_is_frozen();
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
  /* `h = {}; yield h` hands the hash to a block the method does not see:
     the block fills it (`{ |t| t[:a] = 1 }`) with keys this scope has no
     evidence of, and the String-keyed default then refused the store. Give
     it the widest hash, as an argument to a yielding method gets. */
  NT_FOREACH_KIND(nt, NK_YieldNode, id) {
    int anode = nt_ref(nt, id, "arguments");
    int an = 0; const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      int a = args[j];
      if (a < 0 || a >= c->node_cap) continue;
      NodeKind ak = nt_kind(nt, a);
      if (ak == NK_HashNode) {
        int en = 0; nt_arr(nt, a, "elements", &en);
        if (en == 0) c->empty_hash_arg[a] = 1;
        continue;
      }
      if (ak != NK_LocalVariableReadNode) continue;
      const char *ln = nt_str(nt, a, "name");
      Scope *asc = comp_scope_of(c, a);
      if (!ln || !asc || !local_all_writes_empty_hash(c, asc, ln)) continue;
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
  free(mm);
  free(nix);
  comp_scope_index_set_frozen(was_frozen);
}
/* An empty `{}` compared against a hash-typed peer takes that peer's variant,
   so the two sides share a representation and `==` can compare contents
   instead of collapsing to a constant false (#3040). The literal carries no
   keys of its own, so adopting the peer's variant loses nothing. */
static void mark_empty_hash_cmp_peers(Compiler *c) {
  if (!c->hash_want) return;
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
      if (ty_is_hash(pt)) c->hash_want[lit] = pt;
    }
  }
}
/* The key class a literal AST node names, as a bit: a hash whose keys are not
   all one class needs the general boxed variant. 0 for anything else, which
   makes the caller decline rather than guess. */
static const MpIx *g_hkb_wix;   /* (name, scope) -> local writes, while set */
static unsigned hash_key_bit(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *t = id >= 0 ? nt_type(nt, id) : NULL;
  if (!t) return 0;
  if (sp_streq(t, "StringNode") || sp_streq(t, "InterpolatedStringNode")) return 1u;
  if (sp_streq(t, "SymbolNode")) return 2u;
  if (sp_streq(t, "IntegerNode")) return 4u;
  /* `k = "x"; h[k] = 9`: a local key names its class through its writes, and
     only when every one of them agrees. */
  if (sp_streq(t, "LocalVariableReadNode")) {
    const char *kn = nt_str(nt, id, "name");
    const Scope *ks = comp_scope_of(c, id);
    unsigned b = 0;
    if (!kn || !ks) return 0;
    /* the local's own writes from mark_mixed_key_hash_locals' index when it
       is set; the answer does not depend on their order */
    int nwi = 0; const int *wis = NULL;
    if (g_hkb_wix) wis = mp_get(g_hkb_wix, kn, (int)(ks - c->scopes), &nwi);
    for (int wi = 0, w = 0; g_hkb_wix ? wi < nwi : w < nt->count; g_hkb_wix ? wi++ : w++) {
      if (g_hkb_wix) w = wis[wi];
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, kn) || comp_scope_of(c, w) != ks) continue;
      int wv = nt_ref(nt, w, "value");
      const char *wt = wv >= 0 ? nt_type(nt, wv) : NULL;
      unsigned wb = 0;
      if (wt && (sp_streq(wt, "StringNode") || sp_streq(wt, "InterpolatedStringNode"))) wb = 1u;
      else if (wt && sp_streq(wt, "SymbolNode")) wb = 2u;
      else if (wt && sp_streq(wt, "IntegerNode")) wb = 4u;
      if (!wb) return 0;
      b |= wb;
    }
    return (b && !(b & (b - 1))) ? b : 0;
  }
  return 0;
}
/* A hash literal assigned to a local the program later indexes with a key of
   another class. The variant came from the literal alone, so the write went in
   under the WRONG key type -- a Symbol stored into a String-keyed hash became a
   String, into an Integer-keyed one became its symbol id -- or the two sides
   named different C structs for the same object and the build failed. The
   mixed-key literal already answers with the general boxed hash; give this
   literal the same answer, before the fixpoint so every use agrees (#3927). */
typedef struct { const Scope *sc; const char *nm; unsigned bits; } HashKeyUse;
/* The key classes a call writes into its local receiver, or 0 when the call is
   not such a write or its key class is not settled in the AST. */
static unsigned hash_key_write_bits(Compiler *c, int id, const Scope **sc, const char **nm) {
  const NodeTable *nt = c->nt;
  /* `h[:x] ||= v` and `h[:x] &&= v` store under the key too (#4531) */
  if (nt_kind(nt, id) == NK_IndexOrWriteNode || nt_kind(nt, id) == NK_IndexAndWriteNode ||
      nt_kind(nt, id) == NK_IndexOperatorWriteNode) {
    int wr = nt_ref(nt, id, "receiver");
    if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) return 0;
    int wa = nt_ref(nt, id, "arguments"); int wc = 0;
    const int *wv = wa >= 0 ? nt_arr(nt, wa, "arguments", &wc) : NULL;
    if (!wv || wc != 1) return 0;
    unsigned b = hash_key_bit(c, wv[0]);
    if (!b) return 0;
    *sc = comp_scope_of(c, wr);
    *nm = nt_str(nt, wr, "name");
    return (*sc && *nm) ? b : 0;
  }
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *wn = nt_str(nt, id, "name");
  if (!wn) return 0;
  int set = sp_streq(wn, "[]=") || sp_streq(wn, "store");
  int upd = sp_streq(wn, "update") || sp_streq(wn, "merge!");
  if (!set && !upd) return 0;
  int wr = nt_ref(nt, id, "receiver");
  if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) return 0;
  int wa = nt_ref(nt, id, "arguments"); int wc = 0;
  const int *wv = wa >= 0 ? nt_arr(nt, wa, "arguments", &wc) : NULL;
  if (!wv || wc < 1) return 0;
  unsigned b = 0;
  if (set) { if (wc != 2) return 0; b = hash_key_bit(c, wv[0]); }
  else {
    /* `h.update({ "x" => 9 })`: the merged literal's keys land in h. */
    for (int k = 0; k < wc; k++) {
      if (!nt_type(nt, wv[k]) || !sp_streq(nt_type(nt, wv[k]), "HashNode")) return 0;
      int hn = 0; const int *hels = nt_arr(nt, wv[k], "elements", &hn);
      for (int e = 0; e < hn; e++) {
        if (!nt_type(nt, hels[e]) || !sp_streq(nt_type(nt, hels[e]), "AssocNode")) return 0;
        unsigned eb = hash_key_bit(c, nt_ref(nt, hels[e], "key"));
        if (!eb) return 0;
        b |= eb;
      }
    }
  }
  if (!b) return 0;
  *sc = comp_scope_of(c, wr);
  *nm = nt_str(nt, wr, "name");
  return (*sc && *nm) ? b : 0;
}
/* The hash literals a value expression copies its keys from: the literal
   itself, `lit.dup` / `lit.clone`, a local that a write in the same scope
   gave such a literal (`g = {...}; h = g.dup`), or a PARAMETER, which
   stands for the literals passed in its position at every call of the
   method (`def parse(arg) = parse_hash(arg.dup)`). A copy has its source's
   variant, so the widening lands on the source literal; without this a
   Symbol stored into a dup'ed String-keyed hash raised, and into a dup'ed
   Integer-keyed one landed as INT64_MIN (#4540). Appends to `out`, up to
   `cap`; answers the count. */
static int hash_literal_sources(Compiler *c, int val, int depth, int *out, int cap, int n) {
  const NodeTable *nt = c->nt;
  if (val < 0 || depth > 6 || n >= cap) return n;
  const char *vt = nt_type(nt, val);
  if (!vt) return n;
  if (sp_streq(vt, "HashNode")) { out[n++] = val; return n; }
  if (sp_streq(vt, "CallNode")) {
    const char *cn = nt_str(nt, val, "name");
    if (!cn || (!sp_streq(cn, "dup") && !sp_streq(cn, "clone"))) return n;
    if (nt_ref(nt, val, "arguments") >= 0 || nt_ref(nt, val, "block") >= 0) return n;
    return hash_literal_sources(c, nt_ref(nt, val, "receiver"), depth + 1, out, cap, n);
  }
  if (sp_streq(vt, "LocalVariableReadNode")) {
    const char *ln = nt_str(nt, val, "name");
    const Scope *ls = comp_scope_of(c, val);
    if (!ln || !ls) return n;
    int pidx = -1;
    for (int p = 0; p < ls->nparams; p++)
      if (ls->pnames[p] && sp_streq(ls->pnames[p], ln)) { pidx = p; break; }
    if (pidx >= 0 && ls->name) {
      /* a parameter: the arguments in its position at every call by name */
      for (int id = 0; id < nt->count && n < cap; id++) {
        if (nt_kind(nt, id) != NK_CallNode) continue;
        const char *cn = nt_str(nt, id, "name");
        if (!cn || !sp_streq(cn, ls->name)) continue;
        int ca = nt_ref(nt, id, "arguments"); int cc = 0;
        const int *cv = ca >= 0 ? nt_arr(nt, ca, "arguments", &cc) : NULL;
        if (!cv || pidx >= cc) continue;
        n = hash_literal_sources(c, cv[pidx], depth + 1, out, cap, n);
      }
      return n;
    }
    for (int w = 0; w < nt->count && n < cap; w++) {
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, ln) || comp_scope_of(c, w) != ls) continue;
      n = hash_literal_sources(c, nt_ref(nt, w, "value"), depth + 1, out, cap, n);
    }
    return n;
  }
  return n;
}
/* widen `lit` to the boxed variant when the key classes written into it
   (`bits`) and its own keys together name more than one class */
static void hash_literal_widen_if_mixed(Compiler *c, int lit, unsigned bits) {
  const NodeTable *nt = c->nt;
  if (lit < 0 || lit >= c->node_cap || !nt_type(nt, lit) || !sp_streq(nt_type(nt, lit), "HashNode")) return;
  unsigned mask = bits;
  int en = 0; const int *els = nt_arr(nt, lit, "elements", &en);
  for (int e = 0; e < en; e++) {
    if (!nt_type(nt, els[e]) || !sp_streq(nt_type(nt, els[e]), "AssocNode")) return;
    unsigned b = hash_key_bit(c, nt_ref(nt, els[e], "key"));
    if (!b) return;
    mask |= b;
  }
  if (mask && (mask & (mask - 1))) c->hash_want[lit] = TY_POLY_POLY_HASH;
}
static void mark_mixed_key_hash_locals(Compiler *c) {
  if (!c->hash_want) return;
  const NodeTable *nt = c->nt;
  HashKeyUse *uses = NULL; int nu = 0, cap = 0;
  /* (name, scope) -> the use's index, and -> the local's writes: the uses
     were found by a linear search per node, and a key's writes by a walk of
     the table per key (rubys in #5035) */
  MpIx uix = {0}, wix = {0};
  for (int id = comp_kind_first(c, NK_LocalVariableWriteNode); id >= 0; id = comp_kind_next(c, id)) {
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, id, "name");
    if (wn) mp_add(&wix, wn, c->nscope[id], id);
  }
  g_hkb_wix = &wix;   /* hash_key_write_bits reads keys through it too */
  for (int id = 0; id < nt->count; id++) {
    const Scope *sc = NULL; const char *nm = NULL;
    unsigned b = hash_key_write_bits(c, id, &sc, &nm);
    if (!b) continue;
    int f = -1;
    { int nf = 0; const int *fs = mp_get(&uix, nm, (int)(sc - c->scopes), &nf);
      if (nf > 0) f = fs[0]; }
    if (f >= 0) { uses[f].bits |= b; continue; }
    if (nu >= cap) {
      cap = cap ? cap * 2 : 16;
      uses = realloc(uses, sizeof(*uses) * (size_t)cap);
      if (!uses) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    uses[nu].sc = sc; uses[nu].nm = nm; uses[nu].bits = b;
    mp_add(&uix, nm, (int)(sc - c->scopes), nu);
    nu++;
  }
  if (!nu) { g_hkb_wix = NULL; free(uses); mp_free(&uix); mp_free(&wix); return; }
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    int val = nt_ref(nt, id, "value");
    const char *vt = val >= 0 ? nt_type(nt, val) : NULL;
    int hash_new = 0;
    if (vt && sp_streq(vt, "CallNode") && nt_str(nt, val, "name") &&
        sp_streq(nt_str(nt, val, "name"), "new")) {
      int hr = nt_ref(nt, val, "receiver");
      hash_new = hr >= 0 && nt_kind(nt, hr) == NK_ConstantReadNode &&
                 nt_str(nt, hr, "name") && sp_streq(nt_str(nt, hr, "name"), "Hash");
    }
    const char *lname = nt_str(nt, id, "name");
    const Scope *ls = comp_scope_of(c, id);
    if (!lname || !ls) continue;
    unsigned mask = 0;
    { int nf = 0; const int *fs = mp_get(&uix, lname, (int)(ls - c->scopes), &nf);
      if (nf > 0) mask = uses[fs[0]].bits; }
    if (!mask) continue;
    /* a copy of a literal (`{...}.dup`, `g.dup` with g a literal or a
       parameter) takes the literal's variant: widen the literals (#4540) */
    if (!hash_new && !(vt && sp_streq(vt, "HashNode"))) {
      int srcs[32]; int ns = hash_literal_sources(c, val, 0, srcs, 32, 0);
      for (int q = 0; q < ns; q++) hash_literal_widen_if_mixed(c, srcs[q], mask);
      continue;
    }
    if (!vt || (!sp_streq(vt, "HashNode") && !hash_new) || val >= c->node_cap) continue;
    int en = 0; const int *els = hash_new ? NULL : nt_arr(nt, val, "elements", &en);
    for (int k = 0; k < en; k++) {
      if (!nt_type(nt, els[k]) || !sp_streq(nt_type(nt, els[k]), "AssocNode")) { mask = 0; break; }
      unsigned b = hash_key_bit(c, nt_ref(nt, els[k], "key"));
      if (!b) { mask = 0; break; }
      mask |= b;
    }
    /* more than one key class in play: only the boxed variant holds them all */
    if (mask && (mask & (mask - 1))) c->hash_want[val] = TY_POLY_POLY_HASH;
  }
  /* The same through a PARAMETER: `def f(h) = h[:x] = 1` stores into the
     literal a caller passed, `f({ "x" => 0 })`, and that literal's variant
     came from its own keys alone, so the Symbol went in as a String or was
     dropped (#4531). Each use on a parameter reaches the hash literals in
     that position at every call of a method with that name. */
  for (int k = 0; k < nu; k++) {
    const Scope *ps = uses[k].sc;
    int pidx = -1;
    for (int p = 0; ps && p < ps->nparams; p++)
      if (ps->pnames[p] && sp_streq(ps->pnames[p], uses[k].nm)) { pidx = p; break; }
    if (pidx < 0 || !ps->name) continue;
    for (int id = an_calls_named_first(c, ps->name); id >= 0; id = an_calls_named_next(id)) {
      if (nt_kind(nt, id) != NK_CallNode) continue;
      const char *cn = nt_str(nt, id, "name");
      if (!cn || !sp_streq(cn, ps->name)) continue;
      int ca = nt_ref(nt, id, "arguments"); int cc = 0;
      const int *cv = ca >= 0 ? nt_arr(nt, ca, "arguments", &cc) : NULL;
      if (!cv || pidx >= cc) continue;
      /* the literal in that position, or what a dup/clone/local/parameter
         there copies from (#4540) */
      int srcs[32]; int ns = hash_literal_sources(c, cv[pidx], 0, srcs, 32, 0);
      for (int q = 0; q < ns; q++) hash_literal_widen_if_mixed(c, srcs[q], uses[k].bits);
    }
  }
  g_hkb_wix = NULL;
  free(uses); mp_free(&uix); mp_free(&wix);
}

/* A bare `{}` indexed or fetched with a statically-typed key takes the variant
   that key selects, instead of the StrPolyHash fallback that would coerce a
   Symbol key to a String (#3028) or emit ill-typed C for an Integer one
   (#3029). Values stay poly: the empty literal says nothing about them. */
static int mark_empty_hash_key_ctx(Compiler *c) {
  int changed = 0;
  if (!c->hash_want) return 0;
  const NodeTable *nt = c->nt;
  /* The `tap` / `then` / `yield_self` block parameters, collected once. The hop
     loop below follows a key operation written on such a parameter back to the
     call's own receiver (#4026), and it asked by scanning the whole node table
     -- per key site, per hop, and this pass runs on every fixpoint iteration.
     Ascending id order is the order the scan visited in, so the first match
     here is the match it found. */
  int tp_n = 0, ncall = 0;
  const char **tp_name = NULL;
  int *tp_recv = NULL;
  (void)nt_nodes_of_kind(nt, NK_CallNode, &ncall);
  if (ncall > 0) {
    /* One CallNode is the most entries this can take, so size to that and fill
       in a single walk: no growth step to get an allocation failure half way
       through, which would leave SOME blocks followed and the rest not. */
    tp_name = (const char **)malloc(sizeof(*tp_name) * (size_t)ncall);
    tp_recv = (int *)malloc(sizeof(*tp_recv) * (size_t)ncall);
    if (!tp_name || !tp_recv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    NT_FOREACH_KIND(nt, NK_CallNode, tcid) {
      const char *cn = nt_str(nt, tcid, "name");
      if (!cn || (!sp_streq(cn, "tap") && !sp_streq(cn, "then") &&
                  !sp_streq(cn, "yield_self"))) continue;
      int blk = nt_ref(nt, tcid, "block");
      if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
      const char *pn = block_param_name(c, blk, 0);
      if (!pn) continue;
      tp_name[tp_n] = pn;
      tp_recv[tp_n] = nt_ref(nt, tcid, "receiver");
      tp_n++;
    }
  }
  for (int id = 0; id < nt->count; id++) {
    NodeKind idk = nt_kind(nt, id);
    /* `h[k] ||= v` and friends select a variant by their key exactly like a
       read does, but they are not CallNodes, so the name gate below never saw
       them and an Integer key fell back to StrPolyHash (#3353) */
    int is_idx_write = (idk == NK_IndexOrWriteNode || idk == NK_IndexAndWriteNode ||
                        idk == NK_IndexOperatorWriteNode || idk == NK_IndexTargetNode);
    if (idk != NK_CallNode && !is_idx_write) continue;
    const char *nm = is_idx_write ? "[]" : nt_str(nt, id, "name");
    /* `[]=` / `store` name the key in arg 0 exactly as the reads do, and say
       the same thing about which variant the literal has to be (#4026), as do
       the other key-taking methods. */
    static const char *const key_ops[] = {
      "fetch", "[]", "[]=", "store", "key?", "has_key?", "include?", "member?",
      "dig", "slice", "except", "values_at", "fetch_values", "assoc", "delete", NULL };
    int is_key_op = 0;
    for (int k = 0; nm && key_ops[k] && !is_key_op; k++) if (sp_streq(nm, key_ops[k])) is_key_op = 1;
    if (!is_key_op) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || recv >= c->node_cap) continue;
    /* A `tap` / `then` block parameter is another name for the call's own
       receiver, so a key operation through it says the same thing about the
       literal that one written directly on it does. Without this the literal
       kept the String-keyed default and an Array key went into a `const char *`
       slot (#4026). */
    for (int hop = 0; hop < 8 && recv >= 0 && nt_kind(nt, recv) == NK_LocalVariableReadNode; hop++) {
      const char *rn2 = nt_str(nt, recv, "name");
      if (!rn2) break;
      int outer = -1;
      for (int t = 0; t < tp_n; t++)
        if (sp_streq(tp_name[t], rn2)) { outer = tp_recv[t]; break; }
      if (outer < 0) break;
      recv = outer;
    }
    if (recv < 0 || recv >= c->node_cap) continue;
    NodeKind rk = nt_kind(nt, recv);
    int direct = (rk == NK_HashNode || rk == NK_KeywordHashNode);
    if (direct) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en != 0) continue;
      if (ty_is_hash(c->hash_want[recv])) continue;   /* an earlier context won */
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
      /* The default is a value the hash answers, so it is part of the value
         type: `Hash.new("none")` written with Integers widens to a poly value
         that carries the default boxed, instead of casting the string into
         the Integer constructor and stopping the C build. Same rule the global
         and constant slots apply. */
      { int dn = recv_hash_new_default_arg(c, recv);
        if (dn >= 0) wv = ty_unify(wv, hash_default_value_ty(c, dn)); }
      want = (wv == TY_INT || wv == TY_STRING) ? ty_hash_of(TY_STRING, wv)
                                               : TY_STR_POLY_HASH;
    }
    else if (kt == TY_INT) {
      /* The same argument the String branch makes: this mark is permanent and
         only ever widens, so pinning the boxed variant throws away a value type
         the slot's own `h[k] = v` writes state right there. `[]=` joined this
         rule's name list in 9309c09f, which is when an Int-keyed literal first
         started reaching it, and every hash built only by writes lost its typed
         representation from then on. There is no Int->poly variant, so a value
         that is neither Int nor String still takes the boxed one. */
      int nw = 0;
      TyKind wv = aset_value_type_ex(c, recv, &nw);
      if (nw > 0 && wv == TY_UNKNOWN && g_infer_optimistic) continue;
      /* The default is a value the hash answers, so it is part of the value
         type: `Hash.new("none")` written with Integers widens to a poly value
         that carries the default boxed, instead of casting the string into
         the Integer constructor and stopping the C build. Same rule the global
         and constant slots apply. */
      { int dn = recv_hash_new_default_arg(c, recv);
        if (dn >= 0) wv = ty_unify(wv, hash_default_value_ty(c, dn)); }
      want = (wv == TY_INT || wv == TY_STRING) ? ty_hash_of(TY_INT, wv)
                                               : TY_POLY_POLY_HASH;
    }
    /* a boxed key (one call site passes a String, another an Integer) is only
       representable by the poly-keyed variant; the StrPolyHash default would
       hand the boxed value to a const char * slot and segfault */
    else if (kt == TY_POLY) want = TY_POLY_POLY_HASH;
    /* Every other KNOWN key kind -- an Array, a Float, a user object -- has no
       keyed variant of its own either, so the poly-keyed one is the only
       representation that holds it. Falling through here left the literal at
       the StrPolyHash default and put the key straight into a const char *
       slot, which the C compiler reported against generated code (#4000).
       TY_UNKNOWN still falls through: the key is not settled yet. */
    else if (kt != TY_UNKNOWN && kt != TY_VOID) want = TY_POLY_POLY_HASH;
    /* An empty container LITERAL as the key infers no kind at all, but it is
       still a pointer at emit time (a bare `[]` lowers to an array), so it
       needs the same widening -- the unresolved kind is what let `{}.fetch []`
       through to the C compiler. */
    else if (kt == TY_UNKNOWN &&
             (nt_kind(nt, av[0]) == NK_ArrayNode || nt_kind(nt, av[0]) == NK_HashNode))
      want = TY_POLY_POLY_HASH;
    if (!ty_is_hash(want)) continue;
    if (direct) {
      if (c->hash_want[recv] != want) { c->hash_want[recv] = want; changed = 1; }
      /* A KEY operation is stronger evidence than the bare-receiver default:
         the literal was marked as one when it reached a block-taking method
         (`{}.tap { |h| h[k] }`), and that mark answers STR_POLY ahead of the
         want below, so an Array key still went into a const char * slot
         (#4026). */
      if (c->empty_hash_recv && recv < c->node_cap && c->empty_hash_recv[recv] &&
          want != TY_STR_POLY_HASH) { c->empty_hash_recv[recv] = 0; changed = 1; }
      continue;
    }
    /* `@h = {}` in initialize, indexed with a typed key elsewhere in the same
       class: carry the context to the ivar's own empty literal, or the slot
       stays typeless and the index-write has no hash to emit against */
    if (rk == NK_InstanceVariableReadNode) {
      const char *ivn = nt_str(nt, recv, "name");
      Scope *rs = comp_scope_of(c, recv);
      int rcid = rs ? rs->class_id : -1;
      if (!ivn || rcid < 0) continue;
      /* Walk only the ivar-write nodes named `ivn` (index bucket) instead of
         rescanning the whole table per read site -- an O(sites * nodes)
         quadratic on ivar-heavy model graphs otherwise. */
      for (int r = ivw_shared_first(c, ivn); r >= 0; r = ivw_shared_next(r)) {
        int w = ivw_shared_node(r);
        if (nt_kind(nt, w) != NK_InstanceVariableWriteNode) continue;
        const char *wn = nt_str(nt, w, "name");
        Scope *ws = comp_scope_of(c, w);
        if (!wn || !sp_streq(wn, ivn) || !ws || ws->class_id != rcid) continue;
        int wv = nt_ref(nt, w, "value");
        if (wv < 0 || wv >= c->node_cap || nt_kind(nt, wv) != NK_HashNode) continue;
        int en2 = 0; nt_arr(nt, wv, "elements", &en2);
        if (en2 != 0) continue;
        /* An earlier context won -- unless THIS one is the poly-keyed variant.
           The key sites can disagree (`@h[k] = v` where k is a block param of
           a poly iteration reads as poly on one round and as the default
           literal's own symbol kind on another), and the poly-keyed hash is
           the only representation that holds both. Taking whichever landed
           first left `@units = {}` symbol-keyed while the writes carried
           String keys, and every one of them was dropped (#4041). */
        if (ty_is_hash(c->hash_want[wv]) &&
            (want != TY_POLY_POLY_HASH || c->hash_want[wv] == TY_POLY_POLY_HASH))
          continue;
        if (c->hash_want[wv] == want) continue;
        c->hash_want[wv] = want;
        changed = 1;
      }
      continue;
    }
    /* `h = {}; h.fetch(k)`: carry the key context back to the write's literal,
       so the local takes that variant instead of the StrPolyHash default */
    const char *ln = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    if (!ln || !sc) continue;
    /* `def m(k, memo = {})`: the default literal is the only thing that says
       what the parameter holds, and it has no write node to carry the key
       context back to -- so a key op on the parameter left the default at the
       StrPolyHash fallback, where an Integer key never matched (#3877). */
    for (int pi = 0; pi < sc->nparams; pi++) {
      if (!sc->pnames[pi] || !sp_streq(sc->pnames[pi], ln)) continue;
      int dn = sc->pdefault[pi];
      if (dn < 0 || dn >= c->node_cap) break;
      if (nt_kind(nt, dn) != NK_HashNode && nt_kind(nt, dn) != NK_KeywordHashNode) break;
      int den = 0; nt_arr(nt, dn, "elements", &den);
      if (den != 0 || ty_is_hash(c->hash_want[dn])) break;
      c->hash_want[dn] = want;
      changed = 1;
      break;
    }
    if (!local_all_writes_empty_hash(c, sc, ln)) continue;
    /* Walk only the writes of local `ln` in this scope (index bucket) rather
       than rescanning the whole table per read site (see the ivar case). */
    int si = (int)(sc - c->scopes);
    for (int r = lw_shared_first(c, ln, si); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, ln) || comp_scope_of(c, w) != sc) continue;
      int wv = nt_ref(nt, w, "value");
      if (wv >= 0 && wv < c->node_cap && nt_kind(nt, wv) == NK_HashNode &&
          !ty_is_hash(c->hash_want[wv])) {
        c->hash_want[wv] = want;
        changed = 1;
      }
    }
  }
  free((void *)tp_name);
  free(tp_recv);
  return changed;
}
/* A container that holds one hash is written with keys of more than one class
   across its sites: `@c = {1 => 2}` in initialize and `@c["x"] = "y"` in
   another method, or `@c = {}` with `put(1, 2)` storing through a parameter
   and `@c[:k] = 2` elsewhere. Each literal took its variant from its own keys
   or from the first key site the key context reached, so the other class's
   store went into a hash that cannot hold it: dropped, stored under a
   mistyped key, or refused at run time. mark_mixed_key_hash_locals answers
   this for locals whose keys are literals, before the fixpoint; this pass
   reads the keys' inferred types, so it runs inside it, and covers ivars,
   class variables and globals too. More than one key class, counting the
   literal's own keys, takes the poly-keyed variant, the only one that holds
   them all. So does a literal typed by its values (`{"a" => "b"}` is
   String-valued) that another site stores a value of another class into.
   The mark only ever widens. */
typedef struct { int kind; int cls; const Scope *sc; const char *nm; unsigned kbits, vbits; } HashKeySlot;
static unsigned hash_key_class_bit(TyKind kt) {
  if (kt == TY_UNKNOWN || kt == TY_VOID) return 0;
  if (kt == TY_STRING || kt == TY_STRBUF) return 1u;
  if (kt == TY_SYMBOL) return 2u;
  if (kt == TY_INT) return 4u;
  return 8u;
}
/* A boxed value is exempt: the typed setter converts it at run time (#651). */
static unsigned hash_value_class_bit(TyKind vt) {
  if (vt == TY_UNKNOWN || vt == TY_VOID || vt == TY_POLY) return 0;
  if (vt == TY_STRING || vt == TY_STRBUF) return 1u;
  if (vt == TY_INT) return 4u;
  return 8u;
}
/* The container a receiver or write node names: 0 ivar, 1 class variable
   (both by class), 2 global, 3 local (by scope). -1 for anything else. */
static int hash_key_slot_of(Compiler *c, int id, HashKeySlot *out) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  int kind = -1;
  switch (k) {
    case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode:
    case NK_InstanceVariableOrWriteNode: kind = 0; break;
    case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode:
    case NK_ClassVariableOrWriteNode: kind = 1; break;
    case NK_GlobalVariableReadNode: case NK_GlobalVariableWriteNode:
    case NK_GlobalVariableOrWriteNode: kind = 2; break;
    case NK_LocalVariableReadNode: case NK_LocalVariableWriteNode:
    case NK_LocalVariableOrWriteNode: kind = 3; break;
    default: return -1;
  }
  const char *nm = nt_str(nt, id, "name");
  Scope *s = comp_scope_of(c, id);
  if (!nm || !s) return -1;
  out->kind = kind; out->nm = nm; out->kbits = out->vbits = 0; out->cls = -1; out->sc = NULL;
  if (kind == 3) out->sc = s;
  else if (kind == 0) {
    out->cls = s->class_id >= 0 ? s->class_id : comp_class_index(c, "Toplevel");
    if (out->cls < 0) return -1;
    if (class_ivar_pinned(&c->classes[out->cls], nm)) return -1;   /* --rbs seed */
  }
  else if (kind == 1) {
    /* A class body's `@@c = {...}` sits in scope 0, outside its class: its
       owner is the class whose body states it, as infer_cvar_types reads it. */
    out->cls = s->class_id;
    for (int ci = 0; out->cls < 0 && ci < c->nclasses; ci++) {
      int body = nt_ref(nt, c->classes[ci].def_node, "body");
      int n = 0; const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
      for (int q = 0; q < n; q++) if (stmts[q] == id) { out->cls = ci; break; }
    }
    if (out->cls < 0) return -1;
  }
  return kind;
}
static int hash_key_class_under(Compiler *c, int sub, int sup) {
  for (int d = 0; sub >= 0 && d < 64; d++, sub = c->classes[sub].parent)
    if (sub == sup) return 1;
  return 0;
}
/* The same container; an ivar or class variable is also the one a subclass's
   methods write (`@c = {}` in the parent's initialize, `@c[k] = v` in the
   child). */
static int hash_key_slot_same(Compiler *c, const HashKeySlot *a, const HashKeySlot *b, int related) {
  if (a->kind != b->kind || a->sc != b->sc || !sp_streq(a->nm, b->nm)) return 0;
  if (a->cls == b->cls) return 1;
  return related && (hash_key_class_under(c, a->cls, b->cls) || hash_key_class_under(c, b->cls, a->cls));
}
static int widen_mixed_key_hash_slots(Compiler *c) {
  if (!c->hash_want) return 0;
  const NodeTable *nt = c->nt;
  HashKeySlot *slots = NULL; int ns = 0, cap = 0;
  static const NodeKind wkinds[] = {
    NK_CallNode, NK_IndexOrWriteNode, NK_IndexAndWriteNode, NK_IndexOperatorWriteNode };
  for (size_t wk = 0; wk < sizeof(wkinds) / sizeof(wkinds[0]); wk++) {
    NT_FOREACH_KIND(nt, wkinds[wk], id) {
      int is_call = wkinds[wk] == NK_CallNode;
      if (is_call) {
        const char *nm = nt_str(nt, id, "name");
        if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      }
      int recv = nt_ref(nt, id, "receiver");
      int anode = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      if (recv < 0 || !av || an != (is_call ? 2 : 1)) continue;
      HashKeySlot hs;
      if (hash_key_slot_of(c, recv, &hs) < 0) continue;
      unsigned kb = hash_key_class_bit(infer_type(c, av[0]));
      if (!kb) continue;
      /* `h[k] op= v` stores the operator's result, which the key's own reads
         decide; only a plain store and `||=` / `&&=` name the value. */
      int vnode = is_call ? av[1] : wkinds[wk] == NK_IndexOperatorWriteNode ? -1 : nt_ref(nt, id, "value");
      unsigned vb = vnode >= 0 ? hash_value_class_bit(infer_type(c, vnode)) : 0;
      int f = -1;
      for (int q = 0; q < ns; q++) if (hash_key_slot_same(c, &slots[q], &hs, 0)) { f = q; break; }
      if (f < 0) {
        if (ns >= cap) {
          cap = cap ? cap * 2 : 16;
          slots = realloc(slots, sizeof(*slots) * (size_t)cap);
          if (!slots) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        f = ns++;
        slots[f] = hs;
      }
      slots[f].kbits |= kb;
      slots[f].vbits |= vb;
    }
  }
  int changed = 0;
  static const NodeKind vkinds[] = {
    NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
    NK_ClassVariableWriteNode, NK_ClassVariableOrWriteNode,
    NK_GlobalVariableWriteNode, NK_GlobalVariableOrWriteNode,
    NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode };
  /* A slot's keys include those of every literal assigned to it: with
     `@c = {1 => 2}`, `@c = {}` and `@c["x"] = 3`, the empty literal has to
     widen too, or the two literals' variants unify to a plain boxed value
     rather than the poly-keyed hash. */
  for (size_t vk = 0; ns > 0 && vk < sizeof(vkinds) / sizeof(vkinds[0]); vk++) {
    NT_FOREACH_KIND(nt, vkinds[vk], id) {
      int val = nt_ref(nt, id, "value");
      if (val < 0 || nt_kind(nt, val) != NK_HashNode) continue;
      int en = 0; const int *els = nt_arr(nt, val, "elements", &en);
      unsigned lb = 0;
      for (int e = 0; e < en; e++) {
        if (nt_kind(nt, els[e]) != NK_AssocNode) { lb = 0; break; }
        unsigned b = hash_key_class_bit(infer_type(c, nt_ref(nt, els[e], "key")));
        if (!b) { lb = 0; break; }
        lb |= b;
      }
      if (!lb) continue;
      HashKeySlot hs;
      if (hash_key_slot_of(c, id, &hs) < 0) continue;
      for (int q = 0; q < ns; q++)
        if (hash_key_slot_same(c, &slots[q], &hs, 1)) slots[q].kbits |= lb;
    }
  }
  for (size_t vk = 0; ns > 0 && vk < sizeof(vkinds) / sizeof(vkinds[0]); vk++) {
    NT_FOREACH_KIND(nt, vkinds[vk], id) {
      int val = nt_ref(nt, id, "value");
      if (val < 0 || val >= c->node_cap || c->hash_want[val] == TY_POLY_POLY_HASH) continue;
      NodeKind vkd = nt_kind(nt, val);
      if (vkd == NK_CallNode) {
        const char *cn = nt_str(nt, val, "name");
        int hr = nt_ref(nt, val, "receiver");
        if (!cn || !sp_streq(cn, "new") || hr < 0 || nt_kind(nt, hr) != NK_ConstantReadNode ||
            !nt_str(nt, hr, "name") || !sp_streq(nt_str(nt, hr, "name"), "Hash")) continue;
      }
      else if (vkd != NK_HashNode) continue;
      HashKeySlot hs;
      if (hash_key_slot_of(c, id, &hs) < 0) continue;
      unsigned kmask = 0, vmask = 0;
      for (int q = 0; q < ns; q++)
        if (hash_key_slot_same(c, &slots[q], &hs, 1)) { kmask |= slots[q].kbits; vmask |= slots[q].vbits; }
      if (!kmask) continue;
      int en = 0; const int *els = vkd == NK_HashNode ? nt_arr(nt, val, "elements", &en) : NULL;
      for (int e = 0; e < en && kmask; e++) {
        if (nt_kind(nt, els[e]) != NK_AssocNode) { kmask = 0; break; }
        unsigned b = hash_key_class_bit(infer_type(c, nt_ref(nt, els[e], "key")));
        if (!b) { kmask = 0; break; }
        kmask |= b;
      }
      if (!kmask) continue;
      int widen = (kmask & (kmask - 1)) != 0;
      /* A non-empty literal's value class is its own; an empty one's variant
         already reads every store's value (aset_value_type_ex), and a local
         converts to the String-keyed poly variant its stores unify to. */
      if (!widen && en > 0 && hs.kind != 3) {
        TyKind lt = infer_type(c, val);
        if (lt == TY_INT_INT_HASH || lt == TY_STR_INT_HASH)
          widen = (vmask & ~4u) != 0;
        else if (lt == TY_INT_STR_HASH || lt == TY_STR_STR_HASH)
          widen = (vmask & ~1u) != 0;
      }
      if (!widen) continue;
      c->hash_want[val] = TY_POLY_POLY_HASH;
      changed = 1;
      /* The slot may already hold the literal's old variant, and two hash
         variants unify to a plain boxed value: move a typed-hash slot along
         with its literal. A local is re-typed every round. */
      TyKind *slot = NULL;
      if (hs.kind == 0) {
        ClassInfo *ci = &c->classes[hs.cls];
        int iv = comp_ivar_index(ci, hs.nm);
        if (iv >= 0) slot = &ci->ivar_types[iv];
      }
      else if (hs.kind == 1) {
        ClassInfo *ci = &c->classes[hs.cls];
        int cv = comp_cvar_index(ci, hs.nm);
        if (cv >= 0) slot = &ci->cvar_types[cv];
      }
      else if (hs.kind == 2) {
        const char *rn = comp_resolve_gvar(c, hs.nm + 1);
        LocalVar *gv = rn ? comp_gvar(c, rn) : NULL;
        if (gv) slot = &gv->type;
      }
      if (slot && ty_is_hash(*slot)) *slot = TY_POLY_POLY_HASH;
    }
  }
  free(slots);
  return changed;
}
/* `TBL = {}` followed by `TBL[k] = v` elsewhere: an empty literal bound to a
   constant has no type of its own, so the constant got no runtime slot at all
   and every read raised "uninitialized constant". Derive the variant from the
   constant's index-writes (#2879). */
/* The key a read or write on `recv` names, when `recv` is the local `lname`
   of scope `rs` (lname NULL: the constant `cn`): its type unified into *kt
   (and a write's value into *vt). */
static void hash_key_evidence(Compiler *c, int w, const char *cn, const char *lname, Scope *rs, TyKind *kt, TyKind *vt) {
  const NodeTable *nt = c->nt;
  const char *wn = nt_str(nt, w, "name");
  if (!wn) return;
  int write = sp_streq(wn, "[]=") || sp_streq(wn, "store");
  if (!write && !sp_streq(wn, "[]") && !sp_streq(wn, "fetch") && !sp_streq(wn, "dig") &&
      !sp_streq(wn, "key?") && !sp_streq(wn, "has_key?") && !sp_streq(wn, "include?") && !sp_streq(wn, "member?"))
    return;
  int wr = nt_ref(nt, w, "receiver");
  if (wr < 0) return;
  if (lname) {
    if (nt_kind(nt, wr) != NK_LocalVariableReadNode) return;
    const char *rn = nt_str(nt, wr, "name");
    if (!rn || !sp_streq(rn, lname) || comp_scope_of(c, wr) != rs) return;
  }
  else {
    if (nt_kind(nt, wr) != NK_ConstantReadNode) return;
    const char *rn = nt_str(nt, wr, "name");
    if (!rn || !sp_streq(rn, cn)) return;
  }
  int wa = nt_ref(nt, w, "arguments");
  int wan = 0; const int *wav = wa >= 0 ? nt_arr(nt, wa, "arguments", &wan) : NULL;
  if (wan < 1) return;
  *kt = ty_unify(*kt, infer_type(c, wav[0]));
  if (write && wan >= 2) *vt = ty_unify(*vt, infer_type(c, wav[1]));
}
static void mark_empty_hash_const_writes(Compiler *c) {
  if (!c->hash_want) return;
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_ConstantWriteNode) continue;
    const char *cn = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (!cn || v < 0 || v >= c->node_cap) continue;
    /* `EMPTY = {}.freeze` binds the same literal as `EMPTY = {}` (#4510) */
    if (nt_kind(nt, v) == NK_CallNode) {
      const char *fn = nt_str(nt, v, "name");
      int fr = nt_ref(nt, v, "receiver");
      int fa = nt_ref(nt, v, "arguments"); int fan = 0; if (fa >= 0) nt_arr(nt, fa, "arguments", &fan);
      if (fn && fr >= 0 && fan == 0 && nt_ref(nt, v, "block") < 0 &&
          (sp_streq(fn, "freeze") || sp_streq(fn, "dup") || sp_streq(fn, "clone") || sp_streq(fn, "itself")))
        v = fr;
      if (v >= c->node_cap) continue;
    }
    if (nt_kind(nt, v) != NK_HashNode && nt_kind(nt, v) != NK_KeywordHashNode) continue;
    int en = 0; nt_arr(nt, v, "elements", &en);
    if (en != 0 || ty_is_hash(c->hash_want[v])) continue;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int w = 0; w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_CallNode) continue;
      hash_key_evidence(c, w, cn, NULL, NULL, &kt, &vt);
    }
    /* A parameter whose default is the constant reads it under its own name:
       `def initialize(attrs = EMPTY)` followed by `attrs[:x]` says the
       constant's keys are Symbols, and the frozen empty hash shared by every
       constructor of an application has no other evidence at all (#4510). */
    NT_FOREACH_KIND(nt, NK_OptionalParameterNode, op) {
      int dv = nt_ref(nt, op, "value");
      const char *pnm = nt_str(nt, op, "name");
      /* the constant by its bare name or by a path (`Base::EMPTY`, whose node
         carries the leaf name, qualified by qc_rewrite_reads when it has to
         be), as the write above spells it (#4511) */
      if (dv < 0 || !pnm || (nt_kind(nt, dv) != NK_ConstantReadNode && nt_kind(nt, dv) != NK_ConstantPathNode)) continue;
      const char *dn = nt_str(nt, dv, "name");
      if (!dn || !sp_streq(dn, cn)) continue;
      Scope *rs = comp_scope_of(c, op);
      for (int w = 0; w < nt->count; w++) {
        if (nt_kind(nt, w) != NK_CallNode) continue;
        hash_key_evidence(c, w, cn, pnm, rs, &kt, &vt);
      }
    }
    /* No resolved index-write: the constant still needs a slot, so give it the
       widest variant rather than leaving it typeless (and unreachable). */
    TyKind want = (kt == TY_SYMBOL) ? TY_SYM_POLY_HASH
                : (kt == TY_UNKNOWN) ? TY_POLY_POLY_HASH : ty_hash_of(kt, vt);
    if (!ty_is_hash(want)) want = (kt == TY_STRING) ? TY_STR_POLY_HASH : TY_POLY_POLY_HASH;
    c->hash_want[v] = want;
  }
}
/* An Enumerable method called on a LOCAL that only ever holds a bare `{}`:
   the literal has no key context to pick a variant from, so the local stayed
   typeless and the call found no hash arm at all (#3829). A direct `{}.each {}`
   receiver already takes the bare-hash default; a local holding one is the
   same hash. Skipped when a key operation on the local exists, since that is
   what picks a variant and this mark would pre-empt it. */
static void mark_empty_hash_enum_locals(Compiler *c) {
  if (!c->empty_hash_recv) return;
  const NodeTable *nt = c->nt;
  static const char *const enum_names[] = {
    "each_entry", "each_cons", "each_slice", "cycle", "grep", "grep_v",
    "take_while", "drop_while", "chunk_while", "slice_when", "slice_before",
    "slice_after", "minmax", "minmax_by", "zip", "find_index", "flat_map",
    "collect_concat", "sort", "uniq", NULL
  };
  static const char *const key_names[] = {
    "[]", "[]=", "fetch", "store", "dig", "key?", "has_key?", "include?",
    "member?", "merge", "merge!", "update", "transform_keys", "transform_values",
    "default=", NULL
  };
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int hit = 0;
    for (int k = 0; enum_names[k] && !hit; k++) if (sp_streq(nm, enum_names[k])) hit = 1;
    if (!hit) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *lname = nt_str(nt, recv, "name");
    Scope *rs = comp_scope_of(c, recv);
    if (!lname || !rs) continue;
    /* every write of the local is an empty hash literal, and no key op on it */
    int lit = -1, ok = 1;
    for (int w = 0; w < nt->count && ok; w++) {
      NodeKind wk = nt_kind(nt, w);
      if (wk == NK_LocalVariableWriteNode) {
        const char *wn = nt_str(nt, w, "name");
        if (!wn || !sp_streq(wn, lname) || comp_scope_of(c, w) != rs) continue;
        int v = nt_ref(nt, w, "value");
        if (v < 0) { ok = 0; break; }
        NodeKind vk = nt_kind(nt, v);
        int en = 0;
        if (vk == NK_HashNode || vk == NK_KeywordHashNode) nt_arr(nt, v, "elements", &en);
        else { ok = 0; break; }
        if (en != 0) { ok = 0; break; }
        lit = v;
      }
      else if (wk == NK_CallNode || wk == NK_IndexOrWriteNode ||
               wk == NK_IndexAndWriteNode || wk == NK_IndexOperatorWriteNode) {
        int r2 = nt_ref(nt, w, "receiver");
        if (r2 < 0 || nt_kind(nt, r2) != NK_LocalVariableReadNode) continue;
        const char *rn = nt_str(nt, r2, "name");
        if (!rn || !sp_streq(rn, lname) || comp_scope_of(c, r2) != rs) continue;
        const char *cn = wk == NK_CallNode ? nt_str(nt, w, "name") : "[]";
        if (!cn) continue;
        for (int k = 0; key_names[k]; k++) if (sp_streq(cn, key_names[k])) { ok = 0; break; }
      }
    }
    if (ok && lit >= 0 && lit < c->node_cap) c->empty_hash_recv[lit] = 1;
  }
}
static int is_empty_hash_literal(const NodeTable *nt, int id, int cap) {
  if (id < 0 || id >= cap) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k != NK_HashNode && k != NK_KeywordHashNode) return 0;
  int en = 0; nt_arr(nt, id, "elements", &en);
  return en == 0;
}
/* A bare `{}` receiver or interpolation takes the bare-hash default (see
   mark_empty_array_operands); a key operation refines it in mark_empty_hash_key_ctx. */
static void mark_empty_hash_receivers(Compiler *c) {
  if (!c->empty_hash_recv) return;
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int recv = nt_ref(nt, id, "receiver");
    if (!is_empty_hash_literal(nt, recv, c->node_cap)) continue;
    /* `{}.freeze` (dup, clone, itself) is the literal itself, wherever the
       call stands: its variant is the call's context's, not a receiver's
       default. Marked here, `EMPTY = {}.freeze` came out String-keyed while
       `EMPTY = {}` did not, and a constructor defaulting to it dropped out
       of every Class#new dispatch whose argument was Symbol-keyed (#4510). */
    const char *cn = nt_str(nt, id, "name");
    int ac = 0; int args = nt_ref(nt, id, "arguments"); if (args >= 0) nt_arr(nt, args, "arguments", &ac);
    if (cn && ac == 0 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(cn, "freeze") || sp_streq(cn, "dup") || sp_streq(cn, "clone") || sp_streq(cn, "itself")))
      continue;
    c->empty_hash_recv[recv] = 1;
  }
  NT_FOREACH_KIND(nt, NK_EmbeddedStatementsNode, es) {
    int st = nt_ref(nt, es, "statements");
    int bn = 0; const int *bb = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
    if (bn == 1 && is_empty_hash_literal(nt, bb[0], c->node_cap)) c->empty_hash_recv[bb[0]] = 1;
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

/* ANY method scope with this name, or -1. Byref is decided per NAME GROUP --
   every method of a name gets the ABI or none does -- so a caller asking
   "is parameter j byref here" may ask any member and get the group's answer.
   Asking for the UNIQUE one instead made a second same-named method anywhere
   in the program silently answer -1, which reads as "not byref" and is how an
   unused class dropped a callee's appends (#4390). */
static int an_any_scope_by_name(Compiler *c, const char *nm) {
  if (!nm) return -1;
  /* frozen (scope names fixed): the first scope of each name from a table
     built once per scope-index epoch; asked per call site, the scan was
     (sites x scopes) (rubys in #5035) */
  if (comp_scope_index_is_frozen()) {
    static ANameHash names; static int *first, nfirst, stamp_n = -1;
    static unsigned stamp_gen;
    if (stamp_n != c->nscopes || stamp_gen != comp_scope_index_gen()) {
      anh_free(&names); memset(&names, 0, sizeof names);
      free(first); first = NULL; nfirst = 0;
      int cap = 0;
      for (int i = 1; i < c->nscopes; i++) {
        const char *sn = c->scopes[i].name;
        if (!sn || anh_has(&names, sn)) continue;
        if (nfirst == cap) { cap = cap ? cap * 2 : 256; first = realloc(first, sizeof(int) * (size_t)cap); }
        first[nfirst++] = i;
        anh_add(&names, sn);
      }
      stamp_n = c->nscopes; stamp_gen = comp_scope_index_gen();
    }
    int k = anh_find(&names, nm);
    return k >= 0 ? first[k] : -1;
  }
  for (int i = 1; i < c->nscopes; i++)
    if (c->scopes[i].name && sp_streq(c->scopes[i].name, nm)) return i;
  return -1;
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

/* Can a call ever arrive at an instance method of this class or module? Only
   through a value that is one, so a class nobody instantiates -- and that no
   instantiated class inherits from or includes -- has neither a direct call
   site nor a poly-dispatch arm. The emitter already knows this shape: it is
   what marks such a method `__attribute__((unused))`.

   Such a method cannot disagree with a name group about an ABI, because
   nothing can call it to find out. So it neither constrains the group nor
   joins it -- which is what lets an UNUSED class stop taking the ABI away
   from a used one (#4390). Unsure answers 1: giving the group one member too
   many costs an optimisation, and one too few would hand a poly dispatch two
   arms with different C signatures. */
int an_class_can_be_reached(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses) return 1;
  if (c->classes[ci].instantiated) return 1;
  for (int j = 0; j < c->nclasses; j++) {
    if (!c->classes[j].instantiated) continue;
    for (int k = j; k >= 0; k = c->classes[k].parent)
      if (k == ci) return 1;
    /* a module reaches instances through every class that includes it */
    for (int k = j; k >= 0; k = c->classes[k].parent)
      for (int m = 0; m < c->classes[k].nincluded_mods; m++)
        if (c->classes[k].included_mods[m] == ci) return 1;
  }
  return 0;
}

/* Promote parameter `pi` for EVERY method of this name, or for none of them.
   A call site resolves by name, so every arm a poly dispatch can reach has to
   agree on the ABI. The old rule stood in for that by refusing any name
   defined twice -- right about the danger, wrong about the remedy: it also
   refused two methods that agree perfectly, so adding an UNUSED class with a
   same-named method took the ABI away from a method that had it and the
   caller's buffer came back empty (#4390). Agreeing is the thing to require,
   so require it: every member must be eligible, must have a parameter at that
   index, must have it typed String, and must not have blocked it by
   rebinding. One member that cannot take it keeps the value ABI for all. */
static int an_byref_promote_group(Compiler *c, const char *nm, int pi,
                                  const char *elig, const unsigned *blocked, int n) {
  if (!nm || pi < 0 || pi >= 32) return 0;
  for (int k = 1; k < n; k++) {
    Scope *m = &c->scopes[k];
    if (!m->name || !sp_streq(m->name, nm)) continue;
    if (m->class_id >= 0 && !m->is_cmethod && !an_class_can_be_reached(c, m->class_id)) continue;
    if (!elig[k]) return 0;
    if (pi >= m->nparams || !m->pnames[pi]) return 0;
    if (blocked[k] & (1u << pi)) return 0;
    LocalVar *q = scope_local(m, m->pnames[pi]);
    if (!q || !q->is_param || q->is_block_param || q->type != TY_STRING) return 0;
    /* celled for a proc that can outlive the call (a stored proc, a Thread
       body) is not a slot the caller can lend; celled because it is already
       byref is this group, mid-fixpoint. A cell only lifted iteration blocks
       made is consumed while the call runs, and the block reads the caller's
       slot through it exactly as when byref promoted first -- refusing it
       made the ABI depend on which of the two passes ran first, and the
       caller's buffer came back empty (#4568). */
    if (q->is_cell && !q->byref_out && q->cell_outlives) return 0;
  }
  int did = 0;
  for (int k = 1; k < n; k++) {
    Scope *m = &c->scopes[k];
    if (!m->name || !sp_streq(m->name, nm)) continue;
    if (m->class_id >= 0 && !m->is_cmethod && !an_class_can_be_reached(c, m->class_id)) continue;
    LocalVar *q = scope_local(m, m->pnames[pi]);
    if (q && !q->byref_out) { q->byref_out = 1; q->is_cell = 1; did = 1; }
  }
  return did;
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
    /* An instance method qualifies too: `def add(buf, i); buf << ...; end` is
       ordinary Ruby, and without the out-param the append landed in a copy and
       the caller's string stayed empty (#3781). The unique-name rule above
       still holds, so no override or dispatch sees a different ABI. A class
       whose instances reach the method through a poly dispatch is fine: those
       arms fill their arguments through the same emitter. */
    if (s->class_id >= 0 && !s->is_cmethod && comp_class_index(c, "Toplevel") != s->class_id &&
        (c->classes[s->class_id].is_struct || c->classes[s->class_id].is_native_class ||
         c->classes[s->class_id].is_value_type)) continue;
    if (s->rest_idx >= 0 || s->kwrest_idx >= 0 || s->npost_rest > 0) continue;
    /* An OPTIONAL parameter beside the lent one is not an ABI difference, and
       treating it as one dropped the callee's appends without a word (#4390).
       The caller materialises every default, so the callee is emitted
       fixed-arity either way: the two emits of `def fill(io, prefix = nil)`
       and of the same method with `prefix` required differ in one token, the
       parameter's own spelling. The default that is a lent parameter's own
       (`def fill(io = String.new)`) passes a fresh caller-side temp, which is
       unaliased, so the appends go nowhere -- which is what CRuby answers for
       that shape too. */
    if (s->nparams <= 0 || s->nparams > 32) continue;
    int pn = nt_ref(nt, s->def_node, "parameters");
    if (pn >= 0) {
      int kn = 0; nt_arr(nt, pn, "keywords", &kn);
      if (kn > 0) continue;
    }
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
    /* A POLY receiver reaches the method through the cls_id switch, and that
       switch hoists its arguments once, by the argument's own type, for every
       arm to share -- it has no callee to ask, and the arms are what it is
       choosing between. So it passes the value where a byref arm wants the
       slot, and the C build stops:
         expected 'const char **' but argument is of type 'const char *'
       The uniqueness rule used to hide this: two classes defining one name
       was exactly the case it refused, and a poly dispatch needs two. Making
       the group agree (df30ed28) removed that accident and left the dispatch
       unable to call what it now agrees about.
       Teaching the dispatch to lend the slot is the real answer and it is
       part of deciding what a byref slot is. Until then a name a poly
       receiver can reach keeps the value ABI, which is what it had. */
    if (sp_streq(ty, "CallNode")) {
      int rcv = nt_ref(nt, id, "receiver");
      const char *cn = nt_str(nt, id, "name");
      if (rcv >= 0 && cn && comp_ntype(c, rcv) == TY_POLY)
        for (int si = 1; si < n; si++)
          if (elig[si] && c->scopes[si].name && sp_streq(c->scopes[si].name, cn)) elig[si] = 0;
    }
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
          /* the whole name group takes it or none of it does; the cell deref
             forms the body already emits are what the ABI rides on */
          if (p && p->is_param && p->type == TY_STRING && !p->byref_out &&
              an_byref_promote_group(c, s->name, pi, elig, blocked, n))
            changed = 1;
        }
      }
      /* transitive: the param passed on into another method's byref slot */
      if (recv < 0 || (rty && (sp_streq(rty, "ConstantReadNode") ||
                               sp_streq(rty, "ConstantPathNode") ||
                               sp_streq(rty, "SelfNode")))) {
        int mi = an_any_scope_by_name(c, nm);
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
          if (p && p->is_param && p->type == TY_STRING && !p->byref_out &&
              an_byref_promote_group(c, s->name, pi, elig, blocked, n))
            changed = 1;
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
/* Both mutation probes below ask "over EVERY call whose receiver names this
   local (or this ivar), was it mutated in place?" -- and the promotion asks
   them per candidate, from inside whole-table walks, so a probe that rescans
   the node table makes the pass O(candidates x nodes). On roundhouse's
   lobsters emit (41k lines of generated Ruby, the #3115 tree one scale up)
   the two together are ~35% of compile time in a `sample` profile.

   The answer is a pure function of call-site syntax and scope homing, so one
   sweep over the CallNode kind list computes it for EVERY key at once into a
   (name, key) table the probes then read in O(1). Cached while the scope index
   is frozen and stamped with comp_scope_index_gen() -- the same invalidation
   the LWIndex behind local_all_writes_empty_hash rides; while unfrozen it
   rebuilds per query, which costs one kind-list walk, strictly less than the
   whole-table scan it replaces. */
typedef struct {
  const char **name;  /* receiver name, borrowed from the node table */
  int *key;           /* scope index (locals) / class id (ivars) */
  signed char *val;   /* 1 = mutated in place, -1 = disqualified */
  int *next;          /* next record in the same bucket, or -1 */
  int *head;          /* hash buckets: head record index, or -1 */
  int cap;            /* bucket count (power of two) */
  int n, ncap;        /* records used / allocated */
} SbMutTab;

static unsigned sb_mut_hash(const char *name, int key) {
  unsigned h = 2166136261u ^ (unsigned)key;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

/* `n` bounds the record count: the sweep adds at most one record per call
   node, so a table sized to the CallNode population never fills. */
static void sb_mut_tab_init(SbMutTab *t, int n) {
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  t->cap = cap; t->n = 0; t->ncap = n > 0 ? n : 1;
  t->name = (const char **)malloc(sizeof(const char *) * t->ncap);
  t->key = (int *)malloc(sizeof(int) * t->ncap);
  t->val = (signed char *)malloc(sizeof(signed char) * t->ncap);
  t->next = (int *)malloc(sizeof(int) * t->ncap);
  t->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) t->head[i] = -1;
}

static void sb_mut_tab_free(SbMutTab *t) {
  free((void *)t->name); free(t->key); free(t->val); free(t->next); free(t->head);
}

/* Status cell for (name, key); appends a fresh 0 cell when `add` and absent. */
static signed char *sb_mut_tab_slot(SbMutTab *t, const char *name, int key, int add) {
  unsigned h = sb_mut_hash(name, key) & (unsigned)(t->cap - 1);
  for (int r = t->head[h]; r >= 0; r = t->next[r])
    if (t->key[r] == key && sp_streq(t->name[r], name)) return &t->val[r];
  if (!add || t->n >= t->ncap) return NULL;
  int r = t->n++;
  t->name[r] = name; t->key[r] = key; t->val[r] = 0;
  t->next[r] = t->head[h]; t->head[h] = r;
  return &t->val[r];
}

/* -1 wins over 1 whichever order the calls appear, matching the scans' early
   `return -1`; 1 only lifts a cell still at 0. */
static void sb_mut_tab_note(SbMutTab *t, const char *name, int key, signed char v) {
  signed char *slot = sb_mut_tab_slot(t, name, key, 1);
  if (slot && (v < 0 || *slot == 0)) *slot = v;
}

static void sb_mut_tabs_build(Compiler *c, SbMutTab *lt, SbMutTab *it, int toplevel) {
  const NodeTable *nt = c->nt;
  int ncalls = 0;
  nt_nodes_of_kind(nt, NK_CallNode, &ncalls);
  sb_mut_tab_init(lt, ncalls);
  sb_mut_tab_init(it, ncalls);
  NT_FOREACH_KIND(nt, NK_CallNode, u) {
    int ur = nt_ref(nt, u, "receiver");
    if (ur < 0) continue;
    NodeKind rk = nt_kind(nt, ur);
    if (rk != NK_LocalVariableReadNode && rk != NK_InstanceVariableReadNode) continue;
    const char *urn = nt_str(nt, ur, "name");
    const char *un = nt_str(nt, u, "name");
    if (!urn || !un) continue;
    size_t ul = strlen(un);
    Scope *us = comp_scope_of(c, ur);
    if (rk == NK_LocalVariableReadNode) {
      if (sp_str_mutator(un, SP_MUT_LOCAL))
        sb_mut_tab_note(lt, urn, us ? (int)(us - c->scopes) : -1, 1);
      else if (ul > 0 && un[ul - 1] == '!')
        sb_mut_tab_note(lt, urn, us ? (int)(us - c->scopes) : -1, -1);
      continue;
    }
    signed char v = 0;
    if (sp_str_mutator(un, SP_MUT_IVAR)) v = 1;
    else if (sp_streq(un, "insert") || sp_streq(un, "slice!") ||
             sp_streq(un, "[]=") || sp_streq(un, "setbyte") ||
             (ul > 0 && un[ul - 1] == '!')) v = -1;
    else continue;
    int ucid = -1;
    if (us && !us->is_cmethod) ucid = us->class_id >= 0 ? us->class_id : toplevel;
    sb_mut_tab_note(it, urn, ucid, v);
  }
}

static SbMutTab sb_local_mut_tab, sb_ivar_mut_tab;
static const NodeTable *sb_mut_nt = NULL;
static int sb_mut_ntc = -1;
static int sb_mut_toplevel = -1;
static unsigned sb_mut_ntver = 0;
static unsigned sb_mut_gen = 0;
static void sb_mut_tabs_sync(Compiler *c) {
  unsigned gen = comp_scope_index_gen();
  /* A top-level ivar keys on the Toplevel class id, which comp_class_index
     only answers once that class has been seeded -- a fact that arrives
     independently of the scope-index epoch, so it joins the stamp rather than
     being resolved once and cached wrong (the #3227 P4 top-level case). */
  int toplevel = comp_class_index(c, "Toplevel");
  /* nt->version as well as count: a rename pass rewrites a call's `name` in
     place (nt_set_str) without growing the table, and this index is keyed by
     name. Worst case the version churns and we rebuild per query -- one
     kind-list walk, still less than the whole-table scan this replaces. */
  if (comp_scope_index_is_frozen() && sb_mut_nt == c->nt &&
      sb_mut_ntc == c->nt->count && sb_mut_ntver == c->nt->version &&
      sb_mut_gen == gen && sb_mut_toplevel == toplevel)
    return;
  if (sb_mut_nt) { sb_mut_tab_free(&sb_local_mut_tab); sb_mut_tab_free(&sb_ivar_mut_tab); }
  sb_mut_tabs_build(c, &sb_local_mut_tab, &sb_ivar_mut_tab, toplevel);
  sb_mut_nt = c->nt; sb_mut_ntc = c->nt->count; sb_mut_ntver = c->nt->version;
  sb_mut_gen = gen; sb_mut_toplevel = toplevel;
}

/* In-place mutation status of string local `vn` in scope `vs`:
   1 = mutated via a handle-capable mutator, -1 = disqualified (a mutator
   codegen cannot yet run in place on the handle), 0 = not mutated. */
static int strbuf_mut_kind(Compiler *c, const char *vn, Scope *vs) {
  if (!vn) return 0;
  sb_mut_tabs_sync(c);
  signed char *v = sb_mut_tab_slot(&sb_local_mut_tab, vn,
                                   vs ? (int)(vs - c->scopes) : -1, 0);
  return v ? *v : 0;
}
/* In-place mutation status of ivar `nm` of class `cid` (same contract as
   strbuf_mut_kind). The supported set is narrower: the shadow-copy shim
   cannot rename an ivar, so []=/insert/slice!/setbyte disqualify. */
/* True if ivar `nm` of class `cid` is handed to a method as an argument
   anywhere in that class's own scopes. Such a call may take the slot by
   reference, which only works when the receiver itself is a heap object. */
static int an_ivar_passed_as_arg(Compiler *c, int cid, const char *nm) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int an = 0; const int *av = nt_arr(nt, args, "arguments", &an);
    for (int k = 0; k < an; k++) {
      if (nt_kind(nt, av[k]) != NK_InstanceVariableReadNode) continue;
      const char *ivn = nt_str(nt, av[k], "name");
      if (!ivn || !nm || !sp_streq(ivn, nm)) continue;
      Scope *sc = comp_scope_of(c, av[k]);
      if (sc && sc->class_id == cid) return 1;
    }
  }
  return 0;
}

static int strbuf_ivar_mut_kind(Compiler *c, int cid, const char *nm) {
  if (!nm) return 0;
  sb_mut_tabs_sync(c);
  signed char *v = sb_mut_tab_slot(&sb_ivar_mut_tab, nm, cid, 0);
  return v ? *v : 0;
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
/* True when some value STORED into container local `contn` is a string: the
   evidence that makes a mutator on an element read out of it a string
   mutation. Without it a name-keyed mutator table cannot tell `out << "x"`
   (a string append) from `b0 << 4` (an integer shift), and an Integer element
   bound to a local was promoted to a string handle (#3971). */
/* The element a COLLECTING iterator stores into the array it builds: the tail
   of its block. `["a"].map { |s| s.dup }` fills the array with fresh mutable
   strings exactly as `["a".dup]` does, and the two shapes have to be read the
   same way -- only the literal was, so mutating an element of a mapped array
   was silently dropped (#4037). Answers -1 when the value is not one. */
static int strbuf_map_block_tail(Compiler *c, int val) {
  const NodeTable *nt = c->nt;
  if (val < 0 || nt_kind(nt, val) != NK_CallNode) return -1;
  const char *mn = nt_str(nt, val, "name");
  /* map / collect only: flat_map's block tail is an ARRAY of elements, not one
     element, so its stores are a level deeper than this reads. That shape is
     still dropped. */
  if (!mn || !(sp_streq(mn, "map") || sp_streq(mn, "collect"))) return -1;
  int blk = nt_ref(nt, val, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return -1;
  int body = nt_ref(nt, blk, "body");
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return -1;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  return bn > 0 ? bb[bn - 1] : -1;
}

/* The values node `w` stores into container local (contn, conts): the
   elements of an array/hash literal written to it (with `map_tail`, also a
   collecting iterator's block tail), or the arguments of a push/<</[]= on
   it. Fills `stores` (64 slots) and answers the count; 0 when not a store. */
static int strbuf_container_store_values(Compiler *c, int w, const char *contn, Scope *conts,
                                         int map_tail, int *stores) {
  const NodeTable *nt = c->nt;
  int nst = 0;
  NodeKind wk = nt_kind(nt, w);
  if (wk == NK_LocalVariableWriteNode) {
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, contn) || comp_scope_of(c, w) != conts) return 0;
    int val = nt_ref(nt, w, "value");
    if (val < 0) return 0;
    NodeKind vk = nt_kind(nt, val);
    if (vk == NK_ArrayNode) {
      int en = 0; const int *el = nt_arr(nt, val, "elements", &en);
      for (int e = 0; e < en && nst < 64; e++) stores[nst++] = el[e];
    }
    else if (map_tail && strbuf_map_block_tail(c, val) >= 0)
      stores[nst++] = strbuf_map_block_tail(c, val);
    else if (vk == NK_HashNode || vk == NK_KeywordHashNode) {
      int en = 0; const int *el = nt_arr(nt, val, "elements", &en);
      for (int e = 0; e < en && nst < 64; e++)
        if (nt_kind(nt, el[e]) == NK_AssocNode) stores[nst++] = nt_ref(nt, el[e], "value");
    }
  }
  else if (wk == NK_CallNode) {
    int wr = nt_ref(nt, w, "receiver");
    if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) return 0;
    const char *wrn = nt_str(nt, wr, "name");
    if (!wrn || !sp_streq(wrn, contn) || comp_scope_of(c, wr) != conts) return 0;
    const char *wcn = nt_str(nt, w, "name");
    if (!wcn) return 0;
    int a = nt_ref(nt, w, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (sp_streq(wcn, "<<") || sp_streq(wcn, "push") ||
        sp_streq(wcn, "append") || sp_streq(wcn, "unshift")) {
      for (int e = 0; e < an && nst < 64; e++) stores[nst++] = av[e];
    }
    else if (sp_streq(wcn, "[]=") && an >= 2) stores[nst++] = av[an - 1];
  }
  return nst;
}
static int strbuf_container_stores_string(Compiler *c, const char *contn, Scope *conts) {
  for (int w = 0; w < c->nt->count; w++) {
    int stores[64];
    int nst = strbuf_container_store_values(c, w, contn, conts, 1, stores);
    for (int e = 0; e < nst; e++) {
      TyKind st = stores[e] >= 0 ? infer_type(c, stores[e]) : TY_UNKNOWN;
      if (st == TY_STRING || st == TY_STRBUF) return 1;
    }
  }
  return 0;
}
/* The complement of the test above: some value stored into this container is
   provably NOT a string. Such a container is heterogeneous, so a local bound
   from one of its elements is not an alias for a string, whatever the
   mutator-name table says about the calls made on it (#4240). Only a store
   whose type the analysis actually knows counts -- an UNKNOWN one says
   nothing, and answering yes on it would give up the promotion everywhere. */
static int strbuf_container_stores_nonstring(Compiler *c, const char *contn, Scope *conts) {
  for (int w = 0; w < c->nt->count; w++) {
    int stores[64];
    int nst = strbuf_container_store_values(c, w, contn, conts, 0, stores);
    for (int e = 0; e < nst; e++) {
      TyKind st = stores[e] >= 0 ? infer_type(c, stores[e]) : TY_UNKNOWN;
      if (st == TY_UNKNOWN || st == TY_POLY || st == TY_STRING || st == TY_STRBUF)
        continue;
      return 1;
    }
  }
  return 0;
}
/* Is every write of local `rn` (read at `recv`) an array literal? Then the
   local BUILT its array and a widened slot keeps naming it; bound from an
   element or an ivar read it is another name for storage something else
   holds, and the widening's converted copy would cut the alias (#4412). */
static int pw_local_owns_array(Compiler *c, int recv, const char *rn) {
  const NodeTable *nt = c->nt;
  Scope *ls = comp_scope_of(c, recv);
  int owns = 1, saw = 0;
  for (int w = comp_kind_first(c, NK_LocalVariableWriteNode); w >= 0 && owns; w = comp_kind_next(c, w)) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, rn) || comp_scope_of(c, w) != ls) continue;
    saw = 1;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || nt_kind(nt, wv) != NK_ArrayNode) owns = 0;
  }
  return saw && owns;
}
/* Does method scope `mi` mutate its parameter `name` in place (a push, a
   store, a bang), directly or by handing it to a receiverless user method
   that does? The promote reconciliation keeps such a parameter at its kind:
   the argument is passed by reference only while the two agree (#4480). */
static int pw_scope_mutates_param(Compiler *c, int mi, const char *name) {
  static const char *const muts[] = {
    "<<", "push", "append", "unshift", "prepend", "insert", "[]=", "concat",
    "pop", "shift", "delete", "delete_at", "delete_if", "clear", "replace",
    "fill", "map!", "collect!", "select!", "filter!", "reject!", "sort!",
    "sort_by!", "uniq!", "compact!", "reverse!", "shuffle!", "rotate!",
    "slice!", "keep_if", "flatten!", NULL };
  const NodeTable *nt = c->nt;
  for (int q = 0; q < nt->count; q++) {
    if (c->nscope[q] != mi || nt_kind(nt, q) != NK_CallNode) continue;
    const char *cnm = nt_str(nt, q, "name");
    if (!cnm) continue;
    int r = nt_ref(nt, q, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode) {
      const char *rn = nt_str(nt, r, "name");
      if (rn && sp_streq(rn, name))
        for (int k = 0; muts[k]; k++) if (sp_streq(cnm, muts[k])) return 1;
    }
    if (r >= 0 && nt_kind(nt, r) != NK_SelfNode) continue;
    int aa = nt_ref(nt, q, "arguments"); int an = 0;
    const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++) {
      if (nt_kind(nt, av[k]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[k], "name");
      if (!vn || !sp_streq(vn, name)) continue;
      /* passed on: conservatively, a callee that takes it may mutate it */
      return 1;
    }
  }
  return 0;
}
/* Iterators whose block receives the ELEMENT as its first parameter. */
static int strbuf_elem_first_iterator(const char *n) {
  static const char *const names[] = {
    "each", "each_with_index", "each_with_object", "each_entry", "reverse_each",
    "cycle", "map", "collect", "map!", "collect!", "flat_map", "collect_concat",
    "select", "filter", "select!", "filter!", "find_all", "reject", "reject!",
    "delete_if", "keep_if", "filter_map", "find", "detect", "find_index",
    "index", "rindex", "count", "any?", "all?", "none?", "one?", "sum",
    "min_by", "max_by", "minmax_by", "sort_by", "sort_by!", "group_by",
    "partition", "take_while", "drop_while", "uniq", "tally_by", "to_h",
    NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}
/* The nodes that can store into a local, keyed by (name, scope): a write of
   it, and a call on it as the receiver. The question below was answered by a
   walk of the whole node table per container, and it is asked per container
   per round -- the largest single term of lobsters' analysis. Built once per
   promote_shared_stored_strings run (a round's desugars rename and re-point
   nodes in between) and again if the table grows; each list is ascending, so
   the stores are met in the order the walk met them. */
typedef struct SbStoreEnt { const char *name; Scope *sc; int *ids; int n, cap; struct SbStoreEnt *next; } SbStoreEnt;
#define SB_STORE_BUCKETS 4096
static SbStoreEnt *sb_store_tab[SB_STORE_BUCKETS];
static const NodeTable *sb_store_nt; static int sb_store_count = -1; static int sb_store_valid;
static unsigned sb_store_hash(const char *nm, Scope *sc) {
  unsigned h = 2166136261u ^ (unsigned)(uintptr_t)sc;
  for (; *nm; nm++) { h ^= (unsigned char)*nm; h *= 16777619u; }
  return h;
}
static void sb_store_clear(void) {
  for (int b = 0; b < SB_STORE_BUCKETS; b++) {
    for (SbStoreEnt *e = sb_store_tab[b]; e; ) { SbStoreEnt *nx = e->next; free(e->ids); free(e); e = nx; }
    sb_store_tab[b] = NULL;
  }
  sb_store_valid = 0;
}
static void sb_store_add(const char *nm, Scope *sc, int id) {
  unsigned b = sb_store_hash(nm, sc) % SB_STORE_BUCKETS;
  SbStoreEnt *e = sb_store_tab[b];
  while (e && !(e->sc == sc && sp_streq(e->name, nm))) e = e->next;
  if (!e) {
    e = calloc(1, sizeof *e); e->name = nm; e->sc = sc;
    e->next = sb_store_tab[b]; sb_store_tab[b] = e;
  }
  if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 4; e->ids = realloc(e->ids, sizeof(int) * (size_t)e->cap); }
  e->ids[e->n++] = id;
}
static const int *sb_store_nodes(Compiler *c, const char *nm, Scope *sc, int *n) {
  const NodeTable *nt = c->nt;
  if (!sb_store_valid || sb_store_nt != nt || sb_store_count != nt->count) {
    sb_store_clear();
    for (int id = 0; id < nt->count; id++) {
      NodeKind k = nt_kind(nt, id);
      if (k == NK_LocalVariableWriteNode) {
        const char *wn = nt_str(nt, id, "name");
        if (wn) sb_store_add(wn, comp_scope_of(c, id), id);
      }
      else if (k == NK_CallNode) {
        int r = nt_ref(nt, id, "receiver");
        if (r < 0 || nt_kind(nt, r) != NK_LocalVariableReadNode) continue;
        const char *rn = nt_str(nt, r, "name");
        if (rn) sb_store_add(rn, comp_scope_of(c, r), id);
      }
    }
    sb_store_nt = nt; sb_store_count = nt->count; sb_store_valid = 1;
  }
  for (SbStoreEnt *e = sb_store_tab[sb_store_hash(nm, sc) % SB_STORE_BUCKETS]; e; e = e->next)
    if (e->sc == sc && sp_streq(e->name, nm)) { *n = e->n; return e->ids; }
  *n = 0; return NULL;
}
static int strbuf_demand_container_stores_here(Compiler *c, const char *contn, Scope *conts) {
  const NodeTable *nt = c->nt;
  int changed = 0;
    /* every store into this container local (same scope): array/hash
       literal writes, push/<<, []= */
    int nsn = 0;
    const int *sns0 = sb_store_nodes(c, contn, conts, &nsn);
    for (int si = 0; si < nsn; si++) {
      int stores[64];
      int nst = strbuf_container_store_values(c, sns0[si], contn, conts, 1, stores);
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

/* A container that is a PARAMETER has no stores of its own: they live at the
   call sites, in whatever the callers pass. `def bang(a) = a.each { |w| w <<
   "#" }` mutates the caller's elements through the parameter, so the demand
   has to reach the caller's container -- and a caller that passes its own
   parameter on carries it one level further. Without this the callee's block
   param became a handle while the caller's array stayed typed, and the two
   did not even agree on the element's C type. A literal argument is its own
   store site: its string elements mark for the handle wrap directly. */
static int an_call_targets_scope(Compiler *c, int u, int mi2, Scope *m2);
static int an_new_recv_all_constant(Compiler *c);
static int strbuf_demand_param_container_stores(Compiler *c, const char *pn, Scope *ps, int depth) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  if (depth > 8 || !ps || !pn) return 0;
  LocalVar *plv = scope_local(ps, pn);
  if (!plv || !plv->is_param || plv->is_block_param) return 0;
  int pj = an_param_idx(ps, pn);
  if (pj < 0) return 0;
  int mi = (int)(ps - c->scopes);
  for (int u = comp_kind_first(c, NK_CallNode); u >= 0; u = comp_kind_next(c, u)) {
    if (nt_kind(nt, u) != NK_CallNode) continue;
    if (!an_call_targets_scope(c, u, mi, ps)) continue;
    int argsN = nt_ref(nt, u, "arguments");
    int uargc = 0;
    const int *uargv = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &uargc) : NULL;
    if (pj >= uargc) continue;
    int an = uargv[pj];
    NodeKind ak = nt_kind(nt, an);
    if (ak == NK_LocalVariableReadNode) {
      const char *avn = nt_str(nt, an, "name");
      Scope *avs = avn ? comp_scope_of(c, an) : NULL;
      LocalVar *alv = avs ? scope_local(avs, avn) : NULL;
      if (!alv) continue;
      if (alv->is_param && !alv->is_block_param)
        changed |= strbuf_demand_param_container_stores(c, avn, avs, depth + 1);
      else if (ty_is_array(alv->type) || ty_is_hash(alv->type) || alv->type == TY_UNKNOWN)
        changed |= strbuf_demand_container_stores_here(c, avn, avs);
    }
    else if (ak == NK_ArrayNode) {
      int en = 0; const int *el = nt_arr(nt, an, "elements", &en);
      for (int e = 0; e < en; e++) {
        int sn = el[e];
        if (sn < 0 || c->strbuf_box[sn]) continue;
        TyKind st = infer_type(c, sn);
        if (st != TY_STRING && st != TY_STRBUF) continue;
        c->strbuf_box[sn] = 1; changed = 1;
      }
    }
  }
  return changed;
}
static int strbuf_demand_container_stores(Compiler *c, const char *contn, Scope *conts) {
  int changed = strbuf_demand_container_stores_here(c, contn, conts);
  changed |= strbuf_demand_param_container_stores(c, contn, conts, 0);
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
  /* A local written from a HASH or ARRAY literal is not a string, whatever a
     mutator name suggests. The evidence that promotes a slot is a name-keyed
     mutator table, and `[]=`, `insert`, `slice!` and `setbyte` are Hash's and
     Array's as much as String's -- so `data = parse_object(s); data[k] = v`
     demanded the callee's returned `out = {}` into the shared set, and the
     Hash came back through a `const char *` return (#4177). The write is what
     the local IS; a shared name is only what was done to it. Checked
     syntactically because it has to hold in the fixpoint's first round, before
     any type has settled -- the promotion is sticky once made. */
  /* both walks below ran over the WHOLE node table once per candidate local
     (and this shape check runs per fixpoint round): the (scope, name) write
     chain and the per-scope call chain make each walk the handful of nodes
     the scope actually holds. The chains carry hash collisions and every
     filter stays. */
  int vsi = (int)(vs - c->scopes);
  for (int w = comp_lvw_first_sc(c, vsi, vn); w >= 0; w = comp_lvw_next_sc(c, w)) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    if (comp_scope_of(c, w) != vs) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0) continue;
    NodeKind vk = nt_kind(nt, wv);
    if (vk == NK_HashNode || vk == NK_KeywordHashNode || vk == NK_ArrayNode) return 0;
  }
  for (int u = comp_scall_first(c, vsi); u >= 0; u = comp_scall_next(c, u)) {
    const char *uty = nt_type(nt, u);
    if (!uty || !sp_streq(uty, "CallNode")) continue;
    if (comp_scope_of(c, u) != vs) continue;
    /* the group's answer, not the unique one: see an_any_scope_by_name */
    int mi = an_any_scope_by_name(c, nt_str(nt, u, "name"));
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

/* Append accumulators (`out = +""` ... `out << piece` in a loop): give the
   local the growable sp_String handle so each append is amortized O(1). A
   plain-string local reallocates and copies the whole accumulation on every
   `<<`, which is quadratic in the number of appends -- bm_huffman spends 80%
   of its time there and is the one benchmark Spinel loses to CRuby on.

   The handle is not free: reading one demotes through a full copy, so this
   only fires when the local is appended INSIDE a loop and never otherwise
   read inside one. That is exactly the shape where the quadratic lives; an
   accumulator read once after the loop pays a single copy for it.

   Unlike the shared-mutable promotion below, this asks nothing about
   aliasing, so it leaves str_shared alone: the local wants the storage, not
   the identity semantics. */
static void an_append_scan(Compiler *c, int node, int *app, int *rd, int cap, int *n_app, int *n_rd) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  /* a nested definition is a different scope: its locals are not these */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, node, "name");
    int recv = nt_ref(nt, node, "receiver");
    int a = nt_ref(nt, node, "arguments");
    int ac = 0; if (a >= 0) nt_arr(nt, a, "arguments", &ac);
    if (nm && recv >= 0 && ac == 1 && (sp_streq(nm, "<<") || sp_streq(nm, "concat")) &&
        (nt_kind(nt, recv) == NK_LocalVariableReadNode ||
         nt_kind(nt, recv) == NK_InstanceVariableReadNode)) {
      if (*n_app < cap) app[(*n_app)++] = recv;
      /* the receiver read is the append itself, not a use: skip it below */
      int nr0 = nt_num_refs(nt, node);
      for (int i = 0; i < nr0; i++) {
        int ch = nt_ref_at(nt, node, i);
        if (ch != recv) an_append_scan(c, ch, app, rd, cap, n_app, n_rd);
      }
      int na0 = nt_num_arrs(nt, node);
      for (int i = 0; i < na0; i++) {
        int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
        for (int k = 0; k < cnt; k++) an_append_scan(c, ids[k], app, rd, cap, n_app, n_rd);
      }
      return;
    }
  }
  if (nt_kind(nt, node) == NK_LocalVariableReadNode ||
      nt_kind(nt, node) == NK_InstanceVariableReadNode) {
    if (*n_rd < cap) rd[(*n_rd)++] = node;
    return;
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) an_append_scan(c, nt_ref_at(nt, node, i), app, rd, cap, n_app, n_rd);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, node, i, &cnt);
    for (int k = 0; k < cnt; k++) an_append_scan(c, ids[k], app, rd, cap, n_app, n_rd);
  }
}

/* The same accumulator, written out flat: `io << chunk` repeated as N straight
   statements rather than N times round a loop. A template compiler emits
   exactly that -- one append per literal chunk, no loop -- and without the
   handle each append reallocates and copies the whole page, so building it is
   quadratic in bytes even though nothing repeats. A 15 KB page from 178
   appends allocated 1.6 MB, and the collection pressure that follows dominated
   the request (#3480).

   The loop pass cannot see this: it scans loop bodies, and there is no loop.
   What it can share is the safety argument. Reading the handle demotes through
   a copy, so a read is only free once the appending is finished -- which for a
   straight run means every read has to come after the last append. Statement
   order is the list order here, not node ids, so "after" is exact. */
static int promote_straight_line_appends(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->body < 0) continue;
    int sn = 0; const int *stmts = nt_arr(nt, sc->body, "body", &sn);
    if (!stmts || sn < 2) continue;
    enum { CAP = 256, NAMES = 32 };
    const char *nm[NAMES]; int last_app[NAMES], n_app_of[NAMES], first_rd[NAMES], nn = 0;
    for (int k = 0; k < sn; k++) {
      int app[CAP], rd[CAP], n_app = 0, n_rd = 0;
      an_append_scan(c, stmts[k], app, rd, CAP, &n_app, &n_rd);
      for (int i = 0; i < n_app; i++) {
        const char *vn = nt_str(nt, app[i], "name");
        if (!vn || comp_scope_of(c, app[i]) != sc) continue;
        int x = -1;
        for (int j = 0; j < nn; j++) if (sp_streq(nm[j], vn)) { x = j; break; }
        if (x < 0) {
          if (nn >= NAMES) continue;
          x = nn++; nm[x] = vn; n_app_of[x] = 0; first_rd[x] = -1;
        }
        n_app_of[x]++; last_app[x] = k;
      }
      for (int i = 0; i < n_rd; i++) {
        const char *vn = nt_str(nt, rd[i], "name");
        if (!vn || comp_scope_of(c, rd[i]) != sc) continue;
        for (int j = 0; j < nn; j++)
          if (sp_streq(nm[j], vn) && first_rd[j] < 0) first_rd[j] = k;
      }
    }
    for (int j = 0; j < nn; j++) {
      /* one append is not a quadratic, and promoting it only buys a demote */
      if (n_app_of[j] < 2) continue;
      /* a read before the appending is finished pays the demoting copy and
         then re-grows: the very trade this promotion exists to avoid */
      if (first_rd[j] >= 0 && first_rd[j] <= last_app[j]) continue;
      LocalVar *lv = scope_local(sc, nm[j]);
      if (!lv || lv->type != TY_STRING) continue;
      if (lv->is_cell || lv->byref_out || lv->is_param || lv->is_block_param) continue;
      if (!strbuf_slot_eligible_shape(c, nm[j], sc, lv)) continue;
      if (strbuf_mut_kind(c, nm[j], sc) != 1) continue;
      lv->type = TY_STRBUF;
      lv->str_append = 1;
      changed = 1;
    }
  }
  return changed;
}

static int promote_append_accumulators(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int L = 0; L < nt->count; L++) {
    const char *lty = nt_type(nt, L);
    if (!lty) continue;
    int body = -1;
    if (sp_streq(lty, "WhileNode") || sp_streq(lty, "UntilNode") || sp_streq(lty, "ForNode"))
      body = nt_ref(nt, L, "statements");
    else if (sp_streq(lty, "CallNode") && nt_ref(nt, L, "block") >= 0) {
      /* Only a block that actually ITERATES: `proc {}` / `lambda {}` run once
         (and their body's appends may be the caller's byref writes), so they
         are not the repeated-append shape this is for. */
      static const char *const ITER[] = {
        "each", "each_with_index", "each_with_object", "each_entry", "each_pair",
        "each_key", "each_value", "each_char", "each_line", "each_byte",
        "each_slice", "each_cons", "reverse_each", "times", "upto", "downto",
        "step", "loop", "map", "collect", "flat_map", "collect_concat",
        "select", "filter", "reject", "sort_by", "min_by", "max_by",
        "group_by", "partition", "filter_map", "inject", "reduce", "cycle",
        "sum", "count", "tally_by", NULL };
      const char *lnm = nt_str(nt, L, "name");
      int iter = 0;
      for (int k = 0; lnm && ITER[k] && !iter; k++) if (sp_streq(lnm, ITER[k])) iter = 1;
      if (iter) body = nt_ref(nt, nt_ref(nt, L, "block"), "body");
    }
    if (body < 0) continue;
    enum { CAP = 128 };
    int app[CAP], rd[CAP], n_app = 0, n_rd = 0;
    an_append_scan(c, body, app, rd, CAP, &n_app, &n_rd);
    for (int i = 0; i < n_app; i++) {
      const char *vn = nt_str(nt, app[i], "name");
      Scope *vs = comp_scope_of(c, app[i]);
      if (!vn || !vs) continue;
      /* the same accumulator held in an ivar: `@out << chunk` round a loop
         rebuilt the whole string per append (the slot stores a value, so the
         append is a concat), which is the same quadratic (#3781) */
      if (nt_kind(nt, app[i]) == NK_InstanceVariableReadNode) {
        int icid = an_ivar_owner(c, app[i]);
        if (icid < 0) continue;
        int ivx = comp_ivar_index(&c->classes[icid], vn);
        if (ivx < 0 || c->classes[icid].ivar_types[ivx] != TY_STRING) continue;
        int iv_read = 0;
        for (int j = 0; j < n_rd && !iv_read; j++)
          if (nt_kind(nt, rd[j]) == NK_InstanceVariableReadNode &&
              nt_str(nt, rd[j], "name") && sp_streq(nt_str(nt, rd[j], "name"), vn) &&
              an_ivar_owner(c, rd[j]) == icid) iv_read = 1;
        if (iv_read) continue;
        if (strbuf_ivar_mut_kind(c, icid, vn) != 1) continue;
        if (strbuf_promote_ivar(c, icid, vn)) changed = 1;
        continue;
      }
      /* read elsewhere in this loop: the demoting copy would be the new
         quadratic, so leave the local alone */
      int read_in_loop = 0;
      for (int j = 0; j < n_rd && !read_in_loop; j++)
        if (comp_scope_of(c, rd[j]) == vs && nt_str(nt, rd[j], "name") &&
            sp_streq(nt_str(nt, rd[j], "name"), vn)) read_in_loop = 1;
      if (read_in_loop) continue;
      LocalVar *lv = scope_local(vs, vn);
      if (!lv || lv->type != TY_STRING) continue;
      /* a celled or byref slot is shared storage whose writes must stay
         visible through the other holder: leave the representation alone */
      if (lv->is_cell || lv->byref_out || lv->is_param || lv->is_block_param) continue;
      if (!strbuf_slot_eligible_shape(c, vn, vs, lv)) continue;
      if (strbuf_mut_kind(c, vn, vs) != 1) continue;
      lv->type = TY_STRBUF;
      lv->str_append = 1;
      changed = 1;
    }
  }
  changed |= promote_straight_line_appends(c);
  return changed;
}

static int promote_shared_stored_strings(Compiler *c) {
  int changed = 0;
  sb_store_valid = 0;   /* this run's store index is built on first use */
  const NodeTable *nt = c->nt;
  /* mutated string locals: receivers of an in-place mutator */
  /* per-kind chains: these walks run every fixpoint round, and the full-table
     form spent a quarter of a large machine-generated program's analyze just
     skipping unrelated nodes */
  /* the four chains are merged in node-id order (each chain is ascending),
     preserving the full walk's exact processing order -- grouping by kind
     deferred some promotions to a later round and the extra rounds cost more
     than the walk saved */
  int pss_cur[4] = { comp_kind_first(c, NK_ArrayNode), comp_kind_first(c, NK_HashNode),
                     comp_kind_first(c, NK_KeywordHashNode), comp_kind_first(c, NK_CallNode) };
  for (;;) {
    int pk = -1;
    for (int q = 0; q < 4; q++)
      if (pss_cur[q] >= 0 && (pk < 0 || pss_cur[q] < pss_cur[pk])) pk = q;
    if (pk < 0) break;
    int w = pss_cur[pk];
    pss_cur[pk] = comp_kind_next(c, w);
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
  for (int mu = comp_kind_first(c, NK_CallNode); mu >= 0; mu = comp_kind_next(c, mu)) {
    if (nt_kind(nt, mu) != NK_CallNode) continue;
    const char *mun = nt_str(nt, mu, "name");
    if (!mun) continue;
    {
      /* mutator name check (same handle-capable set) */
      if (!sp_str_mutator(mun, SP_MUT_CONTAINER)) continue;
    }
    int mrecv = nt_ref(nt, mu, "receiver");
    if (mrecv < 0 || nt_kind(nt, mrecv) != NK_CallNode) continue;
    /* every element read, not just `[]`: a mutation through `b.first` has to
       reach the container the same way (#4013) */
    if (!container_elem_read_p(nt, mrecv)) continue;
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
  for (int mu = comp_kind_first(c, NK_CallNode); mu >= 0; mu = comp_kind_next(c, mu)) {
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
  for (int w = comp_kind_first(c, NK_LocalVariableWriteNode); w >= 0; w = comp_kind_next(c, w)) {
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
    /* A slot that has widened past String cannot carry the shared-mutable
       REPRESENTATION: strbuf is an sp_String handle, and a poly slot may hold
       anything. Boxing keeps the handle, so identity and in-place mutation
       still travel; forcing strbuf back on it only fought whatever widened it,
       every round, to the fixpoint's cap (#4116). */
    if (srcv->type != TY_POLY && (srcv->type != TY_STRBUF || !srcv->str_shared))
      {  srcv->type = TY_STRBUF; srcv->str_shared = 1; changed = 1;  }
    if (tgtv->type != TY_POLY && (tgtv->type != TY_STRBUF || !tgtv->str_shared))
      {  tgtv->type = TY_STRBUF; tgtv->str_shared = 1; changed = 1;  }
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
    if (llv->type != TY_POLY && (llv->type != TY_STRBUF || !llv->str_shared))
      {  llv->type = TY_STRBUF; llv->str_shared = 1; changed = 1;  }
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
    if (llv2->type != TY_POLY && (llv2->type != TY_STRBUF || !llv2->str_shared))
      {  llv2->type = TY_STRBUF; llv2->str_shared = 1; changed = 1;  }
  }
  /* iteration-variable mutation: `arr.each { |x| x << "!" }` mutates the
     ELEMENT through the block binding, so the container's stored strings
     become handles and the block param binds the handle (#3227 P6). Every
     iterator whose block's FIRST parameter is the element counts, not just
     the two the C emitter binds directly: `arr.filter_map { |x| x << "?"; x }`
     reaches the element through the Ruby-defined builtin's yield, and the
     mutation was lost the same way (inject/each_slice/each_cons/zip bind
     something else first and stay out). */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_CallNode) continue;
    const char *itn = nt_str(nt, w, "name");
    if (!itn) continue;
    int blk4 = nt_ref(nt, w, "block");
    if (blk4 < 0) continue;
    int recv4 = nt_ref(nt, w, "receiver");
    /* the builtin's own copy, once the call has been rewritten onto it:
       `__enum_filter_map__N(arr) { |x| }` carries the container as its
       first argument (desugar_builtin_enum_calls) */
    if (recv4 < 0 && strncmp(itn, "__enum_", 7) == 0) {
      const char *bn = itn + 7;
      const char *sep = strstr(bn, "__");
      char nb[128];
      if (!sep || (size_t)(sep - bn) >= sizeof nb) continue;
      memcpy(nb, bn, (size_t)(sep - bn)); nb[sep - bn] = 0;
      if (!strbuf_elem_first_iterator(nb)) continue;
      int a4 = nt_ref(nt, w, "arguments"); int an4 = 0;
      const int *av4 = a4 >= 0 ? nt_arr(nt, a4, "arguments", &an4) : NULL;
      if (an4 < 1) continue;
      recv4 = av4[0];
    }
    else if (!strbuf_elem_first_iterator(itn)) continue;
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
      /* A poly array may still narrow to a nested numeric table. Binding the
         element param poly here is permanent -- a block parameter only widens
         -- so a row that each_with_index then yields arrives boxed. Wait for
         the pessimistic stage: a container that is still a poly array there
         really is one, and the demand below still runs. */
      if (g_infer_optimistic) continue;
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
    if (bpv4->type != TY_POLY && (bpv4->type != TY_STRBUF || !bpv4->str_shared))
      {  bpv4->type = TY_STRBUF; bpv4->str_shared = 1; changed = 1;  }
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
    /* strbuf_mut_kind is keyed on name + scope only, so `rr << x` reports a
       string mutation whatever rr holds -- `<<` is Array#push and Integer's
       shift as well. Gate the demand on the CALLER's slot being able to hold a
       String, exactly as the uniform branch below gates its own promotion: a
       slot already known to be an array (`rr = mk; rr << 1.5`) is pushing, and
       demanding the callee's tail into the shared set typed the callee's empty
       `[]` a strbuf -- which then flowed back and retyped rr itself, so the
       array literal was emitted as sp_String_new_shared(sp_IntArray_new()). */
    LocalVar *clv3 = (lname3 && ls3) ? scope_local(ls3, lname3) : NULL;
    int caller_may_be_str = clv3 && (clv3->type == TY_UNKNOWN ||
                                     clv3->type == TY_STRING ||
                                     clv3->type == TY_STRBUF ||
                                     clv3->type == TY_POLY);
    if (saw_tail && !shared_ok && lname3 && ls3 && caller_may_be_str &&
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
        if (tlv->type != TY_POLY && (tlv->type != TY_STRBUF || !tlv->str_shared))
          {  tlv->type = TY_STRBUF; tlv->str_shared = 1; changed = 1;  }
      }
      continue;
    }
    if (!shared_ok || !saw_tail) continue;
    if (!clv3 || !strbuf_slot_eligible_shape(c, lname3, ls3, clv3)) continue;
    if (!caller_may_be_str) continue;
    c->strbuf_box[wv] = 1; changed = 1;
    if (clv3->type != TY_POLY && (clv3->type != TY_STRBUF || !clv3->str_shared))
      {  clv3->type = TY_STRBUF; clv3->str_shared = 1; changed = 1;  }
  }
  /* Container-read alias (`r = rows[0]; r.upcase!`): the local is another name
     for the element, so an in-place mutation through it has to land on the
     container's own string. Demand that container's stores into handles,
     promote the local, and mark the read so the emitter hands out the handle
     instead of the safe copy -- the local held a COPY before, and the
     container never saw the mutation (#3941). */
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || nt_kind(nt, wv) != NK_CallNode) continue;
    /* `[]` only: the ALIAS binding needs the emitter to hand the local the
       element's handle, and only the index read does that. `first`/`last`
       reach it anyway -- they are rewritten to `[]` before this runs -- while
       a local bound from `fetch`/`dig` keeps its copy, as it did before. */
    { const char *idxn5 = nt_str(nt, wv, "name");
      if (!idxn5 || !sp_streq(idxn5, "[]")) continue; }
    if (nt_ref(nt, wv, "block") >= 0) continue;
    int cont5 = nt_ref(nt, wv, "receiver");
    if (cont5 < 0 || nt_kind(nt, cont5) != NK_LocalVariableReadNode) continue;
    const char *contn5 = nt_str(nt, cont5, "name");
    Scope *conts5 = contn5 ? comp_scope_of(c, cont5) : NULL;
    LocalVar *contv5 = (contn5 && conts5) ? scope_local(conts5, contn5) : NULL;
    if (!contv5 || (!ty_is_array(contv5->type) && !ty_is_hash(contv5->type) &&
                    contv5->type != TY_UNKNOWN)) continue;
    const char *lname5 = nt_str(nt, w, "name");
    Scope *ls5 = comp_scope_of(c, w);
    LocalVar *llv5 = (lname5 && ls5) ? scope_local(ls5, lname5) : NULL;
    if (!llv5 || !strbuf_slot_eligible_shape(c, lname5, ls5, llv5)) continue;
    if (strbuf_mut_kind(c, lname5, ls5) != 1) continue;
    if (llv5->type != TY_UNKNOWN && llv5->type != TY_STRING &&
        llv5->type != TY_STRBUF && llv5->type != TY_POLY) continue;
    /* The mutator table is name-keyed, so `b0 << 4` on an Integer element
       reads as a string append. Require the container to actually hold a
       string somewhere before treating the binding as a string alias. */
    if (!strbuf_container_stores_string(c, contn5, conts5)) continue;
    /* The stores that ARE strings become handles either way: the container's
       own String element is mutated through it (`s = box[1]; s.replace(x)`),
       and refusing that before the gate below left it a plain box, so the
       mutation landed in a copy and the array never saw it. */
    changed |= strbuf_demand_container_stores(c, contn5, conts5);
    /* The BINDING, though, only means what it says when the container holds
       nothing but strings. A heterogeneous one passes the test above on its
       string element while the local is bound from another: `box =
       [Frag.new(h), "s"]; f = box[0]; f.replace(sel)` bound f to a string
       handle and ran String#replace on a Frag, answering the argument
       (#4240). Leave such a local poly -- the call site then dispatches on
       the element's own class. */
    if (strbuf_container_stores_nonstring(c, contn5, conts5)) continue;
    if (!c->strbuf_box[wv]) { c->strbuf_box[wv] = 1; changed = 1; }
    if (llv5->type != TY_POLY && (llv5->type != TY_STRBUF || !llv5->str_shared))
      {  llv5->type = TY_STRBUF; llv5->str_shared = 1; changed = 1;  }
  }
  return changed;
}

/* A parameter RETAINED in a shared-handle ivar (`def initialize(b) @bt = b end`,
   then `@bt << "x"` somewhere) has to arrive as the handle. The byref machinery
   above only covers a callee that mutates the parameter ITSELF; a callee that
   merely stores it and mutates it later, through the ivar, kept the caller's
   string as a value and the two holders drifted apart -- which
   docs/limitations.md promises they do not (#4363). Marking the parameter is
   all this rule does; convert_byref_handle_params then pulls every call site's
   argument into the shared set, exactly as it does for a byref parameter that
   has just become a handle. */
/* Is this argument already a shared handle?

   Either by name -- an_arg_is_shared_handle reads the slot -- or through a
   reader over a shared slot (`K.new(obj.reader)`), where the handle reaches
   the argument by a call rather than a name and the slot-reading form cannot
   see it. */
static int an_arg_hands_handle(Compiler *c, int node) {
  if (node < 0) return 0;
  if (an_arg_is_shared_handle(c, node)) return 1;
  { char ivb[300]; int defc = -1;
    const char *ivn = an_reader_ivar_of(c, node, &defc, ivb, sizeof ivb);
    if (ivn && defc >= 0) {
      int ivx = comp_ivar_index(&c->classes[defc], ivn);
      if (ivx >= 0 && c->classes[defc].ivar_str_shared[ivx]) return 1;
    } }
  return 0;
}

/* Which scope does CallNode `u` statically call? The forward form of
   an_call_targets_scope, so one walk can fill a table for every scope at once
   instead of re-deciding per candidate. The two must agree; the order of the
   two arms is the same (a unique user method named `new` wins over the
   constructor reading). */
static void an_call_targets_of(Compiler *c, int u, int new_ok,
                               int out[2], int *nout) {
  const NodeTable *nt = c->nt;
  *nout = 0;
  const char *un = nt_str(nt, u, "name");
  if (!un) return;
  /* BOTH arms, because an_call_targets_scope answers for both and a call can
     satisfy each against a different scope: with a method named `new` in the
     program, `K.new(s)` matches that scope by name AND resolves to
     K#initialize as a constructor. Resolving to a single scope dropped one of
     them -- whichever arm ran second -- and the call site vanished from that
     scope's chain, which is how `Holder.new(s)` stopped being evidence and the
     copy came back. */
  int byname = an_unique_scope_by_name(c, un);
  if (byname >= 0 && c->scopes[byname].name &&
      sp_streq(c->scopes[byname].name, un)) out[(*nout)++] = byname;
  if (!sp_streq(un, "new")) return;
  /* `new_ok` is an_new_recv_all_constant, hoisted by the caller: it walks the
     whole node table, so asking it per call node made the build quadratic. */
  if (!new_ok) return;
  int rc = nt_ref(nt, u, "receiver");
  if (rc < 0 || nt_kind(nt, rc) != NK_ConstantReadNode) return;
  const char *cn = nt_str(nt, rc, "name");
  int cid = cn ? comp_class_index(c, cn) : -1;
  if (cid < 0) return;
  int mi = comp_method_in_class(c, cid, "initialize");
  if (mi < 0 || mi >= c->nscopes) return;
  Scope *m2 = &c->scopes[mi];
  if (!m2->name || m2->class_id < 0 || m2->is_cmethod) return;
  if (*nout == 0 || out[0] != mi) out[(*nout)++] = mi;
}

/* (scope, parameter) -> "some call site hands that parameter a shared handle".

   Evidence from the CALLER, which the rules around it do not have. The
   propagation runs param -> argument: a parameter that is byref, or already a
   handle, pulls its call sites' locals into the shared set. Nothing ran the
   other way, so a parameter that is only RETAINED -- never mutated, in a class
   that never mutates the slot -- stayed a value while the argument was a
   handle, and the call site handed it sp_str_concat(cstr, "") instead. The
   holder walked away with a snapshot and never saw a mutation made through any
   other alias:

       class Holder; def initialize(b) @b = b end; def at(i) @b.getbyte(i) end; end
       class Poker;  def initialize(b) @b = b end; def poke(i,v) @b.setbyte(i,v) end; end
       s = +"abcd"; h = Holder.new(s); pk = Poker.new(s)
       pk.poke(2, 7); h.at(2)   # 7 in Ruby, the old byte here

   Poker's parameter is a handle (its slot is mutated) and that made `s` one;
   Holder's had no evidence of its own. The argument being a handle IS the
   evidence -- what a handle is passed to is a handle, the call-site twin of
   the rule below.

   Built ONCE per pass, not per candidate. The question is asked for every ivar
   write that stores a parameter, and answering each by walking the call nodes
   made the pass O(writes x nodes): a 15k-line program whose classes mostly
   retain a string parameter took 3.1x as long to compile. One walk fills every
   slot, and the answers move only between passes anyway, as slots promote. */
typedef struct {
  int *off;            /* scope -> base into `bit`, or -1 when it has no params */
  unsigned char *bit;
  int ok;              /* 0 when allocation failed: fall back to no evidence */
  /* scope -> its call sites, as a head/next chain over call node ids. The
     byref pass below asked `does call u target scope mi2?` for every node and
     every handle parameter, which is O(handle params x nodes) -- fine while
     handle parameters were rare, and the dominant cost once a whole program's
     retaining classes became handles. The same walk that fills `bit` records
     the chain, so both passes read it instead. */
  int *head;           /* scope -> first edge, or -1 */
  int *enext;          /* edge -> next edge for the same scope */
  int *enode;          /* edge -> the call node */
} HandleArgTab;

static void handle_arg_tab_init(Compiler *c, HandleArgTab *t) {
  const NodeTable *nt = c->nt;
  t->off = NULL; t->bit = NULL; t->head = NULL; t->enext = NULL; t->enode = NULL;
  t->ok = 0;
  if (c->nscopes <= 0) return;
  size_t maxe = (size_t)(nt->count > 0 ? nt->count : 1) * 2;
  t->off = (int *)malloc(sizeof(int) * (size_t)c->nscopes);
  t->head = (int *)malloc(sizeof(int) * (size_t)c->nscopes);
  t->enext = (int *)malloc(sizeof(int) * maxe);
  t->enode = (int *)malloc(sizeof(int) * maxe);
  if (!t->off || !t->head || !t->enext || !t->enode) {
    free(t->off); free(t->head); free(t->enext); free(t->enode);
    t->off = NULL; t->head = NULL; t->enext = NULL; t->enode = NULL; return;
  }
  for (int i = 0; i < c->nscopes; i++) t->head[i] = -1;
  int total = 0;
  for (int i = 0; i < c->nscopes; i++) {
    int np = c->scopes[i].nparams;
    if (np <= 0) { t->off[i] = -1; continue; }
    t->off[i] = total; total += np;
  }
  t->bit = total > 0 ? (unsigned char *)calloc((size_t)total, 1) : NULL;
  if (total > 0 && !t->bit) {
    free(t->off); free(t->head); free(t->enext); free(t->enode);
    t->off = NULL; t->head = NULL; t->enext = NULL; t->enode = NULL; return;
  }
  t->ok = 1;
  int ne = 0;
  int new_ok = an_new_recv_all_constant(c);
  for (int u = 0; u < nt->count; u++) {
    if (nt_kind(nt, u) != NK_CallNode) continue;
    int tgt[2], ntg = 0;
    an_call_targets_of(c, u, new_ok, tgt, &ntg);
    for (int k = 0; k < ntg; k++) {
      int mi = tgt[k];
      if (mi < 0 || mi >= c->nscopes) continue;
      t->enode[ne] = u; t->enext[ne] = t->head[mi]; t->head[mi] = ne; ne++;
      if (t->off[mi] < 0 || !t->bit) continue;
      int argsN = nt_ref(nt, u, "arguments");
      int argc2 = 0;
      const int *argv2 = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &argc2) : NULL;
      if (!argv2) continue;
      int np = c->scopes[mi].nparams;
      for (int pj = 0; pj < argc2 && pj < np; pj++)
        if (an_arg_hands_handle(c, argv2[pj])) t->bit[t->off[mi] + pj] = 1;
    }
  }
}

static void handle_arg_tab_free(HandleArgTab *t) {
  free(t->off); free(t->bit); free(t->head); free(t->enext); free(t->enode);
  t->off = NULL; t->bit = NULL; t->head = NULL;
  t->enext = NULL; t->enode = NULL; t->ok = 0;
}

static int handle_arg_tab_get(const HandleArgTab *t, int mi, int pj) {
  if (!t->ok || !t->bit || mi < 0 || pj < 0 || t->off[mi] < 0) return 0;
  return t->bit[t->off[mi] + pj] != 0;
}

static int promote_params_stored_in_shared_ivars(Compiler *c,
                                                 const HandleArgTab *hat) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_InstanceVariableWriteNode) continue;
    int wv = nt_ref(nt, w, "value");
    if (wv < 0 || nt_kind(nt, wv) != NK_LocalVariableReadNode) continue;
    const char *ivname = nt_str(nt, w, "name");
    const char *lname = nt_str(nt, wv, "name");
    if (!ivname || !lname) continue;
    Scope *ms = comp_scope_of(c, wv);
    if (!ms) continue;
    LocalVar *pp = scope_local(ms, lname);
    /* a parameter only: a plain local written from an ivar is the alias rule
       above, and a block parameter binds per iteration rather than per call */
    if (!pp || !pp->is_param || pp->is_block_param) continue;
    if (an_param_idx(ms, lname) < 0) continue;
    int icid = an_ivar_owner(c, w);
    if (icid < 0) continue;
    /* The slot has to be one that is mutated in place; without that there is
       no aliasing to preserve and the value representation stays cheaper.
       Kind -1 is the AMBIGUOUS evidence -- `[]=`, `insert`, `slice!`,
       `setbyte` and the bang forms -- which names Array's and Hash's methods
       as much as String's, so on its own it proves nothing. Once the slot's
       own type says String the ambiguity is settled, and a splice is an
       in-place mutation like any other. */
    { int mk = strbuf_ivar_mut_kind(c, icid, ivname);
      int ivx0 = comp_ivar_index(&c->classes[icid], ivname);
      TyKind ivt0 = ivx0 >= 0 ? c->classes[icid].ivar_types[ivx0] : TY_UNKNOWN;
      /* A slot ANOTHER rule already promoted needs no evidence of its own:
         being a shared handle is the conclusion the evidence was for. The
         mutation census keys on calls whose receiver is an ivar read, so a
         slot mutated only from OUTSIDE its class -- `d.s << "x"` through the
         reader -- records nothing there, and the parameter written into it
         was left a value while the ivar itself was a handle (#4363). */
      int already = ivx0 >= 0 && c->classes[icid].ivar_str_shared[ivx0];
      /* And the converse: the SLOT BEING WRITTEN is evidence too, once what is
         written into it is already a handle. `def initialize(s) @one = s;
         @two = s end` with only @one mutated made the parameter a handle
         through @one, and @two -- having no mutator of its own -- kept a value
         copy, so the second name did not see the mutation and was not the same
         object. What a handle is assigned to is a handle (#4363). */
      int src_is_handle = pp->type == TY_STRBUF && pp->str_shared;
      /* And from the other side of the call: the ARGUMENT is already a handle.
         Without this the chain only ever ran outwards from a slot that could
         show its own mutation, so a retaining class that mutates nothing --
         the reader half of an aliased pair -- was handed a copy. */
      int arg_is_handle =
          handle_arg_tab_get(hat, (int)(ms - c->scopes),
                             an_param_idx(ms, lname));
      if (!already && !src_is_handle && !arg_is_handle &&
          mk != 1 && !(mk == -1 && (ivt0 == TY_STRING || ivt0 == TY_STRBUF)))
        continue; }
    if (pp->type != TY_UNKNOWN && pp->type != TY_STRING &&
        pp->type != TY_STRBUF && pp->type != TY_POLY) continue;
    if (strbuf_promote_ivar(c, icid, ivname)) changed = 1;
    { ClassInfo *ci2 = &c->classes[icid];
      int iv2 = comp_ivar_index(ci2, ivname);
      if (iv2 < 0 || !ci2->ivar_str_shared[iv2]) continue; }
    if (pp->type != TY_POLY && (pp->type != TY_STRBUF || !pp->str_shared))
      {  pp->type = TY_STRBUF; pp->str_shared = 1; pp->byref_out = 0; changed = 1;  }
  }
  return changed;
}

/* Does CallNode `u` statically call scope `mi2`? The byref machinery resolves
   a callee by program-unique name, which is exactly what a constructor is not:
   every class has an `initialize`, so the reported shape -- a string handed to
   `K.new` and retained in an ivar -- had no call site the propagation could
   find (#4363). `K.new(...)` resolves through the constant receiver instead.
   Guarded on the whole program rather than per class: if any `new` in the
   program goes through a receiver that is not a constant, some class is
   instantiated by a class object we cannot pin, and a parameter whose C type
   we are about to change could be reached through it. */
static int an_new_recv_all_constant(Compiler *c) {
  const NodeTable *nt = c->nt;
  static int cached = -1, cached_count = -1;
  if (cached >= 0 && cached_count == nt->count) return cached;
  int ok = 1;
  for (int u = 0; u < nt->count && ok; u++) {
    if (nt_kind(nt, u) != NK_CallNode) continue;
    const char *un = nt_str(nt, u, "name");
    if (!un || !sp_streq(un, "new")) continue;
    int rc = nt_ref(nt, u, "receiver");
    if (rc < 0 || nt_kind(nt, rc) != NK_ConstantReadNode) ok = 0;
  }
  cached = ok; cached_count = nt->count;
  return ok;
}
static int an_call_targets_scope(Compiler *c, int u, int mi2, Scope *m2) {
  const NodeTable *nt = c->nt;
  const char *un = nt_str(nt, u, "name");
  if (!un) return 0;
  if (sp_streq(un, m2->name) && an_unique_scope_by_name(c, un) == mi2) return 1;
  if (!sp_streq(un, "new") || !sp_streq(m2->name, "initialize")) return 0;
  if (m2->class_id < 0 || m2->is_cmethod) return 0;
  if (!an_new_recv_all_constant(c)) return 0;
  int rc = nt_ref(nt, u, "receiver");
  if (rc < 0 || nt_kind(nt, rc) != NK_ConstantReadNode) return 0;
  const char *cn = nt_str(nt, rc, "name");
  int cid = cn ? comp_class_index(c, cn) : -1;
  if (cid < 0) return 0;
  return comp_method_in_class(c, cid, "initialize") == mi2;
}

/* `obj.reader.equal?(x)` and `x.equal?(obj.reader)` ask whether two names are
   one object, and a reader that hands out a reading of the slot answers no for
   an object that IS shared. The in-fixpoint rule beside the container stores
   marks the argument side, but it cannot see a slot promoted after the
   fixpoint -- a parameter retained in an ivar becomes a handle below -- so the
   demand is made again once every promotion has settled.

   It goes in strbuf_handle_demand rather than strbuf_box because the two mean
   different things here: strbuf_box is read as the node's TYPE as well, and
   the emitters pick their arm from the receiver's type, so marking a receiver
   moves `equal?` off the String surface that answers it. This says only "hand
   out the handle" (#4363). */
static int mark_reader_identity_operands(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int w = 0; w < nt->count; w++) {
    if (nt_kind(nt, w) != NK_CallNode) continue;
    const char *nm = nt_str(nt, w, "name");
    if (!nm || (!sp_streq(nm, "equal?") && !sp_streq(nm, "eql?"))) continue;
    int recv = nt_ref(nt, w, "receiver");
    int a = nt_ref(nt, w, "arguments");
    int ac = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
    if (recv < 0 || ac != 1) continue;
    for (int side = 0; side < 2; side++) {
      int opnd = side == 0 ? av[0] : recv;
      if (opnd < 0 || c->strbuf_box[opnd] || c->strbuf_handle_demand[opnd]) continue;
      char ivb[300]; int defc = -1;
      const char *ivn = an_reader_ivar_of(c, opnd, &defc, ivb, sizeof ivb);
      if (!ivn || defc < 0) continue;
      int iv = comp_ivar_index(&c->classes[defc], ivn);
      if (iv < 0 || !c->classes[defc].ivar_str_shared[iv]) continue;
      c->strbuf_handle_demand[opnd] = 1;
      changed = 1;
    }
  }
  return changed;
}

static int convert_byref_handle_params(Compiler *c,
                                       const HandleArgTab *hat) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* the call-site chain is what makes this pass affordable; with no table
     there is nothing to walk, and promoting nothing is the safe answer */
  if (!hat->ok) return 0;
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
      for (int e = hat->head[mi2]; e >= 0; e = hat->enext[e]) {
        int u = hat->enode[e];
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
      for (int e = hat->head[mi2]; e >= 0; e = hat->enext[e]) {
        int u = hat->enode[e];
        int argsN = nt_ref(nt, u, "arguments");
        int uargc = 0;
        const int *uargv = argsN >= 0 ? nt_arr(nt, argsN, "arguments", &uargc) : NULL;
        if (pj >= uargc) continue;
        int an2 = uargv[pj];
        if (nt_kind(nt, an2) == NK_LocalVariableReadNode) {
          const char *vn2 = nt_str(nt, an2, "name");
          Scope *vs2 = vn2 ? comp_scope_of(c, an2) : NULL;
          LocalVar *alv = vs2 ? scope_local(vs2, vn2) : NULL;
          /* The argument is this method's OWN parameter, being passed on:
             `def via(k, y) k.take(y) end`. It has to become a handle for the
             same reason the callee's did, and then this loop's next round
             reaches via's own call sites, so the chain carries however deep it
             runs. strbuf_slot_eligible_shape turns parameters away -- that is
             what defers them to this pass -- so the shape check does not
             apply to them here (#4363). */
          if (alv && alv->is_param && !alv->is_block_param &&
              an_param_idx(vs2, vn2) >= 0 &&
              (alv->type == TY_UNKNOWN || alv->type == TY_STRING ||
               alv->type == TY_STRBUF)) {
            if (alv->type != TY_STRBUF || !alv->str_shared) {
              alv->type = TY_STRBUF; alv->str_shared = 1; alv->byref_out = 0;
              changed = 1;
            }
            continue;
          }
          if (!alv || !strbuf_slot_eligible_shape(c, vn2, vs2, alv)) continue;
          if (alv->type != TY_UNKNOWN && alv->type != TY_STRING &&
              alv->type != TY_STRBUF && alv->type != TY_POLY) continue;
          if (alv->type != TY_POLY && (alv->type != TY_STRBUF || !alv->str_shared))
            {  alv->type = TY_STRBUF; alv->str_shared = 1; changed = 1;  }
        }
        else if (nt_kind(nt, an2) == NK_InstanceVariableReadNode) {
          const char *vn2 = nt_str(nt, an2, "name");
          int cid2 = vn2 ? an_ivar_owner(c, an2) : -1;
          if (cid2 >= 0 && strbuf_ivar_mut_kind(c, cid2, vn2) >= 0)
            if (strbuf_promote_ivar(c, cid2, vn2)) changed = 1;
        }
        /* `K.new(obj.reader)`: the reader has to hand out the HANDLE, or the
           new holder and `obj` walk away with two strings. The P5 rule makes
           exactly this mark, but only ever looked at a local write. Marking
           alone is not enough here: this pass runs after the node-type cache
           is finalized, and comp_ntype answers from the cache, so the caller
           would build a `const char *` temp out of an `sp_String *`. The
           cached type is corrected with the mark, which is what makes the two
           agree (#4363). */
        else if (nt_kind(nt, an2) == NK_CallNode) {
          char ivbuf2[300]; int defc2 = -1;
          const char *ivn2 = an_reader_ivar_of(c, an2, &defc2, ivbuf2, sizeof ivbuf2);
          if (ivn2 && defc2 >= 0 && strbuf_ivar_mut_kind(c, defc2, ivn2) >= 0) {
            if (strbuf_promote_ivar(c, defc2, ivn2)) changed = 1;
            int iv3 = comp_ivar_index(&c->classes[defc2], ivn2);
            if (iv3 >= 0 && c->classes[defc2].ivar_str_shared[iv3] &&
                !c->strbuf_box[an2]) {
              c->strbuf_box[an2] = 1;
              comp_sn_retype(c, an2, TY_STRBUF);
              changed = 1;
            }
          }
        }
      }
    }
  }
  return changed;
}

/* The block-taking method named `fn` that KEEPS its block -- reads its &blk
   parameter as anything but the receiver of `.call` or a `&blk` forward
   (`blk_call_recv` / `blk_arg_expr` mark those reads) -- or -1. `*first` gets
   the first block-taking method of that name, keeper or not. For a callee
   whose receiver has no type yet: every same-named candidate is a possible
   target, and one that keeps the block decides. */
static int a_name_keeps_block(Compiler *c, const char *fn, const char *blk_call_recv,
                              const char *blk_arg_expr, int *first) {
  if (first) *first = -1;
  if (!fn) return -1;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *cs3 = &c->scopes[si];
    if (cs3->is_cmethod || !cs3->name || !sp_streq(cs3->name, fn)) continue;
    if (!cs3->blk_param || !cs3->blk_param[0]) continue;
    if (first && *first < 0) *first = si;
    for (int q = 0; q < c->nt->count; q++) {
      if (c->nscope[q] != si) continue;
      if (nt_kind(c->nt, q) != NK_LocalVariableReadNode) continue;
      const char *qn = nt_str(c->nt, q, "name");
      if (!qn || !sp_streq(qn, cs3->blk_param)) continue;
      if (!(blk_call_recv && blk_call_recv[q]) &&
          !(blk_arg_expr && blk_arg_expr[q])) return si;
    }
  }
  return -1;
}

/* A literal block on a call whose receiver has no type yet, to a method name
   some class defines with a block it keeps (a_name_keeps_block). Such a block
   becomes a real proc once the receiver settles, so a pass that runs before
   then has to count it lifted already. A constant receiver names a class and
   is left to a_block_is_lifted, which resolves it without a type. */
static int a_block_lifted_by_callee_name(Compiler *c, int id, const char *blk_call_recv,
                                         const char *blk_arg_expr) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return 0;
  if (infer_type(c, recv) != TY_UNKNOWN) return 0;
  return a_name_keeps_block(c, nt_str(nt, id, "name"), blk_call_recv, blk_arg_expr, NULL) >= 0;
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
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !subtree_has_yield_node(c, blk, 0)) continue;
    const char *nm = nt_str(nt, id, "name");
    int r = nt_ref(nt, id, "receiver");
    const char *rn = (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode) ? nt_str(nt, r, "name") : NULL;
    if (nm && sp_streq(nm, "new") && rn && (sp_streq(rn, "Thread") || sp_streq(rn, "Fiber"))) return 1;
    /* The same for a block that becomes a first-class proc because its
       callee keeps a real &blk: the proc's body is its own C function, where
       the enclosing method's block is as out of reach as it is from a
       Thread's. `Net::HTTP#request(req, &blk)` with `blk.call(res) unless
       blk.nil?` is that callee, and campfire's `yield response` inside the
       block it is given raised LocalJumpError (#4438). */
    if (a_block_is_lifted(c, id)) return 1;
  }
  return 0;
}

/* Does any call have a poly receiver and this method's name? With a block,
   or without one: a poly receiver dispatches by class to a callable symbol,
   and an inlined method has none, so a blockless `slots["h"].request(req)`
   to a `request(req, &blk)` that inlines at its (all blockless) call sites
   found no arm and raised NoMethodError (#4492, the edge the #4477 approval
   of `blk.nil?` walked over). The proc form is that arm: its block is the
   real proc, NULL here, which is what `blk.nil?` and a bare `yield` (a
   LocalJumpError) already answer for. */
/* Is there a `new` whose class is only known at run time (`k.new`, a Class
   read out of a container)? Its switch arm calls sp_X_new, which cannot splice
   a yielding initialize the way a constant `X.new` site does, so the class
   needs the clone for its constructor to run the body at all. */
static int pf_dynamic_new(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    int rk = nt_kind(nt, recv);
    if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) return 1;
  }
  return 0;
}

static int pf_wanted(Compiler *c, const char *name) {
  const NodeTable *nt = c->nt;
  if (sp_streq(name, "initialize")) return pf_dynamic_new(c);
  /* the calls of this name, not every node: asked per yielding method
     (rubys in #5035) */
  for (int id = an_calls_named_first(c, name); id >= 0; id = an_calls_named_next(id)) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv >= 0 && infer_type(c, recv) == TY_POLY) return 1;
  }
  return 0;
}

/* Does a class above or below `src`'s define the same name? Then a call on
   the ancestor's type dispatches on cls_id, and an inline-only `src` can only
   take its arm through the clone: with none, the arm was dropped and the
   ancestor's method ran in its place. */
static int pf_in_class_dispatch(Compiler *c, const Scope *src) {
  /* a constructor is not reached through that switch */
  if (src->is_cmethod || sp_streq(src->name, "initialize")) return 0;
  /* a Struct's generated each/each_pair is copied into every struct class, so
     a struct subclass always "overrides" it; those copies are one method, and
     their bodies read desugared accumulators a clone does not carry */
  if (c->classes[src->class_id].is_struct) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k == src->class_id) continue;
    if (!is_descendant(c, k, src->class_id) && !is_descendant(c, src->class_id, k)) continue;
    if (comp_method_in_class(c, k, src->name) >= 0) return 1;
  }
  return 0;
}

int make_yield_proc_forms(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = c->nscopes;
  int made = 0;
  for (int s = 1; s < n0; s++) {
    Scope *src = &c->scopes[s];
    /* reachability is decided after this pass, so do not consult it: an
       unused clone is a static function the C compiler drops. */
    if (!src->yields || src->class_id < 0) continue;
    /* a by-value or Struct constructor is built by its own emitter, which
       does not run the clone (ctor_init_proc_form) */
    if (sp_streq(src->name ? src->name : "", "initialize") &&
        (c->classes[src->class_id].is_struct || c->classes[src->class_id].is_value_type)) continue;
    if (src->is_transplanted_source || !src->name) continue;
    if (src->body < 0) continue;
    if (!pf_wanted(c, src->name) && !pf_in_class_dispatch(c, src)) continue;
    /* A method the program reopens has two definitions in the scope table
       and the last one wins (comp_method_in_class): only that one gets the
       clone. Cloning the first left the poly dispatch arm running the
       package's transport where every other call site ran the program's
       stub in front of it (#4502). */
    if (comp_method_in_class(c, src->class_id, src->name) != s) continue;
    /* `#` cannot appear in a Ruby method name, so the clone is invisible to
       the by-name lookups while still mangling to a valid C identifier. */
    char pfname[192];
    snprintf(pfname, sizeof pfname, "%s#pf", src->name);
    if (comp_method_in_class(c, src->class_id, pfname) >= 0) continue;
    int nb = nt_clone_subtree(nt, src->body);
    if (nb < 0) continue;
    comp_scope_new(c, pfname, src->def_node);
    int di = c->nscopes - 1;
    comp_grow_node_arrays(c);
    src = &c->scopes[s];
    Scope *dst = &c->scopes[di];
    dst->class_id = src->class_id;
    dst->is_cmethod = src->is_cmethod;
    dst->body = nb;
    dst->reachable = 1;
    dst->yields = 0;
    dst->nparams = src->nparams;
    dst->nrequired = src->nrequired;
    dst->rest_idx = src->rest_idx;
    dst->kwrest_idx = src->kwrest_idx;
    if (src->nparams > 0) {
      dst->pnames = (char **)calloc((size_t)src->nparams, sizeof(char *));
      dst->pdefault = (int *)malloc(sizeof(int) * (size_t)src->nparams);
      if (!dst->pnames || !dst->pdefault) { dst->nparams = 0; }
      else for (int p = 0; p < src->nparams; p++) {
        dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
        dst->pdefault[p] = src->pdefault[p];
      }
    }
    /* A method that already declares `&blk` binds its block under THAT name,
       and the cloned body reads it under that name. Renaming the parameter to
       the synthetic one left the body reading a `blk` that nothing assigns --
       a nil proc call, silently answering the slot default. Only a method with
       no declared block param needs a name invented for it. */
    dst->blk_param = (src->blk_param && src->blk_param[0]) ? strdup(src->blk_param)
                                                           : strdup("__pf_blk");
    dst->is_proc_form = 1;
    if (getenv("SP_DBG_PF")) fprintf(stderr, "[pf] made %s scope %d cls %d\n", pfname, di, dst->class_id);
    walk_scope(c, nb, di, dst->class_id);
    made = 1;
  }
  if (made) {
    register_locals(c);
    /* register_locals interns the body's names as plain locals; the clone's
       parameters must be marked as such or the prologue declares them a second
       time on top of the C parameters. The block parameter is the proc. */
    for (int s = 1; s < c->nscopes; s++) {
      Scope *d = &c->scopes[s];
      if (!d->is_proc_form) continue;
      for (int p = 0; p < d->nparams; p++) {
        if (!d->pnames[p]) continue;
        LocalVar *lv = scope_local_intern(d, d->pnames[p]);
        if (!lv) continue;
        lv->is_param = 1;
        /* No call site binds the clone's parameters (the dispatch arm is the
           only caller, and it boxes what it passes), so a parameter no
           inference touches stays unknown, and an unknown local in a value
           position is emitted as nil: `perform(req)` inside the clone ran
           `perform(nil)` (#4502). The signature spells it sp_RbVal; the
           local is that from the start. */
        if (lv->type == TY_UNKNOWN) lv->type = TY_POLY;
      }
      if (d->blk_param) {
        LocalVar *bl = scope_local_intern(d, d->blk_param);
        if (bl) { bl->is_param = 1; bl->type = TY_PROC; }
      }
    }
  }
  return made;
}

/* Representation family of a settled type: 1 for a flat scalar in a machine
   register, 2 for a heap pointer. 0 for anything the check must not judge --
   poly and unknown (no static claim), nil (a literal nil is legal in every
   slot), bigint (int converts at the boundary), a by-value object or a
   shared-string handle (neither is a plain pointer). */
static int seed_repr_family(Compiler *c, TyKind t) {
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_SYMBOL: return 1;
    case TY_STRING: case TY_PROC: return 2;
    default: break;
  }
  if (ty_is_array(t) || ty_is_hash(t)) return 2;
  if (ty_is_object(t) && !comp_ty_value_obj(c, t)) return 2;
  return 0;
}

/* Within the pointer family the representations still differ, with no
   conversion between them: an Array, a Hash and a String are three different
   layouts. Anything else that is a pointer (a Proc, an object -- which may
   carry to_s / to_ary conversions the emitter knows) is left unjudged. */
static int seed_ptr_kind(TyKind t) {
  if (ty_is_array(t)) return 1;
  if (ty_is_hash(t))  return 2;
  if (t == TY_STRING) return 3;
  return 0;
}

/* 1 iff a value of type `val` placed in a slot the seed pinned to `slot` would
   be REINTERPRETED rather than converted -- the whole point of the rule. `ret`
   selects the wider judgement a return slot allows (see seed_ret_family). */
static int seed_ret_family(Compiler *c, TyKind t);
static int seed_ret_kind(TyKind t);
static int seed_contradicts(Compiler *c, TyKind slot, TyKind val, int ret) {
  int fs = ret ? seed_ret_family(c, slot) : seed_repr_family(c, slot);
  int fv = ret ? seed_ret_family(c, val)  : seed_repr_family(c, val);
  if (!fs || !fv) return 0;
  if (fs != fv) return 1;
  int ks = ret ? seed_ret_kind(slot) : seed_ptr_kind(slot);
  int kv = ret ? seed_ret_kind(val)  : seed_ptr_kind(val);
  /* Two HASHES differ convertibly in their VALUE kind -- the emitter rebuilds
     one variant from another -- but never in their KEY kind. A Symbol-keyed
     hash can never satisfy a `Hash[String, ...]` slot, and passing that
     pointer unconverted had a Symbol dereferenced as a char * (#3975). A poly
     key holds either, so it is not a clash. */
  if (fs == 2 && ks == 2 && kv == 2) {
    TyKind kslot = ty_hash_key(slot), kval = ty_hash_key(val);
    return kslot != kval && kslot != TY_POLY && kval != TY_POLY &&
           kslot != TY_UNKNOWN && kval != TY_UNKNOWN;
  }
  /* Two ARRAYS whose ELEMENT kind differs are different C structs, and unlike
     a hash's VALUE kind the emitter does not rebuild one from the other at a
     return: rebuilding would hand back a copy, so writes through the getter
     would stop reaching the receiver's own array. The clash therefore reached
     cc, which named sp_IntArray / sp_PolyArray -- types the author has no way
     to connect back to the `Array[untyped]` they wrote (#4151). */
  if (fs == 2 && ks == 1 && kv == 1) {
    /* A POLY array is the one array kind the emitter DOES materialize into a
       typed one at a boundary (sp_StrArray_from_poly_array, #1827), so it can
       satisfy any array slot. A RETURN converts in the other direction too:
       a typed array entering a declared Array[untyped] return is rebuilt with
       its elements boxed, the same copy the unseeded compiler emits when a
       caller's use widens such a value (sp_PolyArray_from_int_array at the
       call site) -- so the wider declaration is satisfied, not contradicted
       (#4191). An IVAR store converts nothing (the slot keeps the object),
       so there only an identical kind will do. */
    if (val == TY_POLY_ARRAY || (ret && slot == TY_POLY_ARRAY)) return 0;
    return slot != val;
  }
  if (fs != 2 || !ks || !kv || ks == kv) return 0;
  return 1;
}

/* The same judgement for a seeded RETURN, which is decidable in two more
   cases than an ivar assignment. A seeded return converts NOTHING: an object
   defining #to_h or #to_str placed in a Hash or String slot is the same C
   error as one defining neither (verified), so a user object crossing into a
   container or String slot is as decidable as an Array is, and a by-value
   object -- which the shared family function leaves alone because it is
   neither a scalar nor a pointer -- is decidable too. Two OBJECTS still stay
   unjudged: a subclass in an ancestor's slot is legitimate. */
static int seed_ret_family(Compiler *c, TyKind t) {
  int f = seed_repr_family(c, t);
  if (f) return f;
  return ty_is_object(t) ? 3 : 0;
}
/* ty_name has no spelling for a user class (it answers "?"), which is exactly
   the half of the message that has to be readable here. */
/* The type as the AUTHOR would have written it in the RBS. The internal
   lattice names (int_array, poly_array) named nothing the signature contains,
   so a contradiction on a container read as being about types the author had
   no way to connect back to the `Array[untyped]` they wrote (#4151). The
   buffer is the caller's: one message prints two of these. */
static const char *seed_ty_name_into(Compiler *c, TyKind t, char *buf, size_t n) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].name) return c->classes[cid].name;
  }
  if (ty_is_array(t)) {
    char eb[128];
    TyKind e = ty_array_elem(t);
    snprintf(buf, n, "Array[%s]", seed_ty_name_into(c, e, eb, sizeof eb));
    return buf;
  }
  if (ty_is_hash(t)) {
    char kb[128], vb[128];
    snprintf(buf, n, "Hash[%s, %s]",
             seed_ty_name_into(c, ty_hash_key(t), kb, sizeof kb),
             seed_ty_name_into(c, ty_hash_val(t), vb, sizeof vb));
    return buf;
  }
  switch (t) {
    case TY_INT:    return "Integer";
    case TY_FLOAT:  return "Float";
    case TY_STRING: return "String";
    case TY_SYMBOL: return "Symbol";
    case TY_BOOL:   return "bool";
    case TY_NIL:    return "nil";
    case TY_PROC:   return "Proc";
    case TY_RANGE:  return "Range";
    case TY_POLY: case TY_UNKNOWN: return "untyped";
    default: break;
  }
  { const char *nm = ty_name(t); return nm ? nm : "?"; }
}
static int seed_ret_kind(TyKind t) {
  int k = seed_ptr_kind(t);
  return k ? k : (ty_is_object(t) ? 4 : 0);
}

/* An --rbs seed the program statically contradicts.
   A seed is trusted, so codegen narrows whatever arrives into the pinned slot
   and a wrong one reinterprets the value rather than converting it. Where both
   the seed and the value are concretely typed and one is a flat scalar while
   the other is a heap pointer, no conversion exists and none is emitted -- the
   result is a pointer read as an integer. That is decidable here, so refuse
   rather than emit it.
   Deliberately narrow. Two types in the SAME family may still disagree (a
   String slot fed a Symbol) but the emitter has coercions for many such pairs,
   and a false error breaks a build that works; the dynamic half of this is
   -DSP_RBS_CHECK, which needs no such caution because it sees the real tag. */
/* Defined in codegen_util.c: 1 under SP_COLLECT_ERRORS. Reached by name rather
   than through codegen_internal.h, which analyze does not include. */
int collect_mode(void);

/* One contradiction used to end the run. On a seeded tree that is the check hit
   first, and fixing one signature only buys the next one a whole compile later
   -- 115 seconds a round on a 347-signature tree (#4140). Under
   SP_COLLECT_ERRORS, report each one and keep going, then fail once at the end:
   the run still refuses to emit (a seed is trusted, so acting on a contradicted
   one is the bug this check exists to prevent), but it hands back the whole
   list. Same bargain the codegen gaps already make. */
static int g_seed_bad = 0;
static void check_seed_contradictions(Compiler *c) {
  const NodeTable *nt = c->nt;
  char _sn1[192], _sn2[192];
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_InstanceVariableWriteNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (!nm || v < 0) continue;
    Scope *sc = comp_scope_of(c, id);
    int cid = sc ? sc->class_id : -1;
    if (cid < 0) continue;
    ClassInfo *ci = &c->classes[cid];
    if (!class_ivar_pinned(ci, nm)) continue;
    int iv = comp_ivar_index(ci, nm);
    if (iv < 0) continue;
    TyKind slot = ci->ivar_types[iv];
    TyKind val = infer_type(c, v);
    if (!seed_contradicts(c, slot, val, 0)) continue;
    int ln  = (int)nt_int(nt, id, "node_line", 0);
    int fid = (int)nt_int(nt, id, "node_file", 0);
    const char *file = nt_file_path(nt, fid);
    if (!file || !*file) file = nt->source_file;
    if (!file || !*file) file = "source.rb";
    fprintf(stderr,
            "spinel: %s:%d: --rbs seed contradicted: %s is declared %s but this "
            "assigns %s\n"
            "  A seed is trusted, so the emitted code would reinterpret the value "
            "rather than convert it.\n"
            "  Fix the signature or the assignment.\n",
            file, ln, nm, seed_ty_name_into(c, slot, _sn1, sizeof _sn1),
            seed_ty_name_into(c, val, _sn2, sizeof _sn2));
    if (!collect_mode()) exit(1);
    g_seed_bad = 1;
  }
  /* The same contradiction on a RETURN. A seeded return is trusted, so the
     emitted function carries the pinned C type and the body's value is placed
     in it as-is: a String body under a `-> Hash[...]` seed returns a char* from
     a function typed sp_SymPolyHash*, which only the C compiler used to report
     -- against generated code, and in its voice rather than spinel's (#4005).
     Same judgement as the ivar rule above: decidable only where both sides are
     concretely typed and their representations cannot convert. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->ret_rbs_seeded || sc->body < 0 || !sc->reachable) continue;
    /* A definition a later `def` has REPLACED cannot be called: dispatch
       resolves to the last one, and did before this rule existed. Judging the
       replaced body refused a program spinel compiles and runs correctly
       (#4024). Only the definition the chain actually resolves to is judged. */
    if (sc->class_id >= 0 && sc->name) {
      int eff = sc->is_cmethod ? comp_cmethod_in_chain(c, sc->class_id, sc->name, NULL)
                               : comp_method_in_chain(c, sc->class_id, sc->name, NULL);
      if (eff >= 0 && eff != s) continue;
    }
    TyKind slot = sc->ret;
    /* every value the method can answer with: the body's tail, and each
       explicit return inside this scope */
    int sites[64]; int nsites = 0;
    sites[nsites++] = sc->body;
    NT_FOREACH_KIND(nt, NK_ReturnNode, id) {
      if (nsites >= 64) break;
      if (c->nscope[id] != s) continue;
      int rv = nt_ref(nt, id, "arguments");
      if (rv < 0) continue;
      int an = 0; const int *av = nt_arr(nt, rv, "arguments", &an);
      if (av && an == 1) sites[nsites++] = av[0];
    }
    for (int k = 0; k < nsites; k++) {
      /* An EMPTY container literal carries a default kind, not evidence: a
         bare `{}` reads as the String-keyed variant and so contradicted every
         Symbol-keyed seed, though it has no keys to disagree about and the
         seed is the only thing in the program that says which kind it is
         (#4025). The seed wins; there is nothing here to judge. */
      { int lit = sites[k];
        if (nt_kind(nt, lit) == NK_StatementsNode) {
          int bn2 = 0; const int *bb2 = nt_arr(nt, lit, "body", &bn2);
          lit = (bn2 > 0 && bb2) ? bb2[bn2 - 1] : -1;
        }
        if (lit >= 0) {
        NodeKind sk = nt_kind(nt, lit);
        if (sk == NK_HashNode || sk == NK_KeywordHashNode || sk == NK_ArrayNode) {
          int en = 0; nt_arr(nt, lit, "elements", &en);
          /* A RETURNED container literal is built at the declared kind -- the
             return boundary carries the seed's type and the literal has no
             identity to preserve, so there is nothing here to judge either
             (#3279). An ivar WRITE of a literal is a different matter: the
             ivar keeps the object, so its kind has to be the declared one. */
          if (en > 0 && sk == NK_ArrayNode && ty_is_array(slot)) continue;
          if (en == 0) {
            /* An EMPTY container literal carries a DEFAULT kind, not evidence:
               a bare `{}` reads as the String-keyed variant and contradicted
               every Symbol-keyed seed, though it has no keys to disagree about.
               The seed is the only thing in the program that says which kind it
               is, so the seed wins -- and the literal is built at it, or the
               emitted return would still be the default kind (#4025). */
            if (lit < c->node_cap &&
                ((ty_is_hash(slot) && (sk == NK_HashNode || sk == NK_KeywordHashNode)) ||
                 (ty_is_array(slot) && sk == NK_ArrayNode))) {
              c->ntype[lit] = slot;
              if (c->hash_want && ty_is_hash(slot)) c->hash_want[lit] = slot;
            }
            continue;
          }
        }
        }
      }
      TyKind val = infer_type(c, sites[k]);
      if (!seed_contradicts(c, slot, val, 1)) continue;
      int ln  = (int)nt_int(nt, sites[k], "node_line", 0);
      int fid = (int)nt_int(nt, sites[k], "node_file", 0);
      const char *file = nt_file_path(nt, fid);
      if (!file || !*file) file = nt->source_file;
      if (!file || !*file) file = "source.rb";
      const char *cn = (sc->class_id >= 0 && sc->class_id < c->nclasses)
                         ? c->classes[sc->class_id].name : NULL;
      char pos[1200];
      if (ln > 0) snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
      else        snprintf(pos, sizeof pos, "%s: ", file);
      fprintf(stderr,
              "spinel: %s--rbs seed contradicted: %s%s%s is declared to return "
              "%s but this returns %s\n"
              "  A seed is trusted, so the emitted function carries the declared type "
              "and the value is placed in it rather than converted.\n"
              "  Fix the signature or the body.\n",
              pos, cn ? cn : "", cn ? "#" : "", sc->name ? sc->name : "?",
              seed_ty_name_into(c, slot, _sn1, sizeof _sn1),
              seed_ty_name_into(c, val, _sn2, sizeof _sn2));
      if (!collect_mode()) exit(1);
      g_seed_bad = 1;
    }
  }

  /* The same contradiction one step earlier: an argument whose type is
     concretely known, handed to a parameter the seed pinned to a type in the
     other representation family. Codegen trusts the pin, so the emitted C
     reads a hash pointer as a C string rather than converting it. Only the
     plain positional shape is judged -- a rest/keyword param, a splat
     argument, or a call whose target cannot be resolved statically leaves the
     mapping in doubt, and a false error breaks a working build. */
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int recv = nt_ref(nt, id, "receiver");
    int mi = -1;
    if (recv < 0) mi = comp_method_index(c, name);
    else if (nt_kind(nt, recv) == NK_ConstantReadNode) {
      const char *cn = nt_str(nt, recv, "name");
      int rc = cn ? comp_class_index(c, cn) : -1;
      /* `Mod.m` is a class (singleton) method, which is not in the instance
         chain; fall back to it before giving up. */
      if (rc >= 0) mi = comp_cmethod_in_chain(c, rc, name, NULL);
      if (rc >= 0 && mi < 0) mi = comp_method_in_chain(c, rc, name, NULL);
    }
    else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
    }
    if (mi < 0 || mi >= c->nscopes) continue;
    Scope *m = &c->scopes[mi];
    if (m->rest_idx >= 0 || m->kwrest_idx >= 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (!argv || argc > m->nparams) continue;
    for (int i = 0; i < argc; i++) {
      if (nt_kind(nt, argv[i]) == NK_SplatNode ||
          nt_kind(nt, argv[i]) == NK_KeywordHashNode ||
          nt_kind(nt, argv[i]) == NK_BlockArgumentNode) break;
      LocalVar *lv = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
      if (!lv || !lv->rbs_seeded) continue;
      /* The DECLARED type is what a seed promises. Inference may narrow the
         slot afterwards -- an `untyped` (poly) parameter whose body indexes it
         with a symbol key reads as a symbol-keyed hash -- and that narrowing is
         spinel's own guess, not a declaration to hold a call site to (#3977). */
      TyKind slot = (lv->rbs_type != TY_UNKNOWN) ? lv->rbs_type : lv->type;
      TyKind val = infer_type(c, argv[i]);
      if (slot == TY_POLY) continue;   /* `untyped` accepts anything */
      if (!seed_contradicts(c, slot, val, 0)) continue;
      int ln  = (int)nt_int(nt, argv[i], "node_line", 0);
      int fid = (int)nt_int(nt, argv[i], "node_file", 0);
      const char *file = nt_file_path(nt, fid);
      if (!file || !*file) file = nt->source_file;
      if (!file || !*file) file = "source.rb";
      fprintf(stderr,
              "spinel: %s:%d: --rbs seed contradicted: parameter %s of %s is "
              "declared %s but this call passes %s\n"
              "  A seed is trusted, so the emitted code would reinterpret the value "
              "rather than convert it.\n"
              "  Fix the signature or the call.\n",
              file, ln, m->pnames[i], name,
              seed_ty_name_into(c, slot, _sn1, sizeof _sn1),
              seed_ty_name_into(c, val, _sn2, sizeof _sn2));
      if (!collect_mode()) exit(1);
      g_seed_bad = 1;
    }
  }
  /* Collect mode gathered them; the run still fails, and stops before codegen
     so nothing is emitted from a seed we just refused. */
  if (g_seed_bad) exit(1);
}

/* Re-assert the type an --rbs seed declared for a parameter. The seed is the
   promise the emitted code is allowed to trust, and inference narrows a slot
   from evidence that is only about ONE call site -- an `untyped` (poly)
   parameter whose body reads `value[:key]` looks like a symbol-keyed hash, and
   every other caller then had its argument reinterpreted through that layout
   (#3977). A concrete seed is already left alone by the passes that check
   rbs_seeded; this catches the ones that do not. */
static int reassert_rbs_param_seeds(Compiler *c) {
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nlocals; i++) {
      LocalVar *lv = &sc->locals[i];
      if (!lv->is_param || !lv->rbs_seeded) continue;
      if (lv->rbs_type == TY_UNKNOWN || lv->type == lv->rbs_type) continue;
      lv->type = lv->rbs_type;
      changed = 1;
    }
  }
  return changed;
}

/* Names whose miss answers nil while the type stays TY_INT (a search index, a
   pop off an empty array): the value they leave in the slot is the sentinel. */
static int nullable_int_call_name(const char *nm) {
  if (!nm) return 0;
  static const char *const N[] = {
    "index", "rindex", "byteindex", "byterindex", "delete_at", "pop", "shift",
    "delete", "nonzero?", "infinite?", "getbyte", "bsearch", "bsearch_index",
    /* `a <=> b` answers nil when the two are not comparable, and the poly
       helper spells that with the sentinel like every other nullable int */
    "<=>", NULL };
  for (int i = 0; N[i]; i++) if (sp_streq(nm, N[i])) return 1;
  return 0;
}
/* A scalar slot on a class the fixpoint could not pin to a receiver still
   dispatches at runtime: codegen emits a cls_id switch over every class that
   defines the name. Ask whether ANY of those targets can answer the sentinel.
   Over-marking here only costs the boxing branch; missing one is the
   silent-wrong hash key of #3505, so the conservative direction is `yes`. */
static int poly_dispatch_nullable(Compiler *c, const char *cn) {
  if (!cn) return 0;
  for (int si = 1; si < c->nscopes; si++)
    if (c->scopes[si].name && sp_streq(c->scopes[si].name, cn) &&
        c->scopes[si].ret_nullable_int) return 1;
  /* an attr_reader over a scalar ivar: those slots are sentinel-defaulted
     (ivar_scalar_nil_init), so the read carries the sentinel like a `return
     nil` would -- the resolved-receiver twin of this lives in codegen's
     call_returns_nullable_int */
  for (int ci = 0; ci < c->nclasses; ci++) {
    if (!comp_reader_in_chain(c, ci, cn, NULL)) continue;
    char ivb[300];
    snprintf(ivb, sizeof ivb, "@%s", comp_resolve_alias(c, ci, cn));
    int iv = comp_ivar_index(&c->classes[ci], ivb);
    if (iv >= 0 && (c->classes[ci].ivar_types[iv] == TY_INT ||
                    c->classes[ci].ivar_types[iv] == TY_FLOAT)) return 1;
  }
  return 0;
}

/* The int/float-array local `nm` is read or written at `at`, and is a candidate
   for the element marking: NULL when it is not one, or is already marked. */
static LocalVar *nullable_elem_local(Compiler *c, int at, const char *nm) {
  Scope *sc = nm ? comp_scope_of(c, at) : NULL;
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  if (!lv || lv->nullable_int_elem) return NULL;
  return (lv->type == TY_INT_ARRAY || lv->type == TY_FLOAT_ARRAY) ? lv : NULL;
}

/* The ivar an ivar read/write node names, for the element marking. */
static int nullable_elem_ivar(Compiler *c, int at, ClassInfo **out) {
  Scope *s = comp_scope_of(c, at);
  int cid = s ? s->class_id : -1;
  if (cid < 0) cid = comp_class_index(c, "Toplevel");
  if (cid < 0 || cid >= c->nclasses) return -1;
  *out = &c->classes[cid];
  return comp_ivar_index(*out, nt_str(c->nt, at, "name"));
}

static int name_in(const char *nm, const char *const *set) {
  if (!nm) return 0;
  for (int i = 0; set[i]; i++) if (sp_streq(nm, set[i])) return 1;
  return 0;
}

/* An array method whose result elements are the receiver's own, so element
   nilability passes straight through it. `compact` is deliberately absent: it
   is what REMOVES the nils. */
static int elem_preserving_call(const char *nm) {
  static const char *const N[] = { "select", "filter", "reject", "sort", "sort_by",
                                   "uniq", "reverse", "rotate", "take", "drop",
                                   "take_while", "drop_while", "shuffle", "to_a",
                                   "dup", "clone", "freeze", "values_at", "slice",
                                   "flatten", "first", "last", NULL };
  return name_in(nm, N);
}

/* A method that hands back one ELEMENT of its receiver. */
static int elem_returning_call(const char *nm) {
  static const char *const N[] = { "[]", "at", "first", "last", "min", "max",
                                   "sample", "pop", "shift", "fetch", "dig", NULL };
  return name_in(nm, N);
}

/* The tail expression of a literal block attached to `call`, or -1. */
static int call_block_tail(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, call, "block");
  if (blk < 0) return -1;
  int body = nt_ref(nt, blk, "body");
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return -1;
  int n = 0; const int *st = nt_arr(nt, body, "body", &n);
  return st && n > 0 ? st[n - 1] : -1;
}

/* Can an ELEMENT of this array-valued expression be the sentinel? A slot
   answers from its own marking; an expression that builds or forwards an array
   answers from what it was built out of, which is what lets a chain with no
   local in it (`[r].map { |x| x.p_ }[0]`) be seen at all (#3505). */
static int nullable_int_elem_expr(Compiler *c, int v, int depth);

/* One level further in: can an element of a container HELD BY `v` be the
   sentinel? `v` is the outer container -- an array of arrays, a hash whose
   values are arrays, or a slot holding one -- so `v[k][j]` reaches the scalar. */
static int nested_elem_nilable(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0 || depth > 8) return 0;
  if (nt_kind(nt, v) == NK_ArrayNode || nt_kind(nt, v) == NK_HashNode) {
    int en = 0; const int *els = nt_arr(nt, v, "elements", &en);
    int hash = nt_kind(nt, v) == NK_HashNode;
    for (int k = 0; els && k < en; k++) {
      int e = hash ? nt_ref(nt, els[k], "value") : els[k];
      if (nullable_int_elem_expr(c, e, depth + 1)) return 1;
    }
    return 0;
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *sc = comp_scope_of(c, v);
    const char *nm = nt_str(nt, v, "name");
    if (!sc || !nm) return 0;
    for (int r = lw_shared_first(c, nm, (int)(sc - c->scopes)); r >= 0; r = lw_shared_next(r)) {
      int id = lw_shared_node(r);
      if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, id, "name");
      if (!wn || !sp_streq(wn, nm) || comp_scope_of(c, id) != sc) continue;
      if (nested_elem_nilable(c, nt_ref(nt, id, "value"), depth + 1)) return 1;
    }
  }
  return 0;
}

static int nullable_int_elem_expr(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0 || depth > 8) return 0;
  if (nt_kind(nt, v) == NK_ArrayNode) {
    int en = 0; const int *els = nt_arr(nt, v, "elements", &en);
    for (int k = 0; els && k < en; k++) if (nullable_int_value(c, els[k])) return 1;
    return 0;
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    const char *n = nt_str(nt, v, "name");
    LocalVar *lv = n && s ? scope_local(s, n) : NULL;
    return lv && lv->nullable_int_elem;
  }
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    ClassInfo *ci = NULL;
    int iv = nullable_elem_ivar(c, v, &ci);
    return iv >= 0 && ci->ivar_nullable_int_elem[iv];
  }
  if (nt_kind(nt, v) == NK_ParenthesesNode) {
    int pb = nt_ref(nt, v, "body");
    int pn = 0; const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    return pd && pn == 1 ? nullable_int_elem_expr(c, pd[0], depth + 1) : 0;
  }
  if (nt_kind(nt, v) == NK_CallNode) {
    const char *nm = nt_str(nt, v, "name");
    int tail = call_block_tail(c, v);
    /* the mapped element IS the block's value */
    if (nm && tail >= 0 && (sp_streq(nm, "map") || sp_streq(nm, "collect") ||
                            sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")))
      return nullable_int_value(c, tail);
    int rc = nt_ref(nt, v, "receiver");
    /* `Array.new(n) { ... }` / `Array.new(n, v)` builds its elements here */
    if (nm && sp_streq(nm, "new") && rc >= 0 && nt_kind(nt, rc) == NK_ConstantReadNode) {
      const char *rn = nt_str(nt, rc, "name");
      if (rn && sp_streq(rn, "Array")) {
        if (tail >= 0) return nullable_int_value(c, tail);
        int ca = nt_ref(nt, v, "arguments"); int an = 0;
        const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
        return an >= 2 && nullable_int_value(c, av[1]);
      }
    }
    if (elem_preserving_call(nm)) return nullable_int_elem_expr(c, rc, depth + 1);
    /* the value is one ELEMENT of the receiver, and that element is itself the
       container being indexed into (`t[i][j]`, `h[:a][0]`) */
    if (elem_returning_call(nm)) return nested_elem_nilable(c, rc, depth + 1);
    /* a method returning such an array: its own tail decides */
    if (nm && rc < 0) {
      int mi = comp_method_index(c, nm);
      if (mi > 0) return nullable_int_elem_expr(c, scope_body_last(c, mi), depth + 1);
    }
    return 0;
  }
  return 0;
}

/* An index read whose RECEIVER is an array that can hold the sentinel. The
   value comes back already boxed by the runtime read, so codegen corrects it
   there; asked receiver-first on purpose, since the general predicate resolves
   a bare `[]` by name across the whole program and would wrap hot reads that
   can never carry one. */
int nullable_int_elem_read(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  if (call < 0 || nt_kind(nt, call) != NK_CallNode) return 0;
  int recv = nt_ref(nt, call, "receiver");
  return recv >= 0 && elem_returning_call(nt_str(nt, call, "name")) &&
         nullable_int_elem_expr(c, recv, 0);
}

/* A call that answers one element of its receiver, or nil when there is none
   (`a[i]`, `h[k]`, `[].max`, `a.find { }`), plus `Integer(s, exception: false)`. */
static int elem_miss_call(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, v, "name");
  if (!nm) return 0;
  int recv = nt_ref(nt, v, "receiver");
  int args = nt_ref(nt, v, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int blk = nt_ref(nt, v, "block");
  if (recv < 0) {
    if ((sp_streq(nm, "Integer") || sp_streq(nm, "Float")) && argc >= 2 && argv &&
        nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode) return 1;
    return 0;
  }
  if (sp_streq(nm, "[]") || sp_streq(nm, "at")) return argc == 1 && blk < 0;
  if (sp_streq(nm, "dig")) return argc >= 1;
  if (sp_streq(nm, "first") || sp_streq(nm, "last") || sp_streq(nm, "sample"))
    return argc == 0 && blk < 0;
  if (sp_streq(nm, "min") || sp_streq(nm, "max")) return argc == 0;
  if (sp_streq(nm, "find") || sp_streq(nm, "detect")) return blk >= 0;
  return 0;
}

/* Can this expression leave the sentinel in an int slot? */
/* Whether an unconditional write of ivar `ivn` is among the top-level
   statements of class k's initialize, or of the initialize it inherits. */
int ivar_assigned_in_initialize(Compiler *c, int k, const char *ivn) {
  const NodeTable *nt = c->nt;
  int mi = comp_method_in_chain(c, k, "initialize", NULL);
  if (mi < 0 || !ivn) return 0;
  int body = c->scopes[mi].body;
  if (body < 0) return 0;
  int n = 0; const int *st = nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : &body;
  if (nt_kind(nt, body) != NK_StatementsNode) n = 1;
  for (int i = 0; i < n; i++) {
    int w = st[i];
    /* `@a = @b = 0` assigns both */
    for (int x = w; x >= 0 && nt_kind(nt, x) == NK_InstanceVariableWriteNode; x = nt_ref(nt, x, "value")) {
      const char *wn = nt_str(nt, x, "name");
      if (wn && sp_streq(wn, ivn)) return 1;
    }
    if (nt_kind(nt, w) == NK_MultiWriteNode) {
      int ln = 0; const int *ls = nt_arr(nt, w, "lefts", &ln);
      for (int j = 0; j < ln; j++)
        if (nt_kind(nt, ls[j]) == NK_InstanceVariableTargetNode &&
            nt_str(nt, ls[j], "name") && sp_streq(nt_str(nt, ls[j], "name"), ivn)) return 1;
    }
  }
  return 0;
}

/* An argument whose boxing must allow nil though its typed reads need no
   nil check: an int ivar read nothing has to assign first, or a parameter
   already bound from one. */
int box_nullable_arg(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0 || cid >= c->nclasses) return 0;
    ClassInfo *ci = &c->classes[cid];
    const char *ivn = nt_str(nt, v, "name");
    int iv = comp_ivar_index(ci, ivn);
    if (iv < 0 || (ci->ivar_types[iv] != TY_INT && ci->ivar_types[iv] != TY_FLOAT)) return 0;
    return !ivar_assigned_in_initialize(c, cid, ivn);
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    const char *ln = nt_str(nt, v, "name");
    LocalVar *lv = s && ln ? scope_local(s, ln) : NULL;
    return lv && lv->is_param && lv->box_nullable;
  }
  return 0;
}

int nullable_int_value(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_NilNode) return 1;
  /* `return e` / `(e)` carry their inner value unchanged; a block tail can be
     either, and so can a method's own tail statement. */
  if (nt_kind(nt, v) == NK_ReturnNode) {
    int rv = nt_ref(nt, v, "arguments");
    int rn = 0; const int *ra = rv >= 0 ? nt_arr(nt, rv, "arguments", &rn) : NULL;
    return ra && rn == 1 ? nullable_int_value(c, ra[0]) : 0;
  }
  /* A conditional's value is one of its arms, so it carries the sentinel if any
     arm can. A ternary, an `if`/`unless` or `case` used as an expression, a
     begin/rescue and `a || b` all reach a Hash key this way (#3505). An arm
     that is absent yields nil, which is the sentinel in an int slot. */
  if (nt_kind(nt, v) == NK_StatementsNode) {
    int n = 0; const int *st = nt_arr(nt, v, "body", &n);
    return st && n > 0 ? nullable_int_value(c, st[n - 1]) : 1;   /* an empty body is nil */
  }
  if (nt_kind(nt, v) == NK_ElseNode) {
    int es = nt_ref(nt, v, "statements");
    return es < 0 || nullable_int_value(c, es);
  }
  if (nt_kind(nt, v) == NK_IfNode || nt_kind(nt, v) == NK_UnlessNode) {
    int ts = nt_ref(nt, v, "statements");
    if (ts < 0 || nullable_int_value(c, ts)) return 1;   /* an empty arm is nil */
    int els = nt_ref(nt, v, nt_kind(nt, v) == NK_IfNode ? "subsequent" : "else_clause");
    return els >= 0 ? nullable_int_value(c, els) : 1;
  }
  if (nt_kind(nt, v) == NK_CaseNode || nt_kind(nt, v) == NK_CaseMatchNode) {
    int nw = 0; const int *whens = nt_arr(nt, v, "conditions", &nw);
    for (int w = 0; w < nw; w++) {
      int ws = nt_ref(nt, whens[w], "statements");
      if (ws < 0 || nullable_int_value(c, ws)) return 1;   /* an empty arm is nil */
    }
    int els = nt_ref(nt, v, "else_clause");
    return els >= 0 ? nullable_int_value(c, els) : 1;
  }
  if (nt_kind(nt, v) == NK_BeginNode) {
    if (nullable_int_value(c, nt_ref(nt, v, "statements"))) return 1;
    for (int rs = nt_ref(nt, v, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent"))
      if (nullable_int_value(c, nt_ref(nt, rs, "statements"))) return 1;
    int els = nt_ref(nt, v, "else_clause");
    return els >= 0 && nullable_int_value(c, nt_ref(nt, els, "statements"));
  }
  if (nt_kind(nt, v) == NK_RescueModifierNode)
    return nullable_int_value(c, nt_ref(nt, v, "expression")) ||
           nullable_int_value(c, nt_ref(nt, v, "rescue_expression"));
  if (nt_kind(nt, v) == NK_OrNode || nt_kind(nt, v) == NK_AndNode)
    return nullable_int_value(c, nt_ref(nt, v, "left")) ||
           nullable_int_value(c, nt_ref(nt, v, "right"));
  /* Reading a slot some write left the sentinel in: the reader method a caller
     resolves to is this read, so its callers box through it too. */
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0 || cid >= c->nclasses) return 0;
    ClassInfo *ci = &c->classes[cid];
    int iv = comp_ivar_index(ci, nt_str(nt, v, "name"));
    return iv >= 0 && ci->ivar_nullable_int[iv];
  }
  if (nt_kind(nt, v) == NK_ParenthesesNode) {
    int pb = nt_ref(nt, v, "body");
    int pn = 0; const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    return pd && pn == 1 ? nullable_int_value(c, pd[0]) : 0;
  }
  /* The value of `yield x` is the BLOCK's, decided per call site. The yield's
     own type says only TY_INT, which an `Integer?` and an `Integer` share, so
     ask the blocks themselves (#3505). */
  if (nt_kind(nt, v) == NK_YieldNode) {
    Scope *ys = comp_scope_of(c, v);
    int ymi = ys ? (int)(ys - c->scopes) : -1;
    if (ymi < 0) return 0;
    int tails[32];
    int n = yield_block_tails(c, ymi, tails, (int)(sizeof tails / sizeof tails[0]));
    /* no literal block in sight (an escaping &blk called through the proc ABI):
       its value arrives boxed, so nothing unboxed carries a sentinel */
    for (int i = 0; i < n; i++) if (nullable_int_value(c, tails[i])) return 1;
    return 0;
  }
  if (nt_kind(nt, v) == NK_CallNode) {
    if (nullable_int_call_name(nt_str(nt, v, "name"))) return 1;
    /* a missed element read is the sentinel in an int slot; only boxing is
       affected, typed reads keep their inline arms */
    if (elem_miss_call(c, v)) return 1;
    /* a method whose --rbs signature pins `Integer?`, or whose own return
       expression can be the sentinel: either way its nil is the sentinel and a
       caller that boxes the value has to answer nil. Resolved the way emission
       resolves it. */
    const char *cn = nt_str(nt, v, "name");
    int rcv0 = nt_ref(nt, v, "receiver");
    /* an element read out of an array some element of which is the sentinel */
    if (rcv0 >= 0 && elem_returning_call(cn) && nullable_int_elem_expr(c, rcv0, 0)) return 1;
    /* a fold's value is its block's, and `find`/`detect` hand back an element */
    if (cn && (sp_streq(cn, "reduce") || sp_streq(cn, "inject"))) {
      int ft = call_block_tail(c, v);
      if (ft >= 0 && nullable_int_value(c, ft)) return 1;
    }
    if (rcv0 >= 0 && cn && (sp_streq(cn, "find") || sp_streq(cn, "detect")) &&
        nullable_int_elem_expr(c, rcv0, 0)) return 1;
    int mi = cn ? comp_method_index(c, cn) : -1;
    int rcv = nt_ref(nt, v, "receiver");
    if (mi < 0 && cn) {
      if (rcv < 0) {
        Scope *self = comp_scope_of(c, v);
        if (self && self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, cn, NULL);
      }
      else {
        TyKind rt = infer_type(c, rcv);
        if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), cn, NULL);
        else if (nt_kind(nt, rcv) == NK_ConstantReadNode) {
          int rci = comp_class_index(c, nt_str(nt, rcv, "name"));
          if (rci >= 0) mi = comp_cmethod_in_chain(c, rci, cn, NULL);
        }
        /* the receiver stayed poly, so no single callee resolves -- fall back
           to the runtime dispatch set */
        else if (rt == TY_POLY) return poly_dispatch_nullable(c, cn);
      }
    }
    return mi >= 0 && c->scopes[mi].ret_nullable_int;
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    const char *rn = nt_str(nt, v, "name");
    Scope *rs = rn ? comp_scope_of(c, v) : NULL;
    LocalVar *rv = rs ? scope_local(rs, rn) : NULL;
    return rv && rv->nullable_int;
  }
  return 0;
}

/* ---- array-or-nil proof (GC root elision) ----

   A poly slot proven to hold only a poly array or nil has an index read that
   takes the runtime's inline array arm: no allocation, and no re-entry into
   Ruby code (the Struct arm calls a generated `to_h`, the String arm allocates
   a character). That is what lets codegen drop the slot's GC root -- the value
   stays reachable from the container it was read out of, and nothing in its
   live range can move it. Conservative in both directions: anything the walk
   does not recognise answers "no". */
static int aon_value(Compiler *c, int v, int depth);

/* Is every ELEMENT of the container-valued expression `v` an array or nil? */
static int aon_container(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0 || depth > 8) return 0;
  if (nt_kind(nt, v) == NK_ArrayNode) {
    int en = 0; const int *els = nt_arr(nt, v, "elements", &en);
    for (int k = 0; els && k < en; k++) if (!aon_value(c, els[k], depth + 1)) return 0;
    return en > 0;
  }
  if (nt_kind(nt, v) == NK_CallNode) {
    const char *nm = nt_str(nt, v, "name");
    int rc = nt_ref(nt, v, "receiver");
    /* `[nil] * n` and `Array.new(n)` fill with nil; `xs.map { [..] }` fills
       with whatever the block's tail builds. */
    if (nm && rc >= 0 && (sp_streq(nm, "*") || sp_streq(nm, "freeze") ||
                          sp_streq(nm, "dup") || sp_streq(nm, "clone") ||
                          sp_streq(nm, "to_a")))
      return aon_container(c, rc, depth + 1);
    if (nm && (sp_streq(nm, "map") || sp_streq(nm, "collect"))) {
      int blk = nt_ref(nt, v, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      return bs && bn > 0 && aon_value(c, bs[bn - 1], depth + 1);
    }
    if (nm && sp_streq(nm, "new") && rc >= 0 && nt_kind(nt, rc) == NK_ConstantReadNode) {
      const char *rn = nt_str(nt, rc, "name");
      int blk = nt_ref(nt, v, "block");
      if (rn && sp_streq(rn, "Array") && blk < 0) {
        int ca = nt_ref(nt, v, "arguments"); int an = 0;
        const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
        return an == 1 || (an >= 2 && aon_value(c, av[1], depth + 1));
      }
    }
    return 0;
  }
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    ClassInfo *ci = NULL;
    int iv = nullable_elem_ivar(c, v, &ci);
    return iv >= 0 && ci->ivar_arr_elem_arr_or_nil[iv];
  }
  /* a container LOCAL: the same scan as the ivar case, over its own writes and
     over every store into it. `entries[key] ||= [..]` fills a hash this way,
     and an `.map` over it is what the ivar above ends up holding. */
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *sc = comp_scope_of(c, v);
    const char *nm = nt_str(nt, v, "name");
    if (!sc || !nm) return 0;
    int saw = 0;
    for (int r = lw_shared_first(c, nm, (int)(sc - c->scopes)); r >= 0; r = lw_shared_next(r)) {
      int id = lw_shared_node(r);
      if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, id, "name");
      if (!wn || !sp_streq(wn, nm) || comp_scope_of(c, id) != sc) continue;
      int wv = nt_ref(nt, id, "value");
      /* an empty literal carries no element evidence of its own; the stores
         below are what fill it */
      if (wv >= 0 && (nt_kind(nt, wv) == NK_HashNode || nt_kind(nt, wv) == NK_ArrayNode)) {
        int en = 0; nt_arr(nt, wv, "elements", &en);
        if (en == 0) { saw = 1; continue; }
      }
      if (!aon_container(c, wv, depth + 1)) return 0;
      saw = 1;
    }
    if (!saw) return 0;
    /* every store into it must put an array or nil there */
    NT_FOREACH_KIND(nt, NK_CallNode, w) {
      const char *wn2 = nt_str(nt, w, "name");
      if (!wn2 || (!sp_streq(wn2, "[]=") && !sp_streq(wn2, "push") &&
                   !sp_streq(wn2, "<<") && !sp_streq(wn2, "unshift") &&
                   !sp_streq(wn2, "store"))) continue;
      int wr = nt_ref(nt, w, "receiver");
      if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) continue;
      const char *rn2 = nt_str(nt, wr, "name");
      if (!rn2 || !sp_streq(rn2, nm) || comp_scope_of(c, wr) != sc) continue;
      int ca = nt_ref(nt, w, "arguments"); int an = 0;
      const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
      int val = an >= 1 ? av[an - 1] : -1;
      if (!aon_value(c, val, depth + 1)) return 0;
    }
    NT_FOREACH_KIND(nt, NK_IndexOrWriteNode, w) {
      int wr = nt_ref(nt, w, "receiver");
      if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) continue;
      const char *rn2 = nt_str(nt, wr, "name");
      if (!rn2 || !sp_streq(rn2, nm) || comp_scope_of(c, wr) != sc) continue;
      if (!aon_value(c, nt_ref(nt, w, "value"), depth + 1)) return 0;
    }
    return 1;
  }
  return 0;
}

/* Is the value of `v` an array or nil? */
static int aon_value(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  if (v < 0 || depth > 8) return 0;
  switch (nt_kind(nt, v)) {
    case NK_NilNode: case NK_ArrayNode: return 1;
    /* `a[i] = x = <expr>`: an assignment's value is what it assigned */
    case NK_LocalVariableWriteNode:
    case NK_InstanceVariableWriteNode:
      return aon_value(c, nt_ref(nt, v, "value"), depth + 1);
    case NK_LocalVariableReadNode: {
      Scope *sc = comp_scope_of(c, v);
      const char *nm = nt_str(nt, v, "name");
      LocalVar *lv = nm && sc ? scope_local(sc, nm) : NULL;
      return lv && lv->arr_or_nil == 1;
    }
    case NK_CallNode: {
      const char *nm = nt_str(nt, v, "name");
      int rc = nt_ref(nt, v, "receiver");
      if (!nm || rc < 0) return 0;
      /* one element out of a container whose elements are all arrays or nil */
      if (sp_streq(nm, "[]") || sp_streq(nm, "at") || sp_streq(nm, "fetch"))
        return aon_container(c, rc, depth + 1);
      return 0;
    }
    default: return 0;
  }
}

/* Public form of the array-or-nil proof, for codegen to emit an index without
   the poly dispatch. Deliberately narrower than aon_value: only a LOCAL the
   fixpoint marked, not the `container[i]` case aon_value also admits.

   That case is sound for what it was written for -- the root elision asks
   whether a read can allocate or re-enter, and reading a non-array through the
   general helper does neither -- but not for choosing the helper, where being
   wrong about the receiver's kind is a wrong VALUE. A container whose elements
   are "all arrays or nil" by the container proof turned out to include one
   that was neither (test/rbs-seed/nilable_scalar_paths). */
int expr_is_arr_or_nil(Compiler *c, int v) {
  if (v < 0 || nt_kind(c->nt, v) != NK_LocalVariableReadNode) return 0;
  Scope *sc = comp_scope_of(c, v);
  const char *nm = nt_str(c->nt, v, "name");
  LocalVar *lv = nm && sc ? scope_local(sc, nm) : NULL;
  return lv && lv->arr_or_nil == 1 && lv->type == TY_POLY;
}

/* Prove the flags to a fixpoint: a local reads out of an ivar whose elements
   come from another ivar, so one pass is not enough. Monotone (flags only get
   set), bounded by the number of flags. */
static void mark_array_or_nil_slots(Compiler *c) {
  const NodeTable *nt = c->nt;
  long rounds = c->nscopes + 2;
  for (int ci = 0; ci < c->nclasses; ci++) rounds += c->classes[ci].nivars;
  for (long round = 0; round < rounds; round++) {
    int changed = 0;
    /* container ivars: every write must fill with arrays or nil */
    NT_FOREACH_KIND(nt, NK_InstanceVariableWriteNode, id) {
      ClassInfo *ci = NULL;
      int iv = nullable_elem_ivar(c, id, &ci);
      if (iv < 0 || ci->ivar_arr_elem_arr_or_nil[iv]) continue;
      if (ci->ivar_types[iv] != TY_POLY_ARRAY) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0 || !aon_container(c, v, 0)) continue;
      /* every element STORE into it must agree too */
      int ok = 1;
      NT_FOREACH_KIND(nt, NK_CallNode, w) {
        const char *wn = nt_str(nt, w, "name");
        if (!wn || (!sp_streq(wn, "[]=") && !sp_streq(wn, "push") &&
                    !sp_streq(wn, "<<") && !sp_streq(wn, "unshift"))) continue;
        int wr = nt_ref(nt, w, "receiver");
        if (wr < 0 || nt_kind(nt, wr) != NK_InstanceVariableReadNode) continue;
        ClassInfo *wc = NULL;
        int wiv = nullable_elem_ivar(c, wr, &wc);
        if (wc != ci || wiv != iv) continue;
        int ca = nt_ref(nt, w, "arguments"); int an = 0;
        const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
        int val = sp_streq(wn, "[]=") ? (an >= 2 ? av[an - 1] : -1) : (an >= 1 ? av[0] : -1);
        if (!aon_value(c, val, 0)) { ok = 0; break; }
      }
      if (ok) { ci->ivar_arr_elem_arr_or_nil[iv] = 1; changed = 1; }
    }
    /* locals: every write must be an array, nil, or such an element read */
    for (int s = 0; s < c->nscopes; s++) {
      Scope *sc = &c->scopes[s];
      for (int i = 0; i < sc->nlocals; i++) {
        LocalVar *lv = &sc->locals[i];
        if (lv->arr_or_nil || lv->type != TY_POLY || lv->is_param || lv->is_block_param) continue;
        if (lv->is_cell || lv->rbs_seeded) continue;
        int saw = 0, ok = 1;
        for (int r = lw_shared_first(c, lv->name, s); r >= 0 && ok; r = lw_shared_next(r)) {
          int id = lw_shared_node(r);
          if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
          const char *wn = nt_str(nt, id, "name");
          if (!wn || !sp_streq(wn, lv->name) || comp_scope_of(c, id) != sc) continue;
          saw = 1;
          if (!aon_value(c, nt_ref(nt, id, "value"), 0)) ok = 0;
        }
        if (saw && ok) { lv->arr_or_nil = 1; changed = 1; }
      }
    }
    if (!changed) break;
  }
}

/* Mark the int locals that can hold that sentinel, so codegen boxes them as
   nil rather than as INTPTR_MIN. Boxing every int through the nil check costs
   ~8% on optcarrot -- every pixel goes through it -- so the marking is static
   and the hot path keeps the plain box. */
static void mark_nullable_int_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* An --rbs `Integer?` return is the seeded form of the same property the
     rounds below infer, so start the propagation from it. */
  for (int mi = 1; mi < c->nscopes; mi++)
    /* nullable_int is the SCALAR-sentinel property; a nilable string is
       recorded by ret_rbs_nilable alone (its nil is NULL, not a reserved
       number), so it must not seed this flag. */
    if (c->scopes[mi].ret_rbs_nilable &&
        (c->scopes[mi].ret == TY_INT || c->scopes[mi].ret == TY_FLOAT))
      c->scopes[mi].ret_nullable_int = 1;
  /* Method returns propagate through this fixpoint too (a pass-through method
     is nilable because its callee is), so the cap has to clear a chain of
     them rather than the single hop the local marking used to need. Every
     sub-pass below skips what is already marked and only ever sets a flag, so
     `changed` is the real exit and the cap is a runaway backstop: measured over
     the 2636 test + benchmark programs, 2563 settle in one round, 72 in two and
     one in three. A chain deeper than the cap would stop early and silently
     under-mark -- the unsafe direction -- hence the did-not-converge check
     after the loop rather than a silent stop.

     Which is why the cap is derived from the program rather than fixed. A
     round that changes anything sets at least one of these flags and never
     clears one, so the rounds cannot outnumber the flags: methods propagate in
     definition order, so a chain of pass-through methods written in reverse
     advances one link per round, and a fixed cap of 32 refused to compile a
     legal 40-deep one. Past this bound the pass is not monotone, which is the
     only thing the check is here to catch. */
  long rounds_max = c->nscopes + 2;
  for (int s = 0; s < c->nscopes; s++) rounds_max += c->scopes[s].nlocals;
  int converged = 0;
  for (long round = 0; round < rounds_max; round++) {
    int changed = 0;
    /* A method whose own return expression can be the sentinel hands it to
       every caller. Without this, only an --rbs-seeded signature made a method
       nilable, so `def pass(x) = x.p_` silently laundered the sentinel into a
       plain int at the caller (#3505). */
    for (int mi = 1; mi < c->nscopes; mi++) {
      Scope *s = &c->scopes[mi];
      if (s->ret_nullable_int || (s->ret != TY_INT && s->ret != TY_FLOAT)) continue;
      int tail = scope_body_last(c, mi);
      if (tail >= 0 && nullable_int_value(c, tail)) { s->ret_nullable_int = 1; changed = 1; }
    }
    /* an explicit `return e` exits the method just as its tail does. Blocks
       share the enclosing method's scope, so a `return` inside one attributes
       to the method it returns from. */
    NT_FOREACH_KIND(nt, NK_ReturnNode, id) {
      Scope *rs = comp_scope_of(c, id);
      int rmi = rs ? (int)(rs - c->scopes) : -1;
      if (rmi < 1 || rmi >= c->nscopes) continue;
      Scope *s = &c->scopes[rmi];
      if (s->ret_nullable_int || (s->ret != TY_INT && s->ret != TY_FLOAT)) continue;
      if (nullable_int_value(c, id)) { s->ret_nullable_int = 1; changed = 1; }
    }
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
      int v = nt_ref(nt, id, "value");
      const char *ln = nt_str(nt, id, "name");
      if (v < 0 || !ln) continue;
      Scope *sc = comp_scope_of(c, id);
      LocalVar *lv = sc ? scope_local(sc, ln) : NULL;
      if (!lv || (lv->type != TY_INT && lv->type != TY_FLOAT) || lv->nullable_int) continue;
      /* An outright `i = nil` on a slot the other writes make an int leaves
         the sentinel in it just as a search miss does. */
      if (nullable_int_value(c, v)) { lv->nullable_int = 1; changed = 1; }
    }
    /* A PARAMETER whose DEFAULT is the nil literal carries the sentinel on
       every defaulted call even when each explicit call site passes a real
       number -- the number is what narrowed the slot to sp_int, and the
       call-site propagation below never sees the default. `of(path,
       line = nil)` stored SP_INT_NIL boxed as an Integer: truthy, non-nil?,
       class Integer, while inspect still said nil (#4212). Only the boxing
       has to know, as everywhere in this family. */
    for (int si2 = 1; si2 < c->nscopes; si2++) {
      Scope *sc2 = &c->scopes[si2];
      if (!sc2->pdefault) continue;
      for (int pk2 = 0; pk2 < sc2->nparams; pk2++) {
        int dv2 = sc2->pdefault[pk2];
        if (dv2 < 0 || nt_kind(nt, dv2) != NK_NilNode) continue;
        LocalVar *p2 = sc2->pnames[pk2] ? scope_local(sc2, sc2->pnames[pk2]) : NULL;
        if (!p2 || (p2->type != TY_INT && p2->type != TY_FLOAT) || p2->nullable_int) continue;
        p2->nullable_int = 1; changed = 1;
      }
    }
    /* An int/float ARRAY local holding such a value hands it to every element
       read and to the block parameter of every iteration over it. The array's
       C element type is unchanged: only the boxing has to know (#3505). */
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      LocalVar *lv = nullable_elem_local(c, id, nt_str(nt, id, "name"));
      if (!lv) continue;
      /* the value is the array itself, however it was built: a literal, an
         alias of another marked local, a chain, a method that returns one */
      if (nullable_int_elem_expr(c, v, 0)) { lv->nullable_int_elem = 1; changed = 1; }
    }
    /* the same slot written through an ivar */
    NT_FOREACH_KIND(nt, NK_InstanceVariableWriteNode, id) {
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      ClassInfo *ci = NULL;
      int iv = nullable_elem_ivar(c, id, &ci);
      if (iv < 0 || ci->ivar_nullable_int_elem[iv]) continue;
      if (ci->ivar_types[iv] != TY_INT_ARRAY && ci->ivar_types[iv] != TY_FLOAT_ARRAY) continue;
      if (nullable_int_elem_expr(c, v, 0)) { ci->ivar_nullable_int_elem[iv] = 1; changed = 1; }
    }
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "<<") && !sp_streq(nm, "push") && !sp_streq(nm, "unshift"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0) continue;
      int ca = nt_ref(nt, id, "arguments"); int an = 0;
      const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
      int nilable = 0;
      for (int k = 0; av && k < an && !nilable; k++) nilable = nullable_int_value(c, av[k]);
      if (!nilable) continue;
      if (nt_kind(nt, recv) == NK_LocalVariableReadNode) {
        LocalVar *lv = nullable_elem_local(c, recv, nt_str(nt, recv, "name"));
        if (lv) { lv->nullable_int_elem = 1; changed = 1; }
      }
      else if (nt_kind(nt, recv) == NK_InstanceVariableReadNode) {
        ClassInfo *ci = NULL;
        int iv = nullable_elem_ivar(c, recv, &ci);
        if (iv >= 0 && !ci->ivar_nullable_int_elem[iv] &&
            (ci->ivar_types[iv] == TY_INT_ARRAY || ci->ivar_types[iv] == TY_FLOAT_ARRAY)) {
          ci->ivar_nullable_int_elem[iv] = 1; changed = 1;
        }
      }
    }
    /* `ks.each { |k| h[k] = ... }`: the block parameter IS the element. */
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      int recv = nt_ref(nt, id, "receiver"), blk = nt_ref(nt, id, "block");
      if (recv < 0 || blk < 0) continue;
      if (!nullable_int_elem_expr(c, recv, 0)) continue;
      int bp = nt_ref(nt, blk, "parameters");
      int params = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
      int rn = 0; const int *reqs = params >= 0 ? nt_arr(nt, params, "requireds", &rn) : NULL;
      /* |v| is the element; `reduce`/`inject` bind BOTH parameters to one.
         Anything else with two (`|k, i|`, `|k, v|`) has a non-element slot. */
      const char *rnm2 = nt_str(nt, id, "name");
      int fold = rnm2 && (sp_streq(rnm2, "reduce") || sp_streq(rnm2, "inject"));
      if (!reqs || rn < 1 || (rn != 1 && !(fold && rn == 2))) continue;
      Scope *bsc = comp_scope_of(c, blk);
      for (int pk = 0; pk < rn; pk++) {
        const char *pnm = nt_str(nt, reqs[pk], "name");
        LocalVar *plv = pnm && bsc ? scope_local(bsc, pnm) : NULL;
        if (!plv || (plv->type != TY_INT && plv->type != TY_FLOAT) || plv->nullable_int) continue;
        plv->nullable_int = 1; changed = 1;
      }
    }
    /* An IVAR written from such a value hands the sentinel to every reader,
       including the attr_reader a caller goes through (`W.new(r.p_).v`). The
       slot keeps its scalar C type, so only the boxing has to know (#3505). */
    NT_FOREACH_KIND(nt, NK_InstanceVariableWriteNode, id) {
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      Scope *s = comp_scope_of(c, id);
      int cid = s ? s->class_id : -1;
      if (cid < 0) cid = comp_class_index(c, "Toplevel");
      if (cid < 0 || cid >= c->nclasses) continue;
      ClassInfo *ci = &c->classes[cid];
      int iv = comp_ivar_index(ci, nt_str(nt, id, "name"));
      if (iv < 0 || ci->ivar_nullable_int[iv]) continue;
      if (ci->ivar_types[iv] != TY_INT && ci->ivar_types[iv] != TY_FLOAT) continue;
      if (nullable_int_value(c, v)) { ci->ivar_nullable_int[iv] = 1; changed = 1; }
    }
    /* A PARAMETER bound from such a value carries the sentinel into the callee,
       where boxing it (`other.inspect`, `x == other`) has the same problem the
       local marking exists to prevent. */
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      int mi = comp_method_index(c, nt_str(nt, id, "name"));
      if (mi < 0) {
        int recv = nt_ref(nt, id, "receiver");
        TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
        if (recv < 0) {
          Scope *self = comp_scope_of(c, id);
          if (self && self->class_id >= 0)
            mi = comp_method_in_chain(c, self->class_id, nt_str(nt, id, "name"), NULL);
        }
        else if (ty_is_object(rt))
          mi = comp_method_in_chain(c, ty_object_class(rt), nt_str(nt, id, "name"), NULL);
        /* `W.new(k)` binds initialize's parameters, and `W.build(k)` a class
           method's: neither receiver is an instance, so the arm above cannot
           see them and the sentinel stopped at the constructor (#3505). */
        else if (nt_kind(nt, recv) == NK_ConstantReadNode) {
          const char *cnm = nt_str(nt, id, "name");
          int rci = comp_class_index(c, nt_str(nt, recv, "name"));
          if (rci >= 0)
            mi = sp_streq(cnm, "new") ? comp_method_in_chain(c, rci, "initialize", NULL)
                                      : comp_cmethod_in_chain(c, rci, cnm, NULL);
        }
      }
      if (mi < 0) continue;
      Scope *m = &c->scopes[mi];
      int ca = nt_ref(nt, id, "arguments");
      int an = 0; const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &an) : NULL;
      for (int k = 0; av && k < an && k < m->nparams; k++) {
        if (m->rest_idx >= 0 && k >= m->rest_idx) break;
        LocalVar *p = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
        if (!p) continue;
        /* an object parameter handed nil, or handed one that was (#5088) */
        if (ty_is_object(p->type) && !p->obj_nilable) {
          int nilarg = nt_kind(nt, av[k]) == NK_NilNode;
          if (!nilarg && nt_kind(nt, av[k]) == NK_LocalVariableReadNode) {
            Scope *as = comp_scope_of(c, av[k]);
            const char *an2 = nt_str(nt, av[k], "name");
            LocalVar *al = as && an2 ? scope_local(as, an2) : NULL;
            nilarg = al && al->is_param && al->obj_nilable;
          }
          if (nilarg) { p->obj_nilable = 1; changed = 1; }
        }
        if ((p->type == TY_INT_ARRAY || p->type == TY_FLOAT_ARRAY) && !p->nullable_int_elem &&
            nullable_int_elem_expr(c, av[k], 0)) { p->nullable_int_elem = 1; changed = 1; }
        if ((p->type != TY_INT && p->type != TY_FLOAT) || p->nullable_int) continue;
        if (nullable_int_value(c, av[k])) { p->nullable_int = 1; changed = 1; continue; }
        /* an ivar that can be read before anything assigned it, or a parameter
           already carrying one: boxing the parameter has to answer nil (#5085) */
        if (!p->box_nullable && box_nullable_arg(c, av[k])) { p->box_nullable = 1; changed = 1; }
      }
    }
    /* A destructuring target the right side cannot supply gets nil, through a
       target node rather than a write node of its own -- `a, b, *c, d, e = 1`
       leaves the sentinel in every int slot after the first. */
    NT_FOREACH_KIND(nt, NK_MultiWriteNode, id) {
      int v = nt_ref(nt, id, "value");
      /* how many values the right side statically supplies: an array literal
         supplies its elements, a scalar supplies exactly one, and anything
         else (an array-valued call or local) is only known at runtime. */
      int supply = 0;
      if (v >= 0 && nt_kind(nt, v) == NK_ArrayNode) nt_arr(nt, v, "elements", &supply);
      else if (v >= 0 && nt_kind(nt, v) != NK_NilNode && !ty_is_array(infer_type(c, v))) supply = 1;
      int tn2 = 0; const int *tv2 = nt_arr(nt, id, "lefts", &tn2);
      for (int k = 0; tv2 && k < tn2; k++) {
        if (k < supply || nt_kind(nt, tv2[k]) != NK_LocalVariableTargetNode) continue;
        const char *tn = nt_str(nt, tv2[k], "name");
        Scope *ts = tn ? comp_scope_of(c, tv2[k]) : NULL;
        LocalVar *tl = ts ? scope_local(ts, tn) : NULL;
        if (tl && (tl->type == TY_INT || tl->type == TY_FLOAT) && !tl->nullable_int) { tl->nullable_int = 1; changed = 1; }
      }
      /* a target after the splat is supplied only when the right side is long
         enough, which a static count almost never proves */
      int rn2 = 0; const int *rv2 = nt_arr(nt, id, "rights", &rn2);
      for (int k = 0; rv2 && k < rn2; k++) {
        if (nt_kind(nt, rv2[k]) != NK_LocalVariableTargetNode) continue;
        const char *tn = nt_str(nt, rv2[k], "name");
        Scope *ts = tn ? comp_scope_of(c, rv2[k]) : NULL;
        LocalVar *tl = ts ? scope_local(ts, tn) : NULL;
        if (tl && (tl->type == TY_INT || tl->type == TY_FLOAT) && !tl->nullable_int) { tl->nullable_int = 1; changed = 1; }
      }
    }
    if (!changed) { converged = 1; break; }
  }
  /* Running out of rounds means some slot that CAN hold the sentinel is still
     unmarked, and codegen would box it as an ordinary number -- a Hash key no
     literal nil matches, with no error at compile or run time (#3505). That is
     the exact silent-wrong-output this pass exists to prevent, so refuse to
     emit rather than emit something quietly wrong. Unreachable for a monotone
     predicate whatever the program (the bound counts the flags themselves), so
     this firing means a marking arm that is not monotone -- worth a bug
     report. */
  if (!converged) {
    fprintf(stderr, "spinel: internal: nilable-scalar marking did not converge in "
                    "%ld rounds; refusing to emit (a nil sentinel would box as an "
                    "ordinary number). Please report this with the source.\n",
            rounds_max);
    exit(1);
  }
}

/* `f(*a)` where the splatted array's length is known statically expands to
   `f(a[0], a[1], ...)`, whatever the target. A user-defined method already
   handles the unexpanded form -- codegen reads the array into its declared
   params -- but a builtin's arity is its C function's, so the array arrived as
   a single argument: `h.fetch(*k)` either failed to build or silently ran the
   one-argument overload and answered the wrong thing (#3515, #3516).

   Runs after walk_scope, because deciding whether a local's length is static
   means asking which writes are in ITS scope, and the synthesized nodes need a
   scope of their own for inference to read the right slot. */
static int splat_lit_len(Compiler *c, int ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *t = nt_type(nt, ex);
  if (!t || !sp_streq(t, "ArrayNode")) return -1;
  int n = 0; const int *el = nt_arr(nt, ex, "elements", &n);
  if (!el) return -1;
  for (int i = 0; i < n; i++) {
    const char *et = nt_type(nt, el[i]);
    if (et && (sp_streq(et, "SplatNode") || sp_streq(et, "KeywordHashNode") ||
               sp_streq(et, "AssocSplatNode"))) return -1;
  }
  return n;
}
/* A local qualifies when its scope assigns it an array literal exactly once
   and never touches it again: no second write, no operator write, no use as a
   receiver (any call on it could be a push, which changes the length). */
static int splat_local_len(Compiler *c, int ex, int callid) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *nm = nt_str(nt, ex, "name");
  if (!nm || callid >= c->node_cap) return -1;
  int sc = c->nscope[callid];
  int len = -1, writes = 0;
  for (int id = 0; id < nt->count && id < c->node_cap; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || c->nscope[id] != sc) continue;
    if (sp_streq(ty, "LocalVariableWriteNode")) {
      const char *wn = nt_str(nt, id, "name");
      if (!wn || !sp_streq(wn, nm)) continue;
      if (++writes > 1) return -1;
      int v = nt_ref(nt, id, "value");
      len = v >= 0 ? splat_lit_len(c, v) : -1;
      if (len < 0) return -1;
      continue;
    }
    if (sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
        sp_streq(ty, "LocalVariableAndWriteNode") ||
        sp_streq(ty, "LocalVariableOperatorWriteNode")) {
      const char *wn = nt_str(nt, id, "name");
      if (wn && sp_streq(wn, nm)) return -1;
    }
    if (sp_streq(ty, "CallNode")) {
      int r = nt_ref(nt, id, "receiver");
      if (r < 0) continue;
      const char *rt = nt_type(nt, r);
      if (!rt || !sp_streq(rt, "LocalVariableReadNode")) continue;
      const char *rn = nt_str(nt, r, "name");
      if (!rn || !sp_streq(rn, nm)) continue;
      /* `a[i]` cannot change the length, and excluding it matters beyond
         tidiness: expanding one call synthesizes exactly that shape, so
         without this the first expansion disqualifies every later one in the
         same scope. */
      const char *cn = nt_str(nt, id, "name");
      if (cn && sp_streq(cn, "[]")) continue;
      return -1;
    }
  }
  return writes == 1 ? len : -1;
}
/* How many arguments the builtin requires, for a splat whose length only the
   run time knows. Only the required count: an optional trailing parameter
   (`fetch`'s default, `split`'s limit) cannot be chosen without knowing the
   length, and reading one that the array may not carry would pass nil where
   Ruby passes nothing. -1 for a name with no single answer. */
static int splat_builtin_arity(const char *name) {
  static const struct { const char *name; int arity; } tab[] = {
    /* slice is NOT here: it is the one name in this list whose arity depends
       on the receiver -- Hash#slice is variadic, Array#slice and String#slice
       take one argument or two -- so no single number is right. Expanding to
       1 dropped every Hash key after the first and dropped Array/String's
       length argument (#4164). A slice splat is expanded only where the
       length is statically known, and left alone otherwise. */
    { "fetch", 1 }, { "store", 2 }, { "insert", 2 },
    { "delete", 1 }, { "sub", 2 }, { "sub!", 2 }, { "gsub", 2 },
    { "gsub!", 2 }, { "[]", 1 }, { "[]=", 2 },
    { "key?", 1 }, { "has_key?", 1 }, { "include?", 1 }, { "member?", 1 },
    { "value?", 1 }, { "has_value?", 1 }, { "index", 1 }, { "rindex", 1 },
    { "count", 1 }, { "split", 1 }, { "join", 1 },
    { "start_with?", 1 }, { "end_with?", 1 }, { "tr", 2 }, { "tr_s", 2 },
    { NULL, 0 }
  };
  for (int i = 0; tab[i].name; i++)
    if (sp_streq(name, tab[i].name)) return tab[i].arity;
  return -1;
}
/* Whether some user method of this name could not take `n` positional
   arguments, which is what makes expanding unsafe: the call might be that
   method's, and then the elements would not fill its parameters. A negative
   `n` means the count is unknown, so any definition of the name rejects. */
static int splat_user_method_rejects(Compiler *c, const char *name, int n) {
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!s->name || !sp_streq(s->name, name)) continue;
    if (n < 0) return 1;
    if (s->rest_idx >= 0) continue;                    /* takes any count */
    if (n < s->nrequired || n > s->nparams) return 1;
  }
  return 0;
}
/* A literal receiver is the builtin's own, whatever user classes share the
   method name. */
static int splat_recv_is_builtin_literal(NodeTable *nt, int id) {
  int r = nt_ref(nt, id, "receiver");
  const char *t = r >= 0 ? nt_type(nt, r) : NULL;
  if (!t) return 0;
  return sp_streq(t, "StringNode") || sp_streq(t, "InterpolatedStringNode") ||
         sp_streq(t, "ArrayNode") || sp_streq(t, "HashNode") ||
         sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
         sp_streq(t, "SymbolNode") || sp_streq(t, "RangeNode");
}
static void expand_static_splat_args(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int count = nt->count;
  for (int id = 0; id < count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int an = nt_ref(nt, id, "arguments");
    if (an < 0) continue;
    int argc = 0; const int *argv0 = nt_arr(nt, an, "arguments", &argc);
    if (!argv0 || argc < 1 || argc > 24) continue;
    /* copied out: synthesizing a node can realloc the table and leave the
       pointer nt_arr returned dangling */
    int argv[24];
    for (int k = 0; k < argc; k++) argv[k] = argv0[k];
    int sp_at = -1;
    for (int k = 0; k < argc; k++) {
      const char *at = nt_type(nt, argv[k]);
      if (at && sp_streq(at, "SplatNode")) { if (sp_at >= 0) { sp_at = -1; break; } sp_at = k; }
    }
    if (sp_at < 0) continue;
    /* Only the fixed-arity builtins, which is the whole population that needs
       this: a user-defined method reads the array into its declared params
       (and reports the arity error the expansion would hide), and the variadic
       builtins take the array correctly as it stands -- `puts(*a)`,
       `format(*args)`, `Struct.new(*syms)` all rely on that, and expanding
       them types each element separately or loses the compile-time member
       names. */
    static const char *fixed_arity_builtins[] = {
      "fetch", "store", "insert", "slice", "fill", "delete",
      "sub", "sub!", "gsub", "gsub!", "[]", "[]=",
      /* the predicate and search families: each takes its argument as a scalar
         in C, so an unexpanded array reached the slot as a pointer and the
         call answered from a comparison against garbage */
      "key?", "has_key?", "include?", "member?", "value?", "has_value?",
      "index", "rindex", "count", "split", "join",
      "start_with?", "end_with?", "tr", "tr_s", NULL
    };
    const char *cnm = nt_str(nt, id, "name");
    if (!cnm) continue;
    int listed = 0;
    for (int j = 0; fixed_arity_builtins[j]; j++)
      if (sp_streq(cnm, fixed_arity_builtins[j])) { listed = 1; break; }
    if (!listed) continue;
    /* ...but the name has to BE the builtin. A receiverless call to a
       top-level `def count(*args)` is the user's own variadic method, and
       expanding its splat to the builtin's arity handed it one element where
       the program passed none: `count(*[])` answered 1 (#4298). A call with a
       receiver still reaches the builtin, so only the bare form declines. */
    if (nt_ref(nt, id, "receiver") < 0 && comp_method_index(c, cnm) >= 0) continue;
    int ex = nt_ref(nt, argv[sp_at], "expression");
    if (ex < 0) continue;
    const char *ext = nt_type(nt, ex);
    if (!ext) continue;
    int is_lit = sp_streq(ext, "ArrayNode");
    int lit_n = 0, lit_el[24];
    int n;
    if (is_lit) {
      n = splat_lit_len(c, ex);
      if (n < 0 || n > 24) continue;
      const int *el0 = nt_arr(nt, ex, "elements", &lit_n);
      if (!el0 || lit_n != n) continue;
      for (int i = 0; i < n; i++) lit_el[i] = el0[i];
    }
    else if (sp_streq(ext, "LocalVariableReadNode")) n = splat_local_len(c, ex, id);
    else continue;
    if (n < 0) {
      /* The length is not static -- the usual case being a splat forwarded
         from a parameter, `def f(keys); h.fetch(*keys); end`. The builtin's
         own arity still says how many elements to read, so expand to that
         many `a[i]`: the array reaching a scalar slot is what breaks, not the
         count. Reading past the end yields nil rather than Ruby's
         ArgumentError, and an element beyond the required arity is not
         passed on. */
      /* A block moves the required count (`sub(pat) { .. }` takes one
         argument, not two), so leave those alone. */
      n = nt_ref(nt, id, "block") >= 0 ? -1 : splat_builtin_arity(cnm);
      /* slice has no arity to expand to on purpose (see the table). Leave the
         splat as it stands rather than refusing the program: Hash#slice's
         emitter iterates it, which is what the call means. */
      if (n < 0 && sp_streq(cnm, "slice")) continue;
      if (n < 0 && splat_user_method_rejects(c, cnm, n)) continue;
      if (n < 0) {
        /* No arity to expand to (a name we list for its literal-splat form
           only). Say so here rather than letting the C compiler report it
           against generated code the user never wrote. (Variadic builtins
           never get here -- they take the array as it stands and were
           filtered out above.) */
        unsupported_feature(c, id, "splat whose length is not known at compile time, "
                                   "into a fixed-arity builtin");
      }
    }
    if (n > 8) continue;                 /* keep the synthesized list small */
    /* A user method of the same name can own the call, and these names are
       ordinary ones to define (`index`, `count`, `include?`). Expanding is
       still right when that method takes exactly this many positional
       arguments, since the elements land on the same parameters either way;
       when it cannot, leave the call alone -- codegen reads the array into
       the declared params -- unless the receiver is a builtin literal, which
       no user method is reachable from. */
    if (splat_user_method_rejects(c, cnm, n) && !splat_recv_is_builtin_literal(nt, id)) continue;
    const char *lnm = is_lit ? NULL : nt_str(nt, ex, "name");
    if (!is_lit && !lnm) continue;
    int nargs[32];
    int m = 0;
    for (int k = 0; k < sp_at; k++) nargs[m++] = argv[k];
    for (int i = 0; i < n; i++) {
      if (is_lit) { nargs[m++] = lit_el[i]; continue; }
      /* a fresh read per element: the original node is shared by the write
         detection above, and each synthesized node needs its own scope entry */
      int rd = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, rd, "name", lnm);
      int ix = nt_new_node(nt, "IntegerNode");
      nt_node_set_int(nt, ix, "value", i);
      int ia = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, ia, "arguments", &ix, 1);
      int cl = nt_new_node(nt, "CallNode");
      nt_node_set_str(nt, cl, "name", "[]");
      nt_node_set_ref(nt, cl, "receiver", rd);
      nt_node_set_ref(nt, cl, "arguments", ia);
      comp_grow_node_arrays(c);
      c->nscope[rd] = c->nscope[id];
      c->nscope[ix] = c->nscope[id];
      c->nscope[ia] = c->nscope[id];
      c->nscope[cl] = c->nscope[id];
      nargs[m++] = cl;
    }
    for (int k = sp_at + 1; k < argc; k++) nargs[m++] = argv[k];
    /* a FRESH ArgumentsNode: the existing one's id array came from the parser
       and is not ours to free, which nt_node_set_arr on it would try to do */
    int na = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, na, "arguments", nargs, m);
    nt_node_set_ref(nt, id, "arguments", na);
    comp_grow_node_arrays(c);
    c->nscope[na] = c->nscope[id];
  }
}

/* Whether a synthesized `__bam_N` wrapper binds a RECEIVER in its first
   parameter (`def __bam_N(__bam_r, ...) = __bam_r.sym(...)`), as opposed to
   the receiverless Kernel wrapper (`def __bam_N(__bam_r, ...) = Integer(__bam_r, ...)`)
   whose first parameter is an ordinary argument. */
static int bam_wrapper_binds_receiver(Compiler *c, Scope *sc) {
  const NodeTable *nt = c->nt;
  if (!sc->name || strncmp(sc->name, "__bam_", 6) != 0 || sc->body < 0) return 0;
  int n = 0; const int *st = nt_arr(nt, sc->body, "body", &n);
  if (n != 1 || !nt_type(nt, st[0]) || !sp_streq(nt_type(nt, st[0]), "CallNode")) return 0;
  int recv = nt_ref(nt, st[0], "receiver");
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv), *rnm = nt_str(nt, recv, "name");
  return rty && sp_streq(rty, "LocalVariableReadNode") && rnm && sp_streq(rnm, "__bam_r");
}


/* A top-level method redefined by a later top-level `def` is a different
   method until that def runs: `def tw(&b)`, calls, then `def tw(a)` and more
   calls reach the first body from the first calls and the second from the
   rest. Spinel has one function per name, so the earlier definitions are
   given a private name each, and the top-level calls that run between one
   definition and the next -- statements of the program body and the blocks
   in them, not method bodies (which run whenever they are called, by then
   usually after the last def) and not class or module bodies -- are renamed
   to match. The last definition keeps the name, and every other call
   reaches it. Before this every call bound to the FIRST definition: a later
   def of another arity refused its calls, one of the same arity was a C
   redefinition. */
static int g_redef_self_too;   /* also rename `self.from` (main's singleton methods) */
static void redef_rename_calls(NodeTable *nt, int id, const char *from, const char *to, int depth) {
  if (id < 0 || id >= nt->count || depth > 400) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode)
    return;
  int crecv = k == NK_CallNode ? nt_ref(nt, id, "receiver") : -1;
  if (k == NK_CallNode && (crecv < 0 || (g_redef_self_too && nt_kind(nt, crecv) == NK_SelfNode))) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, from)) {
      if (!sp_streq(from, to)) nt_set_str(nt, id, "name", to);
      /* `self.k` on main is the singleton method, which is a receiverless
         top-level function here: drop the self (see rename_main_singleton_defs) */
      if (crecv >= 0) nt_node_set_ref(nt, id, "receiver", -1);
    }
  }
  const SpNode *nd = &nt->nodes[id];
  for (int i = 0; i < nd->nr; i++) redef_rename_calls(nt, nd->r[i].ref, from, to, depth + 1);
  for (int i = 0; i < nd->na; i++)
    for (int j = 0; j < nd->a[i].n; j++)
      redef_rename_calls(nt, nd->a[i].ids[j], from, to, depth + 1);
}
static void rename_redefined_toplevel_defs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = nt_ref(nt, nt->root_id, "statements");
  int n = 0;
  const int *st0 = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  if (!st0 || n < 2) return;
  int *st = malloc(sizeof(int) * (size_t)n);   /* renaming may move the array */
  if (!st) return;
  memcpy(st, st0, sizeof(int) * (size_t)n);
  int serial = 0;
  for (int i = 0; i < n; i++) {
    if (nt_kind(nt, st[i]) != NK_DefNode || nt_ref(nt, st[i], "receiver") >= 0) continue;
    const char *nm0 = nt_str(nt, st[i], "name");
    if (!nm0) continue;
    int next = -1;
    for (int j = i + 1; j < n && next < 0; j++)
      if (nt_kind(nt, st[j]) == NK_DefNode && nt_ref(nt, st[j], "receiver") < 0 &&
          nt_str(nt, st[j], "name") && sp_streq(nt_str(nt, st[j], "name"), nm0)) next = j;
    if (next < 0) continue;
    char *nm = strdup(nm0);
    char to[256];
    /* a name the program does not already define (`def f__redef1` is legal) */
    for (;;) {
      snprintf(to, sizeof to, "%s__redef%d", nm, ++serial);
      int taken = 0;
      NT_FOREACH_KIND(nt, NK_DefNode, d) {
        const char *dn = nt_str(nt, d, "name");
        if (dn && sp_streq(dn, to)) { taken = 1; break; }
      }
      if (!taken) break;
    }
    nt_set_str(nt, st[i], "name", to);
    for (int j = i + 1; j < next; j++) {
      redef_rename_calls(nt, st[j], nm, to, 0);
      /* `alias saved f` between the two names the body in effect there */
      if (nt_kind(nt, st[j]) == NK_AliasMethodNode) {
        int on = nt_ref(nt, st[j], "old_name");
        const char *ov = on >= 0 ? nt_str(nt, on, "value") : NULL;
        if (ov && sp_streq(ov, nm)) nt_set_str(nt, on, "value", to);
      }
    }
    /* The earlier body's own calls: it is entered only from the renamed
       calls, all of which run before the later `def`, so while it runs the
       name is still this definition -- a recursive `fact(n - 1)` is itself,
       not the redefinition (CodeRabbit on #4972). Parameter defaults run on
       entry, the same. */
    redef_rename_calls(nt, nt_ref(nt, st[i], "body"), nm, to, 0);
    redef_rename_calls(nt, nt_ref(nt, st[i], "parameters"), nm, to, 0);
    free(nm);
  }
  free(st);
}

/* Can evaluating `e` do anything observable? A read of a local, an ivar or a
   constant, a literal, self, `&:sym`, and a lambda or `proc { }` literal
   cannot; anything else (a call, above all) may. */
static int bo_may_act(const NodeTable *nt, int e) {
  if (e < 0) return 0;
  switch (nt_kind(nt, e)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
  case NK_ConstantReadNode: case NK_SelfNode: case NK_NilNode: case NK_TrueNode:
  case NK_FalseNode: case NK_IntegerNode: case NK_FloatNode: case NK_SymbolNode:
  case NK_StringNode: case NK_LambdaNode:
    return 0;
  /* a literal container of such values, and parentheses around one */
  case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode: {
    int en = 0; const int *el = nt_arr(nt, e, "elements", &en);
    for (int k = 0; k < en; k++) if (bo_may_act(nt, el[k])) return 1;
    return 0;
  }
  case NK_AssocNode:
    return bo_may_act(nt, nt_ref(nt, e, "key")) || bo_may_act(nt, nt_ref(nt, e, "value"));
  case NK_RangeNode:
    return bo_may_act(nt, nt_ref(nt, e, "left")) || bo_may_act(nt, nt_ref(nt, e, "right"));
  case NK_ParenthesesNode: case NK_StatementsNode: {
    int b = nt_kind(nt, e) == NK_ParenthesesNode ? nt_ref(nt, e, "body") : e;
    if (b < 0) return 0;
    if (b != e) return bo_may_act(nt, b);
    int bn = 0; const int *bb = nt_arr(nt, b, "body", &bn);
    for (int k = 0; k < bn; k++) if (bo_may_act(nt, bb[k])) return 1;
    return 0;
  }
  case NK_CallNode: {
    const char *nm = nt_str(nt, e, "name");
    int blk = nt_ref(nt, e, "block");
    if (nt_ref(nt, e, "receiver") < 0 && nm && (sp_streq(nm, "proc") || sp_streq(nm, "lambda")) &&
        blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && nt_ref(nt, e, "arguments") < 0)
      return 0;
    /* `method(:m)` / `obj.method(:m)` only builds the Method object, and the
       block-argument emitters lower that spelling to the method itself */
    if (nm && sp_streq(nm, "method") && blk < 0 && !bo_may_act(nt, nt_ref(nt, e, "receiver"))) {
      int ma = nt_ref(nt, e, "arguments"), mn = 0;
      const int *mv = ma >= 0 ? nt_arr(nt, ma, "arguments", &mn) : NULL;
      if (mn == 1 && nt_kind(nt, mv[0]) == NK_SymbolNode) return 0;
    }
    return 1;
  }
  default:
    return 1;
  }
}
/* A copy of call `id`'s fields into a fresh CallNode (the children are shared,
   not cloned). */
static int bo_shallow_copy(NodeTable *nt, int id) {
  int nc = nt_new_node(nt, "CallNode");
  if (nc < 0) return -1;
  const SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->ns; j++) nt_node_set_str(nt, nc, nd->s[j].key, nd->s[j].val);
  nd = &nt->nodes[id];
  for (int j = 0; j < nd->ni; j++) nt_node_set_int(nt, nc, nd->i[j].key, nd->i[j].val);
  nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) nt_node_set_ref(nt, nc, nd->r[j].key, nd->r[j].ref);
  nd = &nt->nodes[id];
  for (int j = 0; j < nd->na; j++) nt_node_set_arr(nt, nc, nd->a[j].key, nd->a[j].ids, nd->a[j].n);
  return nc;
}
/* `recv.m(args, &expr)`: Ruby evaluates the receiver, then the arguments, then
   the block expression. The emitted C passes the block as one more function
   argument (or hoists it ahead of a Class-value dispatch, #4992), and C leaves
   the order of function arguments to the compiler: `run(say(1), &blk(1))`
   printed the block's side effect first. When the block expression can act
   and something ahead of it can too, the call becomes
     (__bo_N_0 = recv; __bo_N_1 = arg; ...; __bo_N_b = expr; __bo_N_0.m(__bo_N_1, ..., &__bo_N_b))
   so the order is the statements'. The node itself becomes the parentheses,
   keeping its id, line and scope. */
static void desugar_block_arg_order(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    int be = nt_ref(nt, blk, "expression");
    if (!bo_may_act(nt, be)) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av0 = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    int ahead = bo_may_act(nt, recv), plain = 1;
    for (int k = 0; k < an; k++) {
      NodeKind ak = nt_kind(nt, av0[k]);
      if (ak == NK_BlockArgumentNode || (nt_type(nt, av0[k]) && sp_streq(nt_type(nt, av0[k]), "ForwardingArgumentsNode"))) plain = 0;
      else if (ak == NK_SplatNode) ahead |= bo_may_act(nt, nt_ref(nt, av0[k], "expression"));
      else if (ak == NK_KeywordHashNode) {
        int en = 0; const int *el = nt_arr(nt, av0[k], "elements", &en);
        for (int e = 0; e < en; e++) {
          if (nt_kind(nt, el[e]) != NK_AssocNode) { plain = 0; break; }
          ahead |= bo_may_act(nt, nt_ref(nt, el[e], "key")) | bo_may_act(nt, nt_ref(nt, el[e], "value"));
        }
      }
      else ahead |= bo_may_act(nt, av0[k]);
    }
    if (!plain || !ahead) continue;
    Scope *sc = comp_scope_of(c, id);
    if (!sc) continue;
    int *av = an > 0 ? malloc(sizeof(int) * (size_t)an) : NULL;
    if (an > 0 && !av) continue;
    for (int k = 0; k < an; k++) av[k] = av0[k];
    int first = nt->count;
    int stm[256]; int ns = 0, serial = 0;
    char tn[64];
    /* one `__bo_<id>_<k> = e` statement; answers the read that replaces e */
    #define BO_HOIST(E) ({ int _e = (E), _r = _e; if (ns < 255 && bo_may_act(nt, _e)) { \
        snprintf(tn, sizeof tn, "__bo_%d_%d", id, serial++); \
        int _w = nt_new_node(nt, "LocalVariableWriteNode"); \
        int _rd = nt_new_node(nt, "LocalVariableReadNode"); \
        nt_node_set_str(nt, _w, "name", tn); nt_node_set_ref(nt, _w, "value", _e); \
        nt_node_set_str(nt, _rd, "name", tn); \
        stm[ns++] = _w; scope_local_intern(sc, tn); _r = _rd; } _r; })
    int nrecv = BO_HOIST(recv);
    for (int k = 0; k < an; k++) {
      NodeKind ak = nt_kind(nt, av[k]);
      if (ak == NK_SplatNode) {
        int ex = nt_ref(nt, av[k], "expression");
        int nex = BO_HOIST(ex);
        if (nex != ex) nt_node_set_ref(nt, av[k], "expression", nex);
      }
      else if (ak == NK_KeywordHashNode) {
        int en = 0; const int *el0 = nt_arr(nt, av[k], "elements", &en);
        int *el = malloc(sizeof(int) * (size_t)(en > 0 ? en : 1));
        for (int e = 0; e < en; e++) el[e] = el0[e];
        for (int e = 0; e < en; e++) {
          int key = nt_ref(nt, el[e], "key"), val = nt_ref(nt, el[e], "value");
          int nk = BO_HOIST(key); if (nk != key) nt_node_set_ref(nt, el[e], "key", nk);
          int nv = BO_HOIST(val); if (nv != val) nt_node_set_ref(nt, el[e], "value", nv);
        }
        free(el);
      }
      else av[k] = BO_HOIST(av[k]);
    }
    int nbe = BO_HOIST(be);
    #undef BO_HOIST
    if (ns == 0 || ns >= 255) { free(av); continue; }
    nt_node_set_ref(nt, blk, "expression", nbe);
    if (args >= 0) nt_node_set_arr(nt, args, "arguments", av, an);
    free(av);
    int nc = bo_shallow_copy(nt, id);
    if (nc < 0) continue;
    if (recv >= 0) nt_node_set_ref(nt, nc, "receiver", nrecv);
    stm[ns++] = nc;
    int stmts = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, stmts, "body", stm, ns);
    /* the node keeps its id, line and file: the parentheses */
    long long line = nt_int(nt, id, "node_line", 0), file = nt_int(nt, id, "node_file", 0),
              col = nt_int(nt, id, "node_col", 0);
    nt_node_reset(nt, id, "ParenthesesNode");
    nt_node_set_int(nt, id, "node_line", line);
    nt_node_set_int(nt, id, "node_file", file);
    nt_node_set_int(nt, id, "node_col", col);
    nt_node_set_ref(nt, id, "body", stmts);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = first; j < nt->count; j++) c->nscope[j] = encl;
  }
}

/* A top-level `def self.k` is a singleton method of main, and a top-level
   `def k` a private method of Object: both can exist, and on main -- the
   top-level statements, their blocks, and the singleton method's own body --
   a call of `k` or `self.k` reaches the singleton, while from any other object
   `k` is Object's. Both were emitted as sp_k and the C did not compile. The
   singleton gets a private name, and the calls that run with main as self
   after its `def` are renamed to it; a method body keeps `k`, since it runs
   with whatever self it was called on. */
static void rename_main_singleton_defs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = nt_ref(nt, nt->root_id, "statements");
  int n = 0;
  const int *st0 = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  if (!st0 || n < 2) return;
  int *st = malloc(sizeof(int) * (size_t)n);
  if (!st) return;
  memcpy(st, st0, sizeof(int) * (size_t)n);
  int serial = 0;
  for (int i = 0; i < n; i++) {
    if (nt_kind(nt, st[i]) != NK_DefNode) continue;
    int rv = nt_ref(nt, st[i], "receiver");
    if (rv < 0 || nt_kind(nt, rv) != NK_SelfNode) continue;
    const char *nm0 = nt_str(nt, st[i], "name");
    if (!nm0) continue;
    int plain = 0;
    for (int j = 0; j < n && !plain; j++)
      if (nt_kind(nt, st[j]) == NK_DefNode && nt_ref(nt, st[j], "receiver") < 0 &&
          nt_str(nt, st[j], "name") && sp_streq(nt_str(nt, st[j], "name"), nm0)) plain = 1;
    char *nm = strdup(nm0);
    char to[256];
    /* alone, the singleton keeps its name and only `self.k` loses its self:
       the dispatch on main had no arm for main's own singleton methods, and
       `self.k` raised NoMethodError */
    if (!plain) snprintf(to, sizeof to, "%s", nm);
    else for (;;) {
      snprintf(to, sizeof to, "%s__main%d", nm, ++serial);
      int taken = 0;
      NT_FOREACH_KIND(nt, NK_DefNode, d) {
        const char *dn = nt_str(nt, d, "name");
        if (dn && sp_streq(dn, to)) { taken = 1; break; }
      }
      if (!taken) break;
    }
    if (plain) nt_set_str(nt, st[i], "name", to);
    g_redef_self_too = 1;
    for (int j = i + 1; j < n; j++) redef_rename_calls(nt, st[j], nm, to, 0);
    redef_rename_calls(nt, nt_ref(nt, st[i], "body"), nm, to, 0);
    redef_rename_calls(nt, nt_ref(nt, st[i], "parameters"), nm, to, 0);
    g_redef_self_too = 0;
    free(nm);
  }
  free(st);
}

/* Propagate ivar types up the inheritance chain: a base-class method runs on
   subclass instances, so an ivar it reads must carry the union of every
   subclass's assignments. Without this, an abstract base whose @x is only set
   to nil/placeholder there sees the wrong type when it calls `@x.foo`, even
   though every concrete subclass assigns @x a real object.

   It also keeps the structs cast-compatible: an inherited method is emitted
   once and called as `sp_Base_m((sp_Base *)self)`, which is sound only while
   the base struct is a common initial sequence of every subclass struct. An
   ivar held as `sp_PolyArray *` in the base and `sp_RbVal` in a subclass has
   a different width there, so every later field sits at another offset.

   Monotonic (unify only widens), so callers iterate to a fixpoint. Returns
   whether anything widened. */
static int propagate_ivars_up(Compiler *c) {
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
  return prop_changed;
}

void analyze_program(Compiler *c) {
  comp_poly_candidates_reset();
  comp_descendants_reset();
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
  /* The same for a call's RECEIVER: `(Hash.new(0)).size` is the same call on
     the same hash, but the parentheses hid the receiver from every shape check
     that asks what it is -- the scan for a Hash.new used as a receiver, the
     anonymous-struct receiver scan, the empty-literal receiver marking -- and
     each left the value untyped, so its methods read as unresolved calls. They
     carry no meaning here either, so strip them once, as the block argument
     above does. */
  {
    NodeTable *ntm = (NodeTable *)c->nt;
    static const struct { NodeKind k; const char *field; } paren_fields[] = {
      { NK_CallNode, "receiver" },
      /* and the value a name is bound to: `h = (Hash.new(7))` hid the shape
         from the write-scanning resolvers the same way */
      { NK_LocalVariableWriteNode, "value" },
      { NK_InstanceVariableWriteNode, "value" },
      { NK_ClassVariableWriteNode, "value" },
      { NK_ConstantWriteNode, "value" },
    };
    for (int id = 0; id < ntm->count; id++) {
      NodeKind k = nt_kind(ntm, id);
      for (size_t f = 0; f < sizeof paren_fields / sizeof paren_fields[0]; f++) {
        if (paren_fields[f].k != k) continue;
        int rc = nt_ref(ntm, id, paren_fields[f].field);
        if (rc < 0) break;
        int un = unwrap_parens(c, rc);
        if (un >= 0 && un != rc) nt_node_set_ref(ntm, id, paren_fields[f].field, un);
        break;
      }
    }
  }
  /* A block written with its own rescue clause (`do ... rescue ... end`) has a
     BeginNode where every other block has a StatementsNode, so the body read as
     empty and the block's value was nil. Wrap it once here (#3710). */
  {
    NodeTable *ntm = (NodeTable *)c->nt;
    int n0 = ntm->count;
    for (int id = 0; id < n0; id++) {
      if (nt_kind(ntm, id) != NK_BlockNode) continue;
      int bd = nt_ref(ntm, id, "body");
      if (bd < 0 || nt_kind(ntm, bd) == NK_StatementsNode) continue;
      int st = nt_new_node(ntm, "StatementsNode");
      if (st < 0) continue;
      nt_node_set_arr(ntm, st, "body", &bd, 1);
      nt_node_set_ref(ntm, id, "body", st);
      comp_grow_node_arrays(c);
    }
  }
  desugar_root_scoped_constants(c);      /* ::Name -> Name (#4801) */
  desugar_class_reopen(c);               /* class Class / Class.class_eval -> a module */
  /* builtins/enumerable.rb, spliced by the parser: its definitions become
     the receiver-taking top-level functions before any scope is built */
  desugar_builtins(c);
  /* builtins/integer.rb, float.rb, comparable.rb: the same idea, one more
     container per file (analyze_desugar.c's sp_bx_* table) */
  desugar_builtin_scalar_defs(c);
  rename_redefined_toplevel_defs(c);     /* def f; f; def f -> def f__redef1; f__redef1; def f */
  rename_main_singleton_defs(c);         /* def self.k beside def k -> def self.k__main1 */
  scope_numbered_block_params(c);
  rename_shadowing_block_params(c);
  /* `:m.to_proc.call(r, a)` -> `r.m(a)`, before the to_proc rewrite below
     turns the receiver into a fixed-arity lambda (#3097). */
  desugar_sym_to_proc_call(c);
  desugar_enum_chain_shapes(c);          /* each_char.with_index / each.with_object */
  desugar_enum_chain_shapes(c);          /* 2nd pass: shapes the 1st created (e.g.
                                            map(&:sym.to_proc): to_proc->lambda first,
                                            then the lambda block attaches) */
  desugar_rightward_pattern(c);          /* `x => pat` -> one-arm case/in */
  desugar_match_predicate(c);            /* `x in pat` -> case/in true/else false */
  desugar_sort_by_with_index(c);         /* sort_by.with_index -> each_with_index.sort_by */
  desugar_forwarding_to_rest_callee(c);  /* def m(...) = f(*a, **k) -> anon *, ** */
  desugar_block_implicit_rest(c);        /* |x,| -> |x, __implicit_rest| (destructures) */
  desugar_block_destructure_params(c);   /* |a,(b,c),d| -> flat param + `b,c = __destr` */
  desugar_enumerator_produce(c);         /* Enumerator.produce -> fiber generator */
  desugar_recursive_param_defaults(c);   /* def m(x, y = m(..)) -> default helper method */
  qualify_colliding_consts(c);
  qualify_colliding_classes(c);
  walk_scope(c, c->nt->root_id, 0, -1);
  expand_static_splat_args(c);
  register_singleton_defs(c);   /* def CONST.m / def x.m -> synthesized subclass */
  register_structs(c);
  desugar_struct_index_ctor(c);
  fix_struct_block_scopes(c);
  register_module_functions(c);
  register_locals(c);
  desugar_block_arg_order(c);   /* recv.m(a, &expr): receiver, args, then the block (#4992) */
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
    int *rescue_ids = malloc(sizeof(int) * (size_t)(rn ? rn : 1));
    for (int i = 0; i < rn; i++) rescue_ids[i] = arms[i].id;
    for (int i = 0; i < rn; i++) {
      /* unanimity across every same-name rescue arm in the same scope */
      int unanimous = arms[i].spec;
      for (int j = 0; j < rn && unanimous >= 0; j++) {
        if (j == i || arms[j].vsc != arms[i].vsc || !sp_streq(arms[j].nm, arms[i].nm)) continue;
        if (arms[j].spec != arms[i].spec) unanimous = -1;
      }
      LocalVar *lv = scope_local_intern(arms[i].vsc, arms[i].nm);
      lv->type = unanimous >= 0 ? ty_object(unanimous) : TY_EXCEPTION;
      /* the name also holds what an ordinary write put there (`e = Foo.new`
         before `rescue => e`): the pin would retype those reads as the
         exception, so the slot holds either, boxed (#4923) */
      if (rescue_name_written_elsewhere(c, arms[i].vsc, arms[i].nm, rescue_ids, rn)) lv->type = TY_POLY;
      lv->is_block_param = 1;  /* set externally; don't reset in the fixpoint */
    }
    free(rescue_ids);
    free(arms);
  }

  resolve_parents(c);
  inherit_members(c);
  register_includes(c);
  register_include_attrs(c);
  register_extends(c);
  register_prepends(c);
  rewrite_attr_supers(c);
  specialize_inherited_cls_new(c);

  /* collect top-level `include <Mod>` / `extend <Mod>` calls so bare method
     calls can resolve to those modules' methods. At the top level the two
     differ only in who else gets the methods (Object vs main alone); a
     receiverless call in this file reaches them either way (#3787). */
  {
    const NodeTable *nt = c->nt;
    int root_stmts = nt_ref(nt, nt->root_id, "statements");
    int sn = 0;
    const int *stmts = root_stmts >= 0 ? nt_arr(nt, root_stmts, "body", &sn) : NULL;
    for (int i = 0; i < sn; i++) {
      if (!nt_type(nt, stmts[i]) || !sp_streq(nt_type(nt, stmts[i]), "CallNode")) continue;
      { const char *tn = nt_str(nt, stmts[i], "name");
        if (!tn || (!sp_streq(tn, "include") && !sp_streq(tn, "extend"))) continue; }
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
  /* A method whose `super` lands on a yielding parent yields too. The parent
     has no real function -- it is inlined at every call site -- so the child
     must be inlined as well, or the caller's block never reaches the parent
     and the call is left referencing a function nobody emitted. Iterate: a
     chain of supers settles in a few rounds. */
  {
    int ns = c->nscopes;
    char *has_super = (char *)calloc(ns > 0 ? (size_t)ns : 1, 1);
    if (has_super) {
      for (int id = 0; id < c->nt->count; id++) {
        const char *ty = nt_type(c->nt, id);
        if (!ty || (!sp_streq(ty, "SuperNode") && !sp_streq(ty, "ForwardingSuperNode"))) continue;
        Scope *sc = comp_scope_of(c, id);
        if (!sc) continue;
        int idx = (int)(sc - c->scopes);
        if (idx >= 0 && idx < ns) has_super[idx] = 1;
      }
      for (int round = 0; round < 8; round++) {
        int changed = 0;
        for (int i = 0; i < ns; i++) {
          Scope *s = &c->scopes[i];
          if (s->yields || !has_super[i] || s->class_id < 0 || !s->name) continue;
          int p = c->classes[s->class_id].parent;
          if (p < 0) continue;
          int mi = s->is_cmethod ? comp_cmethod_in_chain(c, p, s->name, NULL)
                                 : comp_method_in_chain(c, p, s->name, NULL);
          if (mi >= 0 && mi < ns && c->scopes[mi].yields) { s->yields = 1; changed = 1; }
        }
        if (!changed) break;
      }
      /* The same for a parent that keeps a named `&blk`: a super that brings no
         block of its own hands the parent the block this method was called
         with, declared or not. Without a block parameter to hold it that block
         was dropped, and the parent answered as if called without one (#4852).
         The method takes a synthetic one, which its callers fill as they fill
         any `&blk`. */
      char *bare_super = (char *)calloc((size_t)ns, 1);
      if (bare_super) {
        for (int id = 0; id < c->nt->count; id++) {
          const char *ty = nt_type(c->nt, id);
          if (!ty || (!sp_streq(ty, "SuperNode") && !sp_streq(ty, "ForwardingSuperNode"))) continue;
          if (nt_ref(c->nt, id, "block") >= 0) continue;
          Scope *sc = comp_scope_of(c, id);
          int idx = sc ? (int)(sc - c->scopes) : -1;
          if (idx >= 0 && idx < ns) bare_super[idx] = 1;
        }
        for (int round = 0; round < 8; round++) {
          int changed = 0;
          for (int i = 0; i < ns; i++) {
            Scope *s = &c->scopes[i];
            if (s->yields || s->blk_param || !bare_super[i] || s->class_id < 0 || !s->name) continue;
            int p = c->classes[s->class_id].parent;
            if (p < 0) continue;
            int mi = s->is_cmethod ? comp_cmethod_in_chain(c, p, s->name, NULL)
                                   : comp_method_in_chain(c, p, s->name, NULL);
            if (mi < 0 || mi >= ns) continue;
            Scope *pm = &c->scopes[mi];
            if (!pm->blk_param || !pm->blk_param[0] || pm->yields) continue;
            s->blk_param = strdup("__sblk__");
            LocalVar *sblk = scope_local_intern(s, s->blk_param);
            if (sblk) { sblk->type = TY_PROC; sblk->is_param = 1; }
            changed = 1;
          }
          if (!changed) break;
        }
        free(bare_super);
      }
      free(has_super);
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
  mark_empty_hash_enum_locals(c);
  mark_empty_literal_tails(c);
  mark_empty_literal_args(c);
  /* after the arg/receiver marks: a bare `{}` with no other context takes its
     variant from the peer it is compared against (#3040) or from the key it is
     indexed with (#3028, #3029) */
  /* Same argument, for the four passes mark_empty_literal_args is called
     alongside: none of them creates or renames a scope either, and each crosses
     the node table with method lookups underneath. */
  comp_scope_index_set_frozen(1);
  mark_mixed_key_hash_locals(c);
  mark_empty_hash_cmp_peers(c);
  (void)mark_empty_hash_key_ctx(c);
  mark_empty_hash_const_writes(c);
  comp_scope_index_set_frozen(0);

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
  /* blk_cond_pred[id] -- id is the condition of an if/unless/while/until.
     A bare `blk` there asks only "was a block given?", which emit_cond answers
     at the inline site without naming the slot. It is not an approved use (the
     method still cannot be spliced through it) but it is not a VALUE use
     either, so it must not drag the method into the lowered form -- that turns
     the folded `yield if block` into a real proc call.
     The four are named rather than taking every `predicate` ref: CaseNode and
     CaseMatchNode spell their SUBJECT `predicate` too, and `case blk` wants the
     block's value like any other read. Naming the boolean contexts also makes
     the failure direction the safe one -- a node type left out is read as a
     value use, which lowers a method that need not have been. */
  char *blk_cond_pred = (char *)calloc((size_t)c->nt->count, 1);
  for (int p = 0; blk_cond_pred && p < c->nt->count; p++) {
    const char *cty = nt_type(c->nt, p);
    if (!cty || !(sp_streq(cty, "IfNode") || sp_streq(cty, "UnlessNode") ||
                  sp_streq(cty, "WhileNode") || sp_streq(cty, "UntilNode"))) continue;
    int pr = nt_ref(c->nt, p, "predicate");
    if (pr >= 0 && pr < c->nt->count) blk_cond_pred[pr] = 1;
  }
  for (int p = 0; (blk_call_recv && blk_arg_expr) && p < c->nt->count; p++) {
    const char *pty = nt_type(c->nt, p);
    if (!pty) continue;
    if (sp_streq(pty, "CallNode")) {
      const char *cn = nt_str(c->nt, p, "name");
      int r = nt_ref(c->nt, p, "receiver");
      if (cn && sp_streq(cn, "call")) {
        if (r >= 0 && r < c->nt->count) blk_call_recv[r] = 1;
      }
      /* `blk.nil?` / `!blk` only ASK about the block; the block goes
         nowhere. `__blk.call unless __blk.nil?` is how an ingested Rails
         helper forwards an anonymous block, and reading the nil? test as an
         escape kept the helper out of line, lifted its caller's block into a
         proc, and celled the caller's String buffer -- which took the
         by-reference ABI away from the buffer, so every append the caller
         made vanished (#4477). A bare `if blk` is NOT approved here: the
         methods it would newly inline include forwarders called without a
         block, whose `inner(&blk)` then names a block that no inline site
         declares (block_forward_nilcheck, toplevel_extend_block_param). */
      else if (cn && (sp_streq(cn, "nil?") || sp_streq(cn, "!"))) {
        int an = 0; int aa = nt_ref(c->nt, p, "arguments");
        if (aa >= 0) nt_arr(c->nt, aa, "arguments", &an);
        if (an == 0 && r >= 0 && r < c->nt->count) blk_call_recv[r] = 1;
      }
    }
    else if (sp_streq(pty, "BlockArgumentNode")) {
      int e = nt_ref(c->nt, p, "expression");
      if (e >= 0 && e < c->nt->count) blk_arg_expr[e] = 1;
    }
  }
  /* For each forwarded `&blk` argument, the user method it is handed to: a
     forward is only harmless when the CALLEE lets the block go no further.
     One that stores it (`$p = b`) keeps it past the call, so the forwarder
     cannot be inlined either -- its captures need cells (#3772). */
  int *blk_fwd_callee = (int *)malloc(sizeof(int) * (size_t)(c->nt->count > 0 ? c->nt->count : 1));
  if (blk_fwd_callee) {
    for (int p = 0; p < c->nt->count; p++) blk_fwd_callee[p] = -1;
    for (int p = 0; p < c->nt->count; p++) {
      if (nt_kind(c->nt, p) != NK_CallNode) continue;
      int fb = nt_ref(c->nt, p, "block");
      if (fb < 0 || !nt_type(c->nt, fb) || !sp_streq(nt_type(c->nt, fb), "BlockArgumentNode")) continue;
      int fe = nt_ref(c->nt, fb, "expression");
      if (fe < 0 || fe >= c->nt->count) continue;
      const char *fn = nt_str(c->nt, p, "name");
      if (!fn) continue;
      int fmi = -1;
      int frecv = nt_ref(c->nt, p, "receiver");
      if (frecv < 0) {
        fmi = comp_method_index(c, fn);
        Scope *fs = comp_scope_of(c, p);
        if (fmi < 0 && fs && fs->class_id >= 0)
          fmi = fs->is_cmethod ? comp_cmethod_in_chain(c, fs->class_id, fn, NULL)
                               : comp_method_in_chain(c, fs->class_id, fn, NULL);
      }
      else {
        /* `h.store(&b)`: an object receiver resolves through its class, and a
           constant one through its class methods. Without this the callee was
           unknown and a forward into a method that STORES the block counted as
           harmless -- the block was inlined and its captures never celled
           (#3783). */
        TyKind frt = infer_type(c, frecv);
        if (ty_is_object(frt)) fmi = comp_method_in_chain(c, ty_object_class(frt), fn, NULL);
        else if (nt_kind(c->nt, frecv) == NK_ConstantReadNode ||
                 nt_kind(c->nt, frecv) == NK_ConstantPathNode) {
          int fci = comp_class_index(c, nt_str(c->nt, frecv, "name"));
          if (fci >= 0) fmi = comp_cmethod_in_chain(c, fci, fn, NULL);
          /* `Klass.new(&b)` with no `def self.new` hands the block to
             initialize, which may store it */
          if (fci >= 0 && fmi < 0 && sp_streq(fn, "new"))
            fmi = comp_method_in_chain(c, fci, "initialize", NULL);
        }
        /* This pass runs before the receiver's type settles, so an unresolved
           receiver falls back to the name: any block-taking method that could
           be the callee decides, which at worst leaves a forwarder uninlined. */
        if (fmi < 0) {
          /* Several methods can share the name, and the first one found is not
             necessarily the callee. Take the one that KEEPS the block if any
             does: assuming the harmless candidate compiled the same program
             right or wrong depending on the order the classes were defined in
             (#3786). */
          int first = -1;
          int keeper = a_name_keeps_block(c, fn, blk_call_recv, blk_arg_expr, &first);
          fmi = keeper >= 0 ? keeper : first;
        }
      }
      blk_fwd_callee[fe] = fmi;
    }
  }
  /* The yield-inlining analysis below reads scope shape but never creates or
     renames a scope, so the shape index is stable across it -- the same argument
     mark_empty_literal_args makes for its own body. Frozen, the method lookups
     underneath it (per scope * per node, through a_block_is_lifted -> infer_type
     -> infer_call) take the O(1) hash index instead of the unfrozen O(nscopes)
     reverse scan, and the name-keyed memo in an_user_defines_or_reads -- which
     stands down while the index is unfrozen -- is live for them. */
  comp_scope_index_set_frozen(1);
  char *inline_cand = (char *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
  int cfwd = 64, nfwd = 0;
  int *fwd_from = (int *)malloc(sizeof(int) * (size_t)cfwd);
  int *fwd_to = (int *)malloc(sizeof(int) * (size_t)cfwd);
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
        /* ... and a literal block handed to a method on a receiver whose type
           has not settled yet (`@reg.set { handler.call(v) }`, `@reg` typed
           later): the callee is judged by name, as the forward above is, and
           one that keeps its block lifts this one. Asking a_block_is_lifted
           alone answered "spliced", the method was inlined with no storage for
           `handler`, and codegen -- on settled types -- lifted the block and
           named a `_cell_handler` nothing declared. */
        if (!a_proc_create_or_lifted(c, id) &&
            !a_block_lifted_by_callee_name(c, id, blk_call_recv, blk_arg_expr)) continue;
        if (comp_scope_of(c, id) != m) continue;
        int body = a_proc_body(c, id);
        if (body >= 0) a_mark_subtree(c, body, inproc_m);
      }
    }
    int escapes = 0, uses = 0, value_use = 0;
    for (int id = 0; id < c->nt->count && !(escapes && value_use); id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
      if (comp_scope_of(c, id) != m) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || !sp_streq(nm, m->blk_param)) continue;
      /* A read inside a nested proc body is a capture-escape: the proc
         holds a reference to blk independently of the call site. */
      if (inproc_m && inproc_m[id]) { escapes = 1; value_use = 1; continue; }
      uses++;
      /* approved: receiver of a `.call`, or expression of a `&block` arg */
      int ok = (blk_call_recv && blk_call_recv[id]) || (blk_arg_expr && blk_arg_expr[id]);
      if (!ok) {
        escapes = 1;
        /* Everything the inline path can still answer without the slot --
           `blk.nil?`, `!blk`, and a bare `blk` in a condition -- is approved
           above or folded by emit_cond. What is left genuinely wants the
           block's VALUE, and no call site can supply one. */
        if (!(blk_cond_pred && blk_cond_pred[id])) value_use = 1;
      }
      /* ... but a forward into a user method that keeps the block is an
         escape all the same, one call deeper (#3772). Only while nothing has
         escaped yet: the scan used to stop at the first escape, and an already
         disqualified method adding edges here would crowd real ones out of the
         fixed-size list. */
      else if (!escapes && blk_arg_expr && blk_arg_expr[id] && blk_fwd_callee) {
        int callee = blk_fwd_callee[id];
        if (callee >= 0 && callee < c->nscopes) {
          Scope *cs2 = &c->scopes[callee];
          if (cs2->blk_param && cs2->blk_param[0] && nfwd < cfwd) {
            fwd_from[nfwd] = mi; fwd_to[nfwd] = callee; nfwd++;
          }
        }
      }
    }
    free(inproc_m);
    /* Recorded for the lowering pass further down: a method that wants its
       block's VALUE cannot be spliced, and if it also contains a literal
       `yield` the inline path is the only one it has -- which emits no
       storage for the block while the value use still names it. */
    m->blk_param_value_use = value_use;
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
      if (!has_ret && !scope_calls_itself(c, mi)) inline_cand[mi] = 1;
    }
  }
  /* A forwarder may only be inlined when every method it hands the block to is
     inlined too: an un-inlined callee keeps the block as a real proc past the
     call, so the forwarder's captures must live in cells (#3772). */
  for (int round = 0; round < 8; round++) {
    int changed2 = 0;
    for (int e = 0; e < nfwd; e++)
      if (inline_cand[fwd_from[e]] && !inline_cand[fwd_to[e]] &&
          !c->scopes[fwd_to[e]].yields) {
        inline_cand[fwd_from[e]] = 0; changed2 = 1;
      }
    if (!changed2) break;
  }
  for (int mi = 0; mi < c->nscopes; mi++)
    if (inline_cand[mi]) c->scopes[mi].yields = 1;
  comp_scope_index_set_frozen(0);
  free(inline_cand); free(fwd_from); free(fwd_to);
  free(blk_call_recv);
  free(blk_arg_expr);
  free(blk_cond_pred);
  free(blk_fwd_callee);

  /* intern every symbol literal so codegen can emit the id table */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && sp_streq(ty, "SymbolNode")) {
      const char *v = nt_str(c->nt, id, "value");
      /* the node's own byte length: a symbol literal may hold a NUL, and
         interning it by strlen registers the short name codegen then emits */
      if (v) comp_sym_intern_n(c, v, nt_str_len(c->nt, id, "value"));
    }
    /* a def in value position evaluates to :name. A builtin's definition
       (`__enum_<m>`, `__int_<m>`, `__flt_<m>`, `__cmp_<m>`, and each of
       their per-call-site copies, and enumerator.rb's `__enumw_<m>`) is
       never in value position: its name
       would only add a string per call site to the symbol table. The
       Integer/Float/Comparable containers (analyze_desugar.c's sp_bx_*
       tables) share this same "generic def cloned per call site" shape
       enumerable.rb pioneered, but this exclusion was never extended to
       them -- invisible for Integer/Float's narrower methods, but a
       Comparable#clamp/#between? and its own private helper, reached from
       nearly everywhere, added one string per call site of any of them
       (found in this migration's own corpus sweep). */
    else if (ty && sp_streq(ty, "DefNode")) {
      const char *dn = nt_str(c->nt, id, "name");
      if (dn && strncmp(dn, "__enum_", 7) != 0 && strncmp(dn, "__int_", 6) != 0 &&
          strncmp(dn, "__flt_", 6) != 0 && strncmp(dn, "__cmp_", 6) != 0 &&
          strncmp(dn, "__enumw_", 8) != 0)
        comp_sym_intern(c, dn);
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

  /* The classes that can exist, before any type is derived from them (see
     compute_instantiated): reachability is a matter of names and a
     construction site is a call node, so both are known now. Recomputed
     after the fixpoint, when a dynamic `.new` can be told from a static one. */
  compute_reachable(c);
  compute_instantiated(c, 1);

  g_fixpoint_rounds = 0;
  /* Two rounds. The proc-form clones are made between them: knowing which
     methods a poly dispatch will name needs settled receiver types, and the
     clones' own bodies then need inferring like any other. The second round is
     a no-op when nothing was cloned. (#3399) */
  for (int pf_round = 0; pf_round < 2; pf_round++) {
  if (pf_round == 1 && !make_yield_proc_forms(c)) break;
  g_infer_optimistic = 1;
  for (int iter = 0; iter < 128; iter++) {
    if (iter + 1 > g_fixpoint_rounds) g_fixpoint_rounds = iter + 1;
    g_infer_round = iter + 1;
    int ch = 0;
    seed_unsupplied_nil_defaults(c);   /* ahead of the round's binding, every round: the round's reset clears it (#4583) */
    sp_narrow_memo_bump();  /* invalidate per-iteration narrow-helper memo */
    build_ie_map(c);  /* refresh instance_exec receiver-class map each pass */
    ch |= register_ie_block_ivars(c);  /* slot ivars first assigned in iexec blocks */
    ch |= infer_write_types(c);
    /* The table type has to be visible HERE, not after the fixpoint: a
       parameter bound from `@t[k][j]` widens to poly on the first iteration
       and never comes back (parameters only widen). The narrowing needs
       infer_write_types to have given the ivar its poly-array type first, and
       the locals read out of it (`row = @t[r]`) need one more write pass to
       re-derive from the narrowed type before the binding below sees them. */
    if (narrow_int_table_ivars(c)) ch |= infer_write_types(c);
    /* The same timing argument for a table held in a LOCAL, or one that
       crosses a call: the helper reading `row = t[i]` binds its parameter on
       the round the call is first seen, and a parameter only ever widens. Run
       after the ivar narrowing so a table read out of an ivar is already
       typed when the local reading it is derived. */
    if (narrow_object_arrays(c)) ch |= infer_write_types(c);
    /* Enumerable over a Hash/Range with only an Array arm: route through to_a
       (needs the receiver kind, so it runs inside the fixpoint). */
    if (desugar_enumerable_via_to_a(c)) ch |= infer_write_types(c);
    narrow_locals_from_arrays(c);
    ch |= infer_param_types(c);
    reassert_rbs_param_seeds(c);   /* a seed outranks a narrowing derived from one call site */
    ch |= bind_coerce_operator_params(c);   /* 3 + obj calls obj's op WITH obj */
    ch |= infer_param_hash_value(c);
    ch |= propagate_prep_params(c);
    ch |= infer_string_params(c);
    ch |= infer_default_param_types(c);
    ch |= expand_literal_splat_args(c);        /* builtin/proc m(*[a,b]) -> m(a, b) */
    ch |= pin_arg_position_hash_new(c);        /* f(Hash.new(d)) -> PolyPoly variant */
    ch |= pad_unsupplied_params(c);            /* under-supplied call: placeholder param type */
    ch |= desugar_builtin_method_obj(c);       /* builtin recv.method(:sym) -> wrapper def */
    ch |= desugar_class_body_bare_new(c);      /* class body `new(x)` -> `Klass.new(x)` */
    ch |= desugar_bare_const_get(c);           /* cmethod `const_get(:K)` -> `self.const_get(:K)` */
    ch |= desugar_ie_bare_object_calls(c);     /* instance_eval { is_a?(K) } -> self.is_a?(K) */
    ch |= desugar_masgn_object_index(c);       /* obj[k], x = rhs -> tmp, x = rhs; obj[k] = tmp */
    ch |= desugar_include_math(c);             /* include Math: sqrt(x) -> Math.sqrt(x) */
    ch |= desugar_kernel_recv(c);              /* Kernel.puts x -> puts x */
    ch |= desugar_class_literal_ctors(c);      /* Array[a,b] -> [a,b]; Range.new -> (a..b) */
    ch |= desugar_multi_yield_map_param(c);    /* multi-yield each: map's |x| takes the 1st */
    ch |= desugar_enum_walk_calls(c);          /* enum.map { break } -> __enumw_map(enum) { } */
    ch |= desugar_enum_method_recv(c);         /* obj.map{} -> obj.__enum_to_a.map{} */
    ch |= give_native_self_calls_a_receiver(c);  /* native class: implicit self -> self.m */
    ch |= give_self_predicates_a_receiver(c);    /* is_a?(X) on implicit self -> self.is_a?(X) */
    ch |= desugar_for_enumerable(c);           /* for x in obj -> for x in obj.__enum_to_a */
    ch |= desugar_lazy_terminal(c);            /* lz.sum -> lz.to_a.sum */
    ch |= desugar_string_upto(c);              /* "a".upto("c") -> ("a".."c").each */
    ch |= desugar_file_stat_new(c);            /* File::Stat.new(p) -> File.stat(p) */
    ch |= desugar_module_function_call(c);     /* helper(x) in an includer -> M.helper(x) */
    ch |= desugar_method_block_arg(c);         /* m(&method(:x)) -> m(&method(:x).to_proc) */
    ch |= desugar_curry_block_arg(c);          /* iter(&curried) -> iter { |e| curried[e] } */
    ch |= desugar_yielder_block_arg(c);        /* src.each(&y) -> src.each { |e| y << e } */
    ch |= desugar_to_enum(c);                  /* recv.to_enum(:m) -> generator/blockless */
    ch |= type_block_rest_params(c);           /* |*rest| locals are poly arrays */
    ch |= desugar_public_method(c);            /* recv.public_method(:m) -> recv.method(:m) */
    ch |= desugar_class_eval_value(c);
    ch |= desugar_instance_eval_builtin(c);    /* "s".instance_eval { m } -> splice on a temp */
    ch |= desugar_builtin_class_var_recv(c);   /* k = Array; k.new(..) -> Array.new(..) */
    ch |= desugar_compose_method_operand(c);   /* proc >> meth -> proc >> meth.to_proc */
    ch |= desugar_method_curry(c);             /* meth.curry -> meth.to_proc.curry */
    ch |= desugar_curry_arity_to_int(c);       /* proc.curry(obj) -> proc.curry(obj.to_int) */
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
    ch |= widen_mixed_key_hash_slots(c);
    ch |= desugar_lazy_stateful_stage(c);     /* arr.lazy.uniq -> arr.uniq (finite source) */
    ch |= desugar_lazy_method_call(c);         /* lz.first where `def lz; ...lazy...; end` */
    ch |= desugar_str_range_methods(c);        /* ("a".."e").map -> .to_a.map */
    ch |= desugar_sym_to_proc_call(c);         /* :m.to_proc.call(r, a) -> r.m(a) */
    ch |= desugar_reduce_method_symbol(c);     /* reduce(:gcd) -> reduce { |a,x| a.gcd(x) } */
    ch |= desugar_symbol_var_block_arg(c);     /* m(&sym_var) -> m { |x| x.send(sym_var) } */
    ch |= desugar_kernel_method_block_arg(c);  /* m(&method(:Integer)) -> m { |x| Integer(x) } */
    ch |= desugar_empty_block_body(c);         /* m { } -> m { nil } */
    ch |= desugar_hash_block_arg(c);           /* m(&hash) -> m { |x| hash[x] } */
    ch |= desugar_dynamic_send(c);             /* recv.send(var, a) -> static name dispatch */
    ch |= desugar_toplevel_instance_exec(c);   /* top-level instance_exec(&b) -> b.call */
    ch |= desugar_binding_lvget(c);            /* binding.local_variable_get(:x) -> x.itself */
    ch |= desugar_step_kwargs(c);              /* n.step(to: X, by: Y) -> n.step(X, Y) */
    ch |= desugar_respond_to_probe(c);         /* recv.respond_to?(:m) -> probe recv.m type */
    ch |= desugar_symbol_to_proc_call(c);      /* :sym.to_proc.call(x) -> x.sym */
    ch |= desugar_call_op_write(c);            /* r.x += 1 with a def writer -> r.x = r.x + 1 */
    ch |= desugar_index_op_write_user(c);      /* obj[k] ||= v on a user [] / []= -> the calls */
    ch |= desugar_main_self_call(c);           /* self.m on main with a top-level def m -> m */
    ch |= desugar_array_at(c);                 /* a.at(i) -> a[i] */
    ch |= desugar_array_first_last(c);         /* arr.first -> arr[0], arr.last -> arr[-1] */
    ch |= desugar_to_h_block(c);               /* recv.to_h{|e|[k,v]} -> recv.map{...}.to_h */
    ch |= desugar_to_proc_block_arg(c);        /* &obj (user to_proc) -> &(obj.to_proc) hoisted once */
    ch |= desugar_proc_expr_block_arg(c);      /* &(a >> b) -> hoisted temp */
    ch |= desugar_to_hash_splat(c);            /* f(**obj) -> f(**obj.to_hash) */
    ch |= desugar_value_callable_forwards(c);  /* &proc -> { |x| proc.call(x) } */
    if (desugar_builtin_enum_calls(c)) {       /* recv.m(a) { } -> __enum_m(recv, a) { } */
      ch = 1;
      /* the call is a user method's now: its empty `{}` argument takes the
         widest hash the way any yielding method's does */
      mark_empty_literal_args(c);
    }
    ch |= desugar_builtin_scalar_calls(c);     /* recv.m(a) -> __int_m(recv, a) etc */
    ch |= narrow_empty_array_args_by_yield(c); /* f([]) { |m| m << 1 }: the [] is an int array */
    ch |= fold_static_is_a(c);                 /* if v.is_a?(Array) on a typed local: one arm */
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
    ch |= promote_append_accumulators(c);
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
        if (p && p->type == TY_POLY && !p->is_block_param && !p->poly_dispatch_widened) { p->type = TY_UNKNOWN; why_reset(&p->why); any = 1; }
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
        lv->type = TY_UNKNOWN; why_reset(&lv->why); any = 1;
        if (nlrec >= lcap) { lcap *= 2; recLs = realloc(recLs, sizeof(int) * lcap); recLi = realloc(recLi, sizeof(int) * lcap); }
        recLs[nlrec] = s; recLi[nlrec] = i; nlrec++;
      }
    }
    if (reset_locked_iter_block_params(c)) any = 1;
    if (any) {
      TyKind *prev = (TyKind *)malloc(sizeof(TyKind) * (nrec > 0 ? nrec : 1));
      TyKind *lprev = (TyKind *)malloc(sizeof(TyKind) * (nlrec > 0 ? nlrec : 1));
      /* post-derive snapshots of the previous iteration, for the fixed-cycle
         exit below */
      TyKind *prevd = (TyKind *)malloc(sizeof(TyKind) * (nrec > 0 ? nrec : 1));
      TyKind *lprevd = (TyKind *)malloc(sizeof(TyKind) * (nlrec > 0 ? nlrec : 1));
      int have_prevd = 0;
      for (int iter = 0; iter < 128; iter++) {
        /* Parameters bind from the SETTLED state of the previous iteration,
           before this one's re-clear. Bound after it, a parameter sampled
           whichever of an ivar's writes had been merged by then: a poly ivar
           holding an Array on one path and a Relation on another was merged
           from the Relation write alone when the Array-answering call had not
           been typed yet that round, the parameter was pinned to Relation, and
           the later widening never reached it because the next iteration's
           bind sampled the same partial state. The call site then read the
           Array's header as a Relation (#4437). One iteration of lag, and the
           loop's stability test already waits for the ivars to stop moving. */
        seed_unsupplied_nil_defaults(c);   /* a re-cleared nil-default parameter is poly again before anything binds on it (#4583) */
        infer_param_types(c);
        /* A default is a binding like any call site's argument, and reads the
           same settled state: after the re-clear below, a default reading a
           reset ivar (`romh: @rom`, @rom poly) saw UNKNOWN every iteration, so
           it never widened the parameter and the one call site's explicit
           argument type stood alone. The pass further down still runs for
           defaults over state this iteration derives; types only widen. */
        int pre_def = infer_default_param_types(c);
        /* stash last-settled values, then re-clear the reset ivars so they
           recompute fresh (narrowing) this iteration. */
        for (int k = 0; k < nrec; k++) prev[k] = c->classes[recCi[k]].ivar_types[recIv[k]];
        for (int k = 0; k < nrec; k++) c->classes[recCi[k]].ivar_types[recIv[k]] = TY_UNKNOWN;
        for (int k = 0; k < nlrec; k++) lprev[k] = c->scopes[recLs[k]].locals[recLi[k]].type;
        for (int k = 0; k < nlrec; k++) c->scopes[recLs[k]].locals[recLi[k]].type = TY_UNKNOWN;
        sp_narrow_memo_bump();  /* invalidate per-iteration narrow-helper memo */
        int ch = pre_def, ch_other = pre_def;
        ch |= infer_write_types(c);
        { int _w = bind_coerce_operator_params(c); ch |= _w; ch_other |= _w; }   /* 3 + obj calls obj's op WITH obj */
        { int _w = infer_param_hash_value(c); ch |= _w; ch_other |= _w; }
        { int _w = propagate_prep_params(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_string_params(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_default_param_types(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_block_params(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_for_index(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_catch_block_params(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_global_const_types(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_multiwrite_const_types(c); ch |= _w; ch_other |= _w; }
        ch |= infer_ivar_types(c);
        /* AFTER the ivar write-merge: the re-clear zeroes recorded ivars at
           each iteration's top, so a promote gated on STRING must see the
           freshly re-derived type, not the cleared UNKNOWN (#3227 P4) */
        { int _w = promote_shared_stored_strings(c); ch |= _w; ch_other |= _w; }
        { int _w = promote_append_accumulators(c); ch |= _w; ch_other |= _w; }
        { int _w = widen_shared_cmp_params(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_cvar_types(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_inherited_ivars(c); ch |= _w; ch_other |= _w; }
        { int _w = infer_return_types(c); ch |= _w; ch_other |= _w; }
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
          /* Fixed-cycle exit: when no pass beyond the re-derive pair
             (infer_write_types / infer_ivar_types, which re-fill the slots
             the re-clear zeroes and so report "change" every round by
             construction) changed anything this iteration, AND every
             recorded slot re-derived to the value it re-derived to last
             iteration, the loop is a period-1 cycle: the re-derive pair is
             deterministic over frozen inputs, so each further iteration
             recomputes exactly this state, and running to the 128-round cap
             ends HERE anyway. The stability test above cannot
             see it: it compares against the pre-clear snapshot, and a slot
             the top-of-loop param bind re-widens (poly in, nothing derives
             it back) differs from its own recompute every round, forever.
             One 53k-line machine-generated program burned the whole cap --
             123 no-op iterations, each a dozen whole-program passes -- on
             exactly that shape. */
          if (!ch_other && have_prevd) {
            int cyc = 1;
            for (int k = 0; k < nrec && cyc; k++)
              if (c->classes[recCi[k]].ivar_types[recIv[k]] != prevd[k]) cyc = 0;
            for (int k = 0; k < nlrec && cyc; k++)
              if (c->scopes[recLs[k]].locals[recLi[k]].type != lprevd[k]) cyc = 0;
            if (cyc) break;
          }
          for (int k = 0; k < nrec; k++) prevd[k] = c->classes[recCi[k]].ivar_types[recIv[k]];
          for (int k = 0; k < nlrec; k++) lprevd[k] = c->scopes[recLs[k]].locals[recLi[k]].type;
          have_prevd = 1;
        }
        else if (!ch) break;
      }
      /* the bind lags one iteration; take the settled state once more */
      infer_param_types(c);
      free(prev); free(lprev); free(prevd); free(lprevd);
    }
    free(recCi); free(recIv); free(recLs); free(recLi);
  }

  /* Backstop: a constant bound to an EMPTY array literal has no element type to
     read off the literal, and nothing else in the program need ever narrow it
     (`DISPATCH = []` filled only through `DISPATCH[op] = args`, whose value type
     may itself still be settling). Leaving it UNKNOWN does not read as "unknown"
     downstream, it reads as UNDEFINED: every reference reports the constant as
     defined nowhere and raises NameError, where CRuby answers the empty array
     (#4051). Default it the way an empty literal bound to a local defaults. This
     runs after the fixpoint, so any real element evidence still wins. */
  for (int id = 0; id < c->nt->count; id++) {
    if (nt_kind(c->nt, id) != NK_ConstantWriteNode) continue;
    const char *cwn = nt_str(c->nt, id, "name");
    LocalVar *cwv = cwn ? comp_const(c, cwn) : NULL;
    if (!cwv || cwv->type != TY_UNKNOWN) continue;
    int cwval = nt_ref(c->nt, id, "value");
    if (cwval < 0 || nt_kind(c->nt, cwval) != NK_ArrayNode) continue;
    int cwn_els = 0;
    nt_arr(c->nt, cwval, "elements", &cwn_els);
    if (cwn_els == 0) cwv->type = TY_POLY_ARRAY;
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
      if (p->type == TY_UNKNOWN)
        slot_rule(c, p, TY_POLY, sc->pdefault[i], "a `= nil` default and no call site typing it: the parameter holds nil or a value, untyped");
      else if (p->type == TY_SYMBOL || p->type == TY_BOOL)
        slot_rule(c, p, TY_POLY, sc->pdefault[i], "a `= nil` default on a Symbol or Bool parameter, which has no nil of its own: boxed, untyped");
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
  /* An ivar write in VALUE position evaluates to the slot. When the slot is
     a strbuf, the value is presented exactly as an ordinary read of the slot
     is: the const char * face with the live handle in _sp_ret_strbuf, so the
     write's type, the return of a method whose body it is and every call to
     that method agree on `const char *`. The write used to be marked to
     yield the handle itself (#3993), which typed the function `sp_String *`
     while its callers, typed from the String face, read a `const char *`:
     `p obj.w("a")` for `def w(v) = (@body = v.to_s)` printed garbage, and
     the C did not build under -Werror (#4567). */
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
    if (!lv || lv->rbs_seeded) continue;
    if (lv->type == TY_UNKNOWN) {
      /* Another write of a hash literal makes the slot hold both kinds, and
         only the boxed one does: the two empty literals otherwise agreed on
         "no type" and the array backstop claimed the slot for arrays, so the
         hash literal was built into an array-shaped slot. */
      int has_hash_write = 0;
      for (int w = 0; w < c->nt->count && !has_hash_write; w++) {
        if (nt_kind(c->nt, w) != NK_LocalVariableWriteNode) continue;
        const char *wn = nt_str(c->nt, w, "name");
        if (!wn || !nm || !sp_streq(wn, nm) || comp_scope_of(c, w) != s) continue;
        int wv = nt_ref(c->nt, w, "value");
        if (wv >= 0 && nt_kind(c->nt, wv) == NK_HashNode) has_hash_write = 1;
      }
      lv->type = has_hash_write ? TY_POLY : TY_POLY_ARRAY;
    }
    /* A hash type here is usually one a premature `[]` read promoted, and a
       local whose every write is an empty array literal is definitively an
       array. A local that really is written a hash somewhere else holds both,
       and only the boxed slot does. */
    else if (ty_is_hash(lv->type))
      lv->type = local_all_writes_empty_array(c, s, nm) ? TY_POLY_ARRAY : TY_POLY;
    /* A slot another write typed as something that cannot hold an array (a
       String, an Integer, an object) has to widen: the empty literal carries
       no type of its own, so the union kept the other write's type and the
       array was emitted into a slot shaped for it (`a = ""; a = []` assigned
       an sp_IntArray to a const char *). A non-empty literal already widens
       this way -- the union of the two types is the boxed one (#3990). */
    else if (!ty_is_array(lv->type) && lv->type != TY_POLY) lv->type = TY_POLY;
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
      TyKind want = (c->hash_want && v < c->node_cap) ? c->hash_want[v] : TY_UNKNOWN;
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
      if (cl->ivar_int_table[iv]) continue;   /* already narrowed and pinned */
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
  /* `rescue K => e` where a second arm binds the same name: reads inside an
     arm take that arm's class (#4343). */
  narrow_rescue_arm_reads(c);
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
         stop at nested defs. The worklist grows: a fixed 250 dropped the
         children past it, and an ivar read there was missed. */
      int cap = 256, sp = 0;
      int *stack = (int *)malloc(sizeof(int) * (size_t)cap);
      if (!stack) continue;
      stack[sp++] = v;
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
        int na2 = nt_num_arrs(c->nt, nid);
        int more = nr2;
        for (int i2 = 0; i2 < na2; i2++) { int nn2 = 0; nt_arr_at(c->nt, nid, i2, &nn2); more += nn2; }
        if (sp + more > cap) {
          while (sp + more > cap) cap *= 2;
          int *g = (int *)realloc(stack, sizeof(int) * (size_t)cap);
          if (!g) break;
          stack = g;
        }
        for (int i2 = 0; i2 < nr2; i2++) { int ch2 = nt_ref_at(c->nt, nid, i2); if (ch2 >= 0) stack[sp++] = ch2; }
        for (int i2 = 0; i2 < na2; i2++) { int nn2 = 0; const int *ids2 = nt_arr_at(c->nt, nid, i2, &nn2); for (int k2 = 0; k2 < nn2; k2++) if (ids2[k2] >= 0) stack[sp++] = ids2[k2]; }
      }
      free(stack);
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
        /* a rest collects its arguments into an array whatever the other
           method has in that slot (#4871) */
        if (k == sc1->rest_idx || k == sc2->rest_idx) continue;
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
  /* a module method named by Mod.instance_method(:m) is referenced directly */
  unmark_referenced_module_sources(c);
  /* Which exact cls_ids can appear at runtime -- lets the poly-dispatch switch
     drop `case` arms for classes that are never instantiated (the referenced
     method then DCEs as an unreferenced static). */
  compute_instantiated(c, 0);

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
    /* A self-recursive yielder's `{ yield }` is itself a lifted block (it is
       passed to the lowered method), so the recursion form is asked first:
       it carries the block's value back at its real type, which the lifted
       form does not. */
    int self_rec = scope_calls_itself(c, mi);
    int thread_yld = !self_rec && scope_yields_inside_lifted_body(c, mi);
    /* The declared `&blk` is also used as a VALUE -- handed to another method,
       assigned to a local, captured by a nested proc. The inline path has no
       storage to name there (it emitted a reference to an `lv_blk` it never
       declared), and a `yield` next to such a use is otherwise the only thing
       standing between the method and the ordinary proc-parameter form it
       already compiles without the `yield`. */
    int blk_value_use = !self_rec && !thread_yld && m->blk_param_value_use;
    if (!self_rec && !thread_yld && !blk_value_use) continue;
    m->is_lowered_yield = 1;
    m->lowered_lifted_yield = thread_yld;
    m->yields = 0;
    /* `if block_given? ... else ... end` was typed per call form while the
       method was inlined (Scope.ret_noblock); as one emitted function both
       arms are live at run time, so its value is the two joined */
    if (m->ret_noblock != TY_UNKNOWN) { m->ret = ty_unify(m->ret, m->ret_noblock); m->ret_noblock = TY_UNKNOWN; }
    /* The return the fixpoint derived stands. This forced TY_INT, which was
       right only by coincidence: the body's own tail already types the method,
       and overwriting it made `walk` answer Integer where it answers nil, and
       made a body ending in a String or an Array emit `return char *` from a
       function typed sp_int -- no binary at all (#4145). A body that types to
       nothing usable still needs a declarable slot, which is what the default
       below is for. */
    m->lowered_carries_block_value = !thread_yld && a_scope_returns_a_yield(c, mi);
    /* The return the fixpoint derived stands, unless the method's value comes
       out of a `yield`: then the emitted tail reads the block's answer from the
       poly side-channel as a raw slot, and the function has to be typed to
       receive it. This used to force TY_INT unconditionally, which was right
       only by coincidence -- `walk` answered Integer where it answers nil, and
       a body ending in a String or an Array emitted `return char *` from a
       function typed sp_int, producing no binary at all (#4145). */
    if (!thread_yld &&
        (m->ret == TY_UNKNOWN || m->ret == TY_VOID || m->lowered_carries_block_value))
      /* A method whose value comes out of a `yield` hands back whatever the
         block answered, and one C function serves every call site -- the same
         `countdown` is called with a String block and an Integer block. TY_INT
         was the RAW CARRIER for that: the slot's bits, cast back to the real
         type at the call site. Only some consumer paths emitted that cast
         (`puts x` did, `p x` did not), so a block answering a String or an
         Array reached a `const char *` slot as an sp_int and the C did not
         compile; a block answering poly had no cast that could work, the bits
         being unable to carry a tag. Box instead, and let method_call_ret
         agree: a poly return is the one shape every consumer already handles,
         and it is what "the block answered something only the call site
         knows" has meant everywhere else. */
      m->ret = m->lowered_carries_block_value ? TY_POLY : TY_INT;
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
     the bound Method ABI, which passes sp_int args -- default its untyped
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
  /* The evidence for those parameters: what the program passes at every
     DYNAMIC call of a Method value (`.call` / `.()` / `[]` on a receiver
     typed Method or boxed), per position, over the whole program. A
     captured method reached only out of a container cannot be tied to one
     site, so the union over all of them stands in: optcarrot's dispatch
     table is called as `@store[addr][addr, value]` with two Integers, so its
     handlers keep sp_int parameters, while `exports.fetch(name).call(*args)`
     splats a boxed array and widens every position. A position no dynamic
     site reaches keeps the int default it always had. */
  TyKind dyn_arg[16];
  int dyn_seen[16];
  for (int k = 0; k < 16; k++) { dyn_arg[k] = TY_UNKNOWN; dyn_seen[k] = 0; }
  if (msym_n > 0) {
    for (int id = 0; id < c->nt->count; id++) {
      if (nt_kind(c->nt, id) != NK_CallNode) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]"))) continue;
      int r = nt_ref(c->nt, id, "receiver");
      if (r < 0) continue;
      TyKind rt = c->ntype[r];
      if (rt != TY_METHOD && rt != TY_POLY) continue;
      int a = nt_ref(c->nt, id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
      /* `[]` on a boxed value is mostly a Hash or Array read (`h[:k]`,
         `row["name"]`), which is no evidence about a Method: count it only
         with numeric arguments, the shape a dispatch table is called in
         (`@store[addr][addr, value]`). A Method called through `[]` with
         another kind is not seen here and keeps the int default. */
      if (sp_streq(nm, "[]") && rt != TY_METHOD) {
        int numeric = an > 0;
        for (int k = 0; k < an && numeric; k++) {
          TyKind at = c->ntype[av[k]];
          if (at != TY_INT && at != TY_FLOAT) numeric = 0;
        }
        if (!numeric) continue;
      }
      int splat = 0;
      for (int k = 0; k < an; k++) {
        const char *aty = nt_type(c->nt, av[k]);
        if (aty && (sp_streq(aty, "SplatNode") || sp_streq(aty, "BlockArgumentNode") ||
                    sp_streq(aty, "KeywordHashNode") || sp_streq(aty, "ForwardingArgumentsNode"))) splat = 1;
      }
      if (splat) {
        for (int k = 0; k < 16; k++) { dyn_arg[k] = TY_POLY; dyn_seen[k] = 1; }
        continue;
      }
      for (int k = 0; k < an && k < 16; k++) {
        TyKind at = c->ntype[av[k]];
        if (at == TY_UNKNOWN || at == TY_VOID) at = TY_POLY;
        dyn_arg[k] = dyn_seen[k] ? ty_unify(dyn_arg[k], at) : at;
        dyn_seen[k] = 1;
      }
    }
  }
  int msym_pinned = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || !sc->name) continue;
    int taken = 0;
    for (int i = 0; i < msym_n && !taken; i++)
      if (sp_streq(msym_names[i], sc->name)) taken = 1;
    if (taken) {
      /* The bound-Method ABI passes sp_int args, so a param nothing else
         typed is pinned to int rather than left undeclared. The return is
         NOT pinned any more: the bind site stamps the kind of the C return
         and every Method call reads it (SP_BM_RET_POLY takes the sp_RbVal
         cast, d879c8b0 / 21f32341), so a poly-returning target is called
         correctly as it is. Pinning it to int retyped the method for its
         DIRECT callers too -- `ip = obj.ip` read a String's to_i once
         `method(:ip)` appeared anywhere in the program (#4451). An UNKNOWN
         return still defaults to int, as before, so the method is emitted
         with a value rather than as void. */
      /* POLY, not int, for a method whose parameters nothing typed: the
         bound-Method ABI describes each argument's kind and carries the poly
         ones boxed, so it never needed the int guess -- and the guess is
         wrong for every program that puts a Float, a boolean or a hash in
         one. A Float read as an integer is how the bit pattern of NaN came
         to be compared as a number (#4597). This is the judgement #4451
         already made for the RETURN of these same methods.

         A synthesized __bam_ wrapper keeps the int: it is not a user method
         with no evidence but a wrapper around a builtin whose C signature
         the adapter emission fixes, and its bind site stamps a legacy sig to
         match. Widening it left the stamp describing a poly parameter that
         no call site asks for, and the Method stopped being callable at
         all. */
      int is_bam_wrap = sc->name && strncmp(sc->name, "__bam_", 6) == 0;
      for (int i = 0; i < sc->nparams; i++) {
        LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
        if (p && p->type == TY_UNKNOWN) {
          /* the position's evidence (above): Integer-only keeps the int
             lane, anything else rides the boxed channel; a bam wrapper's
             signature is fixed by the adapter emission */
          TyKind ev = (i < 16 && dyn_seen[i]) ? dyn_arg[i] : TY_INT;
          /* A bam wrapper's parameter takes the one kind the sites pass when
             the legacy signature carries it as itself (a String, a Symbol, a
             bool, a typed array): `method(:Integer).call("42")` stamped an
             Integer slot the String argument could never match, and with no
             thunk on a builtin's wrapper the Method was uncallable in the
             default mode (#4785). Mixed or boxed evidence keeps the int
             default: poly is the one kind such a wrapper cannot take here.
             Under --int-overflow=promote the int default is what widens to
             the poly signature every call site there rides, so it stays. */
          if (is_bam_wrap)
            p->type = (!g_promote_mode && (ev == TY_STRING || ev == TY_SYMBOL || ev == TY_BOOL || ty_is_array(ev))) ? ev : TY_INT;
          else
            p->type = ev == TY_INT ? TY_INT : TY_POLY;
          msym_pinned = 1;
        }
      }
      if (sc->ret == TY_UNKNOWN) { sc->ret = TY_INT; msym_pinned = 1; }
    }
  }
  free(msym_names);
  /* A pin here is a type change after the fixpoint: what the body computes
     from that parameter -- an ivar it stores, the return it becomes -- and
     every direct caller of the method still carry the types they had before
     it. `def poke(a, v) = @latch = v.odd? ? "s" : v` had @latch a String
     while v was unknown and a poly once v was an int, and `p pad.peek(0)`
     kept the String reading over a poly-returning callee (the C did not
     compile). Let the writes settle again. */
  if (msym_pinned)
    for (int k = 0; k < 8; k++) { int ch = infer_write_types(c); ch |= infer_return_types(c); if (!ch) break; }


  /* Widen inherited ivars to their union across the hierarchy (see
     propagate_ivars_up). Monotonic, so iterate to a fixpoint. */
  for (int iter = 0; iter < 16; iter++) if (!propagate_ivars_up(c)) break;

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
  /* Re-assert BEFORE each binding round, not after. A seeded parameter is a
     call site of everything its body calls, and this loop is the last place
     those bindings happen: entering it with the seed already lost, the seeded
     caller contributed UNKNOWN and the callee narrowed to whatever its one
     other call site said. `M.wrapper` declared `(untyped)` in an .rbs stopped
     holding `M.callee`'s parameter open, and the .rbs never mentioned callee
     (#4165). The fixpoint's own copy runs after its binding, which is why it
     recovered on the next iteration and this loop never did. */
  for (int it = 0; it < 8; it++) {
    reassert_rbs_param_seeds(c);
    int ch = infer_param_types(c);
    /* A binding here is a type change after the fixpoint too: a parameter
       just typed feeds the callee's locals and its return, and those feed
       the arguments of the calls that follow, so the round is not settled
       until the writes and returns are -- and a round that bound nothing
       may still be carrying a return the previous one changed. Binding
       alone, the second `m64(l3 + l2)` in a method reachable only through
       `method(:f)` kept binding from an UNKNOWN `l3` while the first call
       had typed the parameter int, and codegen then derived the Bignum both
       sides in fact carry and passed it to the int parameter (the C did not
       build, #4684). */
    ch |= infer_write_types(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }
  reassert_rbs_param_seeds(c);

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
      if (p && p->type == TY_UNKNOWN) slot_rule(c, p, TY_POLY, -1, "never bound: no call site gives it a type");
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
  /* Settle the container-flow bits ONCE, on the types the main fixpoint left,
     then let the write/return re-runs below propagate the widening they gate.
     Not inside the fixpoint: a bit arriving mid-run re-types a call after its
     neighbours have settled, and whether it arrived at all depended on node
     order -- a --line-map and a --no-line-map run of the same program then
     disagreed about whether a program compiled at all. Not before it either:
     the evidence is a container-typed producer, which the fixpoint is what
     establishes. (#3459) */
  for (int iter = 0; iter < 8; iter++) if (!infer_container_flow(c)) break;

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
  widen_ivars_read_into_poly(c);
  g_ret_no_new_poly = 1;
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;
  g_ret_no_new_poly = 0;
  reassert_rbs_param_seeds(c);   /* the post-fixpoint passes narrow too */
  /* The returns settled above may have widened past the locals that were
     derived from them (the write re-run ran first, and its `no new poly` gate
     kept a return narrow until now). Reconcile the object slots, whose
     assignment has no coercion to fall back on. */
  for (int iter = 0; iter < 8; iter++) {
    int ch = widen_object_locals_from_poly_writes(c);
    ch |= widen_arrays_from_map_bang(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }
  /* The write-type re-run can re-derive a hash/array container type for an
     iteration-bound block param from its element-index usage (e.g. `a[1]=v`),
     clobbering the TY_POLY the block-param pass pinned for a poly-collection
     `.each`. Re-pin block-param types so poly elements stay poly. */
  for (int iter = 0; iter < 8; iter++) if (!infer_block_params(c)) break;
  intern_block_params(c);   /* a parameter nothing reads still needs its slot */
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
    /* ... and back up: the re-run above can widen a subclass's copy of an
       inherited ivar (a poly-fallen param feeding it), and the up-propagation
       above has long finished. Without this the base keeps the narrower type,
       its struct stops being a prefix of the subclass's, and every inherited
       method writes through the `(sp_Base *)self` cast at the wrong offsets. */
    ch |= propagate_ivars_up(c);
    /* An ivar that widens here (e.g. `@query_log`, whose heterogeneous `= []` /
       `.push(str)` / `= prev` writes merge to poly) must carry its new type into
       any local that merely READS it (`prev = @query_log`). Widen such a local
       to the ivar's type -- monotonically, without the full local re-derivation
       infer_write_types does (which would reset pattern/massign/block-bound
       locals this late and mistype them). Otherwise the local is stranded at its
       pre-widen scalar type: an unsound `sp_StrArray *` <- `sp_RbVal` at
       `local = @ivar` (#1793). */
    ch |= reconcile_locals_reading_ivars(c);
    /* A method whose value IS such an ivar (`def peek(a) = @latch`) still
       carries the return derived before the ivar widened, and its callers
       read a poly through a String (the C did not build, #4451). Re-derive
       the returns on the widened ivars; the gate against a new poly is off,
       since the widening is exactly what the body now says. */
    g_ret_no_new_poly = 2;
    ch |= infer_return_types(c);
    g_ret_no_new_poly = 0;
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
         a raw sp_int carrier (a string is laundered through the slot, an int
         rides it directly), and every call site casts that carrier back to its
         own block's concrete type. Widening the return to poly would emit a
         `sp_box_str(sp_int)` over that raw carrier and mismatch the per-call
         casts; keep it as the carrier, exactly as default/wrap mode does. */
      if (sc->ret == TY_INT && !is_spaceship && !sc->is_lowered_yield) sc->ret = TY_POLY;
      /* the blockless answer of an `if block_given?` tail (#4659) widens
         with the rest: its else arm is emitted on the widened locals */
      if (sc->ret_noblock == TY_INT && !is_spaceship && !sc->is_lowered_yield) sc->ret_noblock = TY_POLY;
      /* The receiver parameter of a synthesized `<Integer>.method(:sym)`
         wrapper (`__bam_N(__bam_r, ...) = __bam_r.sym(...)`) stays typed as
         well: its bind site stores the receiver raw and the thunk ABI
         (bm_self_ctype) declares the slot sp_int only while the parameter IS
         TY_INT, so a widened one had the callee read a 16-byte box out of a
         pointer-sized slot -- junk cls_id, and every call ended in
         NoMethodError or a TypeError naming an empty class (#4765). The
         same treatment as a block parameter; the wrapper's own arithmetic on
         it follows the typed contract. A boxed self slot in the thunk ABI
         would be the fuller form. A receiverless Kernel wrapper
         (`method(:String)`, `__bam_r` is its first ARGUMENT) widens with the
         rest: its poly signature is what a promote call site rides. */
      int is_bam_recv = bam_wrapper_binds_receiver(c, sc);
      for (int i = 0; i < sc->nlocals; i++) {
        /* Skip block params: they are typed by the iterated collection's
           element type (an IntArray yields int elements), and the block
           emitters already retype the param to that element type for the body
           (use_shadow). Widening them to poly only creates an int/poly shadow
           and inconsistent reads (e.g. `x.even?` keeps the stale poly type
           while `x*2` sees the retyped int). Method params still widen. */
        if (sc->locals[i].is_block_param) continue;
        if (is_bam_recv && sc->locals[i].name && sp_streq(sc->locals[i].name, "__bam_r")) continue;
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
  narrow_int_table_ivars(c);
  narrow_object_arrays(c);
  narrow_locals_from_arrays(c);
  /* An --rbs `Array[Class]` ivar seed the pass could not honour is said so,
     rather than dropped without a word (#4444): the array is used in a way
     its unboxed form has no emitter for, or read from outside the class's
     own instance methods. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++) {
      int req = cl->ivar_oa_seed[iv];
      if (!req) continue;
      TyKind got = cl->ivar_types[iv];
      const char *asked;
      if (req == SEED_OA_INT_TABLE)      { if (got == TY_INT_ARRAY_ARRAY) continue; asked = "Array[Integer]"; }
      else if (req == SEED_OA_FLT_TABLE) { if (got == TY_FLOAT_ARRAY_ARRAY) continue; asked = "Array[Float]"; }
      else {
        if (ty_is_obj_array(got)) continue;
        int want = req - 1;
        asked = want >= 0 && want < c->nclasses ? c->classes[want].name : "?";
      }
      /* A nested request can fail two ways, and they want different advice.
         The slot may have stayed boxed because a use has no emitter in the
         unboxed form, or because a row of the OTHER kind met the seed in the
         element join. The second is not a missing emitter at all but the
         signature and the code disagreeing about the element type, and it is
         the same contradiction a flat seed reports (a seed is trusted, so the
         emitted table would hand a row of one layout to a reader of the
         other): refused like one, not warned about (#4484). */
      if (req < 0 && (cl->ivar_oa_conflict[iv] || ty_is_ptr_array(got))) {
        fprintf(stderr,
                "spinel: --rbs seed contradicted: %s %s is declared Array[%s] but the program "
                "gives it rows of another kind\n"
                "  A seed is trusted, so the emitted table would hand a row of one layout to "
                "a reader of the other.\n"
                "  Fix the signature or the rows.\n",
                cl->name, cl->ivars[iv], asked);
        if (!collect_mode()) exit(1);
        g_seed_bad = 1;
        continue;
      }
      fprintf(stderr, "warning: --rbs: %s %s: Array[%s] stays a boxed array; every use of it must be "
                      "one the unboxed array supports ([], []=, push, length, empty?, first, last, "
                      "min, max, sort) from the class's own instance methods or its attr_reader\n",
              cl->name, cl->ivars[iv], asked);
    }
  }

  /* narrow poly locals that are only ever used as ints (drop per-use boxing);
     the rebuild below re-infers their reads/ops at the narrowed int type. */
  narrow_poly_int_locals(c);

  /* finalize: gc-root needs + full node type cache */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++)
      c->scopes[s].locals[i].gc_root = (c->scopes[s].locals[i].type == TY_STRING);

  mark_empty_array_operands(c);
  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);

  /* --int-overflow=promote: the widen above can change a proc body's return
     type (a captured int local widened to poly), so a proc's caller-side
     proc_ret / a factory method's ret_proc_ret must be re-derived from the
     now-widened body -- else a `.call` reads the wrong return channel (raw
     sp_int slot vs the poly side-channel) and yields 0. Re-run JUST the
     proc_ret / ret_proc_ret derivations (mirroring infer_return_types and
     infer_write_types) as a focused fixpoint; this touches only proc-return
     metadata, never the widened slot types, so it cannot undo the widen.
     A factory's ret_proc_ret feeds a caller's proc_ret, hence the loop. */
  /* Not only in promote mode. A proc's parameter is typed from its `.call`
     sites, and a parameter that settles LATE -- an empty-array local argument
     types it only in the post-convergence pass -- leaves proc_ret derived from
     the body as it read before, so `->(acc){ acc + [1] }.call(e)` with
     `e = []` answered an Integer where the body plainly builds an array
     (#4296). The re-derivation below is the same one, and it only touches
     proc-return metadata. */
  {
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
        /* infer_type, not the cache: a local the steps below widened in this
           same pass reads as its new kind only through a fresh inference. A
           tail that is `if block_given? ... else ... end` answers through its
           block arm: that arm is what a call with a block receives, and the
           if's own type unifies it with the blockless Enumerator arm. */
        if (sc->body >= 0) {
          int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn);
          int tail = bn > 0 ? bb[bn - 1] : -1;
          for (int guard = 0; tail >= 0 && guard < 4 && nt_kind(nt, tail) == NK_IfNode; guard++) {
            int pred = nt_ref(nt, tail, "predicate");
            const char *pnm = pred >= 0 && nt_kind(nt, pred) == NK_CallNode ? nt_str(nt, pred, "name") : NULL;
            if (!pnm || !sp_streq(pnm, "block_given?")) break;
            int st = nt_ref(nt, tail, "statements");
            int sn = 0; const int *sb = st >= 0 ? nt_arr(nt, st, "body", &sn) : NULL;
            tail = sn > 0 ? sb[sn - 1] : -1;
          }
          if (tail >= 0) br = infer_type(c, tail);
        }
        /* the ReturnNodes, not every node: this ran per scope (roundhouse#72) */
        for (int id = comp_kind_first(c, NK_ReturnNode); id >= 0 && br != TY_POLY_ARRAY; id = comp_kind_next(c, id)) {
          if (nt_kind(nt, id) == NK_ReturnNode && comp_scope_of(c, id) == sc) {
            int a = nt_ref(nt, id, "arguments"); int an = 0;
            const int *av = a >= 0 ? nt_arr(nt, id, "arguments", &an) : NULL;
            if (an == 1 && infer_type(c, av[0]) == TY_POLY_ARRAY) br = TY_POLY_ARRAY;
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
      /* (4b) ...and a local written from a `<proc>.call(...)`, whose slot was
         typed from proc_ret as it read BEFORE the re-derivation in (2) above.
         A return that widened there left the slot behind: `r = g.call(e)` kept
         its sp_IntArray * while the call answers the boxed poly now, and the C
         compiler refused the assignment (#4330). Unify rather than assign, so
         this only ever widens -- the discipline the whole block keeps. */
      for (int id = 0; id < nt->count; id++) {
        if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
        int vnode = nt_ref(nt, id, "value");
        if (vnode < 0 || nt_kind(nt, vnode) != NK_CallNode) continue;
        const char *cn = nt_str(nt, vnode, "name");
        if (!cn || !(sp_streq(cn, "call") || sp_streq(cn, "()") || sp_streq(cn, "[]"))) continue;
        int crecv = nt_ref(nt, vnode, "receiver");
        if (crecv < 0 || infer_type(c, crecv) != TY_PROC) continue;
        const char *nm = nt_str(nt, id, "name");
        LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
        if (!lv || lv->type == TY_UNKNOWN) continue;
        /* the CALL NODE's own type, not proc_call_ret: that answers poly for
           an unknowable return (a `&blk` param's), which would widen a slot
           the call-site block types concretely. The node is what the emitter
           produces, so it is what the slot has to hold. */
        TyKind pr = infer_type(c, vnode);
        if (pr == TY_UNKNOWN || pr == lv->type) continue;
        TyKind u = ty_unify(lv->type, pr);
        if (u != lv->type) { lv->type = u; changed = 1; }
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
      /* (6)-(10), --int-overflow=promote only: the slots the widening leaves
         behind. A producer of an Integer array that reads a widened slot --
         `[x, x * 2]`, `[x]`, a map over them -- is a poly array in the final
         types, while the slot it flows into was decided as an Integer array
         during the fixpoint and, unlike a plain local (step 4), was re-derived
         by nothing afterwards: an ivar, a global, a multiple assignment's
         targets, a callee's parameter, and a local pinned to a container's
         element kind whose container has since widened. Each is widened here
         in the same direction step 4 widens, with the array-aware join the
         ivar write merge already uses (a typed array meets a poly array as
         the poly ARRAY, not the poly scalar ty_unify answers); a typed-array
         slot fed a boxed element becomes the box. Default mode never reaches
         these: its fixpoint saw every producer as it is (#4738). */
      if (g_promote_mode) {
        /* the join: what a slot of kind `cur` becomes when fed `val` */
        /* the flat scalar arrays only: a table (Array[Array[Integer]]) and an
           object array are the narrowing passes' kinds, re-derived by them */
        #define PW_TYPED_ARR(t) ((t) == TY_INT_ARRAY || (t) == TY_STR_ARRAY || (t) == TY_FLOAT_ARRAY)
        #define PW_JOIN(cur, val) (PW_TYPED_ARR(cur) && (val) == TY_POLY_ARRAY ? TY_POLY_ARRAY \
                                   : PW_TYPED_ARR(cur) && (val) == TY_POLY ? TY_POLY : (cur))
        /* (6) an ivar / cvar written a poly array or a boxed element */
        for (int id = 0; id < nt->count; id++) {
          NodeKind k = nt_kind(nt, id);
          int iv_write = k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
                         k == NK_InstanceVariableAndWriteNode;
          int cv_write = k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode;
          if (!iv_write && !cv_write) continue;
          int vnode = nt_ref(nt, id, "value");
          if (vnode < 0) continue;
          const char *nm = nt_str(nt, id, "name");
          if (!nm) continue;
          TyKind vt = infer_type(c, vnode);
          if (vt != TY_POLY_ARRAY && vt != TY_POLY) continue;
          if (iv_write) {
            int cid = an_ivar_owner(c, id);
            int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
            if (iv < 0) continue;
            TyKind cur = c->classes[cid].ivar_types[iv], nw = PW_JOIN(cur, vt);
            if (nw != cur) { c->classes[cid].ivar_types[iv] = nw; changed = 1; }
          }
          else {
            Scope *ws = comp_scope_of(c, id);
            int cid = ws ? ws->class_id : -1;
            if (cid < 0) continue;
            ClassInfo *ci = &c->classes[cid];
            for (int cv = 0; cv < ci->ncvars; cv++) {
              if (!ci->cvars[cv] || !sp_streq(ci->cvars[cv], nm)) continue;
              TyKind cur = ci->cvar_types[cv], nw = PW_JOIN(cur, vt);
              if (nw != cur) { ci->cvar_types[cv] = nw; changed = 1; }
            }
          }
        }
        /* (7) a global written a poly array or a boxed element */
        for (int id = 0; id < nt->count; id++) {
          if (nt_kind(nt, id) != NK_GlobalVariableWriteNode && nt_kind(nt, id) != NK_GlobalVariableOrWriteNode) continue;
          int vnode = nt_ref(nt, id, "value");
          const char *nm = nt_str(nt, id, "name");
          if (vnode < 0 || !nm) continue;
          LocalVar *gv = comp_gvar(c, nm[0] == '$' ? nm + 1 : nm);   /* keyed without the `$` */
          if (!gv) continue;
          TyKind vt = infer_type(c, vnode), nw = PW_JOIN(gv->type, vt);
          if (nw != gv->type) { gv->type = nw; changed = 1; }
        }
        /* (8) a multiple assignment: a splat target collects a poly array
           from a boxed scalar or a poly-array source, and a plain target
           takes a boxed element out of a poly-array source */
        for (int id = 0; id < nt->count; id++) {
          if (nt_kind(nt, id) != NK_MultiWriteNode) continue;
          int value = nt_ref(nt, id, "value");
          if (value < 0) continue;
          Scope *ms = comp_scope_of(c, id);
          if (!ms) continue;
          /* a tuple right-hand side (`a, b = [], [x]`) pairs element with
             target: each target follows its own element */
          if (nt_kind(nt, value) == NK_ArrayNode) {
            int en = 0; const int *el = nt_arr(nt, value, "elements", &en);
            int ln0 = 0; const int *lf = nt_arr(nt, id, "lefts", &ln0);
            for (int i = 0; i < ln0 && i < en; i++) {
              if (nt_kind(nt, lf[i]) != NK_LocalVariableTargetNode) continue;
              const char *lnm = nt_str(nt, lf[i], "name");
              LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
              if (!lv) continue;
              TyKind et = infer_type(c, el[i]), nw = PW_JOIN(lv->type, et);
              if (nw != lv->type) { lv->type = nw; changed = 1; }
            }
            continue;
          }
          TyKind st = infer_type(c, value);
          if (st != TY_POLY && st != TY_POLY_ARRAY) continue;
          int rest = nt_ref(nt, id, "rest");
          if (rest >= 0 && nt_kind(nt, rest) == NK_SplatNode) {
            int rin = nt_ref(nt, rest, "expression");
            const char *rnm = rin >= 0 ? nt_str(nt, rin, "name") : NULL;
            LocalVar *rlv = rnm ? scope_local(ms, rnm) : NULL;
            if (rlv && PW_TYPED_ARR(rlv->type)) { rlv->type = TY_POLY_ARRAY; changed = 1; }
          }
          if (st != TY_POLY_ARRAY) continue;
          int ln = 0; const int *lefts = nt_arr(nt, id, "lefts", &ln);
          for (int i = 0; i < ln; i++) {
            if (nt_kind(nt, lefts[i]) != NK_LocalVariableTargetNode) continue;
            const char *lnm = nt_str(nt, lefts[i], "name");
            LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
            if (lv && PW_TYPED_ARR(lv->type)) { lv->type = TY_POLY; changed = 1; }
          }
        }
        /* (9) a parameter bound from an argument that is a poly array now
           (a clone's receiver parameter included: the rewrite passes the
           receiver as the first argument) */
        for (int s = 1; s < c->nscopes; s++) {
          Scope *sc = &c->scopes[s];
          if (!sc->name || sc->nparams <= 0) continue;
          for (int u = comp_kind_first(c, NK_CallNode); u >= 0; u = comp_kind_next(c, u)) {
            if (nt_kind(nt, u) != NK_CallNode) continue;
            if (!an_call_targets_scope(c, u, s, sc)) continue;
            int a = nt_ref(nt, u, "arguments"); int an = 0;
            const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
            for (int j = 0; j < an && j < sc->nparams; j++) {
              if (!sc->pnames[j]) continue;
              LocalVar *pv = scope_local(sc, sc->pnames[j]);
              if (!pv || !PW_TYPED_ARR(pv->type)) continue;
              /* an empty literal argument is built at the parameter's kind
                 and says nothing about it */
              if (nt_kind(nt, av[j]) == NK_ArrayNode) {
                int en0 = 0; nt_arr(nt, av[j], "elements", &en0);
                if (en0 == 0) continue;
              }
              if (infer_type(c, av[j]) != TY_POLY_ARRAY) continue;
              /* a parameter the callee mutates in place is passed by
                 reference at its kind; widened, the argument would be copied
                 into it and the mutation lost (the #4480 rule), and the
                 emitter refuses that shape outright */
              if (pw_scope_mutates_param(c, s, sc->pnames[j])) continue;
              pv->type = TY_POLY_ARRAY; changed = 1;
            }
          }
        }
        /* (10) a local pinned to a container's element kind whose container
           has widened: the read hands it a box now, so the pin no longer
           holds and the slot takes the box (int_array_array's `row = t[3]`) */
        for (int id = 0; id < nt->count; id++) {
          NodeKind k = nt_kind(nt, id);
          if (k != NK_LocalVariableWriteNode && k != NK_LocalVariableOrWriteNode) continue;
          const char *nm = nt_str(nt, id, "name");
          LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
          if (!lv || !PW_TYPED_ARR(lv->type)) continue;
          int vnode = nt_ref(nt, id, "value");
          if (vnode < 0) continue;
          TyKind vt = infer_type(c, vnode);
          if (vt == TY_POLY_ARRAY) { lv->type = TY_POLY_ARRAY; changed = 1; }   /* step 4's rule, for `||=` */
          else if (vt == TY_POLY) { lv->type = TY_POLY; lv->oa_pin = TY_UNKNOWN; changed = 1; }
        }
        /* (11) a block over a receiver that is a poly array now: the params
           the fixpoint typed from the receiver's Integer elements are bound
           from boxed elements (the widening skips block params on purpose,
           since the emitters retype them from the receiver -- but from the
           receiver as the fixpoint saw it). Every scalar-typed param takes the
           box, except an each_with_index / with_index index, which is the
           emitter's own counter; each_cons / each_slice hand their first
           param a slice of the receiver, a poly array. */
        for (int u = comp_kind_first(c, NK_CallNode); u >= 0; u = comp_kind_next(c, u)) {
          if (nt_kind(nt, u) != NK_CallNode) continue;
          int blk = nt_ref(nt, u, "block");
          if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
          int recv = nt_ref(nt, u, "receiver");
          const char *inm = nt_str(nt, u, "name");
          if (!inm) continue;
          /* a Ruby-defined builtin's call, rewritten receiverless with the
             receiver as its first argument (desugar_builtin_enum_calls) */
          if (recv < 0 && strncmp(inm, "__enum_", 7) == 0) {
            int a = nt_ref(nt, u, "arguments"); int an = 0;
            const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
            if (an >= 1) recv = av[0];
          }
          int slice_first = sp_streq(inm, "each_cons") || sp_streq(inm, "each_slice");
          int idx_second = sp_streq(inm, "each_with_index") || sp_streq(inm, "with_index");
          /* only an iterator whose block receives ELEMENTS: `fetch(i) { |i| }`
             hands its block the index, `each_with_index` its second param */
          if (!slice_first && !strbuf_elem_first_iterator(inm) &&
              !sp_streq(inm, "inject") && !sp_streq(inm, "reduce") &&
              !sp_streq(inm, "min") && !sp_streq(inm, "max") && !sp_streq(inm, "minmax") &&
              !sp_streq(inm, "sort") && !sp_streq(inm, "sum") &&
              !(strncmp(inm, "__enum_", 7) == 0)) continue;
          /* `arr.each_cons(2).with_index(1).map { |(x, y), i| }`: the block
             sits on a chain; walk it down to the each_cons / each_slice whose
             receiver is the array, and the first param is its slice */
          for (int guard = 0; recv >= 0 && guard < 6 && nt_kind(nt, recv) == NK_CallNode &&
                              nt_ref(nt, recv, "block") < 0; guard++) {
            const char *cn = nt_str(nt, recv, "name");
            if (!cn) break;
            if (sp_streq(cn, "each_cons") || sp_streq(cn, "each_slice")) {
              slice_first = 1; recv = nt_ref(nt, recv, "receiver"); break;
            }
            if (sp_streq(cn, "with_index") || sp_streq(cn, "each_with_index")) idx_second = 1;
            else if (!(sp_streq(cn, "each") || sp_streq(cn, "with_object") || sp_streq(cn, "lazy") ||
                       sp_streq(cn, "each_entry"))) break;
            recv = nt_ref(nt, recv, "receiver");
          }
          if (recv < 0 || infer_type(c, recv) != TY_POLY_ARRAY) continue;
          Scope *bs = comp_scope_of(c, blk);
          if (!bs) continue;
          for (int k = 0; k < 8; k++) {
            const char *pnm = block_param_name(c, blk, k);
            if (!pnm) break;
            if (idx_second && k == 1) continue;
            LocalVar *pv = scope_local(bs, pnm);
            if (!pv) continue;
            if (slice_first && k == 0) {
              if (PW_TYPED_ARR(pv->type)) { pv->type = TY_POLY_ARRAY; changed = 1; }
              continue;
            }
            if (pv->type == TY_INT || pv->type == TY_FLOAT || pv->type == TY_STRING)
              { pv->type = TY_POLY; changed = 1; }
          }
        }
        /* (12) a typed array local or ivar pushed a boxed value: the push's
           element evidence widened with the slot it reads, and the array has
           to hold the box (a Bignum among them is the mode's whole point) */
        for (int u = comp_kind_first(c, NK_CallNode); u >= 0; u = comp_kind_next(c, u)) {
          if (nt_kind(nt, u) != NK_CallNode) continue;
          const char *pn = nt_str(nt, u, "name");
          if (!pn || !(sp_streq(pn, "<<") || sp_streq(pn, "push") || sp_streq(pn, "append") ||
                       sp_streq(pn, "unshift"))) continue;
          int recv = nt_ref(nt, u, "receiver");
          if (recv < 0) continue;
          int a = nt_ref(nt, u, "arguments"); int an = 0;
          const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
          int boxed = 0;
          for (int j = 0; j < an; j++) if (infer_type(c, av[j]) == TY_POLY) boxed = 1;
          if (!boxed) continue;
          NodeKind rk = nt_kind(nt, recv);
          if (rk == NK_LocalVariableReadNode) {
            const char *rn = nt_str(nt, recv, "name");
            LocalVar *lv = rn ? scope_local(comp_scope_of(c, recv), rn) : NULL;
            /* a parameter is the caller's array by reference and a block
               parameter is bound by its iterator: both keep their kind, and
               the push unboxes (raising on a foreign element, as the typed
               store always has) */
            if (!lv || lv->is_param || lv->is_block_param ||
                !(lv->type == TY_INT_ARRAY || lv->type == TY_FLOAT_ARRAY || lv->type == TY_STR_ARRAY))
              continue;
            /* and only an array the local BUILT: bound from an element or an
               ivar read it is another name for storage something else holds,
               and a widened slot would take a converted copy, so the pushes
               would no longer reach the container (#4412) */
            if (!pw_local_owns_array(c, recv, rn)) continue;
            lv->type = TY_POLY_ARRAY; changed = 1;
          }
          else if (rk == NK_InstanceVariableReadNode) {
            const char *rn = nt_str(nt, recv, "name");
            int cid = rn ? an_ivar_owner(c, recv) : -1;
            int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], rn) : -1;
            if (iv >= 0) {
              TyKind cur = c->classes[cid].ivar_types[iv];
              if (cur == TY_INT_ARRAY || cur == TY_FLOAT_ARRAY || cur == TY_STR_ARRAY)
                { c->classes[cid].ivar_types[iv] = TY_POLY_ARRAY; changed = 1; }
            }
          }
        }
        /* (13) a splice into a typed array from a source that is a poly array
           now -- an object whose #to_ary builds its array out of widened
           ivars (#4764), or a poly array outright: the fold of the splice
           source into the receiver's element kind ran inside the fixpoint,
           before the widening. The receiver, an array the local built, takes
           the poly kind, whose splice arm reads any array source. */
        for (int u = comp_kind_first(c, NK_CallNode); u >= 0; u = comp_kind_next(c, u)) {
          if (nt_kind(nt, u) != NK_CallNode) continue;
          const char *pn = nt_str(nt, u, "name");
          if (!pn || !sp_streq(pn, "[]=")) continue;
          int a = nt_ref(nt, u, "arguments"); int an = 0;
          const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
          if (an < 2) continue;
          int recv = nt_ref(nt, u, "receiver");
          if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
          /* a splice: two index arguments, or a Range index */
          if (an == 2 && infer_type(c, av[0]) != TY_RANGE) continue;
          int val = av[an - 1];
          TyKind vt = infer_type(c, val), src = vt;
          if (ty_is_object(vt)) {
            int tmi = comp_method_in_chain(c, ty_object_class(vt), "to_ary", NULL);
            src = tmi >= 0 ? (TyKind)c->scopes[tmi].ret : TY_UNKNOWN;
          }
          if (src != TY_POLY_ARRAY) continue;
          const char *rn = nt_str(nt, recv, "name");
          LocalVar *lv = rn ? scope_local(comp_scope_of(c, recv), rn) : NULL;
          if (!lv || lv->is_param || lv->is_block_param || !PW_TYPED_ARR(lv->type)) continue;
          if (!pw_local_owns_array(c, recv, rn)) continue;
          lv->type = TY_POLY_ARRAY; changed = 1;
        }
        #undef PW_JOIN
        #undef PW_TYPED_ARR
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
    int cls2 = ty_is_object(rt2) ? ty_object_class(rt2) : ie_poly_mark(c, id, rt2);
    if (cls2 < 0) continue;
    int is_ie2 = sp_streq(nm2, "instance_eval") || sp_streq(nm2, "instance_exec");
    if (is_ie2) {
      if (comp_method_in_chain(c, cls2, nm2, NULL) >= 0) continue;
    }
    else if (!comp_trampoline_kind(c, cls2, nm2, NULL)) continue;
    int bdy2 = nt_ref(c->nt, blk2, "body");
    if (bdy2 < 0) continue;
    int bn2 = 0; const int *bb2 = nt_arr(c->nt, bdy2, "body", &bn2);
    if (bn2 <= 0 || !bb2) continue;
    int saved2 = an_ie_class_id;
    an_ie_class_id = cls2;
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
     to the handle representation, cascading through transitive passes. A
     parameter RETAINED in a shared-handle ivar demands the handle for the
     same reason and feeds the same propagation, so the two run to a joint
     fixpoint rather than one after the other (#4363). */
  for (;;) {
    HandleArgTab hat; handle_arg_tab_init(c, &hat);
    int ch = promote_params_stored_in_shared_ivars(c, &hat);
    if (convert_byref_handle_params(c, &hat)) ch = 1;
    handle_arg_tab_free(&hat);
    if (!ch) break;
  }
  mark_reader_identity_operands(c);

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
      if (nt_int(c->nt, wv, "fzl", 0)) frozen_literal_write = 1;
    }
    if (!saw_write || !all_literal_writes) continue;
    if (frozen_literal_write) {
      /* Every value this local can hold is a frozen literal, and this `<<`
         will raise FrozenError on every execution -- at run time, deep in
         whatever the program was doing. `text = ""; text << line` is the
         most common accumulator idiom in Ruby, and spinel's
         frozen-by-default literals (docs/limitations.md) are exactly the
         wall it walks into -- so say so at compile time, once per local,
         with the escape hatch named (#4207). */
      int first_shl = 1;
      for (int p2 = 0; p2 < id && first_shl; p2++) {
        if (nt_kind(c->nt, p2) != NK_CallNode) continue;
        const char *pn2 = nt_str(c->nt, p2, "name");
        int pr2 = nt_ref(c->nt, p2, "receiver");
        if (pn2 && sp_streq(pn2, "<<") && pr2 >= 0 &&
            nt_type(c->nt, pr2) && sp_streq(nt_type(c->nt, pr2), "LocalVariableReadNode") &&
            nt_str(c->nt, pr2, "name") && sp_streq(nt_str(c->nt, pr2, "name"), vn) &&
            comp_scope_of(c, pr2) == s) first_shl = 0;
      }
      if (first_shl) {
        int wl = (int)nt_int(c->nt, id, "node_line", 0);
        int wf = (int)nt_int(c->nt, id, "node_file", 0);
        const char *wpath = nt_file_path(c->nt, wf);
        if (!wpath || !*wpath) wpath = c->nt->source_file;
        char wpos[1200];
        if (wpath && *wpath && wl > 0) snprintf(wpos, sizeof wpos, "%s:%d: ", wpath, wl);
        else if (wpath && *wpath) snprintf(wpos, sizeof wpos, "%s: ", wpath);
        else wpos[0] = 0;
        fprintf(stderr,
                "%swarning: `%s` only ever holds frozen string literals, so this "
                "`<<` raises FrozenError at run time (string literals are frozen in "
                "spinel; build a mutable string with +\"...\", String.new, or "
                "interpolation -- see docs/limitations.md)\n",
                wpos, vn);
      }
      continue;
    }
    /* A captured-and-celled string stays TY_STRING so it rides a typed-pointer
       cell: `<<` then reassigns through the cell (*_cell = concat(...)), which
       propagates to the enclosing scope. A STRBUF's in-place buffer + read-time
       demotion don't fit the cell's stable-pointer model. (After the warning
       above: a celled accumulator raises the same FrozenError, #4207's own
       repro appends from inside a Thread block.) */
    if (lv->is_cell) continue;
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
      /* the group's answer, not the unique one: see an_any_scope_by_name */
      int mi = an_any_scope_by_name(c, nt_str(c->nt, u, "name"));
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
      if ((lv->str_shared || lv->str_append) &&
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
      /* the same mutation one call away: `helper(@s, x)` where the helper
         appends to its parameter writes through the slot's address, and a
         by-value receiver would hand it the address of a copy */
      if (t == TY_STRING && an_ivar_passed_as_arg(c, i, ci->ivars[j])) { scalar = 0; break; }
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


  int widened_a_read = 0;
  /* Final safety pass: a hash-typed LOCAL assigned from a plain TY_POLY
     value, or from a reader whose backing ivar finished on a different
     type, cannot assume its variant -- codegen would cast the boxed
     pointer to the inferred variant and index a different struct layout
     (doom's `opts = @menu.options`: a PolyPolyHash_get over the actual
     SymPolyHash object segfaulted in respawn_player). Runs LAST so it
     sees final ivar types; widens the local to TY_POLY and repoints the
     node-type cache so subscripts go through the runtime dispatch.
     Rational and Complex need the same reconciliation: they are by-value
     structs, so a local that kept the value type while the slot finished
     poly is a C type error rather than a bad cast, and the program does not
     build at all (#3500). */
  for (int id = 0; id < c->nt->count; id++) {
    const char *nty = nt_type(c->nt, id);
    if (!nty || !sp_streq(nty, "LocalVariableWriteNode")) continue;
    const char *nm = nt_str(c->nt, id, "name");
    Scope *lsc = comp_scope_of(c, id);
    LocalVar *lv = nm && lsc ? scope_local(lsc, nm) : NULL;
    int val_struct = (lv && (lv->type == TY_RATIONAL || lv->type == TY_COMPLEX));
    if (!lv || (!ty_is_hash(lv->type) && !val_struct)) continue;
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
        if (ty_is_hash(c->ntype[nid2]) || c->ntype[nid2] == TY_RATIONAL ||
            c->ntype[nid2] == TY_COMPLEX) c->ntype[nid2] = TY_POLY;
        widened_a_read = 1;
      }
    }
  }

  /* The reads are repointed above, but what CONSUMED them still carries the
     type it was inferred with. An arithmetic operator on a poly receiver
     lowers to sp_poly_<op>, whose value is boxed, so a node still typed
     Rational or Complex from before the widening met the emission at its
     consumer and the C did not compile. Bring those in line with the rule
     inference already states for a poly receiver. Iterated: a chain widens one
     link at a time. */
  if (widened_a_read) {
    for (int iter = 0; iter < 16; iter++) {
      int ch2 = 0;
      NT_FOREACH_KIND(c->nt, NK_CallNode, id) {
        const char *nm = nt_str(c->nt, id, "name");
        if (!nm || !is_arith_op(nm)) continue;
        int r2 = nt_ref(c->nt, id, "receiver");
        int a2 = nt_ref(c->nt, id, "arguments");
        int an2 = 0; if (a2 >= 0) nt_arr(c->nt, a2, "arguments", &an2);
        if (r2 < 0 || an2 != 1) continue;
        if (c->ntype[r2] != TY_POLY || c->ntype[id] == TY_POLY) continue;
        if (c->ntype[id] == TY_UNKNOWN || c->ntype[id] == TY_VOID) continue;
        c->ntype[id] = TY_POLY; ch2 = 1;
      }
      if (!ch2) break;
    }
  }

  /* A self-returning Array mutator answers its receiver, so a local capturing
     one has the receiver's type -- including a widening the receiver took
     after this local was first typed. `a = [1, 2]; c = a.push(:x)` widens a to
     a poly array and left c an int array, so the capture read the same object
     at the wrong layout and printed raw memory. Same shape as the ivar reader
     below, and the same fix: run last, when the receiver's type is final. */
  {
    /* Only the mutators that ALWAYS answer the receiver. select!/reject!/
       uniq!/compact!/flatten! answer nil when nothing changed, so their
       capture is nilable and does not take the receiver's type. */
    static const char *self_ret_mut[] = {
      "push", "unshift", "<<", "concat", "insert", "append", "prepend",
      "clear", "replace", "fill", "sort!", "reverse!", "shuffle!", "rotate!",
      "map!", "collect!", "keep_if", "delete_if", NULL
    };
    NT_FOREACH_KIND(c->nt, NK_LocalVariableWriteNode, id) {
      int v = nt_ref(c->nt, id, "value");
      if (v >= 0) v = unwrap_parens(c, v);      /* `c = (a << x)` */
      if (v < 0 || nt_kind(c->nt, v) != NK_CallNode) continue;
      const char *cn = nt_str(c->nt, v, "name");
      if (!cn) continue;
      int hit = 0;
      for (int k = 0; self_ret_mut[k]; k++)
        if (sp_streq(cn, self_ret_mut[k])) { hit = 1; break; }
      if (!hit) continue;
      int r = nt_ref(c->nt, v, "receiver");
      if (r < 0 || nt_kind(c->nt, r) != NK_LocalVariableReadNode) continue;
      Scope *sc2 = comp_scope_of(c, id);
      const char *rn = nt_str(c->nt, r, "name");
      const char *wn = nt_str(c->nt, id, "name");
      if (!sc2 || !rn || !wn) continue;
      LocalVar *rv = scope_local(sc2, rn);
      LocalVar *wv = scope_local(sc2, wn);
      if (!rv || !wv || rv->type == wv->type) continue;
      if (!ty_is_array(rv->type) && rv->type != TY_POLY) continue;
      /* a local already unified to poly holds other kinds too
         (`r = x.pop; r = x.concat([3])`); it stays poly */
      if (!ty_is_array(wv->type)) continue;
      wv->type = rv->type;
      c->ntype[v] = rv->type;
      NT_FOREACH_KIND(c->nt, NK_LocalVariableReadNode, rid) {
        const char *n2 = nt_str(c->nt, rid, "name");
        if (!n2 || !sp_streq(n2, wn) || comp_scope_of(c, rid) != sc2) continue;
        c->ntype[rid] = rv->type;
      }
    }
  }

  /* A method whose value is an instance variable read has to answer that
     slot's FINAL type. The return fixpoint settles before the ivar types do,
     so a slot seeded `@a = []` and later widened by the explicit
     `@a = @a + [x]` form leaves its reader pinned to the seed's layout, and
     codegen then casts the boxed slot to it -- `(sp_IntArray *)(self->iv_a).v.p`
     reads an sp_RbVal at the wrong layout and every element comes back as its
     raw bits (#3514). Only widening toward POLY, so a narrowing the passes
     above established is never undone. */
  /* Rounds, because the widening propagates: a method whose value IS one of
     these calls answers the widened type too, and so does its own caller. One
     pass left `def m = self.class.key` declared sp_sym while the dispatch it
     returns had become sp_RbVal, which does not compile (#4053). */
  char *wnode = calloc((size_t)c->nt->count, 1);
  for (int wround = 0; wround < 8; wround++) {
  int wdid = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret == TY_POLY || sc->ret == TY_UNKNOWN || sc->ret == TY_VOID) continue;
    if (sc->class_id < 0 || sc->class_id >= c->nclasses || sc->body < 0) continue;
    int bn = 0; const int *bb = nt_arr(c->nt, sc->body, "body", &bn);
    if (!bb || bn < 1) continue;
    int tail = bb[bn - 1];
    const char *tt = nt_type(c->nt, tail);
    if (!tt) continue;
    if (sp_streq(tt, "InstanceVariableReadNode")) {
      const char *ivn = nt_str(c->nt, tail, "name");
      int iv = ivn ? comp_ivar_index(&c->classes[sc->class_id], ivn) : -1;
      if (iv < 0 || c->classes[sc->class_id].ivar_types[iv] != TY_POLY) continue;
    }
    /* the value is a call THIS pass widened: the method answers it. Only a
       call this pass touched -- a tail call that was already poly says nothing
       about a return the passes above deliberately narrowed, and an --rbs seed
       pinned the return on purpose. */
    else if (!(wnode && sp_streq(tt, "CallNode") && wnode[tail])) continue;
    if (sc->ret_rbs_seeded) continue;
    sc->ret = TY_POLY;
    wdid = 1;
    c->ntype[tail] = TY_POLY;
    if (c->ntype[sc->body] != TY_UNKNOWN) c->ntype[sc->body] = TY_POLY;
    /* the callers were typed from the old return, and a call still carrying
       the container type hands the boxed value to a typed helper, which does
       not compile */
    NT_FOREACH_KIND(c->nt, NK_CallNode, cid) {
      const char *cn = nt_str(c->nt, cid, "name");
      if (!cn || !sc->name || !sp_streq(cn, sc->name)) continue;
      int recv = nt_ref(c->nt, cid, "receiver");
      int args = nt_ref(c->nt, cid, "arguments");
      int ac = 0; if (args >= 0) nt_arr(c->nt, args, "arguments", &ac);
      if (recv < 0 || ac != 0 || nt_ref(c->nt, cid, "block") >= 0) continue;
      TyKind rt = c->ntype[recv];
      int hits = (rt == TY_POLY);
      /* the receiver's own chain resolving the name HERE, rather than the
         receiver being exactly this class: a subclass that inherits the method
         unchanged calls this very scope, and its call was left at the old type */
      if (!hits && ty_is_object(rt))
        hits = comp_method_in_chain(c, ty_object_class(rt), cn, NULL) == s;
      /* `Klass.m` / `obj.class.m`: a class-method call reaching this scope. */
      if (!hits && rt == TY_CLASS && sc->is_cmethod) {
        int rci = -1;
        if (nt_kind(c->nt, recv) == NK_ConstantReadNode || nt_kind(c->nt, recv) == NK_ConstantPathNode)
          rci = comp_class_index(c, nt_str(c->nt, recv, "name"));
        else if (nt_kind(c->nt, recv) == NK_CallNode && nt_str(c->nt, recv, "name") &&
                 sp_streq(nt_str(c->nt, recv, "name"), "class")) {
          int robj = nt_ref(c->nt, recv, "receiver");
          TyKind rot = robj >= 0 ? c->ntype[robj] : TY_UNKNOWN;
          if (ty_is_object(rot)) rci = ty_object_class(rot);
        }
        if (rci >= 0) hits = comp_cmethod_in_chain(c, rci, cn, NULL) == s;
      }
      if (!hits) continue;
      if (c->ntype[cid] != TY_UNKNOWN && c->ntype[cid] != TY_VOID && c->ntype[cid] != TY_POLY) {
        c->ntype[cid] = TY_POLY;
        if (wnode) wnode[cid] = 1;
      }
    }
  }
  if (!wdid) break;
  }
  free(wnode);

  /* A proc form holds its block as a real parameter, so a nested block that
     yields and is itself lifted to a standalone proc has to capture it -- the
     same shape a lowered yield method has, and the same cell it needs. The
     YieldNode is not a local read, so the capture pass cannot see it; mark the
     parameter here and codegen's force adds it to the capture set. */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *d = &c->scopes[s];
    if (!d->is_proc_form || !d->blk_param || !d->blk_param[0]) continue;
    for (int id = 0; id < c->nt->count; id++) {
      if (c->nscope[id] != s || !a_proc_create_or_lifted(c, id)) continue;
      int pb = a_proc_body(c, id);
      if (pb < 0 || !subtree_has_yield_node(c, pb, 0)) continue;
      LocalVar *bl = scope_local_intern(d, d->blk_param);
      if (bl) bl->is_cell = 1;
      break;
    }
  }

  /* An --rbs seed the settled types statically contradict is a compile error,
     not something to emit a reinterpretation for. */
  mark_nullable_int_locals(c);
  mark_array_or_nil_slots(c);
  /* A local's array KIND has to agree with what its writes actually build. A
     value whose type widens to a poly array LATE -- a map whose block value
     became boxed because a Proc composition widened the method it calls -- left
     the slot at the kind it had settled on, and the assignment did not compile
     (#4009). Widen the slot to match; the reverse never happens, since a poly
     array is the widest kind. */
  for (int id = 0; id < c->nt->count; id++) {
    if (nt_kind(c->nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(c->nt, id, "name");
    int v = nt_ref(c->nt, id, "value");
    if (!wn || v < 0) continue;
    Scope *sc = comp_scope_of(c, id);
    LocalVar *lv = sc ? scope_local(sc, wn) : NULL;
    if (!lv || lv->is_param || lv->is_block_param || lv->rbs_seeded) continue;
    if (lv->oa_pin != TY_UNKNOWN) continue;   /* a narrowed slot keeps its kind */
    /* An OBJECT slot whose write is boxed: an attribute that holds a base class
       and its subclass reads as poly, and a slot that settled on one of the two
       kept the pointer type while the read handed back an sp_RbVal (#4023). */
    if (ty_is_object(lv->type) && comp_ntype(c, v) == TY_POLY) {
      lv->type = TY_POLY;
      NT_FOREACH_KIND(c->nt, NK_LocalVariableReadNode, rid2) {
        const char *n4 = nt_str(c->nt, rid2, "name");
        if (!n4 || !sp_streq(n4, wn) || comp_scope_of(c, rid2) != sc) continue;
        c->ntype[rid2] = TY_POLY;
      }
      continue;
    }
    if (lv->type != TY_INT_ARRAY && lv->type != TY_FLOAT_ARRAY && lv->type != TY_STR_ARRAY) continue;
    if (comp_ntype(c, v) != TY_POLY_ARRAY) continue;
    /* only where the value is a BLOCK-collecting call: those build the array
       fresh, so nothing else holds it at the old kind */
    if (nt_kind(c->nt, v) != NK_CallNode || nt_ref(c->nt, v, "block") < 0) continue;
    lv->type = TY_POLY_ARRAY;
    NT_FOREACH_KIND(c->nt, NK_LocalVariableReadNode, rid) {
      const char *n3 = nt_str(c->nt, rid, "name");
      if (!n3 || !sp_streq(n3, wn) || comp_scope_of(c, rid) != sc) continue;
      c->ntype[rid] = TY_POLY_ARRAY;
    }
  }

  check_seed_contradictions(c);

  /* A global the program only ever mentions holding nil -- `$log = nil` and
     nothing else, or a read of one never assigned at all -- settles at no type,
     and a global with no type gets no slot, so the reference does not compile.
     Ruby's answer for both is nil, which the boxed form holds. Done after the
     fixpoint so a global that any write gives a real type keeps it. */
  { int promoted = 0;
    for (int i = 0; i < c->ngvars; i++)
      if (c->gvars[i].type == TY_UNKNOWN) { c->gvars[i].type = TY_POLY; promoted = 1; }
    /* Stamp the reads too: a node's type is cached from the round that typed
       it, and every round so far saw this global as having no type. */
    if (promoted) {
      NodeTable *ntg = (NodeTable *)c->nt;
      for (int id = 0; id < ntg->count && id < c->node_cap; id++) {
        if (nt_kind(ntg, id) != NK_GlobalVariableReadNode) continue;
        if (c->ntype[id] != TY_UNKNOWN) continue;
        const char *nm = nt_str(ntg, id, "name");
        const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
        LocalVar *g = rn ? comp_gvar(c, rn) : NULL;
        if (g && g->type == TY_POLY) c->ntype[id] = TY_POLY;
      }
    }
  }

  /* Last: the capture pass again, on the settled types. a_block_is_lifted asks
     whether the receiver is poly, and a receiver that widened after the
     earlier run answered no then and yes now -- so codegen routes the call to
     the cls_id dispatch (a real proc) while analyze left its captures
     uncelled, and the emit refuses. Only sets is_cell, and is idempotent. */
  mark_proc_captures(c);
  /* A capped run emits from whatever the last round left, which need not be a
     fixpoint; that is a compiler bug worth hearing about, not a quiet log. */
  if (g_fixpoint_rounds >= 128)
    fprintf(stderr, "spinel: warning: type inference did not converge in %d rounds; "
            "the output may be built from unsettled types (please report this program)\n",
            g_fixpoint_rounds);
  if (getenv("SP_FIXPOINT_LOG"))
    fprintf(stderr, "[fp] rounds=%d%s\n", g_fixpoint_rounds,
            g_fixpoint_rounds >= 128 ? " (CAP -- did not converge)" : "");
}
