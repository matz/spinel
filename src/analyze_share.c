/* analyze_share.c -- the questions the String rule asks of the program
   (#6765): whether a value is a new String no other name holds
   (share_value_fresh), which literal blocks a method can run
   (share_method_blocks), whether a node hands on a String's handle
   (share_node_carry_kind), and the plain questions of names, targets and
   literals beside them. See share.h.

   The tables they keep (ShareFacts) are read from the calls when the
   rule is applied (share_facts_build): the blocks each method can be
   given, the methods a method hands its own block on to, the names a
   send or a Method may reach, and the memos the questions fill. */

#include <stdlib.h>
#include <string.h>
#include "analyze_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "share.h"
#include "repr.h"
#include "codegen_internal.h"
#include "value_pos.h"

typedef struct ShareFacts {
  int nnodes;
  /* the literal blocks each method can be given (sh_mb_add): pairs, then
     once built, per method scope its run (mb_start..mb_start+1) of mb_blk;
     blk_dyn per method scope: a block the table does not list reaches its
     yields (a block passed as a value, a zsuper's, a dynamic call's), bit 2
     set for the last two */
  int *mb_m, *mb_b, nmb, cmb;
  int *mb_start, *mb_blk;
  unsigned char *blk_dyn;
  /* the blocks a method can run as share_value_fresh asks: a method's
     `&blk` handed on (`m2(&blk)`, an anonymous `&`, a zsuper) as an edge
     from the method to each it reaches (fw_from, fw_to), and blk_unk per
     method scope: a block that is neither a literal nor such a forward
     reaches it; fresh_blk per method scope, once asked: every block it can
     run answers a new String (1), not (0), not asked (-1) */
  int *fw_from, *fw_to, nfw, cfw;
  unsigned char *blk_unk;
  signed char *fresh_blk;
  /* per node, once asked: a `call` on a callable whose every value is a
     new String or no String (1), not (0), not asked (-1); NULL until then */
  signed char *fresh_call;
  /* the method names a Method, `send` or `&:name` can reach; dyn_all when
     one is computed */
  const char **dyn;
  int ndyn, cdyn, dyn_all;
  int ostruct;         /* the program names OpenStruct, whose fields a poly
                          receiver's call may read */
  struct ShNamed { const char *name; int k, yielder; } *blkp;
  /* the block parameters by name: a literal block's and a lambda's, with
     the method that yields to them or Enumerator.new's yielder; built on
     first use (sh_blk_bound) */
  int nblkp, cblkp, blkp_built;
  /* the writes of an ivar by name, for sh_ivar_string: an attr writer's as
     "x=", a reader as "x", and instance_variable_set(:@x, v) as "@x"; k the
     node. ivs: the answers given, by name */
  struct ShNamed *ivw, *ivs;
  int nivw, civw, ivw_built, ivw_all, nivs, civs;
} ShareFacts;

/* Can a value of type t hold a String, or a container of them? An object's
   Strings sit in its ivars, which are holders of their own. */
int sh_may_hold(const Compiler *c, TyKind t) {
  /* an object holds its Strings in ivars, which are holders of their own;
     a native class's object keeps the String its binding declares, and so
     does a subclass's, which is built by the class's constructor */
  if (ty_is_object(t)) {
    int k = ty_object_class(t);
    if (k < c->nclasses && c->classes[k].nsub_root > 0) k = c->classes[k].nsub_root - 1;
    return k < c->nclasses && c->classes[k].native_share_keeps;
  }
  if (ty_is_obj_array(t)) return 0;
  switch (t) {
  case TY_VOID: case TY_NIL: case TY_INT: case TY_BIGINT: case TY_FLOAT: case TY_SYMBOL:
  case TY_BOOL: case TY_RANGE: case TY_FLOAT_RANGE: case TY_TIME: case TY_COMPLEX:
  case TY_RATIONAL: case TY_MATCHDATA: case TY_REGEX: case TY_EXCEPTION:
  case TY_INT_ARRAY: case TY_FLOAT_ARRAY: case TY_INT_ARRAY_ARRAY: case TY_FLOAT_ARRAY_ARRAY:
  case TY_STR_INT_HASH: case TY_INT_INT_HASH: case TY_PROC: case TY_CURRY: case TY_FIBER:
  case TY_THREAD: case TY_QUEUE: case TY_MUTEX: case TY_CONDVAR: case TY_RANDOM: case TY_DIR:
  case TY_ADDRINFO: case TY_SOCKOPT: case TY_TMS: case TY_PROCESS_STATUS:
  case TY_METHOD: case TY_IO: case TY_ARGF: case TY_CLASS:
    return 0;
  default:
    return 1;
  }
}

/* the class whose ivar slot an ivar node names, as the emitters store it */
static int sh_ivar_owner(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  if (s && s->class_id >= 0) return s->class_id;
  return c->toplevel_class;
}

static int sh_method_index(Compiler *c, int node) {
  Scope *s = comp_scope_of(c, node);
  return s && s->def_node >= 0 ? (int)(s - c->scopes) : -1;
}

static void sh_dyn_name(ShareFacts *F, const char *name) {
  if (!name) { F->dyn_all = 1; return; }
  if (F->ndyn >= F->cdyn) {
    F->cdyn = F->cdyn ? F->cdyn * 2 : 16;
    F->dyn = realloc(F->dyn, sizeof(char *) * (size_t)F->cdyn);
  }
  F->dyn[F->ndyn++] = name;
}

static const char *sh_lit_name(const NodeTable *nt, int n);
/* the literal name a `send`, `method` or `instance_variable_*` names */
static const char *sh_lit_name(const NodeTable *nt, int a) {
  if (a < 0) return NULL;
  NodeKind k = nt_kind(nt, a);
  if (k == NK_SymbolNode) return nt_str(nt, a, "value") ? nt_str(nt, a, "value") : nt_str(nt, a, "unescaped");
  if (k == NK_StringNode) return nt_str(nt, a, "content");
  return NULL;
}

static int sh_plain_operand(Compiler *c, int n);
/* How lending a parameter argument node a of `call` keeps CRuby's answer:
   SHL_HOLDS by itself, SHL_READ only while the method's value is taken as
   read (F->mread), SHL_UNSOUND not at all. A value no variable holds is
   lent a temporary nothing else names. A variable's C slot is lent by
   address, so it must be a local (or parameter) nothing can rebind while
   the callee runs: no proc that captures it assigns it, the call passes no
   block (the method's yield runs the block, which may assign it), and every
   argument is a read, a literal or scalar arithmetic over those (*plain,
   asked once per call). A global's or an ivar's slot is refused where the
   call can rebind it at the top level or in a class body (#6179), which
   only the shared class avoids. A block of the call that can assign the
   argument variable, a local or an instance variable
   (comp_block_rebinds_arg: `m(s) { s = +"b" }`, `m(@s) { @s = +"b" }`),
   rebinds the lent slot under the parameter while the method runs: its
   appends then land on the new String. No lend holds there; the parameter
   shares its argument's class instead. */
enum { SHL_UNSOUND = -1, SHL_READ = 0, SHL_HOLDS = 1 };

/* Method from hands the block it was given on to method to (share_value_fresh) */
static void sh_fwd_edge(ShareFacts *F, int from, int to) {
  if (from < 0 || to < 0) return;
  if (F->nfw >= F->cfw) {
    F->cfw = F->cfw ? F->cfw * 2 : 16;
    F->fw_from = realloc(F->fw_from, sizeof(int) * (size_t)F->cfw);
    F->fw_to = realloc(F->fw_to, sizeof(int) * (size_t)F->cfw);
    if (!F->fw_from || !F->fw_to) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->fw_from[F->nfw] = from;
  F->fw_to[F->nfw++] = to;
}
/* Call n hands method mi a block as a value (blk, a BlockArgumentNode): the
   enclosing method's own &blk (or an anonymous `&`) is a forward edge; any
   other block (`&proc`, `&:sym`, `&method(:m)`) is one the facts do not
   list (blk_unk). */
static void sh_block_value_to(ShareFacts *F, Compiler *c, int n, int blk, int mi) {
  const NodeTable *nt = c->nt;
  int x = nt_ref(nt, blk, "expression");
  int cur = sh_method_index(c, n);
  const char *bp = cur >= 0 ? c->scopes[cur].blk_param : NULL;
  if (cur >= 0 && bp &&
      (x < 0 ? !bp[0] : nt_kind(nt, x) == NK_LocalVariableReadNode && nt_int(nt, x, "depth", 0) == 0 &&
                        nt_str(nt, x, "name") && bp[0] && sp_streq(nt_str(nt, x, "name"), bp)))
    sh_fwd_edge(F, cur, mi);
  else F->blk_unk[mi] = 1;
}

static int sh_named_first(const struct ShNamed *a, int n, const char *name);
static int sh_copied_self_targets(Compiler *c, int call, int *out, int cap);
/* ... with the call's fresh plan `p` in hand: the facts' target walk asks
   this of every target of a call, and resolved the plan again for each
   (cplan_user_fresh keeps nothing), which on a Rails program was 7% of a
   sampled window of the share pass. */
static int share_target_armed_plan(Compiler *c, const CallPlan *p, int call, int mi) {
  if (p->via != UC_POLY || p->send_fallback == mi || mi < 0 || mi >= c->nscopes || c->scopes[mi].is_cmethod) return 1;
  int recv = nt_ref(c->nt, call, "receiver");
  /* (a receiver of no type the walk follows the user methods of: with
     them gone its builtin arm is an unknown call, which joins UNKNOWN) */
  if (recv < 0 || c->ntype[recv] != TY_POLY) return 1;
  const ClsSet *cs = cls_fact_node(c, recv);
  if (!cs || cs->all) return 1;
  const char *nm = nt_str(c->nt, call, "name");
  if (!nm) return 1;
  int np = 0;
  const PolyCand *pc = comp_poly_candidates_at(c, call, nm, &np);
  for (int i = 0; i < np; i++)
    if (pc[i].mi == mi || (pc[i].mi < 0 && comp_method_in_chain(c, pc[i].cls, nm, NULL) == mi)) return 1;
  return 0;
}
int share_target_armed(Compiler *c, int call, int mi) {
  return share_target_armed_plan(c, cplan_user_fresh(c, call), call, mi);
}
static int sh_targets_in(ShareFacts *F, Compiler *c, int call, int *out, int cap) {
  (void)F;
  int n = cplan_targets(c, call, out, cap), m = 0;
  if (n == CPT_UNKNOWN) return -1;
  if (n == 0) return sh_copied_self_targets(c, call, out, cap);
  /* the plan once for the call: cplan_user_fresh answers in a buffer the
     next resolution overwrites, so it is copied */
  CallPlan p = *cplan_user_fresh(c, call);
  for (int i = 0; i < n; i++)
    if (share_target_reachable(c, call, out[i]) && share_target_armed_plan(c, &p, call, out[i])) out[m++] = out[i];
  return m;
}
static int sh_targets(Compiler *c, int call, int *out, int cap) { return sh_targets_in(NULL, c, call, out, cap); }
/* A module's method an include copied into its includers runs as those
   copies (is_transplanted_source), where self is an includer, so a
   receiverless call in it reaches what each includer's chain answers to
   the name. Asked in the module's own method, the plan finds nothing for a
   name only the includers define (openssl's Buffering#do_write calls the
   includer's syswrite), and the walk took it as a call it does not follow,
   with the write buffer it was handed. The methods the copies' classes
   reach; 0 when one reaches none (a builtin, or NoMethodError), -1 past
   cap. */
static int sh_copied_self_targets(Compiler *c, int call, int *out, int cap) {
  const NodeTable *nt = c->nt;
  const char *name = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  Scope *s = comp_scope_of(c, call);
  int si = (int)(s - c->scopes);
  if (!name || nt_ref(nt, call, "receiver") >= 0 || nt_int(nt, call, "builtin_only", 0) || !s->is_transplanted_source ||
      s->is_cmethod || s->def_node < 0 || scope_toplevel_included(c, si))
    return 0;
  /* (a name no class answers, a builtin's, reaches none) */
  int n = 0;
  comp_poly_candidates(c, name, &n);
  if (n == 0) return 0;
  n = 0;
  /* (the body's copies, by def_node; every scope was read for them) */
  for (int k = an_scopes_same_def_first(c, si); k >= 0; k = an_scopes_same_def_next(c, k)) {
    const Scope *cp = &c->scopes[k];
    if (k == si || cp->class_id < 0 || cp->is_cmethod) continue;
    int mi = comp_method_in_chain(c, cp->class_id, name, NULL);
    if (mi < 0) return 0;
    int seen = 0;
    for (int j = 0; j < n && !seen; j++) seen = out[j] == mi;
    if (seen) continue;
    if (n == cap) return -1;
    out[n++] = mi;
  }
  return n;
}
int share_target_reachable(Compiler *c, int call, int mi) {
  if (mi < 0 || mi >= c->nscopes || !c->scopes[mi].is_cmethod || cplan_user_fresh(c, call)->via != UC_POLY)
    return 1;
  int ncc = 0;
  const PolyCand *ccs = comp_cmethod_candidates(c, c->scopes[mi].name, &ncc);
  /* and only a class this receiver can hold, as the dispatch arms it
     (poly_cls_value_reaches): `options.fetch(:protocol, "http").sub(...)`
     on a Hash of Strings never reaches `Params.sub`, though Params is a
     value elsewhere (campfire's ActiveStorage::Current.base_url) */
  for (int i = 0; i < ncc; i++)
    if (ccs[i].mi == mi && comp_class_may_be_value(c, ccs[i].cls) && poly_cls_value_reaches(c, call, ccs[i].cls))
      return 1;
  return 0;
}
/* does a call reach a user method (sh_targets != 0), without listing them */
static int sh_has_targets(Compiler *c, int call) {
  const CallPlan *p = cplan_user_fresh(c, call);
  if (p->dispatch == CP_REFUSE) return 0;
  /* (a module's self call that names no method of the module reaches the
     includers' methods its copies call, sh_copied_self_targets: a `puts`
     they define is no Kernel#puts) */
  if (p->mi < 0) {
    int tg[CPT_MAX];
    return sh_copied_self_targets(c, call, tg, CPT_MAX) != 0;
  }
  /* (a boxed receiver's plan lists only the methods its arms reach:
     share_target_reachable, share_target_armed; none may be left) */
  if (p->via != UC_POLY) return 1;
  int tg[CPT_MAX];
  return sh_targets(c, call, tg, CPT_MAX) != 0;
}

/* the receiver family a builtin's share row is keyed by */
static TyKind sh_family(TyKind rt) {
  if (rt == TY_STRING || rt == TY_STRBUF) return TY_STRING;
  if (ty_is_array(rt) || ty_is_ptr_array(rt) || rt == TY_STR_RANGE || rt == TY_ENUMERATOR) return BOP_ANY_ARRAY;
  if (ty_is_hash(rt)) return BOP_ANY_HASH;
  if (rt == TY_ARGF) return TY_IO;
  if (rt == TY_PROC || rt == TY_METHOD || rt == TY_CURRY) return BOP_CALLABLE;
  return rt;
}

static int sh_named_cmp(const void *a, const void *b) {
  int d = strcmp(((const struct ShNamed *)a)->name, ((const struct ShNamed *)b)->name);
  return d ? d : ((const struct ShNamed *)a)->k - ((const struct ShNamed *)b)->k;
}
/* the first entry of a sorted index whose name is `name` (n when none) */
static int sh_named_first(const struct ShNamed *a, int n, const char *name) {
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(a[mid].name, name) < 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}

static int sh_answers_elems_run(int share, int argc);

static int sh_native_placeholder(Compiler *c, const PolyCand *p, const char *name);

/* The class that `K.new(...)` (through parentheses) builds, whatever else
   the program has for the class to be: the instance of exactly K, when K is
   a class of the program's and no `def self.new` stands between. -1 for any
   other node. */
int share_exact_new_class(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  n = nt_paren_single(nt, n);
  if (n < 0 || nt_kind(nt, n) != NK_CallNode || !sp_streq(nt_str(nt, n, "name"), "new")) return -1;
  int r = nt_ref(nt, n, "receiver");
  if (r < 0 || (nt_kind(nt, r) != NK_ConstantReadNode && nt_kind(nt, r) != NK_ConstantPathNode)) return -1;
  int k = const_node_class_index(c, r);
  return k >= 0 && comp_cmethod_in_chain(c, k, "new", NULL) < 0 ? k : -1;
}
/* Kernel#String(x) answers x for a String, and for any other value what its
   #to_str answers, or with none its #to_s. A method of the program answers
   a String it holds as it is, so `String(obj) << "?"` changes the obj's own
   String, however new the call looks (sh_builtin's BSH_ARGS row sees only
   the argument). The #to_str and #to_s that a value of argument arg's type
   can run, as method indexes (-1 for an attr_reader of the name): an
   object's class and its subclasses' (the class alone for a `K.new`, which
   builds exactly K), and a boxed value's classes, each less the classes the
   class facts rule out (cls_fact_excludes). 0 for an argument of any other type, whose
   conversion is the builtin's; more than cap when they do not fit. */
static int sh_string_conv_methods(Compiler *c, int arg, int *mis, int cap) {
  TyKind at = c->ntype[arg];
  int obj = ty_is_object(at), ocid = obj ? ty_object_class(at) : -1, n = 0;
  int exact = obj && share_exact_new_class(c, arg) == ocid;
  if ((!obj && at != TY_POLY && at != TY_UNKNOWN)) return 0;
  for (int j = 0; j < 2; j++) {
    int np = 0;
    const PolyCand *pc = comp_poly_candidates(c, j ? "to_s" : "to_str", &np);
    for (int i = 0; i < np; i++) {
      int k = pc[i].cls;
      if ((obj && k != ocid && (exact || !is_descendant(c, k, ocid))) || cls_fact_excludes(c, arg, k)) continue;
      /* (Kernel#String asks #to_str first: a class with one never runs its #to_s) */
      if (j && comp_method_in_chain(c, k, "to_str", NULL) >= 0) continue;
      if (pc[i].native) continue;
      if (n < cap) mis[n] = pc[i].mi;
      n++;
    }
  }
  return n;
}
/* Is call n a Kernel#String of one plain argument, which the program does
   not define itself? Its argument, or -1. */
static int sh_string_conv_arg(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, n, "name");
  if (!nm || !is_string_class_name(nm) || nt_ref(nt, n, "receiver") >= 0 ||
      nt_ref(nt, n, "block") >= 0) return -1;
  int argc = 0;
  const int *argv = call_args(nt, n, &argc);
  if (argc != 1 || nt_kind(nt, argv[0]) == NK_SplatNode || nt_kind(nt, argv[0]) == NK_KeywordHashNode ||
      nt_kind(nt, argv[0]) == NK_BlockArgumentNode || sh_has_targets(c, n)) return -1;
  return argv[0];
}

/* The names parameter node p binds (a block's ParametersNode, or one of
   its parameters, destructured or not), into F->blkp under scope si. */
static void sh_blkp_add(ShareFacts *F, const NodeTable *nt, int p, int si, int yielder) {
  if (p < 0) return;
  const char *pn = nt_str(nt, p, "name");
  if (pn) {
    if (F->nblkp >= F->cblkp) {
      F->cblkp = F->cblkp ? F->cblkp * 2 : 64;
      F->blkp = realloc(F->blkp, sizeof *F->blkp * (size_t)F->cblkp);
      if (!F->blkp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    F->blkp[F->nblkp++] = (struct ShNamed){ pn, si, yielder };
  }
  static const char *const lists[] = { "requireds", "optionals", "posts", "keywords", "lefts", "rights" };
  for (int l = 0; l < 6; l++) {
    int n = 0; const int *ps = nt_arr(nt, p, lists[l], &n);
    for (int i = 0; i < n; i++) sh_blkp_add(F, nt, ps[i], si, 0);
  }
  static const char *const refs[] = { "rest", "keyword_rest", "block" };
  for (int r = 0; r < 3; r++) sh_blkp_add(F, nt, nt_ref(nt, p, refs[r]), si, 0);
}
/* The parameters and block-locals of block (or lambda) b, under scope si;
   with `yielder`, its first required parameter is Enumerator.new's
   yielder. */
static void sh_blkp_block(ShareFacts *F, const NodeTable *nt, int b, int si, int yielder) {
  int bp = nt_ref(nt, b, "parameters");
  if (bp < 0) return;
  int pn = nt_ref(nt, bp, "parameters");
  int nreq = 0; const int *reqs = pn >= 0 ? nt_arr(nt, pn, "requireds", &nreq) : NULL;
  if (yielder && nreq >= 1 && nt_kind(nt, reqs[0]) != NK_MultiTargetNode) {
    sh_blkp_add(F, nt, reqs[0], si, 1);
    /* the rest as one would add them, the yielder left out */
    for (int i = 1; i < nreq; i++) sh_blkp_add(F, nt, reqs[i], si, 0);
    static const char *const lists[] = { "optionals", "posts", "keywords" };
    for (int l = 0; l < 3; l++) {
      int n = 0; const int *ps = nt_arr(nt, pn, lists[l], &n);
      for (int i = 0; i < n; i++) sh_blkp_add(F, nt, ps[i], si, 0);
    }
    static const char *const refs[] = { "rest", "keyword_rest", "block" };
    for (int r = 0; r < 3; r++) sh_blkp_add(F, nt, nt_ref(nt, pn, refs[r]), si, 0);
  }
  else sh_blkp_add(F, nt, pn, si, 0);
  int ln = 0; const int *ls = nt_arr(nt, bp, "locals", &ln);
  for (int i = 0; i < ln; i++) sh_blkp_add(F, nt, ls[i], si, 0);
}

/* Is call u a builtin class's constructor whose block's first parameter
   is the yielder (its BSH_NEW_YIELDER row: `Enumerator.new { |y| }`)? */
static int sh_yielder_new(Compiler *c, int u) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, u, "receiver");
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? nt_str(nt, r, "name") : NULL;
  const char *nm = nt_str(nt, u, "name");
  return cn && nm && bop_share_named(BOP_ANY_RECV, nm) == BSH_NEW && comp_class_index(c, cn) < 0 &&
         bop_share_named(BOP_CLASS_NEW, cn) == BSH_NEW_YIELDER;
}

/* PolyLits.bound for an_recv_may_be_string: how a literal block or a
   lambda of scope `scope` binds variable `name`, whether or not its binder
   marked it a block's parameter (a Thread's block does not): 0 none binds
   it, 1 only as Enumerator.new's yielder (sh_builtin_new: no String), 2
   another way. The index is built once per build of the facts. */
static int sh_blk_bound(void *ctx, Compiler *c, int scope, const char *name) {
  ShareFacts *F = ctx;
  const NodeTable *nt = c->nt;
  if (!F->blkp_built) {
    F->blkp_built = 1;
    for (int si = 0; si < c->nscopes; si++)
      for (int u = comp_bcall_first(c, si); u >= 0; u = comp_bcall_next(c, u))
        sh_blkp_block(F, nt, nt_ref(nt, u, "block"), si, sh_yielder_new(c, u));
    NT_FOREACH_KIND(nt, NK_LambdaNode, l) {
      Scope *ls = comp_scope_of(c, l);
      if (ls) sh_blkp_block(F, nt, l, (int)(ls - c->scopes), 0);
    }
    if (F->nblkp) qsort(F->blkp, (size_t)F->nblkp, sizeof *F->blkp, sh_named_cmp);
  }
  int got = 0;
  for (int i = sh_named_first(F->blkp, F->nblkp, name); i < F->nblkp && sp_streq(F->blkp[i].name, name); i++)
    if (F->blkp[i].k == scope) got = F->blkp[i].yielder && got != 2 ? 1 : 2;
  return got;
}

static void sh_ivw_add(ShareFacts *F, const char *key, int node) {
  if (F->nivw >= F->civw) {
    F->civw = F->civw ? F->civw * 2 : 64;
    F->ivw = realloc(F->ivw, sizeof *F->ivw * (size_t)F->civw);
    if (!F->ivw) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->ivw[F->nivw++] = (struct ShNamed){ key, node, 0 };
}
/* ShareFacts.ivw, in one pass over the calls */
static void sh_ivar_writes_build(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  F->ivw_built = 1;
  NT_FOREACH_KIND(nt, NK_CallNode, u) {
    const char *nm = nt_str(nt, u, "name");
    if (!nm) continue;
    if (name_is_plain_setter(nm)) { sh_ivw_add(F, nm, u); continue; }
    int args = nt_ref(nt, u, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    const char *lit = argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL;
    int r = nt_ref(nt, u, "receiver");
    if (is_ivar_set(nm)) {
      if (lit) sh_ivw_add(F, lit, u);
      else F->ivw_all = 1;
    }
    else if (is_send_family(nm) || is_method_object_name(nm)) {
      if (!lit || is_send_family(lit) || is_ivar_set(lit) || is_method_object_name(lit)) F->ivw_all = 1;
      else if (name_is_plain_setter(lit)) sh_ivw_add(F, lit, u);
    }
    else if (is_alias_method_name(nm)) {
      const char *old = argc >= 2 ? sh_lit_name(nt, argv[1]) : NULL;
      if (!old || name_is_plain_setter(old)) F->ivw_all = 1;
    }
    else if (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && is_marshal_const(nt_str(nt, r, "name")))
      F->ivw_all = 1;
  }
  NT_FOREACH_KIND(nt, NK_AliasMethodNode, u) {
    const char *old = sh_lit_name(nt, nt_ref(nt, u, "old_name"));
    if (!old || name_is_plain_setter(old)) F->ivw_all = 1;
  }
  /* an attribute's `||=` and `&&=` carry the writer's name, or only the
     reader's (`x`, asked for as such) */
  static const NodeKind ow[] = { NK_CallOrWriteNode, NK_CallAndWriteNode };
  for (size_t i = 0; i < sizeof ow / sizeof *ow; i++)
    NT_FOREACH_KIND(nt, ow[i], u) {
      const char *wn = nt_str(nt, u, "write_name");
      if (!wn) wn = nt_str(nt, u, "name");
      if (wn) sh_ivw_add(F, wn, u);
    }
  NT_FOREACH_KIND(nt, NK_CallTargetNode, u)
    if (nt_str(nt, u, "name")) sh_ivw_add(F, nt_str(nt, u, "name"), u);
  /* an attribute's `+=` (and the other operators) the desugar keeps as is,
     by the reader's name */
  NT_FOREACH_KIND(nt, NK_CallOperatorWriteNode, u)
    if (nt_str(nt, u, "name"))
      sh_ivw_add(F, nt_str(nt, u, "name"), u);
  if (F->nivw) qsort(F->ivw, (size_t)F->nivw, sizeof *F->ivw, sh_named_cmp);
}
/* The value a write ShareFacts.ivw lists stores, or -1 when it cannot be
   read off the call (a splat, a Method, a multiple write's target, an
   operator write like `o.x += v`, which may store anything) */
static int sh_ivw_value(Compiler *c, int u) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, u);
  if (k == NK_CallOrWriteNode || k == NK_CallAndWriteNode) return nt_ref(nt, u, "value");
  if (k != NK_CallNode || nt_ref(nt, u, "block") >= 0) return -1;
  const char *nm = nt_str(nt, u, "name");
  int args = nt_ref(nt, u, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int want = name_is_plain_setter(nm) ? 1 : is_ivar_set(nm) || is_send_family(nm) ? 2 : 0;
  if (!want || argc != want) return -1;
  NodeKind vk = nt_kind(nt, argv[want - 1]);
  return vk == NK_SplatNode || vk == NK_KeywordHashNode || vk == NK_BlockArgumentNode ? -1 : argv[want - 1];
}
/* PolyLits.ivar for an_recv_may_be_string: may instance variable `ivn`
   (`@x`) hold a String? Asked of every write of an ivar of that name in
   any class -- the write nodes, and the calls ShareFacts.ivw lists --
   each value by its type or form (an_value_may_be_string). A Struct's or
   a Data's member of the name is set by its constructor, not a write, so
   it may. Answers are kept per name for the build (ivn is the asker's
   buffer, so the name is copied). */
static int sh_ivar_string(void *ctx, Compiler *c, const char *ivn) {
  ShareFacts *F = ctx;
  const NodeTable *nt = c->nt;
  for (int i = 0; i < F->nivs; i++) if (sp_streq(F->ivs[i].name, ivn)) return F->ivs[i].k;
  if (!F->ivw_built) sh_ivar_writes_build(F, c);
  const PolyLits lits = { sh_blk_bound, F, NULL };
  int may = F->ivw_all;
  for (int k = 0; k < c->nclasses && !may; k++) {
    const ClassInfo *ci = &c->classes[k];
    if (!ci->is_struct && !ci->is_data) continue;
    for (int m = 0; m < ci->nmembers && m < ci->nivars && !may; m++) may = sp_streq(ci->ivars[m], ivn);
  }
  static const NodeKind wk[] = { NK_InstanceVariableWriteNode, NK_InstanceVariableOrWriteNode,
                                 NK_InstanceVariableAndWriteNode, NK_InstanceVariableOperatorWriteNode,
                                 NK_InstanceVariableTargetNode };
  for (size_t i = 0; i < sizeof wk / sizeof *wk && !may; i++)
    NT_FOREACH_KIND(nt, wk[i], w) {
      if (may || !sp_streq(nt_str(nt, w, "name"), ivn)) continue;
      /* `@x += v` and a multiple write's target take a value not read here */
      int v = wk[i] == NK_InstanceVariableOperatorWriteNode || wk[i] == NK_InstanceVariableTargetNode
              ? -1 : nt_ref(nt, w, "value");
      may = v < 0 || an_value_may_be_string(c, v, &lits);
    }
  char wn[300];
  /* (a name too long for the buffer may be anything) */
  if (snprintf(wn, sizeof wn, "%s=", ivn[0] == '@' ? ivn + 1 : ivn) >= (int)sizeof wn) may = 1;
  const char *keys[3] = { wn, ivn, ivn[0] == '@' ? ivn + 1 : ivn };
  for (int j = 0; j < 3 && !may; j++)
    for (int i = sh_named_first(F->ivw, F->nivw, keys[j]);
         !may && i < F->nivw && sp_streq(F->ivw[i].name, keys[j]); i++) {
      int v = sh_ivw_value(c, F->ivw[i].k);
      may = v < 0 || an_value_may_be_string(c, v, &lits);
    }
  if (F->nivs >= F->civs) {
    F->civs = F->civs ? F->civs * 2 : 16;
    F->ivs = realloc(F->ivs, sizeof *F->ivs * (size_t)F->civs);
    if (!F->ivs) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  char *kept = strdup(ivn);
  if (!kept) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  F->ivs[F->nivs++] = (struct ShNamed){ kept, may, 0 };
  return may;
}

/* Does method scope mi answer no String: each expression its value can be
   (a nil among them) is none a String can be, by its type or form or by what
   is written to an instance variable it reads (an_recv_may_be_string)? A
   method whose values the walk does not follow answers 0. */
int share_scope_answers_no_string(Compiler *c, int mi) {
  ShareFacts *F = c->share;
  if (!F || mi <= 0 || mi >= c->nscopes) return 0;
  int out[16];
  int n = method_value_leaves_or_nil(c, mi, out, 16);
  if (n < 0) return 0;
  const PolyLits lits = { sh_blk_bound, F, sh_ivar_string };
  for (int i = 0; i < n; i++)
    if (an_recv_may_be_string(c, out[i], &lits)) return 0;
  return 1;
}

static int sh_builtin_fresh(Compiler *c, int call, int ostruct);
/* Under --share-strings, is call n an attribute assignment (`obj.x = v`,
   call_is_setter_assign) of one plain value? Its value is the right-hand
   side whatever the writer returns, and the object the writer was handed,
   so no name-free String. */
static int sh_assign_call(const Compiler *c, int n) {
  if (nt_kind(c->nt, n) != NK_CallNode || nt_ref(c->nt, n, "block") >= 0 ||
      !call_is_setter_assign(c->nt, n)) return 0;
  int args = nt_ref(c->nt, n, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  return argc == 1 && nt_kind(c->nt, argv[0]) != NK_SplatNode && nt_kind(c->nt, argv[0]) != NK_KeywordHashNode &&
         nt_kind(c->nt, argv[0]) != NK_BlockArgumentNode;
}

static int sh_uplus_copies(Compiler *c, int n);

/* `a, b = x, y`: a multiple write of an Array literal with no splat into
   targets with no rest; each target takes its own value. */
int share_call_stores_block(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, n, "block");
  const char *nm = nt_str(nt, n, "name");
  if (!nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode || sh_has_targets(c, n)) return 0;
  int recv = nt_ref(nt, n, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = nt_str(nt, recv, "name");
    return cn && sp_streq(nm, "new") && (sp_streq(cn, "Array") || sp_streq(cn, "Hash"));
  }
  static const TyKind fams[] = { BOP_ANY_ARRAY, BOP_ANY_HASH, BOP_ANY_RECV };
  int keep = 0, any = 0;
  for (size_t i = 0; i < sizeof fams / sizeof *fams; i++) {
    int s = bop_share_named(fams[i], nm);
    if (s == BSH_ITER_MAP_BANG || s == BSH_FILL) keep = any = 1;
    else if (s == BSH_ITER_MAP || s == BSH_MERGE) any = 1;
  }
  return any && (keep || !an_node_dropped(c, n));
}
int share_literal_unkept(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  const int *par = comp_parent_map(c);
  int p = lit >= 0 ? par[lit] : -1;
  if (p >= 0 && nt_kind(nt, p) == NK_MultiWriteNode && nt_ref(nt, p, "value") == lit) return share_masgn_plain(nt, p);
  int n = p >= 0 ? par[p] : -1;
  if (n < 0 || nt_kind(nt, n) != NK_CallNode || nt_ref(nt, n, "arguments") != p) return 0;
  const char *nm = nt_str(nt, n, "name");
  if (!nm || nt_ref(nt, n, "receiver") >= 0 || nt_ref(nt, n, "block") >= 0) return 0;
  int share = bop_share_named(BOP_KERNEL, nm);
  if (share != BSH_PURE && !(share == BSH_ARGS && an_node_dropped(c, n))) return 0;
  return (is_text_print(nm) || is_inspect_print(nm)) && !sh_has_targets(c, n);
}
int share_masgn_plain(const NodeTable *nt, int n) {
  int value = nt_ref(nt, n, "value");
  if (value < 0 || nt_kind(nt, value) != NK_ArrayNode || nt_ref(nt, n, "rest") >= 0) return 0;
  int en = 0; const int *el = nt_arr(nt, value, "elements", &en);
  for (int i = 0; i < en; i++) if (nt_kind(nt, el[i]) == NK_SplatNode) return 0;
  int nr = 0; nt_arr(nt, n, "rights", &nr);
  return nr == 0;
}

static void sh_free(ShareFacts *F) {
  if (!F) return;
  free(F->mb_m); free(F->mb_b); free(F->mb_start); free(F->mb_blk); free(F->blk_dyn);
  free(F->fw_from); free(F->fw_to); free(F->blk_unk); free(F->fresh_blk); free(F->fresh_call);
  free(F->dyn); free(F->blkp); free(F->ivw);
  for (int i = 0; i < F->nivs; i++) free((char *)F->ivs[i].name);
  free(F->ivs);
  free(F);
}

/* The literal blocks by method (mb_start/mb_blk), a counting sort of the
   pairs sh_mb_add recorded. */
static void sh_index_blocks(ShareFacts *F, Compiler *c) {
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  F->mb_start = calloc(ns + 1, sizeof(int));
  F->mb_blk = malloc(sizeof(int) * (size_t)(F->nmb > 0 ? F->nmb : 1));
  if (!F->mb_start || !F->mb_blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < F->nmb; i++) F->mb_start[F->mb_m[i] + 1]++;
  for (int m = 0; m < c->nscopes; m++) F->mb_start[m + 1] += F->mb_start[m];
  int *at = malloc(sizeof(int) * ns);
  if (!at) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int m = 0; m < c->nscopes; m++) at[m] = F->mb_start[m];
  for (int i = 0; i < F->nmb; i++) F->mb_blk[at[F->mb_m[i]]++] = F->mb_b[i];
  free(at);
}

/* The nodes of the methods the program never names. Such a method runs
   nothing, so its body's reads and writes are no facts about the Strings the
   program holds, and the walk leaves it out: `def run1(f, x) = q { f.call(x) }`
   beside `def run(f) = q { f.call }` joined q's value (one holder per method,
   not per call site) with what any proc may answer, and
   `w = run(proc { +"new" }); w << "!"` was refused (#9258, cluster 9).
   Scope.reachable is the census an earlier phase took, before the rewrites the
   fixpoint adds calls through, so the walk takes its own census of the table
   as it stands (compute_reachable), keyed by the table's version, and puts the
   scopes' flags back as they were. A method the census reaches by name alone
   (a send, a Symbol, an alias) counts as named. */
static void sh_mark_subtree(const NodeTable *nt, int id, unsigned char *dead) {
  if (id < 0 || id >= nt->count || dead[id]) return;
  dead[id] = 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) sh_mark_subtree(nt, nt_ref_at(nt, id, i), dead);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) sh_mark_subtree(nt, ids[j], dead);
  }
}
static const NodeTable *g_dead_nt;
static unsigned g_dead_version;
static int g_dead_count, g_dead_nscopes;
static unsigned char *g_dead;
static unsigned char *sh_dead_nodes(Compiler *c) {
  const NodeTable *nt = c->nt;
  size_t n = (size_t)(nt->count > 0 ? nt->count : 1);
  if (!g_dead || g_dead_nt != nt || g_dead_version != nt->version || g_dead_count != nt->count ||
      g_dead_nscopes != c->nscopes) {
    unsigned char *was = malloc((size_t)(c->nscopes > 0 ? c->nscopes : 1));
    free(g_dead);
    g_dead = calloc(n, 1);
    if (!was || !g_dead) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    /* a node two scopes own (a clone of a method the dispatch splices keeps
       its source's def) is dead only when no scope that owns it is reached */
    unsigned char *live = calloc(n, 1);
    if (!live) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int s = 0; s < c->nscopes; s++) was[s] = (unsigned char)c->scopes[s].reachable;
    compute_reachable(c, 1);
    for (int s = 0; s < c->nscopes; s++) {
      Scope *m = &c->scopes[s];
      if ((m->reachable || was[s]) && m->def_node >= 0) { sh_mark_subtree(nt, m->def_node, live); sh_mark_subtree(nt, m->body, live); }
    }
    for (int s = 0; s < c->nscopes; s++) {
      Scope *m = &c->scopes[s];
      /* (dead only where the earlier census agrees: a scope a rewrite made
         is reached by a mark of its own, not by a name, as the proc form of
         a reopening's method is) */
      if (!m->reachable && !was[s] && m->def_node >= 0) { sh_mark_subtree(nt, m->def_node, g_dead); sh_mark_subtree(nt, m->body, g_dead); }
      { m->reachable = was[s]; comp_touch_scope(m->idx); }
    }
    for (size_t i = 0; i < n; i++) if (live[i]) g_dead[i] = 0;
    free(live); free(was);
    g_dead_nt = nt; g_dead_version = nt->version; g_dead_count = nt->count; g_dead_nscopes = c->nscopes;
  }
  unsigned char *d = malloc(n);
  if (!d) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(d, g_dead, n);
  return d;
}

/* ---- the block tables, read from the calls ----

   The literal blocks each method can be given (mb), the methods a method
   hands its own block on to (fw), the methods a block the table does not
   list reaches (blk_unk), and those whose yields go where no literal shows
   (blk_dyn: 1 a block value, 2 a zsuper's or a dynamic call's). Read by
   share_method_blocks and sh_blocks_fresh. */
static void sh_mb_add(ShareFacts *F, Compiler *c, int blk, int mi) {
  const Scope *m = &c->scopes[mi];
  if (!m->yields && !m->is_lowered_yield && !m->blk_param) return;
  if (F->nmb >= F->cmb) {
    F->cmb = F->cmb ? F->cmb * 2 : 64;
    F->mb_m = realloc(F->mb_m, sizeof(int) * (size_t)F->cmb);
    F->mb_b = realloc(F->mb_b, sizeof(int) * (size_t)F->cmb);
    if (!F->mb_m || !F->mb_b) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  F->mb_m[F->nmb] = mi;
  F->mb_b[F->nmb++] = blk;
}
/* call n hands method mi its block blk (none, a literal, or a value) */
static void sh_mb_block(ShareFacts *F, Compiler *c, int n, int blk, int mi) {
  if (blk < 0 || mi < 0) return;
  if (nt_kind(c->nt, blk) == NK_BlockNode) { sh_mb_add(F, c, blk, mi); return; }
  sh_block_value_to(F, c, n, blk, mi);
  if (c->scopes[mi].yields) F->blk_dyn[mi] |= 1;
}
/* A name a send, a Method or `&:name` reaches a method by; NULL for one
   computed, which may be any. */
static void sh_mb_dyn(ShareFacts *F, Compiler *c, int n, const unsigned char *dead) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, n, "name");
  int args = nt_ref(nt, n, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int blk = nt_ref(nt, n, "block");
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
    int bx = nt_ref(nt, blk, "expression");
    if (bx >= 0 && nt_kind(nt, bx) == NK_SymbolNode) sh_dyn_name(F, nt_str(nt, bx, "value"));
  }
  if (!name || dead[n]) return;
  const char *lit = argc >= 1 ? sh_lit_name(nt, argv[0]) : NULL;
  if (is_send_family(name)) {
    if (lit || !nt_int(nt, n, "dyn_send_complete", 0)) sh_dyn_name(F, lit);
    return;
  }
  if (bop_share_named(BOP_ANY_RECV, name) == BSH_METHOD_REF || bop_share_named(TY_CLASS, name) == BSH_METHOD_REF) {
    if (lit) sh_dyn_name(F, lit);
    else if (!(is_define_method_name(name) && argc >= 1)) F->dyn_all = 1;
  }
}
static void sh_mb_build(ShareFacts *F, Compiler *c) {
  const NodeTable *nt = c->nt;
  const unsigned char *dead = sh_dead_nodes(c);
  int any_new = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (dead[n]) continue;
    sh_mb_dyn(F, c, n, dead);
    int blk = nt_ref(nt, n, "block");
    if (blk < 0) continue;
    int tg[64], ntg = sh_targets(c, n, tg, 64);
    if (ntg < 0) continue;
    int nbind = 0;
    for (int i = 0; i < ntg; i++) nbind += cplan_target_binds(c, n, tg[i]);
    if (ntg > 0 && nbind > 0) {
      for (int i = 0; i < ntg; i++) sh_mb_block(F, c, n, blk, tg[i]);
      continue;
    }
    const char *name = nt_str(nt, n, "name");
    if (!name || bop_share_named(BOP_ANY_RECV, name) != BSH_NEW || nt_kind(nt, blk) != NK_BlockNode) continue;
    int recv = nt_ref(nt, n, "receiver");
    if (recv < 0) {
      /* a bare `new` in a class method: every initialize it may run */
      int own[CPT_MAX], nown = cplan_initializers(c, n, own, CPT_MAX);
      for (int i = 0; i < nown; i++) sh_mb_add(F, c, blk, own[i]);
      continue;
    }
    int mi = cplan_initialize(c, n);
    if (mi >= 0) { sh_mb_add(F, c, blk, mi); continue; }
    TyKind rt = c->ntype[recv];
    /* a class held in a variable: any initialize */
    if (rt == TY_CLASS || rt == TY_POLY || rt == TY_UNKNOWN) {
      for (int m = 0; m < c->nscopes; m++)
        if (c->scopes[m].def_node >= 0 && c->scopes[m].name && sp_streq(c->scopes[m].name, "initialize") &&
            !c->scopes[m].is_cmethod)
          sh_mb_add(F, c, blk, m);
      any_new = 1;
    }
  }
  (void)any_new;
  static const NodeKind sup[] = { NK_SuperNode, NK_ForwardingSuperNode };
  for (int k = 0; k < 2; k++)
    NT_FOREACH_KIND(nt, sup[k], n) {
      if (dead[n]) continue;
      int blk = nt_ref(nt, n, "block");
      int tg[64], ntg = sh_targets(c, n, tg, 64), cur = sh_method_index(c, n);
      for (int i = 0; i < ntg; i++) {
        if (sup[k] == NK_ForwardingSuperNode && cur >= 0) {
          F->blk_dyn[tg[i]] |= 3;
          sh_fwd_edge(F, cur, tg[i]);
        }
        if (blk >= 0 && nt_kind(nt, blk) == NK_BlockNode) sh_mb_add(F, c, blk, tg[i]);
        else if (blk >= 0) sh_block_value_to(F, c, n, blk, tg[i]);
        else if (sup[k] == NK_SuperNode) sh_fwd_edge(F, cur, tg[i]);
      }
    }
  /* what a Method, a `send` or define_method reaches is given a block the
     table does not see; so is a method_missing */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (m->def_node < 0 || !m->name) continue;
    int reach = F->dyn_all || sp_streq(m->name, "method_missing");
    for (int k = 0; k < F->ndyn && !reach; k++) reach = sp_streq(F->dyn[k], m->name);
    if (!reach) continue;
    F->blk_unk[mi] = 1;
    if (m->yields) F->blk_dyn[mi] |= 3;
  }
}
/* The tables the questions below read, without the walk. */
static ShareFacts *sh_memo_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* (the round's types are settled: the call targets held from earlier
     rounds are asked again) */
  cplan_targets_drop();
  ShareFacts *F = calloc(1, sizeof *F);
  size_t ns = (size_t)(c->nscopes > 0 ? c->nscopes : 1);
  if (!F) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  F->nnodes = nt->count;
  F->blk_dyn = calloc(ns, 1);
  F->blk_unk = calloc(ns, 1);
  F->fresh_blk = malloc(ns);
  if (!F->blk_dyn || !F->blk_unk || !F->fresh_blk) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(F->fresh_blk, -1, ns);
  NT_FOREACH_KIND(nt, NK_ConstantReadNode, cr)
    if (!F->ostruct && nt_str(nt, cr, "name") && sp_streq(nt_str(nt, cr, "name"), "OpenStruct")) F->ostruct = 1;
  sh_mb_build(F, c);
  sh_index_blocks(F, c);
  return F;
}

void share_facts_build(Compiler *c) {
  share_facts_free(c);
  c->share = sh_memo_build(c);
  comp_touch_global();   /* the facts inference reads under --share-strings are new */
}

void share_facts_free(Compiler *c) {
  sh_free(c->share);
  c->share = NULL;
}

int share_method_blocks(const Compiler *c, int mi, const int **blocks) {
  const ShareFacts *F = c->share;
  *blocks = NULL;
  if (!F || !F->mb_start || mi < 0 || mi >= c->nscopes || F->blk_dyn[mi]) return -1;
  *blocks = F->mb_blk + F->mb_start[mi];
  return F->mb_start[mi + 1] - F->mb_start[mi];
}
int share_method_reaching_blocks(const Compiler *c, int mi, int *out, int cap) {
  const ShareFacts *F = c->share;
  if (!F || !F->mb_start || mi < 0 || mi >= c->nscopes) return -1;
  int qcap = 64, qn = 0, n = 0, q[64];
  q[qn++] = mi;
  for (int h = 0; h < qn; h++) {
    int m = q[h];
    if (F->blk_dyn[m] & 2) return -1;
    for (int i = F->mb_start[m]; i < F->mb_start[m + 1]; i++) {
      if (n >= cap) return -1;
      out[n++] = F->mb_blk[i];
    }
    for (int e = 0; e < F->nfw; e++) {
      if (F->fw_to[e] != m) continue;
      int seen = 0;
      for (int k = 0; k < qn && !seen; k++) seen = q[k] == F->fw_from[e];
      if (seen) continue;
      if (qn >= qcap) return -1;
      q[qn++] = F->fw_from[e];
    }
  }
  return n;
}
/* Native classes can appear in comp_poly_candidates as placeholders even
   when they have no binding. Skip only those empty native rows; preserve
   every Ruby candidate and every real native binding conservatively. The
   non-arity registry query keeps real bindings regardless of signature. */
static int sh_native_placeholder(Compiler *c, const PolyCand *p, const char *name) {
  if (!p->native || p->mi >= 0 || p->rdcls >= 0) return 0;
  if (comp_reader_in_chain(c, p->cls, name, NULL)) return 0;
  return !comp_poly_arm_defines(c, p->cls, name);
}
/* A boxed call's builtin arms answer a value of their own when the
   any-receiver row says so. String's receiver conversions are the
   exception to Object's row: to_s can hand its String back unchanged. */
static int sh_builtin_fresh(Compiler *c, int call, int ostruct) {
  const char *name = nt_str(c->nt, call, "name");
  if (is_receiver_conversion(name)) return 0;
  int share = bop_share_named(BOP_ANY_RECV, name);
  if (share) return share == BSH_PURE;
  /* A name no builtin owns has only the user targets' answers; every
     other receiver raises. The arity tables already record that ownership.
     The dispatch plan must also exclude readers, native bindings and
     catch-all arms such as OpenStruct's member read. Object's public
     method table covers the names the arity tables omit. (A method_missing
     hook answers no name: Spinel never routes a call to it, and warns
     where one is defined.) */
  if (object_public_method_name(name)) return 0;
  /* With no builtin face or dynamic fields, only the user methods can
     answer. Their freshness is checked by the caller. */
  if (!ostruct && !bop_name_has_reader(name, BOP_READ_NUMERIC | BOP_READ_CONTAINER | BOP_READ_STRING) &&
      !ty_poly_face_owners(name, call_plain_argc(c, call), nt_ref(c->nt, call, "block") >= 0, 1, 1)) {
    int n = 0;
    const PolyCand *p = comp_poly_candidates(c, name, &n);
    /* The walk has no settled dispatch plan yet. Its candidate index
       still exposes aliases the same-named target set can miss. */
    int tg[CPT_MAX], ntg = cplan_targets(c, call, tg, CPT_MAX);
    int user_methods = 0;
    for (int i = 0; i < n; i++) {
      if (p[i].native) {
        if (!sh_native_placeholder(c, &p[i], name)) return 0;
        continue;
      }
      if (p[i].mi < 0 || p[i].rdcls >= 0) return 0;
      user_methods++;
      int found = 0;
      for (int j = 0; j < ntg; j++) if (tg[j] == p[i].mi) { found = 1; break; }
      if (!found) return 0;
    }
    if (user_methods > 0) return 1;
  }
  return 0;
}
/* Is poly arm a of call an IO's read with no buffer to fill (BSH_FILL1's
   row with one argument at most): a new String? The generic default arm
   and its tail read the box as an IO for such a name. */
static int sh_arm_io_read_fresh(Compiler *c, int call, const PolyArm *a) {
  if (call_plain_argc(c, call) >= 2 || bop_share_boxed(TY_IO, nt_str(c->nt, call, "name")) != BSH_FILL1) return 0;
  if (a->kind == PA_TRIAL) return a->key == PA_KEY_TRIAL + PT_GENERIC_TAIL;
  if (a->kind != PA_BUILTIN) return 0;
  int fam = a->key - PA_KEY_BUILTIN;
  return fam == PB_N_IO_READ || fam == PB_IO_SEEK_READ || fam == PB_IO_READ_NB || fam == PB_IO_READPARTIAL ||
         fam == PB_ND_GENERIC;
}
int share_builtin_fresh(Compiler *c, int call) {
  const char *name = nt_str(c->nt, call, "name");
  if (bop_share_named(BOP_ANY_RECV, name) == BSH_PURE && !is_receiver_conversion(name)) return 1;
  /* The share walk's builtin surfaces exclude typed-only methods such
     as Thread#value; the settled plan below still checks every arm. */
  if (!sh_builtin_fresh(c, call, !c->share || c->share->ostruct)) return 0;
  /* A scope-name lookup can miss an alias's method. Every returning user
     arm must occur in the target set whose return identities we check. */
  int tg[CPT_MAX], n = cplan_targets(c, call, tg, CPT_MAX);
  const PolyPlan *p = cplan_poly_fresh(c, call);
  for (int i = 0; i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->kind != PA_USER && a->kind != PA_PROC_FORM) continue;
    int found = 0;
    for (int j = 0; j < n; j++) if (tg[j] == a->mi) { found = 1; break; }
    if (!found) return 0;
  }
  /* The generic default of a dispatch with arguments (PB_ND_GENERIC) is
     only its tail (PT_GENERIC_TAIL) unless a name's own arm joins it,
     which the plan lists as an arm of its own. The tail re-enters the call
     as a builtin, or takes a block arm or an element store, all of names
     some builtin class has; for a name none has (CRuby's tables, as
     strbuf_poly_user_boxes asks) both only raise NoMethodError. */
  int generic_raises = !builtin_object_method_known(name) && !builtin_instance_name_known(name);
  for (int i = 0; i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->kind == PA_USER || a->kind == PA_ARITY || a->kind == PA_VIS || sh_arm_io_read_fresh(c, call, a)) continue;
    if (a->kind == PA_TRIAL && (a->key == PA_KEY_TRIAL + PT_DEFAULT0 ||
                               a->key == PA_KEY_TRIAL + PT_DEFAULT_N)) continue;
    if (generic_raises && ((a->kind == PA_BUILTIN && a->key == PA_KEY_BUILTIN + PB_ND_GENERIC) ||
                           (a->kind == PA_TRIAL && a->key == PA_KEY_TRIAL + PT_GENERIC_TAIL)))
      continue;
    return 0;
  }
  return p->n > 0;
}
/* ---- calls whose members the plan does not list (#9258) ----
   A call on a boxed receiver whose name more classes define than the plan
   lists (CPT_MAX), or on a receiver of no settled type, has members the
   plan cannot name. They are still among the program's methods of its
   name, or a builtin's, where nothing defines or answers that name past
   what the walk sees (share_name_targets): the freshness of the call's
   answer is theirs (sh_name_call_fresh, an_tail_name_fresh). */
/* names a method the walk does not see may answer: one defined at run
   time, a singleton def on a value, an alias, a module_function copy;
   dyn_all: method_missing, a computed definer, a String evaluated */
static const char **g_dyn_names;
static int g_ndyn_names, g_cdyn_names, g_dyn_all;
/* the old names of the program's aliases (a method that may also run under
   another name), and whether it runs a method object on another receiver
   or past an override (bind, bind_call, super_method) */
static const char **g_alias_src;
static int g_nalias_src, g_calias_src, g_rebind;
static void sh_alias_src_add(const char *nm) {
  if (g_nalias_src == g_calias_src) {
    g_calias_src = g_calias_src ? g_calias_src * 2 : 16;
    g_alias_src = realloc(g_alias_src, sizeof(*g_alias_src) * (size_t)g_calias_src);
    if (!g_alias_src) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  g_alias_src[g_nalias_src++] = nm;
}
static const Compiler *g_dyn_c;
static int g_dyn_count = -1;

static void sh_dyn_name_add(const char *nm) {
  if (!nm) return;
  for (int i = 0; i < g_ndyn_names; i++) if (sp_streq(g_dyn_names[i], nm)) return;
  if (g_ndyn_names >= g_cdyn_names) {
    g_cdyn_names = g_cdyn_names ? g_cdyn_names * 2 : 32;
    g_dyn_names = realloc(g_dyn_names, sizeof(char *) * (size_t)g_cdyn_names);
    if (!g_dyn_names) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  g_dyn_names[g_ndyn_names++] = nm;
}
static const char *sh_lit_method_name(const NodeTable *nt, int a) {
  NodeKind k = nt_kind(nt, a);
  if (k == NK_SymbolNode) return nt_str(nt, a, "value");
  if (k == NK_StringNode) return nt_str(nt, a, "content");
  return NULL;
}
/* A call of a definer (define_method, alias_method, module_function, an
   eval): the name it defines, or every name where it is computed */
static void sh_dyn_definer_call(const NodeTable *nt, int id) {
  static const char *const definers[] = { "define_method", "define_singleton_method", "alias_method",
                                          "module_function", "instance_eval", "class_eval", "module_eval",
                                          "class_exec", "module_exec", "instance_exec", "eval", NULL };
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return;
  int args = nt_ref(nt, id, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  /* send(:define_method, ...) is the definer itself; a computed send
     calls a method that exists */
  if ((sp_streq(nm, "send") || sp_streq(nm, "__send__") || sp_streq(nm, "public_send")) && an > 0) {
    nm = sh_lit_method_name(nt, av[0]);
    if (!nm) return;
    av++; an--;
  }
  int k = 0;
  while (definers[k] && !sp_streq(definers[k], nm)) k++;
  if (!definers[k]) return;
  /* module_function's names (a bare one: the defs after it, below) */
  if (sp_streq(nm, "module_function")) {
    for (int a = 0; a < an; a++) {
      const char *ln = sh_lit_method_name(nt, av[a]);
      if (ln) sh_dyn_name_add(ln);
      else g_dyn_all = 1;
    }
    return;
  }
  /* a body evaluated from a String may def anything; a block's defs are
     the walk's own */
  if (strstr(nm, "eval") || strstr(nm, "exec")) {
    if (an > 0) g_dyn_all = 1;
    return;
  }
  if (an == 0) return;
  const char *ln = sh_lit_method_name(nt, av[0]);
  if (ln) sh_dyn_name_add(ln);
  else g_dyn_all = 1;
}
static void sh_dyn_names_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (g_dyn_c == c && g_dyn_count == nt->count) return;
  g_dyn_c = c;
  g_dyn_count = nt->count;
  g_ndyn_names = 0;
  g_nalias_src = 0;
  g_rebind = 0;
  g_dyn_all = an_user_defines_method(c, "method_missing") || an_user_defines_method(c, "respond_to_missing?");
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    sh_dyn_definer_call(nt, id);
    const char *cn = nt_str(nt, id, "name");
    if (!cn) continue;
    if (sp_streq(cn, "super_method") || sp_streq(cn, "bind") || sp_streq(cn, "bind_call")) g_rebind = 1;
    if (sp_streq(cn, "alias_method")) {
      int args = nt_ref(nt, id, "arguments"), an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      const char *on = an >= 2 ? sh_lit_method_name(nt, av[1]) : NULL;
      if (on) sh_alias_src_add(on);
      else g_rebind = 1;
    }
  }
  /* a bare module_function: the defs after it in the same body */
  NT_FOREACH_KIND(nt, NK_StatementsNode, st) {
    int bn = 0;
    const int *bv = nt_arr(nt, st, "body", &bn);
    int on = 0;
    for (int b = 0; b < bn; b++) {
      NodeKind bk = nt_kind(nt, bv[b]);
      if (bk == NK_CallNode && nt_ref(nt, bv[b], "receiver") < 0 && nt_ref(nt, bv[b], "arguments") < 0 &&
          sp_streq(nt_str(nt, bv[b], "name"), "module_function")) on = 1;
      else if (on && bk == NK_DefNode) sh_dyn_name_add(nt_str(nt, bv[b], "name"));
    }
  }
  NT_FOREACH_KIND(nt, NK_DefNode, d) {
    int r = nt_ref(nt, d, "receiver");
    if (r >= 0 && nt_kind(nt, r) != NK_SelfNode) sh_dyn_name_add(nt_str(nt, d, "name"));
  }
  NT_FOREACH_KIND(nt, NK_AliasMethodNode, a) {
    int nn = nt_ref(nt, a, "new_name"), on = nt_ref(nt, a, "old_name");
    const char *ln = nn >= 0 ? sh_lit_method_name(nt, nn) : NULL;
    if (ln) sh_dyn_name_add(ln);
    else g_dyn_all = 1;
    const char *lo = on >= 0 ? sh_lit_method_name(nt, on) : NULL;
    if (lo) sh_alias_src_add(lo);
    else g_rebind = 1;
  }
}
/* Does the walk see every method a call of nm can run? (a writer may be
   an attr_writer's store, which is no method's scope) */
static int sh_name_methods_seen(Compiler *c, const char *nm) {
  sh_dyn_names_build(c);
  if (g_dyn_all || !nm) return 0;
  size_t l = strlen(nm);
  if (l == 0 || nm[l - 1] == '=') return 0;
  for (int i = 0; i < g_ndyn_names; i++) if (sp_streq(g_dyn_names[i], nm)) return 0;
  return 1;
}
/* Does the def at n hold a super or a bare super of its own (not one in a
   nested def)? Too deep to walk counts as one. */
static int sh_subtree_has_super(const NodeTable *nt, int n, int depth) {
  if (n < 0) return 0;
  if (depth > 200) return 1;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) return 1;
  if (depth > 0 && (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode)) return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++) if (sh_subtree_has_super(nt, nt_ref_at(nt, n, i), depth + 1)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int an = 0;
    const int *av = nt_arr_at(nt, n, i, &an);
    for (int j = 0; j < an; j++) if (sh_subtree_has_super(nt, av[j], depth + 1)) return 1;
  }
  return 0;
}
/* A receiverless call in an instance method can run, on each class made
   whose instances run that method (the class or one below it whose chain
   resolves the method's name to it), the method of the call's name that
   class's chain has: those methods, each once, into out (at most cap), or
   -1 where a builtin or method_missing may answer instead (#9258). A class
   whose chain has none and nothing to answer raises NoMethodError, which
   answers no String. */
static int sh_self_call_targets(Compiler *c, int call, const char *nm, int *out, int cap) {
  const NodeTable *nt = c->nt;
  Scope *cur = comp_scope_of(c, call);
  int ci = cur ? (int)(cur - c->scopes) : -1;
  if (ci <= 0 || cur->is_cmethod || cur->class_id < 0 || !cur->name || cur->def_node < 0) return -1;
  if (core_method_name(nm) || object_public_method_name(nm) || bop_share_named(BOP_KERNEL, nm) ||
      bop_share_named(BOP_ANY_RECV, nm))
    return -1;
  /* the enclosing method may also run on a class whose chain gives its
     name another method: under an alias, or taken out of its class and
     bound to another receiver (sh_dyn_names_build), or through a super
     from that other method */
  sh_dyn_names_build(c);
  if (g_rebind) return -1;
  for (int i = 0; i < g_nalias_src; i++) if (sp_streq(g_alias_src[i], cur->name)) return -1;
  int k = 0;
  for (int cid = 0; cid < c->nclasses; cid++) {
    if (!c->classes[cid].instantiated && !c->classes[cid].is_builtin_const) continue;
    int emi = comp_method_in_chain(c, cid, cur->name, NULL);
    if (emi != ci) {
      /* (a class below the method's own, or any where it is a module's) */
      int below = comp_class_is_module(c, &c->classes[cur->class_id]) || is_descendant(c, cid, cur->class_id);
      if (below && emi > 0 && emi < c->nscopes && sh_subtree_has_super(nt, c->scopes[emi].def_node, 0)) return -1;
      continue;
    }
    int mi = comp_method_in_chain(c, cid, nm, NULL);
    if (mi < 0) {
      if (comp_method_in_chain(c, cid, "method_missing", NULL) >= 0) return -1;
      continue;
    }
    if (c->scopes[mi].is_cmethod) return -1;
    int seen = 0;
    for (int i = 0; i < k && !seen; i++) seen = out[i] == mi;
    if (seen) continue;
    if (k >= cap) return -1;
    out[k++] = mi;
  }
  return k;
}
/* Does any class have an attr_reader `nm`? From a set of every class's
   reader names, built again only when a reader table grows (comp_table_gen;
   they never shrink) or the class count changes: share_name_targets asked
   each class at every call it was asked for. */
static int sh_any_reader(Compiler *c, const char *nm) {
  static ANameHash names;
  static unsigned stamp_gen; static int stamp_ncls = -1;
  if (stamp_ncls != c->nclasses || stamp_gen != comp_table_gen) {
    anh_free(&names);
    for (int ci = 0; ci < c->nclasses; ci++)
      for (int j = 0; j < c->classes[ci].nreaders; j++)
        if (c->classes[ci].readers[j] && !anh_has(&names, c->classes[ci].readers[j]))
          anh_add(&names, c->classes[ci].readers[j]);
    stamp_ncls = c->nclasses; stamp_gen = comp_table_gen;
  }
  return anh_has(&names, nm);
}
/* See share.h. */
int share_name_targets(Compiler *c, int call, int *out, int cap) {
  const NodeTable *nt = c->nt;
  if (call < 0 || nt_kind(nt, call) != NK_CallNode || nt_ref(nt, call, "block") >= 0) return -1;
  if (nt_ref(nt, call, "receiver") < 0) {
    const char *sn = nt_str(nt, call, "name");
    if (cplan_targets_unknown_why(c, call, CPT_MAX) == CPT_WHY_NONE || !sh_name_methods_seen(c, sn)) return -1;
    return sh_self_call_targets(c, call, sn, out, cap);
  }
  int why = cplan_targets_unknown_why(c, call, CPT_MAX);
  if (why != CPT_WHY_CAP && why != CPT_WHY_RECV_UNTYPED) return -1;
  const char *nm = nt_str(nt, call, "name");
  if (!sh_name_methods_seen(c, nm)) return -1;
  /* an attr_reader of the name answers the String its ivar holds, and is
     no method's scope the walk below sees */
  if (sh_any_reader(c, nm)) return -1;
  int str_cid = comp_core_class(c, CORE_STRING), k = 0;
  /* the scopes of the name, in ascending order, through the by-name index */
  for (int mi = an_scopes_named_first(c, nm); mi >= 1; mi = an_scopes_named_next(c, nm, mi)) {
    Scope *m = &c->scopes[mi];
    if (!m->name || m->def_node < 0 || !sp_streq(m->name, nm)) continue;
    /* a String reopen's method: its self is the receiver */
    if (str_cid >= 0 && m->class_id == str_cid && !m->is_cmethod) return -1;
    if (k >= cap) return -1;
    out[k++] = mi;
  }
  return k;
}
/* A call past the plan's list answers a new String where its builtin arms
   do and every method of its name answers one (ret_fresh), answers nothing
   (it only raises) or answers what Object's own inspect or to_s does
   (share_name_targets, an_target_answers_new). A method that answers
   anything else (its self, which may be the String receiver, among them)
   leaves the call's answer unknown. */
static int sh_name_call_fresh(Compiler *c, int call) {
  if (!share_builtin_fresh(c, call)) return 0;
  int *tg = malloc(sizeof(int) * (size_t)(c->nscopes + 1));
  if (!tg) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int n = share_name_targets(c, call, tg, c->nscopes + 1), ok = n > 0;
  for (int i = 0; ok && i < n; i++) {
    if (an_target_answers_new(c, tg[i])) continue;
    if (scope_ret_param(c, tg[i]) >= 0 || !scope_ret_fresh(c, tg[i])) ok = 0;
  }
  free(tg);
  return ok;
}
static int sh_user_call_fresh(Compiler *c, int call, int depth) {
  const ShareFacts *F = c->share;
  if (!F || call < 0 || depth > 8 || nt_kind(c->nt, call) != NK_CallNode) return 0;
  /* an attribute assignment answers the String it handed the writer, not
     what the writer returns */
  if (sh_assign_call(c, call)) return 0;
  /* A boxed dispatch can take a builtin arm too: its user targets alone
     do not prove freshness (String#to_s can answer its receiver). */
  if (cplan_user_fresh(c, call)->via == UC_POLY && !share_builtin_fresh(c, call)) return 0;
  int tg[64];
  int n = cplan_targets(c, call, tg, 64);
  if (n == CPT_UNKNOWN) return sh_name_call_fresh(c, call);
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) {
    if (tg[i] < 0 || tg[i] >= c->nscopes) return 0;
    /* (one that cannot bind the arguments only raises) */
    if (!cplan_target_binds(c, call, tg[i])) continue;
    /* (a class method of a class whose object no box holds) */
    if (!share_target_reachable(c, call, tg[i])) continue;
    /* (a method that answers its parameter answers what the caller has) */
    if (scope_ret_param(c, tg[i]) >= 0 || !scope_ret_fresh(c, tg[i])) return 0;
  }
  return 1;
}
/* Does the subtree at n hold a `next` that leaves it (not one in a nested
   block, lambda, method or loop)? With any, also a break or a return. */
static int sh_has_jump_k(const NodeTable *nt, int n, int any) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NextNode || (any && (k == NK_BreakNode || k == NK_ReturnNode))) return 1;
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_WhileNode || k == NK_UntilNode ||
      k == NK_ForNode)
    return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++) if (sh_has_jump_k(nt, nt_ref_at(nt, n, i), any)) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (sh_has_jump_k(nt, ids[j], any)) return 1;
  }
  return 0;
}
static int sh_has_next(const NodeTable *nt, int n) { return sh_has_jump_k(nt, n, 0); }
static int sh_has_jump(const NodeTable *nt, int n) { return sh_has_jump_k(nt, n, 1); }
/* share.h */
int share_block_returns(const Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1, n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  return n == 0 || nt_kind(nt, st[n - 1]) != NK_BreakNode || sh_has_next(nt, body);
}

/* Can a `prepend` or an `extend` the program calls put module mname ahead
   of a builtin's own methods: a call of either naming it, or one whose
   argument is no constant? A module only included, which a class's own
   methods come before, cannot. A constant that aliases the module
   (`Alias = Overrides`) names it just as well, also through a chain of
   them (`Outer = Inner`, `Inner = Overrides`): each link is followed
   (const_alias_source) until a class or module of the program's own, at
   most once per constant, so a cycle ends. */
static int sh_module_put_ahead(Compiler *c, const char *mname) {
  const char *tail = mname ? strrchr(mname, ':') : NULL;
  tail = tail ? tail + 1 : mname;
  const char *names[2] = { "prepend", "extend" };
  for (int j = 0; j < 2; j++)
    for (int id = an_calls_named_first(c, names[j]); id >= 0; id = an_calls_named_next(id)) {
      int argc = 0;
      const int *argv = call_args(c->nt, id, &argc);
      for (int i = 0; i < argc; i++) {
        NodeKind k = nt_kind(c->nt, argv[i]);
        const char *an = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(c->nt, argv[i], "name") : NULL;
        for (int d = 0; an && comp_class_index(c, an) < 0 && d < c->nconsts; d++) {
          const char *src = const_alias_source(c, an);
          if (!src) break;
          an = src;
        }
        if (!an || (tail && sp_streq(an, tail))) return 1;
      }
    }
  return 0;
}
int share_builtin_overridden(Compiler *c, const char *cls, const char *name) {
  int n = 0;
  const PolyCand *p = name ? comp_poly_candidates(c, name, &n) : NULL;
  for (int i = 0; i < n; i++) {
    if (p[i].mi < 0 && p[i].rdcls < 0) continue;
    ClassInfo *ci = &c->classes[p[i].cls];
    if (ci->name && sp_streq(ci->name, cls)) return 1;
    if (comp_class_is_module(c, ci) && sh_module_put_ahead(c, ci->name)) return 1;
  }
  return 0;
}
/* Does share row `share`, with argc arguments, answer a container of some
   of the receiver's own elements and of nothing else: a run of them
   (BSH_SUB, a selection BSH_ITER_SEL, BSH_FLATTEN), or a counted element
   answer (`first(2)`, `min(2)`)? */
static int sh_answers_elems_run(int share, int argc) {
  return share == BSH_SUB || share == BSH_ITER_SEL || share == BSH_FLATTEN ||
         ((share == BSH_ELEM_N || share == BSH_ITER_FIND) && argc >= 1);
}
/* Is node r a temporary container (its own expression, held by no name)
   whose elements are new Strings: an Array literal of them, or `map(&:sym)`
   over elements that hold no String (Symbols, numbers), whose answers are
   new? */
static int sh_fresh_elems(Compiler *c, int r, int depth) {
  const NodeTable *nt = c->nt;
  r = nt_paren_single(nt, r);
  if (r < 0 || depth > 8) return 0;
  if (nt_kind(nt, r) == NK_ArrayNode) {
    int en = 0; const int *el = nt_arr(nt, r, "elements", &en);
    for (int i = 0; i < en; i++) if (!share_value_fresh(c, el[i], depth + 1)) return 0;
    return 1;
  }
  if (nt_kind(nt, r) != NK_CallNode || sh_has_targets(c, r)) return 0;
  int blk = nt_ref(nt, r, "block"), recv = nt_ref(nt, r, "receiver");
  const char *nm = nt_str(nt, r, "name");
  /* a builtin answering a container of its receiver's own elements (the
     share rows' run of them: sort, uniq, drop, reverse, compact, flatten,
     take, a counted first or min) over such a temporary container holds the
     same new Strings, which no name holds either. (A block's parameter
     names them: `sort_by { |x| ... }` is left to the block's own facts, and
     a name the program may answer itself to the Array rows,
     share_builtin_overridden.) */
  if (nm && recv >= 0 && blk < 0 && ty_is_array(c->ntype[recv]) &&
      sh_answers_elems_run(bop_share_named(BOP_ANY_ARRAY, nm), call_plain_argc(c, r)) &&
      !share_builtin_overridden(c, "Array", nm))
    return sh_fresh_elems(c, recv, depth + 1);
  if (!nm || recv < 0 || blk < 0 || bop_share_named(BOP_ANY_ARRAY, nm) != BSH_ITER_MAP) return 0;
  /* map with a literal block whose value is a new String, and no next */
  if (nt_kind(nt, blk) == NK_BlockNode) {
    int body = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn > 0 && !sh_has_next(nt, body) && share_value_fresh(c, bb[bn - 1], depth + 1);
  }
  if (nt_kind(nt, blk) != NK_BlockArgumentNode || nt_kind(nt, nt_ref(nt, blk, "expression")) != NK_SymbolNode) return 0;
  TyKind at = c->ntype[recv];
  TyKind et = ty_is_array(at) ? ty_array_elem(at) : TY_UNKNOWN;
  return et != TY_UNKNOWN && !sh_may_hold(c, et);
}

/* Does every block method mi can run answer a new String: its literal
   blocks' values (with no next, break or return), and those of the methods
   that hand mi their own block, with no block the facts do not list? Asked
   once per method (fresh_blk); a cycle answers no. */
static int sh_blocks_fresh(Compiler *c, int mi, int depth) {
  ShareFacts *F = c->share;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  if (!F) return 0;
  if (F->fresh_blk[mi] >= 0) return F->fresh_blk[mi];
  if (depth > 8 || F->blk_unk[mi] || m->is_lowered_yield || m->is_proc_form || !F->mb_start) return 0;
  F->fresh_blk[mi] = 0;
  int ok = 1;
  for (int i = F->mb_start[mi]; ok && i < F->mb_start[mi + 1]; i++) {
    int body = nt_ref(nt, F->mb_blk[i], "body");
    int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
    ok = bn > 0 && !sh_has_jump(nt, body) && share_value_fresh(c, bb[bn - 1], depth + 1);
  }
  for (int e = 0; ok && e < F->nfw; e++)
    if (F->fw_to[e] == mi) ok = sh_blocks_fresh(c, F->fw_from[e], depth + 1);
  F->fresh_blk[mi] = (signed char)ok;
  return ok;
}

/* Is callable literal k's value (pivs_callable_lit: a lambda's, a proc's
   or a method's tail, with no jump) a new String, or a value no String
   is? */
static int sh_callable_fresh(Compiler *c, int k, int depth) {
  const NodeTable *nt = c->nt;
  const char *un = nt_kind(nt, k) == NK_CallNode ? nt_str(nt, k, "name") : NULL;
  int body;
  if (is_method_ref_name(un)) {
    int a = nt_ref(nt, k, "arguments"), ac = 0;
    const int *av = nt_arr(nt, a, "arguments", &ac);
    int mi = comp_method_index(c, nt_str(nt, av[0], "value"));
    body = mi >= 0 && c->scopes[mi].def_node >= 0 ? nt_ref(nt, c->scopes[mi].def_node, "body") : -1;
  }
  else body = nt_ref(nt, un ? nt_ref(nt, k, "block") : k, "body");
  int bn = 0; const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn == 0 || sh_has_jump(nt, body)) return 0;
  TyKind t = c->ntype[bb[bn - 1]];
  return (t != TY_STRING && t != TY_STRBUF && t != TY_POLY && t != TY_UNKNOWN) ||
         share_value_fresh(c, bb[bn - 1], depth + 1);
}
/* Is `call` n, on a callable the boxed-receiver walk bounds to literals
   (pivs_callables), a new String or no String whichever it runs? */
static int sh_call_fresh(Compiler *c, int n, int depth) {
  ShareFacts *F = c->share;
  if (!F) return 0;
  if (!F->fresh_call) {
    F->fresh_call = malloc((size_t)(F->nnodes > 0 ? F->nnodes : 1));
    if (!F->fresh_call) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(F->fresh_call, -1, (size_t)(F->nnodes > 0 ? F->nnodes : 1));
  }
  if (n >= F->nnodes) return 0;
  if (F->fresh_call[n] >= 0) return F->fresh_call[n];
  F->fresh_call[n] = 0;
  int ks[16];
  int nk = pivs_callables(c, n, ks, 16);
  int ok = nk > 0;
  for (int i = 0; ok && i < nk; i++) ok = sh_callable_fresh(c, ks[i], depth + 1);
  F->fresh_call[n] = (signed char)ok;
  return ok;
}

/* Is node n the frozen String literal itself: a literal of a file whose
   literals are frozen (`fzl`), or a `freeze`, `-@` or `dedup` of one, which
   answer their receiver? Every evaluation answers the one object the
   literal names, so the handle a shared slot takes for it is that
   literal's own (sp_String_literal_handle), never a new one. Adjacent
   literals (`"a" "b"`) and a squiggly heredoc of mixed indents parse as an
   interpolated String that emit_interp folds into one literal: that is one
   too (interp_is_literal_fold). A `+"lit"` is a call that copies, and a
   String with a part to evaluate is built each time: neither is one. */
int share_frozen_literal(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  /* parentheses are the frozen literal their value is (vp_one), as
     share_value_fresh reads them: `(x; "lit")` is "lit" */
  if (vp_one(nt, n, VPS_NONE | VP_PARENS_ONLY, &n) != VP1_NODE) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_StringNode) return nt_int(nt, n, "fzl", 0) != 0;
  if (k == NK_InterpolatedStringNode) return nt_int(nt, n, "fzl", 0) != 0 && interp_is_literal_fold(nt, n);
  if (k != NK_CallNode || nt_ref(nt, n, "arguments") >= 0 || nt_ref(nt, n, "block") >= 0 ||
      (bop_share_named(TY_STRING, nt_str(nt, n, "name")) != BSH_FROZEN && !is_freeze_name(nt_str(nt, n, "name")))) return 0;
  return share_frozen_literal(c, nt_ref(nt, n, "receiver"));
}
/* Is unary `+` of n a new String: n a builtin String constant
   (share_frozen_string), or what the builtin freeze answers, frozen
   whatever its receiver (`+RUBY_ENGINE.freeze`, `+s.freeze`)? */
static int sh_uplus_copies(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int u = nt_paren_single(nt, n);
  /* (a call every target of which answers a String literal, frozen:
     share_plan_call_answers_literal) */
  if (share_plan_call_answers_literal(c, u)) return 1;
  if (u >= 0 && nt_kind(nt, u) == NK_CallNode && is_freeze_name(nt_str(nt, u, "name")) && call_plain_argc(c, u) == 0 &&
      nt_ref(nt, u, "receiver") >= 0 && nt_ref(nt, u, "block") < 0 && !sh_has_targets(c, u))
    return 1;
  return share_frozen_string(c, n);
}
int share_frozen_string(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int u = nt_paren_single(nt, n);
  if (u >= 0 && nt_kind(nt, u) == NK_ConstantReadNode) {
    const char *nm = nt_str(nt, u, "name");
    return is_engine_string_const(nm) && !comp_const(c, nm);
  }
  /* (File::NULL and the separators, static literals while the program
     has no File of its own) */
  if (u >= 0 && nt_kind(nt, u) == NK_ConstantPathNode) {
    int par = nt_ref(nt, u, "parent");
    const char *pn = par >= 0 && nt_kind(nt, par) == NK_ConstantReadNode ? nt_str(nt, par, "name") : NULL;
    /* (or while a File of the program writes no constant of the name: a
       reopen with methods of its own, const_path_in_closed_builtin) */
    return pn && sp_streq(pn, "File") && (comp_core_class(c, CORE_FILE) < 0 || const_path_in_closed_builtin(c, u)) &&
           is_file_string_const(nt_str(nt, u, "name"));
  }
  return share_frozen_literal(c, n);
}
/* Is call n a builtin whose share row answers a new value (PURE), for its
   receiver's family or any receiver, reaching no method of the program's? */
static int sh_builtin_call_pure(Compiler *c, int n) {
  const char *name = nt_str(c->nt, n, "name");
  if (!name || is_receiver_conversion(name) || sh_has_targets(c, n)) return 0;
  int recv = nt_ref(c->nt, n, "receiver");
  int s = recv >= 0 ? bop_share_named(sh_family(c->ntype[recv]), name) : bop_share_named(BOP_KERNEL, name);
  if (!s) s = bop_share_named(BOP_ANY_RECV, name);
  return s == BSH_PURE;
}
/* Is value n, a leaf of share_value_fresh's walk, a new String or nil: a
   builtin that answers a new value, a conversion, a yield or a super that
   answers one, an element read of a temporary container of them? */
static int sh_value_fresh_leaf(Compiler *c, int n, int depth) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NilNode) return 1;
  if (k == NK_StringNode || k == NK_InterpolatedStringNode) return 1;
  /* a builtin whose share row answers a new value, reaching no method of
     the program's */
  if (k == NK_CallNode && sh_builtin_call_pure(c, n)) return 1;
  /* Kernel#String of a value whose #to_str or #to_s the program defines: a
     new String when each of them answers one */
  int conv = k == NK_CallNode ? sh_string_conv_arg(c, n) : -1, mis[64];
  int nconv = conv >= 0 ? sh_string_conv_methods(c, conv, mis, 64) : 0;
  if (nconv > 0) {
    for (int i = 0; nconv <= 64 && i < nconv; i++)
      if (mis[i] < 0 || scope_ret_param(c, mis[i]) >= 0 || !scope_ret_fresh(c, mis[i]))
        return 0;
    /* (a boxed argument may be a String, which Kernel#String answers itself) */
    return nconv <= 64 && (ty_is_object(c->ntype[conv]) || share_value_fresh(c, conv, depth + 1));
  }
  if (k == NK_YieldNode) {
    int mi = sh_method_index(c, n);
    return mi >= 0 && sh_blocks_fresh(c, mi, depth + 1);
  }
  /* super answers what the method it reaches answers: a new String where
     that method's returns joined nothing else, or each answers one, as a
     call of it does (sh_user_call_fresh); not where it answers a
     parameter, which zsuper hands on unseen */
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    const CallPlan *p = cplan_user_fresh(c, n);
    int mi = p->mi;
    if (!c->share || p->dispatch != CP_DIRECT || mi <= 0 || mi >= c->nscopes || scope_ret_param(c, mi) >= 0)
      return 0;
    return scope_ret_fresh(c, mi);
  }
  if (k != NK_CallNode) return 0;
  if (sh_user_call_fresh(c, n, depth)) return 1;
  /* ENV's [] answers a new String each read; to_s, to_str, itself and
     freeze answer a new String receiver itself, and a bang answers it or
     nil (`pick(s).upcase!`, pick answering `value + ""`) */
  int rcv = nt_ref(nt, n, "receiver");
  const char *cn = nt_str(nt, n, "name");
  if (cn && rcv >= 0 && nt_ref(nt, n, "block") < 0 && !sh_has_targets(c, n)) {
    if (nt_kind(nt, rcv) == NK_ConstantReadNode && is_env_const(nt_str(nt, rcv, "name")) && is_aref_name(cn))
      return 1;
    if ((is_receiver_conversion(cn) || is_freeze_name(cn)) && call_plain_argc(c, n) == 0)
      return share_value_fresh(c, rcv, depth + 1);
    /* `+` of a frozen String (a builtin String constant, share_frozen_string)
       is a new String, as `+"lit"` is */
    if (is_unary_plus(cn) && call_plain_argc(c, n) == 0 && sh_uplus_copies(c, rcv)) return 1;
    if (bop_share_self_answer(cn, 0)) return share_value_fresh(c, rcv, depth + 1);
    /* a proc, a lambda or a Method the walk bounds to literals */
    if (is_call_alias(cn) && sh_call_fresh(c, n, depth)) return 1;
  }
  /* an element read of a temporary container of new Strings */
  int recv = nt_ref(nt, n, "receiver");
  const char *nm = nt_str(nt, n, "name");
  int sh = nm && recv >= 0 && !sh_has_targets(c, n) ? bop_share_named(BOP_ANY_ARRAY, nm) : 0;
  /* (max and min with no block or count answer one of them too) */
  if (sh == BSH_ITER_FIND && is_minmax_query(nm) && call_plain_argc(c, n) == 0 &&
      !share_builtin_overridden(c, "Array", nm)) sh = BSH_ELEM;
  return (sh == BSH_ELEM || sh == BSH_ELEM_N) && nt_ref(nt, n, "block") < 0 && sh_fresh_elems(c, recv, depth + 1);
}
typedef struct { Compiler *c; int depth; } ShFresh;
static int sh_fresh_arm_bad(const VpLeaf *lf, void *ud) {
  const ShFresh *f = ud;
  /* a conditional's missing arm and an empty body answer nil */
  if (lf->kind == VPK_NIL) return 0;
  if (lf->kind == VPK_GAVE_UP) return 1;
  /* a rescue modifier is a leaf of the walk: each operand is a value of its
     own, asked as the whole is */
  if (nt_kind(f->c->nt, lf->node) == NK_RescueModifierNode)
    return !(share_value_fresh(f->c, nt_ref(f->c->nt, lf->node, "expression"), f->depth + 1) &&
             share_value_fresh(f->c, nt_ref(f->c->nt, lf->node, "rescue_expression"), f->depth + 1));
  return !sh_value_fresh_leaf(f->c, lf->node, f->depth);
}
/* Is value n a new String or nil: a conditional each of whose arms is one
   (or nil), a rescue modifier whose expression and fallback are, or such a
   leaf? The arms of the conditional are read through the iterator, and so
   are parentheses: `(x; s.dup)` is as new as `s.dup`, and `()` is nil.
   `depth` bounds the leaves' own chains of calls, not the shapes. */
int share_value_fresh(Compiler *c, int n, int depth) {
  if (n < 0 || depth > 8) return 0;
  ShFresh f = { c, depth };
  return !for_each_value_position(c->nt, n, VPS_IF | VPS_UNLESS, sh_fresh_arm_bad, &f);
}

/* A fresh value, or a local's fresh value handed on at its only read. */
int share_value_unobserved(Compiler *c, int n) {
  return share_value_fresh(c, n, 0) || an_local_read_once(c, n);
}

/* Does the String (or a box that may hold one) node v evaluates to have to
   be the String's handle where it is emitted -- stored, boxed, mutated --
   and not its bytes? Under the one representation every String does, but
   nil, a literal (a String literal is its own handle; a container, a Range
   or a Symbol holds none), a value of a type that cannot hold a String, one
   in a method no call reaches, and a frozen String, whose handle is the
   literal's own. */
int share_value_needs_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || v >= nt->count) return SHN_NO;
  NodeKind k = nt_kind(nt, v);
  if (k == NK_NilNode || k == NK_StringNode || k == NK_ArrayNode || k == NK_HashNode ||
      k == NK_RangeNode || k == NK_SymbolNode)
    return SHN_NO;
  TyKind t = c->ntype[v];
  if (t != TY_STRING && t != TY_STRBUF && t != TY_POLY) return SHN_NO;
  Scope *sc = comp_scope_of(c, v);
  if (!sc || !sc->reachable || share_frozen_string(c, v)) return SHN_NO;
  return SHN_YES;
}
int share_store_needs_handle(Compiler *c, int v) { return share_value_needs_handle(c, v); }

static int sh_ivar_owner(Compiler *c, int node);
/* A String method answering its receiver or nil (bop_share_self_answer:
   a bang method, an iterator given a block) called on a slot that holds
   the shared handle -- a local, a global, a constant, a class variable or
   an ivar: its value is that slot's String, or nil. */
static int sh_bang_self_slot(const Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode ||
      !bop_share_self_answer(nt_str(nt, v, "name"), nt_ref(nt, v, "block") >= 0)) return 0;
  int r = nt_ref(nt, v, "receiver");
  if (r < 0) return 0;
  if (nt_kind(nt, r) == NK_LocalVariableReadNode) return repr_of(c, r).kind == RK_STRBUF;
  if (repr_static_share(c, r)) return 1;
  if (nt_kind(nt, r) == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, r, "name");
    int cid = nm ? sh_ivar_owner((Compiler *)c, r) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && repr_of_ivar(c, cid, iv).share;
  }
  return 0;
}
/* Is call u `x.to_s` on a box that the program's own to_s methods make a
   dispatch answering a box, whose String arm is the builtin default
   (PB_D_TO_S in the plan): under the rule it answers a String receiver's
   box itself (emit_poly_defaults0), unless a reopening of String's to_s or
   the String trial takes the String first? */
static int sh_poly_to_s_self(const Compiler *c, int u) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, u) != NK_CallNode || !is_to_s_name(nt_str(nt, u, "name")) ||
      call_plain_argc((Compiler *)c, u) != 0 || nt_ref(nt, u, "block") >= 0)
    return 0;
  int recv = nt_ref(nt, u, "receiver");
  int sci = comp_class_index((Compiler *)c, "String");
  if (recv < 0 || repr_of(c, recv).kind != RK_BOXED ||
      (sci >= 0 && comp_method_in_chain((Compiler *)c, sci, "to_s", NULL) >= 0))
    return 0;
  const PolyPlan *p = cplan_poly((Compiler *)c, u);
  int dflt = 0;
  for (int i = 0; i < p->n; i++) {
    if (p->arm[i].kind == PA_TRIAL && p->arm[i].key == PA_KEY_TRIAL + PT_STR) return 0;
    dflt |= p->arm[i].kind == PA_BUILTIN && p->arm[i].key == PA_KEY_BUILTIN + PB_D_TO_S;
  }
  return dflt && p->ret == TY_POLY;
}
/* Is call u's one target a `def m = @iv` reader of an instance method,
   its whole body that read, of a slot its class keeps boxed? */
static int sh_def_reader_box(const Compiler *c, int u) {
  const NodeTable *nt = c->nt;
  int tg[CPT_MAX];
  if (cplan_targets((Compiler *)c, u, tg, CPT_MAX) != 1 || tg[0] <= 0) return 0;
  const Scope *m = &c->scopes[tg[0]];
  if (m->is_cmethod || m->class_id < 0 || m->class_id >= c->nclasses || m->nparams != 0) return 0;
  int n = 0;
  const int *st = m->body >= 0 && nt_kind(nt, m->body) == NK_StatementsNode ? nt_arr(nt, m->body, "body", &n) : NULL;
  if (n != 1 || nt_kind(nt, st[0]) != NK_InstanceVariableReadNode) return 0;
  const ClassInfo *ci = &c->classes[m->class_id];
  int ix = comp_ivar_index((ClassInfo *)ci, nt_str(nt, st[0], "name"));
  return ix >= 0 && ci->ivar_types[ix] == TY_POLY;
}
/* The kind (share.h's SPK_*) of the route by which node n hands over the
   shared handle (or a box holding it), not a copy of its bytes, or
   SPK_NONE: the carry share_node_carries_handle checks, arm by arm; *op as
   share_plan_producer's. With by_repr 0, a value represented as the handle
   (repr_of's RK_STRBUF, or any strbuf_src) is not taken as one by that
   alone: the String plan reads the representation itself. With lift 0, a
   String a store lifts into a handle (poly_strbuf_lift) is asked the
   other routes, as the String it is before the store's lift. */
static int sh_carry_kind(const Compiler *c, int n, int by_repr, int lift, int *op) {
  *op = -1;
  /* The return route takes the handle its callee publishes, including
     when a borrowed parameter is returned beside a method-owned String. */
  if (repr_call_returns_handle((Compiler *)c, n)) return SPK_PUBLISHED;
  /* freeze answers its receiver: it hands on the handle where its receiver
     does, and a new String's freeze is a String no other name holds */
  int u = nt_paren_single(c->nt, n);
  if (u >= 0 && nt_kind(c->nt, u) == NK_CallNode && is_freeze_name(nt_str(c->nt, u, "name")) &&
      call_plain_argc((Compiler *)c, u) == 0 && nt_ref(c->nt, u, "block") < 0 &&
      cplan_user((Compiler *)c, u)->dispatch == CP_NONE) {
    int r = nt_ref(c->nt, u, "receiver");
    if (r >= 0 && (share_value_fresh((Compiler *)c, r, 0) || share_node_carries_handle(c, r))) {
      *op = nt_paren_single(c->nt, r);
      return SPK_SELF;
    }
  }
  /* A call on a boxed receiver each of whose arms publishes its String's
     handle: an attr reader of a field that holds the handle (its arm reads
     it through sp_strbuf_read_pub), or a method that publishes its own. A
     pickup around the call takes that handle, so the value is the field's
     String itself: `{ host: req.host }` with two classes answering host. */
  if (u >= 0 && nt_kind(c->nt, u) == NK_CallNode && nt_ref(c->nt, u, "receiver") >= 0 && repr_of(c, nt_ref(c->nt, u, "receiver")).kind == RK_BOXED &&
      nt_ref(c->nt, u, "block") < 0) {
    const PolyPlan *pp = cplan_poly_arms((Compiler *)c, u);
    int all = pp->n > 0;
    for (int i = 0; i < pp->n && all; i++) {
      const PolyArm *a = &pp->arm[i];
      if (a->kind == PA_READER) all = a->vty == TY_STRBUF;
      /* (a method whose return is the handle itself, `h[k] = v`'s value,
         answers it as a reader's arm does) */
      else if (a->kind == PA_USER) all = a->mi > 0 && (scope_ret_handle((Compiler *)c, a->mi) || a->vty == TY_STRBUF);
      else all = 0;
    }
    if (all) return SPK_POLY_ARMS;
    /* ...or one whose other arms answer a new String beside it: the
       dispatch boxes each arm's own answer, the published handle or a new
       handle around the fresh String (strbuf_poly_user_boxes), as the
       String plan has it */
    if (strbuf_poly_user_boxes((Compiler *)c, u)) return SPK_POLY_ARMS;
  }
  /* An attr reader of a boxed field on a receiver of one class: a field
     whose class the rule shares stores its Strings as the handle
     (share_lift_poly_ivar_stores), so the box the reader answers is the
     handle's own; `{ host: req.host }` with `attr_accessor :host` */
  if (u >= 0 && nt_kind(c->nt, u) == NK_CallNode && call_plain_argc((Compiler *)c, u) == 0 && nt_ref(c->nt, u, "block") < 0) {
    int rv = nt_ref(c->nt, u, "receiver");
    TyKind rt = rv >= 0 ? comp_ntype((Compiler *)c, rv) : TY_UNKNOWN;
    const char *rn = nt_str(c->nt, u, "name");
    int kd = -1;
    if (rn && ty_is_object(rt) && comp_reader_in_chain((Compiler *)c, ty_object_class(rt), rn, &kd) &&
        ({ int _tg[CPT_MAX]; cplan_targets((Compiler *)c, u, _tg, CPT_MAX) == 0; })) {
      char ivn[260]; snprintf(ivn, sizeof ivn, "@%s", rn);
      ClassInfo *ci = &c->classes[kd >= 0 ? kd : ty_object_class(rt)];
      int ix = comp_ivar_index(ci, ivn);
      if (ix >= 0 && ci->ivar_types[ix] == TY_POLY) return SPK_READER;
    }
    /* ...and so does a `def m = @iv` reader, the one method the call
       reaches, whose whole body reads the slot: it returns the box */
    if (rn && ty_is_object(rt) && sh_def_reader_box(c, u)) return SPK_READER;
  }
  Repr r = repr_of(c, n);
  /* A boxed holder's read keeps what its incoming flows stored there. */
  if (r.kind == RK_BOXED && strbuf_boxed_local((Compiler *)c, n)) return SPK_BOX;
  /* So does a boxed field's read through a receiverless attr reader,
     `self->iv_x` (implicit_self_is_field_read), as the arm above takes one
     with a receiver: a box allocates no copy. Its type can widen to the box
     after the route was recorded on a String. */
  if (r.kind == RK_BOXED && u >= 0 && nt_kind(c->nt, u) == NK_CallNode && nt_ref(c->nt, u, "receiver") < 0 &&
      implicit_self_is_field_read((Compiler *)c, u))
    return SPK_BOX;
  /* A boxed to_s answering a String receiver's own box carries what the
     receiver's box does. */
  if (r.kind == RK_BOXED && u >= 0 && sh_poly_to_s_self(c, u))
    return share_node_carries_handle(c, nt_ref(c->nt, u, "receiver")) ? SPK_BOX : SPK_NONE;
  /* The boxed unary-plus arm keeps a mutable String's handle and copies
     a frozen one, exactly as the typed value route does. */
  if (r.kind == RK_BOXED && nt_kind(c->nt, n) == NK_CallNode &&
      is_unary_plus(nt_str(c->nt, n, "name")) &&
      cplan_user((Compiler *)c, n)->dispatch == CP_NONE) return SPK_BOX;
  /* A boxed append answers its receiver's box, appended in place: a chain
     over a base that carries (`(+v) << x`) keeps that base's handle. */
  if (r.kind == RK_BOXED && nt_kind(c->nt, n) == NK_CallNode) {
    int cur = n, tg[CPT_MAX];
    while (cur >= 0 && nt_kind(c->nt, cur) == NK_CallNode && nt_str(c->nt, cur, "name") &&
           is_append_concat(nt_str(c->nt, cur, "name")) && call_plain_argc((Compiler *)c, cur) == 1 &&
           nt_ref(c->nt, cur, "block") < 0 && cplan_targets((Compiler *)c, cur, tg, CPT_MAX) == 0)
      cur = nt_paren_single(c->nt, nt_ref(c->nt, cur, "receiver"));
    if (cur >= 0 && cur != n && repr_of(c, cur).kind == RK_BOXED && share_node_carries_handle(c, cur)) return SPK_BOX;
  }
  /* a bang method on a handle local: a write hands over the local's handle
     (emit_strbuf_value) */
  if (by_repr && (r.kind == RK_STRBUF || r.strbuf_src != RS_NONE)) return SPK_SLOT;
  if (sh_bang_self_slot(c, n)) {
    *op = nt_paren_single(c->nt, nt_ref(c->nt, n, "receiver"));
    return SPK_SELF;
  }
  /* a boxed variable's read lifted into the handle (poly_strbuf_lift) */
  if (lift && r.poly_lift) return SPK_LIFT;
  /* A yielding call's result and a receiver-returning expression
     use the same handle routes as their eventual store. */
  return strbuf_value_kind((Compiler *)c, n, op);
}
/* share.h: does node n hand over the shared handle (or a box holding it),
   not a copy of its bytes? */
int share_node_carries_handle(const Compiler *c, int n) {
  int op;
  return sh_carry_kind(c, n, 1, 1, &op) != SPK_NONE;
}
int share_node_carry_kind(const Compiler *c, int n, int *op) { return sh_carry_kind(c, n, 0, 1, op); }
int share_node_carry_kind_unlifted(const Compiler *c, int n, int *op) { return sh_carry_kind(c, n, 0, 0, op); }

/* ---- a Hash key borrows a handle's bytes ----

   A String the rule shares is an sp_String * handle, and a String-keyed
   Hash call handed one as its key read it out through the handle's read
   face, a full copy, before the store copied it again (sp_hash_key_str, the
   copy CRuby makes when it dups and freezes a new key) or the lookup only
   compared it. The key can take the handle's live buffer instead, as
   strbuf_read_raw already lets a builtin accessor do (#5745), when nothing
   runs between the borrow and the call: the receiver and every other
   operand are plain reads. (A read-only parameter and a bound C function
   borrow through master's own passes, mark_param_read_only_operands and
   mark_native_str_operands, which run under the flag too.) */

/* An operand evaluated beside the borrowed argument: a variable read, a
   literal, or scalar arithmetic over those. Nothing in it can change the
   String between the borrow and the call. */
static int sh_plain_operand(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 1;
  switch (nt_kind(nt, n)) {
  case NK_LocalVariableReadNode: case NK_InstanceVariableReadNode: case NK_GlobalVariableReadNode:
  case NK_ConstantReadNode: case NK_SelfNode: case NK_IntegerNode: case NK_FloatNode:
  case NK_StringNode: case NK_SymbolNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
    return 1;
  case NK_CallNode: {
    int recv = nt_ref(nt, n, "receiver");
    TyKind rt = recv >= 0 ? c->ntype[recv] : TY_VOID;
    /* a freeze moves no payload and changes no byte: a borrowed key read
       beside one stays the live buffer (`h[k] = k.freeze`) */
    if ((rt == TY_STRING || rt == TY_STRBUF) && is_freeze_name(nt_str(nt, n, "name")) &&
        nt_ref(nt, n, "arguments") < 0 && nt_ref(nt, n, "block") < 0 && !sh_has_targets(c, n))
      return sh_plain_operand(c, recv);
    if ((rt != TY_INT && rt != TY_FLOAT) || nt_ref(nt, n, "block") >= 0) return 0;
    if (sh_has_targets(c, n) || !sh_plain_operand(c, recv)) return 0;
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    for (int i = 0; i < argc; i++)
      if (!sh_plain_operand(c, argv[i])) return 0;
    return 1;
  }
  default:
    return 0;
  }
}

/* Is argument node a a read of a String slot held as the shared handle,
   which reads out through the copying face? */
static int sh_handle_read(Compiler *c, int a) {
  const NodeTable *nt = c->nt;
  if (c->strbuf_box[a] || c->strbuf_handle_demand[a] || c->strbuf_read_raw[a]) return 0;
  if (nt_kind(nt, a) == NK_LocalVariableReadNode) {
    const char *ln = nt_str(nt, a, "name");
    LocalVar *lv = ln ? scope_local(comp_scope_of(c, a), ln) : NULL;
    return lv && lv->type == TY_STRBUF && repr_of_slot(c, lv).kind == RK_STRBUF;
  }
  if (nt_kind(nt, a) == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, a, "name");
    int cid = nm ? sh_ivar_owner(c, a) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF;
  }
  return 0;
}

/* Is call n a builtin String-keyed Hash call whose first argument is a key
   it only compares, or copies to store? */
static int sh_hash_key_call(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, n, "receiver");
  const char *name = nt_str(nt, n, "name");
  if (recv < 0 || !name) return 0;
  TyKind rt = c->ntype[recv];
  if (rt != TY_STR_INT_HASH && rt != TY_STR_STR_HASH && rt != TY_STR_POLY_HASH) return 0;
  if (sh_has_targets(c, n)) return 0;
  if (is_store_alias(name)) return 1;
  return is_hash_key_lookup(name);
}

int share_mark_borrows(Compiler *c) {
  const NodeTable *nt = c->nt;
  int marked = 0;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    int args = nt_ref(nt, n, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc == 0 || nt_ref(nt, n, "block") >= 0) continue;
    /* a String-keyed Hash's key: a lookup only compares it, and a store
       copies it (sp_hash_key_str), as CRuby dups and freezes it */
    if (sh_hash_key_call(c, n) && sh_handle_read(c, argv[0]) &&
        sh_plain_operand(c, nt_ref(nt, n, "receiver"))) {
      int plain = 1;
      for (int i = 1; i < argc && plain; i++) plain = sh_plain_operand(c, argv[i]);
      if (plain) { c->strbuf_read_raw[argv[0]] = 1; marked++; }
    }
  }
  /* a String-keyed Hash literal stores each entry as such a call does:
     its key, beside a plain value */
  NT_FOREACH_KIND(nt, NK_HashNode, n) {
    TyKind ht = c->ntype[n];
    if (ht != TY_STR_INT_HASH && ht != TY_STR_STR_HASH && ht != TY_STR_POLY_HASH) continue;
    int en = 0; const int *el = nt_arr(nt, n, "elements", &en);
    for (int i = 0; i < en; i++) {
      if (nt_kind(nt, el[i]) != NK_AssocNode) continue;
      int k = nt_ref(nt, el[i], "key");
      if (k >= 0 && sh_handle_read(c, k) && sh_plain_operand(c, nt_ref(nt, el[i], "value"))) {
        c->strbuf_read_raw[k] = 1;
        marked++;
      }
    }
  }
  c->share_borrows = marked;
  return marked;
}

