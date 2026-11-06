#include <ruby.h>

static const rb_data_type_t made_type = { "CMade", { NULL, NULL, NULL }, NULL, NULL, 0 };

/* calls the program's `value` on the object it is handed: a method no
   Ruby code of the program calls */
static VALUE ask(VALUE self, VALUE obj) {
    (void)self;
    return rb_funcall(obj, rb_intern("value"), 0);
}
/* an allocator, which runs on `new`, calls the class's `made` */
static VALUE made_alloc(VALUE klass) {
    rb_funcall(klass, rb_intern("made"), 0);
    return rb_data_typed_object_wrap(klass, NULL, &made_type);
}
void Init_callback_versions(void) {
    VALUE mod = rb_define_module("CextAsk");
    rb_define_module_function(mod, "ask", ask, 1);
    rb_define_alloc_func(rb_define_class("CMade", rb_cObject), made_alloc);
}
