# respond_to? and method_defined? asked before a builtin class's reopening
# defines the name answer false, as the plain call raises there; once the def
# has run they answer true. The receiver is nil, true and false, an Integer, an
# IO, a program object (Object and Kernel reopenings) or a class, and the guard
# and the call it guards agree.
p nil.respond_to?(:nil_late)
p 5.respond_to?(:int_late)
p true.respond_to?(:t_late), false.respond_to?(:t_late)
p $stdout.respond_to?(:io_late)
class Foo; end
p Foo.new.respond_to?(:obj_late), 7.respond_to?(:kern_late), "s".respond_to?(:kern_late)
p String.method_defined?(:str_late), Array.method_defined?(:ary_late)
p(5.respond_to?(:int_late) ? 5.int_late : :guarded)
p(true.respond_to?(:t_late) ? true.t_late : :guarded)
p(nil.respond_to?(:nil_late) ? nil.nil_late : :guarded)
[true, false].each { |v| p v.respond_to?(:f_late) }
p nil.respond_to?(:to_a), 3.respond_to?(:abs)

class NilClass; def nil_late = 1; end
class Integer; def int_late = 2; end
class TrueClass; def t_late = 3; end
class FalseClass; def t_late = 4; end
class FalseClass; def f_late = 5; end
class IO; def io_late = 6; end
class Object; def obj_late = 7; end
module Kernel; def kern_late = 8; end
class String; def str_late = 9; end
class Array; def ary_late = 10; end

p nil.respond_to?(:nil_late)
p 5.respond_to?(:int_late)
p true.respond_to?(:t_late), false.respond_to?(:t_late)
p $stdout.respond_to?(:io_late)
p Foo.new.respond_to?(:obj_late), 7.respond_to?(:kern_late), "s".respond_to?(:kern_late)
p String.method_defined?(:str_late), Array.method_defined?(:ary_late)
[true, false].each { |v| p v.respond_to?(:f_late) }

class NilClass; undef_method :to_a; end
p nil.respond_to?(:to_a), nil.respond_to?(:to_s)
