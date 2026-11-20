#include <ruby.h>

static VALUE append(VALUE self, VALUE value) {
    (void)self;
    return rb_str_cat_cstr(value, "!");
}

void Init_yield_next_result(void) {
    VALUE mod = rb_define_module("YieldNextNative");
    rb_define_module_function(mod, "append", append, 1);
}
