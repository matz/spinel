#include "codegen_internal.h"

Buf expr_buf(Compiler *c, int node) {
  Buf b; memset(&b, 0, sizeof b);
  emit_expr(c, node, &b);
  return b;
}

Buf *g_pre = NULL;

/* SP_COLLECT_ERRORS recovery: in collect mode a codegen gap longjmps back to a
   per-unit recovery point armed by the output driver (codegen.c), so one run
   surfaces every unsupported construct instead of aborting on the first. */
jmp_buf g_unsup_recover;
int g_unsup_armed = 0;
int g_unsup_probe = 0;   /* silent emittability probe: longjmp without printing/exiting */
/* Collection is how every compile runs: a refusal stopped the run at the
   first one, and a program brought over from CRuby learned its gaps one
   compile at a time. Now each unit's refusals are reported and the next
   unit is emitted; the run still fails at the end (codegen_program), and
   nothing is written. SP_COLLECT_ERRORS keeps its old meaning on top: the
   gaps are dropped and the rest is emitted anyway, which is what the
   collect-errors gate and a reduction want. */
int collect_mode(void) { return 1; }
int collect_emit_anyway(void) {
  static int v = -1;
  if (v < 0) v = getenv("SP_COLLECT_ERRORS") ? 1 : 0;
  return v;
}
/* What codegen decided at a node, for --emit-types (#4522): how a call was
   dispatched, whether a block was inlined. Kept only under --emit-types;
   a stamp anywhere else is a no-op, so the emitters say what they did
   without paying for it. */
unsigned char *g_ndecide = NULL;
int g_ndecide_cap = 0;
int g_nd_call_id = -1;
void nd_stamp(int id, int kind) {
  static int on = -1;
  if (on < 0) { const char *et = getenv("SPINEL_EMIT_TYPES"); on = (et && *et) ? 1 : 0; }
  if (!on || id < 0) return;
  if (id >= g_ndecide_cap) {
    int ncap = g_ndecide_cap ? g_ndecide_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ndecide = realloc(g_ndecide, (size_t)ncap);
    memset(g_ndecide + g_ndecide_cap, 0, (size_t)(ncap - g_ndecide_cap));
    g_ndecide_cap = ncap;
  }
  g_ndecide[id] = (unsigned char)kind;
}
/* The def a call was bound to, per node: "Owner#name" / "Owner.name" for a
   method in a class, the bare name for a top-level def; a switch's arms
   accumulate in arm order, comma-separated (a class name carries no comma).
   A consumer resolving go-to-definition took the first DefNode of that name,
   wrong whenever two classes define it (#4557). Same gate as nd_stamp. */
char **g_ndtarget = NULL;
int g_ndtarget_cap = 0;
void nd_callee(Compiler *c, int id, int mi, int owner_ci, int add) {
  static int on = -1;
  if (on < 0) { const char *et = getenv("SPINEL_EMIT_TYPES"); on = (et && *et) ? 1 : 0; }
  if (!on || id < 0 || mi < 0 || mi >= c->nscopes) return;
  Scope *m = &c->scopes[mi];
  if (!m->name || !*m->name) return;
  char ent[256];
  if (owner_ci >= 0 && owner_ci < c->nclasses)
    snprintf(ent, sizeof ent, "%s%s%s", c->classes[owner_ci].name, m->is_cmethod ? "." : "#", m->name);
  else snprintf(ent, sizeof ent, "%s", m->name);
  if (id >= g_ndtarget_cap) {
    int ncap = g_ndtarget_cap ? g_ndtarget_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_ndtarget = realloc(g_ndtarget, (size_t)ncap * sizeof *g_ndtarget);
    memset(g_ndtarget + g_ndtarget_cap, 0, (size_t)(ncap - g_ndtarget_cap) * sizeof *g_ndtarget);
    g_ndtarget_cap = ncap;
  }
  char *cur = g_ndtarget[id];
  if (add && cur) {
    /* a body emitted once per specialization stamps its calls again: one
       entry per arm */
    size_t el = strlen(ent);
    for (const char *p = cur; p; ) {
      const char *q = strchr(p, ',');
      size_t l = q ? (size_t)(q - p) : strlen(p);
      if (l == el && !strncmp(p, ent, el)) return;
      p = q ? q + 1 : NULL;
    }
    size_t cl = strlen(cur);
    cur = realloc(cur, cl + 1 + el + 1);
    cur[cl] = ','; memcpy(cur + cl + 1, ent, el + 1);
    g_ndtarget[id] = cur;
    return;
  }
  free(cur);
  g_ndtarget[id] = strdup(ent);
}
/* Every refusal of this run, for --emit-types and the count at the end. */
SpDiag *g_diags = NULL;
int g_ndiags = 0;
static int g_diags_cap = 0;
static void diag_record(const char *file, int line, const char *msg) {
  if (g_ndiags == g_diags_cap) {
    g_diags_cap = g_diags_cap ? g_diags_cap * 2 : 16;
    g_diags = realloc(g_diags, (size_t)g_diags_cap * sizeof *g_diags);
  }
  g_diags[g_ndiags].file = file ? strdup(file) : NULL;
  g_diags[g_ndiags].line = line;
  g_diags[g_ndiags].msg = strdup(msg);
  g_ndiags++;
}
/* Report one refusal and leave: back to the driver's recovery point when one
   is armed (the unit is abandoned), else out of the process. */
static __attribute__((noreturn)) void unsup_leave(const char *file, int line, const char *msg) {
  diag_record(file, line, msg);
  if (line > 0) fprintf(stderr, "spinel: %s:%d: %s\n", file, line, msg);
  else fprintf(stderr, "spinel: %s\n", msg);
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}
/* The .rb position the parser stamped on `id`, or line 0. */
static const char *unsup_pos(Compiler *c, int id, int *line) {
  *line = (int)nt_int(c->nt, id, "node_line", 0);
  int fid = (int)nt_int(c->nt, id, "node_file", 0);
  const char *file = nt_file_path(c->nt, fid);
  if (!file || !*file) file = c->nt->source_file;
  if (!file || !*file) file = "source.rb";
  return file;
}

/* A receiver an arm passes straight into a C call beside an argument is held
   by nothing while that argument runs, nor while the call itself allocates.
   This opens a statement expression that binds the receiver to a rooted temp
   and names that temp in rb; the caller emits the call reading rb and closes
   with "; })" when this returns 1. When this receiver node is in the
   override table (g_argov_node), the operand-order rewrite or an arm that
   re-dispatches through a temp has already declared and rooted the temp in
   front of the call, so rb receives the receiver as rendered and nothing is
   opened. boxed renders through emit_boxed. The caller frees rb. */
int hold_recv_open(Compiler *c, int recv, int boxed, const char *ctype, const char *rootm,
                   Buf *b, Buf *rb) {
  memset(rb, 0, sizeof *rb);
  int bound = 0;
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == recv) bound = 1;
  if (bound) {
    if (boxed) emit_boxed(c, recv, rb); else emit_expr(c, recv, rb);
    if (!rb->p) buf_putn(rb, "", 0);
    return 0;
  }
  Buf rx; memset(&rx, 0, sizeof rx);
  if (boxed) emit_boxed(c, recv, &rx); else emit_expr(c, recv, &rx);
  int t = ++g_tmp;
  buf_printf(b, "({ %s _t%d = %s; %s(_t%d); ", ctype, t, rx.p ? rx.p : "", rootm, t);
  buf_printf(rb, "_t%d", t);
  free(rx.p);
  return 1;
}

void buf_putn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
/* Remove `n` bytes at `off`. Used to take back a prologue line the emitter had
   to write before it could know whether the body would need it. */
void buf_erase(Buf *b, size_t off, size_t n) {
  if (off > b->len || n == 0) return;
  if (off + n > b->len) n = b->len - off;
  memmove(b->p + off, b->p + off + n, b->len - off - n);
  b->len -= n;
  b->p[b->len] = '\0';
}
void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp)) { buf_putn(b, tmp, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_putn(b, big, (size_t)n); free(big);
}
int  g_indent = 0;
/* Argument-hoist overrides: emit_args_filled pre-evaluates GC-hazardous
   call arguments into rooted temps; emit_expr then substitutes the temp
   name when it reaches the overridden node. */
int  g_argov_node[MAX_ARG_OVERRIDE];
char g_argov_text[MAX_ARG_OVERRIDE][16];
int  g_n_argov = 0;
int  g_setter_stmt_id = -1;
/* Node id whose safe-nav (&.) guard is already emitted; the re-entrant
   emit_call skips the guard block for exactly this node. */
int  g_sn_skip = -1;
/* poly-dispatch builtin-arm re-entry marker: the call node whose dispatch is
   currently emitting its builtin-container arm, so the re-entered emission
   does not build the same dispatch again (#3459). */
int  g_pd_skip = -1;
/* Node whose Class-tag dispatch is emitting its non-Class arm, so the
   re-entered emission takes the ordinary path instead of rebuilding it. */
int  g_cls_tag_skip = -1;
/* True if evaluating the subtree at `id` may allocate (and so may trigger
   a GC): any call, container literal, lambda, string, symbol or regexp
   interpolation, or a read of the last match qualifies -- `$~` builds its
   MatchData, $` and $' their String; $& and $+ are reads, counted with
   them. */
int subtree_may_allocate(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") || sp_streq(ty, "ArrayNode") ||
      sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode") ||
      sp_streq(ty, "InterpolatedStringNode") || sp_streq(ty, "InterpolatedSymbolNode") ||
      sp_streq(ty, "InterpolatedRegularExpressionNode") || sp_streq(ty, "BackReferenceReadNode") ||
      sp_streq(ty, "LambdaNode") || sp_streq(ty, "SuperNode") ||
      sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "YieldNode"))
    return 1;
  /* Prism reads the match globals as plain globals as often as not. */
  if (sp_streq(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    return nm && (sp_streq(nm, "$~") || sp_streq(nm, "$`") || sp_streq(nm, "$'"));
  }
  /* A NUL-containing (binary) string literal does not lower to an immortal
     rodata pointer: it allocates a heap string via sp_str_from_bytes (every
     evaluation when unfrozen, or once to fill a call-site cache when frozen).
     Either path can trigger a GC, so it must count as an allocating sibling
     so the operand-rooting logic protects a fresh operand next to it. A plain
     (NUL-free) literal is rodata and never allocates. */
  if (sp_streq(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    if (sc && nt_str_len(nt, id, "content") > strlen(sc)) return 1;
    return 0;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_may_allocate(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_may_allocate(nt, ids[j])) return 1;
  }
  return 0;
}
/* True if evaluating the subtree at `id` can be observed by, or can observe,
   a sibling argument's evaluation: any call (a user method, a mutating builtin,
   or an index read of a container someone else may write) or an assignment.
   Ruby fixes argument evaluation to left-to-right; C leaves a call's operand
   order unspecified, so such arguments have to be sequenced into temps. */
/* A builtin operator over scalars (`x + i`, `n < 3`) computes a value and
   touches nothing reachable, so no sibling argument can observe when it ran.
   It is a CallNode all the same, and counting it as an effect sequenced
   `cell_get(cells, x + ix, y + iy)` into temps for an ordering nobody can
   see -- 12% on the life benchmark. Gated on both the result and the
   receiver being scalar, so a user class's own `+` and `Array#<<` are
   effects as before. */
static int call_is_scalar_op(Compiler *c, int id) {
  static const char *const OPS[] = {
    "+","-","*","/","%","**","<",">","<=",">=","==","!=","<=>","&","|","^","<<",">>", NULL };
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm) return 0;
  int hit = 0;
  for (int i = 0; OPS[i] && !hit; i++) if (sp_streq(nm, OPS[i])) hit = 1;
  if (!hit) return 0;
  if (nt_ref(c->nt, id, "block") >= 0) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv), vt = comp_ntype(c, id);
  int scalar_r = (rt == TY_INT || rt == TY_FLOAT || rt == TY_BOOL);
  int scalar_v = (vt == TY_INT || vt == TY_FLOAT || vt == TY_BOOL);
  return scalar_r && scalar_v;
}

int subtree_has_side_effect(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode") ||
      sp_streq(ty, "YieldNode") || strstr(ty, "WriteNode"))
    return 1;
  if (sp_streq(ty, "CallNode") && !call_is_scalar_op(c, id)) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_side_effect(c, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_side_effect(c, ids[j])) return 1;
  }
  return 0;
}
int  g_tmp = 0;
char g_ren_from[MAX_RENAME][96];
char g_ren_to[MAX_RENAME][112];
int  g_nren = 0;
int  g_block_id = -1;
/* comp_ntype's yield hook: while a literal block is spliced, a YieldNode's
   value is THAT block's tail, not the union the node cache holds over every
   call site -- a second site whose block answers another class was compiled
   as the first one's (#3784). Installed by codegen_main; NULL elsewhere. */
int (*sp_yield_site_type_hook)(const Compiler *c, int id, TyKind *out) = NULL;
/* `blk.call(...)` on the enclosing method's own block parameter, which the
   inliner splices exactly as it splices a `yield`. Its cached type is the one
   the proc form has -- a Proc call answers poly -- but the spliced form
   computes whatever the caller's literal block computes, so the two emissions
   of the one node want different types (#3916). */
static int blk_param_call(const Compiler *c, int id) {
  if (!g_block_param_name || !g_block_param_name[0]) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm || !sp_streq(nm, "call")) return 0;
  int r = nt_ref(c->nt, id, "receiver");
  if (r < 0 || nt_kind(c->nt, r) != NK_LocalVariableReadNode) return 0;
  const char *rn = nt_str(c->nt, r, "name");
  return rn && sp_streq(rn, g_block_param_name);
}
/* The cached value type of every `next` leaving the block body `node`: a
   nested loop, block, lambda or def binds its own. TY_UNKNOWN when none.
   The block's value is its tail joined with these, exactly as analyze's
   yield_value_type joins them (block_next_value_ty). */
TyKind block_next_value_ntype(const Compiler *c, int node) {
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
    return c->ntype[av[0]];
  }
  if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode || k == NK_BlockNode ||
      k == NK_LambdaNode || k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode)
    return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind t = block_next_value_ntype(c, nt_ref_at(nt, node, i));
    if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) {
      TyKind t = block_next_value_ntype(c, ids[j]);
      if (t != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? t : ty_unify(r, t);
    }
  }
  return r;
}
int sp_yield_site_type(const Compiler *c, int id, TyKind *out) {
  if (g_block_id < 0 || id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!sp_streq(ty, "YieldNode") &&
      !(sp_streq(ty, "CallNode") && blk_param_call(c, id))) return 0;
  int bbody = nt_ref(c->nt, g_block_id, "body");
  int bn = 0;
  const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
  if (bn <= 0 || !bb) return 0;
  int tail = bb[bn - 1];
  /* A block whose tail is itself a yield (`wrap { yield }`, or the
     `{ |__fwd| yield __fwd }` a named &block forwards as) answers what the
     block one level out answers, the one the splice will run: the same
     one-level rule emit_boxed applies (#4495), so the two agree on the type
     of one node. That tail's own cache is the union over every expansion. */
  if (g_yield_block_fallback >= 0 && nt_type(c->nt, tail) && sp_streq(nt_type(c->nt, tail), "YieldNode")) {
    int fbody = nt_ref(c->nt, g_yield_block_fallback, "body");
    int fn = 0; const int *fb = fbody >= 0 ? nt_arr(c->nt, fbody, "body", &fn) : NULL;
    if (fn > 0 && fb) tail = fb[fn - 1];
  }
  TyKind bt = c->ntype[tail];
  /* only a CONCRETE per-site answer overrides the cache; an unresolved tail
     leaves the node's own (unified) type in place */
  if (bt == TY_UNKNOWN || bt == TY_VOID) return 0;
  /* a `next v` leaves the block with v: the value is the tail OR any next
     (`{ |i| next 7 if i == 1; nil }` is 7 or nil, not nil) */
  {
    TyKind nx = block_next_value_ntype(c, bbody);
    if (nx != TY_UNKNOWN && nx != TY_VOID) bt = ty_unify(bt, nx);
  }
  *out = bt;
  return 1;
}
int  g_yield_block_fallback = -1;
/* rename-table depth at g_block_id's DEFINITION site: the spliced block body
   and its param names resolve against the entries below this mark only; the
   enclosing inline's renames above it must not capture same-named block
   locals (#3281). Paired with g_block_id / g_yield_block_fallback. */
int  g_block_nren = 0;
int  g_yield_block_fallback_nren = 0;
/* see codegen_internal.h */
const char *g_block_owner_param_name = NULL;
const char *g_yield_block_fallback_param_name = NULL;
/* The (g_self, g_self_deref) that were active when the current g_block_id
   was captured -- i.e. the caller context of the innermost yield-method
   inline. A block spliced at a `yield` is caller code: emit_block_invoke
   emits its body under these instead of the inlined method's rebound self
   (`@map.vertices[...]` inside a block passed to an inlined method must
   read the CALLER's @map). Maintained by the inliners exactly like
   g_yield_block_fallback. */
const char *g_yield_self_fallback = NULL;
/* The same three for the block one level further out (g_yield_block_fallback):
   when a spliced block's own `yield` chains to that block, its body runs
   under THIS self, not the spliced block's. Recorded at every inline from
   the level-one values it replaces, and moved down a level as the splice
   moves g_block_id out (emit_block_invoke). Without it a block passed
   through a forwarding method into an inlined callee resolved its bare
   calls against the intermediate receiver's class: `r.cnt { |i| big?(i) }`
   with `def cnt(&) = @items.count(&)` folded `big?` to NoMethodError. */
const char *g_yield_self_fallback2 = NULL;
const char *g_yield_self_deref_fallback2 = NULL;
int g_yield_emitting_class_fallback2 = -1;
const char *g_yield_self_deref_fallback = NULL;
/* Companion to g_yield_self_fallback: the CALLER's emitting-class, so a
   block body spliced into an inlined callee resolves its implicit-self
   calls against the caller's class (the block is caller code). */
int g_yield_emitting_class_fallback = -1;
const char *g_block_param_name = NULL;
/* Inside an Enumerator.new { |y| ... } generator body, the name of the yielder
   block param. A `y << v` / `y.yield(v)` on it lowers to a Fiber.yield. */
const char *g_yielder_name = NULL;
const char *g_self = "self";
/* Member-access operator for `self`: "->" when self is a pointer (the usual
   heap object), "." when emitting a value-type method body (self is a value). */
const char *g_self_deref = "->";
/* When set, emit_inline_call_x binds self from this pre-hoisted expression
   instead of re-emitting the receiver node -- used by the poly-receiver block
   dispatch to bind self to a per-arm cast of the boxed receiver (#2448). */
const char *g_inline_recv_expr = NULL;
int g_inline_recv_class = -1;
int g_class_body_id = -1;
int g_emitting_class_id = -1;
const char *g_dm_subst_name = NULL;
int g_dm_subst_node = -1;
int g_ie_class_id = -1;
int g_ie_discard_value = 0;
const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
const char *g_retry_label = NULL;
int g_redo_stack[64];
int g_redo_depth = 0;
const char *g_loop_break_var = NULL;
/* When a direct instance_exec/eval splice is wrapped in a do{}while(0), this
   holds the C result temp so a top-level `next <v>` captures its value before
   continuing out of the splice (mirrors g_loop_break_var for `break`). */
const char *g_ie_next_var = NULL;
/* C-loop nesting depth within the current function body: `next` emits a plain
   `continue` only when inside a C loop; at depth 0 inside a proc function it
   is the proc's own return (Ruby block semantics), not a loop control. */
int g_c_loop_depth = 0;
int g_in_proc_body = 0;
/* Set while emitting an instance_exec/eval splice whose result temp is poly:
   a `break <v>` / `next <v>` carrying a scalar value must box it to match. */
int g_ie_res_poly = 0;
/* Set while emitting a block body wrapped in a valued-break setjmp scope:
   the C lvalue holding the enclosing scope's SERIAL, so a top-level
   `break <v>` lowers to sp_brk_throw(<serial>, v) rather than a C `break`.
   NULLed when entering a nested C loop (while/for/until/loop) or another
   emission context (method/proc/fiber body, instance_exec splice) whose own
   `break` must not target this scope. Non-lambda procs capture its value at
   creation as their break home. */
const char *g_brk_ser_var = NULL;
/* g_ensure_depth at the enclosing wrapper: a break at the SAME depth has no
   intervening ensure bodies and delivers by same-function `goto` (register-
   safe: a longjmp would roll back register-allocated locals mutated since
   the setjmp -- the known hazard of the catch/throw machinery); a deeper
   break longjmps via sp_brk_throw so the ensures run. */
int g_brk_ensure_base = 0;
/* The break-scope serial var bound to the CURRENT block (g_block_id): saved
   at the call site when a yielding method is inlined, re-installed while the
   block body is spliced at a yield site -- so a `break` in the block targets
   the call that received it even when the yield sits inside the method's own
   loops or nested iterators. Paired with g_yield_block_fallback the same way
   g_block_id is. */
const char *g_block_brk_var = NULL;
const char *g_yield_blk_brk_fallback = NULL;
int g_block_brk_ebase = 0;
int g_yield_blk_brk_efallback = 0;
/* Proc-literal body context: 1 = lambda (break returns from the lambda),
   2 = non-lambda proc with a break (targets its captured home scope). */
int g_proc_body_kind = 0;
/* C expression for the non-lambda proc's captured break-home serial. */
const char *g_proc_brk_home = NULL;
/* The CallNode id currently being emitted as the inner (unwrapped) call of its
   own break wrapper, so the wrapper is not re-entered recursively. */
int g_brk_skip_id = -1;
const char *g_result_var = NULL;
int g_result_poly = 0;
/* The TyKind of the slot g_result_var names, so a tail value can be checked
   against it (a diverging call may carry an unrelated C type). */
TyKind g_result_ty = TY_UNKNOWN;
/* Non-lambda proc `return` support. While emitting a method that owns a
   proc-return frame, g_method_pr_label / g_method_pr_var name the single-exit
   goto label and the value var, so an explicit `return` funnels there (popping
   the frame once) instead of returning directly. While emitting a returning
   proc's body, g_proc_return_home is the C expression for the home frame index
   (read from the proc capture), so `return` longjmps to the home method. */
const char *g_method_pr_label = NULL;
const char *g_method_pr_var = NULL;
const char *g_proc_return_home = NULL;
/* While a constructor's omitted defaults are emitted: the C text of the object
   being built, so a default that reads self (`def initialize(n = config_val)`)
   resolves against the new instance rather than the caller's self. */
const char *g_ctor_self = NULL;
const char *g_ctor_self_deref = NULL;
/* While a dispatch arm binds its method's arguments: the receiver, cast to the
   arm's class, that method's scope, and the expression depth of the arm's call.
   A default the arm itself omits reads self (`def m(x, y = @extra)`) against
   the receiver, not the caller's self; a call nested in an argument sits
   deeper and binds its own. */
const char *g_arm_self = NULL;
const Scope *g_arm_scope = NULL;
int g_arm_depth = -1;
/* Emitting the body of a non-lambda proc created at top level: a `return`
   there is a TOP-LEVEL return, which ends the script (#3663). */
int g_proc_toplevel_return = 0;
/* Number of live setjmp exception frames (begin/rescue) enclosing the
   current emission point. A `return` from inside a try body must pop them
   (sp_exc_top -= N) before leaving -- a stale frame's jmp_buf points into
   a dead C stack frame, and the next raise longjmps into it (doom's
   SoundManager#[] early returns corrupted the stack this way).
   g_method_pr_exc_depth snapshots the depth at the return-funnel target so
   funnel gotos pop only the frames they actually exit. */
int g_exc_frame_depth = 0;
int g_loop_exc_base = 0;        /* frame depth at the innermost C-loop entry */
int g_loop_ensure_base = 0;     /* ensure depth at the innermost C-loop entry */
int g_brk_exc_base = 0;         /* frame depth at the valued-break wrapper */
int g_block_brk_exc_base = 0;   /* ... for yield-block re-entry (mirrors g_block_brk_ebase) */
int g_method_pr_exc_depth = 0;
/* Loop-invariant string-length hoisting: while a loop whose receiver string is
   not mutated in its body is being emitted, g_hoist_len_recv holds that
   receiver's AST local name and g_hoist_len_var the C temp caching its length;
   a matching `s.length`/`s.size` then emits the temp instead of strlen. */
const char *g_hoist_len_var = NULL;
const char *g_hoist_len_recv = NULL;
TyKind g_ret_type = TY_UNKNOWN;
/* The C function being emitted returns void, whatever g_ret_type says the
   Ruby body's value type is. A fiber/thread body is that case: it is
   `static void _fiber_body_N(sp_Fiber *)` and publishes its value through
   _fb->yielded_value, while g_ret_type is TY_POLY so the body's own
   expressions type. `return <value>` emitted into it is a C constraint
   violation, and GCC 14 rejects it. */
int g_c_ret_void = 0;
/* Mirror of the REAL enclosing function's return funnel: yield-method inlining
   overrides g_method_pr_label/-_var/g_ret_type with a per-inline funnel, and a
   spliced block body (which lexically belongs to the real function, so its
   `return` exits that method) restores from these. Set wherever a fresh
   function context installs (or clears) its funnel. */
const char *g_fn_pr_label = NULL;
const char *g_fn_pr_var = NULL;
TyKind g_fn_ret_type = TY_UNKNOWN;
int g_current_scope_is_lowered = 0;
/* the scope being emitted has an --rbs-seeded return type (#3412) */
int g_ret_seeded = 0;
/* The block-param name of the lowered method currently being emitted (its
   declared &block name, or "__yblk__" when the lowering synthesized one);
   NULL outside a lowered-method emission. Read by emit_yblk_ref. */
const char *g_lowered_blk_name = NULL;
/* Set while emitting a lifted Thread/Fiber body that reaches the lowered
   method's block: the block lives in a cell the frame carries. */
int g_yblk_celled = 0;
/* The enclosing lowered context parked across an inline: an inlined callee's
   own yields splice the call-site block, so emit_inline_call_x clears the
   lowered pair for the callee body and parks it here; emit_block_invoke
   restores it around spliced CALLER code (whose yields do belong to the
   lowered method) -- the same discipline as g_yield_self_fallback. */
int g_yield_lowered_fallback = 0;
const char *g_yield_proc_ref_fallback = NULL;
TyKind g_yield_slot_ty_fallback = TY_UNKNOWN;
const char *g_yield_lowered_blk_fallback = NULL;
/* When a yielding method is inlined and its block is a forwarded REAL proc
   (the caller nil-checks its &block, so the block can't be an inlined literal),
   this holds the C expression for that proc; the inlined `yield` calls it via
   sp_proc_call instead of splicing a block body. NULL otherwise. */
const char *g_yield_proc_ref = NULL;
/* The inlined call's return-slot type while g_yield_proc_ref is set: sp_proc_call
   yields poly, but the slot may be concrete (the analyzer typed this forwarding
   context), so a value-position yield unboxes its result to this. */
TyKind g_yield_slot_ty = TY_UNKNOWN;
/* One level further out still, paired with g_yield_self_fallback2: see
   codegen_internal.h. */
const char *g_yield_proc_ref_fallback2 = NULL;
TyKind g_yield_slot_ty_fallback2 = TY_UNKNOWN;
EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
int       g_ensure_depth = 0;
RescueSave g_rescue_save_stack[MAX_ENSURE_DEPTH];
int        g_rescue_save_depth = 0;

/* rescue bodies crossed by an exit to frame-depth pop_base: those entered at or
   deeper than pop_base (their exc_base >= pop_base). */
int rescues_crossed(int pop_base) {
  int k = 0;
  for (int i = 0; i < g_rescue_save_depth; i++)
    if (g_rescue_save_stack[i].exc_base >= pop_base) k++;
  return k;
}
/* Pop the k crossed rescue-body handlers (no frame pop). Used at sites whose
   frame-pop text is special (the begin..ensure deferred-return). */
void emit_cur_exc_restore(Buf *b, int pop_base) {
  int k = rescues_crossed(pop_base);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
int emit_frame_unwind(Buf *b, int pop_base, const char *guard) {
  int pops = g_exc_frame_depth - pop_base;
  int k = rescues_crossed(pop_base);
  if (pops <= 0 && k == 0) return 0;
  if (guard) buf_printf(b, "if (%s) { ", guard);
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
  if (guard) buf_puts(b, "}");
  return 1;
}
Buf g_procs;
Buf g_proc_protos;
Buf g_pd_protos;
Buf g_pd_defs;
int g_proc_counter = 0;
int g_needs_proc_poly_argslot = 0; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
int g_fiber_counter = 0;
char **g_re_src; int *g_re_flg; int g_re_count, g_re_cap;
int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
/* The RegularExpressionNode behind `nid`, or -1 when the pattern is only
   knowable at run time. A bare literal, a constant bound to one
   (`PAT = /re/[.freeze]`, possibly namespaced) and a regex-typed local bound to
   one all resolve.

   One resolver for every caller. re_lit_index, re_lit_src and re_lit_flags each
   carried their own copy and they disagreed about names: re_lit_src did not
   follow a local, so codegen picked the capturing `scan` emit for a pattern
   named by a constant while typing the same call from the non-capturing shape,
   and dropped the capture groups for one named by a local (#3391). */
/* ---- whole-program indexes for codegen (#4966) ----
   Several emit-time questions were answered by walking every node, once per
   method, per scope or per call site: quadratic in program size. The answers
   depend only on the node table, which codegen extends in two places (each
   setting the new node's scope right away) and otherwise only renames call
   names in place, so the indexes are keyed on the table and its counts. */
static const NodeTable *sn_nt; static int sn_count = -1, sn_nscopes = -1;
static int *sn_off, *sn_ids;
/* A caller may be walking a list when a query from inside its loop meets a
   grown table and rebuilds: the previous arrays stay alive one more build. */
static int *sn_off_prev, *sn_ids_prev;
static void sn_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  int ns = c->nscopes, n = nt->count;
  free(sn_off_prev); free(sn_ids_prev);
  sn_off_prev = sn_off; sn_ids_prev = sn_ids;
  sn_off = calloc((size_t)ns + 2, sizeof(int));
  sn_ids = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  for (int id = 0; id < n; id++) {
    int si = c->nscope[id];
    if (si >= 0 && si < ns) sn_off[si + 2]++;
  }
  for (int si = 0; si < ns; si++) sn_off[si + 2] += sn_off[si + 1];
  for (int id = 0; id < n; id++) {
    int si = c->nscope[id];
    if (si >= 0 && si < ns) sn_ids[sn_off[si + 1]++] = id;
  }
  sn_nt = nt; sn_count = n; sn_nscopes = ns;
}
const int *cg_scope_nodes(Compiler *c, int si, int *n) {
  if (sn_nt != c->nt || sn_count != c->nt->count || sn_nscopes != c->nscopes) sn_build(c);
  if (si < 0 || si >= sn_nscopes) { *n = 0; return sn_ids; }
  *n = sn_off[si + 1] - sn_off[si];
  return sn_ids + sn_off[si];
}

static const NodeTable *bo_nt; static int bo_count = -1;
static int *bo_owner;
int cg_block_owner(Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  /* nodes codegen appends only extend the map: each is above every node
     already in it, so the lowest owner of an older block cannot change */
  if (bo_nt == nt && bo_count >= 0 && nt->count > bo_count) {
    int *nb = realloc(bo_owner, sizeof(int) * (size_t)nt->count);
    if (nb) {
      bo_owner = nb;
      for (int i = bo_count; i < nt->count; i++) bo_owner[i] = -1;
      for (int o = bo_count; o < nt->count; o++) {
        int b = nt_ref(nt, o, "block");
        if (b >= 0 && b < nt->count && bo_owner[b] < 0) bo_owner[b] = o;
      }
      bo_count = nt->count;
    }
  }
  if (bo_nt != nt || bo_count != nt->count) {
    free(bo_owner);
    bo_owner = malloc(sizeof(int) * (size_t)(nt->count > 0 ? nt->count : 1));
    for (int i = 0; i < nt->count; i++) bo_owner[i] = -1;
    for (int o = nt->count - 1; o >= 0; o--) {   /* descending: the lowest owner wins */
      int b = nt_ref(nt, o, "block");
      if (b >= 0 && b < nt->count) bo_owner[b] = o;
    }
    bo_nt = nt; bo_count = nt->count;
  }
  return (blk >= 0 && blk < bo_count) ? bo_owner[blk] : -1;
}

struct CgMemoEnt { char *key; int tag; int val; CgMemoEnt *next; };
#define CG_MEMO_BUCKETS 1024
static unsigned cg_memo_hash(const char *s, int tag) {
  unsigned h = 2166136261u ^ (unsigned)tag;
  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}
int cg_memo_get(Compiler *c, CgMemo *m, const char *key, int tag, int *val) {
  /* appended nodes the answers do not depend on (codegen's own to_s calls)
     keep the memo */
  if (m->tab && m->touches && m->nt == c->nt && m->nscopes == c->nscopes && c->nt->count > m->count) {
    int hit = 0;
    for (int id = m->count; id < c->nt->count && !hit; id++) hit = m->touches(c, id);
    if (!hit) m->count = c->nt->count;
  }
  if (m->tab && (m->nt != c->nt || m->count != c->nt->count || m->nscopes != c->nscopes)) {
    for (int b = 0; b < CG_MEMO_BUCKETS; b++)
      for (CgMemoEnt *e = m->tab[b]; e; ) { CgMemoEnt *nx = e->next; free(e->key); free(e); e = nx; }
    free(m->tab); m->tab = NULL;
  }
  if (!m->tab) {
    m->tab = calloc(CG_MEMO_BUCKETS, sizeof *m->tab);
    m->nt = c->nt; m->count = c->nt->count; m->nscopes = c->nscopes;
  }
  for (CgMemoEnt *e = m->tab[cg_memo_hash(key, tag) % CG_MEMO_BUCKETS]; e; e = e->next)
    if (e->tag == tag && sp_streq(e->key, key)) { *val = e->val; return 1; }
  return 0;
}
void cg_memo_put(CgMemo *m, const char *key, int tag, int val) {
  if (!m->tab) return;
  CgMemoEnt *e = malloc(sizeof *e);
  e->key = strdup(key); e->tag = tag; e->val = val;
  unsigned b = cg_memo_hash(key, tag) % CG_MEMO_BUCKETS;
  e->next = m->tab[b]; m->tab[b] = e;
}

static int re_lit_write_node(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  return k == NK_ConstantWriteNode || k == NK_ConstantPathWriteNode || k == NK_LocalVariableWriteNode;
}
int re_lit_node(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "RegularExpressionNode")) return nid;
  int want_const = sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode");
  int want_local = sp_streq(ty, "LocalVariableReadNode") && comp_ntype(c, nid) == TY_REGEX;
  if (!want_const && !want_local) return -1;
  const char *nm = nt_str(nt, nid, "name");
  if (!nm) return -1;
  /* the answer is fixed by (name, kind): one scan per name, not per use */
  static CgMemo memo = { .touches = re_lit_write_node };
  int got;
  if (cg_memo_get(c, &memo, nm, want_const, &got)) return got;
  int found = -1;
  for (int k = 0; k < nt->count && found < 0; k++) {
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
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "RegularExpressionNode")) found = v;
  }
  cg_memo_put(&memo, nm, want_const, found);
  return found;
}
int re_lit_index(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  if (nid < 0) return -1;
  const char *src = nt_str(c->nt, nid, "unescaped");
  if (!src) return -1;
  int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
  for (int i = 0; i < g_re_count; i++)
    if (g_re_flg[i] == flg && sp_streq(g_re_src[i], src)) return i;
  /* A literal is two constants, the pattern and its flags, so whether the
     engine can read it is known here rather than at the program's startup,
     where it used to surface as a RegexpError from a build that had reported
     nothing. CRuby reports it from the parse, as a SyntaxError. Checked once
     per distinct literal, after the table lookup above. */
  {
    const char *err = sp_re_literal_error(src, (int)strlen(src), flg);
    if (err) unsupported_feature(c, nid, err);
  }
  if (g_re_count >= g_re_cap) {
    g_re_cap = g_re_cap ? g_re_cap * 2 : 8;
    g_re_src = realloc(g_re_src, sizeof(char *) * (size_t)g_re_cap);
    g_re_flg = realloc(g_re_flg, sizeof(int) * (size_t)g_re_cap);
  }
  g_re_src[g_re_count] = (char *)src;
  g_re_flg[g_re_count] = flg;
  return g_re_count++;
}
const char *re_lit_src(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  return nid < 0 ? NULL : nt_str(c->nt, nid, "unescaped");
}
/* Prism flags of a statically resolvable regexp (-1 if `nid` is not one). */
int re_lit_flags(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  return nid < 0 ? -1 : (int)nt_int(c->nt, nid, "flags", 0);
}
void emit_interp(Compiler *c, int id, Buf *b);  /* forward */
int emit_interp_append(Compiler *c, int id, const char *open, const char *open_n, Buf *b, int indent);

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && sp_streq(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    /* Emit the interpolated pattern into a local buffer: an embedded call that
       roots its own args pushes those decls to g_pre, which must land as whole
       statements before this temp's decl, not inside its initializer (#1498). */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_interp(c, nid, &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    /* the pattern text may hold a NUL (Regexp.new("a\0b")), and strlen would
       compile only the part before it */
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)sp_str_byte_len(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}
int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->v[i], nm)) return 1;
  return 0;
}
void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
const char *g_cap_struct = NULL;
NameSet *g_cap_names = NULL;
int g_needs_at_exit = 0;
int g_needs_class_machinery = 0;
int g_has_user_global_marks = 0;
/* The distinct out-of-int64 integer LITERALS of this TU. Each was emitted as
   its own sp_bigint_new_str(...) at every use, so one `0xFFFFFFFF00000000`
   written in forty places parsed a decimal string and allocated a Bignum
   forty times over, every time the line ran. An Integer is immutable, so the
   value can be built once per distinct literal and shared: these hold the
   strings, the emitter names slot i, and the slot is filled on first use and
   marked with the other TU globals. */
char **g_bigl_val = NULL;
int g_bigl_n = 0, g_bigl_cap = 0;
int bigl_intern(const char *v) {
  if (!v) return -1;
  for (int i = 0; i < g_bigl_n; i++) if (sp_streq(g_bigl_val[i], v)) return i;
  if (g_bigl_n >= g_bigl_cap) {
    g_bigl_cap = g_bigl_cap ? g_bigl_cap * 2 : 16;
    g_bigl_val = (char **)realloc(g_bigl_val, sizeof(char *) * (size_t)g_bigl_cap);
  }
  g_bigl_val[g_bigl_n] = strdup(v);
  return g_bigl_n++;
}
int g_uses_symbols = 0;
int g_uses_marshal = 0;
int g_emit_sym_rt = 0;
int g_emit_class_names = 0;
int g_emit_obj_dispatch = 0;
int g_uses_program_name = 0;
int g_gen_obj_hash = 0;
int g_gen_obj_to_json = 0;
int g_gen_obj_to_h = 0;
int g_gen_obj_struct_values = 0;
int g_gen_cls_answers = 0;
int g_gen_obj_with = 0;
int g_uses_regex = 0;
int g_uses_argv = 0;
int g_uses_threads = 0;
int g_has_user_cmp = 0;
int g_has_user_binop = 0;
int g_has_user_aset = 0;
TyKind g_ie_next_ty = TY_UNKNOWN;
int g_has_user_coerce = 0;
int g_has_user_to_io = 0;
int g_gen_obj_hashkey = 0;
int g_gen_obj_valeq = 0;
int g_re_init_needed = 0;
/* A value written into a TYPED array's element slot. A value decided at
   run time (poly) goes through the element check, which stores its own kind
   and refuses a foreign one with TypeError rather than coercing it (#4481);
   a statically typed value is emitted as it is (the emitter's own
   int/float/string forms already convert between the numeric kinds). */
void emit_typed_elem_value(Compiler *c, int node, TyKind et, Buf *b) {
  TyKind vt = comp_ntype(c, node);
  if (vt == TY_POLY && (et == TY_INT || et == TY_FLOAT || et == TY_STRING)) {
    buf_printf(b, "sp_poly_elem_%s(", et == TY_INT ? "i" : et == TY_FLOAT ? "f" : "s");
    emit_expr(c, node, b);
    buf_puts(b, ")");
    return;
  }
  if (et == TY_INT) emit_int_expr(c, node, b);
  else if (et == TY_FLOAT) emit_float_expr(c, node, b);
  else emit_expr(c, node, b);
}
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    /* A TY_PROC capture is stored as (sp_int)(uintptr_t)sp_Proc* in the cell.
       Cast it back to sp_Proc* so call sites work. A heap-object cell is a real
       typed pointer, so its deref is already the right lvalue (no cast). */
    LocalVar *clv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
    if (clv && clv->type == TY_PROC)
      buf_printf(b, "(sp_Proc *)(uintptr_t)(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    else
      buf_printf(b, "(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
  if (lv && lv->is_cell) {
    /* Through the rename map, exactly as the plain form below: a method
       INLINED at its call site renames its locals, and the cell form did not
       follow -- the prologue declared `lv__y1_n` while the body read
       `(*_cell_n)`, which nothing declared (#4088). Outside an inline the map
       is empty and this is the name itself. */
    const char *crn = rename_local(name);
    if (lv->type == TY_PROC) buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", crn);
    else buf_printf(b, "(*_cell_%s)", crn);
    return;
  }
  buf_printf(b, "lv_%s", rename_local(name));
}
void emit_yblk_ref(Buf *b) {
  /* The lowered method's block param: the declared &block name when the def
     has one, else the synthetic __yblk__. */
  const char *nm =
      (g_lowered_blk_name && g_lowered_blk_name[0]) ? g_lowered_blk_name : "__yblk__";
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm)) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*(((%s *)_cap)->c_%s))", g_cap_struct, nm);
  }
  /* A yield inside a lifted Thread/Fiber body reaches the forwarded block
     through the shared cell the body's frame carries, not a local (#3355). */
  else if (g_yblk_celled) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", nm);
  }
  else {
    buf_printf(b, "lv_%s", nm);
  }
}
void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}
/* The C representation of Ruby `nil` for a concretely-typed slot (vs
   default_value's zero-value): a fresh block-local starts nil, and several
   types carry an in-band nil sentinel (NULL string, SP_INT_NIL, NaN float,
   (sp_sym)-1). Types with no sentinel fall back to the zero value. */
const char *nil_value(TyKind t) {
  switch (t) {
    case TY_STRING: return "NULL";
    case TY_INT:    return "SP_INT_NIL";
    case TY_FLOAT:  return "sp_float_nil()";
    case TY_POLY:   return "sp_box_nil()";
    default:        return NULL;
  }
}

static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm);
int subtree_has_param_named_pub(const NodeTable *nt, int id, const char *nm) {
  return subtree_has_param_named(nt, id, nm);
}
static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);

  /* numbered params (_1.._9): the NumberedParametersNode carries no child
     parameter nodes, yet the block's locals list contains the names. */
  /* `_1` as the parser wrote it, and `_1__bNN` where scope_numbered_block_params
     gave a colliding scope's blocks their own -- the same two spellings the
     shadow rename leaves for an ordinary parameter, matched the same way. */
  if (ty && sp_streq(ty, "NumberedParametersNode") &&
      nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' &&
      (!nm[2] || !strncmp(nm + 2, "__b", 3))) return 1;
  if (ty && sp_streq(ty, "ItParametersNode") && sp_streq(nm, "it")) return 1;
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    /* shadow-renaming rewrites a param's node-table name to NAME__bpNN;
       the parser's locals list keeps the raw NAME -- match both. */
    if (pn) {
      size_t nl = strlen(nm);
      if (sp_streq(pn, nm)) return 1;
      if (!strncmp(pn, nm, nl) && !strncmp(pn + nl, "__bp", 4)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_param_named(nt, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_param_named(nt, ids[j], nm)) return 1;
  }
  return 0;
}

/* A FRESH cell for a captured block variable, at the top of an invocation:
   the capture struct copies the current cell pointer at proc creation, so
   per-iteration closures each keep their own binding. `cellv` names the
   cell variable -- the frame's `_cell_x`, or the capture slot when the body
   is inlined inside a proc function (#4127). */
static void emit_fresh_cell(Compiler *c, LocalVar *lv, const char *cellv, Buf *b, int indent) {
  emit_indent(b, indent);
  if (lv->type == TY_FLOAT) {
    buf_printf(b, "%s = (sp_float *)sp_gc_alloc(sizeof(sp_float), NULL, NULL); *%s = 0.0;\n", cellv, cellv);
  }
  else if (lv->type == TY_POLY) {
    buf_printf(b, "%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval); *%s = sp_box_nil();\n", cellv, cellv);
  }
  else if (cell_value_struct(lv->type)) {
    /* a by-value struct rides a cell of its own type, as the two cell
       prologues already say; without this arm the reset fell through to
       the sp_int else and assigned an sp_int * to an sp_Class * */
    const char *vs = cell_value_struct(lv->type);
    buf_printf(b, "%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, NULL); *%s = %s;\n",
               cellv, vs, vs, cellv, cell_value_struct_empty(lv->type));
  }
  else if (lv->type != TY_PROC && lv->type != TY_INT && lv->type != TY_BOOL &&
           lv->type != TY_SYMBOL && lv->type != TY_UNKNOWN && cell_is_typed_ptr(c, lv)) {
    const char *cell_scan = cell_scan_fn(lv->type);
    buf_printf(b, "%s = (", cellv); emit_ctype(c, lv->type, b);
    buf_puts(b, " *)sp_gc_alloc(sizeof(");
    emit_ctype(c, lv->type, b);
    buf_printf(b, "), NULL, %s); *%s = NULL;\n", cell_scan, cellv);
  }
  else if (lv->type == TY_PROC) {
    /* an int cell holding a collectable Proc still needs a scan (#4077) */
    buf_printf(b, "%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, sp_cell_scan_procint); *%s = 0;\n", cellv, cellv);
  }
  else {
    buf_printf(b, "%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, NULL); *%s = 0;\n", cellv, cellv);
  }
  /* The fresh cell is young and the capture struct it was just stored
     into can be old (the enclosing proc outlived a promotion), so the
     store is recorded -- after it, since the allocation above can
     collect and clear a record made before it. The `_cell_x` form is
     a frame local and needs nothing. */
  if (cellv[0] == '(') {
    emit_indent(b, indent);
    buf_puts(b, "sp_gc_wb((void *)_cap);\n");
  }
}
/* Which C expression names a scope's cell variable from here. */
static const char *cell_var_name(const char *rn, char *buf, size_t cap) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, rn))
    snprintf(buf, cap, "((%s *)_cap)->c_%s", g_cap_struct, rn);
  else
    snprintf(buf, cap, "_cell_%s", rn);
  return buf;
}
void emit_block_locals_reset(Compiler *c, int blk, Buf *b, int indent) {
  if (blk < 0) return;
  const char *locs = nt_str(c->nt, blk, "locals");
  if (!locs || !*locs) return;
  char tmpn_buf[128];
  const char *p = locs;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l) {
      /* names longer than the stack buffer are legal Ruby; heap-fall back
         rather than silently skipping the reset */
      char *tmpn = l < sizeof tmpn_buf ? tmpn_buf : malloc(l + 1);
      if (!tmpn) break;
      memcpy(tmpn, p, l); tmpn[l] = 0;
      /* Skip every name bound by the block's parameter list, including
         destructured, optional, rest, and shadow (`; x`) declarations --
         collected straight from the parameters subtree, since
         block_param_name only covers plain leading params. */
      int is_param = subtree_has_param_named(c->nt, nt_ref(c->nt, blk, "parameters"), tmpn);

      if (is_param) {
        /* A cell_shadow param is bound by the loop emitters WRITING THE PLAIN C
           SLOT, while every read in the body already goes through the cell. The
           publish used to sit at the capture fill -- the point the lifted proc
           is built -- so every read before that read a cell still holding the
           previous iteration's value, or nil on the first pass. Two nested
           blocks and a `next` were enough to arrange it: doom's build_composite
           saw `pref` as nil on the first patch of every texture and drew
           nothing. The slot is current from the moment the loop writes it, so
           publish here, at the top of the body, which is where the LocalVar
           doc has always said the copy belongs. */
        Scope *psc = comp_scope_of(c, blk);
        LocalVar *plv = psc ? scope_local(psc, tmpn) : NULL;
        if (plv && plv->is_cell && plv->cell_shadow) {
          const char *prn = rename_local(tmpn);
          /* Inlined inside a real proc function the cell is reachable only
             through the capture struct, and the slot is not this frame's at
             all -- there the capture fill is still the only publish. */
          if (!(g_cap_struct && g_cap_names && nameset_has(g_cap_names, prn))) {
            /* A fresh cell first, as a captured block-local gets below: the
               parameter is bound anew by every iteration too, and one shared
               cell had every proc a `&blk` callee stored answer the last
               value the parameter took (#4462). */
            char pcell_buf[160];
            emit_fresh_cell(c, plv, cell_var_name(prn, pcell_buf, sizeof pcell_buf), b, indent);
            emit_indent(b, indent);
            if (plv->type == TY_PROC)
              buf_printf(b, "*_cell_%s = (sp_int)(uintptr_t)lv_%s;\n", prn, prn);
            else
              buf_printf(b, "*_cell_%s = lv_%s;\n", prn, prn);
          }
        }
      }
      else {
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, tmpn) : NULL;
        /* A captured (cell-backed) block-local gets a FRESH cell each
           invocation: the capture struct copies the current cell pointer at
           proc creation, so per-iteration closures each keep their own
           binding -- one shared cell made every closure observe the final
           iteration's value (#3230). The prologue's SP_GC_ROOT registered
           the cell VARIABLE's address, so the reassignment stays rooted;
           earlier cells stay live through their captures' scans. */
        if (lv && lv->is_cell) {
          const char *rn2 = rename_local(tmpn);
          /* The cell VARIABLE is declared in the frame that owns the scope. A
             block body is not always emitted into that frame: when an
             enclosing block became a real proc function, this one is inlined
             INSIDE it, and there the cell is reachable only through the
             capture struct -- `_cell_terms` named nothing the function
             declared and the C build stopped (#4127). Refreshing the capture
             slot is also the right per-iteration semantics: a proc created in
             this iteration copies the slot at creation, so it keeps this
             iteration's binding. */
          char cellv_buf[160];
          const char *cellv = cell_var_name(rn2, cellv_buf, sizeof cellv_buf);
          emit_fresh_cell(c, lv, cellv, b, indent);
        }
        else if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) {
          emit_indent(b, indent);
          /* A value-type object is stored inline (sp_X, not sp_X*), so its
             empty/nil reset is a zeroed struct -- default_value()'s blanket
             "NULL" would assign a pointer to a struct lvalue (#3267). */
          if (ty_is_object(lv->type) && c->classes[ty_object_class(lv->type)].is_value_type) {
            buf_printf(b, "lv_%s = (sp_%s){0};\n", rename_local(tmpn),
                       c->classes[ty_object_class(lv->type)].c_name);
          }
          else {
            const char *nv = nil_value(lv->type);
            if (!nv) nv = lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type);
            buf_printf(b, "lv_%s = %s;\n", rename_local(tmpn), nv);
          }
        }
      }
      if (tmpn != tmpn_buf) free(tmpn);
    }
    if (!e) break;
    p = e + 1;
  }
}

/* The receiver's local name when it is a shared-mutable (TY_STRBUF) string
   local read, else NULL (#3227 shim gate). */
const char *strbuf_local_name(Compiler *c, int recv) {
  if (recv < 0) return NULL;
  const char *rty = nt_type(c->nt, recv);
  if (!rty || !sp_streq(rty, "LocalVariableReadNode")) return NULL;
  const char *rn = nt_str(c->nt, recv, "name");
  Scope *rs = rn ? comp_scope_of(c, recv) : NULL;
  LocalVar *rl = rs ? scope_local(rs, rn) : NULL;
  return (rl && rl->type == TY_STRBUF) ? rn : NULL;
}
/* The owning class slot of an ivar READ node, mirroring the read emitter's
   storage resolution: instance method -> its class; top-level method ->
   the Toplevel pseudo-class; class-method / instance_eval contexts return
   -1 (their storage is not a per-instance field). */
int strbuf_ivar_owner(Compiler *c, int node) {
  Scope *cs = comp_scope_of(c, node);
  if (!cs) return -1;
  if (cs->is_cmethod) return -1;
  if (cs->class_id >= 0) return cs->class_id;
  if (g_ie_class_id >= 0) return -1;
  return comp_class_index(c, "Toplevel");
}
/* Emit-side lvalue for a shared-mutable string receiver: lv_<x> for a
   strbuf local, <self>-><iv_x> (or civ_Toplevel_x) for a strbuf ivar.
   Returns 1 and fills `out`, or 0 when the receiver is neither (#3227). */
/* An empty `[]` / `{}` assigned into a typed slot has to be built at the SLOT's
   representation, not at the literal's own default: the literal carries no
   element or key type, so emit_expr answers an IntArray / StrPolyHash and the
   store lands on a slot of a different C type. The global and constant writes
   spell this out inline; class variables share the rule (#4054). Returns 1
   when it emitted. */
int emit_empty_container_for_slot(Compiler *c, int v, TyKind slot, Buf *b) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  const char *vty = nt_type(nt, v);
  if (!vty) return 0;
  int n = 0;
  if (sp_streq(vty, "ArrayNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    if (slot == TY_POLY_ARRAY) { buf_puts(b, "sp_PolyArray_new()"); return 1; }
    if (array_kind(slot)) { buf_printf(b, "sp_%sArray_new()", array_kind(slot)); return 1; }
    return 0;
  }
  if (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    const char *hcn = ty_is_hash(slot) ? ty_hash_cname(slot) : NULL;
    if (!hcn) return 0;
    buf_printf(b, "sp_%sHash_new()", hcn);
    return 1;
  }
  /* `Hash.new` with no arguments or block is the same empty producer, and the
     typed slot needs the same fresh `sp_XHash_new()` rather than the boxed
     value the untyped call would emit (the global write says this too). */
  if (sp_streq(vty, "CallNode") && ty_is_hash(slot)) {
    const char *cn = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver");
    const char *rn = (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode) ? nt_str(nt, r, "name") : NULL;
    int a = nt_ref(nt, v, "arguments");
    int an = 0; if (a >= 0) nt_arr(nt, v, "arguments", &an);
    if (cn && rn && sp_streq(cn, "new") && sp_streq(rn, "Hash") && an == 0 &&
        nt_ref(nt, v, "block") < 0) {
      const char *hcn = ty_hash_cname(slot);
      if (!hcn) return 0;
      buf_printf(b, "sp_%sHash_new()", hcn);
      return 1;
    }
  }
  return 0;
}

int emit_poly_rhs_coerced(Compiler *c, TyKind slot, int v, Buf *b) {
  /* yield_site_type, not comp_ntype: a `yield` carries the union over every
     call site, and the block spliced HERE may already hand back the scalar
     this slot wants. Coercing that would unbox a value that is not boxed. */
  if (v < 0 || yield_site_type(c, v) != TY_POLY) return 0;
  /* int and string carry the implicit conversion protocol: this narrowing is
     the moment a boxed user object enters a typed slot, and reading it as 0 or
     as its #to_s rendering is the silent wrong answer. bool keeps the plain
     form -- an object in a bool slot is truthy, not a number. */
  /* The int slot needs nothing here: sp_poly_to_i carries the conversion
     protocol in its cold half, so a narrowing is correct for free. The string
     slot cannot -- sp_poly_to_s renders an object through #to_s, which is
     right for interpolation -- so it takes the protocol form, and only where
     the program defines a #to_str to reach: a narrowing lands wherever the
     analysis put it, including a hot loop, and the test is not free there.
     bool keeps the plain form: an object in a bool slot is truthy. */
  /* A nil narrowed into an int or float slot is that slot's nil sentinel, not
     the 0 under the tag (#4288). TY_BOOL keeps the plain form: nil in a bool
     slot is false, and the int sentinel would read truthy. */
  /* A class-typed slot (a parameter an RBS declaration pinned to its class)
     reassigned from a boxed value (`comment = subtree.shift` over a poly
     array) took the raw sp_RbVal and the C did not compile (#4640). The
     checked unbox: the tag and class are verified, nil stays NULL. */
  if (ty_is_object(slot)) {
    Buf e; memset(&e, 0, sizeof e);
    emit_expr(c, v, &e);
    emit_unbox_text(c, slot, e.p ? e.p : "sp_box_nil()", b);
    free(e.p);
    return 1;
  }
  const char *fn = slot == TY_INT   ? "sp_poly_to_i_or_nil"
                 : slot == TY_BOOL  ? "sp_poly_to_i"
                 : slot == TY_FLOAT ? "sp_poly_to_f_or_nil"
                 : slot == TY_STRING
                     ? (prog_has_conv_method(c, "to_str", TY_STRING) ? "sp_poly_arg_str" : "sp_poly_to_s") : NULL;
  if (!fn) return 0;
  buf_printf(b, "%s(", fn); emit_expr(c, v, b); buf_puts(b, ")");
  return 1;
}

/* Emit a shared-mutable string receiver for an operation that only READS its
   bytes: the live buffer, not the whole-buffer copy an ordinary value read
   makes (#3227). Answers 0 when the receiver is not such a slot, so the caller
   falls back to emit_expr. */
int emit_strbuf_read_ref(Compiler *c, int recv, Buf *b) {
  char sref[1024];
  int svm = c->strbuf_box[recv];
  c->strbuf_box[recv] = 1;
  int is_sb = strbuf_slot_ref(c, recv, sref, sizeof sref);
  c->strbuf_box[recv] = (unsigned char)svm;
  if (!is_sb) return 0;
  buf_printf(b, "sp_String_cstr(%s)", sref);
  return 1;
}
/* The object_id of a String held as a shared sp_String: the handle's address,
   which is what a box of it carries. 0 when `recv` is not one. */
int strbuf_object_ref(Compiler *c, int recv, Buf *b) {
  char sref[1024];
  int svm = c->strbuf_box[recv];
  c->strbuf_box[recv] = 1;
  int is_sb = strbuf_slot_ref(c, recv, sref, sizeof sref);
  c->strbuf_box[recv] = (unsigned char)svm;
  if (!is_sb) return 0;
  buf_printf(b, "((sp_int)(uintptr_t)(%s))", sref);
  return 1;
}
/* `cont[k]` where the container hands its elements out BOXED (a poly array, a
   hash): the read is an sp_RbVal, so a shared-handle destination has to unbox
   it rather than wrap it (#3941). */
int strbuf_boxed_elem_read(Compiler *c, int v) {
  if (!container_elem_read_p(c->nt, v)) return 0;
  int r = nt_ref(c->nt, v, "receiver");
  if (r < 0) return 0;
  TyKind rt = comp_ntype(c, r);
  return rt == TY_POLY || rt == TY_POLY_ARRAY || ty_is_hash(rt);
}
/* A String is a const char * value, so a String mutator (`<<`, the bang
   methods, replace/insert/...) is lowered to a reassignment of its receiver:
   `s = sp_str_append_grow(s, x)`. That needs a receiver whose C form is an
   lvalue holding the string: a local or ivar, and equally a global, a class
   variable or a constant (gv_X, cvar_C_X, cst_X). A constant qualifies when
   its read is the plain slot -- the Class.new-guarded read is a conditional
   expression, not an lvalue, and a `klass::NAME` path whose owner is a run-time
   value reads through a switch. Any other receiver has nowhere to put the new
   string. */
int str_mut_var_recv(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  switch (nt_kind(nt, recv)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
  case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode:
    return 1;
  case NK_ConstantReadNode: {
    LocalVar *cv = comp_const(c, nt_str(nt, recv, "name"));
    return cv && cv->type != TY_UNKNOWN && !cv->init_guarded;
  }
  case NK_ConstantPathNode: {
    /* the path's read resolves through several tables; ask it for its text */
    if (comp_ntype(c, recv) != TY_STRING) return 0;
    int save = g_tmp;
    Buf rb = expr_buf(c, recv);
    g_tmp = save;
    const char *t = rb.p ? rb.p : "";
    int ok = strncmp(t, "cst_", 4) == 0 && t[4];
    for (const char *q = t + 4; ok && *q; q++)
      if (!(isalnum((unsigned char)*q) || *q == '_')) ok = 0;
    free(rb.p);
    return ok;
  }
  default:
    return 0;
  }
}
/* A reader call the shared-mutable shim has substituted with its shadow copy
   (through the argument-override table): the value arms may rebind it as they
   would a local. */
int sb_shadowed_reader(int node) {
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == node && strncmp(g_argov_text[i], "lv__sb", 6) == 0) return 1;
  return 0;
}
/* Open the shim over a reader call `recv` that hands out the shared handle:
   the handle's text goes to sref, and until sb_reader_shim_close the call node
   reads as the shadow `lv__sbT`, with the handle marks lifted. Answers T, or 0
   when `recv` is no such call. */
int sb_reader_shim_open(Compiler *c, int recv, char *sref, size_t cap, SbReaderSave *sv) {
  if (recv < 0 || nt_kind(c->nt, recv) != NK_CallNode) return 0;
  if (!c->strbuf_box[recv] && !c->strbuf_handle_demand[recv]) return 0;
  if (g_n_argov >= MAX_ARG_OVERRIDE) return 0;
  if (!strbuf_slot_ref(c, recv, sref, cap)) return 0;
  int tH = ++g_tmp;
  sv->box = c->strbuf_box[recv]; sv->demand = c->strbuf_handle_demand[recv];
  sv->ty = c->ntype[recv];
  c->strbuf_box[recv] = 0; c->strbuf_handle_demand[recv] = 0;
  if (sv->ty == TY_STRBUF) c->ntype[recv] = TY_STRING;
  g_argov_node[g_n_argov] = recv;
  snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "lv__sb%d", tH);
  g_n_argov++;
  return tH;
}
void sb_reader_shim_close(Compiler *c, int recv, const SbReaderSave *sv) {
  g_n_argov--;
  c->strbuf_box[recv] = sv->box; c->strbuf_handle_demand[recv] = sv->demand;
  c->ntype[recv] = sv->ty;
}
const char *g_sb_iv_name = NULL;
int         g_sb_iv_cid  = -1;
char        g_sb_iv_repl[64];
int strbuf_slot_ref(Compiler *c, int recv, char *out, size_t cap) {
  const char *rn = strbuf_local_name(c, recv);
  if (rn) {
    /* via emit_local_ref: a celled/captured local derefs its cell */
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_local_ref(c, recv, rn, &rb);
    snprintf(out, cap, "%s", rb.p ? rb.p : "");
    free(rb.p);
    return 1;
  }
  /* a demand-marked reader call typed as the handle (external reader
     mutation, e.g. `subs[0].topic << x`): the emitted read IS the sp_String*
     expression. Declined when it does not fit the caller's buffer (the
     branches then fall through to the value-form arms). */
  /* strbuf_handle_demand is the same demand carried without the type: a mark
     made after the node-type cache is finalized cannot move the type without
     moving the call off the surface that dispatches it (see compiler.h). */
  if (recv >= 0 && nt_kind(c->nt, recv) == NK_CallNode &&
      ((c->strbuf_box[recv] && comp_ntype(c, recv) == TY_STRBUF) ||
       c->strbuf_handle_demand[recv])) {
    Buf rb2; memset(&rb2, 0, sizeof rb2);
    emit_expr(c, recv, &rb2);
    /* A container ELEMENT read comes back BOXED (a poly array element, a hash
       value), so the handle has to come out of the box; a reader call already
       emits the sp_String * itself (#3941). */
    int erecv = nt_ref(c->nt, recv, "receiver");
    const char *cnm = nt_str(c->nt, recv, "name");
    TyKind ert = erecv >= 0 ? comp_ntype(c, erecv) : TY_UNKNOWN;
    int boxed = cnm && sp_streq(cnm, "[]") &&
                (ert == TY_POLY || ert == TY_POLY_ARRAY || ty_is_hash(ert));
    int fit = rb2.p && strlen(rb2.p) + 24 <= cap;
    if (fit) snprintf(out, cap, boxed ? "sp_poly_as_strbuf(%s)" : "(%s)", rb2.p);
    free(rb2.p);
    return fit;
  }
  if (recv < 0 || nt_kind(c->nt, recv) != NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(c->nt, recv, "name");
  if (!nm) return 0;
  int cid = strbuf_ivar_owner(c, recv);
  if (cid < 0) return 0;
  int iv = comp_ivar_index(&c->classes[cid], nm);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_STRBUF) return 0;
  Scope *cs = comp_scope_of(c, recv);
  if (cs && cs->class_id < 0)
    snprintf(out, cap, "civ_Toplevel_%s", nm + 1);
  else
    snprintf(out, cap, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
  return 1;
}
const char *rename_local(const char *nm) {
  /* Innermost first. A nested inline pushes its own locals above the caller's,
     and a same-named local belongs to the inner one -- scanning forward gave
     the parent's `yield a` the child's `a` in a super chain. The park
     mechanism bounds visibility by truncating g_nren, so a backward scan sees
     exactly the same entries. */
  for (int i = g_nren - 1; i >= 0; i--)
    if (sp_streq(g_ren_from[i], nm)) return g_ren_to[i];
  return nm;
}
/* Report a feature spinel deliberately does not support (docs/limitations.md).
   Unlike `unsupported`, which describes a codegen gap and dumps the node so the
   compiler can be debugged, this names the feature and stops: the internals are
   noise when the answer is "this is a documented limit". #2652 / #2667 / #2668 */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg) {
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  int ln; const char *file = unsup_pos(c, id, &ln);
  unsup_leave(file, ln, msg);
}

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what) {
  /* Silent emittability probe (dynamic-send arm selection): unwind without a
     diagnostic, the caller just drops this arm. */
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  const char *ty = nt_type(c->nt, id);
  /* Ruby-map the diagnostic (#1338): a codegen gap reports against the source
     line the parser stamped (the same position the #line machinery uses), so
     the message is anchored to the .rb file instead of an opaque node id.
     Falls back to the bare form when the position wasn't stamped. */
  int ln; const char *file = unsup_pos(c, id, &ln);
  char msg[2400];
  const char *mname = ty && sp_streq(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname) {
    int recv = nt_ref(c->nt, id, "receiver");
    int args = nt_ref(c->nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
    /* A bare unresolved identifier (Prism variable-call: no receiver, no
       parens, no args) is CRuby's NameError -- an undefined local that fell
       through to method lookup. Name the enclosing class the way CRuby does. */
    if (recv < 0 && ac == 0 && nt_ref(c->nt, id, "block") < 0 &&
        nt_int(c->nt, id, "vcall", 0)) {
      const char *cn = g_emitting_class_id >= 0 ? class_ruby_name(c, g_emitting_class_id) : NULL;
      snprintf(msg, sizeof msg, "undefined local variable or method '%s' for %s%s (NameError)",
               mname, cn ? "an instance of " : "main", cn ? cn : "");
      unsup_leave(file, ln, msg);
    }
    /* A call on a typed user object whose class chain has no such method is
       not a compiler gap: it is the program's NoMethodError, caught ahead of
       time. Report it in CRuby's words instead of the internal node dump. */
    if (recv >= 0) {
      TyKind rvt = comp_ntype(c, recv);
      /* Same for a typed BUILTIN receiver: when CRuby's own surface for that
         class does not carry the name either, the call is the program's
         NoMethodError, proved at compile time rather than left for run time --
         so say so in CRuby's words. A name CRuby *does* have is a spinel gap
         and keeps the internal report (#3715). */
      {
        const char *bcn = rvt == TY_STRING || rvt == TY_STRBUF ? "String"
                        : rvt == TY_INT ? "Integer" : rvt == TY_FLOAT ? "Float"
                        : rvt == TY_SYMBOL ? "Symbol"
                        : rvt == TY_RANGE || rvt == TY_FLOAT_RANGE || rvt == TY_STR_RANGE ? "Range"
                        : rvt == TY_TIME ? "Time"
                        : ty_is_array(rvt) ? "Array" : ty_is_hash(rvt) ? "Hash" : NULL;
        if (bcn && !builtin_method_known(bcn, mname) &&
            !builtin_object_method_known(mname) &&
            !name_is_enumerable_module_method(mname) &&
            !an_user_defines_method(c, mname)) {
          snprintf(msg, sizeof msg, "undefined method '%s' for an instance of %s (NoMethodError)", mname, bcn);
          unsup_leave(file, ln, msg);
        }
      }
      /* A Class value no class answers: `searched_model.none` where the method
         returns `Story` or `Comment` and neither defines `self.none`. CRuby
         raises NoMethodError when it is reached; say that rather than the
         node dump. A name Module/Class itself has is a spinel gap instead. */
      if (rvt == TY_CLASS) {
        static const char *const module_surface[] = {
          "new", "allocate", "superclass", "name", "to_s", "inspect", "ancestors",
          "instance_methods", "public_instance_methods", "private_instance_methods",
          "instance_method", "method_defined?", "public_method_defined?",
          "private_method_defined?", "protected_method_defined?", "const_get",
          "const_set", "const_defined?", "constants", "class_variable_get",
          "class_variable_set", "class_variables", "class_eval", "module_eval",
          "class_exec", "module_exec", "include?", "included_modules", "define_method",
          "alias_method", "attr_accessor", "attr_reader", "attr_writer", "subclasses",
          "attached_object", "private_constant", "module_function", "include",
          "extend", "prepend", "remove_method", "undef_method", "<", "<=", ">", ">=",
          "<=>", "==", "===", "hash", "freeze", NULL };
        int ncc = 0;
        comp_cmethod_candidates(c, mname, &ncc);
        if (ncc == 0 && !builtin_object_method_known(mname) && !str_in(mname, module_surface)) {
          snprintf(msg, sizeof msg, "undefined method '%s' for a Class: no class in the program defines a class method '%s' (NoMethodError)", mname, mname);
          unsup_leave(file, ln, msg);
        }
      }
      if (ty_is_object(rvt)) {
        int cid = ty_object_class(rvt);
        /* a name every object answers (`send`, `tap`, ...) is not the
           program's NoMethodError: that refusal is a spinel gap, and saying
           "undefined method 'send'" pointed at the wrong thing (#4850) */
        if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_native_class &&
            comp_method_in_chain(c, cid, mname, NULL) < 0 &&
            !builtin_object_method_known(mname)) {
          const char *cn = class_ruby_name(c, cid);
          snprintf(msg, sizeof msg, "undefined method '%s' for an instance of %s (NoMethodError)", mname, cn ? cn : "Object");
          unsup_leave(file, ln, msg);
        }
      }
    }
    int n = snprintf(msg, sizeof msg, "unsupported %s: node %d (%s `%s`) recv=%s/ty%d argc=%d",
                     what, id, ty, mname,
                     recv >= 0 ? nt_type(c->nt, recv) : "-",
                     recv >= 0 ? (int)comp_ntype(c, recv) : -1, ac);
    if (ac > 0 && av && n > 0 && (size_t)n < sizeof msg)
      snprintf(msg + n, sizeof msg - (size_t)n, " arg0ty%d", (int)comp_ntype(c, av[0]));
  }
  else
    snprintf(msg, sizeof msg, "unsupported %s: node %d (%s)", what, id, ty ? ty : "?");
  /* Back to the driver's per-unit recovery point (when armed), abandoning
     this unit's discarded output, so one run surfaces every gap; else out.
     `unsupported` thus never returns. */
  unsup_leave(file, ln, msg);
}

const char *c_type_name(TyKind t) {
  if (ty_is_obj_array(t)) return "sp_PtrArray *";
  switch (t) {
    case TY_INT:         return "sp_int";
    case TY_BIGINT:      return "sp_Bigint *";
    case TY_FLOAT:       return "sp_float";
    case TY_BOOL:        return "sp_bool";
    case TY_STRING:      return "const char *";
    case TY_SYMBOL:      return "sp_sym";
    case TY_RANGE:       return "sp_Range";
    case TY_FLOAT_RANGE: return "sp_FloatRange";
    case TY_STR_RANGE:   return "sp_StrRange";
    case TY_TIME:        return "sp_Time";
    case TY_COMPLEX:     return "sp_Complex";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_MATCHDATA:   return "sp_MatchData *";
    case TY_REGEX:       return "mrb_regexp_pattern *";
    case TY_EXCEPTION:   return "sp_Exception *";
    case TY_STRBUF:      return "sp_String *";
    case TY_INT_ARRAY:   return "sp_IntArray *";
    case TY_FLOAT_ARRAY: return "sp_FloatArray *";
    case TY_STR_ARRAY:   return "sp_StrArray *";
    case TY_STR_INT_HASH: return "sp_StrIntHash *";
    case TY_STR_STR_HASH: return "sp_StrStrHash *";
    case TY_INT_INT_HASH: return "sp_IntIntHash *";
    case TY_INT_STR_HASH: return "sp_IntStrHash *";
    case TY_SYM_POLY_HASH:  return "sp_SymPolyHash *";
    case TY_STR_POLY_HASH:  return "sp_StrPolyHash *";
    case TY_POLY_POLY_HASH: return "sp_PolyPolyHash *";
    case TY_POLY:         return "sp_RbVal";
    case TY_POLY_ARRAY:   return "sp_PolyArray *";
    case TY_INT_ARRAY_ARRAY: return "sp_PtrArray *";
    case TY_FLOAT_ARRAY_ARRAY: return "sp_PtrArray *";
    case TY_PROC:         return "sp_Proc *";
    case TY_CURRY:        return "sp_Curry *";
    case TY_FIBER:        return "sp_Fiber *";
    case TY_THREAD:       return "sp_thread *";
    case TY_QUEUE:        return "sp_queue *";
    case TY_MUTEX:        return "sp_mutex *";
    case TY_CONDVAR:      return "sp_condvar *";
    case TY_RANDOM:       return "sp_Random *";
    case TY_DIR:          return "sp_Dir *";
    case TY_ADDRINFO:     return "sp_Addrinfo *";
    case TY_SOCKOPT:      return "sp_SockOpt *";
    case TY_TMS:          return "sp_Tms";
    case TY_OPENSTRUCT:   return "sp_OpenStruct *";
    case TY_METHOD:       return "sp_BoundMethod *";
    case TY_IO:           return "sp_File *";
    case TY_ARGF:         return "sp_Argf *";
    case TY_ENUMERATOR:   return "sp_Enumerator *";
    case TY_CLASS:        return "sp_Class";
    default:             return NULL;
  }
}
int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE || t == TY_TIME || t == TY_COMPLEX || t == TY_RATIONAL || t == TY_MATCHDATA || t == TY_REGEX || t == TY_EXCEPTION ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY || t == TY_INT_ARRAY_ARRAY ||
         t == TY_FLOAT_ARRAY_ARRAY ||
         t == TY_STRBUF ||
         t == TY_POLY || t == TY_POLY_ARRAY || t == TY_PROC || t == TY_CURRY || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_RANDOM || t == TY_DIR || t == TY_ADDRINFO || t == TY_SOCKOPT || t == TY_METHOD || t == TY_IO || t == TY_ARGF || t == TY_ENUMERATOR || t == TY_CLASS || t == TY_OPENSTRUCT ||
         ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t);
}
/* native binding (Path B): map a spinel type spec to the C type at the ABI
   boundary. any -> the boxed value; string -> the runtime string; scalars
   pass by value. */
/* Kinds whose C representation is a struct passed BY VALUE (see c_type_name).
   A struct never implicitly converts to or from anything else in C, so a
   parameter and an argument that disagree here can never be the same call --
   unlike the numeric scalars, where int-into-float is an ordinary conversion. */
int ty_is_struct_valued(TyKind t) {
  switch (t) {
    case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE:
    case TY_TIME: case TY_COMPLEX: case TY_RATIONAL:
    case TY_TMS: case TY_CLASS:
      return 1;
    default: return 0;
  }
}

const char *native_c_type(const char *spec) {
  if (!spec) return "void";
  if (sp_streq(spec, "any"))    return "sp_RbVal";
  if (sp_streq(spec, "ptr"))    return "void *";   /* raw pointer passthrough */
  if (sp_streq(spec, "string")) return "const char *";
  if (sp_streq(spec, "text"))   return "const char *";   /* write payload: the operand's #to_s */
  if (sp_streq(spec, "string?")) return "const char *";  /* nullable; call site wraps */
  if (sp_streq(spec, "nstring")) return "const char *";   /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return "const char *";   /* borrowed C string; call site dups */
  if (sp_streq(spec, "cbinstr")) return "const char *";  /* borrowed C bytes; call site dups sp_ffi_bin_len of them */
  if (sp_streq(spec, "regexp")) return "mrb_regexp_pattern *";
  if (sp_streq(spec, "int"))    return "sp_int";
  if (sp_streq(spec, "float"))  return "double";
  /* sp_bool, NOT int: a package's C function returns sp_bool (_Bool, one
     byte), so prototyping it here as int is a mismatched declaration. The
     caller then reads a full register where the callee only wrote its low
     byte -- gcc happened to zero the rest, ubuntu clang did not, and
     StringIO.new("x").closed? answered true. */
  if (sp_streq(spec, "bool"))   return "sp_bool";
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return "void";
  return "sp_RbVal";
}

const char *ffi_c_type(const char *spec) {
  const FfiSpecInfo *info = ffi_spec_lookup(spec);
  return info ? info->c_type : "void";
}

/* The C name an ffi_func's extern is declared and called under. The prototype
   is built from the spec types, which need not match a header that also
   declares the symbol (fopen's FILE * is our void *, strchr's char * our
   const char *), and C rejects two declarations of one name with different
   types. So the extern takes a private name per ffi_func and binds it to the
   real symbol with an asm label (SP_FFI_SYM in the emitted prologue): no
   header declaration, function-like macro or second module binding the same
   symbol under other specs can conflict with it. */
void ffi_extern_name(Compiler *c, int fi, Buf *out) {
  const char *sym = c->ffi_funcs[fi].csym ? c->ffi_funcs[fi].csym : c->ffi_funcs[fi].name;
  buf_printf(out, "sp_ffi_f%d_%s", fi, sym);
}

/* The C type of one ffi_callback argument, used to build the trampoline's own
   pointer type. A :ptr callback arg is `const void*` -- the near-universal shape
   of C comparator/visitor callbacks (qsort, bsearch, ...) -- so the generated
   trampoline's type matches the header's declaration exactly (no
   incompatible-function-pointer error). */
const char *ffi_cb_arg_ctype(const char *spec) {
  if (sp_streq(spec, "ptr")) return "const void *";
  return ffi_c_type(spec);
}
/* Write into `out` the C test for "local `en` currently holds nil", and answer
   1. Answers 0 when the slot has no nil representation: every value the type
   can hold is truthy, and its zero is indistinguishable from a real one, so
   `x ||= v` on it genuinely is a no-op.

   The pointer-shaped kinds are exactly the ones declare_local initialises to
   NULL, so a slot still holding NULL has never been assigned -- which is nil,
   not "already truthy". Reading it as truthy is what dropped the assignment in
   `text ||= [...].join(" ")` (#3388). */
/* The initial value of a local's slot: its type's zero, or the type's nil
   sentinel when the local has no definite assignment anywhere (#3388). */
const char *local_init_value(Compiler *c, LocalVar *lv) {
  if (lv->or_write_only && !lv->is_param && !lv->is_block_param) {
    const char *nv = nil_value(lv->type);
    if (nv) return nv;
  }
  /* A value-type object is stored INLINE (sp_X, not sp_X *), so its zero is a
     zeroed struct. default_value answers the blanket "NULL" for every object
     type, which declares `sp_K lv_r = NULL;` -- an invalid initializer, and a
     C build that stops. declare_local has had this arm all along; the inlined
     path reached the shared helper instead. The shape that finds it is the
     resource idiom: `def self.open; r = new; begin; yield r; ensure; r.close;
     end; end` on a class small enough to be a value type. */
  if (comp_ty_value_obj(c, lv->type)) return "{0}";
  return lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type);
}
/* A value landing in a slot of type `slot`. An Integer or Float slot that
   also sees nil is a nullable scalar (ty_unify's nil join), and its nil is
   the sentinel: a bare `nil`, or a nil-typed expression (a void call, an
   always-nil method), is spelled as that, where emit_expr renders the
   numeric 0 that reads as a real value. Every other slot takes emit_expr. */
void emit_expr_slot(Compiler *c, int node, TyKind slot, Buf *b) {
  if (node >= 0 && (slot == TY_INT || slot == TY_FLOAT)) {
    const char *sent = slot == TY_INT ? "SP_INT_NIL" : "sp_float_nil()";
    if (nt_kind(c->nt, node) == NK_NilNode) { buf_puts(b, sent); return; }
    TyKind vt = comp_ntype(c, node);
    if (vt == TY_NIL || vt == TY_VOID) {
      buf_puts(b, "({ (void)("); emit_expr(c, node, b); buf_printf(b, "); %s; })", sent);
      return;
    }
  }
  emit_expr(c, node, b);
}
/* A value written into a typed scalar slot -- an Integer or Float array's
   element, a typed accumulator -- from an expression the inference typed
   boxed: under --int-overflow=promote an arithmetic node whose operands are
   not constants is poly (it may promote), while the slot it lands in stays
   typed (a typed Array does not widen), so the value is converted at the
   sink; a Bignum that does not fit raises there. Any other expression is
   emitted as it is. `text` is the already-rendered expression. */
void emit_typed_sink_text(Compiler *c, int node, TyKind slot, const char *text, Buf *b) {
  TyKind vt = node >= 0 ? comp_ntype(c, node) : TY_UNKNOWN;
  if (vt == TY_POLY && slot == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", text);
  else if (vt == TY_POLY && slot == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", text);
  else if (vt == TY_BIGINT && slot == TY_INT) buf_printf(b, "sp_bigint_to_int(%s)", text);
  else buf_puts(b, text);
}
int local_nil_test(Compiler *c, LocalVar *lv, const char *ref, Buf *out) {
  if (!lv) return 0;
  TyKind t = lv->type;
  /* sp_int 0 and 0.0 are real values, so the slot only distinguishes nil when
     it was declared with the sentinel -- which declare_local does exactly when
     the local has no definite assignment anywhere. */
  int nil_init = lv->or_write_only && !lv->is_param && !lv->is_block_param;
  /* ...or when some write leaves the sentinel in it (`a = nil; a ||= 10`):
     the nil join keeps such a slot an sp_int, and its nil is the sentinel */
  if (lv->nullable_int) nil_init = 1;
  switch (t) {
    case TY_STRING: case TY_BIGINT: case TY_OPENSTRUCT:
      buf_printf(out, "!%s", ref); return 1;
    case TY_CLASS:
      buf_printf(out, "sp_class_nil_p(%s)", ref); return 1;
    case TY_INT:
      if (!nil_init) return 0;
      buf_printf(out, "%s == SP_INT_NIL", ref); return 1;
    case TY_FLOAT:
      if (!nil_init) return 0;
      buf_printf(out, "sp_float_is_nil(%s)", ref); return 1;
    /* value kinds with no in-band nil (and POLY/BOOL/SYMBOL, whose callers
       test their own sentinel before reaching here) */
    case TY_BOOL: case TY_SYMBOL: case TY_POLY:
    case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: case TY_TIME:
    case TY_COMPLEX: case TY_RATIONAL: case TY_TMS:
      return 0;
    default:
      if (comp_ty_value_obj(c, t)) return 0;
      if (t != TY_UNKNOWN && is_scalar_ret(t)) { buf_printf(out, "!%s", ref); return 1; }
      return 0;
  }
}
/* The dead value closing a `({ ...; sp_raise_cls(...); V; })` arm. The raise
   never returns, so V only has to type-check in the slot: an UNKNOWN result
   flows as poly (default_value's "0" would not assign to sp_RbVal), and a
   Range wants its brace form. */
const char *raise_tail_value(TyKind t) {
  if (t == TY_UNKNOWN || t == TY_VOID) return "sp_box_nil()";
  return default_value(t);
}

/* Compiler-aware form: a by-value object class's C representation is a bare
   struct, where default_value's NULL would be ill-typed C. */
const char *raise_tail_value_c(Compiler *c, TyKind t) {
  if (ty_is_object(t) && comp_ty_value_obj(c, t)) {
    /* rotate: one static buffer would make two of these in a single
       buf_printf read the same text, and nothing in the signature says so */
    static char vbuf[4][128];
    static int vslot = 0;
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses) {
      char *out = vbuf[vslot++ & 3];
      snprintf(out, sizeof vbuf[0], "((sp_%s){0})", c->classes[cid].c_name);
      return out;
    }
  }
  return raise_tail_value(t);
}

/* The value a typed slot holds when Ruby's answer is nil: a declared but
   unassigned local, a `next` with no value, an if with no else, a case no
   arm matched, a bare return. For an Integer or a Float that is the
   sentinel, since the nil join of ty_unify makes such a slot a nullable
   scalar; 0 read as a truthy number there (the String's NULL was its nil
   already). A counter an emitter starts at zero writes the literal. */
const char *default_value(TyKind t) {
  switch (t) {
    case TY_INT:    return "SP_INT_NIL";
    case TY_FLOAT:  return "sp_float_nil()";
    case TY_BOOL:   return "0";
    case TY_STRING: return "NULL";
    case TY_SYMBOL: return "((sp_sym)-1)";
    case TY_RANGE:  return "(sp_Range){0}";
    case TY_FLOAT_RANGE: return "(sp_FloatRange){0}";
    case TY_STR_RANGE:   return "(sp_StrRange){0}";
    case TY_TIME:   return "(sp_Time){0}";
    case TY_COMPLEX: return "(sp_Complex){0}";
    case TY_RATIONAL: return "(sp_Rational){0}";
    case TY_MATCHDATA:  return "NULL";
    case TY_BIGINT:     return "NULL";
    case TY_REGEX:      return "NULL";
    case TY_EXCEPTION: return "NULL";
    case TY_STRBUF:    return "NULL";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY:
    case TY_INT_ARRAY_ARRAY: return "NULL";
    case TY_FLOAT_ARRAY_ARRAY: return "NULL";
    case TY_PROC:    return "NULL";
    case TY_CURRY:   return "NULL";
    case TY_FIBER:   return "NULL";
    case TY_THREAD:  return "NULL";
    case TY_QUEUE:   return "NULL";
    case TY_MUTEX:   return "NULL";
    case TY_CONDVAR: return "NULL";
    case TY_RANDOM:  return "NULL";
    case TY_DIR:     return "NULL";
    case TY_ADDRINFO: return "NULL";
    case TY_SOCKOPT: return "NULL";
    case TY_TMS:     return "((sp_Tms){0})";
    case TY_OPENSTRUCT: return "NULL";
    case TY_METHOD:  return "NULL";
    case TY_IO:      return "NULL";
    case TY_ARGF:    return "NULL";
    case TY_ENUMERATOR: return "NULL";
    case TY_POLY:    return "sp_box_nil()";
    case TY_CLASS:   return "((sp_Class){-1})";
    default:        return (ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t)) ? "NULL" : "0";
  }
}
/* The C type of class `cid`'s instances. A `native_struct` carries the name
   its declaration gave -- which need not be derived from the Ruby class name
   (`native_struct "Store", "sp_X509_Store"`) -- and every other class is the
   `sp_<c_name>` struct the generator defines for it. Four rotating buffers so
   one format string can name two classes. */
const char *class_ctype(Compiler *c, int cid) {
  static char bufs[4][160];
  static int turn = 0;
  if (cid < 0 || cid >= c->nclasses) return "void";
  ClassInfo *ci = &c->classes[cid];
  if (ci->is_native_class && ci->c_struct) return ci->c_struct;
  char *out = bufs[turn++ & 3];
  snprintf(out, sizeof bufs[0], "sp_%s", ci->c_name ? ci->c_name : "");
  return out;
}
void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* value-type classes are stored inline (sp_X); others are heap pointers */
    buf_printf(b, "%s %s", class_ctype(c, cid), c->classes[cid].is_value_type ? "" : "*");
  }
  else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}
/* The element stamp a pointer array is boxed with (#4486): the runtime's one
   cls_id for pointer arrays is type-erased, so the box carries what the
   elements are for the poly paths to read them back. */
const char *ptr_array_stamp(Compiler *c, TyKind t) {
  static char buf[64];
  (void)c;
  if (t == TY_INT_ARRAY_ARRAY)   return "SP_PTR_ELEM_INT_ROWS, -1";
  if (t == TY_FLOAT_ARRAY_ARRAY) return "SP_PTR_ELEM_FLT_ROWS, -1";
  snprintf(buf, sizeof buf, "SP_PTR_ELEM_OBJ, %d", ty_obj_array_class(t));
  return buf;
}
void emit_box_open(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_INT)          buf_puts(b, "sp_box_int(");
  else if (t == TY_STRING)  buf_puts(b, "sp_box_str(");
  else if (t == TY_FLOAT)   buf_puts(b, "sp_box_float(");
  else if (t == TY_BOOL)    buf_puts(b, "sp_box_bool(");
  else if (t == TY_NIL)     buf_puts(b, "sp_box_nil(); (void)(");
  else if (t == TY_SYMBOL)  buf_puts(b, "sp_box_sym(");
  /* Array slots are nilable C pointers (a nil-defaulting param, `[x] if cond`
     in value position): box NULL as a proper nil, not a truthy OBJ wrapping
     NULL that passes truthy checks and then segfaults on the first access
     (#3275). Matches emit_boxed_text's array cases. */
  else if (t == TY_INT_ARRAY)   buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_FLOAT_ARRAY) buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_STR_ARRAY)   buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_POLY_ARRAY)  buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (ty_is_ptr_array(t))  buf_puts(b, "sp_box_ptr_array_k((void *)(");   /* by reference, stamped (#4486) */
  else if (t == TY_CLASS) buf_puts(b, "sp_box_class(");
  else if (t == TY_COMPLEX)  buf_puts(b, "sp_box_complex(");
  else if (t == TY_RATIONAL) buf_puts(b, "sp_box_rational(");
  /* Reference-backed builtins are nilable C pointers: box NULL as nil. */
  else if (ty_nullable_builtin_id(t)) buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* the struct typedef is sp_<c_name>; a bare `(<Name> *)` would never
       have compiled, so this arm was effectively unreachable as written */
    buf_printf(b, "sp_box_obj((%s *)( ", class_ctype(c, cid));
  }
  /* TY_POLY: already sp_RbVal, no prefix */
}
void emit_box_close(Compiler *c, TyKind t, Buf *b) {
  (void)c;
  if (t == TY_POLY || t == TY_UNKNOWN) return; /* no-op: already sp_RbVal */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) { buf_printf(b, "), %s)", nbid); return; } }
  if (ty_is_object(t))        { buf_printf(b, "), %d)", ty_object_class(t)); return; }
  /* array open used sp_box_nullable_obj((void *)( ... -- close with the kind. */
  if (t == TY_INT_ARRAY)   { buf_puts(b, "), SP_BUILTIN_INT_ARRAY)"); return; }
  if (t == TY_FLOAT_ARRAY) { buf_puts(b, "), SP_BUILTIN_FLT_ARRAY)"); return; }
  if (t == TY_STR_ARRAY)   { buf_puts(b, "), SP_BUILTIN_STR_ARRAY)"); return; }
  if (t == TY_POLY_ARRAY)  { buf_puts(b, "), SP_BUILTIN_POLY_ARRAY)"); return; }
  if (ty_is_ptr_array(t))  { buf_printf(b, "), %s)", ptr_array_stamp(c, t)); return; }
  buf_puts(b, ")");
}
/* comp_ntype through fold_seed_kind, which owns the rule (see types.c). */
TyKind fold_seed_ntype(Compiler *c, int node) {
  return fold_seed_kind(comp_ntype(c, node), nt_type(c->nt, node));
}
/* sum(seed) through the boxed fold. The receiver is boxed into a ROOTED temp
   before the seed runs: a seed that allocates -- a Rational, a Bignum -- can
   collect a receiver array the same statement just built, and the fold then
   walked an empty one. Rooting also fixes the order, which is Ruby's: the
   receiver first, then the seed, each evaluated exactly once. Shared by the
   typed-array, Hash and poly-receiver call sites, which each had the hazard. */
void emit_poly_sum_seed(Compiler *c, int recv, int seed, Buf *b) {
  int tr = ++g_tmp, ts = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_boxed(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", tr, ts); emit_boxed(c, seed, b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_poly_sum_seed(_t%d, _t%d); })", ts, tr, ts);
}
/* The runtime conversion of a typed array to the poly array; NULL for a kind
   that has none. */
const char *array_to_poly_fn(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "sp_IntArray_to_poly";
    case TY_FLOAT_ARRAY: return "sp_FloatArray_to_poly";
    case TY_STR_ARRAY:   return "sp_StrArray_to_poly_fmt";
    default:             return NULL;
  }
}
/* The kind of the slot the next emit_block_value_into writes, when it is not
   the tail's own (a `then` whose value joined a `next` arm of another array
   kind, #4747); that one call consumes it. */
TyKind g_bv_dest_ty = TY_UNKNOWN;
const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}
/* Storage prefix for a loop over the array itself. A nested numeric table is
   an sp_PtrArray of row pointers, so the walk is sp_PtrArray_length / _get
   (void* converts to the row pointer). Object arrays share that storage but
   are not walked here. */
const char *array_iter_kind(TyKind t) {
  if (t == TY_POLY_ARRAY) return "Poly";
  if (ty_is_ptr_array(t)) return "Ptr";   /* a numeric table's rows, or objects */
  return array_kind(t);
}
void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}
void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}
/* Marker byte before string-literal data: 0xff is an immutable rodata literal
   (frozen? false, value semantics); `frozen` -- set from the node's `fzl` flag
   when its file has `# frozen_string_literal: true` -- switches to 0xf1 so
   `frozen?` is true and mutation raises FrozenError. Synthesized strings
   (symbol names, ivar names, ...) go through emit_str_literal, which is never
   frozen: the pragma only affects literals written in the source. */
/* Open/close a static frozen-literal object around caller-streamed escaped
   bytes. The 0xf1 marker promises a REAL sp_str_hdr immediately in front of
   the data (hash cache, mutation guards), so every frozen literal -- single
   or an adjacent-literal fold -- must carry this header (#1749). */
int emit_frozen_literal_open(Buf *b, size_t raw_len) {
  return emit_frozen_literal_open_a(b, raw_len, 0);
}
/* `ascii7` says every byte is below 0x80, which the caller knows from the
   bytes it is about to stream. Recording it in the header is what lets the
   runtime index this literal by byte -- sp_str_fixed_width wants the bit AND
   a known length, and a literal has always carried the length. Without it a
   frozen literal was walked to find every character index, and the byte-load
   fold for `s[i] == "c"` had no way to prove itself safe (#4239). */
int emit_frozen_literal_open_a(Buf *b, size_t raw_len, int ascii7) {
  static int g_fzl_ctr = 0;
  int id = g_fzl_ctr++;
  size_t dl = raw_len + 1;
  buf_printf(b, "({ static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _fzl_%d = "
                "{ { NULL, %zu%s, %zu, 0 }, 0xf1, \"", dl, id, dl,
             ascii7 ? " | SP_STR_SIZE_ASCII7" : "", raw_len);
  return id;
}
/* Every byte below 0x80 -- the compile-time half of the ASCII7 bit above. */
int bytes_are_ascii7(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] >= 0x80) return 0;
  return 1;
}
void emit_frozen_literal_close(Buf *b, int id) {
  buf_printf(b, "\" }; _fzl_%d.d; })", id);
}
static int round_kw_elem(Compiler *c, const RoundKw *o, int e, int *is_splat, int *opaque) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, o->node, "elements", &n);
  if (e >= n) return -1;
  int key = nt_ref(nt, els[e], "key");
  const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
  int kn_ok = kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"));
  *is_splat = key < 0;
  *opaque = key >= 0 && !kn_ok;
  return nt_ref(nt, els[e], "value");
}

void round_kw_read(Compiler *c, int kwh, RoundKw *o) {
  const NodeTable *nt = c->nt;
  memset(o, 0, sizeof *o);
  o->node = kwh;
  o->half = -1;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  char names[256]; names[0] = 0;
  for (int e = 0; e < n; e++) {
    int val = nt_ref(nt, els[e], "value");
    if (val < 0) continue;              /* nothing to evaluate and nothing to say */
    o->nelem = e + 1;
    int key = nt_ref(nt, els[e], "key");
    if (key < 0) { o->nsplat++; continue; }
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && sp_streq(kn, "half")) { o->half = val; continue; }  /* a repeated key: the last wins */
    const char *ks = (!kn && kty && sp_streq(kty, "StringNode")) ? nt_str(nt, key, "content") : NULL;
    if (!kn && !ks) continue;           /* unreadable: claim nothing */
    if (o->nunknown < 8) {
      size_t at = strlen(names);
      snprintf(names + at, sizeof names - at, "%s%s%s%s", o->nunknown ? ", " : "",
               kn ? ":" : "\"", kn ? kn : ks, kn ? "" : "\"");
    }
    o->nunknown++;
  }
  if (n) o->nelem = n;
  if (o->nunknown)
    snprintf(o->unknown, sizeof o->unknown, "unknown keyword%s: %s",
             o->nunknown > 1 ? "s" : "", names);
}

/* Evaluate every keyword value for its side effects and say nothing else --
   the reject paths, where CRuby has still built the hash before deciding the
   call cannot be made. */
void emit_round_kw_effects(Compiler *c, const RoundKw *kw, Buf *b) {
  for (int e = 0; e < kw->nelem; e++) {
    int sp_, op_; int v = round_kw_elem(c, kw, e, &sp_, &op_);
    if (v < 0) continue;
    buf_puts(b, "(void)("); emit_boxed(c, v, b); buf_puts(b, "); ");
  }
}

/* Bind what CRuby evaluates before a `round`-family call decides anything --
   every keyword value, in source order -- then settle the tie-break mode and
   raise for an unknown keyword, which CRuby does only once the whole hash has
   been read. Emits into an already-open statement expression; answers the
   temp holding the mode, or -1 when the hash names none. */
int emit_round_kw_binds(Compiler *c, const RoundKw *kw, Buf *b) {
  int thalf = -1, has_splat = 0, tfirst = g_tmp + 1;
  for (int e = 0; e < kw->nelem; e++) {
    int is_splat, opaque; int v = round_kw_elem(c, kw, e, &is_splat, &opaque);
    int t = ++g_tmp;
    if (v < 0) { buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); (void)_t%d; ", t, t); continue; }
    buf_printf(b, "sp_RbVal _t%d = ", t);
    emit_boxed(c, v, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", t);
    if (is_splat) has_splat = 1;
    else if (!opaque && v == kw->half) thalf = t;
  }
  if (kw->nunknown) {
    buf_puts(b, "sp_raise_cls(\"ArgumentError\", ");
    emit_str_literal(b, kw->unknown);
    buf_puts(b, "); ");
  }
  if (!has_splat) return thalf;
  /* a `**` source is read in its place, so a `half:` on either side of it
     wins by being later, as it does in the hash the call really builds */
  int tm = ++g_tmp;
  buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); ", tm, tm);
  for (int e = 0; e < kw->nelem; e++) {
    int is_splat, opaque; int v = round_kw_elem(c, kw, e, &is_splat, &opaque);
    int t = tfirst + e;                 /* the temps were numbered in this order */
    if (v < 0) continue;
    if (is_splat)
      buf_printf(b, "{ sp_RbVal _s%d = sp_round_half_kwsplat(_t%d);"
                    " if (_s%d.tag != SP_TAG_NIL) _t%d = _s%d; } ",
                 t, t, t, tm, t);
    else if (!opaque && v == kw->half)
      buf_printf(b, "_t%d = _t%d; ", tm, t);
  }
  return tm;
}

void emit_str_literal_n(Buf *b, const char *content, size_t len, int frozen) {
  const char *mk = frozen ? "\\xf1" : "\\xff";
  /* A frozen literal must carry a REAL sp_str_hdr: the 0xf1 marker promises
     one immediately in front of the data (sp_str_hash caches the FNV hash
     through it, mutation guards and frozen? key off the marker). Baking the
     marker onto a bare rodata literal made that header read/write land in
     whatever rodata precedes the literal -- a garbage cached hash, so a
     Hash#[] with the literal key missed entries whose equal-content keys
     were built at runtime (#1749; ASAN: global-buffer-overflow). Emit a
     static header+marker+data object instead: the layout matches a heap
     string exactly (hdr | marker | bytes), the hash cache write hits our
     own static storage, and next=NULL keeps it off the sweep list. */
  if (frozen) {
    int id = emit_frozen_literal_open_a(b, content ? len : 0,
                                       content && len ? bytes_are_ascii7(content, len) : 1);
    if (content && len) emit_c_escaped_n(b, content, len);
    emit_frozen_literal_close(b, id);
    return;
  }
  if (!content || len == 0) { buf_printf(b, "(&(\"%s\")[1])", mk); return; }
  /* NUL-containing strings: use sp_str_from_bytes with explicit byte count.
     The heap string it builds is writable (0xfe), so a frozen literal is
     sealed with sp_str_freeze_val (flips the heap marker to 0xf1 in place). */
  if (len > strlen(content)) {
    /* A FROZEN NUL-containing literal is immortal (sp_str_sweep never frees a
       0xf1 string), so build it once into a call-site-local static and reuse it.
       This avoids re-allocating it on every evaluation -- which, besides the
       churn, made the literal a GC-triggering sibling that could sweep an
       unrooted operand mid-expression (e.g. the receiver in
       `data[8, 8].delete("\0")`, a use-after-free in doom's WAD name parse). */
    if (frozen) {
      static int g_binlit_ctr = 0;
      int id = g_binlit_ctr++;
      buf_printf(b, "({ static const char *_binlit_%d; _binlit_%d ? _binlit_%d : "
                    "(_binlit_%d = sp_str_freeze_val(sp_str_from_bytes(\"", id, id, id, id);
      emit_c_escaped_n(b, content, len);
      buf_printf(b, "\", %zu))); })", len);
      return;
    }
    buf_puts(b, "sp_str_from_bytes(\"");
    emit_c_escaped_n(b, content, len);
    buf_printf(b, "\", %zu)", len);
    return;
  }
  buf_printf(b, "(&(\"%s\" \"", mk);
  emit_c_escaped_n(b, content, len);
  buf_puts(b, "\")[1])");
}
void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content), 0);
}
/* Ruby-source string literal (a StringNode): unlike the internal-constant
   emitter above, each OCCURRENCE gets its own static array, so two textually
   equal literals are two distinct objects and `equal?` (pointer identity)
   answers like CRuby. The plain `("\xff" "abc")` form let the C compiler merge
   equal literals into one address, making `"abc".equal?("abc")` true. A
   re-evaluated occurrence (a literal in a loop) still yields one address --
   the frozen-string-literal semantics spinel's immutable strings already have.
   Frozen and NUL-containing literals keep their existing per-site forms
   (_fzl_N / sp_str_from_bytes), which are already identity-correct. */
void emit_str_literal_src(Buf *b, const char *content, size_t len, int frozen) {
  static int g_slit_ctr = 0;
  if (frozen || (content && len > strlen(content))) {
    emit_str_literal_n(b, content, len, frozen);
    return;
  }
  int lid = g_slit_ctr++;
  buf_printf(b, "({ static const char _slit_%d[] = \"\\xff\" \"", lid);
  if (content && len) emit_c_escaped_n(b, content, len);
  buf_printf(b, "\"; &_slit_%d[1]; })", lid);
}
/* Emit a catch/throw tag expression; returns the tag KIND (0 = name tag
   matched by content, 1 = object tag matched by pointer identity). */
int emit_catch_tag(Compiler *c, int id, Buf *b) {
  const char *ty = nt_type(c->nt, id);
  if (ty && sp_streq(ty, "SymbolNode")) { emit_str_literal(b, nt_str(c->nt, id, "value")); return 0; }
  if (ty && sp_streq(ty, "StringNode")) { emit_str_literal(b, nt_str(c->nt, id, "unescaped")); return 0; }
  TyKind t = comp_ntype(c, id);
  if (t == TY_SYMBOL) {
    buf_puts(b, "sp_sym_to_s("); emit_expr(c, id, b); buf_puts(b, ")");
    return 0;
  }
  if (ty_is_object(t)) {
    /* a non-symbol object tag matches by identity: carry its pointer */
    buf_puts(b, "(const char *)(void *)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  if (t == TY_POLY) {
    /* A boxed tag is whatever the value is: a Symbol or a String matches by
       name, an object by identity. The kind is the value's at run time, so
       the caller gets the sp_RbVal and -1, and asks sp_catch_tag_of for the
       tag and the kind together. Reading the payload as an object pointer
       made a Symbol element of an Array (`throw TAGS[1]`) a kind-1 tag whose
       pointer was the symbol's id, and it matched nothing (#4523). */
    emit_expr(c, id, b);
    return -1;
  }
  if (t == TY_STRING) {
    /* a dynamic string tag: a valid pointer, matched by content (like the
       StringNode-literal arm above) */
    emit_expr(c, id, b);
    return 0;
  }
  if (t == TY_INT) {
    /* an Integer tag: CRuby matches by identity, which for a Fixnum is value
       equality -- carry the value in the pointer slot and match by identity. */
    buf_puts(b, "(const char *)(intptr_t)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  /* A Float/boolean/nil/Bignum tag would emit a non-pointer (or a struct)
     into the const char* tag slot -- invalid C, and (0/1 truncation) a wrong
     match. These are vanishingly rare as catch/throw tags; reject loudly
     rather than miscompile. */
  unsupported(c, id, "catch/throw with a Float, boolean, nil, or Bignum tag (use a Symbol, String, Integer, or object tag)");
  return 0;
}
/* A key whose static kind can never be in a typed hash's table: a String
   or a user object looked up in an Integer-keyed Hash, a Float where
   1.eql?(1.0) is false, nil in any of them. Hash looks a key up by #hash and
   #eql? and converts nothing, so the lookup is a plain miss in CRuby, not a
   TypeError -- and not the raw pointer in the sp_int slot that stopped the C
   build ({1 => 2}.dig("a"), .fetch("a"), .except(obj)). The same kinds a
   poly key of another tag already misses on. A user object is a miss
   without asking whether its class defines #eql? and #hash: a typed table
   holds no objects, so nothing in it can be eql? to one. */
int hash_key_misses(Compiler *c, int key, TyKind kt) {
  TyKind actual = comp_ntype(c, key);
  if (kt == TY_POLY || actual == kt || actual == TY_POLY || actual == TY_UNKNOWN) return 0;
  if (kt == TY_STRING && actual == TY_STRBUF) return 0;
  return actual == TY_NIL || actual == TY_BOOL || actual == TY_INT ||
         actual == TY_BIGINT || actual == TY_FLOAT || actual == TY_SYMBOL ||
         actual == TY_STRING || actual == TY_STRBUF || actual == TY_RANGE ||
         actual == TY_FLOAT_RANGE || actual == TY_STR_RANGE || actual == TY_TIME ||
         actual == TY_REGEX || ty_is_array(actual) || ty_is_hash(actual) ||
         ty_is_object(actual);
}

void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  TyKind actual = comp_ntype(c, key);
  if (hash_key_misses(c, key, kt)) {
    /* evaluate the key for its effects, then answer the value no key equals */
    buf_puts(b, "({ (void)(");
    emit_expr(c, key, b);
    if (kt == TY_STRING)      buf_puts(b, "); (const char *)0; })");
    else if (kt == TY_SYMBOL) buf_puts(b, "); (sp_sym)-1; })");
    else                      buf_puts(b, "); SP_INT_NIL; })");
    return;
  }
  /* A Symbol key on a String-keyed hash used to coerce to its name, a
     leftover of an older Hash.new{} model (its hash is PolyPoly now): it
     made `h[:a]` find "a"'s entry and `h.delete(:a)` remove it (#4531).
     :a != "a", so it is a miss like any other kind mismatch above. */
  if (actual == TY_POLY && kt != TY_POLY) {
    /* The union member is only valid when the tag agrees. A call site reached
       with a key of another kind -- the same method called with a String and
       with a Float -- read a Float's bits as a `const char *` and dereferenced
       them (#3810). A key of the wrong kind is simply not in the table, so
       answer a value no key can equal and let the lookup miss. */
    buf_puts(b, "({ sp_RbVal _hk = ");
    emit_boxed(c, key, b);
    /* A shared-string handle is `==` to the immediate string with the same
       bytes and now hashes alike, so it is a key that IS in the table: deref
       it rather than answering the no-key sentinel (#4279). */
    if (kt == TY_STRING)      buf_puts(b, "; _hk = sp_poly_strbuf_deref(_hk); _hk.tag == SP_TAG_STR ? _hk.v.s : (const char *)0; })");
    else if (kt == TY_SYMBOL) buf_puts(b, "; _hk.tag == SP_TAG_SYM ? (sp_sym)_hk.v.i : (sp_sym)-1; })");
    else                      buf_puts(b, "; _hk.tag == SP_TAG_INT ? _hk.v.i : SP_INT_NIL; })");
    return;
  }
  if (kt == TY_POLY && actual != TY_POLY) {
    /* PolyPolyHash key: box the typed value into sp_RbVal */
    emit_boxed(c, key, b);
    return;
  }
  emit_expr(c, key, b);
}
/* 1 when `kwh` is a keyword list made only of `**` spreads (`m(**a, **b)`).
   Such a list may be empty at run time, and an empty spread passes no
   argument at all (`m(1, **{})` is `m(1)`). */
int kwh_only_spreads(const NodeTable *nt, int kwh) {
  if (kwh < 0 || nt_kind(nt, kwh) != NK_KeywordHashNode) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  if (en == 0) return 0;
  for (int e = 0; e < en; e++)
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode) return 0;
  return 1;
}
int unwrap_parens(Compiler *c, int id) {
  while (id >= 0) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "ParenthesesNode")) break;
    int body = nt_ref(c->nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
    if (n != 1) break;
    id = bd[0];
  }
  return id;
}

/* Walk a String `<<` chain (`(s << a) << b`) down to its base, peeling
   parens. Fills chain[] outermost-first with each link's argument (at most
   64) and stores the base node in *base. Returns the link count. */
int str_append_chain(Compiler *c, int recv, int *chain, int *base) {
  const NodeTable *nt = c->nt;
  int nchain = 0; int cur = recv;
  while (nchain < 64) {
    while (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "ParenthesesNode")) {
      int pb = nt_ref(nt, cur, "body");
      if (pb < 0) break;
      int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
      if (bn != 1) break;
      cur = bb[0];
    }
    const char *cty = nt_type(nt, cur);
    if (!cty || !sp_streq(cty, "CallNode")) break;
    const char *cnm = nt_str(nt, cur, "name");
    int crecv = nt_ref(nt, cur, "receiver");
    if (!cnm || !sp_streq(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
    int cargs = nt_ref(nt, cur, "arguments");
    int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
    if (cac != 1) break;
    chain[nchain++] = cav[0];
    cur = crecv;
  }
  *base = cur;
  return nchain;
}

/* 1 when the receiver is a range whose begin endpoint is statically a Float
   -- directly a (possibly parenthesized) RangeNode or through a
   sole-assignment local. CRuby raises TypeError "can't iterate from Float"
   when enumerating such a range (an int begin with a float end iterates
   fine); the int-backed sp_Range would otherwise silently truncate. */
int range_float_begin(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rn = unwrap_parens(c, recv);
  if (rn < 0) return 0;
  const char *rty = nt_type(nt, rn);
  if (!rty || !sp_streq(rty, "RangeNode")) {
    rn = local_sole_range_node(c, rn);
    if (rn < 0) return 0;
  }
  int lo = nt_ref(nt, rn, "left");
  return lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
}
const char *int_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_int_add";
  if (sp_streq(op, "-"))  return "sp_int_sub";
  if (sp_streq(op, "*"))  return "sp_int_mul";
  if (sp_streq(op, "/"))  return "sp_idiv";
  if (sp_streq(op, "%"))  return "sp_imod";
  if (sp_streq(op, "**")) return "sp_int_pow";
  return NULL;
}
const char *bigint_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_bigint_add";
  if (sp_streq(op, "-"))  return "sp_bigint_sub";
  if (sp_streq(op, "*"))  return "sp_bigint_mul";
  if (sp_streq(op, "/"))  return "sp_bigint_div";
  if (sp_streq(op, "%"))  return "sp_bigint_mod";
  if (sp_streq(op, "&"))  return "sp_bigint_and";
  if (sp_streq(op, "|"))  return "sp_bigint_or";
  if (sp_streq(op, "^"))  return "sp_bigint_xor";
  return NULL;
}
/* True if any user exception subclass overrides #message or #to_s, so the
   default exception message/to_s path must dispatch to the user method rather
   than reporting the stored message (which defaults to the class name). */
int exc_has_user_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    /* Only a string-returning override is dispatched (see codegen_program),
       so gate on TY_STRING to match -- otherwise a non-string override would
       route every exception query through an empty dispatcher for nothing. */
    int mi_msg = comp_method_in_chain(c, i, "message", NULL);
    if (mi_msg >= 0 && (TyKind)c->scopes[mi_msg].ret == TY_STRING)
      return 1;
    int mi_tos = comp_method_in_chain(c, i, "to_s", NULL);
    if (mi_tos >= 0 && (TyKind)c->scopes[mi_tos].ret == TY_STRING)
      return 1;
  }
  return 0;
}

/* An override that answers something other than a String: Exception#message
   is #to_s, so such a class carries a non-string value out of #message and the
   const char * dispatchers cannot represent it. A boxed pair of dispatchers is
   emitted instead, and the call sites type as poly (#3868). */
int exc_has_nonstring_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    for (int k = 0; k < 2; k++) {
      int mi = comp_method_in_chain(c, i, k ? "to_s" : "message", NULL);
      if (mi < 0) continue;
      TyKind rk = (TyKind)c->scopes[mi].ret;
      if (rk != TY_STRING && rk != TY_UNKNOWN) return 1;
    }
  }
  return 0;
}

/* The class an index-taking Array method was handed instead of an index, or
   NULL when the argument can serve as one. Ruby converts a Float through
   #to_int and takes a Range where the method has a slice form; a String,
   Symbol, Array, Hash, nil or boolean is a TypeError. A poly argument stays on
   the runtime path: its class is not settled here (#3923, #3924, #3925). */
const char *array_index_bad_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int ir = nt_ref(nt, id, "receiver");
  const char *inm = nt_str(nt, id, "name");
  int ia = nt_ref(nt, id, "arguments");
  int ic = 0; const int *iv = ia >= 0 ? nt_arr(nt, ia, "arguments", &ic) : NULL;
  TyKind irt = ir >= 0 ? comp_ntype(c, ir) : TY_UNKNOWN;
  if (ir < 0 || !inm || ic < 1 || !iv || iv[0] < 0) return NULL;
  if (!(ty_is_array(irt) || ty_is_obj_array(irt))) return NULL;
  if (user_defines_or_reads(c, inm)) return NULL;
  static const char *const IDX[] = { "at", "fetch", "first", "last", "take",
                                     "drop", "insert", "dig", "values_at",
                                     "rotate", "[]", "slice", "[]=", NULL };
  static const char *const SLICE_OK[] = { "[]", "slice", "[]=", "values_at", NULL };
  int is_idx = 0, range_ok = 0;
  for (int k = 0; IDX[k]; k++) if (sp_streq(inm, IDX[k])) { is_idx = 1; break; }
  if (!is_idx) return NULL;
  for (int k = 0; SLICE_OK[k]; k++) if (sp_streq(inm, SLICE_OK[k])) { range_ok = 1; break; }
  TyKind at4 = comp_ntype(c, iv[0]);
  if (at4 == TY_STRING || at4 == TY_STRBUF) return "String";
  if (at4 == TY_SYMBOL) return "Symbol";
  if (ty_is_array(at4) || ty_is_obj_array(at4)) return "Array";
  if (ty_is_hash(at4)) return "Hash";
  if (at4 == TY_NIL) return "nil";
  if (at4 == TY_BOOL) return "Boolean";
  if ((at4 == TY_RANGE || at4 == TY_FLOAT_RANGE || at4 == TY_STR_RANGE) && !range_ok)
    return "Range";
  return NULL;
}
/* A method on a REOPENED builtin whose composed C name would be a runtime
   function of the same spelling: sp_String is the shared-mutable String's own
   type, so `class String; def length` emitted an sp_String_length(const char *)
   beside the runtime's sp_String_length(sp_String *) and the program did not
   compile at all. The same holds for dup, freeze, insert, prepend, replace and
   the rest of that family. A reopened method of one of those names takes an
   `_oc` suffix on the class stem instead; every site that composes the name
   goes through here or through mc_reopen_cls below. */
static const char *const sp_rt_string_fns[] = {
  "append", "append_bin", "append_n", "cstr", "dup", "fin", "freeze",
  "insert", "is_frozen", "length", "new", "new_len", "new_shared",
  "prepend", "replace", "set_bin", NULL };
const char *mc_reopen_cls(Compiler *c, int class_id, const char *mname) {
  static char buf[128];
  const char *stem = c->classes[class_id].c_name;
  if (mname && sp_streq(c->classes[class_id].name, "String")) {
    for (int i = 0; sp_rt_string_fns[i]; i++)
      if (sp_streq(mname, sp_rt_string_fns[i])) {
        snprintf(buf, sizeof buf, "%s_oc", stem);
        return buf;
      }
  }
  return stem;
}

const char *mc(const char *name) {
  static char buf[256];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof buf - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    /* operator characters map to distinct tokens so that, e.g., `&` and `|`
       (or `<<` and `>>`) don't mangle to the same C identifier */
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      case '+': tok = "_plus";  break;
      case '-': tok = "_minus"; break;
      case '*': tok = "_star";  break;
      case '/': tok = "_slash"; break;
      case '%': tok = "_pct";   break;
      case '<': tok = "_lt";    break;
      case '>': tok = "_gt";    break;
      case '&': tok = "_amp";   break;
      case '|': tok = "_bar";   break;
      case '^': tok = "_caret"; break;
      case '~': tok = "_tilde"; break;
      case '@': tok = "_at";    break;
      case '[': tok = "_lb";    break;
      case ']': tok = "_rb";    break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  /* A method is emitted as sp_<Class>_<mc(name)>, and sp_<Class>_new is the
     generated constructor: an instance method named `new` redefined it, and
     the call bound to the zero-argument constructor (#4829). Every user of a
     method's C name goes through here, so the rename is seen consistently;
     the class-side `self.new` (sp_<Class>_s_new__m) moves with it. The GC
     scan function, the other generated helper a method name could hit, is
     named sp_<Class>__gc_scan for the same reason. */
  if (strcmp(buf, "new") == 0) return "new__m";
  return buf;
}

/* The names a top-level Ruby method must not take: generated from the runtime
   sources by the Makefile (build/csrc/sp_rt_names.h), because the set is a fact
   about those sources and a hand-kept copy of it goes stale -- `def gcd` and
   `def gets` collided with sp_gcd and sp_gets while the list said nothing. */
#include "sp_rt_names.h"

/* The mangled name of a top-level method: `mc(name)`, with an `rb_` infix when
   the plain form would sit in the runtime's own namespace.

   The segment compared is the whole name when it carries no underscore. A
   prefix names both a runtime NAMESPACE (sp_sym_intern, sp_int_add) and, for
   about a dozen of them, a runtime IDENTIFIER of its own -- the typedefs
   sp_sym and sp_int, sp_raise, sp_thread. Looking only in front of an
   underscore protected the namespace and left the identifier open, so `def
   sym` collided with the typedef and the program failed to build on a C
   diagnostic that never mentions the name the author wrote. */
const char *mc_top(Compiler *c, const char *name) {
  static char buf[272];
  const char *m = mc(name);
  /* A class or module of the same name owns sp_<Name> as its typedef, and
     `def Foo` beside `class Foo` is ordinary Ruby -- URI(), Integer(),
     Array() are all a method sharing a name with a type. Without this the two
     collide and the program fails to build on a C diagnostic that never
     mentions either. Same remedy as the runtime clash below. */
  if (c && comp_class_index(c, name) >= 0) {
    snprintf(buf, sizeof buf, "rb_%s", m);
    return buf;
  }
  const char *us = strchr(m, '_');
  size_t seg = (us && us > m) ? (size_t)(us - m) : strlen(m);
  if (seg > 0) {
    for (int i = 0; SP_RT_PREFIXES[i]; i++) {
      if (strlen(SP_RT_PREFIXES[i]) != seg) continue;
      if (strncmp(m, SP_RT_PREFIXES[i], seg) != 0) continue;
      snprintf(buf, sizeof buf, "rb_%s", m);
      return buf;
    }
  }
  return m;
}
/* Mangle an ivar/struct-member name (sans leading '@') to a valid C field
   identifier, so a Struct/Data member like `verbose?` becomes iv_verbose_p
   rather than the illegal iv_verbose? (#3110). Same character map as mc(), but
   a rotating buffer ring so the 2-4 uses of a field name on one buf_printf line
   don't clobber each other. A no-op for the usual all-identifier ivar names. */
const char *iv_c(const char *name) {
  static char bufs[8][256];
  static int which = 0;
  char *buf = bufs[(which++) & 7];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof bufs[0] - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  return buf;
}
int scope_is_shadowed(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (!sc->name) return 0;
  /* a redefined top-level method: only a later `def` of the same name
     shadows it, and comp_method_index answers that one. Emitting both was
     a C redefinition when the signatures matched. */
  if (sc->class_id < 0) {
    if (sc->def_node < 0 || nt_kind(c->nt, sc->def_node) != NK_DefNode) return 0;
    for (int k = s + 1; k < c->nscopes; k++) {
      Scope *o = &c->scopes[k];
      if (o->class_id < 0 && o->is_cmethod == sc->is_cmethod && o->name &&
          sp_streq(o->name, sc->name) && o->def_node >= 0 && o->def_node != sc->def_node &&
          nt_kind(c->nt, o->def_node) == NK_DefNode) return 1;
    }
    return 0;
  }
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && sp_streq(o->name, sc->name)) return 1;
  }
  return 0;
}
/* True when scope `s` is emitted as a standalone `sp_Class_method` function, so
   a poly-dispatch `case` arm may call it without dangling at link. Mirrors the
   emission gate in codegen.c exactly: a yielding method is inlined at each call
   site (no symbol exists), and a pruned/shadowed/transplanted method is never
   defined. A dispatch arm that targets a scope failing this test references an
   undefined symbol (issues #1583 yields, #1576 pruned). */
/* ---- proc-form emission for yielding methods (#3399) ----
   A yielding method has no symbol: it is inlined at each call site with the
   block spliced in. A poly dispatch has no call site to splice into, so for the
   methods it names we emit a SECOND definition -- an ordinary function taking
   the block as an sp_Proc * -- and point the dispatch at that. Marked during
   dispatch emission (scope_mark_proc_form), emitted afterwards.

   The emission reuses the existing non-yielding shape rather than adding a
   mode: with `yields` cleared the signature already grows the sp_Proc* param
   and roots it, and with g_yield_proc_ref set every `yield` in the body already
   lowers to a call on that proc. begin/end swap those in and back. */
static char **g_pf_flag = NULL;
static int g_pf_cap = 0;
static char g_pf_synth[SP_MAX_PROC_FORM][32];

void scope_mark_proc_form(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return;
  if (!g_pf_flag || g_pf_cap < c->nscopes) {
    char **n = (char **)realloc(g_pf_flag, sizeof(char *) * (size_t)c->nscopes);
    if (!n) return;
    for (int i = g_pf_cap; i < c->nscopes; i++) n[i] = NULL;
    g_pf_flag = n; g_pf_cap = c->nscopes;
  }
  if (g_pf_flag[s] != (char *)2) g_pf_flag[s] = (char *)1;
}
void scope_veto_proc_form(Compiler *c, int s) {
  if (!g_pf_flag || s < 0 || s >= g_pf_cap || s >= c->nscopes) return;
  g_pf_flag[s] = (char *)2;   /* sticky: a later marking pass must not revive it */
}
/* The proc-form clone of scope `s`, or -1. Made in analyze (make_yield_proc_forms):
   a second scope named "<name>#pf" on the same class, holding an independently
   typed copy of the body whose yields answer poly. */
int scope_proc_form_of(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return -1;
  Scope *sc = &c->scopes[s];
  if (!sc->name || sc->class_id < 0 || !sc->yields) return -1;
  char pfname[192];
  snprintf(pfname, sizeof pfname, "%s#pf", sc->name);
  int pi = comp_method_in_class(c, sc->class_id, pfname);
  if (pi < 0 || !c->scopes[pi].is_proc_form) return -1;
  return pi;
}
int scope_needs_proc_form(Compiler *c, int s) {
  return scope_proc_form_of(c, s) >= 0;
}
static int g_pf_saved_yields;
static char *g_pf_saved_blk;
static const char *g_pf_saved_ypr;
static TyKind g_pf_saved_slot;
static TyKind g_pf_saved_ret;
static char g_pf_ref[64];
int g_pf_emitting = 0;
void scope_proc_form_begin(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  g_pf_saved_yields = sc->yields;
  g_pf_saved_blk = sc->blk_param;
  g_pf_saved_ypr = g_yield_proc_ref;
  g_pf_saved_slot = g_yield_slot_ty;
  if (!sc->blk_param || !sc->blk_param[0]) {
    /* a bare `yield` names no block: give the parameter a name of its own */
    int idx = s < SP_MAX_PROC_FORM ? s : 0;
    snprintf(g_pf_synth[idx], sizeof g_pf_synth[0], "__pf_blk");
    sc->blk_param = g_pf_synth[idx];
  }
  sc->yields = 0;
  snprintf(g_pf_ref, sizeof g_pf_ref, "lv_%s", sc->blk_param);
  g_yield_proc_ref = g_pf_ref;
  /* The proc form returns POLY, and it has to: the same yielding method
     inlined at two call sites produces two different C types (an sp_int at
     one, a const char * at the other), because each site is monomorphised
     with its own block. One shared function cannot carry a per-call-site
     return type, so it carries the boxed one and the dispatch unboxes into
     its slot. sp_proc_call already answers through _sp_proc_poly_ret, so
     asking for TY_POLY here is what makes each yield yield a value at all --
     with the method's own (void, since it never needed one) the result was
     computed and dropped. */
  g_pf_saved_ret = sc->ret;
  sc->ret = TY_POLY;
  g_yield_slot_ty = TY_POLY;
  g_pf_emitting = 1;
}
void scope_proc_form_end(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  sc->yields = g_pf_saved_yields;
  sc->blk_param = g_pf_saved_blk;
  sc->ret = g_pf_saved_ret;
  g_yield_proc_ref = g_pf_saved_ypr;
  g_yield_slot_ty = g_pf_saved_slot;
  g_pf_emitting = 0;
}
/* A module whose instance methods a TOP-LEVEL `include` makes callable: the
   bare-call path emits a direct call to the module's own function, so that
   function has to exist even though including the module into a class also
   copied it away (#3795). */
int scope_toplevel_included(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  if (sc->is_cmethod || sc->class_id < 0) return 0;
  for (int i = 0; i < c->ntoplevel_includes; i++)
    if (c->toplevel_includes[i] == sc->class_id) return 1;
  return 0;
}

int scope_has_callable_symbol(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  return sc->reachable && !sc->yields &&
         (!sc->is_transplanted_source || scope_toplevel_included(c, s)) &&
         !scope_is_shadowed(c, s);
}
/* What a keyword flag's value says at compile time: 1 for a literal Ruby
   treats as true (true, a number, a String, a Symbol, a container -- any
   literal but false and nil), 0 for false or nil, -1 when only the run time
   knows. A flag such as `chomp:` was read as true for the TrueNode kind
   alone, so `chomp: 1` did not chomp. */
int kw_flag_static(Compiler *c, int node) {
  const char *t = node >= 0 ? nt_type(c->nt, node) : NULL;
  if (!t) return 0;
  if (sp_streq(t, "FalseNode") || sp_streq(t, "NilNode")) return 0;
  if (sp_streq(t, "TrueNode") || sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
      sp_streq(t, "StringNode") || sp_streq(t, "InterpolatedStringNode") ||
      sp_streq(t, "SymbolNode") || sp_streq(t, "ArrayNode") || sp_streq(t, "HashNode") ||
      sp_streq(t, "RegularExpressionNode") || sp_streq(t, "RangeNode"))
    return 1;
  return -1;
}
/* The C truth of a keyword flag into `out`: "1"/"0" from kw_flag_static, or
   the run-time test in parentheses. An absent flag is "0". */
void emit_kw_flag(Compiler *c, int node, Buf *out) {
  int f = kw_flag_static(c, node);
  if (f >= 0) { buf_printf(out, "%d", f); return; }
  buf_puts(out, "("); emit_cond(c, node, out); buf_puts(out, ")");
}
int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) {
      const char *kn = nt_str(nt, key, "value");
      if (kn && sp_streq(kn, name)) return nt_ref(nt, els[i], "value");
    }
  }
  return -1;
}
int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  if (t == TY_FLOAT_RANGE) return 6;
  if (t == TY_STR_RANGE) return 7;
  return 0;
}
int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  if (t == TY_STRING || t == TY_STRBUF) self_cls = "String";
  else if (t == TY_INT || t == TY_BIGINT) self_cls = "Integer";
  else if (t == TY_FLOAT) self_cls = "Float";
  else if (t == TY_SYMBOL) self_cls = "Symbol";
  else if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) self_cls = "Range";
  else if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  else if (t == TY_NIL) self_cls = "NilClass";
  else if (t == TY_BOOL) self_cls = "Boolean"; /* true/false split handled at call site */
  else if (t == TY_FIBER) self_cls = "Fiber";
  else if (t == TY_THREAD) self_cls = "Thread";
  else if (t == TY_QUEUE) self_cls = "Queue";
  else if (t == TY_MUTEX) self_cls = "Mutex";
  else if (t == TY_CONDVAR) self_cls = "ConditionVariable";
  else if (t == TY_ENUMERATOR) self_cls = "Enumerator";
  else if (t == TY_TIME) self_cls = "Time";
  else if (t == TY_COMPLEX) self_cls = "Complex";
  else if (t == TY_RATIONAL) self_cls = "Rational";
  else if (t == TY_REGEX) self_cls = "Regexp";
  else if (t == TY_MATCHDATA) self_cls = "MatchData";
  else if (t == TY_PROC) self_cls = "Proc";
  else if (t == TY_RANDOM) self_cls = "Random";
  else if (t == TY_IO) self_cls = "IO";
  if (!self_cls) return -1;
  if (sp_streq(cn, self_cls)) return 1;
  if (exact) return 0;
  if (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject") || sp_streq(cn, "Kernel")) return 1;
  if (sp_streq(cn, "Comparable") && (t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
                                     t == TY_FLOAT || t == TY_SYMBOL || t == TY_TIME ||
                                     t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Numeric") && (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
                                  t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE ||
                                     t == TY_ENUMERATOR)) return 1;
  return 0;
}

/* `if (frozen) raise FrozenError` guard preceding an ivar store on a
   freeze-observed class ("can't modify frozen <Name>: <inspect>"); emits
   nothing when instances of the class are never frozen. `selfexpr` is a C
   expression for the instance pointer, evaluated twice (bind a temp first
   when it has effects). */
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b) {
  if (cid < 0 || cid >= c->nclasses) return;
  if (!c->classes[cid].freeze_observed || c->classes[cid].is_value_type) return;
  const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
  buf_printf(b,
      "if (sp_gc_is_frozen((void *)%s)) "
      "sp_raise_frozen_obj(sp_box_obj((void *)%s, %d), (&(\"\\xff\" \"can't modify frozen %s\")[1])); ",
      selfexpr, selfexpr, cid, rn);
}

/* `_t<tmp>` when the node was already evaluated into that temp, or the
   node itself when tmp is -1. */
void emit_node_or_tmp(Compiler *c, int node, int tmp, Buf *b) {
  if (tmp >= 0) buf_printf(b, "_t%d", tmp);
  else emit_expr(c, node, b);
}
/* Root a variable whose C type came from a TyKind. A boxed-poly variable is
   an sp_RbVal, whose first word is a tag rather than a pointer, so it has to
   be rooted through the rbval macro -- rooting it as a raw pointer hands the
   mark walker a small integer and segfaults under GC pressure. Sites that
   emit a temp from a type the inference chose keep getting this wrong one at
   a time, so they go through here; a site that names its variable takes the
   `_var` form, and one that emits around the root asks `ty_gc_rootable`
   first. */
void emit_gc_root_var(Compiler *c, TyKind t, const char *name, Buf *b) {
  if (!ty_gc_rootable(c, t)) return;
  /* a String slot takes the string form: a builder's buffer (marker 0xfd) is
     kept alive by the handle in front of it, which only sp_mark_string
     reaches; the object form's header walk skips the buffer, and reads a
     freed one once the handle is gone (see emit_local_decl) */
  if (t == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(%s);", name);
  else if (t == TY_STRING) buf_printf(b, "SP_GC_ROOT_STR(%s);", name);
  else buf_printf(b, "SP_GC_ROOT(%s);", name);
}
void emit_gc_root_tmp(Compiler *c, TyKind t, int tmp, Buf *b) {
  char name[24]; snprintf(name, sizeof name, "_t%d", tmp);
  emit_gc_root_var(c, t, name, b);
}
/* Whether a variable of this kind takes a root at all. A value-type object
   lives in the variable itself, not behind it: rooting one hands the mark
   walker the struct's first field. */
int ty_gc_rootable(Compiler *c, TyKind t) {
  return needs_root(t) && !comp_ty_value_obj(c, t);
}

/* An arm that hoists its receiver into `_tN` and then evaluates arguments
   holds the receiver in nothing while they run, nor while the call itself
   allocates. This renders the receiver as the initialiser of `_tN` and
   follows it with a root on `_tN`, so the receiver is held until the arm's
   statement expression ends. When this receiver node is in the override
   table (g_argov_node), the operand-order rewrite or an arm that
   re-dispatches through a temp has already declared and rooted the temp the
   receiver renders as, in front of the call, and a second root would only
   repeat it. */
void emit_recv_rooted(Compiler *c, int recv, int t, const char *rootm, Buf *b) {
  emit_expr(c, recv, b);
  int bound = 0;
  for (int i = 0; i < g_n_argov; i++)
    if (g_argov_node[i] == recv) bound = 1;
  if (bound) buf_puts(b, "; ");
  else buf_printf(b, "; %s(_t%d); ", rootm, t);
}
