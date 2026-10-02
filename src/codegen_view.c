/* codegen_view.c -- a node seen as another kind for one nested emission.

   Codegen re-enters an emitter with a node's cached type overridden: a
   Range receiver materialized to an IntArray temp, a poly receiver inside
   one dispatch arm, a call typed boxed for the arm that asks so. The
   override holds for that emission only and the node's own type comes
   back after it.

   view_push / view_pop make each such episode one bracketed pair on one
   stack, so the overrides are visible in one place (#7100). While the
   stack is not empty, what codegen reads for those nodes is a view, not
   the analysis's answer: anything that caches a decision per node must
   cache only at view depth 0. */

#include "codegen_internal.h"

#define VIEW_MAX 256

static struct { Compiler *c; int id; TyKind saved; } view_stack[VIEW_MAX];
static int view_sp;

int view_push(Compiler *c, int id, TyKind t) {
  if (view_sp >= VIEW_MAX) {
    fprintf(stderr, "spinel: internal error: codegen views nested too deep\n");
    exit(1);
  }
  int tok = view_sp++;
  view_stack[tok].c = c;
  view_stack[tok].id = id;
  view_stack[tok].saved = c->ntype[id];
  c->ntype[id] = t;
  return tok;
}

void view_pop(Compiler *c, int tok) {
  if (tok != view_sp - 1) {
    fprintf(stderr, "spinel: internal error: codegen view popped out of order\n");
    exit(1);
  }
  view_sp--;
  c->ntype[view_stack[tok].id] = view_stack[tok].saved;
}

int view_depth(void) { return view_sp; }

/* A refusal longjmps out of an emission past its view_pop. The recovery
   point saved the depth before it and puts back every view opened since,
   the latest first, so a dropped arm leaves no node seen as another kind. */
void view_unwind(int depth) {
  while (view_sp > depth) {
    view_sp--;
    view_stack[view_sp].c->ntype[view_stack[view_sp].id] = view_stack[view_sp].saved;
  }
}
