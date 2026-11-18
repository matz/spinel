#include <ruby.h>

static VALUE read_text(VALUE self, VALUE obj, VALUE text, VALUE mode) {
    (void)self;
    return rb_funcall(obj, rb_intern("answer"), 2, text, mode);
}

void Init_string_returns(void) {
    VALUE mod = rb_define_module("ReadText");
    rb_define_module_function(mod, "read", read_text, 3);
}
