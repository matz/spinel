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
   cache only at view depth 0, or key it on view_epoch().

   view_push_repr does the same for one of the representation flags beside
   the type (repr.h): a String-handle mark or demand, a poly-to-handle lift,
   a read's nil narrowing, a receiver whose nil a call's nil arm tested. The emitters that re-enter with a flag lifted or
   forced push it as a view, so a refusal's view_unwind puts it back with
   the rest. repr_of reads the flags live and memoizes nothing.

   view_bind binds a node to the text emit_expr writes for it instead (a
   hoisted argument's or receiver's temp, g_argov_*): the bindings are a
   stack beside the views, popped by view_unbind, and a refusal's
   view_unwind drops the ones bound since its mark with the rest.

   view_push_face pins a node to one face kind (the face table, types.h)
   for the inference asked under it: face_of (analyze_infer.c) reads the
   innermost pin, here or inference's own. Like the arm context it is no
   view of a node's cached type.

   view_push_stmt marks the statement whose prelude (g_pre) is open, with
   that prelude's buffer, for the emission of the statement
   (emit_with_prelude); view_stmt_top reads the innermost one. Like a face it
   is no view of a node's cached type.

   view_push_splice marks a spliced method's parameter bound as an alias
   of the caller's variable (inline_alias_params) that the call's block
   assigns: the variable's argument node, the splice's tag and the
   parameter's name. view_push_fn fences a C function emitted while such
   splices are open (a proc's or a fiber's body), whose code cannot name
   the splice's slots: view_splice_next walks the open splice entries from
   the innermost down to the innermost fence. Neither views a node.

   view_push_arm does the same for the arm context a poly dispatch's
   builtin arm re-enters the call under (g_arm: the node whose dispatch
   declines its own re-entry, g_pd_skip and g_prbd_skip, and
   g_poly_builtin_arm, under which no user class owns a name). It is no
   view of a node: it counts toward neither view_depth nor view_epoch. */

#include "codegen_internal.h"
#include "share.h"
#include <stdarg.h>

#define VIEW_MAX 256

/* what an entry overrides: the node's type, one representation flag, or
   the arm context */
enum { VK_TYPE = -1, VK_ARM = -2, VK_FACE = -3, VK_STMT = -4, VK_SPLICE = -5, VK_FN = -6 };
static struct { Compiler *c; int id; int kind; int saved; ArmCtx arm_saved; const void *pre; } view_stack[VIEW_MAX];
static int view_face = -1;   /* the innermost face entry, or -1 */
static int view_stmt = -1;   /* the innermost statement entry, or -1 */
static int view_sp;
static int view_nodes;   /* the entries that view a node (all but VK_ARM) */
static unsigned view_epoch_n;

ArmCtx g_arm = { -1, -1, 0, -1 };

/* Argument-hoist overrides: emit_args_filled pre-evaluates GC-hazardous
   call arguments into rooted temps; emit_expr then substitutes the temp
   name when it reaches the overridden node. Twice MAX_ARG_OVERRIDE to start
   with, so only a call of more arguments than that grows it. */
static int  argov_node0[2 * MAX_ARG_OVERRIDE];
static char argov_text0[2 * MAX_ARG_OVERRIDE][ARGOV_TEXT_LEN];
static unsigned char argov_oint0[2 * MAX_ARG_OVERRIDE];
int  *g_argov_node = argov_node0;
char (*g_argov_text)[ARGOV_TEXT_LEN] = argov_text0;
unsigned char *g_argov_oint = argov_oint0;   /* the bound text is an sp_oint / sp_ofloat (view_bind_o) */
static int g_argov_cap = 2 * MAX_ARG_OVERRIDE;
int  g_n_argov = 0;
/* See codegen_internal.h. */
void argov_reserve(void) {
  if (g_n_argov + 1 + MAX_ARG_OVERRIDE <= g_argov_cap) return;
  int cap = 2 * (g_n_argov + 1 + MAX_ARG_OVERRIDE);
  int *nodes = malloc(sizeof *nodes * (size_t)cap);
  char (*texts)[ARGOV_TEXT_LEN] = malloc(sizeof *texts * (size_t)cap);
  if (!nodes || !texts) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(nodes, g_argov_node, sizeof *nodes * (size_t)g_n_argov);
  memcpy(texts, g_argov_text, sizeof *texts * (size_t)g_n_argov);
  unsigned char *oints = calloc((size_t)cap, 1);
  if (!oints) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(oints, g_argov_oint, (size_t)g_n_argov);
  if (g_argov_node != argov_node0) { free(g_argov_node); free(g_argov_text); free(g_argov_oint); }
  g_argov_node = nodes; g_argov_text = texts; g_argov_oint = oints; g_argov_cap = cap;
}

int view_bind(int node, const char *fmt, ...) {
  if (g_n_argov + 1 > g_argov_cap) argov_reserve();
  int slot = g_n_argov++;
  g_argov_node[slot] = node;
  g_argov_oint[slot] = 0;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(g_argov_text[slot], sizeof g_argov_text[0], fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof g_argov_text[0]) {
    fprintf(stderr, "spinel: internal error: a bound node's text is %d bytes, over the %d a slot holds (ARGOV_TEXT_LEN): %.40s...\n",
            n, ARGOV_TEXT_LEN - 1, g_argov_text[slot]);
    abort();
  }
  return slot;
}
/* As view_bind, for a temp holding an Integer / Float with its nil beside
   the value: the node reads as that oint (the dispatcher wraps it as any
   oint producer). */
int view_bind_o(int node, const char *fmt, ...) {
  if (g_n_argov + 1 > g_argov_cap) argov_reserve();
  int slot = g_n_argov++;
  g_argov_node[slot] = node;
  g_argov_oint[slot] = 1;
  va_list ap; va_start(ap, fmt);
  int n;
  if (g_share_check) {
    /* a bound text is what the emitters compare and print: --share-check's
       marks in it would be neither, and could outgrow the slot */
    char text[8 * ARGOV_TEXT_LEN];
    n = vsnprintf(text, sizeof text, fmt, ap);
    if (n >= 0 && (size_t)n < sizeof text) n = (int)share_check_strip(text, (size_t)n);
    if (n >= 0 && (size_t)n < sizeof g_argov_text[0]) memcpy(g_argov_text[slot], text, (size_t)n + 1);
    else snprintf(g_argov_text[slot], sizeof g_argov_text[0], "%s", text);
  }
  else n = vsnprintf(g_argov_text[slot], sizeof g_argov_text[0], fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof g_argov_text[0]) {
    fprintf(stderr, "spinel: internal error: a bound node's text is %d bytes, over the %d a slot holds (ARGOV_TEXT_LEN): %.40s...\n",
            n, ARGOV_TEXT_LEN - 1, g_argov_text[slot]);
    abort();
  }
  return slot;
}

void view_unbind(int n) { g_n_argov = n; }

unsigned view_epoch(void) { return view_epoch_n; }

/* the slot an entry of `kind` names for node `id` */
static int view_read(Compiler *c, int kind, int id) {
  switch (kind) {
  case VK_TYPE:          return (int)c->ntype[id];
  case VR_STRBUF_BOX:    return c->strbuf_box[id];
  case VR_HANDLE_DEMAND: return c->strbuf_handle_demand[id];
  case VR_POLY_LIFT:     return c->poly_strbuf_lift[id];
  case VR_NIL_TESTED:    return c->nil_tested[id];
  case VR_HEAD_HELD:     return c->head_held[id];
  default:               return (int)c->nilnarrow[id];
  }
}
static void view_write(Compiler *c, int kind, int id, int v) {
  switch (kind) {
  case VK_TYPE:          c->ntype[id] = (TyKind)v; break;
  case VR_STRBUF_BOX:    c->strbuf_box[id] = (unsigned char)v; break;
  case VR_HANDLE_DEMAND: c->strbuf_handle_demand[id] = (unsigned char)v; break;
  case VR_POLY_LIFT:     c->poly_strbuf_lift[id] = (unsigned char)v; break;
  case VR_NIL_TESTED:    c->nil_tested[id] = (unsigned char)v; break;
  case VR_HEAD_HELD:     c->head_held[id] = (unsigned char)v; break;
  default:               c->nilnarrow[id] = (TyKind)v; break;
  }
}

static int view_open(Compiler *c, int id, int kind, int v) {
  if (view_sp >= VIEW_MAX) {
    fprintf(stderr, "spinel: internal error: codegen views nested too deep\n");
    exit(1);
  }
  int tok = view_sp++;
  view_stack[tok].c = c;
  view_stack[tok].id = id;
  view_stack[tok].kind = kind;
  view_stack[tok].saved = view_read(c, kind, id);
  view_write(c, kind, id, v);
  view_nodes++;
  view_epoch_n++;
  return tok;
}

int view_push_arm(int pd_skip, int prbd_skip, int builtin_arm) {
  if (view_sp >= VIEW_MAX) {
    fprintf(stderr, "spinel: internal error: codegen views nested too deep\n");
    exit(1);
  }
  int tok = view_sp++;
  view_stack[tok].c = NULL;
  view_stack[tok].id = -1;
  view_stack[tok].kind = VK_ARM;
  view_stack[tok].arm_saved = g_arm;
  g_arm.pd_skip = pd_skip;
  g_arm.prbd_skip = prbd_skip;
  g_arm.builtin_arm = builtin_arm;
  return tok;
}

int view_push_face(int node, TyKind kind) {
  if (view_sp >= VIEW_MAX) {
    fprintf(stderr, "spinel: internal error: codegen views nested too deep\n");
    exit(1);
  }
  int tok = view_sp++;
  view_stack[tok].c = NULL;
  view_stack[tok].id = node;
  view_stack[tok].kind = VK_FACE;
  view_stack[tok].saved = (int)kind;
  view_stack[tok].arm_saved.pd_skip = view_face;   /* the face it hides */
  view_face = tok;
  return tok;
}

int view_face_top(int *node, TyKind *kind) {
  if (view_face < 0) return 0;
  *node = view_stack[view_face].id;
  *kind = (TyKind)view_stack[view_face].saved;
  return 1;
}

/* A statement's entry. Past the stack's last few slots the statement goes
   unmarked (-1, no entry): view_stmt_top then names an enclosing statement
   whose prelude is not the open one, which its caller reads as "unknown". */
int view_push_stmt(int node, const void *pre) {
  if (view_sp >= VIEW_MAX - 8) return -1;
  int tok = view_sp++;
  view_stack[tok].c = NULL;
  view_stack[tok].id = node;
  view_stack[tok].kind = VK_STMT;
  view_stack[tok].saved = view_stmt;   /* the statement it hides */
  view_stack[tok].pre = pre;
  view_stmt = tok;
  return tok;
}

int view_stmt_top(int *node, const void **pre) {
  if (view_stmt < 0) return 0;
  *node = view_stack[view_stmt].id;
  *pre = view_stack[view_stmt].pre;
  return 1;
}

static int view_open_mark(int kind, int node, int v, const void *p) {
  if (view_sp >= VIEW_MAX) {
    fprintf(stderr, "spinel: internal error: codegen views nested too deep\n");
    exit(1);
  }
  int tok = view_sp++;
  view_stack[tok].c = NULL;
  view_stack[tok].id = node;
  view_stack[tok].kind = kind;
  view_stack[tok].saved = v;
  view_stack[tok].pre = p;
  return tok;
}
int view_push_splice(int arg, int tag, const char *pname) { return view_open_mark(VK_SPLICE, arg, tag, pname); }
int view_push_fn(void) { return view_open_mark(VK_FN, -1, 0, NULL); }

int view_splice_next(int from, int *arg, int *tag, const char **pname) {
  for (int i = (from < 0 || from > view_sp ? view_sp : from) - 1; i >= 0; i--) {
    if (view_stack[i].kind == VK_FN) return -1;
    if (view_stack[i].kind != VK_SPLICE) continue;
    *arg = view_stack[i].id;
    *tag = view_stack[i].saved;
    *pname = (const char *)view_stack[i].pre;
    return i;
  }
  return -1;
}

/* the entry on top, put back */
static void view_close(int tok) {
  if (view_stack[tok].kind == VK_SPLICE || view_stack[tok].kind == VK_FN) return;
  if (view_stack[tok].kind == VK_ARM) { g_arm = view_stack[tok].arm_saved; return; }
  if (view_stack[tok].kind == VK_FACE) { view_face = view_stack[tok].arm_saved.pd_skip; return; }
  if (view_stack[tok].kind == VK_STMT) { view_stmt = view_stack[tok].saved; return; }
  view_write(view_stack[tok].c, view_stack[tok].kind, view_stack[tok].id, view_stack[tok].saved);
  view_nodes--;
  view_epoch_n++;
}

int view_push(Compiler *c, int id, TyKind t) { return view_open(c, id, VK_TYPE, (int)t); }

int view_push_repr(Compiler *c, int id, int flag, int v) { return view_open(c, id, flag, v); }

void view_pop(Compiler *c, int tok) {
  if (tok != view_sp - 1) {
    fprintf(stderr, "spinel: internal error: codegen view popped out of order\n");
    exit(1);
  }
  (void)c;
  view_sp--;
  view_close(tok);
}

int view_depth(void) { return view_nodes; }

/* the stack's position and the bindings' fill, for view_unwind: every
   entry opened since, the arm context's too, and every binding */
int view_mark(void) { return view_sp << 16 | (g_n_argov & 0xffff); }

/* A refusal longjmps out of an emission past its view_pop. The recovery
   point saved the depth before it and puts back every view opened since,
   the latest first, so a dropped arm leaves no node seen as another kind. */
void view_unwind(int mark) {
  int depth = mark >> 16;
  while (view_sp > depth) {
    view_sp--;
    view_close(view_sp);
  }
  if (g_n_argov > (mark & 0xffff)) g_n_argov = mark & 0xffff;
}
