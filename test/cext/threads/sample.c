#include <ruby.h>
#include <ruby/thread.h>
#include <stdlib.h>
#include <poll.h>
static int active, violation;
static void *pause_native(void *data) {
    (void)data;
    for (volatile unsigned i = 0; i < 200000; i++) {}
    return NULL;
}
static VALUE serialized(VALUE self) {
    (void)self;
    if (__atomic_add_fetch(&active, 1, __ATOMIC_RELAXED) != 1) rb_raise(rb_eRuntimeError, "concurrent extension entry");
    pause_native(NULL);
    __atomic_sub_fetch(&active, 1, __ATOMIC_RELAXED);
    return INT2FIX(1);
}
static VALUE unlocked(VALUE self) {
    (void)self;
    rb_thread_call_without_gvl(pause_native, NULL, NULL, NULL);
    return INT2FIX(2);
}
static VALUE io_unlocked(VALUE self) {
    (void)self;rb_thread_call_without_gvl(pause_native,NULL,RUBY_UBF_IO,NULL);
    return INT2FIX(2);
}
static VALUE callback(VALUE self) { (void)self; return rb_yield(INT2FIX(3)); }
static VALUE string_callback(VALUE self) {
    (void)self;
    VALUE text = rb_str_new_cstr("arena");
    VALUE result = rb_yield(text);
    return rb_funcall(result, rb_intern("+"), 1, rb_funcall(text, rb_intern("length"), 0));
}
static VALUE protected_yield(VALUE unused) { (void)unused; return rb_yield(INT2FIX(1)); }
static VALUE protected_callback(VALUE self) {
    (void)self;
    int state = 0;
    rb_protect(protected_yield, Qnil, &state);
    if (!state) rb_raise(rb_eRuntimeError, "callback did not raise");
    VALUE saved = rb_errinfo();
    rb_yield(INT2FIX(2));
    VALUE result = saved == rb_errinfo() ? Qtrue : Qfalse;
    rb_set_errinfo(Qnil);
    return result;
}
static int waiting, interrupted;
static void *io_wait_native(void *data) {
    (void)data;__atomic_store_n(&waiting,1,__ATOMIC_RELEASE);
    poll(NULL,0,-1);return NULL;
}
static VALUE wait_io(VALUE self) {
    (void)self;rb_thread_call_without_gvl(io_wait_native,NULL,RUBY_UBF_IO,NULL);
    return Qnil;
}
static VALUE wait_select(VALUE self) {
    (void)self;__atomic_store_n(&waiting,1,__ATOMIC_RELEASE);
    rb_thread_fd_select(0,NULL,NULL,NULL,NULL);
    return Qnil;
}
static void unblock(void *data) {
    (void)data;
    __atomic_store_n(&interrupted, 1, __ATOMIC_RELEASE);
}
static void *wait_native(void *data) {
    (void)data;
    __atomic_store_n(&waiting, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&interrupted, __ATOMIC_ACQUIRE)) {}
    return NULL;
}
static VALUE wait_interrupt(VALUE self) {
    (void)self;
    __atomic_store_n(&waiting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&interrupted, 0, __ATOMIC_RELEASE);
    rb_thread_call_without_gvl(wait_native, NULL, unblock, NULL);
    return Qnil;
}
static VALUE waiting_p(VALUE self) {
    (void)self;
    return __atomic_load_n(&waiting, __ATOMIC_ACQUIRE) ? Qtrue : Qfalse;
}
static VALUE reset(VALUE self) {
    (void)self;
    __atomic_store_n(&waiting, 0, __ATOMIC_RELEASE);
    return Qnil;
}
static int marked, freed;
static void data_mark(void *data) {
    (void)data;
    __atomic_add_fetch(&marked, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&active, 1, __ATOMIC_RELAXED) != 1) __atomic_store_n(&violation, 1, __ATOMIC_RELAXED);
    pause_native(NULL);
    __atomic_sub_fetch(&active, 1, __ATOMIC_RELAXED);
}
static void data_free(void *data) {
    __atomic_add_fetch(&freed, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&active, 1, __ATOMIC_RELAXED) != 1) __atomic_store_n(&violation, 1, __ATOMIC_RELAXED);
    pause_native(NULL);
    free(data);
    __atomic_sub_fetch(&active, 1, __ATOMIC_RELAXED);
}
static const rb_data_type_t data_type = {
    "CxlData", {data_mark, data_free, NULL}, NULL, NULL, 0
};
static VALUE data_allocate(VALUE klass) {
    void *data = calloc(1, 1);
    if (!data) rb_raise(rb_eNoMemError, "allocation failed");
    return TypedData_Wrap_Struct(klass, &data_type, data);
}
static VALUE gc_callbacks(VALUE self) { (void)self; return __atomic_load_n(&marked, __ATOMIC_RELAXED) && __atomic_load_n(&freed, __ATOMIC_RELAXED) && !__atomic_load_n(&violation, __ATOMIC_RELAXED) ? Qtrue : Qfalse; }
static VALUE data_touch(VALUE self) { (void)self; return serialized(self); }
void Init_cxl_sample(void) {
    VALUE klass = rb_define_class("CxlData", rb_cObject);
    rb_define_alloc_func(klass, data_allocate);
    rb_define_method(klass, "touch", data_touch, 0);
    VALUE module = rb_define_module("CxlSample");
    rb_define_singleton_method(module, "gc_callbacks", gc_callbacks, 0);
    rb_define_singleton_method(module, "protected_callback", protected_callback, 0);
    rb_define_singleton_method(module, "wait_interrupt", wait_interrupt, 0);
    rb_define_singleton_method(module, "wait_select", wait_select, 0);
    rb_define_singleton_method(module, "wait_io", wait_io, 0);
    rb_define_singleton_method(module, "waiting?", waiting_p, 0);
    rb_define_singleton_method(module, "reset", reset, 0);
    rb_define_singleton_method(module, "serialized", serialized, 0);
    rb_define_singleton_method(module, "unlocked", unlocked, 0);
    rb_define_singleton_method(module, "io_unlocked", io_unlocked, 0);
    rb_define_singleton_method(module, "callback", callback, 0);
    rb_define_singleton_method(module, "string_callback", string_callback, 0);
}
