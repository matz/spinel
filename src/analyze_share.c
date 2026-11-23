/* analyze_share.c -- the questions the String rule asks of the program
   (#6765): whether a slot's type can hold a String (sh_may_hold), whether
   a call stores the block it is given (share_call_stores_block) and whether
   a block hands control back to its caller (share_block_returns). See
   share.h. */

#include <stdlib.h>
#include <string.h>
#include "analyze_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "method_table_check.h"
#include "share.h"
#include "repr.h"
#include "codegen_internal.h"
#include "value_pos.h"

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
  case TY_METHOD: case TY_IO: case TY_ARGF: case TY_CLASS: case TY_TIME_RANGE: case TY_OBJ_RANGE:
    return 0;
  default:
    return 1;
  }
}

int share_call_stores_block(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, n, "block");
  const char *nm = nt_str(nt, n, "name");
  if (!nm || blk < 0 || nt_kind(nt, blk) != NK_BlockNode || cplan_has_targets(c, n)) return 0;
  int recv = nt_ref(nt, n, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    const char *cn = key_name(c, recv);
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

/* The share rule is about to run on the settled types: the call targets kept
   from earlier rounds are asked again (cplan_targets_drop), what inference
   reads under the rule is new (comp_touch_global), and the readers that ask
   whether the rule ran are told (c->share_applied). */
void share_rule_begin(Compiler *c) {
  cplan_targets_drop();
  comp_touch_global();
  c->share_applied = 1;
}

/* Does the subtree at n hold a `next` that leaves it (not one in a nested
   block, lambda, method or loop)? */
static int sh_has_next(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_NextNode) return 1;
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode || k == NK_WhileNode || k == NK_UntilNode ||
      k == NK_ForNode)
    return 0;
  for (int i = 0; i < nt_num_refs(nt, n); i++) if (sh_has_next(nt, nt_ref_at(nt, n, i))) return 1;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (sh_has_next(nt, ids[j])) return 1;
  }
  return 0;
}
/* share.h */
int share_block_returns(const Compiler *c, int blk) {
  const NodeTable *nt = c->nt;
  int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1, n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  return n == 0 || nt_kind(nt, st[n - 1]) != NK_BreakNode || sh_has_next(nt, body);
}
