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
#include "spinel_rt.h"

const char *(*sp_class_name_fn)(sp_Class);
const char *(*sp_class_display_fn)(sp_Class);
/* the hooks the generated unit installs for this unit (SP_INSTALL_HOOK) */
sp_user_binop_fn sp_user_binop_hook_lib;
sp_obj_eq_fn sp_obj_eq_hook_lib;
sp_obj_hash_fn sp_obj_hash_hook_lib;
sp_obj_eql_fn sp_obj_eql_hook_lib;
void (*sp_user_init_copy_hook_lib)(sp_RbVal, sp_RbVal);
sp_RbVal (*sp_bsub_dup_hook_lib)(sp_RbVal, int, sp_bool *);
/* the symbol interner is generated per program too; a keyword name that is not
   a known symbol reaches it through the hook the generated unit installs */
sp_sym sp_poly_cold_sym_intern(const char *s) { return sp_json_sym_intern_fn ? sp_json_sym_intern_fn(s) : (sp_sym)0; }
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
    case SP_TAG_CLASS: { fputs(sp_class_val_display(v), stdout); putchar('\n'); break; }
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
            const char *_s = _a->data[_i];
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
        case SP_BUILTIN_TIME: puts(sp_Time_to_s((sp_Time *)v.v.p)); break;
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
    case SP_TAG_SYM: return sp_sym_to_s_chilled((sp_sym)v.v.i);
    case SP_TAG_CLASS: return sp_class_val_display(v);
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
        case SP_BUILTIN_TIME: return sp_Time_to_s((sp_Time *)v.v.p);
        case SP_BUILTIN_STRBUF: return sp_String_cstr((sp_String *)v.v.p);   /* live buffer (#3227) */
        case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
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
    case SP_TAG_CLASS: return sp_class_val_display(v);
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
        case SP_BUILTIN_TIME:      return sp_Time_inspect((sp_Time *)v.v.p);
      case SP_BUILTIN_STRBUF: return sp_str_inspect(sp_String_cstr((sp_String *)v.v.p));   /* (#3227) */
      case SP_BUILTIN_METHOD: return sp_method_desc_cstr((sp_BoundMethod *)v.v.p);
        case SP_BUILTIN_COMPLEX:   return sp_complex_inspect(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL:  return sp_rational_inspect(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_BIG_RATIONAL:  return sp_brat_inspect((sp_BigRational *)v.v.p);
        case SP_BUILTIN_REGEX:     return sp_re_inspect_str(v.v.p);
        case SP_BUILTIN_MATCHDATA: return sp_MatchData_inspect((sp_MatchData *)v.v.p);
        case SP_BUILTIN_EXCEPTION: return sp_exc_inspect(v.v.p);
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
{sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_StrPolyHash_set(r,h->order[i],sp_StrPolyHash_get(h,h->order[i]));return r;}

void sp_StrPolyHash_clear(sp_StrPolyHash*h)
{ sp_gc_wb((void*)h);if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}

const char*sp_StrPolyHash_inspect(sp_StrPolyHash*h)
{return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_POLY_HASH)):SPL("nil");}

sp_StrStrHash *sp_StrPolyHash_to_s_values(sp_StrPolyHash *h)
{
  SP_GC_ROOT(h);
  sp_StrStrHash *r = sp_StrStrHash_new(); SP_GC_ROOT(r);
  /* a missing match reads the default, as its to_s (nil's "" is no default) */
  if (h && h->default_v.tag != SP_TAG_NIL) r->default_v = sp_poly_to_s(h->default_v);
  if (h) for (sp_int i = 0; i < h->len; i++) {
    const char *k = h->order[i];
    sp_StrStrHash_set(r, k, sp_poly_to_s(sp_StrPolyHash_get(h, k)));
  }
  return r;
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
{sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_SymPolyHash_set(r,h->order[i],sp_SymPolyHash_get(h,h->order[i]));return r;}

void sp_SymPolyHash_clear(sp_SymPolyHash*h)
{if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;}

const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h)
{return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_SYM_POLY_HASH)):SPL("nil");}

sp_PolyPolyHash*sp_PolyPolyHash_merge(sp_PolyPolyHash*a,sp_PolyPolyHash*b)
{SP_GC_ROOT(a);SP_GC_ROOT(b);sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);if(a){r->default_v=a->default_v;r->dproc=a->dproc;r->dproc_self=a->dproc_self;for(sp_int i=0;i<a->len;i++){sp_int idx=a->order[i];sp_PolyPolyHash_set(r,a->keys[idx],a->vals[idx]);}}if(b){for(sp_int i=0;i<b->len;i++){sp_int idx=b->order[i];sp_PolyPolyHash_set(r,b->keys[idx],b->vals[idx]);}}return r;}

void sp_PolyPolyHash_clear(sp_PolyPolyHash*h)
{if(!h)return;for(sp_int i=0;i<h->cap;i++)h->occ[i]=0;h->len=0;}

sp_RbVal sp_poly_clear(sp_RbVal v)
{
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_QUEUE && v.v.p) { sp_Queue_clear((sp_queue *)v.v.p); return v; }
  /* String#clear on a plain string box: a fresh empty string the receiver
     takes back, as the typed clear builds; a frozen one raises. It answered
     the box untouched. */
  if (v.tag == SP_TAG_STR) { sp_str_check_mutable(v.v.s); return sp_box_str(sp_str_from_bytes("", 0)); }
  sp_poly_coll_chk(v, "clear");
  if (v.tag != SP_TAG_OBJ || !v.v.p) return v;
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
      (b.tag == SP_TAG_STR || b.tag == SP_TAG_SYM)) {
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

sp_RbVal sp_poly_dup(sp_RbVal v, int keep_frozen)
{
  if (sp_bsub_dup_hook && v.tag == SP_TAG_OBJ && v.v.p) {
    sp_bool handled = FALSE;
    sp_RbVal r = sp_bsub_dup_hook(v, keep_frozen, &handled);
    if (handled) return r;
  }
  if (v.tag == SP_TAG_OBJ && v.v.p &&
      (v.cls_id == SP_BUILTIN_RANGE || v.cls_id == SP_BUILTIN_FLOAT_RANGE || v.cls_id == SP_BUILTIN_STR_RANGE))
    return sp_range_dup(v, keep_frozen);

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
        for (sp_int i = 0; i < a->len; i++) sp_StrArray_push(r, a->data[i]);
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
      ((sp_Object *)n)->ivars = t;
    }
    if (sp_user_init_copy_hook) { SP_GC_ROOT(n); sp_RbVal r = v; r.v.p = n; sp_user_init_copy_hook(r, v); }
    if (keep_frozen && h->frozen)
      ((sp_gc_hdr *)((char *)n - sizeof(sp_gc_hdr)))->frozen = 1;
    v.v.p = n;
  }
  return v;
}

void sp_poly_hash_merge_into(sp_RbVal dst, sp_RbVal src)
{
  if (dst.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(dst.cls_id)) return;
  if (src.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(src.cls_id)) return;
  sp_int n = sp_poly_length(src);
  for (sp_int i = 0; i < n; i++) {
    sp_RbVal k, v;
    sp_poly_hash_pair(src, i, &k, &v);
    switch (dst.cls_id) {
      case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash *)dst.v.p, k, v); break;
      case SP_BUILTIN_SYM_POLY_HASH:
        if (k.tag == SP_TAG_SYM) sp_SymPolyHash_set((sp_SymPolyHash *)dst.v.p, (sp_sym)k.v.i, v);
        else sp_poly_typed_hash_store_miss(k, v, "Symbol", NULL);
        break;
      case SP_BUILTIN_STR_POLY_HASH:
        if (k.tag == SP_TAG_STR) sp_StrPolyHash_set((sp_StrPolyHash *)dst.v.p, k.v.s, v);
        else sp_poly_typed_hash_store_miss(k, v, "String", NULL);
        break;
      case SP_BUILTIN_STR_STR_HASH:
        if (k.tag == SP_TAG_STR && v.tag == SP_TAG_STR)
          sp_StrStrHash_set((sp_StrStrHash *)dst.v.p, k.v.s, v.v.s);
        else sp_poly_typed_hash_store_miss(k, v, "String", "String");
        break;
      case SP_BUILTIN_STR_INT_HASH:
        if (k.tag == SP_TAG_STR && v.tag == SP_TAG_INT)
          sp_StrIntHash_set((sp_StrIntHash *)dst.v.p, k.v.s, v.v.i);
        else if (k.tag == SP_TAG_STR && v.tag == SP_TAG_NIL)   /* a nil value is kept (D3b-ii) */
          sp_StrIntHash_set_nil((sp_StrIntHash *)dst.v.p, k.v.s);
        else sp_poly_typed_hash_store_miss(k, v, "String", "Integer");
        break;
      case SP_BUILTIN_INT_INT_HASH:
        if (k.tag == SP_TAG_INT && v.tag == SP_TAG_INT)
          sp_IntIntHash_set((sp_IntIntHash *)dst.v.p, k.v.i, v.v.i);
        else if (k.tag == SP_TAG_INT && v.tag == SP_TAG_NIL)   /* a nil value is kept (D3b-ii) */
          sp_IntIntHash_set_nil((sp_IntIntHash *)dst.v.p, k.v.i);
        else sp_poly_typed_hash_store_miss(k, v, "Integer", "Integer");
        break;
      case SP_BUILTIN_INT_STR_HASH:
        if (k.tag == SP_TAG_INT && v.tag == SP_TAG_STR)
          sp_IntStrHash_set((sp_IntStrHash *)dst.v.p, k.v.i, v.v.s);
        else sp_poly_typed_hash_store_miss(k, v, "Integer", "String");
        break;
      default: break;
    }
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
{SP_GC_ROOT(h);sp_PolyPolyHash*r=sp_PolyPolyHash_new();SP_GC_ROOT(r);r->default_v=h->default_v;r->dproc=h->dproc;r->dproc_self=h->dproc_self;for(sp_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,h->keys[h->order[i]],h->vals[h->order[i]]);return r;}

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
      case SP_BUILTIN_STR_STR_HASH: r->default_v = sp_box_nullable_str(((sp_StrStrHash *)a.v.p)->default_v); break;
      case SP_BUILTIN_INT_STR_HASH: r->default_v = sp_box_nullable_str(((sp_IntStrHash *)a.v.p)->default_v); break;
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
      if (with_default) h->default_v = sp_poly_to_s_or_nil(work->default_v);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_StrStrHash_set(h, sp_poly_to_s(work->keys[j]), sp_poly_to_s_or_nil(work->vals[j]));
      }
      return;
    }
    case SP_BUILTIN_INT_STR_HASH: {
      sp_IntStrHash *h = (sp_IntStrHash *)orig.v.p;
      sp_IntStrHash_clear(h);
      if (with_default) h->default_v = sp_poly_to_s_or_nil(work->default_v);
      for (sp_int i = 0; i < work->len; i++) {
        sp_int j = work->order[i];
        sp_IntStrHash_set(h, sp_poly_to_i(work->keys[j]), sp_poly_to_s_or_nil(work->vals[j]));
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
