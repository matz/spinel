/* order_plan.c -- the operand-order record (#11506). See order_plan.h. */
#include "codegen_internal.h"
#include "analyze.h"
#include "order_plan.h"

/* See order_plan.h. Operands that all answer yes need no ordering and no
   roots: nothing can run between them to collect a value or to move what
   another one reads. A node bound to a temp its value already ran into is
   that temp, whatever its shape, where the walk asks (w->ran). */
int order_pure_walk(Compiler *c, int id, const OrderWalk *w) {
  const NodeTable *nt = c->nt;
  if (id < 0 || (w->ran && w->ran(id))) return 1;
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
      int typed_index = 0;
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      int a = nt_ref(nt, id, "arguments"); int ac = 0;
      const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
      if (nm && is_aref_name(nm) && recv >= 0 && ac == 1 && nt_ref(nt, id, "block") < 0 &&
          ty_is_array(comp_ntype(c, recv)) && comp_ntype(c, av[0]) == TY_INT)
        typed_index = 1;
      if (!typed_index && !call_is_scalar_op(c, id) &&
          !(call_is_field_read(c, id, &alloc) && !(w->alloc && alloc)))
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
