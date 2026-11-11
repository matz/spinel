/* A pure-C host over an --ext-class emission: objects the entries answer,
   their methods called by name through sp_ext_send, the table of a class
   read through sp_ext_methods, and the errors of a wrong call. */
#include <stdio.h>
#include <string.h>
#include "m.h"

static sp_RbVal str(const char *s) { return sp_box_str_new(sp_str_from_bytes(s, (sp_int)strlen(s))); }

static void show(sp_RbVal v) { printf("%s\n", sp_poly_inspect(v)); }

/* a call by name, its result shown, or "no method" */
static void call(sp_RbVal recv, const char *name, int argc, const sp_RbVal *argv) {
  sp_RbVal out;
  if (sp_ext_send(recv, name, argc, argv, &out)) show(out);
  else printf("no method %s\n", name);
}

typedef struct { sp_RbVal recv; const char *name; int argc; const sp_RbVal *argv; } call_t;
static void call_body(void *p) {
  call_t *c = (call_t *)p;
  call(c->recv, c->name, c->argc, c->argv);
}

/* the names of the table of recv's class, each with its arity, a `.` on a
   singleton method, and `^` when an ancestor defines it */
static void list(sp_RbVal recv) {
  const sp_ext_method *m = sp_ext_methods((int)recv.cls_id);
  if (!m) { puts("no table"); return; }
  for (int i = 0; m->name; m++, i++)
    printf("%s%s%s/%d%s", i ? " " : "", m->singleton ? "." : "", m->name, m->argc,
           m->owner == (int)recv.cls_id ? "" : "^");
  puts("");
}

int main(void) {
  Init_ext_methods();
  sp_RbVal c = sp_ExtMethods_s_make(SP_STR_HDL(sp_str_from_bytes("c", 1)));
  SP_GC_ROOT_RBVAL(c);
  sp_RbVal one = sp_box_int(1), colon = str(":");
  call(c, "bump", 1, &one);
  call(c, "bump", 1, &one);
  call(c, "label", 1, &colon);
  call(c, "half", 0, NULL);
  call(c, "n", 0, NULL);
  call(c, "name", 0, NULL);
  sp_RbVal tag = str("t");
  call(c, "tag=", 1, &tag);
  call(c, "tag", 0, NULL);
  sp_RbVal t;
  if (sp_ext_send(c, "twin", 0, NULL, &t)) { SP_GC_ROOT_RBVAL(t); call(t, "name", 0, NULL); }
  call(c, "secret", 0, NULL);
  call(c, "nothing", 0, NULL);
  list(c);
  sp_RbVal l = sp_ExtMethods_s_loud(SP_STR_HDL(sp_str_from_bytes("l", 1)));
  SP_GC_ROOT_RBVAL(l);
  call(l, "bump", 1, &one);
  call(l, "shout", 0, NULL);
  call(l, "half", 0, NULL);
  list(l);
  /* a method declared to answer a Counter that answers a Loud: the box
     carries the Loud's class, so its methods and table answer */
  sp_RbVal yes = sp_box_bool(1), pk;
  if (sp_ext_send(c, "pick", 1, &yes, &pk)) { SP_GC_ROOT_RBVAL(pk); call(pk, "shout", 0, NULL); list(pk); }
  /* a class that only an exported method makes an instance of */
  sp_RbVal sp;
  if (sp_ext_send(c, "spare", 0, NULL, &sp)) { SP_GC_ROOT_RBVAL(sp); call(sp, "hi", 0, NULL); }
  /* a method that changes a String parameter in place changes the host's
     String, which is the handle it appends to */
  sp_RbVal bang = str("s");
  SP_GC_ROOT_RBVAL(bang);
  call(c, "stamp", 1, &bang);
  show(bang);
  /* a singleton method, called on the class value and on the subclass's;
     an instance method is not the class's, nor a singleton method the
     instance's */
  sp_RbVal kc = sp_box_class((sp_Class){ c.cls_id, 0 }), kl = sp_box_class((sp_Class){ l.cls_id, 0 });
  sp_RbVal z;
  if (sp_ext_send(kc, "zero", 0, NULL, &z)) { SP_GC_ROOT_RBVAL(z); call(z, "name", 0, NULL); }
  if (sp_ext_send(kl, "zero", 0, NULL, &z)) { SP_GC_ROOT_RBVAL(z); call(z, "label", 1, &colon); }
  call(kc, "bump", 1, &one);
  call(c, "zero", 0, NULL);
  /* a singleton method over a class instance variable that only the host
     calls: the class's own row runs, and each class counts in its own
     variable */
  call(kc, "tally", 0, NULL);
  call(kl, "tally", 0, NULL);
  call(kc, "tally", 0, NULL);
  /* a define_method in a singleton method that only the host names */
  sp_RbVal dn;
  if (sp_ext_send(kc, "install_dyn", 0, NULL, &dn)) { SP_GC_ROOT_RBVAL(dn); call(kc, "call_dyn", 1, &c); }
  /* a Hash the kernel answered, back into a Hash parameter */
  sp_RbVal h;
  if (sp_ext_send(kc, "opts", 0, NULL, &h)) { SP_GC_ROOT_RBVAL(h); call(c, "keys", 1, &h); }
  /* the errors: a wrong count of arguments, an argument of a wrong kind
     (a scalar, and a container) */
  const char *cls = 0, *msg = 0;
  call_t bad = { c, "bump", 0, NULL };
  if (Init_ext_methods_try(call_body, &bad, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  call_t kind = { c, "bump", 1, &colon };
  if (Init_ext_methods_try(call_body, &kind, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  call_t hk = { c, "keys", 1, &one };
  if (Init_ext_methods_try(call_body, &hk, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  /* nullable parameters take nil; a value of another kind, and nil for an
     object parameter the method reads, are TypeError */
  sp_RbVal nil = sp_box_nil(), sym = sp_box_sym(sp_sym_intern("x")), you = str("you");
  SP_GC_ROOT_RBVAL(you);
  call(c, "half_of", 1, &nil);
  call(c, "greet", 1, &nil);
  call(c, "greet", 1, &you);
  call_t hs = { c, "half_of", 1, &sym };
  if (Init_ext_methods_try(call_body, &hs, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  call_t tn = { c, "twin_name", 1, &nil };
  if (Init_ext_methods_try(call_body, &tn, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  call(c, "opt_mid", 1, &one);
  call(c, "span", 1, &one);
  /* a String handle parameter takes no value of another kind */
  call_t st = { c, "stamp", 1, &one };
  if (Init_ext_methods_try(call_body, &st, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  /* an object with to_str: a String parameter, a value or a handle, takes
     the String its to_str answers */
  sp_RbVal nd;
  if (sp_ext_send(c, "named", 0, NULL, &nd)) {
    SP_GC_ROOT_RBVAL(nd);
    call(c, "greet", 1, &nd);
    call(c, "stamp", 1, &nd);
  }
  /* an Encoding has no to_str: TypeError for a String value parameter and
     for a handle parameter alike */
  sp_RbVal enc = sp_box_encoding(sp_encoding_utf8());
  call_t ge = { c, "greet", 1, &enc }, se = { c, "stamp", 1, &enc };
  if (Init_ext_methods_try(call_body, &ge, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  if (Init_ext_methods_try(call_body, &se, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  /* a value that is no object of an exported class */
  printf("%d\n", sp_ext_send(one, "bump", 1, &one, &t));
  return 0;
}
