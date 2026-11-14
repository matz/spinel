/* Opted-in compiler bridge. The retained recorder objects are linked again;
 * deterministic Init registrations bind the same zero-based function slots. */
#ifdef SP_CEXT
#include "sp_cext.h"
#include "sp_string.h"
#include <ruby/encoding.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "sp_setjmp.h"
void sp_exc_arm(jmp_buf);
void sp_exc_disarm(void);
const char *sp_exc_cur_cls(void);
const char *sp_exc_cur_msg(void);
void *sp_exc_cur_obj(void);
void sp_fiber_reraise(const char *, const char *, void *);
/* The nonlocal exit that reached a C frame's sp_exc_arm: taken before the
 * frame is disarmed, and passed on once the frame has put its state back. A
 * throw or break unwinding through C resumes; a raise is raised again. */
typedef struct { int unwind; const char *cls, *msg; void *obj; } CextExit;
static CextExit cext_exit_take(void) {
    CextExit e;
    e.unwind = sp_cext_unwinding();
    e.cls = sp_exc_cur_cls(); e.msg = sp_exc_cur_msg();
    e.obj = sp_exc_cur_obj();
    return e;
}
static void cext_exit_resume(CextExit e) __attribute__((noreturn));
static void cext_exit_resume(CextExit e) {
    if (e.unwind) sp_cext_resume_unwind();
    else sp_fiber_reraise(e.cls, e.msg, e.obj);
    abort();
}
extern void (*sp_user_init_copy_hook_lib)(sp_RbVal, sp_RbVal);
VALUE rb_cObject, rb_cBasicObject, rb_cClass, rb_cModule;
VALUE rb_cString, rb_cArray, rb_cHash, rb_cInteger, rb_cFloat, rb_cIO;
sp_RbVal (*sp_cext_class_lookup_fn)(const char *);
void *(*sp_cext_object_alloc_fn)(int);
void *(*sp_cext_class_alloc_fn)(int);
static sp_cext_extension *binding;
static size_t next_function, next_constant, next_anonymous;
/* Volatile journal fields survive longjmp; each nested Init owns only its
 * newly registered constant addresses, not the process-wide root registry. */
typedef struct { VALUE **addresses; size_t count; } InitRoots;
static volatile InitRoots *init_roots;
#define current_block ((sp_Proc *)sp_cext_current_block)

/* Ruby may yield or raise while the extension lock is suspended. Restore the
 * complete recursive depth before propagating either kind of nonlocal exit. */
static sp_RbVal call_ruby_unlocked(int kind, const char *key, sp_RbVal receiver,
                                 int argc, const sp_RbVal *args, sp_Proc *block,
                                 sp_int *raw) {
    unsigned depth = sp_cext_lock_suspend();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_gc_nroots = roots;
        sp_cext_lock_resume(depth);
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    sp_RbVal result;
    if (raw) {
        _sp_proc_poly_ret = sp_box_nil();
        sp_proc_call(block, argc, raw);
        result = _sp_proc_poly_ret;
    } else {
        result = sp_cext_dispatch_fn(kind, key, receiver, argc, args, block);
    }
    SP_GC_ROOT_RBVAL(result);
    sp_exc_disarm(); sp_cext_lock_resume(depth);
    return result;
}

static const char *class_name(VALUE v) {
    sp_RbVal r = sp_cext_rbval(v);
    if (r.tag != SP_TAG_CLASS) sp_raise_cls("TypeError", "class or module expected");
    if (r.cls_id == SP_CLASS_BY_NAME) return r.v.s;
    if (sp_obj_cls_name_fn) return sp_obj_cls_name_fn((int)r.v.i);
    sp_raise_cls("TypeError", "unknown C extension class");
}
VALUE rb_obj_class(VALUE object) {
    return sp_cext_value(sp_cext_object_class(sp_cext_rbval(object)));
}
const char *rb_class2name(VALUE klass) {
    sp_RbVal value=sp_cext_rbval(klass);
    /* CRuby skips singleton classes. A modeled class/module singleton's
     * real ancestor is Class/Module, not the attached object's class name. */
    if(value.tag==SP_TAG_CLASS && sp_sclass_id_p(value.cls_id)) {
        sp_Class attached={sp_sclass_base_id(value.cls_id),NULL};
        return sp_class_is_module_fn && sp_class_is_module_fn(attached) ? "Module" : "Class";
    }
    return class_name(klass);
}
static VALUE lookup(const char *name) {
    if (!sp_cext_class_lookup_fn) sp_raise_cls("RuntimeError", "C extension class hook is missing");
    return sp_cext_value(sp_cext_class_lookup_fn(name));
}
/* Paths the running Init has defined, so rb_const_defined answers as the
 * recorder did: from the extension's own definitions and the builtins it
 * knows, never from the program's Ruby state. */
static char **init_paths;
static size_t init_path_count;
static char *qualified(VALUE outer, const char *name) {
    const char *parent = class_name(outer);
    size_t len = strlen(parent) + strlen(name) + 3;
    char *full = malloc(len); if (!full) rb_raise(rb_eNoMemError, "class name allocation failed");
    if (!strcmp(parent, "Object")) snprintf(full, len, "%s", name);
    else snprintf(full, len, "%s::%s", parent, name);
    return full;
}
static void init_path(const char *path) {
    if (!binding) return;
    char **next = realloc(init_paths, (init_path_count + 1) * sizeof(*next));
    char *copy = strdup(path);
    if (!next || !copy) rb_raise(rb_eNoMemError, "C extension Init path allocation failed");
    init_paths = next; init_paths[init_path_count++] = copy;
}
static VALUE define_path(const char *full) { init_path(full); return lookup(full); }
static VALUE define_under(VALUE outer, const char *name) {
    char *full = qualified(outer, name);
    VALUE v = define_path(full); free(full); return v;
}
VALUE rb_define_class(const char *name, VALUE superclass) { (void)superclass; return define_path(name); }
VALUE rb_define_class_under(VALUE outer, const char *name, VALUE superclass) { (void)superclass; return define_under(outer,name); }
VALUE rb_define_module(const char *name) { return define_path(name); }
VALUE rb_define_module_under(VALUE outer, const char *name) { return define_under(outer,name); }
/* Class.new in Init is the class the recorder named for it, in creation
 * order (tools/cext/recorder.c rb_class_new) */
VALUE rb_class_new(VALUE superclass) {
    (void)superclass;
    if (!binding || !binding->name) rb_raise(rb_eNotImpError, "Class.new in a C extension outside its Init is not supported");
    char name[256];
    snprintf(name, sizeof name, "SpinelCextAnonymous_%s_%zu", binding->name, next_anonymous++);
    return define_path(name);
}
static void bind_function(VALUE (*fn)(ANYARGS)) {
    if (!binding || next_function >= binding->count)
        sp_raise_cls("RuntimeError", "C extension definitions outside Init are unsupported");
    binding->functions[next_function++] = fn;
}
void rb_define_method(VALUE owner,const char *name,VALUE (*fn)(ANYARGS),int arity) { (void)owner;(void)name;(void)arity;bind_function(fn); }
void rb_define_private_method(VALUE o,const char *n,VALUE (*f)(ANYARGS),int a) { rb_define_method(o,n,f,a); }
void rb_define_singleton_method(VALUE o,const char *n,VALUE (*f)(ANYARGS),int a) { rb_define_method(o,n,f,a); }
void rb_define_module_function(VALUE o,const char *n,VALUE (*f)(ANYARGS),int a) { rb_define_method(o,n,f,a);rb_define_method(o,n,f,a); }
void rb_define_alloc_func(VALUE o,VALUE (*fn)(VALUE)) { (void)o;bind_function((VALUE (*)(ANYARGS))fn); }
void rb_define_alias(VALUE o,const char *n,const char *old) { (void)o;(void)n;(void)old; }
void rb_define_attr(VALUE o,const char *n,int r,int w) { (void)o;(void)n;(void)r;(void)w; }
void rb_include_module(VALUE o,VALUE m) { (void)o;(void)m; }
void rb_extend_object(VALUE o,VALUE m) { (void)o;(void)m; }
void rb_undef_method(VALUE o,const char *n) { (void)o;(void)n; }
void rb_undef_alloc_func(VALUE o) { (void)o; }
static const char *const recorder_builtins[] = {"BasicObject","Object","Class","Module","String","Array","Hash",
    "Integer","Float","IO","Exception","StandardError","RuntimeError","TypeError","ArgumentError","RangeError",
    "NoMemoryError","SystemCallError","NotImplementedError",NULL};
int rb_const_defined(VALUE outer,ID id) {
    const char *name=rb_id2name(id);
    if(!name)rb_raise(rb_eArgError,"unknown constant ID");
    if(!binding)rb_raise(rb_eRuntimeError,"rb_const_defined outside Init is unsupported");
    for(int i=0;recorder_builtins[i];i++)if(!strcmp(recorder_builtins[i],name))return 1;
    char *full=qualified(outer,name);int found=0;
    for(size_t i=0;i<init_path_count && !found;i++)found=!strcmp(init_paths[i],full);
    free(full);return found;
}
void rb_define_const(VALUE o,const char *n,VALUE v) {
    (void)o;(void)n;
    if(!binding || next_constant>=binding->constant_count)sp_raise_cls("RuntimeError","C constant definitions outside Init are unsupported");
    binding->constants[next_constant]=v;
    VALUE *address=&binding->constants[next_constant++];
    if(sp_cext_register_address(address))init_roots->addresses[init_roots->count++]=address;
}
static void sp_cext_init_locked(sp_cext_extension *extension) {
    if (extension->initialized) return;
    sp_cext_exceptions_init();
#define BUILTIN(var,name) do { var=lookup(name);rb_gc_register_address(&var); } while(0)
    BUILTIN(rb_cBasicObject,"BasicObject");BUILTIN(rb_cObject,"Object");
    BUILTIN(rb_cClass,"Class");BUILTIN(rb_cModule,"Module");
    BUILTIN(rb_cString,"String");BUILTIN(rb_cArray,"Array");
    BUILTIN(rb_cHash,"Hash");BUILTIN(rb_cInteger,"Integer");
    BUILTIN(rb_cFloat,"Float");BUILTIN(rb_cIO,"IO");
#undef BUILTIN
    VALUE **addresses=extension->constant_count?malloc(extension->constant_count*sizeof(*addresses)):NULL;
    if(extension->constant_count && !addresses)rb_raise(rb_eNoMemError,"C extension Init root allocation failed");
    volatile InitRoots journal={addresses,0};
    volatile InitRoots *previous_roots=init_roots;
    sp_cext_arena_mark mark=sp_cext_arena_enter();
    sp_cext_extension *previous=binding;
    size_t previous_function=next_function, previous_constant=next_constant, previous_anonymous=next_anonymous;
    int roots=sp_gc_nroots;
    volatile size_t previous_paths=init_path_count;
    jmp_buf frame;
    if(setjmp(frame)!=0) {
        CextExit left=cext_exit_take();
        sp_exc_disarm();
        for(size_t i=0;i<journal.count;i++) {
            rb_gc_unregister_address(addresses[i]);*addresses[i]=Qnil;
        }
        init_roots=previous_roots;free(addresses);binding=previous;
        next_function=previous_function;next_constant=previous_constant;next_anonymous=previous_anonymous;
        while(init_path_count>previous_paths)free(init_paths[--init_path_count]);
        sp_cext_arena_restore(mark);sp_gc_nroots=roots;
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    init_roots=&journal;binding=extension;next_function=0;next_constant=0;next_anonymous=0;extension->init();
    sp_exc_disarm();init_roots=previous_roots;free(addresses);binding=previous;
    while(init_path_count>previous_paths)free(init_paths[--init_path_count]);
    next_function=previous_function;next_constant=previous_constant;next_anonymous=previous_anonymous;
    extension->initialized=1;
    sp_cext_arena_restore(mark);
}
static sp_cext_data *data_object(VALUE object) {
    sp_RbVal r=sp_cext_rbval(object);
    if(r.tag!=SP_TAG_OBJ || r.cls_id<0 || !r.v.p || !sp_cext_data_class_fn || !sp_cext_data_class_fn(r.cls_id))
        rb_raise(rb_eTypeError,"expected TypedData object");
    return r.v.p;
}
VALUE rb_obj_alloc(VALUE klass) {
    if(!strcmp(class_name(klass),"Object"))return sp_cext_value(sp_cext_plain_object_new());
    sp_RbVal value=sp_cext_rbval(klass);
    if(value.tag==SP_TAG_CLASS && value.cls_id>=0 && sp_cext_data_class_fn && sp_cext_data_class_fn(value.cls_id))
        return sp_cext_value(sp_box_obj(sp_cext_class_alloc_fn(value.cls_id),value.cls_id));
    rb_raise(rb_eTypeError,"class has no C extension allocation hook");
}
VALUE rb_data_typed_object_wrap(VALUE klass,void *data,const rb_data_type_t *type) {
    sp_RbVal k=sp_cext_rbval(klass);
    if(k.tag!=SP_TAG_CLASS || k.cls_id==SP_CLASS_BY_NAME || !sp_cext_object_alloc_fn ||
       !sp_cext_data_class_fn || !sp_cext_data_class_fn((int)k.v.i)) rb_raise(rb_eTypeError,"class has no TypedData layout");
    void *ptr=sp_cext_object_alloc_fn((int)k.v.i);
    sp_cext_data *o=ptr; o->cext_data=data; o->cext_type=type;
    if(!(type->flags&RUBY_TYPED_WB_PROTECTED))sp_gc_pin_remembered(ptr);
    return sp_cext_value(sp_box_obj(ptr,(int)k.v.i));
}
void *rb_data_typed_object_zalloc(VALUE klass,size_t size,const rb_data_type_t *type,VALUE *value) {
    VALUE object=rb_data_typed_object_wrap(klass,NULL,type);
    void *data=calloc(1,size); if(!data)rb_raise(rb_eNoMemError,"TypedData allocation failed");
    data_object(object)->cext_data=data;
    *value=object;return data;
}
void *rb_check_typeddata(VALUE value,const rb_data_type_t *type) {
    sp_cext_data *o=data_object(value);
    if(!sp_cext_data_is_kind_of(o,type))rb_raise(rb_eTypeError,"wrong TypedData type");
    return o->cext_data;
}
void *sp_cext_data_ptr(VALUE value) { return data_object(value)->cext_data; }
const rb_data_type_t *sp_cext_data_type(VALUE value) { return data_object(value)->cext_type; }
static void *sp_cext_allocate_locked(sp_cext_extension *ext,int slot,int cls) {
    /* rb_undef_alloc_func: no slot, as CRuby's allocator-undefined class */
    if(slot<0)rb_raise(rb_eTypeError,"allocator undefined for %s",sp_obj_cls_name_fn?sp_obj_cls_name_fn(cls):"class");
    sp_cext_init(ext);sp_cext_arena_mark mark=sp_cext_arena_enter();
    sp_RbVal k={SP_TAG_CLASS,cls,{.i=cls}};
    VALUE obj=((VALUE (*)(VALUE))ext->functions[slot])(sp_cext_value(k));
    sp_RbVal r=sp_cext_rbval(obj);
    if(r.tag!=SP_TAG_OBJ || !r.v.p || r.cls_id!=cls)rb_raise(rb_eTypeError,"allocator returned the wrong class");
    void *ptr=r.v.p;sp_cext_arena_restore(mark);return ptr;
}
sp_RbVal sp_cext_duplicate(sp_RbVal value,int keep_frozen) {
    if(value.tag!=SP_TAG_OBJ || !value.v.p || value.cls_id<0 || !sp_cext_data_class_fn || !sp_cext_data_class_fn(value.cls_id))return value;
    value.cls_id=(int)((sp_cext_data *)value.v.p)->cls_id;
    SP_GC_ROOT_RBVAL(value);
    void *src=value.v.p;
    sp_gc_hdr *hdr=(sp_gc_hdr *)src-1;
    size_t size=hdr->size-sizeof(*hdr);
    void *ptr=sp_cext_class_alloc_fn(value.cls_id);SP_GC_ROOT(ptr);
    sp_cext_data *fresh=ptr;
    void *data=fresh->cext_data;const rb_data_type_t *type=fresh->cext_type;
    memcpy(ptr,src,size);fresh->cext_data=data;fresh->cext_type=type;
    sp_RbVal copy=value;copy.v.p=ptr;
    if(sp_user_init_copy_hook_lib)sp_user_init_copy_hook_lib(copy,value);
    if(keep_frozen && hdr->frozen)((sp_gc_hdr *)ptr-1)->frozen=1;
    return copy;
}
/* NUM2LONG and NUM2INT answer as CRuby's rb_num2long / rb_num2int, with the
 * same messages; an object of the program converts through its to_int. */
long rb_num2long(VALUE value) {
    sp_RbVal number=sp_cext_rbval(value);
    if(number.tag==SP_TAG_INT)return (long)number.v.i;
    if(number.tag==SP_TAG_FLT) {
        double d=number.v.f;
        if(d<-(double)LONG_MIN && d>=(double)LONG_MIN)return (long)d;
        char text[32];
        if(isnan(d))snprintf(text,sizeof text,"NaN");
        else if(isinf(d))snprintf(text,sizeof text,d<0?"-Inf":"Inf");
        else snprintf(text,sizeof text,"%-.10g",d);
        rb_raise(rb_eRangeError,"float %s out of range of integer",text);
    }
    if(number.tag==SP_TAG_BIGINT)rb_raise(rb_eRangeError,"bignum too big to convert into 'long'");
    if(number.tag==SP_TAG_OBJ && number.cls_id>=0)return (long)sp_cext_object_to_int(number);
    if(number.tag==SP_TAG_NIL)rb_raise(rb_eTypeError,"no implicit conversion from nil to integer");
    if(number.tag==SP_TAG_BOOL)rb_raise(rb_eTypeError,"no implicit conversion of %s into Integer",number.v.b?"true":"false");
    rb_raise(rb_eTypeError,"no implicit conversion of %s into Integer",rb_class2name(rb_obj_class(value)));
}
int rb_num2int(VALUE value) {
    long number=rb_num2long(value);
    if(number>INT_MAX)rb_raise(rb_eRangeError,"integer %ld too big to convert to 'int'",number);
    if(number<INT_MIN)rb_raise(rb_eRangeError,"integer %ld too small to convert to 'int'",number);
    return (int)number;
}
/* NUM2LL: CRuby's rb_num2ll names the kind it cannot convert */
long long rb_num2ll(VALUE value) {
    sp_RbVal number=sp_cext_rbval(value);
    if(number.tag==SP_TAG_NIL)rb_raise(rb_eTypeError,"no implicit conversion from nil");
    if(number.tag==SP_TAG_BOOL)rb_raise(rb_eTypeError,"no implicit conversion from boolean");
    if(number.tag==SP_TAG_STR || (number.tag==SP_TAG_OBJ && number.cls_id==SP_BUILTIN_STRBUF))
        rb_raise(rb_eTypeError,"no implicit conversion from string");
    return (long long)rb_num2long(value);
}
/* NUM2ULONG: a negative number wraps, as CRuby's rb_num2ulong */
unsigned long rb_num2ulong(VALUE value) { return (unsigned long)rb_num2long(value); }
VALUE rb_ull2inum(unsigned long long number) {
    if(number<=(unsigned long long)INT64_MAX)return sp_cext_value(sp_box_int((int64_t)number));
    /* past the word: twice its upper 63 bits, plus its low bit */
    sp_Bigint *half=sp_bigint_new_int((int64_t)(number>>1));SP_GC_ROOT(half);
    sp_Bigint *two=sp_bigint_new_int(2);SP_GC_ROOT(two);
    sp_Bigint *twice=sp_bigint_mul(half,two);SP_GC_ROOT(twice);
    sp_Bigint *bit=sp_bigint_new_int((int64_t)(number&1));SP_GC_ROOT(bit);
    return sp_cext_value(sp_box_bigint(sp_bigint_add(twice,bit)));
}
double rb_num2dbl(VALUE value) { return sp_cext_num2dbl(sp_cext_rbval(value)); }
int rb_integer_pack(VALUE value,void *words,size_t numwords,size_t wordsize,size_t nails,int flags) {
    if(nails || (flags & ~(INTEGER_PACK_NATIVE_BYTE_ORDER|INTEGER_PACK_2COMP|INTEGER_PACK_LSWORD_FIRST)) ||
       !(flags & INTEGER_PACK_2COMP) || ((flags & INTEGER_PACK_NATIVE_BYTE_ORDER)!=INTEGER_PACK_NATIVE_BYTE_ORDER) ||
       (numwords>1 && !(flags & INTEGER_PACK_LSWORD_FIRST)))
        rb_raise(rb_eNotImpError,"rb_integer_pack supports two's complement in the native byte order only");
    sp_RbVal number=sp_cext_rbval(value);
    if(number.tag!=SP_TAG_INT && number.tag!=SP_TAG_BIGINT)number=sp_box_int(rb_num2long(value));
    return sp_cext_integer_pack(number,words,numwords,wordsize);
}
VALUE rb_obj_freeze(VALUE value) { (void)sp_cext_freeze(sp_cext_rbval(value));return value; }
/* rb_path2class: a class of the program or a builtin by its full name */
VALUE rb_path2class(const char *path) { return lookup(path); }
VALUE rb_ary_delete(VALUE ary,VALUE item) { return sp_cext_value(sp_cext_ary_delete(sp_cext_rbval(ary),sp_cext_rbval(item))); }
/* A value kept for the rest of the run: one more registered root */
static VALUE *permanent;
static size_t permanent_n,permanent_cap;
void rb_gc_register_mark_object(VALUE value) {
    if(SPECIAL_CONST_P(value))return;
    if(permanent_n==permanent_cap) {
        size_t n=permanent_cap?permanent_cap*2:16;
        VALUE *next=realloc(permanent,n*sizeof(*next));
        if(!next)rb_raise(rb_eNoMemError,"C extension root allocation failed");
        for(size_t i=0;i<permanent_n;i++)rb_gc_unregister_address(&permanent[i]);
        permanent=next;permanent_cap=n;
        for(size_t i=0;i<permanent_n;i++)rb_gc_register_address(&permanent[i]);
    }
    permanent[permanent_n]=value;rb_gc_register_address(&permanent[permanent_n++]);
}
/* RB_OBJ_WRITE / RB_OBJ_WRITTEN: the store into a data object's struct, and
 * the write barrier a generational collector needs for it */
void sp_cext_obj_written(VALUE object,VALUE value) {
    (void)value;
    sp_RbVal r=sp_cext_rbval(object);
    if(r.tag==SP_TAG_OBJ && r.v.p)sp_gc_wb(r.v.p);
}
VALUE sp_cext_obj_write(VALUE object,VALUE *slot,VALUE value) {
    *slot=value;sp_cext_obj_written(object,value);return value;
}
void *ruby_xmalloc(size_t size) {
    void *p=malloc(size?size:1);
    if(!p)rb_raise(rb_eNoMemError,"failed to allocate memory");
    return p;
}
void *ruby_xmalloc2(size_t count,size_t size) {
    if(size && count>SIZE_MAX/size)rb_raise(rb_eArgError,"malloc: possible integer overflow");
    return ruby_xmalloc(count*size);
}
void *ruby_xcalloc(size_t count,size_t size) {
    void *p=calloc(count?count:1,size?size:1);
    if(!p)rb_raise(rb_eNoMemError,"failed to allocate memory");
    return p;
}
void *ruby_xrealloc(void *pointer,size_t size) {
    void *p=realloc(pointer,size?size:1);
    if(!p)rb_raise(rb_eNoMemError,"failed to allocate memory");
    return p;
}
void *ruby_xrealloc2(void *pointer,size_t count,size_t size) {
    if(size && count>SIZE_MAX/size)rb_raise(rb_eArgError,"realloc: possible integer overflow");
    return ruby_xrealloc(pointer,count*size);
}
VALUE rb_sym2str(VALUE symbol) {
    if(!SYMBOL_P(symbol))rb_raise(rb_eTypeError,"expected a Symbol");
    return sp_cext_value(sp_cext_sym2str((sp_sym)SYM2ID(symbol)));
}
VALUE rb_str_plus(VALUE a,VALUE b) { return sp_cext_value(sp_cext_str_plus(sp_cext_rbval(a),sp_cext_rbval(b))); }
int rb_proc_arity(VALUE proc) {
    sp_RbVal r=sp_cext_rbval(proc);
    if(r.tag!=SP_TAG_OBJ || r.cls_id!=SP_BUILTIN_PROC || !r.v.p)rb_raise(rb_eTypeError,"expected a Proc");
    return (int)sp_proc_arity((sp_Proc *)r.v.p);
}
void rb_ext_ractor_safe(bool safe) { (void)safe; }
/* StringValue's conversion: what to_str answered (Qundef when the object has
 * no to_str), checked as CRuby's rb_string_value checks it */
VALUE sp_cext_to_str(VALUE value,VALUE converted) {
    const char *name=value==Qnil ? "nil" : value==Qtrue ? "true" : value==Qfalse ? "false" : rb_class2name(rb_obj_class(value));
    if(converted==Qundef)rb_raise(rb_eTypeError,"no implicit conversion of %s into String",name);
    if(rb_type(converted)!=T_STRING)
        rb_raise(rb_eTypeError,"can't convert %s to String (%s#to_str gives %s)",name,name,rb_class2name(rb_obj_class(converted)));
    return converted;
}
/* rb_scan_args: leading mandatory and optional counts, a rest `*`, trailing
 * mandatory, `:` for keywords (taken as a trailing Hash) and `&` for the
 * block, as CRuby's format reads */
int rb_scan_args(int argc,const VALUE *argv,const char *fmt,...) {
    const char *p=fmt;
    int lead=0,opt=0,rest=0,trail=0,kw=0,blk=0;
    if(*p>='0' && *p<='9')lead=*p++-'0';
    if(*p>='0' && *p<='9')opt=*p++-'0';
    if(*p=='*') { rest=1;p++; }
    if(*p>='0' && *p<='9')trail=*p++-'0';
    if(*p==':') { kw=1;p++; }
    if(*p=='&') { blk=1;p++; }
    if(*p)rb_raise(rb_eNotImpError,"rb_scan_args format \"%s\" is not supported",fmt);
    VALUE keywords=Qnil;
    if(kw && argc>lead+trail && rb_type(argv[argc-1])==T_HASH) { keywords=argv[argc-1];argc--; }
    int min=lead+trail;
    if(argc<min || (!rest && argc>min+opt)) {
        if(rest)rb_raise(rb_eArgError,"wrong number of arguments (given %d, expected %d+)",argc,min);
        if(opt)rb_raise(rb_eArgError,"wrong number of arguments (given %d, expected %d..%d)",argc,min,min+opt);
        rb_raise(rb_eArgError,"wrong number of arguments (given %d, expected %d)",argc,min);
    }
    va_list ap;va_start(ap,fmt);
    int i=0;
    for(int k=0;k<lead;k++) { VALUE *v=va_arg(ap,VALUE *);if(v)*v=argv[i];i++; }
    int avail=argc-min;
    for(int k=0;k<opt;k++) { VALUE *v=va_arg(ap,VALUE *);if(v)*v=k<avail?argv[i]:Qnil;if(k<avail)i++; }
    if(rest) {
        VALUE *v=va_arg(ap,VALUE *);
        int n=argc-i-trail;
        VALUE list=rb_ary_new();
        for(int k=0;k<n;k++)rb_ary_push(list,argv[i+k]);
        i+=n;if(v)*v=list;
    }
    for(int k=0;k<trail;k++) { VALUE *v=va_arg(ap,VALUE *);if(v)*v=argv[i];i++; }
    if(kw) { VALUE *v=va_arg(ap,VALUE *);if(v)*v=keywords; }
    if(blk) { VALUE *v=va_arg(ap,VALUE *);if(v)*v=rb_block_given_p()?rb_block_proc():Qnil; }
    va_end(ap);
    return argc;
}
int rb_type(VALUE value) { return sp_cext_type(sp_cext_rbval(value),sp_cext_data_class_fn); }
/* CRuby's builtin_types names for Check_Type's message */
static const char *type_name(int type) {
    switch(type) {
    case T_NIL: return "nil"; case T_OBJECT: return "Object"; case T_CLASS: return "Class";
    case T_MODULE: return "Module"; case T_FLOAT: return "Float"; case T_STRING: return "String";
    case T_REGEXP: return "Regexp"; case T_ARRAY: return "Array"; case T_FIXNUM: return "Integer";
    case T_HASH: return "Hash"; case T_STRUCT: return "Struct"; case T_BIGNUM: return "Integer";
    case T_FILE: return "File"; case T_TRUE: return "true"; case T_FALSE: return "false";
    case T_SYMBOL: return "Symbol"; case T_DATA: return "Data"; case T_MATCH: return "MatchData";
    case T_RATIONAL: return "Rational"; case T_COMPLEX: return "Complex";
    default: return "unknown";
    }
}
void rb_check_type(VALUE value,int type) {
    sp_RbVal object=sp_cext_rbval(value);
    if(type==T_STRING && (object.tag==SP_TAG_STR || (object.tag==SP_TAG_OBJ && object.cls_id==SP_BUILTIN_STRBUF)))return;
    if(type==T_ARRAY && object.tag==SP_TAG_OBJ && object.cls_id==SP_BUILTIN_POLY_ARRAY)return;
    int actual=rb_type(value);
    /* an Array the analysis did not widen, or a String or Array subclass:
     * C could not use it as one */
    if(actual==type && (type==T_STRING || type==T_ARRAY))rb_raise(rb_eTypeError,"unexpected C extension argument type");
    if(actual==type)return;
    const char *name=value==Qnil ? "nil" : value==Qtrue ? "true" : value==Qfalse ? "false" : rb_class2name(rb_obj_class(value));
    rb_raise(rb_eTypeError,"wrong argument type %s (expected %s)",name,type_name(type));
}
VALUE rb_ll2inum(long long number) { return sp_cext_value(sp_box_int(number)); }
VALUE rb_float_new(double number) { return sp_cext_value(sp_box_float(number)); }
VALUE rb_str_new(const char *s,long len) {
    if(len<0)rb_raise(rb_eArgError,"negative string size");
    if(len>INT64_MAX/2-64)rb_raise(rb_eArgError,"string size too large");
    sp_String *str=sp_String_new_len(s?s:"",s?len:0);SP_GC_ROOT(str);
    if(!s) {
        if(!sp_fd_grow(str,len))rb_raise(rb_eNoMemError,"string allocation failed");
        memset(str->data,0,(size_t)len);str->len=len;str->data[len]=0;
    }
    str->binary=1;sp_fd_publish(str);
    return sp_cext_value(sp_box_obj(str,SP_BUILTIN_STRBUF));
}
VALUE rb_str_new_cstr(const char *s) { return rb_str_new(s,(long)strlen(s)); }
void ruby_xfree(void *pointer) { free(pointer); }
void *sp_cext_allocv(VALUE *handle,size_t size,size_t count) {
    size_t alignment=_Alignof(max_align_t);
    if(size && count>((size_t)LONG_MAX-alignment)/size)rb_raise(rb_eArgError,"temporary allocation too large");
    /* The handle is pinned by the invocation arena even when C unwinds
     * through a Ruby exception before an explicit cleanup operation. */
    *handle=rb_str_new(NULL,(long)(size*count+alignment));
    uintptr_t pointer=(uintptr_t)RSTRING_PTR(*handle);
    return (void *)((pointer+alignment-1)/alignment*alignment);
}
/* The encodings a String of the program can carry, with CRuby's indexes:
 * ASCII-8BIT or UTF-8 (sp_String.binary); US-ASCII is an encoding no String
 * answers with, so a test for it is false. The UTF-16 pair exists only so
 * that rb_enc_find_index names them: no String has either. */
static sp_String *string(VALUE value);
struct sp_cext_encoding { int binary; int index; const char *name; };
static struct sp_cext_encoding ascii8bit={1,0,"ASCII-8BIT"}, utf8={0,1,"UTF-8"}, usascii={0,2,"US-ASCII"},
    utf16le={0,100,"UTF-16LE"}, utf16be={0,101,"UTF-16BE"};
static rb_encoding *const encodings[]={&ascii8bit,&utf8,&usascii,&utf16le,&utf16be,NULL};
rb_encoding *rb_ascii8bit_encoding(void) { return &ascii8bit; }
rb_encoding *rb_utf8_encoding(void) { return &utf8; }
rb_encoding *rb_usascii_encoding(void) { return &usascii; }
int rb_ascii8bit_encindex(void) { return ascii8bit.index; }
int rb_utf8_encindex(void) { return utf8.index; }
int rb_usascii_encindex(void) { return usascii.index; }
rb_encoding *rb_default_internal_encoding(void) { return NULL; }
int rb_enc_to_index(rb_encoding *encoding) { return encoding ? encoding->index : -1; }
rb_encoding *rb_enc_from_index(int index) {
    for(int i=0;encodings[i];i++)if(encodings[i]->index==index)return encodings[i];
    return NULL;
}
int rb_enc_find_index(const char *name) {
    for(int i=0;encodings[i];i++)if(!strcasecmp(encodings[i]->name,name))return encodings[i]->index;
    if(!strcasecmp(name,"BINARY"))return ascii8bit.index;
    return -1;
}
static int string_binary(VALUE value) {
    sp_RbVal r=sp_cext_rbval(value);
    if(r.tag==SP_TAG_STR)return r.v.s && sp_str_is_binary(r.v.s);
    if(r.tag==SP_TAG_OBJ && r.cls_id==SP_BUILTIN_STRBUF && r.v.p)return ((sp_String *)r.v.p)->binary;
    rb_raise(rb_eTypeError,"expected a String");
}
rb_encoding *rb_enc_get(VALUE value) { return string_binary(value) ? &ascii8bit : &utf8; }
int rb_enc_get_index(VALUE value) { return rb_enc_get(value)->index; }
VALUE rb_enc_from_encoding(rb_encoding *encoding) {
    if(!encoding)return Qnil;
    sp_RbVal v={SP_TAG_ENCODING,0,{.s=encoding->name}};
    return sp_cext_value(v);
}
static rb_encoding *encoding_of(VALUE value) {
    sp_RbVal r=sp_cext_rbval(value);
    if(r.tag==SP_TAG_ENCODING && r.v.s) {
        int index=rb_enc_find_index(r.v.s);
        if(index>=0)return rb_enc_from_index(index);
    }
    if(r.tag==SP_TAG_STR && r.v.s) {
        int index=rb_enc_find_index(r.v.s);
        if(index>=0)return rb_enc_from_index(index);
    }
    rb_raise(rb_eArgError,"unknown encoding name");
}
/* A copy in the encoding: the bytes are kept, as CRuby keeps them between
 * UTF-8 and ASCII-8BIT for ASCII text; a non-ASCII byte raises as CRuby's
 * transcoder does. */
static VALUE string_in_encoding(VALUE value,rb_encoding *to) {
    if(to!=&ascii8bit && to!=&utf8)rb_raise(rb_eNotImpError,"converting a String to %s is not supported",to->name);
    rb_encoding *from=rb_enc_get(value);
    const char *bytes=RSTRING_PTR(value);long len=RSTRING_LEN(value);
    if(from!=to) {
        for(long i=0;i<len;i++)if((unsigned char)bytes[i]>=0x80) {
            char what[32];snprintf(what,sizeof what,"\\x%02X",(unsigned char)bytes[i]);
            sp_raise_cls("Encoding::UndefinedConversionError",sp_sprintf("\"%s\" from %s to %s",what,from->name,to->name));
        }
    }
    VALUE copy=rb_str_new(bytes,len);
    sp_String_force_encoding(string(copy),to==&ascii8bit);
    return copy;
}
VALUE rb_str_encode(VALUE value,VALUE to,int flags,VALUE options) { (void)flags;(void)options;return string_in_encoding(value,encoding_of(to)); }
VALUE rb_str_export_to_enc(VALUE value,rb_encoding *to) { return string_in_encoding(value,to); }
static sp_String *string(VALUE value) {
    sp_RbVal r=sp_cext_rbval(value);
    if(r.tag==SP_TAG_OBJ && r.cls_id==SP_BUILTIN_STRBUF)return r.v.p;
    if(r.tag==SP_TAG_STR)sp_str_check_mutable(r.v.s);
    rb_raise(rb_eTypeError,"expected a shared String");
}
VALUE rb_str_freeze(VALUE v) {
    sp_RbVal r=sp_cext_rbval(v);
    if(r.tag==SP_TAG_STR && sp_str_is_frozen_val(r.v.s))return v;
    sp_String_freeze(string(v));return v;
}
VALUE rb_utf8_str_new(const char *bytes,long length) {
    VALUE value=rb_str_new(bytes,length);
    sp_String_force_encoding(string(value),0);
    return value;
}
VALUE rb_enc_associate(VALUE value,rb_encoding *encoding) {
    if(encoding!=&ascii8bit && encoding!=&utf8 && encoding!=&usascii)rb_raise(rb_eArgError,"unsupported C extension encoding");
    sp_String_force_encoding(string(value),encoding==&ascii8bit);
    return value;
}
VALUE rb_enc_associate_index(VALUE value,int index) {
    rb_encoding *encoding=rb_enc_from_index(index);
    if(!encoding)rb_raise(rb_eArgError,"unknown encoding index %d",index);
    return rb_enc_associate(value,encoding);
}
VALUE rb_utf8_str_new_cstr(const char *bytes) { return rb_utf8_str_new(bytes,(long)strlen(bytes)); }
VALUE rb_enc_interned_str_cstr(const char *bytes,rb_encoding *encoding) {
    VALUE value=rb_utf8_str_new_cstr(bytes);
    if(encoding==&ascii8bit)sp_String_force_encoding(string(value),1);
    return rb_str_freeze(value);
}
extern sp_bool sp_str_valid_encoding(const char *);
int rb_enc_str_coderange(VALUE value) {
    const char *bytes=RSTRING_PTR(value);
    if(sp_str_ascii_only(bytes))return ENC_CODERANGE_7BIT;
    return sp_str_valid_encoding(bytes) ? ENC_CODERANGE_VALID : ENC_CODERANGE_BROKEN;
}
char *sp_cext_string_cstr(VALUE value) {
    char *bytes=RSTRING_PTR(value);
    if(memchr(bytes,0,(size_t)RSTRING_LEN(value)))rb_raise(rb_eArgError,"string contains null byte");
    return bytes;
}
char *sp_cext_string_ptr(VALUE v) {
    sp_RbVal r=sp_cext_rbval(v);if(r.tag==SP_TAG_STR)return (char *)r.v.s;
    return string(v)->data;
}
long sp_cext_string_len(VALUE v) {
    sp_RbVal r=sp_cext_rbval(v);if(r.tag==SP_TAG_STR)return (long)sp_str_byte_len(r.v.s);
    return (long)string(v)->len;
}
VALUE rb_str_cat(VALUE v,const char *s,long len) { if(len<0)rb_raise(rb_eArgError,"negative string size");sp_String_append_n(string(v),s,(size_t)len);return v; }
VALUE rb_str_cat_cstr(VALUE v,const char *s) { return rb_str_cat(v,s,(long)strlen(s)); }
static sp_PolyArray *array(VALUE value) {
    sp_RbVal r=sp_cext_rbval(value);
    if(r.tag==SP_TAG_OBJ && r.cls_id==SP_BUILTIN_POLY_ARRAY)return r.v.p;
    rb_raise(rb_eTypeError,"expected a widened Array");
}
sp_PolyArray *sp_cext_share_new_array(sp_PolyArray *array) {
    SP_GC_ROOT(array);
    if(!array)return array;
    for(sp_int i=0;i<array->len;i++) {
        sp_RbVal value=array->data[i];
        if(value.tag==SP_TAG_STR && value.v.s && !sp_str_is_frozen_val(value.v.s)) {
            sp_RbVal shared=value;
            if(((const unsigned char *)value.v.s)[-1]==0xfd) {
                sp_String *owner=(sp_String *)(void *)((sp_str_hdr *)sp_fd_base(value.v.s))->next;
                if(owner && owner->data==value.v.s)shared=sp_box_obj(owner,SP_BUILTIN_STRBUF);
            }
            if(shared.tag==SP_TAG_STR)shared=sp_box_obj(sp_String_new_shared(value.v.s),SP_BUILTIN_STRBUF);
            sp_gc_wb(array);array->data[i]=shared;
        }
    }
    return array;
}
VALUE rb_ary_new(void) { return sp_cext_value(sp_box_poly_array(sp_PolyArray_new())); }
VALUE rb_hash_new(void) { return sp_cext_value(sp_cext_hash_new()); }
VALUE rb_hash_new_capa(long capacity) {
    if(capacity<0)rb_raise(rb_eArgError,"negative hash size");
    return rb_hash_new();
}
VALUE rb_hash_aset(VALUE hash,VALUE key,VALUE value) {
    sp_cext_hash_set(sp_cext_rbval(hash),sp_cext_rbval(key),sp_cext_rbval(value));
    return value;
}
VALUE rb_ary_new_capa(long capacity) {
    if(capacity<0)rb_raise(rb_eArgError,"negative array size");
    /* Capacity is a reservation hint; the array remains empty. */
    return rb_ary_new();
}
VALUE rb_ary_push(VALUE v,VALUE item) { sp_PolyArray_push(array(v),sp_cext_rbval(item));return v; }
VALUE rb_ary_new_from_values(long n,const VALUE *values) {
    if(n<0)rb_raise(rb_eArgError,"negative array size");
    VALUE v=rb_ary_new();
    for(long i=0;i<n;i++)rb_ary_push(v,values[i]);
    return v;
}
VALUE rb_ary_new_from_args(long n,...) {
    if(n<0)rb_raise(rb_eArgError,"negative array size");
    VALUE v=rb_ary_new();
    va_list ap;va_start(ap,n);
    for(long i=0;i<n;i++)rb_ary_push(v,va_arg(ap,VALUE));
    va_end(ap);return v;
}
VALUE rb_ary_pop(VALUE v) {
    sp_PolyArray *a=array(v);
    if(a->frozen)sp_raise_frozen_array_at(a,SP_BUILTIN_POLY_ARRAY);
    return a->len ? sp_cext_value(a->data[--a->len]) : Qnil;
}
void rb_ary_store(VALUE v,long index,VALUE item) {
    sp_PolyArray *a=array(v);
    if(a->frozen)sp_raise_frozen_array_at(a,SP_BUILTIN_POLY_ARRAY);
    if(index<0)index+=a->len;
    if(index<0)sp_raise_cls("IndexError","array index too small");
    sp_RbVal value=sp_cext_rbval(item);
    while(a->len<index)sp_PolyArray_push(a,sp_box_nil());
    if(index==a->len)sp_PolyArray_push(a,value);
    else { sp_gc_wb(a);a->data[index]=value; }
}
/* A new Array of the elements from begin, as CRuby's rb_ary_subseq: nil when
 * begin is past the end or either bound is negative. */
VALUE rb_ary_subseq(VALUE v,long begin,long length) {
    sp_PolyArray *a=array(v);
    if(begin>a->len || begin<0 || length<0)return Qnil;
    if(length>a->len-begin)length=a->len-begin;
    sp_PolyArray *slice=sp_PolyArray_new();SP_GC_ROOT(slice);
    for(long i=0;i<length;i++)sp_PolyArray_push(slice,a->data[begin+i]);
    return sp_cext_value(sp_box_poly_array(slice));
}
VALUE rb_hash_aref(VALUE hash,VALUE key) { return sp_cext_value(sp_cext_hash_get(sp_cext_rbval(hash),sp_cext_rbval(key))); }
VALUE rb_ary_entry(VALUE v,long i) { sp_PolyArray *a=array(v);if(i<0)i+=a->len;return i<0||i>=a->len?Qnil:sp_cext_value(a->data[i]); }
long sp_cext_array_len(VALUE v) { return (long)array(v)->len; }
ID rb_intern(const char *name) {
    if(!sp_json_sym_intern_fn)sp_raise_cls("RuntimeError","C extension symbol hook is missing");
    return (ID)sp_json_sym_intern_fn(name);
}
const char *rb_id2name(ID id) { return sp_sym_name_fn?sp_sym_name_fn((sp_sym)id):NULL; }
void rb_gc_start(void) { if (sp_gc_collect_fn) sp_gc_collect_fn(); }
int rb_block_given_p(void) { return current_block!=NULL; }
VALUE rb_block_proc(void) {
    if(!current_block)rb_raise(rb_eArgError,"tried to create Proc object without a block");
    return sp_cext_value(sp_box_obj(current_block,SP_BUILTIN_PROC));
}
VALUE rb_yield_values2(int argc,const VALUE *argv) {
    if(!current_block)sp_raise_cls("LocalJumpError","no block given");
    if(argc<0 || argc>SP_PROC_ARG_SLOTS)rb_raise(rb_eArgError,"too many block arguments");
    sp_int raw[SP_PROC_ARG_SLOTS];
    for(int i=0;i<argc;i++) {
        sp_RbVal v=sp_cext_rbval(argv[i]);_sp_proc_poly_args[i]=v;
        raw[i]=v.v.i;
    }
    return sp_cext_value(call_ruby_unlocked(0,NULL,sp_box_nil(),argc,NULL,current_block,raw));
}
VALUE rb_yield(VALUE v) { return rb_yield_values2(1,&v); }
VALUE rb_yield_values(int argc,...) {
    if(argc<0 || argc>SP_PROC_ARG_SLOTS)rb_raise(rb_eArgError,"too many block arguments");
    VALUE values[SP_PROC_ARG_SLOTS];va_list args;va_start(args,argc);
    for(int i=0;i<argc;i++)values[i]=va_arg(args,VALUE);va_end(args);
    return rb_yield_values2(argc,values);
}
static sp_RbVal sp_cext_invoke_locked(sp_cext_extension *ext,int slot,int arity,sp_RbVal self,sp_PolyArray *args,sp_Proc *block) {
    /* no exception is in flight where Ruby calls a C method: rb_errinfo()
       answers nil, as CRuby's does once a rescue is done, rather than the
       exception a C raise left (sqlite3's step() re-raised the last one) */
    sp_cext_set_errinfo(Qnil);
    sp_cext_init(ext);
    SP_GC_ROOT_RBVAL(self);SP_GC_ROOT(args);SP_GC_ROOT(block);
    sp_cext_arena_mark mark=sp_cext_arena_enter();
    int argc=args?(int)args->len:0;
    VALUE *argv=argc?sp_gc_alloc((size_t)argc*sizeof(VALUE),NULL,NULL):NULL;
    SP_GC_ROOT(argv);
    if(argc&&!argv)rb_raise(rb_eNoMemError,"C argument allocation failed");
    for(int i=0;i<argc;i++)argv[i]=sp_cext_value(args->data[i]);
    VALUE recv=sp_cext_value(self), result=Qnil;
    VALUE (*fn)(ANYARGS)=ext->functions[slot];
    sp_Proc *saved=current_block;sp_cext_current_block=block;
    const int roots=sp_gc_nroots;
    jmp_buf frame;
    if(setjmp(frame)!=0) {
        CextExit left=cext_exit_take();
        sp_exc_disarm();sp_cext_arena_restore(mark);sp_gc_nroots=roots;
        sp_cext_current_block=saved;cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    if(arity>=0 && argc!=arity) { rb_raise(rb_eArgError,"wrong number of arguments (given %d, expected %d)",argc,arity); }
    if(arity==-1)result=((VALUE (*)(int,VALUE *,VALUE))fn)(argc,argv,recv);
    else if(arity==-2)result=((VALUE (*)(VALUE,VALUE))fn)(recv,sp_cext_value(sp_box_poly_array(args)));
    else switch(arity) {
    case 0: result=((VALUE (*)(VALUE))fn)(recv);break;
    case 1: result=((VALUE (*)(VALUE, VALUE))fn)(recv, argv[0]);break;
    case 2: result=((VALUE (*)(VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1]);break;
    case 3: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2]);break;
    case 4: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3]);break;
    case 5: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4]);break;
    case 6: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);break;
    case 7: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);break;
    case 8: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);break;
    case 9: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8]);break;
    case 10: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9]);break;
    case 11: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10]);break;
    case 12: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10], argv[11]);break;
    case 13: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10], argv[11], argv[12]);break;
    case 14: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10], argv[11], argv[12], argv[13]);break;
    case 15: result=((VALUE (*)(VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE, VALUE))fn)(recv, argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10], argv[11], argv[12], argv[13], argv[14]);break;
    default: rb_raise(rb_eArgError,"unsupported C method arity");
    }
    sp_exc_disarm();sp_cext_current_block=saved;
    sp_RbVal value=sp_cext_rbval(result);
    sp_cext_arena_restore(mark);return value;
}
#endif

#ifdef SP_CEXT
sp_RbVal (*sp_cext_dispatch_fn)(int, const char *, sp_RbVal, int, const sp_RbVal *, sp_Proc *);
static VALUE dispatch(int kind, const char *key, VALUE receiver, int argc, const VALUE *args, sp_Proc *block) {
    if (!key) rb_raise(rb_eArgError, "unknown C extension dispatch name");
    if (!sp_cext_dispatch_fn) rb_raise(rb_eRuntimeError, "C extension dispatch hook is missing");
    if (argc < 0) rb_raise(rb_eArgError, "negative C extension call arity");
    if (argc && !args) rb_raise(rb_eArgError, "missing C extension arguments");
    sp_PolyArray *boxed = sp_PolyArray_new(); SP_GC_ROOT(boxed); SP_GC_ROOT(block);
    for (int i = 0; i < argc; i++) sp_PolyArray_push(boxed, sp_cext_rbval(args[i]));
    return sp_cext_value(call_ruby_unlocked(kind, key, sp_cext_rbval(receiver), argc, boxed->data, block, NULL));
}
VALUE rb_funcallv(VALUE receiver, ID id, int argc, const VALUE *args) {
    const char *name = rb_id2name(id);
    if (!name) rb_raise(rb_eArgError, "unknown method ID");
    /* Class.new in Init: the class the recorder declared for it */
    if (binding && argc <= 1 && receiver == rb_cClass && !strcmp(name, "new"))
        return rb_class_new(argc ? args[0] : rb_cObject);
    return dispatch(0, name, receiver, argc, args, NULL);
}
/* rb_apply: a send of the Array's elements, through the spread stub of
 * the name (a call with as many arguments as the Array holds) */
VALUE rb_apply(VALUE receiver, ID id, VALUE list) {
    const char *name = rb_id2name(id);
    if (!name) rb_raise(rb_eArgError, "unknown method ID");
    (void)array(list);
    return dispatch(6, name, receiver, 1, &list, NULL);
}
VALUE rb_funcall(VALUE receiver, ID id, int argc, ...) {
    if (argc < 0) rb_raise(rb_eArgError, "negative C extension call arity");
    VALUE *args = argc ? sp_gc_alloc((size_t)argc * sizeof(VALUE), NULL, NULL) : NULL;
    SP_GC_ROOT(args); va_list ap; va_start(ap, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(ap, VALUE);
    va_end(ap); return rb_funcallv(receiver, id, argc, args);
}
/* A name without the @ ("-instances") is a hidden ivar, which no Ruby code
   reads (sp_cext_hidden_set); the rest are the object's own, reached
   through the program's ivar stubs. */
static int hidden_ivar(const char *name) { return name && name[0] && name[0] != '@'; }
VALUE rb_iv_get(VALUE receiver, const char *name) {
    if (hidden_ivar(name)) return sp_cext_value(sp_cext_hidden_get(sp_cext_rbval(receiver), (sp_sym)rb_intern(name)));
    return dispatch(1, name, receiver, 0, NULL, NULL);
}
VALUE rb_iv_set(VALUE receiver, const char *name, VALUE value) {
    if (hidden_ivar(name)) {
        sp_cext_hidden_set(sp_cext_rbval(receiver), (sp_sym)rb_intern(name), sp_cext_rbval(value));
        return value;
    }
    return dispatch(2, name, receiver, 1, &value, NULL);
}
VALUE rb_ivar_get(VALUE receiver, ID id) { return rb_iv_get(receiver, rb_id2name(id)); }
VALUE rb_ivar_set(VALUE receiver, ID id, VALUE value) { return rb_iv_set(receiver, rb_id2name(id), value); }
VALUE rb_ivar_defined(VALUE receiver, ID id) {
    const char *name = rb_id2name(id);
    if (!hidden_ivar(name))
        rb_raise(rb_eNotImpError, "rb_ivar_defined of a Ruby instance variable (%s) is not supported", name ? name : "?");
    return sp_cext_hidden_defined(sp_cext_rbval(receiver), (sp_sym)id) ? Qtrue : Qfalse;
}
VALUE rb_const_get(VALUE receiver, ID id) { (void)class_name(receiver); return dispatch(3, rb_id2name(id), receiver, 0, NULL, NULL); }
VALUE rb_const_get_at(VALUE receiver, ID id) { return rb_const_get(receiver, id); }
VALUE rb_eval_string(const char *code) { return dispatch(4, code, Qnil, 0, NULL, NULL); }
ID rb_intern2(const char *s, long len) {
    if (len < 0) rb_raise(rb_eArgError, "negative symbol length");
    char *name = malloc((size_t)len + 1);
    if (!name) rb_raise(rb_eNoMemError, "symbol allocation failed");
    memcpy(name, s, (size_t)len); name[len] = 0;
    ID id = rb_intern(name); free(name); return id;
}
#endif

#ifdef SP_CEXT
typedef struct {
    rb_block_call_func_t callback;
    sp_RbVal data;
    sp_Proc *outer;
} CextCallback;
static void callback_scan(void *p) {
    sp_gc_mark(p);
    CextCallback *cap = p;
    sp_mark_rbval(cap->data);
    sp_gc_mark(cap->outer);
}
static sp_int callback_trampoline_locked(void *p, sp_int argc, sp_int *raw) {
    (void)raw;
    CextCallback *cap = p;
    SP_GC_ROOT(cap);
    if (argc < 0 || argc > SP_PROC_ARG_SLOTS) rb_raise(rb_eArgError, "too many callback arguments");
    sp_cext_arena_mark mark = sp_cext_arena_enter();
    sp_Proc *saved = current_block;
    sp_cext_current_block = cap->outer;
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_cext_arena_restore(mark); sp_gc_nroots = roots;
        sp_cext_current_block = saved;
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    VALUE argv[SP_PROC_ARG_SLOTS];
    for (int i = 0; i < argc; i++) argv[i] = sp_cext_value(sp_cext_widen_array(_sp_proc_poly_args[i]));
    VALUE yielded = argc ? argv[0] : Qnil;
    VALUE result = cap->callback(yielded, sp_cext_value(cap->data), (int)argc, argv, Qnil);
    sp_RbVal boxed = sp_cext_rbval(result);
    sp_exc_disarm(); sp_cext_arena_restore(mark); sp_cext_current_block = saved;
    _sp_proc_poly_ret = boxed;
    return boxed.v.i;
}
static sp_int callback_trampoline(void *p, sp_int argc, sp_int *raw) {
    SP_GC_ROOT(p);
    sp_cext_lock();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_gc_nroots = roots; sp_cext_unlock();
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    sp_int result = callback_trampoline_locked(p, argc, raw);
    sp_exc_disarm(); sp_cext_unlock();
    return result;
}
sp_RbVal (*sp_cext_catch_fn)(sp_RbVal, sp_RbVal (*)(void *), void *);
void (*sp_cext_throw_fn)(sp_RbVal, sp_RbVal);
/* A C body run under a Ruby catch: its handles are released when it returns
 * and when a throw or a raise leaves it. */
typedef struct { rb_block_call_func_t fn; VALUE tag, data; } CatchBody;
static sp_RbVal catch_body(void *p) {
    CatchBody *body = p;
    sp_cext_arena_mark mark = sp_cext_arena_enter();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_cext_arena_restore(mark); sp_gc_nroots = roots;
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    VALUE result = body->fn(body->tag, body->data, 0, NULL, Qnil);
    sp_RbVal value = sp_cext_rbval(result);
    sp_exc_disarm(); sp_cext_arena_restore(mark);
    return value;
}
static sp_RbVal catch_under(sp_RbVal tag, sp_RbVal (*fn)(void *), void *p) {
    if (!sp_cext_catch_fn || !sp_cext_throw_fn) rb_raise(rb_eRuntimeError, "C extension catch hook is missing");
    return sp_cext_catch_fn(tag, fn, p);
}
VALUE rb_catch(const char *tag, rb_block_call_func_t fn, VALUE data) {
    VALUE symbol = ID2SYM(rb_intern(tag));
    CatchBody body = {fn, symbol, data};
    return sp_cext_value(catch_under(sp_cext_rbval(symbol), catch_body, &body));
}
void rb_throw(const char *tag, VALUE value) {
    if (!sp_cext_throw_fn) rb_raise(rb_eRuntimeError, "C extension catch hook is missing");
    sp_cext_throw_fn(sp_box_sym((sp_sym)rb_intern(tag)), sp_cext_rbval(value));
    abort();
}
/* rb_block_call runs its call under a catch whose tag is the block call
 * itself, so rb_iter_break leaves the iteration as a Ruby break does: the
 * ensures between run, and rb_block_call returns the break value. */
typedef struct IterCall {
    VALUE receiver; const char *name; int argc; const VALUE *argv; sp_Proc *block;
    sp_RbVal tag; struct IterCall *outer;
} IterCall;
static SP_TLS IterCall *iteration;
static sp_RbVal iterate(void *p) {
    IterCall *call = p;
    return sp_cext_rbval(dispatch(0, call->name, call->receiver, call->argc, call->argv, call->block));
}
VALUE rb_block_call(VALUE receiver, ID id, int argc, const VALUE *argv, rb_block_call_func_t fn, VALUE data) {
    if (!fn) return rb_funcallv(receiver, id, argc, argv);
    CextCallback *cap = sp_gc_alloc(sizeof(*cap), NULL, callback_scan);
    SP_GC_ROOT(cap);
    cap->callback = fn; cap->data = sp_cext_rbval(data); cap->outer = current_block;
    sp_Proc *block = sp_proc_new_meta((void *)callback_trampoline, cap, callback_scan, -1, FALSE, 0, NULL, NULL);
    SP_GC_ROOT(block);
    sp_RbVal tag = sp_box_obj(cap, SP_BUILTIN_OBJECT);
    IterCall call = {receiver, rb_id2name(id), argc, argv, block, tag, iteration};
    IterCall *volatile outer = iteration;
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); iteration = outer; sp_gc_nroots = roots;
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    iteration = &call;
    sp_RbVal result = catch_under(tag, iterate, &call);
    sp_exc_disarm(); iteration = outer;
    return sp_cext_value(result);
}
void rb_iter_break_value(VALUE value) {
    if (!iteration) sp_raise_cls("LocalJumpError", "break from proc-closure");
    sp_cext_throw_fn(iteration->tag, sp_cext_rbval(value));
    abort();
}
void rb_iter_break(void) { rb_iter_break_value(Qnil); }
#endif

#ifdef SP_CEXT
VALUE rb_str_intern(VALUE v) { return ID2SYM(rb_intern2(RSTRING_PTR(v), RSTRING_LEN(v))); }
/* SYM2ID in the retained objects, compiled for the recorder's scan */
ID sp_cext_scanned_sym2id(VALUE v) { return SYM2ID(v); }
#endif

#ifdef SP_CEXT
void sp_cext_init(sp_cext_extension *extension) {
    sp_cext_lock();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_gc_nroots = roots; sp_cext_unlock();
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    sp_cext_init_locked(extension);
    sp_exc_disarm(); sp_cext_unlock();
    return;
}
#endif

#ifdef SP_CEXT
void *sp_cext_allocate(sp_cext_extension *ext,int slot,int cls) {
    sp_cext_lock();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_gc_nroots = roots; sp_cext_unlock();
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    void * result = sp_cext_allocate_locked(ext,slot,cls);
    sp_exc_disarm(); sp_cext_unlock();
    return result;
}
#endif

#ifdef SP_CEXT
sp_RbVal sp_cext_invoke(sp_cext_extension *ext,int slot,int arity,sp_RbVal self,sp_PolyArray *args,sp_Proc *block) {
    SP_GC_ROOT_RBVAL(self); SP_GC_ROOT(args); SP_GC_ROOT(block);
    sp_cext_lock();
    const int roots = sp_gc_nroots;
    jmp_buf frame;
    if (setjmp(frame) != 0) {
        CextExit left = cext_exit_take();
        sp_exc_disarm(); sp_gc_nroots = roots; sp_cext_unlock();
        cext_exit_resume(left);
    }
    sp_exc_arm(frame);
    sp_RbVal result = sp_cext_invoke_locked(ext,slot,arity,self,args,block);
    sp_exc_disarm(); sp_cext_unlock();
    return result;
}
#endif
