/* ty_traits_check.c -- the ty_traits table (types.c) against the functions
   it summarizes.

   ty_traits holds, per builtin TyKind, the spellings the compiler writes
   for a value of that kind: its C type, its zero and its nil, how it is
   boxed and unboxed on each path that does so, and which sets it belongs
   to. Each column is read off one existing function; where two functions
   spell the same thing differently, each has a column of its own.
   --check-traits renders every column from its function for every kind and
   reports the cells that differ (spinel --check-traits PROGRAM.rb); a
   table read can then replace a function without changing a byte. */

#include "codegen_internal.h"
#include "repr.h"

int g_check_traits = 0, g_dump_traits = 0;

/* a rendering with its expression written $e and its first temp $t */
static char *trait_render(Buf *src) {
  const char *p = src->p ? src->p : "";
  Buf o; memset(&o, 0, sizeof o);
  for (; *p; p++) {
    if (p[0] == '_' && p[1] == 't' && p[2] == '1' && !(p[3] >= '0' && p[3] <= '9')) {
      buf_puts(&o, "_t$t"); p += 2; continue;
    }
    char ch[2] = { *p, 0 }; buf_puts(&o, ch);
  }
  free(src->p);
  return o.p ? o.p : strdup("");
}

/* Does emit_boxed_text have an arm for kind t (else it stops the compile)? */
static int trait_has_box_text(TyKind t) {
  if (t == TY_POLY || t == TY_UNKNOWN || t == TY_VOID || t == TY_NIL) return 1;
  if (ty_nullable_builtin_id(t) || (ty_is_hash(t) && hash_box_cls(t)) || ty_is_obj_array(t)) return 1;
  switch (t) {
  case TY_STRBUF: case TY_INT: case TY_BIGINT: case TY_FLOAT: case TY_STRING: case TY_BOOL:
  case TY_SYMBOL: case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: case TY_TMS:
  case TY_TIME: case TY_COMPLEX: case TY_RATIONAL: case TY_CLASS: case TY_INT_ARRAY:
  case TY_FLOAT_ARRAY: case TY_STR_ARRAY: case TY_POLY_ARRAY: case TY_OPENSTRUCT:
  case TY_INT_ARRAY_ARRAY: case TY_FLOAT_ARRAY_ARRAY:
    return 1;
  default:
    return 0;
  }
}

/* the cells of kind t, as the functions answer them */
static TyTraits trait_from_functions(Compiler *c, TyKind t) {
  TyTraits r; memset(&r, 0, sizeof r);
  r.ctype = c_type_name(t);
  r.zero = default_value(t);
  r.zero_tail = raise_tail_value(t);
  r.nil = nil_value(t);
  r.box = ty_box_fn(t);
  r.box_nil = ty_box_nil_fn(t);
  int sv_tmp = g_tmp;
  if (trait_has_box_text(t)) {
    Buf b; memset(&b, 0, sizeof b);
    g_tmp = 0;
    emit_boxed_text(c, t, "$e", &b);
    r.box_text = trait_render(&b);
    r.box_form = (unsigned char)emit_boxed_text_form(c, t);
  }
  { Buf b; memset(&b, 0, sizeof b); g_tmp = 0; emit_unbox_text(c, t, "$e", &b); r.unbox = trait_render(&b); }
  { Buf b; memset(&b, 0, sizeof b); g_tmp = 0; emit_unbox_nilable_text(c, t, "$e", &b); r.unbox_nil = trait_render(&b); }
  g_tmp = sv_tmp;
  r.box_id = ty_nullable_builtin_id(t);
  r.hash_id = hash_box_cls(t);
  r.unbox_rhs = poly_rhs_unbox_fn(t);
  r.unbox_sink = poly_sink_unbox_fn(t);
  r.unbox_token = token_unbox_fmt(t);
  { LocalVar lv; memset(&lv, 0, sizeof lv); lv.type = t;
    Buf b; memset(&b, 0, sizeof b);
    if (local_nil_test(c, &lv, "$e", &b)) r.nil_test_local = b.p ? b.p : strdup("");
    else free(b.p); }
  r.null_is_nil = (unsigned char)ty_null_is_nil(t);
  r.needs_root = (unsigned char)needs_root(t);
  r.struct_valued = (unsigned char)ty_is_struct_valued(t);
  r.scalar_ret = (unsigned char)is_scalar_ret(t);
  r.store_class = (unsigned char)repr_store_class(c, t);
  return r;
}

static int trait_streq(const char *a, const char *b) {
  if (!a || !b) return a == b;
  return strcmp(a, b) == 0;
}

static void trait_str(FILE *f, const char *s) {
  if (!s) { fputs("NULL", f); return; }
  fputc('"', f);
  for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); fputc(*s, f); }
  fputc('"', f);
}

/* --dump-traits: the table's rows as the functions answer them */
void ty_traits_dump(Compiler *c) {
  for (int t = 0; t < TY_TRAITS_N; t++) {
    TyTraits r = trait_from_functions(c, (TyKind)t);
    printf("  /* %2d %s */ { ", t, ty_name((TyKind)t));
    const char *const cols[] = { r.ctype, r.zero, r.zero_tail, r.nil, r.box, r.box_nil, r.box_text,
                                 r.box_id, r.hash_id, r.unbox, r.unbox_nil, r.unbox_rhs, r.unbox_sink,
                                 r.unbox_token, r.nil_test_local };
    for (unsigned k = 0; k < sizeof cols / sizeof cols[0]; k++) { trait_str(stdout, cols[k]); fputs(", ", stdout); }
    printf("%d, %d, %d, %d, %d, %d },\n", r.null_is_nil, r.needs_root, r.struct_valued, r.scalar_ret,
           r.store_class, r.box_form);
  }
}

/* --check-traits: every cell against its function; answers the number of
   cells that differ, each reported on stderr */
int ty_traits_check(Compiler *c) {
  int bad = 0;
  for (int t = 0; t < TY_TRAITS_N; t++) {
    const TyTraits *tab = ty_traits_of((TyKind)t);
    TyTraits fn = trait_from_functions(c, (TyKind)t);
#define TRAIT_S(col) \
    if (!trait_streq(tab->col, fn.col)) { \
      fprintf(stderr, "check-traits: %s.%s: table %s%s%s, function %s%s%s\n", ty_name((TyKind)t), #col, \
              tab->col ? "\"" : "", tab->col ? tab->col : "NULL", tab->col ? "\"" : "", \
              fn.col ? "\"" : "", fn.col ? fn.col : "NULL", fn.col ? "\"" : ""); bad++; }
#define TRAIT_B(col) \
    if (tab->col != fn.col) { \
      fprintf(stderr, "check-traits: %s.%s: table %d, function %d\n", ty_name((TyKind)t), #col, \
              tab->col, fn.col); bad++; }
    TRAIT_S(ctype) TRAIT_S(zero) TRAIT_S(zero_tail) TRAIT_S(nil) TRAIT_S(box) TRAIT_S(box_nil)
    TRAIT_S(box_text) TRAIT_S(box_id) TRAIT_S(hash_id) TRAIT_S(unbox) TRAIT_S(unbox_nil)
    TRAIT_S(unbox_rhs) TRAIT_S(unbox_sink) TRAIT_S(unbox_token) TRAIT_S(nil_test_local)
    TRAIT_B(null_is_nil) TRAIT_B(needs_root) TRAIT_B(struct_valued) TRAIT_B(scalar_ret)
    TRAIT_B(store_class) TRAIT_B(box_form)
#undef TRAIT_S
#undef TRAIT_B
  }
  fprintf(stderr, "check-traits: %d kinds, %d cells differ\n", TY_TRAITS_N, bad);
  return bad;
}
