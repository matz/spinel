#include "analyze_internal.h"
#include <limits.h>

/* One table for the builtin class/module/exception names. These four
   predicates were four independent lists that had to be kept in step by hand,
   and the code has a comment noting where they already disagree. A row now
   carries every fact about one name: the flags say which predicate accepts it,
   and `id` is its negative cls_id (0 when codegen has none). Adding a name is
   one row instead of four edits.

   The rows are exactly the union of the old lists, flags included, so every
   predicate answers what it answered before -- what the table changes is that
   the answers can no longer drift apart. It also makes the existing gaps
   visible rather than implicit: six names are classes with no cls_id
   (Encoding, GC, Method, ObjectSpace, Process, UnboundMethod), seven have a
   cls_id but are not bare constants (the Thread:: / Process:: / Math::
   qualified forms), and fourteen exception names have no id. Each is a
   deliberate answer today; closing any of them is a behavior change and does
   not belong in the same commit as the table.

   The `CRUBY_KNOWN` list in codegen_call.c is a fifth list, for a different
   question (names CRuby defines that spinel does not implement), and is left
   alone here. */
enum { BC_CLASS = 1, BC_MODULE = 2, BC_EXCEPTION = 4, BC_SOCKET = 8 };
typedef struct { const char *name; int id; unsigned flags; } BuiltinClass;
static const BuiltinClass BUILTIN_CLASSES[] = {
  { "Integer",                      -100,  BC_CLASS },
  { "Float",                        -101,  BC_CLASS },
  { "String",                       -102,  BC_CLASS },
  { "Symbol",                       -103,  BC_CLASS },
  { "Array",                        -104,  BC_CLASS },
  { "Hash",                         -105,  BC_CLASS },
  { "Range",                        -106,  BC_CLASS },
  { "Time",                         -107,  BC_CLASS },
  { "Module",                       -108,  BC_CLASS },
  { "Class",                        -109,  BC_CLASS },
  { "NilClass",                     -110,  BC_CLASS },
  { "TrueClass",                    -111,  BC_CLASS },
  { "FalseClass",                   -112,  BC_CLASS },
  { "Numeric",                      -113,  BC_CLASS },
  { "Comparable",                   -114,  BC_CLASS | BC_MODULE },
  { "Enumerable",                   -115,  BC_CLASS | BC_MODULE },
  { "Object",                       -116,  BC_CLASS },
  { "BasicObject",                  -117,  BC_CLASS },
  { "Proc",                         -118,  BC_CLASS },
  { "Kernel",                       -119,  BC_CLASS | BC_MODULE },
  { "IO",                           -120,  BC_CLASS },
  { "File",                         -121,  BC_CLASS },
  { "Exception",                    -122,  BC_CLASS | BC_EXCEPTION },
  { "StandardError",                -123,  BC_CLASS | BC_EXCEPTION },
  { "RuntimeError",                 -124,  BC_CLASS | BC_EXCEPTION },
  { "TypeError",                    -125,  BC_CLASS | BC_EXCEPTION },
  { "ArgumentError",                -126,  BC_CLASS | BC_EXCEPTION },
  { "NameError",                    -127,  BC_CLASS | BC_EXCEPTION },
  { "NoMethodError",                -128,  BC_CLASS | BC_EXCEPTION },
  { "StopIteration",                -129,  BC_CLASS | BC_EXCEPTION },
  { "Math",                         -130,  BC_CLASS | BC_MODULE },
  { "Complex",                      -131,  BC_CLASS },
  { "IndexError",                   -132,  BC_CLASS | BC_EXCEPTION },
  { "KeyError",                     -133,  BC_CLASS | BC_EXCEPTION },
  { "RangeError",                   -134,  BC_CLASS | BC_EXCEPTION },
  { "FloatDomainError",             -135,  BC_CLASS | BC_EXCEPTION },
  { "ZeroDivisionError",            -136,  BC_CLASS | BC_EXCEPTION },
  { "FrozenError",                  -137,  BC_CLASS | BC_EXCEPTION },
  { "IOError",                      -138,  BC_CLASS | BC_EXCEPTION },
  { "LocalJumpError",               -139,  BC_CLASS | BC_EXCEPTION },
  { "NotImplementedError",          -140,  BC_CLASS | BC_EXCEPTION },
  { "ScriptError",                  -141,  BC_CLASS | BC_EXCEPTION },
  { "Rational",                     -142,  BC_CLASS },
  { "Regexp",                       -143,  BC_CLASS },
  { "Enumerator",                   -144,  BC_CLASS },
  { "Struct",                       -145,  BC_CLASS },
  { "Data",                         -146,  BC_CLASS },
  { "SyntaxError",                  -147,  BC_CLASS | BC_EXCEPTION },
  { "SecurityError",                -148,  BC_CLASS | BC_EXCEPTION },
  { "RegexpError",                  -149,  BC_CLASS | BC_EXCEPTION },
  { "EncodingError",                -150,  BC_CLASS | BC_EXCEPTION },
  { "SignalException",              -151,  BC_CLASS | BC_EXCEPTION },
  { "Interrupt",                    -152,  BC_CLASS | BC_EXCEPTION },
  { "ThreadError",                  -153,  BC_CLASS | BC_EXCEPTION },
  { "FiberError",                   -154,  BC_CLASS | BC_EXCEPTION },
  { "ClosedQueueError",             -155,  BC_CLASS | BC_EXCEPTION },
  { "UncaughtThrowError",           -156,  BC_CLASS | BC_EXCEPTION },
  { "NoMatchingPatternError",       -157,  BC_CLASS | BC_EXCEPTION },
  { "NoMatchingPatternKeyError",    -158,  BC_CLASS | BC_EXCEPTION },
  { "EOFError",                     -159,  BC_CLASS | BC_EXCEPTION },
  { "Math::DomainError",            -160,  BC_EXCEPTION },
  { "SystemExit",                   -161,  BC_CLASS | BC_EXCEPTION },
  { "Signal",                       -162,  BC_CLASS | BC_MODULE },
  { "Process::Status",              -163,  BC_CLASS },
  { "Process::Tms",                 -164,  0 },
  { "Dir",                          -165,  BC_CLASS },
  { "BasicSocket",                  -166,  BC_CLASS | BC_SOCKET },
  { "IPSocket",                     -167,  BC_CLASS | BC_SOCKET },
  { "TCPSocket",                    -168,  BC_CLASS | BC_SOCKET },
  { "TCPServer",                    -169,  BC_CLASS | BC_SOCKET },
  { "UDPSocket",                    -170,  BC_CLASS | BC_SOCKET },
  { "UNIXSocket",                   -171,  BC_CLASS | BC_SOCKET },
  { "UNIXServer",                   -172,  BC_CLASS | BC_SOCKET },
  { "Socket",                       -173,  BC_CLASS | BC_SOCKET },
  { "Thread",                       -174,  BC_CLASS },
  { "Mutex",                        -175,  BC_CLASS },
  { "Thread::Mutex",                -175,  0 },
  { "Queue",                        -176,  BC_CLASS },
  { "Thread::Queue",                -176,  0 },
  { "SizedQueue",                   -177,  BC_CLASS },
  { "Thread::SizedQueue",           -177,  0 },
  { "ConditionVariable",            -178,  BC_CLASS },
  { "Thread::ConditionVariable",    -178,  0 },
  { "Fiber",                        -179,  BC_CLASS },
  { "MatchData",                    -180,  BC_CLASS },
  { "Encoding",                     0,     BC_CLASS },
  { "Errno::ENOENT",                0,     BC_EXCEPTION },
  { "GC",                           0,     BC_CLASS | BC_MODULE },
  { "IO::EAGAINWaitReadable",       0,     BC_EXCEPTION },
  { "IO::EAGAINWaitWritable",       0,     BC_EXCEPTION },
  { "IO::EINPROGRESSWaitReadable",  0,     BC_EXCEPTION },
  { "IO::EINPROGRESSWaitWritable",  0,     BC_EXCEPTION },
  { "IO::EWOULDBLOCKWaitReadable",  0,     BC_EXCEPTION },
  { "IO::EWOULDBLOCKWaitWritable",  0,     BC_EXCEPTION },
  { "IO::WaitReadable",             0,     BC_EXCEPTION },
  { "IO::WaitWritable",             0,     BC_EXCEPTION },
  { "LoadError",                    0,     BC_EXCEPTION },
  { "Method",                       0,     BC_CLASS },
  { "NoMemoryError",                0,     BC_EXCEPTION },
  { "ObjectSpace",                  0,     BC_CLASS | BC_MODULE },
  { "Process",                      0,     BC_CLASS | BC_MODULE },
  { "StringScanner_Error",          0,     BC_EXCEPTION },
  { "SystemCallError",              0,     BC_EXCEPTION },
  { "SystemStackError",             0,     BC_EXCEPTION },
  { "UnboundMethod",                0,     BC_CLASS },
};
#define BUILTIN_CLASS_N ((int)(sizeof BUILTIN_CLASSES / sizeof BUILTIN_CLASSES[0]))

/* A socket row is a constant only after `require "socket"`, as in CRuby. */
static const BuiltinClass *builtin_row(const char *n) {
  if (!n) return NULL;
  for (int i = 0; i < BUILTIN_CLASS_N; i++) {
    if (!sp_streq(n, BUILTIN_CLASSES[i].name)) continue;
    if ((BUILTIN_CLASSES[i].flags & BC_SOCKET) && !sp_feature_required("socket")) return NULL;
    return &BUILTIN_CLASSES[i];
  }
  return NULL;
}

int is_builtin_class_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  return r && (r->flags & BC_CLASS);
}
int is_builtin_module_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  return r && (r->flags & BC_MODULE);
}
int is_builtin_exception_name(const char *n) {
  const BuiltinClass *r = builtin_row(n);
  if (r && (r->flags & BC_EXCEPTION)) return 1;
  /* the Errno:: family is open -- the runtime picks a name from errno -- so
     any qualified Errno name is an exception class (#2922 follow-up) */
  return n && strncmp(n, "Errno::", 7) == 0;
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
int class_inherits_builtin_exception(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    if (sty && sp_streq(sty, "ConstantReadNode") &&
        is_builtin_exception_name(nt_str(c->nt, sc, "name")))
      return 1;
    if (sty && sp_streq(sty, "ConstantPathNode") &&
        is_builtin_exception_name(nt_str(c->nt, sc, "name")))
      return 1;
  }
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
/* The source text of the regex literal behind `nid`, or NULL when the pattern
   is only known at run time (an interpolated literal, a `Regexp.new(s)` call,
   a method's return). Deliberately mirrors codegen's re_lit_index resolution
   -- a bare literal, a constant bound to one (`PAT = /re/[.freeze]`), or a
   regex-typed local bound to one -- so a type rule here and the emit arm there
   agree on which patterns are statically visible. Unlike re_lit_index this
   only looks, never registers a pattern slot, so it is safe to call during
   inference. */
const char *an_regex_lit_src(Compiler *c, int nid) {
  const NodeTable *nt = c->nt;
  if (nid < 0) return NULL;
  const char *ty = nt_type(nt, nid);
  if (!ty) return NULL;
  if (sp_streq(ty, "RegularExpressionNode")) return nt_str(nt, nid, "unescaped");
  int want_const = sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode");
  int want_local = sp_streq(ty, "LocalVariableReadNode") && infer_type(c, nid) == TY_REGEX;
  if (!want_const && !want_local) return NULL;
  const char *nm = nt_str(nt, nid, "name");
  if (!nm) return NULL;
  for (int k = 0; k < nt->count; k++) {
    const char *kt = nt_type(nt, k);
    if (!kt) continue;
    if (want_const ? (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode"))
                   : !sp_streq(kt, "LocalVariableWriteNode"))
      continue;
    const char *kn = nt_str(nt, k, "name");
    if (!kn || !sp_streq(kn, nm)) continue;
    int v = nt_ref(nt, k, "value");
    if (want_const && v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze"))
      v = nt_ref(nt, v, "receiver");
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "RegularExpressionNode"))
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
static int *g_cn_first, *g_cn_next, g_cn_cap;
static const NodeTable *g_cn_nt;
static unsigned g_cn_ver;
static int g_cn_cnt = -1;
static int g_cn_empty = -1;
int an_calls_named_first(Compiler *c, const char *name) {
  const NodeTable *nt = c->nt;
  if (!name) return -1;
  if (g_cn_nt != nt || g_cn_ver != nt->version || g_cn_cnt != nt->count) {
    anh_free(&g_cn_names); memset(&g_cn_names, 0, sizeof g_cn_names);
    free(g_cn_first); free(g_cn_next);
    g_cn_cap = 64;
    g_cn_first = malloc(sizeof(int) * (size_t)g_cn_cap);
    g_cn_next = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    int *last = malloc(sizeof(int) * (size_t)g_cn_cap);
    if (!g_cn_first || !g_cn_next || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int id = comp_kind_first(c, NK_CallNode); id >= 0; id = comp_kind_next(c, id)) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm) continue;
      g_cn_next[id] = -1;
      int k = anh_find(&g_cn_names, nm);
      if (k < 0) {
        anh_add(&g_cn_names, nm); k = g_cn_names.n - 1;
        if (k >= g_cn_cap) {
          g_cn_cap *= 2;
          g_cn_first = realloc(g_cn_first, sizeof(int) * (size_t)g_cn_cap);
          last = realloc(last, sizeof(int) * (size_t)g_cn_cap);
          if (!g_cn_first || !last) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        }
        g_cn_first[k] = id;
      }
      else g_cn_next[last[k]] = id;
      last[k] = id;
    }
    free(last);
    g_cn_nt = nt; g_cn_ver = nt->version; g_cn_cnt = nt->count;
  }
  int k = anh_find(&g_cn_names, name);
  return k >= 0 ? g_cn_first[k] : -1;
}
int an_calls_named_next(int id) { return (id >= 0 && id < g_cn_cnt) ? g_cn_next[id] : -1; }

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
   an sp_int slot (#3495, #3496). */
int node_is_empty_container(const NodeTable *nt, int node) {
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
  return argc == 0;
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
    "raise", "warn", "printf", NULL};
  return str_in(name, set);
}
/* A local variable that statically holds exactly one user class (every write
   in its scope assigns the same class constant) resolves to that class index;
   -1 when dynamic. Lets `k = Klass; k.new(...)` and `k.members` dispatch
   statically. */
int class_var_static_ci(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  /* an inline `Struct.new(...)` / `Data.define(...)` receiver resolves to the
     anon class synthesized for that call node (#2682) */
  if (ty && sp_streq(ty, "CallNode") && is_struct_call(c, node))
    return anon_struct_ci_for_value(c, node);
  if (!ty || !sp_streq(ty, "LocalVariableReadNode")) return -1;
  const char *vn = nt_str(nt, node, "name");
  if (!vn) return -1;
  Scope *sc = comp_scope_of(c, node);
  int found = -1;
  /* the (scope, name) chain: the name-keyed one degenerates on machine-
     generated programs, where one local name recurs in thousands of scopes */
  for (int w = comp_lvw_first_sc(c, (int)(sc - c->scopes), vn); w >= 0; w = comp_lvw_next_sc(c, w)) {
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
    int val = nt_ref(nt, w, "value");
    const char *vty = val >= 0 ? nt_type(nt, val) : NULL;
    int ci = (vty && sp_streq(vty, "ConstantReadNode"))
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

/* A local variable that statically holds exactly one BUILTIN class constant
   (every write in its scope assigns the same builtin class name): that name,
   or NULL. The user-class analogue is class_var_static_ci; builtins have no
   class index, so this resolves by name (#2715). */
const char *builtin_class_var_static_name(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_LocalVariableReadNode) return NULL;
  const char *vn = nt_str(nt, node, "name");
  if (!vn) return NULL;
  Scope *sc = comp_scope_of(c, node);
  const char *found = NULL;
  for (int w = comp_lvw_first(c, vn); w >= 0; w = comp_lvw_next(c, w)) {
    const char *wn = nt_str(nt, w, "name");
    if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
    int val = nt_ref(nt, w, "value");
    const char *cn = (val >= 0 && nt_kind(nt, val) == NK_ConstantReadNode)
                     ? nt_str(nt, val, "name") : NULL;
    /* a USER class constant qualifies too: the retargeted receiver then rides
       every ConstantReadNode dispatch arm (method_defined?, subclasses,
       class_eval, ...), not just the sites class_var_static_ci was wired into
       (#2717, #2721) */
    if (!cn || !(is_builtin_class_name(cn) || comp_class_index(c, cn) >= 0)) return NULL;
    if (found && !sp_streq(found, cn)) return NULL;   /* two classes: dynamic */
    found = cn;
  }
  return found;
}

/* The literal symbol behind a symbol-typed expression: a SymbolNode itself,
   or a local variable whose only write (in its scope, plain write) is one.
   Lets inject(:op)-style operator selection see through `s = :+; a.inject(s)`.
   Returns the symbol's name, or NULL. */
const char *sym_static_value(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return NULL;
  const char *ty = nt_type(nt, node);
  if (ty && sp_streq(ty, "SymbolNode")) return nt_str(nt, node, "value");
  if (!ty || !sp_streq(ty, "LocalVariableReadNode")) return NULL;
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
  const char *vty = nt_type(nt, val);
  return (vty && sp_streq(vty, "SymbolNode")) ? nt_str(nt, val, "value") : NULL;
}

/* A CallNode chain that evaluates to a Lazy (a `.lazy` somewhere in the
   receiver spine, and a lazy-producing transform -- not a forcing terminal --
   on top). Such a value has no runtime representation in spinel; it is only
   ever fused at a forcing site. (#2932) */
/* See compiler.h for the masks. Every name carries SP_MUT_LOCAL; the
   narrower sites drop the mutators their storage shape cannot serve:
     append_as_bytes            has no guard-narrowed re-route arm
   `[]=` insert slice! setbyte reach an ivar, and a reader call handing out
   its handle, through the shared-mutable shim's shadow (#4363). */
int sp_str_mutator(const char *nm, unsigned want) {
  static const struct { const char *nm; unsigned mask; } M[] = {
    { "[]=",             SP_MUT_LOCAL | SP_MUT_CONTAINER | SP_MUT_IVAR },
    { "insert",          SP_MUT_LOCAL | SP_MUT_CONTAINER | SP_MUT_IVAR },
    { "slice!",          SP_MUT_LOCAL | SP_MUT_CONTAINER | SP_MUT_IVAR },
    { "setbyte",         SP_MUT_LOCAL | SP_MUT_CONTAINER | SP_MUT_IVAR },
    { "append_as_bytes", SP_MUT_LOCAL | SP_MUT_CONTAINER | SP_MUT_IVAR },
    { "<<",              15u }, { "concat",         15u }, { "prepend",    15u },
    { "replace",         15u }, { "clear",          15u }, { "bytesplice", 15u },
    { "gsub!",           15u }, { "sub!",           15u }, { "upcase!",    15u },
    { "downcase!",       15u }, { "capitalize!",    15u }, { "swapcase!",  15u },
    { "strip!",          15u }, { "lstrip!",        15u }, { "rstrip!",    15u },
    { "chomp!",          15u }, { "chop!",          15u }, { "squeeze!",   15u },
    { "tr!",             15u }, { "delete!",        15u }, { "tr_s!",      15u },
    { "delete_prefix!",  15u }, { "delete_suffix!", 15u }, { "reverse!",   15u },
    { "succ!",           15u }, { "next!",          15u },
    { NULL, 0 }
  };
  if (!nm) return 0;
  for (int i = 0; M[i].nm; i++)
    if (sp_streq(nm, M[i].nm)) return (M[i].mask & want) == want;
  return 0;
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
  for (int i = 0; ST[i]; i++) if (sp_streq(nm, ST[i])) return 1;
  return 0;
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
  if (node < 0 || !nt_type(nt, node) || !sp_streq(nt_type(nt, node), "CallNode")) return 0;
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
    if (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
      if (++hops > 8) return 0;
      int a = lazy_alias_chain(c, cur);
      if (a < 0) return 0;
      cur = a;
      continue;
    }
    if (!nt_type(nt, cur) || !sp_streq(nt_type(nt, cur), "CallNode")) return 0;
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
  int val = -1;
  /* Walk the write kinds through the node table's own kind index rather than
     rescanning every node: the query runs per lazy-receiver read on every
     fixpoint iteration, and all five kinds together are a small slice of the
     table. The answer does not depend on the order writes are visited in --
     it is "exactly one matching write, plain kind, same scope" -- so taking
     them kind by kind gives what the linear scan gave. */
  static const NodeKind LOCAL_WRITE_KINDS[] = {
    NK_LocalVariableWriteNode, NK_LocalVariableOrWriteNode,
    NK_LocalVariableAndWriteNode, NK_LocalVariableOperatorWriteNode,
    NK_LocalVariableTargetNode };
  for (size_t ki = 0; ki < sizeof LOCAL_WRITE_KINDS / sizeof *LOCAL_WRITE_KINDS; ki++) {
    NodeKind k = LOCAL_WRITE_KINDS[ki];
    NT_FOREACH_KIND(nt, k, w) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn)) continue;
      if (k != NK_LocalVariableWriteNode || comp_scope_of(c, w) != sc || val >= 0)
        return -1;   /* not a single plain write */
      val = nt_ref(nt, w, "value");
    }
  }
  if (val < 0) return -1;
  return chain_is_lazy_valued(c, val) ? val : -1;
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
  int m = comp_method_index(c, nm);
  if (m < 0 || c->scopes[m].nparams != 0) return -1;
  int body = c->scopes[m].body;
  if (body < 0) return -1;
  int n = 0; const int *st = nt_arr(nt, body, "body", &n);
  if (n != 1 || !st) return -1;
  if (!chain_is_lazy_valued(c, st[0])) return -1;
  return lazy_chain_self_free(c, st[0], 0) ? st[0] : -1;
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
    if (!nt_type(nt, argv[a]) || !sp_streq(nt_type(nt, argv[a]), "SymbolNode")) continue;
    const char *m = nt_str(nt, argv[a], "value");
    if (!m) continue;
    for (int b = 0; b < a; b++) {
      if (!nt_type(nt, argv[b]) || !sp_streq(nt_type(nt, argv[b]), "SymbolNode")) continue;
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
  if (sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) {
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
  if (!sp_streq(nt_type(nt, recv), "CallNode")) return -1;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm || (!sp_streq(nm, "new") && !sp_streq(nm, "__hash_new_default"))) return -1;
  int cr = nt_ref(nt, recv, "receiver");
  if (cr < 0 || !nt_type(nt, cr) || !sp_streq(nt_type(nt, cr), "ConstantReadNode")) return -1;
  const char *cn = nt_str(nt, cr, "name");
  if (!cn || !sp_streq(cn, "Hash")) return -1;
  if (nt_ref(nt, recv, "block") >= 0) return -1;   /* block default: dproc path */
  int a = nt_ref(nt, recv, "arguments");
  int ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  if (ac != 1 || !av) return -1;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "KeywordHashNode")) return -1;
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
  if (t != TY_UNKNOWN) return t;
  NodeKind k = nt_kind(c->nt, dn);
  if (k == NK_ArrayNode || k == NK_HashNode || k == NK_KeywordHashNode) return TY_POLY;
  return TY_UNKNOWN;
}

/* Is CONSTNAME written directly in the body of `class CLSNAME`? Constants live
   in one flat namespace, so the AST is what says which class owns one -- what
   const_defined?/const_get need for an `inherit: false` search (#3762). */
static int cn_body_writes_const(const NodeTable *nt, int root, const char *constname, int top) {
  if (root < 0) return 0;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (!top && (sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode"))) return 0;
    if (sp_streq(ty, "ConstantWriteNode")) {
      const char *n = nt_str(nt, root, "name");
      if (n && sp_streq(n, constname)) return 1;
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (cn_body_writes_const(nt, nt_ref_at(nt, root, i), constname, 0)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (cn_body_writes_const(nt, el[j], constname, 0)) return 1;
  }
  return 0;
}
int const_owned_by_class(Compiler *c, const char *clsname, const char *constname) {
  const NodeTable *nt = c->nt;
  if (!clsname || !constname) return 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "ClassNode")) continue;
    /* a ClassNode carries its name through constant_path, not a name field */
    int cp = nt_ref(nt, id, "constant_path");
    const char *n = cp >= 0 ? nt_str(nt, cp, "name") : nt_str(nt, id, "name");
    if (!n || !sp_streq(n, clsname)) continue;
    int body = nt_ref(nt, id, "body");
    if (body >= 0 && cn_body_writes_const(nt, body, constname, 1)) return 1;
  }
  return 0;
}

/* Does this receiver denote a Hash built by a blockless `Hash.new` (or an
   empty Hash literal)? Such a hash carries no default block. Traces a local's
   writes the way hash_new_default_arg does (#3568). */
int hash_new_blockless(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || !nt_type(nt, recv)) return 0;
  const char *ty = nt_type(nt, recv);
  if (sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode")) return 1;
  if (sp_streq(ty, "LocalVariableReadNode")) {
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
  if (!sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, recv, "name");
  if (!nm || !sp_streq(nm, "new")) return 0;
  int cr = nt_ref(nt, recv, "receiver");
  if (cr < 0 || !nt_type(nt, cr) || !sp_streq(nt_type(nt, cr), "ConstantReadNode")) return 0;
  const char *cn = nt_str(nt, cr, "name");
  if (!cn || !sp_streq(cn, "Hash")) return 0;
  return nt_ref(nt, recv, "block") < 0;
}

int struct_member_idx(Compiler *c, ClassInfo *sc, int keynode) {
  const NodeTable *nt = c->nt;
  const char *kty = nt_type(nt, keynode);
  if (!kty) return -1;
  if (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode")) {
    const char *kn = sp_streq(kty, "SymbolNode") ? nt_str(nt, keynode, "value")
                                                 : nt_str(nt, keynode, "content");
    if (!kn) return -1;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kn);
    int iv = comp_ivar_index(sc, ivn);
    return iv;  /* ivar order == member order */
  }
  if (sp_streq(kty, "IntegerNode")) {
    int idx = (int)nt_int(nt, keynode, "value", INT_MIN);
    if (idx < 0) idx += sc->nivars;   /* negative counts from the last member */
    if (idx >= 0 && idx < sc->nivars) return idx;
  }
  return -1;
}
int scope_body_last(Compiler *c, int mi) {
  int body = c->scopes[mi].body;
  if (body < 0 || !nt_type(c->nt, body) || !sp_streq(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *bb = nt_arr(c->nt, body, "body", &n);
  return n > 0 ? bb[n - 1] : -1;
}
int is_blk_param_call(Compiler *c, int node, int mi) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node) || !sp_streq(nt_type(nt, node), "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]"))) return 0;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  const char *bp = c->scopes[mi].blk_param;
  return rn && bp && bp[0] && sp_streq(rn, bp);
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
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "DefNode")) return TY_UNKNOWN;
  if (sp_streq(ty, "BlockNode") && depth > 0) return TY_UNKNOWN; /* inner block's breaks don't escape */
  if (sp_streq(ty, "BreakNode")) {
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
    const char *tty = nt_type(nt, vargs[0]);
    const char *tnm = NULL;
    if (tty && sp_streq(tty, "SymbolNode")) tnm = nt_str(nt, vargs[0], "value");
    else if (tty && sp_streq(tty, "StringNode")) tnm = nt_str(nt, vargs[0], "unescaped");
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
  return an == 1 && av && nt_type(nt, av[0]) &&
         sp_streq(nt_type(nt, av[0]), "ForwardingArgumentsNode");
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
static unsigned yvt_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}
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
    if (!cty) continue;
    /* a `super` reaching a yielding parent is a call site of that parent: the
       block that arrives is the child's own */
    if (sp_streq(cty, "SuperNode") || sp_streq(cty, "ForwardingSuperNode")) {
      yvt_sup_ids[yvt_sup_n++] = cid;
      continue;
    }
    if (!sp_streq(cty, "CallNode")) continue;
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
    int b = (int)(yvt_hash(cn) % (unsigned)yvt_nb);
    yvt_nm_next[ii] = yvt_nm_head[b]; yvt_nm_head[b] = ii;
  }
  for (int ii = 0; ii < yvt_n; ii++)
    if (!nt_str(nt, yvt_ids[ii], "name") || (yvt_alias_name && yvt_alias_name[ii]))
      yvt_always[yvt_always_n++] = ii;
}
/* The entries that may reach `mi`, ascending: its name's chain, `new`'s for
   an initialize, and the always-list, merged. A method with no name, or a
   prepend shadow, takes every entry (see yvt_may_reach). */
typedef struct { int s[3]; int ai; int all; int next_all; int last; } YvtIt;
static void yvt_it_init(Compiler *c, YvtIt *it, int mi) {
  const char *mn = c->scopes[mi].name;
  it->all = !mn || strncmp(mn, "__prep_", 7) == 0 || !yvt_nm_head;
  it->next_all = 0; it->last = -1; it->ai = 0;
  it->s[0] = it->s[1] = -1;
  if (it->all) return;
  it->s[0] = yvt_nm_head[yvt_hash(mn) % (unsigned)yvt_nb];
  if (sp_streq(mn, "initialize")) it->s[1] = yvt_nm_head[yvt_hash("new") % (unsigned)yvt_nb];
}
static int yvt_it_next(YvtIt *it) {
  if (it->all) return it->next_all < yvt_n ? it->next_all++ : -1;
  for (;;) {
    int a = it->s[0], b = it->s[1];
    int c3 = it->ai < yvt_always_n ? yvt_always[it->ai] : -1;
    int m = -1;
    if (a >= 0) m = a;
    if (b >= 0 && (m < 0 || b < m)) m = b;
    if (c3 >= 0 && (m < 0 || c3 < m)) m = c3;
    if (m < 0) return -1;
    if (a == m) it->s[0] = yvt_nm_next[a];
    if (b == m) it->s[1] = yvt_nm_next[b];
    if (c3 == m) it->ai++;
    if (m > it->last) { it->last = m; return m; }
  }
}
/* Which method does block-passing call site `cid` reach? Shared by
   yield_value_type and yield_block_tails so the two agree on what counts as a
   call site of a given method. */
static int yvt_callee_index(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  const char *cn = nt_str(nt, cid, "name");
  int crecv = nt_ref(nt, cid, "receiver");
  int rmi = -1;
  if (crecv < 0) {
    rmi = comp_cbody_call_mi(c, cid, cn);
    if (rmi < 0) rmi = comp_method_index(c, cn);
    if (rmi < 0) {
      Scope *cs = comp_scope_of(c, cid);
      if (cs->class_id >= 0) {
        rmi = comp_method_in_chain(c, cs->class_id, cn, NULL);
        /* an implicit-self call inside a class method resolves to a CLASS
           method; its block/forward feeds that method's blk_ret too */
        if (rmi < 0) rmi = comp_cmethod_in_chain(c, cs->class_id, cn, NULL);
      }
    }
  }
  else {
    TyKind crt = infer_type(c, crecv);
    if (ty_is_object(crt)) rmi = comp_method_in_chain(c, ty_object_class(crt), cn, NULL);
    /* `Klass.new { block }`: the block feeds the class's initialize. A
       scoped receiver (`NS::Klass`) resolves by its leaf name, the key
       classes are indexed under. */
    else if (cn && sp_streq(cn, "new") && nt_type(nt, crecv) &&
             (sp_streq(nt_type(nt, crecv), "ConstantReadNode") ||
              sp_streq(nt_type(nt, crecv), "ConstantPathNode"))) {
      int nci = comp_class_index(c, nt_str(nt, crecv, "name"));
      if (nci >= 0) rmi = comp_method_in_chain(c, nci, "initialize", NULL);
    }
    /* `Klass.m { block }` / `Mod.m { block }` / `NS::Klass.m { block }`: a
       class/module self-method (singleton). Resolve the constant and look
       up its singleton method so the block's value type flows back to m's
       return type -- instance methods resolve via the ty_is_object arm
       above (#1446). */
    else if (nt_type(nt, crecv) &&
             (sp_streq(nt_type(nt, crecv), "ConstantReadNode") ||
              sp_streq(nt_type(nt, crecv), "ConstantPathNode"))) {
      int nci = comp_class_index(c, nt_str(nt, crecv, "name"));
      if (nci >= 0) rmi = comp_cmethod_in_chain(c, nci, cn, NULL);
    }
  }
  return rmi;
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
    const char *aty = nt_type(nt, av[0]);
    if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
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

TyKind yield_value_type(Compiler *c, int mi) {
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return TY_UNKNOWN;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return TY_UNKNOWN;
  g_yvt_mi[g_yvt_depth++] = mi;

  const NodeTable *nt = c->nt;
  TyKind result = TY_UNKNOWN;
  if (yvt_nt != nt || yvt_ntc != nt->count) yvt_build(c);
  YvtIt yit; yvt_it_init(c, &yit, mi);
  for (int ii; (ii = yvt_it_next(&yit)) >= 0; ) {
    int cid = yvt_ids[ii];
    int blk = nt_ref(nt, cid, "block");
    /* A `callee(...)` forward carries its block implicitly inside the `...`
       (no explicit block node); treat it as a forwarded block too. */
    if (!yvt_may_reach(c, ii, mi)) continue;
    int fwd_args = yvt_fwd ? yvt_fwd[ii] : yvt_call_forwards_block(nt, cid);
    /* skip calls that live inside method mi itself (recursive self-calls);
       only external call sites provide a concrete block value type */
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    if (yvt_callee_index(c, cid) != mi) continue;
    /* `mi(&b)` / `mi(...)`: the call forwards the block of its enclosing
       method rather than passing a literal. The value `mi` yields is then
       whatever that forwarded block produces -- the enclosing method's
       own yield value. */
    const char *blkty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (fwd_args || (blkty && sp_streq(blkty, "BlockArgumentNode"))) {
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      /* `mi(&some_proc)`: a first-class Proc / lambda / Method value, not the
         enclosing method's own block forwarded on. Its result is whatever the
         proc answers at run time, i.e. poly -- typing it from the (absent)
         enclosing block made the whole call answer nil (#3688). */
      if (!fwd_args && blk >= 0) {
        int bexpr = nt_ref(nt, blk, "expression");
        const char *bpn = (encl && encl->blk_param && encl->blk_param[0]) ? encl->blk_param : NULL;
        const char *ben = (bexpr >= 0 && nt_kind(nt, bexpr) == NK_LocalVariableReadNode)
                            ? nt_str(nt, bexpr, "name") : NULL;
        if (!(bpn && ben && sp_streq(bpn, ben))) {
          /* A lambda/proc LITERAL right there types like an ordinary literal
             block -- its tail expression is the value the call yields. Any
             other callable (a proc read from a local, a Method) is only known
             at run time, so poly. */
          TyKind pt = TY_POLY;
          int pbody = -1;
          if (nt_kind(nt, bexpr) == NK_LambdaNode) pbody = nt_ref(nt, bexpr, "body");
          else if (nt_kind(nt, bexpr) == NK_CallNode) {
            const char *pnm = nt_str(nt, bexpr, "name");
            int pblk = nt_ref(nt, bexpr, "block");
            if (pnm && pblk >= 0 && (sp_streq(pnm, "proc") || sp_streq(pnm, "lambda")) &&
                nt_kind(nt, pblk) == NK_BlockNode)
              pbody = nt_ref(nt, pblk, "body");
          }
          if (pbody >= 0) {
            int pn2 = 0; const int *pd = nt_arr(nt, pbody, "body", &pn2);
            if (pn2 == 0) pt = TY_NIL;
            else { pt = infer_type(c, pd[pn2 - 1]); if (pt == TY_VOID) pt = TY_NIL; }
            if (pt == TY_UNKNOWN) pt = TY_POLY;
          }
          if (c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) { result = pt; break; }
          result = ty_unify(result, pt);
          continue;
        }
      }
      TyKind ft = (emi >= 0 && emi != mi) ? yield_value_type(c, emi) : TY_UNKNOWN;
      if (ft == TY_VOID) ft = TY_NIL;
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
    else if (nt_type(nt, bd[bn - 1]) && sp_streq(nt_type(nt, bd[bn - 1]), "ReturnNode"))
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
  g_yvt_depth--;
  return result;
}

/* The tail expression of every literal block that can reach `mi`'s yield.
   This is yield_value_type's call-site scan answering with NODES instead of a
   unified type, because nilability is a property of the producing EXPRESSION,
   not of the type it settles on -- an `Integer?` and an `Integer` both arrive
   as TY_INT, so the type alone cannot say whether the value can be the
   reserved scalar nil sentinel (#3505). A forwarded block (`m(&b)` / `m(...)`)
   has no literal of its own and delegates to the enclosing method's tails.
   Unlike yield_value_type this collects EVERY site rather than stopping at the
   first concrete one: the caller ORs the results, and a single nilable block
   anywhere is enough to make the slot nilable. Writes at most `max` ids and
   returns how many. */
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
    if (yvt_callee_index(c, cid) != mi) continue;
    const char *blkty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (fwd_args || (blkty && sp_streq(blkty, "BlockArgumentNode"))) {
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      if (emi >= 0 && emi != mi) n += yield_block_tails(c, emi, out + n, max - n);
      continue;
    }
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    if (bd && bn > 0) out[n++] = bd[bn - 1];
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
    int p = c->classes[cs->class_id].parent;
    if (p < 0) continue;
    int rmi = cs->is_cmethod ? comp_cmethod_in_chain(c, p, cs->name, NULL)
                             : comp_method_in_chain(c, p, cs->name, NULL);
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
/* Does class ci descend from an explicit `< BasicObject` (a blank slate)?
   Such an instance answers only BasicObject's own methods and what the user
   defined; the Object/Kernel default arms must NOT serve it (#2703). */
int class_is_blank_slate(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    const char *sn = sc >= 0 ? nt_str(c->nt, sc, "name") : NULL;
    if (sn && sp_streq(sn, "BasicObject")) return 1;
    if (sn && c->classes[k].parent < 0) return 0;   /* rooted at another builtin */
  }
  return 0;
}

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
static unsigned udm_hash(const char *s) {
  unsigned h = 2166136261u;
  for (const char *p = s; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}
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
        unsigned j = udm_hash(s->v[i]) & (unsigned)(ncap - 1);
        while (nv[j]) j = (j + 1) & (unsigned)(ncap - 1);
        nv[j] = s->v[i];
      }
    free(s->v); s->v = nv; s->cap = ncap;
  }
  unsigned j = udm_hash(k) & (unsigned)(s->cap - 1);
  while (s->v[j]) {
    if (sp_streq(s->v[j], k)) return;
    j = (j + 1) & (unsigned)(s->cap - 1);
  }
  s->v[j] = strdup(k);
  if (s->v[j]) s->n++;
}
static int udm_set_has(const UdmSet *s, const char *k) {
  if (!s->cap) return 0;
  unsigned j = udm_hash(k) & (unsigned)(s->cap - 1);
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
  unsigned b = udm_cap ? udm_hash(name) & (unsigned)(udm_cap - 1) : 0;
  if (udm_cap)
    for (int i = udm_head[b]; i >= 0; i = udm_next[i])
      if (sp_streq(udm_names[i], name)) return udm_ans[i];
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
  int n = node;
  while (n >= 0 && nt_type(nt, n) && sp_streq(nt_type(nt, n), "ParenthesesNode")) {
    int body = nt_ref(nt, n, "body"); int bn = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    n = bn == 1 ? bd[0] : -1;
  }
  if (n >= 0 && nt_type(nt, n) && sp_streq(nt_type(nt, n), "YieldNode")) {
    Scope *sc = comp_scope_of(c, n);
    int mi = sc ? (int)(sc - c->scopes) : -1;
    if (mi >= 0 && yield_value_diverges(c, mi)) return TY_POLY;
  }
  return infer_type(c, node);
}

/* The statement a call with a block answers from, when the body ends in
   `if block_given? ... else ... end`: the block arm's last statement. */
static int block_given_tail_then_last(Compiler *c, int last) {
  const NodeTable *nt = c->nt;
  if (last < 0 || nt_kind(nt, last) != NK_IfNode) return -1;
  int pred = nt_ref(nt, last, "predicate");
  if (pred < 0 || nt_kind(nt, pred) != NK_CallNode || nt_ref(nt, pred, "receiver") >= 0) return -1;
  const char *pn = nt_str(nt, pred, "name");
  if (!pn || !sp_streq(pn, "block_given?")) return -1;
  int sub = nt_ref(nt, last, "subsequent");
  if (sub < 0 || nt_kind(nt, sub) != NK_ElseNode) return -1;
  int ts = nt_ref(nt, last, "statements");
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
  for (int cid = 0; cid < nt->count && !mixed; cid++) {
    if (nt_kind(nt, cid) != NK_CallNode) continue;
    const char *cn = nt_str(nt, cid, "name");
    if (!cn || !mn || !sp_streq(cn, mn)) continue;
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    if (yvt_callee_index(c, cid) != mi) continue;
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

TyKind method_call_ret(Compiler *c, int mi, int call_id) {
  int last = scope_body_last(c, mi);
  /* `if block_given? ... yield ... else ... end`: the call with a block
     answers from the block arm, typed per call site like a yield tail */
  { int tl = block_given_tail_then_last(c, last); if (tl >= 0) last = tl; }
  int is_yield = last >= 0 && nt_type(c->nt, last) && sp_streq(nt_type(c->nt, last), "YieldNode");
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
  /* A yield-tailed inlined method, or a tail that calls the &block param:
     the block is spliced per call site, so use that site's block body type. */
  if (is_yield || is_blk_param_call(c, last, mi)) {
    int blk = nt_ref(c->nt, call_id, "block");
    const char *bty = blk >= 0 ? nt_type(c->nt, blk) : NULL;
    /* `callee(&b)` / `callee(...)` forwards the block active in the enclosing
       method (a `...` forward carries it implicitly, with no block node), so
       the value `callee` yields is whatever that forwarded block produces --
       i.e. the enclosing method's own per-call-site yield value. */
    int fwd = (bty && sp_streq(bty, "BlockArgumentNode"));
    if (!fwd && blk < 0) {
      int a = nt_ref(c->nt, call_id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
      fwd = (an == 1 && av && nt_type(c->nt, av[0]) &&
             sp_streq(nt_type(c->nt, av[0]), "ForwardingArgumentsNode"));
    }
    if (fwd) {
      Scope *encl = comp_scope_of(c, call_id);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      if (emi >= 0 && emi != mi) {
        TyKind ft = yield_value_type(c, emi);
        if (ft != TY_UNKNOWN && ft != TY_VOID) return ft;
      }
    }
    if (blk >= 0) {
      int bbody = nt_ref(c->nt, blk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn > 0) {
        const char *lty = nt_type(c->nt, bb[bn - 1]);
        if (lty && sp_streq(lty, "ReturnNode"))
          return return_node_type(c, bb[bn - 1]);  /* `{ return e }`: see yield_value_type */
        return infer_type(c, bb[bn - 1]);
      }
    }
  }
  /* `return x unless block_given?`: a blockless call site reads the value
     of that return, one with a block the value of the body proper */
  if (c->scopes[mi].yields && c->scopes[mi].ret_noblock != TY_UNKNOWN) {
    int blk = nt_ref(c->nt, call_id, "block");
    int fwd = 0;
    if (blk < 0) {
      int a = nt_ref(c->nt, call_id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
      fwd = (an == 1 && av && nt_type(c->nt, av[0]) &&
             sp_streq(nt_type(c->nt, av[0]), "ForwardingArgumentsNode"));
    }
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
  if (!ty) return 0;
  if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
    const char *nm = nt_str(nt, n, "name");
    return nm && sp_streq(nm, "Proc");
  }
  return 0;
}
int is_proc_literal(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 && name && (sp_streq(name, "proc") || sp_streq(name, "lambda"))) return 1;
  if (recv >= 0 && name && sp_streq(name, "new") && is_proc_constant(nt, recv)) return 1;
  return 0;
}
/* A block lowered to a REAL sp_Proc by its consumer -- a deferred handler
   (at_exit / Signal.trap, #2836) or an ENV block mutator (#2832). These need
   the full proc capture machinery but keep their own call-level inference
   (trap returns the previous handler, not the proc). */
int is_handler_proc_block(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  if (sp_streq(name, "at_exit") || sp_streq(name, "trap")) {
    if (recv < 0) return 1;
    const char *hrty = nt_type(nt, recv);
    return hrty && sp_streq(hrty, "ConstantReadNode") &&
           nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Signal");
  }
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "ENV") &&
      (sp_streq(name, "delete_if") || sp_streq(name, "reject!") ||
       sp_streq(name, "keep_if") || sp_streq(name, "select!") ||
       sp_streq(name, "filter!")))
    return 1;
  return 0;
}
int is_proc_create(Compiler *c, int id) {
  const char *ty = nt_type(c->nt, id);
  if (ty && sp_streq(ty, "LambdaNode")) return 1;
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
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "ReturnNode")) return return_node_type(c, node);
  /* A nested def/class/module/lambda owns its own returns; stop there. A plain
     block is return-transparent -- a `return` lexically inside a `do..end`
     block returns non-locally from the enclosing proc/lambda, so its type must
     fold in (#3241). A block that IS a nested proc literal's block owns its own
     frame, so it is not descended into. */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") ||
      sp_streq(ty, "ModuleNode") || sp_streq(ty, "LambdaNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, node, i);
    if (ch >= 0 && nt_type(nt, ch) && sp_streq(nt_type(nt, ch), "BlockNode") &&
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
  const char *ty = nt_type(nt, create);
  int body;
  if (ty && sp_streq(ty, "LambdaNode")) body = nt_ref(nt, create, "body");
  else {
    int blk = nt_ref(nt, create, "block");
    /* `Proc.new(&b)` / `proc(&b)`: no body of its own -- the value is the
       forwarded proc, whose return is unknowable here (a block param's
       actual block lives at the caller). TY_UNKNOWN maps to the boxed poly
       return at the .call site instead of a silent nil. */
    if (blk >= 0 && nt_type(nt, blk) && sp_streq(nt_type(nt, blk), "BlockArgumentNode"))
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
  const char *tty = nt_type(nt, tail);
  TyKind tail_ty = (tty && sp_streq(tty, "ReturnNode")) ? return_node_type(c, tail)
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
  if (!ty) return TY_UNKNOWN;
  /* unwrap `(expr)` so `(f << g).call` sees the composition node */
  if (sp_streq(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, node, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn == 1 ? proc_ret_of(c, bb[0]) : TY_UNKNOWN;
  }
  if (sp_streq(ty, "LambdaNode") || is_proc_literal(c, node)) return proc_node_ret(c, node);
  if (sp_streq(ty, "LocalVariableReadNode")) {
    Scope *s = comp_scope_of(c, node);
    LocalVar *lv = scope_local(s, nt_str(nt, node, "name"));
    return lv ? (TyKind)lv->proc_ret : TY_UNKNOWN;
  }
  if (sp_streq(ty, "CallNode")) {
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
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
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
int comp_self_call_mi(Compiler *c, int id, const char *name) {
  if (!name) return -1;
  Scope *self = comp_scope_of(c, id);
  int mi = -1;
  if (self && self->class_id >= 0) {
    /* inside a class method self IS the class, so a bare call reaches sibling
       class methods; inside an instance method it does not */
    if (self->is_cmethod) mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
    if (mi < 0) mi = comp_method_in_chain(c, self->class_id, name, NULL);
  }
  if (mi < 0) mi = comp_method_index(c, name);
  return mi;
}

/* A receiverless call directly in a class body is sent to the class. */
int comp_cbody_call_mi(Compiler *c, int id, const char *name) {
  Scope *s = comp_scope_of(c, id);
  if (!name || (s && s->name) || c->node_cbody[id] < 0) return -1;
  return comp_cmethod_in_chain(c, c->node_cbody[id], name, NULL);
}

TyKind proc_call_ret(Compiler *c, int recv) {
  TyKind r = proc_ret_of(c, recv);
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
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "SymbolNode")) return nt_str(nt, av[0], "value");
  if (aty && sp_streq(aty, "StringNode")) {
    const char *s = nt_str(nt, av[0], "content");
    return s ? s : nt_str(nt, av[0], "unescaped");
  }
  return NULL;
}

/* True if `node` is a `method(:sym)` / `<recv>.method(:sym)` call. */
int is_method_obj_call(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node) || !sp_streq(nt_type(nt, node), "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  /* Module#instance_method builds the same object, unbound (NULL self); the
     Method machinery (name/arity/owner/call-after-bind) rides it (#2676) */
  return nm && (sp_streq(nm, "method") || sp_streq(nm, "instance_method")) &&
         method_sym_arg(c, node) != NULL;
}

/* The target method scope index bound by a `method(:sym)` node, or -1
   (e.g. a top-level Kernel method like `puts`, or a builtin-array receiver). */
int method_obj_target_mi(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  /* <method>.super_method: the same-named method one step up the defining
     class's ancestor chain, or -1 (nil) when there is none (#3247). */
  if (node >= 0 && nt_kind(nt, node) == NK_CallNode && nt_str(nt, node, "name") &&
      sp_streq(nt_str(nt, node, "name"), "super_method")) {
    int imn = method_recv_node(c, nt_ref(nt, node, "receiver"));
    int imi = imn >= 0 ? method_obj_target_mi(c, imn) : -1;
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
      const char *rn = (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode)
                       ? nt_str(nt, recv, "name") : NULL;
      int ci = rn ? comp_class_index(c, rn) : -1;
      return ci >= 0 ? comp_method_in_chain(c, ci, sym, NULL) : -1;
    }
  }
  if (recv < 0) {
    int mi = comp_method_index(c, sym);
    if (mi < 0) { Scope *s = comp_scope_of(c, node); if (s && s->class_id >= 0) mi = comp_method_in_chain(c, s->class_id, sym, NULL); }
    return mi;
  }
  TyKind rt = infer_type(c, recv);
  /* a receiver whose method() was retargeted at a synthesized __bam_* wrapper
     (desugar_builtin_method_obj) -- for an object accessor the wrapper is a
     TOP-LEVEL def, not in the object's own class chain, so resolve it there. */
  if (sym[0] == '_' && sym[1] == '_' && sym[2] == 'b' && sym[3] == 'a' && sym[4] == 'm')
    return comp_method_index(c, sym);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), sym, NULL);
  /* Klass.method(:cmeth) / Module.method(:mf): the class-side method */
  if (rt == TY_CLASS && nt_kind(nt, recv) == NK_ConstantReadNode) {
    const char *rn2 = nt_str(nt, recv, "name");
    int ci2 = rn2 ? comp_class_index(c, rn2) : -1;
    if (ci2 >= 0) return comp_cmethod_in_chain(c, ci2, sym, NULL);
  }
  return -1;
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
    if (sp_streq(op, "[]") || sp_streq(op, "[]=")) return TY_INT;
  }
  else if (arr == TY_STR_ARRAY) {
    if (sp_streq(op, "push")) return TY_STR_ARRAY;
    if (sp_streq(op, "[]") || sp_streq(op, "[]=")) return TY_STRING;
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
  const char *rty = nt_type(nt, recv);
  if (rty && sp_streq(rty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
      if (comp_scope_of(c, w) != sc) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !vn || !sp_streq(wn, vn)) continue;
      int val = nt_ref(nt, w, "value");
      /* resolve the written expression with the same rules as a direct
         receiver (sees through bind/dup/clone chains, super_method) */
      int inner = method_recv_node(c, val);
      if (inner >= 0) return inner;
    }
  }
  return -1;
}

/* The `method(:sym)` node behind a Proc-typed expression created by
   `<method>.to_proc`: the to_proc call itself, or a local variable's
   single assignment to one. Returns -1 when the proc has another origin. */
int proc_to_proc_method_node(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  int cand = recv;
  const char *rty = nt_type(nt, recv);
  if (rty && sp_streq(rty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    cand = -1;
    /* the writes only, through the kind index (the first one that is a call) */
    NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
      if (cand >= 0) continue;
      if (comp_scope_of(c, w) != sc) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !vn || !sp_streq(wn, vn)) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && nt_kind(nt, val) == NK_CallNode) cand = val;
    }
  }
  if (cand < 0) return -1;
  const char *cty = nt_type(nt, cand);
  if (!cty || !sp_streq(cty, "CallNode")) return -1;
  const char *nm = nt_str(nt, cand, "name");
  if (!nm || !sp_streq(nm, "to_proc")) return -1;
  return method_recv_node(c, nt_ref(nt, cand, "receiver"));
}

/* Param-index shift for a call through a Method object. A bound
   `<recv>.method(:__bam_N)` resolves to a synthesized top-level wrapper whose
   first param (__bam_r) is carried by the Method's self slot, so positional
   call args map to params[1..]. A real instance/class method keeps self
   implicit (params are the declared ones) and shifts by 0. */
int method_call_param_shift(Compiler *c, int mn, int mi) {
  if (mn < 0 || mi < 0) return 0;
  if (nt_ref(c->nt, mn, "receiver") < 0) return 0;
  Scope *m = &c->scopes[mi];
  return (m->class_id < 0 && !m->is_cmethod) ? 1 : 0;
}

/* True when scope `scope_idx` contains an explicit `return` (such a method
   cannot be inlined at its call sites). Shared by the inliner and the
   valued-break detector. */
int scope_has_return(Compiler *c, int scope_idx) {
  NT_FOREACH_KIND(c->nt, NK_ReturnNode, id)
    if (c->nscope[id] == scope_idx) return 1;
  return 0;
}

/* Resolve a block-bearing CallNode to an INLINE-ABLE yielding user method:
   mirrors emit_inline_call_x's resolution (free function -> implicit-self
   chain -> Cls class method -> object-receiver chain) and its
   yields/!return guard. -1 for anything else -- builtin iterators, `loop`,
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
    const char *rty = nt_type(nt, recv);
    /* a scoped receiver (NS::Base.transaction { }) resolves by its leaf name,
       the key classes are indexed under */
    const char *cname = (rty && (sp_streq(rty, "ConstantReadNode") ||
                                 sp_streq(rty, "ConstantPathNode")))
                        ? nt_str(nt, recv, "name") : NULL;
    int ci = cname ? comp_class_index(c, cname) : -1;
    if (ci >= 0) mi = comp_cmethod_in_chain(c, ci, name, NULL);
    else if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  }
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return -1;
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
  const char *rty = nt_type(nt, recv);
  if (!rty || !sp_streq(rty, "LocalVariableReadNode")) return -1;
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
  while (val >= 0 && nt_kind(nt, val) == NK_ParenthesesNode) {
    int pb = nt_ref(nt, val, "body");
    int pn = 0;
    const int *pp = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    val = pn == 1 ? pp[0] : -1;
  }
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
