/* implicit_calls.c -- the methods CRuby calls that the program does not name
   (see implicit_calls.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "implicit_calls.h"
#include "analyze_internal.h"
#include "cext_bridge.h"
#include "call_plan.h"

int g_dump_implicit = 0;

/* ---- the calls of a node ---- */

typedef struct { IcCall *out; int max, n; } IcSink;

static void ic_push(IcSink *s, const char *name, int operand, unsigned role, int recv, const char *target) {
  if (s->n < s->max) {
    s->out[s->n].name = name;
    s->out[s->n].operand = operand;
    s->out[s->n].role = (unsigned short)role;
    s->out[s->n].recv = (unsigned char)recv;
    s->out[s->n].target = target;
  }
  s->n++;
}

/* The callbacks of the builtin `name` of core class `recv_class` (of every
   receiver, for NULL), as calls on node `operand`: one per (callback, role).
   -1 when the table has no row for the name: of that class, it was not probed
   (it may call anything); of every receiver, it is a core name nothing was
   probed for. */
static int ic_builtin_callbacks(IcSink *s, const char *name, const char *recv_class, int operand, unsigned roles_wanted, int recv) {
  const BopCallback *cbs = NULL;
  int n = recv_class ? bop_class_callbacks(recv_class, name, &cbs) : bop_name_callbacks(name, &cbs);
  for (int i = 0; i < n; i++)
    for (unsigned bit = 1; bit <= BCR_HOOK; bit <<= 1)   /* (BCR_KEY qualifies the BLOCK call; it is none of its own) */
      if (bit != BCR_GVAR /* the assignment's row, not a call's */ && (cbs[i].roles & bit) && (roles_wanted & bit)) ic_push(s, cbs[i].name, operand, bit, recv, NULL);
  return n;
}

/* The callbacks of `name` on the classes a receiver can only be, named in a
   space-separated list (an ancestors string of the core class table): how many
   of the classes have a row for it (none: the caller falls back to every
   receiver's) */
static int ic_class_rows(IcSink *s, const char *name, const char *classes, int operand) {
  int rows = 0;
  for (const char *p = classes; p && *p;) {
    const char *e = strchr(p, ' ');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    char cls[64];
    if (l < sizeof cls) {
      memcpy(cls, p, l);
      cls[l] = '\0';
      rows += ic_builtin_callbacks(s, name, cls, operand, BCR_ALL, 0) >= 0;
    }
    p += l + (e != NULL);
  }
  return rows;
}

/* Is class k a plain one (comp_plain_class)? Asked once per class. */
static int ic_plain_class(Compiler *c, int k) {
  if (k < 0 || k >= c->nclasses || !c->ic->plain) return 0;
  if (!c->ic->plain[k]) c->ic->plain[k] = comp_plain_class(c, k) ? 2 : 1;
  return c->ic->plain[k] == 2;
}

/* Is the receiver of call `id` (self, without one) an instance of a plain
   class of the program, or main? */
static int ic_plain_receiver(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  if (!c->ic || !c->ic->typed) return 0;
  if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) {
    TyKind rt = comp_ntype(c, recv);
    return ty_is_object(rt) && ic_plain_class(c, ty_object_class(rt));
  }
  const Scope *sc = comp_scope_of(c, id);
  if (!sc || sc->is_cmethod) return 0;
  return sc->class_id >= 0 ? ic_plain_class(c, sc->class_id) : sc == c->scopes;
}

/* The core class of the receiver of call `id` when its type has settled on a
   builtin value (a String, an Array with no program subclass, ...), else NULL */
static const char *ic_core_receiver(Compiler *c, int id) {
  int recv = nt_ref(c->nt, id, "receiver");
  if (!c->ic || !c->ic->typed || recv < 0 || nt_kind(c->nt, recv) == NK_SelfNode) return NULL;
  TyKind t = comp_ntype(c, recv);
  if (!cr_ty_builtin_value(c, t)) return NULL;
  return builtin_class_of_type(t);
}

/* A super: the builtin of its def's name that the next ancestor is, when no
   ancestor of the program has the method (the plan's answer: its target is
   that ancestor's method, kept through the target). Called with the super's
   arguments and block on self. */
static int ic_super_calls(Compiler *c, int id, IcSink *s) {
  const Scope *sc = comp_scope_of(c, id);
  if (!c->ic || !c->ic->typed || !sc || sc->class_id < 0 || !sc->name || cplan_user(c, id).mi >= 0) return 0;
  const char *name = comp_scope_def_name(c, sc);
  if (!name) return 0;
  if (!sc->is_cmethod && ic_plain_class(c, sc->class_id) && ic_class_rows(s, name, core_class_ancestors("Object"), id)) return s->n;
  if (bop_name_callbacks(name, NULL) < 0) return IC_ANY;
  ic_builtin_callbacks(s, name, NULL, id, BCR_ALL, 0);
  return s->n;
}

int ic_node_calls(Compiler *c, int id, IcCall *out, int max) {
  const NodeTable *nt = c->nt;
  IcSink s = { out, max, 0 };
  if (nt_implied_super(nt, id)) return ic_super_calls(c, id, &s);
  NtImplied rows[16];
  int nr = nt_implied_calls(nt, id, rows, 16), cnt = nr;
  NtImplied *big = NULL;
  const NtImplied *row = rows;
  if (nr > 16) {   /* a `when` with many conditions */
    big = malloc(sizeof *big * (size_t)nr);
    if (!big) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    nt_implied_calls(nt, id, big, nr);
    row = big;
  }
  for (int i = 0; i < cnt; i++) {
    ic_push(&s, row[i].name, row[i].operand, 0, 0, row[i].target);
    /* the implied call of a core name is a call of the builtin on its
       operand: the elements of that operand and the operand itself, when it
       is a program subclass of the core class, are handed to its callbacks */
    if (row[i].operand >= 0) ic_builtin_callbacks(&s, row[i].name, NULL, row[i].operand, BCR_ELEM | BCR_SELF, 1);
  }
  free(big);
  if (nt_kind(nt, id) == NK_CallNode) {
    const char *name = nt_str(nt, id, "name");
    if (!name) return s.n;
    /* a method a C extension defines receives values the extension's C code
       calls CRuby's protocol methods on (rb_num2long asks for to_int, and so
       on) and sends names it computes: any callback name, as for a core name
       the table does not cover */
    if (c->ic && c->ic->has_cext && anh_has(&c->ic->cext, name)) return IC_ANY;
    const BopCallback *any = NULL;
    int nany = bop_name_callbacks(name, &any);   /* the union over every receiver: no callback in it, none in a class's */
    if (nany == 0) return s.n;
    /* a call none of whose callbacks any scope defines keeps nothing */
    int defined = nany < 0;
    for (int i = 0; i < nany && !defined; i++) defined = ic_defined(c, any[i].name);
    if (!defined) return s.n;
    /* a builtin value answers its class's methods and Kernel's; an instance
       of a plain class, Object's, Kernel's and BasicObject's: those classes'
       callbacks of the name, not those of every receiver */
    const char *core = ic_core_receiver(c, id);
    char own[64];   /* the probe table's convention: a class's own row and Kernel's functions */
    if (core) snprintf(own, sizeof own, "%s Kernel", core);
    if (core ? ic_class_rows(&s, name, own, id) : ic_plain_receiver(c, id) && ic_class_rows(&s, name, core_class_ancestors("Object"), id)) return s.n;
    if (nany < 0) return IC_ANY;
    ic_builtin_callbacks(&s, name, NULL, id, BCR_ALL, 0);
  }
  return s.n;
}

/* A class object is handed over in the CLASS role, or is the receiver whose
   own hook the builtin calls (HOOK), or a node's syntax calls it on a class
   (a class header's inherited, a constant read's const_missing, a backtick
   in a class method); an object of the class in any other role (a program
   subclass as the receiver, an argument, an element, a block's answer, a
   port, a loaded object, a name asked through rb_check_funcall), or by a
   node's syntax. */
int ic_core_callback(const char *name, int class_side) {
  unsigned roles = bop_callback_roles(name);
  if (class_side) return (roles & (BCR_CLASS | BCR_HOOK)) != 0 || (nt_implied_name_sides(name) & NTS_CLASS) != 0;
  return (roles & ~(unsigned)(BCR_CLASS | BCR_HOOK)) != 0 || (nt_implied_name_sides(name) & NTS_INST) != 0;
}

/* ---- may the operand hold an object of the program's ---- */

/* The program class a receiver node names as a class value: a constant of a
   program class, `self.class` (or `obj.class` of a typed object), or self in a
   class method. -1 when it is none of these. */
static int ic_class_receiver(Compiler *c, int id, int recv) {
  const NodeTable *nt = c->nt;
  const Scope *sc = comp_scope_of(c, id);
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode)
    return sc && sc->class_id >= 0 && sc->is_cmethod ? sc->class_id : -1;
  if (nt_kind(nt, recv) == NK_ConstantReadNode) return const_node_class_index(c, recv);
  if (nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "name") && is_class_query(nt_str(nt, recv, "name")) &&
      nt_ref(nt, recv, "arguments") < 0) {
    int r = nt_ref(nt, recv, "receiver");
    if (r < 0 || nt_kind(nt, r) == NK_SelfNode) return sc && sc->class_id >= 0 && !sc->is_cmethod ? sc->class_id : -1;
    return ty_is_object(comp_ntype(c, r)) ? ty_object_class(comp_ntype(c, r)) : -1;
  }
  return -1;
}

/* A receiverless call in a method of module `mod` runs one of the program's
   when it does with self an instance of every class that includes the module,
   and of Object where the module is included at the top level: the plan's
   answer for each. */
static int ic_module_runs(Compiler *c, int id, int mod) {
  int includers = 0;
  for (int k = 0; k < c->nclasses; k++) {
    const ClassInfo *ci = &c->classes[k];
    int has = 0;
    for (int j = 0; j < ci->nincluded_mods && !has; j++) has = ci->included_mods[j] == mod;
    if (!has) continue;
    if (comp_class_is_module(c, &c->classes[k]) || cplan_user_in(c, id, k, CPX_TARGET).mi < 0) return 0;
    includers++;
  }
  for (int j = 0; j < c->ntoplevel_includes; j++) {
    if (c->toplevel_includes[j] != mod) continue;
    int obj = comp_core_class(c, CORE_OBJECT);
    if (obj < 0 || cplan_user_in(c, id, obj, CPX_TARGET).mi < 0) return 0;
    includers++;
  }
  return includers > 0;
}

/* Does call node `id` run a method of the program's, so that no builtin of its
   name runs? The call plan's question, asked of the classes the receiver (or
   self, without one) can be: a typed object's class has the method, a class
   value's class has it as a class method, a module's includers all have it, or
   a receiverless call names a def at the top level; or the class is a plain
   one and the name is neither Object's nor Kernel's, so the call finds
   nothing. Only a typed run says yes. */
int ic_runs_program_method(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!c->ic || !c->ic->typed || !name || (c->ic->has_cext && anh_has(&c->ic->cext, name))) return 0;
  int recv = nt_ref(nt, id, "receiver"), cls = -1;
  int ccls = ic_class_receiver(c, id, recv);
  if (ccls >= 0 && call_target(c, id, ccls, name, CT_CMETHOD, NULL) >= 0) return 1;
  if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) {
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_object(rt)) return 0;
    cls = ty_object_class(rt);
  }
  else {
    const Scope *sc = comp_scope_of(c, id);
    if (sc && sc->class_id >= 0 && !sc->is_cmethod) cls = sc->class_id;
  }
  if (recv < 0 && call_target(c, id, -1, name, CT_TOP, NULL) >= 0) return 1;
  if (cls < 0) return 0;
  if (recv < 0 && comp_class_is_module(c, &c->classes[cls])) return ic_module_runs(c, id, cls);
  if (cplan_user_in(c, id, cls, CPX_TARGET).mi >= 0) return 1;
  return ic_plain_class(c, cls) && !object_instance_method_name(name);
}

/* A value of a builtin type (cr_ty_builtin_value) calls a method of its
   builtin class, never one of a program's class: unless the program reopened a
   builtin class with a method of that name (the name is not one only plain
   classes hold). */
static int ic_builtin_only(Compiler *c, TyKind t, const char *name) {
  if (!c->ic || !anh_has(&c->ic->class_only, name)) return 0;
  if (t == TY_POLY) return 0;
  return cr_ty_builtin_value(c, t);
}

/* A boxed value (an Integer that may outgrow a machine word under promote)
   that the class facts show holds no class of the program's is a builtin value
   too (cr_recv_builtin_value). */
static int ic_boxed_builtin(Compiler *c, int node, const char *name) {
  if (comp_ntype(c, node) != TY_POLY || !c->ic || !anh_has(&c->ic->class_only, name)) return 0;
  const ClsSet *cs = cls_fact_node(c, node);
  return cs && cs->n == 0;
}

static int ic_may_be_object(Compiler *c, int node, const char *name) {
  if (node < 0) return 1;
  return !ic_builtin_only(c, comp_ntype(c, node), name) && !ic_boxed_builtin(c, node, name);
}

/* Can the elements of the value of `node` be objects of the program's? A
   collection of builtin values cannot; anything not known to be one can. */
static int ic_type_holds(Compiler *c, TyKind t, const char *name, int depth) {
  if (depth > 4) return 1;
  if (ty_is_array(t)) {
    TyKind et = ty_array_elem(t);
    return ic_type_holds(c, et, name, depth + 1) || (!cr_ty_builtin_value(c, et) && !ty_is_array(et) && !ty_is_hash(et));
  }
  if (ty_is_hash(t))
    return ic_type_holds(c, ty_hash_key(t), name, depth + 1) || ic_type_holds(c, ty_hash_val(t), name, depth + 1) ||
           (!cr_ty_builtin_value(c, ty_hash_key(t)) && !ty_is_array(ty_hash_key(t)) && !ty_is_hash(ty_hash_key(t))) ||
           (!cr_ty_builtin_value(c, ty_hash_val(t)) && !ty_is_array(ty_hash_val(t)) && !ty_is_hash(ty_hash_val(t)));
  if (t == TY_RANGE || t == TY_STR_RANGE || t == TY_FLOAT_RANGE) return 0;
  return !ic_builtin_only(c, t, name);
}

static int ic_may_hold_elems(Compiler *c, int node, const char *name) {
  if (node < 0) return 1;
  /* a boxed Range of objects is a builtin to the class facts, but its
     members, which the walk hands succ and <=>, are the program's */
  return ic_type_holds(c, comp_ntype(c, node), name, 0) && (!ic_boxed_builtin(c, node, name) || c->ic->obj_range);
}

/* the last statement's node of a block's body, -1 when it has none */
int ic_block_value(const NodeTable *nt, int blk) {
  int body = nt_ref(nt, blk, "body"), n = 0;
  const int *stmts;
  if (body < 0) return -1;
  if (nt_kind(nt, body) != NK_StatementsNode) return body;
  stmts = nt_arr(nt, body, "body", &n);
  return n > 0 ? stmts[n - 1] : -1;
}

int ic_operand_may_hold(Compiler *c, const IcCall *k) {
  const NodeTable *nt = c->nt;
  if (!c->ic || !c->ic->typed) return 1;
  if (!k->role) return k->operand < 0 ? 1 : ic_may_be_object(c, k->operand, k->name);
  int call = k->operand;
  /* an implied call's builtin has the operand for its receiver and no arguments or block */
  int recv = k->recv ? call : nt_ref(nt, call, "receiver"), args = k->recv ? -1 : nt_ref(nt, call, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  int blk = k->recv ? -1 : nt_ref(nt, call, "block");
  /* a forwarding super hands on its def's own arguments and block */
  if (!k->recv && nt_kind(nt, call) == NK_ForwardingSuperNode && k->role != BCR_SELF) return 1;
  switch (k->role) {
  case BCR_ARG:
  case BCR_CLASS:
    for (int i = 0; i < an; i++) if (ic_may_be_object(c, av[i], k->name)) return 1;
    return 0;
  case BCR_ELEM:
    if (recv < 0 || ic_may_hold_elems(c, recv, k->name)) return 1;
    for (int i = 0; i < an; i++) if (ic_may_hold_elems(c, av[i], k->name)) return 1;
    return 0;
  case BCR_BLOCK:
    if (blk < 0) return 0;
    if (nt_kind(nt, blk) != NK_BlockNode) return 1;
    return ic_may_be_object(c, ic_block_value(nt, blk), k->name);
  case BCR_SELF:
    return recv < 0 || ic_may_be_object(c, recv, k->name);
  case BCR_PORT:
    /* an object is the port when it is handed over: `$stdout = obj` is a write
       of a port global, whose row keeps that object's write at the assignment,
       so the $stdout a puts writes to adds nothing */
    for (int i = 0; i < an; i++) if (ic_may_be_object(c, av[i], k->name)) return 1;
    return 0;
  default:
    return 1;   /* an object the data builds, a name asked, a class's own hook: they do not depend on a type */
  }
}

/* the text for --dump-implicit: the node and what it calls */
void ic_cause(Compiler *c, int node, const IcCall *k, char *buf, size_t n) {
  const NodeTable *nt = c->nt;
  const char *file = nt_file_path(nt, (int)nt_int(nt, node, "node_file", 0));
  int line = (int)nt_int(nt, node, "node_line", 0);
  if (!k->role) {
    snprintf(buf, n, "%s line %d@%s: %s", nt_type(nt, node), line, file, k->name);
    return;
  }
  const char *role = k->role == BCR_ARG ? "ARG" : k->role == BCR_ELEM ? "ELEM" : k->role == BCR_BLOCK ? "BLOCK" :
                     k->role == BCR_SELF ? "SELF" : k->role == BCR_CLASS ? "CLASS" : k->role == BCR_PORT ? "PORT" :
                     k->role == BCR_ASKED ? "ASKED" : k->role == BCR_HOOK ? "HOOK" : "LOADED";
  snprintf(buf, n, "%s line %d@%s: %s of %s", k->recv ? nt_type(nt, node) : "call", line, file, k->name, role);
}

/* ---- what the reachability walk collects (for --dump-implicit alone) ---- */

static void ic_clear(struct IcState *st) {
  for (int i = 0; i < st->nmarks; i++) { free(st->marks[i].name); free(st->marks[i].cause); }
  free(st->marks);
  for (int i = 0; i < st->nlive; i++) { free(st->live[i].name); free(st->live[i].cause); }
  free(st->live);
  anh_free(&st->class_only);
  anh_free(&st->defined);
  anh_free(&st->cext);
  anh_free(&st->live_set);
  free(st->plain);
  memset(st, 0, sizeof *st);
}

void ic_free(Compiler *c) {
  if (!c->ic) return;
  ic_clear(c->ic);
  free(c->ic);
  c->ic = NULL;
}

void ic_begin(Compiler *c, int typed) {
  if (!c->ic) {
    c->ic = calloc(1, sizeof *c->ic);
    if (!c->ic) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  ic_clear(c->ic);
  c->ic->typed = typed;
  c->ic->plain = calloc((size_t)c->nclasses + 1, 1);
  if (!c->ic->plain) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int s = 0; s < c->nscopes; s++) {
    const char *nm = c->scopes[s].name;
    if (!nm) continue;
    if (!anh_has(&c->ic->defined, nm)) anh_add(&c->ic->defined, nm);
    if (cext_scope(c, s) && !anh_has(&c->ic->cext, nm)) { anh_add(&c->ic->cext, nm); c->ic->has_cext = 1; }
  }
  for (int k = 0; k < c->nclasses; k++)
    for (int i = 0; i < c->classes[k].naliases; i++) {
      const char *an = c->classes[k].alias_new[i];
      if (an && !anh_has(&c->ic->defined, an)) anh_add(&c->ic->defined, an);
    }
  cr_class_only_names(c, &c->ic->class_only);   /* also the untyped passes' (g_cr_untyped) */
  for (int id = 0; typed && !c->ic->obj_range && id < c->nt->count; id++)
    c->ic->obj_range = comp_ntype(c, id) == TY_OBJ_RANGE;
}

void ic_scope(Compiler *c, int scope) {
  if (c->ic) c->ic->scope = scope;
}

int ic_defined(const Compiler *c, const char *name) {
  return c->ic && anh_has(&c->ic->defined, name);
}

void ic_record(Compiler *c, const char *name, const char *cause) {
  struct IcState *st = c->ic;
  if (!st || !g_dump_implicit) return;
  if (st->nmarks >= st->capmarks) {
    st->capmarks = st->capmarks ? st->capmarks * 2 : 64;
    st->marks = realloc(st->marks, sizeof *st->marks * (size_t)st->capmarks);
    if (!st->marks) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  IcMark *m = &st->marks[st->nmarks++];
  m->name = strdup(name);
  m->cause = strdup(cause);
  if (!m->name || !m->cause) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->scope = st->scope;
}

void ic_end(Compiler *c) {
  struct IcState *st = c->ic;
  if (!st) return;
  for (int i = 0; i < st->nmarks; i++) {
    const IcMark *m = &st->marks[i];
    if (m->scope < 0 || m->scope >= c->nscopes || !c->scopes[m->scope].reachable) continue;
    if (m->name[0] != '*' && anh_has(&st->live_set, m->name)) continue;
    if (st->nlive >= st->caplive) {
      st->caplive = st->caplive ? st->caplive * 2 : 64;
      st->live = realloc(st->live, sizeof *st->live * (size_t)st->caplive);
      if (!st->live) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    IcLive *l = &st->live[st->nlive++];
    l->name = strdup(m->name);
    l->cause = strdup(m->cause);
    if (!l->name || !l->cause) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    if (m->name[0] != '*') anh_add(&st->live_set, l->name);
  }
}

/* ---- --dump-implicit ---- */

static int ic_live_cmp(const void *a, const void *b) {
  return strcmp(((const IcLive *)a)->name, ((const IcLive *)b)->name);
}

void ic_dump(Compiler *c, FILE *out) {
  struct IcState *st = c->ic;
  if (!st) return;
  qsort(st->live, (size_t)st->nlive, sizeof *st->live, ic_live_cmp);
  for (int i = 0; i < st->nlive; i++) fprintf(out, "%s %s\n", st->live[i].name[0] == '*' ? "any" : "live", st->live[i].cause);
  for (int s = 0; s < c->nscopes; s++) {
    const Scope *sc = &c->scopes[s];
    if (!sc->name || !sc->reachable || sc->def_node < 0 || nt_int(c->nt, sc->def_node, "node_file", 0) != 0) continue;
    fprintf(out, "def %s%s%s line %d\n", sc->class_id >= 0 ? c->classes[sc->class_id].name : "", sc->class_id >= 0 ? (sc->is_cmethod ? "." : "#") : "",
            sc->name, (int)nt_int(c->nt, sc->def_node, "node_line", 0));
  }
}
