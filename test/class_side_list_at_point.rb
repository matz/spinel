# A class-side method list (singleton_methods, with and without `false`)
# answers at its own point: a class method defined, aliased, extended in or
# brought by define_singleton_method after the list is not in it yet, as a
# call of it there raises. The `false` form lists the class's own methods
# alone, whatever a superclass or an extended module holds. Lines marked
# "row" are a builtin class's own class methods, there from the start.

# a def after the list, both forms; a subclass and its parent's later def
class A1; end
p A1.singleton_methods(false)
class A1; def self.v = 1; end
p A1.singleton_methods(false)
class A2; def self.a = 1; end
p A2.singleton_methods.sort
class A2; def self.b = 2; end
p A2.singleton_methods.sort
class P3; def self.pa = 1; end
class C3 < P3; end
p C3.singleton_methods.sort, C3.singleton_methods(false)
class P3; def self.pb = 2; end
class C3; def self.ca = 3; end
p C3.singleton_methods.sort, C3.singleton_methods(false)

# extend, class << C, define_singleton_method, an extended parent
module M4; def mm = 1; end
class C4; def self.a = 1; end
p C4.singleton_methods.sort
C4.extend(M4)
p C4.singleton_methods.sort, C4.singleton_methods(false)
class C5; def self.a = 1; end
p C5.singleton_methods(false).sort
class << C5; def v = 2; end
C5.define_singleton_method(:w) { 3 }
p C5.singleton_methods(false).sort
class P6; end
class C6 < P6; end
p C6.singleton_methods
P6.extend(M4)
p C6.singleton_methods

# the own form with an extended module or a parent holding the name
module M7; def v = 1; end
class C7; extend M7; end
p C7.singleton_methods(false), C7.singleton_methods
def C7.v = 2
p C7.singleton_methods(false), C7.singleton_methods
class P8; def self.v = 1; end
class C8 < P8; end
p C8.singleton_methods(false)
class C8; def self.v = 2; end
p C8.singleton_methods(false), C8.v

# a module's class side: module_function and def self.x after the list
module M9; def f = 1; def self.a = 1; end
p M9.singleton_methods.sort
module M9; module_function :f; def self.x = 2; end
p M9.singleton_methods.sort

# in a block run at once; a list guarding its call; a later private one
class C10; end
[1].each { p C10.singleton_methods(false) }
p(C10.singleton_methods(false).include?(:v) ? C10.v : :none)
class C10; def self.v = 1; def self.h = 2; private_class_method :h; end
[1].each { p C10.singleton_methods(false).sort }
p(C10.singleton_methods(false).include?(:v) ? C10.v : :none)

# a def in a required file
class K1; end
p K1.singleton_methods(false)
require_relative "class_side_list_at_point_lib/late"
p K1.singleton_methods(false)

# stays: a class << self accessor, a block kept and run later, a row
class C11; class << self; attr_accessor :v; end; end
p C11.singleton_methods(false).sort
$blk = nil
def keep_list(&b) = ($blk = b)
keep_list { p C11.singleton_methods(false).sort }
class C11; def self.w = 1; end
$blk.call
p String.singleton_methods.include?(:try_convert)   # row
class String; def self.try_convert(x) = x; end
p String.singleton_methods.include?(:try_convert)
