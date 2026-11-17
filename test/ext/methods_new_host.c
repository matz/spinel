/* A pure-C host over an --ext-class emission of classes: the rows of `new`
   in the table of each class, a call of `new` on the class value with each
   count of arguments, and the classes with no row of the initialize. */
#include <stdio.h>
#include <string.h>
#include "n.h"

/* The block of the host for an initialize that yields. */
static sp_int add_ten(void *cap, sp_int argc, sp_int *args) {
  (void)cap, (void)args;
  sp_RbVal v = argc > 0 ? sp_proc_arg(0) : sp_box_nil();
  sp_proc_ret_set(sp_box_int(sp_poly_arg_int_chk(v) + 10));
  return 0;
}

/* the rows of `new` of the class value k: the arity, and a `&` on a row
   that takes the block */
static void news(const char *what, sp_RbVal k) {
  const sp_ext_method *m = sp_ext_methods((int)k.cls_id);
  printf("%s:", what);
  for (; m && m->name; m++)
    if (m->singleton && !strcmp(m->name, "new")) printf(" new/%d%s", m->argc, m->block ? "&" : "");
  puts("");
}

/* k.new(argv), then the reader `get` of the new object */
typedef struct { sp_RbVal k; int argc; const sp_RbVal *argv; sp_RbVal blk; int with_blk; const char *get; } make_t;
static void make_body(void *p) {
  make_t *t = (make_t *)p;
  sp_RbVal o, v;
  int found = t->with_blk ? sp_ext_send_block(t->k, "new", t->argc, t->argv, t->blk, &o)
                          : sp_ext_send(t->k, "new", t->argc, t->argv, &o);
  if (!found) { puts("no method new"); return; }
  SP_GC_ROOT_RBVAL(o);
  if (o.cls_id != t->k.cls_id) { puts("an object of another class"); return; }
  if (sp_ext_send(o, t->get, 0, NULL, &v)) printf("%s -> %s\n", t->get, sp_poly_inspect(v));
  else printf("no method %s\n", t->get);
}
static void make(sp_RbVal k, int argc, const sp_RbVal *argv, const char *get) {
  make_t t = { k, argc, argv, sp_box_nil(), 0, get };
  const char *cls = 0, *msg = 0;
  if (Init_ext_methods_new_try(make_body, &t, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
}
static void make_blk(sp_RbVal k, int argc, const sp_RbVal *argv, sp_RbVal blk, const char *get) {
  make_t t = { k, argc, argv, blk, 1, get };
  const char *cls = 0, *msg = 0;
  if (Init_ext_methods_new_try(make_body, &t, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
}

int main(void) {
  Init_ext_methods_new();
  sp_PolyArray *all = sp_ExtNew_s_sample();
  SP_GC_ROOT(all);
  enum { NSAMPLE = 21 };
  sp_RbVal k[NSAMPLE];
  for (int i = 0; i < NSAMPLE; i++) k[i] = sp_box_class((sp_Class){ all->data[i].cls_id, 0 });
  sp_RbVal lazy = sp_box_class(sp_ExtNew_s_lazy()), oops = sp_box_class(sp_ExtNew_s_oops());
  sp_RbVal a[4] = { sp_box_int(5), sp_box_int(6), sp_box_int(7), sp_box_int(8) };
  sp_RbVal s[1] = { sp_box_str("host") };
  SP_GC_ROOT_RBVAL(s[0]);
  sp_RbVal b = sp_box_proc(sp_proc_new((void *)add_ten, NULL, NULL));
  SP_GC_ROOT_RBVAL(b);

  /* fixed parameters: one row, and the ArgumentError of another count */
  news("Point", k[0]);
  make(k[0], 2, a, "sum");
  make(k[0], 1, a, "sum");
  /* optional and rest parameters: a row for each count */
  news("Opt", k[1]);
  make(k[1], 1, a, "n");
  make(k[1], 2, a, "n");
  make(k[1], 4, a, "n");
  make(k[1], 0, a, "n");
  /* no initialize: no argument */
  news("Bare", k[2]);
  make(k[2], 0, NULL, "hello");
  make(k[2], 1, a, "hello");
  /* the initialize of the class above: its rows, and an object of Kid */
  news("Kid", k[3]);
  make(k[3], 2, a, "kid");
  /* an initialize of its own under Point: its row only, and the row of
     Point takes its two arguments (above) */
  news("Own", k[4]);
  make(k[4], 1, s, "s");
  make(k[4], 2, a, "s");
  /* a class written by its path, with the initialize of Point, and a
     class under that class */
  news("Deep", k[8]);
  make(k[8], 2, a, "deep");
  news("Far", k[18]);
  make(k[18], 2, a, "far");
  /* a class under a class named from the root, with a class of that name
     in the module: the initialize of the class at the root */
  news("FromRoot", k[20]);
  make(k[20], 1, a, "twice");
  /* a `class << X` of another object in the body: the row of the
     initialize stays */
  news("Shy", k[19]);
  make(k[19], 1, a, "x");
  /* an initialize that yields: with the block of the host, and with none */
  news("Blk", k[5]);
  make_blk(k[5], 0, NULL, b, "v");
  make_blk(k[5], 1, a, b, "v");
  make(k[5], 0, NULL, "v");
  /* a second Point in another namespace, and the class under it: the
     row of that Point */
  news("Inner::Point", k[9]);
  make(k[9], 1, a, "z");
  news("Inner::Kid", k[10]);
  make(k[10], 1, a, "z");
  /* a class the program never makes */
  news("Lazy", lazy);
  make(lazy, 1, s, "name");
  /* a class opened again by its path, from inside another module */
  news("Home::Room", k[17]);
  make(k[17], 1, a, "more");
  /* an initialize under a visibility call */
  news("Priv", k[13]);
  make(k[13], 1, a, "x");
  /* no row: a class under StandardError, a keyword parameter, the
     initialize of a module, a constant of the class's own name */
  news("Oops", oops);
  news("Kw", k[11]);
  news("Mixed", k[12]);
  news("Same", k[14]);
  /* a private new: no row */
  news("Single", k[6]);
  make(k[6], 0, NULL, "tag");
  /* a self.new of the class: the rows of that method */
  news("Custom", k[7]);
  make(k[7], 0, NULL, "v");
  make(k[7], 1, a, "v");
  /* a class under it with an initialize of its own: the rows of that
     self.new, and an object of the class */
  news("Under", k[15]);
  make(k[15], 1, a, "v");
  /* a new in `class << self`: its row, not one of the initialize */
  news("Meta", k[16]);
  make(k[16], 2, a, "v");
  return 0;
}
