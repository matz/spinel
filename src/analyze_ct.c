/* analyze_ct.c -- class-body code run when the program is compiled.

   A class or module body sometimes defines a family of constants and
   classes from a table, with a loop the body runs once when its file loads:

     module HTTPStatus
       StatusMessage = { 200 => 'OK', 404 => 'Not Found' }
       CodeToError = {}
       StatusMessage.each { |code, message|
         err_name = message.gsub(/[ \-]/, '')
         const_set("RC_#{err_name.upcase}", code)
         err_class = Class.new(code < 400 ? Success : ClientError)
         err_class.instance_variable_set(:@code, code)
         const_set(err_name, err_class)
         CodeToError[code] = err_class
       }
     end

   Every name and value is known when the program is compiled. The loop is
   run here by the compile-time evaluator (ct_eval.c), and what it does
   becomes the body's own statements, in place of the loop:

     RC_OK = 200; class OK < Success; @code = 200; end; CodeToError[200] = OK; ...;
     StatusMessage

   (Hash#each answers its receiver). The classes it names are ordinary class
   nodes, so the registration that follows makes them as it makes a written
   class: nothing of the loop is left for the generated program to run.

   The loop runs here only when nothing else can make it answer otherwise.
   Each condition is one question below. Before the walk (a loop that makes
   a class) each is a scan of the whole node table. In the walk (a loop that
   makes none, ct_table_loop_form) each is asked at the loop's point: the
   methods by the load record, and the table's uses by the nodes that may
   run before the loop, since CRuby has run it by the time a later one
   runs. (A value given to one of its constants anywhere still declines
   it: the walk would keep the loop's value over a later one.)

     ct_table_only_read        the table's constant is written once and only
                               read, by calls that leave it as it is
     ct_const_given_elsewhere  a constant the loop sets is given a value
                               anywhere else in the program
     ct_core_method_changed    the program changes a core method the loop ran
     ct_method_defined         the program defines a method of a name that
                               runs whatever the receiver (the inherited and
                               const_added hooks, const_set,
                               instance_variable_set)

   The loop must be a statement of a class or module body, `H.each` or
   `H.each_pair` with a block of two parameters, H a constant the body
   assigned a Hash literal of literal keys and values earlier. A class it
   makes must be named by a const_set before anything else uses it, and gets
   instance variables only before then. A String it writes out is a frozen
   literal's (the table's under the frozen-string-literal pragma), written
   as that literal, or one the loop made, written as a new unfrozen String;
   an object the loop made (such a String, an Array) is written at one place
   only, since each place's node is a new object (ct_objects_written_once).
   Anything else leaves the loop as written.

   Until the inline load model goes (#9898's last part), its text expansion
   (sp_const_loop.c) runs these loops before the parse, and this desugar runs
   on graph tables only (ct_loops_in_analysis). */
#include <stdlib.h>
#include <string.h>

#include "analyze_internal.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "ct_eval.h"

/* two names, either of which may be missing (NULL), are the same */
static int ct_eq(const char *a, const char *b) { return a && b && sp_streq(a, b); }

/* The inline/graph coexistence switch: the inline model's text pass
   expands the loops, so this desugar runs only on a graph table (its root
   carries load_file), and under SPINEL_TABLE_LOOPS=analysis, where the
   inline model leaves its loops to it (a test-only switch, to compare the
   two). Deleted with the inline model. */
static int ct_loops_in_analysis(const NodeTable *nt) {
  const char *sw = getenv("SPINEL_TABLE_LOOPS");
  return nt_int(nt, nt->root_id, "load_file", -1) >= 0 || ct_eq(sw, "analysis");
}

/* ---- the questions ---- */

/* the spelling of constant path node `p` ("A::B"), rooted when it starts
   with `::`; 0 for a path whose head is an expression */
static int ct_path_text(const NodeTable *nt, int p, char *buf, size_t cap, int *rooted) {
  NodeKind k = p >= 0 ? nt_kind(nt, p) : NK_UnsupportedNode;
  const char *nm = nt_str(nt, p, "name");
  if (!nm || (k != NK_ConstantReadNode && k != NK_ConstantPathNode)) return 0;
  buf[0] = '\0';
  if (k == NK_ConstantPathNode) {
    int par = nt_ref(nt, p, "parent");
    if (par < 0) *rooted = 1;
    else if (!ct_path_text(nt, par, buf, cap, rooted)) return 0;
    if (par >= 0 && strlen(buf) + 2 < cap) strcat(buf, "::");
  }
  if (strlen(buf) + strlen(nm) + 1 > cap) return 0;
  strcat(buf, nm);
  return 1;
}

/* the modules `node` stands in, lexically ("A::B", "" at the top level);
   0 where one is a singleton class or a path that can't be spelled */
static int ct_lexical_path(const NodeTable *nt, const int *parent, int node, char *buf, size_t cap) {
  buf[0] = '\0';
  for (int a = parent[node]; a >= 0; a = parent[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_SingletonClassNode) return 0;
    if (k != NK_ClassNode && k != NK_ModuleNode) continue;
    char seg[512]; int rooted = 0;
    if (!ct_path_text(nt, nt_ref(nt, a, "constant_path"), seg, sizeof seg, &rooted)) return 0;
    if (strlen(seg) + strlen(buf) + 3 > cap) return 0;
    char t[1024];
    snprintf(t, sizeof t, "%s%s%s", seg, buf[0] ? "::" : "", buf);
    strcpy(buf, t);
    if (rooted) break;
  }
  return 1;
}

/* is `node` inside the subtree of `top` */
static int ct_within(const int *parent, int node, int top) {
  for (; node >= 0; node = parent[node]) if (node == top) return 1;
  return 0;
}

/* a literal Symbol or String argument's text, or NULL */
static const char *ct_lit_name(const NodeTable *nt, int arg) {
  NodeKind k = nt_kind(nt, arg);
  return k == NK_SymbolNode ? nt_str(nt, arg, "value") : k == NK_StringNode ? nt_str(nt, arg, "content") : NULL;
}

/* the last name of a constant's full path */
static const char *ct_leaf(const char *full) {
  const char *l = strrchr(full, ':');
  return l ? l + 1 : full;
}

/* Does a definition at `at`, spelled `path` (the constant path node, or -1
   with `leaf` for a plain name written in the body), give the constant
   `full` a value? A plain name or a rooted path names one constant; a
   relative path's head is looked up when it runs, so any constant of its
   last name counts. */
static int ct_defines(const NodeTable *nt, const int *parent, int at, int path, const char *leaf, const char *full) {
  char outer[1024], seg[512], t[1600];
  int rooted = 0;
  if (path >= 0) {
    if (!ct_path_text(nt, path, seg, sizeof seg, &rooted)) return 1;
    if (rooted) return ct_eq(seg, full);
    if (strstr(seg, "::")) return ct_eq(nt_str(nt, path, "name"), ct_leaf(full));
    leaf = seg;
  }
  if (!ct_lexical_path(nt, parent, at, outer, sizeof outer)) return 1;
  snprintf(t, sizeof t, "%s%s%s", outer, outer[0] ? "::" : "", leaf);
  return ct_eq(t, full);
}

/* Is one of the constants `full[0..n)` ("M::Name") given a value anywhere
   in the program outside the loop `loop`: a class or module of that name, a
   write or a multiple assignment's target, or a const_set naming it, or
   naming a constant the scan can't spell? A const_set then replaces the
   value (with CRuby's warning) where a class definition would reopen it.
   One walk of the table for all of them. */
/* Does node id give one of the constants full[0..n) a value (the test of
   ct_const_given_elsewhere for one node) */
static int ct_gives_const(const NodeTable *nt, const int *parent, int id, char *const *full, int n, int loop,
                          const char *own) {
  NodeKind k = nt_kind(nt, id);
  const char *leaf = NULL;
  int path = -1;   /* the definition's path node, -1 for a plain name */
  switch (k) {
  case NK_ClassNode: case NK_ModuleNode:
    path = nt_ref(nt, id, "constant_path"); leaf = nt_str(nt, path, "name");
    break;
  case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
  case NK_ConstantOperatorWriteNode: case NK_ConstantTargetNode:
    leaf = nt_str(nt, id, "name");
    break;
  case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
  case NK_ConstantPathOperatorWriteNode:
    path = nt_ref(nt, id, "target"); leaf = nt_str(nt, path, "name");
    break;
  case NK_ConstantPathTargetNode:
    path = id; leaf = nt_str(nt, id, "name");
    break;
  case NK_CallNode: {
    if (!nt_str(nt, id, "name") || !is_const_set_name(nt_str(nt, id, "name")) || ct_within(parent, id, loop)) break;
    int an = 0, args = nt_ref(nt, id, "arguments"), rv = nt_ref(nt, id, "receiver");
    /* (at the loop's point, in the walk: a const_set on self in another
       class or module body sets that one's constant, never the loop's) */
    if (own && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode)) {
      char at_path[1024];
      if (ct_lexical_path(nt, parent, id, at_path, sizeof at_path) && !sp_streq(at_path, own)) break;
    }
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    leaf = an ? ct_lit_name(nt, av[0]) : NULL;
    if (!leaf) return 1;
    /* on another receiver: any constant of that name */
    if (rv >= 0 && nt_kind(nt, rv) != NK_SelfNode)
      for (int i = 0; i < n; i++) if (ct_eq(ct_leaf(full[i]), leaf)) return 1;
    break;
  }
  default: break;
  }
  if (!leaf) return 0;
  for (int i = 0; i < n; i++)
    if (ct_eq(ct_leaf(full[i]), leaf) && ct_defines(nt, parent, id, path, leaf, full[i])) return 1;
  return 0;
}
/* The constant nodes of the facts' map by their leaf names (CtConstIdx,
   below), for the loop scans in the walk: one pass for all the loops */
typedef struct CtConstIdx CtConstIdx;
static const int *ct_const_idx_ids(CtConstIdx *x, const char *leaf, int *n);
static const int *ct_const_idx_unspelled(CtConstIdx *x, int *n);
/* The record's answers at a loop the walk runs (ct_table_loop_form): the
   frame's class and the loop's point; NULL before the walk, where the
   questions are scans of the program. */
typedef struct { Compiler *c; int cls; LoadPoint p; CtConstIdx *idx; } CtLoopAt;
/* In the walk: does node id run after the loop, so that CRuby has run the
   loop by then? It does when it is in a statement after the loop's own in
   a body the loop's statement runs in straight on (its class or module
   body, the bodies around that, a file's top level), and not in a BEGIN
   block. A method or a block it is in is made there, after the loop, so
   it runs after too. (By the program's nesting, since the record has
   points only for the statements the walk has reached.) */
static int ct_runs_after(const NodeTable *nt, const int *parent, const CtLoopAt *at, int id) {
  if (!at) return 0;
  int st = at->p.node;
  for (int a = id, from = -1; a >= 0; from = a, a = parent[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_PreExecutionNode) return 0;
    if (k != NK_StatementsNode || from < 0) continue;
    /* is a list the loop's statement runs in straight on: which of its
       statements holds the loop */
    int on = -1;
    for (int b = st, bf = -1; b >= 0; bf = b, b = parent[b]) {
      if (b == a) { on = bf; break; }
      NodeKind bk = nt_kind(nt, b);
      if (b != st && bk != NK_StatementsNode && bk != NK_ClassNode && bk != NK_ModuleNode && bk != NK_ProgramNode) break;
    }
    if (on < 0) continue;
    if (on == from) return 0;
    int n = 0;
    const int *body = nt_arr(nt, a, "body", &n);
    for (int i = 0; i < n; i++) {
      if (body[i] == from) return 0;
      if (body[i] == on) return 1;
    }
    return 0;
  }
  return 0;
}
static int ct_const_given_elsewhere(const NodeTable *nt, const int *parent, int count, char *const *full, int n, int loop,
                                    const char *own, const CtLoopAt *at) {
  CtConstIdx *x = at ? at->idx : NULL;
  if (x) {
    /* only the nodes of the constants' leaves, and the const_sets the scan
       can't spell */
    for (int i = 0; i < n; i++) {
      int m = 0; const int *ids = ct_const_idx_ids(x, ct_leaf(full[i]), &m);
      for (int k = 0; k < m; k++)
        if (ct_gives_const(nt, parent, ids[k], full, n, loop, own)) return 1;
    }
    int m = 0; const int *ids = ct_const_idx_unspelled(x, &m);
    for (int k = 0; k < m; k++)
      if (ct_gives_const(nt, parent, ids[k], full, n, loop, own)) return 1;
    return 0;
  }
  for (int id = 0; id < count; id++) if (ct_gives_const(nt, parent, id, full, n, loop, own)) return 1;
  return 0;
}

/* `n` names one of the classes and modules `anc` lists (CRuby's ancestors
   of a core class, space-separated, as core_class_ancestors gives them): a
   constant read or a path whose last name is one of them */
static int ct_names_one(const NodeTable *nt, int n, const char *anc) {
  NodeKind k = n >= 0 ? nt_kind(nt, n) : NK_UnsupportedNode;
  const char *nm = nt_str(nt, n, "name");
  if ((k != NK_ConstantReadNode && k != NK_ConstantPathNode) || !nm) return 0;
  size_t l = strlen(nm);
  for (const char *p = anc; p && *p; ) {
    const char *e = strchr(p, ' ');
    size_t pl = e ? (size_t)(e - p) : strlen(p);
    if (pl == l && !strncmp(p, nm, l)) return 1;
    p = e ? e + 1 : p + pl;
  }
  return 0;
}

/* the class, module or singleton class whose body `node` is in, or -1 at
   the top level */
static int ct_owner(const NodeTable *nt, const int *parent, int node) {
  for (int a = parent[node]; a >= 0; a = parent[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return a;
  }
  return -1;
}

/* is owner `o` (ct_owner) one of `names` (ct_names_one): a class or module
   of that name, the singleton class of one, or the top level (Object,
   always among them) */
static int ct_owned_by(const NodeTable *nt, int o, const char *names) {
  if (o < 0) return 1;
  int p = nt_ref(nt, o, nt_kind(nt, o) == NK_SingletonClassNode ? "expression" : "constant_path");
  return ct_names_one(nt, p, names);
}

/* Does definer call `call` give or take a method `meth`: 1 when one of the
   names it spells is `meth` or one is not spelled (a variable). A send of a
   literal name (`send(:define_method, :inherited)`) is read as the call it
   names; a send whose target is computed reaches only the program's own
   methods, and like any call that names no method answers 0. */
static int ct_call_names(const NodeTable *nt, int call, const char *meth) {
  int an = 0, from = 0;
  const int *av = call_args(nt, call, &an);
  const char *namer = call_sent_name(nt, call, &from);
  if (!namer || !is_method_naming_call(namer)) return 0;
  if (an <= from) return 1;
  for (int i = from; i < an; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if (k == NK_TrueNode || k == NK_FalseNode) continue;   /* attr's writer flag names no method */
    const char *lit = ct_lit_name(nt, av[i]);
    if (!lit || ct_eq(lit, meth)) return 1;
    if (!is_method_list_call(namer)) break;   /* define_method / alias_method: the first names the method */
  }
  return 0;
}

/* Does the program change one of the core methods `ran[0..n)` (cls#meth) as
   instances of cls see it: a def, alias or definer call of that name in cls
   or one of its ancestors (a reopening, its singleton class, or the top
   level for Object), a mixin, class_eval, send or other change in one of
   them that the scan can't name, a refinement of one, or such a change
   through a receiver that may hold one of them (a variable)? The ancestors
   are CRuby's (core_class_ancestors); a class that table does not hold
   counts as changed. One walk of the table's nodes [0, count), which parent
   maps, for all of them. */
static int ct_core_method_changed(const NodeTable *nt, const int *parent, int count, const CtRan *ran, int n) {
  for (int i = 0; i < n; i++) if (!core_class_ancestors(ran[i].cls)) return 1;
  for (int id = 0; id < count; id++) {
    NodeKind k = nt_kind(nt, id);
    const char *nm = nt_str(nt, id, "name");
    if (k == NK_DefNode || k == NK_AliasMethodNode) {
      const char *dn = k == NK_DefNode ? nm : nt_str(nt, nt_ref(nt, id, "new_name"), "value");
      int r = k == NK_DefNode ? nt_ref(nt, id, "receiver") : -1;
      for (int i = 0; i < n; i++) {
        const char *names = core_class_ancestors(ran[i].cls);
        if (!ct_eq(dn, ran[i].meth)) continue;
        if (r >= 0 && nt_kind(nt, r) != NK_SelfNode ? ct_names_one(nt, r, names)
                                                     : ct_owned_by(nt, ct_owner(nt, parent, id), names)) return 1;
      }
    }
    else if (k == NK_SingletonClassNode) {
      for (int i = 0; i < n; i++) if (ct_names_one(nt, nt_ref(nt, id, "expression"), core_class_ancestors(ran[i].cls))) return 1;
    }
    else if (k == NK_CallNode) {
      int r = nt_ref(nt, id, "receiver");
      int self_recv = r < 0 || nt_kind(nt, r) == NK_SelfNode;
      int naming = is_method_naming_call(nm), change = is_core_change_call(nm);
      if (is_refine_name(nm)) {
        int an = 0, args = nt_ref(nt, id, "arguments");
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        for (int i = 0; i < n && an; i++) if (ct_names_one(nt, av[0], core_class_ancestors(ran[i].cls))) return 1;
        continue;
      }
      if (!naming && !change) continue;
      int mine = 0;
      for (int i = 0; i < n; i++) {
        const char *names = core_class_ancestors(ran[i].cls);
        if (!(self_recv ? ct_owned_by(nt, ct_owner(nt, parent, id), names) : ct_names_one(nt, r, names))) continue;
        mine = 1;
        if (naming ? ct_call_names(nt, id, ran[i].meth) : 1) return 1;
      }
      /* a class held in a variable may be one of them: a send counts when
         it names a changing method; one named at run time reaches only the
         methods the program defines */
      if (!mine && !self_recv && nt_kind(nt, r) != NK_ConstantReadNode && nt_kind(nt, r) != NK_ConstantPathNode &&
          !(nt_kind(nt, r) == NK_CallNode && is_new_name(nt_str(nt, r, "name")))) {
        const char *sent = call_sent_name(nt, id, NULL);
        if (sent && is_core_change_call(sent)) return 1;
      }
    }
  }
  return 0;
}

/* Does the program define a method `meth` anywhere, on any receiver: a def,
   an alias, or a definer call that names it or names a method the scan
   can't read (ct_call_names), directly or through a send. Asked of the methods
   that run whatever class the receiver is: a hook CRuby calls (inherited,
   const_added) or a method a module of the program may override for itself
   (const_set, instance_variable_set). */
int ct_method_defined(const NodeTable *nt, const char *meth) {
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if (k == NK_DefNode && ct_eq(nt_str(nt, id, "name"), meth)) return 1;
    if (k == NK_AliasMethodNode && ct_eq(nt_str(nt, nt_ref(nt, id, "new_name"), "value"), meth)) return 1;
    if (k == NK_CallNode && ct_call_names(nt, id, meth)) return 1;
  }
  return 0;
}

/* Is the constant `name` (the loop's table) written once, and named
   elsewhere only as the receiver of a call that reads the Hash and leaves it
   as it is (the loop's own each; not another each, freeze or to_h, which
   answer the Hash itself) or as the head of a path? A class or module of
   that name, its singleton class, a def on it, a refinement or any other
   use hands it on, and a path write or multiple assignment of that name
   writes it again. */
/* The test of ct_table_only_read for one node: 1 a write of the table, 0
   a use that leaves it as it is (or no use), -1 any other use */
static int ct_table_use(const NodeTable *nt, const int *parent, int id, const char *name, int loop) {
  NodeKind k = nt_kind(nt, id);
  switch (k) {
  case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
  case NK_ConstantOperatorWriteNode:
    return ct_eq(nt_str(nt, id, "name"), name) ? 1 : 0;
  case NK_ConstantTargetNode: case NK_ConstantPathTargetNode:
    return ct_eq(nt_str(nt, id, "name"), name) ? -1 : 0;
  case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
  case NK_ConstantPathOperatorWriteNode:
    return ct_eq(nt_str(nt, nt_ref(nt, id, "target"), "name"), name) ? -1 : 0;
  case NK_ConstantReadNode: case NK_ConstantPathNode: {
    if (!ct_eq(nt_str(nt, id, "name"), name)) return 0;
    int p = parent[id];
    if (p >= 0 && nt_kind(nt, p) == NK_ConstantPathNode && nt_ref(nt, p, "parent") == id) return 0;
    if (p < 0 || nt_kind(nt, p) != NK_CallNode || nt_ref(nt, p, "receiver") != id) return -1;
    const char *m = nt_str(nt, p, "name");
    if (!is_hash_read_only(m) || (p != loop && is_hash_self_answer(m))) return -1;
    return 0;
  }
  default: return 0;
  }
}
/* (in the walk, a use that runs after the loop is none of its business:
   CRuby has run the loop by then) */
static int ct_table_only_read(const NodeTable *nt, const int *parent, int count, const char *name, int loop,
                              const CtLoopAt *at) {
  CtConstIdx *x = at ? at->idx : NULL;
  int writes = 0, m = 0;
  const int *ids = x ? ct_const_idx_ids(x, name, &m) : NULL;
  for (int k = 0; k < (x ? m : count); k++) {
    if (x && ct_runs_after(nt, parent, at, ids[k])) continue;
    int u = ct_table_use(nt, parent, x ? ids[k] : k, name, loop);
    if (u < 0) return 0;
    writes += u;
  }
  return writes == 1;
}

/* ---- the loop ---- */

/* the Hash literal `h` as the table's keys and values: every one a literal
   the evaluator reads, no repeated key */
static int ct_table(const NodeTable *nt, CtEnv *e, int h, Mv **keys, Mv **vals, int *n) {
  if (nt_kind(nt, h) != NK_HashNode) return 0;
  int en = 0;
  const int *el = nt_arr(nt, h, "elements", &en);
  *n = en;
  *keys = calloc((size_t)en + 1, sizeof(Mv)); *vals = calloc((size_t)en + 1, sizeof(Mv));
  if (!*keys || !*vals) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int ok = 1;
  for (int i = 0; i < en && ok; i++) {
    CtVal kv, vv;
    ok = nt_kind(nt, el[i]) == NK_AssocNode && ct_eval(nt, e, nt_ref(nt, el[i], "key"), &kv) && kv.k == CT_VALUE &&
         ct_eval(nt, e, nt_ref(nt, el[i], "value"), &vv) && vv.k == CT_VALUE &&
         kv.mv.k != MV_ARR && vv.mv.k != MV_ARR;
    if (ok) { (*keys)[i] = kv.mv; (*vals)[i] = vv.mv; }
    for (int j = 0; j < i && ok; j++) if (mv_eq((*keys)[i], (*keys)[j])) ok = 0;   /* a repeated key */
  }
  return ok;
}

/* can v be written back as a node, with the classes named so far */
static int ct_spellable(const CtEnv *e, const int *named, CtVal v) {
  if (v.k == CT_CONST) return 1;
  if (v.k == CT_CLASS) return named[v.cls];
  switch (v.mv.k) {
  case MV_UNDEF: return 0;
  /* a frozen literal's String is the literal of its bytes, and one the loop
     made is a new unfrozen String: a node spells either. Not a table's
     unfrozen element, nor a String the loop made and froze */
  case MV_STR: return v.mv.lit == v.mv.frozen;
  case MV_ARR:
    for (int i = 0; i < v.mv.n; i++) {
      CtVal x; memset(&x, 0, sizeof x); x.mv = v.mv.a[i];
      if (!ct_spellable(e, named, x)) return 0;
    }
    return 1;
  default: return 1;
  }
}

/* the identities of the objects the loop made that value v holds (a String
   or an Array: a node spells each as a new object) onto *ids */
static void ct_made_objects(Mv v, const void ***ids, int *n, int *cap) {
  const void *id = v.k == MV_STR && !v.lit ? (const void *)v.s : v.k == MV_ARR ? (const void *)v.a : NULL;
  if (!id) return;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *ids = realloc(*ids, sizeof(void *) * (size_t)*cap);
    if (!*ids) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*ids)[(*n)++] = id;
  for (int i = 0; v.k == MV_ARR && i < v.n; i++) ct_made_objects(v.a[i], ids, n, cap);
}

/* Is every object the loop made written at one place? Each place writes a
   new object, where CRuby's places would hold the one the loop made. */
static int ct_objects_written_once(const CtEnv *e) {
  const void **ids = NULL;
  int n = 0, cap = 0, ok = 1;
  for (int i = 0; i < e->nfx; i++) {
    const CtFx *f = &e->fx[i];
    if (f->k == CT_FX_INDEX_SET) ct_made_objects(f->key.mv, &ids, &n, &cap);
    if (f->k != CT_FX_CLASS_NEW && !f->names) ct_made_objects(f->val.mv, &ids, &n, &cap);
  }
  for (int i = 1; i < n && ok; i++)   /* by equality: a loop makes few objects */
    for (int j = 0; j < i && ok; j++) ok = ids[i] != ids[j];
  free(ids);
  return ok;
}

static int ct_node(NodeTable *nt, const char *type) {
  int id = nt_new_node(nt, type);
  if (id < 0) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return id;
}

static int ct_const_read(NodeTable *nt, const char *name) {
  int id = ct_node(nt, "ConstantReadNode");
  nt_node_set_str(nt, id, "name", name);
  return id;
}

/* the node of a value ct_spellable accepts */
static int ct_value_node(NodeTable *nt, const CtEnv *e, CtVal v) {
  if (v.k == CT_CONST) return nt_clone_subtree(nt, v.node);
  if (v.k == CT_CLASS) return ct_const_read(nt, e->cls[v.cls].name);
  int id;
  switch (v.mv.k) {
  case MV_NIL: return ct_node(nt, "NilNode");
  case MV_TRUE: return ct_node(nt, "TrueNode");
  case MV_FALSE: return ct_node(nt, "FalseNode");
  case MV_INT: id = ct_node(nt, "IntegerNode"); nt_node_set_int(nt, id, "value", v.mv.i); return id;
  case MV_STR:
    id = ct_node(nt, "StringNode");
    nt_node_set_str(nt, id, "content", v.mv.s);
    if (v.mv.frozen) nt_node_set_int(nt, id, "fzl", 1);
    return id;
  case MV_SYM: id = ct_node(nt, "SymbolNode"); nt_node_set_str(nt, id, "value", v.mv.s); return id;
  default: {
    int *el = malloc(sizeof(int) * ((size_t)v.mv.n + 1));
    if (!el) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < v.mv.n; i++) {
      CtVal x; memset(&x, 0, sizeof x); x.mv = v.mv.a[i];
      el[i] = ct_value_node(nt, e, x);
    }
    id = ct_node(nt, "ArrayNode");
    nt_node_set_arr(nt, id, "elements", el, v.mv.n);
    free(el);
    return id;
  }
  }
}

static void ct_push(int **v, int *n, int *cap, int id) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *v = realloc(*v, sizeof(int) * (size_t)*cap);
    if (!*v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*v)[(*n)++] = id;
}

/* Can the loop's effects be written as statements of the body of `outer`
   (its module path): each class named by its first const_set and given
   instance variables only before, every constant set once and given no
   value elsewhere, every value spellable when it is written? */
static int ct_effects_writable(const NodeTable *nt, const int *parent, int count, const CtEnv *e, const char *outer, int loop,
                               const CtLoopAt *at) {
  int *named = calloc((size_t)e->ncls + 1, sizeof(int));
  char **full = calloc((size_t)e->nfx + 1, sizeof(char *));
  if (!named || !full) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int ok = 1, nfull = 0;
  for (int i = 0; i < e->nfx && ok; i++) {
    const CtFx *f = &e->fx[i];
    if (f->k == CT_FX_IVAR_SET) ok = !named[f->cls] && ct_spellable(e, named, f->val);
    /* a String key the loop made is stored as a frozen copy if the receiver
       is a Hash (and as itself if not): which is not known here */
    else if (f->k == CT_FX_INDEX_SET)
      ok = ct_spellable(e, named, f->key) && !(f->key.mv.k == MV_STR && !f->key.mv.lit) && ct_spellable(e, named, f->val);
    else if (f->k == CT_FX_CONST_SET) {
      for (int j = 0; j < i && ok; j++) if (e->fx[j].k == CT_FX_CONST_SET && ct_eq(e->fx[j].name, f->name)) ok = 0;
      size_t len = strlen(outer) + strlen(f->name) + 3;
      full[nfull] = malloc(len);
      if (!full[nfull]) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      snprintf(full[nfull++], len, "%s%s%s", outer, outer[0] ? "::" : "", f->name);
      if (ok && f->names) named[f->val.cls] = 1;
      else if (ok) ok = ct_spellable(e, named, f->val);
    }
  }
  for (int k = 0; k < e->ncls && ok; k++) ok = named[k];   /* a class left anonymous is a run-time value */
  if (ok) ok = ct_objects_written_once(e);
  if (ok) ok = !ct_const_given_elsewhere(nt, parent, count, full, nfull, loop, at ? outer : NULL, at);
  for (int i = 0; i < nfull; i++) free(full[i]);
  free(full); free(named);
  return ok;
}

/* the statements the effects stand for, appended to *out */
static void ct_effects_write(NodeTable *nt, const CtEnv *e, int **out, int *n, int *cap) {
  int **ivars = calloc((size_t)e->ncls + 1, sizeof(int *));
  int *niv = calloc((size_t)e->ncls + 1, sizeof(int)), *civ = calloc((size_t)e->ncls + 1, sizeof(int));
  if (!ivars || !niv || !civ) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < e->nfx; i++) {
    const CtFx *f = &e->fx[i];
    if (f->k == CT_FX_IVAR_SET) {
      int w = ct_node(nt, "InstanceVariableWriteNode");
      nt_node_set_str(nt, w, "name", f->name);
      nt_node_set_ref(nt, w, "value", ct_value_node(nt, e, f->val));
      ct_push(&ivars[f->cls], &niv[f->cls], &civ[f->cls], w);
    }
    else if (f->k == CT_FX_CONST_SET && f->names) {
      /* the class takes its name: its definition, with what was set on it */
      int cls = ct_node(nt, "ClassNode"), body = -1;
      if (niv[f->val.cls]) {
        body = ct_node(nt, "StatementsNode");
        nt_node_set_arr(nt, body, "body", ivars[f->val.cls], niv[f->val.cls]);
      }
      nt_node_set_ref(nt, cls, "constant_path", ct_const_read(nt, f->name));
      nt_node_set_ref(nt, cls, "superclass", nt_clone_subtree(nt, e->cls[f->val.cls].parent));
      nt_node_set_ref(nt, cls, "body", body);
      ct_push(out, n, cap, cls);
    }
    else if (f->k == CT_FX_CONST_SET) {
      int w = ct_node(nt, "ConstantWriteNode");
      nt_node_set_str(nt, w, "name", f->name);
      nt_node_set_ref(nt, w, "value", ct_value_node(nt, e, f->val));
      ct_push(out, n, cap, w);
    }
    else if (f->k == CT_FX_INDEX_SET) {
      int args = ct_node(nt, "ArgumentsNode"), kv[2];
      kv[0] = ct_value_node(nt, e, f->key);
      kv[1] = ct_value_node(nt, e, f->val);
      nt_node_set_arr(nt, args, "arguments", kv, 2);
      int call = ct_node(nt, "CallNode");
      nt_node_set_str(nt, call, "name", "[]=");
      nt_node_set_int(nt, call, "attr_write", 1);
      nt_node_set_ref(nt, call, "receiver", nt_clone_subtree(nt, f->recv.node));
      nt_node_set_ref(nt, call, "arguments", args);
      nt_node_set_ref(nt, call, "block", -1);
      ct_push(out, n, cap, call);
    }
  }
  for (int k = 0; k < e->ncls; k++) free(ivars[k]);
  free(ivars); free(niv); free(civ);
}

/* Run loop statement `st` = before[i] of a body, whose statements before
   it are before[0..i). On success its statements are appended to *out and 1
   is answered. *parent is the table's parent map, made here when a loop
   first gets past its shape (and again after an expansion grew the table). */
static int ct_loop_core_changed_at(const CtLoopAt *at, const CtRan *ran, int n);
static int ct_try_loop(NodeTable *nt, int **parent, int *pcount, const int *before, int i,
                       int **out, int *n, int *cap, const CtLoopAt *at) {
  int st = before[i];
  int recv = nt_ref(nt, st, "receiver"), blk = nt_ref(nt, st, "block");
  const char *meth = nt_str(nt, st, "name"), *tname = nt_str(nt, recv, "name");
  if (nt_kind(nt, st) != NK_CallNode || recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode ||
      nt_ref(nt, st, "arguments") >= 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode || !meth || !tname || !is_each_or_pair(meth)) return 0;
  int bp = nt_ref(nt, blk, "parameters"), ps = bp >= 0 ? nt_ref(nt, bp, "parameters") : -1;
  int rn = 0, on = 0, pn = 0, kn = 0;
  const int *req = ps >= 0 ? nt_arr(nt, ps, "requireds", &rn) : NULL;
  if (ps >= 0) { nt_arr(nt, ps, "optionals", &on); nt_arr(nt, ps, "posts", &pn); nt_arr(nt, ps, "keywords", &kn); }
  if (!req || rn != 2 || on || pn || kn || nt_ref(nt, ps, "rest") >= 0 || nt_ref(nt, ps, "keyword_rest") >= 0 ||
      nt_ref(nt, ps, "block") >= 0 || nt_kind(nt, req[0]) != NK_RequiredParameterNode ||
      nt_kind(nt, req[1]) != NK_RequiredParameterNode) return 0;
  int body = nt_ref(nt, blk, "body");
  if (body >= 0 && nt_kind(nt, body) != NK_StatementsNode) return 0;
  /* the table: a write of it earlier in this body */
  int def = -1;
  for (int j = 0; j < i; j++)
    if (nt_kind(nt, before[j]) == NK_ConstantWriteNode && ct_eq(nt_str(nt, before[j], "name"), tname)) def = before[j];
  if (def < 0 || nt_kind(nt, nt_ref(nt, def, "value")) != NK_HashNode) return 0;
  if (!at && (!*parent || *pcount != nt->count)) {
    free(*parent);
    *parent = an_parent_map(nt); *pcount = nt->count;
    if (!*parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  if (at && st >= *pcount) return 0;   /* (a statement the walk spliced: no parent the map holds) */
  /* the modules the body's constants go in: the class's own path */
  char outer[1024];
  /* (in the walk the map holds the nodes there were when the walk's facts
     were made: the statements it has spliced since are no table's uses) */
  if (!ct_lexical_path(nt, *parent, st, outer, sizeof outer)) return 0;
  /* (before the walk, the table's uses are asked once the loop is known
     to make a class, the one kind it runs there: below) */
  if (at && !ct_table_only_read(nt, *parent, *pcount, tname, st, at)) return 0;
  int hv = nt_ref(nt, def, "value"), en = 0;
  Mv *keys = NULL, *vals = NULL;
  CtEnv e; memset(&e, 0, sizeof e);
  int ok = ct_table(nt, &e, hv, &keys, &vals, &en);
  const int *el = nt_arr(nt, hv, "elements", &en);
  ct_env_ran(&e, "Hash", meth);
  for (int k = 0; k < en && ok; k++) {
    ct_env_new_call(&e);
    CtVal kv, vv;
    memset(&kv, 0, sizeof kv); memset(&vv, 0, sizeof vv);
    /* a String key is the frozen literal of its bytes (a Hash literal's are,
       pragma or not); a String value is the table's element */
    kv.mv = keys[k]; vv.mv = vals[k];
    if (kv.mv.k == MV_STR) kv.mv.frozen = kv.mv.lit = 1;
    if (vv.mv.k == MV_STR) vv.mv.lit = 1;
    ok = nt_str(nt, req[0], "name") && nt_str(nt, req[1], "name") &&
         ct_env_set(&e, nt_str(nt, req[0], "name"), kv) && ct_env_set(&e, nt_str(nt, req[1], "name"), vv) &&
         ct_exec(nt, &e, body);
  }
  /* A loop that makes a class runs before the walk, and one that makes
     none in the walk, at its point (#10791): a class it made in the walk
     would register by qualify's spelling and merge with a later one of
     the same leaf (the class half of retiring qualify, #10793). */
  if (ok && (at ? e.ncls > 0 : e.ncls == 0)) ok = 0;
  if (ok && !at) ok = ct_table_only_read(nt, *parent, *pcount, tname, st, NULL);
  /* nothing may make what ran answer otherwise: anywhere in the program,
     before the walk; at the loop's point, in it */
  if (ok) ok = at ? !ct_loop_core_changed_at(at, e.ran, e.nran) : !ct_core_method_changed(nt, *parent, *pcount, e.ran, e.nran);
  int sets = 0, ivs = 0;
  for (int f = 0; f < e.nfx; f++) { sets |= e.fx[f].k == CT_FX_CONST_SET; ivs |= e.fx[f].k == CT_FX_IVAR_SET; }
  if (ok && e.ncls) ok = !ct_method_defined(nt, "inherited");   /* CRuby calls it while the class has no name */
  if (ok && sets && !at) ok = !ct_method_defined(nt, "const_set") && !ct_method_defined(nt, "const_added");
  if (ok && ivs && !at) ok = !ct_method_defined(nt, "instance_variable_set");
  /* (the frame's own const_set and const_added at the point: a call on
     self reaches them, whatever another class defines) */
  if (ok && sets && at)
    ok = elab_method_at(at->c, at->cls, "const_set", 1, at->p, NULL) == ELAB_NO &&
         elab_method_at(at->c, at->cls, "const_added", 1, at->p, NULL) == ELAB_NO;
  if (ok && ivs && at) ok = elab_method_at(at->c, at->cls, "instance_variable_set", 1, at->p, NULL) == ELAB_NO;
  if (ok) ok = ct_effects_writable(nt, *parent, *pcount, &e, outer, st, at);
  if (ok) {
    int base = nt->count;
    ct_effects_write(nt, &e, out, n, cap);
    ct_push(out, n, cap, nt_clone_subtree(nt, recv));   /* Hash#each answers its receiver */
    /* the statements stand where the loop stood: its file and line */
    long long line = nt_int(nt, st, "node_line", -1), file = nt_int(nt, st, "node_file", -1), col = nt_int(nt, st, "node_col", -1);
    for (int id = base; id < nt->count && line >= 0; id++) {
      nt_node_set_int(nt, id, "node_line", line);
      if (file >= 0) nt_node_set_int(nt, id, "node_file", file);
      if (col >= 0) nt_node_set_int(nt, id, "node_col", col);
    }
    /* the loop leaves the program: no pass that walks the table by id may
       read its block's locals or Symbols as the program's */
    nt_subtree_blank(nt, st);
  }
  ct_env_free(&e);
  free(keys); free(vals);
  return ok;
}

/* Every class and module body's table loops run (above), in place. */
int desugar_table_loops(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (!ct_loops_in_analysis(nt)) return 0;
  int n0 = nt->count, changed = 0;
  int *parent = NULL, pcount = 0;
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    int body = k == NK_ClassNode || k == NK_ModuleNode ? nt_ref(nt, id, "body") : -1;
    if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) continue;
    int bn = 0;
    const int *bb0 = nt_arr(nt, body, "body", &bn);
    int *bb = malloc(sizeof(int) * ((size_t)bn + 1));
    if (!bb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    if (bn) memcpy(bb, bb0, sizeof(int) * (size_t)bn);
    int *nb = NULL, nn = 0, ncap = 0, any = 0;
    for (int i = 0; i < bn; i++) {
      int st = bb[i];
      if (!ct_try_loop(nt, &parent, &pcount, bb, i, &nb, &nn, &ncap, NULL)) { ct_push(&nb, &nn, &ncap, st); continue; }
      /* the body as it now reads, the statements after the loop still to
         come: a later loop's questions find what this one defined in its
         place, under this class (its parent map is made again) */
      int done = nn;
      for (int j = i + 1; j < bn; j++) ct_push(&nb, &nn, &ncap, bb[j]);
      nt_node_set_arr(nt, body, "body", nb, nn);
      nn = done;
      any = 1;
    }
    if (any) { nt_node_set_arr(nt, body, "body", nb, nn); changed = 1; }
    free(nb); free(bb);
  }
  free(parent);
  if (changed) comp_grow_node_arrays(c);
  return changed;
}

/* ---- class-body macros, a form of the walk (#9898) ----

   A class body often calls a class method of its own or of a superclass
   that defines methods for it:

     class Base
       def self.prop(n) = define_method("get_#{n}") { n.to_s }
     end
     class Foo < Base
       prop :a
     end

   The registration's walk runs the call when it reaches it: the
   compile-time evaluator runs the macro's body in macro mode, with Foo as
   self and the call's literal arguments as its parameters, and each
   define_method it runs becomes a `define_method(:get_a) { ... }` in the
   call's place, its block the macro's with each read of the macro's frame
   written as the value the frame holds when the macro returns (what the
   block sees whenever it runs). The statements go through the host
   (host_splice), and the walk registers them next, as if written there.

   The record answers the questions, at the call's point: the macro is the
   method a call of its name on the class finds there (elab_method_at), a
   def that always ran, of required parameters only; the class is the
   program's, its body is directly the call's statement list, its
   visibility public (a macro's define_method makes a public method), and
   nothing the record can't follow ran in it (elab_opaque_before); no
   method_added hook is defined; Module's define_method, name, to_s and
   inspect are what self's calls of them reach (ct_macro_core); and no fold
   answered a method_defined? query of one of the macro's names before the
   walk (def_asked). The program changes none of the core methods the macro
   ran, and no macro has defined a method of that name (ct_core_method_changed,
   asked once per walk of each). A text's line argument that adds or subtracts
   is Integer's, which the record must show unchanged at the call's point
   (sce_graft). No effect of the run gives self's singleton class a method of
   a name a later call on self was taken to reach the record's answer for
   (ct_relied_redefined). The block must not write the macro's
   locals, yield to its block, ask block_given? or read its binding; it
   reads a constant or class variable only when the macro is defined in the
   same body as the call (both are lexical); a String of the frame only for
   its bytes, or frozen where the block answers it. Anything else leaves
   the call as written.

   Until the inline load model goes, its text expansion (sp_macro.c) runs
   these macros before the parse, and this form runs on graph tables only
   (or under SPINEL_CLASS_MACROS=analysis, a test-only switch, where the
   parse leaves them to it). */

static int ct_macros_in_analysis(const NodeTable *nt) {
  const char *sw = getenv("SPINEL_CLASS_MACROS");
  return nt_int(nt, nt->root_id, "load_file", -1) >= 0 || ct_eq(sw, "analysis");
}

/* What the walk's macros asked of the whole program, once per walk: the
   parent map of the nodes [0, count) and the core methods asked about; and
   the methods the walk's macros have defined since, which that map does
   not hold */
/* self's instance variables a class body's statements leave (#10788):
   each name's value (known 0: not known); one not listed is unset, or not
   known when open */
typedef struct { char *name; Mv v; unsigned char known; } CtIv;
typedef struct { CtIv *iv; int n, cap; unsigned char open; } CtIvs;

struct CtMacroFacts {
  int *parent, count;
  CtRan *ran;
  unsigned char *changed;
  int n, cap;
  char **made;
  int nmade, cmade;
  /* a macro call left as written whose run the evaluator followed, and
     the instance variables of self it writes when it runs */
  int *scall; CtIvs *sw; int nscall, cscall;
  /* the state before the last call followed (ct_class_state): class,
     statement list, the call's index there */
  int c_cls, c_list, c_j; unsigned char c_ok; CtIvs c_st;
  /* the class whose state is followed, and the point it is followed to:
     a call on another receiver reaches its instance variables through a
     method of its singleton the record holds there */
  int s_cls; LoadPoint s_p;
  /* the constant nodes by leaf name, for the table loops the walk runs
     (ct_const_idx), made when the first loop asks */
  CtConstIdx *cidx;
};

/* The nodes [0, count) of the walk's map that write, target, read or
   define a constant (a class or module header) or call const_set, by leaf
   name: a hash of the names, each with its node ids; the const_set calls
   of a name the scan can't spell kept apart. */
struct CtConstIdx {
  const char **name; int **ids; int *nid, *cid; int cap, used;
  int *unsp; int nunsp, cunsp;
};
static unsigned ct_idx_hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static int ct_idx_slot(CtConstIdx *x, const char *leaf, int add) {
  unsigned m = (unsigned)x->cap - 1;
  for (unsigned i = ct_idx_hash(leaf) & m;; i = (i + 1) & m) {
    if (!x->name[i]) { if (!add) return -1; x->name[i] = leaf; x->used++; return (int)i; }
    if (sp_streq(x->name[i], leaf)) return (int)i;
  }
}
static void ct_idx_push(int **v, int *n, int *cap, int id) {
  if (*n == *cap) { *cap = *cap ? *cap * 2 : 4; *v = realloc(*v, sizeof **v * (size_t)*cap); if (!*v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); } }
  (*v)[(*n)++] = id;
}
static CtConstIdx *ct_const_idx(const NodeTable *nt, int count) {
  CtConstIdx *x = calloc(1, sizeof *x);
  if (!x) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  x->cap = 64;
  while (x->cap < count / 2) x->cap *= 2;
  x->name = calloc((size_t)x->cap, sizeof *x->name); x->ids = calloc((size_t)x->cap, sizeof *x->ids);
  x->nid = calloc((size_t)x->cap, sizeof *x->nid); x->cid = calloc((size_t)x->cap, sizeof *x->cid);
  if (!x->name || !x->ids || !x->nid || !x->cid) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0; id < count; id++) {
    const char *leaf = NULL;
    switch (nt_kind(nt, id)) {
    case NK_ClassNode: case NK_ModuleNode: leaf = nt_str(nt, nt_ref(nt, id, "constant_path"), "name"); break;
    case NK_ConstantWriteNode: case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode:
    case NK_ConstantOperatorWriteNode: case NK_ConstantTargetNode: case NK_ConstantPathTargetNode:
    case NK_ConstantReadNode: case NK_ConstantPathNode:
      leaf = nt_str(nt, id, "name"); break;
    case NK_ConstantPathWriteNode: case NK_ConstantPathOrWriteNode: case NK_ConstantPathAndWriteNode:
    case NK_ConstantPathOperatorWriteNode:
      leaf = nt_str(nt, nt_ref(nt, id, "target"), "name"); break;
    case NK_CallNode: {
      const char *cn = nt_str(nt, id, "name");
      if (!cn || !is_const_set_name(cn)) break;
      int an = 0, args = nt_ref(nt, id, "arguments");
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      leaf = an ? ct_lit_name(nt, av[0]) : NULL;
      if (!leaf) { ct_idx_push(&x->unsp, &x->nunsp, &x->cunsp, id); continue; }
      break;
    }
    default: break;
    }
    if (!leaf) continue;
    if (x->used * 2 >= x->cap) {   /* grow: rehash the names */
      CtConstIdx y = *x;
      x->cap *= 2; x->used = 0;
      x->name = calloc((size_t)x->cap, sizeof *x->name); x->ids = calloc((size_t)x->cap, sizeof *x->ids);
      x->nid = calloc((size_t)x->cap, sizeof *x->nid); x->cid = calloc((size_t)x->cap, sizeof *x->cid);
      if (!x->name || !x->ids || !x->nid || !x->cid) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      for (int i = 0; i < y.cap; i++) if (y.name[i]) {
        int k = ct_idx_slot(x, y.name[i], 1);
        x->ids[k] = y.ids[i]; x->nid[k] = y.nid[i]; x->cid[k] = y.cid[i];
      }
      free(y.name); free(y.ids); free(y.nid); free(y.cid);
    }
    int k = ct_idx_slot(x, leaf, 1);
    ct_idx_push(&x->ids[k], &x->nid[k], &x->cid[k], id);
  }
  return x;
}
static const int *ct_const_idx_ids(CtConstIdx *x, const char *leaf, int *n) {
  int k = leaf ? ct_idx_slot(x, leaf, 0) : -1;
  *n = k >= 0 ? x->nid[k] : 0;
  return k >= 0 ? x->ids[k] : NULL;
}
static const int *ct_const_idx_unspelled(CtConstIdx *x, int *n) { *n = x->nunsp; return x->unsp; }
static void ct_const_idx_free(CtConstIdx *x) {
  if (!x) return;
  for (int i = 0; i < x->cap; i++) free(x->ids[i]);
  free(x->name); free(x->ids); free(x->nid); free(x->cid); free(x->unsp); free(x);
}

static void ct_ivs_free(CtIvs *s) {
  for (int i = 0; i < s->n; i++) free(s->iv[i].name);
  free(s->iv);
  memset(s, 0, sizeof *s);
}

void ct_macro_facts_free(struct CtMacroFacts *f) {
  if (!f) return;
  for (int i = 0; i < f->nscall; i++) ct_ivs_free(&f->sw[i]);
  free(f->scall); free(f->sw);
  ct_ivs_free(&f->c_st);
  for (int i = 0; i < f->n; i++) { free((char *)f->ran[i].cls); free((char *)f->ran[i].meth); }
  for (int i = 0; i < f->nmade; i++) free(f->made[i]);
  ct_const_idx_free(f->cidx);
  free(f->ran); free(f->changed); free(f->parent); free(f->made); free(f);
}

/* does the program change one of the core methods `ran[0..n)`, each
   answer kept for the rest of the walk? A method of that name a macro
   defined since the map was made counts as a change */
static struct CtMacroFacts *ct_macro_facts(Compiler *c, HostStep *h) {
  struct CtMacroFacts *f = h->ct;
  if (!f) {
    f = h->ct = calloc(1, sizeof *f);
    if (!f || !(f->parent = an_parent_map(c->nt))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    f->count = c->nt->count;
  }
  return f;
}

static int ct_macro_core_changed(Compiler *c, HostStep *h, const CtRan *ran, int n) {
  struct CtMacroFacts *f = ct_macro_facts(c, h);
  for (int i = 0; i < n; i++) {
    if (name_list_has(f->made, f->nmade, ran[i].meth)) return 1;
    int k = 0;
    while (k < f->n && !(sp_streq(f->ran[k].cls, ran[i].cls) && sp_streq(f->ran[k].meth, ran[i].meth))) k++;
    if (k == f->n) {
      if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->ran = realloc(f->ran, sizeof *f->ran * (size_t)f->cap);
        f->changed = realloc(f->changed, (size_t)f->cap);
        if (!f->ran || !f->changed) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      f->ran[k].cls = strdup(ran[i].cls);
      f->ran[k].meth = strdup(ran[i].meth);
      f->changed[k] = (unsigned char)ct_core_method_changed(c->nt, f->parent, f->count, &ran[i], 1);
      f->n++;
    }
    if (f->changed[k]) return 1;
  }
  return 0;
}

/* the frame the macro runs in: self is class cls at point p */
typedef struct { Compiler *c; int cls; LoadPoint p; } CtMacroHost;
static int ct_macro_core(void *host, const char *meth) {
  CtMacroHost *m = host;
  return elab_method_at(m->c, m->cls, meth, 1, m->p, NULL) == ELAB_NO;
}

/* the macro a call of meth on self runs at p: the def of the program's
   it finds, one that always ran there; -1 for none */
static int ct_macro_def(void *host, const char *meth) {
  CtMacroHost *m = host;
  const LrDef *d = NULL;
  if (elab_method_at(m->c, m->cls, meth, 1, m->p, &d) != ELAB_YES || d->kind != LR_DEF || d->may ||
      nt_kind(m->c->nt, d->node) != NK_DefNode || !elab_def_applied(m->c, d->node))
    return -1;
  return d->node;
}

/* The code a macro's effect becomes, read in macro frame `frame` at block
   level lv (the frame's statements are level 0): a define_method's block,
   a statement the evaluator left as written (residual), or the value of a
   local the call's statements write. Its reads of the frame's locals
   become the values the frame holds when the macro returns (a block's,
   which runs after) or when the statement ran (fx's, which runs where it
   stood) -- checked (apply 0), then written on the clone (apply 1) -- or, a String whose
   object the code observes, stay reads of the local, which the call's
   statements write as the macro did (prom). par and gpar are the node's
   parent and grandparent. 0 where the code can't run outside the macro's
   frame. */
typedef struct { const char *name; int frame; char *as; } CtProm;
typedef struct {
  NodeTable *nt; const CtEnv *e; int frame, lexical, residual, value, apply;
  const CtFx *fx;
  CtProm *prom; int nprom, cprom;
} CtBind;

/* is nm a local of some frame of e's, or the name a promoted local is
   written as */
static int ct_local_taken(const CtBind *b, const char *nm) {
  for (int f = 0; f < b->e->nframes; f++) if (ct_frame_local(b->e, f, nm)) return 1;
  for (int i = 0; i < b->nprom; i++) if (sp_streq(b->prom[i].as, nm)) return 1;
  return 0;
}

/* May frame local l, named nm, be written in the call's statements, so
   the code reads the object there: a statement of the macro's body wrote
   it, once. The statements of one call share their locals, so a local of
   another frame's name is written under a name of its own (as). */
static int ct_promote(CtBind *b, const CtLocal *l, const char *nm) {
  int other = 0;
  for (int i = 0; i < b->nprom; i++) {
    if (b->prom[i].frame == b->frame && sp_streq(b->prom[i].name, nm)) return 1;
    other |= sp_streq(b->prom[i].name, nm);
  }
  int body = nt_ref(b->nt, b->e->frames[b->frame].def, "body"), sn = 0, top = 0;
  const int *ss = l->wr >= 0 && body >= 0 ? nt_arr(b->nt, body, "body", &sn) : NULL;
  for (int i = 0; i < sn && !top; i++) top = ss[i] == l->wr;
  if (!top) return 0;
  if (b->nprom == b->cprom) {
    b->cprom = b->cprom ? b->cprom * 2 : 8;
    b->prom = realloc(b->prom, sizeof *b->prom * (size_t)b->cprom);
    if (!b->prom) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  char *as = NULL;
  for (int k = 1; !as || ct_local_taken(b, as); k++) {
    free(as);
    size_t n = strlen(nm) + 24;
    if (!(as = malloc(n))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    if (other) snprintf(as, n, "%s_%d", nm, k);
    else snprintf(as, n, "%s", nm);
    if (!other) break;
  }
  b->prom[b->nprom].name = l->name;
  b->prom[b->nprom].frame = b->frame;
  b->prom[b->nprom++].as = as;
  return 1;
}

/* the name frame's local nm is written as in the call's statements, or
   NULL when it is not promoted */
static const char *ct_promoted(const CtBind *b, int frame, const char *nm) {
  for (int i = 0; i < b->nprom; i++) if (b->prom[i].frame == frame && sp_streq(b->prom[i].name, nm)) return b->prom[i].as;
  return NULL;
}

static int ct_bind_read(CtBind *b, int id, int lv, int par, int gpar) {
  NodeTable *nt = b->nt;
  const char *nm = nt_str(nt, id, "name");
  const CtLocal *l = ct_frame_local(b->e, b->frame, nm), *v = l;
  /* a statement placed where it ran reads the local as it stood then; a
     block in it, which may run later, only one nothing wrote since */
  if (l && b->fx) {
    int i = (int)(l - b->e->frames[b->frame].loc);
    v = i < b->fx->nsnap ? &b->fx->snap[i] : NULL;
    if (v && lv > 0 && v->nw != l->nw) v = NULL;
  }
  /* `defined?(x)` asks what x is; `&x` of a Symbol is a block the parse
     spells for a written `&:name`, which a node written here is not */
  if (!v || v->v.k != CT_VALUE || nt_kind(nt, par) == NK_DefinedNode || nt_kind(nt, par) == NK_BlockArgumentNode)
    return 0;
  Mv mv = v->v.mv;
  /* a String is an object, which a literal makes anew where it runs: only
     one whose bytes alone are read (interpolated), or a frozen literal's a
     body answers (its statement), which a frozen literal of its bytes is
     the same object as -- not a frozen String a method made (a class's
     name). Any other read reads the local, written in the call's
     statements, which a statement left as written could change under the
     evaluator's eyes: not there */
  NodeKind gk = nt_kind(nt, gpar);
  /* (a frozen literal's String read anywhere is the frozen literal of its
     bytes, the one object for those bytes) */
  if (mv.k == MV_STR && !(mv.frozen && mv.lit) && (nt_kind(nt, par) != NK_StatementsNode ||
                         (gk != NK_EmbeddedStatementsNode && (!mv.frozen || !mv.lit || (gk != NK_BlockNode && gk != NK_BeginNode &&
                          gk != NK_RescueNode && gk != NK_ElseNode && gk != NK_EnsureNode)))))
  {
    if (b->residual || !ct_promote(b, l, nm)) return 0;
    if (b->apply) nt_node_set_str(nt, id, "name", ct_promoted(b, b->frame, nm));
    return 1;
  }
  /* an Array whose bytes alone are read (interpolated): a new Array of its
     elements spells them */
  int arr = mv.k == MV_ARR && nt_kind(nt, par) == NK_StatementsNode && gk == NK_EmbeddedStatementsNode &&
            ct_spellable(b->e, NULL, v->v);
  if (mv.k != MV_NIL && mv.k != MV_TRUE && mv.k != MV_FALSE && mv.k != MV_INT && mv.k != MV_SYM && mv.k != MV_STR && !arr) return 0;
  if (!b->apply) return 1;
  long long line = nt_int(nt, id, "node_line", -1), file = nt_int(nt, id, "node_file", -1), col = nt_int(nt, id, "node_col", -1);
  if (arr) {
    int *el = malloc(sizeof(int) * ((size_t)mv.n + 1));
    if (!el) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < mv.n; i++) {
      CtVal x; memset(&x, 0, sizeof x); x.mv = mv.a[i];
      el[i] = ct_value_node(nt, b->e, x);
    }
    nt_node_reset(nt, id, "ArrayNode");
    nt_node_set_arr(nt, id, "elements", el, mv.n);
    free(el);
  }
  else nt_node_reset(nt, id, mv.k == MV_NIL ? "NilNode" : mv.k == MV_TRUE ? "TrueNode" : mv.k == MV_FALSE ? "FalseNode" :
                            mv.k == MV_INT ? "IntegerNode" : mv.k == MV_SYM ? "SymbolNode" : "StringNode");
  if (mv.k == MV_INT) nt_node_set_int(nt, id, "value", mv.i);
  if (mv.k == MV_SYM) nt_node_set_str(nt, id, "value", mv.s);
  if (mv.k == MV_STR) { nt_node_set_str(nt, id, "content", mv.s); if (mv.frozen) nt_node_set_int(nt, id, "fzl", 1); }
  if (line >= 0) nt_node_set_int(nt, id, "node_line", line);
  if (file >= 0) nt_node_set_int(nt, id, "node_file", file);
  if (col >= 0) nt_node_set_int(nt, id, "node_col", col);
  return 1;
}

/* May node id of a statement left as written run among the class body's
   statements? Not a call on self: the steps before the walk read a class
   body's own calls (attr_*, include, private, a macro). Not a call that
   defines, loads or reflects, nor a class builder: they read those
   through any receiver. No jump out of the macro, alias, undef or super,
   and no instance or class variable (a module body's are rewritten before
   the walk). */
static int ct_residual_node(const NodeTable *nt, int id, NodeKind k) {
  switch (k) {
  case NK_CallNode: {
    const char *nm = nt_str(nt, id, "name");
    int rv = nt_ref(nt, id, "receiver");
    NodeKind rk = nt_kind(nt, rv);
    /* (a receiverless puts, print or p writes output, and defines nothing) */
    if (rv < 0 && is_stmt_output_name(nm)) return 1;
    return nm && rv >= 0 && rk != NK_SelfNode && !is_load_effect_call(nm) && !is_method_name_reflection(nm) &&
           !is_const_query_name(nm) && !is_reflective_access(nm) && !is_definition_hook_name(nm) &&
           !((rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) && is_class_builder(nt_str(nt, rv, "name"), nm));
  }
  case NK_AliasMethodNode: case NK_AliasGlobalVariableNode: case NK_UndefNode: case NK_SuperNode:
  case NK_ReturnNode: case NK_BreakNode: case NK_NextNode: case NK_RedoNode: case NK_RetryNode:
  case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode: case NK_InstanceVariableOperatorWriteNode:
  case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableTargetNode:
  case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode: case NK_ClassVariableOperatorWriteNode:
  case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode: case NK_ClassVariableTargetNode:
    return 0;
  default: return 1;
  }
}

static int ct_bind(CtBind *b, int id, int lv, int par, int gpar) {
  NodeTable *nt = b->nt;
  if (id < 0) return 1;
  for (int i = 0; b->fx && i < b->fx->nfold; i++) if (b->fx->fold[i] == id) return 1;   /* written as its Symbol */
  NodeKind k = nt_kind(nt, id);
  int here = nt_int(nt, id, "depth", -1) == lv;
  if (b->residual && !ct_residual_node(nt, id, k)) return 0;
  switch (k) {
  case NK_LocalVariableReadNode: return !here || ct_bind_read(b, id, lv, par, gpar);
  case NK_LocalVariableWriteNode: case NK_LocalVariableOperatorWriteNode: case NK_LocalVariableOrWriteNode:
  case NK_LocalVariableAndWriteNode: case NK_LocalVariableTargetNode:
    if (here) return 0;
    break;
  /* a scope of its own, which may read the frame in its header; the
     macro's block; a read of the frame by name */
  case NK_DefNode: case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode:
  case NK_YieldNode: case NK_ForwardingSuperNode:
  case NK_NumberedParametersNode: case NK_ItParametersNode:
    return 0;
  case NK_CallNode:   /* block_given? there asks of the macro call, not of the defined method */
    if (is_local_reflection(nt_str(nt, id, "name")) || is_block_given_name(nt_str(nt, id, "name"))) return 0;
    /* a local's value calls self's name, to_s or inspect: in a class body
       they are written on self, as desugar_class_body_object_acts writes
       the body's own */
    if (b->value && b->apply && nt_ref(nt, id, "receiver") < 0) {
      nt_set_self_receiver(nt, id, ct_node(nt, "SelfNode"));
      nt_set_vcall(nt, id, 0);
    }
    break;
  case NK_ConstantReadNode: case NK_ConstantWriteNode: case NK_ConstantOperatorWriteNode: case NK_ConstantOrWriteNode:
  case NK_ConstantAndWriteNode: case NK_ConstantTargetNode:
  case NK_ClassVariableReadNode: case NK_ClassVariableWriteNode: case NK_ClassVariableOperatorWriteNode:
  case NK_ClassVariableOrWriteNode: case NK_ClassVariableAndWriteNode: case NK_ClassVariableTargetNode:
    if (!b->lexical) return 0;
    break;
  default: break;
  }
  if (k == NK_BlockNode || k == NK_LambdaNode) lv++;
  for (int i = 0, nr = nt_num_refs(nt, id); i < nr; i++)
    if (!ct_bind(b, nt_ref_at(nt, id, i), lv, id, par)) return 0;
  for (int i = 0, na = nt_num_arrs(nt, id); i < na; i++)
    for (int j = 0;; j++) {
      int n = 0;
      const int *v = nt_arr_at(nt, id, i, &n);
      if (j >= n) break;
      if (!ct_bind(b, v[j], lv, id, par)) return 0;
    }
  return 1;
}

static void ct_bind_free(CtBind *b) {
  for (int i = 0; i < b->nprom; i++) free(b->prom[i].as);
  free(b->prom);
}

/* the code effect f of the macro becomes, bound: its node (a
   define_method's block, a residual statement, a local write's value) */
static int ct_fx_code(const CtFx *f, const NodeTable *nt) {
  return f->k == CT_FX_DEFINE_METHOD ? f->block : f->k == CT_FX_RESIDUAL ? f->site :
         f->k == CT_FX_LOCAL ? nt_ref(nt, f->site, "value") : f->k == CT_FX_CONST_SET ? f->vcode - 1 : -1;
}

/* Is the local effect f writes a String the macro made, unfrozen, which no
   other local of its frame that the kept code reads (a promoted one) held
   when the write ran: a new object of known bytes, written as `+"bytes"`,
   which is one too. A frozen one (a class's name, a literal's) is the
   object its value answers, which that value, run where the call stands,
   answers again. One a promoted local held (`s = t`) is written as a read of
   that local: the same object. A holder nothing reads (a parameter) shows no
   identity, and the write keeps `+"bytes"`. */
static int ct_local_made(const CtBind *b, const CtFx *f) {
  if (f->val.k != CT_VALUE || f->val.mv.k != MV_STR || f->val.mv.frozen) return 0;
  for (int i = 0; i < f->nsnap; i++)
    if (strcmp(f->snap[i].name, f->name) != 0 && ct_same_string(&f->snap[i].v, &f->val) &&
        ct_promoted(b, f->frame, f->snap[i].name))
      return 0;
  return 1;
}

/* is effect f one the call's statements carry: a definition, a residual,
   or the write of a promoted local */
static int ct_fx_kept(const CtBind *b, const CtFx *f) {
  return f->k != CT_FX_LOCAL || ct_promoted(b, f->frame, f->name) != NULL;
}

static int ct_bind_fx(CtBind *b, Compiler *c, int list, const CtFx *f, int code) {
  b->frame = f->frame;
  b->lexical = elab_stmt_list(c, b->e->frames[f->frame].def, NULL) == list;
  b->residual = f->k == CT_FX_RESIDUAL;
  b->fx = f->k == CT_FX_DEFINE_METHOD ? NULL : f;
  b->value = f->k == CT_FX_LOCAL || f->k == CT_FX_CONST_SET;
  return ct_bind(b, code, 0, f->k == CT_FX_DEFINE_METHOD ? f->site : -1, -1);
}

/* the names a definition effect gives methods, each passed to fn(arg, name):
   0 when fn stops */
static int ct_fx_each_name(const CtFx *f, int (*fn)(void *, const char *), void *arg) {
  if (f->k == CT_FX_DEFINE_METHOD) return fn(arg, f->name);
  for (int i = 0; f->k == CT_FX_ATTR && i < f->nattrs; i++) {
    char wn[300];
    if (is_attr_reader_family(f->name) && !fn(arg, f->attrs[i])) return 0;
    if (is_attr_writer_family(f->name) && (snprintf(wn, sizeof wn, "%s=", f->attrs[i]) >= (int)sizeof wn || !fn(arg, wn))) return 0;
  }
  return 1;
}
static int ct_name_unasked(void *arg, const char *name) {
  Compiler *c = arg;
  return !name_list_has(c->def_asked, c->ndef_asked, name);
}
static int ct_name_made(void *arg, const char *name) {
  struct CtMacroFacts *mf = arg;
  if (mf->nmade == mf->cmade) {
    mf->cmade = mf->cmade * 2 + 8;
    mf->made = realloc(mf->made, sizeof(char *) * (size_t)mf->cmade);
    if (!mf->made) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  if (!(mf->made[mf->nmade++] = strdup(name))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return 1;
}

static int ct_name_is(void *arg, const char *name) {
  return !ct_eq(*(const char **)arg, name);
}
/* Did an effect that ran before a call of some name on self was taken to
   reach what the record says give self's singleton class a method of that
   name: a def of its text (tid[f], tn[f]), a define_singleton_method or an
   attr_* of the singleton class? The record at the macro call's point does
   not hold them (ct_text_stmts parses a text after the run), and the call
   reaches them. */
static int ct_relied_redefined(const NodeTable *nt, const CtEnv *e, int *const *tid, const int *tn) {
  for (int a = 0; a < e->nrelied; a++) {
    const char *meth = e->relied[a].name;
    for (int f = 0; f < e->relied[a].after; f++) {
      const CtFx *x = &e->fx[f];
      for (int i = 0; x->k == CT_FX_EVAL && i < tn[f]; i++)
        if (nt_ref(nt, tid[f][i], "receiver") >= 0 && ct_eq(nt_str(nt, tid[f][i], "name"), meth)) return 1;
      if (x->sg && !ct_fx_each_name(x, ct_name_is, &meth)) return 1;
    }
  }
  return 0;
}

/* Is effect f's statement the value of the macro call: its frame's last
   statement, in a frame whose call is its caller's last, up to the first */
static int ct_fx_answers(const NodeTable *nt, const CtEnv *e, const CtFx *f) {
  for (int fr = f->frame, site = f->site; fr >= 0; site = e->frames[fr].call, fr = e->frames[fr].parent) {
    int body = nt_ref(nt, e->frames[fr].def, "body"), sn = 0;
    const int *ss = body >= 0 ? nt_arr(nt, body, "body", &sn) : NULL;
    if (!sn || ss[sn - 1] != site) return 0;
  }
  return 1;
}

/* A node of kind type, at the call's place */
static int ct_node_at(NodeTable *nt, const char *type, int call) {
  int id = ct_node(nt, type);
  const char *const pos[] = { "node_line", "node_file", "node_col" };
  for (int i = 0; i < 3; i++) {
    long long v = nt_int(nt, call, pos[i], -1);
    if (v >= 0) nt_node_set_int(nt, id, pos[i], v);
  }
  return id;
}

static int syms0_of(const NodeTable *nt, int args) {
  int n = 0;
  const int *v = nt_arr(nt, args, "arguments", &n);
  return n > 0 ? v[0] : -1;
}

/* The node of clone b that is node want of a, the subtree b copies */
static int ct_twin(const NodeTable *nt, int a, int b, int want) {
  if (a < 0 || b < 0) return -1;
  if (a == want) return b;
  for (int i = 0, nr = nt_num_refs(nt, a); i < nr && i < nt_num_refs(nt, b); i++) {
    int t = ct_twin(nt, nt_ref_at(nt, a, i), nt_ref_at(nt, b, i), want);
    if (t >= 0) return t;
  }
  for (int i = 0, na = nt_num_arrs(nt, a); i < na && i < nt_num_arrs(nt, b); i++) {
    int n = 0, m = 0;
    const int *va = nt_arr_at(nt, a, i, &n), *vb = nt_arr_at(nt, b, i, &m);
    for (int j = 0; j < n && j < m; j++) {
      int t = ct_twin(nt, va[j], vb[j], want);
      if (t >= 0) return t;
    }
  }
  return -1;
}

/* The statement effect f becomes at the call's place: `define_method(:name)
   { block }`, `attr_x(:a, ...)` (in `class << self` for the singleton
   class's), or the residual statement or local write, cloned; each bound */
static int ct_fx_stmt(CtBind *b, Compiler *c, int list, int call, const CtFx *f) {
  NodeTable *nt = b->nt;
  if (f->k == CT_FX_CONST_SET) {
    int w = ct_node_at(nt, "ConstantWriteNode", call), v = f->vcode ? nt_clone_subtree(nt, f->vcode - 1) : ct_value_node(nt, b->e, f->val);
    for (int i = 0; i < f->nfold; i++) {   /* each known send name, as its Symbol */
      int t = ct_twin(nt, f->vcode - 1, v, f->fold[i]);
      if (t < 0) continue;
      nt_node_reset(nt, t, "SymbolNode");
      nt_node_set_str(nt, t, "value", f->foldv[i]);
    }
    if (f->vcode) ct_bind_fx(b, c, list, f, v);
    nt_node_set_str(nt, w, "name", f->name);
    nt_node_set_ref(nt, w, "value", v);
    return w;
  }
  if (f->k == CT_FX_LOCAL && ct_local_made(b, f)) {
    int st = ct_node_at(nt, "LocalVariableWriteNode", f->site), dup = ct_node_at(nt, "CallNode", f->site);
    int lit = ct_node_at(nt, "StringNode", f->site);
    nt_node_set_str(nt, lit, "content", f->val.mv.s);
    nt_node_set_int(nt, lit, "fzl", 1);
    nt_node_set_str(nt, dup, "name", "+@");
    nt_node_set_ref(nt, dup, "receiver", lit);
    nt_node_set_ref(nt, dup, "arguments", -1);
    nt_node_set_ref(nt, dup, "block", -1);
    nt_node_set_str(nt, st, "name", ct_promoted(b, f->frame, f->name));
    nt_node_set_int(nt, st, "depth", 0);
    nt_node_set_ref(nt, st, "value", dup);
    return st;
  }
  if (f->k == CT_FX_RESIDUAL || f->k == CT_FX_LOCAL) {
    int st = nt_clone_subtree(nt, f->site);
    ct_bind_fx(b, c, list, f, f->k == CT_FX_LOCAL ? nt_ref(nt, st, "value") : st);
    if (f->k == CT_FX_LOCAL) nt_node_set_str(nt, st, "name", ct_promoted(b, f->frame, f->name));
    return st;
  }
  int n = f->k == CT_FX_DEFINE_METHOD ? 1 : f->nattrs;
  int dm = ct_node_at(nt, "CallNode", call), da = ct_node_at(nt, "ArgumentsNode", call), *syms = malloc(sizeof(int) * (size_t)n);
  if (!syms) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < n; i++) {
    syms[i] = ct_node_at(nt, "SymbolNode", call);
    nt_node_set_str(nt, syms[i], "value", f->k == CT_FX_DEFINE_METHOD ? f->name : f->attrs[i]);
  }
  nt_node_set_arr(nt, da, "arguments", syms, n);
  free(syms);
  nt_set_ignore_visibility(nt, dm, 1);
  nt_node_set_ref(nt, dm, "receiver", -1);
  nt_node_set_ref(nt, dm, "arguments", da);
  if (f->k == CT_FX_ATTR) {
    nt_node_set_str(nt, dm, "name", f->name);
    nt_node_set_ref(nt, dm, "block", -1);
    if (!f->sg) return dm;
    int sc = ct_node_at(nt, "SingletonClassNode", call), body = ct_node_at(nt, "StatementsNode", call);
    nt_node_set_arr(nt, body, "body", &dm, 1);
    nt_node_set_ref(nt, sc, "expression", ct_node_at(nt, "SelfNode", call));
    nt_node_set_ref(nt, sc, "body", body);
    return sc;
  }
  int site = f->site, blk = nt_clone_subtree(nt, f->block);
  nt_node_set_str(nt, dm, "name", nt_str(nt, site, nt_int(nt, site, "define_method_name", 0) ? "dm_name" : "name"));
  if (f->lam) {   /* `define_method(:name, -> { })`: the lambda stays the argument */
    int av2[2] = { syms0_of(nt, da), blk };
    nt_node_set_arr(nt, da, "arguments", av2, 2);
    nt_node_set_ref(nt, dm, "block", -1);
  }
  else nt_node_set_ref(nt, dm, "block", blk);
  ct_bind_fx(b, c, list, f, blk);
  return dm;
}

/* The statements text effect f's code becomes in class body frame's (class
   cls's, at p), grafted as a class_eval of that text at f's call is
   (sce_graft_text), into *ids (*n): 0 where it may not run there. Only a
   class's body, not a module's. Only defs of
   self's methods, each unasked by a folded definition query and none a
   hook (desugar_included_hooks reads those), nor a class method `extend
   Enumerable` routes (desugar_extend_enumerable reads those), nor one
   singleton_method_added would see. In their bodies: no class, module,
   singleton class, nested def, alias or undef; no constant
   (qualify_colliding_classes and refuse_unreachable_bare_constants read
   every reference, the steps before them its other shapes); no operator
   write (desugar_reopened_op_write); no call that loads, defines, reflects
   or opens a singleton class; and none of the other shapes a step before
   the walk reads (host_prewalk_shape). */
static int ct_text_stmts(Compiler *c, HostStep *h, int frame, int cls, LoadPoint p, const CtFx *f, int **ids, int *n) {
  NodeTable *nt = (NodeTable *)c->nt;
  *ids = NULL; *n = 0;
  if (!class_def_is_class(nt, frame) || sce_graft_text(c, h, frame, f->name, f->site, p, ids, n) < 0) return 0;
  int ok = 1, nn = 0;
  for (int i = 0; i < *n && ok; i++) {
    int d = (*ids)[i], rv = nt_ref(nt, d, "receiver");
    const char *dn = nt_str(nt, d, "name");
    ok = nt_kind(nt, d) == NK_DefNode && dn && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode) &&
         ct_name_unasked(c, dn) && !is_definition_hook_name(dn) &&
         (rv < 0 || (extend_enumerable_name_index(dn) < 0 && elab_method_at(c, cls, "singleton_method_added", 1, p, NULL) == ELAB_NO));
  }
  int *all = ok ? nt_subtree_ids(nt, *ids, *n, &nn) : NULL;
  for (int i = 0; i < nn && ok; i++) {
    int id = all[i], top = 0;
    const char *nm = nt_str(nt, id, "name");
    for (int t = 0; t < *n && !top; t++) top = (*ids)[t] == id;
    switch (nt_kind(nt, id)) {
    case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode: case NK_AliasMethodNode:
    case NK_AliasGlobalVariableNode: case NK_UndefNode:
    case NK_ConstantReadNode: case NK_ConstantPathNode: case NK_ConstantWriteNode: case NK_ConstantOperatorWriteNode:
    case NK_ConstantOrWriteNode: case NK_ConstantAndWriteNode: case NK_ConstantTargetNode:
      ok = 0;
      break;
    case NK_DefNode: ok = top; break;
    case NK_CallNode:
      ok = !is_load_effect_call(nm) && !is_reflective_access(nm) && !is_definition_hook_name(nm) &&
           !is_singleton_class_name(nm);
      break;
    default: break;
    }
    if (nt_str(nt, id, "binary_operator")) ok = 0;
  }
  free(all);
  /* the precondition rule: none of the shapes the steps before the walk read */
  if (ok) ok = host_prewalk_shape(nt, *ids, *n) < 0;
  if (!ok) {
    for (int i = 0; i < *n; i++) nt_subtree_blank(nt, (*ids)[i]);
    free(*ids); *ids = NULL; *n = 0;
  }
  return ok;
}

/* The form's state, freed; unless placed, the text's grafted statements
   are taken out */
static void ct_macro_drop(NodeTable *nt, CtBind *b, CtEnv *e, int **tid, int *tn, int placed) {
  for (int f = 0; f < e->nfx; f++) {
    for (int i = 0; !placed && e->fx[f].k == CT_FX_EVAL && i < tn[f]; i++) nt_subtree_blank(nt, tid[f][i]);
    free(tid[f]);
  }
  free(tid); free(tn);
  ct_bind_free(b);
  ct_env_free(e);
}

/* ---- the class's state, as its body's statements leave it (#10788) ----
   A macro's instance variable of self (`@kind = k`) is the class's: a
   later macro reads what an earlier one wrote (`kind :box`, then
   `constant :SIZE` builds its name from kind). Before a macro call runs,
   the statements of the class's bodies ahead of it are followed: a write
   of a literal, a macro call left as written whose run the evaluator
   followed (CtMacroFacts.scall), and both arms of an `if`, whose states
   meet (a name the arms leave differently is not known). A statement
   that may run code of the program's on self (a call the walk did not
   run as a macro) leaves the state open: nothing not listed is known. */
static CtIv *ct_ivs_at(CtIvs *s, const char *nm) {
  for (int i = 0; i < s->n; i++) if (strcmp(s->iv[i].name, nm) == 0) return &s->iv[i];
  return NULL;
}
static void ct_ivs_put(CtIvs *s, const char *nm, Mv v, int known) {
  CtIv *x = ct_ivs_at(s, nm);
  if (!x) {
    if (s->n == s->cap) {
      s->cap = s->cap ? s->cap * 2 : 8;
      s->iv = realloc(s->iv, sizeof *s->iv * (size_t)s->cap);
      if (!s->iv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    x = &s->iv[s->n++];
    x->name = strdup(nm);
  }
  x->v = v; x->known = (unsigned char)known;
}
/* code that may write any of them ran: none is known, until a write after */
static void ct_ivs_open(CtIvs *s) {
  s->open = 1;
  for (int i = 0; i < s->n; i++) s->iv[i].known = 0;
}
static void ct_ivs_copy(CtIvs *to, const CtIvs *from) {
  memset(to, 0, sizeof *to);
  for (int i = 0; i < from->n; i++) ct_ivs_put(to, from->iv[i].name, from->iv[i].v, from->iv[i].known);
  to->open = from->open;
}
/* what name nm holds in s: 1 with *v when known */
static int ct_ivs_value(CtIvs *s, const char *nm, Mv *v) {
  CtIv *x = ct_ivs_at(s, nm);
  if (x) { *v = x->v; return x->known; }
  *v = mv_nil();
  return !s->open;
}
/* a := the state either of a and b may be */
static void ct_ivs_meet(CtIvs *a, CtIvs *b) {
  CtIvs m; memset(&m, 0, sizeof m);
  for (int side = 0; side < 2; side++)
    for (int i = 0; i < (side ? b : a)->n; i++) {
      const char *nm = (side ? b : a)->iv[i].name;
      if (ct_ivs_at(&m, nm)) continue;
      Mv va, vb;
      int ka = ct_ivs_value(a, nm, &va), kb = ct_ivs_value(b, nm, &vb);
      ct_ivs_put(&m, nm, va, ka && kb && va.k != MV_ARR && mv_eq(va, vb));
    }
  m.open = a->open || b->open;
  ct_ivs_free(a);
  *a = m;
}

/* a class body call that runs none of the program's code on self */
static int ct_ivs_quiet_call(const char *nm) {
  return nm && (is_class_declaration_call(nm) || is_stmt_output_name(nm) || is_define_method_name(nm) || is_define_singleton_method_name(nm) ||
                sp_streq(nm, "require") || sp_streq(nm, "require_relative") || sp_streq(nm, "private_class_method") ||
                sp_streq(nm, "public_class_method") || sp_streq(nm, "private_constant") || sp_streq(nm, "public_constant"));
}
/* may node id, run in the class body, write self's instance variables:
   an instance variable write, or a call on self that is not quiet (the
   bodies of a def, a class or a singleton class run with another self, or
   later) */
static int ct_def_touches_self(const NodeTable *nt, int id);
/* Does call `call` on self reach a method of the class's singleton the
   record holds whose body writes no instance variable and calls nothing
   on self (`def self.calc_box_size = 32`, which a spliced
   `SIZE = public_send(:calc_box_size)` calls): one that leaves the state
   as it is? A send's literal name is the method's. */
static int ct_ivs_quiet_def(Compiler *c, struct CtMacroFacts *f, int call) {
  const NodeTable *nt = c->nt;
  const char *nm = call_sent_name(nt, call, NULL);
  const LrDef *d = NULL;
  if (!nm || !f || f->s_cls < 0 || elab_method_at(c, f->s_cls, nm, 1, f->s_p, &d) != ELAB_YES || !d || d->kind != LR_DEF ||
      d->may || nt_kind(nt, d->node) != NK_DefNode)
    return 0;
  return !ct_def_touches_self(nt, nt_ref(nt, d->node, "body"));
}
/* does code id write an instance variable, or call a method on self */
static int ct_def_touches_self(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  switch (nt_kind(nt, id)) {
  case NK_DefNode: case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode: return 0;
  case NK_InstanceVariableWriteNode: case NK_InstanceVariableOperatorWriteNode: case NK_InstanceVariableOrWriteNode:
  case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableTargetNode: case NK_YieldNode: case NK_SuperNode:
  case NK_ForwardingSuperNode: return 1;
  case NK_CallNode: {
    int rv = nt_ref(nt, id, "receiver");
    if (rv < 0 || nt_kind(nt, rv) == NK_SelfNode) return 1;
    break;
  }
  default: break;
  }
  for (int i = 0, nr = nt_num_refs(nt, id); i < nr; i++) if (ct_def_touches_self(nt, nt_ref_at(nt, id, i))) return 1;
  for (int i = 0, na = nt_num_arrs(nt, id); i < na; i++) {
    int n = 0;
    const int *v = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) if (ct_def_touches_self(nt, v[j])) return 1;
  }
  return 0;
}
static int ct_ivs_may_write_in(Compiler *c, struct CtMacroFacts *f, int id, int deep);
static int ct_ivs_may_write(Compiler *c, struct CtMacroFacts *f, int id) { return ct_ivs_may_write_in(c, f, id, 0); }
/* deep: class and module bodies too (code between two of the class's
   bodies, where a nested header may reopen it: `class Object; class Calc`) */
static int ct_ivs_may_write_in(Compiler *c, struct CtMacroFacts *f, int id, int deep) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  switch (nt_kind(nt, id)) {
  case NK_DefNode: return 0;
  case NK_ClassNode: case NK_ModuleNode: case NK_SingletonClassNode: if (!deep) return 0; break;
  case NK_InstanceVariableWriteNode: case NK_InstanceVariableOperatorWriteNode: case NK_InstanceVariableOrWriteNode:
  case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableTargetNode: return 1;
  case NK_CallNode: {
    /* on self, any but a quiet one; on another receiver (a local, a block
       parameter, the class by name), one that may reach a method of the
       class's singleton (a macro), or set or read its state by name */
    int rv = nt_ref(nt, id, "receiver");
    const char *nm = nt_str(nt, id, "name");
    if ((rv < 0 || nt_kind(nt, rv) == NK_SelfNode) ? !ct_ivs_quiet_call(nm) && !ct_ivs_quiet_def(c, f, id) :
        !nm || is_send_family(nm) || is_ivar_set_name(nm) || is_eval_exec_family(nm) ||
        (f && f->s_cls >= 0 && elab_method_at(c, f->s_cls, nm, 1, f->s_p, NULL) != ELAB_NO)) return 1;
    break;
  }
  default: break;
  }
  for (int i = 0, nr = nt_num_refs(nt, id); i < nr; i++) if (ct_ivs_may_write_in(c, f, nt_ref_at(nt, id, i), deep)) return 1;
  for (int i = 0, na = nt_num_arrs(nt, id); i < na; i++) {
    int n = 0;
    const int *v = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) if (ct_ivs_may_write_in(c, f, v[j], deep)) return 1;
  }
  return 0;
}
static int ct_contains(const NodeTable *nt, int root, int x) {
  if (root < 0) return 0;
  if (root == x) return 1;
  for (int i = 0, nr = nt_num_refs(nt, root); i < nr; i++) if (ct_contains(nt, nt_ref_at(nt, root, i), x)) return 1;
  for (int i = 0, na = nt_num_arrs(nt, root); i < na; i++) {
    int n = 0;
    const int *v = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (ct_contains(nt, v[j], x)) return 1;
  }
  return 0;
}

static int ct_ivs_list(Compiler *c, struct CtMacroFacts *f, int list, int stop, CtIvs *st);
/* the arms of if or unless node n: its statements, and its else part (an
   ElseNode's statements, or an elsif's IfNode, as a list of one) */
static int ct_ivs_branch(Compiler *c, struct CtMacroFacts *f, int n, int stop, CtIvs *st) {
  const NodeTable *nt = c->nt;
  int is_if = nt_kind(nt, n) == NK_IfNode;
  if (ct_ivs_may_write(c, f, nt_ref(nt, n, "predicate"))) ct_ivs_open(st);
  int then = nt_ref(nt, n, "statements"), el = nt_ref(nt, n, is_if ? "subsequent" : "else_clause");
  if (stop >= 0) {   /* the arm holding stop, from the state before the if */
    if (ct_contains(nt, then, stop)) return ct_ivs_list(c, f, then, stop, st);
    if (el >= 0 && nt_kind(nt, el) == NK_ElseNode) return ct_ivs_list(c, f, nt_ref(nt, el, "statements"), stop, st);
    if (el >= 0 && (nt_kind(nt, el) == NK_IfNode || nt_kind(nt, el) == NK_UnlessNode)) return ct_ivs_branch(c, f, el, stop, st);
    ct_ivs_open(st);
    return 1;
  }
  CtIvs other;
  ct_ivs_copy(&other, st);
  ct_ivs_list(c, f, then, -1, st);
  if (el >= 0 && nt_kind(nt, el) == NK_ElseNode) ct_ivs_list(c, f, nt_ref(nt, el, "statements"), -1, &other);
  else if (el >= 0) ct_ivs_branch(c, f, el, -1, &other);
  ct_ivs_meet(st, &other);
  ct_ivs_free(&other);
  return 0;
}
/* follow statement list `list` into st, up to statement stop (or within
   the statement that holds it): 1 when stop was reached */
/* what statement s (holding no stop) leaves in st */
static void ct_ivs_stmt(Compiler *c, struct CtMacroFacts *f, int s, CtIvs *st) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, s);
  if (k == NK_InstanceVariableWriteNode) {
    CtEnv none; memset(&none, 0, sizeof none);
    CtVal v;
    int known = ct_eval(nt, &none, nt_ref(nt, s, "value"), &v) && v.k == CT_VALUE && v.mv.k != MV_UNDEF;
    ct_ivs_put(st, nt_str(nt, s, "name"), known ? v.mv : mv_nil(), known);
    return;
  }
  if (k == NK_IfNode || k == NK_UnlessNode) { ct_ivs_branch(c, f, s, -1, st); return; }
  if (k == NK_CallNode && f)
    for (int j = 0; j < f->nscall; j++)
      if (f->scall[j] == s) {   /* a macro call left as written: what its run wrote */
        for (int x = 0; x < f->sw[j].n; x++) ct_ivs_put(st, f->sw[j].iv[x].name, f->sw[j].iv[x].v, f->sw[j].iv[x].known);
        if (f->sw[j].open) ct_ivs_open(st);
        return;
      }
  if (ct_ivs_may_write(c, f, s)) ct_ivs_open(st);
}
static int ct_ivs_list(Compiler *c, struct CtMacroFacts *f, int list, int stop, CtIvs *st) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *b = list >= 0 && nt_kind(nt, list) == NK_StatementsNode ? nt_arr(nt, list, "body", &n) : NULL;
  for (int i = 0; i < n; i++) {
    int s = b[i];
    NodeKind k = nt_kind(nt, s);
    if (s == stop) return 1;
    if (stop >= 0 && ct_contains(nt, s, stop)) {
      if (k == NK_IfNode || k == NK_UnlessNode) return ct_ivs_branch(c, f, s, stop, st);
      ct_ivs_open(st);
      return 1;
    }
    ct_ivs_stmt(c, f, s, st);
  }
  return 0;
}
/* class cls's state where call stands: its bodies in the order the walk
   reached them, up to the call */
static void ct_class_state(Compiler *c, struct CtMacroFacts *f, int cls, int call, int list, int j, CtIvs *st) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *b = nt_arr(nt, list, "body", &n);
  memset(st, 0, sizeof *st);
  f->s_cls = cls; f->s_p = elab_point_of(c, call);
  /* a call later in the body the last one stood in: on from there, so a
     body of K macro calls is followed once, not K times (c_* the last) */
  if (f->c_list == list && f->c_cls == cls && f->c_ok && j >= f->c_j && j < n) {
    ct_ivs_copy(st, &f->c_st);
    for (int i = f->c_j; i < j; i++) ct_ivs_stmt(c, f, b[i], st);
  }
  else
    for (int i = 0, prev = -1;; i++) {
      int hdr = -1, body = elab_class_opening_body(c, cls, i, &hdr);
      if (body == -2) { ct_ivs_open(st); break; }
      /* between two bodies, code that may reach the class (a call of a
         name the program defines, a nested reopening) leaves the state
         open; two headers in different lists are not followed between */
      if (prev >= 0 && !st->open) {
        int lp = elab_stmt_list(c, prev, NULL), lc = elab_stmt_list(c, hdr, NULL), sn = 0, ip = -1, ic = -1;
        const int *ss = lp >= 0 && lp == lc ? nt_arr(nt, lp, "body", &sn) : NULL;
        for (int k = 0; k < sn; k++) { if (ss[k] == prev) ip = k; if (ss[k] == hdr) ic = k; }
        if (ip < 0 || ic < ip) ct_ivs_open(st);
        for (int k = ip + 1; ip >= 0 && k < ic && !st->open; k++) if (ct_ivs_may_write_in(c, f, ss[k], 1)) ct_ivs_open(st);
      }
      prev = hdr;
      if (ct_ivs_list(c, f, body, call, st)) break;
    }
  ct_ivs_free(&f->c_st);
  ct_ivs_copy(&f->c_st, st);
  f->c_cls = cls; f->c_list = list; f->c_j = j; f->c_ok = j < n && b[j] == call;
}

/* Can const_set effect f of a macro run become `NAME = value` at the
   call: the core const_set, with no const_added hook; a name the run sets
   once; a value the evaluator knows and a node spells where the call
   stands (no class it made, and no constant, which names another scope
   in the macro's body than in the class's), or one it does not know,
   whose code runs there bound to the frame's locals (vcode)? The walk records the write
   at its point, and the load record keys it as any written constant. */
static int ct_macro_const_set_ok(Compiler *c, const CtEnv *e, int f, int cls, LoadPoint p) {
  const CtFx *x = &e->fx[f];
  /* (the class's const_set and const_added hook where the call stands, as
     the record holds them: the core's, and none) */
  if (x->names || x->val.k != CT_VALUE || elab_method_at(c, cls, "const_set", 1, p, NULL) != ELAB_NO ||
      elab_method_at(c, cls, "const_added", 1, p, NULL) != ELAB_NO) return 0;
  for (int j = 0; j < e->nfx; j++)   /* (nor one the run gives the class) */
    if ((e->fx[j].k == CT_FX_DEFINE_METHOD && e->fx[j].sg && (ct_eq(e->fx[j].name, "const_set") || ct_eq(e->fx[j].name, "const_added"))) ||
        e->fx[j].k == CT_FX_EVAL) return 0;
  for (int j = 0; j < f; j++) if (e->fx[j].k == CT_FX_CONST_SET && ct_eq(e->fx[j].name, x->name)) return 0;
  return x->vcode || ct_spellable(e, NULL, x->val);
}

/* Is statement list `list` the class body `body`, or an arm of an if or
   unless among its statements (at any depth of arms)? */
static int ct_body_arm(const NodeTable *nt, int body, int list) {
  if (body == list) return 1;
  int n = 0;
  const int *b = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  for (int i = 0; i < n; i++)
    for (int s = b[i]; s >= 0 && (nt_kind(nt, s) == NK_IfNode || nt_kind(nt, s) == NK_UnlessNode); ) {
      int is_if = nt_kind(nt, s) == NK_IfNode, el = nt_ref(nt, s, is_if ? "subsequent" : "else_clause");
      if (ct_body_arm(nt, nt_ref(nt, s, "statements"), list)) return 1;
      if (el >= 0 && nt_kind(nt, el) == NK_ElseNode) return ct_body_arm(nt, nt_ref(nt, el, "statements"), list);
      s = el;
    }
  return 0;
}

/* A macro call the form leaves as written, whose run the evaluator
   followed: the instance variables of self it writes when it runs (a
   statement left as written that may write others leaves the state open) */
static void ct_state_note(Compiler *c, HostStep *h, int call, CtEnv *e, int seeded, const int *seed_nw) {
  struct CtMacroFacts *f = ct_macro_facts(c, h);
  if (f->nscall == f->cscall) {
    f->cscall = f->cscall ? f->cscall * 2 : 8;
    f->scall = realloc(f->scall, sizeof *f->scall * (size_t)f->cscall);
    f->sw = realloc(f->sw, sizeof *f->sw * (size_t)f->cscall);
    if (!f->scall || !f->sw) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  CtIvs *w = &f->sw[f->nscall];
  memset(w, 0, sizeof *w);
  f->scall[f->nscall++] = call;
  for (int i = 0; i < e->nivar; i++)
    if (i >= seeded || e->ivar[i].nw != seed_nw[i])
      ct_ivs_put(w, e->ivar[i].name, e->ivar[i].v.mv, e->ivar[i].v.k == CT_VALUE && e->ivar[i].v.mv.k != MV_UNDEF);
  int only = 1;   /* its effects are self's instance variables alone */
  for (int i = 0; i < e->nfx; i++) {
    int ivw = e->fx[i].k == CT_FX_RESIDUAL && nt_kind(c->nt, e->fx[i].site) == NK_InstanceVariableWriteNode;
    if (e->fx[i].k == CT_FX_RESIDUAL && !ivw && ct_ivs_may_write(c, f, e->fx[i].site)) w->open = 1;
    only &= ivw || e->fx[i].k == CT_FX_LOCAL;
  }
  /* such a call leaves the class as the macro form follows it: a later
     macro call asks the record past it (elab_opaque_before_bar_state) */
  if (only && !w->open) nt_node_set_int((NodeTable *)c->nt, call, "ct_state_only", 1);
}

/* Did the program change a core method a table loop ran, as the record has
   it at the loop's point: a version of the program's that a call on the
   core class (or, for one the program does not reopen, on Object) finds
   there, or a method the walk's macros have defined of that name */
static int ct_loop_core_changed_at(const CtLoopAt *at, const CtRan *ran, int n) {
  Compiler *c = at->c;
  HostStep *h = elab_walk_host(c);
  struct CtMacroFacts *f = h ? ct_macro_facts(c, h) : NULL;
  int obj = comp_core_class(c, CORE_OBJECT);
  for (int i = 0; i < n; i++) {
    if (f && name_list_has(f->made, f->nmade, ran[i].meth)) return 1;
    int k = ran[i].cls ? comp_class_index(c, ran[i].cls) : -1;
    if (k < 0) k = obj;
    if (k >= 0 && elab_method_at(c, k, ran[i].meth, 0, at->p, NULL) != ELAB_NO) return 1;
  }
  return 0;
}

/* A table loop (ct_try_loop), statement j of class or module body list
   `list`, run by the walk at its point: a loop that makes no class, its
   questions answered by the record there (#10791). The first statement now
   in its place, or -1 where it stays as written. */
int ct_table_loop_form(Compiler *c, int st, int list, int j) {
  NodeTable *nt = (NodeTable *)c->nt;
  HostStep *h = elab_walk_host(c);
  int bn = 0, frame = -1;
  const int *bb = nt_arr(nt, list, "body", &bn);
  if (!h || nt_kind(nt, st) != NK_CallNode || nt_ref(nt, st, "block") < 0 || j >= bn || bb[j] != st ||
      !ct_loops_in_analysis(nt) || elab_stmt_list(c, st, &frame) != list ||
      !class_def_opens_frame(nt, frame) || class_def_body(nt, frame) != list)
    return -1;
  /* the loop's shape first (`H.each { |k, v| ... }`, H a constant): the
     walk's facts are made once, when first asked, and a call of another
     shape must not make them ahead of the macros that read them */
  int recv = nt_ref(nt, st, "receiver"), blk = nt_ref(nt, st, "block");
  const char *meth = nt_str(nt, st, "name");
  if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode || nt_ref(nt, st, "arguments") >= 0 ||
      nt_kind(nt, blk) != NK_BlockNode || !meth || !is_each_or_pair(meth))
    return -1;
  CtLoopAt at = { c, elab_self_at(c, st), elab_point_of(c, st), NULL };
  if (at.cls < 0) return -1;
  struct CtMacroFacts *f = ct_macro_facts(c, h);
  if (!f->cidx) f->cidx = ct_const_idx(nt, f->count);
  at.idx = f->cidx;
  int *before = malloc(sizeof(int) * ((size_t)bn + 1));
  if (!before) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(before, bb, sizeof(int) * (size_t)bn);
  int *out = NULL, n = 0, cap = 0, pcount = f->count, *parent = f->parent;
  for (int k = 0; k < j; k++) ct_push(&out, &n, &cap, before[k]);
  int ok = ct_try_loop(nt, &parent, &pcount, before, j, &out, &n, &cap, &at);
  if (!ok) { free(out); free(before); return -1; }
  int nout = n - j;
  for (int k = j + 1; k < bn; k++) ct_push(&out, &n, &cap, before[k]);
  nt_node_set_arr(nt, list, "body", out, n);
  free(out); free(before);
  comp_grow_node_arrays(c);
  host_splice(c, h, frame, list, j, nout, NULL, NULL);
  comp_grow_node_arrays(c);
  bb = nt_arr(nt, list, "body", &bn);
  int first = bb[j];
  elab_walk_replace(c, st, first);
  return first;
}

/* The macro call `call`, statement j of class body list `list` (or of an
   if's arm in it), run: the first statement now in its place, or -1 where
   it stays as written */
int ct_macro_form(Compiler *c, int call, int list, int j) {
  NodeTable *nt = (NodeTable *)c->nt;
  HostStep *h = elab_walk_host(c);
  const char *name = nt_str(nt, call, "name");
  int rv = nt_ref(nt, call, "receiver"), bn = 0, frame = -1;
  const int *bb = nt_arr(nt, list, "body", &bn);
  if (!h || !name || (rv >= 0 && nt_kind(nt, rv) != NK_SelfNode && !nt_int(nt, call, "self_call", 0)) ||
      nt_ref(nt, call, "block") >= 0 || j >= bn || bb[j] != call || !ct_macros_in_analysis(nt) || elab_stmt_list(c, call, &frame) != list ||
      !class_def_opens_frame(nt, frame) || !ct_body_arm(nt, class_def_body(nt, frame), list))
    return -1;
  /* the class, and what its call can meet at its point */
  int cls = elab_self_at(c, call);
  LoadPoint p = elab_point_of(c, call);
  if (cls < 0 || is_builtin_class_name(c->classes[cls].name) || is_builtin_module_name(c->classes[cls].name) ||
      elab_opaque_before_bar_state(c, cls, p) != ELAB_NO || elab_body_visibility_at(c, call) != BVIS_PUBLIC ||
      elab_method_at(c, cls, "method_added", 1, p, NULL) != ELAB_NO)
    return -1;
  /* run it: self the class, the parameters the call's values; the macros
     it calls in turn run in frames of their own */
  CtMacroHost mh = { c, cls, p };
  CtEnv e; memset(&e, 0, sizeof e);
  e.self_name = class_ruby_name(c, cls); e.self_core = ct_macro_core; e.self_macro = ct_macro_def; e.host = &mh;
  /* self's state as the class's statements before the call leave it */
  CtIvs st;
  ct_class_state(c, ct_macro_facts(c, h), cls, call, list, j, &st);
  for (int i = 0; i < st.n; i++) {
    CtVal v; memset(&v, 0, sizeof v);
    if (st.iv[i].known) v.mv = st.iv[i].v;
    else v.mv.k = MV_UNDEF;
    ct_env_ivar_set(&e, st.iv[i].name, v);
  }
  e.ivar_open = st.open;
  int seeded = e.nivar, *seed_nw = malloc(sizeof(int) * ((size_t)seeded + 1));
  if (!seed_nw) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < seeded; i++) seed_nw[i] = e.ivar[i].nw;
  ct_ivs_free(&st);
  int ran = ct_macro_call(nt, &e, call), ok = ran, defs = 0;
  /* a text effect's statements, grafted now (tid[f], tn[f]): each effect
     carries tn[f] statements, the others one */
  int **tid = calloc((size_t)e.nfx + 1, sizeof *tid), *tn = calloc((size_t)e.nfx + 1, sizeof *tn);
  if (!tid || !tn) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int f = 0; f < e.nfx && ok; f++) {
    CtFxKind k = e.fx[f].k;
    tn[f] = k != CT_FX_EVAL;
    if (k == CT_FX_EVAL) ok = ct_text_stmts(c, h, frame, cls, p, &e.fx[f], &tid[f], &tn[f]);
    defs += k == CT_FX_DEFINE_METHOD || k == CT_FX_ATTR || (k == CT_FX_EVAL && tn[f] > 0) || k == CT_FX_CONST_SET;
    ok = ok && (k == CT_FX_DEFINE_METHOD || k == CT_FX_ATTR || k == CT_FX_LOCAL || k == CT_FX_RESIDUAL || k == CT_FX_EVAL ||
                (k == CT_FX_CONST_SET && ct_macro_const_set_ok(c, &e, f, cls, p)));
    if (ok) ok = ct_fx_each_name(&e.fx[f], ct_name_unasked, c);
    }
  if (ok) ok = defs > 0 && !ct_macro_core_changed(c, h, e.ran, e.nran) && !ct_relied_redefined(nt, &e, tid, tn);
  /* the code each effect carries must run where the call stands; a local
     a promoted local's value reads may need promoting too */
  CtBind b = { nt, &e, 0, 0, 0, 0, 0, NULL, NULL, 0, 0 };
  for (int f = 0; f < e.nfx && ok; f++)
    if (e.fx[f].k != CT_FX_LOCAL && e.fx[f].k != CT_FX_ATTR && e.fx[f].k != CT_FX_EVAL &&
        (e.fx[f].k != CT_FX_CONST_SET || e.fx[f].vcode))
      ok = ct_bind_fx(&b, c, list, &e.fx[f], ct_fx_code(&e.fx[f], nt));
  unsigned char *bound = calloc((size_t)e.nfx + 1, 1);
  if (!bound) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int more = 1; ok && more; ) {
    more = 0;
    for (int f = 0; f < e.nfx && ok; f++)
      if (e.fx[f].k == CT_FX_LOCAL && !bound[f] && ct_promoted(&b, e.fx[f].frame, e.fx[f].name) &&
          !ct_local_made(&b, &e.fx[f])) {   /* (a holder promoted later makes it so: asked again) */
        ok = ct_bind_fx(&b, c, list, &e.fx[f], ct_fx_code(&e.fx[f], nt));
        bound[f] = more = 1;
      }
  }
  free(bound);
  /* the call's value is the macro's: the class body's, when it is last */
  int last = -1, nout = 0;
  for (int f = 0; f < e.nfx; f++) if (ct_fx_kept(&b, &e.fx[f]) && tn[f]) { last = f; nout += tn[f]; }
  if (ok && j == bn - 1) ok = last >= 0 && ct_fx_answers(nt, &e, &e.fx[last]);
  if (!ok) {
    if (ran) ct_state_note(c, h, call, &e, seeded, seed_nw);
    free(seed_nw);
    ct_macro_drop(nt, &b, &e, tid, tn, 0);
    return -1;
  }
  free(seed_nw);
  /* each statement, bound, where the call stood */
  int *out = malloc(sizeof(int) * (size_t)(bn + nout));
  if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(out, bb, sizeof(int) * (size_t)j);
  b.apply = 1;
  for (int f = 0, o = j; f < e.nfx; f++)
    if (e.fx[f].k == CT_FX_EVAL) for (int i = 0; i < tn[f]; i++) out[o++] = tid[f][i];
    else if (ct_fx_kept(&b, &e.fx[f])) out[o++] = ct_fx_stmt(&b, c, list, call, &e.fx[f]);
  bb = nt_arr(nt, list, "body", &bn);
  memcpy(out + j + nout, bb + j + 1, sizeof(int) * (size_t)(bn - j - 1));
  nt_node_set_arr(nt, list, "body", out, bn - 1 + nout);
  free(out);
  comp_grow_node_arrays(c);
  host_splice(c, h, frame, list, j, nout, NULL, NULL);
  comp_grow_node_arrays(c);
  /* what the walk defined since the facts' map (ct_macro_core_changed) */
  for (int f = 0; f < e.nfx; f++) ct_fx_each_name(&e.fx[f], ct_name_made, h->ct);
  for (int f = 0; f < e.nfx; f++)
    for (int i = 0; i < (e.fx[f].k == CT_FX_EVAL ? tn[f] : 0); i++) ct_name_made(h->ct, nt_str(nt, tid[f][i], "name"));
  ct_macro_drop(nt, &b, &e, tid, tn, 1);
  /* the call leaves the program; its statement is the first one's */
  bb = nt_arr(nt, list, "body", &bn);
  int first = bb[j];
  elab_walk_replace(c, call, first);
  nt_subtree_blank(nt, call);
  return first;
}
