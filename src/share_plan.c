/* share_plan.c -- the plan for each String value under --share-strings (#8328).

   Whether a String reaches its consumer as the object or as a copy is
   decided today at each emission site, and predicted by the analysis
   through a mirror it keeps of the emitters (strbuf_flow_carries). The
   plan is the one place that decision is meant to come from. For each
   value:

     need    what its consumer needs (share_value_needs_handle): its bytes
             (SPN_BYTES); a handle that may be a new one, because no other
             name can see the String, but a box keeps it (SPN_NEW); or the
             object itself (SPN_OBJECT);
     supply  whether its producer can hand over the handle, or a box that
             holds it (SPS_HANDLE, share_plan_supplies), or only its bytes
             (SPS_BYTES);
     form    what the two give (share_plan_form): the bytes (SPF_BYTES);
             the producer's handle (SPF_HANDLE); the bytes wrapped into a
             new handle, which no other name can tell from the object
             (SPF_WRAP); or no conversion at all, because the consumer
             needs the object and the producer has only bytes (SPF_REFUSE).

   A need is a consumer's. A value has one consumer, the node it sits in,
   whose role says what it does with it (share_plan_role): it takes the
   value -- keeps it, changes it, compares it, hands it on (SPR_TAKE); it
   reads its bytes and keeps nothing, as an interpolation does (SPR_READ);
   it only asks whether it is nil or false, as a predicate does (SPR_TEST);
   or, for an element of the Array literal a multiple write splits (also
   through a splat, `a, b = *[s, t]`), the target the element is stored
   into takes it (SPR_STORE), where the literal is no container; or it
   asks the value's identity, as equal?, object_id and __id__ do where no
   class of the program defines them (SPR_IDENTITY). A taker needs what
   the value's facts say (share_value_needs_handle); a reader and a test
   need its bytes; a store needs what its target does
   (share_store_needs_handle). An identity asks for the object, and is the
   handle where the producer has one (for an exception's message, the box
   the exception keeps it in) and the bytes' pointer where it has only
   bytes, which are then all the String there is: never a new handle,
   which no other name holds, and never a refusal.
   Some values have a second consumer:
     - a call that answers its receiver or nil (rule 3 below) answers its
       receiver's String, so its own answer is only tested for nil
       (share_plan_second_role: SPR_TEST);
     - a write, an attribute or index assignment, an `||=` or `&&=` and a
       call answering its receiver answer a value they also store or
       change: that second consumer is the answering node's own, asked of
       that node;
     - a multiple write whose own value is read answers the values it
       stores in a new Array: a taker beside the store, which needs no more
       than the store (the literal's elements need what its facts say).

   Nothing is refused by the plan. One consumer reads it: object_id and
   __id__ on a String, through emit_str_form (codegen_str.c), the one
   place a consumer is to ask for a String in the plan's form. Every other
   emitter still decides on its own; --share-check (share_check.c) reads
   the plan at each site it marks and reports how often it agrees with the
   conversion the emitted C has there.

   repr_of is not memoised, and neither is anything that reads it. The
   need reads the share facts, final at seal, the node's type, names and
   scope's reachability; it is memoised at the end of repr_seal, in one
   byte per node beside the node's consumer role. Each guard of the memo
   covers one way codegen makes a node read as something else:
     - a view, a scope move or a splice: cplan_plain_ctx, the call plan's
       own rule; a poly dispatch arm: the arm context at rest;
     - a rename for one emission (the value-form bang transform's), a reset,
       a swap of two nodes, an added node: the node table's version, which
       exactly those bump (nt_node_set_str, nt_set_str, nt_node_reset,
       nt_swap_nodes, nt_new_node);
     - a node retyped outside the view stack: its type as at seal;
     - a method codegen makes reachable after seal (the exception message
       and to_s overrides the dispatcher calls): a node of a scope
       unreachable at seal is not memoised (SPN_ASK).
   A swap of a node's receiver, arguments or block (nt_node_set_ref) and
   an emitter's retype of a local bump nothing; the need does not read a
   local's type, and reads a call's receiver only through freshness.
   The supply reads repr_of, which an emitter's retyped local changes
   outside all of those, so it is always computed where it is asked.
   The role is the kind of the node a value sits in, as the program has
   it at seal, and needs no guard: codegen renames calls and retypes
   values, but no interpolation, predicate, multiple write or identity
   query changes where its parts sit, and no rename makes or unmakes an
   identity query; a receiver or arguments swapped in for one
   emission (nt_node_set_ref) and nodes added after seal sit in a call,
   whose role is a taker's. The second role of a call that answers its
   receiver reads its name, its receiver's type and its targets, as rule 3
   does, so it is asked where it is asked.
   --share-check asks the need live too: the memo is measured, never
   trusted, until an emitter reads it. (The one that does asks for an
   identity, whose need is the object whatever the memo says.) */

#include <stdlib.h>
#include "codegen_internal.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "repr.h"
#include "share.h"

int share_plan_form(int need, int supply) {
  if (need == SPN_BYTES) return SPF_BYTES;
  if (supply == SPS_HANDLE) return SPF_HANDLE;
  return need == SPN_NEW ? SPF_WRAP : SPF_REFUSE;
}

const char *share_plan_form_name(int form) {
  static const char *const names[] = { "bytes", "handle", "wrap", "refuse" };
  return form >= SPF_BYTES && form <= SPF_REFUSE ? names[form] : "?";
}

/* The String receiver call v answers (a String method that answers self,
   or self or nil, and no program method of the name), or -1. */
static int share_plan_self_recv(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, v, "name");
  int r = unwrap_parens(c, nt_ref(nt, v, "receiver")), tg[CPT_MAX];
  if (!nm || r < 0 || (c->ntype[r] != TY_STRING && c->ntype[r] != TY_STRBUF)) return -1;
  if (!(bop_answers_self(TY_STRING, nm, call_plain_argc(c, v), nt_ref(nt, v, "block") >= 0) &
        (BOPF_SELF | BOPF_SELF_OR_NIL)))
    return -1;
  return cplan_targets(c, v, tg, CPT_MAX) == 0 ? r : -1;
}

/* The supply node v decides by itself (rules 1, 2 and 4 below), or -1
   when it is a call answering its String receiver, whose supply is the
   receiver's: *recv is then that receiver. */
static int share_plan_supply_at(Compiler *c, int v, int *recv) {
  Repr r = repr_of(c, v);
  *recv = -1;
  if (r.kind == RK_BOXED) return 1;
  if (r.kind == RK_STRBUF) return r.strbuf_src != RS_FRESH;
  int rv = share_plan_self_recv(c, v);
  if (rv >= 0) { *recv = rv; return -1; }
  /* a new String (share_value_fresh) has no handle to hand over, whatever
     a route says about whether its copy can be seen */
  return share_node_carries_handle(c, v) && !share_value_fresh(c, v, 0);
}

/* One answer of a chain of self-answering calls: the stamp it holds for
   (the view epoch and the node table's version), the node that decided it
   and what that node's representation was. */
struct SharePlanSup { unsigned epoch, ver; int d; unsigned char kind, src, val, set; };

/* Is cache entry e of a chain still the answer: the same stamp, and the
   node that decided it deciding it the same way? The stamp does not move
   with a dispatch arm, a yield site's types, a splice, a scope move or a
   receiver swap, and the links between the asked node and the deciding
   one are not checked again: the entry rests on the deciding node's answer
   alone, which a query of every supply both ways (cached and walked) over
   the corpus found to agree. */
static int share_plan_sup_valid(Compiler *c, const struct SharePlanSup *e) {
  if (!e->set || e->epoch != view_epoch() || e->ver != c->nt->version) return 0;
  Repr r = repr_of(c, e->d);
  int rv;
  return r.kind == e->kind && r.strbuf_src == e->src && share_plan_supply_at(c, e->d, &rv) == e->val;
}

/* Can node v's producer hand over the String's handle?
     1. a box hands over itself, whatever it holds: a String boxed elsewhere
        was planned where it was boxed;
     2. a value represented as the handle has it, unless repr_of says the
        handle is a new one wrapped around its bytes where it is demanded
        (RS_FRESH);
     3. a call that answers its String receiver hands over what that
        receiver does;
     4. any other value, represented as its bytes, has one where the routes
        the analysis checks a carry against hand it along
        (share_node_carries_handle: a callee that publishes it, a bang over
        a slot that holds it, a variable whose slot holds it, a String
        Range's endpoint), unless it is a new String.
   A chain of rule 3 is asked at each of its links, outermost first: the
   walk down it keeps each link's answer with the node that decided it
   (Compiler.share_sup), and a link asked again reads it while the view
   epoch and the table's version are the same and the deciding node still
   decides it the same way, so a chain costs its length once, not once per
   link. Nothing outside the chain's own emission can change a link in
   between: an emitter retypes a block parameter's local, which no
   receiver chain outside the block reaches. */
static int share_plan_supplies(Compiler *c, int v) {
  int rv, s = share_plan_supply_at(c, v, &rv);
  if (s >= 0) return s;
  int n = c->nt->count;
  if (!c->share_sup || c->share_sup_n < n) {
    free(c->share_sup);
    c->share_sup = calloc((size_t)n + 1, sizeof *c->share_sup);
    if (!c->share_sup) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->share_sup_n = n;
  }
  struct SharePlanSup *e = &c->share_sup[v];
  if (share_plan_sup_valid(c, e)) return e->val;
  /* walk down to the deciding node, then stamp every link on the way */
  int d = rv;
  while ((s = share_plan_supply_at(c, d, &rv)) < 0) {
    if (d < n && share_plan_sup_valid(c, &c->share_sup[d])) { s = c->share_sup[d].val; d = c->share_sup[d].d; break; }
    d = rv;
  }
  Repr r = repr_of(c, d);
  struct SharePlanSup fill = { view_epoch(), c->nt->version, d, r.kind, r.strbuf_src, (unsigned char)s, 1 };
  for (int l = v; l >= 0 && l < n && l != d; l = share_plan_self_recv(c, l)) c->share_sup[l] = fill;
  return s;
}

/* The memo's byte per node: the need of the value (SPN_*), or SPN_ASK for
   a node whose scope was unreachable at seal (codegen can still make its
   method reachable, so its need is asked live), and above it, in the
   byte's other six bits, the role of the node's consumer (SPR_*). */
enum { SPN_ASK = 3, SPM_NEED = 3, SPM_ROLE_SHIFT = 2 };

static int share_plan_need_of(int need) {
  return need == SHN_NO ? SPN_BYTES : need == SHN_FRESH ? SPN_NEW : SPN_OBJECT;
}
int share_plan_need(Compiler *c, int v) { return share_plan_need_of(share_value_needs_handle(c, v)); }

/* What a consumer in `role` needs of value v, whose own need (what its
   facts say a taker needs) is `need`. */
static int share_plan_role_need(Compiler *c, int v, int role, int need) {
  if (role == SPR_READ || role == SPR_TEST) return SPN_BYTES;
  if (role == SPR_IDENTITY) return SPN_OBJECT;
  /* the store asks past the literal it splits (never less than the value) */
  if (role == SPR_STORE && need == SPN_BYTES) return share_plan_need_of(share_store_needs_handle(c, v));
  return need;
}

/* The plan of value v for a consumer in `role`, v's own need known. */
SharePlan share_plan_with_need(Compiler *c, int v, int role, int need) {
  need = share_plan_role_need(c, v, role, need);
  SharePlan p = { (unsigned char)need, SPS_BYTES, SPF_BYTES };
  if (need == SPN_BYTES) return p;
  p.supply = share_plan_supplies(c, v) ? SPS_HANDLE : SPS_BYTES;
  /* an exception's message asked its identity is the box the exception
     keeps it in (exc_message_identity_operand), which the identity reads
     where the value is read as bytes: no handle is attached to the read,
     so no other consumer has it */
  if (role == SPR_IDENTITY && p.supply == SPS_BYTES && exc_message_identity_operand(c, v)) p.supply = SPS_HANDLE;
  /* (an identity is the handle or the bytes' pointer) */
  p.form = (unsigned char)(role == SPR_IDENTITY && p.supply == SPS_BYTES ? SPF_BYTES : share_plan_form(p.need, p.supply));
  return p;
}

const char *share_plan_role_name(int role) {
  static const char *const names[] = { "take", "read", "test", "store", "identity" };
  return role >= SPR_TAKE && role <= SPR_IDENTITY ? names[role] : "?";
}

int share_plan_role(Compiler *c, int v) {
  return c->share_plan && v >= 0 && v < c->share_plan_n ? c->share_plan[v] >> SPM_ROLE_SHIFT : SPR_TAKE;
}

int share_plan_second_role(Compiler *c, int v) { return share_plan_self_recv(c, v) >= 0 ? SPR_TEST : -1; }

/* Give node n's consumer `role`, and through parentheses and a statement
   list's last statement, the node whose value n is. */
static void share_plan_role_mark(Compiler *c, int n, int role) {
  const NodeTable *nt = c->nt;
  while (n >= 0 && n < c->share_plan_n) {
    c->share_plan[n] = (unsigned char)((c->share_plan[n] & SPM_NEED) | (role << SPM_ROLE_SHIFT));
    NodeKind k = nt_kind(nt, n);
    if (k == NK_ParenthesesNode) n = nt_ref(nt, n, "body");
    else if (k == NK_StatementsNode) {
      int bn = 0;
      const int *bv = nt_arr(nt, n, "body", &bn);
      n = bn > 0 ? bv[bn - 1] : -1;
    }
    else break;
  }
}

/* The elements a multiple write's value v -- an Array literal, and the
   literals splatted in it (`a, b = *[s, t]`) -- stores into its targets. */
static void share_plan_masgn_stores(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  v = an_unparen(nt, v);
  if (v >= 0 && nt_kind(nt, v) == NK_SplatNode) v = an_unparen(nt, nt_ref(nt, v, "expression"));
  if (v < 0 || nt_kind(nt, v) != NK_ArrayNode) return;
  int en = 0;
  const int *el = nt_arr(nt, v, "elements", &en);
  for (int i = 0; i < en; i++) {
    if (nt_kind(nt, el[i]) == NK_SplatNode) share_plan_masgn_stores(c, el[i]);
    else share_plan_role_mark(c, el[i], SPR_STORE);
  }
}

/* The operands of identity query n (equal?'s receiver and argument,
   object_id's and __id__'s receiver), where no class of the program
   defines its name: a method of the program's own takes them. An arm of
   a dynamic send shares the send's receiver, which the send takes. */
static void share_plan_identity_roles(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, n, "name");
  int recv = nt_ref(nt, n, "receiver"), args = nt_ref(nt, n, "arguments"), np = 0, an = 0;
  if (!nm || recv < 0 || nt_ref(nt, n, "block") >= 0 || !is_identity_query(nm) || is_freeze_family(nm) ||
      nt_int(nt, n, "dyn_arm", 0))
    return;
  int pair = is_equality_name(nm);
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != pair || call_plain_argc(c, n) != pair) return;
  const PolyCand *pc = comp_poly_candidates(c, nm, &np);
  for (int i = 0; i < np; i++)
    if (pc[i].mi >= 0 || pc[i].rdcls >= 0) return;
  share_plan_role_mark(c, recv, SPR_IDENTITY);
  if (pair) share_plan_role_mark(c, av[0], SPR_IDENTITY);
}

/* The roles other than a taker's, from the consumers that have them: an
   interpolation reads, a predicate tests, a multiple write stores each
   element of its value's literal (and of a literal splatted in it) into
   its target, an identity query asks its operands' identity. One pass over
   each kind. */
static void share_plan_roles_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_EmbeddedStatementsNode, e) share_plan_role_mark(c, nt_ref(nt, e, "statements"), SPR_READ);
  static const NodeKind preds[] = { NK_IfNode, NK_UnlessNode, NK_WhileNode, NK_UntilNode };
  for (int k = 0; k < 4; k++)
    NT_FOREACH_KIND(nt, preds[k], n) share_plan_role_mark(c, nt_ref(nt, n, "predicate"), SPR_TEST);
  NT_FOREACH_KIND(nt, NK_MultiWriteNode, mw) share_plan_masgn_stores(c, nt_ref(nt, mw, "value"));
  NT_FOREACH_KIND(nt, NK_CallNode, n) share_plan_identity_roles(c, n);
}

int share_plan_box_plain(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  int n = c->share_parent_n, p = c->share_parent && v >= 0 && v < n ? c->share_parent[v] : -1;
  while (p >= 0 && p < n && nt_kind(nt, p) == NK_ParenthesesNode) p = c->share_parent[p];
  if (p >= 0 && p < n && nt_kind(nt, p) == NK_AssocNode) p = c->share_parent[p];
  if (p < 0 || p >= n || (nt_kind(nt, p) != NK_ArrayNode && nt_kind(nt, p) != NK_HashNode)) return 0;
  /* (a temporary literal is not: the walk joins an element read off it,
     `[s, 1].first`, to no class of the literal's) */
  return share_node_anchored(c, p) && share_node_elems_local(c, p);
}

void share_plan_free(Compiler *c) {
  free(c->share_parent);
  c->share_parent = NULL;
  c->share_parent_n = 0;
  free(c->share_plan);
  free(c->share_plan_ty);
  free(c->share_sup);
  c->share_plan = NULL;
  c->share_plan_ty = NULL;
  c->share_sup = NULL;
  c->share_plan_n = c->share_sup_n = 0;
}

/* Once per compile, over the node table: each node's need, with the type
   and the table version it was asked under, and its consumer's role. */
void share_plan_build(Compiler *c) {
  share_plan_free(c);
  if (!repr_share_rule(c) || !c->share) return;
  int n = c->nt->count;
  c->share_plan = malloc((size_t)n + 1);
  c->share_plan_ty = malloc(sizeof *c->share_plan_ty * ((size_t)n + 1));
  if (!c->share_plan || !c->share_plan_ty) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int v = 0; v < n; v++) {
    Scope *sc = comp_scope_of(c, v);
    c->share_plan[v] = sc && !sc->reachable ? SPN_ASK : (unsigned char)share_plan_need(c, v);
    c->share_plan_ty[v] = c->ntype[v];
  }
  c->share_plan_n = n;
  c->share_plan_ver = c->nt->version;
  share_plan_roles_build(c);
  c->share_parent = an_parent_map(c->nt);
  if (!c->share_parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->share_parent_n = n;
}

/* Is codegen where the memo may answer: the node read as itself (no view,
   no scope move, no splice) and no poly dispatch arm open? */
static int share_plan_plain_ctx(void) {
  return cplan_plain_ctx() && g_arm.pd_skip < 0 && g_arm.prbd_skip < 0 && !g_arm.builtin_arm &&
         g_arm.send_split < 0;
}

int share_plan_memo_need(Compiler *c, int v) {
  int m = c->share_plan && v >= 0 && v < c->share_plan_n && (c->share_plan[v] & SPM_NEED) != SPN_ASK &&
          c->nt->version == c->share_plan_ver && c->ntype[v] == c->share_plan_ty[v] && share_plan_plain_ctx();
  return m ? c->share_plan[v] & SPM_NEED : -1;
}

SharePlan share_plan_of(Compiler *c, int v, int role, int *memo) {
  int m = share_plan_memo_need(c, v);
  if (memo) *memo = m >= 0;
  return share_plan_with_need(c, v, role, m >= 0 ? m : share_plan_need(c, v));
}
