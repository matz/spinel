/* order_plan.c -- the operand-order record (#11506). See order_plan.h. */
#include "codegen_internal.h"
#include "analyze.h"
#include "order_plan.h"
#include "implicit_calls.h"
#include "codegen_call_arms.h"
#include <limits.h>

/* See order_plan.h. Operands that all answer yes need no ordering and no
   roots: nothing can run between them to collect a value or to move what
   another one reads. A node bound to a temp its value already ran into is
   that temp, whatever its shape, where the walk asks (w->ran). */
static int pure_walk_node(Compiler *c, int id, const OrderWalk *w);
/* Is call id an index read of a typed Array by an Integer (`@ref[addr & 3]`)? */
static int order_typed_index(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int a = nt_ref(nt, id, "arguments"); int ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  return nm && is_aref_name(nm) && recv >= 0 && ac == 1 && nt_ref(nt, id, "block") < 0 &&
         ty_is_array(comp_ntype(c, recv)) && comp_ntype(c, av[0]) == TY_INT;
}
int order_pure_walk(Compiler *c, int id, const OrderWalk *w) {
  if (id < 0 || (w->ran && w->ran(id))) return 1;
  int m = w->memo && id < w->memo_n;
  if (m && w->memo[id]) return w->memo[id] == 1;
  int r = pure_walk_node(c, id, w);
  if (m) w->memo[id] = r ? 1 : 2;
  return r;
}
static int pure_walk_node(Compiler *c, int id, const OrderWalk *w) {
  const NodeTable *nt = c->nt;
  switch (nt_kind(nt, id)) {
    case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
    case NK_ClassVariableReadNode: case NK_SelfNode: case NK_IntegerNode:
    case NK_FloatNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    case NK_SymbolNode:
      return 1;
    /* a constant the program defines reads its static (`cst_NAME`, see
       emit_constant_expr), as an ivar or class variable read does; one read
       during its own Class.new init can raise, and any other spelling (a
       class value, an engine constant, an undefined name) is left out, as is
       one that raises while it is unset */
    case NK_ConstantReadNode: {
      const char *cn = const_key_at(c, id);
      LocalVar *cv = cn ? comp_const(c, cn) : NULL;
      return cv && cv->type != TY_UNKNOWN && !cv->init_guarded && !const_has_flag(cv);
    }
    case NK_ParenthesesNode: case NK_StatementsNode: case NK_ArgumentsNode:
      break;
    case NK_CallNode: {
      int alloc = 0;
      if (!order_typed_index(c, id) && !scalar_op_runs_no_code(c, id) &&
          !(call_is_field_read(c, id, w->alloc ? &alloc : NULL) && !alloc))
        return 0;
      break;
    }
    default:
      return 0;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (!order_pure_walk(c, nt_ref_at(nt, id, i), w)) return 0;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (!order_pure_walk(c, ids[j], w)) return 0;
  }
  return 1;
}

/* ---- the list layer ---- */

static void oplist_push(OpList *l, int node) {
  if (node < 0) return;
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    int *nn = realloc(l->node, sizeof *nn * (size_t)l->cap);
    if (!nn) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    l->node = nn;
  }
  l->node[l->n++] = node;
}
/* the keys and values of a Hash literal's or a keyword hash's elements, in
   order; a `**h` element is one operand (its to_hash) */
static void oplist_assocs(const NodeTable *nt, int hash, OpList *l) {
  int n = 0;
  const int *el = nt_arr(nt, hash, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (nt_kind(nt, el[i]) == NK_AssocNode) {
      oplist_push(l, nt_ref(nt, el[i], "key"));
      oplist_push(l, nt_ref(nt, el[i], "value"));
    }
    else oplist_push(l, el[i]);
  }
}
/* an ArgumentsNode's arguments: a keyword hash flattened, a splat in place */
static void oplist_args(const NodeTable *nt, int args, OpList *l) {
  if (args < 0) return;
  int n = 0;
  const int *av = nt_arr(nt, args, "arguments", &n);
  for (int i = 0; i < n; i++) {
    if (nt_kind(nt, av[i]) == NK_KeywordHashNode) oplist_assocs(nt, av[i], l);
    else oplist_push(l, av[i]);
  }
}
/* the block-pass operand, last: `&expr` (an anonymous `&` forwards a
   parameter and has none) */
static void oplist_block_pass(const NodeTable *nt, int owner, OpList *l) {
  int b = nt_ref(nt, owner, "block");
  if (b >= 0 && nt_kind(nt, b) == NK_BlockArgumentNode && nt_ref(nt, b, "expression") >= 0) oplist_push(l, b);
}
/* See order_plan.h. */
int oplist_of(const Compiler *c, int owner, OpList *l) {
  const NodeTable *nt = c->nt;
  l->n = 0;
  l->guard = OPG_AT_CALL;
  if (owner < 0) return 0;
  switch (nt_kind(nt, owner)) {
    case NK_CallNode:
      l->kind = OPL_CALL;
      oplist_push(l, nt_ref(nt, owner, "receiver"));
      if (nt_safe_nav(nt, owner)) l->guard = OPG_AFTER_RECV;
      oplist_args(nt, nt_ref(nt, owner, "arguments"), l);
      oplist_block_pass(nt, owner, l);
      return 1;
    case NK_SuperNode:
      l->kind = OPL_SUPER;
      oplist_args(nt, nt_ref(nt, owner, "arguments"), l);
      oplist_block_pass(nt, owner, l);
      return 1;
    case NK_YieldNode:
      l->kind = OPL_YIELD;
      oplist_args(nt, nt_ref(nt, owner, "arguments"), l);
      return 1;
    case NK_InterpolatedStringNode: case NK_InterpolatedSymbolNode:
    case NK_InterpolatedXStringNode: case NK_InterpolatedRegularExpressionNode: {
      l->kind = OPL_INTERP;
      int n = 0;
      const int *pv = nt_arr(nt, owner, "parts", &n);
      for (int i = 0; i < n; i++)
        if (nt_kind(nt, pv[i]) != NK_StringNode) oplist_push(l, pv[i]);
      return 1;
    }
    case NK_ArrayNode: {
      l->kind = OPL_ARRAY;
      int n = 0;
      const int *ev = nt_arr(nt, owner, "elements", &n);
      for (int i = 0; i < n; i++) oplist_push(l, ev[i]);
      return 1;
    }
    case NK_HashNode:
      l->kind = OPL_HASH;
      oplist_assocs(nt, owner, l);
      return 1;
    default:
      return 0;
  }
}
void oplist_free(OpList *l) {
  free(l->node);
  memset(l, 0, sizeof *l);
}

/* Is this call a reader on a boxed receiver that every class the receiver
   can hold answers by a public attr_reader or Struct member, with no `def`
   of the name winning over it? The dispatch then reads a field in each arm
   and runs no code of the program's. A builtin value among them (a nil)
   answers with its own method or raises NoMethodError, which runs none
   either when no builtin has the name or every one that has it reads a
   value, the program defines no method of the name a builtin could take,
   and no method_missing. */
static int call_is_poly_field_read(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!nm || recv < 0 || nt_ref(nt, id, "block") >= 0 || nt_ref(nt, id, "arguments") >= 0) return 0;
  if (comp_ntype(c, recv) != TY_POLY) return 0;
  const ClsSet *s = cls_fact_node(c, recv);
  if (!s || s->all || s->os || s->n == 0) return 0;
  if (an_user_defines_method(c, "method_missing")) return 0;
  if (s->bv && (an_user_defines_method(c, nm) ||
                (!is_plain_builtin_reader(nm) &&
                 (builtin_object_method_known(nm) || builtin_instance_name_known(nm)))))
    return 0;
  for (int k = cls_set_next(s, -1); k >= 0; k = cls_set_next(s, k)) {
    if (k >= c->nclasses || c->classes[k].is_native_class) return 0;
    int rdc = -1, mdc = -1;
    if (!comp_reader_in_chain(c, k, nm, &rdc)) return 0;
    if (comp_resolve_member(c, k, nm, 0, &mdc, NULL) != SP_MEMBER_ATTR) return 0;
    if (comp_method_vis_in_chain(c, k, nm) != SP_VIS_PUBLIC) return 0;
  }
  return 1;
}

/* See order_plan.h. An index read of a boxed value runs Ruby code only
   through a `[]` of the program's, a Hash's default block, or the index's
   own methods (to_int, hash, eql?); with none of them it is a lookup (a
   poly Array of Arrays). */
int order_call_runs_no_code(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (scalar_op_runs_no_code(c, id) || order_typed_index(c, id)) return 1;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int a = nt_ref(nt, id, "arguments"); int ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  if (nm && is_aref_name(nm) && recv >= 0 && ac == 1 && nt_ref(nt, id, "block") < 0 &&
      repr_of(c, recv).kind == RK_BOXED && ty_runs_no_code(comp_ntype(c, av[0])) &&
      !any_class_defines(c, "[]") && !prog_has_hash_default_block(c))
    return 1;
  int alloc = 0;
  return call_is_field_read(c, id, &alloc) || call_is_poly_field_read(c, id);
}

/* ---- the record ---- */

/* What the subtree of a code operand may do (order_sum): assign state
   (subtree_may_reassign_state), run a proc (subtree_may_run_proc), make an
   implicit call that may run a method of the program's, or write a
   local. */
enum { OS_KNOWN = 1, OS_REASSIGN = 2, OS_PROC = 4, OS_IMPL = 8, OS_LOCAL = 16 };
/* The nearest code operand to the right of the read being decided that
   may change it, by what the read is (facts_rebinder); NONE for none. */
typedef struct { int any, state, cell; } OrderNext;
enum { ORDER_NONE = INT_MAX };

typedef struct OrderFacts {
  OpOrder *row;
  int nrow, caprow;
  OpSlot *slot;
  int nslot, capslot;
  int n;                  /* the node count the columns cover */
  int *row_of;            /* [n] the owner's row; -1 visited, nothing bound; -2 not visited */
  int *slot_at;           /* [n] the operand's slot, -1 */
  unsigned char *code;    /* [n] the walk's memo: 1 runs no code, 2 runs code */
  /* --dump-order=all: the lists with no row, and why (1 every operand a
     pure read, 2 one code operand last beside immutable values) */
  int *none;
  unsigned char *none_why;
  int nnone, capnone;
  /* while the rows are built: */
  int *par;               /* [n] a node's parent in the tree, -1 a root, -2 out of it */
  unsigned char *inblk;   /* [n] order_in_block's answer, set with par */
  /* [n] order_sum's memo: a list's code operand is often the list below it
     (a chain of calls), so each node is walked once, not once per list
     above it */
  unsigned char *sum;
  /* order_writes_ivar's memo, open addressing: a node's id and an
     instance variable's names index, the class asked for, the answer (1
     no, 2 yes; 0 an empty entry) */
  unsigned long long *ivw_key;
  int *ivw_cls;
  unsigned char *ivw_val;
  int nivw, capivw;
  ANameHash names;        /* the variables the lists name, interned */
  int *next;              /* [names] the nearest code operand that may write it, ORDER_NONE */
  int capnext;
  int *touched, ntouched, captouched;   /* the next[] entries the list set */
  int *ivk, nivk, capivk; /* the instance variables the list reads in its def (names index) */
  unsigned char check;    /* SPINEL_ORDER_CHECK, read when the phase runs */
} OrderFacts;

int g_dump_order = 0;

static void *order_grow(void *p, int *cap, int need, size_t sz) {
  if (need <= *cap) return p;
  int nc = *cap ? *cap : 16;
  while (nc < need) nc *= 2;
  p = realloc(p, sz * (size_t)nc);
  if (!p) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  *cap = nc;
  return p;
}
static OrderFacts *order_facts_new(int n) {
  OrderFacts *f = calloc(1, sizeof *f);
  if (!f) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  f->n = n;
  f->row_of = malloc(sizeof *f->row_of * (size_t)(n + 1));
  f->slot_at = malloc(sizeof *f->slot_at * (size_t)(n + 1));
  f->code = calloc((size_t)n + 1, 1);
  if (!f->row_of || !f->slot_at || !f->code) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < n; i++) { f->row_of[i] = -2; f->slot_at[i] = -1; }
  return f;
}
/* the scratch the rows are built with */
static void order_facts_scratch_free(OrderFacts *f) {
  free(f->par); free(f->inblk); free(f->sum); free(f->next); free(f->touched); free(f->ivk);
  f->inblk = NULL;
  free(f->ivw_key); free(f->ivw_cls); free(f->ivw_val);
  f->ivw_key = NULL; f->ivw_cls = NULL; f->ivw_val = NULL;
  f->nivw = f->capivw = 0;
  anh_free(&f->names);
  memset(&f->names, 0, sizeof f->names);
  f->par = f->next = f->touched = f->ivk = NULL;
  f->sum = NULL;
  f->capnext = f->captouched = f->capivk = 0;
}
static void order_facts_del(OrderFacts *f) {
  if (!f) return;
  free(f->row); free(f->slot); free(f->row_of); free(f->slot_at); free(f->code);
  free(f->none); free(f->none_why);
  order_facts_scratch_free(f);
  free(f);
}
void order_facts_free(Compiler *c) {
  order_facts_del(c->order);
  c->order = NULL;
}

static int facts_runs_code(Compiler *c, OrderFacts *f, int node) {
  OrderWalk w = { NULL, 0, f->code, f->n };
  return !order_pure_walk(c, node, &w);
}
/* See order_plan.h. */
int order_runs_code(Compiler *c, int node) {
  if (c->order && node >= 0 && node < c->order->n) return facts_runs_code(c, c->order, node);
  OrderWalk w = { NULL, 0, NULL, 0 };
  return !order_pure_walk(c, node, &w);
}

/* The program's tree, top-down from the file roots and from each scope's
   def and body (a proc-form clone is a scope's body and no root's child),
   each node once, into f->par: a node's parent in the tree, -1 for a root,
   -2 for a node out of it, and into f->inblk, order_in_block's answer. A
   desugar leaves the nodes it replaced in the table, out of the tree; a
   node it reused is in it under the node that holds it now. */
static void order_tree_mark(const Compiler *c, OrderFacts *f) {
  const NodeTable *nt = c->nt;
  int n = f->n, sp = 0;
  int *stack = malloc(sizeof *stack * (size_t)(n + 1));
  f->par = malloc(sizeof *f->par * (size_t)(n + 1));
  f->inblk = calloc((size_t)n + 1, 1);
  if (!stack || !f->par || !f->inblk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < n; i++) f->par[i] = -2;
  int nroots = nt->nfile_roots ? nt->nfile_roots : 1;
  for (int r = 0; r < nroots + 2 * c->nscopes; r++) {
    int x = r < nroots ? nt_root(nt, r)
          : (r - nroots) % 2 ? c->scopes[(r - nroots) / 2].body : c->scopes[(r - nroots) / 2].def_node;
    if (x < 0 || x >= n || f->par[x] != -2) continue;
    f->par[x] = -1;
    stack[sp++] = x;
    while (sp > 0) {
      int p = stack[--sp];
      NodeKind pk = nt_kind(nt, p);
      unsigned char blk = pk == NK_BlockNode || pk == NK_LambdaNode ? 1 : pk == NK_DefNode ? 0 : f->inblk[p];
      int nr = nt_num_refs(nt, p);
      for (int i = 0; i < nr; i++) {
        int ch = nt_ref_at(nt, p, i);
        if (ch >= 0 && ch < n && f->par[ch] == -2) { f->par[ch] = p; f->inblk[ch] = blk; stack[sp++] = ch; }
      }
      int na = nt_num_arrs(nt, p);
      for (int i = 0; i < na; i++) {
        int k = 0;
        const int *ids = nt_arr_at(nt, p, i, &k);
        for (int j = 0; j < k; j++)
          if (ids[j] >= 0 && ids[j] < n && f->par[ids[j]] == -2) { f->par[ids[j]] = p; f->inblk[ids[j]] = blk; stack[sp++] = ids[j]; }
      }
    }
  }
  free(stack);
}

/* Is `x` inside a block or lambda of its def (the tree), where self may be
   another object (instance_exec, define_method)? Its nearest block,
   lambda or def above it decides; order_tree_mark sets it for each node. */
static int order_in_block(const OrderFacts *f, int x) {
  return x >= 0 && x < f->n && f->inblk[x];
}
/* The index of variable name `nm` among the interned names, with its
   next[] entry. */
static int order_name(OrderFacts *f, const char *nm) {
  int k = anh_find(&f->names, nm);
  if (k >= 0) return k;
  anh_add(&f->names, nm);
  k = f->names.n - 1;
  f->next = order_grow(f->next, &f->capnext, k + 1, sizeof *f->next);
  f->next[k] = ORDER_NONE;
  return k;
}
static void order_next_set(OrderFacts *f, int k, int j) {
  if (f->next[k] == ORDER_NONE) {
    f->touched = order_grow(f->touched, &f->captouched, f->ntouched + 1, sizeof *f->touched);
    f->touched[f->ntouched++] = k;
  }
  f->next[k] = j;
}
/* The OS_ bits of the subtree at `id`, from each node's own answers
   (node_may_reassign_state, node_may_run_proc; an implicit call that may
   run a method of the program's, ic_node_may_call_program, which a call
   that runs no code itself, order_call_runs_no_code, does not make; a
   local's write), each node once (f->sum). */
static unsigned order_sum(Compiler *c, OrderFacts *f, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  int m = id < f->n;
  if (m && f->sum[id]) return f->sum[id];
  NodeKind k = nt_kind(nt, id);
  int own[4] = {
    node_may_reassign_state(c, id), node_may_run_proc(c, id),
    !(k == NK_CallNode && order_call_runs_no_code(c, id)) && ic_node_may_call_program(c, id) ? 1 : -1,
    comp_is_local_write(k) ? 1 : -1,
  };
  unsigned r = OS_KNOWN, ask = 0;
  for (int b = 0; b < 4; b++) {
    if (own[b] > 0) r |= OS_REASSIGN << b;
    else if (own[b] < 0) ask |= OS_REASSIGN << b;
  }
  int nr = ask ? nt_num_refs(nt, id) : 0;
  for (int i = 0; i < nr; i++) r |= order_sum(c, f, nt_ref_at(nt, id, i)) & ask;
  int na = ask ? nt_num_arrs(nt, id) : 0;
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) r |= order_sum(c, f, ids[j]) & ask;
  }
  if (m) f->sum[id] = (unsigned char)r;
  return r;
}
/* each local the subtree at `id` writes: its next[] is j */
static void order_note_local_writes(Compiler *c, OrderFacts *f, int id, int j) {
  const NodeTable *nt = c->nt;
  if (!(order_sum(c, f, id) & OS_LOCAL)) return;
  if (comp_is_local_write(nt_kind(nt, id))) {
    const char *wn = nt_str(nt, id, "name");
    if (wn) order_next_set(f, order_name(f, wn), j);
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) order_note_local_writes(c, f, nt_ref_at(nt, id, i), j);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int k = 0;
    const int *ids = nt_arr_at(nt, id, i, &k);
    for (int m = 0; m < k; m++) order_note_local_writes(c, f, ids[m], j);
  }
}
/* The entry of order_writes_ivar's memo for (id, k, cls): its slot. */
static int order_ivw_slot(const OrderFacts *f, int id, int k, int cls) {
  unsigned long long key = (unsigned long long)(unsigned)id << 32 | (unsigned)k;
  unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(f->capivw - 1);
  while (f->ivw_val[h] && (f->ivw_key[h] != key || f->ivw_cls[h] != cls)) h = (h + 1) & (unsigned)(f->capivw - 1);
  return (int)h;
}
static void order_ivw_put(OrderFacts *f, int id, int k, int cls, int r) {
  if (2 * (f->nivw + 1) > f->capivw) {
    unsigned long long *ok = f->ivw_key; int *oc = f->ivw_cls; unsigned char *ov = f->ivw_val;
    int ocap = f->capivw;
    f->capivw = ocap ? ocap * 2 : 1024;
    f->ivw_key = malloc(sizeof *f->ivw_key * (size_t)f->capivw);
    f->ivw_cls = malloc(sizeof *f->ivw_cls * (size_t)f->capivw);
    f->ivw_val = calloc((size_t)f->capivw, 1);
    if (!f->ivw_key || !f->ivw_cls || !f->ivw_val) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!ov[i]) continue;
      int h = order_ivw_slot(f, (int)(ok[i] >> 32), (int)(unsigned)ok[i], oc[i]);
      f->ivw_key[h] = ok[i]; f->ivw_cls[h] = oc[i]; f->ivw_val[h] = ov[i];
    }
    free(ok); free(oc); free(ov);
  }
  int h = order_ivw_slot(f, id, k, cls);
  f->ivw_key[h] = (unsigned long long)(unsigned)id << 32 | (unsigned)k;
  f->ivw_cls[h] = cls;
  f->ivw_val[h] = r ? 2 : 1;
  f->nivw++;
}
/* subtree_may_write_ivar(c, id, names[k], cls, 0), from each node's own
   answer (node_may_write_ivar), each node once per instance variable and
   class: a chain's lists each ask of the list below. */
static int order_writes_ivar(Compiler *c, OrderFacts *f, int id, int k, int cls) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  if (f->capivw) {
    int h = order_ivw_slot(f, id, k, cls);
    if (f->ivw_val[h]) return f->ivw_val[h] == 2;
  }
  int r = node_may_write_ivar(c, id, f->names.key[k], cls, 0);
  if (r < 0) {
    r = 0;
    int nr = nt_num_refs(nt, id);
    for (int i = 0; i < nr && !r; i++) r = order_writes_ivar(c, f, nt_ref_at(nt, id, i), k, cls);
    int na = nt_num_arrs(nt, id);
    for (int i = 0; i < na && !r; i++) {
      int n = 0;
      const int *ids = nt_arr_at(nt, id, i, &n);
      for (int j = 0; j < n && !r; j++) r = order_writes_ivar(c, f, ids[j], k, cls);
    }
  }
  order_ivw_put(f, id, k, cls, r);
  return r;
}
/* An instance variable read in a def of its class, outside a block, is
   changed only by a method of self's that may write it
   (subtree_may_write_ivar); its scope's class, or -1 for any other. */
static int order_ivar_class(Compiler *c, const OrderFacts *f, int x) {
  Scope *xs = comp_scope_of(c, x);
  if (!xs || xs->def_node < 0 || xs->is_cmethod || xs->class_id < 0 || order_in_block(f, x)) return -1;
  return xs->class_id;
}
/* Code operand j of list l is now the nearest to the right: what it may
   change. */
static void facts_next_at(Compiler *c, OrderFacts *f, const OpList *l, int j, int cls, OrderNext *nx) {
  unsigned s = order_sum(c, f, l->node[j]);
  nx->any = j;
  if (s & (OS_REASSIGN | OS_IMPL)) nx->state = j;
  if (s & (OS_PROC | OS_IMPL)) nx->cell = j;
  order_note_local_writes(c, f, l->node[j], j);
  for (int k = 0; k < f->nivk; k++)
    if (s & OS_IMPL || (s & OS_REASSIGN && order_writes_ivar(c, f, l->node[j], f->ivk[k], cls)))
      order_next_set(f, f->ivk[k], j);
}
static int order_min(int a, int b) { return a < b ? a : b; }
/* The nearest code operand to the right that may change what read `x`
   reads (nx and next[], as facts_next_at left them), or -1. A local: one
   that writes it, or, when a proc assigns its cell, one that may run a
   proc. An instance variable: in a def of its class, outside a block, one
   that may assign it (a method of self's that writes it); anywhere else
   one that may assign state (node_may_reassign_state: a write or a target
   of one, `rescue => @@x`, or a call that may run code). A global or a
   class variable: the same. A literal: none. Anything else read (a
   constant, an element, a field, arithmetic over reads): any code. An
   implicit call that may run a method of the program's counts as all of
   these but a local's write. The emission form (rebound_walk) asks the
   same of the variables, with the view's bindings and its self, and asks
   an effect (subtree_has_side_effect) of a global's or a class
   variable's too. */
static int facts_rebinder(Compiler *c, OrderFacts *f, int x, int cls, const OrderNext *nx) {
  const NodeTable *nt = c->nt;
  int r = ORDER_NONE;
  switch (nt_kind(nt, x)) {
    case NK_LocalVariableReadNode: {
      const char *nm = nt_str(nt, x, "name");
      if (!nm) return -1;
      int k = order_name(f, nm);
      r = f->next[k];
      Scope *xs = comp_scope_of(c, x);
      LocalVar *lv = xs ? scope_local(xs, nm) : NULL;
      if (lv && lv->is_cell && lv->proc_rebinds) r = order_min(r, nx->cell);
      break;
    }
    case NK_InstanceVariableReadNode: {
      const char *iv = nt_str(nt, x, "name");
      int k = iv && cls >= 0 && order_ivar_class(c, f, x) == cls ? order_name(f, iv) : -1;
      r = k >= 0 ? f->next[k] : nx->state;
      break;
    }
    case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode:
      r = nx->state;
      break;
    case NK_IntegerNode: case NK_FloatNode: case NK_NilNode: case NK_TrueNode:
    case NK_FalseNode: case NK_SymbolNode: case NK_SelfNode:
      return -1;
    default:
      r = nx->any;
      break;
  }
  return r == ORDER_NONE ? -1 : r;
}
/* Is operand `x`, which runs no code, a value no code to its right can
   change (facts_rebinder): a literal, or a variable's read of a scalar
   (rule 1)? An element or a field read is not: a later call may store into
   its container. */
static int order_immutable(Compiler *c, OrderFacts *f, int x, int cls, const OrderNext *nx) {
  switch (nt_kind(c->nt, x)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_NilNode: case NK_TrueNode:
    case NK_FalseNode: case NK_SymbolNode: case NK_SelfNode:
      return 1;
    case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
    case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode: {
      TyKind t = comp_ntype(c, x);
      return (t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_SYMBOL || t == TY_NIL) &&
             facts_rebinder(c, f, x, cls, nx) < 0;
    }
    default:
      return 0;
  }
}
static void facts_none(OrderFacts *f, int owner, int why) {
  f->row_of[owner] = -1;
  if (g_dump_order < 2) return;
  if (f->nnone == f->capnone) {
    f->capnone = f->capnone ? f->capnone * 2 : 64;
    f->none = realloc(f->none, sizeof *f->none * (size_t)f->capnone);
    f->none_why = realloc(f->none_why, (size_t)f->capnone);
    if (!f->none || !f->none_why) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  f->none[f->nnone] = owner;
  f->none_why[f->nnone++] = (unsigned char)why;
}
/* The rule (order-plan DESIGN 3.1) over the operands o0..on-1 of one list:
   - code(i): oi runs code (order_runs_code); L is the last such. None: no
     row (rule 0).
   - Rule 1: when every operand before oL runs no code and is immutable
     (order_immutable), and oL may change no read after it
     (facts_rebinder), oL stays inline too: no row.
   - Otherwise every code operand is AHEAD, in its place; a read before L is
     HOLD when an AHEAD operand after it may change what it reads (`by`
     names the nearest), else INLINE, read at the call.
   - OPF_ACROSS: an AHEAD operand runs after it, i.e. it comes before L.
   - The guard point: after the receiver for `&.`, else at the call. EFFECT
     and steps come from rows the record does not read yet.
   One pass from the right, which keeps the nearest code operand that may
   change each kind of read: linear in the list, with each operand's
   subtree walked once for the program (order_sum, order_writes_ivar). */
static void facts_decide(Compiler *c, OrderFacts *f, int owner, const OpList *l) {
  int n = l->n, last = -1, cls = -1;
  for (int i = 0; i < n; i++)
    if (facts_runs_code(c, f, l->node[i])) last = i;
  if (last < 0) { facts_none(f, owner, 1); return; }
  /* the instance variables read in their class's def: next[] per name */
  f->nivk = 0;
  for (int i = 0; i < n; i++) {
    int x = l->node[i];
    const char *iv = nt_kind(c->nt, x) == NK_InstanceVariableReadNode ? nt_str(c->nt, x, "name") : NULL;
    int xc = iv ? order_ivar_class(c, f, x) : -1;
    if (xc < 0 || (cls >= 0 && xc != cls)) continue;
    cls = xc;
    int k = order_name(f, iv);
    int seen = 0;
    for (int m = 0; m < f->nivk && !seen; m++) seen = f->ivk[m] == k;
    if (seen) continue;
    f->ivk = order_grow(f->ivk, &f->capivk, f->nivk + 1, sizeof *f->ivk);
    f->ivk[f->nivk++] = k;
  }
  OrderNext nx = { ORDER_NONE, ORDER_NONE, ORDER_NONE };
  facts_next_at(c, f, l, last, cls, &nx);
  int tail = 1;
  for (int i = 0; i < last && tail; i++)
    if (facts_runs_code(c, f, l->node[i]) || !order_immutable(c, f, l->node[i], cls, &nx)) tail = 0;
  /* ...and nothing read after oL is something oL may change: `m[2] = @s`
     with m appending to @s reads @s after m ran */
  for (int i = last + 1; i < n && tail; i++)
    if (facts_rebinder(c, f, l->node[i], cls, &nx) >= 0) tail = 0;
  if (tail) facts_none(f, owner, 2);
  else {
    f->row = order_grow(f->row, &f->caprow, f->nrow + 1, sizeof *f->row);
    f->slot = order_grow(f->slot, &f->capslot, f->nslot + n, sizeof *f->slot);
    OpOrder *r = &f->row[f->nrow];
    r->owner = owner; r->kind = l->kind; r->guard = l->guard; r->steps = 0;
    r->first = f->nslot; r->n = n;
    for (int i = n - 1; i >= 0; i--) {
      OpSlot *s = &f->slot[f->nslot + i];
      int x = l->node[i];
      s->node = x; s->by = -1; s->flags = i < last ? OPF_ACROSS : 0;
      if (facts_runs_code(c, f, x)) {
        s->at = OPA_AHEAD; s->flags |= OPF_CODE;
        if (i != last) facts_next_at(c, f, l, i, cls, &nx);
      }
      else {
        s->at = OPA_INLINE;
        if (i < last && (s->by = facts_rebinder(c, f, x, cls, &nx)) >= 0) s->at = OPA_HOLD;
      }
      if (x >= 0 && x < f->n) f->slot_at[x] = f->nslot + i;
    }
    if (owner < f->n) f->row_of[owner] = f->nrow;
    f->nslot += n;
    f->nrow++;
  }
  for (int k = 0; k < f->ntouched; k++) f->next[f->touched[k]] = ORDER_NONE;
  f->ntouched = 0;
}
static OrderFacts *order_facts_make(Compiler *c) {
  const NodeTable *nt = c->nt;
  OrderFacts *f = order_facts_new(nt->count);
  order_tree_mark(c, f);
  f->sum = calloc((size_t)f->n + 1, 1);
  if (!f->sum) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  OpList l; memset(&l, 0, sizeof l);
  for (int id = 0; id < nt->count; id++) {
    if (f->par[id] == -2 || !oplist_of(c, id, &l) || l.n == 0) continue;
    facts_decide(c, f, id, &l);
  }
  oplist_free(&l);
  order_facts_scratch_free(f);
  return f;
}
/* See order_plan.h. */
void order_facts_build(Compiler *c) {
  order_facts_free(c);
  c->order = order_facts_make(c);
  c->order->check = getenv("SPINEL_ORDER_CHECK") != NULL;
}

/* ---- lookups ---- */

/* See order_plan.h. */
const OpOrder *op_order_of(const Compiler *c, int owner) {
  const OrderFacts *f = c->order;
  if (!f || owner < 0 || owner >= f->n || f->row_of[owner] < 0) return NULL;
  return &f->row[f->row_of[owner]];
}
int op_order_visited(const Compiler *c, int owner) {
  const OrderFacts *f = c->order;
  return f && owner >= 0 && owner < f->n && f->row_of[owner] != -2;
}
const OpSlot *op_slot_of(const Compiler *c, int owner, int node) {
  const OpOrder *r = op_order_of(c, owner);
  const OrderFacts *f = c->order;
  if (!r || node < 0 || node >= f->n) return NULL;
  int s = f->slot_at[node];
  return s >= r->first && s < r->first + r->n ? &f->slot[s] : NULL;
}
int op_held_across(const Compiler *c, int node) {
  const OrderFacts *f = c->order;
  if (!f || node < 0 || node >= f->n || f->slot_at[node] < 0) return 0;
  return (f->slot[f->slot_at[node]].flags & OPF_ACROSS) != 0;
}

/* ---- --dump-order ---- */

static const char *order_at_name(int at) {
  switch (at) {
    case OPA_INLINE: return "INLINE";
    case OPA_AHEAD: return "AHEAD";
    case OPA_HOLD: return "HOLD";
    default: return "EFFECT";
  }
}
static const char *order_kind_name(int k) {
  switch (k) {
    case OPL_CALL: return "call";
    case OPL_SUPER: return "super";
    case OPL_YIELD: return "yield";
    case OPL_INTERP: return "interp";
    case OPL_ARRAY: return "array";
    default: return "hash";
  }
}
/* a short spelling of operand x: a variable's name, a literal, a call's
   receiver and name one level deep, else its node kind */
static void order_operand_text(const Compiler *c, int x, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, x, "name");
  switch (nt_kind(nt, x)) {
    case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode:
    case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode: case NK_ConstantReadNode:
      buf_puts(b, nm ? nm : "?");
      return;
    case NK_IntegerNode:
      buf_printf(b, "%lld", (long long)nt_int(nt, x, "value", 0));
      return;
    case NK_CallNode: {
      int r = nt_ref(nt, x, "receiver");
      if (r >= 0) {
        const char *rn = nt_str(nt, r, "name");
        NodeKind rk = nt_kind(nt, r);
        if (rn && (rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode ||
                   rk == NK_GlobalVariableReadNode || rk == NK_ConstantReadNode || rk == NK_CallNode))
          buf_printf(b, "%s.", rn);
        else buf_puts(b, "(..).");
      }
      buf_puts(b, nm ? nm : "?");
      return;
    }
    default: {
      const char *t = nt_type(nt, x);
      size_t n = t ? strlen(t) : 0;
      if (n > 4 && sp_streq(t + n - 4, "Node")) n -= 4;
      buf_printf(b, "%.*s", (int)n, t ? t : "?");
      return;
    }
  }
}
/* the owner's place and name: `f.rb:12 call center` */
static void order_owner_text(const Compiler *c, int owner, int kind, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *fp = nt_file_path(nt, (int)nt_int(nt, owner, "node_file", 0));
  const char *base = fp ? strrchr(fp, '/') : NULL;
  buf_printf(b, "%s:%lld %s", base ? base + 1 : fp ? fp : "?", (long long)nt_int(nt, owner, "node_line", 0),
             order_kind_name(kind));
  const char *nm = kind == OPL_CALL ? nt_str(nt, owner, "name") : NULL;
  if (nm) buf_printf(b, " %s", nm);
}
typedef struct { long long file, line, col; int owner; char *text; } OrderLine;
static int order_line_cmp(const void *pa, const void *pb) {
  const OrderLine *a = pa, *b = pb;
  if (a->file != b->file) return a->file < b->file ? -1 : 1;
  if (a->line != b->line) return a->line < b->line ? -1 : 1;
  if (a->col != b->col) return a->col < b->col ? -1 : 1;
  return a->owner < b->owner ? -1 : a->owner > b->owner;
}
static void order_line_add(const Compiler *c, OrderLine **v, int *n, int *cap, int owner, Buf *b) {
  const NodeTable *nt = c->nt;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *v = realloc(*v, sizeof **v * (size_t)*cap);
    if (!*v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*v)[(*n)++] = (OrderLine){ nt_int(nt, owner, "node_file", 0), nt_int(nt, owner, "node_line", 0),
                              nt_int(nt, owner, "node_col", 0), owner, b->p };
  memset(b, 0, sizeof *b);
}
/* See order_plan.h. One line per row, sorted by place:
   `f.rb:12 call center: 0 s HOLD(by 2) across | 1 10 INLINE across | 2 g.call AHEAD code | guard=call steps=0`;
   with =all, a list with no row reads `none (pure)` (every operand a pure
   read) or `none (tail)` (one code operand, last, beside immutable values). */
char *order_dump(Compiler *c) {
  const OrderFacts *f = c->order;
  OrderLine *v = NULL;
  int n = 0, cap = 0;
  Buf b; memset(&b, 0, sizeof b);
  for (int k = 0; f && k < f->nrow; k++) {
    const OpOrder *r = &f->row[k];
    order_owner_text(c, r->owner, r->kind, &b);
    buf_puts(&b, ":");
    for (int i = 0; i < r->n; i++) {
      const OpSlot *s = &f->slot[r->first + i];
      buf_printf(&b, "%s %d ", i ? " |" : "", i);
      order_operand_text(c, s->node, &b);
      buf_printf(&b, " %s", order_at_name(s->at));
      if (s->at == OPA_HOLD) buf_printf(&b, "(by %d)", s->by);
      if (s->flags & OPF_CODE) buf_puts(&b, " code");
      if (s->flags & OPF_ACROSS) buf_puts(&b, " across");
    }
    buf_printf(&b, " | guard=%s steps=%d", r->guard == OPG_AFTER_RECV ? "recv" : "call", r->steps);
    order_line_add(c, &v, &n, &cap, r->owner, &b);
  }
  for (int k = 0; f && k < f->nnone; k++) {
    OpList l; memset(&l, 0, sizeof l);
    oplist_of(c, f->none[k], &l);
    order_owner_text(c, f->none[k], l.kind, &b);
    buf_printf(&b, ": none (%s)", f->none_why[k] == 1 ? "pure" : "tail");
    oplist_free(&l);
    order_line_add(c, &v, &n, &cap, f->none[k], &b);
  }
  qsort(v, (size_t)n, sizeof *v, order_line_cmp);
  Buf out; memset(&out, 0, sizeof out);
  for (int k = 0; k < n; k++) { buf_printf(&out, "%s\n", v[k].text ? v[k].text : ""); free(v[k].text); }
  free(v);
  if (!out.p) buf_puts(&out, "");
  return out.p;
}

/* ---- SPINEL_ORDER_CHECK ---- */

/* do the rows of one owner in two builds differ (NULL: no row)? */
static int order_rows_differ(const OrderFacts *a, const OpOrder *ra, const OrderFacts *b, const OpOrder *rb) {
  if (!ra || !rb) return ra != rb;
  if (ra->kind != rb->kind || ra->guard != rb->guard || ra->steps != rb->steps || ra->n != rb->n) return 1;
  for (int i = 0; i < ra->n; i++) {
    const OpSlot *x = &a->slot[ra->first + i], *y = &b->slot[rb->first + i];
    if (x->node != y->node || x->at != y->at || x->flags != y->flags || x->by != y->by) return 1;
  }
  return 0;
}
static void order_check_report(const OrderFacts *f, const OpOrder *r, const char *who) {
  fprintf(stderr, "  %s:", who);
  if (!r) { fprintf(stderr, " no row\n"); return; }
  for (int i = 0; i < r->n; i++) {
    const OpSlot *s = &f->slot[r->first + i];
    fprintf(stderr, " %d %s%s%s", i, order_at_name(s->at), s->flags & OPF_CODE ? " code" : "",
            s->flags & OPF_ACROSS ? " across" : "");
  }
  fputc('\n', stderr);
}
/* See order_plan.h. */
void order_facts_check(Compiler *c) {
  OrderFacts *was = c->order;
  if (!was || !was->check) return;
  OrderFacts *now = order_facts_make(c);
  int bad = 0;
  for (int id = 0; id < now->n; id++) {
    /* a list of the phase's nodes in the tree now must have been seen */
    if (id < was->n && was->row_of[id] == -2 && now->row_of[id] != -2) {
      bad++;
      fprintf(stderr, "spinel: SPINEL_ORDER_CHECK: the list of node %d (%s, line %lld) is in the tree at the seal and the phase did not see it\n",
              id, nt_type(c->nt, id) ? nt_type(c->nt, id) : "?", (long long)nt_int(c->nt, id, "node_line", 0));
      continue;
    }
    /* (a node made after the phase has no row there) */
    const OpOrder *a = id < was->n && was->row_of[id] >= 0 ? &was->row[was->row_of[id]] : NULL;
    const OpOrder *b = now->row_of[id] >= 0 ? &now->row[now->row_of[id]] : NULL;
    if (!order_rows_differ(was, a, now, b)) continue;
    bad++;
    fprintf(stderr, "spinel: SPINEL_ORDER_CHECK: the row of node %d (%s, line %lld) moved between the phase and the seal\n",
            id, nt_type(c->nt, id) ? nt_type(c->nt, id) : "?", (long long)nt_int(c->nt, id, "node_line", 0));
    order_check_report(was, a, "phase");
    order_check_report(now, b, "seal");
  }
  order_facts_del(now);
  if (bad) {
    fprintf(stderr, "spinel: SPINEL_ORDER_CHECK: %d list%s differ%s\n", bad, bad == 1 ? "" : "s", bad == 1 ? "s" : "");
    exit(1);
  }
}
