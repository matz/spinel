/* A pure-C host over an --ext-class emission of methods that take a block:
   the rows of the table, a call with a Proc of the host as the block, and
   the calls that take no block. */
#include <stdio.h>
#include <string.h>
#include "b.h"

/* The block of the host: a Proc of the runtime whose fn is a C function.
   The runtime calls fn(cap, argc, args); fn reads its boxed argument k with
   sp_proc_arg(k) and answers with sp_proc_ret_set(v). cap counts the
   calls. */
static sp_int add_ten(void *cap, sp_int argc, sp_int *args) {
  (void)args;
  ++*(int *)cap;
  sp_RbVal v = argc > 0 ? sp_proc_arg(0) : sp_box_nil();
  sp_proc_ret_set(sp_box_int(sp_poly_arg_int_chk(v) + 10));
  return 0;
}

static void call(sp_RbVal recv, const char *name, int argc, const sp_RbVal *argv, sp_RbVal blk) {
  sp_RbVal out;
  if (sp_ext_send_block(recv, name, argc, argv, blk, &out)) printf("%s -> %s\n", name, sp_poly_inspect(out));
  else printf("no method %s\n", name);
}

typedef struct { sp_RbVal recv; const char *name; sp_RbVal blk; int with_blk; } call_t;
static void call_body(void *p) {
  call_t *c = (call_t *)p;
  sp_RbVal out;
  if (c->with_blk) call(c->recv, c->name, 0, NULL, c->blk);
  else if (sp_ext_send(c->recv, c->name, 0, NULL, &out)) printf("%s -> %s\n", c->name, sp_poly_inspect(out));
}
static void try_call(sp_RbVal recv, const char *name, sp_RbVal blk, int with_blk) {
  call_t ct = { recv, name, blk, with_blk };
  const char *cls = 0, *msg = 0;
  if (Init_ext_methods_blk_try(call_body, &ct, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
}

/* the rows of the table of recv's class: the name, the arity, a `.` on a
   singleton method and a `&` on a row that takes the block */
static void list(sp_RbVal recv) {
  const sp_ext_method *m = sp_ext_methods((int)recv.cls_id);
  if (!m) { puts("no table"); return; }
  for (int i = 0; m->name; m++, i++) {
    if (!strncmp(m->name, "__", 2)) { i--; continue; }   /* a helper of the compiler */
    printf("%s%s%s/%d%s", i ? " " : "", m->singleton ? "." : "", m->name, m->argc, m->block ? "&" : "");
  }
  puts("");
}

int main(void) {
  Init_ext_methods_blk();
  sp_RbVal r = sp_ExtBlk_s_make();
  SP_GC_ROOT_RBVAL(r);
  int calls = 0;
  sp_RbVal b = sp_box_proc(sp_proc_new((void *)add_ten, &calls, NULL));
  SP_GC_ROOT_RBVAL(b);
  sp_RbVal a[3] = { sp_box_int(5), sp_box_int(6), sp_box_int(7) };
  list(r);
  /* a method that yields, inherited, and one with a block parameter */
  sp_RbVal out;
  sp_ext_send_block(r, "each", 0, NULL, b, &out);
  printf("each ran the block %d times\n", calls);
  call(r, "map", 0, NULL, b);
  /* the default, and the optional argument and the rest before the block */
  call(r, "pick", 0, NULL, b);
  call(r, "pick", 3, a, b);
  /* with the block, with none, and with nil as the block */
  call(r, "maybe", 0, NULL, b);
  try_call(r, "maybe", b, 0);
  call(r, "maybe", 0, NULL, sp_box_nil());
  /* a super hands the block on; the method of the ancestor yields */
  call(r, "up", 1, a, b);
  /* a super whose method takes no block, and a def inside the body that
     yields: one row each, and the call goes on without the block */
  call(r, "plain", 0, NULL, b);
  call(r, "nest", 0, NULL, b);
  /* a singleton method, on the class value */
  sp_RbVal k = sp_box_class((sp_Class){ r.cls_id, 0 });
  call(k, "twice", 1, a, b);
  /* a method with no block goes on without it */
  call(r, "size", 0, NULL, b);
  /* a method that yields, with no block */
  try_call(r, "each", b, 0);
  return 0;
}
