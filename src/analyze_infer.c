#include "analyze_internal.h"

/* Per-iteration memo for the (cid, ivname) full-table-scan narrow helpers
   below. Each is O(nodes); they are queried once per `@ivar[i]` expression,
   so on a large input (the self-compile) the naive form is O(nodes^2). The
   result is stable within one fixpoint iteration, so a generation-stamped
   direct-mapped cache collapses the repeats to O(nodes) per (cid, ivar).
   The generation is bumped once per fixpoint iteration (sp_narrow_memo_bump),
   so a stale entry can only survive within an iteration the fixpoint will
   re-run anyway. */
/* Receiver node whose type the boxed-hash face re-inference pretends is the
   general boxed-key/value hash (see infer_call's last resort). -1 when idle;
   set for the duration of one nested infer_call only. */
/* The type a READ of ivar slot `iv` yields. A shared-mutable string slot
   stores an sp_String handle but reads as a plain String (the read copies),
   so every reader -- attr_reader, a `def m = @x` shim, instance_variable_get
   -- reports the value form, exactly as a direct ivar read does. */
TyKind ivar_value_ty(ClassInfo *ci, int iv) {
  if (iv < 0 || iv >= ci->nivars) return TY_UNKNOWN;
  return ci->ivar_types[iv] == TY_STRBUF ? TY_STRING : ci->ivar_types[iv];
}

/* Codegen unboxes a poly receiver to one concrete kind before re-entering the
   typed emitter (the face table in types.h), and needs the inference to
   answer the same way for the duration -- poking the node's cached type does
   not survive, because anything under the re-emission that asks re-establishes
   it (under a safe-navigation guard the re-emission asks again, and the typed
   emitters then decline the very call they were re-entered to serve). One
   node at a time; -1 clears the pin. */
static int g_face_node = -1;
static TyKind g_face_kind = TY_UNKNOWN;
void an_set_face_node(int node, TyKind kind) { g_face_node = node; g_face_kind = kind; }
int an_face_node(void) { return g_face_node; }

/* What a receiver answers universally, as a RAW C scalar, with the type it
   answers. ONE table: the inference below reads it, and so does the
   implicit-self redirect in analyze.c, through an_poly_raw_argc. A second
   hand-kept copy of "what an object answers" goes stale -- the safe-navigation
   emitter carried one and it did (#4070 follow-up: `begin`, `end`, `count` and
   `bytes` missing, `infinite?` with the wrong type), and the redirect's own
   short list did too (#4387). Add a name here and every consumer has it. */
const struct an_poly_raw_row AN_POLY_RAW[] = {
      { "frozen?", 0, TY_BOOL }, { "nil?", 0, TY_BOOL }, { "zero?", 0, TY_BOOL },
      { "positive?", 0, TY_BOOL }, { "negative?", 0, TY_BOOL },
      { "even?", 0, TY_BOOL }, { "odd?", 0, TY_BOOL }, { "nan?", 0, TY_BOOL },
      { "finite?", 0, TY_BOOL }, { "integer?", 0, TY_BOOL }, { "empty?", 0, TY_BOOL },
      { "eql?", 1, TY_BOOL }, { "equal?", 1, TY_BOOL }, { "instance_of?", 1, TY_BOOL },
      { "bytesize", 0, TY_INT }, { "ord", 0, TY_INT }, { "bit_length", 0, TY_INT },
      { "numerator", 0, TY_INT }, { "denominator", 0, TY_INT },
      { "to_i", 0, TY_INT }, { "hash", 0, TY_INT }, { "object_id", 0, TY_INT },
      { "begin", 0, TY_INT }, { "end", 0, TY_INT }, { "count", 0, TY_INT },
      { "to_f", 0, TY_FLOAT }, { "to_r", 0, TY_RATIONAL }, { "to_c", 0, TY_COMPLEX },
      { "class", 0, TY_CLASS }, { "bytes", 0, TY_INT_ARRAY },
      { NULL, 0, TY_UNKNOWN } };

/* The argument count the universal table answers `name` with, or -1 when the
   table does not carry it. */
int an_poly_raw_argc(const char *name) {
  for (int q = 0; AN_POLY_RAW[q].n; q++)
    if (sp_streq(name, AN_POLY_RAW[q].n)) return AN_POLY_RAW[q].ac;
  return -1;
}

TyKind an_face_kind(void) { return g_face_kind; }
#define SP_NMEMO_SZ 16384
static unsigned g_narrow_gen = 1;
static struct { unsigned gen; long key; signed char val; } g_nmemo[SP_NMEMO_SZ];
/* A user class owns this name as a method, a class method, or an attr / Struct
   / Data reader. The poly String shortcuts must decline to the general dispatch
   when it does, or a reader named after a String method answers that method's
   result on the receiver's #inspect (#3364). Mirrors user_defines_or_reads on
   the codegen side. */
/* `exception: false` on a *_nonblock call: the would-block answer is the
   :wait_readable / :wait_writable marker rather than a raise, so the result is
   boxed (a symbol or the value). */
static int an_nonblock_no_exception(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, id, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (!av || an == 0) return 0;
  const char *lty = nt_type(nt, av[an - 1]);
  if (!lty || !sp_streq(lty, "KeywordHashNode")) return 0;
  int e = kwh_lookup(nt, av[an - 1], "exception");
  return e >= 0 && nt_type(nt, e) && sp_streq(nt_type(nt, e), "FalseNode");
}

/* Set while re-deriving a poly-receiver call's type as if no user class owned
   the name: the answer the builtin surface alone would give. A union whose
   receiver provably carries a container needs both that answer and the user
   one, and codegen needs to know the builtin shape to emit its arm (#3459). */
static int an_builtin_only = 0;
int an_builtin_only_p(void) { return an_builtin_only; }

/* Name-keyed answer memo, the same shape (and the same staleness argument) as
   udm_ in an_user_defines_method: the question below crosses every class with
   the ancestor chain, and infer_call asks it for every poly-receiver call node
   on every fixpoint iteration -- on a 40k-line program that is hundreds of
   millions of chain walks for a few thousand distinct answers. The answer only
   moves when scope or class shape does, which the (gen, nscopes, nclasses)
   stamp tracks. */
static char **udr_names = NULL;
static signed char *udr_ans = NULL;
static int *udr_next = NULL, *udr_head = NULL;
static int udr_n = 0, udr_cap = 0;
static unsigned udr_gen = (unsigned)-1;
static int udr_nscopes = -1, udr_nclasses = -1;
static unsigned udr_hash(const char *s) {
  unsigned h = 2166136261u;
  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}

int an_user_defines_or_reads(Compiler *c, const char *name) {
  if (an_builtin_only) return 0;
  if (!name) return 0;
  /* Outside the frozen fixpoint scope shape can still change without the counts
     moving (a rename), so neither consult nor populate the memo there. */
  int memo = comp_scope_index_is_frozen();
  unsigned b = 0;
  if (memo) {
    unsigned gen = comp_scope_index_gen();
    if (gen != udr_gen || c->nscopes != udr_nscopes || c->nclasses != udr_nclasses) {
      for (int i = 0; i < udr_n; i++) free(udr_names[i]);
      udr_n = 0;
      if (!udr_cap) {
        udr_cap = 4096;
        udr_names = malloc(sizeof(char *) * (size_t)udr_cap);
        udr_ans = malloc((size_t)udr_cap);
        udr_next = malloc(sizeof(int) * (size_t)udr_cap);
        udr_head = malloc(sizeof(int) * (size_t)udr_cap);
        if (!udr_names || !udr_ans || !udr_next || !udr_head) udr_cap = 0;
      }
      for (int i = 0; i < udr_cap; i++) udr_head[i] = -1;
      udr_gen = gen; udr_nscopes = c->nscopes; udr_nclasses = c->nclasses;
    }
    if (udr_cap) {
      b = udr_hash(name) & (unsigned)(udr_cap - 1);
      for (int i = udr_head[b]; i >= 0; i = udr_next[i])
        if (sp_streq(udr_names[i], name)) return udr_ans[i];
    }
    else memo = 0;
  }
  int ans = 0;
  for (int k = 0; k < c->nclasses && !ans; k++) {
    if (comp_method_in_chain(c, k, name, NULL) >= 0) ans = 1;
    else if (comp_is_reader(&c->classes[k], name)) ans = 1;
  }
  /* A CLASS method of the same name is deliberately not consulted: it is only
     reachable through a Class-valued receiver, so it cannot be the answer to
     an instance call. Counting it made `k.downcase` on a String bind to a
     `def self.downcase(s)` elsewhere in the program and compile to the arity
     raise (#3520). A TOP-LEVEL method is out for the same reason one step on:
     it lands on Object as a PRIVATE method, so an explicit receiver can never
     reach it -- CRuby answers `nil.size` after `def size(v)` with "private
     method 'size' called", not with the method. Counting it made every caller
     of this predicate stand its builtin arm down, and `def upcase(v) = v.upcase`
     compiled its own body to an unconditional NoMethodError. */
  if (memo && udr_n < udr_cap) {
    udr_names[udr_n] = strdup(name);
    if (udr_names[udr_n]) {
      udr_ans[udr_n] = (signed char)ans;
      udr_next[udr_n] = udr_head[b]; udr_head[b] = udr_n; udr_n++;
    }
  }
  return ans;
}

void sp_narrow_memo_bump(void) { g_narrow_gen++; }
static long narrow_key(int which, int cid, const char *ivname) {
  unsigned long h = 1469598103934665603UL ^ (unsigned)which;
  h = (h * 1099511628211UL) ^ (unsigned)cid;
  for (const char *p = ivname; p && *p; p++) h = (h * 1099511628211UL) ^ (unsigned char)*p;
  return (long)(h & 0x7fffffffffffffffUL);
}
static int narrow_memo_get(long key, int *hit) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  if (g_nmemo[slot].gen == g_narrow_gen && g_nmemo[slot].key == key) { *hit = 1; return g_nmemo[slot].val; }
  *hit = 0; return 0;
}
static void narrow_memo_put(long key, int val) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  g_nmemo[slot].gen = g_narrow_gen; g_nmemo[slot].key = key; g_nmemo[slot].val = (signed char)val;
}

/* Unify the value type of every splice-bound break/next in `node` (break/next
   inside a nested loop or block bind there, not to the splice). TY_UNKNOWN if
   none carry a value. Mirrors codegen's ie_splice_value_ty so the inferred
   instance_exec result type matches the slot codegen sizes. */
TyKind ie_block_break_next_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "BreakNode") || sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    if (an > 0) {
      /* `next *x` delivers the splat-built ARRAY, not the element type that
         infer_type reports for a SplatNode in an array literal. */
      const char *aty = nt_type(nt, av[0]);
      if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
      return infer_type(c, av[0]);
    }
    /* a bare `next` yields nil: `[1,2].map { |v| next if v == 1; v }` is [nil, 2] */
    return sp_streq(ty, "NextNode") ? TY_NIL : TY_UNKNOWN;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind s = ie_block_break_next_ty(c, nt_ref_at(nt, node, i));
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = ie_block_break_next_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
}

int g_infer_ignore_brk = 0;

/* Post-backstop return re-derivation must not newly widen a return to poly
   (see infer_return_types); set only around analyze_program's post-pass. */
int g_ret_no_new_poly = 0;

/* Top-level `break` detector: a BreakNode binding to the enclosing block,
   stopping at nested loops and nested block-bearing calls (which capture their
   own break). Mirrors codegen_stmt's subtree_has_loop_break. */
int block_has_top_break(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "BreakNode")) return 1;
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, node, "block") >= 0) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (block_has_top_break(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) if (block_has_top_break(c, el[j])) return 1;
  }
  return 0;
}

/* A call is a break-wrapped iterator when it takes a literal block whose body
   has a top-level break and is either a receiver-bearing builtin iterator or
   a call resolving to an inline-able yielding user method (whose body -- and
   so the block, at its yield sites -- is spliced at this call site, putting
   the wrapper's setjmp in exactly the right C scope). Receiverless
   NON-methods (loop / catch / proc / lambda literals) run their own scopes
   and stay excluded, as does instance_exec/eval (handled inline). */
int call_breaks(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;   /* not &proc / &:sym */
  const char *name = nt_str(nt, id, "name");
  if (name && (sp_streq(name, "instance_exec") || sp_streq(name, "instance_eval"))) return 0;
  if (nt_ref(nt, id, "receiver") < 0 && call_user_yield_mi(c, id) < 0) return 0;
  return block_has_top_break(c, nt_ref(nt, block, "body"));
}

/* str.unpack1(fmt) with a literal single-directive numeric format: the
   directive fixes the extracted value's type, so the result does not need
   to stay poly (`data[4, 4].unpack1('V')` reading a WAD header count in
   doom). Count/endian suffixes ("V2", "l<", "q>*") keep the first value's
   type. Only the numeric directives sp_str_unpack decodes qualify --
   integers to TY_INT, the float/double directives to TY_FLOAT;
   multi-directive, interpolated, and other formats stay TY_POLY. */
static TyKind an_unpack1_lit_type(const NodeTable *nt, int arg) {
  const char *aty = nt_type(nt, arg);
  if (!aty || !sp_streq(aty, "StringNode")) return TY_POLY;
  const char *f = nt_str(nt, arg, "content");
  if (!f || !f[0]) return TY_POLY;
  char d = f[0];
  const char *p = f + 1;
  while (*p == '<' || *p == '>' || *p == '!' || *p == '_') p++;
  while (*p >= '0' && *p <= '9') p++;
  if (*p == '*') p++;
  if (*p) return TY_POLY;  /* further directives: not this one's type */
  if (strchr("cCsSlLqQnNvV", d)) return TY_INT;
  if (strchr("dDfFeEgG", d)) return TY_FLOAT;
  return TY_POLY;
}

/* Whether every element written into the poly-array ivar `@<ivname>` of class
   `cid` is an int-returning kind: a bound method (a dispatch-table entry, called
   with an int arg returns int), an int array, an int, or nil filler. Mirrors
   legacy's cls_ivar_observed_types check in poly_index_narrow_int. */
/* Classify a value type stored as an element of a dispatch/data table:
   1 = int-returning when indexed/called (int bit, int array get, bound
   method/proc call, or a poly assumed to be such a callable); 2 = neutral
   filler (nil/unknown); 0 = a clearly non-int kind. */
static int table_elem_int_returning(TyKind vt) {
  if (vt == TY_INT || vt == TY_INT_ARRAY || vt == TY_METHOD || vt == TY_PROC ||
      vt == TY_POLY)
    return 1;
  if (vt == TY_NIL || vt == TY_UNKNOWN) return 2;
  return 0;
}

static int ivar_array_elems_int_returning_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* element write `@ivar[i] = v` */
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0) continue;
      const char *rty = nt_type(nt, recv);
      if (!rty || !sp_streq(rty, "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      int k = table_elem_int_returning(comp_ntype(c, av[1]));
      if (k == 0) return 0;
      if (k == 1) saw = 1;
      continue;
    }
    /* whole-ivar write `@ivar = [..]` / `@ivar = [x] * n` */
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;   /* the ArrayNode whose elements to inspect */
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");   /* `[..] * n` */
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      if (arr < 0) return 0;   /* non-literal whole write: can't verify */
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        int k = table_elem_int_returning(comp_ntype(c, els[e]));
        if (k == 0) return 0;
        if (k == 1) saw = 1;
      }
      continue;
    }
  }
  return saw;
}

/* `@table[i][j]` where @table is a poly array of int-returning callables/arrays
   (a method dispatch table) yields an int. Returns 1 if `id` matches that shape
   for the enclosing class. The index types don't matter (they may themselves be
   poly mid-fixpoint); the result type follows from the table's element kinds. */
static int ivar_array_elems_int_returning(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(0, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_int_returning_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

static int poly_double_index_int(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;   /* slice, not an element */
  int inner = nt_ref(nt, id, "receiver");
  if (inner < 0) return 0;
  const char *ity = nt_type(nt, inner);
  if (!ity || !sp_streq(ity, "CallNode")) return 0;
  const char *inm = nt_str(nt, inner, "name");
  if (!inm || !sp_streq(inm, "[]")) return 0;
  int iargs = nt_ref(nt, inner, "arguments");
  int ian = 0;
  const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &ian) : NULL;
  if (ian != 1) return 0;
  (void)iav;
  int ivnode = nt_ref(nt, inner, "receiver");
  if (ivnode < 0) return 0;
  const char *vty = nt_type(nt, ivnode);
  if (!vty || !sp_streq(vty, "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, ivnode, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_int_returning(c, cid, ivname);
}

/* Whether every element stored into poly-array ivar `@<ivname>` is an int
   array (a nested array of int arrays, e.g. @chr_banks / @nmt_mem). Element
   reads then yield an int array rather than a boxed poly. */
static int ivar_array_elems_all_int_array_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
        /* `@x = src.map { ... }`: elements are the block's result type. */
        int blk = nt_ref(nt, v, "block");
        int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn <= 0) return 0;
        TyKind et = comp_ntype(c, bb[bn - 1]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      if (arr < 0) return 0;
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        TyKind et = comp_ntype(c, els[e]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      continue;
    }
  }
  return saw;
}

/* `@nested[i]` (single index) where @nested is a poly array whose every
   element is an int array yields an int array (not a boxed poly). Returns the
   ivar name via *out_iv for codegen, or NULL. */
static int ivar_array_elems_all_int_array(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(1, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_all_int_array_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

/* Whether every element of poly-array constant `CNAME` is an int array
   (e.g. `WAVE_FORM = [..].map { (0..7).map { .. } }`). Element reads then
   yield sp_IntArray* instead of a boxed poly. All writes to the constant and
   any `CNAME[i] = v` mutation must agree. */
static int const_array_elems_all_int_array_impl(Compiler *c, const char *cname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, cname)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (!sp_streq(ty, "ConstantWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, cname)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return 0;
    /* `CNAME = [...].freeze` binds the same literal */
    if (nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze") &&
        nt_ref(nt, v, "receiver") >= 0)
      v = nt_ref(nt, v, "receiver");
    const char *vty = nt_type(nt, v);
    int arr = -1;
    if (vty && sp_streq(vty, "ArrayNode")) arr = v;
    else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
             (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
      int blk = nt_ref(nt, v, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn <= 0) return 0;
      TyKind et = comp_ntype(c, bb[bn - 1]);
      if (et == TY_INT_ARRAY) { saw = 1; continue; }
      return 0;
    }
    if (arr < 0) return 0;
    int en = 0;
    const int *els = nt_arr(nt, arr, "elements", &en);
    for (int e = 0; e < en; e++) {
      TyKind et = comp_ntype(c, els[e]);
      if (et == TY_INT_ARRAY) { saw = 1; continue; }
      if (et == TY_NIL || et == TY_UNKNOWN) continue;
      return 0;
    }
  }
  return saw;
}

int const_array_elems_all_int_array(Compiler *c, const char *cname) {
  long k = narrow_key(2, 0, cname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = const_array_elems_all_int_array_impl(c, cname);
  narrow_memo_put(k, v);
  return v;
}

/* `CONST[i]` on a poly-array constant of int arrays yields an int array. */
static int const_poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) return 0;
  const char *cname = nt_str(nt, recv, "name");
  LocalVar *cv = cname ? comp_const(c, cname) : NULL;
  if (!cv || cv->type != TY_POLY_ARRAY) return 0;
  return const_array_elems_all_int_array(c, cname);
}

static int poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, recv, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_all_int_array(c, cid, ivname);
}

/* A plain int literal value (not an out-of-int64 bigint literal). */
static int infer_const_int_node(const NodeTable *nt, int id, long long *out) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "IntegerNode")) return 0;
  if (nt_str(nt, id, "bigval")) return 0;
  *out = (long long)nt_int(nt, id, "value", 0);
  return 1;
}

/* Whether base**exp does not fit in signed 64 bits (so it must be a Bignum). */
static int infer_int_pow_overflows(long long base, long long exp) {
  if (exp <= 0) return 0;
  if (base >= -1 && base <= 1) return 0;
  long long r = 1;
  for (long long i = 0; i < exp; i++)
    if (__builtin_mul_overflow(r, base, &r)) return 1;
  return 0;
}

/* Whether base << amount does not fit in signed 64 bits (a Bignum in CRuby --
   `1 << 64` is 2**64, not a wrapped/UB C shift). */
static int infer_int_shl_overflows(long long base, long long amount) {
  if (base == 0 || amount <= 0) return 0;
  long long r = base;
  for (long long i = 0; i < amount; i++)
    if (__builtin_mul_overflow(r, 2, &r)) return 1;
  return 0;
}

/* A blockless `range.each` is an external Enumerator only when used standalone
   or consumed by an enumerator method (#next/#peek/#rewind/#size). When it is
   the receiver of a collection method (.to_a/.map/.select/...), it materializes
   to a typed array instead, so those chains keep the fast unboxed path. */
/* Literal Float::INFINITY as a range endpoint (the infer-side twin of
   codegen's lazy_endpoint_is_infinite; nil/missing ends are checked by the
   callers directly). */
int infer_end_is_float_inf(Compiler *c, int right) {
  const NodeTable *nt = c->nt;
  if (right < 0) return 0;
  const char *rty = nt_type(nt, right);
  if (!rty || !sp_streq(rty, "ConstantPathNode")) return 0;
  const char *cpnm = nt_str(nt, right, "name");
  if (!cpnm || !sp_streq(cpnm, "INFINITY")) return 0;
  int par = nt_ref(nt, right, "parent");
  const char *parnm = (par >= 0 && nt_type(nt, par) &&
                       sp_streq(nt_type(nt, par), "ConstantReadNode"))
                      ? nt_str(nt, par, "name") : NULL;
  return parnm && sp_streq(parnm, "Float");
}

/* A range endpoint that is an infinite Float: the `Float::INFINITY` constant
   or its negation. Such a bound has no sp_int value at all (#3670). */
static int infer_endpoint_is_infinite(Compiler *c, int ep) {
  const NodeTable *nt = c->nt;
  if (ep < 0) return 0;
  if (infer_end_is_float_inf(c, ep)) return 1;
  if (nt_kind(nt, ep) == NK_CallNode && nt_str(nt, ep, "name") &&
      sp_streq(nt_str(nt, ep, "name"), "-@"))
    return infer_end_is_float_inf(c, nt_ref(nt, ep, "receiver"));
  return 0;
}

/* The right endpoint of a literal Range receiver when it was written as a
   finite Float over an Integer begin (1..5.5): that Range keeps the integer
   representation (CRuby iterates it), but its END readers answer the Float the
   caller wrote, which the sp_int fields cannot hold (#3896). */
int range_lit_float_end(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rnode = recv;
  for (int g = 0; g < 8 && rnode >= 0 && nt_kind(nt, rnode) == NK_ParenthesesNode; g++) {
    int pb = nt_ref(nt, rnode, "body");
    int pn = 0; const int *ps = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    rnode = (pn == 1 && ps) ? ps[0] : -1;
  }
  if (rnode < 0 || nt_kind(nt, rnode) != NK_RangeNode) return -1;
  int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
  if (lo < 0 || hi < 0) return -1;
  if (infer_type(c, lo) != TY_INT) return -1;
  if (infer_type(c, hi) != TY_FLOAT || infer_endpoint_is_infinite(c, hi)) return -1;
  return hi;
}


static int range_each_is_external(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (nt_ref(nt, n, "receiver") != id) continue;
    const char *m = nt_str(nt, n, "name");
    if (m && (sp_streq(m, "next") || sp_streq(m, "peek") ||
              sp_streq(m, "rewind") || sp_streq(m, "size") ||
              sp_streq(m, "class") || sp_streq(m, "inspect") ||
              /* Enumerator-only chains (Array has no such method): the each
                 must stay an Enumerator, not materialize to an int array (#3228) */
              sp_streq(m, "with_index") || sp_streq(m, "with_object") ||
              sp_streq(m, "each_with_index") || sp_streq(m, "each_index") ||
              /* the forms that answer the RECEIVER: materializing the each
                 would answer the elements instead (#3857). each_entry is
                 renamed to each, so the recorded self-result marks it. */
              sp_streq(m, "each_entry") ||
              ((sp_streq(m, "each_slice") || sp_streq(m, "each_cons")) &&
               nt_ref(nt, n, "block") >= 0))) return 1;
    if (nt_int(nt, n, "enum_self_result", -1) == id) return 1;
    /* the drain a self-result rewrite interposed: the each stays an Enumerator
       (the call answers it) and the drain reads its elements (#3857) */
    if (m && sp_streq(m, "to_a") && nt_str(nt, n, "enum_recv")) return 1;
    return 0;   /* receiver of a collection method -> materialize to an array */
  }
  return 1;     /* standalone -> enumerator */
}

int range_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  int args = nt_ref(nt, id, "arguments"); int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  /* Only an integer range materializes faithfully: sp_Range holds sp_int
     bounds, so redispatch only when a literal range receiver has both bounds
     present and typed TY_INT. A float/string bound would truncate or fail to
     compile, and a beginless/endless range (`(1..)`, `(..5)`) has no int array
     to build -- all fall through to the loud `unsupported` reject rather than
     silently miscompiling. (A non-literal receiver, e.g. `r = (1.0..5.0);
     r.find`, is the pre-existing int-only-sp_Range limitation, not detectable
     here.) */
  int rn = nt_ref(nt, id, "receiver");
  while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
    int body = nt_ref(nt, rn, "body"); int bn = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    rn = bn == 1 ? bd[0] : -1;
  }
  if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
    int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
    if (lo < 0 || hi < 0) return 0;
    /* A BOXED bound is fine: the range literal's own emission coerces it to the
       sp_int sp_Range holds, exactly as it does for `(1...m).sum`. Demanding a
       statically int bound refused a destructured block parameter, whose leaf
       binds poly (#3363). A float / string bound still declines. */
    for (int e = 0; e < 2; e++) {
      TyKind bt = infer_type(c, e ? hi : lo);
      if (bt != TY_INT && bt != TY_POLY) return 0;
    }
  }
  /* Non-collecting Enumerable methods: their result does not depend on the
     block-produced element type, so materializing the range to an int array is
     transparent. flat_map/collect_concat also redispatch because the block-param
     typing pass types their range block parameter as an int; other array-building
     collectors (filter_map/partition/chunk_while) are not typed there yet, so
     they stay a clean reject rather than miscompile. */
  if (sp_streq(name, "group_by") || sp_streq(name, "find") ||
      sp_streq(name, "detect") || sp_streq(name, "zip") ||
      sp_streq(name, "tally")) return 1;
  /* each_slice/each_cons: the block form and the blockless Enumerator form
     (.to_a / .map chains) both materialize transparently -- the slices carry
     the range's own ints. */
  if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && argc >= 1)
    return 1;
  /* block-taking Enumerable forms the array emitters serve: partition,
     each_with_index, sort_by, chunk_while, sum { }, and the block forms of
     inject/reduce with an initial value. */
  if (block >= 0 &&
      (sp_streq(name, "partition") || sp_streq(name, "each_with_index") ||
       sp_streq(name, "sort_by") || sp_streq(name, "chunk_while") ||
       sp_streq(name, "slice_when") ||
       sp_streq(name, "chunk") ||
       sp_streq(name, "sum") || sp_streq(name, "each_with_object") ||
       sp_streq(name, "take_while") || sp_streq(name, "drop_while")))
    return 1;
  if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) && block >= 0)
    return 1;
  /* cycle { }: the array emitter serves both the counted and endless forms;
     the yielded elements are the range's own ints */
  if (sp_streq(name, "cycle") && block >= 0) return 1;
  if ((sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")) && block >= 0) return 1;
  /* reduce/inject: the explicit symbol / initial-value forms (no block). */
  if ((sp_streq(name, "reduce") || sp_streq(name, "inject")) && argc >= 1 && block < 0) return 1;
  /* count: the block / argument forms (bare count is size, handled natively). */
  if (sp_streq(name, "count")) return block >= 0 || argc >= 1;
  /* take/drop and reverse_each materialize transparently (the results carry
     the range's own ints); filter_map has array block-param typing. */
  if ((sp_streq(name, "take") || sp_streq(name, "drop")) && argc == 1) return 1;
  if (sp_streq(name, "reverse_each")) return 1;
  if (sp_streq(name, "filter_map") && block >= 0) return 1;
  /* min(n)/max(n)/minmax with a count return arrays of the range's ints */
  if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc >= 1) return 1;
  /* blockless all?/any?/none?/one?: a truthiness scan, which the materialized
     int array performs identically (an int is always truthy) (#3859). The
     pattern-argument forms scan the same elements with `===`. */
  if (block < 0 && argc <= 1 &&
      (sp_streq(name, "all?") || sp_streq(name, "any?") ||
       sp_streq(name, "none?") || sp_streq(name, "one?"))) return 1;
  /* blockless cycle(n) / each_entry: an Enumerator over the range's own ints,
     which the materialized array yields identically (#3840). A countless
     `cycle` never ends, so it keeps its own emitter. */
  if (block < 0 && sp_streq(name, "cycle") && argc == 1) return 1;
  if (block < 0 && sp_streq(name, "each_entry") && argc == 0) return 1;
  /* blockless select/reject/map: an Enumerator over the range's own ints,
     which the materialized array builds identically (#3062). */
  if (block < 0 && argc == 0 &&
      (sp_streq(name, "select") || sp_streq(name, "filter") ||
       sp_streq(name, "find_all") || sp_streq(name, "reject"))) return 1;
  return 0;
}

/* A Hash Enumerable method served by materializing the hash into its
   [key, value] pair array (sp_enum_items_from) and re-dispatching as a poly
   array: reduce/inject and each_with_index block forms. The hash-native
   folds (sum/count/find/map/...) keep their own emitters. */
int hash_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  /* blockless pair-order Enumerables: min/max compare the [k, v] pairs */
  if (block < 0 && (sp_streq(name, "min") || sp_streq(name, "max") ||
                    sp_streq(name, "minmax")))
    return 1;
  /* blockless each_with_index: an external Enumerator of [[k, v], i] */
  if (block < 0 && sp_streq(name, "each_with_index")) return 1;
  /* inject(:op) / reduce(:op): the Symbol-operator fold runs over the [k, v]
     pairs, the same materialization the block form below rides (#3830) */
  if (block < 0 && (sp_streq(name, "reduce") || sp_streq(name, "inject"))) return 1;
  /* pair-array Enumerables with no dedicated hash emitter: find_index, uniq,
     zip, tally, reverse_each ride the materialized redispatch, block or not
     (#2372) */
  if (sp_streq(name, "find_index") || sp_streq(name, "uniq") ||
      sp_streq(name, "zip") || sp_streq(name, "tally") ||
      sp_streq(name, "reverse_each")) return 1;
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  /* comparator-block min/max/minmax compare the [k, v] pairs like the
     blockless forms (min_by/max_by keep their dedicated hash emitters) */
  if (sp_streq(name, "min") || sp_streq(name, "max") || sp_streq(name, "minmax")) return 1;
  if (sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "find_all")) return 1;
  if (sp_streq(name, "each_with_index")) return 1;
  if (sp_streq(name, "reduce") || sp_streq(name, "inject")) return 1;
  /* comparator-block sort over the [k, v] pairs -> a poly array of pairs */
  if (sp_streq(name, "sort")) return 1;
  /* each_with_object / flat_map keep their dedicated hash emitters */
  /* min_by/max_by keep their dedicated hash emitters; only minmax_by rides
     the pair redispatch */
  if (sp_streq(name, "minmax_by")) return 1;
  return 0;
}

/* True if a reduce/inject block's tail value flows FROM the accumulator param
   `accp`: a bare read of it (`...; acc`) or a call whose receiver is it
   (`acc << x`, `acc.merge(...)`). Such a tail carries the accumulator's own
   (seed) type even when the param currently infers poly, so it must not trip
   the poly-widening of the fold's result type (#3240). */
int reduce_tail_from_acc(Compiler *c, int tail, const char *accp) {
  const NodeTable *nt = c->nt;
  if (tail < 0 || !accp) return 0;
  const char *ty = nt_type(nt, tail);
  if (!ty) return 0;
  if (sp_streq(ty, "LocalVariableReadNode"))
    return nt_str(nt, tail, "name") && sp_streq(nt_str(nt, tail, "name"), accp);
  if (sp_streq(ty, "CallNode")) {
    int rcv = nt_ref(nt, tail, "receiver");
    if (rcv >= 0 && nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "LocalVariableReadNode"))
      return nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), accp);
  }
  return 0;
}

/* Does the program build Method objects (`method(:x)` / `instance_method(:x)`)?
   Only then can a boxed value answering #name be a Method (#3692). */
int an_program_builds_methods(Compiler *c) {
  static const NodeTable *memo_nt = NULL; static int memo = 0;
  if (memo_nt == c->nt) return memo;
  memo_nt = c->nt; memo = 0;
  for (int id = 0; id < c->nt->count && !memo; id++) {
    if (nt_kind(c->nt, id) != NK_CallNode) continue;
    const char *n = nt_str(c->nt, id, "name");
    if (n && (sp_streq(n, "method") || sp_streq(n, "instance_method") ||
              sp_streq(n, "unbind"))) memo = 1;
  }
  return memo;
}

/* Does this subtree pull from an external Enumerator (`e.next`)? Such a call
   is what ends a break-less `loop` with StopIteration (#3588). */
static int an_subtree_calls_enum_next(Compiler *c, int root) {
  const NodeTable *nt = c->nt;
  if (root < 0) return 0;
  if (nt_kind(nt, root) == NK_CallNode) {
    const char *n = nt_str(nt, root, "name");
    if (n && sp_streq(n, "next") && nt_ref(nt, root, "receiver") >= 0) return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (an_subtree_calls_enum_next(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n2 = 0; const int *el = nt_arr_at(nt, root, i, &n2);
    for (int j = 0; j < n2; j++)
      if (an_subtree_calls_enum_next(c, el[j])) return 1;
  }
  return 0;
}

/* Zero-argument builtin methods the poly dispatch already serves with a real
   arm. `require "ostruct"` turns any other bare name on a poly receiver into
   a possible OpenStruct member read (#3197); these must NOT be swallowed by
   that catch-all, or the member lookup replaces the builtin and returns nil
   (#3341: `switches.uniq` became an OpenStruct member fetch). */
int poly_builtin_zero_arg_name(const char *m) {
  static const char *const B[] = {
    "to_s", "inspect", "length", "size", "count",
    "uniq", "sort", "reverse", "flatten", "compact", "to_a", "to_h", "to_i",
    "to_f", "to_sym", "keys", "values", "first", "last", "min", "max", "sum",
    "pop", "shift", "clear", "dup", "clone", "freeze", "chars", "bytes",
    "strip", "chomp", "chop", "upcase", "downcase", "capitalize", "swapcase",
    "succ", "next", "abs", "round", "floor", "ceil", "arity", "call", NULL };
  for (int i = 0; B[i]; i++) if (sp_streq(m, B[i])) return 1;
  return 0;
}
/* The return a user class gives `name` at this arity, unified over every class
   that defines it, or TY_UNKNOWN when none does (or none has settled). The
   poly-dispatch union rules ask this to tell a name whose user answer AGREES
   with the builtin one from a name whose answer does not. */
static TyKind an_user_read_ty(Compiler *c, const char *name, int argc) {
  TyKind r = TY_UNKNOWN; int found = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (c->classes[k].is_native_class) {
      int nmk = comp_native_method_find(c, k, name, argc, 0);
      if (nmk >= 0 && c->native_methods[nmk].nargs == argc) {
        TyKind nr = sp_streq(c->native_methods[nmk].ret, "self")
                      ? ty_object(k) : native_spec_to_ty(c->native_methods[nmk].ret);
        r = found ? ty_unify(r, nr) : nr; found = 1;
      }
      continue;
    }
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi >= 0 && c->scopes[mi].ret != TY_UNKNOWN) {
      r = found ? ty_unify(r, c->scopes[mi].ret) : (TyKind)c->scopes[mi].ret;
      found = 1;
    }
  }
  return found ? r : TY_UNKNOWN;
}

/* The analyze twin of codegen's bare_call_class_owned: a receiverless call the
   enclosing class's own chain answers. The class sits ABOVE Kernel, so the
   builtin arms below must stand down for it, or the two halves of the compiler
   name different methods for the same call. */
/* 1 when a user class defines `name` as an instance method whose return is not
   `want`. The poly dispatch emits that class's arm beside the builtin ones and
   accumulates them all in one C temp, so the call's type has to be one both can
   hold: `Tags#include?` answering an index came back through an sp_bool slot as
   true/false, and `0` -- truthy in Ruby -- arrived as false (#4072). */
int an_user_ret_disagrees(Compiler *c, const char *name, TyKind want) {
  if (!name) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi < 0 || mi >= c->nscopes) continue;
    TyKind r = (TyKind)c->scopes[mi].ret;
    if (r != want && r != TY_UNKNOWN) return 1;
  }
  return 0;
}

/* Whether a chunk_while/slice_when/chunk block call is consumed by a `.to_a`
   terminal. Without one it answers a first-class Enumerator; with one, the
   poly array of runs the terminal materializes. Both the typed-receiver arm
   and the poly-receiver arm have to make the same call, or the slot the value
   lands in disagrees with what the emitter renders. */
int an_chunk_family_to_a(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, w) {
    if (nt_ref(nt, w, "receiver") != id) continue;
    const char *wn = nt_str(nt, w, "name");
    if (wn && sp_streq(wn, "to_a")) return 1;
  }
  return 0;
}

/* Can this type's C slot hold a nil of its own? Integer and Float have their
   sentinels; String, the arrays and every reference object have NULL. A bool, a
   Symbol, a Class, a Rational and a Complex have no such value, so a nil in one
   of those slots has to be boxed. Stated once: the `&.` widening below asks the
   same question, and the two copies would drift. */
int an_ty_holds_nil(TyKind t) {
  return !(t == TY_BOOL || t == TY_CLASS || t == TY_SYMBOL ||
           t == TY_RATIONAL || t == TY_COMPLEX);
}

/* The answer the poly-receiver section reaches, filtered through the question
   above. Every concrete type that section returns becomes the C type of the ONE
   temp the dispatch accumulates into, and a user class's arm writes its own
   return into that same temp -- so a disagreeing return reads back through the
   wrong union member. `Chain#join` answering a Chain came back through the
   builtin's `const char *` as `.v.s` and printed "" (#4083); `Tags#include?`
   answering an index came back through an sp_bool (#4072). Three arms in that
   section had thought to ask; the rest had not, which is why the answer
   depended on which name you picked. Ask once, on the way out. */
static TyKind an_poly_concrete(Compiler *c, const char *name, TyKind t) {
  if (t == TY_POLY || t == TY_UNKNOWN || !name) return t;
  /* The builtin-only re-derivation asks what this call would be if NO user
     class owned the name, so the union with a user return is the one thing it
     must not take: the dispatch records that answer to shape its BUILTIN arm,
     and a poly there made the arm assign sp_poly_values()'s raw sp_PolyArray *
     into the boxed slot the user arms need. */
  if (an_builtin_only) return t;
  for (int k = 0; k < c->nclasses; k++) {
    int mi = comp_method_in_chain(c, k, name, NULL);
    if (mi < 0 || mi >= c->nscopes) continue;
    TyKind r = (TyKind)c->scopes[mi].ret;
    if (r == t || r == TY_UNKNOWN || r == TY_VOID) continue;
    /* A nil answer FITS a slot that has a nil of its own -- a NULL array is
       nil, and that is exactly why the array trio may stay concrete (#3461).
       Widening those would leave the builtin arm's sp_PolyArray * meeting an
       sp_RbVal, which is the shape #3461 fixed. */
    if (r == TY_NIL && an_ty_holds_nil(t)) continue;
    return TY_POLY;
  }
  return t;
}

static int an_bare_call_class_owned(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "receiver") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  Scope *sc = comp_scope_of(c, id);
  int cid = sc ? sc->class_id : -1;
  if (cid < 0 || cid >= c->nclasses) return 0;
  if (sc->is_cmethod) return comp_cmethod_in_chain(c, cid, name, NULL) >= 0;
  return comp_method_in_chain(c, cid, name, NULL) >= 0 ||
         comp_reader_in_chain(c, cid, name, NULL);
}

static TyKind infer_call_inner(Compiler *c, int id);

/* A builtin call whose count the arity guard refuses raises before it
   answers, so any type is sound for it; nil lets the shapes that need one
   -- a send candidate, which is dropped when it types unknown, an argument
   -- compile down to the raise instead of a miss. The guard runs after a
   few arms of emit_call, so none of those may claim a call it refuses, or
   the value typed here is not the one emitted. */
TyKind infer_call(Compiler *c, int id) {
  TyKind t = infer_call_inner(c, id);
  if (t == TY_UNKNOWN && builtin_arity_violation(c, id)) return TY_NIL;
  return t;
}

/* The call's trailing keyword hash says `exception: false` (the literal:
   the form that asks for nil instead of a raise). */
static int kconv_noraise_kw(Compiler *c, int argc, const int *argv) {
  if (argc < 1) return 0;
  int kw = argv[argc - 1];
  const char *kt = nt_type(c->nt, kw);
  if (!kt || !sp_streq(kt, "KeywordHashNode")) return 0;
  int en = 0; const int *el = nt_arr(c->nt, kw, "elements", &en);
  for (int e = 0; e < en; e++) {
    int k = nt_ref(c->nt, el[e], "key"), v = nt_ref(c->nt, el[e], "value");
    const char *kn = (k >= 0 && nt_type(c->nt, k) && sp_streq(nt_type(c->nt, k), "SymbolNode"))
                     ? nt_str(c->nt, k, "value") : NULL;
    const char *vt = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (kn && sp_streq(kn, "exception") && vt && sp_streq(vt, "FalseNode")) return 1;
  }
  return 0;
}
/* Kernel#Integer's static kind. Integer, unless the argument is a user
   object whose #to_int or #to_i is typed as answering a Bignum: then the
   answer is carried, not truncated, in a Bignum slot for the strict form
   (which never answers nil; a Bignum slot holds the small values too) and
   in a boxed slot for the `exception: false` form, whose nil a Bignum slot
   cannot hold. A method whose answer the analysis cannot pin keeps the
   Integer slot in the strict form, where a Bignum answer is a loud
   RangeError at run time: typing it as a Bignum would refuse to build every
   site that needs an Integer, a Range bound for one. The `exception: false`
   form of such a call is boxed already, so it carries the answer. */
static TyKind kconv_integer_kind(Compiler *c, int arg, int noraise) {
  static const char *const names[] = { "to_int", "to_i" };
  TyKind at = infer_type(c, arg);
  if (!ty_is_object(at)) return TY_INT;
  for (int k = 0; k < 2; k++) {
    int mi = comp_method_in_chain(c, ty_object_class(at), names[k], NULL);
    /* the same admission as conv_bridge_callee's: a method with parameters
       or one that yields has no bridge row, so it must not type the slot */
    if (mi < 0 || c->scopes[mi].nparams != 0 || c->scopes[mi].yields) continue;
    if (c->scopes[mi].ret == TY_BIGINT) return noraise ? TY_POLY : TY_BIGINT;
    if (c->scopes[mi].ret == TY_POLY && noraise) return TY_POLY;
  }
  return TY_INT;
}

static TyKind infer_call_inner(Compiler *c, int id) {

  /* a yielder push (`y << v` inside an Enumerator.new generator) lowers to a
     Fiber.yield, whose value is boxed -- never the array append it looks like */
  if (nt_int(c->nt, id, "yielder_push", 0)) return TY_POLY;
  /* the rewrite recorded that this call answers nil whatever it drives (#3589) */
  if (nt_int(c->nt, id, "nil_result", 0)) return TY_NIL;
  /* the redirect recorded that this call yields its original receiver (#2981) */
  {
    int sr = nt_int(c->nt, id, "enum_self_result", -1);
    if (sr >= 0) return infer_type(c, sr);
  }
  const NodeTable *nt = c->nt;
  /* a dynamic send lowered to a name-dispatch (desugar_dynamic_send) yields one
     of several boxed method results -> poly. */
  { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) return TY_POLY; }
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) return TY_UNKNOWN;

  /* `Module.accessor.cmethod(...)` where the singleton accessor statically
     folds to a constant: dispatch as that constant's class method. This has to
     come BEFORE anything that decides by the receiver's type, because the
     receiver -- a `class << self; attr_reader` read -- types poly (its slot
     holds a class value), and every rule that answers a poly receiver answers
     poly. It sat below them, and was never reached: the emitter, which asks
     the same fold, devirtualized the call to the class method's C function
     while the site expected an sp_RbVal, and clang refused the seam (#4426).
     The hand-written `def self.adapter; @adapter; end` never had the problem
     because that reader is a class method with a typed return, not a slot. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int mi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
      if (mi >= 0) return method_call_ret(c, mi, id);
    }
    /* Stage-2: accessor holds one of several constants; unify their cmethod returns. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      TyKind r = TY_UNKNOWN;
      for (int k = 0; k < ncand; k++) {
        int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
        if (mi >= 0) r = ty_unify(r, method_call_ret(c, mi, id));
      }
      if (r != TY_UNKNOWN) return r;
    }
  }

  /* The universal predicates and conversions the poly runtime answers as a RAW
     C scalar. The emitters have always known this; the TYPE did not say it, so
     `x&.frozen?` was inferred poly while its value arm rendered an sp_bool, and
     the safe-navigation emitter carried a hand-kept table of these names to
     put the two back together. A table of what a call ANSWERS belongs in the
     inference, where every consumer reads it -- and it went stale as tables do
     (#4070 follow-up: `begin`, `end`, `count` and `bytes` were missing and
     `infinite?` was listed with the wrong type). Guarded the way the emitter
     guarded its table: a user class owning the name answers for itself. */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 && !an_user_defines_or_reads(c, name) &&
      infer_type(c, recv) == TY_POLY) {
    for (int q = 0; AN_POLY_RAW[q].n; q++)
      if (AN_POLY_RAW[q].ac == argc && sp_streq(name, AN_POLY_RAW[q].n)) return AN_POLY_RAW[q].t;
  }

  /* A retargeted `x.send(:m)` reaching a top-level def: see the codegen twin. */
  if (nt_str(nt, id, "send_blind") && recv >= 0 && nt_ref(nt, id, "block") < 0) {
    int smi = comp_method_index(c, name);
    if (smi >= 0 && !(smi < c->nscopes && c->scopes[smi].yields)) {
      TyKind srt = infer_type(c, recv);
      int owns = ty_is_object(srt) &&
                 (comp_method_in_chain(c, ty_object_class(srt), name, NULL) >= 0 ||
                  comp_reader_in_chain(c, ty_object_class(srt), name, NULL));
      if (!owns) return method_call_ret(c, smi, id);
    }
  }

  /* A call with NO receiver resolves the way CRuby's ancestry does: the
     enclosing scope's own chain first, then Object -- where a top-level `def`
     lands -- and only then a Kernel builtin. The user-method arm further down
     said the second part already, but the Kernel arms are reached by POSITION
     and several sit above it, so `caller`, `p`, `puts` and `loop` were typed
     from the builtin while codegen called the user's method: the local was
     declared sp_StrArray * and assigned a const char *. One rule, asked before
     any builtin arm, keeps the two halves of the compiler on the same method. */
  if (recv < 0 && nt_ref(nt, id, "block") < 0 &&
      !nt_str(nt, id, "vis_enforce")) {
    int bmi = comp_method_index(c, name);
    if (bmi < 0) bmi = comp_included_method_index(c, name);
    if (bmi >= 0) {
      Scope *bsc = comp_scope_of(c, id);
      int bcls = bsc ? bsc->class_id : -1;
      int shadowed = bcls >= 0 && bcls < c->nclasses &&
                     (bsc->is_cmethod
                        ? comp_cmethod_in_chain(c, bcls, name, NULL) >= 0
                        : (comp_method_in_chain(c, bcls, name, NULL) >= 0 ||
                           comp_is_reader(&c->classes[bcls], name)));
      /* A callee that yields carries a block parameter in its C signature,
         which the arm below is what establishes: answering here left the call
         site and the definition disagreeing about the signature. */
      if (!shadowed && !(bmi < c->nscopes && c->scopes[bmi].yields))
        return method_call_ret(c, bmi, id);
    }
  }

  /* the accessors every exception carries, on an instance of a user subclass
     (#3732): #exception is self, #cause another exception, #backtrace the
     frames, and the two message renderings strings */
  if (recv >= 0 && argc == 0 && ty_is_object(infer_type(c, recv)) &&
      class_is_exc_subclass(c, ty_object_class(infer_type(c, recv))) &&
      comp_method_in_chain(c, ty_object_class(infer_type(c, recv)), name, NULL) < 0) {
    if (sp_streq(name, "exception")) return infer_type(c, recv);
    if (sp_streq(name, "cause")) return TY_EXCEPTION;
    if (sp_streq(name, "backtrace")) return TY_STR_ARRAY;
    if (sp_streq(name, "full_message") || sp_streq(name, "detailed_message")) return TY_STRING;
  }

  /* #dup drops the receiver's singleton methods, so the copy is an instance of
     the class Ruby sees rather than the synthesized singleton one (#3739) */
  if (recv >= 0 && sp_streq(name, "dup") && argc == 0) {
    TyKind drt0 = infer_type(c, recv);
    if (ty_is_object(drt0)) {
      int dci0 = ty_object_class(drt0);
      int vis = singleton_visible_ci(c, dci0);
      if (vis != dci0) return ty_object(vis);
    }
  }

  /* A copy of a value whose type is only known at run time is itself only known
     at run time: answering UNKNOWN left the slot open to the usage rules, and an
     int-keyed `copy[i] = v` then typed the copy of an Array as a HASH -- the
     method answered `{}` where CRuby answered an Array (#3952). */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "dup") || sp_streq(name, "clone")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY;

  /* `!`, `!=` and `==` are ordinary methods a class may override, so the
     result is whatever its definition answers, not a bool (#3740). `==` joins
     them: a class whose `==` answers something else had every call site typed
     bool, and the value was rendered as true/false. */
  if (recv >= 0 && (sp_streq(name, "!") || sp_streq(name, "!=") || sp_streq(name, "==")) &&
      argc == (sp_streq(name, "!") ? 0 : 1)) {
    TyKind nrt = infer_type(c, recv);
    if (ty_is_object(nrt)) {
      int nmi = comp_method_in_chain(c, ty_object_class(nrt), name, NULL);
      if (nmi >= 0) return (TyKind)c->scopes[nmi].ret;
    }
  }

  /* $~'s MatchData face over the match registers (codegen reads the same
     backing the back-references use): nullable strings. */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "GlobalVariableReadNode") ||
       sp_streq(nt_type(nt, recv), "BackReferenceReadNode")) &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "$~") &&
      (sp_streq(name, "pre_match") || sp_streq(name, "post_match") || sp_streq(name, "to_s")))
    return TY_STRING;

  /* A block with a top-level `break <v>` makes this call return <v>, so its
     result is the union of the normal result and the break value -- poly. The
     break wrapper suppresses this (g_infer_ignore_brk) to recover the normal
     result type for the inner emission. */
  if (!g_infer_ignore_brk && call_breaks(c, id)) return TY_POLY;

  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  /* A block call on a poly receiver whose candidates include a YIELDING method
     is served by that method's proc-form clone, which answers poly uniformly
     (its yield is a call on a real proc). This has to precede every
     `poly.<builtin name> { }` rule below: those type the call from the builtin
     alone, so a candidate answering an Integer array landed in a generic-array
     slot with no conversion and the value was read as the wrong container
     (#3399, #3409). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 &&
      poly_enum_op_for(name)) {
    for (int k = 0; k < c->nclasses; k++) {
      if (!c->classes[k].instantiated) continue;
      if (comp_method_in_chain(c, k, name, NULL) >= 0) return TY_POLY;
    }
  }
  /* max_by / min_by on a boxed receiver -- an Array read out of a container.
     Codegen materializes the elements and re-dispatches as the array form, so
     the result is the winning element, itself boxed. Without a type here the
     call stayed untyped and every method on the result was rejected, the way
     sort_by (which does have one) never was. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "max_by") || sp_streq(name, "min_by")))
    return TY_POLY;
  /* Same, for a name outside that surface: only a yielding candidate makes
     the call a dispatch there, since a non-yielding one leaves a
     block-carrying call on the builtin path entirely. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0) {
    for (int k = 0; k < c->nclasses; k++) {
      if (!c->classes[k].instantiated) continue;
      int ymi = comp_method_in_chain(c, k, name, NULL);
      if (ymi >= 0 && c->scopes[ymi].yields) return TY_POLY;
    }
  }
  /* Range receivers (analyze_infer_recv.c). */
  { TyKind rr; if (infer_range_call(c, id, rt, &rr)) return rr; }
  /* A Range Enumerable method spinel serves by materializing to an int array:
     infer it as the array version (the array arms below key on `rt`). */
  if (rt == TY_RANGE && range_enum_redispatch(c, id)) rt = TY_INT_ARRAY;
  /* the block form of each_with_index returns the receiver hash (#2417) */
  if (ty_is_hash(rt) && sp_streq(name, "each_with_index") &&
      nt_ref(nt, id, "block") >= 0) return rt;
  if (ty_is_hash(rt) && hash_enum_redispatch(c, id)) rt = TY_POLY_ARRAY;
  /* `each_slice(n) { } / each_cons(n) { }` answer the receiver. When the
     receiver is the marked `to_a` hop above, that is the original Hash or
     Range, not the pair array the loop walked (#3842). */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode") &&
      ((argc == 1 && (sp_streq(name, "each_slice") || sp_streq(name, "each_cons"))) ||
       /* each_entry answers the receiver too, and the value emitter yields it:
          left on the pair array's type the two disagreed and the C compiler
          was handed a hash where an array was declared (#3895) */
       (argc == 0 && (sp_streq(name, "each_entry") ||
                      /* each_entry is renamed to each before this point */
                      sp_streq(name, "each") ||
                      /* reverse_each over an Enumerator reaches the array
                         machinery through the same marked hop, and answers the
                         Enumerator, not the array it walked (#4325) */
                      sp_streq(name, "reverse_each")))) &&
      nt_kind(nt, recv) == NK_CallNode && nt_str(nt, recv, "enum_recv")) {
    int orecv = nt_ref(nt, recv, "receiver");
    if (orecv >= 0) return infer_type(c, orecv);
  }
  /* A block each-family call returns its receiver (each, each_value/each_key/
     each_pair, each_with_index, reverse_each), so the value form composes:
     r = arr.each { }; arr.each { }.map { }. Gated to receivers that define
     the method -- the respond_to? machinery probes a synthesized
     `recv.m { }`, and an unconditional arm would make every type "respond"
     to each. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode")) {
    /* `obj.__enum_to_a.<m> { }` (m in each / each_with_index / reverse_each) is
       the desugared each-family call on a user Enumerable / Struct receiver:
       Ruby returns the enumerable itself, so type it as the original receiver
       obj, not the intermediate member array (#2546/#2547). The codegen value
       form (emit_iter_value_expr) yields obj to match. */
    if ((sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      int orecv = rnm && sp_streq(rnm, "__enum_to_a") ? nt_ref(nt, recv, "receiver") : -1;
      if (orecv >= 0) return infer_type(c, orecv);
    }
    int enumerable_recv = ty_is_array(rt) || ty_is_hash(rt) ||
                          rt == TY_RANGE || rt == TY_ENUMERATOR;
    /* each_entry belongs here too: Enumerable#each_entry answers the receiver
       exactly as #each does, and typing it nil left `arr.each_entry { }.class`
       reading NilClass (#3395). */
    if (enumerable_recv &&
        (sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")))
      return rt;
    /* A poly receiver answers `each` with itself, exactly as the typed kinds
       do -- that is the receiver's own type, so claiming it says nothing the
       receiver did not already say. Left UNKNOWN, a chained
       `h.each { }.length` had nothing to dispatch on and was refused at
       compile time, naming nothing: `JSON.parse("[]").each { }.size` reached
       that even though the value is an Array at run time (#3987). A receiver
       with no `each` raises where it always did, from the run-time dispatch.
       The value form of the iterator (emit_iter_value_expr) yields the
       receiver for a poly one too, so the two agree. */
    if (rt == TY_POLY && nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")))
      return TY_POLY;
    if (ty_is_hash(rt) &&
        (sp_streq(name, "each_value") || sp_streq(name, "each_key") ||
         sp_streq(name, "each_pair")))
      return rt;
  }
  /* `recv.sort_by { }` always returns a new Array. For a typed array receiver
     it stays that array's type (arm below); for a hash / poly / not-yet-settled
     receiver it is a generic Array. Without this a hash sort_by whose receiver
     was still poly when the local was typed settled to poly, and a downstream
     `.first(n).each` then had no array to iterate (#2876). */
  if (recv >= 0 && sp_streq(name, "sort_by") && nt_ref(nt, id, "block") >= 0) {
    /* A hash / poly / not-yet-settled receiver yields a generic Array. A typed
       array or a Range keep their own element-typed sort path below. */
    TyKind srt = infer_type(c, recv);
    if (ty_is_hash(srt) || srt == TY_POLY || srt == TY_UNKNOWN) return TY_POLY_ARRAY;
  }
  /* `poly.members` on a Struct/Data read out of a container: the field-name
     symbols as a generic Array. #deconstruct is the member values (like to_a). */
  if (recv >= 0 && (sp_streq(name, "members") || sp_streq(name, "deconstruct")) &&
      argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !an_user_defines_method(c, name) && infer_type(c, recv) == TY_POLY)
    return TY_POLY_ARRAY;
  /* `poly.reject/select/filter { }` on a value only known to be an array at
     runtime (read out of a poly container): a filtered generic Array. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "map!") || sp_streq(name, "collect!")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY_ARRAY;
  /* The filtering siblings answer whatever kind the receiver is -- Hash#select
     is a Hash, Array#select an Array -- and only the runtime value says which,
     so the result rides boxed (#3449). */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY;
  /* `poly.times { }` / `upto(n) { }` / `downto(n) { }` answer the receiver,
     which codegen unboxes to an sp_int before handing the call to the typed
     emitters. step is left out: a Float owns it too. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      ((argc == 0 && sp_streq(name, "times")) ||
       (argc == 1 && (sp_streq(name, "upto") || sp_streq(name, "downto")))) &&
      infer_type(c, recv) == TY_POLY && !an_user_defines_or_reads(c, name))
    return TY_INT;
  /* `poly.find { }` / `detect { }` answer the winning ELEMENT, boxed. Without
     an arm here they fell through to the last-resort Hash face below, which
     types them as the winning [k, v] pair -- and the emitter, which answers
     the element either way, then had its already-boxed value boxed a second
     time under that array type. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "find") || sp_streq(name, "detect")) &&
      infer_type(c, recv) == TY_POLY && !an_user_defines_or_reads(c, name))
    return TY_POLY;
  /* find_all is NOT the third spelling of select: Hash#select answers a Hash,
     Hash#find_all the [k, v] pairs as an Array. take_while and drop_while
     answer an Array the same way. All three are an array whatever the
     receiver turns out to be. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "find_all") || sp_streq(name, "take_while") ||
       sp_streq(name, "drop_while")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY_ARRAY;
  /* `poly.each_slice(n) { }` / `each_cons(n) { }` answer the receiver, whatever
     kind it turns out to be -- an Array for an Array, the Hash itself for a
     Hash -- so the result rides boxed, like the filtering siblings above. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 1 &&
      (sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY;
  /* `poly.zip(other) { }` / `poly.cycle(n) { }` answer nil, as they do for a
     typed receiver. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && argc == 1 &&
      (sp_streq(name, "zip") || sp_streq(name, "cycle")) &&
      infer_type(c, recv) == TY_POLY)
    return TY_NIL;
  /* `poly.zip(other...)` on a value only known to be an array at runtime
     (e.g. a row that is a block param of an outer nested-array iterator):
     a poly array of tuples, matching the array-receiver form (#3190). */
  if (recv >= 0 && sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0 &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY_ARRAY;
  /* `poly_recv.sum { }` (a group_by bucket, a case-merged local) folds the
     block result with sp_poly_add, so the accumulation is a boxed poly value.
     Concrete typed arrays keep their int/float/string sum arms below. (#2872) */
  if (recv >= 0 && sp_streq(name, "sum") && nt_ref(nt, id, "block") >= 0 &&
      infer_type(c, recv) == TY_POLY)
    return TY_POLY;
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
  /* Object#itself is the receiver, whatever its type -- the scattered per-type
     arms below predate this and remain harmless. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "itself") &&
      nt_ref(nt, id, "block") < 0 && !an_user_defines_or_reads(c, "itself"))
    return infer_type(c, recv);

  /* A literal integer power whose result exceeds int64 (`10 ** 30`, `2 ** 70`)
     is a Bignum in every overflow mode (CRuby). Type it bigint so codegen emits
     sp_bigint_pow rather than the saturating float sp_int_pow. */
  if (sp_streq(name, "**") && recv >= 0 && argc == 1) {
    long long base, exp;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &exp) &&
        infer_int_pow_overflows(base, exp))
      return TY_BIGINT;
  }
  /* Integer ** <negative literal>: a Rational in CRuby, typed statically. A
     runtime exponent keeps the static Integer result (typing it poly would
     cascade through every int-arithmetic consumer -- see limitations.md); the
     negative case raises loudly, and the poly-dispatched path (promote-mode
     params) resolves the class at runtime in sp_poly_pow. */
  /* The cheap operand-type and name gates come BEFORE infer_type(c, recv): that
     receiver re-inference recurses through infer_call, so calling it for every
     1-arg call node makes a wide call graph explode (#2707). */
  if (a0 == TY_INT && sp_streq(name, "**") && recv >= 0 && argc == 1 &&
      infer_type(c, recv) == TY_INT) {
    long long exp;
    if (infer_const_int_node(nt, argv[0], &exp) && exp < 0) return TY_RATIONAL;
  }
  /* Integer with a Rational/Complex operand: ** Complex is Complex; ** Rational
     is a Float (by design, see codegen); fdiv is Float, div is the Integer floor. */
  if (recv >= 0 && argc == 1 && (a0 == TY_COMPLEX || a0 == TY_RATIONAL) &&
      (sp_streq(name, "**") || sp_streq(name, "fdiv") || sp_streq(name, "div")) &&
      infer_type(c, recv) == TY_INT) {
    if (sp_streq(name, "**") && a0 == TY_COMPLEX) return TY_COMPLEX;
    if (sp_streq(name, "**") && a0 == TY_RATIONAL) return TY_FLOAT;
    if (sp_streq(name, "fdiv") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_FLOAT;
    if (sp_streq(name, "div") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_INT;
  }
  /* A literal left shift whose result exceeds int64 (`1 << 64`, the 2**64 mask)
     is a Bignum -- type it bigint so codegen emits a bigint shift, not a UB C
     `1LL << 64LL`. */
  if (sp_streq(name, "<<") && recv >= 0 && argc == 1) {
    long long base, amount;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &amount) &&
        infer_int_shl_overflows(base, amount))
      return TY_BIGINT;
  }

  /* `@table[i][j]` on a poly-array dispatch table of int-returning entries
     yields an int (a method/peek table). Resolves the optcarrot CPU's
     fetch/peek/store, which would otherwise run boxed-poly. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_double_index_int(c, id))
    return TY_INT;
  /* `@nested[i]` on a poly array of int arrays yields an int array (nested
     array of int arrays -- @chr_banks / @nmt_mem). Without this the element
     read is a boxed poly and cascades poly through the PPU. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_index_int_array(c, id))
    return TY_INT_ARRAY;
  /* `CONST[i]` on a poly-array constant of int arrays (WAVE_FORM / TILE_LUT
     shapes) yields an int array; codegen unboxes the poly element. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && const_poly_index_int_array(c, id))
    return TY_INT_ARRAY;

  /* Complex / Rational value types. */
  /* A class-tagged poly value answers #name/#to_s/#inspect with its class name
     (Base.subclasses / #ancestors hand back boxed classes, #2656). Only when no
     user class defines the method -- then it dispatches to that instead. */
  /* Boxed (poly) receivers: the run of poly-face arms of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_poly_call(c, id, rt, &rr)) return rr; }
  /* bool/nil <=> : 0 for an equal immediate pair, nil otherwise (#2733) */
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>") &&
      (rt == TY_BOOL || rt == TY_NIL)) return TY_POLY;
  /* __enum_chain(arr): the desugared Enumerable#chain / Enumerator#+ (#2545) */
  if (recv < 0 && sp_streq(name, "__enum_chain") && argc == 1) return TY_ENUMERATOR;
  /* Dir surface (#2823, #2828, #2830) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
    if (sp_streq(name, "empty?") && argc == 1) return TY_BOOL;
    if (sp_streq(name, "home") && argc == 1) return TY_STRING;
    if (sp_streq(name, "glob") && (argc == 1 || argc == 2)) return TY_STR_ARRAY;
    if (sp_streq(name, "for_fd") && argc == 1) return TY_DIR;
    if (sp_streq(name, "fchdir") && argc == 1) return TY_INT;
  }
  /* the desugared ENV snapshot (#2742) */
  if (recv < 0 && sp_streq(name, "__env_to_h") && argc == 0) return TY_STR_STR_HASH;
  /* `exception: false` on an unparseable String answers nil, so the call is
     nilable and cannot be the unboxed value type (#3893). */
  if (recv < 0 && (sp_streq(name, "Complex") || sp_streq(name, "Rational")) &&
      argc == 2 && infer_type(c, argv[0]) == TY_STRING &&
      nt_type(nt, argv[1]) &&
      (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
       sp_streq(nt_type(nt, argv[1]), "HashNode"))) {
    int kn9 = 0; const int *els9 = nt_arr(nt, argv[1], "elements", &kn9);
    int only_exc9 = kn9 > 0;
    for (int e = 0; e < kn9 && only_exc9; e++) {
      int key9 = els9 ? nt_ref(nt, els9[e], "key") : -1;
      const char *kt9 = key9 >= 0 ? nt_type(nt, key9) : NULL;
      const char *kn9s = (kt9 && sp_streq(kt9, "SymbolNode")) ? nt_str(nt, key9, "value") : NULL;
      if (!kn9s || !sp_streq(kn9s, "exception")) only_exc9 = 0;
    }
    if (only_exc9) return TY_POLY;
  }
  if (recv < 0 && sp_streq(name, "Complex")) return TY_COMPLEX;
  if (recv < 0 && sp_streq(name, "Rational") && (argc == 1 || argc == 2)) {
    /* a bignum operand needs the big Rational, which is a boxed value */
    if (infer_type(c, argv[0]) == TY_BIGINT ||
        (argc == 2 && infer_type(c, argv[1]) == TY_BIGINT)) return TY_POLY;
    return TY_RATIONAL;
  }
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    if (rrty && sp_streq(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Complex") &&
        (sp_streq(name, "polar") || sp_streq(name, "rect") || sp_streq(name, "rectangular")))
      return TY_COMPLEX;
  }
  /* Numeric receivers: the Complex and Rational faces, the mixed Integer/Float x Complex operators, and the curried-Proc accumulator (analyze_infer_recv.c). */
  { TyKind rr; if (infer_numeric_call(c, id, rt, &rr)) return rr; }

  /* Safe navigation &. : nil receiver always short-circuits to nil */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.") && rt == TY_NIL) return TY_NIL;
  }

  /* nil receiver: type inference for NilClass methods */
  if (recv >= 0 && sp_streq(name, "display") && argc == 0 &&
      !(ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR))
    return TY_NIL;
  if (recv >= 0 && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      ty_is_object(rt)) return TY_BOOL;
  if (recv >= 0 && rt == TY_SYMBOL && argc == 0 && sp_streq(name, "encoding"))
    return TY_POLY;  /* a boxed Encoding value */
  if (recv >= 0 && (rt == TY_BOOL || rt == TY_SYMBOL || rt == TY_FLOAT) && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?"))) return TY_BOOL;
  if (recv >= 0 && rt == TY_FLOAT && argc == 1 && sp_streq(name, "===")) return TY_BOOL;  /* (#2400) */
  if (recv >= 0 && rt == TY_NIL) {
    if (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
        sp_streq(name, "===") || sp_streq(name, "equal?") || sp_streq(name, "eql?") ||
        sp_streq(name, "!~")) return TY_BOOL;
    if (sp_streq(name, "=~")) return TY_NIL;
    if (sp_streq(name, "rationalize")) return TY_RATIONAL;
    /* nil <=> nil is 0 (int); any other operand answers nil */
    if (sp_streq(name, "<=>") && argc == 1)
      return infer_type(c, argv[0]) == TY_NIL ? TY_INT : TY_NIL;
    if (sp_streq(name, "tap")) return TY_POLY;  /* the (boxed) nil receiver */
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) &&
        nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk9 = nt_ref(nt, id, "block");
      int bd9 = nt_ref(nt, blk9, "body");
      int bn9 = 0; const int *bb9 = bd9 >= 0 ? nt_arr(nt, bd9, "body", &bn9) : NULL;
      if (bn9 >= 1) {
        TyKind bt9 = infer_type(c, bb9[bn9 - 1]);
        return bt9 == TY_NIL ? TY_POLY : bt9;
      }
    }
    if (sp_streq(name, "to_c")) return TY_COMPLEX;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?")) return TY_BOOL;
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int")) return TY_INT;
    if (sp_streq(name, "to_f")) return TY_FLOAT;
    if (sp_streq(name, "to_r")) return TY_RATIONAL;
    if (sp_streq(name, "to_a")) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_h")) return TY_SYM_POLY_HASH;
    if (sp_streq(name, "respond_to?")) return TY_BOOL;
  }

  /* <array>.cycle.first(n) / .take(n) -> same array kind (bounded consumer of the
     infinite cycle; the unbounded forms stay a loud reject). */
  if (recv >= 0 && (sp_streq(name, "first") || sp_streq(name, "take")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "cycle") &&
      nt_ref(nt, recv, "block") < 0) {
    int cargs = nt_ref(nt, recv, "arguments");
    int cac = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &cac);
    int pr = nt_ref(nt, recv, "receiver");
    if (cac == 0 && pr >= 0) { TyKind rt2 = infer_type(c, pr); if (ty_is_array(rt2)) return rt2; }
  }

  /* int_array.chunk_while/slice_when/chunk { |...| } .to_a -> a poly array (runs / pairs) */
  if (recv >= 0 && sp_streq(name, "to_a") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "chunk_while") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_when") ||
       sp_streq(nt_str(nt, recv, "name"), "chunk") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_before") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_after")) &&
      nt_ref(nt, recv, "block") >= 0 &&
      (nt_ref(nt, recv, "arguments") < 0 ||
       (!sp_streq(nt_str(nt, recv, "name"), "slice_before") &&
        !sp_streq(nt_str(nt, recv, "name"), "slice_after")))) {
    int pr = nt_ref(nt, recv, "receiver");
    if (pr >= 0) {
      TyKind prt = infer_type(c, pr);
      if (prt == TY_INT_ARRAY || prt == TY_POLY_ARRAY || prt == TY_STR_ARRAY ||
          prt == TY_FLOAT_ARRAY ||
          /* a boxed receiver walks whatever sp_poly_arr_recv renders -- a
             hash's [key, value] pairs, an array's elements (#3451) */
          prt == TY_POLY ||
          (prt == TY_UNKNOWN && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "ArrayNode")) ||
          (prt == TY_RANGE && range_enum_redispatch(c, recv)))
        return TY_POLY_ARRAY;
      /* hash.chunk { |k, v| key }.to_a materializes the same way (the chunk
         first-class emitter iterates the hash directly); gated to the named
         two-param block shape the emitter serves. */
      if (ty_is_hash(prt) && sp_streq(nt_str(nt, recv, "name"), "chunk") &&
          block_param_name(c, nt_ref(nt, recv, "block"), 0) &&
          block_param_name(c, nt_ref(nt, recv, "block"), 1))
        return TY_POLY_ARRAY;
    }
  }

  /* chunk_while/slice_when/chunk { } standing on its own (no .to_a terminal,
     which the arm above claims first): a first-class Enumerator over the
     eagerly materialized runs */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "chunk_while") || sp_streq(name, "slice_when") ||
       sp_streq(name, "chunk") ||
       ((sp_streq(name, "slice_before") || sp_streq(name, "slice_after")) &&
        nt_ref(nt, id, "arguments") < 0))) {
    TyKind crt = infer_type(c, recv);
    if (crt == TY_POLY_ARRAY || crt == TY_INT_ARRAY || crt == TY_STR_ARRAY ||
        (crt == TY_RANGE && range_enum_redispatch(c, id))) {
      if (!an_chunk_family_to_a(c, id)) return TY_ENUMERATOR;
    }
  }

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last of an empty array is nil, boxed to poly (codegen emits
           sp_box_nil). min/max/pop/shift/sample keep the historical int-0
           shortcut pending their own nil arms. */
        if ((sp_streq(name, "first") || sp_streq(name, "last") ||
             sp_streq(name, "sample")) && argc == 0) return TY_POLY;  /* nil, boxed (#2322) */
        if ((sp_streq(name, "nil?") || sp_streq(name, "empty?")) && argc == 0) return TY_BOOL;
        /* [] + X / [] - X / set-ops keep the OTHER operand's array kind (the
           codegen emits sp_<kind>Array_<op>(NULL, X)); the result type must
           agree with that kind, not the default poly array. */
        if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
             sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
             sp_streq(name, "intersection")) && argc == 1) {
          TyKind at = infer_type(c, argv[0]);
          if (ty_is_array(at) || at == TY_POLY_ARRAY) return at;
        }
        if ((sp_streq(name, "min") || sp_streq(name, "max") ||
             sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) return TY_INT;
        rt = TY_POLY_ARRAY;
      }
    }
    /* mirror for an empty HASH literal receiver (`{}.freeze`): without this
       the identity `freeze` stays unresolved, so `CONST = {}.freeze` never
       gets a type and the constant is dropped from codegen entirely -- reads
       then raise "uninitialized constant" or break the C build when the
       constant is a typed ivar's || fallback (#1758). Deliberately
       TY_STR_POLY_HASH, the same C type codegen emits for a bare {}, so the
       declaration, initializer, and readers agree (POLY_POLY would disagree
       with sp_StrPolyHash_new and produce garbage). */
    else if (rty && (sp_streq(rty, "HashNode") || sp_streq(rty, "KeywordHashNode"))) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* object query methods on a bare {} fold to a bool (see codegen) */
        if (argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "frozen?") ||
                          sp_streq(name, "empty?") || sp_streq(name, "any?"))) return TY_BOOL;
        if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                          sp_streq(name, "instance_of?"))) return TY_BOOL;
        if (argc == 0 && sp_streq(name, "to_h")) return TY_STR_POLY_HASH;   /* (#2410) */
        if (argc <= 1 && sp_streq(name, "sum")) {
          /* the empty collection answers the SEED itself, so the call is the
             seed's type -- except that nil and true/false have no scalar slot
             to be answered in, and ride boxed (`{}.sum(nil)` is nil) */
          TyKind st0 = argc == 1 ? infer_type(c, argv[0]) : TY_INT;         /* (#2416) */
          if (st0 == TY_NIL || st0 == TY_BOOL || st0 == TY_VOID) st0 = TY_POLY;
          return st0;
        }
        if (argc == 0 && (sp_streq(name, "min") || sp_streq(name, "max"))) return TY_POLY;
        if (argc == 0 && sp_streq(name, "minmax")) return TY_POLY_ARRAY;    /* (#2406) */
        if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, "<=") ||
                          sp_streq(name, ">") || sp_streq(name, ">="))) return TY_BOOL;  /* (#2399) */
        rt = TY_STR_POLY_HASH;
      }
    }
  }

  /* `<&block-param>.call(...)` inside a yielding method: the explicit-call form
     of yield. Its value is the call-site block's value (resolved like yield). */
  {
    int emi = (int)(comp_scope_of(c, id) - c->scopes);
    if (emi > 0 && is_blk_param_call(c, id, emi)) {
      /* In a proc form the block is a real proc supplied per call site, so the
         call answers poly uniformly -- the same reason the YieldNode arm does.
         Resolving it against one site's block fixes the clone to that site,
         and a method declaring `&blk` reaches the dispatch this way rather
         than through a yield (#3408). */
      if (c->scopes[emi].is_proc_form) return TY_POLY;
      /* The per-site answer is the FIRST concrete block's type, but one body
         is emitted per site and a `&block` method reached from two sites whose
         blocks answer differently (Enumerable's collector and its generator)
         then boxes one of them against the other's type -- the C compiler saw
         an sp_RbVal cast to a pointer. When the sites disagree the value is
         only known at run time (#3793). */
      TyKind bfirst = yield_value_type(c, emi);
      { int sv_ua = g_yvt_unify_all;
        g_yvt_unify_all = 1;
        TyKind ball = yield_value_type(c, emi);
        g_yvt_unify_all = sv_ua;
        if (ball != bfirst && ball != TY_UNKNOWN) return TY_POLY; }
      return bfirst;
    }
  }

  /* __dir__ -> the source directory (a string) */
  if (recv < 0 && sp_streq(name, "__dir__") && argc == 0) return TY_STRING;

  /* Kernel#caller -> the call stack as strings (empty in release builds) */
  if (recv < 0 && sp_streq(name, "caller") && argc <= 2 &&
      !an_bare_call_class_owned(c, id)) return TY_STR_ARRAY;
  /* caller_locations -> an array of Backtrace::Location objects. AOT builds keep
     no runtime frame stack (like `caller`, which is empty in release), so this is
     an empty array. Returning a poly array (not nil) keeps `&.first&.label`,
     `.each`, `.map`, and `.is_a?(Array)` well-typed and nil-safe. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "caller_locations") && argc <= 2) return TY_POLY_ARRAY;

  /* bare `name` inside a class method body -> the class name string */
  if (recv < 0 && sp_streq(name, "name") && argc == 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }
  /* `self.name` / `self.to_s` inside a class method -> the class name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }

  /* loop { break val } -> the type of the break value */
  if (recv < 0 && sp_streq(name, "loop") && !an_bare_call_class_owned(c, id)) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        TyKind bt = scan_break_type(c, body, 0);
        if (bt != TY_UNKNOWN) return bt;
      }
      /* A loop ended by StopIteration answers that exception's #result -- the
         exhausted enumerator. Only a body that pulls from one can end that
         way; every other break-less loop keeps its nil (#3588). */
      if (body >= 0 && an_subtree_calls_enum_next(c, body)) return TY_POLY;
      return TY_NIL;
    }
    /* blockless `loop` is an infinite Enumerator yielding nil (#3236) */
    return TY_ENUMERATOR;
  }

  /* catch(:tag) { ... } -> unify the block's last value with every throw
     value that can target the tag, across method boundaries (the throw need
     not be syntactically inside the body). */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "catch")) {
    int blk = nt_ref(nt, id, "block");
    TyKind result = TY_UNKNOWN;
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
        if (bn > 0) result = infer_type(c, bb[bn - 1]);
      }
      const char *tag = NULL;
      int targ = nt_ref(nt, id, "arguments");
      int tac = 0; const int *tav = targ >= 0 ? nt_arr(nt, targ, "arguments", &tac) : NULL;
      if (tac >= 1 && nt_type(nt, tav[0])) {
        if (sp_streq(nt_type(nt, tav[0]), "SymbolNode")) tag = nt_str(nt, tav[0], "value");
        else if (sp_streq(nt_type(nt, tav[0]), "StringNode")) tag = nt_str(nt, tav[0], "unescaped");
      }
      TyKind tt = scan_throw_type(c, tag);
      if (tt != TY_UNKNOWN) result = ty_unify(result, tt);
    }
    return result == TY_UNKNOWN ? TY_NIL : result;
  }

  /* recv.instance_eval/exec { ... } -> the block's last-expression type
     (bare calls inside resolve via the ie node->class map). A trampoline
     method `recv.M { ... }` resolves the same way. */
  int ie_kind = (recv >= 0 && (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec")) &&
                 ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  /* a non-object receiver (nil, a scalar) is served by the non-object splice;
     type it by the block's last-expression the same way (#2956) */
  if (!ie_kind && recv >= 0 && !ty_is_object(rt) &&
      (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec"))) {
    int nblk = nt_ref(nt, id, "block");
    if (nblk >= 0 && nt_type(nt, nblk) && sp_streq(nt_type(nt, nblk), "BlockNode")) ie_kind = 1;
  }
  if (!ie_kind && recv >= 0 && ty_is_object(rt) && nt_ref(nt, id, "block") >= 0)
    ie_kind = comp_trampoline_kind(c, ty_object_class(rt), name, NULL) != 0;
  /* receiverless instance_eval/exec inside an instance method resolves to self */
  if (!ie_kind && recv < 0 && ie_implicit_self_class(c, id) >= 0) ie_kind = 1;
  if (ie_kind) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      const char *bty = nt_type(nt, blk);
      if (bty && sp_streq(bty, "BlockArgumentNode")) {
        /* `instance_exec(args, &b)` forwards the enclosing method's block; the
           value it produces is that method's own forwarded-block value across
           call sites (the method inlines per site, splicing the literal). */
        Scope *encl = comp_scope_of(c, id);
        int emi = encl ? (int)(encl - c->scopes) : -1;
        if (emi >= 0) {
          TyKind ft = yield_value_type(c, emi);
          if (ft != TY_UNKNOWN && ft != TY_VOID) return ft;
        }
        /* The forwarded block's value isn't statically pinned here (its call
           sites may not be typed yet during the fixpoint). It is a real boxed
           value, not nil -- poly keeps the result a scalar carrier so the
           enclosing method stays inlinable and the splice yields it per site. */
        return TY_POLY;
      }
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn > 0) {
        TyKind bt = infer_type(c, bb[bn - 1]);
        if (bt == TY_VOID) bt = TY_NIL;
        /* A value-carrying break/next can widen the result past the last
           expression (e.g. `next val + 1` poly vs trailing `999` int). */
        TyKind bnt = ie_block_break_next_ty(c, body);
        if (bnt != TY_UNKNOWN)
          bt = (bt == TY_NIL || bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
        return bt;
      }
      return TY_NIL;
    }
  }

  /* <StructClass>.members at the class level: symbol array */
  if (recv >= 0 && sp_streq(name, "members") && argc == 0) {
    const char *mrty = nt_type(nt, recv);
    int mci = -1;
    if (mrty && (sp_streq(mrty, "ConstantReadNode") || sp_streq(mrty, "ConstantPathNode")))
      mci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (mrty && (sp_streq(mrty, "LocalVariableReadNode") ||
                      (sp_streq(mrty, "CallNode") && is_struct_call(c, recv))))
      mci = class_var_static_ci(c, recv);
    if (mci >= 0 && c->classes[mci].is_struct &&
        comp_cmethod_in_chain(c, mci, "members", NULL) < 0) return TY_POLY_ARRAY;
  }
  if (recv >= 0 && sp_streq(name, "keyword_init?") && argc == 0) {
    const char *krty = nt_type(nt, recv);
    int kci = -1;
    if (krty && (sp_streq(krty, "ConstantReadNode") || sp_streq(krty, "ConstantPathNode")))
      kci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (krty && sp_streq(krty, "LocalVariableReadNode"))
      kci = class_var_static_ci(c, recv);
    if (kci >= 0 && c->classes[kci].is_struct &&
        comp_cmethod_in_chain(c, kci, "keyword_init?", NULL) < 0) return TY_POLY;  /* nil/true/false */
  }
  /* Integer.sqrt(Bignum) -> Bignum (#2420) */
  if (recv >= 0 && sp_streq(name, "sqrt") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      infer_type(c, argv[0]) == TY_BIGINT) return TY_BIGINT;
  /* Hash[k: v] desugared to a bare hash literal: transparent passthrough */
  if (recv >= 0 && sp_streq(name, "__hash_brackets_kw")) return infer_type(c, recv);
  /* Hash[] with no arguments: an empty hash (same C type as a bare {}) */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash"))
    return TY_STR_POLY_HASH;
  /* Array/Integer/String/IO.try_convert(x) -> the value or nil (poly)
     (#2325, #2585) */
  if (recv >= 0 && name && sp_streq(name, "try_convert") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "Array") || sp_streq(nt_str(nt, recv, "name"), "Integer") ||
       sp_streq(nt_str(nt, recv, "name"), "String") || sp_streq(nt_str(nt, recv, "name"), "IO")))
    return TY_POLY;
  if (recv >= 0 && name && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash")) {
    if (sp_streq(name, "try_convert")) return TY_POLY;
  }
  /* container-read builtin pre-arms (#3234). The name/argc gates run
     FIRST: the infer_type(recv) probe recurses through the receiver's
     own call chain, and paying it for EVERY one-arg call re-infers each
     chain suffix once more per level -- exponential on deep chains (a
     Rails-scale tree went 23s -> >10min under it). Gated this way only
     the arm names below ever pay the probe. */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "cover?") || sp_streq(name, "gcdlcm") ||
       sp_streq(name, "sum") || sp_streq(name, "inject") ||
       sp_streq(name, "reduce")) &&
      infer_type(c, recv) == TY_POLY) {
    if (sp_streq(name, "cover?")) return TY_BOOL;
    if (sp_streq(name, "gcdlcm")) return TY_INT_ARRAY;
    if (sp_streq(name, "sum") && nt_ref(nt, id, "block") < 0) return TY_POLY;
    if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) &&
        nt_ref(nt, id, "block") < 0 && infer_type(c, argv[0]) == TY_SYMBOL) return TY_POLY;
  }
  /* method(:sym) / <recv>.method(:sym) -> a bound Method object */
  if (name && sp_streq(name, "method") && method_sym_arg(c, id) != NULL) return TY_METHOD;

  /* <method>.call(args) / [] / bind_call(obj, args) -> the target's return
     type (bind_call = bind(obj).call(args), #3246). */
  if (recv >= 0 && rt == TY_METHOD &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]") ||
       (sp_streq(name, "===") && argc >= 1) ||
       (sp_streq(name, "bind_call") && argc >= 1))) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (mi >= 0) return c->scopes[mi].ret == TY_UNKNOWN ? TY_INT : c->scopes[mi].ret;
    /* Unresolved target (a Method that arrived through a parameter or a
       slot): the value is whatever the target answers, boxed by the return
       kind its bind site stamped, so the call yields poly (#4445). Reading
       the raw sp_int as an Integer answered a String's pointer. Under
       promote it was always poly: every method is poly-signatured there. */
    return TY_POLY;
  }
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "to_proc")) return TY_PROC;
  /* A Method read out of a container answers these from its sp_BoundMethod;
     the value is boxed, so the call is poly (#3692). */
  if (recv >= 0 && argc == 0 && infer_type(c, recv) == TY_POLY &&
      sp_streq(name, "owner") && !an_user_defines_or_reads(c, name))
    return TY_POLY;
  /* proc << / >> a Proc read out of a container: a composed Proc (#3655) */
  if (recv >= 0 && argc == 1 && (sp_streq(name, "<<") || sp_streq(name, ">>"))) {
    TyKind crt = infer_type(c, recv), cat = infer_type(c, argv[0]);
    /* a curried Proc composes like the proc it stands for (#3864) */
    int cr_p = (crt == TY_PROC || crt == TY_CURRY), ca_p = (cat == TY_PROC || cat == TY_CURRY);
    if ((cr_p && (ca_p || cat == TY_POLY)) ||
        (ca_p && crt == TY_POLY && sp_streq(name, ">>")))
      return TY_PROC;
    /* a statically non-callable operand still types as the composition:
       the codegen arm raises CRuby's TypeError (callable object is
       expected) in its place. A program that reopens a builtin with its
       own #call disarms the rule -- 5.call works there, so composing a 5
       must too. */
    if ((cr_p || crt == TY_METHOD) && ty_never_callable(cat) &&
        !an_user_defines_or_reads(c, "call"))
      return TY_PROC;
  }
  /* Proc#to_proc is self (#3687) */
  if (recv >= 0 && rt == TY_PROC && argc == 0 && sp_streq(name, "to_proc")) return TY_PROC;
  /* Method/UnboundMethod reflection (#3247) */
  if (recv >= 0 && rt == TY_METHOD && argc == 0) {
    if (sp_streq(name, "original_name")) return TY_SYMBOL;
    if (sp_streq(name, "parameters") || sp_streq(name, "source_location")) return TY_POLY_ARRAY;
    if (sp_streq(name, "dup") || sp_streq(name, "clone")) return TY_METHOD;
    if (sp_streq(name, "unbind")) return TY_METHOD;
    if (sp_streq(name, "super_method")) return TY_METHOD;
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "box")) return TY_NIL;  /* namespace-less: never boxed */
  }
  if (recv >= 0 && rt == TY_METHOD && argc == 1 &&
      (sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "equal?")))
    return TY_BOOL;
  /* Klass.instance_method(:m) -> an (unbound) method object; #bind re-binds (#2676) */
  if (recv >= 0 && sp_streq(name, "instance_method") && method_sym_arg(c, id) != NULL &&
      method_obj_target_mi(c, id) >= 0) return TY_METHOD;
  if (recv >= 0 && rt == TY_METHOD && argc == 1 && sp_streq(name, "bind")) return TY_METHOD;
  /* Method#owner is a class value; #receiver is the bound receiver (#2701) */
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "owner")) return TY_CLASS;
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "receiver")) {
    int mn = method_recv_node(c, recv);
    int mrecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
    if (mrecv >= 0) return infer_type(c, mrecv);
  }
  /* <method>.name -> the method name as a Symbol; .arity -> int */
  if (recv >= 0 && rt == TY_METHOD && argc == 0) {
    if (sp_streq(name, "name")) return TY_SYMBOL;
    if (sp_streq(name, "arity")) return TY_INT;
    if (sp_streq(name, "to_proc")) return TY_PROC;
  }
  /* <poly>.call(args): a boxed Proc publishes its result through the boxed
     return slot, so the value is genuinely dynamic -- type it poly and let
     the call site read the slot intact (unboxing to int truncated an array
     or string result to garbage). A boxed slot may hold a Proc regardless
     of whether some user class defines `call` (the poly dispatch's callable
     pre-arm routes it through the callable machinery), so the result stays
     dynamic even then; the user methods' return type must not constrain it. */
  if (recv >= 0 && rt == TY_POLY &&
      (sp_streq(name, "call") || sp_streq(name, "()")))
    return TY_POLY;

  /* strftime on a poly value that is really a Time formats to a String. A
     nilable Time (`created_at : Time?`) is held as a poly sp_RbVal, so the
     call would otherwise infer poly/unknown and the formatted string get
     discarded; the codegen poly dispatch gives it a SP_BUILTIN_TIME arm that
     formats a real Time and raises NoMethodError otherwise, so its non-raising
     result is always a String. Only when no user class supplies strftime, to
     match the codegen guard. Issue #2457 (family2 nilable value-method). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "strftime") && argc == 1 &&
      infer_type(c, argv[0]) == TY_STRING) {
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand == 0) return TY_STRING;
  }

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) return TY_PROC;

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type;
     Proc#=== (case/when dispatch) IS a call in CRuby */
  /* Proc#=== answers the proc's return VALUE (#3818). Typing it from the
     proc's body pins it to one shape, and a proc that arrives through a slot
     has no body to read, so the answer is boxed. */
  if (recv >= 0 && rt == TY_PROC && sp_streq(name, "===") && argc == 1) return TY_POLY;
  if (recv >= 0 && rt == TY_PROC &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]"))) {
    /* In a proc form, a call on the block parameter is the yield: the block is
       a real proc supplied per call site, so its result is poly uniformly --
       the same reason the YieldNode arm answers poly there. Typing it from any
       one site's block fixes the clone to that site (#3408). */
    return proc_call_ret(c, recv);
  }

  /* Proc composition: proc << proc / proc >> proc -> a new Proc. */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (sp_streq(name, "<<") || sp_streq(name, ">>")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_PROC;

  /* Proc introspection */
  /* parameters(lambda: <bool/nil>) forces the view; same shape as parameters() */
  if (recv >= 0 && rt == TY_PROC && argc == 1 && sp_streq(name, "parameters"))
    return TY_POLY_ARRAY;
  if (recv >= 0 && rt == TY_PROC && argc == 0) {
    if (sp_streq(name, "arity")) return TY_INT;
    if (sp_streq(name, "lambda?")) return TY_BOOL;
    if (sp_streq(name, "parameters")) return TY_POLY_ARRAY;
    if (sp_streq(name, "source_location")) return TY_POLY_ARRAY;  /* [file, line] */
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if (sp_streq(name, "freeze") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "itself")) return TY_PROC;
  }
  /* Proc identity: equal?/eql?/== against another Proc -> bool */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_BOOL;
  /* Hash#default_proc: the stored Hash.new{} block as a first-class Proc
     (NULL-encoded nil when the hash has none) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 0 && sp_streq(name, "default_proc"))
    return TY_PROC;
  /* default_proc= installs the lambda; the assignment's value is the proc (#2371) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 1 && sp_streq(name, "default_proc="))
    return TY_PROC;

  /* k = Struct.new(:a, :b): the value IS the synthesized anonymous struct
     class, as a first-class class object */
  if (anon_struct_ci_for_value(c, id) >= 0) return TY_CLASS;

  /* TY_CLASS method dispatch -- .new on a dynamic class variable returns TY_POLY.
     Exception: self.class.new(...) resolves to the enclosing class statically. */
  if (recv >= 0 && rt == TY_CLASS && sp_streq(name, "new") &&
      nt_type(nt, recv) && !sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      !sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
    int _is_self_class = (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class") &&
      nt_ref(nt, recv, "receiver") >= 0 &&
      nt_type(nt, nt_ref(nt, recv, "receiver")) &&
      sp_streq(nt_type(nt, nt_ref(nt, recv, "receiver")), "SelfNode"));
    /* a local statically holding one STRUCT class (k = Struct.new(..) /
       k = StructKlass) falls through to the static-class .new arms below --
       its typed member accessors then dispatch statically. A plain class
       stays on the Tier-5 dynamic boxed path (test/dynamic_class_new.rb). */
    int _cvi = _is_self_class ? -1 : class_var_static_ci(c, recv);
    if (!_is_self_class &&
        (_cvi < 0 || !(c->classes[_cvi].is_struct || c->classes[_cvi].is_data)))
      return TY_POLY;
  }

  if (recv >= 0 && rt == TY_CLASS && !sp_streq(name, "new")) {
    if (argc == 0 && (sp_streq(name, "to_s") || sp_streq(name, "name") || sp_streq(name, "inspect")))
      return TY_STRING;
    if (argc == 0 && sp_streq(name, "nil?")) return TY_BOOL;
    if (argc == 0 && sp_streq(name, "singleton_class?")) return TY_BOOL;
    if (argc == 0 && sp_streq(name, "frozen?")) return TY_BOOL;
    /* Module#constants -> sym array, recovered from the AST (#2674) */
    /* Class.const_set(:K, v) stores into the existing constant and yields the
       value; only a literal name whose type matches is emittable (#2675). */
    if (sp_streq(name, "const_set") && argc == 2) {
      const char *cs_aty = nt_type(nt, argv[0]);
      const char *cs_qm = NULL;
      if (cs_aty && sp_streq(cs_aty, "SymbolNode")) cs_qm = nt_str(nt, argv[0], "value");
      else if (cs_aty && sp_streq(cs_aty, "StringNode")) cs_qm = nt_str(nt, argv[0], "content");
      LocalVar *cv = cs_qm ? comp_const(c, cs_qm) : NULL;
      if (cv && cv->type != TY_UNKNOWN) return cv->type;
    }
    if (sp_streq(name, "constants") && argc <= 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "class_variables") && argc == 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "included_modules") && argc == 0) return TY_POLY_ARRAY;
    if (argc == 0 && sp_streq(name, "class")) return TY_CLASS;
    /* #superclass is a (nullable) class value: BasicObject's is the nil-class
       sentinel, carried within TY_CLASS (#2654). */
    if (argc == 0 && sp_streq(name, "superclass")) return TY_CLASS;
    if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "!=") ||
                      sp_streq(name, "==="))) return TY_BOOL;
    /* Class ordering is tri-state: true/false when related, nil when the two
       classes have no subclass relationship (CRuby). <=> is -1/0/1 or nil. */
    if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
                      sp_streq(name, ">=") || sp_streq(name, "<=>"))) return TY_POLY;
    if (argc == 0 && sp_streq(name, "ancestors")) return TY_POLY_ARRAY;
    if (argc == 0 && sp_streq(name, "subclasses")) return TY_POLY_ARRAY;
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) return TY_BOOL;
    if (argc == 1 && sp_streq(name, "include?")) return TY_BOOL;
    if (argc == 1 && sp_streq(name, "class_variable_defined?")) return TY_BOOL;
    if (sp_streq(name, "class_variable_get") || sp_streq(name, "class_variable_set")) return TY_POLY;
    if (argc <= 1 && (sp_streq(name, "instance_methods") ||
                      sp_streq(name, "public_instance_methods") ||
                      sp_streq(name, "private_instance_methods") ||
                      sp_streq(name, "protected_instance_methods"))) return TY_POLY;
    /* a user class method on a Class-typed value carried in a plain variable
       (`model.table_name`): unify the return types of every user class
       defining it (poly on disagreement). A constant/accessor receiver keeps
       its existing dispatch, so only fire for a variable receiver (#2445). */
    {
      int recv_is_var = class_recv_is_dynamic(c, recv);
      TyKind uret = TY_UNKNOWN; int nc = recv_is_var ? 0 : -1000, set = 0;
      for (int k = 0; recv_is_var && k < c->nclasses; k++) {
        if (is_builtin_reopen(c->classes[k].name)) continue;
        int kmi = comp_cmethod_in_chain(c, k, name, NULL);
        if (kmi < 0) continue;
        /* A candidate that yields, takes a block, or has a rest param has no
           arm the emitter can build, so it kills the whole dispatch. A
           candidate with the WRONG ARITY does not: this call cannot reach it,
           so it neither contributes a return type nor vetoes the others
           (#4129). Codegen's cls_arm_takes_argc is the same rule. */
        if (c->scopes[kmi].rest_idx >= 0 || c->scopes[kmi].yields ||
            (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0])) { nc = 0; break; }
        if (argc < c->scopes[kmi].nrequired || argc > c->scopes[kmi].nparams) continue;
        nc++;
        TyKind kr = (TyKind)c->scopes[kmi].ret;
        if (!set) { uret = kr; set = 1; }
        else if (kr != uret) uret = TY_POLY;
      }
      if (nc > 0 && nt_ref(nt, id, "block") < 0)
        return (uret == TY_UNKNOWN || uret == TY_VOID) ? TY_POLY : uret;
    }
  }

  /* __method__ / __callee__ -> the enclosing method's name (a symbol), or
     nil at the top level, where the enclosing scope has no name (matching
     the codegen, which emits sp_box_nil() there) */
  if (recv < 0 && argc == 0 &&
      (sp_streq(name, "__method__") || sp_streq(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    return (s && s->name && s->name[0]) ? TY_SYMBOL : TY_NIL;
  }

  /* identity methods: return the receiver unchanged (clone also with its
     freeze: keyword argument) */
  if (recv >= 0 &&
      (argc == 0 ||
       (argc == 1 && sp_streq(name, "clone") && argv && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))) &&
      (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
       sp_streq(name, "dup") || sp_streq(name, "clone")) &&
      /* a generated READER of the name owns it on a concrete object, as any
         reader does in CRuby: fall through to the member-read rule (#4190) */
      !(ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR))
    return rt;

  /* bareword freeze (implicit self) returns self, so `def seal = freeze` and
     other freeze-as-value uses stay typed as the instance. (Matches the
     codegen arm that lowers bareword freeze to self.) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "freeze")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      return ty_object(s->class_id);
  }
  /* bareword frozen? reads the instance's GC-header bit (see the codegen arm) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "frozen?")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && !s->is_cmethod &&
        comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      return TY_BOOL;
  }

  /* x.class -> a first-class Class value for every known receiver kind
     (name-backed for builtins, id-backed for user objects) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "class")) {
    /* empty container literal receivers coerce like everywhere else */
    if (rt == TY_UNKNOWN && nt_type(nt, recv)) {
      const char *rty0 = nt_type(nt, recv);
      int en0 = 0;
      if (sp_streq(rty0, "ArrayNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) return TY_CLASS; }
      if (sp_streq(rty0, "HashNode") || sp_streq(rty0, "KeywordHashNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) return TY_CLASS; }
      /* an argument-less `Array.new` is that same empty array (#3613) */
      if (sp_streq(rty0, "CallNode") && nt_str(nt, recv, "name") &&
          sp_streq(nt_str(nt, recv, "name"), "new") && nt_ref(nt, recv, "block") < 0) {
        int arn0 = nt_ref(nt, recv, "receiver");
        int aa0 = nt_ref(nt, recv, "arguments"); int aac0 = 0;
        if (aa0 >= 0) nt_arr(nt, aa0, "arguments", &aac0);
        if (aac0 == 0 && arn0 >= 0 && nt_type(nt, arn0) &&
            sp_streq(nt_type(nt, arn0), "ConstantReadNode") &&
            nt_str(nt, arn0, "name") && sp_streq(nt_str(nt, arn0, "name"), "Array"))
          return TY_CLASS;
      }
      /* top-level `self.class`: self is main (an Object) -> Object (#3035) */
      if (sp_streq(rty0, "SelfNode")) {
        Scope *ss = comp_scope_of(c, id);
        if (!ss || ss->class_id < 0) return TY_CLASS;
      }
    }
    /* a member/method literally named `class` (a Data/Struct member) shadows
       Object#class: fall through to the reader/method dispatch (#2975) */
    if (ty_is_object(rt) &&
        !(ty_object_class(rt) >= 0 && comp_reader_in_chain(c, ty_object_class(rt), "class", NULL)))
      return TY_CLASS;
    if (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
        rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL || rt == TY_POLY ||
        rt == TY_METHOD || rt == TY_PROC || rt == TY_IO || rt == TY_ARGF ||
        rt == TY_MATCHDATA || rt == TY_REGEX ||
        rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_CURRY ||
        rt == TY_FIBER || rt == TY_ENUMERATOR || rt == TY_TMS ||
        ty_is_array(rt) || ty_is_hash(rt))
      return TY_CLASS;
  }

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "name") || sp_streq(name, "to_s")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class"))
    return TY_STRING;

  /* __ENCODING__.name / .to_s / .inspect -> the encoding name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SourceEncodingNode"))
    return TY_STRING;
  /* <enc>.encoding.name -> the encoding name string */
  if (recv >= 0 && argc == 0 && sp_streq(name, "name") && rt == TY_POLY &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* Module.singleton_writer= / Module.singleton_reader */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
       sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) {
      ClassInfo *cls = &c->classes[ci];
      int nlen = (int)strlen(name);
      /* setter: name ends with '=' */
      if (nlen > 1 && name[nlen - 1] == '=') {
        char base[256]; int blen = nlen - 1;
        if (blen > 0 && blen < (int)sizeof(base)) {
          memcpy(base, name, (size_t)blen); base[blen] = '\0';
          if (comp_is_sg_writer(cls, base)) return TY_VOID;
        }
      }
else {
        /* an accessor backed by the class-level ivar reads that slot's type;
           the alias table maps a renamed accessor onto it (#3776) */
        const char *rn = comp_resolve_alias(c, ci, name);
        const char *base2 = rn ? rn : name;
        if (comp_is_sg_reader(cls, base2)) {
          if (comp_is_sg_civ(cls, base2)) {
            char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base2);
            int ivi = comp_ivar_index(cls, ivn);
            if (ivi >= 0 && cls->ivar_types[ivi] != TY_UNKNOWN) return ivar_value_ty(cls, ivi);
          }
          return TY_POLY;
        }
      }
    }
  }
  /* self.singleton_writer= / self.singleton_reader: inside a class method
     or directly in a class/module body (g_cbody_class_id). */
  if ((recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) ||
      (recv < 0 && argc == 0)) {
    Scope *_self = comp_scope_of(c, id);
    int _sg_cid = (_self && _self->is_cmethod && _self->class_id >= 0)
                  ? _self->class_id : g_cbody_class_id;
    if (_sg_cid >= 0) {
      ClassInfo *_cls = &c->classes[_sg_cid];
      int _nlen = (int)strlen(name);
      if (_nlen > 1 && name[_nlen - 1] == '=') {
        char _base[256]; int _blen = _nlen - 1;
        if (_blen > 0 && _blen < (int)sizeof(_base)) {
          memcpy(_base, name, (size_t)_blen); _base[_blen] = '\0';
          if (comp_is_sg_writer(_cls, _base)) return TY_VOID;
        }
      }
      else {
        /* the alias table maps a renamed accessor onto the reader (#3788) */
        const char *_rn = comp_resolve_alias(c, _sg_cid, name);
        if (comp_is_sg_reader(_cls, _rn ? _rn : name)) return TY_POLY;
      }
    }
  }

  /* FFI: call on a module that registered ffi_func/ffi_buffer/ffi_read_* */
  if (recv >= 0 && nt_type(nt, recv)) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (sp_streq(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (sp_streq(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      /* native binding (Path B): a native_func returns its declared type,
         gated by the module's require-gate feature. */
      int nvi = comp_native_find(c, rcmod, name);
      if (nvi >= 0) {
        const char *feat = c->native_funcs[nvi].feat;
        if (!feat || !feat[0] || sp_feature_enabled(feat))
          return native_spec_to_ty(c->native_funcs[nvi].ret);
      }
      int fi = ffi_find_func(c, rcmod, name);
      if (fi >= 0) return ffi_spec_to_ty(c->ffi_funcs[fi].ret);
      /* ffi_buffer: Module.buf_name returns the static char* (ptr type -> TY_POLY) */
      if (ffi_find_buf(c, rcmod, name) >= 0) return TY_POLY;
      /* ffi_read_*: Module.reader_name(buf) returns int or ptr */
      int ri = ffi_find_reader(c, rcmod, name);
      if (ri >= 0) {
        const char *kind = c->ffi_readers[ri].kind;
        if (kind && sp_streq(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
      /* ffi_struct: Name_new -> ptr, Name_get_<f> -> the field's type,
         Name_set_<f> -> nil. */
      int fsi, ffi;
      int fsm = ffi_struct_method(c, rcmod, name, &fsi, &ffi);
      if (fsm == FFI_SM_NEW) return TY_POLY;
      if (fsm == FFI_SM_GET) return ffi_spec_to_ty(c->ffi_structs[fsi].fields[ffi].spec);
      if (fsm == FFI_SM_SET) return TY_NIL;
      /* ffi_write_*: Module.writer_name(buf, val) returns the written value */
      int wi = ffi_find_writer(c, rcmod, name);
      if (wi >= 0) {
        const char *kind = c->ffi_writers[wi].kind;
        if (kind && sp_streq(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
    }
  }

  /* SomeClass.name / .to_s / .inspect -> class name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_STRING;
  /* SomeClass.superclass -> sp_Class value for the parent class */
  if (recv >= 0 && argc == 0 && sp_streq(name, "superclass") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_CLASS;

  /* SomeClass.ancestors -> PolyArray of class objects */
  if (recv >= 0 && argc == 0 && sp_streq(name, "ancestors") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_POLY_ARRAY;

  /* SomeClass.instance_methods / .public_instance_methods -> PolyArray of symbols */
  if (recv >= 0 && argc <= 1 &&
      (sp_streq(name, "instance_methods") || sp_streq(name, "public_instance_methods") ||
       sp_streq(name, "private_instance_methods") || sp_streq(name, "protected_instance_methods")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    return TY_POLY_ARRAY;

  /* self.class.new(...) -> an instance of the enclosing class */
  if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      sp_streq(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0) return ty_object(self->class_id);
  }

  /* Class#allocate -> a bare instance of that class (no initialize run). */
  if (recv >= 0 && sp_streq(name, "allocate") && argc == 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      /* Use the same exception-subclass predicate as codegen (class_is_exc_subclass)
         so inference and emission agree on which classes take the allocate path. */
      if (ci >= 0 && !class_is_exc_subclass(c, ci)) return ty_object(ci);
      const char *bcn = nt_str(nt, recv, "name");
      if (bcn && sp_streq(bcn, "String")) return TY_STRING;
      if (bcn && sp_streq(bcn, "Array"))  return TY_POLY_ARRAY;
      if (bcn && sp_streq(bcn, "Hash"))   return TY_POLY_POLY_HASH;
      if (bcn && sp_streq(bcn, "Object")) return TY_POLY;
    }
  }

  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && (sp_streq(name, "new") || sp_streq(name, "__hash_new_default"))) {
    const char *rty = nt_type(nt, recv);
    /* a namespaced class (M::Sub) or root-qualified builtin (::Array etc) */
    if (rty && sp_streq(rty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          return (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci));
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        /* a reopened builtin (`class String; def ...`) keeps its builtin
           representation: `String.new` is a String, not a user object (#3109) */
        if (!(cn && is_builtin_reopen(cn))) return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      /* ::Array.new / ::String.new / ::StringIO.new etc. */
      if (cn && sp_streq(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && sp_streq(cn, "Array")) return TY_POLY_ARRAY;
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) return TY_POLY;
      if (cn && sp_streq(cn, "String")) return TY_STRING;
      if (cn && sp_streq(cn, "Hash"))
        return sp_streq(name, "__hash_new_default") ? TY_POLY_POLY_HASH : TY_UNKNOWN;
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
      if (cn && sp_streq(cn, "Fiber")) return TY_FIBER;
      if (cn && sp_streq(cn, "File")) return TY_IO;   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "IO")) return TY_IO;     /* IO.new(fd) is IO.for_fd */
      if (cn && sp_streq(cn, "Dir")) return TY_DIR;   /* Dir.new is an open handle (#2821) */
      /* the socket classes ARE IO handles (#2922) */
      if (cn && (sp_streq(cn, "TCPServer") || sp_streq(cn, "TCPSocket") ||
                 sp_streq(cn, "UDPSocket") || sp_streq(cn, "UNIXSocket") ||
                 sp_streq(cn, "UNIXServer") || sp_streq(cn, "Socket")) &&
          sp_feature_required("socket")) return TY_IO;
      if (cn && sp_streq(cn, "OpenStruct") && sp_feature_required("ostruct")) return TY_OPENSTRUCT;
      /* A Mutex reached through a PATH is the same Mutex. `Thread::Mutex` is
         CRuby's own name for the class, and the bare-constant branch types
         `Mutex.new` TY_MUTEX -- but the path spelling fell into the boxed
         catch-all below, so the local was declared sp_RbVal while the emitter
         still wrote sp_Mutex_new() and the C did not compile (#4421).
         Thread::Queue, Thread::SizedQueue and Thread::ConditionVariable were
         never in that catch-all and have always worked through the path
         spelling, which is what made this one look arbitrary rather than
         missing. */
      if (cn && (sp_streq(cn, "Mutex") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor"))))
        return TY_MUTEX;
      if (cn && (sp_streq(cn, "Thread") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor")) ||
                 sp_streq(cn, "Random") || sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) return TY_POLY;
    }
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "LocalVariableReadNode") ||
                (sp_streq(rty, "CallNode") && is_struct_call(c, recv)))) {  /* inline Data.define(...).new (#2682) */
      const char *cn = sp_streq(rty, "ConstantReadNode") ? nt_str(nt, recv, "name") : NULL;
      int ci = cn ? comp_class_index(c, cn) : class_var_static_ci(c, recv);
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          return (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci));
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        /* a reopened builtin keeps its builtin representation (#3109) */
        if (!(cn && is_builtin_reopen(cn))) return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      if (cn && sp_streq(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && sp_streq(cn, "Array")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* Array.new(n) { body }: element type from last expression of block body */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          if (bn > 0 && bb) {
            TyKind et = infer_type(c, bb[bn - 1]);
            if (et != TY_UNKNOWN) return ty_array_of(et);
            /* Element type unsettled: stay UNKNOWN rather than latch
               POLY_ARRAY. "I do not know what this holds yet" is not "it holds
               anything" -- and the difference is permanent, because a
               POLY_ARRAY binds monotonically into a callee parameter that can
               never un-widen to the INT_ARRAY it really was.

               That matters most where the answer feeds back into what it was
               derived from. An extension-field add is
               `Array.new(4) { |i| Field.add(a[i], b[i]) }`: answering
               POLY_ARRAY on the round before Field.add's own return settles
               makes the caller's accumulator poly, which makes this method's
               own parameters poly, which makes `a[i]` poly -- and the cycle has
               no way back. Waiting one round instead lets the concrete entry
               point (an int-array literal, a zero element) propagate all the
               way round, and the whole field settles on the Integer array.

               The second stage, with g_infer_optimistic cleared, still answers
               POLY_ARRAY for an element that genuinely never settles -- an
               empty `[]`, a heterogeneous block. The narrower rule below is
               what that stage keeps: an index param still being inferred gives
               the element type once it arrives (#3157). */
            if (g_infer_optimistic) return TY_UNKNOWN;
            int bpn = a_proc_params_node(c, id);   /* id is the Array.new call */
            int brn = 0; const int *breqs = bpn >= 0 ? nt_arr(nt, bpn, "requireds", &brn) : NULL;
            if (brn > 0 && breqs) {
              Scope *bsc = comp_scope_of(c, bb[bn - 1]);   /* the block's own scope */
              const char *bpname = nt_str(nt, breqs[0], "name");
              LocalVar *bplv = (bsc && bpname) ? scope_local(bsc, bpname) : NULL;
              if (bplv && bplv->type == TY_UNKNOWN) return TY_UNKNOWN;
            }
          }
        }
        /* a bare `Array.new` carries no element type; leave it UNKNOWN (like an
           empty `[]`) so the push-promotion pass can narrow it from `<<`/push. */
        if (argc == 0 && blk < 0) return TY_UNKNOWN;
        return TY_POLY_ARRAY;
      }
      if (cn && sp_streq(cn, "Array")) return TY_POLY_ARRAY; /* Array.new / Array.new(n) */
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) return TY_POLY;  /* identity sentinel */
      if (cn && sp_streq(cn, "String")) return TY_STRING;
      /* Hash.new { |hash, key| default } : a poly-keyed poly hash with a
         default-proc (the block computes the missing-key value). A default-block
         hash is polymorphic by nature (keys are populated dynamically), so the
         faithful PolyPoly variant boxes each key by value -- inspect then renders
         symbol keys as `a:`, string keys as `"a"=>`, etc., all correctly. */
      if (cn && sp_streq(cn, "Hash") && nt_ref(nt, id, "block") >= 0) return TY_POLY_POLY_HASH;
      if (cn && sp_streq(cn, "Hash")) {
        /* argument-position Hash.new was renamed: PolyPoly; else key usage decides */
        if (sp_streq(name, "__hash_new_default")) return TY_POLY_POLY_HASH;
        /* keys of more than one class are written into it (#3927) */
        if (c->hash_want && id < c->node_cap && c->hash_want[id] == TY_POLY_POLY_HASH)
          return TY_POLY_POLY_HASH;
        /* A Hash.new called on DIRECTLY has no key usage to decide it, and
           staying unknown made every method on it an unresolved call
           ("undefined method 'fetch' for unknown", #3823). The faithful
           variant is the one the argument position already uses. */
        NT_FOREACH_KIND(nt, NK_CallNode, use) {
          if (nt_ref(nt, use, "receiver") == id) return TY_POLY_POLY_HASH;
        }
        /* ...and a Hash.new that is a method's VALUE has no receiver use of
           its own either, so it stayed unknown and the method emitted as
           void: `def mk = Hash.new(0)` answered nothing, and every call on
           the result reported Hash.new itself as undefined (#4291). The
           value position is the same "nothing narrows it" case the receiver
           scan above covers. */
        {
          Scope *hs = comp_scope_of(c, id);
          if (hs && hs->body >= 0) {
            int hbn = 0; const int *hbb = nt_arr(nt, hs->body, "body", &hbn);
            /* ...but only when nothing else settles the method. The CALLERS
               narrow a returned Hash.new -- `h = str_hash; h["k"] = "v"`
               gives Hash[String, String] -- and answering the widest variant
               here overrode that narrowing rather than filling in an unknown,
               re-emitting every such helper as a poly hash (#4304).

               Both halves below are load-bearing, and neither alone fixes it.
               The wait: the callers' narrowing happens during the optimistic
               rounds, so answering there latches the widest variant before
               there is anything to stand aside for. The concrete test: once
               the rounds have settled, a return that reached a real variant
               is not the "nothing narrows it" case, and without this test the
               final round widens it straight back. */
            int narrowed = g_infer_optimistic ||
                           (ty_is_hash(hs->ret) && hs->ret != TY_POLY_POLY_HASH);
            if (hbb && hbn > 0 && hbb[hbn - 1] == id && !narrowed) return TY_POLY_POLY_HASH;
          }
        }
        return TY_UNKNOWN;
      }
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
      /* Builtin object types */
      if (cn && sp_streq(cn, "Fiber")) return TY_FIBER;
      /* Thread.new { block }: an eager green thread (sp_thread) on the scheduler. */
      if (cn && sp_streq(cn, "Thread") && nt_ref(nt, id, "block") >= 0) return TY_THREAD;
      if (cn && (sp_streq(cn, "Queue") || sp_streq(cn, "SizedQueue"))) return TY_QUEUE;
      if (cn && (sp_streq(cn, "Mutex") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor")))) return TY_MUTEX;
      if (cn && sp_streq(cn, "ConditionVariable")) return TY_CONDVAR;
      if (cn && sp_streq(cn, "Random")) return TY_RANDOM;
      if (cn && sp_streq(cn, "File")) return TY_IO;   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "IO")) return TY_IO;     /* IO.new(fd) is IO.for_fd */
      if (cn && sp_streq(cn, "Dir")) return TY_DIR;   /* Dir.new is an open handle (#2821) */
      /* the socket classes ARE IO handles (#2922) */
      if (cn && (sp_streq(cn, "TCPServer") || sp_streq(cn, "TCPSocket") ||
                 sp_streq(cn, "UDPSocket") || sp_streq(cn, "UNIXSocket") ||
                 sp_streq(cn, "UNIXServer") || sp_streq(cn, "Socket")) &&
          sp_feature_required("socket")) return TY_IO;
      if (cn && sp_streq(cn, "OpenStruct") && sp_feature_required("ostruct")) return TY_OPENSTRUCT;
      if (cn && (sp_streq(cn, "Thread") ||
                 sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) return TY_POLY;
    }
  }

  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && sp_streq(name, "compile")) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
    }
  }

  /* StringScanner instance methods */
  /* StringScanner: a native-bound class (packages/strscan); no arms here. */

  /* Regexp class methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if ((sp_streq(name, "escape") || sp_streq(name, "quote")) && argc >= 1) return TY_STRING;
    if (sp_streq(name, "union")) return TY_REGEX;  /* argc 0 = the never-matching /(?!)/ */
    if (sp_streq(name, "last_match") && argc == 0) return TY_MATCHDATA;
    if (sp_streq(name, "last_match") && argc == 1) return TY_STRING;
    if (sp_streq(name, "linear_time?") && argc == 1) return TY_BOOL;
    if (sp_streq(name, "try_convert") && argc == 1) return TY_POLY;
    if (sp_streq(name, "timeout") && argc == 0) return TY_POLY;   /* nil */
    if (sp_streq(name, "timeout") && argc == 1) return TY_POLY;   /* timeout= returns its arg */
    if (sp_streq(name, "timeout=") && argc == 1) return TY_POLY;
  }

  /* Regexp instance methods */
  if (recv >= 0 && rt == TY_REGEX) {
    if (sp_streq(name, "match?") || sp_streq(name, "===")) return TY_BOOL;
    /* the block form evaluates to the block's value (nil on a miss) (#3642) */
    if (sp_streq(name, "match")) return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
    if (sp_streq(name, "=~")) return TY_POLY;
    if (sp_streq(name, "~") && argc == 0) return TY_POLY;   /* ~ /re/ == /re/ =~ $_ */
    if (sp_streq(name, "source") || sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "names")) return TY_STR_ARRAY;
    if (sp_streq(name, "named_captures")) return TY_STR_POLY_HASH;  /* {name => [group indices]} */
    if (sp_streq(name, "freeze") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "itself")) return TY_REGEX;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if ((sp_streq(name, "==") || sp_streq(name, "!=") ||
         sp_streq(name, "equal?") || sp_streq(name, "eql?")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "encoding")) return TY_POLY;  /* a boxed Encoding value */
    if (sp_streq(name, "fixed_encoding?")) return TY_BOOL;
    if (sp_streq(name, "options")) return TY_INT;
    if (sp_streq(name, "casefold?")) return TY_BOOL;
    if (sp_streq(name, "timeout")) return TY_POLY;   /* nil: no per-instance timeout */
  }

  /* MatchData instance methods */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    if (sp_streq(name, "[]") && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_RANGE ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode"))))
      return TY_POLY_ARRAY;   /* md[range] (#2532) */
    if (sp_streq(name, "[]") && argc == 1) return TY_STRING;
    if (sp_streq(name, "[]") && argc == 2) return TY_POLY_ARRAY;   /* md[start, length] (#2507) */
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) return TY_BOOL;   /* (#2529) */
    if (sp_streq(name, "inspect") && argc == 0) return TY_STRING;   /* (#2500) */
    if (sp_streq(name, "match") && argc == 1) return TY_STRING;   /* group substring (#2501) */
    if (sp_streq(name, "match_length") && argc == 1) return TY_POLY;   /* int or nil (#2501) */
    if (sp_streq(name, "deconstruct") && argc == 0) return TY_POLY_ARRAY;   /* (#2503) */
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_SYM_POLY_HASH;   /* (#2503) */
    if (sp_streq(name, "named_captures") && argc == 1) {
      /* symbolize_names: false asks for the string keys (#3640) */
      int kv = kwh_lookup(nt, argv[0], "symbolize_names");
      const char *kvt = kv >= 0 ? nt_type(nt, kv) : NULL;
      if (kvt && sp_streq(kvt, "FalseNode")) return TY_STR_POLY_HASH;
      return TY_SYM_POLY_HASH;   /* symbolize (#2530) */
    }
    if (sp_streq(name, "regexp") && argc == 0) return TY_REGEX;   /* (#2499) */
    if (sp_streq(name, "pre_match") || sp_streq(name, "post_match") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "begin") || sp_streq(name, "end") || sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
    if (sp_streq(name, "bytebegin") || sp_streq(name, "byteend")) return TY_INT;
    if (sp_streq(name, "offset") || sp_streq(name, "byteoffset")) return TY_INT_ARRAY;
    if (sp_streq(name, "values_at")) return TY_POLY_ARRAY;
    if (sp_streq(name, "captures") || sp_streq(name, "to_a")) return TY_POLY_ARRAY;
    if (sp_streq(name, "named_captures")) return TY_STR_POLY_HASH;  /* {String => String|nil} */
    if (sp_streq(name, "names")) return TY_STR_ARRAY;
    if (sp_streq(name, "string")) return TY_STRING;  /* the match subject */
    if (sp_streq(name, "nil?")) return TY_BOOL;
  }

  /* StringIO: a native-bound class (packages/stringio); no arms here. .new
     resolves through the class table, .open is Ruby in the package, and
     instance methods use the native_method declarations. */

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Time") &&
        (sp_streq(name, "now") || sp_streq(name, "at") || sp_streq(name, "local") ||
         sp_streq(name, "mktime") || sp_streq(name, "utc") || sp_streq(name, "gm") ||
         sp_streq(name, "new")))
      return TY_TIME;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        (sp_streq(name, "start") || sp_streq(name, "compact")))
      return TY_NIL;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        sp_streq(name, "stat"))
      return TY_STR_INT_HASH;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Process")) {
      if (sp_streq(name, "times") && argc == 0) return TY_TMS;
      if (sp_streq(name, "pid") || sp_streq(name, "ppid") ||
          sp_streq(name, "uid") || sp_streq(name, "gid") ||
          sp_streq(name, "euid") || sp_streq(name, "egid") ||
          sp_streq(name, "getsid") || sp_streq(name, "getpgrp") ||
          (sp_streq(name, "getpriority") && argc == 2)) return TY_INT;
      if (sp_streq(name, "groups") && argc == 0) return TY_INT_ARRAY;
      if (sp_streq(name, "clock_gettime") || sp_streq(name, "clock_getres")) {
        /* an integer unit (:nanosecond/:microsecond/:millisecond/:second) makes
           the result an Integer; the default and float units keep it Float. */
        if (argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode")) {
          const char *u = nt_str(nt, argv[1], "value");
          if (u && (sp_streq(u, "nanosecond") || sp_streq(u, "microsecond") ||
                    sp_streq(u, "millisecond") || sp_streq(u, "second"))) return TY_INT;
        }
        return TY_FLOAT;
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
        sp_streq(name, "sqrt"))
      return TY_INT;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Marshal")) {
      if (sp_streq(name, "dump") && argc == 1) return TY_STRING;
      /* Marshal.dump(obj, io) writes the bytes to io and answers io (#4112) */
      if (sp_streq(name, "dump") && argc == 2) return TY_IO;
      if (sp_streq(name, "load") && argc == 1) return TY_POLY;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "sin") || sp_streq(name, "cos") || sp_streq(name, "tan") ||
         sp_streq(name, "asin") || sp_streq(name, "acos") || sp_streq(name, "atan") ||
         sp_streq(name, "atan2") || sp_streq(name, "sinh") || sp_streq(name, "cosh") ||
         sp_streq(name, "tanh") || sp_streq(name, "asinh") || sp_streq(name, "acosh") ||
         sp_streq(name, "atanh") || sp_streq(name, "exp") || sp_streq(name, "log") ||
         sp_streq(name, "log2") || sp_streq(name, "log10") || sp_streq(name, "sqrt") ||
         sp_streq(name, "cbrt") || sp_streq(name, "hypot") ||
         sp_streq(name, "expm1") || sp_streq(name, "log1p") ||
         sp_streq(name, "ldexp") || sp_streq(name, "erf") || sp_streq(name, "erfc") ||
         sp_streq(name, "gamma")))
      return TY_FLOAT;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "lgamma") || sp_streq(name, "frexp")) && argc == 1)
      return TY_POLY_ARRAY;  /* [log(|gamma|), sign] / [fraction, exponent] */
    /* JSON.generate/dump return type comes from the native binding
       (packages/json, inferred in the FFI/native block above), not a hardcoded
       arm. */
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
        (sp_streq(name, "exist?") || sp_streq(name, "exists?")))
      return TY_BOOL;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
      if (sp_streq(name, "pwd") || sp_streq(name, "home")) return TY_STRING;
      if (sp_streq(name, "glob") || sp_streq(name, "entries") || sp_streq(name, "children")) return TY_STR_ARRAY;
      if (sp_streq(name, "mkdir") || sp_streq(name, "rmdir") || sp_streq(name, "chdir"))
        return TY_INT;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") &&
        (sp_streq(nt_str(nt, recv, "name"), "File") ||
         sp_streq(nt_str(nt, recv, "name"), "FileTest"))) {
      if (sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname") ||
          sp_streq(name, "read") || sp_streq(name, "binread") || sp_streq(name, "expand_path") ||
          sp_streq(name, "join") || sp_streq(name, "realpath") ||
          sp_streq(name, "realdirpath") || sp_streq(name, "ftype") ||
          sp_streq(name, "path") || sp_streq(name, "absolute_path"))
        return TY_STRING;
      if (sp_streq(name, "exist?") || sp_streq(name, "exists?"))
        return TY_BOOL;
      if (sp_streq(name, "write") || sp_streq(name, "binwrite") || sp_streq(name, "delete") ||
          sp_streq(name, "unlink") || sp_streq(name, "rename") || sp_streq(name, "size") ||
          sp_streq(name, "size?") || sp_streq(name, "chmod") || sp_streq(name, "truncate") ||
          sp_streq(name, "chown") || sp_streq(name, "symlink") || sp_streq(name, "link") ||
          sp_streq(name, "mkfifo") || sp_streq(name, "umask") || sp_streq(name, "utime") ||
          sp_streq(name, "world_readable?") || sp_streq(name, "world_writable?"))
        return TY_INT;   /* world_*? are nullable int (bits or nil) */
      if (sp_streq(name, "readlink")) return TY_STRING;
      if (sp_streq(name, "absolute_path?")) return TY_BOOL;
      if (sp_streq(name, "readable?") || sp_streq(name, "directory?") || sp_streq(name, "file?") ||
          sp_streq(name, "zero?") || sp_streq(name, "empty?") || sp_streq(name, "symlink?") ||
          sp_streq(name, "writable?") || sp_streq(name, "executable?") || sp_streq(name, "pipe?") ||
          sp_streq(name, "readable_real?") || sp_streq(name, "writable_real?") ||
          sp_streq(name, "executable_real?") ||
          sp_streq(name, "identical?") || sp_streq(name, "fnmatch") || sp_streq(name, "fnmatch?") ||
          sp_streq(name, "owned?") || sp_streq(name, "grpowned?") || sp_streq(name, "setuid?") ||
          sp_streq(name, "setgid?") || sp_streq(name, "sticky?") || sp_streq(name, "socket?") ||
          sp_streq(name, "blockdev?") || sp_streq(name, "chardev?"))
        return TY_BOOL;
      if (sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ctime") ||
          sp_streq(name, "birthtime"))
        return TY_TIME;
      if (sp_streq(name, "readlines") || sp_streq(name, "split")) return TY_STR_ARRAY;
      if (sp_streq(name, "stat") || sp_streq(name, "lstat")) return TY_IO;   /* the path-carrying stat handle */
      /* File.open / File.new without a block -> a typed IO handle */
      if (sp_streq(name, "open") || sp_streq(name, "new")) {
        int blk = nt_ref(nt, id, "block");
        if (blk < 0) return TY_IO;
        /* Pin block param to TY_IO so body dispatch works (f.write, f.puts, etc.) */
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_IO;
        return TY_POLY;
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Addrinfo") && sp_feature_required("socket")) {
      if (((sp_streq(name, "tcp") || sp_streq(name, "udp")) && argc == 2) ||
          ((sp_streq(name, "ip") || sp_streq(name, "unix")) && argc == 1))
        return TY_ADDRINFO;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Socket") && sp_feature_required("socket")) {
      if (sp_streq(name, "gethostname") && argc == 0) return TY_STRING;
      if ((sp_streq(name, "pair") || sp_streq(name, "socketpair")) && argc >= 2) return TY_POLY_ARRAY;
      if (sp_streq(name, "getaddrinfo") && argc >= 2) return TY_POLY_ARRAY;
      /* The packed sockaddr is a byte String (it carries NUL), and
         unpack answers [port, host] (#4137). */
      if ((sp_streq(name, "sockaddr_in") || sp_streq(name, "pack_sockaddr_in")) && argc == 2) return TY_STRING;
      if ((sp_streq(name, "sockaddr_un") || sp_streq(name, "pack_sockaddr_un")) && argc == 1) return TY_STRING;
      if (sp_streq(name, "unpack_sockaddr_in") && argc == 1) return TY_POLY_ARRAY;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "IO")) {
      /* IO.pipe -> [reader, writer], boxed IO handles (#2815) */
      if (sp_streq(name, "pipe")) return TY_POLY_ARRAY;
      if (sp_streq(name, "copy_stream") || sp_streq(name, "sysopen")) return TY_INT;
      /* IO.new(fd, ...) is the descriptor form, same as IO.for_fd */
      if ((sp_streq(name, "for_fd") || sp_streq(name, "new")) && argc >= 1) return TY_IO;
      /* [ready_read, ready_write, ready_error] or nil on timeout */
      if (sp_streq(name, "select") && argc >= 1) return TY_POLY;
    }

    /* <local>.yield(v) or bare <local>.yield: a generator yielder / fiber yield
       returns the value the next resume (or Enumerator#feed) supplies -- poly.
       Gated on a local receiver so Fiber.yield (const receiver) and the yield
       keyword are untouched; without this the return is typed nil and
       `x = y.yield` drops the fed value. Zero-arg `y.yield` is valid too. */
    if (recv >= 0 && sp_streq(name, "yield") &&
        rty && sp_streq(rty, "LocalVariableReadNode"))
      return TY_POLY;
    /* Fiber.new {} / Thread.new {} / Fiber.current etc.
       Handles both bare Const and ::Const path forms. */
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *cn2 = nt_str(nt, recv, "name");
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Enumerator") &&
          nt_ref(nt, id, "block") >= 0) return TY_ENUMERATOR;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Fiber")) return TY_FIBER;
      /* Thread.new { block }: an eager green thread on the scheduler. */
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Thread") &&
          nt_ref(nt, id, "block") >= 0)
        return TY_THREAD;
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Queue") || sp_streq(cn2, "SizedQueue"))) return TY_QUEUE;
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Mutex") || (sp_streq(cn2, "Monitor") && sp_feature_enabled("monitor")))) return TY_MUTEX;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "ConditionVariable")) return TY_CONDVAR;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Random")) return TY_RANDOM;
      if (cn2 && sp_streq(cn2, "Enumerator") && sp_streq(name, "product") && (argc == 2 || argc == 3))
        return TY_ENUMERATOR;   /* #2484 */
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "current")) return TY_THREAD;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "main")) return TY_THREAD;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "list")) return TY_POLY_ARRAY;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "pass")) return TY_NIL;
      if (cn2 && sp_streq(cn2, "Thread") &&
          (sp_streq(name, "report_on_exception") || sp_streq(name, "report_on_exception="))) return TY_BOOL;
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "current")) return TY_FIBER;
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "yield")) return TY_POLY;
      /* Random class methods: Random.rand(float)->float / Random.rand(int)->int
         / Random.rand->float */
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "rand")) {
        if (argc < 1) return TY_FLOAT;
        TyKind rr0 = infer_type(c, argv[0]);
        return rr0 == TY_FLOAT ? TY_FLOAT : rr0 == TY_BIGINT ? TY_BIGINT : TY_INT;
      }
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "bytes")) return TY_STRING;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "new_seed")) return TY_INT;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "urandom")) return TY_STRING;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "srand")) return TY_INT;
    }
  }

  /* TY_FIBER instance methods */
  if (recv >= 0 && rt == TY_FIBER) {
    if (sp_streq(name, "resume") || sp_streq(name, "transfer") || sp_streq(name, "raise")) return TY_POLY;
    if (sp_streq(name, "alive?")) return TY_BOOL;
    if (sp_streq(name, "value")) return TY_POLY;
    if (sp_streq(name, "kill")) return TY_FIBER;   /* returns the receiver */
  }

  /* Object's identity protocol on the native kinds: typed from the same
     decision the codegen arm (emit_native_object_protocol) emits from, so the
     two cannot drift. A user exception subclass's own definition wins, as it
     does in the arm. */
  if (recv >= 0 && nt_ref(c->nt, id, "block") < 0 &&
      ty_object_protocol_answers(rt, argc == 1 ? infer_type(c, argv[0]) : TY_UNKNOWN, name, argc) &&
      !(rt == TY_EXCEPTION && exc_subclass_defines(c, name))) {
    if (sp_streq(name, "freeze")) return rt;
    if (sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "<=>")) return TY_POLY;   /* 0 or nil, boxed */
    return TY_BOOL;
  }

  /* universal query methods on the concurrency handles (#3124) */
  if (recv >= 0 && (rt == TY_THREAD || rt == TY_QUEUE || rt == TY_MUTEX ||
                    rt == TY_CONDVAR) && argc == 0) {
    if (sp_streq(name, "class")) return TY_CLASS;
    if (sp_streq(name, "frozen?") || sp_streq(name, "nil?")) return TY_BOOL;
    if (sp_streq(name, "itself")) return rt;
  }
  /* TY_THREAD instance methods */
  if (recv >= 0 && rt == TY_THREAD) {
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) return TY_STRING;
    if (sp_streq(name, "value")) return TY_POLY;
    if (sp_streq(name, "join") || sp_streq(name, "kill") || sp_streq(name, "exit") ||
        sp_streq(name, "terminate") || sp_streq(name, "raise")) return TY_THREAD;   /* return self */
    if (sp_streq(name, "alive?")) return TY_BOOL;
    if (sp_streq(name, "report_on_exception") || sp_streq(name, "report_on_exception=")) return TY_BOOL;
    if (sp_streq(name, "status") || sp_streq(name, "[]") || sp_streq(name, "[]=") ||
        sp_streq(name, "name") || sp_streq(name, "name=")) return TY_POLY;
    if (sp_streq(name, "key?") || sp_streq(name, "equal?")) return TY_BOOL;
  }

  /* Array#push / #append / #unshift / #prepend answer the RECEIVER, and on a
     boxed receiver that is the poly value itself. Leaving the call untyped made
     the poly dispatch declare an sp_int result temp whose push arm (which only
     assigns for a poly result) never wrote it, so the answer was the temp's
     zero seed: `->(acc) { acc.push(1) }` answered nil (#4320). Poly is also the
     right answer when a user class owns the name -- it is the union of that
     return and the builtin one, which is what every other name on this surface
     already widens to. */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 &&
      (sp_streq(name, "push") || sp_streq(name, "append") ||
       sp_streq(name, "unshift") || sp_streq(name, "prepend")))
    return TY_POLY;

  /* Array#pop(n) / #shift(n) on a boxed array answer an Array of the removed
     elements (#3613) */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (sp_streq(name, "pop") || sp_streq(name, "shift")))
    return TY_POLY_ARRAY;

  /* TY_QUEUE instance methods */
  if (recv >= 0 && rt == TY_QUEUE) {
    if (sp_streq(name, "pop") || sp_streq(name, "shift") || sp_streq(name, "deq")) return TY_POLY;
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "enq") ||
        sp_streq(name, "close") || sp_streq(name, "clear")) return TY_QUEUE;   /* return self */
    if (sp_streq(name, "size") || sp_streq(name, "length") || sp_streq(name, "max")) return TY_INT;
    if (sp_streq(name, "empty?") || sp_streq(name, "closed?")) return TY_BOOL;
  }

  /* TY_MUTEX instance methods */
  if (recv >= 0 && rt == TY_MUTEX) {
    if (sp_streq(name, "lock") || sp_streq(name, "unlock")) return TY_MUTEX;   /* return self */
    if (sp_streq(name, "try_lock") || sp_streq(name, "locked?") || sp_streq(name, "owned?")) return TY_BOOL;
    if (sp_streq(name, "synchronize")) return TY_POLY;   /* the block's result */
  }

  /* TY_CONDVAR instance methods */
  if (recv >= 0 && rt == TY_CONDVAR) {
    if (sp_streq(name, "wait") || sp_streq(name, "signal") || sp_streq(name, "broadcast")) return TY_CONDVAR;
  }

  /* Process::Tms accessors: four cumulative CPU times, all Float (#3044) */
  if (recv >= 0 && rt == TY_TMS && argc == 0 &&
      (sp_streq(name, "utime") || sp_streq(name, "stime") ||
       sp_streq(name, "cutime") || sp_streq(name, "cstime"))) return TY_FLOAT;
  /* Process::Status accessors. The runtime returns sp_int (with -1 for
     the nil-or-false slot on exitstatus/termsig); the analyze pass keeps
     Integer here and the codegen wraps -1 in sp_box_nil for those two.
     Boolean predicates are TY_BOOL; the accessors that can be nil are
     TY_INT (so `result[1].termsig.nil?` is well-typed). */
  if (recv >= 0 && rt == TY_PROCESS_STATUS && argc == 0) {
    if (sp_streq(name, "signaled?") || sp_streq(name, "exited?") ||
        sp_streq(name, "coredump?"))
      return TY_BOOL;
    /* success? is nil, not false, when the process did not exit normally */
    if (sp_streq(name, "success?")) return TY_POLY;
    if (sp_streq(name, "exitstatus") || sp_streq(name, "termsig") ||
        sp_streq(name, "pid"))
      return TY_INT;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect") ||
        sp_streq(name, "class")) return TY_STRING;
    if (sp_streq(name, "==")) return TY_BOOL;
  }
  /* The same names on a BOXED status -- which is how one normally arrives,
     since waitpid2 answers an Array and its second element is read out of a
     poly container. emit_poly_builtin_method already emits the unboxed scalar
     for exactly these seven (behind a runtime cls_id check), so without the
     matching rule here the two sides disagreed: `"exit #{st.exitstatus}"`
     asked sp_poly_to_s for a poly the emitter had produced as an sp_int. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      !an_user_defines_or_reads(c, name)) {
    if (sp_streq(name, "signaled?") || sp_streq(name, "exited?") ||
        sp_streq(name, "coredump?"))
      return TY_BOOL;
    if (sp_streq(name, "success?")) return TY_POLY;
    if (sp_streq(name, "exitstatus") || sp_streq(name, "termsig") ||
        sp_streq(name, "pid"))
      return TY_INT;
  }
  /* OpenStruct: dynamic members. A member read (any name, arg-less, no
     writer) or `[sym]` returns a boxed value; a writer / `[]=` returns the
     assigned value; the rest is a small fixed surface (#3135). */
  if (recv >= 0 && rt == TY_OPENSTRUCT) {
    if (sp_streq(name, "to_h") && argc == 0) return TY_SYM_POLY_HASH;
    if (sp_streq(name, "respond_to?")) return TY_BOOL;
    if (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?")) return TY_BOOL;
    if ((sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "!=")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "class") && argc == 0) return TY_CLASS;
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if ((sp_streq(name, "[]=") || sp_streq(name, "[]")) ) return TY_POLY;
    if (sp_streq(name, "frozen?") || sp_streq(name, "nil?")) return TY_BOOL;
    if (sp_streq(name, "dig")) return TY_POLY;
    if (sp_streq(name, "each_pair") || sp_streq(name, "freeze") ||
        sp_streq(name, "itself")) return TY_OPENSTRUCT;
    /* any other arg-less name (or a `name=` writer) is a dynamic member */
    return TY_POLY;
  }
  /* TY_ENUMERATOR instance methods */
  if (recv >= 0 && rt == TY_ENUMERATOR) {
    if (sp_streq(name, "next") || sp_streq(name, "peek")) return TY_POLY;
    /* find/detect with a block: driven lazily via #next (works on infinite
       generator enums like blockless Kernel#loop); nil on no match (#3236) */
    if ((sp_streq(name, "find") || sp_streq(name, "detect")) &&
        nt_ref(nt, id, "block") >= 0) return TY_POLY;
    /* take_while rides the same lazy driver and collects the prefix (#3590) */
    if (sp_streq(name, "take_while") && nt_ref(nt, id, "block") >= 0) return TY_POLY_ARRAY;
    /* include?/member? scan through the driver and stop at the first hit */
    if ((sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) return TY_BOOL;
    if (sp_streq(name, "next_values") || sp_streq(name, "peek_values")) return TY_POLY_ARRAY;   /* #2482 */
    if (sp_streq(name, "+") && argc == 1 && infer_type(c, argv[0]) == TY_ENUMERATOR) return TY_ENUMERATOR;  /* #2481 */
    if (sp_streq(name, "rewind")) return TY_ENUMERATOR;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if ((sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "freeze") || sp_streq(name, "itself")) return TY_ENUMERATOR;
    if (sp_streq(name, "feed") && argc == 1) return TY_NIL;   /* #feed returns nil */
    /* blockless enum.with_index(off) is another materialized Enumerator (over
       [element, index] pairs); the block/terminal-chain forms are typed below */
    if (sp_streq(name, "with_index") && argc <= 1 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    /* blockless enum.each_with_index / each_index -> a chained Enumerator (#2487) */
    if ((sp_streq(name, "each_with_index") || sp_streq(name, "each_index")) &&
        argc == 0 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    /* Stored-enumerator block form returns the underlying each return (the
       boxed source). Immediate chains (arr.each.with_index { } and the
       map/select shapes) keep their own typed arms below -- skip a blockless
       iter-shaped CallNode receiver over an array. */
    if (sp_streq(name, "with_index") && argc <= 1 && nt_ref(nt, id, "block") >= 0) {
      int wchain = 0;
      if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
          nt_ref(nt, recv, "block") < 0) {
        const char *wnm = nt_str(nt, recv, "name");
        int wrcv = nt_ref(nt, recv, "receiver");
        if (wnm && wrcv >= 0 &&
            (sp_streq(wnm, "each") || ty_iter_shape(wnm) != TY_ITER_NONE) &&
            ty_is_array(infer_type(c, wrcv)))
          wchain = 1;
      }
      if (!wchain) return TY_POLY;
    }
    /* #size is nil for a generator with no size, an Integer for a materialized
       snapshot, or whatever a stored size value/callable yields -- hence poly. */
    if (sp_streq(name, "size")) return TY_POLY;
    if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "drop") && argc == 1 && nt_ref(nt, id, "block") < 0) return TY_POLY_ARRAY;
    /* reject/select/filter/map with a block over the materialized pairs: a
       generic Array (each_with_index.reject { |v, i| ... }, each_index.map { }). */
    if ((sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "map") || sp_streq(name, "collect") || sp_streq(name, "flat_map") ||
         sp_streq(name, "filter_map")) &&
        argc == 0 && nt_ref(nt, id, "block") >= 0) return TY_POLY_ARRAY;
    /* block forms over the materialized pairs: sort_by is a reordered Array;
       max_by/min_by pick one pair (a boxed element); sum { } folds to a poly. */
    if (sp_streq(name, "sort_by") && argc == 0 && nt_ref(nt, id, "block") >= 0) return TY_POLY_ARRAY;
    if ((sp_streq(name, "max_by") || sp_streq(name, "min_by") || sp_streq(name, "sum")) &&
        argc == 0 && nt_ref(nt, id, "block") >= 0) return TY_POLY;
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) return TY_POLY_ARRAY;
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) return TY_STRING;
  }

  /* Kernel#p returns its argument (one arg; several return the array), so it
     composes as an expression: x = p(y), f(p(y)). Statement-position p keeps
     its own emitter; this types the value form. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc >= 2)
    return TY_POLY_ARRAY;   /* p(a, b, ...) returns the array of its arguments */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc == 1)
    return infer_type(c, argv[0]);
  /* Object#instance_variables: a static symbol list for a typed object */
  if (recv >= 0 && ty_is_object(rt) && argc == 0 &&
      sp_streq(name, "instance_variables"))
    return TY_POLY_ARRAY;

  /* Kernel#puts / #print return nil; typed so the value form composes. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "puts") || sp_streq(name, "print")) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#warn / #printf / p() (no args) return nil in value position. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && (sp_streq(name, "warn") || sp_streq(name, "printf") ||
                   ((sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 0)) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#putc returns its argument. */
  if (recv < 0 && !an_bare_call_class_owned(c, id) && sp_streq(name, "putc") && argc == 1 && nt_ref(nt, id, "block") < 0)
    return infer_type(c, argv[0]);

  /* TY_RANDOM instance methods */
  if (recv >= 0 && rt == TY_RANDOM) {
    if (sp_streq(name, "rand")) {
      if (argc < 1) return TY_FLOAT;
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_FLOAT || a0 == TY_FLOAT_RANGE) return TY_FLOAT;   /* rand(Float range) -> Float (#2521) */
      if (a0 == TY_BIGINT) return TY_BIGINT;   /* rand(Bignum bound) -> Bigint (#3058) */
      /* rand(Float range) -> Float (#2521) */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && sp_streq(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;
      }
      return TY_INT;
    }
    if (sp_streq(name, "bytes")) return TY_STRING;
    if (sp_streq(name, "seed")) return TY_INT;
    if (sp_streq(name, "class")) return TY_CLASS;
    if ((sp_streq(name, "==") || sp_streq(name, "equal?") || sp_streq(name, "eql?")) && argc == 1)
      return TY_BOOL;
  }

  /* ARGF pseudo-IO methods */
  if (recv >= 0 && rt == TY_ARGF) {
    if (sp_streq(name, "read") || sp_streq(name, "gets") || sp_streq(name, "readline") ||
        sp_streq(name, "filename") || sp_streq(name, "path") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "readlines") || sp_streq(name, "to_a")) return TY_STR_ARRAY;
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) return TY_BOOL;
    if (sp_streq(name, "each_line") || sp_streq(name, "each_string") || sp_streq(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      return TY_ARGF;
    }
  }

  /* Dir.new / Dir.open and the Dir handle instance surface (#2821) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (sp_streq(name, "new") || sp_streq(name, "open")) && argc >= 1) {
    int dblk = nt_ref(nt, id, "block");
    if (dblk < 0) return TY_DIR;
    const char *dp0 = block_param_name(c, dblk, 0);
    Scope *dbs = dp0 ? comp_scope_of(c, dblk) : NULL;
    LocalVar *dlv = (dbs && dp0) ? scope_local(dbs, dp0) : NULL;
    if (dlv) dlv->type = TY_DIR;
    return TY_POLY;   /* block form: the block's boxed value (File.open's rule) */
  }
  if (recv >= 0 && rt == TY_DIR) {
    if (sp_streq(name, "class")) return TY_CLASS;
    if (sp_streq(name, "read") || sp_streq(name, "path") || sp_streq(name, "to_path"))
      return TY_STRING;
    if (sp_streq(name, "children") || sp_streq(name, "entries")) return TY_STR_ARRAY;
    if (sp_streq(name, "tell") || sp_streq(name, "pos")) return TY_INT;
    if (sp_streq(name, "fileno")) return TY_INT;      /* dirfd (#2967) */
    if (sp_streq(name, "pos=")) return TY_INT;        /* -> assigned value (#2968) */
    if (sp_streq(name, "close")) return TY_POLY;   /* nil */
    if (sp_streq(name, "rewind") || sp_streq(name, "seek")) return TY_DIR;
    /* Enumerable#each_entry yields what #each yields (dots included) and
       answers the receiver, so on a Dir it IS #each (#3395). */
    if (sp_streq(name, "each") || sp_streq(name, "each_child") ||
        sp_streq(name, "each_entry")) {
      int dblk3 = nt_ref(nt, id, "block");
      if (dblk3 >= 0) {
        const char *dbp3 = block_param_name(c, dblk3, 0);
        Scope *dbs3 = dbp3 ? comp_scope_of(c, dblk3) : NULL;
        LocalVar *dlv3 = (dbs3 && dbp3) ? scope_local(dbs3, dbp3) : NULL;
        if (dlv3) dlv3->type = TY_STRING;
      }
      return TY_DIR;
    }
  }

  /* TY_IO (File/IO handle) instance methods */
  if (recv >= 0 && rt == TY_SOCKOPT && argc == 0) {
    if (sp_streq(name, "int") || sp_streq(name, "level") ||
        sp_streq(name, "optname") || sp_streq(name, "family")) return TY_INT;
    if (sp_streq(name, "bool")) return TY_BOOL;
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "class")) return TY_CLASS;
  }
  if (recv >= 0 && rt == TY_ADDRINFO && argc == 0) {
    if (sp_streq(name, "ip_address") || sp_streq(name, "unix_path") ||
        sp_streq(name, "afamily_name") || sp_streq(name, "to_sockaddr") ||
        sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "ip_port") || sp_streq(name, "socktype") ||
        sp_streq(name, "protocol") ||
        sp_streq(name, "afamily") || sp_streq(name, "pfamily")) return TY_INT;
    if (sp_streq(name, "class")) return TY_CLASS;
    if (sp_streq(name, "ipv4?") || sp_streq(name, "ipv6?") ||
        sp_streq(name, "unix?") || sp_streq(name, "ip?")) return TY_BOOL;
  }
  if (recv >= 0 && rt == TY_IO) {
    if (sp_streq(name, "read") || sp_streq(name, "gets") || sp_streq(name, "readline") ||
        sp_streq(name, "path") || sp_streq(name, "to_path")) return TY_STRING;
    if (sp_streq(name, "read") && nt_ref(nt, id, "arguments") >= 0) return TY_STRING;
    if (sp_streq(name, "readlines")) return TY_STR_ARRAY;
    if (sp_streq(name, "write") || sp_streq(name, "syswrite") || sp_streq(name, "pos") ||
        sp_streq(name, "tell") || sp_streq(name, "seek") || sp_streq(name, "rewind"))
      return TY_INT;
    if (sp_streq(name, "close")) return TY_POLY;      /* nil (#2801) */
    if (sp_streq(name, "print") || sp_streq(name, "puts")) return TY_NIL;
    if (sp_streq(name, "flush") || sp_streq(name, "binmode")) return TY_IO;  /* self (#2799) */
    if (sp_streq(name, "closed?") || sp_streq(name, "eof?") || sp_streq(name, "eof") ||
        sp_streq(name, "tty?") || sp_streq(name, "isatty") ||
        sp_streq(name, "sync") || sp_streq(name, "sync=") ||
        sp_streq(name, "autoclose?") ||
        /* the File::Stat predicates: a stat is carried as the handle itself */
        sp_streq(name, "file?") || sp_streq(name, "directory?") ||
        sp_streq(name, "symlink?") || sp_streq(name, "owned?") ||
        sp_streq(name, "grpowned?") || sp_streq(name, "setuid?") ||
        sp_streq(name, "setgid?") || sp_streq(name, "sticky?") ||
        sp_streq(name, "socket?") ||
        sp_streq(name, "==") || sp_streq(name, "equal?") || sp_streq(name, "eql?"))
      return TY_BOOL;
    if (sp_streq(name, "fileno") || sp_streq(name, "to_i") || sp_streq(name, "lineno") ||
        sp_streq(name, "lineno=") || sp_streq(name, "pos=") || sp_streq(name, "flock") ||
        sp_streq(name, "fsync") || sp_streq(name, "fdatasync") || sp_streq(name, "getbyte") ||
        (sp_streq(name, "chown") && argc == 2) ||   /* (#3104) */
        sp_streq(name, "sysseek") || sp_streq(name, "size") || sp_streq(name, "chmod") ||
        sp_streq(name, "mode"))
      return TY_INT;
    /* File::Stat's numeric fields and its mode predicates (#3765). size? is an
       int-or-nil (the sentinel), so it stays TY_INT like the other counts. */
    if (argc == 0 &&
        (sp_streq(name, "uid") || sp_streq(name, "gid") || sp_streq(name, "nlink") ||
         sp_streq(name, "dev") || sp_streq(name, "ino") || sp_streq(name, "blksize") ||
         sp_streq(name, "blocks") || sp_streq(name, "rdev") || sp_streq(name, "size?")))
      return TY_INT;
    if (argc == 0 &&
        (sp_streq(name, "pipe?") || sp_streq(name, "zero?") || sp_streq(name, "readable?") ||
         sp_streq(name, "writable?") || sp_streq(name, "executable?") ||
         sp_streq(name, "blockdev?") || sp_streq(name, "chardev?")))
      return TY_BOOL;
    if (sp_streq(name, "getc") || sp_streq(name, "readchar") || sp_streq(name, "readpartial") ||
        sp_streq(name, "sysread") || sp_streq(name, "ftype")) return TY_STRING;
    if (sp_streq(name, "inspect") && argc == 0) return TY_STRING;
    if (sp_streq(name, "nil?") && argc == 0) return TY_BOOL;
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                      sp_streq(name, "instance_of?"))) return TY_BOOL;
    /* Object's hash and object_id on the handle: the generic arms already
       emit them (the pointer hash, the pointer), but the catch-all at the end
       of this block typed the slot poly, so the C they produced refused to
       compile. respond_to? stays untyped on purpose: the compile-time fold
       reads the analyze-time probe, which that same catch-all answers for
       every name, so a typed slot would turn today's refusal into a wrong
       `true` for `f.respond_to?(:nope)`. */
    if (argc == 0 && (sp_streq(name, "hash") || sp_streq(name, "object_id") ||
                      sp_streq(name, "__id__"))) return TY_INT;
    /* the readiness family answers the handle itself or nil -- a nullable
       sp_File*, which TY_IO already models (NULL is nil) */
    if (sp_streq(name, "wait_readable") || sp_streq(name, "wait_writable") ||
        sp_streq(name, "wait_priority") || sp_streq(name, "wait"))
      return TY_IO;
    /* socket methods on the IO handle (#2922) */
    if (sp_feature_required("socket")) {
      if (sp_streq(name, "accept") && argc == 0) return TY_IO;
      if ((sp_streq(name, "addr") || sp_streq(name, "peeraddr")) && argc == 0)
        return TY_POLY_ARRAY;
      if ((sp_streq(name, "local_address") || sp_streq(name, "remote_address")) && argc == 0)
        return TY_ADDRINFO;
      /* the non-blocking family: the handle / the bytes / the byte count, each
         nullable so `exception: false` can answer nil */
      if (sp_streq(name, "accept_nonblock") || sp_streq(name, "recv_nonblock") ||
          sp_streq(name, "connect_nonblock"))
        return an_nonblock_no_exception(c, id)
               ? TY_POLY                                  /* :wait_* or the value */
               : sp_streq(name, "accept_nonblock") ? TY_IO
               : sp_streq(name, "recv_nonblock") ? TY_STRING : TY_INT;
      if (sp_streq(name, "recv") && argc == 1) return TY_STRING;
      if (sp_streq(name, "recvfrom") && argc == 1) return TY_POLY_ARRAY;
      if (((sp_streq(name, "bind") || sp_streq(name, "connect")) && argc == 2) ||
          (sp_streq(name, "send") && (argc == 2 || argc == 4)) ||
          (sp_streq(name, "shutdown") && argc <= 1) ||
          (sp_streq(name, "listen") && argc == 1) ||
          (sp_streq(name, "setsockopt") && argc == 3))
        return TY_INT;
      if (sp_streq(name, "getsockopt") && argc == 2) return TY_SOCKOPT;
    }
    /* fd-backed IO instance methods (#3038) */
    if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) &&
        an_nonblock_no_exception(c, id))
      return TY_POLY;
    if (sp_streq(name, "readbyte") || sp_streq(name, "fcntl") ||
        sp_streq(name, "pwrite") || sp_streq(name, "write_nonblock")) return TY_INT;
    if (sp_streq(name, "pread") || sp_streq(name, "read_nonblock")) return TY_STRING;
    if (sp_streq(name, "binmode?") || sp_streq(name, "close_on_exec?") ||
        sp_streq(name, "close_on_exec=") || sp_streq(name, "autoclose=")) return TY_BOOL;
    if (sp_streq(name, "to_io") || sp_streq(name, "reopen")) return TY_IO;
    if (sp_streq(name, "ungetbyte") || sp_streq(name, "advise") ||
        sp_streq(name, "close_read") || sp_streq(name, "close_write")) return TY_POLY;
    if (sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ctime") ||
        sp_streq(name, "birthtime")) return TY_TIME;
    if (sp_streq(name, "stat") || sp_streq(name, "lstat")) return TY_IO;
    if (sp_streq(name, "putc") || sp_streq(name, "printf") || sp_streq(name, "ungetc") ||
        sp_streq(name, "pid")) return TY_POLY;
    if (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) return TY_INT_ARRAY;
    if (sp_streq(name, "<<")) return TY_IO;   /* writes, returns self (chainable) */
    if (sp_streq(name, "each_line") || sp_streq(name, "each") ||
        sp_streq(name, "each_char") || sp_streq(name, "each_byte") ||
        sp_streq(name, "each_codepoint")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = (sp_streq(name, "each_byte") ||
                              sp_streq(name, "each_codepoint")) ? TY_INT : TY_STRING;
      }
      return TY_IO;
    }
    return TY_POLY;
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (sp_streq(name, "-") && argc > 0) {
      TyKind at = infer_type(c, argv[0]);
      /* Time - Time is a Float duration; Time - poly likewise (the poly holds
         a Time at run time, the common mixed-collection shape, #2456). An
         int/float offset keeps the Time type via the general `-` arm below. */
      if (at == TY_TIME || at == TY_POLY) return TY_FLOAT;
    }
    if (sp_streq(name, "utc") || sp_streq(name, "gmtime") || sp_streq(name, "getutc") ||
        sp_streq(name, "getgm") ||
        sp_streq(name, "localtime") || sp_streq(name, "getlocal") || sp_streq(name, "+") ||
        sp_streq(name, "-")) return TY_TIME;
    if (sp_streq(name, "clamp") && argc == 2) return TY_TIME;  /* self or a bound */
    if (sp_streq(name, "to_a") && argc == 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_r") && argc == 0) return TY_RATIONAL;
    if ((sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round")) && (argc == 0 || argc == 1)) return TY_TIME;
    if (sp_streq(name, "xmlschema")) return TY_STRING;   /* with or without a fraction-digits arg (#3094) */
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_POLY;  /* boxed Sym=>Int hash */
    if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) return TY_STRING;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect") || sp_streq(name, "strftime") ||
        sp_streq(name, "zone") || sp_streq(name, "asctime") ||
        sp_streq(name, "ctime")) return TY_STRING;
    if (sp_streq(name, "to_f")) return TY_FLOAT;
    /* Integer 0 for a whole second, else a Rational -- boxed at the arm */
    if (sp_streq(name, "subsec")) return TY_POLY;
    if (sp_streq(name, "utc?") || sp_streq(name, "gmt?") || sp_streq(name, "dst?") ||
        sp_streq(name, "isdst") ||
        sp_streq(name, "sunday?") || sp_streq(name, "monday?") ||
        sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
        sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    /* Time <=> Time is an Integer; against a non-Time operand it is nil, so
       the result is poly (#2677). */
    if (sp_streq(name, "<=>") && argc == 1)
      return infer_type(c, argv[0]) == TY_TIME ? TY_INT : TY_POLY;
    if (sp_streq(name, "class")) return TY_STRING;
    /* predicates (is_a?/kind_of?/instance_of?/between?/...) before the int
       catch-all below swallows them */
    { size_t tnl = strlen(name); if (tnl > 0 && name[tnl - 1] == '?') return TY_BOOL; }
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    return TY_INT;
  }

  /* Class.cmethod(...) / M::Sub.cmethod(...) -> the class method's return type.
     method_call_ret, not the raw scope ret: a tail-yield class method carries
     THIS call site's block value (the instance/implicit-self arms already
     route through it; the raw ret is the first site's type and a diverging
     second site miscompiled through it). */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) return method_call_ret(c, mi, id);
      }
    }
    /* obj.class.cmeth(...) -> unify class method return types across hierarchy */
    if (rty && sp_streq(rty, "CallNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
      int robj = nt_ref(nt, recv, "receiver");
      TyKind rrt = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
      if (ty_is_object(rrt)) {
        int cid = ty_object_class(rrt);
        int mi = comp_cmethod_in_chain(c, cid, name, NULL);
        if (mi >= 0) {
          TyKind r = (TyKind)c->scopes[mi].ret;
          for (int k = 0; k < c->nclasses; k++) {
            int _desc = 0;
            for (int _p = c->classes[k].parent; _p >= 0; _p = c->classes[_p].parent)
              if (_p == cid) { _desc = 1; break; }
            if (!_desc) continue;
            int kmi = comp_cmethod_in_class(c, k, name);
            if (kmi >= 0) r = ty_unify(r, (TyKind)c->scopes[kmi].ret);
          }
          return r;
        }
      }
    }
  }

  /* Struct instance methods */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct &&
      /* a method written in the Struct.new / Data.define block overrides the
         generated one of that name, so its own return type is the answer
         (#3794) -- mirrors the same guard in the emitter */
      !(name && comp_method_in_chain(c, ty_object_class(rt), name, NULL) >= 0)) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    if (sp_streq(name, "with") && sc->is_data) return rt;  /* copy-update returns the same type */
    if (sp_streq(name, "to_a") || sp_streq(name, "values") ||
        sp_streq(name, "deconstruct") || sp_streq(name, "members")) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_h")) {
      int block = nt_ref(nt, id, "block");
      if (block >= 0) {
        /* to_h { |k,v| [nk, nv] }: hash type from the block's pair */
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, els[0]), vt = infer_type(c, els[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING && vt == TY_STRING) return TY_STR_STR_HASH;
            if (kt == TY_STRING) return TY_STR_POLY_HASH;
            TyKind h = ty_hash_of(kt, vt);
            /* the key is not a String here, so the string-keyed fallback would
               have put whatever the block answers into a const char * slot
               (#3602) */
            return h != TY_UNKNOWN ? h : TY_POLY_POLY_HASH;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
    if ((sp_streq(name, "size") || sp_streq(name, "length")) && argc == 0) {
      /* a member of that name wins: its generated reader is the method */
      char szn[272]; snprintf(szn, sizeof szn, "@%s", name);
      if (comp_ivar_index(sc, szn) < 0) return TY_INT;
    }
    if (sp_streq(name, "values_at")) return TY_POLY_ARRAY;   /* no keys selects nothing */
    if (sp_streq(name, "hash") && argc == 0) {
      /* a member of that name wins, like size/length above (#4190) */
      char hn2[272]; snprintf(hn2, sizeof hn2, "@%s", name);
      if (comp_ivar_index(sc, hn2) < 0) return TY_INT;
    }
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_SYM_POLY_HASH;
    if (sp_streq(name, "dig") && argc >= 1) {
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) {
        TyKind mt = sc->ivar_types[mi];
        if (argc == 1) return mt;
        /* nested struct members: walk the remaining literal keys */
        {
          ClassInfo *cur = sc; int cmi = mi; int di = 1;
          while (di < argc) {
            TyKind mt2 = cur->ivar_types[cmi];
            if (!ty_is_object(mt2) || !c->classes[ty_object_class(mt2)].is_struct) break;
            ClassInfo *nx = &c->classes[ty_object_class(mt2)];
            int nmi = struct_member_idx(c, nx, argv[di]);
            if (nmi < 0) break;
            cur = nx; cmi = nmi; di++;
          }
          if (di == argc && di > 1) return cur->ivar_types[cmi];
        }
        /* dig(member, key, ...): index into the member's container */
        if (ty_is_hash(mt) && argc == 2) return ty_hash_val(mt);
        if (ty_is_array(mt) && argc == 2) return ty_array_elem(mt);
        return TY_POLY;
      }
      /* a key no literal member matches resolves at run time (#3849) */
      return TY_POLY;
    }
    if (sp_streq(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int]: return specific member type if known */
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) return sc->ivar_types[mi];
      /* integer index: try to resolve literal */
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "IntegerNode")) {
        long long idx = (long long)nt_int(nt, argv[0], "value", 0);
        if (idx < 0) idx += (long long)sc->nivars;
        if (idx >= 0 && idx < sc->nivars) return sc->ivar_types[(int)idx];
      }
      return TY_POLY;
    }
    if (sp_streq(name, "[]=") && argc == 2) return sc->nivars > 0 ? sc->ivar_types[0] : TY_POLY;
  }

  /* built-in class reopening: look up user-defined methods on scalar built-in types */
  if (recv >= 0) {
    const char *oc_cn = NULL;
    if (rt == TY_STRING)       oc_cn = "String";
    else if (rt == TY_INT)     oc_cn = "Integer";
    else if (rt == TY_FLOAT)   oc_cn = "Float";
    else if (rt == TY_SYMBOL)  oc_cn = "Symbol";
    else if (rt == TY_BOOL)    oc_cn = "TrueClass";
    if (oc_cn) {
      int oc_ci = comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return method_call_ret(c, oc_mi, id);
      }
    }
  }

  /* instance_variable_get(:@x) on a POLY receiver: unify @x's declared type
     across every instantiated class that has the slot (all the same concrete
     type -> that type; mixed or none -> poly). Without this the call fell
     through to an unrelated rule and inferred a bogus type, so the whole
     chain was silently dropped. Codegen dispatches on cls_id per class. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") && argc >= 1) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) {
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      if (sym && sym[0] == '@') {
        TyKind uni = TY_UNKNOWN;
        for (int ci = 0; ci < c->nclasses; ci++) {
          if (!c->classes[ci].instantiated) continue;
          int iv = comp_ivar_index(&c->classes[ci], sym);
          if (iv < 0) continue;
          TyKind t = c->classes[ci].ivar_types[iv];
          if (uni == TY_UNKNOWN) uni = t;
          else if (uni != t) { uni = TY_POLY; break; }
        }
        return uni == TY_UNKNOWN ? TY_POLY : uni;
      }
      return TY_POLY;
    }
  }

  /* nil? on a pointer-backed Enumerator: bool (NULL-as-nil test) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") && rt == TY_ENUMERATOR)
    return TY_BOOL;

  /* frozen? on an immutable value type: constantly-true bool */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?") &&
      (rt == TY_INT || rt == TY_FLOAT || rt == TY_SYMBOL || rt == TY_BOOL ||
       rt == TY_NIL || rt == TY_RANGE || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_BIGINT))
    return TY_BOOL;

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  /* Object receivers: the user-object face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_object_call(c, id, rt, &rr)) return rr; }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      { int rdcls2 = -1;
        if (comp_reader_in_chain(c, self->class_id, name, &rdcls2)) {
          const char *rname2 = comp_resolve_alias(c, self->class_id, name);
          char ivn[256];
          snprintf(ivn, sizeof ivn, "@%s", rname2);
          ClassInfo *rci2 = (rdcls2 >= 0 && rdcls2 < c->nclasses) ? &c->classes[rdcls2] : &c->classes[self->class_id];
          int iv = comp_ivar_index(rci2, ivn);
          if (iv >= 0) return ivar_value_ty(rci2, iv);
        }
      }
      /* bare `new` inside a class method returns an instance of self's class */
      if (self->is_cmethod && sp_streq(name, "new"))
        return ty_object(self->class_id);
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi < 0 && self->is_cmethod)
        mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        TyKind r = method_call_ret(c, mi, id);
        /* Unify with descendant direct overrides: codegen dispatch will
           emit a cls_id switch over all overrides, so the return type
           must accommodate every override's return type. */
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name) :
                                       comp_method_in_class(c, k, name);
          if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
        }
        return r;
      }
      /* Built-in class reopening: implicit self → delegate to built-in type lookup */
      if (mi < 0 && !self->is_cmethod) {
        const char *bcn = c->classes[self->class_id].name;
        TyKind brt = TY_UNKNOWN;
        if (sp_streq(bcn, "String"))        brt = TY_STRING;
        else if (sp_streq(bcn, "Integer"))  brt = TY_INT;
        else if (sp_streq(bcn, "Float"))    brt = TY_FLOAT;
        else if (sp_streq(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          /* Temporarily set rt to the built-in type and recursively call infer_call
             is not safe. Instead inline key return types for common method names. */
          if (brt == TY_STRING) {
            if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
                sp_streq(name, "capitalize") || sp_streq(name, "reverse") || sp_streq(name, "strip") ||
                sp_streq(name, "lstrip") || sp_streq(name, "rstrip") || sp_streq(name, "chomp") ||
                sp_streq(name, "chop") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
                sp_streq(name, "to_s") || sp_streq(name, "inspect") || sp_streq(name, "succ") ||
                sp_streq(name, "next") || sp_streq(name, "chr") || sp_streq(name, "encode") ||
                sp_streq(name, "encode!") || sp_streq(name, "scrub!") ||
                sp_streq(name, "b") || sp_streq(name, "force_encoding") || sp_streq(name, "scrub") ||
                sp_streq(name, "squeeze") || sp_streq(name, "tr") || sp_streq(name, "delete"))
              return TY_STRING;
            if ((sp_streq(name, "+") || sp_streq(name, "*")) && argc >= 1) return TY_STRING;
            if (sp_streq(name, "gsub") || sp_streq(name, "sub")) return TY_STRING;
            if (sp_streq(name, "[]") || sp_streq(name, "slice") || sp_streq(name, "slice!")) return TY_STRING;
            if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "bytesize") ||
                sp_streq(name, "to_i") || sp_streq(name, "count") || sp_streq(name, "ord") ||
                sp_streq(name, "hex") || sp_streq(name, "oct") || sp_streq(name, "rindex") ||
                sp_streq(name, "index"))
              return TY_INT;
            if (sp_streq(name, "to_f")) return TY_FLOAT;
            if (sp_streq(name, "to_sym") || sp_streq(name, "intern")) return TY_SYMBOL;
            if (sp_streq(name, "empty?") || sp_streq(name, "include?") ||
                sp_streq(name, "start_with?") || sp_streq(name, "end_with?") ||
                sp_streq(name, "==") || sp_streq(name, "!="))
              return TY_BOOL;
            if (sp_streq(name, "split") || sp_streq(name, "chars") || sp_streq(name, "lines") ||
                sp_streq(name, "bytes"))
              return TY_STR_ARRAY;
          }
          else if (brt == TY_INT) {
            if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                sp_streq(name, "/") || sp_streq(name, "%") || sp_streq(name, "**") ||
                sp_streq(name, "abs") || sp_streq(name, "succ") || sp_streq(name, "next") ||
                sp_streq(name, "pred") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
                sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
                sp_streq(name, "<<") || sp_streq(name, ">>"))
              return TY_INT;
            if (sp_streq(name, "to_f")) return TY_FLOAT;
            if (sp_streq(name, "to_s")) return TY_STRING;
            if (sp_streq(name, "to_r")) return TY_POLY;
            if (sp_streq(name, "odd?") || sp_streq(name, "even?") || sp_streq(name, "zero?") ||
                sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "<") ||
                sp_streq(name, "<=") || sp_streq(name, ">") || sp_streq(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_FLOAT) {
            if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                sp_streq(name, "/") || sp_streq(name, "**") || sp_streq(name, "abs") ||
                sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round") ||
                sp_streq(name, "truncate"))
              return TY_FLOAT;
            if (sp_streq(name, "to_i")) return TY_INT;
            if (sp_streq(name, "to_s")) return TY_STRING;
            if (sp_streq(name, "zero?") || sp_streq(name, "nan?") || sp_streq(name, "infinite?") ||
                sp_streq(name, "finite?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
                sp_streq(name, "<") || sp_streq(name, "<=") || sp_streq(name, ">") ||
                sp_streq(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_SYMBOL) {
            if (sp_streq(name, "to_s") || sp_streq(name, "id2name") || sp_streq(name, "inspect"))
              return TY_STRING;
            if (sp_streq(name, "to_sym") || sp_streq(name, "itself")) return TY_SYMBOL;
            if (sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
            if (sp_streq(name, "empty?") || sp_streq(name, "==") || sp_streq(name, "!="))
              return TY_BOOL;
          }
        }
      }
      /* Method defined only in descendants (not in base chain): unify the
         return types of all descendant implementations -- codegen emits a
         cls_id virtual dispatch for exactly this shape, so leaving the node
         UNKNOWN made emit_boxed discard the dispatch's value through the
         effect-comma nil (a Comparable base <=> over int/float subclass
         keys compared nil, #3237). Instance methods included. */
      {
        TyKind r = TY_UNKNOWN; int found = 0;
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name)
                                     : comp_method_in_class(c, k, name);
          if (dmi < 0) continue;
          r = found ? ty_unify(r, (TyKind)c->scopes[dmi].ret) : (TyKind)c->scopes[dmi].ret;
          found = 1;
        }
        if (found) return r;
      }
    }
  }

  /* bare call inside a module/class body -> class method of that module/class.
     Use the per-node enclosing-cbody: g_cbody_class_id is only set during the
     scope pass, not the inference fixpoint, so relying on it leaves a bare
     module-body cmethod call (e.g. `take(mk)` where mk is `def self.mk`) typed
     void. The scope pass records the enclosing cbody per node in node_cbody[id]
     (cf. analyze_scope.c, analyze.c which already read it during inference). */
  if (recv < 0) {
    int cbody = c->node_cbody[id];
    if (cbody < 0) cbody = g_cbody_class_id;
    if (cbody >= 0) {
      int smi = comp_cmethod_in_chain(c, cbody, name, NULL);
      if (smi >= 0) return method_call_ret(c, smi, id);
    }
  }
  /* bare call inside an instance_eval/exec block: dispatch on receiver class */
  if (recv < 0) {
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) return method_call_ret(c, imi, id);
    }
  }
  /* Kernel conversion with an explicit user-object receiver: obj.send(:Float, x)
     desugars to obj.Float(x); the private Kernel method is available on every
     object, so it types like the receiverless form when the receiver's chain
     does not define the name (mirrors the codegen dispatch).
     The NAME gate must come first: inferring the receiver for every 1/2-arg
     call added an infer_call<->infer_type recursion edge that other arms
     avoid structurally, and looped forever on whole-program shapes with
     zero conversion calls (the tep regression). Only the six capitalized
     conversion names ever pay the receiver inference. */
  /* Kernel#Integer/#Float take `exception: false`; the keyword hash is not one
     of the value arguments, so it must not shift the arity (#3718) */
  int kw_argc = argc;
  if (argc > 0) {
    const char *lkt = nt_type(c->nt, argv[argc - 1]);
    if (lkt && sp_streq(lkt, "KeywordHashNode")) kw_argc--;
  }
  if (recv >= 0 && (kw_argc == 1 || kw_argc == 2) && name[0] >= 'A' && name[0] <= 'Z' &&
      (sp_streq(name, "Integer") || sp_streq(name, "Float") ||
       sp_streq(name, "String") || sp_streq(name, "Rational") ||
       sp_streq(name, "Complex") || sp_streq(name, "Array"))) {
    TyKind krt = infer_type(c, recv);
    int kdisp = (ty_is_object(krt) &&
                 comp_method_in_chain(c, ty_object_class(krt), name, NULL) < 0) ||
                ((krt == TY_NIL || krt == TY_POLY || krt == TY_UNKNOWN) &&
                 comp_method_index(c, name) < 0);
    if (kdisp) {
      if (sp_streq(name, "Integer") && (kw_argc == 1 || kw_argc == 2))
        return kconv_integer_kind(c, argv[0], kw_argc < argc && kconv_noraise_kw(c, argc, argv));
      if (kw_argc == 1) {
        if (sp_streq(name, "Float"))    return TY_FLOAT;
        if (sp_streq(name, "String"))   return TY_STRING;
        if (sp_streq(name, "Rational")) return TY_RATIONAL;
        if (sp_streq(name, "Complex"))  return TY_COMPLEX;
        if (sp_streq(name, "Array")) {
          TyKind kat = infer_type(c, argv[0]);
          if (ty_is_array(kat)) return kat;
          if (kat == TY_INT)    return TY_INT_ARRAY;
          if (kat == TY_FLOAT)  return TY_FLOAT_ARRAY;
          if (kat == TY_STRING) return TY_STR_ARRAY;
          /* an object answers through its own to_ary/to_a (#3721) */
          if (ty_is_object(kat)) {
            int aci = ty_object_class(kat), ami = comp_method_in_chain(c, aci, "to_ary", NULL);
            if (ami < 0) ami = comp_method_in_chain(c, aci, "to_a", NULL);
            if (ami >= 0) return (TyKind)c->scopes[ami].ret;
          }
        }
      }
    }
  }

  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi < 0) mi = comp_included_method_index(c, name);
    if (mi >= 0) return method_call_ret(c, mi, id);
    /* Kernel conversions */
    if (sp_streq(name, "Integer") && (kw_argc == 1 || kw_argc == 2))
        return kconv_integer_kind(c, argv[0], kw_argc < argc && kconv_noraise_kw(c, argc, argv));
    if (sp_streq(name, "Float") && kw_argc == 1) return TY_FLOAT;
    if (sp_streq(name, "String") && argc == 1) return TY_STRING;
    if (sp_streq(name, "Array") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (ty_is_array(at)) return at;
      /* an object answers through its own to_ary/to_a (#3721) */
      if (ty_is_object(at)) {
        int aci2 = ty_object_class(at), ami2 = comp_method_in_chain(c, aci2, "to_ary", NULL);
        if (ami2 < 0) ami2 = comp_method_in_chain(c, aci2, "to_a", NULL);
        if (ami2 >= 0) return (TyKind)c->scopes[ami2].ret;
      }
      if (at == TY_INT)    return TY_INT_ARRAY;    /* Array(int)   -> [int]   */
      if (at == TY_FLOAT)  return TY_FLOAT_ARRAY;  /* Array(float) -> [float] */
      if (at == TY_STRING) return TY_STR_ARRAY;    /* Array(str)   -> [str]   */
      if (at == TY_RANGE)  return TY_INT_ARRAY;    /* Array(range) enumerates */
      return TY_POLY_ARRAY;
    }
    if (sp_streq(name, "Hash") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (ty_is_hash(at)) return at;              /* Hash(hash) -> the hash */
      /* an object answers through its own #to_hash (#3721) */
      if (ty_is_object(at)) {
        int hci2 = ty_object_class(at), hmi = comp_method_in_chain(c, hci2, "to_hash", NULL);
        if (hmi >= 0) return (TyKind)c->scopes[hmi].ret;
      }
      if (at == TY_POLY) return TY_POLY;          /* nil-or-hash decided at runtime */
      return TY_POLY_POLY_HASH;                   /* Hash(nil) / Hash([]) -> {} */
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && argc >= 1) return TY_STRING;
    if (sp_streq(name, "system") && argc >= 1) return TY_BOOL;
    if (sp_streq(name, "trap") && argc >= 1) return TY_POLY;  /* the previous handler: a command string or a Proc */
    /* at_exit answers the Proc it registered, so the handler stays callable (#3727) */
    if (sp_streq(name, "at_exit") && nt_ref(nt, id, "block") >= 0) return TY_PROC;
    if (sp_streq(name, "rand")) {
      if (argc == 0) return TY_FLOAT;
      if (infer_type(c, argv[0]) == TY_FLOAT_RANGE) return TY_FLOAT;   /* rand(float range) */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && sp_streq(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        int hi = nt_ref(nt, argv[0], "right");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;   /* rand(float_range) */
        /* a statically empty/reversed int range yields nil (#2519) */
        if (lo >= 0 && hi >= 0 &&
            nt_type(nt, lo) && sp_streq(nt_type(nt, lo), "IntegerNode") &&
            nt_type(nt, hi) && sp_streq(nt_type(nt, hi), "IntegerNode")) {
          long long lov = nt_int(nt, lo, "value", 0);
          long long hiv = nt_int(nt, hi, "value", 0);
          int excl = (nt_int(nt, argv[0], "flags", 0) & 4) ? 1 : 0;
          if ((excl ? hiv - 1 : hiv) < lov) return TY_POLY;   /* nil */
        }
        return TY_INT;
      }
      /* rand(int range held in a variable): an Integer, or nil if the range is
         empty at runtime -> a poly (#3221). (A Float range in a variable is
         TY_FLOAT_RANGE, handled above.) */
      if (infer_type(c, argv[0]) == TY_RANGE) return TY_POLY;
      /* rand(literal 0) is a Float in [0,1) like rand(); nonzero -> Integer */
      if (atype && sp_streq(atype, "IntegerNode") && nt_int(nt, argv[0], "value", 0) == 0)
        return TY_FLOAT;
      if (infer_type(c, argv[0]) == TY_BIGINT) return TY_BIGINT;   /* rand(Bignum bound) (#3058) */
      /* rand(Float max): CRuby truncates a positive max to an Integer range and
         returns an Integer, but max 0.0 falls back to a Float in [0,1) -- so a
         Float argument is a runtime-chosen poly (#2549). */
      if (infer_type(c, argv[0]) == TY_FLOAT) return TY_POLY;
      /* a literal nonzero Integer is an Integer; a dynamic Integer could be 0
         (Float) or nonzero (Integer), so its result is a runtime-chosen poly. */
      if (atype && sp_streq(atype, "IntegerNode")) return TY_INT;
      /* Any dynamic (non-literal) argument -- an Integer var, a poly value, a
         destructured tuple element -- may be 0 (a Float [0,1)) or nonzero (an
         Integer), so codegen boxes the result; type it poly to match rather
         than the old TY_INT default, which left `rand(poly) == 0` comparing a
         boxed value with an int (#2897). */
      return TY_POLY;
    }
    if (sp_streq(name, "srand")) return TY_INT;
    if (sp_streq(name, "sleep") && argc <= 1) return TY_INT;
  }
  /* Kernel.sleep(seconds) / ::Kernel.sleep -> Integer seconds slept */
  if (recv >= 0 && sp_streq(name, "sleep") && argc <= 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *rname = nt_str(nt, recv, "name");
      if (rname && sp_streq(rname, "Kernel")) return TY_INT;
    }
  }
  /* Signal.trap / Signal.list / Signal.signame; Process.kill (#2735, #2750) */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *rname = nt_str(nt, recv, "name");
    if (rname && sp_streq(rname, "ENV")) {
      if (sp_streq(name, "class")) return TY_CLASS;
      if (sp_streq(name, "frozen?")) return TY_BOOL;
      if (sp_streq(name, "shift")) return TY_POLY;
      if (sp_streq(name, "reject!") || sp_streq(name, "select!") || sp_streq(name, "filter!"))
        return TY_POLY;   /* nil when nothing changed (#2844) */
      if (sp_streq(name, "clear") || sp_streq(name, "update") || sp_streq(name, "merge!") ||
          sp_streq(name, "replace") || sp_streq(name, "delete_if") || sp_streq(name, "keep_if"))
        return TY_STR_STR_HASH;   /* filter-block params seed in mark_proc_captures */
    }
    if (rname && sp_streq(rname, "Signal")) {
      if (sp_streq(name, "trap") && argc >= 1) return TY_POLY;
      if (sp_streq(name, "list") && argc == 0) return TY_STR_INT_HASH;
      if (sp_streq(name, "signame") && argc == 1) return TY_STRING;
    }
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "kill") && argc >= 2)
      return TY_INT;
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "spawn") && argc >= 1)
      return TY_INT;
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "waitpid2") && argc == 1)
      return TY_POLY_ARRAY;
  }

  /* Fiber storage: Fiber[:k] and Fiber.current[:k] -> poly */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "Fiber")) return TY_POLY;
    }
    if (rty && sp_streq(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && sp_streq(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber"))
          return TY_POLY;
      }
    }
  }
  /* Fiber[:k] = v -> returns v's type */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    const char *rty = nt_type(nt, recv);
    int is_fiber = 0;
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "Fiber")) is_fiber = 1;
    }
    else if (rty && sp_streq(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && sp_streq(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber"))
          is_fiber = 1;
      }
    }
    if (is_fiber) return infer_type(c, argv[1]);
  }
  /* ENV[key] -> string or nil (use TY_STRING; null means nil). ENV.fetch
     answers its default on a miss, so the call is the union of String with
     the default's type: a String or nil default keeps the nullable string,
     anything else boxes. Typed String alone, an Array default was placed in
     the const char * slot as it stood. (The block form is the ENV snapshot's
     Hash#fetch, #2742, and types there.) */
  if (recv >= 0 && argc >= 1 && (sp_streq(name, "[]") || sp_streq(name, "fetch"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        if (sp_streq(name, "fetch") && argc >= 2) {
          TyKind dt = infer_type(c, argv[1]);
          if (dt == TY_UNKNOWN || dt == TY_NIL || dt == TY_VOID) return TY_STRING;
          return ty_unify(TY_STRING, dt);
        }
        return TY_STRING;
      }
    }
  }
  /* ENV.key?/has_key?/include?/member?(key) -> bool */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "key?") || sp_streq(name, "has_key?") ||
       sp_streq(name, "include?") || sp_streq(name, "member?"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) return TY_BOOL;
    }
  }
  /* ENV direct arms (#2743, #2746): typed results so p/interp accept them */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *rn = nt_str(nt, recv, "name");
    if (rn && sp_streq(rn, "ENV")) {
      if (sp_streq(name, "to_s")) return TY_STRING;
      /* a non-String key raises TypeError; type the (diverging) call poly */
      if (argc >= 1 &&
          (sp_streq(name, "[]") || sp_streq(name, "fetch") || sp_streq(name, "key?") ||
           sp_streq(name, "has_key?") || sp_streq(name, "include?") || sp_streq(name, "member?") ||
           sp_streq(name, "delete") || sp_streq(name, "store"))) {
        TyKind kt0 = infer_type(c, argv[0]);
        if (kt0 == TY_SYMBOL || kt0 == TY_INT || kt0 == TY_FLOAT ||
            kt0 == TY_BOOL || kt0 == TY_NIL) return TY_POLY;
      }
      /* wrong arity raises ArgumentError; poly likewise */
      if (sp_streq(name, "[]") && argc != 1) return TY_POLY;
      if (sp_streq(name, "fetch") && (argc == 0 || argc > 2)) return TY_POLY;
      if (sp_streq(name, "delete")) return TY_STRING;   /* nullable string: NULL is nil */
      if (sp_streq(name, "store") || sp_streq(name, "[]=")) return TY_STRING;
      if (sp_streq(name, "dup") || sp_streq(name, "clone") || sp_streq(name, "freeze")) return TY_POLY;
    }
  }

  /* each_slice(n).map/collect { |...| } chain: return array of block result type.
     The blockless each_slice receiver types as TY_ENUMERATOR (a first-class
     enumerator value); the codegen fold still unrolls this chain syntactically,
     so accept that receiver type here too and keep the array result. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) && (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_es = nt_ref(nt, id, "block");
    if (blk_es >= 0) {
      int body_es = nt_ref(nt, blk_es, "body");
      int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
      return ty_array_of(bn_es > 0 ? infer_type(c, bb_es[bn_es - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).map/collect { |...| } chain: return array of block result type.
     Same as each_slice above -- the blockless each_cons receiver is now
     TY_ENUMERATOR, but the codegen fold unrolls this chain syntactically. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) && (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_ec = nt_ref(nt, id, "block");
    if (blk_ec >= 0) {
      int body_ec = nt_ref(nt, blk_ec, "body");
      int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
      return ty_array_of(bn_ec > 0 ? infer_type(c, bb_ec[bn_ec - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).with_index(off).map/collect { |...| } chain. The receiver used
     to infer TY_UNKNOWN; a blockless enum.with_index is now itself a
     materialized Enumerator, so accept that type here too -- the chain arm must
     keep winning over the generic enumerator surface. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) &&
      (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int blk_wi = nt_ref(nt, id, "block");
      if (blk_wi >= 0) {
        int body_wi = nt_ref(nt, blk_wi, "body");
        int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
        return ty_array_of(bn_wi > 0 ? infer_type(c, bb_wi[bn_wi - 1]) : TY_UNKNOWN);
      }
    }
  }

  /* array.{map,each,select,...}.with_index(off) { |x, i| } result: map collects
     the block value (array of body type); each yields the receiver; select/reject
     filter, preserving the receiver's array type. */
  if (recv >= 0 && sp_streq(name, "with_index") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    const char *inner = nt_str(nt, recv, "name");
    int arr_recv = nt_ref(nt, recv, "receiver");
    TyKind arr_t = arr_recv >= 0 ? infer_type(c, arr_recv) : TY_UNKNOWN;
    /* an Integer Range source behaves as an int array (materialized by the
       emitter); each.with_index still yields the Range itself (#3228) */
    TyKind arr_t0 = arr_t;
    if (arr_t == TY_RANGE) arr_t = TY_INT_ARRAY;
    if (inner && ty_is_array(arr_t)) {
      if (sp_streq(inner, "map") || sp_streq(inner, "collect")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
        }
      }
      else if (sp_streq(inner, "each") || sp_streq(inner, "select") ||
               sp_streq(inner, "filter") || sp_streq(inner, "reject") ||
               sp_streq(inner, "take_while") || sp_streq(inner, "drop_while") ||
               sp_streq(inner, "map!") || sp_streq(inner, "collect!"))
        return (arr_t0 == TY_RANGE && sp_streq(inner, "each")) ? TY_RANGE
             : arr_t;   /* take_while/drop_while keep the element type (subset) */
    }
  }

  /* arr.each.with_index(off).<terminal> / arr.each_with_index.<terminal>:
     a blockless [elem, index]-pair enumerator consumed by the terminal.
     (matz/spinel#1481 inject/reduce result; #1483 others.) */
  if (recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    const char *rn = nt_str(nt, recv, "name");
    int chain_arr = -1;
    if (rn && sp_streq(rn, "each_with_index")) {
      chain_arr = nt_ref(nt, recv, "receiver");
    }
    else if (rn && sp_streq(rn, "with_index")) {
      int wir = nt_ref(nt, recv, "receiver");
      if (wir >= 0 && nt_type(nt, wir) && sp_streq(nt_type(nt, wir), "CallNode") &&
          nt_str(nt, wir, "name") && sp_streq(nt_str(nt, wir, "name"), "each") &&
          nt_ref(nt, wir, "block") < 0)
        chain_arr = nt_ref(nt, wir, "receiver");
    }
    TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
    if (ty_is_array(chain_at)) {
      TyKind elem = ty_array_elem(chain_at);
      if (sp_streq(name, "inject") || sp_streq(name, "reduce")) {
        int args = nt_ref(nt, id, "arguments");
        int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
        TyKind acc = (argc > 0 && argv) ? infer_type(c, argv[0]) : elem;
        /* an empty `{}` seed is a general boxed-key/value hash builder, like
           each_with_object({}) -- not the element type (#2958) */
        if (acc == TY_UNKNOWN && argc > 0 && argv && nt_type(nt, argv[0]) &&
            sp_streq(nt_type(nt, argv[0]), "HashNode")) {
          int hn = 0; nt_arr(nt, argv[0], "elements", &hn);
          if (hn == 0) acc = TY_POLY_POLY_HASH;
        }
        if (acc == TY_UNKNOWN) acc = elem;
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc = ty_promote_numeric(acc, bt); }
        }
        return acc;
      }
      int blk = nt_ref(nt, id, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      /* The codegen path only handles the |v, i| two-param block form; gate the
         result type on it so single-param forms fall to their normal rules. */
      int two_param = blk >= 0 && !block_param_is_multi(c, blk, 0) &&
                      block_param_name(c, blk, 0) && block_param_name(c, blk, 1);
      if (two_param && (sp_streq(name, "map") || sp_streq(name, "collect")))
        return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
      /* filter_map collects the truthy block values (like map, then compact) */
      if (two_param && sp_streq(name, "filter_map"))
        return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
      if (sp_streq(name, "to_a") || sp_streq(name, "entries") ||
          (two_param && (sp_streq(name, "select") || sp_streq(name, "filter") || sp_streq(name, "reject"))))
        return TY_POLY_ARRAY;   /* an array of [element, index] pairs */
      if (blk < 0 && sp_streq(name, "to_h")) {
        /* an array of [element, index] pairs collected into {element => index};
           the block form instead maps each pair, so leave it to its own rule. */
        TyKind h = ty_hash_of(elem, TY_INT);
        return h != TY_UNKNOWN ? h : TY_POLY_POLY_HASH;
      }
      if (two_param && sp_streq(name, "count")) return TY_INT;
      if (two_param && (sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?")))
        return TY_BOOL;
    }
  }

  /* homogeneous object array (sp_PtrArray of unboxed sp_X*): the typed
     counterpart of the poly-array block below, for the narrowed TY_OBJ_ARRAY
     type. Only the ops narrow_object_arrays admits appear here. */
  if (recv >= 0 && ty_is_obj_array(rt)) {
    int ecls = ty_obj_array_class(rt);
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) return ty_object(ecls);
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) return ty_object(ecls);
    if (sp_streq(name, "[]=") && argc == 2) return ty_object(ecls);
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) return rt;
    if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0) return TY_INT;
    if (sp_streq(name, "empty?") && argc == 0) return TY_BOOL;
    /* no-block comparisons (admitted by the narrowing pass only for element
       classes with `<=>`): sort keeps the array type, min/max yield an
       element (NULL-encoded nil when empty). */
    if ((sp_streq(name, "sort") || sp_streq(name, "sort!")) && argc == 0) return rt;
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) return ty_object(ecls);
  }

  /* array receiver methods */
  /* Array receivers: the array face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_array_call(c, id, rt, &rr)) return rr; }

  /* Exception class-level methods: Cls.exception(msg) is Cls.new (#2740);
     Exception.to_tty? answers whether stderr is a terminal (#2757). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *ecn = nt_str(nt, recv, "name");
    if (ecn && is_builtin_exception_name(ecn)) {
      if (sp_streq(name, "exception")) return TY_EXCEPTION;
      if (sp_streq(name, "to_tty?")) return TY_BOOL;
    }
  }

  /* exception receiver methods */
  /* A specialized rescue var is typed as the exception subclass object, but
     its exception-shaped queries still answer as on a base exception, unless
     the subclass defines its own override (#1415). */
  int exc_shaped = rt == TY_EXCEPTION ||
                   (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)) &&
                    comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (recv >= 0 && exc_shaped) {
    /* Exception#message is #to_s, so an override answering something other
       than a String carries that value out: the string-typed helper answered
       the stored message (the class name) instead (#3868). */
    if (sp_streq(name, "message") && ty_is_object(rt)) {
      int mi8 = comp_method_in_chain(c, ty_object_class(rt), "to_s", NULL);
      if (mi8 >= 0 && (TyKind)c->scopes[mi8].ret != TY_STRING &&
          (TyKind)c->scopes[mi8].ret != TY_UNKNOWN)
        return (TyKind)c->scopes[mi8].ret;
    }
    /* On a receiver whose class is only known at run time, any exception in the
       program may be the one answering: when some subclass carries a non-String
       out of #message, the query is a union and rides the boxed dispatcher. */
    if (rt == TY_EXCEPTION && (sp_streq(name, "message") || sp_streq(name, "to_s")) &&
        exc_has_nonstring_msg_override(c))
      return TY_POLY;
    if (sp_streq(name, "message") || sp_streq(name, "to_s") ||
        sp_streq(name, "to_str") || sp_streq(name, "inspect") ||
        sp_streq(name, "full_message") || sp_streq(name, "detailed_message"))
      return TY_STRING;
    /* #exception answers an instance of the receiver's own class -- itself
       with no argument, a copy carrying the new message with one; #== is the
       value comparison. Neither had an arm for a user subclass instance, so
       the value was discarded into nil (#3870). */
    if (sp_streq(name, "exception") && argc <= 1) return rt;
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "class")) return TY_CLASS;  /* a Class object, carried by name */
    if (sp_streq(name, "backtrace")) return TY_STR_ARRAY;  /* empty: no frames captured */
    if (sp_streq(name, "cause")) return TY_EXCEPTION;      /* the threaded cause, nil if none */
    if (sp_streq(name, "result")) return TY_POLY;          /* StopIteration#result, nil otherwise */
    if (sp_streq(name, "name")) return TY_POLY;            /* NameError#name, nil otherwise */
    if (sp_streq(name, "dup") || sp_streq(name, "clone")) return rt;  /* a copy keeps the (subclass) type */
    if (sp_streq(name, "key") || sp_streq(name, "receiver") || sp_streq(name, "args") ||
        sp_streq(name, "reason") || sp_streq(name, "exit_value") ||
        sp_streq(name, "tag") || sp_streq(name, "value"))
      return TY_POLY;   /* class-gated introspection accessors (#2753-#2756, #2770) */
    if (sp_streq(name, "private_call?")) return TY_BOOL;
    if (sp_streq(name, "status")) return TY_INT;       /* SystemExit#status */
    if (sp_streq(name, "success?")) return TY_BOOL;    /* SystemExit#success? */
    if (sp_streq(name, "signo")) return TY_INT;        /* SignalException#signo */
    if (sp_streq(name, "signm")) return TY_STRING;     /* SignalException#signm */
    if (rt == TY_EXCEPTION && sp_streq(name, "exception")) return TY_EXCEPTION;  /* self, or a copy carrying a new message */
    if (sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "equal?"))
      return TY_BOOL;
  }

  /* poly receiver / poly operand: result type of operations on sp_RbVal */
  if (recv >= 0 && (rt == TY_POLY || a0 == TY_POLY)) {
    /* array * n is repetition (yielding the same array type), not poly
       arithmetic, even when the count `n` widened to poly under promote. */
    if ((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "*") && argc == 1)
      return rt;
    /* `arr - x` / `arr & x` / `arr | x` with a poly operand are SET operations,
       not arithmetic. codegen coerces the operand at run time -- an Array
       becomes a poly array, anything else raises CRuby's TypeError -- so the
       result is a poly array. Typed as arithmetic instead, `-` reached
       sp_poly_sub, which had no array case and answered "no implicit
       conversion of Array into Array" on two real Arrays (#3475). */
    if ((ty_is_array(rt) || rt == TY_POLY_ARRAY) && argc == 1 &&
        (sp_streq(name, "-") || sp_streq(name, "&") || sp_streq(name, "|") ||
         sp_streq(name, "difference") || sp_streq(name, "intersection") ||
         sp_streq(name, "union")))
      return TY_POLY_ARRAY;
    /* String operators with a poly operand are NOT poly arithmetic: `str % x`
       is printf formatting, `str + x` is concatenation, `str * n` is repeat --
       all yield a string. Defer them to the rt==TY_STRING path below. */
    if (!(rt == TY_STRING && (sp_streq(name, "%") || sp_streq(name, "+") || sp_streq(name, "*"))) &&
        (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
         sp_streq(name, "/") || sp_streq(name, "%") || sp_streq(name, "**")))
      return TY_POLY;
    /* unary numeric operators on a poly receiver: negation/unary-plus stay
       poly, bitwise complement yields int. Resolve them here so the poly
       method-dispatch below does not bind `-@`/`+@` to a user class that
       happens to define one (e.g. `-@cents` with @cents widened to poly must
       not infer the enclosing Money type). */
    if (argc == 0 && (sp_streq(name, "-@") || sp_streq(name, "+@"))) return TY_POLY;
    if (argc == 0 && sp_streq(name, "~")) return TY_INT;
    if ((sp_streq(name, "include?") || sp_streq(name, "member?")) &&
        an_user_ret_disagrees(c, name, TY_BOOL))
      return TY_POLY;   /* the user arm answers something a bool cannot hold */
    if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
        sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "include?"))
      return TY_BOOL;
    if (rt == TY_POLY) {
      /* A container read the poly dispatch may have to serve from a builtin
         Array or Hash: record what the builtin surface alone would answer, so
         codegen can shape that arm (its emitters read the node's own type, and
         the node here holds the union). Computed once per node. */
      if (recv >= 0 && !an_builtin_only && poly_container_read_p(name) &&
          nt_ref(nt, id, "block") < 0 && c->poly_builtin_ty &&
          id < c->node_cap && c->poly_builtin_ty[id] == TY_UNKNOWN &&
          an_user_defines_or_reads(c, name)) {
        an_builtin_only = 1;
        TyKind bt = infer_call(c, id);
        an_builtin_only = 0;
        c->poly_builtin_ty[id] = bt;
      }
      if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return an_poly_concrete(c, name, TY_STRING);
      if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2) return an_poly_concrete(c, name, TY_STRING);
      /* a numeric argument makes it Thread#join(limit), whose answer is the
         thread or nil, not a joined string (#4287) */
      if (sp_streq(name, "join") && argc == 1 && ty_is_numeric(infer_type(c, argv[0])))
        return TY_POLY;
      if (sp_streq(name, "join")) return an_poly_concrete(c, name, TY_STRING);
      /* The multi-set forms of String#count/#delete/#squeeze, and
         Hash#store, on a boxed value: the runtime helper answers the
         string's (or the hash's) own result, and raises NoMethodError for
         anything else, so the type is the string form's (#4195). delete's
         single-set form has its own rules. */
      if (argc >= 1 && nt_ref(nt, id, "block") < 0) {
        if (argc >= 2 && sp_streq(name, "count")) return an_poly_concrete(c, name, TY_INT);
        if ((argc >= 2 && sp_streq(name, "delete")) || sp_streq(name, "squeeze"))
          return an_poly_concrete(c, name, TY_STRING);
        if (argc == 2 && sp_streq(name, "store")) return an_poly_concrete(c, name, TY_POLY);
        /* count(v): the value-equality count (sp_poly_count_val). A user
           definition blocks it only when it can TAKE one positional
           argument -- the same judgement codegen's arm makes; an
           arity-incompatible `count(a, b)` cannot answer this call, and
           counting it left the two halves naming different methods. */
        if (sp_streq(name, "count")) {
          int can1 = 0;
          for (int k2 = 0; k2 < c->nclasses && !can1; k2++) {
            int mi2 = comp_method_in_chain(c, k2, "count", NULL);
            if (mi2 >= 0 && mi2 < c->nscopes) {
              Scope *cs2 = &c->scopes[mi2];
              if (cs2->rest_idx >= 0 || (1 >= cs2->nrequired && 1 <= cs2->nparams))
                can1 = 1;
            }
          }
          if (!can1) return TY_INT;
        }
      }
      /* A length-like read answers an Integer -- unless a user class owns the
         name and answers something else, in which case the call's value is
         that union. The dispatch ALWAYS emits the builtin length arms (a
         symbol, a string, every array and hash kind), so pinning the call to
         Integer left their sp_int and the user arm's own return meeting in one
         slot, and the build stopped. Only a DISAGREEING user return widens it:
         one that answers an Integer already agrees, and widening every
         `poly.size` would box a count the whole program reads as a number. */
      if (sp_streq(name, "to_i") || sp_streq(name, "length") || sp_streq(name, "size")) {
        TyKind ur = an_user_read_ty(c, name, argc);
        if (sp_streq(name, "to_i") || ur == TY_UNKNOWN || ur == TY_INT || ur == TY_VOID)
          return an_poly_concrete(c, name, TY_INT);
        return an_poly_concrete(c, name, TY_POLY);
      }
      if (sp_streq(name, "to_f")) return an_poly_concrete(c, name, TY_FLOAT);
      /* Hash#keys / #values on a poly hash -> a poly array (boxed elements).
         to_a on a poly value follows the same rule: nil -> [], arrays and
         hashes materialize, anything else raises (sp_poly_to_a_arr). A user
         class owning the name is handled by the container-union rule at the
         user lookup below. */
      if ((sp_streq(name, "keys") || sp_streq(name, "values") ||
           sp_streq(name, "to_a")) && argc == 0) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) return an_poly_concrete(c, name, TY_POLY_ARRAY);
      }
      if (sp_streq(name, "clamp")) return an_poly_concrete(c, name, TY_POLY);  /* boxed numeric clamp -> poly */
      /* a boxed Encoding value, as the concrete String arm answers. Without a
         type the call is UNKNOWN, and emit_boxed's untyped arm renders
         `(expr, sp_box_nil())` -- it evaluates the answer and throws it away. */
      if (sp_streq(name, "encoding") && argc == 0) return an_poly_concrete(c, name, TY_POLY);
      /* nil-aware conversions (a nil local widens to poly): boxed results */
      if (argc == 0 && nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "to_a") || sp_streq(name, "to_h") ||
           sp_streq(name, "to_r") || sp_streq(name, "rationalize") ||
           sp_streq(name, "to_c"))) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) {
          /* concrete result types, matching the TY_NIL receiver arm so a
             local settled on an early (pre-widening) pass stays consistent */
          if (sp_streq(name, "to_a")) return an_poly_concrete(c, name, TY_POLY_ARRAY);
          /* Hash#to_h is the identity, so a boxed receiver keeps whatever
             variant it really holds: narrowing to the symbol-keyed one made
             `opts.to_h` on a String-keyed hash raise (#3972). nil.to_h is the
             empty hash, which the boxed answer covers too. */
          if (sp_streq(name, "to_h")) return an_poly_concrete(c, name, TY_POLY);
          if (sp_streq(name, "to_r") || sp_streq(name, "rationalize")) return an_poly_concrete(c, name, TY_RATIONAL);
          return an_poly_concrete(c, name, TY_COMPLEX);
        }
      }
      if (argc == 1 && sp_streq(name, "===")) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        /* A boxed receiver can be a Proc, whose #=== answers the proc's
           return value rather than a boolean (#3818); a poly slot holds the
           booleans every other kind answers just as well. */
        if (!has_user) return an_poly_concrete(c, name, TY_POLY);
      }
      /* & | ^ on a poly receiver dispatch on the runtime tag (nil/bool take
         the boolean ops, ints the bitwise ones) via sp_poly_bitop, whose
         result is a boxed value -- so the static type stays poly (#2401). */
      if (argc == 1 && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
        return an_poly_concrete(c, name, TY_POLY);
      /* poly.arity on a Method read out of a container: the stamped arity, an
         Integer (#3231). */
      if (argc == 0 && sp_streq(name, "arity")) return an_poly_concrete(c, name, TY_INT);
      /* the rest of the Proc face on a value read out of a container (#3685) */
      if (argc == 0 && sp_streq(name, "lambda?")) return an_poly_concrete(c, name, TY_BOOL);
      if (argc == 0 && sp_streq(name, "parameters")) return an_poly_concrete(c, name, TY_POLY_ARRAY);
      if (argc == 0 && sp_streq(name, "curry")) return an_poly_concrete(c, name, TY_CURRY);
      if (argc == 0 && sp_streq(name, "to_proc")) return an_poly_concrete(c, name, TY_POLY);
      /* String transforms on a boxed value: emit_poly_call routes these
         through sp_poly_to_s and re-boxes the result, so the value stays
         poly (mirrors the codegen list in codegen_call_recv.c). */
      if (argc == 0 &&
          (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
           sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
           sp_streq(name, "strip") || sp_streq(name, "reverse") ||
           sp_streq(name, "chomp") || sp_streq(name, "chop") ||
           sp_streq(name, "succ") || sp_streq(name, "next") ||
           sp_streq(name, "chr") ||
           /* `strip` was here and its one-sided siblings were not, which is
              what most of this line is: a String reaching the dispatch
              through a poly slot answered NoMethodError for a method String
              has. `to_str` matters most of them -- it is the implicit
              conversion protocol, so a poly slot holding a String has to
              answer it. The answers stay boxed, which is why they are POLY
              here: `ascii_only?` is a boxed boolean and reads as one. */
           sp_streq(name, "lstrip") || sp_streq(name, "rstrip") ||
           sp_streq(name, "to_str") || sp_streq(name, "ascii_only?") ||
           sp_streq(name, "valid_encoding?") || sp_streq(name, "encode") ||
           sp_streq(name, "scrub") || sp_streq(name, "b")))
        return an_poly_concrete(c, name, TY_POLY);
      /* ...and the same names where they take arguments. unpack answers a
         boxed array, byteslice a boxed String or nil; the codegen arm for
         these sits outside its argc==0 block, and this mirrors it. */
      if ((sp_streq(name, "unpack") && argc == 1) ||
          (sp_streq(name, "byteslice") && (argc == 1 || argc == 2)) ||
          (sp_streq(name, "scrub") && argc == 1) ||
          (sp_streq(name, "encode") && argc >= 1 && argc <= 3) ||
          ((sp_streq(name, "force_encoding") || sp_streq(name, "encode!")) && argc >= 1 && argc <= 2))
        return an_poly_concrete(c, name, TY_POLY);
      /* chomp / chop / delete_prefix / delete_suffix answer a String and are
         served at argc 0 only, so the separator forms -- `line.chomp("|")`,
         which is what a line reader does with its own separator -- fell to
         NoMethodError on a boxed receiver with a clean C build. */
      if (argc == 1 && (sp_streq(name, "chomp") || sp_streq(name, "delete_prefix") ||
                        sp_streq(name, "delete_suffix")))
        return an_poly_concrete(c, name, TY_STRING);
      /* poly.ljust/rjust/center(width[, pad]): a String read from a container
         widened to poly; emit_poly_call pads via sp_poly_to_s and re-boxes, so
         the result stays poly (#3222). */
      if ((sp_streq(name, "ljust") || sp_streq(name, "rjust") || sp_streq(name, "center")) &&
          (argc == 1 || argc == 2))
        return an_poly_concrete(c, name, TY_POLY);
      /* poly.bytes / poly.codepoints on a value that is really a String (a
         binary lump read whose method widened to poly): a concrete int array,
         emitted via sp_str_bytes(sp_poly_to_s(...)) with no boxing. */
      if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
          nt_ref(nt, id, "block") < 0) {
        if (!an_user_defines_or_reads(c, name)) return an_poly_concrete(c, name, TY_INT_ARRAY);
        /* A user class owns the name too, so the value may be a String (an int
           array) or that class's member (whatever it holds). One C slot cannot
           be both, and letting the member's type win made the String answer 0.
           Box it: the codegen's tag pre-arm fills either side (#3380). */
        return an_poly_concrete(c, name, TY_POLY);
      }
      /* poly.unpack1(fmt): String#unpack1 on a value that widened to poly
         (pervasive in doom's binary WAD parsing). Mirrors the rt==TY_STRING
         rule so a single-directive int format stays int, not poly. */
      if (sp_streq(name, "unpack1") && argc == 1) {
        /* a user class owning the name puts its own arm in the dispatch, and
           both arms share one C temp: type the call for what both can hold */
        TyKind u1 = an_unpack1_lit_type(nt, argv[0]);
        return an_user_ret_disagrees(c, name, u1) ? TY_POLY : u1;
      }
      /* poly.delete(chars): String#delete on a value that widened to poly
         (`data[offset, 8].delete("\x00").upcase` stripping NUL padding off a
         fixed-width WAD name field in doom's texture parser). Resolve it here
         so the poly method-dispatch below does not bind `delete` to whatever
         user class happens to define one: the receiver can still be a string,
         so a user-class `delete` (e.g. the bundled Set's) unifies WITH
         TY_STRING (-> poly) instead of replacing it. No user class keeps the
         concrete TY_STRING, like the rt==TY_STRING rule. */
      if (sp_streq(name, "delete") && argc == 1) {
        /* The answer is whatever the receiver's own kind returns, so it is
           boxed: a Hash gives the deleted value, an Array the object, a
           String the stripped copy. Committing to TY_STRING here made the
           emitter commit to String#delete, which stringified a Hash receiver
           and answered a substring of its inspect text (#3806). The container
           check below stays as documentation of the same conclusion. */
        (void)poly_expr_flows_container;
        return an_poly_concrete(c, name, TY_POLY);
      }
      if (sp_streq(name, "[]") && argc == 1) return an_poly_concrete(c, name, TY_POLY);  /* boxed array element access */
      if (sp_streq(name, "[]") && argc == 2) return an_poly_concrete(c, name, TY_POLY);  /* 2-arg poly slice */
      /* fetch on a poly Hash yields a boxed (poly) value, like `[]` -- the
         hash-value type is not statically known through the poly widening. Type
         it here so the boxed dispatch result is not discarded as nil (without
         this, `fetch` fell through to the non-hash `fetch(k, default)` rule or
         to nil, and its value-position result was dropped). */
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) return an_poly_concrete(c, name, TY_POLY);
      /* `x = v` through a writer on a poly receiver is the assigned value as
         written, like `[]=` below: the dispatch calls the writer for effect
         and yields the argument's own temp, so no arm's return widens it.
         Only when some class has the writer -- otherwise the call is the
         NoMethodError the dispatch raises. */
      if (argc == 1 && name_is_plain_setter(name) && nt_ref(nt, id, "block") < 0) {
        int owned = 0;
        char sbase[256];
        int has_base = setter_base_name(name, sbase, sizeof sbase);
        for (int k = 0; k < c->nclasses && !owned; k++)
          owned = comp_method_in_chain(c, k, name, NULL) >= 0 ||
                  (has_base && comp_writer_in_chain(c, k, sbase, NULL));
        TyKind at = owned ? infer_type(c, argv[0]) : TY_UNKNOWN;
        if (at != TY_UNKNOWN) return at;
      }
      /* []= on a poly receiver yields the assigned value, emitted boxed */
      if (sp_streq(name, "[]=") && (argc == 2 || argc == 3)) return an_poly_concrete(c, name, TY_POLY);
      if (sp_streq(name, "dig") && argc >= 1) return an_poly_concrete(c, name, TY_POLY);
      {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          TyKind et = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
          TyKind bnt = ie_block_break_next_ty(c, body);
          if (bnt != TY_UNKNOWN) et = (et == TY_UNKNOWN) ? bnt : ty_unify(et, bnt);
          return et != TY_UNKNOWN ? ty_array_of(et) : TY_POLY_ARRAY;
        }
      }
      /* poly method dispatch: unify the return type over every class that
         defines `name` (the runtime cls_id picks the impl). */
      TyKind r = TY_UNKNOWN; int found = 0, nat_found = 0;
      for (int k = 0; k < c->nclasses; k++) {
        if (an_builtin_only) continue;   /* the builtin answer alone is wanted */
        if (c->classes[k].is_native_class) {
          /* The lookup's loose fallback answers a same-name binding of ANY
             arity; one that cannot take this call's arguments is no answer to
             it. StringIO's zero-argument `getbyte` typed `s.getbyte(i)` on a
             boxed String as Integer while the emission was the builtin
             sp_poly_getbyte, whose value is boxed (#4432). */
          int nmk = comp_native_method_find(c, k, name, argc, 0);
          if (nmk >= 0 && c->native_methods[nmk].nargs == argc) {
            TyKind nr = sp_streq(c->native_methods[nmk].ret, "self")
                          ? ty_object(k) : native_spec_to_ty(c->native_methods[nmk].ret);
            r = found ? ty_unify(r, nr) : nr; found = 1; nat_found = 1;
          }
          continue;
        }
        int mi = comp_method_in_chain(c, k, name, NULL);
        /* A candidate whose own return has not been derived yet contributes
           nothing: "not known yet" is not an answer, and taking it as one is
           permanent. `def zero?; @value.zero?; end` on a union receiver
           resolves to ITSELF -- the only class defining the name -- so the
           union was its own unfinished return and the method came out void,
           answering nil for every receiver. `<=>` did the same and took
           Comparable's operators down with it (#3488, #3490). With the
           candidate skipped the builtin answer for the name applies, and once
           the method's return does settle it unifies in on a later round. */
        if (mi >= 0 && c->scopes[mi].ret == TY_UNKNOWN) continue;
        if (mi >= 0) { r = found ? ty_unify(r, c->scopes[mi].ret) : c->scopes[mi].ret; found = 1; continue; }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          /* resolve alias so `alias_method :required?, :required` reads the
             backing @required, not a bogus @required? */
          const char *rname = comp_resolve_alias(c, k, name);
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rname);
          int iv = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind rt2 = iv >= 0 ? ivar_value_ty(&c->classes[rdcls], iv) : TY_UNKNOWN;
          r = found ? ty_unify(r, rt2) : rt2; found = 1;
        }
      }
      /* The receiver is a union. When it provably carries a builtin Array or
         Hash (see infer_container_flow), a container read's value is the user
         return OR the builtin answer -- pinning it to the user return left the
         dispatch no room for the builtin arm, which then raised NoMethodError
         on a genuine Array or Hash (#3459). Poly holds both. Without the
         evidence the slot is a user object the fixpoint has not settled yet,
         and widening it there poisons the class (Set's own @data).
         The builtin answer's own shape is recorded for codegen, which emits
         the builtin arm by re-entering the ordinary emission and has to know
         whether that produces an array pointer or a boxed value. */
      if (found && !an_builtin_only && poly_container_read_p(name) &&
          nt_ref(nt, id, "block") < 0 && poly_expr_flows_container(c, recv)) {
        return an_poly_concrete(c, name, TY_POLY);
      }
      /* Nor do the blockless ENUMERATOR producers: a boxed receiver can always
         be an Array, which answers them with an Enumerator. A Struct gets one
         of these synthesized, so any program with a Struct in it typed
         `arr.each_with_index` as that Struct (#4021). */
      if (found && !an_builtin_only && argc == 0 && nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "each_with_index") || sp_streq(name, "each_index"))) {
        /* ... but the user arm is in the dispatch too, and both arms share one
           C temp. Typed from the builtin alone, its answer was crammed into an
           sp_Enumerator * -- so when the two disagree the call is poly, which
           is the only thing that holds either. */
        if (an_user_ret_disagrees(c, name, TY_ENUMERATOR)) return an_poly_concrete(c, name, TY_POLY);
        return an_poly_concrete(c, name, TY_ENUMERATOR);
      }
      /* `merge` needs no container precondition either: a boxed receiver can
         always be a Hash, and the dispatch ends in the runtime merge that lets
         it answer for itself. Typed from the user arm alone the switch and the
         slot disagreed -- and where that arm was ALSO dropped as
         type-incompatible, the switch came out empty and the call quietly
         answered its zero initializer (#4033). */
      if (found && !an_builtin_only && sp_streq(name, "merge") &&
          nt_ref(nt, id, "block") < 0 && argc >= 1)
        return an_poly_concrete(c, name, TY_POLY);
      /* the numeric surface needs no container precondition: a boxed receiver
         can always be a number (#4012) */
      if (found && !an_builtin_only && argc == 0 && poly_numeric_read_p(name) &&
          nt_ref(nt, id, "block") < 0) {
        return an_poly_concrete(c, name, TY_POLY);
      }
      /* A binary operator on a boxed receiver is lowered to sp_poly_<op>,
         whose value is boxed however the runtime dispatches it. Taking the
         user return from this union instead left the type and the emission
         disagreeing, and the two met at the assignment (#3502). */
      if (found && argc == 1 &&
          (is_arith_op(name) || sp_streq(name, "<<") || sp_streq(name, ">>") ||
           sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
        return an_poly_concrete(c, name, TY_POLY);
      /* The user arms agreed on `r`, and the dispatch writes them into one C
         temp -- but it also emits whatever the BUILTIN surface answers for the
         name, into that same temp. Ask what that would be. `Box#index`
         answering a String pinned the temp to `const char *` while the Array
         and String arms boxed their answers, and the build stopped (#4083).
         The rules above name the cases someone hit one at a time; this asks
         for every name, which is the same question an_poly_concrete asks from
         the other side. */
      if (found && !an_builtin_only && r != TY_POLY && r != TY_UNKNOWN &&
          recv >= 0 && (nat_found || an_user_defines_or_reads(c, name))) {
        /* Asking costs a full re-inference of the call, and the same node is
           asked many times inside one fixpoint iteration: counted on a 51k-line
           Rails emit, 294,164 asks over 18k nodes, 96% of them a repeat of a
           node already asked in that same iteration.  Memoize per node on the
           narrow generation, which is bumped once per iteration and so releases
           every answer when the types move.
           The answer is stable across the repeats but not perfectly: on the same
           tree 2 asks of 87,033 saw it change mid-iteration (TY_ENUMERATOR then
           TY_UNKNOWN, for one name).  The memo pins the first answer, so those
           take the earlier one.  It changed no output on either app measured. */
        long bk = narrow_key(3, id, "");
        int bhit; int bcached = narrow_memo_get(bk, &bhit);
        TyKind bt;
        /* A cached answer is trusted only while it says "no disagreement",
           which is the common case and where the whole win is -- 2 asks of
           87,033 were measured changing mid-iteration, so the rest hit. The
           answer that WIDENS is the consequential one, and the one those two
           were, so confirm it against a fresh ask rather than pinning a stale
           one. Pinning it widened calls that should have stayed typed: a
           concrete object became TY_POLY, its direct call became a runtime
           cls_id switch with a NoMethodError arm, and four rubyspec examples
           went with it (an identity assertion through .equal? cannot survive
           the value boxing). */
        int btrust = bhit && ((TyKind)bcached == TY_UNKNOWN ||
                              (TyKind)bcached == TY_VOID ||
                              (TyKind)bcached == r);
        if (btrust) { bt = (TyKind)bcached; }
        else {
          an_builtin_only = 1;
          bt = infer_call(c, id);
          an_builtin_only = 0;
          narrow_memo_put(bk, (int)bt);
        }
        if (bt != TY_UNKNOWN && bt != TY_VOID && bt != r) return TY_POLY;
      }
      if (found) return r;
      /* Numeric queries / rounding on a boxed value: the sp_poly_* helpers
         dispatch on the runtime tag (a non-numeric tag raises CRuby's
         NoMethodError). abs keeps the receiver's class and floor/... can
         return a bigint unchanged, so those stay boxed. */
      if (argc == 0) {
        if (sp_streq(name, "nan?") || sp_streq(name, "finite?") ||
            sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
            sp_streq(name, "negative?")) return an_poly_concrete(c, name, TY_BOOL);
        if (sp_streq(name, "abs") || sp_streq(name, "infinite?") ||
            sp_streq(name, "floor") || sp_streq(name, "ceil") ||
            sp_streq(name, "round") || sp_streq(name, "truncate") ||
            sp_streq(name, "conjugate") || sp_streq(name, "conj") ||
            sp_streq(name, "abs2") || sp_streq(name, "magnitude")) return an_poly_concrete(c, name, TY_POLY);
        if (sp_streq(name, "bytesize") || sp_streq(name, "ord") ||
            sp_streq(name, "bit_length") ||
            sp_streq(name, "numerator") || sp_streq(name, "denominator") ||
            sp_streq(name, "begin") || sp_streq(name, "end")) return an_poly_concrete(c, name, TY_INT);
      }
      /* Numeric#round(ndigits) on a boxed value: Float when n > 0, Integer
         when n <= 0 -- either way a boxed poly (sp_poly_round_n). */
      if (argc == 1 && sp_streq(name, "round")) return an_poly_concrete(c, name, TY_POLY);
      /* divmod answers a pair, modulo and quo a number whose class follows the
         operands. Without a type here the emitted call was evaluated for
         effect and its value dropped (#3512). */
      if (argc == 1 && (sp_streq(name, "divmod") || sp_streq(name, "modulo") ||
                        sp_streq(name, "quo") || sp_streq(name, "div") ||
                        sp_streq(name, "remainder") || sp_streq(name, "coerce")))
        return an_poly_concrete(c, name, TY_POLY);
      /* String#getbyte on a boxed value: int byte or nil on out-of-range. */
      if (argc == 1 && sp_streq(name, "getbyte")) return an_poly_concrete(c, name, TY_POLY);
      /* The count-taking Array reads on a boxed array. Their value is a new
         array; without a rule they typed nil/void and the call emitted as a
         discarded statement (#3464). rotate's count is optional. */
      if ((argc == 1 || (argc == 0 && sp_streq(name, "rotate"))) &&
          nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "first") || sp_streq(name, "last") ||
           sp_streq(name, "take") || sp_streq(name, "drop") ||
           sp_streq(name, "rotate") || sp_streq(name, "sample"))) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0 ||
              comp_reader_in_chain(c, k, name, NULL)) has_user = 1;
        if (!has_user) return an_poly_concrete(c, name, TY_POLY_ARRAY);
      }
      if (argc >= 1 && sp_streq(name, "values_at") && nt_ref(nt, id, "block") < 0) {
        int has_user = 0;
        if (!an_builtin_only)
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) return an_poly_concrete(c, name, TY_POLY_ARRAY);
      }
      /* Array-reduction methods on a boxed array element (a run from
         chunk_while etc.): the concrete element type is erased to poly, so the
         result is a boxed poly value resolved at runtime by cls_id. */
      if (argc == 0 &&
          (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
           sp_streq(name, "first") || sp_streq(name, "last") || sp_streq(name, "sample")))
        return an_poly_concrete(c, name, TY_POLY);
      /* Block iterators on a poly value that holds an array at runtime (a
         recursive param, a `case` whose arms mix arrays and scalars): the result
         is a poly array. codegen coerces the receiver via sp_poly_to_poly_array. */
      if (nt_ref(nt, id, "block") >= 0 &&
          (sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")))
        return an_poly_concrete(c, name, TY_POLY_ARRAY);
      /* Fiber/Thread/IO/File instance methods: fallback when no user class defines `name`. */
      if (sp_streq(name, "resume") || sp_streq(name, "value") || sp_streq(name, "join"))
        return an_poly_concrete(c, name, TY_POLY);
      if (sp_streq(name, "alive?") || sp_streq(name, "dead?") || sp_streq(name, "closed?") ||
          sp_streq(name, "eof?") || sp_streq(name, "tty?") || sp_streq(name, "isatty") ||
          sp_streq(name, "sync") || sp_streq(name, "sync="))
        return an_poly_concrete(c, name, TY_BOOL);
      /* IO#winsize on a poly-carried handle: [rows, cols], same as the TY_IO
         arm. Without this the call falls through to a plain poly result and the
         `size[0]` that follows reads it as an untyped value. */
      if (sp_streq(name, "winsize") && sp_feature_enabled("io/console"))
        return an_poly_concrete(c, name, TY_INT_ARRAY);
      /* the non-blocking pair on a poly-carried handle, typed as the TY_IO arm
         types it: `exception: false` answers a wait symbol (read) or nil
         (write) as well as the ordinary result, so that shape is poly and a
         String slot cannot hold it (#4236/#4237) */
      if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) &&
          an_nonblock_no_exception(c, id))
        return an_poly_concrete(c, name, TY_POLY);
      if (sp_streq(name, "read_nonblock")) return an_poly_concrete(c, name, TY_STRING);
      if (sp_streq(name, "write_nonblock")) return an_poly_concrete(c, name, TY_INT);
      if (sp_streq(name, "read") || sp_streq(name, "gets") ||
          sp_streq(name, "readline")) return an_poly_concrete(c, name, TY_STRING);
      if (sp_streq(name, "write") || sp_streq(name, "syswrite"))
        return an_poly_concrete(c, name, TY_INT);   /* IO#write / #syswrite: the byte count */
      if (sp_streq(name, "close") || sp_streq(name, "flush")) return an_poly_concrete(c, name, TY_NIL);
      if (sp_streq(name, "fileno")) return an_poly_concrete(c, name, TY_INT);
      if (sp_streq(name, "synchronize")) {
        int blk_id = nt_ref(nt, id, "block");
        if (blk_id >= 0) {
          int bdy = nt_ref(nt, blk_id, "body");
          int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
          if (bbn > 0) return infer_type(c, bbb[bbn - 1]);
        }
        return an_poly_concrete(c, name, TY_NIL);
      }
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (sp_streq(name, "to_s") || sp_streq(name, "id2name") || sp_streq(name, "name")) return TY_STRING;
    if (sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
        sp_streq(name, "to_sym") || sp_streq(name, "intern") ||
        sp_streq(name, "itself")) return TY_SYMBOL;
    if (sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
    if (sp_streq(name, "empty?") || sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    if (sp_streq(name, "succ") || sp_streq(name, "next")) return TY_SYMBOL;
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && (argc == 1 || argc == 2)) return TY_STRING;
    if ((sp_streq(name, "start_with?") || sp_streq(name, "end_with?") || sp_streq(name, "match?")) && argc == 1)
      return TY_BOOL;
    /* Symbol#<=> is defined only between Symbols; String included, any other
       operand is not comparable and the result is nil (#3081) */
    if (sp_streq(name, "<=>") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_SYMBOL) return TY_INT;
      if (at != TY_POLY && at != TY_UNKNOWN) return TY_NIL;
    }
    /* casecmp/casecmp? against a non-symbol operand answer nil */
    if (sp_streq(name, "casecmp") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_INT : TY_NIL;
    if (sp_streq(name, "casecmp?") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_BOOL : TY_NIL;
  }

  /* range receiver methods */
  if (recv >= 0 && rt == TY_RANGE) {
    /* a literal string range ("a".."z") yields strings, not ints */
    if (sp_streq(name, "to_a")) {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 && infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING)
          return TY_STR_ARRAY;
      }
    }
    /* String-endpoint range accessors read/return strings, not ints (#2467) */
    {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && !sp_streq(nt_type(nt, rn), "RangeNode")) {
        int sl = local_sole_range_node(c, rn);
        if (sl >= 0) rn = sl;
      }
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 &&
            infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING) {
          if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "end") ||
                            sp_streq(name, "first") || sp_streq(name, "last") ||
                            sp_streq(name, "min") || sp_streq(name, "max")))
            return TY_STRING;
          if (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last")))
            return TY_STR_ARRAY;
        }
      }
    }
    if (sp_streq(name, "to_a") || sp_streq(name, "entries")) return TY_INT_ARRAY;  /* (#2414) */
    if (sp_streq(name, "minmax")) return TY_POLY_ARRAY;   /* [nil, nil] when empty (#2412) */
    /* `x..Float::INFINITY`: the int range records only "unbounded", but the
       literal says what the bound was, so #end answers the Float (#3670) */
    if (sp_streq(name, "end") && argc == 0) {
      int _ri = recv;
      while (_ri >= 0 && nt_kind(nt, _ri) == NK_ParenthesesNode) {
        int _bd = nt_ref(nt, _ri, "body"); int _bn = 0;
        const int *_bb = _bd >= 0 ? nt_arr(nt, _bd, "body", &_bn) : NULL;
        _ri = _bn == 1 ? _bb[0] : -1;
      }
      if (_ri >= 0 && nt_kind(nt, _ri) == NK_RangeNode &&
          infer_endpoint_is_infinite(c, nt_ref(nt, _ri, "right")) &&
          nt_ref(nt, _ri, "right") >= 0)
        return TY_FLOAT;
    }
    /* an ENDLESS literal range: #end is nil (#2413) */
    if (sp_streq(name, "end") && ({ int _rn = recv;
        while (_rn >= 0 && nt_type(nt, _rn) && sp_streq(nt_type(nt, _rn), "ParenthesesNode")) {
          int _bd = nt_ref(nt, _rn, "body"); int _bn = 0;
          const int *_bb = _bd >= 0 ? nt_arr(nt, _bd, "body", &_bn) : NULL;
          _rn = _bn == 1 ? _bb[0] : -1;
        }
        _rn >= 0 && nt_type(nt, _rn) && sp_streq(nt_type(nt, _rn), "RangeNode") &&
        nt_ref(nt, _rn, "right") < 0; })) return TY_POLY;
    /* step { } in value position returns the receiver range (#2415) */
    if (sp_streq(name, "step") && nt_ref(nt, id, "block") >= 0) return TY_RANGE;
    if (sp_streq(name, "include?") || sp_streq(name, "member?") ||
        sp_streq(name, "cover?") || sp_streq(name, "exclude_end?") ||
        sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "overlap?")) return TY_BOOL;
    if (sp_streq(name, "step")) {
      /* step with a block walks the range and returns self */
      if (nt_ref(nt, id, "block") >= 0) return rt;
      /* a float step, or a literal range with float bounds, yields floats */
      int sfloat = argc >= 1 && infer_type(c, argv[0]) == TY_FLOAT;
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      int bfloat = 0;
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        bfloat = (lo >= 0 && infer_type(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && infer_type(c, hi) == TY_FLOAT);
      }
      return (sfloat || bfloat) ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
    }
    if (sp_streq(name, "all?") || sp_streq(name, "any?") ||
        sp_streq(name, "none?") || sp_streq(name, "one?")) return TY_BOOL;
    if (sp_streq(name, "each") && nt_ref(nt, id, "block") < 0)
      return range_each_is_external(c, id) ? TY_ENUMERATOR : TY_INT_ARRAY;
    if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) return TY_INT_ARRAY;
    if (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "size") || sp_streq(name, "count") ||
        sp_streq(name, "begin") || sp_streq(name, "end"))  return TY_INT;
    if (sp_streq(name, "bsearch")) {
      /* a float-bounded range yields a float member (or nil); an int range an
         int member. The bound types are on the receiver's RangeNode. */
      int brn = recv;
      while (brn >= 0 && nt_type(nt, brn) && sp_streq(nt_type(nt, brn), "ParenthesesNode")) {
        int pb = nt_ref(nt, brn, "body"); int pbn = 0;
        const int *pbd = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
        brn = pbn == 1 ? pbd[0] : -1;
      }
      if (brn >= 0 && nt_type(nt, brn) && sp_streq(nt_type(nt, brn), "RangeNode")) {
        int bl = nt_ref(nt, brn, "left"), br = nt_ref(nt, brn, "right");
        /* both bounds must be real NUMERIC nodes (the float-bisection branch
           needs a finite interval; a beginless/endless bound is nil) and at
           least one float -> a float member */
        TyKind blt = bl >= 0 ? infer_type(c, bl) : TY_NIL;
        TyKind brt = br >= 0 ? infer_type(c, br) : TY_NIL;
        if ((blt == TY_INT || blt == TY_FLOAT) && (brt == TY_INT || brt == TY_FLOAT) &&
            (blt == TY_FLOAT || brt == TY_FLOAT)) return TY_FLOAT;
      }
      return TY_INT;  /* a member, or nil (nullable int) */
    }
    int block = nt_ref(nt, id, "block");
    /* finite-range Enumerable methods that materialize to an int array in
       codegen: select/reject/filter (fused loop) and min_by/max_by. */
    if ((sp_streq(name, "min_by") || sp_streq(name, "max_by")) && argc >= 1) return TY_POLY_ARRAY;
    if ((sp_streq(name, "min_by") || sp_streq(name, "max_by")) && block >= 0) return TY_INT;
    if ((ty_iter_shape(name) == TY_ITER_SELECT || ty_iter_shape(name) == TY_ITER_REJECT) &&
        block >= 0) return TY_INT_ARRAY;
    if (block >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind et = bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN;
      TyKind bnt = ie_block_break_next_ty(c, body);
      if (bnt != TY_UNKNOWN) et = (et == TY_UNKNOWN) ? bnt : ty_unify(et, bnt);
      return ty_array_of(et);
    }
  }

  /* A lazy chain answers Enumerator::Lazy for #class. The chain itself has no
     runtime value, so this is the one non-forcing call it can serve (#3358). */
  if (sp_streq(name, "class") && argc == 0 && recv >= 0 && chain_is_lazy_valued(c, recv))
    return TY_CLASS;

  /* (range).lazy[.select/reject{blk}].first(n) / .first. The chain may be held
     in a variable (`p = src.lazy.select{}; p.first(n)`) -- resolve the alias to
     the chain node so the forced type matches emit_lazy_pipeline_expr (#2932). */
  if (sp_streq(name, "first") || sp_streq(name, "last")) {
    int lrecv = recv;
    if (lrecv >= 0 && nt_type(nt, lrecv) && sp_streq(nt_type(nt, lrecv), "LocalVariableReadNode")) {
      int a = lazy_alias_chain(c, lrecv);
      if (a >= 0) lrecv = a;
    }
    else if (lrecv >= 0) { int a = lazy_method_chain(c, lrecv); if (a >= 0) lrecv = a; }
  if (lrecv >= 0 && nt_type(nt, lrecv) && sp_streq(nt_type(nt, lrecv), "CallNode")) {
    int lazy_src = -1;
    int grouped = 0;   /* a terminal-adjacent each_cons/each_slice groups the stream */
    {
      /* peel the chain terminal-first: an optional grouping stage, then the
         filtering stages, down to the blockless `lazy` (mirrors the shapes
         emit_lazy_pipeline_expr fuses) */
      int cur9 = lrecv;
      const char *rname9 = nt_str(nt, cur9, "name");
      if (rname9 && (sp_streq(rname9, "each_cons") || sp_streq(rname9, "each_slice")) &&
          nt_ref(nt, cur9, "block") < 0) {
        grouped = 1;
        cur9 = nt_ref(nt, cur9, "receiver");
      }
      while (cur9 >= 0 && nt_type(nt, cur9) && sp_streq(nt_type(nt, cur9), "CallNode")) {
        const char *nm9 = nt_str(nt, cur9, "name");
        if (!nm9) break;
        if (sp_streq(nm9, "lazy") && nt_ref(nt, cur9, "block") < 0) {
          lazy_src = nt_ref(nt, cur9, "receiver");
          break;
        }
        if (sp_streq(nm9, "select") || sp_streq(nm9, "reject") || sp_streq(nm9, "filter")) {
          cur9 = nt_ref(nt, cur9, "receiver");
          continue;
        }
        break;
      }
    }
    (void)grouped;
    TyKind lst = lazy_src >= 0 ? infer_type(c, lazy_src) : TY_UNKNOWN;
    /* emit_lazy_pipeline_expr collects into a PolyArray; the counted form is
       that array, the bare form its first (boxed) element (#2994). */
    if (lazy_src >= 0 && lst == TY_RANGE)
      return (argc == 1) ? TY_POLY_ARRAY : TY_POLY;
    /* An array-source lazy first(n) (e.g. the `arr.lazy.take(n).to_a` that the
       take->first desugar produces) materializes n boxed elements; the bare
       form is that array's first element, exactly as for a range source --
       leaving it out typed `[1,2,3].lazy.first` nil and the pipeline's value
       was discarded (#3357). */
    if (lazy_src >= 0 &&
        (ty_is_array(lst) ||
         (lst == TY_UNKNOWN && nt_type(nt, lazy_src) &&
          sp_streq(nt_type(nt, lazy_src), "ArrayNode"))))
      return (argc == 1) ? TY_POLY_ARRAY : (argc == 0 ? TY_POLY : TY_UNKNOWN);
  }
  }

  /* `<source>.lazy.<ops>.size` -> the propagated source size: nil if any stage
     changes the element count, Float::INFINITY for an unbounded endless source,
     else Integer. (#2485) */
  if (sp_streq(name, "size") && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    int cur = recv, lazy_src = -1, ok = 1, kill = 0, has_take = 0;
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) { lazy_src = nt_ref(nt, cur, "receiver"); break; }
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop")) && nt_ref(nt, cur, "block") < 0) {
        if (sp_streq(nm, "take")) has_take = 1;
        cur = nt_ref(nt, cur, "receiver"); continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (sp_streq(nm, "map") || sp_streq(nm, "collect")) { cur = nt_ref(nt, cur, "receiver"); continue; }
      if (sp_streq(nm, "select") || sp_streq(nm, "filter") || sp_streq(nm, "find_all") ||
          sp_streq(nm, "reject") || sp_streq(nm, "take_while") || sp_streq(nm, "drop_while") ||
          sp_streq(nm, "filter_map") || sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) {
        kill = 1; cur = nt_ref(nt, cur, "receiver"); continue;
      }
      ok = 0; break;
    }
    /* unwrap `(1..n)` parentheses so the endless check sees the RangeNode */
    while (lazy_src >= 0 && nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ParenthesesNode")) {
      int pb = nt_ref(nt, lazy_src, "body"); int pn = 0;
      const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
      lazy_src = pn == 1 ? pd[0] : -1;
    }
    if (ok && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      int is_arr_lit = nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ArrayNode");
      if (st == TY_RANGE || ty_is_array(st) || is_arr_lit) {
        if (kill) return TY_POLY;   /* nil */
        int endless = 0;
        if (st == TY_RANGE && nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "RangeNode"))
          endless = lazy_endpoint_is_infinite(c, nt_ref(nt, lazy_src, "right"));
        if (endless && !has_take) return TY_FLOAT;   /* Float::INFINITY */
        return TY_INT;
      }
    }
  }

  /* General lazy pipeline: <int range | int array>.lazy.<map/select/reject/
     filter/take_while...>.{first(n) | take(n) | to_a | force} -> an int array. */
  if ((sp_streq(name, "first") ||
       sp_streq(name, "to_a") || sp_streq(name, "force")) &&
      recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "CallNode") ||
       (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") &&
        lazy_alias_chain(c, recv) >= 0)) &&
      nt_ref(nt, id, "block") < 0 &&
      !(sp_streq(name, "first") && argc > 1) &&
      !((sp_streq(name, "to_a") || sp_streq(name, "force")) && argc != 0)) {
    int cur = recv, lazy_src = -1, ok = 1, saw_op = 0;
    /* the chain may be held in a variable (#3012); resolve it like the
       first/last arm above does */
    if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
      int a = lazy_alias_chain(c, cur);
      if (a >= 0) cur = a;
    }
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) {
        int lrcv9 = nt_ref(nt, cur, "receiver");
        if (chain_is_lazy_valued(c, lrcv9)) { saw_op = 1; cur = lrcv9; continue; }
        lazy_src = lrcv9;
        break;
      }
      /* blockless counter stages fuse into the pipeline (codegen re-validates
         the single integer argument). */
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop") ||
           (sp_streq(nm, "each_slice") && cur == recv) ||
           sp_streq(nm, "with_index") || sp_streq(nm, "each_with_index") ||
           sp_streq(nm, "each_cons")) &&
          nt_ref(nt, cur, "block") < 0) {
        saw_op = 1; cur = nt_ref(nt, cur, "receiver");
        if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
          int a9 = lazy_alias_chain(c, cur);
          if (a9 >= 0) cur = a9;
        }
        continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (!sp_streq(nm, "map") && !sp_streq(nm, "collect") && !sp_streq(nm, "select") &&
          !sp_streq(nm, "filter") && !sp_streq(nm, "reject") && !sp_streq(nm, "take_while") &&
          !sp_streq(nm, "drop_while") &&
          !sp_streq(nm, "filter_map") && !sp_streq(nm, "flat_map") &&
          !sp_streq(nm, "collect_concat")) { ok = 0; break; }
      saw_op = 1;
      cur = nt_ref(nt, cur, "receiver");
      if (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "LocalVariableReadNode")) {
        int a = lazy_alias_chain(c, cur);
        if (a >= 0) cur = a;
      }
    }
    /* An endless range needs no `.lazy` to be one: there is no array to
       materialize, so the pipeline over it is the only well-defined reading
       (#3840). */
    if (ok && lazy_src < 0 && saw_op) {
      int rr = cur;
      while (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "ParenthesesNode")) {
        int bd = nt_ref(nt, rr, "body"); int bn = 0;
        const int *bl = bd >= 0 ? nt_arr(nt, bd, "body", &bn) : NULL;
        rr = bn == 1 ? bl[0] : -1;
      }
      if (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "RangeNode") &&
          nt_ref(nt, rr, "right") < 0 && nt_ref(nt, rr, "left") >= 0)
        lazy_src = rr;
    }
    /* `first` needs no stage between it and the lazy source: `e.lazy.first(2)`
       is as well defined as `e.lazy.map { }.first(2)` (#3586) */
    if (ok && (saw_op || sp_streq(name, "to_a") || sp_streq(name, "force") ||
               sp_streq(name, "first")) && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      /* bare `first` unwraps to the single element, not the collected array */
      TyKind res = (sp_streq(name, "first") && argc == 0) ? TY_POLY : TY_POLY_ARRAY;
      if (st == TY_RANGE || st == TY_INT_ARRAY || st == TY_ENUMERATOR ||
          st == TY_POLY_ARRAY || st == TY_STR_ARRAY || st == TY_FLOAT_ARRAY) return res;
      /* an empty array literal has no element type and so types UNKNOWN, but
         the pipeline over it is still well defined -- it yields [] (#2996) */
      if (st == TY_UNKNOWN && nt_type(nt, lazy_src) &&
          sp_streq(nt_type(nt, lazy_src), "ArrayNode")) return res;
    }
  }

  /* hash receiver methods */
  if (recv >= 0 && sp_streq(name, "default") && argc <= 1 &&
      nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "HashNode") ||
                             sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
    return TY_POLY; /* {}.default -> nil (poly nil) */
  }
  /* a literal (or single-assignment variable) Hash.new(d) receiver that
     never narrowed: .default and [] both yield the default (no write can
     have reached it). The variable form widens to poly, hence both gates. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      ((sp_streq(name, "default") && argc <= 1) ||
       (sp_streq(name, "[]") && argc == 1))) {
    int dn = hash_new_default_arg(c, recv);
    if (dn >= 0) return infer_type(c, dn);
  }
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      sp_streq(name, "values_at") && argc >= 1 &&
      hash_new_default_arg(c, recv) >= 0) return TY_POLY_ARRAY;  /* (#2408) */
  /* h.default = v as a value: the assigned value (works for the untyped {}
     literal receiver too) */
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && ty_is_hash(rt))
    return infer_type(c, argv[0]);
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && rt == TY_UNKNOWN &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")))
    return infer_type(c, argv[0]);
  /* fetch(key, default) on an unknown/empty hash: return the default type */
  if (recv >= 0 && sp_streq(name, "fetch") && argc >= 2 && !ty_is_hash(rt)) {
    TyKind dt = infer_type(c, argv[1]);
    if (dt != TY_UNKNOWN) return dt;
  }
  /* Hash receivers: the hash face of infer_call (analyze_infer_recv.c). */
  { TyKind rr; if (infer_hash_call(c, id, rt, &rr)) return rr; }

  /* <str>.encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    if (sp_streq(name, "clear") && argc == 0) return TY_STRING;  /* empties + returns self (#2332) */
    /* s[i] = v / s[i, n] = v / s[range] = v / s["sub"] = v as a VALUE: the
       assigned string (#2370) */
    if (sp_streq(name, "[]=") && (argc == 2 || argc == 3)) return TY_STRING;
    if (sp_streq(name, "concat") && argc == 0) return TY_STRING;  /* self (#2309) */
    if (sp_streq(name, "clone") && argc == 1) return TY_STRING;  /* clone(freeze: ...) */
    if (sp_streq(name, "encoding") && argc == 0) return TY_POLY;  /* an Encoding value */
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
        sp_streq(name, "reverse") ||
        ((sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) && argc == 1) ||
        sp_streq(name, "strip") || sp_streq(name, "lstrip") ||
        sp_streq(name, "rstrip") || sp_streq(name, "chomp") ||
        sp_streq(name, "chop") || sp_streq(name, "chr") || sp_streq(name, "clamp") ||
        sp_streq(name, "squeeze") || sp_streq(name, "tr") || sp_streq(name, "tr_s") ||
        sp_streq(name, "succ") || sp_streq(name, "next") ||
        sp_streq(name, "delete")) return TY_STRING;
    if (sp_streq(name, "slice!")) return TY_STRING;  /* removed part, or nil */
    if (sp_streq(name, "[]") || sp_streq(name, "slice") || sp_streq(name, "byteslice") ||
        sp_streq(name, "bytesplice") || sp_streq(name, "append_as_bytes") ||
        sp_streq(name, "force_encoding") || sp_streq(name, "b") || sp_streq(name, "encode") ||
        sp_streq(name, "encode!")) return TY_STRING;
    if ((sp_streq(name, "dump") || sp_streq(name, "undump")) && argc == 0) return TY_STRING;
    if (sp_streq(name, "index") && argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      /* nullable int (SP_INT_NIL on no match), matching the string-needle
         form -- the emitter carries the same sentinel for a regexp needle */
      if (aty && sp_streq(aty, "RegularExpressionNode")) return TY_INT;
      if (infer_type(c, argv[0]) == TY_REGEX) return TY_INT;
    }
    /* casecmp/casecmp? with a statically non-string argument: CRuby answers
       nil rather than raising, so the call types nil (the emitter drops the
       comparison and evaluates the argument for effect). */
    if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1) {
      TyKind at0 = infer_type(c, argv[0]);
      if (at0 == TY_POLY) return TY_POLY;  /* runtime tag decides: boxed result or nil */
      /* an operand that answers #to_str is converted and compared
         (rb_check_string_type). The call is typed POLY, not the Integer or
         boolean a String operand gives, because the conversion can still
         answer nothing: a #to_str answering nil is CRuby's nil casecmp. The
         emitter takes the same shape test, so both agree. */
      if (ty_is_object(at0) && class_has_to_str_shape(c, ty_object_class(at0)))
        return TY_POLY;
      if (at0 != TY_STRING && at0 != TY_UNKNOWN) return TY_NIL;
      return sp_streq(name, "casecmp") ? TY_INT : TY_BOOL;
    }
    if (sp_streq(name, "index") || sp_streq(name, "to_i") || sp_streq(name, "count") ||
        sp_streq(name, "oct") || sp_streq(name, "hex") || sp_streq(name, "ord") ||
        sp_streq(name, "bytesize") || sp_streq(name, "setbyte") || sp_streq(name, "getbyte")) return TY_INT;
    if (sp_streq(name, "scrub") || sp_streq(name, "scrub!") || sp_streq(name, "crypt")) return TY_STRING;
    if (sp_streq(name, "sum") && argc <= 1) return TY_INT;
    if (sp_streq(name, "unpack1") && (argc == 1 || argc == 2)) return an_unpack1_lit_type(nt, argv[0]);
    if (sp_streq(name, "rindex")) return TY_INT;
    /* byteindex/byterindex over a String or Regexp needle -> byte offset or
       nil (SP_INT_NIL). */
    if ((sp_streq(name, "byteindex") || sp_streq(name, "byterindex")) &&
        (argc == 1 || argc == 2) &&
        (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_REGEX))
      return TY_INT;
    if (sp_streq(name, "partition") || sp_streq(name, "rpartition")) return TY_STR_ARRAY;
    /* value-form mutators: the post-mutation string; the no-change bang
       contract carries nil as NULL through the nullable string. */
    if (sp_streq(name, "gsub!") || sp_streq(name, "sub!") || sp_streq(name, "upcase!") ||
        sp_streq(name, "downcase!") || sp_streq(name, "capitalize!") || sp_streq(name, "swapcase!") ||
        sp_streq(name, "strip!") || sp_streq(name, "lstrip!") || sp_streq(name, "rstrip!") ||
        sp_streq(name, "chomp!") || sp_streq(name, "chop!") || sp_streq(name, "squeeze!") ||
        sp_streq(name, "tr!") || sp_streq(name, "delete!") || sp_streq(name, "reverse!") ||
        sp_streq(name, "tr_s!") || sp_streq(name, "delete_prefix!") ||
        sp_streq(name, "delete_suffix!") || sp_streq(name, "dedup") ||
        sp_streq(name, "succ!") || sp_streq(name, "next!") ||
        sp_streq(name, "concat") || sp_streq(name, "<<") || sp_streq(name, "prepend") ||
        sp_streq(name, "insert") || sp_streq(name, "replace"))
      return TY_STRING;
    if (sp_streq(name, "ascii_only?") || sp_streq(name, "valid_encoding?")) return TY_BOOL;
    if (sp_streq(name, "to_f"))  return TY_FLOAT;
    if (sp_streq(name, "to_r") && argc == 0) return TY_RATIONAL;
    if ((sp_streq(name, "each_char") || sp_streq(name, "each_line") ||
         sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))
      return TY_ENUMERATOR;  /* each_line(chomp: ...) blockless */
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        infer_type(c, argv[0]) == TY_STRING)
      return TY_ENUMERATOR;  /* each_line(sep) blockless */
    if (sp_streq(name, "lines") && argc == 1 && infer_type(c, argv[0]) == TY_STRING)
      return TY_STR_ARRAY;   /* lines(sep) */
    if (sp_streq(name, "lines") && argc == 2 && infer_type(c, argv[0]) == TY_STRING &&
        nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode"))
      return TY_STR_ARRAY;   /* lines(sep, chomp: true) (#3546) */
    if (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "each_byte")) return TY_STRING;
    { int blk = nt_ref(nt, id, "block");
      if (blk >= 0 && (sp_streq(name, "chars") || sp_streq(name, "lines"))) return TY_STRING;
      if (blk >= 0 && (sp_streq(name, "bytes") || sp_streq(name, "codepoints"))) return TY_STRING;
      /* the block form of split iterates and answers the receiver too */
      if (blk >= 0 && sp_streq(name, "split")) return TY_STRING; }
    if (sp_streq(name, "split") || sp_streq(name, "lines")) return TY_STR_ARRAY;
    if (sp_streq(name, "scan") && argc == 1) {
      /* the block form iterates and returns self (the receiver string) */
      if (nt_ref(nt, id, "block") >= 0) return TY_STRING;
      /* scan with capture groups returns poly_array (array of arrays or
         strings). Whether the rows are whole matches or capture rows is a
         property of the pattern's SOURCE, and the pattern may be reached
         through a name: a constant or a local bound to a literal is exactly as
         visible as the literal, which is what an_regex_lit_src resolves (and
         what codegen's re_lit_node resolves on its side). Reading only a direct
         literal node left the two disagreeing the moment the pattern had a
         name -- a capturing constant compiled to sp_re_scan_poly under a
         str_array type (#3391). */
      const char *rsrc = an_regex_lit_src(c, argv[0]);
      if (rsrc) return an_re_has_captures(rsrc) ? TY_POLY_ARRAY : TY_STR_ARRAY;
      /* A regex whose source is not visible at all -- an interpolated literal,
         an inline `Regexp.new(s)`, a method's return -- can only be asked at
         run time whether it captures, so take the shape that answers both:
         sp_re_scan_poly pushes the whole match when the pattern has no groups
         and a captures row when it does (#3389). */
      if (infer_type(c, argv[0]) == TY_REGEX) return TY_POLY_ARRAY;
      return TY_STR_ARRAY;
    }
    if (sp_streq(name, "upto") && argc == 1) return TY_STR_ARRAY;  /* blockless: materialized sequence */
    if (sp_streq(name, "bytes") || sp_streq(name, "codepoints")) return TY_INT_ARRAY;
    if (sp_streq(name, "unpack") && (argc == 1 || argc == 2)) return TY_POLY_ARRAY;
    if (sp_streq(name, "chars")) return TY_STR_ARRAY;
    if (sp_streq(name, "intern") && argc == 0) return TY_SYMBOL;
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    if (sp_streq(name, "gsub") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RegularExpressionNode"))
      return TY_ENUMERATOR;  /* blockless gsub(/re/): an Enumerator of matches */
    if (sp_streq(name, "gsub") || sp_streq(name, "sub") || sp_streq(name, "tr") ||
        sp_streq(name, "center") || sp_streq(name, "ljust") || sp_streq(name, "rjust"))
      return TY_STRING;
    if (sp_streq(name, "*")) return TY_STRING;
    /* in-place append / concat reassign the receiver and evaluate to it */
    if ((sp_streq(name, "<<") || sp_streq(name, "concat") || sp_streq(name, "prepend")) && argc == 1)
      return TY_STRING;
  }
  /* <int_array>.product(<int_array>)[.to_a].inspect -> a string */
  if (sp_streq(name, "inspect") && argc == 0 && recv >= 0) {
    int pr = recv;
    if (nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") &&
        nt_str(nt, pr, "name") && sp_streq(nt_str(nt, pr, "name"), "to_a"))
      pr = nt_ref(nt, pr, "receiver");
    if (pr >= 0 && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") && nt_str(nt, pr, "name") &&
        (sp_streq(nt_str(nt, pr, "name"), "product") || sp_streq(nt_str(nt, pr, "name"), "slice_before") ||
         sp_streq(nt_str(nt, pr, "name"), "slice_after") || sp_streq(nt_str(nt, pr, "name"), "slice_when") ||
         sp_streq(nt_str(nt, pr, "name"), "chunk")))
      return TY_STRING;
  }

  /* numeric.step(...) without a block materializes the sequence as an array */
  if (recv >= 0 && ty_is_numeric(rt) && sp_streq(name, "step") && nt_ref(nt, id, "block") < 0) {
    int args = nt_ref(nt, id, "arguments");
    int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
    int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
              (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
    /* a Bignum limit or step walks the sequence boxed (#3006) */
    for (int sk = 0; sk < sc; sk++)
      if (infer_type(c, sv[sk]) == TY_BIGINT) return TY_POLY_ARRAY;
    return isf ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    /* pow(exp, mod) with a Bignum modulus stays in bigint; lcm with a Bignum
       argument is at least that large (#3006) */
    if (sp_streq(name, "pow") && argc == 2 && infer_type(c, argv[1]) == TY_BIGINT)
      return TY_BIGINT;
    if (sp_streq(name, "lcm") && argc == 1 && infer_type(c, argv[0]) == TY_BIGINT)
      return TY_BIGINT;
    if (sp_streq(name, "ceil") || sp_streq(name, "floor") ||
        sp_streq(name, "round") || sp_streq(name, "truncate")) return TY_INT;  /* no precision arg -> self */
    if (sp_streq(name, "divmod") && argc == 1) return TY_INT_ARRAY;  /* [quotient, remainder] */
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "even?") || sp_streq(name, "odd?") || sp_streq(name, "zero?") ||
        sp_streq(name, "positive?") || sp_streq(name, "negative?") ||
        sp_streq(name, "integer?") || sp_streq(name, "finite?") ||
        sp_streq(name, "real?")) return TY_BOOL;
    if (sp_streq(name, "infinite?") && argc == 0) return TY_INT;  /* always nil (nullable int) */
    /* Numeric / Complex-projection on a real Integer (#2328) */
    if ((sp_streq(name, "abs2") || sp_streq(name, "real") || sp_streq(name, "imaginary") ||
         sp_streq(name, "imag") || sp_streq(name, "conj") || sp_streq(name, "conjugate")) && argc == 0)
      return TY_INT;
    if (sp_streq(name, "i") && argc == 0) return TY_COMPLEX;
    if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0)
      return TY_POLY;  /* Integer 0 or Float PI */
    if ((sp_streq(name, "rect") || sp_streq(name, "rectangular")) && argc == 0) return TY_INT_ARRAY;
    if (sp_streq(name, "polar") && argc == 0) return TY_POLY_ARRAY;
    if ((sp_streq(name, "ord") || sp_streq(name, "to_int")) && argc == 0) return TY_INT;
    /* pow with a literal negative exponent yields the exact Rational */
    if (sp_streq(name, "pow") && argc == 1 && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
        nt_int(nt, argv[0], "value", 0) < 0) return TY_RATIONAL;
    if (sp_streq(name, "pow") && argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
    if ((sp_streq(name, "ceildiv") || sp_streq(name, "pow")) && argc >= 1) return TY_INT;
    if ((sp_streq(name, "pred") || sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) return TY_INT;
    if (sp_streq(name, "nonzero?") && argc == 0) return TY_INT;  /* self or nil (nullable int) */
    /* Integer as a Rational: numerator is self, denominator is 1. */
    if ((sp_streq(name, "numerator") || sp_streq(name, "denominator")) && argc == 0) return TY_INT;
    if ((sp_streq(name, "to_r") && argc == 0) ||
        (sp_streq(name, "rationalize") && (argc == 0 || argc == 1))) return TY_RATIONAL;
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    /* times/upto/downto/step with a block return the receiver (self) */
    if ((sp_streq(name, "times") || sp_streq(name, "upto") || sp_streq(name, "downto") ||
         sp_streq(name, "step")) && nt_ref(nt, id, "block") >= 0) return TY_INT;
    /* times/upto/downto without a block return a range-like enumerator */
    if ((sp_streq(name, "times") || sp_streq(name, "upto") || sp_streq(name, "downto")) &&
        nt_ref(nt, id, "block") < 0) return TY_RANGE;
    if (sp_streq(name, "chr")) return TY_STRING;
    if (sp_streq(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (sp_streq(name, "bit_length") && argc == 0) return TY_INT;
    if (sp_streq(name, "fdiv") && argc == 1) return TY_FLOAT;
    if (sp_streq(name, "[]") && (argc == 1 || argc == 2)) return TY_INT;  /* bit access / bit-range field */
    if (sp_streq(name, "div") && argc == 1) return TY_INT;  /* floor division */
    if (sp_streq(name, "gcd") || sp_streq(name, "lcm")) return TY_INT;
    /* clamp keeps the applied operand's class: a Float bound can be returned, so
       the mixed int-receiver/float-bound form is poly; pure-int stays Integer. */
    if (sp_streq(name, "clamp")) {
      if (argc == 2) {
        TyKind b0 = infer_type(c, argv[0]), b1 = infer_type(c, argv[1]);
        if (b0 == TY_FLOAT || b1 == TY_FLOAT || b0 == TY_POLY || b1 == TY_POLY) return TY_POLY;
      }
      return TY_INT;
    }
    if (sp_streq(name, "magnitude") && argc == 0) return TY_INT;  /* alias for abs */
    if ((sp_streq(name, "modulo") || sp_streq(name, "remainder")) && argc == 1) return TY_INT;
    if (sp_streq(name, "gcdlcm") && argc == 1) return TY_INT_ARRAY;  /* [gcd, lcm] */
    if (sp_streq(name, "digits")) return TY_INT_ARRAY;
    if (sp_streq(name, "to_s") && argc == 1) return TY_STRING;
    if (sp_streq(name, "coerce") && argc == 1) {
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_BIGINT) return TY_POLY_ARRAY;   /* [big, big] boxed pair (#2419) */
      if (a0 == TY_FLOAT || a0 == TY_RATIONAL || a0 == TY_COMPLEX) return TY_FLOAT_ARRAY;
      /* Only a NUMBER coerces. Typing anything else as the int pair put the
         argument straight into an sp_int slot, so a String stopped the C build
         and a nil answered a coerced 0 where CRuby raises (#4011). The boxed
         pair carries whatever the runtime decides, including the raise. */
      if (a0 == TY_POLY) return TY_POLY_ARRAY;   /* the tag decides at run time */
      /* everything else is the Float() pair (and its errors) */
      if (a0 != TY_INT && a0 != TY_UNKNOWN) return TY_FLOAT_ARRAY;
      return TY_INT_ARRAY;
    }
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0)
      return TY_POLY;  /* Integer 0 or Float PI (#2316) */
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    if (sp_streq(name, "coerce") && argc == 1) return TY_FLOAT_ARRAY;  /* [Float(other), self] */
    if (sp_streq(name, "divmod") && argc == 1) return TY_POLY_ARRAY;  /* [Integer, Float] */
    if (sp_streq(name, "infinite?")) return TY_INT;   /* nil / 1 / -1 (nullable int) */
    if (sp_streq(name, "nan?") || sp_streq(name, "finite?") ||
        sp_streq(name, "positive?") || sp_streq(name, "negative?") ||
        sp_streq(name, "zero?") || sp_streq(name, "integer?") ||
        sp_streq(name, "real?")) return TY_BOOL;
    if (sp_streq(name, "nonzero?")) return TY_POLY;   /* self (Float) or nil */
    if (sp_streq(name, "div") && argc == 1) return TY_INT;  /* integer floor-division */
    /* Complex-view methods on a real Float */
    if (sp_streq(name, "abs2") || sp_streq(name, "real") ||
        sp_streq(name, "conj") || sp_streq(name, "conjugate")) return TY_FLOAT;
    if (sp_streq(name, "imag") || sp_streq(name, "imaginary")) return TY_INT;
    if (sp_streq(name, "rect") || sp_streq(name, "rectangular") ||
        sp_streq(name, "polar")) return TY_POLY_ARRAY;
    if (sp_streq(name, "i")) return TY_COMPLEX;
    /* Float <=> Rational: compare via the rational's float value (#2596) */
    if (sp_streq(name, "<=>") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) return TY_INT;
    if (sp_streq(name, "next_float") || sp_streq(name, "prev_float") ||
        sp_streq(name, "abs") || sp_streq(name, "magnitude") ||
        sp_streq(name, "modulo") || sp_streq(name, "remainder") || sp_streq(name, "to_f") ||
        (sp_streq(name, "fdiv") && argc == 1)) return TY_FLOAT;
    /* Float#numerator is an Integer when finite and the (non-finite) Float
       itself otherwise, so it is boxed; #denominator is always an Integer
       (1 for a non-finite value). (#3011) */
    if (sp_streq(name, "numerator") && argc == 0) return TY_POLY;
    if (sp_streq(name, "denominator") && argc == 0) return TY_INT;
    if ((sp_streq(name, "to_r") && argc == 0) ||
        (sp_streq(name, "rationalize") && (argc == 0 || argc == 1))) return TY_RATIONAL;
    if (sp_streq(name, "eql?") && argc == 1) return TY_BOOL;
    /* clamp with float bounds returns a float (matches codegen in codegen_call.c);
       a mixed/int bound can return the Integer bound, so leave that poly. */
    if (sp_streq(name, "clamp") && argc == 2 &&
        infer_type(c, argv[0]) == TY_FLOAT && infer_type(c, argv[1]) == TY_FLOAT)
      return TY_FLOAT;
    if (sp_streq(name, "floor") || sp_streq(name, "ceil") ||
        sp_streq(name, "round") || sp_streq(name, "truncate")) {
      /* CRuby chooses the return class from the runtime ndigits value: Integer
         when ndigits <= 0, Float when ndigits > 0. With a literal ndigits we
         match it exactly. A NON-literal ndigits can't be classified statically,
         so the result stays Float and the value is still computed exactly (x
         rounded to n places); only #class differs from CRuby when n turns out
         <= 0 -- the documented residual divergence (docs/float-rounding.md). */
      /* a trailing `half:` keyword only picks the tie-break mode; peel it
         off the positional count for the class choice */
      int fr_argc = argc;
      if (fr_argc >= 1 && nt_type(nt, argv[fr_argc - 1]) &&
          sp_streq(nt_type(nt, argv[fr_argc - 1]), "KeywordHashNode"))
        fr_argc--;
      if (fr_argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        /* Non-literal ndigits: the class (Integer when <= 0, Float when > 0) is
           only known at runtime, so the result is a boxed poly chosen there. */
        if (!aty || !sp_streq(aty, "IntegerNode")) return TY_POLY;
        return nt_int(nt, argv[0], "value", 0) > 0 ? TY_FLOAT : TY_INT;
      }
      return TY_INT;  /* no arg -> self truncated to Integer */
    }
  }

  /* A boxed receiver's `===` can be a Proc's, whose answer is the proc's
     return value rather than a boolean (#3818). Everything else boxed answers
     a boolean, which a poly slot holds just as well. */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 && rt == TY_POLY)
    return TY_POLY;
  /* /re/ === str -> match boolean */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode"))
    return TY_BOOL;
  /* Class.===(obj) is always bool */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    return TY_BOOL;

  if ((sp_streq(name, "-@") || sp_streq(name, "+@")) && recv >= 0 && argc == 0) {
    if (rt == TY_STRING) return TY_STRING;  /* +str = mutable copy; -str = frozen self */
    return ty_is_numeric(rt) ? rt : rt == TY_POLY ? TY_POLY : TY_UNKNOWN;
  }
  /* unary bitwise complement: ~int / ~poly -> int (poly value coerced via to_i) */
  if (sp_streq(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY))
    return TY_INT;
  if (sp_streq(name, "!")) return TY_BOOL;
  if (sp_streq(name, "respond_to?") && recv >= 0) return TY_BOOL;
  if ((sp_streq(name, "method_defined?") || sp_streq(name, "const_defined?") ||
       sp_streq(name, "public_method_defined?") || sp_streq(name, "private_method_defined?") ||
       sp_streq(name, "protected_method_defined?")) && recv >= 0) return TY_BOOL;
  /* const_get(:K) with a literal name resolves to the constant's type (codegen
     emits cst_<K>); a literal name that does not resolve raises NameError at
     runtime, so its value type is poly. A dynamic name is left unresolved. */
  if (sp_streq(name, "const_get") && recv >= 0 && argc >= 1) {
    const char *cgt = nt_type(nt, argv[0]);
    const char *cgn = NULL;
    if (cgt && sp_streq(cgt, "SymbolNode")) cgn = nt_str(nt, argv[0], "value");
    else if (cgt && sp_streq(cgt, "StringNode")) cgn = nt_str(nt, argv[0], "content");
    /* const_get(name, false) searches only the receiver's own constants, so an
       inherited one is a NameError, not that constant's type (#3762) */
    if (cgn && argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "FalseNode")) {
      const char *cg_rty = nt_type(nt, recv);
      const char *cg_rnm = (cg_rty && (sp_streq(cg_rty, "ConstantReadNode") ||
                                       sp_streq(cg_rty, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
      if (cg_rnm && !const_owned_by_class(c, cg_rnm, cgn)) return TY_POLY;
    }
    /* a CLASS or module name answers the class object itself (#3969) */
    if (cgn && comp_class_index(c, cgn) >= 0) return TY_CLASS;
    if (cgn) { LocalVar *cv = comp_const(c, cgn); if (cv && cv->type != TY_UNKNOWN) return cv->type; return TY_POLY; }
  }
  if (sp_streq(name, "nil?") && recv >= 0 && argc == 0) return TY_BOOL;
  /* A generated READER of this name owns it on a concrete object, as it does
     in CRuby -- Data.define(:object_id) answers the member (CRuby warns and
     defines it). Codegen's reader arm already wins there; typing the call
     Integer here split the two halves and the build stopped (#4190). */
  if ((sp_streq(name, "object_id") || sp_streq(name, "__id__")) && recv >= 0 && argc == 0) {
    if (ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR)
      { /* fall through to the member-read rule below */ }
    else return TY_INT;
  }
  /* #hash on a primitive returns an Integer (CRuby's any_hash contract): the
     value is the receiver boxed through sp_rbval_hash_key, the same hashing the
     Hash container uses, so a user `def hash = v.hash` composes consistently. A
     concrete user object is left to its own #hash method dispatch (which may
     return any type -- honored via the sp_obj_hash_hook for keys). */
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 && !ty_is_object(rt)) return TY_INT;
  if (sp_streq(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) return TY_BOOL;
  /* int & | ^ a Bignum operand promotes (#2422). `&` too: a negative receiver
     is sign-extended forever, so `-1 & 0xFFFFFFFFFFFFFFFF` IS that mask. */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "|") || sp_streq(name, "^") || sp_streq(name, "&")) &&
      infer_type(c, recv) == TY_INT && infer_type(c, argv[0]) == TY_BIGINT) return TY_BIGINT;
  if ((sp_streq(name, "match?") || sp_streq(name, "!~")) && recv >= 0) return TY_BOOL;
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    const char *rrt = nt_type(nt, recv), *art = argc > 0 ? nt_type(nt, argv[0]) : NULL;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")))
      /* the block form evaluates to the block's value (nil on a miss) */
      return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
  }
  if (sp_streq(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")) ||
        rt == TY_REGEX || a0t == TY_REGEX) return TY_POLY;
  }
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if (rt == TY_REGEX || a0t == TY_REGEX)
      return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
    /* String#match with a String pattern (regexp source) -> MatchData */
    if ((rt == TY_STRING || rt == TY_STRBUF) && a0t == TY_STRING)
      return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode")) {
    if (sp_streq(name, "source")) return TY_STRING;
    if (sp_streq(name, "options")) return TY_INT;
  }

  /* array set operations: &, intersection, |, union, -, difference. The named
     forms are variadic (fold over each argument); the operators are binary. */
  if (recv >= 0 && argc >= 1 &&
      (sp_streq(name, "&") || sp_streq(name, "intersection") ||
       sp_streq(name, "|") || sp_streq(name, "union") ||
       sp_streq(name, "-") || sp_streq(name, "difference"))) {
    if (ty_is_array(rt) && a0 == rt) return rt;
    /* empty array [] arg (TY_UNKNOWN): result is same kind as receiver */
    if (ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
    /* any array receiver with a different-kind (or poly) array argument: the
       codegen boxes both operands to poly and runs the poly set op, so the
       result is a poly array. */
    if (ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
  }
  /* The variadic set operations with NO argument answer a copy of the
     receiver, and fetch_values with none answers an empty Array; only the
     union form had an arm, so the others were refused outright (#3851). */
  if (recv >= 0 && argc == 0 && ty_is_array(rt) &&
      (sp_streq(name, "intersection") || sp_streq(name, "difference") ||
       sp_streq(name, "union")))
    return rt;
  /* Array#intersect?(other) -> bool */
  if (recv >= 0 && argc == 1 && sp_streq(name, "intersect?") && ty_is_array(rt))
    return TY_BOOL;
  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (sp_streq(name, "%")) return TY_STRING;  /* sprintf (array or single value) */
      if (sp_streq(name, "+") || sp_streq(name, "*")) {
        /* `str + x` / `str * n` always yield a String; a poly operand (which
           holds a string at runtime) is coerced via sp_poly_to_s in codegen. */
        return TY_STRING;
      }
      return TY_UNKNOWN;
    }
    /* array + same-kind -> same kind; different-kind -> poly_array */
    if (sp_streq(name, "+") && ty_is_array(rt) && a0 == rt) return rt;
    if (sp_streq(name, "+") && ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
    /* typed array +/- an empty literal [] (a0 UNKNOWN) keeps the receiver kind */
    if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
         sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
         sp_streq(name, "intersection")) && ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
    /* array * int -> same array type (repeat); array * string -> join string */
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_INT) return rt;
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(rt) && ty_is_numeric(a0)) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) return TY_FLOAT;
      if (rt == TY_BIGINT || a0 == TY_BIGINT) return TY_BIGINT;
      return TY_INT;
    }
    /* numeric receiver <op> a coercing user object: the result is what the
       object's own <op> returns (coerce yields a pair of that class). */
    if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_RATIONAL || rt == TY_BIGINT) &&
        ty_is_object(a0)) {
      int acls = ty_object_class(a0);
      if (comp_method_in_chain(c, acls, "coerce", NULL) >= 0) {
        int op_mi = comp_method_in_chain(c, acls, name, NULL);
        /* Only when the pair is of the object's own class does its operator
           decide the type. The documented plain-number idiom never reaches
           that method -- the pair's receiver is a Float -- so taking the
           method's return there answered one operator from the class and
           another from the pair. */
        if (op_mi >= 0 && !class_has_coerce_shape(c, acls))
          return (TyKind)c->scopes[op_mi].ret;
        /* The standard idiom answers a pair of plain NUMBERS and defines no
           operator of its own, so the result is whatever the pair computes --
           known only at run time. Left UNKNOWN, the expression emitted a nil
           and `5 + obj` answered nil instead of the coerced sum. The emittable
           test is the one emit_numeric_coerce_call makes, so a #coerce this TU
           cannot call keeps the type it had rather than promising a pair that
           never gets computed. */
        return TY_POLY;
      }
    }
    /* a poly operand makes the +,-,*,/ result poly: codegen lowers these to
       sp_poly_<op>, which returns a (boxed) poly, so the static type must agree. */
    if ((rt == TY_POLY || a0 == TY_POLY) &&
        (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/") ||
         /* every operator codegen lowers the same way. Left out, `>>` took the
            user return from the poly-dispatch union while the emission still
            produced an sp_RbVal, and the two met at the assignment (#3502). */
         sp_streq(name, "%") || sp_streq(name, "**") ||
         sp_streq(name, "<<") || sp_streq(name, ">>") ||
         sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
      return TY_POLY;
    /* An Integer/Bignum arith op with a non-coercible (String/Symbol/nil/bool/
       Array/Hash/Range) argument raises TypeError at run time; type the raising
       expression as int so any value position (p, assignment) can emit it -- the
       codegen raise-expr is int-typed too (#2471). */
    if ((rt == TY_INT || rt == TY_BIGINT) &&
        (a0 == TY_STRING || a0 == TY_SYMBOL || a0 == TY_NIL || a0 == TY_BOOL ||
         ty_is_array(a0) || ty_is_hash(a0) || a0 == TY_RANGE))
      return TY_INT;
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) return TY_INT;
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) return TY_BOOL;
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
       sp_streq(name, "<<") || sp_streq(name, ">>")))
    return TY_INT;
  /* bigint bitwise ops keep arbitrary precision (a `<<` widening that overflows
     int is exactly why the receiver was promoted to bigint; and `bignum & MASK`
     can still exceed int64, e.g. 0x9e37…c16 & ((1<<64)-1)). */
  if (recv >= 0 && argc == 1 && rt == TY_BIGINT &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
       sp_streq(name, "<<") || sp_streq(name, ">>")))
    return TY_BIGINT;
  /* Integer#bit_length on a Bignum answers an int (the bit count fits int64). */
  if (recv >= 0 && argc == 0 && rt == TY_BIGINT && sp_streq(name, "bit_length"))
    return TY_INT;
  if (recv >= 0 && rt == TY_BIGINT) {
    if ((sp_streq(name, "even?") || sp_streq(name, "odd?")) && argc == 0) return TY_BOOL;
    if (sp_streq(name, "abs") && argc == 0) return TY_BIGINT;
    /* coerce pairs the operand with self; clamp stays in bigint (#3129) */
    if (sp_streq(name, "coerce") && argc == 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "clamp") && argc == 2) return TY_BIGINT;
    if ((sp_streq(name, "magnitude") || sp_streq(name, "abs2")) && argc == 0) return TY_BIGINT;  /* (#2418/#2424) */
    /* Bignum#downto/#upto with no block: materialized poly array of Bignums (#2305) */
    if ((sp_streq(name, "downto") || sp_streq(name, "upto")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_s") && argc == 1) return TY_STRING;
    if (sp_streq(name, "digits") && argc <= 1) return TY_INT_ARRAY;
    /* #2318 / #2319: query + reflection on a Bignum */
    if ((sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
         sp_streq(name, "negative?") || sp_streq(name, "integer?")) && argc == 0) return TY_BOOL;
    /* to_i / to_int is self (the full Bignum, not a truncated int); succ/pred
       stay Bignum */
    if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "succ") ||
         sp_streq(name, "next") || sp_streq(name, "pred")) && argc == 0) return TY_BIGINT;
    if (sp_streq(name, "class") && argc == 0) return TY_CLASS;
    if ((sp_streq(name, "round") || sp_streq(name, "ceil") || sp_streq(name, "floor")) &&
        (argc == 0 || argc == 1)) return TY_BIGINT;  /* #2303 */
    /* Integer/Float/bool-returning Bignum methods that need no Rational (#2469).
       to_r/rationalize/quo would need a bigint-backed Rational and stay
       unsupported. */
    if (sp_streq(name, "~") && argc == 0) return TY_BIGINT;
    if ((sp_streq(name, "numerator") || sp_streq(name, "ord")) && argc == 0) return TY_BIGINT;
    if ((sp_streq(name, "denominator") || sp_streq(name, "size")) && argc == 0) return TY_INT;
    if (sp_streq(name, "nonzero?") && argc == 0) return TY_POLY;   /* self or nil */
    if (sp_streq(name, "fdiv") && argc == 1) return TY_FLOAT;
    if (sp_streq(name, "pow") && argc == 1) return TY_BIGINT;
    /* modulo/%/remainder/modular-pow stay Bignum; divmod is a [q, r] pair;
       #[] is a single bit (0/1) (#2594) */
    if ((sp_streq(name, "modulo") || sp_streq(name, "%") || sp_streq(name, "remainder")) &&
        argc == 1) return TY_BIGINT;
    if (sp_streq(name, "pow") && argc == 2) return TY_BIGINT;
    if (sp_streq(name, "divmod") && argc == 1) return TY_POLY_ARRAY;
    /* #[] is a single bit, a bit-slice (Range or start,len) -- all narrowed to
       int here (a very wide slice truncates, like the int-receiver arm) (#3156) */
    if (sp_streq(name, "[]") && (argc == 1 || argc == 2)) return TY_INT;
    if ((sp_streq(name, "div") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
         sp_streq(name, "ceildiv")) && argc == 1) return TY_BIGINT;
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) &&
        argc == 1) return TY_BOOL;
    if (sp_streq(name, "gcdlcm") && argc == 1) return TY_POLY_ARRAY;
    /* to_r/rationalize/quo on a Bignum produce a boxed big Rational (#2469) */
    if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) return TY_POLY;
    if (sp_streq(name, "quo") && argc == 1) return TY_POLY;
  }
  /* poly recv bitwise op / `>>`: the runtime keeps a bignum operand in bignum
     space (a positive value past 2^63 must not truncate to a negative int), so
     the result is boxed like the receiver was (#3371). */
  if (recv >= 0 && argc == 1 && rt == TY_POLY &&
      (sp_streq(name, ">>") || sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
    return TY_POLY;
  /* poly recv `<<` is ambiguous (Integer#<< shift vs Array#push append); the
     runtime sp_poly_shl dispatches on the tag and returns a boxed result either
     way, so the static type is poly. A downstream bitwise op coerces it back to
     int, and an append keeps its (boxed) array -- both stay consistent. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && sp_streq(name, "<<"))
    return TY_POLY;
  /* boolean &/|/^ */
  if (recv >= 0 && argc == 1 && rt == TY_BOOL &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
    return TY_BOOL;

  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') return TY_BOOL;

  if (sp_streq(name, "to_s") || sp_streq(name, "inspect") ||
      sp_streq(name, "chr") || sp_streq(name, "to_str")) return TY_STRING;
  if (sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
      sp_streq(name, "length") || sp_streq(name, "size") ||
      sp_streq(name, "count") ||
      sp_streq(name, "ord") || sp_streq(name, "abs")) return TY_INT;
  if (sp_streq(name, "to_f")) return TY_FLOAT;
  if (sp_streq(name, "to_sym")) return TY_SYMBOL;

  if (is_void_call(name) && recv < 0) return TY_VOID;

  /* $stdout/$stderr.puts/print/write return nil (so a value-position use --
     an if/else arm or assignment -- unifies and boxes as nil). */
  if (recv >= 0 && (sp_streq(name, "puts") || sp_streq(name, "print") || sp_streq(name, "write") ||
                    sp_streq(name, "syswrite")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gv = nt_str(nt, recv, "name");
    if (gv && (sp_streq(gv, "$stdout") || sp_streq(gv, "$stderr")))
      return (sp_streq(name, "write") || sp_streq(name, "syswrite")) ? TY_INT : TY_NIL;
  }

  /* tap: run block, return self */
  if (sp_streq(name, "tap") && recv >= 0) {
    /* `[].tap { |a| a << x }`: the empty-array-literal receiver has no element
       type of its own (rt stays unknown/poly), so the block param -- typed from
       its pushes -- carries the real container type. Return it so the tap result
       and any chained `.join` (or the `p` that inspects it) agree with the
       container codegen materializes for the receiver (#3200, #3208). */
    const char *rty = nt_type(nt, recv);
    int rel = 0;
    if (rty && sp_streq(rty, "ArrayNode")) nt_arr(nt, recv, "elements", &rel);
    int tblk = nt_ref(nt, id, "block");
    if (rty && sp_streq(rty, "ArrayNode") && rel == 0 && tblk >= 0) {
      const char *bp = block_param_name(c, tblk, 0);
      Scope *bs = bp ? comp_scope_of(c, tblk) : NULL;
      LocalVar *blv = (bs && bp) ? scope_local(bs, bp) : NULL;
      if (blv) {
        /* the tap node may be inferred before the block body typed the param;
           infer the body now so blv->type is settled before we read it. */
        int bdy = nt_ref(nt, tblk, "body");
        int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
        for (int j = 0; j < bbn; j++) infer_subtree(c, bbb[j]);
        if (ty_is_array(blv->type)) return blv->type;
      }
    }
    return rt;
  }
  /* then / yield_self: run block, return block result */
  if (sp_streq(name, "then") || sp_streq(name, "yield_self")) {
    int blk_id = nt_ref(nt, id, "block");
    /* with NO block it is an enumerator of one element, the receiver (#4028) */
    if (blk_id < 0 && nt_ref(nt, id, "arguments") < 0) return TY_ENUMERATOR;
    if (blk_id >= 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      /* Pin block param to receiver type so body inference uses the right type */
      const char *bp0 = block_param_name(c, blk_id, 0);
      Scope *bs = bp0 ? comp_scope_of(c, blk_id) : NULL;
      LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind saved_blv = blv ? blv->type : TY_UNKNOWN;
      if (blv && rt != TY_UNKNOWN) blv->type = rt;
      TyKind result = infer_type(c, bbb[bbn - 1]);
      if (blv) blv->type = saved_blv;
      return result;
    }
  }
  if (sp_streq(name, "instance_eval")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0 && ty_is_object(rt) &&
        comp_method_in_chain(c, ty_object_class(rt), "instance_eval", NULL) < 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      int saved_ie = an_ie_class_id;
      an_ie_class_id = ty_object_class(rt);
      TyKind result = infer_type(c, bbb[bbn - 1]);
      an_ie_class_id = saved_ie;
      return result;
    }
    return TY_POLY;
  }

  /* safe navigation &. with unresolved type: return poly (receiver may be nil at runtime) */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.")) return TY_POLY;
  }

  /* Builtin class reopening: look up user-defined methods on Array/Numeric/Object
     receivers where no builtin method matched. */
  if (recv >= 0) {
    /* Array reopening: any array-typed receiver */
    if (ty_is_array(rt)) {
      int oc_ci = comp_class_index(c, "Array");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Numeric reopening: integers and floats */
    if (rt == TY_INT || rt == TY_FLOAT) {
      int oc_ci = comp_class_index(c, "Numeric");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* FalseClass methods (TrueClass already checked earlier for TY_BOOL) */
    if (rt == TY_BOOL) {
      int oc_ci = comp_class_index(c, "FalseClass");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Object reopening: universal fallback for any receiver type */
    {
      int oc_ci = comp_class_index(c, "Object");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* A poly receiver may hold a Class at runtime, where `name` is a class
       method (`def self.name`) -- codegen dispatches it on the class tag (#3215).
       Type the call poly (not unknown) so the result flows as a value instead of
       being discarded as a void unresolved call. */
    if (rt == TY_POLY && name) {
      for (int k = 0; k < c->nclasses; k++)
        if (comp_cmethod_in_chain(c, k, name, NULL) >= 0) return TY_POLY;
    }
  }

  /* A boxed HANDLE answering one of its own exclusive names: type the call as
     if the receiver were that handle. Codegen unboxes it back to exactly that
     before re-dispatching, and checks the runtime cls_id first, so a value of
     any other kind still raises NoMethodError (#4158 follow-up). */
  if (recv >= 0 && rt == TY_POLY && g_face_node < 0 &&
      ty_poly_handle_face(name) != TY_UNKNOWN &&
      !an_user_defines_or_reads(c, name)) {
    an_set_face_node(recv, ty_poly_handle_face(name));
    TyKind kt = infer_call(c, id);
    an_set_face_node(-1, TY_UNKNOWN);
    if (kt != TY_UNKNOWN) return kt;
  }
  /* Last resort for a boxed receiver: the face table. Answer as the typed
     call would with the receiver pinned to each owner kind in turn -- codegen
     unboxes to exactly that kind before re-entering the typed emitter, so
     both sides agree on the result slot -- and unify the owners' answers: one
     owner gives the typed call's own type, owners that disagree give poly and
     the emission boxes each arm. A receiver of no owner's kind raises
     NoMethodError there, as it did before (#3449). */
  if (recv >= 0 && rt == TY_POLY && g_face_node < 0 && !an_user_defines_or_reads(c, name)) {
    int blk = nt_ref(nt, id, "block") >= 0;
    unsigned own = ty_poly_face_owners(name, argc, blk, nt_call_args_plain(nt, id), 1) & PF_OWNERS;
    if (own) {
      TyKind r = TY_UNKNOWN;
      for (unsigned bit = 1; bit & PF_OWNERS; bit <<= 1) {
        if (!(own & bit)) continue;
        an_set_face_node(recv, ty_poly_face_kind(bit));
        TyKind ht = infer_call(c, id);
        an_set_face_node(-1, TY_UNKNOWN);
        if (ht == TY_UNKNOWN) continue;
        /* a Hash mutator answers its box, not the general copy its emitter
           worked on (see emit_face_arm) */
        if (bit == PF_HASH && (ty_poly_face_owner_flags(name, argc, blk, nt_call_args_plain(nt, id), bit) & PF_VAL_SELF)) ht = TY_POLY;
        r = r == TY_UNKNOWN ? ht : ty_unify(r, ht);
      }
      if (r != TY_UNKNOWN) return r;
    }
  }

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

/* A branch whose last statement is a bare raise / throw / exit never produces
   a value, so unifying its type into the surrounding case or if widens the
   result for nothing: `case o when Integer then 7 when String then 12 else
   raise end` is an Integer, not a poly. The BeginNode arm has applied the same
   rule to a diverging body since #2739; this generalizes it to the branch
   forms. */
static int stmts_diverge(Compiler *c, int st) {
  const NodeTable *nt = c->nt;
  if (st < 0) return 0;
  int n = 0; const int *b = nt_arr(nt, st, "body", &n);
  if (n <= 0 || !b) return 0;
  int last = b[n - 1];
  if (nt_kind(nt, last) != NK_CallNode || nt_ref(nt, last, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, last, "name");
  return nm && (sp_streq(nm, "raise") || sp_streq(nm, "fail") || sp_streq(nm, "throw") ||
                sp_streq(nm, "exit") || sp_streq(nm, "abort") || sp_streq(nm, "exit!"));
}
/* The statements a branch node carries, for the divergence test above. */
static int branch_stmts(Compiler *c, int b) {
  if (b < 0) return -1;
  NodeKind k = nt_kind(c->nt, b);
  if (k == NK_ElseNode || k == NK_IfNode || k == NK_UnlessNode)
    return nt_ref(c->nt, b, "statements");
  return k == NK_StatementsNode ? b : -1;
}

/* An empty `[]` / `{}` carries no element type of its own, so it caches
   TY_UNKNOWN, and unifying an arm that ends in one DROPS it: the branch then
   answers whatever the other arms said, and the literal's construction is
   emitted into that slot. `if true then [] else 1 end` typed Integer and the
   C compiler refused the program; `... else [] end` typed nil and answered
   nil where CRuby answers []. Answer the container the literal is instead,
   and let the unify see it. TY_UNKNOWN here means "not an empty literal
   tail", which leaves the caller's own answer alone. */
static TyKind an_empty_container_tail(Compiler *c, int stmts) {
  if (stmts < 0) return TY_UNKNOWN;
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = nt_arr(nt, stmts, "body", &n);
  int tail = (bb && n > 0) ? bb[n - 1] : stmts;
  const char *ty = nt_type(nt, tail);
  int len = 0;
  if (ty && sp_streq(ty, "ArrayNode")) {
    nt_arr(nt, tail, "elements", &len);
    if (len == 0) return TY_POLY_ARRAY;
  }
  else if (ty && sp_streq(ty, "HashNode")) {
    nt_arr(nt, tail, "elements", &len);
    if (len == 0) return TY_STR_POLY_HASH;
  }
  return TY_UNKNOWN;
}

/* infer_type for one branch arm, with the empty-literal tail above given its
   container type. */
static TyKind an_branch_ty(Compiler *c, int stmts) {
  TyKind t = stmts >= 0 ? infer_type(c, stmts) : TY_NIL;
  if (t == TY_UNKNOWN) {
    TyKind e = an_empty_container_tail(c, stmts);
    if (e != TY_UNKNOWN) return e;
  }
  return t;
}

TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  NodeKind nk = nt_kind(nt, id);
  /* A read marked strbuf_box yields the shared sp_String* HANDLE, whatever
     the node kind: a container-stored local read, a demanded string literal
     element, or a demanded call-result store (#3227). */
  if (c->strbuf_box[id]) return TY_STRBUF;

  if (nk == NK_IntegerNode)             return nt_str(nt, id, "bigval") ? TY_BIGINT : TY_INT;
  if (nk == NK_FloatNode)               return TY_FLOAT;
  if (nk == NK_ImaginaryNode)           return TY_COMPLEX;
  if (nk == NK_RationalNode)            return TY_RATIONAL;
  if (nk == NK_StringNode)              return TY_STRING;
  if (nk == NK_SourceFileNode)          return TY_STRING;
  if (nk == NK_SourceLineNode)          return TY_INT;
  if (nk == NK_SourceEncodingNode)      return TY_POLY;
  if (nk == NK_RegularExpressionNode ||
      nk == NK_InterpolatedRegularExpressionNode) return TY_REGEX;
  if (nk == NK_MatchPredicateNode)      return TY_BOOL;   /* `expr in pattern` */
  /* `/(?<n>..)/ =~ str` -- the value is the `=~` result (match index or nil). */
  if (sp_streq(ty, "MatchWriteNode")) return TY_POLY;
  if (nk == NK_InterpolatedStringNode)  return TY_STRING;
  if (nk == NK_XStringNode || nk == NK_InterpolatedXStringNode) return TY_STRING;
  if (nk == NK_InterpolatedSymbolNode)  return TY_SYMBOL;
  if (nk == NK_SymbolNode)              return TY_SYMBOL;
  if (nk == NK_TrueNode)                return TY_BOOL;
  if (nk == NK_FalseNode)               return TY_BOOL;
  if (nk == NK_NilNode)                 return TY_NIL;
  /* A while/until loop in value position evaluates to nil (a valued `break`
     is a separate gap); type it as poly so the slot holds a boxed nil. */
  if (nk == NK_WhileNode || nk == NK_UntilNode) return TY_POLY;
  if (nk == NK_RangeNode) {
    /* (:a..:e): symbols enumerate by name succession -- the whole range
       lowers to a poly array of boxed symbols (see codegen_expr) */
    {
      int slo = nt_ref(nt, id, "left"), shi = nt_ref(nt, id, "right");
      const char *slt = slo >= 0 ? nt_type(nt, slo) : NULL;
      const char *sht = shi >= 0 ? nt_type(nt, shi) : NULL;
      if (slt && sht && sp_streq(slt, "SymbolNode") && sp_streq(sht, "SymbolNode"))
        return TY_POLY_ARRAY;
    }
    /* infer the bounds so codegen can tell an int range from a string range */
    int lo = nt_ref(nt, id, "left"), hi = nt_ref(nt, id, "right");
    TyKind lt = lo >= 0 ? infer_type(c, lo) : TY_UNKNOWN;
    TyKind ht = hi >= 0 ? infer_type(c, hi) : TY_UNKNOWN;
    /* (1.0..3.0): a BOUNDED range with both endpoints float lowers to the
       distinct sp_FloatRange type. A finite mixed int/float range (1..3.0)
       stays on the ordinary int TY_RANGE path, which serves it well (its
       #to_a, #sum and #cover? are all right there and its iteration is the
       integer one CRuby performs).
       An INFINITE bound is the exception: sp_int has no value for it, so the
       int range can only record "unbounded" and #begin / #end then answer nil
       where CRuby answers +/-Infinity. Such a range takes the float
       representation, which holds the infinity -- at the cost of reporting the
       other (finite) bound as a Float (see docs/limitations.md). #3670 */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_FLOAT)
      return TY_FLOAT_RANGE;
    /* A FLOAT begin with an Integer end (1.5..5) is a Float range too: it
       cannot be iterated from a Float in CRuby either, and the integer
       representation truncated the begin, which every reader then answered
       (#3896). The mirror case (1..5.5) keeps the integer representation --
       CRuby does iterate it -- and its end readers answer the literal. */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_INT)
      return TY_FLOAT_RANGE;
    /* Only when the BEGIN is the infinite one: `(2..Float::INFINITY)` is the
       canonical lazy source and its integer enumeration is what the fused
       pipeline walks, so that shape keeps the int representation and reports
       its end through the literal arm instead. */
    if (lo >= 0 && hi >= 0 &&
        (lt == TY_FLOAT || lt == TY_INT) && (ht == TY_FLOAT || ht == TY_INT) &&
        infer_endpoint_is_infinite(c, lo))
      return TY_FLOAT_RANGE;
    /* ("a".."e"): both endpoints strings -> the distinct sp_StrRange, so a
       range held in a variable stays a Range rather than materializing into
       its element array (#3064). */
    if (lo >= 0 && hi >= 0 && lt == TY_STRING && ht == TY_STRING)
      return TY_STR_RANGE;
    return TY_RANGE;
  }
  /* A splat inside an array literal (`[*0..10]`, `[*arr]`) contributes the
     element type of the splatted collection, so the literal stays a typed
     array instead of widening to poly_array. The value returned here is the
     would-be element type, which the ArrayNode arm unifies in. */
  if (nk == NK_SplatNode) {
    int inner = nt_ref(nt, id, "expression");
    if (inner < 0) return TY_UNKNOWN;
    const char *ity = nt_type(nt, inner);
    if (ity && sp_streq(ity, "RangeNode")) {
      int lo = nt_ref(nt, inner, "left");
      return (lo >= 0 && infer_type(c, lo) == TY_STRING) ? TY_STRING : TY_INT;
    }
    TyKind it = infer_type(c, inner);
    if (ty_is_array(it)) return ty_array_elem(it);
    return TY_POLY;
  }
  if (nk == NK_LambdaNode)              return TY_PROC;
  /* an assignment expression evaluates to the assigned value -- but codegen
     lowers `x = expr` to `({ lv_x = ...; lv_x; })`, so the chain value IS the
     slot. Return the local's slot type (when known) so a chained `a = b = expr`
     boxes consistently with the slot, mirroring the ivar-write rule below. */
  if (nk == NK_LocalVariableWriteNode) {
    const char *lwn = nt_str(nt, id, "name");
    Scope *lws = comp_scope_of(c, id);
    LocalVar *lwv = lwn ? scope_local(lws, lwn) : NULL;
    if (lwv && lwv->type != TY_UNKNOWN) return lwv->type;
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_InstanceVariableWriteNode ||
      nk == NK_InstanceVariableOrWriteNode ||
      nk == NK_InstanceVariableAndWriteNode ||
      nk == NK_InstanceVariableOperatorWriteNode) {
    /* expression evaluates to the ivar slot's type (same as a read): codegen
       lowers `@a = expr` to `({ iv_a = ...; iv_a; })`, so the chain value IS the
       slot, and inference must match to keep `@x = @a = expr` boxing consistent. */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* inside an instance_eval/exec splice the block scope has no class_id; the
       ivar belongs to the rebound receiver class (an_ie_class_id). */
    int wcls = s->class_id >= 0 ? s->class_id : an_ie_class_id;
    if (wcls < 0) return infer_type(c, nt_ref(nt, id, "value"));
    ClassInfo *ci = &c->classes[wcls];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (nk == NK_LocalVariableOperatorWriteNode) {
    const char *nm2 = nt_str(nt, id, "name");
    Scope *s2 = comp_scope_of(c, id);
    LocalVar *lv2 = nm2 ? scope_local(s2, nm2) : NULL;
    TyKind ct2 = lv2 ? lv2->type : TY_UNKNOWN;
    TyKind vt2 = infer_type(c, nt_ref(nt, id, "value"));
    if (ct2 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct2) && ty_is_numeric(vt2))
      return (ct2 == TY_FLOAT || vt2 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct2 != TY_UNKNOWN ? ct2 : vt2;
  }
  if (nk == NK_LocalVariableOrWriteNode || nk == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
  }

  if (nk == NK_LocalVariableReadNode) {
    /* a `return .. if p.nil?`-guarded param read: the non-nil type (#1661) */
    if (c->nilnarrow[id] != TY_UNKNOWN) return c->nilnarrow[id];
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* &block param that escapes (not yield-inlined): the LocalVar slot type is
       TY_UNKNOWN, but the value is a Proc object when the method does not inline
       the block (yields==0). Return TY_PROC so callers can type the return value. */
    if (nm && s && s->blk_param && s->blk_param[0] && sp_streq(nm, s->blk_param)
        && !s->yields)
      return TY_PROC;
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    /* an UNMARKED read of a (phase-3-promoted) mutable string demotes to the
       plain string type: ordinary consumers see the copy-read value exactly
       as before promotion (#3227) */
    if (lv) {
      TyKind lt = lv->type == TY_STRBUF ? TY_STRING : lv->type;
      /* Reset-and-re-derive leaves a local UNKNOWN until its own write is
         reached, so a read that precedes it in node order answered UNKNOWN and
         the enclosing unify dropped the arm: `v = if c then 9 else (t = s.upcase; t) end`
         typed v Integer, and the String was converted into an int slot. The
         previous round's answer is the honest one to carry here; hoisting the
         same write out of the arm already gave the right type, which is what
         made the defect depend on node order rather than on the program. */
      /* Only a CONCRETE carry. A stale poly would widen where the old
         behaviour let the other arm's concrete type stand, and that is a
         direction this must not move in: two optcarrot float locals went from
         sp_float to sp_RbVal on it. UNKNOWN is what those sites answered
         before, so declining leaves them exactly as they were. */
      if (lt == TY_UNKNOWN && g_infer_write_round && !lv->is_param &&
          !lv->is_block_param && (TyKind)lv->gc_root != TY_UNKNOWN &&
          (TyKind)lv->gc_root != TY_POLY)
        return (TyKind)lv->gc_root;
      return lt;
    }
    return TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    /* $stdin is the IO handle over the C stdin stream (gets/read/tty?/... via
       the TY_IO dispatch). $stdout/$stderr keep their dedicated AST-shape
       emission arm and stay untyped here. */
    if (nm && (sp_streq(nm, "$stdin") || sp_streq(nm, "$stdout") ||
               sp_streq(nm, "$stderr"))) return TY_IO;   /* the C stream handles (#2818) */
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && sp_streq(nm, "$/")) return TY_STRING;
    if (nm && sp_streq(nm, "$?")) return TY_INT;  /* last child exit status */
    if (nm && (sp_streq(nm, "$PROGRAM_NAME") || sp_streq(nm, "$0"))) return TY_STRING;
    if (nm && sp_streq(nm, "$!")) return TY_EXCEPTION;  /* the exception being handled, or nil (NULL) outside a rescue */
    if (nm && (sp_streq(nm, "$;") || sp_streq(nm, "$,"))) return TY_NIL;
    /* regex match globals: $~ is the last MatchData (NULL = nil); the
       text back-references are nullable strings */
    if (nm && sp_streq(nm, "$~")) return TY_MATCHDATA;
    if (nm && (sp_streq(nm, "$&") || sp_streq(nm, "$`") ||
               sp_streq(nm, "$'") || sp_streq(nm, "$+"))) return TY_STRING;
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableOperatorWriteNode) {
    /* `$g += v` evaluates to the updated value (the local/ivar op-write forms
       above already do; #1484). Plain `$g = v` and `||=`/`&&=` stay untyped
       statements, mirroring the local-variable policy. */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (ct == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct) && ty_is_numeric(vt))
      return (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct != TY_UNKNOWN ? ct : vt;
  }
  if (nk == NK_ConstantReadNode) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    /* a registered constant whose type never settled (e.g. an anonymous
       Struct class assignment) must not shadow the class-table fallbacks
       below -- Pt = Struct.new(:x) reads as the class value, not unknown */
    if (lv && lv->type != TY_UNKNOWN) return lv->type;
    /* `include Math` exposes bare PI/E as Float constants (#2600) */
    if (c->has_include_math && !lv && nm && (sp_streq(nm, "PI") || sp_streq(nm, "E")))
      return TY_FLOAT;
    if (nm && (sp_streq(nm, "RUBY_DESCRIPTION") || sp_streq(nm, "RUBY_VERSION") ||
               sp_streq(nm, "RUBY_PLATFORM") || sp_streq(nm, "RUBY_ENGINE") ||
               sp_streq(nm, "RUBY_ENGINE_VERSION") || sp_streq(nm, "RUBY_RELEASE_DATE") ||
               sp_streq(nm, "RUBY_REVISION") || sp_streq(nm, "RUBY_COPYRIGHT"))) return TY_STRING;
    if (nm && sp_streq(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && sp_streq(nm, "ARGF")) return TY_ARGF;
    /* STDOUT/STDERR/STDIN are IO handles wrapping the C standard streams, so
       puts/print/write/flush -- and gets/read/tty? for STDIN -- route through
       the existing TY_IO dispatch. */
    if (nm && (sp_streq(nm, "STDOUT") || sp_streq(nm, "STDERR") ||
               sp_streq(nm, "STDIN"))) return TY_IO;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* `OpenStruct` as a value (o.class == OpenStruct) resolves only under
       require "ostruct" (#3155). */
    if (nm && sp_streq(nm, "OpenStruct") && sp_feature_required("ostruct")) return TY_CLASS;
    return TY_UNKNOWN;
  }
  if (nk == NK_DefinedNode) return TY_STRING;  /* a label string, or nil (NULL) */
  if (nk == NK_NumberedReferenceReadNode) return TY_STRING;  /* $1..$9: capture, or nil (NULL) */
  if (nk == NK_BackReferenceReadNode) return TY_STRING;  /* $&/$`/$'/$~/$+: nullable string */
  if (nk == NK_ConstantPathNode) {
    /* M::CONST -> resolve by the final path component (constants register
       under their unqualified name) */
    const char *nm = nt_str(nt, id, "name");
    /* An ffi_const is parent-qualified; resolve it BEFORE the leaf-keyed
       plain-constant table, or a same-leaf plain constant in another module
       silently claims the reference (and its type). */
    {
      int fpar = nt_ref(nt, id, "parent");
      const char *fpty = fpar >= 0 ? nt_type(nt, fpar) : NULL;
      /* the qualifying module is the parent's LEAF name, so a nested path
         (Outer::CSql::TEXT) qualifies by CSql -- the same unqualified name
         the ffi decl registered under */
      const char *fpnm = (fpty && (sp_streq(fpty, "ConstantReadNode") ||
                                   sp_streq(fpty, "ConstantPathNode")))
                         ? nt_str(nt, fpar, "name") : NULL;
      if (fpnm && nm)
        for (int fci = 0; fci < c->n_ffi_consts; fci++)
          if (sp_streq(c->ffi_consts[fci].mod, fpnm) &&
              sp_streq(c->ffi_consts[fci].name, nm))
            return TY_INT;
    }
    /* `klass::CONST` on a class VALUE: the constant is whichever the runtime
       class owns, so the type is the one they agree on -- not the leaf-named
       constant's, which is a different constant entirely (#4257). Same
       candidate search the emitter makes. */
    {
      int dpar = nt_ref(nt, id, "parent");
      const char *dpty = dpar >= 0 ? nt_type(nt, dpar) : NULL;
      int dyn = dpar >= 0 && !(dpty && (sp_streq(dpty, "ConstantReadNode") ||
                                        sp_streq(dpty, "ConstantPathNode")));
      /* ...and a constant that HOLDS a class rather than naming one is a
         dynamic receiver too (#4259) */
      if (!dyn && dpar >= 0 && dpty && sp_streq(dpty, "ConstantReadNode")) {
        const char *pn = nt_str(nt, dpar, "name");
        if (pn && comp_class_index(c, pn) < 0 && !is_builtin_class_name(pn) &&
            comp_const(c, pn)) dyn = 1;
      }
      TyKind prt = dyn ? infer_type(c, dpar) : TY_UNKNOWN;
      if (nm && dyn && (prt == TY_CLASS || prt == TY_POLY)) {
        TyKind ct = TY_UNKNOWN; int nc = 0, uniform = 1;
        for (int k = 0; k < c->nclasses; k++) {
          const char *kn = c->classes[k].c_name;
          if (!kn) continue;
          char tail[512];
          snprintf(tail, sizeof tail, "%s__%s", kn, nm);
          size_t tl = strlen(tail);
          for (int ci2 = 0; ci2 < c->nconsts; ci2++) {
            const char *cn2 = c->consts[ci2].name;
            size_t l2 = strlen(cn2);
            if (l2 < tl || strcmp(cn2 + l2 - tl, tail) != 0) continue;
            if (l2 > tl && strncmp(cn2 + l2 - tl - 2, "__", 2) != 0) continue;
            if (nc == 0) ct = c->consts[ci2].type;
            else if (c->consts[ci2].type != ct) uniform = 0;
            nc++;
            break;
          }
        }
        if (nc > 0 && uniform && ct != TY_UNKNOWN) return ct;
      }
    }
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    /* Same guard the bare ConstantReadNode carries: a registered constant
       whose type never settled (Block = Struct.new(:kind), which registers
       the name before the anonymous class exists) must not shadow the
       class-table fallback below. Without it `Probe::Block` read as a value
       is TY_UNKNOWN, the slot is sp_RbVal, and boxing an unknown kind takes
       the nil tail -- so the class id is emitted and then discarded by a
       comma expression. The bare form was already guarded; only the
       qualified path was not (#4271). */
    if (lv && lv->type != TY_UNKNOWN) return lv->type;
    /* A top-level scoped constant `::Name` (no parent) names the same thing as
       the bare constant `Name`; resolve it as a class when it is one so is_a?,
       case/when, etc. treat `::Integer` exactly like `Integer` (#2683). */
    if (nm && nt_ref(nt, id, "parent") < 0 &&
        (comp_class_index(c, nm) >= 0 || is_builtin_class_name(nm)))
      return TY_CLASS;
    if (nm && sp_streq(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && sp_streq(nm, "ARGF")) return TY_ARGF;
    /* well-known module constants */
    int par_id = nt_ref(nt, id, "parent");
    /* a ::-scoped BUILTIN class (Math::DomainError, Process::Status) is a
       first-class Class value like its bare siblings (#2840) */
    {
      int qpar = par_id;
      const char *qpty = qpar >= 0 ? nt_type(nt, qpar) : NULL;
      const char *qpnm = (qpty && (sp_streq(qpty, "ConstantReadNode") ||
                                   sp_streq(qpty, "ConstantPathNode")))
                         ? nt_str(nt, qpar, "name") : NULL;
      if (qpnm && nm) {
        char qbuf[160];
        snprintf(qbuf, sizeof qbuf, "%s::%s", qpnm, nm);
        if (builtin_class_id(qbuf) != 0) return TY_CLASS;
      }
    }
    const char *par_ty = par_id >= 0 ? nt_type(nt, par_id) : NULL;
    /* the qualifying module is the parent's leaf name, so a nested / root
       path (`::Float::MAX`) qualifies by `Float` too -- match codegen's
       par_nmc, which already accepts a ConstantPathNode parent here */
    const char *par_nm = (par_ty && (sp_streq(par_ty, "ConstantReadNode") ||
                                     sp_streq(par_ty, "ConstantPathNode")))
                         ? nt_str(nt, par_id, "name") : NULL;
    if (par_nm && sp_streq(par_nm, "Float")) {
      if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN") || sp_streq(nm, "EPSILON") ||
                 sp_streq(nm, "INFINITY") || sp_streq(nm, "NAN"))) return TY_FLOAT;
      /* DIG/MANT_DIG/RADIX and the exponent limits are Integer constants */
      if (nm && (sp_streq(nm, "DIG") || sp_streq(nm, "MANT_DIG") || sp_streq(nm, "RADIX") ||
                 sp_streq(nm, "MAX_EXP") || sp_streq(nm, "MIN_EXP") ||
                 sp_streq(nm, "MAX_10_EXP") || sp_streq(nm, "MIN_10_EXP"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Math")) {
      if (nm && (sp_streq(nm, "PI") || sp_streq(nm, "E"))) return TY_FLOAT;
    }
    if (par_nm && sp_streq(par_nm, "Regexp")) {
      if (nm && (sp_streq(nm, "IGNORECASE") || sp_streq(nm, "EXTENDED") ||
                 sp_streq(nm, "MULTILINE"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Encoding") && nm) {
      /* every ALL_CAPS Encoding constant is a boxed Encoding value: the two
         the runtime transcodes and every other name it only carries (see the
         codegen arm) */
      int caps = 1;
      for (const char *q = nm; *q; q++)
        if (!((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) { caps = 0; break; }
      if (caps && *nm) return TY_POLY;
    }
    if (par_nm && sp_streq(par_nm, "File")) {
      if (nm && (sp_streq(nm, "SEPARATOR") || sp_streq(nm, "PATH_SEPARATOR") ||
                 sp_streq(nm, "ALT_SEPARATOR") || sp_streq(nm, "NULL"))) return TY_STRING;
      if (nm && (sp_streq(nm, "RDONLY") || sp_streq(nm, "WRONLY") || sp_streq(nm, "RDWR") ||
                 sp_streq(nm, "CREAT") || sp_streq(nm, "EXCL") || sp_streq(nm, "TRUNC") ||
                 sp_streq(nm, "APPEND") || sp_streq(nm, "NONBLOCK") || sp_streq(nm, "BINARY") ||
                 sp_streq(nm, "LOCK_SH") || sp_streq(nm, "LOCK_EX") || sp_streq(nm, "LOCK_UN") ||
                 sp_streq(nm, "LOCK_NB")))
        return TY_INT;   /* the open(2)/flock(2) flag constants (#2788, #2808) */
    }
    if (par_nm && (sp_streq(par_nm, "IO") || sp_streq(par_nm, "File"))) {
      /* IO#seek whence constants (File inherits them from IO) */
      if (nm && (sp_streq(nm, "SEEK_SET") || sp_streq(nm, "SEEK_CUR") ||
                 sp_streq(nm, "SEEK_END"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Process")) {
      /* clock ids (codegen emits their integer values) */
      if (nm && (sp_streq(nm, "CLOCK_MONOTONIC") || sp_streq(nm, "CLOCK_REALTIME") ||
                 sp_streq(nm, "CLOCK_PROCESS_CPUTIME_ID") || sp_streq(nm, "CLOCK_THREAD_CPUTIME_ID") ||
                 sp_streq(nm, "PRIO_PROCESS") || sp_streq(nm, "PRIO_PGRP") || sp_streq(nm, "PRIO_USER")))
        return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Integer")) {
      if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) return TY_UNKNOWN; /* raises NameError */
    }
    /* Socket::<CONST> is an Integer flag; the runtime resolves its value, so
       an unknown name raises NameError there rather than typing here. */
    if (par_nm && sp_streq(par_nm, "Socket") && sp_feature_required("socket") && nm)
      return TY_INT;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* FFI const: Module::NAME -> int */
    if (par_nm && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (sp_streq(c->ffi_consts[fci].mod, par_nm) &&
            sp_streq(c->ffi_consts[fci].name, nm))
          return TY_INT;
      }
    }
    return TY_UNKNOWN;
  }
  if (nk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    int self_cls = s->class_id;
    /* inside a class method, bare `self` is the Class object (#2443) */
    if (self_cls >= 0 && s->is_cmethod) return TY_CLASS;
    /* `self` inside an instance_eval/exec block is the rebound receiver. */
    if (self_cls < 0) self_cls = (an_ie_class_id >= 0) ? an_ie_class_id : ie_class_of(c, id);
    if (self_cls < 0) return TY_UNKNOWN;
    const char *cn = c->classes[self_cls].name;
    if (sp_streq(cn, "String"))  return TY_STRING;
    if (sp_streq(cn, "Integer")) return TY_INT;
    if (sp_streq(cn, "Float"))   return TY_FLOAT;
    if (sp_streq(cn, "Symbol"))  return TY_SYMBOL;
    if (sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass") || sp_streq(cn, "NilClass")) return TY_BOOL;
    if (sp_streq(cn, "Array"))   return TY_POLY_ARRAY;
    if (sp_streq(cn, "Object"))  return TY_POLY;  /* dynamic: called on any receiver type */
    return ty_object(self_cls);
  }
  if (nk == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cls_id = (s->class_id >= 0) ? s->class_id : an_ie_class_id;
    if (cls_id < 0) cls_id = ie_class_of(c, id);
    if (cls_id < 0) cls_id = comp_class_index(c, "Toplevel");
    if (cls_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    if (iv < 0) return TY_UNKNOWN;
    /* an UNMARKED read of a shared-mutable string slot demotes to the plain
       string type (copy-read), mirroring the local-read demotion (#3227) */
    if (ci->ivar_types[iv] == TY_STRBUF) return TY_STRING;
    return ci->ivar_types[iv];
  }
  if (nk == NK_ClassVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) return TY_UNKNOWN;
    int idx = nm ? comp_cvar_index(&c->classes[cid], nm) : -1;
    return idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableOperatorWriteNode || nk == NK_ClassVariableWriteNode ||
      nk == NK_ClassVariableOrWriteNode || nk == NK_ClassVariableAndWriteNode) {
    /* `@@x op= v` / `@@x = v` / `@@x ||= v` / `@@x &&= v` write the cvar and yield
       the stored value, which the codegen coerces to the cvar's slot type (poly
       when widened under promote) -- so the expression's type is the cvar's, not
       v's. (For a non-widened cvar this equals v's type, so default mode is
       unchanged.) */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    int idx = (cid >= 0 && nm) ? comp_cvar_index(&c->classes[cid], nm) : -1;
    if (idx >= 0) return c->classes[cid].cvar_types[idx];
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_IndexOrWriteNode || nk == NK_IndexAndWriteNode ||
      nk == NK_IndexOperatorWriteNode) {
    /* all three yield the slot's (post-write) value, so the expression's type
       is the slot type -- op-write included (`a[i] += x` used as a value, e.g.
       the tail of a block whose proc result is consumed). */
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) return TY_UNKNOWN;
    TyKind rt = infer_type(c, recv);
    if (ty_is_array(rt)) return ty_array_elem(rt);
    if (ty_is_hash(rt)) return ty_hash_val(rt);
    return TY_POLY;
  }
  if (nk == NK_ParenthesesNode) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_NIL;
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_StatementsNode) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_CaseNode) {
    /* value = unify of each when's body; a missing else means a no-match
       falls through to nil */
    int nw = 0; const int *whens = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, whens[w], "statements");
      if (stmts_diverge(c, st)) continue;
      r = ty_unify(r, an_branch_ty(c, st));
    }
    if (else_c >= 0) {
      int st = nt_ref(nt, else_c, "statements");
      if (!stmts_diverge(c, st)) r = ty_unify(r, an_branch_ty(c, st));
    }
    else r = ty_unify(r, TY_NIL);
    return r;
  }
  if (nk == NK_CaseMatchNode) {
    /* case X; in PATTERN; ... — value = unify of each arm's body (+ else). */
    int nw = 0; const int *conds = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, conds[w], "statements");
      if (stmts_diverge(c, st)) continue;
      r = ty_unify(r, an_branch_ty(c, st));
    }
    if (else_c >= 0) {
      int st = nt_ref(nt, else_c, "statements");
      if (!stmts_diverge(c, st)) r = ty_unify(r, an_branch_ty(c, st));
    }
    return r;
  }
  if (nk == NK_IfNode || nk == NK_UnlessNode) {
    int is_unless = nk == NK_UnlessNode;
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    /* a statically-answered defined? predicate types as its live arm alone
       (the dead arm is never emitted -- see the codegen folds) */
    int dpred = nt_ref(nt, id, "predicate");
    int df = comp_defined_guard_false(c, dpred);
    int dt = df ? 0 : comp_defined_guard_true(c, dpred);
    if (df || dt) {
      int take_then = is_unless ? df : dt;
      if (take_then) return then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
      return else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    }
    int tdiv = stmts_diverge(c, branch_stmts(c, then_b));
    int ediv = stmts_diverge(c, branch_stmts(c, else_b));
    /* both arms diverging leaves nothing to type: fall through to the plain
       unify rather than answering UNKNOWN out of nowhere */
    if (tdiv && !ediv) return an_branch_ty(c, else_b);
    if (ediv && !tdiv) return an_branch_ty(c, then_b);
    TyKind tt = an_branch_ty(c, then_b);
    TyKind et = an_branch_ty(c, else_b);
    return ty_unify(tt, et);
  }
  if (nk == NK_ElseNode) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (nk == NK_ArrayNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* An empty `[]` consumed directly as a receiver or interpolation has no
         writes to infer from, so mark_empty_array_operands types it as a poly
         array. An argument may instead take a specific layout through arr_want.
         Elsewhere it stays UNKNOWN so `x = []; x << 1` can back-fill its kind. */
      if (c->empty_arr_recv && id < c->node_cap && c->empty_arr_recv[id])
        return TY_POLY_ARRAY;
      /* kind fixed by the use context (mark_empty_array_operands) */
      if (c->arr_want && id < c->node_cap && ty_is_array(c->arr_want[id]))
        return c->arr_want[id];
      return TY_UNKNOWN;  /* empty: element type comes from usage */
    }
    TyKind e = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      TyKind et = infer_type(c, els[k]);
      /* A nested container literal whose own type is still open (an empty
         `[]` / `{}` element) is a non-scalar value all the same: treat it as
         poly, exactly like the HashNode arm below does for `{}`. Otherwise
         UNKNOWN unifies away and `[[], 1]` collapses to an IntArray, whose
         emit pushes the nested array POINTER as an int element (silent
         garbage). */
      if (et == TY_UNKNOWN) {
        const char *ety = nt_type(nt, els[k]);
        if (ety && (sp_streq(ety, "ArrayNode") || sp_streq(ety, "HashNode") ||
                    sp_streq(ety, "KeywordHashNode")))
          et = TY_POLY;
      }
      e = ty_unify(e, et);
    }
    /* ty_array_of holds an all-unknown element type at bottom while the
       fixpoint runs; see its TY_UNKNOWN case. */
    return ty_array_of(e);
  }
  if (nk == NK_HashNode || nk == NK_KeywordHashNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* a bare `{}` used directly as a hash block-method receiver dispatches as
         the STR_POLY hash (mirrors the empty-array-receiver mark) (#2336). */
      if (c->empty_hash_recv && id < c->node_cap && c->empty_hash_recv[id])
        return TY_STR_POLY_HASH;
      /* an empty literal whose use context fixes a variant (compared against a
         hash-typed peer) adopts it, so both sides share a representation and
         the comparison is a real content check (#3040). */
      /* A literal that is ALSO passed to a user method has to be wide enough
         for whatever the callee writes, which the key context cannot see. The
         arg rule therefore wins over the key context rather than the other way
         round: narrowing here left the caller's `{}` and the callee's
         parameter naming different C structs for the same object (#3386). */
      int eh_arg = c->empty_hash_arg && id < c->node_cap && c->empty_hash_arg[id];
      if (eh_arg) return TY_POLY_POLY_HASH;
      if (c->hash_want && id < c->node_cap && ty_is_hash(c->hash_want[id]))
        return c->hash_want[id];
      return TY_UNKNOWN;
    }
    /* A literal whose local is later given a key of another type has to hold
       both, exactly as a mixed-key literal does (#3927). */
    if (c->hash_want && id < c->node_cap && c->hash_want[id] == TY_POLY_POLY_HASH)
      return TY_POLY_POLY_HASH;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (aty && sp_streq(aty, "AssocSplatNode")) {
        /* `{ **h, ... }`: merge the spread source's key/value types so the
           rebuilt literal keeps a concrete typed-hash variant instead of
           erasing to UNKNOWN. */
        int src = nt_ref(nt, els[k], "value");
        TyKind sh = src >= 0 ? infer_type(c, src) : TY_UNKNOWN;
        if (ty_is_hash(sh)) {
          kt = ty_unify(kt, ty_hash_key(sh));
          vt = ty_unify(vt, ty_hash_val(sh));
        }
        else if (sh == TY_POLY) {
          /* a poly spread source (a hash reached through a poly binding) merges
             at runtime into a fully-poly hash. */
          kt = ty_unify(kt, TY_POLY);
          vt = ty_unify(vt, TY_POLY);
        }
        else {
          return TY_UNKNOWN;  /* unresolved or non-hash splat */
        }
        continue;
      }
      if (!aty || !sp_streq(aty, "AssocNode")) return TY_UNKNOWN;
      kt = ty_unify(kt, infer_type(c, nt_ref(nt, els[k], "key")));
      int vnode = nt_ref(nt, els[k], "value");
      TyKind vt_elem = infer_type(c, vnode);
      /* A nested hash/array literal whose element kind is unresolved (a bare
         `{}` or `[]`) is still a non-scalar value; treat it as poly so the
         outer hash promotes to a poly-valued variant rather than erasing the
         whole hash to UNKNOWN (which would reject `{ "k" => [] }`). */
      if (vt_elem == TY_UNKNOWN) {
        const char *vnode_ty = nt_type(nt, vnode);
        if (vnode_ty && (sp_streq(vnode_ty, "HashNode") || sp_streq(vnode_ty, "KeywordHashNode") ||
                         sp_streq(vnode_ty, "ArrayNode")))
          vt_elem = TY_POLY;
      }
      vt = ty_unify(vt, vt_elem);
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    if (hv != TY_UNKNOWN) return hv;
    /* No scalar (key,val) variant: the value is poly-stored (a nested hash/
       array/object, or a mix). The key type still selects the hash variant --
       string keys stay a str-keyed poly hash rather than collapsing to a
       fully-poly-keyed one, so the literal matches a `Hash[String, untyped]`
       (StrPolyHash) parameter without a layout-mismatching pointer cast. */
    if (vt != TY_UNKNOWN) {
      if (kt == TY_STRING) return TY_STR_POLY_HASH;
      return TY_POLY_POLY_HASH;
    }
    return hv;
  }
  if (nk == NK_DefNode) return TY_SYMBOL;  /* `def` evaluates to :name */
  if (nk == NK_CallOrWriteNode || nk == NK_CallAndWriteNode) {
    /* `a.v ||= x` evaluates to the attribute's (assigned-or-existing) value:
       the backing ivar's type when the receiver class is known. */
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    TyKind rt2 = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
    if (attr && ty_is_object(rt2)) {
      int cid2 = ty_object_class(rt2);
      /* An explicit `def` reader or writer is a method call, not an ivar
         touch: the value is the reader's answer or the assigned value, and
         neither need be the backing ivar's type (`def v=(x); @v = x * 10; end`
         stores an Integer while the expression answers what was assigned).
         Unify the two arms, the way the emitter does (#4148). */
      int rmi2 = -1, wmi2 = -1;
      int rk2 = comp_resolve_member(c, cid2, attr, 0, NULL, &rmi2);
      int wk2 = comp_resolve_member(c, cid2, attr, 1, NULL, &wmi2);
      if (rk2 == SP_MEMBER_METHOD || wk2 == SP_MEMBER_METHOD) {
        int v3 = nt_ref(nt, id, "value");
        TyKind at = v3 >= 0 ? infer_type(c, v3) : TY_UNKNOWN;
        TyKind rr = (rk2 == SP_MEMBER_METHOD && rmi2 >= 0) ? (TyKind)c->scopes[rmi2].ret : TY_UNKNOWN;
        if (rr == TY_UNKNOWN || rr == TY_VOID) return at;
        if (at == TY_UNKNOWN) return rr;
        return ty_unify(rr, at);
      }
      char ivn2[300]; snprintf(ivn2, sizeof ivn2, "@%s", attr);
      int ii2 = comp_ivar_index(&c->classes[cid2], ivn2);
      if (ii2 >= 0) return ivar_value_ty(&c->classes[cid2], ii2);
    }
    int v2 = nt_ref(nt, id, "value");
    return v2 >= 0 ? infer_type(c, v2) : TY_UNKNOWN;
  }
  if (nk == NK_NextNode) {
    /* `next v` produces the BLOCK's value: its type is v's type (nil when
       bare). Leaving it untyped made a yield whose block ends in `next v`
       infer nil, so the delivered value was discarded at the call site. */
    int nargs = nt_ref(nt, id, "arguments");
    int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
    if (nvc > 0) {
      const char *aty = nt_type(nt, nv[0]);
      /* `next *x` delivers the splat-built ARRAY (the SplatNode arm above
         answers with the ELEMENT type, for array-literal splices). */
      if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
      return infer_type(c, nv[0]);
    }
    return TY_NIL;
  }
  if (nk == NK_YieldNode) {
    int ymi = (int)(comp_scope_of(c, id) - c->scopes);
    /* In a proc form the block is a real proc, so the yield is a call on it:
       poly, uniformly, whatever any individual call site's block answers. That
       is the whole point of the clone -- everything the yield feeds widens with
       it, so one body serves every site (#3399). */
    if (getenv("SP_DBG_PF2")) fprintf(stderr, "[y] node=%d scope=%d pf=%d name=%s\n", id, ymi, (ymi>=0&&ymi<c->nscopes)?c->scopes[ymi].is_proc_form:-1, (ymi>=0&&ymi<c->nscopes&&c->scopes[ymi].name)?c->scopes[ymi].name:"?");
    if (ymi >= 0 && ymi < c->nscopes && c->scopes[ymi].is_proc_form) return TY_POLY;
    /* When the block value diverges across call sites (string block at one,
       int at another) AND this yield is the value of an assignment (its result
       flows into a LOCAL), the local settles its type from the first site and
       the other site miscompiles into that slot. Type the yield poly so the
       local is a boxed carrier and each inlined site boxes its own value. A
       bare-yield tail is handled per-site by emit_block_invoke_coerced /
       method_call_ret and must keep its concrete first-site type. */
    if (yield_value_diverges(c, ymi)) {
      for (int w = 0; w < nt->count; w++) {
        NodeKind wk = nt_kind(nt, w);
        if ((wk == NK_LocalVariableWriteNode || wk == NK_LocalVariableOperatorWriteNode ||
             wk == NK_LocalVariableOrWriteNode || wk == NK_LocalVariableAndWriteNode) &&
            nt_ref(nt, w, "value") == id) return TY_POLY;
      }
      /* A yield whose value leaves through an ENSURE frame is in the same
         position as one written to a local, for the same reason: the frame
         carries the value in a slot of its own, and that slot settles its type
         at whichever call site is analyzed first. The per-site coercion the
         comment above relies on handles the method's own tail, and does not
         reach a tail one frame in. So `def run; begin; yield 7; ensure; nil;
         end; end` answered its first site's type at its second -- `p run { |x|
         x == 7 }` then `p run { |x| x * 3 }` printed true twice, where CRuby
         says true and 21, and a pair whose types do not share a C slot stopped
         the build instead. Poly makes the slot a boxed carrier, and each
         inlined site boxes its own value. */
      for (int w = 0; w < nt->count; w++) {
        if (nt_kind(nt, w) != NK_BeginNode) continue;
        if (nt_ref(nt, w, "ensure_clause") < 0) continue;
        int st = nt_ref(nt, w, "statements");
        if (st < 0) continue;
        int bn = 0; const int *bs = nt_arr(nt, st, "body", &bn);
        if (bs && bn > 0 && bs[bn - 1] == id) return TY_POLY;
      }
    }
    return yield_value_type(c, ymi);
  }
  if (nk == NK_SuperNode || nk == NK_ForwardingSuperNode) {
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
    if (shadow) {
      int mi = comp_method_in_class(c, s->class_id, shadow);
      return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
    }
    const char *uname = comp_prep_user_name(s->name);
    int p = c->classes[s->class_id].parent;
    if (p < 0) return TY_UNKNOWN;
    /* super inside a class method resolves through the parent's CLASS-method
       chain (the instance chain would miss `def self.x` entirely). */
    int mi = s->is_cmethod ? comp_cmethod_in_chain(c, p, uname, NULL)
                           : comp_method_in_chain(c, p, uname, NULL);
    if (mi < 0) return TY_UNKNOWN;
    TyKind sret = (TyKind)c->scopes[mi].ret;
    /* A yielding parent's return is whatever its yield produces, decided per
       call site, so its own `ret` stays unknown. The block reaching it is the
       one this method is called with, so take that value's type. */
    if ((sret == TY_UNKNOWN || sret == TY_VOID) && c->scopes[mi].yields) {
      int smi = (int)(s - c->scopes);
      TyKind yt = yield_value_type(c, smi);
      /* a middle link in a super chain has no call sites of its own */
      if (yt == TY_UNKNOWN || yt == TY_VOID) yt = yield_value_type_via_super(c, smi);
      if (yt != TY_UNKNOWN && yt != TY_VOID) return yt;
    }
    return sret;
  }
  if (nk == NK_AndNode || nk == NK_OrNode) {
    int lnd = nt_ref(nt, id, "left"), rnd = nt_ref(nt, id, "right");
    TyKind lt = infer_type(c, lnd);
    TyKind rt = infer_type(c, rnd);
    if (lt == TY_BOOL && rt == TY_BOOL) return TY_BOOL;
    /* An empty `[]` / `{}` on the right carries no element type of its own, so
       it caches UNKNOWN and ty_unify() drops it, answering the LEFT's type.
       That is what typed `true && []` as BOOL: the array construction was
       then emitted into a bool slot, which the C compiler met as a
       pointer/integer mismatch and, for an Integer left, refused outright.
       Give the literal the container type it is before unifying.

       With a nil (or not-yet-typed) left the answer is the container itself
       rather than the union: `nil_valued || []` is the nil-guard fallback,
       where the literal is the value the expression yields and unifying it
       with nil typed the whole thing nil, so `.size` on it raised
       NoMethodError (#3462). */
    if (rt == TY_UNKNOWN && rnd >= 0) {
      const char *rty = nt_type(nt, rnd);
      int rn = 0;
      if (rty && sp_streq(rty, "ArrayNode")) {
        nt_arr(nt, rnd, "elements", &rn);
        if (rn == 0) rt = TY_POLY_ARRAY;
      }
      else if (rty && sp_streq(rty, "HashNode")) {
        nt_arr(nt, rnd, "elements", &rn);
        if (rn == 0) rt = TY_STR_POLY_HASH;
      }
      if (rt != TY_UNKNOWN && (lt == TY_NIL || lt == TY_UNKNOWN)) return rt;
    }
    return ty_unify(lt, rt);  /* value form: a || b -> common type */
  }
  if (nk == NK_BeginNode) {
    /* value = body value unified with each rescue handler's value. With an
       `else` clause the else's value REPLACES the body's on success, so the
       else is the value source; the body's divergence still counts, since a
       body that raises never reaches the else. Typed from the body alone,
       `begin; :sym; rescue; else; false; end` held false in a Symbol slot and
       read it back as symbol number 0. */
    int body = nt_ref(nt, id, "statements");
    int else_c = nt_ref(nt, id, "else_clause");
    int else_st = else_c >= 0 ? nt_ref(nt, else_c, "statements") : -1;
    int vsrc = else_st >= 0 ? else_st : body;
    TyKind r = vsrc >= 0 ? infer_type(c, vsrc) : TY_NIL;
    if (else_c >= 0 && else_st < 0) r = TY_NIL;   /* an empty else is the value: nil */
    TyKind body_t = r;
    /* a body whose last statement is a bare raise diverges: the begin's value
       comes from the rescue arms alone, so their type must not widen (#2739) */
    if (body >= 0 && nt_ref(nt, id, "rescue_clause") >= 0) {
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      if (bn > 0 && bs) {
        const char *lt = nt_type(nt, bs[bn - 1]);
        const char *lnm = (lt && sp_streq(lt, "CallNode")) ? nt_str(nt, bs[bn - 1], "name") : NULL;
        if (lnm && (sp_streq(lnm, "raise") || sp_streq(lnm, "fail")) &&
            nt_ref(nt, bs[bn - 1], "receiver") < 0)
          r = TY_VOID;
      }
    }
    /* An empty container body reads UNKNOWN for want of an element type, not
       because it diverges. Left as "no value" the begin took the handler's
       type alone, and the array pointer went into an sp_int slot (#3496).
       It carries a value, so the two arms are a union: poly. */
    if (r == TY_UNKNOWN && vsrc >= 0) {
      int bn2 = 0; const int *bs2 = nt_arr(nt, vsrc, "body", &bn2);
      if (bs2 && bn2 > 0 && node_is_empty_container(nt, bs2[bn2 - 1])) r = TY_POLY;
    }
    /* A body that carries a value of a type not settled yet is not the same as
       a body that carries none: taking the handler's type alone made the whole
       begin an Integer when the body answered an array whose local had not been
       typed at this point in the walk, and the array was coerced into an int
       slot (#3708). Stay UNKNOWN and let a later round settle it. */
    if (r == TY_UNKNOWN && vsrc >= 0) {
      int bn3 = 0; const int *bs3 = nt_arr(nt, vsrc, "body", &bn3);
      /* only for a local read: its slot is typed by a write that comes later
         in the walk, so within this round it reads UNKNOWN however concrete it
         really is. Anything else that answers UNKNOWN here genuinely has no
         value, and the handler's type is the right answer for it. */
      if (bs3 && bn3 > 0 && nt_kind(nt, bs3[bn3 - 1]) == NK_LocalVariableReadNode)
        return TY_UNKNOWN;
      /* A call ON such a local reads UNKNOWN for the same reason: `a << 2`
         answers the array `a`, whose slot the write earlier in this very body
         types only later in the walk. Taking the handler's type made the whole
         begin a Class, and the boxed value was assigned to an sp_Class (#3867).
         The two arms are a union, so poly is the honest answer -- and it is
         also right for the unresolved call this shape covers, whose raise
         leaves only the handler's (boxed-compatible) value. */
      if (bs3 && bn3 > 0 && nt_kind(nt, bs3[bn3 - 1]) == NK_CallNode) {
        int lrcv = nt_ref(nt, bs3[bn3 - 1], "receiver");
        if (lrcv >= 0 && nt_kind(nt, lrcv) == NK_LocalVariableReadNode) r = TY_POLY;
      }
    }
    int have = !(r == TY_UNKNOWN || r == TY_VOID);
    for (int rs = nt_ref(nt, id, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent")) {
      int st = nt_ref(nt, rs, "statements");
      TyKind at = st >= 0 ? infer_type(c, st) : TY_NIL;
      /* the same on the handler side: an empty container arm carries a value */
      if (at == TY_UNKNOWN && st >= 0) {
        int an2 = 0; const int *as2 = nt_arr(nt, st, "body", &an2);
        if (as2 && an2 > 0 && node_is_empty_container(nt, as2[an2 - 1])) at = TY_POLY;
      }
      if (at == TY_UNKNOWN || at == TY_VOID) continue;  /* this arm diverges too */
      r = have ? ty_unify(r, at) : at;
      have = 1;
    }
    return have ? r : body_t;
  }
  if (nk == NK_CallNode) return infer_call(c, id);

  if (nk == NK_RescueModifierNode) {
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    TyKind et = e >= 0 ? infer_type(c, e) : TY_NIL;
    TyKind rt = r >= 0 ? infer_type(c, r) : TY_NIL;
    /* a diverging expression (raise, a void writer) yields only the rescue
       arm's value: its type stands. A still-UNRESOLVED try (a mid-fixpoint
       estimate) is different -- committing to the rescue arm's concrete type
       here let a local pin to sp_Class while the try later settled on Integer
       (#3130). Answer the boxed union until the try resolves; if it never
       does (the NoMethodError gate's raise-all token), poly still holds the
       rescue arm's value correctly. */
    if (et == TY_VOID || et == TY_NIL) return rt;
    if (et == TY_UNKNOWN) {
      /* an empty container carries a value; the union of the two arms is poly */
      if (node_is_empty_container(nt, e)) return TY_POLY;
      /* a KNOWN diverging form never produces a try value however the
         fixpoint settles: keep the rescue arm's type for it */
      const char *ety = e >= 0 ? nt_type(nt, e) : NULL;
      if (ety && sp_streq(ety, "CallNode") && nt_ref(nt, e, "receiver") < 0) {
        const char *en = nt_str(nt, e, "name");
        if (en && (sp_streq(en, "throw") || sp_streq(en, "raise") ||
                   sp_streq(en, "exit") || sp_streq(en, "abort") ||
                   sp_streq(en, "exit!")))
          return rt;
      }
      return (rt == TY_UNKNOWN || rt == TY_VOID) ? rt : TY_POLY;
    }
    return ty_unify(et, rt);
  }

  /* MultiWriteNode as expression: value is the RHS array. */
  if (nk == NK_MultiWriteNode)
    return infer_type(c, nt_ref(nt, id, "value"));

  return TY_UNKNOWN;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  /* The face re-inference (infer_call's last resort, and codegen's re-entry)
     asks what one call would be with this receiver pinned to one concrete
     kind (the face table in types.h). Only that receiver node, only for the
     duration, and the cache is left untouched so the receiver's own type is
     unaffected. */
  if (id == g_face_node) return g_face_kind;
  TyKind t = infer_uncached(c, id);
  /* The builtin-only re-derivation (see an_builtin_only) asks what this call
     would be if no user class owned the name. That answer is not the node's
     real type, and neither are the child types derived under it, so the cache
     must not record any of them -- leaving them in made the whole analysis
     order-sensitive (#3459). */
  /* `x&.pred?` on a receiver that may be nil answers nil OR a boolean, and a
     C bool has no nil: the two arms of the emitted guard then disagreed about
     their type and the program did not build (#3899). Such a call is poly.
     A bool is not the only such answer -- a Symbol, a Class, a Rational and a
     Complex have no C nil either, and the guard boxed nil into their slot all
     the same. The two rules in infer_uncached that call a `&.` poly are placed
     by POSITION in the inference chain, so a name resolved before them
     (`class`, `to_sym`) never reached one; this one runs after every arm and
     asks the property that actually decides it: can the answer's C type hold
     a nil? Integer and Float have their sentinels, String and the arrays and
     the reference objects have NULL, so those keep their concrete type and
     the guard uses that nil (the array trio of #3461 is this same rule).
     The receiver's type does not narrow this. A miss on a specialized
     container is the element type's C nil -- a NULL string, SP_INT_NIL --
     not a poly nil, so `h["zz"]&.empty?` reached the guard with a concrete
     receiver and answered `false` where CRuby answers nil (#4070). `&.` is
     the program saying nil is possible; the answer has to be able to hold
     one. */
  if (!an_ty_holds_nil(t) && nt_kind(c->nt, id) == NK_CallNode) {
    const char *sn_op = nt_str(c->nt, id, "call_operator");
    int sn_recv = nt_ref(c->nt, id, "receiver");
    if (sn_op && sp_streq(sn_op, "&.") && sn_recv >= 0) t = TY_POLY;
  }
  if (!an_builtin_only) c->ntype[id] = t;
  return t;
}

/* See analyze.h. Children first, so a parent whose inference reads child
   caches (comp_ntype) sees the refreshed values. */
void infer_subtree(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) infer_subtree(c, nt_ref_at(nt, id, i));
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) infer_subtree(c, ids[k]);
  }
  infer_type(c, id);
}

/* ---- scope assignment ---- */

void scope_add_param(Scope *s, const char *name, int defnode) {
  if (s->nparams % 8 == 0) {
    s->pnames = realloc(s->pnames, sizeof(char *) * (size_t)(s->nparams + 8));
    s->pdefault = realloc(s->pdefault, sizeof(int) * (size_t)(s->nparams + 8));
  }
  s->pdefault[s->nparams] = defnode;
  s->pnames[s->nparams++] = strdup(name);
  if (defnode < 0) s->nrequired = s->nparams;
  LocalVar *lv = scope_local_intern(s, name);
  lv->is_param = 1;
}

/* Collect parameters from a DefNode into scope s. */
