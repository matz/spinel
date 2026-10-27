#include "analyze_internal.h"
#include "builtin_names.h"
#include "share.h"
#include "call_plan.h"
#include <errno.h>
#include <limits.h>

/* The builtin class/module/exception names: one table, Spinel's native
   classes (builtin_class_at, builtin_names.c), read here and by the load
   graph. */

/* The first row of each name, through a table hashed on the first ask: the
   lookup compared the name with every row, and is asked of class names per
   call (is_builtin_reopen, of each class a poly block dispatch weighs). */
#define BUILTIN_ROW_CAP 512   /* a power of two above 4 * the rows */
static short builtin_row_slot[BUILTIN_ROW_CAP];
static int builtin_row_built;
static int builtin_row_find(const char *n) {
  const BuiltinClass *r;
  if (!builtin_row_built) {
    for (int i = 0; i < BUILTIN_ROW_CAP; i++) builtin_row_slot[i] = -1;
    for (int i = 0; (r = builtin_class_at(i)) != NULL; i++) {
      unsigned h = sp_strhash(r->name) & (BUILTIN_ROW_CAP - 1);
      while (builtin_row_slot[h] >= 0 && !sp_streq(builtin_class_at(builtin_row_slot[h])->name, r->name))
        h = (h + 1) & (BUILTIN_ROW_CAP - 1);
      if (builtin_row_slot[h] < 0) builtin_row_slot[h] = (short)i;   /* a later row of the name: the first answers */
    }
    builtin_row_built = 1;
  }
  unsigned h = sp_strhash(n) & (BUILTIN_ROW_CAP - 1);
  while (builtin_row_slot[h] >= 0 && !sp_streq(builtin_class_at(builtin_row_slot[h])->name, n))
    h = (h + 1) & (BUILTIN_ROW_CAP - 1);
  return builtin_row_slot[h];
}

/* A socket row is a constant only after `require "socket"`, as in CRuby. */
static const BuiltinClass *builtin_row(const char *n) {
  if (!n) return NULL;
  int i = builtin_row_find(n);
  const BuiltinClass *r = i >= 0 ? builtin_class_at(i) : NULL;
  if (!r) return NULL;
  if ((r->flags & BC_SOCKET) && !sp_feature_required("socket")) return NULL;
  return r;
}

int is_builtin_class_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  return r && (r->flags & BC_CLASS);
}
int is_builtin_module_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  return r && (r->flags & BC_MODULE);
}
/* The i-th builtin MODULE (Comparable, Enumerable, Kernel, Math, Signal,
   GC, ObjectSpace, Process): its name and runtime class id, 0 for one that
   is a class value by name only. Answers 0 past the last. */
int builtin_module_at(int i, const char **name, int *id) {
  const BuiltinClass *r;
  for (int k = 0; (r = builtin_class_at(k)) != NULL; k++) {
    if (!(r->flags & BC_MODULE)) continue;
    if (i-- > 0) continue;
    *name = r->name; *id = r->id;
    return 1;
  }
  return 0;
}
int is_builtin_exception_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  if (r && (r->flags & BC_EXCEPTION)) return 1;
  /* the Errno:: family is open -- the runtime picks a name from errno -- so
     any qualified Errno name is an exception class (#2922 follow-up) */
  return n && strncmp(n, "Errno::", 7) == 0;
}
/* Does subtree n (not entering a nested class, module or method) write
   the constant nm? */
static int body_writes_const(const NodeTable *nt, int n, const char *nm) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode || k == NK_DefNode) return 0;
  if ((k == NK_ConstantWriteNode || k == NK_ConstantOrWriteNode || k == NK_ConstantAndWriteNode ||
       k == NK_ConstantOperatorWriteNode) && sp_streq(nt_str(nt, n, "name"), nm)) return 1;
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) if (body_writes_const(nt, nt_ref_at(nt, n, i), nm)) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0;
    const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (body_writes_const(nt, ids[j], nm)) return 1;
  }
  return 0;
}
/* Is `node` the path `pn::nm` (or `::pn::nm`)? */
static int const_path_is(const NodeTable *nt, int node, const char *pn, const char *nm) {
  if (node < 0 || !sp_streq(nt_str(nt, node, "name"), nm)) return 0;
  int par = nt_ref(nt, node, "parent");
  const char *qn = par >= 0 ? nt_str(nt, par, "name") : NULL;   /* none for `self::nm` */
  return qn && sp_streq(qn, pn);
}
/* Does the program write the constant nm into the class or module named pn:
   in a body of it, or through the path `pn::nm = v`? */
static int program_writes_const_in(Compiler *c, const char *pn, const char *nm) {
  const NodeTable *nt = c->nt;
  static const NodeKind bodies[] = { NK_ClassNode, NK_ModuleNode };
  for (int i = 0; i < 2; i++)
    NT_FOREACH_KIND(nt, bodies[i], d) {
      int cp = nt_ref(nt, d, "constant_path");
      if (cp < 0 || !sp_streq(nt_str(nt, cp, "name"), pn)) continue;
      if (body_writes_const(nt, nt_ref(nt, d, "body"), nm)) return 1;
    }
  static const NodeKind writes[] = { NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode,
                                     NK_ConstantPathAndWriteNode, NK_ConstantPathOperatorWriteNode };
  for (int i = 0; i < 4; i++)
    NT_FOREACH_KIND(nt, writes[i], w)
      if (const_path_is(nt, nt_ref(nt, w, "target"), pn, nm)) return 1;
  NT_FOREACH_KIND(nt, NK_ConstantPathTargetNode, t)
    if (const_path_is(nt, t, pn, nm)) return 1;
  return 0;
}
/* The Ruby leaf of a class's name: a name qualify_colliding_classes gave
   (`Cache__Coder`) carries its path, and its leaf is the last component. */
static const char *class_leaf_name(const char *n) {
  const char *l = n;
  for (const char *p = n; (p = strstr(p, "__")) != NULL; p += 2) l = p + 2;
  return l;
}
/* May NAME be a constant of program class or module k, by its own
   definitions or its ancestors' (a superclass, an included module)? A class
   or module the program nests in k, a constant it writes in a body of k or
   through `k::NAME = v`, or one a write the analysis cannot place may
   define (a path written through a dynamic parent, const_set). *sub is the
   class or module the member is, -1 when it is a value or unknown. */
static int class_may_have_const(Compiler *c, int k, const char *nm, int *sub, int depth) {
  if (k < 0 || k >= c->nclasses || depth > 32) return 0;
  ClassInfo *ki = &c->classes[k];
  if (!ki->name) return 1;
  for (int j = 0; j < c->nclasses; j++)
    if (c->classes[j].enclosing_class == k && c->classes[j].name &&
        (sp_streq(class_leaf_name(c->classes[j].name), nm) || sp_streq(c->classes[j].name, nm))) {
      *sub = j;
      return 1;
    }
  /* a class declared under a qualified name (native_struct "M::Leaf") is
     keyed by its leaf, with no enclosing link to check */
  size_t nl = strlen(nm);
  for (int j = 0; j < c->nclasses; j++) {
    const char *rn = c->classes[j].enclosing_class < 0 ? c->classes[j].ruby_name_cache : NULL;
    size_t rl = rn ? strlen(rn) : 0;
    if (rl > nl + 2 && sp_streq(rn + rl - nl, nm) && !strncmp(rn + rl - nl - 2, "::", 2)) { *sub = j; return 1; }
  }
  const char *kl = class_leaf_name(ki->name);
  if (program_writes_const_in(c, ki->name, nm) || (kl != ki->name && program_writes_const_in(c, kl, nm))) {
    *sub = comp_class_index(c, nm);
    return 1;
  }
  if (class_may_have_const(c, ki->parent, nm, sub, depth + 1)) return 1;
  for (int i = 0; i < ki->nincluded_mods; i++)
    if (class_may_have_const(c, ki->included_mods[i], nm, sub, depth + 1)) return 1;
  return 0;
}
/* Can the program define constant `nm` where no class body or `P::NAME = v`
   path shows it: through a path whose parent is not a constant
   (`self::NAME = v`, `klass::NAME = v`), which places the name it writes?
   A const_set places none: with a literal name it re-assigns a constant
   the program already defines, a C global, and with a computed one it is
   refused (or raises, deferred), so neither makes NAME a member of P. A
   send dispatch's const_set arm (`target.send(m, *args)`, desugared over
   every candidate name) is one with a computed name. */
typedef struct { int any; int n, cap; const char **names; } DynConsts;
static void dyn_consts_add(DynConsts *d, const char *nm) {
  if (!nm) { d->any = 1; return; }
  if (d->n == d->cap) {
    d->cap = d->cap ? d->cap * 2 : 16;
    d->names = realloc(d->names, (size_t)d->cap * sizeof *d->names);
  }
  d->names[d->n++] = nm;
}
static void places_consts_dynamically(Compiler *c, DynConsts *d) {
  const NodeTable *nt = c->nt;
  static const NodeKind writes[] = { NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode,
                                     NK_ConstantPathAndWriteNode, NK_ConstantPathOperatorWriteNode };
  for (int i = 0; i < 4; i++)
    NT_FOREACH_KIND(nt, writes[i], w) {
      int t = nt_ref(nt, w, "target"), q = t >= 0 ? nt_ref(nt, t, "parent") : -1;
      if (q >= 0 && nt_kind(nt, q) != NK_ConstantReadNode && nt_kind(nt, q) != NK_ConstantPathNode)
        dyn_consts_add(d, nt_str(nt, t, "name"));
    }
  NT_FOREACH_KIND(nt, NK_ConstantPathTargetNode, t) {
    int q = nt_ref(nt, t, "parent");
    if (q >= 0 && nt_kind(nt, q) != NK_ConstantReadNode && nt_kind(nt, q) != NK_ConstantPathNode)
      dyn_consts_add(d, nt_str(nt, t, "name"));
  }
}
static int program_places_const_dynamically(Compiler *c, const char *nm) {
  /* a walk over the program, asked once per defined? path: once per tree */
  static long long memo_ver = -1; static DynConsts memo;
  if (c->nt->version != memo_ver) {
    free(memo.names);
    memset(&memo, 0, sizeof memo);
    places_consts_dynamically(c, &memo);
    memo_ver = c->nt->version;
  }
  if (memo.any) return 1;
  for (int i = 0; i < memo.n; i++) if (sp_streq(memo.names[i], nm)) return 1;
  return 0;
}
int const_seg_builtin(Compiler *c, int id);
/* The scope a constant path segment names, for defined?: a class or module
   of the program (its index), or -1 for one whose constants are not the
   program's to list (a builtin, the top level, a value, a dynamic parent).
   *missing is set when a segment is provably no member of its parent. */
static int const_path_scope(Compiler *c, int seg, int *missing) {
  const NodeTable *nt = c->nt;
  NodeKind sk = nt_kind(nt, seg);
  const char *nm = nt_str(nt, seg, "name");
  if (!nm) return -1;
  if (sk == NK_ConstantReadNode) {
    int k = comp_class_index(c, nm);
    return k >= 0 && !c->classes[k].is_builtin_const && !comp_const(c, nm) ? k : -1;
  }
  if (sk != NK_ConstantPathNode) return -1;
  int par = nt_ref(nt, seg, "parent");
  int pk;
  if (par < 0) {                                  /* `::NAME`: the top level */
    int k = comp_class_index(c, nm);
    return k >= 0 && c->classes[k].enclosing_class < 0 && !c->classes[k].is_builtin_const &&
           !comp_const(c, nm) ? k : -1;
  }
  NodeKind pkk = nt_kind(nt, par);
  if (pkk != NK_ConstantReadNode && pkk != NK_ConstantPathNode) return -1;
  pk = const_path_scope(c, par, missing);
  if (*missing || pk < 0) {
    /* a builtin, the top level, a value: the leaf decides, by its name */
    int k = comp_class_index(c, nm);
    return !*missing && k >= 0 && !c->classes[k].is_builtin_const && !comp_const(c, nm) ? k : -1;
  }
  int sub = -1;
  if (class_may_have_const(c, pk, nm, &sub, 0)) return sub >= 0 && !c->classes[sub].is_builtin_const ? sub : -1;
  if (program_places_const_dynamically(c, nm)) return -1;
  /* a member the runtime gives a program module: a builtin exception
     (JSON::ParserError) or an FFI constant */
  const char *pn = nt_str(nt, par, "name");
  if (const_seg_builtin(c, seg)) return -1;
  for (int i = 0; pn && i < c->n_ffi_consts; i++)
    if (sp_streq(c->ffi_consts[i].mod, pn) && sp_streq(c->ffi_consts[i].name, nm)) return -1;
  *missing = 1;
  return -1;
}
/* `P::...::NAME` where some segment names a class or module of the program
   that holds no such constant: the path names nothing, though each segment
   resolves by its own name (JSON::Coder, with only Cache::Coder defined). */
int const_path_misses_member(Compiler *c, int id) {
  if (nt_kind(c->nt, id) != NK_ConstantPathNode) return 0;
  int missing = 0;
  const_path_scope(c, id, &missing);
  return missing;
}
/* The class or module of the program a constant path names, by the
   membership walk above (A::B where two modules each nest a B, renamed
   apart): -1 when it names none, or one the walk cannot list. */
int const_path_class(Compiler *c, int id) {
  if (nt_kind(c->nt, id) != NK_ConstantPathNode) return -1;
  int missing = 0;
  int k = const_path_scope(c, id, &missing);
  return missing ? -1 : k;
}
/* Does the constant or constant path segment name a builtin: a builtin
   class, module or exception (Encoding, Thread::Mutex, Errno::ENOENT), or a
   constant of a builtin class or module the compiler reads (Float::INFINITY,
   File::SEPARATOR, Encoding::UTF_8)? Defined, though nothing the program
   writes has the name. */
static int builtin_const_name(const char *n) {
  return n && (sp_streq(n, "Errno") || is_builtin_class_name(n) || is_builtin_module_name(n) || is_builtin_exception_name(n));
}
int const_seg_builtin(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (k != NK_ConstantReadNode && k != NK_ConstantPathNode)) return 0;
  int par = k == NK_ConstantPathNode ? nt_ref(nt, id, "parent") : -1;
  if (par < 0) return builtin_const_name(nm);
  NodeKind pk = nt_kind(nt, par);
  if (pk != NK_ConstantReadNode && pk != NK_ConstantPathNode) return 0;
  const char *pn = nt_str(nt, par, "name");
  if (!pn) return 0;
  char qn[256];
  snprintf(qn, sizeof qn, "%s::%s", pn, nm);
  if (builtin_const_name(qn)) return 1;
  return builtin_const_name(pn) && infer_cext_builtin_constant(c, id);
}
/* Does the program define a class or module whose own name is `leaf`, nested
   in another (Frame::JSON::Encoding)? Inside it, `leaf::X` can name it. */
static int program_nests_class_named(Compiler *c, const char *leaf) {
  size_t ll = strlen(leaf);
  for (int k = 0; k < c->nclasses; k++) {
    const char *n = c->classes[k].name;
    size_t l = n ? strlen(n) : 0;
    if (l > ll + 2 && sp_streq(n + l - ll, leaf) && !strncmp(n + l - ll - 2, "::", 2)) return 1;
    n = c->classes[k].c_name;
    l = n ? strlen(n) : 0;
    if (l > ll + 2 && sp_streq(n + l - ll, leaf) && !strncmp(n + l - ll - 2, "__", 2)) return 1;
  }
  return 0;
}
/* Is class j's own (leaf) name `leaf`? A nested class may carry its path
   (`Frame::JSON::Encoding`, or `Frame__JSON__Encoding` once qualified). */
static int class_leaf_is(Compiler *c, int j, const char *leaf) {
  size_t ll = strlen(leaf);
  const char *n = c->classes[j].name;
  size_t l = n ? strlen(n) : 0;
  if (l == ll && sp_streq(n, leaf)) return 1;
  if (l > ll + 2 && sp_streq(n + l - ll, leaf) && !strncmp(n + l - ll - 2, "::", 2)) return 1;
  n = c->classes[j].c_name;
  l = n ? strlen(n) : 0;
  return l > ll + 2 && sp_streq(n + l - ll, leaf) && !strncmp(n + l - ll - 2, "__", 2);
}
/* Does node `id` sit inside a namespace that holds its own class or module
   `leaf` (in AS::JSON, or a class nested in it, `Encoding` is
   AS::JSON::Encoding)? The enclosing body and the classes around it stand
   for the lexical scopes. Without the class-body map, any nested `leaf`
   counts. */
static int lexically_sees_nested(Compiler *c, int id, const char *leaf) {
  if (!c->node_cbody || id >= c->node_cap) return program_nests_class_named(c, leaf);
  int d = 0;
  for (int k = c->node_cbody[id]; k >= 0 && k < c->nclasses && d < 64; k = c->classes[k].enclosing_class, d++)
    for (int j = 0; j < c->nclasses; j++)
      if (c->classes[j].enclosing_class == k && class_leaf_is(c, j, leaf)) return 1;
  return 0;
}
/* `P::NAME` where P is a builtin class or module: NAME is P's own constant,
   never a program constant that shares the leaf name (the leaf-keyed table
   answered M::Flags::BINARY for Encoding::BINARY), unless the program writes
   NAME into P. Object is not one: its constants are the top-level ones. Nor
   is a P the path names lexically: one nested in a namespace the path is
   written in. */
int const_path_in_closed_builtin(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_ConstantPathNode) return 0;
  int par = nt_ref(nt, id, "parent");
  if (par < 0) return 0;
  NodeKind pk = nt_kind(nt, par);
  /* `Encoding::X` or `::Encoding::X` */
  if (!(pk == NK_ConstantReadNode || (pk == NK_ConstantPathNode && nt_ref(nt, par, "parent") < 0)))
    return 0;
  const char *pn = nt_str(nt, par, "name"), *nm = nt_str(nt, id, "name");
  if (!pn || !nm || sp_streq(pn, "Object")) return 0;
  if (!is_builtin_class_name(pn) && !is_builtin_module_name(pn)) return 0;
  if (comp_const(c, pn)) return 0;
  /* the walks below look at the whole program: once per path while the
     tree and the class table stay as they are */
  static signed char *memo; static int memo_cap, memo_ncls; static unsigned memo_ver;
  if (nt->version != memo_ver || c->nclasses != memo_ncls || nt->count > memo_cap) {
    if (nt->count > memo_cap) {
      memo_cap = nt->count * 2;
      free(memo);
      memo = malloc((size_t)memo_cap);
      if (!memo) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    memset(memo, -1, (size_t)memo_cap);
    memo_ver = nt->version; memo_ncls = c->nclasses;
  }
  if (memo[id] < 0)
    memo[id] = (signed char)(!lexically_sees_nested(c, id, pn) && !program_writes_const_in(c, pn, nm));
  return memo[id];
}
/* Can the path write `t` (`Q::NAME = v`) define NAME where a lookup
   through class k finds it: Q is k, an ancestor of k, or a module one of
   them includes? `::NAME = v` writes the top level; a Q the program has no
   class for counts, as it may be any of them. */
static int const_path_write_reaches(Compiler *c, int t, int k) {
  const NodeTable *nt = c->nt;
  int q = nt_ref(nt, t, "parent");
  if (q < 0) return 0;
  NodeKind qk = nt_kind(nt, q);
  const char *qn = qk == NK_ConstantReadNode || qk == NK_ConstantPathNode ? nt_str(nt, q, "name") : NULL;
  int qi = qn ? comp_class_index(c, qn) : -1;
  if (qi < 0) return 1;
  for (int a = k, d = 0; a >= 0 && a < c->nclasses && d < 64; a = c->classes[a].parent, d++)
    if (comp_module_includes(c, a, qi, 0)) return 1;
  return 0;
}
/* `P::NAME` where P is a class or module of the program (not Object) and
   NAME is a constant the program writes only at the top level: Object's
   constant, which a path through another class or module does not reach
   (CRuby's NameError since 2.5), where the leaf-keyed table answered it.
   A NAME written in any body, or through a path, is left to the table. */
int const_path_misses_toplevel(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_ConstantPathNode || !c->node_cbody) return 0;
  int par = nt_ref(nt, id, "parent");
  if (par < 0) return 0;
  NodeKind pk = nt_kind(nt, par);
  if (pk != NK_ConstantReadNode && pk != NK_ConstantPathNode) return 0;
  const char *pn = nt_str(nt, par, "name"), *nm = nt_str(nt, id, "name");
  if (!pn || !nm || sp_streq(pn, "Object") || comp_class_index(c, pn) < 0 || comp_const(c, pn)) return 0;
  static const NodeKind writes[] = { NK_ConstantWriteNode, NK_ConstantOrWriteNode, NK_ConstantAndWriteNode,
                                     NK_ConstantOperatorWriteNode, NK_ConstantTargetNode };
  int top = 0;
  for (int i = 0; i < 5; i++)
    NT_FOREACH_KIND(nt, writes[i], w) {
      if (!sp_streq(nt_str(nt, w, "name"), nm)) continue;
      if (w >= c->node_cap || c->node_cbody[w] >= 0) return 0;
      top = 1;
    }
  static const NodeKind paths[] = { NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode,
                                    NK_ConstantPathAndWriteNode, NK_ConstantPathOperatorWriteNode };
  int k = comp_class_index(c, pn);
  for (int i = 0; i < 4; i++)
    NT_FOREACH_KIND(nt, paths[i], w) {
      int t = nt_ref(nt, w, "target");
      if (t >= 0 && sp_streq(nt_str(nt, t, "name"), nm) && const_path_write_reaches(c, t, k)) return 0;
    }
  NT_FOREACH_KIND(nt, NK_ConstantPathTargetNode, t)
    if (sp_streq(nt_str(nt, t, "name"), nm) && const_path_write_reaches(c, t, k)) return 0;
  return top;
}
/* The SystemCallError family, by builtin name: SystemCallError, the Errno
   classes and the IO wait-readiness classes under them. Its #initialize
   builds the message from the class's errno (syserr_initialize). */
int is_syserr_family_name(const char *n) {
  if (!n) return 0;
  return sp_streq(n, "SystemCallError") || strncmp(n, "Errno::", 7) == 0 ||
         (strncmp(n, "IO::E", 5) == 0 && strstr(n, "Wait") && is_builtin_exception_name(n));
}
/* The Errno:: class a name stands for. CRuby defines one class per errno
   number: a later name whose number an earlier class already has is a
   constant for that class, so on Linux Errno::EOPNOTSUPP *is* Errno::ENOTSUP
   (both 95) while on macOS (102, 45) they are two classes. The numbers are
   the C library's, so the compiler asks its own <errno.h>, the one the
   program's runtime is built against. Returns the canonical name, or n. */
const char *errno_canonical_name(const char *n) {
  if (!n || strncmp(n, "Errno::", 7) != 0) return n;
  static const struct { const char *alias, *canon; int same; } ALIASES[] = {
#if defined(EWOULDBLOCK) && defined(EAGAIN)
    { "Errno::EWOULDBLOCK", "Errno::EAGAIN", EWOULDBLOCK == EAGAIN },
#endif
#if defined(EOPNOTSUPP) && defined(ENOTSUP)
    { "Errno::EOPNOTSUPP", "Errno::ENOTSUP", EOPNOTSUPP == ENOTSUP },
#endif
#if defined(EDEADLOCK) && defined(EDEADLK)
    { "Errno::EDEADLOCK", "Errno::EDEADLK", EDEADLOCK == EDEADLK },
#endif
    { NULL, NULL, 0 }
  };
  for (int i = 0; ALIASES[i].alias; i++)
    if (ALIASES[i].same && strcmp(n, ALIASES[i].alias) == 0) return ALIASES[i].canon;
  return n;
}

/* Defined here rather than in codegen_util.c so the id and the name
   predicates cannot disagree; codegen_internal.h still declares it. */
/* The builtin class above a builtin a program can subclass, as the generated
   sp_builtin_superclass table answers it (the exception chain is asked of
   the runtime instead): File is an IO, the numbers are Numeric, everything
   else sits under Object (-116). */
int builtin_class_parent_id(int id) {
  switch (id) {
    case -121: return -120;                                    /* File -> IO */
    case -100: case -101: case -131: case -142: return -113;   /* -> Numeric */
    default: return -116;
  }
}

int builtin_class_id(const char *name) {
  const BuiltinClass *r = builtin_row(name);
  return r ? r->id : 0;
}
/* The builtin exception a class's superclass node `sc` names, or NULL. A
   path is read whole first: `class E < Errno::ENOENT` names Errno::ENOENT,
   which its leaf "ENOENT" is not, so the class was taken for a plain object
   and `raise E` failed. A path whose whole name is no builtin keeps the
   leaf reading it always had (`::StandardError`, `Mod::RuntimeError`). The
   result is a stable string: the qualified forms are interned here. */
const char *superclass_builtin_exc_name(const NodeTable *nt, int sc) {
  const char *sty = sc >= 0 ? nt_type(nt, sc) : NULL;
  NodeKind sk = nt_kind(nt, sc);
  if (!sty) return NULL;
  int is_read = sk == NK_ConstantReadNode, is_path = sk == NK_ConstantPathNode;
  if (!is_read && !is_path) return NULL;
  const char *leaf = nt_str(nt, sc, "name");
  if (!leaf) return NULL;
  if (is_path) {
    char q[256]; size_t o = strlen(leaf);
    if (o < sizeof q) {
      memcpy(q, leaf, o + 1);
      int ok = 1, any = 0;
      for (int par = nt_ref(nt, sc, "parent"); par >= 0; par = nt_ref(nt, par, "parent")) {
        NodeKind pk = nt_kind(nt, par);
        const char *pn = nt_str(nt, par, "name");
        if (!pn || (pk != NK_ConstantReadNode && pk != NK_ConstantPathNode)) { ok = 0; break; }
        size_t pl = strlen(pn);
        if (o + pl + 2 >= sizeof q) { ok = 0; break; }
        memmove(q + pl + 2, q, o + 1); memcpy(q, pn, pl); q[pl] = ':'; q[pl + 1] = ':';
        o += pl + 2; any = 1;
        if (pk == NK_ConstantReadNode) break;
      }
      if (ok && any && is_builtin_exception_name(q)) {
        /* grows: a full table would send the name to the leaf reading
           below, and "ENOENT" alone names no exception */
        static char **interned; static int ninterned, cap;
        for (int i = 0; i < ninterned; i++) if (sp_streq(interned[i], q)) return interned[i];
        if (ninterned == cap) {
          cap = cap ? cap * 2 : 16;
          interned = realloc(interned, sizeof *interned * (size_t)cap);
        }
        return interned[ninterned++] = strdup(q);
      }
    }
  }
  return is_builtin_exception_name(leaf) ? leaf : NULL;
}
int class_inherits_builtin_exception(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent)
    if (superclass_builtin_exc_name(c->nt, nt_ref(c->nt, c->classes[k].def_node, "superclass")))
      return 1;
  return 0;
}
int an_re_has_captures(const char *src) {
  if (!src) return 0;
  int in_class = 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    /* a `(` inside a `[...]` character class is a literal paren, not a group
       (`/[()]/` has no captures) (#2912) */
    if (in_class) { if (*p == ']') in_class = 0; continue; }
    if (*p == '[') { in_class = 1; continue; }
    if (*p == '(') {
      if (p[1] != '?') return 1;
      /* (?<name>...) and (?'name'...) are named CAPTURE groups
         ((?<=..)/(?<!..) lookbehinds are not) */
      if (p[1] == '?' && p[2] == '<' && p[3] != '=' && p[3] != '!') return 1;
      if (p[1] == '?' && p[2] == 0x27) return 1;
    }
  }
  return 0;
}
/* The literal a Regexp local holds where it is read at `read`: the one
   pattern every write to the local in the read's scope assigns, or -1. A
   local is its scope's, so two methods' `r` are two locals (they were
   resolved by name, and the second method matched with the first's
   pattern), and a local written twice (`r = /a/ ... r = /b/`, or again in
   a block) holds whichever write ran last, which no one literal names. A
   parameter holds its caller's. Only looks: inference (an_regex_lit_src)
   and codegen (re_lit_node) both ask it, so a type rule and the emit arm
   agree on which patterns are static. */
int an_regex_local_lit(Compiler *c, int read) {
  const NodeTable *nt = c->nt;
  const char *nm = read >= 0 ? nt_str(nt, read, "name") : NULL;
  Scope *sc = nm ? comp_scope_of(c, read) : NULL;
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  if (!lv || lv->is_param || lv->is_block_param) return -1;
  int found = -1;
  for (int k = 0; k < nt->count; k++) {
    NodeKind kk = nt_kind(nt, k);
    if (kk != NK_LocalVariableWriteNode && kk != NK_LocalVariableOrWriteNode &&
        kk != NK_LocalVariableAndWriteNode && kk != NK_LocalVariableOperatorWriteNode &&
        kk != NK_LocalVariableTargetNode) continue;
    const char *kn = nt_str(nt, k, "name");
    if (!kn || !sp_streq(kn, nm) || comp_scope_of(c, k) != sc) continue;
    int v = kk == NK_LocalVariableWriteNode ? nt_ref(nt, k, "value") : -1;
    if (v < 0 || nt_kind(nt, v) != NK_RegularExpressionNode) return -1;
    if (found >= 0 &&
        (nt_regexp_opts(nt, v) != nt_regexp_opts(nt, found) ||
         !sp_streq(nt_str(nt, v, "unescaped"), nt_str(nt, found, "unescaped"))))
      return -1;
    if (found < 0) found = v;
  }
  return found;
}
/* The source text of the regex literal behind `nid`, or NULL when the pattern
   is only known at run time (an interpolated literal, a `Regexp.new(s)` call,
   a method's return). Deliberately mirrors codegen's re_lit_index resolution
   -- a bare literal, a constant bound to one (`PAT = /re/[.freeze]`), or a
   regex-typed local bound to one (an_regex_local_lit, which both share) -- so a type rule here and the emit arm there
   agree on which patterns are statically visible. Unlike re_lit_index this
   only looks, never registers a pattern slot, so it is safe to call during
   inference. */
const char *an_regex_lit_src(Compiler *c, int nid) {
  const NodeTable *nt = c->nt;
  if (nid < 0) return NULL;
  const char *ty = nt_type(nt, nid);
  NodeKind ty_k = nt_kind(nt, nid);
  if (!ty) return NULL;
  if (ty_k == NK_RegularExpressionNode) return nt_str(nt, nid, "unescaped");
  int want_const = ty_k == NK_ConstantReadNode || ty_k == NK_ConstantPathNode;
  int want_local = ty_k == NK_LocalVariableReadNode && infer_type(c, nid) == TY_REGEX;
  if (!want_const && !want_local) return NULL;
  const char *nm = nt_str(nt, nid, "name");
  if (!nm) return NULL;
  if (want_local) {
    int lit = an_regex_local_lit(c, nid);
    return lit >= 0 ? nt_str(nt, lit, "unescaped") : NULL;
  }
  for (int k = 0; k < nt->count; k++) {
    const char *kt = nt_type(nt, k);
    NodeKind kk = nt_kind(nt, k);
    if (!kt) continue;
    if (kk != NK_ConstantWriteNode && kk != NK_ConstantPathWriteNode) continue;
    const char *kn = nt_str(nt, k, "name");
    if (!kn || !sp_streq(kn, nm)) continue;
    int v = nt_ref(nt, k, "value");
    if (v >= 0 && nt_kind(nt, v) == NK_CallNode &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze"))
      v = nt_ref(nt, v, "receiver");
    if (v >= 0 && nt_kind(nt, v) == NK_RegularExpressionNode)
      return nt_str(nt, v, "unescaped");
  }
  return NULL;
}
/* The CallNodes of each name, ascending, rebuilt whenever the node table
   changes (a rename or a new node bumps its version). Passes that asked "the
   calls named m" walked every call per ask (rubys/roundhouse#72). A caller
   must still check the node, which a re-point since the build may have
   changed; the list only narrows the walk. */
static ANameHash g_cn_names;
static int *g_cn_first, *g_cn_next, *g_cn_last, g_cn_cap;
static const NodeTable *g_cn_nt;
static unsigned g_cn_ver, g_cn_clones;
static int g_cn_cnt = -1;
static int g_cn_empty = -1;
/* call `id` onto its name's chain, at the end: the chains are ascending */
static void cn_add(const NodeTable *nt, int id) {
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return;
  g_cn_next[id] = -1;
  int k = anh_find(&g_cn_names, nm);
  if (k < 0) {
    anh_add(&g_cn_names, nm); k = g_cn_names.n - 1;
    if (k >= g_cn_cap) {
      g_cn_cap *= 2;
      g_cn_first = realloc(g_cn_first, sizeof(int) * (size_t)g_cn_cap);
      g_cn_last = realloc(g_cn_last, sizeof(int) * (size_t)g_cn_cap);
      if (!g_cn_first || !g_cn_last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    g_cn_first[k] = id;
  }
  else g_cn_next[g_cn_last[k]] = id;
  g_cn_last[k] = id;
}
int an_calls_named_first(Compiler *c, const char *name) {
  const NodeTable *nt = c->nt;
  if (!name) return -1;
  if (g_cn_nt == nt && nt->count > g_cn_cnt && g_cn_ver != nt->version &&
      nt->version - g_cn_ver == nt->clone_version - g_cn_clones) {
    /* Only subtrees were cloned since the build, onto the end of the
       table: the nodes indexed are as they were, and the clones' calls go
       onto their chains. make_yield_proc_forms asks for the calls of each
       yielding method's name and clones between the asks, and each ask
       rebuilt the index over every call of the program (600k on a Rails
       program, 85% of a sampled window). */
    int *grown = realloc(g_cn_next, sizeof(int) * (size_t)nt->count);
    if (!grown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    g_cn_next = grown;
    for (int id = g_cn_cnt; id < nt->count; id++)
      if (nt_kind(nt, id) == NK_CallNode) cn_add(nt, id);
    g_cn_ver = nt->version; g_cn_clones = nt->clone_version; g_cn_cnt = nt->count;
  }
  if (g_cn_nt != nt || g_cn_ver != nt->version || g_cn_cnt != nt->count) {
    anh_free(&g_cn_names); memset(&g_cn_names, 0, sizeof g_cn_names);
    free(g_cn_first); free(g_cn_next); free(g_cn_last);
    g_cn_cap = 64;
    g_cn_first = malloc(sizeof(int) * (size_t)g_cn_cap);
    g_cn_last = malloc(sizeof(int) * (size_t)g_cn_cap);
    g_cn_next = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    if (!g_cn_first || !g_cn_next || !g_cn_last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int id = comp_kind_first(c, NK_CallNode); id >= 0; id = comp_kind_next(c, id)) cn_add(nt, id);
    g_cn_nt = nt; g_cn_ver = nt->version; g_cn_clones = nt->clone_version; g_cn_cnt = nt->count;
  }
  int k = anh_find(&g_cn_names, name);
  return k >= 0 ? g_cn_first[k] : -1;
}
int an_calls_named_next(int id) { return (id >= 0 && id < g_cn_cnt) ? g_cn_next[id] : -1; }

/* The LocalVariableWriteNodes of each name, ascending, rebuilt whenever the
   node table changes, as the calls above are. Resolving a Method-typed local
   walked every write of the program per read (method_recv_node), once per
   call site a bound builtin wrapper counts (bam_call_argc). */
static ANameHash g_lw_names;
static int *g_lw_first, *g_lw_next, g_lw_cap;
static const NodeTable *g_lw_nt;
static unsigned g_lw_ver;
static int g_lw_cnt = -1;
int an_local_writes_named_first(Compiler *c, const char *name) {
  const NodeTable *nt = c->nt;
  if (!name) return -1;
  if (g_lw_nt != nt || g_lw_ver != nt->version || g_lw_cnt != nt->count) {
    anh_free(&g_lw_names); memset(&g_lw_names, 0, sizeof g_lw_names);
    free(g_lw_first); free(g_lw_next);
    g_lw_cap = 64;
    g_lw_first = malloc(sizeof(int) * (size_t)g_lw_cap);
    g_lw_next = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    int *last = malloc(sizeof(int) * (size_t)g_lw_cap);
    if (!g_lw_first || !g_lw_next || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int id = comp_kind_first(c, NK_LocalVariableWriteNode); id >= 0; id = comp_kind_next(c, id)) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm) continue;
      g_lw_next[id] = -1;
      int k = anh_find(&g_lw_names, nm);
      if (k < 0) {
        anh_add(&g_lw_names, nm); k = g_lw_names.n - 1;
        if (k >= g_lw_cap) {
          g_lw_cap *= 2;
          g_lw_first = realloc(g_lw_first, sizeof(int) * (size_t)g_lw_cap);
          last = realloc(last, sizeof(int) * (size_t)g_lw_cap);
          if (!g_lw_first || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        g_lw_first[k] = id;
      }
      else g_lw_next[last[k]] = id;
      last[k] = id;
    }
    free(last);
    g_lw_nt = nt; g_lw_ver = nt->version; g_lw_cnt = nt->count;
  }
  int k = anh_find(&g_lw_names, name);
  return k >= 0 ? g_lw_first[k] : -1;
}
int an_local_writes_named_next(int id) { return (id >= 0 && id < g_lw_cnt) ? g_lw_next[id] : -1; }

int str_in(const char *s, const char *const *set) {
  if (!s) return 0;
  for (int i = 0; set[i]; i++) if (sp_streq(s, set[i])) return 1;
  return 0;
}
/* A construct whose VALUE is an empty container. Its type reads UNKNOWN
   because it carries no element type, which is not the same as producing no
   value -- and the two were conflated wherever an expression's type decides
   whether to keep its result. In a `rescue` that meant the array was built
   and thrown away (`x = ([] rescue 0)` assigned nil), and in a begin/rescue
   the value unified to the handler's Integer and the array pointer went into
   an sp_int slot (#3495, #3496). An `Array.new` or `Hash.new` the program
   answers with its own class method is that method's value, not an empty
   container. */
int node_is_empty_container(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  node = nt_paren_single(nt, node);   /* `([])` is as empty as `[]` */
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ArrayNode || k == NK_HashNode || k == NK_KeywordHashNode) {
    int n = 0;
    nt_arr(nt, node, k == NK_ArrayNode ? "elements" : "elements", &n);
    return n == 0;
  }
  if (k != NK_CallNode || nt_ref(nt, node, "block") >= 0) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || !sp_streq(nm, "new")) return 0;
  int r = nt_ref(nt, node, "receiver");
  if (r < 0 || nt_kind(nt, r) != NK_ConstantReadNode) return 0;
  const char *rn = nt_str(nt, r, "name");
  if (!rn || (!sp_streq(rn, "Array") && !sp_streq(rn, "Hash"))) return 0;
  int ca = nt_ref(nt, node, "arguments");
  int argc = 0; if (ca >= 0) nt_arr(nt, ca, "arguments", &argc);
  return argc == 0 && !cplan_const_user(c, node, rn, 1);
}

int is_arith_op(const char *op) {
  static const char *const set[] = {"+", "-", "*", "/", "%", "**", NULL};
  return str_in(op, set);
}
int is_cmp_op(const char *op) {
  static const char *const set[] = {"<", ">", "<=", ">=", NULL};
  return str_in(op, set);
}
/* The numeric operations CRuby routes through #coerce: the arithmetic
   operators, the ordered comparisons and `<=>`, and the named division family.
   `==` is deliberately absent -- Numeric#== answers false for an operand it
   cannot coerce rather than asking, so routing it here would turn a plain
   false into the coerced comparison. */
int is_numeric_coerce_op(const char *op) {
  static const char *const set[] = {
    "+", "-", "*", "/", "%", "**",
    "<", ">", "<=", ">=", "<=>",
    "div", "modulo", "remainder", "quo", "fdiv", "divmod", NULL};
  return str_in(op, set);
}
/* 1 if class k defines a #coerce whose SHAPE the protocol can use: one
   parameter, no rest. Nothing here consults reachability or the return type,
   because compute_reachable runs long after the type fixpoint -- a predicate
   that asked would answer 0 for every call the fixpoint makes and the rule
   would be dormant exactly where it is needed. Typing and emission both ask
   this, so they agree at every point in the pipeline; whether the coerce
   dispatch ends up carrying an arm for the class is a separate question that
   only codegen needs (class_coerce_emittable), and a class that fails it finds
   the hook unhandled and gets the TypeError CRuby raises for it anyway. */
int class_has_coerce_shape(Compiler *c, int k) {
  int mi = comp_method_in_chain(c, k, "coerce", NULL);
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  return m->nparams == 1 && m->rest_idx < 0;
}
/* 1 if class k defines a #coerce this TU emits and can call from the runtime
   hook: one parameter, no rest, an array return (the [other, self] pair).
   The coerce protocol needs it for `5 + obj` where obj only reads poly. */
int class_coerce_emittable(Compiler *c, int k) {
  int defcls = -1;
  int mi = comp_method_in_chain(c, k, "coerce", &defcls);
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->reachable || m->yields || scope_is_shadowed(c, mi) || m->is_transplanted_source)
    return 0;
  if (m->nparams != 1 || m->rest_idx >= 0) return 0;
  if (!ty_is_array(m->ret) && m->ret != TY_POLY_ARRAY) return 0;
  return 1;
}
/* 1 if class k defines a #to_str whose SHAPE a String comparison can convert
   through: no parameters, and an answer the String slot can take -- a static
   String, or one the analysis could only pin to poly (a body reading a poly
   ivar, or one with a nil branch), whose boxed answer takes the check CRuby
   applies to the RESULT of #to_str. Like the coerce shape above, this
   consults neither reachability nor emittability: it is asked by the type
   rules for #casecmp and by every comparison arm the emitter renders, so the
   TYPED side agrees at every point in the pipeline. The BOXED side does not
   ask it and cannot: it reaches #to_str through the generated conversion
   bridge, which carries only a #to_str whose static return is a String
   (conv_bridge_callee), so a poly-returning one converts typed and answers
   "no conversion" boxed. That asymmetry is deliberate -- widening the bridge
   is its own change -- and it is named again where the bridge is asked, in
   sp_poly_check_str.

   A parameter of any kind is declined even though CRuby only needs #to_str to
   be callable with none: `def to_str(x = 1)`, `def to_str(*a)` and
   `def to_str(k: 1)` all fail the C build at the tree's existing conversion
   site (`"x" + A.new("y")`, checked on all three), and this rule leaves them
   where it found them rather than adding a new way in. An
   `attr_reader :to_str` defines no method scope at all, so the lookup below
   never sees it. A native class is excluded because the conversion is a
   direct call to a method this TU compiles. */
int class_has_to_str_shape(Compiler *c, int k) {
  if (k < 0 || k >= c->nclasses || c->classes[k].is_native_class) return 0;
  int mi = comp_method_in_chain(c, k, "to_str", NULL);
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  /* an ALIAS is not this shape yet: the conversion emitter names the call
     after the protocol (sp_A_to_str) while `alias to_str raw` compiles one
     function named sp_A_raw, and reachability seeds the implicit protocols by
     the method's own name, so the aliased body is not emitted at all. Master
     fails the C build on that pair already (`"x" + A.new("y")`); this rule
     leaves it exactly where it found it rather than adding a new way in. */
  if (!sp_streq(m->name, "to_str")) return 0;
  return m->nparams == 0 && m->rest_idx < 0 &&
         (m->ret == TY_STRING || m->ret == TY_POLY);
}
int is_eq_op(const char *op) {
  static const char *const set[] = {"==", "!=", "===", NULL};
  return str_in(op, set);
}
int is_void_call(const char *name) {
  static const char *const set[] = {
    "puts", "print", "p", "pp", "require", "require_relative",
    "raise", "fail", "warn", "printf", NULL};
  return str_in(name, set);
}
/* 1 for a local-write kind whose `value` is what the local then holds (a
   plain write, or `||=`/`&&=`, which bind it or keep the old one); 0 for a
   multiple-assignment, for, rescue or pattern target and for `op=`. */
int local_write_binds_value(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
         k == NK_LocalVariableAndWriteNode;
}
/* A local variable that statically holds exactly one user class (every write
   in its scope assigns the same class constant) resolves to that class index;
   -1 when dynamic. Lets `k = Klass; k.new(...)` and `k.members` dispatch
   statically. */
int class_var_static_ci(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  NodeKind ty_k = nt_kind(nt, node);
  /* an inline `Struct.new(...)` / `Data.define(...)` receiver resolves to the
     anon class synthesized for that call node (#2682) */
  if (ty_k == NK_CallNode && is_struct_call(c, node))
    return anon_struct_ci_for_value(c, node);
  if (ty_k != NK_LocalVariableReadNode) return -1;
  const char *vn = nt_str(nt, node, "name");
  if (!vn) return -1;
  Scope *sc = comp_scope_of(c, node);
  int found = -1;
  /* the (scope, name) chain: the name-keyed one degenerates on machine-
     generated programs, where one local name recurs in thousands of scopes */
  for (int w = comp_lvw_first_sc(c, (int)(sc - c->scopes), vn); w >= 0; w = comp_lvw_next_sc(c, w)) {
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
    /* a multiple-assignment, for, rescue or pattern target, or an op-write,
       binds a value no constant names; `||=`/`&&=` bind theirs or keep one */
    if (!local_write_binds_value(nt_kind(nt, w))) return -1;
    int val = nt_ref(nt, w, "value");
    NodeKind vk = nt_kind(nt, val);
    /* a constant, or a path to one by its leaf (`k = Outer::Foo`), as
       class_recv_static_ci reads a receiver */
    int ci = (vk == NK_ConstantReadNode || vk == NK_ConstantPathNode)
             ? comp_class_index(c, nt_str(nt, val, "name")) : -1;
    /* k = Struct.new(:a, :b): resolve to the anonymous struct class that
       register_structs synthesized for this write (keyed by def_node) */
    if (ci < 0 && is_struct_call(c, val)) {
      for (int k = 0; k < c->nclasses; k++)
        if (c->classes[k].is_anon_struct && c->classes[k].def_node == w) { ci = k; break; }
    }
    if (ci < 0) return -1;                   /* a non-class write: dynamic */
    if (found >= 0 && found != ci) return -1; /* two classes: dynamic */
    found = ci;
  }
  return found;
}
/* The user class a class-valued receiver names at compile time: a constant
   (`Foo`, or the leaf of `Outer::Foo`) or a local that holds one class;
   -1 when it is only known at run time. */
int class_recv_static_ci(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if ((k == NK_ConstantReadNode || k == NK_ConstantPathNode) &&
      nt_str(nt, node, "name"))
    return comp_class_index(c, nt_str(nt, node, "name"));
  return class_var_static_ci(c, node);
}

/* The class method `name` the program defines for the class a class-valued
   receiver names at compile time (class_recv_static_ci), up its ancestors;
   -1 when there is none or the class is known only at run time: the target
   of call `call` on receiver `node` (call_target). */
int class_recv_user_cmethod(Compiler *c, int call, int node, const char *name) {
  int ci = class_recv_static_ci(c, node);
  return ci >= 0 ? call_target(c, call, ci, name, CT_CMETHOD, NULL) : -1;
}

/* The literal symbol behind a symbol-typed expression: a SymbolNode itself,
   or a local variable whose only write (in its scope, plain write) is one.
   Lets inject(:op)-style operator selection see through `s = :+; a.inject(s)`.
   Returns the symbol's name, or NULL. */
const char *sym_static_value(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return NULL;
  NodeKind ty_k = nt_kind(nt, node);
  if (ty_k == NK_SymbolNode) return nt_str(nt, node, "value");
  if (ty_k != NK_LocalVariableReadNode) return NULL;
  const char *vn = nt_str(nt, node, "name");
  if (!vn) return NULL;
  Scope *sc = comp_scope_of(c, node);
  int val = -1;
  for (int w = 0; w < nt->count; w++) {
    NodeKind k = nt_kind(nt, w);
    if (k != NK_LocalVariableWriteNode && k != NK_LocalVariableOrWriteNode &&
        k != NK_LocalVariableAndWriteNode && k != NK_LocalVariableOperatorWriteNode &&
        k != NK_LocalVariableTargetNode)
      continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    if (k != NK_LocalVariableWriteNode || comp_scope_of(c, w) != sc || val >= 0)
      return NULL;
    val = nt_ref(nt, w, "value");
  }
  if (val < 0) return NULL;
  NodeKind vk = nt_kind(nt, val);
  return (vk == NK_SymbolNode) ? nt_str(nt, val, "value") : NULL;
}

/* A CallNode chain that evaluates to a Lazy (a `.lazy` somewhere in the
   receiver spine, and a lazy-producing transform -- not a forcing terminal --
   on top). Such a value has no runtime representation in spinel; it is only
   ever fused at a forcing site. (#2932) */
/* See compiler.h for the masks. Every name carries SP_MUT_LOCAL; the
   narrower sites drop the mutators their storage shape cannot serve:
     append_as_bytes            has no guard-narrowed re-route arm
   `[]=` insert slice! setbyte reach an ivar, and a reader call handing out
   its handle, through the shared-mutable shim's shadow (#4363), and a
   guard-narrowed box through its poly arm as the others do. */
int sp_str_mutator(const char *nm, unsigned want) {
  /* This byte-mutator API excludes state-only names. */
  return bop_name_mutates(nm, want ? want : BOP_MUT_LOCAL);
}
/* A String call whose value is its receiver, whatever it did to it first:
   to_s, to_str, itself and freeze, and the mutators that answer self --
   prepend, insert, replace, clear, reverse!, force_encoding, encode!. (`<<`
   and concat, which every String alias walk already takes, are left to
   those walks.) A block would make the name some other method's. Whether
   the receiver IS a String is the caller's question: to_s and itself are
   every class's names. */
int str_self_call(const NodeTable *nt, int id) {
  if (id < 0 || nt_kind(nt, id) != NK_CallNode) return 0;
  if (nt_ref(nt, id, "receiver") < 0 || nt_ref(nt, id, "block") >= 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return 0;
  int a = nt_ref(nt, id, "arguments");
  int ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  if (nt_argv_has(nt, av, ac, NT_ARG_SPLAT | NT_ARG_BLOCK | NT_ARG_KEYWORDS)) return 0;
  if (ac == 0 && (sp_streq(nm, "to_s") || sp_streq(nm, "to_str") || sp_streq(nm, "itself") ||
                  sp_streq(nm, "freeze") || sp_streq(nm, "clear") || sp_streq(nm, "reverse!")))
    return 1;
  if (ac == 1 && (sp_streq(nm, "replace") || sp_streq(nm, "force_encoding") ||
                  sp_streq(nm, "encode!") || sp_streq(nm, "prepend")))
    return 1;
  return ac == 2 && sp_streq(nm, "insert");
}
int fiber_storage_recv(const NodeTable *nt, int recv) {
  NodeKind rk = nt_kind(nt, recv);
  const char *rn = nt_str(nt, recv, "name");
  if (rk == NK_ConstantReadNode) return rn && sp_streq(rn, "Fiber");
  if (rk != NK_CallNode) return 0;
  int rr = nt_ref(nt, recv, "receiver");
  if (!rn || !sp_streq(rn, "current") || rr < 0) return 0;
  NodeKind rrk = nt_kind(nt, rr);
  const char *rrn = nt_str(nt, rr, "name");
  return rrk == NK_ConstantReadNode && rrn && sp_streq(rrn, "Fiber");
}
/* A receiver-mutating Array method. */
int array_mutator_name(const char *nm) {
  size_t l = nm ? strlen(nm) : 0;
  if (!l) return 0;
  static const char *const M[] = {
    "<<", "push", "append", "unshift", "prepend", "insert", "concat", "fill",
    "clear", "delete", "delete_at", "pop", "shift", "replace", "[]=", "keep_if",
    "delete_if", "slice!", NULL };
  for (int i = 0; M[i]; i++) if (sp_streq(nm, M[i])) return 1;
  return nm[l - 1] == '!';
}

int lazy_stage_name(const char *nm) {
  static const char *const ST[] = {
    /* re-lazy on an already-lazy chain is transparent */
    "lazy",
    /* block stages (LAZY_BLOCK_STAGE in codegen_call.c) */
    "map", "collect", "select", "filter", "find_all", "reject",
    "take_while", "drop_while", "filter_map", "flat_map", "collect_concat",
    /* blockless counter / grouping stages (LAZY_COUNTER_STAGE) */
    "take", "drop", "each_slice", "each_cons", NULL };
  if (!nm) return 0;
  return str_in(nm, ST);
}

static int chain_is_lazy_valued_1(Compiler *c, int node);
/* lazy_alias_chain resolves an alias by calling back in here, so the hop bound
   inside has to span the RECURSION and not just one call's own walk: on a
   self-referential write (`s = s.take(n)`) the two alternate, every new frame
   started the counter again, and the compiler ran out of stack. #3324 bounded
   the walk; this bounds the mutual recursion (#3929). */
int chain_is_lazy_valued(Compiler *c, int node) {
  static int depth = 0;
  int r;
  if (depth > 8) return 0;
  depth++;
  r = chain_is_lazy_valued_1(c, node);
  depth--;
  return r;
}
static int chain_is_lazy_valued_1(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *top = nt_str(nt, node, "name");
  if (!top) return 0;
  /* with_index / zip keep the VALUE lazy (so a chain ending in them is still
     a Lazy for recognition purposes) but the pipeline cannot fuse them, so
     they are not stages -- the write-suppression walk must not accept them. */
  if (!lazy_stage_name(top) && !sp_streq(top, "with_index") &&
      !sp_streq(top, "each_with_index") && !sp_streq(top, "zip"))
    return 0;
  int hops = 0;
  for (int cur = node; cur >= 0; ) {
    /* part of the chain may be held in a variable
       (`s = src.lazy.select{}; s.each_cons(2).lazy...`): resolve the
       single-plain-write alias and keep walking. Bounded: a
       self-referential write (`s = s.map{}`) would otherwise recurse
       through lazy_alias_chain forever (#3324). */
    if (nt_kind(nt, cur) == NK_LocalVariableReadNode) {
      if (++hops > 8) return 0;
      int a = lazy_alias_chain(c, cur);
      if (a < 0) return 0;
      cur = a;
      continue;
    }
    if (nt_kind(nt, cur) != NK_CallNode) return 0;
    const char *nm = nt_str(nt, cur, "name");
    if (nm && sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) return 1;
    cur = nt_ref(nt, cur, "receiver");
  }
  return 0;
}

/* If VAR (a LocalVariableReadNode) is a single-plain-write alias for a lazy
   chain in the same scope, return the chain node so a forcing call on the
   variable can fuse it as if written inline; else -1. Models sym_static_value.
   (#2932) */
int lazy_alias_chain(Compiler *c, int var_read) {
  const NodeTable *nt = c->nt;
  if (var_read < 0 || nt_kind(nt, var_read) != NK_LocalVariableReadNode) return -1;
  const char *vn = nt_str(nt, var_read, "name");
  if (!vn) return -1;
  Scope *sc = comp_scope_of(c, var_read);
  /* The plain writes first, through the name index: the answer is -1 unless
     there is exactly one, in this scope, whose value is a lazy chain. The
     query runs per `.first`/`.to_a`/`.force` on a local on every fixpoint
     iteration, and walked every write of five kinds each time; a local not
     holding a lazy chain now stops here. */
  int val = -1, nplain = 0;
  for (int w = an_local_writes_named_first(c, vn); w >= 0; w = an_local_writes_named_next(w)) {
    const char *wn = nt_str(nt, w, "name");
    /* (a local of another method's of the same name is another local) */
    if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
    if (++nplain > 1) return -1;   /* not a single plain write */
    val = nt_ref(nt, w, "value");
  }
  if (val < 0 || !chain_is_lazy_valued(c, val)) return -1;
  /* ... and no write of another kind: an `||=`, `&&=`, `op=` or a
     multiple-assignment target makes it not a single plain write */
  static const NodeKind OTHER_WRITE_KINDS[] = {
    NK_LocalVariableOrWriteNode, NK_LocalVariableAndWriteNode,
    NK_LocalVariableOperatorWriteNode, NK_LocalVariableTargetNode };
  for (size_t ki = 0; ki < sizeof OTHER_WRITE_KINDS / sizeof *OTHER_WRITE_KINDS; ki++)
    NT_FOREACH_KIND(nt, OTHER_WRITE_KINDS[ki], w) {
      const char *wn = nt_str(nt, w, "name");
      if (wn && sp_streq(wn, vn) && comp_scope_of(c, w) == sc) return -1;
    }
  return val;
}

/* A bare call to a parameterless user method whose whole body is a lazy chain
   resolves to that chain, so `def lz; [1,2,3].lazy; end; lz.first` fuses the
   same way the inline form does. A lazy value has no runtime representation to
   return, so without this the method body is simply not emittable (#3358).
   Restricted to a self-independent chain -- no self, ivar, or local read --
   because the chain is re-emitted at the CALL SITE, where the callee's
   receiver and locals are not in scope. */
static int lazy_chain_self_free(Compiler *c, int node, int depth) {
  const NodeTable *nt = c->nt;
  if (node < 0 || depth > 32) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_SelfNode || k == NK_InstanceVariableReadNode ||
      k == NK_LocalVariableReadNode || k == NK_ClassVariableReadNode)
    return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    /* a block body binds its own params, so its reads are its own */
    if (ch >= 0 && nt_kind(nt, ch) == NK_BlockNode) continue;
    if (ch >= 0 && !lazy_chain_self_free(c, ch, depth + 1)) return 0;
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++)
      if (ids[j] >= 0 && nt_kind(nt, ids[j]) != NK_BlockNode &&
          !lazy_chain_self_free(c, ids[j], depth + 1)) return 0;
  }
  return 1;
}
int lazy_method_chain(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  if (call < 0 || nt_kind(nt, call) != NK_CallNode) return -1;
  if (nt_ref(nt, call, "receiver") >= 0 || nt_ref(nt, call, "block") >= 0) return -1;
  { int ar = nt_ref(nt, call, "arguments"); int ac = 0;
    if (ar >= 0) nt_arr(nt, ar, "arguments", &ac);
    if (ac != 0) return -1; }
  const char *nm = nt_str(nt, call, "name");
  if (!nm) return -1;
  int m = call_target(c, call, -1, nm, CT_TOP, NULL);
  if (m < 0 || c->scopes[m].nparams != 0) return -1;
  int body = c->scopes[m].body;
  if (body < 0) return -1;
  int n = 0; const int *st = nt_arr(nt, body, "body", &n);
  if (n != 1 || !st) return -1;
  if (!chain_is_lazy_valued(c, st[0])) return -1;
  return lazy_chain_self_free(c, st[0], 0) ? st[0] : -1;
}
int lazy_resolve_chain(Compiler *c, int n) {
  int a = lazy_alias_chain(c, n);
  if (a < 0) a = lazy_method_chain(c, n);
  return a >= 0 ? a : n;
}

/* The anonymous struct class synthesized for a `k = Struct.new(:a, :b)`
   VALUE node (the write is the class's def_node), or -1. Lets the value
   type as TY_CLASS and emit as the class object. */
/* The first member name that appears more than once in a Struct.new(...) /
   Data.define(...) call's argument list, or NULL. CRuby raises ArgumentError
   ("duplicate member") for such a definition (#2705). */
const char *struct_call_dup_member(Compiler *c, int callnode) {
  const NodeTable *nt = c->nt;
  if (callnode < 0) return NULL;
  int args = nt_ref(nt, callnode, "arguments");
  int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (!argv) return NULL;
  for (int a = 0; a < an; a++) {
    if (nt_kind(nt, argv[a]) != NK_SymbolNode) continue;
    const char *m = nt_str(nt, argv[a], "value");
    if (!m) continue;
    for (int b = 0; b < a; b++) {
      if (nt_kind(nt, argv[b]) != NK_SymbolNode) continue;
      const char *m2 = nt_str(nt, argv[b], "value");
      if (m2 && sp_streq(m2, m)) return m;
    }
  }
  return NULL;
}

int anon_struct_ci_for_value(Compiler *c, int val) {
  const NodeTable *nt = c->nt;
  if (val < 0) return -1;
  /* Anonymous structs number a handful at most while c->classes can hold
     hundreds, and this runs per candidate receiver: collect the anon subset
     once instead of rescanning every class on each call. */
  if (!c->anon_struct_ids_valid) {
    int *ids = malloc((size_t)(c->nclasses > 0 ? c->nclasses : 1) * sizeof(int));
    if (ids) {
      free(c->anon_struct_ids);
      c->anon_struct_ids = ids;
      c->n_anon_struct_ids = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (c->classes[k].is_anon_struct) c->anon_struct_ids[c->n_anon_struct_ids++] = k;
      c->anon_struct_ids_valid = 1;
    }
    else {
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].is_anon_struct) continue;
        int w = c->classes[k].def_node;
        if (w >= 0 && (nt_ref(nt, w, "value") == val || w == val)) return k;
      }
      return -1;
    }
  }
  for (int q = 0; q < c->n_anon_struct_ids; q++) {
    int k = c->anon_struct_ids[q];
    int w = c->classes[k].def_node;
    /* keyed by a write node (k = Struct.new -> def_node's value is the call),
       or, for an inline `Data.define(...).method(...)` receiver, keyed by the
       struct/data call node itself (def_node IS that call). #2682 */
    if (w >= 0 && (nt_ref(nt, w, "value") == val || w == val)) return k;
  }
  return -1;
}

/* `Hash.new(d)` (or the desugared __hash_new_default) as a bare expression:
   return the default-value argument node, or -1. Lets a literal receiver's
   .default / [] / fetch fold to the default when the hash type never
   narrows (no writes ever reach it). */
static int hash_new_default_arg_compute(Compiler *c, int recv);

/* hash_new_default_arg's result is a pure function of the (static) AST node
   table plus scope assignments, which are fixed while the scope-index is
   frozen. The local-variable branch scans every write node in the program, and
   the function is called for every `x[k]` / `x.default` on an untyped receiver
   during the inference fixpoint -- O(N) per call, O(N^2) overall. Memoize per
   receiver node while frozen, discarding the cache when the scope epoch changes
   (see comp_scope_index_gen); fall back to a direct compute while unfrozen. */
int hash_new_default_arg(Compiler *c, int recv) {
  if (recv < 0) return -1;
  if (!comp_scope_index_is_frozen() || recv >= c->node_cap)
    return hash_new_default_arg_compute(c, recv);
  unsigned gen = comp_scope_index_gen();
  if (!c->hash_default_arg_memo || c->hash_default_arg_memo_cap < c->node_cap) {
    free(c->hash_default_arg_memo);
    c->hash_default_arg_memo = malloc((size_t)c->node_cap * sizeof(int));
    c->hash_default_arg_memo_cap = c->hash_default_arg_memo ? c->node_cap : 0;
    c->hash_default_arg_memo_gen = gen - 1u; /* force the re-init below */
  }
  if (!c->hash_default_arg_memo) return hash_new_default_arg_compute(c, recv);
  if (c->hash_default_arg_memo_gen != gen) {
    for (int i = 0; i < c->hash_default_arg_memo_cap; i++) c->hash_default_arg_memo[i] = INT_MIN;
    c->hash_default_arg_memo_gen = gen;
  }
  if (c->hash_default_arg_memo[recv] != INT_MIN) return c->hash_default_arg_memo[recv];
  /* Mark the slot resolved-but-dynamic (-1) before recomputing: the compute
     recurses through a local's writes, which can re-enter for this same node
     via a write cycle (`a = b; b = a`). -1 is the correct conservative result
     for such a cyclic (non-single-Hash.new) definition and breaks the cycle;
     the real result overwrites it below. */
  c->hash_default_arg_memo[recv] = -1;
  int r = hash_new_default_arg_compute(c, recv);
  c->hash_default_arg_memo[recv] = r;
  return r;
}

static int hash_new_default_arg_compute(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || !nt_type(nt, recv)) return -1;
  /* a local whose every same-scope write is the same Hash.new(d) shape
     resolves like the literal (`a = Hash.new(7); a.default`) */
  if (nt_kind(nt, recv) == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, recv, "name");
    if (!vn) return -1;
    Scope *sc = comp_scope_of(c, recv);
    if (!sc) return -1;
    int found = -1;
    for (int r = lw_shared_first(c, vn, (int)(sc - c->scopes)); r >= 0;
         r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      int dn = val >= 0 ? hash_new_default_arg(c, val) : -1;
      if (dn < 0) return -1;                 /* a non-matching write: dynamic */
      if (found >= 0) return -1;             /* multiple writes: dynamic */
      found = dn;
    }
    return found;
  }
  if (nt_kind(nt, recv) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm || (!sp_streq(nm, "new") && !sp_streq(nm, "__hash_new_default"))) return -1;
  int cr = nt_ref(nt, recv, "receiver");
  if (cr < 0 || nt_kind(nt, cr) != NK_ConstantReadNode) return -1;
  const char *cn = nt_str(nt, cr, "name");
  if (!cn || !sp_streq(cn, "Hash")) return -1;
  if (nt_ref(nt, recv, "block") >= 0) return -1;   /* block default: dproc path */
  int a = nt_ref(nt, recv, "arguments");
  int ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  if (ac != 1 || !av) return -1;
  NodeKind ak = nt_kind(nt, av[0]);
  if (ak == NK_KeywordHashNode) return -1;
  return av[0];
}

/* The default-value node of the single `Hash.new(d)` that a local or an ivar
   receiver is bound to, or -1. The local half is hash_new_default_arg's own;
   the ivar half walks the class's writes of that name the way the codegen's
   ivar_write_slot_ty does, and answers only when exactly one write exists and
   it is that shape -- two writes, or one of another shape, are dynamic. */
int recv_hash_new_default_arg(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_LocalVariableReadNode) return hash_new_default_arg(c, recv);
  if (rk != NK_InstanceVariableReadNode) return -1;
  const char *ivn = nt_str(nt, recv, "name");
  Scope *rs = comp_scope_of(c, recv);
  if (!ivn || !rs || rs->class_id < 0) return -1;
  int found = -1;
  /* the writes of this ivar name only: a key site asks this per round, and a
     walk of every ivar write in the program per ask was quadratic. The answer
     does not depend on the order (one Hash.new write, or nothing). */
  for (int r = ivw_shared_first(c, ivn); r >= 0; r = ivw_shared_next(r)) {
    int w = ivw_shared_node(r);
    if (nt_kind(nt, w) != NK_InstanceVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, ivn)) continue;
    Scope *ws = comp_scope_of(c, w);
    if (!ws || ws->class_id != rs->class_id) continue;
    int dn = hash_new_default_arg(c, nt_ref(nt, w, "value"));
    if (dn < 0) return -1;
    if (found >= 0) return -1;
    found = dn;
  }
  return found;
}

/* The value type a `Hash.new(d)` default contributes to its hash's value
   type. An empty `[]` or `{}` default infers no element type of its own, but
   it is still a non-scalar value the hash answers on a miss, so it widens the
   value to poly rather than vanishing into the unify -- the rule the nested
   hash-literal typing already applies to an unresolved element (#4000). */
TyKind hash_default_value_ty(Compiler *c, int dn) {
  if (dn < 0) return TY_UNKNOWN;
  TyKind t = infer_type(c, dn);
  /* (one-String mode: a default held as its handle is a String to the
     Hash's value kind, which holds the handle) */
  if (t == TY_STRBUF) return TY_STRING;
  if (t != TY_UNKNOWN) return t;
  NodeKind k = nt_kind(c->nt, dn);
  if (k == NK_ArrayNode || k == NK_HashNode || k == NK_KeywordHashNode) return TY_POLY;
  return TY_UNKNOWN;
}

/* Is CONSTNAME written directly in the body of `class CLSNAME`? Constants live
   in one flat namespace, so the AST is what says which class owns one -- what
   const_defined?/const_get need for an `inherit: false` search (#3762).
   Answers the name the write carries: CONSTNAME itself, or the qualified
   `Mod__..__CONSTNAME` qualify_colliding_consts gave a constant whose leaf
   another constant or class shares. */
static const char *cn_body_const_key(const NodeTable *nt, int root, const char *constname, int top) {
  if (root < 0) return NULL;
  const char *ty = nt_type(nt, root);
  NodeKind ty_k = nt_kind(nt, root);
  if (ty) {
    if (!top && (ty_k == NK_ClassNode || ty_k == NK_ModuleNode)) return NULL;
    if (ty_k == NK_ConstantWriteNode) {
      const char *n = nt_str(nt, root, "name");
      size_t nl = n ? strlen(n) : 0, cl = strlen(constname);
      if (n && (sp_streq(n, constname) ||
                (nl > cl + 2 && !strncmp(n + nl - cl - 2, "__", 2) && sp_streq(n + nl - cl, constname) &&
                 qc_const_qualified(n))))
        return n;
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) {
    const char *k = cn_body_const_key(nt, nt_ref_at(nt, root, i), constname, 0);
    if (k) return k;
  }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) {
      const char *k = cn_body_const_key(nt, el[j], constname, 0);
      if (k) return k;
    }
  }
  return NULL;
}
/* The ClassNodes and ModuleNodes of each name, ascending, and the body
   answers const_value_owned_key gave per (class, constant) pair, kept while
   the node table is unchanged (no node added, no field rewritten). A
   runtime-name const_get is one literal arm per constant, and an arm on a
   receiver known only at run time asks every class and each of its
   ancestors: each ask walked the whole table. */
static ANameHash g_cvo_names;          /* class or module name -> chain */
static int *g_cvo_first, *g_cvo_next, *g_cvo_ids;
static ANameHash g_cvo_memo;           /* "class\001constant" -> answer */
static const char **g_cvo_ans;
static const NodeTable *g_cvo_nt;
static unsigned g_cvo_ver;
static unsigned long g_cvo_edits;
static int g_cvo_cnt = -1;
static void cvo_reset(void) {
  anh_free(&g_cvo_names); memset(&g_cvo_names, 0, sizeof g_cvo_names);
  for (int i = 0; i < g_cvo_memo.n; i++) free((char *)g_cvo_memo.key[i]);
  anh_free(&g_cvo_memo); memset(&g_cvo_memo, 0, sizeof g_cvo_memo);
  free(g_cvo_first); free(g_cvo_next); free(g_cvo_ids); free(g_cvo_ans);
  g_cvo_first = g_cvo_next = g_cvo_ids = NULL; g_cvo_ans = NULL;
}
static void cvo_build(const NodeTable *nt) {
  cvo_reset();
  int ncl = 0, nmo = 0;
  const int *cl = nt_nodes_of_kind(nt, NK_ClassNode, &ncl);
  const int *mo = nt_nodes_of_kind(nt, NK_ModuleNode, &nmo);
  size_t n = (size_t)(ncl + nmo > 0 ? ncl + nmo : 1);
  g_cvo_ids = malloc(sizeof(int) * n);
  g_cvo_next = malloc(sizeof(int) * n);
  g_cvo_first = malloc(sizeof(int) * n);
  int *last = malloc(sizeof(int) * n);
  if (!g_cvo_ids || !g_cvo_next || !g_cvo_first || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* both kinds merged in id order, the order a walk of the table meets them */
  for (int i = 0, j = 0, p = 0; p < ncl + nmo; p++) {
    int id = j >= nmo || (i < ncl && cl[i] < mo[j]) ? cl[i++] : mo[j++];
    g_cvo_ids[p] = id; g_cvo_next[p] = -1;
    /* a ClassNode carries its name through constant_path, not a name field */
    int cp = nt_ref(nt, id, "constant_path");
    const char *nm = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, id, "name");
    if (!nm) continue;
    int k = anh_find(&g_cvo_names, nm);
    if (k < 0) { anh_add(&g_cvo_names, nm); k = g_cvo_names.n - 1; g_cvo_first[k] = p; }
    else g_cvo_next[last[k]] = p;
    last[k] = p;
  }
  free(last);
  g_cvo_nt = nt; g_cvo_ver = nt->version; g_cvo_edits = nt->edits; g_cvo_cnt = nt->count;
}
/* The key of the first value constant CONSTNAME written directly in the body
   of a `class`/`module CLSNAME`, or NULL. */
static const char *cvo_body_key(const NodeTable *nt, const char *clsname, const char *constname) {
  if (g_cvo_nt != nt || g_cvo_ver != nt->version || g_cvo_edits != nt->edits || g_cvo_cnt != nt->count)
    cvo_build(nt);
  size_t cl = strlen(clsname), kl = strlen(constname);
  char *mk = malloc(cl + kl + 2);
  if (!mk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(mk, clsname, cl); mk[cl] = '\001'; memcpy(mk + cl + 1, constname, kl + 1);
  int m = anh_find(&g_cvo_memo, mk);
  if (m >= 0) { free(mk); return g_cvo_ans[m]; }
  const char *key = NULL;
  int k = anh_find(&g_cvo_names, clsname);
  for (int p = k >= 0 ? g_cvo_first[k] : -1; p >= 0 && !key; p = g_cvo_next[p]) {
    int body = nt_ref(nt, g_cvo_ids[p], "body");
    key = body >= 0 ? cn_body_const_key(nt, body, constname, 1) : NULL;
  }
  anh_add(&g_cvo_memo, mk);
  g_cvo_ans = realloc(g_cvo_ans, sizeof(char *) * (size_t)g_cvo_memo.cap);
  if (!g_cvo_ans) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  g_cvo_ans[g_cvo_memo.n - 1] = key;
  return key;
}
/* The key of value constant CONSTNAME written directly in the body of class
   or module CLSNAME, or NULL when it holds none. */
static const char *const_value_owned_key(Compiler *c, const char *clsname, const char *constname) {
  const NodeTable *nt = c->nt;
  const char *key = cvo_body_key(nt, clsname, constname);
  if (key) return key;
  /* ... or a path write through it (`Base::N = v`, or through an alias of
     it), under the key the constant qualifier gave it. The qualifier
     resolved the whole path (qc_owner): `Left::Base` is not a top-level
     Base. */
  const char *rn = class_ruby_name(c, comp_class_index(c, clsname));
  if (!rn) return NULL;
  static const NodeKind pw[] = { NK_ConstantPathWriteNode, NK_ConstantPathOrWriteNode,
                                 NK_ConstantPathAndWriteNode, NK_ConstantPathOperatorWriteNode };
  for (int i = 0; i < 4; i++)
    NT_FOREACH_KIND(nt, pw[i], w) {
      int t = nt_ref(nt, w, "target");
      const char *on = t >= 0 ? nt_str(nt, t, "qc_orig_name") : NULL;
      if (!on) on = t >= 0 ? nt_str(nt, t, "name") : NULL;
      const char *own = t >= 0 ? nt_str(nt, t, "qc_owner") : NULL;
      if (on && own && sp_streq(on, constname) && sp_streq(own, rn)) return nt_str(nt, t, "name");
    }
  return NULL;
}
static int const_value_owned(Compiler *c, const char *clsname, const char *constname) {
  return const_value_owned_key(c, clsname, constname) != NULL;
}
/* The classes of each name's leaf, ascending, while a caller asks const_get
   of every class in turn (const_get_leaves_begin .. _end): the class table
   does not change in that stretch, and each ask walked every class once per
   ancestor. Outside it the walks below run as they are. */
static ANameHash g_cgl_leaves;
static int *g_cgl_first, *g_cgl_next;
static const Compiler *g_cgl_c;
void const_get_leaves_begin(Compiler *c) {
  const_get_leaves_end();
  size_t n = (size_t)(c->nclasses > 0 ? c->nclasses : 1);
  g_cgl_first = malloc(sizeof(int) * n);
  g_cgl_next = malloc(sizeof(int) * n);
  int *last = malloc(sizeof(int) * n);
  if (!g_cgl_first || !g_cgl_next || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < c->nclasses; k++) {
    g_cgl_next[k] = -1;
    const char *rn = class_ruby_name(c, k), *l = rn ? strrchr(rn, ':') : NULL;
    if (!rn) continue;
    const char *leaf = l ? l + 1 : rn;
    int h = anh_find(&g_cgl_leaves, leaf);
    if (h < 0) { anh_add(&g_cgl_leaves, leaf); h = g_cgl_leaves.n - 1; g_cgl_first[h] = k; }
    else g_cgl_next[last[h]] = k;
    last[h] = k;
  }
  free(last);
  g_cgl_c = c;
}
void const_get_leaves_end(void) {
  anh_free(&g_cgl_leaves); memset(&g_cgl_leaves, 0, sizeof g_cgl_leaves);
  free(g_cgl_first); free(g_cgl_next);
  g_cgl_first = g_cgl_next = NULL;
  g_cgl_c = NULL;
}
/* The first class of leaf LEAF, -1 for none, while the index is up. */
static int cgl_first(const char *leaf) {
  int h = anh_find(&g_cgl_leaves, leaf);
  return h >= 0 ? g_cgl_first[h] : -1;
}
/* The class nested directly in class or module O under the leaf LEAF (its
   key may be qualified, `O__LEAF`, when another constant or class shares the
   leaf), or -1. */
static int class_nested_leaf(Compiler *c, int o, const char *leaf) {
  if (o < 0) return -1;
  if (g_cgl_c == c) {
    for (int k = cgl_first(leaf); k >= 0; k = g_cgl_next[k])
      if (c->classes[k].enclosing_class == o) return k;
    return -1;
  }
  for (int k = 0; k < c->nclasses; k++) {
    if (c->classes[k].enclosing_class != o) continue;
    const char *rn = class_ruby_name(c, k), *l = rn ? strrchr(rn, ':') : NULL;
    if (rn && sp_streq(l ? l + 1 : rn, leaf)) return k;
  }
  return -1;
}
/* Is any class or module of the program named LEAF, at any nesting? */
static int class_leaf_any(Compiler *c, const char *leaf) {
  if (g_cgl_c == c) return cgl_first(leaf) >= 0;
  for (int k = 0; k < c->nclasses; k++) {
    const char *rn = class_ruby_name(c, k), *l = rn ? strrchr(rn, ':') : NULL;
    if (rn && sp_streq(l ? l + 1 : rn, leaf)) return 1;
  }
  return 0;
}
/* Does class or module CLSNAME hold constant CONSTNAME itself -- a value
   written in its body, or a class or module nested in it? */
int const_owned_by_class(Compiler *c, const char *clsname, const char *constname) {
  if (!clsname || !constname) return 0;
  if (sp_streq(clsname, "Object") && const_name_resolves_top_level(c, constname)) return 1;
  if (const_value_owned(c, clsname, constname)) return 1;
  return class_nested_leaf(c, comp_class_index(c, clsname), constname) >= 0;
}
/* The module a `recv.const_get` searches, by name: a constant receiver's, or
   for `self` the class or module whose method or body the call is in. */
const char *const_get_recv_name(Compiler *c, int call, int recv) {
  const NodeTable *nt = c->nt;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return nt_str(nt, recv, "name");
  if (rk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, call);
    if (s && s->class_id >= 0 && s->class_id < c->nclasses) return c->classes[s->class_id].name;
  }
  return NULL;
}
/* What `R.const_get(:N)` reads. Like Ruby, R first and then each superclass,
   taking at each the value constant or the class it holds under N; past them
   the top-level constant or the class keyed N. Answers the value's key (N, or
   the qualified name R's own carries) or NULL with *cls the class, -1 when
   there is neither. */
static const char *const_get_resolve(Compiler *c, const char *rnm, const char *cgn, int *cls) {
  *cls = -1;
  /* A qualified path ("A::B", "::A::B") is read segment by segment: each
     but the last names a class or module, which the next is looked up in,
     a leading `::` from the top level; a segment that is none is the
     NameError of the whole path. */
  if (cgn && strstr(cgn, "::")) {
    const char *p = cgn, *cur = rnm;
    if (p[0] == ':' && p[1] == ':') { p += 2; cur = NULL; }
    for (const char *sep; (sep = strstr(p, "::")); p = sep + 2) {
      char seg[256];
      size_t l = (size_t)(sep - p);
      if (l == 0 || l >= sizeof seg) return NULL;
      memcpy(seg, p, l); seg[l] = '\0';
      int k = -1;
      if (const_get_resolve(c, cur, seg, &k) || k < 0) return NULL;
      cur = c->classes[k].name;
    }
    return *p ? const_get_resolve(c, cur, p, cls) : NULL;
  }
  LocalVar *cv = comp_const(c, cgn);
  /* the common case, one constant of that leaf and no class: no body scan */
  if (cv && cv->type != TY_UNKNOWN && !qc_leaf_qualified(cgn) && !class_leaf_any(c, cgn)) return cgn;
  int rk = rnm ? comp_class_index(c, rnm) : -1;
  for (int k = rk, n = 0; k >= 0 && n < 64; k = c->classes[k].parent, n++) {
    const char *own = c->classes[k].name ? const_value_owned_key(c, c->classes[k].name, cgn) : NULL;
    if (own) {
      LocalVar *ov = comp_const(c, own);
      if (ov && ov->type != TY_UNKNOWN) return own;
    }
    int nk = class_nested_leaf(c, k, cgn);
    if (nk >= 0) { *cls = nk; return NULL; }
  }
  /* a receiver the class table lacks (a builtin module): its body still counts */
  if (rk < 0 && rnm) {
    const char *own = const_value_owned_key(c, rnm, cgn);
    LocalVar *ov = own ? comp_const(c, own) : NULL;
    if (ov && ov->type != TY_UNKNOWN) return own;
  }
  *cls = comp_class_index(c, cgn);
  if (!cv || cv->type == TY_UNKNOWN) return NULL;
  return *cls < 0 ? cgn : NULL;
}
/* Does the program know constant N, for `R.const_defined?(:N)` (R's name
   or NULL): a constant or class keyed N, a wellknown one, or the class R's
   const_get finds under a qualified key. */
int const_defined_known(Compiler *c, const char *rnm, const char *cgn) {
  if (comp_const(c, cgn) || comp_class_index(c, cgn) >= 0 || comp_is_wellknown_const(cgn)) return 1;
  return rnm && const_get_class_index(c, rnm, cgn) >= 0;
}
/* Does class or module K see constant CGN as `K.const_defined?(CGN)` looks:
   K, its ancestors and the modules they include. (A top-level constant,
   Object's, every class sees: the caller asks that first.) */
int const_seen_by_class(Compiler *c, int k, const char *cgn) {
  static int depth = 0;
  if (depth > 8) return 0;
  for (int n = 0; k >= 0 && k < c->nclasses && n < 64; k = c->classes[k].parent, n++) {
    if (c->classes[k].name && const_owned_by_class(c, c->classes[k].name, cgn)) return 1;
    for (int i = 0; i < c->classes[k].nincluded_mods; i++) {
      depth++;
      int seen = const_seen_by_class(c, c->classes[k].included_mods[i], cgn);
      depth--;
      if (seen) return 1;
    }
  }
  return 0;
}
/* `R.const_defined?(:N)` with R's name: N known to the program and seen
   from R. The table is flat, keyed by the leaf, so a constant another class
   holds answered for every receiver (#9876). A receiver the class table
   lacks (a builtin module), a qualified N and a top-level N keep the
   answer the flat table gives. */
int const_defined_from(Compiler *c, const char *rnm, const char *cgn) {
  if (!const_defined_known(c, rnm, cgn)) return 0;
  if (!rnm || !cgn || strstr(cgn, "::") || const_name_resolves_top_level(c, cgn)) return 1;
  int rk = comp_class_index(c, rnm);
  if (rk < 0) return 1;
  return const_seen_by_class(c, rk, cgn);
}
/* `R.const_get(:N)` with N both a class and a value constant of the program
   (constants live in one flat namespace, keyed by the leaf): the value when
   R or a superclass holds it first, else the class. */
int const_get_takes_value(Compiler *c, const char *rnm, const char *cgn) {
  return const_get_value_key(c, rnm, cgn) != NULL;
}
/* ... and the key of the value constant it reads then, or NULL when it
   answers the class. */
const char *const_get_value_key(Compiler *c, const char *rnm, const char *cgn) {
  int cls;
  return const_get_resolve(c, rnm, cgn, &cls);
}
/* The class `R.const_get(:N)` answers, -1 when there is none. */
int const_get_class_index(Compiler *c, const char *rnm, const char *cgn) {
  int cls;
  const_get_resolve(c, rnm, cgn, &cls);
  return cls;
}
/* Both at once: the value's key, or NULL with *cls the class (-1 for none). */
const char *const_get_lookup(Compiler *c, const char *rnm, const char *cgn, int *cls) {
  return const_get_resolve(c, rnm, cgn, cls);
}

/* Does this receiver denote a Hash built by a blockless `Hash.new` (or an
   empty Hash literal)? Such a hash carries no default block. Traces a local's
   writes the way hash_new_default_arg does (#3568). */
int hash_new_blockless(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || !nt_type(nt, recv)) return 0;
  NodeKind k = nt_kind(nt, recv);
  if (k == NK_HashNode || k == NK_KeywordHashNode) return 1;
  if (k == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = vn ? comp_scope_of(c, recv) : NULL;
    if (!sc) return 0;
    int found = 0;
    for (int r = lw_shared_first(c, vn, (int)(sc - c->scopes)); r >= 0; r = lw_shared_next(r)) {
      int w = lw_shared_node(r);
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || !hash_new_blockless(c, val)) return 0;
      found = 1;
    }
    return found;
  }
  if (k != NK_CallNode) return 0;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm || !sp_streq(nm, "new")) return 0;
  int cr = nt_ref(nt, recv, "receiver");
  if (cr < 0 || nt_kind(nt, cr) != NK_ConstantReadNode) return 0;
  const char *cn = nt_str(nt, cr, "name");
  if (!cn || !sp_streq(cn, "Hash")) return 0;
  return nt_ref(nt, recv, "block") < 0;
}

int struct_member_idx(Compiler *c, ClassInfo *sc, int keynode) {
  const NodeTable *nt = c->nt;
  const char *kty = nt_type(nt, keynode);
  NodeKind kk = nt_kind(nt, keynode);
  if (!kty) return -1;
  if (kk == NK_SymbolNode || kk == NK_StringNode) {
    const char *kn = kk == NK_SymbolNode ? nt_str(nt, keynode, "value")
                                                 : nt_str(nt, keynode, "content");
    if (!kn) return -1;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kn);
    int iv = comp_member_index(sc, ivn);
    return iv;  /* ivar order == member order */
  }
  if (kk == NK_IntegerNode) {
    int idx = (int)nt_int(nt, keynode, "value", INT_MIN);
    if (idx < 0) idx += sc->nmembers;   /* negative counts from the last member */
    if (idx >= 0 && idx < sc->nmembers) return idx;
  }
  return -1;
}
/* The member a Float literal offset names: it is cut to the Integer it
   converts to, as CRuby's does. -1 for anything else. Only the `[]=` rewrite
   asks, so that the member's type unifies with the stored value. */
int struct_member_idx_float(Compiler *c, ClassInfo *sc, int keynode) {
  const NodeTable *nt = c->nt;
  NodeKind kk = nt_kind(nt, keynode);
  if (kk != NK_FloatNode) return -1;
  const char *txt = nt_content(nt, keynode);
  double fv = txt ? strtod(txt, NULL) : 1e18;
  if (!(fv > -1e9 && fv < 1e9)) return -1;
  int idx = (int)fv;
  if (idx < 0) idx += sc->nmembers;
  return idx >= 0 && idx < sc->nmembers ? idx : -1;
}
int scope_body_last(Compiler *c, int mi) {
  int body = c->scopes[mi].body;
  if (body < 0 || nt_kind(c->nt, body) != NK_StatementsNode) return -1;
  int n = 0; const int *bb = nt_arr(c->nt, body, "body", &n);
  return n > 0 ? bb[n - 1] : -1;
}
/* The node whose value a scope's body answers: its last statement, or the
   begin that is the body of a def with a rescue of its own; -1 if none. */
int scope_body_value(Compiler *c, int mi) {
  int last = scope_body_last(c, mi), body = c->scopes[mi].body;
  return last < 0 && body >= 0 && nt_kind(c->nt, body) == NK_BeginNode ? body : last;
}
/* The ivar a memoizing reader hands out: method `mi`'s value is `@x ||= v`,
   bare or in parentheses, and so is the ivar's slot itself. NULL for any
   other body. */
const char *an_memo_reader_ivar(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  int last = nt_paren_single(nt, scope_body_last(c, mi));
  if (last < 0 || nt_kind(nt, last) != NK_InstanceVariableOrWriteNode) return NULL;
  return nt_str(nt, last, "name");
}
int is_blk_param_call(Compiler *c, int node, int mi) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || !is_call_alias(nm)) return 0;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) return 0;
  const char *rn = nt_str(nt, recv, "name");
  const char *bp = c->scopes[mi].blk_param;
  return rn && bp && bp[0] && sp_streq(rn, bp);
}
/* Is the `return` written in a lambda (`-> { return 1 }`, `lambda { return 1 }`)?
   It leaves the lambda, not the method the lambda is written in. */
int return_in_lambda(Compiler *c, int rid) {
  const NodeTable *nt = c->nt;
  const int *par = comp_parent_map(c);
  for (int n = par[rid]; n >= 0; n = par[n]) {
    NodeKind k = nt_kind(nt, n);
    if (k == NK_DefNode) return 0;
    if (k == NK_LambdaNode) return 1;
    if (k == NK_BlockNode && par[n] >= 0 && nt_kind(nt, par[n]) == NK_CallNode) {
      const char *cn = nt_str(nt, par[n], "name");
      if (cn && is_lambda_constructor(cn) && nt_ref(nt, par[n], "receiver") < 0) return 1;
    }
  }
  return 0;
}
/* Is node `a` written before node `b`? By the place the parser stamped on
   them (a line and a column of one file), which the nodes a desugar clones
   keep: their ids come after those of every statement of the method. Where a
   node has none, by id. */
static int written_before(const NodeTable *nt, int a, int b) {
  long long fa = nt_int(nt, a, "node_file", -1), fb = nt_int(nt, b, "node_file", -1);
  long long la = nt_int(nt, a, "node_line", 0), lb = nt_int(nt, b, "node_line", 0);
  if (la > 0 && lb > 0 && fa == fb) {
    long long ca = nt_int(nt, a, "node_col", 0), cb = nt_int(nt, b, "node_col", 0);
    if (la != lb) return la < lb;
    if (ca != cb) return ca < cb;
  }
  return a < b;
}
/* Is there a `return` of scope `si` other than `guard_ret`, written before it
   (an early `return 0 if @a.nil?` before `return to_enum(:m) unless
   block_given?`)? The guard then does not stop every blockless call. */
int return_before(Compiler *c, int si, int guard_ret) {
  NT_FOREACH_KIND(c->nt, NK_ReturnNode, rid)
    if (rid != guard_ret && c->nscope[rid] == si && written_before(c->nt, rid, guard_ret) &&
        !return_in_lambda(c, rid)) return 1;
  return 0;
}
/* Is `node` an instance_eval / instance_exec that hands the method's own
   block on (`instance_exec(*args, &block)`) as the value the method falls off
   its end with? The inlined method answers the value that block has at each
   call site, as a tail `yield` does. */
int ie_forwards_block_tail(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || !is_instance_eval_family(nm)) return 0;
  int blk = nt_ref(nt, node, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode || !call_forwards_own_block(c, node)) return 0;
  /* on self, or on a receiver of one known class, whose self the block takes
     (a boxed receiver is dispatched by class at run time: the block's value
     there is that of each class's body) */
  int recv = nt_ref(nt, node, "receiver");
  if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && !ty_is_object(infer_type(c, recv))) return 0;
  Scope *sc = comp_scope_of(c, node);
  return sc && node_is_scope_tail(c, (int)(sc - c->scopes), node);
}
int g_yvt_mi[MAX_YVT_DEPTH];
int g_yvt_depth = 0;
/* set by yield_value_diverges to make yield_value_type unify all call sites
   (rather than take the first concrete one) for divergence detection */
int g_yvt_unify_all = 0;
int an_ie_class_id = -1;
int g_cbody_class_id = -1;
int g_cbody_direct = -1;
TyKind scan_break_type(Compiler *c, int id, int depth) {
  if (id < 0 || depth > 32) return TY_UNKNOWN;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  NodeKind ty_k = nt_kind(nt, id);
  if (!ty) return TY_UNKNOWN;
  if (ty_k == NK_DefNode) return TY_UNKNOWN;
  if (ty_k == NK_BlockNode && depth > 0) return TY_UNKNOWN; /* inner block's breaks don't escape */
  if (ty_k == NK_BreakNode) {
    int v = nt_ref(nt, id, "arguments");
    if (v < 0) return TY_NIL;
    int vargc = 0; const int *vargs = nt_arr(nt, v, "arguments", &vargc);
    if (vargc > 0) return infer_type(c, vargs[0]);
    return TY_NIL;
  }
  TyKind result = TY_UNKNOWN;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) {
    TyKind t = scan_break_type(c, nt_ref_at(nt, id, i), depth + 1);
    if (t != TY_UNKNOWN) result = ty_unify(result, t);
  }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind t = scan_break_type(c, ids[k], depth + 1);
      if (t != TY_UNKNOWN) result = ty_unify(result, t);
    }
  }
  return result;
}
TyKind scan_throw_type(Compiler *c, const char *tag) {
  /* Program-wide: a throw can reach its catch through any number of method
     frames, so the catch's value type must unify with every throw that can
     target it. A literal tag on both sides that differs is provably
     unreachable; a non-literal tag on either side always participates. */
  const NodeTable *nt = c->nt;
  TyKind result = TY_UNKNOWN;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "throw") || nt_ref(nt, id, "receiver") >= 0) continue;
    int v = nt_ref(nt, id, "arguments");
    int vargc = 0; const int *vargs = v >= 0 ? nt_arr(nt, v, "arguments", &vargc) : NULL;
    if (vargc < 1) continue;
    NodeKind tk = nt_kind(nt, vargs[0]);
    const char *tnm = NULL;
    if (tk == NK_SymbolNode) tnm = nt_str(nt, vargs[0], "value");
    else if (tk == NK_StringNode) tnm = nt_str(nt, vargs[0], "unescaped");
    if (tag && tnm && !sp_streq(tnm, tag)) continue;
    TyKind t = vargc >= 2 ? infer_type(c, vargs[1]) : TY_NIL;
    if (t != TY_UNKNOWN) result = ty_unify(result, t);
  }
  return result;
}
/* Call sites that pass a block (an explicit block node, or a `...` forward that
   carries one implicitly), cached per node table. yield_value_type scans for
   these once per method during the fixpoint; without the index that is
   O(methods * nodes * iterations). The structural shape is stable across the
   pass; the callee resolution below still runs fresh. */
static const NodeTable *yvt_nt = NULL;
static int yvt_ntc = -1, yvt_n = 0, yvt_sup_n = 0;
static int *yvt_ids = NULL, *yvt_sup_ids = NULL;
static int yvt_call_forwards_block(const NodeTable *nt, int cid) {
  int a = nt_ref(nt, cid, "arguments");
  int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  return nt_argv_only_forwarding(nt, av, an);
}
/* Could block-passing call site `cid` reach method `mi` at all? A callee is
   found by name: the call's own name, that name through an `alias`, or `new`
   for `initialize` (yvt_callee_index). So a call whose name is none of those
   cannot, and the resolution -- a receiver type inference per call -- is
   skipped for it. Every yielding method used to resolve every block call in
   the program, methods x calls per round (#4847). An aliased name, and a
   prepend shadow (named `__prep_N_m`), are left to the full resolution. */
static int *yvt_nm_head = NULL, *yvt_nm_next = NULL, *yvt_always = NULL;
static int yvt_nb = 0, yvt_always_n = 0;
/* the alias-named entries by the names their alias may stand for: name t's
   are yvt_tg_ii[yvt_tg_off[t] .. yvt_tg_off[t + 1]), ascending (yvt_tg_build) */
static ANameHash yvt_tg_names;
static int *yvt_tg_off = NULL, *yvt_tg_ii = NULL, yvt_tg_nn = 0;
/* the names are copies of their own: an alias table's string is replaced
   when the alias settles (settle_mixin_aliases), and a node's when a
   desugar renames it, neither of which rebuilds this index */
static char **yvt_tg_owned = NULL; static int yvt_tg_nowned = 0, yvt_tg_ocap = 0;
static const char *yvt_tg_own(const char *s) {
  if (yvt_tg_nowned == yvt_tg_ocap) {
    yvt_tg_ocap = yvt_tg_ocap ? yvt_tg_ocap * 2 : 64;
    yvt_tg_owned = realloc(yvt_tg_owned, sizeof(char *) * (size_t)yvt_tg_ocap);
    if (!yvt_tg_owned) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  char *cp = strdup(s);
  if (!cp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  yvt_tg_owned[yvt_tg_nowned++] = cp;
  return cp;
}
static void yvt_tg_build(Compiler *c);
static unsigned char *yvt_alias_name = NULL;   /* per yvt_ids entry: its name is an alias somewhere */
static unsigned char *yvt_fwd = NULL;          /* per yvt_ids entry: yvt_call_forwards_block */
static int yvt_name_is_alias(Compiler *c, const char *cn) {
  for (int k = 0; k < c->nclasses; k++)
    for (int i = 0; i < c->classes[k].naliases; i++)
      if (c->classes[k].alias_new[i] && sp_streq(c->classes[k].alias_new[i], cn)) return 1;
  return 0;
}
static int yvt_may_reach(Compiler *c, int ii, int mi) {
  const char *mn = c->scopes[mi].name;
  const char *cn = nt_str(c->nt, yvt_ids[ii], "name");
  if (!mn || !cn) return 1;
  if (sp_streq(cn, mn)) return 1;
  if (sp_streq(cn, "new") && sp_streq(mn, "initialize")) return 1;
  if (strncmp(mn, "__prep_", 7) == 0) return 1;
  return yvt_alias_name && yvt_alias_name[ii];
}
static void yvt_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  free(yvt_ids); free(yvt_sup_ids); free(yvt_alias_name); free(yvt_fwd);
  yvt_alias_name = NULL; yvt_fwd = NULL;
  yvt_ids = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  yvt_sup_ids = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  yvt_n = 0; yvt_sup_n = 0;
  yvt_nt = nt; yvt_ntc = n;
  if (!yvt_ids || !yvt_sup_ids) return;
  for (int cid = 0; cid < n; cid++) {
    const char *cty = nt_type(nt, cid);
    NodeKind ck = nt_kind(nt, cid);
    if (!cty) continue;
    /* a `super` reaching a yielding parent is a call site of that parent: the
       block that arrives is the child's own */
    if (ck == NK_SuperNode || ck == NK_ForwardingSuperNode) {
      yvt_sup_ids[yvt_sup_n++] = cid;
      continue;
    }
    if (ck != NK_CallNode) continue;
    if (nt_ref(nt, cid, "block") < 0 && !yvt_call_forwards_block(nt, cid)) continue;
    yvt_ids[yvt_n++] = cid;
  }
  /* the forward test is structural, and was re-run for every entry of this
     list once per yielding method: 7% of a 16K-line analysis (#4847) */
  yvt_fwd = malloc((size_t)(yvt_n > 0 ? yvt_n : 1));
  if (yvt_fwd)
    for (int ii = 0; ii < yvt_n; ii++) yvt_fwd[ii] = (unsigned char)yvt_call_forwards_block(nt, yvt_ids[ii]);
  yvt_alias_name = malloc((size_t)(yvt_n > 0 ? yvt_n : 1));
  if (yvt_alias_name)
    for (int ii = 0; ii < yvt_n; ii++) {
      const char *cn = nt_str(nt, yvt_ids[ii], "name");
      yvt_alias_name[ii] = cn ? (unsigned char)yvt_name_is_alias(c, cn) : 1;
    }
  /* The entries by call name, each chain ascending, and the ones any method
     may be reached by (no name, or an alias). A yielding method walked every
     entry to find those of its own name, per ask: (yielding methods x block
     calls), quadratic in a program of many same-shaped classes. */
  free(yvt_nm_head); free(yvt_nm_next); free(yvt_always);
  yvt_nb = yvt_n > 0 ? yvt_n : 1;
  yvt_nm_head = malloc(sizeof(int) * (size_t)yvt_nb);
  yvt_nm_next = malloc(sizeof(int) * (size_t)(yvt_n > 0 ? yvt_n : 1));
  yvt_always = malloc(sizeof(int) * (size_t)(yvt_n > 0 ? yvt_n : 1));
  yvt_always_n = 0;
  if (!yvt_nm_head || !yvt_nm_next || !yvt_always) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int b = 0; b < yvt_nb; b++) yvt_nm_head[b] = -1;
  for (int ii = yvt_n - 1; ii >= 0; ii--) {
    const char *cn = nt_str(nt, yvt_ids[ii], "name");
    yvt_nm_next[ii] = -1;
    if (!cn) continue;
    int b = (int)(sp_strhash(cn) % (unsigned)yvt_nb);
    yvt_nm_next[ii] = yvt_nm_head[b]; yvt_nm_head[b] = ii;
  }
  for (int ii = 0; ii < yvt_n; ii++)
    if (!nt_str(nt, yvt_ids[ii], "name")) yvt_always[yvt_always_n++] = ii;
  yvt_tg_build(c);
}
/* The alias-named entries, by the names their alias may stand for: the
   alias tables' edges (new -> old, every class's, as the chain resolution
   hops them) followed from the entry's name to every name they reach. Such
   an entry was on the always-list, walked by every yielding method per
   ask; on a Rails program most block calls are named like some alias
   (each, map, call ...), so the list was most of the entries, and walking
   it was 12% of a sampled window. An entry named like an alias of no
   method's name resolves to that method along no path: the lookup matches
   names, and only an alias edge changes the name it matches. */
static void yvt_tg_build(Compiler *c) {
  anh_free(&yvt_tg_names); memset(&yvt_tg_names, 0, sizeof yvt_tg_names);
  for (int i = 0; i < yvt_tg_nowned; i++) free(yvt_tg_owned[i]);
  yvt_tg_nowned = 0;
  free(yvt_tg_off); free(yvt_tg_ii); yvt_tg_off = NULL; yvt_tg_ii = NULL; yvt_tg_nn = 0;
  const NodeTable *nt = c->nt;
  /* the edges, chained by their new name */
  ANameHash en; memset(&en, 0, sizeof en);
  int *ehead = NULL, *enext = NULL, ne = 0, ecap = 0, hcap = 0;
  const char **eold = NULL;
  for (int k = 0; k < c->nclasses; k++)
    for (int i = 0; i < c->classes[k].naliases; i++) {
      const char *a = c->classes[k].alias_new[i], *o = c->classes[k].alias_old[i];
      if (!a || !o) continue;
      int e = anh_find(&en, a);
      if (e < 0) { anh_add(&en, a); e = anh_find(&en, a); }
      if (e >= hcap) {
        int nc = e + 64;
        ehead = realloc(ehead, sizeof(int) * (size_t)nc);
        if (!ehead) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        for (int j = hcap; j < nc; j++) ehead[j] = -1;
        hcap = nc;
      }
      if (ne == ecap) {
        ecap = ecap ? ecap * 2 : 64;
        enext = realloc(enext, sizeof(int) * (size_t)ecap);
        eold = realloc(eold, sizeof(const char *) * (size_t)ecap);
        if (!enext || !eold) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      enext[ne] = ehead[e]; eold[ne] = o; ehead[e] = ne; ne++;
    }
  /* each alias-named call name's closure, once: the target name ids it
     reaches (its own name excluded: that chain the entry is on already) */
  ANameHash yn; memset(&yn, 0, sizeof yn);
  int *yoff = NULL, *ylen = NULL, ycap = 0, *tpool = NULL, tn = 0, tcap = 0;
  int *pair_t = NULL, *pair_ii = NULL, np = 0, pcap = 0;
  for (int ii = 0; ii < yvt_n; ii++) {
    if (!yvt_alias_name || !yvt_alias_name[ii]) continue;
    const char *cn = nt_str(nt, yvt_ids[ii], "name");
    if (!cn) continue;
    int y = anh_find(&yn, cn);
    if (y < 0) {
      anh_add(&yn, cn); y = anh_find(&yn, cn);
      if (y >= ycap) {
        int nc = y + 64;
        yoff = realloc(yoff, sizeof(int) * (size_t)nc); ylen = realloc(ylen, sizeof(int) * (size_t)nc);
        if (!yoff || !ylen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        ycap = nc;
      }
      yoff[y] = tn; ylen[y] = 0;
      /* breadth-first over the edges; `seen` holds the names reached */
      ANameHash seen; memset(&seen, 0, sizeof seen);
      const char **q = NULL; int qn = 0, qcap = 0, qh = 0;
      anh_add(&seen, cn);
      q = realloc(q, sizeof(const char *) * 16); qcap = 16; q[qn++] = cn;
      while (qh < qn) {
        const char *nm = q[qh++];
        int e = anh_find(&en, nm);
        for (int j = e >= 0 ? ehead[e] : -1; j >= 0; j = enext[j]) {
          if (anh_has(&seen, eold[j])) continue;
          anh_add(&seen, eold[j]);
          if (qn == qcap) { qcap *= 2; q = realloc(q, sizeof(const char *) * (size_t)qcap); if (!q) { fprintf(stderr, "spinel: out of memory\n"); exit(1); } }
          q[qn++] = eold[j];
          int t = anh_find(&yvt_tg_names, eold[j]);
          if (t < 0) { const char *own = yvt_tg_own(eold[j]); anh_add(&yvt_tg_names, own); t = anh_find(&yvt_tg_names, own); }
          if (t >= yvt_tg_nn) yvt_tg_nn = t + 1;
          if (tn == tcap) { tcap = tcap ? tcap * 2 : 64; tpool = realloc(tpool, sizeof(int) * (size_t)tcap); if (!tpool) { fprintf(stderr, "spinel: out of memory\n"); exit(1); } }
          tpool[tn++] = t; ylen[y]++;
        }
      }
      free(q); anh_free(&seen);
    }
    for (int j = 0; j < ylen[y]; j++) {
      if (np == pcap) {
        pcap = pcap ? pcap * 2 : 256;
        pair_t = realloc(pair_t, sizeof(int) * (size_t)pcap); pair_ii = realloc(pair_ii, sizeof(int) * (size_t)pcap);
        if (!pair_t || !pair_ii) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      pair_t[np] = tpool[yoff[y] + j]; pair_ii[np] = ii; np++;
    }
  }
  /* the pairs by target name, each name's entries ascending (they were
     met ascending): counting sort */
  yvt_tg_off = calloc((size_t)yvt_tg_nn + 1, sizeof(int));
  yvt_tg_ii = malloc(sizeof(int) * (size_t)(np > 0 ? np : 1));
  if (!yvt_tg_off || !yvt_tg_ii) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int p = 0; p < np; p++) yvt_tg_off[pair_t[p] + 1]++;
  for (int t = 0; t < yvt_tg_nn; t++) yvt_tg_off[t + 1] += yvt_tg_off[t];
  int *fill = malloc(sizeof(int) * (size_t)(yvt_tg_nn > 0 ? yvt_tg_nn : 1));
  if (!fill) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int t = 0; t < yvt_tg_nn; t++) fill[t] = yvt_tg_off[t];
  for (int p = 0; p < np; p++) yvt_tg_ii[fill[pair_t[p]]++] = pair_ii[p];
  free(fill); free(pair_t); free(pair_ii); free(tpool); free(yoff); free(ylen); anh_free(&yn);
  free(ehead); free(enext); free(eold); anh_free(&en);
}
/* The entries that may reach `mi`, ascending: its name's chain, `new`'s for
   an initialize, the alias-named entries whose alias may stand for its
   name, and the always-list, merged. A method with no name, or a prepend
   shadow, takes every entry (see yvt_may_reach). */
typedef struct { int s[3]; int ai; int tp, te; int all; int next_all; int last; } YvtIt;
static void yvt_it_init(Compiler *c, YvtIt *it, int mi) {
  const char *mn = c->scopes[mi].name;
  it->all = !mn || strncmp(mn, "__prep_", 7) == 0 || !yvt_nm_head;
  it->next_all = 0; it->last = -1; it->ai = 0; it->tp = it->te = 0;
  it->s[0] = it->s[1] = -1;
  if (it->all) return;
  it->s[0] = yvt_nm_head[sp_strhash(mn) % (unsigned)yvt_nb];
  if (sp_streq(mn, "initialize")) it->s[1] = yvt_nm_head[sp_strhash("new") % (unsigned)yvt_nb];
  int t = yvt_tg_off ? anh_find(&yvt_tg_names, mn) : -1;
  if (t >= 0 && t < yvt_tg_nn) { it->tp = yvt_tg_off[t]; it->te = yvt_tg_off[t + 1]; }
}
static int yvt_it_next(YvtIt *it) {
  if (it->all) return it->next_all < yvt_n ? it->next_all++ : -1;
  for (;;) {
    int a = it->s[0], b = it->s[1];
    int c3 = it->ai < yvt_always_n ? yvt_always[it->ai] : -1;
    int d = it->tp < it->te ? yvt_tg_ii[it->tp] : -1;
    int m = -1;
    if (a >= 0) m = a;
    if (b >= 0 && (m < 0 || b < m)) m = b;
    if (c3 >= 0 && (m < 0 || c3 < m)) m = c3;
    if (d >= 0 && (m < 0 || d < m)) m = d;
    if (m < 0) return -1;
    if (a == m) it->s[0] = yvt_nm_next[a];
    if (b == m) it->s[1] = yvt_nm_next[b];
    if (c3 == m) it->ai++;
    if (d == m) it->tp++;
    if (m > it->last) { it->last = m; return m; }
  }
}
/* Which method does block-passing call site `cid` reach? Shared by
   yield_value_type and yield_block_tails so the two agree on what counts as a
   call site of a given method. */
static int yvt_callee_index_scan(Compiler *c, int cid);
/* Each site's callee, kept as long as infer_type's memo keeps the types it
   was resolved from (infer_memo_edits), and only while that memo is in use.
   A yielding method's value asks the callee of every block call that may
   reach it -- those of its name, and every one whose name is an alias
   somewhere (yvt_may_reach) -- so on a Rails program, where the common
   names are aliases in some class, each ask resolved thousands of sites,
   and the asks of a round resolved the same site over and over: the
   resolution was 10% of a sampled window late in the fixpoint. */
static int *yci_ans; static unsigned *yci_stamp; static int yci_cap;
static unsigned yci_gen = 1; static unsigned long yci_edits = (unsigned long)-1;
static int yvt_callee_index(Compiler *c, int cid) {
  if (cid < 0 || !infer_memo_usable()) return yvt_callee_index_scan(c, cid);
  unsigned long edits = infer_memo_edits(c);
  if (edits != yci_edits || cid >= yci_cap) {
    if (cid >= yci_cap) {
      int cap = c->nt->count > cid + 1 ? c->nt->count : cid + 1;
      free(yci_ans); free(yci_stamp);
      yci_ans = malloc(sizeof(int) * (size_t)cap);
      yci_stamp = calloc((size_t)cap, sizeof(unsigned));
      if (!yci_ans || !yci_stamp) { free(yci_ans); free(yci_stamp); yci_ans = NULL; yci_stamp = NULL; yci_cap = 0; return yvt_callee_index_scan(c, cid); }
      yci_cap = cap;
    }
    yci_edits = edits;
    if (++yci_gen == 0) { memset(yci_stamp, 0, sizeof(unsigned) * (size_t)yci_cap); yci_gen = 1; }
  }
  if (yci_stamp[cid] == yci_gen) return yci_ans[cid];
  int rmi = yvt_callee_index_scan(c, cid);
  /* the scan's inferences may have moved the memo on (a type found, a
     local interned): an answer from before that is not kept */
  if (infer_memo_edits(c) == yci_edits) { yci_ans[cid] = rmi; yci_stamp[cid] = yci_gen; }
  return rmi;
}
static int yvt_callee_index_scan(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, cid, "name");
  int crecv = nt_ref(nt, cid, "receiver");
  int rmi = -1;
  if (crecv < 0) {
    rmi = comp_cbody_call_mi(c, cid, cn);
    /* resolved as emission resolves it: a class method's own chain before
       a top-level def of the same name */
    if (rmi < 0 && cn) rmi = comp_self_call_mi(c, cid, cn);
    if (rmi < 0) rmi = call_target(c, cid, -1, cn, CT_TOP, NULL);
    if (rmi < 0) {
      Scope *cs = comp_scope_of(c, cid);
      if (cs->class_id >= 0) {
        rmi = call_target(c, cid, cs->class_id, cn, 0, NULL);
        /* an implicit-self call inside a class method resolves to a CLASS
           method; its block/forward feeds that method's blk_ret too */
        if (rmi < 0) rmi = call_target(c, cid, cs->class_id, cn, CT_CMETHOD, NULL);
      }
    }
    /* a bare call to a method of a module included at the top level, as
       infer_uncached resolves it: its block feeds that method's yield.
       Missed here, T.twice's return was typed from no block at all, so
       `n = twice { 2 }` under `include T` typed nil */
    if (rmi < 0) rmi = comp_included_method_index(c, cn, cid);
  }
  else {
    TyKind crt = infer_type(c, crecv);
    if (ty_is_object(crt)) rmi = call_target(c, cid, ty_object_class(crt), cn, 0, NULL);
    /* `Klass.new { block }`: the block feeds the class's initialize. A
       scoped receiver (`NS::Klass`) resolves by its leaf name, the key
       classes are indexed under. */
    else if (cn && sp_streq(cn, "new") && nt_type(nt, crecv) &&
             (nt_kind(nt, crecv) == NK_ConstantReadNode ||
              nt_kind(nt, crecv) == NK_ConstantPathNode)) {
      int nci = comp_class_index(c, nt_str(nt, crecv, "name"));
      if (nci >= 0) rmi = comp_method_in_chain(c, nci, "initialize", NULL);
    }
    /* `Klass.m { block }` / `Mod.m { block }` / `NS::Klass.m { block }`: a
       class/module self-method (singleton). Resolve the constant and look
       up its singleton method so the block's value type flows back to m's
       return type -- instance methods resolve via the ty_is_object arm
       above (#1446). */
    else if (nt_type(nt, crecv) &&
             (nt_kind(nt, crecv) == NK_ConstantReadNode ||
              nt_kind(nt, crecv) == NK_ConstantPathNode)) {
      int nci = comp_class_index(c, nt_str(nt, crecv, "name"));
      if (nci >= 0) rmi = call_target(c, cid, nci, cn, CT_CMETHOD, NULL);
    }
  }
  return rmi;
}

int init_accepts_kw_call(Compiler *c, int initm, const int *argv, int argc);

/* A block-passing call whose receiver is poly, named like instance method
   `mi`: yvt_callee_index cannot resolve it, yet codegen dispatches it on the
   receiver's class at run time and may land on `mi` with this block. */
static int yvt_poly_recv_may_reach(Compiler *c, int cid, int mi) {
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, cid, "name");
  const char *mn = c->scopes[mi].name;
  if (!cn || !mn || !sp_streq(cn, mn)) return 0;
  if (c->scopes[mi].class_id < 0 || c->scopes[mi].is_cmethod) return 0;
  /* the receiver is read once the names agree: nearly every site of the
     list is of another name, and its field scan was 7% of a Rails window */
  int crecv = nt_ref(nt, cid, "receiver");
  if (crecv < 0) return 0;
  return infer_type(c, crecv) == TY_POLY;
}

static int yvt_reaches(Compiler *c, int cid, int mi) {
  /* A `new` on a cycle of constructors hands its block to the initialize's
     proc-form clone as a proc (ctor_site_on_cycle), so it is no site of the
     initialize's own yield, which only the sites outside the cycle splice.
     A ring of N such classes otherwise answered each initialize's yield
     from all of them, the clone's copies too. */
  Scope *m = &c->scopes[mi];
  if (m->ctor_cycle && m->class_id >= 0 && !c->classes[m->class_id].is_struct &&
      sp_streq(comp_prep_user_name(m->name), "initialize") && ctor_site_on_cycle(c, cid, mi))
    return 0;
  if (yvt_callee_index(c, cid) == mi) return 1;
  /* the rest is a `new` reaching an initialize: the method's side first,
     before the site's fields are read (every site asks this of every
     yielding method) */
  const char *mn = c->scopes[mi].name;
  if (!mn || !sp_streq(mn, "initialize") || !c->scopes[mi].yields) return 0;
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, cid, "name");
  int crecv = nt_ref(nt, cid, "receiver");
  if (!cn || !sp_streq(cn, "new") || crecv < 0 || nt_ref(nt, cid, "block") < 0) return 0;
  NodeKind rk = nt_kind(nt, crecv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return 0;
  TyKind rt = infer_type(c, crecv);
  if (rt != TY_POLY && !(rt == TY_CLASS && class_var_static_ci(c, crecv) < 0)) return 0;
  int a = nt_ref(nt, cid, "arguments");
  int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  if (!init_accepts_kw_call(c, mi, av, an)) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (!c->classes[k].is_struct && comp_method_in_chain(c, k, "initialize", NULL) == mi) return 1;
  return 0;
}

/* The value type of every `next` that leaves the block whose body is `node`
   (a nested loop, block, lambda or def binds its own). A bare `next` is nil.
   TY_UNKNOWN when there is none. The block's value is its tail joined with
   these: `{ |i| next 7 if i == 1; nil }` answers 7 or nil, and typing it from
   the tail alone made the yield nil, so `n += 1 if yield x` never counted. */
TyKind block_next_value_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NextNode) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an == 0) return TY_NIL;
    if (an > 1) return TY_POLY_ARRAY;
    NodeKind ak = nt_kind(nt, av[0]);
    if (ak == NK_SplatNode) return TY_POLY_ARRAY;
    return infer_type(c, av[0]);
  }
  if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode)
    return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind t = block_next_value_ty(c, nt_ref_at(nt, node, i));
    if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) {
      TyKind t = block_next_value_ty(c, ids[j]);
      if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
    }
  }
  return r;
}

/* The value a block forwarded out of method `emi` (`callee(&)`,
   `callee(&b)`, `callee(...)`) answers inside it. The forwarding call is one
   node in emi's body, shared by every site emi is spliced into, so the first
   concrete site's block type is not enough: when emi's sites' blocks answer
   different kinds (`machine(:x) { "s" }` and `machine(:y) { 42 }`), the value
   is only known at run time, as for a `&block.call` (#3793). Typed from the
   first site, the second site's Integer was stored into the first site's
   `const char *` and the C did not build. */
static TyKind yvt_forwarded_value(Compiler *c, int emi) {
  TyKind first = yield_value_type(c, emi);
  if (g_yvt_unify_all || first == TY_UNKNOWN || first == TY_VOID) return first;
  g_yvt_unify_all = 1;
  TyKind all = yield_value_type(c, emi);
  g_yvt_unify_all = 0;
  return all != first && all != TY_UNKNOWN ? TY_POLY : first;
}

/* Whether block-passing call `cid` hands its callee the block the enclosing
   method was given: a `...` forward, or `&b` naming that method's block
   parameter (an anonymous `&` reads it under its desugared name). Any other
   `&expr` -- a proc in a local, a lambda, a Method -- is a value of its own,
   which the callee yields to in place of the enclosing method's block. The
   yield's value, the call's, the tails read for nilability and an
   instance_exec's value all ask this one question, so they agree: the call
   was typed from the enclosing method's block while its yield ran the proc. */
int call_forwards_own_block(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, cid, "block");
  if (blk < 0) return yvt_call_forwards_block(nt, cid);
  if (nt_kind(nt, blk) != NK_BlockArgumentNode) return 0;
  Scope *encl = comp_scope_of(c, cid);
  int bexpr = nt_ref(nt, blk, "expression");
  const char *bpn = encl && encl->blk_param && encl->blk_param[0] ? encl->blk_param : NULL;
  const char *ben = bexpr >= 0 && nt_kind(nt, bexpr) == NK_LocalVariableReadNode
                      ? nt_str(nt, bexpr, "name") : NULL;
  return bpn && ben && sp_streq(bpn, ben);
}

/* The body of the lambda or proc literal a `&expr` block argument writes in
   place (`m(&-> { 1 })`, `m(&proc { 1 })`), or -1 for any other value. */
static int yvt_proc_arg_body(Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  int bexpr = nt_ref(nt, blk, "expression");
  if (bexpr < 0) return -1;
  if (nt_kind(nt, bexpr) == NK_LambdaNode) return nt_ref(nt, bexpr, "body");
  if (nt_kind(nt, bexpr) == NK_CallNode) {
    const char *pnm = nt_str(nt, bexpr, "name");
    int pblk = nt_ref(nt, bexpr, "block");
    if (pnm && pblk >= 0 && is_proc_constructor(pnm) && nt_kind(nt, pblk) == NK_BlockNode)
      return nt_ref(nt, pblk, "body");
  }
  return -1;
}

/* What a callee yields to when the call passes a proc value of its own
   (call_forwards_own_block is 0): a literal written there types like an
   ordinary literal block, its tail being the value; any other callable is
   only known at run time, so poly. Typing it from the enclosing method's
   block made the whole call answer nil (#3688). */
static TyKind yvt_proc_arg_value(Compiler *c, int blk) {
  int pbody = yvt_proc_arg_body(c, blk);
  if (pbody < 0) return TY_POLY;
  int pn = 0; const int *pd = nt_arr(c->nt, pbody, "body", &pn);
  if (pn == 0) return TY_NIL;
  TyKind pt = infer_type(c, pd[pn - 1]);
  if (pt == TY_VOID) return TY_NIL;
  return pt == TY_UNKNOWN ? TY_POLY : pt;
}

TyKind yield_value_type(Compiler *c, int mi) {
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return TY_UNKNOWN;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return TY_UNKNOWN;
  g_yvt_mi[g_yvt_depth++] = mi;

  const NodeTable *nt = c->nt;
  TyKind result = TY_UNKNOWN;
  int pf_fwd = 0, poly_site = 0;
  if (yvt_nt != nt || yvt_ntc != nt->count) yvt_build(c);
  YvtIt yit; yvt_it_init(c, &yit, mi);
  /* Under --load-model=graph the call sites are taken in the program's
     order (prog_sort): where the first site's block answers (a method
     specialized per site keeps one), it is the site the program reaches
     first, whichever file holds it, not the one the node table happens to
     number first. One program text (the inline model) numbers its nodes in
     that order already, and its sites are read as they come. */
  int nsite = 0, csite = 0, si = 0, *site = NULL;
  if (prog_has_files(c)) {
    for (int ii; (ii = yvt_it_next(&yit)) >= 0; ) {
      if (nsite == csite) {
        csite = csite ? csite * 2 : 16;
        site = realloc(site, sizeof(int) * (size_t)csite);
        if (!site) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      }
      site[nsite++] = ii;
    }
    prog_sort(c, site, nsite, yvt_ids);
  }
  for (int ii; (ii = site ? (si < nsite ? site[si++] : -1) : yvt_it_next(&yit)) >= 0; ) {
    int cid = yvt_ids[ii];
    /* A `callee(...)` forward carries its block implicitly inside the `...`
       (no explicit block node); treat it as a forwarded block too. */
    if (!yvt_may_reach(c, ii, mi)) continue;
    int fwd_args = yvt_fwd ? yvt_fwd[ii] : yvt_call_forwards_block(nt, cid);
    /* skip calls that live inside method mi itself (recursive self-calls);
       only external call sites provide a concrete block value type */
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    if (!yvt_reaches(c, cid, mi)) {
      if (yvt_poly_recv_may_reach(c, cid, mi)) poly_site = 1;
      continue;
    }
    /* read once the site is known to reach: the sites that do not are
       nearly all of the list */
    int blk = nt_ref(nt, cid, "block");
    /* `mi(&b)` / `mi(...)`: the call forwards the block of its enclosing
       method rather than passing a literal. The value `mi` yields is then
       whatever that forwarded block produces -- the enclosing method's
       own yield value. */
    if (fwd_args || nt_call_has_block_arg(nt, cid)) {
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      /* `mi(&some_proc)`: a first-class Proc / lambda / Method value, not the
         enclosing method's own block forwarded on. */
      if (!call_forwards_own_block(c, cid)) {
        TyKind pt = yvt_proc_arg_value(c, blk);
        if (c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) { result = pt; break; }
        result = ty_unify(result, pt);
        continue;
      }
      TyKind ft = (emi >= 0 && emi != mi) ? yvt_forwarded_value(c, emi) : TY_UNKNOWN;
      if (ft == TY_VOID) ft = TY_NIL;
      if (ft == TY_UNKNOWN && emi >= 0 && c->scopes[emi].is_proc_form) pf_fwd = 1;
      if (c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) {
        if (ft != TY_UNKNOWN) { result = ft; break; }
        continue;
      }
      result = ty_unify(result, ft);
      continue;
    }
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    TyKind bt;
    if (bn == 0) bt = TY_NIL;
    else if (nt_kind(nt, bd[bn - 1]) == NK_ReturnNode)
      /* `{ return e }`: a non-local return -- the yield never produces a
         value, but typing it as e's type keeps the enclosing method's
         return shape consistent (the inline emits `return e` directly). */
      bt = return_node_type(c, bd[bn - 1]);
    else bt = infer_type(c, bd[bn - 1]);
    if (bt == TY_VOID) bt = TY_NIL;
    {
      TyKind nx = block_next_value_ty(c, bb);
      if (nx != TY_UNKNOWN) bt = (bt == TY_UNKNOWN) ? nx : ty_unify(bt, nx);
    }
    /* A yield-inlined (or self-recursive-lowered) method is specialized per
       call site, so its internal block type is the first concrete block. A
       non-inlined method (an escaping &block called via the proc ABI) has one
       body, so its block value type must unify ALL call sites (string + int
       block -> poly). */
    if ((c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) && !g_yvt_unify_all) { result = bt; break; }
    result = ty_unify(result, bt);
  }
  /* A proc form forwarding its block (`def self.f(a, &) = g(a, &)` called
     only through a Method) hands on a real proc no literal types: its value
     is poly, where an unknown one unboxed it into the other arm's slot. */
  free(site);
  if (result == TY_UNKNOWN && pf_fwd) result = TY_POLY;
  /* The only calls were on a poly receiver (a proc parameter, a value out of a
     poly collection). They still run this method with their block, and the
     yield reads that block's value boxed, so it is poly. Left unknown, a local
     assigned from the yield (`found = yield(row)`) typed from its other writes
     alone: a nil, so the method returned nil and threw the value away. */
  if (result == TY_UNKNOWN && poly_site) result = TY_POLY;
  g_yvt_depth--;
  return result;
}

/* The tail expression of every literal block that can reach `mi`'s yield.
   This is yield_value_type's call-site scan answering with NODES instead of a
   unified type, because nilability is a property of the producing EXPRESSION,
   not of the type it settles on -- an `Integer?` and an `Integer` both arrive
   as TY_INT, so the type alone cannot say whether the value can be nil
   (#3505). A forwarded block (`m(&b)` / `m(...)`) has no literal of its own
   and delegates to the enclosing method's tails.
   Unlike yield_value_type this collects EVERY site rather than stopping at the
   first concrete one: the caller ORs the results, and a single nilable block
   anywhere is enough to make the slot nilable. Writes at most `max` ids and
   returns how many. */
/* With yield_block_tails_next set, a literal block's `next v` (and a bare
   `next`) is one of its values too: the NextNode ids follow its tail (the
   nested blocks', lambdas', defs' and loops' own `next` excluded). Only the
   nil question asks for them (nullable_int_value). */
int yield_block_tails_next = 0;
static int ybt_collect_next(const NodeTable *nt, int id, int *out, int max, int depth) {
  if (id < 0 || max <= 0 || depth > 200) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_NextNode) { out[0] = id; return 1; }
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_WhileNode ||
      k == NK_UntilNode || k == NK_LambdaNode || k == NK_ForNode || k == NK_BlockNode) return 0;
  int n = 0;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr && n < max; i++) n += ybt_collect_next(nt, nt_ref_at(nt, id, i), out + n, max - n, depth + 1);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na && n < max; i++) {
    int an = 0; const int *ids = nt_arr_at(nt, id, i, &an);
    for (int j = 0; j < an && n < max; j++) n += ybt_collect_next(nt, ids[j], out + n, max - n, depth + 1);
  }
  return n;
}

int yield_block_tails(Compiler *c, int mi, int *out, int max) {
  if (max <= 0) return 0;
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return 0;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return 0;
  g_yvt_mi[g_yvt_depth++] = mi;

  const NodeTable *nt = c->nt;
  int n = 0;
  if (yvt_nt != nt || yvt_ntc != nt->count) yvt_build(c);
  YvtIt yit; yvt_it_init(c, &yit, mi);
  for (int ii; n < max && (ii = yvt_it_next(&yit)) >= 0; ) {
    int cid = yvt_ids[ii];
    int blk = nt_ref(nt, cid, "block");
    if (!yvt_may_reach(c, ii, mi)) continue;
    int fwd_args = yvt_fwd ? yvt_fwd[ii] : yvt_call_forwards_block(nt, cid);
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    if (!yvt_reaches(c, cid, mi)) continue;
    if (fwd_args || nt_call_has_block_arg(nt, cid)) {
      /* a proc value of the call's own: a literal's tail, as a literal
         block's; any other arrives boxed, with no scalar nil to carry */
      if (!call_forwards_own_block(c, cid)) {
        int pbody = yvt_proc_arg_body(c, blk);
        int pn = 0; const int *pd = pbody >= 0 ? nt_arr(nt, pbody, "body", &pn) : NULL;
        if (pd && pn > 0) out[n++] = pd[pn - 1];
        continue;
      }
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      if (emi >= 0 && emi != mi) n += yield_block_tails(c, emi, out + n, max - n);
      continue;
    }
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    if (bd && bn > 0) out[n++] = bd[bn - 1];
    if (yield_block_tails_next && bb >= 0 && n < max) n += ybt_collect_next(nt, bb, out + n, max - n, 0);
  }
  g_yvt_depth--;
  return n;
}
/* The calls whose literal block reaches `mi`'s yield, as yield_block_tails
   finds them (a forwarded block delegating to the enclosing method's
   calls), into out; how many, or -1 when a chain of forwarded blocks runs
   past the walk's depth (the list is then not known to be complete). A
   method already on the walk is being listed by the frame that holds it.
   A proc handed in (`m(&pr)`) has no literal block whose parameters the
   yield binds, and is left out. */
int yield_block_calls(Compiler *c, int mi, int *out, int max) {
  if (max <= 0) return 0;
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return 0;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return -1;
  g_yvt_mi[g_yvt_depth++] = mi;
  const NodeTable *nt = c->nt;
  int n = 0;
  if (yvt_nt != nt || yvt_ntc != nt->count) yvt_build(c);
  YvtIt yit; yvt_it_init(c, &yit, mi);
  for (int ii; n < max && (ii = yvt_it_next(&yit)) >= 0; ) {
    int cid = yvt_ids[ii];
    if (!yvt_may_reach(c, ii, mi)) continue;
    int fwd_args = yvt_fwd ? yvt_fwd[ii] : yvt_call_forwards_block(nt, cid);
    if (!yvt_reaches(c, cid, mi)) continue;
    if (fwd_args || nt_call_has_block_arg(nt, cid)) {
      /* (a call in mi itself forwarding mi's own block is mi's caller's) */
      if ((int)(comp_scope_of(c, cid) - c->scopes) == mi || !call_forwards_own_block(c, cid)) continue;
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      int k = emi >= 0 && emi != mi ? yield_block_calls(c, emi, out + n, max - n) : 0;
      if (k < 0) { g_yvt_depth--; return -1; }
      n += k;
      continue;
    }
    /* a recursive call with a literal block reaches the yield too */
    out[n++] = cid;
  }
  g_yvt_depth--;
  return n;
}
/* The block value that reaches `mi` from BELOW: a child method whose `super`
   lands on mi forwards its own caller's block down. Only the middle link of a
   super chain needs this -- its own call sites are all `super`, so the site
   scan above finds none and its value would come out void. Kept separate from
   yield_value_type so that a method with real call sites keeps its per-site
   specialization instead of being pinned to a sibling's block type. */
TyKind yield_value_type_via_super(Compiler *c, int mi) {
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return TY_UNKNOWN;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return TY_UNKNOWN;
  g_yvt_mi[g_yvt_depth++] = mi;
  const NodeTable *nt = c->nt;
  if (yvt_nt != nt || yvt_ntc != nt->count) yvt_build(c);
  TyKind result = TY_UNKNOWN;
  for (int ii = 0; ii < yvt_sup_n; ii++) {
    Scope *cs = comp_scope_of(c, yvt_sup_ids[ii]);
    if (!cs || cs->class_id < 0 || !cs->name) continue;
    int cmi = (int)(cs - c->scopes);
    if (cmi == mi) continue;
    /* where that super lands: the next shadow in its own class (an earlier
       include's or extend's copy), or the superclass chain */
    int rmi = a_super_target(c, cs);
    if (rmi != mi) continue;
    TyKind ft = yield_value_type(c, cmi);
    if (ft == TY_UNKNOWN) ft = yield_value_type_via_super(c, cmi);
    if (ft == TY_VOID) ft = TY_NIL;
    if (ft == TY_UNKNOWN) continue;
    result = (result == TY_UNKNOWN) ? ft : ty_unify(result, ft);
  }
  g_yvt_depth--;
  return result;
}
/* Whether a yield-inlined method's block value type DIVERGES across its call
   sites (one caller's block returns int, another's an object): the shared
   accumulator that receives yield results must then hold a poly element (an
   sp_IntArray built from an object-returning block miscompiles, #2454). Reuses
   yield_value_type's site scan with the first-concrete break disabled. */
int yield_value_diverges(Compiler *c, int mi) {
  if (mi < 0 || !c->scopes[mi].yields) return 0;
  int sv = g_yvt_unify_all; g_yvt_unify_all = 1;
  TyKind t = yield_value_type(c, mi);
  g_yvt_unify_all = sv;
  if (t == TY_POLY) return 1;
  /* Asking whether the UNION is poly misses the disagreements ty_unify can
     absorb, and a nil one it always can: `nil` joined to a String is a
     nullable String, to an Integer a nullable Integer. So a method yielded to
     from two sites, one block answering a value and the other answering
     nothing, read as agreeing -- and the per-site scan then handed the FIRST
     site's type to every site, so the other's value was typed from it. With
     the nil site first, `Tempfile.create { ... find(path) }` answered nil for
     a block that plainly returns a String, and nothing was reported (#4819).
     Compare the sites themselves instead: two that answer different types
     diverge whether or not a union exists for them. */
  {
    int tails[32];
    int nsite = yield_block_tails(c, mi, tails, 32);
    TyKind seen = TY_UNKNOWN; int have = 0;
    for (int i = 0; i < nsite; i++) {
      TyKind bt = tails[i] >= 0 ? infer_type(c, tails[i]) : TY_NIL;
      if (bt == TY_VOID) bt = TY_NIL;
      if (bt == TY_UNKNOWN) continue;
      if (!have) { seen = bt; have = 1; continue; }
      if (bt != seen) return 1;
    }
  }
  return 0;
}
/* The value type `node` contributes to an accumulator, widened to poly when it
   is a `yield` whose enclosing method's block value type diverges across call
   sites. One shared inlined body cannot size the accumulator for both an int-
   and a String-returning block: whichever site is analyzed first settles the
   slot, and the other emits into it. Used for an explicit `acc << yield(x)`
   (#2454) and for the collector an iterator builds from a `yield` block body
   -- `[x].map { |v| yield v }` inside a wrapper method (#2457). */
/* Does the program define a method of this name itself? A builtin fallback for
   a poly receiver must decline then: the user method is the likelier target. */
/* The scan below is O(nclasses * chain) per query and the fallback arms ask it
   per call site per fixpoint iteration; memoize per name. The answer depends
   only on scope/class shape, so stamp entries with the scope-index epoch (the
   same invalidation hash_new_default_arg's memo rides) plus the scope/class
   counts for the unfrozen phases where the epoch doesn't tick. */
static unsigned udm_gen = 0;
static int udm_nscopes = -1, udm_nclasses = -1, udm_cap = 0, udm_n = 0;
static char **udm_names = NULL;
static signed char *udm_ans = NULL;
static int *udm_next = NULL, *udm_head = NULL;
/* Per-epoch string sets making the memo fill O(1) for the common case:
   every instance-method name any class defines, and every alias source name.
   Open-addressed; rebuilt alongside the memo on an epoch change. */
typedef struct { char **v; int n, cap; } UdmSet;
static UdmSet udm_defined_set, udm_aliased_set;
static int udm_sets_stale = 1;
static void udm_set_clear(UdmSet *s) {
  for (int i = 0; i < s->cap; i++) { free(s->v[i]); s->v[i] = NULL; }
  s->n = 0;
}
static void udm_set_add(UdmSet *s, const char *k) {
  if (!k) return;
  if (s->n * 2 >= s->cap) {
    int ncap = s->cap ? s->cap * 2 : 1024;
    char **nv = calloc((size_t)ncap, sizeof(char *));
    if (!nv) return;
    for (int i = 0; i < s->cap; i++)
      if (s->v[i]) {
        unsigned j = sp_strhash(s->v[i]) & (unsigned)(ncap - 1);
        while (nv[j]) j = (j + 1) & (unsigned)(ncap - 1);
        nv[j] = s->v[i];
      }
    free(s->v); s->v = nv; s->cap = ncap;
  }
  unsigned j = sp_strhash(k) & (unsigned)(s->cap - 1);
  while (s->v[j]) {
    if (sp_streq(s->v[j], k)) return;
    j = (j + 1) & (unsigned)(s->cap - 1);
  }
  s->v[j] = strdup(k);
  if (s->v[j]) s->n++;
}
static int udm_set_has(const UdmSet *s, const char *k) {
  if (!s->cap) return 0;
  unsigned j = sp_strhash(k) & (unsigned)(s->cap - 1);
  while (s->v[j]) {
    if (sp_streq(s->v[j], k)) return 1;
    j = (j + 1) & (unsigned)(s->cap - 1);
  }
  return 0;
}
static void udm_sets_fill(Compiler *c) {
  udm_set_clear(&udm_defined_set);
  udm_set_clear(&udm_aliased_set);
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].class_id >= 0 && !c->scopes[s].is_cmethod && c->scopes[s].name)
      udm_set_add(&udm_defined_set, c->scopes[s].name);
  for (int k = 0; k < c->nclasses; k++)
    for (int i = 0; i < c->classes[k].naliases; i++)
      udm_set_add(&udm_aliased_set, c->classes[k].alias_new[i]);
  udm_sets_stale = 0;
}
static int udm_defined_name(Compiler *c, const char *name) {
  if (udm_sets_stale) udm_sets_fill(c);
  return udm_set_has(&udm_defined_set, name);
}
static int udm_aliased_name(Compiler *c, const char *name) {
  if (udm_sets_stale) udm_sets_fill(c);
  return udm_set_has(&udm_aliased_set, name);
}
/* True if a native (C-backed) class declares `name`. Such a method is not a
   scope, so an_user_defines_method cannot see it, but it means the same thing
   to a caller: the name is not necessarily the builtin container operation. */
int an_native_defines_method(Compiler *c, const char *name) {
  if (!name) return 0;
  for (int i = 0; i < c->n_native_methods; i++)
    if (c->native_methods[i].kind == 0 && c->native_methods[i].name &&
        sp_streq(c->native_methods[i].name, name)) return 1;
  return 0;
}
/* an_user_defines_method for a call with an explicit receiver: a top-level
   def is a private method of Object, which no explicit receiver reaches, so
   it does not take the name from a builtin (`def uniq(o) = o.uniq`). */
int an_user_recv_defines_method(Compiler *c, const char *name) {
  if (!an_user_defines_method(c, name)) return 0;
  if (comp_method_index(c, name) < 0) return 1;
  for (int uk = 0; uk < c->nclasses; uk++)
    if (comp_method_in_chain(c, uk, name, NULL) >= 0) return 1;
  return 0;
}
/* A call of merge!, update or fetch_values with no argument is a Hash or Array
   method only when the program defines no method of that name: with one (on
   Hash, Array, Object, a module they include or prepend, ...) or an alias to
   it (`alias update keys`) the call keeps the path it took before these forms
   were accepted. */
int an_zero_arg_builtin_shadowed(Compiler *c, const char *name, int argc) {
  if (argc != 0 || !name) return 0;
  if (!sp_streq(name, "merge!") && !sp_streq(name, "update") && !sp_streq(name, "fetch_values")) return 0;
  /* asked directly, not through an_user_defines_method: the builtin-only
     derivation (a user class owning the name beside a builtin receiver) must
     keep the program's definition in view too */
  for (int uk = 0; uk < c->nclasses; uk++)
    if (comp_method_in_chain(c, uk, name, NULL) >= 0) return 1;
  return udm_aliased_name(c, name);
}
int an_user_defines_method(Compiler *c, const char *name) {
  if (an_builtin_only_p()) return 0;   /* deriving the builtin-only answer (#3459) */
  if (!name) return 0;
  if (!comp_scope_index_is_frozen()) {
    /* scope shape may still change without the counts moving (renames);
       don't consult or populate the memo outside the frozen fixpoint. */
    for (int uk = 0; uk < c->nclasses; uk++)
      if (comp_method_in_chain(c, uk, name, NULL) >= 0) return 1;
    return comp_method_index(c, name) >= 0;
  }
  unsigned gen = comp_scope_index_gen();
  /* A frozen index only grows: the builtins' rewrites add a method per
     site, each followed by an ask. A new method can make a name defined,
     never undefined, so its name joins the set and only the "no" answers
     are asked again -- clearing everything per method made the rewrite
     pass quadratic in the methods it added. */
  if (gen == udm_gen && c->nclasses == udm_nclasses && c->nscopes > udm_nscopes && udm_nscopes >= 0) {
    for (int s = udm_nscopes; s < c->nscopes && !udm_sets_stale; s++)
      if (c->scopes[s].class_id >= 0 && !c->scopes[s].is_cmethod && c->scopes[s].name)
        udm_set_add(&udm_defined_set, c->scopes[s].name);
    for (int i = 0; i < udm_n; i++) if (udm_ans[i] == 0) udm_ans[i] = -1;
    udm_nscopes = c->nscopes;
  }
  if (gen != udm_gen || c->nscopes != udm_nscopes || c->nclasses != udm_nclasses) {
    for (int i = 0; i < udm_n; i++) free(udm_names[i]);
    udm_n = 0;
    if (!udm_cap) {
      udm_cap = 4096;
      udm_names = malloc(sizeof(char *) * (size_t)udm_cap);
      udm_ans = malloc((size_t)udm_cap);
      udm_next = malloc(sizeof(int) * (size_t)udm_cap);
      udm_head = malloc(sizeof(int) * (size_t)udm_cap);
      if (!udm_names || !udm_ans || !udm_next || !udm_head) { udm_cap = 0; }
    }
    for (int i = 0; i < udm_cap; i++) udm_head[i] = -1;
    udm_gen = gen; udm_nscopes = c->nscopes; udm_nclasses = c->nclasses;
    udm_sets_stale = 1;
  }
  unsigned b = udm_cap ? sp_strhash(name) & (unsigned)(udm_cap - 1) : 0;
  int again = -1;
  if (udm_cap)
    for (int i = udm_head[b]; i >= 0; i = udm_next[i])
      if (sp_streq(udm_names[i], name)) {
        if (udm_ans[i] >= 0) return udm_ans[i];
        again = i;
        break;
      }
  int ans = 0;
  if (udm_aliased_name(c, name)) {
    /* the name is an alias source somewhere: per-class resolution can redirect
       it, so only the full scan answers exactly (rare -- memoized above) */
    for (int uk = 0; uk < c->nclasses; uk++)
      if (comp_method_in_chain(c, uk, name, NULL) >= 0) { ans = 1; break; }
  }
  else {
    /* no alias redirects this name anywhere, so resolution is the identity for
       every class: defined iff some class's own method table has it (take that
       class as the chain start) */
    ans = udm_defined_name(c, name);
  }
  if (!ans) ans = comp_method_index(c, name) >= 0;
  if (again >= 0) { udm_ans[again] = (signed char)ans; return ans; }
  /* a full memo grows, and its buckets with it (as an_user_defines_or_reads'
     does): past 4,096 names it stopped recording, and every later name
     walked every class again at each ask */
  if (udm_cap && udm_n == udm_cap) {
    int ncap = udm_cap * 2;
    char **nn = realloc(udm_names, sizeof(char *) * (size_t)ncap);
    signed char *na = realloc(udm_ans, (size_t)ncap);
    int *nx = realloc(udm_next, sizeof(int) * (size_t)ncap);
    int *nh = realloc(udm_head, sizeof(int) * (size_t)ncap);
    if (nn) udm_names = nn;
    if (na) udm_ans = na;
    if (nx) udm_next = nx;
    if (nh) udm_head = nh;
    if (nn && na && nx && nh) {
      udm_cap = ncap;
      for (int i = 0; i < udm_cap; i++) udm_head[i] = -1;
      for (int i = 0; i < udm_n; i++) {
        unsigned h = sp_strhash(udm_names[i]) & (unsigned)(udm_cap - 1);
        udm_next[i] = udm_head[h]; udm_head[h] = i;
      }
      b = sp_strhash(name) & (unsigned)(udm_cap - 1);
    }
  }
  if (udm_cap && udm_n < udm_cap) {
    udm_names[udm_n] = strdup(name);
    if (udm_names[udm_n]) {
      udm_ans[udm_n] = (signed char)ans;
      udm_next[udm_n] = udm_head[b]; udm_head[b] = udm_n; udm_n++;
    }
  }
  return ans;
}

TyKind yield_aware_elem_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  /* the value parentheses answer: `(x; yield)` is the yield's */
  int n;
  if (vp_one(nt, node, VPS_NONE | VP_PARENS_ONLY, &n) == VP1_NODE && nt_kind(nt, n) == NK_YieldNode) {
    Scope *sc = comp_scope_of(c, n);
    int mi = sc ? (int)(sc - c->scopes) : -1;
    if (mi >= 0 && yield_value_diverges(c, mi)) return TY_POLY;
  }
  return infer_type(c, node);
}

/* The statement a call with a block answers from, when the body ends in
   `if/unless block_given? ... else ... end`: the block arm's last statement. */
int block_given_tail_then_last(Compiler *c, int last) {
  const NodeTable *nt = c->nt;
  if (last < 0) return -1;
  NodeKind k = nt_kind(nt, last);
  if (k != NK_IfNode && k != NK_UnlessNode) return -1;
  int pred = nt_ref(nt, last, "predicate");
  if (pred < 0 || nt_kind(nt, pred) != NK_CallNode || nt_ref(nt, pred, "receiver") >= 0) return -1;
  const char *pn = nt_str(nt, pred, "name");
  if (!pn || !sp_streq(pn, "block_given?")) return -1;
  int sub = nt_ref(nt, last, k == NK_UnlessNode ? "else_clause" : "subsequent");
  if (sub < 0 || nt_kind(nt, sub) != NK_ElseNode) return -1;
  int ts = nt_ref(nt, k == NK_UnlessNode ? sub : last, "statements");
  int tn = 0; const int *tb = ts >= 0 ? nt_arr(nt, ts, "body", &tn) : NULL;
  return tn > 0 ? tb[tn - 1] : -1;
}

/* Whether the calls of method `mi` give it a block: 1 when every call site
   does (a literal block, a forwarded `&b`, a `...`), 0 when none does, -1 when
   they differ or there is no site to read. A forwarded block is counted as
   present here; a forward from a method whose own answer is mixed is mixed
   too, one level up (recursion bounded like yield_value_type's). */
static int method_block_presence(Compiler *c, int mi) {
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return -1;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return -1;
  g_yvt_mi[g_yvt_depth++] = mi;
  const NodeTable *nt = c->nt;
  const char *mn = c->scopes[mi].name;
  int with = 0, without = 0, mixed = 0;
  /* the calls of the method's name, not every node: asked per call of a
     method whose answer depends on its block */
  for (int cid = mn ? an_calls_named_first(c, mn) : -1; cid >= 0 && !mixed; cid = an_calls_named_next(cid)) {
    if (nt_kind(nt, cid) != NK_CallNode) continue;
    const char *cn = nt_str(nt, cid, "name");
    if (!cn || !mn || !sp_streq(cn, mn)) continue;
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    if (!yvt_reaches(c, cid, mi)) continue;
    int blk = nt_ref(nt, cid, "block");
    if (blk < 0 && !yvt_call_forwards_block(nt, cid)) { without++; continue; }
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
      /* `m(&b)`: b is the enclosing method's own block, or a value that may
         be nil at run time */
      int ex = nt_ref(nt, blk, "expression");
      Scope *encl = comp_scope_of(c, cid);
      const char *xn = ex >= 0 && nt_kind(nt, ex) == NK_LocalVariableReadNode ? nt_str(nt, ex, "name") : NULL;
      if (encl && encl->blk_param && encl->blk_param[0] && xn && sp_streq(xn, encl->blk_param)) {
        int up = method_block_presence(c, (int)(encl - c->scopes));
        if (up == 1) with++; else if (up == 0) without++; else mixed = 1;
      }
      else mixed = 1;
      continue;
    }
    if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && nt_int(nt, blk, "fwd_yield", 0)) {
      Scope *encl = comp_scope_of(c, cid);
      int up = encl ? method_block_presence(c, (int)(encl - c->scopes)) : -1;
      if (up == 1) with++; else if (up == 0) without++; else mixed = 1;
      continue;
    }
    with++;
  }
  g_yvt_depth--;
  if (mixed || (with && without) || (!with && !without)) return -1;
  return with ? 1 : 0;
}

TyKind dispatch_ret_over(Compiler *c, int cid, const char *name, int cmeth, int base_mi, TyKind r,
                         int call_id) {
  int nd = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  for (int i = 0; i < nd; i++) {
    int kmi = cmeth ? call_target(c, call_id, ds[i], name, CT_CMETHOD, NULL)
                    : call_target(c, call_id, ds[i], name, 0, NULL);
    if (kmi < 0 || kmi == base_mi) continue;
    r = ty_unify(r, call_id >= 0 && c->scopes[kmi].yields ? method_call_ret(c, kmi, call_id)
                                                          : (TyKind)c->scopes[kmi].ret);
  }
  return r;
}

/* A tail yield specializes to its block, but an earlier return still leaves
   the same call. Keep both values in the inline result's type. */
static TyKind method_yield_ret(Compiler *c, int mi, TyKind ret) {
  TyKind own = (TyKind)c->scopes[mi].ret;
  if (own != TY_UNKNOWN && own != TY_VOID && ret != TY_UNKNOWN && ret != TY_VOID &&
      scope_has_return(c, mi)) return ty_unify(own, ret);
  return ret;
}

/* The values of a method pinned to its guard's Enumerator (Scope.ret_pin_guard)
   other than the guard's, joined: what a call with a block answers. 0 when
   the walk of its values gives up or there are none (the pin stands);
   otherwise 1, with *out the join, TY_UNKNOWN while a value is not typed yet. */
static int pinned_block_form_ret(Compiler *c, int mi, TyKind *out) {
  int lv[16];
  int n = method_value_leaves_or_nil(c, mi, lv, 16);
  TyKind acc = TY_UNKNOWN;
  int any = 0, nil = 0;
  for (int i = 0; i < n; i++) {
    if (lv[i] == c->scopes[mi].ret_pin_guard || return_in_lambda(c, lv[i])) continue;   /* a lambda's return leaves the lambda */
    TyKind t = infer_type(c, lv[i]);
    any = 1;
    if (t == TY_UNKNOWN) { *out = TY_UNKNOWN; return 1; }
    if (t == TY_VOID) t = TY_NIL;
    nil |= t == TY_NIL;
    acc = ty_unify(acc, t);
  }
  /* a path that answers nil (a bare `return`, an arm left out) adds no value to the walk, but it is one */
  if (any && n >= 0 && method_value_leaves(c, mi, lv, 16) < 0) { nil = 1; acc = ty_unify(acc, TY_NIL); }
  /* an Integer or a Float beside a nil is boxed: the nil flag of a call's slot is a mark that settles later */
  if (nil && (acc == TY_INT || acc == TY_FLOAT)) acc = TY_POLY;
  *out = acc;
  return any;
}

TyKind method_call_ret(Compiler *c, int mi, int call_id) {
  IMEMO_DEP_SCOPE(mi);   /* (the method's value is read here) */
  int last = scope_body_last(c, mi);
  /* `if block_given? ... yield ... else ... end`: the call with a block
     answers from the block arm, typed per call site like a yield tail */
  int tl = block_given_tail_then_last(c, last);
  if (tl >= 0) last = tl;
  int is_yield = last >= 0 && nt_kind(c->nt, last) == NK_YieldNode;
  /* A force-lowered Enumerable #each returns self (its ret is pinned to the
     defining class), not the block's value -- the per-call-site block typing
     below is only for the value-carrying (self-recursive) lowering. */
  if (c->scopes[mi].is_lowered_yield && ty_is_object(c->scopes[mi].ret))
    return c->scopes[mi].ret;
  /* Lowered because a yield sits in a lifted Thread/Fiber body: the method's
     value is its own tail (`t.value`), not the block's (#3355). */
  if (c->scopes[mi].lowered_lifted_yield) return c->scopes[mi].ret;
  /* Every remaining lowered method answers its own declared return, for one of
     two reasons that used to be separate arms:
       - its value is its OWN tail rather than the block's (`walk` ends in
         `nil`, not in a yield). Reading the block's type here made the caller
         discard a real answer as nil (#4145);
       - its value IS the block's, and one function serves every call site, so
         the signature is poly and the call site must read what the function
         returns rather than the per-site block type -- which held only while
         the raw carrier happened to fit, i.e. for scalars.
     The two conditions are exhaustive over is_lowered_yield, which is why the
     per-call-site arm below no longer names it. */
  if (c->scopes[mi].is_lowered_yield) return c->scopes[mi].ret;
  /* A tail that calls the &block param of a method that also uses the
     block as a value (`@last = block; block.call`): nothing splices the
     block, the call site passes a proc and reads what the one function
     returns, which sites with blocks of different types box */
  if (!is_yield && c->scopes[mi].blk_param_value_use && is_blk_param_call(c, last, mi))
    return c->scopes[mi].ret;
  /* A yield-tailed inlined method, or a tail that calls the &block param:
     the block is spliced per call site, so use that site's block body type. */
  if (is_yield || is_blk_param_call(c, last, mi) || ie_forwards_block_tail(c, last)) {
    int blk = nt_ref(c->nt, call_id, "block");
    /* `callee(&b)` / `callee(...)` forwards the block active in the enclosing
       method (a `...` forward carries it implicitly, with no block node), so
       the value `callee` yields is whatever that forwarded block produces --
       i.e. the enclosing method's own per-call-site yield value. */
    int fwd = nt_call_has_block_arg(c->nt, call_id);
    if (!fwd && blk < 0) fwd = yvt_call_forwards_block(c->nt, call_id);
    /* `callee(&lp)` with a proc of the call's own: the yield runs that
       proc, so the call answers its value, as the yield is typed. Read
       through the enclosing method's block, `lp = proc { "lp" }; m(&lp)`
       inside a method given `{ :blk }` answered a Symbol, and the String
       the proc returned was unboxed into it as an empty one. */
    if (fwd && !call_forwards_own_block(c, call_id)) return yvt_proc_arg_value(c, blk);
    if (fwd) {
      Scope *encl = comp_scope_of(c, call_id);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      if (emi >= 0 && emi != mi) {
        TyKind ft = yvt_forwarded_value(c, emi);
        TyKind nb = tl >= 0 && method_block_presence(c, emi) != 1 ? c->scopes[mi].ret_noblock : TY_UNKNOWN;
        if (ft != TY_UNKNOWN && ft != TY_VOID)
          return method_yield_ret(c, mi, nb == TY_UNKNOWN ? ft : ty_unify(ft, nb));
      }
    }
    if (blk >= 0) {
      int bbody = nt_ref(c->nt, blk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn > 0) {
        NodeKind lk = nt_kind(c->nt, bb[bn - 1]);
        if (lk == NK_ReturnNode)
          return method_yield_ret(c, mi, return_node_type(c, bb[bn - 1]));  /* `{ return e }`: see yield_value_type */
        /* A `next v` leaves the block with v, so the call answers v's type
           joined with the tail's, as yield_value_type joins them. Typed from
           the tail alone, `run { next true if c; nil }` was a nil call: `p`
           folded it to "nil", and with a String tail the boxed union was
           stored into a `const char *` and the C did not compile. */
        TyKind bt = infer_type(c, bb[bn - 1]);
        TyKind nx = block_next_value_ty(c, bbody);
        if (nx != TY_UNKNOWN) {
          if (bt == TY_VOID) bt = TY_NIL;
          bt = bt == TY_UNKNOWN ? nx : ty_unify(bt, nx);
        }
        return method_yield_ret(c, mi, bt);
      }
    }
  }
  /* a pinned `return enum_for(:m) unless block_given?` method called with
     a block of its own answers what the rest of its body does */
  if (c->scopes[mi].ret_pin_guard >= 0) {
    int blk = nt_ref(c->nt, call_id, "block");
    if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockNode && !nt_int(c->nt, blk, "fwd_yield", 0)) {
      TyKind t;
      if (pinned_block_form_ret(c, mi, &t)) return t;
    }
  }
  /* `return x unless block_given?`: a blockless call site reads the value
     of that return, one with a block the value of the body proper */
  if (c->scopes[mi].yields && c->scopes[mi].ret_noblock != TY_UNKNOWN) {
    int blk = nt_ref(c->nt, call_id, "block");
    int fwd = blk < 0 && yvt_call_forwards_block(c->nt, call_id);
    if (blk < 0 && !fwd) return c->scopes[mi].ret_noblock;
    /* `m(&f)` with a proc value: whether there is a block is decided at run
       time (the proc may be nil), so both arms are live and the call
       answers their union. Typed from the block arm alone, the else arm's
       Enumerator landed in the block arm's array slot and the C did not
       build (a boxed receiver's `filter_map(&f)`). */
    if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockArgumentNode)
      return ty_unify(c->scopes[mi].ret, c->scopes[mi].ret_noblock);
  }
  /* the `{ |__fwd| yield __fwd }` a forwarded `&b` became: the enclosing
     method's own calls decide which arm runs, and when they differ the call
     answers the union (`def g(&b) = a.take_while(&b)` called both ways
     answers an Array or an Enumerator). A node's type only ever derives
     from unknown, so until the else arm is typed the answer waits rather
     than settling on the block arm's. */
  if (c->scopes[mi].yields) {
    int blk = nt_ref(c->nt, call_id, "block");
    if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockNode && nt_int(c->nt, blk, "fwd_yield", 0)) {
      Scope *encl = comp_scope_of(c, call_id);
      int up = encl ? method_block_presence(c, (int)(encl - c->scopes)) : -1;
      if (up != 1) {
        if (c->scopes[mi].ret_noblock == TY_UNKNOWN) return TY_UNKNOWN;
        if (up == 0) return c->scopes[mi].ret_noblock;
        return ty_unify(c->scopes[mi].ret, c->scopes[mi].ret_noblock);
      }
    }
  }
  return c->scopes[mi].ret;
}
int is_proc_constant(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  const char *ty = nt_type(nt, n);
  NodeKind k = nt_kind(nt, n);
  if (!ty) return 0;
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) {
    const char *nm = nt_str(nt, n, "name");
    return nm && sp_streq(nm, "Proc");
  }
  return 0;
}
int is_proc_literal(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  if (k != NK_CallNode) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 && name && (is_proc_constructor(name))) return 1;
  if (recv >= 0 && name && sp_streq(name, "new") && is_proc_constant(nt, recv)) return 1;
  return 0;
}
/* `Hash.new { |h, k| ... }`, whose block codegen emits as a bare C function
   taking only the hash and the key (emit_class_new_call). */
static int is_hash_new_block(const NodeTable *nt, int id) {
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  return name && sp_streq(name, "new") && recv >= 0 &&
         nt_kind(nt, recv) == NK_ConstantReadNode &&
         nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash");
}
static int hash_new_body_needs_frame(Compiler *c, int id, const char *hp, const char *kp) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  NodeKind ty_k = nt_kind(nt, id);
  if (!ty) return 0;
  if (ty_k == NK_LocalVariableReadNode || ty_k == NK_LocalVariableWriteNode ||
      ty_k == NK_LocalVariableTargetNode || ty_k == NK_LocalVariableOperatorWriteNode ||
      ty_k == NK_LocalVariableOrWriteNode || ty_k == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !((hp && sp_streq(nm, hp)) || (kp && sp_streq(nm, kp)))) return 1;
  }
  if (ty_k == NK_LambdaNode) return 1;
  /* a nested default block that stays bare is a function of its own, its
     parameters its own; one that does not is a proc made here */
  if (is_hash_new_block(nt, id)) return hash_new_block_is_proc(c, id);
  if (ty_k == NK_CallNode && a_proc_create_or_lifted(c, id)) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (hash_new_body_needs_frame(c, nt_ref_at(nt, id, i), hp, kp)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (hash_new_body_needs_frame(c, ids[k], hp, kp)) return 1;
  }
  return 0;
}
/* A `return` of the default block itself: not one of a def, a lambda, a proc
   or another default block written in it. */
static int hash_new_body_returns(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_ReturnNode) return 1;
  if (k == NK_DefNode || k == NK_LambdaNode || is_proc_literal(c, id) || is_hash_new_block(nt, id)) return 0;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (hash_new_body_returns(c, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) if (hash_new_body_returns(c, ids[j])) return 1;
  }
  return 0;
}
/* Marks the Hash.new calls written in a def's own statements, with no block,
   lambda or class body between: there the frame that makes the hash is the
   method's. */
static void mark_hash_new_in_def(const NodeTable *nt, int id, char *in_def) {
  if (id < 0 || id >= nt->count) return;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_DefNode || k == NK_LambdaNode || k == NK_BlockNode || k == NK_ClassNode || k == NK_ModuleNode ||
      k == NK_SingletonClassNode) return;
  if (is_hash_new_block(nt, id)) in_def[id] = 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) mark_hash_new_in_def(nt, nt_ref_at(nt, id, i), in_def);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) mark_hash_new_in_def(nt, ids[j], in_def);
  }
}
static int hash_new_in_def(Compiler *c, int id) {
  static const Compiler *memo_c; static char *in_def; static int memo_n;
  const NodeTable *nt = c->nt;
  if (memo_c != c || memo_n != nt->count) {
    free(in_def); in_def = calloc(nt->count ? nt->count : 1, 1);
    memo_c = c; memo_n = nt->count;
    for (int d = 0; d < nt->count; d++)
      if (nt_kind(nt, d) == NK_DefNode) mark_hash_new_in_def(nt, nt_ref(nt, d, "body"), in_def);
  }
  return in_def[id];
}
/* A `Hash.new { |h, k| ... }` block that is lowered to a real sp_Proc, which
   the hash's default calls through sp_dyn_hash_dproc. The bare function
   declares nothing but the hash and the key: a local the block writes, the
   parameter of an iteration block it runs, an enclosing local it reads, and a
   proc it makes (which captures through cells the function never had) all
   named identifiers it did not declare, and a proc literal's own function was
   written into the middle of it. Those blocks take the proc machinery, the
   way a deferred handler does; a block reading only its two parameters keeps
   the bare function. */
int hash_new_block_is_proc(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!is_hash_new_block(nt, id)) return 0;
  int blk = nt_ref(nt, id, "block");
  /* a `return` leaves the method that made the hash, which only a proc with
     a home can do: in the bare function it ended the block. Taken where the
     hash is made in the method's own frame. */
  if (hash_new_in_def(c, id) && hash_new_body_returns(c, nt_ref(nt, blk, "body"))) return 1;
  return hash_new_body_needs_frame(c, nt_ref(nt, blk, "body"),
                                   block_param_name(c, blk, 0), block_param_name(c, blk, 1));
}
/* A block lowered to a REAL sp_Proc by its consumer -- a deferred handler
   (at_exit / Signal.trap, #2836), an ENV block mutator (#2832) or a Hash.new
   default block that needs a frame of its own (hash_new_block_is_proc). These
   need the full proc capture machinery but keep their own call-level
   inference (trap returns the previous handler, not the proc). */
int is_handler_proc_block(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  if (k != NK_CallNode) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  if (hash_new_block_is_proc(c, id)) return 1;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  if (sp_streq(name, "at_exit") || sp_streq(name, "trap")) {
    if (recv < 0) return 1;
    NodeKind hrk = nt_kind(nt, recv);
    return hrk == NK_ConstantReadNode &&
    nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Signal");
  }
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "ENV") &&
      is_select_bang(name))
    return 1;
  return 0;
}
int is_proc_create(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_LambdaNode) return 1;
  return is_proc_literal(c, id);
}
/* Unify the value types of `return <expr>` nodes lexically inside a proc body,
   without descending into a nested scope (a def/class/module, or a nested block
   or lambda -- whose returns are their own or non-local). Used to widen a proc
   whose paths return different types to TY_POLY, so the .call site reads the
   boxed return slot instead of trusting one path's scalar type. */
static TyKind proc_interior_return_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  NodeKind ty_k = nt_kind(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (ty_k == NK_ReturnNode) return return_node_type(c, node);
  /* A nested def/class/module/lambda owns its own returns; stop there. A plain
     block is return-transparent -- a `return` lexically inside a `do..end`
     block returns non-locally from the enclosing proc/lambda, so its type must
     fold in (#3241). A block that IS a nested proc literal's block owns its own
     frame, so it is not descended into. */
  if (ty_k == NK_DefNode || ty_k == NK_ClassNode ||
      ty_k == NK_ModuleNode || ty_k == NK_LambdaNode) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    if (ch >= 0 && nt_kind(nt, ch) == NK_BlockNode &&
        is_proc_literal(c, node)) continue;   /* a nested proc's own block */
    TyKind s = proc_interior_return_ty(c, ch);
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = proc_interior_return_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
}
TyKind proc_node_ret(Compiler *c, int create) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, create);
  int body;
  if (k == NK_LambdaNode) body = nt_ref(nt, create, "body");
  else {
    int blk = nt_ref(nt, create, "block");
    /* `Proc.new(&b)` / `proc(&b)`: no body of its own -- the value is the
       forwarded proc, whose return is unknowable here (a block param's
       actual block lives at the caller). TY_UNKNOWN maps to the boxed poly
       return at the .call site instead of a silent nil. */
    if (nt_call_has_block_arg(nt, create))
      return TY_UNKNOWN;
    body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
  }
  if (body < 0) return TY_NIL;
  int bn = 0;
  const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn <= 0) return TY_NIL;
  /* An explicit `return <expr>` tail is a ReturnNode, which infer_type does not
     value-type -- type the proc by the returned expression so the call site and
     the body's return channel agree (otherwise the value is silently lost). */
  int tail = bb[bn - 1];
  NodeKind tk = nt_kind(nt, tail);
  TyKind tail_ty = (tk == NK_ReturnNode) ? return_node_type(c, tail)
                                                        : infer_type(c, tail);
  /* Widen to the unification of every exit's type: a proc returning different
     types on different paths (`return "x" if c; n`) must be POLY so the .call
     site reads the boxed slot rather than mis-unboxing one path's scalar type
     (silent-wrong under the universal boxed return ABI). A block-level `next`/
     `break <expr>` carries a value out of the proc too (loop-bound ones bind to
     the loop, not the proc -- ie_block_break_next_ty stops at nested loops and
     scopes), so fold their types in as well. */
  TyKind ir = proc_interior_return_ty(c, body);
  TyKind bnt = ie_block_break_next_ty(c, body);
  if (bnt != TY_UNKNOWN) ir = (ir == TY_UNKNOWN) ? bnt : ty_unify(ir, bnt);
  if (ir != TY_UNKNOWN && tail_ty != TY_UNKNOWN && ir != tail_ty)
    return ty_unify(tail_ty, ir);
  return tail_ty;
}
TyKind proc_ret_of(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  NodeKind k = nt_kind(nt, node);
  if (!ty) return TY_UNKNOWN;
  /* unwrap `(expr)` so `(f << g).call` sees the composition node */
  if (k == NK_ParenthesesNode) {
    int body = nt_ref(nt, node, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn == 1 ? proc_ret_of(c, bb[0]) : TY_UNKNOWN;
  }
  if (k == NK_LambdaNode || is_proc_literal(c, node)) return proc_node_ret(c, node);
  if (k == NK_LocalVariableReadNode) {
    Scope *s = comp_scope_of(c, node);
    LocalVar *lv = scope_local(s, nt_str(nt, node, "name"));
    return lv ? (TyKind)lv->proc_ret : TY_UNKNOWN;
  }
  if (k == NK_CallNode) {
    /* a method call that returns a proc -> the callee's recorded proc return */
    int recv = nt_ref(nt, node, "receiver");
    const char *name = nt_str(nt, node, "name");
    /* Hash#to_proc: the proc maps a key to the hash's value type. */
    if (recv >= 0 && name && sp_streq(name, "to_proc")) {
      TyKind rt = infer_type(c, recv);
      if (ty_is_hash(rt)) return ty_hash_val(rt);
    }
    /* proc << proc / proc >> proc: the composed call returns the OUTER proc's
       value (f<<g outer=f; f>>g outer=g). */
    if (recv >= 0 && name && infer_type(c, recv) == TY_PROC) {
      int args = nt_ref(nt, node, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an == 1 && infer_type(c, av[0]) == TY_PROC) {
        if (sp_streq(name, "<<")) return proc_ret_of(c, recv);
        if (sp_streq(name, ">>")) return proc_ret_of(c, av[0]);
      }
    }
    int mi = -1;
    if (recv < 0) mi = comp_self_call_mi(c, node, name);
else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = call_target(c, node, ty_object_class(rt), name, 0, NULL);
    }
    if (mi >= 0) return (TyKind)c->scopes[mi].ret_proc_ret;
  }
  return TY_UNKNOWN;
}
/* Resolve a RECEIVERLESS call to the user method it lands in, or -1.
   Ruby resolves it against the enclosing definition's self: a class method
   against the singleton chain, an instance method against the instance chain.
   A top-level `def` is a private method on Object, so it sits at the BOTTOM of
   every ancestry and is the last thing to try. Several passes asked for the
   free functions FIRST, so a same-named top-level def stood in for the real
   callee -- and since it is typed by its own body, the real callee's
   parameters never saw the call's argument types (#4106, #4130). */
/* The instance method a class's own self reaches by `name`, or -1: a class
   is an instance of Class, so of the instance methods on its chain only one
   Object, BasicObject, Kernel, Module or Class gives it is the class's; the
   rest (an included module's, the class's own) are its instances'. Asked
   for call `call` (call_target). */
int comp_class_self_inst_mi(Compiler *c, int call, int class_id, const char *name) {
  static const char *const OBJ_SIDE[] = { "Object", "BasicObject", "Kernel", "Module", "Class", NULL };
  int dc = -1;
  int mi = call_target(c, call, class_id, name, 0, &dc);
  return mi >= 0 && dc >= 0 && c->classes[dc].name && str_in(c->classes[dc].name, OBJ_SIDE) ? mi : -1;
}
/* See compiler.h. A builtins/ file's helper is known by its def's stamp
   (node_bi) and its name, a desugar's copy by the mark the desugar sets on
   it: a copy of a program's own Integer or Enumerable method has no stamp,
   and its name is one the program could have written. */
int comp_scope_is_private_helper(Compiler *c, int si) {
  const Scope *s = si >= 0 && si < c->nscopes ? &c->scopes[si] : NULL;
  if (!s || !s->name || s->def_node < 0) return 0;
  if (nt_int(c->nt, s->def_node, "private_helper", 0)) return 1;
  return nt_int(c->nt, s->def_node, "node_bi", 0) != 0 && is_builtin_helper_name(s->name);
}
int comp_name_is_private_helper(Compiler *c, const char *m) {
  if (!m || m[0] != '_' || m[1] != '_') return 0;
  int helper = 0;
  for (int s = an_scopes_named_first(c, m); s >= 0; s = an_scopes_named_next(c, m, s)) {
    if (!comp_scope_is_private_helper(c, s)) return 0;
    helper = 1;
  }
  return helper;
}
/* See compiler.h. The method an instance of `cid` reaches by `m` (its
   chain, then the top-level def every object has as a private method),
   for `cid` < 0 the top-level def alone: one name can be a builtin's
   helper on one class (ThreadGroup's `__remove`) and a program's method on
   another, so the answer is the resolved method's, not the name's. */
int comp_method_is_private_helper(Compiler *c, int cid, const char *m) {
  if (!m || m[0] != '_' || m[1] != '_') return 0;
  /* the member an instance reaches: a reader the program declares nearer
     than the helper is its own (comp_resolve_member) */
  int mi = -1;
  if (cid >= 0 && comp_resolve_member(c, cid, m, 0, NULL, &mi) == SP_MEMBER_ATTR) return 0;
  for (int s = mi < 0 ? an_scopes_named_first(c, m) : -1; s >= 0 && mi < 0; s = an_scopes_named_next(c, m, s))
    if (c->scopes[s].class_id < 0 && !c->scopes[s].is_cmethod && !c->scopes[s].is_proc_form) mi = s;
  return comp_scope_is_private_helper(c, mi);
}
/* Compiler-synthesized methods that must stay invisible to reflection
   (respond_to?, methods, instance_methods, method_defined?) and to a send
   with a computed name: they are implementation detail with no CRuby
   counterpart. Matched by exact name, or by the definition's mark, so a
   user-defined `__foo` stays visible. */
int name_is_synth_method(Compiler *c, const char *m) {
  if (!m) return 0;
  if (comp_name_is_private_helper(c, m)) return 1;
  if (sp_streq(m, "__enum_to_a")) return 1;
  /* a class's copy of its initialize as a method, for its own calls of it
     (desugar_explicit_initialize_calls) */
  if (sp_streq(m, "__spinel_reinit")) return 1;
  /* `__noarg inspect` / `__noarg to_s`: the no-argument siblings of an
     inspect / to_s whose parameters are all optional
     (desugar_noarg_display_wrappers); the space is no `def`'s, so a method
     the program names `__noarg_inspect` is its own */
  if (strncmp(m, "__noarg ", 8) == 0) return 1;
  /* `__inc <n> <name>`: the shadow copy of an included module's method, kept
     so an override can reach it through super (#3738) */
  if (strncmp(m, "__inc ", 6) == 0) return 1;
  /* `m#pf`, the proc-form clone of a yielding method: `#` is never part of a
     Ruby method name */
  if (strchr(m, '#')) return 1;
  /* `__to_enum_<m>`, the Enumerator generator for `to_enum(:m)`
     (synth_to_enum_generators): it shares the DefNode of the method it
     wraps, which a user method of that name never does */
  if (strncmp(m, "__to_enum_", 10) == 0) {
    for (int si = 0; si < c->nscopes; si++) {
      Scope *s = &c->scopes[si];
      if (!s->name || !sp_streq(s->name, m) || s->def_node < 0) continue;
      for (int sj = 0; sj < c->nscopes; sj++)
        if (c->scopes[sj].name && sp_streq(c->scopes[sj].name, m + 10) &&
            c->scopes[sj].def_node == s->def_node) return 1;
    }
    return 0;
  }
  /* a parameter default moved into a method of its own
     (desugar_recursive_param_defaults, #4900): known by the mark on its
     DefNode rather than its name, which the desugar picks so that no method
     the program defines has it */
  if (strncmp(m, "__sp_default_", 13) != 0) return 0;
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->def_node >= 0 && s->name && sp_streq(s->name, m) &&
        nt_int(c->nt, s->def_node, "default_helper", 0)) return 1;
  }
  return 0;
}

int comp_self_call_mi(Compiler *c, int id, const char *name) {
  if (!name || nt_int(c->nt, id, "kernel_recv", 0)) return -1;
  Scope *self = comp_scope_of(c, id);
  int mi = -1;
  if (self && self->class_id >= 0) {
    /* inside a class method self IS the class, so a bare call reaches sibling
       class methods, not the class's instance methods; inside an instance
       method it does not reach class methods */
    if (self->is_cmethod) {
      mi = call_target(c, id, self->class_id, name, CT_CMETHOD, NULL);
      if (mi < 0) mi = comp_class_self_inst_mi(c, id, self->class_id, name);
    }
    else mi = call_target(c, id, self->class_id, name, 0, NULL);
  }
  if (mi < 0) mi = call_target(c, id, -1, name, CT_TOP, NULL);
  return mi;
}

int send_blind_recv_owns(Compiler *c, int call, int recv, TyKind srt, const char *name) {
  if (ty_is_object(srt))
    return call_target(c, call, ty_object_class(srt), name, 0, NULL) >= 0 ||
           comp_reader_in_chain(c, ty_object_class(srt), name, NULL);
  /* a class named by a constant (or `self.class`): its own class methods
     come before Object's private top-level def */
  NodeKind rk = nt_kind(c->nt, recv);
  int ci = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode
           ? comp_class_index(c, nt_str(c->nt, recv, "name")) : self_class_static_ci(c, recv);
  if (ci >= 0 && call_target(c, call, ci, name, CT_CMETHOD, NULL) >= 0) return 1;
  if (srt == TY_POLY) {
    /* any class's chain, from the by-name lists while the chain memo can
       be trusted, class by class otherwise */
    int nr;
    comp_name_responders(c, name, 0, &nr);
    if (nr > 0) return 1;
    if (nr == 0) return comp_name_reader_any(c, name);
    for (int k = 0; k < c->nclasses; k++)
      if (call_target(c, call, k, name, 0, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL)) return 1;
    return 0;
  }
  const char *bc = srt == TY_INT ? "Integer" : srt == TY_FLOAT ? "Float"
                 : (srt == TY_STRING || srt == TY_STRBUF) ? "String" : srt == TY_SYMBOL ? "Symbol"
                 : ty_is_array(srt) ? "Array" : ty_is_hash(srt) ? "Hash"
                 : srt == TY_RANGE ? "Range" : srt == TY_PROC ? "Proc" : srt == TY_TIME ? "Time" : NULL;
  return bc && (builtin_method_known(bc, name) || builtin_module_owns(bc, name));
}

/* A receiverless call directly in a class body is sent to the class. */
int comp_cbody_call_mi(Compiler *c, int id, const char *name) {
  Scope *s = comp_scope_of(c, id);
  if (!name || (s && s->name) || c->node_cbody[id] < 0) return -1;
  return call_target(c, id, c->node_cbody[id], name, CT_CMETHOD, NULL);
}

TyKind proc_call_ret(Compiler *c, int recv) {
  TyKind r = proc_ret_of(c, recv);
  if (r == TY_UNKNOWN && g_infer_optimistic) return TY_UNKNOWN;
  return r == TY_UNKNOWN ? TY_POLY : r;
}

/* The symbol-name argument of a `method(:sym)` call, or NULL. */
const char *method_sym_arg(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  /* <method>.super_method names the same method (in the parent class) */
  if (node >= 0 && nt_kind(nt, node) == NK_CallNode && nt_str(nt, node, "name") &&
      sp_streq(nt_str(nt, node, "name"), "super_method")) {
    int imn = method_recv_node(c, nt_ref(nt, node, "receiver"));
    return imn >= 0 ? method_sym_arg(c, imn) : NULL;
  }
  int args = nt_ref(nt, node, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 1) return NULL;
  NodeKind ak = nt_kind(nt, av[0]);
  if (ak == NK_SymbolNode) return nt_str(nt, av[0], "value");
  if (ak == NK_StringNode) {
    const char *s = nt_str(nt, av[0], "content");
    return s ? s : nt_str(nt, av[0], "unescaped");
  }
  return NULL;
}

/* True if `node` is a `method(:sym)` / `<recv>.method(:sym)` call. */
int is_method_obj_call(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, node, "name");
  /* Module#instance_method builds the same object, unbound (NULL self); the
     Method machinery (name/arity/owner/call-after-bind) rides it (#2676) */
  return nm && (sp_streq(nm, "method") || sp_streq(nm, "instance_method")) &&
         method_sym_arg(c, node) != NULL;
}

/* The target method scope index bound by a `method(:sym)` node, or -1
   (e.g. a top-level Kernel method like `puts`, or a builtin-array receiver). */
static int method_obj_target_mi_raw(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  /* <method>.super_method: the same-named method one step up the defining
     class's ancestor chain, or -1 (nil) when there is none (#3247). */
  if (node >= 0 && nt_kind(nt, node) == NK_CallNode && nt_str(nt, node, "name") &&
      sp_streq(nt_str(nt, node, "name"), "super_method")) {
    int imn = method_recv_node(c, nt_ref(nt, node, "receiver"));
    int imi = imn >= 0 ? method_obj_target_mi_raw(c, imn) : -1;
    if (imi < 0 || c->scopes[imi].class_id < 0) return -1;
    int par = c->classes[c->scopes[imi].class_id].parent;
    if (par < 0) return -1;
    return c->scopes[imi].is_cmethod
             ? comp_cmethod_in_chain(c, par, c->scopes[imi].name, NULL)
             : comp_method_in_chain(c, par, c->scopes[imi].name, NULL);
  }
  const char *sym = method_sym_arg(c, node);
  if (!sym) return -1;
  int recv = nt_ref(nt, node, "receiver");
  /* Klass.instance_method(:m): the class's INSTANCE method (Klass.method(:m)
     would be the class method) */
  {
    const char *nm0 = nt_str(nt, node, "name");
    if (nm0 && sp_streq(nm0, "instance_method")) {
      const char *rn = (recv >= 0 && (nt_kind(nt, recv) == NK_ConstantReadNode ||
                                      nt_kind(nt, recv) == NK_ConstantPathNode))
                       ? nt_str(nt, recv, "name") : NULL;
      int ci = rn ? comp_class_index(c, rn) : -1;
      return ci >= 0 ? comp_method_in_chain(c, ci, sym, NULL) : -1;
    }
  }
  /* a bare `method(:m)` names what a bare `m` would call: inside a class
     method self is the class, so its class methods come first (Ruby's
     method lookup, comp_self_call_mi) */
  if (recv < 0) return comp_self_call_mi(c, node, sym);
  /* `self.method(:m)` in a class method: self is the class */
  if (nt_kind(nt, recv) == NK_SelfNode) {
    Scope *ss = comp_scope_of(c, node);
    if (ss && ss->is_cmethod && ss->class_id >= 0)
      return comp_cmethod_in_chain(c, ss->class_id, sym, NULL);
  }
  TyKind rt = infer_type(c, recv);
  /* a receiver whose method() was retargeted at a synthesized __bam_* wrapper
     (desugar_builtin_method_obj) -- for an object accessor the wrapper is a
     TOP-LEVEL def, not in the object's own class chain, so resolve it there. */
  if (sym[0] == '_' && sym[1] == '_' && sym[2] == 'b' && sym[3] == 'a' && sym[4] == 'm')
    return comp_method_index(c, sym);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), sym, NULL);
  /* Klass.method(:cmeth) / Module.method(:mf): the class-side method */
  if (rt == TY_CLASS && (nt_kind(nt, recv) == NK_ConstantReadNode ||
                         nt_kind(nt, recv) == NK_ConstantPathNode)) {
    /* `Outer::Mod.method(:m)` names the class by its last segment, as every
       other constant receiver resolves */
    const char *rn2 = nt_str(nt, recv, "name");
    int ci2 = rn2 ? comp_class_index(c, rn2) : -1;
    if (ci2 >= 0) return comp_cmethod_in_chain(c, ci2, sym, NULL);
  }
  /* `self.class.method(:cmeth)` in an instance method of a class nothing
     inherits from: that class's class-side method */
  if (rt == TY_CLASS) {
    int ci3 = self_class_static_ci(c, recv);
    if (ci3 >= 0) return comp_cmethod_in_chain(c, ci3, sym, NULL);
  }
  return -1;
}

/* A yielding method is spliced into each call site and has no function of
   its own; a Method object calls it through its proc form, the clone whose
   `yield` is a call on the block the Method passes (make_yield_proc_forms
   makes one for every method a `method(:name)` names). */
int method_obj_target_mi(Compiler *c, int node) {
  int mi = method_obj_target_mi_raw(c, node);
  /* a lowering's own helper is no method a Method can name */
  if (comp_scope_is_private_helper(c, mi)) return -1;
  int pf = scope_proc_form_of(c, mi);
  return pf >= 0 ? pf : mi;
}

/* The method name of `k.instance_method(:m)` that names no one target, or
   NULL: k is a Class value only the run time knows (a parameter, a local
   written more than one class, a poly slot), which method_obj_target_mi does
   not resolve (a local holding one class is retargeted to its constant), or
   a user class constant with no `m` at all. A bind_call on it dispatches on
   the class (class_value_bind_call_target), and a class without `m` raises
   NameError. A name every object answers (`to_s`, `inspect`, ...) is left
   alone: a class without its own is not a class without it. */
const char *class_value_instance_method_sym(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return NULL;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || !sp_streq(nm, "instance_method") || method_obj_target_mi(c, node) >= 0) return NULL;
  int recv = nt_ref(nt, node, "receiver");
  const char *sym = method_sym_arg(c, node);
  if (recv < 0 || !sym || builtin_object_method_known(sym)) return NULL;
  TyKind rt = infer_type(c, recv);
  if (rt == TY_POLY || (rt == TY_CLASS && class_recv_is_dynamic(c, recv))) return sym;
  int ci = rt == TY_CLASS ? class_recv_static_ci(c, recv) : -1;
  return ci >= 0 && !is_builtin_reopen(c->classes[ci].name) ? sym : NULL;
}

/* The instance method that class `ci` binds for such a bind_call, or -1 when
   `ci` has no `sym`. A class held by value and a module (whose method binds
   any object) have no arm, nor has a target taking a block, which the arms
   do not pass: *armless is set for them. */
int class_value_bind_call_target(Compiler *c, int ci, const char *sym, int *armless) {
  if (ci < 0 || ci >= c->nclasses || is_builtin_reopen(c->classes[ci].name)) return -1;
  int mi = comp_method_in_chain(c, ci, sym, NULL);
  if (mi < 0) return -1;
  const ClassInfo *k = &c->classes[ci];
  Scope *tm = &c->scopes[mi];
  if (k->is_value_type || (k->def_node >= 0 && nt_kind(c->nt, k->def_node) == NK_ModuleNode) ||
      tm->yields || (tm->blk_param && tm->blk_param[0])) {
    if (armless) *armless = 1;
    return -1;
  }
  return mi;
}

/* Whether a bind_call on such a node (class_value_instance_method_sym) meets
   a class it has no arm for: a user class the arms skip, a builtin the
   program reopens with the method, or, for a Class value the run time
   picks, a core class's method of the name, which a builtin class held in
   it would answer. The call is refused rather than raise NameError there. */
int class_value_bind_call_gap(Compiler *c, int node) {
  const char *sym = class_value_instance_method_sym(c, node);
  if (!sym) return 0;
  int armless = 0;
  for (int k = 0; k < c->nclasses && !armless; k++) {
    if (is_builtin_reopen(c->classes[k].name)) armless = comp_method_in_chain(c, k, sym, NULL) >= 0;
    else (void)class_value_bind_call_target(c, k, sym, &armless);
  }
  int recv = nt_ref(c->nt, node, "receiver");
  return armless || (core_method_name(sym) && class_recv_static_ci(c, recv) < 0);
}

/* The Ruby return kind of a typed-array adapter Method (`<array>.method(:op)`)
   with no target scope: IntArray `push` answers the array, `[]`/`[]=` the int
   element; StrArray `push` the array, `[]`/`[]=` the String element; anything
   else is TY_UNKNOWN (not an adapter). The analyzer's inferred type and the
   codegen's stamped SP_BM_RET_* must not drift -- both read this helper. */
TyKind method_obj_adapter_ret(TyKind arr, const char *op) {
  if (!op) return TY_UNKNOWN;
  if (arr == TY_INT_ARRAY) {
    if (sp_streq(op, "push")) return TY_INT_ARRAY;
    if (is_element_access(op)) return TY_INT;
  }
  else if (arr == TY_STR_ARRAY) {
    if (sp_streq(op, "push")) return TY_STR_ARRAY;
    if (is_element_access(op)) return TY_STRING;
  }
  return TY_UNKNOWN;
}

/* Is this Method-typed expression an UNBOUND method (Klass.instance_method
   with no #bind crossed)? Resolves the same way method_recv_node does, but a
   bind on the path means the value is bound (#2724). */
int method_expr_is_unbound(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return 0;
  if (nt_kind(nt, recv) == NK_CallNode) {
    const char *nm = nt_str(nt, recv, "name");
    if (nm && sp_streq(nm, "bind")) return 0;
    if (nm && sp_streq(nm, "instance_method") && method_sym_arg(c, recv) != NULL) return 1;
    if (nm && sp_streq(nm, "unbind")) return 1;
    if (nm && sp_streq(nm, "super_method"))
      return method_expr_is_unbound(c, nt_ref(nt, recv, "receiver"));
    return 0;
  }
  if (nt_kind(nt, recv) == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = vn ? comp_scope_of(c, recv) : NULL;
    int found = 0, n = 0;
    for (int w = 0; vn && w < nt->count; w++) {
      if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      n++;
      if (method_expr_is_unbound(c, nt_ref(nt, w, "value"))) found = 1;
    }
    return n == 1 && found;   /* a re-written local is dynamic: stay bound-ish */
  }
  return 0;
}

/* Do two `method(:sym)` nodes bind the same method the same way: the one
   target, both with or both without a receiver, and a constant receiver
   (the class a class method runs on) the same constant? */
static int method_nodes_same_target(Compiler *c, int a, int b) {
  const NodeTable *nt = c->nt;
  const char *na = nt_str(nt, a, "name"), *nb = nt_str(nt, b, "name");
  if (!na || !nb || !sp_streq(na, nb)) return 0;
  int ra = nt_ref(nt, a, "receiver"), rb = nt_ref(nt, b, "receiver");
  if ((ra < 0) != (rb < 0)) return 0;
  int ca = ra >= 0 && (nt_kind(nt, ra) == NK_ConstantReadNode || nt_kind(nt, ra) == NK_ConstantPathNode);
  int cb = rb >= 0 && (nt_kind(nt, rb) == NK_ConstantReadNode || nt_kind(nt, rb) == NK_ConstantPathNode);
  if (ca != cb) return 0;
  if (ca) {
    const char *sa = nt_str(nt, ra, "name"), *sb = nt_str(nt, rb, "name");
    if (!sa || !sb || !sp_streq(sa, sb)) return 0;
  }
  int ta = method_obj_target_mi(c, a);
  return ta >= 0 && ta == method_obj_target_mi(c, b);
}

/* The `method(:sym)` node a Method-typed expression resolves to: either the
   call itself (inline) or, for a local variable, its assignment in scope. */
int method_recv_node(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  if (is_method_obj_call(c, recv)) return recv;
  /* UnboundMethod#bind(obj) re-binds the same target: see through it (#2676).
     dup/clone of a method are identity copies: see through them too (#3247). */
  if (nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "bind") ||
       sp_streq(nt_str(nt, recv, "name"), "dup") ||
       sp_streq(nt_str(nt, recv, "name"), "clone") ||
       /* #unbind names the same target with the receiver dropped, so the
          UnboundMethod it answers resolves like the Method it came from
          (#3658) */
       sp_streq(nt_str(nt, recv, "name"), "unbind")))
    return method_recv_node(c, nt_ref(nt, recv, "receiver"));
  /* a super_method call IS a method node: method_obj_target_mi resolves it
     through the parent chain, so hand it back as-is (#3247) */
  if (nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "name") &&
      sp_streq(nt_str(nt, recv, "name"), "super_method") &&
      method_recv_node(c, nt_ref(nt, recv, "receiver")) >= 0)
    return recv;
  NodeKind rk = nt_kind(nt, recv);
  static int depth;
  static const char *active_vn[32];
  static Scope *active_sc[32];
  if (rk == NK_LocalVariableReadNode && depth < 32) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    /* a write reading the local it writes (`m = m.dup`) says nothing new */
    for (int d = 0; d < depth; d++)
      if (active_sc[d] == sc && vn && active_vn[d] && sp_streq(active_vn[d], vn)) return -2;
    /* Every write must name the same method: the static binding (self-ful
       or self-less, the target's signature) is the one node's, and a local
       re-written to another Method (`m = A.new.method(:x); m = method(:top)`)
       or to a value of no such node took the first write's, calling the
       second through the first's ABI (a garbage argument, or a crash).
       Such a local answers -1, and its call dispatches on the Method value
       at run time. A write of the same target again (a loop's
       `m = o.method(:x)`, `m = m.dup`) keeps the node. */
    int found = -1;
    for (int w = an_local_writes_named_first(c, vn); w >= 0; w = an_local_writes_named_next(w)) {
      if (comp_scope_of(c, w) != sc) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !vn || !sp_streq(wn, vn)) continue;
      int val = nt_ref(nt, w, "value");
      /* resolve the written expression with the same rules as a direct
         receiver (sees through bind/dup/clone chains, super_method) */
      active_vn[depth] = vn; active_sc[depth] = sc;
      depth++;
      int inner = method_recv_node(c, val);
      depth--;
      if (inner == -2) continue;
      if (inner < 0) return -1;
      if (found < 0) found = inner;
      else if (inner != found && !method_nodes_same_target(c, found, inner)) return -1;
    }
    return found;
  }
  return -1;
}

/* Append mn to the growing list method_recv_nodes answers. */
static void mrn_push(int **out, int *n, int *cap, int mn) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    *out = realloc(*out, sizeof(int) * (size_t)*cap);
    if (!*out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*out)[(*n)++] = mn;
}

/* Every `method(:sym)` node a Method-typed local may hold: one per write
   that names one, where method_recv_node declines a local written to more
   than one method. Each target is called with the call's arguments, so the
   analysis binds them to all of them. Answers the count, the nodes in a
   malloc'd *out the caller frees (NULL for none). The list holds every
   target, however many writes there are: cut at a fixed eight, a ninth was
   bound nothing (its parameter typed Integer by default or by a direct
   call, so the String a call passed it raised TypeError), and a bound
   builtin's wrapper there was counted no call, so the call raised
   NoMethodError. */
int method_recv_nodes(Compiler *c, int recv, int **out) {
  const NodeTable *nt = c->nt;
  int n = 0, cap = 0;
  *out = NULL;
  int one = method_recv_node(c, recv);
  if (one >= 0) { mrn_push(out, &n, &cap, one); return n; }
  if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) return 0;
  const char *vn = nt_str(nt, recv, "name");
  Scope *sc = comp_scope_of(c, recv);
  for (int w = an_local_writes_named_first(c, vn); w >= 0; w = an_local_writes_named_next(w)) {
    if (comp_scope_of(c, w) != sc) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !vn || !sp_streq(wn, vn)) continue;
    int mn = method_recv_node(c, nt_ref(nt, w, "value"));
    if (mn >= 0) mrn_push(out, &n, &cap, mn);
  }
  return n;
}

/* Every `method(:sym)` node behind a Proc-typed expression created by
   `<method>.to_proc`: the to_proc call itself, or a local variable's
   single assignment to one. The Method it converts may be a local written
   to more than one method; each target is called with the proc's
   arguments, so the analysis binds them to all of them, as it does for a
   `.call` on that local (method_recv_nodes). Answers the count, the nodes
   in a malloc'd *out the caller frees; 0 and NULL when the proc has
   another origin. */
int proc_to_proc_method_nodes(Compiler *c, int recv, int **out) {
  const NodeTable *nt = c->nt;
  *out = NULL;
  if (recv < 0) return 0;
  int cand = recv;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_LocalVariableReadNode) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    cand = -1;
    /* the writes only, through the kind index (the first one that is a call) */
    for (int w = an_local_writes_named_first(c, vn); w >= 0; w = an_local_writes_named_next(w)) {
      if (cand >= 0) continue;
      if (comp_scope_of(c, w) != sc) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !vn || !sp_streq(wn, vn)) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && nt_kind(nt, val) == NK_CallNode) cand = val;
    }
  }
  if (cand < 0) return 0;
  NodeKind ck = nt_kind(nt, cand);
  if (ck != NK_CallNode) return 0;
  const char *nm = nt_str(nt, cand, "name");
  if (!nm || !sp_streq(nm, "to_proc")) return 0;
  return method_recv_nodes(c, nt_ref(nt, cand, "receiver"), out);
}

/* Param-index shift for a call through a Method object. A bound
   `<recv>.method(:__bam_N)` resolves to a synthesized top-level wrapper whose
   first param (__bam_r) is carried by the Method's self slot, so positional
   call args map to params[1..]. A real instance/class method keeps self
   implicit (params are the declared ones) and shifts by 0. A target without
   a first parameter has no receiver slot to shift past. */
int method_call_param_shift(Compiler *c, int mn, int mi) {
  if (mn < 0 || mi < 0) return 0;
  if (nt_ref(c->nt, mn, "receiver") < 0) return 0;
  Scope *m = &c->scopes[mi];
  return (m->class_id < 0 && !m->is_cmethod && m->nparams > 0 && m->pnames) ? 1 : 0;
}

/* True when scope `scope_idx` contains an explicit `return`, which needs
   a return funnel when the method is inlined at its call sites. */
int scope_has_return(Compiler *c, int scope_idx) {
  NT_FOREACH_KIND(c->nt, NK_ReturnNode, id)
    if (c->nscope[id] == scope_idx) return 1;
  return 0;
}

/* Resolve a block-bearing CallNode to an INLINE-ABLE yielding user method:
   mirrors emit_inline_call_x's resolution (free function -> implicit-self
   chain -> Cls class method -> object-receiver chain) and its
   yields guard. -1 for anything else -- builtin iterators, `loop`,
   `catch`, proc/lambda literals, and methods the inliner would refuse. */
int call_user_yield_mi(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return -1;
  int mi = -1;
  if (recv < 0) mi = comp_self_call_mi(c, id, name);
  else {
    TyKind rt = infer_type(c, recv);
    NodeKind rk = nt_kind(nt, recv);
    /* a scoped receiver (NS::Base.transaction { }) resolves by its leaf name,
       the key classes are indexed under */
    const char *cname = ((rk == NK_ConstantReadNode ||
                                 rk == NK_ConstantPathNode))
                        ? nt_str(nt, recv, "name") : NULL;
    /* ... and `self.class.run { }` by the class self.class names, as the
       inliner does (self_class_static_ci) */
    int ci = cname ? comp_class_index(c, cname) : self_class_static_ci(c, recv);
    if (ci >= 0) mi = call_target(c, id, ci, name, CT_CMETHOD, NULL);
    else if (ty_is_object(rt)) mi = call_target(c, id, ty_object_class(rt), name, 0, NULL);
  }
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (!m->yields) return -1;
  return mi;
}

/* The RangeNode behind a TY_RANGE local-variable read, when the local has
   exactly one write anywhere in the program (a plain LocalVariableWriteNode
   in the read's own scope) and that write's value is a (possibly
   parenthesized) range literal. Lets endpoint-shape checks (endless /
   Float::INFINITY size, take/first prefix) see through the local, e.g.
   `r = (1..Float::INFINITY); r.size`. Same-name writes elsewhere -- other
   scopes, destructures, or/and/op-assigns -- disqualify conservatively
   (there is no depth field to tell a captured outer write apart).
   Returns -1 when the receiver has any other origin. */
int local_sole_range_node(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  NodeKind rk = nt_kind(nt, recv);
  if (rk != NK_LocalVariableReadNode) return -1;
  const char *vn = nt_str(nt, recv, "name");
  if (!vn) return -1;
  Scope *sc = comp_scope_of(c, recv);
  int val = -1;
  for (int w = 0; w < nt->count; w++) {
    NodeKind k = nt_kind(nt, w);
    if (k != NK_LocalVariableWriteNode && k != NK_LocalVariableOrWriteNode &&
        k != NK_LocalVariableAndWriteNode && k != NK_LocalVariableOperatorWriteNode &&
        k != NK_LocalVariableTargetNode)
      continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    if (k != NK_LocalVariableWriteNode || comp_scope_of(c, w) != sc || val >= 0)
      return -1;
    val = nt_ref(nt, w, "value");
  }
  val = nt_paren_single(nt, val);
  if (val < 0 || nt_kind(nt, val) != NK_RangeNode) return -1;
  /* callers re-emit the endpoints at the use site (take/first prefix loops),
     so both must be pure: literals or constant reads only */
  for (int e = 0; e < 2; e++) {
    int ep = nt_ref(nt, val, e ? "right" : "left");
    if (ep < 0) continue;
    NodeKind ek = nt_kind(nt, ep);
    if (ek != NK_IntegerNode && ek != NK_FloatNode && ek != NK_NilNode &&
        ek != NK_ConstantReadNode && ek != NK_ConstantPathNode)
      return -1;
  }
  return val;
}

/* A method result (`.to_sym`, `.join`, ...) or a string/symbol interpolation
   produces a fresh value that need not appear anywhere as a source literal. */
static int node_is_computed_name(const NodeTable *nt, int n) {
  const char *ty = n >= 0 ? nt_type(nt, n) : NULL;
  NodeKind k = nt_kind(nt, n);
  if (!ty) return 0;
  return k == NK_CallNode || k == NK_InterpolatedStringNode ||
  k == NK_InterpolatedSymbolNode;
}

/* An element read out of a named container (`PLAN[i]`, `PLAN.first`,
   `TABLE.fetch(k)`, `@plan[i]`) is the same kind of name a block parameter
   over that container is: one of the values the container was filled with,
   which the arm set already covers when those are literals. It was refused
   as computed, with the refusal misreading `send` itself as undefined
   (#4850). */
static int node_is_container_read(const NodeTable *nt, int n) {
  if (n < 0 || nt_kind(nt, n) != NK_CallNode) return 0;
  const char *en = nt_str(nt, n, "name");
  int er = nt_ref(nt, n, "receiver");
  NodeKind rk = er >= 0 ? nt_kind(nt, er) : NK_NONE;
  return en && (sp_streq(en, "[]") || sp_streq(en, "first") || sp_streq(en, "last") ||
                sp_streq(en, "fetch") || sp_streq(en, "at") || sp_streq(en, "dig") ||
                sp_streq(en, "sample")) &&
         (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ||
          rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode);
}

/* A dynamic-send name is genuinely runtime-computed -- and so cannot be covered
   by desugar_dynamic_send's literal-derived arm set -- when it is a computed
   expression directly, or a local assigned such a computation. A bare variable
   or block parameter whose values are the program's symbol/string literals
   (`m = :upcase; x.send(m)`, `%w[a b].each { |k| x.send(k) }`) is NOT computed:
   its runtime value is one of those literals, which the arm set already covers.
   Only a computed name is rejected, so it fails loudly at build time rather than
   silently raising the wrong NoMethodError for a method the arms never built. */
static int send_name_is_computed(Compiler *c, int arg, int depth);

/* Does subtree n build a name no literal spells: an interpolated Symbol or
   String, or a to_sym / intern of something? */
static int subtree_builds_name(const NodeTable *nt, int n, int depth) {
  if (n < 0 || depth > 64) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_InterpolatedSymbolNode || k == NK_InterpolatedStringNode) return 1;
  if (k == NK_CallNode) {
    const char *cn = nt_str(nt, n, "name");
    if (cn && (sp_streq(cn, "to_sym") || sp_streq(cn, "intern"))) return 1;
  }
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr; i++) if (subtree_builds_name(nt, nt_ref_at(nt, n, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (subtree_builds_name(nt, ids[j], depth + 1)) return 1;
  }
  return 0;
}
/* Does node x name the same container as receiver r: a local of the same
   scope, an ivar of the same class, a constant of the same name? */
static int container_same(Compiler *c, int r, int x) {
  const NodeTable *nt = c->nt;
  NodeKind rk = nt_kind(nt, r), xk = nt_kind(nt, x);
  const char *rn = nt_str(nt, r, "name"), *xn = nt_str(nt, x, "name");
  if (!rn || !xn || !sp_streq(rn, xn)) return 0;
  switch (rk) {
    case NK_LocalVariableReadNode:
      return (xk == NK_LocalVariableReadNode || xk == NK_LocalVariableWriteNode) && comp_scope_of(c, x) == comp_scope_of(c, r);
    case NK_InstanceVariableReadNode:
      return (xk == NK_InstanceVariableReadNode || xk == NK_InstanceVariableWriteNode) &&
             comp_ivar_owner(c, x) == comp_ivar_owner(c, r);
    case NK_ConstantReadNode: case NK_ConstantPathNode:
      return xk == NK_ConstantReadNode || xk == NK_ConstantPathNode || xk == NK_ConstantWriteNode;
    default:
      return 0;
  }
}
/* Is the container a read of element `arg` comes out of filled with names
   no literal spells? One whose own write builds them (`M = Hash.new { |h,
   t| h[t] = :"on_#{t}" }`, `plan = [:"on_#{x}"]`, `@tab = [...]`), or one a
   `X[k] = v`, `X << v` or `X.push(v)` stores such a name into -- the same
   container: a local of the read's scope, an ivar of its class. Then the
   element is a computed name, whose arms are every method rather than the
   literals. */
static int container_read_holds_computed(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, arg, "receiver");
  if (r < 0 || !nt_str(nt, r, "name")) return 0;
  /* the container's own writes (`M = {...}`, `plan = [:"on_#{x}"]`,
     `@tab = [...]`) */
  static const NodeKind wk[] = { NK_ConstantWriteNode, NK_LocalVariableWriteNode, NK_InstanceVariableWriteNode };
  for (int k = 0; k < 3; k++)
    NT_FOREACH_KIND(nt, wk[k], w)
      if (container_same(c, r, w) && subtree_builds_name(nt, nt_ref(nt, w, "value"), 0)) return 1;
  /* ... and the elements stored into it (`X[k] = v`, `X << v`, `X.push(v)`) */
  NT_FOREACH_KIND(nt, NK_CallNode, w) {
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !(sp_streq(wn, "[]=") || sp_streq(wn, "<<") || sp_streq(wn, "push") || sp_streq(wn, "unshift")))
      continue;
    int wr = nt_ref(nt, w, "receiver");
    if (wr < 0 || !container_same(c, r, wr)) continue;
    int a = nt_ref(nt, w, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an >= 1 && subtree_builds_name(nt, av[an - 1], 0)) return 1;
  }
  return 0;
}
/* Is value n a name no literal spells: computed itself, or an element of a
   container holding such names? */
static int value_is_computed_name(Compiler *c, int n) {
  if (node_is_container_read(c->nt, n)) return container_read_holds_computed(c, n);
  return node_is_computed_name(c->nt, n);
}

/* Is the parameter `vn` of the method owning `arg` handed a computed name by
   a caller (#8592)? `handle(name) = public_send(name)` called with
   `"on_#{ev}".to_sym` reaches names no literal spells, so its send takes the
   complete arm set a computed name takes. A caller's argument that is itself
   a parameter asks the same of its own callers, a few levels deep. The
   answer is memoized per (scope, parameter) and dropped when the node table
   grew, since the callers are found by one walk over the program's calls. */
static int param_handed_computed_name(Compiler *c, int arg, const char *vn, int depth) {
  const NodeTable *nt = c->nt;
  Scope *sc = comp_scope_of(c, arg);
  if (!sc || !sc->name || sc->def_node < 0 || depth > 4) return 0;
  LocalVar *lv = scope_local(sc, vn);
  if (!lv || !lv->is_param) return 0;
  int pi = -1;
  for (int k = 0; k < sc->nparams; k++)
    if (sc->pnames[k] && sp_streq(sc->pnames[k], vn)) { pi = k; break; }
  /* a positional parameter ahead of any rest: its argument sits at its index */
  if (pi < 0 || (sc->rest_idx >= 0 && pi >= sc->rest_idx) || (sc->kwrest_idx >= 0 && pi >= sc->kwrest_idx)) return 0;
  int si = (int)(sc - c->scopes);
  int key = pi < 64 ? si * 64 + pi : -1;
  if (c->send_param_memo && (c->send_param_memo_nodes != nt->count || c->send_param_memo_n < c->nscopes * 64)) {
    free(c->send_param_memo); c->send_param_memo = NULL;
  }
  if (!c->send_param_memo) {
    c->send_param_memo_n = c->nscopes * 64;
    c->send_param_memo = calloc((size_t)c->send_param_memo_n, 1);
    c->send_param_memo_nodes = nt->count;
  }
  if (key >= 0 && c->send_param_memo && c->send_param_memo[key]) return c->send_param_memo[key] == 2;
  int res = 0;
  /* the calls of the method's name, off the calls-by-name index: every
     call of the program was read for each parameter asked about */
  for (int id = an_calls_named_first(c, sc->name); id >= 0 && !res; id = an_calls_named_next(id)) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn || !sp_streq(cn, sc->name)) continue;
    int a = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (pi >= an) continue;
    int x = av[pi];
    NodeKind xk = nt_kind(nt, x);
    if (xk == NK_SplatNode || xk == NK_KeywordHashNode || xk == NK_BlockArgumentNode) continue;
    if (value_is_computed_name(c, x) || (xk == NK_LocalVariableReadNode && send_name_is_computed(c, x, depth + 1))) res = 1;
  }
  /* the memo may have been rebuilt by a deeper call: write only into the live one */
  if (key >= 0 && c->send_param_memo && c->send_param_memo_nodes == nt->count && key < c->send_param_memo_n)
    c->send_param_memo[key] = res ? 2 : 1;
  return res;
}

static int send_name_is_computed(Compiler *c, int arg, int depth) {
  const NodeTable *nt = c->nt;
  if (node_is_container_read(nt, arg)) return container_read_holds_computed(c, arg);
  if (node_is_computed_name(nt, arg)) return 1;
  NodeKind ak = nt_kind(nt, arg);
  if (ak != NK_LocalVariableReadNode) return 0;
  const char *vn = nt_str(nt, arg, "name");
  int scope = (arg >= 0 && arg < nt->count) ? c->nscope[arg] : -1;
  if (!vn) return 0;
  int written = 0;
  for (int id = an_local_writes_named_first(c, vn); id >= 0; id = an_local_writes_named_next(id)) {
    if (c->nscope[id] != scope) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    written = 1;
    /* a local bound to such an element (`step = PLAN[i]; send(step)`) is
       that element */
    int wv = nt_ref(nt, id, "value");
    if (value_is_computed_name(c, wv)) return 1;
    /* and one bound to another local (`t = s`, as `&s` binds the Symbol it
       is passed) is that local */
    if (nt_kind(nt, wv) == NK_LocalVariableReadNode && depth < 4 &&
        send_name_is_computed(c, wv, depth + 1)) return 1;
  }
  /* a parameter no statement of the method rebinds: the names its callers hand it */
  if (!written) return param_handed_computed_name(c, arg, vn, depth);
  return 0;
}

int an_send_name_is_computed(Compiler *c, int arg) {
  return send_name_is_computed(c, arg, 0);
}

int an_indexed_each_source(const NodeTable *nt, int recv) {
  const char *rn = nt_str(nt, recv, "name");
  if (rn && sp_streq(rn, "each_with_index")) return nt_ref(nt, recv, "receiver");
  if (!rn || !sp_streq(rn, "with_index")) return -1;
  int wir = nt_ref(nt, recv, "receiver");
  if (wir >= 0 && nt_kind(nt, wir) == NK_CallNode &&
      nt_str(nt, wir, "name") && sp_streq(nt_str(nt, wir, "name"), "each") &&
      nt_ref(nt, wir, "block") < 0)
    return nt_ref(nt, wir, "receiver");
  return -1;
}

void an_node_dir(const NodeTable *nt, int id, char *dir, size_t cap) {
  const char *sf = nt_str(nt, id, "src_file");
  if (!sf) sf = nt->source_file;
  const char *sl = sf ? strrchr(sf, '/') : NULL;
  size_t n = sl && sl > sf ? (size_t)(sl - sf) : 1;
  if (n >= cap) n = cap - 1;
  memcpy(dir, sl ? sf : ".", n); dir[n] = 0;
}

/* The classes whose instances are the runtime's IO handles (sp_File). */
static const char *const io_family[] = {
  "File", "IO", "TCPServer", "TCPSocket", "UDPSocket", "IPSocket", "UNIXServer",
  "UNIXSocket", "Socket", "BasicSocket", NULL };
int io_family_name(const char *n) {
  if (!n) return 0;
  if (is_io_class_name(n)) return 1;
  /* a socket class is the builtin only once the program loads socket
     (asked after the name matches: it walks the program's requires) */
  for (int i = 2; io_family[i]; i++) if (sp_streq(n, io_family[i])) return sp_feature_required("socket");
  return 0;
}
/* Class k is a reopening of one of them, not a class of that name nested
   in a module of the program's own. */
int io_family_class(Compiler *c, int k) {
  return k >= 0 && k < c->nclasses && c->classes[k].enclosing_class < 0 &&
         io_family_name(c->classes[k].name);
}
/* A typed IO's kind is only known at run time. Its reopened method is looked
   up as a File's first (File, then IO), then as a socket's. */
/* Set while a typed IO's call (g_io_skip_node) is re-emitted as the
   builtin it overrides: the reopenings are out of sight for that call
   alone, not for the calls in its arguments. */
int g_io_skip_reopen = 0, g_io_skip_node = -1;
int io_reopen_class(Compiler *c, const char *name) {
  if (!name || g_io_skip_reopen) return -1;
  for (int i = 0; io_family[i]; i++) {
    int k = comp_class_index(c, io_family[i]), def = -1;
    if (k >= 0 && comp_method_in_chain(c, k, name, &def) >= 0 && io_family_class(c, def)) return def;
  }
  return -1;
}
/* The IO-family reopenings that define `name` themselves (public ones only
   when asked), in class order, into ks[]; their count. */
int io_reopen_defs(Compiler *c, const char *name, int public_only, int *ks, int max) {
  int n = 0;
  for (int k = 0; k < c->nclasses && n < max; k++) {
    int def = -1;
    if (!io_family_class(c, k)) continue;
    if (comp_method_in_chain(c, k, name, &def) < 0 || def != k) continue;
    if (public_only && comp_method_vis_in_chain(c, k, name) != SP_VIS_PUBLIC) continue;
    ks[n++] = k;
  }
  return n;
}
/* Do the reopenings defining `name` answer different types? A typed IO's
   call is then boxed, each kind's answer boxed into it. */
int io_reopen_ret_mixed(Compiler *c, const char *name) {
  int ks[16], n = io_reopen_defs(c, name, 0, ks, 16);
  for (int i = 1; i < n; i++)
    if (c->scopes[comp_method_in_chain(c, ks[i], name, NULL)].ret !=
        c->scopes[comp_method_in_chain(c, ks[0], name, NULL)].ret) return 1;
  return 0;
}
/* The builtin superclass of an IO-family class, as lib/sp_io.c walks a
   handle's kind (sp_io_super_of); NULL above IO. */
const char *io_super_name(const char *k) {
  static const char *const pairs[][2] = {
    {"TCPServer", "TCPSocket"}, {"TCPSocket", "IPSocket"}, {"UDPSocket", "IPSocket"},
    {"IPSocket", "BasicSocket"}, {"UNIXServer", "UNIXSocket"}, {"UNIXSocket", "BasicSocket"},
    {"Socket", "BasicSocket"}, {"BasicSocket", "IO"}, {"File", "IO"}, {NULL, NULL} };
  for (int i = 0; pairs[i][0]; i++) if (sp_streq(k, pairs[i][0])) return pairs[i][1];
  return NULL;
}
/* Is IO-family class k the class `owner` or below it? */
int io_family_descends(Compiler *c, int k, int owner) {
  if (!io_family_class(c, k) || !io_family_class(c, owner)) return 0;
  for (const char *n = c->classes[k].name; n; n = io_super_name(n))
    if (sp_streq(n, c->classes[owner].name)) return 1;
  return 0;
}
/* Does some IO kind reach no reopening of `name` (no IO#name among them),
   so that the builtin answers for it? */
int io_reopen_leaves_builtin(Compiler *c, const char *name) {
  int ks[16], n = io_reopen_defs(c, name, 0, ks, 16);
  for (int i = 0; i < n; i++) if (sp_streq(c->classes[ks[i]].name, "IO")) return 0;
  return n > 0;
}

/* Builtin classes that the builtin class table lacks: Monitor, and
   OpenStruct once `require "ostruct"` makes it one. */
int is_untabled_native_class_name(const char *n) {
  return n && (sp_streq(n, "Monitor") || (sp_streq(n, "OpenStruct") && sp_feature_required("ostruct")));
}

int is_builtin_module_const_name(const char *n) {
  if (!n) return 0;
  static const char *const names[] = {
    "Kernel", "Comparable", "Enumerable", "Math", "Marshal", "FileTest", "Errno", "Warning",
    "ObjectSpace", "Process", "GC", "Signal", NULL
  };
  for (int i = 0; names[i]; i++) if (sp_streq(names[i], n)) return 1;
  return is_builtin_module_name(n);
}

/* Call `id` is a `require "time"` class parser that reads one fixed format
   -- Time.httpdate, rfc2822 / rfc822, xmlschema / iso8601, or Time.parse for
   its numeric ISO 8601 forms -- on the builtin Time, with its one argument:
   the runtime function that parses it, else NULL (not one of them, no
   `require "time"`, a wrong argument count, or a program whose Time has its
   own class method of that name). */
const char *time_strict_parser_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return NULL;
  const char *fn = is_time_native_format(name)
                   ? (is_iso8601_alias(name) ? "sp_time_s_xmlschema" : is_rfc2822_alias(name) ? "sp_time_s_rfc2822"
                                                                                       : "sp_time_s_httpdate")
                 : sp_streq(name, "parse") ? "sp_time_s_parse"
                 : NULL;
  if (!fn || !sp_feature_required("time")) return NULL;
  int recv = nt_ref(nt, id, "receiver");
  const char *rn = recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode ? nt_str(nt, recv, "name") : NULL;
  if (!rn || !sp_streq(rn, "Time") || nt_ref(nt, id, "block") >= 0) return NULL;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return NULL;
  if (nt_argv_has(nt, av, an, NT_ARG_SPLAT | NT_ARG_KEYWORDS | NT_ARG_BLOCK)) return NULL;
  int tci = comp_class_index(c, "Time");
  if (tci >= 0 && call_target(c, id, tci, name, CT_CMETHOD, NULL) >= 0) return NULL;
  return fn;
}

/* The program's parent map (an_parent_map), kept on the Compiler as the
   emitter's is (codegen_parents, the same fields) and rebuilt whenever the
   node table changed, for an analysis that asks where a node sits on every
   round. */
const int *comp_parent_map(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!c->cg_parent || c->cg_parent_n != nt->count || c->cg_parent_ver != nt->version) {
    free(c->cg_parent);
    c->cg_parent = an_parent_map(nt);
    if (!c->cg_parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->cg_parent_n = nt->count; c->cg_parent_ver = nt->version;
  }
  return c->cg_parent;
}

/* Is node n a statement the next statement follows, so that its value is
   thrown away? Climbs from n through parentheses and statement lists
   (vp_climb over the wrappers alone) to a statement before its list's last.
   A conditional's arm, a loop's body and the like end the climb: the
   callers that ask for those use an_value_dropped. */
int comp_value_dropped(Compiler *c, int n) {
  VpUp up;
  return n >= 0 && n < c->nt->count && vp_climb(c->nt, comp_parent_map(c), n, VPS_NONE, &up) == VPU_DROPPED;
}


/* Is argument n's type want, or (unknown_ok, for the gate's early rounds)
   not settled yet? */
static int strh_arg_ty_ok(const Compiler *c, int n, TyKind want, TyKind want2, int unknown_ok) {
  TyKind t = comp_ntype(c, n);
  return t == want || t == want2 || (unknown_ok && t == TY_UNKNOWN);
}
/* Does block blk take no parameter or one plain one, which binds the
   element (not a destructuring `|(x, y)|`, a rest, a default, a keyword, a
   block parameter or a block-local)? */
static int strh_plain_block_params(Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  int bp = nt_ref(nt, blk, "parameters");
  if (bp < 0) return 1;
  if (nt_kind(nt, bp) != NK_BlockParametersNode) return 0;
  int lc = 0;
  nt_arr(nt, bp, "locals", &lc);
  if (lc > 0) return 0;
  int pn = nt_ref(nt, bp, "parameters");
  if (pn < 0) return 1;
  int n = 0;
  const int *rq = nt_arr(nt, pn, "requireds", &n);
  if (n > 1 || (n == 1 && nt_kind(nt, rq[0]) != NK_RequiredParameterNode)) return 0;
  /* (a destructuring parameter is a hidden parameter and a read of its
     elements by the time the walk runs) */
  const char *pnm = n == 1 ? nt_str(nt, rq[0], "name") : NULL;
  if (pnm && !strncmp(pnm, "__destr", 7)) return 0;
  int k = 0;
  nt_arr(nt, pn, "optionals", &k);
  if (k > 0) return 0;
  nt_arr(nt, pn, "posts", &k);
  if (k > 0) return 0;
  nt_arr(nt, pn, "keywords", &k);
  if (k > 0) return 0;
  return nt_ref(nt, pn, "rest") < 0 && nt_ref(nt, pn, "keyword_rest") < 0 && nt_ref(nt, pn, "block") < 0;
}
int strh_call_op_in(Compiler *c, int call, TyKind rt, int unknown_ok) {
  const NodeTable *nt = c->nt;
  if (rt != TY_STR_ARRAY || call < 0 || nt_kind(nt, call) != NK_CallNode) return STRH_NONE;
  const char *nm = nt_str(nt, call, "name");
  int ca = nt_ref(nt, call, "arguments"), argc = 0, blk = nt_ref(nt, call, "block");
  const int *av = ca >= 0 ? nt_arr(nt, ca, "arguments", &argc) : NULL;
  if (nt_argv_has(nt, av, argc, NT_ARG_SPLAT | NT_ARG_KEYWORDS | NT_ARG_BLOCK)) return STRH_NONE;
  int op = strh_op(nm, argc, blk < 0 ? 0 : nt_kind(nt, blk) == NK_BlockNode ? 1 : 2);
  if (op == STRH_EACH && !strh_plain_block_params(c, blk)) return STRH_NONE;
  if ((op == STRH_ELEM && argc == 1) || op == STRH_STORE)
    for (int k = 0; k < (op == STRH_STORE ? 1 : argc); k++)
      if (!strh_arg_ty_ok(c, av[k], TY_INT, TY_INT, unknown_ok)) return STRH_NONE;
  /* (what is stored is a String, or a box that may hold one, which
     sp_StrhArray_unbox opens: any other value is no store of this form, and
     widens the Array (fold_container_evidence), so the gate sees it) */
  if (op == STRH_PUSH || (op == STRH_STORE && argc == 2))
    for (int k = op == STRH_STORE ? 1 : 0; k < argc; k++) {
      TyKind vt = comp_ntype(c, av[k]);
      if (vt != TY_STRING && vt != TY_STRBUF && vt != TY_UNKNOWN && vt != TY_POLY) return STRH_NONE;
    }
  /* (the other operand of == is another Array of handles) */
  if (op == STRH_EQ && !strh_arg_ty_ok(c, av[0], TY_STR_ARRAY, TY_STR_ARRAY, unknown_ok)) return STRH_NONE;
  /* (a separator or a needle that is a String) */
  if ((op == STRH_JOIN || op == STRH_INCLUDE) && argc == 1 &&
      !strh_arg_ty_ok(c, av[0], TY_STRING, TY_STRBUF, unknown_ok)) return STRH_NONE;
  if (op != STRH_NONE && share_builtin_overridden(c, "Array", nm)) return STRH_NONE;
  return op;
}
int strh_call_op(Compiler *c, int call, TyKind rt) { return strh_call_op_in(c, call, rt, 0); }
/* Is node n a read of a variable (a local, an instance variable, a global, a
   constant or a class variable), which the handle element emitters can read
   again where an element is used as the slot it is? */
int strh_var_read(const NodeTable *nt, int n) {
  NodeKind k = n >= 0 ? nt_kind(nt, n) : NK_NONE;
  return k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || k == NK_GlobalVariableReadNode ||
         k == NK_ConstantReadNode || k == NK_ClassVariableReadNode;
}

/* Is `recv` the builtin Kernel module as call `id`'s receiver (a constant
   read of Kernel the program gives no class method of the call's name)? */
int an_kernel_const_recv(Compiler *c, int id, int recv, int fresh) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && sp_streq(rn, "Kernel") && !cplan_const_user(c, id, rn, fresh);
}

/* Is `recv` a receiver Kernel#` answers call `id` for: the Kernel module
   (Kernel.`("cmd")), `self` in the builtin default of a poly self dispatch
   (`in_default`), or a builtin value the call may reach private methods of.
   A backtick in an instance_eval block became `self.`("cmd")`, or a call on
   the spliced receiver's temp (ie_subtree_self_calls). Every Object has
   Kernel's private `, so a String or an Integer self answers it unless the
   program reopens that class with one; the poly default takes the receivers
   whose class has no ` of its own, which a BasicObject subclass's instance
   also reaches (its emitter raises there). A ` the program gives Object or
   Kernel, or a module the top level includes, sits above Kernel's and
   answers those receivers instead, which no arm here emits, so they keep the
   NoMethodError. So does a poly self that main extends with one, since main
   may be that self. */
int an_kernel_backtick_recv(Compiler *c, int id, int recv, int fresh, int in_default) {
  const NodeTable *nt = c->nt;
  if (an_kernel_const_recv(c, id, recv, fresh)) return 1;
  if (recv < 0) return 0;
  int ob = comp_class_index(c, "Object"), ke = comp_class_index(c, "Kernel");
  if ((ob >= 0 && comp_method_in_chain(c, ob, "`", NULL) >= 0) ||
      (ke >= 0 && comp_method_in_chain(c, ke, "`", NULL) >= 0) ||
      comp_toplevel_mod_defines(c, "`", 0)) return 0;
  int self_recv = nt_kind(nt, recv) == NK_SelfNode;
  if (!self_recv && !nt_ignore_visibility(nt, id)) return 0;
  TyKind rt = self_recv ? TY_POLY : infer_type(c, recv);
  /* (a spliced receiver's temp held boxed, as an Integer is under
     --int-overflow=promote, is a poly self too) */
  if (rt == TY_POLY) return in_default && !comp_toplevel_mod_defines(c, "`", 1);
  const char *cn = rt == TY_CLASS || ty_is_object(rt) ? NULL : builtin_class_of_type(rt);
  if (!cn) return 0;
  int ci = comp_class_index(c, cn);
  return ci < 0 || comp_method_in_chain(c, ci, "`", NULL) < 0;
}

/* The instantiated BasicObject subclasses whose chain has no method `name`
   (a malloc'd list the caller frees, NULL for none), their count in *n: the
   receivers a poly dispatch's builtin default reaches that have no Kernel
   method of the name. */
int *an_blank_slates_lacking(Compiler *c, const char *name, int *n) {
  int *out = NULL;
  *n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated || !class_is_blank_slate(c, k) || comp_method_in_chain(c, k, name, NULL) >= 0)
      continue;
    out = realloc(out, sizeof(int) * (size_t)(*n + 1));
    if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    out[(*n)++] = k;
  }
  return out;
}

/* The String a `super` in a ` override (call `id` in scope `s`) hands
   Kernel#`: 1 for super(x) with one plain argument (*arg is x), 2 for a
   zero-argument super in a def of one required parameter and nothing
   else, 0 for any other shape, -1 where no Kernel#` is above self. */
int an_super_backtick_shape(Compiler *c, int id, const Scope *s, int *arg) {
  const NodeTable *nt = c->nt;
  *arg = -1;
  /* a BasicObject instance has no Kernel above it: NoMethodError */
  if (!s->is_cmethod && s->class_id >= 0 && class_is_blank_slate(c, s->class_id)) return -1;
  if (nt_kind(nt, id) == NK_SuperNode) {
    int a = nt_ref(nt, id, "arguments"), an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an != 1 || nt_ref(nt, id, "block") >= 0) return 0;
    NodeKind ak = nt_kind(nt, av[0]);
    if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode) return 0;
    *arg = av[0];
    return 1;
  }
  if (nt_kind(nt, id) != NK_ForwardingSuperNode) return 0;
  if (s->nparams != 1 || s->rest_idx >= 0 || s->kwrest_idx >= 0 || s->pdefault[0] >= 0) return 0;
  int pn = s->def_node >= 0 && nt_kind(nt, s->def_node) == NK_DefNode ? nt_ref(nt, s->def_node, "parameters") : -1;
  if (pn >= 0) {
    int rn = 0, kn = 0;
    nt_arr(nt, pn, "requireds", &rn);
    nt_arr(nt, pn, "keywords", &kn);
    if (rn != 1 || kn != 0 || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0) return 0;
  }
  return 2;
}

/* ---- The lowerings onto a library's method (builtin_names.h, LowerRow) ----
   Whether call `call` passes lo..hi arguments (hi -1: no bound; no
   ArgumentsNode counts as none) of the kinds `flags` allows, and has the
   block and the receiver they ask for. */
static int lower_args_fit(const NodeTable *nt, int call, int lo, int hi, unsigned flags) {
  int a = nt_ref(nt, call, "arguments"), an = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  if (an < lo || (hi >= 0 && an > hi)) return 0;
  int blk = nt_ref(nt, call, "block");
  if ((flags & LR_NO_BLOCK) && blk >= 0) return 0;
  if ((flags & LR_BLOCK) && blk < 0) return 0;
  if ((flags & (LR_BLOCK_LITERAL | LR_BREAKS)) && (blk < 0 || nt_kind(nt, blk) != NK_BlockNode)) return 0;
  if ((flags & LR_HAS_RECV) && nt_ref(nt, call, "receiver") < 0) return 0;
  if ((flags & LR_NO_ARGS) && a >= 0) return 0;
  if ((flags & LR_KWHASH) && nt_argv_kwh(nt, av, an) < 0) return 0;
  for (int i = 0; i < an; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    if ((flags & (LR_NO_SPLAT | LR_PLAIN_ARGS)) && k == NK_SplatNode) return 0;
    if ((flags & LR_PLAIN_ARGS) && (k == NK_KeywordHashNode || k == NK_BlockArgumentNode)) return 0;
  }
  if ((flags & LR_BREAKS) && !block_body_breaks(nt, nt_ref(nt, blk, "body"))) return 0;
  return 1;
}
/* Is `n` the constant read `name`? */
static int lower_const_is(const NodeTable *nt, int n, const char *name) {
  return n >= 0 && nt_kind(nt, n) == NK_ConstantReadNode && nt_str(nt, n, "name") &&
         sp_streq(nt_str(nt, n, "name"), name);
}
/* The row of `family` whose shape call `id` has, or NULL: the desugar that
   lowers the family asks it before its own value, type and program tests. */
const LowerRow *lower_row_nt(const NodeTable *nt, int id, int family) {
  if (id < 0 || nt_kind(nt, id) != NK_CallNode) return NULL;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return NULL;
  int recv = nt_ref(nt, id, "receiver");
  const LowerRow *r;
  for (int i = 0; (r = lower_row_at(i)) != NULL; i++) {
    if (r->family != family || !sp_streq(r->name, nm)) continue;
    if (r->recv == LR_RECV_NONE && recv >= 0) continue;
    if (r->recv == LR_RECV_SOME && recv < 0) continue;
    if (r->recv == LR_RECV_CONST && !lower_const_is(nt, recv, r->recv_name)) continue;
    if (r->recv == LR_RECV_CALL) {
      const char *rn = recv >= 0 && nt_kind(nt, recv) == NK_CallNode ? nt_str(nt, recv, "name") : NULL;
      if (!rn || !sp_streq(rn, r->recv_name)) continue;
      if (r->recv_const && !lower_const_is(nt, nt_ref(nt, recv, "receiver"), r->recv_const)) continue;
      if (!lower_args_fit(nt, recv, r->rargc_min, r->rargc_max, r->rflags)) continue;
    }
    if (r->recv_calls && recv >= 0 && nt_kind(nt, recv) == NK_CallNode && nt_ref(nt, recv, "block") < 0) {
      const char *rn = nt_str(nt, recv, "name");
      int k = 0;
      while (rn && r->recv_calls[k] && !sp_streq(rn, r->recv_calls[k])) k++;
      if (!rn || !r->recv_calls[k]) continue;
    }
    if (!lower_args_fit(nt, id, r->argc_min, r->argc_max, r->flags)) continue;
    return r;
  }
  return NULL;
}

/* SPINEL_LOWER_ROWS_CHECK, in the table modes: the load graph matches the
   rows on each call's parse before the table exists (spinel_parse.c's
   sp_pm_row_fits) and notes the rows it matched on the call's node
   (`lr_rows`, a mask of row indices). For each family the first row so
   noted must be the one lower_row_nt matches on the node, as flatten made
   it. Each disagreement is reported with its place, then the count; the
   number of disagreements. */
int lower_rows_check(const NodeTable *nt) {
  int calls = 0, bad = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    long long mask = nt_int(nt, id, "lr_rows", -1);
    if (mask < 0) continue;
    calls++;
    for (int fam = LR_SYSTEM_OPTIONS; fam <= LR_TO_SET; fam++) {
      const LowerRow *a = NULL, *r;
      for (int i = 0; i < LOWER_ROWS_MASK_BITS && !a && (r = lower_row_at(i)) != NULL; i++)
        if (r->family == fam && (mask >> i & 1)) a = r;
      const LowerRow *b = lower_row_nt(nt, id, fam);
      if (a == b) continue;
      bad++;
      int f = (int)nt_int(nt, id, "node_file", 0);
      fprintf(stderr, "lower-rows-check: %s:%lld:%lld `%s`: the parse matches %s, the node %s\n",
              f >= 0 && f < nt->nfiles && nt->files[f] ? nt->files[f] : "?", nt_int(nt, id, "node_line", 0),
              nt_int(nt, id, "node_col", 0), nt_str(nt, id, "name"), a ? a->target : "no row", b ? b->target : "no row");
    }
  }
  if (lower_row_at(LOWER_ROWS_MASK_BITS)) {
    fprintf(stderr, "lower-rows-check: the rows past %d are not checked (the mask is full)\n", LOWER_ROWS_MASK_BITS);
    bad++;
  }
  fprintf(stderr, "lower-rows-check: %d calls, %d disagreements\n", calls, bad);
  return bad;
}
