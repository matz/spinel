#include <ruby.h>

static VALUE name_of(VALUE self, VALUE symbol) {
    (void)self;
    return rb_sym2str(symbol);
}
void Init_symbol_names(void) {
    VALUE mod = rb_define_module("CextSymbolNames");
    rb_define_module_function(mod, "name_of", name_of, 1);
}
