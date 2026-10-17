/* analyze_cls.c -- the user classes a value can hold (#8530).

   One fact, decided once by the analysis before the type fixpoint, for
   every node and every slot: the set of user classes an object the value
   holds, or reaches through a container, a Proc or a method value, can be
   an instance of. A call on a boxed receiver can then bind and dispatch to
   the methods of the classes in the receiver's set instead of every class
   with the name: `[History.new, Empty.new][0].output` need not box
   Seq#output's parameters.

   The fact is a may-hold over-approximation computed without types, so it
   is the same whatever the fixpoint later decides: a Steensgaard-style
   unification. Every node and every slot (a local, a parameter, an ivar by
   class family and name, a global, a constant, a class variable, a method's
   value, a method's yield arguments and its blocks' values) is an element
   of a union-find; a flow unifies two elements (an assignment, a read, an
   argument with a parameter, a call with a method's value, a yield with a
   block parameter), and a constructor (`X.new`, `X.allocate`, a Struct's
   `X[...]`, `self` in an instance method, a rescue binding) adds classes
   to its element's set. A container is not told from its elements (the set
   of `[a, b]` is a's and b's; an element read or an iteration keeps the
   container's set), so a store through any name of the container grows
   every name's set, as a mutation through an alias needs. A builtin call
   the analysis does not model answers the union of its receiver, its
   arguments, its block's parameters and its block's value, which is also
   where any of them may end up stored.

   Where the analysis cannot see what reaches an element it holds every
   class (CF_ALL): a call named by a send, a method or a define_method, a
   value a Marshal, an ObjectSpace, a Fiber or a Thread hands out, a
   parameter of a method the runtime calls back (a protocol method: to_s,
   <=>, each, call, ...), a lambda's or a define_method block's parameter,
   a pattern's binding, a multiple assignment's target, a loop's value, a
   `new` on a class value.

   Rounds: the unifications a call makes depend on the receiver's set
   (which classes' methods it names), and that set grows as elements
   unify, so the node walk repeats until a round changes nothing. Each
   round only unifies and only adds classes, so it stops.

   Only the node facts are kept (c->cls_fact, read through cls_fact_node):
   a consumer asks about a call's receiver. Nodes made after the pass (a
   desugar's) have no fact and hold every class. */

#include "analyze_internal.h"
#include "builtin_names.h"

int class_has_exc_name(Compiler *c, int ci);   /* codegen.c */

#define CF_ALL   0      /* the pool index of the set of every class */
#define CF_EMPTY 1      /* ... and of no class */
#define CF_YARGS 8      /* yield positions a method's blocks are bound by */
#define CF_ROUNDS 64
#define CF_WHY_CAP (-1)

typedef struct { int cls; const char *name; int el; } CFEntry;
typedef struct { const char *name; int *mi; int n; int *cmi; int ncm; int *rd; int nrd; int *wr; int nwr; } CFByName;

typedef struct CF {
  Compiler *c; const NodeTable *nt; int *par; int nnodes;
  int nel, cel; int *uf; int *set;      /* union-find: parent, and the root's pool index */
  ClsSet *pool; int npool, cpool; int *ph; int phcap;
  int **lv_el; int *nlv;                /* per scope, per local index */
  int *ret_el, *yval_el, *self_el, **yarg_el;
  int *def_mi;                          /* DefNode -> scope */
  CFEntry *ent; int ent_cap, ent_n;     /* (key, name) slots */
  int *own, *desc, *fam;                /* per class: {k + its singleton classes}, its subtree, its family key */
  unsigned char *mod;                   /* per class: a module */
  unsigned char *has_super;             /* per scope: a super in it, which hands the block on */
  unsigned char *protocol;              /* per scope: the runtime may call it with its own arguments */
  const char **dyn; int ndyn, cdyn;     /* method names a send, a method(:m) or a define_method names */
  int dyn_all, all_ivars, has_mm;
  int exc_set;                          /* every user exception class */
  int fiber_ch;                         /* what a Fiber.yield answers: a resume's arguments */
  unsigned char *yval_used;             /* per scope: a yield's value is read (else the blocks' values go nowhere) */
  unsigned char *has_native;            /* per class: native bindings, made on first ask */
  CFByName *bn; int bn_cap, bn_n;
  int changed, round;
  int phase;              /* 0: only what the receivers' classes decide; 1: the builtin answers too */
  int *open; int nopen;   /* the runtime's classes and modules the program reopens (cf_open_self) */
  int *why;                   /* SPINEL_CLS_FACTS=why: per root, the rule (its source line) that made it
                                 every class, CF_WHY_CAP when a union outgrew CLS_SET_MAX */
  int *why_el;                /* ... and the element the rule named (a node: the call, the argument) */
  int *usz;                   /* ... per root, its element count */
  int (*uev)[3]; int nuev, cuev;  /* ... each union of two classes: the rule's line, the root, the smaller count */
  unsigned char *exec_esc;    /* per scope: its block parameter may run under another self */
  unsigned char *exec_blk;    /* per node: a block or lambda that may run under another self */
  int pending;            /* calls round 0 left to phase 1 */
} CF;

static void cf_oom(void) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }

/* ---- the pool of sets ---- */

static int cf_set_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static unsigned cf_set_hash(const ClsSet *s) {
  unsigned h = (2166136261u ^ (unsigned)s->all) * 16777619u ^ (unsigned)s->bv;
  for (int i = 0; i < s->n; i++) h = (h ^ (unsigned)s->cls[i]) * 16777619u;
  return h;
}
static int cf_set_eq(const ClsSet *a, const ClsSet *b) {
  if (a->all != b->all || a->n != b->n || a->bv != b->bv) return 0;
  for (int i = 0; i < a->n; i++) if (a->cls[i] != b->cls[i]) return 0;
  return 1;
}
/* the pool index of set s, normalized (sorted, no duplicate, every class
   past the cap) */
static int cf_pool(CF *f, ClsSet *s) {
  if (s->all) { s->n = 0; s->bv = 0; return CF_ALL; }
  if (s->n > 1) {
    qsort(s->cls, (size_t)s->n, sizeof s->cls[0], cf_set_cmp);
    int w = 0;
    for (int i = 0; i < s->n; i++) if (w == 0 || s->cls[w - 1] != s->cls[i]) s->cls[w++] = s->cls[i];
    s->n = (unsigned char)w;
  }
  if (s->n == 0 && !s->bv) return CF_EMPTY;
  if (f->npool * 2 >= f->phcap) {
    int ncap = f->phcap ? f->phcap * 2 : 1024;
    int *nh = malloc(sizeof *nh * (size_t)ncap);
    if (!nh) cf_oom();
    for (int i = 0; i < ncap; i++) nh[i] = -1;
    for (int i = 0; i < f->npool; i++) {
      unsigned j = cf_set_hash(&f->pool[i]) & (unsigned)(ncap - 1);
      while (nh[j] >= 0) j = (j + 1) & (unsigned)(ncap - 1);
      nh[j] = i;
    }
    free(f->ph); f->ph = nh; f->phcap = ncap;
  }
  unsigned j = cf_set_hash(s) & (unsigned)(f->phcap - 1);
  while (f->ph[j] >= 0) {
    if (cf_set_eq(&f->pool[f->ph[j]], s)) return f->ph[j];
    j = (j + 1) & (unsigned)(f->phcap - 1);
  }
  if (f->npool == f->cpool) {
    f->cpool = f->cpool ? f->cpool * 2 : 256;
    f->pool = realloc(f->pool, sizeof *f->pool * (size_t)f->cpool);
    if (!f->pool) cf_oom();
  }
  f->pool[f->npool] = *s;
  f->ph[j] = f->npool;
  return f->npool++;
}
static int cf_union(CF *f, int a, int b) {
  if (a == b || b == CF_EMPTY) return a;
  if (a == CF_EMPTY) return b;
  if (a == CF_ALL || b == CF_ALL) return CF_ALL;
  const ClsSet *x = &f->pool[a], *y = &f->pool[b];
  ClsSet s; memset(&s, 0, sizeof s);
  s.bv = (unsigned char)(x->bv | y->bv);
  if (x->n + y->n > CLS_SET_MAX) {
    /* the merge may still fit: count the distinct ones */
    int i = 0, j = 0, n = 0;
    while (i < x->n || j < y->n) {
      if (j >= y->n || (i < x->n && x->cls[i] < y->cls[j])) i++;
      else if (i >= x->n || y->cls[j] < x->cls[i]) j++;
      else { i++; j++; }
      n++;
    }
    if (n > CLS_SET_MAX) return CF_ALL;
  }
  /* a member of both is taken once: the count above found the merge fits */
  int i = 0, j = 0;
  while (i < x->n || j < y->n) {
    if (j >= y->n || (i < x->n && x->cls[i] < y->cls[j])) s.cls[s.n++] = x->cls[i++];
    else if (i >= x->n || y->cls[j] < x->cls[i]) s.cls[s.n++] = y->cls[j++];
    else { s.cls[s.n++] = x->cls[i]; i++; j++; }
  }
  return cf_pool(f, &s);
}
static int cf_single(CF *f, int cls) {
  ClsSet s; memset(&s, 0, sizeof s);
  s.cls[0] = cls; s.n = 1;
  return cf_pool(f, &s);
}
/* the set of a builtin value with no class of the program's */
static int cf_bv_set(CF *f) {
  ClsSet s; memset(&s, 0, sizeof s);
  s.bv = 1;
  return cf_pool(f, &s);
}
/* the Class object of k: a member past the classes */
static int cf_classval(CF *f, int k) { return cf_single(f, k + f->c->nclasses); }
/* the Class objects of the instance classes of set pidx */
static int cf_classvals_of(CF *f, int pidx) {
  if (pidx == CF_ALL) return CF_ALL;
  const ClsSet *x = &f->pool[pidx];
  ClsSet s; memset(&s, 0, sizeof s);
  for (int i = 0; i < x->n; i++) {
    if (x->cls[i] >= f->c->nclasses) continue;
    if (s.n >= CLS_SET_MAX) { s.all = 1; break; }
    s.cls[s.n++] = x->cls[i] + f->c->nclasses;
  }
  return cf_pool(f, &s);
}

/* ---- the union-find ---- */

static int cf_new_el(CF *f) {
  if (f->nel == f->cel) {
    f->cel = f->cel ? f->cel * 2 : 1024;
    f->uf = realloc(f->uf, sizeof *f->uf * (size_t)f->cel);
    f->set = realloc(f->set, sizeof *f->set * (size_t)f->cel);
    if (!f->uf || !f->set) cf_oom();
    if (f->why && !(f->why = realloc(f->why, sizeof *f->why * (size_t)f->cel))) cf_oom();
    if (f->usz && !(f->usz = realloc(f->usz, sizeof *f->usz * (size_t)f->cel))) cf_oom();
    if (f->why_el && !(f->why_el = realloc(f->why_el, sizeof *f->why_el * (size_t)f->cel))) cf_oom();
  }
  f->uf[f->nel] = f->nel; f->set[f->nel] = CF_EMPTY;
  if (f->why) { f->why[f->nel] = 0; f->why_el[f->nel] = -1; f->usz[f->nel] = 1; }
  return f->nel++;
}
static int cf_find(CF *f, int x) {
  while (f->uf[x] != x) { f->uf[x] = f->uf[f->uf[x]]; x = f->uf[x]; }
  return x;
}
static void cf_unite_at(CF *f, int a, int b, int line) {
  if (a < 0 || b < 0) return;
  a = cf_find(f, a); b = cf_find(f, b);
  if (a == b) return;
  if (a > b) { int t = a; a = b; b = t; }
  f->uf[b] = a;
  if (f->usz) {
    int lo = f->usz[a] < f->usz[b] ? f->usz[a] : f->usz[b];
    f->usz[a] += f->usz[b];
    if (f->nuev == f->cuev) {
      f->cuev = f->cuev ? f->cuev * 2 : 4096;
      if (!(f->uev = realloc(f->uev, sizeof *f->uev * (size_t)f->cuev))) cf_oom();
    }
    f->uev[f->nuev][0] = line; f->uev[f->nuev][1] = a; f->uev[f->nuev][2] = lo; f->nuev++;
  }
  int sa = f->set[a], sb = f->set[b];
  f->set[a] = cf_union(f, sa, sb);
  if (f->why && f->set[a] == CF_ALL && sa != CF_ALL) {
    f->why[a] = sb == CF_ALL ? f->why[b] : CF_WHY_CAP;
    f->why_el[a] = sb == CF_ALL ? f->why_el[b] : -1;
  }
  f->changed = 1;
}
static void cf_add_set_at(CF *f, int x, int pidx, int line) {
  if (x < 0) return;
  int x0 = x;
  x = cf_find(f, x);
  int u = cf_union(f, f->set[x], pidx);
  if (u != f->set[x]) {
    if (f->why && u == CF_ALL) { f->why[x] = pidx == CF_ALL ? line : CF_WHY_CAP; f->why_el[x] = x0; }
    f->set[x] = u; f->changed = 1;
  }
}
#define cf_add_set(f, x, pidx) cf_add_set_at((f), (x), (pidx), __LINE__)
#define cf_unite(f, a, b) cf_unite_at((f), (a), (b), __LINE__)
#define cf_all(f, x) cf_add_set_at((f), (x), CF_ALL, __LINE__)
static int cf_set_of(CF *f, int x) { return x < 0 ? CF_ALL : f->set[cf_find(f, x)]; }

/* ---- the slots ---- */

static int cf_scope_of(CF *f, int v) {
  int si = v >= 0 && v < f->nnodes ? f->c->nscope[v] : 0;
  return si < 0 || si >= f->c->nscopes ? 0 : si;
}
/* the element of local `nm` as node v's scope sees it, -1 for none */
static int cf_lv(CF *f, int v, const char *nm) {
  if (!nm) return -1;
  int si = cf_scope_of(f, v);
  Scope *s = &f->c->scopes[si];
  LocalVar *lv = scope_local(s, nm);
  if (!lv) return -1;
  int idx = (int)(lv - s->locals);
  if (idx < 0 || idx >= f->nlv[si]) return -1;
  if (!f->lv_el[si]) {
    f->lv_el[si] = malloc(sizeof(int) * (size_t)(f->nlv[si] + 1));
    if (!f->lv_el[si]) cf_oom();
    for (int k = 0; k < f->nlv[si]; k++) f->lv_el[si][k] = -1;
  }
  if (f->lv_el[si][idx] < 0) f->lv_el[si][idx] = cf_new_el(f);
  return f->lv_el[si][idx];
}
static int cf_lv_in(CF *f, int si, const char *nm) {
  Scope *s = &f->c->scopes[si];
  LocalVar *lv = nm ? scope_local(s, nm) : NULL;
  if (!lv) return -1;
  int idx = (int)(lv - s->locals);
  if (idx < 0 || idx >= f->nlv[si]) return -1;
  if (!f->lv_el[si]) {
    f->lv_el[si] = malloc(sizeof(int) * (size_t)(f->nlv[si] + 1));
    if (!f->lv_el[si]) cf_oom();
    for (int k = 0; k < f->nlv[si]; k++) f->lv_el[si][k] = -1;
  }
  if (f->lv_el[si][idx] < 0) f->lv_el[si][idx] = cf_new_el(f);
  return f->lv_el[si][idx];
}

static unsigned cf_ent_hash(int cls, const char *name) { return sp_strhash(name) * 31u + (unsigned)(cls + 1000); }
/* the element of the (key, name) slot: an ivar by class family, a class's
   own ivar (key -(k+2)), the main object's (-1), a class variable (-2), a
   global (-3), a constant (-4) */
static int cf_entry(CF *f, int cls, const char *name) {
  if (!name) return -1;
  if (f->ent_n * 2 >= f->ent_cap) {
    int ncap = f->ent_cap ? f->ent_cap * 2 : 256;
    CFEntry *nv = calloc((size_t)ncap, sizeof *nv);
    if (!nv) cf_oom();
    for (int k = 0; k < f->ent_cap; k++) {
      if (!f->ent[k].name) continue;
      unsigned j = cf_ent_hash(f->ent[k].cls, f->ent[k].name) & (unsigned)(ncap - 1);
      while (nv[j].name) j = (j + 1) & (unsigned)(ncap - 1);
      nv[j] = f->ent[k];
    }
    free(f->ent); f->ent = nv; f->ent_cap = ncap;
  }
  unsigned j = cf_ent_hash(cls, name) & (unsigned)(f->ent_cap - 1);
  while (f->ent[j].name) {
    if (f->ent[j].cls == cls && sp_streq(f->ent[j].name, name)) return f->ent[j].el;
    j = (j + 1) & (unsigned)(f->ent_cap - 1);
  }
  char *own = strdup(name);
  if (!own) cf_oom();
  f->ent[j].cls = cls; f->ent[j].name = own; f->ent[j].el = cf_new_el(f);
  f->ent_n++;
  return f->ent[j].el;
}
/* is there a (key, name) slot already? */
static int cf_entry_has(CF *f, int cls, const char *name) {
  if (!name || !f->ent_cap) return 0;
  unsigned j = cf_ent_hash(cls, name) & (unsigned)(f->ent_cap - 1);
  while (f->ent[j].name) {
    if (f->ent[j].cls == cls && sp_streq(f->ent[j].name, name)) return 1;
    j = (j + 1) & (unsigned)(f->ent_cap - 1);
  }
  return 0;
}
#define CF_K_MAIN  (-1)
#define CF_K_CVAR  (-2)
#define CF_K_GVAR  (-3)
#define CF_K_CONST (-4)
#define CF_K_CONSTW (-5)   /* a constant the program writes (by its last segment) */

/* a method the compiler supplies (builtins/*.rb, spliced in and marked
   node_bi; a desugar's synthesized def): calls the desugars make reach it
   after this pass, so its parameters and its self take anything, and a
   call that resolves to it is answered as the builtin it implements */
static int cf_prelude_scope(CF *f, int mi) {
  Scope *s = &f->c->scopes[mi];
  if (s->def_node < 0) return 0;
  return nt_int(f->nt, s->def_node, "node_bi", 0) != 0 || nt_int(f->nt, s->def_node, "node_line", 0) == 0;
}
/* does class k carry native bindings (ffi_func, a Fiddle extern): a call
   on it the program's own methods do not answer may be one of those, whose
   value is anything */
static int cf_class_has_natives(CF *f, int k) {
  if (k < 0 || k >= f->c->nclasses) return 0;
  if (!f->has_native) {
    f->has_native = calloc((size_t)f->c->nclasses + 1, 1);
    if (!f->has_native) cf_oom();
    for (int i = 0; i < f->c->n_native_methods; i++) {
      int ck = f->c->native_methods[i].class_id;
      if (ck >= 0 && ck < f->c->nclasses) f->has_native[ck] = 1;
    }
  }
  return f->has_native[k] || f->c->classes[k].is_native_class;
}
/* a top-level class or module of the runtime's that the program reopens
   (Object, Kernel, Comparable, String, Exception, ...): its methods run for
   objects the program's classes do not list, its own subclasses and
   includers among them */
static int cf_open_self(CF *f, int k) {
  ClassInfo *ci = &f->c->classes[k];
  const char *nm = ci->name;
  if (!nm || ci->enclosing_class >= 0) return 0;
  return is_builtin_reopen_name(nm) || is_object_root(nm) || is_builtin_class_name(nm) ||
         is_builtin_module_name(nm) || is_builtin_exception_name(nm);
}
/* a Kernel function or a class body's declaration: a receiverless call
   whose answer does not come from self */
static int cf_selfless_fn(const char *nm) {
  static const char *const names[] = { "puts", "print", "p", "pp", "printf", "sprintf", "format", "putc",
    "raise", "fail", "require", "require_relative", "load", "sleep", "rand", "srand", "Integer", "Float",
    "String", "Array", "Hash", "Rational", "Complex", "loop", "catch", "throw", "lambda", "proc",
    "block_given?", "binding", "caller", "caller_locations", "exit", "exit!", "abort", "at_exit", "gets",
    "warn", "open", "system", "spawn", "`", "select", "trap", "fork", "exec", "autoload", "readline",
    "readlines", "__method__", "__dir__", "attr_reader", "attr_writer", "attr_accessor", "attr", "private",
    "public", "protected", "module_function", "include", "prepend", "define_method", "alias_method",
    "remove_method", "undef_method", "private_constant", "private_class_method", "public_class_method", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
/* a scope's instance class: the class whose instances run it, -1 for a
   class method, a class body or the top level */
static int cf_inst_class(CF *f, int si) {
  Scope *s = &f->c->scopes[si];
  return s->def_node >= 0 && !s->is_cmethod && s->class_id >= 0 ? s->class_id : -1;
}
/* the ivar slots node v's scope reads `name` from, into out (at most max) */
static int cf_ivar_els(CF *f, int v, const char *name, int *out, int max) {
  Compiler *c = f->c;
  int si = cf_scope_of(f, v), n = 0;
  int k = cf_inst_class(f, si);
  if (k >= 0 && !f->mod[k]) { out[n++] = cf_entry(f, f->fam[k], name); return n; }
  if (k >= 0) {
    /* a module's method runs in its includers */
    for (int i = 0; i < c->nclasses && n < max - 1; i++) {
      if (f->mod[i]) continue;
      int inc = 0;
      for (int m = 0; m < c->classes[i].nincluded_mods && !inc; m++)
        if (c->classes[i].included_mods[m] == k) inc = 1;
      if (inc) out[n++] = cf_entry(f, f->fam[i], name);
    }
    out[n++] = cf_entry(f, CF_K_MAIN, name);
    return n;
  }
  Scope *s = &c->scopes[si];
  int ck = s->def_node >= 0 && s->is_cmethod ? s->class_id
         : v >= 0 && v < f->nnodes && c->node_cbody ? c->node_cbody[v] : -1;
  out[n++] = cf_entry(f, ck >= 0 ? -(ck + 2) : CF_K_MAIN, name);
  return n;
}
/* `X.name` or `X.name = v` through a `class << self` accessor of class k:
   k's class-level ivar, the slot its class body and its class methods name
   `@name` (an over-approximation where the accessor's storage is its own) */
static int cf_sg_access(CF *f, int v, int k, const char *nm, const int *argv, int argc) {
  ClassInfo *ci = &f->c->classes[k];
  char ivn[256];
  if (argc == 0 && comp_is_sg_reader(ci, nm)) {
    snprintf(ivn, sizeof ivn, "@%s", nm);
    if (v >= 0) cf_unite(f, v, cf_entry(f, -(k + 2), ivn));
    return 1;
  }
  char base[256];
  if (setter_base_name(nm, base, sizeof base)) {
    if (!comp_is_sg_writer(ci, base)) return 0;
    char wivn[sizeof base + 1];
    snprintf(wivn, sizeof wivn, "@%s", base);
    int e = cf_entry(f, -(k + 2), wivn);
    for (int a = 0; a < argc; a++) { cf_unite(f, e, argv[a]); if (v >= 0) cf_unite(f, v, argv[a]); }
    return 1;
  }
  return 0;
}
/* the last segment of a constant path's name */
static const char *cf_const_key(const char *name) {
  if (!name) return NULL;
  const char *p = strrchr(name, ':');
  return p ? p + 1 : name;
}

/* ---- the classes ---- */

static int cf_is_class_const(CF *f, int n) {
  if (n < 0) return -1;
  NodeKind k = nt_kind(f->nt, n);
  if (k != NK_ConstantReadNode && k != NK_ConstantPathNode) return -1;
  return comp_class_index(f->c, nt_str(f->nt, n, "name"));
}
/* the methods named `name`, memoized: the class structure does not move
   during the pass */
static CFByName *cf_byname(CF *f, const char *name) {
  Compiler *c = f->c;
  if (f->bn_n * 2 >= f->bn_cap) {
    int ncap = f->bn_cap ? f->bn_cap * 2 : 512;
    CFByName *nv = calloc((size_t)ncap, sizeof *nv);
    if (!nv) cf_oom();
    for (int k = 0; k < f->bn_cap; k++) {
      if (!f->bn[k].name) continue;
      unsigned j = sp_strhash(f->bn[k].name) & (unsigned)(ncap - 1);
      while (nv[j].name) j = (j + 1) & (unsigned)(ncap - 1);
      nv[j] = f->bn[k];
    }
    free(f->bn); f->bn = nv; f->bn_cap = ncap;
  }
  unsigned j = sp_strhash(name) & (unsigned)(f->bn_cap - 1);
  while (f->bn[j].name) {
    if (sp_streq(f->bn[j].name, name)) return &f->bn[j];
    j = (j + 1) & (unsigned)(f->bn_cap - 1);
  }
  CFByName *e = &f->bn[j];
  e->name = strdup(name);
  if (!e->name) cf_oom();
  f->bn_n++;
  int cap = 8, ccap = 4, rcap = 4, wcap = 4;
  e->mi = malloc(sizeof(int) * (size_t)cap); e->cmi = malloc(sizeof(int) * (size_t)ccap);
  e->rd = malloc(sizeof(int) * (size_t)rcap); e->wr = malloc(sizeof(int) * (size_t)wcap);
  if (!e->mi || !e->cmi || !e->rd || !e->wr) cf_oom();
  char base[256];
  int is_w = setter_base_name(name, base, sizeof base);
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi >= 0) {
      int seen = 0;
      for (int i = 0; i < e->n; i++) if (e->mi[i] == mi) { seen = 1; break; }
      if (!seen) {
        if (e->n == cap) { cap *= 2; e->mi = realloc(e->mi, sizeof(int) * (size_t)cap); if (!e->mi) cf_oom(); }
        e->mi[e->n++] = mi;
      }
    }
    int cm = comp_cmethod_in_chain(c, k, name, NULL);
    if (cm >= 0) {
      int seen = 0;
      for (int i = 0; i < e->ncm; i++) if (e->cmi[i] == cm) { seen = 1; break; }
      if (!seen) {
        if (e->ncm == ccap) { ccap *= 2; e->cmi = realloc(e->cmi, sizeof(int) * (size_t)ccap); if (!e->cmi) cf_oom(); }
        e->cmi[e->ncm++] = cm;
      }
    }
    if (comp_reader_in_chain(c, k, name, NULL)) {
      if (e->nrd == rcap) { rcap *= 2; e->rd = realloc(e->rd, sizeof(int) * (size_t)rcap); if (!e->rd) cf_oom(); }
      e->rd[e->nrd++] = k;
    }
    if (is_w && comp_writer_in_chain(c, k, base, NULL)) {
      if (e->nwr == wcap) { wcap *= 2; e->wr = realloc(e->wr, sizeof(int) * (size_t)wcap); if (!e->wr) cf_oom(); }
      e->wr[e->nwr++] = k;
    }
  }
  int tl = comp_method_index(c, name);
  if (tl >= 0) {
    int seen = 0;
    for (int i = 0; i < e->n; i++) if (e->mi[i] == tl) { seen = 1; break; }
    if (!seen) {
      if (e->n == cap) { cap *= 2; e->mi = realloc(e->mi, sizeof(int) * (size_t)cap); if (!e->mi) cf_oom(); }
      e->mi[e->n++] = tl;
    }
  }
  return e;
}

static int cf_is_dyn(CF *f, const char *name) {
  if (f->dyn_all) return 1;
  for (int i = 0; i < f->ndyn; i++) if (sp_streq(f->dyn[i], name)) return 1;
  return 0;
}
static void cf_dyn_add(CF *f, const char *name) {
  if (!name) return;
  if (f->ndyn == f->cdyn) {
    f->cdyn = f->cdyn ? f->cdyn * 2 : 16;
    f->dyn = realloc(f->dyn, sizeof *f->dyn * (size_t)f->cdyn);
    if (!f->dyn) cf_oom();
  }
  f->dyn[f->ndyn++] = name;
}

/* a method name whose call the analysis cannot follow: its value, and
   where its arguments go, are unknown */
static int cf_opaque_name(const char *nm) {
  static const char *const names[] = {
    "send", "__send__", "public_send", "method", "public_method", "instance_method",
    "singleton_method", "define_method", "define_singleton_method", "instance_variable_get",
    "instance_variable_set", "instance_variables", "instance_exec", "instance_eval", "class_eval",
    "class_exec", "module_eval", "module_exec", "const_get", "const_set", "eval", "binding",
    "catch", "throw", "method_missing", "remove_instance_variable", "singleton_class", "extend",
    "local_variable_get", "local_variable_set", "receiver", "owner", "unbind", "bind", "bind_call",
    "__method__", "caller", "caller_locations", "ObjectSpace", "superclass", "subclasses", "ancestors",
    "included_modules", "instance_methods", "public_instance_methods", "const_source_location", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
/* a constant whose methods hand out values from anywhere */
static int cf_poison_const(const char *nm) {
  static const char *const names[] = { "Marshal", "ObjectSpace", "Fiber", "Thread", "Ractor", "Binding",
    "TracePoint", "YAML", "Psych", "Kernel", "BasicObject", "Module", "Class", "Method", "UnboundMethod",
    "RubyVM", "ThreadGroup", "DRb", "PP", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
/* a method the runtime calls back with arguments of its own */
static int cf_protocol_name(const char *nm) {
  static const char *const names[] = {
    "to_s", "inspect", "==", "!=", "eql?", "equal?", "hash", "<=>", "===", "=~", "!~", "<", "<=", ">", ">=",
    "+", "-", "*", "/", "%", "**", "&", "|", "^", "<<", ">>", "!", "~", "+@", "-@", "[]", "[]=",
    "each", "each_pair", "call", "to_proc", "to_ary", "to_a", "to_h", "to_hash", "to_str", "to_int",
    "to_i", "to_f", "to_r", "to_c", "to_sym", "to_path", "to_io", "to_json", "as_json", "coerce",
    "succ", "pred", "dup", "clone", "initialize_copy", "initialize_dup", "initialize_clone",
    "method_missing", "respond_to?", "respond_to_missing?", "exception", "message", "backtrace",
    "set_backtrace", "cause", "full_message", "detailed_message", "marshal_dump", "marshal_load",
    "_dump", "_load", "encode_with", "init_with", "pretty_print", "inherited", "included", "extended",
    "prepended", "method_added", "singleton_method_added", "const_missing", "write", "print", "puts",
    "read", "getc", "gets", "readpartial", "sysread", "syswrite", "close", "flush", "sync", "fileno",
    "tty?", "isatty", "binmode", "external_encoding", "set_encoding", "path", "size", "length", "empty?",
    "fetch", "key?", "keys", "values", "include?", "between?", "clamp", "step", "divmod", "fdiv",
    "arity", "curry", "parameters", "lambda?", "yield", "deconstruct", "deconstruct_keys", "freeze",
    "frozen?", "each_entry", "first", "last", "min", "max", "sum", "count", "find", "select", "map",
    "reject", "sort", "sort_by", "reduce", "inject", "zip", "each_with_index", "each_with_object",
    "entries", "lazy", "with_index", "next", "peek", "rewind", "default", "default_proc", "compare_by_identity",
    "join", "flatten", "concat", "push", "pop", "shift", "unshift", "insert", "delete", "delete_at",
    "delete_if", "slice", "dig", "values_at", "merge", "update", "store", "key", "invert", "transform_values",
    "replace", "clear", "then", "yield_self", "tap", "itself", "display", "nil?", "is_a?", "kind_of?",
    "instance_of?", "class", "object_id", "__id__", "singleton_methods", "methods", "public_methods",
    "define_method", "attr_accessor", "attr_reader", "attr_writer", "new", "allocate", "superclass",
    "ancestors", "name", "instance_methods", "const_get", "exit", "at_exit", "trap", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}

/* the statements' value: the last one, -1 for none */
static int cf_tail(CF *f, int st) {
  if (st < 0) return -1;
  if (nt_kind(f->nt, st) != NK_StatementsNode) return st;
  int n = 0; const int *b = nt_arr(f->nt, st, "body", &n);
  return n > 0 ? b[n - 1] : -1;
}
static int cf_unparen(CF *f, int v) {
  for (int d = 0; v >= 0 && d < 16 && nt_kind(f->nt, v) == NK_ParenthesesNode; d++) v = cf_tail(f, nt_ref(f->nt, v, "body"));
  return v;
}

/* a block's or a lambda's parameter names, into out; -1 for a splat, a
   keyword or a rest among them (the positions past it are unknown) */
static int cf_block_params(CF *f, int blk, const char **out, int max, int *plain) {
  const NodeTable *nt = f->nt;
  int n = 0;
  *plain = 1;
  int bp = nt_ref(nt, blk, "parameters");
  if (bp < 0) return 0;
  int pn = nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : bp;
  if (nt_kind(nt, bp) == NK_NumberedParametersNode) {
    for (int i = 0; i < max; i++) {
      const char *nm = block_param_name(f->c, blk, i);
      if (!nm) break;
      out[n++] = nm;
    }
    return n;
  }
  if (pn < 0 || nt_kind(nt, pn) != NK_ParametersNode) return 0;
  int rn = 0; const int *rq = nt_arr(nt, pn, "requireds", &rn);
  for (int i = 0; i < rn && n < max; i++) {
    const char *nm = nt_kind(nt, rq[i]) == NK_RequiredParameterNode ? nt_str(nt, rq[i], "name") : NULL;
    if (!nm) { *plain = 0; continue; }   /* a destructuring `|(a, b)|` */
    out[n++] = nm;
  }
  int on = 0; const int *op = nt_arr(nt, pn, "optionals", &on);
  for (int i = 0; i < on && n < max; i++) { out[n++] = nt_str(nt, op[i], "name"); }
  int pon = 0; nt_arr(nt, pn, "posts", &pon);
  int kn = 0; nt_arr(nt, pn, "keywords", &kn);
  if (pon > 0 || kn > 0 || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0) *plain = 0;
  return n;
}
/* every name a block, a lambda or a def declares as a parameter, into out */
static int cf_all_param_names(CF *f, int params_owner, const char **out, int max) {
  const NodeTable *nt = f->nt;
  int bp = nt_ref(nt, params_owner, "parameters");
  if (bp < 0) return 0;
  if (nt_kind(nt, bp) == NK_NumberedParametersNode) {
    int n = 0;
    for (int i = 0; i < max; i++) {
      const char *nm = block_param_name(f->c, params_owner, i);
      if (!nm) break;
      out[n++] = nm;
    }
    return n;
  }
  int pn = nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : bp;
  if (pn < 0) return 0;
  static const char *const lists[] = { "requireds", "optionals", "posts", "keywords", NULL };
  int n = 0;
  for (int l = 0; lists[l]; l++) {
    int ln = 0; const int *ps = nt_arr(nt, pn, lists[l], &ln);
    for (int i = 0; i < ln && n < max; i++) {
      const char *nm = nt_str(nt, ps[i], "name");
      if (nm) out[n++] = nm;
    }
  }
  static const char *const refs[] = { "rest", "keyword_rest", "block", NULL };
  for (int r = 0; refs[r]; r++) {
    int p = nt_ref(nt, pn, refs[r]);
    const char *nm = p >= 0 ? nt_str(nt, p, "name") : NULL;
    if (nm && n < max) out[n++] = nm;
  }
  return n;
}
static void cf_all_block_params(CF *f, int blk) {
  const char *names[64];
  int n = cf_all_param_names(f, blk, names, 64);
  int body = nt_ref(f->nt, blk, "body");
  for (int i = 0; i < n; i++) cf_all(f, cf_lv(f, body >= 0 ? body : blk, names[i]));
}

/* the owning method of node v: the DefNode above it, -1 at the top level
   or inside a lambda that comes first (when lambda_stop) */
static int cf_owner_def(CF *f, int v, int lambda_stop, int *lambda) {
  if (lambda) *lambda = -1;
  if (lambda_stop)
    for (int q = f->par ? f->par[v] : -1, d = 0; q >= 0 && d < 100000; q = f->par[q], d++) {
      NodeKind k = nt_kind(f->nt, q);
      if (k == NK_LambdaNode) { if (lambda) *lambda = q; return -1; }
      if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) break;
    }
  /* the owning scope, which a synthesized def's detached subtree still has */
  int si = cf_scope_of(f, v);
  return si > 0 && f->c->scopes[si].def_node >= 0 ? si : -1;
}
/* the block above node v (a BlockNode, a LambdaNode), stopping at a def;
   -1 for none */
static int cf_owner_block(CF *f, int v, int loop_stop) {
  for (int q = f->par ? f->par[v] : -1, d = 0; q >= 0 && d < 100000; q = f->par[q], d++) {
    NodeKind k = nt_kind(f->nt, q);
    if (k == NK_BlockNode || k == NK_LambdaNode) return q;
    if (loop_stop && (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode)) return -1;
    if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return -1;
  }
  return -1;
}

static int cf_in_exec_block(CF *f, int v);
static void cf_ivar_write_any(CF *f, const char *name, int v);

/* the loop a break under v leaves: the nearest enclosing while, until or
   for, or -1 when a block, a lambda or a def is nearer */
static int cf_owner_loop(CF *f, int v) {
  for (int q = f->par ? f->par[v] : -1, d = 0; q >= 0 && d < 100000; q = f->par[q], d++) {
    NodeKind k = nt_kind(f->nt, q);
    if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode) return q;
    if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode) return -1;
  }
  return -1;
}

/* ---- parameters ---- */

/* is parameter idx of method m a keyword parameter? */
static int cf_param_is_kw(CF *f, Scope *m, int idx) {
  const NodeTable *nt = f->nt;
  if (m->def_node < 0 || idx == m->rest_idx || idx == m->kwrest_idx) return 0;
  int pn = nt_ref(nt, m->def_node, "parameters");
  int kn = 0; const int *kws = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn) : NULL;
  for (int i = 0; i < kn; i++) {
    const char *nm = nt_str(nt, kws[i], "name");
    if (nm && m->pnames[idx] && sp_streq(nm, m->pnames[idx])) return 1;
  }
  return 0;
}
static int cf_param_el(CF *f, int mi, int idx) {
  Scope *m = &f->c->scopes[mi];
  if (idx < 0 || idx >= m->nparams || !m->pnames[idx]) return -1;
  return cf_lv_in(f, mi, m->pnames[idx]);
}
static void cf_all_params(CF *f, int mi) {
  Scope *m = &f->c->scopes[mi];
  for (int i = 0; i < m->nparams; i++) cf_all(f, cf_param_el(f, mi, i));
  if (m->blk_param && m->blk_param[0]) cf_all(f, cf_lv_in(f, mi, m->blk_param));
}
/* the call's arguments to method mi's parameters */
static void cf_bind_args(CF *f, int mi, const int *argv, int argc) {
  const NodeTable *nt = f->nt;
  Compiler *c = f->c;
  if (mi < 0 || mi >= c->nscopes) return;
  Scope *m = &c->scopes[mi];
  int pos[256], npos = 0, splat = 0, kwh = -1, fwd = 0;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_BlockArgumentNode) continue;
    if (k == NK_ForwardingArgumentsNode) { fwd = 1; continue; }
    if (k == NK_KeywordHashNode) { kwh = argv[i]; continue; }
    if (k == NK_SplatNode) splat = 1;
    if (npos < 256) pos[npos++] = argv[i];
  }
  if (fwd) { cf_all_params(f, mi); return; }
  /* the positional parameters, in order */
  int ppos[256], np = 0, has_kw = 0;
  for (int i = 0; i < m->nparams && np < 256; i++) {
    if (i == m->kwrest_idx) { has_kw = 1; continue; }
    if (cf_param_is_kw(f, m, i)) { has_kw = 1; continue; }
    ppos[np++] = i;
  }
  /* a keyword hash the method takes as a positional Hash */
  if (kwh >= 0 && !has_kw && npos < 256) { pos[npos++] = kwh; kwh = -1; }
  if (splat || m->rest_idx >= 0 || m->npost_rest > 0 || npos > np) {
    /* the positions are not known: every argument may reach every parameter */
    for (int i = 0; i < np; i++)
      for (int a = 0; a < npos; a++) {
        int an = nt_kind(nt, pos[a]) == NK_SplatNode ? nt_ref(nt, pos[a], "expression") : pos[a];
        cf_unite(f, cf_param_el(f, mi, ppos[i]), an);
      }
  }
  else {
    for (int a = 0; a < npos && a < np; a++) cf_unite(f, cf_param_el(f, mi, ppos[a]), pos[a]);
  }
  if (kwh >= 0) {
    int en = 0; const int *els = nt_arr(nt, kwh, "elements", &en);
    for (int e = 0; e < en; e++) {
      int key = nt_kind(nt, els[e]) == NK_AssocNode ? nt_ref(nt, els[e], "key") : -1;
      const char *kn = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
      int val = nt_ref(nt, els[e], "value");
      int bound = 0;
      if (kn) {
        for (int i = 0; i < m->nparams; i++)
          if (m->pnames[i] && sp_streq(m->pnames[i], kn) && cf_param_is_kw(f, m, i)) { cf_unite(f, cf_param_el(f, mi, i), val); bound = 1; }
        if (m->kwrest_idx >= 0) { cf_unite(f, cf_param_el(f, mi, m->kwrest_idx), val); bound = 1; }
      }
      if (!bound) {
        /* a computed key, a double splat: any keyword parameter */
        for (int i = 0; i < m->nparams; i++)
          if (i == m->kwrest_idx || cf_param_is_kw(f, m, i)) cf_unite(f, cf_param_el(f, mi, i), val);
      }
    }
  }
}
/* the block of a call with a user method target: its parameters take the
   method's yield arguments, its value is what the yields answer */
static void cf_bind_block(CF *f, int mi, int blk) {
  const NodeTable *nt = f->nt;
  Compiler *c = f->c;
  if (blk < 0) return;
  Scope *m = &c->scopes[mi];
  if (nt_kind(nt, blk) == NK_BlockArgumentNode) {
    /* `&pr`: the proc's value is its body's, its parameters take anything */
    int e = nt_ref(nt, blk, "expression");
    if (e >= 0 && nt_kind(nt, e) == NK_SymbolNode) {
      const char *sym = nt_str(nt, e, "value");
      if (sym) { CFByName *b = cf_byname(f, sym); for (int i = 0; i < b->n; i++) cf_unite(f, f->yval_el[mi], f->ret_el[b->mi[i]]); }
      if (!sym || sp_streq(sym, "new") || sp_streq(sym, "allocate") || cf_opaque_name(sym)) cf_all(f, f->yval_el[mi]);
    }
    else cf_unite(f, f->yval_el[mi], e);
    return;
  }
  if (nt_kind(nt, blk) != NK_BlockNode) return;
  if (f->yval_used[mi]) cf_unite(f, f->yval_el[mi], blk);
  int body = nt_ref(nt, blk, "body");
  const char *names[CF_YARGS];
  int plain = 1;
  int n = cf_block_params(f, blk, names, CF_YARGS, &plain);
  int opaque = (m->blk_param && m->blk_param[0]) || f->has_super[mi] || m->is_proc_form || m->is_lowered_yield;
  if (!plain || opaque) { cf_all_block_params(f, blk); return; }
  for (int i = 0; i < n; i++) cf_unite(f, cf_lv(f, body >= 0 ? body : blk, names[i]), f->yarg_el[mi][i]);
}

/* ---- calls ---- */

static void cf_construct(CF *f, int v, int x, const int *argv, int argc, int blk, const char *nm);

/* an iteration that drops its block's values */
static int cf_drops_block_value(const char *nm) {
  static const char *const names[] = { "each", "each_pair", "each_with_index", "each_index", "each_key",
    "each_value", "each_char", "each_byte", "each_line", "each_slice", "each_cons", "each_entry", "reverse_each",
    "times", "upto", "downto", "step", "cycle", "each_char", "each_codepoint", "each_grapheme_cluster", NULL };
  for (int i = 0; nm && names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
/* the builtin answer: the call's value is its receiver's, its arguments',
   its block's parameters' and its block's value (an element read, an
   iteration, a store, a pick, a copy, a conversion that hands on what it
   was given); an iteration that drops its block's values does not take
   them */
/* the methods a builtin calls back on the objects it is handed, whose
   values it may answer or build from (flatten's to_ary, sort's <=>) */
static void cf_protocol_returns(CF *f, int v, int el) {
  static const char *const names[] = { "to_ary", "to_a", "to_h", "to_hash", "to_s", "to_str", "to_proc",
    "to_path", "to_io", "to_int", "to_i", "coerce", "<=>", "==", "call", "succ", "exception", "message",
    "inspect", "hash", "eql?", "each", "dup", "clone", NULL };
  if (el < 0) return;
  int pidx = cf_set_of(f, el);
  if (pidx == CF_ALL || pidx == CF_EMPTY) return;
  const ClsSet *x = &f->pool[pidx];
  for (int i = 0; i < x->n; i++) {
    int k = x->cls[i];
    if (k < 0 || k >= f->c->nclasses) continue;
    for (int q = 0; names[q]; q++) {
      int mi = comp_method_in_chain(f->c, k, names[q], NULL);
      if (mi < 0 || cf_prelude_scope(f, mi)) continue;   /* the compiler's own answer as the builtin */
      cf_unite(f, v, f->ret_el[mi]);
      if (sp_streq(names[q], "each")) for (int y = 0; y < CF_YARGS; y++) cf_unite(f, v, f->yarg_el[mi][y]);
    }
  }
}
/* a builtin whose value is a scalar no program class is: a comparison, a
   count or size, a conversion to a String, a number or a Symbol, a
   predicate. It keeps none of its arguments. */
static int cf_scalar_answer(const char *nm) {
  static const char *const names[] = { "==", "!=", "<", "<=", ">", ">=", "<=>", "===", "eql?", "equal?",
    "=~", "!~", "size", "length", "bytesize", "count", "hash", "to_i", "to_f", "to_s", "to_str", "to_sym",
    "inspect", NULL };
  if (!nm) return 0;
  for (int i = 0; names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  size_t l = strlen(nm);
  return l > 1 && nm[l - 1] == '?';
}
/* a builtin that answers its receiver's elements (one, or a run of them)
   and keeps none of its arguments: an index or a key is only looked up */
static int cf_element_answer(const char *nm) {
  static const char *const names[] = { "[]", "first", "last", "pop", "shift", "sample", "min", "max",
    "dig", "at", NULL };
  for (int i = 0; nm && names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return 0;
}
/* Is the call plain: no block, no Symbol (a method a builtin may call by
   name), no splat? Only then is the answer by kind. A block keeps today's
   rule: its parameters take what the call's value is united with, and a
   receiver built by a method the compiler supplies (ThreadGroup's
   __groups) reaches them only that way. */
static int cf_plain_builtin_call(CF *f, const int *argv, int argc, int blk) {
  const NodeTable *nt = f->nt;
  if (blk >= 0) return 0;
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_SymbolNode || k == NK_BlockArgumentNode || k == NK_SplatNode) return 0;
  }
  return 1;
}
static void cf_builtin_rule(CF *f, int v, int recv_el, const int *argv, int argc, int blk, const char *nm) {
  const NodeTable *nt = f->nt;
  if (cf_plain_builtin_call(f, argv, argc, blk)) {
    if (cf_scalar_answer(nm)) {
      cf_add_set(f, v, cf_bv_set(f));
      return;
    }
    if (cf_element_answer(nm)) {
      cf_unite(f, v, recv_el);
      cf_add_set(f, v, cf_bv_set(f));
      cf_protocol_returns(f, v, recv_el);
      return;
    }
  }
  cf_unite(f, v, recv_el);
  cf_add_set(f, v, cf_bv_set(f));
  cf_protocol_returns(f, v, recv_el);
  for (int i = 0; i < argc; i++)
    if (nt_kind(nt, argv[i]) != NK_BlockArgumentNode && nt_kind(nt, argv[i]) != NK_SymbolNode)
      cf_protocol_returns(f, v, nt_kind(nt, argv[i]) == NK_SplatNode ? nt_ref(nt, argv[i], "expression") : argv[i]);
  for (int i = 0; i < argc; i++) {
    NodeKind k = nt_kind(nt, argv[i]);
    if (k == NK_BlockArgumentNode) {
      int e = nt_ref(nt, argv[i], "expression");
      if (e >= 0 && nt_kind(nt, e) == NK_SymbolNode) {
        const char *sym = nt_str(nt, e, "value");
        if (!sym || sp_streq(sym, "new") || sp_streq(sym, "allocate") || cf_opaque_name(sym)) cf_all(f, v);
        else { CFByName *b = cf_byname(f, sym); for (int q = 0; q < b->n; q++) cf_unite(f, v, f->ret_el[b->mi[q]]); }
      }
      else cf_unite(f, v, e);
      continue;
    }
    if (k == NK_SymbolNode) {
      /* `to_enum(:m)`: the Enumerator yields what m yields, so its values
         are m's yield arguments (as each's are, cf_protocol_returns) */
      if (i == 0 && (sp_streq(nm, "to_enum") || sp_streq(nm, "enum_for"))) {
        CFByName *b = cf_byname(f, nt_str(nt, argv[i], "value"));
        for (int q = 0; q < b->n; q++) for (int y = 0; y < CF_YARGS; y++) cf_unite(f, v, f->yarg_el[b->mi[q]][y]);
      }
      continue;
    }
    cf_unite(f, v, k == NK_SplatNode ? nt_ref(nt, argv[i], "expression") : argv[i]);
  }
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) {
    if (!cf_drops_block_value(nm)) cf_unite(f, v, blk);
    int body = nt_ref(nt, blk, "body");
    const char *names[64];
    int n = cf_all_param_names(f, blk, names, 64);
    for (int i = 0; i < n; i++) cf_unite(f, v, cf_lv(f, body >= 0 ? body : blk, names[i]));
  }
  else if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
    int e = nt_ref(nt, blk, "expression");
    if (e >= 0 && nt_kind(nt, e) == NK_SymbolNode) {
      const char *sym = nt_str(nt, e, "value");
      if (!sym || sp_streq(sym, "new") || sp_streq(sym, "allocate") || cf_opaque_name(sym)) cf_all(f, v);
      else { CFByName *b = cf_byname(f, sym); for (int q = 0; q < b->n; q++) cf_unite(f, v, f->ret_el[b->mi[q]]); }
    }
    else cf_unite(f, v, e);
  }
  (void)nm;
}

/* a user method target: its value, its parameters, its block; 0 for a
   method the compiler supplies, which answers as the builtin it is */
static int cf_user_target(CF *f, int v, int mi, const int *argv, int argc, int blk) {
  if (mi < 0) return 0;
  if (cf_prelude_scope(f, mi)) return 0;
  if (v >= 0) cf_unite(f, v, f->ret_el[mi]);
  cf_bind_args(f, mi, argv, argc);
  cf_bind_block(f, mi, blk);
  return 1;
}

/* `X.new(...)`: an instance of the user class X, initialized */
static void cf_construct(CF *f, int v, int x, const int *argv, int argc, int blk, const char *nm) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  cf_add_set(f, v, f->own[x]);
  ClassInfo *k = &c->classes[x];
  int cn = sp_streq(nm, "new") ? comp_cmethod_in_chain(c, x, "new", NULL) : -1;
  if (cn >= 0 && cf_user_target(f, v, cn, argv, argc, blk)) return;
  if (sp_streq(nm, "allocate")) return;
  int init = comp_method_in_chain(c, x, "initialize", NULL);
  if (init >= 0) { cf_user_target(f, -1, init, argv, argc, blk); return; }
  if (k->is_struct || k->is_data) {
    /* the members, in order, or by keyword */
    int kwh = -1, p = 0;
    for (int i = 0; i < argc; i++) {
      NodeKind ak = nt_kind(nt, argv[i]);
      if (ak == NK_KeywordHashNode) { kwh = argv[i]; continue; }
      if (ak == NK_BlockArgumentNode) continue;
      if (ak == NK_SplatNode) {
        for (int r = 0; r < k->nreaders; r++) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", k->readers[r]); cf_unite(f, cf_entry(f, f->fam[x], ivn), nt_ref(nt, argv[i], "expression")); }
        continue;
      }
      if (p < k->nreaders) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", k->readers[p]); cf_unite(f, cf_entry(f, f->fam[x], ivn), argv[i]); }
      p++;
    }
    if (kwh >= 0) {
      int en = 0; const int *els = nt_arr(nt, kwh, "elements", &en);
      for (int e = 0; e < en; e++) {
        int key = nt_kind(nt, els[e]) == NK_AssocNode ? nt_ref(nt, els[e], "key") : -1;
        const char *kn = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
        int val = nt_ref(nt, els[e], "value");
        if (kn) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kn); cf_unite(f, cf_entry(f, f->fam[x], ivn), val); }
        else for (int r = 0; r < k->nreaders; r++) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", k->readers[r]); cf_unite(f, cf_entry(f, f->fam[x], ivn), val); }
      }
    }
  }
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
}

/* the call on a receiver whose classes are known (set pidx, not every
   class): each class's method, reader or writer; the builtin answer where
   one has none */
static void cf_call_known(CF *f, int v, int pidx, int recv_el, const char *nm, const int *argv, int argc, int blk) {
  Compiler *c = f->c;
  const ClsSet *s = &f->pool[pidx];
  int builtin = pidx == CF_EMPTY || s->bv;
  char base[256] = "";
  int is_w = setter_base_name(nm, base, sizeof base);
  for (int i = 0; i < s->n; i++) {
    int k = s->cls[i];
    if (k >= c->nclasses) {
      /* the Class object: a construction, a class method, else Class's own */
      int kc = k - c->nclasses;
      if (kc < 0 || kc >= c->nclasses) { builtin = 1; continue; }
      if (sp_streq(nm, "new") || sp_streq(nm, "allocate") ||
          ((c->classes[kc].is_struct || c->classes[kc].is_data) && sp_streq(nm, "[]"))) {
        cf_construct(f, v, kc, argv, argc, blk, nm);
        continue;
      }
      int cm = comp_cmethod_in_chain(c, kc, nm, NULL);
      if (cm >= 0 && cf_user_target(f, v, cm, argv, argc, blk)) continue;
      if (cm < 0 && cf_sg_access(f, v, kc, nm, argv, argc)) continue;
      if (cm < 0 && f->mod[kc]) { int mm = comp_method_in_chain(c, kc, nm, NULL); if (mm >= 0 && cf_user_target(f, v, mm, argv, argc, blk)) continue; }
      if (cf_class_has_natives(f, kc)) { cf_all(f, v); continue; }
      builtin = 1;
      continue;
    }
    if (k < 0) { builtin = 1; continue; }
    int mi = comp_method_in_chain(c, k, nm, NULL);
    if (mi >= 0) { if (!cf_user_target(f, v, mi, argv, argc, blk)) builtin = 1; continue; }
    if (comp_reader_in_chain(c, k, nm, NULL)) {
      /* the reader an alias names reads its own ivar (`alias loc location`) */
      char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", comp_resolve_alias(c, k, nm));
      cf_unite(f, v, cf_entry(f, f->fam[k], ivn));
      continue;
    }
    if (is_w && comp_writer_in_chain(c, k, base, NULL)) {
      char ivn[sizeof base + 1]; snprintf(ivn, sizeof ivn, "@%s", base);
      for (int a = 0; a < argc; a++) { cf_unite(f, cf_entry(f, f->fam[k], ivn), argv[a]); cf_unite(f, v, argv[a]); }
      continue;
    }
    int tl = comp_method_index(c, nm);
    if (tl >= 0 && cf_user_target(f, v, tl, argv, argc, blk)) continue;
    if (comp_method_in_chain(c, k, "method_missing", NULL) >= 0) {
      cf_all(f, v);
      for (int a = 0; a < argc; a++) cf_all(f, argv[a]);
      if (blk >= 0 && nt_kind(f->nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
      continue;
    }
    /* a native binding of the class: anything */
    if (cf_class_has_natives(f, k)) {
      cf_all(f, v);
      if (blk >= 0 && nt_kind(f->nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
      continue;
    }
    /* a builtin method of the object: a copy, a conversion, a Struct's
       members, Enumerable over the class's own each */
    builtin = 1;
    ClassInfo *ci = &c->classes[k];
    if (ci->is_struct || ci->is_data)
      for (int r = 0; r < ci->nreaders; r++) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", ci->readers[r]); cf_unite(f, v, cf_entry(f, f->fam[k], ivn)); }
    int each = comp_method_in_chain(c, k, "each", NULL);
    if (each >= 0) for (int y = 0; y < CF_YARGS; y++) cf_unite(f, v, f->yarg_el[each][y]);
  }
  if (builtin) {
    /* Deferred until the classes have settled: the builtin answer unifies
       the receiver with the call's value and arguments, which a receiver
       whose class is known only later (its write is a later node, or a
       method's value) would then share with that class's methods. */
    if (!f->phase) { f->pending++; return; }
    if (argc > 0 && (sp_streq(nm, "resume") || sp_streq(nm, "transfer"))) cf_all(f, f->fiber_ch);
    if (cf_opaque_name(nm) || sp_streq(nm, "new") || sp_streq(nm, "allocate")) {
      cf_all(f, v);
      for (int a = 0; a < argc; a++) cf_all(f, argv[a]);
      if (blk >= 0 && nt_kind(f->nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
      if (sp_streq(nm, "new")) { CFByName *b = cf_byname(f, "initialize"); for (int q = 0; q < b->n; q++) cf_bind_args(f, b->mi[q], argv, argc); }
      return;
    }
    /* `.class`: the Class objects of the receiver's classes */
    if (sp_streq(nm, "class") && argc == 0) cf_add_set(f, v, cf_classvals_of(f, pidx));
    /* a method the program adds to a runtime class or module it reopens
       answers for the runtime's values too (`class Object; def tagged`,
       `42.tagged`) */
    for (int i = 0; i < f->nopen; i++) {
      int mi = comp_method_in_chain(c, f->open[i], nm, NULL);
      if (mi >= 0) cf_user_target(f, v, mi, argv, argc, blk);
    }
    cf_builtin_rule(f, v, recv_el, argv, argc, blk, nm);
  }
}

/* the call on a receiver that can be anything: every method of the name
   takes the arguments and the block; the value is anything */
static void cf_call_any(CF *f, int v, const char *nm, const int *argv, int argc, int blk) {
  Compiler *c = f->c;
  cf_all(f, v);
  if (argc > 0 && (sp_streq(nm, "resume") || sp_streq(nm, "transfer"))) cf_all(f, f->fiber_ch);
  CFByName *b = cf_byname(f, nm);
  for (int i = 0; i < b->n; i++) { cf_bind_args(f, b->mi[i], argv, argc); cf_bind_block(f, b->mi[i], blk); }
  for (int i = 0; i < b->ncm; i++) { cf_bind_args(f, b->cmi[i], argv, argc); cf_bind_block(f, b->cmi[i], blk); }
  for (int i = 0; i < b->nwr; i++) {
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%.*s", (int)strlen(nm) - 1, nm);
    for (int a = 0; a < argc; a++) cf_unite(f, cf_entry(f, f->fam[b->wr[i]], ivn), argv[a]);
  }
  if (sp_streq(nm, "new")) { CFByName *ini = cf_byname(f, "initialize"); for (int q = 0; q < ini->n; q++) cf_bind_args(f, ini->mi[q], argv, argc); }
  /* the block's parameters and the arguments may go anywhere */
  if (blk >= 0 && nt_kind(f->nt, blk) == NK_BlockNode) { cf_all_block_params(f, blk); cf_unite(f, v, blk); }
  for (int a = 0; a < argc; a++) {
    NodeKind k = nt_kind(f->nt, argv[a]);
    if (k == NK_BlockArgumentNode || k == NK_SymbolNode) continue;
    cf_all(f, k == NK_SplatNode ? nt_ref(f->nt, argv[a], "expression") : argv[a]);
  }
  (void)c;
}

static void cf_send_writers(CF *f, int v, int r) {
  Compiler *c = f->c;
  Scope *sc = &c->scopes[cf_scope_of(f, v)];
  int ks = r >= 0 ? cf_set_of(f, r) : sc->def_node >= 0 && sc->class_id >= 0 ? f->desc[sc->class_id] : CF_EMPTY;
  const ClsSet *s = &f->pool[ks];
  for (int k = 0; k < c->nclasses; k++) {
    int in = s->all;
    for (int i = 0; i < s->n && !in; i++) in = s->cls[i] == k;
    for (int x = in ? k : -1; x >= 0; x = c->classes[x].parent)
      for (int w = 0; w < c->classes[x].nwriters; w++) {
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", c->classes[x].writers[w]);
        cf_all(f, cf_entry(f, f->fam[k], ivn));
      }
  }
}

static void cf_call(CF *f, int v) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  const char *nm = nt_str(nt, v, "name");
  int r = nt_ref(nt, v, "receiver");
  int a = nt_ref(nt, v, "arguments"), argc = 0;
  const int *argv = a >= 0 ? nt_arr(nt, a, "arguments", &argc) : NULL;
  int blk = nt_ref(nt, v, "block");
  if (!nm) { cf_all(f, v); return; }
  if (cf_opaque_name(nm)) {
    cf_all(f, v);
    for (int i = 0; i < argc; i++) cf_all(f, argv[i]);
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) { cf_all_block_params(f, blk); cf_all(f, blk); }
    if (argc >= 2 && (sp_streq(nm, "send") || sp_streq(nm, "__send__") || sp_streq(nm, "public_send")) &&
        nt_kind(nt, argv[0]) != NK_SymbolNode && nt_kind(nt, argv[0]) != NK_StringNode)
      cf_send_writers(f, v, r);
    return;
  }
  /* an alias that captured a builtin (`alias old_dup dup`, #8914) calls the
     builtin: not the method the class defines of the name after it, whose
     answer is not the receiver */
  if (nt_int(nt, v, "builtin_only", 0)) {
    int recv_el = r;
    if (r < 0) {
      int ik = cf_inst_class(f, cf_scope_of(f, v));
      recv_el = ik >= 0 && !cf_selfless_fn(nm) ? f->self_el[ik] : -1;
    }
    cf_builtin_rule(f, v, recv_el, argv, argc, blk, nm);
    return;
  }
  if (r < 0) {
    /* a call on self: the method of self's class, or of a subclass whose
       instance runs this code; else the top-level def; else Kernel's */
    int si = cf_scope_of(f, v);
    Scope *s = &c->scopes[si];
    int found = 0;
    if (s->def_node >= 0 && s->class_id >= 0) {
      const ClsSet *ds = &f->pool[f->desc[s->class_id]];
      if (ds->all) { cf_call_any(f, v, nm, argv, argc, blk); return; }
      /* `new` or `allocate` in a class method: an instance of the class
         the method runs for, which a subclass's call makes its own */
      if (s->is_cmethod && (sp_streq(nm, "new") || sp_streq(nm, "allocate")) &&
          comp_cmethod_in_chain(c, s->class_id, nm, NULL) < 0) {
        cf_add_set(f, v, f->desc[s->class_id]);
        /* each class as `X.new` builds it: its initialize, or a Struct's
           or Data's members, which have none */
        for (int i = 0; i < ds->n; i++) cf_construct(f, -1, ds->cls[i], argv, argc, blk, nm);
        return;
      }
      if (s->is_cmethod) {
        int cm = comp_cmethod_in_chain(c, s->class_id, nm, NULL);
        if (cm >= 0 && cf_user_target(f, v, cm, argv, argc, blk)) found = 1;
        for (int i = 0; i < ds->n && !found; i++) {
          int cm2 = comp_cmethod_in_chain(c, ds->cls[i], nm, NULL);
          if (cm2 >= 0 && cf_user_target(f, v, cm2, argv, argc, blk)) found = 1;
        }
        /* a singleton accessor of the class self is, or of a subclass */
        if (!found && cf_sg_access(f, v, s->class_id, nm, argv, argc)) found = 1;
        for (int i = 0; i < ds->n; i++) if (ds->cls[i] != s->class_id && cf_sg_access(f, v, ds->cls[i], nm, argv, argc)) found = 1;
      }
      else {
        for (int i = 0; i < ds->n; i++) {
          int mi = comp_method_in_chain(c, ds->cls[i], nm, NULL);
          if (mi >= 0) { if (cf_user_target(f, v, mi, argv, argc, blk)) found = 1; continue; }
          if (comp_reader_in_chain(c, ds->cls[i], nm, NULL)) {
            char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", comp_resolve_alias(c, ds->cls[i], nm));
            cf_unite(f, v, cf_entry(f, f->fam[ds->cls[i]], ivn));
            found = 1;
          }
        }
        if (!found) {
          int mi = comp_method_in_chain(c, s->class_id, nm, NULL);
          if (mi >= 0 && cf_user_target(f, v, mi, argv, argc, blk)) found = 1;
        }
        if (!found && f->has_mm) {
          for (int i = 0; i < ds->n && !found; i++)
            if (comp_method_in_chain(c, ds->cls[i], "method_missing", NULL) >= 0) { cf_call_any(f, v, nm, argv, argc, blk); return; }
        }
      }
    }
    else if (v < f->nnodes && c->node_cbody && c->node_cbody[v] >= 0) {
      /* a class body: the class's own class methods (`attr_accessor`,
         `include` are builtins of Module); a bare `new` is the class's */
      int cb = c->node_cbody[v];
      if (sp_streq(nm, "new") || sp_streq(nm, "allocate")) { cf_construct(f, v, cb, argv, argc, blk, nm); return; }
      int cm = comp_cmethod_in_chain(c, cb, nm, NULL);
      if (cm >= 0 && cf_user_target(f, v, cm, argv, argc, blk)) found = 1;
    }
    if (!found) {
      int tl = comp_method_index(c, nm);
      if (tl >= 0 && cf_user_target(f, v, tl, argv, argc, blk)) found = 1;
    }
    /* a module included at top level is Object's, so its method answers a
       bare call no class or top-level def did */
    for (int k = 0; k < c->ntoplevel_includes && !found; k++) {
      int mi = comp_method_in_chain(c, c->toplevel_includes[k], nm, NULL);
      if (mi >= 0 && cf_user_target(f, v, mi, argv, argc, blk)) found = 1;
    }
    if (found) return;
    /* a native binding of the class (an ffi_func): anything */
    if (s->def_node >= 0 && s->class_id >= 0) {
      const ClsSet *ds = &f->pool[f->desc[s->class_id]];
      int nat = cf_class_has_natives(f, s->class_id);
      for (int i = 0; i < ds->n && !nat; i++) nat = cf_class_has_natives(f, ds->cls[i]);
      if (nat) {
        cf_all(f, v);
        if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
        return;
      }
    }
    if (cf_owner_block(f, v, 0) >= 0) {
      CFByName *b = cf_byname(f, nm);
      if (b->n || b->ncm || b->nrd || b->nwr) { cf_call_any(f, v, nm, argv, argc, blk); return; }
    }
    /* Kernel: a conversion, an iteration, `lambda`, `proc`, `loop`, `raise`
       hand on what they are given; `new` has no class here */
    if (sp_streq(nm, "lambda") || sp_streq(nm, "proc") || sp_streq(nm, "define_method")) {
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
    }
    /* self is the receiver (`def seal = freeze` answers self, a bare
       `each` in an Array subclass iterates self) */
    int ik = cf_inst_class(f, si);
    cf_builtin_rule(f, v, ik >= 0 && !cf_selfless_fn(nm) ? f->self_el[ik] : -1, argv, argc, blk, nm);
    return;
  }
  int x = cf_is_class_const(f, r);
  if (x >= 0) {
    ClassInfo *k = &c->classes[x];
    if (sp_streq(nm, "new") || sp_streq(nm, "allocate") || ((k->is_struct || k->is_data) && sp_streq(nm, "[]"))) {
      cf_construct(f, v, x, argv, argc, blk, nm);
      return;
    }
    int cm = comp_cmethod_in_chain(c, x, nm, NULL);
    if (cm >= 0 && cf_user_target(f, v, cm, argv, argc, blk)) return;
    if (cm < 0 && cf_sg_access(f, v, x, nm, argv, argc)) return;
    if (cm < 0 && f->mod[x]) {
      /* a module function */
      int mi = comp_method_in_chain(c, x, nm, NULL);
      if (mi >= 0 && cf_user_target(f, v, mi, argv, argc, blk)) return;
    }
    /* a native binding of the class or module (an ffi_func, a Fiddle
       extern): its value is anything */
    if (cf_class_has_natives(f, x)) {
      cf_all(f, v);
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
      return;
    }
    /* a method of Class or Module: nothing of the program's own comes out,
       except through a block */
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) cf_all_block_params(f, blk);
    cf_builtin_rule(f, v, -1, argv, argc, blk, nm);
    return;
  }
  NodeKind rk = nt_kind(nt, r);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = nt_str(nt, r, "name");
    const char *key = cf_const_key(cn);
    if (cn && (cf_poison_const(cn) || (key && cf_poison_const(key)))) {
      /* its value, and its block's parameters, come from anywhere; its
         arguments are only stored (a Fiber's yield hands them to a resume,
         whose value is one of these). Fiber.yield answers what a resume
         passes: the channel, which a resume with arguments makes anything
         (the enumerators the compiler makes resume with none). */
      if (key && sp_streq(key, "Fiber") && sp_streq(nm, "yield")) { cf_unite(f, v, f->fiber_ch); return; }
      cf_all(f, v);
      if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) { cf_all_block_params(f, blk); cf_all(f, blk); }
      return;
    }
    /* a constant holding a value of the program's own (`REG = Registry.new`)
       is a receiver like any other; a builtin class or module answers
       builtin values, built from the arguments (`Set.new(list)`,
       `Array.new(n, x)`, `Hash.new { }`), its block's parameters taking
       anything (`Proc.new { |x| }`, `Hash.new { |h, k| }`) */
    int prog = cf_entry_has(f, CF_K_CONSTW, key);
    int ce = prog ? cf_entry(f, CF_K_CONST, key) : -1;
    int pidx = prog ? cf_set_of(f, ce) : CF_EMPTY;
    if (pidx != CF_EMPTY) {
      if (pidx == CF_ALL) { cf_call_any(f, v, nm, argv, argc, blk); return; }
      cf_call_known(f, v, pidx, ce, nm, argv, argc, blk);
      return;
    }
    /* a Proc's parameters take what its calls pass; an Enumerator's yielder,
       a Hash's default block's hash and the rest are the builtin's own */
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && sp_streq(nm, "new") && key && sp_streq(key, "Proc")) cf_all_block_params(f, blk);
    cf_builtin_rule(f, v, ce, argv, argc, blk, nm);
    return;
  }
  if (rk == NK_SelfNode && (sp_streq(nm, "new") || sp_streq(nm, "allocate"))) {
    /* `self.new` in a class method: as the bare call above */
    int si = cf_scope_of(f, v);
    Scope *s = &c->scopes[si];
    if (s->def_node >= 0 && s->is_cmethod && s->class_id >= 0 && comp_cmethod_in_chain(c, s->class_id, nm, NULL) < 0) {
      const ClsSet *ds = &f->pool[f->desc[s->class_id]];
      cf_add_set(f, v, f->desc[s->class_id]);
      if (ds->all) { for (int i = 0; i < argc; i++) cf_all(f, argv[i]); return; }
      for (int i = 0; i < ds->n; i++) cf_construct(f, -1, ds->cls[i], argv, argc, blk, nm);
      return;
    }
  }
  int recv_el = r;
  int pidx = cf_set_of(f, recv_el);
  if (pidx == CF_ALL) { cf_call_any(f, v, nm, argv, argc, blk); return; }
  cf_call_known(f, v, pidx, recv_el, nm, argv, argc, blk);
}

/* an attribute or index write through a call: `o.x ||= v`, `h[k] += v`
   (`o.x += v` is a read and a write by then). The node's value is the
   read, the right side and an operator's answer; that is what the write
   stores. */
static void cf_op_answer(CF *f, int v, const char *op, int val) {
  if (!op) return;
  int pidx = cf_set_of(f, v), argv[1] = { val };
  if (pidx == CF_ALL) cf_call_any(f, v, op, argv, 1, -1);
  else cf_call_known(f, v, pidx, v, op, argv, 1, -1);
}
static void cf_call_write(CF *f, int v) {
  const NodeTable *nt = f->nt;
  NodeKind k = nt_kind(nt, v);
  int r = nt_ref(nt, v, "receiver");
  int val = nt_ref(nt, v, "value");
  const char *op = k == NK_IndexOperatorWriteNode ? nt_str(nt, v, "binary_operator") : NULL;
  cf_unite(f, v, val);
  if (r < 0) { cf_all(f, v); return; }
  int argv[33], argc = 0;
  if (k == NK_IndexAndWriteNode || k == NK_IndexOrWriteNode || k == NK_IndexOperatorWriteNode) {
    int a = nt_ref(nt, v, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int i = 0; i < an && argc < 32; i++) argv[argc++] = av[i];
    int pidx = cf_set_of(f, r);
    if (pidx == CF_ALL) cf_call_any(f, v, "[]", argv, argc, -1);
    else cf_call_known(f, v, pidx, r, "[]", argv, argc, -1);
    cf_op_answer(f, v, op, val);
    argv[argc++] = v;
    if (pidx == CF_ALL) cf_call_any(f, v, "[]=", argv, argc, -1);
    else cf_call_known(f, v, pidx, r, "[]=", argv, argc, -1);
    return;
  }
  /* the parser keeps the reader's name; the writer's is it with `=` */
  const char *rn = nt_str(nt, v, "name");
  char wn[256];
  if (!rn || strlen(rn) + 2 > sizeof wn) { cf_all(f, v); return; }
  snprintf(wn, sizeof wn, "%s=", rn);
  int x = cf_is_class_const(f, r);
  if (x >= 0) {
    int cm = comp_cmethod_in_chain(f->c, x, rn, NULL);
    if (cm >= 0) cf_user_target(f, v, cm, NULL, 0, -1);
    else cf_sg_access(f, v, x, rn, NULL, 0);
    cf_op_answer(f, v, op, val);
    argv[argc++] = v;
    cm = comp_cmethod_in_chain(f->c, x, wn, NULL);
    if (cm >= 0) cf_user_target(f, v, cm, argv, argc, -1);
    else cf_sg_access(f, v, x, wn, argv, argc);
    return;
  }
  int pidx = cf_set_of(f, r);
  if (pidx == CF_ALL) { cf_call_any(f, v, rn, NULL, 0, -1); cf_op_answer(f, v, op, val); argv[argc++] = v; cf_call_any(f, v, wn, argv, argc, -1); return; }
  cf_call_known(f, v, pidx, r, rn, NULL, 0, -1);
  cf_op_answer(f, v, op, val);
  argv[argc++] = v;
  cf_call_known(f, v, pidx, r, wn, argv, argc, -1);
}

/* ---- the node walk ---- */

static void cf_rescue(CF *f, int v) {
  const NodeTable *nt = f->nt;
  int ref = nt_ref(nt, v, "reference");
  if (ref < 0) return;
  int el = -1;
  NodeKind rk = nt_kind(nt, ref);
  if (rk == NK_LocalVariableTargetNode) el = cf_lv(f, ref, nt_str(nt, ref, "name"));
  else if (rk == NK_InstanceVariableTargetNode) { int els[64]; int n = cf_ivar_els(f, ref, nt_str(nt, ref, "name"), els, 64); for (int i = 0; i < n; i++) cf_all(f, els[i]); return; }
  else return;
  if (el < 0) return;
  int en = 0; const int *ex = nt_arr(nt, v, "exceptions", &en);
  if (en == 0) { cf_add_set(f, el, f->exc_set); return; }
  for (int i = 0; i < en; i++) {
    int x = cf_is_class_const(f, ex[i]);
    if (x >= 0) cf_add_set(f, el, f->desc[x]);
    else if (nt_kind(nt, ex[i]) == NK_ConstantReadNode || nt_kind(nt, ex[i]) == NK_ConstantPathNode) cf_add_set(f, el, f->exc_set);  /* a builtin class: its user subclasses too */
    else cf_all(f, el);
  }
}

static void cf_node(CF *f, int v) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  NodeKind k = nt_kind(nt, v);
  switch (k) {
  case NK_NONE:
  case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_IntegerNode: case NK_FloatNode:
  case NK_RationalNode: case NK_ImaginaryNode: case NK_StringNode: case NK_SymbolNode:
  case NK_InterpolatedStringNode: case NK_InterpolatedSymbolNode: case NK_XStringNode:
  case NK_InterpolatedXStringNode: case NK_RegularExpressionNode: case NK_InterpolatedRegularExpressionNode:
  case NK_SourceFileNode: case NK_SourceLineNode: case NK_SourceEncodingNode: case NK_DefinedNode:
  case NK_DefNode: case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode: case NK_AliasMethodNode:
  case NK_AliasGlobalVariableNode: case NK_UndefNode: case NK_MatchPredicateNode: case NK_MatchRequiredNode:
  case NK_RedoNode: case NK_RetryNode: case NK_BackReferenceReadNode: case NK_NumberedReferenceReadNode:
  case NK_ParametersNode: case NK_BlockParametersNode: case NK_NumberedParametersNode:
  case NK_RequiredParameterNode: case NK_RestParameterNode: case NK_KeywordRestParameterNode:
  case NK_BlockParameterNode: case NK_InNode: case NK_EmbeddedStatementsNode:
  case NK_PreExecutionNode: case NK_PostExecutionNode: case NK_PinnedExpressionNode: case NK_PinnedVariableNode:
  case NK_ConstantPathTargetNode: case NK_CallTargetNode: case NK_IndexTargetNode:
    return;
  case NK_OptionalParameterNode: case NK_OptionalKeywordParameterNode: {
    /* a default: into the parameter it defaults */
    int owner = cf_owner_block(f, v, 0);
    int d = cf_owner_def(f, v, 1, NULL);
    const char *nm = nt_str(nt, v, "name");
    int el = owner >= 0 && nt_kind(nt, owner) == NK_BlockNode ? cf_lv(f, nt_ref(nt, owner, "body") >= 0 ? nt_ref(nt, owner, "body") : owner, nm)
           : d >= 0 ? cf_lv_in(f, d, nm) : cf_lv(f, v, nm);
    cf_unite(f, el, nt_ref(nt, v, "value"));
    return;
  }
  case NK_SelfNode: {
    if (cf_in_exec_block(f, v) || nt_int(nt, v, "ie_rself", 0)) { cf_all(f, v); return; }
    int si = cf_scope_of(f, v);
    /* a self a pass gave a call as its receiver after the scopes were
       walked (`r` -> `self.r`) has none recorded: the call's is its */
    if (si == 0 && f->par && v < f->nnodes && f->par[v] >= 0) si = cf_scope_of(f, f->par[v]);
    /* `def (self.class).m`: the receiver is the enclosing scope's self */
    for (int x = v, q = f->par ? f->par[v] : -1; q >= 0; x = q, q = f->par[q]) {
      if (nt_kind(nt, q) != NK_DefNode) continue;
      if (nt_ref(nt, q, "receiver") == x && f->par[q] >= 0) si = cf_scope_of(f, f->par[q]);
      break;
    }
    int ik = cf_inst_class(f, si);
    if (ik >= 0) { cf_unite(f, v, f->self_el[ik]); return; }
    Scope *s = &c->scopes[si];
    /* the Class object: of the class a class method runs for, or one below
       it; the class whose body this is */
    if (s->def_node >= 0 && s->is_cmethod && s->class_id >= 0) {
      /* a module's own (a module_function's, an `extend self`'s) runs for
         the module too, which its subtree does not list; a reopened
         runtime class's for its classes the program does not list */
      if (cf_open_self(f, s->class_id)) { cf_all(f, v); return; }
      cf_add_set(f, v, cf_classvals_of(f, f->desc[s->class_id]));
      if (f->mod[s->class_id]) cf_add_set(f, v, cf_classval(f, s->class_id));
    }
    else if (v < f->nnodes && c->node_cbody && c->node_cbody[v] >= 0) cf_add_set(f, v, cf_classval(f, c->node_cbody[v]));
    else if (s->def_node < 0 && cf_owner_block(f, v, 0) >= 0) cf_all(f, v);
    return;
  }
  case NK_ArrayNode: {
    cf_add_set(f, v, cf_bv_set(f));
    int n = 0; const int *els = nt_arr(nt, v, "elements", &n);
    for (int i = 0; i < n; i++) cf_unite(f, v, nt_kind(nt, els[i]) == NK_SplatNode ? nt_ref(nt, els[i], "expression") : els[i]);
    return;
  }
  case NK_HashNode: case NK_KeywordHashNode: {
    cf_add_set(f, v, cf_bv_set(f));
    int n = 0; const int *els = nt_arr(nt, v, "elements", &n);
    for (int i = 0; i < n; i++) {
      if (nt_kind(nt, els[i]) == NK_AssocNode) cf_unite(f, v, nt_ref(nt, els[i], "key"));
      cf_unite(f, v, nt_ref(nt, els[i], "value"));
    }
    return;
  }
  case NK_AssocNode: case NK_AssocSplatNode: return;
  case NK_SplatNode: cf_unite(f, v, nt_ref(nt, v, "expression")); return;
  case NK_BlockArgumentNode: return;
  case NK_RangeNode: cf_add_set(f, v, cf_bv_set(f)); cf_unite(f, v, nt_ref(nt, v, "left")); cf_unite(f, v, nt_ref(nt, v, "right")); return;
  case NK_ParenthesesNode: cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "body"))); return;
  case NK_StatementsNode: cf_unite(f, v, cf_tail(f, v)); return;
  case NK_ElseNode: cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "statements"))); return;
  case NK_BeginNode: {
    cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "statements")));
    cf_unite(f, v, nt_ref(nt, v, "else_clause"));
    for (int rc = nt_ref(nt, v, "rescue_clause"), d = 0; rc >= 0 && d < 64; rc = nt_ref(nt, rc, "subsequent"), d++)
      cf_unite(f, v, cf_tail(f, nt_ref(nt, rc, "statements")));
    return;
  }
  case NK_RescueNode: cf_rescue(f, v); return;

  case NK_IfNode: case NK_UnlessNode: {
    cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "statements")));
    cf_unite(f, v, nt_ref(nt, v, k == NK_IfNode ? "subsequent" : "else_clause"));
    return;
  }
  case NK_CaseNode: case NK_CaseMatchNode: {
    cf_unite(f, v, nt_ref(nt, v, "else_clause"));
    int wn = 0; const int *w = nt_arr(nt, v, "conditions", &wn);
    for (int i = 0; i < wn; i++) cf_unite(f, v, cf_tail(f, nt_ref(nt, w[i], "statements")));
    return;
  }
  case NK_AndNode: case NK_OrNode: cf_unite(f, v, nt_ref(nt, v, "left")); cf_unite(f, v, nt_ref(nt, v, "right")); return;
  case NK_RescueModifierNode: cf_unite(f, v, nt_ref(nt, v, "expression")); cf_unite(f, v, nt_ref(nt, v, "rescue_expression")); return;
  /* nil, or what a break hands it (NK_BreakNode) */
  case NK_WhileNode: case NK_UntilNode: return;
  case NK_ForNode: {
    /* the collection it walked, or what a break hands it */
    cf_unite(f, v, nt_ref(nt, v, "collection"));
    int idx = nt_ref(nt, v, "index");
    if (idx >= 0 && nt_kind(nt, idx) == NK_LocalVariableTargetNode) cf_unite(f, cf_lv(f, idx, nt_str(nt, idx, "name")), nt_ref(nt, v, "collection"));
    return;
  }
  case NK_LocalVariableWriteNode: case NK_LocalVariableOrWriteNode: case NK_LocalVariableAndWriteNode: {
    int val = nt_ref(nt, v, "value");
    int el = cf_lv(f, v, nt_str(nt, v, "name"));
    cf_unite(f, v, val);
    if (el < 0) cf_all(f, v); else cf_unite(f, el, val);
    return;
  }
  case NK_LocalVariableOperatorWriteNode: {
    /* `x op= y`: the operator's value, a builtin's or the class's own */
    int val = nt_ref(nt, v, "value");
    int el = cf_lv(f, v, nt_str(nt, v, "name"));
    const char *op = nt_str(nt, v, "binary_operator");
    if (el < 0 || !op) { cf_all(f, v); return; }
    int argv1[1] = { val };
    int pidx = cf_set_of(f, el);
    if (pidx == CF_ALL) cf_all(f, v);
    else cf_call_known(f, v, pidx, el, op, argv1, 1, -1);
    cf_unite(f, el, v);
    return;
  }
  case NK_LocalVariableTargetNode: {
    int p = f->par ? f->par[v] : -1;
    if (p >= 0 && (nt_kind(nt, p) == NK_RescueNode || nt_kind(nt, p) == NK_ForNode)) return;
    cf_all(f, cf_lv(f, v, nt_str(nt, v, "name")));
    return;
  }
  case NK_LocalVariableReadNode: {
    int el = cf_lv(f, v, nt_str(nt, v, "name"));
    if (el < 0) cf_all(f, v); else cf_unite(f, v, el);
    return;
  }
  case NK_InstanceVariableWriteNode: case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode:
  case NK_InstanceVariableOperatorWriteNode: {
    int val = nt_ref(nt, v, "value");
    if (cf_in_exec_block(f, v)) {
      cf_unite(f, v, val);
      if (k != NK_InstanceVariableWriteNode) cf_all(f, v);
      cf_ivar_write_any(f, nt_str(nt, v, "name"), v);
      return;
    }
    int els[64]; int n = cf_ivar_els(f, v, nt_str(nt, v, "name"), els, 64);
    if (k == NK_InstanceVariableOperatorWriteNode) {
      const char *op = nt_str(nt, v, "binary_operator");
      int argv1[1] = { val };
      int pidx = n > 0 ? cf_set_of(f, els[0]) : CF_ALL;
      if (pidx == CF_ALL || !op) cf_all(f, v); else cf_call_known(f, v, pidx, els[0], op, argv1, 1, -1);
    }
    else cf_unite(f, v, val);
    for (int i = 0; i < n; i++) cf_unite(f, els[i], v);
    if (f->all_ivars) cf_all(f, v);
    return;
  }
  case NK_InstanceVariableTargetNode: {
    int p = f->par ? f->par[v] : -1;
    if (p >= 0 && nt_kind(nt, p) == NK_RescueNode) return;
    int els[64]; int n = cf_ivar_els(f, v, nt_str(nt, v, "name"), els, 64);
    for (int i = 0; i < n; i++) cf_all(f, els[i]);
    return;
  }
  case NK_InstanceVariableReadNode: {
    if (cf_in_exec_block(f, v)) { cf_all(f, v); return; }
    int els[64]; int n = cf_ivar_els(f, v, nt_str(nt, v, "name"), els, 64);
    for (int i = 0; i < n; i++) cf_unite(f, v, els[i]);
    if (f->all_ivars) cf_all(f, v);
    return;
  }
  case NK_ClassVariableWriteNode: case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode:
  case NK_ClassVariableOperatorWriteNode: case NK_GlobalVariableWriteNode: case NK_GlobalVariableOrWriteNode:
  case NK_GlobalVariableAndWriteNode: case NK_GlobalVariableOperatorWriteNode: case NK_ConstantWriteNode:
  case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode: case NK_ConstantOperatorWriteNode: {
    int key = (k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode || k == NK_ClassVariableAndWriteNode || k == NK_ClassVariableOperatorWriteNode) ? CF_K_CVAR
            : (k == NK_ConstantWriteNode || k == NK_ConstantOrWriteNode || k == NK_ConstantAndWriteNode || k == NK_ConstantOperatorWriteNode) ? CF_K_CONST : CF_K_GVAR;
    const char *nm = nt_str(nt, v, "name");
    if (key == CF_K_GVAR && nm && nm[0] == '$') nm = comp_resolve_gvar(c, nm + 1);
    int el = cf_entry(f, key, key == CF_K_CONST ? cf_const_key(nm) : nm);
    int val = nt_ref(nt, v, "value");
    int opw = k == NK_ClassVariableOperatorWriteNode || k == NK_GlobalVariableOperatorWriteNode || k == NK_ConstantOperatorWriteNode;
    if (opw) cf_all(f, v); else cf_unite(f, v, val);
    cf_unite(f, el, v);
    return;
  }
  case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
  case NK_ConstantPathOperatorWriteNode: {
    int t = nt_ref(nt, v, "target");
    const char *nm = t >= 0 ? nt_str(nt, t, "name") : nt_str(nt, v, "name");
    int val = nt_ref(nt, v, "value");
    if (k == NK_ConstantPathOperatorWriteNode) cf_all(f, v); else cf_unite(f, v, val);
    if (!nm) { cf_all(f, v); return; }
    cf_unite(f, cf_entry(f, CF_K_CONST, cf_const_key(nm)), v);
    return;
  }
  case NK_ClassVariableTargetNode: cf_all(f, cf_entry(f, CF_K_CVAR, nt_str(nt, v, "name"))); return;
  case NK_GlobalVariableTargetNode: { const char *nm = nt_str(nt, v, "name"); if (nm && nm[0] == '$') nm = comp_resolve_gvar(c, nm + 1); cf_all(f, cf_entry(f, CF_K_GVAR, nm)); return; }
  case NK_ConstantTargetNode: cf_all(f, cf_entry(f, CF_K_CONST, cf_const_key(nt_str(nt, v, "name")))); return;
  case NK_ClassVariableReadNode: cf_unite(f, v, cf_entry(f, CF_K_CVAR, nt_str(nt, v, "name"))); return;
  case NK_GlobalVariableReadNode: {
    const char *nm = nt_str(nt, v, "name");
    if (!nm) { cf_all(f, v); return; }
    /* `$!`, `$~`, `$1`: the runtime's own */
    if (nm[0] == '$' && (nm[1] == '!' || nm[1] == '@')) { cf_all(f, v); return; }
    if (nm[0] == '$' && !((nm[1] >= 'a' && nm[1] <= 'z') || (nm[1] >= 'A' && nm[1] <= 'Z') || nm[1] == '_')) return;
    cf_unite(f, v, cf_entry(f, CF_K_GVAR, comp_resolve_gvar(c, nm + 1)));
    return;
  }
  case NK_ConstantReadNode: case NK_ConstantPathNode: {
    const char *nm = nt_str(nt, v, "name");
    if (!nm) { cf_all(f, v); return; }
    int kc = comp_class_index(c, nm);
    if (kc >= 0) { cf_add_set(f, v, cf_classval(f, kc)); return; }   /* the Class object */
    if (!cf_entry_has(f, CF_K_CONSTW, cf_const_key(nm))) return;   /* a builtin's: ARGV, Float::INFINITY */
    cf_unite(f, v, cf_entry(f, CF_K_CONST, cf_const_key(nm)));
    return;
  }
  case NK_MultiWriteNode: cf_unite(f, v, nt_ref(nt, v, "value")); return;
  case NK_MultiTargetNode: return;
  case NK_YieldNode: {
    int mi = cf_owner_def(f, v, 0, NULL);
    if (mi < 0) { cf_all(f, v); return; }
    cf_unite(f, v, f->yval_el[mi]);
    int a = nt_ref(nt, v, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    int spread = 0;
    for (int i = 0; i < an; i++) if (nt_kind(nt, av[i]) == NK_SplatNode || nt_kind(nt, av[i]) == NK_KeywordHashNode) spread = 1;
    if (spread || an > CF_YARGS) {
      for (int i = 0; i < an; i++) for (int y = 0; y < CF_YARGS; y++)
        cf_unite(f, f->yarg_el[mi][y], nt_kind(nt, av[i]) == NK_SplatNode ? nt_ref(nt, av[i], "expression") : av[i]);
      return;
    }
    for (int i = 0; i < an; i++) cf_unite(f, f->yarg_el[mi][i], av[i]);
    return;
  }
  case NK_ReturnNode: {
    int a = nt_ref(nt, v, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    int lam = -1;
    int mi = cf_owner_def(f, v, 1, &lam);
    int to = lam >= 0 ? lam : mi >= 0 ? f->ret_el[mi] : -1;
    if (to < 0) return;
    for (int i = 0; i < an; i++) cf_unite(f, to, nt_kind(nt, av[i]) == NK_SplatNode ? nt_ref(nt, av[i], "expression") : av[i]);
    return;
  }
  case NK_NextNode: case NK_BreakNode: {
    int a = nt_ref(nt, v, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0) return;
    int blk = cf_owner_block(f, v, 1);
    if (blk < 0) {
      /* a break is its loop's value; a next's argument in a loop goes nowhere */
      int loop = k == NK_BreakNode ? cf_owner_loop(f, v) : -1;
      for (int i = 0; loop >= 0 && i < an; i++) cf_unite(f, loop, nt_kind(nt, av[i]) == NK_SplatNode ? nt_ref(nt, av[i], "expression") : av[i]);
      return;
    }
    /* a next is the block's value; a break is the call's */
    int to = blk;
    if (k == NK_BreakNode) {
      to = f->par ? f->par[blk] : -1;
      if (to < 0 || nt_kind(nt, to) != NK_CallNode) { if (nt_kind(nt, blk) == NK_LambdaNode) to = blk; else return; }
    }
    for (int i = 0; i < an; i++) cf_unite(f, to, nt_kind(nt, av[i]) == NK_SplatNode ? nt_ref(nt, av[i], "expression") : av[i]);
    return;
  }
  case NK_BlockNode: {
    cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "body")));
    int p = f->par ? f->par[v] : -1;
    /* a block whose call the walk does not bind (a super's) takes anything */
    if (p < 0 || (nt_kind(nt, p) != NK_CallNode && nt_kind(nt, p) != NK_LambdaNode)) cf_all_block_params(f, v);
    return;
  }
  case NK_LambdaNode: {
    cf_add_set(f, v, cf_bv_set(f));
    cf_unite(f, v, cf_tail(f, nt_ref(nt, v, "body")));
    cf_all_block_params(f, v);
    return;
  }
  case NK_CallNode: cf_call(f, v); return;
  case NK_CallOrWriteNode: case NK_CallAndWriteNode:
  case NK_IndexOrWriteNode: case NK_IndexAndWriteNode: case NK_IndexOperatorWriteNode:
    cf_call_write(f, v); return;
  case NK_SuperNode: case NK_ForwardingSuperNode: {
    int mi = cf_owner_def(f, v, 0, NULL);
    if (mi < 0) { cf_all(f, v); return; }
    Scope *s = &c->scopes[mi];
    if (!s->name) { cf_all(f, v); return; }
    int blk = nt_ref(nt, v, "block");
    if (s->is_cmethod && sp_streq(s->name, "new") && s->class_id >= 0) cf_add_set(f, v, f->desc[s->class_id]);
    /* the ancestors' methods of the name: by name, which is a superset */
    CFByName *b = cf_byname(f, s->name);
    int argv2[256], argc2 = 0;
    if (k == NK_SuperNode) {
      int a = nt_ref(nt, v, "arguments"), an = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      for (int i = 0; i < an && argc2 < 256; i++) argv2[argc2++] = av[i];
    }
    const int *mis = s->is_cmethod ? b->cmi : b->mi;
    int nmis = s->is_cmethod ? b->ncm : b->n;
    int others = 0;
    for (int i = 0; i < nmis; i++) {
      int t = mis[i];
      if (t == mi) continue;
      others++;
      cf_unite(f, v, f->ret_el[t]);
      if (k == NK_SuperNode) cf_bind_args(f, t, argv2, argc2);
      else {
        /* the same parameters, by position */
        Scope *ts = &c->scopes[t];
        for (int p = 0; p < s->nparams && p < ts->nparams; p++) cf_unite(f, cf_param_el(f, mi, p), cf_param_el(f, t, p));
        if (ts->nparams != s->nparams) for (int p = 0; p < ts->nparams; p++) for (int q = 0; q < s->nparams; q++) cf_unite(f, cf_param_el(f, t, p), cf_param_el(f, mi, q));
      }
      if (blk >= 0) cf_bind_block(f, t, blk);
      else if (f->yval_el[mi] >= 0) {
        /* the block handed on: the parent's yields reach this method's block */
        for (int y = 0; y < CF_YARGS; y++) cf_unite(f, f->yarg_el[mi][y], f->yarg_el[t][y]);
        cf_unite(f, f->yval_el[mi], f->yval_el[t]);
      }
    }
    /* not nmis: the method itself is among those of its name */
    if (others == 0 && !(s->is_cmethod && sp_streq(s->name, "new"))) {
      /* a builtin ancestor's method (Exception#initialize, Array#push, a
         Struct's): its value is its own, built from the arguments, and
         self is its receiver, as for a bare builtin call (Object#freeze
         answers it) */
      int ik = s->is_cmethod ? -1 : cf_inst_class(f, mi);
      cf_builtin_rule(f, v, ik >= 0 && !cf_selfless_fn(s->name) ? f->self_el[ik] : -1, argv2, argc2, blk, s->name);
    }
    return;
  }
  default:
    cf_all(f, v);
    return;
  }
}

/* ---- the pass ---- */

static void cf_collect_dyn(CF *f) {
  const NodeTable *nt = f->nt;
  /* the constants the program writes: a read of any other names a builtin */
  static const NodeKind cw[] = { NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode,
    NK_ConstantOperatorWriteNode, NK_ConstantTargetNode };
  for (int q = 0; q < 5; q++)
    NT_FOREACH_KIND(nt, cw[q], id) { (void)cf_entry(f, CF_K_CONSTW, cf_const_key(nt_str(nt, id, "name"))); }
  static const NodeKind cpw[] = { NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode, NK_ConstantPathAndWriteNode,
    NK_ConstantPathOperatorWriteNode, NK_ConstantPathTargetNode };
  for (int q = 0; q < 5; q++)
    NT_FOREACH_KIND(nt, cpw[q], id) {
      int t = nt_ref(nt, id, "target");
      const char *nm = t >= 0 ? nt_str(nt, t, "name") : nt_str(nt, id, "name");
      if (nm) (void)cf_entry(f, CF_K_CONSTW, cf_const_key(nm)); else f->dyn_all = 1;
    }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    if (sp_streq(cn, "instance_variable_set") || sp_streq(cn, "instance_variable_get") || sp_streq(cn, "remove_instance_variable")) f->all_ivars = 1;
    /* `inject(:m)`, `inject(init, :m)` and `each_with_object(x, &:m)` call m
       with arguments the pass does not see: m takes any class */
    {
      int a0 = nt_ref(nt, id, "arguments"), n0 = 0;
      const int *v0 = a0 >= 0 ? nt_arr(nt, a0, "arguments", &n0) : NULL;
      int by_sym = sp_streq(cn, "inject") || sp_streq(cn, "reduce");
      for (int i = 0; by_sym && i < n0; i++)
        if (nt_kind(nt, v0[i]) == NK_SymbolNode) cf_dyn_add(f, nt_str(nt, v0[i], "value"));
      int blk0 = nt_ref(nt, id, "block");
      if ((by_sym || sp_streq(cn, "each_with_object")) && blk0 >= 0 &&
          nt_kind(nt, blk0) == NK_BlockNode && nt_int(nt, blk0, "sym_proc_block", 0))
        cf_dyn_add(f, nt_str(nt, blk0, "sym_proc"));
    }
    if (!sp_streq(cn, "send") && !sp_streq(cn, "__send__") && !sp_streq(cn, "public_send") &&
        !sp_streq(cn, "method") && !sp_streq(cn, "public_method") && !sp_streq(cn, "instance_method") &&
        !sp_streq(cn, "define_method") && !sp_streq(cn, "define_singleton_method") &&
        !sp_streq(cn, "alias_method") && !sp_streq(cn, "singleton_method") && !sp_streq(cn, "respond_to?") &&
        !sp_streq(cn, "method_defined?") && !sp_streq(cn, "instance_method"))
      continue;
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    int any = 0;
    for (int i = 0; i < an; i++) {
      NodeKind ak = nt_kind(nt, av[i]);
      const char *s = ak == NK_SymbolNode ? nt_str(nt, av[i], "value") : ak == NK_StringNode ? nt_str(nt, av[i], "content") : NULL;
      if (!s) continue;
      cf_dyn_add(f, s);
      any = 1;
    }
    if (!any && !sp_streq(cn, "respond_to?") && !sp_streq(cn, "method_defined?")) f->dyn_all = 1;
  }
}

/* ---- blocks run under another self ---- */

/* a call that runs its block with another self (or as a method body) */
static int cf_exec_name(const char *nm) {
  return nm && (sp_streq(nm, "instance_exec") || sp_streq(nm, "instance_eval") || sp_streq(nm, "class_exec") ||
                sp_streq(nm, "class_eval") || sp_streq(nm, "module_exec") || sp_streq(nm, "module_eval") ||
                sp_streq(nm, "define_method") || sp_streq(nm, "define_singleton_method"));
}
/* a call that may hand its block to such a call: by name, or a method of
   the name whose block parameter does */
static int cf_exec_call(CF *f, const char *nm) {
  if (cf_exec_name(nm)) return 1;
  if (!nm) return 0;
  CFByName *b = cf_byname(f, nm);
  for (int i = 0; i < b->n; i++) if (f->exec_esc[b->mi[i]]) return 1;
  for (int i = 0; i < b->ncm; i++) if (f->exec_esc[b->cmi[i]]) return 1;
  return 0;
}
/* Which blocks may run with a self that is not the scope's: one a call
   above hands to instance_exec and its kin, directly or through the
   block parameters of the program's methods (a fixpoint: `def check(&b) =
   new.instance_exec(&b)`), and every proc and lambda when one of those
   calls takes a block from elsewhere (`instance_exec(&pr)`). A method
   whose block parameter is stored or passed on as a value is taken as
   handing it on. */
static void cf_collect_exec(CF *f) {
  Compiler *c = f->c;
  const NodeTable *nt = f->nt;
  int procs = 0;
  for (int changed = 1; changed; ) {
    changed = 0;
    NT_FOREACH_KIND(nt, NK_BlockArgumentNode, id) {
      int e = nt_ref(nt, id, "expression");
      if (e >= 0 && nt_kind(nt, e) == NK_SymbolNode) continue;
      int p = f->par ? f->par[id] : -1;
      while (p >= 0 && nt_type(nt, p) && sp_streq(nt_type(nt, p), "ArgumentsNode")) p = f->par[p];
      if (p < 0 || nt_kind(nt, p) != NK_CallNode || !cf_exec_call(f, nt_str(nt, p, "name"))) continue;
      int mi = cf_owner_def(f, id, 1, NULL);
      Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
      int own = m && m->blk_param && (e < 0 ? m->blk_param[0] == 0 :
                nt_kind(nt, e) == NK_LocalVariableReadNode && nt_str(nt, e, "name") && sp_streq(nt_str(nt, e, "name"), m->blk_param));
      if (own) { if (!f->exec_esc[mi]) { f->exec_esc[mi] = 1; changed = 1; } }
      else procs = 1;
    }
    /* a block parameter read as a value: stored, passed, returned */
    NT_FOREACH_KIND(nt, NK_LocalVariableReadNode, id) {
      int mi = cf_owner_def(f, id, 1, NULL);
      if (mi < 0 || f->exec_esc[mi]) continue;
      Scope *m = &c->scopes[mi];
      const char *nm = nt_str(nt, id, "name");
      if (!m->blk_param || !m->blk_param[0] || !nm || !sp_streq(nm, m->blk_param)) continue;
      int p = f->par ? f->par[id] : -1;
      if (p >= 0 && nt_kind(nt, p) == NK_BlockArgumentNode) continue;
      if (p >= 0 && nt_kind(nt, p) == NK_CallNode && nt_ref(nt, p, "receiver") == id) {
        const char *cn = nt_str(nt, p, "name");
        if (cn && (sp_streq(cn, "call") || sp_streq(cn, "()") || sp_streq(cn, "yield") || sp_streq(cn, "===") ||
                   sp_streq(cn, "[]") || sp_streq(cn, "arity") || sp_streq(cn, "nil?") || sp_streq(cn, "lambda?") ||
                   sp_streq(cn, "parameters"))) continue;
      }
      f->exec_esc[mi] = 1; changed = 1;
    }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (cf_exec_call(f, nm) ||
        (procs && nm && (sp_streq(nm, "proc") || sp_streq(nm, "lambda") || sp_streq(nm, "new"))))
      f->exec_blk[blk] = 1;
  }
  if (procs) NT_FOREACH_KIND(nt, NK_LambdaNode, id) f->exec_blk[id] = 1;
}
/* node v runs in a block that may have another self */
static int cf_in_exec_block(CF *f, int v) {
  for (int q = f->par ? f->par[v] : -1, d = 0; q >= 0 && d < 100000; q = f->par[q], d++) {
    NodeKind k = nt_kind(f->nt, q);
    if ((k == NK_BlockNode || k == NK_LambdaNode) && q < f->nnodes && f->exec_blk[q]) return 1;
    if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return 0;
  }
  return 0;
}
/* an ivar written where self is unknown: into the ivar of the name of
   every class, the class-level ones and main's */
static void cf_ivar_write_any(CF *f, const char *name, int v) {
  if (!name) return;
  for (int k = 0; k < f->c->nclasses; k++) {
    if (!f->mod[k]) cf_unite(f, cf_entry(f, f->fam[k], name), v);
    cf_unite(f, cf_entry(f, -(k + 2), name), v);
  }
  cf_unite(f, cf_entry(f, CF_K_MAIN, name), v);
}

static int cf_scope_has_super(CF *f, int mi) {
  Scope *s = &f->c->scopes[mi];
  if (s->body < 0) return 0;
  int body = s->body;
  const NodeTable *nt = f->nt;
  int has = 0;
  NT_FOREACH_KIND(nt, NK_ForwardingSuperNode, id) {
    if (f->c->nscope[id] == mi) { has = 1; break; }
  }
  if (has) return 1;
  NT_FOREACH_KIND(nt, NK_SuperNode, id) {
    if (f->c->nscope[id] == mi && nt_ref(nt, id, "block") < 0) { has = 1; break; }
  }
  (void)body;
  return has;
}

/* SPINEL_CLS_FACTS=why: for the receivers with no fact, what made their
   element every class (the source line of the rule here, or the cap),
   and the largest elements by node count with their reason */
static int cf_why_cmp(const void *a, const void *b) { return ((const int *)b)[1] - ((const int *)a)[1]; }
static int cf_seed_key_cmp(const void *a, const void *b) {
  const int *x = a, *y = b;
  return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : x[1] != y[1] ? (x[1] < y[1] ? -1 : 1) : 0;
}
static int cf_seed_count_cmp(const void *a, const void *b) { return ((const int *)b)[2] - ((const int *)a)[2]; }
static void cf_why_report(CF *f) {
  const NodeTable *nt = f->nt;
  int maxl = 4096, *byline = calloc((size_t)maxl + 2, sizeof *byline);
  int *size = calloc((size_t)f->nel, sizeof *size), *recv_any = calloc((size_t)f->nel, sizeof *recv_any);
  if (!byline || !size || !recv_any) cf_oom();
  int total = 0;
  for (int v = 0; v < f->nnodes; v++) size[cf_find(f, v)]++;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int r = nt_ref(nt, id, "receiver");
    if (r < 0 || cf_set_of(f, r) != CF_ALL) continue;
    int root = cf_find(f, r), w = f->why[root];
    byline[w == CF_WHY_CAP ? maxl + 1 : w > 0 && w <= maxl ? w : 0]++;
    recv_any[root]++;
    total++;
  }
  fprintf(stderr, "cls-fact-why: %d receivers with no fact\n", total);
  for (int l = 0; l <= maxl + 1; l++)
    if (byline[l]) fprintf(stderr, "cls-fact-why: %6d %s%d\n", byline[l], l == maxl + 1 ? "cap " : l ? "analyze_cls.c:" : "unknown ", l == maxl + 1 ? CLS_SET_MAX : l);
  int nr = 0, (*roots)[2] = malloc(sizeof *roots * (size_t)f->nel);
  if (!roots) cf_oom();
  for (int x = 0; x < f->nel; x++) if (recv_any[x]) { roots[nr][0] = x; roots[nr][1] = recv_any[x]; nr++; }
  qsort(roots, (size_t)nr, sizeof *roots, cf_why_cmp);
  for (int i = 0; i < nr && i < 10; i++) {
    int x = roots[i][0];
    fprintf(stderr, "cls-fact-why: element %d: %d receivers, %d nodes, %s%d\n", x, roots[i][1], size[x],
            f->why[x] == CF_WHY_CAP ? "cap " : "analyze_cls.c:", f->why[x] == CF_WHY_CAP ? CLS_SET_MAX : f->why[x]);
    /* back through the calls on a receiver with no fact, to where it began */
    for (int step = 0, root = x; step < 12; step++) {
      int e = f->why_el[root];
      if (e < 0 || e >= f->nnodes) { fprintf(stderr, "cls-fact-why:     <- element %d (not a node)\n", e); break; }
      int fid = (int)nt_int(nt, e, "node_file", 0);
      const char *fp = nt_file_path(nt, fid), *nm = nt_str(nt, e, "name");
      fprintf(stderr, "cls-fact-why:     <- %s:%d %s %s (rule %d)\n", fp ? fp : "?", (int)nt_int(nt, e, "node_line", 0),
              nt_type(nt, e), nm ? nm : "", f->why[root]);
      int r = nt_kind(nt, e) == NK_CallNode ? nt_ref(nt, e, "receiver") : -1;
      if (r < 0 || cf_set_of(f, r) != CF_ALL) break;
      int nroot = cf_find(f, r);
      if (nroot == root) { fprintf(stderr, "cls-fact-why:     (its receiver is in the same element)\n"); break; }
      root = nroot;
    }
  }
  /* each receiver with no fact, back to where its every-class began: the
     rule and the node (or element) it named */
  {
    int cap = f->nnodes + 1, nall = 0, ns = 0;
    int (*seed)[3] = calloc((size_t)cap, sizeof *seed);   /* line, element, count */
    if (!seed) cf_oom();
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      int r = nt_ref(nt, id, "receiver");
      if (r < 0 || cf_set_of(f, r) != CF_ALL) continue;
      int root = cf_find(f, r), line = f->why[root], el = f->why_el[root];
      for (int step = 0; step < 200; step++) {
        int nxt = -1;
        if (el >= 0 && el < f->nnodes && nt_kind(nt, el) == NK_CallNode) {
          int rr = nt_ref(nt, el, "receiver");
          if (rr >= 0 && cf_set_of(f, rr) == CF_ALL) nxt = cf_find(f, rr);
        }
        if (nxt < 0 || nxt == root) break;
        root = nxt; line = f->why[root]; el = f->why_el[root];
      }
      if (nall < cap) { seed[nall][0] = line; seed[nall][1] = el; seed[nall][2] = 1; nall++; }
    }
    /* group equal seeds, then the largest groups first */
    qsort(seed, (size_t)nall, sizeof *seed, cf_seed_key_cmp);
    for (int i = 0; i < nall; i++) {
      if (ns > 0 && seed[ns - 1][0] == seed[i][0] && seed[ns - 1][1] == seed[i][1]) { seed[ns - 1][2]++; continue; }
      memcpy(seed[ns++], seed[i], sizeof seed[0]);
    }
    qsort(seed, (size_t)ns, sizeof *seed, cf_seed_count_cmp);
    for (int i = 0; i < ns && i < 25; i++) {
      int e = seed[i][1];
      if (e >= f->nnodes) {
        /* a slot: name the local or parameter */
        for (int mi = 0; mi < f->c->nscopes; mi++)
          for (int q = 0; q < f->nlv[mi]; q++)
            if (f->lv_el[mi] && f->lv_el[mi][q] == e) {
              Scope *sc = &f->c->scopes[mi];
              const char *ln = q < sc->nlocals ? sc->locals[q].name : "?";
              fprintf(stderr, "cls-fact-why: seed %5d receivers: rule %d at local %s of %s%s%s\n", seed[i][2], seed[i][0], ln ? ln : "?",
                      sc->class_id >= 0 ? f->c->classes[sc->class_id].name : "", sc->is_cmethod ? "." : "#", sc->name ? sc->name : "?");
              goto named;
            }
      }
      const char *fp = e >= 0 && e < f->nnodes ? nt_file_path(nt, (int)nt_int(nt, e, "node_file", 0)) : NULL;
      const char *nm = e >= 0 && e < f->nnodes ? nt_str(nt, e, "name") : NULL;
      fprintf(stderr, "cls-fact-why: seed %5d receivers: rule %d at %s:%d %s %s\n", seed[i][2], seed[i][0],
              fp ? fp : "(element)", e >= 0 && e < f->nnodes ? (int)nt_int(nt, e, "node_line", 0) : e,
              e >= 0 && e < f->nnodes ? nt_type(nt, e) : "", nm ? nm : "");
      named:;
    }
    free(seed);
  }
  /* the unions that built the largest element, by rule, weighted by the
     smaller side (what each union brought in) */
  if (nr > 0) {
    int big = roots[0][0];
    long *w = calloc((size_t)maxl + 1, sizeof *w);
    int *cnt = calloc((size_t)maxl + 1, sizeof *cnt);
    if (!w || !cnt) cf_oom();
    for (int i = 0; i < f->nuev; i++) {
      if (cf_find(f, f->uev[i][1]) != big) continue;
      int l = f->uev[i][0] > 0 && f->uev[i][0] <= maxl ? f->uev[i][0] : 0;
      w[l] += f->uev[i][2]; cnt[l]++;
    }
    for (int l = 0; l <= maxl; l++)
      if (w[l] >= 200) fprintf(stderr, "cls-fact-why: unions into element %d: analyze_cls.c:%d brought %ld (%d unions)\n", big, l, w[l], cnt[l]);
    free(w); free(cnt);
  }
  free(roots); free(byline); free(size); free(recv_any);
}

void an_cls_facts(Compiler *c) {
  const NodeTable *nt = c->nt;
  CF f;
  memset(&f, 0, sizeof f);
  f.c = c; f.nt = nt; f.nnodes = nt->count;
  f.par = du_parent_map(nt);
  /* The pool: CF_ALL and CF_EMPTY are sentinel indices that cf_pool answers
     without a lookup, so their entries are written here, as the sets they
     stand for. The hash table's rebuild hashes every entry below npool;
     left unwritten, CF_ALL's entry was heap garbage, with a member count
     that read past the struct and a hash that could alias a real set. */
  f.cpool = 256;
  f.pool = malloc(sizeof *f.pool * (size_t)f.cpool);
  if (!f.pool) cf_oom();
  memset(&f.pool[0], 0, sizeof f.pool[0] * 2);
  f.pool[CF_ALL].all = 1;
  f.npool = 2;
  /* the elements: the nodes first */
  f.cel = nt->count + 1024;
  f.uf = malloc(sizeof *f.uf * (size_t)f.cel);
  f.set = malloc(sizeof *f.set * (size_t)f.cel);
  if (!f.uf || !f.set) cf_oom();
  for (int i = 0; i < nt->count; i++) { f.uf[i] = i; f.set[i] = CF_EMPTY; }
  f.nel = nt->count;
  const char *dump = getenv("SPINEL_CLS_FACTS");
  if (dump && sp_streq(dump, "why")) {
    f.why = calloc((size_t)f.cel, sizeof *f.why);
    f.why_el = malloc(sizeof *f.why_el * (size_t)f.cel);
    f.usz = malloc(sizeof *f.usz * (size_t)f.cel);
    if (!f.why || !f.why_el || !f.usz) cf_oom();
    for (int i = 0; i < f.cel; i++) { f.why_el[i] = -1; f.usz[i] = 1; }
  }
  f.lv_el = calloc((size_t)c->nscopes + 1, sizeof *f.lv_el);
  f.nlv = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  f.ret_el = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  f.yval_el = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  f.yarg_el = malloc(sizeof *f.yarg_el * ((size_t)c->nscopes + 1));
  f.has_super = calloc((size_t)c->nscopes + 1, 1);
  f.protocol = calloc((size_t)c->nscopes + 1, 1);
  f.def_mi = malloc(sizeof(int) * ((size_t)nt->count + 1));
  f.self_el = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.own = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.desc = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.fam = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.mod = calloc((size_t)c->nclasses + 1, 1);
  f.open = malloc(sizeof(int) * ((size_t)c->nclasses + 1));
  f.exec_esc = calloc((size_t)c->nscopes + 1, 1);
  f.exec_blk = calloc((size_t)nt->count + 1, 1);
  if (!f.lv_el || !f.nlv || !f.ret_el || !f.yval_el || !f.yarg_el || !f.has_super || !f.protocol || !f.def_mi ||
      !f.self_el || !f.own || !f.desc || !f.fam || !f.mod || !f.open || !f.exec_esc || !f.exec_blk || !f.par) cf_oom();
  for (int k = 0; k < nt->count; k++) f.def_mi[k] = -1;
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *s = &c->scopes[mi];
    f.nlv[mi] = s->nlocals;
    f.ret_el[mi] = cf_new_el(&f);
    f.yval_el[mi] = cf_new_el(&f);
    f.yarg_el[mi] = malloc(sizeof(int) * CF_YARGS);
    if (!f.yarg_el[mi]) cf_oom();
    for (int y = 0; y < CF_YARGS; y++) f.yarg_el[mi][y] = cf_new_el(&f);
    if (s->def_node >= 0 && s->def_node < nt->count && f.def_mi[s->def_node] < 0) f.def_mi[s->def_node] = mi;
  }
  /* the classes: modules, families, each class with the singleton classes
     made of it, each class's subtree (a module's: its includers') */
  for (int k = 0; k < c->nclasses; k++) {
    f.mod[k] = (unsigned char)(comp_class_is_module(c, &c->classes[k]) != 0);
    int r = k, d = 0;
    while (c->classes[r].parent >= 0 && c->classes[r].parent < c->nclasses && d++ < 1000) r = c->classes[r].parent;
    f.fam[k] = r;
  }
  for (int k = 0; k < c->nclasses; k++) {
    ClsSet s; memset(&s, 0, sizeof s);
    s.cls[s.n++] = k;
    for (int j = 0; j < c->nclasses && !s.all; j++)
      if (c->classes[j].is_singleton_of - 1 == k) { if (s.n < CLS_SET_MAX) s.cls[s.n++] = j; else s.all = 1; }
    f.own[k] = cf_pool(&f, &s);
  }
  for (int k = 0; k < c->nclasses; k++) {
    ClsSet s; memset(&s, 0, sizeof s);
    if (!f.mod[k]) s.cls[s.n++] = k;
    for (int j = 0; j < c->nclasses && !s.all; j++) {
      if (j == k || f.mod[j]) continue;
      int in = is_descendant(c, j, k);
      for (int x = j; x >= 0 && !in; x = c->classes[x].parent)
        for (int m = 0; m < c->classes[x].nincluded_mods && !in; m++)
          if (c->classes[x].included_mods[m] == k) in = 1;
      if (!in) continue;
      if (s.n < CLS_SET_MAX) s.cls[s.n++] = j; else s.all = 1;
    }
    f.desc[k] = cf_pool(&f, &s);
    f.self_el[k] = cf_new_el(&f);
    f.set[f.self_el[k]] = f.desc[k];
    /* a class or module the compiler supplies, or one of the runtime's the
       program reopens, runs for any object */
    if (c->classes[k].def_node >= 0 && nt_int(nt, c->classes[k].def_node, "node_bi", 0) != 0) cf_all(&f, f.self_el[k]);
    if (cf_open_self(&f, k)) {
      cf_all(&f, f.self_el[k]);
      f.open[f.nopen++] = k;
    }
  }
  {
    ClsSet s; memset(&s, 0, sizeof s);
    for (int k = 0; k < c->nclasses && !s.all; k++)
      if (!f.mod[k] && class_is_exc_subclass(c, k)) { if (s.n < CLS_SET_MAX) s.cls[s.n++] = k; else s.all = 1; }
    f.exc_set = cf_pool(&f, &s);
  }
  f.fiber_ch = cf_new_el(&f);
  /* a yield whose value is read: not one in statement position ahead of
     another statement */
  f.yval_used = calloc((size_t)c->nscopes + 1, 1);
  if (!f.yval_used) cf_oom();
  NT_FOREACH_KIND(nt, NK_YieldNode, id) {
    int mi = cf_owner_def(&f, id, 0, NULL);
    if (mi < 0) continue;
    int p = f.par ? f.par[id] : -1;
    int used = 1;
    if (p >= 0 && nt_kind(nt, p) == NK_StatementsNode) {
      int n = 0; const int *b = nt_arr(nt, p, "body", &n);
      used = n > 0 && b[n - 1] == id;
    }
    if (used) f.yval_used[mi] = 1;
  }
  cf_collect_dyn(&f);
  cf_collect_exec(&f);
  for (int k = 0; k < c->nclasses && !f.has_mm; k++)
    if (comp_method_in_chain(c, k, "method_missing", NULL) >= 0) f.has_mm = 1;
  /* the parameters a call the analysis cannot see binds: anything */
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *s = &c->scopes[mi];
    f.has_super[mi] = (unsigned char)cf_scope_has_super(&f, mi);
    if (!s->name) continue;
    /* a method named like a call the analysis does not follow (`bind`,
       `send`, ...): such a call binds nothing, so its parameters are left
       to hold anything rather than nothing (#8720) */
    if (cf_protocol_name(s->name) || cf_opaque_name(s->name) || cf_is_dyn(&f, s->name) ||
        s->is_proc_form || s->is_lowered_yield ||
        s->cs_synth || s->is_module_function || cf_prelude_scope(&f, mi) ||
        (c->n_cext_extensions && s->def_node >= 0 && nt_int(c->nt, s->def_node, "cext_stub", 0))) {
      f.protocol[mi] = 1;
      cf_all_params(&f, mi);
    }
    if (s->blk_param && s->blk_param[0]) {
      int be = cf_lv_in(&f, mi, s->blk_param);
      cf_unite(&f, be, f.yval_el[mi]);
      cf_add_set(&f, be, cf_bv_set(&f));   /* a Proc: its own methods answer */
    }
  }
  /* Children first (a node's id is below its children's), so a receiver's
     read has met its slot when its call asks for the classes. The rounds
     with the builtin answers held back run to a fixpoint first; a round
     that lets them in follows, and the two alternate until a round of each
     changes nothing. */
  f.phase = 0;
  for (int round = 0; round < CF_ROUNDS; round++) {
    f.changed = 0; f.pending = 0;
    f.round = round;
    for (int v = nt->count - 1; v >= 0; v--) cf_node(&f, v);
    /* a method's value: its body's tail */
    for (int mi = 1; mi < c->nscopes; mi++) {
      Scope *s = &c->scopes[mi];
      if (s->body >= 0) cf_unite(&f, f.ret_el[mi], cf_tail(&f, s->body));
    }
    if (f.changed) { f.phase = 0; continue; }
    if (f.phase == 1 || f.pending == 0) break;
    f.phase = 1;
  }
  /* the node facts */
  free(c->cls_fact); free(c->cls_pool);
  c->cls_fact = malloc(sizeof(int) * ((size_t)nt->count + 1));
  if (!c->cls_fact) cf_oom();
  for (int v = 0; v < nt->count; v++) c->cls_fact[v] = cf_set_of(&f, v);
  c->cls_fact_n = nt->count;
  c->cls_pool = f.pool; c->ncls_pool = f.npool;
  if (getenv("SPINEL_CLS_FACTS")) {
    NT_FOREACH_KIND(nt, NK_CallNode, id) {
      /* a call on self has no fact of its own: a consumer holds every
         class there, and so does the dump */
      int r = nt_ref(nt, id, "receiver");
      const ClsSet *s = r >= 0 ? cls_fact_node(c, r) : NULL;
      const char *fp = nt_file_path(nt, (int)nt_int(nt, id, "node_file", 0));
      fprintf(stderr, "cls-fact: %s:%d %s receiver:", fp && *fp ? fp : "?", (int)nt_int(nt, id, "node_line", 0), nt_str(nt, id, "name") ? nt_str(nt, id, "name") : "?");
      if (!s) fprintf(stderr, r < 0 ? " ANY self" : " ANY");
      else if (s->n == 0) fprintf(stderr, " none");
      else for (int i = 0; i < s->n; i++) {
        int k = s->cls[i] >= c->nclasses ? s->cls[i] - c->nclasses : s->cls[i];
        /* the Ruby name, as class_ruby_name builds it (a singleton
           subclass is its parent; a segment qualify_colliding_classes
           rewrote is its leaf), without its cache: the name is not
           settled this early */
        char cn[512] = ""; int chain[16], d = 0;
        for (int e = singleton_visible_ci(c, k); e >= 0 && d < 16; e = c->classes[e].enclosing_class) chain[d++] = e;
        for (int q = d - 1; q >= 0; q--) {
          const char *seg = c->classes[chain[q]].name ? c->classes[chain[q]].name : "?";
          const char *t = q < d - 1 ? strstr(seg, "__") : NULL;
          while (t) { seg = t + 2; t = strstr(seg, "__"); }
          size_t l = strlen(cn);
          snprintf(cn + l, sizeof cn - l, "%s%s", q < d - 1 ? "::" : "", seg);
        }
        fprintf(stderr, s->cls[i] >= c->nclasses ? " Class(%s)" : " %s", cn);
      }
      if (s && s->bv) fprintf(stderr, " +builtin");
      fprintf(stderr, "\n");
    }
  }
  if (f.why) cf_why_report(&f);
  /* the pass's own */
  free(f.par); free(f.uf); free(f.set); free(f.ph);
  for (int mi = 0; mi < c->nscopes; mi++) { free(f.lv_el[mi]); free(f.yarg_el[mi]); }
  free(f.lv_el); free(f.nlv); free(f.ret_el); free(f.yval_el); free(f.yarg_el); free(f.has_super); free(f.protocol);
  free(f.def_mi); free(f.why); free(f.why_el); free(f.usz); free(f.uev); free(f.open); free(f.exec_esc); free(f.exec_blk); free(f.self_el); free(f.own); free(f.desc); free(f.fam); free(f.mod); free(f.dyn); free(f.yval_used); free(f.has_native);
  for (int k = 0; k < f.ent_cap; k++) free((char *)f.ent[k].name);
  free(f.ent);
  for (int k = 0; k < f.bn_cap; k++) if (f.bn[k].name) { free((char *)f.bn[k].name); free(f.bn[k].mi); free(f.bn[k].cmi); free(f.bn[k].rd); free(f.bn[k].wr); }
  free(f.bn);
}

/* See compiler.h. */
const ClsSet *cls_fact_node(Compiler *c, int node) {
  if (!c->cls_fact || node < 0 || node >= c->cls_fact_n) return NULL;
  int p = c->cls_fact[node];
  if (p == CF_ALL || p < 0 || p >= c->ncls_pool) return NULL;
  return &c->cls_pool[p];
}

/* See compiler.h. */
int cls_fact_bounded(Compiler *c, int k) {
  if (k < 0 || k >= c->nclasses) return 0;
  ClassInfo *ci = &c->classes[k];
  if (ci->is_native_class || !ci->name) return 0;
  /* a reopened builtin's instances are the runtime's values, an IO's and
     an exception's too */
  if (is_builtin_reopen_name(ci->name) || io_family_class(c, k) || class_has_exc_name(c, k)) return 0;
  if (comp_class_is_module(c, ci)) return 0;
  /* a class with no definition of its own: a Struct's, a singleton's, else
     the runtime's */
  if (ci->def_node < 0 && !ci->is_struct && !ci->is_data && !ci->is_singleton_of) return 0;
  return 1;
}

/* See compiler.h. */
int cls_fact_excludes(Compiler *c, int recv, int k) {
  const ClsSet *s = cls_fact_node(c, recv);
  if (!s || !cls_fact_bounded(c, k)) return 0;
  for (int i = 0; i < s->n; i++) if (s->cls[i] == k) return 0;
  return 1;
}

/* See compiler.h. */
int cls_fact_idx(Compiler *c, int node) {
  if (!c->cls_fact || node < 0 || node >= c->cls_fact_n) return -1;
  int p = c->cls_fact[node];
  return p <= 0 || p >= c->ncls_pool ? -1 : p;
}
