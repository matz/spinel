/* lib/sp_poly_cold.c -- the cold half of the boxed-value runtime.

   These functions (inspect, to_s, puts, dup, slice, merge, clear, the keyword
   checks, ...) used to be `static` in spinel_rt.h, so every generated
   translation unit parsed and discarded them. They are compiled once here and
   the header declares them. This unit includes the header as a host
   (SPINEL_EXT_HOST), so the state the generated unit owns -- the exception
   stack, the pending-exception slots, ARGV -- stays extern here, and the header's
   constructors (which hand the collector THIS unit's hooks) are turned off.
   Nothing in this file may keep mutable state of its own: the nm check in
   `make poly-cold-test` fails when a writable local symbol appears. The symbol
   and class-name tables are generated per program; they are reached through the
   hooks the generated unit installs (sp_sym_name_fn, sp_class_name_fn,
   sp_json_sym_intern_fn). */
#define SPINEL_EXT_HOST 1
#include "sp_compat.h"
#undef SP_CONSTRUCTOR
#define SP_CONSTRUCTOR SP_UNUSED
#define sp_sym_to_s sp_poly_cold_sym_to_s
#define sp_class_to_s sp_poly_cold_class_to_s
#define sp_sym_intern sp_poly_cold_sym_intern
#define sp_sym_intern_n sp_poly_cold_sym_intern_n
#include "spinel_rt.h"

#ifdef SP_CEXT
sp_RbVal sp_cext_object_class(sp_RbVal object) {
  return sp_box_class(sp_poly_class_val(object));
}
sp_RbVal sp_cext_plain_object_new(void) {
  return sp_box_obj(sp_Object_new(),SP_BUILTIN_OBJECT);
}
sp_RbVal sp_cext_hash_new(void) {
  return sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH);
}
/* an object of the program as an Integer: its to_int, else CRuby's TypeError */
sp_int sp_cext_object_to_int(sp_RbVal object) {
  return sp_poly_arg_int_obj(object);
}
/* CRuby's T_* number for a value: an extension data class instance is
   T_DATA, an instance of a program class that inherits from a builtin answers
   as that builtin, a Struct's or a Data's T_STRUCT. */
static int sp_cext_class_type(int cls_id) {
  if (!sp_class_kind_of_name_fn) return T_OBJECT;
  if (sp_class_kind_of_name_fn(cls_id, "Array")) return T_ARRAY;
  if (sp_class_kind_of_name_fn(cls_id, "Hash")) return T_HASH;
  if (sp_class_kind_of_name_fn(cls_id, "String")) return T_STRING;
  if (sp_class_kind_of_name_fn(cls_id, "Struct") || sp_class_kind_of_name_fn(cls_id, "Data")) return T_STRUCT;
  return T_OBJECT;
}
int sp_cext_type(sp_RbVal v, int (*data_class)(int)) {
  switch (v.tag) {
  case SP_TAG_NIL: return T_NIL;
  case SP_TAG_BOOL: return v.v.b ? T_TRUE : T_FALSE;
  case SP_TAG_INT: return v.v.i >= RUBY_FIXNUM_MIN && v.v.i <= RUBY_FIXNUM_MAX ? T_FIXNUM : T_BIGNUM;
  case SP_TAG_BIGINT: return T_BIGNUM;
  case SP_TAG_FLT: return T_FLOAT;
  case SP_TAG_SYM: return T_SYMBOL;
  case SP_TAG_STR: return T_STRING;
  case SP_TAG_CLASS:
    return sp_class_is_module_fn && sp_class_is_module_fn((sp_Class){v.cls_id, NULL}) ? T_MODULE : T_CLASS;
  case SP_TAG_OBJ:
    if (v.cls_id == SP_BUILTIN_STRBUF) return T_STRING;
    if (sp_poly_is_array_kind(v.cls_id)) return T_ARRAY;
    if (sp_poly_is_hash_kind(v.cls_id)) return T_HASH;
    if (v.cls_id >= 0) return data_class && data_class(v.cls_id) ? T_DATA : sp_cext_class_type(v.cls_id);
    switch (v.cls_id) {
    case SP_BUILTIN_REGEX: return T_REGEXP;
    case SP_BUILTIN_RANGE: case SP_BUILTIN_FLOAT_RANGE: case SP_BUILTIN_STR_RANGE: return T_STRUCT;
    case SP_BUILTIN_IO: return T_FILE;
    case SP_BUILTIN_MATCHDATA: return T_MATCH;
    case SP_BUILTIN_COMPLEX: return T_COMPLEX;
    case SP_BUILTIN_RATIONAL: case SP_BUILTIN_BIG_RATIONAL: return T_RATIONAL;
    case SP_BUILTIN_OBJECT: case SP_BUILTIN_BASIC_OBJECT: case SP_BUILTIN_EXCEPTION: return T_OBJECT;
    default: return T_DATA;
    }
  default: return T_OBJECT;
  }
}
/* hash[key], with the Hash's default or default proc for a missing key */
sp_RbVal sp_cext_hash_get(sp_RbVal hash, sp_RbVal key) {
  if(hash.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(hash.cls_id))
    sp_raise_cls("TypeError", "expected a Hash");
  return sp_poly_index_poly(hash, key);
}
/* A C extension's hidden instance variables: a name rb_iv_set takes without
   the @ (sqlite3's "-instances"), which CRuby keeps where no Ruby code reads
   it. They live in a Hash of the object's own beside its Ruby ivars
   (sp_ivtbl_hidden_put), keyed as a builtin value's ivars are, and a program
   object by its address. */
static const void *sp_cext_hidden_key(sp_RbVal o) {
  if (o.tag == SP_TAG_OBJ && o.v.p && o.cls_id >= 0) return o.v.p;
  return sp_bivar_key(o);
}
static sp_PolyPolyHash *sp_cext_hidden_table(sp_RbVal o) {
  const void *key = sp_cext_hidden_key(o);
  return key ? (sp_PolyPolyHash *)sp_ivtbl_hidden_get(key) : NULL;
}
int sp_cext_hidden_defined(sp_RbVal o, sp_sym name) {
  sp_PolyPolyHash *t = sp_cext_hidden_table(o);
  return t && sp_PolyPolyHash_has_key(t, sp_box_sym(name));
}
sp_RbVal sp_cext_hidden_get(sp_RbVal o, sp_sym name) {
  sp_PolyPolyHash *t = sp_cext_hidden_table(o);
  if (!t || !sp_PolyPolyHash_has_key(t, sp_box_sym(name))) return sp_box_nil();
  return sp_PolyPolyHash_get(t, sp_box_sym(name));
}
void sp_cext_hidden_set(sp_RbVal o, sp_sym name, sp_RbVal value) {
  if (sp_poly_frozen(o) && o.tag != SP_TAG_CLASS) {
    SP_GC_ROOT_RBVAL(o);
    const char *what = sp_str_concat((&("\xff" "can't modify frozen ")[1]), sp_poly_class_name(o));
    SP_GC_ROOT_STR(what);
    sp_raise_frozen_obj(o, what);
  }
  const void *key = sp_cext_hidden_key(o);
  if (!key)
    sp_raise_cls("NotImplementedError",
                 sp_sprintf("a hidden instance variable on a %s is not supported: the value keeps no identity "
                            "for the variable to live on", sp_poly_class_name(o)));
  SP_GC_ROOT_RBVAL(o); SP_GC_ROOT_RBVAL(value);
  sp_PolyPolyHash *t = (sp_PolyPolyHash *)sp_ivtbl_hidden_get(key);
  if (!t) {
    t = sp_PolyPolyHash_new();
    sp_ivtbl_hidden_put(key, t);
  }
  sp_PolyPolyHash_set(t, sp_box_sym(name), value);
}
/* rb_obj_freeze */
sp_RbVal sp_cext_freeze(sp_RbVal v) {
  return sp_poly_freeze(v);
}
/* rb_ary_delete: every element == item goes, and the answer is item, or nil
   when none was there */
sp_RbVal sp_cext_ary_delete(sp_RbVal ary, sp_RbVal item) {
  if (ary.tag != SP_TAG_OBJ || ary.cls_id != SP_BUILTIN_POLY_ARRAY)
    sp_raise_cls("TypeError", "expected a widened Array");
  return sp_PolyArray_delete((sp_PolyArray *)ary.v.p, item);
}
/* NUM2DBL, as CRuby's rb_num2dbl: a real number converts; nil, true, false
   and a String are a TypeError rather than Kernel#Float's parse */
double sp_cext_num2dbl(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) sp_raise_cls("TypeError", "no implicit conversion to float from nil");
  if (v.tag == SP_TAG_BOOL)
    sp_raise_cls("TypeError", v.v.b ? "no implicit conversion to float from true" : "no implicit conversion to float from false");
  if (v.tag == SP_TAG_STR || (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF))
    sp_raise_cls("TypeError", "no implicit conversion to float from string");
  return sp_poly_to_f(v);
}
/* rb_integer_pack for a word order and byte order of the machine's own, in
   two's complement: the words take the Integer's low bytes, and the answer
   is its sign (-1, 0, +1), or +-2 when it does not fit, as CRuby's bary_pack
   counts fitting: a positive Integer fits in the words' bits unsigned, a
   negative one down to minus two to the bits less one (2**63 fits in a
   64-bit word as 0x8000..., -(2**63) too, -(2**63) - 1 does not). */
int sp_cext_integer_pack(sp_RbVal v, void *words, size_t numwords, size_t wordsize) {
  size_t bytes = numwords * wordsize, nbits = bytes * 8;
  unsigned char *out = words;
  int sign;
  int64_t low;        /* the low 64 bits, two's complement */
  int fits;
  if (v.tag == SP_TAG_BIGINT) {
    sp_Bigint *b = (sp_Bigint *)v.v.p;
    SP_GC_ROOT(b);
    sign = sp_bigint_sign(b);
    low = sp_unbox_i64(v);
    intptr_t bl = sp_bigint_bit_length(b);   /* of the magnitude; of (magnitude - 1) when negative, as Integer#bit_length */
    fits = sign >= 0 ? (size_t)bl <= nbits : (size_t)bl <= nbits - 1;
  }
  else if (v.tag == SP_TAG_INT) {
    int64_t n = v.v.i;
    sign = n < 0 ? -1 : n > 0 ? 1 : 0;
    low = n;
    if (bytes >= sizeof n) fits = 1;
    else {
      int64_t lo = bytes ? -((int64_t)1 << (nbits - 1)) : 0;
      uint64_t hi = bytes ? (((uint64_t)1 << nbits) - 1) : 0;
      fits = n < 0 ? n >= lo : (uint64_t)n <= hi;
    }
  }
  else { sp_raise_cls("TypeError", sp_sprintf("can't convert %s to Integer", sp_poly_class_name(v))); return 0; }
  unsigned char raw[sizeof low];
  memcpy(raw, &low, sizeof low);
  int little = 1;
  little = *(unsigned char *)&little;
  for (size_t i = 0; i < bytes; i++) {
    size_t k = little ? i : bytes - 1 - i;   /* the byte's significance */
    out[i] = k < sizeof low ? raw[little ? k : sizeof low - 1 - k] : (sign < 0 ? 0xff : 0);
  }
  if (!fits) return sign < 0 ? -2 : 2;
  return sign;
}
/* rb_sym2str: the Symbol's name as a frozen String */
sp_RbVal sp_cext_sym2str(sp_sym s) {
  return sp_box_str(sp_sym_to_s(s));
}
/* rb_str_plus: a new String of both, ASCII-8BIT when either is */
sp_RbVal sp_cext_str_plus(sp_RbVal a, sp_RbVal b) {
  SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
  sp_RbVal x = sp_poly_strbuf_deref(a), y = sp_poly_strbuf_deref(b);
  if (x.tag != SP_TAG_STR || y.tag != SP_TAG_STR) sp_raise_cls("TypeError", "no implicit conversion into String");
  SP_GC_ROOT_RBVAL(x); SP_GC_ROOT_RBVAL(y);
  int64_t la = (int64_t)sp_str_byte_len(x.v.s), lb = (int64_t)sp_str_byte_len(y.v.s);
  sp_String *r = sp_String_new_len(x.v.s, la); SP_GC_ROOT(r);
  sp_String_append_n(r, y.v.s, (size_t)lb);
  if (sp_str_is_binary(x.v.s) || sp_str_is_binary(y.v.s)) sp_String_force_encoding(r, 1);
  return sp_box_obj(r, SP_BUILTIN_STRBUF);
}
/* An Array a method yields to a C extension's block (rb_block_call) that
   the analysis did not widen, since the block reaches the method through
   a name the extension computes (racc's yyparse(recv, mid)): C reads it as
   a poly Array of its elements, a copy with its Strings shared, which it
   reads as CRuby's; a change C makes to the copy does not reach the typed
   Array (docs/limitations.md). A poly Array, or any other value, is itself. */
sp_RbVal sp_cext_widen_array(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ || !v.v.p || v.cls_id == SP_BUILTIN_POLY_ARRAY || !sp_poly_is_array_kind(v.cls_id)) return v;
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *copy = sp_poly_to_poly_array(v);
  SP_GC_ROOT(copy);
  return sp_box_poly_array(sp_cext_share_new_array(copy));
}
void sp_cext_hash_set(sp_RbVal hash, sp_RbVal key, sp_RbVal value) {
  if(hash.tag != SP_TAG_OBJ || hash.cls_id != SP_BUILTIN_POLY_POLY_HASH)
    sp_raise_cls("TypeError", "expected a widened Hash");
  if(sp_gc_is_frozen(hash.v.p))sp_raise_frozen_hash_at(hash.v.p,SP_BUILTIN_POLY_POLY_HASH);
  sp_PolyPolyHash_set(hash.v.p,key,value);
}
#endif

const char *(*sp_class_name_fn)(sp_Class);
const char *(*sp_class_display_fn)(sp_Class);
const char *(*sp_class_user_to_s_fn)(sp_Class);
const char *(*sp_class_user_inspect_fn)(sp_Class);
/* A boxed Time's to_s (inspect 0) or inspect (1): a Time subclass
   instance's goes through its class's own dispatcher, which runs an
   override of the class's and falls back to Time's. */
static const char *sp_poly_time_display(sp_RbVal v, int inspect) {
  int k = sp_bsub_cls_fn ? sp_bsub_cls_fn(v) : -1;
  if (k >= 0) {
    if (inspect && sp_obj_inspect_fn) return sp_obj_inspect_fn(k, v.v.p);
    if (!inspect && sp_obj_to_s_fn) return sp_obj_to_s_fn(k, v.v.p);
  }
  return inspect ? sp_Time_inspect((sp_Time *)v.v.p) : sp_Time_to_s((sp_Time *)v.v.p);
}
/* the hooks the generated unit installs for this unit (SP_INSTALL_HOOK) */
sp_user_binop_fn sp_user_binop_hook_lib;
sp_user_aset_fn sp_user_aset_hook_lib;
sp_obj_eq_fn sp_obj_eq_hook_lib;
sp_obj_hash_fn sp_obj_hash_hook_lib;
sp_obj_eql_fn sp_obj_eql_hook_lib;
void (*sp_user_init_copy_hook_lib)(sp_RbVal, sp_RbVal);
sp_RbVal (*sp_bsub_dup_hook_lib)(sp_RbVal, int, sp_bool *);
/* the symbol interner is generated per program too; a keyword name that is not
   a known symbol reaches it through the hook the generated unit installs */
sp_sym sp_poly_cold_sym_intern(const char *s) { return sp_json_sym_intern_fn ? sp_json_sym_intern_fn(s) : (sp_sym)0; }
/* and a counted name, for sp_thread_local_key: this unit compiles the
   header's sp_poly_has_key and sp_poly_index_poly (slice's key walk), whose
   Thread arms call it. The hook takes a C string, so a key interns up to its
   first NUL byte. */
sp_sym sp_poly_cold_sym_intern_n(const char *s, size_t n) { (void)n; return sp_poly_cold_sym_intern(s); }
const char *sp_poly_cold_sym_to_s(sp_sym id) { return sp_sym_name_fn ? sp_sym_name_fn(id) : sp_str_empty; }
const char *sp_poly_cold_class_to_s(sp_Class c) {
  if (c.name) return c.name;
  return sp_class_name_fn ? sp_class_name_fn(c) : sp_str_empty;
}
/* The class name a default #to_s / #inspect prints for a user object: the
   display form of a Class.new class when the program installed one, else its name. */
static const char *sp_poly_obj_class_display(sp_RbVal v) {
  if (sp_class_display_fn && v.cls_id >= 0) {
    sp_Class c = {v.cls_id, NULL};
    return sp_class_display_fn(c);
  }
  return sp_poly_class_name(v);
}

const char *sp_sym_inspect(sp_sym id)
{ if (id == (sp_sym)-1) return SPL("nil"); /* nilable-symbol sentinel */ return sp_sym_inspect_name(sp_sym_to_s(id)); }

void sp_poly_puts(sp_RbVal v)
{
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) {
        size_t _n = sp_str_byte_len(v.v.s);
        fwrite(v.v.s, 1, _n, stdout);
        if (!_n || v.v.s[_n - 1] != '\n') putchar('\n');
      }
      else putchar('\n'); break;
    case SP_TAG_FLT: { fputs(sp_float_to_s(v.v.f), stdout); putchar('\n'); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    case SP_TAG_ENCODING: { const char *_es = v.v.s ? v.v.s : sp_str_empty; fputs(_es, stdout); putchar('\n'); break; }
    case SP_TAG_CLASS: { fputs(sp_class_val_user(v, sp_class_user_to_s_fn), stdout); putchar('\n'); break; }
    case SP_TAG_BIGINT: { const char *_bs = sp_bigint_to_s((sp_Bigint *)v.v.p); if (_bs) fputs(_bs, stdout); putchar('\n'); break; }
    case SP_TAG_OBJ: {
      /* MRI's `puts arr` iterates an Array, printing one element per
         line (using to_s on each); a non-Array OBJ falls back to
         inspect / class-name. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++)
            printf("%lld\n", (long long)_a->data[_a->start + _i]);
          break;
        }
        case SP_BUILTIN_FLT_ARRAY: {
          sp_FloatArray *_a = (sp_FloatArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            fputs(sp_float_to_s(_a->data[_i]), stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_STR_ARRAY: {
          sp_StrArray *_a = (sp_StrArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            const char *_s = SP_SEB(_a->data[_i]);
            if (_s) {
              size_t _n = sp_str_byte_len(_s);
              fwrite(_s, 1, _n, stdout);
              if (!_n || _s[_n - 1] != '\n') putchar('\n');
            }
            else putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_SYM_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (sp_int _i = 0; _i < _a->len; _i++) {
            const char *_s = sp_sym_to_s((sp_sym)_a->data[_a->start + _i]);
            fputs(_s, stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_RANGE: puts(sp_range_str(*(sp_Range *)v.v.p)); break;
        case SP_BUILTIN_FLOAT_RANGE: puts(sp_frange_inspect(*(sp_FloatRange *)v.v.p)); break;
        case SP_BUILTIN_STR_RANGE: puts(sp_srange_to_s(SP_SRANGE_OF(v.v.p))); break;
        /* a Time subclass instance, boxed as its Time, shows through its
           class's to_s (an override of its own included) */
        case SP_BUILTIN_TIME: sp_puts_str_line(sp_poly_time_display(v, 0)); break;
        case SP_BUILTIN_STRBUF: sp_puts_str_line(sp_String_cstr((sp_String *)v.v.p)); break;
        case SP_BUILTIN_COMPLEX: puts(sp_complex_to_s(*(sp_Complex *)v.v.p)); break;
        case SP_BUILTIN_RATIONAL: puts(sp_rational_to_s(*(sp_Rational *)v.v.p)); break;
        case SP_BUILTIN_BIG_RATIONAL: puts(sp_brat_to_s((sp_BigRational *)v.v.p)); break;
        case SP_BUILTIN_REGEX: puts(sp_re_to_s_str(v.v.p)); break;
        case SP_BUILTIN_PTR_ARRAY: {   /* rows or objects, one per line like any array (#4486) */
          sp_PtrArray *_pa = (sp_PtrArray *)v.v.p;
          if (_pa) for (sp_int _i = 0; _i < _pa->len; _i++) sp_poly_puts(sp_PtrArray_elem_box(_pa, _pa->data[_i]));
          break;
        }
        case SP_BUILTIN_POLY_ARRAY: {
          /* puts flattens arrays recursively, one element per line -- and one
             that holds itself gets CRuby's single `[...]` line instead of an
             endless flattening. */
          sp_PolyArray *_a = (sp_PolyArray *)v.v.p;
          if (sp_poly_recur_seen(SP_POLY_RECUR_PUTS, _a, NULL)) { puts("[...]"); break; }
          int _pm = sp_poly_recur_push(SP_POLY_RECUR_PUTS, _a, NULL);
          for (sp_int _i = 0; _i < _a->len; _i++) sp_poly_puts(_a->data[_i]);
          sp_poly_recur_pop(_pm);
          break;
        }
        /* A user object (or any non-array OBJ) prints via to_s: delegate to
           sp_poly_to_s, which dispatches the class's user #to_s (falling back to
           the default #<Name:0x..>). The bare #<Object> print skipped the user
           to_s for an object read from a collection (#3189). */
        default: { fputs(sp_poly_to_s(v), stdout); putchar('\n'); break; }
      }
      break;
    }
    default: printf("%lld\n", (long long)v.v.i); break;
  }
}

const char *sp_poly_to_s(sp_RbVal v)
{
  switch (v.tag) {
    case SP_TAG_INT: return sp_int_to_s(v.v.i);
    case SP_TAG_STR: return v.v.s ? v.v.s : sp_str_frozen_empty;
    case SP_TAG_FLT: return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? sp_str_frozen_true : sp_str_frozen_false;
    case SP_TAG_NIL: return sp_str_frozen_empty;
    case SP_TAG_SYM: return sp_sym_to_s_frozen((sp_sym)v.v.i);
    case SP_TAG_CLASS: return sp_class_val_user(v, sp_class_user_to_s_fn);
    case SP_TAG_ENCODING: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect_k((sp_PtrArray *)v.v.p);
        /* Array#to_s is Array#inspect; a boxed PolyArray element had no arm
           and fell through to "" (#3007) */
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE: return sp_range_str(*(sp_Range *)v.v.p);
        case SP_BUILTIN_FLOAT_RANGE: return sp_frange_inspect(*(sp_FloatRange *)v.v.p);
        case SP_BUILTIN_STR_RANGE: return sp_srange_to_s(SP_SRANGE_OF(v.v.p));
        case SP_BUILTIN_TIME: return sp_poly_time_display(v, 0);
        case SP_BUILTIN_STRBUF: return sp_String_cstr((sp_String *)v.v.p);   /* live buffer (#3227) */
        case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
        /* Proc#to_s is its inspect; a boxed one (an element of a mixed Array)
           fell through to "" */
        case SP_BUILTIN_PROC: return sp_proc_inspect((sp_Proc *)v.v.p);
        case SP_BUILTIN_CURRY: return sp_curry_inspect((sp_Curry *)v.v.p);
        case SP_BUILTIN_COMPLEX: return sp_complex_to_s(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL: return sp_rational_to_s(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_BIG_RATIONAL: return sp_brat_to_s((sp_BigRational *)v.v.p);
        case SP_BUILTIN_REGEX: return sp_re_to_s_str(v.v.p);
        /* MatchData#to_s is the whole match (#3641) */
        case SP_BUILTIN_MATCHDATA: { const char *_m0 = sp_MatchData_aref((sp_MatchData *)v.v.p, 0); return _m0 ? _m0 : sp_str_empty; }
        case SP_BUILTIN_EXCEPTION: return sp_exc_message((volatile struct sp_Exception_s *)v.v.p);
        /* Object#to_s on a boxed handle of the IO family, as the typed arm
           renders it: a boxed one fell through to "" (`puts [f, d]`) */
        case SP_BUILTIN_IO: return sp_io_to_s((sp_File *)v.v.p);
        case SP_BUILTIN_DIR: return sp_Dir_to_s((sp_Dir *)v.v.p);
        case SP_BUILTIN_FIBER: return sp_Fiber_inspect((sp_Fiber *)v.v.p);
        case SP_BUILTIN_THREAD: return sp_Thread_inspect((sp_thread *)v.v.p);
        /* OpenStruct#to_s is its inspect, as sp_poly_inspect renders it */
        case SP_BUILTIN_OPENSTRUCT: return sp_OpenStruct_inspect((struct sp_OpenStruct_s *)v.v.p);
        default:
          if ((v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT) && v.v.p) {
            /* a class with a user #to_s renders through the generated
               dispatcher; the rest (bare Object.new included) get CRuby's
               default #<Name:0xADDR> */
            if (v.cls_id >= 0 && sp_obj_to_s_fn) {
              /* rooted across the user #to_s, which allocates: the boxed
                 copy may be a fresh object's only reference (`puts(k ?
                 C.new : 1)`), as in sp_poly_check_str_obj */
              SP_GC_ROOT_RBVAL(v);
              const char *us = sp_obj_to_s_fn(v.cls_id, v.v.p);
              if (us) return us;
            }
            /* a user exception subclass boxes under its own class id */
            if (sp_is_exc_subclass_cls(v.cls_id))
              return sp_exc_message((volatile struct sp_Exception_s *)v.v.p);
            if (v.v.p == sp_main_obj) return SPL("main");
            return sp_sprintf("#<%s:0x%016llx>", sp_poly_obj_class_display(v),
                              (unsigned long long)(uintptr_t)v.v.p);
          }
          /* a builtin container kind with no explicit arm above (the hash
             variants): to_s is its inspect */
          if (sp_poly_is_hash_kind(v.cls_id)) return sp_poly_inspect(v);
          return sp_str_empty;
      }
    default: return sp_str_empty;
  }
}

const char *sp_poly_class_name(sp_RbVal v)
{
  switch (v.tag) {
    case SP_TAG_INT: return SPL("Integer");
    case SP_TAG_STR: return SPL("String");
    case SP_TAG_FLT: return SPL("Float");
    case SP_TAG_BOOL: return v.v.b ? SPL("TrueClass") : SPL("FalseClass");
    case SP_TAG_NIL: return SPL("NilClass");
    case SP_TAG_SYM: return SPL("Symbol");
    case SP_TAG_ENCODING: return SPL("Encoding");
    case SP_TAG_CLASS: {
      sp_Class c = sp_unbox_class(v);
      int m = sp_class_is_module_fn ? sp_class_is_module_fn(c)
            : (c.cls_id == -114 || c.cls_id == -115 || c.cls_id == -119 || c.cls_id == -130 ||
               c.cls_id == -162);
      return m ? SPL("Module") : SPL("Class");
    }
    case SP_TAG_BIGINT: return SPL("Integer");
    case SP_TAG_OBJ:
      /* an Array subclass instance boxed as its Array (#7449) */
      if (sp_bsub_cls_fn && sp_obj_cls_name_fn) {
        int k = sp_bsub_cls_fn(v);
        if (k >= 0) return sp_obj_cls_name_fn(k);
      }
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
        case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
        case SP_BUILTIN_PTR_ARRAY: case SP_BUILTIN_POLY_ARRAY: return SPL("Array");
        case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
        case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_SYM_INT_HASH:
         case SP_BUILTIN_INT_INT_HASH:
        case SP_BUILTIN_SYM_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
        case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH: return SPL("Hash");
        case SP_BUILTIN_RANGE: return SPL("Range");
        case SP_BUILTIN_FLOAT_RANGE: case SP_BUILTIN_STR_RANGE: return SPL("Range");
        case SP_BUILTIN_TIME: return SPL("Time");
    case SP_BUILTIN_STRBUF: return SPL("String");   /* (#3227) */
        case SP_BUILTIN_COMPLEX: return SPL("Complex");
        case SP_BUILTIN_RATIONAL: return SPL("Rational");
        case SP_BUILTIN_BIG_RATIONAL: return SPL("Rational");
        case SP_BUILTIN_REGEX: return SPL("Regexp");
        case SP_BUILTIN_MATCHDATA: return SPL("MatchData");   /* (#3641) */
        case SP_BUILTIN_OBJECT: return SPL("Object");   /* a bare Object.new instance */
        case SP_BUILTIN_BASIC_OBJECT: return SPL("BasicObject");
        case SP_BUILTIN_PROC: return SPL("Proc");
        /* a curried proc IS a Proc to Ruby (#3885) */
        case SP_BUILTIN_CURRY: return SPL("Proc");
        case SP_BUILTIN_METHOD:
          return ((sp_BoundMethod *)v.v.p)->unbound ? SPL("UnboundMethod") : SPL("Method");   /* (#3692) */
        /* a chain and a product report their own class, as the typed
           #class does */
        case SP_BUILTIN_ENUMERATOR: return sp_enum_class_name(v.v.p);
        case SP_BUILTIN_IO: {
          /* the handle kind names the class, through the same authority the
             typed .class emit uses -- a boxed socket must not report plain IO */
          sp_File *_iof = (sp_File *)v.v.p;
          if (_iof && _iof->mode &&
              (strcmp(_iof->mode, "stat") == 0 || strcmp(_iof->mode, "lstat") == 0))
            return SPL("File::Stat");
          return sp_io_kind_name(_iof);
        }
        /* The concurrency handles: a boxed one reached the default arm and
           answered an empty name, so `(:ok && queue).class` printed nothing
           and a method on it raised NoMethodError with a blank class (#3484).
           Mutex and Queue name themselves through the same helpers the typed
           `.class` emit uses, so a Monitor and a SizedQueue stay distinct. */
        case SP_BUILTIN_MUTEX: return sp_Mutex_class_name((sp_mutex *)v.v.p);
        case SP_BUILTIN_QUEUE: return sp_Queue_class_name((sp_queue *)v.v.p);
        case SP_BUILTIN_CONDVAR: return SPL("Thread::ConditionVariable");
        case SP_BUILTIN_FIBER: return SPL("Fiber");
        case SP_BUILTIN_THREAD: return SPL("Thread");
        case SP_BUILTIN_DIR: return SPL("Dir");
        case SP_BUILTIN_TMS: return SPL("Process::Tms");
        case SP_BUILTIN_OPENSTRUCT: return SPL("OpenStruct");
        /* These two had no arm of their own. They went unnoticed because their
           cls_ids collided with STRBUF and OPENSTRUCT, so a boxed one answered
           String / OpenStruct rather than nothing (#4158). */
        case SP_BUILTIN_ADDRINFO: return SPL("Addrinfo");
        case SP_BUILTIN_SOCKOPT: return SPL("Socket::Option");
        case SP_BUILTIN_PROCESS_STATUS: return SPL("Process::Status");
        case SP_BUILTIN_YIELDER: return SPL("Enumerator::Yielder");
        case SP_BUILTIN_RANDOM: return SPL("Random");
        case SP_BUILTIN_ARGF: return SPL("ARGF.class");
        case SP_BUILTIN_EXCEPTION: return sp_exc_class_name((volatile struct sp_Exception_s *)v.v.p);
        default: { sp_Class c = {v.cls_id}; return sp_class_to_s(c); }
      }
    default: return SPL("Object");
  }
}

sp_RbVal sp_poly_slice(sp_RbVal a, sp_int start, sp_int len)
{
  if (a.tag == SP_TAG_STR) return sp_box_nullable_str(sp_str_sub_range(a.v.s ? a.v.s : "", start, len));
  /* A shared-string handle is a String: slicing is non-mutating, so it answers
     as its live value rather than falling through to the array kinds and out
     the nil default, which is what `s = +""; s << "abc"; s[0, 2]` did (#4279). */
  if (sp_poly_is_strbuf(a)) return sp_poly_slice(sp_poly_strbuf_deref(a), start, len);
  /* An Integer answers `n[start, len]`, the len-bit field starting at bit
     `start` -- the two-argument form of the bit read, which the typed arms
     have had all along. A boxed receiver fell past this to the nil default
     below, so `[255, nil][0][0, 4]` was nil where CRuby says 15, and the
     value went on being used as a number (#4742). */
  if (a.tag == SP_TAG_INT) {
    return sp_box_int(sp_int_bit_range(a.v.i, start, len));
  }
  if (a.tag == SP_TAG_BIGINT) {
    sp_Bigint *n = (sp_Bigint *)a.v.p;
    if (!n) return sp_box_nil();
    if (start < 0 || len < 0) return sp_box_int(0);
    { sp_Bigint *sh = sp_bigint_shr(n, (int64_t)start);
      sp_Bigint *m = sp_bigint_sub(sp_bigint_shl(sp_bigint_new_int(1), (int64_t)len),
                                   sp_bigint_new_int(1));
      return sp_box_int(sp_bigint_to_int(sp_bigint_and(sh, m))); }
  }
  /* A Symbol has `[]`: it answers its NAME sliced, as a String. Falling to
     the default below made `[:sym, nil][0][0, 4]` nil, which then went on
     being used as one. */
  if (a.tag == SP_TAG_SYM)
    return sp_box_nullable_str(sp_str_sub_range(sp_sym_to_s((sp_sym)a.v.i), start, len));
  /* Everything else here -- nil, a Float, true, false -- simply has no `[]`,
     and CRuby says so. The default used to answer nil for all of them, so a
     receiver that was never sliceable produced a value indistinguishable
     from an in-range miss, and the program carried it forward. (The Integer
     arm above notes the same thing about nil: a missing method, not a bit
     field.) */
  if (a.tag != SP_TAG_OBJ) sp_raise_poly_nomethod("[]", a);
  /* arr[start, negative] is nil in CRuby (the slice helpers would return []) */
  if (len < 0 && sp_poly_is_array_kind(a.cls_id)) return sp_box_nil();
  /* bm[a, b]: a boxed bound Method called with two int arguments (optcarrot's
     store dispatch table: `@store[addr][addr, value]`). The raw fn cast is
     only valid for a target whose stamped per-position ABI accepts a scalar
     at every fixed position (sp_bm_legacy_abi_ok); anything else (a
     rest/optional/keyword signature, a float/poly/struct parameter, or a
     pointer parameter the integer operands cannot fill) would read the wrong
     C types, so route it through the callable helper, which raises CRuby's
     NoMethodError instead of crashing (#4395). */
  if (a.cls_id == SP_BUILTIN_METHOD) {
    sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
    /* A poly-ABI target (stamped at bind time; the promote signature) takes
       the two operands boxed and answers boxed. Checked ahead of the legacy
       gate: under promote the legacy stamp is 0, and elsewhere the two are
       mutually exclusive by construction. */
    if (m->fn && sp_bm_poly_abi_ok(m, 2)) {
      sp_RbVal _a = sp_box_int(start), _b = sp_box_int(len);
      if (m->legacy_ret == SP_BM_RET_POLY) {
        if (m->recv_bound)
          return ((sp_RbVal (*)(void *, sp_RbVal, sp_RbVal))(uintptr_t)m->fn)((void *)m->self, _a, _b);
        return ((sp_RbVal (*)(sp_RbVal, sp_RbVal))(uintptr_t)m->fn)(_a, _b);
      }
      if (m->legacy_ret == SP_BM_RET_NIL) {   /* a C void function: wasm checks the signature */
        if (m->recv_bound) ((void (*)(void *, sp_RbVal, sp_RbVal))(uintptr_t)m->fn)((void *)m->self, _a, _b);
        else ((void (*)(sp_RbVal, sp_RbVal))(uintptr_t)m->fn)(_a, _b);
        return sp_box_nil();
      }
      if (m->recv_bound)
        return sp_bm_box_ret(m, sp_bm_norm_ret(m, ((sp_int (*)(void *, sp_RbVal, sp_RbVal))(uintptr_t)m->fn)((void *)m->self, _a, _b)));
      return sp_bm_box_ret(m, sp_bm_norm_ret(m, ((sp_int (*)(sp_RbVal, sp_RbVal))(uintptr_t)m->fn)(_a, _b)));
    }
    /* The two operands are sp_int, so the target's fixed positions must be the
       int token (or the TY_UNKNOWN wildcard), not some other scalar kind. */
    if (sp_bm_legacy_abi_ok(m, 2, "0000000100000001")) {
      /* A top-level method has no self; its C signature leads with the first
         parameter, so the self-ful cast would shift both operands (#4395). A
         poly return is an sp_RbVal in two registers, so it takes its own cast. */
      if (m->legacy_ret == SP_BM_RET_POLY) {
        if (m->recv_bound)
          return ((sp_RbVal (*)(void *, sp_int, sp_int))(uintptr_t)m->fn)((void *)m->self, start, len);
        return ((sp_RbVal (*)(sp_int, sp_int))(uintptr_t)m->fn)(start, len);
      }
      /* a nil-returning target is a C void function, and wasm checks the
         callee's signature at the call (sp_method_proc_tramp has the same arm) */
      if (m->legacy_ret == SP_BM_RET_NIL) {
        if (m->recv_bound) ((void (*)(void *, sp_int, sp_int))(uintptr_t)m->fn)((void *)m->self, start, len);
        else ((void (*)(sp_int, sp_int))(uintptr_t)m->fn)(start, len);
        return sp_box_nil();
      }
      if (m->recv_bound)
        return sp_bm_box_ret(m, ((sp_int (*)(void *, sp_int, sp_int))(uintptr_t)m->fn)((void *)m->self, start, len));
      return sp_bm_box_ret(m, ((sp_int (*)(sp_int, sp_int))(uintptr_t)m->fn)(start, len));
    }
    _sp_proc_poly_args[0] = sp_box_int(start);
    _sp_proc_poly_args[1] = sp_box_int(len);
    sp_int slots[2]; slots[0] = start; slots[1] = len;
    return sp_poly_callable_call(a, 2, slots);
  }
  /* Proc#[] is #call: a two-int slice on a callable is a two-argument call.
     The emitter sends only statically-Integer operands here, so a boxed Proc
     previously fell into the array switch and answered nil (#4395). */
  if (a.v.p && (a.cls_id == SP_BUILTIN_PROC || a.cls_id == SP_BUILTIN_CURRY)) {
    _sp_proc_poly_args[0] = sp_box_int(start);
    _sp_proc_poly_args[1] = sp_box_int(len);
    sp_int slots[16];
    slots[0] = start;
    slots[1] = len;
    return sp_poly_callable_call(a, 2, slots);
  }
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  return sp_box_int_array(sp_IntArray_slice((sp_IntArray*)a.v.p, start, len));
    case SP_BUILTIN_FLT_ARRAY:  return sp_box_float_array(sp_FloatArray_slice((sp_FloatArray*)a.v.p, start, len));
    case SP_BUILTIN_STR_ARRAY:  return sp_box_str_array(sp_StrArray_slice((sp_StrArray*)a.v.p, start, len));
    case SP_BUILTIN_POLY_ARRAY: return sp_box_poly_array(sp_PolyArray_slice((sp_PolyArray*)a.v.p, start, len));
    /* a slice of a pointer array is a fresh general Array of the same rows or
       objects, by reference (#4486) */
    case SP_BUILTIN_PTR_ARRAY:  return sp_box_poly_array(sp_PolyArray_slice(sp_PtrArray_to_poly((sp_PtrArray*)a.v.p), start, len));
    default: return sp_box_nil();
  }
}

const char *sp_poly_inspect(sp_RbVal v)
{
  switch (v.tag) {
    case SP_TAG_INT:  return sp_int_to_s(v.v.i);
    case SP_TAG_STR:  return sp_str_inspect(v.v.s);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    /* true.inspect is true.to_s, the frozen one; nil.inspect is a new "nil" */
    case SP_TAG_BOOL: return v.v.b ? sp_str_frozen_true : sp_str_frozen_false;
    case SP_TAG_NIL:  return SPL("nil");
    case SP_TAG_SYM:  return sp_sym_inspect((sp_sym)v.v.i);
    case SP_TAG_ENCODING: return sp_encoding_inspect_name(v.v.s ? v.v.s : "");
    case SP_TAG_CLASS: return sp_class_val_user(v, sp_class_user_inspect_fn);
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
 /* Built-in container / value-type tags get their typed inspect
    helper. Matches the dispatch shape in sp_poly_to_s above and the
    `puts` poly arm earlier in this file; without it, a Range / Time
    / typed Array stored as an sp_RbVal value (e.g. a sym_poly_hash
    that mixes `200..299` and `404`) reported "#<Object>" from
    `.inspect`, which was both wrong for CRuby parity and useless
    for debugging. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect_k((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE:     return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_FLOAT_RANGE: return sp_frange_inspect(*(sp_FloatRange *)v.v.p);
        case SP_BUILTIN_STR_RANGE: return sp_srange_inspect(SP_SRANGE_OF(v.v.p));
        case SP_BUILTIN_TIME:      return sp_poly_time_display(v, 1);
      case SP_BUILTIN_STRBUF: return sp_str_inspect(sp_String_cstr((sp_String *)v.v.p));   /* (#3227) */
      case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
      case SP_BUILTIN_PROC: return sp_proc_inspect((sp_Proc *)v.v.p);
      case SP_BUILTIN_CURRY: return sp_curry_inspect((sp_Curry *)v.v.p);
        case SP_BUILTIN_COMPLEX:   return sp_complex_inspect(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL:  return sp_rational_inspect(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_BIG_RATIONAL:  return sp_brat_inspect((sp_BigRational *)v.v.p);
        case SP_BUILTIN_REGEX:     return sp_re_inspect_str(v.v.p);
        case SP_BUILTIN_MATCHDATA: return sp_MatchData_inspect((sp_MatchData *)v.v.p);
        case SP_BUILTIN_EXCEPTION: return sp_exc_inspect(v.v.p);
        case SP_BUILTIN_IO:        return sp_File_inspect((sp_File *)v.v.p);
        case SP_BUILTIN_STR_INT_HASH:  return sp_StrIntHash_inspect((sp_StrIntHash *)v.v.p);
        case SP_BUILTIN_STR_STR_HASH:  return sp_StrStrHash_inspect((sp_StrStrHash *)v.v.p);
        case SP_BUILTIN_INT_STR_HASH:  return sp_IntStrHash_inspect((sp_IntStrHash *)v.v.p);
        case SP_BUILTIN_INT_INT_HASH:  return sp_IntIntHash_inspect((sp_IntIntHash *)v.v.p);
        case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_inspect((sp_StrPolyHash *)v.v.p);
        case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_inspect((sp_SymPolyHash *)v.v.p);
        case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_inspect((sp_PolyPolyHash *)v.v.p);
        case SP_BUILTIN_OPENSTRUCT: return sp_OpenStruct_inspect((struct sp_OpenStruct_s *)v.v.p);
        case SP_BUILTIN_ENUMERATOR: return sp_enum_inspect_boxed(v);
        /* #<Dir:PATH>, as the typed Dir's inspect renders it (#3250) */
        case SP_BUILTIN_DIR: {
          const char *_dp = v.v.p ? ((sp_Dir *)v.v.p)->path : NULL;
          return sp_sprintf("#<Dir:%s>", _dp ? _dp : "");
        }
        case SP_BUILTIN_FIBER:  return sp_Fiber_inspect((sp_Fiber *)v.v.p);
        case SP_BUILTIN_THREAD: return sp_Thread_inspect((sp_thread *)v.v.p);
        case SP_BUILTIN_ARGF:   return SPL("ARGF");
        /* the typed form's rendering, which lists the ivars a program set */
        case SP_BUILTIN_RANDOM: if (v.v.p) return sp_Random_inspect((sp_Random *)v.v.p); return SPL("#<Object>");
        default:
          /* a user object: the generated per-class ivar walk renders
             #<Name:0x... @a=..., ...> like CRuby's default inspect */
          if (v.cls_id >= 0 && sp_obj_inspect_fn && v.v.p) {
            SP_GC_ROOT_RBVAL(v);   /* across the user #inspect or the ivar walk, as for #to_s */
            return sp_obj_inspect_fn(v.cls_id, v.v.p);
          }
          if (v.cls_id == SP_BUILTIN_OBJECT && v.v.p && v.v.p == sp_main_obj) return SPL("main");
          if ((v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT) && v.v.p)
            return sp_sprintf("#<%s:0x%016llx>", sp_poly_obj_class_display(v),
                              (unsigned long long)(uintptr_t)v.v.p);
          /* a builtin handle with no inspect of its own (a Fiber, a Queue, a
             Mutex): name it rather than answering the useless "#<Object>" --
             the tag knows what it is, and CRuby's default inspect is this
             shape too */
          if (v.v.p) {
            const char *bn = sp_poly_class_name(v);
            if (bn && bn[0])
              return sp_sprintf("#<%s:0x%016llx>", bn, (unsigned long long)(uintptr_t)v.v.p);
          }
          return SPL("#<Object>");
      }
    default:          return sp_str_empty;
  }
}

sp_StrPolyHash*sp_StrPolyHash_merge(sp_StrPolyHash*a,sp_StrPolyHash*b)
{SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++)sp_StrPolyHash_set(r,a->order[i],sp_StrPolyHash_get(a,a->order[i]));for(sp_int i=0;i<b->len;i++)sp_StrPolyHash_set(r,b->order[i],sp_StrPolyHash_get(b,b->order[i]));return r;}

sp_StrPolyHash*sp_StrPolyHash_dup(sp_StrPolyHash*h)
{if(!h)return NULL;SP_GC_ROOT(h);sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_StrPolyHash_set(r,h->order[i],sp_StrPolyHash_get(h,h->order[i]));return r;}

void sp_StrPolyHash_clear(sp_StrPolyHash*h)
{ sp_gc_wb((void*)h);if (SP_UNLIKELY(!h)) sp_nil_recv("clear");for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}

const char*sp_StrPolyHash_inspect(sp_StrPolyHash*h)
{return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_POLY_HASH)):SPL("nil");}

sp_StrStrHash *sp_StrPolyHash_to_s_values(sp_StrPolyHash *h)
{
  SP_GC_ROOT(h);
  sp_StrStrHash *r = sp_StrStrHash_new(); SP_GC_ROOT(r);
  /* a missing match reads the default, as its to_s (nil's "" is no default) */
  if (h && h->default_v.tag != SP_TAG_NIL) r->default_v = SP_SEF(sp_poly_to_s(h->default_v));
  if (h) for (sp_int i = 0; i < h->len; i++) {
    const char *k = h->order[i];
    sp_StrStrHash_set_e(r, k, SP_SEF(sp_poly_to_s(sp_StrPolyHash_get(h, k))));
  }
  return r;
}

/* The text sub/gsub put in for a match a Hash of any keys maps: the value
   #[] reads for it (a default proc runs, and may store the key), as its
   to_s, nil's "" included */
const char *sp_PolyPolyHash_repl(sp_PolyPolyHash *h, const char *key)
{
  SP_GC_ROOT(h);
  return sp_poly_to_s(sp_PolyPolyHash_get(h, sp_box_str(key)));
}

sp_SymPolyHash*sp_SymPolyHash_merge(sp_SymPolyHash*a,sp_SymPolyHash*b)
{SP_GC_ROOT(a);SP_GC_ROOT(b);sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++)sp_SymPolyHash_set(r,a->order[i],sp_SymPolyHash_get(a,a->order[i]));for(sp_int i=0;i<b->len;i++)sp_SymPolyHash_set(r,b->order[i],sp_SymPolyHash_get(b,b->order[i]));return r;}

sp_OpenStruct *sp_OpenStruct_dup(sp_OpenStruct *o, int keep_frozen)
{
  if(!o) return NULL;
  SP_GC_ROOT(o);
  sp_SymPolyHash *t=sp_OpenStruct_to_h(o); SP_GC_ROOT(t);
  sp_OpenStruct *r=sp_OpenStruct_new_from(t);
  if(keep_frozen&&sp_gc_is_frozen(o)) sp_gc_freeze(r);
  return r;
}

const char *sp_OpenStruct_inspect(sp_OpenStruct *o)
{
  SP_GC_ROOT(o);      /* o may be the caller's bare temporary; the rendering allocates */
  /* an OpenStruct reached from inside itself renders as the ellipsis and stops,
     as CRuby's does: `#<OpenStruct a=1, me=#<OpenStruct ...>>` */
  if(o&&sp_poly_recur_seen(SP_POLY_RECUR_INSPECT,o,NULL)) return (&("\xff" "#<OpenStruct ...>")[1]);
  int rmark=o?sp_poly_recur_push(SP_POLY_RECUR_INSPECT,o,NULL):-1;
  sp_String *s=sp_String_new(""); SP_GC_ROOT(s);
  sp_String_append(s,"#<OpenStruct");
  if(o&&o->tbl) for(sp_int i=0;i<o->tbl->len;i++){
    sp_sym k=o->tbl->order[i];
    sp_String_append(s, i==0?" ":", ");
    sp_String_append(s, sp_sym_to_s(k));
    sp_String_append(s, "=");
    sp_String_append(s, sp_poly_inspect(sp_SymPolyHash_get(o->tbl,k)));
  }
  sp_String_append(s,">");
  if(rmark>=0) sp_poly_recur_pop(rmark);
  /* an independent GC heap string, not the sp_String's own buffer: the wrapper
     is unrooted on return and its data would dangle if the result is stored. */
  return sp_str_concat(sp_String_cstr(s), (&("\xff")[1]));
}

sp_SymPolyHash*sp_SymPolyHash_dup(sp_SymPolyHash*h)
{if(!h)return NULL;sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_SymPolyHash_set(r,h->order[i],sp_SymPolyHash_get(h,h->order[i]));return r;}

void sp_SymPolyHash_clear(sp_SymPolyHash*h)
{if (SP_UNLIKELY(!h)) sp_nil_recv("clear");for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;}

const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h)
{return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_SYM_POLY_HASH)):SPL("nil");}

sp_PolyPolyHash*sp_PolyPolyHash_merge(sp_PolyPolyHash*a,sp_PolyPolyHash*b)
{SP_GC_ROOT(a);SP_GC_ROOT(b);if (SP_UNLIKELY(!a)) sp_nil_recv("merge");sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++){sp_int idx=a->order[i];sp_PolyPolyHash_set(r,a->keys[idx],a->vals[idx]);}if(b){for(sp_int i=0;i<b->len;i++){sp_int idx=b->order[i];sp_PolyPolyHash_set(r,b->keys[idx],b->vals[idx]);}}return r;}

void sp_PolyPolyHash_clear(sp_PolyPolyHash*h)
{if (SP_UNLIKELY(!h)) sp_nil_recv("clear");for(sp_int i=0;i<h->cap;i++)h->occ[i]=0;h->len=0;}

sp_RbVal sp_poly_clear(sp_RbVal v)
{
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_QUEUE && v.v.p) { sp_Queue_clear((sp_queue *)v.v.p); return v; }
  /* String#clear on a plain string box: a fresh empty string the receiver
     takes back, as the typed clear builds; a frozen one raises. It answered
     the box untouched. */
  if (v.tag == SP_TAG_STR) { sp_str_check_mutable(v.v.s); return sp_box_str(sp_str_from_bytes("", 0)); }
  sp_poly_coll_chk(v, "clear");
  sp_poly_hash_check_mutable(v);
  if (v.tag != SP_TAG_OBJ || !v.v.p) return v;
  /* a frozen Array raises, as the typed clear does; it was emptied */
  if (sp_poly_is_array_kind(v.cls_id) && sp_poly_array_frozen(v)) sp_raise_frozen_array_at(v.v.p, v.cls_id);
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:      ((sp_IntArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_FLT_ARRAY:      ((sp_FloatArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_STR_ARRAY:      ((sp_StrArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_POLY_ARRAY:     ((sp_PolyArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_PTR_ARRAY:      ((sp_PtrArray *)v.v.p)->len = 0; break;
    case SP_BUILTIN_STRBUF: {
      sp_String *_m = (sp_String *)v.v.p;
      if (sp_String_is_frozen(_m)) { sp_raise_frozen_str(_m->data); break; }
      _m->len = 0; _m->data[0] = 0; sp_fd_publish(_m);
      break;
    }
    case SP_BUILTIN_STR_INT_HASH:   sp_StrIntHash_clear((sp_StrIntHash *)v.v.p); break;
    case SP_BUILTIN_STR_STR_HASH:   sp_StrStrHash_clear((sp_StrStrHash *)v.v.p); break;
    case SP_BUILTIN_INT_STR_HASH:   sp_IntStrHash_clear((sp_IntStrHash *)v.v.p); break;
    case SP_BUILTIN_INT_INT_HASH:   sp_IntIntHash_clear((sp_IntIntHash *)v.v.p); break;
    case SP_BUILTIN_STR_POLY_HASH:  sp_StrPolyHash_clear((sp_StrPolyHash *)v.v.p); break;
    case SP_BUILTIN_SYM_POLY_HASH:  sp_SymPolyHash_clear((sp_SymPolyHash *)v.v.p); break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_clear((sp_PolyPolyHash *)v.v.p); break;
    default: sp_raise_nomethod(sp_nomethod_msg("clear", v)); break;
  }
  return v;
}

sp_RbVal sp_poly_slice_or_call(sp_RbVal v, sp_RbVal a, sp_RbVal b)
{
  /* a shared String handle receiver reads as its bytes; a capture name
     does below, while a callable's argument stays the String itself */
  if (sp_poly_is_strbuf(v)) v = sp_poly_strbuf_deref(v);
  /* `s[/re/, n]` on a boxed String: capture n of the first match, nil when
     there is none -- the form the typed emitter answers inline. Read as a
     two-integer slice, the Regexp operand raised TypeError (campfire's
     `response.headers["Link"][/<(.*)>/, 1]` off a Hash[String, String]). */
  if (v.tag == SP_TAG_STR && a.tag == SP_TAG_OBJ && a.v.p && a.cls_id == SP_BUILTIN_REGEX &&
      b.tag == SP_TAG_INT) {
    sp_int n = b.v.i;
    if (sp_re_match((mrb_regexp_pattern *)a.v.p, v.v.s ? v.v.s : "") < 0) return sp_box_nil();
    if (n == 0) return sp_box_nullable_str(sp_re_match_str);
    if (n >= 1 && n <= 9) return sp_box_nullable_str(sp_re_captures[n]);
    return sp_box_nil();
  }
  /* `s[/(?<x>..)/, "x"]` or `, :x`: the named capture of the first match,
     nil when there is none, as the typed emitter answers it */
  if (v.tag == SP_TAG_STR && a.tag == SP_TAG_OBJ && a.v.p && a.cls_id == SP_BUILTIN_REGEX &&
      (b.tag == SP_TAG_STR || b.tag == SP_TAG_SYM || sp_poly_is_strbuf(b))) {
    if (sp_poly_is_strbuf(b)) b = sp_poly_strbuf_deref(b);
    const char *nm = b.tag == SP_TAG_SYM ? sp_sym_to_s((sp_sym)b.v.i) : (b.v.s ? b.v.s : "");
    if (sp_re_match((mrb_regexp_pattern *)a.v.p, v.v.s ? v.v.s : "") < 0) return sp_box_nil();
    return sp_box_nullable_str(sp_re_named_capture((mrb_regexp_pattern *)a.v.p, nm));
  }
  /* a Method's [] too: its arguments of any kind are the call's, where
     sp_poly_slice took a String for an index and raised TypeError (#6179) */
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id == SP_BUILTIN_PROC || v.cls_id == SP_BUILTIN_CURRY || v.cls_id == SP_BUILTIN_METHOD)) {
    _sp_proc_poly_args[0] = a;
    _sp_proc_poly_args[1] = b;
    sp_int slots[16];
    slots[0] = sp_poly_slot_i(a);
    slots[1] = sp_poly_slot_i(b);
    return sp_poly_callable_call(v, 2, slots);
  }
  return sp_poly_slice(v, sp_poly_arg_int_chk(a), sp_poly_arg_int_chk(b));
}

/* Object's copy machinery allocates a receiver before initialize_copy fails.
   Do not copy a queue's synchronization state or return the original handle. */
sp_queue *sp_Queue_copy_error(sp_queue *q) {
  if (!q) return NULL;  /* a nullable typed receiver is nil, whose copy is nil */
  SP_GC_ROOT(q);
  sp_queue *copy = sp_Queue_max(q) > 0 ? sp_SizedQueue_new(sp_Queue_max(q)) : sp_Queue_new();
  SP_GC_ROOT(copy);
  sp_RbVal original = sp_box_obj(q, SP_BUILTIN_QUEUE);
  sp_raise_nomethod(sp_nomethod_msg_args("initialize_copy", sp_box_obj(copy, SP_BUILTIN_QUEUE), 1, &original));
  return NULL;
}

sp_RbVal sp_poly_dup(sp_RbVal v, int keep_frozen)
{
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_QUEUE && v.v.p)
    sp_Queue_copy_error((sp_queue *)v.v.p);
#ifdef SP_CEXT
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && sp_cext_data_class_fn && sp_cext_data_class_fn(v.cls_id))
    return sp_cext_duplicate(v, keep_frozen);
#endif
  if (sp_bsub_dup_hook && v.tag == SP_TAG_OBJ && v.v.p) {
    sp_bool handled = FALSE;
    sp_RbVal r = sp_bsub_dup_hook(v, keep_frozen, &handled);
    if (handled) return r;
  }
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id == SP_BUILTIN_RANGE || v.cls_id == SP_BUILTIN_FLOAT_RANGE || v.cls_id == SP_BUILTIN_STR_RANGE))
    return sp_range_dup(v, keep_frozen);
  /* a Time is a reference: its copy is a new Time a later localtime / utc
     changes apart */
  if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == SP_BUILTIN_TIME)
    return sp_box_time(sp_time_dup((sp_Time *)v.v.p));

  /* Hash#dup/#clone on a boxed hash: a shallow copy of the same variant. The
     hash went out as-is, so a `dup` taken to keep the caller's hash intact
     aliased it and every write through the copy landed in the original
     (#4646: a parameter typed poly by a recursive call cycle). */
  if (v.tag == SP_TAG_OBJ && v.v.p && sp_poly_is_hash_kind(v.cls_id)) {
    void *p = v.v.p; SP_GC_ROOT(p);
    switch (v.cls_id) {
      case SP_BUILTIN_STR_INT_HASH:  v.v.p = sp_StrIntHash_dup((sp_StrIntHash *)p); break;
      case SP_BUILTIN_STR_STR_HASH:  v.v.p = sp_StrStrHash_dup((sp_StrStrHash *)p); break;
      case SP_BUILTIN_INT_STR_HASH:  v.v.p = sp_IntStrHash_dup((sp_IntStrHash *)p); break;
      case SP_BUILTIN_INT_INT_HASH:  v.v.p = sp_IntIntHash_dup((sp_IntIntHash *)p); break;
      case SP_BUILTIN_STR_POLY_HASH: v.v.p = sp_StrPolyHash_dup((sp_StrPolyHash *)p); break;
      case SP_BUILTIN_SYM_POLY_HASH: v.v.p = sp_SymPolyHash_dup((sp_SymPolyHash *)p); break;
      case SP_BUILTIN_POLY_POLY_HASH: v.v.p = sp_PolyPolyHash_dup((sp_PolyPolyHash *)p); break;
    }
    if (keep_frozen && sp_gc_is_frozen(p)) sp_gc_freeze(v.v.p);
    return v;
  }
  /* Array#dup/#clone on a boxed array (read out of a poly container): a shallow
     copy of the same kind. A raw struct memcpy (the user-object path below)
     would share the element buffer, so the copy would alias -- mutating it
     would corrupt the original. */
  if (v.tag == SP_TAG_OBJ && v.v.p && sp_poly_is_array_kind(v.cls_id)) {
    switch (v.cls_id) {
      case SP_BUILTIN_POLY_ARRAY: v.v.p = sp_PolyArray_dup((sp_PolyArray *)v.v.p); break;
      case SP_BUILTIN_INT_ARRAY: {
        sp_IntArray *a = (sp_IntArray *)v.v.p; SP_GC_ROOT(a);
        sp_IntArray *r = sp_IntArray_new();
        SP_GC_ROOT(r);
        for (sp_int i = 0; i < a->len; i++) sp_IntArray_push_o(r, sp_IntArray_oget(a, i));
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
      case SP_BUILTIN_STR_ARRAY: {
        sp_StrArray *a = (sp_StrArray *)v.v.p; SP_GC_ROOT(a);
        sp_StrArray *r = sp_StrArray_new();
        for (sp_int i = 0; i < a->len; i++) sp_StrArray_push_e(r, a->data[i]);
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
      case SP_BUILTIN_FLT_ARRAY: {
        sp_FloatArray *a = (sp_FloatArray *)v.v.p; SP_GC_ROOT(a);
        sp_FloatArray *r = sp_FloatArray_new();
        SP_GC_ROOT(r);
        for (sp_int i = 0; i < a->len; i++) sp_FloatArray_push_o(r, sp_FloatArray_oget(a, i));
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
      case SP_BUILTIN_PTR_ARRAY: {
        /* a shallow copy of the same kind, its stamp carried over (#4486) */
        sp_PtrArray *a = (sp_PtrArray *)v.v.p; SP_GC_ROOT(a);
        sp_PtrArray *r = sp_PtrArray_new_scan(a->scan_elem);
        r->elem_kind = a->elem_kind; r->elem_cls = a->elem_cls;
        for (sp_int i = 0; i < a->len; i++) sp_PtrArray_push(r, a->data[i]);
        if (keep_frozen && a->frozen) r->frozen = 1;
        v.v.p = r; break;
      }
    }
    return v;
  }
  /* IO#dup/#clone on a boxed IO: a handle on its own descriptor */
  if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == SP_BUILTIN_IO) {
    v.v.p = sp_File_dup((sp_File *)v.v.p, keep_frozen);
    return v;
  }
  /* Dir#dup on a boxed Dir: a distinct Dir with no stream and no path */
  if (!keep_frozen && v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == SP_BUILTIN_DIR) {
    v.v.p = sp_dir_dup_handle((sp_Dir *)v.v.p);
    return v;
  }
  /* OpenStruct#dup/#clone on a boxed OpenStruct: a copy over its own member
     table. Its cls_id is a builtin's, so the user-object copy below skips it
     and the original came back. */
  if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == SP_BUILTIN_OPENSTRUCT) {
    v.v.p = sp_OpenStruct_dup((sp_OpenStruct *)v.v.p, keep_frozen);
    return v;
  }
  /* String#dup on a boxed string: a copy, unfrozen unless kept. Handed back
     as-is, a `(fmt || FORMAT).dup` aliased the frozen constant and the next
     gsub! on the "copy" raised FrozenError (Benchmark::Tms#format). */
  if (v.tag == SP_TAG_STR && v.v.s) {
    const char *src = v.v.s;
    const char *d = sp_str_dup(src);
    if (keep_frozen && sp_str_is_frozen_val(src)) d = sp_str_freeze_val(d);
    v.v.s = d;
    return v;
  }
  /* String#dup/#clone on a boxed shared handle (#6179): a new handle over a
     copy of the bytes, its ASCII-8BIT tag kept and its frozen flag only for
     clone. Its cls_id is a builtin's, so the user-object copy below skipped
     it and the handle itself came back: an append to the "copy" landed in
     the original, and `clone(freeze: false)` of a frozen String stayed
     frozen. */
  if (v.tag == SP_TAG_OBJ && v.v.p && v.cls_id == SP_BUILTIN_STRBUF) {
    sp_String *h = (sp_String *)v.v.p; SP_GC_ROOT(h);
    sp_String *n = sp_String_new_unfrozen(sp_String_cstr(h));
    if (keep_frozen && sp_String_is_frozen(h)) sp_String_freeze(n);
    v.v.p = n;
    return v;
  }
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id >= 0 || v.cls_id == SP_BUILTIN_OBJECT)) {
    sp_gc_hdr *h = (sp_gc_hdr *)((char *)v.v.p - sizeof(sp_gc_hdr));
    size_t payload = h->size - sizeof(sp_gc_hdr);
    void *src = v.v.p;
    SP_GC_ROOT(src);
    void *n = sp_gc_alloc(payload, h->finalize, h->scan);
    memcpy(n, src, payload);
    /* a bare Object's ivars are its own table: the copy takes a copy */
    if (v.cls_id == SP_BUILTIN_OBJECT && ((sp_Object *)n)->ivars) {
      SP_GC_ROOT(n);
      sp_SymPolyHash *t = sp_SymPolyHash_dup(((sp_Object *)n)->ivars);
      sp_gc_wb(n);   /* the copy can collect and promote the rooted object */
      ((sp_Object *)n)->ivars = t;
    }
    if (sp_user_init_copy_hook) { SP_GC_ROOT(n); sp_RbVal r = v; r.v.p = n; sp_user_init_copy_hook(r, v); }
    if (keep_frozen && h->frozen)
      ((sp_gc_hdr *)((char *)n - sizeof(sp_gc_hdr)))->frozen = 1;
    v.v.p = n;
  }
  return v;
}

/* Each pair of src stored into dst as a boxed `dst[k] = v` stores it
   (sp_poly_set_poly): a typed variant takes a String key or value that comes
   as a shared handle by its contents, and refuses a key or value of another
   kind. Its own switch tested only the byte tag, so a String value read from
   a String-valued source as its handle was refused as no String
   (`h.merge!(*[{"c" => "d"}, {"e" => "f"}])`). */
void sp_poly_hash_merge_into(sp_RbVal dst, sp_RbVal src)
{
  if (dst.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(dst.cls_id)) return;
  if (src.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(src.cls_id)) return;
  SP_GC_ROOT_RBVAL(dst); SP_GC_ROOT_RBVAL(src);
  sp_int n = sp_poly_length(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(src, i, &k, &v);
    sp_poly_set_poly(dst, k, v);
  }
}

sp_RbVal sp_kw_splat_check(sp_RbVal h, const char *const *mem, int n, const unsigned char *lit, int is_data)
{
  SP_GC_ROOT_RBVAL(h);
  char buf[1024]; size_t len = 0; int cnt = 0;
  buf[0] = 0;
  sp_int nk = sp_poly_length(h);
  if (is_data) {
    for (sp_int j = 0; j < nk; j++) {
      sp_RbVal k, v;
      sp_poly_hash_pair(h, j, &k, &v);
      sp_poly_to_name(k);
    }
    for (int i = 0; i < n; i++) {
      if (lit && lit[i]) continue;
      int f = 0;
      for (sp_int j = 0; j < nk && !f; j++) {
        sp_RbVal k, v;
        sp_poly_hash_pair(h, j, &k, &v);
        f = strcmp(sp_kw_key_name(k), mem[i]) == 0;
      }
      if (f) continue;
      if (len < sizeof buf) len += (size_t)snprintf(buf + len, sizeof buf - len, "%s:%s", cnt ? ", " : "", mem[i]);
      cnt++;
    }
    if (cnt) sp_raise_kw_error("missing", cnt, buf);
  }
  sp_PolyPolyHash *named = NULL;
  SP_GC_ROOT(named);
  for (sp_int j = 0; j < nk && !is_data && !named; j++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(h, j, &k, &v);
    if (!sp_kw_key_name(k)) named = sp_PolyPolyHash_new();
  }
  for (sp_int j = 0; j < nk; j++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(h, j, &k, &v);
    const char *kn = sp_kw_key_name(k);
    int at = -1;
    sp_int idx = 0;
    for (int i = 0; kn && i < n && at < 0; i++) if (strcmp(kn, mem[i]) == 0) at = i;
    if (!kn) at = sp_kw_key_pos(k, n, &idx);
    if (at >= 0) {
      if (named) sp_PolyPolyHash_set(named, sp_box_sym(sp_sym_intern(mem[at])), v);
      continue;
    }
    if (len < sizeof buf) len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", cnt ? ", " : "",
                                                  is_data ? sp_poly_inspect(k) : kn ? kn : sp_int_to_s(idx));
    cnt++;
  }
  /* a Struct's own wording names its keys bare and always plural
     (rb_struct_initialize_m) */
  if (cnt && is_data) sp_raise_kw_error("unknown", cnt, buf);
  if (cnt) sp_raise_cls("ArgumentError", sp_sprintf("unknown keywords: %s", buf));
  return named ? sp_box_obj(named, SP_BUILTIN_POLY_POLY_HASH) : h;
}

sp_RbVal sp_poly_to_h_m(sp_RbVal v)
{
  if (v.tag == SP_TAG_NIL) return sp_box_obj(sp_SymPolyHash_new(), SP_BUILTIN_SYM_POLY_HASH);
  if (v.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(v.cls_id)) return v;
  /* an OpenStruct (an OpenStruct|nil union reaches here boxed): its member
     table is already a symbol-keyed hash (#3282) */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_OPENSTRUCT)
    return sp_box_obj(sp_OpenStruct_to_h((sp_OpenStruct *)v.v.p), SP_BUILTIN_SYM_POLY_HASH);
  /* a Struct/Data read out of a container: dispatch its symbol-keyed to_h by
     cls_id through the generated hook (#2906). */
  if (v.tag == SP_TAG_OBJ && sp_obj_to_h_fn) {
    sp_RbVal h = sp_obj_to_h_fn(v);
    if (h.tag == SP_TAG_OBJ) return h;
  }
  /* an array of [k, v] pairs -> a hash keyed by whatever the pairs hold: a
     Symbol-keyed one where every key is a Symbol (the Hash#partition
     sub-array and Enumerable pair lists this was written for), the general
     boxed hash otherwise -- reading an Integer key as a symbol id built a
     hash whose keys were other programs' symbols (#3972) */
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) {
    sp_int n = sp_poly_length(v);
    int all_sym = 1;
    for (sp_int i = 0; i < n && all_sym; i++) {
      sp_RbVal pair = sp_poly_arr_get(v, i);
      /* CRuby's messages, as sp_poly_to_h_val raises them */
      if (!(pair.tag == SP_TAG_OBJ && sp_poly_is_array_kind(pair.cls_id)))
        sp_raise_cls("TypeError", sp_sprintf("wrong element type %s at %lld (expected array)",
                                          sp_poly_class_name(pair), (long long)i));
      if (sp_poly_length(pair) != 2)
        sp_raise_cls("ArgumentError", sp_sprintf("wrong array length at %lld (expected 2, was %lld)",
                                              (long long)i, (long long)sp_poly_length(pair)));
      if (sp_poly_arr_get(pair, 0).tag != SP_TAG_SYM) all_sym = 0;
    }
    if (!all_sym) {
      sp_PolyPolyHash *ph = sp_PolyPolyHash_new();
      SP_GC_ROOT(ph);
      for (sp_int i = 0; i < n; i++) {
        sp_RbVal pair = sp_poly_arr_get(v, i);
        sp_PolyPolyHash_set(ph, sp_poly_arr_get(pair, 0), sp_poly_arr_get(pair, 1));
      }
      return sp_box_obj(ph, SP_BUILTIN_POLY_POLY_HASH);
    }
    sp_SymPolyHash *h = sp_SymPolyHash_new();
    SP_GC_ROOT(h);
    for (sp_int i = 0; i < n; i++) {
      sp_RbVal pair = sp_poly_arr_get(v, i);
      sp_RbVal k = sp_poly_arr_get(pair, 0);
      sp_SymPolyHash_set(h, (sp_sym)k.v.i, sp_poly_arr_get(pair, 1));
    }
    return sp_box_obj(h, SP_BUILTIN_SYM_POLY_HASH);
  }
  /* an Integer Range has Enumerable#to_h: its elements are no pairs, so a
     non-empty one is CRuby's TypeError, and an empty one is {} */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE && v.v.p) {
    if (sp_range_count(*(sp_Range *)v.v.p) <= 0) return sp_box_obj(sp_SymPolyHash_new(), SP_BUILTIN_SYM_POLY_HASH);
    sp_raise_cls("TypeError", "wrong element type Integer (expected array)");
  }
  /* a String Range likewise, its elements Strings */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STR_RANGE && v.v.p) {
    if (!sp_srange_min_v(SP_SRANGE_OF(v.v.p))) return sp_box_obj(sp_SymPolyHash_new(), SP_BUILTIN_SYM_POLY_HASH);
    sp_raise_cls("TypeError", "wrong element type String (expected array)");
  }
  sp_raise_nomethod(sp_nomethod_msg("to_h", v));  /* "for true", "for an instance of String", as CRuby words it */
}

void sp_kwargs_verify_at(sp_RbVal h, const char *const *allowed, const char *const *required, const char *const *lit, const char *const *unk, int nbefore, int check_unknown)
{
  sp_PolyArray *k = sp_poly_length(h) > 0 ? sp_poly_keys(h) : sp_PolyArray_new(); SP_GC_ROOT(k);
  char list[256]; int n = 0, cnt = 0;
  list[0] = 0;
  for (const char *const *r = required; *r; r++) {
    int found = sp_kwargs_name_in(*r, lit);
    for (sp_int i = 0; i < k->len && !found; i++)
      if (k->data[i].tag == SP_TAG_SYM && !strcmp(sp_sym_to_s((sp_sym)k->data[i].v.i), *r)) found = 1;
    if (!found) sp_kwargs_list_add(list, &n, &cnt, sp_sprintf(":%s", *r));
  }
  if (cnt) sp_raise_kw_error("missing", cnt, list);
  if (!check_unknown) return;
  int nunk = 0;
  for (const char *const *u = unk; u && *u; u++) nunk++;
  int nb = nbefore < 0 || nbefore > nunk ? nunk : nbefore;
  for (int u = 0; u < nb; u++) sp_kwargs_list_add(list, &n, &cnt, unk[u]);
  for (const char *const *l = lit; !unk && *l; l++)
    if (!sp_kwargs_name_in(*l, allowed)) sp_kwargs_list_add(list, &n, &cnt, sp_sprintf(":%s", *l));
  for (sp_int i = 0; i < k->len; i++) {
    const char *iv = sp_poly_inspect(k->data[i]);
    int later = 0;   /* a key written after the `**` too: named here, where the `**` holds it */
    for (int u = nb; u < nunk && !later; u++) later = !strcmp(iv, unk[u]);
    if (k->data[i].tag == SP_TAG_SYM) {
      const char *nm = sp_sym_to_s((sp_sym)k->data[i].v.i);
      if (sp_kwargs_name_in(nm, allowed) || (!later && sp_kwargs_name_in(nm, lit))) continue;
    }
    int ahead = 0;   /* a literal key's ahead of the `**`, named already */
    for (int u = 0; u < nb && !ahead; u++) ahead = !strcmp(iv, unk[u]);
    if (ahead) continue;
    sp_kwargs_list_add(list, &n, &cnt, iv);
  }
  for (int u = nb; u < nunk; u++) {
    int in_hash = 0;
    for (sp_int i = 0; i < k->len && !in_hash; i++) in_hash = !strcmp(sp_poly_inspect(k->data[i]), unk[u]);
    if (!in_hash) sp_kwargs_list_add(list, &n, &cnt, unk[u]);
  }
  if (cnt) sp_raise_kw_error("unknown", cnt, list);
}

void sp_kwrest_merge_poly(sp_SymPolyHash *dst, sp_RbVal h)
{
  SP_GC_ROOT(dst);
  h = sp_kw_splat_conv(h, 0);
  if (h.tag == SP_TAG_NIL) return;
  SP_GC_ROOT_RBVAL(h);
  sp_int n = sp_poly_length(h);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(h, i, &k, &v);
    if (k.tag != SP_TAG_SYM) sp_poly_typed_hash_store_miss(k, v, "Symbol", NULL);
    sp_SymPolyHash_set(dst, (sp_sym)k.v.i, v);
  }
}

void sp_kw_merge_any(sp_PolyPolyHash *dst, sp_RbVal h)
{
  SP_GC_ROOT(dst);
  h = sp_kw_splat_conv(h, 0);
  if (h.tag == SP_TAG_NIL) return;
  SP_GC_ROOT_RBVAL(h);
  sp_int n = sp_poly_length(h);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(h, i, &k, &v);
    sp_PolyPolyHash_set(dst, k, v);
  }
}

sp_PolyPolyHash*sp_PolyPolyHash_dup(sp_PolyPolyHash*h)
{if(!h)return NULL;SP_GC_ROOT(h);sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;r->ident=h->ident;for(sp_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,h->keys[h->order[i]],h->vals[h->order[i]]);return r;}

const char*sp_PolyPolyHash_inspect(sp_PolyPolyHash*h)
{return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_POLY_POLY_HASH)):SPL("nil");}

sp_PolyPolyHash *sp_poly_hash_merge(sp_RbVal a, sp_RbVal b)
{
  /* Before the allocation, not after: the operands are read for the whole
     loop below and the new hash is the first thing that can collect them.
     Copying them into hs[] does not root them -- the collector walks the
     registered root slots, not the stack. */
  SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b);
  sp_PolyPolyHash *r = sp_PolyPolyHash_new();
  SP_GC_ROOT(r);
  /* merge inherits the receiver's default; cross-layout receivers arrive boxed */
  if (a.tag == SP_TAG_OBJ && a.v.p) {
    switch (a.cls_id) {
      case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *h = (sp_StrIntHash *)a.v.p; r->default_v = h->default_nil ? sp_box_nil() : sp_box_int(h->default_v); break; }
      case SP_BUILTIN_STR_STR_HASH: r->default_v = SP_SEBOXN(((sp_StrStrHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_INT_STR_HASH: r->default_v = SP_SEBOXN(((sp_IntStrHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_INT_INT_HASH: { sp_IntIntHash *h = (sp_IntIntHash *)a.v.p; r->default_v = h->default_nil ? sp_box_nil() : sp_box_int(h->default_v); break; }
      case SP_BUILTIN_STR_POLY_HASH: r->default_v = ((sp_StrPolyHash *)a.v.p)->default_v; break;
      case SP_BUILTIN_SYM_POLY_HASH: r->default_v = ((sp_SymPolyHash *)a.v.p)->default_v; break;
      case SP_BUILTIN_POLY_POLY_HASH: r->default_v = ((sp_PolyPolyHash *)a.v.p)->default_v; break;
      default: break;
    }
    int has_dproc = 0;
    switch (a.cls_id) {
      case SP_BUILTIN_STR_POLY_HASH: has_dproc = ((sp_StrPolyHash *)a.v.p)->dproc != NULL; break;
      case SP_BUILTIN_SYM_POLY_HASH: has_dproc = ((sp_SymPolyHash *)a.v.p)->dproc != NULL; break;
      case SP_BUILTIN_POLY_POLY_HASH: has_dproc = ((sp_PolyPolyHash *)a.v.p)->dproc != NULL; break;
      default: break;
    }
    if (has_dproc) {
      sp_poly_hash_dproc_ctx *ctx = (sp_poly_hash_dproc_ctx *)sp_gc_alloc(
          sizeof(*ctx), NULL, sp_poly_hash_dproc_ctx_scan);
      ctx->source = a;
      r->dproc = sp_poly_hash_dproc_bridge;
      sp_gc_wb((void *)r);   /* the context's allocation can promote the rooted hash */
      r->dproc_self = ctx;
    }
  }
  sp_RbVal hs[2]; hs[0] = a; hs[1] = b;
  for (int h = 0; h < 2; h++) {
    if (hs[h].tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(hs[h].cls_id)) continue;
    sp_PolyArray *pairs = sp_poly_to_a_arr(hs[h]);
    SP_GC_ROOT(pairs);
    for (sp_int i = 0; pairs && i < pairs->len; i++) {
      sp_RbVal pair = pairs->data[i];
      sp_PolyPolyHash_set(r, sp_poly_arr_get(pair, 0), sp_poly_arr_get(pair, 1));
    }
  }
  return r;
}

sp_RbVal sp_poly_hash_slice(sp_RbVal v, int n, sp_RbVal *keys)
{
  /* The receiver is read once per key while the loop below allocates the
     result and grows it, and it is a temporary whenever the call chains off
     one (`poly(1).slice(*keys)`). Unrooted, it was collected part way through
     and the remaining keys missed. */
  SP_GC_ROOT_RBVAL(v);
  sp_PolyPolyHash *h = sp_PolyPolyHash_new();
  SP_GC_ROOT(h);
  for (int i = 0; i < n; i++)
    if (sp_poly_has_key(v, keys[i]))
      sp_PolyPolyHash_set(h, keys[i], sp_poly_index_poly(v, keys[i]));
  return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
}

void sp_poly_hash_writeback_ex(sp_RbVal orig, sp_PolyPolyHash *work, int with_default)
{
  if (orig.tag != SP_TAG_OBJ || !work || !orig.v.p) return;
  if (orig.cls_id == SP_BUILTIN_POLY_POLY_HASH) return;
  if (!sp_poly_is_hash_kind(orig.cls_id)) return;
  if (sp_gc_is_frozen(orig.v.p)) sp_raise_frozen_hash_at(orig.v.p, orig.cls_id);
  SP_GC_ROOT_RBVAL(orig); SP_GC_ROOT(work);
  for (sp_int i = 0; i < work->len; i++) {
    sp_int j = work->order[i];
    sp_RbVal k = work->keys[j], v = work->vals[j];
    switch (orig.cls_id) {
      case SP_BUILTIN_STR_INT_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); sp_hash_wb_want(v, SP_TAG_INT, 1, "value", "Integer values"); break;   /* a nil value is kept (D3b-ii) */
      case SP_BUILTIN_STR_STR_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); sp_hash_wb_want(v, SP_TAG_STR, 1, "value", "String values"); break;
      case SP_BUILTIN_INT_STR_HASH: sp_hash_wb_want(k, SP_TAG_INT, 0, "key", "Integer keys"); sp_hash_wb_want(v, SP_TAG_STR, 1, "value", "String values"); break;
      case SP_BUILTIN_INT_INT_HASH: sp_hash_wb_want(k, SP_TAG_INT, 0, "key", "Integer keys"); sp_hash_wb_want(v, SP_TAG_INT, 1, "value", "Integer values"); break;
      case SP_BUILTIN_STR_POLY_HASH: sp_hash_wb_want(k, SP_TAG_STR, 0, "key", "String keys"); break;
      case SP_BUILTIN_SYM_POLY_HASH: sp_hash_wb_want(k, SP_TAG_SYM, 0, "key", "Symbol keys"); break;
      default: return;
    }
  }
  /* The default too, when the mutator may have set one (`with_default`: a
     boxed `h.default = v` set it on the copy; a `replace` keeps the
     receiver's own, as the typed emitter does). An Integer- or
     String-valued variant holds one of its own kind (or nil); the Symbol-
     and String-keyed poly variants hold any value. A default PROC set
     through the box (a PolyPolyHash body) has no slot of its signature in
     a typed variant: refused, like a key of the wrong kind, rather than
     dropped. The bridge is the original's own proc, installed on the copy
     by sp_poly_hash_merge, and means no change. */
  if (with_default) {
    if (work->dproc && work->dproc != sp_poly_hash_dproc_bridge)
      sp_raise_cls("TypeError", sp_sprintf("can't store a default proc in a %s through a boxed receiver",
                                           sp_poly_class_name(orig)));
    switch (orig.cls_id) {
      case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_INT_INT_HASH: sp_hash_wb_want(work->default_v, SP_TAG_INT, 1, "default", "Integer values"); break;
      case SP_BUILTIN_STR_STR_HASH: case SP_BUILTIN_INT_STR_HASH: sp_hash_wb_want(work->default_v, SP_TAG_STR, 1, "default", "String values"); break;
      default: break;
    }
  }
  switch (orig.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: {
      sp_StrIntHash *h = (sp_StrIntHash *)orig.v.p;
      sp_StrIntHash_clear(h);
      if (with_default) { h->default_nil = work->default_v.tag == SP_TAG_NIL; h->default_v = h->default_nil ? 0 : sp_poly_to_i(work->default_v); }
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrIntHash_oset(h, sp_poly_to_s(work->keys[j]), sp_poly_hval_oi(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_STR_STR_HASH: {
      sp_StrStrHash *h = (sp_StrStrHash *)orig.v.p;
      sp_StrStrHash_clear(h);
      if (with_default) h->default_v = SP_SEF(sp_poly_to_s_or_nil(work->default_v));
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrStrHash_set_e(h, sp_poly_to_s(work->keys[j]), SP_SEF(sp_poly_to_s_or_nil(work->vals[j])));
      }
      return;
    }
    case SP_BUILTIN_INT_STR_HASH: {
      sp_IntStrHash *h = (sp_IntStrHash *)orig.v.p;
      sp_IntStrHash_clear(h);
      if (with_default) h->default_v = SP_SEF(sp_poly_to_s_or_nil(work->default_v));
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_IntStrHash_set_e(h, sp_poly_to_i(work->keys[j]), SP_SEF(sp_poly_to_s_or_nil(work->vals[j])));
      }
      return;
    }
    case SP_BUILTIN_INT_INT_HASH: {
      sp_IntIntHash *h = (sp_IntIntHash *)orig.v.p;
      sp_IntIntHash_clear(h);
      if (with_default) { h->default_nil = work->default_v.tag == SP_TAG_NIL; h->default_v = h->default_nil ? 0 : sp_poly_to_i(work->default_v); }
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_IntIntHash_oset(h, sp_poly_to_i(work->keys[j]), sp_poly_hval_oi(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_STR_POLY_HASH: {
      sp_StrPolyHash *h = (sp_StrPolyHash *)orig.v.p;
      sp_StrPolyHash_clear(h);
      if (with_default) h->default_v = work->default_v;
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrPolyHash_set(h, sp_poly_to_s(work->keys[j]), work->vals[j]);
      }
      return;
    }
    case SP_BUILTIN_SYM_POLY_HASH: {
      sp_SymPolyHash *h = (sp_SymPolyHash *)orig.v.p;
      sp_SymPolyHash_clear(h);
      if (with_default) h->default_v = work->default_v;
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_SymPolyHash_set(h, (sp_sym)work->keys[j].v.i, work->vals[j]);
      }
      return;
    }
    default: return;
  }
}

void sp_poly_hash_writeback(sp_RbVal orig, sp_PolyPolyHash *work)
{
  sp_poly_hash_writeback_ex(orig, work, 1);
}

sp_RbVal sp_builtin_class_new(int kind, sp_int argc, const sp_RbVal *av, sp_Proc *blk)
{
  SP_GC_ROOT(blk);
  switch (kind) {
  case 'S': {
    sp_dyn_new_arity(argc, 1);
    if (argc == 0) return sp_box_str(sp_str_dup_external((&("\xff")[1])));
    sp_RbVal s = sp_poly_is_strbuf(av[0]) ? sp_poly_strbuf_deref(av[0]) : av[0];
    if (s.tag != SP_TAG_STR)
      sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into String", sp_poly_class_name(s)));
    return sp_box_str(sp_str_dup(s.v.s));
  }
  case 'A': {
    sp_dyn_new_arity(argc, 2);
    sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
    if (argc == 0) return sp_box_poly_array(r);
    if (argc == 1 && av[0].tag == SP_TAG_OBJ && sp_poly_is_array_kind(av[0].cls_id)) {
      sp_int n = sp_poly_length(av[0]);
      for (sp_int i = 0; i < n; i++) sp_PolyArray_push(r, sp_poly_arr_get(av[0], i));
      return sp_box_poly_array(r);
    }
    if (av[0].tag != SP_TAG_INT)
      sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into Integer", sp_poly_class_name(av[0])));
    if (av[0].v.i < 0) sp_raise_cls("ArgumentError", "negative array size");
    for (sp_int i = 0; i < av[0].v.i; i++)
      sp_PolyArray_push(r, blk ? sp_penum_call1(blk, sp_box_int(i)) : argc == 2 ? av[1] : sp_box_nil());
    return sp_box_poly_array(r);
  }
  case 'H': {
    sp_dyn_new_arity(argc, blk ? 0 : 1);
    sp_PolyPolyHash *h = blk ? sp_PolyPolyHash_new_dproc(sp_dyn_hash_dproc, blk)
                       : argc ? sp_PolyPolyHash_new_with_default(av[0]) : sp_PolyPolyHash_new();
    return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
  }
  default:
    sp_dyn_new_arity(argc, 0);
    return sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT);
  }
}

/* A case/in that no arm took, or a rightward `expr => pat` that failed:
   NoMatchingPatternError with the subject's inspect. A single pattern also
   says why it failed (`detail`), as CRuby does: "[1, 2]: [1, 2] length
   mismatch (given 2, expected 1)". A key the hash lacks is the
   NoMatchingPatternKeyError subclass. */
void sp_pm_raise_nomatch(sp_RbVal subj, const char *detail, int key_err) {
  SP_GC_ROOT_RBVAL(subj); SP_GC_ROOT_STR(detail);
  const char *s = sp_poly_inspect(subj); SP_GC_ROOT_STR(s);
  sp_raise_cls(key_err ? "NoMatchingPatternKeyError" : "NoMatchingPatternError",
               detail ? sp_sprintf("%s: %s", s, detail) : s);
}
/* `pat === v` was false; pat is the pattern's inspect */
/* sp_sprintf renders a long message again from its arguments after it
   allocates, so every inspect it is handed is rooted first */
const char *sp_pm_eqq_msg(const char *pat, sp_RbVal v) {
  SP_GC_ROOT_STR(pat);
  const char *s = sp_poly_inspect(v); SP_GC_ROOT_STR(s);
  return sp_sprintf("%s === %s does not return true", pat, s);
}
/* `fmt`, a message with one %s, about v's inspect */
const char *sp_pm_inspect_msg(const char *fmt, sp_RbVal v) {
  SP_GC_ROOT_RBVAL(v);
  const char *s = sp_poly_inspect(v); SP_GC_ROOT_STR(s);
  return sp_sprintf(fmt, s);
}
/* an array pattern of `want` elements (at least that many with a rest) */
const char *sp_pm_len_msg(sp_RbVal arr, sp_int want, int at_least) {
  SP_GC_ROOT_RBVAL(arr);
  const char *s = sp_poly_inspect(arr); SP_GC_ROOT_STR(s);
  return sp_sprintf("%s length mismatch (given %lld, expected %lld%s)", s,
                    (long long)sp_poly_length(arr), (long long)want, at_least ? "+" : "");
}

/* format(*args) with boxed arguments (spinel_rt.h) */
const char *sp_str_format_polyarr(const char *fmt, sp_PolyArray *a) {
  size_t cap = strlen(fmt) + 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t out = 0; sp_int idx = 0; const char *p = fmt;
  /* CRuby refuses to mix numbered (%1$s), sequential (%s) and named (%<a>d)
     references in one format string (#3723) */
  sp_bool used_numbered = FALSE, used_sequential = FALSE, used_named = FALSE;
  while (*p) {
    if (*p != '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = *p++; continue;
    }
    if (p[1] == '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = '%'; p += 2; continue;
    }
    char spec[64]; size_t sl = 0; spec[sl++] = *p++;
    sp_bool this_numbered = FALSE;
    /* positional argument reference: %N$conv selects the Nth (1-based) arg */
    {
      const char *q = p; sp_int argnum = 0; sp_bool overflow = FALSE;
      while (*q >= '0' && *q <= '9') {
        if (sp_int_mul_overflow_p(argnum, 10, &argnum) ||
            sp_int_add_overflow_p(argnum, *q - '0', &argnum)) { overflow = TRUE; break; }
        q++;
      }
      if (!overflow && argnum > 0 && *q == '$') {
        idx = argnum - 1; p = q + 1;
        if (used_sequential || used_named) { free(buf); sp_raise_cls("ArgumentError", "numbered(1) after unnumbered(1)"); }
        used_numbered = TRUE; this_numbered = TRUE;
      }
    }
    /* %<name>conv / %{name}: named reference into the format's hash argument,
       which may stand anywhere among the flags and width (`%-5<s>s`,
       `%0<n>5d`). %<name> substitutes the value for the conversion that
       follows; %{name} is the conversion itself, the value's to_s, padded as
       a %s when flags came before it. */
    sp_RbVal named_v = sp_box_nil(); sp_bool have_named = FALSE, brace_named = FALSE;
    while (*p && sl < sizeof(spec) - 8) {
      char c = *p;
      if (c == '*') {
        /* dynamic width / precision: the next positional argument supplies the
           number (a negative width left-justifies, like printf) */
        if (idx >= a->len) { free(buf); sp_raise_cls("ArgumentError", "too few arguments"); }
        sp_RbVal wv = a->data[idx]; idx++;
        if (wv.tag == SP_TAG_FLT && !(wv.v.f >= (sp_float)INTPTR_MIN && wv.v.f < -(sp_float)INTPTR_MIN)) {
          free(buf); sp_float_arg_check(wv.v.f);
        }
        long long wnum = (wv.tag == SP_TAG_INT) ? (long long)wv.v.i
                       : (wv.tag == SP_TAG_FLT) ? (long long)wv.v.f : 0;
        /* an Integer past what a C int holds is CRuby's RangeError, not a size */
        if (wv.tag == SP_TAG_INT && (wnum > INT32_MAX || wnum < INT32_MIN)) {
          free(buf);
          sp_raise_cls("RangeError", sp_sprintf("integer %lld too %s to convert to 'int'",
                                                wnum, wnum < 0 ? "small" : "big"));
        }
        /* a negative PRECISION is ignored (a negative width left-justifies) */
        if (wnum < 0 && sl > 0 && spec[sl - 1] == '.') { sl--; p++; continue; }
        char wbuf[24]; int wl = snprintf(wbuf, sizeof wbuf, "%lld", wnum);
        if (sl + (size_t)wl < sizeof(spec) - 8) { memcpy(spec + sl, wbuf, (size_t)wl); sl += (size_t)wl; }
        p++;
      }
      else if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '.' || (c >= '0' && c <= '9')) { spec[sl++] = c; p++; }
      else if ((c == '<' || c == '{') && !have_named) {
        char nclose = (c == '<') ? '>' : '}';
        char nm[64]; size_t nl = 0; const char *q = p + 1;
        while (*q && *q != nclose && nl < sizeof nm - 1) nm[nl++] = *q++;
        nm[nl] = 0;
        if (*q != nclose) break;
        p = q + 1;
        if (used_numbered || used_sequential) { free(buf); sp_raise_cls("ArgumentError", sp_sprintf("named%c%s%c after unnumbered(1)", c, nm, nclose)); }
        named_v = sp_fmt_named_ref(a, nm, nclose, buf);
        have_named = TRUE;
        used_named = TRUE;
        if (nclose == '}') { brace_named = TRUE; break; }
      }
      else break;
    }
    if (brace_named && sl == 0) {
      const char *sv2 = sp_poly_to_s(named_v);
      size_t svl = sv2 ? strlen(sv2) : 0;
      if (out + svl + 1 >= cap) { cap = (out + svl) * 2 + 64; buf = (char *)realloc(buf, cap); }
      if (svl) memcpy(buf + out, sv2, svl);
      out += svl;
      continue;
    }
    /* a trailing bare `%` is a malformed format, not a literal (#3723) */
    if (!brace_named && !*p) { free(buf); sp_raise_cls("ArgumentError", "incomplete format specifier; use %% (double %) instead"); }
    char conv = brace_named ? 's' : *p++; spec[sl++] = conv;
    /* Ruby's %u is %d: it prints a negative as -N rather than wrapping */
    if (conv == 'u') { conv = 'd'; spec[sl - 1] = 'd'; }
    char fmt_use[80];
    if (conv == 'd' || conv == 'i') {
      memcpy(fmt_use, spec, sl - 1);
      fmt_use[sl - 1] = 'l'; fmt_use[sl] = 'l'; fmt_use[sl + 1] = conv; fmt_use[sl + 2] = 0;
    }
else {
      memcpy(fmt_use, spec, sl); fmt_use[sl] = 0;
    }
    char tmp[256]; int wn = 0;
    char *wide = NULL;   /* a field longer than tmp, formatted again into the heap */
    sp_RbVal v;
    if (have_named) v = named_v;
    else {
      /* CRuby raises rather than padding a missing argument with nil/0 */
      if (idx >= a->len) { free(buf); sp_raise_cls("ArgumentError", "too few arguments"); }
      if (!this_numbered) {
        if (used_named) { free(buf); sp_raise_cls("ArgumentError", "unnumbered(1) mixed with named"); }
        if (used_numbered) { free(buf); sp_raise_cls("ArgumentError", "unnumbered(1) mixed with numbered"); }
        used_sequential = TRUE;
      }
      v = a->data[idx]; idx++;
    }
    /* A mutable String arrives as a shared handle, not a plain String:
       `%c` of one printed nothing and `%d` and `%f` refused it as no String,
       where CRuby reads the String. Every conversion reads the handle as its
       value, as %s did through sp_poly_to_s. */
    if (sp_poly_is_strbuf(v)) v = sp_poly_strbuf_deref(v);
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o' ||
        conv == 'b' || conv == 'B') {
      long long lv = 0;
      /* A BIGNUM does not fit the long long the conversions below format, and
         truncating it printed 0 for every value past 64 bits. Render its own
         digits in the requested base and pass them through the spec as a
         string, which keeps the width and left-justify flags (#4010). */
      if (v.tag == SP_TAG_BIGINT && v.v.p) {
        sp_int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o') ? 8
                    : (conv == 'b' || conv == 'B') ? 2 : 10;
        const char *bs = sp_bigint_to_s_base((sp_Bigint *)v.v.p, base);
        if (!bs) bs = "0";
        /* a non-decimal base goes through the binary formatter: a negative
           one is shown as its two's complement, as an Integer's is */
        if (base != 10) {
          wn = sp_fmt_binary_big(spec, sl, conv, bs, tmp, sizeof(tmp));
          for (size_t wsz = sizeof(tmp) * 2; wn >= (int)(wsz / 2) && wsz <= ((size_t)1 << 30); wsz *= 2) {
            char *w2 = (char *)realloc(wide, wsz);
            if (!w2) { wn = (int)sizeof(tmp); free(wide); wide = NULL; break; }
            wide = w2;
            wn = sp_fmt_binary_big(spec, sl, conv, bs, wide, wsz);
          }
        }
        else {
        /* The sign and base flags belong to the digits, not to the %s that
           carries the width: compose them in, then keep only the width and
           left-justify flags in the spec printf sees. */
        int neg_b = bs[0] == '-';
        char pre[8]; int pl = 0;
        if (!neg_b) for (int fi = 1; fi < sl - 1; fi++) {
          if (spec[fi] == '+') { pre[pl++] = '+'; break; }
          if (spec[fi] == ' ') { pre[pl++] = ' '; break; }
          if (spec[fi] != '-' && spec[fi] != '#' && spec[fi] != '0') break;
        }
        for (int fi = 1; fi < sl - 1; fi++) {
          if (spec[fi] == '#') {
            if (base == 16) { pre[pl++] = '0'; pre[pl++] = (conv == 'X') ? 'X' : 'x'; }
            else if (base == 8) pre[pl++] = '0';
            else if (base == 2) { pre[pl++] = '0'; pre[pl++] = (conv == 'B') ? 'B' : 'b'; }
            break;
          }
          if (spec[fi] < '0' || spec[fi] > '9') continue;
          break;
        }
        pre[pl] = 0;
        char digits[512];
        snprintf(digits, sizeof digits, "%s%s%s", neg_b ? "-" : "", pre, bs + (neg_b ? 1 : 0));
        char bfmt[80]; int bl = 0;
        bfmt[bl++] = '%';
        for (int fi = 1; fi < sl - 1; fi++)
          if (spec[fi] == '-' || (spec[fi] >= '0' && spec[fi] <= '9' && !(bl == 1 && spec[fi] == '0')))
            bfmt[bl++] = spec[fi];
        bfmt[bl++] = 's'; bfmt[bl] = 0;
        wn = snprintf(tmp, sizeof(tmp), bfmt, digits);
        if (wn >= (int)sizeof(tmp) && (wide = (char *)malloc((size_t)wn + 1)) != NULL)
          snprintf(wide, (size_t)wn + 1, bfmt, digits);
        else if (wn >= (int)sizeof(tmp)) wn = (int)sizeof(tmp) - 1;
        if (conv == 'X') for (char *q = wide ? wide : tmp; *q; q++) if (*q >= 'a' && *q <= 'f') *q -= 32;
        }
      }
      else {
      if (v.tag == SP_TAG_FLT && !(v.v.f >= (sp_float)INTPTR_MIN && v.v.f < -(sp_float)INTPTR_MIN)) {
        free(buf); (void)sp_float_to_i_checked(v.v.f);
      }
      if (v.tag == SP_TAG_INT) lv = (long long)v.v.i;
      else if (v.tag == SP_TAG_FLT) lv = (long long)v.v.f;
      /* a String argument converts the way Integer() does, so unparseable text
         is an ArgumentError rather than a silent zero (#3554) */
      else if (v.tag == SP_TAG_STR && v.v.s) lv = (long long)sp_str_to_i_strict(v.v.s);
      /* Anything else is not a number at all, and formatting it as 0 was a
         quiet wrong answer where CRuby refuses (#4010). Same wording and same
         value spelling as Integer() uses. */
      else { free(buf); sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Integer", sp_convert_src_name(v))); }
      /* the non-decimal bases go through our own formatter: C's printf drops
         the '+' and ' ' flags on them and has no two's-complement form */
      if (conv == 'd' || conv == 'i') {
        wn = snprintf(tmp, sizeof(tmp), fmt_use, lv);
        if (wn >= (int)sizeof(tmp) && (wide = (char *)malloc((size_t)wn + 1)) != NULL)
          snprintf(wide, (size_t)wn + 1, fmt_use, lv);
        else if (wn >= (int)sizeof(tmp)) wn = (int)sizeof(tmp) - 1;
      }
      else {
        wn = sp_fmt_binary(spec, sl, conv, lv, tmp, sizeof(tmp));
        /* it stops at the buffer's end: a wider field is formatted again */
        for (size_t wsz = sizeof(tmp) * 2; wn >= (int)(wsz / 2) && wsz <= ((size_t)1 << 30); wsz *= 2) {
          char *w2 = (char *)realloc(wide, wsz);
          if (!w2) { wn = (int)sizeof(tmp); free(wide); wide = NULL; break; }
          wide = w2;
          wn = sp_fmt_binary(spec, sl, conv, lv, wide, wsz);
        }
      }
      }
    }
else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G' ||
         conv == 'a' || conv == 'A') {
      double dv = 0;
      if (v.tag == SP_TAG_FLT) dv = v.v.f;
      else if (v.tag == SP_TAG_INT) dv = (double)v.v.i;
      else if (sp_poly_is_rational(v)) dv = sp_poly_to_f_with_rational(v);
      else if (v.tag == SP_TAG_BIGINT) dv = sp_poly_to_f(v);
      /* a String converts the way Float() does; anything else is not a number
         and CRuby refuses rather than formatting a zero (#4010) */
      else if (v.tag == SP_TAG_STR) dv = (double)sp_str_to_f_strict(v.v.s ? v.v.s : sp_str_empty);
      /* a user object converts through its #to_f, which may allocate */
      else if (sp_poly_is_user_obj(v)) {
        SP_GC_ROOT(a); SP_GC_ROOT_STR(fmt);
        dv = sp_poly_Float_ex(v, 1).v;
      }
      else { free(buf); sp_raise_cls("TypeError", sp_sprintf("can't convert %s into Float", sp_convert_src_name(v))); }
      /* Ruby prints non-finite floats as Inf/-Inf/NaN (C printf lowercases) */
      if (!isfinite(dv)) wn = snprintf(tmp, sizeof(tmp), "%s", isnan(dv) ? "NaN" : dv > 0 ? "Inf" : "-Inf");
      /* pinned "C" locale so the decimal point is always '.', not the process
         locale's separator (the printf field/flag machinery stays libc's) */
      else wn = sp_snprintf_ruby_float(tmp, sizeof(tmp), fmt_use, dv);
      /* `%.2f` of 1e300 is over 300 characters: snprintf answered the full
         length and tmp held only its first 255, and the copy below read past
         it (DBL_MAX printed stack bytes) */
      if (wn >= (int)sizeof(tmp) && isfinite(dv)) {
        wide = (char *)malloc((size_t)wn + 1);
        if (wide) sp_snprintf_ruby_float(wide, (size_t)wn + 1, fmt_use, dv);
        else wn = (int)sizeof(tmp) - 1;
      }
    }
else if (conv == 's' || conv == 'p') {
      /* %s is to_s, %p is inspect -- for every tag (symbols, arrays, hashes,
         booleans, user objects), not just the scalar three. Hash/Array to_s
         is its inspect, which sp_poly_to_s already returns. %p is the same
         conversion over the inspect text (C's %p is a pointer). */
      const char *sv = (conv == 's') ? sp_poly_to_s(v) : sp_poly_inspect(v);
      if (!sv) sv = "";
      /* The field is composed by hand: snprintf's %s counts BYTES, so a width
         or a precision fell in the middle of a multibyte character (`%.1s` of
         "日本" cut it in half, `%-6s` padded it by its byte count), it stops at
         an embedded NUL, and a field wider than the 256-byte buffer it wrote
         into was copied out of that buffer whole. CRuby's width and precision
         count characters, as String#length does; the value's length in bytes
         is its own (an embedded NUL is a byte of the string, #4632). The '0',
         '+', ' ' and '#' flags mean nothing to a String. */
      size_t svl = sp_str_byte_len(sv);
      int left = 0, width = 0, prec = -1, in_prec = 0;
      for (size_t fi = 1; fi + 1 < sl; fi++) {
        char fc = spec[fi];
        if (fc == '-') left = 1;
        else if (fc == '.') { in_prec = 1; prec = 0; }
        else if (fc >= '0' && fc <= '9') {
          int *np = in_prec ? &prec : &width;
          if (*np > (INT32_MAX - (fc - '0')) / 10) {
            free(buf);
            sp_raise_cls("ArgumentError", in_prec ? "precision too big" : "width too big");
          }
          *np = *np * 10 + (fc - '0');
        }
      }
      sp_int nch = sp_str_length(sv);
      if (prec >= 0 && prec < nch) { svl = sp_utf8_byte_offset(sv, prec); nch = prec; }
      size_t pad = (sp_int)width > nch ? (size_t)((sp_int)width - nch) : 0;
      if (out + svl + pad + 1 >= cap) { cap = (out + svl + pad) * 2 + 64; buf = (char *)realloc(buf, cap); }
      if (!left) { memset(buf + out, ' ', pad); out += pad; }
      memcpy(buf + out, sv, svl); out += svl;
      if (left) { memset(buf + out, ' ', pad); out += pad; }
      continue;
    }
else if (conv == 'c') {
      /* an Integer is a codepoint (UTF-8 encoded), a String contributes its
         first whole character -- not a single truncated byte (#3083) */
      char cbuf[8]; int clen = 0;
      if (v.tag == SP_TAG_INT) clen = sp_utf8_encode((uint32_t)v.v.i, cbuf);
      else if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) {
        clen = sp_utf8_advance(v.v.s);
        if (clen > 8) clen = 8;
        memcpy(cbuf, v.v.s, (size_t)clen);
      }
      /* the character occupies one column; honor a width and the '-' flag */
      int left = 0, width = 0;
      for (size_t fi = 1; fi + 1 < sl; fi++) {
        char fc = spec[fi];
        if (fc == '-') left = 1;
        else if (fc >= '0' && fc <= '9') width = width * 10 + (fc - '0');
      }
      int pad = width > 1 ? width - 1 : 0;
      if ((size_t)(clen + pad) >= sizeof(tmp)) pad = (int)sizeof(tmp) - clen - 1;
      if (pad < 0) pad = 0;
      int o2 = 0;
      if (!left) for (int i = 0; i < pad; i++) tmp[o2++] = ' ';
      memcpy(tmp + o2, cbuf, (size_t)clen); o2 += clen;
      if (left) for (int i = 0; i < pad; i++) tmp[o2++] = ' ';
      tmp[o2] = 0;
      wn = o2;
    }
else {
      /* CRuby: an unknown / truncated conversion is a hard error */
      free(buf);
      if (conv)
        sp_raise_cls("ArgumentError", sp_sprintf("malformed format string - %%%c", conv));
      else
        sp_raise_cls("ArgumentError", "incomplete format specifier; use %% (double %) instead");
      return "";
    }
    if (wn < 0) { free(wide); continue; }
    if (out + (size_t)wn + 1 >= cap) { cap = ((out + wn) * 2) + 64; buf = (char *)realloc(buf, cap); }
    memcpy(buf + out, wide ? wide : tmp, wn); out += wn;
    free(wide);
  }
  buf[out] = 0;
  char *r = sp_str_alloc(out); memcpy(r, buf, out); free(buf); return r;
}

/* A grouped redirection shares one open file description across its slots. */
static int sp_process_redirect_slot(sp_RbVal key) {
  if (key.tag == SP_TAG_INT) return key.v.i >= 0 && key.v.i < 3 ? (int)key.v.i : -1;
  if (key.tag != SP_TAG_SYM || !sp_sym_name_fn) return -1;
  const char *name = sp_sym_name_fn((sp_sym)key.v.i);
  return strcmp(name, "in") == 0 ? 0 : strcmp(name, "out") == 0 ? 1 :
         strcmp(name, "err") == 0 ? 2 : -1;
}
static int sp_process_redirect_group(sp_RbVal key) {
  if (!sp_rbval_is_array(key)) return 0;
  int mask = 0;
  sp_int n = sp_poly_length(key);
  for (sp_int j = 0; j < n; j++) {
    int slot = sp_process_redirect_slot(sp_poly_arr_get(key, j));
    if (slot < 0) return 0;
    mask |= 1 << slot;
  }
  return mask;
}
/* Process.spawn's value for one standard descriptor slot (0 in, 1 out,
   2 err): a :in/:out/:err key, or an Array key such as [:out, :err] that
   names the slot. For a group, *prior is the group's first slot when that
   comes before this one, so the caller reuses the descriptor it resolved
   there instead of opening the same file again. */
sp_RbVal sp_process_redirect_value(sp_RbVal opts, int slot, sp_bool *found, int *prior) {
  SP_GC_ROOT_RBVAL(opts);
  *found = FALSE;
  *prior = -1;
  for (sp_int j = 0, n = sp_poly_length(opts); j < n; j++) {
    sp_RbVal key, value;
    sp_poly_hash_pair(opts, j, &key, &value);
    if (key.tag == SP_TAG_SYM && sp_process_redirect_slot(key) == slot) {
      *found = TRUE; return value;
    }
    int mask = sp_process_redirect_group(key);
    if (mask & (1 << slot)) {
      for (int k = 0; k < slot; k++) if (mask & (1 << k)) { *prior = k; break; }
      *found = TRUE; return value;
    }
  }
  return sp_box_nil();
}
