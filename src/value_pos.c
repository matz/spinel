/* value_pos.c -- the nodes an expression's value can come from (see
   value_pos.h).

   The walk is a depth-first loop over a stack of items still to visit. An item
   is a node and the place it hangs from; a visit peels the wrappers
   (parentheses, statements, else and an `in` clause) in a loop, then either
   pushes the node's value positions (last first, so the first pops first) or
   hands the node to the callback as a leaf. A chain (`elsif`, a run of rescue
   clauses) is a node pushing its successor, so it costs stack entries, not
   frames.

   vp_step holds the switch. Its uniform arms are generated from
   tools/value_edges.yml (value_pos_arms.inc, `make value-edges-regen`); the
   arms below it are the rules that are evaluation semantics, not edges, each
   named in the annotation as role `hand`.

   The same value fields are read the other way for the walkers that ask how a
   node's parent uses its value (vp_is_value_edge, vp_climb): vp_up_edge's
   uniform arms are generated too (value_pos_edges.inc), so the two
   directions come from one annotation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "value_pos.h"
#include "analyze.h"

typedef struct {
  int node;     /* the node to visit, or -1: nil by absence */
  int parent;   /* where it hangs from, as VpLeaf says */
  int arm;
  int depth;
  int link;     /* pushed as the next link of a chain: its arm numbering
                   carries on from `arm` */
  unsigned reach;   /* VPR_* bits, inherited from the item it was pushed by */
} VpItem;

enum { VP_SMALL = 32 };

/* The shapes a walk or a climb sees through: those named, none with VPS_NONE,
   every one when no VPS_ bit is given. */
static unsigned vp_shapes(unsigned flags) {
  if (flags & VPS_ALL) return flags & VPS_ALL;
  return (flags & VPS_NONE) ? 0 : VPS_ALL;
}

typedef struct {
  const NodeTable *nt;
  unsigned flags;     /* the VP_* switches */
  unsigned shapes;    /* the VPS_* shapes that are on */
  VpFn fn;
  void *ud;
  long budget;        /* visits left: a tree cannot take more than one per node */
  unsigned reach;     /* the VPR_* bits of the item being visited: what vp_push gives its children */
  unsigned char *seen; /* when set (the dump's check), a mark for each node the walk visits */
  VpItem *items;      /* the stack: `small` first, the heap past it */
  int n, cap;
  VpItem small[VP_SMALL];
} VpWalk;

/* what a step did with the item */
typedef enum {
  VPSTEP_LEAF,      /* the node is a leaf */
  VPSTEP_JUMP,      /* a jump that is not followed */
  VPSTEP_PEEL,      /* a wrapper: the item now holds its child */
  VPSTEP_EXPANDED,  /* the value positions are pushed */
  VPSTEP_LEAF_THEN_EXPANDED  /* the node is a leaf, and its positions follow it */
} VpStep;

static void vp_push(VpWalk *w, int node, int parent, int arm, int depth, int link) {
  if (w->n == w->cap) {
    int cap = w->cap * 2;
    VpItem *p;
    if (w->items == w->small) {
      p = malloc((size_t)cap * sizeof *p);
      if (p) memcpy(p, w->small, (size_t)w->n * sizeof *p);
    }
    else p = realloc(w->items, (size_t)cap * sizeof *p);
    if (!p) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    w->items = p;
    w->cap = cap;
  }
  VpItem *it = &w->items[w->n++];
  it->node = node; it->parent = parent; it->arm = arm; it->depth = depth; it->link = link;
  it->reach = w->reach;
}

static int vp_emit(VpWalk *w, const VpItem *it, VpKind kind) {
  VpLeaf lf;
  lf.node = it->node; lf.kind = kind; lf.parent = it->parent; lf.arm = it->arm;
  lf.cond_depth = it->depth;
  lf.reach = it->reach;
  return w->fn(&lf, w->ud);
}

/* ---- the rules that are not a list of edges ---- */

/* A begin: its body unless an `else` follows (the else's value replaces it:
   VP_BEGIN_BOTH keeps both), each rescue clause, then the else; never the
   ensure clause. A begin with none of the three is only its body, a wrapper
   like parentheses. */
static VpStep vp_begin(VpWalk *w, VpItem *cur) {
  const NodeTable *nt = w->nt;
  int n = cur->node;
  int body = nt_ref(nt, n, "statements"), rescue = nt_ref(nt, n, "rescue_clause");
  int els = nt_ref(nt, n, "else_clause"), ensure = nt_ref(nt, n, "ensure_clause");
  if (rescue < 0 && els < 0 && ensure < 0) {
    if (!(w->shapes & VPS_BEGIN_PLAIN)) return VPSTEP_LEAF;
    cur->node = body;
    cur->link = 0;
    return VPSTEP_PEEL;
  }
  if (!(w->shapes & VPS_BEGIN_CLAUSES)) return VPSTEP_LEAF;
  int nres = 0;
  for (int r = rescue; r >= 0 && nres < nt->count; r = nt_ref(nt, r, "subsequent")) nres++;
  int d = cur->depth + 1;
  if (els >= 0) vp_push(w, els, n, 1 + nres, d, 0);
  if (rescue >= 0) vp_push(w, rescue, n, 1, d, 1);
  if (els < 0 || (w->flags & VP_BEGIN_BOTH)) vp_push(w, body, n, 0, d, 0);
  return VPSTEP_EXPANDED;
}

/* `a && b`: the right side, and the left side (the value when it is falsy)
   only under VP_AND_LEFT. */
static VpStep vp_and(VpWalk *w, VpItem *cur) {
  if (!(w->shapes & VPS_LOGIC)) return VPSTEP_LEAF;
  int n = cur->node, d = cur->depth + 1;
  vp_push(w, nt_ref(w->nt, n, "right"), n, 1, d, 0);
  if (w->flags & VP_AND_LEFT) vp_push(w, nt_ref(w->nt, n, "left"), n, 0, d, 0);
  return VPSTEP_EXPANDED;
}

/* case/in: the in clauses and the else. Without an else CRuby raises
   NoMatchingPatternError, which VP_CASE_IN_RAISE_NIL reads as a nil position. */
static VpStep vp_case_in(VpWalk *w, VpItem *cur) {
  const NodeTable *nt = w->nt;
  if (!(w->shapes & VPS_CASE_IN)) return VPSTEP_LEAF;
  int n = cur->node, d = cur->depth + 1, an = 0;
  const int *av = nt_arr(nt, n, "conditions", &an);
  int els = nt_ref(nt, n, "else_clause");
  if (els >= 0 || (w->flags & VP_CASE_IN_RAISE_NIL)) vp_push(w, els, n, an, d, 0);
  for (int i = an - 1; i >= 0; i--) vp_push(w, av[i], n, i, d, 0);
  return VPSTEP_EXPANDED;
}

/* return, next and break: with the flag on, the single value they hand out
   (a bare one hands out nil). Several values or a splat is an array the walk
   cannot point at: the jump stays a leaf of kind VPK_JUMP. */
static VpStep vp_jump(VpWalk *w, VpItem *cur, unsigned follow) {
  const NodeTable *nt = w->nt;
  if (!(w->flags & follow)) return VPSTEP_JUMP;
  int n = cur->node, an = 0;
  int args = nt_ref(nt, n, "arguments");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an > 1 || (an == 1 && nt_kind(nt, av[0]) == NK_SplatNode)) return VPSTEP_JUMP;
  vp_push(w, an == 1 ? av[0] : -1, n, 0, cur->depth, 0);
  return VPSTEP_EXPANDED;
}

/* A plain write compiles its value kept and dups it, so `x = e` is `e`'s
   value: under VP_WRITES the positions are `e`'s. An operator write
   (`x += e`) is the operator's result and is not listed here. */
static VpStep vp_write(VpWalk *w, VpItem *cur) {
  if (!(w->flags & VP_WRITES)) return VPSTEP_LEAF;
  vp_push(w, nt_ref(w->nt, cur->node, "value"), cur->node, 0, cur->depth, 0);
  return VPSTEP_EXPANDED;
}

/* `x ||= e` answers the target's old value when it is truthy and `e` when it
   is not; `x &&= e` the old value when it is falsy. Under VP_WRITES the write
   node is a leaf, standing for that old value (a caller classifies it as it
   would a read of the target), and `e`'s positions follow it. The old value
   answers only when truthy for `||=` and only when falsy for `&&=`: the leaf
   says so in its reach, `e`'s positions keep what they inherited. */
static VpStep vp_logical_write(VpWalk *w, VpItem *cur) {
  if (!(w->flags & VP_WRITES)) return VPSTEP_LEAF;
  vp_push(w, nt_ref(w->nt, cur->node, "value"), cur->node, 0, cur->depth, 0);
  const char *t = nt_type(w->nt, cur->node);
  size_t tl = strlen(t);
  cur->reach |= tl >= 12 && strcmp(t + tl - 12, "AndWriteNode") == 0 ? VPR_FALSY : VPR_TRUTHY;
  return VPSTEP_LEAF_THEN_EXPANDED;
}

/* The writes the iterator reads under VP_WRITES, by what they answer: a plain
   write is its value; an or-write or and-write is the target's old value or
   its value; any other kind is no write here. One list, for both directions. */
typedef enum { VPW_NONE, VPW_PLAIN, VPW_LOGICAL } VpWriteKind;

static VpWriteKind vp_write_kind(NodeKind k) {
  switch (k) {
  case NK_LocalVariableWriteNode:
  case NK_InstanceVariableWriteNode:
  case NK_GlobalVariableWriteNode:
  case NK_ClassVariableWriteNode:
  case NK_ConstantWriteNode:
  case NK_ConstantPathWriteNode:
  case NK_MultiWriteNode:
    return VPW_PLAIN;
  case NK_LocalVariableOrWriteNode:
  case NK_InstanceVariableOrWriteNode:
  case NK_GlobalVariableOrWriteNode:
  case NK_ClassVariableOrWriteNode:
  case NK_ConstantOrWriteNode:
  case NK_ConstantPathOrWriteNode:
  case NK_LocalVariableAndWriteNode:
  case NK_InstanceVariableAndWriteNode:
  case NK_GlobalVariableAndWriteNode:
  case NK_ClassVariableAndWriteNode:
  case NK_ConstantAndWriteNode:
  case NK_ConstantPathAndWriteNode:
  case NK_CallOrWriteNode:
  case NK_CallAndWriteNode:
  case NK_IndexOrWriteNode:
  case NK_IndexAndWriteNode:
    return VPW_LOGICAL;
  default:
    return VPW_NONE;
  }
}

/* Does a ParenthesesNode hold other than exactly one statement (`()`,
   `(a; b)`): the leaf VP_PAREN_ONE stops at. Only this node's own count is
   read, so a cycle of parentheses is left to the walk's budget. */
static int vp_paren_not_one(const NodeTable *nt, int node) {
  int body = nt_ref(nt, node, "body"), cnt = 0;
  if (body >= 0) nt_arr(nt, body, "body", &cnt);
  return cnt != 1;
}

static VpStep vp_step(VpWalk *w, VpItem *cur) {
  const NodeTable *nt = w->nt;
  if ((w->flags & VP_PARENS_ONLY) &&
      (nt_kind(nt, cur->node) == NK_ElseNode || nt_kind(nt, cur->node) == NK_InNode))
    return VPSTEP_LEAF;
  if ((w->flags & VP_PAREN_ONE) && nt_kind(nt, cur->node) == NK_ParenthesesNode &&
      vp_paren_not_one(nt, cur->node))
    return VPSTEP_LEAF;
  switch (nt_kind(nt, cur->node)) {
#include "value_pos_arms.inc"
  /* ---- hand-written arms, one per `role: hand` entry of value_edges.yml ---- */
  case NK_BeginNode:
    return vp_begin(w, cur);
  case NK_AndNode:
    return vp_and(w, cur);
  case NK_CaseMatchNode:
    return vp_case_in(w, cur);
  case NK_ReturnNode:
    return vp_jump(w, cur, VP_FOLLOW_RETURN);
  case NK_NextNode:
    return vp_jump(w, cur, VP_FOLLOW_NEXT);
  case NK_BreakNode:
    return vp_jump(w, cur, VP_FOLLOW_BREAK);
  default:
    switch (vp_write_kind(nt_kind(nt, cur->node))) {
    case VPW_PLAIN:
      return vp_write(w, cur);
    case VPW_LOGICAL:
      return vp_logical_write(w, cur);
    default:
      break;
    }
    break;
  }
  return VPSTEP_LEAF;
}

/* The operands of `a || b` and `a && b` have just been pushed, the left one
   last: it is the value of `||` only when truthy, and of `&&` (when the left
   side is a position at all) only when falsy. */
static void vp_mark_left(VpWalk *w, int node) {
  NodeKind k = nt_kind(w->nt, node);
  if (k == NK_OrNode) w->items[w->n - 1].reach |= VPR_TRUTHY;
  else if (k == NK_AndNode && (w->flags & VP_AND_LEFT)) w->items[w->n - 1].reach |= VPR_FALSY;
}

/* One item: peel until it is a leaf or has pushed its positions. */
static int vp_visit(VpWalk *w, VpItem cur) {
  w->reach = cur.reach;
  for (;;) {
    if (cur.node < 0) return vp_emit(w, &cur, VPK_NIL);
    if (--w->budget < 0) {
      w->n = 0;   /* one gave-up leaf, then the walk is over */
      return vp_emit(w, &cur, VPK_GAVE_UP);
    }
    if (w->seen) w->seen[cur.node] = 1;
    if ((w->flags & VP_LOGIC_NODES) && (w->shapes & VPS_LOGIC) &&
        (nt_kind(w->nt, cur.node) == NK_OrNode || nt_kind(w->nt, cur.node) == NK_AndNode) &&
        vp_emit(w, &cur, VPK_EXPR))
      return 1;
    switch (vp_step(w, &cur)) {
    case VPSTEP_PEEL:
      continue;
    case VPSTEP_EXPANDED:
      vp_mark_left(w, cur.node);
      return 0;
    case VPSTEP_JUMP:
      return vp_emit(w, &cur, VPK_JUMP);
    case VPSTEP_LEAF_THEN_EXPANDED:
      return vp_emit(w, &cur, VPK_EXPR);
    case VPSTEP_LEAF:
      break;
    }
    return vp_emit(w, &cur, VPK_EXPR);
  }
}

static int vp_walk(const NodeTable *nt, int root, unsigned flags, VpFn fn, void *ud, unsigned char *seen) {
  VpWalk w;
  w.nt = nt; w.flags = flags; w.fn = fn; w.ud = ud; w.seen = seen;
  w.shapes = vp_shapes(flags);
  w.budget = nt->count;
  w.reach = 0;
  w.items = w.small; w.n = 0; w.cap = VP_SMALL;
  vp_push(&w, root, -1, 0, 0, 0);
  int stop = 0;
  while (!stop && w.n > 0) {
    VpItem it = w.items[--w.n];
    stop = vp_visit(&w, it);
  }
  if (w.items != w.small) free(w.items);
  return stop;
}

int for_each_value_position(const NodeTable *nt, int root, unsigned flags, VpFn fn, void *ud) {
  return vp_walk(nt, root, flags, fn, ud, NULL);
}

/* vp_one's callback: the first leaf is the answer, and a second leaf, a jump
   or a gave-up leaf ends the walk with no single answer. */
typedef struct { int seen, many, nil, node; } Vp1State;
static int vp1_leaf(const VpLeaf *leaf, void *ud) {
  Vp1State *o = (Vp1State *)ud;
  if (o->seen || leaf->kind == VPK_JUMP || leaf->kind == VPK_GAVE_UP) { o->many = 1; return 1; }
  o->seen = 1;
  o->nil = leaf->kind == VPK_NIL;
  o->node = leaf->node;
  return 0;
}

Vp1Kind vp_one(const NodeTable *nt, int root, unsigned flags, int *node) {
  Vp1State o = { 0, 0, 0, -1 };
  *node = -1;
  if (root < 0) return VP1_MANY;
  for_each_value_position(nt, root, flags, vp1_leaf, &o);
  if (o.many || !o.seen) return VP1_MANY;
  if (o.nil) return VP1_NIL;
  *node = o.node;
  return VP1_NODE;
}

/* ---- the upward half ---- */

/* How a child hangs from its parent: not a value position of it, a wrapper's
   (parentheses, a statement list's last, an else: its value is the child's),
   or a branch's (a conditional that crossed). */
enum { VPE_NONE, VPE_WRAP, VPE_BRANCH };

/* The hand-written upward arms, one per `role: hand` entry of
   value_edges.yml that is neither a jump (the climb reads those) nor a write
   (the rule below the switch). Each reads the same fields as its downward arm
   in vp_step. */
static int vp_up_hand(const NodeTable *nt, int p, int child, unsigned flags, unsigned shapes) {
  NodeKind k = nt_kind(nt, p);
  switch (k) {
  case NK_BeginNode: {
    int body = nt_ref(nt, p, "statements"), rescue = nt_ref(nt, p, "rescue_clause");
    int els = nt_ref(nt, p, "else_clause"), ensure = nt_ref(nt, p, "ensure_clause");
    if (rescue < 0 && els < 0 && ensure < 0)
      return (shapes & VPS_BEGIN_PLAIN) && child == body ? VPE_WRAP : VPE_NONE;
    if (!(shapes & VPS_BEGIN_CLAUSES)) return VPE_NONE;
    if (child == rescue || child == els) return VPE_BRANCH;
    return child == body && (els < 0 || (flags & VP_BEGIN_BOTH)) ? VPE_BRANCH : VPE_NONE;
  }
  case NK_AndNode:
    if (!(shapes & VPS_LOGIC)) return VPE_NONE;
    if (child == nt_ref(nt, p, "right")) return VPE_BRANCH;
    return child == nt_ref(nt, p, "left") && (flags & VP_AND_LEFT) ? VPE_BRANCH : VPE_NONE;
  case NK_CaseMatchNode: {
    if (!(shapes & VPS_CASE_IN)) return VPE_NONE;
    if (child == nt_ref(nt, p, "else_clause")) return VPE_BRANCH;
    int an = 0;
    const int *av = nt_arr(nt, p, "conditions", &an);
    for (int i = 0; i < an; i++)
      if (av[i] == child) return VPE_BRANCH;
    return VPE_NONE;
  }
  default:
    break;
  }
  VpWriteKind wk = (flags & VP_WRITES) ? vp_write_kind(k) : VPW_NONE;
  if (wk != VPW_NONE && child == nt_ref(nt, p, "value")) return VPE_WRAP;
  /* a `when` clause: its statements are its value */
  if (nt_kind(nt, p) == NK_WhenNode && child == nt_ref(nt, p, "statements")) return VPE_WRAP;
  return VPE_NONE;
}

static int vp_up_edge(const NodeTable *nt, int p, int child, unsigned flags, unsigned shapes) {
  if ((flags & VP_PARENS_ONLY) &&
      (nt_kind(nt, p) == NK_ElseNode || nt_kind(nt, p) == NK_InNode)) return VPE_NONE;
  if ((flags & VP_PAREN_ONE) && nt_kind(nt, p) == NK_ParenthesesNode && vp_paren_not_one(nt, p))
    return VPE_NONE;
  switch (nt_kind(nt, p)) {
#include "value_pos_edges.inc"
  default:
    break;
  }
  return vp_up_hand(nt, p, child, flags, shapes);
}

int vp_is_value_edge(const NodeTable *nt, int parent, int child, unsigned flags) {
  return parent >= 0 && child >= 0 && vp_up_edge(nt, parent, child, flags, vp_shapes(flags)) != VPE_NONE;
}

/* The jump a node is the single value of, when the flag follows it: the
   ArgumentsNode of a return, next or break with one non-splat argument. */
static int vp_jump_of(const NodeTable *nt, const int *par, int args, int child, unsigned flags) {
  int g = par[args];
  if (g < 0) return -1;
  NodeKind gk = nt_kind(nt, g);
  unsigned need = gk == NK_ReturnNode ? VP_FOLLOW_RETURN : gk == NK_NextNode ? VP_FOLLOW_NEXT
                : gk == NK_BreakNode ? VP_FOLLOW_BREAK : 0;
  if (!(flags & need) || nt_ref(nt, g, "arguments") != args) return -1;
  int an = 0;
  const int *av = nt_arr(nt, args, "arguments", &an);
  return an == 1 && av[0] == child && nt_kind(nt, child) != NK_SplatNode ? g : -1;
}

VpUpKind vp_climb(const NodeTable *nt, const int *par, int node, unsigned flags, VpUp *up) {
  unsigned shapes = vp_shapes(flags);
  long budget = nt->count;
  int cur = node, branches = 0;
  unsigned reach = 0;
  up->kind = VPU_ROOT; up->node = node; up->parent = -1; up->branches = 0; up->reach = 0;
  for (;;) {
    int p = cur >= 0 && cur < nt->count ? par[cur] : -1;
    up->node = cur; up->parent = p; up->branches = branches; up->reach = reach;
    if (p < 0) return up->kind = VPU_ROOT;
    if (--budget < 0) return up->kind = VPU_GAVE_UP;
    if (nt_kind(nt, p) == NK_ArgumentsNode) {
      int jump = vp_jump_of(nt, par, p, cur, flags);
      if (jump >= 0) { up->parent = jump; return up->kind = VPU_JUMP; }
      return up->kind = VPU_ENDED;
    }
    int e = vp_up_edge(nt, p, cur, flags, shapes);
    if (e == VPE_NONE)
      return up->kind = nt_kind(nt, p) == NK_StatementsNode ? VPU_DROPPED : VPU_ENDED;
    if (e == VPE_BRANCH) branches++;
    /* a left operand is the value of `||` when truthy, of `&&` when falsy */
    if (cur == nt_ref(nt, p, "left")) {
      if (nt_kind(nt, p) == NK_OrNode) reach |= VPR_TRUTHY;
      else if (nt_kind(nt, p) == NK_AndNode) reach |= VPR_FALSY;
    }
    cur = p;
  }
}

int vp_up_def(const NodeTable *nt, const VpUp *up) {
  return up->kind == VPU_ENDED && nt_kind(nt, up->parent) == NK_DefNode && nt_ref(nt, up->parent, "body") == up->node
         ? up->parent : -1;
}

int vp_up_loop(const NodeTable *nt, const VpUp *up) {
  if (up->kind != VPU_ENDED) return -1;
  NodeKind k = nt_kind(nt, up->parent);
  return (k == NK_WhileNode || k == NK_UntilNode) && nt_ref(nt, up->parent, "statements") == up->node ? up->parent : -1;
}

int vp_up_block_call(const NodeTable *nt, const int *par, const VpUp *up) {
  if (up->kind != VPU_ENDED || nt_kind(nt, up->parent) != NK_BlockNode || nt_ref(nt, up->parent, "body") != up->node) return -1;
  int call = par[up->parent];
  return call >= 0 && nt_kind(nt, call) == NK_CallNode ? call : -1;
}

int vp_up_effect_clause(const NodeTable *nt, const VpUp *up) {
  if (up->kind != VPU_ENDED) return 0;
  NodeKind k = nt_kind(nt, up->parent);
  if (k == NK_BeginNode)
    return nt_ref(nt, up->parent, "else_clause") >= 0 && nt_ref(nt, up->parent, "statements") == up->node;
  return k == NK_EnsureNode || k == NK_PreExecutionNode || k == NK_PostExecutionNode;
}

/* ---- --dump-value-positions ---- */

typedef struct {
  const NodeTable *nt;
  FILE *out;
  int line;   /* the scope's line: where a nil with no parent is */
  const int *ord;   /* by node id: the program node's number, as vp_ord_table counts */
  const int *par;   /* the parent map, for the upward check */
  int root;         /* the root being walked */
  unsigned flags;   /* the flags of the walk and of the climbs checked against it */
  int print;        /* the leaves are printed (the flags of the expected file) */
  unsigned char *seen;   /* the nodes the walk visited */
} VpDump;

/* The numbers a dump prints for nodes. Ids count from the first node the
   parser made, and the builtins spliced in ahead of the program are parsed
   first, so a builtin that grows moved every id of the dump. A program node
   is numbered by how many program nodes come before it (the count
   comp_node_tag numbers it by), which the builtins do not change. */
static int *vp_ord_table(const NodeTable *nt) {
  int *ord = malloc((size_t)(nt->count ? nt->count : 1) * sizeof *ord);
  if (!ord) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int next = 0;
  for (int id = 0; id < nt->count; id++)
    ord[id] = nt_int(nt, id, "node_bi", 0) ? -1 : next++;
  return ord;
}

/* "#12" for a program node. A builtin's node, should a dump reach one, is
   "#p3": its number inside the builtin method (or top-level statement) it
   belongs to, which only an edit of that builtin moves. */
static void vp_print_id(const VpDump *d, int id) {
  if (d->ord[id] >= 0) fprintf(d->out, " #%d", d->ord[id]);
  else fprintf(d->out, " #p%d", id - ((int)nt_int(d->nt, id, "node_bi", 0) - 1));
}

/* Climbs from `node` with the dump's flags, through the jumps it follows:
   the top node reached, and the conditionals and truth values crossed. */
static int vp_dump_top(const VpDump *d, int node, int *branches, unsigned *reach, int *hops) {
  VpUp up;
  *branches = 0; *hops = 0;
  for (; *hops < d->nt->count; (*hops)++) {
    vp_climb(d->nt, d->par, node, d->flags, &up);
    *branches += up.branches;
    if (up.kind != VPU_JUMP) break;
    node = up.parent;
  }
  *reach = up.reach;
  return up.node;
}

/* The upward half must agree with the walk, in both directions.
   From a leaf, the climb with the same flags goes back to the root the
   leaf was yielded from (through the jumps it followed), crossing as many
   conditionals as the leaf's depth says and the operands its reach says. And
   from any node of the table, a climb that reaches the root starts at a node
   the walk visited: a leaf, or a node it looked through or expanded (the
   `when` clause, which the walk reads through its case, is the one node it
   does not visit). A disagreement is printed, so that value-pos-test fails
   on it. (An or-write or and-write is a leaf standing for the target's old
   value, which no operand carries: its reach is not asked.) */
static void vp_dump_report(const VpDump *d, int node, const char *what) {
  fprintf(d->out, "  UP-MISMATCH");
  vp_print_id(d, node);
  fprintf(d->out, " (flags %#x) %s\n", d->flags, what);
}

static const char *vp_kind_name(VpKind k) {
  switch (k) {
  case VPK_EXPR: return "EXPR";
  case VPK_NIL: return "NIL";
  case VPK_JUMP: return "JUMP";
  case VPK_GAVE_UP: return "GAVE_UP";
  }
  return "?";
}

static int vp_dump_leaf(const VpLeaf *lf, void *ud) {
  VpDump *d = ud;
  const NodeTable *nt = d->nt;
  if (d->print) {
    fprintf(d->out, "  %s", vp_kind_name(lf->kind));
    if (lf->node >= 0) { vp_print_id(d, lf->node); fprintf(d->out, " %s", nt_type(nt, lf->node)); }
    int line = lf->node >= 0 ? (int)nt_int(nt, lf->node, "node_line", 0)
             : lf->parent >= 0 ? (int)nt_int(nt, lf->parent, "node_line", 0) : d->line;
    fprintf(d->out, " line %d arm %d", line, lf->arm);
    if (lf->parent >= 0) {
      fprintf(d->out, " parent");
      vp_print_id(d, lf->parent);
      fprintf(d->out, " %s", nt_type(nt, lf->parent));
    }
    else fprintf(d->out, " parent -");
    fprintf(d->out, " depth %d", lf->cond_depth);
    if (lf->reach) fprintf(d->out, " reach %s", lf->reach == VPR_TRUTHY ? "truthy" : lf->reach == VPR_FALSY ? "falsy" : "never");
    fputc('\n', d->out);
  }
  if (lf->node < 0 || lf->kind == VPK_GAVE_UP) return 0;
  int branches, hops;
  unsigned reach;
  int top = vp_dump_top(d, lf->node, &branches, &reach, &hops);
  if (top != d->root) vp_dump_report(d, lf->node, "does not climb back to its root");
  else if (branches != lf->cond_depth) vp_dump_report(d, lf->node, "crosses another number of conditionals than its depth");
  else if (hops == 0 && vp_write_kind(nt_kind(nt, lf->node)) != VPW_LOGICAL && reach != lf->reach)
    vp_dump_report(d, lf->node, "crosses other operands than its reach");
  return 0;
}

static void vp_dump_converse(const VpDump *d) {
  const NodeTable *nt = d->nt;
  for (int n = 0; n < nt->count; n++) {
    int branches, hops;
    unsigned reach;
    if (d->seen[n] || nt_kind(nt, n) == NK_WhenNode) continue;
    if (vp_dump_top(d, n, &branches, &reach, &hops) == d->root) vp_dump_report(d, n, "climbs to the root, but the walk did not visit it");
  }
}

static void vp_dump_scope(const NodeTable *nt, const int *par, FILE *out, const int *ord, const char *title, int line, int root) {
  VpDump d;
  d.nt = nt; d.out = out; d.line = line; d.ord = ord; d.par = par; d.root = root;
  /* the flag sets the check runs the climbs and the walk with: every shape
     alone, none, and everything but one of the other flags */
  const unsigned flag_sets[] = {
    VPS_IF, VPS_UNLESS, VPS_CASE, VPS_CASE_IN, VPS_BEGIN_PLAIN, VPS_BEGIN_CLAUSES, VPS_RESCUE_MOD, VPS_LOGIC, VPS_NONE,
    VP_ALL_FLAGS & ~VP_AND_LEFT, VP_ALL_FLAGS & ~VP_BEGIN_BOTH, VP_ALL_FLAGS & ~VP_WRITES,
    VP_ALL_FLAGS & ~VP_FOLLOW_RETURN, VP_ALL_FLAGS & ~VP_FOLLOW_NEXT, VP_ALL_FLAGS & ~VP_FOLLOW_BREAK,
    VP_ALL_FLAGS & ~(VP_FOLLOW_RETURN | VP_FOLLOW_NEXT | VP_FOLLOW_BREAK)
  };
  d.seen = calloc((size_t)(nt->count > 0 ? nt->count : 1), 1);
  if (!d.seen) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (title) fprintf(out, "%s line %d\n", title, line);
  else fprintf(out, "main\n");
  for (size_t i = 0; i < sizeof flag_sets / sizeof flag_sets[0] + 1; i++) {
    d.print = i == 0;
    d.flags = i == 0 ? VP_ALL_FLAGS : flag_sets[i - 1];
    memset(d.seen, 0, (size_t)(nt->count > 0 ? nt->count : 1));
    vp_walk(nt, root, d.flags, vp_dump_leaf, &d, d.seen);
    vp_dump_converse(&d);
  }
  free(d.seen);
}

/* The program, then every method, block and lambda body of the program's own
   file in node order (the builtins parsed ahead of it are another file). A
   block is a root of its own: the walk never goes into one. */
void value_pos_dump(const NodeTable *nt, FILE *out) {
  int *par = an_parent_map(nt);
  if (!par) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int *ord = vp_ord_table(nt);
  vp_dump_scope(nt, par, out, ord, NULL, 1, nt_ref(nt, nt->root_id, "statements"));
  for (int id = 0; id < nt->count; id++) {
    NodeKind k = nt_kind(nt, id);
    if ((k != NK_DefNode && k != NK_BlockNode && k != NK_LambdaNode) || nt_int(nt, id, "node_file", 0) != 0) continue;
    const char *name = nt_str(nt, id, "name");
    char title[256];
    if (k == NK_DefNode) snprintf(title, sizeof title, "def %s", name ? name : "?");
    else snprintf(title, sizeof title, "%s", k == NK_BlockNode ? "block" : "lambda");
    vp_dump_scope(nt, par, out, ord, title, (int)nt_int(nt, id, "node_line", 0), nt_ref(nt, id, "body"));
  }
  free(ord);
  free(par);
}

/* ---- --check-value-pos ---- */

/* What a walk over a table built by hand saw: leaves of each kind, whether
   the EXPR leaves came in the order and with the arms the check expects. */
typedef struct {
  int leaves[4];   /* by VpKind */
  int next_arm;    /* the arm the next EXPR leaf should carry */
  int out_of_order;
  int last_node;
} VpCheck;

static void vp_check_init(VpCheck *k) {
  memset(k, 0, sizeof *k);
  k->last_node = -1;
}

static int vp_check_leaf(const VpLeaf *lf, void *ud) {
  VpCheck *k = ud;
  k->leaves[lf->kind]++;
  if (lf->kind == VPK_EXPR) {
    if (lf->arm != k->next_arm || lf->node <= k->last_node) k->out_of_order = 1;
    k->next_arm = lf->arm + 1;
    k->last_node = lf->node;
  }
  return 0;
}

/* A cycle of parentheses and statements, which only a shared subtree could
   make in a real table: the walk ends with one gave-up leaf. */
static int vp_check_budget(void) {
  NodeTable *nt = nt_new_table();
  int p = nt_new_node(nt, "ParenthesesNode"), s = nt_new_node(nt, "StatementsNode");
  int body[1];
  body[0] = p;
  nt_node_set_ref(nt, p, "body", s);
  nt_node_set_arr(nt, s, "body", body, 1);
  VpCheck k;
  vp_check_init(&k);
  for_each_value_position(nt, p, VP_ALL_FLAGS, vp_check_leaf, &k);
  int ok = k.leaves[VPK_GAVE_UP] == 1 && k.leaves[VPK_EXPR] + k.leaves[VPK_NIL] + k.leaves[VPK_JUMP] == 0;
  nt_free(nt);
  return ok;
}

/* vp_one over parentheses built by hand: `()` is nil, `(a; b)` and `((b))`
   are b, an `if` is one node when only the wrappers are looked through (and
   two leaves when its shape is walked), a cycle and no expression are no
   single answer. */
static int vp_check_one(void) {
  NodeTable *nt = nt_new_table();
  int a = nt_new_node(nt, "IntegerNode"), b = nt_new_node(nt, "IntegerNode");
  int ps = nt_new_node(nt, "ParenthesesNode"), ss = nt_new_node(nt, "StatementsNode");
  int two[2] = { a, b };
  nt_node_set_ref(nt, ps, "body", ss);
  nt_node_set_arr(nt, ss, "body", two, 2);
  int empty = nt_new_node(nt, "ParenthesesNode");
  int inner = nt_new_node(nt, "ParenthesesNode"), is = nt_new_node(nt, "StatementsNode");
  int one[1] = { b };
  nt_node_set_ref(nt, inner, "body", is);
  nt_node_set_arr(nt, is, "body", one, 1);
  int outer = nt_new_node(nt, "ParenthesesNode"), os = nt_new_node(nt, "StatementsNode");
  int wrapped[1] = { inner };
  nt_node_set_ref(nt, outer, "body", os);
  nt_node_set_arr(nt, os, "body", wrapped, 1);
  int iff = nt_new_node(nt, "IfNode");
  nt_node_set_ref(nt, iff, "statements", a);
  nt_node_set_ref(nt, iff, "subsequent", b);
  int n = -2, ok = 1;
  ok &= vp_one(nt, ps, VPS_NONE, &n) == VP1_NODE && n == b;
  ok &= vp_one(nt, empty, VPS_NONE, &n) == VP1_NIL && n == -1;
  ok &= vp_one(nt, outer, VPS_NONE, &n) == VP1_NODE && n == b;
  ok &= vp_one(nt, a, VPS_NONE, &n) == VP1_NODE && n == a;
  ok &= vp_one(nt, iff, VPS_NONE, &n) == VP1_NODE && n == iff;
  ok &= vp_one(nt, iff, VPS_ALL, &n) == VP1_MANY && n == -1;
  ok &= vp_one(nt, -1, VPS_NONE, &n) == VP1_MANY && n == -1;
  int ret = nt_new_node(nt, "ReturnNode");   /* a jump that is not followed is no value */
  ok &= vp_one(nt, ret, VPS_NONE, &n) == VP1_MANY && n == -1;
  int cp = nt_new_node(nt, "ParenthesesNode"), cs = nt_new_node(nt, "StatementsNode");
  int cyc[1] = { cp };
  nt_node_set_ref(nt, cp, "body", cs);
  nt_node_set_arr(nt, cs, "body", cyc, 1);
  ok &= vp_one(nt, cp, VPS_NONE, &n) == VP1_MANY && n == -1;
  ok &= vp_one(nt, cp, VPS_NONE | VP_PAREN_ONE, &n) == VP1_MANY && n == -1;
  int clause = nt_new_node(nt, "ElseNode");
  nt_node_set_ref(nt, clause, "statements", ss);
  ok &= vp_one(nt, ps, VPS_NONE | VP_PARENS_ONLY, &n) == VP1_NODE && n == b;
  ok &= vp_one(nt, empty, VPS_NONE | VP_PARENS_ONLY, &n) == VP1_NIL;
  ok &= vp_one(nt, clause, VPS_NONE | VP_PARENS_ONLY, &n) == VP1_NODE && n == clause;
  ok &= vp_one(nt, ps, VPS_NONE | VP_PAREN_ONE, &n) == VP1_NODE && n == ps;
  ok &= vp_one(nt, empty, VPS_NONE | VP_PAREN_ONE, &n) == VP1_NODE && n == empty;
  ok &= vp_one(nt, outer, VPS_NONE | VP_PAREN_ONE, &n) == VP1_NODE && n == b;
  ok &= vp_one(nt, clause, VPS_NONE, &n) == VP1_NODE && n == b;
  ok &= !vp_is_value_edge(nt, clause, ss, VPS_NONE | VP_PARENS_ONLY);
  ok &= vp_is_value_edge(nt, clause, ss, VPS_NONE);
  nt_free(nt);
  return ok;
}

/* A case with 100 when clauses and no else (the stack outgrows its 32 entries
   on the C stack) and an if/elsif chain of 5000 arms (no recursion to cut):
   every arm is seen once, in source order, with its place in the chain. */
static int vp_check_long(void) {
  NodeTable *nt = nt_new_table();
  enum { WHENS = 100, ARMS = 5000 };
  int whens[WHENS];
  for (int i = 0; i < WHENS; i++) {
    whens[i] = nt_new_node(nt, "WhenNode");
    nt_node_set_ref(nt, whens[i], "statements", nt_new_node(nt, "IntegerNode"));
  }
  int cs = nt_new_node(nt, "CaseNode");
  nt_node_set_arr(nt, cs, "conditions", whens, WHENS);
  VpCheck kc;
  vp_check_init(&kc);
  for_each_value_position(nt, cs, VP_ALL_FLAGS, vp_check_leaf, &kc);
  int ok = kc.leaves[VPK_EXPR] == WHENS && kc.leaves[VPK_NIL] == 1 && !kc.out_of_order && kc.next_arm == WHENS;
  int root = nt_new_node(nt, "IfNode"), cur = root;
  for (int i = 0; i < ARMS; i++) {
    int next = nt_new_node(nt, "IfNode");
    nt_node_set_ref(nt, cur, "statements", nt_new_node(nt, "IntegerNode"));
    nt_node_set_ref(nt, cur, "subsequent", next);
    cur = next;
  }
  nt_node_set_ref(nt, cur, "statements", nt_new_node(nt, "IntegerNode"));
  int tail = nt_new_node(nt, "ElseNode");
  nt_node_set_ref(nt, tail, "statements", nt_new_node(nt, "IntegerNode"));
  nt_node_set_ref(nt, cur, "subsequent", tail);
  VpCheck ki;
  vp_check_init(&ki);
  for_each_value_position(nt, root, VP_ALL_FLAGS, vp_check_leaf, &ki);
  ok = ok && ki.leaves[VPK_EXPR] == ARMS + 2 && ki.leaves[VPK_GAVE_UP] == 0 && !ki.out_of_order;
  /* and the climb back from the last arm crosses them all */
  int *par = an_parent_map(nt);
  if (!par) { nt_free(nt); return 0; }
  int deepest = nt_ref(nt, tail, "statements");
  VpUp up;
  vp_climb(nt, par, deepest, VP_ALL_FLAGS, &up);
  ok = ok && up.kind == VPU_ROOT && up.node == root && up.branches == ARMS + 1;
  free(par);
  nt_free(nt);
  return ok;
}

/* A cycle of parentheses and statements ends a climb in VPU_GAVE_UP, as it
   ends a walk. */
static int vp_check_climb_budget(void) {
  NodeTable *nt = nt_new_table();
  int p = nt_new_node(nt, "ParenthesesNode"), s = nt_new_node(nt, "StatementsNode");
  int body[1];
  body[0] = p;
  nt_node_set_ref(nt, p, "body", s);
  nt_node_set_arr(nt, s, "body", body, 1);
  int *par = an_parent_map(nt);
  if (!par) { nt_free(nt); return 0; }
  VpUp up;
  int ok = vp_climb(nt, par, p, VP_ALL_FLAGS, &up) == VPU_GAVE_UP;
  ok &= vp_climb(nt, par, p, VP_ALL_FLAGS | VP_PAREN_ONE, &up) == VPU_GAVE_UP;
  free(par);
  nt_free(nt);
  return ok;
}

/* VP_LOGIC_NODES: `(a || b) && c` yields the && as a leaf, then the || as a
   leaf, then a, b and c, with the truth values each one answers for. */
typedef struct {
  int n;
  int node[8];
  unsigned reach[8];
} VpLogicCheck;

static int vp_logic_leaf(const VpLeaf *lf, void *ud) {
  VpLogicCheck *k = ud;
  if (k->n < 8) { k->node[k->n] = lf->node; k->reach[k->n] = lf->reach; }
  k->n++;
  return 0;
}

static int vp_check_logic(void) {
  NodeTable *nt = nt_new_table();
  int a = nt_new_node(nt, "IntegerNode"), b = nt_new_node(nt, "IntegerNode"), c = nt_new_node(nt, "IntegerNode");
  int orn = nt_new_node(nt, "OrNode"), andn = nt_new_node(nt, "AndNode");
  nt_node_set_ref(nt, orn, "left", a);
  nt_node_set_ref(nt, orn, "right", b);
  nt_node_set_ref(nt, andn, "left", orn);
  nt_node_set_ref(nt, andn, "right", c);
  VpLogicCheck k;
  memset(&k, 0, sizeof k);
  for_each_value_position(nt, andn, VP_AND_LEFT | VP_LOGIC_NODES | VPS_ALL, vp_logic_leaf, &k);
  int ok = k.n == 5 && k.node[0] == andn && k.node[1] == orn && k.node[2] == a && k.node[3] == b && k.node[4] == c &&
           k.reach[0] == 0 && k.reach[1] == VPR_FALSY && k.reach[2] == (VPR_FALSY | VPR_TRUTHY) &&
           k.reach[3] == VPR_FALSY && k.reach[4] == 0;
  memset(&k, 0, sizeof k);
  for_each_value_position(nt, andn, VPS_ALL, vp_logic_leaf, &k);   /* without VP_AND_LEFT: the right side of the && only */
  ok = ok && k.n == 1 && k.node[0] == c;
  nt_free(nt);
  return ok;
}

int value_pos_check(void) {
  int ok = 1;
  if (!vp_check_budget()) { fprintf(stderr, "value-pos-check: a cycle did not end in one gave-up leaf\n"); ok = 0; }
  if (!vp_check_climb_budget()) { fprintf(stderr, "value-pos-check: a cycle did not end a climb in one gave-up answer\n"); ok = 0; }
  if (!vp_check_logic()) { fprintf(stderr, "value-pos-check: the logical operators did not yield their node and their truth values\n"); ok = 0; }
  if (!vp_check_long()) { fprintf(stderr, "value-pos-check: a wide case or a long elsif chain was not walked in order, or not climbed back\n"); ok = 0; }
  if (!vp_check_one()) { fprintf(stderr, "value-pos-check: vp_one did not name the one node of a sequence, nil for () and no answer for several\n"); ok = 0; }
  if (ok) printf("value-pos-check: ok\n");
  return ok ? 0 : 1;
}
